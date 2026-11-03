// cv.cpp: the C canvas over canvas_ity (see cv.h).
#define CANVAS_ITY_IMPLEMENTATION
#define CANVAS_ITY_SRGB_BLEND
#include "canvas_ity.hpp"
#include "cv.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

using namespace canvas_ity;

namespace {
struct font_t { const uint8_t *ttf; int len; float asc, desc; };   // asc/desc per em
font_t fonts[4];

int u16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
int s16(const uint8_t *p) { int v = u16(p); return v >= 32768 ? v - 65536 : v; }
uint32_t u32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// hhea ascender / descender over head unitsPerEm (what Chrome uses for these fonts)
void metrics(font_t &f) {
  f.asc = 0.9f; f.desc = 0.25f;
  if (f.len < 12) return;
  int n = u16(f.ttf + 4); int upem = 1000; int asc = 0, desc = 0; bool have = false;
  for (int i = 0; i < n; i++) {
    const uint8_t *r = f.ttf + 12 + i * 16;
    uint32_t off = u32(r + 8);
    if (!memcmp(r, "head", 4)) upem = u16(f.ttf + off + 18);
    if (!memcmp(r, "hhea", 4)) { asc = s16(f.ttf + off + 4); desc = s16(f.ttf + off + 6); have = true; }
  }
  if (have && upem > 0) { f.asc = (float)asc / upem; f.desc = (float)-desc / upem; }
}

struct css_rgba { float r, g, b, a; };
float hexv(char c) { return (float)(isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10)); }
css_rgba parse(const char *s) {
  css_rgba c = {0, 0, 0, 1};
  while (*s == ' ') s++;
  if (*s == '#') {
    size_t n = strlen(s + 1);
    if (n == 3 || n == 4) {
      c.r = hexv(s[1]) * 17 / 255.f; c.g = hexv(s[2]) * 17 / 255.f; c.b = hexv(s[3]) * 17 / 255.f;
      if (n == 4) c.a = hexv(s[4]) * 17 / 255.f;
    } else if (n >= 6) {
      c.r = (hexv(s[1]) * 16 + hexv(s[2])) / 255.f; c.g = (hexv(s[3]) * 16 + hexv(s[4])) / 255.f;
      c.b = (hexv(s[5]) * 16 + hexv(s[6])) / 255.f;
      if (n >= 8) c.a = (hexv(s[7]) * 16 + hexv(s[8])) / 255.f;
    }
    return c;
  }
  if (!strncmp(s, "rgb", 3)) {
    const char *p = strchr(s, '(');
    float v[4] = {0, 0, 0, 1}; int k = 0;
    while (p && k < 4) { p++; v[k++] = strtof(p, (char **)&p); while (*p == ' ') p++; if (*p != ',') break; }
    c.r = v[0] / 255.f; c.g = v[1] / 255.f; c.b = v[2] / 255.f; c.a = v[3];
    return c;
  }
  if (!strcmp(s, "transparent")) { c.a = 0; return c; }
  if (!strcmp(s, "white")) { c.r = c.g = c.b = 1; return c; }
  return c;   // black
}
}  // namespace

struct cv {
  canvas *k;
  int w, h;
  int font, cur_font; float px, cur_px;
  float spacing;
  int align, baseline;
  brush_type last_grad;
  // the state save() keeps that canvas_ity does not: font, spacing, align, baseline
  struct st { int font; float px, spacing; int align, baseline; } stack[32]; int sp;
};

