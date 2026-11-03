/* hud.c: the play HUD, the scope view and the orbital drop readouts. Static
 * plates and text are painted once into layers (hud_gl.c) and redrawn only when
 * their numbers change; bars, reticles, stripes and the scope mask are GL
 * shapes, so a play frame costs a couple of dozen quads and almost no CPU paint.
 * The other screens live in hud_screens.c. */
#include "hud_int.h"
#include "game.h"
#include <stdlib.h>

static float now_t, dt_;

/* ── layers ── */
static layer_t L_vit, L_segs, L_wpn, L_score, L_time, L_boss, L_banner, L_hints, L_arc;
static layer_t L_ret[4], L_brk, L_weak, L_zl, L_zr, L_dalt, L_drings, L_dwarn;

/* smoothed values, so bars glide and damage leaves a trail */
static float s_health, s_trail, s_shield, s_boss, s_boss_trail, s_score;
static double banner_t0; static uint32_t banner_key;
static double scope_t0; static bool was_scoped; static int last_zoom;

static void fmt_int(char *b, size_t n, int v) {   /* 12,400 */
  char t[24]; snprintf(t, sizeof t, "%d", v < 0 ? 0 : v);
  int l = (int)strlen(t), o = 0;
  for (int i = 0; i < l && o < (int)n - 2; i++) { if (i && (l - i) % 3 == 0) b[o++] = ','; b[o++] = t[i]; }
  b[o] = 0;
}
static float ease_out(float t) { t = ds_clamp(t, 0, 1); return 1 - (1 - t) * (1 - t) * (1 - t); }



/* ── reusable bits ── */
/* a bar made of chunky cells: the mask layer carries the cell gaps */
static void bar(float x, float y, float w, float h, float v, float trail, uint32_t col, uint32_t trail_col) {
  hg_rect(x, y, w, h, 0x07080c, 1);
  hg_rect(x, y, w * ds_clamp(trail, 0, 1), h, trail_col, 0.85f);
  hg_rect(x, y, w * ds_clamp(v, 0, 1), h, col, 1);
  hg_rect(x, y, w * ds_clamp(v, 0, 1), h * 0.32f, 0xffffff, 0.16f);
}
static void segs(float x, float y, float w, float h, float cell) {
  int key = (int)(w * 10 + h);
  cv *c = layer_begin(&L_segs, (int)w, (int)h, (uint32_t)key);
  if (c) {
    cv_fill_hex(c, C_INK, 1);
    for (float sx = cell; sx < w - 1; sx += cell) cv_fill_rect(c, sx - 1.5f, 0, 3, h);
    layer_end(&L_segs, c);
  }
  hg_tex(&L_segs, x, y, 1);
}

/* ── play ── */
static void play_vitals(const hud_t *h) {
  const float x = 48, y = 556, w = 404, hh = 116;
  cv *c = layer_begin(&L_vit, (int)w, (int)hh, 1);
  if (c) {
    p_plate(c, 0, 0, w, hh, 16, C_INK, 0.84f);
    p_hazard(c, 0, 18, 10, hh - 18, 14, C_HIVIS, C_INK);
    p_text(c, F_DISPLAY, 18, 4, CV_LEFT, "SUIT", 26, 52, C_CREAM, 1);
    p_text(c, F_DISPLAY, 15, 3, CV_LEFT, "SHIELD", 26, 90, C_TEAL, 1);
    p_text(c, F_TYPE, 15, 2, CV_LEFT, "WASHER RIG 04", 26, 24, C_HIVIS, 0.9f);
    layer_end(&L_vit, c);
  }
  hg_tex(&L_vit, x, y, 1);
  float t = (float)(now_t * 9);
  bool low = s_health < 0.28f;
  uint32_t hc = low ? (sinf(t) > 0 ? C_ALERT : 0xff8a7a) : C_HIVIS;
  bar(x + 112, y + 32, 270, 26, s_health, s_trail, hc, 0xf2ead8);
  segs(x + 112, y + 32, 270, 26, 18);
  bar(x + 112, y + 78, 270, 14, s_shield, s_shield, C_TEAL, C_TEAL);
  segs(x + 112, y + 78, 270, 14, 18);
  if (h->shield <= 0.001f) hg_rect(x + 112, y + 78, 270, 14, 0x07080c, 0.6f);
}

