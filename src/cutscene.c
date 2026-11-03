/* cutscene.c: the director. A cutscene is a list of lines (the script, typed
 * out as captions) and a list of shots whose cuts are keyed to those lines;
 * each cutscene also has an act function that poses Juno and the staged cast
 * from the clock. The sets and props are in cine_set.c, the radio script and
 * the loading-card quips are at the bottom. Everything here is original to
 * this game (docs/STORY.md). */
#include "cutscene.h"
#include "cine_int.h"
#include "audio.h"
#include "world.h"
#include "fx.h"
#include "voice.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int prep_i;   /* the next caption line to synthesise ahead of time */

#define MAXL 24
#define MAXS 28
#define TYPE_CPS 45.f            /* captions type out at about 45 characters per second */
#define SKIP_HOLD 0.8f

typedef struct { const char *who, *text; float pre, t0, dur; int n; } line_t;
/* a shot starts at line `line` plus `off` (line < 0: `off` from the start). Its camera runs
 * p0,t0,f0 to p1,t1,f1 until the next shot starts; coordinates are in the cutscene's frame */
typedef struct { int line; float off, s0; v3 p0, p1, a0, a1; float f0, f1, shake; int ease; bool follow; } shot_t;

typedef void (*act_fn)(float t, float dt, cine_view_t *o);

static struct {
  bool active, fade_io, armed;
  cut_id id;
  int level;
  float t, end, skip, bars, tail, fi, fo;
  line_t L[MAXL]; int nl;
  shot_t S[MAXS]; int ns;
  v3 org; float yaw;
  act_fn act;
  v3 (*follow)(float t);
  bool ray;
  int last_chars;
} C;

static t3_scene *scn;
static t3_object *pool;
static t3_object *juno_root;
static act_t TF[6], TS[3], TL[4], MA, MB, MC[3];      /* flyers, small flyers, grunts, the Brute, the Superintendent, more Brutes */
static int last_line;
static v3 player_pos; static bool have_player;

/* ── what a cutscene changes and must hand back ── */
typedef struct { t3_object *o; bool vis; } saved_t;
static saved_t sv[256]; static int nsv;
static t3_fog sv_fog; static t3_color sv_bg; static bool sv_has_bg;
static t3_light *lt_hemi, *lt_sun;
static t3_color sv_hc, sv_hg, sv_sc; static float sv_hi, sv_si;
static t3_object *pistol;

/* a one-shot sound at a time in the cutscene */
static bool cued[12];
static void cue(int i, float t, float at, sfx_t s, float gain) { if (!cued[i] && t >= at) { cued[i] = true; sfx(s, 0, gain); } }

static float sm(float x) { x = ds_clamp(x, 0, 1); return x * x * (3 - 2 * x); }
static float seg(float t, float a, float b) { return ds_clamp((t - a) / (b - a), 0, 1); }
static v3 W(v3 p) {   /* the cutscene's frame to the world */
  float c = cosf(C.yaw), s = sinf(C.yaw);
  return v3_(C.org.x + p.x * c + p.z * s, C.org.y + p.y, C.org.z - p.x * s + p.z * c);
}
static float WY(float y) { return C.yaw + y; }

/* ── script building ── */
static void cut_reset(void) {
  C.ray = false; C.nl = C.ns = 0; C.tail = 2; C.fade_io = true; C.fi = 1.2f; C.fo = 1.15f;
  C.org = v3_(0, 0, 0); C.yaw = 0;
}
static int say(const char *who, const char *text, float pre) {
  line_t *l = &C.L[C.nl];
  l->who = who; l->text = text; l->pre = pre; l->n = (int)strlen(text);
  return C.nl++;
}
static void shot(int line, float off, v3 p0, v3 a0, float f0, v3 p1, v3 a1, float f1, float shake, int ease) {
  shot_t *s = &C.S[C.ns++];
  s->follow = false; s->line = line; s->off = off; s->p0 = p0; s->a0 = a0; s->f0 = f0; s->p1 = p1; s->a1 = a1; s->f1 = f1; s->shake = shake; s->ease = ease;
}
/* the shot just added tracks the cutscene's follow point; its target values become offsets from it */
static void follow_last(void) { C.S[C.ns - 1].follow = true; }
/* a locked-off or slowly pushing shot with one end set */
static void hold(int line, float off, v3 p, v3 a, float f, float shake) { shot(line, off, p, a, f, p, a, f, shake, 1); }
static float line_t0(int i) { return C.L[i].t0; }
static void finish_lines(void) {
  float t = 0;
  for (int i = 0; i < C.nl; i++) {
    line_t *l = &C.L[i];
    t += l->pre; l->t0 = t;
    l->dur = (float)l->n / TYPE_CPS + 1.0f + 0.03f * (float)l->n;   /* typing, then time to read it */
    t += l->dur;
  }
  C.end = t + C.tail;
}
static void resolve_shots(void) {
  for (int i = 0; i < C.ns; i++) C.S[i].s0 = C.S[i].line >= 0 ? C.L[C.S[i].line].t0 + C.S[i].off : C.S[i].off;
}

/* ── the cast ── */
static void cast_hide_all(void) {
  for (int i = 0; i < 6; i++) TF[i].root->visible = false;
  for (int i = 0; i < 3; i++) TS[i].root->visible = false;
  for (int i = 0; i < 4; i++) TL[i].root->visible = false;
  MA.root->visible = false; MB.root->visible = false;
  for (int i = 0; i < 3; i++) MC[i].root->visible = false;
}
static void cast_tick(float dt) {
  for (int i = 0; i < 6; i++) act_tick(&TF[i], dt);
  for (int i = 0; i < 3; i++) act_tick(&TS[i], dt);
  for (int i = 0; i < 4; i++) act_tick(&TL[i], dt);
  act_tick(&MA, dt); act_tick(&MB, dt);
  for (int i = 0; i < 3; i++) act_tick(&MC[i], dt);
}

static bool is_mine(t3_object *o) { return o == pool || o == HA.root || o == SU.root || o == FR.root || o == CO.root || o == AN.root || o == BS.root; }
static void skin_cb(t3_object *o, void *ctx) { if (o->type == T3_SKINNED_MESH) *(bool *)ctx = true; }
static bool has_skin(t3_object *o) { bool f = false; t3_object_traverse(o, skin_cb, &f); return f; }
/* all: hide the whole level (the cutscene has its own set); else only the actors, so the real arena stays */
static void hide_world(bool all) {
  nsv = 0;
  for (int i = 0; i < scn->base.child_count && nsv < 256; i++) {
    t3_object *c = scn->base.children[i];
    if (is_mine(c) || c->type == T3_HEMISPHERE_LIGHT || c->type == T3_DIRECTIONAL_LIGHT || c->type == T3_AMBIENT_LIGHT || c == juno_root) continue;
    if (all || has_skin(c)) { sv[nsv].o = c; sv[nsv].vis = c->visible; nsv++; c->visible = false; }
  }
}
static void restore_world(void) {
  for (int i = 0; i < nsv; i++) sv[i].o->visible = sv[i].vis;
  nsv = 0;
}
static void light_cb(t3_object *o, void *ctx) {
  (void)ctx;
  if (o->type == T3_HEMISPHERE_LIGHT && !lt_hemi) lt_hemi = (t3_light *)o;
  if (o->type == T3_DIRECTIONAL_LIGHT && !lt_sun) lt_sun = (t3_light *)o;
}
static void look_save(void) {
  sv_fog = scn->fog; sv_bg = scn->background; sv_has_bg = scn->has_background;
  if (lt_hemi) { sv_hc = lt_hemi->color; sv_hg = lt_hemi->ground_color; sv_hi = lt_hemi->intensity; }
  if (lt_sun) { sv_sc = lt_sun->color; sv_si = lt_sun->intensity; }
}
static void look_restore(void) {
  scn->fog = sv_fog; scn->background = sv_bg; scn->has_background = sv_has_bg;
  if (lt_hemi) { lt_hemi->color = sv_hc; lt_hemi->ground_color = sv_hg; lt_hemi->intensity = sv_hi; }
  if (lt_sun) { lt_sun->color = sv_sc; lt_sun->intensity = sv_si; }
}
static void look(uint32_t fogc, float dens, uint32_t hc, uint32_t hg, float hi, uint32_t sc, float si) {
  scn->fog = t3_fog_exp2(fogc, dens);
  scn->background = t3_color_hex(fogc); scn->has_background = true;
  if (lt_hemi) { lt_hemi->color = t3_color_hex(hc); lt_hemi->ground_color = t3_color_hex(hg); lt_hemi->intensity = hi; }
  if (lt_sun) { lt_sun->color = t3_color_hex(sc); lt_sun->intensity = si; }
}

/* ═════════════ PROLOGUE: morning on Halcyon Reach ═════════════ */
static const v3 HO = {0, 3000, 0};
static v3 pv(float x, float y, float z) { return v3_(x, y, z); }   /* a point in the set's frame */

/* the stolen window's flight: from tower 8, out, up and toward the ships */
static v3 panel_pos(float u) {   /* u seconds since the Tenants took hold */
  float a = sm(u / 3.0f), b = sm((u - 2.5f) / 9.0f);
  v3 p = v3_(76 + sinf(u * 1.1f) * 0.25f, 4 + a * 1.5f + b * 45, 0.12f + a * 11 + b * 180);
  p.x += b * 60;
  return p;
}
static float panel_u(float t) { return t - (line_t0(5) - 1.0f); }

