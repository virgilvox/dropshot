/* audio.c: the synth. No samples. A voice pool for effects (oscillator or
 * noise, an exponential pitch glide, a decay curve, a swept one pole filter,
 * a little FM and an echo send), a stereo echo bus, and a 16 step tracker for
 * the music moods (bass, pad, arp, lead, kick, snare, hat). The synth runs at
 * whatever rate the host asks for, so there is nothing to resample. */
#include "audio.h"
#include "voice.h"
#include <math.h>
#include <string.h>
#include <stdbool.h>

#define NV 28
#define TAU 6.28318530718f
enum { W_SINE, W_SQ, W_SAW, W_TRI, W_NOISE };

static float SR = 48000;
static uint32_t rs = 0x12345678u;
static float rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (float)(rs & 0xffffff) / 8388608.f - 1.f; }
static float sgn_frac(float x) { return x - floorf(x); }

static float osc(int w, float ph, float duty) {
  switch (w) {
    case W_SINE: return sinf(ph * TAU);
    case W_SQ: return (ph < duty ? 0.7f : -0.7f) - (2.f * duty - 1.f) * 0.7f;   /* no DC for thin pulses */
    case W_SAW: return ph * 2.f - 1.f;
    case W_TRI: return ph < 0.5f ? ph * 4.f - 1.f : 3.f - ph * 4.f;
    default: return rnd();
  }
}

/* ── effect voices ── */
typedef struct {
  bool on; int wave, curve, id;
  float f, mult, dur, age, delay, atk, vol, gl, gr, noise, duty, send;
  float lp, lp1, hp, fm_r, fm_i, ph, mph, lz, hz, drive, wob;
} voice;
static voice vox[NV];

static float pan_clamp(float p) { return p < -1 ? -1 : p > 1 ? 1 : p; }

/* start a voice: wave, start and end frequency, duration, volume, pan */
static voice *vx(int id, int wave, float f0, float f1, float dur, float vol, float pan) {
  int k = 0; float worst = -1;
  for (int i = 0; i < NV; i++) {
    if (!vox[i].on) { k = i; break; }
    float r = vox[i].age / vox[i].dur;
    if (r > worst) { worst = r; k = i; }
  }
  voice *v = &vox[k];
  memset(v, 0, sizeof *v);
  v->on = true; v->id = id; v->wave = wave; v->f = f0; v->dur = dur;
  v->mult = (f0 > 0 && f1 > 0) ? powf(f1 / f0, 1.f / (dur * SR)) : 1.f;
  v->vol = vol; v->curve = 2; v->duty = 0.5f; v->atk = 0.002f;
  float a = (pan_clamp(pan) + 1.f) * 0.78539816f;
  v->gl = cosf(a); v->gr = sinf(a);
  return v;
}

/* ── echo bus ── */
#define ECH 32768
static float eb[2][ECH]; static int ew; static float elp[2];

/* ── music ── */
typedef struct {
  float bpm; int root, nch; const char *chords;   /* chord roots as semitone letters 'a'+n, then quality in quals */
  const char *quals;
  const char *bass, *kick, *snare, *hat, *arp, *lead;   /* 16 step patterns; lead is 64 */
  int major, answer, bars_once;                          /* bars_once: play this many bars and stop (jingle) */
  float v_bass, v_pad, v_arp, v_lead, v_drum;
  int bass_w, lead_w, lead_shift;                        /* bass: 0 saw 1 square 2 tri; lead: 0 soft, 1 pulse, 2 saws; shift in semitones */
  float bass_lp, drive, bass_dec, arp_dec;               /* per sample decay of a bass note and an arp ping */
} song;

/* patterns: bass r root, f fifth, o octave, s b7, t third; arp digits 0..5 chord tones (root, 3rd, 5th, then octave);
 * lead digits 0..9 a..f pentatonic degrees, '-' holds, '.' rests */
