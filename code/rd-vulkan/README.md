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
  `Assertion 0 failed` crash on a direct `devmap` documented earlier in this
  file - not caused by this change, reproduced identically with patches
  reverted); `vjun1`'s frustum-culled/visible batch split with patches
  enabled (11576/12556, ~92%) closely matches the same map with patches
  skipped (11462/12436, ~92%) across `wait_frames` 30/90/200, so the new
  per-sub-patch AABBs aren't corrupting culling. Both test maps' default
  camera angle happens to be a poor vantage point for actually *seeing*
  terrain (confirmed pre-existing and unrelated to this change too, by
  reproducing the identical framing with patches reverted) - a visible
  rounded silhouette on `hoth2`'s one on-screen curved shape is the closest
  thing to a direct visual check achieved here, short of a proper "fly a
  camera to a clear vantage point" capability this headless harness doesn't
  have yet. `academy1`/`sp_menu` are unaffected (identical batch/vertex/index
  counts and screenshot to before, as expected for a map with no patches to
  begin with).

### Ghoul2 rendering (tr_model.cpp)

Character/weapon model (`.glm`) rendering, in the models' **static bind
pose only** - no skeletal animation, no bone math at all. The scope-reducing
fact making that small: `mdxmVertex_t::vertCoords` (`rd-common/mdx_format.h`)
is already the model's bind-pose object-space position - bone weight data
only matters for computing how a vertex should move *away* from bind pose
during animation - so the `.gla` skeleton/animation file is never even
opened. GLM parsing is a fresh implementation (see "Ghoul2 is not reused
from rd-vanilla" below for why), but the offset/pointer arithmetic is
copied field-for-field from rd-vanilla's real `R_LoadMDXM`
(`rd-vanilla/tr_ghoul2.cpp`) rather than rederived from the struct comments
alone. Loaded models reuse `tr_world.cpp`'s vertex format, pipeline, and
descriptor-set-building helper wholesale - a Ghoul2 surface is, for drawing
purposes, just another indexed triangle batch paired with `vk.whiteImage`
as its "lightmap" (Ghoul2 meshes have no baked lightmap of their own).

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
  skipped); the three-part `head|torso|lower` macro skin syntax is **not**
  implemented. `RE_RegisterSkin` now calls it for real (previously a stub
  returning a fake handle `1`). The model cache key is
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
- Ghoul2 (character/weapon model) rendering (`tr_model.cpp`), in models'
  static bind pose - see "Ghoul2 rendering" above for what was verified and
  its scope: no skeletal animation, LOD selection, per-surface on/off
  overrides, or gore, but real `.glm` mesh parsing, `.skin` texture
  resolution (single-file case only), bind-pose bone bolts (see below), and
  per-frame entity dispatch through the same pipeline/vertex-format world
  geometry uses.

## What's not implemented yet (safe no-ops, won't crash, won't draw)

- Ghoul2 skeletal animation and everything downstream of it: bone math, LOD
  selection, per-surface on/off overrides (`SetSurfaceOnOff`), gore, tags,
  ragdoll, model-to-model attachment (`AttachG2Model`/`AttachEnt`) and
  surface bolts (a bolt naming a *surface* rather than a bone - only bone
  bolts are implemented, see "Ghoul2 rendering" above for the real, bind-
  pose-only `AddBolt`/`GetBoltMatrix`). Models render in a fixed bind pose
  only - see "Ghoul2 rendering" above for exactly what *is* real (mesh
  parsing, `.skin` textures, bone bolts, entity dispatch) and "Ghoul2 is
  not reused from rd-vanilla" below for why the animation system in
  particular is a separate, larger task. Every other `G2API_*` entry point
  beyond model loading/skin registration/bone bolts is still a safe stub.
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
- Runtime polys (`AddPolyToScene`) - still a stub. Entities
  (`AddRefEntityToScene`) are real for Ghoul2 (`RT_MODEL`, see "Ghoul2
  rendering" above), but every other `refEntityType_t` (sprites, beams,
  electricity, oriented quads, ...) is queued and silently ignored at draw
  time - nothing about those types renders yet.
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
