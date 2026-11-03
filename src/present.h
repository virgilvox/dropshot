/* present.h: the finished frame at the screen's size.
 *
 * The game renders at its own size (rw x rh) into an offscreen target. When
 * the host prefers a bigger 16:9 frame, the cart reports that size instead
 * and present_scale draws the target up to it. The HeatSync station asks for
 * 1920x1080 and scales frames by whole multiples only, so a 1280x720 frame
 * sat in a box at 1x in the middle of the screen. */
#ifndef PRESENT_H
#define PRESENT_H
#include "wasmcart.h"
#include "wc_gl.h"

/* The output size for a game rendering at rw x rh: the largest 16:9 frame
 * inside the host's preferred size (at most 3840x2160), or rw x rh when that
 * is not bigger. A host with no preference gets rw x rh. */
void present_size(const wc_host_info_t *host, int rw, int rh, int *ow, int *oh);

/* An offscreen target of rw x rh: RGBA8 colour (*tex, linear filtered for
 * present_scale) with depth and stencil. Returns the framebuffer. */
GLuint present_target(int rw, int rh, GLuint *tex);

/* Draw tex (rw x rh) into framebuffer 0 at ow x oh with a Catmull-Rom
 * filter. Leaves GL state changed: the caller resets its renderer's cache. */
void present_scale(GLuint tex, int rw, int rh, int ow, int oh);

#endif
