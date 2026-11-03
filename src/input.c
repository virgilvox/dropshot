/* input.c: see input.h. Every connected pad and the keyboard drive the one
 * player; the strongest stick wins and buttons are or'd. */
#include "input.h"
#include "ds.h"
#include <string.h>

static uint8_t kcur[32], kprev[32];
static wc_pad_t pcur[4], pprev[4];
static intent_t I;
static int nav_ox, nav_oy;
static bool last_pad;

#define DOWN(k, c) (((k)[(c) >> 3] >> ((c) & 7)) & 1)

bool input_key_down(int c) { return c > 0 && c < 256 && DOWN(kcur, c); }
static bool kp(int c) { return DOWN(kcur, c) && !DOWN(kprev, c); }
static bool any_k(const int *c, int n) { for (int i = 0; i < n; i++) if (DOWN(kcur, c[i])) return true; return false; }
static bool any_kp(const int *c, int n) { for (int i = 0; i < n; i++) if (kp(c[i])) return true; return false; }

static float stick(int16_t v) {
  float f = v / 32767.f, dz = 0.2f;
  if (fabsf(f) < dz) return 0;
  return ds_sign(f) * fminf(1, (fabsf(f) - dz) / (1 - dz));
}

void input_frame(const wc_pad_t *pads, const uint8_t *keys) {
  memcpy(kprev, kcur, 32); memcpy(kcur, keys, 32);
  memcpy(pprev, pcur, sizeof pcur); memcpy(pcur, pads, sizeof pcur);
  memset(&I, 0, sizeof I);

  static const int K_FIRE[] = {WC_KEY_J, WC_KEY_F, WC_KEY_LCTRL}, K_SCOPE[] = {WC_KEY_K, WC_KEY_Q},
                   K_ROLL[] = {DS_KEY_LSHIFT, DS_KEY_RSHIFT}, K_JUMP[] = {WC_KEY_SPACE},
                   K_OK[] = {WC_KEY_ENTER, WC_KEY_SPACE, WC_KEY_J}, K_BACK[] = {WC_KEY_ESCAPE, WC_KEY_BACKSPACE};
  I.mx = (DOWN(kcur, WC_KEY_D) ? 1.f : 0.f) - (DOWN(kcur, WC_KEY_A) ? 1.f : 0.f);
  I.my = (DOWN(kcur, WC_KEY_W) ? 1.f : 0.f) - (DOWN(kcur, WC_KEY_S) ? 1.f : 0.f);
  I.ax = (DOWN(kcur, WC_KEY_RIGHT) ? 1.f : 0.f) - (DOWN(kcur, WC_KEY_LEFT) ? 1.f : 0.f);
  I.ay = (DOWN(kcur, WC_KEY_UP) ? 1.f : 0.f) - (DOWN(kcur, WC_KEY_DOWN) ? 1.f : 0.f);
  I.jump = any_kp(K_JUMP, 1); I.jump_held = any_k(K_JUMP, 1);
  I.fire = any_kp(K_FIRE, 3); I.fire_held = any_k(K_FIRE, 3);
  I.scope = any_k(K_SCOPE, 2);
  if (kp(WC_KEY_R) || kp(WC_KEY_EQUAL)) I.zoom = 1;
  if (kp(WC_KEY_V) || kp(WC_KEY_MINUS)) I.zoom = -1;
  I.roll = any_kp(K_ROLL, 2); I.decoy = kp(WC_KEY_E); I.swap = kp(WC_KEY_TAB);
  I.pause = kp(WC_KEY_ESCAPE) || kp(WC_KEY_ENTER) || kp(WC_KEY_P);
  I.ok = any_kp(K_OK, 3); I.back = any_kp(K_BACK, 2);
  int dx = (DOWN(kcur, WC_KEY_D) || DOWN(kcur, WC_KEY_RIGHT)) - (DOWN(kcur, WC_KEY_A) || DOWN(kcur, WC_KEY_LEFT));
  int dy = (DOWN(kcur, WC_KEY_S) || DOWN(kcur, WC_KEY_DOWN)) - (DOWN(kcur, WC_KEY_W) || DOWN(kcur, WC_KEY_UP));
  bool any_key = false;
  for (int i = 0; i < 32; i++) if (kcur[i] & ~kprev[i]) any_key = true;
  if (any_key) last_pad = false;

  for (int p = 0; p < 4; p++) {
    const wc_pad_t *n = &pcur[p], *o = &pprev[p];
    if (!n->connected) continue;
    uint32_t b = n->buttons, pb = o->buttons, pr = b & ~pb;
    float lx = stick(n->left_x), ly = -stick(n->left_y), rx = stick(n->right_x), ry = -stick(n->right_y);
    if (b & WC_BTN_LEFT) lx = -1;
    if (b & WC_BTN_RIGHT) lx = 1;
    bool lt = n->left_trigger > WC_TRIGGER_PRESSED, rt = n->right_trigger > WC_TRIGGER_PRESSED;
    bool plt = o->left_trigger > WC_TRIGGER_PRESSED, prt = o->right_trigger > WC_TRIGGER_PRESSED;
    if (pr || (rt && !prt) || (lt && !plt) || fabsf(lx) + fabsf(ly) + fabsf(rx) + fabsf(ry) > 0.3f) last_pad = true;
    if (fabsf(lx) > fabsf(I.mx)) I.mx = lx;
    if (fabsf(ly) > fabsf(I.my)) I.my = ly;
    if (fabsf(rx) > fabsf(I.ax)) I.ax = rx;
    if (fabsf(ry) > fabsf(I.ay)) I.ay = ry;
    I.jump |= (pr & WC_BTN_A) != 0; I.jump_held |= (b & WC_BTN_A) != 0;
    I.fire |= rt && !prt; I.fire_held |= rt;
    I.scope |= lt;
    if (lt) {
      if (pr & (WC_BTN_R | WC_BTN_UP)) I.zoom = 1;
      if (pr & (WC_BTN_L | WC_BTN_DOWN)) I.zoom = -1;
    } else {
      if (b & WC_BTN_UP) I.my = 1;
      if (b & WC_BTN_DOWN) I.my = -1;
    }
    I.roll |= (pr & WC_BTN_B) != 0; I.decoy |= (pr & WC_BTN_X) != 0; I.swap |= (pr & WC_BTN_Y) != 0;
    I.pause |= (pr & WC_BTN_START) != 0;
    I.ok |= (pr & (WC_BTN_A | WC_BTN_START)) != 0; I.back |= (pr & WC_BTN_B) != 0;
    int px = (b & WC_BTN_RIGHT || n->left_x > 16000) - (b & WC_BTN_LEFT || n->left_x < -16000);
    int py = (b & WC_BTN_DOWN || n->left_y > 16000) - (b & WC_BTN_UP || n->left_y < -16000);
    if (px) dx = px;
    if (py) dy = py;
  }
  I.nav_x = dx != nav_ox ? dx : 0; I.nav_y = dy != nav_oy ? dy : 0;
  nav_ox = dx; nav_oy = dy;
  I.used_pad = last_pad;
}

const intent_t *input_get(void) { return &I; }
bool input_start_held(void) {
  for (int p = 0; p < 4; p++) if (pcur[p].connected && (pcur[p].buttons & WC_BTN_START)) return true;
  return false;
}
