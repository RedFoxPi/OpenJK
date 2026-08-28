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

#include "../server/exe_headers.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include "tr_local.h"
#include <vector>
#include <cstring>
#include <cstdio>

refimport_t ri;
vkGlobals_t vk;

// Referenced (extern) by the reused rd-common/tr_font.cpp.
cvar_t *r_verbose = nullptr;
cvar_t *se_language = nullptr;
cvar_t *com_buildScript = nullptr;
cvar_t *r_lodbias = nullptr;
cvar_t *r_ghoul2AnimDebug = nullptr;

extern void R_InitFonts( void );
extern void R_ShutdownFonts( void );
extern void R_ImageLoader_Init();

// Small common-function wrappers the reused rd-common/ files (and any future
// reused rd-vanilla code) call directly rather than through ri.* - copied
// from rd-vanilla/tr_subs.cpp, which is otherwise not reusable as a whole
// file for the same quote-include reason as G2_*.cpp (see CMakeLists.txt).
void QDECL Com_Printf( const char *msg, ... )
{
	va_list argptr;
	char text[1024];
	va_start( argptr, msg );
	Q_vsnprintf( text, sizeof( text ), msg, argptr );
	va_end( argptr );
	ri.Printf( PRINT_ALL, "%s", text );
}

void QDECL Com_Error( int level, const char *error, ... )
{
	va_list argptr;
	char text[1024];
	va_start( argptr, error );
	Q_vsnprintf( text, sizeof( text ), error, argptr );
	va_end( argptr );
	ri.Error( level, "%s", text );
}

void Com_DPrintf( const char *format, ... )
{
	va_list argptr;
	char text[1024];
	va_start( argptr, format );
	Q_vsnprintf( text, sizeof( text ), format, argptr );
	va_end( argptr );
	ri.Printf( PRINT_DEVELOPER, "%s", text );
}

void *R_Malloc( int iSize, memtag_t eTag, qboolean bZeroit ) { return ri.Malloc( iSize, eTag, bZeroit, 4 ); }
void R_Free( void *ptr ) { ri.Z_Free( ptr ); }
int R_MemSize( memtag_t eTag ) { return ri.Z_MemSize( eTag ); }
void R_MorphMallocTag( void *pvBuffer, memtag_t eDesiredTag ) { ri.Z_MorphMallocTag( pvBuffer, eDesiredTag ); }
void *R_Hunk_Alloc( int iSize, qboolean bZeroit ) { return ri.Malloc( iSize, TAG_HUNKALLOC, bZeroit, 4 ); }

// ============================================================================
// Small helpers
// ============================================================================

void VK_Check( VkResult r, const char *what )
{
	if ( r != VK_SUCCESS )
	{
		ri.Error( ERR_FATAL, "rd-vulkan: %s failed (VkResult %d)\n", what, (int)r );
	}
}

static uint32_t VK_FindMemoryType( uint32_t typeFilter, VkMemoryPropertyFlags properties )
{
	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties( vk.physicalDevice, &memProps );

	for ( uint32_t i = 0; i < memProps.memoryTypeCount; i++ )
	{
		if ( (typeFilter & (1u << i)) &&
			(memProps.memoryTypes[i].propertyFlags & properties) == properties )
		{
			return i;
		}
	}
	ri.Error( ERR_FATAL, "rd-vulkan: no suitable memory type\n" );
	return 0;
}

void VK_CreateBuffer( VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
	VkBuffer *buffer, VkDeviceMemory *memory )
{
	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_Check( vkCreateBuffer( vk.device, &bufferInfo, nullptr, buffer ), "vkCreateBuffer" );

	VkMemoryRequirements memReq;
	vkGetBufferMemoryRequirements( vk.device, *buffer, &memReq );

	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = VK_FindMemoryType( memReq.memoryTypeBits, properties );
	VK_Check( vkAllocateMemory( vk.device, &allocInfo, nullptr, memory ), "vkAllocateMemory (buffer)" );

	vkBindBufferMemory( vk.device, *buffer, *memory, 0 );
}

VkCommandBuffer VK_BeginOneShotCommands( void )
{
	VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandPool = vk.frames[0].commandPool;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer cmd;
	vkAllocateCommandBuffers( vk.device, &allocInfo, &cmd );

	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer( cmd, &beginInfo );
	return cmd;
}

void VK_EndOneShotCommands( VkCommandBuffer cmd )
{
	vkEndCommandBuffer( cmd );

	VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;
	vkQueueSubmit( vk.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE );
	vkQueueWaitIdle( vk.graphicsQueue );

	vkFreeCommandBuffers( vk.device, vk.frames[0].commandPool, 1, &cmd );
}

VkShaderModule VK_CreateShaderModule( const uint32_t *code, size_t codeSize )
{
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	ci.codeSize = codeSize;
	ci.pCode = code;
	VkShaderModule module;
	VK_Check( vkCreateShaderModule( vk.device, &ci, nullptr, &module ), "vkCreateShaderModule" );
	return module;
}

// ============================================================================
// Instance / device / swapchain bring-up
// ============================================================================

static VKAPI_ATTR VkBool32 VKAPI_CALL VK_DebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT type,
	const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
	void *userData )
{
	(void)type; (void)userData;
	if ( severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan (validation): %s\n", pCallbackData->pMessage );
	}
	return VK_FALSE;
}

static void VK_CreateInstance( void )
{
	unsigned int extCount = 0;
	SDL_Vulkan_GetInstanceExtensions( vk.window, &extCount, nullptr );
	std::vector<const char *> extensions( extCount );
	SDL_Vulkan_GetInstanceExtensions( vk.window, &extCount, extensions.data() );

	bool wantValidation = false;
	uint32_t layerCount = 0;
	vkEnumerateInstanceLayerProperties( &layerCount, nullptr );
	std::vector<VkLayerProperties> layers( layerCount );
	vkEnumerateInstanceLayerProperties( &layerCount, layers.data() );
	for ( const auto &l : layers )
	{
		if ( !strcmp( l.layerName, "VK_LAYER_KHRONOS_validation" ) )
		{
			wantValidation = true;
		}
	}

	if ( wantValidation )
	{
		extensions.push_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
	}

#ifdef __APPLE__
	extensions.push_back( VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME );
#endif

	VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	appInfo.pApplicationName = "OpenJK";
	appInfo.applicationVersion = VK_MAKE_VERSION( 1, 0, 0 );
	appInfo.pEngineName = "rd-vulkan";
	appInfo.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ci.pApplicationInfo = &appInfo;
	ci.enabledExtensionCount = (uint32_t)extensions.size();
	ci.ppEnabledExtensionNames = extensions.data();

	const char *validationLayer = "VK_LAYER_KHRONOS_validation";
	if ( wantValidation )
	{
		ci.enabledLayerCount = 1;
		ci.ppEnabledLayerNames = &validationLayer;
	}

#ifdef __APPLE__
	ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

	VK_Check( vkCreateInstance( &ci, nullptr, &vk.instance ), "vkCreateInstance" );

	if ( wantValidation )
	{
		auto createDebugUtils = (PFN_vkCreateDebugUtilsMessengerEXT)
			vkGetInstanceProcAddr( vk.instance, "vkCreateDebugUtilsMessengerEXT" );
		if ( createDebugUtils )
		{
			VkDebugUtilsMessengerCreateInfoEXT dbgInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
			dbgInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			dbgInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			dbgInfo.pfnUserCallback = VK_DebugCallback;
			createDebugUtils( vk.instance, &dbgInfo, nullptr, &vk.debugMessenger );
		}
	}
}

static void VK_PickPhysicalDevice( void )
{
	uint32_t count = 0;
	vkEnumeratePhysicalDevices( vk.instance, &count, nullptr );
	if ( count == 0 )
	{
		ri.Error( ERR_FATAL, "rd-vulkan: no Vulkan-capable devices found\n" );
	}
	std::vector<VkPhysicalDevice> devices( count );
	vkEnumeratePhysicalDevices( vk.instance, &count, devices.data() );

	// Prefer a discrete GPU, but happily fall back to whatever's available
	// (integrated, or a software rasterizer like lavapipe for headless testing).
	VkPhysicalDevice fallback = devices[0];
	for ( auto dev : devices )
	{
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties( dev, &props );
		if ( props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU )
		{
			vk.physicalDevice = dev;
			vk.physicalDeviceProps = props;
			return;
		}
	}
	vk.physicalDevice = fallback;
	vkGetPhysicalDeviceProperties( fallback, &vk.physicalDeviceProps );
}

static uint32_t VK_FindGraphicsPresentQueueFamily( void )
{
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties( vk.physicalDevice, &count, nullptr );
	std::vector<VkQueueFamilyProperties> families( count );
	vkGetPhysicalDeviceQueueFamilyProperties( vk.physicalDevice, &count, families.data() );

	for ( uint32_t i = 0; i < count; i++ )
	{
		if ( !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) )
			continue;
		VkBool32 presentSupport = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR( vk.physicalDevice, i, vk.surface, &presentSupport );
		if ( presentSupport )
			return i;
	}
	ri.Error( ERR_FATAL, "rd-vulkan: no queue family supports both graphics and present\n" );
	return 0;
}

static void VK_CreateLogicalDevice( void )
{
	vk.graphicsQueueFamily = VK_FindGraphicsPresentQueueFamily();

	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queueInfo.queueFamilyIndex = vk.graphicsQueueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;

	std::vector<const char *> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

#ifdef __APPLE__
	// MoltenVK requires this to be enabled explicitly on the device as well.
	deviceExtensions.push_back( "VK_KHR_portability_subset" );
#endif

	VkPhysicalDeviceFeatures features = {};

	VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	ci.queueCreateInfoCount = 1;
	ci.pQueueCreateInfos = &queueInfo;
	ci.pEnabledFeatures = &features;
	ci.enabledExtensionCount = (uint32_t)deviceExtensions.size();
	ci.ppEnabledExtensionNames = deviceExtensions.data();

	VK_Check( vkCreateDevice( vk.physicalDevice, &ci, nullptr, &vk.device ), "vkCreateDevice" );
	vkGetDeviceQueue( vk.device, vk.graphicsQueueFamily, 0, &vk.graphicsQueue );
}

static void VK_CreateSwapchain( void )
{
	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR( vk.physicalDevice, vk.surface, &caps );

	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR( vk.physicalDevice, vk.surface, &formatCount, nullptr );
	std::vector<VkSurfaceFormatKHR> formats( formatCount );
	vkGetPhysicalDeviceSurfaceFormatsKHR( vk.physicalDevice, vk.surface, &formatCount, formats.data() );

	VkSurfaceFormatKHR chosen = formats[0];
	for ( const auto &f : formats )
	{
		if ( f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR )
		{
			chosen = f;
			break;
		}
	}
	vk.swapchainFormat = chosen.format;

	vk.swapchainExtent = caps.currentExtent;
	if ( vk.swapchainExtent.width == UINT32_MAX )
	{
		int w, h;
		SDL_Vulkan_GetDrawableSize( vk.window, &w, &h );
		vk.swapchainExtent.width = (uint32_t)w;
		vk.swapchainExtent.height = (uint32_t)h;
	}

	uint32_t imageCount = caps.minImageCount + 1;
	if ( caps.maxImageCount > 0 && imageCount > caps.maxImageCount )
		imageCount = caps.maxImageCount;

	VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	ci.surface = vk.surface;
	ci.minImageCount = imageCount;
	ci.imageFormat = chosen.format;
	ci.imageColorSpace = chosen.colorSpace;
	ci.imageExtent = vk.swapchainExtent;
	ci.imageArrayLayers = 1;
	ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform = caps.currentTransform;
	ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always supported, good enough for now
	ci.clipped = VK_TRUE;

	VK_Check( vkCreateSwapchainKHR( vk.device, &ci, nullptr, &vk.swapchain ), "vkCreateSwapchainKHR" );

	uint32_t count = 0;
	vkGetSwapchainImagesKHR( vk.device, vk.swapchain, &count, nullptr );
	vk.swapchainImages.resize( count );
	vkGetSwapchainImagesKHR( vk.device, vk.swapchain, &count, vk.swapchainImages.data() );

	vk.swapchainImageViews.resize( count );
	for ( uint32_t i = 0; i < count; i++ )
	{
		VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vci.image = vk.swapchainImages[i];
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = vk.swapchainFormat;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_Check( vkCreateImageView( vk.device, &vci, nullptr, &vk.swapchainImageViews[i] ), "vkCreateImageView (swapchain)" );
	}
}

static VkFormat VK_FindDepthFormat( void )
{
	VkFormat candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT };
	for ( VkFormat format : candidates )
	{
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties( vk.physicalDevice, format, &props );
		if ( props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT )
		{
			return format;
		}
	}
	ri.Error( ERR_FATAL, "rd-vulkan: no supported depth format\n" );
	return VK_FORMAT_UNDEFINED;
}

// One persistent depth image sized to the swapchain, reused every frame -
// see the comment on vkGlobals_t::depthImage in tr_local.h for why that's
// safe with this renderer's current (fully-serialized) frame pacing.
static void VK_CreateDepthResources( void )
{
	vk.depthFormat = VK_FindDepthFormat();

	VkImageCreateInfo imgInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgInfo.imageType = VK_IMAGE_TYPE_2D;
	imgInfo.extent = { vk.swapchainExtent.width, vk.swapchainExtent.height, 1 };
	imgInfo.mipLevels = 1;
	imgInfo.arrayLayers = 1;
	imgInfo.format = vk.depthFormat;
	imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imgInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_Check( vkCreateImage( vk.device, &imgInfo, nullptr, &vk.depthImage ), "vkCreateImage (depth)" );

	VkMemoryRequirements memReq;
	vkGetImageMemoryRequirements( vk.device, vk.depthImage, &memReq );

	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties( vk.physicalDevice, &memProps );
	uint32_t memType = 0;
	for ( uint32_t i = 0; i < memProps.memoryTypeCount; i++ )
	{
		if ( (memReq.memoryTypeBits & (1u << i)) &&
			(memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) )
		{
			memType = i;
			break;
		}
	}

	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = memType;
	VK_Check( vkAllocateMemory( vk.device, &allocInfo, nullptr, &vk.depthImageMemory ), "vkAllocateMemory (depth)" );
	vkBindImageMemory( vk.device, vk.depthImage, vk.depthImageMemory, 0 );

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = vk.depthImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = vk.depthFormat;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
	VK_Check( vkCreateImageView( vk.device, &viewInfo, nullptr, &vk.depthImageView ), "vkCreateImageView (depth)" );
}

static void VK_CreateRenderPass( void )
{
	VkAttachmentDescription colorAttachment = {};
	colorAttachment.format = vk.swapchainFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	VkAttachmentDescription depthAttachment = {};
	depthAttachment.format = vk.depthFormat;
	depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;
	subpass.pDepthStencilAttachment = &depthRef;

	// Two dependencies: one governing entry into the subpass (a fresh
	// swapchain image may still be in use by a previous present), and -
	// critically - one governing exit, since RE_EndFrame's readback copy
	// (VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL read) and the present transition
	// both happen right after vkCmdEndRenderPass in the same command buffer;
	// without an explicit EXTERNAL dependency here, nothing guarantees the
	// color attachment writes are visible to that read (Vulkan does not
	// order-of-submission-implies-visibility the way command order suggests).
	// Both dependencies also cover the depth attachment's read/write stages
	// so the depth image's load-clear-at-entry and write-during-subpass are
	// correctly synchronized the same way the color attachment is.
	VkSubpassDependency dependencies[2] = {};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependencies[0].srcAccessMask = 0;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	VkAttachmentDescription attachments[] = { colorAttachment, depthAttachment };

	VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	ci.attachmentCount = 2;
	ci.pAttachments = attachments;
	ci.subpassCount = 1;
	ci.pSubpasses = &subpass;
	ci.dependencyCount = 2;
	ci.pDependencies = dependencies;

	VK_Check( vkCreateRenderPass( vk.device, &ci, nullptr, &vk.renderPass ), "vkCreateRenderPass" );
}

