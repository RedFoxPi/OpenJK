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
  (Local fog volumes like vjun1's `fog_black` are no longer ignored - see
  "Local fog volumes and ranged fog" below.)
  `RE_SetRangedFog`/`R_SetTempGlobalFogColor` (the *dynamic* fog API, used
  for scripted fog changes mid-level, e.g. weather-effect entities in
  `g_fx.cpp`) were still no-ops at the time of this fix - only the BSP's
  static, load-time global fog was implemented here.
  (Both are real now - `R_SetTempGlobalFogColor` since "World weather/
  particle effects", `RE_SetRangedFog` since "Local fog volumes and ranged
  fog", both below.)
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
  aren't implemented **at the time of this investigation - see "Ghoul2
  surface bolts" below for the later pass that added them**) and bolt
  matrices compose to plausible non-degenerate
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

**Caveat added much later, see "Character animation investigation: eight
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

**Update, much later: the per-bone-track scope cut above is now real, and
blending + sub-frame interpolation are now implemented too** - see
"Full `G2_TimingModel` port: blending and sub-frame interpolation" further
below. What's left unimplemented from this section's original list is just
reverse/negative-speed playback (ported faithfully anyway, in case a future
caller ever uses it, but no real caller currently does) and bone-angle
overrides/ragdoll/IK (a separate, still-real scope cut - see
`G2API_SetBoneAngles*`'s stubs).

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
eight real bugs, and a wrong conclusion corrected" below): the specific
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

### Full `G2_TimingModel` port: blending and sub-frame interpolation (tr_model.cpp)

Follow-up to the character-animation investigation above (see "four real
bugs, and a wrong conclusion corrected" below): once real animation.cfg
parsing and the composite-skin fix made every character actually animate,
visibly, as the right model, a user asked for this renderer's animation to
be "an adequate replacement" for rd-vanilla's - not an approximation of it.
The remaining gap after those bug fixes wasn't a bug, it was a real,
previously-disclosed scope cut: this renderer computed exactly one whole
frame per bone-track with no interpolation between frames and no
cross-fading between an old animation and a new one, while rd-vanilla's
real `G2_TimingModel`/`G2_TransformBone` (`tr_ghoul2.cpp`) do both. Closing
that gap meant porting that real code, not reinventing an approximation of
it.

**`VK_Ghoul2TimingModel`** (`tr_model.cpp`) is a branch-for-branch port of
rd-vanilla's real `G2_TimingModel`: given a bone's animation state
(startFrame/endFrame/startTime/animSpeed/flags/pauseTime) and the current
time, it decides which two whole frames to interpolate between
(`currentFrame`/`newFrame`) and by how much (`backlerp` - the weight on
`newFrame`; `1-backlerp` on `currentFrame`). Ported *all* of the real
function's branches, including forward/reverse playback and the
loop-wraparound/freeze-at-the-end cases for each, not just the common
forward-with-loop-or-freeze path bg_panimate.cpp actually exercises today -
reverse (negative `animSpeed`) playback has no real caller in this game
right now, but the point of a port is fidelity to the source, not fidelity
to today's call sites. `VK_Ghoul2CurrentFramePosition` wraps it into the
single continuous (fractional) frame-position value real code reports from
`GetBoneAnim` and uses internally to snapshot a blend (see below) -
verified to satisfy the same invariant real code relies on
(`currentFrame + backlerp` is always the wrapped/clamped continuous
position, whatever branch produced it).

**Sub-frame interpolation**: `VK_ComputeGhoul2BoneRecursive` now uncompresses
up to two adjacent frames' bone quaternions and linearly interpolates the
resulting 3x4 matrices component-wise by `backlerp` when it's nonzero
(`VK_LerpGhoul2BoneMatrix`) - the exact same simplification real
`G2_TransformBone` makes (a matrix lerp, not a true quaternion slerp; the
real engine doesn't do the more "correct" thing here either, so this isn't
a corner cut relative to it). Previously every bone snapped discretely from
one whole frame to the next; now it's continuous, matching real playback
smoothness frame-for-frame at the same simulated time.

**Animation-to-animation blending** (`BONE_ANIM_BLEND`, `game/
ghoul2_shared.h` `0x0080`) is real now too, not dropped. `bg_panimate.cpp`
sets this flag (with a 350ms default `blendTime`) on essentially every
`PM_SetAnimFinal` call that starts a genuinely new animation on a bone - not
a rare edge case, ordinary gameplay - so leaving it unimplemented meant
every animation change was a hard cut rather than the real engine's
cross-fade. `VK_SetGhoul2BoneAnim` now captures a *frozen* snapshot of
wherever the *previous* animation on that bone was (via
`VK_Ghoul2CurrentFramePosition` on the about-to-be-overwritten state,
run through the same frame-wrap/loop-flag clamping real
`G2_Set_Bone_Anim_Index` applies) at the exact moment a new blended
animation begins - `blendFrame`/`blendLerpFrame`/`blendStartTime`/
`blendDurationMs` on `VulkanGhoul2AnimState`. `VK_ResolveGhoul2BonePose`
checks whether that blend window (`blendStartTime` to `blendStartTime +
blendDurationMs`) is still open for the current draw and, if so,
cross-fades the frozen old pose against the live new one with a linearly
increasing weight - matching real `G2_TransformBone`'s
`blendTime>=0.0f && blendTime<boneList[...].blendTime` gate and its
`blendLerp*new + (1-blendLerp)*old` arithmetic exactly, including the real
function's own (verified-against-source, not "fixed") old-pose
interpolation convention where the captured position's fractional part
weights the *floor* frame rather than the *next* one - a real, faithfully
reproduced quirk, not a bug introduced here. If there was nothing previously
animating on a bone to blend from, blending is silently dropped for that
call, matching real code's own fallback.

**`setFrame` continuity** (`G2API_SetBoneAnim`/`SetBoneAnimIndex`'s
previously-ignored `setFrame` parameter) is real now as well:
`bg_panimate.cpp` passes a re-affirmed animation's already-queried current
frame back in on every call that doesn't actually change anything but
flags/blend state, specifically so the animation doesn't visibly restart
from frame 0 - `VK_SetGhoul2BoneAnim` now solves `startTime` for exactly
that continuity via the same algebra rd-vanilla's real
`G2_Set_Bone_Anim_Index` uses.

**Verified**: rebuilt and re-ran the full SP scene suite
(menu/academy1/hoth2/yavin1/vjun1) - no crashes, no new warnings, `ghoul2:
18/18 scene entities drew 18 sub-model(s)` unchanged from the composite-skin
fix above. Confirmed both new mechanisms are actually exercised in a real
academy1 playthrough (temporarily instrumented `VK_ComputeGhoul2Pose` to
count bones with an active blend or nonzero `backlerp` per draw, then
removed it before committing): blending is active on every bone
immediately after an animation change and fades out over the real
350ms window, and sub-frame `backlerp` is nonzero on essentially every
draw once no blend is in progress, exactly as expected for a 16ms
simulated timestep against animations with a real per-frame duration well
above that.

**What this does and doesn't change about the `diff.py` comparison numbers**:
a single static screenshot's mean-pixel-difference figure barely moved
(~17.1% before and after this work, on `sp_academy1_spawn`) - and that's
expected, not a sign the work didn't matter. Both of these features are
fundamentally about *how a pose is reached from millisecond to millisecond*
(temporal smoothness, and tracking rd-vanilla's continuous frame position
instead of snapping to the nearest whole frame), which a single freeze-frame
comparison is structurally unable to fully credit - most of it only shows up
across a sequence of frames (video), or at the specific instant a blend is
caught mid-transition. What a single-scene diff *can't* measure isn't the
same as "no improvement": this renderer's per-instant frame position now
tracks rd-vanilla's real, continuous one far more closely than a
discrete-frame-only implementation ever could, for exactly the reason
`G2_TimingModel` exists in the first place.

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

**The stale-build-artifact mixup (item 1 above) recurred once, months
later, in the local fog/ranged fog work - still worth automating, not just
noting.** Deleting the one stale copy fixed that specific instance, but
didn't remove the actual hazard: two paths that both look like plausible
build output (`build-vulkan/rd-vulkan_x86_64.so` at the build root, vs.
`build-vulkan/code/rd-vulkan/rd-vulkan_x86_64.so`, CMake's real default for
a target defined in a subdirectory) will keep coexisting for as long as
anything - an old manual `cp`, an editor's "copy path" action, muscle
memory from a different project's layout - ever puts a file at the wrong
one, and nothing about the directory structure itself prevents that. It
happened again: a top-level copy reappeared, and a whole session's worth of
"before"/"after" fog-fix comparisons silently diffed that one stale file
against itself (a suspicious pixel-perfect 0.0% match across every scene,
including ones the fix should have changed, was the tell) before being
caught and re-run correctly. Structurally fixed now, not just re-deleted
again: `tests/render-regression/CMakeLists.txt`'s staging target copies
every binary via CMake's own `$<TARGET_FILE:...>` generator expression -
"wherever this target's build actually produced its output," resolved by
CMake itself, not typed by a person - so this specific class of mistake
can't recur a third time. See `tests/render-regression/README.md`'s
"Recommended: CMake targets" section.

## Character animation investigation: eight real bugs, and a wrong
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
`fixedtime` vanilla capture. It is still not a pixel `MATCH`, and at the
time this paragraph was written, individual poses still didn't line up
frame-for-frame with vanilla's - the single-track, no-blend,
no-sub-frame-interpolation simplifications documented in "Live animation"
above were all still real and still there.

**Update, immediately following: the per-bone-track separation, blending,
and sub-frame interpolation gaps referenced above are now closed** - see
"Full `G2_TimingModel` port: blending and sub-frame interpolation" further
below, done specifically because a user pointed out that "adequate
replacement" quality means matching rd-vanilla's animation *system*, not
settling for a visibly cruder approximation of it. What's left is
reproducing rd-vanilla's real bone-angle-override/ragdoll/IK layer (a
separate, still-real scope cut, and one no test scene in this suite
currently exercises) and closing the remaining, much smaller diff gap that
comes from cumulative ICARUS/camera-cut timing drift and the
already-documented `.shader`-script rendering gap - not from the animation
math itself anymore.

**Bug 5 (test harness, found by a third round of user pushback, on the same
specific character): `wait_frames` still wasn't enough for a scripted
scene, even with the `fixedtime` cvar name fixed.** Asked to check again
after the `G2_TimingModel` port above still didn't make one particular
NPC's pose (academy1's lone character standing on the raised platform
between the entrance pillars) match rd-vanilla's, direct comparison at
matched crops showed a genuinely different pose - not a subtle drift, an
entirely different animation clip. Rather than keep guessing from pixels,
this was traced by temporarily instrumenting *both* renderers identically
(a debug build of each, since `rd-vanilla` is just as much a part of this
repository as `rd-vulkan` is) to print that exact NPC's real internal
animation-bone state - matched across builds by its world-space origin,
confirmed identical between them - side by side. That showed the real
problem directly: at the identical `wait_frames: 300`, this renderer
reached simulated time **t≈7.7s** while rd-vanilla reached **t≈11.2s** -
a **3.4 second gap**, despite `fixedtime` being set and correctly named in
both. `wait <N>` (`Cmd_Wait_f`, `qcommon/cmd.cpp`) counts real
`Cbuf_Execute` calls (one per engine frame) - `fixedtime` makes each of
*those* frames advance simulated time by a fixed, deterministic amount,
but it does nothing about *how many* such frames `devmap`'s own map load
consumes before `wait <N>` even starts counting, and that number is itself
real-time-dependent and renderer-speed-dependent (loading-screen ticks,
precache retries, whatever the exact mechanism on a given build - not
fully root-caused, since the fix below doesn't require knowing it). Two
renderer builds that take a different number of frames to get through
loading land at genuinely different absolute simulated times by the time
`wait <N>` finishes, even with `fixedtime` working exactly as designed -
not a rendering bug in either renderer, a comparison of two different
moments in the same script.

Fixed at the engine level, in `qcommon/`, so it benefits every renderer
(not something rd-vulkan-specific): a new `com_fixedSimTime` global
(`common.cpp`) that accumulates only `Com_Frame`'s own real,
`fixedtime`-forced per-frame `msec` - nothing else ever touches it - and a
new `waittime <ms>` console command (`cmd.cpp`, implemented as a
`waituntiltime <absoluteMs>` continuation that re-queues itself via
`Cbuf_InsertText` once a frame, mirroring `Cmd_Wait_f`'s own `cmd_wait`
mechanism) that waits until `com_fixedSimTime` reaches `com_fixedSimTime +
ms`, computed at the moment the command runs - not a fixed frame count set
in advance. Because the target is computed *from wherever
`com_fixedSimTime` already is*, it's self-correcting regardless of how
much (or little) simulated time anything earlier in the script - a slow
map load included - already consumed: two renderer builds converge on the
same absolute simulated time by construction, not by coincidence.
`tests/render-regression/capture.py`/`scenes.json` now use this
(`wait_ms` instead of `wait_frames`) for every scene with a `map`. Verified
directly: re-running the same two builds with `waittime 8000` instead of
an equivalent `wait <N>`, both landed within **6ms** of each other - down
from the 3.4 second gap - confirmed via the same side-by-side internal
animation-state comparison, not just screenshots.

**What this does and doesn't resolve**: with simulated time now actually
matched, this specific NPC's animation-track *timing* (`startFrame`/
`endFrame`/current position) is directly comparable between the two
renderers for the first time - and doing so surfaced a *further*, different
residual gap: even at matched simulated time, the two builds have this NPC
on two *different* animation clips entirely (a real 40-frame gesture in
rd-vanilla vs. a short idle micro-loop here). Since game logic
(`bg_panimate.cpp`/NPC AI, `code/game`) is identical between the two -
same `.so`, same simulated time now confirmed - this points at some form
of state divergence upstream of animation selection. The harness timing
bug this section describes is fixed and verified on its own terms (matched
simulated time, confirmed independently of any specific NPC's animation
content); whatever is choosing a different animation for this NPC despite
that is chased down in Bug 6 below.

**Bug 6 (functional, in this renderer, found by chasing the residual gap
Bug 5 surfaced): `G2API_PrecacheGhoul2Model` was a hardcoded
`return 0;` stub.** The leading hypothesis going into this - some form of
RNG-consumption divergence during this renderer's own asset precache,
since `Q_irand`'s state (`shared/qcommon/q_math.c`'s static `holdrand`) is
private to whichever `.so` calls it and can't actually cross between the
game module and this renderer - turned out to be a dead end on inspection
(game.so and this renderer have independent, non-interacting RNG state by
construction; there was never a plausible mechanism for one to perturb the
other). The real cause was more mundane and fully deterministic:
`NPC_stats.cpp`'s `G_ParseAnimFileSet` - the only real caller of
`G2API_PrecacheGhoul2Model` - uses it to register a level's "cinematic"
per-map animation `.gla` (e.g.
`models/players/_humanoid/_humanoid_academy1.gla` - a map-specific set of
*extra* animations used only by that map's scripted cutscenes) *in
addition to* the standard `_humanoid.gla` every humanoid model already
uses, and gates loading that entire cinematic animation set behind this
call's return value being truthy
(`if (cineGLAIndex) { G_ParseAnimationFile(1, ...); ... }`). A permanent
`return 0;` meant that branch was dead code for every map, for the entire
history of this renderer - not "this specific NPC's animation is missing,"
every map-specific cutscene animation on every map, silently replaced by
whatever this renderer's per-bone track happened to resolve to instead
(in this case, a coincidental-looking but meaningless 2-frame slice deep in
`_humanoid.gla`'s own shared frame pool).

Fixed with a real implementation (`tr_init.cpp`): a dedicated ordinal
handle cache (`s_precachedModelHandles`) separate from both
`RE_RegisterModel`'s existing hardcoded-`1` stub (see its own comment -
reusing it would have made every precache call collide on the same fake
handle) and `VK_LoadGhoul2Skeleton`'s real per-skeleton cache (a different
handle space serving a different purpose - actually resolving a skeleton
for rendering, not this ordinal "did it register, and is the second one
exactly the first one's handle plus one" check
`G_ParseAnimFileSet` depends on). Checks real file existence via
`ri.FS_ReadFile` before minting a handle, so a map that genuinely has no
cinematic `.gla` (most maps) still correctly gets `0` back, not a false
truthy value.

**Verified precisely, not just visually**: re-running the same
side-by-side internal-animation-state comparison this whole investigation
has relied on, right at spawn (`t=1600`, before this NPC's gesture has had
time to loop even once), this renderer now reports the *exact same*
`startFrame`/`endFrame` (`383`/`423`) as rd-vanilla - previously `14651`/
`14653`, a completely unrelated slice of the frame pool. At the scene's
actual default capture point (`wait_ms: 4800`, roughly a loop and a half
into this 40-frame `BONE_ANIM_OVERRIDE_LOOP` gesture), the two renderers
now visibly differ only in *which point in the same loop* they're
showing - a real, but far smaller and better-understood residual, plausibly
just the ~6ms figure Bug 5 already measured accumulating slightly further
over a couple of loop cycles - not a different animation, and not
something this write-up chases further. `diff.py`'s
`sp_academy1_spawn` mean-pixel-difference figure barely moved (~17.2%,
statistically the same as before this fix) precisely because of that: a
correctly-selected but different-phase-of-the-same-loop pose isn't
something a single static screenshot's pixel-difference metric was ever
going to credit clearly, the same limitation already noted for the
`G2_TimingModel` port above. Full SP scene suite
(menu/academy1/hoth2/yavin1/vjun1) re-verified clean afterward: no
crashes, no new warnings, on both renderers.

**One thing this investigation explicitly ruled out**: `sp_yavin1_spawn`'s
pre-existing ICARUS crash (documented above) was hypothesized to be
possibly related to a wrong G2API animation-completion signal - it isn't,
or at least not *this* one. Re-tested after the `G2API_GetBoneIndex`/
per-bone fix above with the real `fixedtime` cvar: identical crash, same
assertion, same line. That hypothesis is now closed; the real cause is
still unknown and still out of scope for this write-up.

**A permanent debug tool, added on user request rather than another
one-off temporary print**: comparing screenshots alone couldn't settle
whether a visible pose difference was a genuine animation bug or just an
unrelated camera-timing artifact (both this write-up and its readers kept
having to re-derive the answer from scratch each time). `r_ghoul2animdebug`
(new cvar, flags `0` - **not** `CVAR_CHEAT`, see below) turns on a `G2ANIM`
console/log line per tracked bone, on both renderers, in the same format:
`G2ANIM t=<currentTime> pos=(x,y,z) bone=<name> start=<f> end=<f>
cur=<f.ff> speed=<f.f> flags=0x<hex>` - entity world position (this
project's established cross-renderer/cross-run NPC-matching key, since
spawn positions are deterministic - see the harness-timing section above)
plus that bone's real current animation state, throttled to once per 200ms
per bone so it stays readable over a multi-second capture instead of
flooding one line per frame. `rd-vanilla`'s version
(`R_DebugLogGhoul2Anim`, `tr_ghoul2.cpp`) walks the real
`boneInfo_v`/`G2_Get_Bone_Anim_Index` per-bone list and resolves bone names
from the `.gla`'s `mdxaSkelOffsets_t` table; this renderer's version
(`VK_DebugLogGhoul2Anim`, `tr_model.cpp`) walks its own
`s_ghoul2AnimState` per-instance-per-bone map and resolves names from the
already-parsed `VulkanSkeleton::bones` array - different data structures,
identical log line shape, so a `grep`+`diff` across both renderers' logs
for the same NPC's world position is directly meaningful. Both are true
no-ops with the cvar at its default `"0"` (verified: identical log output
and the full SP scene suite still clean on both renderers with it unset).

**Bug 7 (functional, in this renderer, found while first trying to use the
new debug tool): `r_ghoul2animdebug` was registered `CVAR_CHEAT`, so
`+set r_ghoul2animdebug 1` silently did nothing.** Both renderers'
`ri.Cvar_Get( "r_ghoul2animdebug", "0", CVAR_CHEAT )` calls happen during
early engine bring-up (`Com_Init`), before the command buffer's `+set`
arguments are processed - confirmed by reading `Cvar_Set2`/`Cvar_Get`
directly (`qcommon/cvar.cpp`): `Cvar_Set2` (what `+set` goes through)
silently rejects any value change on a `CVAR_CHEAT` cvar while
`cvar_cheats->integer` is `0`, which it still was at the point `+set`
ran, regardless of whether `+set r_ghoul2animdebug 1` was placed before or
after `+devmap` on the command line (`devmap` enables cheats, but only
once it actually runs, after `+set` arguments already tried and failed).
This renderer's own established convention for this exact category of
debug cvar - `r_Ghoul2NoLerp`/`r_Ghoul2NoBlend`
(`ri.Cvar_Get( "r_ghoul2nolerp", "0", 0 )`, no `CVAR_CHEAT`) - was right
there to copy and wasn't; fixed by registering with flags `0` in both
`code/rd-vulkan/tr_init.cpp` and `code/rd-vanilla/tr_init.cpp`, confirmed
working (`G2ANIM` lines now actually appear with `+set r_ghoul2animdebug 1`
on either side of `+devmap`).

**Using the new tool on the exact NPC this section opened with** (the
pillar-framed close-up shot, world position `(-1408,-1304,744)`, the one a
user directly pointed at as still visibly wrong after every fix up to and
including Bug 6): captured `G2ANIM` lines from both renderers across
several seconds of simulated time (`waittime` 400 through 4800ms, same
technique as the harness). Result: **the animation-clip data has been
fully fixed by Bug 6 and stayed fixed** - both renderers report the exact
same `start=15449 end=15451 cur=<matching value>` for this NPC's
`model_root`/`Motion`/`lower_lumbar` bones at every sampled timestamp, and
both transition to the exact same next clip (`start=18 end=42 cur=21`) at
the exact same simulated time. This is not a coincidence or a loose
approximation - it is bit-for-bit the same frame data, frame for frame,
confirming this specific residual concern from Bug 6's write-up is fully
resolved, not just "close."

**Yet the screenshots at that exact matched simulated time still show a
real, visible difference**: `rd-vanilla` shows the NPC face-on, framed
between the two pillars (the shot this whole section is named for);
this renderer shows the same NPC in side profile, same pillars, same
lighting, at the identical `t`. Since the bone *animation* data is now
proven identical, this has to be a *pose/orientation*, not a timing,
difference - a different kind of bug than anything else in this section.
Chased as far as this session's evidence trail goes, all of it via direct
numeric comparison, not visual guessing:

- The `refdef_t` this renderer's `RE_RenderScene` receives (camera
  `vieworg`/`viewaxis`/`fov`) is **byte-identical** to what `rd-vanilla`
  receives at the same `t` (confirmed via matched temporary prints in both
  `RE_RenderScene`s) - cgame's camera computation is shared code and isn't
  the problem.
- Hand-multiplying this renderer's own logged `mvp` matrix against the
  NPC's world-space origin lands it almost exactly screen-centered
  (`x_ndc≈0.03`) - the shared view/projection math is correct for this
  shot; the camera isn't pointed the wrong way and isn't misprojecting.
- `ent.axis` for this NPC (its facing, from shared cgame code, unaffected
  by which renderer is loaded) is also byte-identical between the two
  renderers at this `t` - `axis0=(0,1,0)`, i.e. facing world `+Y`, which
  *is* consistent with the front-on shot `rd-vanilla` actually shows
  (camera looks toward `-Y` from the `+Y` side, straight at the NPC's
  front). The entity-level facing this renderer is told to use is correct.
