/* hud.h: everything 2D. The play HUD, the scope view, and every screen and
 * menu, painted with cv.h and composited over the frame. game.c fills a
 * hud_t each frame; hud_draw paints it. */
#ifndef DS_HUD_H
#define DS_HUD_H
#include "ds.h"

typedef enum {
  HUD_TITLE,        /* logo, "press A / Enter", level select once unlocked */
  HUD_INTRO,        /* level name card over the orbital drop */
  HUD_DROP,         /* free fall: altitude, rings, "hold A to open the wing" */
  HUD_PLAY,
  HUD_PAUSE,
  HUD_DEAD,         /* "retry from checkpoint" */
  HUD_CLEAR,        /* level stats */
  HUD_END,          /* after the last boss: credits */
  HUD_CINE          /* a cutscene: nothing but hud_cine.c's captions */
} hud_mode;

typedef struct {
  hud_mode mode;
  float t;                     /* seconds in this mode */
  int level;                   /* 0-based */
  const char *level_name, *level_blurb;
  /* play */
  float health;                /* 0..1 */
  float shield;                /* 0..1, 0 = none */
  int snipe_ammo, decoys;
  int weapon;                  /* 0 chain gun, 1 scope shot */
  float spin;                  /* chain gun spin-up 0..1 */
  int score;
  float time_s;
  float hit_marker;            /* >0: show the hit marker (seconds left) */
  bool hit_weak;               /* the last hit was a weak point */
  float hurt;                  /* >0: red edge flash */
  float hurt_angle;            /* where the hurt came from, radians, 0 = ahead, + = right */
  bool used_pad;               /* draw pad prompts, else key caps */
  const char *banner;          /* centre message ("CHECKPOINT"), NULL = none */
  float banner_t;              /* seconds left */
  /* scope */
  bool scoped;
  int zoom;                    /* 1..4 */
  float range_m;               /* distance to what the reticle is on */
  bool on_enemy, on_weak;      /* the reticle is over an enemy / a weak point */
  bool weak_mark; float weak_x, weak_y;   /* a weak point's screen position (pixels) to bracket */
  /* drop */
  float altitude_m;
  int rings, rings_total;
  bool wing_open;
  /* boss */
  bool boss; float boss_health; int boss_phase; const char *boss_name;
  /* menus: the selected row (pause: 0 resume, 1 restart level, 2 settings, 3 quit; title: level rows) */
  int menu_sel;
  int unlocked;                /* levels unlocked (title level select) */
  bool music_on, shake_on, invert_y;
  /* clear */
  int kills, kills_total; float clear_time_s; int best_time_ms; int best_score; bool new_best;
  /* cutscenes (hud_cine.c): letterbox 0..1, the caption being typed, skip hold 0..1 */
  float cine;
  const char *cap_speaker, *cap_text; int cap_chars;
  float skip;
  /* radio during play: a caption at the bottom, timer in seconds left */
  const char *radio_speaker, *radio_text; float radio_t;
} hud_t;

void hud_init(void);
/* paint h over the current frame target (call after render_scene) */
void hud_draw(const hud_t *h);
/* hud_cine.c: captions, letterbox and radio; hud_draw calls it last */
void hud_cine_draw(const hud_t *h);

#endif
