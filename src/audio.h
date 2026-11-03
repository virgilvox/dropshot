/* audio.h: the synth. Effects are fire-and-forget; music follows a mood. */
#ifndef DS_AUDIO_H
#define DS_AUDIO_H
#include <stdint.h>

typedef enum {
  SFX_SHOT, SFX_SNIPE, SFX_ZOOM, SFX_JUMP, SFX_GLIDE, SFX_LAND, SFX_ROLL,
  SFX_HIT, SFX_KILL, SFX_HURT, SFX_PICKUP, SFX_DECOY, SFX_BOOM, SFX_UI, SFX_UI_OK,
  SFX_BOSS_HIT, SFX_ALARM, SFX_COUNT
} sfx_t;

typedef enum { MOOD_TITLE, MOOD_DROP, MOOD_EXPLORE, MOOD_FIGHT, MOOD_BOSS, MOOD_CLEAR, MOOD_SILENT } mood_t;

void audio_init(uint32_t rate);
void audio_render(float *ring, uint32_t cap, uint32_t *cursor, uint32_t rate, double ms);
/* pan -1..1, gain 0..1 */
void sfx(sfx_t s, float pan, float gain);
void music_mood(mood_t m);
void audio_set_music(int on);

#endif
