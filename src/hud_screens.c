/* hud_screens.c: title, level card, pause, dead, clear and credits. Each is a
 * few painted layers (redrawn when a menu row or toggle changes) moved, faded
 * and flashed with GL, so the animation is free. */
#include "hud_int.h"
#include "ds.h"
#include "world.h"

static layer_t L_logo, L_sub, L_tmenu, L_prompt, L_foot;
static layer_t L_card, L_ptitle, L_pmenu, L_dead, L_dprompt, L_clear, L_stamp, L_cprompt, L_cred, L_hold;

static float ease_out(float t) { t = ds_clamp(t, 0, 1); return 1 - (1 - t) * (1 - t) * (1 - t); }
static float blink(float rate) { return 0.55f + 0.45f * sinf((float)ds_now() * rate); }

void scr_dim(float a) { hg_rect(0, 0, DS_W, DS_H, C_INK, a); hg_scan(0.1f); }

static void fmt_time(char *b, size_t n, float s) {
  int t = (int)(fmaxf(s, 0) * 10);
  snprintf(b, n, "%02d:%02d.%d", t / 600, t / 10 % 60, t % 10);
}
static void fmt_int(char *b, size_t n, int v) {
  char t[24]; snprintf(t, sizeof t, "%d", v < 0 ? 0 : v);
  int l = (int)strlen(t), o = 0;
  for (int i = 0; i < l && o < (int)n - 2; i++) { if (i && (l - i) % 3 == 0) b[o++] = ','; b[o++] = t[i]; }
  b[o] = 0;
}

/* a blinking centred prompt in its own small layer */
static void prompt(layer_t *L, const char *s, float cy, float a, uint32_t col, float px) {
  cv *c = layer_begin(L, 900, 70, hash_str(s, (uint32_t)px));
  if (c) { p_text(c, F_DISPLAY, px, 5, CV_CENTER, s, 450, 52, 0x000000, 0.55f); p_text(c, F_DISPLAY, px, 5, CV_CENTER, s, 450, 49, col, 1); layer_end(L, c); }
  hg_tex(L, 640 - 450, cy - 35, a);
}

/* ── title ── */
static void logo_paint(cv *c) {
  cv_font(c, F_DISPLAY, 150); cv_letter_spacing(c, 4);
  float w = cv_measure(c, "DROPSHOT"), px = fminf(168, 150 * 880 / w);
  cv_save(c);
  cv_transform(c, 1, 0, -0.2f, 1, 0.2f * 190 + 20, 0);
  cv_font(c, F_DISPLAY, px); cv_letter_spacing(c, 4); cv_text_align(c, CV_CENTER); cv_text_baseline(c, CV_ALPHABETIC);
  cv_line_join(c, CV_JOIN_ROUND);
  float cx = 500 - 0.2f * 90;
  cv_stroke_hex(c, C_TEAL, 1); cv_line_width(c, 14); cv_stroke_text(c, "DROPSHOT", cx + 11, 201);
  cv_fill_hex(c, C_TEAL, 1); cv_fill_text(c, "DROPSHOT", cx + 11, 201);
  cv_stroke_hex(c, C_INK, 1); cv_line_width(c, 14); cv_stroke_text(c, "DROPSHOT", cx, 190);
  cv_fill_linear(c, 0, 190 - px * 0.7f, 0, 190);
  cv_fill_stop(c, 0, "#ffc46b"); cv_fill_stop(c, 0.5f, "#ff7a1a"); cv_fill_stop(c, 1, "#d94a00");
  cv_fill_text(c, "DROPSHOT", cx, 190);
  cv_restore(c);
  p_cut(c, 0, 190 - px * 0.34f, 1000, 5);   /* the stencil bridge */
}

