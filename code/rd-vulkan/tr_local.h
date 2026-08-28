/*
===========================================================================
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#pragma once

// First-pass Vulkan renderer. See README.md in this directory for exactly
// what is and is not implemented yet - in short: window/instance/device/
// swapchain setup, a 2D textured-quad draw path (RE_StretchPic, used for
// the whole UI/menu layer and for font glyphs), static world/BSP geometry,
// and Ghoul2 (character/weapon model) rendering in a fixed bind pose are
// real; skeletal animation and everything downstream of it (bolts, LOD
// selection, surface on/off overrides, gore, ragdoll), dynamic lights, and
// BSP visibility culling are not implemented yet and are deliberately safe
// no-ops for now.

#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <string>

#include "../qcommon/q_shared.h"
#include "../qcommon/qfiles.h"
#include "../rd-common/tr_common.h"
#include "../rd-common/tr_public.h"

struct SDL_Window;

// Referenced (extern) by the reused rd-common/tr_font.cpp.
extern cvar_t *r_verbose;
extern cvar_t *se_language;
extern cvar_t *com_buildScript;
// Real rd-vanilla quality slider (tr_init.cpp) - only referenced here for
// RT_ELECTRICITY's fractal recursion depth (tr_model.cpp), the one thing
// in this renderer that reads it; see that code's comment for why it's
// clamped rather than used as raw as rd-vanilla's own "2 - r_lodbias"
// formula does.
extern cvar_t *r_lodbias;
// Permanent debug tool, not a one-off: prints one line per animated Ghoul2
// bone-track per drawn entity, at a fixed real-time rate, in a format
// identical to rd-vanilla's own "r_ghoul2animdebug" (tr_ghoul2.cpp) so the
// two renderers' logs can be directly diffed/grepped against each other by
// entity position and bone name - see tr_model.cpp's VK_DrawGhoul2Entities
// for the exact fields. Added after repeated one-off temporary
// instrumentation (added, used to find a real bug, then deleted) during
// the character-animation investigation (README.md) turned out to be
// needed more than once - a permanent, symmetrical tool in both renderers
// answers "what is this NPC's animation state doing over time in each
// renderer" directly, without re-deriving throwaway prints each time.
extern cvar_t *r_ghoul2AnimDebug;

#define MAX_VK_IMAGES 4096
// vk.worldDescriptorPool/vk.ghoul2DescriptorPool's maxSets - deliberately a
// separate, much larger constant from MAX_VK_IMAGES, not a reuse of it: the
// UI descriptor pool allocates one set per unique *image* (bounded by how
// many distinct textures ever get loaded, comfortably under 4096), but the
// world/Ghoul2 pools allocate one set per *surface batch* - a completely
// different scale. A real, complex level's world geometry alone can vastly
// exceed 4096 batches (vjun1: 12833, confirmed via its own "N draw batches"
// load-time log line) - reusing MAX_VK_IMAGES here would silently run out
// of pool capacity partway through the surface list on any map exceeding
// it, and vkAllocateDescriptorSets failing doesn't produce a clean, loud
// error either (VK_Check's ri.Error(ERR_FATAL, ...) would have made this
// obvious immediately) - some Vulkan implementations (Mesa's lavapipe,
// used to develop and test this renderer, among them) don't strictly
// enforce a pool's nominal maxSets/poolSize at allocation time, so an
// exhausted pool can keep "succeeding" while corrupting other descriptor
// sets' already-written image bindings instead. A real, independently
// justified fix per the Vulkan spec regardless of any specific symptom -
// found and fixed while chasing vjun1's missing-cockpit/orange-artifact
// bugs (see README.md), and initially suspected as their cause, but
// re-tested against them directly (rebuilt with only this fix applied) and
// confirmed to make no visible difference to either: both turned out to
// have their own, unrelated root causes (a never-loaded static MD3 model,
// and real background BSP geometry visible through the cockpit's own
// window, respectively - see README.md's "vjun1's missing cockpit and NPC
// torso" section for the actual fixes). Kept anyway on its own merits.
#define MAX_VK_WORLD_DESCRIPTOR_SETS 65536
#define VK_FRAMES_IN_FLIGHT 2
#define UI_VERTEX_BUFFER_CAPACITY 4096u // quads per frame
// Fan-expanded triangle-list vertices per frame across every queued
// RE_AddPolyToScene poly (see VK_DrawScenePolys, tr_model.cpp) - not a
// "polys per frame" count like rd-vanilla's MAX_POLYS/MAX_POLYVERTS
// (tr_local.h in rd-vanilla), a generous scratch-buffer size instead.
#define POLY_VERTEX_BUFFER_CAPACITY 16384u
// Fan-expanded triangle-list vertices per frame across every weather
// particle actually drawn this frame (tr_weather.cpp) - sized generously
// against real preset particle counts (rd-vanilla/tr_WorldEffects.cpp: the
// largest single preset, rain/heavyrain, requests 1000-2000; hoth2's real
// "snow"+"fog" combo is 1000+60=1060), not a hard per-cloud limit - see
// MAX_WEATHER_PARTICLES_PER_CLOUD's own comment. 4096 particles' worth
// (WEATHER_VERTEX_BUFFER_CAPACITY / 6 verts-per-quad).
#define WEATHER_VERTEX_BUFFER_CAPACITY 24576u
// Per-cloud particle count cap - real presets top out at 2000 (rain/
// heavyrain/acidrain); generous headroom above that for any future preset,
// same spirit as MAX_SCENE_ENTITIES/MAX_SCENE_POLYS elsewhere in this
// renderer (a sane bound, not a measured hard engine limit).
#define MAX_WEATHER_PARTICLES_PER_CLOUD 4096
// Fan-expanded triangle-list vertices per frame across every static
// MST_FLARE surface actually drawn this frame (tr_world.cpp's
// VK_DrawWorldFlares) - a per-map load-time-fixed count, not a per-frame
// varying one like weather's, but sized the same way: comfortable headroom
// above the real numbers seen in this checkout's own test maps (hoth2: 98,
// vjun1: 45 - see rd-vulkan/README.md). 512 flares' worth
// (FLARE_VERTEX_BUFFER_CAPACITY / 6 verts-per-quad).
#define FLARE_VERTEX_BUFFER_CAPACITY 3072u

// Blend mode a UI draw needs, taken from the first stage of the image's
// matching .shader script (see tr_shader.cpp). Vulkan bakes blend factors
// into VkPipeline (unlike GL's dynamic glBlendFunc), so each mode here
// corresponds to a distinct VkPipeline variant - keep this list small and
// only add a mode once a pipeline for it actually exists.
enum vkBlendMode_t
{
	// Default for images with no matching .shader script - matches
	// rd-vanilla's implicit LIGHTMAP_2D fallback shader (tr_shader.cpp,
	// R_FindShader's "GUI elements" case) for a bare image reference.
	BLEND_ALPHA,	// blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
	BLEND_ADDITIVE,	// blendFunc GL_ONE GL_ONE / "add"
	// Default for a *defined* shader's first stage when it has no blendFunc
	// keyword at all - rd-vanilla treats a missing blendFunc as disabled
	// blending (tr_shader.cpp ParseStage: blendSrcBits/blendDstBits default
	// to 0, i.e. opaque overwrite), not as alpha blending.
	BLEND_OPAQUE,
};

struct image_t
{
	std::string name;
	int width = 0, height = 0;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	vkBlendMode_t blendMode = BLEND_ALPHA;
};

struct vkFrame_t
{
	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkSemaphore imageAvailable = VK_NULL_HANDLE;
	VkSemaphore renderFinished = VK_NULL_HANDLE;
	VkFence inFlight = VK_NULL_HANDLE;
};

// Layout must match ui.vert/ui.frag's `layout(push_constant)` block exactly.
// GLSL requires vec4 members to start on a 16-byte boundary, so - unlike a
// plain C++ struct - `color` cannot immediately follow a vec2 at byte 8; the
// compiler pads it out to byte 16. Getting this wrong doesn't error, it just
// makes the shader silently read the wrong bytes (color ends up reading
// this struct's own tail plus out-of-bounds memory past what was actually
// pushed via vkCmdPushConstants).
struct vkPushConstants_t
{
	float viewportSize[2];
	float _pad[2];
	float color[4];
};

// Static world geometry only (see tr_world.cpp): position, diffuse UV,
// lightmap UV, and a per-vertex colour. Still opaque, single-diffuse-
// texture-per-surface geometry - no normal, no dynamic lights - but lit by
// the map's baked lightmap (VK_LoadLightmaps) and, for vertex-lit surfaces
// specifically, this real baked per-vertex colour too (see `color`'s own
// comment) - together what actually makes it comparable to rd-vanilla's
// default (non-fullbright) output.
struct WorldVertex
{
	float pos[3];
	float uv[2];
	float lightmapUV[2];
	// Real BSP-baked vertex normal (drawVert_t::normal, qcommon/qfiles.h),
	// world-space (world geometry has no per-entity transform, so this needs
	// no further rotation before use). Always populated straight from the
	// BSP at load time (RE_LoadWorldMap) regardless of surface type - the
	// data is already sitting right next to xyz/st/lightmap in every
	// drawVert_t, so there's no reason to special-case which surfaces get
	// it. Only actually read by world.vert's reflection-vector computation
	// though, gated per-batch on WorldSurfaceBatch::envMap - every other
	// surface carries a real normal here that's simply never sampled.
	float normal[3];
	// dsurface_t/drawVert_t's real baked colour (style 0), normalized to
	// 0..1 - but only genuinely populated from BSP data for vertex-lit
	// surfaces (WorldSurfaceBatch::vertexLit, RE_LoadWorldMap); every other
	// vertex - real lightmapped world geometry, sky faces, Ghoul2 meshes -
	// gets a hardcoded (1,1,1,1) here instead of its own BSP-baked value
	// (lightmapped surfaces have one too, but real Quake3 shaders for them
	// use `rgbGen identityLighting`, not `rgbGen vertex`, so rd-vanilla
	// itself never actually applies it - this renderer doesn't parse
	// `rgbGen` at all, so hardcoding white for the surfaces where it
	// wouldn't apply anyway reaches the same visual result without needing
	// to). world.frag always multiplies by this unconditionally - a
	// (1,1,1,1) here is a true no-op, so no shader-side branch is needed to
	// tell vertex-lit and lightmapped/Ghoul2 vertices apart.
	float color[4];
};

// Runtime polys (RE_AddPolyToScene - particles, sparks, decals; see
// tr_model.cpp's VK_DrawScenePolys and shaders/poly.vert/poly.frag). World-
// space position, one UV set, and a per-vertex RGBA colour - matches
// polyVert_t (rd-common/tr_types.h) directly except colour is float 0..1
// here rather than that struct's byte 0..255 (see VK_DrawScenePolys for the
// /255 conversion), the same normalization world.frag's texture sampling
// already assumes.
struct PolyVertex
{
	float pos[3];
	float uv[2];
	float color[4];
};

// Layout must match world.vert/world.frag's `layout(push_constant) uniform
// PushConstants { mat4 mvp; vec4 camPos; vec4 fogColor; vec4 fogStart; vec4
// uvScale; }`. A plain column-major float[16] matches GLSL's mat4 layout
// directly (16-byte-aligned columns); the vec4 fields are already
// vec4-sized/aligned so no padding trap there either. fogColor[3] doubles
// as the fog's "opaque" distance (see VK_LoadWorldFog in tr_world.cpp) - 0
// disables fog entirely (the sky draw always passes 0 here; see
// RE_RenderScene), matching world.frag's `if (fogColor.a > 0.0)` gate.
// fogStart[0] is the world-space distance before which no fog applies at
// all (0 for the common case - fog ramps in starting right at the camera,
// same behaviour as before this field existed) - only nonzero when this
// batch's fog is the map's single global one AND "ranged fog" is active
// (VK_SetRangedFog/a worldspawn `linFogStart` key - see tr_world.cpp's own
// comment on s_rangedFog for what that's for and why it's unverified
// against this renderer's own test maps). fogStart[1]/fogStart[2] double
// again as this batch's tcMod scroll UV offset (world.vert adds them to
// the diffuse UV, after uvScale below is applied) - already-computed
// CPU-side as `scrollSpeed * currentTimeSeconds` (see RE_RenderScene), not
// the raw per-second speed itself, so world.vert never needs the current
// time as its own separate uniform. 0,0 for the common case (no `tcMod
// scroll` on this batch's shader - see VK_GetShaderTcModScroll,
// tr_shader.cpp). fogStart[3] is unused padding. camPos[3] similarly
// doubles as a per-draw overbright factor (world.frag's comment): 2.0 for
// real BSP lightmapped world/sky geometry (baked assuming this doubling -
// Quake3's "overbright bits"), 1.0 for Ghoul2 draws (tr_model.cpp), which
// are paired with a plain white placeholder in the lightmap slot rather
// than an actual baked-and-compensated one - doubling that too silently
// rendered every character twice as bright as its own diffuse texture, a
// real, user-reported bug (Vulkan screenshots reading much brighter than
// rd-vanilla's), not a deliberate simplification. uvScale.xy is this
// batch's `tcMod scale` multiplier (world.vert multiplies the diffuse UV
// by it before adding the scroll offset above) - 1.0,1.0 (a true no-op)
// for the common case (no `tcMod scale` on this batch's shader). uvScale.z
// is the tcGen-environment flag (VK_GetShaderTcGenEnvironment, tr_shader.cpp)
// - 1.0 makes world.vert ignore inUV/uvScale.xy entirely and instead
// generate UVs per-vertex from the vertex normal and camPos (real
// RB_CalcEnvironmentTexCoords, rd-vanilla's tr_shade_calc.cpp); 0.0 (the
// common case) draws ordinary UVs exactly as before. uvScale.w is unused
// padding. Every call site must set camPos[3] AND uvScale.xy explicitly -
// a zero-initialized push (`= {}`) defaults camPos[3] to 0.0 (multiplies
// the surface to solid black, not "no overbright") and uvScale.xy to
// 0.0,0.0 (multiplies every UV to (0,0), not "no rescale") - both are
// silent-black/silent-wrong-texture bugs, not crashes, so a missed call
// site is easy to overlook without a direct screenshot check. uvScale.z
// is safe to leave at a zero-initialized push's default 0.0 (ordinary UVs,
// the correct behaviour for every call site except world geometry's own
// per-batch loop, which sets it explicitly either way for clarity).
struct vkWorldPushConstants_t
{
	float mvp[16];
	float camPos[4];
	float fogColor[4];
	float fogStart[4];
	float uvScale[4];
};

typedef struct
{
	qboolean registered = qfalse;

	glconfig_t glConfig = {};

	SDL_Window *window = nullptr;

	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties physicalDeviceProps = {};
	VkDevice device = VK_NULL_HANDLE;
	uint32_t graphicsQueueFamily = 0;
	VkQueue graphicsQueue = VK_NULL_HANDLE;

	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D swapchainExtent = {};
	std::vector<VkImage> swapchainImages;
	std::vector<VkImageView> swapchainImageViews;
	std::vector<VkFramebuffer> swapchainFramebuffers;

	VkRenderPass renderPass = VK_NULL_HANDLE;

	// 2D (UI/font) draw path - the only path that actually draws anything yet
	VkDescriptorSetLayout uiDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool uiDescriptorPool = VK_NULL_HANDLE;
	VkPipelineLayout uiPipelineLayout = VK_NULL_HANDLE;
	VkPipeline uiPipeline = VK_NULL_HANDLE;		// BLEND_ALPHA
	VkPipeline uiPipelineAdditive = VK_NULL_HANDLE;	// BLEND_ADDITIVE
	VkPipeline uiPipelineOpaque = VK_NULL_HANDLE;		// BLEND_OPAQUE
	VkSampler uiSampler = VK_NULL_HANDLE;
	VkBuffer uiVertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory uiVertexBufferMemory = VK_NULL_HANDLE;
	void *uiVertexBufferMapped = nullptr;

	// Depth buffer - one persistent image reused across frames (safe because
	// RE_EndFrame fully serializes frames with vkQueueWaitIdle already, so
	// there's never more than one frame's draws in flight against it).
	VkFormat depthFormat = VK_FORMAT_UNDEFINED;
	VkImage depthImage = VK_NULL_HANDLE;
	VkDeviceMemory depthImageMemory = VK_NULL_HANDLE;
	VkImageView depthImageView = VK_NULL_HANDLE;

	// 3D world geometry draw path (tr_world.cpp) - opaque, lightmapped,
	// unculled static BSP surfaces only, see README.md for exactly what
	// that means. Its own descriptor set layout/pool (distinct from the UI
	// path's) because each draw needs two bound textures (diffuse +
	// lightmap), not the UI path's one.
	VkDescriptorSetLayout worldDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool worldDescriptorPool = VK_NULL_HANDLE;
	VkPipelineLayout worldPipelineLayout = VK_NULL_HANDLE;
	VkPipeline worldPipeline = VK_NULL_HANDLE;
	// Same layout/vertex format as worldPipeline, but depth test/write both
	// off - the skybox (tr_world.cpp: VK_LoadSky) is drawn camera-centered,
	// before normal world geometry, and must never occlude or be occluded
	// by anything; ordinary depth-tested world geometry drawn afterward
	// naturally overdraws it wherever real geometry exists.
	VkPipeline skyPipeline = VK_NULL_HANDLE;
	// Same layout/vertex format/shaders as worldPipeline - only the blend
	// state and depth-write differ, matching vk.polyPipeline/
	// polyPipelineAdditive's reasoning exactly: Vulkan bakes blend factors
	// into the pipeline, so a per-surface .shader blendFunc (see
	// WorldSurfaceBatch::blendMode, tr_world.cpp) needs a distinct
	// VkPipeline, selected per-batch in RE_RenderScene the same way
	// vertexLit/fogIndex/scroll already are. Depth-tested against opaque
	// geometry (so a translucent surface is still occluded by a wall in
	// front of it) but doesn't write depth itself - the common, simple
	// approximation for translucent geometry this renderer already uses
	// for runtime polys (vk.polyPipeline's own comment), not per-shader
	// real depth-write control or back-to-front sorting between
	// translucent surfaces.
	VkPipeline worldPipelineAlpha = VK_NULL_HANDLE;    // BLEND_ALPHA
	VkPipeline worldPipelineAdditive = VK_NULL_HANDLE; // BLEND_ADDITIVE
	VkSampler worldSampler = VK_NULL_HANDLE;
	// Separate pool, same vk.worldDescriptorSetLayout/vk.worldSampler -
	// Ghoul2 model descriptor sets (tr_model.cpp) must NOT come from
	// vk.worldDescriptorPool: that pool is reset on every RE_LoadWorldMap
	// (see VK_ShutdownWorld), but a loaded Ghoul2 model - and the
	// descriptor sets its surfaces hold - is cached by filename and expected
	// to survive a world reload (e.g. the player model persisting across a
	// level transition), same as vk.images. This pool is only reset at
	// renderer shutdown (VK_ShutdownGhoul2Models), matching that lifetime.
	VkDescriptorPool ghoul2DescriptorPool = VK_NULL_HANDLE;

	// Runtime polys (RE_AddPolyToScene, tr_model.cpp's VK_DrawScenePolys) -
	// world-space geometry like worldPipeline, but reuses vk.uiDescriptorSetLayout/
	// vk.uiSampler (one plain texture, no lightmap - same shape the UI path
	// already needs) rather than a third descriptor set layout, since every
	// image_t already carries its own descriptorSet built against that exact
	// layout (see VK_UploadImage, tr_image.cpp) - nothing new to allocate
	// per poly draw. Three blend-mode variants, same reasoning and the same
	// three modes as vk.uiPipeline/uiPipelineAdditive/uiPipelineOpaque
	// (vkBlendMode_t) - Vulkan bakes blend factors into the pipeline, so a
	// distinct .shader blendFunc needs a distinct VkPipeline. Depth test
	// against world/Ghoul2 geometry is on (polys should be occluded by a
	// wall, not drawn through it); depth write is off for all three variants
	// uniformly, even the opaque one - not real per-shader depth-write
	// control, just the common case for transient effect geometry.
	VkPipelineLayout polyPipelineLayout = VK_NULL_HANDLE;
	VkPipeline polyPipeline = VK_NULL_HANDLE;         // BLEND_ALPHA
	VkPipeline polyPipelineAdditive = VK_NULL_HANDLE; // BLEND_ADDITIVE
	VkPipeline polyPipelineOpaque = VK_NULL_HANDLE;   // BLEND_OPAQUE
	// Host-visible/coherent, persistently mapped, rewritten fresh every
	// frame (VK_DrawScenePolys resets its write cursor to 0 each call) -
	// same "per-frame CPU scratch buffer" pattern as vk.uiVertexBuffer, not
	// a one-time upload like tr_world.cpp's static geometry.
	VkBuffer polyVertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory polyVertexBufferMemory = VK_NULL_HANDLE;
	void *polyVertexBufferMapped = nullptr;

	// Weather particles (tr_weather.cpp) - rain/snow/fog/etc world effects.
	// Same PolyVertex layout and per-frame-scratch-buffer pattern as
	// vk.polyVertexBuffer above (and deliberately reuses vk.polyPipeline/
	// vk.polyPipelineAdditive/vk.polyPipelineLayout wholesale rather than
	// creating dedicated ones - identical vertex format, identical depth-
	// test-on/depth-write-off/blend-mode shape already covers exactly what
	// camera-facing particle billboards need), but its own separate buffer
	// so weather's per-frame write cursor can never collide with
	// VK_DrawScenePolys' own (RE_AddPolyToScene decals, sprite/oriented-quad
	// entities, electricity, ...) - two independent systems sharing one
	// pipeline's compiled state, not one buffer's write cursor.
	VkBuffer weatherVertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory weatherVertexBufferMemory = VK_NULL_HANDLE;
	void *weatherVertexBufferMapped = nullptr;

	// Static-geometry flares (MST_FLARE, tr_world.cpp's VK_DrawWorldFlares) -
	// same PolyVertex layout, per-frame-scratch-buffer pattern, and reused
	// vk.polyPipeline/polyPipelineAdditive/polyPipelineLayout as weather
	// above, own buffer for the same "independent write cursor" reason.
	// Depth-test-on/depth-write-off (already true of polyPipeline) is what
	// gives flares real per-pixel occlusion against whatever world/Ghoul2
	// geometry the depth buffer already holds by the time this draws - no
	// separate depth-readback pass needed, unlike rd-vanilla's real
	// RB_TestZFlare (see VK_DrawWorldFlares' own comment).
	VkBuffer flareVertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory flareVertexBufferMemory = VK_NULL_HANDLE;
	void *flareVertexBufferMapped = nullptr;

	// Shared across tr_cmds.cpp's and tr_world.cpp's draw calls (both bind
	// pipelines into the same per-frame command buffer) so a pipeline bound
	// by one never gets silently assumed still-bound by the other. Reset to
	// VK_NULL_HANDLE at the start of every frame in RE_BeginFrame.
	VkPipeline lastBoundPipeline = VK_NULL_HANDLE;

	vkFrame_t frames[VK_FRAMES_IN_FLIGHT];
	uint32_t currentFrame = 0;
	uint32_t currentSwapchainImage = 0;
	VkCommandBuffer activeCommandBuffer = VK_NULL_HANDLE;
	bool frameActive = false;

	float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float drawColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	std::vector<image_t *> images;
	std::unordered_map<std::string, qhandle_t> imagesByName;
	image_t *whiteImage = nullptr;
	// Returned by RegisterShader*() when the real image/shader can't be
	// resolved (e.g. a videoMap or other .shader-script-only reference this
	// renderer doesn't parse yet - see README.md). Deliberately NOT the same
	// as handle 0 ("white", a real API convention many callers rely on
	// intentionally) - falling back to white for a failed lookup previously
	// painted an opaque white square over part of the menu.
	image_t *transparentImage = nullptr;

} vkGlobals_t;

extern vkGlobals_t vk;

// tr_init.cpp
void VK_Shutdown( qboolean destroyWindow );
VkShaderModule VK_CreateShaderModule( const uint32_t *code, size_t codeSize );
void VK_Check( VkResult r, const char *what );
void VK_CreateBuffer( VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
	VkBuffer *buffer, VkDeviceMemory *memory );
VkCommandBuffer VK_BeginOneShotCommands( void );
void VK_EndOneShotCommands( VkCommandBuffer cmd );

// tr_world.cpp
//
// Static world geometry only - see WorldVertex/README.md for exactly what
// this does and does not draw (no lighting, no BSP culling). Entities (see
// tr_model.cpp) are drawn from RE_RenderScene too, but queued/tracked
// separately.
void RE_LoadWorldMap( const char *name );
void RE_RenderScene( const refdef_t *fd );
void VK_ShutdownWorld( void );
// Static MST_FLARE surfaces parsed by RE_LoadWorldMap - see this function's
// own comment (tr_world.cpp) for the full picture; RE_RenderScene is the
// only caller.
void VK_DrawWorldFlares( const float *mvp, const refdef_t *fd );
// Small helpers implemented in tr_world.cpp but reused by tr_model.cpp
// (Ghoul2 models are just more indexed triangle batches drawn through the
// same pipeline/descriptor-set shape as world surfaces - see tr_model.cpp's
// file header) rather than duplicated.
void VK_UploadDeviceLocalBuffer( const void *data, VkDeviceSize size, VkBufferUsageFlags usage,
	VkBuffer *outBuffer, VkDeviceMemory *outMemory );
VkDescriptorSet VK_BuildWorldDescriptorSet( VkDescriptorPool pool, image_t *diffuse, image_t *lightmap );
// out = b * a when a/b/out are read as column-major matrices - see this
// function's definition in tr_world.cpp for the full explanation of why the
// argument order is backwards from what the name suggests.
void VK_MultiplyMatrix( const float *a, const float *b, float *out );
// Used only by tr_weather.cpp's VK_SetTempGlobalFogColor, to temporarily
// override (and later restore) the world's real global fog colour without
// tr_weather.cpp reaching into tr_world.cpp's own file-static fog state
// directly.
bool VK_HasWorldFog( void );
void VK_GetWorldFogColor( float outColor[3] );
void VK_SetWorldFogColor( const float color[3] );
// Real implementation of the "ranged fog" API (RE_SetRangedFog, tr_init.cpp)
// - see tr_world.cpp's own comment on s_rangedFog for the full picture
// (a sniper-scope-style widening of the map's single global fog's near/far
// transition, ported from rd-vanilla's real RE_SetRangedFog/
// RB_IterateStagesGeneric) and why it's implemented but unverified against
// any of this renderer's own test maps.
void VK_SetRangedFog( float dist );

// tr_model.cpp
//
// Ghoul2 (character/weapon model) rendering - bind pose only, see README.md
// for exactly what that means and what's still missing (animation, bolts,
// LOD selection, surface on/off overrides, gore).
void RE_ClearScene( void );
void RE_AddRefEntityToScene( const refEntity_t *re );
// Queues a copy of numVerts polyVert_t (a triangle fan, vertex 0 is the
// pivot - matches rd-vanilla's RB_SurfacePolychain convention) under hShader
// for drawing this scene. Copies immediately, same reason as rd-vanilla's
// real RE_AddPolyToScene (tr_scene.cpp): callers routinely reuse/free verts
// right after this call returns.
void RE_AddPolyToScene( qhandle_t hShader, int numVerts, const polyVert_t *verts );
void VK_DrawGhoul2Entities( const float *mvp, int currentTime );
// Draws every poly queued this scene via RE_AddPolyToScene, plus every
// RT_SPRITE/RT_ORIENTED_QUAD entity queued via RE_AddRefEntityToScene - see
// its own comment in tr_model.cpp for the fan-to-triangle-list expansion,
// the quad-stamp math, and why both share one function/vertex-buffer
// cursor. fd is only used for its viewaxis (camera-facing sprites need the
// camera's own left/up basis vectors) - mvp is still passed separately
// since it's already computed by the caller.
void VK_DrawScenePolys( const float *mvp, const refdef_t *fd );
void VK_ShutdownGhoul2Models( void );
// Non-Ghoul2 static models (.md3) - map set pieces (misc_model_static),
// weapon world models, gibs/props. Loads (or returns the cached index of)
// the .md3 at fileName; single LOD, single frame (see VulkanStaticModel's
// comment, tr_model.cpp, for exactly why that's enough for the confirmed
// real-world case - vjun1's missing cockpit interior, see README.md). 0 on
// failure, same convention as VK_LoadGhoul2Model. Called from
// RE_RegisterModel (tr_init.cpp), which offsets a successful return by
// VK_STATIC_MODEL_HANDLE_BASE before handing it back as a qhandle_t - see
// that constant's own comment for why.
int VK_LoadMD3Model( const char *fileName );
// Real R_ModelBounds for a static .md3 handle (VK_STATIC_MODEL_HANDLE_BASE-
// offset) - see this function's own comment (tr_model.cpp) and R_ModelBounds
// (tr_init.cpp), the only caller, for what "real" means here and why every
// other handle correctly still gets zero. Returns false (mins/maxs
// untouched) for any handle this doesn't recognize.
bool VK_ModelBounds( qhandle_t handle, float mins[3], float maxs[3] );
// RE_RegisterModel's generic qhandle_t space previously meant nothing (every
// call returned the same fake "1", see RE_RegisterModel's own comment) - now
// that a real .md3 model cache index is sometimes returned instead, it needs
// to be distinguishable from that fake handle and from every other qhandle_t
// space (images, Ghoul2 models, skins - each already has its own, per this
// file's existing convention) sharing the same C `int` type. Chosen larger
// than any realistic model count so a real index i>0 is returned as
// VK_STATIC_MODEL_HANDLE_BASE+i, never colliding with the fallback "1" or
// with any other in-range handle. ent.hModel (refEntity_t) is otherwise
// completely unused by this renderer - Ghoul2 entities are matched via
// ent.ghoul2, not ent.hModel - so this is the only reader of it.
#define VK_STATIC_MODEL_HANDLE_BASE 1000000
// Loads (or returns the cached index of, if already loaded with the same
// skinHandle) the .glm at fileName. skinHandle (from VK_RegisterSkin, or 0
// for "no skin") selects per-surface texture overrides for models whose own
// embedded shader names are empty - see VulkanSkin's comment in tr_model.cpp
// for why that's the common case for humanoid models. Returns 0 - never a
// valid index, see the model cache's comment in tr_model.cpp - if the file
// can't be read/parsed or has no drawable surfaces. Called from
// G2API_InitGhoul2Model below.
int VK_LoadGhoul2Model( const char *fileName, int skinHandle );
// Parses a .skin file into a surfacename->shader override map (see
// VulkanSkin in tr_model.cpp), returns a handle usable as VK_LoadGhoul2Model's
// skinHandle. Called from RE_RegisterSkin below.
int VK_RegisterSkin( const char *name );
// Resolves a Ghoul2 surface name to its original mdxmSurfHierarchy_t index
// (case-insensitive, matching real G2_IsSurfaceLegal - tr_model.cpp for the
// only caller, G2API_SetSurfaceOnOff, tr_init.cpp) and, if found, writes
// that surface's baked-default G2SURFACEFLAG_* flags to *outBaseFlags.
// Returns -1 if not found (unknown name, or modelCacheIndex isn't a real
// Ghoul2 model - a VulkanStaticModel's empty surfaceNames always misses).
int VK_FindGhoul2SurfaceIndex( int modelCacheIndex, const char *surfaceName, unsigned int *outBaseFlags );
// Bolt support (see VulkanSkeleton's comment in tr_model.cpp) - used by
// G2API_AddBolt/G2API_GetBoltMatrix below. modelCacheIndex is a
// VK_LoadGhoul2Model return value (i.e. CGhoul2Info::mModel).
// VK_FindGhoul2Bone returns -1 if not found (same contract as
// G2API_AddBolt). VK_GetGhoul2BoneBasePoseMat (rest pose) and
// VK_GetGhoul2BoneCurrentPoseMat (this instance's actual currently animated
// pose - see its own comment, tr_model.cpp, for why G2API_GetBoltMatrix
// needs this one, not the rest-pose one) both return false (leaving *out
// untouched) on any invalid index.
int VK_FindGhoul2Bone( int modelCacheIndex, const char *boneName );
bool VK_GetGhoul2BoneBasePoseMat( int modelCacheIndex, int boneIndex, mdxaBone_t *out );
bool VK_GetGhoul2BoneCurrentPoseMat( int modelCacheIndex, const CGhoul2Info *ghlInfo, int boneIndex, int currentTime, mdxaBone_t *out );
// Surface-bolt counterpart to VK_GetGhoul2BoneCurrentPoseMat, for a bolt
// whose boltInfo_t has a surfaceNumber instead of a boneNumber - see this
// function's own comment (tr_model.cpp) for the real formula it ports and
// why. Returns false (leaving *out untouched) if surfIndex isn't a real
// G2SURFACEFLAG_ISBOLT surface on this model (VulkanGhoul2Model::
// tagTriangles has no entry for it) or any other invalid input.
bool VK_GetGhoul2SurfaceBoltMatrix( int modelCacheIndex, int surfIndex, const CGhoul2Info *ghlInfo, int currentTime, mdxaBone_t *out );
// CGhoul2Info::mModelBoltLink's bit-packing (model-to-model attachment,
// G2API_AttachG2Model/G2API_DetachG2Model in tr_init.cpp, consumed at draw
// time by VK_DrawGhoul2Entities in tr_model.cpp) - same encoding as
// rd-vanilla's real MODEL_SHIFT/BOLT_SHIFT/MODEL_AND/BOLT_AND (ghoul2/G2.h),
// copied verbatim rather than rederived since a bit-packing width subtly
// wrong is easy to get wrong and hard to notice. Shared here (rather than
// duplicated) so the encode side (tr_init.cpp) and decode side
// (tr_model.cpp) can never drift apart.
static const int kG2ModelWidth = 10;
static const int kG2BoltWidth = 10;
static const int kG2BoltShift = 0;
static const int kG2ModelShift = kG2BoltShift + kG2BoltWidth;
static const int kG2ModelAnd = ( 1 << kG2ModelWidth ) - 1;
static const int kG2BoltAnd = ( 1 << kG2BoltWidth ) - 1;
// The .gla's own recorded name (mdxaHeader_t::name, e.g.
// "models/players/_humanoid/_humanoid" - no extension), read straight out
// of VulkanSkeleton::fileData (kept resident for exactly this kind of
// on-demand header access - see that field's comment). Backs
// G2API_GetGLAName below; real callers (NPC_stats.cpp's G_LoadAnimFileSet,
// g_client.cpp, g_main.cpp) use this to find a model's animation.cfg by
// skeleton name, so a wrong or hardcoded-always-"_humanoid" answer here
// silently breaks animation.cfg lookup for every non-humanoid model
// (droids, creatures) even once real per-model resolution is otherwise
// working - see G2API_GetGLAName's own comment (tr_init.cpp). Returns
// nullptr for an invalid modelCacheIndex, same convention as
// VK_FindGhoul2Bone's -1.
const char *VK_GetGhoul2GLAName( int modelCacheIndex );
// Computes every bone's object-space pose matrix for one instance right
// now (see VK_ComputeGhoul2Pose's own comment in tr_model.cpp for the real,
// verified-against-rd-vanilla math, the per-bone hierarchy-inheritance
// resolution, and its remaining deliberate scope cuts). skeletonIndex is
// VK_LoadGhoul2Skeleton's return value, not a model cache index. Clears
// and leaves outBones empty on any invalid input. attachBase, if non-null,
// replaces the fixed root-rotation constant every root bone would
// otherwise seed its hierarchy walk with - see VK_DrawGhoul2Entities's own
// comment ("Ghoul2 model-to-model attachment") for the one real case that
// needs this: a sub-model attached to a bolt on a sibling sub-model within
// the same entity (G2API_AttachG2Model) is seeded with that bolt's current
// matrix instead of the ordinary fixed root rotation, so its own hierarchy
// composes relative to the bolt rather than the entity's raw origin/axis.
void VK_ComputeGhoul2Pose( int skeletonIndex, const CGhoul2Info *ghlInfo, int currentTime, std::vector<mdxaBone_t> &outBones, const mdxaBone_t *attachBase = nullptr );
// Per-level animation-file-override handle space (G2API_SetAnimIndex/
// GetAnimIndex/PrecacheGhoul2Model, tr_init.cpp) - see
// VK_PrecacheGhoul2AnimHandle's own comment (tr_model.cpp) for the real
// mechanism this backs. animNameNoExt has no ".gla" extension (same
// convention as VK_LoadGhoul2Skeleton, which this calls internally).
// Returns 0 (falsy) if the file doesn't exist, same as a failed
// RE_RegisterModel in real vanilla.
int VK_PrecacheGhoul2AnimHandle( const char *animNameNoExt );
// Live per-instance, per-bone animation state (see VulkanGhoul2AnimState's
// comment in tr_model.cpp for the real scope/simplifications) - backs
// G2API_SetBoneAnim/GetBoneAnim/PauseBoneAnim/IsPaused/StopBoneAnim below.
// ghlInfo is the exact CGhoul2Info pointer those G2API calls receive, used
// as an opaque identity key (this renderer never dereferences it itself -
// only G2API_GetBoneIndex, tr_init.cpp, does, to resolve a bone name via
// ghlInfo->mModel). boneIndex is the real skeleton bone index this state
// applies to (resolved by the caller - the By-name G2API variants resolve
// it via VK_FindGhoul2Bone before calling these; the ...Index variants
// already have one, now that G2API_GetBoneIndex actually returns real
// indices instead of always -1 - see its own comment for why that
// mattered far beyond just animation).
// setFrame (-1 = start fresh at startFrame) and blendTime (only meaningful
// with the real BONE_ANIM_BLEND flag bit set in flags) are real parameters
// now, not ignored - see this function's own comment (tr_model.cpp) for the
// exact rd-vanilla-matching arithmetic (continuity across a re-affirmed
// anim, and cross-fading from whatever was previously playing on this bone
// over blendTime milliseconds).
void VK_SetGhoul2BoneAnim( const CGhoul2Info *ghlInfo, int boneIndex, int startFrame, int endFrame, int flags, float animSpeed, int startTime, float setFrame, int blendTime );
bool VK_GetGhoul2BoneAnim( const CGhoul2Info *ghlInfo, int boneIndex, int currentTime, float *currentFrame, int *startFrame, int *endFrame, int *flags, float *animSpeed );
bool VK_PauseGhoul2BoneAnim( const CGhoul2Info *ghlInfo, int boneIndex, int currentTime );
bool VK_IsGhoul2BoneAnimPaused( const CGhoul2Info *ghlInfo, int boneIndex );
bool VK_StopGhoul2BoneAnim( const CGhoul2Info *ghlInfo, int boneIndex );
// Real G2API_SetBoneAngles/SetBoneAnglesIndex (tr_init.cpp) - see this
// function's own comment (tr_model.cpp) for the real G2_Generate_Matrix
// formula it ports, and why only the BONE_ANGLES_POSTMULT flag is
// implemented (the only one any real call site in this game's own code
// ever uses). Returns false (silent no-op, leaving any previously-set
// override for this bone unchanged) for an unresolvable ghlInfo/boneIndex
// or a flags value without POSTMULT set.
bool VK_SetGhoul2BoneAngles( const CGhoul2Info *ghlInfo, int boneIndex, const vec3_t angles, int flags, Eorientations up, Eorientations left, Eorientations forward );

// tr_shader.cpp
//
// Minimal .shader script support: just enough to recover the blend mode a
// shader's first stage wants (so RE_StretchPic, tr_cmds.cpp, can pick the
// matching VkPipeline), its fogparms (world fog volumes), and its first
// stage's tcMod scroll (world-geometry scrolling textures) - see
// README.md. Does NOT implement multi-stage compositing, any other tcMod
// type, rgbGen/alphaGen waves, sky, or anything beyond that.
void VK_LoadShaderScripts( void );
// notFoundDefault only applies when `name` has no `.shader` script block at
// all (a bare texture reference, the common case for ordinary wall/floor
// textures) - see this function's own comment (tr_shader.cpp) for why the
// 2D UI path (RE_StretchPic, its default caller) and 3D world geometry
// (RE_LoadWorldMap, tr_world.cpp) need genuinely different defaults for
// that specific case, not just different callers of the same answer.
vkBlendMode_t VK_GetShaderBlendMode( const char *name, vkBlendMode_t notFoundDefault = BLEND_ALPHA );
bool VK_GetShaderFogParms( const char *name, float color[3], float *opaqueDist );
// See this function's own comment (tr_shader.cpp) for what's actually
// supported (`scroll` and `scale`, and how they compose when a stage has
// both) and the real test cases that motivated each.
bool VK_GetShaderTcModScroll( const char *name, float *sSpeed, float *tSpeed, float *scaleS, float *scaleT );
// First stage's `map`/`clampmap` file path, or nullptr - see this
// function's own comment (tr_shader.cpp) for why RE_LoadWorldMap
// (tr_world.cpp) needs this as a fallback when a shader's own name isn't
// directly a texture file.
const char *VK_GetShaderMapImage( const char *name );
// First stage's `alphaGen portal <range>` numeric argument, or rd-vanilla's
// own RB_SurfaceFlare default (30) if absent - see this function's own
// comment (tr_shader.cpp) and RE_LoadWorldMap's MST_FLARE handling
// (tr_world.cpp), the only caller.
float VK_GetShaderPortalRange( const char *name );
// First stage's `rgbGen const ( r g b )` colour - see this function's own
// comment (tr_shader.cpp); two real callers now: RE_LoadWorldMap's
// per-vertex colour overwrite for ordinary world surfaces, and
// VK_DrawWorldFlares for flares (both tr_world.cpp).
bool VK_GetShaderRgbGenConst( const char *name, float color[3] );
// First stage's `rgbGen wave sin <base> <amp> <phase> <freq>` (sin only) -
// see this function's own comment (tr_shader.cpp) and VK_DrawWorldFlares
// (tr_world.cpp), the only caller.
bool VK_GetShaderRgbWave( const char *name, float *base, float *amp, float *phase, float *freq );
// Whether a shader's first stage declares any `tcGen` keyword at all - see
// this function's own comment (tr_shader.cpp) and RE_LoadWorldMap's map-
// image-fallback safety gate (tr_world.cpp), the only caller.
bool VK_ShaderHasTcGen( const char *name );
// Whether a shader's first stage declares `tcGen environment` specifically -
// see this function's own comment (tr_shader.cpp) and RE_LoadWorldMap (the
// only caller), which flags matching world surfaces for real per-vertex
// reflection-mapped UV generation (world.vert) instead of ordinary UVs.
bool VK_GetShaderTcGenEnvironment( const char *name );

// tr_weather.cpp
//
// World weather/particle effects (rain, snow, wind-blown fog/dust/sand) -
// a fresh, scoped port of rd-vanilla's real tr_WorldEffects.cpp
// (CParticleCloud/CWindZone/COutside), not a reuse (that file leans on
// Raven's own Ravl/Ratl container library, not part of this checkout - see
// README.md's "Ghoul2 is not reused from rd-vanilla" for the same reasoning
// applied here). Confirmed real motivation: hoth2's blizzard (`fx_snow` +
// `fx_wind` map entities - g_fx.cpp's SP_CreateSnow/SP_CreateWind - send
// "snow"/"fog"/"constantwind"/"gustingwind" through exactly these entry
// points) and vjun1's exterior acid rain (`fx_rain`, spawnflags 8 ->
// "acidrain") - see README.md's own weather section for what's a faithful
// port vs a deliberate simplification (there are several: no persistent
// disk-cached outside/inside grid - every "is this point outside" query is
// a live ri.CM_PointContents call instead, real per-vertex-lit-surface
// interaction aside; wind-zone retargeting drives off real elapsed time
// rather than an assumed-per-frame tick count; no MakeNormalVectors/
// VectorNormalize reuse from a shared math file since none of this
// renderer's other code needed them either - small self-contained
// equivalents live in this file instead).
void VK_InitWorldEffects( void );
void VK_ShutdownWorldEffects( void );
// Parses and applies one weather console/script command - see
// tr_weather.cpp's own comment on VK_WorldEffectCommand for the full
// supported command list (mirrors rd-vanilla's R_WorldEffectCommand
// exactly: clear/freeze/zone/wind/constantwind/gustingwind/windzone/
// lightrain/rain/acidrain/heavyrain/snow/spacedust/sand/fog/heavyrainfog/
// light_fog/outsideshake/outsidepain).
void VK_WorldEffectCommand( const char *command );
// Accepted and ignored - see this function's own comment (tr_weather.cpp)
// for why zones (which only matter for rd-vanilla's cached outside/inside
// grid) are a no-op once every "is this point outside" query is answered
// live instead.
void VK_AddWeatherZone( vec3_t mins, vec3_t maxs );
// Called once per rendered scene from RE_RenderScene (tr_world.cpp), same
// call site as rd-vanilla's real RE_RenderWorldEffects (tr_scene.cpp) -
// updates every active particle cloud/wind zone by real elapsed time since
// the last call, then draws whatever's left rendering after that update.
void VK_DrawWeatherEffects( const float *mvp, const refdef_t *fd );
bool VK_GetWindVector( vec3_t windVector, vec3_t atPoint );
bool VK_GetWindGusting( vec3_t atPoint );
bool VK_IsOutside( vec3_t pos );
float VK_IsOutsideCausingPain( vec3_t pos );
float VK_GetChanceOfSaberFizz( void );
bool VK_IsShaking( vec3_t pos );
// Temporarily overrides the world's real global fog colour (tr_world.cpp -
// e.g. a lightning flash tinting fog red) - color[0..2] all zero restores
// the original. Returns false (no-op) if this map has no global fog at
// all, same convention as rd-vanilla's real function.
bool VK_SetTempGlobalFogColor( vec3_t color );

// tr_image.cpp
image_t *VK_FindImage( const char *name );
image_t *VK_CreateSolidImage( const char *name, byte r, byte g, byte b, byte a );
void VK_UploadImage( image_t *img, const byte *pixels, int width, int height );
void VK_ShutdownImages( void );
// hShader == 0 (RE_RegisterShader's failure return) or an out-of-range
// index both fall back to vk.whiteImage rather than a null dereference -
// shared by every draw path that resolves a qhandle_t to an image_t*
// (RE_StretchPic, VK_DrawScenePolys, ...).
image_t *VK_GetImageByHandle( qhandle_t hShader );
qhandle_t RE_RegisterShaderNoMip( const char *name );
qhandle_t RE_RegisterShader( const char *name );

// tr_cmds.cpp
void RE_StretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader );
// Shared 2D quad submission (arbitrary 4 corners, not just axis-aligned) -
// RE_StretchPic's own real implementation, and RE_DrawRotatePic/
// RE_DrawRotatePic2's real callers (tr_init.cpp), both reduce to this.
void VK_DrawQuad( float x0, float y0, float u0, float v0,
	float x1, float y1, float u1, float v1,
	float x2, float y2, float u2, float v2,
	float x3, float y3, float u3, float v3,
	qhandle_t hShader );
void RE_SetColor( const float *rgba );
void RE_BeginFrame( stereoFrame_t stereoFrame );
void RE_EndFrame( int *frontEndMsec, int *backEndMsec );
void RE_GetScreenShot( byte *buffer, int w, int h );
void R_ScreenShotPNG_f( void );
void VK_DestroyReadbackImage( void );
// Real swapchain recreation - see this function's own comment (tr_init.cpp)
// for the exact frozen-rendering bug it fixes and RE_BeginFrame's own
// comment (tr_cmds.cpp) for both call sites.
void VK_RecreateSwapchain( void );
