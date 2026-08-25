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

## Recommended: CMake targets

Configuring, building, and manually copying the right `.so`s into the right
`fs_basepath` directory before running `capture.py` by hand is exactly the
kind of multi-step manual process that's easy to get subtly wrong once and
not notice - see `code/rd-vulkan/README.md`'s "A capture-harness bug that
invalidated a run of 'verified' claims" section for a real incident where a
hand-typed path to a renderer `.so` silently kept resolving to a stale
leftover build from an earlier point in the project, invalidating an entire
session's worth of "verified" screenshots before the mixup was caught. The
targets below exist to make that specific mistake structurally impossible:
every binary they stage is named via CMake's own "wherever this target's
build actually produced its output" mechanism, never a hand-typed path.

```sh
# Once, or whenever your game install moves:
cmake -B build -DRenderRegressionBasepath=/path/to/gamedata ...

# Any time after that:
ninja -C build render_regression_vulkan   # rd-vulkan only
ninja -C build render_regression_vanilla  # rdsp-vanilla only
ninja -C build render_regression_diff     # both, then diff.py between them
ninja -C build render_regression          # alias for render_regression_diff
```

`RenderRegressionBasepath` can also be overridden per-invocation, with no
reconfigure, via the `RENDER_REGRESSION_BASEPATH` environment variable
(checked first) - useful for CI or for switching between game installs
without touching the CMake cache. These targets always run - they're not
part of the default `ninja`/`make` build (no dependency from `all`) and
CMake has no way to know if game/screenshot content changed since the last
run, so treat them like a test suite you invoke on demand, not a build
artifact that's cached.

Output lands under `<build dir>/render-regression/`: `JediAcademy/` (the
staged binaries), `home-<renderer>/` (fresh every run - see
`run_capture.sh`'s own comment on why), `screenshots-<renderer>/`, and
`diff/` (from `render_regression_diff`). None of it is committed - the
whole `<build dir>` is gitignored already.

Only exercises the **SP** scenes in `scenes.json` (filtered via
`--filter sp_`) - these targets only stage the SP engine/gamecode/renderer
binaries (`BuildSPEngine`/`BuildSPGame`/`BuildSPRdVanilla`/
`BuildSPRdVulkan`). For MP scene coverage, build the MP targets
(`BuildMPEngine` etc.) and invoke `capture.py` directly as described below.

`render_regression_diff`'s underlying `diff.py` exits non-zero whenever it
finds so much as one `MINOR_DIFF`/`MAJOR_DIFF` scene, not just on a real
script error - so `ninja`/`make` will report that target (and the
`render_regression` alias) as failed any time the two renderers'
output differs meaningfully, which is still the expected, documented
baseline at this stage of `rd-vulkan`'s development (see
`code/rd-vulkan/README.md`). Read the actual per-scene summary this target
prints, and the `.diff.png` images it writes, rather than treating a bare
"FAILED" as a regression signal by itself.

## Manual usage (what the targets above do under the hood)

Useful directly for a one-off invocation, a scene subset, or a renderer/
engine combination the CMake targets above don't cover (MP scenes, ASan
builds, ...).

### Requirements

- A build of the engine (`openjk_sp.x86_64`/`openjk.x86_64` plus their
  renderer and game `.so`s, e.g. via `make install`).
- Your own legally-owned copy of the game assets (`base/assets0-3.pk3`).
  Neither this script nor `scenes.json` contain any game content - only
  map identifiers (plain strings like `"academy1"`).

**Do not commit game assets, screenshots, or capture logs to this
repository.** Point `--basepath`/`--homepath`/`--out` somewhere outside
the working tree.

### Usage

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

### Known gotcha

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
scene), a settle time before the screenshot (`wait_ms` for any scene with
a `map` - see below for why `wait_frames` isn't enough for those - or
`wait_frames` for a menu-only scene, where there's no map-load/ICARUS
timing to get wrong), optional `extra_set` (cvar overrides), and `notes`
describing what it's meant to exercise. The starter set covers: UI/2D-only,
indoor+outdoor static geometry with a skeletal NPC, snow/outdoor terrain,
foliage/vegetation, dark atmospheric lighting/fog, and both the SP and MP
renderer code copies (`code/rd-vanilla` vs `codemp/rd-vanilla` are
separate, independently-maintained copies of the same renderer).

