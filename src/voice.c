/* voice.c: speech for radio calls, dialogue and alerts. tts.c (Flite) makes
 * one plain voice; each speaker is that voice with its own pitch and pace plus
 * a little DSP (radio band and squelch, PA ring and echo, a homesick wobble).
 * Finished lines are cached as 16 bit PCM at the output rate, keyed by speaker
 * and text, so repeats and prepared lines start at once. */
#include "voice.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "tts.h"

#define PI_F 3.14159265f
#define CACHE_MAX 192
#define CACHE_BYTES (24u * 1024u * 1024u)   /* oldest lines go first past this */
#define TEXT_MAX 512
#define TARGET_RMS 0.17f                     /* every speaker lands near the same loudness */
#define VOICE_GAIN 0.9f

typedef struct { float pitch, range, stretch; } vo_style;
static const vo_style STYLE[VO_COUNT] = {
  [VO_MARGUERITE] = { 1.00f, 1.00f, 0.95f },
  [VO_JUNO]       = { 0.86f, 0.70f, 1.08f },   /* low and flat: tired */
  [VO_SUPER]      = { 0.68f, 0.45f, 1.20f },   /* slow and level, a machine that has said this before */
  [VO_TENANT]     = { 1.38f, 1.35f, 1.06f },
  [VO_ALERT]      = { 1.00f, 0.55f, 0.82f },
};

typedef struct {
  vo_speaker sp;
  char *text;
  int16_t *pcm;
  int n;
  unsigned seq;
} entry;

static uint32_t out_rate = 48000;
static bool enabled = true;
static entry cache[CACHE_MAX];
static unsigned cache_seq;
static size_t cache_bytes;
static entry *cur;           /* the line playing, NULL when idle */
static int pos;
static int fade = -1;        /* samples of fade-out left after voice_stop, -1 when not stopping */
static float rng = 1.f;

static float noise(void) {   /* xorshift, deterministic so a line always sounds the same */
  uint32_t x = (uint32_t)(rng * 2147483647.f);
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  rng = (float)(x & 0x7fffffff) / 2147483647.f + 1e-6f;
  return rng * 2.f - 1.f;
}

/* ── filters: RBJ biquads in direct form 1 ── */
typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; } biq;
static biq bq_make(int kind, float f, float q, float rate) {   /* 0 lowpass, 1 highpass */
  float w = 2.f * PI_F * fminf(f, rate * 0.45f) / rate, c = cosf(w), a = sinf(w) / (2.f * q), a0 = 1.f + a;
  biq b = {0};
  if (kind == 0) { b.b0 = (1.f - c) / 2.f; b.b1 = 1.f - c; b.b2 = b.b0; }
  else { b.b0 = (1.f + c) / 2.f; b.b1 = -(1.f + c); b.b2 = b.b0; }
  b.b0 /= a0; b.b1 /= a0; b.b2 /= a0; b.a1 = -2.f * c / a0; b.a2 = (1.f - a) / a0;
  return b;
}
static void bq_run(biq *b, float *x, int n) {
  for (int i = 0; i < n; i++) {
    float y = b->b0 * x[i] + b->b1 * b->x1 + b->b2 * b->x2 - b->a1 * b->y1 - b->a2 * b->y2;
    b->x2 = b->x1; b->x1 = x[i]; b->y2 = b->y1; b->y1 = y; x[i] = y;
  }
}
static void band(float *x, int n, float lo, float hi, float rate) {   /* two poles each side */
  for (int k = 0; k < 2; k++) {
    biq h = bq_make(1, lo, 0.707f, rate), l = bq_make(0, hi, 0.707f, rate);
    bq_run(&h, x, n); bq_run(&l, x, n);
  }
}

/* windowed-sinc resample, so the 16 kHz voice does not alias or image at the output rate.
 * The kernel is tabulated once (sin per tap per sample is far too slow in wasm). */
#define RS_A 8
#define RS_RES 256
static float rs_k[2 * RS_A * RS_RES + 2];
static void rs_table(void) {
  if (rs_k[RS_A * RS_RES] != 0.f) return;
  for (int i = 0; i <= 2 * RS_A * RS_RES; i++) {
    double d = (double)i / RS_RES - RS_A, pd = 3.14159265358979 * d;
    rs_k[i] = (float)(d == 0 ? 1.0 : sin(pd) / pd * (sin(pd / RS_A) / (pd / RS_A)));
  }
  rs_k[2 * RS_A * RS_RES + 1] = 0.f;
}
static float *resample(const int16_t *in, int n, int from, int to, int *outn) {
  int m = (int)((double)n * to / from) + 1;
  float *o = (float *)malloc((size_t)m * sizeof(float));
  if (!o) return NULL;
  rs_table();
  double step = (double)from / to;
  float fc = from < to ? 1.f : (float)to / from;
  int span = (int)ceilf(RS_A / fc);   /* taps reach further when downsampling */
  for (int i = 0; i < m; i++) {
    double t = i * step;
    int c = (int)floor(t);
    float acc = 0;
    for (int j = c - span + 1; j <= c + span; j++) {
      if (j < 0 || j >= n) continue;
      float u = ((float)(t - j) * fc + RS_A) * RS_RES;
      if (u < 0 || u >= 2 * RS_A * RS_RES) continue;
      int k = (int)u; float f = u - k;
      acc += in[j] * (rs_k[k] + (rs_k[k + 1] - rs_k[k]) * f);
    }
    o[i] = acc * fc * (1.f / 32768.f);
  }
  *outn = m;
  return o;
}

