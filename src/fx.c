/* fx.c: particles, tracers, flashes and explosions. Three instanced meshes
 * (additive glow quads, additive rings, lit blobs), all pooled as rings of
 * fixed size and rewritten once a frame: no allocation after init. */
#include "fx.h"
#include "world.h"
#include <stdlib.h>
#include <string.h>

enum { K_BILL, K_STREAK, K_LINE, K_FLAT, K_RINGB, K_BLOB, K_SMOKE, K_FORM };

typedef struct {
  v3 p, v, p2;
  float life, max, s0, s1, w0, w1, rot, grav, drag, fy, bright;
  float c[3];
  uint8_t kind, bounce;
} part_t;

typedef struct { t3_instanced_mesh *im; part_t *p; int cap, next; } pool_t;

static pool_t glow, ring, blob, smoke, form;
static v3 cam_r, cam_u, cam_p;
static int frame_no, trail_frame;
static bool trail_ok[2];
static v3 trail_prev[2];

static pool_t pool_make(t3_scene *sc, t3_geometry *g, t3_material *m, int cap) {
  pool_t P = { t3_instanced_mesh_new(g, m, cap), calloc(cap, sizeof(part_t)), cap, 0 };
  t3_instanced_mesh_set_color_at(P.im, 0, t3_color_hex(0xffffff));
  t3_object *o = (t3_object *)P.im;
  o->frustum_culled = false; o->cast_shadow = false; o->receive_shadow = false;
  P.im->count = 0; o->visible = false;
  t3_object_add(&sc->base, P.im);
  t3_release(g); t3_release(m);
  return P;
}

static t3_texture *tex_make(bool ring_shape, bool alpha) {
  enum { N = 128 };
  static uint8_t px[N * N * 4];
  for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
    float dx = (x + .5f) / N * 2 - 1, dy = (y + .5f) / N * 2 - 1, r = sqrtf(dx * dx + dy * dy), v;
    if (ring_shape) { float d = fabsf(r - 0.94f) / 0.05f; v = r > 1 ? 0 : fminf(1, fmaxf(0, 1 - d) * 1.4f); }
    else { v = fmaxf(0, 1 - r); v = v * v * (0.55f + 2.2f * v * v); v = fminf(1, v); }
    uint8_t b = (uint8_t)(v * 255);
    uint8_t *q = px + (y * N + x) * 4; q[0] = q[1] = q[2] = alpha ? 255 : b; q[3] = alpha ? (uint8_t)(b * 0.55f) : 255;
  }
  t3_texture *t = t3_texture_new(N, N, px);
  t->color_space = T3_LINEAR_SRGB_COLOR_SPACE;
  return t;
}

/* a complaint form: cream paper, a red header stamp, grey lines of small print and a box ticked in the corner */
static t3_texture *tex_form(void) {
  enum { W = 48, Hh = 64 };
  static uint8_t px[W * Hh * 4];
  for (int y = 0; y < Hh; y++) for (int x = 0; x < W; x++) {
    uint8_t *q = px + (y * W + x) * 4;
    int edge = x < 1 || y < 1 || x >= W - 1 || y >= Hh - 1;
    float g = 1.f;
    if (!edge) {
      if (y >= 6 && y < 11 && x >= 5 && x < 30) g = 0.62f;                       /* header stamp */
      else if (y >= 16 && y < 56 && (y % 6) < 2 && x >= 5 && x < W - 5 - ((y / 6) % 3) * 6) g = 0.72f;   /* print */
      else if (y >= 5 && y < 12 && x >= W - 14 && x < W - 6) g = (x == W - 14 || x == W - 7 || y == 5 || y == 11) ? 0.5f : 0.9f;
    } else g = 0.78f;
    float r = g, gr = g, b = g;
    if (y >= 6 && y < 11 && x >= 5 && x < 30) { r = 0.95f; gr = 0.42f; b = 0.36f; }
    q[0] = (uint8_t)(255 * r); q[1] = (uint8_t)(255 * gr); q[2] = (uint8_t)(255 * b); q[3] = 255;
  }
  t3_texture *t = t3_texture_new(W, Hh, px);
  t->color_space = T3_LINEAR_SRGB_COLOR_SPACE;
  return t;
}

