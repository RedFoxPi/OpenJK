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
- **Bug found and fixed: the sky was rendering vertically flipped.** The
  per-vertex UV formula was `uv = ((s+1)*0.5, (t+1)*0.5)`, which *looks*
  like a faithful copy of rd-vanilla's real `MakeSkyVec` (`tr_sky.cpp`) -
  and the `s`/position math is - but `MakeSkyVec` has one more line after
  that: `t = 1.0 - t;`, a real, separate V-flip on top of the remap, not
  just a naming coincidence. Dropping it flipped every face vertically:
  the ground-level tree-line band each face's texture has near its bottom
  edge rendered near the *top* of the visible sky instead. Caught by
  comparing an academy1 screenshot against a vanilla reference (the
  flipped tree-line was visible as foliage appearing at the top of a
  window opening with blown-out brightness filling the rest, rather than
  trees near the sill with sky above) - the exact same reference screenshot
  used for the Ghoul2 skin-handle bug below, checked more closely once that
  fix landed. Fixed by adding the missing `1.0f -` back onto `v.uv[1]`.
- **Bug found and fixed: a sky shader with no textured farbox rendered as a
  pure black void instead of a sky.** Flagged by comparing a `hoth2`
  screenshot against vanilla: ours showed a stark white terrain wedge
  against solid black, unlike anything in the vanilla reference (a soft,
  foggy snowscape). Root-caused, not just patched over: `VK_LoadSky`
  assumes every sky shader has 6 real `_rt`/`_lf`/`_bk`/`_ft`/`_up`/`_dn`
  face images to load and gives up entirely (no sky drawn at all, leaving
  the Vulkan clear colour - black) the moment one is missing. hoth2's sky
  shader (`textures/skies/hoth`, confirmed by parsing `LUMP_SHADERS`
  directly) turned out to be `skyParms - 512 -` (`shaders/skies.shader`) -
  a literal dash for the farbox parameter, valid Quake3 syntax for a
  fog/portal-only sky with **no textured cubemap at all**. Confirmed no
  `hoth*_rt`/etc face files exist anywhere in any `assets*.pk3`, so this
  was never a naming-mismatch bug (the kind the missing `skyparms` parsing
  noted below would explain) - the faces genuinely don't exist. Since this
  renderer doesn't parse `skyParms` at all, it can't recover the shader's
  real fog colour, but falling back to *no sky whatsoever* was clearly
  worse than an approximation: fixed by falling back to a flat neutral
  grey box (`VK_CreateSolidImage`) whenever any face fails to resolve,
  rather than aborting sky rendering entirely. One more bug caught in the
  process: the first attempt at this used an on-screen grey of (140,150,
  160), which rendered as solid white - sky faces are drawn through the
  same `world.frag` as everything else, paired with the full-white
  `s_whiteLightmap`, so the `*2.0` "overbright bits" approximation
  (see above) doubles the colour before it hits the screen; halving the
  raw texture value (70,75,80) fixed it. **Verified**: re-ran `hoth2`
  with per-batch debug logging (shader name + AABB for every surface that
  passed frustum culling) before touching the sky code at all, confirming
  every drawn batch was `textures/hoth/snow_01` with small, sane,
  correctly-positioned extents - real terrain, not a stray/degenerate
  polygon, ruling out a geometry bug before concluding this was a sky
  issue. After the fix, the same camera position now shows the grey
  fallback sky and, distinguishable against it for the first time, a
  genuinely shadowed terrain mound that previously blended invisibly into
  the black void. Re-tested `academy1` (working textured sky) and `vjun1`
  (same missing-farbox situation, different map) to confirm no regression:
  academy1 still loads its real 6-face sky with no fallback-colour log
  suffix and an unchanged screenshot; vjun1 falls back cleanly with no
  crash. The camera's low/close framing on `hoth2` itself - separate from
  this bug - is discussed in "Curved surfaces" above.
