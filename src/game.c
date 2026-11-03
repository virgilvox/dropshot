/* game.c: the state machine, Juno, the camera and scope, and the glue
 * between the modules. world.c owns the level, enemy.c the Tenants, fx.c
 * the particles, hud.c everything 2D, audio.c the sound. */
#include "game.h"
#include "input.h"
#include "audio.h"
#include "world.h"
#include "enemy.h"
#include "fx.h"
#include "hud.h"
#include "cutscene.h"
#include "voice.h"
#include <stdlib.h>
#include <string.h>

typedef enum { ST_TITLE, ST_DROP, ST_PLAY, ST_PAUSE, ST_DEAD, ST_CLEAR, ST_END, ST_CUT } state_t;
/* what follows a cutscene */
typedef enum { AFTER_DROP, AFTER_PLAY, AFTER_BOSS, AFTER_END } after_t;

static t3_scene *scene;
static t3_camera *cam;
static t3_light *sun;
static state_t st;
static float st_t;
static int level;
static hud_t H;
static after_t cut_after;
static bool cut_seen[CUT_COUNT];   /* this session: a restart does not replay them */
static bool boss_cut_done, boss_dead_t_on;
static float boss_dead_t;
static cine_view_t CV;
static int radio_prep = 8;   /* the next radio beat to synthesise ahead of time */

/* ── a skinned, animated GLB ── */
typedef struct {
  t3_gltf *gltf;
  t3_object *root, *model;
  t3_animation_mixer *mixer;
  t3_animation_action *cur, *prev;
  float fade;
} actor_t;

static void mesh_setup(t3_object *o, void *ctx) {
  (void)ctx;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  o->frustum_culled = false; o->cast_shadow = true; o->receive_shadow = true;
  t3_mesh *m = (t3_mesh *)o;
  for (int i = 0; i < m->material_count; i++) {
    t3_material *M = m->materials[i];
    if (!M) continue;
    M->metalness = 0.15f; M->roughness = 0.6f;
    M->version++;
  }
}

static bool actor_load(actor_t *a, const char *asset, float scale) {
  size_t n; uint8_t *b = ds_asset(asset, &n);
  if (!b) return false;
  a->gltf = t3_gltf_parse(b, n, NULL, NULL);
  free(b);
  if (!a->gltf) { ds_log("bad glb %s", asset); return false; }
  a->root = t3_object_new();
  a->model = a->gltf->scene;
  t3_object_add(a->root, a->model);
  t3_object_set_scale(a->model, scale, scale, scale);
  t3_object_traverse(a->model, mesh_setup, NULL);
  a->mixer = t3_animation_mixer_new(a->model);
  return true;
}

/* clip names come as "CharacterArmature|Run": match the part after the bar */
static t3_animation_clip *actor_clip(actor_t *a, const char *name) {
  for (int i = 0; i < a->gltf->animation_count; i++) {
    const char *n = a->gltf->animations[i]->name, *bar = strrchr(n, '|');
    if (!strcmp(bar ? bar + 1 : n, name)) return a->gltf->animations[i];
  }
  return NULL;
}

static void actor_play(actor_t *a, const char *name, float speed) {
  t3_animation_clip *c = actor_clip(a, name);
  if (!c) return;
  t3_animation_action *act = t3_animation_mixer_clip_action(a->mixer, c);
  t3_animation_action_set_effective_time_scale(act, speed);
  if (act == a->cur) return;
  if (a->prev && a->prev != act) t3_animation_action_stop(a->prev);
  a->prev = a->cur;
  a->cur = act;
  t3_animation_action_play(act);
  t3_animation_action_set_effective_weight(act, 0);
  a->fade = 0;
}

static void actor_update(actor_t *a, float dt) {
  a->fade = fminf(1, a->fade + dt * 8);
  if (a->cur) t3_animation_action_set_effective_weight(a->cur, a->fade);
  if (a->prev) {
    t3_animation_action_set_effective_weight(a->prev, 1 - a->fade);
    if (a->fade >= 1) { t3_animation_action_stop(a->prev); a->prev = NULL; }
  }
  t3_animation_mixer_update(a->mixer, dt);
}

/* ── Juno ── */
#define JUNO_SCALE 0.65f        /* the model is 2.8 m tall as authored */
#define JUNO_R 0.4f
#define JUNO_H 1.8f
#define HP_MAX 100.f
#define SPIN_UP 0.3f
#define CHAIN_RATE 14.f
#define CHAIN_DMG 8.f
#define SNIPE_DMG 120.f
#define ROLL_T 0.45f

static actor_t juno;
static t3_object *head_bone, *torso_bone, *wing, *scope_prop, *decoy_obj;
static v3 pos, vel;
static float yaw;
static bool grounded, gliding;
static float hp, shield, spin, fire_cd, roll_t, roll_cd, combat_t, hurt_iframes;
static v3 roll_dir;
static int snipe_ammo, decoys, weapon, kills, score, rings;
static float level_time, decoy_t;
static v3 decoy_pos;
static v3 checkpoint;
static float wing_open;   /* 0..1 animation */

/* ── the camera ── */
static float cam_yaw, cam_pitch = -0.2f, cam_dist = 4.2f;
static v3 cam_pos, cam_fwd;
static bool scoped;
static int zoom = 1;
static float fov = 62, shake;
static const float ZOOM_FOV[5] = {62, 28, 14, 7, 3.5f};

static void banner(const char *s) { H.banner = s; H.banner_t = 2.2f; }
/* the alerts worth hearing mid-fight; never over a radio call or dialogue */
static const char *const SPOKEN[] = {"CHECKPOINT", "SHIELD", "OUT OF SCOPE ROUNDS", "PLEASE KEEP TO THE WALKWAYS"};
static void alert(const char *s) {
  banner(s);
  if (!voice_busy()) voice_say(VO_ALERT, s);
}

static t3_mesh *box_mesh(float w, float h, float d, uint32_t color, float emissive) {
  t3_material *m = t3_mesh_standard_material_new(color);
  if (emissive > 0) { m->emissive = t3_color_hex(color); m->emissive_intensity = emissive; }
  t3_mesh *b = t3_mesh_new(t3_box_geometry_new(w, h, d, 1, 1, 1), m);
  ((t3_object *)b)->cast_shadow = true;
  return b;
}

