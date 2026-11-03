/* cine_set.c: the kit the cutscene sets are built from, the staged actors and
 * the sets themselves (Halcyon Reach, the Sublet's gardens, the Freehold).
 * Everything lives far from the playable levels and is only shown while a
 * cutscene runs (cutscene.c toggles the roots). */
#include "cine_int.h"
#include "batch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static t3_scene *scn;
halc_t HA;
subl_t SU;
bay_t BAY;
free_t FR;

static float fr(int i) { return (float)((uint32_t)(i * 2654435761u) >> 8 & 0xffff) / 65535.f; }
static void hexk(cv *c, uint32_t rgb, float k) {
  int r = (int)(((rgb >> 16) & 255) * k), g = (int)(((rgb >> 8) & 255) * k), b = (int)((rgb & 255) * k);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  cv_fill_hex(c, (uint32_t)(r << 16 | g << 8 | b), 1);
}

/* ── the kit ── */
t3_texture *k_paint(int w, int h, void (*fn)(cv *, int, int), bool repeat) {
  cv *c = cv_new(w, h);
  fn(c, w, h);
  uint8_t *px = malloc((size_t)w * h * 4);
  cv_read(c, px);
  cv_free(c);
  t3_texture *t = t3_texture_new(w, h, px);
  free(px);
  t->color_space = T3_SRGB_COLOR_SPACE;
  if (repeat) t->wrap_s = t->wrap_t = T3_REPEAT;
  t->anisotropy = 4;
  return t;
}
t3_material *km_basic(uint32_t col, bool fog) {
  t3_material *m = t3_mesh_basic_material_new(col);
  m->fog = fog;
  return m;
}
t3_material *km_std(uint32_t col, float rough, float metal, uint32_t emis, float ei) {
  t3_material *m = t3_mesh_standard_material_new(col);
  m->roughness = rough; m->metalness = metal;
  if (ei > 0) { m->emissive = t3_color_hex(emis); m->emissive_intensity = ei; }
  return m;
}
t3_material *km_tex(t3_texture *tex, bool basic, bool fog) {
  t3_material *m = basic ? t3_mesh_basic_material_new(0xffffff) : t3_mesh_standard_material_new(0xffffff);
  if (!basic) { m->roughness = 0.8f; m->metalness = 0.1f; }
  t3_material_set_map(m, tex); t3_release(tex);
  m->fog = fog;
  return m;
}
static t3_mesh *put_mesh(t3_object *par, t3_geometry *g, t3_material *m, float x, float y, float z) {
  t3_mesh *me = t3_mesh_new(g, m);
  t3_release(g);
  T3_OBJ(me)->frustum_culled = false;
  t3_object_set_position(me, x, y, z);
  t3_object_add(par, me); t3_release(me);
  return me;
}
t3_mesh *k_box(t3_object *par, t3_material *m, float x, float y, float z, float w, float h, float d) {
  return put_mesh(par, t3_box_geometry_new(w, h, d, 1, 1, 1), m, x, y, z);
}
t3_mesh *k_cyl(t3_object *par, t3_material *m, float x, float y, float z, float rtop, float rbot, float h, int seg) {
  return put_mesh(par, t3_cylinder_geometry_new(rtop, rbot, h, seg, 1, false), m, x, y, z);
}
t3_mesh *k_sph(t3_object *par, t3_material *m, float x, float y, float z, float r) {
  return put_mesh(par, t3_sphere_geometry_new(r, 24, 14), m, x, y, z);
}
t3_mesh *k_plane(t3_object *par, t3_material *m, float x, float y, float z, float w, float h, float u0, float v0, float u1, float v1) {
  t3_geometry *g = t3_plane_geometry_new(w, h, 1, 1);
  float *uv = t3_attribute_f32(g->attributes[T3_ATTR_UV]);
  for (int i = 0; i < g->attributes[T3_ATTR_UV]->count; i++) {
    uv[i * 2] = u0 + uv[i * 2] * (u1 - u0);
    uv[i * 2 + 1] = v0 + uv[i * 2 + 1] * (v1 - v0);
  }
  return put_mesh(par, g, m, x, y, z);
}
t3_object *k_group(t3_object *par, float x, float y, float z) {
  t3_object *g = t3_object_new();
  t3_object_set_position(g, x, y, z);
  if (par) { t3_object_add(par, g); t3_release(g); }
  return g;
}
void k_look(t3_object *o, v3 from, v3 to) {
  t3_object_set_position(o, from.x, from.y, from.z);
  t3_object_look_at(o, to.x, to.y, to.z);
}

/* static models from assets/models, parsed once and instanced as shared parts */
typedef struct { t3_geometry *g; t3_material **ms; int nm; t3_vec3 pos, sc; t3_quat q; } part_t;
typedef struct { char file[32]; part_t *parts; int np, cap; float lo[3], hi[3]; bool ok; } kmodel_t;
static kmodel_t kmodels[24];
static int nkmodels;

