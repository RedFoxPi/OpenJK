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

### 3D world geometry (tr_world.cpp)

Run against `academy1` (SP's first tutorial level), headlessly, loading the
real map via `devmap academy1` and letting it play out for several hundred
frames:

- The `.bsp`'s static, opaque, non-patch surfaces (see "What's actually
  implemented" below for exactly which) load, upload to GPU buffers, and
  render with a real camera derived from the game's own `refdef_t` each
  frame - correct perspective (parallel level geometry visibly converges
  toward vanishing points), correct depth testing (nearer surfaces occlude
  farther ones), and correctly UV-mapped diffuse textures. No crashes, no
  validation errors, across a full level load and several hundred rendered
  frames.
- This was **verified two ways**, not just by eyeballing a screenshot: (1)
  by hand-transforming a known world-space BSP vertex through the exact
  camera/projection matrices this code computes and confirming the resulting
  NDC coordinates land in a sane, expected range (this is how the matrix
  multiply order bug below was actually found and fixed - the screenshot
  alone wouldn't have distinguished "camera math is wrong" from "camera math
  is right but something else is missing"); (2) visually, the rendered
  screenshot shows coherent architecture with correct-looking perspective,
  not noise or a degenerate blob.
- World geometry is now **lit by the map's baked lightmap** (see
  `VK_LoadLightmaps` in `tr_world.cpp`), not flat/fullbright: `LUMP_LIGHTMAPS`
  is a flat array of fixed-size (128x128) RGB images with no explicit count
  or index (divide the lump length by one image's byte size), each uploaded
  as a plain texture; each surface's second UV set (`drawVert_t.lightmap[0]`,
  already parsed, just previously unused) samples its assigned lightmap
  (`dsurface_t.lightmapNum[0]`, or a 1x1 white fallback for surfaces with
  none - vertex-lit/fullbright cases), multiplied with the diffuse texture
  in `world.frag`. This is what makes a *non*-`r_fullbright` comparison
  against `rd-vanilla` meaningful for the first time - visually, the
  academy1 screenshot now shows real light shafts and shadowing (a grated
  ceiling casting light bars across the floor) instead of flat, uniformly-lit
  surfaces, and reads as a recognizable game screenshot rather than a
  lighting-flattened render.
- **Sky** now renders as a real camera-centered skybox (see `VK_LoadSky` in
  `tr_world.cpp`) instead of `SURF_SKY` surfaces just being skipped: the
  sky-flagged shader's name is used directly as a skybox basename (this
  renderer doesn't parse `.shader` `skyparms`, see "What's not implemented
  yet" below, so it can't follow a `skyparms` override to a *different*
  basename - it works here because academy1's sky shader's own name matches
  its face image names, the common case), its 6 `_rt`/`_lf`/`_bk`/`_ft`/
  `_up`/`_dn` faces are loaded and built into a box using rd-vanilla's exact
  `MakeSkyVec` corner formula (`tr_sky.cpp`) so face orientation matches
  rather than being guessed, and it's drawn first with depth test/write
  both off (a dedicated `vk.skyPipeline`) so ordinary depth-tested world
  geometry drawn afterward always overdraws it correctly. It's a flat
  6-quad box, not rd-vanilla's subdivided/warped dome, so expect visible
  seams at the box edges up close - a deliberate first-pass simplification,
  not a bug. Visually confirmed on academy1: the ceiling grate gaps now
  show hazy blue sky instead of solid black/clear-color.
- A pixel diff against an `rd-vanilla` reference (same map, same camera, no
  special cvars now that lighting is real) is still a **`MAJOR_DIFF`, ~40%
  mean pixel difference**, and that's expected, not a regression to chase
  down: academy1's spawn point is a scripted character-portrait shot
  (confirmed by comparing against a `capture.py` reference captured
  independently, much earlier, with default settings - same framing both
  times, so this is genuinely the level's intro camera, not an artifact of
  any flag used here) where the player's own Ghoul2 model fills a large
  fraction of the frame, and Ghoul2 rendering is entirely unimplemented
  (see "Ghoul2 is not reused from rd-vanilla" below). Given that, a large
  diff on *this specific scene* is exactly what "no models yet" predicts -
  it's not evidence the world-geometry, lighting, or sky path itself is
  wrong (see the hand-verified matrix math above, and the visual lightmap/
  sky checks just above, for that). Before lightmaps and sky landed, the
  same comparison needed `r_fullbright 1` on the vanilla side to be
  meaningful at all, and scored progressively 39% (initial) then 23%
  (after the `SURF_NODRAW`/dust-quad fixes below) mean diff - those numbers
  aren't directly comparable to the current ~40%, since fullbright-vs-
  fullbright and lit-vs-lit are different comparisons, not a regression
  between them.
