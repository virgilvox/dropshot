/* enemy.c: the Tenants. Grunts, flyers, turrets, Brutes and the Superintendent.
 * Yaw is a rotation about +y with forward = (sin yaw, 0, cos yaw), like Juno.
 * Models are parsed once per instance (three.c has no skeleton clone), the
 * GLB bytes are cached and the parsed actors are recycled through spare lists. */
#include "enemy.h"
#include "fx.h"
#include "world.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXE 96          /* the levels place 60 to 70 Tenants; far ones sleep (see enemy_update) */
#define STREAM_R 120.f   /* a Tenant's model is built when the player comes this close */
#define SLEEP_R 90.f     /* beyond this an unalerted enemy is neither updated nor drawn */
#define FLASH_T 0.1f
#define MAXP 64
#define MAXS 8
#define MAXW 4
#define NSPARE 12

enum { M_LARGE, M_FLYING, M_SMALL, M_MECH_A, M_MECH_B, M_COUNT };
static const char *model_file[M_COUNT] = { "e_large.glb", "e_flying.glb", "e_small.glb", "e_mech_a.glb", "e_mech_b.glb" };
/* per-model scale multiplier (the models are already near their nominal size: grunt 2.4 m, mech 2.56 m at 1.0) */
static const float model_unit[M_COUNT] = { 1, 1, 1, 1, 1 };
static uint8_t *model_bytes[M_COUNT];
static size_t model_len[M_COUNT];

typedef struct {
  t3_gltf *gltf;
  t3_object *root, *model;
  t3_animation_mixer *mixer;
  t3_animation_action *cur, *prev;
  float fade, dur;
  char name[24];
  bool frozen;
  int kind;
  t3_material *mats[6]; int nmats;
} actor_t;

static t3_scene *scn;
static actor_t *spare[M_COUNT][NSPARE];
static int nspare[M_COUNT];

static void collect_mats(t3_object *o, void *ctx) {
  actor_t *a = ctx;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  o->frustum_culled = false; o->cast_shadow = true; o->receive_shadow = true;
  t3_mesh *m = (t3_mesh *)o;
  for (int i = 0; i < m->material_count && a->nmats < 6; i++) if (m->materials[i]) {
    m->materials[i]->metalness = 0.15f; m->materials[i]->roughness = 0.65f; m->materials[i]->version++;
    a->mats[a->nmats++] = m->materials[i];
  }
}

static actor_t *actor_get(int kind, float scale) {
  actor_t *a;
  if (nspare[kind]) a = spare[kind][--nspare[kind]];
  else {
    if (!model_bytes[kind]) model_bytes[kind] = ds_asset(model_file[kind], &model_len[kind]);
    if (!model_bytes[kind]) return NULL;
    a = calloc(1, sizeof *a);
    a->gltf = t3_gltf_parse(model_bytes[kind], model_len[kind], NULL, NULL);
    if (!a->gltf) { free(a); return NULL; }
    a->kind = kind;
    a->root = t3_object_new(); a->root->rotation.order = T3_YXZ;
    a->model = a->gltf->scene;
    t3_object_add(a->root, a->model);
    t3_object_traverse(a->model, collect_mats, a);
    a->mixer = t3_animation_mixer_new(a->model);
  }
  float s = model_unit[kind] * scale;
  t3_object_set_scale(a->model, s, s, s);
  a->cur = a->prev = NULL; a->frozen = false; a->name[0] = 0;
  t3_object_add(&scn->base, a->root);
  return a;
}

static void actor_put(actor_t *a) {
  if (!a) return;
  if (a->cur) t3_animation_action_stop(a->cur);
  if (a->prev) t3_animation_action_stop(a->prev);
  a->cur = a->prev = NULL;
  t3_object_remove(&scn->base, a->root);
  for (int i = 0; i < a->nmats; i++) { a->mats[i]->emissive = t3_color_hex(0); a->mats[i]->color = t3_color_hex(0xffffff); a->mats[i]->version++; }
  if (nspare[a->kind] < NSPARE) spare[a->kind][nspare[a->kind]++] = a;
}

/* clip names come as "CharacterArmature|Run": match the part after the bar */
static t3_animation_clip *actor_clip(actor_t *a, const char *name) {
  for (int i = 0; i < a->gltf->animation_count; i++) {
    const char *n = a->gltf->animations[i]->name, *bar = strrchr(n, '|');
    if (!strcmp(bar ? bar + 1 : n, name)) return a->gltf->animations[i];
  }
  return NULL;
}

/* restart: a one-shot starts from its first frame */
static void actor_play(actor_t *a, const char *name, float speed, bool restart) {
  if (!a) return;
  if (!restart && a->cur && !strcmp(a->name, name)) { if (!a->frozen) t3_animation_action_set_effective_time_scale(a->cur, speed); return; }
  t3_animation_clip *c = actor_clip(a, name);
  if (!c) return;
  t3_animation_action *act = t3_animation_mixer_clip_action(a->mixer, c);
  if (act == a->cur && !restart) return;
  if (a->prev && a->prev != act) t3_animation_action_stop(a->prev);
  a->prev = a->cur != act ? a->cur : NULL;
  a->cur = act; a->dur = c->duration; a->frozen = false;
  snprintf(a->name, sizeof a->name, "%s", name);
  if (restart) act->time = 0;
  t3_animation_action_set_effective_time_scale(act, speed);
  t3_animation_action_play(act);
  t3_animation_action_set_effective_weight(act, 0);
  a->fade = 0;
}

static void actor_update(actor_t *a, float dt, bool hold_end) {
  if (!a || !a->cur) return;
  a->fade = fminf(1, a->fade + dt * 9);
  t3_animation_action_set_effective_weight(a->cur, a->fade);
  if (a->prev) {
    t3_animation_action_set_effective_weight(a->prev, 1 - a->fade);
    if (a->fade >= 1) { t3_animation_action_stop(a->prev); a->prev = NULL; }
  }
  if (a->frozen) return;
  t3_animation_mixer_update(a->mixer, dt);
  if (hold_end && a->cur->time >= a->dur - 0.03f) { a->cur->time = a->dur - 0.02f; t3_animation_action_set_effective_time_scale(a->cur, 0); a->frozen = true; }
}

/* hold a clip at one moment (the boss asleep, and rising): k 0..1 is how far through the clip */
static void actor_pose(actor_t *a, const char *name, float k) {
  if (!a) return;
  actor_play(a, name, 0, false);
  if (!a->cur) return;
  if (a->prev) { t3_animation_action_stop(a->prev); a->prev = NULL; }
  a->fade = 1; t3_animation_action_set_effective_weight(a->cur, 1);
  a->cur->time = ds_clamp(k, 0, 1) * (a->dur - 0.03f);
  a->frozen = true;
  t3_animation_mixer_update(a->mixer, 0);
}

/* ── enemies ── */
enum { S_IDLE, S_PATROL, S_ALERT, S_COMBAT, S_WIND, S_ACT, S_RECOVER, S_DIVE, S_LOCK, S_BURST, S_STAGGER, S_DEAD, S_DORMANT, S_RISE };
enum { A_NONE, A_SHOOT, A_PUNCH, A_STOMP, A_SWEEP, A_FLYERS, A_BOMBS };

typedef struct { bool alive; float hp, r; v3 off, p; t3_mesh *mesh; } wp_t;

typedef struct {
  bool used, minion, seen, los, grounded, melee_on, melee_done;
  int type, state, atk, shots, phase;
  v3 pos, vel, home, last_seen, mpos;
  float yaw, hp, maxhp, scale, H, r, h;
  float st, cd, strafe, strafe_t, flash, hit_cd, los_t, lost_t, dead_t, turn, orbit, orbit_dir, phase_t, mr, mdmg, anim_t, aim, aux, aux2;
  bool pending; int fkind;           /* model not built yet (fkind: which flyer skin) */
  float tint[3], tinted, power;     /* power: 0 dark and asleep .. 1 lit */
  v3 kb;                            /* knockback velocity, decays */
  float lean, stag, ping_cd, wake_t; /* hit lean (rad), damage since the last flinch, armour ping cooldown, boss rise time */
  actor_t *a;
  wp_t wp[MAXW]; int nwp;
  /* turret parts */
  t3_object *tgroup, *thead; t3_mesh *tlens;
} en_t;

static en_t E[MAXE];

/* the Tenants' tint per ship (docs/STORY.md): lime on the Lessee, moss and teal on the Sublet, sickly violet on the Freehold.
 * col multiplies the green skin, emis is the glow under it, fx colours the dust they shed and the small one is a shade lighter */
static const struct { uint32_t col, col_small, emis; float ei; uint32_t fx; } PAL[3] = {
  { 0xe8ff90, 0x50d040, 0xa8e020, 0.34f, 0xc8f060 },
  { 0xa0f0d8, 0x40c890, 0x30c8a0, 0.38f, 0x7fe0c0 },
  { 0xd8b8ff, 0xa890d8, 0x88c898, 0.36f, 0xb69cf0 },
};
static int lvl;
/* six levels share three ship palettes: Cold Storage is aboard the Lessee,
 * the Annex Tenants are refugees from the Sublet, the Basement is the Freehold's */
void enemy_set_level(int level) {
  static const int pal[6] = {0, 1, 0, 1, 2, 2};
  lvl = level < 0 ? 0 : level > 5 ? 2 : pal[level];
}
static int score_acc, boss_id = -1;
static v3 tgt;
static bool tgt_decoy;
static float gtime;

static v3 fwd(float yaw) { return v3_(sinf(yaw), 0, cosf(yaw)); }
static v3 rgt(float yaw) { return v3_(cosf(yaw), 0, -sinf(yaw)); }
static v3 chest(v3 p) { return v3_(p.x, p.y + 1.2f, p.z); }
static v3 en_local(const en_t *e, v3 o) { return v3_add(e->pos, v3_add(v3_add(v3_scale(rgt(e->yaw), o.x), v3_(0, o.y, 0)), v3_scale(fwd(e->yaw), o.z))); }
static float flat_dist(v3 a, v3 b) { return hypotf(a.x - b.x, a.z - b.z); }