static void title_bg(const hud_t *h, float dt) {
  (void)dt;
  float t = (float)ds_now();
  hg_vgrad(0, DS_H, C_INK, 0.3f, 0.82f);
  hg_halftone(0, 400, DS_W, 320, C_TEAL, 0.22f, 13, false);
  for (int i = 0; i < 16; i++) {   /* washer fluid running down the glass */
    float s = (float)((i * 7) % 16) / 16.f, x = 40 + i * 80 + s * 30, ph = fmodf(t * (0.25f + 0.35f * s) + s * 5, 1.4f);
    float y = -100 + ph * 900, len = 70 + s * 150;
    hg_grad(x, y - len / 2, len, 3, DS_PI / 2, 0xdff1f5, 0.0f, 0.32f);
  }
  float lt = h->t;
  for (int i = 0; i < 36; i++) {   /* a slow target ring behind the logo */
    float a = i * DS_PI / 18 + t * 0.12f;
    hg_rect_m(640 + sinf(a) * 330, 215 - cosf(a) * 200, 4, i % 3 ? 12 : 26, a, C_CREAM, 0.18f + 0.1f * (i % 3 == 0));
  }
  hg_stripes(0, 0, DS_W, 22, C_HIVIS, 1, C_INK, 1, 48, -t * 0.08f);
  hg_stripes(0, DS_H - 22, DS_W, 22, C_HIVIS, 1, C_INK, 1, 48, t * 0.08f);
  (void)lt;
}

void scr_title(const hud_t *h, float dt) {
  title_bg(h, dt);
  float t = h->t, t2 = (float)ds_now();
  cv *c = layer_begin(&L_logo, 1000, 250, 1);
  if (c) { logo_paint(c); layer_end(&L_logo, c); }
  float off = -460 * expf(-5.5f * t) * cosf(8.f * t);
  float ly = 52 + off;
  hg_tex(&L_logo, 140, ly, 1);
  hg_shine(&L_logo, 140, ly, fmodf(t2, 4.5f) / 4.5f * 1900 - 350, 46, 0.5f);

  c = layer_begin(&L_sub, 900, 48, 1);
  if (c) {
    p_plate(c, 0, 0, 900, 48, 12, C_INK, 0.88f);
    p_hazard(c, 0, 0, 14, 48, 12, C_HIVIS, C_INK);
    p_text(c, F_TYPE, 21, 2, CV_CENTER, "EXTERIOR GLAZING AND ORBITAL PEST REMEDIATION", 457, 32, C_CREAM, 1);
    layer_end(&L_sub, c);
  }
  float sa = ds_clamp((t - 0.5f) / 0.4f, 0, 1);
  hg_tex(&L_sub, 190, 300, sa);

  int n = h->unlocked < 1 ? 1 : h->unlocked > 3 ? 3 : h->unlocked;
  int sel = h->menu_sel;
  uint32_t key = (uint32_t)sel | (uint32_t)n << 4 | (uint32_t)h->music_on << 8 | (uint32_t)h->shake_on << 9 | (uint32_t)h->invert_y << 10 | (uint32_t)h->used_pad << 11;
  /* rows tighten once there are more levels than fit at full size */
  float pitch = n > 3 ? 46.f : 60.f, ph = pitch - 8, fs = n > 3 ? 22.f : 26.f;
  float mh = n > 1 ? n * pitch + 24 + 44 : 44;
  c = layer_begin(&L_tmenu, 760, (int)mh, key);
  if (c) {
    float y = 0;
    if (n > 1) {
      for (int i = 0; i < n; i++, y += pitch) {
        bool on = sel == i;
        p_plate(c, 120, y, 520, ph, 12, on ? C_HIVIS : C_INK, on ? 1.f : 0.82f);
        char b[8]; snprintf(b, sizeof b, "%02d", i + 1);
        p_text(c, F_DISPLAY, fs, 2, CV_LEFT, b, 144, y + ph * 0.69f, on ? C_INK : C_HIVIS, 1);
        p_text(c, F_DISPLAY, fs, 3, CV_LEFT, world_level_name(i), 214, y + ph * 0.69f, on ? C_INK : C_CREAM, 1);
        if (on) p_text(c, F_DISPLAY, fs - 2, 0, CV_RIGHT, h->used_pad ? "A" : "ENTER", 620, y + ph * 0.67f, C_INK, 1);
      }
      y += 24;
    }
    const char *lab[3] = {"MUSIC", "SHAKE", "INVERT Y"};
    bool val[3] = {h->music_on, h->shake_on, h->invert_y};
    for (int k = 0; k < 3; k++) {
      bool on = sel == n + k;
      float x = 8 + k * 248;
      p_plate(c, x, y, 240, 44, 10, on ? C_HIVIS : C_INK, on ? 1.f : 0.82f);
      p_text(c, F_DISPLAY, 17, 2, CV_LEFT, lab[k], x + 16, y + 29, on ? C_INK : C_CREAM, 1);
      p_text(c, F_DISPLAY, 17, 2, CV_RIGHT, val[k] ? "ON" : "OFF", x + 226, y + 29, on ? C_INK : val[k] ? C_GOO : 0x8d93a3, 1);
    }
    layer_end(&L_tmenu, c);
  }
  float ma = ds_clamp((t - 0.8f) / 0.4f, 0, 1);
  float my = n > 1 ? 380 : 612;
  hg_tex(&L_tmenu, 260, my, ma);
  if (n == 1 && ma > 0) {
    bool on = sel == 0;
    const char *s = h->used_pad ? "PRESS A" : "PRESS ENTER";
    prompt(&L_prompt, s, 500, ma * (on ? blink(7) : 0.6f), on ? C_HIVIS : C_CREAM, 66);
  }
  c = layer_begin(&L_foot, 1280, 30, 1);
  if (c) {
    p_text(c, F_MONO, 17, 1, CV_LEFT, "A HEATSYNC LABS GAME", 48, 22, C_CREAM, 0.85f);
    p_text(c, F_MONO, 17, 1, CV_RIGHT, "WORK ORDER 0.1", 1232, 22, C_CREAM, 0.85f);
    layer_end(&L_foot, c);
  }
  hg_tex(&L_foot, 0, 662, ma);
  hg_scan(0.08f);
}