static void play_weapon(const hud_t *h) {
  const float x = 824, y = 556, w = 408, hh = 116;
  uint32_t key = (uint32_t)h->weapon | (uint32_t)(h->snipe_ammo & 255) << 4 | (uint32_t)(h->decoys & 255) << 12 | (uint32_t)h->used_pad << 20;
  cv *c = layer_begin(&L_wpn, (int)w, (int)hh, key);
  if (c) {
    p_plate(c, 0, 0, w, hh, 16, C_INK, 0.84f);
    for (int i = 0; i < 2; i++) {
      float sx = 14 + i * 196, sy = 12, sw = 184, sh = 62;
      bool sel = h->weapon == i;
      cv_fill_hex(c, sel ? C_HIVIS : 0x1d212c, 1); cv_fill_rect(c, sx, sy, sw, sh);
      uint32_t ink = sel ? C_INK : 0x8d93a3;
      p_text(c, F_MONO, 15, 0, CV_LEFT, i ? "2" : "1", sx + 9, sy + 20, ink, 1);
      p_text(c, F_DISPLAY, 20, 2, CV_LEFT, i ? "SPOTTER" : "DE-ICER", sx + 30, sy + 24, ink, 1);
      if (i) {
        char b[16]; snprintf(b, sizeof b, "%02d", h->snipe_ammo);
        p_text(c, F_DISPLAY, 30, 1, CV_LEFT, b, sx + 14, sy + 54, h->snipe_ammo ? ink : C_ALERT, 1);
        for (int k = 0; k < 8; k++) {   /* rounds as little cartridges */
          bool on = k < h->snipe_ammo;
          cv_fill_hex(c, on ? ink : 0x000000, on ? 1.f : 0.3f);
          cv_fill_rect(c, sx + 70 + k * 13, sy + 34, 8, 20);
          if (on) { cv_fill_hex(c, sel ? C_INK : C_HIVIS, 1); cv_fill_rect(c, sx + 70 + k * 13, sy + 34, 8, 5); }
        }
      } else {
        cv_fill_hex(c, 0x000000, sel ? 0.28f : 0.5f); cv_fill_rect(c, sx + 14, sy + 38, 156, 12);   /* spin track */
      }
    }
    p_text(c, F_DISPLAY, 15, 3, CV_LEFT, "DECOY", 16, 102, C_CREAM, 1);
    for (int k = 0; k < 5; k++) {
      bool on = k < h->decoys;
      cv_begin(c); cv_arc(c, 108 + k * 24, 96, 8, 0, 6.2832f, false);
      if (on) { cv_fill_hex(c, C_TEAL, 1); cv_fill(c); } else { cv_stroke_hex(c, 0x586070, 1); cv_line_width(c, 2); cv_stroke(c); }
    }
    if (h->decoys > 5) { char b[8]; snprintf(b, sizeof b, "+%d", h->decoys - 5); p_text(c, F_MONO, 16, 0, CV_LEFT, b, 232, 102, C_TEAL, 1); }
    p_text(c, F_MONO, 15, 0, CV_RIGHT, key_name(h, "Y SWAP", "TAB SWAP"), w - 18, 102, 0x9aa1b2, 1);
    layer_end(&L_wpn, c);
  }
  hg_tex(&L_wpn, x, y, 1);
  if (h->weapon == 0) {
    float s = ds_clamp(h->spin, 0, 1);
    hg_rect(x + 28, y + 12 + 38, 156 * s, 12, s > 0.92f ? C_ALERT : C_INK, 1);
  }
}

static void play_score(const hud_t *h) {
  char b[24]; snprintf(b, sizeof b, "%08d", (int)s_score);
  uint32_t key = hash_str(b, (uint32_t)h->level * 7 + 3);
  cv *c = layer_begin(&L_score, 300, 68, key);
  if (c) {
    p_plate(c, 0, 0, 300, 68, 14, C_INK, 0.84f);
    char lab[24]; snprintf(lab, sizeof lab, "SCORE  L%d", h->level + 1);
    p_text(c, F_DISPLAY, 15, 3, CV_LEFT, lab, 18, 24, C_HIVIS, 1);
    p_text(c, F_DISPLAY, 36, 2, CV_LEFT, b, 16, 58, C_CREAM, 1);
    layer_end(&L_score, c);
  }
  hg_tex(&L_score, 48, 38, 1);
}