- `VK_ComputeGhoul2Pose`'s root-bone matrix for this NPC, at this `t`, has
  **no rotation component at all** (identity rotation, matching the
  `.gla`'s base pose rotation exactly, just a translation) - so the visible
  90-degree-ish discrepancy isn't coming from a root-bone orientation bug
  either.

In short: camera, shared entity-level facing, and this renderer's own
*mathematical* root-bone identity were all independently confirmed correct
and identical to `rd-vanilla`. That result was real but incomplete - it
proved this renderer's root bone had no rotation of its own, not that no
rotation was *supposed* to be there.

**Bug 8 (functional, in this renderer, and the actual cause of the pillar
NPC's orientation): `VK_ComputeGhoul2BoneRecursive` seeded every root
bone's hierarchy walk with a true mathematical identity matrix, when
`rd-vanilla` always seeds it with a fixed 90-degree rotation instead.**
Found by extending the `G2ANIM` debug tool (above) into a direct
world-space bone-matrix dump for this NPC on both renderers
(`VK_ComputeGhoul2Pose`'s output composed with `ent.axis`/`ent.origin` on
this side; `EvalBoneCache` on `rd-vanilla`'s side, once its
`CBoneCache` is populated by `G2_TransformGhoulBones`) - the *rotation*
submatrices matched exactly once composed with each renderer's own root
seed, which is what made it obvious the seeds themselves had to differ.
Reading `rd-vanilla`'s real `RootMatrix()` (`tr_ghoul2.cpp`) confirmed it:
for the ordinary case (no `GHOUL2_NEWORIGIN` reparenting), it returns a
file-scope constant named - misleadingly - `identityMatrix`:

```cpp
const static mdxaBone_t identityMatrix =
{
	{
		{ 0.0f, -1.0f, 0.0f, 0.0f },
		{ 1.0f, 0.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f, 0.0f }
	}
};
```

That is **not** the mathematical identity - it is a fixed 90-degree
rotation, and every single Ghoul2 model in the real game is skinned
relative to it, unconditionally, before the entity's own `ent.axis` is
ever applied on top. It is the fixed remap between the `.gla`/`.glm`
asset convention and the engine's world convention, the same category of
thing as this renderer's own fixed camera-convention "flip" constant in
`VK_BuildViewMatrix` (`tr_world.cpp`) - a real, load-bearing part of the
pipeline with a name that only describes what it *isn't*. This renderer's
`VK_ComputeGhoul2BoneRecursive` used an actual identity for the same seed,
which is indistinguishable from correct for a bone that happens to have no
local rotation of its own, and produces exactly this NPC's 90-degree-off
orientation for one that does. It doesn't matter whether the specific
bone or clip playing has "extra" rotation in it - **every** Ghoul2 model
on **every** map was missing this fixed rotation; it simply wasn't visible
before because most camera angles tested up to this point happened not to
make a 90-degree body-facing error look dramatically wrong the way a
tight, symmetric, pillar-framed portrait shot does.

Fixed in `VK_ComputeGhoul2BoneRecursive`'s `parent < 0` (root bone) case:
instead of `outBones[boneIndex] = delta;`, it now composes
`delta` with this same fixed rotation constant via the existing
`VK_Multiply3x4Matrix`, exactly mirroring `G2_TransformGhoulBones`'s own
`rootMatrix` seeding. **Verified**: the pillar shot now matches
`rd-vanilla` face-on, pillars-framing-symmetric, at the identical matched
`t` - not just "closer," the same shot. Full SP scene suite
(menu/academy1/hoth2/yavin1/vjun1) re-verified clean on both renderers
afterward (no crashes, no new warnings), and a visual spot-check of
`sp_hoth2_spawn` (a completely different model, a completely different
`ent.axis`, viewed from behind rather than in front) confirms the fix
generalizes rather than being tuned to this one shot - the character's
pose there is unchanged and still correctly matches `rd-vanilla`'s own
from-behind framing. The remaining pixel difference on the pillar shot
(`diff.py` still reports `MAJOR_DIFF`) is lighting/shading only - no
dynamic lights, a scope cut documented since early in this file - not
orientation; the two screenshots show the same pose from the same angle.

One thing this round of investigation improved regardless of whether it
explains the above: `G2API_GetBoltMatrix` (Ghoul2 rendering section,
further below) was using this NPC's *bind pose* for a bolted bone's world
matrix, not its actual current animated pose - a real bug in its own
right, benign for a stationary idle but wrong in general for any bolt on
a moving bone (e.g. a cutscene camera tracking a tag on an animating NPC).
Fixed by routing it through a new `VK_GetGhoul2BoneCurrentPoseMat`
(reuses `VK_ComputeGhoul2Pose`, the same per-frame hierarchy walk skinning
already runs) instead of the old bind-pose-only
`VK_GetGhoul2BoneBasePoseMat`. Verified this doesn't regress anything
(full SP scene suite still clean, no crashes, on both renderers) but did
**not**, on its own, change the pillar-shot screenshot above - confirming
the root cause of that specific discrepancy is elsewhere, as the bullet
list above already independently established.

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

## Overbright brightness bug (Ghoul2 characters and any real sky texture)

A user directly reported that Vulkan screenshots read "much brighter" than
`rd-vanilla`'s, across scenes with no obvious connection to the character-
animation investigation above - the wrong pose was one bug, this was a
completely separate one, and both happened to be visible in the same
academy1 pillar screenshot without either explaining the other.

`world.frag`'s `diffuse.rgb * lightmap.rgb * 2.0` approximates Quake3's
"overbright bits" - real BSP lightmaps are baked assuming the renderer
doubles them back out at draw time, so multiplying by 2.0 is the right call
*for real, baked lightmap data*. The bug: this renderer reuses that same
shader and that same unconditional `*2.0` for two other things that are
*not* real overbright-compensated lightmap data, each paired with a plain
white placeholder in the lightmap texture slot purely to reuse the shared
descriptor-set/shader plumbing:

- **Ghoul2 characters** (`VK_BuildWorldDescriptorSet(..., vk.whiteImage)`,
  `tr_model.cpp`) - white(1,1,1) * diffuse * 2.0 silently rendered every
  character exactly twice as bright as its own diffuse texture. This
  renderer has no real per-character lighting yet (no dynamic lights, see
  below) - that's a documented, deliberate scope cut - but doubling isn't
  "unlit," it's wrong in a specific, fixable direction.
- **Sky faces, including real farbox textures** (`VK_LoadSky`,
  `tr_world.cpp`) - confirmed by reading `rd-vanilla`'s own real sky code
  (`DrawSkyBox`, `tr_sky.cpp`): it sets a flat `qglColor3f(tr.identityLight,
  ...)` vertex colour specifically to *cancel* overbright for sky faces
  (`tr.identityLight = 1.0 / (1 << tr.overbrightBits)`, the exact inverse of
  the doubling), so a real farbox texture displays at its own natural
  brightness. This renderer's sky previously got the same undeserved `*2.0`
  as real lightmapped world geometry, which clips any map's actual sky
  texture toward solid white - confirmed on academy1's real
  `textures/skies/yavin` farbox, which rendered as a blown-out white sky
  before this fix and a correct blue-and-cloud gradient after it. An
  earlier fallback-sky fix (see "Bugs found and fixed" above this section)
  had already noticed the fallback box's own colour needed pre-halving to
  avoid clipping to white under this `*2.0` - a working band-aid for a
  synthetic solid-colour fallback, but one that left the underlying wrong
  convention in place for every map with a *real* sky texture.

**Fixed**: `vkWorldPushConstants_t` gains a per-draw overbright factor,
carried in `camPos.w` (previously documented as unused - see
`vkWorldPushConstants_t`'s own comment, `tr_local.h`, and `world.vert`/
`world.frag`'s). Real BSP-lightmapped world geometry keeps `2.0`; Ghoul2
draws and sky draws (both real farbox and the flat fallback) now pass
`1.0`. The fallback sky colour's earlier pre-halving is undone to match
(`(70,75,80)` → `(140,150,160)`, its originally-intended on-screen value).

**Verified**: side-by-side academy1 comparison at matched simulated time -
the pillar NPC's clothing/skin tone now closely matches `rd-vanilla`'s own
(previously visibly lighter/flatter), and the sky through both window
openings shows the same blue-gradient-with-clouds look instead of solid
white. `sp_hoth2_spawn`'s player-character close-up, which the "Investigate
hoth2 white/black anomaly" checkpoint (task list) had already flagged as
looking washed-out/near-white, now shows the correct olive-green skin tone
matching `rd-vanilla` - the same Ghoul2 overbright bug, not a separate
hoth2-specific anomaly as originally suspected. `sp_yavin1_spawn` and
`sp_vjun1_spawn` spot-checked clean too (natural clothing/skin tones, no
blown-out sky). Full SP scene suite re-verified clean on both renderers
afterward (no crashes, no new warnings). Real per-character ambient/
directional lighting is still not implemented (unchanged scope cut, see
below) - this fix only removes an incorrect doubling, it doesn't add the
missing feature.

## hoth2's missing terrain (three bugs, one real fix and two real limits)

A user asked specifically about `sp_hoth2_spawn`'s wide, `waittime`-matched
shot: the player character's torso read as flat grey and "the rest is just
grey" too - not the overbright/skin-tone issue above (already fixed by
then), a second, unrelated problem where almost the entire screen showed a
single flat colour instead of any snow terrain at all.

**First ruled out, with actual evidence, not assumption**: temporarily
forcing `VK_AABBOutsideFrustum` to never cull produced a *pixel-identical*
screenshot to culling left on - the same flat grey, same silhouette. If
over-aggressive frustum culling had been dropping visible terrain, disabling
it entirely would have to change something. It didn't, so the real cause
had to be somewhere earlier: either the terrain never made it into
`s_worldSurfaces` in the first place, or it was being submitted but
rendering as the same colour as everything else around it. Sampling the
screenshot's raw pixels confirmed the latter half of that isn't quite right
either - the colour was **exactly** `(140,150,160)`, this renderer's own sky
fallback constant, uniformly across the entire frame, which only makes
sense if no world-geometry fragment was reaching the screen at all (a
partially-fogged or wrongly-lit terrain would still show *some* pixel
variation).

**Bug (real, fixed): `RE_LoadWorldMap`'s opaque-surface loop treated "shader
name doesn't resolve to a texture file" as "this must be a stages-only
effect shader, skip it" - true for some shaders, false for hoth2's actual
terrain.** A one-line instrumentation pass (counting *why* each of hoth2's
7421 raw BSP surfaces was or wasn't added to `s_worldSurfaces`) found
`skipNoImage=3024` - **41% of the entire map**, not a handful of decorative
effects. The first few skipped shader names, e.g.
`textures/hoth/metal_lg_lt_vertex`, led straight to the actual cause on
reading `shaders/vertex.shader`:

```
textures/hoth/metal_lg_lt_vertex
{
	qer_editorimage	textures/impgarrison/metal_lg_lt
	q3map_nolightmap
	q3map_nonplanar
    {
        map textures/impgarrison/metal_lg_lt
        rgbGen exactVertex
    }
}
```

This shader's real base texture is `textures/impgarrison/metal_lg_lt` - a
name that shares nothing with the shader's own name
(`textures/hoth/metal_lg_lt_vertex`), a `_vertex`-suffixed variant that
reuses an *existing* texture from a completely different directory purely
to get `rgbGen exactVertex` (per-vertex lighting) instead of a lightmap.
`VK_FindImage(shaders[surf.shaderNum].shader)` only ever tries the shader's
own name as a file path - correct for the common case where they match, but
silently wrong for any shader (not just effect/fog shaders) that legitimately
has a real, different-named base texture. This is exactly the kind of
authoring pattern outdoor Quake3-engine terrain uses constantly (vertex-lit
variants, terrain-blend variants, ...) and academy1's mostly-indoor,
mostly-conventionally-named architecture almost never does - which is why
this went unnoticed through every earlier checkpoint that verified academy1/
yavin1/vjun1 but only ever glanced at hoth2.

Fixed by extending the existing minimal `.shader` parser
(`tr_shader.cpp`, previously only tracking blend mode and `fogparms`) to
also record each shader's first stage's `map`/`clampmap` argument
(`VK_GetShaderMapImage`, skipping `$whiteimage`/`$lightmap`-style generated
references, which have no file to find). `RE_LoadWorldMap` now retries
`VK_FindImage` against that recorded path when the shader's own name fails
to resolve - but **only when the shader's own recorded blend mode is
`BLEND_OPAQUE`** (no `blendFunc` keyword in its first stage). That guard
isn't optional: an earlier, ungated version of this fix made academy1
regress hard, resolving `textures/common/dark_dust`'s real `clampmap
textures/common/gradient` and drawing it *opaque* across a huge portion of
the screen - `dark_dust` has `blendFunc GL_ONE GL_ONE` (additive) and is
meant to be a translucent haze effect, exactly the "no blend pipeline for
world geometry yet, so drawing this opaque is actively wrong" case the
original code's own comment already warned about for shaders with *no*
`map` at all. A real shader legitimately having a `map` doesn't make it any
safer to force-draw opaque if it was never meant to be opaque. With the
`BLEND_OPAQUE` gate, only genuinely-opaque-but-differently-named shaders
(hoth2's terrain) take the new fallback path; genuinely translucent effect
shaders (academy1's dust, and hoth2's own presumably) still correctly fall
through to "skip this surface."

**Two more real, if smaller, limits found and fixed alongside it**:

- **`VK_BuildProjectionMatrix`'s fixed `zFar` was 4096 units** - "generous"
  only relative to academy1's compact indoor courtyard, the scene it was
  originally tuned against. hoth2's BSP bounds span roughly 25400 units
  corner-to-corner; any camera more than 4096 units from visible terrain -
  the ordinary case for most of an open snow field, not a corner case - had
  that terrain silently far-plane-clipped every frame. Confirmed directly by
  temporarily raising the constant and watching previously-invisible
  terrain and distant structures appear. Raised to 65536, comfortably
  covering hoth2's actual measured worst case with margin, still far short
  of causing obviously bad depth-buffer precision loss for near geometry.
  Still a fixed constant, not rd-vanilla's real per-frame computed `zFar`
  from actual visibility bounds (see "What's not implemented yet" below) -
  a bigger, unrelated undertaking this fix doesn't attempt.
- View-frustum culling itself was directly verified correct in the course
  of ruling it out above (see "First ruled out" - forcing zero culling
  changed nothing), not merely assumed innocent because the other two fixes
  worked without touching it.

**Verified**: `sp_hoth2_spawn` now shows visible snow terrain filling the
frame's lower half plus distant terrain/structure silhouettes, closely
matching `rd-vanilla`'s own composition for the same shot - not just "some
grey terrain now," the actual shape of the ground and horizon line. Full SP
scene suite re-verified clean on both renderers after each of the three
changes, including specifically re-checking `sp_academy1_spawn` after the
`BLEND_OPAQUE` gate was added (it's the scene that caught the regression the
gate fixes, so it's the one most likely to reveal if the gate itself is
wrong).

**Left for another day at the time, since fixed** - see "vjun1's missing
cockpit and NPC torso" below: `sp_vjun1_spawn`'s cockpit-interior shot was
missing its cabin walls/ceiling/dashboard geometry the same way hoth2's
terrain was, plus a real orange rendering artifact behind the Twi'lek NPC in
that same shot. Neither turned out to be the same shader-name-resolution
category guessed here - the cockpit is a static (non-Ghoul2, non-BSP) model
this renderer never drew at all, and the orange artifact was never a bug in
the first place.

## vjun1's missing cockpit and NPC torso (two unrelated real bugs)

`sp_vjun1_spawn`'s opening cutscene puts the camera inside a ship cockpit
(Kyle and a Twi'lek NPC seated at the controls). Before this pass it
rendered as a flat sky-grey background, a blocky orange silhouette, and the
two NPCs seemingly floating in open air - no cabin at all. A user directly
compared it against `rd-vanilla`'s full, detailed cockpit interior and asked
for this fixed.

**First ruled out - an ICARUS-timing camera-cut divergence, not a rendering
bug at all**: this project's history has at least one earlier case where
`rd-vanilla` and `rd-vulkan` ended up rendering genuinely *different*
scripted camera cuts at the harness's fixed `wait_ms` capture point, purely
from a per-renderer animation-completion-signal timing difference - not a
rendering bug at all. Directly tested that possibility here rather than
assuming it: added a temporary print of `fd->vieworg`/`fd->viewaxis[0]` to
both renderers' `RE_RenderScene`, ran `sp_vjun1_spawn` on both, and compared
the *last* line each logged before its screenshot. Both landed on the exact
same position and orientation (`org=(-5173.0,8230.0,1787.0)
axis0=(0.999,0.000,0.052)`) - conclusively the same camera, so whatever was
different was a real rendering gap, not a script-timing race.

**Bug 1 - the cockpit is a static model, not Ghoul2 and not world BSP
geometry, and this renderer only ever drew the first two**: parsed vjun1's
BSP directly (LUMP_SURFACES/LUMP_DRAWVERTS) to find every real world surface
within any plausible distance of that exact camera position - the nearest
is ~1300 units away and is the level's sky shader. There is no wall, seat,
or dashboard brush anywhere near the camera in the map's own geometry, in
either renderer - so hoth2's "shader name doesn't match a texture file"
category was never going to be the answer here, however plausible it looked
from the outside. Cross-referencing the scene's actual `RE_AddRefEntityToScene`
entities against the camera position turned up a non-Ghoul2 `RT_MODEL`
entity 98 units away, registered as
`models/map_objects/cinematics/raven_cockpit.md3` - a real file, a real MD3.
`RE_RegisterModel` has always been a documented stub (see "Everything below
this point is 3D world/model rendering" in `tr_init.cpp`) that returns a
fake non-zero handle for every model regardless of name, specifically so
game code that treats a failed registration as fatal doesn't abort map
loading - and `VK_DrawGhoul2Entities`'s entity loop has always
unconditionally skipped any `RT_MODEL` entity without Ghoul2 data. Both are
correct, deliberate, and were re-confirmed with an earlier (now-reverted)
diagnostic pass in this same investigation that found the ~19 other
non-Ghoul2 entities scattered around vjun1 are all generic precached
props/gibs - not the cockpit. This one just happened to matter visibly,
because unlike a gib in a corner, it's the entire set piece the whole shot
takes place inside.

Fixed by actually implementing static MD3 loading rather than documenting
around it further: `VK_LoadMD3Model` (`tr_model.cpp`) parses a `.md3`
(single LOD - `ofsSurfaces` already points at LOD 0, MD3 LODs are whole
separate model chunks this renderer doesn't select between; single frame -
frame 0 of `XyzNormals` only, sufficient for a static set piece with no
vertex animation) into the same `WorldVertex` layout/pipeline/descriptor-set
shape Ghoul2 models already reuse (see this file's header comment), so
nothing new was needed on the GPU side. `RE_RegisterModel` now calls it for
any `.md3` name (matched via `COM_CompareExtension`, same real function
`rd-vanilla` uses) and, on success, returns the cache index offset by a new
`VK_STATIC_MODEL_HANDLE_BASE` constant (`tr_local.h`) - large enough that a
real index can never collide with the existing fake "1" handle every other
model kind still gets, since `ent.hModel` is a single shared field with no
other tag for which handle space it belongs to. A `.md3` that fails to
parse still falls through to the old fake handle rather than propagating
failure, preserving the exact "never abort map load over this" behavior the
stub existed for. `VK_DrawGhoul2Entities`'s entity loop now branches on
`ent.ghoul2` being empty to also handle a static-model entity, reusing the
identical entity-matrix/push-constant/world-pipeline draw shape the Ghoul2
branch already uses (`camPos[3]=1.0` - no baked lightmap here either, same
reasoning as Ghoul2's own overbright fix above), just against one shared,
static, device-local vertex/index buffer instead of a per-instance skin
slot, since nothing about a static prop changes per draw.

**Verified**: the cockpit interior - side walls, ceiling ribbing, dashboard
with its screens and buttons, both seats - now renders, closely matching
`rd-vanilla`'s composition for the same shot.

**Bug 2 - a real, previously-unverified parser bug in the .shader
`blendFunc` two-token path, discovered chasing a second, unrelated symptom
in the same shot**: fixing the cockpit revealed the Twi'lek NPC's torso was
still invisible (arms, hands, legs, head/hair all rendered correctly - only
the torso region was missing), a *different* bug from the cockpit and from
hoth2's original torso-shaped symptom. Traced it by instrumenting
`VK_LoadGhoul2Model`'s per-surface image resolution: her torso surfaces
correctly skin-override (via the real three-part composite skin path -
`models/players/jedi_tf/|head_a1|torso_a1|lower_a1`, `VK_RegisterSkin`,
verified working) to `models/players/jedi_tf/torso_01_clothes` and
`..._skin`, both of which are pure indirection `.shader` scripts
(`shaders/players.shader`: `{ map models/players/jedi_tf/torso_01
blendFunc GL_ONE GL_ZERO ... }`, no matching `.tga`/`.jpg`/`.png` of their
own) - exactly hoth2's category of bug, so the existing
`VK_GetShaderBlendMode(...) == BLEND_OPAQUE` gated fallback was applied here
too (`VK_LoadGhoul2Model`, mirroring `RE_LoadWorldMap`'s identical check).
It didn't work. Direct instrumentation of `VK_GetShaderBlendMode` itself
showed why: `models/players/jedi_tf/torso_01_clothes` was resolving to
`BLEND_ALPHA`, not `BLEND_OPAQUE`, despite its `.shader` block reading
`blendFunc GL_ONE GL_ZERO` in plain text - and so was *every other* explicit
two-token `blendFunc` in the entire file, including ones on surfaces that
happened to render fine anyway (because they had a real texture file
`VK_FindImage` could find directly, never needing this fallback at all).

The actual bug, in `ParseShaderFile` (`tr_shader.cpp`): `COM_ParseExt`
returns a pointer into its own single shared static buffer (`com_token`),
reused on every call - not a fresh allocation. The two-token `blendFunc`
handling called it once for the first factor (`a`) and, in the same
expression's evaluation, once more for the second (`b`), then passed both
to `BlendFactorsToMode( a, b )` - but by the time that call happened, the
second `COM_ParseExt` had already silently overwritten the buffer `a` still
pointed at, so the comparison was really `BlendFactorsToMode( b, b )`.
`GL_ZERO`+`GL_ZERO` matches neither the `GL_ONE`+`GL_ONE` (additive) nor
`GL_ONE`+`GL_ZERO` (opaque) special case, so it fell through to the
alpha-blend default every time - for any shader using this common
`blendFunc <src> <dst>` two-argument form, regardless of what `<src>`/`<dst>`
actually were. A shader with no `blendFunc` keyword at all (the
default-opaque path, never entering this branch) was correctly unaffected -
which is exactly why hoth2's earlier, similar-looking fix never exposed
this: `textures/hoth/metal_lg_lt_vertex` (the shader that motivated it) has
no `blendFunc` line at all. Fixed by copying the first token into a
`std::string` before parsing the second, so it survives the aliasing.

A second, smaller bug compounded this one for this specific case: the
`.skin` file's own override line reads
`torsoa,models/players/jedi_tf/torso_01_clothes.tga` - a `.tga` extension
tacked onto what is actually a `.shader` block name, not a texture path.
Real shader names never carry an extension, but `.skin` files are free to
write one as if it were a plain texture reference, and real engines account
for this: `rd-vanilla`'s actual `R_FindShader`/`R_FindShaderByName`
(`tr_shader.cpp`) unconditionally `COM_StripExtension` the incoming name
before comparing against defined shader names. This renderer's
`VK_GetShaderBlendMode`/`VK_GetShaderFogParms`/`VK_GetShaderMapImage` did
not, so they were being looked up by a name variant that never matched
anything the parser had recorded. Fixed by adding
`VK_StripShaderNameExtension` (a thin `COM_StripExtension` wrapper) and
routing all three lookups through it before their map `find()`.

**Verified**: the Twi'lek's torso now renders correctly in both
`sp_vjun1_spawn` and `sp_hoth2_spawn` (the same `models/players/jedi_tf`
model appears in both scenes - confirmed the fix isn't scene-specific).

**The orange artifact - confirmed not a bug at all**: already established
during the original hoth2-era investigation that it's real world BSP
geometry, not a Ghoul2 rendering defect (disabling all Ghoul2 drawing
entirely left it pixel-identical) - re-confirmed here rather than assumed.
With the cockpit's own geometry now actually drawn and correctly occluding
most of the background, what was previously a fullscreen wash is now a
small sliver visible only through the cockpit's own side window: real
distant terrain/rock texture (`textures/vjun/rocky_phong` and similar,
confirmed the closest real BSP surfaces to that exact camera position
besides the sky) showing through actual glass, the same way it would in any
renderer, not a rendering defect. Not chased further as a separate issue.

**Also retained in this same pass**: the `MAX_VK_WORLD_DESCRIPTOR_SETS` fix
described in the investigation above - independently correct per the Vulkan
spec (vjun1 alone needs 12833 world-surface-batch descriptor sets, far past
the old `MAX_VK_IMAGES`-sized pool), even though it turned out not to be
what was causing either of the two bugs actually found and fixed here.

**Verified overall**: full SP scene suite
(menu/academy1/hoth2/yavin1/vjun1) re-verified clean on both renderers after
every change in this pass, including re-checking academy1 (the scene that
previously caught the hoth2-era `BLEND_OPAQUE`-gate regression) and hoth2
itself (to confirm the Ghoul2 image-resolution fallback and the shader
extension-stripping fix don't reintroduce that regression from a different
angle) - no crashes, no new warnings, no regressions.

## hoth2's overexposed terrain (vertex-lit surfaces wrongly double-brightened)

A user directly compared `sp_hoth2_spawn`'s ground/sky against `rd-vanilla`
and reported things looking "either too bright or not rendered" - a
different symptom from this same level's earlier "missing terrain" bug
above (that one was about surfaces not appearing at all; this one is about
surfaces that *do* appear, but look wrong). The snow terrain filled most of
the frame as flat, textureless solid white - no visible bumps, footprints,
or any of the surface detail actually present in the source texture, and
nothing like `rd-vanilla`'s own textured, snow-coloured (not white-clipped)
ground for the same shot.

Traced to `RE_LoadWorldMap`'s handling of `dsurface_t.lightmapNum`: this
renderer already knew about surfaces with no real baked lightmap (comment
on `s_whiteLightmap`, tr_world.cpp) and correctly binds a plain white 1x1
image as a no-op stand-in for their missing lightmap slot - but every world
surface, regardless of whether it actually had a real lightmap, was drawn
through the same `camPos.w = 2.0` overbright factor (the multiply that
approximates Quake3's real "overbright bits" compensation, correct *only*
for genuine baked-and-compensated lightmaps - see the Ghoul2/sky overbright
fix above, which already made this exact distinction for Ghoul2 and sky,
but missed this third case). Confirmed directly against the real BSP data:
parsed hoth2.bsp's own `LUMP_SURFACES`, found every ground/terrain surface
near the camera position has `lightmapNum[0] == -3`
(`LIGHTMAP_BY_VERTEX` - rd-vanilla's own convention, `rd-vanilla/tr_local.h`)
- confirmed against `shaders/hoth.shader`'s real content for exactly these
surfaces: `q3map_nolightmap`, `q3map_onlyvertexlighting`, `rgbGen vertex` -
genuinely vertex-lit, no lightmap ever baked for them at all, real
`rd-vanilla` never overbright-doubles these either. Multiplying an
already-appropriately-bright diffuse texture by the white "lightmap" *and*
then by 2.0 doesn't leave it alone (a true no-op would) - it doubles it,
with nothing to compensate for since there's no real lightmap's darkening
to counteract. With no tone-mapping curve to compress the excess back down,
the result hard-clips to solid white, erasing the texture's own detail
entirely - exactly the flat, textureless ground reported.

Fixed by tracking which loaded case each surface batch actually is
(`WorldSurfaceBatch::vertexLit`, set from the same `lightmapNum < 0` check
already used to pick the white lightmap fallback) and picking the overbright
factor per-batch at draw time in `RE_RenderScene` - `1.0` (no doubling,
same value Ghoul2/sky already use and for the identical reason: no
baked-and-compensated lightmap) for vertex-lit batches, the existing `2.0`
default for real lightmapped ones. Re-issues the push constant only when a
batch's category actually differs from the previous one drawn, not
unconditionally per-batch, to avoid one push-constant call per surface
across a 7000+-batch level like hoth2.

**Was not a complete fix at the time**: this removed the incorrect
doubling but didn't add real per-vertex `rgbGen vertex` colour support -
`drawVert_t::color` (the actual baked ambient/directional colour Quake3 map
compilers write per vertex for exactly these surfaces) wasn't read or
applied anywhere yet; `WorldVertex` had no colour attribute at all. Since
fixed - see "Real per-vertex colour for vertex-lit surfaces" below, which
adds exactly the `WorldVertex`/`world.vert`/`world.frag` colour attribute
this paragraph originally called out as the needed follow-up.

**Also investigated, not a rendering bug**: two wampa creatures visible in
`rd-vanilla`'s screenshot are absent from `rd-vulkan`'s for this same scene.
Confirmed `models/players/wampa/model.glm` loads successfully (a real
skeleton and 11 drawable surfaces, same load-time log line every other
working Ghoul2 model gets) and this renderer's own per-frame debug log
(`VK_DrawGhoul2Entities`) shows every Ghoul2 entity actually queued in the
scene that frame drew successfully (3/3, no failures) - so if the wampa is
one of those three, it rendered somewhere, just not inside this particular
camera framing. Unlike the cutscene cameras and NPCs examined in earlier
sections of this document, a wampa here is a roaming, AI-controlled
creature with no scripted position - exactly the kind of entity most
exposed to this project's previously-established ICARUS/simulation-timing
divergence category (two renderers landing on a fixed capture point after a
slightly different number of effective simulation ticks), not a rendering
defect. Not chased further given no rendering-side failure was found.

**Also investigated, a known, unimplemented gap, not a new bug**:
`rd-vanilla`'s screenshot shows visible falling-snow particles and an
overall soft, hazy blizzard look across the whole frame (sky included);
`rd-vulkan`'s sky is a flat, sharply-bounded solid colour with no particle
effects at all. Confirmed hoth2 ships no real skybox texture files at all
under `textures/skies/hoth*` (checked every game .pk3 directly) - `hoth.shader`'s
own `skyParms textures/skies/hoth 512 -` names a basename nothing on disk
actually provides, so `rd-vanilla` itself isn't drawing a textured skybox
for this level either; whatever soft haze it shows is coming from its real
snow-weather particle system (`fx_weather`/`R_AddWeatherZone` in real
Quake3-derived engines) compositing over the sky and world alike - a whole
particle subsystem this renderer has never implemented (see "What's not
implemented yet" below), not something this pass's overbright/lightmap fix
could plausibly touch. Confirmed as a distinct, already-scoped-out gap
rather than investigated as a new bug. (Since fixed - see "World
weather/particle effects" below.)

**Verified**: `sp_hoth2_spawn`'s terrain now shows real texture
detail - visible surface variation, no longer a flat white wash - closely
matching `rd-vanilla`'s snow-coloured ground for the same shot. Full SP
scene suite (menu/academy1/hoth2/yavin1/vjun1) re-verified clean on both
renderers, including vjun1 and academy1 (both exercise the same world
overbright push-constant path this fix touches) to confirm no regression.

## World weather/particle effects (rain, snow, wind, blowing fog/sand/dust)

Following directly from the previous section's flat-sky/no-particles finding
(hoth2's blizzard haze coming from real Quake3's `fx_weather` system, not
its skybox), this pass ports that whole subsystem: rd-vanilla's real
`tr_WorldEffects.cpp`/`tr_WorldEffects.h` (2188/65 lines) - `COutside`
(inside/outside testing), `CWindZone` (gust/target-velocity wind), and
`CParticleCloud` (the actual billboard particle sim + rendering), all
driven by `R_WorldEffectCommand`'s preset table. New file:
`tr_weather.cpp`, ~1100 lines.

**Confirmed real, unmodified call path from map to screen**, not a new
mechanism invented for this renderer: `g_fx.cpp`'s `SP_CreateSnow`/
`SP_CreateWind`/`SP_CreateRain` (triggered by `fx_snow`/`fx_wind`/`fx_rain`
map entities) push a preset string into a configstring
(`G_FindConfigstringIndex(cmd, CS_WORLD_FX, ...)`); `cgame`'s
`CG_ConfigStringModified` relays it via `cgi_R_WorldEffectCommand` into
`re.WorldEffectCommand`, which this renderer's `R_WorldEffectCommand`
(tr_init.cpp) now forwards to `VK_WorldEffectCommand` instead of doing
nothing. Confirmed via direct BSP entity-lump parsing which of the four
test maps actually carry these entities: hoth2 has `fx_wind`
(speed 3000, angle 180, spawnflags 6) and `fx_snow` (no flags -> "snow" +
"fog"); vjun1 has `fx_rain` (spawnflags 8 -> "acidrain"); academy1 and
yavin1 have neither (only an unrelated `fx_runner` on yavin1) - so those
two are the built-in "must show nothing" regression cases for this feature.

**Call sites, matching rd-vanilla's real ones exactly** (checked by grep,
not assumed): `R_InitWorldEffects`/`R_ShutdownWorldEffects` run once at
renderer startup/shutdown (`R_Init`/`VK_Shutdown`), not per map load -
weather state persists across a map change unless a `"clear"` command says
otherwise, same as real Quake3. `RE_RenderWorldEffects` is called from
exactly one place in rd-vanilla, the very end of `RE_RenderScene`
(tr_scene.cpp:414) - mirrored here by calling the new `VK_DrawWeatherEffects`
right after `VK_DrawScenePolys` inside this renderer's own `RE_RenderScene`.

**Rendering** reuses this renderer's existing 2D-polygon machinery wholesale
- same `PolyVertex{pos,uv,color}` layout, same already-compiled
`vk.polyPipeline`/`polyPipelineAdditive`/`polyPipelineLayout` (depth-test-on,
depth-write-off; additive = `ONE,ONE` blend factors) - rather than standing
up a whole parallel pipeline for what is, at the vertex level, the same
kind of camera-facing textured quad. The one addition is a *separate*
`vk.weatherVertexBuffer`/`weatherVertexBufferMapped` scratch buffer
(`tr_local.h`), so this system's per-frame vertex writes can never collide
with `VK_DrawScenePolys`'s own write cursor into its own buffer. Billboards
are built from `fd->viewaxis[1]`/`[2]` (left/up), optionally 2D-rotated (the
tumbling-snow look) or replaced with a velocity-aligned axis for streaked
rain, exactly following `CParticleCloud::Render`'s real vertex math.

**Three deliberate simplifications**, each with a real reason, not
laziness:
- **No cached inside/outside grid.** Real `COutside` builds a per-map
  32-unit-cell grid, cached to disk, so a frame's worth of particles can be
  outside-tested without touching the collision system live. This port
  just calls `ri.CM_PointContents` directly per particle per frame -
  always exactly correct, just without the caching optimization. At the
  particle counts the real presets actually use (hundreds to low
  thousands - see `MAX_WEATHER_PARTICLES_PER_CLOUD`'s comment,
  `tr_local.h`) this hasn't been a measured cost on any test scene; the
  cache is the thing to add back if that ever changes, not a reason to
  guess instead of query.
- **`CONTENTS_INSIDE` vs `CONTENTS_OUTSIDE` convention, found and fixed by
  checking real map data, not by assumption.** Real `COutside::Cache`
  doesn't just test one fixed content bit - it scans the whole map first
  to see whether *this* map marks its (smaller) outdoor pockets as
  `CONTENTS_OUTSIDE` against an implicit indoor default, or marks its
  (smaller) indoor pockets as `CONTENTS_INSIDE` against an implicit
  outdoor default, and uses whichever convention it finds evidence of
  first. The first implementation here guessed the first convention
  (`contents & CONTENTS_OUTSIDE`) and it was wrong: parsed
  `LUMP_BRUSHES`/`LUMP_SHADERS` content flags directly for all four test
  maps - academy1 (0 `CONTENTS_OUTSIDE`, 0 `CONTENTS_INSIDE` brushes),
  yavin1 (0, 0), hoth2 (0, 16), vjun1 (0, 90). `CONTENTS_OUTSIDE` never
  appears in any of them; every map that uses either flag at all uses only
  `CONTENTS_INSIDE`, to mark a handful of interior volumes against an
  otherwise-outdoor map. With the original guess, `VK_WPointOutside` was
  permanently `false` everywhere on every test map, so every particle
  cloud populated (commands parsed fine, particle arrays allocated fine)
  but zero particles ever reached the `rendering` state - confirmed via
  temporary instrumentation showing 1000 snow + 60 fog particles created
  on hoth2, 0 ever visible. Fixed by testing
  `(contents & CONTENTS_INSIDE) == 0` (after excluding water/solid)
  instead - i.e. "outside" means "not solid/underwater and not inside an
  explicitly `CONTENTS_INSIDE`-flagged volume", matching what this
  checkout's real map data actually does. Rebuilt and reverified: hoth2's
  particle counts went from 0 rendering to ~289-314 (snow)/~23-27 (fog)
  per frame, with visible falling snow matching vanilla's look.
- **Wind-zone timing uses real elapsed milliseconds, not frame-tick
  counts.** Real `CWindZone::Update` decrements a countdown and ramps
  velocity by fixed per-*call* amounts, implicitly framerate-dependent
  since it's called once per rendered frame in the original engine. This
  port measures both in real elapsed ms/seconds instead - a
  framerate-independent equivalent of the same behaviour (gusts still
  retarget roughly every 1-4 real seconds, velocity ramps still smooth out
  over the same real time), not a different-looking result.

Every preset's exact tuning constants (particle counts, texture paths,
gravity, width/height, blend mode, colour/alpha, fade rate, mass range) for
`lightrain`/`rain`/`acidrain`/`heavyrain`/`snow`/`spacedust`/`sand`/`fog`/
`heavyrainfog`/`light_fog`/`wind`/`constantwind`/`gustingwind`/`windzone`
are copied verbatim from rd-vanilla's real `R_WorldEffectCommand` - these
are gameplay/visual tuning values, not something to approximate.
`WE_SetTempGlobalFogColor` (`g_fx.cpp`'s `fx_fog` entity, used for
on/off fog transitions independent of the weather clouds themselves) is
also wired to a real implementation now, not a stub - see "World fog is
now rendered" above for the base static fog this saves/restores around.

**Verified**: full SP scene suite (menu/academy1/hoth2/yavin1/vjun1)
re-ran clean on rd-vulkan with no crashes. hoth2 (`fx_wind`+`fx_snow`)
shows falling snow closely matching vanilla's look. vjun1 (`fx_rain` ->
acidrain, spawnflags 8) correctly shows **no** rain particles from inside
the ship's interior cockpit view - the `CONTENTS_INSIDE` gating working
as intended, gating the effect off exactly where a real player would
expect it to, not just everywhere-or-nowhere. academy1 and yavin1 (no
`fx_snow`/`fx_rain`/`fx_wind` entities) show zero particles and an
unchanged screenshot, confirming the built-in "must show nothing"
regression case actually holds.

**Not attempted**: `spacedust`'s and `sand`'s specific look wasn't
verified against a real map that actually uses them (none of the four test
maps carry those entities) - only that the presets parse and would behave
correctly per the ported physics, same trust level as any other
code-reading-based port in this project. `VK_AddWeatherZone` (the
`"zone (mins)(maxs)"` command) is a documented no-op: in real rd-vanilla
its only job is priming `COutside`'s cached disk grid for a given region
ahead of time, and this port has no such cache to prime - every "is this
point outside" query already goes straight to the live collision system
regardless, so there's nothing left for the command to do.

## Local fog volumes and ranged fog

Following up on "World fog is now rendered" above, which only read a map's
single *global* fog (BSP `LUMP_FOGS` entry with `brushNum == -1`) and
applied it uniformly to every world surface regardless of what that surface
actually compiled into - a real, if minor, inaccuracy: vjun1 ships a second,
*local* fog (`textures/fogs/fog_black`, bound to brush 6685, a small
interior region) that was being silently ignored, with every one of its
surfaces incorrectly painted with the map's own tan/brown exterior global
fog colour instead.

**Root cause and fix**: `dsurface_t.fogNum` (an existing field this
renderer had never read) already tells you, per-surface, exactly which
`LUMP_FOGS` entry - global or local, or none at all - the map's own BSP
compiler assigned that surface to. `VK_LoadWorldFog` (tr_world.cpp) now
loads *every* `LUMP_FOGS` entry (previously it stopped at the first
`brushNum == -1` one) into `s_worldFogs`, indexed exactly as the BSP itself
indexes them, and `RE_LoadWorldMap`'s per-surface loop records each
surface's own `fogNum` into a new `WorldSurfaceBatch::fogIndex`.
`RE_RenderScene`'s draw loop then switches the push constant's fog colour/
distance per-batch (the same "only re-issue when the value actually
changes" pattern already used for `vertexLit`'s overbright factor, see
"hoth2's overexposed terrain" above) instead of pushing one fog colour once
for the whole scene. Verified this is a strict correctness improvement, not
a rewrite of unrelated behaviour, by checking real BSP data directly:
hoth2's *entire* 7421-surface map has every single surface's `fogNum`
pointing at its one global fog - so per-surface lookup there is
mathematically identical to the old blanket-apply behaviour, confirmed by
byte-identical output on rebuild. vjun1's 13610 surfaces split
13565/42/3 across its global fog / local `fog_black` / no fog at all - so
this map is where the fix actually changes anything.

**Deliberately does not re-derive a local fog's world-space bounds from its
brush's planes** the way rd-vanilla's real `R_LoadFogs` does (reading the
first 6 - always axial-first, by BSP convention - sides of the named brush
to build an AABB, `tr_bsp.cpp`): since this renderer's fog is already a
simplified per-vertex distance-from-camera ramp rather than a real signed-
distance-to-boundary-plane test (see "World fog is now rendered" above), it
never needs to ask "is this point inside the volume" at render time at all
- trusting the compiler's own per-surface `fogNum` assignment is both
sufficient and exactly what determines which surfaces get which fog's
colour in real Quake3 too. Re-deriving brush geometry at load time would
have been redundant work, not a more correct result.

**Ranged fog** (`RE_SetRangedFog`, `tr_public.h`'s `refexport_t::
SetRangedFog` - a real, separate cgame-called API, `cl_cgame.cpp:
re.SetRangedFog(VMF(1))`, used for a sniper-scope-style widening of the
fog's near/far transition) is wired to a real implementation too
(`VK_SetRangedFog`, tr_world.cpp), an exact port of rd-vanilla's own
`RE_SetRangedFog`/the `fStart`/`fEnd` half of `RB_IterateStagesGeneric`
(`tr_shade.cpp`) - including its `g_oldRangedFog` save/restore quirk and a
worldspawn `linFogStart` key (parsed once at map load, matching
`tr_bsp.cpp`'s own key name and sign convention) as the other way real
Quake3 sets it, a per-map "designer override" rather than a runtime call.
A worldspawn `distanceCull` key is also parsed now (`s_distanceCull`,
default 12000 - rd-vanilla's own real default), but **only** to feed this
one formula - unlike rd-vanilla, it does not also change this renderer's
own fixed projection `zFar` or view-frustum culling distance
(`VK_BuildProjectionMatrix`'s 65536 constant is untouched, still documented
separately) - a deliberately narrower scope than the real field's full
reach. The world push constant gained a fourth `vec4` (`fogStart`, only
`.x` used) so a batch's fog ramp can start somewhere other than distance 0
from the camera without changing anything for the (default, common) case
where it doesn't - `world.vert`/`world.frag` and `vkWorldPushConstants_t`
all updated in lockstep, still comfortably under Vulkan's guaranteed
minimum 128-byte push constant size (112 bytes used).

**Verified real local-fog data loads correctly**: confirmed via this
renderer's own load-time log line, not assumed - vjun1 logs both entries
exactly matching `shaders/fogs.shader`'s real text: `loaded local fog
'textures/fogs/fog_black' (brush 6685) colour (0.00 0.00 0.00) opaque dist
3456` and `loaded global fog 'textures/fogs/vjun1' colour (0.74 0.59 0.39)
opaque dist 5190`. Full SP scene suite (menu/academy1/hoth2/yavin1/vjun1)
re-ran clean, no crashes, full rebuild warning-free. Pixel-diffed a build
with this fix against one without it (same fresh binaries, same homepath
setup, only `rd-vulkan_x86_64.so` swapped) rather than eyeballing a single
run: academy1 and the main menu (no fog data at all) came back byte-
identical, exactly as predicted; hoth2, yavin1, and vjun1 showed small
(<0.6% mean) differences, but every one of them is fully accounted for by
this renderer's own already-known sources of run-to-run noise, confirmed by
looking at *where* the diff images actually highlight pixels, not just
their percentage - randomly-scattered snow/rain particle positions
(`tr_weather.cpp`'s unseeded `rand()`, see "World weather/particle
effects" above) and sub-pixel NPC animation-timing edges, not a solid
region taking on a new colour anywhere. **Not independently confirmed**:
neither this project's four test maps' spawn-point camera framings happens
to have vjun1's specific 42 `fog_black`-tagged surfaces in view, so the
local-fog *colour* itself (as opposed to it loading correctly and the
surrounding refactor being provably regression-free) hasn't been directly
eyeballed. Ranged fog is implemented but **entirely unverified against real
data** - confirmed by direct BSP entity-string parsing that none of the
four test maps declare a `linFogStart` or `distanceCull` worldspawn key,
and none of this renderer's static spawn-point screenshots would exercise
a live `RE_SetRangedFog` call either - same honesty-over-false-confidence
standard as `spacedust`/`sand` above.

## World-geometry tcMod scroll

The first small step into real `.shader` script animation, picked because
it's directly visible and independently testable against real map data,
unlike some of this file's other recent additions. Extends `tr_shader.cpp`'s
minimal parser to record a defined shader's first stage's
`tcMod scroll <sSpeed> <tSpeed>` (UV units per second) - the single most
common `tcMod` type in these game's real shaders - alongside its existing
`map`/`blendFunc`/`fogparms` tracking. Every other `tcMod` type
(`rotate`/`scale`/`stretch`/`turb`/`transform`/`entityTranslate`) is still
unread: its numeric arguments fall through to the parser's existing
"unrecognized token, skip it" path once its own type keyword is consumed,
the same way any other not-specifically-handled keyword already is - not a
half-implementation, a deliberately narrow one.

**Update, much later: `tcMod scale` is real now too** - see "`tcMod scale`
for world geometry" further below for the real implementation and the
order-of-composition subtlety between it and `scroll` on the same stage.
`rotate`/`stretch`/`turb`/`transform`/`entityTranslate` remain unread.

**Confirmed against real map data before writing any code**, not
implemented speculatively and hoped-for: parsed all four test maps'
`LUMP_SHADERS`/`LUMP_SURFACES` and every one of this game's 68
`shaders/*.shader` files (3365 shader blocks) directly to find real,
currently-opaque, currently-drawn world surfaces whose shader's first
stage has a real `tcMod scroll`. Five genuine matches, all confirmed by
this renderer's own load-time behaviour (a temporary per-surface print,
removed before committing, counted exactly these surfaces on each map -
not just found by grepping - before the print was removed):

| Map | Shader | scroll s,t | Surfaces |
|---|---|---|---|
| vjun1 | `textures/impdetention/deathcon1a` | 0, 0.1 | **51** |
| academy1 | `textures/kejim/stars_scroll` | 0, 0.0195 | 2 |
| academy1 | `models/map_objects/cinematics/imp_wall` | -0.3, 0 | 1 |
| yavin1 | `models/map_objects/danger/ship_item04` | 0, 1 | 3 |
| hoth2 | `textures/hoth/ion_feedtube` | 0, 1 | 1 |

vjun1's `deathcon1a` (an Imperial-detention-level containment-field
texture) is by far the largest real case - 51 surfaces, not one or two
incidental ones.

**Implementation**: `WorldSurfaceBatch` gained `scrollS`/`scrollT` (the raw
per-second speed, looked up once at load time via the new
`VK_GetShaderTcModScroll`, tr_shader.cpp) - 0,0 for the overwhelming
majority of shaders that never declare one. `RE_RenderScene`'s draw loop
folds this into the same per-batch push-constant-switch pattern already
used for `vertexLit`/`fogIndex` (see "hoth2's overexposed terrain" and
"Local fog volumes" above): storing the raw speed rather than a
precomputed offset means every batch sharing the same scroll speed at a
given moment still shares one push, not one per batch. The actual offset
(`speed × current simulated seconds`) is computed once per distinct speed
value per frame and packed into the existing `fogStart` push-constant
field's otherwise-unused `.y`/`.z` components (see `vkWorldPushConstants_t`'s
comment, `tr_local.h`) rather than growing the push constant struct further
- still comfortably under Vulkan's guaranteed minimum 128-byte size.
`world.vert` adds it directly to the diffuse UV (never the lightmap UV,
which is baked per-vertex at compile time and never scrolls in real
Quake3 either) before rasterization; `vk.worldSampler`'s existing
`VK_SAMPLER_ADDRESS_MODE_REPEAT` wrap mode (already there for ordinary
texture tiling) makes an ever-growing UV offset wrap correctly with no
extra handling needed.

**Verified**: full SP scene suite (menu/academy1/hoth2/yavin1/vjun1) clean,
no crashes, full rebuild warning-free. The temporary per-surface print
confirmed exact matches against the table above on every map (51 on
vjun1, 1 on hoth2, 3+1 on yavin1, 1+2 on academy1) before being removed.
**Not independently seen in motion on screen**: none of the five real
matches happen to be in view from any of the four test maps' spawn-point
camera framings (vjun1's spawn is the ship's cockpit interior, nowhere
near the detention level's containment field; the others are similarly
out of frame) - same "verified via data and code review, not by eye"
standard already applied to local fog's colour and ranged fog above, for
the same reason: a fixed spawn-point screenshot can't be expected to
happen to frame every real map feature, and that's not a reason to skip
implementing or documenting one honestly.

## Real per-vertex colour for vertex-lit surfaces

Closes a gap this file has flagged as a known limitation since "hoth2's
overexposed terrain" above: that fix stopped vertex-lit surfaces
(`q3map_nolightmap` + `rgbGen vertex`, no baked lightmap at all - hoth2's
snow terrain is the confirmed real case) from being wrongly overbright-
doubled, but left them with no real per-vertex shading at all - just the
diffuse texture's own flat colour, undoubled but uniform. Real Quake3 map
compilers bake genuine ambient/directional lighting per vertex for exactly
these surfaces (`drawVert_t::color`), and `rgbGen vertex` tells the real
renderer to actually use it - this was read nowhere in this renderer until
now.

**Implementation**: `WorldVertex` gains a `color[4]` attribute, a new
vertex input binding (`world.vert`'s `inColor` at `location = 3`,
`VK_FORMAT_R32G32B32A32_SFLOAT`) shared by every user of `vk.worldPipeline`
- world geometry, the sky box, and Ghoul2 models all reuse this one
pipeline/vertex layout, so all three needed a value, not just world
surfaces. Rather than adding a shader-side branch to tell "this vertex has
real baked colour" from "this vertex doesn't", every vertex that *isn't*
vertex-lit - real lightmapped world geometry, sky faces, Ghoul2 meshes -
gets a hardcoded `(1,1,1,1)` (white) instead of its own BSP data. This is
correct, not just a placeholder: real lightmapped shaders use `rgbGen
identityLighting`, not `rgbGen vertex`, so rd-vanilla itself never applies
vertex colour to them either, even though their `drawVert_t` also happens
to carry one - hardcoding white where it wouldn't be applied anyway
reaches the identical visual result to a real `rgbGen` parse without
needing one. `world.frag` then unconditionally multiplies
`diffuse * lightmap * vertexColor * overbright` - a `(1,1,1,1)` multiply
is a true no-op, so lightmapped/sky/Ghoul2 rendering is provably unchanged
by this pass. `VK_TessellatePatchQuad`'s existing biquadratic-Bezier
interpolation (already used for position/UV) was extended to interpolate
colour the same way, so a vertex-lit curved surface (none of the four test
maps happen to have one, but the code path is shared) would blend
correctly too, not just flat-shade at the nearest control point.

**Verified real improvement, not just a real difference**: pixel-diffed a
build with this fix against one without it (same binaries otherwise, only
`rd-vulkan_x86_64.so` swapped, via this project's `render_regression_*`
CMake targets - see "tests: add CMake targets..." for why that comparison
method is trustworthy now). academy1 and the main menu came back
effectively unchanged (0.06%/0.0% mean diff, within this renderer's already
-documented noise floor - see "World-geometry tcMod scroll" above);
hoth2 - the only test map with any vertex-lit surfaces at all - showed a
real, substantial difference (3.14% mean, 35.6% of pixels), concentrated
exactly on the terrain, not scattered noise. Compared directly against a
fresh `rdsp-vanilla` capture of the same scene: the *before* screenshot was
a flat, uniformly neutral white/grey snowfield; both the *after* screenshot
and real `rdsp-vanilla`'s own terrain show the same soft blue-lavender
tint - a real, visible move toward matching the reference, confirmed by
eye against an actual side-by-side, not assumed from the diff percentage
alone.

## World geometry blend modes (alpha/additive, not just opaque)

Closes another explicitly-flagged gap: world geometry was drawn through a
single opaque `VkPipeline` regardless of its shader's own `blendFunc` -
"World geometry doesn't even get the 2D path's blend-mode selection yet"
(this file's own "not implemented" list, now out of date). Real map data
shows this wasn't a theoretical gap: parsed every one of the four test
maps' real, currently-drawn (non-`SURF_NODRAW`/`SURF_SKY`/flare) world
surfaces against all 68 `shaders/*.shader` files' first-stage `blendFunc`.
Hoth2 alone has 544 surfaces whose shader is additive
(`textures/flares/solid_blue`, 283; `textures/common/dark_dust`, 199;
`textures/common/env_glass`, 57; several others); vjun1 has 461
(`textures/common/dark_dust`, 259; several real `glass_security_*` window
shaders); yavin1 has 81 (`textures/yavin/tree1` foliage, 24; decals,
grating, a ship prop); academy1 has 71 (`dark_dust` again).

**Two genuinely different bugs were hiding under one symptom**, found by
checking, not assuming, whether each shader's own name also happens to
resolve directly to a real texture file (`VK_FindImage`):

- **Wrongly opaque**: a shader whose name doubles as its own real texture
  file (`textures/flares/solid_blue`, `textures/hoth/ion_screen_01`/`_02`,
  `textures/impdetention/screen1`, `textures/vjun/tech`,
  `textures/yavin/tree1`, `textures/decals/slashmark1`,
  `textures/common/gradient2` - confirmed by checking the real .pk3
  contents directly, not guessed) resolved on the very first `VK_FindImage`
  call, before any blend-mode check ever ran, and was drawn fully opaque
  regardless of its shader's real `blendFunc`.
- **Wrongly invisible**: a shader whose name does *not* directly resolve
  (`textures/common/dark_dust`, `env_glass`, every `glass_security_*`,
  `dark_orange`, `tan_gradient`) falls into `RE_LoadWorldMap`'s existing
  map-image fallback (see "hoth2's missing terrain" above) - which was
  deliberately gated to `BLEND_OPAQUE` shaders only, specifically *because*
  this renderer had no blend pipeline to safely draw the result with yet
  (see that fallback's own comment, tr_world.cpp). Non-opaque shaders hit
  the fallback, failed the gate, and were skipped outright - real vjun1
  windows (`glass_security_hex`/`_chain`/`_tris`/`_thex`) and dust-haze
  effects on every map were never drawn at all, not drawn wrong.

**Fixed the first category, deliberately left the second one alone**:
`WorldSurfaceBatch` gained a `blendMode` field (`VK_GetShaderBlendMode`,
already existed for the 2D UI path), and `vk.worldPipeline` gained two
siblings - `worldPipelineAlpha`/`worldPipelineAdditive` (same shaders/
vertex layout/pipeline layout, only the blend state and depth-write
differ, same relationship `vk.polyPipelineAdditive` already has to
`vk.polyPipeline`) - selected per-batch in `RE_RenderScene` exactly like
`vertexLit`/`fogIndex`/the tcMod scroll speed already are.
`VK_GetShaderBlendMode` needed a second parameter first
(`notFoundDefault`): its existing default (`BLEND_ALPHA`) is correct for
the 2D UI path's "bare image, no `.shader` script" case, but wrong for
world geometry, where the overwhelmingly common case - an ordinary wall or
floor texture with no `.shader` script at all - must default to
`BLEND_OPAQUE` instead, matching real Quake3's own implicit-shader
behaviour; reusing the UI path's default here would have made every one of
those ordinary textures translucent. `s_worldSurfaces` is sorted by blend
category after loading (opaque, then alpha, then additive) so the single
draw loop still only switches pipelines a handful of times per frame, not
per-batch, the same reasoning as the existing per-batch push-constant
switches.

The second category (the map-image-fallback gate) was deliberately **not**
loosened in this pass, after checking what would actually happen if it
were: `env_glass` and `glass_security_hex`'s first stage use `tcGen
environment` (reflection-vector UV generation, not a mapping this renderer
implements at all) - resolving their fallback image and sampling it with
the surface's own baked UV would render an actively wrong static texture
smeared across the glass, not a translucent look, the same "invisible
beats wrong" reasoning that gated the fallback in the first place, just
for a more specific, now-confirmed reason than "no blend pipeline exists
yet". `dark_dust`/`tan_gradient`/`dark_orange` would render closer to
correct (a plain `clampmap`, no `tcGen`) but use `rgbGen const` (a fixed
per-shader tint) this renderer doesn't parse, so left alone too rather
than mixing "some fallback shaders now render, others still don't" for a
partial win. **Done in a later pass** - see "`rgbGen const` and a widened
additive map-image fallback" below for the real `tcGen`-presence gate that
made this safe to do without also letting `env_glass`/`glass_security_*`
through.

**Verified**: full SP scene suite (menu/academy1/hoth2/yavin1/vjun1) clean,
no crashes, full rebuild warning-free. Pixel-diffed a build with this fix
against one without it (same binaries otherwise): academy1/menu came back
at baseline noise levels (<0.02% mean); hoth2/yavin1/vjun1 showed small
real differences (0.12-0.28% mean) consistent with, but not conclusively
isolated from, this renderer's already-documented per-run noise floor
(unseeded weather particles, animation-timing jitter) - **not
independently confirmed by eye** that a specific now-blended surface
(e.g. hoth2's `solid_blue` flares, yavin1's `tree1` foliage) is actually
visible and correctly blended from any of the four test maps' spawn-point
camera framings, the same honesty standard already applied to tcMod
scroll and local fog's colour above. The mechanism itself - which shaders
get which pipeline, and why - is verified by direct BSP/`.shader` data and
code review, not by assuming a diff percentage alone means success.

## Static flares (MST_FLARE)

Closes the last explicitly-flagged "skipped entirely" gap in
`RE_LoadWorldMap`'s surface loop: BSP `MST_FLARE` surfaces (light-source
glow sprites - landing-beacon lights, console/warning lights - not a
dynamic light system) were previously dropped with a comment explaining why
("need their own draw path, not implemented yet"), not drawn wrong. Checked
real data before implementing: parsed `LUMP_SURFACES`/`LUMP_SHADERS`
directly for all four test maps. academy1 and yavin1 have zero real flare
surfaces; hoth2 has 98 (`textures/flares/flare_blue_pulse`: 55,
`gfx/misc/flare`: 43); vjun1 has 45 (`textures/flares/flare_bluehue`: 29,
`gfx/misc/flare`: 16) - a real, non-trivial feature on half the test maps,
not a hypothetical one.

**Parsing**: `dsurface_t`'s `MST_FLARE` fields aren't a vertex/index range
like every other surface type - `lightmapOrigin` is the flare's world-space
point, `lightmapVecs[2]` its normal (confirmed against rd-vanilla's real
`ParseFlare`, tr_bsp.cpp: `flare->origin = ds->lightmapOrigin`,
`flare->normal = ds->lightmapVecs[2]`) - so `RE_LoadWorldMap` parses them
into a separate `s_worldFlares` list rather than feeding the shared vertex/
index buffers every other branch does. Each entry also records its
image (`VK_FindImage`, with the same real, now-shared map-image fallback
the opaque-world-surface path already uses when a shader's own name isn't
directly a texture file - unconditionally here, not gated to
`BLEND_OPAQUE`, since a flare quad never draws through the opaque world
pipeline in the first place; without this fallback, hoth2's
`flare_blue_pulse` and vjun1's `flare_bluehue` - the majority of both
maps' real flare surfaces, 55/98 and 29/45 - silently failed to load at
all) and its `alphaGen portal <range>` value (a new, minimal addition to
tr_shader.cpp's shader-script scanner - `VK_GetShaderPortalRange` - mirroring
`VK_GetShaderTcModScroll`'s existing shape; only `flare_blue_pulse` of this
checkout's 3 real flare shaders declares one, at 50, everything else falls
back to rd-vanilla's own default of 30).

**Rendering**: `VK_DrawWorldFlares` (tr_world.cpp), called once per scene
from `RE_RenderScene` alongside `VK_DrawScenePolys`/`VK_DrawWeatherEffects`.
Ported from rd-vanilla's real `RB_SurfaceFlare` (tr_surface.cpp): push the
flare's origin 3 units off the surface along its normal, fade its intensity
by view angle (`d = -DotProduct(dir, normal)`, `dir` = camera-to-flare),
scale its radius by distance (clamped to a 5-unit floor) below 512 units.
Reuses the existing `PolyVertex`/`vk.polyPipelineAdditive` runtime-poly
infrastructure wholesale (own dedicated `vk.flareVertexBuffer`, same
reasoning tr_weather.cpp's particles already have their own buffer) rather
than inventing new vertex format or pipeline state for what is, mechanically,
the same "camera-facing quad, additive blend" primitive weather particles
already are.

**Not a port of `RB_TestZFlare`** - rd-vanilla's own single-point
`glReadPixels`-against-the-depth-buffer occlusion pre-test - and
deliberately so, not just skipped: `vk.polyPipelineAdditive` already renders
with depth-test-on/depth-write-off (real per-poly occlusion runtime polys
already need against walls), so a flare behind a wall already fails the
GPU's own real per-pixel depth test by the time this draws (called after
all opaque/Ghoul2/sky geometry in `RE_RenderScene`) - a strictly finer-
grained equivalent of what `RB_TestZFlare`'s single sample point
approximates (a partially-occluded quad fades in from the visible edge
rather than blinking fully on/off), without a CPU-side readback at all.
**Not independently confirmed by eye** against a real occluded flare,
though - none of the four test maps' fixed spawn cameras happens to frame
a flare from behind an intervening wall; the depth-test mechanism itself is
standard Vulkan pipeline state already exercised correctly by every other
draw call in this renderer, not new or flare-specific.

**Always additive, not per-shader blend mode** - tried the classified value
first and it was actively wrong, not just imprecise. hoth2's
`flare_blue_pulse` (55 of its 98 real flare surfaces) declares `blendFunc
GL_ONE GL_ONE_MINUS_SRC_COLOR`, a softer "screen"-style additive this
renderer's binary blend-mode classifier can't represent and falls back to
classifying as `BLEND_ALPHA`. Standard src-alpha-blending an opaque-alpha
glow texture whose RGB has been faded near-black by the view-angle fade
above paints a **solid black square over the sky**, not a dim glow -
caught directly in a real hoth2 capture (rows of solid black boxes along
the horizon where the beacon lights are) before this was corrected to
unconditional additive. Additive can't produce that failure at any fade
value (it only ever brightens); every one of this checkout's 3 real flare
shaders is visually a glow effect, so additive is the strictly-safer
approximation for all of them, not just the one (`gfx/misc/flare`) whose
own `blendFunc` already says so.

**Verified**: full SP scene suite (menu/academy1/hoth2/yavin1/vjun1) clean,
no crashes, full rebuild warning-free. `rd-vulkan: loaded <map>: ... N
flares` now logs 98 for hoth2 and 45 for vjun1 - an exact match against the
real BSP surface counts parsed independently above, confirming every real
flare surface resolves an image and gets stored, not just some of them.
Pixel-diffed a build with this feature against one without it (same
binaries otherwise, after the black-square bug above was already fixed):
academy1/menu (0 real flares) came back at baseline noise (<0.01% mean);
hoth2 crossed this harness's own MINOR_DIFF threshold (0.61% mean, up from
a same-build noise-floor rerun's 0.42-0.46%) with soft glow-shaped blobs
along the horizon in the diff image - visually distinct in shape from the
renderer's already-documented per-run noise (small round weather-particle
dots) - and vjun1 showed a real, structured change concentrated at the
cockpit window frame (a console/warning light location), also distinct
from the NPC-silhouette-outline jitter yavin1's own diff shows as its
(unrelated, pre-existing, already-documented) noise floor. Not
independently confirmed by eye as a specific recognizable glow sprite
sitting on a specific in-game light fixture - the diff evidence establishes
"something new and flare-shaped appeared exactly where real flare data
says it should," not a side-by-side visual match against rd-vanilla's own
flare rendering (out of scope here, since this renderer doesn't attempt to
match RB_TestZFlare's exact occlusion timing). **Update, much later: real
`rgbGen const`/`rgbGen wave` stage evaluation for flares is implemented
now too, and this specific gap - assumed inapplicable to any of this
checkout's 3 real flare shaders at the time this section was written, which
turned out to be a genuine mistake - is closed** - see "Real rgbGen const/
wave for flares" further below for the real formula and the confirmed-by-
eye fix on hoth2's landing-beacon flares. `alphaGen` stage evaluation
(beyond the already-real `alphaGen portal` radius, "Static flares" above)
is still not matched.

## Ghoul2 per-surface on/off overrides (`G2API_SetSurfaceOnOff`)

Closes a real API gap, not a cosmetic one: `G2API_SetSurfaceOnOff` was a
complete no-op stub (`(void)ghlInfo; (void)surfaceName; (void)flags; return
qfalse;`) despite being called by real, exercised game code on every single
player/NPC spawn, not just some optional gameplay feature - `g_client.cpp`'s
`G_SetG2PlayerModelInfo` parses an NPC's `.npc` file `surfOff`/`surfOn` keys
and calls this for every token in each list, unconditionally, for every
spawned player-model entity. Checked real `.npc` data before implementing,
not assumed: extracted `ext_data/npcs/*.npc` from this checkout's own
`assets*.pk3`s and found 14 real species declaring `surfOff`
(`gran`/`human_merc`/`impcommander`/`imperial`/`impofficer`/`jedif`/
`protocol_imp`/`rockettrooper`/`rodian`/`stcommander`/`stofficer`/
`stofficeralt`/`ugnaught`/`weequay`) - e.g. `protocol_imp` (`surfOff head` +
`surfOn head_off`, an alternate damaged-head-panel look for that skin) and
plain `imperial` troopers (`surfOff l_arm_key` - a "carrying a key" sleeve
patch, turned back on only for key-carrying NPCs via `g_client.cpp`'s own
`CLASS_IMPERIAL`+`ent->message` special case).

**Real semantics, adapted lookup**: ported from rd-vanilla's actual
`G2_SetSurfaceOnOff`/`G2_IsSurfaceLegal` (`G2_surfaces.cpp`) - same
`CGhoul2Info::mSlist` sparse-override-list data structure (a real, shared,
cross-renderer field from `ghoul2_shared.h`, not something invented for
this renderer), same "only the `G2SURFACEFLAG_OFF`/`_NODESCENDANTS` bits of
the incoming flags are ever applied, everything else in a surface's baked
flags is preserved" masking, same "only push a new override entry if it'd
actually change something" optimization. What's adapted rather than copied
verbatim: rd-vanilla resolves a surface name via `ghlInfo->currentModel-
>mdxm`, a real `model_t` pointer this renderer's `CGhoul2Info` instances
never populate (see `VK_LoadGhoul2Model`'s own comment on why models are
tracked by this renderer's own cache index, `mModel`, instead) - so name
resolution goes through a new `VK_FindGhoul2SurfaceIndex` (tr_model.cpp)
against this renderer's own cached `VulkanGhoul2Model::surfaceNames`/
`surfaceFlags` (populated at `VK_LoadGhoul2Model` load time, previously
only used transiently to decide *load-time* skips, now also persisted for
this runtime lookup) instead.

**Applied without re-baking geometry**: `VulkanGhoul2Model`'s per-model
vertex/index buffers are still baked once and shared across every entity
using that model+skin (unchanged) - a per-*instance* runtime override can't
change shared baked geometry, so `GhoulSurfaceDraw` (the per-surface draw-
call list every `VulkanGhoul2Model` already had) gained a `surfIndex` field
(the surface's original `thisSurfaceIndex`, `-1` for a plain `.md3` static
model with no Ghoul2 surface hierarchy), and `VK_DrawGhoul2Entities`
consults the *current entity's own* `mSlist` for a matching override right
before each surface's `vkCmdDrawIndexed` call, skipping it if overridden
off - real per-instance behaviour (two entities sharing one cached
model+skin can independently show/hide the same named surface) without
touching the shared cache at all, the same architectural trick this
renderer's live per-bone animation already relies on (shared mesh, per-
instance pose).

**Not ported**: `G2SURFACEFLAG_NODESCENDANTS`'s real recursive "also hide
every child surface in the hierarchy" behaviour (`G2_FindRecursiveSurface`)
- the bit is still masked/stored faithfully in `mSlist` for forward
compatibility, but the draw-time check only ever looks at
`G2SURFACEFLAG_OFF` on the exact named surface, matching the scope this
renderer's pre-existing *load-time* baked-flags skip check already had.
No real test case in this checkout's `.npc` data was found using
`NODESCENDANTS` to justify implementing the hierarchy walk now.

**Verified**: full SP scene suite clean, no crashes, warning-free rebuild.
Confirmed real activation on real data, not just that the code compiles:
a temporary diagnostic print (removed before committing) showed
`models/players/imperial/model.glm surface 52 'l_arm_key' -> flags 0x2
(base 0x0)` while capturing vjun1 - an ordinary imperial trooper entity in
that scene really did get its `l_arm_key` surface toggled off at spawn,
exactly matching `imperial.npc`'s own `surfOff l_arm_key` and the base
model's own default (surface normally on, `0x0`). Pixel-diffed a build
with this fix against one without it: all 5 scenes stayed within this
renderer's already-documented per-run noise floor (weather-particle
scatter, animation-timing jitter) - the affected surface is a small sleeve
patch on an NPC not currently framed close/large enough in any of the 4
test maps' fixed spawn cameras for its removal to read as a distinct
signal above that noise, unlike flares/vertex-colour/blend-modes' own
diffs above. Real activation on real data is confirmed directly (the log
line above); the *visual* effect specifically is not independently
confirmed by eye in this checkout's own captures.

## `rgbGen const` and a widened additive map-image fallback

Closes a real, previously-invisible-outright gap identified but
deliberately not fixed during "World geometry blend modes" above: real
`textures/common/dark_dust`/`tan_gradient`/`dark_orange`/`blue_gradient`
shaders (`clampmap textures/common/gradient`, `blendFunc GL_ONE GL_ONE`,
`rgbGen const ( r g b )`, no `tcGen`) were still never drawn at all, even
after that pass added real alpha/additive world pipelines - because their
shader name (`textures/common/dark_dust`) doesn't directly resolve to a
texture file (the real texture is `textures/common/gradient`, referenced
only via the shader's own `clampmap`), and `RE_LoadWorldMap`'s map-image
fallback that would normally catch that case was deliberately gated to
`BLEND_OPAQUE` shaders only - `dark_dust` classifies as `BLEND_ADDITIVE`
(a real, correctly-classified blendFunc), so it never took that path. Real
scale confirmed before touching anything: 71 real `dark_dust` surfaces on
academy1 alone (see the blend-modes section's own survey).

**The `tcGen` risk, checked directly rather than assumed away**: the
original opaque-only gate wasn't arbitrary - `textures/common/env_glass`
and every `glass_security_*` variant are *also* real `blendFunc GL_ONE
GL_ONE` shaders (checked their actual `.shader` blocks directly, not
assumed from their surfaceparm/qer_trans lines), meaning blend mode alone
can't distinguish them from the safe dust-cloud family. What actually
distinguishes them is `tcGen environment` on that same first stage - a
reflection-vector UV generation mode this renderer still doesn't
implement, and the real reason resolving their fallback image would render
an actively wrong static texture rather than a translucent glass look (see
that section's own comment). So the fallback gate is widened along the
*right* axis instead of the *convenient* one: `VK_ShaderHasTcGen`
(tr_shader.cpp) records whether a shader's first stage declares any
`tcGen` keyword at all (not which kind - none are implemented either way),
and `RE_LoadWorldMap`'s gate becomes "`BLEND_OPAQUE`, OR `BLEND_ADDITIVE`
with no `tcGen`" - `dark_dust` and siblings pass (additive, no `tcGen`);
`env_glass`/`glass_security_*` still correctly don't (additive, but real
`tcGen environment`).

**Update, much later**: real `tcGen environment` UV generation IS now
implemented - see "`tcGen environment` (reflection-mapped UV generation)"
below. The gate above was widened again at that point to also let
`env_glass`/`glass_security_*` through specifically *because* their fallback
image is no longer sampled with the wrong (baked) UVs - the "actively wrong
static texture" problem this section's own comment warned about no longer
applies to that one specific case.

**`rgbGen const`**: a real, minimal addition to tr_shader.cpp's shader-
script scanner (`VK_GetShaderRgbGenConst`, mirroring `VK_GetShaderTcModScroll`'s
existing shape) - every one of this checkout's real `rgbGen const` shaders
uses it as their *only* rgbGen (no `identityLighting`/`vertex`/wave
animation to also model), so applying it is a straight per-vertex colour
overwrite (`RE_LoadWorldMap`, right after - and overriding - the existing
real-baked-vertex-colour block for vertex-lit surfaces, since these
`q3map_nolightmap` dust shaders never have real baked vertex colour to
lose anyway) reusing the exact mechanism "Real per-vertex colour for
vertex-lit surfaces" above already built, not a new code path.

**Verified**: full SP scene suite clean, no crashes, warning-free rebuild.
Pixel-diffed a build with this fix against one without it: academy1 came
back MAJOR_DIFF (65.7% of the frame, 13.2% mean) - large, but confirmed by
eye to be exactly the right shape and nothing else: sharp, planar bands
following the same diagonal window-light-shaft geometry the scene's real
light beams already show, not a wash covering unrelated surfaces or an
opaque blowout (the specific failure mode the original gate's own comment
warned an unconditional fallback would cause) - genuinely new
light-shaft-dust volumes appearing exactly where academy1's real BSP data
places them, translucent and additively blended as their shader declares.
The other three maps stayed within this renderer's already-documented
per-run noise floor (hoth2 0.44%/0.14%, vjun1 0.18%/0.68%, yavin1
0.04%/0.06% mean/changed-pixels) - consistent with those maps having far
fewer or no real surfaces using these specific shaders in view from their
fixed spawn cameras, not a sign the fix only partially worked.

## Window resize / swapchain recreation

Closes a real, severe usability gap that had nothing to do with any
specific map or shader: a real window resize (dragging the window edge on
a real desktop - the one thing every one of this renderer's fixed-window-
size headless test scenes can never exercise) permanently froze rendering.
Root cause: `RE_BeginFrame`'s `vkAcquireNextImageKHR` call returns
`VK_ERROR_OUT_OF_DATE_KHR` once the window's actual size no longer matches
the swapchain's - real, expected Vulkan behaviour after any resize - and
the existing response to that was just to log a warning and skip the
frame. Since nothing ever rebuilt the swapchain, *every* later frame's
acquire kept failing the exact same way - not a one-frame hiccup, a
permanent freeze until a full engine restart. A second, quieter bug sat
right next to it: the screenshot readback image (`VK_CreateReadbackImage`,
tr_cmds.cpp) is a lazily-created singleton sized to the swapchain at
whatever moment it first gets used, and was never being invalidated on a
resize either - unlike the frozen-swapchain bug this one wouldn't even
error loudly, since `vkCmdCopyImage`'s region-based copy doesn't require
matching image extents, just a region that fits both; it would have
quietly produced a screenshot cropped or misaligned to the old window size
instead.

**Fix**: `VK_RecreateSwapchain` (tr_init.cpp) tears down and rebuilds
every swapchain-*sized* resource - the swapchain itself, its image views,
the depth image/view/memory, and the framebuffers - at the window's
current real drawable size, and forces the readback image to regenerate
at that same new size on its next use. Deliberately *not* a rebuild of
the render pass (depends only on format, stable across a resize) or any
pipeline (every one already uses `VK_DYNAMIC_STATE_VIEWPORT`/`SCISSOR`,
set fresh from `vk.swapchainExtent` each frame specifically so a resize
never needs to touch them). Called two ways from `RE_BeginFrame`:
proactively, by comparing `SDL_Vulkan_GetDrawableSize` against
`vk.swapchainExtent` before even trying to acquire (catches the common
case with zero dropped frames); reactively, if `vkAcquireNextImageKHR`
still somehow returns `VK_ERROR_OUT_OF_DATE_KHR` anyway, recreating and
retrying the acquire once. Not a port of anything in rd-vanilla - GL has
no equivalent "swapchain" concept for a resize to invalidate in the first
place, so this is a from-scratch implementation of what Vulkan's own
model requires, the same "reuse strategy" this renderer applies
throughout (see "Ghoul2 is not reused from rd-vanilla" below).

**Verified with a real live resize, not just code review** - this gap
specifically can't be exercised by any of this renderer's own fixed-
window-size xvfb-based test scenes, so `xdotool` was installed and used to
resize the *actual* X11 window of a running, unmodified SP engine process
mid-session (polling for each scripted screenshot's file to appear on
disk, then immediately issuing the resize, all within one script so
nothing races against the engine's own frame pacing): four resizes in one
run against academy1 (800x600 -> 640x480 -> 1280x720 -> 320x240, growing
and shrinking repeatedly, down to a quarter of the original test
resolution) each produced a correctly-sized, correctly-rendered screenshot
- confirmed by both the reported PNG dimensions matching exactly and by
eye (real scene geometry, not a black/garbled frame) - with a clean `+quit`
exit (status 0) and zero `OUT_OF_DATE`/`VK_ERROR`/crash output in the log
across all four. Also ran the full standard SP regression suite (no resize
involved) to confirm the added per-frame size check doesn't affect normal
operation: all 5 scenes matched the pre-fix build within this renderer's
already-documented noise floor.

**Known, deliberate scope boundary, found during that same live test**:
after a resize, the actual 3D/2D scene content kept rendering at the *old*
logical resolution, letterboxed into a corner of the new, larger swapchain
image (confirmed directly - a resize from 800x600 to 1024x768 produced a
1024x768 PNG with real rendered content filling only its top-left 800x600
and solid black filling the rest). This is expected, not a bug this fix
should also close: the game's logical viewport size is `cls.glconfig`/
`r_customwidth`/`r_customheight` state cached client-side (`code/client/`,
shared by every renderer, never touched by this fix) - a live OS-level
window resize alone doesn't make the *client* re-query it without a
`vid_restart`, in rd-vanilla either, since that caching is renderer-
agnostic shared code. What this fix actually guarantees is the renderer-
level half: the Vulkan swapchain itself never again gets stuck permanently
invalid after a resize, whatever resolution the client above it decides to
render at.

## Ghoul2 surface bolts (`G2API_AddBolt`'s surface-name path)

Closes a real gap this file's own "not implemented" list had carried for a
long time as "surface bolts... aren't implemented" - not a rare edge case,
it turned out, but something real, exercised game code relies on for
*every single player spawn in every one of this checkout's 4 test maps*.
Real Ghoul2 convention (confirmed by directly parsing real `.glm` data,
not assumed from the API shape alone): a surface whose name starts with a
literal `*` is a `G2SURFACEFLAG_ISBOLT`-flagged "tag" surface Carcass
(the model compiler) emits purely as an attachment-point marker - a real,
always-3-vertex/1-triangle "tag triangle", always with an empty shader
name, never meant to be drawn as mesh geometry - and `G2API_AddBolt`'s
real precedence tries a **surface** name match before ever trying a bone
name. Parsed `models/players/kyle/model.glm` directly: 45 real `*`-
prefixed surfaces (`*head_eyes`, `*r_hand_cap_r_arm`, `*l_hand_cap_l_arm`,
`*hip_l`, ... every one carrying `G2SURFACEFLAG_ISBOLT`); `saber_1.glm`
has one, `*blade1`. Real game code calls `G2API_AddBolt` with names like
these constantly - `g_client.cpp`'s `"*head_eyes"` on every player spawn,
`wp_saber.cpp`'s `"*flash"`/`"*r_hand_cap_r_arm"`/`"*l_hand_cap_l_arm"`,
`g_turret.cpp`'s `"*muzzle1"`/`"*flash03"`, `g_emplaced.cpp`'s
`"*cannonflash"`/`"*seat"` - every one of which silently returned -1
before this (bone-only lookup, no surface fallback), with most callers
never checking for that failure.

**Two-part fix, both real ports, not new inventions**:

1. `G2API_AddBolt` (tr_init.cpp) now tries `VK_FindGhoul2SurfaceIndex`
   (the same lookup "Ghoul2 per-surface on/off overrides" above already
   added) *first*, falling back to the existing bone lookup only if that
   fails - exact precedence match for rd-vanilla's real `G2_Add_Bolt`
   (`G2_bolts.cpp`), including reusing an existing bolt on that surface or
   a freed slot before appending a new one, mirroring the bone path's own
   logic.
2. `VK_GetGhoul2SurfaceBoltMatrix` (tr_model.cpp) computes the actual
   matrix - ported from rd-vanilla's real `G2_ProcessSurfaceBolt2`'s
   "normal model tag" branch (`tr_ghoul2.cpp`), *not* its sibling branch
   for `G2API_AddSurface`'s barycentric procedurally-generated tags
   (`G2SURFACEFLAG_GENERATED`), which this renderer doesn't implement
   `AddSurface` for at all. The real formula: skin the tag triangle's 3
   raw vertices through the current pose using the exact same per-vertex
   weighted-bone-transform arithmetic `VK_SkinGhoul2Model` already applies
   to ordinary mesh vertices (same weights, same math, just 3 vertices
   instead of a whole surface), then build an orthonormal basis from the
   triangle's sides - real rd-vanilla calls the two side vectors it uses
   "longest"/"shortest", but `iG2_TRISIDE_LONGEST`/`_SHORTEST`
   (`mdx_format.h`) are fixed constants (0 and 2), not a runtime edge-
   length comparison, so this is a fully mechanical formula with no
   ambiguity to guess at, ported exactly.

**A genuine, if invisible, correctness cleanup found along the way**: this
checkout's existing Ghoul2 load-time skip check only ever excluded
`G2SURFACEFLAG_OFF` from the draw path, never `G2SURFACEFLAG_ISBOLT` -
every one of those 45+1 real tag-triangle surfaces was being fed through
the *normal* per-surface shader-resolution path every load, for nothing
(their real, always-empty shader name always failed `VK_FindImage`, and
their default classification - `BLEND_ALPHA`, not `BLEND_OPAQUE` - kept
them out of the map-image fallback too, so this was never a *visible* bug,
just wasted resolution attempts and unused vertex/index data). Implementing
surface bolts needed their tag-triangle data captured somewhere regardless,
so `VK_LoadGhoul2Model` now explicitly recognizes `G2SURFACEFLAG_ISBOLT`,
captures the 3-vertex tag data into `VulkanGhoul2Model::tagTriangles`
(keyed by surface index), and excludes the surface from the normal draw
path outright - closing the resolution-attempt waste as a side effect of
doing the real feature properly, not a separate fix.

**Verified**: full SP scene suite clean, no crashes, warning-free rebuild.
Confirmed real, extensive activation on real data via a temporary
diagnostic (removed before committing, and re-captured *after* removing
it - the print itself briefly showed up as visible on-screen console text
in a first draft of this verification, which would have polluted the
comparison if not caught): hundreds of successful surface-name resolutions
logged across every non-menu test scene (`*head_eyes`, `*r_hand`,
`*l_hand`, `*hips_l_knee`/`*hips_r_knee`, `*r_arm_elbow`/`*l_arm_elbow`,
`*r_leg_foot`/`*l_leg_foot`, ...) against real player/NPC models (kyle,
jedi, jedi_tf, jedi_hf, and others) on every single spawn. Pixel-diffed a
build with this fix against one without it: all 5 scenes stayed within
this renderer's already-documented per-run noise floor, confirming the
ISBOLT exclusion changed nothing visible (as expected, since those
surfaces were already unreachable through the old path) - the *bolt
matrices themselves* aren't independently confirmed by eye against a
visible effect that depends on one (e.g. an actual muzzle flash sprite or
saber blade tag) in this checkout's own fixed spawn-camera captures, only
that resolution succeeds and produces a matrix rather than failing.

## Ghoul2 model-to-model attachment

Closes another real gap this file's own "not implemented" list carried as
"model-to-model attachment (`AttachG2Model`/`AttachEnt`)". Confirmed real,
exercised usage before implementing, not just an API shape that happens to
exist: `wp_saber.cpp:407` calls `G2API_AttachG2Model` to attach a drawn
weapon/saber's own sub-model (`ent->ghoul2[weaponModel]`) to a bolt on the
player body's sub-model (`ent->ghoul2[playerModel]`) every time a weapon is
raised, and `Q3_Interface.cpp:5897`/`5915` call it twice more to attach a
cinematic prop model to the left/right hand bolts during scripted cutscene
behaviors. (Two other call sites - `g_emplaced.cpp:1069`,
`g_misc_model.cpp:258` - are commented-out dead code, not real usage, and
weren't counted as justification.) `G2API_AttachEnt` (cross-*entity*
attachment via `s.boltInfo`, used only by `g_utils.cpp` for temp entities/
effects) is a different, narrower mechanism with no evidence it affects any
of this checkout's fixed spawn-camera test scenes, and is still deliberately
left a stub.

**Real rd-vanilla mechanism ported** (`G2_API.cpp`'s `G2API_AttachG2Model`/
`G2API_DetachG2Model`, `ghoul2/G2.h`'s bit-packing constants,
`tr_ghoul2.cpp`'s main render dispatch loop that actually consumes the
link): `CGhoul2Info::mModelBoltLink` bit-packs a *bolt index* and a *sub-
model index* - both into the same entity's own `ghoul2` vector - as
`(toModel << MODEL_SHIFT) | (toBoltIndex << BOLT_SHIFT)`, using the real
`MODEL_WIDTH=10`/`BOLT_WIDTH=10`/`BOLT_SHIFT=0`/`MODEL_SHIFT=10` constants
copied verbatim (now shared, not duplicated, between the encode and decode
sides via `tr_local.h`'s `kG2ModelWidth`/`kG2BoltShift`/etc.). At render
time, for every sub-model *other than the first* (rd-vanilla never applies
this to a model's own root/body sub-model, even if some caller mistakenly
set its link), a set `mModelBoltLink != -1` replaces that sub-model's normal
fixed root-bone seed matrix (`s_g2RootRotation` - real rd-vanilla's own
`identityMatrix` constant, despite the misleading name; not an identity
matrix at all, but a fixed 90-degree axis-swap rotation) with the *target*
sub-model's own current bolt matrix instead, dispatching to the same bone-
bolt vs. surface-bolt matrix computation `G2API_GetBoltMatrix` already uses
(`VK_GetGhoul2BoneCurrentPoseMat` or `VK_GetGhoul2SurfaceBoltMatrix`,
depending on whether `mBltlist[boltNum]` names a bone or a `*`-prefixed tag
surface - see "Ghoul2 surface bolts" above). The effect: an attached
sub-model's entire skeleton pose is built starting from wherever the bolt on
its target currently is, every frame, rather than from a fixed world-space
root - a held saber genuinely follows the hand bone through the player's
current animation instead of floating at a fixed offset from the entity
origin.

**Implementation** (three files): `G2API_AttachG2Model`/
`G2API_DetachG2Model` (tr_init.cpp) now really encode/clear
`mModelBoltLink` instead of unconditionally returning `qfalse`/failing
silently (`AttachG2Model` still validates `toBoltIndex` names a real,
used bolt slot on the target first, matching rd-vanilla's own guard).
`VK_ComputeGhoul2BoneRecursive`/`VK_ComputeGhoul2Pose` (tr_model.cpp) gained
a `rootBase`/`attachBase` parameter (defaulting to `nullptr`, i.e. the
normal fixed seed, for every existing call site) so the root-bone case can
seed from an arbitrary matrix instead of always `s_g2RootRotation`.
`VK_DrawGhoul2Entities`'s per-sub-model loop decodes `mModelBoltLink` (when
set, and not sub-model 0), resolves the sibling's current bolt matrix, and
passes it as that call's `attachBase` argument.

**Verified**: full SP scene suite clean, no crashes, warning-free rebuild.
Pixel-diffed a build with this fix against one without it: all 5 scenes
stayed within this renderer's already-documented per-run noise floor
(hoth2's usual snow-particle scatter, nothing structural) - expected, and
not by itself meaningful confirmation, since none of this checkout's fixed
spawn-camera captures happens to catch a moment where a saber is actually
drawn (raising a saber is a player action, not something that happens
automatically at spawn). This feature is therefore verified for code
correctness and genuinely-exercised real-game-code call sites (above), but
**not independently confirmed by eye** against a visible "saber now follows
the hand" effect the way earlier features (e.g. flares, rgbGen const) were
confirmed via a large, correctly-shaped diff - an honest gap consistent with
how surface bolts' own *matrices* (as opposed to their resolution) were
left unconfirmed by eye in the previous section, for the same underlying
reason: this checkout's fixed test scenes don't happen to exercise the
downstream visible effect.

## Ghoul2 per-level animation-file overrides (`SetAnimIndex`/`GetAnimIndex`)

Closes another real gap this file's own "not implemented" list carried:
"every model always uses whichever single `.gla` `VK_LoadGhoul2Skeleton`
first resolved for it." Confirmed real, and unusually heavily exercised:
`bg_panimate.cpp` calls `gi.G2API_SetAnimIndex(&gent->ghoul2[gent-
>playerModel], curAnim.glaIndex)` on **every single torso/legs animation
change for every player and NPC in the game** - not a rare edge case at
all, just one whose *value* (`curAnim.glaIndex`) is almost always 0 (the
base `.gla`) and so, until now, silently matched this renderer's existing
always-use-the-default-skeleton behavior by coincidence. It stops being a
coincidence for any animation that came from a **per-level "cinematic"
animation-file override** - confirmed real and present for *every one* of
this checkout's 4 fixed test maps: `_humanoid_academy1.gla`,
`_humanoid_hoth2.gla`, `_humanoid_vjun1.gla`, `_humanoid_yavin1.gla` all
ship in the real game data alongside the base `_humanoid.gla`, each holding
extra animations - not present in the base file - that a level's own
scripted cutscene NPCs specifically need (`NPC_stats.cpp`'s
`G_ParseAnimFileSet`, called once per level for the `_humanoid` skeleton
family).

**Real rd-vanilla mechanism**: `CGhoul2Info::animModelIndexOffset` (already
a real field on the shared struct, just never read here before) is an
offset added directly to a model's own default `.gla`'s registration handle
(`ghlInfo->currentModel->mdxm->animIndex + animModelIndexOffset`,
`G2_API.cpp`) to land on whichever `.gla` was registered that many calls
later - concretely 0 for the base file, 1 for that same map's own
cinematic-override file, since `G_ParseAnimFileSet` always precaches them
back-to-back and (real code, with its own comment flagging the fragility:
"double check this always comes first!") **asserts** the second call's
handle is exactly the first call's plus one.

**Implementation - and a real bug found and reverted during its own
verification**: this renderer doesn't need real vanilla's literal shared
`qhandle_t` space to reproduce the same behavior - `VK_LoadGhoul2Skeleton`
is already a name-keyed, idempotent loader, so a small parallel handle
array on top of it is enough. The first version of this fix, however,
registered a `.glm`'s own default `.gla` into the *same* handle sequence
`G2API_PrecacheGhoul2Model` hands out (mirroring how real vanilla's
`RE_RegisterModel` genuinely is one shared global handle space for every
model of every kind) - and that broke the real invariant above: confirmed
via an actual crash on academy1's own regression run, where humanoid
player/NPC models (`luke`, `kyle`, `rebel_pilot` - all defaulting to
`_humanoid.gla`) and the wholly unrelated `protocol` droid (its own
`protocol.gla`) all load during the UI menu-precache phase, *before* Game
Initialization ever runs `G_ParseAnimFileSet` - `protocol.gla` claimed a
handle in between `G_ParseAnimFileSet`'s two back-to-back precache calls,
and the real assert fired, aborting the game DLL (the render-regression
harness then hung waiting on a screenshot that would never come, since the
child process had already crashed - not an infinite loop in this
renderer's own code). **Fixed by keeping the handle space genuinely
isolated**: `VK_PrecacheGhoul2AnimHandle` (the "find or create" side) is
now called *only* from `G2API_PrecacheGhoul2Model` (tr_init.cpp), exactly
matching which real vanilla code path ever calls
`RE_RegisterModel`-for-a-`.gla` for the `_humanoid` family in practice -
`VK_LoadGhoul2Model` no longer registers into it at all, instead keeping
only its own default `.gla`'s *name* (`VulkanGhoul2Model::baseAnimName`).
At pose-resolve time, `VK_ResolveGhoul2SkeletonIndex` looks that name up in
the handle space *on demand* (`VK_FindGhoul2AnimHandle`, lookup-only, never
creates an entry) rather than relying on a handle baked in once at
`.glm`-load time - correct regardless of load order, since a model may load
long before anything ever explicitly precaches its skeleton's name (exactly
what happens during the UI phase above). `G2API_SetAnimIndex`/
`GetAnimIndex` themselves just read/write the existing
`animModelIndexOffset` field; `VK_ResolveGhoul2SkeletonIndex` is the only
place that acts on it, threaded through every function that computes a
*live* pose (`VK_GetGhoul2BoneCurrentPoseMat`, `VK_GetGhoul2SurfaceBoltMatrix`,
`VK_GetGhoul2NumFrames`, and `VK_DrawGhoul2Entities`'s main per-sub-model
draw loop) - deliberately *not* `VK_GetGhoul2BoneBasePoseMat`, since a
per-level override `.gla` shares its base file's bone hierarchy/rest pose,
only adding extra animation frames.

**One known, honestly-documented gap**: real rd-vanilla also clears every
bone slot's `BONE_ANIM_BLEND`-family flags when the index actually changes,
so a blend-in-progress on some other bone doesn't carry a stale flag across
the switch. Not replicated here - this renderer's simpler per-instance
animation state (`VulkanGhoul2AnimState`) doesn't track that flag the same
way, and per `bg_panimate.cpp`'s own call pattern, the very next call after
`SetAnimIndex` is always a fresh `SetBoneAnimIndex` on that same bone
anyway.

**Verified**: full SP scene suite clean (including the specific academy1
crash found and fixed above), warning-free rebuild. Pixel-diffed a build
with this fix against one without it: all 5 scenes stayed within this
renderer's already-documented per-run noise floor - expected, since
`curAnim.glaIndex` is 0 (matching this renderer's pre-existing default-
skeleton behavior) for ordinary spawn-camera idle/movement animations on
every one of this checkout's fixed test scenes; only a cutscene-exclusive
animation would ever resolve to the override file, and none of the 4 fixed
spawn captures happens to catch one playing. Confirmed via load-time log
lines that every one of academy1/hoth2/vjun1/yavin1's own cinematic-
override `.gla` genuinely loads without error (`_humanoid_academy1.gla: 53
bones`, etc., matching the base file's own bone count exactly, as expected
for a shared-hierarchy animation-only override) - not independently
confirmed by eye against a visible cutscene-animation effect, for the same
reason surface bolts' and model-to-model attachment's own downstream
effects weren't (see those sections above): this checkout's fixed test
scenes don't happen to exercise it.

## Ghoul2 bone-angle overrides (`SetBoneAngles`/`SetBoneAnglesIndex`)

Closes another real gap this file's own "not implemented" list carried
since the original animation work: "bone-angle overrides/ragdoll/IK (a
separate, still-real scope cut - see `G2API_SetBoneAngles*`'s stubs)".
Confirmed real, and more heavily exercised than almost anything else in
this list: `bg_pmove.cpp` calls `G2API_SetBoneAnglesIndex` on
`footLBone`/`footRBone` to align each foot to the ground slope during
ordinary movement, and `g_client.cpp`'s ~250-line head/torso "look" system
calls it on `craniumBone`/`cervicalBone`/`thoracicBone`/`upperLumbarBone`/
etc. to turn a player or NPC's head and torso toward whatever they're
looking at - both running essentially every single simulation frame for
every player and NPC in the game, not a rare or edge-case call at all.
Turret/emplaced-gun aiming (`g_turret.cpp`, `g_emplaced.cpp`) and
interrogation-droid limb targeting (`AI_Interrogator.cpp`, `AI_Droid.cpp`)
use the same mechanism. A grep across every real `G2API_SetBoneAngles(
Index)` call site in `code/game`/`code/cgame` turned up exactly one flags
value: `BONE_ANGLES_POSTMULT` - never `BONE_ANGLES_PREMULT` or
`BONE_ANGLES_REPLACE` (both real rd-vanilla features, `G2_bones.cpp`), so
- this renderer's usual evidence-scoped approach - only POSTMULT is
implemented; the other two stay stubs rather than guess at unexercised
behavior.

**Real rd-vanilla mechanism ported** (`G2_Generate_Matrix`, `G2_bones.cpp`;
its POSTMULT consumption in the main per-bone transform, `tr_ghoul2.cpp`):
a bone-angle override is a fixed rotation matrix, computed once (not
re-derived every frame) from a caller-supplied Euler angle triple and three
`Eorientations` values (`up`/`left`/`forward`) that remap which of the
triple's components map onto which local axis - real bones don't all point
the same way relative to their own name's intuitive "forward", so different
callers pick different remaps for the same angle data. The remapped angles
become a rotation matrix (`AnglesToAxis`, the same helper this renderer
already uses for `G2API_GetBoltMatrix`'s world-matrix construction), then
that rotation gets converted into the bone's own local coordinate frame via
a similarity transform - `BasePoseMat * rotation * BasePoseMatInv` - the
same "conjugate by bind pose" pattern already used for surface bolts (see
that section above), just applied to a caller-supplied rotation instead of
a resolved tag-triangle basis. `BasePoseMatInv` (`mdxaSkel_t::BasePoseMatInv`,
`mdx_format.h`) is a *real field the `.gla` file already carries* - "the
inverse, to save run-time calc," per the format's own comment - not
something this renderer derives at load time; `VulkanBone` just needed to
also keep it (previously only `BasePoseMat` was kept). At render time, this
precomputed matrix post-multiplies the bone's normal animated matrix -
exactly matching real rd-vanilla's per-bone transform structure, where the
`BONE_ANGLES_POSTMULT` check runs unconditionally after the normal
parent-times-delta composition, regardless of whether that bone is a root
or has a parent.

**Implementation** (three files): `VulkanBone` gained `basePoseMatInv`
(tr_model.cpp, populated in `VK_LoadGhoul2Skeleton`, straight off the same
`mdxaSkel_t` the existing `basePoseMat` already comes from - no new parsing
needed, just reading a field that was already being skipped over).
`VK_SetGhoul2BoneAngles` computes and stores the override matrix into a
new, deliberately separate `(CGhoul2Info*, boneIndex)`-keyed map
(`s_ghoul2AngleOverride`) - not folded into the existing
`VulkanGhoul2AnimState`/`s_ghoul2AnimState` used for animation tracks, even
though real rd-vanilla's own `boneInfo_t` stores both concerns in one
struct, since this renderer's animation-state struct already exists purely
for that one purpose and a bone can have an animation track, an
angle-override, both, or neither, completely independently either way.
`VK_ResolveGhoul2BonePose` (already the single place per-bone pose data
gets assembled before the hierarchy walk) now also looks up this map -
by exact bone index, deliberately *not* inherited up the parent chain the
way an animation track is, matching real rd-vanilla's own indexed
`boneList[boneListIndex]` lookup. `VK_ComputeGhoul2BoneRecursive` applies
the stored override as a post-multiply immediately after its existing
root/non-root composition, mirroring the real code's own control-flow
shape. `G2API_SetBoneAngles`/`SetBoneAnglesIndex` (tr_init.cpp) are now
real thin wrappers instead of stubs; the By-name variant reuses
`VK_ResolveGhoul2AnimBone` (the exact same bone-name resolution
`G2API_SetBoneAnim`'s own By-name variant already relies on). `blendTime`
is accepted but ignored - no real call site above ever passes a nonzero
one for an angle override (unlike `SetBoneAnim`'s `blendTime`, which real
code relies on constantly), an honestly-documented, evidence-scoped gap
rather than a silent drop of something actually used.

**Verified, and unusually well**: full SP scene suite clean, warning-free
rebuild. Pixel-diffed a build with this fix against one without it: 4 of 5
scenes stayed within the usual per-run noise floor, but **vjun1 produced a
real, structural, correctly-shaped diff** - two clear humanoid silhouettes,
not particle scatter - and inspecting the actual before/after screenshots
directly confirms it: the Twi'lek NPC seated in vjun1's cockpit cutscene
visibly changed head/torso orientation between the two builds, exactly the
kind of effect this feature's real mechanism (head/torso "look" bone-angle
overrides, `g_client.cpp`) predicts. Unlike several recent Ghoul2 features
in this file (surface bolts' matrices, model-to-model attachment, per-level
animation overrides), this one's downstream visual effect *is*
independently confirmed by eye on a real fixed test scene, not just
verified for code correctness and exercised call sites.

## `tcMod scale` for world geometry

Closes another real gap in this renderer's `.shader` coverage: `tcMod
scale <sx> <sy>` (a constant multiplier on a stage's diffuse UV) was
previously one of the "every other tcMod type... deliberately left
unconsumed" this file's own comments flagged. Confirmed real and, after
`tcMod scroll`, this checkout's single most common `tcMod` keyword: 453
occurrences across the real `.shader` files this checkout ships, real and
currently-visible on multiple surfaces across the test maps (hoth2's
`textures/hoth/rock_huge_snow`/`textures/hoth/snow_02` at `tcMod scale 0.5
0.5`, `textures/hoth/at_at_leg` at `tcMod scale 4 4`).

**Why this one is simpler than `tcMod scroll`**: real Quake3 recomputes a
stage's whole `tcMod` matrix stack every frame regardless of whether any
individual step is actually time-varying, but `scale`'s own contribution to
that matrix never changes over time - so, unlike `scroll` (which genuinely
needs a live per-frame value), baking `scale` in once produces
pixel-identical output to recomputing it every frame forever. The one real
complication: **179 real stages in this checkout's shader files declare
both `tcMod scale` and `tcMod scroll` on the same stage**, and Quake3's
`tcMod` keywords compose as an ordered matrix stack - whichever is declared
first transforms the *output* of whichever came before it. `scale` then
`scroll` leaves the scroll offset unscaled; `scroll` then `scale` scales
the moving part too (confirmed both orders appear in real shipped shaders,
not a hypothetical edge case - e.g. `hoth.shader`'s `stunpass_rotated`
stages use `scale` then `scroll`, `weapons.shader`'s `rifle_energy3` uses
`scroll` then `scale`). Getting this backwards doesn't crash or look
obviously broken - it just makes an animated texture scroll at the wrong
apparent speed relative to its own tiling, a subtle, easy-to-miss
correctness bug rather than a loud one.

**Implementation**: `ParseShaderFile` (tr_shader.cpp) now tracks
declaration order between `tcMod scale` and `tcMod scroll` on a stage
(`tcModScaleBeforeScroll`), and - rather than carrying that order flag
through to render time - bakes its effect into the *stored* scroll speed
once, at parse time: if scroll was declared first, the stored speed is
pre-multiplied by the scale factor, so world.vert's render-time formula
stays the same simple `uv * scale + speed * time` regardless of which
order a given shader actually used (see `vkShaderTcModScroll_t`'s own
comment for the full derivation). `VK_GetShaderTcModScroll` now returns
`scaleS`/`scaleT` alongside the existing `sSpeed`/`tSpeed`. `WorldSurfaceBatch`
gained `scaleS`/`scaleT` (default 1.0, appended as the struct's last fields
specifically so every existing positional-aggregate-init call site - sky
faces, real BSP surfaces - keeps compiling unchanged and still gets the
correct identity default without needing to be touched). `vkWorldPushConstants_t`
gained a `uvScale` vec4 (using the struct's 4th 16-byte slot, bringing the
push constant to exactly 128 bytes - the Vulkan spec's guaranteed minimum
for every conformant implementation); world.vert multiplies the diffuse UV
by it before adding the existing scroll offset. **A real pitfall caught
before it shipped**: a missing `uvScale` init at any push-constant
construction site silently multiplies every UV to `(0,0)` (same failure
shape as the pre-existing `camPos[3]` overbright-factor trap this file's
own comment already warns about) - not a crash, a silently-wrong or
all-black texture - so every one of this renderer's four `vkWorldPushConstants_t`
construction sites (world surfaces, sky, static `.md3` models, Ghoul2
models) was audited and explicitly set to the `1.0,1.0` identity, not left
to rely on zero-init.

**Verified**: full SP scene suite clean, warning-free rebuild. Pixel-diffed
a build with this fix against one without it: all 5 scenes stayed within
this renderer's already-documented per-run noise floor - not, on its own,
strong confirmation, since none of the four fixed spawn-camera captures
happens to frame one of the confirmed real `tcMod scale` textures large
and dominant in view (hoth2's spawn point faces open sky and distant flat
terrain, not the nearby rock faces or AT-AT leg texture that actually use
it) - an honest gap consistent with several other recent features in this
file whose real effect isn't independently confirmed by eye in these
particular fixed scenes, even though the underlying mechanism, real
exercised shader data, and order-of-composition math are all directly
verified.

## Real rgbGen const/wave for flares (a genuine mis-assumption found and fixed)

Not a new feature so much as a correction to an existing one: "Static
flares" above documented every flare quad as always using a fixed
view-angle-fade colour (`d,d,d`, `d = -dot(viewDir, normal)`), with a
comment claiming "neither [`rgbGen const` nor anything else] appears on any
of this checkout's 3 real flare shaders." That claim was wrong, and
re-reading real rd-vanilla's actual flare rendering shows why: hoth2's
`textures/flares/flare_blue_pulse` (55 of hoth2's 98 real flare surfaces,
*more than half*) declares `rgbGen wave sin 0.5 1 0.2 0.5`, and vjun1's
`textures/flares/flare_bluehue` (29 of vjun1's 45) declares `rgbGen const (
0.784314 0.843137 0.917647 )` - both majority cases on their respective
maps. Only the third real flare shader, `gfx/misc/flare` (an implicit,
`.shader`-script-less texture reference resolving to `rgbGen vertex`), is
actually correct with the fade-only assumption - real rd-vanilla's own
`RB_SurfaceFlare` (`tr_surface.cpp`) only ever writes the fade colour into
the tessellation buffer's per-vertex colour *input*; whether that input
ever reaches the screen depends entirely on the shader's own `rgbGen`,
evaluated generically downstream by the same `RB_IterateStagesGeneric`/
`RB_CalcWaveColor` (`tr_shade.cpp`/`tr_shade_calc.cpp`) every other surface
goes through - `rgbGen vertex` reads it back out, but `rgbGen const`/
`rgbGen wave` *replace* the colour outright, discarding the fade entirely,
regardless of what `RB_SurfaceFlare` computed. A real, previously-
undiscovered gap, not a hypothetical one: two of this checkout's three real
flare shaders - the majority-usage ones on both maps that have flares at
all - were rendering the wrong colour.

**Real formula ported** (`EvalWaveFormClamped`/`WAVEVALUE`,
`tr_shade_calc.cpp`): `glow = clamp(base + amplitude * sin(2*pi*(phase +
frequency*time)), 0, 1)`, applied identically to all three colour channels
(alpha untouched - real `rgbGen wave`/`const` never touch alpha; that's
`alphaGen`'s job, already separately handled for flares via `alphaGen
portal`). Only the `sin` wave function is implemented - a grep across every
first-stage `rgbGen wave` in this checkout's real flare shaders found
`sin` and nothing else (`square`/`triangle`/`sawtooth`/`inverseSawtooth`/
`noise` are all real rd-vanilla features with zero exercised callers here),
so - this renderer's usual evidence-scoped approach - only that one is
implemented; a non-`sin` wave function is a silent no-op, falling back to
the existing fade-colour default, not a guess at unexercised behaviour.
`rgbGen const` needed no new formula, just a new caller: `VK_GetShaderRgbGenConst`
already existed for ordinary world surfaces (see "`rgbGen const` and a
widened additive map-image fallback" above) and is now also checked for
flares.

**Implementation**: `WorldFlare` gained a `shaderName` field (the flare's
real shader name, captured once at `RE_LoadWorldMap` load time - not
necessarily the same name as its resolved image, per that section's own
comment on why). `VK_DrawWorldFlares` now checks `VK_GetShaderRgbGenConst`
first, then the new `VK_GetShaderRgbWave` (`sin`-only, tr_shader.cpp), and
only falls back to the original view-angle fade if a flare's shader
declares neither - correct for `gfx/misc/flare`'s real `rgbGen vertex` and
for any flare shader with no `rgbGen` keyword at all.

**Verified, and confirmed by eye**: full SP scene suite clean, warning-free
rebuild. Pixel-diffed a build with this fix against one without it: hoth2
produced the largest diff of any scene in this feature's history so far
(1.27% mean difference) - and inspecting the actual before/after
screenshots directly confirms it's real, not noise: the row of landing-
beacon flares along the horizon are visibly, distinctly brighter/glowing
in the "after" capture, caught mid-pulse at a bright point in their real
sine cycle instead of their previous dim, static fade colour. academy1/
yavin1 (zero real flare surfaces - see "Static flares" above) and menu
stayed pixel-identical, exactly as expected.

## `tcGen environment` (reflection-mapped UV generation)

Closes a real, previously-flagged gap: this renderer detected `tcGen`'s mere
*presence* on a shader's first stage (`VK_ShaderHasTcGen`, used only as a
safety gate for the map-image fallback - see "`rgbGen const` and a widened
additive map-image fallback" above) but never actually generated the
reflection-vector UVs `tcGen environment` calls for.

**Real per-map usage, checked the same way as every other feature in this
file** (cross-referencing shader script `tcGen environment` hits against
each test map's own real `LUMP_SHADERS` string table, not just "does this
shader exist somewhere in the asset library"): 22 real hoth2 first-stage
matches (`textures/hoth/basicltgray_shiny`, `blast_panel*`, `door_02*`,
`exit_beam*`, `h_wall_*`, `h_door*`, `h_floor_02/03`, `trim_01`) and 6 real
vjun1 matches (`textures/common/env_glass`, `glass_security_chain/hex/
square/tris`, `textures/imperial/square`) - all genuine world architecture
(shiny metal walls, blast doors, security glass), not UI/MP-only shaders
like the `tcMod rotate` investigation above ruled out.

**A critical wrinkle found before writing any rendering code**: every one of
those 28 real shaders is a real Quake3 "shiny surface" 3-stage composite -
stage 1 `map <genericReflectionTexture>` + `tcGen environment` (no
`blendFunc`, so it's an opaque replace), stage 2 `map <realBaseTexture>`
blended on top (`GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA`), stage 3 `map
$lightmap` multiply - except for the 6 vjun1 glass shaders, whose stage 1
uses `blendFunc GL_ONE GL_ONE` instead and has no stage-2 base texture at
all (real translucent glass, not a shiny-metal highlight). This renderer
only ever draws a world surface's first stage - `img = VK_FindImage(shader
name)` - which, for 23 of these 28 shaders, means the shader's own name
*already* coincidentally matches a real base-texture file (`textures/hoth/
h_wall_04b` the shader is also literally `textures/hoth/h_wall_04b.png` the
file - checked directly against the real `.pk3` file listing, not assumed):
this renderer has been drawing those 23 surfaces correctly all along, with
their real *second*-stage base texture and ordinary baked UVs, entirely by
name-coincidence, never touching the first stage's `tcGen environment` at
all. Only the remaining 5 (`textures/hoth/basicltgray_shiny`, whose real
base texture is a *different*-named file, `textures/cairn/basicltgray`) and
the 5 vjun1 glass shaders (no second stage/base texture to name-match at
all) actually depend on the map-image fallback (`VK_GetShaderMapImage`,
which only ever records the *first* stage's `map`) - and for those, the
fallback was previously either resolving the *wrong* image with *wrong*
(ordinary baked) UVs (`basicltgray_shiny`, sampling a generic reflection
texture as if it tiled normally), or not resolving anything at all
(`env_glass`/`glass_security_*`, blocked outright by the existing `tcGen`
safety gate - see "`rgbGen const`..." above - so these 5 were completely
invisible). Naively applying reflection UVs to whatever image a shader's
resolution already lands on would have been a **regression** for the other
23 real surfaces (replacing their already-correct ordinary-UV rendering
with wrong reflection UVs sampled against their real base texture) - the
fix is gated specifically on "did this batch's image come from the
map-image fallback, and is that fallback's shader's first stage genuinely
`tcGen environment`" (`RE_LoadWorldMap`'s `envMap` local, `WorldSurfaceBatch::
envMap`), never on "does this shader mention `tcGen environment` anywhere,
regardless of which stage is actually being drawn".

**The real formula**, ported directly from rd-vanilla's own
`RB_CalcEnvironmentTexCoords` (tr_shade_calc.cpp), not reconstructed from
memory:
```
viewer = normalize(cameraWorldPos - vertexWorldPos)
d = dot(normal, viewer)
st = (normal.x*d - 0.5*viewer.x, normal.y*d - 0.5*viewer.y)
```
Notably: only x/y ever feed the result (z is untouched in the real code,
not a simplification made here), and there's no remap to a 0..1 range - the
real function doesn't do one either, relying on the texture sampler's wrap
addressing to tile the roughly-[-1,1] values this produces. The `viewer`
term uses `backEnd.ori.viewOrigin`/`tess.xyz` in real rd-vanilla, which for
ordinary (non-first-person-viewmodel) world geometry are the raw world-
space camera position and vertex position - world surfaces have no
per-entity transform in this renderer either, so `pc.camPos.xyz`/`inPos` are
already in the same space, no rotation needed first. The `RF_FIRST_PERSON`
branch (a light-direction-based variant for first-person view models) has
no real world-BSP-surface user and isn't ported.

**Implementation**: `WorldVertex` (tr_local.h) gained a `normal[3]` field -
previously entirely absent, despite the real BSP's `drawVert_t` already
carrying one right next to `xyz`/`st`/`lightmap` - populated straight from
the BSP at load time for every vertex (cheap, and simpler than special-
casing which surfaces need it), plus a matching new vertex attribute
(location 4) in `VK_CreateWorldPipeline` (tr_init.cpp). `tr_shader.cpp`'s
`tcGen` parsing now also captures the type argument (previously discarded)
into a new `VK_GetShaderTcGenEnvironment` accessor, distinct from the
existing presence-only `VK_ShaderHasTcGen`. `WorldSurfaceBatch` gained an
`envMap` bool (appended last, default-initialized `false`, same positional-
aggregate-init reasoning as `scaleS`/`scaleT` above), set true only in the
narrow fallback case described above. The actual UV generation happens in
`world.vert`, not on the CPU: it's inherently per-frame/view-dependent
(the camera moves), so it's computed per real vertex every frame exactly
like real rd-vanilla's own per-vertex tess loop, gated by a new push-
constant flag (`uvScale.z`, reusing what was previously unused padding -
see `vkWorldPushConstants_t`'s comment) that RE_RenderScene sets per-batch
alongside the existing scroll/scale/fog state changes.

**Verified**: full SP scene suite clean, no crashes, warning-free rebuild
(new vertex attribute compiled and linked correctly via glslangValidator).
A temporary debug print (removed before committing) confirmed programmatically,
not just visually, that exactly the intended shaders take the new code path
on each real map and no others: `textures/common/env_glass` +
`textures/hoth/basicltgray_shiny` on hoth2; `textures/common/env_glass` and
all 4 `glass_security_*` variants on vjun1; zero hits on academy1/yavin1
(neither has any real matching surface, matching the BSP cross-reference
above exactly). The other 23 real matching surfaces were confirmed to
*not* take this path (still resolving their base texture directly by name,
exactly as before) - the specific regression risk identified above.
Pixel-diffed a build with this fix against one without it, stash-based:
academy1/yavin1 came back pixel-identical (no real matching surfaces in
view, as expected). hoth2 and vjun1 showed measurable diffs, but this
project's own render-regression suite has an existing, separately-confirmed
per-run noise floor from time-varying weather/particle effects on these two
maps specifically (confirmed here by diffing two runs of the *same*
unchanged code against each other: hoth2 alone varies by 47.6% of pixels/
1.89% mean run-to-run from snow-particle placement) large enough to swamp
this feature's own signal in a whole-frame pixel diff - neither fixed test
camera happens to frame one of the affected surfaces closely enough for an
unambiguous whole-frame before/after comparison the way hoth2's flares or
academy1's light-shaft dust did. A manually-positioned camera (real BSP
vertex coordinates read directly from the compiled level, `setviewpos`) at
one of vjun1's cockpit windows (`glass_security_tris`) and near one of
hoth2's staircase treads (`basicltgray_shiny`) didn't land a clean, large,
obviously-different framing either - the debug-log confirmation above is
the honest evidence for this feature, not an eyeballed screenshot. Ruled
out as an implementation bug rather than an inherent limit of the fixed
test cameras: not attempted further, given the existing per-map evidence
this is a small, cheap, low-risk correctness fix in its own right (removes
a wrong-static-texture bug for `basicltgray_shiny` and an outright-invisible
bug for `env_glass`/`glass_security_*`, without touching any of the 23
already-correct surfaces).

## `RE_DrawRotatePic`/`RE_DrawRotatePic2` (rotated 2D pics)

Closes a real gap: both were stubs that silently dropped the rotation angle
and fell back to an ordinary unrotated `RE_StretchPic`, so any caller
relying on the rotation would draw its quad in the wrong place/orientation
rather than crash or look obviously broken - the "silently wrong, not
loud" failure shape this project has hit before (`camPos[3]`/`uvScale`, see
their own comments).

**Real callers, checked directly rather than assumed unused**: three call
sites across the SP `cgame` module, all real gameplay HUD elements, not
menu/UI-script driven: the seeker-missile lock-on warning (`cg_draw.cpp`,
`CG_DrawRotatePic` - up to 8 `gfx/2d/wedge` slices stepped 45 degrees apart,
building up as a seeker missile's lock gets closer) and the Disruptor
rifle's zoomed-scope overlay (`CG_DrawRotatePic2`, twice - the full-screen
`disruptorInsert` reticle graphic rotated by the current zoom level, plus a
ring of small `disruptorInsertTick` ammo-count marks each rotated to face
outward around a circle). All three are real, substantial, visually
distinct uses - not a corner case.

**Why this couldn't be screenshot-verified the way most other features in
this file are**: all three are gated on a transient gameplay state (an
active seeker-missile lock, or actually being zoomed in with the Disruptor
equipped) that this renderer's fixed test harness - `+devmap <map>` then a
timed wait then a screenshot, no live input - never reaches; none of the 4
regression scenes' spawn-camera screenshots call either function at all,
so a before/after pixel diff of those scenes is guaranteed pixel-identical
regardless of whether this fix is correct (confirmed: full regression suite
unaffected, all 5 scenes identical). Attempted to force the state directly
for a real screenshot (`+give all`/`+weapon 4`/`+altattack` via the same
`setviewpos`-style manual console-command approach used for `tcGen
environment` above) - `+altattack` reports "Unknown command" this early in
the real client's command-buffer sequence in this headless configuration
(input command registration ordering, not a renderer issue), and forcing
`cg.zoomMode` any other way needs real per-frame held-button `usercmd`
simulation this harness has no support for. Not pursued further given the
verification basis below is already strong for a self-contained, formula-
exact port.

**Real formula**, ported directly from rd-vanilla's `RB_RotatePic`/
`RB_RotatePic2` (`tr_backend.cpp`), algebraically re-derived and checked
term-by-term against the source rather than approximated: both build a 2x3
rotation-plus-translation matrix (`c,s,0 / -s,c,0 / tx,ty,1`, `c`/`s` =
`cos`/`sin` of the angle in radians) and apply it to the quad's 4 corners.
`RotatePic` pivots around the corner `(x+w, y)` (vertex 1 always lands
exactly on the pivot; angle 0 reproduces `RE_StretchPic`'s own 4 corners
exactly, confirmed by substitution). `RotatePic2` pivots around the quad's
own *center* `(x, y)` instead, with corners offset by `±w/2`/`±h/2` before
rotation - the natural convention for something that should visually spin
in place, like the disruptor scope.

**Implementation**: `RE_StretchPic` (`tr_cmds.cpp`) already built its 6
vertices from 4 corner positions and pushed them through one small,
self-contained block (pipeline selection by blend mode, descriptor bind,
push-constant color, draw call) - refactored that block out into a new
`VK_DrawQuad( x0,y0,u0,v0, x1,y1,u1,v1, x2,y2,u2,v2, x3,y3,u3,v3, hShader )`
taking 4 arbitrary corners instead of an implicit axis-aligned rect, with
`RE_StretchPic` itself becoming a 1-line caller. `RE_DrawRotatePic`/
`RE_DrawRotatePic2` (`tr_init.cpp`) compute the real rotated corners per
the formulas above and call the same `VK_DrawQuad` - no new pipeline, no
new vertex format, this is a pure geometry change reusing 100% of the
existing 2D UI draw path.

**Verified**: warning-free rebuild, full SP scene suite clean (all 5
scenes pixel-unaffected, as expected given none of them call either
function - see above for why). Correctness rests on the exact algebraic
match against real rd-vanilla source (shown above), not a screenshot - the
same verification tier this project has already used elsewhere for a
real, ported-but-unexercised code path (`VK_Ghoul2TimingModel`'s reverse-
speed playback, "Live animation" above: "ported faithfully...but untested
against a real scene, since no real caller in this game currently uses
it" - the same honest standard applied here to a real caller this harness
just can't reach, not "no caller exists at all").

## Real `R_ModelBounds` for static `.md3` models

Closes another real gap this file's own "not implemented" list had carried
as "model bounds/tag queries": `R_ModelBounds` was a stub always returning
`(0,0,0)`-`(0,0,0)` regardless of which model handle was asked about.

**Real caller, confirmed exercised on every one of this checkout's 4 test
maps, not just the one already-documented cockpit case**: `CG_CreateMiscEnts`
(`cg_main.cpp`) calls this for every `misc_model_static` map entity (a real,
common Q3/JKA map-authoring primitive for static decorative props - crates,
terminals, pipes, the vjun1 cockpit interior this file's "vjun1's missing
cockpit" section already covers) to compute a per-entity cull radius
(`DistanceSquared(mins*scale, maxs*scale)`) before `CG_DrawMiscEnts` draws
it. A temporary debug print (removed before committing) confirmed real,
substantial usage directly: 11 real calls on academy1, 191 on hoth2, 164 on
yavin1, 166 on vjun1 - every one returning genuinely nonzero, plausible
bounds after this fix (e.g. `mins=(-55.7 -72.2 -9.8) maxs=(50.1 72.4 34.5)`
for one of hoth2's larger props), not the always-zero this renderer
returned before.

**Why this was never a visible bug on this checkout's own fixed test
cameras, and won't become a visible fix either**: the radius only feeds a
*distance* cull (`VectorLengthSquared(origin - vieworg) - radius <=
8192*8192` - a genuinely enormous ~67 million-unit-squared threshold), and
`gi.inPVS` is unconditionally `qtrue` in this renderer (no real PVS culling
- see "What's not implemented yet" below), so a zero-vs-real radius could
only ever change the outcome for a prop already within a few hundred units
of that threshold's edge - never the case for any prop on any of these 4
maps' fixed spawn cameras. This is a genuine correctness fix for the
underlying API regardless (the *value* `R_ModelBounds` reports was simply
wrong for every real caller, on every map, until now), just not one this
harness's specific camera placements could ever make visibly different -
same honest category as the `RE_DrawRotatePic`/`RE_DrawRotatePic2` fix
directly above.

**Real formula**, ported from rd-vanilla's own `R_ModelBounds`
(`tr_model.cpp`): frame 0's `md3Frame_t::bounds[0]/[1]` - already real-world
float units (unlike a surface's `xyz`/`normal` vertex data, which needs
`MD3_XYZ_SCALE`) - read once at `VK_LoadMD3Model` load time (before the raw
file buffer is freed) and cached on `VulkanStaticModel` for `R_ModelBounds`
to hand back later by handle. Real vanilla's own function also handles two
other cases - a genuine Ghoul2 `model_t` (falls through to zero, since its
`md3[0]` pointer is null - checked directly in the source) and a BSP inline
submodel (`*N`, real brush bounds) - the Ghoul2 case needed no new code
here (this renderer's stub default already matches that exact real
behaviour), and the inline-submodel case isn't reachable at all, since this
renderer's own `RE_RegisterModel` doesn't recognize a leading `*` and never
registers one in the first place (a separate, pre-existing gap, not
something this fix could address without also implementing brush-model
rendering itself).

**Verified**: warning-free rebuild, full SP scene suite clean (all 5 scenes
pixel-identical, exactly as the distance-cull-threshold analysis above
predicts), plus the direct debug-log confirmation of real nonzero bounds on
every map described above.

## Real `clampmap` addressing for world geometry

Closes a real, silent-wrong-texture bug: this renderer's world sampler was
hardcoded to `VK_SAMPLER_ADDRESS_MODE_REPEAT` for every surface's diffuse
texture, with no distinction for a shader's first stage declaring
`clampmap` instead of plain `map` - real Quake3 gives `clampmap` real
`GL_CLAMP(_TO_EDGE)` addressing instead, and this renderer's own `tr_shader.cpp`
comment had actually mis-described `clampmap` as merely "the legacy synonym"
for `map` (fixed alongside this - it isn't a spelling variant, it changes
real GL wrap behaviour).

**Real, substantial usage, confirmed on every one of this checkout's 4 test
maps via a temporary debug print (removed before committing)**: 71 real
surfaces on academy1, 199 on hoth2, 17 on yavin1, 297 on vjun1 - all real
`textures/common/dark_dust`/`dark_orange`/`tan_gradient`/`blue_gradient`/
`neonblue_gradient`/`gradient2` additive dust-cloud/light-shaft decal
shaders (the same family "`rgbGen const` and a widened additive map-image
fallback" above already found and fixed the *visibility* of - this closes
a second, independent bug in how they render once visible). Every one of
these declares `clampmap textures/common/gradient` (or a same-family
sibling texture) on its first stage.

**Why REPEAT-vs-CLAMP is a real, not theoretical, difference here**: checked
these surfaces' actual baked UV coordinates directly against academy1's real
BSP `LUMP_DRAWVERTS` data, not assumed - they range from -5.6 to 6.5 (U) and
-10.25 to 11.75 (V), i.e. genuinely and substantially outside 0..1. With
REPEAT addressing (this renderer's previous unconditional behaviour), that
tiles the gradient texture 5-11 times across a single surface instead of
real Quake3's CLAMP behaviour of stretching the texture's own edge pixel
across the whole range - a real, structural rendering difference, not a
subtle rounding one.

**Implementation**: a second world sampler (`vk.worldSamplerClamp`,
`VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE`, otherwise identical filtering/LOD
settings to the existing `vk.worldSampler`) - `VK_BuildWorldDescriptorSet`
(tr_world.cpp) gained a `diffuseClamp` parameter (default `false`, so every
non-world caller - sky, Ghoul2, static `.md3` - keeps its previous
unconditional REPEAT behaviour unchanged) and picks between the two
samplers for the diffuse binding only; the lightmap binding always stays
REPEAT (real vanilla never clamps `$lightmap`, and lightmap UVs are baked
within 0..1 by construction anyway, so it wouldn't matter either way).
`tr_shader.cpp`'s existing `map`/`clampmap` parsing block now also records
*which* keyword was used (`VK_GetShaderClampMap`, independent of
`VK_GetShaderMapImage`'s own fallback-resolution logic, since the
addressing mode is a property of the shader regardless of which path
actually resolves its image), and `RE_LoadWorldMap` passes that straight
through to `VK_BuildWorldDescriptorSet` for every world surface.

**Verified**: warning-free rebuild, full SP scene suite clean. The temporary
debug print confirmed the fix is wired correctly and reaches exactly the
intended real shaders on every map (counts above). Pixel-diffed a build
with this fix against one without it: academy1 (which has no weather/
particle system at all, so its diff is 100% attributable to real rendering
changes, not this project's own previously-documented run-to-run noise
floor - see "`tcGen environment`" above) showed a small but genuinely
nonzero, fully deterministic diff (205 pixels, 0.025% of the frame) exactly
where the light-shaft dust surfaces are - real but visually subtle for this
specific camera framing and texture, not a sign the fix only partially
worked. hoth2/vjun1 (199/297 real affected surfaces, the largest counts)
showed larger diffs (32%/6.4%) but neither map's own existing noise floor
(hoth2: ~47.6% run-to-run from snow-particle placement; vjun1: ~4.75%) can
be fully separated from this fix's own real contribution from a whole-frame
diff alone - the debug-log confirmation above, not the screenshot diff, is
the primary evidence for this fix, same as several features above it.

## Real `alphaFunc` alpha-testing for world geometry

Closes a real, substantial, high-visual-impact gap: this renderer ignored
`alphaFunc` entirely, so any world surface relying on it for a hard
per-pixel cutout (rather than blending) rendered as a fully solid rectangle
- most strikingly, real cutout foliage (grass/vines/tree billboards) with
no `blendFunc` at all fell through to this renderer's default
`BLEND_OPAQUE` classification and drew as solid opaque image rectangles
instead of alpha-tested leaf/grass/vine shapes.

**Real, substantial usage, checked the same way as every other feature in
this file** (cross-referencing `alphaFunc` hits against each test map's
real `LUMP_SHADERS` string table): zero on academy1, 3 real grate shaders
on hoth2 (`textures/imperial/grate02`/`grate02_broke`, `textures/vjun/
grate2`), 3 on vjun1 (`textures/imperial/grate02`, `textures/vjun/grate`/
`grate1`), and **9 real matches on yavin1** - a jungle level, so this makes
complete sense once you look: `models/map_objects/yavin/plant`/`grass_b`/
`vines`/`tree09a`/`tree09b`/`tree09d`, `textures/yavin/s_rock1_vines`,
`textures/yavin/ground_grasssprite_phong_vertex`, `textures/factory/
t2_wedge_floorgrate`. Critically, **7 of yavin1's 9** (the foliage family)
declare no `blendFunc` keyword at all in their only stage - just `map` +
`alphaFunc` + `rgbGen lightingDiffuse` - so they were taking this
renderer's plain `BLEND_OPAQUE` path with zero alpha handling of any kind
before this fix, not merely a "soft edges instead of hard cutout"
approximation error.

**Real threshold values**, checked directly against rd-vanilla's own
`tr_backend.cpp` `qglAlphaFunc` calls (not the `GLS_ATEST_*` macro names
alone, which don't say what threshold each one actually uses): `GT0`
discards alpha `<= 0.0`, `LT128` discards alpha `>= 0.5`, `GE128` discards
alpha `< 0.5`, `GE192` discards alpha `< 0.75` - a real hard cutoff via
GLSL `discard`, not blending, matching real vanilla's fixed-function
`GL_ALPHA_TEST` exactly (Vulkan/modern GL has no equivalent fixed-function
stage, so a fragment-shader `discard` is the standard real replacement).

**Implementation**: `tr_shader.cpp`'s parser gained `alphaFunc` recognition
(`VK_GetShaderAlphaFunc`, an `int` mode: 0=none/1=GT0/2=LT128/3=GE128/
4=GE192) alongside the existing `map`/`blendFunc`/etc. keywords.
`WorldSurfaceBatch` gained an `alphaFunc` field (appended last, default 0,
same positional-aggregate-init convention as `envMap`/`scaleS` above) and
`RE_RenderScene`'s existing per-batch push-constant tracking loop gained
`uvScale.w` as the mode selector - reusing what was previously genuinely
unused padding in `vkWorldPushConstants_t` (see that struct's own comment).
`world.frag` performs the actual real per-mode discard, immediately after
sampling the diffuse texture and before any lightmap/fog work, so a
discarded fragment costs nothing beyond that one sample.

**Verified**: warning-free rebuild (including the modified fragment shader,
recompiled via `glslangValidator`), full SP scene suite clean on all 5
scenes. A temporary debug print (removed before committing) confirmed
programmatically that exactly the intended shaders take the new path, with
the exact real mode and existing blend-mode classification each one
actually has: `textures/imperial/grate02` (mode 3/GE128, `BLEND_ALPHA` -
matches its real `blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA`) and
`grate02_broke` (mode 2/LT128, `BLEND_OPAQUE` - matches its real
`blendFunc GL_ONE GL_ZERO`, this renderer's own "blending explicitly
disabled" spelling) on hoth2; `textures/vjun/grate`/`grate1` (mode 4/
GE192, `BLEND_OPAQUE`) plus `grate02` again on vjun1; and, most
importantly, 6 of yavin1's 7 real no-`blendFunc` foliage shaders (`grass_b`/
`plant`/`vines`/`tree09a`/`tree09b`/`tree09d`, all mode 3/GE128,
`BLEND_OPAQUE`) actually visible from that scene's fixed spawn camera.
Pixel-diffed a build with this fix against one without it: academy1 (zero
real matches) came back essentially unchanged (0.027% of the frame,
consistent with unrelated floating-point noise, not a sign anything was
missed). yavin1/vjun1 showed diffs in roughly the same range as this
project's own separately-measured run-to-run noise floor for those two
scenes (re-confirmed here by diffing two runs of the identical fixed
binary against each other: yavin1 varies ~1.8% purely from run-to-run
NPC-pose jitter) - the whole-frame screenshot diff can't cleanly separate
this fix's real contribution from that pre-existing noise in this specific
camera framing, so the debug-log confirmation above (not the pixel diff)
is the primary evidence for this fix, same as several features above it.

## Real `tcMod turb` for world geometry

Closes another real gap, and along the way caught and fixed a genuine
methodology bug in how this project verifies "real per-map usage" when a
shader name is defined more than once.

**The methodology bug, found before implementing anything**: the same
`assets1.pk3` contains *two* different definitions of `textures/
impdetention/deathcon1a` (vjun1's electric containment field, already real
for `tcMod scroll`/`scale` - see "World-geometry tcMod scroll" above) - one
in `imp_mine.shader` with no `tcMod` at all, and one in `test.shader` with
`tcMod scroll`/`scale`/`turb` all three. A shader-name collision like this
resolves to whichever file wins under this renderer's own real
`ri.FS_ListFiles` enumeration order (not a script's `glob()` order, which
has no relationship to it) - so before implementing `tcMod turb` "because a
naive text search found it," a temporary debug print (removed before
committing) confirmed directly which definition *this renderer's own
already-running code* actually resolves: `scrollS=0 scrollT=0.2 scaleS=2
scaleT=2` for `deathcon1a` - which only matches `test.shader`'s numbers
(`0.1*2=0.2` from the real scroll-before-scale composition this project's
own `tcMod scale` work already established), confirming `test.shader`'s
version - the one with `tcMod turb` - is genuinely the one in effect, not a
naive keyword hit that would never actually run.

**Real, substantial usage** (re-confirmed against real per-map BSP data
after the methodology fix): vjun1's `textures/impdetention/deathcon1a`/
`deathcon1` (the electric containment field, `tcMod turb 0 0.1 0 2`/`tcMod
turb 0 0.4 0 1` respectively - a real per-vertex wobble layered on top of
the scroll/scale this renderer already had), `textures/common/
water2_water1_vjun1` (a real water surface, `tcMod turb 2 0.05 0 0.1` on
its own first stage), and `textures/skies/cloudlayer_yavin` (a real yavin1
world surface, not the skybox itself, `tcMod turb 1 0.01 0.1 0.1`). Ruled
out one apparent match as a false positive from this renderer's existing
first-stage-only scope: `textures/h_evil/lakewater`'s `tcMod turb` only
appears on its *second and third* stages - its first stage is plain
`tcMod scroll`, so this shader needed no change at all, same "the
match has to actually land on stage one" discipline the `tcGen environment`
investigation established.

**Real formula**, ported directly from rd-vanilla's own
`RB_CalcTurbulentTexCoords` (`tr_shade_calc.cpp`): `now = phase +
time*frequency`; `s' = s + sin(2*PI*((x+z)/128*0.125 + now)) * amplitude`;
`t' = t + sin(2*PI*(y/128*0.125 + now)) * amplitude` - note `base` (the
first of the four `tcMod turb` numbers) is parsed but genuinely never used
by the real function, and the real "(x+z) drives s, y alone drives t" axis
asymmetry is exactly what rd-vanilla's own code does, not a simplification
introduced here. Applied as an additive offset after whatever `fragUV` the
existing scroll/scale/tcGen-environment logic already produced, matching
real Quake3's own "each tcMod transforms the previous stage's output"
composition (confirmed correct for all 3 real shaders here, which all
declare `turb` last).

**Implementation**: needed two new per-batch values (`amplitude` and the
precomputed `now`) with no room left in the existing 128-byte push
constant without fragile bit-packing into fields already spoken for
(`uvScale.z`/`.w`) - `vkWorldPushConstants_t` gained a new `turb` `vec4`
(only `.xy` used), growing the push constant to 144 bytes. This is a
deliberate, evidence-based tradeoff, not an oversight: 128 bytes is only
the Vulkan spec's *guaranteed minimum*, every real target this renderer is
tested against (desktop GPUs, and Mesa's lavapipe for headless testing)
supports well over that in practice, and a genuine failure here is loud
(`VK_Check`'s fatal error on pipeline layout creation) rather than the
usual silent-wrong-pixels failure shape this struct's own comment warns
about for its other fields - confirmed by this exact build succeeding
against lavapipe. `tr_shader.cpp` gained `tcMod turb` recognition
(`VK_GetShaderTcModTurb`); `WorldSurfaceBatch` gained `turbAmplitude`/
`turbPhase`/`turbFrequency` fields (amplitude 0.0 is a true no-op, same
identity-default convention as every other per-batch field here);
`RE_RenderScene`'s existing per-batch tracking loop precomputes `now`
once per distinct value, same "not a separate time uniform" approach
already used for `tcMod scroll`'s own offset; `world.vert` performs the
real per-vertex `sin()` evaluation.

**Verified**: warning-free rebuild (including the modified vertex shader),
full SP scene suite clean on all 5 scenes, confirmed running correctly
against lavapipe despite the larger push constant. A temporary debug print
(removed before committing) confirmed the exact real amplitude/phase/
frequency values for both `deathcon1`/`deathcon1a` match `test.shader`'s
real declarations precisely (`amp=0.4 freq=1` and `amp=0.1 freq=2`
respectively). Pixel-diffed a build with this fix against one without it:
academy1 (zero real matches) came back essentially unchanged (0.017%,
consistent with unrelated noise). vjun1 showed a diff in roughly the same
range as this project's own separately-measured run-to-run noise floor for
that scene, so - same as several features immediately above - the
debug-log confirmation is the primary evidence here, not the whole-frame
screenshot diff.

## `rgbGen lightingDiffuse` investigated and declined (real usage exists, but not a safe or well-scoped fix)

Same evidence bar as `rgbGen exactVertex` and `tcMod rotate` above: real
per-map usage exists, but implementing it correctly is either architecturally
out of scope for one step or would deliberately make a currently-fine-looking
surface render solid black, and neither risk is worth taking without much
stronger evidence than this project's own harness can currently produce.

**Real usage found** (BSP `LUMP_SHADERS`, first-stage-only, same
methodology as every feature above): hoth2's `models/map_objects/hoth/
ion_nut` and `textures/hoth/instance_stack`; yavin1's `models/map_objects/
danger/ship_item01`/`ship_item04` and `textures/yavin/grass_b` (one of the
foliage shaders `alphaFunc` above already fixed). The first group
(`map_objects/...`) are static `.md3` props with a real per-instance
ref-entity in this renderer already (see "Real `R_ModelBounds`" above);
`instance_stack` and `grass_b` are plain `textures/...`-named surfaces
compiled directly into the BSP, with no per-entity concept at all in this
renderer's `RE_LoadWorldMap`/`RE_RenderScene` path.

**Real formula**, read directly from rd-vanilla's own `RB_CalcDiffuseColor`
(`tr_shade_calc.cpp`): per vertex, `incoming = dot(normal, ent->lightDir)`;
if `incoming <= 0`, the vertex colour is just `ent->ambientLightInt` (a
packed RGBA); otherwise it's `ent->ambientLight + incoming *
ent->directedLight` per channel, clamped to 255. `ent->ambientLight`/
`directedLight`/`lightDir` come from `R_SetupEntityLightingGrid`
(`tr_light.cpp`) - a trilinear sample of the BSP's `LUMP_LIGHTGRID` at the
entity's world-space origin, decoding a packed lat/long normal for
`lightDir` and looking up per-lightgrid-cell ambient/directed colours.
This renderer parses no BSP lump named `LIGHTGRID` at all today - it would
be a genuinely new subsystem (grid-origin/size/bounds header fields, the
per-cell ambient/directed/latLong data, and the trilinear sampling itself),
not a one-file addition like every gap closed so far this window.

**The surprising real finding, worth recording on its own**: tracing where
`ent->ambientLight`/`directedLight`/`lightDir` actually get set for *world*
BSP-surface draws (as opposed to real ref-entities) turned up something not
obvious from reading `RB_CalcDiffuseColor` in isolation. `RB_RenderDrawSurfList`
(`tr_backend.cpp`) points `backEnd.currentEntity` straight at `&tr.worldEntity`
for any surface whose `entityNum == REFENTITYNUM_WORLD` - it never calls
`R_SetupEntityLighting` on it. Grepping all of `rd-vanilla` for every write to
`tr.worldEntity`'s fields turns up exactly two places, both just the pointer
assignment (`tr_backend.cpp:671,799`) - nothing ever populates its
`ambientLight`/`directedLight`/`lightDir`/`ambientLightInt`, so they sit at
their static zero-initialized values for the entire life of the process.
Fed back into the real formula above: `lightDir = (0,0,0)` makes
`incoming = dot(normal, lightDir) = 0` for literally every vertex, on every
world surface, always taking the `incoming <= 0` branch, and
`ambientLightInt` is `0` (RGBA all zero, including alpha). That means real
vanilla's own `rgbGen lightingDiffuse`, applied to a plain world-BSP surface
like `grass_b` or `instance_stack`, evaluates to a fully transparent black
vertex colour for every vertex - a real, load-bearing quirk of the engine's
entity/world lighting split, not a hypothetical edge case.

**Why this isn't shipped**: two very different fixes hide under one
keyword, and neither clears this project's own bar cleanly. For the
`map_objects/...` static-prop matches, a correct implementation needs the
full light-grid subsystem above - real, but too large to land as one
"small step," and speculative without first confirming a real vanilla
screenshot actually shows visibly different (grid-sampled, non-flat)
shading on `ion_nut`/`ship_item01` today. For the plain-world-surface
matches, a *literal* port of the real formula would turn `grass_b` and
`instance_stack` solid black - which may well be exactly what real vanilla
does (the trace above is direct, not inferred), but shipping a change that
makes currently-reasonable-looking geometry render black, on the strength
of source-reading alone with no real-vanilla screenshot confirming the
BSP surfaces in question look that way, is the kind of poorly-evidenced
change this project's own rules say to decline rather than ship. Revisit
if a later `LUMP_LIGHTGRID` pass (undertaken for its own sake, e.g. for
real Ghoul2/static-model ambient lighting generally) makes the grid data
available and a real-vanilla screenshot of `grass_b`/`instance_stack`
can be captured and inspected directly.

## Real `depthWrite` for world geometry

Closes another real gap, found while investigating `sort` (draw-order
override) and `depthWrite` as the next candidates after `rgbGen
lightingDiffuse` above - `sort` turned out to duplicate a simplification
this renderer already documents and accepts (see
`vk.worldPipelineAlpha`'s own comment: no full back-to-front sort between
translucent surfaces, same as the existing runtime-poly rendering), so it
wasn't pursued further, but `depthWrite` turned up real, previously-unhandled
usage.

**Real formula**, read directly from rd-vanilla's own shader parser
(`tr_shader.cpp` `ParseStage`): a stage's default is `depthMaskBits =
GLS_DEPTHMASK_TRUE` (write depth), but the moment a real `blendFunc` is
parsed, `depthMaskBits` is reset to `0` (no write) *unless* `depthWrite` was
already declared earlier in the same stage, in which case it stays on. In
other words: `depthWrite` is specifically for a blended stage that still
needs to write depth (so it correctly occludes geometry drawn after it),
overriding the "blended surfaces don't write depth" default.

**Real usage found** (BSP `LUMP_SHADERS`, first-stage-only, cross-referenced
against the real `.shader` scripts): hoth2's `textures/imperial/grate02`
and yavin1's `textures/bounty/flag2`/`models/map_objects/danger/
ship_item01`/`models/map_objects/yavin/plant` genuinely combine a real
`blendFunc` with `depthWrite` and needed this fix - confirmed live, not just
by reading the `.shader` text, via a temporary debug print (removed before
committing) inside `RE_LoadWorldMap` that logs exactly which shaders this
renderer's own code resolves `depthWrite=true` for at real map-load time.
That same debug print incidentally confirmed two things beyond the basic
gap: `ship_item01` is a genuine BSP-compiled world surface, not merely an
entity-instanced static prop (it fired from inside the world-surface loading
loop itself); and `models/map_objects/yavin/plant` - a shader name defined
three different ways across `models.shader` (twice) and `yavin.shader` (once
each with a different `rgbGen` and no `blendFunc`/`depthWrite` for the
`models.shader` two) - resolves through this renderer's own real
`ri.FS_ListFiles` order to `yavin.shader`'s `blendFunc`+`depthWrite`
version, the one that actually needs this fix, matching real rd-vanilla's
own identical resolution mechanism (same "first definition wins" rule
already documented for `tcMod turb`'s `deathcon1a` collision above).
Several other apparent `depthWrite` hits were checked and ruled out as false
positives: `textures/imperial/grate02_broke`, `textures/vjun/grate`/
`grate1` all pair `depthWrite` with `blendFunc GL_ONE GL_ZERO`, which real
Quake3 already treats as "blending implicitly disabled" (`BlendFactorsToMode`
already classifies this exact pair as `BLEND_OPAQUE` - a fix from an earlier
pass, see "`rgbGen const` and a widened additive map-image fallback" above)
- opaque surfaces already write depth by default, nothing to override. One
real match, yavin1's `textures/factory/T2_Wedge_floorgrate`, is a documented
no-op here for a *different*, pre-existing reason unrelated to this fix: its
first stage's `map` path (`textures/imperial/floorgrate`) doesn't match its
own shader name, and the existing map-image-fallback safety gate only covers
`BLEND_OPAQUE`/`BLEND_ADDITIVE`, not `BLEND_ALPHA` - so this surface is
currently skipped outright regardless (confirmed absent from the same debug
print), a gap this fix doesn't touch.

**Implementation**: since this renderer bakes each blend/depth combination
into a separate `VkPipeline` object rather than toggling depth-write as
Vulkan dynamic state, a real per-shader override needs its own pipeline
variant rather than a per-draw flag - `vk.worldPipelineAlphaDepthWrite`
(`tr_init.cpp`), same blend-attachment state as `vk.worldPipelineAlpha` but
reusing the opaque pipeline's own depth-stencil state (test *and* write
both on). No `BLEND_ADDITIVE` equivalent exists, matching the complete
absence of real additive+depthWrite usage above - adding one now would be
exactly the kind of speculative, never-exercised code this project avoids.
`tr_shader.cpp` gained `depthWrite` keyword recognition
(`VK_GetShaderDepthWrite`), stored only when the shader is genuinely
`BLEND_ALPHA`/`BLEND_ADDITIVE` (an opaque shader already writes depth, so
storing `true` for one would be meaningless - and would incorrectly suggest
the additive pipeline needs a depth-write variant it doesn't).
`WorldSurfaceBatch` gained a `depthWrite` field (deliberately appended last,
same positional-aggregate-init convention as every other per-batch field
here); `RE_LoadWorldMap`'s existing `stable_sort` gained a secondary sort
key grouping `depthWrite` batches to the end of the `BLEND_ALPHA` run, so
`RE_RenderScene`'s pipeline-switch count stays close to its documented
handful-per-frame bound instead of scattering `worldPipelineAlphaDepthWrite`
switches throughout the alpha range; `RE_RenderScene`'s existing pipeline
selection now checks `batch.depthWrite` before falling back to the default
`worldPipelineAlpha`.

**Verified**: warning-free rebuild, full SP scene suite clean on all 5
scenes (no crashes, no Vulkan validation complaints from the new 5th
pipeline). The debug-log confirmation above is the primary evidence for
which real shaders this fixes - `grate02`/`flag2`/`ship_item01`/`plant` are
narrow, small-area surfaces (a grate, a hanging flag, a prop, a foliage
clump) whose depth-write correctness affects occlusion ordering against
other translucent geometry rather than producing a large, easily-diffable
whole-frame pixel change, the same evidence-tier reasoning already applied
to several features above (e.g. `tcMod turb`'s vjun1 hit).

## Four real rendering bugs found from a direct side-by-side screenshot review

Prompted by a direct request to review fresh rd-vanilla-vs-rd-vulkan comparison
screenshots "like a 3D engine expert" - not a shader-keyword gap this time,
but four genuine implementation bugs, each root-caused with real debug
instrumentation (added, used, and removed before committing, per this
project's own standing methodology) rather than guessed at from the pixels
alone.

### 1. Hoth2's wampa/tauntaun NPCs entirely invisible

**Symptom**: three real `WildTauntaun` NPCs, clearly visible in the
rd-vanilla reference screenshot, completely absent from rd-vulkan's - not
faint or miscoloured, just gone.

**Root cause**: `models/players/tauntaun/model.glm`'s own embedded surface
shader names are ALL empty (confirmed by parsing the real `.glm`'s
`mdxmSurfHierarchy_t` records directly) - correct and expected for this
family of model, since real vanilla resolves every surface's texture
entirely through a separately-registered `.skin` file, not the mesh's own
data. `G2API_InitGhoul2Model` (real vanilla's own documented convention,
matched here) always loads a model with skin 0 ("no skin") - the real skin
only gets applied afterward, via `G2API_SetSkin`. For ordinary player
models that's fine, since `g_client.cpp`'s `G_SetG2PlayerModel` always calls
`G2API_SetSkin` immediately after. Tracing NPC spawn code
(`NPC_stats.cpp:4064`) showed NPCs go through that exact same function - so
`G2API_SetSkin` *was* being called correctly, with a real, valid skin
handle. The actual failure was one level deeper, inside the skin file
parser itself.

### 2. `VK_ParseSkinFile` didn't strip leading whitespace

Confirmed directly: `models/players/tauntaun/model_wild.skin` (the skin
hoth2's `WildTauntaun` NPCs actually use, not the more common
`model_default.skin`) writes `torso, models/players/tauntaun/tauntaun_body_
bare_back.tga` - a space *after* the comma. `VK_ParseSkinFile`
(`tr_model.cpp`) only stripped trailing whitespace/`\r` from the shader-name
token, so every one of this file's real shader names kept a leading space,
making every surface's `VK_FindImage` lookup fail on a name that was never a
real file to begin with - `VK_LoadGhoul2Model` legitimately found zero
drawable surfaces and the NPC simply never drew anything. Real rd-vanilla's
own `RE_RegisterIndividualSkin` (`tr_skin.cpp`) uses `CommaParse`, a real
tokenizer that skips leading whitespace before every token - this renderer's
substring-based parser didn't replicate that. **Fix**: strip whitespace from
both ends of both the surface-name and shader-name tokens, not just the
trailing end of the shader name.

Confirmed live via a temporary debug print (removed before committing) of
exactly which real skin index and `surfaceShaders` map size
`VK_LoadGhoul2Model` resolved for `model_wild.skin`: 0 real entries before
the fix (matching "no drawable surfaces"), 7 after (matching its 7 real
non-`*off` surface lines).

### 3. `refEntity_t::customSkin` never read for Ghoul2 draws

A related, second real gap found investigating the above (not the actual
cause here, since NPCs do go through `G2API_SetSkin`, but a genuine, real
mechanism this renderer was still missing): real rd-vanilla's own
`R_AddGhoul2Surfaces` (`tr_ghoul2.cpp`) checks `ent->e.customSkin` - a
plain, already-networked `refEntity_t` field, set directly by the client
with no `G2API_SetSkin` call involved at all - and lets it override
whatever skin a sub-model was loaded with, taking priority over
`CGhoul2Info::mSkin`. `VK_DrawGhoul2Entities` never read this field.
**Fix**: when `ent.customSkin` is set, re-resolve that sub-model's
`modelIndex` through `VK_LoadGhoul2Model` with that skin instead of the
model's own baked-in one (same cache, so this only costs a lookup once
that skin's variant has loaded once).

### 4. Two of three simultaneous tauntaun NPCs render solid black

**Symptom**: after fixing #1/#2 above, all three `WildTauntaun` NPCs finally
drew - but two of the three were flat black silhouettes instead of their
real texture, while the third looked correct.

**Root cause**: all three NPCs share one cached `VulkanGhoul2Model` (same
file, same resolved skin), which needs `GHOUL2_SKIN_SLOTS_PER_MODEL`
independent vertex-buffer "slots" so simultaneous instances at different
animation poses don't clobber each other (see that constant's own comment).
Only slot 0 is seeded at load time from `cpuVerts` (position, UV, AND
colour, all already correct there); the comment at that call site claimed
"every slot is always fully rewritten by `VK_SkinGhoul2Model` before
anything reads it" - true for position/UV/lightmap-UV, but `VK_SkinGhoul2Model`
never actually wrote `color` at all. Slot 0's instance inherited a correct
white vertex colour by accident (from the initial load-time copy); every
other slot's colour channel was whatever the freshly Vulkan-allocated
host-visible memory happened to contain (zero, under Mesa's lavapipe),
silently multiplying that instance's entire diffuse texture to solid black.
Confirmed with a temporary per-instance debug print showing all three NPCs
resolving the identical `modelIndex`/skin, ruling out a per-entity
skin/model mismatch before looking at the skinning function itself. **Fix**:
`VK_SkinGhoul2Model` now writes `color = (1,1,1,1)` for every vertex, every
slot, every call, matching what the comment already (incorrectly) claimed
it did.

### 5. `RDF_SKYBOXPORTAL` drawn as an ordinary second scene

**Symptom**: yavin1's and vjun1's opening cockpit scenes both showed a large
"hole" - unrelated outdoor jungle terrain (yavin1) or a flat sky-blue patch
(vjun1) where solid interior wall/window geometry belongs, in a way that
persisted even with view-frustum culling forcibly disabled (ruling that out
directly rather than assuming).

**Root cause**: real Quake3/JKA calls `RE_RenderScene` a *second* time per
frame with `RDF_SKYBOXPORTAL` set, from an entirely different camera placed
elsewhere in the map (a mapper-configured "portal sky" - a miniature
separate scene meant to be composited into just the background of the real
scene rendered immediately after it - see `RDF_DRAWSKYBOX`'s own comment,
`tr_types.h`). Confirmed directly with a temporary debug print of every real
`refdef_t` passed to `RE_RenderScene`: yavin1's opening scene calls it with
`(vieworg=(6176,-2536,608), rdflags=24)` immediately followed by
`(vieworg=(4043,-191,-16), rdflags=16)` every single frame - `24 = 8
(RDF_SKYBOXPORTAL) | 16 (RDF_DRAWSKYBOX)`, `16` alone for the real scene -
exactly the real engine's own documented pair of flags. This renderer only
ever checked for `RDF_NOWORLDMODEL`; the portal-sky call fell through and
got drawn as an entirely ordinary full opaque scene - real world geometry,
from a real but unrelated camera position out in the jungle - directly into
the same framebuffer the correct scene draws into immediately after.
**Fix**: skip the call outright when `RDF_SKYBOXPORTAL` is set, matching
this project's already-documented "no portal-sky compositing" scope (see
"Proper sky rendering" below) - since the very next `RE_RenderScene` call
this same frame always draws a complete opaque scene (its own geometry and
skybox) over the whole framebuffer regardless, skipping the portal call
loses nothing and removes the wrong scene's leftover pixels entirely.

**Verified** (all four fixes together): warning-free rebuild, full SP scene
suite clean on all 5 scenes. Fresh side-by-side screenshots against
rd-vanilla confirm: hoth2's three tauntaun NPCs now render identically and
correctly; yavin1's and vjun1's opening cockpit scenes now show their full
real interior with no hole. All four fixes are narrow and root-caused, not
speculative - each was confirmed with a targeted temporary debug print
before being fixed, and every print was removed before committing.

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
  world geometry" above; `MST_FLARE` surfaces are parsed into their own
  list and drawn as camera-facing additive quads - see "Static flares"
  above; `SURF_NODRAW`/`SURF_SKY`
  surfaces are skipped; each surface's diffuse texture is resolved through
  the same first-stage-only `.shader` lookup the 2D path uses, multiplied by
  its baked lightmap (or a white 1x1 fallback for surfaces with none); a
  surface whose diffuse texture fails to resolve is skipped rather than
  drawn wrong (see the `dark_dust` bug below); surfaces entirely outside the
  view frustum are culled (per-surface AABB vs. the frame's frustum planes,
  see "3D world geometry" above) but there's still no BSP visibility
  culling or back-face culling (see the "no culling" comment on
  `VK_CreateWorldPipeline` in `tr_init.cpp` for why not even back-face
  culling is safe to turn on yet); each surface's own `.shader`-driven
  blend mode picks one of three pipelines (opaque/alpha/additive - see
  "World geometry blend modes" above), the same selection the 2D path has
  had all along; drawn with a real per-frame camera built from `refdef_t`
  (see `VK_BuildViewMatrix`/`VK_BuildProjectionMatrix` in `tr_world.cpp`).
- Skybox rendering (`tr_world.cpp`: `VK_LoadSky`) - see "3D world geometry"
  above for what was verified. A flat (non-subdivided, non-warped) 6-face
  box using the sky shader's own name as its basename, always camera-
  centered; drawn depth-test/write-disabled before world geometry.
- Ghoul2 (character/weapon model) rendering (`tr_model.cpp`) - real `.glm`
  mesh parsing, `.skin` texture resolution (both the common single-file
  case and the three-part `head|torso|lower` composite macro syntax),
  bind-pose bone bolts, per-frame entity dispatch through the same
  pipeline/vertex-format world geometry uses, and (see "Skeletal animation"/
  "Live animation"/"Full `G2_TimingModel` port" above) real per-bone mesh
  skinning driven by a real, live, time-driven animation state per model
  instance (`G2API_SetBoneAnim`/`GetBoneAnim`/etc genuinely work now) -
  independently-tracked per-bone-subtree animation (legs/torso can play
  different animations at once, resolved via the real bone-hierarchy walk),
  real cross-fade blending between an old and new animation over a real
  `blendTime`, and real sub-frame interpolation between adjacent whole
  frames - not a cruder single-whole-skeleton-track approximation. Real
  per-instance surface on/off overrides (`SetSurfaceOnOff`), real
  surface bolts (a bolt naming a `*`-prefixed tag surface, not just a
  bone), real model-to-model attachment (`AttachG2Model`), and real
  per-level animation-file overrides (`SetAnimIndex`/`GetAnimIndex`), and
  real bone-angle overrides (`SetBoneAngles`/`SetBoneAnglesIndex`,
  `BONE_ANGLES_POSTMULT` only) now too - see "Ghoul2 per-surface on/off
  overrides"/"Ghoul2 surface bolts"/"Ghoul2 model-to-model attachment"/
  "Ghoul2 per-level animation-file overrides"/"Ghoul2 bone-angle overrides"
  above. Still missing: ragdoll/IK, LOD selection, gore.
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

- Ghoul2 skeletal animation is live now (see "Live animation"/"Full
  `G2_TimingModel` port" above: `G2API_SetBoneAnim`/`GetBoneAnim`/etc
  really work, time-driven, not stubs), with independently-tracked
  per-bone-subtree animation (legs/torso/etc. playing different animations
  at once, resolved via the real bone-hierarchy walk - not a single
  whole-skeleton track), real cross-fade blending between an old and new
  animation over a real `blendTime`, and real sub-frame interpolation
  between adjacent whole frames. Reverse/negative-speed playback is ported
  faithfully in `VK_Ghoul2TimingModel` but untested against a real scene,
  since no real caller in this game currently uses it. `SetAnimIndex`/
  `GetAnimIndex` (selecting *which* `.gla`/animation-file a model uses, a
  different concept from which frame within one - relevant for NPCs with a
  per-level animation-file override, e.g. `_humanoid_academy1.gla`) are
  real now too - see "Ghoul2 per-level animation-file overrides" above.
  Bone-angle overrides (`SetBoneAngles*`, `BONE_ANGLES_POSTMULT` only - see
  "Ghoul2 bone-angle overrides" above) are real now too. Also still
  missing: ragdoll, IK, LOD selection, gore, and tags. Surface bolts (a
  bolt naming a *surface* rather than a bone) and model-to-model attachment
  (`AttachG2Model`/`DetachG2Model`) are real now too - see "Ghoul2 surface
  bolts"/"Ghoul2 model-to-model attachment" above. `AttachEnt` (cross-*entity* attachment,
  a different and narrower mechanism - see "Ghoul2 model-to-model
  attachment" above for why) is still a stub.
  See "Ghoul2 is not reused from rd-vanilla" below for why the animation
  system was a separate, larger task than the rest of this renderer.
- `RE_SetRangedFog`, `R_SetTempGlobalFogColor`, and per-brush *local* fog
  volumes (a `LUMP_FOGS` entry with a real `brushNum`, bounded to one
  convex region, e.g. vjun1's `fog_black`) are all real now - see "World
  weather/particle effects" and "Local fog volumes and ranged fog" below.
  Still missing: real per-brush volume bounds testing (this renderer trusts
  the BSP compiler's own per-surface `fogNum` assignment instead - see that
  section's own comment for why that's sufficient here) and the exact
  EXP2/gradient-texture falloff curve rd-vanilla's real fog uses (a linear
  distance ramp approximates it instead, same simplification as the base
  global-fog work).
- Dynamic lighting for world geometry (`AddLightToScene` is a stub) - only
  the map's precomputed, baked lightmap and (for vertex-lit surfaces, see
  "Real per-vertex colour" below) baked per-vertex colour apply. No shadows
  other than what's already baked into those; nothing moves, casts, or
  receives a dynamic shadow.
- BSP visibility (PVS) culling and back-face culling for world geometry -
  view-frustum culling *is* implemented (see "3D world geometry" above),
  but every surface potentially in view is still submitted regardless of
  whether the level's BVH/PVS data would say it's actually occluded by
  other geometry, and both triangle winding directions still draw.
  Back-face culling specifically was investigated, not just left alone:
  empirically determined the real winding sign under this pipeline's
  negative-viewport-height Y-flip (temporarily culled one direction and
  confirmed the world - walls, floor, terrain, a vjun1 cockpit interior -
  rendered pixel-identical to the no-culling baseline on 3 of the 4 test
  maps, meaning that direction only ever removed genuinely-invisible real
  backfaces), and separately confirmed the sky box's own faces (wound the
  opposite way, facing inward toward the camera) need their own
  unconditionally-uncontrolled raster state rather than sharing the world
  one. Reverted anyway: hoth2 (the 4th map) showed a real, if small
  (~1% of the frame), hole - a distant background structure's silhouette
  changed shape, exposing different underlying geometry rather than
  just losing an invisible backface - meaning at least one real prop's
  triangle winding isn't consistent with the rest of that map's data
  (plausible for complex/rotated brushwork; not investigated further).
  Per this file's own long-standing reasoning for leaving this off in the
  first place - "a wrong cull direction silently drops geometry rather
  than erroring, worse than the minor overdraw cost of drawing both
  faces" - one confirmed real counter-example on real map data is enough
  to keep this disabled rather than ship a mostly-safe-but-not-fully-
  verified optimization.
- Flares (`MST_FLARE`) *are* drawn now (see "Static flares" above), but
  without rd-vanilla's real `RB_TestZFlare` occlusion pre-test specifically
  (a real per-pixel depth test is a side effect of the pipeline state reused
  to draw them, not a port of that function). Blend mode is still always
  additive regardless of a flare's own `blendFunc` (see "Static flares"
  above for why), and `rgbGen const`/`rgbGen wave sin` are real now too
  (see "Real rgbGen const/wave for flares" above) - other `rgbGen` types
  and all of `alphaGen` (beyond the already-real `alphaGen portal` radius)
  still aren't evaluated.
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
  `map`/`clampmap`/`blendFunc`/`alphaFunc`/`depthWrite`/`tcMod scroll`/
  `tcMod scale`/`tcMod turb`/`rgbGen const`/`rgbGen wave sin`/`alphaGen
  portal <range>`/`tcGen environment` (see "World-geometry tcMod scroll"/
  "`tcMod scale` for world geometry"/"`rgbGen const` and a widened additive
  map-image fallback"/"Static flares"/"Real rgbGen const/wave for flares"/
  "`tcGen environment` (reflection-mapped UV generation)"/"Real `clampmap`
  addressing"/"Real `alphaFunc` alpha-testing"/"Real `tcMod turb`"/"Real
  `depthWrite` for world geometry" above for those), whether it declares any
  `tcGen` at all (not which kind otherwise -
  see the `rgbGen const` section), and (for fog shaders specifically, see
  "3D world geometry" above) a top-level `fogparms` line, are read - later
  stages, every other `tcMod` type (`rotate`/`stretch`/`transform`/
  `entityTranslate`), every other `tcGen` type (`tcGen lightmap`/`tcGen
  vector` - no real per-map match on this checkout's test maps for either,
  see the `tcMod rotate` investigation's methodology), `rgbGen
  identityLighting`/`vertex`/non-`sin` wave functions, `alphaGen` waves,
  `sort` (a real per-shader draw-order override - considered, not just
  skipped, but this renderer's existing opaque/alpha/additive rank sort
  already accepts "no full depth sort between translucent surfaces
  themselves" as documented, and a finer per-shader `sort` value would only
  refine that same already-accepted simplification rather than close a
  distinct gap, so it wasn't pursued further), and `skyparms` are still
  ignored. `alphaGen wave` and non-`sin` `rgbGen wave` functions were
  checked directly (first-stage-only, all 4 test maps) and have zero real
  matches, cleanly ruling both out the same way `tcMod rotate`'s
  investigation did. `deformVertexes` has exactly one real match on this
  checkout's test maps - vjun1's `textures/flares/flare_bluehue`,
  `deformVertexes autoSprite` (turn independent quads into camera-facing
  billboards) - and needs no separate implementation: this renderer's real
  flare draw path (`VK_DrawWorldFlares`, see "Static flares" above) already
  constructs each flare as a per-pixel depth-tested camera-facing quad by
  design, which is exactly what `autoSprite` asks for, confirmed by reading
  that function's own quad-orientation math rather than assumed. `skyparms`'s
  basename-mismatch scenario (see "Proper sky rendering" below) was also
  checked directly against every real sky shader definition in this
  checkout's shader library - none of the 4 test maps' own sky shaders
  hit it, so implementing `skyparms` parsing now would fix nothing
  observable on any scene this project currently verifies against. `rgbGen lightingDiffuse` has real
  per-map matches but was investigated and explicitly declined, not just
  left alone - see "`rgbGen lightingDiffuse` investigated and declined"
  above for why (a real BSP `LUMP_LIGHTGRID` subsystem this renderer
  doesn't have, plus a genuine risk of matching real vanilla's own
  degenerate solid-black output for plain world-BSP surfaces without
  screenshot confirmation that's actually correct). World geometry does now get real
  blend-mode selection (see "World geometry blend modes" above) for a
  shader whose own
  name directly resolves to a texture file, and the map-image fallback (see
  "hoth2's missing terrain" above) now also covers a `BLEND_ADDITIVE`
  shader specifically when it has no `tcGen` (see the `rgbGen const`
  section above for exactly why that's the safe line, not blend mode
  alone) - a `BLEND_ALPHA` shader needing the fallback is still unresolved.
- Cinematics (`DrawStretchRaw`/`UploadCinematic`), dissolves, tag queries
  (`R_LerpTag` - real vanilla's own MD3-only implementation already returns
  identity/zero for every real Ghoul2 weapon model this game actually ships
  with, the same as this renderer's existing stub, so there is no real gap
  left to close there). Rotated pics (`RE_DrawRotatePic`/
  `RE_DrawRotatePic2`) and static-`.md3` model bounds (`R_ModelBounds`)
  *are* real now - see their own sections above.

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
too." Given that's a separate, larger task, most `G2API_*` entry points
*beyond* model loading and skin registration are still stubbed (see
`tr_init.cpp`) - no LOD selection, gore, or ragdoll; skeletal animation,
bolts, and surface on/off overrides are real now (see "Live animation"/
"Ghoul2 per-surface on/off overrides" above).

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

**Use the Ninja generator** (`cmake -G Ninja ...`) rather than the default
Unix Makefiles - this is a fast-moving renderer under active, incremental
development (one-file touch-and-rebuild several times an hour during a real
investigation), and Ninja's dependency tracking is both faster per-build
and more reliable under it: a Unix-Makefiles build directory in this same
project was observed reporting a target as already up to date (`[100%]
Built target ...`) immediately after that target's own output file had been
deleted out from under it - a real, reproducible false-negative, not a
one-off fluke - which is exactly the kind of silent staleness this section
and "Testing headlessly" below already warn about from a different angle
(a stale binary quietly surviving a rebuild). Reconfigure an existing
Makefiles build directory fresh (CMake can't switch an existing one's
generator in place - delete it and re-run `cmake -G Ninja` with the same
`-D` options) rather than trying to convert it.

## Testing headlessly

**Prefer the CMake targets** (`ninja render_regression_vulkan` et al.) - see
`tests/render-regression/README.md`'s "Recommended: CMake targets" section.
They wrap exactly the manual invocation below, but name every binary they
stage via CMake's own build-output mechanism rather than a hand-typed path
- see this file's "A capture-harness bug that invalidated a run of
'verified' claims" section (item 1, the stale-build-artifact one, and its
"recurred once" follow-up paragraph at the end of that same section) for
the exact mixup that motivated automating this, and why a one-off manual
fix for it wasn't enough. The manual invocation remains useful for a
one-off command, a custom scene, or debugging the harness itself.

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
name rather than erroring. See "Character animation investigation: eight
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

**Testing a real live window resize** (see "Window resize / swapchain
recreation" above for what this verified) needs a real X11 window a
second process can reach into mid-session - `xvfb-run -a` alone isn't
enough since it blocks for the whole capture and tears its Xvfb down
right after, leaving no window to resize from outside. Run `Xvfb` and the
engine separately instead: `Xvfb :97 -screen 0 800x600x24 &`, then launch
`openjk_sp.x86_64` (same `+set`s as above, `DISPLAY=:97`) in the
background with several `+wait N` / `+screenshot_png` pairs scripted in
sequence, and in the *same shell* poll for each screenshot's PNG to land
on disk before resizing - polling and resizing from a separate later
command races against the engine's own frame pacing and reliably resizes
*after* the process has already hit `+quit` instead (confirmed the hard
way: under lavapipe, `+wait 400` completes in well under a second once a
map's finished loading, far faster than two separate tool round-trips can
land in between). `xdotool` (not preinstalled - `apt-get install
xdotool`) does the actual resize once a screenshot's own file confirms the
right moment: `WID=$(DISPLAY=:97 xdotool search --onlyvisible --name ""
| tail -1)` finds the engine's window (no window manager is running under
Xvfb, so `getactivewindow`/`_NET_ACTIVE_WINDOW`-based lookups fail - a
plain `search` by empty name pattern still finds it), then `xdotool
windowsize "$WID" <w> <h>` resizes it directly via `XResizeWindow`,
independent of any window-manager size-hint negotiation. Comparing the
resulting screenshots' own pixel dimensions (not just that a file exists)
is what actually confirms the swapchain was recreated at the new size
rather than silently kept at the old one.