static const song SONGS[6] = {
  /* TITLE: moody, slow, minor, wide pad and a lonely lead */
  { 76, 38, 8, "aiahaiah", "mMmMmMMM",
    "r.......r.f.....", "x.....x.....x...", "................", "..x...x...x...x.", "0.2.1.2.0.2.4.2.",
    "4---3---0-------" "5---4---3-2-----" "7---5---4-------" "3---4---2-------",
    1, 0, 0, 0.8f, 0.55f, 0.22f, 0.55f, 0.5f, 2, 0, 0, 520, 0.f, 0.00006f, 0.0003f },
  /* DROP: soaring, major, bright arps over a four on the floor */
  { 132, 36, 8, "ahjfahfh", "MMmMMMMM",
    "r.rrr.rrr.rrr.r.", "x...x...x...x...", "....x.......x...", "..x...x...x...x.", "0123454321012345",
    "7---5---6---4---" "7-8-7-5-6---8---" "9---7---8---6-5-" "7---8---5-------",
    1, 2, 0, 0.55f, 0.35f, 0.4f, 0.65f, 0.8f, 0, 2, 0, 1400, 0.f, 0.0003f, 0.0007f },
  /* EXPLORE: tense, a pedal bass, sparse ticks, notes held too long */
  { 104, 40, 8, "aabaakba", "mmMmmMMm",
    "r.r.rr.sr.r.r.f.", "x.....x...x.....", "........x.......", "x.x.x.x.x.x.x.xx", "0.....2...1.....",
    "..4-------3-----" "..5-----4-------" "..4-----2-------" "3-----------0---",
    0, 3, 0, 0.8f, 0.4f, 0.28f, 0.5f, 0.4f, 1, 0, 0, 700, 0.f, 0.0002f, 0.0006f },
  /* FIGHT: fast, driving 8ths, busy hats, a running arp */
  { 158, 33, 8, "aidkaikh", "mMMMmMMM",
    "rrorrorrrorrorrf", "x...x...x...x...", "....x.......x..x", "x.xxx.xxx.xxx.xx", "0202121202021210",
    "4-3-4-5-7-5-4-3-" "5-4-5-7-9-7-5-4-" "7-9-7-5-4-5-7---" "9-7-5-4-3-4-5---",
    0, 2, 0, 0.7f, 0.25f, 0.4f, 0.55f, 0.85f, 2, 1, 0, 1800, 0.25f, 0.0004f, 0.0009f },
  /* BOSS: heavy, slow, saw bass pushed into clipping, stabs and a siren lead */
  { 112, 31, 8, "aabaaakb", "mmMmmmMM",
    "r..rr..r..rr.s..", "x..x..x...x.x...", "........x.......", "x...x...x...x...", "0.....0.0.....0.",
    "0-------1-------" "0-------3-------" "0---1---0-----3-" "4---------------",
    0, 2, 0, 0.95f, 0.45f, 0.45f, 0.5f, 1.f, 0, 1, -12, 900, 0.7f, 0.00025f, 0.0006f },
  /* CLEAR: a short major fanfare, played once */
  { 132, 36, 4, "ahfa", "MMMM",
    "r.o.f.o.r.o.f.o.", "x...x...x...x...", "....x.......x...", "x.x.x.x.x.x.x.x.", "0.1.2.3.2.1.2.3.",
    "5.7.8.a---a-8-7-" "8.8.7.8.a-------" "a.8.a.c---c-a-8-" "d---------------",
    1, 0, 4, 0.6f, 0.45f, 0.4f, 0.65f, 0.7f, 2, 2, 0, 1800, 0.f, 0.0003f, 0.0008f },
};

typedef struct {   /* a note voice for the sequencer */
  float f, ph, env, rate, z, ph2;
} nv;
static int cur = -1, want = MOOD_SILENT;
static float mgain, mtarget = 1, music_on = 1;
static double step_ctr; static int step, bar;
static bool once_done;
static nv bass_v, lead_v, arp_v, pad_v[3], kick_v, snare_v, hat_v;
static float pad_z[2], lead_hold, vibt, hat_open;