static void play_time(const hud_t *h) {
  int tenths = (int)(fmaxf(h->time_s, 0) * 10);
  char b[24]; snprintf(b, sizeof b, "%02d:%02d.%d", tenths / 600, tenths / 10 % 60, tenths % 10);
  cv *c = layer_begin(&L_time, 232, 68, hash_str(b, 11));
  if (c) {
    p_plate(c, 0, 0, 232, 68, 14, C_INK, 0.84f);
    p_text(c, F_DISPLAY, 15, 3, CV_LEFT, "ON THE CLOCK", 18, 24, C_TEAL, 1);
    p_text(c, F_MONO, 34, 0, CV_LEFT, b, 16, 58, C_CREAM, 1);
    layer_end(&L_time, c);
  }
  hg_tex(&L_time, 1280 - 48 - 232, 38, 1);
}

static void play_boss(const hud_t *h) {
  const float x = 360, y = 36, w = 560;
  int ph = h->boss_phase < 1 ? 1 : h->boss_phase;
  cv *c = layer_begin(&L_boss, (int)w, 82, hash_str(h->boss_name, (uint32_t)ph * 31));
  if (c) {
    p_plate(c, 0, 0, w, 82, 16, C_INK, 0.88f);
    p_hazard(c, 0, 0, w, 7, 14, C_ALERT, C_INK);
    p_text(c, F_DISPLAY, 24, 3, CV_LEFT, h->boss_name ? h->boss_name : "MANAGEMENT", 20, 38, C_CREAM, 1);
    char b[16]; snprintf(b, sizeof b, "PHASE %d", ph);
    p_text(c, F_DISPLAY, 16, 3, CV_RIGHT, b, w - 20, 36, C_ALERT, 1);
    cv_fill_hex(c, 0x07080c, 1); cv_fill_rect(c, 20, 48, w - 40, 22);
    layer_end(&L_boss, c);
  }
  hg_tex(&L_boss, x, y, 1);
  bar(x + 20, y + 48, w - 40, 22, s_boss, s_boss_trail, C_ALERT, 0xf2ead8);
  for (int i = 1; i < 3; i++) hg_rect(x + 20 + (w - 40) * i / 3 - 1.5f, y + 48, 3, 22, C_INK, 1);
}

static void play_banner(const hud_t *h) {
  if (!h->banner || h->banner_t <= 0) return;
  uint32_t k = hash_str(h->banner, 5);
  if (k != banner_key) { banner_key = k; banner_t0 = now_t; }
  cv *c = layer_begin(&L_banner, 760, 116, k);
  if (c) {
    cv_font(c, F_DISPLAY, 56); cv_letter_spacing(c, 5);
    float tw = cv_measure(c, h->banner), bw = fminf(tw + 80, 760);
    p_plate(c, 380 - bw / 2, 14, bw, 88, 16, C_INK, 0.86f);
    p_hazard(c, 380 - bw / 2, 14, bw, 7, 14, C_HIVIS, C_INK);
    p_hazard(c, 380 - bw / 2, 95, bw, 7, 14, C_HIVIS, C_INK);
    p_text(c, F_DISPLAY, 56, 5, CV_CENTER, h->banner, 380, 82, C_CREAM, 1);
    layer_end(&L_banner, c);
  }
  float age = (float)(now_t - banner_t0), pop = 1 + 0.35f * expf(-9 * age);
  float a = ds_clamp(h->banner_t / 0.35f, 0, 1);
  float m[6]; mat_place(m, 640, 190, 0, 380, 58, pop, pop);
  hg_tex_m(&L_banner, m, a, 0xffffff);
}

static void play_hints(const hud_t *h) {
  float a = ds_clamp((20 - h->t) / 1.5f, 0, 1);
  if (a <= 0) return;
  cv *c = layer_begin(&L_hints, 800, 30, (uint32_t)h->used_pad);
  if (c) {
    const char *s = h->used_pad ? "A JUMP/GLIDE   RT FIRE   LT SCOPE   B ROLL   X DECOY   Y SWAP"
                                : "SPACE JUMP/GLIDE   J FIRE   K SCOPE   SHIFT ROLL   E DECOY   TAB SWAP";
    cv_font(c, F_MONO, 17); float tw = cv_measure(c, s);
    p_plate(c, 400 - tw / 2 - 14, 2, tw + 28, 26, 8, C_INK, 0.7f);
    p_text(c, F_MONO, 17, 0, CV_CENTER, s, 400, 22, C_CREAM, 1);
    layer_end(&L_hints, c);
  }
  hg_tex(&L_hints, 240, 684, a);
}

