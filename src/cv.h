/* cv.h: an HTML canvas 2D context in C, over canvas_ity (patched to blend in
 * sRGB space, as browsers do). The original paints its textures, the dance
 * break and (here) the whole HUD and every screen with this API, so the calls
 * map one to one: fillStyle is cv_fill_css, fillText is cv_fill_text, ...
 *
 * Colours are CSS strings ("#f2ab1e", "#fff", "rgba(0,0,0,.4)", "transparent")
 * or 0xRRGGBB with an alpha. Text uses the three fonts of ck.h (F_MARKER, ...);
 * letter spacing is CSS letter-spacing in pixels (added after every glyph). */
#ifndef CK_CV_H
#define CK_CV_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cv cv;

void cv_font_add(int id, const uint8_t *ttf, int len);   /* once, at init */

cv *cv_new(int w, int h);
void cv_free(cv *c);
int cv_w(const cv *c);
int cv_h(const cv *c);
/* the pixels, unpremultiplied sRGB RGBA8, w*h*4 (rows top down) */
void cv_read(cv *c, uint8_t *out);
void cv_read_rect(cv *c, uint8_t *out, int x, int y, int w, int h);

void cv_save(cv *c);
void cv_restore(cv *c);
void cv_translate(cv *c, float x, float y);
void cv_rotate(cv *c, float a);
void cv_scale(cv *c, float x, float y);
void cv_transform(cv *c, float a, float b, float cc, float d, float e, float f);
void cv_set_transform(cv *c, float a, float b, float cc, float d, float e, float f);
void cv_reset_transform(cv *c);

void cv_alpha(cv *c, float a);                 /* globalAlpha */
enum { CV_SOURCE_OVER, CV_DEST_OUT, CV_LIGHTER, CV_SOURCE_ATOP, CV_DEST_IN, CV_COPY };
void cv_comp(cv *c, int op);                   /* globalCompositeOperation */

void cv_fill_css(cv *c, const char *css);
void cv_stroke_css(cv *c, const char *css);
void cv_fill_hex(cv *c, uint32_t rgb, float a);
void cv_stroke_hex(cv *c, uint32_t rgb, float a);
/* gradients: set, then add stops (stops apply to the brush just set) */
void cv_fill_linear(cv *c, float x0, float y0, float x1, float y1);
void cv_fill_radial(cv *c, float x0, float y0, float r0, float x1, float y1, float r1);
void cv_fill_stop(cv *c, float t, const char *css);
void cv_stroke_linear(cv *c, float x0, float y0, float x1, float y1);
void cv_stroke_stop(cv *c, float t, const char *css);
/* createPattern(image, "repeat"): rgba is unpremultiplied sRGB RGBA8 */
void cv_fill_pattern(cv *c, const uint8_t *rgba, int w, int h);

void cv_line_width(cv *c, float w);
enum { CV_JOIN_MITER, CV_JOIN_BEVEL, CV_JOIN_ROUND };
enum { CV_CAP_BUTT, CV_CAP_SQUARE, CV_CAP_ROUND };
void cv_line_join(cv *c, int j);
void cv_line_cap(cv *c, int cap);
void cv_shadow(cv *c, const char *css, float blur, float dx, float dy);
void cv_no_shadow(cv *c);

void cv_begin(cv *c);
void cv_move(cv *c, float x, float y);
void cv_line(cv *c, float x, float y);
void cv_quad(cv *c, float cx, float cy, float x, float y);
void cv_bezier(cv *c, float c1x, float c1y, float c2x, float c2y, float x, float y);
void cv_arc(cv *c, float x, float y, float r, float a0, float a1, bool ccw);
void cv_arc_to(cv *c, float x1, float y1, float x2, float y2, float r);
void cv_ellipse(cv *c, float x, float y, float rx, float ry, float rot, float a0, float a1, bool ccw);
void cv_rect(cv *c, float x, float y, float w, float h);
void cv_round_rect(cv *c, float x, float y, float w, float h, float r);
void cv_close(cv *c);
void cv_fill(cv *c);
void cv_stroke(cv *c);
void cv_clip(cv *c);
/* an SVG path string (M L H V Z, absolute and relative; what Path2D gets in the original) */
void cv_svg_path(cv *c, const char *d);

void cv_fill_rect(cv *c, float x, float y, float w, float h);
void cv_stroke_rect(cv *c, float x, float y, float w, float h);
void cv_clear_rect(cv *c, float x, float y, float w, float h);
void cv_clear(cv *c);

/* text */
enum { CV_LEFT, CV_RIGHT, CV_CENTER };
enum { CV_ALPHABETIC, CV_TOP, CV_MIDDLE, CV_BOTTOM, CV_HANGING };
void cv_font(cv *c, int font, float px);
void cv_letter_spacing(cv *c, float px);
void cv_text_align(cv *c, int a);
void cv_text_baseline(cv *c, int b);
void cv_fill_text(cv *c, const char *s, float x, float y);
void cv_stroke_text(cv *c, const char *s, float x, float y);
float cv_measure(cv *c, const char *s);        /* advance width, with letter spacing */
/* font metrics of the current font at its size: ascent, descent (positive), in px */
void cv_font_metrics(cv *c, float *ascent, float *descent);

/* drawImage(src, dx, dy, dw, dh) of a w x h RGBA8 image (unpremultiplied sRGB) */
void cv_draw_image(cv *c, const uint8_t *rgba, int w, int h, float dx, float dy, float dw, float dh);
/* drawImage of another canvas */
void cv_draw_canvas(cv *c, cv *src, float dx, float dy, float dw, float dh);

#ifdef __cplusplus
}
#endif
#endif
