# DROPSHOT design

A third-person run-and-gun with a long-range scope, for the HeatSync game station (wasmcart, Batocera) and the browser.

## Premise

Juno Kade is a flamingo, and she washes windows on orbital habitats. Three derelict arcology ships have drifted into low orbit over the colony, and a species of squatters (the Tenants) has moved in. They are cutting the colony's towers loose to build nests. Nobody else is up there. Juno has her washer rig: a helmet scope for spotting streaks at three kilometres, a squeegee-wing glider for drops between towers, and a de-icing chain gun.

The tone is deadpan and a little surreal. Signs on the ships are passive-aggressive building-management notices. The Tenants leave complaint forms. Bosses are building managers.

## The loop

Each level starts with an **orbital drop**: Juno falls from high above the ship, steering through rings for points, then opens the glider and lands. Then it's run-and-gun across decks, towers and gaps, with glides between platforms and sniping from cover. The level ends at a **boss** you only beat by scoping in and shooting glowing weak points.

## Controls

| | Pad | Keyboard | Mouse (web page) |
|---|---|---|---|
| Move | left stick, d-pad | WASD | |
| Aim / camera | right stick | arrows | move (pointer lock) |
| Jump, glide (hold in air) | A | Space | |
| Fire | RT | J, F, Left Ctrl | left button |
| Scope (hold) | LT | K, Q | right button |
| Zoom in / out while scoped | RB / LB, d-pad up / down | R / V, = / - | wheel |
| Roll | B | Shift | |
| Decoy | X | E | |
| Swap weapon | Y | Tab | |
| Pause | START | Enter, Esc, P | |

Weapons: the **chain gun** (hip fire, infinite, spreads as it spins up) and **scope shots** (fired while scoped, limited ammo, big damage, 2x on weak points).

## Levels

1. **Curtain Wall**: the outer hull of the first ship. Wide decks, solar fields, gaps to glide over, grunts, flyers and turrets. Ends with a Brute.
2. **Hydroponic Spine**: a long greenhouse spine of floating gardens over a chasm. Sniper alleys, flyer swarms, moving platforms. Ends with a Brute pair.
3. **Cold Storage**: the Lessee's freezer holds, racks of the owners' never unpacked furniture, cargo conveyors, a heater core. Ends with a Brute.
4. **The Annex**: Halcyon Reach's own glass towers, glides and window gondolas between roofs, Brutes bolting turrets to the colony. Ends with a Brute arena and the tram.
5. **The Basement**: the Freehold's undercroft, filing cabinet canyons and server aisles, a Brute pair, and the eviction relay.
6. **The Superintendent**: a ring arena on the third ship's crown. The boss is a giant mech with weak points on its back, shoulders and core. Three phases.

## Code layout and ownership

| File | Owns |
|---|---|
| `src/main.c` | wasmcart glue, render target, present |
| `src/game.c`, `game.h` | the state machine, the player, the camera and scope, the orbital drop, glue between modules |
| `src/input.c`, `input.h` | pads and keyboard into one `intent_t` |
| `src/world.c`, `world.h` | level geometry from the asset kits, collision, pickups, spawns, exits, checkpoints |
| `src/enemy.c`, `enemy.h` | enemies, their AI and attacks, the boss, weak points |
| `src/fx.c`, `fx.h` | particles, tracers, muzzle flashes, explosions, decals |
| `src/hud.c`, `hud.h` | the HUD, the scope overlay, every screen and menu (drawn with cv.h) |
| `src/audio.c`, `audio.h` | the synth: effects and music moods |
| `src/voice.c`, `src/tts.c` | speech: CMU Flite in the cart, coloured per speaker, ducking the music |
| `src/cutscene.c`, `src/cine_set.c`, `src/hud_cine.c` | cutscenes, their sets, captions, and the radio script |
| `web/index.src.html` | the one-file web player |

Units: metres, y up. Juno is about 1.8 m tall. Gravity is 24 m/s².

## Assets

Everything comes from `../../3d-assets` and is CC0. The models used are copied into `assets/models/` as GLB:

- Juno: the flamingo from the Quaternius Ultimate Space Kit `Astronaut`, recoloured into a hi-vis suit, with a helmet scope and a squeegee wing added in code.
- Tenants: the Ultimate Space Kit enemies (`Enemy Large`, `Enemy Small`, `Enemy Flying`) and `Mech` for Brutes and the boss.
- The ships: Ultimate Space Kit buildings, supports, solar panels and plants, plus Kenney Space Kit and Space Station Kit pieces.

Keep the cart under 8 MB, since the web player carries it as base64.