/* ── projectiles, shockwaves, blasts ── */
enum { PJ_GOO, PJ_FAST, PJ_BIG, PJ_BOMB };
typedef struct { bool on; int kind, src; v3 p, pp, v, land; float life, dmg, r; t3_mesh *mesh; } proj_t;
typedef struct { bool on, hit; v3 c; float r, speed, max, dmg, delay; int src; } wave_t;
typedef struct { float life; bool hit; v3 c; float r, dmg; int src; } blast_t;
#ifdef DS_BOT
#define DBG_DORMANT(e) ((e)->state == S_DORMANT || (e)->state == S_RISE)
static void dbg_on_damage(int id);
static void dbg_on_kill(int id, int type);
#define DBG_SHOT(k) ds_log("[bot] shot kind %d src %d gtime %.1f", k, cur_src, gtime)
#define DBG_HIT(id) dbg_on_damage(id)
#define DBG_KILL(id, t) dbg_on_kill(id, t)
#else
#define DBG_SHOT(k) ((void)0)
#define DBG_HIT(id) ((void)0)
#define DBG_KILL(id, t) ((void)0)
#endif
static int cur_src;
static int last_src __attribute__((unused));   /* the enemy type whose update is running; who last hurt the player */
static proj_t PJ[MAXP];
static wave_t WV[MAXS];
static blast_t BL[8];
static t3_geometry *g_ball;

static void proj_color(proj_t *q, uint32_t hex) { t3_material *m = q->mesh->materials[0]; m->color = t3_color_hex(hex); m->version++; }

static proj_t *shoot(int kind, v3 from, v3 dir, float speed, float dmg) {
  proj_t *q = NULL;
  for (int i = 0; i < MAXP; i++) if (!PJ[i].on) { q = &PJ[i]; break; }
  if (!q) return NULL;
  static const float rad[] = { 0.4f, 0.16f, 0.75f, 0.6f }, life[] = { 5, 3, 6, 6 };
  static const uint32_t col[] = { C_GOO, 0xff9a3a, 0xff7a1a, 0xff4a30 };
  DBG_SHOT(kind);
  q->on = true; q->kind = kind; q->p = q->pp = from; q->v = v3_scale(v3_norm(dir), speed);
  q->dmg = dmg; q->r = rad[kind]; q->life = life[kind]; q->src = cur_src;
  proj_color(q, col[kind]);
  float s = rad[kind] * 2;
  t3_object_set_scale(q->mesh, s, s, s);
  ((t3_object *)q->mesh)->visible = true;
  return q;
}

static void lob(v3 from, v3 to, float tf) {
  float g = 16;
  proj_t *q = shoot(PJ_BOMB, from, v3_(0, 1, 0), 1, 15);
  if (!q) return;
  q->v = v3_((to.x - from.x) / tf, (to.y - from.y + 0.5f * g * tf * tf) / tf, (to.z - from.z) / tf);
  q->land = to; q->life = tf + 1.5f;
}

static void wave(v3 c, float speed, float max, float dmg, float delay) {
  for (int i = 0; i < MAXS; i++) if (!WV[i].on) { WV[i] = (wave_t){ true, false, c, 0.5f, speed, max, dmg, delay, cur_src }; return; }
}
static void blast_player(v3 c, float r, float dmg) {
  for (int i = 0; i < 8; i++) if (BL[i].life <= 0) { BL[i] = (blast_t){ 0.1f, false, c, r, dmg, cur_src }; return; }
}
static void proj_burst(proj_t *q) {
  int keep = cur_src; cur_src = q->src;
  if (q->kind == PJ_BOMB) { fx_explosion(q->p, 2.4f); sfx(SFX_BOOM, 0, 1); blast_player(q->p, 5.5f, q->dmg); }
  else if (q->kind == PJ_BIG) { fx_explosion(q->p, 1.2f); sfx(SFX_BOOM, 0, 0.7f); blast_player(q->p, 3.2f, q->dmg * 0.5f); }
  else if (q->kind == PJ_GOO) fx_goo(q->p, 6);
  else fx_sparks(q->p, v3_(0, 1, 0), 0xffb347, 5);
  cur_src = keep;
  q->on = false; ((t3_object *)q->mesh)->visible = false;
}

static void proj_update(float dt) {
  static const uint32_t glow[] = { C_GOO, 0xff9a3a, 0xff7a1a, 0xff4a30 };
  for (int i = 0; i < MAXP; i++) {
    proj_t *q = &PJ[i];
    if (!q->on) continue;
    q->pp = q->p;
    if (q->kind == PJ_BOMB) q->v.y -= 16 * dt;
    v3 step = v3_scale(q->v, dt);
    float sl = v3_len(step), t;
    q->life -= dt;
    bool end = q->life <= 0;
    if (sl > 1e-4f && world_raycast(q->p, v3_scale(step, 1 / sl), sl + q->r * 0.5f, &t, NULL)) { q->p = v3_add(q->p, v3_scale(step, t / sl)); end = true; }
    else q->p = v3_add(q->p, step);
    if (q->kind == PJ_BOMB && q->p.y < world_floor_y(v3_(q->p.x, q->p.y + 1, q->p.z)) + 0.4f) end = true;
    if (tgt_decoy && v3_len(v3_sub(q->p, chest(tgt))) < 1.0f) end = true;
    t3_object_set_position(q->mesh, q->p.x, q->p.y, q->p.z);
    float pulse = 0.85f + 0.15f * sinf(gtime * 30 + i);
    fx_glow(q->p, q->r * (q->kind == PJ_FAST ? 5 : 3.2f) * pulse, glow[q->kind]);
    if (q->kind == PJ_FAST) fx_beam(q->p, v3_sub(q->p, v3_scale(v3_norm(q->v), 1.6f)), 0xffb347, 0.14f);
    else if (q->kind == PJ_GOO) fx_beam(q->p, v3_sub(q->p, v3_scale(v3_norm(q->v), 1.1f)), 0x7ac040, 0.35f);
    if (q->kind == PJ_BOMB) fx_ring_flat(v3_(q->land.x, world_floor_y(v3_(q->land.x, q->land.y + 1, q->land.z)) + 0.1f, q->land.z), 5.5f, 0xff3a28);
    if (end) proj_burst(q);
  }
  for (int i = 0; i < MAXS; i++) {
    wave_t *w = &WV[i];
    if (!w->on) continue;
    if (w->delay > 0) { w->delay -= dt; fx_ring_flat(v3_(w->c.x, w->c.y + 0.1f, w->c.z), w->max, 0xff3a28); continue; }
    w->r += w->speed * dt;
    fx_ring_flat(v3_(w->c.x, w->c.y + 0.12f, w->c.z), w->r, 0xff8a40);
    fx_ring_flat(v3_(w->c.x, w->c.y + 0.12f, w->c.z), w->r - 0.5f, 0xffd8a0);
    if (w->r >= w->max) w->on = false;
  }
  for (int i = 0; i < 8; i++) if (BL[i].life > 0) BL[i].life -= dt;
}

/* nearest point of a vertical cylinder to a sphere centre: does the sphere touch it? */
static bool cyl_sphere(v3 p, float r, float h, v3 c, float rad) {
  float dx = c.x - p.x, dz = c.z - p.z, l = hypotf(dx, dz);
  float cx = l > r ? p.x + dx / l * r : c.x, cz = l > r ? p.z + dz / l * r : c.z;
  float cy = ds_clamp(c.y, p.y, p.y + h);
  float ex = c.x - cx, ey = c.y - cy, ez = c.z - cz;
  return ex * ex + ey * ey + ez * ez < rad * rad;
}

float enemy_hurt_player(v3 p, float r, float h, v3 *from) {
  float total = 0; v3 src = v3_(0, 0, 0);
  for (int i = 0; i < MAXP; i++) {
    proj_t *q = &PJ[i];
    if (!q->on) continue;
    v3 mid = v3_lerp(q->pp, q->p, 0.5f);
    if (cyl_sphere(p, r, h, q->p, q->r) || cyl_sphere(p, r, h, mid, q->r)) {
      if (q->kind == PJ_BOMB || q->kind == PJ_BIG) { proj_burst(q); continue; } /* the blast below does the damage */
      total += q->dmg; src = q->p; last_src = q->src; proj_burst(q);
    }
  }
  for (int i = 0; i < 8; i++) {
    blast_t *b = &BL[i];
    if (b->life <= 0 || b->hit) continue;
    v3 c = v3_(ds_clamp(b->c.x, p.x - r, p.x + r), ds_clamp(b->c.y, p.y, p.y + h), ds_clamp(b->c.z, p.z - r, p.z + r));
    float d = v3_len(v3_sub(c, b->c));
    if (d < b->r) { b->hit = true; total += b->dmg * (1 - 0.6f * d / b->r); src = b->c; last_src = b->src; }
  }
  for (int i = 0; i < MAXS; i++) {
    wave_t *w = &WV[i];
    if (!w->on || w->hit || w->delay > 0) continue;
    float d = flat_dist(p, w->c);
    if (fabsf(d - w->r) < 0.9f + r && p.y < w->c.y + 1.0f && p.y + h > w->c.y) { w->hit = true; total += w->dmg; src = w->c; last_src = w->src; }
  }
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (!e->used || !e->melee_on || e->melee_done || e->state == S_DEAD) continue;
    if (cyl_sphere(p, r, h, e->mpos, e->mr)) { e->melee_done = true; total += e->mdmg; src = e->pos; last_src = e->type; }
  }
  if (total > 0 && from) *from = src;
  return total;
}

