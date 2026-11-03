/* tts.c: Flite wrapper. One voice (cmu_us_slt) serves every speaker;
 * voice.c does the per-speaker colouring on the PCM. Flite's audio output,
 * file loading and sockets are not compiled in. */
#include "tts.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "flite.h"
#include "cst_cg.h"
#include "cst_audio.h"

extern cst_voice *register_cmu_us_slt(const char *voxdir);
extern cst_cg_db cmu_us_slt_cg_db;

/* Flite's audio dir (drivers, streaming) is not compiled; the voice features still name this
 * val type, so give it the two functions it needs. Nothing here ever creates one. */
CST_VAL_REGISTER_TYPE(audio_streaming_info, cst_audio_streaming_info)
void delete_audio_streaming_info(cst_audio_streaming_info *asi) { cst_free(asi); }

static cst_voice *vox;
static float base_mean, base_std;

static int ready(void) {
  if (vox) return 1;
  flite_init();
  vox = register_cmu_us_slt(NULL);
  if (!vox) return 0;
  base_mean = cmu_us_slt_cg_db.f0_mean;
  base_std = cmu_us_slt_cg_db.f0_stddev;
  return 1;
}

int tts_rate(void) { return ready() ? get_param_int(vox->features, "sample_rate", 16000) : 16000; }

/* Flite spells out words that come in capitals (it takes them for acronyms),
 * so lower-case any run of two or more capitals. Quote marks go, dashes become
 * pauses, anything outside ASCII is a space. Apostrophes stay inside words. */
void tts_normalise(const char *in, char *out, int cap) {
  int o = 0;
  size_t n = strlen(in);
  for (size_t i = 0; i < n && o < cap - 2; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '"' || c == '`') continue;
    if (c == '\'') {
      bool inside = i > 0 && i + 1 < n && isalpha((unsigned char)in[i - 1]) && isalpha((unsigned char)in[i + 1]);
      if (inside) out[o++] = '\'';
      continue;
    }
    if (c >= 0x80) {
      /* UTF-8: curly apostrophe inside a word stays, curly quotes go, dashes pause, the rest is a gap */
      if (c == 0xE2 && i + 2 < n && (unsigned char)in[i + 1] == 0x80) {
        unsigned char d = (unsigned char)in[i + 2];
        if (d == 0x99 && i > 0 && isalpha((unsigned char)in[i - 1])) out[o++] = '\'';
        else if (d == 0x93 || d == 0x94) { out[o++] = ','; out[o++] = ' '; }
        i += 2;
        continue;
      }
      while (i + 1 < n && ((unsigned char)in[i + 1] & 0xC0) == 0x80) i++;
      out[o++] = ' ';
      continue;
    }
    if (c == '-' && i + 1 < n && in[i + 1] == '-') { out[o++] = ','; out[o++] = ' '; while (i + 1 < n && in[i + 1] == '-') i++; continue; }
    if (c == '_' || c == '*' || c == '#' || c == '~' || c == '^' || c == '|' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}') { out[o++] = ' '; continue; }
    if (isupper(c)) {
      size_t j = i;
      while (j < n && isupper((unsigned char)in[j])) j++;
      bool word_end = j >= n || !isalnum((unsigned char)in[j]);
      if (j - i >= 2 && word_end) {
        for (; i < j && o < cap - 2; i++) out[o++] = (char)tolower((unsigned char)in[i]);
        i--;
        continue;
      }
    }
    out[o++] = (char)c;
  }
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = 0;
}

int16_t *tts_synth(const char *text, float pitch, float pitch_range, float stretch, int *n) {
  *n = 0;
  if (!ready() || !text || !*text) return NULL;
  feat_set_float(vox->features, "int_f0_target_mean", base_mean * pitch);
  feat_set_float(vox->features, "int_f0_target_stddev", base_std * pitch_range);
  feat_set_float(vox->features, "duration_stretch", stretch);
  feat_set_int(vox->features, "mlsa_speed_param", 8);   /* fewer mel-cepstrum terms: ~25% faster, fine on small speakers */
  cst_wave *w = flite_text_to_wave(text, vox);
  if (!w) return NULL;
  int16_t *pcm = NULL;
  if (w->num_samples > 0 && (pcm = (int16_t *)malloc((size_t)w->num_samples * sizeof(int16_t)))) {
    memcpy(pcm, w->samples, (size_t)w->num_samples * sizeof(int16_t));
    *n = w->num_samples;
  }
  delete_wave(w);
  return pcm;
}