static void VK_CreateFramebuffers( void )
{
	vk.swapchainFramebuffers.resize( vk.swapchainImageViews.size() );
	for ( size_t i = 0; i < vk.swapchainImageViews.size(); i++ )
	{
		VkImageView attachments[] = { vk.swapchainImageViews[i], vk.depthImageView };
		VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		ci.renderPass = vk.renderPass;
		ci.attachmentCount = 2;
		ci.pAttachments = attachments;
		ci.width = vk.swapchainExtent.width;
		ci.height = vk.swapchainExtent.height;
		ci.layers = 1;
		VK_Check( vkCreateFramebuffer( vk.device, &ci, nullptr, &vk.swapchainFramebuffers[i] ), "vkCreateFramebuffer" );
	}
}

// Tears down every swapchain-*sized* resource (not the render pass, which
// only depends on format - stable across a resize, not extent - and not
// the device/instance/surface/pipelines, none of which need to change
// either: every pipeline already uses VK_DYNAMIC_STATE_VIEWPORT/SCISSOR,
// set fresh per frame from vk.swapchainExtent, precisely so a resize never
// needs to rebuild any of them). See VK_RecreateSwapchain's own comment
// for why this exists as a separate step from VK_Shutdown's full teardown.
static void VK_DestroySwapchainResources( void )
{
	for ( auto fb : vk.swapchainFramebuffers ) vkDestroyFramebuffer( vk.device, fb, nullptr );
	vk.swapchainFramebuffers.clear();
	if ( vk.depthImageView ) { vkDestroyImageView( vk.device, vk.depthImageView, nullptr ); vk.depthImageView = VK_NULL_HANDLE; }
	if ( vk.depthImage ) { vkDestroyImage( vk.device, vk.depthImage, nullptr ); vk.depthImage = VK_NULL_HANDLE; }
	if ( vk.depthImageMemory ) { vkFreeMemory( vk.device, vk.depthImageMemory, nullptr ); vk.depthImageMemory = VK_NULL_HANDLE; }
	for ( auto view : vk.swapchainImageViews ) vkDestroyImageView( vk.device, view, nullptr );
	vk.swapchainImageViews.clear();
	if ( vk.swapchain ) { vkDestroySwapchainKHR( vk.device, vk.swapchain, nullptr ); vk.swapchain = VK_NULL_HANDLE; }
}

// Real swapchain recreation - a genuine gap this renderer had since its
// first pass (see RE_BeginFrame's own comment, tr_cmds.cpp, for the exact
// symptom this fixes): before this, any real window resize made
// vkAcquireNextImageKHR return VK_ERROR_OUT_OF_DATE_KHR every single frame
// from then on (the swapchain was still sized for the *old* window), and
// RE_BeginFrame's response was just to log a warning and skip the frame -
// forever, since nothing ever rebuilt the swapchain, permanently freezing
// rendering until a full engine restart. Called two ways: proactively from
// RE_BeginFrame when the window's actual drawable size no longer matches
// vk.swapchainExtent (the common real-world case, catches a resize before
// even trying to acquire), and reactively if vkAcquireNextImageKHR itself
// still returns VK_ERROR_OUT_OF_DATE_KHR despite that check (a surface
// capability change not reflected in drawable size alone, or a race
// between the check and the acquire call).
//
// Deliberately NOT a port of rd-vanilla's own resize handling - that
// renderer's GL context has no equivalent "swapchain" concept at all (GL
// just renders into whatever size the window currently is, no explicit
// recreation step exists to port) - this is a from-scratch implementation
// of what Vulkan's own swapchain model requires, same "reuse strategy"
// this renderer applies throughout (see README.md).
void VK_RecreateSwapchain( void )
{
	int w = 0, h = 0;
	SDL_Vulkan_GetDrawableSize( vk.window, &w, &h );
	if ( w <= 0 || h <= 0 )
	{
		// Minimized (or otherwise zero-area) - nothing to recreate yet;
		// the caller's own per-frame size check will retry once the
		// window has a real size again.
		return;
	}

	vkDeviceWaitIdle( vk.device );

	VK_DestroySwapchainResources();
	// Forces a fresh readback image at the new size on the very next
	// screenshot/GetScreenShot - it's a lazily-created singleton sized to
	// whatever vk.swapchainExtent was at its own creation time (see
	// VK_CreateReadbackImage, tr_cmds.cpp) and, unlike everything else
	// here, was never being invalidated on a size change at all before
	// this fix - a real second bug alongside the frozen-swapchain one,
	// just silent instead of loud: a stale-sized readback image would
	// still successfully copy (Vulkan doesn't require matching src/dst
	// extents for vkCmdCopyImage's *region*, only that the region fits
	// both), quietly producing a screenshot cropped/misaligned to the old
	// window size instead of an error.
	VK_DestroyReadbackImage();

	VK_CreateSwapchain();
	VK_CreateDepthResources();
	VK_CreateFramebuffers();

	vk.glConfig.vidWidth = vk.swapchainExtent.width;
	vk.glConfig.vidHeight = vk.swapchainExtent.height;
}

static void VK_CreateCommandPoolsAndSync( void )
{
	VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = vk.graphicsQueueFamily;

	VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		VK_Check( vkCreateCommandPool( vk.device, &poolInfo, nullptr, &vk.frames[i].commandPool ), "vkCreateCommandPool" );

		VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocInfo.commandPool = vk.frames[i].commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		VK_Check( vkAllocateCommandBuffers( vk.device, &allocInfo, &vk.frames[i].commandBuffer ), "vkAllocateCommandBuffers" );

		VK_Check( vkCreateSemaphore( vk.device, &semInfo, nullptr, &vk.frames[i].imageAvailable ), "vkCreateSemaphore" );
		VK_Check( vkCreateSemaphore( vk.device, &semInfo, nullptr, &vk.frames[i].renderFinished ), "vkCreateSemaphore" );
		VK_Check( vkCreateFence( vk.device, &fenceInfo, nullptr, &vk.frames[i].inFlight ), "vkCreateFence" );
	}
}

// Embedded SPIR-V (generated at build time from shaders/ui.vert, shaders/ui.frag)
#include "ui_vert_spv.h"
#include "ui_frag_spv.h"

struct UiVertex
{
	float pos[2];
	float uv[2];
};

static void VK_CreateUiPipeline( void )
{
	VkDescriptorSetLayoutBinding samplerBinding = {};
	samplerBinding.binding = 0;
	samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	samplerBinding.descriptorCount = 1;
	samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo dslInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dslInfo.bindingCount = 1;
	dslInfo.pBindings = &samplerBinding;
	VK_Check( vkCreateDescriptorSetLayout( vk.device, &dslInfo, nullptr, &vk.uiDescriptorSetLayout ), "vkCreateDescriptorSetLayout" );

	VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_VK_IMAGES };
	VkDescriptorPoolCreateInfo dpInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpInfo.poolSizeCount = 1;
	dpInfo.pPoolSizes = &poolSize;
	dpInfo.maxSets = MAX_VK_IMAGES;
	VK_Check( vkCreateDescriptorPool( vk.device, &dpInfo, nullptr, &vk.uiDescriptorPool ), "vkCreateDescriptorPool" );

	VkPushConstantRange pushRange = {};
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof( vkPushConstants_t );

	VkPipelineLayoutCreateInfo plInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plInfo.setLayoutCount = 1;
	plInfo.pSetLayouts = &vk.uiDescriptorSetLayout;
	plInfo.pushConstantRangeCount = 1;
	plInfo.pPushConstantRanges = &pushRange;
	VK_Check( vkCreatePipelineLayout( vk.device, &plInfo, nullptr, &vk.uiPipelineLayout ), "vkCreatePipelineLayout" );

	VkShaderModule vertModule = VK_CreateShaderModule( ui_vert_spv, sizeof( ui_vert_spv ) );
	VkShaderModule fragModule = VK_CreateShaderModule( ui_frag_spv, sizeof( ui_frag_spv ) );

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertModule;
	stages[0].pName = "main";
	stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragModule;
	stages[1].pName = "main";

	VkVertexInputBindingDescription binding = { 0, sizeof( UiVertex ), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attrs[2] = {
		{ 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof( UiVertex, pos ) },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof( UiVertex, uv ) },
	};

	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &binding;
	vertexInput.vertexAttributeDescriptionCount = 2;
	vertexInput.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// Three blend-mode variants (BLEND_ALPHA / BLEND_ADDITIVE / BLEND_OPAQUE
	// - see vkBlendMode_t in tr_local.h). Vulkan bakes blend factors into the
	// pipeline, unlike GL's dynamic glBlendFunc, so a distinct .shader
	// blendFunc means a distinct VkPipeline, selected per draw in
	// RE_StretchPic (tr_cmds.cpp) based on the image's parsed blend mode.
	VkPipelineColorBlendAttachmentState blendAttachment = {};
	blendAttachment.blendEnable = VK_TRUE;
	blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
		| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendAttachmentState blendAttachmentAdditive = blendAttachment;
	blendAttachmentAdditive.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachmentAdditive.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachmentAdditive.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachmentAdditive.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;

	VkPipelineColorBlendAttachmentState blendAttachmentOpaque = blendAttachment;
	blendAttachmentOpaque.blendEnable = VK_FALSE;

	VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments = &blendAttachment;

	VkPipelineColorBlendStateCreateInfo colorBlendAdditive = colorBlend;
	colorBlendAdditive.pAttachments = &blendAttachmentAdditive;

	VkPipelineColorBlendStateCreateInfo colorBlendOpaque = colorBlend;
	colorBlendOpaque.pAttachments = &blendAttachmentOpaque;

	VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynState.dynamicStateCount = 2;
	dynState.pDynamicStates = dynStates;

	VkGraphicsPipelineCreateInfo pipeInfos[3] = {};
	pipeInfos[0] = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipeInfos[0].stageCount = 2;
	pipeInfos[0].pStages = stages;
	pipeInfos[0].pVertexInputState = &vertexInput;
	pipeInfos[0].pInputAssemblyState = &inputAssembly;
	pipeInfos[0].pViewportState = &viewportState;
	pipeInfos[0].pRasterizationState = &raster;
	pipeInfos[0].pMultisampleState = &multisample;
	pipeInfos[0].pColorBlendState = &colorBlend;
	pipeInfos[0].pDynamicState = &dynState;
	pipeInfos[0].layout = vk.uiPipelineLayout;
	pipeInfos[0].renderPass = vk.renderPass;
	pipeInfos[0].subpass = 0;

	pipeInfos[1] = pipeInfos[0];
	pipeInfos[1].pColorBlendState = &colorBlendAdditive;

	pipeInfos[2] = pipeInfos[0];
	pipeInfos[2].pColorBlendState = &colorBlendOpaque;

	VkPipeline pipelines[3] = {};
	VK_Check( vkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 3, pipeInfos, nullptr, pipelines ), "vkCreateGraphicsPipelines" );
	vk.uiPipeline = pipelines[0];
	vk.uiPipelineOpaque = pipelines[2];
	vk.uiPipelineAdditive = pipelines[1];

	vkDestroyShaderModule( vk.device, vertModule, nullptr );
	vkDestroyShaderModule( vk.device, fragModule, nullptr );

	VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
	VK_Check( vkCreateSampler( vk.device, &samplerInfo, nullptr, &vk.uiSampler ), "vkCreateSampler" );

	VK_CreateBuffer( sizeof( UiVertex ) * UI_VERTEX_BUFFER_CAPACITY * 6,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&vk.uiVertexBuffer, &vk.uiVertexBufferMemory );
	vkMapMemory( vk.device, vk.uiVertexBufferMemory, 0, VK_WHOLE_SIZE, 0, &vk.uiVertexBufferMapped );
}

// Embedded SPIR-V for the 3D world pipeline (tr_world.cpp - static, opaque,
// unlit BSP geometry only, see README.md).
#include "world_vert_spv.h"
#include "world_frag_spv.h"

