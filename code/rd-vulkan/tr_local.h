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
// swapchain setup and a 2D textured-quad draw path (RE_StretchPic, used for
// the whole UI/menu layer and for font glyphs) are real; 3D world/model
// rendering (RenderScene and everything it touches) is not implemented yet
// and is deliberately a safe no-op for now.

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