/* Juno's rig: a helmet scope and a squeegee wing on her back. Both hang off
 * the root in metres (the bones sit under the FBX export's 100x armature and
 * unit rotations); the scope follows the head bone's world position. */
static void build_rig(void) {
  head_bone = t3_object_get_by_name(juno.model, "Head");
  torso_bone = t3_object_get_by_name(juno.model, "Torso");
  if (head_bone) {
    scope_prop = t3_object_new();
    t3_mesh *tube = t3_mesh_new(t3_cylinder_geometry_new(0.045f, 0.055f, 0.42f, 10, 1, false), t3_mesh_standard_material_new(0x2b303a));
    t3_object_set_rotation(tube, DS_PI / 2, 0, 0);
    t3_object_add(scope_prop, tube);
    t3_mesh *lens = t3_mesh_new(t3_cylinder_geometry_new(0.05f, 0.05f, 0.02f, 10, 1, false), t3_mesh_standard_material_new(C_TEAL));
    lens->materials[0]->emissive = t3_color_hex(C_TEAL); lens->materials[0]->emissive_intensity = 1.6f;
    t3_object_set_rotation(lens, DS_PI / 2, 0, 0);
    t3_object_set_position(lens, 0, 0, 0.22f);
    t3_object_add(scope_prop, lens);
    t3_mesh *strap = box_mesh(0.03f, 0.12f, 0.06f, C_HIVIS, 0);
    t3_object_set_position(strap, -0.05f, -0.07f, 0);
    t3_object_add(scope_prop, strap);
    t3_object_add(juno.root, scope_prop);
  }
  if (torso_bone) {
    wing = t3_object_new();
    for (int s = -1; s <= 1; s += 2) {
      t3_object *half = t3_object_new();
      t3_mesh *blade = box_mesh(1.1f, 0.04f, 0.32f, C_CREAM, 0);
      t3_object_set_position(blade, s * 0.6f, 0, 0);
      t3_object_add(half, blade);
      t3_mesh *edge = box_mesh(1.1f, 0.05f, 0.06f, C_TEAL, 0.8f);   /* the rubber squeegee edge */
      t3_object_set_position(edge, s * 0.6f, 0, -0.17f);
      t3_object_add(half, edge);
      half->name = s < 0 ? "wing_l" : "wing_r";
      t3_object_add(wing, half);
    }
    t3_mesh *spine = box_mesh(0.18f, 0.5f, 0.12f, 0x2b303a, 0);
    t3_object_add(wing, spine);
    t3_object_add(juno.root, wing);
  }
  /* the decoy: a hi-vis cardboard stand-up of Juno, more or less */
  decoy_obj = t3_object_new();
  t3_mesh *board = box_mesh(0.7f, 1.7f, 0.06f, C_HIVIS, 0.4f);
  t3_object_set_position(board, 0, 0.85f, 0);
  t3_object_add(decoy_obj, board);
  t3_mesh *foot = box_mesh(0.5f, 0.06f, 0.4f, 0x2b303a, 0);
  t3_object_set_position(foot, 0, 0.03f, 0);
  t3_object_add(decoy_obj, foot);
  decoy_obj->visible = false;
  t3_object_add(&scene->base, decoy_obj);
}

/* the wing's halves sweep from folded (along the spine) to open (level) */
static void pose_wing(float open, float t) {
  if (!wing) return;
  for (int i = 0; i < 2; i++) {
    t3_object *half = t3_object_get_by_name(wing, i ? "wing_r" : "wing_l");
    if (!half) continue;
    float s = i ? 1.f : -1.f, flutter = open > 0.9f ? sinf(t * 20 + i) * 0.03f : 0;
    t3_object_set_rotation(half, 0, 0, s * ds_lerp(1.35f, 0.05f + flutter, open));
  }
  float sc = ds_lerp(0.35f, 1, open);
  t3_object_set_scale(wing, sc, sc, sc);
  t3_object_set_position(wing, 0, 1.25f, -0.32f);
  /* the scope rides beside the head, in the root's (yaw-only) frame */
  if (scope_prop && head_bone) {
    t3_object_update_matrix_world(juno.root, true);
    v3 h; { t3_vec3 w = t3_object_get_world_position(head_bone); h = v3_(w.x - pos.x, w.y - pos.y, w.z - pos.z); }
    float c = cosf(-yaw), s = sinf(-yaw);
    t3_object_set_position(scope_prop, h.x * c + h.z * s + 0.16f, h.y + 0.42f, -h.x * s + h.z * c + 0.1f);
  }
}

/* ── states ── */
static void save_write(void) {
  /* a DSH1 save keeps its progress but loses invert Y: the old title menu wrapped
   * from the top row onto that setting, so a stray W then Enter turned it on */
  if (DS_SAVE.magic == DS_SAVE_MAGIC_V1) { DS_SAVE.magic = DS_SAVE_MAGIC; DS_SAVE.invert_y = 0; }
  if (DS_SAVE.magic != DS_SAVE_MAGIC) { memset(&DS_SAVE, 0, sizeof DS_SAVE); DS_SAVE.magic = DS_SAVE_MAGIC; DS_SAVE.music = 1; DS_SAVE.shake = 1; }
}

static void reset_player(v3 at) {
  pos = at; vel = v3_(0, 0, 0);
  hp = HP_MAX; spin = 0; roll_t = roll_cd = 0; hurt_iframes = 1.5f;
  grounded = false; gliding = false;
  if (juno.root) juno.root->visible = true;
}

/* run a cutscene once per session, then carry on as `after` says */
static void play_cut(cut_id id, after_t after) {
  if (cut_seen[id]) {
    if (after == AFTER_BOSS) enemy_boss_wake();
    if (after == AFTER_END) { st = ST_END; st_t = 0; music_mood(MOOD_TITLE); }
    return;
  }
  cut_seen[id] = true;
  cutscene_hint_player(pos, yaw);   /* the Brute and boss scenes stage around where she stands */
  cutscene_start(id, level);
  if (!cutscene_active()) { cut_seen[id] = false; if (after == AFTER_BOSS) enemy_boss_wake(); if (after == AFTER_END) { st = ST_END; st_t = 0; } return; }
  cut_after = after;
  st = ST_CUT; st_t = 0; scoped = false;
  memset(&CV, 0, sizeof CV);
}