/* ── geometry queries ── */
static float sph_hit(v3 ro, v3 rd, v3 c, float r) {
  v3 oc = v3_sub(ro, c);
  float b = v3_dot(oc, rd), k = v3_dot(oc, oc) - r * r, h = b * b - k;
  if (h < 0) return -1;
  float t = -b - sqrtf(h);
  if (t < 0) t = -b + sqrtf(h);
  return t;
}
static float cap_hit(v3 ro, v3 rd, v3 pa, v3 pb, float r) {
  float best = -1, t;
  v3 ba = v3_sub(pb, pa), oa = v3_sub(ro, pa);
  float baba = v3_dot(ba, ba), bard = v3_dot(ba, rd), baoa = v3_dot(ba, oa), rdoa = v3_dot(rd, oa), oaoa = v3_dot(oa, oa);
  float a = baba - bard * bard, b = baba * rdoa - baoa * bard, c = baba * oaoa - baoa * baoa - r * r * baba, h = b * b - a * c;
  if (a > 1e-6f && h >= 0) {
    t = (-b - sqrtf(h)) / a;
    float y = baoa + t * bard;
    if (t >= 0 && y > 0 && y < baba) best = t;
  }
  t = sph_hit(ro, rd, pa, r); if (t >= 0 && (best < 0 || t < best)) best = t;
  t = sph_hit(ro, rd, pb, r); if (t >= 0 && (best < 0 || t < best)) best = t;
  return best;
}
static void body_cap(const en_t *e, v3 *a, v3 *b, float *r) {
  if (e->type == EN_FLYER) { *a = v3_(e->pos.x, e->pos.y - 0.2f, e->pos.z); *b = v3_(e->pos.x, e->pos.y + 0.3f, e->pos.z); *r = e->r; return; }
  *r = e->r; *a = v3_(e->pos.x, e->pos.y + e->r, e->pos.z); *b = v3_(e->pos.x, e->pos.y + fmaxf(e->h - e->r, e->r), e->pos.z);
}

static bool untargetable(const en_t *e) { return !e->used || e->state == S_DEAD || e->state == S_DORMANT || e->state == S_RISE; }

bool enemy_raycast(v3 o, v3 d, float max_dist, int *id, float *t, bool *weak) {
  float best = max_dist; int bi = -1; bool bw = false;
  d = v3_norm(d);
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (untargetable(e)) continue;
    v3 a, b; float r; body_cap(e, &a, &b, &r);
    float tb = cap_hit(o, d, a, b, r), tw = -1; bool isw = false;
    for (int k = 0; k < e->nwp; k++) if (e->wp[k].alive) {
      float x = sph_hit(o, d, e->wp[k].p, e->wp[k].r + 0.15f);
      if (x >= 0 && (tw < 0 || x < tw)) tw = x;
    }
    float th = tb;
    if (tw >= 0 && (tb < 0 || tw < tb + 0.35f)) { th = tw; isw = true; }
    if (th >= 0 && th < best) { best = th; bi = i; bw = isw; }
  }
  if (bi < 0) return false;
  if (id) *id = bi; if (t) *t = best; if (weak) *weak = bw;
  return true;
}

bool enemy_weak_point_near(v3 o, v3 d, float cc, v3 *at) {
  float best = cc; bool f = false; d = v3_norm(d);
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (untargetable(e)) continue;
    for (int k = 0; k < e->nwp; k++) if (e->wp[k].alive) {
      v3 w = v3_sub(e->wp[k].p, o);
      float l = v3_len(w);
      if (l < 1e-3f) continue;
      float c = v3_dot(w, d) / l;
      if (c >= best) { best = c; *at = e->wp[k].p; f = true; }
    }
  }
  return f;
}

/* ── lifecycle ── */
static void wp_add(en_t *e, v3 off, float r) {
  if (e->nwp >= MAXW) return;
  wp_t *w = &e->wp[e->nwp++];
  t3_material *m = t3_mesh_basic_material_new(0xffa030);
  w->mesh = t3_mesh_new(g_ball, m);
  t3_release(m);
  ((t3_object *)w->mesh)->frustum_culled = false;
  t3_object_add(&scn->base, w->mesh);
  w->off = off; w->r = r; w->alive = true; w->hp = 0;
  t3_object_set_scale(w->mesh, r * 2, r * 2, r * 2);
}
static void wp_clear(en_t *e) {
  for (int i = 0; i < e->nwp; i++) if (e->wp[i].mesh) { t3_object_remove(&scn->base, e->wp[i].mesh); e->wp[i].mesh = NULL; }
  e->nwp = 0;
}

static void tint(en_t *e, uint32_t col, uint32_t emis, float ei) {
  actor_t *a = e->a;
  if (!a) return;
  t3_color em = t3_color_hex(emis);
  e->tint[0] = em.r * ei; e->tint[1] = em.g * ei; e->tint[2] = em.b * ei;
  for (int i = 0; i < a->nmats; i++) { a->mats[i]->color = t3_color_hex(col); a->mats[i]->version++; }
  e->tinted = -1;
}
static void apply_flash(en_t *e, float glow) {
  actor_t *a = e->a;
  if (!a) return;
  float k = e->flash > 0 ? e->flash / FLASH_T : 0, g = glow + k + e->power * 7;
  if (fabsf(g - e->tinted) < 0.01f) return;
  e->tinted = g;
  float pw = e->power, wk = k * 0.8f;   /* the hit flash is white, so it reads on every tint */
  for (int i = 0; i < a->nmats; i++) {
    t3_material *m = a->mats[i];
    m->emissive = (t3_color){ e->tint[0] * pw * (1 + glow * 2) + wk + glow * 0.4f, e->tint[1] * pw * (1 - glow * 0.4f) + wk, e->tint[2] * pw * (1 - glow * 0.4f) + wk };
    m->emissive_intensity = 1; m->version++;
  }
}

/* the sleeping boss is the first part of its Death clip held, crouched with its head down; rising plays it back */
#define BOSS_SLEEP_K 0.34f
static void boss_pose(en_t *e, float k) {
  actor_pose(e->a, "Death", BOSS_SLEEP_K * (1 - k));
  float c = 0.35f + 0.65f * k;
  if (e->a) for (int i = 0; i < e->a->nmats; i++) { e->a->mats[i]->color = (t3_color){ c, c, c }; e->a->mats[i]->version++; }
  e->power = k;
}

static t3_mesh *prim(t3_geometry *g, uint32_t col, t3_object *parent, float x, float y, float z) {
  t3_material *m = t3_mesh_standard_material_new(col);
  m->metalness = 0.5f; m->roughness = 0.5f;
  t3_mesh *me = t3_mesh_new(g, m);
  t3_release(m); t3_release(g);
  ((t3_object *)me)->cast_shadow = true; ((t3_object *)me)->receive_shadow = true;
  t3_object_set_position(me, x, y, z);
  t3_object_add(parent, me);
  return me;
}

static void build_turret(en_t *e) {
  e->tgroup = t3_group_new();
  prim(t3_cylinder_geometry_new(1.0f, 1.2f, 0.5f, 10, 1, false), 0x4a5160, e->tgroup, 0, 0.25f, 0);
  prim(t3_cylinder_geometry_new(0.4f, 0.55f, 1.0f, 8, 1, false), 0x7b8494, e->tgroup, 0, 1.0f, 0);
  e->thead = t3_group_new();
  t3_object_set_position(e->thead, 0, 1.75f, 0);
  t3_object_add(e->tgroup, e->thead);
  prim(t3_box_geometry_new(1.1f, 0.8f, 1.4f, 1, 1, 1), 0xc9532a, e->thead, 0, 0, 0);
  prim(t3_box_geometry_new(1.3f, 0.2f, 0.9f, 1, 1, 1), 0x2a2e3a, e->thead, 0, 0.5f, -0.1f);
  t3_mesh *b1 = prim(t3_cylinder_geometry_new(0.1f, 0.1f, 1.5f, 8, 1, false), 0x2a2e3a, e->thead, 0.3f, -0.1f, 1.0f);
  t3_mesh *b2 = prim(t3_cylinder_geometry_new(0.1f, 0.1f, 1.5f, 8, 1, false), 0x2a2e3a, e->thead, -0.3f, -0.1f, 1.0f);
  t3_object_set_rotation(b1, DS_PI / 2, 0, 0); t3_object_set_rotation(b2, DS_PI / 2, 0, 0);
  e->tlens = prim(t3_sphere_geometry_new(0.2f, 10, 8), 0xff3a28, e->thead, 0, 0.1f, 0.72f);
  e->tlens->materials[0]->emissive = t3_color_hex(0xff3a28); e->tlens->materials[0]->version++;
  e->thead->rotation.order = T3_YXZ;
  e->tgroup->rotation.order = T3_YXZ;
  t3_object_add(&scn->base, e->tgroup);
}

void enemy_init(t3_scene *scene) {
  scn = scene;
  g_ball = t3_sphere_geometry_new(0.5f, 12, 8);
  for (int i = 0; i < MAXP; i++) {
    t3_material *m = t3_mesh_basic_material_new(0xffffff);
    PJ[i].mesh = t3_mesh_new(g_ball, m);
    t3_release(m);
    ((t3_object *)PJ[i].mesh)->visible = false; ((t3_object *)PJ[i].mesh)->frustum_culled = false;
    t3_object_add(&scene->base, PJ[i].mesh);
  }
  for (int k = 0; k < M_COUNT; k++) if (!model_bytes[k]) model_bytes[k] = ds_asset(model_file[k], &model_len[k]);
}

static void en_free(en_t *e) {
  actor_put(e->a);
  wp_clear(e);
  if (e->tgroup) { t3_object_remove(&scn->base, e->tgroup); t3_release(e->tgroup); }
  memset(e, 0, sizeof *e);
}

void enemy_clear(void) {
  for (int i = 0; i < MAXE; i++) if (E[i].used) en_free(&E[i]);
  for (int i = 0; i < MAXP; i++) { PJ[i].on = false; ((t3_object *)PJ[i].mesh)->visible = false; }
  memset(WV, 0, sizeof WV); memset(BL, 0, sizeof BL);
  boss_id = -1; score_acc = 0;
}

