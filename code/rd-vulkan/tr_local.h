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

#define MAX_VK_IMAGES 4096
#define VK_FRAMES_IN_FLIGHT 2
#define UI_VERTEX_BUFFER_CAPACITY 4096u // quads per frame

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

// Static world geometry only (see tr_world.cpp): position, diffuse UV, and
// lightmap UV. Still opaque, single-diffuse-texture-per-surface geometry -
// no vertex color/normal, no dynamic lights - but lit by the map's baked
// lightmap, which is what actually makes it comparable to rd-vanilla's
// default (non-fullbright) output. See VK_LoadLightmaps in tr_world.cpp.
struct WorldVertex
{
	float pos[3];
	float uv[2];
	float lightmapUV[2];
};

// Layout must match world.vert's `layout(push_constant) uniform PushConstants
// { mat4 mvp; }`. A plain column-major float[16] matches GLSL's mat4 layout
// directly (16-byte-aligned columns), unlike vkPushConstants_t above there's
// no vec4-after-vec2 padding trap here since it's the only member.
struct vkWorldPushConstants_t
{
	float mvp[16];
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

// tr_model.cpp
//
// Ghoul2 (character/weapon model) rendering - bind pose only, see README.md
// for exactly what that means and what's still missing (animation, bolts,
// LOD selection, surface on/off overrides, gore).
void RE_ClearScene( void );
void RE_AddRefEntityToScene( const refEntity_t *re );
void VK_DrawGhoul2Entities( const float *mvp );
void VK_ShutdownGhoul2Models( void );
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

// tr_shader.cpp
//
// Minimal .shader script support: just enough to recover the blend mode a
// shader's first stage wants, so RE_StretchPic (tr_cmds.cpp) can pick the
// matching VkPipeline. Does NOT implement multi-stage compositing, tcMod
// animation, rgbGen, sky/fog, or anything beyond that - see README.md.
void VK_LoadShaderScripts( void );
vkBlendMode_t VK_GetShaderBlendMode( const char *name );

// tr_image.cpp
image_t *VK_FindImage( const char *name );
image_t *VK_CreateSolidImage( const char *name, byte r, byte g, byte b, byte a );
void VK_UploadImage( image_t *img, const byte *pixels, int width, int height );
void VK_ShutdownImages( void );
qhandle_t RE_RegisterShaderNoMip( const char *name );
qhandle_t RE_RegisterShader( const char *name );

// tr_cmds.cpp
void RE_StretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader );
void RE_SetColor( const float *rgba );
void RE_BeginFrame( stereoFrame_t stereoFrame );
void RE_EndFrame( int *frontEndMsec, int *backEndMsec );
void RE_GetScreenShot( byte *buffer, int w, int h );
void R_ScreenShotPNG_f( void );
void VK_DestroyReadbackImage( void );
