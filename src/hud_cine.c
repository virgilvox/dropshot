/* hud_cine.c: the cutscene overlay (letterbox bars, the typed caption plate,
 * the skip ring) and the radio caption shown during play. Text is painted once
 * per line of dialogue; the typewriter reveals it by sub-rect, so a typed
 * caption costs no repaint per character. hud_draw calls this last. */
#include "hud_int.h"

#define BAR_H 92.f              /* a 2.39:1 frame at 1280x720 */
#define CAP_X 160.f
#define CAP_W 960.f
#define CAP_Y 546.f
#define CAP_H 118.f
#define CAP_PX 22.f
#define CAP_LH 30
#define CAP_MAXLINES 3
#define CAP_TEXT_X 40.f

static layer_t L_plate, L_cap, L_hint, L_ricon, L_rwave, L_rtext;

/* wrapped caption: where each line starts and how far along it each character ends */
static int cap_ln_start[CAP_MAXLINES + 1], cap_ln_n;
static float cap_cum[CAP_MAXLINES][140];

static float ease_io(float t) { t = ds_clamp(t, 0, 1); return t * t * (3 - 2 * t); }

/* speaker colours (STORY.md): the plate takes on the speaker's colour */
static uint32_t speaker_col(const char *who) {
  if (!who) return C_CREAM;
  if (!strncmp(who, "MARGUERITE", 10)) return C_TEAL;
  if (!strncmp(who, "JUNO", 4)) return C_HIVIS;
  if (!strncmp(who, "THE SUPERINTENDENT", 18)) return C_ALERT;
  if (!strncmp(who, "BRUTE", 5)) return 0xf2b84b;
  return C_CREAM;   /* Tenant forms: FORM 27-B and the like */
}

/* ── the typed caption ── */
static void cap_paint(cv *c, const char *s) {
  cv_font(c, F_TYPE, CAP_PX); cv_letter_spacing(c, 0.4f);
  char line[200]; int ll = 0, n = (int)strlen(s), i = 0;
  cap_ln_n = 0; cap_ln_start[0] = 0;
  line[0] = 0;
  /* greedy word wrap by measured width */
  while (i < n && cap_ln_n < CAP_MAXLINES) {
    int j = i;
    while (j < n && s[j] != ' ') j++;
    int wl = j - i;
    char trial[200];
    snprintf(trial, sizeof trial, "%.*s%s%.*s", ll, line, ll ? " " : "", wl, s + i);
    if (ll && cv_measure(c, trial) > CAP_W - 2 * CAP_TEXT_X) {
      cap_ln_n++;
      if (cap_ln_n >= CAP_MAXLINES) break;
      cap_ln_start[cap_ln_n] = i;
      ll = 0; line[0] = 0;
      continue;
    }
    snprintf(line, sizeof line, "%s", trial); ll = (int)strlen(line);
    i = j; while (i < n && s[i] == ' ') i++;
  }
  if (cap_ln_n < CAP_MAXLINES) cap_ln_n++;
  cap_ln_start[cap_ln_n] = n;
  for (int k = 0; k < cap_ln_n; k++) {
    int a = cap_ln_start[k], b = cap_ln_start[k + 1];
    while (b > a && s[b - 1] == ' ') b--;
    char buf[200]; int len = b - a > 199 ? 199 : b - a;
    memcpy(buf, s + a, (size_t)len); buf[len] = 0;
    float y = (float)k * CAP_LH + 22;
    p_text(c, F_TYPE, CAP_PX, 0.4f, CV_LEFT, buf, 0, y, C_CREAM, 1);
    cap_cum[k][0] = 0;
    cv_font(c, F_TYPE, CAP_PX); cv_letter_spacing(c, 0.4f);
    for (int m = 1; m <= len && m < 140; m++) { char sv = buf[m]; buf[m] = 0; cap_cum[k][m] = cv_measure(c, buf); buf[m] = sv; }
  }
}

static void plate_paint(cv *c, const char *who, uint32_t col, bool form) {
  p_plate(c, 0, 0, CAP_W, CAP_H, 16, form ? 0x1a1814 : C_INK, 0.9f);
  p_hazard(c, 0, 16, 12, CAP_H - 16, 14, col, form ? 0x1a1814 : C_INK);
  cv_fill_hex(c, col, 1); cv_fill_rect(c, 12, 0, CAP_W - 28, 3);
  if (form) {   /* a paper form: ruled lines and a stamp box */
    cv_fill_hex(c, C_CREAM, 0.12f);
    for (int k = 0; k < 2; k++) cv_fill_rect(c, CAP_TEXT_X, 66 + k * CAP_LH, CAP_W - 2 * CAP_TEXT_X, 1.5f);
  }
  p_text(c, F_DISPLAY, 20, 4, CV_LEFT, who ? who : "", CAP_TEXT_X, 38, col, 1);
}

