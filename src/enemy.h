/* enemy.h: the Tenants. Grunts, flyers, turrets, Brutes and the boss, their
 * AI and attacks, and the weak points the scope is for. */
#ifndef DS_ENEMY_H
#define DS_ENEMY_H
#include "ds.h"
#include "three.h"

typedef enum { EN_GRUNT, EN_FLYER, EN_TURRET, EN_BRUTE, EN_BOSS, EN_TYPE_COUNT } enemy_type;

void enemy_init(t3_scene *scene);         /* once: load the models */
void enemy_set_level(int level);          /* 0..2: before spawning a level's enemies (picks the Tenants' tint) */
void enemy_clear(void);                   /* remove every enemy (level change, retry) */
void enemy_spawn(int type, v3 pos, float yaw);

/* step every enemy. player: Juno's feet; decoy: where a decoy is, if active */
void enemy_update(float dt, v3 player, bool decoy_active, v3 decoy);

/* the first enemy hit by a ray: its id, the distance, and whether a weak
 * point was hit. Returns false if none within max_dist. */
bool enemy_raycast(v3 origin, v3 dir, float max_dist, int *id, float *t, bool *weak);
/* damage an enemy; at is the hit point. Returns true if this killed it. */
bool enemy_damage(int id, float amount, v3 at, bool weak);
/* explosion damage to everything in range */
void enemy_blast(v3 at, float radius, float amount);

/* the damage enemy attacks did to a player cylinder this frame, and the
 * direction it came from (for the HUD's hit marker) */
float enemy_hurt_player(v3 player, float r, float h, v3 *from);

int enemy_alive(void);                    /* enemies left this level */
int enemy_take_score(void);               /* points earned since the last call */
bool enemy_boss_active(void);
float enemy_boss_health(void);            /* 0..1 */
int enemy_boss_phase(void);               /* 1..3 */
bool enemy_boss_dead(void);
/* the nearest weak point on screen-ish terms: a world position the HUD can
 * mark when scoped. Returns false if none. */
bool enemy_weak_point_near(v3 origin, v3 dir, float cos_cone, v3 *at);

/* the boss stays dormant (idle, untargetable, harmless) until woken, which
 * game.c does when its reveal cutscene ends */
void enemy_boss_wake(void);
/* enemies of a type (or -1 for any) alive within r of c: arenas lock until 0 */
int enemy_alive_near(v3 c, float r, int type);

#endif