/* Tuned against the player: chain gun 7 per round at about 13 a second, scope shot 120 (240 on a weak point), 100 HP.
 * grunt: about 1.3 s of chain gun, or a scope shot to the body and a round or two. Brute: about 22 s of chain gun, or
 * 5 scope hits on its weak point. Boss: the pods are the fight, 2 or 3 scope hits each; the chain gun barely scratches them. */
typedef struct { float hp; int score; float H, scale, r_frac; } info_t;
static const info_t INFO[EN_TYPE_COUNT] = {
  { 125, 100, 2.2f, 1.0f, 0.38f }, { 28, 75, 1.2f, 1.0f, 0.55f }, { 140, 150, 2.6f, 1.0f, 0.4f },
  { 2000, 1000, 4.1f, 1.6f, 0.28f }, { 3000, 5000, 15.4f, 6.0f, 0.2f },
};
static const float POD_HP[3] = { 440, 480, 520 };   /* per phase: 2, 2, 3 scope weak-point hits (240 each) */
#define BOSS_PODS 4
#define HEAVY 40.f                                   /* a hit this big is a scope round, not the chain gun */

/* build a Tenant's model (parsing a GLB is the slow part of a spawn, so a level of 60 does it as you go) */
static void attach_model(en_t *e) {
  e->pending = false;
  int lv = lvl;
  if (e->type == EN_GRUNT) {
    e->a = actor_get(M_LARGE, 1); actor_play(e->a, "Idle", 1, true);
    tint(e, PAL[lv].col, PAL[lv].emis, PAL[lv].ei);
  } else {
    e->a = actor_get(e->fkind ? M_SMALL : M_FLYING, 1); actor_play(e->a, "Flying_Idle", 1, true);
    tint(e, e->fkind ? PAL[lv].col_small : PAL[lv].col, PAL[lv].emis, PAL[lv].ei);
  }
  if (e->a) t3_object_set_position(e->a->root, e->pos.x, e->pos.y - (e->type == EN_FLYER ? 0.8f : 0), e->pos.z);
}

void enemy_spawn(int type, v3 pos, float yaw) {
  if (type < 0 || type >= EN_TYPE_COUNT) return;
  en_t *e = NULL; int id = 0;
  for (int i = 0; i < MAXE; i++) if (!E[i].used) { e = &E[i]; id = i; break; }
  if (!e) return;
  memset(e, 0, sizeof *e);
  const info_t *I = &INFO[type];
  e->used = true; e->type = type; e->pos = e->home = pos; e->yaw = yaw; e->hp = e->maxhp = I->hp;
  e->scale = I->scale; e->H = I->H; e->h = I->H; e->r = I->H * I->r_frac;
  e->state = S_IDLE; e->cd = ds_rr(0.5f, 1.5f); e->strafe = ds_rand() < 0.5f ? 1.f : -1.f; e->orbit_dir = e->strafe; e->orbit = ds_rr(0, 6.28f);
  e->tinted = -1; e->phase = 1; e->power = 1;
  switch (type) {
  case EN_GRUNT: e->pending = true; break;   /* the model is built when the player gets near */
  case EN_FLYER:
    e->pending = true; e->fkind = id & 1;
    if (e->fkind) { e->H = 0.9f; e->r = 0.5f; }
    e->pos.y += 5; break;
  case EN_TURRET: build_turret(e); e->h = 2.6f; e->aim = yaw;
    wp_add(e, v3_(0, 0, 0), 0.26f);   /* the lens: the plating hides the rest */
    break;
  case EN_BRUTE:
    e->a = actor_get(M_MECH_A, e->scale); actor_play(e->a, "Idle", 1, true);
    tint(e, 0xffc89a, 0x1a0804, 1);
    wp_add(e, v3_(0, 0.62f * e->H, -0.2f * e->H), 0.11f * e->H); break;
  case EN_BOSS:
    e->a = actor_get(M_MECH_B, e->scale); actor_play(e->a, "Idle", 1, true);
    tint(e, 0xffffff, 0x401408, 0.5f);
    wp_add(e, v3_(0, 0.64f * e->H, -0.2f * e->H), 0.08f * e->H);
    wp_add(e, v3_(-0.27f * e->H, 0.74f * e->H, 0), 0.07f * e->H);
    wp_add(e, v3_(0.27f * e->H, 0.74f * e->H, 0), 0.07f * e->H);
    wp_add(e, v3_(0, 0.6f * e->H, 0.19f * e->H), 0.07f * e->H);
    boss_id = id;
    /* asleep until enemy_boss_wake: crouched, dark, harmless and out of the scope's reach */
    e->state = S_DORMANT; e->power = 0; e->seen = false;
    boss_pose(e, 0);
    break;
  }
  for (int i = 0; i < e->nwp; i++) { e->wp[i].alive = true; e->wp[i].hp = type == EN_BOSS ? POD_HP[0] : 1e9f; }
  if (type == EN_BOSS && !e->a) boss_id = -1;
}

/* ── death and damage ── */
static void kill(en_t *e) {
  e->state = S_DEAD; e->dead_t = 0; e->melee_on = false;
  DBG_KILL((int)(e - E), e->type);
  score_acc += INFO[e->type].score;
  sfx(SFX_KILL, 0, 1);
  switch (e->type) {
  case EN_GRUNT: case EN_FLYER:
    /* the Tenants go home with a puff and leave a complaint form, nothing worse */
    fx_poof(chest(e->pos), PAL[lvl].fx, e->type == EN_GRUNT ? 1.5f : 1.0f);
    fx_form(chest(e->pos));
    actor_play(e->a, "Death", 1, true); break;
  case EN_TURRET: fx_explosion(v3_(e->pos.x, e->pos.y + 1.8f, e->pos.z), 2.2f); sfx(SFX_BOOM, 0, 0.8f); break;
  default:
    actor_play(e->a, "Death", 1, true); fx_explosion(chest(e->pos), e->H * 0.25f); sfx(SFX_BOOM, 0, 1);
    for (int i = 0; i < MAXE; i++) if (E[i].used && E[i].minion && E[i].state != S_DEAD) { E[i].hp = 0; kill(&E[i]); }
  }
  for (int i = 0; i < e->nwp; i++) e->wp[i].alive = false;
}

static void boss_phase_check(en_t *e) {
  float f = e->hp / e->maxhp;
  int ph = f > 0.66f ? 1 : f > 0.33f ? 2 : 3;
  if (ph > e->phase && e->state != S_DEAD) {
    e->phase = ph; e->state = S_STAGGER; e->aux2 = 0; e->atk = A_NONE; e->melee_on = false;
    for (int i = 0; i < e->nwp; i++) { e->wp[i].alive = true; e->wp[i].hp = POD_HP[ph - 1]; }
    sfx(SFX_ALARM, 0, 1); fx_explosion(en_local(e, v3_(0, 0.6f * e->H, 0.1f * e->H)), e->H * 0.12f);
    actor_play(e->a, "HitRecieve_2", 1, true);
  }
}

bool enemy_damage(int id, float amt, v3 at, bool weak) {
  if (id < 0 || id >= MAXE || !E[id].used || E[id].state == S_DEAD || E[id].state == S_DORMANT || E[id].state == S_RISE) return false;
  en_t *e = &E[id];
  bool metal = e->type >= EN_TURRET, heavy = amt >= HEAVY, armoured = false;
  v3 away = v3_norm(v3_(e->pos.x - tgt.x, 0, e->pos.z - tgt.z));
  switch (e->type) {
  case EN_TURRET:
    /* plating on the front deflects the chain gun: flank it, or scope the lens */
    if (weak) amt *= heavy ? 1 : 1.5f;
    else if (!heavy && v3_dot(fwd(e->aim), v3_scale(away, -1)) > 0.3f) { amt *= 0.45f; armoured = true; }
    break;
  case EN_BRUTE:
    if (weak && heavy) amt *= 1.7f;      /* the scope round is already doubled on a weak point */
    break;
  case EN_BOSS: {
    wp_t *hit = NULL; float bd = 1e9f;
    if (weak) for (int k = 0; k < e->nwp; k++) if (e->wp[k].alive) {
      float d = v3_len(v3_sub(at, e->wp[k].p));
      if (d < bd) { bd = d; hit = &e->wp[k]; }
    }
    if (hit) {
      if (!heavy) amt *= 0.25f;          /* the pods shrug off small arms: this is the scope's fight */
      hit->hp -= amt;
      if (hit->hp <= 0) {
        hit->alive = false; fx_explosion(hit->p, e->H * 0.05f); sfx(SFX_BOOM, 0, 0.8f); score_acc += 50;
        if (e->state != S_STAGGER) { e->state = S_STAGGER; e->aux2 = 0; e->atk = A_NONE; e->melee_on = false; actor_play(e->a, "HitRecieve_1", 1, true); }
      }
    } else { weak = false; amt *= 0.1f; }
    break; }
  }
  if (weak) score_acc += 50;
  e->hp -= amt; e->flash = FLASH_T;
  DBG_HIT(id);
  if (e->type != EN_BOSS) e->seen = true;
  sfx(weak && e->type == EN_BOSS ? SFX_BOSS_HIT : SFX_HIT, 0, 0.6f);
  if (armoured) {
    fx_sparks(at, v3_scale(away, -1), 0xdfe6f0, 3);
    if (e->ping_cd <= 0) { sfx(SFX_BOSS_HIT, 0, 0.18f); e->ping_cd = 0.25f; }
  } else if (metal) fx_sparks(at, v3_norm(v3_sub(at, chest(e->pos))), weak ? 0xffd36a : 0xffb347, weak ? 10 : 4);
  else fx_spores(at, PAL[lvl].fx, heavy ? 8 : 3);
  /* hit reactions: a lean away from the shot and, for the soft ones, a shove and a flinch */
  if (e->type == EN_GRUNT || e->type == EN_FLYER) {
    e->lean = fminf(0.4f, e->lean + (heavy ? 0.3f : 0.07f));
    float push = heavy ? 5.5f : 0.9f;
    if (e->type == EN_GRUNT) { e->kb = v3_add(e->kb, v3_scale(away, push)); float l = hypotf(e->kb.x, e->kb.z); if (l > 3.2f) e->kb = v3_scale(e->kb, 3.2f / l); }
    else e->vel = v3_add(e->vel, v3_scale(v3_norm(v3_sub(e->pos, at)), push * 0.6f));
    e->stag += amt;
    if (e->state != S_DEAD && e->hit_cd <= 0 && e->stag >= 22 && e->state != S_WIND) {
      e->stag = 0; e->hit_cd = 0.7f; e->st = 0.3f;
      actor_play(e->a, "HitReact", 1, true); e->state = e->type == EN_FLYER ? S_RECOVER : S_ALERT; e->atk = A_NONE; e->melee_on = false;
    }
  } else if (e->type == EN_BRUTE || e->type == EN_BOSS) e->lean = fminf(0.06f, e->lean + 0.015f);
  if (e->type == EN_BOSS) boss_phase_check(e);
  if (e->hp <= 0) { e->hp = 0; kill(e); return true; }
  return false;
}

