# rd-vulkan (work in progress)

A Vulkan renderer plugin for the SP (`code/`) engine, implementing the same
`refexport_t`/`GetRefAPI()` plugin ABI as `rd-vanilla`, selectable at runtime
via `cl_renderer rd-vulkan`. Target platforms: Linux and Android natively,
iOS via MoltenVK (Vulkan-to-Metal translation, not yet tested on an actual
Apple device - the `__APPLE__`-gated extension/flag code is there but
unverified).

**This is a first-pass scaffold, not a feature-complete renderer.** Read this
before assuming any particular scene will render correctly - and see
"Verified state" below for what's actually been run and checked, as opposed
to just compiled.

## Verified state

Run against the real game's `sp_menu` scene, headlessly, under Xvfb + Mesa's
`lavapipe` software Vulkan driver (see "Testing headlessly" below), and
scored with `tests/render-regression/diff.py` against a same-scene
`rd-vanilla` reference screenshot:

- Instance/device/swapchain/render pass/pipeline creation succeeds, no
  validation errors, no crashes.
- The actual main menu renders correctly: title, background art, buttons,
  radar/starfield graphic, and glow/highlight compositing all match
  `rd-vanilla`'s output closely. Currently **`MINOR_DIFF`, 1.5% mean pixel
  difference** (down from an initial 15.9%/`MAJOR_DIFF` - see "History" in
  this section).
- `screenshot_png` reads back real, correct pixel data (used for the above).