static t3_material *add_material(t3_texture *t) {
  t3_material *m = t3_mesh_basic_material_new(0xffffff);
  t3_material_set_map(m, t);
  t3_release(t);
  m->transparent = true; m->blending = T3_ADDITIVE_BLENDING; m->depth_write = false; m->side = T3_DOUBLE_SIDE;
  m->fog = false; m->tone_mapped = false; m->version++;
  return m;
}

void fx_init(t3_scene *sc) {
  glow = pool_make(sc, t3_plane_geometry_new(1, 1, 1, 1), add_material(tex_make(false, false)), 420);
  ring = pool_make(sc, t3_plane_geometry_new(2, 2, 1, 1), add_material(tex_make(true, false)), 40);
  t3_material *b = t3_mesh_lambert_material_new(0xffffff);
  blob = pool_make(sc, t3_icosahedron_geometry_new(0.5f, 0), b, 200);
  ((t3_object *)blob.im)->cast_shadow = true;
  t3_material *sm = t3_mesh_basic_material_new(0xffffff);
  t3_material_set_map(sm, tex_make(false, true));
  sm->transparent = true; sm->depth_write = false; sm->side = T3_DOUBLE_SIDE; sm->version++;
  smoke = pool_make(sc, t3_plane_geometry_new(1, 1, 1, 1), sm, 120);
  /* same material state as the smoke, so it costs no new shader program */
  t3_material *fm = t3_mesh_basic_material_new(0xffffff);
  t3_material_set_map(fm, tex_form());
  fm->transparent = true; fm->depth_write = false; fm->side = T3_DOUBLE_SIDE; fm->version++;
  form = pool_make(sc, t3_plane_geometry_new(1, 1, 1, 1), fm, 24);
}

void fx_clear(void) {
  pool_t *ps[5] = { &glow, &ring, &blob, &smoke, &form };
  for (int i = 0; i < 5; i++) { if (ps[i]->p) for (int k = 0; k < ps[i]->cap; k++) ps[i]->p[k].life = 0; ps[i]->im->count = 0; ((t3_object *)ps[i]->im)->visible = false; }
  trail_ok[0] = trail_ok[1] = false;
}

static part_t *emit(pool_t *P, int kind, v3 p, v3 v, float life, float s0, float s1, uint32_t hex) {
  part_t *q = &P->p[P->next]; P->next = (P->next + 1) % P->cap;
  memset(q, 0, sizeof *q);
  q->kind = kind; q->p = p; q->v = v; q->life = q->max = life; q->s0 = s0; q->s1 = s1; q->bright = 1;
  t3_color c = t3_color_hex(hex); q->c[0] = c.r; q->c[1] = c.g; q->c[2] = c.b;
  return q;
}
static v3 rnd_dir(void) {
  v3 d; do { d = v3_(ds_rr(-1, 1), ds_rr(-1, 1), ds_rr(-1, 1)); } while (v3_dot(d, d) > 1 || v3_dot(d, d) < 0.01f);
  return v3_norm(d);
}
static void line(v3 a, v3 b, uint32_t col, float w0, float w1, float life) {
  part_t *q = emit(&glow, K_LINE, a, v3_(0, 0, 0), life, 0, 0, col); q->p2 = b; q->w0 = w0; q->w1 = w1;
}

void fx_tracer(v3 from, v3 to, uint32_t color, float width) {
  line(from, to, color, width, width * 0.5f, 0.08f);
  line(from, to, 0xffffff, width * 0.35f, width * 0.2f, 0.08f);
}
void fx_beam(v3 from, v3 to, uint32_t color, float width) { line(from, to, color, width, width, 0.001f); }
void fx_glow(v3 at, float size, uint32_t color) { emit(&glow, K_BILL, at, v3_(0, 0, 0), 0.001f, size, size, color); }
void fx_ring_flat(v3 at, float radius, uint32_t color) { emit(&ring, K_FLAT, at, v3_(0, 0, 0), 0.001f, radius, radius, color); }