static void prologue_build(void) {
  C.org = HO; C.yaw = 0;
  say("MARGUERITE", "Halcyon Reach Facilities, morning dispatch. Tower nine, east face. Status?", 2.8f);   /* 0 */
  say("JUNO", "Streaky.", 0.5f);                                                                         /* 1 */
  say("MARGUERITE", "Logging it as streaky. Take your time. The tower isn't going anywhere.", 0.4f);   /* 2 */
  say("MARGUERITE", "Juno. Three ships have come up over the limb. They are not on the schedule.", 2.2f);  /* 3 */
  say("JUNO", "Not ours.", 0.5f);                                                                        /* 4 */
  say("MARGUERITE", "Not anyone's. And now there are tenants, taking a window out of tower eight. Politely.", 0.4f);  /* 5 */
  say("JUNO", "That's a good window.", 3.0f);                                                            /* 6 */
  say("MARGUERITE", "Security has declined the work order. Their contractor says it's out of scope.", 0.4f);   /* 7 */
  say("MARGUERITE", "You have the only suit, the only scope and the only ladder on the ring.", 0.3f);  /* 8 */
  say("JUNO", "It's one window.", 0.6f);                                                                 /* 9 */
  say("MARGUERITE", "They are windows, Juno.", 0.5f);                                                    /* 10 */
  say("JUNO", "...Fine.", 1.0f);                                                                         /* 11 */
  C.tail = 4.6f;
  finish_lines();
  float tf = C.L[11].t0 + C.L[11].dur - 0.3f;   /* when she steps off */
  /* the establishing crane down the tower face */
  shot(-1, 0, pv(9, 46, 26), pv(0, 40, 0), 52, pv(4.6, 3.0, 9), pv(-0.6, 1.8, 1.4), 42, 0.05f, 1);
  shot(0, 0.8f, pv(3.4, 1.9, 4.6), pv(-0.8, 1.6, 1.4), 42, pv(2.6, 1.8, 3.6), pv(-0.8, 1.6, 1.4), 38, 0.04f, 0);
  shot(1, 0, pv(1.6, 1.7, 1.9), pv(-1.0, 1.65, 1.5), 30, pv(1.3, 1.68, 1.8), pv(-1.0, 1.65, 1.5), 26, 0.03f, 0);
  shot(2, 0, pv(-2.9, 2.1, 3.5), pv(-0.6, 2.5, 0.2), 40, pv(-2.2, 2.0, 3.0), pv(-0.5, 2.6, 0.2), 36, 0.04f, 0);
  shot(3, -1.6f, pv(0.6, 1.7, 0.5), pv(-1.5, 1.9, 2.6), 42, pv(0.4, 1.7, 0.55), pv(-1.6, 1.95, 2.8), 38, 0.03f, 0);
  shot(4, 0, pv(-2.5, 1.9, 4.0), pv(6, 10, 120), 46, pv(-2.3, 1.9, 3.9), pv(6, 12, 120), 44, 0.05f, 0);
  shot(5, 0, pv(62, 3, 16), pv(0, 0, 0), 38, pv(70, 9, 34), pv(0, 1, 0), 36, 0.05f, 1); follow_last();
  shot(6, 0, pv(0.9, 1.75, 0.7), pv(-1.0, 1.7, 1.5), 30, pv(0.8, 1.74, 0.75), pv(-1.0, 1.7, 1.5), 27, 0.03f, 0);
  shot(7, 0, pv(-4, 1.8, 4), pv(0, 0, 0), 34, pv(-3, 2, 3.5), pv(0, 2, 0), 30, 0.05f, 1); follow_last();
  shot(9, 0, pv(1.8, 2.1, 0.7), pv(-1.0, 1.75, 1.5), 28, pv(1.5, 2.0, 0.75), pv(-1.0, 1.75, 1.5), 25, 0.03f, 0);
  shot(10, 0, pv(-1.0, 1.9, 4.8), pv(-1.0, 1.9, 60), 38, pv(-1.0, 1.85, 3.5), pv(-1.0, 1.9, 60), 34, 0.03f, 0);
  shot(11, 0, pv(0.4, 1.78, 0.62), pv(-1.0, 1.72, 1.5), 26, pv(0.2, 1.76, 0.66), pv(-1.0, 1.72, 1.5), 23, 0.02f, 0);
  shot(-1, tf, pv(2.6, 2.3, 6.5), pv(-1.0, 1.0, 2.0), 40, pv(4.5, -2.0, 11.0), pv(0, 0, 0), 46, 0.04f, 1); follow_last();
  resolve_shots();
}

/* the step off: she walks to the rail, goes over it and falls toward the planet */
static v3 juno_fall(float w) {
  if (w < 1.0f) return pv(-1.0f, 0, ds_lerp(1.5f, 2.35f, sm(w)));
  float f = w - 1.0f;
  return pv(-1.0f, 1.0f + 2.2f * f - 6.0f * f * f, 2.35f + 2.4f * f);
}
static v3 prologue_follow(float t) {
  float tf = C.L[11].t0 + C.L[11].dur - 0.3f;
  return t >= tf ? juno_fall(t - tf) : panel_pos(panel_u(t));
}
/* the squeegee rides in her right hand (the bone's world position, a frame behind) */
static void squeegee_follow(bool on) {
  HA.squeegee->visible = on;
  t3_object *hand = juno_root ? t3_object_get_by_name(juno_root, "Middle1.R") : NULL;
  if (!on || !hand) return;
  t3_vec3 w = t3_object_get_world_position(hand);
  t3_object_set_position(HA.squeegee, w.x - HA.origin.x, w.y - HA.origin.y, w.z - HA.origin.z);
  t3_object_set_rotation(HA.squeegee, -1.0f + sinf((float)ds_now() * 3) * 0.1f, 0.2f, 0);
}
static float turn_to_sky(float t) { return DS_PI - DS_PI * sm((t - (line_t0(3) - 0.6f)) / 1.6f); }

static void prologue_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  float tf = C.L[11].t0 + C.L[11].dur - 0.3f;
  o->juno_visible = true;
  v3 jp = pv(-1.0f + sinf(t * 0.7f) * 0.02f, 0, 1.5f);
  float yaw = turn_to_sky(t);
  const char *clip = t < line_t0(3) - 0.5f ? "Wave" : "Idle";
  if (t > line_t0(11) - 0.2f && t < tf) clip = "Yes";
  float sp = 0.8f;
  if (t >= tf) {
    float w = t - tf;
    jp = juno_fall(w); jp.y = w < 1.0f ? 0 : jp.y - 1.0f;
    yaw = 0.0f;
    clip = w < 0.95f ? "Walk" : w < 1.15f ? "Jump" : "Jump_Idle";
    sp = 1.1f;
  }
  o->juno_pos = W(jp); o->juno_yaw = WY(yaw);
  o->juno_clip = clip; o->juno_clip_speed = sp;
  squeegee_follow(t < tf);
  cue(0, t, line_t0(3) - 3.8f, SFX_ALARM, 0.22f);
  cue(1, t, line_t0(5) - 1.0f, SFX_PICKUP, 0.5f);
  cue(2, t, tf + 1.0f, SFX_JUMP, 0.8f);
  cue(3, t, tf + 1.3f, SFX_GLIDE, 0.35f);
  HA.gondola->position.x = sinf(t * 0.5f) * 0.05f;
  for (int i = 0; i < 8; i++) HA.strip[i]->visible = t > line_t0(1) - 1.0f + i * 0.9f;
  for (int i = 0; i < 3; i++) {
    float r = sm((t - (line_t0(3) - 4.0f + i * 0.9f)) / 6.0f);
    HA.ship[i]->visible = true;
    HA.ship[i]->position.y = -420 + r * 520;
  }
  /* the Tenants arrive, lift the window out and carry it away */
  float u = panel_u(t);
  v3 pp = panel_pos(u);
  HA.panel->position.x = pp.x; HA.panel->position.y = pp.y; HA.panel->position.z = pp.z;
  HA.panel->rotation.z = 0; t3_object_set_rotation(HA.panel, sinf(u * 0.9f) * 0.05f, sm((u - 2.5f) / 6.0f) * 0.7f, sinf(u * 0.7f) * 0.06f + sm((u - 3) / 8.0f) * 0.25f);
  static const v3 grip[4] = {{-2.6f, 1.9f, 0.5f}, {2.6f, 1.9f, 0.5f}, {-2.6f, -1.9f, 0.5f}, {2.6f, -1.9f, 0.5f}};
  for (int i = 0; i < 4; i++) {
    float arrive = sm((u + 5.0f - i * 0.4f) / 4.5f);   /* flying in from out in the sky */
    v3 off = grip[i];
    float cy = cosf(HA.panel->rotation.y), sy = sinf(HA.panel->rotation.y);
    v3 g = v3_(pp.x + off.x * cy + off.z * sy, pp.y + off.y, pp.z - off.x * sy + off.z * cy);
    v3 from = v3_(g.x + 40 + i * 9, g.y + 24 - i * 6, g.z + 140);
    v3 at = v3_lerp(from, g, arrive);
    at.y += sinf(t * 2.2f + i) * 0.18f;
    bool vis = u > -5.5f;
    act_put(&TF[i], W(at), WY(DS_PI + (arrive < 1 ? 0.15f : 0)), 1.4f, vis);
    act_play(&TF[i], arrive < 0.98f ? "Fast_Flying" : "Flying_Idle", 1);
  }
  for (int i = 0; i < 2; i++) {   /* two small ones scout ahead of the ships */
    v3 at = v3_(pp.x - 12 + i * 22 + sinf(t * 1.3f + i * 2) * 3, pp.y + 6 + i * 5 + sinf(t * 1.7f + i) * 1.5f, pp.z + 14 + i * 9);
    act_put(&TS[i], W(at), WY(DS_PI + 0.4f), 1.5f, u > -1.0f);
    act_play(&TS[i], "Flying_Idle", 1);
  }
}

/* a Tenant or two on the ground and in the air, placed in the cutscene's frame */
static void put_tl(int i, v3 p, float yaw, const char *clip, float spd) { act_put(&TL[i], W(p), WY(yaw), 1.0f, true); act_play(&TL[i], clip, spd); }
static void put_tf(int i, v3 p, float yaw, const char *clip) { act_put(&TF[i], W(p), WY(yaw), 1.0f, true); act_play(&TF[i], clip, 1); }
static void put_ts(int i, v3 p, float yaw, const char *clip) { act_put(&TS[i], W(p), WY(yaw), 1.0f, true); act_play(&TS[i], clip, 1); }

