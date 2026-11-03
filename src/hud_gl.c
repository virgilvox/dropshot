/* hud_gl.c: layers and the compositing program. One quad program, blended
 * ONE, ONE_MINUS_SRC_ALPHA into the (non sRGB) frame target, so it matches how
 * cv blends. Everything is in 1280x720 pixels, y down. */
#include "hud_int.h"
#include "ds.h"
#include "wasmcart.h"
#include "wc_gl.h"
#include <stdlib.h>

#ifndef GL_UNPACK_ALIGNMENT
#define GL_UNPACK_ALIGNMENT 0x0CF5
#endif

static GLuint prog, vao, vbo;
static GLint u_m, u_size, u_tex, u_mode, u_col, u_col2, u_alpha, u_prm, u_uv;
static cv *scratch;
static uint8_t *upbuf; static size_t upcap;

static const char *VS =
  "#version 300 es\n"
  "layout(location=0) in vec2 p;\n"
  "uniform mat3 m; uniform vec2 size; uniform vec4 uv;\n"
  "out vec2 vuv; out vec2 vpx; out vec2 vp;\n"
  "void main(){ vec3 s = m * vec3(p * size, 1.0); vuv = uv.xy + p * uv.zw; vpx = s.xy; vp = p;\n"
  "  gl_Position = vec4(s.x / 640.0 - 1.0, 1.0 - s.y / 360.0, 0.0, 1.0); }\n";
static const char *FS =
  "#version 300 es\nprecision highp float; precision highp int;\n"
  "in vec2 vuv; in vec2 vpx; in vec2 vp; out vec4 o;\n"
  "uniform sampler2D tex; uniform int mode; uniform vec4 col; uniform vec4 col2; uniform float alpha; uniform vec4 prm;\n"
  "vec4 pm(vec4 c){ return vec4(c.rgb * c.a, c.a); }\n"
  "void main(){\n"
  "  if (mode == 0) { vec4 c = texture(tex, vuv); o = vec4(c.rgb * col.rgb, c.a) * alpha; }\n"        /* texture is premultiplied */
  "  else if (mode == 1) { o = pm(col); }\n"
  /* scope: black outside the lens, a soft dark rim just inside it */
  "  else if (mode == 2) { float d = length(vpx - prm.xy); float out_ = smoothstep(prm.z - 1.5, prm.z + 0.5, d);\n"
  "    float rim = 0.55 * smoothstep(prm.z * 0.62, prm.z, d); o = vec4(0.0, 0.0, 0.0, max(out_, rim * (1.0 - out_))); }\n"
  "  else if (mode == 3) { vec2 d = (vpx - vec2(640.0, 360.0)) / vec2(905.0, 509.0); float t = smoothstep(prm.x, 1.0, length(d));\n"
  "    o = pm(vec4(col.rgb, col.a * t)); }\n"
  "  else if (mode == 4) { float s = fract((vpx.x + vpx.y) / prm.x + prm.y); vec4 c = s < 0.5 ? col : col2; o = pm(c); }\n"
  /* halftone: dots on a 45 degree grid, bigger toward the bottom (or the top when flipped) */
  "  else if (mode == 5) { float t = prm.y > 0.5 ? 1.0 - vp.y : vp.y; vec2 r = mat2(0.7071, -0.7071, 0.7071, 0.7071) * vpx / prm.x;\n"
  "    float d = length(fract(r) - 0.5); float rad = t * 0.62; float a = smoothstep(rad, rad - 0.12, d); o = pm(vec4(col.rgb, col.a * a)); }\n"
  "  else if (mode == 6) { float d = length(vpx - prm.xy); o = pm(vec4(col.rgb, col.a * (1.0 - smoothstep(prm.z - 1.5, prm.z, d)))); }\n"
  "  else if (mode == 7) { o = pm(vec4(col.rgb, mix(col.a, col2.a, vp.x))); }\n"
  /* shine: a soft slanted band swept over a layer's own alpha, added */
  "  else if (mode == 8) { vec4 c = texture(tex, vuv); float k = (vpx.x - 0.35 * vpx.y) - prm.x; float b = exp(-k * k / (prm.y * prm.y));\n"
  "    o = vec4(vec3(c.a * b * alpha), 0.0); }\n"
  "  else if (mode == 9) { float l = mod(floor(vpx.y), 3.0); o = l > 1.5 ? pm(col) : vec4(0.0); }\n"
  "  else { o = pm(vec4(col.rgb, mix(col.a, col2.a, vp.y))); }\n"        /* 10: vertical gradient */
  "}\n";