void fx_muzzle(v3 at, v3 dir) {
  part_t *q = emit(&glow, K_BILL, at, v3_(0, 0, 0), 0.06f, 0.9f, 0.5f, 0xffd36a); q->rot = ds_rr(0, 6);
  emit(&glow, K_BILL, at, v3_(0, 0, 0), 0.05f, 0.35f, 0.2f, 0xffffff);
  v3 d = v3_norm(dir);
  for (int i = 0; i < 3; i++) {
    v3 v = v3_scale(v3_add(d, v3_scale(rnd_dir(), 0.25f)), ds_rr(10, 22));
    q = emit(&glow, K_STREAK, at, v, ds_rr(0.05f, 0.1f), 0.07f, 0.03f, 0xffb347); q->w0 = 0.05f;
  }
}

void fx_sparks(v3 at, v3 n, uint32_t color, int count) {
  float fy = world_floor_y(at);
  emit(&glow, K_BILL, at, v3_(0, 0, 0), 0.07f, 0.45f, 0.15f, color);
  for (int i = 0; i < count; i++) {
    v3 v = v3_add(v3_scale(n, ds_rr(2, 7)), v3_scale(rnd_dir(), ds_rr(1, 4)));
    part_t *q = emit(&glow, K_STREAK, at, v, ds_rr(0.25f, 0.6f), 0.06f, 0.02f, color);
    q->grav = 14; q->w0 = 0.04f; q->fy = fy; q->bounce = 1;
  }
}

void fx_goo(v3 at, int count) {
  float fy = world_floor_y(at);
  emit(&glow, K_BILL, at, v3_(0, 0, 0), 0.18f, 1.6f, 0.5f, 0x4a7a2a);
  for (int i = 0; i < count; i++) {
    v3 d = rnd_dir(); d.y = fabsf(d.y) * 1.2f + 0.2f;
    part_t *q = emit(&blob, K_BLOB, at, v3_scale(d, ds_rr(2, 7)), ds_rr(0.7f, 1.4f), ds_rr(0.14f, 0.34f), 0.05f, ds_rand() < 0.3f ? 0xd6ff9a : 0x9be35a);
    q->grav = 16; q->drag = 0.4f; q->fy = fy; q->bounce = 1; q->rot = ds_rr(0.6f, 1.2f);
  }
  for (int i = 0; i < count / 3 + 1; i++) {
    part_t *q = emit(&glow, K_STREAK, at, v3_scale(rnd_dir(), ds_rr(3, 8)), ds_rr(0.2f, 0.45f), 0.07f, 0.03f, 0x9be35a);
    q->grav = 12; q->w0 = 0.07f;
  }
}

void fx_spores(v3 at, uint32_t color, int count) {
  emit(&glow, K_BILL, at, v3_(0, 0, 0), 0.1f, 0.9f, 0.3f, 0xffffff)->bright = 0.7f;
  for (int i = 0; i < count; i++) {
    v3 d = rnd_dir(); d.y = d.y * 0.6f + 0.5f;
    part_t *q = emit(&glow, K_BILL, at, v3_scale(d, ds_rr(1.5f, 4)), ds_rr(0.35f, 0.7f), ds_rr(0.12f, 0.24f), 0.03f, color);
    q->drag = 2.2f; q->rot = ds_rr(0, 6); q->bright = 0.9f;
  }
}