static void caption(const hud_t *h) {
  static char last_who[40], last_txt[260];
  static float a;
  float dt = 1.f / 60;
  bool on = h->cap_text && h->cap_text[0] && h->cap_chars > 0;
  if (on) {
    snprintf(last_who, sizeof last_who, "%s", h->cap_speaker ? h->cap_speaker : "");
    snprintf(last_txt, sizeof last_txt, "%s", h->cap_text);
  }
  a = ds_damp(a, on ? 1.f : 0.f, on ? 22.f : 14.f, dt);
  if (a < 0.01f || !last_txt[0]) return;
  uint32_t col = speaker_col(last_who);
  bool form = col == C_CREAM;
  cv *c = layer_begin(&L_plate, (int)CAP_W, (int)CAP_H, hash_str(last_who, 17));
  if (c) { plate_paint(c, last_who, col, form); layer_end(&L_plate, c); }
  c = layer_begin(&L_cap, (int)CAP_W - 80, CAP_LH * CAP_MAXLINES, hash_str(last_txt, 41));
  if (c) { cap_paint(c, last_txt); layer_end(&L_cap, c); }
  float rise = (1 - a) * 14;
  float px = CAP_X, py = CAP_Y + rise;
  hg_tex(&L_plate, px, py, a);
  int total = (int)strlen(last_txt), shown = on ? (h->cap_chars > total ? total : h->cap_chars) : total;
  float cx = 0, cy = 0; bool cursor = false;
  for (int k = 0; k < cap_ln_n; k++) {
    int a0 = cap_ln_start[k], b0 = cap_ln_start[k + 1];
    int len = b0 - a0; while (len > 0 && last_txt[a0 + len - 1] == ' ') len--;
    int m = shown - a0; if (m <= 0) break;
    if (m > len) m = len;
    int sw = (int)ceilf(cap_cum[k][m < 139 ? m : 139]) + 2;
    if (sw > (int)CAP_W - 80) sw = (int)CAP_W - 80;
    hg_sub(&L_cap, 0, k * CAP_LH, sw, CAP_LH, px + CAP_TEXT_X, py + 52 + k * CAP_LH, a);
    cx = px + CAP_TEXT_X + cap_cum[k][m < 139 ? m : 139] + 3; cy = py + 52 + k * CAP_LH;
    cursor = shown < total && shown >= a0;
  }
  if (cursor) hg_rect(cx, cy + 4, 11, 20, col, a * (0.55f + 0.45f * sinf((float)ds_now() * 18)));
}

/* ── the skip ring ── */
static void skip_ring(const hud_t *h, float t_cine) {
  float cx = DS_W - 66, cy = DS_H - BAR_H * 0.5f, r = 17;
  float a = h->skip > 0.02f ? 1.f : 0.5f;
  a *= ds_clamp(t_cine * 3, 0, 1);
  const char *s = h->used_pad ? "HOLD START TO SKIP" : "HOLD ENTER TO SKIP";
  cv *c = layer_begin(&L_hint, 300, 28, hash_str(s, 9));
  if (c) { p_text(c, F_TYPE, 16, 2, CV_RIGHT, s, 296, 20, C_CREAM, 1); layer_end(&L_hint, c); }
  hg_tex(&L_hint, cx - r - 20 - 300, cy - 14, a * 0.8f);
  hg_circle(cx, cy, r + 5, C_INK, 0.55f * a);
  int n = 32, lit = (int)(h->skip * n + 0.5f);
  for (int i = 0; i < n; i++) {
    float ang = (float)i * 2 * DS_PI / n;
    bool on = i < lit;
    hg_rect_m(cx + sinf(ang) * r, cy - cosf(ang) * r, 3.4f, on ? 9.f : 6.f, ang, on ? C_HIVIS : C_CREAM, on ? 1.f : 0.35f * a);
  }
  if (h->skip > 0.02f) hg_circle(cx, cy, 3.5f, C_HIVIS, 1);
}

/* ── the radio during play ── */
static void radio_icon(cv *c) {
  /* a field radio: body, speaker grille, dial and an antenna */
  cv_fill_hex(c, C_TEAL, 1);
  cv_round_rect(c, 4, 18, 36, 24, 4); cv_fill(c);
  cv_fill_hex(c, C_INK, 1);
  cv_begin(c); cv_arc(c, 15, 30, 7, 0, 6.3f, false); cv_fill(c);
  cv_fill_hex(c, C_TEAL, 1);
  for (int k = 0; k < 3; k++) cv_fill_rect(c, 12, 26 + k * 4.f, 6, 1.6f);
  cv_fill_hex(c, C_INK, 1); cv_fill_rect(c, 26, 23, 10, 4);
  cv_begin(c); cv_arc(c, 31, 35, 2.6f, 0, 6.3f, false); cv_fill(c);
  cv_stroke_hex(c, C_TEAL, 1); cv_line_width(c, 2.4f);
  cv_begin(c); cv_move(c, 32, 18); cv_line(c, 40, 4); cv_stroke(c);
  cv_fill_hex(c, C_HIVIS, 1); cv_begin(c); cv_arc(c, 40, 4, 2.6f, 0, 6.3f, false); cv_fill(c);
}
static void radio_waves(cv *c) {
  cv_stroke_hex(c, C_HIVIS, 1); cv_line_width(c, 2.4f);
  for (int k = 0; k < 2; k++) {
    float r = 7 + k * 6;
    cv_begin(c); cv_arc(c, 40, 4, r, -1.2f, 0.3f, false); cv_stroke(c);
  }
}