**Every map-based scene sets `fixedtime` (currently `16`, i.e. a fixed
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
`fixedtime` forces every frame, regardless of how long it actually took
to render, to advance the simulated clock by exactly that many
milliseconds - making `wait_frames × fixedtime` a precise, renderer-
speed-independent target instead. This was found and verified empirically,
not assumed: see `code/rd-vulkan/README.md`'s "Testing headlessly" and
"Live animation" sections for the investigation (a case where two
renderers landed on unrelated moments of the same hoth2 cutscene at an
identical `wait_frames` without this, and matched closely with it) that
led to adding it here. If you add a scene with a `map`, set it too, unless
the scene is deliberately testing something time-*independent* (a static
frame-0-equivalent snapshot) where simulated time genuinely doesn't matter.

**The cvar's real name is `fixedtime`, not `com_fixedtime`** (its C++
variable is `com_fixedtime` in `qcommon/common.cpp`, but the string
actually passed to `Cvar_Get` - what `+set`/`extra_set` must use - is
`"fixedtime"`). `scenes.json` used the wrong key (`com_fixedtime`) from
the point this section was first added until it was caught during a later
animation-comparison investigation - `+set com_fixedtime 16` silently
creates a brand new, unrelated, never-read cvar (`+set` auto-vivifies
unknown cvars) rather than erroring, so every capture taken with the wrong
key ran at genuinely uncontrolled `wait_frames`-counts-real-frames timing
the whole time, with nothing in the tool's output to suggest otherwise.
See `code/rd-vulkan/README.md`'s "A capture-harness bug that invalidated a
run of 'verified' claims" section (the *second* one - the earlier
harness-bug section covers a different, `cl_renderer`-related mistake) for
the full account and what it changed once actually fixed. If you're
scripting captures directly rather than through `scenes.json`, double-check
you're typing `+set fixedtime <ms>`, not `+set com_fixedtime <ms>`.

**Even with the cvar name fixed, `wait_frames` still isn't enough for a
scene with a `map` - use `wait_ms` (see the "waittime" console command,
`qcommon/cmd.cpp`) instead.** This is a *third*, deeper timing bug, found
by tracing a specific character's pose directly (not just diffing pixels)
after a user reported it still looked wrong even with `fixedtime` correctly
named: `wait <N>` counts real `Cbuf_Execute` calls (one per engine frame),
regardless of how much simulated time each one represents. `fixedtime`
makes *that part* deterministic - but `devmap`'s own map load, before
`wait <N>` even starts counting, can itself consume a real-time-dependent,
renderer-speed-dependent number of those calls (loading-screen ticks,
precache retries, whatever the underlying cause on a given build). Two
renderer builds that take a different number of frames to get through
*that* land at genuinely different absolute simulated times by the time
`wait <N>` finishes, even though `fixedtime` is correctly set and named -
confirmed empirically at ~3.4 seconds apart between two builds at an
identical `wait_frames: 300`, by comparing each renderer's own internal
animation-frame accounting (not screenshots) for the literal same NPC.
`waittime <ms>` (the console command backing `scenes.json`'s `wait_ms`
field) fixes this by targeting an *absolute* point on the engine's own
`com_fixedSimTime` clock - which only ever advances via each frame's real,
`fixedtime`-forced `msec` (`qcommon/common.cpp`'s `Com_Frame`) - rather
than a fixed frame count from wherever the script happened to already be.
Whatever `com_fixedSimTime` already is when `waittime` runs (whether the
map loaded in 100 frames or 400), the result always lands at exactly
`com_fixedSimTime + ms`, so two renderer builds converge to the same
absolute simulated time regardless of how differently their loading
performed. Verified: re-running the same two builds with `waittime 8000`
instead of an equivalent `wait_frames`, both landed within 6ms of each
other (down from the ~3.4 second gap) at the same command. `wait_frames`
is still fine, and preferred, for a scene with no `map` (nothing to load,
so nothing for this to apply to).

### Comparing two renderers (`diff.py`)

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
the same moment** - see the `fixedtime` and `wait_ms`/`waittime` notes
above. Scene captures taken before `fixedtime` was added *and correctly
named*, or before map-based scenes switched from `wait_frames` to
`wait_ms`, in `scenes.json` can't be trusted to isolate genuine rendering
differences from these timing confounds; a
`MAJOR_DIFF` on an old capture pair may partly or entirely be "two
different points in the same cutscene," not a rendering bug. Re-capture
with the current `scenes.json` before drawing conclusions from a diff on
any scene with a `map`.

This is a simple mean-pixel-difference metric, not perceptual/SSIM - good
enough to catch "obviously broken" (wrong colors, missing geometry, a blank
screen) and to see at a glance where two renders diverge, not a claim of
pixel-perfect parity.
