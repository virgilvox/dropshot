/* world.c: the six levels. Visuals are textured boxes plus Quaternius and
 * Kenney kit pieces; collision is a flat list of axis-aligned boxes authored
 * next to them. Units are metres, y up, yaw 0 faces +z. */
#include "world.h"
#include "enemy.h"
#include "cv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXB 1500
#define STEP_UP 0.45f

typedef struct { v3 lo, hi; int plat; } box_t;
typedef struct { int bi; v3 lo, hi, axis, off, delta; float period, phase; t3_object *vis; } plat_t;
typedef struct { int kind; v3 pos; bool taken; t3_object *vis; float ph; } pick_t;
typedef struct { v3 pos; t3_object *flag; bool hit; } cp_t;
typedef struct { t3_object *o; v3 ax; float rate; } spin_t;

static t3_scene *g_scene;
static t3_object *g_root, *g_cur;           /* the level's group; the group props are added to now */
static box_t g_box[MAXB];
static int g_nb;
static plat_t g_plat[16];
static int g_np;
static pick_t g_pk[48];
static int g_npk;
static cp_t g_cp[8];
static int g_ncp, g_cp_at;
static spawn_t g_sp[160];
static int g_nsp;
static v3 g_ring[12];
static t3_object *g_ring_vis[12];
static bool g_ring_got[12];
static int g_nring;
static spin_t g_spin[48];
static int g_nspin;
static v3 g_start, g_spawn;
static v3 g_exit_lo, g_exit_hi, g_arena_lo, g_arena_hi;
static float g_kill_y;
static float g_time;
static uint32_t g_rng = 12345;

static float rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) / 16777216.f; }
static float rr(float a, float b) { return a + rnd() * (b - a); }

/* ── models ── */
enum { MD_BLDG, MD_BASE, MD_DOME, MD_HCYL, MD_HLONG, MD_HONE, MD_SUPPORT, MD_SOLARST, MD_SOLAR, MD_ANT, MD_RADAR,
       MD_ROCK, MD_ROCKL, MD_PLANT, MD_PLANT2, MD_BUSH, MD_GRASS, MD_TREEB, MD_TREEF, MD_TREES, MD_TREEW, MD_TREEL,
       MD_PK_HEALTH, MD_PK_AMMO, MD_PK_JAR, MD_PK_SPHERE, MD_CRATE, MD_SHIP, MD_SHIP2,
       MD_GATE, MD_HANGAR, MD_DISH, MD_GEN, MD_BARRELS, MD_STRUCT, MD_SUPPORTS, MD_CARGO, MD_PIPE,
       MD_SOFA, MD_SOFAL, MD_LAMP, MD_LAMP2, MD_SHELF, MD_CHAIR, MD_CTABLE, MD_BED, MD_FRIDGE, MD_DESK, MD_TVCAB, MD_N };
static const char *g_mdname[MD_N] = {
  "w_bldg", "w_base", "w_dome", "w_hcyl", "w_hlong", "w_hone", "w_support", "w_solarst", "w_solar", "w_antenna", "w_radar",
  "w_rock", "w_rockl", "w_plant", "w_plant2", "w_bush", "w_grass", "w_treeb", "w_treef", "w_trees", "w_treew", "w_treel",
  "w_pk_health", "w_pk_ammo", "w_pk_jar", "w_pk_sphere", "w_crate", "w_ship", "w_ship2",
  "wk_door", "wk_barrier", "wk_display", "wk_container", "wk_containerw",
  "wk_structure", "wk_pipering", "w_ship", "wk_pillar",
  "w_sofa", "w_sofal", "w_lamp", "w_lamp2", "w_shelf", "w_chair", "w_ctable", "w_bed", "w_fridge", "w_desk", "w_tvcab" };
typedef struct { t3_geometry *g; t3_material **ms; int nm; t3_vec3 pos, sc; t3_quat q; } part_t;
typedef struct { part_t *parts; int np; v3 lo, hi; bool ok; } model_t;
static model_t g_md[MD_N];
static t3_gltf *g_gltf[MD_N];

typedef struct { part_t *parts; int n, cap; } collect_t;
static void collect_cb(t3_object *o, void *ctx) {
  collect_t *c = ctx;
  if (o->type != T3_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  if (!m->geometry || !m->geometry->attributes[T3_ATTR_POSITION]) return;
  if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 8; c->parts = realloc(c->parts, c->cap * sizeof(part_t)); }
  part_t *p = &c->parts[c->n++];
  p->g = m->geometry;
  p->nm = m->material_count;
  p->ms = malloc(sizeof(t3_material *) * (p->nm ? p->nm : 1));
  for (int i = 0; i < p->nm; i++) {
    p->ms[i] = m->materials[i];
    if (p->ms[i]) { p->ms[i]->metalness = 0.1f; p->ms[i]->roughness = 0.75f; p->ms[i]->version++; }
  }
  t3_mat4_decompose(&o->matrix_world, &p->pos, &p->q, &p->sc);
}

static void model_load(int id) {
  char nm[48]; snprintf(nm, sizeof nm, "%s.glb", g_mdname[id]);
  size_t n; uint8_t *b = ds_asset(nm, &n);
  model_t *m = &g_md[id];
  if (!b) { ds_log("world: missing %s", nm); return; }
  g_gltf[id] = t3_gltf_parse(b, n, NULL, NULL);
  free(b);
  if (!g_gltf[id]) { ds_log("world: bad %s", nm); return; }
  t3_object_update_matrix_world(g_gltf[id]->scene, true);
  collect_t c = {0};
  t3_object_traverse(g_gltf[id]->scene, collect_cb, &c);
  m->parts = c.parts; m->np = c.n;
  float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
  for (int i = 0; i < c.n; i++) {
    part_t *p = &c.parts[i];
    t3_geometry_compute_bounding_box(p->g);
    t3_box3 bb = p->g->bounding_box;
    t3_vec3 one = p->sc, tr = p->pos; t3_quat q = p->q;
    for (int k = 0; k < 8; k++) {
      float v[3] = {k & 1 ? bb.max.x : bb.min.x, k & 2 ? bb.max.y : bb.min.y, k & 4 ? bb.max.z : bb.min.z};
      v[0] *= one.x; v[1] *= one.y; v[2] *= one.z;
      /* rotate by q */
      float ux = q.y * v[2] - q.z * v[1], uy = q.z * v[0] - q.x * v[2], uz = q.x * v[1] - q.y * v[0];
      float r[3] = {v[0] + 2 * (q.w * ux + q.y * uz - q.z * uy), v[1] + 2 * (q.w * uy + q.z * ux - q.x * uz),
                    v[2] + 2 * (q.w * uz + q.x * uy - q.y * ux)};
      r[0] += tr.x; r[1] += tr.y; r[2] += tr.z;
      for (int a = 0; a < 3; a++) { if (r[a] < lo[a]) lo[a] = r[a]; if (r[a] > hi[a]) hi[a] = r[a]; }
    }
  }
  /* origin to the footprint centre, y kept */
  float cx = (lo[0] + hi[0]) / 2, cz = (lo[2] + hi[2]) / 2;
  for (int i = 0; i < c.n; i++) { c.parts[i].pos.x -= cx; c.parts[i].pos.z -= cz; }
  m->lo = v3_(lo[0] - cx, lo[1], lo[2] - cz);
  m->hi = v3_(hi[0] - cx, hi[1], hi[2] - cz);
  m->ok = c.n > 0;
  /* the kit's foliage atlas is near black under our lights: swap its materials for flat Sublet palette colours */
  if (id >= MD_PLANT && id <= MD_TREEL) {
    static const uint32_t fol[MD_TREEL - MD_PLANT + 1] = {0x58c06a, 0x3fb0a0, 0x6cc860, 0x8acc58, 0x3fa85c, 0xe27ab0, 0x9ad060, 0x4cc0a8, 0x78c470};
    for (int i = 0; i < c.n; i++)
      for (int k = 0; k < c.parts[i].nm; k++) {
        t3_material *fm = t3_mesh_standard_material_new(fol[id - MD_PLANT]);
        fm->metalness = 0.05f; fm->roughness = 0.85f;
        c.parts[i].ms[k] = fm;
      }
  }
}

/* ── textures and materials ── */
enum { M_DECK, M_HULL, M_PLATE, M_DARK, M_ORANGE, M_TEAL, M_WIN, M_WIN2, M_SOIL, M_HEDGE, M_ARENA, M_ARENASIDE,
       M_ORANGEG, M_TEALG, M_HAZ, M_SKY, M_RING1, M_RING2, M_PLANET, M_HALO, M_FARHULL, M_WOOD, M_LIGHTS, M_STAR,
       M_PANE, M_PINK, M_REDG, M_CRANE, M_TOWER, M_TOWERGAP, M_COLRING, M_MOSS,
       M_FROST, M_WRAP, M_AMBER, M_BELT, M_GLASS, M_ROOF, M_CABINET, M_SERVER, M_LED1, M_LED2, M_TUBE, M_CORE,
       M_TENANT, M_EYE, M_TERM, M_GOLD, M_RACK, M_COUNT };
static t3_material *g_mat[M_COUNT];
static t3_geometry *g_unit, *g_plane1, *g_sph;
/* signs: style 0 cream (the Lessee), 1 moss plaque (the Sublet), 2 red PA board (the Freehold),
 * 3 frost label (Cold Storage), 4 hi-vis notice (the Annex), 5 green terminal (the Basement) */
enum { SG_WALK, SG_WALK2, SG_NONEST, SG_QUIET, SG_TRIPLICATE, SG_GLASS, SG_FINAL, SG_BAY, SG_EXIT, SG_CLEAN, SG_GLIDE, SG_DOCK,
       SG_PLANTS, SG_KNOCK, SG_PLANTS2, SG_LIFT, SG_RENT, SG_HOME, SG_QUIET2, SG_BREACH, SG_EVICT, SG_FIXTURE, SG_LEASE, SG_RELOC,
       SG_CALM, SG_SUPER, SG_COMPLAINT, SG_STAIR,
       SG_C_DATE, SG_C_FRAGILE, SG_C_WARM, SG_C_HEAT, SG_C_LEASE, SG_C_LIFT, SG_C_CONV, SG_C_COLD,
       SG_A_TOWER, SG_A_GLIDE, SG_A_LOBBY, SG_A_BOLT, SG_A_LOCKERS, SG_A_NOTE, SG_A_TRAM, SG_A_GONDOLA, SG_A_STREAK,
       SG_B_RECV, SG_B_ARCH, SG_B_RULE, SG_B_RULE2, SG_B_AISLE, SG_B_RELAY, SG_B_TUBE, SG_B_STAFF, SG_B_ARMED, NSIGN };
static t3_material *g_sign[NSIGN];
static const struct { int style; const char *txt; } g_sign_def[NSIGN] = {
  {0, "PLEASE KEEP TO|THE WALKWAYS."},
  {0, "WALKWAYS ARE FOR WALKING.|Thank you for walking."},
  {0, "NO NESTING.|NO LOITERING.|NO GLIDING."},
  {0, "QUIET HOURS|ARE IN EFFECT.|All hours are quiet hours."},
  {0, "COMPLAINTS MAY BE FILED|IN TRIPLICATE.|Triplicate is closed."},
  {0, "UNAUTHORISED GLASS|WILL BE REMOVED.|Nests are not glass."},
  {0, "FINAL REMINDER (4 OF 4)|Reminders 1 to 3|were also final."},
  {0, "MAINTENANCE BAY 7|STAFF ONLY.|They will know."},
  {0, "EXIT|Thank you for your tenancy."},
  {0, "WINDOW CLEANING|is a shared responsibility.|Thank you for your patience."},
  {0, "PLEASE DO NOT GLIDE|BETWEEN TOWERS.|Unless urgent."},
  {0, "LOADING DOCK|NO LOADING"},
  {1, "THE PLANTS ARE NOT|responsible for your mood."},
  {1, "WE LIVE HERE NOW.|Please knock softly."},
  {1, "PLEASE KEEP OFF|THE PLANTS.|The plants have asked nicely."},
  {1, "LIFT TO LOWER GARDENS|Maximum one flamingo."},
  {1, "RENT IS PAYABLE|IN LIGHT.|Receipts on request."},
  {1, "WELCOME HOME|(from the Tenants)"},
  {1, "QUIET HOURS|STILL IN EFFECT.|Staff are listening."},
  {2, "ALL OCCUPANTS|ARE IN BREACH."},
  {2, "THIS IS A|SCHEDULED EVICTION."},
  {2, "WINDOWS ARE FIXTURES.|FIXTURES STAY."},
  {2, "YOUR LEASE EXPIRED|214 YEARS AGO."},
  {2, "NOTICE OF RELOCATION:|HALCYON REACH."},
  {2, "PLEASE REMAIN CALM|AND VACATE."},
  {0, "SUPERINTENDENT ON DUTY|Please knock.|Please do not knock."},
  {0, "UNIT 4B: Your complaint|has been received and|filed under LATER."},
  {0, "THIS STAIRWELL IS CLOSED|for your convenience."},
  {3, "DELIVER TO: OWNER|DATE: 214 YEARS AGO|They are running late."},
  {3, "FRAGILE: ONE (1) SOFA|Some assembly required."},
  {3, "WARM CORNER|Tenants resting.|Please walk softly."},
  {3, "HEATER CORE 3|DO NOT STAND NEAR IT.|Everyone is near it."},
  {3, "LEASE: 99 YEARS|EXPIRED ON A TUESDAY.|Filed under SOON."},
  {3, "CARGO LIFT TO HALCYON|Something is being shipped.|Do not sign for it."},
  {3, "CONVEYOR IN USE.|Riders ride at their|own appeal."},
  {3, "KEEP DOORS CLOSED.|There are no doors."},
  {4, "TOWER 9|WINDOW CREW ONLY|Flamingos welcome."},
  {4, "GLIDING BETWEEN TOWERS|is permitted today.|Management has relented."},
  {4, "TEMPORARY ACCOMMODATION|Please queue.|They are queueing."},
  {4, "LOAD-BEARING WINDOW|Do not bolt things to it."},
  {4, "WINDOW CREW LOCKERS|Thermos on the shelf.|Eat the soup."},
  {4, "JUNO: soup in the thermos.|Come back for the shift.|From the crew."},
  {4, "TRAM TO THE TETHER|Next departure: now.|Standing room: yes."},
  {4, "GONDOLA 4|Maximum one flamingo,|two squeegees."},
  {4, "STREAK REPORT:|none on this tower.|Good morning."},
  {5, "FORM RECEIVED.|FORM RECEIVED.|FORM RECEIVED."},
  {5, "COMPLAINT ARCHIVE|2,114,908 FORMS|READ: 0"},
  {2, "BUILDING RULE 41:|NO ECHOES."},
  {2, "BUILDING RULE 7:|ALL RULES ARE RULE 7."},
  {5, "AISLE 12|Please do not|file in the aisle."},
  {5, "EVICTION RELAY|TARGET: HALCYON REACH|STATUS: ARMED"},
  {5, "PNEUMATIC TUBE 6|Out of order.|Everything goes in anyway."},
  {5, "STAFF ONLY.|You are not staff.|That is why."},
  {5, "RELAY ARMED.|Cut the grey cable.|It is the red one."} };

static float frand_seed(int i) { return (float)((i * 2654435761u) >> 8 & 0xffff) / 65535.f; }