static void VK_CreateWorldPipeline( void )
{
	// World draws need two bound textures per surface (diffuse + lightmap -
	// see tr_world.cpp's VK_LoadLightmaps), unlike the UI path's one, so
	// this gets its own descriptor set layout/pool rather than reusing
	// vk.uiDescriptorSetLayout. One set is built per surface batch at map
	// load (VK_BuildWorldDescriptorSet in tr_world.cpp), not per texture.
	VkDescriptorSetLayoutBinding bindings[2] = {};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo dslInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dslInfo.bindingCount = 2;
	dslInfo.pBindings = bindings;
	VK_Check( vkCreateDescriptorSetLayout( vk.device, &dslInfo, nullptr, &vk.worldDescriptorSetLayout ), "vkCreateDescriptorSetLayout (world)" );

	// maxSets is MAX_VK_WORLD_DESCRIPTOR_SETS, NOT MAX_VK_IMAGES - see that
	// constant's own comment (tr_local.h) for why reusing the UI pool's
	// "number of unique images" cap here silently broke large levels
	// (one set per surface *batch*, not per unique image - a real map can
	// have far more batches than distinct textures).
	VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_VK_WORLD_DESCRIPTOR_SETS * 2 };
	VkDescriptorPoolCreateInfo dpInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpInfo.poolSizeCount = 1;
	dpInfo.pPoolSizes = &poolSize;
	dpInfo.maxSets = MAX_VK_WORLD_DESCRIPTOR_SETS;
	VK_Check( vkCreateDescriptorPool( vk.device, &dpInfo, nullptr, &vk.worldDescriptorPool ), "vkCreateDescriptorPool (world)" );

	// Ghoul2 models' own pool - see vkGlobals_t::ghoul2DescriptorPool's
	// comment for why it can't share vk.worldDescriptorPool. Same layout/
	// binding shape, so no separate VkDescriptorPoolSize/layout needed.
	VK_Check( vkCreateDescriptorPool( vk.device, &dpInfo, nullptr, &vk.ghoul2DescriptorPool ), "vkCreateDescriptorPool (ghoul2)" );

	VkSamplerCreateInfo worldSamplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	worldSamplerInfo.magFilter = VK_FILTER_LINEAR;
	worldSamplerInfo.minFilter = VK_FILTER_LINEAR;
	worldSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	worldSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	worldSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	worldSamplerInfo.maxLod = VK_LOD_CLAMP_NONE;
	VK_Check( vkCreateSampler( vk.device, &worldSamplerInfo, nullptr, &vk.worldSampler ), "vkCreateSampler (world)" );

	VkPushConstantRange pushRange = {};
	// Fragment stage needs camPos/fogColor too (world.frag does the actual
	// fog mix), not just mvp - vertex and fragment share this one range
	// rather than needing two, since both stages' SPIR-V just read the
	// fields they care about out of the same layout.
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof( vkWorldPushConstants_t );

	VkPipelineLayoutCreateInfo plInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plInfo.setLayoutCount = 1;
	plInfo.pSetLayouts = &vk.worldDescriptorSetLayout;
	plInfo.pushConstantRangeCount = 1;
	plInfo.pPushConstantRanges = &pushRange;
	VK_Check( vkCreatePipelineLayout( vk.device, &plInfo, nullptr, &vk.worldPipelineLayout ), "vkCreatePipelineLayout (world)" );

	VkShaderModule vertModule = VK_CreateShaderModule( world_vert_spv, sizeof( world_vert_spv ) );
	VkShaderModule fragModule = VK_CreateShaderModule( world_frag_spv, sizeof( world_frag_spv ) );

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertModule;
	stages[0].pName = "main";
	stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragModule;
	stages[1].pName = "main";

	VkVertexInputBindingDescription binding = { 0, sizeof( WorldVertex ), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attrs[5] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof( WorldVertex, pos ) },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof( WorldVertex, uv ) },
		{ 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof( WorldVertex, lightmapUV ) },
		{ 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof( WorldVertex, color ) },
		{ 4, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof( WorldVertex, normal ) },
	};

	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &binding;
	vertexInput.vertexAttributeDescriptionCount = 5;
	vertexInput.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	// No culling: this first pass doesn't know/trust winding order across
	// every BSP surface type (planar polygons vs triangle soups), and a
	// wrong cull direction silently drops geometry rather than erroring -
	// worse than the minor overdraw cost of drawing both faces. Revisit once
	// winding is verified surface-type by surface-type.
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

	// Sky variant: test/write both off (see vk.skyPipeline's comment in
	// tr_local.h) - otherwise identical, same layout/shaders/vertex format.
	VkPipelineDepthStencilStateCreateInfo skyDepthStencil = depthStencil;
	skyDepthStencil.depthTestEnable = VK_FALSE;
	skyDepthStencil.depthWriteEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState blendAttachment = {};
	blendAttachment.blendEnable = VK_FALSE;
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
		| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	// Alpha/additive variants for WorldSurfaceBatch::blendMode - same
	// factors as vk.polyPipeline/polyPipelineAdditive and
	// vk.uiPipeline/uiPipelineAdditive, kept consistent across every blend
	// pipeline this renderer has rather than re-deriving them.
	VkPipelineColorBlendAttachmentState blendAttachmentAlpha = blendAttachment;
	blendAttachmentAlpha.blendEnable = VK_TRUE;
	blendAttachmentAlpha.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachmentAlpha.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachmentAlpha.colorBlendOp = VK_BLEND_OP_ADD;
	blendAttachmentAlpha.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachmentAlpha.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	blendAttachmentAlpha.alphaBlendOp = VK_BLEND_OP_ADD;

	VkPipelineColorBlendAttachmentState blendAttachmentAdditive = blendAttachmentAlpha;
	blendAttachmentAdditive.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachmentAdditive.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachmentAdditive.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAttachmentAdditive.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;

	VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments = &blendAttachment;

	VkPipelineColorBlendStateCreateInfo colorBlendAlpha = colorBlend;
	colorBlendAlpha.pAttachments = &blendAttachmentAlpha;

	VkPipelineColorBlendStateCreateInfo colorBlendAdditive = colorBlend;
	colorBlendAdditive.pAttachments = &blendAttachmentAdditive;

	// Translucent world geometry depth-tests against opaque geometry (so a
	// wall in front of it still occludes it) but doesn't write depth itself
	// - see vkGlobals_t::worldPipelineAlpha's own comment for why that's an
	// accepted simplification rather than real per-shader depth-write
	// control or a full translucency sort.
	VkPipelineDepthStencilStateCreateInfo blendDepthStencil = depthStencil;
	blendDepthStencil.depthWriteEnable = VK_FALSE;

	VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynState.dynamicStateCount = 2;
	dynState.pDynamicStates = dynStates;

	VkGraphicsPipelineCreateInfo pipeInfos[4] = {};
	pipeInfos[0] = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipeInfos[0].stageCount = 2;
	pipeInfos[0].pStages = stages;
	pipeInfos[0].pVertexInputState = &vertexInput;
	pipeInfos[0].pInputAssemblyState = &inputAssembly;
	pipeInfos[0].pViewportState = &viewportState;
	pipeInfos[0].pRasterizationState = &raster;
	pipeInfos[0].pMultisampleState = &multisample;
	pipeInfos[0].pDepthStencilState = &depthStencil;
	pipeInfos[0].pColorBlendState = &colorBlend;
	pipeInfos[0].pDynamicState = &dynState;
	pipeInfos[0].layout = vk.worldPipelineLayout;
	pipeInfos[0].renderPass = vk.renderPass;
	pipeInfos[0].subpass = 0;

	pipeInfos[1] = pipeInfos[0];
	pipeInfos[1].pDepthStencilState = &skyDepthStencil;

	pipeInfos[2] = pipeInfos[0];
	pipeInfos[2].pDepthStencilState = &blendDepthStencil;
	pipeInfos[2].pColorBlendState = &colorBlendAlpha;

	pipeInfos[3] = pipeInfos[0];
	pipeInfos[3].pDepthStencilState = &blendDepthStencil;
	pipeInfos[3].pColorBlendState = &colorBlendAdditive;

	VkPipeline pipelines[4] = {};
	VK_Check( vkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 4, pipeInfos, nullptr, pipelines ), "vkCreateGraphicsPipelines (world)" );
	vk.worldPipeline = pipelines[0];
	vk.skyPipeline = pipelines[1];
	vk.worldPipelineAlpha = pipelines[2];
	vk.worldPipelineAdditive = pipelines[3];

	vkDestroyShaderModule( vk.device, vertModule, nullptr );
	vkDestroyShaderModule( vk.device, fragModule, nullptr );
}

// Embedded SPIR-V for runtime polys (RE_AddPolyToScene) - see
// shaders/poly.vert/poly.frag and VulkanGlobals_t::polyPipeline's comment.
#include "poly_vert_spv.h"
#include "poly_frag_spv.h"

static void VK_CreatePolyPipeline( void )
{
	// Reuses vk.uiDescriptorSetLayout/vk.uiSampler (one plain texture, no
	// lightmap) rather than allocating a third descriptor set layout/pool -
	// see vkGlobals_t::polyPipelineLayout's comment. Must run after
	// VK_CreateUiPipeline, which is where that layout is actually created.
	VkPushConstantRange pushRange = {};
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof( float ) * 16; // mat4 mvp only - see poly.vert

	VkPipelineLayoutCreateInfo plInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plInfo.setLayoutCount = 1;
	plInfo.pSetLayouts = &vk.uiDescriptorSetLayout;
	plInfo.pushConstantRangeCount = 1;
	plInfo.pPushConstantRanges = &pushRange;
	VK_Check( vkCreatePipelineLayout( vk.device, &plInfo, nullptr, &vk.polyPipelineLayout ), "vkCreatePipelineLayout (poly)" );

	VkShaderModule vertModule = VK_CreateShaderModule( poly_vert_spv, sizeof( poly_vert_spv ) );
	VkShaderModule fragModule = VK_CreateShaderModule( poly_frag_spv, sizeof( poly_frag_spv ) );

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertModule;
	stages[0].pName = "main";
	stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragModule;
	stages[1].pName = "main";

	VkVertexInputBindingDescription binding = { 0, sizeof( PolyVertex ), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attrs[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof( PolyVertex, pos ) },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof( PolyVertex, uv ) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof( PolyVertex, color ) },
	};

	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = 1;
	vertexInput.pVertexBindingDescriptions = &binding;
	vertexInput.vertexAttributeDescriptionCount = 3;
	vertexInput.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	// No culling - particle/effect polys are usually meant to be visible
	// from both sides (billboards, fans with no guaranteed winding).
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// Test against world/Ghoul2 depth so a poly behind a wall is correctly
	// hidden; no write, for all three blend variants uniformly - see
	// vkGlobals_t::polyPipeline's comment for why.
	VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

	// Same three blend-mode variants as VK_CreateUiPipeline (vkBlendMode_t) -
	// see that function for the real blend-factor values, copied verbatim
	// rather than rederived.
	VkPipelineColorBlendAttachmentState blendAlpha = {};
	blendAlpha.blendEnable = VK_TRUE;
	blendAlpha.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAlpha.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAlpha.colorBlendOp = VK_BLEND_OP_ADD;
	blendAlpha.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAlpha.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	blendAlpha.alphaBlendOp = VK_BLEND_OP_ADD;
	blendAlpha.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
		| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendAttachmentState blendAdditive = blendAlpha;
	blendAdditive.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAdditive.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAdditive.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blendAdditive.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;

	VkPipelineColorBlendAttachmentState blendOpaque = blendAlpha;
	blendOpaque.blendEnable = VK_FALSE;

	VkPipelineColorBlendStateCreateInfo colorBlendAlpha = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colorBlendAlpha.attachmentCount = 1;
	colorBlendAlpha.pAttachments = &blendAlpha;

	VkPipelineColorBlendStateCreateInfo colorBlendAdditive = colorBlendAlpha;
	colorBlendAdditive.pAttachments = &blendAdditive;

	VkPipelineColorBlendStateCreateInfo colorBlendOpaque = colorBlendAlpha;
	colorBlendOpaque.pAttachments = &blendOpaque;

	VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynState.dynamicStateCount = 2;
	dynState.pDynamicStates = dynStates;

	VkGraphicsPipelineCreateInfo pipeInfos[3] = {};
	pipeInfos[0] = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipeInfos[0].stageCount = 2;
	pipeInfos[0].pStages = stages;
	pipeInfos[0].pVertexInputState = &vertexInput;
	pipeInfos[0].pInputAssemblyState = &inputAssembly;
	pipeInfos[0].pViewportState = &viewportState;
	pipeInfos[0].pRasterizationState = &raster;
	pipeInfos[0].pMultisampleState = &multisample;
	pipeInfos[0].pDepthStencilState = &depthStencil;
	pipeInfos[0].pColorBlendState = &colorBlendAlpha;
	pipeInfos[0].pDynamicState = &dynState;
	pipeInfos[0].layout = vk.polyPipelineLayout;
	pipeInfos[0].renderPass = vk.renderPass;
	pipeInfos[0].subpass = 0;

	pipeInfos[1] = pipeInfos[0];
	pipeInfos[1].pColorBlendState = &colorBlendAdditive;

	pipeInfos[2] = pipeInfos[0];
	pipeInfos[2].pColorBlendState = &colorBlendOpaque;

	VkPipeline pipelines[3] = {};
	VK_Check( vkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 3, pipeInfos, nullptr, pipelines ), "vkCreateGraphicsPipelines (poly)" );
	vk.polyPipeline = pipelines[0];
	vk.polyPipelineAdditive = pipelines[1];
	vk.polyPipelineOpaque = pipelines[2];

	vkDestroyShaderModule( vk.device, vertModule, nullptr );
	vkDestroyShaderModule( vk.device, fragModule, nullptr );

	VK_CreateBuffer( sizeof( PolyVertex ) * POLY_VERTEX_BUFFER_CAPACITY,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&vk.polyVertexBuffer, &vk.polyVertexBufferMemory );
	vkMapMemory( vk.device, vk.polyVertexBufferMemory, 0, VK_WHOLE_SIZE, 0, &vk.polyVertexBufferMapped );

	// Weather particles (tr_weather.cpp) - own buffer, same PolyVertex
	// layout, reuses this same pipeline/layout - see vkGlobals_t::
	// weatherVertexBuffer's comment (tr_local.h) for why it needs its own
	// buffer despite sharing everything else with polyVertexBuffer above.
	VK_CreateBuffer( sizeof( PolyVertex ) * WEATHER_VERTEX_BUFFER_CAPACITY,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&vk.weatherVertexBuffer, &vk.weatherVertexBufferMemory );
	vkMapMemory( vk.device, vk.weatherVertexBufferMemory, 0, VK_WHOLE_SIZE, 0, &vk.weatherVertexBufferMapped );

	// Static-geometry flares (tr_world.cpp's VK_DrawWorldFlares) - own
	// buffer, same reasoning as weather's above.
	VK_CreateBuffer( sizeof( PolyVertex ) * FLARE_VERTEX_BUFFER_CAPACITY,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&vk.flareVertexBuffer, &vk.flareVertexBufferMemory );
	vkMapMemory( vk.device, vk.flareVertexBufferMemory, 0, VK_WHOLE_SIZE, 0, &vk.flareVertexBufferMapped );
}

// ============================================================================
// Public entry points
// ============================================================================

