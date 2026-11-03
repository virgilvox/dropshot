# DROPSHOT

Juno Kade is a flamingo who washes windows in orbit. Squatters have taken over three derelict arcology ships, and she is the only one up there with a scope, a squeegee wing and a de-icing chain gun.

Marguerite, her dispatcher, talks her through it on the radio. Six in-engine cutscenes carry the story from a gondola on tower 9 to the Superintendent's office and back (hold START or Enter to skip one). `docs/STORY.md` is the story bible every line follows.

A third-person run-and-gun for the HeatSync Labs game station (wasmcart, Batocera) and the browser. Each level opens with an orbital drop through rings, then decks, glides and sniping, and the last one ends at The Superintendent, a building-sized mech you take apart through its weak points.

## Play

Play it in the browser at **https://virgilvox.github.io/dropshot/**, or download `dropshot.html` from the latest release (or build it, below) and open it. It is one file with the game inside: double-click it, no server needed. Click the game to aim with the mouse. Radio calls and dialogue are spoken aloud.

| | Pad | Keyboard | Mouse |
|---|---|---|---|
| Move | left stick | WASD | |
| Aim | right stick | arrows | move |
| Jump, glide (hold in air) | A | Space | |
| Fire | RT | J or F | left button |
| Scope (hold) | LT | K or Q | right button |
| Zoom while scoped | RB / LB | R / V | wheel |
| Roll | B | Shift | |
| Decoy | X | E | |
| Swap weapon | Y | Tab | |
| Pause | START | Enter or Esc | |

On the station, copy `build/dropshot.wasc` to `/userdata/roms/wasmcart/` (back up `gamelist.xml` first if you add an entry).

## Build

```sh
./build.sh                 # build/dropshot.wasc and web/dropshot.html
REGEN_STATES=1 ./build.sh  # after adding states to build/missing.txt
tools/publish-pages.sh     # put the built web/dropshot.html on GitHub Pages (the gh-pages branch)
```

Needs emsdk and Node 22. Clone with `--recursive` (or the first build fetches them): three.c and wasmcart are git submodules, and `patches/three.c.patch` is applied to three.c on the first build. Regenerating the shader table needs three.c's own packages: `(cd third_party/three.c && npm install --ignore-scripts)`.

`tools/shot.mjs` runs the cart headless with scripted input and writes screenshots. If the cart logs a line like `[cart] standard+map+fog+rep+shadow+l1001`, that shader state is missing from the program table: add it to `build/missing.txt` and rebuild with `REGEN_STATES=1`. `EXTRA_CFLAGS=-DDS_TRACE OUT=build-trace ./build.sh` builds a test cart that logs the player state once a second, with 9 to jump to the level 3 arena and 8 to refill health.

`docs/DESIGN.md` has the premise, the levels and which source file owns what. `src/cutscene.c` and `src/cine_set.c` hold the cutscenes and the radio script; `world_story_trigger` in `src/world.c` places the radio beats.

## Credits

- Characters, enemies and most of the ships: Quaternius (Ultimate Space Kit), CC0. Juno is its flamingo astronaut, recoloured, with the scope and wing added in code.
- Station pieces: Kenney (Space Station Kit), CC0.
- Fonts: Archivo, IBM Plex Mono and JetBrains Mono, under the SIL Open Font License (`assets/fonts/OFL.txt`).
- Speech: CMU Flite and its cmu_us_slt voice (BSD-style licence, `third_party/flite/COPYING`), compiled into the cart.
- canvas_ity by Andrew Kensler (ISC licence), for the HUD.
- Code, music and sound: original, written for this game, under the MIT licence (`LICENSE`).