void fx_poof(v3 at, uint32_t color, float size) {
  emit(&glow, K_BILL, at, v3_(0, 0, 0), 0.16f, size * 2.2f, size * 0.8f, 0xffffff)->bright = 0.8f;
  t3_color cc = t3_color_hex(color);
  uint32_t pale = ((uint32_t)(fminf(1, cc.r * 0.5f + 0.5f) * 255) << 16) | ((uint32_t)(fminf(1, cc.g * 0.5f + 0.5f) * 255) << 8) | (uint32_t)(fminf(1, cc.b * 0.5f + 0.5f) * 255);
  for (int i = 0; i < 5; i++) {
    v3 p = v3_add(at, v3_scale(rnd_dir(), size * 0.35f));
    part_t *q = emit(&smoke, K_SMOKE, p, v3_(ds_rr(-0.6f, 0.6f), ds_rr(0.4f, 1.4f), ds_rr(-0.6f, 0.6f)), ds_rr(0.7f, 1.1f), size * 0.55f, size * ds_rr(1.1f, 1.6f), pale);
    q->drag = 1.5f; q->rot = ds_rr(0, 6);
  }
  for (int i = 0; i < 12; i++) {
    v3 d = rnd_dir(); d.y = fabsf(d.y) * 0.8f + 0.3f;
    part_t *q = emit(&glow, K_BILL, at, v3_scale(d, ds_rr(1.5f, 4.5f)), ds_rr(0.5f, 1.0f), ds_rr(0.1f, 0.22f), 0.02f, i & 1 ? color : pale);
    q->drag = 2.4f; q->grav = -1.5f; q->rot = ds_rr(0, 6); q->bright = 0.9f;
  }
}

void fx_form(v3 at) {
  float fy = world_floor_y(at);
  if (fy < -1e8f) fy = at.y - 1.2f;
  float fall = fmaxf(0, at.y - fy) / 1.1f;
  part_t *q = emit(&form, K_FORM, at, v3_(0, 0, 0), fminf(fall, 6) + 4.5f, 0.6f, 0.6f, 0xf2ead8);
  q->fy = fy; q->rot = ds_rr(0, 6.28f); q->w0 = ds_rr(0, 6.28f);
}

void fx_explosion(v3 at, float size) {
  float fy = world_floor_y(at);
  part_t *q = emit(&glow, K_BILL, at, v3_(0, 0, 0), 0.14f, size * 4.f, size * 2.f, 0xfff1c8); q->rot = ds_rr(0, 6);
  for (int i = 0; i < 6; i++) {
    v3 p = v3_add(at, v3_scale(rnd_dir(), size * 0.5f));
    q = emit(&glow, K_BILL, p, v3_(0, size * 1.2f, 0), ds_rr(0.35f, 0.6f), size * ds_rr(1.4f, 2.2f), size * 0.4f, i & 1 ? 0xff7a1a : 0xffb347);
    q->rot = ds_rr(0, 6); q->drag = 2;
  }
  int sparks = 10 + (int)(size * 6);
  for (int i = 0; i < sparks; i++) {
    v3 d = rnd_dir(); d.y = fabsf(d.y) + 0.2f;
    q = emit(&glow, K_STREAK, at, v3_scale(d, ds_rr(6, 16) * (0.6f + size * 0.4f)), ds_rr(0.3f, 0.8f), 0.12f, 0.03f, 0xffb347);
    q->grav = 12; q->w0 = 0.08f; q->fy = fy; q->bounce = 1;
  }
  for (int i = 0; i < 8; i++) {
    v3 d = rnd_dir(); d.y = fabsf(d.y) + 0.3f;
    q = emit(&blob, K_BLOB, at, v3_scale(d, ds_rr(4, 11) * (0.6f + size * 0.3f)), ds_rr(0.8f, 1.5f), size * ds_rr(0.15f, 0.35f), 0.02f, i & 1 ? 0x3a3a40 : 0x6b4a2a);
    q->grav = 22; q->fy = fy; q->bounce = 1; q->rot = ds_rr(0.5f, 1.3f);
  }
  for (int i = 0; i < 6; i++) {
    v3 p = v3_add(at, v3_scale(rnd_dir(), size * 0.6f));
    q = emit(&smoke, K_SMOKE, p, v3_(ds_rr(-1, 1), ds_rr(1.5f, 3.5f), ds_rr(-1, 1)), ds_rr(1.0f, 1.8f), size * 0.8f, size * ds_rr(1.8f, 2.8f), i & 1 ? 0x55555c : 0x3a3a40);
    q->drag = 1.5f; q->rot = ds_rr(0, 6);
  }
  emit(&ring, K_FLAT, v3_(at.x, fy + 0.08f, at.z), v3_(0, 0, 0), 0.4f, size * 0.5f, size * 3.2f, 0xffb347);
}