- World surfaces are now **view-frustum culled** (`VK_ExtractFrustumPlanes`/
  `VK_AABBOutsideFrustum` in `tr_world.cpp`): each surface's AABB (computed
  once at load time from its own vertex range) is tested against the 6
  frustum planes extracted from that frame's view-projection matrix, and a
  surface entirely outside is skipped. **Verified two ways**, the same
  standard the camera math itself was held to: (1) on academy1, the same
  camera that gets a `MAJOR_DIFF` against vanilla above culled 265-284 of
  365 world batches (73-78%) depending on the frame, so the mechanism is
  doing real work, not a no-op; (2) a screenshot from that exact camera with
  culling enabled is **bit-for-bit identical**, outside a small console-text
  strip from this feature's own load-time log line, to the same screenshot
  captured before culling existed - i.e. removing ~75% of the draw calls
  changed nothing about what's actually visible, which is exactly what
  correct culling should do (an incorrect cull that drops something that's
  actually in view would very likely have shown up as a pixel difference,
  not passed silently). Frustum culling is unrelated to triangle winding
  order, so it doesn't run into the "no culling" backface concern below -
  it operates on bounding boxes, not face direction.
- **Bug found and fixed**: `SURF_NODRAW`-flagged surfaces (editor-only
  geometry - caulk, clip, trigger volumes; 3 of academy1's 16 shaders) and
  `SURF_SKY`-flagged surfaces weren't excluded at all, so each painted a
  stray, wrongly-textured polygon (these flags are on `dshader_t` itself,
  already parsed - the real fix was just checking them, no new parsing).
  More impactful: a shader with no plain image of its own - typically an
  effects-only shader built entirely from its stages' `map` references,
  which this renderer's first-stage-only `.shader` support (see "3D world
  geometry" above) doesn't parse for world geometry - was falling back to
  an **opaque white quad** (`vk.whiteImage`), the same mistake as the
  `videologo` white-square bug in the 2D path, just newly possible here
  because world geometry has no blend pipeline to fall back to instead.
  Concretely: academy1's `textures/common/dark_dust` (a `surfaceparm trans
  nonopaque` fog/dust volume with no base image, only an editor preview
  texture) was covering large parts of the screen in solid white. Since
  drawing it translucent isn't possible yet, the fix is to skip the surface
  entirely instead - "invisible" is honest about "not implemented yet",
  "solid white wall" is actively wrong. This alone dropped the vanilla diff
  from 39% to 23% and made the screenshot recognizably academy1's actual
  architecture (columns, floor, stairway) rather than mostly a white void.
- **Bug found and fixed**: the view/projection matrix combination was
  computed as `VK_MultiplyMatrix(projection, view, mvp)` - the "obvious"
  argument order - but `VK_MultiplyMatrix` (a byte-for-byte copy of
  rd-vanilla's `myGlMultMatrix`) computes `out = b * a` for column-major
  matrices, not `out = a * b`; the arguments are effectively swapped
  relative to normal multiplication notation. The wrong order silently
  computed `view * projection` instead of `projection * view`, producing
  clip-space coordinates in the thousands (should be roughly `[-1,1]` before
  the perspective divide's denominator) - a screen-filling, garbled result
  that LOOKED like "nothing renders" (actually: geometry projected so far
  outside the frustum it was entirely clipped) rather than an obviously
  wrong picture. Found by hand-transforming a known vertex and comparing
  against the expected NDC range (see above), not by inspection. Fixed by
  swapping the call to `VK_MultiplyMatrix(view, projection, mvp)`; see that
  function's comment in `tr_world.cpp` for the full explanation of why the
  arguments are order-swapped in the first place.
- **Bug found and fixed, unrelated to rendering**: `academy1` (like most SP
  levels) spawns the player as a Ghoul2 model, and this renderer's
  `G2API_InitGhoul2Model`/`RE_RegisterModel` stubs originally reported
  failure (`-1`/`0`). Game-side code (`g_client.cpp`'s
  `G_SetG2PlayerModel`, `cg_main.cpp`'s `misc_model_static` spawning) treats
  a failed model/skin registration as fatal (`Com_Error(ERR_DROP, ...)`),
  which aborted map loading entirely - `RE_LoadWorldMap`/`RE_RenderScene`
  never even got called. Fixed by having those stubs report success (with
  believable-enough fake data - an allocated-but-empty Ghoul2 slot, a
  standard-humanoid skeleton name) instead of failure; the model still
  doesn't render (Ghoul2 is still entirely stubbed), but game logic no
  longer treats "no Ghoul2 rendering yet" as a fatal error. Also needed:
  `CVulkanGhoul2InfoArray::New()` must never return handle `0` -
  `CGhoul2Info_v` (`game/ghoul2_shared.h`) uses `0` as its own "not
  allocated" sentinel, so a genuinely-valid handle of `0` was
  indistinguishable from "empty" and tripped `assert(mItem)` in
  `CGhoul2Info_v::operator[]`.
- **Bug found and fixed, unrelated to 3D specifically**: `VK_Shutdown`
  originally tore down the entire `VkDevice`/`VkInstance` unconditionally.
  `CL_FlushMemory()` (`client/cl_main.cpp`) calls `re.Shutdown(qfalse,
  qfalse)` - "don't destroy window or context" - on *every* map load,
  expecting a GL-style soft restart where the window/context survive and a
  later `RE_BeginRegistration` cheaply rebuilds internal state. This
  renderer has no equivalent of "recreate everything except the window" (and
  `RE_BeginRegistration` doesn't attempt one), so tearing down the device
  here left it null with nothing to recreate it, crashing the next texture
  registration. Fixed by making `VK_Shutdown` a no-op unless `destroyWindow`
  is true (i.e. an actual full shutdown, not a soft restart).

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
- Static world/BSP geometry (`tr_world.cpp`: `RE_LoadWorldMap`,
  `RE_RenderScene`, `RE_ClearScene`) - see "3D world geometry" above for what
  was actually verified. Concretely: a `.bsp`'s `LUMP_SHADERS`/
  `LUMP_DRAWVERTS`/`LUMP_DRAWINDEXES`/`LUMP_SURFACES`/`LUMP_LIGHTMAPS` lumps
  (shared, GL-agnostic structs from `qcommon/qfiles.h`) are parsed; only
  `MST_PLANAR`/`MST_TRIANGLE_SOUP` surfaces are kept (patches/curves and
  flares are skipped, not tessellated or drawn); `SURF_NODRAW`/`SURF_SKY`
  surfaces are skipped; each surface's diffuse texture is resolved through
  the same first-stage-only `.shader` lookup the 2D path uses, multiplied by
  its baked lightmap (or a white 1x1 fallback for surfaces with none); a
  surface whose diffuse texture fails to resolve is skipped rather than
  drawn wrong (see the `dark_dust` bug below); surfaces entirely outside the
  view frustum are culled (per-surface AABB vs. the frame's frustum planes,
  see "3D world geometry" above) but there's still no BSP visibility
  culling or back-face culling (see the "no culling" comment on
  `VK_CreateWorldPipeline` in `tr_init.cpp` for why not even back-face
  culling is safe to turn on yet) and every surface uses one opaque
  pipeline (not even the `.shader`-driven blend-mode selection the 2D path
  has); drawn with a real per-frame camera built from `refdef_t` (see
  `VK_BuildViewMatrix`/`VK_BuildProjectionMatrix` in `tr_world.cpp`).
- Skybox rendering (`tr_world.cpp`: `VK_LoadSky`) - see "3D world geometry"
  above for what was verified. A flat (non-subdivided, non-warped) 6-face
  box using the sky shader's own name as its basename, always camera-
  centered; drawn depth-test/write-disabled before world geometry.

## What's not implemented yet (safe no-ops, won't crash, won't draw)

- Ghoul2 (character/weapon model) rendering entirely, including the CPU-side
  bone/skeleton math - see "Ghoul2 is not reused from rd-vanilla" below for
  why. Every `G2API_*` entry point is a safe stub; no character or weapon
  models will animate, attach, or render. (Registration now reports success
  - see the "3D world geometry" bugs-found list above - so game logic
  proceeds normally; it's specifically the rendering that's still a no-op.)
- Dynamic lighting for world geometry (`AddLightToScene` is a stub) and
  vertex lighting/colors - only the map's precomputed, baked lightmap
  applies (see "3D world geometry" above). No shadows other than what's
  already baked into that lightmap; nothing moves, casts, or receives a
  dynamic shadow.
- BSP visibility (PVS) culling and back-face culling for world geometry -
  view-frustum culling *is* implemented (see "3D world geometry" above),
  but every surface potentially in view is still submitted regardless of
  whether the level's BVH/PVS data would say it's actually occluded by
  other geometry, and both triangle winding directions still draw.
- Curved surfaces/patches (`MST_PATCH`) and flares (`MST_FLARE`) - skipped
  entirely at load time, not just unlit.
- Proper sky rendering: what's implemented (see "3D world geometry" above)
  is a flat, non-subdivided skybox using the sky shader's *name* as its
  basename - no `.shader` `skyparms` parsing (so a level whose sky script
  points at a different-named basename won't find its faces), no dome
  warping/subdivision (visible seams at box edges), no `RDF_SKYBOXPORTAL`
  (a portal showing a miniature separate scene - a distinct, unimplemented
  feature from the base skybox).
- Dynamic scene content: entities (`AddRefEntityToScene`), runtime polys
  (`AddPolyToScene`) - both still stubs, so nothing except the static world
  itself ever appears in a 3D scene yet.
- Full `.shader` script parsing: only a defined shader's first stage's
  `map`/`blendFunc` is read (see "What's actually implemented" above) -
  later stages, `tcMod` animation, `rgbGen`/`alphaGen` waves, sky, and fog
  are all ignored. World geometry doesn't even get the 2D path's blend-mode
  selection yet (see above) - everything is one opaque pipeline.
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

One easy-to-repeat mistake in that implementation, `CVulkanGhoul2InfoArray`
(`tr_init.cpp`): its `New()` must never return handle `0` - `CGhoul2Info_v`
(`game/ghoul2_shared.h`) uses `0` as its own "not allocated" sentinel, so a
genuinely-valid handle of `0` is indistinguishable from "empty" and trips
`assert(mItem)` in `CGhoul2Info_v::operator[]`. The constructor burns index 0
at startup (permanently marked invalid) so real allocations start at 1 - see
its comment.

Also worth knowing: `G2API_InitGhoul2Model`/`RE_RegisterModel`/
`RE_RegisterSkin` report *success* (with fake/empty data), not failure, even
though nothing they "register" actually renders. That's deliberate, not an
oversight - see "3D world geometry" above for the game-side fatal-error
chain (`Com_Error(ERR_DROP, ...)`) that a failure return triggers, which
otherwise aborts map loading before `RE_RenderScene` ever runs.

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

`tr_world.cpp`'s BSP loader is the one place this has actually happened, and
it's a useful data point for how much of the above holds up: the raw lump
*structs* (`dheader_t`/`dshader_t`/`drawVert_t`/`dsurface_t`,
`qcommon/qfiles.h`) needed zero adaptation - they were never the problem,
being plain data with no rd-vanilla-specific types or GL calls anywhere near
them. What got rewritten instead was the *parsing and submission logic*
around them (`R_LoadSurfaces` et al. in rd-vanilla's `tr_bsp.cpp`), because
that code is entangled with rd-vanilla's `world_t`/`shader_t`/GL upload calls
throughout - confirming the split this section describes, not an exception
to it.

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
