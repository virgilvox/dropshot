/* fx.h: particles, tracers, flashes and explosions in the 3D scene. */
#ifndef DS_FX_H
#define DS_FX_H
#include "ds.h"
#include "three.h"

void fx_init(t3_scene *scene);
void fx_clear(void);
void fx_update(float dt, const t3_camera *cam);
void fx_tracer(v3 from, v3 to, uint32_t color, float width);
void fx_muzzle(v3 at, v3 dir);
void fx_sparks(v3 at, v3 normal, uint32_t color, int count);
void fx_goo(v3 at, int count);                       /* Tenant hit */
void fx_spores(v3 at, uint32_t color, int count);    /* a Tenant is hit: a soft puff of dust motes, no gore */
void fx_poof(v3 at, uint32_t color, float size);     /* a Tenant going home: a round puff and a few rising motes */
void fx_form(v3 at);                                 /* a complaint form flutters down from at and lies there a few seconds */
void fx_explosion(v3 at, float size);
void fx_ring_pop(v3 at, float radius);               /* a drop ring collected */
void fx_glide_trail(v3 left_tip, v3 right_tip);      /* called every frame while gliding */

/* one-frame draws, call every frame while the thing exists (telegraphs, lasers) */
void fx_beam(v3 from, v3 to, uint32_t color, float width);
void fx_glow(v3 at, float size, uint32_t color);
void fx_ring_flat(v3 at, float radius, uint32_t color);  /* flat on the ground */

#endif