static void cut_finish(void) {
  st_t = 0;
  switch (cut_after) {
    case AFTER_DROP: st = ST_DROP; music_mood(MOOD_DROP); break;
    case AFTER_PLAY: st = ST_PLAY; break;
    case AFTER_BOSS: st = ST_PLAY; enemy_boss_wake(); break;
    case AFTER_END: st = ST_END; music_mood(MOOD_TITLE); break;
  }
  if (juno.root) juno.root->visible = true;
}

static void start_level(int l) {
  level = l;
  fx_clear();
  enemy_clear();
  world_load(level);
  enemy_set_level(level);
  radio_prep = 0;   /* this level's radio lines are synthesised one a frame (game_frame), not here */
  /* the levels were laid out dense; thin the rank and file so fights are
   * pockets with breathing room between them. Brutes and the boss all stay. */
  const spawn_t *sp; int n = world_spawns(&sp);
  int seen[EN_TYPE_COUNT] = {0}, kept[EN_TYPE_COUNT] = {0};
  for (int i = 0; i < n; i++) {
    int t = sp[i].type, k = seen[t]++;
    bool keep = t == EN_TURRET ? (k % 2 == 0) : (t == EN_GRUNT || t == EN_FLYER) ? (k % 3 != 2) : true;
    if (keep) { enemy_spawn(t, sp[i].pos, sp[i].yaw); kept[t]++; }
  }
#ifdef DS_TRACE
  ds_log("spawns L%d: grunt %d/%d flyer %d/%d turret %d/%d brute %d boss %d", level, kept[EN_GRUNT], seen[EN_GRUNT],
         kept[EN_FLYER], seen[EN_FLYER], kept[EN_TURRET], seen[EN_TURRET], kept[EN_BRUTE], kept[EN_BOSS]);
#endif
  checkpoint = world_spawn();
  reset_player(world_drop_start());
  shield = 0; snipe_ammo = level == WORLD_LEVELS - 1 ? 20 : 12; decoys = 2; weapon = 0; kills = 0; score = 0; rings = 0; level_time = 0;   /* the last level is a scope fight: more rounds */
  decoy_t = 0; decoy_obj->visible = false;
  wing_open = 0; scoped = false; zoom = 1;
  cam_yaw = DS_PI; cam_pitch = -0.2f;   /* behind Juno: the levels run toward +z */
  enemy_take_score();
  st = ST_DROP; st_t = 0;
  H.mode = HUD_INTRO;
  H.radio_t = 0;
  boss_cut_done = false; boss_dead_t_on = false; boss_dead_t = 0;
  world_set_gates(false);
  music_mood(MOOD_DROP);
  /* each level opens on its own scene; the last is always the Freehold's */
  static const cut_id before[] = {CUT_PROLOGUE, CUT_SUBLET, CUT_COLD, CUT_ANNEX, CUT_BASEMENT};
  play_cut(level == WORLD_LEVELS - 1 ? CUT_FREEHOLD : before[level % 5], AFTER_DROP);
}

static void to_title(void) {
  save_write();   /* in case the host filled the save region after init */
  st = ST_TITLE; st_t = 0;
  level = DS_SAVE.magic == DS_SAVE_MAGIC ? DS_SAVE.level_reached : 0;
  if (level >= WORLD_LEVELS) level = WORLD_LEVELS - 1;
  fx_clear();
  enemy_clear();
  world_load(level);
  reset_player(world_spawn());
  H.menu_sel = level;
  music_mood(MOOD_TITLE);
}

void game_init(void) {
  save_write();
  scene = t3_scene_new();
  scene->background = t3_color_hex(C_SKY); scene->has_background = true;
  scene->fog = t3_fog_linear(C_SKY, 60, 420);
  cam = t3_perspective_camera_new(62, (float)DS_W / DS_H, 0.25f, 4000);
  t3_object_add(&scene->base, t3_hemisphere_light_new(0xdfefff, 0x3a3428, 1.1f));
  sun = t3_directional_light_new(0xfff1d6, 2.4f);
  t3_object_set_position(sun, 20, 40, 10);
  ((t3_object *)sun)->cast_shadow = true;
  t3_object_add(&scene->base, sun);
  if (actor_load(&juno, "juno.glb", JUNO_SCALE)) {
    t3_object_add(&scene->base, juno.root);
    actor_play(&juno, "Idle_Gun", 1);
  }
  build_rig();
  world_init(scene);
  enemy_init(scene);
  cutscene_init(scene);
  for (unsigned i = 0; i < sizeof SPOKEN / sizeof SPOKEN[0]; i++) voice_prepare(VO_ALERT, SPOKEN[i]);
  fx_init(scene);
  hud_init();
  audio_set_music(DS_SAVE.music);
  to_title();
}

/* the aim ray: from the camera through the screen centre */
static void aim_ray(v3 *o, v3 *d) { *o = cam_pos; *d = cam_fwd; }

static void hurt(float dmg, v3 from) {
  if (dmg <= 0 || roll_t > 0 || hurt_iframes > 0 || st != ST_PLAY) return;
  if (shield > 0) { float a = fminf(shield * 50, dmg); shield -= a / 50; dmg -= a; }
  hp -= dmg;
  hurt_iframes = 0.25f;   /* overlapping shots from a crowd land as one hit, not a shredder */
  combat_t = 4;
  H.hurt = 0.5f;
  v3 to = v3_sub(from, pos);
  H.hurt_angle = ds_wrap_pi(atan2f(to.x, to.z) - atan2f(cam_fwd.x, cam_fwd.z));
  shake = fmaxf(shake, 0.25f);
  sfx(SFX_HURT, 0, 1);
  if (hp <= 0) {
    hp = 0; st = ST_DEAD; st_t = 0; scoped = false;
    actor_play(&juno, "Death", 1);
    music_mood(MOOD_SILENT);
  }
}