/* the centre: reticle spreads with the chain gun's spin, hit marker pops, hurt arc points at the source */
static void play_centre(const hud_t *h) {
  const float cx = 640, cy = 360;
  if (h->weapon == 0) {
    float gap = 11 + ds_clamp(h->spin, 0, 1) * 24;
    for (int i = 0; i < 4; i++) {
      float a = i * DS_PI / 2, dx = sinf(a), dy = -cosf(a), r = gap + 8;
      hg_rect_m(cx + dx * r, cy + dy * r, 4 + 4, 16 + 4, a, C_INK, 0.65f);
      hg_rect_m(cx + dx * r, cy + dy * r, 4, 16, a, C_CREAM, 1);
    }
    hg_rect_m(cx, cy, 8, 8, 0.785f, C_INK, 0.65f);
    hg_rect_m(cx, cy, 4, 4, 0.785f, C_HIVIS, 1);
  } else {   /* scope shot weapon, not scoped: a small open diamond and a prompt to scope */
    for (int i = 0; i < 4; i++) {
      float a = i * DS_PI / 2 + 0.785f;
      hg_rect_m(cx + sinf(a) * 14, cy - cosf(a) * 14, 14, 4, a + DS_PI / 2, C_INK, 0.65f);
      hg_rect_m(cx + sinf(a) * 14, cy - cosf(a) * 14, 12, 2, a + DS_PI / 2, C_CREAM, 1);
    }
  }
  if (h->hit_marker > 0) {
    float k = ds_clamp(h->hit_marker / 0.25f, 0, 1), sc = 1 + 0.5f * k, a = ds_clamp(h->hit_marker / 0.1f, 0, 1);
    uint32_t col = h->hit_weak ? C_HIVIS : C_CREAM;
    float off = (h->hit_weak ? 22 : 17) * sc, len = h->hit_weak ? 16 : 12, th = h->hit_weak ? 5 : 3;
    for (int i = 0; i < 4; i++) {
      float ang = DS_PI / 4 + i * DS_PI / 2, dx = sinf(ang), dy = -cosf(ang);
      hg_rect_m(cx + dx * off, cy + dy * off, th + 3, len + 3, ang, C_INK, 0.7f * a);
      hg_rect_m(cx + dx * off, cy + dy * off, th, len, ang, col, a);
    }
    if (h->hit_weak) {   /* a diamond for weak points */
      float d = 11 * sc;
      for (int i = 0; i < 4; i++) {
        float ang = DS_PI / 4 + i * DS_PI / 2;
        hg_rect_m(cx + sinf(ang) * d * 0.7f, cy - cosf(ang) * d * 0.7f, d * 1.5f, 3, ang + DS_PI / 2, col, a);
      }
    }
  }
}

static void hurt_fx(const hud_t *h) {
  if (h->hurt <= 0) return;
  float k = ds_clamp(h->hurt / 0.5f, 0, 1);
  hg_vignette(C_ALERT, 0.62f * k, 0.45f);
  cv *c = layer_begin(&L_arc, 360, 120, 1);
  if (c) {   /* an arc of a ring around the screen centre, drawn about a point below the layer */
    cv_save(c);
    cv_line_cap(c, CV_CAP_BUTT);
    for (int i = 0; i < 12; i++) {
      float a0 = -0.62f + i * 0.02f, a1 = 0.62f - i * 0.02f;
      cv_begin(c); cv_arc(c, 180, 290, 230 - i * 2.5f, -DS_PI / 2 + a0, -DS_PI / 2 + a1, false);
      cv_stroke_hex(c, C_ALERT, 0.14f + i * 0.07f); cv_line_width(c, 5); cv_stroke(c);
    }
    cv_restore(c);
    layer_end(&L_arc, c);
  }
  float m[6]; mat_place(m, 640, 360, h->hurt_angle, 180, 290, 1, 1);
  hg_tex_m(&L_arc, m, k, 0xffffff);
}