/* ═════════════ SUBLET: Marguerite's archive briefing over the gardens ═════════════ */
static void sublet_build(void) {
  C.org = SU.origin; C.yaw = 0;
  say("MARGUERITE", "Juno, I've been in the colony archive. The Tenancies were built for owners who never arrived.", 3.0f);   /* 0 */
  say("MARGUERITE", "A caretaker intelligence was left to run them. The lease expired two hundred years ago. Nobody told it.", 0.5f);  /* 1 */
  say("JUNO", "Somebody should.", 0.6f);                                                                                           /* 2 */
  say("MARGUERITE", "The Sublet is next. The Tenants have turned its spine into gardens, lit with our windows.", 0.7f);          /* 3 */
  say("JUNO", "Good light.", 0.8f);                                                                                                /* 4 */
  say("MARGUERITE", "Past the gardens, those lights are the Freehold. I wouldn't call it asleep.", 0.9f);                         /* 5 */
  C.tail = 3.2f;
  finish_lines();
  shot(-1, 0, pv(-44, 24, -50), pv(0, 4, 4), 50, pv(-26, 10, -28), pv(2, 4, 6), 44, 0.04f, 1);
  shot(0, 1.5f, pv(-30, 4, -22), pv(0, 3, 6), 40, pv(-14, 3.4, -10), pv(4, 3, 8), 38, 0.05f, 0);
  shot(1, 0, pv(-6, 2.6, -2), pv(8, 2.2, 8), 40, pv(-1, 2.4, 1), pv(8, 2.2, 9), 38, 0.04f, 0);
  shot(2, 0, pv(-2.4, 1.5, -5.2), pv(-4, 1.6, -8), 30, pv(-2.7, 1.55, -5.6), pv(-4, 1.6, -8), 27, 0.03f, 0);
  shot(3, 0, pv(-4, 2.3, 1.5), pv(7, 1.8, 8), 36, pv(0, 2.1, 4.0), pv(7, 1.9, 8), 34, 0.04f, 0);
  shot(4, 0, pv(-1.3, 1.6, -9.4), pv(-4, 1.6, -8), 30, pv(-1.6, 1.62, -9.1), pv(-4, 1.6, -8), 27, 0.03f, 0);
  shot(5, 0, pv(-8, 2.5, -14), pv(-30, 20, 600), 44, pv(-8, 14, -18), pv(-300, 150, 3100), 34, 0.03f, 1);
  resolve_shots();
}
static void sublet_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  o->juno_visible = t > line_t0(2) - 1.5f && t < line_t0(5) + 0.5f;
  o->juno_pos = W(pv(-4, 0, -8)); o->juno_yaw = WY(0.5f);
  o->juno_clip = "Idle"; o->juno_clip_speed = 0.7f;
  /* gardeners at the nest, a flyer overhead hauling a pane, small ones among the lamps */
  put_tl(0, pv(3.5f, 0, 5), 3.8f, "Idle", 0.8f);
  put_tl(1, pv(9.5f, 0, 5.5f), 2.6f, t > line_t0(3) ? "Wave" : "Idle", 0.9f);
  put_tl(2, pv(6, 0, 12.5f), 0.4f, "Idle", 0.7f);
  put_tl(3, pv(-14, 0, -5.5f), 0.9f, "Idle", 0.7f);
  put_tf(0, pv(-20 + sinf(t * 0.2f) * 6, 12 + sinf(t * 0.6f), -8 + t * 0.9f), 0.7f, "Flying_Idle");
  put_ts(0, pv(2 + sinf(t * 0.9f) * 5, 6.5f + sinf(t * 1.3f), 2 + cosf(t * 0.9f) * 5), t * 0.9f, "Flying_Idle");
  put_ts(1, pv(8 + sinf(t * 0.7f + 2) * 6, 5 + sinf(t * 1.1f), 9 + cosf(t * 0.7f + 2) * 4), t * 0.7f + 2, "Flying_Idle");
  put_ts(2, pv(-16 + sinf(t * 0.8f) * 4, 4.5f, -9 + cosf(t * 0.8f) * 3), t * 0.8f, "Flying_Idle");
  /* the grow lamps and the nests glow */
  for (int i = 0; i < 5; i++) fx_glow(W(pv(-12 + i * 8.f, 7, 18 - (i & 1) * 36.f)), 5.0f + sinf(t * 2 + i), 0xff8ac8);
  static const float nx[5] = {6, -14, 38, -42, -8}, nz[5] = {8, -10, 38, 22, 76};
  for (int i = 0; i < 5; i++) fx_glow(W(pv(nx[i], 1.4f, nz[i])), 6.0f + sinf(t * 1.5f + i), 0xff9ad0);
  SU.freehold->position.y = 40 + sinf(t * 0.1f) * 2;
}

/* ═════════════ FREEHOLD: night side, the cranes unfold ═════════════ */
static float unfold(float t, int i) { return sm((t - (line_t0(0) + 4.0f + i * 0.9f)) / 9.0f); }
static void freehold_build(void) {
  C.org = FR.origin; C.yaw = 0;
  say("MARGUERITE", "Night side. This is the Freehold, the flagship. It's quiet, which I do not like.", 3.2f);   /* 0 */
  say("MARGUERITE", "Juno, it's assembling itself out of the furniture.", 1.6f);                               /* 1 */
  say("JUNO", "Noted.", 0.7f);                                                                                 /* 2 */
  say("MARGUERITE", "Logging that as a structural concern.", 0.6f);                                            /* 3 */
  say("THE SUPERINTENDENT", "AUTOMATED NOTICE. ASSEMBLY IN PROGRESS. PLEASE USE ANOTHER ENTRANCE.", 1.2f);    /* 4 */
  C.tail = 3.4f;
  finish_lines();
  shot(-1, 0, pv(0, 70, 440), pv(0, 18, 0), 44, pv(0, 52, 300), pv(0, 22, 0), 42, 0.03f, 1);
  shot(0, 2.2f, pv(110, 7, 120), pv(0, 22, 0), 42, pv(95, 9, 105), pv(0, 26, 0), 40, 0.05f, 0);
  shot(1, 0, pv(-70, 30, 100), pv(0, 36, 0), 44, pv(-45, 26, 70), pv(0, 40, 0), 42, 0.05f, 0);
  shot(2, 0, pv(4.2, 1.7, 70.5), pv(0, 1.65, 72), 30, pv(3.6, 1.7, 70.8), pv(0, 1.65, 72), 27, 0.03f, 0);
  shot(3, 0, pv(130, 46, 190), pv(0, 40, 0), 46, pv(98, 66, 140), pv(0, 46, 0), 44, 0.04f, 1);
  shot(4, 0, pv(0, 30, -8), pv(0, 46, -150), 46, pv(0, 40, -60), pv(0, 70, -150), 36, 0.05f, 1);
  resolve_shots();
}
static void freehold_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  o->juno_visible = true;
  o->juno_pos = W(pv(0, 0, 72)); o->juno_yaw = WY(DS_PI);
  o->juno_clip = "Idle"; o->juno_clip_speed = 0.7f;
  float alarm = sm((t - line_t0(4)) / 1.0f);
  cue(0, t, line_t0(0) + 4.0f, SFX_BOOM, 0.5f);
  cue(1, t, line_t0(4), SFX_ALARM, 0.45f);
  for (int i = 0; i < 8; i++) {
    float u = unfold(t, i);
    t3_object_set_rotation(FR.crane[i], 0, 0, ds_lerp(-1.45f, 0.35f, u) + sinf(t * 1.3f + i) * 0.01f * u);
    t3_object_set_rotation(FR.jib[i], 0, 0, ds_lerp(3.0f, -0.9f, sm(u * 1.4f - 0.2f)));
    t3_object_update_matrix_world(FR.lamp[i], true);
    t3_vec3 lw = t3_object_get_world_position(FR.lamp[i]);
    fx_glow(v3_(lw.x, lw.y, lw.z), 7 + 2 * sinf(t * 3 + i) + 5 * alarm, i & 1 ? 0xe8433a : 0xff8a6a);
  }
  fx_glow(W(pv(0, 90, -113)), 14 + 10 * alarm * (0.6f + 0.4f * sinf(t * 9)), 0xe8433a);
  /* the furniture it is assembling itself from, drawn in toward the crown */
  for (int i = 0; i < 10; i++) {
    float f = sm((t - (line_t0(0) + 1.0f + i * 0.5f)) / 14.0f);
    float a = (float)i * 0.628f + t * 0.22f + (1 - f) * 1.4f;
    float r = ds_lerp(170 + i * 8.f, 30 + (i % 3) * 8.f, f);
    float y = ds_lerp(30 + i * 7.f, 34 + (i % 4) * 7.f, f) + sinf(t * 0.8f + i) * 2;
    t3_object *g = FR.furn[i];
    g->visible = t > line_t0(0) + 0.5f;
    t3_object_set_position(g, sinf(a) * r, y, cosf(a) * r);
    t3_object_set_rotation(g, t * 0.3f + i, t * 0.4f * (i & 1 ? 1 : -1) + i, t * 0.2f);
  }
}

/* where the arena cutscenes stand: Juno's spot (or the level's arena) and the way she faces */
static float floor_at(v3 p, float fallback) { v3 q = p; q.y += 40; float y = world_floor_y(q); return y > -1e8f ? y : fallback; }
static void arena_frame(const char *arena, float back) {
  v3 ac, sp; float ay;
  bool have_a = world_anchor(arena, &ac, &ay);
  if (!have_a) ac = have_player ? v3_add(player_pos, v3_(0, 0, 20)) : world_spawn();
  v3 from = have_player ? player_pos : v3_sub(ac, v3_(0, 0, back));
  float dx = ac.x - from.x, dz = ac.z - from.z;
  float l = sqrtf(dx * dx + dz * dz);
  if (l < 2) { dx = 0; dz = 1; l = 1; }
  C.yaw = atan2f(dx / l, dz / l);
  C.org = from;
  (void)sp; (void)ay;
}

/* ═════════════ BRUTE: a Brute unfolds from a maintenance bay ═════════════ */
static float brute_k(float t) { return sm((t - 1.0f) / 3.2f); }
static void brute_build(void) {
  arena_frame("brute_arena", 14);
  C.fi = 0.35f; C.fo = 0.45f; C.ray = true;
  say("BRUTE", "QUIET HOURS ARE IN EFFECT.", 2.8f);                       /* 0 */
  say("BRUTE", "PLEASE LOWER YOUR WEAPON. AND YOUR VOICE.", 0.3f);        /* 1 */
  say("MARGUERITE", "That's not a Tenant. That's staff.", 0.3f);          /* 2 */
  say("JUNO", "I was being quiet.", 0.5f);                                /* 3 */
  C.tail = 1.4f;
  finish_lines();
  shot(-1, 0, pv(3.0, 1.1, -2.5), pv(0, 1.2, 14), 50, pv(1.6, 1.5, -1.2), pv(0, 2.0, 14), 46, 0.06f, 1);
  shot(0, -0.3f, pv(6.5, 2.0, 7.5), pv(0, 3.6, 14), 40, pv(5.4, 3.0, 8.5), pv(0, 4.0, 14), 38, 0.05f, 0);
  shot(1, 0, pv(-0.9, 1.7, -0.9), pv(0, 3.2, 14), 40, pv(-0.7, 1.75, -0.7), pv(0, 3.4, 14), 36, 0.03f, 0);
  shot(2, 0, pv(1.0, 1.7, 2.7), pv(0, 1.6, 0), 30, pv(0.9, 1.7, 2.5), pv(0, 1.6, 0), 27, 0.03f, 0);
  shot(3, 0, pv(-1.4, 1.65, 2.6), pv(0, 1.62, 0), 26, pv(-1.2, 1.65, 2.4), pv(0, 1.62, 0), 24, 0.02f, 0);
  resolve_shots();
}
static void brute_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  o->juno_visible = true;
  o->juno_pos = W(pv(0, 0, 0)); o->juno_yaw = WY(0);
  o->juno_clip = "Idle_Gun"; o->juno_clip_speed = 0.8f;
  cue(0, t, 0.5f, SFX_BOOM, 0.5f);
  cue(1, t, 4.0f, SFX_ALARM, 0.3f);
  v3 hp = W(pv(0, 0, 14));
  float fy = floor_at(hp, C.org.y);
  BAY.root->visible = true;
  t3_object_set_position(BAY.root, hp.x, fy + 0.03f, hp.z);
  t3_object_set_rotation(BAY.root, 0, WY(0), 0);
  float open = sm((t - 0.5f) / 1.2f);
  t3_object_set_position(BAY.plate[0], -3.1f * open, 0, 0);
  t3_object_set_position(BAY.plate[1], 3.1f * open, 0, 0);
  BAY.lamp->visible = sinf(t * 9) > -0.2f;
  float k = brute_k(t);
  for (int i = 0; i < 4; i++) if (open > 0.05f) fx_glow(v3_(hp.x + (i & 1 ? 3.6f : -3.6f), fy + 0.8f, hp.z + (i & 2 ? 4.2f : -4.2f)), 3.5f * (BAY.lamp->visible ? 1 : 0.4f), 0xff9a2a);
  /* unfold from the crouch the bay keeps it in, rise on the lift, then walk out a step or two */
  float step = sm((t - 5.0f) / 5.0f) * 2.5f;
  v3 bp = pv(0, -4.2f * (1 - k), 14 - step);
  act_put(&MA, W(bp), WY(DS_PI), 1.0f, t > 0.9f);
  if (k < 1) act_pose(&MA, "Pickup", 0.5f * (1 - k) * (1 - k));
  else act_play(&MA, step > 0.05f && step < 2.45f ? "Walk" : "Idle", 0.8f);
  if (open > 0.3f && k < 0.9f) { static float acc; acc += dt; if (acc > 0.12f) { acc = 0; fx_sparks(v3_(hp.x, fy + 0.3f, hp.z), v3_(0, 1, 0), 0xffb347, 3); } }
}