- **World fog is now rendered.** Even with the sky fallback and camera
  timing above understood, a `hoth2` screenshot still looked nothing like a
  real game scene next to vanilla's soft, hazy snowscape - flagged again by
  direct comparison, not eyeballed away a second time. Root cause: this
  renderer had **no fog support at all** - `RE_SetRangedFog` and
  `R_SetTempGlobalFogColor` were (and still are, see below) complete
  no-ops, and `world.frag` had no fog term. hoth2's BSP defines exactly one
  `LUMP_FOGS` entry, `textures/fogs/Hoth2fog` with `brushNum == -1`
  (confirmed by parsing the lump directly) - rd-vanilla's own
  `R_LoadFogs`/`tr_bsp.cpp` treats `brushNum == -1` as a **global** fog
  covering the entire map, not bounded to one brush (verified by reading
  that function, not assumed), and the shader's `fogparms` line
  (`shaders/fogs.shader`) gives its real values: colour `(0.7 0.7 0.7)`,
  opaque at `1800` units - real data, not guessed constants.
  Implemented: `tr_shader.cpp`'s existing minimal `.shader` scanner (until
  now only used for `blendFunc`) also records each shader's top-level
  `fogparms` line into a second lookup table
  (`VK_GetShaderFogParms`); `tr_world.cpp`'s new `VK_LoadWorldFog` reads
  `LUMP_FOGS`, keeps only a `brushNum == -1` global entry (a real per-brush
  local fog volume - hoth2 has none, but vjun1's `textures/fogs/fog_black`,
  `brushNum 6685`, does - is correctly left unimplemented, not attempted),
  and looks up its colour/distance via that table. `world.vert`/`world.frag`
  gained a `camPos`/`fogColor` push constant (shared vertex+fragment stage
  now, not vertex-only) and a `mix()` toward the fog colour based on
  `distance(worldPos, camPos)`, saturating at the shader's declared opaque
  distance. This is a **linear** distance ramp, not rd-vanilla's real fog
  (`tr_shade_calc.cpp`'s `RB_CalcFogTexCoords` - a dot-product "depth along
  the fog plane's normal" measure fed through a precomputed gradient
  texture, i.e. a different falloff curve along a different axis) - same
  "simplify the algorithm, keep the visual intent" tradeoff as the flat
  skybox box and fixed-subdivision patches elsewhere in this file. Only
  world geometry is fogged, not the sky (deliberately - see the push
  constant comments) or Ghoul2 models (not attempted this pass).
  **Verified**: hoth2 went from a stark, geometrically-flat black/white
  cutout to a soft grey-white scene whose colour palette closely matches
  the vanilla reference (both dominated by soft blue-grey/white tones with
  a hazy horizon, instead of solid black); confirmed the `(0.7 0.7 0.7)`/
  `1800` values load and log correctly, matching the shader text exactly.
  Regression-checked three ways: academy1 (zero `LUMP_FOGS` entries,
  confirmed by parsing) shows no "loaded global fog" log line and an
  unchanged screenshot; vjun1 (one local + one global fog) correctly picks
  only the global one (`textures/fogs/vjun1`, not `fog_black`) and loads/
  runs/screenshots with no crash; a full clean rebuild is warning-free.
  `RE_SetRangedFog`/`R_SetTempGlobalFogColor` (the *dynamic* fog API, used
  for scripted fog changes mid-level, e.g. weather-effect entities in
  `g_fx.cpp`) are still no-ops - only the BSP's static, load-time global
  fog is implemented here.
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
- **Curved surfaces (`MST_PATCH`) are now tessellated** (`VK_TessellatePatchQuad`)
  instead of being skipped entirely. academy1 (this file's other verification
  scenes) turns out to have **zero** `MST_PATCH` surfaces - confirmed by
  parsing its `LUMP_SURFACES` directly - so it was never actually exercising
  this gap; `hoth2`/`yavin1`/`vjun1` (the render-regression harness's other
  scenes) have 101/58/146 respectively, all curved terrain/snowdrifts/pipes
  that previously just weren't there. This is a fixed-subdivision
  simplification of rd-vanilla's real, adaptive `R_SubdividePatchToGrid`
  (`tr_curve.cpp`), which only subdivides as much as a patch's actual
  curvature needs (checked against the `r_subdivisions` cvar's error
  tolerance) - every patch here always tessellates to the same fixed
  8x8-quad resolution per 3x3 control-point sub-patch, regardless of size or
  flatness. That's not a shortcut on the *math*, though: quadratic Bezier
  de Casteljau subdivision (rd-vanilla's recursive midpoint-bisection
  approach) and the closed-form biquadratic Bernstein basis evaluation this
  uses instead both trace the exact same curve family - sampled at fixed
  rather than adaptive density, not a different or approximate shape. Same
  "simplify the algorithm, keep the math faithful" tradeoff as the flat
  (non-subdivided) skybox box elsewhere in this file.
  **Verified**: no crash on `hoth2` or `vjun1` across several `wait_frames`
  values (`yavin1` still hits the same pre-existing, unrelated ICARUS
  `Assertion 0 failed` crash on a direct `devmap` - see "A capture-harness
  bug that invalidated a run of 'verified' claims" further below, where
  this same crash was independently rediscovered and actually root-caused
  much later; not caused by this change, reproduced identically with patches
  reverted); `vjun1`'s frustum-culled/visible batch split with patches
  enabled (11576/12556, ~92%) closely matches the same map with patches
  skipped (11462/12436, ~92%) across `wait_frames` 30/90/200, so the new
  per-sub-patch AABBs aren't corrupting culling. `academy1`/`sp_menu` are
  unaffected (identical batch/vertex/index counts and screenshot to before,
  as expected for a map with no patches to begin with).
  `hoth2`'s default `devmap` camera sits low and close to the ground rather
  than at an elevated establishing shot like vanilla's at the same
  `wait_frames`, which combined with the sky bug below to look badly broken
  at first - **this was checked properly** (see below), not just eyeballed
  and dismissed: temporary per-batch debug logging (shader name + world-
  space AABB for every surface that actually passed frustum culling that
  frame) confirmed every drawn batch was `textures/hoth/snow_01` with
  small, sane, correctly-positioned extents - real terrain, not a stray or
  degenerate polygon.
  **The camera framing itself turned out not to be a bug at all** - traced
  to ground truth, not guessed at. hoth2's intro (`scripts/hoth2/intro1.ibi`,
  triggered by a `target_scriptrunner` at spawn) cuts between four
  `ref_tag` entities (`newcam01`-`04`) placed in the map, smoothly
  interpolating position/angles between them over real time (ICARUS
  `CAMERA MOVE`/`PAN` with a multi-second `duration`, shared client/cgame
  code - the renderer doesn't touch this at all). Temporary logging added
  to `code/game/Q3_Interface.cpp` (`CQuake3GameInterface::Set`/`CameraMove`/
  `CameraPan`, reverted after use - not part of this repo's history) traced
  the actual ICARUS command stream by wall-clock game time: at `wait 300`
  vanilla had reached `t=5950ms` game time and started an NPC animation,
  while this renderer had only reached `t=2100ms` - still sitting at the
  very first camera cut (`newcam01`), its position/angles matching that
  `ref_tag`'s `origin`/`angles` **exactly**, confirming the camera code
  itself is correct and the harness's fixed `wait 300` had simply captured
  two renderers at different points in the *same* real-time cutscene.
  Confirmed conclusively by giving this renderer a much larger frame budget
  (`wait 3000`): the same trace then reached `newcam02` - again an exact
  origin/angles match - mid-flight through its 2000ms interpolation.
  This renderer is simply far slower per frame than `rd-vanilla` under this
  headless software rasterizer (lavapipe vs. llvmpipe), so the same
  `wait_frames` budget lets much less real/game time elapse - the same
  class of test-harness timing artifact already documented for academy1's
  cutscene elsewhere in this file, just far more pronounced here. Not a
  rendering, camera, or ICARUS bug, and no code change was needed.

### Ghoul2 rendering (tr_model.cpp)

Character/weapon model (`.glm`) rendering. Real skeletal animation now
exists (see "Skeletal animation" below) - meshes are skinned per bone,
not held in a fixed bind pose - but only up to a real, specific limit:
every model instance always plays **frame 0 of its `.gla`'s animation
data**, not whatever animation the game actually asked for. `G2API_SetBoneAnim`
and friends are still stubs (see "What's not implemented yet"), so there is
not yet any live, time-driven, game-selected animation - just a single,
correctly-posed *static* frame instead of the old single *bind-pose* frame.
GLM parsing is a fresh implementation (see "Ghoul2 is not reused from
rd-vanilla" below for why), but the offset/pointer arithmetic is copied
field-for-field from rd-vanilla's real `R_LoadMDXM` (`rd-vanilla/tr_ghoul2.cpp`)
rather than rederived from the struct comments alone. Loaded models reuse
`tr_world.cpp`'s vertex format, pipeline, and descriptor-set-building helper
wholesale - a Ghoul2 surface is, for drawing purposes, just another indexed
triangle batch paired with `vk.whiteImage` as its "lightmap" (Ghoul2 meshes
have no baked lightmap of their own).

Run against `academy1`, headlessly, same as the world-geometry checks above:

- The mesh parser was verified against real game data two ways: (1)
  Python-parsing `models/players/kyle/model.glm`'s raw bytes independently
  confirmed all 82 surfaces have an **empty** embedded shader name (see the
  skin bullet below for why that matters) with plausible names/flags/child
  counts, matching what the C++ parser reads; (2) non-humanoid models
  (weapons, the `protocol` droid NPC) whose `.glm` *does* embed real shader
  names loaded with correct, non-garbage surface/vertex/triangle counts
  (e.g. `models/weapons2/saber/saber_w.glm`: 2 surfaces, 583 verts, 1602
  indexes; `models/players/protocol/model.glm`: 32 surfaces, 3150 verts,
  10455 indexes) and rendered without corruption.
- Entity dispatch (`RE_AddRefEntityToScene`/`RE_ClearScene`/
  `VK_DrawGhoul2Entities`) is real, not a stub: a per-frame queue of
  `refEntity_t` is drawn each `RE_RenderScene` using the entity's own
  `origin`/`axis` to build a model matrix (same column-major construction
  as rd-vanilla's `R_RotateForEntity`, composed with the frame's `mvp` via
  the same `VK_MultiplyMatrix` convention the camera/sky matrices use).
- **Skin (`.skin` file) support was required and added**, not originally
  planned: humanoid player/NPC `.glm` files ship every surface's embedded
  shader name **empty** - their actual per-surface textures come entirely
  from an external `.skin` file (`surfacename,shaderpath` per line) applied
  at runtime, confirmed against `models/players/kyle/model_default.skin`'s
  real contents. Without parsing it, every humanoid model resolved zero
  images and skipped every surface. `VK_RegisterSkin` (`tr_model.cpp`)
  parses the common single-file case (comma-separated lines, `tag_` lines
  skipped) and the three-part `head|torso|lower` composite macro syntax
  (`VK_SplitCompositeSkinName`/`VK_ParseSkinFile`, an exact port of
  rd-vanilla's real `RE_SplitSkins`/`RE_RegisterIndividualSkin`, including
  the `"_off"`-suffixed-surface-name convention). **This was originally left
  unimplemented, on the assumption it was a rare/cosmetic feature - it
  isn't**: academy1's generic filler-student NPCs (`jedi_tf`/`jedi_hm`/
  `jedi_hf`/`jedi_kdm`/`jedi_zf`/`jedi_rm`, randomized head+torso+lower-body
  combinations) all use this syntax, and with it unimplemented,
  `VK_RegisterSkin` tried to `ri.FS_ReadFile` the literal macro string as a
  filename, which of course never exists - every one of those NPCs resolved
  *zero* drawable surfaces (`VK_LoadGhoul2Model`'s "has no drawable
  surfaces" case) and was entirely invisible, not merely mis-posed. Found
  while chasing a user's report that character poses still looked "far from
  exact resemblance" to vanilla even after the animation.cfg fix above -
  academy1's `ghoul2: 9/18 scene entities drew` log line was the tell (half
  the NPCs weren't drawing *anything*); fixing this brought it to `18/18`
  and the crowd/formation now visibly matches vanilla's same cutscene
  moment, not just the handful of already-single-skinned named characters
  (Kyle, Rosh, etc.) that happened to work before. `RE_RegisterSkin` now
  calls it for real (previously a stub returning a fake handle `1`). The
  model cache key is
  `(fileName, skinHandle)`, not just `fileName`, since the same `.glm`
  loaded with two different skins needs two different sets of baked
  per-surface textures/descriptor sets.
- **Bug found and fixed**: a single entity's Ghoul2 vector can (and
  routinely does) hold several sub-models at once - body + weapon + saber
  blade, each loaded via its own `G2API_InitGhoul2Model` call on the *same*
  `CGhoul2Info_v`. The original stub (`ghoul2.resize(1)` on every call)
  unconditionally kept only one slot, so a second call silently discarded
  the first-loaded model instead of appending - harmless while rendering
  was still stubbed, but would have meant "only ever draws the
  last-loaded sub-model" once it wasn't. Fixed to match rd-vanilla's real
  `G2API_InitGhoul2Model` (`G2_API.cpp`): find a free slot
  (`mModelindex == -1`, `CGhoul2Info`'s default) or append; the returned
  slot index is what game code indexes back into. `VK_DrawGhoul2Entities`
  iterates every valid slot per entity, not just index `0`.
- **Bug found and fixed - the one that actually made characters invisible**:
  the very first working build of this feature loaded correctly (real
  entities, real per-model geometry, real per-frame draw calls, no crash)
  but rendered **no visible character** anywhere - caught by comparing
  against a vanilla reference screenshot of academy1's opening, which shows
  an NPC standing centered and close enough to fill much of the frame; this
  renderer's screenshot from the identical camera showed empty pillars and
  floor, nothing else. The cause: `G2API_InitGhoul2Model`'s `customSkin`
  parameter is **not** a `VK_RegisterSkin`/`RE_RegisterSkin` handle, despite
  the name suggesting it lines up with `RE_RegisterSkin`'s return value -
  real callers (`g_client.cpp`'s `G_SetG2PlayerModel`) pass
  `G_SkinIndex(skinName)` instead, a small networked *configstring* index
  (position in `CS_CHARSKINS`, renumbered across the whole game session),
  a completely different, unrelated numbering scheme from this renderer's
  own skin cache. Treating it as a `VK_RegisterSkin` handle (this feature's
  first attempt) silently applied a random *other* model's skin whenever
  the two numbering schemes happened to collide - concretely, academy1's
  `rosh_penin` NPC picked up several of the `protocol` droid's textures,
  and most of its surfaces failed to resolve *any* image and were skipped
  (confirmed by a temporary per-surface trace: `rosh_penin`'s `.glm` has 14
  real drawable body surfaces, all with matching entries in its own
  `.skin` file by name, but only 4 surfaces actually resolved - one to a
  `models/players/protocol/...` texture path that has no business being on
  a `rosh_penin` instance at all). The real renderer skin handle only shows
  up later, as `G2API_SetSkin`'s *`renderSkin`* (third) parameter -
  `G_SetG2PlayerModel` gets it from `gi.RE_RegisterSkin(skinName)` directly
  and always calls `G2API_SetSkin` immediately after
  `G2API_InitGhoul2Model`. Fixed by having `G2API_InitGhoul2Model` ignore
  its `customSkin` parameter entirely (load with no skin, same as a model
  with no `G2API_SetSkin` call ever made) and implementing `G2API_SetSkin`
  for real - it re-resolves `ghlInfo->mModel` via
  `VK_LoadGhoul2Model(ghlInfo->mFileName, renderSkin)` using its own
  `renderSkin` parameter, the one actually in this renderer's handle space.
  After the fix, `rosh_penin` resolves 14/14 real body surfaces (not 4),
  and the academy1 screenshot from that same camera **now shows the NPC**,
  matching the vanilla reference's framing, pose, and dark-robe/belt-buckle
  look closely - the first checkpoint in this session verified with an
  actual positive screenshot rather than log evidence alone. Other
  academy1 NPCs' resolved-surface counts jumped similarly post-fix (e.g.
  `kyle` 5 -> 19, `jedi` -> 17, `protocol` 32 -> two distinct skin variants
  at 32 and 26).
- **Bind-pose bolt support added (`G2API_AddBolt`/`GetBoltMatrix`,
  `VK_LoadGhoul2Skeleton` in `tr_model.cpp`) after a real, confirmed bug**:
  comparing an academy1 screenshot against vanilla showed the camera
  planted essentially *inside* an NPC (extreme close-up on a belt buckle
  and gloved hand) at a point where vanilla shows a normal few-feet-away
  portrait shot. Root cause, traced through `code/cgame/cg_camera.cpp`'s
  `CGCam_FollowUpdate`: academy1's intro camera tracks a bolt (a named
  bone attachment point) on the NPC via `G2API_AddBolt`/`GetBoltMatrix` to
  frame the shot, then places the camera a fixed distance from that bolt.
  This renderer's `G2API_GetBoltMatrix` was a stub that always returned a
  *zeroed* matrix and `qfalse` - and `CGCam_FollowUpdate` never checks that
  return value, so the "tracked point" silently collapsed to the NPC's own
  origin, and the camera ended up parked on top of it. Fixed by parsing
  the `.gla` skeleton file for real (`mdxaHeader_t`/`mdxaSkel_t`, bone
  names + `BasePoseMat` - offset arithmetic matched field-for-field against
  rd-vanilla's real `R_LoadMDXA`/`G2_Add_Bolt`, same discipline as the
  `.glm` mesh parser) and implementing real bone-name-to-bolt resolution
  and bind-pose bolt-matrix composition (matched against rd-vanilla's real
  `Multiply_3x4Matrix`/`Create_Matrix`/`G2API_GetBoltMatrix`, a *different*
  row-major 3x4 affine convention from the column-major `mat4` used
  elsewhere in this renderer, so it's a fresh small matrix multiply, not a
  reuse of `VK_MultiplyMatrix`). "Bind-pose" here matters: there's still no
  skeletal animation, so a bolt on a bone that would move during an
  animation reports that bone's *rest* position, not wherever the
  animation would actually put it - correct for a standing-still NPC,
  wrong once real animation exists.
- **Verified the fix is doing real work, but not that it fully resolves
  academy1's camera framing** - academy1's intro turned out to be a
  multi-shot cinematic with several distinct camera setups (confirmed by
  capturing `rd-vanilla` itself, not just this renderer, at matching
  `wait_frames` values: 150 and 300 show the same close-up portrait, 600
  shows a completely different wide shot of a room full of seated/standing
  NPCs, 1000 shows yet another close-up from a different angle - this is
  cutscene *cuts*, not a single continuous pan). Bone-name resolution now
  works correctly (confirmed via a temporary trace: real bone names like
  `pelvis`/`cervical`/`lower_lumbar` resolve to sane indices; the
  `*`-prefixed surface-attachment names some models also request correctly
  return "not found", since surface bolts - as opposed to bone bolts -
  aren't implemented) and bolt matrices compose to plausible non-degenerate
  positions (a believable few dozen units above the entity origin, not
  zero). Despite that, this renderer's own camera still doesn't match
  vanilla's framing at the shots tested - it stays close to a subject
  throughout rather than settling into vanilla's portrait/wide-shot
  sequence. Since `+wait N` waits for N *rendered client frames*, not N
  fixed server ticks, and academy1's cutscene timeline advances in real
  (wall-clock) time via ICARUS commands, two renderers with different
  per-frame cost reach different points along that timeline - and possibly
  different points in whatever dialogue/sound-gated waits the script uses,
  which this test harness runs with sound disabled - by the time each has
  rendered its Nth frame. That's a real confound this hasn't been
  untangled from, on top of whatever's left of the original bug (if
  anything is). Bottom line: the underlying bolt-matrix bug that was
  found and root-caused is genuinely fixed, but full camera-framing parity
  for this specific cutscene is **not** confirmed and would need more
  investigation into `cg_camera.cpp`'s broader subject/distance logic (or
  running with sound enabled) to pin down further.

### Skeletal animation (tr_model.cpp)

Real per-bone mesh skinning, now **live and time-driven** - the biggest
remaining gap this file used to document ("no skeletal animation, no bone
math at all"). `G2API_SetBoneAnim`/`GetBoneAnim`/`GetAnimRange`/
`PauseBoneAnim`/`IsPaused`/`StopBoneAnim` (and their `...Index` siblings)
are real now, not stubs: each Ghoul2 model instance tracks its own
start/end frame, playback speed, and start time, and is skinned to its
actual current frame - computed from real elapsed game time - every frame
it's drawn. This replaced the previous checkpoint's fixed single frame
(always frame 0, regardless of what the game asked for). See "Live
animation" below for the implementation and its verification, and the
frame-0 caveat bullet further down for what the *previous* checkpoint's
static screenshots did and didn't prove (still true of any instance that
genuinely never gets a `SetBoneAnim` call - frame 0 remains the fallback).

The math (bone-hierarchy composition, frame decompression, the skinning
formula itself) is copied verbatim from rd-vanilla's real, working
implementation - `Multiply_3x4Matrix`/`G2_GetBonePoolIndex`/`UnCompressBone`/
`G2_RagGetAnimMatrix`/`R_AddGHOULSurfaces` (`rd-vanilla/tr_ghoul2.cpp`) -
not rederived from the file format comments, for the same reason this
renderer's other binary-format/matrix-math ports always copy real arithmetic
rather than reinvent it (see the patch-tessellation and sky-corner-formula
entries above): a skeletal animation pipeline has a lot of small places to
get a sign, a row/column, or an indirection backwards and have it *look*
plausible while being wrong.

- `VK_LoadGhoul2Skeleton` (`.gla` loading) now keeps the whole file resident
  instead of reading out just bone names/`BasePoseMat` and freeing it - real
  animation needs on-demand access to `mdxaHeader_t::ofsFrames`/
  `ofsCompBonePool` at arbitrary later times, not just at load time.
- `VK_ComputeGhoul2Pose` computes every bone's object-space matrix for one
  frame: `MC_UnCompressQuat` (`qcommon/matcomp.cpp` - already linked into
  this renderer, unused until now) decompresses each bone's frame-local
  delta from the compressed bone pool, and a recursive walk composes each
  bone with its already-composed parent (`child = parent ∘ delta`, matching
  rd-vanilla's exact `Multiply_3x4Matrix` argument order).
- **A real surprise, checked against rd-vanilla's actual code rather than
  assumed**: this composition deliberately never touches `BasePoseMat`/
  `BasePoseMatInv` anywhere. That looks wrong at first for something
  producing an *object-space* pose - but rd-vanilla's real mesh-skinning
  code (`R_AddGHOULSurfaces`) applies its bone-cache result directly to
  `mdxmVertex_t::vertCoords` with no bind-pose-inverse step at all;
  `BasePoseMat`/`BasePoseMatInv` are only used elsewhere, for bolt queries
  against a fixed reference pose and an optional cvar-gated "unsquash"
  renormalization pass this renderer doesn't implement. Confirmed empirically
  too, not just by reading: temporary debug logging of frame 0's composed
  matrices showed rotation parts very close to identity (trace ≈ 3.0) but
  *non-zero* translations that don't match `BasePoseMat`'s own translations -
  consistent with frame 0 being an ordinary animation frame (the first frame
  of whatever clip happens to be first in the file, not a T-pose/rest
  reference), which is exactly what the "no BasePoseMatInv" reading predicts
  and a "frame 0 = bind pose" reading would not have explained.
- `VK_SkinGhoul2Model` applies that pose to the mesh: linear blend skinning,
  up to 4 weighted bones per vertex (`G2_GetVertWeights`/`GetVertBoneIndex`/
  `GetVertBoneWeight`, `rd-common/mdx_format.h`'s existing packed-weight
  helpers, unused until now), formula copied verbatim from
  `R_AddGHOULSurfaces`. One indirection worth calling out because it's easy
  to get wrong silently: `G2_GetVertBoneIndex` returns an index into that
  *surface's own* `ofsBoneReferences` table, not a global skeleton bone
  index directly (confirmed against rd-vanilla's real
  `piBoneReferences[G2_GetVertBoneIndex(v,k)]` usage) - remapped to a real
  global bone index once at load time (`VK_LoadGhoul2Model`), not redone on
  every skin.
- The vertex buffer itself changed from device-local/upload-once (fine for
  a fixed bind pose) to host-visible/coherent and persistently mapped, since
  `VK_SkinGhoul2Model` now rewrites its contents from the CPU whenever a
  model instance's pose changes - CPU skinning, not GPU vertex-shader
  skinning, the smaller/simpler of the two architectures and consistent
  with this renderer's existing "CPU-generate geometry, upload a flat
  buffer" pattern elsewhere (patch tessellation, the skybox box). A real
  performance cost worth flagging honestly: every visible animated entity
  gets fully re-skinned and re-uploaded, currently once per model-cache
  entry per draw (frame 0 never changes, so `lastSkinnedFrame` caching
  already avoids repeat work *this* pass) - once real per-entity animation
  state exists, different entities sharing one cached model
  (`s_ghoul2ModelsByKey`, keyed by file+skin, not by entity) will need
  independent poses and therefore independent buffers, a real architecture
  change flagged in `tr_model.cpp`'s comments for that follow-up to address,
  not solved here.
- **Bug found and fixed, unrelated to animation itself**: `VK_DrawGhoul2Entities`
  was still pushing only `mvp` (64 bytes, `VK_SHADER_STAGE_VERTEX_BIT` alone)
  through `vk.worldPipelineLayout`, which the fog work in a previous session
  had already widened to a 96-byte `mvp`+`camPos`+`fogColor` range requiring
  both vertex and fragment stages. Per the Vulkan spec, a `vkCmdPushConstants`
  call must match the *union* of stage flags for every push-constant range
  overlapping the bytes it touches - this was pushing a stage-flag subset of
  what the layout at that byte range actually requires, a real spec
  violation (silently tolerated by lavapipe in practice, since it never
  crashed or visibly misrendered, which is exactly why it went unnoticed
  until reading this code closely for an unrelated reason). Fixed by pushing
  the full struct with both stages, matching `RE_RenderScene`'s own world/sky
  pushes exactly; `fogColor.a` stays 0 (fog disabled) since Ghoul2 models
  aren't fogged this pass.
- **Verified**: no crash across `academy1`/`vjun1`/`hoth2` (a clean rebuild,
  zero warnings, all three). Visually: `academy1`'s Kyle-model close-up shows
  a fully intact, correctly proportioned head/neck/torso/belt at frame 0 -
  not exploded, stretched, or missing geometry, the failure mode a wrong
  weight/index/matrix-order bug would produce. More convincingly,
  `vjun1`'s character (a different model, different `.gla`) shows a visibly
  *articulated* pose - one arm raised and extended away from the body, not
  just a rigid whole-model translation - real evidence that per-bone
  rotation is propagating correctly through the hierarchy, not just
  translation. `academy1`/`hoth2` screenshots otherwise match the prior
  (bind-pose-only) session's framing and composition, as expected since
  nothing about camera/world/sky/fog changed here.
- **Important caveat, found by direct question rather than assumed away**:
  "frame 0" is not a meaningful rest/idle *body* pose and the academy1
  screenshot's specific arm position/facing should **not** be read as
  something the real cutscene ever actually shows. Checked directly, not
  guessed: the Kyle model in that screenshot uses
  `models/players/_humanoid/_humanoid.gla` (confirmed via its own load
  log), and that skeleton's `animation.cfg` (`enum, targetFrame,
  frameCount, ...`) lists frame 0 as belonging to `FACE_ALERT` - a 2-frame
  **facial-expression-only** clip (there's a whole separate `FACE_*` block
  of tiny clips before the real `BOTH_*`/body clips start). The real game
  drives face, torso, and legs as up to three independently-selected,
  independently-blended animations at once (ICARUS `SET_ANIM_HOLDTIME_BOTH/
  UPPER/LOWER`, `TID_ANIM_*` in `code/game`); this renderer's frame-0
  shortcut instead reads *every* bone's compressed data at the same single
  global frame index, so the body bones shown are whatever incidental data
  happens to sit in that slot - not a deliberate rest pose, and not
  anything a real playthrough would display. The `vjun1` articulated-arm
  result above is still a legitimate confirmation of the skinning *math*
  (it correctly reproduces whatever pose the data at a given frame encodes)
  - just not evidence that frame 0 specifically looks like real gameplay.
  This is the concrete, visible face of the "not live yet" limitation
  already called out above, not a new bug - and a good illustration of why
  live animation state (picking the frame the game actually asked for) is
  the natural next step.

### Live animation (tr_model.cpp)

The follow-up to the above: `G2API_SetBoneAnim` and friends now really
work, so a model instance plays the animation the game actually asked for,
advancing over real time, instead of a permanently frozen frame 0.

**Caveat added much later, see "Character animation investigation: four
real bugs, and a wrong conclusion corrected" below**: everything in this
section describes the frame-advance machinery working correctly in
isolation, which it does - but for a long time afterward, none of it was
ever actually being exercised during a real playthrough, because a separate
bug (`RE_GetAnimationCFG` unconditionally reporting every `animation.cfg` as
missing) meant the game's own `NPC_SetAnim`/`PM_SetAnimFinal` call chain
that would invoke `G2API_SetBoneAnim` always silently no-opped before ever
reaching it. Every character kept showing the same non-gameplay pose the
whole time this checkpoint's own verification believed it had fixed that.

Scope, precisely - **one whole-skeleton animation track per model
instance**, not the real engine's independently-blended per-bone-subtree
tracks. rd-vanilla's real game code calls `G2API_SetBoneAnim` separately
per body region (`bg_panimate.cpp`: `"lower_lumbar"`/`"upper_lumbar"`/
`"thoracic"` etc. for legs/torso/arms, blended together at draw time via a
real bone-hierarchy walk that checks each bone for its own or an ancestor's
override); here, whichever call landed *last* for a given instance simply
wins for the *entire* skeleton, regardless of which bone name or index it
targeted. So legs and torso can't play independent animations (walking
while gesturing, say) - a visibly cruder result than the real engine's, but
a real, live, correctly time-driven single animation instead of a static
pose. Also not implemented: blending/crossfade between two animations
(`BONE_ANIM_BLEND`), sub-frame interpolation between two adjacent whole
frames (frames step discretely), and reverse/negative-speed playback -
three more real, visible-quality features of rd-vanilla's
`G2_TimingModel` (`tr_ghoul2.cpp`) this doesn't reproduce.

The frame-advance formula is copied from that same function, not
rederived, and cross-checked against its real caller: `G2_TimingModel`
computes `frame = startFrame + ((currentTime - startTime) / 50.0) *
animSpeed`, and `bg_panimate.cpp` computes the `animSpeed` it passes in as
`50.0f / curAnim.frameLerp` (`frameLerp` being the clip's real
milliseconds-per-frame from `animation.cfg`) - so the "50" in both places
cancels out to exactly "elapsed milliseconds ÷ the clip's real per-frame
duration," not an arbitrary constant. `BONE_ANIM_OVERRIDE_LOOP`'s real flag
value (`0x0010`, `game/ghoul2_shared.h`) is honored for looping playback;
any other flag bit is ignored.

**A real, separate bug found and fixed while wiring this up, not just a
missing feature**: caching a model's skinned vertex buffer per *cached
model* (keyed by file+skin, shared across every entity using that model -
see `s_ghoul2ModelsByKey`'s comment) was fine when every instance was
always frame 0, but is a genuine correctness bug once instances can be at
different frames - and multiple identical NPCs sharing one model is a
completely ordinary scene, not a rare edge case. The failure mode is subtle
because of how a Vulkan command buffer actually executes: recording several
`vkCmdDrawIndexed` calls against the *same* vertex buffer, with a CPU
`memcpy` re-skinning it in between each recording, does **not** give each
draw call "its skin as of when it was recorded" - the buffer only holds
whatever the *last* CPU write left there by the time the whole command
buffer is actually submitted and executed, well after all the recording
(and all the memcpys) already happened. Every instance of a shared model
would have silently rendered with whichever instance's pose was skinned
last, not its own. Fixed by giving each cached model's vertex buffer
`GHOUL2_SKIN_SLOTS_PER_MODEL` (8) independent slots and round-robin
assigning one slot per drawn sub-model instance each frame
(`VulkanGhoul2Model::nextSkinSlot`, reset once per `VK_DrawGhoul2Entities`
call) - past 8 simultaneous instances of the exact same model in one frame,
slots wrap and reuse an already-claimed one (a stale-for-one-frame pose,
not corruption or a crash), a deliberately accepted rare-scene limit rather
than unbounded per-frame allocation.

**Verified**: academy1, with `com_fixedtime 16` (see "Testing headlessly"
below) for a controlled, renderer-speed-independent comparison. At
`wait 100` (~1.6s of simulated time) the pose has visibly changed from the
old static frame 0, and the shot is a close character portrait in both
this renderer and `rd-vanilla` at the identical simulated time - a
reasonably close match this early, before much can have drifted. At
`wait 400` (~6.4s), academy1's camera has advanced to a wide balcony shot
showing **three simultaneous instances of the same officer NPC model**
side by side, each independently and correctly posed (one arm raised, not
three copies of whatever the last-skinned instance happened to be) -
direct, real-scene confirmation that the per-slot buffer fix above actually
works, not just a synthetic test. `rd-vanilla` at that same simulated time
shows a different shot (a face close-up) rather than the same wide balcony
view - an expected consequence of the scope cuts above, not a new bug:
`rd-vanilla`'s real animation system tracks legs/torso/face independently
and signals ICARUS's "wait for anim complete" the moment the *specific*
targeted bone's true, blended animation finishes, while this renderer's
single whole-skeleton, no-blend approximation reaches its own notion of
"done" at a different real time, which cascades into ICARUS reaching its
next scripted camera cut earlier or later than vanilla does - camera-cut
*parity* with vanilla was never a target of this checkpoint (chasing it
would mean reproducing rd-vanilla's real multi-track blended timing model
exactly, a much larger undertaking than live single-track animation
itself), only genuinely live, per-instance, mathematically real animation
playback, which this demonstrably now is. No crash across
academy1/vjun1/hoth2, clean rebuild, zero warnings.

**Correction, found much later (see "Character animation investigation:
four real bugs, and a wrong conclusion corrected" below): the specific
`wait 100`/`wait 400` observations above can't be trusted.** `com_fixedtime`
(used to control for
render-speed timing skew here) is not this cvar's real registered name -
`fixedtime` is - and it's not possible to determine after the fact whether
the console commands actually run for this checkpoint used the correct
name or not. What's still true regardless: the underlying *mechanism*
described (this renderer's single-track animation reaching its own "done"
signal at a different real time than vanilla's real per-region blended one,
which cascades into ICARUS camera-cut timing) is real and confirmed by
reading the actual G2API call pattern (see the per-bone-animation-state
fix, README section below) - just not necessarily illustrated by the exact
`wait_frames` values and screenshots cited here. The per-slot skin-buffer
fix itself (the actual subject of this checkpoint) is unaffected by any of
this - it's pure Vulkan command-buffer/memory-timing correctness, verified
independently of simulated-game-time control.

### Runtime polys (tr_model.cpp)

`RE_AddPolyToScene` was a stub until now - every call from game code (blaster
bolt trails/impacts, saber marks, and other one-off effect geometry that
isn't a Ghoul2 model or a world surface: see `code/cgame/FxPrimitives.cpp`,
`FxSystem.cpp`, `cg_localents.cpp`, `cg_marks.cpp`) was silently dropped.
Polys are now queued per scene (`s_scenePolys`, cleared by `RE_ClearScene`
same as Ghoul2 entities) and drawn by `VK_DrawScenePolys`, called from
`RE_RenderScene` right after `VK_DrawGhoul2Entities`.

Each `RE_AddPolyToScene` call copies its `polyVert_t` array immediately
(the caller's buffer isn't guaranteed to survive to draw time - real
`RE_AddPolyToScene`, `rd-vanilla/tr_scene.cpp`, does the same for the same
reason), capped at `MAX_SCENE_POLYS` (2048, the same order of magnitude as
rd-vanilla's real `MAX_POLYS`, `tr_local.h` - not a hard engine-parity
number, just a generous per-scene cap). A `polyVert_t` array is a **triangle
fan** (vertex 0 is the pivot) - confirmed against rd-vanilla's real
`RB_SurfacePolychain` (`tr_surface.cpp`), which generates `(0, i+1, i+2)`
triangles for `i` in `[0, numVerts-2)`. This renderer expands that fan into
a plain triangle list on the CPU at draw time (same "no GPU fan topology,
no index buffer" choice already made for the UI path) rather than using
`VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN`.

A small dedicated pipeline (`poly.vert`/`poly.frag`, `VK_CreatePolyPipeline`
in `tr_init.cpp`) draws them: world-space position, one UV set, and a
per-vertex RGBA modulate colour (`polyVert_t`, `rd-common/tr_types.h`) -
closer to the UI path's vertex shape than to `WorldVertex`'s (no lightmap
UV, no world fog - same scope cut already made for Ghoul2 models), but with
a real `mvp` and real depth testing (test on, write off, so a poly behind a
wall is correctly hidden but two overlapping polys don't fight each other -
`rd-vanilla` doesn't depth-sort/write polys against each other either) and
three blend-mode pipeline variants (alpha/additive/opaque, mirroring the UI
path's `vkBlendMode_t` handling) selected per-poly from its shader's parsed
blend mode. No new descriptor infrastructure was needed for this: an
`image_t`'s `descriptorSet` is built once at upload time
(`VK_UploadImage`, `tr_image.cpp`) against `vk.uiDescriptorSetLayout`/
`vk.uiSampler`, and the poly pipeline layout reuses that exact same set
layout (it needs the same "one plain texture" shape), so it's reused
directly rather than allocated again. Vertices are written into a
per-scene host-visible/coherent scratch buffer (`vk.polyVertexBuffer`,
`POLY_VERTEX_BUFFER_CAPACITY` = 16384 vertices), with a cursor reset once
per `VK_DrawScenePolys` call - a per-*scene*, not per-*frame*, reset, unlike
the UI path's `s_uiVertexCursor` (reset in `RE_BeginFrame`), because polys
are drawn once per `RE_RenderScene` call rather than interleaved with many
separate 2D draws across a frame; the same "drop the draw rather than
corrupt the buffer" overflow guard applies if a scene somehow queues more
vertices than the scratch buffer holds.

**Verified**: clean rebuild, zero warnings. No crash and no Vulkan
validation errors across all five map-based scenes (`sp_academy1_spawn`,
`sp_hoth2_spawn`, `sp_yavin1_spawn`, `sp_vjun1_spawn`) plus `sp_menu`, and
no visible regression in any of their screenshots. None of those scenes'
fixed spawn-time captures happen to catch a moment with an actual poly
effect on screen (blaster fire, saber marks, etc. are transient and
combat-triggered, not present at a cutscene's opening beats), so this is a
"doesn't crash, doesn't corrupt other rendering, wires up cleanly to real
game calls" verification rather than a pixel-level "a poly effect actually
appeared and looked right" one - the latter would need a scene manifest
entry timed to a specific in-game effect trigger, which doesn't exist yet.

### Sprite and oriented-quad ref entities (tr_model.cpp)

The follow-up to runtime polys: two of the non-`RT_MODEL` `refEntityType_t`
values `RE_AddRefEntityToScene` accepts now actually draw something instead
of being silently ignored - `RT_SPRITE` (a quad that always faces the
camera - muzzle flashes, impact glows, and similar billboarded effects) and
`RT_ORIENTED_QUAD` (a quad facing a fixed direction, `ent.axis[0]`, instead
of the camera - decal-like marks). Both are handled inside
`VK_DrawScenePolys`, not a separate function: a sprite/quad is just a
quad-shaped `PolyVertex` write into the same scratch vertex buffer runtime
polys use, drawn through the exact same pipeline/blend-mode selection, so
there was no reason to duplicate that machinery in a second function - only
`VK_EmitQuadStamp` (building the 4 corners and writing them as 6 non-indexed
vertices) and `VK_DrawPolyRange` (bind-pipeline-and-draw for one vertex
range) are new, and the latter was factored out of the existing poly-fan
loop too, since it was about to be duplicated a third time.

Corner math is copied, not rederived, from rd-vanilla's real
`RB_AddQuadStampExt`/`RB_SurfaceSprite`/`RB_SurfaceOrientedQuad`
(`tr_surface.cpp`): four corners at `origin +-left +-up` (`left`/`up` scaled
by `ent.radius`, optionally rotated in the quad's own plane by
`ent.rotation` degrees), triangles `(0,1,3)` and `(3,1,2)`. The one
real difference between the two types is where `left`/`up` come from:
`RT_SPRITE` takes them from the *camera's* axes (`refdef_t::viewaxis[1]`/
`[2]` - the same `[forward, left, up]` layout `VK_BuildViewMatrix` already
relies on, see its comment in `tr_world.cpp`), so the quad always faces the
viewer; `RT_ORIENTED_QUAD` takes them from the *entity's* own facing
(`MakeNormalVectors( ent.axis[0], ... )` - the real shared function from
`shared/qcommon/q_math.c`, not reimplemented here) so its orientation is
fixed regardless of camera angle. Color is the entity's `shaderRGBA`,
constant across all 4 corners, matching rd-vanilla. `RF_THIRD_PERSON`
entities are skipped unconditionally: rd-vanilla's real
`R_AddEntitySurfaces` (`tr_main.cpp`) only draws those in a portal/mirror
view ("self blood sprites, talk balloons, etc should not be drawn in the
primary view"), and this renderer has no portal/mirror support at all (see
"What's not implemented yet" below), so its one and only rendered view is
always that primary view - the real check's condition is unconditionally
true here rather than something that varies per frame.

A shared vertex-buffer cursor between the poly-fan loop and this one
(rather than each resetting its own to 0) is deliberate, not an oversight -
see `VK_DrawScenePolys`'s own comment for why a second reset would silently
corrupt whichever poly draws were recorded first, the same command-buffer-
execution-timing bug class documented in "Live animation" above for
Ghoul2's skin buffers.

**Verified**: clean rebuild, zero warnings. No crash and no Vulkan
validation errors across all five map-based scenes plus `sp_menu`, no
visible regression in any screenshot. A one-time-ish debug print (mirroring
`VK_DrawGhoul2Entities`' own) confirmed it never actually fired across any
of those captures - same limitation as runtime polys above: none of the
fixed spawn-time captures happen to catch a moment with an `RT_SPRITE`/
`RT_ORIENTED_QUAD` entity queued, so this is verified as "compiles, doesn't
crash, wires up to the real entity queue and the real math" rather than "a
sprite was seen on screen and looked right."

### Saber glow and beam ref entities (tr_model.cpp)

Two more `refEntityType_t` values, both drawn inside the same
`VK_DrawScenePolys` (still no reason to split them into their own
functions - see "Sprite and oriented-quad ref entities" above for why).

`RT_SABER_GLOW` is the soft additive halo around a lightsaber blade (the
blade core itself is a separate, already-working piece of geometry -
either a Ghoul2 weapon model or, more likely for the blade specifically,
something outside this renderer's current scope entirely; this is only the
glow). Copied from rd-vanilla's real `RB_SurfaceSaberGlow`/`DoSprite`
(`tr_surface.cpp`): a loop of camera-facing quads (exactly `VK_EmitQuadStamp`
with the *view's* left/up, i.e. the same math `RT_SPRITE` above already
uses) marching from the blade tip (`ent.saberLength` - the same union
member as `ent.rotation`/`ent.endTime`, see `refEntity_t`,
`rd-common/tr_types.h`) back toward the hilt, with the sprite radius
growing by a fixed `0.017` each step and the step size itself shrinking as
that radius grows, plus one larger "hilt glow" sprite at the entity's
origin whose size re-rolls a small random offset every frame (`Q_flrand`,
the real shared function - a deliberate subtle pulse per the real code's
own comment, not something to make deterministic). `RT_BEAM` is a 6-sided
open tube from `ent.origin` to `ent.oldorigin`, flat-colored by
`ent.skinNum` (0/1/2 = red/green/blue), built from `PerpendicularVector`
and `RotatePointAroundVector` (both the real shared functions,
`shared/qcommon/q_math.c`) exactly like the real `RB_SurfaceBeam`, then
expanded from a triangle strip into a triangle list on the CPU (matching
every other geometry source in this renderer - see poly.vert's comment for
why there's no strip/fan topology anywhere in this pipeline); winding
parity from the strip isn't preserved through that expansion because the
poly pipeline has no backface culling to begin with, so it can't matter.

Both hardcode a plain white texture and additive blending unconditionally
in the real code (`GL_Bind( tr.whiteImage )` /
`GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE )`), never resolving the
entity's own `customShader` at all - so this renderer does the same
(`vk.whiteImage`, a `forcedPipeline` parameter added to the shared
`VK_DrawPolyRange` helper that skips the normal blend-mode-from-image
lookup) rather than resolving and binding a shader real vanilla would have
ignored anyway.

**A real quirk in rd-vanilla, preserved verbatim, not "fixed":**
`RB_SurfaceBeam`'s `start_points` are never translated by `ent.origin` (see
its own commented-out `// VectorAdd( start_points[i], origin, ... )`), and
`R_RotateForEntity` (`tr_main.cpp`) confirms why that isn't a bug elsewhere
compensating for it: for every non-`RT_MODEL` `reType`, it returns the
plain view/world matrix unchanged, applying no entity transform at all. So
a real `RT_BEAM` always renders anchored near world-space `(0,0,0)`, using
only the *direction* from `origin` to `oldorigin`, never their actual
position - surprising, but this renderer's job is to reproduce rd-vanilla's
real behavior, not a "more correct" one it doesn't actually have. (This
same `R_RotateForEntity` finding also confirms, after the fact, that
`RT_SPRITE`/`RT_ORIENTED_QUAD`/`RT_SABER_GLOW` writing world-space
positions directly with no separate per-entity model matrix - which is what
this renderer already did for all three - was the correct choice, not an
assumption.)

One genuine defensive addition, not part of the real algorithm: the saber
glow loop is capped at 256 segments. Real `saberLength`/`radius` values
need nowhere near that many (typically a few dozen), but nothing in the
real formula stops a corrupt or pathological entity (`radius <= 0`) from
making the step size non-positive and looping forever; the cap turns a
would-be hang into "draws a very long glow and stops," never triggering for
any real saber.

**Verified**: clean rebuild, zero warnings. No crash and no Vulkan
validation errors across all five map-based scenes plus `sp_menu`. Debug
prints (mirroring the sprite/quad ones above) never fired across any
captured scene - academy1 and vjun1's opening cutscenes (the only scenes
this harness currently captures) never reach a moment with the player
holding an ignited saber or any `RT_BEAM` entity in view; manually forcing
a saber via console commands (`give all` / `weapon 1`) mid-cutscene didn't
help either, since the player isn't yet controllable and the scripted
sequence doesn't equip one. So, same honest caveat as the previous two
sections: this is verified as "compiles, doesn't crash, and the geometry
math was cross-checked line-by-line against the real formulas (catching and
fixing one real transcription bug of ours in the process - the saber glow
loop's decrement was initially written wrong, recomputing `i` from `seg *
radius` instead of tracking the real code's running subtraction, which
would have marched the wrong distance along the blade)," not "a glowing
saber or beam was seen on screen and looked right."

### Line and cylinder ref entities (tr_model.cpp)

Two more `refEntityType_t` values, same `VK_DrawScenePolys` home as
everything above. `RT_LINE` is a single flat quad from `ent.origin` to
`ent.oldorigin` (tracer-like effects) - copied from rd-vanilla's real
`RB_SurfaceLine`/`DoLine` (`tr_surface.cpp`): its width axis is
`normalize(cross(origin - vieworg, oldorigin - vieworg))`, not the view's
own up vector, so the quad always stays edge-on to the camera regardless of
the line's own orientation, a different (and more specific) camera-relative
construction than `RT_SPRITE`'s. `RT_CYLINDER` is a tube (or, when one end
tapers small enough, a cone - the same threshold and cone/cylinder split as
the real `RB_SurfaceCylinder`'s early-out into `RB_SurfaceCone`) between
`ent.origin` and `ent.oldorigin`, radius `ent.radius` at the bottom and
`ent.backlerp` at the top (the real code overloads that field, normally a
frame-lerp fraction, as the top radius for this one `reType` -
`RB_SurfaceCylinder`'s own comment flags this too). Segment count is
view-distance-adaptive in the real code, copied verbatim rather than this
renderer's usual fixed-subdivision simplification (see "3D world geometry"
above for that precedent with `MST_PATCH`) - it's cheap per-draw-call math
here, not an asset-time tessellation choice, so there's no real reason to
simplify it away.

Unlike `RT_SABER_GLOW`/`RT_BEAM`, both of these resolve and bind the
entity's real `customShader` (they're in `R_AddEntitySurfaces`' normal
shader-lookup bucket, `tr_main.cpp`, not the hardcoded-white-additive
group), so they go through the un-forced `VK_DrawPolyRange` path, same as
`RT_SPRITE`. `RT_CYLINDER`'s cone and cylinder cases each reconstruct the
real index buffer's exact vertex references as direct triangle-list
emission (no index buffer anywhere in this pipeline, per the established
pattern) - including the real "wrap the last segment's texture coordinate
back to the first ring" behavior, reproduced by computing each wraparound
vertex's own UV formula directly rather than literally duplicating a vertex
the way the real tesselator does, which comes out identical since both
approaches place the same position with the same UV.

**A real bug in the previous checkpoint, found and fixed while writing
this one**: `RT_SABER_GLOW` and `RT_BEAM` (added just before this) were
missing the `RF_THIRD_PERSON` gate that real `R_AddEntitySurfaces`
(`tr_main.cpp`) applies uniformly to this entire bucket of "simple
generated" ref entity types - noticed only because writing `RT_LINE`/
`RT_CYLINDER` right after made the missing check in the two before them
obvious side by side. Fixed by adding the same check both already have
copy-pasted correctly for `RT_SPRITE`/`RT_ORIENTED_QUAD`.

**Verified**: clean rebuild, zero warnings, no crash and no Vulkan
validation errors on `sp_menu`/`sp_academy1_spawn`/`sp_hoth2_spawn`/
`sp_vjun1_spawn` (`sp_yavin1_spawn`'s status was unknown at the time this
was first written - see "A capture-harness bug that invalidated a run of
'verified' claims" below for why, and for where its real status ended up).

### RT_ELECTRICITY ref entities (tr_model.cpp)

The last non-Ghoul2 `refEntityType_t` implemented so far: procedural
lightning bolts. By far the most algorithmically involved of this whole
group - genuinely recursive, not just a fancier quad - so it's split across
three functions (`VK_DoElectricityBoltSeg`/`VK_ApplyElectricityShape`/
`VK_EmitElectricityQuad`, `tr_model.cpp`) mirroring rd-vanilla's real
`DoBoltSeg`/`ApplyShape`/`DoLine2` (`tr_surface.cpp`) one-for-one rather
than flattened, since the real functions are themselves mutually
recursive and flattening them would obscure the algorithm's actual shape.

The real algorithm, in order: `RB_SurfaceElectricity` computes the bolt's
start/end (`ent.origin`/`ent.oldorigin`, with `RF_GROW` optionally
shortening the visible end point over time - `ent.endTime`/`ent.angles[1]`
as the grow duration, another case of `refEntity_t`'s union/`angles[]`
fields being reused per-`reType`, same pattern as `RT_SABER_GLOW`'s
`saberLength` and `RT_CYLINDER`'s `backlerp` above) and a view-relative
width axis (the same cross-product construction `RT_LINE` uses); then
`DoBoltSeg` walks that segment in fixed 16-unit steps, jittering a
*running* offset (never reset per step) by an amount seeded from the
entity's own `e->frame` (threaded through the whole recursive call tree by
pointer, exactly like real code threads `&e->frame` - the one part of this
algorithm that's per-entity-deterministic rather than using the engine's
global RNG); each step calls `ApplyShape`, which recursively splits its
segment into a jittered ternary "fractal" tree (using the *global* RNG,
`Q_flrand`, for the split points - `CreateShape` inlined rather than kept
as separate module-level scratch state, since every use happens before any
recursive call could overwrite it, so a plain local is behaviorally
identical without needing shared mutable state) down to a real
recursion-depth cap (`2 - r_lodbias->integer`, i.e. 9 leaf quads at the
default `r_lodbias 0`), terminating each leaf in one `DoLine2`-equivalent
quad. `RF_FORKED` bolts can additionally spawn up to 3 child `DoBoltSeg`
calls (`f_count`, threaded through as `forkBudget` the same way `rngSeed`
is) biased toward the bolt's real endpoint (`topLevelEnd`, kept separate
from each recursive call's own local `end` since forking needs the
*entity's* target, not whichever sub-segment happened to spawn the fork).
Like `RT_LINE`/`RT_CYLINDER`, this resolves the entity's real
`customShader` (same `R_AddEntitySurfaces` bucket).

One new registration this needed: `r_lodbias` itself (`tr_init.cpp`) -
this renderer had never read it before. Real code's `2 - r_lodbias->integer`
formula isn't clamped, so a very negative `r_lodbias` is a real (if
self-inflicted) way to make even rd-vanilla generate up to `3^n` leaf
quads per bolt-walk step; this renderer clamps the effective count to
`[0,6]` as a defensive addition (same spirit as `RT_SABER_GLOW`'s segment
cap above) - real quality settings (`r_lodbias` 0-3, only *reducing*
detail) are nowhere near that range, so the clamp never affects normal use.

**Verified past what every earlier ref-entity type in this document got**,
because a recursive, RNG-driven algorithm has more ways to go wrong than a
fixed-shape quad: a synthetic worst-case entity was injected directly
(temporarily, removed before commit) with every relevant `renderfx` flag
set at once (`RF_FORKED|RF_TAPERED|RF_GROW`), a 3000-unit bolt, and
`r_lodbias -3` (well past what the `[0,6]` clamp above exists for) - it
rendered every frame across a real run with no crash, no hang, and no
Vulkan validation errors. This is the one ref-entity checkpoint in this
whole series that got genuine confirmation the code executes correctly
under real (if synthetic) load, not just "compiles and never got queued by
any captured scene" - see the next section for why that distinction
matters more than it should have.

## A capture-harness bug that invalidated a run of "verified" claims

While stress-testing `RT_ELECTRICITY` above, the synthetic entity's debug
print never appeared in *any* capture, including ones with valid-looking
screenshots - a strong enough signal to investigate rather than shrug off,
and it uncovered two compounding bugs, both now fixed:

1. **A stale build artifact.** `cmake --build` for this target actually
   writes to `build-vulkan/code/rd-vulkan/rd-vulkan_x86_64.so` (matching
   the source's directory, CMake's default), but a `.so` also existed
   directly at `build-vulkan/rd-vulkan_x86_64.so` (the build root) from an
   earlier, one-off manual copy - and every subsequent verification step in
   this whole ref-entity series copied *that* stale file to the test
   `gamedata` install instead of the freshly-built one, without noticing,
   because both paths exist and look equally plausible. Fixed by deleting
   the stale root copy; going forward, always copy from
   `build-vulkan/code/rd-vulkan/rd-vulkan_x86_64.so`.
2. **A capture-harness bug, in `tests/render-regression/capture.py` itself,
   that made this much worse than a one-off stale binary.** `cl_renderer`
   is `CVAR_ARCHIVE` - it persists in whichever `--homepath`'s own saved
   config, across every future run against that homepath - and
   `capture.py` never set it explicitly. A *fresh* `--homepath` (nothing
   saved yet, e.g. any brand-new directory) silently falls back to the
   engine's compiled-in default, `rdsp-vanilla` for the `sp` binary - not
   `rd-vulkan`, regardless of what `--bindir` was actually built for, and
   with no error of any kind. Every ref-entity checkpoint's capture runs in
   this whole series used a freshly-created `--homepath` (a new directory
   each time), so **every one of those "no crash, no Vulkan validation
   errors" verifications above was actually run against `rdsp-vanilla`,
   not `rd-vulkan`, and never touched this renderer's code at all.** Fixed
   two ways: `capture.py` now takes a `--renderer` flag that sets
   `cl_renderer` explicitly for every run (see
   `tests/render-regression/README.md`), and this write-up says so plainly
   rather than leaving the earlier sections' "Verified" claims looking more
   solid than they were.

**What this means for the earlier sections' claims, now that they were
re-run correctly** (correct binary, `--renderer rd-vulkan`, real Xvfb+
lavapipe headless setup per `code/rd-vulkan/README.md`'s "Testing
headlessly"): runtime polys, `RT_SPRITE`/`RT_ORIENTED_QUAD`,
`RT_SABER_GLOW`/`RT_BEAM`, and `RT_LINE`/`RT_CYLINDER` all genuinely do
build clean and run with no crash and no Vulkan validation errors on
`sp_menu`/`sp_academy1_spawn`/`sp_hoth2_spawn`/`sp_vjun1_spawn` - the
"verified" claims in each of those sections above are correct, just
correct for reasons that were only actually confirmed afterward, not at
the time each section was originally written. `sp_yavin1_spawn` could not
be re-verified: see below.

**A second, unrelated pre-existing bug, previously only observed in
passing and now actually root-caused**: `sp_yavin1_spawn` crashes with
`rd-vulkan` - `TaskManager.cpp:725: Assertion '0' failed` inside ICARUS
(`code/icarus/`), the game's cutscene-scripting engine, not this renderer.
The "Curved surfaces (`MST_PATCH`)" bug entry above already noted this same
crash in passing (`yavin1` was excluded from that checkpoint's own
verification for exactly this reason) but never investigated further or
determined whether it was pre-existing - this is that follow-up. Confirmed
**not** a regression from any ref-entity checkpoint in this document: it
reproduces identically as far back as commit `1c4301e` (live
Ghoul2 animation, the last checkpoint verified in the session before this
whole ref-entity series began - tested via an isolated `git worktree`
build of that exact commit), and none of this renderer's ref-entity code
touches ICARUS or any other game-side state at all - it only reads
`refEntity_t` data already queued by `RE_AddRefEntityToScene`. The same
scene runs fine under `rdsp-vanilla`. Given this renderer's known,
already-documented Ghoul2/animation-timing divergence from vanilla (see
"Live animation" above - camera cuts and script-completion signals firing
at different real times than vanilla), the most likely explanation is that
some G2API return value or animation-completion signal ICARUS depends on
differs just enough on this scene's specific script to trip a genuinely
unexpected state - but that's a hypothesis, not a confirmed root cause.
**Not fixed here** - it's unrelated to every feature in this document, and
a real investigation (likely needing to trace which specific ICARUS
command in yavin1's script sequence hits this) is its own separate task.
Every scene capture and regression claim elsewhere in this document that
mentions `sp_yavin1_spawn`, `hoth2`, `vjun1`, `academy1` alongside it from
before this bug was found should be read as "the scenes that didn't crash
before this was discovered," not as "yavin1 was confirmed fine."

### RT_LATHE and RT_CLOUDS ref entities (tr_model.cpp)

The last two `refEntityType_t` values - every one of them is now handled in
some form (see "What's actually implemented" below). Both are lathed
(surface-of-revolution) shapes, copied from rd-vanilla's real
`RB_SurfaceLathe`/`RB_SurfaceClouds` (`tr_surface.cpp`), and both resolve
the entity's real `customShader` like the rest of this bucket.

`RT_LATHE` revolves a cubic-Bezier profile curve - `ent.axis[0]` (start),
`ent.axis[1]`/`ent.axis[2]` (the two Bezier handles), and `ent.oldorigin`
(end), only their X/Y components used as a 2D (radius, height) curve, not
an actual orientation - a full circle around a fixed world Z axis anchored
at `ent.origin`. `ent.endTime` optionally grows the visible profile length
over the real second before it; `ent.frame` - here a real timestamp of a
recent hit, not an RNG seed like `RT_ELECTRICITY`'s use of the same field -
drives a brief post-hit texture "pain" wobble decaying over one second,
using the entity's own floatTime (`fd->time * 0.001 - ent.shaderTime`,
matching the real per-entity `backEnd.refdef.floatTime`,
`tr_backend.cpp` - the first type in this series needing it). One quirk
preserved exactly: the wobble's phase truncates a dot product to an `int`
before use (real code: `int i = pt[0]*0.1f + pt[1]*0.1f;`) - a real
precision-losing accident in rd-vanilla, not something to "fix" by keeping
it as a float. Segment counts (both along the profile and around the
lathe) are LOD-scaled by `r_lodbias` exactly like the real code - already
clamped to a sane `[1,4]` range by the real formula itself, so unlike
`RT_ELECTRICITY`'s unclamped one, no extra defensive cap was needed here.

`RT_CLOUDS` is a disk (default) or tube (`RF_GROW`) built from a small
fixed-size strip of (position, alpha, curve-height) keyframes lathed
around a full circle, 30 degrees per step - `ent.radius`/`ent.rotation`
set the outer/inner radius, `ent.backlerp` scales curve height, and
`RF_GROW` negates `backlerp` ("needs to be reversed", the real comment)
and switches from the 4-keyframe disk table to the 6-keyframe tube one. A
real, deliberate-looking quirk kept exactly as rd-vanilla has it, not
"fixed": color RGB all come from `shaderRGBA`'s **red channel only**,
times the keyframe's own alpha value, while the vertex's actual alpha
stays constant at `shaderRGBA[3]` - the shape fades to black at its edges
rather than fading transparent, which only reads correctly under
additive-style blending where black is already invisible. Texture
coordinates are the vertex's final world-space X/Y scaled by `0.1`, not a
lathe-angle wraparound like `RT_LATHE` above - a different, simpler UV
scheme the two types don't actually share despite both being "lathe a
strip around a circle."

**Verified**: clean rebuild, zero warnings. No crash and no Vulkan
validation errors on `sp_menu`/`sp_academy1_spawn`/`sp_hoth2_spawn`/
`sp_vjun1_spawn` (correct binary, `--renderer rd-vulkan`, real Xvfb+
lavapipe setup - see the harness-bug section above for why that
qualification is stated explicitly now rather than assumed).
`sp_yavin1_spawn` still hits the pre-existing, unrelated ICARUS crash
documented above. Same honest caveat as every ref-entity type in this
whole series: debug prints never fired on any captured scene, so this is
verified as "compiles, doesn't crash, and the geometry/UV/color math was
cross-checked against the real formulas," not "seen on screen and looked
right."

With this, **every `refEntityType_t` value now does something** other than
`RT_MODEL`: real Ghoul2 rendering for `RT_MODEL`, real geometry for all
nine of the "simple generated" types (`RT_SPRITE` through `RT_CLOUDS`), and
a real no-op for `RT_PORTALSURFACE` (matching rd-vanilla's own real
behavior for it, not a gap - see `R_AddEntitySurfaces`, `tr_main.cpp`).

## Character animation investigation: four real bugs, and a wrong
conclusion corrected

**Bottom line up front, since the investigation below initially got this
wrong: every NPC in every scene stood in the same non-gameplay pose because
animation was never being applied to *any* character, *ever* - not a
test-harness timing artifact.** The actual cause was `RE_GetAnimationCFG`
(below) unconditionally reporting "file not found" for every `animation.cfg`
in the game, which made every character's per-anim frame data permanently
zeroed out, which made `PM_SetAnimFinal` (`bg_panimate.cpp`) silently no-op
on literally every `NPC_SetAnim` call it ever received. This was caught
because a user directly disputed an earlier, wrong conclusion in this same
section (see "What this does and doesn't mean", further down, for that
history) by pointing out a specific, correct, falsifiable observation: the
same strange pose appeared on *every* character in *every* screenshot, which
a pure test-harness timing artifact cannot produce (that would show
different-but-still-plausible poses at different wrong moments, not one
constant impossible one) but a "no animation ever applies" bug produces
exactly.

A direct, side-by-side `rd-vanilla` vs. this renderer comparison at
matched simulated time (following up on the "Live animation" checkpoint's
own camera-cut-timing caveat above) turned up something worth its own
write-up: at `sp_academy1_spawn`'s scripted `wait_frames` value, this
renderer showed the intro cutscene's camera at a completely different
cut - the player character in side profile against two courtyard pillars -
while `rd-vanilla` at the identical `wait_frames` still showed the
cutscene's very first front-facing portrait shot. Not a subtle drift; a
different scene entirely. This looked exactly like it should be the "Live
animation" checkpoint's documented ICARUS-timing-divergence mechanism
(this renderer's single-track animation reaching its own "done" signal at
a different real time than vanilla's real per-region blended one) finally
showing up somewhere severe enough to actually chase down.

**Bug 1 (functional, in this renderer): `G2API_GetBoneIndex` always
returned -1.** Reading rd-vanilla's real `bg_panimate.cpp` confirmed the
"Live animation" checkpoint's mechanism was real: the game calls
`G2API_SetBoneAnimIndex`/`GetBoneAnimIndex` *separately* for `bodyBone`
(legs) and `torsBone` (upper body) - e.g. `bodyAnimating`/`torsAnimating`
are genuinely independent per-region completion checks - and this
renderer's `VK_SetGhoul2BoneAnim`/`GetBoneAnim` collapsed every call,
regardless of target, into one shared whole-skeleton track, so a query for
one region could silently return a *different* region's timing entirely.
But `bodyBone`/`torsBone` themselves come from `gi.G2API_GetBoneIndex(...,
boneName, ...)` (`g_client.cpp` and ~60 other call sites across
`g_combat.cpp`, `g_turret.cpp`, `g_emplaced.cpp`, `g_mover.cpp`, ...), and
this renderer's implementation was `{ return -1; }` unconditionally,
regardless of input - a stub nobody had noticed because the bone-collision
bug above meant every caller's index was already being ignored anyway.
Fixed both together, since fixing one without the other would have
accomplished nothing: `G2API_GetBoneIndex` now resolves a bone name to its
real skeleton index via the existing `VK_FindGhoul2Bone` lookup (already
used for bolts), `s_ghoul2AnimState` is now keyed by `(CGhoul2Info*,
boneIndex)` instead of just `CGhoul2Info*`, and `VK_ComputeGhoul2Pose`
resolves each bone's frame by walking up its parent chain to the nearest
bone with its own explicit track (itself or an ancestor) - the real
engine's own bone-tree inheritance rule (a `torsBone` override only
affects that bone and its descendants; everything else keeps whichever
ancestor's track actually covers it), not the previous single frame
applied uniformly to the whole skeleton. This is a real, confirmed
correctness fix, verified by reading the actual call pattern it fixes -
**but empirically it changed nothing for academy1's specific divergence**:
instrumenting `VK_SetGhoul2BoneAnim` directly confirmed *zero* calls to it
happen anywhere in that scene's captured window (the player's held idle
pose there apparently never re-triggers `NPC_SetAnim`/`PM_SetAnimFinal`),
so a per-bone-tracking bug literally couldn't have been the culprit for
*this* symptom. It's still real, valuable infrastructure for any scene
that does exercise multi-region animation (combat gestures, torso
aim-independent-of-legs, etc.) - see below for the diff that isolated its
actual (lack of) effect on the scenes this renderer's test suite covers.

**Bug 2 (test harness, much bigger in practice): the `fixedtime` cvar
was never actually being set.** Chasing why the per-bone fix changed
nothing led to checking whether `fixedtime` itself was even doing its job,
and it wasn't: `com_fixedtime` (the name used throughout this file,
`tests/render-regression/scenes.json`, and every ad-hoc console command
run during this whole investigation) is the cvar's **C++ variable name**,
not its **registered name** - `qcommon/common.cpp`'s own
`Cvar_Get("fixedtime", "0", CVAR_CHEAT)` call registers it as `"fixedtime"`
with no `com_` prefix at all. `+set com_fixedtime 16` doesn't error or
warn - `+set` silently creates a brand new, entirely unrelated,
never-read cvar with that literal name - so every capture in this entire
project that used `com_fixedtime` (which, per a repository-wide search
while fixing this, is *all of them*) ran with genuinely uncontrolled,
render-speed-dependent `wait_frames` timing the whole time, exactly the
failure mode `fixedtime` exists to prevent. Re-running the academy1
comparison with the *correct* `+set fixedtime 16` changed the result
completely: at the same `wait_frames` this renderer was diverging at,
`rd-vanilla` now also lands on the wide balcony cutscene shot, with
similar NPC counts/positions/poses - not the front-facing portrait it
showed under the broken cvar. The dramatic, "completely different scene"
divergence that kicked off this whole investigation was, in large part, an
artifact of comparing two renderers at genuinely different, uncontrolled
points in the same script - not a rendering or animation bug. Fixed in
`tests/render-regression/scenes.json` (`extra_set` now says `"fixedtime"`)
and in every `com_fixedtime` reference across this file and
`tests/render-regression/README.md`.

**What this does and doesn't mean for character animation quality (original,
wrong conclusion, kept here for the record)**: with `fixedtime` actually
working, a fresh `sp_academy1_spawn` diff still isn't a pixel `MATCH`
(`MAJOR_DIFF`, ~23% mean difference) - but visually inspecting the diff image
showed the *same* camera cut and *plausible-looking* NPC layout, so this
write-up originally concluded the severity of the character-animation
problem was "substantially a test-methodology artifact." **That conclusion
was wrong, and a user correctly called it out**: every character's pose
looked plausible in isolation but was actually *the same fixed pose*, not
gameplay animation - visible only by comparing multiple screenshots of
different scenes/moments side by side and noticing the pose never changes,
which a single before/after diff of one scene doesn't surface. Chasing that
down properly (rather than re-asserting the `fixedtime` explanation) found
the real cause:

**Bug 3 (functional, in this renderer, the actual root cause):
`RE_GetAnimationCFG` always reported the file as not found.** Every
character's per-animation frame data (`firstFrame`/`numFrames`/`loopFrames`/
`frameLerp` for each of `MAX_ANIMATIONS` named animations, e.g. `BOTH_STAND1`,
`BOTH_RUN1`, ...) comes from parsing a model's `animation.cfg` -
`NPC_stats.cpp`'s `G_ParseAnimationFile` reads it via `gi.RE_GetAnimationCFG`,
a renderer entry point (real implementation: `rd-vanilla`'s `tr_skin.cpp`,
which opens the file via `ri.FS_FOpenFileRead`/`FS_Read` and returns its
text). This renderer's version was `{ return 0; }` unconditionally, with no
filesystem access at all - so `G_ParseAnimationFile` always took its
"couldn't read the file" early-out, leaving every `animation_t` entry in
every `level.knownAnimFileSets` slot at its zeroed init state
(`numFrames == 0`). `bg_panimate.cpp`'s `PM_SetAnimFinal` - the function
every `NPC_SetAnim` call in the game funnels through - checks exactly that
(`if (animations[anim].numFrames==0) { ...; return; }`) essentially first
thing, before it ever gets to the per-region `bodyBone`/`torsBone` logic Bug
1 above fixed. So this one stub meant **every** `NPC_SetAnim` call for
**every** character, for the entire life of the process, silently did
nothing - confirmed empirically by temporarily instrumenting that exact
early-return in a debug build of the game module (`jagamex86_64.so`) and
seeing it fire on 100% of `PM_SetAnimFinal` calls in an academy1 run, never
once falling through to the code that actually plays an animation. Fixed by
implementing `RE_GetAnimationCFG` for real (`tr_init.cpp`): reads the file
via `ri.FS_FOpenFileRead`/`FS_Read`/`FS_FCloseFile` (the same `ri` entry
points already used elsewhere in this renderer) and copies its text into the
caller's buffer, matching rd-vanilla's contract exactly. (rd-vanilla also
caches file contents by filename as a documented dev-mode hot-reload
convenience - not reproduced here, since `G_ParseAnimationFile` already only
calls this once per distinct skeleton name per level via its own
`knownAnimFileSets` lookup, so the cache is a performance nicety rather than
something correctness depends on.)

Fixing this exposed a second, smaller pre-existing gap in the same
subsystem: `G2API_GetGLAName` (used by `G_LoadAnimFileSet` and others to
find *which* `animation.cfg` to parse for a given model) was hardcoded to
always report `"models/players/_humanoid/_humanoid.glm"` regardless of the
actual model - harmless by accident for academy1-style all-humanoid scenes
(every character happens to want that exact skeleton name anyway) but wrong
for any non-humanoid Ghoul2 model (droids, creatures with their own
skeleton), and immaterial either way while `RE_GetAnimationCFG` was
guaranteed to fail regardless of which filename it was asked for. Fixed
alongside Bug 3: `G2API_GetGLAName` now resolves the real `.gla` name via a
new `VK_GetGhoul2GLAName` (reads `mdxaHeader_t::name` straight out of the
already-resident skeleton file data, `tr_model.cpp`), falling back to the
standard humanoid path only when a model's particular skin combination
failed to resolve any drawable surfaces at all (`ghlInfo->mModel == 0`) -
`g_client.cpp`'s `G_StandardHumanoid` asserts this call never returns null,
so a bare `nullptr` on that pre-existing, separate skin-resolution gap
would have turned a already-known cosmetic issue into a hard crash instead
of leaving it exactly as visible (or not) as it was before.

**Bug 4 (functional, in this renderer, found by a second round of user
pushback): `VK_RegisterSkin` didn't implement the three-part
`head|torso|lower` composite skin macro at all** - a scope cut the "Skin
support" bullet above already disclosed, but one that turned out to matter
far more than "cosmetic." Asked to check again after Bug 3 shipped, because
the fixed poses still looked "far from exact resemblance" to vanilla side
by side, direct comparison at matched crops showed vanilla's academy1
courtyard with roughly twice as many visible NPCs as this renderer's -
not a pose difference, an *entity count* difference. The log line
`rd-vulkan: ghoul2: 9/18 scene entities drew 9 sub-model(s)` was the tell:
half of academy1's NPCs weren't drawing anything at all. Those are the
generic filler-student models (`jedi_tf`, `jedi_hm`, `jedi_hf`, `jedi_kdm`,
`jedi_zf`, `jedi_rm` - randomized head+torso+lower-body combinations, used
for background students in the sparring scene), and every one of them is
assigned a skin name like `models/players/jedi_tf/|head_a1|torso_a1|lower_a1`
rather than a plain `.skin` file path. `VK_RegisterSkin` had no special
handling for the `|`-delimited macro syntax, so it tried to
`ri.FS_ReadFile` that literal string as a filename (logged as
`VK_RegisterSkin: ...|head_a1|torso_a1|lower_a1 not found`), got nothing,
and every surface of every one of those models fell back to
`VK_LoadGhoul2Model`'s "has no drawable surfaces" case - a fully loaded,
fully animated, completely *invisible* entity. Fixed by porting
rd-vanilla's real `RE_SplitSkins`/`RE_RegisterIndividualSkin` (`tr_skin.cpp`)
as `VK_SplitCompositeSkinName`/`VK_ParseSkinFile` (`tr_model.cpp`): split the
macro into its three real `.skin` file paths, parse and merge all three into
one `VulkanSkin` (including the real `"_off"`-suffixed-surface-name
handling, not just the common case), de-duplicating identical part paths the
same way the real function does. Verified: academy1's log now reads
`ghoul2: 18/18 scene entities drew 18 sub-model(s)`, and a direct crop
comparison shows the full crowd/formation - not just the handful of
already-single-skinned named characters that happened to work before -
now visibly matching vanilla's NPC count and rough layout for the same
cutscene moment.

**What this actually means for character animation quality**: with all
four bugs fixed, `sp_academy1_spawn` now shows the *right number* of NPCs,
in varied, distinct poses, in roughly the right formation, matching the
general character of rd-vanilla's same cutscene moment (a sparring/training
scene - some standing, some walking, some down) instead of one identical
frozen pose repeated on a fraction of the models. The `diff.py` mean
pixel-difference figure for `sp_academy1_spawn` dropped from ~23.5% (Bug
3 fixed, Bug 4 not yet found) to ~17.1% (both fixed) against the matched-
`fixedtime` vanilla capture. It is still not a pixel `MATCH`, and individual
poses still don't line up frame-for-frame with vanilla's - the single-track,
no-blend, no-sub-frame-interpolation simplifications documented in "Live
animation" above are all still real and still there, so a scripted combat
sequence's exact choreographed beats (which frame of which animation a
given character is on at a given millisecond) won't match rd-vanilla's real
multi-track blended timing model bone-for-bone without reproducing that
model in full - a substantially larger undertaking than either bug fixed
here. What changed is qualitative, not just quantitative: every character is
now the right model, visible, and actually animating in a plausible,
varied, roughly-correctly-timed way, which was not true at all before this
investigation.

**One thing this investigation explicitly ruled out**: `sp_yavin1_spawn`'s
pre-existing ICARUS crash (documented above) was hypothesized to be
possibly related to a wrong G2API animation-completion signal - it isn't,
or at least not *this* one. Re-tested after the `G2API_GetBoneIndex`/
per-bone fix above with the real `fixedtime` cvar: identical crash, same
assertion, same line. That hypothesis is now closed; the real cause is
still unknown and still out of scope for this write-up.

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
  `MST_PLANAR`/`MST_TRIANGLE_SOUP` surfaces are kept, and `MST_PATCH`
  (curved surfaces) are tessellated at a fixed subdivision level - see "3D
  world geometry" above; `MST_FLARE` is still skipped, not drawn;
  `SURF_NODRAW`/`SURF_SKY`
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
- Ghoul2 (character/weapon model) rendering (`tr_model.cpp`) - real `.glm`
  mesh parsing, `.skin` texture resolution (single-file case only),
  bind-pose bone bolts, per-frame entity dispatch through the same
  pipeline/vertex-format world geometry uses, and (see "Skeletal animation"/
  "Live animation" above) real per-bone mesh skinning driven by a real,
  live, time-driven animation state per model instance
  (`G2API_SetBoneAnim`/`GetBoneAnim`/etc genuinely work now) - one
  whole-skeleton animation track per instance, not the real engine's
  independently-blended per-bone-subtree tracks, and no animation
  blending/crossfade or sub-frame interpolation, but real bone math and
  weighted skinning driving an actually-moving model, not a static pose.
  Still missing: LOD selection, per-surface on/off overrides, gore.
- Runtime polys (`RE_AddPolyToScene`, `tr_model.cpp`) - see "Runtime polys"
  above. Real per-scene queueing, CPU fan-to-triangle-list expansion, and a
  dedicated pipeline with the same three blend-mode variants (alpha/
  additive/opaque) the UI path has, reusing the UI descriptor-set layout/
  sampler rather than new descriptor infrastructure.
- `RT_SPRITE` (camera-facing billboard) and `RT_ORIENTED_QUAD`
  (fixed-direction quad) ref entities - see "Sprite and oriented-quad ref
  entities" above. Real quad-stamp geometry matching rd-vanilla's corner
  math and winding exactly, drawn through the same pipeline runtime polys
  use (no new pipeline needed).
- `RT_SABER_GLOW` (lightsaber blade glow) and `RT_BEAM` (a flat-colored
  tube between two points) ref entities - see "Saber glow and beam ref
  entities" above. Real geometry math (including `RT_BEAM`'s real
  never-translated-by-origin quirk, preserved rather than fixed) drawn
  through the same pipeline as the other poly/sprite paths, with a
  `forcedPipeline` override so their real hardcoded-additive-white-texture
  behavior is reproduced exactly rather than resolving a shader real
  vanilla ignores.
- `RT_LINE` (a camera-edge-on tracer quad) and `RT_CYLINDER` (a tapered
  tube/cone) ref entities - see "Line and cylinder ref entities" above.
  Both resolve the entity's real `customShader` like `RT_SPRITE` does;
  `RT_CYLINDER`'s cone/cylinder split and view-distance-adaptive segment
  count match the real code exactly rather than using this renderer's
  usual fixed-subdivision simplification.
- `RT_ELECTRICITY` (procedural fractal lightning bolts) ref entities - see
  "RT_ELECTRICITY ref entities" above. The only ref-entity type so far
  needing genuine recursion (real `DoBoltSeg`/`ApplyShape` copied
  one-for-one, not flattened) rather than a fixed-shape quad/tube, and the
  only one stress-tested directly (synthetic worst-case entity, all
  relevant `renderfx` flags at once) rather than only confirmed to compile
  and never get queued by a captured scene.
- `RT_LATHE` (a lathed cubic-Bezier profile) and `RT_CLOUDS` (a lathed
  disk/tube of fixed keyframes) ref entities - see "RT_LATHE and
  RT_CLOUDS ref entities" above. The last two `refEntityType_t` values;
  every one of them now does something other than `RT_MODEL` (or, for
  `RT_PORTALSURFACE`, correctly nothing - matching rd-vanilla's own real
  behavior for it).

## What's not implemented yet (safe no-ops, won't crash, won't draw)

- Ghoul2 skeletal animation is live now (see "Live animation" above:
  `G2API_SetBoneAnim`/`GetBoneAnim`/etc really work, time-driven, not
  stubs) but only as **one whole-skeleton animation track per model
  instance** - not the real engine's independently-blended per-bone-subtree
  tracks (legs/torso/face playing different animations at once), and with
  no blending/crossfade between two animations, no sub-frame interpolation
  (frames step discretely), and no reverse/negative-speed playback. `
  SetAnimIndex`/`GetAnimIndex` (selecting *which* `.gla`/animation-file a
  model uses, a different concept from which frame within one - relevant
  for NPCs with a per-level animation-file override, e.g.
  `_humanoid_academy1.gla`) are still stubs; every model always uses
  whichever single `.gla` `VK_LoadGhoul2Skeleton` first resolved for it.
  Also still missing: LOD selection, per-surface on/off overrides
  (`SetSurfaceOnOff`), gore, tags, ragdoll, model-to-model attachment
  (`AttachG2Model`/`AttachEnt`), and surface bolts (a bolt naming a
  *surface* rather than a bone - only bone bolts are implemented, see
  "Ghoul2 rendering" above for `AddBolt`/`GetBoltMatrix`, itself still
  bind-pose-only regardless of what the mesh is doing). See "Ghoul2 is not
  reused from rd-vanilla" below for why the animation system was a separate,
  larger task than the rest of this renderer.
- Dynamic/scripted fog changes (`RE_SetRangedFog`/`R_SetTempGlobalFogColor`
  are stubs, used by e.g. weather-effect entities in `g_fx.cpp` for
  mid-level fog changes) and per-brush *local* fog volumes (a `LUMP_FOGS`
  entry with a real `brushNum`, bounded to one convex region) - only a
  map's single static *global* fog (see "3D world geometry" above) is
  implemented.
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
- Flares (`MST_FLARE`) - skipped entirely at load time, not just unlit.
  Curved surfaces (`MST_PATCH`) *are* tessellated now (see "3D world
  geometry" above), but only at a fixed subdivision level, not rd-vanilla's
  real adaptive one - large or nearly-flat patches get more triangles than
  they need.
- Proper sky rendering: what's implemented (see "3D world geometry" above)
  is a flat, non-subdivided skybox using the sky shader's *name* as its
  basename - no `.shader` `skyparms` parsing (so a level whose sky script
  points at a different-named basename won't find its faces), no dome
  warping/subdivision (visible seams at box edges), no `RDF_SKYBOXPORTAL`
  (a portal showing a miniature separate scene - a distinct, unimplemented
  feature from the base skybox).
- Full `.shader` script parsing: only a defined shader's first stage's
  `map`/`blendFunc`, and (for fog shaders specifically, see "3D world
  geometry" above) a top-level `fogparms` line, are read - later stages,
  `tcMod` animation, `rgbGen`/`alphaGen` waves, and `skyparms` are still
  ignored. World geometry doesn't even get the 2D path's blend-mode
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
rd-vanilla's own `tr_model.cpp`/`tr_shader.cpp` - **not** the same file as
this renderer's own `tr_model.cpp`, see below) to actually link. That's not
"free" reuse, it's "port rd-vanilla's whole animation/bone-transform system
too." Given that's a separate, larger task, every `G2API_*` entry point
*beyond* model loading and skin registration is still stubbed (see
`tr_init.cpp`) - no skeletal animation, bolts, LOD selection, surface on/off
overrides, gore, or ragdoll.

What **is** real, and *is* a from-scratch reuse-avoiding implementation
rather than a stub: `.glm` mesh parsing and static bind-pose rendering
(`tr_model.cpp` in *this* directory - see "Ghoul2 rendering" above). That
file re-derives the offset arithmetic from `mdx_format.h`'s shared,
GL-agnostic structs directly (checked field-for-field against rd-vanilla's
real `R_LoadMDXM`) rather than needing rd-vanilla's model registry at all -
bind-pose geometry doesn't need bone transforms, so it sidesteps the
animation-system reuse problem entirely for this first pass.
`CGhoul2Info_v`'s backing store (`IGhoul2InfoArray`, `tr_init.cpp`) is also
real, small and self-contained.

One easy-to-repeat mistake in that backing-store implementation,
`CVulkanGhoul2InfoArray` (`tr_init.cpp`): its `New()` must never return
handle `0` - `CGhoul2Info_v` (`game/ghoul2_shared.h`) uses `0` as its own
"not allocated" sentinel, so a genuinely-valid handle of `0` is
indistinguishable from "empty" and trips `assert(mItem)` in
`CGhoul2Info_v::operator[]`. The constructor burns index 0 at startup
(permanently marked invalid) so real allocations start at 1 - see its
comment. `tr_model.cpp`'s own model/skin caches (indices into
`s_ghoul2Models`/`s_skins`) follow the identical "index 0 is reserved,
0 == not-yet-loaded/failed-to-load" convention for the same reason.

Also worth knowing: `G2API_InitGhoul2Model`/`RE_RegisterModel` still report
*success* even when the underlying model fails to load or has no drawable
surfaces (`mModel` just stays `0`, silently skipped at draw time) - that's
deliberate, not an oversight, see "3D world geometry" above for the
game-side fatal-error chain (`Com_Error(ERR_DROP, ...)`) that a failure
return triggers, which would otherwise abort map loading before
`RE_RenderScene` ever runs. `RE_RegisterSkin` is a real implementation now
(see "Ghoul2 rendering" above), not part of that fake-success list.

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

**`+wait N` counts rendered client frames, not elapsed game time** - a real,
recurring source of noise when comparing two renderers or two runs, called
out several times elsewhere in this file (academy1's and hoth2's cutscene-
camera investigations above). Two renderers with different per-frame cost
(this one is markedly slower than `rd-vanilla` under lavapipe vs. llvmpipe)
reach different points along a real-time-paced ICARUS script by the time
each has rendered its Nth frame, making any comparison at a fixed
`wait_frames` an apples-to-oranges snapshot unless the scene happens to be
static. **Fix: `+set fixedtime <ms>`** (`qcommon/common.cpp`'s
`Com_ModifyMsec`) forces *every* frame - regardless of how long it actually
took to render - to advance the simulated clock by exactly that many
milliseconds, making `wait_frames * fixedtime` a precise, renderer-
speed-independent amount of game time. It's `CVAR_CHEAT`-flagged but freely
settable here since `devmap` auto-enables cheats in SP.

**The cvar's real name is `fixedtime`, not `com_fixedtime`** - its C++
variable is named `com_fixedtime`, but the string actually registered with
`Cvar_Get` (`qcommon/common.cpp`) is `"fixedtime"`, and `+set`/config-file
cvar references go by that registered name, not the variable name. Whether
the original investigation into this technique typed the correct
`+set fixedtime <ms>` directly at the console isn't something that can be
verified after the fact, but the wrong key (`com_fixedtime`) is what ended
up permanently baked into `tests/render-regression/scenes.json`'s
automation - and went undetected for a long time, because `+set` silently
creates a new, harmless-looking, entirely unread cvar for any unrecognized
name rather than erroring. See "Character animation investigation: four
real bugs, and a wrong conclusion corrected" further below for the full
account of how this was finally caught, what it actually changed once
fixed, and why the specific hoth2/academy1 screenshots this section used
to cite as "verified" matches are no longer cited here - they can't be
vouched for without knowing
whether that particular investigation's own commands used the correct name
or not, and re-deriving that with certainty isn't possible after the fact.

This is no longer just a manual technique for one-off investigations -
`tests/render-regression/scenes.json` sets `fixedtime` on every map-based
scene (see that harness's own README for the same explanation from the
"comparing two captures" side, and its own note on this same cvar-name
mistake), so any future `capture.py` run against this renderer and
`rd-vanilla` produces genuinely comparable screenshots by default, and
`diff.py`'s mean-pixel-difference numbers on those scenes reflect real
rendering differences rather than an unrelated mix of "different
rendering" and "different moment of the same script." Screenshot pairs
captured before `fixedtime` was set *under its correct name* don't have
that property and shouldn't be used to draw conclusions about a
`MAJOR_DIFF`/`MINOR_DIFF` result on any scene with a `map` - which, given
the mistake above, includes essentially every capture taken via
`scenes.json` before the fix described below, not just captures from
before `com_fixedtime` was added to it at all.
