/* cutscene.h: in-engine cutscenes and the radio script (docs/STORY.md).
 * cutscene.c owns the shots, the sets it builds for itself, the captions and
 * every line of dialogue; game.c runs it and shows what it asks for. */
#ifndef DS_CUTSCENE_H
#define DS_CUTSCENE_H
#include "ds.h"
#include "three.h"
#include "input.h"

typedef enum { CUT_PROLOGUE, CUT_SUBLET, CUT_FREEHOLD, CUT_BRUTE, CUT_BOSS, CUT_ENDING,
               CUT_COLD, CUT_ANNEX, CUT_BASEMENT, CUT_COUNT } cut_id;

/* what a cutscene frame wants shown */
typedef struct {
  v3 cam_pos, cam_target; float fov;
  bool juno_visible; v3 juno_pos; float juno_yaw; const char *juno_clip; float juno_clip_speed;
  bool juno_wing;                        /* her glider open */
  float letterbox;                       /* 0..1 */
  const char *speaker, *text; int chars; /* the caption, typed out to chars */
  float skip;                            /* skip hold progress 0..1 */
  float fade;                            /* 0 clear .. 1 black */
} cine_view_t;

void cutscene_init(t3_scene *scene);     /* once, after world_init (builds its own sets) */
void cutscene_start(cut_id id, int level);
bool cutscene_active(void);
/* optional: where Juno stands when a level cutscene begins, so the arena cuts can stage around her */
void cutscene_hint_player(v3 pos, float yaw);
/* step; false once it has finished or been skipped (hold START / Enter / A) */
bool cutscene_update(float dt, const intent_t *I, cine_view_t *out);

/* radio during play: the line for a story beat, NULL when there is none */
const char *story_radio(int level, int beat, const char **speaker);
/* the voice for a caption speaker name ("MARGUERITE", "THE SUPERINTENDENT", ...) */
#include "voice.h"
vo_speaker cutscene_voice_for(const char *speaker);
/* a deadpan line for the death screen and the loading card */
const char *story_quip(int seed);

#endif