void enemy_blast(v3 at, float radius, float amount) {
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (untargetable(e)) continue;
    v3 a, b; float r; body_cap(e, &a, &b, &r);
    v3 c = v3_(ds_clamp(at.x, a.x, a.x), ds_clamp(at.y, a.y, b.y), ds_clamp(at.z, a.z, a.z));
    float d = v3_len(v3_sub(c, at)) - r;
    if (d < radius) enemy_damage(i, amount * (1 - ds_clamp(d, 0, radius) / radius * 0.7f), c, false);
  }
}

int enemy_alive(void) { int n = 0; for (int i = 0; i < MAXE; i++) if (E[i].used && E[i].state != S_DEAD && !E[i].minion) n++; return n; }
int enemy_take_score(void) { int s = score_acc; score_acc = 0; return s; }
bool enemy_boss_active(void) { return boss_id >= 0 && E[boss_id].used && E[boss_id].seen && E[boss_id].state != S_DEAD; }
float enemy_boss_health(void) { return boss_id >= 0 && E[boss_id].used ? E[boss_id].hp / E[boss_id].maxhp : 0; }
int enemy_boss_phase(void) { return boss_id >= 0 && E[boss_id].used ? E[boss_id].phase : 0; }
bool enemy_boss_dead(void) { return boss_id >= 0 && E[boss_id].used && E[boss_id].state == S_DEAD; }

/* ── behaviour helpers ── */
static void turn_to(en_t *e, float want, float rate, float dt) {
  float d = ds_wrap_pi(want - e->yaw);
  e->yaw += ds_clamp(d, -rate * dt, rate * dt);
}
static float yaw_to(v3 from, v3 to) { return atan2f(to.x - from.x, to.z - from.z); }

/* walk on the world: dir is a unit xz direction, speed in m/s */
static void walk(en_t *e, v3 dir, float speed, float dt) {
  v3 want = v3_scale(dir, speed);
  /* a shot's shove, unless it would push the thing off the deck */
  float kl = hypotf(e->kb.x, e->kb.z);
  if (kl > 0.1f) {
    v3 ahead = v3_add(e->pos, v3_scale(e->kb, 0.6f));
    if (world_floor_y(v3_(ahead.x, e->pos.y + 1, ahead.z)) > e->pos.y - 1.6f) want = v3_add(want, v3_(e->kb.x, 0, e->kb.z));
    e->kb = v3_scale(e->kb, expf(-6 * dt));
  }
  if (speed > 0.1f) {
    v3 ahead = v3_add(e->pos, v3_scale(dir, 1.5f + e->r));
    if (world_floor_y(v3_(ahead.x, e->pos.y + 1, ahead.z)) < e->pos.y - 1.6f) want = v3_(0, 0, 0);
  }
  e->vel.x = ds_damp(e->vel.x, want.x, 10, dt); e->vel.z = ds_damp(e->vel.z, want.z, 10, dt);
  e->vel.y -= 24 * dt;
  world_move(&e->pos, &e->vel, e->r, e->h, dt, &e->grounded);
}

static void sense(en_t *e, float dt, float range, bool cone) {
  e->los_t -= dt;
  if (e->los_t > 0) return;
  e->los_t = 0.2f + (float)((e - E) % 5) * 0.02f;
  v3 eye = v3_(e->pos.x, e->pos.y + e->H * 0.8f, e->pos.z), to = chest(tgt);
  v3 d = v3_sub(to, eye);
  float L = v3_len(d);
  bool ok = L < range;
  if (ok && cone && L > 12) { v3 f = fwd(e->yaw); ok = v3_dot(v3_norm(v3_(d.x, 0, d.z)), f) > -0.3f || e->seen; }
  if (ok) { float t; if (world_raycast(eye, v3_scale(d, 1 / L), L, &t, NULL) && t < L - 0.6f) ok = false; }
  e->los = ok;
  if (ok) { e->last_seen = tgt; e->lost_t = 0; }
}

static void clip_by_speed(en_t *e, const char *idle, const char *walkc, const char *runc, float walk_ref, float run_ref) {
  float sp = hypotf(e->vel.x, e->vel.z);
  if (sp < 0.4f) actor_play(e->a, idle, 1, false);
  else if (sp < (walk_ref + run_ref) * 0.5f) actor_play(e->a, walkc, fmaxf(0.5f, sp / walk_ref), false);
  else actor_play(e->a, runc, fmaxf(0.6f, sp / run_ref), false);
}

/* a wind-up glow that grows to full over the telegraph time */
static void telegraph(v3 at, float size, float k, uint32_t col) { fx_glow(at, size * (0.3f + 0.9f * k) * (0.9f + 0.1f * sinf(gtime * 40)), col); }

/* ── grunt ── */
static void grunt_update(en_t *e, float dt) {
  float dist = flat_dist(e->pos, tgt);
  bool alive_hp_low = e->hp < e->maxhp * 0.35f;
  sense(e, dt, 40, true);
  e->cd -= dt; e->hit_cd -= dt; e->st -= dt;
  v3 to = v3_norm(v3_(tgt.x - e->pos.x, 0, tgt.z - e->pos.z));
  switch (e->state) {
  case S_IDLE: case S_PATROL: {
    if (e->los || (dist < 9 && e->seen)) { e->state = S_ALERT; e->st = 0.6f; e->seen = true; sfx(SFX_ALARM, 0, 0.35f); actor_play(e->a, "Idle", 1, false); break; }
    if (e->state == S_IDLE) {
      walk(e, v3_(0, 0, 0), 0, dt);
      if (e->cd <= 0) { e->state = S_PATROL; e->aux = ds_rr(0, 6.28f); e->aux2 = ds_rr(2, 6); e->st = ds_rr(2, 4); }
    } else {
      v3 goal = v3_(e->home.x + cosf(e->aux) * e->aux2, 0, e->home.z + sinf(e->aux) * e->aux2);
      v3 d = v3_(goal.x - e->pos.x, 0, goal.z - e->pos.z);
      if (v3_len(d) < 1 || e->st <= 0) { e->state = S_IDLE; e->cd = ds_rr(1, 3); break; }
      d = v3_norm(d); turn_to(e, yaw_to(e->pos, goal), 3, dt); walk(e, fwd(e->yaw), 1.8f, dt);
    }
    clip_by_speed(e, "Idle", "Walk", "Run", 1.8f, 5);
    break; }
  case S_ALERT:
    walk(e, v3_(0, 0, 0), 0, dt); turn_to(e, yaw_to(e->pos, tgt), 6, dt);
    if (e->st <= 0) { e->state = S_COMBAT; e->cd = ds_rr(0.4f, 1.0f); e->lost_t = 0; }
    break;
  case S_COMBAT: {
    turn_to(e, yaw_to(e->pos, tgt), 5, dt);
    e->strafe_t -= dt;
    if (e->strafe_t <= 0) { e->strafe = -e->strafe; e->strafe_t = ds_rr(1.2f, 2.8f); }
    float lo = alive_hp_low ? 20 : 11, hi = alive_hp_low ? 32 : 20;
    float radial = dist > hi ? 1 : dist < lo ? -1 : 0;
    v3 side = v3_(-to.z, 0, to.x);
    v3 dir = v3_add(v3_scale(to, radial * (alive_hp_low ? 1.3f : 1)), v3_scale(side, e->strafe * (radial ? 0.6f : 1)));
    float sp = alive_hp_low && dist < lo ? 5.2f : 3.6f;
    walk(e, v3_norm(dir), sp, dt);
    clip_by_speed(e, "Idle", "Walk", "Run", 1.8f, 5);
    if (!e->los) { e->lost_t += dt; if (e->lost_t > 3.5f) { e->state = S_PATROL; e->aux = 0; e->aux2 = 1; e->st = 6; e->home = e->last_seen; e->seen = false; } }
    if (dist < 2.8f && e->cd <= 0.3f) { e->state = S_WIND; e->atk = A_PUNCH; e->st = 0.35f; actor_play(e->a, "Punch", 1.3f, true); break; }
    if (e->los && e->cd <= 0 && dist < 40) { e->state = S_WIND; e->atk = A_SHOOT; e->st = 0.55f; actor_play(e->a, "Weapon", 1, true); }
    break; }
  case S_WIND: {
    walk(e, v3_(0, 0, 0), 0, dt);
    if (e->atk == A_SHOOT) {
      turn_to(e, yaw_to(e->pos, tgt), 4, dt);
      v3 mz = en_local(e, v3_(0.3f, 1.35f, 0.9f));
      telegraph(mz, 1.4f, 1 - e->st / 0.55f, C_GOO);
      if (e->st <= 0) {
        v3 dir = v3_sub(chest(tgt), mz);
        shoot(PJ_GOO, mz, dir, 15, 6);
        sfx(SFX_SHOT, 0, 0.4f); fx_muzzle(mz, dir);
        e->cd = alive_hp_low ? ds_rr(2.8f, 3.6f) : ds_rr(1.6f, 2.4f);
        e->state = S_COMBAT;
      }
    } else {
      if (e->st <= 0.1f) { e->melee_on = true; e->melee_done = false; e->mpos = en_local(e, v3_(0, 1.3f, 1.3f)); e->mr = 1.0f; e->mdmg = 12; }
      if (e->st <= -0.2f) { e->melee_on = false; e->state = S_COMBAT; e->cd = 1.2f; }
    }
    break; }
  case S_DEAD: break;
  }
  if (e->state == S_ALERT && e->hit_cd > 0 && e->st <= 0) e->state = S_COMBAT;
}

