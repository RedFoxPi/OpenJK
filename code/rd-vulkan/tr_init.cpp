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

static void VK_Check( VkResult r, const char *what )
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

	VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;

	// Two dependencies: one governing entry into the subpass (a fresh
	// swapchain image may still be in use by a previous present), and -
	// critically - one governing exit, since RE_EndFrame's readback copy
	// (VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL read) and the present transition
	// both happen right after vkCmdEndRenderPass in the same command buffer;
	// without an explicit EXTERNAL dependency here, nothing guarantees the
	// color attachment writes are visible to that read (Vulkan does not
	// order-of-submission-implies-visibility the way command order suggests).
	VkSubpassDependency dependencies[2] = {};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = 0;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	ci.attachmentCount = 1;
	ci.pAttachments = &colorAttachment;
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
		VkImageView attachments[] = { vk.swapchainImageViews[i] };
		VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		ci.renderPass = vk.renderPass;
		ci.attachmentCount = 1;
		ci.pAttachments = attachments;
		ci.width = vk.swapchainExtent.width;
		ci.height = vk.swapchainExtent.height;
		ci.layers = 1;
		VK_Check( vkCreateFramebuffer( vk.device, &ci, nullptr, &vk.swapchainFramebuffers[i] ), "vkCreateFramebuffer" );
	}
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

	VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments = &blendAttachment;

	VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynState.dynamicStateCount = 2;
	dynState.pDynamicStates = dynStates;

	VkGraphicsPipelineCreateInfo pipeInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipeInfo.stageCount = 2;
	pipeInfo.pStages = stages;
	pipeInfo.pVertexInputState = &vertexInput;
	pipeInfo.pInputAssemblyState = &inputAssembly;
	pipeInfo.pViewportState = &viewportState;
	pipeInfo.pRasterizationState = &raster;
	pipeInfo.pMultisampleState = &multisample;
	pipeInfo.pColorBlendState = &colorBlend;
	pipeInfo.pDynamicState = &dynState;
	pipeInfo.layout = vk.uiPipelineLayout;
	pipeInfo.renderPass = vk.renderPass;
	pipeInfo.subpass = 0;

	VK_Check( vkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &vk.uiPipeline ), "vkCreateGraphicsPipelines" );

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
	VK_CreateRenderPass();
	VK_CreateFramebuffers();
	VK_CreateCommandPoolsAndSync();
	VK_CreateUiPipeline();

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

	R_ImageLoader_Init();
	R_InitFonts();

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
	if ( vk.device )
	{
		vkDeviceWaitIdle( vk.device );
	}

	VK_ShutdownImages();
	VK_DestroyReadbackImage();

	if ( vk.uiVertexBufferMemory ) vkUnmapMemory( vk.device, vk.uiVertexBufferMemory );
	if ( vk.uiVertexBuffer ) vkDestroyBuffer( vk.device, vk.uiVertexBuffer, nullptr );
	if ( vk.uiVertexBufferMemory ) vkFreeMemory( vk.device, vk.uiVertexBufferMemory, nullptr );
	if ( vk.uiSampler ) vkDestroySampler( vk.device, vk.uiSampler, nullptr );
	if ( vk.uiPipeline ) vkDestroyPipeline( vk.device, vk.uiPipeline, nullptr );
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

// Everything below this point is 3D world/model rendering, which this first
// pass of rd-vulkan does not implement yet - see README.md. These are
// deliberately safe no-ops rather than left NULL, so the plugin doesn't
// crash when e.g. a map is loaded; they just won't draw anything.