/* ═════════════ BOSS: the Superintendent rises ═════════════ */
static float boss_dist;
static v3 boss_wp[4] = {{0, 0.64f * 15.4f, -0.2f * 15.4f}, {-0.27f * 15.4f, 0.74f * 15.4f, 0}, {0.27f * 15.4f, 0.74f * 15.4f, 0}, {0, 0.6f * 15.4f, 0.19f * 15.4f}};
static void boss_build(void) {
  v3 bs; float by; bool have_b = world_anchor("boss_spawn", &bs, &by);
  v3 ac; float ay; bool have_a = world_anchor("boss_arena", &ac, &ay);
  if (!have_b) bs = have_a ? v3_add(ac, v3_(0, 0, 14)) : have_player ? v3_add(player_pos, v3_(0, 0, 34)) : world_spawn();
  v3 from = have_player ? player_pos : v3_sub(bs, v3_(0, 0, 34));
  float dx = from.x - bs.x, dz = from.z - bs.z, l = sqrtf(dx * dx + dz * dz);
  if (l < 2) { dx = 0; dz = -1; l = 1; }
  boss_dist = ds_clamp(l, 26, 46);
  C.org = bs; C.yaw = atan2f(dx / l, dz / l);
  C.fi = 0.35f; C.fo = 0.5f; C.ray = true;
  say("THE SUPERINTENDENT", "NOTICE OF EVICTION. YOU ARE IN BREACH OF A LEASE YOU HAVE NOT SIGNED.", 6.2f);          /* 0 */
  say("THE SUPERINTENDENT", "INSPECTION HATCHES ONE THROUGH FOUR ARE CLEAN AND IN GOOD ORDER.", 0.7f);                /* 1 */
  say("MARGUERITE", "Juno, that is the most helpful eviction notice I have ever read.", 0.9f);                         /* 2 */
  say("JUNO", "I can see those from here.", 0.7f);                                                                      /* 3 */
  C.tail = 1.6f;
  finish_lines();
  float D = boss_dist;
  shot(-1, 0, pv(-16, 2.0, 24), pv(0, 3, 0), 56, pv(-12, 3.0, 24), pv(0, 6, 0), 54, 0.07f, 1);
  shot(-1, 3.0f, pv(-20, 2.0, 20), pv(0, 4, 0), 62, pv(-10, 13, 30), pv(0, 11, 0), 56, 0.05f, 1);
  shot(0, 0, pv(0, 5, D), pv(0, 8, 0), 48, pv(0, 7, D - 5), pv(0, 8.5f, 0), 46, 0.04f, 0);
  shot(1, 0, pv(9, 11.5f, 15), pv(0, 10.5f, 0), 40, pv(6, 12, 13), pv(0, 10.5f, 0), 36, 0.04f, 0);
  shot(2, 0, pv(2.4, 1.85f, D - 3.4f), pv(0, 1.65f, D), 30, pv(2.2, 1.85f, D - 3.2f), pv(0, 1.65f, D), 27, 0.03f, 0);
  shot(3, 0, pv(-2.0, 2.4f, D + 5.5f), pv(0.5f, 8.5f, 0), 44, pv(-1.6, 2.3f, D + 4.2f), pv(0.3f, 9.0f, 0), 38, 0.03f, 1);
  resolve_shots();
}
static void boss_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  cue(0, t, 0.8f, SFX_BOOM, 0.9f);
  cue(1, t, 3.0f, SFX_BOOM, 0.7f);
  cue(2, t, line_t0(0), SFX_ALARM, 0.5f);
  v3 jp = pv(0, 0, boss_dist);
  v3 jw = W(jp);
  float jy = floor_at(jw, C.org.y);
  o->juno_visible = true;
  o->juno_pos = v3_(jw.x, jy, jw.z); o->juno_yaw = WY(DS_PI);
  o->juno_clip = "Idle_Gun"; o->juno_clip_speed = 0.8f;
  /* it rises through the arena floor, shaking dust loose, then straightens up */
  float rise = sm((t - 0.8f) / 4.6f);
  float shake_a = (1 - rise) * rise * 4 * 0.5f;
  v3 bp = pv(sinf(t * 31) * shake_a * 0.3f, -16.5f * (1 - rise), 0);
  act_put(&MB, W(bp), WY(0), 1.0f, true);
  act_play(&MB, "Idle", 0.7f);
  if (t > 0.8f && t < 5.6f) {
    static float acc; acc += dt;
    if (acc > 0.09f) { acc = 0; float a = ds_rr(0, 6.28f); v3 r = W(pv(sinf(a) * 7, 0.2f, cosf(a) * 7)); fx_sparks(v3_(r.x, C.org.y + 0.4f, r.z), v3_(0, 1, 0), 0x9a8cd0, 4); fx_ring_flat(v3_(C.org.x, C.org.y + 0.1f, C.org.z), 5 + rise * 14, 0xe8433a); }
  }
  /* the four inspection hatches light in turn as it reads them out */
  float lit0 = line_t0(1) + 0.6f;
  for (int i = 0; i < 4; i++) {
    float on = sm((t - (lit0 + i * 0.55f)) / 0.3f);
    if (on <= 0) continue;
    v3 w = W(v3_(boss_wp[i].x, boss_wp[i].y + bp.y, boss_wp[i].z));
    float pulse = 1 + 0.15f * sinf(t * 8 + i);
    fx_glow(w, 4.6f * on * pulse, 0xffa030);
    fx_glow(w, 2.0f * on, 0xffffff);
  }
  /* the red eyes of the building: a scan line sweeps the floor between it and her */
  if (t > line_t0(0)) fx_beam(W(pv(0, 13.0f, 1.2f)), W(pv(sinf(t * 1.3f) * 6, 0.2f, boss_dist * 0.6f)), 0xe8433a, 0.12f);
}