void R_Init( void )
{
	windowDesc_t windowDesc = {};
	windowDesc.api = GRAPHICS_API_VULKAN;

	vk.window = nullptr;
	window_t win = ri.WIN_Init( &windowDesc, &vk.glConfig );
	vk.window = (SDL_Window *)win.handle;

	if ( !vk.window )
	{
		ri.Error( ERR_FATAL, "rd-vulkan: WIN_Init did not return an SDL_Window handle\n" );
	}

	VK_CreateInstance();

	if ( !SDL_Vulkan_CreateSurface( vk.window, vk.instance, &vk.surface ) )
	{
		ri.Error( ERR_FATAL, "rd-vulkan: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError() );
	}

	VK_PickPhysicalDevice();
	VK_CreateLogicalDevice();
	VK_CreateSwapchain();
	VK_CreateDepthResources();
	VK_CreateRenderPass();
	VK_CreateFramebuffers();
	VK_CreateCommandPoolsAndSync();
	VK_CreateUiPipeline();
	VK_CreateWorldPipeline();
	VK_CreatePolyPipeline();

	vk.glConfig.renderer_string = vk.physicalDeviceProps.deviceName;
	vk.glConfig.vendor_string = "rd-vulkan";
	vk.glConfig.version_string = "1.1";
	vk.glConfig.extensions_string = "";
	vk.glConfig.maxTextureSize = (int)vk.physicalDeviceProps.limits.maxImageDimension2D;
	vk.glConfig.maxActiveTextures = 1;
	vk.glConfig.colorBits = 32;
	vk.glConfig.depthBits = 0;
	vk.glConfig.stencilBits = 0;
	vk.glConfig.vidWidth = vk.swapchainExtent.width;
	vk.glConfig.vidHeight = vk.swapchainExtent.height;
	vk.glConfig.isFullscreen = qfalse;

	r_verbose = ri.Cvar_Get( "r_verbose", "0", CVAR_CHEAT );
	se_language = ri.Cvar_Get( "se_language", "english", CVAR_ARCHIVE | CVAR_NORESTART );
	com_buildScript = ri.Cvar_Get( "com_buildScript", "0", 0 );
	// Same name/default/flags as rd-vanilla's real registration (tr_init.cpp)
	// - only RT_ELECTRICITY (tr_model.cpp) reads it in this renderer so far.
	r_lodbias = ri.Cvar_Get( "r_lodbias", "0", CVAR_ARCHIVE_ND );
	// Same name/flags as rd-vanilla's own registration (tr_init.cpp) - see
	// tr_local.h's comment for why this exists in both renderers with a
	// matching name and output format.
	r_ghoul2AnimDebug = ri.Cvar_Get( "r_ghoul2animdebug", "0", 0 );

	R_ImageLoader_Init();
	R_InitFonts();
	// Same one-time-at-startup call site as rd-vanilla's real R_Init
	// (tr_init.cpp) - see VK_InitWorldEffects' own declaration comment
	// (tr_local.h) for why this isn't tied to map load/unload instead.
	VK_InitWorldEffects();

	vk.whiteImage = VK_CreateSolidImage( "*white", 255, 255, 255, 255 );
	vk.images.push_back( vk.whiteImage );
	vk.imagesByName[vk.whiteImage->name] = 0;

	vk.transparentImage = VK_CreateSolidImage( "*transparent", 0, 0, 0, 0 );
	vk.images.push_back( vk.transparentImage );
	vk.imagesByName[vk.transparentImage->name] = (qhandle_t)( vk.images.size() - 1 );

	ri.Printf( PRINT_ALL, "rd-vulkan: initialized on %s (%dx%d)\n",
		vk.physicalDeviceProps.deviceName, vk.glConfig.vidWidth, vk.glConfig.vidHeight );
}

void VK_Shutdown( qboolean destroyWindow )
{
	// CL_FlushMemory() (client/cl_main.cpp) calls re.Shutdown(qfalse, qfalse)
	// - "don't destroy window or context" - on every map load, expecting a
	// GL-style soft restart where the window/context survive and
	// RE_BeginRegistration's later R_Init() (rd-vanilla) or no-op
	// (rd-vulkan) call cheaply rebuilds internal state. This renderer has no
	// equivalent notion of "recreate everything except the window" - tearing
	// down the VkDevice/VkInstance here when destroyWindow is false would
	// invalidate them with nothing to recreate them (RE_BeginRegistration
	// doesn't call R_Init again), crashing on the next texture registration.
	// So: only do a real teardown when the window itself is going away.
	if ( !destroyWindow )
	{
		return;
	}

	if ( vk.device )
	{
		vkDeviceWaitIdle( vk.device );
	}

	VK_ShutdownImages();
	VK_DestroyReadbackImage();
	VK_ShutdownGhoul2Models();
	VK_ShutdownWorld();
	VK_ShutdownWorldEffects();

	if ( vk.worldPipeline ) vkDestroyPipeline( vk.device, vk.worldPipeline, nullptr );
	if ( vk.skyPipeline ) vkDestroyPipeline( vk.device, vk.skyPipeline, nullptr );
	if ( vk.worldPipelineAlpha ) vkDestroyPipeline( vk.device, vk.worldPipelineAlpha, nullptr );
	if ( vk.worldPipelineAdditive ) vkDestroyPipeline( vk.device, vk.worldPipelineAdditive, nullptr );
	if ( vk.worldPipelineLayout ) vkDestroyPipelineLayout( vk.device, vk.worldPipelineLayout, nullptr );
	if ( vk.worldSampler ) vkDestroySampler( vk.device, vk.worldSampler, nullptr );
	if ( vk.ghoul2DescriptorPool ) vkDestroyDescriptorPool( vk.device, vk.ghoul2DescriptorPool, nullptr );
	if ( vk.worldDescriptorPool ) vkDestroyDescriptorPool( vk.device, vk.worldDescriptorPool, nullptr );
	if ( vk.worldDescriptorSetLayout ) vkDestroyDescriptorSetLayout( vk.device, vk.worldDescriptorSetLayout, nullptr );

	if ( vk.polyPipeline ) vkDestroyPipeline( vk.device, vk.polyPipeline, nullptr );
	if ( vk.polyPipelineAdditive ) vkDestroyPipeline( vk.device, vk.polyPipelineAdditive, nullptr );
	if ( vk.polyPipelineOpaque ) vkDestroyPipeline( vk.device, vk.polyPipelineOpaque, nullptr );
	if ( vk.polyPipelineLayout ) vkDestroyPipelineLayout( vk.device, vk.polyPipelineLayout, nullptr );
	if ( vk.polyVertexBufferMemory ) vkUnmapMemory( vk.device, vk.polyVertexBufferMemory );
	if ( vk.polyVertexBuffer ) vkDestroyBuffer( vk.device, vk.polyVertexBuffer, nullptr );
	if ( vk.polyVertexBufferMemory ) vkFreeMemory( vk.device, vk.polyVertexBufferMemory, nullptr );
	if ( vk.weatherVertexBufferMemory ) vkUnmapMemory( vk.device, vk.weatherVertexBufferMemory );
	if ( vk.weatherVertexBuffer ) vkDestroyBuffer( vk.device, vk.weatherVertexBuffer, nullptr );
	if ( vk.weatherVertexBufferMemory ) vkFreeMemory( vk.device, vk.weatherVertexBufferMemory, nullptr );
	if ( vk.flareVertexBufferMemory ) vkUnmapMemory( vk.device, vk.flareVertexBufferMemory );
	if ( vk.flareVertexBuffer ) vkDestroyBuffer( vk.device, vk.flareVertexBuffer, nullptr );
	if ( vk.flareVertexBufferMemory ) vkFreeMemory( vk.device, vk.flareVertexBufferMemory, nullptr );

	if ( vk.depthImageView ) vkDestroyImageView( vk.device, vk.depthImageView, nullptr );
	if ( vk.depthImage ) vkDestroyImage( vk.device, vk.depthImage, nullptr );
	if ( vk.depthImageMemory ) vkFreeMemory( vk.device, vk.depthImageMemory, nullptr );

	if ( vk.uiVertexBufferMemory ) vkUnmapMemory( vk.device, vk.uiVertexBufferMemory );
	if ( vk.uiVertexBuffer ) vkDestroyBuffer( vk.device, vk.uiVertexBuffer, nullptr );
	if ( vk.uiVertexBufferMemory ) vkFreeMemory( vk.device, vk.uiVertexBufferMemory, nullptr );
	if ( vk.uiSampler ) vkDestroySampler( vk.device, vk.uiSampler, nullptr );
	if ( vk.uiPipeline ) vkDestroyPipeline( vk.device, vk.uiPipeline, nullptr );
	if ( vk.uiPipelineAdditive ) vkDestroyPipeline( vk.device, vk.uiPipelineAdditive, nullptr );
	if ( vk.uiPipelineOpaque ) vkDestroyPipeline( vk.device, vk.uiPipelineOpaque, nullptr );
	if ( vk.uiPipelineLayout ) vkDestroyPipelineLayout( vk.device, vk.uiPipelineLayout, nullptr );
	if ( vk.uiDescriptorPool ) vkDestroyDescriptorPool( vk.device, vk.uiDescriptorPool, nullptr );
	if ( vk.uiDescriptorSetLayout ) vkDestroyDescriptorSetLayout( vk.device, vk.uiDescriptorSetLayout, nullptr );

	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		if ( vk.frames[i].inFlight ) vkDestroyFence( vk.device, vk.frames[i].inFlight, nullptr );
		if ( vk.frames[i].renderFinished ) vkDestroySemaphore( vk.device, vk.frames[i].renderFinished, nullptr );
		if ( vk.frames[i].imageAvailable ) vkDestroySemaphore( vk.device, vk.frames[i].imageAvailable, nullptr );
		if ( vk.frames[i].commandPool ) vkDestroyCommandPool( vk.device, vk.frames[i].commandPool, nullptr );
	}

	for ( auto fb : vk.swapchainFramebuffers ) vkDestroyFramebuffer( vk.device, fb, nullptr );
	if ( vk.renderPass ) vkDestroyRenderPass( vk.device, vk.renderPass, nullptr );
	for ( auto view : vk.swapchainImageViews ) vkDestroyImageView( vk.device, view, nullptr );
	if ( vk.swapchain ) vkDestroySwapchainKHR( vk.device, vk.swapchain, nullptr );
	if ( vk.device ) vkDestroyDevice( vk.device, nullptr );
	if ( vk.surface ) vkDestroySurfaceKHR( vk.instance, vk.surface, nullptr );

	if ( vk.debugMessenger )
	{
		auto destroyDebugUtils = (PFN_vkDestroyDebugUtilsMessengerEXT)
			vkGetInstanceProcAddr( vk.instance, "vkDestroyDebugUtilsMessengerEXT" );
		if ( destroyDebugUtils ) destroyDebugUtils( vk.instance, vk.debugMessenger, nullptr );
	}
	if ( vk.instance ) vkDestroyInstance( vk.instance, nullptr );

	R_ShutdownFonts();

	if ( destroyWindow )
	{
		ri.WIN_Shutdown();
	}

	vk = vkGlobals_t();
}

void RE_Shutdown( qboolean destroyWindow, qboolean restarting )
{
	(void)restarting;
	VK_Shutdown( destroyWindow );
}

void RE_BeginRegistration( glconfig_t *config )
{
	*config = vk.glConfig;
	vk.registered = qtrue;
}

// Everything below this point is 3D world/model rendering. Static opaque
// world geometry (tr_world.cpp) and Ghoul2 character/weapon models in their
// bind pose (tr_model.cpp) are real now - everything else here (dynamic
// lights, polys, effects) is still a deliberately safe no-op rather than
// left NULL, so the plugin doesn't crash when e.g. a map is loaded; they
// just won't draw anything. See README.md for exactly what's real vs
// stubbed.

// Returning 0 ("failed to register") here isn't just "no model renders" -
// some game-side code (e.g. cg_main.cpp's misc_model_static spawning) treats
// a failed model registration as fatal (Com_Error(ERR_DROP, ...)), which
// aborts map loading entirely before RE_RenderScene ever gets a chance to
// draw the world. So a .md3 that fails to actually load (VK_LoadMD3Model
// returns 0 - bad ident/version, unreadable file, no drawable surfaces)
// still falls through to the same fake non-zero handle every other model
// kind gets, rather than propagating that failure - "never renders" is an
// acceptable outcome here, aborting map load over it is not.
//
// Real .md3 loading (VK_LoadMD3Model, tr_model.cpp) was added after this
// always-fake-handle stub turned out to be hiding an actual, confirmed
// rendering bug rather than just an intentionally-unimplemented one: vjun1's
// opening cutscene cockpit interior
// (models/map_objects/cinematics/raven_cockpit.md3) is exactly this kind of
// entity, and with every non-Ghoul2 RT_MODEL entity unconditionally skipped
// at draw time (see VK_DrawGhoul2Entities, tr_model.cpp), it silently never
// rendered at all - see README.md. Other non-Ghoul2 model kinds this engine
// doesn't otherwise use for anything real (no other extension shows up in
// practice) still get the harmless fake handle unchanged.
qhandle_t RE_RegisterModel( const char *name ) {
	if ( name && COM_CompareExtension( name, ".md3" ) )
	{
		int idx = VK_LoadMD3Model( name );
		if ( idx > 0 )
		{
			return VK_STATIC_MODEL_HANDLE_BASE + idx;
		}
	}
	return 1;
}
// Real implementation (tr_model.cpp: VK_RegisterSkin) - Ghoul2 humanoid
// models need it to resolve any texture at all, see G2API_InitGhoul2Model
// below and VulkanSkin's comment in tr_model.cpp.
qhandle_t RE_RegisterSkin( const char *name ) { return (qhandle_t)VK_RegisterSkin( name ); }
// Real implementation (rd-vanilla's RE_GetAnimationCFG, tr_skin.cpp): reads
// a *.cfg file (a skeleton's own <name>.cfg, or its animation.cfg fallback)
// via the filesystem and copies its text into psDest. This was previously a
// stub that unconditionally reported "file not found" (returned 0 with no
// filesystem access at all) - which meant NPC_stats.cpp's
// G_ParseAnimationFile ALWAYS failed to parse every model's animation.cfg,
// so every animation_t entry in every knownAnimFileSets slot stayed at its
// zeroed-out init state (numFrames == 0) for the entire life of the process.
// bg_panimate.cpp's PM_SetAnimFinal checks exactly that
// (`animations[anim].numFrames==0`) before doing anything else and silently
// returns if it's true - so this one stub meant NPC_SetAnim's calls into
// PM_SetAnimFinal were a no-op for every character, every time, with no
// exceptions - the real root cause of "every character shows the same
// strange, non-gameplay pose in every screenshot" (see README.md's
// character-animation investigation section). Not the G2API_GetBoneIndex/
// per-bone-animation-state bug fixed alongside this one (real, but never
// actually reached - PM_SetAnimFinal returned here long before getting to
// the bodyBone/torsBone gates that bug affects), and not the fixedtime
// cvar-name test-harness bug (real too, but orthogonal - it affects how
// comparable two *correctly playing* animations are across renderers, not
// whether animation plays at all). rd-vanilla caches file contents by
// filename as a documented dev-mode hot-reload convenience, not something
// correctness depends on - not reproduced here since the caller
// (G_ParseAnimationFile) already only calls this once per distinct
// skeletonName per level via its own knownAnimFileSets lookup.
int RE_GetAnimationCFG( const char *psCFGFilename, char *psDest, int iDestSize )
{
	fileHandle_t f;
	long len = ri.FS_FOpenFileRead( psCFGFilename, &f, qfalse );
	if ( len <= 0 || !f )
	{
		return 0;
	}

	std::vector<char> buffer( (size_t)len + 1 );
	ri.FS_Read( buffer.data(), (int)len, f );
	buffer[(size_t)len] = '\0';
	ri.FS_FCloseFile( f );

	if ( psDest && iDestSize > 0 )
	{
		Q_strncpyz( psDest, buffer.data(), iDestSize );
	}
	return (int)strlen( buffer.data() );
}
// RE_LoadWorldMap/RE_RenderScene are real implementations in tr_world.cpp,
// RE_ClearScene/RE_AddRefEntityToScene in tr_model.cpp - not stubs, see
// README.md for exactly what they draw.
void RE_RegisterMedia_LevelLoadBegin( const char *psMapName, ForceReload_e eForceReload, qboolean bAllowScreenDissolve ) { (void)psMapName; (void)eForceReload; (void)bAllowScreenDissolve; }
void RE_RegisterMedia_LevelLoadEnd( void ) {}
int RE_RegisterMedia_GetLevel( void ) { return 0; }
qboolean RE_RegisterModels_LevelLoadEnd( qboolean bDeleteEverythingNotUsedThisLevel ) { (void)bDeleteEverythingNotUsedThisLevel; return qfalse; }
qboolean RE_RegisterImages_LevelLoadEnd( void ) { return qfalse; }
void RE_SetWorldVisData( const byte *vis ) { (void)vis; }
void RE_EndRegistration( void ) {}
// Real implementation in tr_model.cpp, alongside the rest of this
// renderer's per-frame scene-queue handling (RE_AddRefEntityToScene/
// RE_ClearScene) - see VK_DrawScenePolys there.
void RE_AddLightToScene( const vec3_t org, float intensity, float r, float g, float b ) { (void)org; (void)intensity; (void)r; (void)g; (void)b; }
qboolean RE_GetLighting( const vec3_t org, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir )
{
	(void)org;
	VectorSet( ambientLight, 1.f, 1.f, 1.f );
	VectorSet( directedLight, 0.f, 0.f, 0.f );
	VectorSet( lightDir, 0.f, 0.f, 1.f );
	return qfalse;
}
// Real formula ported directly from rd-vanilla's RB_RotatePic (tr_backend.cpp),
// not a guess: rotates a w*h quad by `a1` degrees around the pivot (x+w, y) -
// the same "one corner of the unrotated rect stays fixed" convention real
// vanilla uses (a1=0 reproduces RE_StretchPic's own 4 corners exactly, since
// vertex1 always lands on the pivot itself). Real callers: the seeker-missile
// lock-on warning wedges (cg_draw.cpp, CG_DrawRotatePic - 8 wedges stepped 45
// degrees apart) - only active while an actual seeker missile is tracking the
// player, a transient gameplay condition this renderer's fixed spawn-screenshot
// regression scenes never trigger, so this couldn't be verified via that
// harness; verified instead by exact formula match against the real source
// plus a clean warning-free build and full regression suite pass (no crashes,
// no visible change to any of the 4 fixed scenes, none of which call this).
void RE_DrawRotatePic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, float a1, qhandle_t hShader )
{
	float angle = DEG2RAD( a1 );
	float c = cosf( angle );
	float s = sinf( angle );
	float tx = x + w;
	float ty = y;

	float v0x = c * -w + tx, v0y = s * -w + ty;
	float v1x = tx, v1y = ty;
	float v2x = -s * h + tx, v2y = c * h + ty;
	float v3x = c * -w - s * h + tx, v3y = s * -w + c * h + ty;

	VK_DrawQuad( v0x, v0y, s1, t1, v1x, v1y, s2, t1, v2x, v2y, s2, t2, v3x, v3y, s1, t2, hShader );
}
// Real formula ported directly from rd-vanilla's RB_RotatePic2
// (tr_backend.cpp) - rotates a w*h quad by `a1` degrees around its own
// *center* (x, y), unlike RE_DrawRotatePic's corner pivot above. Real
// callers: the Disruptor rifle's zoomed-scope overlay (cg_draw.cpp - the
// full-screen `disruptorInsert` reticle graphic, rotated by the current zoom
// level, plus a ring of small `disruptorInsertTick` ammo marks each rotated
// to face outward) - only active while actually zoomed in with that specific
// weapon equipped, again a transient gameplay state this renderer's fixed
// spawn-screenshot scenes never reach, so not directly screenshot-verified -
// same verification basis as RE_DrawRotatePic above (exact formula match,
// clean build, full regression suite unaffected).
void RE_DrawRotatePic2( float x, float y, float w, float h, float s1, float t1, float s2, float t2, float a1, qhandle_t hShader )
{
	float angle = DEG2RAD( a1 );
	float c = cosf( angle );
	float s = sinf( angle );
	float halfW = w * 0.5f;
	float halfH = h * 0.5f;

	float v0x = c * -halfW + -s * -halfH + x, v0y = s * -halfW + c * -halfH + y;
	float v1x = c * halfW + -s * -halfH + x, v1y = s * halfW + c * -halfH + y;
	float v2x = c * halfW + -s * halfH + x, v2y = s * halfW + c * halfH + y;
	float v3x = c * -halfW + -s * halfH + x, v3y = s * -halfW + c * halfH + y;

	VK_DrawQuad( v0x, v0y, s1, t1, v1x, v1y, s2, t1, v2x, v2y, s2, t2, v3x, v3y, s1, t2, hShader );
}
void RE_LAGoggles( void ) {}
void RE_Scissor( float x, float y, float w, float h ) { (void)x; (void)y; (void)w; (void)h; }
void RE_DrawStretchRaw( int x, int y, int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty )
{
	(void)x; (void)y; (void)w; (void)h; (void)cols; (void)rows; (void)data; (void)client; (void)dirty;
}
void RE_UploadCinematic( int cols, int rows, const byte *data, int client, qboolean dirty )
{
	(void)cols; (void)rows; (void)data; (void)client; (void)dirty;
}
qboolean RE_ProcessDissolve( void ) { return qfalse; }
qboolean RE_InitDissolve( qboolean bForceCircularExtroWipe ) { (void)bForceCircularExtroWipe; return qfalse; }
byte *RE_TempRawImage_ReadFromFile( const char *psLocalFilename, int *piWidth, int *piHeight, byte *pbReSampleBuffer, qboolean qbVertFlip )
{
	(void)psLocalFilename; (void)pbReSampleBuffer; (void)qbVertFlip;
	if ( piWidth ) *piWidth = 0;
	if ( piHeight ) *piHeight = 0;
	return nullptr;
}
void RE_TempRawImage_CleanUp( void ) {}
int R_MarkFragments( int numPoints, const vec3_t *points, const vec3_t projection, int maxPoints, vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer )
{
	(void)numPoints; (void)points; (void)projection; (void)maxPoints; (void)pointBuffer; (void)maxFragments; (void)fragmentBuffer;
	return 0;
}
void R_LerpTag( orientation_t *tag, qhandle_t model, int startFrame, int endFrame, float frac, const char *tagName )
{
	(void)model; (void)startFrame; (void)endFrame; (void)frac; (void)tagName;
	AxisClear( tag->axis );
	VectorClear( tag->origin );
}
void R_ModelBounds( qhandle_t model, vec3_t mins, vec3_t maxs ) { (void)model; VectorClear( mins ); VectorClear( maxs ); }
void RE_GetLightStyle( int style, color4ub_t color ) { (void)style; color[0] = color[1] = color[2] = color[3] = 255; }
void RE_SetLightStyle( int style, int color ) { (void)style; (void)color; }
void RE_GetBModelVerts( int bmodelIndex, vec3_t *vec, vec3_t normal ) { (void)bmodelIndex; (void)vec; (void)normal; }
// Real implementation - tr_weather.cpp. See that file's own header comment
// and tr_local.h's declarations for the full picture (a scoped port of
// rd-vanilla's real tr_WorldEffects.cpp).
void R_WorldEffectCommand( const char *command ) { VK_WorldEffectCommand( command ); }
void RE_GetModelBounds( refEntity_t *refEnt, vec3_t bounds1, vec3_t bounds2 ) { (void)refEnt; VectorClear( bounds1 ); VectorClear( bounds2 ); }
void RE_SVModelInit( void ) {}
void R_InitWorldEffects( void ) { VK_InitWorldEffects(); }
void R_ClearStuffToStopGhoul2CrashingThings( void ) {}
qboolean R_inPVS( vec3_t p1, vec3_t p2 ) { (void)p1; (void)p2; return qtrue; }
static float s_zero = 0.f;
static qboolean s_qfalse = qfalse;
float *get_tr_distortionAlpha( void ) { return &s_zero; }
float *get_tr_distortionStretch( void ) { return &s_zero; }
qboolean *get_tr_distortionPrePost( void ) { return &s_qfalse; }
qboolean *get_tr_distortionNegate( void ) { return &s_qfalse; }
bool R_GetWindVector( vec3_t windVector, vec3_t atPoint ) { return VK_GetWindVector( windVector, atPoint ); }
bool R_GetWindGusting( vec3_t atpoint ) { return VK_GetWindGusting( atpoint ); }
bool R_IsOutside( vec3_t pos ) { return VK_IsOutside( pos ); }
float R_IsOutsideCausingPain( vec3_t pos ) { return VK_IsOutsideCausingPain( pos ); }
float R_GetChanceOfSaberFizz( void ) { return VK_GetChanceOfSaberFizz(); }
bool R_IsShaking( vec3_t pos ) { return VK_IsShaking( pos ); }
void R_AddWeatherZone( vec3_t mins, vec3_t maxs ) { VK_AddWeatherZone( mins, maxs ); }
bool R_SetTempGlobalFogColor( vec3_t color ) { return VK_SetTempGlobalFogColor( color ); }
// Real implementation - tr_world.cpp's VK_SetRangedFog. See that function's
// own declaration comment (tr_local.h) for what "ranged fog" is and why
// it's implemented but unverified against this renderer's own test maps.
void RE_SetRangedFog( float dist ) { VK_SetRangedFog( dist ); }