static void shoot(float dt, const intent_t *I) {
  fire_cd -= dt;
  v3 o, d; aim_ray(&o, &d);
  if (scoped) {
    if (I->fire && fire_cd <= 0) {
      if (snipe_ammo <= 0) { alert("OUT OF SCOPE ROUNDS"); sfx(SFX_UI, 0, 0.6f); fire_cd = 0.3f; return; }
      snipe_ammo--; fire_cd = 0.9f; combat_t = 4;
      sfx(SFX_SNIPE, 0, 1);
      shake = fmaxf(shake, 0.12f);
      float wt = 2000, et; int id; bool weak;
      v3 n = v3_(0, 1, 0);
      bool hw = world_raycast(o, d, 2000, &wt, &n);
      if (enemy_raycast(o, d, hw ? wt : 2000, &id, &et, &weak)) {
        v3 at = v3_add(o, v3_scale(d, et));
        if (enemy_damage(id, weak ? SNIPE_DMG * 2 : SNIPE_DMG, at, weak)) kills++;
        H.hit_marker = 0.35f; H.hit_weak = weak;
        sfx(weak ? SFX_BOSS_HIT : SFX_HIT, 0, 1);
      } else if (hw) {
        fx_sparks(v3_add(o, v3_scale(d, wt)), n, C_CREAM, 14);
      }
    }
    return;
  }
  if (I->fire_held) spin = fminf(1, spin + dt / SPIN_UP); else spin = fmaxf(0, spin - dt * 2);
  if (!I->fire_held || spin < 0.35f || fire_cd > 0) return;
  fire_cd = 1.f / (CHAIN_RATE * (0.5f + spin * 0.5f));
  combat_t = 4;
  float spread = 0.012f + (1 - spin) * 0.03f;
  v3 r = v3_norm(v3_cross(d, v3_(0, 1, 0))), u = v3_cross(r, d);
  d = v3_norm(v3_add(d, v3_add(v3_scale(r, ds_rr(-spread, spread)), v3_scale(u, ds_rr(-spread, spread)))));
  float wt = 300, et; int id; bool weak;
  v3 n = v3_(0, 1, 0);
  bool hw = world_raycast(o, d, 300, &wt, &n);
  /* the muzzle is at Juno's right hand; the tracer runs from there to the hit */
  v3 right = v3_(cosf(yaw), 0, -sinf(yaw)), fwd = v3_(sinf(yaw), 0, cosf(yaw));
  v3 muzzle = v3_add(pos, v3_add(v3_(0, 1.05f, 0), v3_add(v3_scale(right, -0.35f), v3_scale(fwd, 0.6f))));
  v3 end = v3_add(o, v3_scale(d, wt));
  if (enemy_raycast(o, d, hw ? wt : 300, &id, &et, &weak)) {
    end = v3_add(o, v3_scale(d, et));
    if (enemy_damage(id, CHAIN_DMG, end, weak)) kills++;
    H.hit_marker = 0.15f; H.hit_weak = weak;
  } else if (hw) {
    fx_sparks(end, n, 0xffd27a, 4);
  }
  fx_muzzle(muzzle, d);
  fx_tracer(muzzle, end, 0xffd27a, 0.03f);
  sfx(SFX_SHOT, 0.2f, 0.7f);
}

static void update_camera(float dt, const intent_t *I) {
  float inv = DS_SAVE.invert_y ? -1.f : 1.f;
  float sens = scoped ? 1.4f / (float)(1 << (zoom - 1)) : 2.7f;
  cam_yaw -= I->ax * sens * dt;
  cam_pitch = ds_clamp(cam_pitch + I->ay * inv * sens * 0.7f * dt, scoped ? -1.3f : -1.1f, scoped ? 1.2f : 0.55f);
  cam_fwd = v3_(-sinf(cam_yaw) * cosf(cam_pitch), sinf(cam_pitch), -cosf(cam_yaw) * cosf(cam_pitch));
  v3 right = v3_norm(v3_cross(cam_fwd, v3_(0, 1, 0)));
  v3 want;
  if (scoped) {
    want = v3_add(pos, v3_(0, 1.65f, 0));
    fov = ds_damp(fov, ZOOM_FOV[zoom], 18, dt);
  } else {
    /* over the right shoulder, pulled in when a wall is behind */
    v3 focus = v3_add(v3_add(pos, v3_(0, 1.55f, 0)), v3_scale(right, 0.75f));
    float dist = cam_dist + (gliding ? 1.5f : 0) + (st == ST_DROP ? 4 : 0);
    v3 back = v3_scale(cam_fwd, -1);
    float t; v3 n;
    if (world_raycast(focus, back, dist, &t, &n)) dist = fmaxf(0.6f, t - 0.3f);
    want = v3_add(focus, v3_scale(back, dist));
    fov = ds_damp(fov, ZOOM_FOV[0] + (roll_t > 0 ? 6 : 0) + (st == ST_DROP ? 14 : 0), 10, dt);
  }
  cam_pos = scoped ? want : v3_lerp(cam_pos, want, 1 - expf(-16 * dt));
  v3 p = cam_pos;
  if (shake > 0 && DS_SAVE.shake) {
    p = v3_add(p, v3_(ds_rr(-1, 1) * shake * 0.3f, ds_rr(-1, 1) * shake * 0.3f, ds_rr(-1, 1) * shake * 0.3f));
  }
  shake = fmaxf(0, shake - dt * 1.5f);
  cam->fov = fov;
  t3_camera_update_projection_matrix(cam);
  t3_object_set_position(cam, p.x, p.y, p.z);
  t3_object_look_at(cam, p.x + cam_fwd.x, p.y + cam_fwd.y, p.z + cam_fwd.z);
}