static float mtof(float n) { return 440.f * powf(2.f, (n - 69.f) / 12.f); }
static float pent_min[5] = {0, 3, 5, 7, 10}, pent_maj[5] = {0, 2, 4, 7, 9};
static int degree_semi(int d, int major) { return (d / 5) * 12 + (major ? pent_maj : pent_min)[d % 5]; }
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static void song_reset(void) {
  step = 0; bar = 0; step_ctr = 0; once_done = false;
  memset(&bass_v, 0, sizeof bass_v); memset(&lead_v, 0, sizeof lead_v); memset(&arp_v, 0, sizeof arp_v);
  memset(pad_v, 0, sizeof pad_v); memset(&kick_v, 0, sizeof kick_v); memset(&snare_v, 0, sizeof snare_v); memset(&hat_v, 0, sizeof hat_v);
  pad_z[0] = pad_z[1] = 0; lead_hold = 0;
}

static int chord_root(const song *s, int b) { return s->chords[b % s->nch] - 'a'; }
static int chord_third(const song *s, int b) { return s->quals[b % s->nch] == 'M' ? 4 : 3; }

static void trigger(const song *s) {
  int st = step & 15, b = bar;
  int cr = chord_root(s, b), ct = chord_third(s, b);
  float root = (float)(s->root + cr);
  char bc = s->bass[st];
  if (bc != '.') {
    int off = bc == 'f' ? 7 : bc == 'o' ? 12 : bc == 's' ? 10 : bc == 't' ? ct : 0;
    bass_v.f = mtof(root + 12 + off);   /* an octave up, so small speakers carry it */ bass_v.env = 1; bass_v.rate = 0;
  }
  char ac = s->arp[st];
  if (ac != '.') {
    int d = ac - '0'; int tone = d % 3, oct = d / 3;
    int off = tone == 0 ? 0 : tone == 1 ? ct : 7;
    arp_v.f = mtof(root + 24 + off + oct * 12); arp_v.env = 1;
  }
  if (s->kick[st] == 'x') { kick_v.env = 1; kick_v.f = 0; kick_v.ph = 0; }
  if (s->snare[st] == 'x') snare_v.env = 1;
  if (s->hat[st] == 'x') { hat_v.env = (st & 3) == 2 ? 1.f : 0.7f; hat_open = (st == 14) ? 1.f : 0.f; }
  /* lead: the 64 step line, answered a few degrees higher on the second four bars */
  int idx = (b & 3) * 16 + st;
  char lc = s->lead[idx];
  int d = hexv(lc);
  if (d >= 0) {
    d += ((b >> 2) & 1) * s->answer;
    lead_v.f = mtof((float)(s->root + 24 + s->lead_shift + degree_semi(d, s->major)));
    vibt = 0; lead_hold = 1;
  } else if (lc == '.') lead_hold = 0;
  if (cur == MOOD_CLEAR && st == 0 && b == 3) hat_v.env = 1;
}

