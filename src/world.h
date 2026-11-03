/* world.h: the levels. Geometry built from the asset kits, collision against
 * it, pickups, enemy spawn points, checkpoints and the exit. */
#ifndef DS_WORLD_H
#define DS_WORLD_H
#include "ds.h"
#include "three.h"

#define WORLD_LEVELS 6

typedef enum { PK_HEALTH, PK_SNIPE_AMMO, PK_DECOY, PK_SHIELD, PK_COUNT } pickup_t;

/* an enemy to place when the level loads (enemy.h enemy_type) */
typedef struct { int type; v3 pos; float yaw; } spawn_t;

void world_init(t3_scene *scene);         /* once: load the shared models */
void world_load(int level);               /* replace the current level's geometry in the scene */
const char *world_level_name(int level);  /* "CURTAIN WALL" */
const char *world_level_blurb(int level); /* one deadpan line for the intro card */

v3 world_drop_start(void);                /* where the orbital drop begins (high up) */
v3 world_spawn(void);                     /* the landing point (the first checkpoint) */
v3 world_checkpoint(v3 p);                /* the latest checkpoint the player has passed near p */
float world_kill_y(void);                 /* below this the player has fallen off */
bool world_at_exit(v3 p);                 /* inside the level's exit (or the boss arena gate) */
bool world_in_boss_arena(v3 p);

/* Move a standing cylinder (feet at p->y, radius r, height h) by vel*dt
 * against the level. Slides along walls, lands on floors (sets *grounded and
 * zeroes vel->y), bumps heads on ceilings, steps up ledges below 0.45 m.
 * Moving platforms carry a grounded player. */
void world_move(v3 *p, v3 *vel, float r, float h, float dt, bool *grounded);
/* first hit of a ray against the level geometry; *t is the distance */
bool world_raycast(v3 origin, v3 dir, float max_dist, float *t, v3 *normal);
/* height of the floor under p (within 50 m below), or -1e9 */
float world_floor_y(v3 p);

/* pickups: returns the kind taken near p within r, or -1 */
int world_take_pickup(v3 p, float r);
int world_spawns(const spawn_t **out);    /* this level's enemies */

/* drop rings for the orbital drop: count, positions, radius; a ring is
 * collected once (world_ring_collect returns true the first time) */
int world_rings(const v3 **pos, float *radius);
bool world_ring_collect(int i);

void world_update(float dt, v3 player);   /* moving platforms, spinning things */

/* story and cutscenes (docs/STORY.md) */
const char *world_ship_name(int level);   /* "THE LESSEE" */
/* named places for cutscenes (pos is a point on the floor unless noted; yaw is the
 * facing a camera or actor should take, 0 = looking down the level toward +z):
 *   "landing"      the touchdown point, every level
 *   "brute_arena"  centre of the Brute arena, levels 0 to 4 (what game.c measures the fight from)
 *   "exit"         just short of the exit gate, levels 0 to 4 (cargo lift, tram, relay); the level 5 value is the arena centre
 *   "boss_arena"   centre of the ring arena, level 5
 *   "boss_spawn"   where the Superintendent rises, level 5 (yaw faces the gate)
 *   "vista"        a landmark to frame, every level: stand the camera about 90 m behind it
 *                  (pos - 90 * (sin yaw, 0, cos yaw), up 12 m) and look at pos; with the colony
 *                  hanging in the sky behind the landmark. The landmark is the solar field (0),
 *                  the greenhouse island (1), the heater core (2), the lobby tower (3), the relay (4)
 *                  and the first crane base (5).
 * Levels: 0 Curtain Wall, 1 Hydroponic Spine, 2 Cold Storage, 3 The Annex, 4 The Basement,
 * 5 The Freehold (the boss; game.c treats WORLD_LEVELS - 1 as the boss level).
 * false if the level has no such place (then *pos is the landing point). */
bool world_anchor(const char *name, v3 *pos, float *yaw);
/* the beat number (0, 1, ...) the first time p enters a story zone this
 * level, else -1. Beats are in level order; story.c has the lines. */
int world_story_trigger(v3 p);
/* arena gates: doors across each arena mouth, closed while a fight is on (they block like
 * walls, and the visible door slides). A door stays open while an enemy is still outside it,
 * so nothing gets locked out. */
void world_set_gates(bool closed);

#endif
