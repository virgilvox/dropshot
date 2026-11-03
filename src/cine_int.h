/* cine_int.h: what cutscene.c (the director and the script) and cine_set.c (the
 * sets, the props and the staged actors) share. */
#ifndef DS_CINE_INT_H
#define DS_CINE_INT_H
#include "ds.h"
#include "three.h"
#include "cv.h"

/* ── the kit (cine_set.c) ── */
t3_material *km_basic(uint32_t col, bool fog);
t3_material *km_std(uint32_t col, float rough, float metal, uint32_t emis, float ei);
t3_material *km_tex(t3_texture *tex, bool basic, bool fog);
t3_mesh *k_box(t3_object *par, t3_material *m, float x, float y, float z, float w, float h, float d);
t3_mesh *k_cyl(t3_object *par, t3_material *m, float x, float y, float z, float rtop, float rbot, float h, int seg);
t3_mesh *k_sph(t3_object *par, t3_material *m, float x, float y, float z, float r);
/* a plane facing +z; uv window u0,v0..u1,v1 (v up) */
t3_mesh *k_plane(t3_object *par, t3_material *m, float x, float y, float z, float w, float h, float u0, float v0, float u1, float v1);
t3_object *k_group(t3_object *par, float x, float y, float z);
/* a model from assets/models scaled so its height is `height`, feet at y, centred on x/z */
t3_object *k_model(t3_object *par, const char *file, float height, float x, float y, float z, float yaw);
t3_texture *k_paint(int w, int h, void (*fn)(cv *, int, int), bool repeat);
void k_look(t3_object *o, v3 from, v3 to);

/* ── staged actors: a skinned GLB with a mixer, posed by name and time ── */
typedef struct {
  t3_gltf *gltf;
  t3_object *root, *model;
  t3_animation_mixer *mixer;
  t3_animation_action *cur, *prev;
  float fade, scale;
  char name[24];
  bool live;
} act_t;
bool act_load(act_t *a, t3_object *parent, const char *file, float scale);
/* play a clip (looping); same clip again is a no-op. Crossfades from the last. */
void act_play(act_t *a, const char *clip, float speed);
/* hold a clip at t (0..1 of its length), no playing: kneel, unfold, power down */
void act_pose(act_t *a, const char *clip, float t01);
float act_len(act_t *a, const char *clip);
void act_tick(act_t *a, float dt);
void act_put(act_t *a, v3 p, float yaw, float scale_mul, bool visible);
void act_tint(act_t *a, uint32_t col, uint32_t emis, float ei);

/* ── the sets ── */
typedef struct {
  v3 origin;
  t3_object *root, *dome, *planet;
  t3_object *gondola, *panel, *hole, *halo;
  t3_object *strip[8];
  t3_object *ship[3];
  t3_object *window_back;     /* the window carried back in the ending */
  t3_object *form;            /* the Tenants' thank-you form */
  t3_object *squeegee;
} halc_t;
typedef struct {
  v3 origin;
  t3_object *root, *dome, *lamps, *freehold;
  t3_object *nests;
} subl_t;
typedef struct {
  v3 origin;
  t3_object *root, *dome, *arena;
  t3_object *crane[8];        /* pivots at the base of each boom */
  t3_object *jib[8];          /* the second joint of each crane */
  t3_object *lamp[8];
  t3_object *furn[10];        /* the furniture it is assembling itself from */
  t3_object *beam;            /* the lighthouse beam of the ending */
  t3_object *lease;           /* the new lease */
} free_t;
typedef struct {
  t3_object *root, *plate[2], *lamp;   /* the maintenance bay: a service hatch in the floor */
} bay_t;
typedef struct {
  v3 origin;
  t3_object *root, *chute, *lamp;
  t3_object *label[2];        /* the two delivery labels, century old */
  t3_object *banner;          /* "in case they arrive" */
  t3_object *heater;          /* the amber glow at the far end */
} cold_t;
typedef struct {
  v3 origin;
  t3_object *root, *dome, *planet;
  t3_object *gondola, *bucket;  /* Juno's own gondola, and the bucket one small Tenant is wearing */
  t3_object *pane;              /* a window carried in a huddle */
  t3_object *turret;            /* bolted to tower 4 */
  t3_object *ship[3];
} annx_t;
typedef struct {
  v3 origin;
  t3_object *root;
  t3_object *stamp;           /* the arm of the stamping machine */
  t3_object *form[6];         /* forms riding the belt, then stamped */
  t3_object *cap[10];         /* capsules shuttling in the pneumatic tubes */
  t3_object *relay;           /* the eviction relay at the end of the aisle */
  t3_object *relay_ring;
} base_t;
extern bay_t BAY;
extern cold_t CO;
extern annx_t AN;
extern base_t BS;
extern halc_t HA;
extern subl_t SU;
extern free_t FR;
void sets_init(t3_scene *scene);

#endif