/* ── scope ── */
static void scope_layers(int zoom) {
  cv *c = layer_begin(&L_ret[zoom - 1], 720, 720, (uint32_t)zoom);
  if (c) {
    const float O = 360, R = 330, sp = 34 - zoom * 3;
    cv_line_cap(c, CV_CAP_BUTT);
    /* arms: thin and fine inside, heavy posts outside */
    for (int pass = 0; pass < 2; pass++) {
      cv_stroke_hex(c, pass ? 0xffffff : 0x000000, pass ? 1.f : 0.5f);
      for (int i = 0; i < 4; i++) {
        float a = i * DS_PI / 2, dx = sinf(a), dy = -cosf(a);
        cv_line_width(c, pass ? 2.f : 4.f);
        cv_begin(c); cv_move(c, O + dx * 22, O + dy * 22); cv_line(c, O + dx * 150, O + dy * 150); cv_stroke(c);
        cv_line_width(c, pass ? 5.f : 8.f);
        cv_begin(c); cv_move(c, O + dx * 150, O + dy * 150); cv_line(c, O + dx * (R - 4), O + dy * (R - 4)); cv_stroke(c);
      }
    }
    for (int i = 0; i < 4; i++) {   /* mil-dots */
      float a = i * DS_PI / 2, dx = sinf(a), dy = -cosf(a);
      for (int k = 1; k <= 4; k++) {
        float r = 22 + k * sp; if (r > 146) break;
        cv_begin(c); cv_arc(c, O + dx * r, O + dy * r, k == 4 ? 3.2f : 2.6f, 0, 6.2832f, false);
        cv_fill_hex(c, 0x000000, 0.5f); cv_fill(c);
        cv_begin(c); cv_arc(c, O + dx * r, O + dy * r, 2.f, 0, 6.2832f, false);
        cv_fill_hex(c, 0xffffff, 1); cv_fill(c);
      }
    }
    cv_stroke_hex(c, 0xffffff, 0.8f); cv_line_width(c, 2);   /* bezel ring and its ticks */
    cv_begin(c); cv_arc(c, O, O, R - 2, 0, 6.2832f, false); cv_stroke(c);
    cv_line_width(c, 1.5f);
    for (int a = 0; a < 360; a += 10) {
      float r = a % 30 == 0 ? 20.f : 10.f, rad = a * DS_PI / 180;
      cv_begin(c); cv_move(c, O + sinf(rad) * (R - 3), O - cosf(rad) * (R - 3)); cv_line(c, O + sinf(rad) * (R - 3 - r), O - cosf(rad) * (R - 3 - r)); cv_stroke(c);
    }
    cv_stroke_hex(c, 0xffffff, 0.22f); cv_line_width(c, 1.2f);
    cv_begin(c); cv_arc(c, O, O, 166, 0, 6.2832f, false); cv_stroke(c);
    cv_begin(c); cv_arc(c, O, O, 2.4f, 0, 6.2832f, false); cv_fill_hex(c, 0xffffff, 1); cv_fill(c);
    layer_end(&L_ret[zoom - 1], c);
  }
  c = layer_begin(&L_brk, 96, 96, 1);
  if (c) {
    cv_line_cap(c, CV_CAP_SQUARE);
    for (int pass = 0; pass < 2; pass++) {
      cv_stroke_hex(c, pass ? C_HIVIS : C_INK, pass ? 1.f : 0.8f); cv_line_width(c, pass ? 5.f : 9.f);
      for (int i = 0; i < 4; i++) {
        float sx = i & 1 ? 1.f : -1.f, sy = i & 2 ? 1.f : -1.f, x = 48 + sx * 38, y = 48 + sy * 38;
        cv_begin(c); cv_move(c, x - sx * 20, y); cv_line(c, x, y); cv_line(c, x, y - sy * 20); cv_stroke(c);
      }
    }
    layer_end(&L_brk, c);
  }
  c = layer_begin(&L_weak, 280, 40, 1);
  if (c) {
    p_plate(c, 0, 0, 280, 40, 10, C_HIVIS, 1);
    p_text(c, F_DISPLAY, 24, 4, CV_CENTER, "WEAK POINT", 140, 29, C_INK, 1);
    layer_end(&L_weak, c);
  }
}

/* paint the scope's big layers up front, so the first zoom in is not a hitch */
void hud_init(void) {
  hg_init();
  for (int z = 1; z <= 4; z++) scope_layers(z);
}