/* ── level card ── */
void scr_intro(const hud_t *h) {
  float t = h->t;
  uint32_t key = hash_str(h->level_name, hash_str(h->level_blurb, (uint32_t)h->level + 90));
  cv *c = layer_begin(&L_card, 900, 270, key);
  if (c) {
    p_plate(c, 0, 0, 900, 270, 24, C_INK, 0.92f);
    p_hazard(c, 0, 0, 900, 12, 18, C_HIVIS, C_INK);
    p_hazard(c, 0, 258, 900, 12, 18, C_HIVIS, C_INK);
    char b[24]; snprintf(b, sizeof b, "LEVEL %d", h->level + 1);
    p_text(c, F_DISPLAY, 24, 8, CV_LEFT, b, 46, 64, C_HIVIS, 1);
    p_text(c, F_MONO, 16, 2, CV_RIGHT, "WORK ORDER APPROVED BY MANAGEMENT", 856, 62, 0x8d93a3, 1);
    const char *nm = h->level_name ? h->level_name : "UNLISTED";
    cv_font(c, F_DISPLAY, 70); cv_letter_spacing(c, 2);
    float w = cv_measure(c, nm), px = w > 810 ? 70 * 810 / w : 70;
    p_text(c, F_DISPLAY, px, 2, CV_LEFT, nm, 46, 140, 0x000000, 0.5f);
    p_text(c, F_DISPLAY, px, 2, CV_LEFT, nm, 44, 136, C_CREAM, 1);
    cv_fill_hex(c, C_TEAL, 1); cv_fill_rect(c, 46, 160, 90, 5);
    if (h->level_blurb) p_wrap(c, h->level_blurb, 46, 202, 800, 28, 0xd7d0bf);
    layer_end(&L_card, c);
  }
  float in = ease_out(t / 0.5f), out = ease_out((t - 2.5f) / 0.5f);
  float x = -940 + in * (940 + 190) - out * 1300;
  float a = 1 - ds_clamp((t - 2.7f) / 0.3f, 0, 1);
  hg_vgrad(380, 720, C_INK, 0.0f, 0.45f * (in - out > 0 ? 1.f : 0.f));
  hg_tex(&L_card, x, 215, a);
  hg_stripes(0, 0, DS_W, 14, C_HIVIS, 0.9f * a, C_INK, 0.9f * a, 40, -t * 0.1f);
  hg_stripes(0, DS_H - 14, DS_W, 14, C_HIVIS, 0.9f * a, C_INK, 0.9f * a, 40, t * 0.1f);
}