/* ═════════════ ENDING ═════════════ */
static v3 hv(float x, float y, float z) { return v3_(x + 4000, y, z); }   /* Halcyon's frame inside the ending's (the Freehold's) */
static void ending_build(void) {
  C.org = FR.origin; C.yaw = 0;
  say("THE SUPERINTENDENT", "LEASE... RENEWED?", 4.2f);                                                              /* 0 */
  say("MARGUERITE", "I have drafted a new lease, Juno. The Tenants keep the Sublet's gardens.", 0.8f);                 /* 1 */
  say("MARGUERITE", "The colony gets its windows back. The Freehold becomes a lighthouse.", 0.4f);                   /* 2 */
  say("THE SUPERINTENDENT", "TERMS ACCEPTABLE. GOODNIGHT.", 0.8f);                                                   /* 3 */
  say("FORM 31-T", "THANK YOU FOR THE VIEW. PLEASE DISREGARD ALL PRIOR COMPLAINTS ABOUT YOU.", 3.8f);               /* 4 */
  say("MARGUERITE", "There's a streak.", 2.6f);                                                                      /* 5 */
  say("JUNO", "There's always a streak.", 0.7f);                                                                     /* 6 */
  C.tail = 5.0f;
  finish_lines();
  /* the crown: the Superintendent kneels */
  shot(-1, 0, pv(-6, 3.0, 62), pv(0, 8, 10), 48, pv(-3, 4.0, 52), pv(0, 8, 10), 44, 0.05f, 1);
  shot(0, 0, pv(7, 8, 30), pv(0, 8, 10), 38, pv(5, 8.5f, 25), pv(0, 8, 10), 34, 0.03f, 0);
  shot(1, 0, pv(2.6, 1.8, 44), pv(0, 1.65, 47), 30, pv(2.3, 1.8, 44.5f), pv(0, 1.65, 47), 27, 0.03f, 0);
  shot(2, 0, pv(110, 40, 200), pv(0, 70, -150), 46, pv(90, 55, 170), pv(0, 80, -150), 42, 0.03f, 1);
  shot(3, 0, pv(-4, 9, 22), pv(0, 9, 10), 36, pv(-2, 8.6f, 19), pv(0, 8.8f, 10), 32, 0.02f, 0);
  /* Halcyon Reach again: the window goes home, the Tenants' form arrives */
  shot(4, -3.4f, hv(62, 3, 16), hv(0, 0, 0), 40, hv(66, 4, 20), hv(0, 0, 0), 36, 0.05f, 1); follow_last();
  shot(4, 0.6f, hv(1.6, 1.85, 3.4), hv(-1.0, 1.7, 1.5), 34, hv(1.0, 1.8, 2.6), hv(-1.0, 1.7, 1.5), 28, 0.03f, 0);
  shot(5, 0, hv(-3.0, 2.0, 3.8), hv(-0.3, 2.2, 0.2), 38, hv(-2.2, 1.95, 3.2), hv(-0.3, 2.2, 0.2), 34, 0.03f, 0);
  shot(6, 0.2f, hv(0.4, 1.8, 0.65), hv(-1.0, 1.7, 1.5), 26, hv(0.3, 1.8, 0.7), hv(-1.0, 1.7, 1.5), 23, 0.02f, 0);
  shot(6, 2.3f, hv(-2.0, 2.2, 6.5), hv(0, 8, 60), 36, hv(-4.5, 4.5, 14), hv(0, 30, 120), 44, 0.03f, 1);
  resolve_shots();
}
static int end_part(float t) { return t < line_t0(4) - 3.9f ? 0 : 1; }
/* the window's trip home, shared by the carriers and the camera that follows them */
static float end_u(float t) { return (t - (line_t0(4) - 4.5f)) / 5.0f; }
static v3 end_window(float t) {
  float e = sm(end_u(t));
  return v3_lerp(v3_(146, 34, 120.12f), v3_(76, 4, 0.12f), e);
}
static v3 ending_follow(float t) { v3 w = end_window(t); return v3_(w.x + 4000, w.y, w.z); }
static int end_part_prev;
static void ending_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  int part = end_part(t);
  if (part != end_part_prev) { end_part_prev = part; if (part) look(0xbfd3e6, 0.0007f, 0xdfefff, 0x6a6258, 1.15f, 0xffe6c4, 2.2f); }
  FR.root->visible = part == 0; HA.root->visible = part == 1;
  if (part == 0) {
    /* the Superintendent, on its knees: bowed, lowered, powering down */
    float k = sm((t - 0.6f) / 3.2f);
    cue(0, t, 0.6f, SFX_BOOM, 0.5f);
    cue(1, t, line_t0(3) + 0.8f, SFX_UI_OK, 0.8f);
    v3 bp = pv(0, -0.3f * k, 10);
    act_put(&MB, W(bp), WY(0), 1.0f, true);
    act_pose(&MB, "Death", 0.24f * k);
    if (t > line_t0(0) + 1.4f && t < line_t0(3) + 2) { static float acc; acc += dt; if (acc > 0.5f) { acc = 0; fx_explosion(W(pv(ds_rr(-3, 3), ds_rr(4, 9), 12)), 1.4f); } }
    float off = sm((t - (line_t0(3) + 0.8f)) / 1.2f);   /* the red goes out as it says goodnight */
    float a = 1 - off;
    for (int i = 0; i < 4; i++) fx_glow(W(v3_(boss_wp[i].x, boss_wp[i].y * (1 - 0.42f * k), boss_wp[i].z + 10)), 3.6f * a * (0.7f + 0.3f * sinf(t * 6 + i)), 0xff5a3a);
    FR.beam->visible = t > line_t0(3) + 1.0f;
    for (int i = 0; i < 8; i++) {
      t3_object_set_rotation(FR.crane[i], 0, 0, 0.35f - 0.2f * k);
      t3_object_set_rotation(FR.jib[i], 0, 0, -0.9f + 0.5f * k);
      FR.crane[i]->visible = true;
    }
    for (int i = 0; i < 10; i++) FR.furn[i]->visible = false;
    t3_object_set_rotation(FR.beam, 0, t * 0.5f, 0);
    o->juno_visible = true;
    o->juno_pos = W(pv(0, 0, 48)); o->juno_yaw = WY(DS_PI); o->juno_clip = "Idle"; o->juno_clip_speed = 0.7f;
  } else {
    act_put(&MB, W(hv(0, -50, 0)), 0, 1, false);
    for (int i = 0; i < 8; i++) { HA.strip[i]->visible = i != 5; }
    for (int i = 0; i < 3; i++) { HA.ship[i]->visible = true; HA.ship[i]->position.y = 100; }
    o->juno_visible = true;
    o->juno_pos = W(hv(-1.0f, 0, 1.5f)); o->juno_yaw = WY(DS_PI);
    o->juno_clip = t > line_t0(5) - 2.4f ? "Wave" : "Idle"; o->juno_clip_speed = 0.8f;
    if (t > line_t0(5) - 2.4f && t < line_t0(5) + 0.3f) o->juno_clip = "Idle";
    HA.panel->visible = false;
    /* the stolen window comes home: four Tenants carry it back into tower 8 */
    float u = end_u(t), e = sm(u);
    v3 wp = end_window(t);
    HA.window_back->visible = true;
    t3_object_set_position(HA.window_back, wp.x, wp.y, wp.z);
    t3_object_set_rotation(HA.window_back, 0, (1 - e) * 0.6f, 0);
    HA.hole->visible = e < 1;
    static const v3 grip[4] = {{-2.6f, 1.9f, 0.5f}, {2.6f, 1.9f, 0.5f}, {-2.6f, -1.9f, 0.5f}, {2.6f, -1.9f, 0.5f}};
    float rel = sm((u - 1.0f) / 1.2f);   /* once it is seated they let go and drift off */
    for (int i = 0; i < 4; i++) {
      v3 g = v3_add(wp, grip[i]);
      g = v3_add(g, v3_(rel * (14 + i * 3), rel * (10 + i * 2), rel * 30));
      act_put(&TF[i], W(hv(g.x, g.y + sinf(t * 2 + i) * 0.15f, g.z)), WY(DS_PI), 1.4f, u > -0.2f && rel < 0.98f);
      act_play(&TF[i], "Flying_Idle", 1);
    }
    /* the form: a flyer brings it to the gondola, where it waits on the rail */
    cue(2, t, line_t0(4) - 0.5f, SFX_PICKUP, 0.6f);
    float f = sm((t - (line_t0(4) - 0.5f)) / 3.2f);
    v3 fp = v3_lerp(v3_(18, 7, 14), v3_(0.9f, 1.55f, 2.0f), f);
    act_put(&TS[0], W(hv(fp.x + 0.7f, fp.y + 0.4f, fp.z)), WY(DS_PI + 0.6f), 1.2f, f < 1 || t < line_t0(5) + 3);
    act_play(&TS[0], "Flying_Idle", 1);
    HA.form->visible = true;
    t3_object_set_position(HA.form, fp.x, fp.y, fp.z);
    t3_object_set_rotation(HA.form, -0.15f * (1 - f), sinf(t * 1.2f) * 0.1f * (1 - f), 0.2f * (1 - f));
    HA.gondola->position.x = sinf(t * 0.5f) * 0.05f;
  }
}

/* ═════════════ COLD STORAGE: the exit chute drops Juno into the freezer holds ═════════════ */
static float cold_tl(void) { return line_t0(0) + 3.2f; }   /* she lands, having dropped for a second and a half */
static v3 cold_juno(float t) {   /* where she is: down the chute, then on the floor walking to the tags */
  float tl = cold_tl();
  if (t < tl) { float u = ds_clamp((t - (tl - 1.5f)) / 1.5f, 0, 1); return pv(0, 13.5f * (1 - u) * (1 - u), 18); }
  float w = ds_clamp((t - (line_t0(2) + 0.2f)) / 7.5f, 0, 1);
  return pv(ds_lerp(0, -4.6f, sm(w)), 0, ds_lerp(18, 35, w));
}
static v3 cold_follow(float t) { return cold_juno(t); }
static void cold_build(void) {
  C.org = CO.origin; C.yaw = 0;
  say("MARGUERITE", "Juno, the Sublet's exit chute lets out in the Lessee's freezer holds. Mind the drop.", 2.4f);   /* 0 */
  say("JUNO", "Cold.", 0.5f);                                                                                         /* 1 */
  say("MARGUERITE", "Two hundred years of frost. Everything here is wrapped. Nothing was ever unwrapped.", 0.5f);     /* 2 */
  say("MARGUERITE", "Look at the tags. Delivered two hundred years ago. Never collected.", 0.4f);                      /* 3 */
  say("MARGUERITE", "The Superintendent keeps the owners' furniture here, in case they arrive.", 0.6f);               /* 4 */
  say("JUNO", "They are late.", 0.6f);                                                                                /* 5 */
  say("MARGUERITE", "The warm corners will have Tenants in them. Please step quietly.", 0.6f);                         /* 6 */
  say("JUNO", "Define quietly.", 0.5f);                                                                               /* 7 */
  C.tail = 2.8f;
  finish_lines();
  shot(-1, 0, pv(-5, 1.6, 8), pv(0, 18, 18), 54, pv(-3, 1.8, 11), pv(0, 22, 18), 48, 0.03f, 1);          /* up the chute */
  shot(0, 1.0f, pv(5.5, 9.5, 24), pv(0, 0.5, 0), 46, pv(4.0, 2.4, 22), pv(0, 0.5, 0), 38, 0.05f, 1); follow_last();   /* down the fall */
  shot(1, 0, pv(0.9, 1.72, 20.3), pv(0, 1.4, 18), 30, pv(0.75, 1.74, 20.0), pv(0, 1.4, 18), 26, 0.02f, 0);
  shot(2, 0, pv(3.2, 2.0, 12), pv(0, 1.0, 0), 44, pv(3.0, 2.2, 30), pv(0, 1.0, 0), 40, 0.04f, 0); follow_last();
  shot(3, 0, pv(-1.6, 1.5, 27), pv(-7.2, 0.95, 32.5), 40, pv(-2.8, 1.3, 31), pv(-7.2, 0.95, 37), 34, 0.02f, 1);
  shot(4, 0, pv(1.5, 1.7, 24), pv(0, 6.5, 64), 46, pv(1.0, 1.9, 28), pv(0, 8, 64), 40, 0.03f, 1);
  shot(5, 0, pv(-2.7, 1.75, 33.3), pv(-4.6, 1.4, 35), 30, pv(-2.9, 1.72, 33.8), pv(-4.6, 1.4, 35), 26, 0.02f, 0);
  shot(6, 0, pv(0, 1.8, 90), pv(0, 3, 196), 44, pv(0, 2.2, 148), pv(0, 3.5, 196), 33, 0.03f, 1);
  shot(7, 0, pv(-4.0, 1.7, 37.6), pv(-4.6, 1.4, 35), 26, pv(-4.2, 1.7, 37.2), pv(-4.6, 1.4, 35), 23, 0.02f, 0);
  resolve_shots();
}
static void cold_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  float tl = cold_tl(), tf = tl - 1.5f;
  v3 jp = cold_juno(t);
  float yaw = 0; const char *clip = "Idle"; float sp = 0.7f;
  o->juno_visible = t > tf;
  if (t < tl) {
    float u = ds_clamp((t - tf) / 1.5f, 0, 1);
    clip = "Jump_Idle"; o->juno_wing = u > 0.12f && u < 0.97f;
  } else if (t < tl + 0.7f) {
    clip = "Jump_Land"; sp = 1.0f;
  } else {
    float w = ds_clamp((t - (line_t0(2) + 0.2f)) / 7.5f, 0, 1);
    if (w <= 0) yaw = 0;
    else if (w < 1) { clip = "Walk"; sp = 1.2f; yaw = -0.26f; }
    else {   /* at the tags she squares up to them, then turns back to the hall for the warm corners */
      float a = sm((t - (line_t0(2) + 7.7f)) / 0.6f), b = sm((t - (line_t0(6) - 0.6f)) / 1.0f);
      yaw = ds_lerp(ds_lerp(-0.26f, -DS_PI / 2, a), 0, b);
    }
  }
  o->juno_pos = W(jp); o->juno_yaw = WY(yaw);
  o->juno_clip = clip; o->juno_clip_speed = sp;
  static bool landed;
  if (t < tf) landed = false;
  if (t >= tl && !landed) { landed = true; v3 w = W(pv(0, 0.2f, 18)); fx_poof(w, 0xdff3ff, 2.2f); fx_sparks(w, v3_(0, 1, 0), 0xcfe8ff, 10); sfx(SFX_LAND, 0, 0.7f); }
  cue(0, t, tf - 0.2f, SFX_GLIDE, 0.35f);
  /* the chute's lamp and the hold's long lamps; the heater core at the end */
  fx_glow(W(pv(0, 14.5f, 18)), 7 + sinf(t * 7) * 0.6f, 0xfff3c8);
  for (int i = 0; i < 6; i++) {
    float z = 14 + i * 26.f;
    fx_glow(W(pv(i & 1 ? 6 : -6, 12.8f, z)), 6, 0xcfe8ff);
  }
  float fl = 1 + 0.08f * sinf(t * 5) + 0.05f * sinf(t * 13);
  fx_glow(W(pv(0, 4.0f, 192)), 22 * fl, 0xff9a3a);
  fx_glow(W(pv(0, 1.2f, 189)), 12 * fl, 0xffb060);
  /* the sleepers in the warm corner: big ones curled up, small ones perched */
  static const float sx[3] = {-3.2f, 2.8f, 0.4f}, sz[3] = {187, 185, 182.5f}, sy[3] = {0.6f, 3.5f, -2.8f};
  for (int i = 0; i < 3; i++) { act_put(&TL[i], W(pv(sx[i], 0, sz[i])), WY(sy[i]), 1.0f, true); act_pose(&TL[i], "Death", 0.9f); }
  act_put(&TL[3], W(pv(5.0f, 0, 58)), WY(2.4f), 1.0f, true); act_pose(&TL[3], "Death", 0.9f);
  for (int i = 0; i < 2; i++) { act_put(&TS[i], W(pv(-1.6f + i * 3.6f, 0.45f + sinf(t * 0.9f + i) * 0.05f, 187.5f)), WY(DS_PI + i), 1.0f, true); act_play(&TS[i], "Flying_Idle", 0.35f); }
  /* frost motes drifting through the cold air near the camera */
  static float acc; acc += dt;
  if (acc > 0.35f) { acc = 0; v3 c = o->cam_pos, f = v3_sub(o->cam_target, o->cam_pos); float l = v3_len(f); if (l > 0.1f) { f = v3_scale(f, 1 / l); fx_spores(v3_add(v3_add(c, v3_scale(f, ds_rr(3, 14))), v3_(ds_rr(-3, 3), ds_rr(-1, 2), ds_rr(-3, 3))), 0xdff3ff, 1); } }
}