static void scope_draw(const hud_t *h) {
  int zoom = (int)ds_clamp((float)h->zoom, 1, 4);
  if (!was_scoped || zoom != last_zoom) { if (!was_scoped) scope_t0 = now_t; }
  was_scoped = true; last_zoom = zoom;
  scope_layers(zoom);
  const float cx = 640, cy = 360;
  float age = (float)(now_t - scope_t0);
  float R = 330 + 520 * (1 - ease_out(age / 0.18f));   /* the lens irises in */
  hg_circle(cx, cy, R, C_ALERT, h->on_enemy ? 0.17f : 0);
  /* the lens fogged with suds until the squeegee has crossed it */
  const float sweep_t = 0.55f, tilt = -0.14f;
  if (age < sweep_t + 0.1f) {
    float k = ease_out(age / sweep_t), bx = 280 + k * 740;
    float dx = 1000 - bx + 400;
    hg_rect_m(bx + dx / 2 + 6, cy, dx, 1000, tilt, 0xdff1f5, 0.5f * (1 - ds_clamp((age - sweep_t) / 0.1f, 0, 1)));
    hg_grad(bx - 55, cy, 110, 1000, tilt, 0xdff1f5, 0.0f, 0.5f);
    hg_rect_m(bx, cy, 16, 1000, tilt, C_INK, 1);
    hg_rect_m(bx + 9, cy, 3, 1000, tilt, C_HIVIS, 1);
  }
  hg_scope(cx, cy, R < 330 ? 330 : R);
  float m[6]; mat_place(m, cx, cy, 0, 360, 360, R > 330 ? R / 330 : 1, R > 330 ? R / 330 : 1);
  hg_tex_m(&L_ret[zoom - 1], m, ds_clamp(age / 0.12f, 0, 1), h->on_enemy ? C_ALERT : 0xffffff);

  float pulse = 0.5f + 0.5f * sinf((float)now_t * 10);
  if (h->on_weak) {
    float s = 1.35f + 0.18f * pulse;
    float mm[6]; mat_place(mm, cx, cy, 0, 48, 48, s, s);
    hg_tex_m(&L_brk, mm, 1, 0xffffff);
    hg_tex(&L_weak, cx - 140, cy + 92, 0.55f + 0.45f * pulse);
  }
  if (h->weak_mark) {
    float s = 0.8f + 0.12f * pulse;
    float mm[6]; mat_place(mm, h->weak_x, h->weak_y, 0, 48, 48, s, s);
    hg_tex_m(&L_brk, mm, 0.9f, 0xffffff);
  }

  /* readouts in the dark corners beside the lens */
  char b[32];
  cv *c = layer_begin(&L_zl, 250, 260, (uint32_t)zoom | (uint32_t)(h->snipe_ammo & 255) << 8);
  if (c) {
    p_plate(c, 0, 0, 250, 260, 16, C_INK, 0.9f);
    p_text(c, F_DISPLAY, 16, 4, CV_LEFT, "MAGNIFY", 20, 36, C_HIVIS, 1);
    snprintf(b, sizeof b, "%dX", 1 << zoom);
    p_text(c, F_DISPLAY, 86, 0, CV_LEFT, b, 18, 128, C_CREAM, 1);
    for (int i = 1; i <= 4; i++) { cv_fill_hex(c, i <= zoom ? C_HIVIS : 0x2a2f3c, 1); cv_fill_rect(c, 20 + (i - 1) * 54, 148, 46, 10); }
    p_text(c, F_DISPLAY, 16, 4, CV_LEFT, "ROUNDS", 20, 204, C_TEAL, 1);
    snprintf(b, sizeof b, "%02d", h->snipe_ammo);
    p_text(c, F_DISPLAY, 44, 2, CV_LEFT, b, 20, 248, h->snipe_ammo ? C_CREAM : C_ALERT, 1);
    p_text(c, F_MONO, 15, 0, CV_RIGHT, key_name(h, "RB/LB", "R/V"), 232, 248, 0x9aa1b2, 1);
    layer_end(&L_zl, c);
  }
  hg_tex(&L_zl, 40, 230, 1);
  int rng = (int)(h->range_m + 0.5f);
  uint32_t kr = (uint32_t)(rng < 0 ? 0 : rng > 9999 ? 9999 : rng) | (uint32_t)h->on_enemy << 16 | (uint32_t)h->on_weak << 17;
  c = layer_begin(&L_zr, 250, 260, kr);
  if (c) {
    p_plate(c, 0, 0, 250, 260, 16, C_INK, 0.9f);
    p_text(c, F_DISPLAY, 16, 4, CV_LEFT, "RANGE", 20, 36, C_HIVIS, 1);
    if (h->range_m > 0.5f) snprintf(b, sizeof b, "%d", rng); else snprintf(b, sizeof b, "---");
    p_text(c, F_DISPLAY, 70, 0, CV_LEFT, b, 18, 118, C_CREAM, 1);
    p_text(c, F_DISPLAY, 24, 3, CV_LEFT, "METRES", 20, 154, C_TEAL, 1);
    if (h->on_enemy) {
      p_plate(c, 20, 178, 210, 54, 10, C_ALERT, 1);
      p_text(c, F_DISPLAY, 26, 3, CV_CENTER, h->on_weak ? "WEAK!" : "TENANT", 125, 217, C_CREAM, 1);
    } else {
      cv_stroke_hex(c, 0x586070, 1); cv_line_width(c, 2); cv_begin(c); cv_rect(c, 21, 179, 208, 52); cv_stroke(c);
      p_text(c, F_DISPLAY, 18, 3, CV_CENTER, "GLASS CLEAN", 125, 213, 0x7b8294, 1);
    }
    layer_end(&L_zr, c);
  }
  hg_tex(&L_zr, 1280 - 40 - 250, 230, 1);
  /* suit bar, so scoping in does not blind you to dying */
  bar(40, 520, 200, 12, s_health, s_trail, s_health < 0.28f && sinf((float)now_t * 9) > 0 ? C_ALERT : C_HIVIS, 0xf2ead8);
  segs(40, 520, 200, 12, 14);
}