static t3_texture *paint(int w, int h, void (*fn)(cv *, int, int), bool repeat) {
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
static void hexcol(cv *c, uint32_t rgb, float k) {
  int r = (int)(((rgb >> 16) & 255) * k), g = (int)(((rgb >> 8) & 255) * k), b = (int)((rgb & 255) * k);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  cv_fill_hex(c, (uint32_t)(r << 16 | g << 8 | b), 1);
}
static void tx_deck(cv *c, int w, int h) {
  (void)h;
  int n = 4, s = w / n;
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++) {
      hexcol(c, 0x5a647a, 0.9f + 0.24f * frand_seed(j * 9 + i));
      cv_fill_rect(c, (float)i * s, (float)j * s, (float)s, (float)s);
      cv_fill_hex(c, 0x5a6479, 0.5f);
      cv_fill_rect(c, (float)i * s + 4, (float)j * s + 4, (float)s - 8, 3);
      hexcol(c, 0x707b92, 1);
      for (int k = 0; k < 4; k++) { cv_begin(c); cv_arc(c, (float)i * s + (k & 1 ? s - 10 : 10), (float)j * s + (k & 2 ? s - 10 : 10), 2.5f, 0, 6.3f, false); cv_fill(c); }
    }
  cv_stroke_hex(c, 0x2a3040, 1); cv_line_width(c, 4);
  for (int i = 0; i <= n; i++) {
    cv_begin(c); cv_move(c, (float)i * s, 0); cv_line(c, (float)i * s, (float)w); cv_stroke(c);
    cv_begin(c); cv_move(c, 0, (float)i * s); cv_line(c, (float)w, (float)i * s); cv_stroke(c);
  }
  cv_fill_hex(c, 0xff7a1a, 0.9f); cv_fill_rect(c, 3, (float)s * 2 - 3, 22, 6);
}
static void tx_hull(cv *c, int w, int h) {
  for (int j = 0; j < 8; j++) {
    hexcol(c, 0x7a8497, 0.85f + 0.3f * frand_seed(j + 40));
    cv_fill_rect(c, 0, (float)j * h / 8, (float)w, (float)h / 8);
    cv_stroke_hex(c, 0x3b4252, 1); cv_line_width(c, 3);
    float off = (j & 1) ? w / 2 : w / 4;
    cv_begin(c); cv_move(c, off, (float)j * h / 8); cv_line(c, off, (float)(j + 1) * h / 8); cv_stroke(c);
    cv_begin(c); cv_move(c, 0, (float)j * h / 8); cv_line(c, (float)w, (float)j * h / 8); cv_stroke(c);
  }
  cv_fill_hex(c, 0x2ec4b6, 0.9f); cv_fill_rect(c, 0, 4 * h / 8 - 3, (float)w, 4);
}
static void tx_plate(cv *c, int w, int h) {
  cv_fill_hex(c, 0x5b6579, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_stroke_hex(c, 0x2e3444, 1); cv_line_width(c, 6); cv_stroke_rect(c, 3, 3, (float)w - 6, (float)h - 6);
  cv_fill_hex(c, 0x7a8499, 0.7f);
  for (int i = 0; i < 3; i++) cv_fill_rect(c, 12, 14 + (float)i * 14, (float)w - 24, 5);
}
static void tx_win(cv *c, int w, int h) {
  cv_fill_hex(c, 0x131a2a, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  int cols = 8, rows = 8;
  for (int j = 0; j < rows; j++)
    for (int i = 0; i < cols; i++) {
      float f = frand_seed(j * 11 + i * 3 + 5);
      uint32_t col = f < 0.18f ? 0xffcf8a : f < 0.32f ? 0x7fe3d8 : f < 0.5f ? 0x2d3a58 : 0x1d2640;
      cv_fill_hex(c, col, 1);
      cv_fill_rect(c, (float)i * w / cols + 4, (float)j * h / rows + 5, (float)w / cols - 8, (float)h / rows - 10);
    }
}
static void tx_win2(cv *c, int w, int h) {
  cv_fill_hex(c, 0x16241f, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int j = 0; j < 4; j++)
    for (int i = 0; i < 4; i++) {
      float f = frand_seed(j * 7 + i * 5 + 90);
      uint32_t col = f < 0.3f ? 0xb6ff8a : f < 0.5f ? 0xffe9a0 : 0x244036;
      cv_fill_hex(c, col, 1);
      cv_fill_rect(c, (float)i * w / 4 + 5, (float)j * h / 4 + 5, (float)w / 4 - 10, (float)h / 4 - 10);
    }
  cv_stroke_hex(c, 0x0c1612, 1); cv_line_width(c, 5);
  for (int i = 0; i <= 4; i++) {
    cv_begin(c); cv_move(c, (float)i * w / 4, 0); cv_line(c, (float)i * w / 4, (float)h); cv_stroke(c);
    cv_begin(c); cv_move(c, 0, (float)i * h / 4); cv_line(c, (float)w, (float)i * h / 4); cv_stroke(c);
  }
}
static void tx_soil(cv *c, int w, int h) {
  cv_fill_hex(c, 0x3f6b3a, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int i = 0; i < 260; i++) {
    hexcol(c, frand_seed(i) < 0.5f ? 0x4f8a45 : 0x2f5a30, 1);
    float x = frand_seed(i * 3 + 1) * w, y = frand_seed(i * 5 + 2) * h;
    cv_fill_rect(c, x, y, 3 + frand_seed(i) * 6, 3);
  }
  cv_stroke_hex(c, 0x6e5538, 1); cv_line_width(c, 5);
  cv_begin(c); cv_move(c, 0, 0); cv_line(c, (float)w, 0); cv_stroke(c);
  cv_begin(c); cv_move(c, 0, 0); cv_line(c, 0, (float)h); cv_stroke(c);
}
static void tx_hedge(cv *c, int w, int h) {
  cv_fill_hex(c, 0x1f4a2a, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int i = 0; i < 420; i++) {
    uint32_t col = frand_seed(i) < 0.4f ? 0x2f7a3a : frand_seed(i) < 0.7f ? 0x265e30 : 0x4a9a48;
    cv_fill_hex(c, col, 1);
    cv_begin(c); cv_arc(c, frand_seed(i * 3 + 7) * w, frand_seed(i * 7 + 3) * h, 3 + frand_seed(i * 2) * 5, 0, 6.3f, false); cv_fill(c);
  }
}
static void tx_haz(cv *c, int w, int h) {
  cv_fill_hex(c, 0x1a1c24, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_fill_hex(c, 0xff7a1a, 1);
  for (int i = -2; i < 6; i++) {
    cv_begin(c); cv_move(c, (float)i * w / 4, 0); cv_line(c, (float)i * w / 4 + w / 8.f, 0);
    cv_line(c, (float)i * w / 4 + w / 8.f + h, (float)h); cv_line(c, (float)i * w / 4 + h, (float)h); cv_close(c); cv_fill(c);
  }
}
static void tx_wood(cv *c, int w, int h) {
  cv_fill_hex(c, 0x6a4c34, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_stroke_hex(c, 0x3c2a1c, 1); cv_line_width(c, 3);
  for (int i = 0; i < 6; i++) { cv_begin(c); cv_move(c, 0, (float)i * h / 6); cv_line(c, (float)w, (float)i * h / 6); cv_stroke(c); }
}
static void tx_arena(cv *c, int w, int h) {
  float cx = w / 2.f, cy = h / 2.f, R = w / 2.f;
  cv_fill_hex(c, 0x3a4152, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  /* panel seams in a grid */
  cv_stroke_hex(c, 0x222735, 1); cv_line_width(c, 2);
  for (int i = 0; i <= 20; i++) {
    cv_begin(c); cv_move(c, (float)i * w / 20, 0); cv_line(c, (float)i * w / 20, (float)h); cv_stroke(c);
    cv_begin(c); cv_move(c, 0, (float)i * h / 20); cv_line(c, (float)w, (float)i * h / 20); cv_stroke(c);
  }
  /* rim hazard ring, teal guide rings, orange centre mark */
  cv_stroke_hex(c, 0xff7a1a, 1); cv_line_width(c, R * 0.035f);
  cv_begin(c); cv_arc(c, cx, cy, R * 0.955f, 0, 6.3f, false); cv_stroke(c);
  cv_stroke_hex(c, 0x2ec4b6, 1); cv_line_width(c, R * 0.012f);
  cv_begin(c); cv_arc(c, cx, cy, R * 0.7f, 0, 6.3f, false); cv_stroke(c);
  cv_begin(c); cv_arc(c, cx, cy, R * 0.4f, 0, 6.3f, false); cv_stroke(c);
  cv_stroke_hex(c, 0xff7a1a, 1); cv_line_width(c, R * 0.03f);
  cv_begin(c); cv_arc(c, cx, cy, R * 0.16f, 0, 6.3f, false); cv_stroke(c);
  cv_begin(c); cv_move(c, cx - R * 0.26f, cy); cv_line(c, cx + R * 0.26f, cy); cv_stroke(c);
  cv_begin(c); cv_move(c, cx, cy - R * 0.26f); cv_line(c, cx, cy + R * 0.26f); cv_stroke(c);
  /* corners outside the disc are not seen */
}
static void tx_planet(cv *c, int w, int h) {
  cv_fill_linear(c, 0, 0, 0, (float)h);
  cv_fill_stop(c, 0, "#1f4a78"); cv_fill_stop(c, 0.5f, "#2b6fa5"); cv_fill_stop(c, 1, "#1f4a78");
  cv_fill_rect(c, 0, 0, (float)w, (float)h);
  /* every blob is drawn three times, shifted by the width, so the u seam wraps cleanly */
  for (int i = 0; i < 46; i++) {
    float x = frand_seed(i * 3 + 1) * w, y = (0.25f + frand_seed(i * 5 + 4) * 0.5f) * h, r = 14 + frand_seed(i) * 34;
    hexcol(c, frand_seed(i * 2 + 9) < 0.55f ? 0x4f8a50 : 0x8a7b52, 1);
    for (int sh = -1; sh <= 1; sh++)
      for (int k = 0; k < 4; k++) { cv_begin(c); cv_ellipse(c, x + sh * w + k * r * 0.45f, y + (k & 1) * r * 0.25f, r, r * 0.8f, 0, 0, 6.3f, false); cv_fill(c); }
  }
  cv_alpha(c, 0.4f);
  for (int i = 0; i < 40; i++) {
    float x = frand_seed(i * 7 + 2) * w, y = frand_seed(i * 11 + 6) * h, r = 10 + frand_seed(i) * 22;
    cv_fill_hex(c, 0xffffff, 1);
    for (int sh = -1; sh <= 1; sh++) { cv_begin(c); cv_ellipse(c, x + sh * w, y, r * 1.5f, r, 0, 0, 6.3f, false); cv_fill(c); }
  }
  cv_alpha(c, 1);
}
static void tx_lights(cv *c, int w, int h) {
  cv_fill_hex(c, 0x161c2e, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int j = 0; j < 16; j++)
    for (int i = 0; i < 16; i++) {
      if (frand_seed(j * 31 + i) < 0.45f) continue;
      cv_fill_hex(c, frand_seed(j + i * 5) < 0.3f ? 0x7fe3d8 : 0xffcf8a, 0.9f);
      cv_fill_rect(c, (float)i * w / 16 + 2, (float)j * h / 16 + 3, (float)w / 16 - 5, (float)h / 16 - 7);
    }
}

static void sign_paint(cv *c, int w, int h, int style, const char *txt) {
  static const uint32_t bg[6] = {0xf2ead8, 0x1d3a2a, 0x0c0a14, 0xdcebf5, 0xffb02e, 0x06120a},
                        ink[6] = {0x101218, 0xf2ead8, 0xff4a3a, 0x14283c, 0x101218, 0x5dff8a},
                        stripe[6] = {0xff7a1a, 0xff8ab0, 0xe8433a, 0x4aa6e0, 0x101218, 0x1f7a3a},
                        edge[6] = {0x101218, 0x6fcf8a, 0x5a1a1a, 0x14283c, 0x101218, 0x1f7a3a};
  cv_fill_hex(c, bg[style], 1); cv_round_rect(c, 0, 0, (float)w, (float)h, 10); cv_fill(c);
  cv_stroke_hex(c, edge[style], 1); cv_line_width(c, 5); cv_round_rect(c, 8, 8, (float)w - 16, (float)h - 16, 6); cv_stroke(c);
  cv_fill_hex(c, stripe[style], 1); cv_fill_rect(c, 14, 14, (float)w - 28, 8);
  char buf[160]; strncpy(buf, txt, sizeof buf - 1); buf[sizeof buf - 1] = 0;
  int line = 0;
  for (char *s = buf, *e; s; s = e ? e + 1 : NULL) {
    e = strchr(s, '|'); if (e) *e = 0;
    cv_fill_hex(c, ink[style], 1);
    float px = line == 0 ? 25.f : 17.f;
    cv_font(c, line == 0 || style == 2 ? F_DISPLAY : F_TYPE, px);
    float mw = cv_measure(c, s);
    if (mw > w - 40) { px *= (w - 40) / mw; cv_font(c, line == 0 || style == 2 ? F_DISPLAY : F_TYPE, px); }
    cv_text_align(c, CV_CENTER); cv_text_baseline(c, CV_MIDDLE);
    cv_fill_text(c, s, w / 2.f, 44.f + line * 28.f);
    line++;
    if (!e) break;
  }
}

/* Tenant complaint forms and the Superintendent's work order: paper props */
enum { FM_NOISE, FM_WINDOW, FM_VIEW, FM_FAMILY, FM_PLANET, FM_ORDER, FM_LEASE, NFORM };
static t3_material *g_form[NFORM];
static void form_paint(cv *c, int w, int h, int id) {
  static const char *head[NFORM] = {"FORM 27-B", "FORM 12-A", "FORM 3-C", "FORM 27-B", "FORM 99-Z", "WORK ORDER 0001", "LEASE NO. 1"};
  static const char *sub[NFORM] = {"NOISE COMPLAINT", "WINDOW RELOCATION", "REQUEST FOR A VIEW", "FAMILY SIZE", "DRAWING OF THE PLANET", "THE SUPERINTENDENT", "THE OWNERS"};
  static const char *body[NFORM][3] = {
    {"Subject: the flamingo", "Duration: ongoing", "Squeaking: yes"},
    {"Reason: needed light", "Previous window: tower 4", "Return: no"},
    {"Current view: a wall", "Preferred view: any", "Please advise"},
    {"Residents: 6", "Rooms: 1", "Views: 0"},
    {"", "", ""},
    {"REMOVAL OF ALL", "NON-LEASEHOLDERS,", "INCLUDING GLASS."},
    {"Term: 99 years", "Ended: a Tuesday", "Renewal: pending"} };
  bool order = id == FM_ORDER;
  cv_fill_hex(c, order ? 0xe9dfc2 : 0xf4efe0, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_stroke_hex(c, 0x9a8f78, 1); cv_line_width(c, 3); cv_stroke_rect(c, 2, 2, (float)w - 4, (float)h - 4);
  cv_fill_hex(c, order ? 0x5a1a1a : 0x2b3a58, 1); cv_fill_rect(c, 8, 8, (float)w - 16, order ? 30.f : 24.f);
  cv_fill_hex(c, 0xf4efe0, 1);
  cv_font(c, F_DISPLAY, order ? 20.f : 14.f); cv_text_align(c, CV_LEFT); cv_text_baseline(c, CV_MIDDLE);
  cv_fill_text(c, head[id], 14, order ? 24.f : 21.f);
  cv_fill_hex(c, 0x101218, 1);
  cv_font(c, F_TYPE, order ? 16.f : 11.f); cv_text_align(c, CV_CENTER);
  cv_fill_text(c, sub[id], w / 2.f, order ? 58.f : 42.f);
  if (id == FM_PLANET) {
    /* a child's crayon planet, a tiny flamingo and a house with a window */
    cv_fill_hex(c, 0x2b6fa5, 1); cv_begin(c); cv_arc(c, w * 0.5f, h * 0.5f, w * 0.3f, 0, 6.3f, false); cv_fill(c);
    cv_fill_hex(c, 0x4f8a50, 1); cv_begin(c); cv_ellipse(c, w * 0.42f, h * 0.46f, w * 0.1f, w * 0.06f, 0.5f, 0, 6.3f, false); cv_fill(c);
    cv_begin(c); cv_ellipse(c, w * 0.6f, h * 0.58f, w * 0.07f, w * 0.05f, 0, 0, 6.3f, false); cv_fill(c);
    cv_stroke_hex(c, 0xe8433a, 1); cv_line_width(c, 3); cv_begin(c); cv_move(c, w * 0.78f, h * 0.76f); cv_line(c, w * 0.78f, h * 0.66f); cv_line(c, w * 0.8f, h * 0.62f); cv_stroke(c);
    cv_fill_hex(c, 0xffd27a, 1); cv_fill_rect(c, w * 0.12f, h * 0.7f, w * 0.12f, w * 0.1f);
    cv_fill_hex(c, 0x101218, 1); cv_font(c, F_TYPE, 10.f); cv_fill_text(c, "this is where we live", w / 2.f, h - 16.f);
  } else {
    cv_font(c, F_TYPE, order ? 15.f : 10.f);
    for (int i = 0; i < 3; i++) cv_fill_text(c, body[id][i], w / 2.f, (order ? 88.f : 66.f) + i * (order ? 24.f : 18.f));
    cv_stroke_hex(c, 0xb8b0a0, 1); cv_line_width(c, 1.5f);
    for (int i = 0; i < 4; i++) { float y = (order ? 168.f : 128.f) + i * 11.f; if (y < h - 8) { cv_begin(c); cv_move(c, 12, y); cv_line(c, (float)w - 12, y); cv_stroke(c); } }
  }
  /* a stamp, a little crooked */
  cv_save(c);
  cv_translate(c, w * 0.7f, h * (order ? 0.72f : 0.86f)); cv_rotate(c, -0.25f);
  cv_stroke_hex(c, order ? 0xc03030 : 0x3a5a9a, 1); cv_line_width(c, 3); cv_stroke_rect(c, -36, -11, 72, 22);
  cv_fill_hex(c, order ? 0xc03030 : 0x3a5a9a, 1); cv_font(c, F_DISPLAY, 12.f); cv_text_align(c, CV_CENTER); cv_text_baseline(c, CV_MIDDLE);
  cv_fill_text(c, order ? "OVERDUE" : id == FM_PLANET ? "LATER" : id == FM_LEASE ? "EXPIRED" : "FILED", 0, 1);
  cv_restore(c);
  if (order) {
    cv_fill_hex(c, 0x3a3a40, 1);
    for (int i = 0; i < 4; i++) { cv_begin(c); cv_arc(c, i & 1 ? w - 12.f : 12.f, i & 2 ? h - 12.f : 12.f, 4.5f, 0, 6.3f, false); cv_fill(c); }
  }
}

static void tx_pane(cv *c, int w, int h) {
  /* a colony window: teal sky glass with a pale frame and one mullion */
  cv_fill_linear(c, 0, 0, (float)w, (float)h);
  cv_fill_stop(c, 0, "#9fe0e8"); cv_fill_stop(c, 0.55f, "#4aa6c4"); cv_fill_stop(c, 1, "#2a6a94");
  cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_alpha(c, 0.35f); cv_fill_hex(c, 0xffffff, 1);
  cv_begin(c); cv_move(c, w * 0.15f, 0); cv_line(c, w * 0.33f, 0); cv_line(c, w * 0.1f, (float)h); cv_line(c, 0, (float)h); cv_line(c, 0, h * 0.7f); cv_close(c); cv_fill(c);
  cv_alpha(c, 1);
  cv_stroke_hex(c, 0xe8f0f4, 1); cv_line_width(c, (float)w * 0.07f); cv_stroke_rect(c, 0, 0, (float)w, (float)h);
  cv_line_width(c, (float)w * 0.035f);
  cv_begin(c); cv_move(c, w / 2.f, 0); cv_line(c, w / 2.f, (float)h); cv_stroke(c);
}
static void tower_paint(cv *c, int w, int h, bool gaps) {
  /* big calm cells, so the ring reads from far away instead of shimmering */
  cv_fill_hex(c, 0x3a8aa4, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  int cols = 2, rows = 8;
  for (int j = 0; j < rows; j++)
    for (int i = 0; i < cols; i++) {
      float f = frand_seed(j * 13 + i * 7 + (gaps ? 200 : 0));
      /* a missing window is a dark hole: the Tenants have been here */
      bool gone = gaps && (f > 0.78f || (j >= 3 && j <= 4 && i == 0));
      uint32_t col = gone ? 0x06101c : f < 0.3f ? 0xffe0a8 : 0xa8f0f4;
      cv_fill_hex(c, col, 1);
      cv_fill_rect(c, (float)i * w / cols + 5, (float)j * h / rows + 4, (float)w / cols - 10, (float)h / rows - 8);
    }
}
static void tx_tower(cv *c, int w, int h) { tower_paint(c, w, h, false); }
static void tx_towergap(cv *c, int w, int h) { tower_paint(c, w, h, true); }
static void tx_moss(cv *c, int w, int h) {
  cv_fill_hex(c, 0x3c6a3a, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  for (int i = 0; i < 160; i++) {
    cv_fill_hex(c, frand_seed(i) < 0.5f ? 0x5a9a48 : 0x2c5a30, 1);
    cv_begin(c); cv_arc(c, frand_seed(i * 3 + 1) * w, frand_seed(i * 5 + 3) * h, 2 + frand_seed(i * 7) * 5, 0, 6.3f, false); cv_fill(c);
  }
}

/* Cold Storage: frosted floor panels, cream shrink-wrap, cargo belts */
static void tx_frost(cv *c, int w, int h) {
  cv_fill_hex(c, 0x9fb4c8, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  int n = 4, s = w / n;
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++) { hexcol(c, 0xa9c0d4, 0.92f + 0.2f * frand_seed(j * 7 + i + 300)); cv_fill_rect(c, (float)i * s + 2, (float)j * s + 2, (float)s - 4, (float)s - 4); }
  cv_stroke_hex(c, 0x3a4c60, 1); cv_line_width(c, 4);
  for (int i = 0; i <= n; i++) {
    cv_begin(c); cv_move(c, (float)i * s, 0); cv_line(c, (float)i * s, (float)h); cv_stroke(c);
    cv_begin(c); cv_move(c, 0, (float)i * s); cv_line(c, (float)w, (float)i * s); cv_stroke(c);
  }
  /* frost: a speckle of ice over the panels */
  cv_alpha(c, 0.55f); cv_fill_hex(c, 0xf2faff, 1);
  for (int i = 0; i < 150; i++) { cv_begin(c); cv_arc(c, frand_seed(i * 3 + 11) * w, frand_seed(i * 5 + 7) * h, 1.5f + frand_seed(i) * 4, 0, 6.3f, false); cv_fill(c); }
  cv_alpha(c, 1);
  cv_fill_hex(c, 0xffb030, 0.9f); cv_fill_rect(c, 3, (float)s * 2 - 3, 20, 5);
}
static void tx_wrap(cv *c, int w, int h) {
  cv_fill_hex(c, 0xe9e3cc, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_stroke_hex(c, 0xfffbea, 0.7f); cv_line_width(c, 2);
  for (int i = 0; i < 14; i++) { float x = frand_seed(i * 3 + 2) * w; cv_begin(c); cv_move(c, x, 0); cv_line(c, x + 24 - frand_seed(i) * 48, (float)h); cv_stroke(c); }
  cv_stroke_hex(c, 0xb8b09a, 0.6f); cv_line_width(c, 1.5f);
  for (int i = 0; i < 10; i++) { float y = frand_seed(i * 5 + 9) * h; cv_begin(c); cv_move(c, 0, y); cv_line(c, (float)w, y + 10 - frand_seed(i + 4) * 20); cv_stroke(c); }
  cv_fill_hex(c, 0x3a7ab8, 1);
  cv_fill_rect(c, 0, h * 0.22f, (float)w, 7); cv_fill_rect(c, 0, h * 0.74f, (float)w, 7);
  cv_fill_hex(c, 0xf8f6ea, 1); cv_fill_rect(c, w * 0.3f, h * 0.38f, w * 0.4f, h * 0.24f);
  cv_fill_hex(c, 0x3a3a44, 1);
  for (int i = 0; i < 3; i++) cv_fill_rect(c, w * 0.34f, h * 0.43f + i * h * 0.06f, w * (0.32f - i * 0.07f), 3);
}
static void tx_belt(cv *c, int w, int h) {
  cv_fill_hex(c, 0x2a2e38, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  cv_stroke_hex(c, 0xffb030, 1); cv_line_width(c, 6);
  for (int i = 0; i < 3; i++) {
    float y = (float)(i * h / 3 + 10);
    cv_begin(c); cv_move(c, w * 0.2f, y + h * 0.18f); cv_line(c, w * 0.5f, y); cv_line(c, w * 0.8f, y + h * 0.18f); cv_stroke(c);
  }
  cv_stroke_hex(c, 0x161a22, 1); cv_line_width(c, 3);
  for (int i = 0; i <= 3; i++) { cv_begin(c); cv_move(c, 0, (float)i * h / 3); cv_line(c, (float)w, (float)i * h / 3); cv_stroke(c); }
}
/* the Annex: colony glass in the morning, and sun-bleached roofs */
static void tx_glass(cv *c, int w, int h) {
  cv_fill_linear(c, 0, 0, 0, (float)h);
  cv_fill_stop(c, 0, "#7fd0e0"); cv_fill_stop(c, 1, "#2a7aa0");
  cv_fill_rect(c, 0, 0, (float)w, (float)h);
  int cols = 6, rows = 24;
  float cw = (float)w / cols, ch = (float)h / rows;
  for (int j = 0; j < rows; j++)
    for (int i = 0; i < cols; i++) {
      float f = frand_seed(j * 17 + i * 5 + 400);
      uint32_t col = f > 0.9f ? 0x14303c : f < 0.2f ? 0xffe2a0 : f < 0.5f ? 0x9fe8f0 : 0x5cc0d8;
      cv_fill_hex(c, col, 1);
      cv_fill_rect(c, i * cw + 1.5f, j * ch + 1.5f, cw - 3, ch - 3);
    }
  cv_stroke_hex(c, 0xe8f4f4, 1); cv_line_width(c, 1.5f);
  for (int j = 0; j <= rows; j += 4) { cv_begin(c); cv_move(c, 0, j * ch); cv_line(c, (float)w, j * ch); cv_stroke(c); }
}
static void tx_roof(cv *c, int w, int h) {
  cv_fill_hex(c, 0xcfd6d8, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  int n = 4, s = w / n;
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++) { hexcol(c, 0xd8dedd, 0.9f + 0.2f * frand_seed(j * 5 + i + 700)); cv_fill_rect(c, (float)i * s + 2, (float)j * s + 2, (float)s - 4, (float)s - 4); }
  cv_stroke_hex(c, 0x6c7a82, 1); cv_line_width(c, 3);
  for (int i = 0; i <= n; i++) {
    cv_begin(c); cv_move(c, (float)i * s, 0); cv_line(c, (float)i * s, (float)h); cv_stroke(c);
    cv_begin(c); cv_move(c, 0, (float)i * s); cv_line(c, (float)w, (float)i * s); cv_stroke(c);
  }
  cv_fill_hex(c, 0xff7a1a, 0.9f); cv_fill_rect(c, 3, (float)s * 2 - 3, 24, 6);
}
/* the Basement: steel filing cabinets and server racks */
static void tx_cabinet(cv *c, int w, int h) {
  cv_fill_hex(c, 0x5a6a62, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  int rows = 4;
  float dh = (float)h / rows;
  for (int j = 0; j < rows; j++) {
    hexcol(c, 0x6a7c72, 0.9f + 0.2f * frand_seed(j + 800));
    cv_fill_rect(c, 3, j * dh + 3, (float)w - 6, dh - 6);
    cv_fill_hex(c, 0x1c2420, 1); cv_fill_rect(c, w * 0.3f, j * dh + dh * 0.18f, w * 0.4f, dh * 0.12f);
    cv_fill_hex(c, 0xd8e0d4, 1); cv_fill_rect(c, w * 0.34f, j * dh + dh * 0.2f, w * 0.32f, dh * 0.08f);
    cv_fill_hex(c, 0x1c2420, 1); cv_fill_rect(c, w * 0.4f, j * dh + dh * 0.55f, w * 0.2f, dh * 0.1f);
  }
  cv_stroke_hex(c, 0x1c2420, 1); cv_line_width(c, 3); cv_stroke_rect(c, 1.5f, 1.5f, (float)w - 3, (float)h - 3);
}
static void tx_server(cv *c, int w, int h) {
  cv_fill_hex(c, 0x161c1a, 1); cv_fill_rect(c, 0, 0, (float)w, (float)h);
  int rows = 12;
  float uh = (float)h / rows;
  for (int j = 0; j < rows; j++) {
    cv_fill_hex(c, 0x242c2a, 1); cv_fill_rect(c, 3, j * uh + 2, (float)w - 6, uh - 4);
    cv_fill_hex(c, 0x0c100e, 1);
    for (int k = 0; k < 4; k++) cv_fill_rect(c, w * 0.08f, j * uh + 4 + k * 2.2f, w * 0.5f, 1);
    for (int k = 0; k < 4; k++) {
      float f = frand_seed(j * 9 + k + 900);
      cv_fill_hex(c, f < 0.1f ? 0xff3a2e : f < 0.7f ? 0x3dff7a : 0x1c5a34, 1);
      cv_fill_rect(c, w * 0.66f + k * 5.f, j * uh + uh * 0.35f, 3, 3);
    }
  }
  cv_stroke_hex(c, 0x3a4a44, 1); cv_line_width(c, 3); cv_stroke_rect(c, 1.5f, 1.5f, (float)w - 3, (float)h - 3);
}

static t3_material *mk(uint32_t col, t3_texture *tex, bool basic) {
  t3_material *m = basic ? t3_mesh_basic_material_new(col) : t3_mesh_standard_material_new(col);
  if (!basic) { m->metalness = 0.15f; m->roughness = 0.8f; }
  if (tex) { t3_material_set_map(m, tex); t3_release(tex); }
  return m;
}

void world_init(t3_scene *scene) {
  g_scene = scene;
  for (int i = 0; i < MD_N; i++) model_load(i);
  g_unit = t3_box_geometry_new(1, 1, 1, 1, 1, 1);
  g_mat[M_DECK] = mk(0xffffff, paint(256, 256, tx_deck, true), false);
  g_mat[M_HULL] = mk(0xffffff, paint(256, 256, tx_hull, true), false);
  g_mat[M_PLATE] = mk(0xffffff, paint(128, 64, tx_plate, true), false);
  g_mat[M_DARK] = mk(0x232738, NULL, false);
  g_mat[M_ORANGE] = mk(C_HIVIS, NULL, false);
  g_mat[M_TEAL] = mk(C_TEAL, NULL, false);
  g_mat[M_WIN] = mk(0xffffff, paint(256, 256, tx_win, true), true);
  g_mat[M_WIN2] = mk(0xffffff, paint(256, 256, tx_win2, true), true);
  g_mat[M_SOIL] = mk(0xffffff, paint(256, 256, tx_soil, true), false);
  g_mat[M_HEDGE] = mk(0xffffff, paint(256, 256, tx_hedge, true), false);
  g_mat[M_ARENA] = mk(0xffffff, paint(512, 512, tx_arena, false), false);
  g_mat[M_ARENASIDE] = mk(0x2a3040, NULL, false);
  g_mat[M_ORANGEG] = mk(C_HIVIS, NULL, true);
  g_mat[M_TEALG] = mk(C_TEAL, NULL, true);
  g_mat[M_HAZ] = mk(0xffffff, paint(128, 128, tx_haz, true), false);
  g_mat[M_SKY] = mk(0x5a6c8a, NULL, true);
  g_mat[M_RING1] = mk(C_HIVIS, NULL, true);
  g_mat[M_RING2] = mk(C_TEAL, NULL, true);
  g_mat[M_PLANET] = mk(0xffffff, paint(1024, 512, tx_planet, true), true);
  g_mat[M_PLANET]->fog = false;
  g_mat[M_HALO] = mk(0x7fb8ff, NULL, true);
  g_mat[M_HALO]->transparent = true; g_mat[M_HALO]->opacity = 0.22f; g_mat[M_HALO]->side = T3_BACK_SIDE;
  g_mat[M_HALO]->depth_write = false; g_mat[M_HALO]->fog = false;
  g_mat[M_FARHULL] = mk(0x2a3550, NULL, true);
  g_mat[M_FARHULL]->fog = false;
  g_mat[M_WOOD] = mk(0xffffff, paint(128, 128, tx_wood, true), false);
  g_mat[M_LIGHTS] = mk(0xffffff, paint(256, 256, tx_lights, true), true);
  g_mat[M_LIGHTS]->fog = false;
  g_mat[M_STAR] = mk(0xffffff, NULL, true);
  g_mat[M_STAR]->fog = false;
  for (int i = 0; i < NSIGN; i++) {
    cv *c = cv_new(256, 128);
    sign_paint(c, 256, 128, g_sign_def[i].style, g_sign_def[i].txt);
    uint8_t *px = malloc(256 * 128 * 4); cv_read(c, px); cv_free(c);
    t3_texture *t = t3_texture_new(256, 128, px); free(px);
    t->color_space = T3_SRGB_COLOR_SPACE;
    g_sign[i] = mk(0xffffff, t, true);
    g_sign[i]->side = T3_DOUBLE_SIDE;
  }
  for (int i = 0; i < NFORM; i++) {
    int w = i == FM_ORDER ? 256 : 160, h = i == FM_ORDER ? 192 : 224;
    cv *c = cv_new(w, h);
    form_paint(c, w, h, i);
    uint8_t *px = malloc((size_t)w * h * 4); cv_read(c, px); cv_free(c);
    t3_texture *t = t3_texture_new(w, h, px); free(px);
    t->color_space = T3_SRGB_COLOR_SPACE;
    g_form[i] = mk(0xffffff, t, true);
    g_form[i]->side = T3_DOUBLE_SIDE;
  }
  g_mat[M_PANE] = mk(0xffffff, paint(128, 96, tx_pane, false), true);
  g_mat[M_PINK] = mk(0xff7ab8, NULL, true);
  g_mat[M_REDG] = mk(0xff3a2e, NULL, true);
  g_mat[M_CRANE] = mk(0x4a4468, NULL, false);
  g_mat[M_TOWER] = mk(0xffffff, paint(64, 256, tx_tower, false), true); /* a full tower and a gapped one: two draws */
  g_mat[M_TOWER]->fog = false;
  g_mat[M_TOWERGAP] = mk(0xffffff, paint(64, 256, tx_towergap, false), true);
  g_mat[M_TOWERGAP]->fog = false;
  g_mat[M_COLRING] = mk(0xbfeaf0, NULL, true);
  g_mat[M_COLRING]->fog = false;
  g_mat[M_MOSS] = mk(0xffffff, paint(128, 128, tx_moss, true), false);
  g_mat[M_FROST] = mk(0xffffff, paint(256, 256, tx_frost, true), false);
  g_mat[M_WRAP] = mk(0xffffff, paint(128, 128, tx_wrap, false), false);
  g_mat[M_AMBER] = mk(0xffb030, NULL, true);
  g_mat[M_BELT] = mk(0xffffff, paint(128, 96, tx_belt, true), false);
  g_mat[M_GLASS] = mk(0xffffff, paint(128, 512, tx_glass, false), false);
  g_mat[M_ROOF] = mk(0xffffff, paint(256, 256, tx_roof, true), false);
  g_mat[M_CABINET] = mk(0xffffff, paint(64, 128, tx_cabinet, true), false);
  g_mat[M_SERVER] = mk(0xffffff, paint(64, 128, tx_server, true), false);
  g_mat[M_LED1] = mk(0x3dff7a, NULL, true);
  g_mat[M_LED2] = mk(0x3dff7a, NULL, true);
  g_mat[M_TUBE] = mk(0x6a8a80, NULL, false);
  g_mat[M_CORE] = mk(0xff2a22, NULL, true);
  g_mat[M_TENANT] = mk(0x7fe0a0, NULL, false);
  g_mat[M_EYE] = mk(0xf4f0e0, NULL, true);
  g_mat[M_TERM] = mk(0x2fe070, NULL, true);
  g_mat[M_GOLD] = mk(0xffc860, NULL, false);
  g_mat[M_RACK] = mk(0x7b97b6, NULL, false);
  g_plane1 = t3_plane_geometry_new(1, 1, 1, 1);
  g_sph = t3_sphere_geometry_new(0.5f, 14, 9);
}

/* ── scene building ── */
static void attach(void *o) { t3_object_add(g_cur ? g_cur : g_root, o); t3_release(o); }

/* a world-scaled textured box (uv in metres / tile); visual only */
static t3_mesh *tbox(v3 lo, v3 hi, int mat, float tile, bool cast) {
  float w = hi.x - lo.x, h = hi.y - lo.y, d = hi.z - lo.z;
  t3_geometry *g = t3_box_geometry_new(w, h, d, 1, 1, 1);
  float *uv = t3_attribute_f32(g->attributes[T3_ATTR_UV]);
  float du[6] = {d, d, w, w, w, w}, dv[6] = {h, h, d, d, h, h};
  for (int f = 0; f < 6; f++)
    for (int v = 0; v < 4; v++) { uv[(f * 4 + v) * 2] *= du[f] / tile; uv[(f * 4 + v) * 2 + 1] *= dv[f] / tile; }
  t3_mesh *m = t3_mesh_new(g, g_mat[mat]);
  t3_release(g);
  t3_object_set_position(m, (lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2);
  T3_OBJ(m)->cast_shadow = cast; T3_OBJ(m)->receive_shadow = true;
  attach(m);
  return m;
}
/* a shared-geometry coloured box, so the renderer can batch it */
static t3_mesh *ucube(float cx, float cy, float cz, float sx, float sy, float sz, int mat, bool cast) {
  t3_mesh *m = t3_mesh_new(g_unit, g_mat[mat]);
  t3_object_set_position(m, cx, cy, cz);
  t3_object_set_scale(m, sx, sy, sz);
  T3_OBJ(m)->cast_shadow = cast; T3_OBJ(m)->receive_shadow = true;
  attach(m);
  return m;
}
static void ubox(v3 lo, v3 hi, int mat, bool cast) {
  ucube((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2, hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, mat, cast);
}
static int col(v3 lo, v3 hi) {
  if (g_nb >= MAXB) { ds_log("world: out of boxes"); return -1; }
  g_box[g_nb] = (box_t){lo, hi, -1};
  return g_nb++;
}
static int solid(v3 lo, v3 hi, int mat, float tile, bool cast) { tbox(lo, hi, mat, tile, cast); return col(lo, hi); }
/* a strut between two points (cables, rails, bracing); visual only */
static void strut(v3 a, v3 b, float th, int mat) {
  v3 d = v3_sub(b, a);
  t3_mesh *m = t3_mesh_new(g_unit, g_mat[mat]);
  t3_object_set_position(m, (a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2);
  t3_object_set_scale(m, th, th, v3_len(d));
  t3_object_look_at(m, b.x, b.y, b.z);
  T3_OBJ(m)->receive_shadow = true;
  attach(m);
}

static t3_object *put(int md, float x, float y, float z, float yaw, float sc) {
  model_t *m = &g_md[md];
  t3_object *grp = t3_object_new();
  t3_object_set_position(grp, x, y, z);
  t3_object_set_rotation(grp, 0, yaw, 0);
  t3_object_set_scale(grp, sc, sc, sc);
  for (int i = 0; i < m->np; i++) {
    part_t *p = &m->parts[i];
    t3_mesh *me = p->nm > 1 ? t3_mesh_new_multi(p->g, p->ms, p->nm) : t3_mesh_new(p->g, p->ms[0]);
    t3_object_set_position(me, p->pos.x, p->pos.y, p->pos.z);
    t3_object_set_quaternion(me, p->q);
    t3_object_set_scale(me, p->sc.x, p->sc.y, p->sc.z);
    T3_OBJ(me)->cast_shadow = T3_OBJ(me)->receive_shadow = true;
    t3_object_add(grp, me); t3_release(me);
  }
  attach(grp);
  return grp;
}
/* put() plus a collision box fitted to the model's bounds (k shrinks it) */
static t3_object *putcol(int md, float x, float y, float z, float yaw, float sc, float k) {
  model_t *m = &g_md[md];
  float hx = (m->hi.x - m->lo.x) / 2 * sc * k, hz = (m->hi.z - m->lo.z) / 2 * sc * k;
  float c = fabsf(cosf(yaw)), s = fabsf(sinf(yaw));
  float ex = c * hx + s * hz, ez = s * hx + c * hz;
  col(v3_(x - ex, y + m->lo.y * sc, z - ez), v3_(x + ex, y + m->hi.y * sc, z + ez));
  return put(md, x, y, z, yaw, sc);
}
static void spinner(t3_object *o, float ax, float ay, float az, float rate) {
  if (g_nspin < 48) g_spin[g_nspin++] = (spin_t){o, v3_(ax, ay, az), rate};
}

static int g_lip = M_ORANGE, g_band = M_TEALG, g_platmat = M_PLATE;   /* per level: edge strips, cover band, moving platform top */
/* a platform slab: textured top, hazard lip, a dark hull and keel below for silhouette */
static void deck_vis(float x0, float z0, float x1, float z1, float y, float th, int mat, bool under) {
  tbox(v3_(x0, y - th, z0), v3_(x1, y, z1), mat, 8, false);
  float w = x1 - x0, d = z1 - z0, lip = 0.7f;
  ucube((x0 + x1) / 2, y + 0.07f, z0 + lip / 2, w, 0.14f, lip, g_lip, false);
  ucube((x0 + x1) / 2, y + 0.07f, z1 - lip / 2, w, 0.14f, lip, g_lip, false);
  ucube(x0 + lip / 2, y + 0.07f, (z0 + z1) / 2, lip, 0.14f, d - 2 * lip, g_lip, false);
  ucube(x1 - lip / 2, y + 0.07f, (z0 + z1) / 2, lip, 0.14f, d - 2 * lip, g_lip, false);
  if (!under) return;
  float ins = fminf(w, d) * 0.12f, hd = fminf(9.f, fminf(w, d) * 0.5f);
  tbox(v3_(x0 + ins, y - th - hd, z0 + ins), v3_(x1 - ins, y - th, z1 - ins), M_HULL, 12, false);
  /* the dark keel is for the big decks only: every extra box is a draw call */
  if (w * d < 1800) return;
  tbox(v3_(x0 + ins * 2.2f, y - th - hd * 1.9f, z0 + ins * 2.2f), v3_(x1 - ins * 2.2f, y - th - hd, z1 - ins * 2.2f), M_DARK, 12, false);
  ucube((x0 + x1) / 2, y - th - hd * 0.5f, z0 + ins - 0.05f, w - 2 * ins, 0.5f, 0.2f, M_TEALG, false);
}
static void deck(float x0, float z0, float x1, float z1, float y, float th, int mat) {
  col(v3_(x0, y - th, z0), v3_(x1, y, z1));
  deck_vis(x0, z0, x1, z1, y, th, mat, true);
}
static void cover(float cx, float cz, float w, float d, float h, float y) {
  v3 lo = v3_(cx - w / 2, y, cz - d / 2), hi = v3_(cx + w / 2, y + h, cz + d / 2);
  solid(lo, hi, M_PLATE, 2.5f, true);
  ucube(cx, y + h - 0.08f, cz, w + 0.12f, 0.16f, d + 0.12f, g_lip, false);
  ucube(cx, y + h * 0.5f, cz, w + 0.06f, 0.12f, d + 0.06f, g_band, false);
}
static void wall(float x0, float z0, float x1, float z1, float y, float h, int mat) {
  solid(v3_(x0, y, z0), v3_(x1, y + h, z1), mat, 6, true);
  ucube((x0 + x1) / 2, y + h + 0.1f, (z0 + z1) / 2, x1 - x0 + 0.2f, 0.2f, z1 - z0 + 0.2f, g_lip, false);
}
/* dir 0 +z, 1 -z, 2 +x, 3 -x; (x, z) is the low end centre; the flight rises n * rise */
static void stairs(float x, float y0, float z, int dir, float w, int n, float rise, float run) {
  float dx = dir == 2 ? 1.f : dir == 3 ? -1.f : 0.f, dz = dir == 0 ? 1.f : dir == 1 ? -1.f : 0.f;
  float sx = dx ? run : w, sz = dz ? run : w;
  for (int i = 0; i < n; i++) {
    float cx = x + dx * (i + 0.5f) * run, cz = z + dz * (i + 0.5f) * run, top = y0 + (i + 1) * rise;
    v3 lo = v3_(cx - sx / 2, y0, cz - sz / 2), hi = v3_(cx + sx / 2, top, cz + sz / 2);
    col(lo, hi);
    ucube(cx, (y0 + top) / 2, cz, sx, top - y0, sz, i & 1 ? M_PLATE : M_DARK, i % 4 == 0);
    ucube(cx, top - 0.04f, cz, sx, 0.08f, sz, M_ORANGE, false);
  }
  /* rails */
  float len = n * run, hgt = n * rise;
  for (int sd = -1; sd <= 1; sd += 2) {
    float ox = dz ? sd * (w / 2 - 0.1f) : 0, oz = dx ? sd * (w / 2 - 0.1f) : 0;
    v3 a = v3_(x + ox, y0 + 1.1f, z + oz), b = v3_(x + ox + dx * len, y0 + hgt + 1.1f, z + oz + dz * len);
    strut(a, b, 0.12f, M_ORANGE);
    for (int i = 0; i <= n; i += 4) {
      float px = x + ox + dx * i * run, pz = z + oz + dz * i * run;
      ucube(px, y0 + i * rise + 0.55f, pz, 0.1f, 1.1f, 0.1f, M_DARK, false);
    }
  }
}
static void sign(float x, float y, float z, float yaw, int id, float w) {
  t3_geometry *g = t3_plane_geometry_new(w, w / 2, 1, 1);
  t3_mesh *m = t3_mesh_new(g, g_sign[id % NSIGN]);
  t3_release(g);
  t3_object_set_position(m, x, y, z);
  t3_object_set_rotation(m, 0, yaw, 0);   /* the face looks along +z rotated by yaw; DS_PI faces a player running +z */
  attach(m);
  ucube(x - 0.12f * sinf(yaw), y - w / 4 - 0.8f, z - 0.12f * cosf(yaw), 0.18f, 1.6f, 0.18f, M_DARK, false);
}
static void beam(float x, float z0, float z1, float y, float w) { ucube(x, y, (z0 + z1) / 2, w, w, z1 - z0, M_DARK, false); }

/* ── story dressing: zones, anchors, gates, forms, nests, cranes, the colony ── */
typedef struct { v3 lo, hi; int beat; bool fired; } zone_t;
typedef struct { const char *name; v3 pos; float yaw; } anchor_t;
typedef struct { v3 lo, hi; int bi; t3_object *vis; float h, open; } gate_t;
static zone_t g_zone[8];
static int g_nzone;
static anchor_t g_anc[8];
static int g_nanc;
static gate_t g_gate[4];
static int g_ngate;
static bool g_gates_closed;
static v3 g_gate_probe;   /* enemies near here (outside the doors) keep the gates from closing: nobody gets locked out with a grunt */
static float g_gate_r;

static void zone(int beat, float x0, float y0, float z0, float x1, float y1, float z1) {
  if (g_nzone < 8) g_zone[g_nzone++] = (zone_t){v3_(x0, y0, z0), v3_(x1, y1, z1), beat, false};
}
static void anchor(const char *name, v3 p, float yaw) { if (g_nanc < 8) g_anc[g_nanc++] = (anchor_t){name, p, yaw}; }

/* a door across an arena mouth: a hazard slab that slides up into a header; the collider follows world_set_gates */
static void gate(float x0, float x1, float z, float y, float h) {
  if (g_ngate >= 4) return;
  gate_t *g = &g_gate[g_ngate++];
  v3 lo = v3_(x0, y, z - 0.6f), hi = v3_(x1, y + h, z + 0.6f);
  t3_object *grp = t3_object_new();
  t3_object_add(g_cur ? g_cur : g_root, grp); t3_release(grp);
  g_cur = grp;
  tbox(lo, hi, M_HAZ, 3.5f, true);
  ucube((x0 + x1) / 2, y + h * 0.5f, z, x1 - x0 - 1.2f, 0.5f, 1.4f, M_DARK, false);
  g_cur = NULL;
  ubox(v3_(x0 - 0.4f, y + h, z - 0.9f), v3_(x1 + 0.4f, y + h + 1.2f, z + 0.9f), M_ORANGE, true);
  g->lo = lo; g->hi = hi; g->vis = grp; g->h = h; g->open = 1;
  g->bi = col(lo, hi);
  g_box[g->bi].lo = g_box[g->bi].hi = v3_(0, -9999, 0);
  t3_object_set_position(grp, 0, h, 0);
}

/* flat paper: lying on a deck (tilt flat) or pinned upright, facing yaw */
static void form(float x, float y, float z, float yaw, int id, float w, bool flat) {
  t3_mesh *m = t3_mesh_new(g_plane1, g_form[id]);
  float h = id == FM_ORDER ? w * 0.75f : w * 1.4f;
  t3_object_set_scale(m, w, h, 1);
  if (flat) { t3_object_set_position(m, x, y + 0.035f, z); t3_object_set_rotation(m, -DS_PI / 2, 0, yaw + DS_PI); }
  else { t3_object_set_position(m, x, y, z); t3_object_set_rotation(m, 0, yaw, 0); }
  attach(m);
}
/* a signpost with a form pinned to it, for the walkways */
static void form_post(float x, float y, float z, float yaw, int id) {
  ucube(x, y + 0.7f, z, 0.1f, 1.4f, 0.1f, M_DARK, false);
  form(x + 0.07f * sinf(yaw), y + 1.35f, z + 0.07f * cosf(yaw), yaw, id, 0.75f, false);
}

/* a window pane box: colony glass in a pale frame, the Tenants' building block */
static t3_object *pane(float cx, float cy, float cz, float sx, float sy, float sz, float rx, float ry, float rz) {
  t3_mesh *m = t3_mesh_new(g_unit, g_mat[M_PANE]);
  t3_object_set_position(m, cx, cy, cz);
  t3_object_set_scale(m, sx, sy, sz);
  t3_object_set_rotation(m, rx, ry, rz);
  T3_OBJ(m)->receive_shadow = true;
  attach(m);
  return T3_OBJ(m);
}
/* a Tenant nest of stacked colony panes with a warm lamp inside and a collision box;
 * u scales it, yaw turns its doorway (local +z) */
static void nest(float x, float y, float z, float yaw, float u) {
  t3_object *grp = t3_object_new();
  t3_object_set_position(grp, x, y, z);
  t3_object_set_rotation(grp, 0, yaw, 0);
  t3_object_add(g_cur ? g_cur : g_root, grp); t3_release(grp);
  t3_object *keep = g_cur; g_cur = grp;
  pane(0, 1.3f * u, 0, 4.2f * u, 2.6f * u, 3.4f * u, 0, 0, 0);
  pane(0.5f * u, 3.5f * u, 0.2f * u, 3.0f * u, 1.9f * u, 2.6f * u, 0, 0.35f, 0);
  pane(-0.4f * u, 5.1f * u, 0.1f * u, 1.8f * u, 1.4f * u, 1.7f * u, 0, -0.25f, 0);
  pane(0.2f * u, 6.15f * u, 0, 3.4f * u, 0.14f, 2.4f * u, 0.12f, 0.2f, 0.28f);
  pane(-2.4f * u, 0.9f * u, 1.6f * u, 0.14f, 2.2f * u, 1.8f * u, 0, 0, 0.2f);   /* a pane leaning on the wall */
  ucube(0, 1.0f * u, 1.72f * u, 1.1f * u, 1.7f * u, 0.12f, M_DARK, false);       /* the doorway */
  ucube(0, 1.1f * u, 1.6f * u, 0.5f * u, 0.5f * u, 0.3f, M_PINK, false);         /* the lamp */
  ucube(1.6f * u, 4.7f * u, 1.45f * u, 0.5f * u, 0.4f * u, 0.4f * u, M_PINK, false);
  g_cur = keep;
  float c = fabsf(cosf(yaw)), sn = fabsf(sinf(yaw));
  float hx = 2.0f * u, hz = 1.65f * u, ex = c * hx + sn * hz, ez = sn * hx + c * hz;
  col(v3_(x - ex, y, z - ez), v3_(x + ex, y + 2.6f * u, z + ez));
}
/* panes leant in a stack against a rack, waiting to be carried off */
static void pane_stack(float x, float y, float z, float yaw, int n) {
  t3_object *grp = t3_object_new();
  t3_object_set_position(grp, x, y, z);
  t3_object_set_rotation(grp, 0, yaw, 0);
  t3_object_add(g_cur ? g_cur : g_root, grp); t3_release(grp);
  t3_object *keep = g_cur; g_cur = grp;
  for (int i = 0; i < n; i++) pane(0, 1.5f + 0.05f * i, -0.0f + i * 0.2f, 3.0f, 2.8f, 0.14f, -0.28f, 0, 0);
  ucube(0, 0.15f, 0.7f, 3.4f, 0.3f, 1.6f, M_DARK, false);
  g_cur = keep;
  float c = fabsf(cosf(yaw)), sn = fabsf(sinf(yaw));
  col(v3_(x - (c * 1.6f + sn * 1.0f), y, z - (sn * 1.6f + c * 1.0f)), v3_(x + (c * 1.6f + sn * 1.0f), y + 2.2f, z + (sn * 1.6f + c * 1.0f)));
}

/* a model with collision fitted to its footprint (kx shrink) and the lower kh of its height */
static t3_object *putfit(int md, float x, float y, float z, float yaw, float sc, float kx, float kh) {
  model_t *m = &g_md[md];
  float hx = (m->hi.x - m->lo.x) / 2 * sc * kx, hz = (m->hi.z - m->lo.z) / 2 * sc * kx;
  float c = fabsf(cosf(yaw)), s = fabsf(sinf(yaw));
  float ex = c * hx + s * hz, ez = s * hx + c * hz;
  col(v3_(x - ex, y + m->lo.y * sc, z - ez), v3_(x + ex, y + m->lo.y * sc + (m->hi.y - m->lo.y) * sc * kh, z + ez));
  return put(md, x, y, z, yaw, sc);
}

/* a crane: a lattice mast, cab, jib with a hook cable, counterweight and warning lamps; solid at the foot */
static void crane(float x, float y, float z, float h, float jib, float dir) {
  const float w = 2.4f;
  for (int sx = -1; sx <= 1; sx += 2) for (int sz = -1; sz <= 1; sz += 2) ucube(x + sx * w, y + h / 2, z + sz * w, 0.8f, h, 0.8f, M_CRANE, false);
  for (float yy = 4; yy < h; yy += 7) {
    ucube(x, yy + y, z - w, w * 2, 0.35f, 0.35f, M_CRANE, false); ucube(x, yy + y, z + w, w * 2, 0.35f, 0.35f, M_CRANE, false);
    ucube(x - w, yy + y, z, 0.35f, 0.35f, w * 2, M_CRANE, false); ucube(x + w, yy + y, z, 0.35f, 0.35f, w * 2, M_CRANE, false);
    strut(v3_(x - w, yy + y - 3.5f, z - w), v3_(x + w, yy + y + 3.5f, z - w), 0.22f, M_CRANE);
    strut(v3_(x + w, yy + y - 3.5f, z + w), v3_(x - w, yy + y + 3.5f, z + w), 0.22f, M_CRANE);
  }
  ucube(x, y + h + 2, z, 5.2f, 4, 5.2f, M_DARK, false);
  ucube(x, y + h + 2.4f, z + 2.7f, 3.2f, 1.6f, 0.2f, M_ORANGEG, false);
  ucube(x + dir * jib / 2, y + h + 3.2f, z, jib, 1.6f, 2.0f, M_CRANE, false);
  ucube(x - dir * 9, y + h + 3.2f, z, 14, 1.6f, 2.0f, M_CRANE, false);
  ucube(x - dir * 14, y + h + 1.6f, z, 4, 3.4f, 3.4f, M_DARK, false);
  ucube(x + dir * (jib - 6), y + h - h * 0.18f, z, 0.12f, h * 0.36f + 3.2f, 0.12f, M_DARK, false);
  ucube(x + dir * (jib - 6), y + h - h * 0.36f, z, 1.2f, 1.4f, 1.2f, M_ORANGEG, false);
  ucube(x, y + h + 4.6f, z, 0.7f, 0.7f, 0.7f, M_REDG, false);
  ucube(x + dir * jib, y + h + 3.2f, z, 0.7f, 0.7f, 0.7f, M_REDG, false);
  col(v3_(x - w - 0.5f, y, z - w - 0.5f), v3_(x + w + 0.5f, y + h * 0.5f, z + w + 0.5f));
}

/* Halcyon Reach: a ring of glass towers pointing inward, seen in the sky. The ring lies in the
 * plane tilted by (tx, tz); some towers have lost windows. */
static void colony(float cx, float cy, float cz, float R, float tx, float tz) {
  t3_object *grp = t3_object_new();
  t3_object_set_position(grp, cx, cy, cz);
  t3_object_set_rotation(grp, tx, 0, tz);
  t3_object_add(g_cur ? g_cur : g_root, grp); t3_release(grp);
  t3_object *keep = g_cur; g_cur = grp;
  const int n = 28;
  for (int i = 0; i < n; i++) {
    float a = i * 2 * DS_PI / n, len = R * (0.16f + 0.14f * frand_seed(i * 5 + 3)), tw = R * 0.05f;
    float px = cosf(a) * (R - len / 2), pz = sinf(a) * (R - len / 2);
    t3_mesh *m = t3_mesh_new(g_unit, g_mat[(i % 6 == 2) ? M_TOWERGAP : M_TOWER]);
    t3_object_set_position(m, px, 0, pz);
    t3_object_set_scale(m, tw, tw * 1.3f, len);
    t3_object_set_rotation(m, 0, atan2f(-cosf(a), -sinf(a)), 0);
    T3_OBJ(m)->frustum_culled = false;
    attach(m);
  }
  t3_geometry *tg = t3_torus_geometry_new(R, R * 0.018f, 6, 64, DS_PI * 2);
  t3_mesh *tm = t3_mesh_new(tg, g_mat[M_COLRING]);
  t3_release(tg);
  t3_object_set_rotation(tm, DS_PI / 2, 0, 0);
  T3_OBJ(tm)->frustum_culled = false;
  attach(tm);
  g_cur = keep;
}

/* pickups, checkpoints, enemies, rings */
static const int pk_md[PK_COUNT] = {MD_PK_HEALTH, MD_PK_AMMO, MD_PK_JAR, MD_PK_SPHERE};
static void pk(int kind, float x, float y, float z) {
  if (g_npk >= 48) return;
  t3_object *o = put(pk_md[kind], x, y + 1.0f, z, 0, 1.7f);
  ucube(x, y + 0.06f, z, 1.5f, 0.12f, 1.5f, kind == PK_HEALTH ? M_ORANGEG : M_TEALG, false);
  g_pk[g_npk++] = (pick_t){kind, v3_(x, y + 1.0f, z), false, o, rnd() * 6};
}
static void cp(float x, float y, float z) {
  if (g_ncp >= 8) return;
  float fx = x + 5.2f;
  ucube(fx, y + 3.5f, z, 0.25f, 7, 0.25f, M_DARK, true);
  ucube(fx, y + 0.3f, z, 1.2f, 0.6f, 1.2f, M_DARK, true);
  t3_mesh *f = ucube(fx - 1.7f, y + 6.2f, z, 3.4f, 1.3f, 0.12f, M_ORANGEG, false);
  ucube(x, y + 0.05f, z, 5.6f, 0.1f, 3.2f, M_TEALG, false);
  ucube(x, y + 0.06f, z, 4.4f, 0.12f, 2.0f, M_DARK, false);
  g_cp[g_ncp++] = (cp_t){v3_(x, y, z), T3_OBJ(f), false};
}
static float floor_at(float x, float z, float from) {
  float best = -1e9f;
  for (int i = 0; i < g_nb; i++) {
    const box_t *b = &g_box[i];
    if (x > b->lo.x && x < b->hi.x && z > b->lo.z && z < b->hi.z && b->hi.y <= from + 0.6f && b->hi.y > best) best = b->hi.y;
  }
  return best;
}
static void en(int type, float x, float z, float from) {
  if (g_nsp >= 160) return;
  float y = floor_at(x, z, from);
  if (y < -1e8f) y = from;
  float yaw = DS_PI + rr(-0.5f, 0.5f);
  if (type == EN_FLYER) y += rr(4.5f, 8.f);
  g_sp[g_nsp++] = (spawn_t){type, v3_(x, y, z), yaw};
}
static void enh(int type, float x, float y, float z) { if (g_nsp < 160) g_sp[g_nsp++] = (spawn_t){type, v3_(x, y, z), DS_PI}; }

static void rings(float drop_h, float weave) {
  g_start = v3_add(g_spawn, v3_(0, drop_h, -70));
  g_nring = 10;
  for (int i = 0; i < g_nring; i++) {
    float t = 0.1f + 0.8f * i / 9.f;
    v3 p = v3_lerp(g_start, v3_add(g_spawn, v3_(0, 4, 0)), t);
    p.x += sinf(t * 9.f) * weave * (1 - t);
    p.y += cosf(t * 7.f) * 3.f * (1 - t);
    g_ring[i] = p;
    t3_geometry *g = t3_torus_geometry_new(6.5f, 0.45f, 12, 28, DS_PI * 2);
    t3_mesh *m = t3_mesh_new(g, g_mat[i & 1 ? M_RING2 : M_RING1]);
    t3_release(g);
    t3_object_set_position(m, p.x, p.y, p.z);
    v3 dir = v3_norm(v3_sub(v3_add(g_spawn, v3_(0, 4, 0)), g_start));
    t3_object_look_at(m, p.x + dir.x * 10, p.y + dir.y * 10, p.z + dir.z * 10);
    attach(m);
    g_ring_vis[i] = T3_OBJ(m);
    g_ring_got[i] = false;
  }
}

/* sky: a planet below, haze halo, and far hull silhouettes with lit windows */
static void planet(float cx, float cy, float cz, float r) {
  t3_geometry *g = t3_sphere_geometry_new(r, 48, 24);
  t3_mesh *m = t3_mesh_new(g, g_mat[M_PLANET]);
  t3_release(g);
  t3_object_set_position(m, cx, cy, cz);
  t3_object_set_rotation(m, DS_PI / 2, 0.6f, 0);
  T3_OBJ(m)->frustum_culled = false;
  attach(m);
  g = t3_sphere_geometry_new(r * 1.035f, 48, 24);
  m = t3_mesh_new(g, g_mat[M_HALO]);
  t3_release(g);
  t3_object_set_position(m, cx, cy, cz);
  T3_OBJ(m)->frustum_culled = false;
  attach(m);
}
static void far_hull(float x, float y, float z, float len, float rad, float yaw) {
  t3_geometry *g = t3_cylinder_geometry_new(rad, rad * 0.8f, len, 14, 1, false);
  t3_geometry_rotate_x(g, DS_PI / 2);
  float *uv = t3_attribute_f32(g->attributes[T3_ATTR_UV]);
  for (int i = 0; i < g->attributes[T3_ATTR_UV]->count; i++) { uv[i * 2] *= 10; uv[i * 2 + 1] *= len / 40; }
  t3_mesh *m = t3_mesh_new(g, g_mat[M_LIGHTS]);
  t3_release(g);
  t3_object_set_position(m, x, y, z);
  t3_object_set_rotation(m, 0, yaw, 0);
  T3_OBJ(m)->frustum_culled = false;
  attach(m);
  /* spires and the bow */
  for (int i = 0; i < 6; i++) {
    float f = rr(-0.45f, 0.45f);
    t3_object *s = put(MD_BLDG, x + sinf(yaw) * len * f, y + rad * 0.85f, z + cosf(yaw) * len * f, yaw, rad / 4.f);
    (void)s;
  }
}
static void stars(int n) {
  for (int i = 0; i < n; i++) {
    float th = rr(0, 6.283f), ph = rr(-0.1f, 1.2f), R = 1900;
    t3_mesh *m = t3_mesh_new(g_unit, g_mat[M_STAR]);
    t3_object_set_position(m, cosf(th) * cosf(ph) * R, sinf(ph) * R, 300 + sinf(th) * cosf(ph) * R);
    float s = rr(3, 9); t3_object_set_scale(m, s, s, s);
    T3_OBJ(m)->frustum_culled = false;
    attach(m);
  }
}
static void facade(float x0, float z0, float x1, float z1, float y0, float y1, int mat, float tile) {
  tbox(v3_(x0, y0, z0), v3_(x1, y1, z1), mat, tile, false);
  bool along_z = (z1 - z0) > (x1 - x0);
  for (float t = along_z ? z0 + 20 : x0 + 20; t < (along_z ? z1 : x1); t += 45) {
    float h = y1 - y0;
    if (along_z) ucube((x0 + x1) / 2, (y0 + y1) / 2, t, x1 - x0 + 5, h, 5, M_DARK, false);
    else ucube(t, (y0 + y1) / 2, (z0 + z1) / 2, 5, h, z1 - z0 + 5, M_DARK, false);
  }
  for (float yy = y0 + 30; yy < y1; yy += 60) {
    if (along_z) ucube((x0 + x1) / 2, yy, (z0 + z1) / 2, x1 - x0 + 6, 1.2f, z1 - z0, M_TEALG, false);
    else ucube((x0 + x1) / 2, yy, (z0 + z1) / 2, x1 - x0, 1.2f, z1 - z0 + 6, M_TEALG, false);
  }
}
static void scatter_props(int md, int n, float x0, float z0, float x1, float z1, float y, float s0, float s1) {
  for (int i = 0; i < n; i++) put(md, rr(x0, x1), y, rr(z0, z1), rr(0, 6.28f), rr(s0, s1));
}
static void floaters(float x0, float x1, float z0, float z1, float y0, float y1, int n) {
  for (int i = 0; i < n; i++) {
    float x = rr(x0, x1), z = rr(z0, z1), y = rr(y0, y1);
    if (fabsf(x) < 60) x = x < 0 ? x - 70 : x + 70;
    put(MD_ROCKL, x, y, z, rr(0, 6.28f), rr(5, 12));
    if (rnd() < 0.5f) put(MD_TREEF, x, y + 8, z, rr(0, 6.28f), rr(3, 6));
  }
}

/* a moving platform: its group moves with off; the box follows */
static plat_t *plat(float x0, float z0, float x1, float z1, float y, float th, v3 axis, float period, float phase, bool vertical) {
  if (g_np >= 16) return NULL;
  plat_t *p = &g_plat[g_np++];
  t3_object *grp = t3_object_new();
  t3_object_add(g_root, grp); t3_release(grp);
  g_cur = grp;
  deck_vis(x0, z0, x1, z1, y, th, g_platmat, false);
  /* underside thruster pods */
  ucube((x0 + x1) / 2, y - th - 0.6f, (z0 + z1) / 2, (x1 - x0) * 0.5f, 1.2f, (z1 - z0) * 0.5f, M_DARK, false);
  ucube((x0 + x1) / 2, y - th - 1.25f, (z0 + z1) / 2, (x1 - x0) * 0.3f, 0.2f, (z1 - z0) * 0.3f, M_TEALG, false);
  g_cur = NULL;
  p->bi = col(v3_(x0, y - th, z0), v3_(x1, y, z1));
  g_box[p->bi].plat = (int)(p - g_plat);
  p->lo = g_box[p->bi].lo; p->hi = g_box[p->bi].hi;
  p->axis = axis; p->period = period; p->phase = phase; p->vis = grp;
  p->off = p->delta = v3_(0, 0, 0);
  (void)vertical;
  return p;
}

/* ── level 0: Curtain Wall ── */
static void build_l0(void) {
  g_spawn = v3_(0, 0, 8);
  g_kill_y = -55;
  /* backdrop: facades both sides, planet below, far hulls */
  facade(-96, -80, -78, 640, -120, 180, M_WIN, 14);
  facade(78, -80, 96, 640, -120, 220, M_WIN, 14);
  planet(300, -3300, 300, 2500);
  far_hull(-520, 60, 400, 1100, 120, 0.1f);
  far_hull(620, 160, 900, 1400, 150, -0.2f);
  floaters(-300, 300, 0, 600, -130, -60, 10);
  colony(-120, 280, 1480, 400, -0.5f, 0.22f);

  /* Z1 landing deck */
  deck(-24, -14, 24, 64, 0, 2.5f, M_DECK);
  putcol(MD_CARGO, -17, 0, 4, 0.4f, 1.2f, 0.85f);
  putcol(MD_GEN, 17, 0, -2, 0.5f, 5, 0.9f);
  wall(-24, -14, 24, -12.5f, 0, 5, M_HULL);
  cover(-9, 36, 4, 2, 1.3f, 0); cover(8, 42, 5, 2, 1.3f, 0); cover(4, 52, 3, 3, 1.3f, 0);
  putcol(MD_BARRELS, -15, 0, 28, 0, 5, 0.9f);
  putcol(MD_BARRELS, 16, 0, 33, 1, 5, 0.9f);
  cp(0, 0, 8); g_cp[0].pos = g_spawn;
  pk(PK_HEALTH, -16, 0, 50); pk(PK_SNIPE_AMMO, 18, 0, 52);
  en(EN_GRUNT, -10, 44, 1); en(EN_GRUNT, 9, 48, 1); en(EN_GRUNT, 0, 58, 1);

  /* Z2 road, stairs to the upper road, turret nests */
  deck(-13, 64, 13, 112, 0, 2.2f, M_DECK);
  wall(-13.4f, 64, -12.6f, 100, 0, 1.2f, M_PLATE); wall(12.6f, 64, 13.4f, 100, 0, 1.2f, M_PLATE);
  cover(-5, 76, 4, 1.6f, 1.3f, 0); cover(5, 84, 4, 1.6f, 1.3f, 0); cover(-4, 94, 5, 1.6f, 1.3f, 0);
  en(EN_GRUNT, -6, 74, 1); en(EN_GRUNT, 6, 82, 1); en(EN_GRUNT, -7, 94, 1); en(EN_GRUNT, 8, 98, 1); en(EN_GRUNT, 0, 104, 1);
  pk(PK_DECOY, -10, 0, 90);
  stairs(0, 0, 112, 0, 26, 8, 0.4f, 1.0f);
  deck(-13, 120, 13, 164, 3.2f, 2.2f, M_DECK);
  deck(13, 124, 27, 142, 3.2f, 2.2f, M_DECK); deck(-27, 124, -13, 142, 3.2f, 2.2f, M_DECK);
  cover(-4, 132, 4, 1.6f, 1.3f, 3.2f); cover(5, 144, 4, 1.6f, 1.3f, 3.2f); cover(0, 154, 6, 1.6f, 1.3f, 3.2f);
  cover(21, 133, 3, 3, 1.4f, 3.2f); cover(-21, 133, 3, 3, 1.4f, 3.2f);
  putfit(MD_RADAR, -23, 3.2f, 139, 2, 2, 0.35f, 1); spinner(putfit(MD_RADAR, 23, 3.2f, 139, 1, 2, 0.35f, 1), 0, 1, 0, 1.2f);
  enh(EN_TURRET, 21, 4.6f, 135); enh(EN_TURRET, -21, 4.6f, 135);
  en(EN_GRUNT, -6, 140, 4); en(EN_GRUNT, 6, 148, 4); en(EN_GRUNT, -3, 158, 4);
  en(EN_FLYER, 0, 150, 4); en(EN_FLYER, 8, 156, 4);
  pk(PK_HEALTH, 22, 3.2f, 128); pk(PK_SNIPE_AMMO, -22, 3.2f, 128);

  /* gap 1 (26 m): a drifting service pad in the middle */
  deck(-5, 172, 5, 180, 0.2f, 1.5f, M_DECK);
  pk(PK_SHIELD, 0, 0.2f, 176);
  /* Z4 solar field */
  deck(-34, 190, 34, 292, -5, 3, M_DECK);
  for (int r = 0; r < 4; r++)
    for (int s = -1; s <= 1; s += 2) {
      float z = 202 + r * 22, x = s * 19;
      putcol(MD_SOLARST, x, -5, z, 0, 2.8f, 0.6f);
      put(MD_SOLAR, x, -5, z, 0, 2.8f);
      if (r == 1 || r == 3) { putcol(MD_SOLARST, s * 29, -5, z + 8, 0, 2.2f, 0.6f); put(MD_SOLAR, s * 29, -5, z + 8, 0, 2.2f); }
    }
  cover(0, 214, 4, 1.8f, 1.3f, -5); cover(-5, 236, 3, 3, 1.3f, -5); cover(6, 252, 4, 1.8f, 1.3f, -5); cover(0, 272, 6, 1.8f, 1.3f, -5);
  putcol(MD_GEN, -8, -5, 262, 0, 5, 0.9f); putcol(MD_DISH, 9, -5, 228, 0.5f, 6, 0.9f);
  cp(0, -5, 244);
  pk(PK_HEALTH, -30, -5, 214); pk(PK_SNIPE_AMMO, 30, -5, 236); pk(PK_DECOY, 0, -5, 286);
  float zs[] = {200, 210, 224, 236, 250, 262, 274, 284};
  for (int i = 0; i < 8; i++) en(EN_GRUNT, (i & 1 ? 1 : -1) * rr(4, 12), zs[i], -4);
  en(EN_FLYER, -10, 240, -5); en(EN_FLYER, 12, 256, -5); en(EN_FLYER, 0, 275, -5);
  enh(EN_TURRET, -31, -5, 224); enh(EN_TURRET, 31, -5, 262);

  /* Z5 tower stairs */
  deck(-9, 290, 9, 294, -5, 3, M_DECK);
  stairs(0, -5, 292, 0, 10, 36, 0.4f, 0.9f);
  en(EN_GRUNT, -3, 296, -4);
  /* top: wide landing, then the narrow sniper bridge */
  deck(-12, 324, 12, 340, 9.4f, 2.5f, M_DECK);
  cover(-7, 331, 3, 1.4f, 1.3f, 9.4f); cover(7, 331, 3, 1.4f, 1.3f, 9.4f);
  enh(EN_TURRET, -9, 9.4f, 333); enh(EN_TURRET, 9, 9.4f, 333);
  deck(-3.5f, 340, 3.5f, 402, 9.4f, 1.2f, M_PLATE);
  for (float z = 346; z < 402; z += 14) { strut(v3_(-3.2f, 9.4f, z), v3_(-3.2f, 9.4f, z + 0.1f), 0.1f, M_DARK); }
  for (int s = -1; s <= 1; s += 2) {
    strut(v3_(s * 3.4f, 10.5f, 340), v3_(s * 3.4f, 10.5f, 402), 0.12f, M_ORANGE);
    for (float z = 342; z < 402; z += 8) ucube(s * 3.4f, 9.95f, z, 0.1f, 1.1f, 0.1f, M_DARK, false);
  }
  /* pylons with turret perches and sniper decks */
  for (int i = 0; i < 3; i++) {
    float z = 352 + i * 22;
    for (int s = -1; s <= 1; s += 2) {
      float x = s * (i & 1 ? 21.f : 24.f);
      solid(v3_(x - 1.5f, -40, z - 1.5f), v3_(x + 1.5f, 12.5f, z + 1.5f), M_HULL, 10, true);
      strut(v3_(x, 12, z), v3_(s * 3.4f, 10.5f, z), 0.18f, M_DARK);
      ucube(x, 12.7f, z, 3.6f, 0.4f, 3.6f, M_ORANGE, false);
      enh(EN_TURRET, x, 12.5f, z);
    }
  }
  deck(15, 362, 27, 374, 9.4f, 1.5f, M_DECK); deck(-27, 346, -15, 358, 9.4f, 1.5f, M_DECK);
  cover(21, 368, 3, 1.4f, 1.3f, 9.4f); cover(-21, 352, 3, 1.4f, 1.3f, 9.4f);
  pk(PK_HEALTH, 21, 9.4f, 366); pk(PK_SNIPE_AMMO, -21, 9.4f, 350); pk(PK_SNIPE_AMMO, 0, 9.4f, 372);
  en(EN_GRUNT, 21, 370, 10); en(EN_GRUNT, -21, 354, 10); en(EN_GRUNT, 0, 380, 10); en(EN_GRUNT, 0, 394, 10);
  en(EN_FLYER, -10, 360, 9.4f); en(EN_FLYER, 10, 370, 9.4f); en(EN_FLYER, 0, 384, 9.4f); en(EN_FLYER, -12, 390, 9.4f);
  /* checkpoint deck */
  deck(-18, 402, 18, 428, 9.4f, 2.5f, M_DECK);
  cp(0, 9.4f, 410);
  putcol(MD_GATE, -12, 9.4f, 424, 0, 12, 0.9f); putcol(MD_GATE, 12, 9.4f, 424, 0, 12, 0.9f);
  cover(-9, 418, 4, 1.6f, 1.3f, 9.4f); cover(9, 418, 4, 1.6f, 1.3f, 9.4f);
  pk(PK_HEALTH, -14, 9.4f, 410); pk(PK_SHIELD, 14, 9.4f, 410);

  /* Z6 dome yard */
  deck(-36, 460, 36, 600, -4.6f, 3, M_DECK);
  float dz[][2] = {{-22, 480}, {22, 488}, {-21, 510}, {22, 518}};
  for (int i = 0; i < 4; i++) putfit(MD_DOME, dz[i][0], -4.6f, dz[i][1], i * 1.3f, 1.7f, 0.85f, 1);
  putcol(MD_HANGAR, -30, -4.6f, 530, 1.57f, 9, 0.9f);
  putcol(MD_STRUCT, 28, -4.6f, 470, 0, 7, 0.8f);
  cover(-8, 470, 4, 1.8f, 1.3f, -4.6f); cover(10, 502, 4, 1.8f, 1.3f, -4.6f); cover(-6, 522, 5, 1.8f, 1.3f, -4.6f);
  for (int i = 0; i < 7; i++) en(EN_GRUNT, rr(-24, 24), 468 + i * 6, -4.6f);
  en(EN_FLYER, -8, 480, -4.6f); en(EN_FLYER, 10, 496, -4.6f); en(EN_FLYER, 0, 498, -4.6f); en(EN_FLYER, 14, 470, -4.6f);
  enh(EN_TURRET, -33, -4.6f, 490); enh(EN_TURRET, 33, -4.6f, 505);
  pk(PK_HEALTH, -30, -4.6f, 500); pk(PK_SNIPE_AMMO, 30, -4.6f, 482); pk(PK_DECOY, 0, -4.6f, 520);
  cp(0, -4.6f, 466);

  /* Z7 brute arena */
  g_arena_lo = v3_(-24, -6, 532); g_arena_hi = v3_(24, 20, 590);
  wall(-25, 532, -23, 590, -4.6f, 6, M_HULL); wall(23, 532, 25, 590, -4.6f, 6, M_HULL);
  wall(-25, 590, -6, 592, -4.6f, 6, M_HULL); wall(6, 590, 25, 592, -4.6f, 6, M_HULL);
  wall(-25, 530, -7, 532, -4.6f, 6, M_HULL); wall(7, 530, 25, 532, -4.6f, 6, M_HULL);
  cover(-10, 545, 5, 5, 3.6f, -4.6f); cover(10, 545, 5, 5, 3.6f, -4.6f); cover(-11, 574, 5, 5, 3.6f, -4.6f); cover(11, 574, 5, 5, 3.6f, -4.6f);
  cover(0, 560, 3, 3, 1.3f, -4.6f);
  pk(PK_HEALTH, -20, -4.6f, 538); pk(PK_HEALTH, 20, -4.6f, 538); pk(PK_SNIPE_AMMO, 0, -4.6f, 536);
  en(EN_BRUTE, 0, 566, -4.6f); en(EN_GRUNT, -14, 570, -4.6f); en(EN_GRUNT, 14, 570, -4.6f);

  /* exit gate */
  deck(-9, 592, 9, 618, -4.6f, 2, M_DECK);
  solid(v3_(-9, -4.6f, 606), v3_(-6.5f, 9, 609), M_HULL, 6, true);
  solid(v3_(6.5f, -4.6f, 606), v3_(9, 9, 609), M_HULL, 6, true);
  ubox(v3_(-9.5f, 9, 605.5f), v3_(9.5f, 11, 609.5f), M_ORANGE, true);
  ubox(v3_(-6.5f, 8.3f, 606.4f), v3_(6.5f, 8.7f, 608.6f), M_TEALG, false);
  putfit(MD_ANT, -7.7f, 9, 607.5f, 0, 2, 0.5f, 1); putfit(MD_ANT, 7.7f, 9, 607.5f, 0, 2, 0.5f, 1);
  g_exit_lo = v3_(-7, -5.5f, 600); g_exit_hi = v3_(7, 8, 616);

  /* ── the Lessee: signage that escalates from courtesy to a work order ── */
  sign(0, 3.6f, -9, DS_PI, SG_WALK, 7);
  sign(-10.5f, 3, 70, 1.57f, SG_WALK2, 5);
  sign(0, 5.4f, 166, DS_PI, SG_GLIDE, 6);
  sign(-9, -2.1f, 196, DS_PI, SG_NONEST, 5);
  sign(-30, -1.7f, 464, DS_PI - 0.3f, SG_GLASS, 5);
  sign(7, -1, 300, -1.57f, SG_STAIR, 4);
  sign(0, 12.5f, 338, DS_PI, SG_QUIET, 6);
  sign(0, 12.5f, 427, DS_PI, SG_TRIPLICATE, 6);
  sign(-21, -2, 480, DS_PI, SG_DOCK, 4);
  sign(-11, -1.3f, 527.5f, DS_PI, SG_FINAL, 7);
  sign(0, 5, 604, DS_PI, SG_EXIT, 6);
  /* maintenance bay 7 in the arena's east wall: the Brute's kennel */
  ubox(v3_(22.2f, -4.6f, 555), v3_(22.9f, 2.4f, 573), M_HAZ, false);
  ubox(v3_(22.3f, -4.6f, 556.2f), v3_(23.1f, 1.8f, 571.8f), M_DARK, false);
  sign(21.6f, -1.5f, 564, -1.57f, SG_BAY, 6);
  /* the work order, nailed to the exit gate's left pillar, and its twin on the right */
  form(-7.75f, 4.2f, 605.9f, DS_PI, FM_ORDER, 2.3f, false);
  form(7.75f, 4.2f, 605.9f, DS_PI, FM_ORDER, 2.3f, false);
  /* complaint forms: one by the landing, a cluster at the first beat, then thinner */
  form(-6, 0, 22, 0.4f, FM_NOISE, 0.9f, true);
  form_post(-12.2f, 0, 72, 1.57f, FM_NOISE);
  form(-3.5f, 0, 74, 0.8f, FM_NOISE, 0.9f, true); form(2, 0, 70, -0.5f, FM_VIEW, 0.9f, true); form(6.5f, 0, 77, 2.2f, FM_NOISE, 0.9f, true);
  form(8, 0, 104, 0.3f, FM_WINDOW, 0.9f, true);
  form(-12, 3.2f, 150, 1, FM_NOISE, 0.9f, true);
  form(-12, -5, 206, 0.2f, FM_WINDOW, 0.9f, true); form(14, -5, 246, 1.4f, FM_VIEW, 0.9f, true);
  form(5, 9.4f, 332, 0.9f, FM_NOISE, 0.9f, true);
  form(-6, 9.4f, 414, 0.2f, FM_WINDOW, 0.9f, true);
  form(14, -4.6f, 494, 0.6f, FM_NOISE, 0.9f, true); form(-10, -4.6f, 526, 1.9f, FM_VIEW, 0.9f, true);
  form(3, -4.6f, 598, 0.2f, FM_NOISE, 0.9f, true); form(-4, -4.6f, 602, 2.6f, FM_WINDOW, 0.9f, true); form(2.5f, -4.6f, 585, 1.1f, FM_NOISE, 0.9f, true);
  /* the Tenants' nests, built of stacked colony panes, in the solar field */
  nest(-13, -5, 226, 0.5f, 1.3f);
  nest(14, -5, 262, -0.6f, 1.5f);
  nest(-14, -5, 282, 0.3f, 1.1f);
  pane_stack(13, -5, 206, -0.4f, 5); pane_stack(-12, -5, 250, 0.6f, 4); pane_stack(10, -5, 286, -0.7f, 6);
  /* sodium lamps along the deck edges */
  for (float z = 8; z < 600; z += 28) {
    float w = z < 64 ? 23.f : z < 120 ? 12.f : z > 460 && z < 530 ? 35.f : z > 190 && z < 292 ? 33.f : 0.f;
    if (w == 0.f) continue;
    float y = z < 120 ? 0.f : z < 160 ? 3.2f : z > 190 && z < 292 ? -5.f : z > 460 ? -4.6f : 0.f;
    for (int sd = -1; sd <= 1; sd += 2) { ucube(sd * w, y + 3.2f, z, 0.2f, 6.4f, 0.2f, M_DARK, false); ucube(sd * (w - 0.7f), y + 6.5f, z, 1.6f, 0.35f, 0.8f, M_ORANGEG, false); }
  }
  /* beats, anchors */
  zone(0, -24, -3, -14, 24, 12, 44);
  zone(1, -13.5f, -2, 64, 13.5f, 8, 84);
  zone(2, -34, -8, 192, 34, 10, 232);
  zone(3, -36, -8, 508, 36, 14, 531.5f);
  zone(4, -9, -6, 594, 9, 10, 605);
  anchor("landing", g_spawn, 0);
  anchor("brute_arena", v3_(0, -4.6f, 561), 0);
  anchor("exit", v3_(0, -4.6f, 604), 0);
  anchor("vista", v3_(0, 0, 240), 0);
  gate(-7, 7, 531, -4.6f, 6);
  gate(-6, 6, 591, -4.6f, 6);
  g_gate_probe = v3_(0, -4.6f, 515); g_gate_r = 30;
  rings(250, 14);
}

/* ── level 1: Hydroponic Spine ── */
static void garden(float x0, float z0, float x1, float z1, float y, float th) {
  col(v3_(x0, y - th, z0), v3_(x1, y, z1));
  tbox(v3_(x0, y - th, z0), v3_(x1, y - 0.3f, z1), M_MOSS, 4, false);
  tbox(v3_(x0 + 0.5f, y - 0.3f, z0 + 0.5f), v3_(x1 - 0.5f, y, z1 - 0.5f), M_SOIL, 6, false);
  ubox(v3_(x0, y - 0.35f, z0), v3_(x1, y + 0.1f, z0 + 0.5f), M_TEAL, false);
  ubox(v3_(x0, y - 0.35f, z1 - 0.5f), v3_(x1, y + 0.1f, z1), M_TEAL, false);
  ubox(v3_(x0, y - 0.35f, z0), v3_(x0 + 0.5f, y + 0.1f, z1), M_TEAL, false);
  ubox(v3_(x1 - 0.5f, y - 0.35f, z0), v3_(x1, y + 0.1f, z1), M_TEAL, false);
  float ins = fminf(x1 - x0, z1 - z0) * 0.14f, hd = 10;
  tbox(v3_(x0 + ins, y - th - hd, z0 + ins), v3_(x1 - ins, y - th, z1 - ins), M_HULL, 12, false);
  tbox(v3_(x0 + ins * 2.4f, y - th - hd * 2, z0 + ins * 2.4f), v3_(x1 - ins * 2.4f, y - th - hd, z1 - ins * 2.4f), M_DARK, 12, false);
  /* hanging roots and cables for the floating look */
  for (int i = 0; i < 4; i++) {
    float x = rr(x0 + ins, x1 - ins), z = rr(z0 + ins, z1 - ins);
    strut(v3_(x, y - th - hd * 1.9f, z), v3_(x + rr(-2, 2), y - th - hd * 1.9f - rr(10, 24), z + rr(-2, 2)), 0.25f, M_HEDGE);
  }
}
static void hedge(float x0, float z0, float x1, float z1, float y, float h) { solid(v3_(x0, y, z0), v3_(x1, y + h, z1), M_HEDGE, 3, true); }
static bool clear_spot(float x, float y, float z, float pad) {
  for (int i = 0; i < g_nb; i++) {
    const box_t *b = &g_box[i];
    if (b->hi.y > y + 0.3f && b->lo.y < y + 3 && x > b->lo.x - pad && x < b->hi.x + pad && z > b->lo.z - pad && z < b->hi.z + pad) return false;
  }
  return true;
}
/* kit trees: a trunk-sized collider; bushes and plants lower and wider */
static void trees_on(float x0, float z0, float x1, float z1, float y, int n) {
  static const struct { int md; float kx, kh; } tm[] = {{MD_TREEB, 0.13f, 1}, {MD_TREES, 0.13f, 1}, {MD_TREEW, 0.13f, 1}, {MD_TREEL, 0.13f, 1},
                                                      {MD_PLANT, 0.35f, 0.6f}, {MD_PLANT2, 0.35f, 0.6f}, {MD_BUSH, 0.5f, 0.45f}, {MD_TREEF, 0.13f, 1}};
  for (int i = 0; i < n; i++) {
    int k = (int)(rnd() * 8) & 7;
    float sc = rr(1.7f, 3.3f), x = 0, z = 0;
    int tries = 0;
    do { x = rr(x0, x1); z = rr(z0, z1); } while (!clear_spot(x, y, z, 1.6f) && ++tries < 8);
    if (tries >= 8) continue;
    putfit(tm[k].md, x, y, z, rr(0, 6.28f), sc, tm[k].kx, tm[k].kh);
  }
}
/* flower beds: a scatter of little bright boxes, cheap because they instance */
static void flowers(float x0, float z0, float x1, float z1, float y, int n) {
  static const int fm[3] = {M_PINK, M_TEALG, M_ORANGEG};
  for (int i = 0; i < n; i++) {
    float x = rr(x0, x1), z = rr(z0, z1);
    if (!clear_spot(x, y, z, 0.5f)) continue;
    ucube(x, y + 0.25f, z, 0.2f, 0.5f, 0.2f, fm[i % 3], false);
  }
}
/* a grow lamp: a pink panel on a hanging post */
static void growlamp(float x, float y, float z) {
  ucube(x, y + 3.5f, z, 0.2f, 7, 0.2f, M_DARK, false);
  ucube(x, y + 7.1f, z, 3.4f, 0.3f, 2.2f, M_PINK, false);
}

static void build_l1(void) {
  g_spawn = v3_(0, 0, 8);
  g_kill_y = -62;
  facade(-100, -80, -80, 660, -140, 160, M_WIN2, 14);
  facade(80, -80, 100, 660, -140, 160, M_WIN2, 14);
  planet(-200, -3300, 400, 2500);
  far_hull(520, 80, 500, 1300, 130, 0.0f);
  floaters(-320, 320, -20, 640, -150, -50, 16);
  colony(170, 300, 1250, 380, -0.55f, -0.2f);
  /* greenhouse ribs spanning the chasm high above */
  for (float z = 20; z < 650; z += 50) {
    strut(v3_(-90, 150, z), v3_(0, 190, z), 1.4f, M_DARK);
    strut(v3_(90, 150, z), v3_(0, 190, z), 1.4f, M_DARK);
  }

  /* A: landing garden */
  garden(-20, -14, 20, 54, 0, 2.5f);
  trees_on(-19, -12, -12, 50, 0, 7); trees_on(12, -12, 19, 50, 0, 7);
  hedge(-20, -14, 20, -12.5f, 0, 5);
  cover(-8, 30, 4, 2, 1.3f, 0); cover(8, 36, 4, 2, 1.3f, 0);
  putfit(MD_HONE, -12, 0, 18, 0.7f, 2.4f, 0.8f, 1);
  cp(0, 0, 8); g_cp[0].pos = g_spawn;
  pk(PK_HEALTH, 15, 0, 40); pk(PK_SNIPE_AMMO, -15, 0, 44);
  en(EN_GRUNT, -8, 42, 1); en(EN_GRUNT, 8, 48, 1);

  /* B: plank bridge with planters */
  garden(-3.5f, 54, 3.5f, 98, 0, 1.2f);
  for (float z = 60; z < 96; z += 12) { solid(v3_(-3.5f, 0, z), v3_(-1.6f, 1.2f, z + 3), M_HEDGE, 3, true); solid(v3_(1.6f, 0, z + 6), v3_(3.5f, 1.2f, z + 9), M_HEDGE, 3, true); }
  en(EN_GRUNT, 0, 76, 1); en(EN_GRUNT, 0, 90, 1);
  enh(EN_FLYER, 0, 7, 84);
  pk(PK_DECOY, 0, 0, 70);

  /* C: greenhouse island */
  garden(-26, 98, 26, 156, -2, 2.5f);
  putfit(MD_HLONG, -16, -2, 128, 1.57f, 2.0f, 0.85f, 1);
  putfit(MD_HCYL, 17, -2, 124, 0, 1.8f, 0.85f, 1);
  cover(0, 112, 5, 2, 1.3f, -2); cover(-4, 140, 5, 2, 1.3f, -2); cover(8, 146, 4, 2, 1.3f, -2);
  trees_on(-25, 100, 25, 154, -2, 14);
  hedge(-26, 98, -24.5f, 156, -2, 1.5f); hedge(24.5f, 98, 26, 156, -2, 1.5f);
  en(EN_GRUNT, -7, 110, -1); en(EN_GRUNT, 9, 118, -1); en(EN_GRUNT, -5, 132, -1); en(EN_GRUNT, 6, 140, -1); en(EN_GRUNT, 0, 150, -1);
  enh(EN_TURRET, -22, -2, 148); enh(EN_TURRET, 22, -2, 148);
  en(EN_FLYER, 0, 130, -2); en(EN_FLYER, -10, 146, -2);
  pk(PK_HEALTH, 22, -2, 108); pk(PK_SHIELD, -22, -2, 108);

  /* D: sniper alley: hedge canyon with perches on top */
  garden(-24, 180, 24, 270, -10, 3);
  cp(0, -10, 188);
  hedge(-12, 200, -9.5f, 258, -10, 7); hedge(9.5f, 200, 12, 258, -10, 7);
  cover(0, 214, 4, 1.6f, 1.3f, -10); cover(0, 236, 5, 1.6f, 1.3f, -10); cover(0, 250, 4, 1.6f, 1.3f, -10);
  trees_on(-23, 182, -14, 268, -10, 8); trees_on(14, 182, 23, 268, -10, 8);
  enh(EN_TURRET, -10.75f, -3, 204); enh(EN_TURRET, 10.75f, -3, 222); enh(EN_TURRET, -10.75f, -3, 244); enh(EN_TURRET, 10.75f, -3, 254);
  en(EN_GRUNT, -4, 206, -9); en(EN_GRUNT, 4, 216, -9); en(EN_GRUNT, -3, 226, -9); en(EN_GRUNT, 3, 240, -9); en(EN_GRUNT, -4, 252, -9);
  en(EN_GRUNT, -18, 230, -9); en(EN_GRUNT, 18, 214, -9);
  en(EN_FLYER, 0, 220, -10); en(EN_FLYER, 6, 240, -10); en(EN_FLYER, -6, 258, -10); en(EN_FLYER, 0, 196, -10);
  pk(PK_HEALTH, -20, -10, 192); pk(PK_SNIPE_AMMO, 20, -10, 192); pk(PK_SNIPE_AMMO, 0, -10, 262);
  /* glide gap 1: a stepping garden */
  garden(-4, 164, 4, 172, -6, 1.2f);
  pk(PK_DECOY, 0, -6, 168);

  /* E: three shuttle platforms over the chasm */
  plat(-4, 278, 4, 286, -11, 1.0f, v3_(10, 0, 0), 7.5f, 0, false);
  plat(-4, 296, 4, 304, -12, 1.0f, v3_(-10, 0, 0), 8.0f, 1.2f, false);
  plat(-4, 314, 4, 322, -13, 1.0f, v3_(10, 0, 0), 7.0f, 0.4f, false);
  plat(-4, 330, 4, 338, -14.5f, 1.0f, v3_(-10, 0, 0), 7.5f, 2.0f, false);
  en(EN_FLYER, -10, 292, -12); en(EN_FLYER, 10, 312, -14); en(EN_FLYER, 0, 330, -14);
  pk(PK_SHIELD, 0, -10, 266);
  /* F: the hub */
  garden(-34, 340, 34, 422, -16, 3);
  cp(0, -16, 352);
  putfit(MD_DOME, 0, -16, 384, 0, 2.5f, 0.82f, 1);
  putfit(MD_HLONG, -22, -16, 362, 1.57f, 2.0f, 0.85f, 1); putfit(MD_HCYL, 22, -16, 372, 0, 1.8f, 0.85f, 1);
  cover(-12, 360, 4, 2, 1.3f, -16); cover(12, 360, 4, 2, 1.3f, -16); cover(-10, 402, 5, 2, 1.3f, -16); cover(10, 404, 5, 2, 1.3f, -16);
  trees_on(-33, 342, 33, 420, -16, 14);
  for (int i = 0; i < 6; i++) en(EN_GRUNT, rr(-26, 26), 356 + i * 9, -15);
  for (int i = 0; i < 5; i++) en(EN_FLYER, rr(-24, 24), 360 + i * 12, -16);
  enh(EN_TURRET, -31, -16, 410); enh(EN_TURRET, 31, -16, 410);
  pk(PK_HEALTH, -30, -16, 348); pk(PK_HEALTH, 30, -16, 348); pk(PK_SNIPE_AMMO, 0, -16, 414);
  /* the lift up */
  plat(-5, 422, 5, 434, -9.5f, 1.2f, v3_(0, 6.5f, 0), 10.f, 0, true);
  g_plat[g_np - 1].phase = -DS_PI / 2;
  /* G: the orchard, gone up to y -3 */
  garden(-32, 434, 32, 526, -3, 3);
  cp(0, -3, 446);
  for (int s = -1; s <= 1; s += 2)
    for (int i = 0; i < 5; i++) {
      float z = 454 + i * 14;
      solid(v3_(s * 17 - 1.5f, -3, z - 1.5f), v3_(s * 17 + 1.5f, 9, z + 1.5f), M_HEDGE, 3, true);
      put(MD_TREEB, s * 17, 9, z, 0, 3.2f);
    }
  cover(0, 462, 5, 2, 1.3f, -3); cover(-6, 484, 5, 2, 1.3f, -3); cover(6, 500, 5, 2, 1.3f, -3);
  trees_on(-31, 436, 31, 520, -3, 12);
  enh(EN_TURRET, -17, 9.4f, 468); enh(EN_TURRET, 17, 9.4f, 482); enh(EN_TURRET, -17, 9.4f, 496); enh(EN_TURRET, 17, 9.4f, 510);
  for (int i = 0; i < 7; i++) en(EN_GRUNT, rr(-24, 24), 456 + i * 7, -2);
  for (int i = 0; i < 6; i++) en(EN_FLYER, rr(-22, 22), 460 + i * 10, -3);
  pk(PK_HEALTH, 28, -3, 450); pk(PK_SNIPE_AMMO, -28, -3, 450); pk(PK_DECOY, 0, -3, 514); pk(PK_SHIELD, 28, -3, 510);

  /* H: the arena below, and the exit */
  garden(-28, 548, 28, 618, -11, 3);
  g_arena_lo = v3_(-28, -14, 552); g_arena_hi = v3_(28, 20, 616);
  hedge(-28, 552, -26.5f, 616, -11, 5); hedge(26.5f, 552, 28, 616, -11, 5);
  hedge(-28, 616, -7, 618, -11, 5); hedge(7, 616, 28, 618, -11, 5);
  hedge(-28, 550, -8, 552, -11, 5); hedge(8, 550, 28, 552, -11, 5);
  cover(-12, 572, 5, 5, 3.4f, -11); cover(12, 572, 5, 5, 3.4f, -11); cover(-12, 598, 5, 5, 3.4f, -11); cover(12, 598, 5, 5, 3.4f, -11);
  cover(0, 585, 4, 3, 1.3f, -11);
  en(EN_BRUTE, -10, 590, -11); en(EN_BRUTE, 10, 590, -11); en(EN_GRUNT, 0, 600, -11); en(EN_GRUNT, -18, 584, -11);
  pk(PK_HEALTH, -22, -11, 558); pk(PK_HEALTH, 22, -11, 558); pk(PK_SNIPE_AMMO, 0, -11, 556); pk(PK_SHIELD, 0, -11, 610);
  cp(0, -11, 556);
  deck(-9, 618, 9, 642, -11, 2, M_DECK);
  solid(v3_(-9, -11, 632), v3_(-6.5f, 3, 635), M_HULL, 6, true);
  solid(v3_(6.5f, -11, 632), v3_(9, 3, 635), M_HULL, 6, true);
  ubox(v3_(-9.5f, 3, 631.5f), v3_(9.5f, 5, 635.5f), M_ORANGE, true);
  ubox(v3_(-6.5f, 2.3f, 632.4f), v3_(6.5f, 2.7f, 634.6f), M_TEALG, false);
  g_exit_lo = v3_(-7, -12, 626); g_exit_hi = v3_(7, 2, 640);
  /* glide gap: orchard to arena is 26 m */

  /* ── the Sublet: gardens, grow lamps, nests of colony glass ── */
  sign(0, 3.4f, -10, DS_PI, SG_PLANTS, 7);
  sign(-9, 2.9f, 24, DS_PI, SG_PLANTS2, 5);
  sign(0, 1.2f, 102, DS_PI, SG_HOME, 6);
  sign(-14, -7.1f, 184, DS_PI, SG_RENT, 5);
  sign(-28, -13.1f, 345, DS_PI, SG_KNOCK, 5);
  sign(8, -13.4f, 414, DS_PI, SG_LIFT, 4);
  sign(0, 0.1f, 438, DS_PI, SG_PLANTS, 6);
  sign(-13, -7.9f, 549.2f, DS_PI, SG_QUIET2, 6);
  sign(0, -6, 630, DS_PI, SG_EXIT, 6);
  for (float z = 6; z < 54; z += 24) { growlamp(-17, 0, z); growlamp(17, 0, z); }
  for (float z = 106; z < 156; z += 24) { growlamp(-24, -2, z); growlamp(24, -2, z); }
  for (float z = 190; z < 270; z += 26) { growlamp(-22, -10, z); growlamp(22, -10, z); }
  for (float z = 346; z < 420; z += 24) { growlamp(-32, -16, z); growlamp(32, -16, z); }
  for (float z = 440; z < 526; z += 28) { growlamp(-30, -3, z); growlamp(30, -3, z); }
  for (float z = 556; z < 616; z += 30) { growlamp(-26, -11, z); growlamp(26, -11, z); }
  flowers(-19, -12, 19, 52, 0, 28); flowers(-25, 100, 25, 154, -2, 26); flowers(-23, 182, 23, 268, -10, 20);
  flowers(-33, 342, 33, 420, -16, 28); flowers(-31, 436, 31, 524, -3, 28); flowers(-27, 554, 27, 616, -11, 20);
  /* Tenant nests, windows from tower 4 and its neighbours */
  nest(-17, 0, 6, 0.9f, 0.9f);
  nest(-23, -2, 118, 1.2f, 1.2f); pane_stack(23, -2, 106, -1.57f, 5);
  nest(-18, -10, 232, 1.4f, 1.1f); nest(18, -10, 244, -1.4f, 1.1f);
  nest(26, -16, 410, -1.2f, 1.2f);
  nest(-22, -16, 392, 1.57f, 1.7f);                                    /* the family nest */
  form(-22 + 1.74f * 1.7f + 0.1f, -16 + 2.4f, 392 + 1.3f * 1.7f, 1.57f, FM_PLANET, 1.5f, false);
  form(-14.5f, -16, 388, 0.3f, FM_FAMILY, 0.9f, true); form(-15, -16, 396, 2.0f, FM_FAMILY, 0.9f, true);
  nest(-27, -3, 478, 1.3f, 1.3f); nest(27, -3, 498, -1.4f, 1.0f); pane_stack(27, -3, 458, -1.57f, 5);
  /* forms: some by the walkway posts, family sized ones near the nests */
  form_post(-5.5f, 0, 28, DS_PI + 0.3f, FM_NOISE); form(3, 0, 44, 0.5f, FM_VIEW, 0.9f, true);
  form(-1, 0, 84, 0.4f, FM_WINDOW, 0.9f, true); form(6, -2, 112, 1.1f, FM_VIEW, 0.9f, true);
  form_post(9, -2, 104, DS_PI - 0.3f, FM_WINDOW);
  form(5, -10, 200, 0.7f, FM_NOISE, 0.9f, true); form(-4, -10, 260, 2.4f, FM_VIEW, 0.9f, true);
  form(8, -16, 366, 0.4f, FM_FAMILY, 0.9f, true); form_post(-8, -16, 346, DS_PI + 0.2f, FM_FAMILY);
  form(-4, -3, 470, 1.7f, FM_NOISE, 0.9f, true); form(4, -3, 520, 0.6f, FM_WINDOW, 0.9f, true);
  form(2, -11, 600, 0.2f, FM_NOISE, 0.9f, true);
  /* the lift: guide rails and a beacon */
  for (int sd = -1; sd <= 1; sd += 2) { ucube(sd * 5.4f, -8, 422.3f, 0.35f, 16, 0.35f, M_DARK, false); ucube(sd * 5.4f, -8, 433.7f, 0.35f, 16, 0.35f, M_DARK, false); }
  ucube(0, 0.5f, 422.3f, 11.4f, 0.4f, 0.4f, M_TEALG, false); ucube(0, 0.5f, 433.7f, 11.4f, 0.4f, 0.4f, M_TEALG, false);
  ucube(0, 1.4f, 428, 0.8f, 0.8f, 0.8f, M_PINK, false);
  zone(0, -20, -3, -14, 20, 10, 40);
  zone(1, -26, -6, 98, 26, 8, 114);
  zone(2, -34, -20, 368, -12, -6, 410);
  zone(3, -8, -20, 408, 8, 0, 436);
  zone(4, -30, -20, 530, 30, 6, 552);
  zone(5, -9, -14, 618, 9, 4, 634);
  anchor("landing", g_spawn, 0);
  anchor("brute_arena", v3_(0, -11, 584), 0);
  anchor("exit", v3_(0, -11, 632), 0);
  anchor("vista", v3_(0, 0, 128), 0);
  gate(-8, 8, 551, -11, 5.2f);
  gate(-7, 7, 617, -11, 5.2f);
  g_gate_probe = v3_(0, -11, 525); g_gate_r = 28;
  rings(250, 12);
}

/* ── level 5: The Freehold (the Superintendent) ──
 * The crown is a climb: landing pad, crown walk, then switchback decks and stairs up
 * between cranes, a shuttle platform and a glide gap, to the ring arena at y = ARY. */
#define ARY 36.f
static void pylon(float x, float z, float ytop) { solid(v3_(x - 1.5f, -80, z - 1.5f), v3_(x + 1.5f, ytop, z + 1.5f), M_HULL, 10, true); }
static void build_l5(void) {
  g_spawn = v3_(0, 0, 8);
  g_kill_y = -70;
  planet(-150, -3000, 600, 2500);
  far_hull(-380, 60, 300, 900, 120, 0.15f);
  far_hull(520, 20, 520, 1200, 150, -0.1f);
  far_hull(40, 220, 1100, 1000, 170, 1.4f);
  stars(400);
  colony(-60, 150, 960, 380, -0.42f, 0.18f);
  for (int i = 0; i < 14; i++) {
    float a = i * 0.9f, rr0 = rr(150, 260);
    put(MD_BLDG, cosf(a) * rr0, rr(-90, -30), 150 + sinf(a) * rr0, rr(0, 6.28f), rr(5, 9));
  }
  /* the Freehold's other cranes, standing out of the dark below */
  crane(-130, -70, 160, 150, 60, 1); crane(150, -70, 380, 150, 70, -1); crane(-150, -60, 420, 130, 60, 1); crane(-40, -80, 600, 170, 80, -1);

  /* landing pad and crown walk */
  deck(-24, -14, 24, 54, 0, 2.5f, M_DECK);
  cp(0, 0, 8); g_cp[0].pos = g_spawn;
  putfit(MD_CARGO, 16, 0, 0, 2.0f, 1.2f, 0.85f, 1);
  putfit(MD_BARRELS, -15, 0, 20, 0.4f, 5, 0.9f, 1);
  wall(-24, -14, 24, -12.5f, 0, 5, M_HULL);
  cover(-9, 34, 4, 2, 1.3f, 0); cover(9, 40, 4, 2, 1.3f, 0);
  pk(PK_HEALTH, -16, 0, 46); pk(PK_SNIPE_AMMO, 16, 0, 46);
  en(EN_GRUNT, -8, 46, 1); en(EN_GRUNT, 8, 50, 1);
  deck(-10, 54, 10, 118, 0, 2.2f, M_DECK);
  wall(-10.4f, 56, -9.6f, 90, 0, 1.2f, M_PLATE); wall(9.6f, 56, 10.4f, 90, 0, 1.2f, M_PLATE);
  cover(-3, 70, 4, 1.6f, 1.3f, 0); cover(4, 84, 4, 1.6f, 1.3f, 0); cover(-3, 100, 4, 1.6f, 1.3f, 0);
  en(EN_GRUNT, -5, 78, 1); en(EN_GRUNT, 5, 90, 1); en(EN_GRUNT, 0, 106, 1);
  enh(EN_TURRET, -9, 0, 112);
  pk(PK_DECOY, 7, 0, 62);
  /* glide gap */
  deck(-4, 130, 4, 138, -2.5f, 1.2f, M_DECK);
  pk(PK_SHIELD, 0, -2.5f, 134);
  /* terrace A (y -6): the first fight */
  deck(-32, 148, 32, 214, -6, 3, M_DECK);
  cp(0, -6, 160);
  putfit(MD_DOME, -20, -6, 180, 0, 1.6f, 0.85f, 1); putfit(MD_DOME, 20, -6, 190, 1, 1.6f, 0.85f, 1);
  putfit(MD_HANGAR, -22, -6, 204, 0, 9, 0.9f, 1);
  cover(-8, 168, 4, 1.8f, 1.3f, -6); cover(8, 176, 4, 1.8f, 1.3f, -6); cover(5, 188, 5, 1.8f, 1.3f, -6);
  for (int i = 0; i < 5; i++) en(EN_GRUNT, rr(-24, 24), 164 + i * 8, -5);
  en(EN_FLYER, -10, 170, -6); en(EN_FLYER, 12, 184, -6); en(EN_FLYER, 0, 196, -6);
  enh(EN_TURRET, -30, -6, 190); enh(EN_TURRET, 30, -6, 172);
  pk(PK_HEALTH, -28, -6, 156); pk(PK_SNIPE_AMMO, 28, -6, 156); pk(PK_HEALTH, 0, -6, 210);
  /* up: stairs to T1 (y 2), a shorter beat between fights */
  stairs(0, -6, 214, 0, 16, 20, 0.4f, 0.9f);
  deck(-24, 232, 24, 266, 2, 2.5f, M_DECK);
  cover(-12, 246, 4, 1.8f, 1.3f, 2); cover(0, 256, 5, 1.8f, 1.3f, 2);
  en(EN_GRUNT, -10, 252, 3); en(EN_GRUNT, 10, 244, 3); en(EN_FLYER, 0, 250, 2);
  pk(PK_HEALTH, 18, 2, 238); pk(PK_SNIPE_AMMO, -18, 2, 238);
  /* east: stairs to the first crane base (T2, y 12): the second fight */
  stairs(24, 2, 250, 2, 12, 25, 0.4f, 0.9f);
  deck(46.5f, 226, 94, 282, 12, 2.5f, M_DECK);
  cp(62, 12, 254);
  crane(86, 12, 234, 62, 40, -1);
  cover(60, 240, 4, 1.8f, 1.3f, 12); cover(70, 266, 5, 1.8f, 1.3f, 12); cover(54, 270, 3, 3, 1.3f, 12);
  for (int i = 0; i < 5; i++) en(EN_GRUNT, rr(52, 90), 238 + i * 8, 13);
  en(EN_FLYER, 60, 250, 12); en(EN_FLYER, 78, 270, 12);
  enh(EN_TURRET, 91, 12, 268); enh(EN_TURRET, 50, 12, 232);
  pk(PK_HEALTH, 90, 12, 274); pk(PK_SNIPE_AMMO, 52, 12, 230); pk(PK_DECOY, 70, 12, 252);
  pylon(50, 230, 9.5f); pylon(90, 278, 9.5f);
  /* north to T3 (y 20): the halfway point of the crown */
  stairs(70, 12, 282, 0, 12, 20, 0.4f, 0.9f);
  deck(46, 300, 94, 346, 20, 2.5f, M_DECK);
  cover(56, 312, 4, 1.8f, 1.3f, 20); cover(84, 330, 4, 1.8f, 1.3f, 20);
  en(EN_GRUNT, 60, 330, 21); en(EN_GRUNT, 80, 318, 21); en(EN_GRUNT, 70, 338, 21);
  pk(PK_HEALTH, 88, 20, 306); pk(PK_SHIELD, 52, 20, 340);
  pylon(50, 342, 17.5f); pylon(90, 304, 17.5f);
  /* the gantry west: a shuttle platform bridges the gap, turrets watch from pylons */
  deck(14, 320, 46, 326, 20, 1.2f, M_PLATE);
  deck(-34, 320, -2, 326, 20, 1.2f, M_PLATE);
  plat(3, 320, 9, 326, 20, 1.0f, v3_(5, 0, 0), 8.f, 0, false);
  for (int sd = -1; sd <= 1; sd += 2) {
    float zz = 323 + sd * 3.3f;
    strut(v3_(46, 21.1f, zz), v3_(14, 21.1f, zz), 0.12f, M_REDG);
    strut(v3_(-2, 21.1f, zz), v3_(-34, 21.1f, zz), 0.12f, M_REDG);
  }
  solid(v3_(24, -80, 328), v3_(27, 23, 331), M_HULL, 10, true); enh(EN_TURRET, 25.5f, 23, 329.5f);
  solid(v3_(-22, -80, 315), v3_(-19, 23, 318), M_HULL, 10, true); enh(EN_TURRET, -20.5f, 23, 316.5f);
  en(EN_FLYER, 6, 323, 22); en(EN_FLYER, -16, 323, 22);
  /* T4 (y 20): the third fight, under a second crane */
  deck(-76, 298, -34, 348, 20, 2.5f, M_DECK);
  cp(-55, 20, 322);
  crane(-72, 20, 304, 52, 44, 1);
  cover(-50, 310, 4, 1.8f, 1.3f, 20); cover(-62, 330, 5, 1.8f, 1.3f, 20); cover(-44, 338, 3, 3, 1.3f, 20);
  for (int i = 0; i < 6; i++) en(EN_GRUNT, rr(-72, -38), 312 + i * 6, 21);
  en(EN_FLYER, -50, 320, 20); en(EN_FLYER, -62, 336, 20); en(EN_FLYER, -44, 345, 20);
  enh(EN_TURRET, -74, 20, 344); enh(EN_TURRET, -36, 20, 340);
  pk(PK_HEALTH, -72, 20, 340); pk(PK_SNIPE_AMMO, -38, 20, 304); pk(PK_DECOY, -56, 20, 345);
  pylon(-74, 346, 17.5f);
  /* north to T5 (y 30) */
  stairs(-55, 20, 348, 0, 12, 25, 0.4f, 0.9f);
  deck(-76, 370.5f, -34, 414, 30, 2.5f, M_DECK);
  cover(-60, 384, 4, 1.8f, 1.3f, 30); cover(-46, 400, 4, 1.8f, 1.3f, 30);
  en(EN_GRUNT, -64, 396, 31); en(EN_GRUNT, -42, 380, 31); en(EN_GRUNT, -56, 408, 31); en(EN_FLYER, -50, 392, 30);
  pk(PK_HEALTH, -72, 30, 376); pk(PK_SNIPE_AMMO, -38, 30, 410);
  /* a glide gap with a stepping pad, then T6 */
  deck(-34, 388, -20, 396, 30, 1.2f, M_PLATE);
  deck(-12, 388, -4, 396, 29, 1.0f, M_PLATE);
  pk(PK_SHIELD, -8, 29, 392);
  deck(4, 370, 52, 420, 30, 2.5f, M_DECK);
  cp(28, 30, 395);
  crane(44, 30, 376, 48, 34, -1);
  cover(14, 384, 4, 1.8f, 1.3f, 30); cover(22, 408, 5, 1.8f, 1.3f, 30); cover(40, 396, 3, 3, 1.3f, 30);
  for (int i = 0; i < 4; i++) en(EN_GRUNT, rr(10, 48), 380 + i * 9, 31);
  en(EN_FLYER, 20, 390, 30); en(EN_FLYER, 36, 404, 30);
  enh(EN_TURRET, 8, 30, 374); enh(EN_TURRET, 50, 30, 416);
  pk(PK_HEALTH, 10, 30, 414); pk(PK_SNIPE_AMMO, 50, 30, 380);
  pylon(8, 374, 27.5f); pylon(50, 416, 27.5f);
  /* the last flight and the gate pad (y 36) */
  stairs(28, 30, 420, 0, 14, 15, 0.4f, 0.9f);
  deck(-24, 433.5f, 24, 462, ARY, 2.5f, M_DECK);
  cp(0, ARY, 442);
  pk(PK_HEALTH, -18, ARY, 440); pk(PK_HEALTH, 18, ARY, 440); pk(PK_SNIPE_AMMO, -16, ARY, 452); pk(PK_SNIPE_AMMO, 16, ARY, 452); pk(PK_SHIELD, 0, ARY, 456);
  cover(-14, 448, 3, 3, 1.3f, ARY); cover(14, 448, 3, 3, 1.3f, ARY);
  pylon(-22, 436, ARY - 2.5f); pylon(22, 460, ARY - 2.5f);
  /* the gate: posts, a lintel, a door that closes behind her */
  solid(v3_(-12, ARY, 462), v3_(-8, ARY + 14, 466), M_HULL, 6, true);
  solid(v3_(8, ARY, 462), v3_(12, ARY + 14, 466), M_HULL, 6, true);
  ubox(v3_(-12.5f, ARY + 14, 461.5f), v3_(12.5f, ARY + 16.5f, 466.5f), M_ORANGE, true);
  ubox(v3_(-8, ARY + 13, 462.6f), v3_(8, ARY + 13.5f, 465.4f), M_TEALG, false);
  gate(-8, 8, 464, ARY, 8);
  /* the ring arena: R 40 at (0, ARY, 506) */
  const float cx = 0, cz = 506, R = 40, Y0 = ARY;
  t3_geometry *cg = t3_cylinder_geometry_new(R, R, 4, 56, 1, false);
  t3_material *ms[3] = {g_mat[M_ARENASIDE], g_mat[M_ARENA], g_mat[M_ARENASIDE]};
  t3_mesh *cm = t3_mesh_new_multi(cg, ms, 3);
  t3_release(cg);
  t3_object_set_position(cm, cx, Y0 - 2, cz);
  T3_OBJ(cm)->receive_shadow = true;
  attach(cm);
  t3_geometry *kg = t3_cylinder_geometry_new(R * 0.82f, R * 0.35f, 28, 24, 1, false);
  t3_mesh *km = t3_mesh_new(kg, g_mat[M_DARK]);
  t3_release(kg);
  t3_object_set_position(km, cx, Y0 - 18, cz);
  attach(km);
  for (int i = 0; i < 40; i++) {
    float z0 = -R + i * 2.f, zc = fabsf(z0 + 1.f) + 1.f;
    float hw = zc >= R ? 1.f : sqrtf(R * R - zc * zc);
    col(v3_(cx - hw, Y0 - 4, cz + z0), v3_(cx + hw, Y0, cz + z0 + 2));
  }
  col(v3_(-8, Y0 - 4, 462), v3_(8, Y0, 467));
  ubox(v3_(-8, Y0 - 0.05f, 466.5f), v3_(8, Y0 + 0.02f, 468), M_TEALG, false);
  for (int i = 0; i < 12; i++) {
    float a = i * DS_PI / 6 + DS_PI / 12;
    float px = cx + cosf(a) * (R + 3), pz = cz + sinf(a) * (R + 3);
    if (fabsf(px - cx) < 10 && pz < cz) continue;
    solid(v3_(px - 1.6f, Y0 - 4, pz - 1.6f), v3_(px + 1.6f, Y0 + 30, pz + 1.6f), M_HULL, 8, true);
    ucube(px, Y0 + 31.5f, pz, 4, 3, 4, M_ORANGE, false);
    ucube(px - cosf(a) * 1.7f, Y0 + 12, pz - sinf(a) * 1.7f, 0.5f, 18, 0.5f, M_TEALG, false);
    ucube(px, Y0 + 33.4f, pz, 0.8f, 0.8f, 0.8f, M_REDG, false);
  }
  t3_geometry *tg = t3_torus_geometry_new(R + 3, 1.4f, 8, 72, DS_PI * 2);
  t3_mesh *tm = t3_mesh_new(tg, g_mat[M_DARK]);
  t3_release(tg);
  t3_object_set_position(tm, cx, Y0 + 31, cz);
  t3_object_set_rotation(tm, DS_PI / 2, 0, 0);
  attach(tm);
  tg = t3_torus_geometry_new(R + 3, 0.5f, 6, 72, DS_PI * 2);
  tm = t3_mesh_new(tg, g_mat[M_REDG]);
  t3_release(tg);
  t3_object_set_position(tm, cx, Y0 + 29.4f, cz);
  t3_object_set_rotation(tm, DS_PI / 2, 0, 0);
  attach(tm);
  for (int i = 0; i < 6; i++) {
    float a = i * DS_PI / 3 + 0.5f, px = cx + cosf(a) * 24, pz = cz + sinf(a) * 24;
    cover(px, pz, 4.5f, 4.5f, 4.2f, Y0);
  }
  for (int i = 0; i < 4; i++) {
    float a = i * DS_PI / 2 + 0.78f, px = cx + cosf(a) * 12.5f, pz = cz + sinf(a) * 12.5f;
    cover(px, pz, 5.5f, 1.6f, 1.3f, Y0);
  }
  for (int i = 0; i < 4; i++) {
    float a = i * DS_PI / 2 + 0.3f;
    pk(i & 1 ? PK_SNIPE_AMMO : PK_HEALTH, cx + cosf(a) * 33, Y0, cz + sinf(a) * 33);
  }
  pk(PK_SHIELD, cx - 33, Y0, cz + 2); pk(PK_DECOY, cx + 33, Y0, cz - 2);
  g_arena_lo = v3_(-42, Y0 - 6, 467); g_arena_hi = v3_(42, Y0 + 60, 552);
  g_exit_lo = v3_(-9, Y0 - 1, 460); g_exit_hi = v3_(9, Y0 + 12, 468);
  en(EN_BOSS, cx, cz + 8, Y0);
  g_sp[g_nsp - 1].yaw = DS_PI;
  /* the Superintendent's office, behind the arena: a lit window wall and a red PA board */
  solid(v3_(-34, Y0 - 4, 556), v3_(34, Y0 + 26, 584), M_HULL, 8, true);
  tbox(v3_(-30, Y0 + 6, 555.7f), v3_(30, Y0 + 20, 556.1f), M_WIN, 5, false);
  ubox(v3_(-31, Y0 + 5.2f, 555.4f), v3_(31, Y0 + 6, 556.1f), M_REDG, false);
  ubox(v3_(-31, Y0 + 20, 555.4f), v3_(31, Y0 + 20.8f, 556.1f), M_REDG, false);
  sign(0, Y0 + 4.1f, 555, DS_PI, SG_SUPER, 10);
  putfit(MD_ANT, -50, Y0 - 4, 540, 0, 8, 0.4f, 1); putfit(MD_ANT, 50, Y0 - 4, 540, 0, 8, 0.4f, 1);
  spinner(put(MD_RADAR, 0, Y0 + 26, 570, 0, 9), 0, 1, 0, 0.6f);

  /* PA notices in red, the Superintendent's voice on the walls */
  ucube(-10.8f, 4, 74, 0.4f, 8, 0.4f, M_DARK, false); ucube(10.8f, 4, 74, 0.4f, 8, 0.4f, M_DARK, false); ucube(0, 8.2f, 74, 22, 0.4f, 0.4f, M_DARK, false);
  sign(0, 6.6f, 74, DS_PI, SG_BREACH, 9);
  ucube(0, 8.55f, 73.8f, 9, 0.15f, 0.15f, M_REDG, false);
  sign(0, 5.2f, 122, DS_PI, SG_EVICT, 6);
  sign(-14, -2.4f, 210, DS_PI, SG_FIXTURE, 5);
  sign(60, 15.1f, 228.5f, DS_PI, SG_LEASE, 6);
  sign(-56, 23.1f, 340, DS_PI, SG_RELOC, 6);
  sign(20, 33.1f, 418, DS_PI, SG_CALM, 6);
  sign(0, ARY + 20.6f, 461.2f, DS_PI, SG_BREACH, 10);
  form(2, 0, 28, 0.3f, FM_NOISE, 0.9f, true); form(-4, 0, 66, 1.2f, FM_NOISE, 0.9f, true); form(6, 0, 96, 2.0f, FM_WINDOW, 0.9f, true);
  form(-14, -6, 156, 0.6f, FM_VIEW, 0.9f, true); form(14, -6, 204, 1.9f, FM_NOISE, 0.9f, true);
  form(20, 2, 240, 0.8f, FM_NOISE, 0.9f, true); form(80, 12, 262, 2.4f, FM_WINDOW, 0.9f, true);
  form(66, 20, 310, 0.5f, FM_NOISE, 0.9f, true); form(-60, 20, 340, 1.4f, FM_VIEW, 0.9f, true);
  form(-56, 30, 380, 2.1f, FM_NOISE, 0.9f, true); form(26, 30, 410, 0.9f, FM_WINDOW, 0.9f, true);
  form(-6, ARY, 446, 0.3f, FM_NOISE, 0.9f, true); form(8, ARY, 452, 1.7f, FM_FAMILY, 0.9f, true);
  /* stolen windows even here: a small nest by the first crane base */
  nest(88, 12, 270, -1.4f, 0.8f);
  zone(0, -24, -3, -14, 24, 10, 40);
  zone(1, -11, -3, 60, 11, 8, 86);
  zone(2, 40, 14, 296, 96, 32, 350);
  zone(3, -24, 34, 436, 24, 46, 462);
  anchor("landing", g_spawn, 0);
  anchor("boss_arena", v3_(cx, Y0, cz), 0);
  anchor("boss_spawn", v3_(cx, Y0, cz + 8), DS_PI);
  anchor("exit", v3_(cx, Y0, cz), 0);
  anchor("vista", v3_(60, 20, 300), 0);
  g_gate_probe = v3_(0, ARY, 440); g_gate_r = 30;
  rings(250, 12);
}


/* ── shared dressing for the three middle levels ── */
static int g_level;
/* a floor slab: collision, textured top and edge strips, no hull below (the new levels sit on a floor or a tower) */
static void flat(float x0, float z0, float x1, float z1, float y, float th, int mat) {
  col(v3_(x0, y - th, z0), v3_(x1, y, z1));
  deck_vis(x0, z0, x1, z1, y, th, mat, false);
}
/* shrink-wrapped cargo: a cover-sized crate with a frost cap */
static void wrapbox(float cx, float cz, float w, float d, float h, float y) {
  col(v3_(cx - w / 2, y, cz - d / 2), v3_(cx + w / 2, y + h, cz + d / 2));
  ucube(cx, y + h / 2, cz, w, h, d, M_WRAP, true);
  ucube(cx, y + h + 0.05f, cz, w + 0.1f, 0.12f, d + 0.1f, M_EYE, false);
}
/* a kit piece held in strapping and frost; the straps follow its yaw */
static t3_object *wrapped(int md, float x, float y, float z, float yaw, float sc) {
  model_t *m = &g_md[md];
  float w = (m->hi.x - m->lo.x) * sc, d = (m->hi.z - m->lo.z) * sc, h = (m->hi.y - m->lo.y) * sc, by = y + m->lo.y * sc;
  t3_object *o = put(md, x, y, z, yaw, sc);
  for (int i = 0; i < 2; i++) {
    t3_mesh *s = ucube(x, by + h * (i ? 0.72f : 0.28f), z, w * 1.07f, 0.16f, d * 1.07f, M_WRAP, false);
    t3_object_set_rotation(s, 0, yaw, 0);
  }
  t3_mesh *cap = ucube(x, by + h + 0.03f, z, w * 0.9f, 0.1f, d * 0.9f, M_EYE, false);
  t3_object_set_rotation(cap, 0, yaw, 0);
  return o;
}
static const int g_furn[7] = {MD_SOFA, MD_LAMP, MD_CHAIR, MD_BED, MD_SHELF, MD_FRIDGE, MD_DESK};
static const float g_furn_sc[7] = {2.6f, 2.3f, 2.4f, 2.0f, 1.9f, 1.7f, 2.0f};
/* a rack block: one collision box, planks and cream-wrapped goods on both long faces, kit furniture here and there */
static void rack(float x0, float z0, float x1, float z1, float y, float h, bool furn) {
  col(v3_(x0, y, z0), v3_(x1, y + h, z1));
  bool az = (z1 - z0) >= (x1 - x0);
  float cx = (x0 + x1) / 2, cz = (z0 + z1) / 2, len = az ? z1 - z0 : x1 - x0, wd = az ? x1 - x0 : z1 - z0;
  ubox(v3_(x0 + 0.2f, y, z0 + 0.2f), v3_(x1 - 0.2f, y + h, z1 - 0.2f), M_RACK, false);
  int shelves = (int)(h / 4.2f);
  for (int side = -1; side <= 1; side += 2) {
    if (az && fabsf(cx) > 3 && (cx < 0) != (side > 0)) continue;   /* the face turned away from the aisle is never seen */
    float face = (az ? cx : cz) + side * wd / 2;
    for (int k = 0; k < shelves; k++) {
      float ys = y + 0.15f + k * 4.2f;
      float pc = face + side * 0.2f;
      ucube(az ? pc : cx, ys, az ? cz : pc, az ? 0.9f : len, 0.2f, az ? len : 0.9f, M_PLATE, false);
      for (float t = -len / 2 + 0.8f; t < len / 2 - 3; ) {
        float bw = rr(3.0f, 5.2f), bh = rr(2.4f, 3.5f), cn = face - side * 0.3f, tc = t + bw / 2;
        float r = rnd();
        if (r < 0.05f) { t += bw + 0.3f; continue; }
        if (furn && r > 0.72f) {
          int f = (int)(rnd() * 7) % 7;
          float fy = side * 1.5708f;
          wrapped(g_furn[f], az ? face + side * 0.1f : cx + tc, ys + 0.1f, az ? cz + tc : face + side * 0.1f, az ? fy : (side > 0 ? 0.f : DS_PI), g_furn_sc[f] * 0.9f);
        } else {
          ucube(az ? cn : cx + tc, ys + 0.1f + bh / 2, az ? cz + tc : cn, az ? 1.6f : bw, bh, az ? bw : 1.6f, M_WRAP, false);
        }
        t += bw + 0.3f;
      }
    }
  }
  /* frost along the top */
  ucube(cx, y + h + 0.06f, cz, x1 - x0, 0.14f, z1 - z0, M_EYE, false);
}
/* a heater core: a caged amber column on a dark plinth, a warm pool on the floor, pipes to the walls */
static void heater(float x, float y, float z) {
  col(v3_(x - 4.6f, y, z - 4.6f), v3_(x + 4.6f, y + 16, z + 4.6f));
  ucube(x, y + 0.7f, z, 9.4f, 1.4f, 9.4f, M_DARK, true);
  ucube(x, y + 8.4f, z, 5.2f, 14, 5.2f, M_AMBER, false);
  for (int sx = -1; sx <= 1; sx += 2) for (int sz = -1; sz <= 1; sz += 2) ucube(x + sx * 3.1f, y + 8.4f, z + sz * 3.1f, 0.5f, 15, 0.5f, M_DARK, false);
  for (int i = 0; i < 4; i++) ucube(x, y + 2.5f + i * 3.6f, z, 6.9f, 0.4f, 6.9f, M_DARK, false);
  ucube(x, y + 16.2f, z, 8.2f, 0.9f, 8.2f, M_DARK, false);
  ucube(x, y + 0.05f, z, 17, 0.08f, 17, M_GOLD, false);
  ucube(x, y + 22, z, 1.2f, 12, 1.2f, M_TUBE, false);
  ucube(x - 12, y + 6, z + 2, 24, 1.1f, 1.1f, M_TUBE, false);
  ucube(x + 12, y + 3, z - 2, 24, 1.1f, 1.1f, M_TUBE, false);
}
/* a warm corner: a nest of colony glass with a heater lamp and a blanket; the sleepers are grunts placed by the caller */
static void nook(float x, float y, float z, float yaw) {
  nest(x, y, z, yaw, 1.0f);
  ucube(x + sinf(yaw) * 3.4f, y + 0.5f, z + cosf(yaw) * 3.4f, 1.2f, 1.0f, 1.2f, M_AMBER, false);
  ucube(x + sinf(yaw) * 4.6f, y + 0.08f, z + cosf(yaw) * 4.6f, 3.2f, 0.16f, 2.4f, M_WRAP, false);
}
/* a cargo conveyor over a pit: a belt platform that shuttles along z, with guide rails and a gantry */
static void conveyor(float zlo, float zhi, float xh, float y, float period, float phase) {
  const float L = 9;
  float amp = (zhi - zlo - L) / 2 - 1.5f, zc = (zlo + zhi) / 2;
  g_platmat = M_BELT;
  plat(-xh, zc - L / 2, xh, zc + L / 2, y, 1.0f, v3_(0, 0, amp), period, phase, false);
  g_platmat = M_PLATE;
  for (int sd = -1; sd <= 1; sd += 2) {
    ucube(sd * (xh + 1.4f), y + 0.5f, zc, 0.3f, 1.0f, zhi - zlo, M_ORANGE, false);
    for (float z = zlo + 4; z < zhi; z += 12) ucube(sd * (xh + 1.4f), y - 6, z, 0.4f, 12, 0.4f, M_DARK, false);
  }
  for (float z = zlo + 6; z < zhi; z += 16) { ucube(0, y + 11, z, xh * 2 + 6, 0.8f, 0.8f, M_DARK, false); ucube(0, y + 10.4f, z, 2.6f, 0.4f, 0.8f, M_AMBER, false); }
}
/* a lone tenant: a soft green body, a head and two eyes; shared geometry, so cheap */
static void tenant(float x, float y, float z, float yaw, float s) {
  t3_mesh *b = t3_mesh_new(g_sph, g_mat[M_TENANT]);
  t3_object_set_position(b, x, y + 0.55f * s, z); t3_object_set_scale(b, 1.0f * s, 1.15f * s, 0.9f * s);
  t3_object_set_rotation(b, 0, yaw, 0); attach(b);
  t3_mesh *h = t3_mesh_new(g_sph, g_mat[M_TENANT]);
  t3_object_set_position(h, x + sinf(yaw) * 0.12f * s, y + 1.3f * s, z + cosf(yaw) * 0.12f * s); t3_object_set_scale(h, 0.7f * s, 0.7f * s, 0.7f * s); attach(h);
  for (int e = -1; e <= 1; e += 2) {
    float ex = x + sinf(yaw) * 0.42f * s + cosf(yaw) * e * 0.17f * s, ez = z + cosf(yaw) * 0.42f * s - sinf(yaw) * e * 0.17f * s;
    t3_mesh *w = t3_mesh_new(g_sph, g_mat[M_EYE]);
    t3_object_set_position(w, ex, y + 1.38f * s, ez); t3_object_set_scale(w, 0.22f * s, 0.26f * s, 0.2f * s); attach(w);
    t3_mesh *pu = t3_mesh_new(g_sph, g_mat[M_DARK]);
    t3_object_set_position(pu, ex + sinf(yaw) * 0.07f * s, y + 1.38f * s, ez + cosf(yaw) * 0.07f * s); t3_object_set_scale(pu, 0.09f * s, 0.1f * s, 0.09f * s); attach(pu);
  }
}
/* a huddle of refugee tenants round a little heater and a blanket */
static void huddle(float cx, float y, float cz, int n, float r) {
  ucube(cx, y + 0.06f, cz, r * 2.6f, 0.12f, r * 2.6f, M_WRAP, false);
  ucube(cx, y + 0.35f, cz, 0.7f, 0.7f, 0.7f, M_AMBER, false);
  for (int i = 0; i < n; i++) {
    float a = i * 6.2832f / n + 0.4f;
    tenant(cx + cosf(a) * r, y + 0.1f, cz + sinf(a) * r, atan2f(-cosf(a), -sinf(a)), 0.9f + 0.25f * frand_seed(i + 33));
  }
}

/* ── level 2: Cold Storage ──
 * The Lessee's freezer holds. A long hall of wrapped furniture: landing dock, rack aisles with
 * warm corners, the frozen sofa, two cargo conveyors over a pit, the heater core, a climb over
 * the high racks, a Brute arena, and the cargo lift out. */
static void build_cold(void) {
  g_spawn = v3_(0, 0, 8);
  g_kill_y = -30;
  g_level = 2;
  /* the hold: frost-white side walls, far stacks of wrapped goods, girders and amber lamps overhead (the breach we drop through is the open bay at the landing) */
  facade(-110, -60, -90, 800, -40, 70, M_HULL, 14);
  facade(90, -60, 110, 800, -40, 70, M_HULL, 14);
  facade(-110, 800, 110, 818, -40, 70, M_HULL, 14);
  for (int sd = -1; sd <= 1; sd += 2) {
    tbox(v3_(sd * 70 - 12, -30, 70), v3_(sd * 70 + 12, 44, 340), M_WRAP, 6, false);
    tbox(v3_(sd * 70 - 12, -30, 380), v3_(sd * 70 + 12, 52, 700), M_WRAP, 6, false);
  }
  for (float z = 90; z < 780; z += 40) {
    ucube(0, 44, z, 180, 1.6f, 2.4f, M_DARK, false);
    for (int sd = -1; sd <= 1; sd += 2) { ucube(sd * 24.f, 41.6f, z + 8, 3, 0.5f, 1.4f, M_AMBER, false); ucube(sd * 24.f, 43, z + 8, 0.12f, 2.4f, 0.12f, M_DARK, false); }
  }
  ucube(-34, 44, 440, 2, 1.6f, 700, M_DARK, false); ucube(34, 44, 440, 2, 1.6f, 700, M_DARK, false);
  ucube(0, -25, 296, 90, 2, 128, M_DARK, false);   /* the pit's floor, far below: nobody lands on it */

  /* S1 landing dock */
  flat(-30, -14, 30, 72, 0, 2.5f, M_FROST);
  wall(-30, -14, 30, -12.5f, 0, 6, M_HULL);
  rack(-30, 12, -22, 62, 0, 12.6f, true); rack(22, 12, 30, 62, 0, 12.6f, true);
  wrapbox(-9, 36, 4, 2.6f, 1.6f, 0); wrapbox(8, 42, 5, 2.6f, 1.6f, 0); wrapbox(3, 56, 3.4f, 3.4f, 1.6f, 0);
  wrapbox(-14, 24, 5, 5, 3.2f, 0); wrapbox(15, 28, 5, 5, 3.2f, 0);
  cp(0, 0, 8); g_cp[0].pos = g_spawn;
  pk(PK_HEALTH, -17, 0, 50); pk(PK_SNIPE_AMMO, 17, 0, 52);
  en(EN_GRUNT, -12, 46, 1); en(EN_GRUNT, 10, 50, 1); en(EN_GRUNT, 0, 63, 1);
  en(EN_FLYER, 0, 56, 1);

  /* S2 rack aisles with the Tenants' warm corners in the cross gaps */
  flat(-26, 72, 26, 172, 0, 2.5f, M_FROST);
  for (int i = 0; i < 3; i++) {
    float z0 = 74 + i * 36;
    rack(-26, z0, -12, z0 + 24, 0, 12.6f, true); rack(12, z0, 26, z0 + 24, 0, 12.6f, true);
  }
  nook(-19, 0, 104, 1.57f); nook(19, 0, 140, -1.57f);
  wrapbox(-3, 84, 4, 2.6f, 1.6f, 0); wrapbox(4, 100, 5, 2.6f, 1.6f, 0); wrapbox(-4, 120, 4, 2.6f, 1.6f, 0);
  wrapbox(5, 140, 5, 2.6f, 1.6f, 0); wrapbox(0, 158, 6, 2.6f, 1.6f, 0);
  enh(EN_TURRET, -12.8f, 12.6f, 86); enh(EN_TURRET, 12.8f, 12.6f, 122); enh(EN_TURRET, -12.8f, 12.6f, 152);
  en(EN_GRUNT, -16, 100, 1); en(EN_GRUNT, -20, 109, 1); en(EN_GRUNT, 17, 138, 1); en(EN_GRUNT, 21, 145, 1); en(EN_GRUNT, 0, 92, 1); en(EN_GRUNT, 2, 130, 1);
  enh(EN_FLYER, -8, 13, 92); enh(EN_FLYER, 8, 14, 128);
  pk(PK_HEALTH, -7, 0, 116); pk(PK_DECOY, 6, 0, 150); pk(PK_SNIPE_AMMO, -18, 0, 140);

  /* S3 the frozen sofa */
  flat(-34, 172, 34, 232, 0, 2.5f, M_FROST);
  rack(-34, 176, -26, 228, 0, 12.6f, true); rack(26, 176, 34, 228, 0, 12.6f, true);
  wrapbox(0, 206, 11, 7.5f, 0.9f, 0);
  wrapped(MD_SOFAL, 0, 0.9f, 206, 0, 3.4f);
  put(MD_LAMP, -9, 0, 200, 0, 2.6f); put(MD_LAMP2, 9, 0, 212, 0, 2.6f);
  ucube(-9, 3.6f, 200, 1.2f, 1.2f, 1.2f, M_AMBER, false); ucube(9, 3.4f, 212, 1.2f, 1.2f, 1.2f, M_AMBER, false);
  wrapbox(-19, 190, 5, 4, 3.2f, 0); wrapbox(19, 194, 5, 4, 3.2f, 0); wrapbox(-14, 221, 4, 2.6f, 1.6f, 0); wrapbox(14, 223, 4, 2.6f, 1.6f, 0);
  enh(EN_TURRET, -29.5f, 12.6f, 200); enh(EN_TURRET, 29.5f, 12.6f, 214);
  en(EN_GRUNT, -22, 186, 1); en(EN_GRUNT, 22, 188, 1); en(EN_GRUNT, -10, 216, 1); en(EN_GRUNT, 10, 220, 1);
  en(EN_FLYER, -12, 200, 1); en(EN_FLYER, 12, 206, 1);
  pk(PK_HEALTH, -28, 0, 180); pk(PK_SNIPE_AMMO, 28, 0, 180); pk(PK_DECOY, 0, 0, 226);

  /* S4 two cargo conveyors over the pit, with an island between */
  conveyor(232, 286, 6, 0, 13.f, -DS_PI / 2);
  flat(-16, 286, 16, 308, 0, 2.5f, M_FROST);
  wrapbox(-8, 296, 4, 2.6f, 1.6f, 0); wrapbox(9, 300, 4, 2.6f, 1.6f, 0);
  cp(-2, 0, 297);
  conveyor(308, 360, 6, 0, 13.5f, DS_PI / 2);
  static const float py[4][2] = {{-22, 246}, {22, 266}, {-22, 326}, {22, 344}};
  for (int i = 0; i < 4; i++) {
    float x = py[i][0], z = py[i][1];
    col(v3_(x - 1.5f, -30, z - 1.5f), v3_(x + 1.5f, 10.5f, z + 1.5f));
    ubox(v3_(x - 1.5f, -30, z - 1.5f), v3_(x + 1.5f, 10.5f, z + 1.5f), M_DARK, true);
    ucube(x, 10.7f, z, 3.6f, 0.4f, 3.6f, M_ORANGE, false);
    if (i != 2) enh(EN_TURRET, x, 10.5f, z);
  }
  en(EN_GRUNT, -6, 292, 1); en(EN_GRUNT, 8, 296, 1); en(EN_GRUNT, 0, 303, 1);
  enh(EN_FLYER, 0, 7, 250); enh(EN_FLYER, 6, 8, 272); enh(EN_FLYER, -6, 7, 322); enh(EN_FLYER, 0, 8, 346);
  pk(PK_HEALTH, -12, 0, 300); pk(PK_SHIELD, 12, 0, 292);

  /* S5 the heater core and the sleepers */
  flat(-36, 360, 36, 450, 0, 2.5f, M_FROST);
  rack(-36, 366, -28, 444, 0, 12.6f, true); rack(28, 366, 36, 444, 0, 12.6f, true);
  heater(0, 0, 408);
  nook(-20, 0, 380, 0.62f); nook(20, 0, 384, -0.69f); nook(-22, 0, 430, 2.36f); nook(22, 0, 430, -2.36f);
  wrapbox(-10, 372, 5, 3, 1.6f, 0); wrapbox(12, 374, 5, 3, 1.6f, 0); wrapbox(-12, 441, 4, 3, 1.6f, 0); wrapbox(10, 439, 4, 3, 1.6f, 0);
  form(-1.5f, 5, 405.2f, DS_PI, FM_LEASE, 1.8f, false);
  ucube(2.5f, 0.35f, 400, 0.6f, 0.6f, 2.6f, M_TUBE, false);   /* the filing tube the lease was in */
  cp(-2, 0, 368);
  enh(EN_TURRET, -28.8f, 12.6f, 392); enh(EN_TURRET, 28.8f, 12.6f, 412);
  en(EN_GRUNT, -18, 384, 1); en(EN_GRUNT, 18, 388, 1); en(EN_GRUNT, -20, 426, 1); en(EN_GRUNT, 20, 427, 1); en(EN_GRUNT, 0, 436, 1);
  enh(EN_FLYER, -12, 13, 398); enh(EN_FLYER, 12, 14, 420);
  pk(PK_HEALTH, -28, 0, 400); pk(PK_HEALTH, 28, 0, 400); pk(PK_SHIELD, 0, 0, 444); pk(PK_SNIPE_AMMO, 0, 0, 366);

  /* S6 up the stairs to the high racks: two gantries with a glide between, down again to the arena */
  stairs(0, 0, 450, 0, 12, 25, 0.4f, 0.9f);
  flat(-12, 472.5f, 12, 498, 10, 1.6f, M_PLATE);
  flat(-12, 508, 12, 552, 10, 1.6f, M_PLATE);
  for (int sd = -1; sd <= 1; sd += 2) {
    ucube(sd * 12.1f, 10.6f, 485, 0.2f, 1.2f, 25.5f, M_ORANGE, false); ucube(sd * 12.1f, 10.6f, 530, 0.2f, 1.2f, 44, M_ORANGE, false);
  }
  stairs(0, 0, 574, 1, 12, 25, 0.4f, 0.9f);
  wrapbox(-7, 484, 4, 2.6f, 1.6f, 10); wrapbox(7, 536, 4, 2.6f, 1.6f, 10);
  for (int i = 0; i < 4; i++) {
    float x = (i & 1 ? 22.f : -22.f), z = 482 + i * 22;
    if (i == 1 || i == 2) { col(v3_(x - 1.5f, -30, z - 1.5f), v3_(x + 1.5f, 19, z + 1.5f)); ubox(v3_(x - 1.5f, -30, z - 1.5f), v3_(x + 1.5f, 19, z + 1.5f), M_DARK, true); ucube(x, 19.2f, z, 3.6f, 0.4f, 3.6f, M_ORANGE, false); enh(EN_TURRET, x, 19, z); }
  }
  en(EN_GRUNT, -4, 482, 10); en(EN_GRUNT, 5, 492, 10); en(EN_GRUNT, 0, 520, 10);
  enh(EN_FLYER, -6, 14, 503); enh(EN_FLYER, 6, 15, 530);
  pk(PK_HEALTH, 8, 10, 476); pk(PK_SNIPE_AMMO, -8, 10, 546); pk(PK_DECOY, 0, 10, 512);

  /* S7 the Brute arena among the frozen furniture */
  flat(-28, 574, 28, 660, 0, 2.5f, M_FROST);
  g_arena_lo = v3_(-28, -3, 588); g_arena_hi = v3_(28, 24, 656);
  wall(-28, 586, -7, 588, 0, 6, M_HULL); wall(7, 586, 28, 588, 0, 6, M_HULL);
  wall(-28, 656, -7, 658, 0, 6, M_HULL); wall(7, 656, 28, 658, 0, 6, M_HULL);
  rack(-34, 588, -28, 656, 0, 12.6f, true); rack(28, 588, 34, 656, 0, 12.6f, true);
  wrapbox(-14, 604, 6, 5, 3.6f, 0); wrapped(MD_SOFA, -14, 3.6f, 604, 0.5f, 2.6f);
  wrapbox(14, 604, 6, 5, 3.6f, 0); wrapped(MD_BED, 14, 3.6f, 604, -0.4f, 2.0f);
  wrapbox(-14, 640, 6, 5, 3.6f, 0); wrapped(MD_FRIDGE, -14, 3.6f, 640, 0.2f, 1.7f);
  wrapbox(14, 640, 6, 5, 3.6f, 0); wrapped(MD_SHELF, 14, 3.6f, 640, 3.0f, 1.9f);
  wrapbox(0, 622, 4, 4, 1.6f, 0);
  cp(-2, 0, 580);
  enh(EN_TURRET, -30.5f, 12.6f, 610); enh(EN_TURRET, 30.5f, 12.6f, 636);
  en(EN_BRUTE, 0, 640, 0); en(EN_GRUNT, -16, 626, 1); en(EN_GRUNT, 16, 626, 1);
  pk(PK_HEALTH, -24, 0, 594); pk(PK_HEALTH, 24, 0, 594); pk(PK_SNIPE_AMMO, 0, 0, 592); pk(PK_SHIELD, 0, 0, 650);

  /* the cargo lift: a cage with something big waiting to ship */
  flat(-10, 658, 10, 692, 0, 2.5f, M_FROST);
  flat(-10, 692, 10, 714, 0, 2.5f, M_PLATE);
  for (int sx = -1; sx <= 1; sx += 2) for (int sz = 0; sz < 2; sz++) ucube(sx * 10.f, 20, 692 + sz * 22.f, 0.7f, 40, 0.7f, M_DARK, false);
  ucube(0, 40, 692, 21, 1, 1, M_DARK, false); ucube(0, 40, 714, 21, 1, 1, M_DARK, false);
  ucube(0, 36, 703, 1, 1, 22, M_AMBER, false);
  wrapbox(-5, 706, 5, 4, 5, 0); wrapped(MD_SOFAL, -5, 5, 706, 1.2f, 2.4f);
  g_exit_lo = v3_(-8, -1, 696); g_exit_hi = v3_(8, 8, 712);

  /* signs, forms, the hold's own notices */
  sign(0, 3.6f, -9, DS_PI, SG_C_DATE, 7);
  sign(0, 5.2f, 70, DS_PI, SG_C_COLD, 6);
  sign(0, 5.2f, 98, DS_PI, SG_C_WARM, 6);
  sign(0, 4.4f, 196, DS_PI, SG_C_FRAGILE, 6);
  sign(-7, 3.4f, 230, DS_PI, SG_C_CONV, 5);
  sign(-9, 3.6f, 372, DS_PI, SG_C_HEAT, 6);
  sign(8, 3.6f, 398, DS_PI, SG_C_LEASE, 6);
  sign(0, 14, 470, DS_PI, SG_C_DATE, 6);
  sign(0, 5.2f, 590, DS_PI, SG_C_COLD, 6);
  sign(0, 6, 690, DS_PI, SG_C_LIFT, 7);
  form(-6, 0, 22, 0.4f, FM_NOISE, 0.9f, true); form(7, 0, 112, 1.1f, FM_WINDOW, 0.9f, true); form(-19, 0, 112, 2.0f, FM_VIEW, 0.9f, true);
  form(18, 0, 146, 0.7f, FM_FAMILY, 0.9f, true); form(-20, 0, 386, 0.3f, FM_NOISE, 0.9f, true); form(21, 0, 392, 1.9f, FM_FAMILY, 0.9f, true);
  form(-3, 10, 478, 0.9f, FM_VIEW, 0.9f, true); form(4, 0, 598, 0.6f, FM_NOISE, 0.9f, true);
  for (float z = 6; z < 70; z += 30) { ucube(-27, 10.5f, z, 0.2f, 3, 0.2f, M_DARK, false); }
  zone(0, -30, -3, -14, 30, 12, 46);
  zone(1, -26, -3, 92, 26, 12, 150);
  zone(2, -34, -3, 188, 34, 10, 226);
  zone(3, -14, -3, 386, 14, 16, 428);
  zone(4, -10, -3, 672, 10, 8, 695);
  anchor("landing", g_spawn, 0);
  anchor("brute_arena", v3_(0, 0, 622), 0);
  anchor("exit", v3_(0, 0, 700), 0);
  anchor("vista", v3_(0, 0, 408), 0);
  gate(-7, 7, 587, 0, 6);
  gate(-7, 7, 657, 0, 6);
  g_gate_probe = v3_(0, 0, 570); g_gate_r = 24;
  rings(170, 10);
}

/* ── level 3: The Annex ──
 * Halcyon Reach at morning. Nine roofs of glass towers, crossed by glides, two window-washing
 * gondolas and a lift gondola: tower 9 (landing), a lobby of huddled refugees, the Brute bolting a
 * turret to tower 4, the crew lockers, a rooftop arena, and the tram to the tether. */
/* a tower: a glass body hanging 120 m below a sun-bleached roof; solid all the way down so nobody glides through it */
static void tower(float x0, float z0, float x1, float z1, float y) {
  col(v3_(x0 - 0.5f, y - 120, z0 - 0.5f), v3_(x1 + 0.5f, y, z1 + 0.5f));
  ucube((x0 + x1) / 2, y - 60.6f, (z0 + z1) / 2, x1 - x0 - 0.6f, 119.4f, z1 - z0 - 0.6f, M_GLASS, false);
  deck_vis(x0 - 0.5f, z0 - 0.5f, x1 + 0.5f, z1 + 0.5f, y, 1.2f, M_ROOF, false);
}
static void wallseg(float x0, float z0, float x1, float z1, float y, float h, int mat) {
  col(v3_(x0, y, z0), v3_(x1, y + h, z1));
  ubox(v3_(x0, y, z0), v3_(x1, y + h, z1), mat, false);
}
/* a glass pavilion on a roof: sill and frame you can see through but not walk through, a door gap in both short walls */
static void pavilion(float x0, float z0, float x1, float z1, float y, float h, float door) {
  float cx = (x0 + x1) / 2;
  col(v3_(x0, y, z0), v3_(x0 + 0.5f, y + h, z1)); col(v3_(x1 - 0.5f, y, z0), v3_(x1, y + h, z1));
  col(v3_(x0, y, z0), v3_(cx - door / 2, y + h, z0 + 0.5f)); col(v3_(cx + door / 2, y, z0), v3_(x1, y + h, z0 + 0.5f));
  col(v3_(x0, y, z1 - 0.5f), v3_(cx - door / 2, y + h, z1)); col(v3_(cx + door / 2, y, z1 - 0.5f), v3_(x1, y + h, z1));
  for (int sd = 0; sd < 2; sd++) {
    float xx = sd ? x1 - 0.25f : x0 + 0.25f;
    ucube(xx, y + 0.5f, (z0 + z1) / 2, 0.5f, 1.0f, z1 - z0, M_ORANGE, false);
    for (float z = z0; z <= z1 + 0.1f; z += 5) ucube(xx, y + h / 2, z, 0.5f, h, 0.5f, M_ORANGE, false);
  }
  for (int sd = 0; sd < 2; sd++) {
    float zz = sd ? z1 - 0.25f : z0 + 0.25f;
    ucube((x0 + cx - door / 2) / 2, y + 0.5f, zz, cx - door / 2 - x0, 1.0f, 0.5f, M_ORANGE, false);
    ucube((x1 + cx + door / 2) / 2, y + 0.5f, zz, x1 - cx - door / 2, 1.0f, 0.5f, M_ORANGE, false);
    for (float x = x0; x <= x1 + 0.1f; x += 5) if (fabsf(x - cx) > door / 2 - 0.2f) ucube(x, y + h / 2, zz, 0.5f, h, 0.5f, M_ORANGE, false);
    ucube(cx - door / 2, y + h / 2, zz, 0.5f, h, 0.5f, M_ORANGE, false); ucube(cx + door / 2, y + h / 2, zz, 0.5f, h, 0.5f, M_ORANGE, false);
  }
  col(v3_(x0 - 0.8f, y + h, z0 - 0.8f), v3_(x1 + 0.8f, y + h + 0.7f, z1 + 0.8f));
  ubox(v3_(x0 - 0.8f, y + h, z0 - 0.8f), v3_(x1 + 0.8f, y + h + 0.7f, z1 + 0.8f), M_GOLD, true);
}
/* a window-washing gondola: a platform that rides a rig, with rails and hoist cables up to it */
static void gondola(float x0, float z0, float x1, float z1, float y, v3 axis, float period, float phase, float cable) {
  plat_t *p = plat(x0, z0, x1, z1, y, 0.9f, axis, period, phase, true);
  if (!p) return;
  g_cur = p->vis;
  float cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
  ucube(x0 + 0.1f, y + 0.9f, cz, 0.12f, 0.12f, z1 - z0, M_ORANGE, false); ucube(x1 - 0.1f, y + 0.9f, cz, 0.12f, 0.12f, z1 - z0, M_ORANGE, false);
  ucube(cx, y + 0.9f, z0 + 0.1f, x1 - x0, 0.12f, 0.12f, M_ORANGE, false); ucube(cx, y + 0.9f, z1 - 0.1f, x1 - x0, 0.12f, 0.12f, M_ORANGE, false);
  for (int sx = 0; sx < 2; sx++) for (int sz = 0; sz < 2; sz++) {
    float px = sx ? x1 - 0.1f : x0 + 0.1f, pz = sz ? z1 - 0.1f : z0 + 0.1f;
    ucube(px, y + 0.5f, pz, 0.14f, 1.0f, 0.14f, M_ORANGE, false);
    if (cable > 0 && sx == sz) ucube(px, y + cable / 2, pz, 0.08f, cable, 0.08f, M_DARK, false);
  }
  ucube(cx, y + 1.5f, cz, 0.5f, 1.0f, 0.5f, M_TEAL, false);   /* the bucket */
  g_cur = NULL;
}
static void bridge(float x0, float x1, float z0, float z1, float y) {
  flat(x0, z0, x1, z1, y, 0.8f, M_PLATE);
  for (int sd = 0; sd < 2; sd++) {
    float xx = sd ? x1 - 0.1f : x0 + 0.1f;
    ucube(xx, y + 1.0f, (z0 + z1) / 2, 0.12f, 0.12f, z1 - z0, M_ORANGE, false);
    for (float z = z0 + 1; z < z1; z += 4) ucube(xx, y + 0.5f, z, 0.12f, 1.0f, 0.12f, M_DARK, false);
  }
  ucube((x0 + x1) / 2, y - 3, (z0 + z1) / 2, 0.5f, 5, z1 - z0, M_DARK, false);
}
static void ac_unit(float x, float y, float z, float yaw) {
  col(v3_(x - 1.6f, y, z - 1.1f), v3_(x + 1.6f, y + 1.5f, z + 1.1f));
  t3_mesh *m = ucube(x, y + 0.75f, z, 3.2f, 1.5f, 2.2f, M_PLATE, true); t3_object_set_rotation(m, 0, yaw, 0);
  ucube(x, y + 1.55f, z, 1.6f, 0.1f, 1.6f, M_DARK, false);
}
/* the crew's lockers: a hut of hi-vis cabinets, a bench, a thermos and a note */
static void lockers(float x0, float z0, float x1, float z1, float y) {
  pavilion(x0, z0, x1, z1, y, 4.6f, 10);
  for (int sd = 0; sd < 2; sd++) {
    float xx = sd ? x1 - 1.2f : x0 + 1.2f;
    for (float z = z0 + 2; z < z1 - 1.5f; z += 2.2f) {
      ucube(xx, y + 1.15f, z, 1.1f, 2.3f, 1.8f, M_ORANGE, false);
      ucube(xx + (sd ? -0.57f : 0.57f), y + 1.5f, z, 0.06f, 1.2f, 0.2f, M_DARK, false);
    }
  }
  float cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
  col(v3_(cx - 1.4f, y, cz - 0.5f), v3_(cx + 1.4f, y + 0.5f, cz + 0.5f));
  ucube(cx, y + 0.25f, cz, 2.8f, 0.5f, 1.0f, M_PLATE, false);
  ucube(cx - 0.6f, y + 0.85f, cz, 0.28f, 0.7f, 0.28f, M_TEAL, false); ucube(cx - 0.6f, y + 1.25f, cz, 0.32f, 0.12f, 0.32f, M_DARK, false);
  form(cx + 0.5f, y + 0.52f, cz, 0.3f, FM_NOISE, 0.5f, true);
  sign(cx, y + 2.6f, z1 - 1.2f, DS_PI, SG_A_NOTE, 3.2f);
}

static void build_annex(void) {
  g_spawn = v3_(0, 0, 8);
  g_kill_y = -80;
  g_level = 3;
  /* the ring at morning: planet below, the colony's own arc overhead, far towers in the gold haze */
  planet(-60, -3300, 700, 2500);
  colony(0, 420, 1800, 560, -0.5f, 0.1f);
  for (int i = 0; i < 26; i++) {
    float z = -40 + i * 30, sd = (i & 1) ? 1.f : -1.f, x = sd * rr(90, 220), top = rr(-60, 40);
    t3_mesh *m = t3_mesh_new(g_unit, g_mat[i % 5 == 2 ? M_TOWERGAP : M_TOWER]);
    float w = rr(24, 34);
    t3_object_set_position(m, x, top - w * 2, z + rr(-10, 10)); t3_object_set_scale(m, w, w * 4, w);
    T3_OBJ(m)->frustum_culled = false;
    attach(m);
  }
  /* the colony's glass towers either side of the route, tops at all heights */
  for (int i = 0; i < 28; i++) {
    float z = -30 + i * 27.f, sd = (i & 1) ? 1.f : -1.f, x = sd * (62 + 34 * frand_seed(i * 7 + 3)), top = -30 + 70 * frand_seed(i * 11 + 5);
    ucube(x, top - 60, z, 28, 120, 28, M_GLASS, false);
    ucube(x, top + 0.3f, z, 29, 0.8f, 29, M_GOLD, false);
  }
  /* roofs (x0, z0, x1, z1, y) */
  tower(-22, -20, 22, 40, 0);       /* 9: landing */
  tower(-18, 64, 18, 104, -5);      /* 8 */
  tower(-20, 142, 20, 204, -8);     /* 7: the lobby */
  tower(-10, 228, 34, 272, -16);    /* 6 */
  tower(-10, 290, 34, 330, -4);     /* 5 */
  tower(-22, 356, 18, 408, -9);     /* 4: the Brute and the turret */
  tower(-24, 444, 16, 502, -12);    /* 3: the crew lockers */
  tower(-30, 532, 30, 604, -12);    /* 2: the rooftop arena */
  tower(-24, 630, 24, 700, -12);    /* 1: the tram */

  /* tower 9: the landing, Juno's gondola parked on the roof */
  cp(0, 0, 8); g_cp[0].pos = g_spawn;
  col(v3_(-16.5f, 0, -12), v3_(-6.5f, 0.4f, -3));
  ubox(v3_(-16.5f, 0, -12), v3_(-6.5f, 0.4f, -3), M_ORANGE, true);
  for (int sd = 0; sd < 2; sd++) { ucube(sd ? -6.6f : -16.4f, 1.0f, -7.5f, 0.12f, 1.2f, 9, M_DARK, false); ucube(-11.5f, 1.0f, sd ? -3.1f : -11.9f, 10, 1.2f, 0.12f, M_DARK, false); }
  ucube(-8, 0.9f, -9, 0.5f, 1.0f, 0.5f, M_TEAL, false); ucube(-14, 1.6f, -10.5f, 0.12f, 2.4f, 0.12f, M_TEAL, false); ucube(-14, 2.8f, -10.5f, 1.4f, 0.12f, 0.12f, M_TEAL, false);
  ucube(-11.5f, 6, -19.4f, 0.3f, 12, 0.3f, M_DARK, false); ucube(-11.5f, 11.8f, -16, 0.3f, 0.3f, 8, M_DARK, false);   /* the davit it hangs from */
  sign(-11.5f, 2.9f, -3.2f, DS_PI, SG_A_GONDOLA, 3.4f);
  sign(0, 3.4f, -14, DS_PI, SG_A_TOWER, 7);
  putfit(MD_ANT, 15, 0, -10, 0, 2.2f, 0.5f, 1); putfit(MD_RADAR, 14, 0, 12, 1, 2, 0.35f, 1);
  ac_unit(-15, 0, 14, 0.3f); ac_unit(17, 0, 30, 0); wrapbox(-8, 28, 4, 2.6f, 1.6f, 0);
  cover(6, 26, 4, 1.8f, 1.3f, 0); cover(-6, 35, 5, 1.8f, 1.3f, 0);
  pk(PK_HEALTH, 14, 0, 24); pk(PK_SNIPE_AMMO, -17, 0, 24);
  en(EN_GRUNT, -8, 32, 1); en(EN_GRUNT, 10, 36, 1);
  en(EN_FLYER, 0, 36, 1);

  /* tower 8: first glide target; turrets bolted to the roof edge */
  cover(-8, 76, 4, 1.8f, 1.3f, -5); cover(8, 84, 4, 1.8f, 1.3f, -5); cover(0, 94, 5, 1.8f, 1.3f, -5);
  ac_unit(-14, -5, 98, 0.2f); ac_unit(14, -5, 70, 0);
  enh(EN_TURRET, -16, -5, 101); enh(EN_TURRET, 16, -5, 101);
  en(EN_GRUNT, -6, 82, -5); en(EN_GRUNT, 7, 90, -5);
  en(EN_FLYER, -10, 90, -5);
  pk(PK_HEALTH, 14, -5, 74); pk(PK_DECOY, -14, -5, 88); pk(PK_SHIELD, 0, -5, 100);
  sign(0, -1.8f, 66, DS_PI, SG_A_GLIDE, 6);

  /* gondola 1 along a rig between towers 8 and 7 */
  gondola(-5, 118.5f, 5, 127.5f, -6.5f, v3_(0, 0, 13), 12.f, -DS_PI / 2, 16.5f);
  ucube(0, 10, 123, 1.0f, 0.8f, 40, M_DARK, false); ucube(0, 2.5f, 103.5f, 1, 15, 1, M_DARK, false); ucube(0, 1, 141.5f, 1, 18, 1, M_DARK, false);
  ucube(0, 9.3f, 104.5f, 3, 0.4f, 0.4f, M_ORANGE, false); ucube(0, 9.3f, 141.5f, 3, 0.4f, 0.4f, M_ORANGE, false);
  enh(EN_FLYER, 0, 4, 112); enh(EN_FLYER, 4, 5, 132);

  /* tower 7: the lobby, with its refugees. Nothing here shoots; the Brute stands behind it. */
  cp(-2, -8, 146);
  pavilion(-14, 150, 14, 178, -8, 5, 10);
  ubox(v3_(-13.5f, -8.05f, 150.5f), v3_(13.5f, -7.9f, 177.5f), M_TEAL, false);   /* the lobby carpet */
  put(MD_DESK, 0, -8, 172, DS_PI, 2.4f); col(v3_(-2.4f, -8, 171), v3_(2.4f, -6.9f, 173.6f));
  huddle(-8, -8, 165, 6, 2.0f); huddle(8, -8, 168, 5, 1.8f); huddle(-9, -8, 156, 4, 1.4f);
  for (int i = 0; i < 4; i++) put(MD_CHAIR, 3.5f, -8, 154 + i * 2.6f, 1.57f, 1.8f);
  put(MD_PLANT2, -12, -8, 176, 0, 2.0f); put(MD_PLANT, 12, -8, 152, 0, 2.2f);
  sign(0, -3.2f, 151.3f, DS_PI, SG_A_LOBBY, 5);
  form(-4, -8, 160, 0.4f, FM_FAMILY, 0.9f, true); form(2, -8, 166, 1.5f, FM_WINDOW, 0.9f, true); form(10, -8, 160, 0.2f, FM_VIEW, 0.9f, true);
  ac_unit(-14, -8, 192, 0.1f); ac_unit(15, -8, 186, 0);
  cover(-6, 190, 5, 1.8f, 1.3f, -8); cover(8, 197, 5, 1.8f, 1.3f, -8);
  enh(EN_TURRET, -17, -8, 200); enh(EN_TURRET, 17, -8, 200);
  en(EN_BRUTE, 0, 196, -8);
  en(EN_FLYER, -8, 186, -8); en(EN_FLYER, 8, 192, -8);
  pk(PK_HEALTH, 0, -8, 183); pk(PK_SNIPE_AMMO, -17, -8, 190);

  /* tower 6: after a long glide down */
  cp(12, -16, 236);
  cover(-2, 240, 4, 1.8f, 1.3f, -16); cover(20, 250, 5, 1.8f, 1.3f, -16); cover(8, 262, 5, 1.8f, 1.3f, -16); cover(26, 238, 4, 1.8f, 1.3f, -16);
  putfit(MD_ANT, -6, -16, 266, 0, 2.2f, 0.5f, 1);
  enh(EN_TURRET, 31, -16, 232); enh(EN_TURRET, -7, -16, 262); enh(EN_TURRET, 31, -16, 268);
  en(EN_GRUNT, 4, 244, -16); en(EN_GRUNT, 24, 256, -16);
  en(EN_FLYER, 4, 250, -16); en(EN_FLYER, 22, 244, -16); en(EN_FLYER, 14, 262, -16);
  pk(PK_HEALTH, -6, -16, 246); pk(PK_SNIPE_AMMO, 28, -16, 250); pk(PK_DECOY, 14, -16, 232);
  sign(8, -12.6f, 232, DS_PI, SG_A_STREAK, 5);
  /* a lift gondola up to tower 5, riding the gap on guide posts */
  gondola(12, 275, 24, 287, -10, v3_(0, 6, 0), 14.f, 0, 40);
  for (int sx = 0; sx < 2; sx++) for (int sz = 0; sz < 2; sz++) ucube(sx ? 25.2f : 10.8f, -10, sz ? 288.2f : 273.8f, 0.5f, 60, 0.5f, M_DARK, false);
  ucube(18, 8, 281, 16, 0.7f, 16, M_DARK, false);
  enh(EN_FLYER, 18, -6, 262);

  /* tower 5: a Brute, and turrets on the roof edge */
  ac_unit(-4, -4, 300, 0); ac_unit(30, -4, 322, 0.4f);
  cover(8, 306, 5, 1.8f, 1.3f, -4); cover(24, 312, 4, 1.8f, 1.3f, -4); cover(14, 322, 5, 1.8f, 1.3f, -4);
  enh(EN_TURRET, -8, -4, 326); enh(EN_TURRET, 32, -4, 326);
  en(EN_BRUTE, 14, 314, -4); en(EN_GRUNT, 28, 300, -4);
  en(EN_FLYER, 4, 312, -4); en(EN_FLYER, 24, 322, -4);
  pk(PK_HEALTH, -6, -4, 296); pk(PK_SHIELD, 30, -4, 318);

  /* tower 4: the Brute is bolting a turret to a load-bearing window */
  cp(0, -9, 362);
  col(v3_(2, -9, 386), v3_(6, -8.4f, 390)); ubox(v3_(2, -9, 386), v3_(6, -8.4f, 390), M_DARK, true);
  for (int i = 0; i < 4; i++) ucube(i & 1 ? 5.4f : 2.6f, -8.35f, i & 2 ? 389.4f : 386.6f, 0.4f, 0.2f, 0.4f, M_ORANGE, false);
  ucube(-8, -7.6f, 380, 0.3f, 5, 0.3f, M_DARK, false); ucube(-8, -5, 384, 0.3f, 0.3f, 8.6f, M_DARK, false);   /* the crane arm */
  pane(-14, -6.5f, 374, 6.5f, 5.5f, 0.3f, -0.18f, 0, 0);
  sign(-11, -6.4f, 366, DS_PI, SG_A_BOLT, 5);
  cover(-14, 396, 5, 1.8f, 1.3f, -9); cover(10, 372, 4, 1.8f, 1.3f, -9); cover(12, 396, 4, 1.8f, 1.3f, -9);
  ac_unit(-18, -9, 360, 0);
  enh(EN_TURRET, 4, -8.6f, 388); enh(EN_TURRET, -20, -9, 404); enh(EN_TURRET, 16, -9, 404);
  en(EN_BRUTE, -2, 388, -9); en(EN_GRUNT, 8, 396, -9);
  en(EN_FLYER, -10, 372, -9); en(EN_FLYER, 8, 380, -9); en(EN_FLYER, 0, 402, -9);
  pk(PK_HEALTH, 14, -9, 362); pk(PK_SNIPE_AMMO, -18, -9, 372); pk(PK_DECOY, 0, -9, 404);

  /* gondola 2 along a rig between towers 4 and 3 */
  gondola(-7, 421.5f, 3, 430.5f, -10.5f, v3_(0, 0, 12), 11.f, DS_PI / 2, 16.5f);
  ucube(-2, 8, 426, 1.0f, 0.8f, 40, M_DARK, false); ucube(-2, 0, 407.5f, 1, 17, 1, M_DARK, false); ucube(-2, 0, 444.5f, 1, 17, 1, M_DARK, false);
  enh(EN_FLYER, -2, 2, 416); enh(EN_FLYER, 0, 3, 436);

  /* tower 3: the crew lockers */
  cp(-6, -12, 450);
  lockers(-14, 456, 6, 482, -12);
  sign(-4, -7.2f, 453, DS_PI, SG_A_LOCKERS, 5);
  ac_unit(-18, -12, 490, 0.2f); ac_unit(10, -12, 468, 0);
  cover(-12, 494, 5, 1.8f, 1.3f, -12); cover(4, 492, 5, 1.8f, 1.3f, -12);
  enh(EN_TURRET, -21, -12, 490); enh(EN_TURRET, 13, -12, 490);
  en(EN_GRUNT, -4, 488, -12); en(EN_GRUNT, 8, 476, -12);
  en(EN_FLYER, -10, 470, -12);
  pk(PK_HEALTH, -2, -12, 466); pk(PK_SNIPE_AMMO, 10, -12, 462);

  /* the skybridge and the rooftop arena on tower 2: two Brutes */
  cp(-2, -12, 510);
  bridge(-4, 4, 502, 532, -12);
  g_arena_lo = v3_(-30, -14, 534); g_arena_hi = v3_(30, 30, 602);
  wall(-30, 532, -4, 534, -12, 5, M_HULL); wall(4, 532, 30, 534, -12, 5, M_HULL);
  wall(-30, 602, -4, 604, -12, 5, M_HULL); wall(4, 602, 30, 604, -12, 5, M_HULL);
  wall(-30, 534, -28.5f, 602, -12, 5, M_HULL); wall(28.5f, 534, 30, 602, -12, 5, M_HULL);
  cover(-12, 552, 5, 5, 3.6f, -12); cover(12, 552, 5, 5, 3.6f, -12); cover(-12, 584, 5, 5, 3.6f, -12); cover(12, 584, 5, 5, 3.6f, -12);
  cover(0, 568, 3, 3, 1.3f, -12);
  ac_unit(-12, -8.4f, 552, 0); ac_unit(12, -8.4f, 584, 0.5f);
  enh(EN_TURRET, -29.2f, -7, 556); enh(EN_TURRET, 29.2f, -7, 580);
  en(EN_BRUTE, -10, 586, -12); en(EN_BRUTE, 10, 586, -12);
  en(EN_FLYER, -16, 570, -12); en(EN_FLYER, 16, 566, -12);
  pk(PK_HEALTH, -24, -12, 540); pk(PK_HEALTH, 24, -12, 540); pk(PK_SNIPE_AMMO, 0, -12, 538); pk(PK_SHIELD, 0, -12, 596);
  bridge(-4, 4, 604, 630, -12);

  /* tower 1: the tram to the Freehold's tether */
  const float tx = 12;
  ubox(v3_(tx - 1.8f, -11.2f, 640), v3_(tx + 1.8f, -7.8f, 690), M_ORANGE, true);
  col(v3_(tx - 1.8f, -12, 640), v3_(tx + 1.8f, -7.8f, 690));
  ucube(tx - 1.86f, -9.6f, 665, 0.1f, 1.3f, 46, M_PANE, false); ucube(tx + 1.86f, -9.6f, 665, 0.1f, 1.3f, 46, M_PANE, false);
  ucube(tx, -7.6f, 665, 3.4f, 0.3f, 50.5f, M_DARK, false);
  for (int i = 0; i < 3; i++) ucube(tx - 1.88f, -10, 650 + i * 15.f, 0.1f, 2.6f, 2.4f, M_DARK, false);
  ucube(tx - 1.0f, -11.9f, 665, 0.25f, 0.2f, 130, M_DARK, false); ucube(tx + 1.0f, -11.9f, 665, 0.25f, 0.2f, 130, M_DARK, false);
  for (float z = 700; z < 760; z += 6) ucube(tx, -13, z, 3.2f, 0.4f, 0.5f, M_DARK, false);
  ucube(tx, -26, 745, 0.6f, 26, 0.6f, M_DARK, false); ucube(tx, -39, 790, 1.2f, 1.2f, 90, M_DARK, false);
  ucube(30, 140, 830, 7, 400, 7, M_CRANE, false); ucube(30, 100, 830, 8.6f, 2, 8.6f, M_REDG, false); ucube(30, 200, 830, 8.6f, 2, 8.6f, M_REDG, false);
  ucube(0, -7.5f, 664, 22, 0.5f, 30, M_ORANGE, false);   /* the station canopy */
  for (int sx = -1; sx <= 1; sx += 2) for (int sz = 0; sz < 3; sz++) ucube(sx * 10.5f, -10, 652 + sz * 12.f, 0.4f, 4.4f, 0.4f, M_DARK, false);
  ac_unit(-18, -12, 690, 0); wrapbox(-14, 648, 4, 2.6f, 1.6f, -12);
  enh(EN_TURRET, -21, -12, 696); en(EN_FLYER, 4, 686, -12);
  pk(PK_HEALTH, -16, -12, 660); pk(PK_SNIPE_AMMO, -4, -12, 640);
  g_exit_lo = v3_(3, -13, 654); g_exit_hi = v3_(10.2f, -4, 686);

  /* notices and the zone beats, in story order: landing, lobby, tower 4, lockers, tram */
  sign(0, -1.6f, 106, DS_PI, SG_A_GONDOLA, 5);
  sign(8, 2.2f, 645, DS_PI, SG_A_TRAM, 5);
  form(-8, -12, 650, 0.6f, FM_NOISE, 0.9f, true); form(2, 0, 20, 1.2f, FM_WINDOW, 0.9f, true);
  zone(0, -22, -3, -20, 22, 12, 44);
  zone(1, -14, -9, 150, 14, 0, 178);
  zone(2, -22, -11, 360, 18, 6, 400);
  zone(3, -14, -13, 456, 6, -2, 482);
  zone(4, -24, -13, 640, 24, -2, 664);
  anchor("landing", g_spawn, 0);
  anchor("brute_arena", v3_(0, -12, 568), 0);
  anchor("exit", v3_(7, -12, 670), 0);
  anchor("vista", v3_(0, -8, 160), 0);
  gate(-4, 4, 533, -12, 5.2f);
  gate(-4, 4, 603, -12, 5.2f);
  g_gate_probe = v3_(0, -12, 514); g_gate_r = 22;
  rings(250, 14);
}

/* ── level 4: The Basement ──
 * The Freehold's undercroft. Filing cabinet canyons, a chicane of unread complaints, a long server
 * hall with sniper sightlines, a pit crossing, a catwalk over the stacks, a Brute pair arena, and
 * the eviction relay. */
/* a wall of giant filing cabinets: one box, the drawers tile across it */
static void cabwall(float x0, float z0, float x1, float z1, float y, float h) {
  solid(v3_(x0, y, z0), v3_(x1, y + h, z1), M_CABINET, 4, true);
  ucube((x0 + x1) / 2, y + h + 0.1f, (z0 + z1) / 2, x1 - x0 + 0.2f, 0.2f, z1 - z0 + 0.2f, g_lip, false);
}
static void cabcover(float cx, float cz, float w, float d, float h, float y) {
  solid(v3_(cx - w / 2, y, cz - d / 2), v3_(cx + w / 2, y + h, cz + d / 2), M_CABINET, 2, true);
  ucube(cx, y + h + 0.06f, cz, w + 0.12f, 0.12f, d + 0.12f, g_lip, false);
}
/* a block of server racks; blinking terminal-green lamps stud the faces */
static void servers(float x0, float z0, float x1, float z1, float y, float h) {
  solid(v3_(x0, y, z0), v3_(x1, y + h, z1), M_SERVER, 3, true);
  bool az = (z1 - z0) >= (x1 - x0);
  float len = az ? z1 - z0 : x1 - x0;
  for (int side = 0; side < 2; side++)
    for (float t = 1.5f; t < len - 1; t += 5.5f) {
      float hh = y + 1.2f + frand_seed((int)(x0 * 3 + z0 * 7 + t * 13)) * (h - 2.4f);
      int m = ((int)(t * 2) & 1) ? M_LED1 : M_LED2;
      if (az) ucube(side ? x1 + 0.04f : x0 - 0.04f, hh, z0 + t, 0.1f, 0.3f, 0.3f, m, false);
      else ucube(x0 + t, hh, side ? z1 + 0.04f : z0 - 0.04f, 0.3f, 0.3f, 0.1f, m, false);
    }
}
static void servcover(float cx, float cz, float w, float d, float h, float y) {
  solid(v3_(cx - w / 2, y, cz - d / 2), v3_(cx + w / 2, y + h, cz + d / 2), M_SERVER, 3, true);
  ucube(cx, y + h + 0.06f, cz, w + 0.12f, 0.12f, d + 0.12f, g_lip, false);
}
/* pneumatic tubes: a run overhead with a carrier lamp, or a riser */
static void tube_run(float x, float y, float z, float len, bool along_z) {
  ucube(x, y, z, along_z ? 1.1f : len, 1.1f, along_z ? len : 1.1f, M_TUBE, false);
  ucube(x, y, z, along_z ? 1.2f : 1.4f, 1.2f, along_z ? 1.4f : 1.2f, M_LED2, false);
}
static void riser(float x, float z, float y0, float y1) { ucube(x, (y0 + y1) / 2, z, 1.1f, y1 - y0, 1.1f, M_TUBE, false); }
/* the eviction relay: a dark machine with a red core behind bars, a switch panel at its foot */
static void relay(float x, float z) {
  col(v3_(x - 9, 0, z - 9), v3_(x + 9, 4, z + 9));
  ucube(x, 2, z, 18, 4, 18, M_TUBE, true);
  ucube(x, 3.7f, z - 9.05f, 18.2f, 0.5f, 0.12f, M_REDG, false);
  col(v3_(x - 5, 4, z - 5), v3_(x + 5, 28, z + 5));
  ucube(x, 16, z, 10, 24, 10, M_TUBE, true);
  for (int i = 0; i < 4; i++) ucube(x, 8 + i * 5.5f, z, 10.5f, 0.5f, 10.5f, M_REDG, false);
  t3_mesh *core = t3_mesh_new(g_sph, g_mat[M_CORE]);
  t3_object_set_position(core, x, 13, z - 6.5f); t3_object_set_scale(core, 8.5f, 8.5f, 8.5f); attach(core);
  for (int i = -3; i <= 3; i++) ucube(x + i * 1.5f, 13, z - 10.9f, 0.3f, 10, 0.3f, M_DARK, false);
  ucube(x, 8.2f, z - 10.9f, 9, 0.3f, 0.3f, M_DARK, false); ucube(x, 17.8f, z - 10.9f, 9, 0.3f, 0.3f, M_DARK, false);
  ucube(x - 3, 40, z, 1.6f, 24, 1.6f, M_TUBE, false); ucube(x + 3, 40, z, 1.6f, 24, 1.6f, M_TUBE, false);
  ucube(x - 12, 6, z - 1, 24, 1.4f, 1.4f, M_TUBE, false); ucube(x + 12, 9, z - 1, 24, 1.4f, 1.4f, M_TUBE, false);
  ubox(v3_(x - 3.5f, 0, z - 11.4f), v3_(x + 3.5f, 1.5f, z - 9.2f), M_PLATE, true);
  ubox(v3_(x - 0.5f, 1.5f, z - 10.8f), v3_(x + 0.5f, 3.2f, z - 10.2f), M_REDG, false);
  ucube(x - 2.2f, 1.7f, z - 10.2f, 1.3f, 0.3f, 0.3f, M_TUBE, false); ucube(x + 2.2f, 1.7f, z - 10.2f, 1.3f, 0.3f, 0.3f, M_REDG, false);
}

static void build_basement(void) {
  g_spawn = v3_(0, 0, 8);
  g_kill_y = -30;
  g_level = 4;
  g_lip = M_REDG; g_band = M_TERM;
  /* the undercroft: a dark ceiling of beams and pipes, and warning-red lamps */
  for (float z = 70; z < 650; z += 24) {
    ucube(0, 24, z, 70, 1.4f, 2, M_DARK, false);
    for (int sd = -1; sd <= 1; sd += 2) ucube(sd * 16.f, 22.6f, z + 6, 3.4f, 0.4f, 1.4f, (int)(z / 24) & 1 ? M_TERM : M_REDG, false);
  }
  for (int sd = -1; sd <= 1; sd += 2) tube_run(sd * 14.f, 20, 360, 600, true);
  ucube(0, -22, 380, 90, 2, 200, M_DARK, false);    /* the sump below the catwalks: nobody lands on it */

  /* B1 landing bay, walled in by cabinets */
  flat(-22, -14, 22, 60, 0, 2.5f, M_DECK);
  cabwall(-28, -16, 28, -14, 0, 14); cabwall(-28, -14, -22, 62, 0, 14); cabwall(22, -14, 28, 62, 0, 14);
  cabcover(-8, 30, 5, 2, 1.4f, 0); cabcover(8, 38, 5, 2, 1.4f, 0); cabcover(0, 50, 4, 2, 1.4f, 0);
  cp(0, 0, 8); g_cp[0].pos = g_spawn;
  for (int i = 0; i < 9; i++) ucube(-16 + (i % 3) * 3.f, 0.5f, 18 + (i / 3) * 3.f, 1.6f, 1.0f, 1.2f, M_WRAP, false);   /* forms in stacks */
  pk(PK_HEALTH, -16, 0, 50); pk(PK_SNIPE_AMMO, 16, 0, 50);
  en(EN_GRUNT, -8, 40, 1); en(EN_GRUNT, 9, 46, 1); en(EN_GRUNT, 0, 56, 1);
  enh(EN_TURRET, -23.4f, 14, 46);

  /* B2 the complaint archive: cabinet rows that make you weave */
  flat(-22, 60, 22, 170, 0, 2.5f, M_DECK);
  cabwall(-28, 62, -22, 172, 0, 14); cabwall(22, 62, 28, 172, 0, 14);
  cabwall(-22, 84, 6, 88, 0, 3.2f); cabwall(-6, 108, 22, 112, 0, 3.2f); cabwall(-22, 132, 6, 136, 0, 3.2f); cabwall(-6, 156, 22, 160, 0, 3.2f);
  for (int i = 0; i < 14; i++) ucube(rr(-18, 18), 0.5f, rr(64, 166), rr(1.4f, 2.2f), 1.0f, rr(1.0f, 1.8f), M_WRAP, false);
  for (int i = 0; i < 22; i++) form(rr(-19, 19), 0, rr(64, 168), rr(0, 6.28f), i % 4 == 3 ? FM_VIEW : i % 4 == 2 ? FM_WINDOW : FM_NOISE, 0.9f, true);
  tube_run(0, 11, 70, 44, false); tube_run(0, 11, 100, 44, false); tube_run(0, 11, 130, 44, false); tube_run(0, 11, 160, 44, false);
  riser(-21, 100, 0, 24); riser(21, 130, 0, 24);
  enh(EN_TURRET, -23.4f, 14, 92); enh(EN_TURRET, 23.4f, 14, 120); enh(EN_TURRET, -23.4f, 14, 150);
  en(EN_GRUNT, -12, 76, 1); en(EN_GRUNT, 10, 80, 1); en(EN_GRUNT, -6, 98, 1); en(EN_GRUNT, 12, 102, 1);
  en(EN_GRUNT, -14, 122, 1); en(EN_GRUNT, 8, 126, 1); en(EN_GRUNT, -10, 146, 1); en(EN_GRUNT, 12, 150, 1);
  en(EN_FLYER, 0, 92, 1); en(EN_FLYER, 0, 140, 1);
  pk(PK_HEALTH, -18, 0, 98); pk(PK_DECOY, 16, 0, 116); pk(PK_SNIPE_AMMO, -16, 0, 140); pk(PK_HEALTH, 14, 0, 164);

  /* B3 the server hall: a long aisle between stacks, alcoves, snipers at both ends */
  flat(-22, 170, 22, 300, 0, 2.5f, M_DECK);
  static const float seg[3][2] = {{170, 210}, {216, 250}, {256, 300}};
  for (int sd = -1; sd <= 1; sd += 2)
    for (int i = 0; i < 3; i++) {
      if (sd < 0) servers(-22, seg[i][0], -8, seg[i][1], 0, 10); else servers(8, seg[i][0], 22, seg[i][1], 0, 10);
    }
  for (int sd = -1; sd <= 1; sd += 2) { cabwall(sd < 0 ? -28.f : 22.f, 210, sd < 0 ? -22.f : 28.f, 216, 0, 10); cabwall(sd < 0 ? -28.f : 22.f, 250, sd < 0 ? -22.f : 28.f, 256, 0, 10); }
  cabwall(-28, 298, -8, 302, 0, 12); cabwall(8, 298, 28, 302, 0, 12);
  cabcover(-3, 196, 3, 2, 1.6f, 0); cabcover(4, 226, 3, 2, 1.6f, 0); cabcover(-4, 246, 3, 2, 1.6f, 0); cabcover(3, 268, 3, 2, 1.6f, 0); cabcover(-3, 284, 3, 2, 1.6f, 0);
  cabcover(0, 294, 6, 2, 1.6f, 0);
  cp(-2, 0, 288);
  enh(EN_TURRET, 9, 10, 186); enh(EN_TURRET, -9, 10, 230); enh(EN_TURRET, 9, 10, 268); enh(EN_TURRET, 0, 0, 296); enh(EN_TURRET, -9, 10, 282);
  en(EN_GRUNT, 0, 190, 1); en(EN_GRUNT, -4, 210, 1); en(EN_GRUNT, 4, 240, 1); en(EN_GRUNT, 0, 262, 1); en(EN_GRUNT, -3, 276, 1); en(EN_GRUNT, 5, 284, 1);
  en(EN_FLYER, 0, 250, 1);
  pk(PK_HEALTH, -16, 0, 213); pk(PK_SNIPE_AMMO, 16, 0, 213); pk(PK_DECOY, -15, 0, 253); pk(PK_SHIELD, 15, 0, 253);
  tube_run(0, 14, 190, 16, false); tube_run(0, 14, 240, 16, false); tube_run(0, 14, 280, 16, false);

  /* B4 the pit: a pneumatic lift across, then up to a catwalk over the stacks */
  g_platmat = M_TUBE;
  plat(-5, 307.5f, 5, 316.5f, 0, 1.0f, v3_(0, 0, 6), 9.f, 0, false);
  g_platmat = M_PLATE;
  ubox(v3_(-7, 0.4f, 300), v3_(-5.8f, 1.2f, 324), M_REDG, false); ubox(v3_(5.8f, 0.4f, 300), v3_(7, 1.2f, 324), M_REDG, false);
  flat(-14, 324, 14, 352, 0, 2.5f, M_DECK);
  cabwall(-20, 324, -14, 352, 0, 14); cabwall(14, 324, 20, 352, 0, 14);
  cp(-2, 0, 334);
  stairs(0, 0, 352, 0, 10, 20, 0.4f, 0.9f);
  flat(-5, 370, 5, 392, 8, 0.8f, M_PLATE);
  flat(-5, 400, 5, 424, 8, 0.8f, M_PLATE);
  stairs(0, 0, 442, 1, 10, 20, 0.4f, 0.9f);
  for (int sd = -1; sd <= 1; sd += 2) {
    ucube(sd * 5.f, 8.9f, 381, 0.12f, 0.12f, 22, M_REDG, false); ucube(sd * 5.f, 8.9f, 412, 0.12f, 0.12f, 24, M_REDG, false);
  }
  for (int i = 0; i < 3; i++) {
    float z = 380 + i * 14.f, x = (i & 1 ? 12.f : -12.f);
    col(v3_(x - 2, -30, z - 2), v3_(x + 2, 12, z + 2));
    ubox(v3_(x - 2, -30, z - 2), v3_(x + 2, 12, z + 2), M_DARK, true);
    ucube(x, 12.2f, z, 4.4f, 0.4f, 4.4f, M_REDG, false);
    enh(EN_TURRET, x, 12, z);
  }
  /* stacks of cabinets down in the sump, for scale */
  for (int i = 0; i < 12; i++) ubox(v3_(-30 + (i % 4) * 16.f, -22, 364 + (i / 4) * 24.f), v3_(-22 + (i % 4) * 16.f, -22 + 8 + (i % 3) * 4.f, 372 + (i / 4) * 24.f), M_DARK, false);
  en(EN_GRUNT, 0, 380, 8); en(EN_GRUNT, 0, 412, 8);
  enh(EN_FLYER, 0, 14, 396); enh(EN_FLYER, 0, 13, 340);
  pk(PK_HEALTH, 0, 8, 374); pk(PK_SNIPE_AMMO, 0, 8, 420); pk(PK_SHIELD, 0, 0, 346);

  /* B5 the Brute pair arena */
  flat(-12, 442, 12, 450, 0, 2.5f, M_DECK);
  flat(-26, 450, 26, 522, 0, 2.5f, M_DECK);
  cp(-2, 0, 444);
  g_arena_lo = v3_(-26, -3, 452); g_arena_hi = v3_(26, 24, 520);
  cabwall(-26, 450, -8, 452, 0, 6); cabwall(8, 450, 26, 452, 0, 6); cabwall(-26, 520, -8, 522, 0, 6); cabwall(8, 520, 26, 522, 0, 6);
  cabwall(-32, 450, -26, 522, 0, 8); cabwall(26, 450, 32, 522, 0, 8);
  servcover(-11, 470, 5, 5, 3.6f, 0); servcover(11, 470, 5, 5, 3.6f, 0); servcover(-11, 500, 5, 5, 3.6f, 0); servcover(11, 500, 5, 5, 3.6f, 0);
  servcover(0, 485, 3, 3, 1.4f, 0);
  enh(EN_TURRET, -29, 8, 470); enh(EN_TURRET, 29, 8, 490);
  en(EN_BRUTE, -10, 504, 0); en(EN_BRUTE, 10, 504, 0); en(EN_GRUNT, 0, 466, 1); en(EN_GRUNT, -16, 488, 1);
  pk(PK_HEALTH, -22, 0, 458); pk(PK_HEALTH, 22, 0, 458); pk(PK_SNIPE_AMMO, 0, 0, 456); pk(PK_SHIELD, 0, 0, 514);

  /* B6 the relay chamber: the exit is the switch at the relay's foot */
  flat(-30, 522, 30, 640, 0, 2.5f, M_DECK);
  servers(-36, 522, -30, 640, 0, 14); servers(30, 522, 36, 640, 0, 14);
  cabwall(-36, 640, 36, 644, 0, 18);
  relay(0, 616);
  cabcover(-14, 545, 5, 2, 1.6f, 0); servcover(14, 552, 4, 4, 2.2f, 0); cabcover(-10, 572, 4, 2, 1.6f, 0); servcover(12, 582, 5, 3, 1.6f, 0);
  cp(-2, 0, 528);
  enh(EN_TURRET, -31.5f, 14, 546); enh(EN_TURRET, 31.5f, 14, 566); enh(EN_TURRET, -12, 0, 606); enh(EN_TURRET, 12, 0, 606);
  en(EN_GRUNT, -10, 534, 1); en(EN_GRUNT, 10, 542, 1); en(EN_GRUNT, 0, 560, 1); en(EN_GRUNT, -14, 580, 1); en(EN_GRUNT, 14, 586, 1);
  en(EN_FLYER, -6, 556, 1); en(EN_FLYER, 8, 576, 1);
  pk(PK_HEALTH, -26, 0, 530); pk(PK_HEALTH, 26, 0, 530); pk(PK_SNIPE_AMMO, 0, 0, 598); pk(PK_DECOY, -26, 0, 590); pk(PK_SHIELD, 26, 0, 590);
  g_exit_lo = v3_(-8, -1, 596); g_exit_hi = v3_(8, 6, 607);

  /* notices: the Superintendent's rules on the PA, receipts, and the relay's own status board */
  sign(0, 3.4f, -10, DS_PI, SG_B_STAFF, 6);
  sign(0, 5.2f, 66, DS_PI, SG_B_ARCH, 7);
  sign(-14, 3, 98, DS_PI, SG_B_RECV, 5); sign(12, 3, 146, DS_PI, SG_B_RECV, 5);
  sign(0, 6, 176, DS_PI, SG_B_RULE, 7);
  sign(0, 6, 256, DS_PI, SG_B_RULE2, 7);
  sign(0, 5, 305, DS_PI, SG_B_TUBE, 5);
  sign(-7, 3.4f, 334, DS_PI, SG_B_AISLE, 5);
  sign(0, 3.4f, 440, DS_PI, SG_B_STAFF, 5);
  sign(0, 5, 540, DS_PI, SG_B_RELAY, 7);
  sign(0, 5, 592, DS_PI, SG_B_ARMED, 6);
  sign(0, 5, 520, DS_PI, SG_B_RULE, 6);
  zone(0, -22, -3, -14, 22, 12, 46);
  zone(1, -22, -3, 88, 22, 8, 132);
  zone(2, -8, -3, 172, 8, 8, 200);
  zone(3, -30, -3, 524, 30, 10, 570);
  zone(4, -10, -3, 578, 10, 10, 598);
  anchor("landing", g_spawn, 0);
  anchor("brute_arena", v3_(0, 0, 486), 0);
  anchor("exit", v3_(0, 0, 600), 0);
  anchor("vista", v3_(0, 0, 616), 0);
  gate(-8, 8, 451, 0, 5.2f);
  gate(-8, 8, 521, 0, 5.2f);
  g_gate_probe = v3_(0, 0, 440); g_gate_r = 20;
  rings(150, 10);
}

/* ── API ── */
static const char *g_names[WORLD_LEVELS] = {"CURTAIN WALL", "HYDROPONIC SPINE", "COLD STORAGE", "THE ANNEX", "THE BASEMENT", "THE SUPERINTENDENT"};
static const char *g_blurb[WORLD_LEVELS] = {
  "Tenants have been reminded that nesting is not a permitted use.",
  "Please keep off the plants. The plants have asked nicely.",
  "Two centuries of furniture, held in case the owners arrive.",
  "Morning on the ring. Management has relocated the Tenants onto you.",
  "Every complaint ever filed is down here. None have been read.",
  "Management will see you now. Management has rescheduled you."};
const char *world_level_name(int level) { return g_names[(unsigned)level % WORLD_LEVELS]; }
const char *world_level_blurb(int level) { return g_blurb[(unsigned)level % WORLD_LEVELS]; }

#ifdef WORLD_STATS
static void stat_cb(t3_object *o, void *ctx) {
  struct { void **g, **m; int *n, *nm; } *c = ctx;
  if (o->type != T3_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  (*c->nm)++;
  for (int i = 0; i < *c->n; i++) if (c->g[i] == m->geometry && c->m[i] == m->materials[0]) return;
  if (*c->n < 4096) { c->g[*c->n] = m->geometry; c->m[*c->n] = m->materials[0]; (*c->n)++; }
}
#endif
void world_load(int level) {
  if (g_root) { t3_object_remove(&g_scene->base, g_root); t3_release(g_root); g_root = NULL; }
  level = (int)((unsigned)level % WORLD_LEVELS);
  g_nb = g_np = g_npk = g_ncp = g_nsp = g_nring = g_nspin = 0; g_cp_at = 0; g_time = 0;
  g_nzone = g_nanc = g_ngate = 0; g_gates_closed = false; g_gate_r = 0;
  g_rng = 12345 + level * 977;
  g_cur = NULL;
  g_root = t3_object_new();
  t3_object_add(&g_scene->base, g_root);
  g_arena_lo = g_arena_hi = g_exit_lo = g_exit_hi = v3_(0, 0, 0);
  /* per ship: fog (also the sky), hemisphere sky/ground, sun, and a tint over the shared hull textures */
  static const uint32_t fogc[WORLD_LEVELS] = {0x9aa4b0, 0x6fae98, 0xa6c6e0, 0xf2d8a4, 0x0c1612, 0x2e2658};
  static const float fogd[WORLD_LEVELS] = {0.0040f, 0.0044f, 0.0050f, 0.0026f, 0.0068f, 0.0036f};
  static const uint32_t hsky[WORLD_LEVELS] = {0xe6e0d4, 0xd2f2dc, 0xdcefff, 0xfff2d4, 0x6a8a78, 0x9a8ee0},
                        hgnd[WORLD_LEVELS] = {0x5a4326, 0x5a3050, 0x3a587a, 0x7a6a4a, 0x1a2420, 0x2a1a44};
  static const float hint[WORLD_LEVELS] = {1.1f, 1.1f, 1.4f, 1.2f, 1.5f, 0.9f};
  static const uint32_t sunc[WORLD_LEVELS] = {0xffc890, 0xffe0e4, 0xd8ecff, 0xffd890, 0xb0e0c4, 0xb8c4ff};
  static const float suni[WORLD_LEVELS] = {2.3f, 2.0f, 2.0f, 2.4f, 1.7f, 1.9f};
  static const uint32_t tint[WORLD_LEVELS] = {0xfff0e0, 0xd8f4d8, 0xcfe6ff, 0xfff0c8, 0x9fb0a6, 0xb8aee8};
  static const uint32_t farh[WORLD_LEVELS] = {0x5a7494, 0x4f7a68, 0x6a88a8, 0x8a9ab0, 0x1a2a24, 0x2b2652};
  static const uint32_t lite[WORLD_LEVELS] = {0xffd8a8, 0xa8d8c0, 0xcfe4ff, 0xffe0a0, 0x7fe0a0, 0x9a8cd0};
  static const uint32_t halo[WORLD_LEVELS] = {0x7fb8ff, 0x8fe8c8, 0xa8d4ff, 0xffd890, 0x3a6a50, 0x9a7bff};
  g_scene->background = t3_color_hex(fogc[level]); g_scene->has_background = true;
  g_scene->fog = t3_fog_exp2(fogc[level], fogd[level]);
  for (int i = 0; i < g_scene->base.child_count; i++) {
    t3_object *o = g_scene->base.children[i];
    if (o->type == T3_HEMISPHERE_LIGHT) { t3_light *l = (t3_light *)o; l->color = t3_color_hex(hsky[level]); l->ground_color = t3_color_hex(hgnd[level]); l->intensity = hint[level]; }
    else if (o->type == T3_DIRECTIONAL_LIGHT) { t3_light *l = (t3_light *)o; l->color = t3_color_hex(sunc[level]); l->intensity = suni[level]; }
  }
  static const int tinted[] = {M_DECK, M_HULL, M_PLATE, M_ARENA, M_HAZ};
  for (unsigned i = 0; i < sizeof tinted / sizeof *tinted; i++) { g_mat[tinted[i]]->color = t3_color_hex(tint[level]); g_mat[tinted[i]]->version++; }
  g_mat[M_FARHULL]->color = t3_color_hex(farh[level]);
  g_mat[M_LIGHTS]->color = t3_color_hex(lite[level]);
  g_mat[M_HALO]->color = t3_color_hex(halo[level]);
  /* the colony's far towers pick up the Annex's morning gold, and the ring colour stays pale elsewhere */
  g_mat[M_TOWER]->color = g_mat[M_TOWERGAP]->color = t3_color_hex(level == 3 ? 0xffe8b8 : 0xffffff);
  g_lip = M_ORANGE; g_band = M_TEALG; g_platmat = M_PLATE;
  g_level = level;
  if (level == 0) build_l0(); else if (level == 1) build_l1(); else if (level == 2) build_cold();
  else if (level == 3) build_annex(); else if (level == 4) build_basement(); else build_l5();
  g_cp[0].hit = true;
  if (g_ncp) { t3_mesh *m = (t3_mesh *)g_cp[0].flag; t3_release(m->materials[0]); m->materials[0] = t3_retain(g_mat[M_TEALG]); }
#ifdef WORLD_STATS
  { void *pg[4096]; void *pm[4096]; int np = 0, nmesh = 0;
    t3_object_traverse(g_root, stat_cb, &(struct { void **g, **m; int *n, *nm; }){pg, pm, &np, &nmesh});
    ds_log("world: %d meshes, %d distinct geometry+material pairs", nmesh, np); }
#endif
  ds_log("world: level %d boxes %d plats %d pickups %d spawns %d", level, g_nb, g_np, g_npk, g_nsp);
}

v3 world_drop_start(void) { return g_start; }
v3 world_spawn(void) { return g_spawn; }
float world_kill_y(void) { return g_kill_y; }

static void cp_check(v3 p) {
  for (int i = g_cp_at + 1; i < g_ncp; i++) {
    float dx = p.x - g_cp[i].pos.x, dz = p.z - g_cp[i].pos.z;
    if (dx * dx + dz * dz < 14 * 14 && fabsf(p.y - g_cp[i].pos.y) < 8) {
      g_cp_at = i; g_cp[i].hit = true;
      if (g_cp[i].flag) {
        t3_mesh *m = (t3_mesh *)g_cp[i].flag;
        t3_release(m->materials[0]); m->materials[0] = t3_retain(g_mat[M_TEALG]);
      }
    }
  }
}
v3 world_checkpoint(v3 p) { cp_check(p); return g_ncp ? g_cp[g_cp_at].pos : g_spawn; }

static bool inside(v3 p, v3 lo, v3 hi) { return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z; }
bool world_at_exit(v3 p) { return inside(p, g_exit_lo, g_exit_hi); }
bool world_in_boss_arena(v3 p) { return inside(p, g_arena_lo, g_arena_hi); }

/* ── collision ── */
static bool circ_box(float px, float pz, float r, const box_t *b, float *nx, float *nz, float *pen) {
  float cx = ds_clamp(px, b->lo.x, b->hi.x), cz = ds_clamp(pz, b->lo.z, b->hi.z);
  float dx = px - cx, dz = pz - cz, d2 = dx * dx + dz * dz;
  if (d2 >= r * r) return false;
  if (d2 > 1e-8f) { float d = sqrtf(d2); *nx = dx / d; *nz = dz / d; *pen = r - d; return true; }
  float l = px - b->lo.x, rt = b->hi.x - px, f = pz - b->lo.z, k = b->hi.z - pz, m = fminf(fminf(l, rt), fminf(f, k));
  *nx = *nz = 0;
  if (m == l) *nx = -1; else if (m == rt) *nx = 1; else if (m == f) *nz = -1; else *nz = 1;
  *pen = m + r;
  return true;
}
static bool blocked_at(v3 p, float r, float h, int skip) {
  for (int i = 0; i < g_nb; i++) {
    const box_t *b = &g_box[i];
    float nx, nz, pen;
    if (i == skip || p.y >= b->hi.y - 1e-3f || p.y + h <= b->lo.y + 1e-3f) continue;
    if (circ_box(p.x, p.z, r, b, &nx, &nz, &pen)) return true;
  }
  return false;
}
/* the box a cylinder with feet at p stands on (top within tol of the feet), or -1 */
static int support(v3 p, float r, float tol) {
  int best = -1; float by = -1e9f;
  for (int i = 0; i < g_nb; i++) {
    const box_t *b = &g_box[i];
    float nx, nz, pen;
    if (b->hi.y > p.y + tol || b->hi.y < p.y - tol || b->hi.y <= by) continue;
    if (circ_box(p.x, p.z, r * 0.92f, b, &nx, &nz, &pen)) { best = i; by = b->hi.y; }
  }
  return best;
}

void world_move(v3 *p, v3 *vel, float r, float h, float dt, bool *grounded) {
  bool was = *grounded;
  if (was) {
    /* the platform moved by delta since the last tick: carry whoever stood on it at its old place. Every mover
     * (Juno, each enemy) is carried once per tick as it stands on the old box; a once-per-tick guard here let the
     * first enemy to move eat the carry, so riders slid off whenever any enemy was awake */
    for (int i = 0; i < g_np; i++) {
      const plat_t *pl = &g_plat[i];
      box_t ob = g_box[pl->bi];
      ob.lo = v3_sub(ob.lo, pl->delta); ob.hi = v3_sub(ob.hi, pl->delta);
      float nx, nz, pen;
      if (fabsf(ob.hi.y - p->y) < 0.1f && circ_box(p->x, p->z, r * 0.92f, &ob, &nx, &nz, &pen)) { *p = v3_add(*p, pl->delta); break; }
    }
  }
  float sp = fmaxf(fmaxf(fabsf(vel->x), fabsf(vel->z)), fabsf(vel->y)) * dt;
  int n = (int)ceilf(sp / (r * 0.6f)); if (n < 1) n = 1; if (n > 20) n = 20;
  float ds = dt / n;
  bool gr = false;
  for (int it = 0; it < n; it++) {
    /* vertical */
    float oldy = p->y;
    p->y += vel->y * ds;
    for (int i = 0; i < g_nb; i++) {
      const box_t *b = &g_box[i];
      float nx, nz, pen;
      if (p->y >= b->hi.y || p->y + h <= b->lo.y) {
        if (!(vel->y <= 0 && oldy >= b->hi.y - 0.06f && p->y < b->hi.y + 1e-4f && p->y + h > b->lo.y) &&
            !(vel->y > 0 && oldy + h <= b->lo.y + 0.06f && p->y + h > b->lo.y)) continue;
      }
      if (!circ_box(p->x, p->z, r * 0.92f, b, &nx, &nz, &pen)) continue;
      if (vel->y <= 0 && oldy >= b->hi.y - 0.06f) { p->y = b->hi.y; if (vel->y < 0) vel->y = 0; gr = true; }
      else if (vel->y > 0 && oldy + h <= b->lo.y + 0.06f) { p->y = b->lo.y - h; vel->y = 0; }
    }
    /* horizontal, with step-up */
    p->x += vel->x * ds; p->z += vel->z * ds;
    for (int pass = 0; pass < 3; pass++) {
      bool hit = false;
      for (int i = 0; i < g_nb; i++) {
        const box_t *b = &g_box[i];
        float nx, nz, pen;
        if (p->y >= b->hi.y - 1e-3f || p->y + h <= b->lo.y + 1e-3f) continue;
        if (!circ_box(p->x, p->z, r, b, &nx, &nz, &pen)) continue;
        if (b->hi.y - p->y <= STEP_UP && vel->y <= 1.f) {
          v3 q = v3_(p->x, b->hi.y, p->z);
          if (!blocked_at(q, r, h, i)) { p->y = b->hi.y; if (vel->y < 0) vel->y = 0; gr = true; continue; }
        }
        p->x += nx * pen; p->z += nz * pen;
        float vd = vel->x * nx + vel->z * nz;
        if (vd < 0) { vel->x -= vd * nx; vel->z -= vd * nz; }
        hit = true;
      }
      if (!hit) break;
    }
    if (!gr && vel->y <= 0 && support(*p, r, 0.03f) >= 0) gr = true;
  }
  /* walking off a stair or small ledge keeps the feet down */
  if (!gr && was && vel->y <= 0) {
    for (int i = 0; i < g_nb; i++) {
      const box_t *b = &g_box[i];
      float nx, nz, pen;
      if (b->hi.y > p->y || b->hi.y < p->y - 0.4f) continue;
      if (circ_box(p->x, p->z, r * 0.92f, b, &nx, &nz, &pen) && !blocked_at(v3_(p->x, b->hi.y, p->z), r, h, i)) { p->y = b->hi.y; vel->y = 0; gr = true; break; }
    }
  }
  *grounded = gr;
}

bool world_raycast(v3 o, v3 d, float max_dist, float *t, v3 *normal) {
  float best = max_dist; bool found = false; v3 bn = v3_(0, 1, 0);
  float inv[3] = {fabsf(d.x) > 1e-9f ? 1 / d.x : 1e9f, fabsf(d.y) > 1e-9f ? 1 / d.y : 1e9f, fabsf(d.z) > 1e-9f ? 1 / d.z : 1e9f};
  float oo[3] = {o.x, o.y, o.z};
  for (int i = 0; i < g_nb; i++) {
    const box_t *b = &g_box[i];
    float lo[3] = {b->lo.x, b->lo.y, b->lo.z}, hi[3] = {b->hi.x, b->hi.y, b->hi.z};
    float t0 = 0, t1 = best; int ax = -1; float sg = 0;
    bool ok = true;
    for (int a = 0; a < 3; a++) {
      float ta = (lo[a] - oo[a]) * inv[a], tb = (hi[a] - oo[a]) * inv[a], s = -1;
      if (ta > tb) { float tmp = ta; ta = tb; tb = tmp; s = 1; }
      if (ta > t0) { t0 = ta; ax = a; sg = s; }
      if (tb < t1) t1 = tb;
      if (t0 > t1) { ok = false; break; }
    }
    if (!ok) continue;
    if (ax < 0) continue; /* started inside */
    best = t0; found = true;
    bn = v3_(0, 0, 0);
    if (ax == 0) bn.x = sg; else if (ax == 1) bn.y = sg; else bn.z = sg;
  }
  if (found) { *t = best; if (normal) *normal = bn; }
  return found;
}

float world_floor_y(v3 p) {
  float best = -1e9f;
  for (int i = 0; i < g_nb; i++) {
    const box_t *b = &g_box[i];
    if (p.x >= b->lo.x && p.x <= b->hi.x && p.z >= b->lo.z && p.z <= b->hi.z && b->hi.y <= p.y + 0.5f && b->hi.y >= p.y - 50 && b->hi.y > best) best = b->hi.y;
  }
  return best;
}

int world_take_pickup(v3 p, float r) {
  for (int i = 0; i < g_npk; i++) {
    pick_t *k = &g_pk[i];
    if (k->taken) continue;
    float dx = p.x - k->pos.x, dz = p.z - k->pos.z, dy = p.y + 0.9f - k->pos.y;
    if (dx * dx + dz * dz < (r + 0.6f) * (r + 0.6f) && fabsf(dy) < 1.8f) {
      k->taken = true; T3_OBJ(k->vis)->visible = false;
      return k->kind;
    }
  }
  return -1;
}
int world_spawns(const spawn_t **out) { *out = g_sp; return g_nsp; }
int world_rings(const v3 **pos, float *radius) { *pos = g_ring; *radius = 6.5f; return g_nring; }
bool world_ring_collect(int i) {
  if (i < 0 || i >= g_nring || g_ring_got[i]) return false;
  g_ring_got[i] = true; T3_OBJ(g_ring_vis[i])->visible = false;
  return true;
}

void world_update(float dt, v3 player) {
  g_time += dt;
  for (int i = 0; i < g_np; i++) {
    plat_t *p = &g_plat[i];
    float s = sinf(g_time * 2 * DS_PI / p->period + p->phase);
    v3 off = v3_scale(p->axis, s);
    p->delta = v3_sub(off, p->off);
    p->off = off;
    box_t *b = &g_box[p->bi];
    b->lo = v3_add(p->lo, off); b->hi = v3_add(p->hi, off);
    t3_object_set_position(p->vis, off.x, off.y, off.z);
  }
  /* lamps that breathe: the heater cores, the servers' terminal green, the relay's red core */
  if (g_level == 2 || g_level == 4) {
    float a = 0.82f + 0.18f * sinf(g_time * 1.6f), b = 0.55f + 0.45f * fmaxf(0, sinf(g_time * 5.1f)), c = 0.5f + 0.5f * fmaxf(0, sinf(g_time * 3.3f + 2));
    if (g_level == 2) { g_mat[M_AMBER]->color = t3_color_hex(((int)(0xff * a) << 16) | ((int)(0xb0 * a) << 8) | (int)(0x30 * a)); g_mat[M_AMBER]->version++; }
    else {
      g_mat[M_LED1]->color = t3_color_hex(((int)(0x3d * b) << 16) | ((int)(0xff * b) << 8) | (int)(0x7a * b)); g_mat[M_LED1]->version++;
      g_mat[M_LED2]->color = t3_color_hex(((int)(0x3d * c) << 16) | ((int)(0xff * c) << 8) | (int)(0x7a * c)); g_mat[M_LED2]->version++;
      float k = 0.7f + 0.3f * sinf(g_time * 2.4f);
      g_mat[M_CORE]->color = t3_color_hex(((int)(0xff * k) << 16) | ((int)(0x2a * k) << 8) | (int)(0x22 * k)); g_mat[M_CORE]->version++;
    }
  }
  for (int i = 0; i < g_nspin; i++) {
    t3_object *o = g_spin[i].o;
    t3_object_set_rotation(o, o->rotation.x + g_spin[i].ax.x * g_spin[i].rate * dt, o->rotation.y + g_spin[i].ax.y * g_spin[i].rate * dt, o->rotation.z + g_spin[i].ax.z * g_spin[i].rate * dt);
  }
  for (int i = 0; i < g_npk; i++) {
    pick_t *k = &g_pk[i];
    if (k->taken) continue;
    t3_object_set_position(k->vis, k->pos.x, k->pos.y + sinf(g_time * 2.2f + k->ph) * 0.18f, k->pos.z);
    t3_object_set_rotation(k->vis, 0, g_time * 1.6f + k->ph, 0);
  }
  for (int i = 0; i < g_nring; i++) {
    if (g_ring_got[i]) continue;
    float s = 1 + sinf(g_time * 3 + i) * 0.04f;
    t3_object_set_scale(g_ring_vis[i], s, s, s);
  }
  for (int i = 0; i < g_ngate; i++) {
    gate_t *g = &g_gate[i];
    g->open = ds_clamp(g->open + (g_gates_closed ? -1.f : 1.f) * dt * 1.6f, 0, 1);
    t3_object_set_position(g->vis, 0, g->open * g->h, 0);
    g->vis->visible = g->open < 0.995f;
    if (g->open < 0.6f) { g_box[g->bi].lo = g->lo; g_box[g->bi].hi = g->hi; }
    else g_box[g->bi].lo = g_box[g->bi].hi = v3_(0, -9999, 0);
  }
  cp_check(player);
}

/* ── story hooks ── */
const char *world_ship_name(int level) {
  static const char *n[WORLD_LEVELS] = {"THE LESSEE", "THE SUBLET", "THE LESSEE", "HALCYON REACH", "THE FREEHOLD", "THE FREEHOLD"};
  return n[(unsigned)level % WORLD_LEVELS];
}
bool world_anchor(const char *name, v3 *pos, float *yaw) {
  for (int i = 0; i < g_nanc; i++)
    if (!strcmp(g_anc[i].name, name)) { *pos = g_anc[i].pos; *yaw = g_anc[i].yaw; return true; }
  *pos = g_spawn; *yaw = 0;
  return false;
}
int world_story_trigger(v3 p) {
  for (int i = 0; i < g_nzone; i++) {
    zone_t *z = &g_zone[i];
    if (!z->fired && inside(p, z->lo, z->hi)) {
      z->fired = true;
      return z->beat;
    }
  }
  return -1;
}
void world_set_gates(bool closed) {
  /* never shut the doors on a chasing enemy: it would be locked out and unkillable */
  if (closed && !g_gates_closed && g_gate_r > 0 && enemy_alive_near(g_gate_probe, g_gate_r, -1) > 0) closed = false;
  g_gates_closed = closed;
}