static GLuint sh(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL); glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log); ds_log("hud shader: %s", log); }
  return s;
}

void hg_init(void) {
  prog = glCreateProgram();
  glAttachShader(prog, sh(GL_VERTEX_SHADER, VS));
  glAttachShader(prog, sh(GL_FRAGMENT_SHADER, FS));
  glLinkProgram(prog);
  u_m = glGetUniformLocation(prog, "m"); u_size = glGetUniformLocation(prog, "size");
  u_tex = glGetUniformLocation(prog, "tex"); u_mode = glGetUniformLocation(prog, "mode");
  u_col = glGetUniformLocation(prog, "col"); u_col2 = glGetUniformLocation(prog, "col2");
  u_alpha = glGetUniformLocation(prog, "alpha"); u_prm = glGetUniformLocation(prog, "prm");
  u_uv = glGetUniformLocation(prog, "uv");
  static const float q[] = {0, 0, 1, 0, 0, 1, 1, 1};
  glGenVertexArrays(1, &vao); glBindVertexArray(vao);
  glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glBindVertexArray(0);
  scratch = cv_new(DS_W, DS_H);
}

void hg_begin(void) {
  render_target();
  glUseProgram(prog);
  glBindVertexArray(vao);
  glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
  glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, DS_W, DS_H);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(u_tex, 0);
}
void hg_end(void) {
  glBindVertexArray(0);
  glUseProgram(0);
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
}

/* ── layers ── */
uint32_t hash_str(const char *s, uint32_t h) {
  if (!s) return h ^ 0x9e3779b9u;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}