Remaining known gaps on this scene: a starfield "twinkle" dot overlay on the
radar graphic isn't drawn, the scrolling reflection overlay on the title logo
(`gfx/menus/jediacademy`'s second stage) isn't drawn, and the scrolling
Matrix-style side-column decorations differ (all three are genuinely
*animated* effects - a screenshot taken at a slightly different frame will
never match pixel-for-pixel even between two runs of the *same* renderer, so
some residual diff here is expected, not just "not implemented yet"). Beyond
that, this renderer's `.shader` support (see below) only ever looks at a
shader's *first* stage, so any effect that depends on additional stages
compositing on top (glow overlays, multi-layer detail texturing) won't
reproduce those extra layers - only the base layer.

**History**: the first working screenshot scored 15.9% mean diff
(`MAJOR_DIFF`) despite the menu being visually recognizable, because a
solid **opaque white square** covered the radar/starfield decoration -
`RegisterShaderNoMip("gfx/menus/videologo")` (a `videoMap`-only shader with
no direct image fallback) failed to resolve and fell back to handle 0
("white"), which is the correct behavior for a caller that *intentionally*
passes 0, but wrong for a failed lookup. Splitting those two cases -
`RE_RegisterShaderNoMip` now returns a dedicated transparent placeholder on
failure instead of reusing handle 0 - dropped the diff to 3.8% in one fix,
without touching `.shader` parsing at all.

Adding minimal `.shader` parsing (`tr_shader.cpp` - first stage's `map`/
`blendFunc` only, see below) and three blend-mode pipeline variants dropped
the diff further, from 3.8% to **1.5%**, in two steps:
- Additive blending (`blendFunc GL_ONE GL_ONE`, e.g. `menu_buttonback`'s glow)
  had essentially no measurable effect on its own - that stage's contribution
  to this particular scene turned out to be small.
- The bigger win was realizing a *missing* `blendFunc` keyword is not the
  same as standard alpha blending: rd-vanilla treats an unspecified
  `blendFunc` as **blending disabled** (opaque overwrite - see
  `tr_shader.cpp`'s `ParseStage` in `rd-vanilla`, which defaults
  `blendSrcBits`/`blendDstBits` to 0). This renderer's default before this
  fix was to alpha-blend everything, which made `gfx/menus/jediacademy` (the
  "STAR WARS" title logo, whose first stage relies on rgbGen vertex + no
  blendFunc = opaque) render washed-out/translucent against the dark
  background instead of at full brightness. Fixing the *default* blend mode
  for a defined-but-blendFunc-less stage (`BLEND_OPAQUE` in `tr_local.h`)
  dropped mean diff from 3.8% to 1.5% - more than the additive fix, despite
  being "just" a more accurate default.

## Bugs found and fixed during that verification (worth knowing about if you
touch this code)

- **Render pass was missing an exit dependency.** `RE_EndFrame` copies the
  swapchain image to a readback buffer and transitions it to present layout
  immediately after `vkCmdEndRenderPass`, in the same command buffer. Without
  an explicit `subpass 0 -> VK_SUBPASS_EXTERNAL` dependency, nothing
  guarantees the color attachment writes are visible to that copy - command
  order in a command buffer does not imply memory visibility in Vulkan.
  Symptom was a normal-looking clear color but no draw content ever visible
  in the readback. Fixed in `VK_CreateRenderPass()`.
- **Push-constant struct didn't match GLSL's alignment rules.** The C++ side
  had `struct { float viewportSize[2]; float color[4]; }` (24 bytes, tightly
  packed), but GLSL's `layout(push_constant)` block requires `vec4` members
  to start on a 16-byte boundary, so the shader's `color` was actually being
  read starting at byte 16 of the pushed data - i.e. the tail of the C++
  struct's own `color` array plus out-of-bounds memory past what was ever
  pushed via `vkCmdPushConstants`. Getting this wrong doesn't error or show
  up in validation the way you'd hope; it just silently reads the wrong
  bytes. Symptom: draws with correct-looking RGB (the wrong bytes happened to
  read mostly-white color data for most UI elements) but effectively zero
  alpha, so every draw blended as fully transparent - a screen that stayed
  the clear color no matter what was drawn. Fixed by adding explicit padding
  to `vkPushConstants_t` in `tr_local.h` so its layout matches the shader's.
  **If you add fields to the push constant struct or the shaders, re-check
  this alignment by hand** - nothing will catch a mismatch for you.

## What's actually implemented

- Real Vulkan bring-up: instance, physical/logical device, swapchain, render
  pass, framebuffers, per-frame command buffers and sync objects.
- A working, verified 2D textured-quad draw path (`RE_StretchPic`, the
  function the entire UI/menu layer and font glyph rendering go through).
  Blending, scissoring via viewport, and `SetColor` all work correctly.
- Texture registration (`RegisterShader`/`RegisterShaderNoMip`): the named
  image is decoded and uploaded as a single RGBA texture (one `image_t` per
  name, cached), same as before. What's new is `tr_shader.cpp`: a minimal
  `.shader` script scanner that looks up the same name in the enumerated
  `shaders/*.shader` files (`ri.FS_ListFiles("shaders", ".shader", ...)`,
  matching rd-vanilla's convention) and records *only* its first stage's
  blend mode (`vkBlendMode_t` in `tr_local.h`: `BLEND_ALPHA`,
  `BLEND_ADDITIVE`, or `BLEND_OPAQUE` for a stage with no `blendFunc`
  keyword at all - see "History" above for why that distinction matters).
  `RE_StretchPic` picks the matching one of three baked `VkPipeline`
  variants per draw. This is still not real `.shader` support: later
  stages, `tcMod` animation, `rgbGen` (other than the implicit push-constant
  color), sky, and fog are all ignored, so a shader whose *effect* depends
  on more than its first stage's base texture and blend mode won't
  reproduce that effect (see "Verified state" above for what this actually
  looks like on a real menu, and what's specifically missing).
- `screenshot_png` and `GetScreenShot`, via a persistent readback image
  copied out of the swapchain every frame - this is what
  `tests/render-regression` needs to work at all, and it's been verified to
  produce correct pixel data, not just "a file gets written."

## What's not implemented yet (safe no-ops, won't crash, won't draw)

- 3D world/BSP rendering (`RenderScene`, `LoadWorld`, `AddRefEntityToScene`,
  `AddPolyToScene`, `AddLightToScene`) - the biggest remaining piece.
- Ghoul2 (character/weapon model) rendering entirely, including the CPU-side
  bone/skeleton math - see "Ghoul2 is not reused from rd-vanilla" below for
  why. Every `G2API_*` entry point is a safe stub; no character or weapon
  models will animate, attach, or render.
- Full `.shader` script parsing: only a defined shader's first stage's
  `map`/`blendFunc` is read (see "What's actually implemented" above) -
  later stages, `tcMod` animation, `rgbGen`/`alphaGen` waves, sky, and fog
  are all ignored.
- Cinematics (`DrawStretchRaw`/`UploadCinematic`), rotated pics, weather/world
  effects, dissolves, model bounds/tag queries.
- Window resize / swapchain recreation - a resize will currently just log a
  warning and stop rendering rather than crash.

## Ghoul2 is not reused from rd-vanilla

The original plan for this scaffold was "the Ghoul2 CPU skeletal-animation
system (`G2_*.cpp`) has zero direct OpenGL calls, so compile it in unchanged."
That turned out to be true but insufficient: those files use
`#include "tr_local.h"` as a quote-include, which resolves against their
*own* directory (`code/rd-vanilla/`) before any `-I` search path, so
compiling them here silently pulls in rd-vanilla's real, GL-coupled
`trGlobals_t`/`model_t`/`shader_t` types - and then needs rd-vanilla's actual
model/shader/skin registry (`R_GetModelByHandle` et al., defined in
`tr_model.cpp`/`tr_shader.cpp`) to actually link. That's not "free" reuse,
it's "port rd-vanilla's whole asset-parsing layer too." Given that's a
separate, larger task, `G2API_*` is stubbed instead for this pass (see
`tr_init.cpp`); `CGhoul2Info_v`'s backing store (`IGhoul2InfoArray`) is
implemented for real since it's small, self-contained, and lets UI code that
merely checks "do I have a ghoul2 model" behave sanely.

## Reuse strategy for the next passes

`tr_shader.cpp`/`tr_bsp.cpp`/`tr_model.cpp` (and `tr_ghoul2.cpp`, `G2_*.cpp`)
from rd-vanilla parse asset formats into CPU data structures and are largely
graphics-API-agnostic *except* where they also issue GL calls inline (texture
upload, state changes) or depend on rd-vanilla's own directory-local
`tr_local.h` (see above). Real reuse needs those split into "parse"
(reusable as-is or with the include fixed) and "submit to GPU"
(Vulkan-specific, new) - and the include-resolution gotcha needs a real fix
(e.g. compiling a rd-vulkan-local copy that includes *this* directory's
tr_local.h, or restructuring rd-vanilla's includes to not rely on
same-directory resolution) before anything from `code/rd-vanilla/` can be
compiled into this target and get *this* target's types.

## Building

Requires the Vulkan SDK (loader + headers) and `glslangValidator` (part of
`glslang-tools` on Debian/Ubuntu). Configure with `-DBuildSPRdVulkan=ON`; if
either dependency is missing, the CMake configure step disables this target
with a warning rather than failing the whole build.

## Testing headlessly

Mesa's `lavapipe` software Vulkan driver (package `mesa-vulkan-drivers`) is
the Vulkan equivalent of `llvmpipe` for OpenGL, and works the same way for
headless testing - **except** that SDL2's `offscreen` video driver, which
works fine for headless GL testing, does not support creating a
`SDL_WINDOW_VULKAN` window (`SDL_CreateWindow` fails with "Invalid window").
Use a real (virtual) X server instead:

```
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json   # if not auto-detected
export SDL_VIDEODRIVER=x11
xvfb-run -a --server-args="-screen 0 800x600x24" ./openjk_sp.x86_64 \
    +set cl_renderer rd-vulkan +set r_customwidth 640 +set r_customheight 480 \
    +set in_nograb 1 +wait 120 +screenshot_png test +quit
```