/* sequencer, advance one sample; returns the stereo music sample into m[2] and the echo send */
static void music_sample(const song *s, float *mL, float *mR, float *snd) {
  if (step_ctr <= 0) {
    if (!once_done) {
      trigger(s);
    }
    step_ctr += SR * 60.0 / s->bpm / 4.0;
    step++;
    if ((step & 15) == 0) {
      bar++;
      if (s->bars_once && bar >= s->bars_once) { once_done = true; mtarget = 0; }
    }
  }
  step_ctr -= 1;
  if (once_done && mgain < 0.002f) { *mL = *mR = *snd = 0; return; }
  float dt = 1.f / SR;
  int b = step > 0 ? (step - 1) >> 4 : 0;   /* the bar of the step just triggered */
  int cr = chord_root(s, b), ct = chord_third(s, b);
  float root = (float)(s->root + cr);
  float L = 0, R = 0, sd = 0;

  /* bass: saw and sub through a one pole whose cutoff follows the note */
  if (bass_v.env > 0.0005f) {
    bass_v.ph = sgn_frac(bass_v.ph + bass_v.f * dt);
    float w = s->bass_w == 1 ? osc(W_SQ, bass_v.ph, 0.5f) : s->bass_w == 2 ? osc(W_TRI, bass_v.ph, 0) : osc(W_SAW, bass_v.ph, 0);
    bass_v.ph2 = sgn_frac(bass_v.ph2 + bass_v.f * 0.5f * dt);
    float x = w * 0.55f + osc(W_SINE, bass_v.ph2, 0) * 0.65f;
    float cut = s->bass_lp * (0.35f + 0.65f * bass_v.env);
    bass_v.z += (x - bass_v.z) * fminf(1.f, cut * TAU * dt);
    float y = bass_v.z;
    if (s->drive > 0) y = tanhf(y * (1.f + s->drive * 5.f)) * 0.8f;
    y *= bass_v.env * s->v_bass;
    L += y; R += y;
    bass_v.env *= 1.f - s->bass_dec * (48000.f / SR);
  }
  /* pad: a triad of detuned saws, slow to follow the chord */
  {
    float fr[3] = {mtof(root + 12), mtof(root + 12 + ct), mtof(root + 19)};
    float acc = 0;
    for (int i = 0; i < 3; i++) {
      pad_v[i].f += (fr[i] - pad_v[i].f) * 0.0008f * (48000.f / SR);
      if (pad_v[i].f < 20) pad_v[i].f = fr[i];
      pad_v[i].ph = sgn_frac(pad_v[i].ph + pad_v[i].f * dt);
      pad_v[i].ph2 = sgn_frac(pad_v[i].ph2 + pad_v[i].f * 1.006f * dt);
      acc += osc(W_SAW, pad_v[i].ph, 0) + osc(W_SAW, pad_v[i].ph2, 0);
    }
    pad_z[0] += (acc - pad_z[0]) * 0.035f * (48000.f / SR);
    pad_z[1] += (pad_z[0] - pad_z[1]) * 0.035f * (48000.f / SR);
    float y = pad_z[1] * s->v_pad * 0.14f;
    L += y * 0.9f; R += y * 1.0f; sd += y * 0.2f;
  }
  /* arp: a thin pulse that pings and fades, alternating sides */
  if (arp_v.env > 0.0005f) {
    arp_v.ph = sgn_frac(arp_v.ph + arp_v.f * dt);
    float y = osc(W_SQ, arp_v.ph, 0.25f) * arp_v.env * s->v_arp * 0.3f;
    L += y * 0.8f; R += y * 0.5f; sd += y * 0.35f;
    arp_v.env *= 1.f - s->arp_dec * (48000.f / SR);
  }
  /* lead: a pulse with late vibrato, held until the next note or rest */
  if (lead_hold > 0 || lead_v.env > 0.001f) {
    vibt += dt;
    float vib = 1.f + 0.006f * sinf(vibt * 38.f) * fminf(1.f, vibt * 3.f);
    lead_v.ph = sgn_frac(lead_v.ph + lead_v.f * vib * dt);
    lead_v.ph2 = sgn_frac(lead_v.ph2 + lead_v.f * vib * 1.004f * dt);
    float w = s->lead_w == 2 ? osc(W_SAW, lead_v.ph, 0) + osc(W_SAW, lead_v.ph2, 0) : s->lead_w == 1 ? osc(W_SQ, lead_v.ph, 0.4f) * 1.2f + osc(W_SAW, lead_v.ph2, 0) * 0.5f : osc(W_TRI, lead_v.ph, 0) * 1.4f + osc(W_SINE, lead_v.ph2, 0);
    lead_v.z += (w - lead_v.z) * 0.22f * (48000.f / SR);
    float target = lead_hold > 0 ? 1.f : 0.f;
    lead_v.env += (target - lead_v.env) * (target > lead_v.env ? 0.004f : 0.0012f) * (48000.f / SR);
    float y = lead_v.z * lead_v.env * s->v_lead * 0.22f;
    L += y * 0.8f; R += y * 0.9f; sd += y * 0.5f;
  }
  /* drums */
  float dr = 0;
  if (kick_v.env > 0.0005f) {
    kick_v.f = 55.f + 110.f * kick_v.env * kick_v.env * kick_v.env;
    kick_v.ph = sgn_frac(kick_v.ph + kick_v.f * dt);
    dr += sinf(kick_v.ph * TAU) * kick_v.env * 1.1f;
    kick_v.env *= 1.f - 0.00055f * (48000.f / SR) * 1.f;
    kick_v.env -= 0.00006f * (48000.f / SR);
  }
  if (snare_v.env > 0.0005f) {
    snare_v.ph = sgn_frac(snare_v.ph + 185.f * dt);
    float n = rnd();
    snare_v.z += (n - snare_v.z) * 0.55f;
    dr += ((n - snare_v.z) * 0.8f + osc(W_TRI, snare_v.ph, 0) * 0.4f) * snare_v.env * 0.6f;
    snare_v.env *= 1.f - 0.0012f * (48000.f / SR);
  }
  if (hat_v.env > 0.0005f) {
    float n = rnd();
    hat_v.z += (n - hat_v.z) * 0.7f;
    dr += (n - hat_v.z) * hat_v.env * 0.28f;
    hat_v.env *= 1.f - (hat_open > 0 ? 0.0004f : 0.004f) * (48000.f / SR);
  }
  dr *= s->v_drum;
  L += dr; R += dr;
  *mL = L; *mR = R; *snd = sd;
}