static void radio(const hud_t *h) {
  static const char *last; static double t0;
  if (!h->radio_text || h->radio_t <= 0) { last = NULL; return; }
  double now = ds_now();
  if (h->radio_text != last) { last = h->radio_text; t0 = now; }
  const float PW = 540, X = 48;
  float el = (float)(now - t0);
  float slide = ease_io(el / 0.28f), a = fminf(slide, ds_clamp(h->radio_t / 0.6f, 0, 1));
  uint32_t col = speaker_col(h->radio_speaker);
  /* word wrap at 20px mono into at most 3 lines */
  uint32_t key = hash_str(h->radio_text, hash_str(h->radio_speaker, 3));
  const float TX = 70, TW = PW - TX - 16;
  static int nl = 1;
  cv *c = layer_begin(&L_rtext, (int)PW, 128, key);
  if (c) {
    char lines[3][120]; nl = 0; lines[0][0] = 0;
    int ll = 0; const char *p = h->radio_text;
    cv_font(c, F_TYPE, 19); cv_letter_spacing(c, 0.3f);
    while (*p && nl < 3) {
      char word[48]; int k = 0;
      while (*p && *p != ' ' && k < 47) word[k++] = *p++;
      word[k] = 0; while (*p == ' ') p++;
      char trial[200]; snprintf(trial, sizeof trial, ll ? "%s %s" : "%s%s", lines[nl], word);
      if (ll && cv_measure(c, trial) > TW) { if (++nl >= 3) break; snprintf(lines[nl], sizeof lines[nl], "%s", word); ll = 1; }
      else { snprintf(lines[nl], sizeof lines[nl], "%s", trial); ll = 1; }
    }
    if (nl < 3) nl++;
    float ph = 38 + nl * 25 + 10;
    p_plate(c, 0, 0, PW, ph, 14, C_INK, 0.88f);
    p_hazard(c, 0, 14, 8, ph - 14, 12, col, C_INK);
    cv_fill_hex(c, col, 1); cv_fill_rect(c, 8, 0, PW - 22, 2.5f);
    p_text(c, F_DISPLAY, 15, 3, CV_LEFT, h->radio_speaker ? h->radio_speaker : "RADIO", TX, 27, col, 1);
    for (int k = 0; k < nl; k++) p_text(c, F_TYPE, 19, 0.3f, CV_LEFT, lines[k], TX, 54 + k * 25.f, C_CREAM, 1);
    layer_end(&L_rtext, c);
  }
  c = layer_begin(&L_ricon, 48, 48, 1);
  if (c) { radio_icon(c); layer_end(&L_ricon, c); }
  c = layer_begin(&L_rwave, 48, 48, 1);
  if (c) { radio_waves(c); layer_end(&L_rwave, c); }
  float ph = 38 + nl * 25 + 10;
  float y = 540 - ph + (1 - slide) * 10, x = X - (1 - slide) * 40;
  hg_tex(&L_rtext, x, y, a);
  hg_tex(&L_ricon, x + 14, y + 22, a);
  hg_tex(&L_rwave, x + 14, y + 22, a * (0.35f + 0.65f * (0.5f + 0.5f * sinf((float)now * 7))));
}

void hud_cine_draw(const hud_t *h) {
  /* cine runs 0..1 for the bars; 1..2 is the same bars plus a fade to black (hud_t has no fade field) */
  float cine = ds_clamp(h->cine, 0, 2), bars = ease_io(fminf(cine, 1)), fade = fmaxf(0, cine - 1);
  static float cine_t;
  if (h->mode == HUD_CINE) {
    cine_t += 1.f / 60;
    if (h->t < 0.05f) cine_t = 0;
    float b = BAR_H * bars;
    hg_vignette(C_INK, 0.38f * bars, 0.55f);
    hg_rect(0, 0, DS_W, b, 0x000000, 1);
    hg_rect(0, DS_H - b, DS_W, b, 0x000000, 1);
    if (b > 4) {
      hg_rect(0, b, DS_W, 2, C_TEAL, 0.35f * bars);
      hg_rect(0, DS_H - b - 2, DS_W, 2, C_TEAL, 0.35f * bars);
    }
    if (bars > 0.95f) caption(h);
    if (bars > 0.5f) skip_ring(h, cine_t);
    if (fade > 0) hg_rect(0, 0, DS_W, DS_H, 0x000000, fade);
  } else radio(h);
}
