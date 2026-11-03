/* ds.h: what every DROPSHOT source shares. The render size, small math, the
 * random source, the asset loader and the palette. */
#ifndef DS_COMMON_H
#define DS_COMMON_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>

#define DS_W 1280
#define DS_H 720
#define DS_PI 3.14159265358979323846f

typedef struct { float x, y, z; } v3;
static inline v3 v3_(float x, float y, float z) { v3 r = {x, y, z}; return r; }
static inline v3 v3_add(v3 a, v3 b) { return v3_(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 v3_sub(v3 a, v3 b) { return v3_(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 v3_scale(v3 a, float s) { return v3_(a.x * s, a.y * s, a.z * s); }
static inline float v3_dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float v3_len(v3 a) { return sqrtf(v3_dot(a, a)); }
static inline v3 v3_norm(v3 a) { float l = v3_len(a); return l > 1e-6f ? v3_scale(a, 1 / l) : v3_(0, 0, 0); }
static inline v3 v3_lerp(v3 a, v3 b, float t) { return v3_(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t); }
static inline v3 v3_cross(v3 a, v3 b) { return v3_(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }

static inline float ds_clamp(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float ds_lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline float ds_sign(float v) { return v > 0 ? 1.f : v < 0 ? -1.f : 0.f; }
static inline float ds_wrap_pi(float a) { return atan2f(sinf(a), cosf(a)); }
/* frame-rate independent ease toward a target: rate is "fraction left after one second" style speed */
static inline float ds_damp(float cur, float target, float speed, float dt) { return ds_lerp(cur, target, 1 - expf(-speed * dt)); }

void ds_seed(uint32_t s);
float ds_rand(void);                                          /* [0, 1) */
static inline float ds_rr(float a, float b) { return a + ds_rand() * (b - a); }

double ds_now(void);                                          /* seconds since boot */
uint8_t *ds_asset(const char *name, size_t *len);             /* malloc'd, NULL if missing */
void ds_log(const char *fmt, ...);

/* fonts (assets/fonts), registered with cv at init */
enum { F_DISPLAY = 0, F_TYPE = 1, F_MONO = 2 };               /* Archivo Black, IBM Plex Mono, JetBrains Mono */

/* palette: hi-vis washer orange over a cold orbital sky */
#define C_HIVIS  0xff7a1au
#define C_TEAL   0x2ec4b6u
#define C_SKY    0x9fc3e0u
#define C_DUSK   0x23263au
#define C_INK    0x101218u
#define C_CREAM  0xf2ead8u
#define C_ALERT  0xe8433au
#define C_GOO    0x9be35au

#endif