void R_ScreenShotPNG_f( void );

// ============================================================================
// Ghoul2 - NOT reused from rd-vanilla (see CMakeLists.txt comment: G2_*.cpp's
// quote-include of "tr_local.h" resolves to rd-vanilla's own directory, so
// "reusing" those files as-is actually means porting rd-vanilla's whole
// model/shader registry too). Model-handle bookkeeping (CGhoul2Info_v's
// backing store) is implemented for real below since it's small and
// self-contained; every actual G2API_* entry point is a safe stub for now -
// no character/weapon models will animate or render yet.
// ============================================================================

class CVulkanGhoul2InfoArray : public IGhoul2InfoArray
{
	std::vector<std::vector<CGhoul2Info>> mArray;
	std::vector<bool> mValid;
public:
	// CGhoul2Info_v (game/ghoul2_shared.h) uses handle 0 as its own "not
	// allocated yet" sentinel (mItem == 0 means empty/null throughout that
	// class), so a *valid* handle from New() must never be 0 or every
	// CGhoul2Info_v holding it would be indistinguishable from an empty one
	// and hit its own assert(mItem) in operator[]. Burn index 0 permanently
	// at construction so real allocations start at 1.
	CVulkanGhoul2InfoArray()
	{
		mArray.emplace_back();
		mValid.push_back( false );
	}
	int New() override
	{
		for ( size_t i = 1; i < mValid.size(); i++ )
		{
			if ( !mValid[i] )
			{
				mValid[i] = true;
				mArray[i].clear();
				return (int)i;
			}
		}
		mArray.emplace_back();
		mValid.push_back( true );
		return (int)mArray.size() - 1;
	}
	void Delete( int handle ) override
	{
		if ( handle >= 0 && (size_t)handle < mValid.size() )
		{
			mValid[handle] = false;
			mArray[handle].clear();
		}
	}
	bool IsValid( int handle ) const override
	{
		return handle >= 0 && (size_t)handle < mValid.size() && mValid[handle];
	}
	std::vector<CGhoul2Info> &Get( int handle ) override { return mArray[handle]; }
	const std::vector<CGhoul2Info> &Get( int handle ) const override { return mArray[handle]; }
};

static CVulkanGhoul2InfoArray s_ghoul2InfoArray;
IGhoul2InfoArray &TheGhoul2InfoArray() { return s_ghoul2InfoArray; }

// Real surface-bolt support now (see VK_GetGhoul2SurfaceBoltMatrix's own
// comment, tr_model.cpp, for the matrix side) - mirrors rd-vanilla's real
// G2_Add_Bolt (G2_bolts.cpp) exactly, including its precedence: a surface
// name is tried FIRST, a bone name only if that fails, not the other way
// around. This isn't a minor detail - real, heavily-used game code relies
// on it: every real surface bolt name in this codebase's own game code
// (g_client.cpp's "*head_eyes" on every single player spawn, wp_saber.cpp's
// "*flash"/"*r_hand_cap_r_arm"/"*l_hand_cap_l_arm", g_turret.cpp's
// "*muzzle1"/"*flash03", g_emplaced.cpp's "*cannonflash"/"*seat", ...) uses
// the real Ghoul2 convention of a literal leading "*" in the surface's own
// name - confirmed by directly parsing real .glm data (kyle/model.glm has
// 45 such surfaces, saber_1.glm's blade tag is "*blade1"), not assumed.
// Every one of those would have silently returned -1 before this (bone-only
// lookup, no surface fallback), and a caller that doesn't check for that
// (most don't) would go on to query/attach at a permanently-invalid bolt.
//
// Reuse a bolt already on this surface/bone, or a freed (-1,-1) slot, else
// append - a bolt index is a position in ghlInfo->mBltlist, not a global
// handle, same as before.
int G2API_AddBolt( CGhoul2Info *ghlInfo, const char *boneName )
{
	if ( !ghlInfo || !boneName || !boneName[0] )
	{
		return -1;
	}

	int surfIndex = VK_FindGhoul2SurfaceIndex( (int)ghlInfo->mModel, boneName, nullptr );
	if ( surfIndex >= 0 )
	{
		for ( size_t i = 0; i < ghlInfo->mBltlist.size(); i++ )
		{
			if ( ghlInfo->mBltlist[i].surfaceNumber == surfIndex )
			{
				ghlInfo->mBltlist[i].boltUsed++;
				return (int)i;
			}
		}
		for ( size_t i = 0; i < ghlInfo->mBltlist.size(); i++ )
		{
			if ( ghlInfo->mBltlist[i].boneNumber == -1 && ghlInfo->mBltlist[i].surfaceNumber == -1 )
			{
				ghlInfo->mBltlist[i].surfaceNumber = surfIndex;
				ghlInfo->mBltlist[i].boltUsed = 1;
				return (int)i;
			}
		}
		boltInfo_t surfBolt;
		surfBolt.surfaceNumber = surfIndex;
		surfBolt.boltUsed = 1;
		ghlInfo->mBltlist.push_back( surfBolt );
		return (int)ghlInfo->mBltlist.size() - 1;
	}

	int boneIndex = VK_FindGhoul2Bone( (int)ghlInfo->mModel, boneName );
	if ( boneIndex < 0 )
	{
		return -1;
	}
	for ( size_t i = 0; i < ghlInfo->mBltlist.size(); i++ )
	{
		if ( ghlInfo->mBltlist[i].boneNumber == boneIndex )
		{
			ghlInfo->mBltlist[i].boltUsed++;
			return (int)i;
		}
	}
	for ( size_t i = 0; i < ghlInfo->mBltlist.size(); i++ )
	{
		if ( ghlInfo->mBltlist[i].boneNumber == -1 && ghlInfo->mBltlist[i].surfaceNumber == -1 )
		{
			ghlInfo->mBltlist[i].boneNumber = boneIndex;
			ghlInfo->mBltlist[i].boltUsed = 1;
			return (int)i;
		}
	}
	boltInfo_t bolt;
	bolt.boneNumber = boneIndex;
	bolt.boltUsed = 1;
	ghlInfo->mBltlist.push_back( bolt );
	return (int)ghlInfo->mBltlist.size() - 1;
}
int G2API_AddBoltSurfNum( CGhoul2Info *ghlInfo, const int surfIndex ) { (void)ghlInfo; (void)surfIndex; return -1; }
int G2API_AddSurface( CGhoul2Info *ghlInfo, int surfaceNumber, int polyNumber, float bi, float bj, int lod ) { (void)ghlInfo; (void)surfaceNumber; (void)polyNumber; (void)bi; (void)bj; (void)lod; return -1; }
void G2API_AnimateG2Models( CGhoul2Info_v &ghoul2, int t, CRagDollUpdateParams *p ) { (void)ghoul2; (void)t; (void)p; }
qboolean G2API_AttachEnt( int *boltInfo, CGhoul2Info *ghlInfoTo, int toBoltIndex, int entNum, int toModelNum ) { (void)boltInfo; (void)ghlInfoTo; (void)toBoltIndex; (void)entNum; (void)toModelNum; return qfalse; }
// Real model-to-model attachment now (see VK_DrawGhoul2Entities's own
// comment, tr_model.cpp, for how mModelBoltLink is actually consumed at
// draw time) - same encoding as rd-vanilla's real G2API_AttachG2Model
// (G2_API.cpp): MODEL_SHIFT/BOLT_SHIFT/MODEL_AND/BOLT_AND (ghoul2/G2.h) are
// small, fixed bit-packing constants copied verbatim, not rederived, since
// getting a bit-packing width subtly wrong is easy to do and hard to
// notice by eye. Confirmed real, exercised usage before implementing:
// wp_saber.cpp calls this to attach a held saber/weapon's own sub-model
// (`ent->ghoul2[weaponModel]`) to a bolt on the player body's sub-model
// (`ent->ghoul2[playerModel]`) - both indices into the *same* entity's
// ghoul2 vector, which is exactly what `toModel` identifies here. The
// kG2ModelWidth/kG2BoltShift/etc. constants live in tr_local.h, shared with
// the decode side in tr_model.cpp.
qboolean G2API_AttachG2Model( CGhoul2Info *ghlInfo, CGhoul2Info *ghlInfoTo, int toBoltIndex, int toModel )
{
	if ( !ghlInfo || !ghlInfoTo )
	{
		return qfalse;
	}
	if ( toBoltIndex < 0 || (size_t)toBoltIndex >= ghlInfoTo->mBltlist.size() )
	{
		return qfalse;
	}
	// Real bolt, not a freed (-1,-1) slot - same check as rd-vanilla's own.
	if ( ghlInfoTo->mBltlist[toBoltIndex].boneNumber == -1 && ghlInfoTo->mBltlist[toBoltIndex].surfaceNumber == -1 )
	{
		return qfalse;
	}
	ghlInfo->mModelBoltLink = ( ( toModel & kG2ModelAnd ) << kG2ModelShift ) | ( ( toBoltIndex & kG2BoltAnd ) << kG2BoltShift );
	return qtrue;
}
void G2API_CollisionDetect( CCollisionRecord *collRecMap, CGhoul2Info_v &ghoul2, const vec3_t angles, const vec3_t position, int frameNumber, int entNum, vec3_t rayStart, vec3_t rayEnd, vec3_t scale, CMiniHeap *heap, EG2_Collision traceType, int useLod, float fRadius )
{
	(void)collRecMap; (void)ghoul2; (void)angles; (void)position; (void)frameNumber; (void)entNum; (void)rayStart; (void)rayEnd; (void)scale; (void)heap; (void)traceType; (void)useLod; (void)fRadius;
}
void G2API_CleanGhoul2Models( CGhoul2Info_v &ghoul2 ) { ghoul2.clear(); }
void G2API_CopyGhoul2Instance( CGhoul2Info_v &from, CGhoul2Info_v &to, int modelIndex ) { (void)from; (void)to; (void)modelIndex; }
void G2API_DetachEnt( int *boltInfo ) { (void)boltInfo; }
qboolean G2API_DetachG2Model( CGhoul2Info *ghlInfo ) { if ( !ghlInfo ) return qfalse; ghlInfo->mModelBoltLink = -1; return qtrue; }
qboolean G2API_GetAnimFileName( CGhoul2Info *ghlInfo, char **filename ) { (void)ghlInfo; if ( filename ) *filename = nullptr; return qfalse; }
char *G2API_GetAnimFileNameIndex( qhandle_t modelIndex ) { (void)modelIndex; return nullptr; }
char *G2API_GetAnimFileInternalNameIndex( qhandle_t modelIndex ) { (void)modelIndex; return nullptr; }
// Real now - see G2API_SetAnimIndex's own comment below for the mechanism
// this is the read side of (CGhoul2Info::animModelIndexOffset, already a
// real field on the shared struct - not new API surface, just an unread
// one until now).
int G2API_GetAnimIndex( CGhoul2Info *ghlInfo ) { return ghlInfo ? ghlInfo->animModelIndexOffset : 0; }
// Resolves a By-name G2API call's boneName to the same real skeleton bone
// index an equivalent ...Index call would already have (ghlInfo->mModel is
// the model cache index VK_FindGhoul2Bone expects - the same field
// VK_DrawGhoul2Entities, tr_model.cpp, reads as g2Instance.mModel). This is
// the one place in this whole file that dereferences ghlInfo - safe because
// every real caller passes the address of a live CGhoul2Info the game
// itself owns (e.g. &ent->ghoul2[ent->playerModel]), never a dangling or
// null one after the qtrue/qfalse null checks below. -1 (VK_FindGhoul2Bone's
// "not found" return) is a perfectly ordinary key into
// s_ghoul2AnimState's per-bone map, not a special case that needs handling
// here - see that map's own comment (tr_model.cpp).
static int VK_ResolveGhoul2AnimBone( CGhoul2Info *ghlInfo, const char *boneName )
{
	if ( !ghlInfo || !boneName )
	{
		return -1;
	}
	return VK_FindGhoul2Bone( ghlInfo->mModel, boneName );
}
// GetAnimRange reports the *stored* startFrame/endFrame verbatim (not the
// current playback position - that's GetBoneAnim's currentFrame), so this
// can reuse GetBoneAnim's lookup and just discard the frame/flags/speed
// outputs it doesn't need.
qboolean G2API_GetAnimRange( CGhoul2Info *ghlInfo, const char *boneName, int *startFrame, int *endFrame ) { return VK_GetGhoul2BoneAnim( ghlInfo, VK_ResolveGhoul2AnimBone( ghlInfo, boneName ), 0, nullptr, startFrame, endFrame, nullptr, nullptr ) ? qtrue : qfalse; }
qboolean G2API_GetAnimRangeIndex( CGhoul2Info *ghlInfo, const int boneIndex, int *startFrame, int *endFrame ) { return VK_GetGhoul2BoneAnim( ghlInfo, boneIndex, 0, nullptr, startFrame, endFrame, nullptr, nullptr ) ? qtrue : qfalse; }
// Real per-bone-region tracking now (see s_ghoul2AnimState's comment,
// tr_model.cpp, for the bug this fixes): the By-name and ...Index variants
// of Get/SetBoneAnim are just two ways to name the same underlying bone
// index, resolved once here rather than each duplicating the lookup.
qboolean G2API_GetBoneAnim( CGhoul2Info *ghlInfo, const char *boneName, const int t, float *currentFrame, int *startFrame, int *endFrame, int *flags, float *animSpeed, int *modelList ) { (void)modelList; return VK_GetGhoul2BoneAnim( ghlInfo, VK_ResolveGhoul2AnimBone( ghlInfo, boneName ), t, currentFrame, startFrame, endFrame, flags, animSpeed ) ? qtrue : qfalse; }
qboolean G2API_GetBoneAnimIndex( CGhoul2Info *ghlInfo, const int boneIndex, const int t, float *currentFrame, int *startFrame, int *endFrame, int *flags, float *animSpeed, int *modelList ) { (void)modelList; return VK_GetGhoul2BoneAnim( ghlInfo, boneIndex, t, currentFrame, startFrame, endFrame, flags, animSpeed ) ? qtrue : qfalse; }
// Used throughout game code (60+ call sites - g_client.cpp's
// rootBone/lowerLumbarBone/motionBone, g_turret.cpp, g_emplaced.cpp,
// g_combat.cpp, ...) to cache a bone index once and reuse it - a real,
// previously always-wrong stub (unconditionally returned -1 regardless of
// input) that this renderer never noticed because Get/SetBoneAnim used to
// ignore whatever index they were given anyway. Now that they don't (see
// s_ghoul2AnimState's comment, tr_model.cpp), a caller that cached this
// stub's -1 forever would still collide every distinct bone into the same
// key - this fix is what actually makes the per-bone tracking above mean
// anything for real game callers, not just for direct ...Index calls that
// happen to pass a real index already. bAddIfNotFound is ignored: unlike
// the real engine's internal "active bone list" cache, every skeleton bone
// is already available via VK_FindGhoul2Bone with no separate add step.
int G2API_GetBoneIndex( CGhoul2Info *ghlInfo, const char *boneName, qboolean bAddIfNotFound ) { (void)bAddIfNotFound; return VK_ResolveGhoul2AnimBone( ghlInfo, boneName ); }
// Byte-for-byte-in-spirit copy of rd-vanilla's real Multiply_3x4Matrix
// (tr_ghoul2.cpp) for mdxaBone_t's row-major 3x4 affine representation -
// NOT the same convention as VK_MultiplyMatrix (tr_world.cpp)'s column-major
// mat4 float[16], so it isn't reused from there. out = in2 applied after in
// (i.e. "in" transforms first, then "in2" - out = in2 * in in ordinary
// matrix-product notation, matching the real function's own doc comment
// via its usage, not just its name).
static void VK_Multiply3x4Matrix( mdxaBone_t *out, const mdxaBone_t *in2, const mdxaBone_t *in )
{
	for ( int row = 0; row < 3; row++ )
	{
		for ( int col = 0; col < 3; col++ )
		{
			out->matrix[row][col] =
				in2->matrix[row][0] * in->matrix[0][col] +
				in2->matrix[row][1] * in->matrix[1][col] +
				in2->matrix[row][2] * in->matrix[2][col];
		}
		out->matrix[row][3] =
			in2->matrix[row][0] * in->matrix[0][3] +
			in2->matrix[row][1] * in->matrix[1][3] +
			in2->matrix[row][2] * in->matrix[2][3] +
			in2->matrix[row][3];
	}
}