static void update_drop(float dt, const intent_t *I) {
  /* free fall: steer with the stick, the camera looks down the path */
  float ground = world_floor_y(pos);
  float alt = pos.y - (ground > -1e8f ? ground : world_spawn().y);
  v3 fwd = v3_(-sinf(cam_yaw), 0, -cosf(cam_yaw)), right = v3_(cosf(cam_yaw), 0, -sinf(cam_yaw));
  v3 wish = v3_add(v3_scale(fwd, I->my), v3_scale(right, I->mx));
  bool open = wing_open > 0.5f;
  /* look down the fall at the ship and the rings; level out under the wing */
  cam_pitch = ds_damp(cam_pitch, open ? -0.4f : -1.0f, 2.5f, dt);
  float steer = open ? 9 : 22;
  /* a guide drift that would put her on the landing deck at touchdown; the
   * stick steers on top of it (through the rings, or anywhere she likes) */
  v3 sp = world_spawn();
  float tleft = fmaxf(0.6f, (pos.y - sp.y) / fmaxf(5, -vel.y));
  v3 guide = v3_((sp.x - pos.x) / tleft, 0, (sp.z - pos.z) / tleft);
  float gl = v3_len(guide);
  if (gl > 30) guide = v3_scale(guide, 30 / gl);
  vel.x = ds_damp(vel.x, guide.x + wish.x * steer, 3, dt);
  vel.z = ds_damp(vel.z, guide.z + wish.z * steer, 3, dt);
  if (!open && alt < 70 && I->jump_held) { wing_open = 0.01f; sfx(SFX_GLIDE, 0, 1); }
  if (!open && wing_open == 0 && alt < 18) { wing_open = 0.01f; sfx(SFX_GLIDE, 0, 1); banner("AUTO WING. TUT."); }
  if (wing_open > 0) wing_open = fminf(1, wing_open + dt * 3);
  float target = open ? -5.5f : -48;
  vel.y = ds_damp(vel.y, target, open ? 3 : 1.2f, dt);
  world_move(&pos, &vel, JUNO_R, JUNO_H, dt, &grounded);
  if (wish.x * wish.x + wish.z * wish.z > 0.01f) yaw += ds_wrap_pi(atan2f(wish.x, wish.z) - yaw) * fminf(1, dt * 6);
  /* rings */
  const v3 *rp; float rr; int nr = world_rings(&rp, &rr);
  for (int i = 0; i < nr; i++)
    if (v3_len(v3_sub(rp[i], v3_add(pos, v3_(0, 1, 0)))) < rr && world_ring_collect(i)) {
      rings++; score += 50; fx_ring_pop(rp[i], rr); sfx(SFX_PICKUP, 0, 1);
    }
  H.altitude_m = fmaxf(0, alt);
  H.rings = rings; H.rings_total = nr; H.wing_open = open;
  if (open) {
    v3 r = v3_(cosf(yaw), 0, -sinf(yaw));
    v3 c = v3_add(pos, v3_(0, 1.3f, 0));
    fx_glide_trail(v3_add(c, v3_scale(r, -1.3f)), v3_add(c, v3_scale(r, 1.3f)));
  }
  actor_play(&juno, open ? "Jump_Idle" : "Duck", 1);
  pose_wing(wing_open, (float)ds_now());
  if (grounded) {
    st = ST_PLAY; st_t = 0; wing_open = 0;
    cam_pitch = -0.18f;   /* level the drop's downward look */
    sfx(SFX_LAND, 0, 1);
    shake = 0.3f;
    music_mood(MOOD_EXPLORE);
    banner(world_level_name(level));
  }
  if (pos.y < world_kill_y()) { reset_player(world_spawn()); st = ST_PLAY; }
}