static void part_cb(t3_object *o, void *ctx) {
  kmodel_t *k = ctx;
  if (o->type != T3_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  if (!m->geometry || !m->geometry->attributes[T3_ATTR_POSITION]) return;
  if (k->np == k->cap) { k->cap = k->cap ? k->cap * 2 : 8; k->parts = realloc(k->parts, k->cap * sizeof(part_t)); }
  part_t *p = &k->parts[k->np++];
  p->g = m->geometry; p->nm = m->material_count;
  p->ms = malloc(sizeof(t3_material *) * (p->nm ? p->nm : 1));
  for (int i = 0; i < p->nm; i++) {
    p->ms[i] = m->materials[i];
    if (p->ms[i]) { p->ms[i]->metalness = 0.1f; p->ms[i]->roughness = 0.75f; p->ms[i]->version++; }
  }
  t3_mat4_decompose(&o->matrix_world, &p->pos, &p->q, &p->sc);
}
static kmodel_t *kmodel(const char *file) {
  for (int i = 0; i < nkmodels; i++) if (!strcmp(kmodels[i].file, file)) return &kmodels[i];
  if (nkmodels >= 24) return NULL;
  kmodel_t *k = &kmodels[nkmodels++];
  memset(k, 0, sizeof *k);
  snprintf(k->file, sizeof k->file, "%s", file);
  size_t n; uint8_t *b = ds_asset(file, &n);
  if (!b) return k;
  t3_gltf *gl = t3_gltf_parse(b, n, NULL, NULL);
  free(b);
  if (!gl) return k;
  t3_object_update_matrix_world(gl->scene, true);
  t3_object_traverse(gl->scene, part_cb, k);
  float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
  for (int i = 0; i < k->np; i++) {
    part_t *p = &k->parts[i];
    t3_geometry_compute_bounding_box(p->g);
    t3_box3 bb = p->g->bounding_box;
    for (int c = 0; c < 8; c++) {
      float v[3] = {c & 1 ? bb.max.x : bb.min.x, c & 2 ? bb.max.y : bb.min.y, c & 4 ? bb.max.z : bb.min.z};
      v[0] *= p->sc.x; v[1] *= p->sc.y; v[2] *= p->sc.z;
      t3_quat q = p->q;
      float ux = q.y * v[2] - q.z * v[1], uy = q.z * v[0] - q.x * v[2], uz = q.x * v[1] - q.y * v[0];
      float r[3] = {v[0] + 2 * (q.w * ux + q.y * uz - q.z * uy) + p->pos.x, v[1] + 2 * (q.w * uy + q.z * ux - q.x * uz) + p->pos.y,
                    v[2] + 2 * (q.w * uz + q.x * uy - q.y * ux) + p->pos.z};
      for (int a = 0; a < 3; a++) { if (r[a] < lo[a]) lo[a] = r[a]; if (r[a] > hi[a]) hi[a] = r[a]; }
    }
  }
  float cx = (lo[0] + hi[0]) / 2, cz = (lo[2] + hi[2]) / 2;
  for (int i = 0; i < k->np; i++) { k->parts[i].pos.x -= cx; k->parts[i].pos.z -= cz; k->parts[i].pos.y -= lo[1]; }
  k->lo[0] = lo[0] - cx; k->lo[1] = 0; k->lo[2] = lo[2] - cz;
  k->hi[0] = hi[0] - cx; k->hi[1] = hi[1] - lo[1]; k->hi[2] = hi[2] - cz;
  k->ok = k->np > 0;
  return k;
}
t3_object *k_model(t3_object *par, const char *file, float height, float x, float y, float z, float yaw) {
  kmodel_t *k = kmodel(file);
  t3_object *grp = k_group(par, x, y, z);
  if (!k || !k->ok) return grp;
  float s = k->hi[1] > 1e-4f ? height / k->hi[1] : 1;
  t3_object_set_scale(grp, s, s, s);
  t3_object_set_rotation(grp, 0, yaw, 0);
  for (int i = 0; i < k->np; i++) {
    part_t *p = &k->parts[i];
    t3_mesh *me = p->nm > 1 ? t3_mesh_new_multi(p->g, p->ms, p->nm) : t3_mesh_new(p->g, p->ms[0]);
    t3_object_set_position(me, p->pos.x, p->pos.y, p->pos.z);
    t3_object_set_quaternion(me, p->q);
    t3_object_set_scale(me, p->sc.x, p->sc.y, p->sc.z);
    T3_OBJ(me)->frustum_culled = false;
    t3_object_add(grp, me); t3_release(me);
  }
  return grp;
}

/* ── staged actors ── */
static void actor_mesh_cb(t3_object *o, void *ctx) {
  (void)ctx;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  o->frustum_culled = false; o->cast_shadow = true; o->receive_shadow = true;
  t3_mesh *m = (t3_mesh *)o;
  for (int i = 0; i < m->material_count; i++) {
    t3_material *M = m->materials[i];
    if (!M) continue;
    M->metalness = 0.15f; M->roughness = 0.65f;
    M->version++;
  }
}
bool act_load(act_t *a, t3_object *parent, const char *file, float scale) {
  memset(a, 0, sizeof *a);
  size_t n; uint8_t *b = ds_asset(file, &n);
  if (!b) return false;
  a->gltf = t3_gltf_parse(b, n, NULL, NULL);
  free(b);
  if (!a->gltf) { ds_log("cine: bad %s", file); return false; }
  a->root = t3_object_new();
  a->model = a->gltf->scene;
  t3_object_add(a->root, a->model);
  a->scale = scale;
  t3_object_set_scale(a->model, scale, scale, scale);
  t3_object_traverse(a->model, actor_mesh_cb, NULL);
  a->mixer = t3_animation_mixer_new(a->model);
  t3_object_add(parent, a->root); t3_release(a->root);
  a->root->visible = false;
  a->live = true;
  return true;
}
static t3_animation_clip *act_clip(act_t *a, const char *name) {
  for (int i = 0; i < a->gltf->animation_count; i++) {
    const char *n = a->gltf->animations[i]->name, *bar = strrchr(n, '|');
    if (!strcmp(bar ? bar + 1 : n, name)) return a->gltf->animations[i];
  }
  return NULL;
}
float act_len(act_t *a, const char *clip) { t3_animation_clip *c = a->live ? act_clip(a, clip) : NULL; return c ? c->duration : 1; }
void act_play(act_t *a, const char *name, float speed) {
  if (!a->live) return;
  t3_animation_clip *c = act_clip(a, name);
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
void act_pose(act_t *a, const char *name, float t01) {
  if (!a->live) return;
  t3_animation_clip *c = act_clip(a, name);
  if (!c) return;
  t3_animation_action *act = t3_animation_mixer_clip_action(a->mixer, c);
  if (act != a->cur) {
    if (a->cur) t3_animation_action_stop(a->cur);
    if (a->prev) { t3_animation_action_stop(a->prev); a->prev = NULL; }
    a->cur = act;
    t3_animation_action_play(act);
  }
  t3_animation_action_set_effective_time_scale(act, 0);
  t3_animation_action_set_effective_weight(act, 1);
  act->time = ds_clamp(t01, 0, 1) * (c->duration - 0.001f);
  a->fade = 1;
  t3_animation_mixer_update(a->mixer, 0.0001f);
}
void act_tick(act_t *a, float dt) {
  if (!a->live || !a->root->visible || !a->cur) return;
  a->fade = fminf(1, a->fade + dt * 8);
  t3_animation_action_set_effective_weight(a->cur, a->fade);
  if (a->prev) {
    t3_animation_action_set_effective_weight(a->prev, 1 - a->fade);
    if (a->fade >= 1) { t3_animation_action_stop(a->prev); a->prev = NULL; }
  }
  t3_animation_mixer_update(a->mixer, dt);
}
void act_put(act_t *a, v3 p, float yaw, float scale_mul, bool visible) {
  if (!a->live) return;
  a->root->visible = visible;
  t3_object_set_position(a->root, p.x, p.y, p.z);
  t3_object_set_rotation(a->root, 0, yaw, 0);
  float s = a->scale * scale_mul;
  t3_object_set_scale(a->model, s, s, s);
}
static void tint_cb(t3_object *o, void *ctx) {
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  uint32_t *v = ctx;
  t3_mesh *m = (t3_mesh *)o;
  for (int i = 0; i < m->material_count; i++) if (m->materials[i]) {
    t3_material *M = m->materials[i];
    M->color = t3_color_hex(v[0]); M->emissive = t3_color_hex(v[1]); M->emissive_intensity = *(float *)&v[2]; M->version++;
  }
}
void act_tint(act_t *a, uint32_t col, uint32_t emis, float ei) {
  if (!a->live) return;
  uint32_t v[3] = {col, emis, 0};
  memcpy(&v[2], &ei, 4);
  t3_object_traverse(a->model, tint_cb, v);
}

/* ═════════════ textures ═════════════ */
/* a tower face: 4 x 4 panes of sky-reflecting glass, some lit rooms */
static void tx_tower(cv *c, int w, int h) {
  int n = 4, pw = w / n, ph = h / n;
  cv_fill_hex(c, 0x8e99a6, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++) {
      float f = fr(j * 7 + i * 13 + 3);
      float x = (float)i * pw + 3, y = (float)j * ph + 3, ww = (float)pw - 6, hh = (float)ph - 6;
      if (f < 0.9f) {
        cv_fill_linear(c, 0, y, 0, y + hh);
        cv_fill_stop(c, 0, "#bcd6ec"); cv_fill_stop(c, 0.55f, "#7fa8cf"); cv_fill_stop(c, 1, "#5f8bb6");
        cv_fill_rect(c, x, y, ww, hh);
        cv_alpha(c, 0.28f); cv_fill_hex(c, 0xffffff, 1);
        cv_begin(c); cv_move(c, x + ww * 0.15f, y + hh); cv_line(c, x + ww * 0.45f, y); cv_line(c, x + ww * 0.62f, y); cv_line(c, x + ww * 0.32f, y + hh); cv_close(c); cv_fill(c);
        cv_alpha(c, 1);
      } else if (f < 0.95f) {
        cv_fill_hex(c, 0x6a6470, 1); cv_fill_rect(c, x, y, ww, hh);
        cv_fill_hex(c, 0xffe2b8, 0.22f); cv_fill_rect(c, x + ww * 0.1f, y + hh * 0.12f, ww * 0.8f, hh * 0.62f);
      } else {
        cv_fill_linear(c, 0, y, 0, y + hh);
        cv_fill_stop(c, 0, "#24384c"); cv_fill_stop(c, 1, "#16283a");
        cv_fill_rect(c, x, y, ww, hh);
      }
    }
}
/* the pane being washed: two clean panes, or the same two under grime */
static void tx_work_pane(cv *c, int w, int h, bool dirty) {
  cv_fill_hex(c, 0x98a3b0, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int i = 0; i < 2; i++) {
    float x = (float)i * w / 2 + 5, y = 5, ww = (float)w / 2 - 10, hh = (float)h - 10;
    cv_fill_linear(c, 0, y, 0, y + hh);
    cv_fill_stop(c, 0, "#d7e8f5"); cv_fill_stop(c, 0.5f, "#9cc0de"); cv_fill_stop(c, 1, "#7da6cb");
    cv_fill_rect(c, x, y, ww, hh);
    cv_alpha(c, 0.3f); cv_fill_hex(c, 0xffffff, 1);
    cv_begin(c); cv_move(c, x + ww * 0.1f, y + hh); cv_line(c, x + ww * 0.4f, y); cv_line(c, x + ww * 0.55f, y); cv_line(c, x + ww * 0.25f, y + hh); cv_close(c); cv_fill(c);
    cv_alpha(c, 1);
  }
  if (!dirty) return;
  for (int i = 0; i < 70; i++) {   /* grime blotches */
    float x = fr(i * 5 + 1) * w, y = fr(i * 7 + 2) * h, r = 8 + fr(i * 3) * 34;
    cv_fill_hex(c, fr(i) < 0.5f ? 0x6b6256 : 0x7d7a74, 1);
    cv_alpha(c, 0.10f + fr(i * 11) * 0.16f);
    cv_begin(c); cv_ellipse(c, x, y, r, r * 0.7f, fr(i) * 3, 0, 6.3f, false); cv_fill(c);
  }
  for (int i = 0; i < 26; i++) {   /* drip streaks */
    float x = fr(i * 13 + 4) * w, y = fr(i * 17) * h * 0.5f, l = 40 + fr(i * 19) * 140;
    cv_alpha(c, 0.30f); cv_fill_hex(c, 0x5a5348, 1);
    cv_fill_rect(c, x, y, 2.5f, l);
  }
  cv_alpha(c, 1);
  cv_fill_hex(c, 0x98a3b0, 1); cv_fill_rect(c, (float)w / 2 - 5, 0, 10, (float)h);
}
static void tx_work_dirty(cv *c, int w, int h) { tx_work_pane(c, w, h, true); }
static void tx_work_clean(cv *c, int w, int h) { tx_work_pane(c, w, h, false); }
/* the morning sky, equirect: cold blue above, peach at the rim, sun glow low */
static void tx_sky_morning(cv *c, int w, int h) {
  cv_fill_linear(c, 0, 0, 0, (float)h);
  cv_fill_stop(c, 0, "#16408a"); cv_fill_stop(c, 0.30f, "#3a78c4"); cv_fill_stop(c, 0.44f, "#8fbbe3");
  cv_fill_stop(c, 0.495f, "#ffc79a"); cv_fill_stop(c, 0.5f, "#ffcf9c"); cv_fill_stop(c, 0.52f, "#a9b9d2"); cv_fill_stop(c, 1, "#5a77a6");
  cv_fill_rect(c, 0, 0, (float)w, (float)h);
  float sx = w * 0.31f, sy = h * 0.49f;
  cv_fill_radial(c, sx, sy, 4, sx, sy, h * 0.34f);
  cv_fill_stop(c, 0, "rgba(255,246,222,0.95)"); cv_fill_stop(c, 0.18f, "rgba(255,226,180,0.55)"); cv_fill_stop(c, 1, "rgba(255,210,160,0)");
  cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_alpha(c, 0.35f); cv_fill_hex(c, 0xffffff, 1);
  for (int i = 0; i < 18; i++) {   /* thin high cloud */
    cv_begin(c); cv_ellipse(c, fr(i * 3) * w, h * (0.18f + 0.26f * fr(i * 7)), 60 + fr(i) * 120, 3 + fr(i * 5) * 5, 0, 0, 6.3f, false); cv_fill(c);
  }
  cv_alpha(c, 1);
}
static void tx_planet(cv *c, int w, int h) {
  cv_fill_linear(c, 0, 0, 0, (float)h);
  cv_fill_stop(c, 0, "#24507f"); cv_fill_stop(c, 0.5f, "#2b6fa5"); cv_fill_stop(c, 1, "#1f4a78");
  cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int i = 0; i < 90; i++) {
    float x = fr(i * 3 + 1) * w, y = (0.15f + fr(i * 5 + 4) * 0.7f) * h, r = 10 + fr(i) * 50;
    hexk(c, fr(i * 2 + 9) < 0.5f ? 0x4f8a50 : 0x8a7b52, 1);
    cv_alpha(c, 0.85f);
    for (int k = 0; k < 4; k++) { cv_begin(c); cv_ellipse(c, x + k * r * 0.5f, y + (k & 1) * r * 0.3f, r, r * 0.5f, 0, 0, 6.3f, false); cv_fill(c); }
    cv_alpha(c, 1);
  }
  cv_alpha(c, 0.6f);
  for (int i = 0; i < 130; i++) {
    cv_fill_hex(c, 0xffffff, 1);
    cv_begin(c); cv_ellipse(c, fr(i * 7 + 2) * w, fr(i * 11 + 6) * h, 24 + fr(i) * 90, 3 + fr(i * 3) * 9, 0, 0, 6.3f, false); cv_fill(c);
  }
  cv_alpha(c, 1);
}
/* a Tenant form in crayon: the planet, coloured in, and a stamp */
static void tx_form(cv *c, int w, int h) {
  cv_fill_hex(c, 0xf2ead8, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, 0x101218, 0.8f); cv_fill_rect(c, 14, 14, w - 28.f, 30);
  cv_fill_hex(c, 0x101218, 0.35f);
  for (int i = 0; i < 5; i++) cv_fill_rect(c, 20, 200.f + i * 18, w - 40.f, 2);
  cv_fill_hex(c, 0x2b6fa5, 1); cv_begin(c); cv_arc(c, w / 2.f, 120, 58, 0, 6.3f, false); cv_fill(c);
  cv_fill_hex(c, 0x4f8a50, 1); cv_begin(c); cv_ellipse(c, w / 2.f - 14, 112, 24, 15, 0.5f, 0, 6.3f, false); cv_fill(c);
  cv_begin(c); cv_ellipse(c, w / 2.f + 22, 140, 18, 10, -0.3f, 0, 6.3f, false); cv_fill(c);
  cv_stroke_hex(c, 0x9be35a, 1); cv_line_width(c, 4);
  cv_begin(c); cv_arc(c, w / 2.f, 120, 62, 0, 6.3f, false); cv_stroke(c);
  cv_stroke_hex(c, 0xe8433a, 0.85f); cv_line_width(c, 5);
  cv_begin(c); cv_arc(c, w - 70.f, h - 50.f, 30, 0, 6.3f, false); cv_stroke(c);
  cv_font(c, F_DISPLAY, 18); cv_letter_spacing(c, 2); cv_text_align(c, CV_LEFT);
  cv_fill_hex(c, 0xf2ead8, 1); cv_fill_text(c, "FORM 31-T", 22, 36);
}

/* ═════════════ Halcyon Reach ═════════════ */
static t3_material *M_orange, *M_dark, *M_cable, *M_frame, *M_teal;

static void tower(t3_object *par, t3_material *face, t3_material *side, float x0, float x1, float ybot, float ytop, float uvx, float uvy) {
  float w = x1 - x0, h = ytop - ybot, cx = (x0 + x1) / 2, cy = (ybot + ytop) / 2;
  k_plane(par, face, cx, cy, 0, w, h, 0, 0, w / uvx, h / uvy);
  t3_mesh *s = k_plane(par, side, x0, cy, -35, 70, h, 0, 0, 70 / uvx, h / uvy);
  t3_object_set_rotation(s, 0, -DS_PI / 2, 0);
  s = k_plane(par, side, x1, cy, -35, 70, h, 0, 0, 70 / uvx, h / uvy);
  t3_object_set_rotation(s, 0, DS_PI / 2, 0);
  s = k_plane(par, M_dark, cx, ytop, -35, w, 70, 0, 0, 1, 1);
  t3_object_set_rotation(s, -DS_PI / 2, 0, 0);
}

static t3_material *flat(uint32_t c) { return km_basic(c, false); }
static void ship_lessee(t3_object *par) {
  /* the outermost hull: long, flat, all decks, solar farms and signage (hull grey, hazard orange, sodium light) */
  k_box(par, flat(0x222a3a), 0, 0, 0, 700, 130, 200);
  k_box(par, flat(0x2d3748), 0, 78, -10, 620, 26, 150);
  k_box(par, flat(0xb85a1a), 0, -50, 100.6f, 700, 10, 1);
  for (int i = 0; i < 5; i++) k_box(par, flat(0xffc070), -20, -32 + i * 20.f, 100.8f, 600 - i * 70, 3, 1);
  for (int i = 0; i < 12; i++) k_box(par, flat(0x23406e), -300 + i * 54.f, 94, -10, 44, 3, 140);
  for (int i = 0; i < 4; i++) k_box(par, flat(0x1a2130), -240 + i * 160.f, 135, 40, 10, 90, 10);
  k_box(par, flat(0xe8433a), 320, 100, 100, 6, 6, 6);
}
static void ship_sublet(t3_object *par) {
  /* a long cylindrical spine banded with glass and grow light */
  t3_mesh *spine = k_cyl(par, flat(0x15302b), 0, 0, 0, 105, 105, 760, 24);
  t3_object_set_rotation(spine, 0, 0, DS_PI / 2);
  for (int i = 0; i < 8; i++) {
    t3_mesh *r = k_cyl(par, flat(i & 1 ? 0x2ec4b6 : 0xd8509a), -320 + i * 92.f, 0, 0, 107, 107, 10, 24);
    t3_object_set_rotation(r, 0, 0, DS_PI / 2);
  }
  for (int i = 0; i < 3; i++) {
    t3_mesh *c = k_cyl(par, flat(0x101a26), -250 + i * 250.f, 0, 0, 140, 140, 18, 16);
    t3_object_set_rotation(c, 0, 0, DS_PI / 2);
  }
  k_box(par, flat(0x0c1612), 420, 0, 0, 70, 150, 150);
  k_box(par, flat(0x7fe3d8), 0, 108, 0, 700, 4, 30);
}
static void ship_freehold(t3_object *par) {
  /* the flagship: a squat black hull, a tower and, on its crown, the cranes */
  k_box(par, flat(0x16132a), 0, 0, 0, 640, 140, 260);
  k_box(par, flat(0x1d1a36), 0, 220, 0, 230, 300, 190);
  for (int i = 0; i < 8; i++) k_box(par, flat(0x9a8cd0), 0, 100 + i * 30.f, 95.6f, 190, 3, 1);
  for (int i = 0; i < 46; i++) k_box(par, flat(fr(i * 3) < 0.5f ? 0xffd89a : 0x9a8cd0), -105 + fr(i * 7) * 210, 80 + fr(i * 11) * 280, 95.8f, 6, 7, 1);
  for (int i = 0; i < 7; i++) {
    t3_object *g = k_group(par, -90 + i * 30.f, 370, (i & 1) * 20 - 10.f);
    t3_object_set_rotation(g, 0, 0, (i - 3) * 0.14f);
    k_box(g, flat(0x120f20), 0, 90, 0, 8, 180, 8);
    k_box(g, flat(0xe8433a), 0, 184, 0, 7, 7, 7);
  }
}

/* a washer's cradle on two cables of the given length (uses the Halcyon materials, built first) */
static t3_object *gondola_build(t3_object *par, float x, float y, float z, float cable_h, bool bucket) {
  t3_object *G = k_group(par, x, y, z);
  k_box(G, M_dark, 0, -0.1f, 0, 4.4f, 0.2f, 1.5f);
  for (int s = -1; s <= 1; s += 2) {
    k_box(G, M_orange, s * 2.15f, 0.55f, 0, 0.1f, 1.3f, 1.4f);
    k_box(G, M_orange, 0, 1.1f, s * 0.7f, 4.4f, 0.08f, 0.08f);
    k_box(G, M_orange, 0, 0.55f, s * 0.7f, 4.4f, 0.06f, 0.06f);
  }
  for (int s = -1; s <= 1; s += 2) {
    k_cyl(G, M_cable, s * 2.0f, cable_h / 2, -0.3f, 0.03f, 0.03f, cable_h, 5);
    k_box(G, M_dark, s * 2.0f, 1.0f, -0.3f, 0.22f, 0.5f, 0.22f);
  }
  k_box(G, M_teal, 1.5f, 0.3f, -0.3f, 0.7f, 0.5f, 0.5f);          /* the winch box */
  if (bucket) k_cyl(G, km_std(0x3a7fa8, 0.5f, 0.1f, 0, 0), -1.5f, 0.28f, 0.2f, 0.22f, 0.18f, 0.36f, 12);   /* the bucket */
  k_box(G, M_frame, -0.6f, 0.06f, 0.3f, 0.5f, 0.12f, 0.35f);
  return G;
}

static t3_material *flat_(uint32_t c) { return km_basic(c, false); }
static void halcyon_build(void) {
  HA.origin = v3_(0, 3000, 0);
  HA.root = k_group(&scn->base, HA.origin.x, HA.origin.y, HA.origin.z);
  t3_object *R = HA.root;
  M_orange = km_std(C_HIVIS, 0.55f, 0.1f, 0, 0);
  M_dark = km_std(0x2a2f3a, 0.7f, 0.2f, 0, 0);
  M_cable = km_basic(0x20242d, true);
  M_frame = km_std(0xe4e8ee, 0.4f, 0.3f, 0, 0);
  M_teal = km_std(C_TEAL, 0.5f, 0.1f, C_TEAL, 0.4f);

  /* sky, planet and its haze */
  t3_geometry *sg = t3_sphere_geometry_new(3900, 32, 16);
  t3_material *skym = km_tex(k_paint(1024, 512, tx_sky_morning, false), true, false);
  skym->side = T3_BACK_SIDE; skym->depth_write = false;
  HA.dome = (t3_object *)put_mesh(R, sg, skym, 0, 0, 0);
  T3_OBJ(HA.dome)->render_order = -10;
  t3_geometry *pg = t3_sphere_geometry_new(2600, 56, 28);
  t3_material *pm = km_tex(k_paint(1024, 512, tx_planet, false), true, false);
  HA.planet = (t3_object *)put_mesh(R, pg, pm, 150, -2790, 3050);
  t3_object_set_rotation(HA.planet, DS_PI / 2, 0.6f, 0);
  t3_material *hm = km_basic(0x9fc8ff, false);
  hm->transparent = true; hm->opacity = 0.22f; hm->side = T3_BACK_SIDE; hm->depth_write = false;
  HA.halo = (t3_object *)put_mesh(R, t3_sphere_geometry_new(2700, 48, 24), hm, 150, -2790, 3050);

  /* the ring itself: a pale arch of glass curving over the sky, lit windows along its inner face */
  { t3_material *rm = km_basic(0xd7e1ee, false);
    t3_mesh *ring = put_mesh(R, t3_torus_geometry_new(2200, 26, 8, 90, 1.3f), rm, 0, -1500, 500);
    t3_object_set_rotation(ring, 0, 0, DS_PI / 2 - 0.65f);
    t3_mesh *inner = put_mesh(R, t3_torus_geometry_new(2188, 12, 6, 90, 1.3f), km_basic(0xffe0a8, false), 0, -1500, 500);
    t3_object_set_rotation(inner, 0, 0, DS_PI / 2 - 0.65f); }
  /* the towers, all facing +z; tower 9 is ours */
  t3_material *face = km_tex(k_paint(256, 256, tx_tower, true), true, true);
  t3_material *side = km_tex(k_paint(256, 256, tx_tower, true), true, true);
  static const float tw[][4] = {{-210, -150, 300, 420}, {-130, -70, 380, 330}, {-50, 10, 400, 400}, {30, 90, 330, 350}, {110, 170, 450, 300}};
  (void)tw;
  tower(R, face, side, -30, 30, -420, 410, 9.6f, 14.4f);      /* tower 9 */
  tower(R, face, side, -110, -45, -420, 300, 9.6f, 14.4f);    /* 10 */
  tower(R, face, side, 45, 110, -420, 360, 9.6f, 14.4f);      /* 8 */
  tower(R, face, side, 125, 190, -420, 470, 9.6f, 14.4f);     /* 7 */
  tower(R, face, side, 205, 270, -420, 330, 9.6f, 14.4f);     /* 6 */
  tower(R, face, side, -190, -125, -420, 440, 9.6f, 14.4f);   /* 11 */

  /* the work pane on tower 9 and the strips of clean glass that appear as it is wiped */
  t3_material *dirty = km_tex(k_paint(256, 192, tx_work_dirty, false), true, true);
  t3_material *clean = km_tex(k_paint(256, 192, tx_work_clean, false), true, true);
  k_plane(R, dirty, 0, 2.2f, 0.03f, 4.8f, 3.6f, 0, 0, 1, 1);
  for (int i = 0; i < 8; i++) {
    HA.strip[i] = (t3_object *)k_plane(R, clean, -2.4f + (i + 0.5f) * 0.6f, 2.2f, 0.05f, 0.6f, 3.6f, i / 8.f, 0, (i + 1) / 8.f, 1);
    HA.strip[i]->visible = false;
  }

  /* tower 8's window, the one the Tenants take: a clean double pane in a frame, over a dark recess */
  const float px = 76, py = 4;
  HA.hole = (t3_object *)k_plane(R, km_basic(0x0d1522, true), px, py, 0.04f, 4.8f, 3.6f, 0, 0, 1, 1);
  HA.panel = k_group(R, px, py, 0.12f);
  k_plane(HA.panel, clean, 0, 0, 0, 4.8f, 3.6f, 0, 0, 1, 1);
  k_box(HA.panel, M_frame, 0, 1.85f, 0.03f, 5.0f, 0.2f, 0.18f);
  k_box(HA.panel, M_frame, 0, -1.85f, 0.03f, 5.0f, 0.2f, 0.18f);
  k_box(HA.panel, M_frame, -2.5f, 0, 0.03f, 0.2f, 3.7f, 0.18f);
  k_box(HA.panel, M_frame, 2.5f, 0, 0.03f, 0.2f, 3.7f, 0.18f);
  k_box(HA.panel, M_frame, 0, 0, 0.03f, 0.12f, 3.6f, 0.14f);
  /* the same window, carried back in the ending, is a second copy */
  HA.window_back = k_group(R, 0, 0, 0);
  k_plane(HA.window_back, clean, 0, 0, 0, 4.8f, 3.6f, 0, 0, 1, 1);
  k_box(HA.window_back, M_frame, 0, 1.85f, 0.03f, 5.0f, 0.2f, 0.18f);
  k_box(HA.window_back, M_frame, 0, -1.85f, 0.03f, 5.0f, 0.2f, 0.18f);
  k_box(HA.window_back, M_frame, -2.5f, 0, 0.03f, 0.2f, 3.7f, 0.18f);
  k_box(HA.window_back, M_frame, 2.5f, 0, 0.03f, 0.2f, 3.7f, 0.18f);
  HA.window_back->visible = false;

  HA.gondola = gondola_build(R, 0, 0, 1.45f, 300, true);   /* a washer's cradle on two cables */

  /* the squeegee: a pole and a blade, held by the director at Juno's hand */
  /* the Tenants' thank-you form: a sheet with a drawing of the planet */
  HA.form = k_group(R, 0, -50, 0);
  { t3_material *fm = km_tex(k_paint(256, 320, tx_form, false), true, true); fm->side = T3_DOUBLE_SIDE;
    k_plane(HA.form, fm, 0, 0, 0, 0.62f, 0.78f, 0, 0, 1, 1); }
  HA.form->visible = false;
  HA.squeegee = k_group(R, 0, 0, 0);
  k_box(HA.squeegee, M_dark, 0, 0.55f, 0, 0.035f, 1.1f, 0.035f);
  k_box(HA.squeegee, M_teal, 0, 1.12f, 0, 0.42f, 0.05f, 0.07f);
  HA.squeegee->visible = false;

  /* the Tenancies, dark against the sunrise */
  const float sx[3] = {-1500, -30, 1450}, sz[3] = {3000, 3250, 3050};
  for (int i = 0; i < 3; i++) {
    HA.ship[i] = k_group(R, sx[i], -600, sz[i]);
    if (i == 0) ship_lessee(HA.ship[i]); else if (i == 1) ship_sublet(HA.ship[i]); else ship_freehold(HA.ship[i]);
    t3_object_set_scale(HA.ship[i], 2.1f, 2.1f, 2.1f);
    t3_object_set_rotation(HA.ship[i], 0, i == 0 ? 0.45f : i == 1 ? -0.3f : 0.25f, 0);
    HA.ship[i]->visible = false;
  }
  R->visible = false;
}

/* ═════════════ the Sublet's gardens ═════════════ */
static void tx_dusk(cv *c, int w, int h) {
  cv_fill_linear(c, 0, 0, 0, (float)h);
  cv_fill_stop(c, 0, "#0e2a44"); cv_fill_stop(c, 0.32f, "#1f5f66"); cv_fill_stop(c, 0.46f, "#4d9a8c");
  cv_fill_stop(c, 0.5f, "#f0a0b4"); cv_fill_stop(c, 0.53f, "#7a4a7a"); cv_fill_stop(c, 1, "#1a2040");
  cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int i = 0; i < 160; i++) {
    cv_fill_hex(c, 0xffffff, 0.2f + 0.6f * fr(i * 5));
    cv_fill_rect(c, fr(i * 3 + 1) * w, fr(i * 7 + 2) * h * 0.42f, 1.5f + fr(i) * 1.5f, 1.5f + fr(i) * 1.5f);
  }
}
static void tx_nest(cv *c, int w, int h) {
  cv_fill_linear(c, 0, 0, 0, (float)h);
  cv_fill_stop(c, 0, "#a8e6dc"); cv_fill_stop(c, 0.6f, "#d8a0c8"); cv_fill_stop(c, 1, "#ff9ac8");
  cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, 0xffffff, 0.22f);
  cv_begin(c); cv_move(c, w * 0.15f, (float)h); cv_line(c, w * 0.45f, 0); cv_line(c, w * 0.6f, 0); cv_line(c, w * 0.3f, (float)h); cv_close(c); cv_fill(c);
  cv_stroke_hex(c, 0xcfd8dc, 1); cv_line_width(c, 8); cv_stroke_rect(c, 4, 4, (float)w - 8, (float)h - 8);
}
static void island(t3_object *par, float x, float z, float w, float d, uint32_t top) {
  k_box(par, km_std(0x4a3a2c, 0.9f, 0, 0, 0), x, -1.5f, z, w, 3, d);
  k_box(par, km_std(top, 0.9f, 0, 0, 0), x, 0.06f, z, w - 0.6f, 0.2f, d - 0.6f);
  k_cyl(par, km_std(0x3a2e26, 0.95f, 0, 0, 0), x, -9.0f, z, w * 0.42f, w * 0.06f, 12, 9);
}
static t3_object *nest(t3_object *par, float x, float z, t3_material *pane, float sc) {
  t3_object *n = k_group(par, x, 0, z);
  t3_object_set_scale(n, sc, sc, sc);
  for (int i = 0; i < 7; i++) {
    float a = (float)i * 2 * DS_PI / 7 + 0.2f;
    t3_object *g = k_group(n, 0, 0, 0);
    t3_object_set_rotation(g, 0, a, 0);
    t3_mesh *p = k_plane(g, pane, 0, 1.8f, 2.3f, 2.4f, 3.6f, 0, 0, 1, 1);
    t3_object_set_rotation(p, -0.32f, 0, 0);
  }
  k_sph(n, km_basic(0xff8ac8, false), 0, 1.3f, 0, 0.35f);
  return n;
}
static void sublet_build(void) {
  SU.origin = v3_(4000, 3000, 0);
  SU.root = k_group(&scn->base, SU.origin.x, SU.origin.y, SU.origin.z);
  t3_object *R = SU.root;
  t3_material *skym = km_tex(k_paint(1024, 512, tx_dusk, false), true, false);
  skym->side = T3_BACK_SIDE; skym->depth_write = false;
  SU.dome = (t3_object *)put_mesh(R, t3_sphere_geometry_new(3900, 32, 16), skym, 0, 0, 0);
  T3_OBJ(SU.dome)->render_order = -10;
  t3_material *pane = km_tex(k_paint(128, 192, tx_nest, false), true, true);
  pane->side = T3_DOUBLE_SIDE;
  /* the spine: three drifting beds, nests of stacked colony windows, grow lamps */
  island(R, 0, 0, 46, 54, 0x4f8f3c);
  island(R, -42, 24, 26, 26, 0x468a4a);
  island(R, 38, 40, 30, 34, 0x5a9a44);
  island(R, -10, 78, 34, 30, 0x4a8f46);
  SU.nests = k_group(R, 0, 0, 0);
  nest(SU.nests, 6, 8, pane, 1.0f);
  nest(SU.nests, -14, -10, pane, 0.8f);
  nest(SU.nests, 38, 38, pane, 1.1f);
  nest(SU.nests, -42, 22, pane, 0.7f);
  nest(SU.nests, -8, 76, pane, 0.9f);
  static const char *trees[] = {"w_treef.glb", "w_trees.glb", "w_treel.glb", "w_treef.glb"};
  for (int i = 0; i < 16; i++) {
    float x = -20 + fr(i * 5 + 1) * 40, z = -22 + fr(i * 9 + 2) * 44;
    if (fabsf(x - 6) < 5 && fabsf(z - 8) < 5) continue;
    if (fabsf(x + 14) < 5 && fabsf(z + 10) < 5) continue;
    if (fabsf(x + 4) < 7 && fabsf(z + 8) < 7) continue;    /* the clearing where Juno stands */
    k_model(R, trees[i & 3], 3 + fr(i * 3) * 3.5f, x, 0.1f, z, fr(i) * 6);
  }
  for (int i = 0; i < 30; i++) {
    float x = -21 + fr(i * 13 + 4) * 42, z = -24 + fr(i * 11 + 6) * 48;
    if (fabsf(x + 4) < 6 && fabsf(z + 8) < 6) continue;
    k_model(R, i % 3 ? "w_plant.glb" : "w_plant2.glb", 1.0f + fr(i) * 1.3f, x, 0.1f, z, fr(i * 2) * 6);
  }
  for (int i = 0; i < 10; i++) k_model(R, "w_bush.glb", 1.1f, 36 + fr(i) * 14 - 7, 0.1f, 36 + fr(i * 3) * 18 - 9, fr(i) * 6);
  for (int i = 0; i < 8; i++) k_model(R, i & 1 ? "w_treef.glb" : "w_trees.glb", 3.5f + fr(i) * 3, 28 + fr(i * 5) * 20, 0.1f, 28 + fr(i * 7) * 24, fr(i));
  for (int i = 0; i < 6; i++) k_model(R, "w_treef.glb", 3.5f + fr(i) * 2.5f, -50 + fr(i * 3) * 16, 0.1f, 14 + fr(i * 5) * 20, fr(i * 2));
  for (int i = 0; i < 5; i++) {   /* grow lamps on poles */
    float x = -12 + i * 8.f, z = 18 - (i & 1) * 36.f;
    k_box(R, km_std(0x2a2f3a, 0.7f, 0.2f, 0, 0), x, 3.5f, z, 0.15f, 7, 0.15f);
    k_box(R, km_basic(0xff8ac8, false), x, 7.1f, z, 2.2f, 0.18f, 0.5f);
  }
  /* the Freehold, far off, lit */
  SU.freehold = k_group(R, -900, 150, 3100);
  ship_freehold(SU.freehold);
  t3_object_set_scale(SU.freehold, 2.6f, 2.6f, 2.6f);
  t3_object_set_rotation(SU.freehold, 0, DS_PI + 0.35f, 0);
  R->visible = false;
}