/* ── flyer ── */
static void flyer_update(en_t *e, float dt) {
  float dist = v3_len(v3_sub(chest(tgt), e->pos));
  sense(e, dt, 45, false);
  e->cd -= dt; e->hit_cd -= dt; e->st -= dt;
  v3 want = e->pos; float speed = 9;
  float fy = world_floor_y(v3_(e->pos.x, e->pos.y, e->pos.z));
  if (fy < -1e8f) fy = tgt.y;
  switch (e->state) {
  case S_IDLE: case S_PATROL:
    want = v3_(e->home.x + cosf(gtime * 0.5f + e->orbit) * 3, e->home.y + sinf(gtime * 1.3f + e->orbit) * 0.5f, e->home.z + sinf(gtime * 0.5f + e->orbit) * 3);
    speed = 2.5f;
    actor_play(e->a, "Flying_Idle", 1, false);
    if (e->los || (e->seen)) { e->state = S_COMBAT; e->seen = true; e->cd = ds_rr(1.5f, 3); sfx(SFX_ALARM, 0, 0.3f); }
    break;
  case S_COMBAT: {
    e->orbit += e->orbit_dir * (11.f / fmaxf(dist, 8)) * dt * 0.9f;
    float R = 12 + 3 * sinf(gtime * 0.9f + e->orbit * 2);
    want = v3_(tgt.x + cosf(e->orbit) * R, tgt.y + 5.5f + 1.5f * sinf(gtime * 1.7f), tgt.z + sinf(e->orbit) * R);
    speed = 10;
    actor_play(e->a, "Fast_Flying", 1, false);
    if (e->cd <= 0 && dist < 32 && e->los) {
      e->state = S_WIND; e->st = 0.5f; actor_play(e->a, "Headbutt", 0.6f, true);
      sfx(SFX_ZOOM, 0, 0.55f); sfx(SFX_JUMP, 0, 0.3f);   /* the dive whistle: two rising sweeps */
    }
    break; }
  case S_WIND:
    want = v3_add(e->pos, v3_scale(v3_norm(v3_sub(e->pos, chest(tgt))), 2)); speed = 4;
    telegraph(e->pos, 2.2f, 1 - e->st / 0.5f, 0xff5a30);
    if (e->st <= 0) { sfx(SFX_GLIDE, 0, 0.5f); e->state = S_DIVE; e->st = 1.4f; e->aim = 0; e->melee_done = false; e->vel = v3_scale(v3_norm(v3_sub(chest(tgt), e->pos)), 22); e->st = 1.4f; }
    break;
  case S_DIVE: {
    v3 d = v3_sub(chest(tgt), e->pos);
    v3 cur = v3_norm(e->vel);
    cur = v3_norm(v3_add(cur, v3_scale(v3_norm(d), 1.4f * dt)));
    e->vel = v3_scale(cur, 22);
    e->melee_on = true; e->mpos = e->pos; e->mr = 1.0f; e->mdmg = e->minion ? 6 : 10;
    fx_beam(e->pos, v3_sub(e->pos, v3_scale(cur, 3)), 0xd6ff9a, 0.4f);
    if (e->st <= 0 || (v3_dot(d, cur) < 0 && v3_len(d) > 2) || e->melee_done) { e->state = S_RECOVER; e->st = 1.3f; e->melee_on = false; e->cd = ds_rr(2.5f, 4.5f); e->orbit_dir = -e->orbit_dir; }
    break; }
  case S_RECOVER:
    want = v3_(e->pos.x + e->vel.x, tgt.y + 7, e->pos.z + e->vel.z); speed = 8;
    if (e->st <= 0) e->state = S_COMBAT;
    break;
  case S_ALERT: if (e->st <= 0) e->state = S_RECOVER, e->st = 0.5f; speed = 2; break;
  }
  if (e->state != S_DIVE) {
    v3 d = v3_sub(want, e->pos);
    float l = v3_len(d);
    v3 desired = l > 0.05f ? v3_scale(d, fminf(speed, l * 2.5f) / l) : v3_(0, 0, 0);
    e->vel = v3_(ds_damp(e->vel.x, desired.x, 4, dt), ds_damp(e->vel.y, desired.y, 4, dt), ds_damp(e->vel.z, desired.z, 4, dt));
  }
  float sl = v3_len(e->vel) * dt, t;
  if (sl > 1e-4f) {
    v3 dn = v3_scale(e->vel, dt / sl);
    if (world_raycast(e->pos, dn, sl + e->r, &t, NULL)) { e->vel = v3_scale(e->vel, 0.2f); if (e->state == S_DIVE) e->st = 0; }
    else e->pos = v3_add(e->pos, v3_scale(e->vel, dt));
  }
  if (fy > -1e8f && e->pos.y < fy + 1.2f) { e->pos.y = fy + 1.2f; if (e->vel.y < 0) e->vel.y = 0; }
  if (hypotf(e->vel.x, e->vel.z) > 0.5f) turn_to(e, atan2f(e->vel.x, e->vel.z), 8, dt);
  else if (dist < 40) turn_to(e, yaw_to(e->pos, tgt), 3, dt);
}

/* ── turret ── */
static void turret_update(en_t *e, float dt) {
  e->cd -= dt; e->st -= dt;
  v3 pivot = v3_(e->pos.x, e->pos.y + 1.75f, e->pos.z);
  v3 tc = chest(tgt);
  v3 d = v3_sub(tc, pivot);
  float dist = v3_len(d), want_yaw = atan2f(d.x, d.z), want_pitch = -asinf(ds_clamp(d.y / fmaxf(dist, 0.1f), -1, 1));
  bool in_range = dist < 45;
  bool vis = false;
  if (in_range) {
    e->los_t -= dt;
    if (e->los_t <= 0) { e->los_t = 0.12f; float t; e->los = !(world_raycast(pivot, v3_scale(d, 1 / dist), dist, &t, NULL) && t < dist - 0.6f); }
    vis = e->los;
  }
  float dyaw = fabsf(ds_wrap_pi(want_yaw - e->aim));
  float pitch = e->aux;
  switch (e->state) {
  case S_IDLE: case S_PATROL: {
    float sweep = e->yaw + sinf(gtime * 0.8f + e->orbit) * 1.1f;
    e->aim += ds_clamp(ds_wrap_pi(sweep - e->aim), -dt * 2, dt * 2);
    pitch = ds_damp(pitch, 0.15f, 3, dt);
    if (vis && e->cd <= 0 && dyaw < 0.14f && fabsf(want_pitch - pitch) < 0.3f) { e->state = S_LOCK; e->st = 0.7f; e->seen = true; sfx(SFX_ALARM, 0, 0.4f); }
    break; }
  case S_LOCK:
    e->aim += ds_clamp(ds_wrap_pi(want_yaw - e->aim), -dt * 1.8f, dt * 1.8f);
    pitch = ds_damp(pitch, want_pitch, 5, dt);
    if (!vis || dyaw > 0.5f) { e->state = S_IDLE; e->cd = 0.5f; }
    else if (e->st <= 0) { e->state = S_BURST; e->shots = 4; e->st = 0; }
    break;
  case S_BURST:
    e->aim += ds_clamp(ds_wrap_pi(want_yaw - e->aim), -dt * 1.2f, dt * 1.2f);
    pitch = ds_damp(pitch, want_pitch, 4, dt);
    if (e->st <= 0 && e->shots > 0) {
      v3 f = v3_(sinf(e->aim) * cosf(pitch), -sinf(pitch), cosf(e->aim) * cosf(pitch));
      v3 mz = v3_add(pivot, v3_scale(f, 1.9f));
      shoot(PJ_FAST, mz, f, 38, 4); fx_muzzle(mz, f); sfx(SFX_SHOT, 0, 0.3f);
      e->shots--; e->st = 0.13f;
    }
    if (e->shots <= 0 && e->st <= 0) { e->state = S_IDLE; e->cd = 2.0f; e->st = 0; }
    break;
  case S_DEAD: pitch = ds_damp(pitch, 0.7f, 2, dt); break;
  }
  e->aux = pitch;
  t3_object_set_position(e->tgroup, e->pos.x, e->pos.y, e->pos.z);
  t3_object_set_rotation(e->thead, pitch, e->aim, 0);
  v3 f = v3_(sinf(e->aim) * cosf(pitch), -sinf(pitch), cosf(e->aim) * cosf(pitch));
  if (e->nwp) {   /* the lens rides the barrel */
    wp_t *w = &e->wp[0];
    w->p = v3_add(v3_add(pivot, v3_scale(f, 0.78f)), v3_(0, 0.1f, 0));
    if (w->mesh) { ((t3_object *)w->mesh)->visible = w->alive; t3_object_set_position(w->mesh, w->p.x, w->p.y, w->p.z); w->mesh->materials[0]->color = (t3_color){ 1, 0.5f + 0.2f * sinf(gtime * 5), 0.1f }; w->mesh->materials[0]->version++; }
  }
  if (e->state != S_DEAD) {
    /* the sight line: a thin red scan while it sweeps, thicker and whiter as the lock closes, and a ring that
     * shrinks on whatever it is locking (the player's feet), so the 0.7 s of warning is visible from any angle */
    v3 mz = v3_add(pivot, v3_scale(f, 1.9f)), end;
    float t;
    if (world_raycast(mz, f, 50, &t, NULL)) end = v3_add(mz, v3_scale(f, t)); else end = v3_add(mz, v3_scale(f, 50));
    float lock = e->state == S_LOCK ? 1 - ds_clamp(e->st / 0.7f, 0, 1) : e->state == S_BURST ? 1.f : 0.f;
    bool cool = e->state == S_IDLE && e->cd > 0;
    float bw = cool ? 0.03f : 0.045f + lock * 0.075f;
    fx_beam(mz, end, cool ? 0x802018 : lock > 0 ? 0xff5a40 : 0xff3a28, bw);
    if (lock > 0) {
      fx_beam(mz, end, 0xffe0d8, bw * 0.35f);
      fx_glow(end, 0.6f + lock * 0.9f, 0xff5a40);
      if (flat_dist(end, tgt) < 4 && e->state == S_LOCK && ((int)(gtime * (6 + lock * 14)) & 1)) fx_ring_flat(v3_(tgt.x, tgt.y + 0.1f, tgt.z), 0.5f + 1.7f * (1 - lock), 0xff4a38);
    }
    fx_glow(mz, 0.4f + lock * 0.6f, 0xff3a28);
  }
}