static void update_play(float dt, const intent_t *I) {
  level_time += dt;
#ifdef DS_TRACE
  if (input_key_down(WC_KEY_9)) { pos = level == WORLD_LEVELS - 1 ? v3_(0, 36.5f, 450) : v3_(0, 0.5f, 250); vel = v3_(0, 0, 0); }   /* test only: toward the arena */
  if (input_key_down(WC_KEY_8)) hp = HP_MAX;
#endif
  combat_t -= dt; hurt_iframes -= dt;
  /* scope */
  bool want_scope = I->scope && grounded && roll_t <= 0;
  if (want_scope != scoped) { scoped = want_scope; sfx(SFX_ZOOM, 0, 0.7f); if (scoped) { zoom = 2; spin = 0; } }
  if (scoped && I->zoom) { int z = zoom + I->zoom; if (z >= 1 && z <= 4) { zoom = z; sfx(SFX_ZOOM, 0, 0.5f); } }
  /* movement relative to the camera */
  v3 fwd = v3_(-sinf(cam_yaw), 0, -cosf(cam_yaw)), right = v3_(cosf(cam_yaw), 0, -sinf(cam_yaw));
  v3 wish = v3_add(v3_scale(fwd, I->my), v3_scale(right, I->mx));
  float wl = v3_len(wish);
  if (wl > 1) { wish = v3_scale(wish, 1 / wl); wl = 1; }
  float speed = scoped ? 0 : (I->fire_held ? 5.5f : 7.5f);
  if (I->roll && roll_cd <= 0 && grounded && !scoped) {
    roll_t = ROLL_T; roll_cd = 0.8f;
    roll_dir = wl > 0.1f ? v3_norm(wish) : v3_(sinf(yaw), 0, cosf(yaw));
    sfx(SFX_ROLL, 0, 1);
  }
  roll_cd -= dt;
  if (roll_t > 0) {
    roll_t -= dt;
    vel.x = roll_dir.x * 13; vel.z = roll_dir.z * 13;
    yaw = atan2f(roll_dir.x, roll_dir.z);
  } else {
    vel.x = ds_damp(vel.x, wish.x * speed, grounded ? 14 : 3, dt);
    vel.z = ds_damp(vel.z, wish.z * speed, grounded ? 14 : 3, dt);
    /* aiming: Juno faces where the camera looks; otherwise where she runs */
    float face = (I->fire_held || scoped || spin > 0) ? atan2f(cam_fwd.x, cam_fwd.z) : (wl > 0.1f ? atan2f(wish.x, wish.z) : yaw);
    yaw += ds_wrap_pi(face - yaw) * fminf(1, dt * 14);
  }
  if (grounded && I->jump && !scoped) { vel.y = 9.8f; grounded = false; sfx(SFX_JUMP, 0, 1); }
  bool was_gliding = gliding;
  gliding = !grounded && vel.y < 0 && I->jump_held && roll_t <= 0;
  if (gliding && !was_gliding) sfx(SFX_GLIDE, 0, 0.8f);
  wing_open = ds_damp(wing_open, gliding ? 1 : 0, 10, dt);
  float g = gliding ? 7 : 24;
  vel.y -= g * dt;
  if (gliding) {
    if (vel.y < -2.8f) vel.y = -2.8f;
    /* the wing carries her forward */
    v3 f = v3_(sinf(yaw), 0, cosf(yaw));
    vel.x = ds_damp(vel.x, f.x * 9 + wish.x * 3, 2, dt);
    vel.z = ds_damp(vel.z, f.z * 9 + wish.z * 3, 2, dt);
    v3 r = v3_(cosf(yaw), 0, -sinf(yaw)), c = v3_add(pos, v3_(0, 1.3f, 0));
    fx_glide_trail(v3_add(c, v3_scale(r, -1.3f)), v3_add(c, v3_scale(r, 1.3f)));
  }
  float vy0 = vel.y;
  bool was_grounded = grounded;
  world_move(&pos, &vel, JUNO_R, JUNO_H, dt, &grounded);
  if (grounded && !was_grounded && vy0 < -6) { sfx(SFX_LAND, 0, fminf(1, -vy0 / 20)); shake = fmaxf(shake, fminf(0.25f, -vy0 / 80)); }
  if (pos.y < world_kill_y()) {
    alert("PLEASE KEEP TO THE WALKWAYS");
    hurt_iframes = 0; hurt(25, pos);
    if (st == ST_PLAY) reset_player(checkpoint);
    hp = fmaxf(hp, 1);
  }
  /* weapons */
  if (I->swap && !scoped) { weapon ^= 1; sfx(SFX_UI, 0, 0.6f); }
  if (weapon == 1 && !scoped && I->fire) banner("HOLD SCOPE TO USE SCOPE ROUNDS");
  shoot(dt, I);
  if (I->decoy && decoys > 0 && grounded) {
    decoys--; decoy_t = 9;
    decoy_pos = v3_add(pos, v3_scale(v3_(sinf(yaw), 0, cosf(yaw)), 2));
    decoy_pos.y = world_floor_y(v3_add(decoy_pos, v3_(0, 2, 0)));
    if (decoy_pos.y < -1e8f) decoy_pos.y = pos.y;
    t3_object_set_position(decoy_obj, decoy_pos.x, decoy_pos.y, decoy_pos.z);
    t3_object_set_rotation(decoy_obj, 0, yaw + DS_PI, 0);
    decoy_obj->visible = true;
    sfx(SFX_DECOY, 0, 1);
  }
  if (decoy_t > 0) { decoy_t -= dt; if (decoy_t <= 0) { decoy_obj->visible = false; fx_explosion(decoy_pos, 1.2f); enemy_blast(decoy_pos, 5, 60); sfx(SFX_BOOM, 0, 1); } }
  /* pickups */
  int pk = world_take_pickup(v3_add(pos, v3_(0, 0.9f, 0)), 1.3f);
  if (pk >= 0) {
    sfx(SFX_PICKUP, 0, 1);
    if (pk == PK_HEALTH) { hp = fminf(HP_MAX, hp + 35); banner("FIRST AID KIT"); }
    else if (pk == PK_SNIPE_AMMO) { snipe_ammo += 8; banner("SCOPE ROUNDS +8"); }
    else if (pk == PK_DECOY) { decoys++; banner("DECOY +1"); }
    else if (pk == PK_SHIELD) { shield = 1; alert("SHIELD"); }
  }
  /* enemies */
  v3 from;
  float dmg = enemy_hurt_player(pos, JUNO_R, JUNO_H, &from);
  if (dmg > 0) hurt(dmg, from);
  /* checkpoints */
  v3 cp = world_checkpoint(pos);
  if (v3_len(v3_sub(cp, checkpoint)) > 1) { checkpoint = cp; alert("CHECKPOINT"); sfx(SFX_UI_OK, 0, 0.8f); }
  /* the radio */
  int beat = world_story_trigger(pos);
  if (beat >= 0) {
    const char *who, *line = story_radio(level, beat, &who);
    if (line) { H.radio_speaker = who; H.radio_text = line; H.radio_t = 5 + (float)strlen(line) * 0.04f; sfx(SFX_UI, 0, 0.5f); voice_say(cutscene_voice_for(who), line); }
  }
  /* arenas: the gates close behind her until the arena is clear */
  if (world_in_boss_arena(pos)) {
    v3 ac; float ay;
    if (!world_anchor(level == WORLD_LEVELS - 1 ? "boss_arena" : "brute_arena", &ac, &ay)) ac = pos;
    bool fight = enemy_alive_near(ac, 50, -1) > 0;
    world_set_gates(fight);
    if (level == 0 && !cut_seen[CUT_BRUTE] && fight) { play_cut(CUT_BRUTE, AFTER_PLAY); return; }
    if (level == WORLD_LEVELS - 1 && !boss_cut_done) { boss_cut_done = true; play_cut(CUT_BOSS, AFTER_BOSS); return; }
  } else world_set_gates(false);
  /* music follows the fighting */
  if (enemy_boss_active()) music_mood(MOOD_BOSS);
  else music_mood(combat_t > 0 ? MOOD_FIGHT : MOOD_EXPLORE);
  /* the way out */
  if (level == WORLD_LEVELS - 1 && enemy_boss_dead()) {
    /* let the boss's explosion chain play out, then the ending */
    if (!boss_dead_t_on) { boss_dead_t_on = true; boss_dead_t = 0; music_mood(MOOD_CLEAR); }
    boss_dead_t += dt;
  }
  bool done = level == WORLD_LEVELS - 1 ? (boss_dead_t_on && boss_dead_t > 4) : world_at_exit(pos);
  if (done && st == ST_PLAY) {
    st = ST_CLEAR; st_t = 0; scoped = false;
    music_mood(MOOD_CLEAR);
    int ms = (int)(level_time * 1000);
    H.new_best = false;
    if (!DS_SAVE.best_ms[level] || ms < (int)DS_SAVE.best_ms[level]) { DS_SAVE.best_ms[level] = ms; H.new_best = true; }
    if ((uint32_t)score > DS_SAVE.best_score[level]) { DS_SAVE.best_score[level] = score; H.new_best = true; }
    if (DS_SAVE.level_reached < level + 1 && level + 1 < WORLD_LEVELS) DS_SAVE.level_reached = level + 1;
    actor_play(&juno, "Wave", 1);
  }
  /* animation */
  float hs = sqrtf(vel.x * vel.x + vel.z * vel.z);
  if (st != ST_PLAY) {}
  else if (roll_t > 0) actor_play(&juno, "Duck", 1.6f);
  else if (!grounded) actor_play(&juno, gliding ? "Jump_Idle" : "Jump", 1);
  else if (hs > 0.6f) actor_play(&juno, (I->fire_held && weapon == 0) ? "Run_Gun_Shoot" : "Run_Gun", fmaxf(0.6f, hs / 7.5f * 1.1f));
  else actor_play(&juno, "Idle_Gun", 1);
  pose_wing(wing_open, (float)ds_now());
}