/* ── pause ── */
void scr_pause(const hud_t *h) {
  scr_dim(0.66f);
  hg_stripes(0, 0, DS_W, 14, C_HIVIS, 0.9f, C_INK, 0.9f, 40, 0);
  hg_stripes(0, DS_H - 14, DS_W, 14, C_HIVIS, 0.9f, C_INK, 0.9f, 40, 0);
  cv *c = layer_begin(&L_ptitle, 600, 330, 1);
  if (c) {
    p_text(c, F_DISPLAY, 112, 2, CV_LEFT, "ON", 0, 120, 0x000000, 0.5f);
    p_text(c, F_DISPLAY, 112, 2, CV_LEFT, "HOLD", 0, 240, 0x000000, 0.5f);
    p_text(c, F_DISPLAY, 112, 2, CV_LEFT, "ON", -4, 116, C_HIVIS, 1);
    p_text(c, F_DISPLAY, 112, 2, CV_LEFT, "HOLD", -4, 236, C_CREAM, 1);
    p_cut(c, -10, 78, 620, 4); p_cut(c, -10, 198, 620, 4);
    cv_fill_hex(c, C_TEAL, 1); cv_fill_rect(c, 0, 262, 120, 6);
    p_text(c, F_TYPE, 19, 0, CV_LEFT, "Your shift is paused.", 0, 304, 0xd7d0bf, 1);
    p_text(c, F_TYPE, 19, 0, CV_LEFT, "Customers have been told to wait.", 0, 328, 0xd7d0bf, 1);
    layer_end(&L_ptitle, c);
  }
  hg_tex(&L_ptitle, 90, 150, 1);
  int sel = h->menu_sel;
  uint32_t key = (uint32_t)sel | (uint32_t)h->music_on << 8 | (uint32_t)h->shake_on << 9 | (uint32_t)h->invert_y << 10 | (uint32_t)h->used_pad << 11;
  c = layer_begin(&L_pmenu, 520, 560, key);
  if (c) {
    static const char *rows[4] = {"RESUME", "RESTART LEVEL", "SETTINGS", "QUIT TO TITLE"};
    for (int i = 0; i < 4; i++) {
      bool on = sel == i || (i == 2 && sel >= 4);
      float y = i * 68;
      p_plate(c, 0, y, 520, 58, 14, on ? C_HIVIS : C_INK, on ? 1.f : 0.86f);
      p_text(c, F_DISPLAY, 28, 3, CV_LEFT, rows[i], 28, y + 40, on ? C_INK : C_CREAM, 1);
      if (sel == i) p_text(c, F_DISPLAY, 28, 0, CV_RIGHT, ">", 490, y + 40, C_INK, 1);
    }
    const char *lab[3] = {"MUSIC", "SCREEN SHAKE", "INVERT Y"};
    bool val[3] = {h->music_on, h->shake_on, h->invert_y};
    for (int k = 0; k < 3; k++) {
      bool on = sel == 4 + k;
      float y = 288 + k * 52;
      p_plate(c, 40, y, 480, 44, 10, on ? C_TEAL : C_INK, on ? 1.f : 0.78f);
      p_text(c, F_DISPLAY, 20, 2, CV_LEFT, lab[k], 60, y + 30, on ? C_INK : C_CREAM, 1);
      p_text(c, F_DISPLAY, 20, 2, CV_RIGHT, val[k] ? "ON" : "OFF", 498, y + 30, on ? C_INK : val[k] ? C_GOO : 0x8d93a3, 1);
    }
    p_text(c, F_MONO, 17, 0, CV_LEFT, h->used_pad ? "UP/DOWN MOVE   A PICK   START RESUME" : "UP/DOWN MOVE   ENTER PICK   ESC RESUME", 0, 480, 0xa0a6b6, 1);
    layer_end(&L_pmenu, c);
  }
  hg_tex(&L_pmenu, 690, 110, 1);
}

