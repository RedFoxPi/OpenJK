# Render-regression harness

Headless screenshot capture for exercising the renderer across a range of
maps/features, and for later diffing one renderer implementation against
another (e.g. rd-vanilla vs. a future Metal renderer).

Runs entirely headless via SDL2's `offscreen` video driver + software GL
(Mesa llvmpipe) - no display or Xvfb required.

## Requirements

- A build of the engine (`openjk_sp.x86_64`/`openjk.x86_64` plus their
  renderer and game `.so`s, e.g. via `make install`).
- Your own legally-owned copy of the game assets (`base/assets0-3.pk3`).
  Neither this script nor `scenes.json` contain any game content - only
  map identifiers (plain strings like `"academy1"`).

**Do not commit game assets, screenshots, or capture logs to this
repository.** Point `--basepath`/`--homepath`/`--out` somewhere outside
the working tree.

## Usage

```sh
python3 capture.py \
    --bindir  /path/to/install/JediAcademy \
    --basepath /path/to/gamedata \
    --homepath /path/to/gamedata/home \
    --out /path/to/gamedata/screenshots

# Just a subset:
python3 capture.py --bindir ... --basepath ... --homepath ... --out ... --filter sp_academy1
```

To run under ASan/UBSan, point `--bindir` at a build compiled with
`-fsanitize=address,undefined` and set `ASAN_OPTIONS`/`UBSAN_OPTIONS` in
the environment before invoking - the script inherits the parent
environment. The script scans each scene's log for sanitizer report
markers and flags it as `SANITIZER` in the summary even if the process
otherwise exited cleanly.

## Known gotcha

`make install` places the game/renderer `.so`s in `<prefix>/OpenJK/`
alongside the executables, but the engine's mod search path looks for
them under `fs_basepath/OpenJK/` or `fs_basepath/base/` - not next to the
executable. Copy them into your `fs_basepath`'s `base/` (or `OpenJK/`)
directory alongside the `.pk3`s, e.g.:

```sh
cp install/JediAcademy/OpenJK/jagamex86_64.so   gamedata/base/
cp install/JediAcademy/rdsp-vanilla_x86_64.so   gamedata/base/
cp install/JediAcademy/OpenJK/jampgamex86_64.so gamedata/base/
cp install/JediAcademy/OpenJK/cgamex86_64.so    gamedata/base/
cp install/JediAcademy/OpenJK/uix86_64.so       gamedata/base/
cp install/JediAcademy/rd-vanilla_x86_64.so     gamedata/base/
```

## Scene manifest (`scenes.json`)

Each entry: `id` (used for filenames), `binary` (`sp` or `mp`, selects
which executable/renderer), `map` (BSP name, or `null` for a menu-only
scene), `wait_frames` (settle time before the screenshot), optional
`extra_set` (cvar overrides), and `notes` describing what it's meant to
exercise. The starter set covers: UI/2D-only, indoor+outdoor static
geometry with a skeletal NPC, snow/outdoor terrain, foliage/vegetation,
dark atmospheric lighting/fog, and both the SP and MP renderer code
copies (`code/rd-vanilla` vs `codemp/rd-vanilla` are separate,
independently-maintained copies of the same renderer).

Pixel-diff comparison tooling (for comparing two renderers against each
other, once a second renderer exists) is not built yet - this is capture
only for now.