// frameNum/modelList (real engine legacy parameters, unused by the real
// implementation either) are ignored, but the bolt itself now reports this
// instance's actual *currently animated* pose, not a fixed rest pose - see
// VK_GetGhoul2BoneCurrentPoseMat's comment (tr_model.cpp) for why: an
// earlier bind-pose-only version of this function fixed academy1's cutscene
// camera collapsing onto the NPC's own origin (see README.md's "Ghoul2
// rendering" section for that story), but left the camera - bolted to this
// NPC and re-queried every frame by cg_camera.cpp's CGCam_FollowUpdate -
// pointed in a fixed rest-pose direction regardless of the NPC's real
// current pose, a second, subtler bug the debug-log comparison against
// rd-vanilla in README.md's character-animation investigation section
// caught: the NPC's own skeleton pose matched rd-vanilla frame-for-frame,
// yet the two renderers' cutscene camera angle for the identical shot
// didn't, because only the camera's bolt matrix - not the NPC's own
// skin - was still using the rest pose. g_vkGhoul2LastRenderTime (extern,
// tr_model.cpp) is the only per-frame animation clock available here:
// this function has no currentTime of its own (cg_camera.cpp doesn't pass
// one), so it uses whatever time this renderer most recently drew a Ghoul2
// scene at - one render-call of lag behind the very next RE_RenderScene,
// unavoidable and harmless for a camera that's re-queried every frame.
extern int g_vkGhoul2LastRenderTime;
qboolean G2API_GetBoltMatrix( CGhoul2Info_v &ghoul2, const int modelIndex, const int boltIndex, mdxaBone_t *matrix, const vec3_t angles, const vec3_t position, const int frameNum, qhandle_t *modelList, const vec3_t scale )
{
	(void)frameNum; (void)modelList;
	if ( !matrix )
	{
		return qfalse;
	}

	// Entity (world) transform - same construction as rd-vanilla's real
	// Create_Matrix/G2_GenerateWorldMatrix (G2_misc.cpp): column c (i.e.
	// matrix[row][c] for row 0-2) is AnglesToAxis's axis[c] - note `angles`
	// here is a PITCH/YAW/ROLL Euler triple, a different representation
	// from refEntity_t::axis (already a 3x3 basis) used elsewhere in this
	// renderer, so this can't reuse that entity-matrix code either.
	matrix3_t axis;
	AnglesToAxis( angles, axis );
	mdxaBone_t worldMatrix = {};
	for ( int row = 0; row < 3; row++ )
	{
		worldMatrix.matrix[row][0] = axis[0][row];
		worldMatrix.matrix[row][1] = axis[1][row];
		worldMatrix.matrix[row][2] = axis[2][row];
		worldMatrix.matrix[row][3] = position[row];
	}

	mdxaBone_t bolt = {};
	bool ok = false;
	if ( modelIndex >= 0 && modelIndex < ghoul2.size() )
	{
		CGhoul2Info &ghlInfo = ghoul2[modelIndex];
		if ( boltIndex >= 0 && (size_t)boltIndex < ghlInfo.mBltlist.size() && ghlInfo.mBltlist[boltIndex].boneNumber >= 0 )
		{
			ok = VK_GetGhoul2BoneCurrentPoseMat( (int)ghlInfo.mModel, &ghlInfo, ghlInfo.mBltlist[boltIndex].boneNumber, g_vkGhoul2LastRenderTime, &bolt );
		}
		else if ( boltIndex >= 0 && (size_t)boltIndex < ghlInfo.mBltlist.size() && ghlInfo.mBltlist[boltIndex].surfaceNumber >= 0 )
		{
			ok = VK_GetGhoul2SurfaceBoltMatrix( (int)ghlInfo.mModel, ghlInfo.mBltlist[boltIndex].surfaceNumber, &ghlInfo, g_vkGhoul2LastRenderTime, &bolt );
		}
		if ( ok )
		{
			// Same conditional scale-the-translation step as the real
			// G2API_GetBoltMatrix - scale[i]==0 means "no override" for
			// this axis, not "scale to zero" (refEntity_t::modelScale's
			// convention).
			if ( scale[0] ) bolt.matrix[0][3] *= scale[0];
			if ( scale[1] ) bolt.matrix[1][3] *= scale[1];
			if ( scale[2] ) bolt.matrix[2][3] *= scale[2];
		}
	}
	if ( !ok )
	{
		// Same fixed fallback rotation the real G2API_GetBoltMatrix uses on
		// failure (its `identityMatrix` constant, G2_API.cpp) rather than a
		// zeroed matrix - callers that don't check the qfalse return (like
		// cg_camera.cpp's CGCam_FollowUpdate) still get a well-formed
		// matrix instead of a degenerate one.
		bolt.matrix[0][0] = 0; bolt.matrix[0][1] = -1; bolt.matrix[0][2] = 0;
		bolt.matrix[1][0] = 1; bolt.matrix[1][1] = 0;  bolt.matrix[1][2] = 0;
		bolt.matrix[2][0] = 0; bolt.matrix[2][1] = 0;  bolt.matrix[2][2] = 1;
	}

	VK_Multiply3x4Matrix( matrix, &worldMatrix, &bolt );
	return ok ? qtrue : qfalse;
}
int G2API_GetGhoul2ModelFlags( CGhoul2Info *ghlInfo ) { (void)ghlInfo; return 0; }
// Real implementation now (VK_GetGhoul2GLAName, tr_model.cpp) - this used to
// be a stub hardcoded to always report "models/players/_humanoid/_humanoid"
// (with a stray ".glm" that wasn't even in the real *.gla naming convention)
// regardless of ghlInfo, which meant every model - including genuinely
// non-humanoid ones (droids, creatures with their own skeleton) - had its
// animation.cfg resolved against the wrong skeleton name in
// NPC_stats.cpp's G_LoadAnimFileSet/g_client.cpp/g_main.cpp. This didn't
// visibly break academy1-style all-humanoid scenes (see README.md's
// character-animation investigation - RE_GetAnimationCFG being a hard
// always-fails stub was the actual reason *no* animation.cfg loaded there,
// masking this bug entirely) but would have broken animation.cfg lookup for
// any non-humanoid Ghoul2 model once that real blocker was fixed.
//
// Falls back to the standard humanoid skeleton path (matching this stub's
// old always-humanoid answer) rather than nullptr when mModel doesn't
// resolve to a live model+skeleton - ghlInfo->mModel legitimately stays 0
// for models whose particular skin combination this renderer's simpler skin
// system fails to resolve any surfaces for (a separate, pre-existing gap -
// see VK_LoadGhoul2Model's "has no drawable surfaces" case), and
// g_client.cpp's G_StandardHumanoid asserts this is always non-null with no
// null-safe fallback of its own - unlike NPC_stats.cpp's G_LoadAnimFileSet,
// which already treats a null GLAName as an expected "take a guess" case.
char *G2API_GetGLAName( CGhoul2Info *ghlInfo )
{
	static char fallback[] = "models/players/_humanoid/_humanoid";
	if ( !ghlInfo )
	{
		return fallback;
	}
	const char *name = VK_GetGhoul2GLAName( ghlInfo->mModel );
	return name ? const_cast<char *>( name ) : fallback;
}
int G2API_GetParentSurface( CGhoul2Info *ghlInfo, const int index ) { (void)ghlInfo; (void)index; return -1; }
qboolean G2API_GetRagBonePos( CGhoul2Info_v &ghoul2, const char *boneName, vec3_t pos, vec3_t entAngles, vec3_t entPos, vec3_t entScale ) { (void)ghoul2; (void)boneName; (void)entAngles; (void)entPos; (void)entScale; VectorClear( pos ); return qfalse; }
int G2API_GetSurfaceIndex( CGhoul2Info *ghlInfo, const char *surfaceName ) { (void)ghlInfo; (void)surfaceName; return -1; }
char *G2API_GetSurfaceName( CGhoul2Info *ghlInfo, int surfNumber ) { (void)ghlInfo; (void)surfNumber; return nullptr; }
int G2API_GetSurfaceRenderStatus( CGhoul2Info *ghlInfo, const char *surfaceName ) { (void)ghlInfo; (void)surfaceName; return -1; }
int G2API_GetTime( int argTime ) { return argTime; }
// Faithful copy of rd-vanilla's real G2API_GiveMeVectorFromMatrix
// (G2_API.cpp) - extracts a bolt matrix's origin (translation column) or
// one of its three basis vectors (+/- X/Y/Z).
void G2API_GiveMeVectorFromMatrix( mdxaBone_t &boltMatrix, Eorientations flags, vec3_t &vec )
{
	switch ( flags )
	{
		case ORIGIN:
			vec[0] = boltMatrix.matrix[0][3];
			vec[1] = boltMatrix.matrix[1][3];
			vec[2] = boltMatrix.matrix[2][3];
			break;
		case POSITIVE_Y:
			vec[0] = boltMatrix.matrix[0][1];
			vec[1] = boltMatrix.matrix[1][1];
			vec[2] = boltMatrix.matrix[2][1];
			break;
		case POSITIVE_X:
			vec[0] = boltMatrix.matrix[0][0];
			vec[1] = boltMatrix.matrix[1][0];
			vec[2] = boltMatrix.matrix[2][0];
			break;
		case POSITIVE_Z:
			vec[0] = boltMatrix.matrix[0][2];
			vec[1] = boltMatrix.matrix[1][2];
			vec[2] = boltMatrix.matrix[2][2];
			break;
		case NEGATIVE_Y:
			vec[0] = -boltMatrix.matrix[0][1];
			vec[1] = -boltMatrix.matrix[1][1];
			vec[2] = -boltMatrix.matrix[2][1];
			break;
		case NEGATIVE_X:
			vec[0] = -boltMatrix.matrix[0][0];
			vec[1] = -boltMatrix.matrix[1][0];
			vec[2] = -boltMatrix.matrix[2][0];
			break;
		case NEGATIVE_Z:
			vec[0] = -boltMatrix.matrix[0][2];
			vec[1] = -boltMatrix.matrix[1][2];
			vec[2] = -boltMatrix.matrix[2][2];
			break;
	}
}
qboolean G2API_HaveWeGhoul2Models( CGhoul2Info_v &ghoul2 ) { return ghoul2.size() ? qtrue : qfalse; }
qboolean G2API_IKMove( CGhoul2Info_v &ghoul2, int t, sharedIKMoveParams_t *params ) { (void)ghoul2; (void)t; (void)params; return qfalse; }
int G2API_InitGhoul2Model( CGhoul2Info_v &ghoul2, const char *fileName, int modelIndex, qhandle_t customSkin, qhandle_t customShader, int modelFlags, int lodBias )
{
	// Same "find a free slot (mModelindex == -1, CGhoul2Info's default), else
	// append" logic as rd-vanilla's real G2API_InitGhoul2Model (G2_API.cpp) -
	// a single entity's ghoul2 vector commonly holds several sub-models at
	// once (body + weapon + saber blade, each loaded via its own call to
	// this function on the SAME CGhoul2Info_v), so this renderer's original
	// stub behavior (ghoul2.resize(1), unconditionally keeping only one slot)
	// silently discarded all but the last-loaded model - VK_DrawGhoul2Entities
	// (tr_model.cpp) draws every valid slot, not just [0], so that only
	// mattered once model loading itself became real.
	//
	// Always succeeding (never returning -1 for a load failure) is still
	// required regardless: game.so's G_SetG2PlayerModel (g_client.cpp)
	// treats a failed player model load as fatal (Com_Error(ERR_DROP, ...)
	// after also failing its stormtrooper fallback), which would abort map
	// loading entirely before RE_RenderScene ever gets a chance to draw the
	// world - same spirit as RE_RegisterModel below. A slot whose model
	// fails to load (mModel stays 0, see VK_LoadGhoul2Model) is silently
	// skipped at draw time instead.
	//
	// customSkin here is NOT a VK_RegisterSkin/RE_RegisterSkin handle,
	// despite the parameter name suggesting it lines up with RE_RegisterSkin's
	// return value - real callers (g_client.cpp's G_SetG2PlayerModel) pass
	// G_SkinIndex(skinName) instead, a small networked *configstring* index
	// (position in CS_CHARSKINS, shared/renumbered across the whole game
	// session) in a completely different, unrelated numbering scheme.
	// Treating it as a VK_RegisterSkin handle (this renderer's first attempt)
	// silently applied a random *other* model's skin whenever the two
	// numbering schemes happened to collide - e.g. academy1's rosh_penin
	// picking up the protocol droid's textures on some surfaces. The real
	// renderer skin handle only shows up later, as G2API_SetSkin's
	// *renderSkin* (third) parameter - see that function below, which
	// `G_SetG2PlayerModel` always calls immediately after this one. So:
	// load with no skin (0) here, exactly like a model with no G2API_SetSkin
	// call ever made; custom shaders/LOD bias/modelFlags still aren't
	// implemented either (see README.md).
	(void)modelIndex; (void)customSkin; (void)customShader; (void)modelFlags; (void)lodBias;

	int slot = -1;
	for ( int i = 0; i < ghoul2.size(); i++ )
	{
		if ( ghoul2[i].mModelindex == -1 )
		{
			ghoul2[i] = CGhoul2Info();
			slot = i;
			break;
		}
	}
	if ( slot < 0 )
	{
		ghoul2.push_back( CGhoul2Info() );
		slot = ghoul2.size() - 1;
	}

	Q_strncpyz( ghoul2[slot].mFileName, fileName, sizeof( ghoul2[slot].mFileName ) );
	ghoul2[slot].mModelindex = slot;
	ghoul2[slot].mModel = (qhandle_t)VK_LoadGhoul2Model( fileName, 0 );
	return slot;
}
qboolean G2API_IsPaused( CGhoul2Info *ghlInfo, const char *boneName ) { return VK_IsGhoul2BoneAnimPaused( ghlInfo, VK_ResolveGhoul2AnimBone( ghlInfo, boneName ) ) ? qtrue : qfalse; }
void G2API_ListBones( CGhoul2Info *ghlInfo, int frame ) { (void)ghlInfo; (void)frame; }
void G2API_ListSurfaces( CGhoul2Info *ghlInfo ) { (void)ghlInfo; }
void G2API_LoadGhoul2Models( CGhoul2Info_v &ghoul2, char *buffer ) { (void)ghoul2; (void)buffer; }
void G2API_LoadSaveCodeDestructGhoul2Info( CGhoul2Info_v &ghoul2 ) { (void)ghoul2; }
qboolean G2API_PauseBoneAnim( CGhoul2Info *ghlInfo, const char *boneName, const int t ) { return VK_PauseGhoul2BoneAnim( ghlInfo, VK_ResolveGhoul2AnimBone( ghlInfo, boneName ), t ) ? qtrue : qfalse; }
qboolean G2API_PauseBoneAnimIndex( CGhoul2Info *ghlInfo, const int boneIndex, const int t ) { return VK_PauseGhoul2BoneAnim( ghlInfo, boneIndex, t ) ? qtrue : qfalse; }
// Real implementation now - this used to be a hardcoded `return 0;`
// (always "not found"), which was more consequential than a normal
// precache no-op: NPC_stats.cpp's G_ParseAnimFileSet (the only real
// caller) uses this to register a level's "cinematic" per-map animation
// GLA (e.g. "models/players/_humanoid/_humanoid_academy1.gla" - a
// map-specific set of extra animations used only by that map's scripted
// cutscenes) *in addition to* the standard "_humanoid.gla" every humanoid
// model already uses, and it gates loading that entire cinematic
// animation set behind this call's return value being truthy
// (`if (cineGLAIndex) { G_ParseAnimationFile(1, ...); ... }`) - a
// permanent `return 0;` silently meant no map's cinematic-only animations
// ever loaded, for any NPC, ever - not just "this specific .gla is
// missing," every map that ships one. Uses a dedicated handle cache
// (`VK_PrecacheGhoul2AnimHandle`'s own registry, tr_model.cpp), not
// `RE_RegisterModel`'s existing hardcoded-1 stub (see its own comment) or
// `VK_LoadGhoul2Skeleton`'s real per-skeleton cache (a different handle
// space, used for a different purpose - actually rendering a skeleton, not
// this ordinal "did it register, and did the second one land right after
// the first" check) -
// `G_ParseAnimFileSet` asserts a *second* precache call (the cinematic
// GLA) returns exactly the *first* call's handle (the standard GLA) plus
// one, so this needs handles assigned strictly in first-seen call order,
// with no other unrelated precache activity able to land in between and
// break that invariant. Returns 0 (falsy, matching real RE_RegisterModel
// semantics for "file doesn't exist") only when a never-before-seen
// filename genuinely can't be read - the common, expected case for any
// map that doesn't ship a cinematic-specific GLA at all, not an error.
//
// Now routes through VK_PrecacheGhoul2AnimHandle (tr_model.cpp), which
// actually loads the skeleton behind each handle (not just an existence
// check discarding the buffer), so `handle + offset`
// (G2API_SetAnimIndex/GetAnimIndex below) actually resolves to a real,
// already-loaded skeleton rather than just an opaque ordinal - see that
// function's own comment for why this handle space still needs to stay
// isolated from ordinary per-model skeleton loading (VK_LoadGhoul2Model
// never registers into it), not shared the way real vanilla's literal
// qhandle_t space is. See "Ghoul2 per-level animation-file overrides"
// (README.md) for the full story.
qhandle_t G2API_PrecacheGhoul2Model( const char *fileName )
{
	if ( !fileName || !fileName[0] )
	{
		return 0;
	}
	std::string name = fileName;
	if ( COM_CompareExtension( fileName, ".gla" ) && name.size() > 4 )
	{
		name.resize( name.size() - 4 );
	}
	return (qhandle_t)VK_PrecacheGhoul2AnimHandle( name.c_str() );
}
qboolean G2API_RagEffectorGoal( CGhoul2Info_v &ghoul2, const char *boneName, vec3_t pos ) { (void)ghoul2; (void)boneName; (void)pos; return qfalse; }
qboolean G2API_RagEffectorKick( CGhoul2Info_v &ghoul2, const char *boneName, vec3_t velocity ) { (void)ghoul2; (void)boneName; (void)velocity; return qfalse; }
qboolean G2API_RagForceSolve( CGhoul2Info_v &ghoul2, qboolean force ) { (void)ghoul2; (void)force; return qfalse; }
qboolean G2API_RagPCJConstraint( CGhoul2Info_v &ghoul2, const char *boneName, vec3_t min, vec3_t max ) { (void)ghoul2; (void)boneName; (void)min; (void)max; return qfalse; }
qboolean G2API_RagPCJGradientSpeed( CGhoul2Info_v &ghoul2, const char *boneName, const float speed ) { (void)ghoul2; (void)boneName; (void)speed; return qfalse; }
qboolean G2API_RemoveBolt( CGhoul2Info *ghlInfo, const int index ) { (void)ghlInfo; (void)index; return qfalse; }
qboolean G2API_RemoveBone( CGhoul2Info *ghlInfo, const char *boneName ) { (void)ghlInfo; (void)boneName; return qfalse; }
qboolean G2API_RemoveGhoul2Model( CGhoul2Info_v &ghlInfo, const int modelIndex ) { (void)ghlInfo; (void)modelIndex; return qfalse; }
qboolean G2API_RemoveSurface( CGhoul2Info *ghlInfo, const int index ) { (void)ghlInfo; (void)index; return qfalse; }
void G2API_SaveGhoul2Models( CGhoul2Info_v &ghoul2 ) { (void)ghoul2; }
// Real now - closes a real gap this file's own "not implemented" list
// carried as "every model always uses whichever single .gla
// VK_LoadGhoul2Skeleton first resolved for it." Confirmed real, exercised
// usage: bg_panimate.cpp calls this on *every* torso/legs animation change
// for every player/NPC in the game (`curAnim.glaIndex`, almost always 0 -
// the base .gla - but 1 whenever that animation came from a per-level
// "cinematic" animation-file override, per G_ParseAnimFileSet's real
// mechanism, NPC_stats.cpp - see VK_ResolveGhoul2SkeletonIndex's own
// comment, tr_model.cpp, for the full story and why a matching
// "_humanoid_<mapname>.gla" ships for every one of this checkout's 4 fixed
// test maps). Just stores the offset - VK_ResolveGhoul2SkeletonIndex (used
// by every place that computes a live pose) is what actually acts on it.
// Real rd-vanilla (G2_API.cpp) also clears every bone slot's
// BONE_ANIM_BLEND-family flags when the index actually changes, so a
// blend-in-progress on some *other* bone doesn't carry a stale flag across
// the switch; not replicated here (this renderer's simpler per-instance
// animation state - VulkanGhoul2AnimState - doesn't track that flag the
// same way, and the very next call on the same bone is always a fresh
// SetBoneAnimIndex anyway per bg_panimate.cpp's own call pattern above) -
// a known, minor, honestly-documented gap, not silently dropped.
qboolean G2API_SetAnimIndex( CGhoul2Info *ghlInfo, const int index )
{
	if ( !ghlInfo )
	{
		return qfalse;
	}
	ghlInfo->animModelIndexOffset = index;
	return qtrue;
}
// boneName/index, setFrame, and blendTime are ignored - see
// VK_SetGhoul2BoneAnim's comment (tr_model.cpp): a single whole-skeleton
// track per instance (not per-bone-subtree), no blending between two
// animations, no arbitrary mid-anim setFrame seek (always starts from
// startFrame at time t).
// setFrame/blendTime are real now (previously discarded) - see
// VK_SetGhoul2BoneAnim's comment (tr_model.cpp) for exactly what they do.
// bg_panimate.cpp passes both on every PM_SetAnimFinal call (setFrame to
// keep a re-affirmed animation from visibly restarting, blendTime -
// 350ms by default - to cross-fade into a genuinely new one) so this
// mattered for ordinary gameplay, not just an edge case.
qboolean G2API_SetBoneAnim( CGhoul2Info *ghlInfo, const char *boneName, const int startFrame, const int endFrame, const int flags, const float animSpeed, const int t, const float setFrame, const int blendTime ) { VK_SetGhoul2BoneAnim( ghlInfo, VK_ResolveGhoul2AnimBone( ghlInfo, boneName ), startFrame, endFrame, flags, animSpeed, t, setFrame, blendTime ); return qtrue; }
qboolean G2API_SetBoneAnimIndex( CGhoul2Info *ghlInfo, const int index, const int startFrame, const int endFrame, const int flags, const float animSpeed, const int t, const float setFrame, const int blendTime ) { VK_SetGhoul2BoneAnim( ghlInfo, index, startFrame, endFrame, flags, animSpeed, t, setFrame, blendTime ); return qtrue; }
// Real now (BONE_ANGLES_POSTMULT only - see VK_SetGhoul2BoneAngles's own
// comment, tr_model.cpp, for the real formula ported and why that's the
// only flag combination implemented). By-name resolution reuses
// VK_ResolveGhoul2AnimBone (this file, above) - the exact same bone-name-
// to-real-skeleton-index lookup G2API_SetBoneAnim's By-name variant already
// uses, since both ultimately key into the same per-instance,
// per-bone-index space (ghlInfo, boneIndex). blendTime is ignored - see
// VK_SetGhoul2BoneAngles's own comment for why (no real call site in this
// game ever passes a nonzero one for an angle override, unlike
// SetBoneAnim's blendTime which real code relies on constantly).
qboolean G2API_SetBoneAngles( CGhoul2Info *ghlInfo, const char *boneName, const vec3_t angles, const int flags, const Eorientations up, const Eorientations left, const Eorientations forward, qhandle_t *modelList, int blendTime, int t ) { (void)modelList; (void)blendTime; (void)t; return VK_SetGhoul2BoneAngles( ghlInfo, VK_ResolveGhoul2AnimBone( ghlInfo, boneName ), angles, flags, up, left, forward ) ? qtrue : qfalse; }
qboolean G2API_SetBoneAnglesIndex( CGhoul2Info *ghlInfo, const int index, const vec3_t angles, const int flags, const Eorientations yaw, const Eorientations pitch, const Eorientations roll, qhandle_t *modelList, int blendTime, int t ) { (void)modelList; (void)blendTime; (void)t; return VK_SetGhoul2BoneAngles( ghlInfo, index, angles, flags, yaw, pitch, roll ) ? qtrue : qfalse; }
qboolean G2API_SetBoneAnglesMatrix( CGhoul2Info *ghlInfo, const char *boneName, const mdxaBone_t &matrix, const int flags, qhandle_t *modelList, int blendTime, int t ) { (void)ghlInfo; (void)boneName; (void)matrix; (void)flags; (void)modelList; (void)blendTime; (void)t; return qfalse; }
qboolean G2API_SetBoneAnglesMatrixIndex( CGhoul2Info *ghlInfo, const int index, const mdxaBone_t &matrix, const int flags, qhandle_t *modelList, int blendTime, int t ) { (void)ghlInfo; (void)index; (void)matrix; (void)flags; (void)modelList; (void)blendTime; (void)t; return qfalse; }
qboolean G2API_SetBoneIKState( CGhoul2Info_v &ghoul2, int t, const char *boneName, int ikState, sharedSetBoneIKStateParams_t *params ) { (void)ghoul2; (void)t; (void)boneName; (void)ikState; (void)params; return qfalse; }
qboolean G2API_SetGhoul2ModelFlags( CGhoul2Info *ghlInfo, const int flags ) { (void)ghlInfo; (void)flags; return qfalse; }
void G2API_SetGhoul2ModelIndexes( CGhoul2Info_v &ghoul2, qhandle_t *modelList, qhandle_t *skinList ) { (void)ghoul2; (void)modelList; (void)skinList; }
qboolean G2API_SetLodBias( CGhoul2Info *ghlInfo, int lodBias ) { (void)ghlInfo; (void)lodBias; return qfalse; }
qboolean G2API_SetNewOrigin( CGhoul2Info *ghlInfo, const int boltIndex ) { (void)ghlInfo; (void)boltIndex; return qfalse; }
void G2API_SetRagDoll( CGhoul2Info_v &ghoul2, CRagDollParams *parms ) { (void)ghoul2; (void)parms; }
qboolean G2API_SetRootSurface( CGhoul2Info_v &ghlInfo, const int modelIndex, const char *surfaceName ) { (void)ghlInfo; (void)modelIndex; (void)surfaceName; return qfalse; }
qboolean G2API_SetShader( CGhoul2Info *ghlInfo, qhandle_t customShader ) { (void)ghlInfo; (void)customShader; return qfalse; }
// customSkin is the same networked configstring index G2API_InitGhoul2Model's
// same-named parameter is (see that function's comment) - NOT usable as a
// VK_RegisterSkin handle. renderSkin is the real one: g_client.cpp's
// G_SetG2PlayerModel gets it from `gi.RE_RegisterSkin(skinName)` directly
// (assigned to a local it calls `skin`) and passes *that* here, immediately
// after G2API_InitGhoul2Model - this is where a humanoid model actually
// picks up its real per-surface textures, not at Init time.
qboolean G2API_SetSkin( CGhoul2Info *ghlInfo, qhandle_t customSkin, qhandle_t renderSkin )
{
	(void)customSkin;
	if ( !ghlInfo || !ghlInfo->mFileName[0] )
	{
		return qfalse;
	}
	ghlInfo->mCustomSkin = renderSkin;
	ghlInfo->mModel = (qhandle_t)VK_LoadGhoul2Model( ghlInfo->mFileName, (int)renderSkin );
	return qtrue;
}
// Real per-instance surface visibility toggling - ported from rd-vanilla's
// real G2_SetSurfaceOnOff (G2_surfaces.cpp), adapted to this renderer's own
// model-cache lookup (VK_FindGhoul2SurfaceIndex, tr_model.cpp) instead of
// rd-vanilla's currentModel->mdxm pointer (never populated by this
// renderer's CGhoul2Info instances - see VK_LoadGhoul2Model's own comment
// on why this renderer tracks models by its own cache index, mModel,
// rather than a real model_t). Same real semantics as the original:
// ghlInfo->mSlist (a shared, cross-renderer field - CGhoul2Info::mSlist,
// ghoul2_shared.h) holds a sparse list of per-instance overrides, one entry
// per surface the game has ever explicitly toggled away from its .glm's own
// baked default; only the G2SURFACEFLAG_OFF/NODESCENDANTS bits of an
// incoming `flags` are ever applied (matching the real comment - "the only
// bit we really care about... is the off bit"), and a new entry is only
// pushed if it'd actually change anything, not for every call. Confirmed
// real, exercised data behind this, not just real API shape: hoth2's
// `protocol_imp` NPC (`ext_data/npcs/protocol_imp.npc`) declares `surfOff
// head` + `surfOn head_off`, applied once at spawn via
// G_SetG2PlayerModelInfo (g_client.cpp) - previously a silent no-op here,
// meaning that droid always showed its default "head" surface and never
// its "head_off" variant regardless of skin.
//
// NODESCENDANTS' real recursive "also hide every child surface in the
// hierarchy" behaviour (G2_FindRecursiveSurface) is NOT ported - the bit is
// still masked/stored faithfully in mSlist for forward-compatibility, but
// VK_DrawGhoul2Entities' own on/off check (this file's other half of this
// feature, tr_model.cpp) only ever looks at G2SURFACEFLAG_OFF on the exact
// surface named, same scope this renderer's existing *load-time* baked-
// flags check already had before this change.
qboolean G2API_SetSurfaceOnOff( CGhoul2Info *ghlInfo, const char *surfaceName, const int flags )
{
	if ( !ghlInfo || !surfaceName )
	{
		return qfalse;
	}

	static const int kMask = G2SURFACEFLAG_OFF | G2SURFACEFLAG_NODESCENDANTS;

	unsigned int baseFlagsRaw = 0;
	int surfIndex = VK_FindGhoul2SurfaceIndex( ghlInfo->mModel, surfaceName, &baseFlagsRaw );
	if ( surfIndex < 0 )
	{
		return qfalse;
	}
	int baseFlags = (int)baseFlagsRaw;

	for ( surfaceInfo_t &entry : ghlInfo->mSlist )
	{
		if ( entry.surface == surfIndex )
		{
			entry.offFlags &= ~kMask;
			entry.offFlags |= flags & kMask;
			return qtrue;
		}
	}

	int newFlags = ( baseFlags & ~kMask ) | ( flags & kMask );
	if ( newFlags != baseFlags )
	{
		surfaceInfo_t entry;
		entry.surface = surfIndex;
		entry.offFlags = newFlags;
		ghlInfo->mSlist.push_back( entry );
	}
	return qtrue;
}
void G2API_SetTime( int currentTime, int clock ) { (void)currentTime; (void)clock; }
qboolean G2API_StopBoneAnim( CGhoul2Info *ghlInfo, const char *boneName ) { return VK_StopGhoul2BoneAnim( ghlInfo, VK_ResolveGhoul2AnimBone( ghlInfo, boneName ) ) ? qtrue : qfalse; }
qboolean G2API_StopBoneAnimIndex( CGhoul2Info *ghlInfo, const int index ) { return VK_StopGhoul2BoneAnim( ghlInfo, index ) ? qtrue : qfalse; }
qboolean G2API_StopBoneAngles( CGhoul2Info *ghlInfo, const char *boneName ) { (void)ghlInfo; (void)boneName; return qfalse; }
qboolean G2API_StopBoneAnglesIndex( CGhoul2Info *ghlInfo, const int index ) { (void)ghlInfo; (void)index; return qfalse; }