/* ── dead ── */
void scr_dead(const hud_t *h) {
  static const char *LINES[] = {
    "Your contract has been paused.", "Please do not fall near the customers.",
    "Cleaning schedule disrupted. Rescheduling.", "A complaint has been filed on your behalf.",
    "You are now marked as Unavailable.", "Management thanks you for your brief service.",
    "The window remains dirty.", "Gravity has been notified of your concerns."};
  static int count; static float last_t = 99;
  if (h->t < last_t) count++;
  last_t = h->t;
  float k = ds_clamp(h->t / 0.7f, 0, 1);
  scr_dim(0.62f * k);
  hg_vignette(C_ALERT, 0.5f * k, 0.3f);
  const char *line = LINES[count % 8];
  cv *c = layer_begin(&L_dead, 1000, 300, hash_str(line, (uint32_t)count));
  if (c) {
    p_text(c, F_DISPLAY, 128, 4, CV_CENTER, "SHIFT OVER", 500, 140, 0x000000, 0.6f);
    p_text(c, F_DISPLAY, 128, 4, CV_CENTER, "SHIFT OVER", 494, 134, C_CREAM, 1);
    p_cut(c, 0, 134 - 128 * 0.34f, 1000, 4);
    p_hazard(c, 200, 164, 600, 8, 16, C_ALERT, C_INK);
    p_text(c, F_TYPE, 27, 0, CV_CENTER, line, 500, 222, 0xe9dfc9, 1);
    layer_end(&L_dead, c);
  }
  float m[6]; float s = 1 + 0.3f * expf(-8 * h->t);
  mat_place(m, 640, 320, 0, 500, 150, s, s);
  hg_tex_m(&L_dead, m, k, 0xffffff);
  if (h->t > 1.0f) prompt(&L_dprompt, h->used_pad ? "PRESS A TO RETRY FROM CHECKPOINT" : "PRESS ENTER TO RETRY FROM CHECKPOINT", 520, blink(7), C_HIVIS, 36);
}

/* ── clear ── */
void scr_clear(const hud_t *h) {
  scr_dim(0.78f);
  hg_halftone(0, 360, DS_W, 360, C_TEAL, 0.18f, 13, false);
  hg_stripes(0, 0, DS_W, 14, C_HIVIS, 0.9f, C_INK, 0.9f, 40, 0);
  hg_stripes(0, DS_H - 14, DS_W, 14, C_HIVIS, 0.9f, C_INK, 0.9f, 40, 0);
  char kb[24], tb[24], bt[24], sb[24], bb[24], lv[64];
  snprintf(kb, sizeof kb, "%d / %d", h->kills, h->kills_total);
  fmt_time(tb, sizeof tb, h->clear_time_s);
  if (h->best_time_ms > 0) fmt_time(bt, sizeof bt, h->best_time_ms / 1000.f); else snprintf(bt, sizeof bt, "--:--.-");
  fmt_int(sb, sizeof sb, h->score); fmt_int(bb, sizeof bb, h->best_score);
  snprintf(lv, sizeof lv, "LEVEL %d  %s", h->level + 1, h->level_name ? h->level_name : "");
  uint32_t key = hash_str(kb, hash_str(tb, hash_str(bt, hash_str(sb, hash_str(bb, hash_str(lv, 5))))));
  cv *c = layer_begin(&L_clear, 900, 560, key);
  if (c) {
    p_text(c, F_DISPLAY, 24, 5, CV_LEFT, lv, 4, 34, C_HIVIS, 1);
    p_text(c, F_DISPLAY, 104, 2, CV_LEFT, "LEVEL CLEAR", 2, 138, 0x000000, 0.5f);
    p_text(c, F_DISPLAY, 104, 2, CV_LEFT, "LEVEL CLEAR", 0, 132, C_CREAM, 1);
    p_cut(c, -4, 132 - 104 * 0.34f, 920, 4);
    p_text(c, F_TYPE, 22, 0, CV_LEFT, "Inspection passed. Minor residue was noted.", 4, 172, 0xd7d0bf, 1);
    const char *lab[4] = {"KILLS", "TIME", "SCORE", "BEST"};
    const char *val[4] = {kb, tb, sb, bb};
    for (int i = 0; i < 4; i++) {
      float y = 200 + i * 86;
      p_plate(c, 0, y, 760, 74, 14, C_INK, 0.86f);
      cv_fill_hex(c, i == 3 ? C_TEAL : C_HIVIS, 1); cv_fill_rect(c, 0, y + 14, 8, 60);
      p_text(c, F_DISPLAY, 24, 5, CV_LEFT, lab[i], 32, y + 48, C_CREAM, 1);
      p_text(c, F_DISPLAY, 46, 1, CV_RIGHT, val[i], 736, y + 54, i == 3 ? C_TEAL : C_CREAM, 1);
      if (i == 1) {
        char b[40]; snprintf(b, sizeof b, "BEST %s", bt);
        p_text(c, F_MONO, 16, 0, CV_LEFT, b, 200, y + 46, 0x8d93a3, 1);
      }
    }
    layer_end(&L_clear, c);
  }
  float rv[5] = {0.1f, 0.5f, 0.8f, 1.1f, 1.4f};
  float a0 = ds_clamp((h->t - rv[0]) / 0.3f, 0, 1);
  hg_sub(&L_clear, 0, 0, 900, 190, 190 - (1 - a0) * 60, 60, a0);
  for (int i = 0; i < 4; i++) {
    float a = ds_clamp((h->t - rv[i + 1]) / 0.3f, 0, 1);
    hg_sub(&L_clear, 0, 200 + i * 86, 760, 74, 190 + (1 - ease_out(a)) * 220, 265 + i * 86, a);
  }
  if (h->new_best && h->t > 2.0f) {
    c = layer_begin(&L_stamp, 420, 120, 1);
    if (c) {
      cv_stroke_hex(c, C_HIVIS, 1); cv_line_width(c, 8); cv_begin(c); cv_round_rect(c, 14, 14, 392, 92, 10); cv_stroke(c);
      p_text(c, F_DISPLAY, 62, 4, CV_CENTER, "NEW BEST", 210, 82, C_HIVIS, 1);
      p_cut(c, 0, 60, 420, 3);
      layer_end(&L_stamp, c);
    }
    float k = h->t - 2.0f, s = 1 + 1.4f * expf(-12 * k);
    float m[6]; s *= 0.8f; mat_place(m, 1090, 330, -0.14f, 210, 60, s, s);
    hg_tex_m(&L_stamp, m, ds_clamp(k / 0.08f, 0, 1), 0xffffff);
  }
  if (h->t > 2.2f) prompt(&L_cprompt, h->used_pad ? "PRESS A TO CONTINUE" : "PRESS ENTER TO CONTINUE", 668, blink(7), C_HIVIS, 34);
}