/* ── the drop ── */
static void drop_draw(const hud_t *h) {
  float t = (float)now_t;
  /* streaking air */
  for (int i = 0; i < 22; i++) {
    float seed = (float)(i * 37 % 22) / 22.f, ang = i * 0.2856f + seed * 0.4f, sp = 0.9f + 0.9f * seed;
    float ph = fmodf(t * sp + seed * 3.7f, 1.f), r = 180 + ph * ph * 700, len = 40 + ph * 160;
    hg_rect_m(640 + sinf(ang) * r, 360 - cosf(ang) * r * 0.62f, 3, len, ang + 0.0f - (sinf(ang) > 0 ? 0.0f : 0.0f), 0xdff1f5, 0.28f * (1 - ph * 0.4f));
  }
  hg_vignette(C_INK, 0.35f, 0.5f);
  int alt = (int)(h->altitude_m < 0 ? 0 : h->altitude_m);
  char b[32]; fmt_int(b, sizeof b, alt);
  cv *c = layer_begin(&L_dalt, 360, 118, hash_str(b, 21));
  if (c) {
    p_plate(c, 0, 0, 360, 118, 18, C_INK, 0.86f);
    p_hazard(c, 0, 0, 360, 8, 14, C_HIVIS, C_INK);
    p_text(c, F_DISPLAY, 16, 4, CV_LEFT, "ALTITUDE", 22, 40, C_HIVIS, 1);
    cv_font(c, F_DISPLAY, 62);
    p_text(c, F_DISPLAY, 62, 0, CV_LEFT, b, 20, 100, C_CREAM, 1);
    float tw = cv_measure(c, b);
    p_text(c, F_DISPLAY, 26, 2, CV_LEFT, "M", 26 + tw, 100, C_TEAL, 1);
    layer_end(&L_dalt, c);
  }
  hg_tex(&L_dalt, 48, 38, 1);
  char rb[24]; snprintf(rb, sizeof rb, "%02d/%02d", h->rings, h->rings_total);
  c = layer_begin(&L_drings, 250, 118, hash_str(rb, 22) ^ (uint32_t)h->wing_open);
  if (c) {
    p_plate(c, 0, 0, 250, 118, 18, C_INK, 0.86f);
    p_hazard(c, 0, 0, 250, 8, 14, C_TEAL, C_INK);
    p_text(c, F_DISPLAY, 16, 4, CV_LEFT, "RINGS", 22, 40, C_TEAL, 1);
    p_text(c, F_DISPLAY, 56, 0, CV_LEFT, rb, 20, 100, C_CREAM, 1);
    if (h->wing_open) p_text(c, F_DISPLAY, 15, 3, CV_RIGHT, "WING OPEN", 230, 40, C_GOO, 1);
    layer_end(&L_drings, c);
  }
  hg_tex(&L_drings, 1280 - 48 - 250, 38, 1);
  /* a log-scale gauge down the right edge, with a marker for where you are */
  float gy = 190, gh = 380, gx = 1280 - 48 - 16;
  float f = ds_clamp(log10f((float)alt + 1) / 4.f, 0, 1);
  hg_rect(gx, gy, 16, gh, C_INK, 0.8f);
  hg_rect(gx + 3, gy + gh - gh * f, 10, gh * f, C_TEAL, 0.9f);
  hg_rect(gx + 3, gy + gh - gh * log10f(61.f) / 4.f, 10, 3, C_ALERT, 1);   /* the 60 m line */
  hg_rect_m(gx - 8, gy + gh - gh * f, 22, 8, 0, C_HIVIS, 1);
  if (alt < 60 && !h->wing_open) {
    uint32_t k = (uint32_t)h->used_pad + 40;
    c = layer_begin(&L_dwarn, 860, 150, k);
    if (c) {
      p_plate(c, 0, 14, 860, 122, 20, C_INK, 0.9f);
      p_hazard(c, 0, 14, 860, 12, 16, C_ALERT, C_INK);
      p_hazard(c, 0, 124, 860, 12, 16, C_ALERT, C_INK);
      const char *ws = h->used_pad ? "HOLD A TO OPEN THE WING" : "HOLD SPACE TO OPEN THE WING";
      cv_font(c, F_DISPLAY, 60); cv_letter_spacing(c, 4);
      float ww = cv_measure(c, ws), wpx = ww > 800 ? 60 * 800 / ww : 60;
      p_text(c, F_DISPLAY, wpx, 4, CV_CENTER, ws, 430, 92, C_CREAM, 1);
      p_text(c, F_TYPE, 20, 2, CV_CENTER, "THE GROUND IS NOT A CUSTOMER", 430, 116, C_ALERT, 1);
      layer_end(&L_dwarn, c);
    }
    float bl = sinf(t * 12) > -0.3f ? 1.f : 0.45f, pop = 1 + 0.04f * sinf(t * 12);
    float m[6]; mat_place(m, 640, 560, 0, 430, 75, pop, pop);
    hg_tex_m(&L_dwarn, m, bl, 0xffffff);
    hg_vignette(C_ALERT, 0.28f * (0.5f + 0.5f * sinf(t * 12)), 0.4f);
  }
}