void fx_ring_pop(v3 at, float radius) {
  emit(&ring, K_RINGB, at, v3_(0, 0, 0), 0.5f, radius * 0.4f, radius * 1.5f, 0x2ec4b6);
  emit(&ring, K_RINGB, at, v3_(0, 0, 0), 0.35f, radius * 0.2f, radius * 1.0f, 0xffffff);
  emit(&glow, K_BILL, at, v3_(0, 0, 0), 0.2f, radius * 0.8f, radius * 0.2f, 0x9ff3ea);
  for (int i = 0; i < 14; i++) {
    v3 d = rnd_dir();
    part_t *q = emit(&glow, K_STREAK, v3_add(at, v3_scale(d, radius * 0.6f)), v3_scale(d, ds_rr(4, 10)), ds_rr(0.3f, 0.6f), 0.1f, 0.02f, i & 1 ? 0x2ec4b6 : 0xff7a1a);
    q->w0 = 0.07f; q->drag = 1.5f;
  }
}

void fx_glide_trail(v3 l, v3 r) {
  v3 now[2] = { l, r };
  for (int i = 0; i < 2; i++) {
    if (trail_ok[i] && trail_frame == frame_no - 1) {
      part_t *q = emit(&glow, K_LINE, trail_prev[i], v3_(0, 0, 0), 0.7f, 0, 0, 0xcfeeff);
      q->p2 = now[i]; q->w0 = 0.16f; q->w1 = 0.01f; q->bright = 0.55f;
    }
    trail_prev[i] = now[i]; trail_ok[i] = true;
  }
  trail_frame = frame_no;
}

/* write one instance: columns x, y, z axes and translation */
static void put(pool_t *P, int i, v3 x, v3 y, v3 z, v3 p, const float c[3], float k) {
  float *m = (float *)P->im->instance_matrix->array + i * 16;
  m[0] = x.x; m[1] = x.y; m[2] = x.z; m[3] = 0;
  m[4] = y.x; m[5] = y.y; m[6] = y.z; m[7] = 0;
  m[8] = z.x; m[9] = z.y; m[10] = z.z; m[11] = 0;
  m[12] = p.x; m[13] = p.y; m[14] = p.z; m[15] = 1;
  t3_color col = { c[0] * k, c[1] * k, c[2] * k };
  t3_instanced_mesh_set_color_at(P->im, i, col);
}