qhandle_t RE_RegisterModel( const char *name ) { (void)name; return 0; }
qhandle_t RE_RegisterSkin( const char *name ) { (void)name; return 0; }
int RE_GetAnimationCFG( const char *psCFGFilename, char *psDest, int iDestSize ) { (void)psCFGFilename; if (psDest && iDestSize) psDest[0] = 0; return 0; }
void RE_LoadWorldMap( const char *name ) { (void)name; }
void RE_RegisterMedia_LevelLoadBegin( const char *psMapName, ForceReload_e eForceReload, qboolean bAllowScreenDissolve ) { (void)psMapName; (void)eForceReload; (void)bAllowScreenDissolve; }
void RE_RegisterMedia_LevelLoadEnd( void ) {}
int RE_RegisterMedia_GetLevel( void ) { return 0; }
qboolean RE_RegisterModels_LevelLoadEnd( qboolean bDeleteEverythingNotUsedThisLevel ) { (void)bDeleteEverythingNotUsedThisLevel; return qfalse; }
qboolean RE_RegisterImages_LevelLoadEnd( void ) { return qfalse; }
void RE_SetWorldVisData( const byte *vis ) { (void)vis; }
void RE_EndRegistration( void ) {}
void RE_ClearScene( void ) {}
void RE_AddRefEntityToScene( const refEntity_t *re ) { (void)re; }
void RE_AddPolyToScene( qhandle_t hShader, int numVerts, const polyVert_t *verts ) { (void)hShader; (void)numVerts; (void)verts; }
void RE_AddLightToScene( const vec3_t org, float intensity, float r, float g, float b ) { (void)org; (void)intensity; (void)r; (void)g; (void)b; }
void RE_RenderScene( const refdef_t *fd ) { (void)fd; }
qboolean RE_GetLighting( const vec3_t org, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir )
{
	(void)org;
	VectorSet( ambientLight, 1.f, 1.f, 1.f );
	VectorSet( directedLight, 0.f, 0.f, 0.f );
	VectorSet( lightDir, 0.f, 0.f, 1.f );
	return qfalse;
}
void RE_DrawRotatePic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, float a1, qhandle_t hShader )
{
	// not implemented yet - falls back to an unrotated stretch pic so callers still see *something*
	(void)a1;
	RE_StretchPic( x, y, w, h, s1, t1, s2, t2, hShader );
}
void RE_DrawRotatePic2( float x, float y, float w, float h, float s1, float t1, float s2, float t2, float a1, qhandle_t hShader )
{
	(void)a1;
	RE_StretchPic( x, y, w, h, s1, t1, s2, t2, hShader );
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
void R_WorldEffectCommand( const char *command ) { (void)command; }
void RE_GetModelBounds( refEntity_t *refEnt, vec3_t bounds1, vec3_t bounds2 ) { (void)refEnt; VectorClear( bounds1 ); VectorClear( bounds2 ); }
void RE_SVModelInit( void ) {}
void R_InitWorldEffects( void ) {}
void R_ClearStuffToStopGhoul2CrashingThings( void ) {}
qboolean R_inPVS( vec3_t p1, vec3_t p2 ) { (void)p1; (void)p2; return qtrue; }
static float s_zero = 0.f;
static qboolean s_qfalse = qfalse;
float *get_tr_distortionAlpha( void ) { return &s_zero; }
float *get_tr_distortionStretch( void ) { return &s_zero; }
qboolean *get_tr_distortionPrePost( void ) { return &s_qfalse; }
qboolean *get_tr_distortionNegate( void ) { return &s_qfalse; }
bool R_GetWindVector( vec3_t windVector, vec3_t atPoint ) { (void)atPoint; VectorClear( windVector ); return false; }
bool R_GetWindGusting( vec3_t atpoint ) { (void)atpoint; return false; }
bool R_IsOutside( vec3_t pos ) { (void)pos; return false; }
float R_IsOutsideCausingPain( vec3_t pos ) { (void)pos; return 0.f; }
float R_GetChanceOfSaberFizz( void ) { return 0.f; }
bool R_IsShaking( vec3_t pos ) { (void)pos; return false; }
void R_AddWeatherZone( vec3_t mins, vec3_t maxs ) { (void)mins; (void)maxs; }
bool R_SetTempGlobalFogColor( vec3_t color ) { (void)color; return false; }
void RE_SetRangedFog( float dist ) { (void)dist; }

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
	int New() override
	{
		for ( size_t i = 0; i < mValid.size(); i++ )
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

int G2API_AddBolt( CGhoul2Info *ghlInfo, const char *boneName ) { (void)ghlInfo; (void)boneName; return -1; }
int G2API_AddBoltSurfNum( CGhoul2Info *ghlInfo, const int surfIndex ) { (void)ghlInfo; (void)surfIndex; return -1; }
int G2API_AddSurface( CGhoul2Info *ghlInfo, int surfaceNumber, int polyNumber, float bi, float bj, int lod ) { (void)ghlInfo; (void)surfaceNumber; (void)polyNumber; (void)bi; (void)bj; (void)lod; return -1; }
void G2API_AnimateG2Models( CGhoul2Info_v &ghoul2, int t, CRagDollUpdateParams *p ) { (void)ghoul2; (void)t; (void)p; }
qboolean G2API_AttachEnt( int *boltInfo, CGhoul2Info *ghlInfoTo, int toBoltIndex, int entNum, int toModelNum ) { (void)boltInfo; (void)ghlInfoTo; (void)toBoltIndex; (void)entNum; (void)toModelNum; return qfalse; }
qboolean G2API_AttachG2Model( CGhoul2Info *ghlInfo, CGhoul2Info *ghlInfoTo, int toBoltIndex, int toModel ) { (void)ghlInfo; (void)ghlInfoTo; (void)toBoltIndex; (void)toModel; return qfalse; }
void G2API_CollisionDetect( CCollisionRecord *collRecMap, CGhoul2Info_v &ghoul2, const vec3_t angles, const vec3_t position, int frameNumber, int entNum, vec3_t rayStart, vec3_t rayEnd, vec3_t scale, CMiniHeap *heap, EG2_Collision traceType, int useLod, float fRadius )
{
	(void)collRecMap; (void)ghoul2; (void)angles; (void)position; (void)frameNumber; (void)entNum; (void)rayStart; (void)rayEnd; (void)scale; (void)heap; (void)traceType; (void)useLod; (void)fRadius;
}
void G2API_CleanGhoul2Models( CGhoul2Info_v &ghoul2 ) { ghoul2.clear(); }
void G2API_CopyGhoul2Instance( CGhoul2Info_v &from, CGhoul2Info_v &to, int modelIndex ) { (void)from; (void)to; (void)modelIndex; }
void G2API_DetachEnt( int *boltInfo ) { (void)boltInfo; }
qboolean G2API_DetachG2Model( CGhoul2Info *ghlInfo ) { (void)ghlInfo; return qfalse; }
qboolean G2API_GetAnimFileName( CGhoul2Info *ghlInfo, char **filename ) { (void)ghlInfo; if ( filename ) *filename = nullptr; return qfalse; }
char *G2API_GetAnimFileNameIndex( qhandle_t modelIndex ) { (void)modelIndex; return nullptr; }
char *G2API_GetAnimFileInternalNameIndex( qhandle_t modelIndex ) { (void)modelIndex; return nullptr; }
int G2API_GetAnimIndex( CGhoul2Info *ghlInfo ) { (void)ghlInfo; return 0; }
qboolean G2API_GetAnimRange( CGhoul2Info *ghlInfo, const char *boneName, int *startFrame, int *endFrame ) { (void)ghlInfo; (void)boneName; if (startFrame) *startFrame = 0; if (endFrame) *endFrame = 0; return qfalse; }
qboolean G2API_GetAnimRangeIndex( CGhoul2Info *ghlInfo, const int boneIndex, int *startFrame, int *endFrame ) { (void)ghlInfo; (void)boneIndex; if (startFrame) *startFrame = 0; if (endFrame) *endFrame = 0; return qfalse; }
qboolean G2API_GetBoneAnim( CGhoul2Info *ghlInfo, const char *boneName, const int t, float *currentFrame, int *startFrame, int *endFrame, int *flags, float *animSpeed, int *modelList ) { (void)ghlInfo; (void)boneName; (void)t; (void)modelList; if (currentFrame) *currentFrame = 0; if (startFrame) *startFrame = 0; if (endFrame) *endFrame = 0; if (flags) *flags = 0; if (animSpeed) *animSpeed = 0; return qfalse; }
qboolean G2API_GetBoneAnimIndex( CGhoul2Info *ghlInfo, const int boneIndex, const int t, float *currentFrame, int *startFrame, int *endFrame, int *flags, float *animSpeed, int *modelList ) { (void)ghlInfo; (void)boneIndex; (void)t; (void)modelList; if (currentFrame) *currentFrame = 0; if (startFrame) *startFrame = 0; if (endFrame) *endFrame = 0; if (flags) *flags = 0; if (animSpeed) *animSpeed = 0; return qfalse; }
int G2API_GetBoneIndex( CGhoul2Info *ghlInfo, const char *boneName, qboolean bAddIfNotFound ) { (void)ghlInfo; (void)boneName; (void)bAddIfNotFound; return -1; }
qboolean G2API_GetBoltMatrix( CGhoul2Info_v &ghoul2, const int modelIndex, const int boltIndex, mdxaBone_t *matrix, const vec3_t angles, const vec3_t position, const int frameNum, qhandle_t *modelList, const vec3_t scale )
{
	(void)ghoul2; (void)modelIndex; (void)boltIndex; (void)angles; (void)position; (void)frameNum; (void)modelList; (void)scale;
	if ( matrix ) memset( matrix, 0, sizeof( *matrix ) );
	return qfalse;
}
int G2API_GetGhoul2ModelFlags( CGhoul2Info *ghlInfo ) { (void)ghlInfo; return 0; }
char *G2API_GetGLAName( CGhoul2Info *ghlInfo ) { (void)ghlInfo; return nullptr; }
int G2API_GetParentSurface( CGhoul2Info *ghlInfo, const int index ) { (void)ghlInfo; (void)index; return -1; }
qboolean G2API_GetRagBonePos( CGhoul2Info_v &ghoul2, const char *boneName, vec3_t pos, vec3_t entAngles, vec3_t entPos, vec3_t entScale ) { (void)ghoul2; (void)boneName; (void)entAngles; (void)entPos; (void)entScale; VectorClear( pos ); return qfalse; }
int G2API_GetSurfaceIndex( CGhoul2Info *ghlInfo, const char *surfaceName ) { (void)ghlInfo; (void)surfaceName; return -1; }
char *G2API_GetSurfaceName( CGhoul2Info *ghlInfo, int surfNumber ) { (void)ghlInfo; (void)surfNumber; return nullptr; }
int G2API_GetSurfaceRenderStatus( CGhoul2Info *ghlInfo, const char *surfaceName ) { (void)ghlInfo; (void)surfaceName; return -1; }
int G2API_GetTime( int argTime ) { return argTime; }
void G2API_GiveMeVectorFromMatrix( mdxaBone_t &boltMatrix, Eorientations flags, vec3_t &vec ) { (void)boltMatrix; (void)flags; VectorClear( vec ); }
qboolean G2API_HaveWeGhoul2Models( CGhoul2Info_v &ghoul2 ) { return ghoul2.size() ? qtrue : qfalse; }
qboolean G2API_IKMove( CGhoul2Info_v &ghoul2, int t, sharedIKMoveParams_t *params ) { (void)ghoul2; (void)t; (void)params; return qfalse; }
int G2API_InitGhoul2Model( CGhoul2Info_v &ghoul2, const char *fileName, int modelIndex, qhandle_t customSkin, qhandle_t customShader, int modelFlags, int lodBias )
{
	(void)fileName; (void)modelIndex; (void)customSkin; (void)customShader; (void)modelFlags; (void)lodBias;
	ghoul2.clear();
	return -1;
}
qboolean G2API_IsPaused( CGhoul2Info *ghlInfo, const char *boneName ) { (void)ghlInfo; (void)boneName; return qfalse; }
void G2API_ListBones( CGhoul2Info *ghlInfo, int frame ) { (void)ghlInfo; (void)frame; }
void G2API_ListSurfaces( CGhoul2Info *ghlInfo ) { (void)ghlInfo; }
void G2API_LoadGhoul2Models( CGhoul2Info_v &ghoul2, char *buffer ) { (void)ghoul2; (void)buffer; }
void G2API_LoadSaveCodeDestructGhoul2Info( CGhoul2Info_v &ghoul2 ) { (void)ghoul2; }
qboolean G2API_PauseBoneAnim( CGhoul2Info *ghlInfo, const char *boneName, const int t ) { (void)ghlInfo; (void)boneName; (void)t; return qfalse; }
qboolean G2API_PauseBoneAnimIndex( CGhoul2Info *ghlInfo, const int boneIndex, const int t ) { (void)ghlInfo; (void)boneIndex; (void)t; return qfalse; }
qhandle_t G2API_PrecacheGhoul2Model( const char *fileName ) { (void)fileName; return 0; }
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
qboolean G2API_SetAnimIndex( CGhoul2Info *ghlInfo, const int index ) { (void)ghlInfo; (void)index; return qfalse; }
qboolean G2API_SetBoneAnim( CGhoul2Info *ghlInfo, const char *boneName, const int startFrame, const int endFrame, const int flags, const float animSpeed, const int t, const float setFrame, const int blendTime ) { (void)ghlInfo; (void)boneName; (void)startFrame; (void)endFrame; (void)flags; (void)animSpeed; (void)t; (void)setFrame; (void)blendTime; return qfalse; }
qboolean G2API_SetBoneAnimIndex( CGhoul2Info *ghlInfo, const int index, const int startFrame, const int endFrame, const int flags, const float animSpeed, const int t, const float setFrame, const int blendTime ) { (void)ghlInfo; (void)index; (void)startFrame; (void)endFrame; (void)flags; (void)animSpeed; (void)t; (void)setFrame; (void)blendTime; return qfalse; }
qboolean G2API_SetBoneAngles( CGhoul2Info *ghlInfo, const char *boneName, const vec3_t angles, const int flags, const Eorientations up, const Eorientations left, const Eorientations forward, qhandle_t *modelList, int blendTime, int t ) { (void)ghlInfo; (void)boneName; (void)angles; (void)flags; (void)up; (void)left; (void)forward; (void)modelList; (void)blendTime; (void)t; return qfalse; }
qboolean G2API_SetBoneAnglesIndex( CGhoul2Info *ghlInfo, const int index, const vec3_t angles, const int flags, const Eorientations yaw, const Eorientations pitch, const Eorientations roll, qhandle_t *modelList, int blendTime, int t ) { (void)ghlInfo; (void)index; (void)angles; (void)flags; (void)yaw; (void)pitch; (void)roll; (void)modelList; (void)blendTime; (void)t; return qfalse; }
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
qboolean G2API_SetSkin( CGhoul2Info *ghlInfo, qhandle_t customSkin, qhandle_t renderSkin ) { (void)ghlInfo; (void)customSkin; (void)renderSkin; return qfalse; }
qboolean G2API_SetSurfaceOnOff( CGhoul2Info *ghlInfo, const char *surfaceName, const int flags ) { (void)ghlInfo; (void)surfaceName; (void)flags; return qfalse; }
void G2API_SetTime( int currentTime, int clock ) { (void)currentTime; (void)clock; }
qboolean G2API_StopBoneAnim( CGhoul2Info *ghlInfo, const char *boneName ) { (void)ghlInfo; (void)boneName; return qfalse; }
qboolean G2API_StopBoneAnimIndex( CGhoul2Info *ghlInfo, const int index ) { (void)ghlInfo; (void)index; return qfalse; }
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