static void add_click(float *x, int total, int at, float ms, float amp, float rate) {
  int len = (int)(ms * 0.001f * rate);
  for (int i = 0; i < len && at + i < total; i++) x[at + i] += noise() * amp * expf(-5.f * i / len);
}

/* colour a resampled line in place: the buffer holds lead + speech + tail samples. */
static void colour(vo_speaker sp, float *x, int total, int lead, int speech, float rate) {
  switch (sp) {
    case VO_MARGUERITE: {
      /* radio: soft squelch click in, a longer one out with a breath of hiss, band limited, a touch of drive */
      add_click(x, total, 0, 14, 0.35f, rate);
      add_click(x, total, lead + speech + (int)(0.012f * rate), 22, 0.28f, rate);
      add_click(x, total, lead + speech + (int)(0.034f * rate), 70, 0.05f, rate);
      band(x, total, 300.f, 3400.f, rate);
      for (int i = 0; i < total; i++) x[i] = tanhf(x[i] * 2.4f) * 0.55f;
      biq l = bq_make(0, 3600.f, 0.707f, rate); bq_run(&l, x, total);
      break; }
    case VO_JUNO: {   /* close and clean: a little warmth under the voice, no band limit */
      biq l = bq_make(0, 220.f, 0.707f, rate);
      for (int i = 0; i < total; i++) { float lp = x[i]; bq_run(&l, &lp, 1); x[i] += 0.35f * lp; }
      biq h = bq_make(1, 70.f, 0.707f, rate); bq_run(&h, x, total);
      break; }
    case VO_SUPER: {  /* PA horn: narrow band, metallic ring modulation, then a short slap-back echo */
      band(x, total, 260.f, 5200.f, rate);
      float ph = 0, dph = 2.f * PI_F * 95.f / rate;
      for (int i = 0; i < total; i++) { x[i] = x[i] * 0.4f + x[i] * sinf(ph) * 0.75f; ph += dph; }
      int d = (int)(0.115f * rate), d2 = (int)(0.19f * rate);
      for (int i = d; i < total; i++) x[i] += 0.36f * x[i - d] + (i >= d2 ? 0.14f * x[i - d2] : 0.f);
      break; }
    case VO_TENANT: { /* a gentle wobble: a short delay line swept slowly, about +-3 percent in pitch */
      float *src = (float *)malloc((size_t)total * sizeof(float));
      if (!src) break;
      memcpy(src, x, (size_t)total * sizeof(float));
      float depth = 0.0011f * rate, base = depth + 2.f;
      for (int i = 0; i < total; i++) {
        float t = (float)i - (base + depth * sinf(2.f * PI_F * 5.2f * i / rate));
        int k = (int)floorf(t); float f = t - k;
        float a = k >= 0 && k < total ? src[k] : 0.f, b = k + 1 >= 0 && k + 1 < total ? src[k + 1] : 0.f;
        x[i] = a + (b - a) * f;
      }
      free(src);
      break; }
    default: {        /* alert: neutral and clean, just trim the rumble */
      biq h = bq_make(1, 120.f, 0.707f, rate); bq_run(&h, x, total);
      break; }
  }
}

static void entry_free(entry *e) {
  if (!e->pcm) return;
  cache_bytes -= (size_t)e->n * sizeof(int16_t);
  free(e->pcm); free(e->text);
  memset(e, 0, sizeof *e);
}

static entry *entry_find(vo_speaker s, const char *text) {
  for (int i = 0; i < CACHE_MAX; i++)
    if (cache[i].pcm && cache[i].sp == s && !strcmp(cache[i].text, text)) return &cache[i];
  return NULL;
}

/* make room: drop the oldest line that is not playing, until a slot and the bytes are free */
static entry *entry_slot(size_t need) {
  for (;;) {
    entry *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < CACHE_MAX; i++) {
      entry *e = &cache[i];
      if (!e->pcm) { if (!free_slot) free_slot = e; }
      else if (e != cur && (!oldest || e->seq < oldest->seq)) oldest = e;
    }
    if (free_slot && cache_bytes + need <= CACHE_BYTES) return free_slot;
    if (!oldest) return free_slot;
    entry_free(oldest);
  }
}