/* ═════════════ the Freehold at night ═════════════ */
static void tx_night(cv *c, int w, int h) {
  cv_fill_linear(c, 0, 0, 0, (float)h);
  cv_fill_stop(c, 0, "#03030c"); cv_fill_stop(c, 0.35f, "#0d0a28"); cv_fill_stop(c, 0.48f, "#2c1a56");
  cv_fill_stop(c, 0.5f, "#5a3a8a"); cv_fill_stop(c, 0.52f, "#150e30"); cv_fill_stop(c, 1, "#06050f");
  cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int i = 0; i < 420; i++) {
    cv_fill_hex(c, 0xffffff, 0.25f + 0.75f * fr(i * 5));
    float sz = 1.2f + fr(i) * 1.8f;
    cv_fill_rect(c, fr(i * 3 + 1) * w, fr(i * 7 + 2) * h * 0.49f, sz, sz);
  }
}
static void tx_deck_night(cv *c, int w, int h) {
  cv_fill_hex(c, 0x22223a, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_stroke_hex(c, 0x3a3a60, 1); cv_line_width(c, 3);
  for (int r = 40; r < w / 2; r += 52) { cv_begin(c); cv_arc(c, w / 2.f, h / 2.f, (float)r, 0, 6.3f, false); cv_stroke(c); }
  cv_stroke_hex(c, 0xe8433a, 0.8f); cv_line_width(c, 6);
  cv_begin(c); cv_arc(c, w / 2.f, h / 2.f, w * 0.46f, 0, 6.3f, false); cv_stroke(c);
  cv_stroke_hex(c, 0x9a8cd0, 0.5f); cv_line_width(c, 4);
  cv_begin(c); cv_move(c, 0, h / 2.f); cv_line(c, (float)w, h / 2.f); cv_stroke(c);
  cv_begin(c); cv_move(c, w / 2.f, 0); cv_line(c, w / 2.f, (float)h); cv_stroke(c);
}
static t3_object *furniture(t3_object *par, int kind) {
  t3_object *g = k_group(par, 0, 0, 0);
  t3_material *wood = km_std(0x6a4a3a, 0.8f, 0.1f, 0, 0), *metal = km_std(0x6b7384, 0.5f, 0.5f, 0, 0), *seat = km_std(0x7a3a4a, 0.8f, 0.1f, 0, 0);
  if (kind == 0) {   /* a desk */
    k_box(g, wood, 0, 3, 0, 8, 0.5f, 4);
    for (int i = 0; i < 4; i++) k_box(g, wood, (i & 1 ? 3.6f : -3.6f), 1.4f, (i & 2 ? 1.6f : -1.6f), 0.5f, 3, 0.5f);
    k_box(g, wood, 2.4f, 2.0f, 0, 2.6f, 1.6f, 3.4f);
  } else if (kind == 1) {   /* a swivel chair */
    k_box(g, seat, 0, 2.2f, 0, 2.6f, 0.5f, 2.6f);
    k_box(g, seat, 0, 3.8f, -1.2f, 2.6f, 3, 0.4f);
    k_cyl(g, metal, 0, 1.2f, 0, 0.2f, 0.2f, 2, 8);
    for (int i = 0; i < 5; i++) { t3_mesh *m = k_box(g, metal, 0, 0.3f, 0, 3, 0.2f, 0.3f); t3_object_set_rotation(m, 0, i * 1.257f, 0); }
  } else if (kind == 2) {   /* a filing cabinet */
    k_box(g, metal, 0, 3, 0, 2.6f, 6, 3);
    for (int i = 0; i < 4; i++) k_box(g, km_std(0x3a4252, 0.6f, 0.4f, 0, 0), 0, 1 + i * 1.4f, 1.52f, 2.2f, 0.12f, 0.1f);
  } else {   /* a desk lamp */
    k_cyl(g, metal, 0, 0.3f, 0, 1.2f, 1.4f, 0.6f, 12);
    k_box(g, metal, 0, 3, 0, 0.3f, 5.5f, 0.3f);
    k_box(g, km_basic(0xfff0c0, false), 1.2f, 5.6f, 0, 2.6f, 0.4f, 1.6f);
  }
  return g;
}
static void freehold_build(void) {
  FR.origin = v3_(-4000, 3000, 0);
  FR.root = k_group(&scn->base, FR.origin.x, FR.origin.y, FR.origin.z);
  t3_object *R = FR.root;
  t3_material *skym = km_tex(k_paint(1024, 512, tx_night, false), true, false);
  skym->side = T3_BACK_SIDE; skym->depth_write = false;
  FR.dome = (t3_object *)put_mesh(R, t3_sphere_geometry_new(3900, 32, 16), skym, 0, 0, 0);
  T3_OBJ(FR.dome)->render_order = -10;
  /* the planet's night limb far below */
  t3_material *pm = km_tex(k_paint(1024, 512, tx_planet, false), true, false);
  pm->color = t3_color_hex(0x30406a);
  t3_object *pl = (t3_object *)put_mesh(R, t3_sphere_geometry_new(2600, 56, 28), pm, 300, -2900, 2000);
  t3_object_set_rotation(pl, DS_PI / 2, 2.0f, 0);
  /* the crown: a ring arena on the hull */
  FR.arena = k_group(R, 0, 0, 0);
  t3_material *deck = km_tex(k_paint(512, 512, tx_deck_night, false), false, true);
  t3_mesh *disc = k_cyl(FR.arena, deck, 0, -2, 0, 90, 90, 4, 48);
  (void)disc;
  k_cyl(FR.arena, km_std(0x15152a, 0.8f, 0.3f, 0, 0), 0, -30, 0, 112, 70, 56, 36);
  k_cyl(FR.arena, km_std(0x0f0f20, 0.9f, 0.3f, 0, 0), 0, -90, 0, 70, 20, 70, 24);
  k_box(R, km_std(0x4a4478, 0.6f, 0.3f, 0, 0), 0, 40, -150, 110, 90, 70);       /* the Superintendent's office */
  for (int i = 0; i < 6; i++) k_box(R, km_basic(0xffd89a, true), -45 + i * 18.f, 56, -114.6f, 12, 9, 0.4f);
  k_box(R, km_basic(0xe8433a, true), 0, 90, -114.6f, 40, 4, 0.4f);
  for (int i = 0; i < 24; i++) {   /* the arena's rim lights */
    float a = (float)i * 2 * DS_PI / 24;
    k_box(R, km_basic(i % 3 ? 0x9a8cd0 : 0xe8433a, true), sinf(a) * 89, 0.4f, cosf(a) * 89, 2, 0.8f, 2);
  }
  /* eight cranes on the rim: pivot, boom, a second joint with its jib and a red lamp */
  t3_material *crane_m = km_std(0x5a4a8a, 0.5f, 0.4f, 0, 0), *rim_m = km_basic(0x9a8cd0, true);
  for (int i = 0; i < 8; i++) {
    float a = (float)i * 2 * DS_PI / 8 + 0.2f;
    t3_object *base = k_group(R, sinf(a) * 80, 1, cosf(a) * 80);
    t3_object_set_rotation(base, 0, a - DS_PI / 2, 0);   /* local +x points outward */
    k_cyl(base, crane_m, 0, 1, 0, 4, 5, 3, 10);
    FR.crane[i] = k_group(base, 0, 2, 0);
    k_box(FR.crane[i], crane_m, 0, 30, 0, 5.5f, 60, 5.5f);
    k_box(FR.crane[i], rim_m, 0, 30, 2.2f, 0.6f, 56, 0.4f);
    FR.jib[i] = k_group(FR.crane[i], 0, 60, 0);
    k_box(FR.jib[i], crane_m, 0, 20, 0, 4, 40, 4);
    k_box(FR.jib[i], rim_m, 0, 20, 1.7f, 0.5f, 36, 0.4f);
    k_cyl(FR.jib[i], km_std(0xff8a7a, 0.4f, 0, 0xe8433a, 1.2f), 0, 41, 0, 1.2f, 1.2f, 2, 8);
    FR.lamp[i] = k_group(FR.jib[i], 0, 42, 0);
  }
  for (int i = 0; i < 10; i++) {
    FR.furn[i] = furniture(R, i % 4);
    t3_object_set_scale(FR.furn[i], 1.4f, 1.4f, 1.4f);
    FR.furn[i]->visible = false;
  }
  /* the lighthouse beam of the ending: a long pale cone from the tower's lamp */
  t3_material *bm = km_basic(0xfff3c8, false);
  bm->transparent = true; bm->opacity = 0.2f; bm->depth_write = false;
  FR.beam = k_group(R, 0, 120, -150);
  t3_mesh *bc = k_cyl(FR.beam, bm, 0, 0, 450, 3, 40, 900, 14);
  t3_object_set_rotation(bc, DS_PI / 2, 0, 0);
  k_sph(FR.beam, km_basic(0xfff3c8, false), 0, 0, 0, 5);
  FR.beam->visible = false;
  R->visible = false;
}

/* ═════════════ the maintenance bay: a service hatch in the floor, placed by the director ═════════════ */
static void bay_build(void) {
  BAY.root = k_group(&scn->base, 0, -500, 0);
  t3_object *R = BAY.root;
  t3_material *haz = km_std(0xd8a21a, 0.6f, 0.2f, 0, 0), *steel = km_std(0x4a5262, 0.5f, 0.5f, 0, 0), *dark = km_std(0x07080c, 0.9f, 0, 0, 0);
  k_box(R, dark, 0, -0.1f, 0, 6.2f, 0.1f, 7.2f);
  k_box(R, haz, -3.35f, 0.08f, 0, 0.5f, 0.2f, 8.2f);
  k_box(R, haz, 3.35f, 0.08f, 0, 0.5f, 0.2f, 8.2f);
  k_box(R, haz, 0, 0.08f, -4.1f, 7.2f, 0.2f, 0.5f);
  k_box(R, haz, 0, 0.08f, 4.1f, 7.2f, 0.2f, 0.5f);
  for (int i = 0; i < 2; i++) {   /* the two doors slide apart along x */
    BAY.plate[i] = k_group(R, 0, 0, 0);
    k_box(BAY.plate[i], steel, i ? 1.5f : -1.5f, 0.1f, 0, 3.0f, 0.28f, 7.0f);
    for (int k = 0; k < 4; k++) k_box(BAY.plate[i], haz, i ? 0.25f : -0.25f, 0.26f, -2.6f + k * 1.75f, 0.5f, 0.04f, 0.8f);
  }
  BAY.lamp = k_group(R, 0, 0, 0);
  for (int i = 0; i < 4; i++) k_box(BAY.lamp, km_basic(0xff9a2a, true), (i & 1 ? 3.6f : -3.6f), 0.5f, (i & 2 ? 4.2f : -4.2f), 0.4f, 1.0f, 0.4f);
  R->visible = false;
}

/* a floor or ceiling plane: k_plane faces +z, so lay it flat (up) with its height along -z */
static t3_mesh *k_floor(t3_object *par, t3_material *m, float x, float y, float z, float w, float d, float uvx, float uvz) {
  t3_mesh *p = k_plane(par, m, x, y, z, w, d, 0, 0, w / uvx, d / uvz);
  t3_object_set_rotation(p, -DS_PI / 2, 0, 0);
  return p;
}
/* a wall plane facing +x (yaw PI/2) or -x (yaw -PI/2) */
static t3_mesh *k_wall(t3_object *par, t3_material *m, float x, float y, float z, float len, float h, float yaw, float uvx, float uvy) {
  t3_mesh *p = k_plane(par, m, x, y, z, len, h, 0, 0, len / uvx, h / uvy);
  t3_object_set_rotation(p, 0, yaw, 0);
  return p;
}

/* ═════════════ Cold Storage: the Lessee's freezer holds ═════════════ */
cold_t CO;
static t3_material *M_wrap;

static void tx_frost_floor(cv *c, int w, int h) {
  cv_fill_hex(c, 0x6c8ba2, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, 0x5f7f97, 1); cv_fill_rect(c, 0, 0, w / 2.f, h / 2.f); cv_fill_rect(c, w / 2.f, h / 2.f, w / 2.f, h / 2.f);
  cv_stroke_hex(c, 0x3a5468, 1); cv_line_width(c, 3); cv_stroke_rect(c, 1.5f, 1.5f, w - 3.f, h - 3.f);
  for (int i = 0; i < 36; i++) {   /* drifts of frost */
    cv_fill_hex(c, 0xeaf6ff, 0.08f + 0.16f * fr(i));
    cv_begin(c); cv_ellipse(c, fr(i * 5 + 1) * w, fr(i * 7 + 2) * h, 10 + fr(i * 3) * 34, 4 + fr(i * 11) * 14, fr(i) * 3, 0, 6.3f, false); cv_fill(c);
  }
  for (int i = 0; i < 200; i++) { cv_fill_hex(c, 0xffffff, 0.2f + 0.5f * fr(i * 13)); cv_fill_rect(c, fr(i * 3 + 4) * w, fr(i * 9 + 1) * h, 1.5f, 1.5f); }
}
static void tx_cold_wall(cv *c, int w, int h) {
  cv_fill_hex(c, 0x56707f, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, 0x3f5766, 1);
  for (int i = 0; i < 8; i++) cv_fill_rect(c, i * w / 8.f, 0, 4, (float)h);
  cv_fill_linear(c, 0, 0, 0, (float)h);   /* frost creeping up from the floor */
  cv_fill_stop(c, 0, "rgba(220,240,255,0)"); cv_fill_stop(c, 0.55f, "rgba(220,240,255,0.08)"); cv_fill_stop(c, 1, "rgba(235,248,255,0.55)");
  cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int i = 0; i < 90; i++) { cv_fill_hex(c, 0xffffff, 0.2f + 0.4f * fr(i * 7)); cv_fill_rect(c, fr(i * 3) * w, h * (0.4f + 0.6f * fr(i * 5)), 2, 2); }
}
static void tx_banner(cv *c, int w, int h) {
  cv_fill_hex(c, 0x1a2530, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, 0xd8a21a, 1); cv_fill_rect(c, 0, 0, (float)w, 8); cv_fill_rect(c, 0, h - 8.f, (float)w, 8);
  cv_font(c, F_DISPLAY, 26); cv_letter_spacing(c, 1); cv_text_align(c, CV_CENTER); cv_text_baseline(c, CV_MIDDLE);
  cv_fill_hex(c, 0xffe9a8, 1); cv_fill_text(c, "HELD IN CASE THEY ARRIVE", w / 2.f, h / 2.f - 12);
  cv_font(c, F_MONO, 17); cv_letter_spacing(c, 1);
  cv_fill_hex(c, 0xcfd8dc, 0.9f); cv_fill_text(c, "OWNERS' PROPERTY. DO NOT UNWRAP.", w / 2.f, h / 2.f + 22);
  for (int i = 0; i < 80; i++) { cv_fill_hex(c, 0xffffff, 0.3f); cv_fill_rect(c, fr(i * 3) * w, fr(i * 7) * h, 2, 2); }
}
/* a delivery tag, brown with age and furred with frost */
static void tag_paint(cv *c, int w, int h, const char *l1, const char *l2, const char *l3, const char *l4) {
  cv_fill_hex(c, 0xcfc29a, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_linear(c, 0, 0, (float)w, (float)h);
  cv_fill_stop(c, 0, "rgba(90,60,30,0.35)"); cv_fill_stop(c, 0.5f, "rgba(90,60,30,0)"); cv_fill_stop(c, 1, "rgba(90,60,30,0.4)");
  cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, 0xa8342a, 1); cv_fill_rect(c, 0, 0, (float)w, 30);
  cv_font(c, F_DISPLAY, 20); cv_letter_spacing(c, 3); cv_text_align(c, CV_LEFT); cv_text_baseline(c, CV_MIDDLE);
  cv_fill_hex(c, 0xf2e8d0, 1); cv_fill_text(c, "DELIVERY TAG", 12, 16);
  cv_font(c, F_MONO, 16); cv_letter_spacing(c, 0);
  cv_fill_hex(c, 0x2a2118, 1);
  cv_fill_text(c, l1, 12, 58); cv_fill_text(c, l2, 12, 84); cv_fill_text(c, l3, 12, 110); cv_fill_text(c, l4, 12, 136);
  cv_stroke_hex(c, 0xa8342a, 0.8f); cv_line_width(c, 3); cv_begin(c); cv_arc(c, w - 46.f, h - 40.f, 26, 0, 6.3f, false); cv_stroke(c);
  for (int i = 0; i < 70; i++) { cv_fill_hex(c, 0xffffff, 0.25f + 0.4f * fr(i * 5)); cv_fill_rect(c, fr(i * 3 + 2) * w, fr(i * 7 + 1) * h, 2, 2); }
}
static void tx_tag_a(cv *c, int w, int h) { tag_paint(c, w, h, "TO: THE OWNERS", "SUITE 1. SOFA, 3 SEAT", "DELIVERED YEAR 212", "SIGNED: SUPERINTENDENT"); }
static void tx_tag_b(cv *c, int w, int h) { tag_paint(c, w, h, "TO: THE OWNERS", "SUITE 1. ARMCHAIR", "DELIVERED YEAR 212", "STATUS: IN TRANSIT"); }