/* ── api ── */
static float sfx_vol = 0.9f, mus_vol = 0.5f;

void audio_init(uint32_t rate) {
  SR = rate ? (float)rate : 48000.f;
  memset(vox, 0, sizeof vox); memset(eb, 0, sizeof eb);
  cur = -1; want = MOOD_SILENT; mgain = 0; mtarget = 1; ew = 0;
  voice_init(rate);
}

void music_mood(mood_t m) { want = (int)m; }
void audio_set_music(int on) { music_on = on ? 1.f : 0.f; }

void sfx(sfx_t s, float pan, float gain) {
  if (s < 0 || s >= SFX_COUNT) return;
  float g = gain <= 0 ? 0 : gain > 1 ? 1 : gain, p = pan;
  voice *v;
#define V(w, f0, f1, d, vol) vx((int)s, w, f0, f1, d, (vol) * g, p)
  switch (s) {
    case SFX_SHOT: {   /* chain gun: a bark of noise on a short square thump, pitch jittered per round */
      float j = 1.f + rnd() * 0.12f;
      v = V(W_NOISE, 0, 0, 0.075f, 0.5f); v->lp = 7500; v->lp1 = 1400; v->curve = 3;
      v = V(W_SQ, 210 * j, 70, 0.07f, 0.4f); v->duty = 0.35f; v->curve = 2; v->lp = 2200;
      v = V(W_SAW, 1900 * j, 500, 0.02f, 0.18f); v->curve = 1;
      break; }
    case SFX_SNIPE: {  /* crack, chest thump, a ringing rail and a long roll of tail */
      v = V(W_NOISE, 0, 0, 0.05f, 0.9f); v->lp = 14000; v->lp1 = 3500; v->curve = 3;
      v = V(W_SINE, 150, 32, 0.55f, 1.0f); v->curve = 2; v->drive = 2.f;
      v = V(W_SAW, 1100, 90, 0.14f, 0.4f); v->lp = 5000; v->lp1 = 400; v->curve = 3;
      v = V(W_NOISE, 0, 0, 1.5f, 0.5f); v->lp = 3200; v->lp1 = 180; v->curve = 3; v->atk = 0.03f; v->send = 0.7f;
      v = V(W_SINE, 1500, 1500, 0.7f, 0.1f); v->fm_r = 1.5f; v->fm_i = 3.f; v->send = 0.7f; v->curve = 3;
      break; }
    case SFX_ZOOM: {   /* a servo: rising whirr with a clunk at each end */
      v = V(W_SAW, 260, 720, 0.24f, 0.4f); v->lp = 1600; v->curve = 1; v->atk = 0.03f; v->wob = 40.f;
      v = V(W_NOISE, 0, 0, 0.22f, 0.25f); v->lp = 4000; v->hp = 900; v->curve = 1;
      v = V(W_SQ, 900, 700, 0.015f, 0.25f); v->curve = 1;
      v = V(W_SQ, 600, 500, 0.02f, 0.25f); v->delay = 0.21f; v->curve = 1;
      break; }
    case SFX_JUMP: {
      v = V(W_TRI, 170, 520, 0.2f, 0.7f); v->curve = 2;
      v = V(W_NOISE, 0, 0, 0.18f, 0.5f); v->lp = 700; v->lp1 = 3200; v->hp = 300; v->atk = 0.05f; v->curve = 2;
      break; }
    case SFX_GLIDE: {  /* rate limited: while a glide gust plays, the next call joins it instead of stacking */
      for (int i = 0; i < NV; i++) if (vox[i].on && vox[i].id == SFX_GLIDE && vox[i].age < 0.4f) return;
      v = V(W_NOISE, 0, 0, 0.8f, 0.8f); v->lp = 700; v->lp1 = 1700; v->hp = 250; v->atk = 0.18f; v->curve = 1; v->send = 0.15f;
      v = V(W_NOISE, 0, 0, 0.8f, 0.25f); v->lp = 3200; v->lp1 = 4800; v->hp = 1500; v->atk = 0.25f; v->curve = 1;
      break; }
    case SFX_LAND: {
      v = V(W_SINE, 110, 36, 0.28f, 1.0f); v->curve = 2;
      v = V(W_NOISE, 0, 0, 0.14f, 0.5f); v->lp = 1200; v->lp1 = 200; v->curve = 3;
      v = V(W_TRI, 220, 75, 0.16f, 0.6f); v->curve = 2;
      break; }
    case SFX_ROLL: {
      v = V(W_NOISE, 0, 0, 0.32f, 0.9f); v->lp = 500; v->lp1 = 3000; v->hp = 200; v->atk = 0.08f; v->curve = 2;
      v = V(W_TRI, 160, 80, 0.2f, 0.5f); v->curve = 2;
      break; }
    case SFX_HIT: {    /* the Tenants are wet: a squelch that dips and wobbles */
      v = V(W_SAW, 260, 55, 0.17f, 0.5f); v->lp = 2600; v->lp1 = 250; v->wob = 55.f; v->curve = 2;
      v = V(W_NOISE, 0, 0, 0.1f, 0.4f); v->lp = 2000; v->lp1 = 300; v->curve = 3;
      v = V(W_SINE, 700, 170, 0.08f, 0.35f); v->curve = 2;
      break; }
    case SFX_KILL: {   /* a pop with a short rising bubble after it */
      v = V(W_SINE, 520, 60, 0.14f, 0.9f); v->curve = 2;
      v = V(W_NOISE, 0, 0, 0.06f, 0.6f); v->lp = 6000; v->lp1 = 800; v->curve = 3;
      v = V(W_TRI, 300, 1000, 0.07f, 0.35f); v->delay = 0.05f; v->curve = 1;
      break; }
    case SFX_HURT: {
      v = V(W_SAW, 320, 80, 0.3f, 0.55f); v->lp = 2400; v->lp1 = 300; v->curve = 2;
      v = V(W_SQ, 233, 100, 0.2f, 0.3f); v->duty = 0.3f; v->curve = 2;
      v = V(W_NOISE, 0, 0, 0.18f, 0.5f); v->lp = 3500; v->lp1 = 400; v->curve = 2;
      break; }
    case SFX_PICKUP: {
      static const float n[3] = {880, 1108.7f, 1318.5f};
      for (int i = 0; i < 3; i++) { v = V(W_SINE, n[i], n[i], 0.32f - i * 0.04f, 0.4f); v->delay = i * 0.06f; v->fm_r = 2.f; v->fm_i = 1.2f; v->send = 0.35f; v->curve = 2; v->atk = 0.004f; }
      break; }
    case SFX_DECOY: {
      for (int i = 0; i < 2; i++) { v = V(W_SQ, 1050, 1050, 0.06f, 0.6f); v->duty = 0.25f; v->delay = i * 0.1f; v->lp = 4000; v->curve = 1; v->send = 0.2f; }
      v = V(W_SINE, 525, 525, 0.3f, 0.4f); v->delay = 0.2f; v->curve = 3; v->send = 0.3f;
      break; }
    case SFX_BOOM: {
      v = V(W_SINE, 90, 22, 1.2f, 1.0f); v->curve = 2; v->drive = 1.5f;
      v = V(W_SAW, 170, 40, 0.8f, 0.4f); v->lp = 600; v->curve = 2; v->drive = 1.f;
      v = V(W_NOISE, 0, 0, 1.3f, 0.7f); v->lp = 3000; v->lp1 = 90; v->curve = 3; v->atk = 0.004f; v->send = 0.4f;
      v = V(W_NOISE, 0, 0, 0.08f, 0.8f); v->hp = 2000; v->lp = 9000; v->curve = 3;
      break; }
    case SFX_UI: {
      v = V(W_SQ, 1900, 1500, 0.03f, 0.4f); v->duty = 0.3f; v->curve = 1; v->lp = 5000;
      break; }
    case SFX_UI_OK: {
      v = V(W_TRI, 660, 660, 0.09f, 0.4f); v->curve = 1;
      v = V(W_TRI, 990, 990, 0.16f, 0.4f); v->delay = 0.07f; v->curve = 2; v->send = 0.25f;
      break; }
    case SFX_BOSS_HIT: {   /* a struck girder: inharmonic FM bell over a thud */
      v = V(W_SINE, 430, 430, 0.7f, 0.5f); v->fm_r = 1.41f; v->fm_i = 6.f; v->curve = 2; v->send = 0.35f;
      v = V(W_SINE, 660, 660, 0.45f, 0.3f); v->fm_r = 2.76f; v->fm_i = 4.f; v->curve = 2; v->send = 0.3f;
      v = V(W_SINE, 110, 45, 0.2f, 0.8f); v->curve = 2;
      v = V(W_NOISE, 0, 0, 0.03f, 0.6f); v->hp = 3000; v->lp = 12000; v->curve = 2;
      break; }
    case SFX_ALARM: {
      for (int i = 0; i < 4; i++) { v = V(W_SQ, i & 1 ? 660 : 880, i & 1 ? 660 : 880, 0.17f, 0.6f); v->delay = i * 0.18f; v->duty = 0.4f; v->lp = 3200; v->curve = 1; v->atk = 0.006f; v->send = 0.2f; }
      break; }
    default: break;
  }
#undef V
}

