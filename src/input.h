/* input.h: one player's intent each frame, from any pad and the keyboard.
 *
 * Pad (standard mapping): left stick moves, right stick aims, A jumps (held in
 * the air: glide), RT fires, LT holds the scope (LB / RB or the d-pad zoom
 * while scoped), B rolls, X throws a decoy, Y swaps weapon, START pauses.
 *
 * Keyboard: WASD moves, arrows aim, Space jumps and glides, J or F fires,
 * K or Q holds the scope (R / F... wheel on the page zooms), Shift rolls,
 * E throws a decoy, Tab swaps, Enter or Esc pauses. The web page also turns
 * a locked mouse into the right stick, left click into RT, right click into LT
 * and the wheel into LB / RB, so the cart only ever sees pads and keys. */
#ifndef DS_INPUT_H
#define DS_INPUT_H
#include <stdbool.h>
#include <stdint.h>
#include "wasmcart.h"

#define DS_KEY_LSHIFT 0xE1
#define DS_KEY_RSHIFT 0xE5

typedef struct {
  float mx, my;            /* move, -1..1 (y +1 = forward) */
  float ax, ay;            /* aim rate, -1..1 (y +1 = up) */
  bool jump, jump_held;    /* jump: pressed this frame */
  bool fire, fire_held;
  bool scope;              /* held */
  int zoom;                /* -1 / +1 when a zoom step was asked this frame */
  bool roll, decoy, swap;
  bool pause;
  /* menus */
  int nav_x, nav_y;        /* edges, y +1 = down */
  bool ok, back;
  bool used_pad;           /* the last thing touched was a pad (for on-screen prompts) */
} intent_t;

void input_frame(const wc_pad_t *pads, const uint8_t *keys);
const intent_t *input_get(void);
bool input_key_down(int code);
bool input_start_held(void);   /* START is down on any pad (cutscene skip hold) */

#endif