/* ── credits ── */
void scr_end(const hud_t *h) {
  scr_dim(0.82f);
  hg_halftone(0, 0, DS_W, 300, C_TEAL, 0.14f, 13, true);
  static const char *L[] = {
    "#THANK YOU FOR CHOOSING", "!DROPSHOT", "", "@A HEATSYNC LABS GAME", "",
    "#STARRING", "JUNO KADE, WASHER OF WINDOWS", "THE TENANTS, NOW ON NOTICE", "",
    "#CHARACTER AND SHIP MODELS", "CC0 BY QUATERNIUS AND KENNEY", "",
    "#FONTS UNDER THE OPEN FONT LICENSE", "ARCHIVO BLACK, IBM PLEX MONO, JETBRAINS MONO", "",
    "#MUSIC AND SOUND", "SYNTHESISED IN CODE, NO SAMPLES", "",
    "NO WINDOWS WERE HARMED.", "SOME WERE CLEANED."};
  cv *c = layer_begin(&L_cred, 960, 700, 1);
  if (c) {
    float y = 40;
    for (size_t i = 0; i < sizeof L / sizeof *L; i++) {
      const char *s = L[i];
      if (!*s) { y += 16; continue; }
      if (*s == '!') { p_text(c, F_DISPLAY, 92, 6, CV_CENTER, s + 1, 480, y + 78, C_HIVIS, 1); y += 100; }
      else if (*s == '#') { p_text(c, F_DISPLAY, 22, 5, CV_CENTER, s + 1, 480, y + 22, C_TEAL, 1); y += 36; }
      else if (*s == '@') { p_text(c, F_DISPLAY, 28, 5, CV_CENTER, s + 1, 480, y + 26, C_CREAM, 1); y += 40; }
      else { p_text(c, F_TYPE, 24, 0, CV_CENTER, s, 480, y + 22, 0xe9dfc9, 1); y += 34; }
    }
    layer_end(&L_cred, c);
  }
  float y = 720 - h->t * 58;
  if (y < -24) y = -24;   /* the roll stops with the last line on screen */
  hg_tex(&L_cred, 160, y, 1);
  hg_stripes(0, 0, DS_W, 14, C_HIVIS, 1, C_INK, 1, 40, 0);
  hg_stripes(0, DS_H - 14, DS_W, 14, C_HIVIS, 1, C_INK, 1, 40, 0);
  if (h->t > 14) prompt(&L_hold, h->used_pad ? "PRESS A" : "PRESS ENTER", 678, blink(7), C_CREAM, 26);
}