static void toggle_setting(int which) {
  if (which == 0) { DS_SAVE.music ^= 1; audio_set_music(DS_SAVE.music); }
  else if (which == 1) DS_SAVE.shake ^= 1;
  else DS_SAVE.invert_y ^= 1;
  sfx(SFX_UI_OK, 0, 0.8f);
}

/* title menu (hud_screens.c's rows): the unlocked levels (or the one PRESS
 * row), then music, shake and invert */
static void update_title(const intent_t *I) {
  int unlocked = (DS_SAVE.magic == DS_SAVE_MAGIC ? DS_SAVE.level_reached : 0) + 1;
  if (unlocked > WORLD_LEVELS) unlocked = WORLD_LEVELS;
  int rows = unlocked + 3;
  /* no wrapping: up from the top row must not land on a setting */
  if (I->nav_y) { int m = H.menu_sel + I->nav_y; if (m >= 0 && m < rows) { H.menu_sel = m; sfx(SFX_UI, 0, 0.6f); } }
  if (I->ok && st_t > 0.4f) {
    if (H.menu_sel < unlocked) { sfx(SFX_UI_OK, 0, 1); start_level(H.menu_sel); return; }   /* before the title camera overwrites the level's */
    else toggle_setting(H.menu_sel - unlocked);
  }
  H.unlocked = unlocked;
  /* the title camera drifts around Juno */
  cam_yaw = (float)ds_now() * 0.15f; cam_pitch = -0.12f;
  actor_play(&juno, "Idle_Gun", 1);
}

/* pause rows 0..3 (resume, restart, settings, quit); settings opens rows
 * 4..6 (music, shake, invert), B goes back to the settings row */
static void update_pause(const intent_t *I) {
  if (H.menu_sel >= 4) {
    if (I->nav_y) { int m = H.menu_sel + I->nav_y; if (m >= 4 && m < 7) { H.menu_sel = m; sfx(SFX_UI, 0, 0.6f); } }
    if (I->ok) toggle_setting(H.menu_sel - 4);
    if (I->back) { H.menu_sel = 2; sfx(SFX_UI, 0, 0.6f); }
    return;
  }
  if (I->nav_y) { int m = H.menu_sel + I->nav_y; if (m >= 0 && m < 4) { H.menu_sel = m; sfx(SFX_UI, 0, 0.6f); } }
  if (I->ok && H.menu_sel == 2) { H.menu_sel = 4; sfx(SFX_UI_OK, 0, 0.8f); return; }
  if (I->back || I->pause || (I->ok && H.menu_sel == 0)) { st = ST_PLAY; sfx(SFX_UI, 0, 0.6f); return; }
  if (I->ok && H.menu_sel == 1) { start_level(level); return; }
  if (I->ok && H.menu_sel == 3) { to_title(); return; }
}