static void run_pool(pool_t *P, float dt) {
  int n = 0;
  for (int k = 0; k < P->cap; k++) {
    part_t *q = &P->p[k];
    if (q->life <= 0) continue;
    float t = 1 - q->life / q->max, fade = 1 - t;
    float s = ds_lerp(q->s0, q->s1, t);
    v3 view = v3_norm(v3_sub(q->p, cam_p));
    switch (q->kind) {
    case K_SMOKE: {
      float sc = s * fminf(1, fade * 2.5f), cr = cosf(q->rot) * sc, sr = sinf(q->rot) * sc;
      put(P, n++, v3_add(v3_scale(cam_r, cr), v3_scale(cam_u, sr)), v3_add(v3_scale(cam_r, -sr), v3_scale(cam_u, cr)), view, q->p, q->c, 1);
      break; }
    case K_BILL: case K_RINGB: {
      float cr = cosf(q->rot), sr = sinf(q->rot);
      v3 x = v3_add(v3_scale(cam_r, cr * s), v3_scale(cam_u, sr * s)), y = v3_add(v3_scale(cam_r, -sr * s), v3_scale(cam_u, cr * s));
      put(P, n++, x, y, v3_scale(view, 1), q->p, q->c, fade * fade * q->bright);
      break; }
    case K_FLAT:
      put(P, n++, v3_(s, 0, 0), v3_(0, 0, s), v3_(0, 1, 0), q->p, q->c, fade * q->bright);
      break;
    case K_STREAK: case K_LINE: {
      v3 a, b;
      if (q->kind == K_LINE) { a = q->p; b = q->p2; }
      else { float sp = v3_len(q->v); a = q->p; b = v3_sub(q->p, v3_scale(q->v, (fminf(sp * 0.035f, 1.5f) + s) / fmaxf(sp, 0.01f))); }
      v3 d = v3_sub(b, a); float L = v3_len(d);
      if (L < 1e-4f) { b = v3_add(a, v3_scale(cam_r, 0.01f)); d = v3_sub(b, a); L = 0.01f; }
      v3 dn = v3_scale(d, 1 / L), side = v3_norm(v3_cross(dn, view));
      float w = ds_lerp(q->w0, q->w1 > 0 ? q->w1 : q->w0 * 0.3f, t);
      v3 mid = v3_scale(v3_add(a, b), 0.5f);
      put(P, n++, v3_scale(d, 1), v3_scale(side, w), view, mid, q->c, fade * q->bright);
      break; }
    case K_FORM: {
      float age = q->max - q->life;
      if (!q->bounce) {
        /* drifts down and sways like paper; the last drops land flat */
        q->p.y -= 1.1f * dt;
        q->p.x += sinf(age * 3.1f + q->w0) * 0.9f * dt; q->p.z += cosf(age * 2.3f + q->w0) * 0.7f * dt;
        q->rot += sinf(age * 2 + q->w0) * 1.2f * dt;
        if (q->p.y <= q->fy + 0.03f) { q->p.y = q->fy + 0.03f; q->bounce = 1; }
      }
      float a = q->bounce ? 0 : 0.9f * sinf(age * 5 + q->w0), sc = s * fminf(1, q->life / 0.6f);
      v3 R = v3_(cosf(q->rot), 0, sinf(q->rot)), Fh = v3_(-sinf(q->rot), 0, cosf(q->rot));
      v3 nm = v3_add(v3_(0, cosf(a), 0), v3_scale(Fh, sinf(a)));
      put(P, n++, v3_scale(R, sc * 0.75f), v3_scale(v3_cross(nm, R), sc), nm, q->p, q->c, 1);
      break; }
    case K_BLOB: {
      float sc = s * fminf(1, fade * 4);
      float cr = cosf(t * 6 + k), sr = sinf(t * 6 + k), asp = q->rot;
      put(P, n++, v3_(cr * sc, 0, -sr * sc), v3_(0, sc * asp, 0), v3_(sr * sc, 0, cr * sc), q->p, q->c, 1);
      break; }
    }
    /* age after drawing, so a one-frame effect is drawn once */
    q->life -= dt;
    if (q->kind != K_LINE && q->kind != K_FLAT && q->kind != K_RINGB && q->kind != K_FORM) {
      q->v.y -= q->grav * dt;
      if (q->drag > 0) { float f = expf(-q->drag * dt); q->v = v3_scale(q->v, f); }
      q->p = v3_add(q->p, v3_scale(q->v, dt));
      if (q->bounce && q->p.y < q->fy + 0.03f && q->v.y < 0) { q->p.y = q->fy + 0.03f; q->v.y *= -0.35f; q->v.x *= 0.6f; q->v.z *= 0.6f; }
    }
  }
  P->im->count = n;
  ((t3_object *)P->im)->visible = n > 0;
  P->im->instance_matrix->version++;
  if (P->im->instance_color) P->im->instance_color->version++;
}

void fx_update(float dt, const t3_camera *cam) {
  frame_no++;
  const float *m = cam->base.matrix_world.e;
  cam_r = v3_(m[0], m[1], m[2]); cam_u = v3_(m[4], m[5], m[6]); cam_p = v3_(m[12], m[13], m[14]);
  run_pool(&glow, dt); run_pool(&ring, dt); run_pool(&blob, dt); run_pool(&smoke, dt); run_pool(&form, dt);
}