/* ── mechs: brute and the Superintendent ── */
static void mech_pods(en_t *e) {
  for (int i = 0; i < e->nwp; i++) {
    wp_t *w = &e->wp[i];
    w->p = en_local(e, w->off);
    if (!w->mesh) continue;
    ((t3_object *)w->mesh)->visible = w->alive;
    if (!w->alive) continue;
    t3_object_set_position(w->mesh, w->p.x, w->p.y, w->p.z);
    float pulse = 0.6f + 0.4f * sinf(gtime * 6 + i), lit = e->power;   /* a sleeping boss has dim pods */
    t3_material *m = w->mesh->materials[0];
    m->color = (t3_color){ ds_lerp(0.28f, 1.0f, lit), ds_lerp(0.05f, 0.35f + 0.3f * pulse, lit), ds_lerp(0.04f, 0.05f, lit) }; m->version++;
    if (lit > 0.1f) fx_glow(w->p, w->r * 5.5f * (0.8f + 0.3f * pulse) * lit, 0xff8a2a);
  }
}

static void brute_update(en_t *e, float dt) {
  float dist = flat_dist(e->pos, tgt);
  sense(e, dt, 50, false);
  e->cd -= dt; e->st -= dt;
  switch (e->state) {
  case S_IDLE: case S_PATROL:
    walk(e, v3_(0, 0, 0), 0, dt); actor_play(e->a, "Idle", 1, false);
    if (e->los || e->seen) { e->state = S_COMBAT; e->seen = true; e->cd = 1.2f; sfx(SFX_ALARM, 0, 0.6f); }
    break;
  case S_COMBAT:
    turn_to(e, yaw_to(e->pos, tgt), 0.8f, dt);
    walk(e, fwd(e->yaw), dist > 18 ? 3.2f : dist < 8 ? -1.5f : 0, dt);
    clip_by_speed(e, "Idle", "Walk", "Run", 3, 8);
    if (e->cd <= 0 && e->los) {
      if (dist < 9) { e->state = S_WIND; e->atk = A_STOMP; e->st = 0.7f; actor_play(e->a, "Kick", 1, true); }
      else { e->state = S_WIND; e->atk = A_SHOOT; e->st = 0.65f; actor_play(e->a, "Shoot_Big", 1, true); }
    }
    break;
  case S_WIND: {
    walk(e, v3_(0, 0, 0), 0, dt);
    if (e->atk == A_SHOOT) {
      turn_to(e, yaw_to(e->pos, tgt), 0.5f, dt);
      v3 mz = en_local(e, v3_(0.35f * e->H, 0.5f * e->H, 0.45f * e->H));
      telegraph(mz, 2.4f, 1 - e->st / 0.65f, 0xff5a30);
      if (e->st <= 0) {
        shoot(PJ_BIG, mz, v3_sub(chest(tgt), mz), 20, 24);
        fx_muzzle(mz, v3_sub(chest(tgt), mz)); sfx(SFX_BOOM, 0, 0.4f);
        e->cd = 2.6f; e->state = S_RECOVER; e->st = 0.6f;
      }
    } else {
      float k = 1 - e->st / 0.7f;
      fx_ring_flat(v3_(e->pos.x, e->pos.y + 0.1f, e->pos.z), 12, 0xff3a28);
      telegraph(v3_(e->pos.x, e->pos.y + 0.3f, e->pos.z), 4, k, 0xff5a30);
      if (e->st <= 0) {
        wave(v3_(e->pos.x, e->pos.y, e->pos.z), 13, 13, 20, 0);
        fx_explosion(v3_(e->pos.x, e->pos.y + 0.2f, e->pos.z), 1.0f); sfx(SFX_BOOM, 0, 0.9f);
        e->cd = 3.2f; e->state = S_RECOVER; e->st = 0.9f;
      }
    }
    break; }
  case S_RECOVER:
    walk(e, v3_(0, 0, 0), 0, dt); turn_to(e, yaw_to(e->pos, tgt), 0.4f, dt);
    if (e->st <= 0) e->state = S_COMBAT;
    break;
  case S_DEAD: break;
  }
  mech_pods(e);
}

#define WAKE_T 2.0f
static void boss_rise(en_t *e, float dt) {
  float t0 = e->wake_t;
  e->wake_t += dt;
  float k = ds_clamp(e->wake_t / WAKE_T, 0, 1), ease = k * k * (3 - 2 * k), fy = e->pos.y;
  boss_pose(e, ease);
  if (t0 <= 0) { sfx(SFX_ALARM, 0, 1); sfx(SFX_BOOM, 0, 0.5f); }
  /* a ground ring and dust all round the feet, sparks from the joints, a thump or two on the way up */
  fx_ring_flat(v3_(e->pos.x, fy + 0.1f, e->pos.z), 6 + e->wake_t * 12, 0xff8a40);
  fx_ring_flat(v3_(e->pos.x, fy + 0.1f, e->pos.z), 3 + e->wake_t * 9, 0xffd8a0);
  if ((int)(e->wake_t * 7) != (int)(t0 * 7)) {
    float a = ds_rr(0, 6.28f), r = ds_rr(2, 7);
    fx_poof(v3_(e->pos.x + cosf(a) * r, fy + 0.8f, e->pos.z + sinf(a) * r), 0x9a9aa4, 3.2f);
    fx_sparks(en_local(e, v3_(ds_rr(-0.25f, 0.25f) * e->H, ds_rr(0.3f, 0.75f) * e->H, ds_rr(-0.1f, 0.2f) * e->H)), v3_(0, 1, 0), 0xffb347, 5);
  }
  if ((int)(e->wake_t * 1.4f) != (int)(t0 * 1.4f)) sfx(SFX_BOOM, 0, 0.35f);
  if (e->wake_t >= WAKE_T) {
    e->state = S_COMBAT; e->cd = 1.8f; e->power = 1; e->tinted = -1;
    actor_play(e->a, "Idle", 1, true);
    fx_explosion(v3_(e->pos.x, fy + 0.3f, e->pos.z), 2.2f); sfx(SFX_BOOM, 0, 1);
  }
}