void hud_draw(const hud_t *h) {
  double n = ds_now();
  dt_ = (float)ds_clamp((float)(n - now_t), 0, 0.1f);
  now_t = n;
  if (h->mode != HUD_PLAY || !h->scoped) was_scoped = false;
  /* glide the display values toward the real ones */
  s_health = ds_damp(s_health, h->health, 14, dt_);
  s_shield = ds_damp(s_shield, h->shield, 14, dt_);
  s_trail = s_trail > s_health ? fmaxf(s_health, s_trail - 0.35f * dt_) : s_health;
  s_boss = ds_damp(s_boss, h->boss_health, 14, dt_);
  s_boss_trail = s_boss_trail > s_boss ? fmaxf(s_boss, s_boss_trail - 0.25f * dt_) : s_boss;
  if (h->mode != HUD_PLAY && h->mode != HUD_PAUSE) { s_health = s_trail = h->health; s_shield = h->shield; }
  s_score += (h->score - s_score) * ds_clamp(dt_ * 9, 0, 1);
  if (fabsf(h->score - s_score) < 1) s_score = (float)h->score;

  hg_begin();
  switch (h->mode) {
    case HUD_TITLE: scr_title(h, dt_); break;
    case HUD_INTRO: scr_intro(h); break;
    case HUD_DROP: drop_draw(h); break;
    case HUD_PLAY:
      if (h->scoped) { scope_draw(h); hurt_fx(h); play_banner(h); break; }
      play_centre(h);
      hurt_fx(h);
      play_vitals(h); play_weapon(h); play_score(h); play_time(h);
      if (h->boss) play_boss(h);
      play_banner(h);
      play_hints(h);
      break;
    case HUD_PAUSE: scr_pause(h); break;
    case HUD_DEAD: scr_dead(h); break;
    case HUD_CLEAR: scr_clear(h); break;
    case HUD_END: scr_end(h); break;
    case HUD_CINE: break;
  }
  hud_cine_draw(h);   /* captions, letterbox and radio go over everything */
  hg_end();
  t3_renderer_reset_bindings(DS_R);
}
