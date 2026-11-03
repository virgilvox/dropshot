/* batch.c: see batch.h. */
#include "batch.h"
#include "ds.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAXM 2048
static t3_mesh *list[MAXM]; static int nlist;
static t3_object *const *SKIP; static int NSKIP;
static t3_object *ROOT;

static bool skipped(t3_object *o) {
  for (t3_object *p = o; p && p != ROOT; p = p->parent) {
    if (!p->visible) return true;
    for (int i = 0; i < NSKIP; i++) if (SKIP[i] == p) return true;
  }
  return false;
}

static bool eligible(t3_mesh *m) {
  t3_object *o = &m->base;
  if (o->type != T3_MESH || m->material_count != 1 || !m->materials[0] || !m->geometry) return false;
  t3_material *mt = m->materials[0];
  if (mt->transparent || mt->blending != 1 || mt->vertex_colors || mt->alpha_test > 0 || !mt->depth_write) return false;
  t3_geometry *g = m->geometry;
  if (!g->attributes[T3_ATTR_POSITION] || !g->attributes[T3_ATTR_NORMAL]) return false;
  if (g->morph_count || g->attributes[T3_ATTR_SKIN_INDEX] || g->attributes[T3_ATTR_COLOR] || g->attributes[T3_ATTR_TANGENT] || g->attributes[T3_ATTR_UV1]) return false;
  if (g->named_count) return false;   /* groups with one material: three.js draws the whole geometry */
  if (g->attributes[T3_ATTR_POSITION]->type != T3_FLOAT32 || g->attributes[T3_ATTR_POSITION]->item_size != 3) return false;
  if (g->attributes[T3_ATTR_NORMAL]->type != T3_FLOAT32 || g->attributes[T3_ATTR_NORMAL]->item_size != 3) return false;
  if (g->attributes[T3_ATTR_UV] && (g->attributes[T3_ATTR_UV]->type != T3_FLOAT32 || g->attributes[T3_ATTR_UV]->item_size != 2)) return false;
  if (o->render_order != 0) return false;
  return !skipped(o);
}

static int why[12];
static void reason(t3_mesh *m) {
  t3_object *o = &m->base;
  if (m->material_count != 1) { why[0]++; return; }
  t3_material *mt = m->materials[0];
  if (mt->transparent) { why[1]++; return; }
  if (mt->blending != 1) { why[2]++; return; }
  if (mt->vertex_colors) { why[3]++; return; }
  t3_geometry *g = m->geometry;
  if (!g->attributes[T3_ATTR_NORMAL]) { why[4]++; return; }
  if (g->attributes[T3_ATTR_COLOR] || g->attributes[T3_ATTR_TANGENT] || g->attributes[T3_ATTR_UV1]) { why[5]++; return; }
  if (g->group_count > 1) { why[6]++; return; }
  if (g->attributes[T3_ATTR_UV] && (g->attributes[T3_ATTR_UV]->type != T3_FLOAT32)) { why[7]++; return; }
  if (g->attributes[T3_ATTR_POSITION]->type != T3_FLOAT32) { why[8]++; return; }
  if (o->render_order) { why[9]++; return; }
  if (skipped(o)) { why[10]++; return; }
  why[11]++;
}
static void collect(t3_object *o, void *ctx) {
  (void)ctx;
  if (o->type == T3_MESH && !eligible((t3_mesh *)o)) reason((t3_mesh *)o);
  if (nlist < MAXM && o->type == T3_MESH && eligible((t3_mesh *)o)) list[nlist++] = (t3_mesh *)o;
}

/* textures with the same pixels and sampling are one texture */
static t3_texture *canon[256]; static int ncanon;
static t3_texture *canonical(t3_texture *t) {
  if (!t) return NULL;
  for (int i = 0; i < ncanon; i++) {
    t3_texture *c = canon[i];
    if (c == t) return c;
    if (c->width == t->width && c->height == t->height && c->wrap_s == t->wrap_s && c->wrap_t == t->wrap_t &&
        c->mag_filter == t->mag_filter && c->min_filter == t->min_filter && c->anisotropy == t->anisotropy &&
        c->color_space == t->color_space && c->flip_y == t->flip_y && c->generate_mipmaps == t->generate_mipmaps &&
        c->repeat.x == t->repeat.x && c->repeat.y == t->repeat.y && c->offset.x == t->offset.x && c->offset.y == t->offset.y &&
        c->rotation == t->rotation && c->type == t->type && c->pixels && t->pixels &&
        !memcmp(c->pixels, t->pixels, (size_t)t->width * t->height * 4)) return c;
  }
  if (ncanon < 256 && t->pixels) canon[ncanon++] = t;
  return t;
}