void audio_render(float *ring, uint32_t cap, uint32_t *cursor, uint32_t rate, double ms) {
  if (rate && (float)rate != SR) SR = (float)rate;
  static double carry;
  double exact = SR * ms / 1000.0 + carry;
  uint32_t n = (uint32_t)exact; carry = exact - n;
  if (n > cap) n = cap;
  float dt = 1.f / SR;
  /* the voice mixes into a scratch block; its ducking target pulls the music down ~60% while it speaks */
  static float vbuf[8192 * 2];
  static float duck;
  if (n > 8192) n = 8192;
  memset(vbuf, 0, n * 2 * sizeof(float));
  float duck_to = voice_mix(vbuf, (int)n);
  int e1 = (int)(0.19f * SR), e2 = (int)(0.253f * SR);
  if (e1 >= ECH) e1 = ECH - 1; if (e2 >= ECH) e2 = ECH - 1;
  for (uint32_t i = 0; i < n; i++) {
    /* mood changes fade the old tune down, then start the new one from its top */
    float mtgt = (want == cur && cur >= 0 && cur != MOOD_SILENT) ? mtarget : 0.f;
    if (want == MOOD_SILENT) mtgt = 0.f;
    mgain += (mtgt * music_on - mgain) * (mtgt > mgain ? 0.0004f : 0.0006f) * (48000.f / SR);
    if (want != cur && mgain < 0.01f) { cur = want; mtarget = 1; song_reset(); mgain = 0; }
    float mL = 0, mR = 0, msd = 0;
    if (cur >= 0 && cur < MOOD_SILENT && (mgain > 0.0005f || mtgt > 0)) {
      music_sample(&SONGS[cur], &mL, &mR, &msd);
      mL *= mgain * mus_vol; mR *= mgain * mus_vol; msd *= mgain * mus_vol;
    }
    duck += (duck_to - duck) * (duck_to > duck ? 0.0025f : 0.0004f) * (48000.f / SR);
    float dk = 1.f - 0.6f * duck;
    mL *= dk; mR *= dk; msd *= dk;
    float L = mL, R = mR, sd = msd;
    for (int k = 0; k < NV; k++) {
      voice *v = &vox[k];
      if (!v->on) continue;
      if (v->delay > 0) { v->delay -= dt; continue; }
      float u = 1.f - v->age / v->dur;
      if (u <= 0) { v->on = false; continue; }
      float env = u; for (int c = 1; c < v->curve; c++) env *= u;
      if (v->age < v->atk) env *= v->age / v->atk;
      v->f *= v->mult;
      float f = v->f;
      if (v->wob > 0) f *= 1.f + 0.12f * sinf(v->age * v->wob);
      float x;
      if (v->wave == W_NOISE) x = rnd();
      else {
        v->ph = sgn_frac(v->ph + f * dt);
        if (v->fm_i > 0) {
          v->mph = sgn_frac(v->mph + f * v->fm_r * dt);
          x = sinf(v->ph * TAU + v->fm_i * env * sinf(v->mph * TAU));
        } else x = osc(v->wave, v->ph, v->duty);
      }
      if (v->lp > 0) {
        float fc = v->lp1 > 0 ? v->lp + (v->lp1 - v->lp) * (v->age / v->dur) : v->lp;
        v->lz += (x - v->lz) * fminf(1.f, fc * TAU * dt);
        x = v->lz;
      }
      if (v->hp > 0) { v->hz += (x - v->hz) * fminf(1.f, v->hp * TAU * dt); x -= v->hz; }
      if (v->drive > 0) x = tanhf(x * (1.f + v->drive));
      x *= env * v->vol;
      L += x * v->gl; R += x * v->gr; sd += x * v->send;
      v->age += dt;
    }
    /* echo bus, a touch darker on each repeat */
    int r1 = (ew - e1 + ECH) % ECH, r2 = (ew - e2 + ECH) % ECH;
    float dl = eb[0][r1], dr = eb[1][r2];
    elp[0] += (dl - elp[0]) * 0.35f; elp[1] += (dr - elp[1]) * 0.35f;
    eb[0][ew] = sd * 0.9f + elp[1] * 0.38f;
    eb[1][ew] = sd * 0.9f + elp[0] * 0.38f;
    ew = (ew + 1) % ECH;
    L += elp[0] * 0.5f; R += elp[1] * 0.5f;
    L *= sfx_vol; R *= sfx_vol;
    /* a soft clip, so stacked effects and the music never overshoot */
    L = tanhf(L * 0.85f); R = tanhf(R * 0.85f);
    L = fmaxf(-1.f, fminf(1.f, L + vbuf[i * 2])); R = fmaxf(-1.f, fminf(1.f, R + vbuf[i * 2 + 1]));
    uint32_t w = (*cursor + i) % cap;
    ring[w * 2] = L; ring[w * 2 + 1] = R;
  }
  *cursor = (*cursor + n) % cap;
}
