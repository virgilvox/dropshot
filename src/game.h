/* game.h: the game's state and the render hooks main.c provides. */
#ifndef DS_GAME_H
#define DS_GAME_H
#include "ds.h"
#include "three.h"

/* the save region (RetroArch .srm, the page's storage) */
typedef struct {
  uint32_t magic;            /* DS_SAVE_MAGIC once written */
  uint8_t level_reached;     /* furthest level unlocked, 0-based */
  uint8_t music, shake, invert_y;
  uint32_t best_ms[8];       /* best clear time per level, 0 = none */
  uint32_t best_score[8];
  uint8_t pad[16];
} ds_save_t;
#define DS_SAVE_MAGIC 0x32485344u   /* 'DSH2'; 'DSH1' saves are upgraded in save_write */
#define DS_SAVE_MAGIC_V1 0x31485344u
extern ds_save_t DS_SAVE;

extern t3_renderer *DS_R;
void render_scene(t3_scene *scene, t3_camera *cam);   /* into the frame target */
void render_target(void);                             /* bind the frame target (for raw GL overlays) */
void render_clear(uint32_t rgb);

void game_init(void);
void game_frame(float dt);
void game_draw(void);

#endif