static bool same_color(t3_color a, t3_color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static bool same_material(const t3_material *a, const t3_material *b) {
  if (a == b) return true;
  return a->type == b->type && same_color(a->color, b->color) && same_color(a->emissive, b->emissive) &&
         same_color(a->specular, b->specular) && a->opacity == b->opacity && a->shininess == b->shininess &&
         a->roughness == b->roughness && a->metalness == b->metalness && a->emissive_intensity == b->emissive_intensity &&
         canonical(a->map) == canonical(b->map) && a->normal_map == b->normal_map && a->ao_map == b->ao_map &&
         a->emissive_map == b->emissive_map && a->roughness_map == b->roughness_map && a->metalness_map == b->metalness_map &&
         a->alpha_map == b->alpha_map && a->side == b->side && a->flat_shading == b->flat_shading && a->fog == b->fog &&
         a->tone_mapped == b->tone_mapped && a->env_map == b->env_map && a->env_map_intensity == b->env_map_intensity &&
         a->depth_test == b->depth_test && a->depth_func == b->depth_func && a->color_write == b->color_write &&
         a->light_map == b->light_map && a->shadow_side == b->shadow_side;
}

static bool same_group(t3_mesh *a, t3_mesh *b) {
  return a->base.cast_shadow == b->base.cast_shadow && a->base.receive_shadow == b->base.receive_shadow &&
         !!a->geometry->attributes[T3_ATTR_UV] == !!b->geometry->attributes[T3_ATTR_UV] &&
         same_material(a->materials[0], b->materials[0]);
}

static int vcount(t3_geometry *g) { return g->attributes[T3_ATTR_POSITION]->count; }
static int icount(t3_geometry *g) { return g->index ? g->index->count : vcount(g); }
static uint32_t idx_at(t3_geometry *g, int i) {
  if (!g->index) return (uint32_t)i;
  return g->index->type == T3_UINT16 ? ((uint16_t *)g->index->array)[i] : ((uint32_t *)g->index->array)[i];
}

static void merge(t3_mesh **ms, int n) {
  int nv = 0, ni = 0;
  bool uv = ms[0]->geometry->attributes[T3_ATTR_UV] != NULL;
  for (int k = 0; k < n; k++) {
    t3_geometry *g = ms[k]->geometry;
    int s = g->draw_start > 0 ? g->draw_start : 0, c = g->draw_count >= 0 ? g->draw_count : icount(g) - s;
    if (c > icount(g) - s) c = icount(g) - s;
    nv += vcount(g); ni += c;
  }
  float *pos = malloc(sizeof(float) * 3 * nv), *nor = malloc(sizeof(float) * 3 * nv), *tex = uv ? malloc(sizeof(float) * 2 * nv) : NULL;
  uint32_t *ix = malloc(sizeof(uint32_t) * ni);
  int vo = 0, io = 0;
  for (int k = 0; k < n; k++) {
    t3_mesh *m = ms[k];
    t3_geometry *g = m->geometry;
    const float *e = m->base.matrix_world.e;
    /* the normal matrix: the inverse transpose of the upper 3 x 3 */
    float a = e[0], b = e[4], c = e[8], d = e[1], f = e[5], h = e[9], i = e[2], j = e[6], l = e[10];
    float A = f * l - h * j, B = -(d * l - h * i), C = d * j - f * i;
    float det = a * A + b * B + c * C;
    float inv = det != 0 ? 1.f / det : 0;
    float nm[9] = {A * inv, B * inv, C * inv,
                   -(b * l - c * j) * inv, (a * l - c * i) * inv, -(a * j - b * i) * inv,
                   (b * h - c * f) * inv, -(a * h - c * d) * inv, (a * f - b * d) * inv};
    const float *P = t3_attribute_f32(g->attributes[T3_ATTR_POSITION]), *N = t3_attribute_f32(g->attributes[T3_ATTR_NORMAL]);
    const float *U = uv ? t3_attribute_f32(g->attributes[T3_ATTR_UV]) : NULL;
    int cnt = vcount(g);
    for (int v = 0; v < cnt; v++) {
      float x = P[v * 3], y = P[v * 3 + 1], z = P[v * 3 + 2];
      float *o = pos + (vo + v) * 3;
      o[0] = e[0] * x + e[4] * y + e[8] * z + e[12];
      o[1] = e[1] * x + e[5] * y + e[9] * z + e[13];
      o[2] = e[2] * x + e[6] * y + e[10] * z + e[14];
      float nx = N[v * 3], ny = N[v * 3 + 1], nz = N[v * 3 + 2];
      float *q = nor + (vo + v) * 3;
      q[0] = nm[0] * nx + nm[1] * ny + nm[2] * nz;
      q[1] = nm[3] * nx + nm[4] * ny + nm[5] * nz;
      q[2] = nm[6] * nx + nm[7] * ny + nm[8] * nz;
      float ln = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
      if (ln > 0) { q[0] /= ln; q[1] /= ln; q[2] /= ln; }
      if (uv) { tex[(vo + v) * 2] = U[v * 2]; tex[(vo + v) * 2 + 1] = U[v * 2 + 1]; }
    }
    int s = g->draw_start > 0 ? g->draw_start : 0, c2 = g->draw_count >= 0 ? g->draw_count : icount(g) - s;
    if (c2 > icount(g) - s) c2 = icount(g) - s;
    bool flip = det < 0;   /* a mirrored transform turns the winding over */
    for (int t = 0; t + 2 < c2; t += 3) {
      uint32_t i0 = idx_at(g, s + t), i1 = idx_at(g, s + t + 1), i2 = idx_at(g, s + t + 2);
      ix[io++] = (uint32_t)vo + i0;
      ix[io++] = (uint32_t)vo + (flip ? i2 : i1);
      ix[io++] = (uint32_t)vo + (flip ? i1 : i2);
    }
    vo += cnt;
  }
  t3_geometry *G = t3_geometry_new();
  t3_attribute *ap = t3_attribute_new(T3_FLOAT32, pos, nv, 3), *an = t3_attribute_new(T3_FLOAT32, nor, nv, 3);
  t3_geometry_set_attribute(G, T3_ATTR_POSITION, ap); t3_release(ap);
  t3_geometry_set_attribute(G, T3_ATTR_NORMAL, an); t3_release(an);
  if (uv) { t3_attribute *at = t3_attribute_new(T3_FLOAT32, tex, nv, 2); t3_geometry_set_attribute(G, T3_ATTR_UV, at); t3_release(at); }
  t3_attribute *ai = nv < 65536 ? NULL : t3_attribute_new(T3_UINT32, ix, io, 1);
  if (!ai) {
    uint16_t *s16 = malloc(sizeof(uint16_t) * io);
    for (int i = 0; i < io; i++) s16[i] = (uint16_t)ix[i];
    ai = t3_attribute_new(T3_UINT16, s16, io, 1); free(s16);
  }
  t3_geometry_set_index(G, ai); t3_release(ai);
  t3_geometry_compute_bounding_sphere(G);
  t3_material *mat = ms[0]->materials[0];
  if (mat->map) { t3_texture *c = canonical(mat->map); if (c != mat->map) t3_material_set_map(mat, c); }
  t3_mesh *M = t3_mesh_new(G, mat);
  M->base.cast_shadow = ms[0]->base.cast_shadow; M->base.receive_shadow = ms[0]->base.receive_shadow;
  t3_object_add(ROOT, M);
  t3_release(M); t3_release(G);
  for (int k = 0; k < n; k++) t3_object_remove(ms[k]->base.parent, ms[k]);
  free(pos); free(nor); free(tex); free(ix);
}

void batch_static(t3_object *root, t3_object *const *skip, int nskip) {
  ROOT = root; SKIP = skip; NSKIP = nskip; nlist = 0; ncanon = 0;
  /* merged vertices land in the root's space: take its transform off while
   * merging (the cutscene sets sit kilometres away) and put it back after */
  t3_vec3 rp = root->position, rs = root->scale; t3_quat rq = root->quaternion;
  root->position = t3_v3(0, 0, 0); root->scale = t3_v3(1, 1, 1); root->quaternion = (t3_quat){0, 0, 0, 1};
  t3_object_update_matrix_world(root, true);
  t3_object_traverse(root, collect, NULL);
  int before = nlist, after = 0;
  bool *done = calloc(nlist, 1);
  t3_mesh **grp = malloc(sizeof(t3_mesh *) * (nlist ? nlist : 1));
  for (int i = 0; i < nlist; i++) {
    if (done[i]) continue;
    int n = 0; grp[n++] = list[i]; done[i] = true;
    for (int j = i + 1; j < nlist; j++) if (!done[j] && same_group(list[i], list[j])) { grp[n++] = list[j]; done[j] = true; }
    if (n >= 2) { for (int k = 0; k < n; k++) t3_retain(grp[k]); merge(grp, n); for (int k = 0; k < n; k++) t3_release(grp[k]); }
    after++;
  }
  free(done); free(grp);
  root->position = rp; root->scale = rs; root->quaternion = rq;
  t3_object_update_matrix_world(root, true);
  ds_log("batch: %d static meshes into %d draws; not merged: multi %d trans %d blend %d vcol %d nonorm %d attrs %d groups %d uvtype %d postype %d order %d skip %d other %d",
         before, after, why[0], why[1], why[2], why[3], why[4], why[5], why[6], why[7], why[8], why[9], why[10], why[11]);
}