/* a piece of the owners' furniture in its wrap; faces +z. kind 0 sofa, 1 armchair, 2 table, 3 crate */
static t3_object *wrapped(t3_object *par, int kind, float x, float z, float yaw, uint32_t hero) {
  t3_object *g = k_group(par, x, 0, z);
  t3_object_set_rotation(g, 0, yaw, 0);
  static const uint32_t col[4] = {0x7a3a4a, 0x3a6a70, 0x9a7a2a, 0x6a4a3a};
  t3_material *m = km_std(hero ? hero : col[(kind + (int)(fr((int)(x * 7 + z * 3)) * 4)) & 3], 0.8f, 0.05f, 0, 0);
  float W = 1, D = 1, H = 1;
  if (kind == 0) {
    k_box(g, m, 0, 0.4f, 0, 3.0f, 0.8f, 1.4f); k_box(g, m, 0, 1.1f, -0.5f, 3.0f, 0.9f, 0.4f);
    k_box(g, m, -1.4f, 0.8f, 0.1f, 0.3f, 0.5f, 1.2f); k_box(g, m, 1.4f, 0.8f, 0.1f, 0.3f, 0.5f, 1.2f);
    W = 3.3f; D = 1.6f; H = 1.8f;
  } else if (kind == 1) {
    k_box(g, m, 0, 0.4f, 0, 1.4f, 0.8f, 1.3f); k_box(g, m, 0, 1.1f, -0.5f, 1.4f, 0.9f, 0.3f);
    W = 1.6f; D = 1.5f; H = 1.8f;
  } else if (kind == 2) {
    k_box(g, m, 0, 1.0f, 0, 2.2f, 0.15f, 1.2f);
    for (int i = 0; i < 4; i++) k_box(g, m, i & 1 ? 0.95f : -0.95f, 0.5f, i & 2 ? 0.45f : -0.45f, 0.12f, 1.0f, 0.12f);
    W = 2.4f; D = 1.4f; H = 1.2f;
  } else {
    k_box(g, km_std(0x8a6a44, 0.9f, 0, 0, 0), 0, 0.6f, 0, 1.6f, 1.2f, 1.2f);
    W = 1.7f; D = 1.3f; H = 1.3f;
  }
  t3_mesh *wr = k_box(g, M_wrap, 0, H / 2, 0, W, H, D);
  (void)wr;
  return g;
}
static void rack(t3_object *par, t3_material *steel, float x, float z, int side, int seed) {
  for (int i = 0; i < 4; i++) k_box(par, steel, x + (i & 1 ? 1.7f : -1.7f), 4.6f, z + (i & 2 ? 6.1f : -6.1f), 0.22f, 9.2f, 0.22f);
  for (int s = 0; s < 3; s++) {
    float y = 0.1f + s * 3.0f;
    k_box(par, steel, x, y, z, 3.6f, 0.16f, 12.4f);
    for (int k = 0; k < 3; k++) {
      float f = fr(seed * 31 + s * 7 + k * 3);
      if (f < 0.12f) continue;
      t3_object_set_position(wrapped(par, (int)(f * 4) & 3, x, z + (k - 1) * 4.0f, side < 0 ? DS_PI / 2 : -DS_PI / 2, 0), x, y + 0.08f, z + (k - 1) * 4.0f);
    }
  }
}
static void cold_build(void) {
  CO.origin = v3_(0, 3000, 4000);
  CO.root = k_group(&scn->base, CO.origin.x, CO.origin.y, CO.origin.z);
  t3_object *R = CO.root;
  M_wrap = km_std(0xdcecf6, 0.25f, 0, 0, 0);
  M_wrap->transparent = true; M_wrap->opacity = 0.3f; M_wrap->depth_write = false;
  t3_material *steel = km_std(0x7e8c99, 0.5f, 0.5f, 0, 0);
  t3_material *floor_m = km_tex(k_paint(256, 256, tx_frost_floor, true), false, true);
  t3_material *wall_m = km_tex(k_paint(256, 256, tx_cold_wall, true), true, true);
  wall_m->side = T3_DOUBLE_SIDE;
  k_floor(R, floor_m, 0, 0, 100, 42, 220, 5.0f, 5.0f);
  k_wall(R, wall_m, -20, 7, 100, 220, 14, DS_PI / 2, 6, 7);
  k_wall(R, wall_m, 20, 7, 100, 220, 14, -DS_PI / 2, 6, 7);
  t3_mesh *bw = k_plane(R, wall_m, 0, 7, 207, 42, 14, 0, 0, 7, 2); (void)bw;
  t3_mesh *fw = k_plane(R, wall_m, 0, 7, -6, 42, 14, 0, 0, 7, 2); (void)fw;
  /* the ceiling girders, pipes and long lamps; no ceiling, so the dark above is the fog */
  t3_material *gird = km_std(0x34424e, 0.7f, 0.4f, 0, 0), *ice = km_std(0xdaf0ff, 0.2f, 0, 0xbfe6ff, 0.3f), *lampm = km_basic(0xd8f0ff, false);
  for (int i = 0; i < 16; i++) {
    float z = 6 + i * 13.f;
    k_box(R, gird, 0, 13.6f, z, 40, 0.5f, 0.8f);
    k_box(R, lampm, -6, 13.2f, z + 6, 0.5f, 0.12f, 5); k_box(R, lampm, 6, 13.2f, z + 6, 0.5f, 0.12f, 5);
    for (int k = 0; k < 5; k++) {   /* icicles off the girders */
      float len = 0.5f + 1.4f * fr(i * 11 + k * 5), ix = -17 + fr(i * 13 + k * 7) * 34;
      k_cyl(R, ice, ix, 13.35f - len / 2, z, 0.1f, 0.0f, len, 5);
    }
  }
  for (int i = 0; i < 2; i++) { t3_mesh *p = k_cyl(R, steel, i ? 9 : -9, 12.8f, 100, 0.25f, 0.25f, 220, 8); t3_object_set_rotation(p, DS_PI / 2, 0, 0); }
  /* racks of wrapped furniture down both sides */
  for (int i = 0; i < 12; i++) { rack(R, steel, -14, 12 + i * 15.f, -1, i); rack(R, steel, 14, 12 + i * 15.f, 1, i + 40); }
  /* the chute: the Sublet's exit, a pipe let into the roof of the hold */
  CO.chute = k_group(R, 0, 14, 18);
  { t3_mesh *pipe = k_cyl(CO.chute, km_std(0x4a5560, 0.5f, 0.5f, 0, 0), 0, 8, 0, 2.5f, 2.5f, 16, 20);
    pipe->materials[0]->side = T3_DOUBLE_SIDE;
    k_cyl(CO.chute, km_std(0xd8a21a, 0.6f, 0.2f, 0, 0), 0, -0.2f, 0, 2.7f, 2.7f, 0.7f, 20);
    CO.lamp = (t3_object *)k_cyl(CO.chute, km_basic(0xfff3c8, false), 0, 12, 0, 2.2f, 2.2f, 0.2f, 16); }
  /* the hero pieces: the sofa and the armchair, each with a century old tag */
  wrapped(R, 0, -8, 38, DS_PI / 2, 0xb04a5c);
  wrapped(R, 1, -8, 32.5f, DS_PI / 2, 0x3f9aa4);
  t3_material *ta = km_tex(k_paint(256, 160, tx_tag_a, false), true, true), *tb = km_tex(k_paint(256, 160, tx_tag_b, false), true, true);
  CO.label[0] = (t3_object *)k_plane(R, ta, -7.16f, 1.0f, 38.6f, 0.95f, 0.6f, 0, 0, 1, 1);
  CO.label[1] = (t3_object *)k_plane(R, tb, -7.16f, 0.95f, 32.5f, 0.95f, 0.6f, 0, 0, 1, 1);
  for (int i = 0; i < 2; i++) t3_object_set_rotation(CO.label[i], 0, DS_PI / 2, 0);
  /* the gantry banner over the aisle */
  CO.banner = k_group(R, 0, 0, 64);
  k_box(CO.banner, steel, -6.6f, 5, 0, 0.3f, 10, 0.3f); k_box(CO.banner, steel, 6.6f, 5, 0, 0.3f, 10, 0.3f);
  k_box(CO.banner, steel, 0, 10, 0, 13.4f, 0.3f, 0.3f);
  { t3_mesh *bn = k_plane(CO.banner, km_tex(k_paint(512, 96, tx_banner, false), true, true), 0, 8.4f, -0.2f, 9, 1.7f, 0, 0, 1, 1);
    t3_object_set_rotation(bn, 0, DS_PI, 0); }
  /* the heater core at the far end and the nest of foil blankets around it */
  CO.heater = k_group(R, 0, 0, 196);
  k_cyl(CO.heater, km_std(0x3a3430, 0.6f, 0.4f, 0, 0), 0, 4.5f, 0, 3.4f, 3.6f, 9, 18);
  t3_object_set_rotation(k_cyl(CO.heater, km_basic(0xff9a3a, false), 0, 4.0f, -3.5f, 2.3f, 2.3f, 0.6f, 18), DS_PI / 2, 0, 0);
  for (int i = 0; i < 6; i++) k_box(CO.heater, km_std(0x2a2622, 0.6f, 0.4f, 0, 0), -3 + i * 1.2f, 6.2f, -3.2f, 0.9f, 4, 0.5f);
  for (int i = 0; i < 6; i++) {
    t3_object *b = (t3_object *)k_box(CO.heater, km_std(0xb8c4cc, 0.3f, 0.8f, 0, 0), -6 + i * 2.4f, 0.4f, -7 + fr(i * 5) * 3, 2.4f, 0.8f, 1.8f);
    t3_object_set_rotation(b, 0, fr(i * 3) * 3, 0);
  }
  R->visible = false;
}