/* ═════════════ THE ANNEX: Halcyon Reach, the morning the Tenants arrive ═════════════ */
static void annex_build(void) {
  C.org = AN.origin; C.yaw = 0;
  say("MARGUERITE", "Morning on Halcyon Reach. The Brutes are herding the Tenants onto our towers.", 2.6f);         /* 0 */
  say("JUNO", "Invasion.", 0.6f);                                                                                    /* 1 */
  say("MARGUERITE", "Look closer. They carried their children. They carried their windows.", 0.6f);                  /* 2 */
  say("MARGUERITE", "They are refugees, Juno. The Superintendent evicted them. We were the nearest door.", 0.5f);   /* 3 */
  say("JUNO", "That's my gondola.", 1.3f);                                                                          /* 4 */
  say("MARGUERITE", "It's the warmest place on the tower. The small one has your bucket.", 0.6f);                   /* 5 */
  say("JUNO", "Keep the bucket.", 1.2f);                                                                            /* 6 */
  say("MARGUERITE", "Brutes and turrets only. Nobody who is running.", 0.8f);                                        /* 7 */
  C.tail = 2.8f;
  finish_lines();
  shot(-1, 0, pv(-14, 2.6, 10), pv(30, 75, 3000), 52, pv(-10, 2.4, 8), pv(30, 70, 3000), 44, 0.03f, 1);
  shot(0, 1.8f, pv(-6, 2.0, 12), pv(10, 1.3, -2), 46, pv(-2, 2.0, 11), pv(10, 1.3, -2), 42, 0.03f, 0);
  shot(1, 0, pv(-6.5, 1.78, 5.6), pv(-9, 1.45, 4), 28, pv(-6.8, 1.78, 5.3), pv(-9, 1.45, 4), 26, 0.02f, 0);
  shot(2, 0, pv(2, 1.5, 10.5), pv(10.8, 1.4, 3.8), 40, pv(4.6, 1.4, 9.0), pv(10.8, 1.3, 3.8), 34, 0.02f, 1);
  shot(3, 0, pv(-13.6, 1.9, 1.2), pv(10, 1.3, 3.5), 40, pv(-12.4, 1.8, 1.8), pv(10, 1.3, 3.5), 36, 0.02f, 1);
  shot(4, 0, pv(-11.0, 2.1, 8.5), pv(-8.2, 0.7, 16.6), 36, pv(-10.6, 2.0, 9.2), pv(-8.2, 0.7, 16.6), 33, 0.02f, 0);
  shot(5, 0, pv(-10.6, 1.8, 9.6), pv(-8.2, 1.3, 16.5), 34, pv(-9.8, 1.65, 11.4), pv(-8.2, 1.5, 16.5), 24, 0.02f, 1);
  shot(6, 0, pv(-5.4, 1.6, 4.9), pv(-9, 1.45, 4.0), 27, pv(-5.7, 1.62, 4.7), pv(-9, 1.45, 4.0), 24, 0.02f, 0);
  shot(7, 0, pv(-13.0, 2.0, 1.4), pv(31.5, 0.8, 20), 32, pv(-10.5, 2.2, 2.8), pv(31.5, 0.8, 20), 28, 0.03f, 1);
  resolve_shots();
}
static void annex_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  o->juno_visible = true;
  /* she stands on tower 9's roof and turns: to the huddle, to her gondola, to the turret */
  float y1 = DS_PI / 2, y2 = 0.1f, y3 = DS_PI / 2;
  float yaw = y1;
  if (t > line_t0(4) - 0.6f) yaw = ds_lerp(y1, y2, sm((t - (line_t0(4) - 0.6f)) / 0.9f));
  if (t > line_t0(7)) yaw = ds_lerp(y2, y3, sm((t - line_t0(7)) / 1.2f));
  o->juno_pos = W(pv(-9, 0, 4)); o->juno_yaw = WY(yaw);
  o->juno_clip = "Idle"; o->juno_clip_speed = 0.7f;
  /* the huddle: four big Tenants crouched close, small ones among them, a spare window against them */
  static const float hx[4] = {11, 9.6f, 12.4f, 10.6f}, hz[4] = {3.4f, 5.4f, 5.2f, 1.4f}, hy[4] = {-1.9f, -0.2f, 3.6f, -2.8f};
  for (int i = 0; i < 4; i++) { act_put(&TL[i], W(pv(hx[i] + sinf(t * 0.8f + i) * 0.02f, 0, hz[i])), WY(hy[i]), 0.9f, true); act_pose(&TL[i], "Duck", 0.5f); }
  for (int i = 1; i < 3; i++) { act_put(&TS[i], W(pv(10.4f + i * 0.9f, -0.4f + sinf(t * 1.1f + i) * 0.04f, 3.8f + i * 0.5f)), WY(DS_PI + 0.7f * i), 1.0f, true); act_play(&TS[i], "Flying_Idle", 0.4f); }
  AN.pane->visible = true;
  t3_object_set_position(AN.pane, 9.4f, 0.7f, 4.0f);
  t3_object_set_rotation(AN.pane, -0.18f, -0.5f, 0);
  /* the cordon: three Brutes at the hatch end of the roof, creeping forward */
  float adv = ds_clamp(t * 0.12f, 0, 2.4f);
  act_put(&MA, W(pv(5.5f, 0, -11 + adv)), WY(0.35f), 1.0f, true); act_play(&MA, "Walk", 0.35f);
  act_put(&MC[0], W(pv(9.5f, 0, -12.5f + adv)), WY(0.1f), 1.0f, true); act_play(&MC[0], "Walk", 0.3f);
  act_put(&MC[1], W(pv(13.0f, 0, -10.0f + adv)), WY(-0.15f), 1.0f, true); act_play(&MC[1], "Walk", 0.33f);
  /* tower 4: a Brute bolting a turret down */
  act_put(&MC[2], W(pv(33.6f, 0.4f, 18.4f)), WY(-DS_PI / 2 + 0.3f), 1.0f, true); act_play(&MC[2], "Pickup", 0.5f);
  if (fmodf(t, 1.6f) < dt) fx_sparks(W(pv(31.4f, 2.0f, 20)), v3_(0, 1, 0), 0xffb347, 4);
  /* the small one asleep in the gondola, in Juno's bucket */
  v3 gp = pv(-8.2f, -0.75f + sinf(t * 0.8f) * 0.03f, 16.5f);   /* the flyers hover well above their origin */
  act_put(&TS[0], W(gp), WY(DS_PI - 0.2f), 1.0f, true); act_play(&TS[0], "Flying_Idle", 0.25f);
  AN.bucket->visible = true;
  t3_object_set_position(AN.bucket, gp.x + 0.02f, gp.y + 2.95f, gp.z);
  t3_object_set_scale(AN.bucket, 1.5f, 1.5f, 1.5f);
  t3_object_set_rotation(AN.bucket, DS_PI + 0.2f, 0, 0.18f);   /* upside down on its head, a little askew */
  AN.gondola->position.x = -8 + sinf(t * 0.5f) * 0.04f;
  /* Tenants streaming in from the ships and settling on the far towers */
  static const float tx_[6] = {-70, -30, 30, 75, 110, -105}, tz_[6] = {20, -40, -30, -10, 10, 0}, ty_[6] = {24, 40, 52, 8, 34, 26};
  for (int i = 0; i < 6; i++) {
    float u = fmodf(t * 0.045f + i / 6.f, 1.f);
    v3 from = pv(tx_[i] * 0.6f + 30, ty_[i] + 40, 520), to = pv(tx_[i], ty_[i], tz_[i]);
    v3 p = v3_lerp(from, to, sm(u)); p.y += sinf(t * 1.7f + i) * 0.4f;
    float s = sm(u * 6) * sm((1 - u) * 6);
    act_put(&TF[i], W(p), WY(DS_PI + 0.1f * i), 1.4f * s, true);
    act_play(&TF[i], "Fast_Flying", 1);
  }
  static float acc; acc += dt;
  if (acc > 0.5f) { acc = 0; fx_spores(W(pv(10.5f + ds_rr(-2, 2), ds_rr(0.5f, 2), 4 + ds_rr(-2, 2))), 0xcfe8b0, 1); }
}

