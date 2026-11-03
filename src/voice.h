/* voice.h: speech for radio calls, dialogue and alerts, synthesised in the
 * cart (tts.c) and coloured per speaker (a radio band for Marguerite, a PA
 * system for the Superintendent). audio.c mixes it in and ducks the music. */
#ifndef DS_VOICE_H
#define DS_VOICE_H
#include <stdbool.h>
#include <stdint.h>

typedef enum { VO_MARGUERITE, VO_JUNO, VO_SUPER, VO_TENANT, VO_ALERT, VO_COUNT } vo_speaker;

void voice_init(uint32_t rate);
/* synthesise a line ahead of time so voice_say starts at once (call at loads
 * and cutscene starts; it can take tens of milliseconds per line) */
void voice_prepare(vo_speaker s, const char *text);
/* speak a line now, replacing whatever is playing; synthesises it first if it
 * was not prepared */
void voice_say(vo_speaker s, const char *text);
void voice_stop(void);
bool voice_busy(void);
float voice_seconds(vo_speaker s, const char *text);  /* length of a prepared line, 0 if unknown */
void voice_set_enabled(bool on);
/* audio.c: add the voice to n stereo frames at the output rate; returns the
 * ducking amount 0..1 for the music */
float voice_mix(float *lr, int n);

#endif
