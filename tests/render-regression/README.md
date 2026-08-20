# Render-regression harness

Headless screenshot capture for exercising the renderer across a range of
maps/features, and for later diffing one renderer implementation against
another (e.g. rd-vanilla vs. a future Metal renderer).

Runs entirely headless via SDL2's `offscreen` video driver + software GL
(Mesa llvmpipe) - no display or Xvfb required, for GL-based renderers
(`rd-vanilla`). **`rd-vulkan` is the exception**: SDL2's `offscreen` driver
does not support creating a Vulkan-capable window, so testing it headlessly
needs Xvfb + the `x11` SDL driver + Mesa's `lavapipe` software Vulkan driver
instead - see `code/rd-vulkan/README.md`'s "Testing headlessly" section for
the exact invocation.

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
    --out /path/to/gamedata/screenshots \
    --renderer rd-vulkan

# Just a subset:
python3 capture.py --bindir ... --basepath ... --homepath ... --out ... --renderer rd-vulkan --filter sp_academy1
```

**Always pass `--renderer`.** `cl_renderer` is `CVAR_ARCHIVE` - once set, it
persists in `--homepath`'s own saved config across every future run against
that homepath, and a *fresh* `--homepath` (nothing saved yet) silently falls
back to the engine's compiled-in default (`rdsp-vanilla` for the `sp`
binary) - in both cases with no error, regardless of which renderer
`--bindir` was actually built for. This is exactly the bug that produced a
run of false-positive "no crash, verified" results during `rd-vulkan`
development: several checkpoints' capture runs used freshly-created
homepaths without `--renderer`, so every one of them silently exercised
`rdsp-vanilla` instead of the renderer actually being tested - not a rare
edge case, the default behavior with no `--renderer` flag. `--renderer` was
added specifically to make this impossible to get wrong by omission; there
is no scenario where skipping it is actually safer than passing it.

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

**Every map-based scene sets `com_fixedtime` (currently `16`, i.e. a fixed
16ms/frame simulated timestep) via `extra_set`** - this is what makes
`wait_frames` mean something comparable across two different renderer
builds/binaries at all. Without it, `wait_frames` counts *rendered client
frames*, not elapsed simulated game time: a slower renderer (any software
rasterizer, or one still early in development) takes longer per frame in
real wall-clock time, so by the time it's rendered its Nth frame, real-
time-paced content (NPC animations, scripted camera cuts, ICARUS cutscene
timing) has advanced far less than it has for a faster renderer at that
same `wait_frames` value - the two screenshots end up capturing genuinely
different moments of the scene, not the same moment rendered two ways.
`com_fixedtime` forces every frame, regardless of how long it actually took
to render, to advance the simulated clock by exactly that many
milliseconds - making `wait_frames × com_fixedtime` a precise, renderer-
speed-independent target instead. This was found and verified empirically,
not assumed: see `code/rd-vulkan/README.md`'s "Testing headlessly" and
"Live animation" sections for the investigation (a case where two
renderers landed on unrelated moments of the same hoth2 cutscene at an
identical `wait_frames` without this, and matched closely with it) that
led to adding it here. If you add a scene with a `map`, set it too, unless
the scene is deliberately testing something time-*independent* (a static
frame-0-equivalent snapshot) where simulated time genuinely doesn't matter.

## Comparing two renderers (`diff.py`)

Run `capture.py` twice - once per renderer, into separate `--out`
directories, each with the matching `--renderer` - then compare:

```sh
pip install pillow   # only needed for diff.py, not capture.py itself

python3 capture.py --bindir ... --renderer rdsp-vanilla --out screenshots-vanilla ...
python3 capture.py --bindir ... --renderer rd-vulkan    --out screenshots-vulkan  ...

python3 diff.py --a screenshots-vanilla --b screenshots-vulkan --out diffs
```

For each scene present in both directories, this prints a per-pixel mean
difference and a "% of pixels visibly changed" figure, classifies it as
`MATCH` / `MINOR_DIFF` / `MAJOR_DIFF`, and writes an amplified difference
image (`<scene>.diff.png`) so mismatches are visible at a glance rather than
having to eyeball two screenshots side by side. Exits non-zero if anything
isn't a `MATCH`, so it's usable as a regression gate once a renderer is far
enough along that `MATCH` is actually the expected outcome for most scenes -
right now, with `rd-vulkan`'s `.shader`-script gap (see
`code/rd-vulkan/README.md`), expect `MAJOR_DIFF` on most non-trivial scenes
and treat the diff image as a diagnostic, not a pass/fail signal.

**A `MAJOR_DIFF` is only informative if both screenshots are actually of
the same moment** - see the `com_fixedtime` note above. Scene captures
taken before `com_fixedtime` was added to `scenes.json` can't be trusted
to isolate genuine rendering differences from this timing confound; a
`MAJOR_DIFF` on an old capture pair may partly or entirely be "two
different points in the same cutscene," not a rendering bug. Re-capture
with the current `scenes.json` before drawing conclusions from a diff on
any scene with a `map`.

This is a simple mean-pixel-difference metric, not perceptual/SSIM - good
enough to catch "obviously broken" (wrong colors, missing geometry, a blank
screen) and to see at a glance where two renders diverge, not a claim of
pixel-perfect parity.