#ifdef DS_TRACE
static void trace_count(t3_object *o, void *ctx) { int *c = ctx; c[0]++; if (o->type == T3_MESH || o->type == T3_SKINNED_MESH || o->type == T3_INSTANCED_MESH) c[1]++; }
#endif
void game_frame(float dt) {
  const intent_t *I = input_get();
  st_t += dt;
  /* one radio line a frame while the level runs (a line takes tens of ms to synthesise) */
  if (radio_prep < 8 && st != ST_CUT && st != ST_TITLE) {
    const char *who, *line = story_radio(level, radio_prep++, &who);
    if (line) voice_prepare(cutscene_voice_for(who), line);
  }
#ifdef DS_TRACE
  { static int fr; if (++fr % 60 == 0) ds_log("[trace] f%d st%d t%.1f pos %.1f %.1f %.1f vy %.1f hp %.0f alive %d wing %.2f", fr, st, st_t, pos.x, pos.y, pos.z, vel.y, hp, enemy_alive(), wing_open);
    const t3_render_info *ri = t3_renderer_info(DS_R);
    int cnt[2] = {0, 0}; t3_object_traverse(&scene->base, trace_count, cnt);
    ds_log("[trace] draw calls %u tris %u objects %d meshes %d", ri->calls, ri->triangles, cnt[0], cnt[1]); }
#endif
  if (st == ST_PAUSE) { update_pause(I); }
  else {
    switch (st) {
      case ST_TITLE: update_title(I); break;
      case ST_DROP: update_drop(dt, I); break;
      case ST_PLAY:
        if (I->pause) { st = ST_PAUSE; H.menu_sel = 0; scoped = false; sfx(SFX_UI, 0, 0.6f); break; }
        update_play(dt, I);
        break;
      case ST_DEAD:
        if (st_t > 1.2f && (I->ok || I->jump)) {
          st = ST_PLAY; st_t = 0;
          reset_player(checkpoint);
          snipe_ammo = snipe_ammo < 8 ? 8 : snipe_ammo;
          music_mood(MOOD_EXPLORE);
        }
        break;
      case ST_CLEAR:
        if (st_t > 1.5f && (I->ok || I->jump)) {
          if (level + 1 < WORLD_LEVELS) start_level(level + 1);
          else play_cut(CUT_ENDING, AFTER_END);
        }
        break;
      case ST_END:
        if (st_t > 3 && (I->ok || I->jump)) to_title();
        break;
      case ST_CUT:
        if (!cutscene_update(dt, I, &CV)) cut_finish();
        break;
      default: break;
    }
    if (st == ST_PLAY || st == ST_DROP || st == ST_DEAD || st == ST_CLEAR || st == ST_TITLE || st == ST_CUT) {
      world_update(dt, pos);
      if (st != ST_TITLE && st != ST_CUT) enemy_update(dt, pos, decoy_t > 0, decoy_pos);
      score += enemy_take_score();
    }
  }
  if (st != ST_PAUSE) {
    if (juno.root) {
      t3_object_set_position(juno.root, pos.x, pos.y, pos.z);
      t3_object_set_rotation(juno.root, 0, yaw, 0);
      juno.root->visible = !scoped;
      actor_update(&juno, dt);
    }
    if (st == ST_CUT) {
      /* the cutscene drives the camera and Juno */
      if (juno.root) {
        juno.root->visible = CV.juno_visible;
        t3_object_set_position(juno.root, CV.juno_pos.x, CV.juno_pos.y, CV.juno_pos.z);
        t3_object_set_rotation(juno.root, 0, CV.juno_yaw, 0);
        if (CV.juno_clip) actor_play(&juno, CV.juno_clip, CV.juno_clip_speed > 0 ? CV.juno_clip_speed : 1);
        pose_wing(CV.juno_wing ? 1 : 0, (float)ds_now());
      }
      cam->fov = CV.fov > 1 ? CV.fov : 50;
      t3_camera_update_projection_matrix(cam);
      t3_object_set_position(cam, CV.cam_pos.x, CV.cam_pos.y, CV.cam_pos.z);
      t3_object_look_at(cam, CV.cam_target.x, CV.cam_target.y, CV.cam_target.z);
      cam_pos = CV.cam_pos;
    } else update_camera(dt, I);
    fx_update(dt, cam);
  }
  v3 sc = st == ST_CUT ? CV.cam_target : pos;
  t3_object_set_position(sun, sc.x + 30, sc.y + 60, sc.z + 15);
  t3_object_set_position(sun->target, sc.x, sc.y, sc.z);

  /* the HUD */
  H.t = st_t;
  H.level = level;
  H.level_name = world_level_name(level);
  H.level_blurb = world_level_blurb(level);
  H.mode = st == ST_TITLE ? HUD_TITLE : st == ST_DROP ? (st_t < 3.5f ? HUD_INTRO : HUD_DROP) : st == ST_PAUSE ? HUD_PAUSE
         : st == ST_DEAD ? HUD_DEAD : st == ST_CLEAR ? HUD_CLEAR : st == ST_END ? HUD_END : st == ST_CUT ? HUD_CINE : HUD_PLAY;
  H.cine = st == ST_CUT ? CV.letterbox : 0;
  H.cap_speaker = st == ST_CUT ? CV.speaker : NULL; H.cap_text = st == ST_CUT ? CV.text : NULL; H.cap_chars = CV.chars;
  H.skip = st == ST_CUT ? CV.skip : 0;
  if (st != ST_PAUSE) H.radio_t = fmaxf(0, H.radio_t - dt);
  if (H.radio_t <= 0) H.radio_text = NULL;
  H.health = hp / HP_MAX; H.shield = shield;
  H.snipe_ammo = snipe_ammo; H.decoys = decoys; H.weapon = scoped ? 1 : weapon; H.spin = spin;
  H.score = score; H.time_s = level_time;
  H.hit_marker = fmaxf(0, H.hit_marker - dt);
  H.hurt = fmaxf(0, H.hurt - dt);
  H.used_pad = I->used_pad;
  H.banner_t = fmaxf(0, H.banner_t - dt);
  if (H.banner_t <= 0) H.banner = NULL;
  H.scoped = scoped; H.zoom = zoom;
  H.on_enemy = H.on_weak = false; H.weak_mark = false; H.range_m = 0;
  if (scoped) {
    v3 o, d; aim_ray(&o, &d);
    float t, et; v3 n; int id; bool weak;
    bool hw = world_raycast(o, d, 2000, &t, &n);
    H.range_m = hw ? t : 0;
    if (enemy_raycast(o, d, hw ? t : 2000, &id, &et, &weak)) { H.on_enemy = true; H.on_weak = weak; H.range_m = et; }
    v3 at;
    if (enemy_weak_point_near(o, d, cosf(ZOOM_FOV[zoom] * DS_PI / 180 * 0.5f), &at)) {
      /* project into the 1280x720 frame */
      v3 rel = v3_sub(at, cam_pos);
      v3 rgt = v3_norm(v3_cross(cam_fwd, v3_(0, 1, 0))), up = v3_cross(rgt, cam_fwd);
      float z = v3_dot(rel, cam_fwd);
      if (z > 0.1f) {
        float f = 1 / tanf(fov * DS_PI / 360);
        H.weak_x = DS_W * 0.5f + v3_dot(rel, rgt) / z * f * DS_H * 0.5f;
        H.weak_y = DS_H * 0.5f - v3_dot(rel, up) / z * f * DS_H * 0.5f;
        H.weak_mark = true;
      }
    }
  }
  H.boss = enemy_boss_active(); H.boss_health = enemy_boss_health(); H.boss_phase = enemy_boss_phase();
  H.boss_name = level == WORLD_LEVELS - 1 ? "THE SUPERINTENDENT" : "BRUTE";
  H.music_on = DS_SAVE.music; H.shake_on = DS_SAVE.shake; H.invert_y = DS_SAVE.invert_y;
  H.kills = kills; H.kills_total = kills + enemy_alive(); H.clear_time_s = level_time;
  H.best_time_ms = DS_SAVE.best_ms[level]; H.best_score = DS_SAVE.best_score[level];
}

void game_draw(void) {
  render_scene(scene, cam);
  hud_draw(&H);
}