extern "C" {

void cv_font_add(int id, const uint8_t *ttf, int len) {
  if (id < 0 || id >= 4) return;
  fonts[id].ttf = ttf; fonts[id].len = len; metrics(fonts[id]);
}

cv *cv_new(int w, int h) {
  cv *c = (cv *)calloc(1, sizeof(cv));
  c->k = new canvas(w, h); c->w = w; c->h = h;
  c->font = 0; c->px = 10; c->cur_font = -1; c->cur_px = -1;
  return c;
}
void cv_free(cv *c) { if (!c) return; delete c->k; free(c); }
int cv_w(const cv *c) { return c->w; }
int cv_h(const cv *c) { return c->h; }
void cv_read(cv *c, uint8_t *out) { c->k->get_image_data(out, c->w, c->h, c->w * 4, 0, 0); }
void cv_read_rect(cv *c, uint8_t *out, int x, int y, int w, int h) { c->k->get_image_data(out, w, h, w * 4, x, y); }

void cv_save(cv *c) {
  c->k->save();
  if (c->sp < 32) c->stack[c->sp] = {c->font, c->px, c->spacing, c->align, c->baseline};
  c->sp++;
}
void cv_restore(cv *c) {
  c->k->restore();
  c->cur_font = -1;   // canvas_ity restored its own font: set ours again on the next text
  if (c->sp > 0 && --c->sp < 32) {
    cv::st &s = c->stack[c->sp];
    c->font = s.font; c->px = s.px; c->spacing = s.spacing; c->align = s.align; c->baseline = s.baseline;
  }
}
void cv_translate(cv *c, float x, float y) { c->k->translate(x, y); }
void cv_rotate(cv *c, float a) { c->k->rotate(a); }
void cv_scale(cv *c, float x, float y) { c->k->scale(x, y); }
void cv_transform(cv *c, float a, float b, float cc, float d, float e, float f) { c->k->transform(a, b, cc, d, e, f); }
void cv_set_transform(cv *c, float a, float b, float cc, float d, float e, float f) { c->k->set_transform(a, b, cc, d, e, f); }
void cv_reset_transform(cv *c) { c->k->set_transform(1, 0, 0, 1, 0, 0); }

void cv_alpha(cv *c, float a) { c->k->set_global_alpha(a); }
void cv_comp(cv *c, int op) {
  static const composite_operation m[] = {source_over, destination_out, lighter, source_atop, destination_in, source_copy};
  c->k->global_composite_operation = m[op];
}

void cv_fill_css(cv *c, const char *css) { css_rgba v = parse(css); c->k->set_color(fill_style, v.r, v.g, v.b, v.a); }
void cv_stroke_css(cv *c, const char *css) { css_rgba v = parse(css); c->k->set_color(stroke_style, v.r, v.g, v.b, v.a); }
void cv_fill_hex(cv *c, uint32_t rgb, float a) {
  c->k->set_color(fill_style, ((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
void cv_stroke_hex(cv *c, uint32_t rgb, float a) {
  c->k->set_color(stroke_style, ((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
void cv_fill_linear(cv *c, float x0, float y0, float x1, float y1) { c->k->set_linear_gradient(fill_style, x0, y0, x1, y1); c->last_grad = fill_style; }
void cv_fill_radial(cv *c, float x0, float y0, float r0, float x1, float y1, float r1) {
  c->k->set_radial_gradient(fill_style, x0, y0, r0, x1, y1, r1); c->last_grad = fill_style;
}
void cv_fill_stop(cv *c, float t, const char *css) { css_rgba v = parse(css); c->k->add_color_stop(fill_style, t, v.r, v.g, v.b, v.a); }
void cv_stroke_linear(cv *c, float x0, float y0, float x1, float y1) { c->k->set_linear_gradient(stroke_style, x0, y0, x1, y1); }
void cv_stroke_stop(cv *c, float t, const char *css) { css_rgba v = parse(css); c->k->add_color_stop(stroke_style, t, v.r, v.g, v.b, v.a); }
void cv_fill_pattern(cv *c, const uint8_t *rgba, int w, int h) { c->k->set_pattern(fill_style, rgba, w, h, w * 4, repeat); }

void cv_line_width(cv *c, float w) { c->k->set_line_width(w); }
void cv_line_join(cv *c, int j) { c->k->line_join = j == CV_JOIN_ROUND ? rounded : j == CV_JOIN_BEVEL ? bevel : miter; }
void cv_line_cap(cv *c, int cap) { c->k->line_cap = cap == CV_CAP_ROUND ? circle : cap == CV_CAP_SQUARE ? square : butt; }
void cv_shadow(cv *c, const char *css, float blur, float dx, float dy) {
  css_rgba v = parse(css);
  c->k->set_shadow_color(v.r, v.g, v.b, v.a); c->k->set_shadow_blur(blur);
  c->k->shadow_offset_x = dx; c->k->shadow_offset_y = dy;
}
void cv_no_shadow(cv *c) { c->k->set_shadow_color(0, 0, 0, 0); c->k->set_shadow_blur(0); c->k->shadow_offset_x = c->k->shadow_offset_y = 0; }

void cv_begin(cv *c) { c->k->begin_path(); }
void cv_move(cv *c, float x, float y) { c->k->move_to(x, y); }
void cv_line(cv *c, float x, float y) { c->k->line_to(x, y); }
void cv_quad(cv *c, float cx, float cy, float x, float y) { c->k->quadratic_curve_to(cx, cy, x, y); }
void cv_bezier(cv *c, float a, float b, float d, float e, float x, float y) { c->k->bezier_curve_to(a, b, d, e, x, y); }
void cv_arc(cv *c, float x, float y, float r, float a0, float a1, bool ccw) { c->k->arc(x, y, r, a0, a1, ccw); }
void cv_arc_to(cv *c, float x1, float y1, float x2, float y2, float r) { c->k->arc_to(x1, y1, x2, y2, r); }
void cv_ellipse(cv *c, float x, float y, float rx, float ry, float rot, float a0, float a1, bool ccw) {
  // the spec's ellipse(): a polyline fine enough for any size the game draws
  float sweep = a1 - a0;
  if (!ccw && sweep < 0) sweep = fmodf(sweep, 6.2831853f) + 6.2831853f;
  if (ccw && sweep > 0) sweep = fmodf(sweep, 6.2831853f) - 6.2831853f;
  if (fabsf(sweep) >= 6.2831853f) sweep = sweep < 0 ? -6.2831853f : 6.2831853f;
  int n = 96;
  float cr = cosf(rot), sr = sinf(rot);
  for (int i = 0; i <= n; i++) {
    float a = a0 + sweep * i / n, px = cosf(a) * rx, py = sinf(a) * ry;
    float X = x + px * cr - py * sr, Y = y + px * sr + py * cr;
    if (i == 0) c->k->line_to(X, Y); else c->k->line_to(X, Y);
  }
}
void cv_rect(cv *c, float x, float y, float w, float h) { c->k->rectangle(x, y, w, h); }
void cv_round_rect(cv *c, float x, float y, float w, float h, float r) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  c->k->move_to(x + r, y);
  c->k->arc_to(x + w, y, x + w, y + h, r);
  c->k->arc_to(x + w, y + h, x, y + h, r);
  c->k->arc_to(x, y + h, x, y, r);
  c->k->arc_to(x, y, x + w, y, r);
  c->k->close_path();
}
void cv_close(cv *c) { c->k->close_path(); }
void cv_fill(cv *c) { c->k->fill(); }
void cv_stroke(cv *c) { c->k->stroke(); }
void cv_clip(cv *c) { c->k->clip(); }

void cv_svg_path(cv *c, const char *d) {
  float cx = 0, cy = 0, sx = 0, sy = 0; char cmd = 'M';
  const char *p = d;
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    if (isalpha((unsigned char)*p)) { cmd = *p++; if (cmd == 'Z' || cmd == 'z') { c->k->close_path(); cx = sx; cy = sy; } continue; }
    char *e; float a = strtof(p, &e); if (e == p) { p++; continue; } p = e;
    bool rel = islower((unsigned char)cmd);
    switch (toupper((unsigned char)cmd)) {
      case 'M': case 'L': {
        while (*p == ' ' || *p == ',') p++;
        float b = strtof(p, &e); p = e;
        float x = rel ? cx + a : a, y = rel ? cy + b : b;
        if (toupper((unsigned char)cmd) == 'M') { c->k->move_to(x, y); sx = x; sy = y; cmd = rel ? 'l' : 'L'; }
        else c->k->line_to(x, y);
        cx = x; cy = y; break;
      }
      case 'H': cx = rel ? cx + a : a; c->k->line_to(cx, cy); break;
      case 'V': cy = rel ? cy + a : a; c->k->line_to(cx, cy); break;
      default: break;
    }
  }
}

void cv_fill_rect(cv *c, float x, float y, float w, float h) { c->k->fill_rectangle(x, y, w, h); }
void cv_stroke_rect(cv *c, float x, float y, float w, float h) { c->k->stroke_rectangle(x, y, w, h); }
void cv_clear_rect(cv *c, float x, float y, float w, float h) { c->k->save(); c->k->set_color(fill_style, 0, 0, 0, 1); c->k->clear_rectangle(x, y, w, h); c->k->restore(); }
void cv_clear(cv *c) {
  /* canvas_ity's clear_rectangle erases with the fill colour's alpha: make it opaque first */
  c->k->save(); c->k->set_transform(1, 0, 0, 1, 0, 0); c->k->set_color(fill_style, 0, 0, 0, 1);
  c->k->clear_rectangle(0, 0, (float)c->w, (float)c->h); c->k->restore();
}

void cv_font(cv *c, int font, float px) { c->font = font; c->px = px; }
void cv_letter_spacing(cv *c, float px) { c->spacing = px; }
void cv_text_align(cv *c, int a) { c->align = a; }
void cv_text_baseline(cv *c, int b) { c->baseline = b; }

static void use_font(cv *c) {
  // canvas_ity copies the font on set_font, and save() keeps it: only set it on a change
  if (c->cur_font == c->font && c->cur_px == c->px) return;
  font_t &f = fonts[c->font];
  if (f.ttf) c->k->set_font(f.ttf, f.len, c->px);
  c->cur_font = c->font; c->cur_px = c->px;
}

static int utf8_len(const char *s) {
  unsigned char ch = (unsigned char)*s;
  return ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
}

float cv_measure(cv *c, const char *s) {
  use_font(c);
  if (c->spacing == 0) return c->k->measure_text(s);
  float w = 0; char g[8];
  while (*s) {
    int n = utf8_len(s); memcpy(g, s, n); g[n] = 0; s += n;
    w += c->k->measure_text(g) + c->spacing;
  }
  return w;
}

void cv_font_metrics(cv *c, float *ascent, float *descent) {
  font_t &f = fonts[c->font];
  if (ascent) *ascent = f.asc * c->px;
  if (descent) *descent = f.desc * c->px;
}

static void text(cv *c, const char *s, float x, float y, bool stroke) {
  use_font(c);
  // baseline: canvas_ity's middle and top use its own font box; place it from the hhea metrics,
  // which is what the browser's textBaseline does for these fonts
  font_t &f = fonts[c->font];
  float asc = f.asc * c->px, desc = f.desc * c->px;
  float by = y;
  if (c->baseline == CV_TOP || c->baseline == CV_HANGING) by = y + asc;
  else if (c->baseline == CV_MIDDLE) by = y + (asc - desc) / 2;
  else if (c->baseline == CV_BOTTOM) by = y - desc;
  c->k->text_baseline = alphabetic;
  float w = cv_measure(c, s);
  float x0 = c->align == CV_CENTER ? x - w / 2 : c->align == CV_RIGHT ? x - w : x;
  c->k->text_align = leftward;
  if (c->spacing == 0) {
    if (stroke) c->k->stroke_text(s, x0, by); else c->k->fill_text(s, x0, by);
    return;
  }
  char g[8];
  while (*s) {
    int n = utf8_len(s); memcpy(g, s, n); g[n] = 0; s += n;
    if (stroke) c->k->stroke_text(g, x0, by); else c->k->fill_text(g, x0, by);
    x0 += c->k->measure_text(g) + c->spacing;
  }
}
void cv_fill_text(cv *c, const char *s, float x, float y) { text(c, s, x, y, false); }
void cv_stroke_text(cv *c, const char *s, float x, float y) { text(c, s, x, y, true); }

void cv_draw_image(cv *c, const uint8_t *rgba, int w, int h, float dx, float dy, float dw, float dh) {
  c->k->draw_image(rgba, w, h, w * 4, dx, dy, dw, dh);
}
void cv_draw_canvas(cv *c, cv *src, float dx, float dy, float dw, float dh) {
  uint8_t *px = (uint8_t *)malloc((size_t)src->w * src->h * 4);
  cv_read(src, px);
  c->k->draw_image(px, src->w, src->h, src->w * 4, dx, dy, dw, dh);
  free(px);
}

}  // extern "C"

// the cart links with emcc as C, without libc++: new and delete over malloc
void *operator new(size_t n) { return malloc(n ? n : 1); }
void *operator new[](size_t n) { return malloc(n ? n : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