/* ═════════════ the Annex: Halcyon Reach, morning ═════════════ */
annx_t AN;
static void tx_roof(cv *c, int w, int h) {
  cv_fill_hex(c, 0x8b9096, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_stroke_hex(c, 0x6d7278, 1); cv_line_width(c, 2); cv_stroke_rect(c, 1, 1, w - 2.f, h - 2.f);
  for (int i = 0; i < 90; i++) { cv_fill_hex(c, i & 1 ? 0xb4b8bc : 0x70757b, 0.2f); cv_fill_rect(c, fr(i * 3) * w, fr(i * 7) * h, 3, 3); }
}
/* a tower as a four faced box of glass: ybot..ytop, faces textured at the colony's pane size */
static void tbox(t3_object *par, t3_material *face, float cx, float cz, float w, float d, float ybot, float ytop) {
  float h = ytop - ybot, cy = (ybot + ytop) / 2;
  struct { float x, z, len, yaw; } f[4] = {{cx, cz + d / 2, w, 0}, {cx, cz - d / 2, w, DS_PI}, {cx - w / 2, cz, d, -DS_PI / 2}, {cx + w / 2, cz, d, DS_PI / 2}};
  for (int i = 0; i < 4; i++) {
    t3_mesh *p = k_plane(par, face, f[i].x, cy, f[i].z, f[i].len, h, 0, 0, f[i].len / 9.6f, h / 14.4f);
    t3_object_set_rotation(p, 0, f[i].yaw, 0);
  }
  t3_mesh *t = k_floor(par, M_dark, cx, ytop, cz, w, d, 1, 1); (void)t;
}
static t3_object *turret_build(t3_object *par, float x, float y, float z, float yaw) {
  t3_object *g = k_group(par, x, y, z);
  t3_object_set_rotation(g, 0, yaw, 0);
  t3_material *m = km_std(0x59606b, 0.5f, 0.5f, 0, 0);
  k_box(g, m, 0, 0.2f, 0, 1.8f, 0.4f, 1.8f);
  k_cyl(g, m, 0, 0.9f, 0, 0.5f, 0.7f, 1.0f, 12);
  k_box(g, m, 0, 1.6f, 0, 1.0f, 0.6f, 1.2f);
  t3_mesh *bar = k_cyl(g, km_std(0x30343c, 0.5f, 0.5f, 0, 0), 0, 1.6f, 1.0f, 0.12f, 0.12f, 1.6f, 8);
  t3_object_set_rotation(bar, DS_PI / 2, 0, 0);
  k_sph(g, km_basic(0xe8433a, false), 0, 1.95f, 0.45f, 0.1f);
  return g;
}
static void annex_build(void) {
  AN.origin = v3_(4000, 3000, 4000);
  AN.root = k_group(&scn->base, AN.origin.x, AN.origin.y, AN.origin.z);
  t3_object *R = AN.root;
  t3_material *skym = km_tex(k_paint(1024, 512, tx_sky_morning, false), true, false);
  skym->side = T3_BACK_SIDE; skym->depth_write = false;
  AN.dome = (t3_object *)put_mesh(R, t3_sphere_geometry_new(3900, 32, 16), skym, 0, 0, 0);
  T3_OBJ(AN.dome)->render_order = -10;
  t3_material *pm = km_tex(k_paint(1024, 512, tx_planet, false), true, false);
  AN.planet = (t3_object *)put_mesh(R, t3_sphere_geometry_new(2600, 56, 28), pm, 150, -2790, 3050);
  t3_object_set_rotation(AN.planet, DS_PI / 2, 0.6f, 0);
  t3_material *hm = km_basic(0x9fc8ff, false);
  hm->transparent = true; hm->opacity = 0.22f; hm->side = T3_BACK_SIDE; hm->depth_write = false;
  put_mesh(R, t3_sphere_geometry_new(2700, 48, 24), hm, 150, -2790, 3050);
  { t3_mesh *ring = put_mesh(R, t3_torus_geometry_new(2200, 26, 8, 90, 1.3f), km_basic(0xd7e1ee, false), 0, -1500, 500);
    t3_object_set_rotation(ring, 0, 0, DS_PI / 2 - 0.65f); }
  /* tower 9's roof is the floor of the scene; its neighbours stand around it */
  t3_material *face = km_tex(k_paint(256, 256, tx_tower, true), true, true);
  face->side = T3_DOUBLE_SIDE;
  t3_material *roof = km_tex(k_paint(128, 128, tx_roof, true), false, true);
  tbox(R, face, 0, 0, 30, 30, -420, 0);
  k_floor(R, roof, 0, 0.02f, 0, 30, 30, 3, 3);
  static const float tw[][5] = {{-52, 4, 30, 30, 16}, {44, 22, 30, 30, 0.4f}, {0, -58, 40, 30, 44}, {-62, -66, 30, 30, 4}, {64, -70, 30, 30, 26},
                                {112, 2, 30, 30, 30}, {-110, -4, 30, 30, 24}, {30, -130, 40, 40, 60}, {-40, -140, 40, 40, 50}};
  for (int i = 0; i < 9; i++) tbox(R, face, tw[i][0], tw[i][1], tw[i][2], tw[i][3], -420, tw[i][4]);
  /* roof furniture: a parapet on three sides, an access hatch, a davit over the gondola */
  t3_material *conc = km_std(0xa0a5ab, 0.8f, 0.05f, 0, 0);
  k_box(R, conc, -14.7f, 0.5f, 0, 0.6f, 1.0f, 30); k_box(R, conc, 14.7f, 0.5f, 0, 0.6f, 1.0f, 30); k_box(R, conc, 0, 0.5f, -14.7f, 30, 1.0f, 0.6f);
  k_box(R, conc, -12.75f, 0.5f, 14.7f, 4.5f, 1.0f, 0.6f); k_box(R, conc, 4.75f, 0.5f, 14.7f, 20.5f, 1.0f, 0.6f);   /* the front edge is open at x -10.5..-5.5 for the gondola bay */
  for (int i = 0; i < 3; i++) k_box(R, km_std(0x59606b, 0.6f, 0.4f, 0, 0), -2 + i * 4.f, 0.6f, -6 + (i & 1) * 2.f, 1.6f, 1.2f, 1.2f);   /* vents */
  k_box(R, km_basic(C_HIVIS, false), 0, 1.03f, -14.7f, 30, 0.06f, 0.6f);   /* crew paint along the parapet */
  k_box(R, km_std(0x59606b, 0.6f, 0.4f, 0, 0), 4, 1.2f, -10, 3.2f, 2.4f, 2.6f);
  k_box(R, km_std(C_HIVIS, 0.6f, 0.1f, 0, 0), 4, 1.25f, -8.68f, 1.2f, 1.8f, 0.1f);
  k_model(R, "w_antenna.glb", 9, -11, 0, -11, 0);
  t3_material *orange = km_std(C_HIVIS, 0.55f, 0.1f, 0, 0);
  k_box(R, orange, -11.5f, 1.9f, 16.3f, 0.25f, 3.8f, 0.25f);   /* the davit post beside the bay, and the beam the cables hang from */
  k_box(R, orange, -8.5f, 3.1f, 16.3f, 6.4f, 0.2f, 0.2f);
  AN.gondola = gondola_build(R, -8, -0.2f, 16.6f, 3.2f, false);
  AN.bucket = k_group(R, 0, -50, 0);
  { t3_mesh *bk = k_cyl(AN.bucket, km_std(0x4aa8e0, 0.5f, 0.1f, 0x2a6f9a, 0.3f), 0, 0.18f, 0, 0.18f, 0.24f, 0.36f, 12); (void)bk; }
  /* the window the huddle carries: a spare pane in a frame */
  AN.pane = k_group(R, 0, -50, 0);
  { t3_material *glass = km_tex(k_paint(256, 192, tx_work_clean, false), true, true);
    k_plane(AN.pane, glass, 0, 0, 0, 1.5f, 1.15f, 0, 0, 1, 1);
    k_box(AN.pane, M_frame, 0, 0.6f, 0.02f, 1.6f, 0.07f, 0.06f); k_box(AN.pane, M_frame, 0, -0.6f, 0.02f, 1.6f, 0.07f, 0.06f);
    k_box(AN.pane, M_frame, -0.78f, 0, 0.02f, 0.07f, 1.2f, 0.06f); k_box(AN.pane, M_frame, 0.78f, 0, 0.02f, 0.07f, 1.2f, 0.06f); }
  /* tower 4: the turret a Brute is bolting to its roof edge */
  AN.turret = turret_build(R, 31.2f, 0.4f, 20, -DS_PI / 2);
  /* the Tenancies, close now and dark against the sun */
  const float sx[3] = {-1500, -30, 1450}, sz[3] = {3000, 3250, 3050};
  for (int i = 0; i < 3; i++) {
    AN.ship[i] = k_group(R, sx[i], 60, sz[i]);
    if (i == 0) ship_lessee(AN.ship[i]); else if (i == 1) ship_sublet(AN.ship[i]); else ship_freehold(AN.ship[i]);
    t3_object_set_scale(AN.ship[i], 2.1f, 2.1f, 2.1f);
    t3_object_set_rotation(AN.ship[i], 0, i == 0 ? 0.45f : i == 1 ? -0.3f : 0.25f, 0);
  }
  R->visible = false;
}

/* ═════════════ the Basement: the Freehold's undercroft ═════════════ */
base_t BS;
static void tx_cabinet(cv *c, int w, int h) {
  cv_fill_hex(c, 0x4c5560, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int i = 0; i < 4; i++) {
    float y = 4 + i * (h - 8.f) / 4;
    cv_fill_linear(c, 0, y, 0, y + (h - 8.f) / 4 - 4);
    cv_fill_stop(c, 0, "#6a7480"); cv_fill_stop(c, 1, "#535c68");
    cv_fill_rect(c, 4, y, w - 8.f, (h - 8.f) / 4 - 4);
    cv_fill_hex(c, 0xd8dcd0, 1); cv_fill_rect(c, w / 2.f - 12, y + 6, 24, 9);
    cv_fill_hex(c, 0x20252b, 1); cv_fill_rect(c, w / 2.f - 14, y + 24, 28, 4);
  }
}
static void tx_bfloor(cv *c, int w, int h) {
  cv_fill_hex(c, 0x1c2026, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_stroke_hex(c, 0x2b3037, 1); cv_line_width(c, 2); cv_stroke_rect(c, 1, 1, w - 2.f, h - 2.f);
  for (int i = 0; i < 80; i++) { cv_fill_hex(c, 0x3a4048, 0.3f); cv_fill_rect(c, fr(i * 3) * w, fr(i * 7) * h, 2, 2); }
}
static void tx_server(cv *c, int w, int h) {
  cv_fill_hex(c, 0x0e1216, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int j = 0; j < 20; j++)
    for (int i = 0; i < 6; i++) {
      float f = fr(j * 7 + i * 3);
      cv_fill_hex(c, f < 0.7f ? 0x37ff9a : f < 0.9f ? 0x1a5a3a : 0xe8433a, 0.35f + 0.65f * fr(j * 5 + i));
      cv_fill_rect(c, 8.f + i * 18, 6.f + j * 12, 8, 4);
    }
  cv_fill_hex(c, 0x2a323a, 1); cv_fill_rect(c, 0, 0, 3, (float)h); cv_fill_rect(c, w - 3.f, 0, 3, (float)h);
}
static void tx_pile(cv *c, int w, int h) {
  cv_fill_hex(c, 0xe4dcc6, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int i = 0; i < 14; i++) { cv_fill_hex(c, 0x8a8576, 0.5f); cv_fill_rect(c, 0, i * h / 14.f, (float)w, 1.5f); }
}
/* a Tenant form: crayon planet, a few lines, and the stamp */
static void tx_form_blank(cv *c, int w, int h) {
  cv_fill_hex(c, 0xf2ead8, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, 0x101218, 0.8f); cv_fill_rect(c, 8, 8, w - 16.f, 20);
  cv_fill_hex(c, 0x101218, 0.35f);
  for (int i = 0; i < 6; i++) cv_fill_rect(c, 12, 120.f + i * 14, w - 24.f, 2);
  cv_fill_hex(c, 0x2b6fa5, 1); cv_begin(c); cv_arc(c, w / 2.f, 76, 32, 0, 6.3f, false); cv_fill(c);
  cv_fill_hex(c, 0x4f8a50, 1); cv_begin(c); cv_ellipse(c, w / 2.f - 8, 70, 13, 8, 0.5f, 0, 6.3f, false); cv_fill(c);
}
static void tx_form_stamped(cv *c, int w, int h) {
  tx_form_blank(c, w, h);
  cv_save(c);
  cv_translate(c, w / 2.f, h * 0.62f); cv_rotate(c, -0.22f);
  cv_stroke_hex(c, 0xd23a2e, 0.92f); cv_line_width(c, 5); cv_stroke_rect(c, -w * 0.42f, -17, w * 0.84f, 34);
  cv_font(c, F_DISPLAY, 22); cv_letter_spacing(c, 2); cv_text_align(c, CV_CENTER); cv_text_baseline(c, CV_MIDDLE);
  cv_fill_hex(c, 0xd23a2e, 0.92f); cv_fill_text(c, "RECEIVED", 0, 1);
  cv_restore(c);
}
static void tx_stamp_mark(cv *c, int w, int h) {
  cv_clear(c);
  cv_save(c);
  cv_translate(c, w / 2.f, h / 2.f); cv_rotate(c, -0.22f);
  cv_stroke_hex(c, 0xd23a2e, 0.92f); cv_line_width(c, 5); cv_stroke_rect(c, -w * 0.45f, -17, w * 0.9f, 34);
  cv_font(c, F_DISPLAY, 22); cv_letter_spacing(c, 2); cv_text_align(c, CV_CENTER); cv_text_baseline(c, CV_MIDDLE);
  cv_fill_hex(c, 0xd23a2e, 0.92f); cv_fill_text(c, "RECEIVED", 0, 1);
  cv_restore(c);
}
static void tx_relay_sign(cv *c, int w, int h) {
  cv_fill_hex(c, 0x1a0c0c, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_stroke_hex(c, 0xe8433a, 1); cv_line_width(c, 5); cv_stroke_rect(c, 5, 5, w - 10.f, h - 10.f);
  cv_font(c, F_DISPLAY, 30); cv_letter_spacing(c, 3); cv_text_align(c, CV_CENTER); cv_text_baseline(c, CV_MIDDLE);
  cv_fill_hex(c, 0xff6a5a, 1); cv_fill_text(c, "EVICTION RELAY", w / 2.f, h / 2.f - 12);
  cv_font(c, F_MONO, 16); cv_letter_spacing(c, 1);
  cv_fill_hex(c, 0xffb0a0, 0.9f); cv_fill_text(c, "AUTHORISED STAFF ONLY", w / 2.f, h / 2.f + 22);
}
static void basement_build(void) {
  BS.origin = v3_(-4000, 3000, 4000);
  BS.root = k_group(&scn->base, BS.origin.x, BS.origin.y, BS.origin.z);
  t3_object *R = BS.root;
  t3_material *floor_m = km_tex(k_paint(128, 128, tx_bfloor, true), false, true);
  floor_m->roughness = 0.5f;
  k_floor(R, floor_m, 0, 0, 100, 90, 240, 3.0f, 3.0f);
  k_floor(R, km_basic(0x37ff9a, false), 0, 0.02f, 100, 0.12f, 240, 1, 1);   /* the green guide line down the aisle */
  /* the cabinets: rows of drawers to the horizon, aisles crossing every so often */
  t3_material *cab = km_tex(k_paint(64, 128, tx_cabinet, true), false, true);
  cab->side = T3_DOUBLE_SIDE; cab->roughness = 0.6f; cab->metalness = 0.4f;
  t3_material *body = km_std(0x2e353d, 0.7f, 0.4f, 0, 0), *led = km_basic(0x37ff9a, false);
  for (int r = 0; r < 4; r++)
    for (int s = -1; s <= 1; s += 2) {
      float x = s * (r == 0 ? 3.6f : 10.5f + (r - 1) * 7.0f);
      for (int n = 0; n < 8; n++) {
        float z0 = -10 + n * 32.f + r * 9.f, len = 27.f, z = z0 + len / 2;
        k_box(R, body, x, 3.75f, z, 1.2f, 7.5f, len);
        for (int f = -1; f <= 1; f += 2) {
          t3_mesh *p = k_plane(R, cab, x + f * 0.61f, 3.75f, z, len, 7.5f, 0, 0, len / 1.25f, 3);
          t3_object_set_rotation(p, 0, f * DS_PI / 2, 0);
        }
        if (r == 0) k_box(R, led, x - s * 0.5f, 7.7f, z, 0.1f, 0.1f, len);   /* terminal green strip along the top */
      }
    }
  /* pneumatic tubes overhead, with capsules shuttling */
  t3_material *tube = km_std(0xaab4bd, 0.35f, 0.7f, 0, 0), *capm = km_std(0xf0ece0, 0.5f, 0, 0x37ff9a, 0.4f);
  for (int i = 0; i < 4; i++) { t3_mesh *t = k_cyl(R, tube, -1.8f + i * 1.2f, 8.4f, 100, 0.2f, 0.2f, 240, 10); t3_object_set_rotation(t, DS_PI / 2, 0, 0); }
  for (int i = 0; i < 28; i++) k_box(R, body, 0, 8.9f, -6 + i * 8.f, 6, 0.15f, 0.2f);
  for (int i = 0; i < 10; i++) BS.cap[i] = (t3_object *)k_box(R, capm, -1.8f + (i % 4) * 1.2f, 8.4f, 0, 0.3f, 0.3f, 0.7f);
  /* the stamping station: a drop pipe, a belt, a stamp arm, a bin */
  t3_material *steel = km_std(0x6b7480, 0.5f, 0.5f, 0, 0), *red = km_std(0xd23a2e, 0.5f, 0.3f, 0xd23a2e, 0.35f);
  { t3_mesh *drop = k_cyl(R, tube, 0.2f, 4.7f, 17.2f, 0.28f, 0.28f, 8.8f, 10); (void)drop; }
  k_box(R, steel, 1.0f, 0.55f, 22.2f, 1.0f, 0.12f, 6.6f);
  k_box(R, body, 1.0f, 0.25f, 22.2f, 1.2f, 0.5f, 6.8f);
  k_box(R, steel, 0.35f, 2.3f, 22.2f, 0.18f, 2.4f, 0.18f); k_box(R, steel, 1.65f, 2.3f, 22.2f, 0.18f, 2.4f, 0.18f);
  k_box(R, steel, 1.0f, 3.5f, 22.2f, 1.5f, 0.2f, 0.4f);
  BS.stamp = k_group(R, 1.0f, 2.2f, 22.2f);
  k_box(BS.stamp, steel, 0, 0.8f, 0, 0.12f, 1.6f, 0.12f);
  k_box(BS.stamp, red, 0, 0.0f, 0, 0.7f, 0.2f, 0.7f);
  t3_material *fm = km_tex(k_paint(128, 160, tx_form_blank, false), true, true), *stm = km_tex(k_paint(128, 160, tx_stamp_mark, false), true, true);
  fm->side = stm->side = T3_DOUBLE_SIDE; stm->transparent = true; stm->depth_write = false;
  for (int i = 0; i < 6; i++) {
    BS.form[i] = k_group(R, 1.0f, 0.66f, 19);
    t3_mesh *a = k_plane(BS.form[i], fm, 0, 0, 0, 0.5f, 0.62f, 0, 0, 1, 1); t3_object_set_rotation(a, -DS_PI / 2, 0, 0);
    t3_mesh *b = k_plane(BS.form[i], stm, 0, 0.01f, 0, 0.5f, 0.62f, 0, 0, 1, 1); t3_object_set_rotation(b, -DS_PI / 2, 0, 0);
    T3_OBJ(b)->visible = false;
  }
  /* the heaps of forms, stamped and shelved nowhere */
  t3_material *pile = km_tex(k_paint(64, 64, tx_pile, true), false, true);
  pile->roughness = 0.9f;
  k_box(R, pile, 1.0f, 0.4f, 26.3f, 1.4f, 0.8f, 1.2f);
  static const float hx[8] = {-1.8f, -2.2f, 2.4f, 2.0f, -2.5f, 2.3f, -1.5f, 1.8f}, hz[8] = {12, 28, 14, 36, 44, 52, 62, 70};
  for (int i = 0; i < 8; i++) { t3_mesh *m = k_box(R, pile, hx[i], 0.6f + 0.3f * fr(i), hz[i], 1.1f, 1.2f + 0.6f * fr(i * 3), 1.1f); t3_object_set_rotation(m, 0, fr(i) * 1.2f, 0); }
  { t3_material *fs = km_tex(k_paint(128, 160, tx_form_stamped, false), true, true); fs->side = T3_DOUBLE_SIDE;
    for (int i = 0; i < 26; i++) {   /* loose forms across the floor */
      t3_mesh *p = k_plane(R, fs, -2.6f + fr(i * 5) * 5.2f, 0.03f + 0.002f * i, 5 + fr(i * 7) * 60, 0.5f, 0.62f, 0, 0, 1, 1);
      t3_object_set_rotation(p, -DS_PI / 2, 0, fr(i) * 6);
    } }
  /* the eviction relay and its server stacks at the far end of the aisle */
  t3_material *srv = km_tex(k_paint(128, 256, tx_server, false), true, false);
  for (int i = 0; i < 7; i++) {
    float x = -15 + i * 5.f;
    if (i == 3) continue;
    k_box(R, body, x, 5, 206, 3.4f, 10, 2.4f);
    t3_object_set_rotation(k_plane(R, srv, x, 5, 204.78f, 3.2f, 9.6f, 0, 0, 1, 1), 0, DS_PI, 0);
  }
  BS.relay = k_group(R, 0, 0, 200);
  k_cyl(BS.relay, body, 0, 0.5f, 0, 3.2f, 3.6f, 1.0f, 20);
  k_cyl(BS.relay, km_std(0xff5a4a, 0.3f, 0.3f, 0xe8433a, 1.1f), 0, 5, 0, 1.0f, 1.0f, 9, 16);
  BS.relay_ring = k_group(BS.relay, 0, 6.0f, 0);
  { t3_mesh *rg = put_mesh(BS.relay_ring, t3_torus_geometry_new(2.2f, 0.18f, 8, 36, DS_PI * 2), km_basic(0xff5a4a, false), 0, 0, 0);
    t3_object_set_rotation(rg, DS_PI / 2, 0, 0); }
  { t3_mesh *sn = k_plane(R, km_tex(k_paint(384, 96, tx_relay_sign, false), true, false), 0, 11.2f, 203.8f, 8, 2, 0, 0, 1, 1); t3_object_set_rotation(sn, 0, DS_PI, 0); }
  R->visible = false;
}

/* Each set is hundreds of small static meshes; drawn one by one they cost a
 * thousand draws a frame (two frames a second in a browser). Merge what never
 * moves; everything a scene animates is held by name in the set structs and
 * left alone. */
#define SKIPS(...) (t3_object *[]){__VA_ARGS__}, (int)(sizeof (t3_object *[]){__VA_ARGS__} / sizeof(t3_object *))
static void batch_set(t3_object *root, t3_object **skip, int n) {
  int k = 0;
  for (int i = 0; i < n; i++) if (skip[i]) skip[k++] = skip[i];   /* sets leave some slots empty */
  batch_static(root, skip, k);
}

void sets_init(t3_scene *scene) {
  scn = scene;
  halcyon_build();
  sublet_build();
  freehold_build();
  bay_build();
  cold_build();
  annex_build();
  basement_build();
  batch_set(HA.root, SKIPS(HA.dome, HA.planet, HA.gondola, HA.panel, HA.hole, HA.halo, HA.strip[0], HA.strip[1], HA.strip[2], HA.strip[3],
                           HA.strip[4], HA.strip[5], HA.strip[6], HA.strip[7], HA.ship[0], HA.ship[1], HA.ship[2], HA.window_back, HA.form, HA.squeegee));
  batch_set(SU.root, SKIPS(SU.dome, SU.lamps, SU.freehold, SU.nests));
  batch_set(FR.root, SKIPS(FR.dome, FR.arena, FR.crane[0], FR.crane[1], FR.crane[2], FR.crane[3], FR.crane[4], FR.crane[5], FR.crane[6], FR.crane[7],
                           FR.jib[0], FR.jib[1], FR.jib[2], FR.jib[3], FR.jib[4], FR.jib[5], FR.jib[6], FR.jib[7],
                           FR.lamp[0], FR.lamp[1], FR.lamp[2], FR.lamp[3], FR.lamp[4], FR.lamp[5], FR.lamp[6], FR.lamp[7],
                           FR.furn[0], FR.furn[1], FR.furn[2], FR.furn[3], FR.furn[4], FR.furn[5], FR.furn[6], FR.furn[7], FR.furn[8], FR.furn[9],
                           FR.beam, FR.lease));
  batch_set(BAY.root, SKIPS(BAY.plate[0], BAY.plate[1], BAY.lamp));
  batch_set(CO.root, SKIPS(CO.chute, CO.lamp, CO.label[0], CO.label[1], CO.banner, CO.heater));
  batch_set(AN.root, SKIPS(AN.dome, AN.planet, AN.gondola, AN.bucket, AN.pane, AN.turret, AN.ship[0], AN.ship[1], AN.ship[2]));
  batch_set(BS.root, SKIPS(BS.stamp, BS.form[0], BS.form[1], BS.form[2], BS.form[3], BS.form[4], BS.form[5], BS.cap[0], BS.cap[1], BS.cap[2], BS.cap[3],
                           BS.cap[4], BS.cap[5], BS.cap[6], BS.cap[7], BS.cap[8], BS.cap[9], BS.relay, BS.relay_ring));
}