/* ═════════════ THE BASEMENT: the Freehold's undercroft ═════════════ */
static float stamp_u(float t, int i) { return fmodf(t * 0.12f + i / 6.f, 1.f); }
static void basement_build(void) {
  C.org = BS.origin; C.yaw = 0;
  say("MARGUERITE", "Juno. Every form the Tenants ever filed ended up in the Freehold's basement.", 2.6f);        /* 0 */
  say("MARGUERITE", "Every one is stamped RECEIVED. Not one of them was ever read.", 0.5f);                        /* 1 */
  say("MARGUERITE", "The cabinets run for miles. I stopped counting at nine thousand.", 0.5f);                    /* 2 */
  say("JUNO", "Nobody read any of it.", 0.7f);                                                                     /* 3 */
  say("MARGUERITE", "The eviction runs through a relay in the server stacks. Cut it and the Freehold stops.", 0.8f);  /* 4 */
  say("MARGUERITE", "That is the plan. Please try not to disturb the filing.", 0.4f);                              /* 5 */
  say("JUNO", "Received.", 0.7f);                                                                                  /* 6 */
  C.tail = 3.2f;
  finish_lines();
  shot(-1, 0, pv(0, 7.2, -6), pv(0, 3.5, 150), 58, pv(0, 3.4, 14), pv(0, 3.5, 150), 48, 0.03f, 1);
  shot(0, 1.0f, pv(0.8, 1.3, -4.5), pv(0, 2.2, 60), 46, pv(0.6, 1.35, -1.0), pv(0, 2.2, 60), 44, 0.03f, 0);
  shot(1, 0, pv(-1.8, 1.5, 15.5), pv(1.0, 0.9, 22.2), 36, pv(-0.8, 1.25, 17.5), pv(1.0, 0.9, 22.2), 28, 0.02f, 1);
  shot(2, 0, pv(-0.5, 0.9, 30), pv(0, 3, 140), 52, pv(0, 6.0, 46), pv(0, 4, 150), 46, 0.03f, 1);
  shot(3, 0, pv(0.4, 1.7, 14.6), pv(-0.8, 1.62, 12), 28, pv(0.5, 1.72, 14.3), pv(-0.8, 1.62, 12), 25, 0.02f, 0);
  shot(4, 0, pv(0, 2.0, 105), pv(0, 5, 200), 44, pv(0, 3, 160), pv(0, 5.5, 200), 34, 0.03f, 1);
  shot(5, 0, pv(-1.8, 1.5, 8.2), pv(-0.2, 2.5, 40), 44, pv(-1.6, 1.55, 8.6), pv(-0.2, 2.5, 40), 40, 0.02f, 1);
  shot(6, 0, pv(0.3, 1.72, 14.4), pv(-0.8, 1.62, 12), 24, pv(0.35, 1.72, 14.2), pv(-0.8, 1.62, 12), 22, 0.02f, 0);
  resolve_shots();
}
static void basement_act(float t, float dt, cine_view_t *o) {
  (void)dt;
  o->juno_visible = true;
  float w = ds_clamp((t - 0.8f) / 8.0f, 0, 1);
  float z = ds_lerp(-1.5f, 12, w);
  bool walking = w > 0 && w < 1;
  o->juno_pos = W(pv(ds_lerp(0.1f, -0.8f, w), 0, z)); o->juno_yaw = WY(0);
  o->juno_clip = walking ? "Walk" : "Idle"; o->juno_clip_speed = walking ? 1.0f : 0.7f;
  /* the belt: forms ride from the drop pipe under the stamp and on to the heap */
  float arm = 0;
  for (int i = 0; i < 6; i++) {
    float u = stamp_u(t, i);
    t3_object *f = BS.form[i];
    t3_object_set_position(f, 1.0f, 0.67f, 19.0f + u * 6.4f);
    t3_object_set_rotation(f, 0, sinf(i * 2.3f) * 0.15f, 0);
    f->visible = u < 0.97f;
    T3_OBJ(f->children[1])->visible = u > 0.52f;
    float d = (u - 0.5f) / 0.035f;
    arm = fmaxf(arm, expf(-d * d));
  }
  t3_object_set_position(BS.stamp, 1.0f, 2.35f - 1.35f * arm, 22.2f + 0.0f);
  static float last_arm;
  if (arm > 0.9f && last_arm <= 0.9f) { sfx(SFX_UI, 0, 0.18f); fx_sparks(W(pv(1.0f, 0.75f, 22.2f)), v3_(0, 1, 0), 0xd23a2e, 2); }
  last_arm = arm;
  /* the clerk: a Brute that feeds the belt and never looks at a word */
  act_put(&MA, W(pv(3.0f, 0, 19.4f)), WY(-DS_PI / 2 - 0.2f), 0.7f, true);
  act_play(&MA, "Pickup", 0.7f);
  /* a small Tenant, at a drawer far down the aisle, looking for its family's form */
  act_put(&TS[0], W(pv(-2.2f, 1.2f + sinf(t * 0.9f) * 0.08f, 47)), WY(DS_PI / 2 + 0.3f), 1.0f, true);
  act_play(&TS[0], "Flying_Idle", 0.5f);
  /* capsules shuttle through the tubes; the relay hums and turns */
  for (int i = 0; i < 10; i++) {
    float zz = -6 + fmodf(t * (6 + (i % 3) * 2.5f) + i * 23.f, 230.f);
    t3_object_set_position(BS.cap[i], -1.8f + (i % 4) * 1.2f, 8.4f, zz);
  }
  float p = 1 + 0.15f * sinf(t * 3);
  t3_object_set_rotation(BS.relay_ring, t * 0.8f, 0, 0.3f * sinf(t * 0.5f));
  fx_glow(W(pv(0, 5.5f, 198)), 18 * p, 0xe8433a);
  fx_glow(W(pv(0, 9, 203)), 9, 0xff5a4a);
  for (int i = 0; i < 4; i++) fx_glow(W(pv(i & 1 ? 3.2f : -3.2f, 7.7f, 14 + i * 26.f)), 5, 0x37ff9a);
  cue(0, t, line_t0(6) + 0.1f, SFX_UI_OK, 0.7f);
}

/* ═════════════ the director ═════════════ */
static void restore_all(void) {
  restore_world();
  look_restore();
  HA.root->visible = SU.root->visible = FR.root->visible = false;
  CO.root->visible = AN.root->visible = BS.root->visible = false;
  AN.bucket->visible = AN.pane->visible = false;
  pool->visible = false;
  BAY.root->visible = false; FR.beam->visible = false;
  HA.window_back->visible = false; HA.form->visible = false; HA.panel->visible = true; HA.hole->visible = true;
  cast_hide_all();
  if (pistol) pistol->visible = true;
}

void cutscene_init(t3_scene *scene) {
  scn = scene;
  sets_init(scene);
  pool = k_group(&scene->base, 0, 0, 0);
  pool->visible = false;
  for (int i = 0; i < 6; i++) act_load(&TF[i], pool, "e_flying.glb", 1);
  for (int i = 0; i < 3; i++) act_load(&TS[i], pool, "e_small.glb", 1);
  for (int i = 0; i < 4; i++) act_load(&TL[i], pool, "e_large.glb", 1);
  act_load(&MA, pool, "e_mech_a.glb", 1.6f);
  act_load(&MB, pool, "e_mech_b.glb", 6);
  for (int i = 0; i < 3; i++) { act_load(&MC[i], pool, "e_mech_a.glb", 1.6f); act_tint(&MC[i], 0xffc89a, 0x1a0804, 1.0f); }
  for (int i = 0; i < 6; i++) act_tint(&TF[i], 0xffffff, 0x5fa83a, 0.5f);
  for (int i = 0; i < 3; i++) act_tint(&TS[i], i & 1 ? 0xffe9b0 : 0xffffff, 0x5fa83a, 0.5f);
  for (int i = 0; i < 4; i++) act_tint(&TL[i], 0xffffff, 0x5fa83a, 0.5f);
  act_tint(&MA, 0xffc89a, 0x1a0804, 1.0f); act_tint(&MB, 0xffffff, 0x401408, 0.5f);
  for (int i = 0; i < scene->base.child_count; i++) {
    t3_object *c = scene->base.children[i];
    if (!juno_root && !is_mine(c) && t3_object_get_by_name(c, "FernandoTheFlamingo")) juno_root = c;
  }
  if (juno_root) pistol = t3_object_get_by_name(juno_root, "Pistol");
  t3_object_traverse(&scene->base, light_cb, NULL);
}

void cutscene_hint_player(v3 pos, float yaw) { (void)yaw; player_pos = pos; have_player = true; }

void cutscene_start(cut_id id, int level) {
  if (!scn || C.active || (unsigned)id >= CUT_COUNT) return;
  cut_reset();
  memset(&C.skip, 0, sizeof C.skip); C.follow = NULL;
  C.id = id; C.level = level;
  switch (id) {
  case CUT_PROLOGUE: prologue_build(); C.act = prologue_act; C.follow = prologue_follow; break;
  case CUT_SUBLET: sublet_build(); C.act = sublet_act; break;
  case CUT_FREEHOLD: freehold_build(); C.act = freehold_act; break;
  case CUT_BRUTE: brute_build(); C.act = brute_act; break;
  case CUT_BOSS: boss_build(); C.act = boss_act; break;
  case CUT_ENDING: ending_build(); C.act = ending_act; C.follow = ending_follow; break;
  case CUT_COLD: cold_build(); C.act = cold_act; C.follow = cold_follow; break;
  case CUT_ANNEX: annex_build(); C.act = annex_act; break;
  case CUT_BASEMENT: basement_build(); C.act = basement_act; break;
  default: return;   /* not scripted yet: the game carries on */
  }
  memset(cued, 0, sizeof cued);
  last_line = -1; C.active = true; C.t = 0; C.skip = 0; C.armed = false; C.last_chars = 0;
#ifdef CINE_T0
  C.t = CINE_T0;
#endif
#ifdef CINE_DEBUG
  for (int i = 0; i < C.nl; i++) ds_log("cine %d line %d t0 %.1f dur %.1f", id, i, C.L[i].t0, C.L[i].dur);
  ds_log("cine %d length %.1f", id, C.end);
#endif
  prep_i = 0;   /* the lines are synthesised one a frame in cutscene_update, not all at once here */
  look_save();
  bool set = id == CUT_PROLOGUE || id == CUT_SUBLET || id == CUT_FREEHOLD || id == CUT_ENDING || id == CUT_COLD || id == CUT_ANNEX || id == CUT_BASEMENT;
  bool halc = id == CUT_PROLOGUE;
  hide_world(set);
  pool->visible = true;
  cast_hide_all();
  if (id == CUT_SUBLET) { SU.root->visible = true; look(0x2f5a5c, 0.0030f, 0xffd0e8, 0x4a6a58, 1.7f, 0xffc0dc, 2.2f); }
  if (id == CUT_FREEHOLD) { FR.root->visible = true; look(0x140e2e, 0.0008f, 0xa8a6ff, 0x4a3a78, 2.0f, 0xb8c0ff, 2.6f); }
  if (id == CUT_ENDING) { FR.root->visible = true; look(0x140e2e, 0.0008f, 0xa8a6ff, 0x4a3a78, 2.0f, 0xb8c0ff, 2.6f); end_part_prev = 0; }
  if (id == CUT_COLD) { CO.root->visible = true; look(0x2b4254, 0.0135f, 0xbfdcff, 0x405a70, 1.3f, 0xbfe0ff, 1.5f); }
  if (id == CUT_ANNEX) { AN.root->visible = true; look(0xd6d4d8, 0.0005f, 0xdfefff, 0x6a6258, 1.15f, 0xffd9a0, 2.4f); }
  if (id == CUT_BASEMENT) { BS.root->visible = true; look(0x0a1412, 0.010f, 0xb8e8d8, 0x30403a, 2.0f, 0xd8fff0, 0.9f); }
  if (halc) { HA.root->visible = true; look(0xbfd3e6, 0.0007f, 0xdfefff, 0x6a6258, 1.15f, 0xffe6c4, 2.2f); }
  if (pistol && set) pistol->visible = false;
  if (id == CUT_BRUTE || id == CUT_BOSS) BAY.root->visible = false;
}