typedef struct
{
	const char *cmd;
	xcommand_t func;
} consoleCommand_t;

static consoleCommand_t commands[] = {
	{ "screenshot_png", R_ScreenShotPNG_f },
};
static const size_t numCommands = ARRAY_LEN( commands );

extern "C" Q_EXPORT refexport_t *QDECL GetRefAPI( int apiVersion, refimport_t *refimp )
{
	static refexport_t re;

	ri = *refimp;
	memset( &re, 0, sizeof( re ) );

	if ( apiVersion != REF_API_VERSION )
	{
		ri.Printf( PRINT_ALL, "rd-vulkan: Mismatched REF_API_VERSION: expected %i, got %i\n", REF_API_VERSION, apiVersion );
		return NULL;
	}

	R_Init();

	for ( size_t i = 0; i < numCommands; i++ )
	{
		ri.Cmd_AddCommand( commands[i].cmd, commands[i].func );
	}

	re.Shutdown = RE_Shutdown;
	re.BeginRegistration = RE_BeginRegistration;
	re.RegisterModel = RE_RegisterModel;
	re.RegisterSkin = RE_RegisterSkin;
	re.GetAnimationCFG = RE_GetAnimationCFG;
	re.RegisterShader = RE_RegisterShader;
	re.RegisterShaderNoMip = RE_RegisterShaderNoMip;
	re.LoadWorld = RE_LoadWorldMap;
	re.R_LoadImage = R_LoadImage;
	re.RegisterMedia_LevelLoadBegin = RE_RegisterMedia_LevelLoadBegin;
	re.RegisterMedia_LevelLoadEnd = RE_RegisterMedia_LevelLoadEnd;
	re.RegisterMedia_GetLevel = RE_RegisterMedia_GetLevel;
	re.RegisterModels_LevelLoadEnd = RE_RegisterModels_LevelLoadEnd;
	re.RegisterImages_LevelLoadEnd = RE_RegisterImages_LevelLoadEnd;
	re.SetWorldVisData = RE_SetWorldVisData;
	re.EndRegistration = RE_EndRegistration;
	re.ClearScene = RE_ClearScene;
	re.AddRefEntityToScene = RE_AddRefEntityToScene;
	re.AddPolyToScene = RE_AddPolyToScene;
	re.AddLightToScene = RE_AddLightToScene;
	re.RenderScene = RE_RenderScene;
	re.GetLighting = RE_GetLighting;
	re.SetColor = RE_SetColor;
	re.DrawStretchPic = RE_StretchPic;
	re.DrawRotatePic = RE_DrawRotatePic;
	re.DrawRotatePic2 = RE_DrawRotatePic2;
	re.LAGoggles = RE_LAGoggles;
	re.Scissor = RE_Scissor;
	re.DrawStretchRaw = RE_DrawStretchRaw;
	re.UploadCinematic = RE_UploadCinematic;
	re.BeginFrame = RE_BeginFrame;
	re.EndFrame = RE_EndFrame;
	re.ProcessDissolve = RE_ProcessDissolve;
	re.InitDissolve = RE_InitDissolve;
	re.GetScreenShot = RE_GetScreenShot;
	re.TempRawImage_ReadFromFile = RE_TempRawImage_ReadFromFile;
	re.TempRawImage_CleanUp = RE_TempRawImage_CleanUp;
	re.MarkFragments = R_MarkFragments;
	re.LerpTag = R_LerpTag;
	re.ModelBounds = R_ModelBounds;
	re.GetLightStyle = RE_GetLightStyle;
	re.SetLightStyle = RE_SetLightStyle;
	re.GetBModelVerts = RE_GetBModelVerts;
	re.WorldEffectCommand = R_WorldEffectCommand;
	re.GetModelBounds = RE_GetModelBounds;
	re.SVModelInit = RE_SVModelInit;

	re.RegisterFont = RE_RegisterFont;
	re.Font_HeightPixels = RE_Font_HeightPixels;
	re.Font_StrLenPixels = RE_Font_StrLenPixels;
	re.Font_DrawString = RE_Font_DrawString;
	re.Font_StrLenChars = RE_Font_StrLenChars;
	re.Language_IsAsian = Language_IsAsian;
	re.Language_UsesSpaces = Language_UsesSpaces;
	re.AnyLanguage_ReadCharFromString = AnyLanguage_ReadCharFromString;

	re.R_InitWorldEffects = R_InitWorldEffects;
	re.R_ClearStuffToStopGhoul2CrashingThings = R_ClearStuffToStopGhoul2CrashingThings;
	re.R_inPVS = R_inPVS;

	re.tr_distortionAlpha = get_tr_distortionAlpha;
	re.tr_distortionStretch = get_tr_distortionStretch;
	re.tr_distortionPrePost = get_tr_distortionPrePost;
	re.tr_distortionNegate = get_tr_distortionNegate;

	re.GetWindVector = R_GetWindVector;
	re.GetWindGusting = R_GetWindGusting;
	re.IsOutside = R_IsOutside;
	re.IsOutsideCausingPain = R_IsOutsideCausingPain;
	re.GetChanceOfSaberFizz = R_GetChanceOfSaberFizz;
	re.IsShaking = R_IsShaking;
	re.AddWeatherZone = R_AddWeatherZone;
	re.SetTempGlobalFogColor = R_SetTempGlobalFogColor;
	re.SetRangedFog = RE_SetRangedFog;

	re.TheGhoul2InfoArray = TheGhoul2InfoArray;

#define G2EX(x)	re.G2API_##x = G2API_##x

	G2EX(AddBolt);
	G2EX(AddBoltSurfNum);
	G2EX(AddSurface);
	G2EX(AnimateG2Models);
	G2EX(AttachEnt);
	G2EX(AttachG2Model);
	G2EX(CollisionDetect);
	G2EX(CleanGhoul2Models);
	G2EX(CopyGhoul2Instance);
	G2EX(DetachEnt);
	G2EX(DetachG2Model);
	G2EX(GetAnimFileName);
	G2EX(GetAnimFileNameIndex);
	G2EX(GetAnimFileInternalNameIndex);
	G2EX(GetAnimIndex);
	G2EX(GetAnimRange);
	G2EX(GetAnimRangeIndex);
	G2EX(GetBoneAnim);
	G2EX(GetBoneAnimIndex);
	G2EX(GetBoneIndex);
	G2EX(GetBoltMatrix);
	G2EX(GetGhoul2ModelFlags);
	G2EX(GetGLAName);
	G2EX(GetParentSurface);
	G2EX(GetRagBonePos);
	G2EX(GetSurfaceIndex);
	G2EX(GetSurfaceName);
	G2EX(GetSurfaceRenderStatus);
	G2EX(GetTime);
	G2EX(GiveMeVectorFromMatrix);
	G2EX(HaveWeGhoul2Models);
	G2EX(IKMove);
	G2EX(InitGhoul2Model);
	G2EX(IsPaused);
	G2EX(ListBones);
	G2EX(ListSurfaces);
	G2EX(LoadGhoul2Models);
	G2EX(LoadSaveCodeDestructGhoul2Info);
	G2EX(PauseBoneAnim);
	G2EX(PauseBoneAnimIndex);
	G2EX(PrecacheGhoul2Model);
	G2EX(RagEffectorGoal);
	G2EX(RagEffectorKick);
	G2EX(RagForceSolve);
	G2EX(RagPCJConstraint);
	G2EX(RagPCJGradientSpeed);
	G2EX(RemoveBolt);
	G2EX(RemoveBone);
	G2EX(RemoveGhoul2Model);
	G2EX(RemoveSurface);
	G2EX(SaveGhoul2Models);
	G2EX(SetAnimIndex);
	G2EX(SetBoneAnim);
	G2EX(SetBoneAnimIndex);
	G2EX(SetBoneAngles);
	G2EX(SetBoneAnglesIndex);
	G2EX(SetBoneAnglesMatrix);
	G2EX(SetBoneIKState);
	G2EX(SetGhoul2ModelFlags);
	G2EX(SetGhoul2ModelIndexes);
	G2EX(SetLodBias);
	G2EX(SetNewOrigin);
	G2EX(SetRagDoll);
	G2EX(SetRootSurface);
	G2EX(SetShader);
	G2EX(SetSkin);
	G2EX(SetSurfaceOnOff);
	G2EX(SetTime);
	G2EX(StopBoneAnim);
	G2EX(StopBoneAnimIndex);
	G2EX(StopBoneAngles);
	G2EX(StopBoneAnglesIndex);

#undef G2EX

	return &re;
}