cv *layer_begin(layer_t *L, int w, int h, uint32_t key) {
  if (L->ok && L->w == w && L->h == h && L->key == key) return NULL;
  if (w > DS_W) w = DS_W;
  if (h > DS_H * 2) h = DS_H * 2;
  if (h > DS_H) { ds_log("hud layer too tall %d", h); h = DS_H; }
  if (!L->tex) glGenTextures(1, &L->tex);
  if (!L->ok || L->w != w || L->h != h) {
    glBindTexture(GL_TEXTURE_2D, L->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  }
  L->w = w; L->h = h; L->key = key; L->ok = true;
  cv_save(scratch);
  cv_reset_transform(scratch); cv_alpha(scratch, 1); cv_comp(scratch, CV_SOURCE_OVER); cv_no_shadow(scratch);
  cv_clear_rect(scratch, 0, 0, (float)w, (float)h);
  return scratch;
}
void layer_end(layer_t *L, cv *c) {
  size_t n = (size_t)L->w * L->h;
  if (n * 4 > upcap) { upcap = n * 4; upbuf = realloc(upbuf, upcap); }
  cv_read_rect(c, upbuf, 0, 0, L->w, L->h);
  for (size_t i = 0; i < n; i++) {   /* premultiply for the ONE, 1-a blend */
    uint8_t *q = upbuf + i * 4; unsigned a = q[3];
    if (a == 255) continue;
    q[0] = (uint8_t)((q[0] * a + 127) / 255); q[1] = (uint8_t)((q[1] * a + 127) / 255); q[2] = (uint8_t)((q[2] * a + 127) / 255);
  }
  glBindTexture(GL_TEXTURE_2D, L->tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, L->w, L->h, GL_RGBA, GL_UNSIGNED_BYTE, upbuf);
  cv_restore(c);
}

/* ── drawing ── */
static void set_m(const float m[6]) {
  float g[9] = {m[0], m[1], 0, m[2], m[3], 0, m[4], m[5], 1};
  glUniformMatrix3fv(u_m, 1, GL_FALSE, g);
}
static void col4(GLint u, uint32_t rgb, float a) { glUniform4f(u, ((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a); }
static void quad(int mode, float x, float y, float w, float h) {
  static const float id[6] = {1, 0, 0, 1, 0, 0};
  float m[6] = {1, 0, 0, 1, x, y}; (void)id;
  glUniform1i(u_mode, mode);
  glUniform2f(u_size, w, h);
  set_m(m);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void quad_m(int mode, float w, float h, const float m[6]) {
  glUniform1i(u_mode, mode);
  glUniform2f(u_size, w, h);
  set_m(m);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void centred(float m[6], float cx, float cy, float w, float h, float ang) {
  float c = cosf(ang), s = sinf(ang);
  m[0] = c; m[1] = s; m[2] = -s; m[3] = c;
  m[4] = cx - (c * w / 2 - s * h / 2); m[5] = cy - (s * w / 2 + c * h / 2);
}
void mat_place(float m[6], float cx, float cy, float ang, float ox, float oy, float sx, float sy) {
  float c = cosf(ang), s = sinf(ang);
  m[0] = c * sx; m[1] = s * sx; m[2] = -s * sy; m[3] = c * sy;
  m[4] = cx - (m[0] * ox + m[2] * oy); m[5] = cy - (m[1] * ox + m[3] * oy);
}

void hg_tex_m(const layer_t *L, const float m[6], float alpha, uint32_t tint) {
  if (!L || !L->ok || alpha <= 0) return;
  glBindTexture(GL_TEXTURE_2D, L->tex);
  glUniform1f(u_alpha, alpha); col4(u_col, tint, 1);
  glUniform4f(u_uv, 0, 0, 1, 1);
  quad_m(0, (float)L->w, (float)L->h, m);
}
void hg_tex(const layer_t *L, float x, float y, float alpha) {
  float m[6] = {1, 0, 0, 1, x, y};
  hg_tex_m(L, m, alpha, 0xffffff);
}
void hg_sub(const layer_t *L, int sx, int sy, int sw, int sh, float x, float y, float alpha) {
  if (!L || !L->ok || alpha <= 0) return;
  glBindTexture(GL_TEXTURE_2D, L->tex);
  glUniform1f(u_alpha, alpha); col4(u_col, 0xffffff, 1);
  glUniform4f(u_uv, (float)sx / L->w, (float)sy / L->h, (float)sw / L->w, (float)sh / L->h);
  quad(0, x, y, (float)sw, (float)sh);
}
void hg_rect(float x, float y, float w, float h, uint32_t rgb, float a) {
  if (a <= 0 || w <= 0 || h <= 0) return;
  col4(u_col, rgb, a);
  quad(1, x, y, w, h);
}
void hg_rect_m(float cx, float cy, float w, float h, float ang, uint32_t rgb, float a) {
  if (a <= 0) return;
  float m[6]; centred(m, cx, cy, w, h, ang);
  col4(u_col, rgb, a);
  quad_m(1, w, h, m);
}
void hg_grad(float cx, float cy, float w, float h, float ang, uint32_t rgb, float a0, float a1) {
  float m[6]; centred(m, cx, cy, w, h, ang);
  col4(u_col, rgb, a0); col4(u_col2, rgb, a1);
  quad_m(7, w, h, m);
}
void hg_vgrad(float y0, float y1, uint32_t rgb, float a0, float a1) {
  col4(u_col, rgb, a0); col4(u_col2, rgb, a1);
  quad(10, 0, y0, DS_W, y1 - y0);
}
void hg_scope(float cx, float cy, float r) {
  glUniform4f(u_prm, cx, cy, r, 0);
  quad(2, 0, 0, DS_W, DS_H);
}
void hg_circle(float cx, float cy, float r, uint32_t rgb, float a) {
  col4(u_col, rgb, a); glUniform4f(u_prm, cx, cy, r, 0);
  quad(6, 0, 0, DS_W, DS_H);
}
void hg_vignette(uint32_t rgb, float a, float inner) {
  if (a <= 0) return;
  col4(u_col, rgb, a); glUniform4f(u_prm, inner, 0, 0, 0);
  quad(3, 0, 0, DS_W, DS_H);
}
void hg_stripes(float x, float y, float w, float h, uint32_t c1, float a1, uint32_t c2, float a2, float period, float scroll) {
  col4(u_col, c1, a1); col4(u_col2, c2, a2); glUniform4f(u_prm, period, scroll, 0, 0);
  quad(4, x, y, w, h);
}
void hg_halftone(float x, float y, float w, float h, uint32_t rgb, float a, float cell, bool flip) {
  col4(u_col, rgb, a); glUniform4f(u_prm, cell, flip ? 1.f : 0.f, 0, 0);
  quad(5, x, y, w, h);
}
void hg_scan(float a) {
  col4(u_col, 0x000000, a);
  quad(9, 0, 0, DS_W, DS_H);
}
void hg_shine(const layer_t *L, float x, float y, float pos, float width, float alpha) {
  if (!L || !L->ok) return;
  glBindTexture(GL_TEXTURE_2D, L->tex);
  glUniform1f(u_alpha, alpha); glUniform4f(u_prm, pos, width, 0, 0);
  glUniform4f(u_uv, 0, 0, 1, 1);
  quad(8, x, y, (float)L->w, (float)L->h);
}

/* ── paint helpers ── */
void p_text(cv *c, int font, float px, float ls, int align, const char *s, float x, float y, uint32_t rgb, float a) {
  cv_font(c, font, px); cv_letter_spacing(c, ls); cv_text_align(c, align); cv_text_baseline(c, CV_ALPHABETIC);
  cv_fill_hex(c, rgb, a);
  cv_fill_text(c, s, x, y);
}
void p_plate(cv *c, float x, float y, float w, float h, float cut, uint32_t rgb, float a) {
  cv_begin(c);
  cv_move(c, x + cut, y); cv_line(c, x + w, y); cv_line(c, x + w, y + h - cut);
  cv_line(c, x + w - cut, y + h); cv_line(c, x, y + h); cv_line(c, x, y + cut); cv_close(c);
  cv_fill_hex(c, rgb, a); cv_fill(c);
}
void p_hazard(cv *c, float x, float y, float w, float h, float period, uint32_t c1, uint32_t c2) {
  cv_save(c);
  cv_begin(c); cv_rect(c, x, y, w, h); cv_clip(c);
  cv_fill_hex(c, c2, 1); cv_fill_rect(c, x, y, w, h);
  cv_fill_hex(c, c1, 1);
  for (float sx = x - h; sx < x + w + h; sx += period) {
    cv_begin(c); cv_move(c, sx, y + h); cv_line(c, sx + period / 2, y + h); cv_line(c, sx + period / 2 + h, y); cv_line(c, sx + h, y); cv_close(c); cv_fill(c);
  }
  cv_restore(c);
}
void p_cut(cv *c, float x, float y, float w, float h) {
  cv_save(c); cv_comp(c, CV_DEST_OUT); cv_fill_hex(c, 0, 1); cv_fill_rect(c, x, y, w, h); cv_restore(c);
}
void p_wrap(cv *c, const char *s, float x, float y, float w, float lh, uint32_t rgb) {
  char line[160]; int n = 0; line[0] = 0;
  const char *p = s;
  cv_font(c, F_TYPE, 20); cv_letter_spacing(c, 0);
  while (*p) {
    char word[48]; int k = 0;
    while (*p && *p != ' ' && k < 47) word[k++] = *p++;
    word[k] = 0; while (*p == ' ') p++;
    char trial[200]; snprintf(trial, sizeof trial, n ? "%s %s" : "%s%s", line, word);
    if (n && cv_measure(c, trial) > w) { p_text(c, F_TYPE, 20, 0, CV_LEFT, line, x, y, rgb, 1); y += lh; snprintf(line, sizeof line, "%s", word); }
    else snprintf(line, sizeof line, "%s", trial);
    n++;
  }
  if (line[0]) p_text(c, F_TYPE, 20, 0, CV_LEFT, line, x, y, rgb, 1);
}
const char *key_name(const hud_t *h, const char *pad, const char *kb) { return h->used_pad ? pad : kb; }
