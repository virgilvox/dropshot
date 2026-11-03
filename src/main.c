/* main.c: the wasmcart glue. wc_info, init, the frame (input, game step,
 * render, present) and the audio ring.
 *
 * Render pipeline: three.c draws the scene into our framebuffer (RGBA8 +
 * depth, tone mapped), the HUD is composited over it (hud.c), and present.c
 * scales the result to the host's preferred 16:9 size. */
#include "wasmcart.h"
#include "wc_gl.h"
#include "three.h"
#include "ds.h"
#include "cv.h"
#include "input.h"
#include "game.h"
#include "audio.h"
#include "present.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_CAP 8192

static uint32_t framebuffer[4];
static float audio_ring[AUDIO_CAP * 2];
static uint32_t audio_cursor;
static wc_pad_t pads[4];
static uint8_t keys[32];
static wc_pointer_t pointers[10];
static wc_time_t tm;
static wc_info_t info;
static wc_host_info_t host_info;
ds_save_t DS_SAVE;

t3_renderer *DS_R;
static double clock_s;
static uint32_t seed = 7;
static GLuint fbo, fbo_color, fbo_depth;
static int out_w = DS_W, out_h = DS_H;
static uint32_t audio_rate = 48000;
static uint64_t rs0, rs1;

/* ── shared helpers (ds.h) ── */
void ds_seed(uint32_t s) {
  uint64_t z = (uint64_t)s * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
  for (int i = 0; i < 2; i++) {
    z += 0x9E3779B97F4A7C15ull;
    uint64_t x = z;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    if (i == 0) rs0 = x; else rs1 = x;
  }
}
float ds_rand(void) {
  uint64_t s1 = rs0, s0 = rs1;
  rs0 = s0;
  s1 ^= s1 << 23; s1 ^= s1 >> 17; s1 ^= s0; s1 ^= s0 >> 26;
  rs1 = s1;
  float f = (float)((double)((rs0 + rs1) >> 11) / 9007199254740992.0);
  return f >= 1.f ? 0.99999994f : f;
}
double ds_now(void) { return clock_s; }
uint8_t *ds_asset(const char *name, size_t *len) {
  int n = wc_asset_size(name, (unsigned)strlen(name));
  if (n < 0) { ds_log("missing asset %s", name); if (len) *len = 0; return NULL; }
  uint8_t *p = malloc((size_t)n + 1);
  if (!p) return NULL;
  int got = wc_load_asset(name, (unsigned)strlen(name), p, (unsigned)n);
  if (got < 0) { free(p); if (len) *len = 0; return NULL; }
  p[got] = 0;
  if (len) *len = (size_t)got;
  return p;
}
void ds_log(const char *fmt, ...) {
  char b[1024];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n >= (int)sizeof b) n = sizeof b - 1;
  wc_log(b, (unsigned)n);
}

__attribute__((export_name("wc_get_info")))
wc_info_t *wc_get_info(void) {
  info.version = WC_ABI_VERSION;
  info.width = out_w; info.height = out_h;
  info.fb_ptr = (uint32_t)(uintptr_t)framebuffer;
  info.audio_ptr = (uint32_t)(uintptr_t)audio_ring;
  info.audio_cap = AUDIO_CAP;
  info.audio_write_ptr = (uint32_t)(uintptr_t)&audio_cursor;
  info.input_ptr = (uint32_t)(uintptr_t)pads;
  info.time_ptr = (uint32_t)(uintptr_t)&tm;
  info.host_info_ptr = (uint32_t)(uintptr_t)&host_info;
  info.keys_ptr = (uint32_t)(uintptr_t)keys;
  info.pointer_ptr = (uint32_t)(uintptr_t)pointers;
  info.flags = WC_FLAG_AUDIO_F32 | WC_FLAG_KEYBOARD;
  info.gpu_api = 1;
  info.audio_sample_rate = 0;
  info.save_ptr = (uint32_t)(uintptr_t)&DS_SAVE;
  info.save_size = sizeof DS_SAVE;
  return &info;
}

__attribute__((export_name("wc_set_seed")))
void wc_set_seed(uint32_t s) { seed = s ? s : 7; }

static void fonts_init(void) {
  static const char *f[3] = {"Archivo.ttf", "IBMPlexMono.ttf", "JetBrainsMono.ttf"};
  for (int i = 0; i < 3; i++) {
    size_t n; uint8_t *b = ds_asset(f[i], &n);
    if (b) cv_font_add(i, b, (int)n);   /* kept for the life of the cart */
  }
}

__attribute__((export_name("wc_init")))
void wc_init(void) {
  extern const struct t3_gen_table ds_gl;
  t3_register_program_table(&ds_gl, NULL);
  ds_seed(seed);
  audio_rate = host_info.audio_sample_rate ? host_info.audio_sample_rate : 48000;

  DS_R = t3_renderer_new(DS_W, DS_H);
  t3_renderer_set_antialias(DS_R, 4);
  t3_renderer_set_tone_mapping(DS_R, T3_ACES_FILMIC_TONE_MAPPING, 1.05f);
  t3_renderer_set_shadow_map(DS_R, true, T3_PCF_SHADOW_MAP);

  glGenTextures(1, &fbo_color);
  glBindTexture(GL_TEXTURE_2D, fbo_color);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, DS_W, DS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenRenderbuffers(1, &fbo_depth);
  glBindRenderbuffer(GL_RENDERBUFFER, fbo_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, DS_W, DS_H);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_color, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo_depth);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  t3_renderer_set_framebuffer(DS_R, fbo);
  present_size(&host_info, DS_W, DS_H, &out_w, &out_h);
  if (out_w != DS_W || out_h != DS_H) {
    /* present_scale samples the target between texels */
    glBindTexture(GL_TEXTURE_2D, fbo_color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  info.width = out_w; info.height = out_h;

  fonts_init();
  audio_init(audio_rate);
  game_init();
}

void render_scene(t3_scene *scene, t3_camera *cam) {
  t3_renderer_set_framebuffer(DS_R, fbo);
  t3_renderer_render(DS_R, scene, cam);
  t3_renderer_reset_bindings(DS_R);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glViewport(0, 0, DS_W, DS_H);
}
void render_target(void) {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glViewport(0, 0, DS_W, DS_H);
}
void render_clear(uint32_t rgb) {
  render_target();
  glDisable(GL_SCISSOR_TEST);
  glClearColor(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1);
  glDepthMask(GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}
static void present(void) {
  if (out_w != DS_W || out_h != DS_H) {
    present_scale(fbo_color, DS_W, DS_H, out_w, out_h);
    t3_renderer_reset_bindings(DS_R);
    return;
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, DS_W, DS_H, 0, 0, DS_W, DS_H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  t3_renderer_reset_bindings(DS_R);
}

__attribute__((export_name("wc_render")))
void wc_render(void) {
  float dt = tm.delta_ms > 0 ? (float)(tm.delta_ms / 1000.0) : 1.f / 60.f;
  if (dt > 0.05f) dt = 0.05f;
  clock_s += dt;

  input_frame(pads, keys);
  game_frame(dt);
  game_draw();
  present();

  audio_render(audio_ring, AUDIO_CAP, &audio_cursor, audio_rate, tm.delta_ms > 0 ? tm.delta_ms : dt * 1000.0);

  static int logged;
  const char *miss = t3_renderer_generated_missing(DS_R);
  if (miss && *miss && logged < 4 && tm.frame % 120 == 0) { wc_log(miss, (unsigned)strlen(miss)); logged++; }
}