static void boss_update(en_t *e, float dt) {
  if (e->state == S_DORMANT) { mech_pods(e); return; }
  if (e->state == S_RISE) { boss_rise(e, dt); mech_pods(e); return; }
  float dist = flat_dist(e->pos, tgt);
  float rage = e->phase == 3 ? 1.35f : 1;
  sense(e, dt, 120, false);
  e->cd -= dt; e->st -= dt;
  v3 to = v3_norm(v3_(tgt.x - e->pos.x, 0, tgt.z - e->pos.z));
  v3 side = v3_(-to.z, 0, to.x);
  float bs = e->H;
  switch (e->state) {
  case S_IDLE: case S_PATROL:
    walk(e, v3_(0, 0, 0), 0, dt); actor_play(e->a, "Idle", 1, false);
    if (e->seen) { e->state = S_COMBAT; e->cd = 1.5f; }
    break;
  case S_COMBAT: {
    turn_to(e, yaw_to(e->pos, tgt), 0.9f * rage, dt);
    e->strafe_t -= dt;
    if (e->strafe_t <= 0) { e->strafe = -e->strafe; e->strafe_t = ds_rr(3, 6); }
    float lo = 18, hi = 30;   /* close enough that the stomp ring reaches a scoper who stands still */
    v3 dir = v3_add(v3_scale(to, dist > hi ? 1 : dist < lo ? -1 : 0), v3_scale(side, 0.5f * e->strafe));
    float home_d = flat_dist(e->pos, e->home);
    if (home_d > 26) dir = v3_add(dir, v3_scale(v3_norm(v3_(e->home.x - e->pos.x, 0, e->home.z - e->pos.z)), 1.5f));
    walk(e, v3_norm(dir), 3.4f * rage, dt);
    clip_by_speed(e, "Idle", "Walk", "Run", 3.4f, 9);
    if (e->cd <= 0) {
      int pick = (int)(ds_rand() * (e->phase == 1 ? 3 : 4));
      int minions = 0; for (int i = 0; i < MAXE; i++) if (E[i].used && E[i].minion && E[i].state != S_DEAD) minions++;
      if (pick == 0 && dist < 26) pick = 1;
      if (e->phase == 1) e->atk = pick == 1 || dist < 18 ? A_STOMP : pick == 2 ? A_BOMBS : A_SWEEP;
      else e->atk = pick == 0 ? A_SWEEP : pick == 1 ? A_STOMP : pick == 2 ? (minions < 2 ? A_FLYERS : A_BOMBS) : A_BOMBS;
      e->state = S_WIND; e->st = 0.6f; e->shots = 0;
      actor_play(e->a, e->atk == A_STOMP ? "Kick" : "Shoot_Big", 1, true);
      sfx(SFX_ALARM, 0, 0.5f);
    }
    break; }
  case S_WIND: {
    walk(e, v3_(0, 0, 0), 0, dt);
    float k = 1 - ds_clamp(e->st / 0.6f, 0, 1);
    if (e->atk != A_STOMP) turn_to(e, yaw_to(e->pos, tgt), 0.6f, dt);
    v3 mz = en_local(e, v3_(0, 0.6f * bs, 0.25f * bs));
    if (e->atk == A_STOMP) {
      fx_ring_flat(v3_(e->pos.x, e->pos.y + 0.1f, e->pos.z), 24, 0xff3a28);
      telegraph(v3_(e->pos.x, e->pos.y + 1, e->pos.z), 12, k, 0xff5a30);
    } else telegraph(mz, bs * 0.45f, k, e->atk == A_BOMBS ? 0xff9a30 : e->atk == A_FLYERS ? 0x9be35a : 0xff4a30);
    if (e->st <= 0) {
      e->state = S_ACT; e->shots = 0; e->aux = 0;
      e->aim = yaw_to(e->pos, tgt);
      if (e->atk == A_STOMP) {
        wave(v3_(e->pos.x, e->pos.y, e->pos.z), 16 * rage, 26, 16, 0);
        if (e->phase == 3) wave(v3_(e->pos.x, e->pos.y, e->pos.z), 16 * rage, 26, 16, 0.55f);
        fx_explosion(v3_(e->pos.x, e->pos.y + 0.3f, e->pos.z), 3); sfx(SFX_BOOM, 0, 1);
        e->st = 0.8f;
      } else if (e->atk == A_FLYERS) {
        for (int i = 0; i < 1 + (e->phase == 3); i++) {
          v3 sp = en_local(e, v3_((i - 0.5f) * 0.5f * bs, 0.75f * bs, 0.1f * bs));
          int before = -1; for (int k2 = 0; k2 < MAXE; k2++) if (!E[k2].used) { before = k2; break; }
          if (before >= 0) { enemy_spawn(EN_FLYER, sp, e->yaw); E[before].minion = true; E[before].seen = true; E[before].state = S_COMBAT; E[before].home = sp; fx_goo(sp, 6); }
        }
        e->st = 1.0f;
      } else if (e->atk == A_BOMBS) {
        e->shots = e->phase == 3 ? 5 : 3; e->st = 0;
      } else { e->shots = e->phase == 3 ? 9 : 7; e->st = 0; }
    }
    break; }
  case S_ACT: {
    walk(e, v3_(0, 0, 0), 0, dt);
    v3 mz = en_local(e, v3_(0, 0.6f * bs, 0.25f * bs));
    if (e->atk == A_SWEEP) {
      if (e->st <= 0 && e->shots > 0) {
        int n = e->phase == 3 ? 9 : 7, i = n - e->shots;
        float a = e->aim + (i - (n - 1) * 0.5f) * 0.09f;
        v3 dir = v3_(sinf(a), (chest(tgt).y - mz.y) / fmaxf(dist, 5), cosf(a));
        shoot(PJ_GOO, mz, dir, 17 * rage, 8); fx_muzzle(mz, dir); sfx(SFX_SHOT, 0, 0.5f);
        e->shots--; e->st = 0.12f / rage;
      }
      if (e->shots <= 0) { e->state = S_RECOVER; e->st = 0.8f; }
    } else if (e->atk == A_BOMBS) {
      if (e->st <= 0 && e->shots > 0) {
        v3 lp = v3_(tgt.x + ds_rr(-4, 4), tgt.y, tgt.z + ds_rr(-4, 4));
        if (e->shots == 3 || e->shots == 5) lp = tgt;
        lob(v3_add(mz, v3_(0, bs * 0.2f, 0)), lp, 1.7f);
        sfx(SFX_BOOM, 0, 0.3f);
        e->shots--; e->st = 0.35f / rage;
      }
      if (e->shots <= 0) { e->state = S_RECOVER; e->st = 0.8f; }
    } else if (e->st <= 0) { e->state = S_RECOVER; e->st = 0.6f; }
    break; }
  case S_RECOVER:
    walk(e, v3_(0, 0, 0), 0, dt); turn_to(e, yaw_to(e->pos, tgt), 0.5f, dt);
    if (e->st <= 0) { e->state = S_COMBAT; e->cd = (e->phase == 1 ? 2.4f : e->phase == 2 ? 1.9f : 1.2f) * ds_rr(0.8f, 1.2f); }
    break;
  case S_STAGGER:
    walk(e, v3_(0, 0, 0), 0, dt);
    e->aux2 += dt;
    if (e->aux2 > 1.6f) { e->state = S_COMBAT; e->cd = 1.2f; }
    break;
  case S_DEAD:
    e->dead_t += 0; break;
  }
  mech_pods(e);
}

/* ── the frame ── */
void enemy_update(float dt, v3 player, bool decoy_active, v3 decoy) {
  gtime += dt;
  tgt_decoy = decoy_active; tgt = decoy_active ? decoy : player;
  for (int i = 0; i < MAXE; i++) {
    en_t *e = &E[i];
    if (!e->used) continue;
    float pd = flat_dist(e->pos, player);
    if (e->pending) { if (pd > STREAM_R) continue; attach_model(e); }
    if (e->type != EN_BOSS && !e->minion && !e->seen && e->state != S_DEAD && pd > SLEEP_R) {   /* far and unalerted: asleep */
      if (e->a) e->a->root->visible = false;
      if (e->tgroup) e->tgroup->visible = false;
      for (int k = 0; k < e->nwp; k++) if (e->wp[k].mesh) ((t3_object *)e->wp[k].mesh)->visible = false;
      continue;
    }
    if (e->a) e->a->root->visible = true;
    if (e->tgroup) e->tgroup->visible = true;
    e->flash = fmaxf(0, e->flash - dt); e->ping_cd -= dt;
    e->lean = ds_damp(e->lean, 0, 12, dt);
    bool dead = e->state == S_DEAD;
    if (dead) {
      e->dead_t += dt;
      if (e->type == EN_BOSS && e->dead_t < 3.5f && ((int)(e->dead_t * 6) != (int)((e->dead_t - dt) * 6)))
        fx_explosion(en_local(e, v3_(ds_rr(-0.2f, 0.2f) * e->H, ds_rr(0.1f, 0.8f) * e->H, ds_rr(-0.2f, 0.2f) * e->H)), e->H * 0.1f), sfx(SFX_BOOM, 0, 0.6f);
      if (e->type == EN_FLYER) { e->vel.y -= 20 * dt; e->pos = v3_add(e->pos, v3_scale(e->vel, dt)); float fy = world_floor_y(e->pos); if (e->pos.y < fy + 0.4f) { e->pos.y = fy + 0.4f; e->vel = v3_(0, 0, 0); } }
      if (e->type == EN_TURRET) { turret_update(e, dt); if (e->dead_t < 1.5f && ((int)(e->dead_t * 8) != (int)((e->dead_t - dt) * 8))) fx_sparks(v3_(e->pos.x, e->pos.y + 1.8f, e->pos.z), v3_(0, 1, 0), 0xffb347, 3); }
      for (int k = 0; k < e->nwp; k++) if (e->wp[k].mesh) ((t3_object *)e->wp[k].mesh)->visible = false;
      if (e->dead_t > (e->type == EN_BOSS ? 6.f : 2.f)) { if (i == boss_id) { /* keep the boss's dead flag readable */ e->dead_t = 6; if (e->a) { actor_put(e->a); e->a = NULL; } } else en_free(e); continue; }
    } else {
      cur_src = e->type;
      switch (e->type) {
      case EN_GRUNT: grunt_update(e, dt); break;
      case EN_FLYER: flyer_update(e, dt); break;
      case EN_TURRET: turret_update(e, dt); break;
      case EN_BRUTE: brute_update(e, dt); break;
      case EN_BOSS: boss_update(e, dt); break;
      }
    }
    if (e->a) {
      float glow = 0;
      if (e->type == EN_BOSS && e->phase == 3 && !dead) glow = 0.5f + 0.3f * sinf(gtime * 8);
      if (e->state == S_WIND) glow += 0.6f * (1 - ds_clamp(e->st, 0, 0.6f) / 0.6f);
      apply_flash(e, glow);
      actor_update(e->a, dt, dead);
      float pitch = -e->lean * 0.6f;   /* rocks back from the shot */
      if (e->type == EN_FLYER && !dead) pitch += ds_clamp(-e->vel.y * 0.04f, -0.6f, 0.6f);
      /* the Tenants fold up and vanish once they have gone home */
      if (dead && (e->type == EN_GRUNT || e->type == EN_FLYER)) {
        float sc = ds_clamp((2.0f - e->dead_t) / 0.5f, 0.01f, 1);
        t3_object_set_scale(e->a->model, sc, sc, sc);
      }
      t3_object_set_position(e->a->root, e->pos.x, e->pos.y - (e->type == EN_FLYER ? 0.8f : 0), e->pos.z);
      t3_object_set_rotation(e->a->root, pitch, e->yaw, 0);
    }
  }
  proj_update(dt);
}

void enemy_boss_wake(void) {
  if (boss_id < 0 || !E[boss_id].used || E[boss_id].state != S_DORMANT) return;
  en_t *e = &E[boss_id];
  e->state = S_RISE; e->wake_t = 0; e->seen = true;
}

int enemy_alive_near(v3 c, float r, int type) {
  int n = 0;
  for (int i = 0; i < MAXE; i++) {
    const en_t *e = &E[i];
    if (!e->used || e->minion || e->state == S_DEAD || (type >= 0 && e->type != type)) continue;
    if (v3_len(v3_sub(e->pos, c)) < r) n++;
  }
  return n;
}
#ifdef DS_BOT
#include "enemy_dbg.inc"
#endif