bool cutscene_active(void) { return C.active; }

vo_speaker cutscene_voice_for(const char *who) {
  if (!who) return VO_ALERT;
  if (!strcmp(who, "MARGUERITE")) return VO_MARGUERITE;
  if (!strcmp(who, "JUNO")) return VO_JUNO;
  if (strstr(who, "SUPERINTENDENT")) return VO_SUPER;
  if (strstr(who, "FORM") || strstr(who, "TENANT")) return VO_TENANT;
  return VO_ALERT;
}

/* in the real level a wall or a prop can sit between the camera and its subject: pull the camera in front of it */
static void cam_clear(v3 *cam, v3 target) {
  v3 d = v3_sub(*cam, target);
  float l = v3_len(d);
  if (l < 0.5f) return;
  d = v3_scale(d, 1 / l);
  float t; v3 n;
  if (world_raycast(target, d, l, &t, &n) && t < l) *cam = v3_add(target, v3_scale(d, fmaxf(t - 0.35f, 0.5f)));
}
static float shake_n(float t, float k) { return sinf(t * 1.7f * k + 1) * 0.6f + sinf(t * 3.1f * k + 4) * 0.3f + sinf(t * 5.3f * k) * 0.1f; }

bool cutscene_update(float dt, const intent_t *I, cine_view_t *o) {
  if (!C.active) return false;
  memset(o, 0, sizeof *o);
  if (prep_i < C.nl) { voice_prepare(cutscene_voice_for(C.L[prep_i].who), C.L[prep_i].text); prep_i++; }
  /* skip: hold START, Enter or A; a tap does nothing, and the press that began the scene is ignored */
  bool held = I->jump_held || input_key_down(WC_KEY_ENTER) || input_start_held();
  if (!held) C.armed = true;
  if (C.armed && held) C.skip += dt / SKIP_HOLD; else C.skip -= dt * 2.5f;
  C.skip = ds_clamp(C.skip, 0, 1);
  /* a line's voice may run past the time its caption was given: hold the
   * clock at the end of the line until it finishes, rather than cut it off */
  bool hold = false;
  if (last_line >= 0 && voice_busy()) {
    const line_t *l = &C.L[last_line];
    if (C.t + dt >= l->t0 + l->dur - 0.4f) hold = true;
  }
  if (!hold) C.t += dt;
  if (C.t >= C.end || C.skip >= 1) { C.active = false; voice_stop(); restore_all(); return false; }
  float t = C.t;

  /* the camera: the shot in force, interpolated and shaken */
  int k = 0;
  for (int i = 0; i < C.ns; i++) if (C.S[i].s0 <= t) k = i;
  shot_t *s = &C.S[k];
  float s1 = k + 1 < C.ns ? C.S[k + 1].s0 : C.end;
  float u = ds_clamp((t - s->s0) / fmaxf(0.1f, s1 - s->s0), 0, 1);
  if (s->ease) u = sm(u);
  v3 p = v3_lerp(s->p0, s->p1, u), a = v3_lerp(s->a0, s->a1, u);
  if (s->follow && C.follow) { v3 f = C.follow(t); a = v3_add(a, f); }
  float sh = s->shake;
  p.x += shake_n(t, 1.0f) * sh; p.y += shake_n(t + 9, 1.3f) * sh * 0.7f; p.z += shake_n(t + 5, 0.9f) * sh;
  a.x += shake_n(t + 2, 1.1f) * sh * 1.5f; a.y += shake_n(t + 7, 1.2f) * sh * 1.5f;
  o->cam_pos = W(p); o->cam_target = W(a);
  if (C.ray) cam_clear(&o->cam_pos, o->cam_target);
  o->fov = ds_lerp(s->f0, s->f1, u);
#ifdef CINE_CAMS
  { static const float cc[][7] = {CINE_CAMS}; const float *c7 = cc[(int)(C.t - (float)CINE_T0) % (int)(sizeof cc / sizeof cc[0])];
    o->cam_pos = W(v3_(c7[0], c7[1], c7[2])); o->cam_target = W(v3_(c7[3], c7[4], c7[5])); o->fov = c7[6]; }
#endif

  cast_tick(dt);
  if (C.act) C.act(t, dt, o);

  /* captions */
  int li = -1;
  for (int i = 0; i < C.nl; i++) if (C.L[i].t0 <= t) li = i;
  if (li >= 0 && t < C.L[li].t0 + C.L[li].dur - 0.35f) {
    line_t *l = &C.L[li];
    int ch = (int)((t - l->t0) * TYPE_CPS);
    if (ch > l->n) ch = l->n;
    o->speaker = l->who; o->text = l->text; o->chars = ch;
    if (li != last_line) { last_line = li; C.last_chars = 0; voice_say(cutscene_voice_for(l->who), l->text); }
    for (int c = C.last_chars + 1; c <= ch; c++)
      if (c % 3 == 0 && l->text[c - 1] != ' ') sfx(SFX_UI, 0, 0.15f);   /* a soft tick every few characters */
    C.last_chars = ch;
  }

  /* bars and the fade, folded into one number for the HUD (see hud_cine.c) */
  float fade = fmaxf(1 - seg(t, 0.12f, 0.12f + C.fi), seg(t, C.end - C.fo, C.end - 0.08f));
  o->fade = fade;
  o->letterbox = 1 + fade;   /* bars are always full: they come in under the black */
  o->skip = C.skip;
  return true;
}

/* ═════════════ the radio and the quips ═════════════ */
/* Six levels (docs/STORY.md): 0 Lessee, 1 Sublet, 2 Cold Storage, 3 Annex, 4 Basement, 5 Freehold.
 * The Freehold is always the last level in the game, whatever WORLD_LEVELS is, so it is looked up
 * by that rather than by a number. Lines are written to be spoken by the synthesiser: short
 * sentences, numbers as words, no symbols. who: 0 Marguerite, 1 the Superintendent, 2 Juno. */
const char *story_radio(int level, int beat, const char **speaker) {
  static const char *WHO[3] = {"MARGUERITE", "THE SUPERINTENDENT", "JUNO"};
  static const struct { int who; const char *text; } R[6][6] = {
    { {0, "Touchdown confirmed, Juno. The ship's signage is very passive aggressive. Ignore it."},
      {0, "That's a Tenant form. They've filed a noise complaint about you. I'm logging it as received."},
      {0, "The Tenants are stacking colony windows into nests out there. Those are tower four's."},
      {0, "Heat signature ahead. Large, orderly, and not a Tenant. Keep your voice down."},
      {0, "There's a work order on the gate, signed THE SUPERINTENDENT. Remove all non-leaseholders, including glass."},
      {0, 0} },
    { {0, "Touchdown confirmed. It's warm and it smells like soil. I don't love how much I like it."},
      {0, "They just wanted somewhere with a view."},
      {0, "A family nest. They've left a complaint form. It's a drawing of the planet. Coloured in."},
      {0, "Superintendent memo, on the lift wall. The colony is to be relocated. Into the planet. In bold."},
      {0, "Two Brutes this time. Matching. I suppose it's a department."},
      {0, "The Freehold is moving, Juno. It's leaving its orbit. It's heading for the colony."} },
    /* 2 Cold Storage */
    { {0, "Touchdown. Every crate in here is labelled with a delivery date two hundred years ago. All of it is still in transit."},
      {0, "Warm patches ahead. Hibernating Tenants. They came in for the heat. Please step quietly."},
      {2, "A sofa. Still in its wrap. Some assembly required."},
      {0, "That's the heater core. And I have found the original lease, in a filing tube. It expired two hundred years ago, on a Tuesday."},
      {0, "The cargo lift goes up toward the colony. Something is being shipped there. I would like to know what."},
      {0, 0} },
    /* 3 the Annex */
    { {0, "You're down on tower nine. That is your gondola, Juno. Exactly where you left it."},
      {0, "Tenants in the lobby. They're huddled. They are scared, Juno."},
      {0, "A Brute is bolting a turret to tower four. That is a load bearing window."},
      {0, "The crew lockers. Your colleagues left you a thermos and a note. The note says, do not lose the squeegee."},
      {0, "The tram runs to the Freehold's tether. I have a plan. We cut the eviction relay, in the Freehold's basement."},
      {0, 0} },
    /* 4 the Basement */
    { {0, "Touchdown. Filing cabinets, to the horizon. This is the most beautiful thing I have ever seen."},
      {2, "This one says, please may we have a window. It's stamped received."},
      {1, "ATTENTION OCCUPANT. BUILDING RULE ONE. THERE ARE NO OCCUPANTS. BUILDING RULE TWO. SEE RULE ONE."},
      {0, "The relay is cut. The Freehold has stopped moving the colony. Logging that as resolved."},
      {1, "THIS MATTER REQUIRES MANAGEMENT. THE SUPERINTENDENT WILL HANDLE IT IN PERSON. REMAIN WHERE YOU ARE."},
      {0, 0} },
    /* 5 the Freehold: the old level two, unchanged */
    { {0, "Touchdown, night side. The dress code appears to be violet. Please do not touch the furniture."},
      {1, "ALL OCCUPANTS ARE IN BREACH."},
      {1, "THE CROWN IS CLOSED FOR REFURBISHMENT. PLEASE HOLD STILL."},
      {0, "That's the arena gate. There's a notice on the lock. Management will see you now."},
      {0, 0}, {0, 0} },
  };
  *speaker = 0;
  if ((unsigned)level >= WORLD_LEVELS || (unsigned)beat > 5) return 0;
  int k = level == WORLD_LEVELS - 1 ? 5 : level;   /* the last level is the Freehold */
  if (!R[k][beat].text) return 0;
  *speaker = WHO[R[k][beat].who];
  return R[k][beat].text;
}

const char *story_quip(int seed) {
  static const char *Q[12] = {
    "Please keep to the walkways.",
    "Your complaint has been filed under LATER.",
    "Window cleaning is a shared responsibility.",
    "Streaks are not covered by the warranty.",
    "Marguerite has logged that as a structural concern.",
    "The ground is not a customer.",
    "Always check the cable. Then check it again.",
    "Quiet hours are in effect. You were not quiet.",
    "Tenants are reminded that nesting is not permitted. Neither is falling.",
    "A clean window is a dirty window that has not happened yet.",
    "Twelve years without a lost squeegee. Do not lose the squeegee.",
    "Please take a ticket. You are being evicted in the order received.",
  };
  return Q[(unsigned)seed % 12];
}