static entry *synth_line(vo_speaker s, const char *text) {
  if (s < 0 || s >= VO_COUNT || !text || !*text) return NULL;
  entry *hit = entry_find(s, text);
  if (hit) return hit;
  char norm[TEXT_MAX];
  tts_normalise(text, norm, sizeof norm);
  if (!*norm) return NULL;
  const vo_style *st = &STYLE[s];
  int n16 = 0;
  int16_t *raw = tts_synth(norm, st->pitch, st->range, st->stretch, &n16);
  if (!raw) return NULL;
  int rate_in = tts_rate(), speech = 0;
  float *sp = resample(raw, n16, rate_in, (int)out_rate, &speech);
  free(raw);
  if (!sp) return NULL;
  float rate = (float)out_rate;
  int lead = (int)((s == VO_MARGUERITE ? 0.05f : 0.01f) * rate);
  float tail_s = s == VO_SUPER ? 0.7f : s == VO_MARGUERITE ? 0.16f : 0.04f;
  int total = lead + speech + (int)(tail_s * rate);
  float *x = (float *)calloc((size_t)total, sizeof(float));
  if (!x) { free(sp); return NULL; }
  memcpy(x + lead, sp, (size_t)speech * sizeof(float));
  free(sp);
  colour(s, x, total, lead, speech, rate);
  /* level to a common loudness, never past full scale, and de-click the ends */
  double e = 0; float pk = 1e-6f;
  for (int i = 0; i < total; i++) { e += (double)x[i] * x[i]; if (fabsf(x[i]) > pk) pk = fabsf(x[i]); }
  float rms = (float)sqrt(e / (speech > 0 ? speech : 1)), g = fminf(0.95f / pk, TARGET_RMS / (rms + 1e-6f));
  int ramp = (int)(0.004f * rate);
  int16_t *pcm = (int16_t *)malloc((size_t)total * sizeof(int16_t));
  if (!pcm) { free(x); return NULL; }
  for (int i = 0; i < total; i++) {
    float v = x[i] * g;
    if (i < ramp) v *= (float)i / ramp;
    if (total - 1 - i < ramp) v *= (float)(total - 1 - i) / ramp;
    pcm[i] = (int16_t)lrintf(fmaxf(-1.f, fminf(1.f, v)) * 32767.f);
  }
  free(x);
  entry *slot = entry_slot((size_t)total * sizeof(int16_t));
  size_t tl = strlen(text) + 1;
  char *copy = slot ? (char *)malloc(tl) : NULL;
  if (!copy) { free(pcm); return NULL; }
  memcpy(copy, text, tl);
  slot->sp = s; slot->text = copy; slot->pcm = pcm; slot->n = total; slot->seq = ++cache_seq;
  cache_bytes += (size_t)total * sizeof(int16_t);
  return slot;
}

void voice_init(uint32_t rate) {
  out_rate = rate ? rate : 48000;
  voice_stop();
  for (int i = 0; i < CACHE_MAX; i++) entry_free(&cache[i]);   /* a new rate invalidates the cached PCM */
  cur = NULL; pos = 0; fade = -1; cache_seq = 0;
  tts_rate();   /* load the voice now, so the first spoken line is not the one that stalls */
}

void voice_prepare(vo_speaker s, const char *text) { if (enabled) synth_line(s, text); }

void voice_say(vo_speaker s, const char *text) {
  if (!enabled) return;
  entry *e = synth_line(s, text);
  if (!e) return;
  e->seq = ++cache_seq;          /* a line in use is the newest, not the next to evict */
  cur = e; pos = 0; fade = -1;
}

void voice_stop(void) {
  if (cur && fade < 0) fade = (int)(0.02f * out_rate) + 1;
}

bool voice_busy(void) { return cur != NULL; }

float voice_seconds(vo_speaker s, const char *text) {
  if (s < 0 || s >= VO_COUNT || !text) return 0.f;
  entry *e = entry_find(s, text);
  return e ? (float)e->n / (float)out_rate : 0.f;
}

void voice_set_enabled(bool on) {
  enabled = on;
  if (!on && cur) { cur = NULL; pos = 0; fade = -1; }
}

float voice_mix(float *lr, int n) {
  if (!cur) return 0.f;
  int fade_len = (int)(0.02f * out_rate) + 1;
  for (int i = 0; i < n; i++) {
    if (pos >= cur->n || fade == 0) { cur = NULL; return i > 0 ? 1.f : 0.f; }
    float g = VOICE_GAIN;
    if (fade > 0) { g *= (float)fade / fade_len; fade--; }
    float v = cur->pcm[pos++] * (1.f / 32768.f) * g;
    lr[i * 2] += v; lr[i * 2 + 1] += v;
  }
  return 1.f;
}
