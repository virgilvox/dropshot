/* present.c: the finished frame at the screen's size (present.h).
 *
 * The scale is a draw, not a glBlitFramebuffer: the wasmcart libretro core
 * reads the source rect of a cart's blit into framebuffer 0 as the size of
 * the frame, so a 1280x720 to 1920x1080 blit would show a 1280x720 corner of
 * the 1920x1080 frame. A fullscreen triangle leaves the core's size alone.
 *
 * The filter is Catmull-Rom in nine bilinear taps (the 16 texel kernel with
 * the middle weights folded into one fetch per axis): sharper than bilinear
 * at 1.5x, which keeps HUD type crisp, and clamped so it cannot ring past
 * black or white. */
#include "present.h"
#include <string.h>

static GLuint prog, vao;
static GLint u_size;

static const char *VS =
  "#version 300 es\n"
  "out vec2 uv;\n"
  "void main(){ vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
  "  uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *FS =
  "#version 300 es\nprecision highp float;\n"
  "uniform sampler2D tex; uniform vec2 size;\n"
  "in vec2 uv; out vec4 o;\n"
  "void main(){\n"
  "  vec2 p = uv * size; vec2 t1 = floor(p - 0.5) + 0.5; vec2 f = p - t1;\n"
  "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
  "  vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
  "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
  "  vec2 w3 = f * f * (-0.5 + 0.5 * f);\n"
  "  vec2 w12 = w1 + w2;\n"
  "  vec2 a = (t1 - 1.0) / size, b = (t1 + w2 / w12) / size, c = (t1 + 2.0) / size;\n"
  "  vec3 s = texture(tex, vec2(a.x, a.y)).rgb * w0.x * w0.y + texture(tex, vec2(b.x, a.y)).rgb * w12.x * w0.y + texture(tex, vec2(c.x, a.y)).rgb * w3.x * w0.y\n"
  "         + texture(tex, vec2(a.x, b.y)).rgb * w0.x * w12.y + texture(tex, vec2(b.x, b.y)).rgb * w12.x * w12.y + texture(tex, vec2(c.x, b.y)).rgb * w3.x * w12.y\n"
  "         + texture(tex, vec2(a.x, c.y)).rgb * w0.x * w3.y + texture(tex, vec2(b.x, c.y)).rgb * w12.x * w3.y + texture(tex, vec2(c.x, c.y)).rgb * w3.x * w3.y;\n"
  "  o = vec4(clamp(s, 0.0, 1.0), 1.0); }\n";

static void say(const char *what, const char *log) {
  char m[600];
  strcpy(m, "present: "); strncat(m, what, 64); strncat(m, log, 512);
  wc_log(m, (unsigned)strlen(m));
}

static GLuint sh(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL); glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[512]; log[0] = 0; glGetShaderInfoLog(s, 512, NULL, log); say("shader ", log); }
  return s;
}

void present_size(const wc_host_info_t *host, int rw, int rh, int *ow, int *oh) {
  int w = (int)host->preferred_width, h = (int)host->preferred_height;
  if (w > 3840) w = 3840;
  if (h > 2160) h = 2160;
  if (h * 16 > w * 9) h = w * 9 / 16; else w = h * 16 / 9;
  w &= ~1; h &= ~1;
  if (w > rw && h > rh) { *ow = w; *oh = h; } else { *ow = rw; *oh = rh; }
}

GLuint present_target(int rw, int rh, GLuint *tex) {
  GLuint t = 0, rb = 0, fbo = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenRenderbuffers(1, &rb);
  glBindRenderbuffer(GL_RENDERBUFFER, rb);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, rw, rh);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) say("target ", "incomplete");
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  *tex = t;
  return fbo;
}

void present_scale(GLuint tex, int rw, int rh, int ow, int oh) {
  if (!prog) {
    prog = glCreateProgram();
    GLuint v = sh(GL_VERTEX_SHADER, VS), f = sh(GL_FRAGMENT_SHADER, FS);
    glAttachShader(prog, v); glAttachShader(prog, f); glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; log[0] = 0; glGetProgramInfoLog(prog, 512, NULL, log); say("link ", log); }
    glDeleteShader(v); glDeleteShader(f);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    u_size = glGetUniformLocation(prog, "size");
    glGenVertexArrays(1, &vao);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, ow, oh);
  glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glUseProgram(prog);
  glUniform2f(u_size, (float)rw, (float)rh);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glBindVertexArray(vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
}
