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

#include "tr_local.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <vector>

// ============================================================================
// 2D draw path - the only thing this first pass of rd-vulkan actually draws.
// ============================================================================

struct UiVertex
{
	float pos[2];
	float uv[2];
};

static uint32_t s_uiVertexCursor = 0;

void VK_CopySwapchainImageToReadback( VkCommandBuffer cmd, VkImage swapImage );
void VK_DestroyReadbackImage( void );

void RE_SetColor( const float *rgba )
{
	if ( !rgba )
	{
		vk.drawColor[0] = vk.drawColor[1] = vk.drawColor[2] = vk.drawColor[3] = 1.0f;
	}
	else
	{
		memcpy( vk.drawColor, rgba, sizeof( vk.drawColor ) );
	}
}

// Shared quad submission for every 2D draw path (RE_StretchPic and, below,
// RE_DrawRotatePic/RE_DrawRotatePic2 - tr_init.cpp) - the only difference
// between them is how the 4 corner positions are computed; the pipeline
// selection/descriptor/push-constant/draw-call mechanics are identical.
// Corners are wound 0-1-2-3 (the same order RE_StretchPic always used) -
// harmless regardless of actual winding, since every 2D pipeline draws with
// VK_CULL_MODE_NONE (see VK_CreateUIPipeline's comment, tr_init.cpp).
void VK_DrawQuad( float x0, float y0, float u0, float v0,
	float x1, float y1, float u1, float v1,
	float x2, float y2, float u2, float v2,
	float x3, float y3, float u3, float v3,
	qhandle_t hShader )
{
	if ( !vk.frameActive )
	{
		return;
	}

	image_t *img = VK_GetImageByHandle( hShader );
	if ( !img )
	{
		return;
	}

	if ( s_uiVertexCursor + 6 > UI_VERTEX_BUFFER_CAPACITY * 6 )
	{
		// out of per-frame scratch space - drop the draw rather than corrupt the buffer
		return;
	}

	UiVertex *verts = (UiVertex *)vk.uiVertexBufferMapped + s_uiVertexCursor;
	verts[0] = { { x0, y0 }, { u0, v0 } };
	verts[1] = { { x1, y1 }, { u1, v1 } };
	verts[2] = { { x2, y2 }, { u2, v2 } };
	verts[3] = { { x0, y0 }, { u0, v0 } };
	verts[4] = { { x2, y2 }, { u2, v2 } };
	verts[5] = { { x3, y3 }, { u3, v3 } };

	VkCommandBuffer cmd = vk.activeCommandBuffer;

	VkPipeline pipeline = vk.uiPipeline;
	if ( img->blendMode == BLEND_ADDITIVE )
	{
		pipeline = vk.uiPipelineAdditive;
	}
	else if ( img->blendMode == BLEND_OPAQUE )
	{
		pipeline = vk.uiPipelineOpaque;
	}
	if ( pipeline != vk.lastBoundPipeline )
	{
		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
		vk.lastBoundPipeline = pipeline;
	}

	vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.uiPipelineLayout,
		0, 1, &img->descriptorSet, 0, nullptr );

	vkPushConstants_t pc;
	pc.viewportSize[0] = (float)vk.swapchainExtent.width;
	pc.viewportSize[1] = (float)vk.swapchainExtent.height;
	memcpy( pc.color, vk.drawColor, sizeof( pc.color ) );
	vkCmdPushConstants( cmd, vk.uiPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof( pc ), &pc );

	VkDeviceSize offset = s_uiVertexCursor * sizeof( UiVertex );
	vkCmdBindVertexBuffers( cmd, 0, 1, &vk.uiVertexBuffer, &offset );
	vkCmdDraw( cmd, 6, 1, 0, 0 );

	s_uiVertexCursor += 6;
}

void RE_StretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader )
{
	VK_DrawQuad( x, y, s1, t1, x + w, y, s2, t1, x + w, y + h, s2, t2, x, y + h, s1, t2, hShader );
}

void RE_BeginFrame( stereoFrame_t stereoFrame )
{
	(void)stereoFrame;

	vkFrame_t &frame = vk.frames[vk.currentFrame];
	vkWaitForFences( vk.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX );

	// Proactive resize check - the common real-world case (a window drag-
	// resize) reaches here before vkAcquireNextImageKHR ever has a chance
	// to complain, so catching it here means a resize is usually invisible
	// to the frame that follows it rather than causing even one dropped/
	// warned-about frame. See VK_RecreateSwapchain's own comment
	// (tr_init.cpp) for the real bug this - together with the
	// VK_ERROR_OUT_OF_DATE_KHR retry below - fixes: without either check,
	// any real resize permanently froze rendering (every later frame's
	// acquire kept failing against a swapchain still sized for the old
	// window) until a full engine restart, not just until the next resize.
	int drawableW = 0, drawableH = 0;
	SDL_Vulkan_GetDrawableSize( vk.window, &drawableW, &drawableH );
	if ( drawableW > 0 && drawableH > 0 &&
		( (uint32_t)drawableW != vk.swapchainExtent.width || (uint32_t)drawableH != vk.swapchainExtent.height ) )
	{
		VK_RecreateSwapchain();
	}

	VkResult acquireResult = vkAcquireNextImageKHR( vk.device, vk.swapchain, UINT64_MAX,
		frame.imageAvailable, VK_NULL_HANDLE, &vk.currentSwapchainImage );
	if ( acquireResult == VK_ERROR_OUT_OF_DATE_KHR )
	{
		// The proactive check above missed it (e.g. a surface capability
		// change not reflected in drawable size alone) - recreate and
		// retry once. A second failure here is treated the same as any
		// other unexpected result (skip this one frame, warn) rather than
		// looping - the next frame's proactive check above is the real
		// backstop against getting stuck.
		VK_RecreateSwapchain();
		acquireResult = vkAcquireNextImageKHR( vk.device, vk.swapchain, UINT64_MAX,
			frame.imageAvailable, VK_NULL_HANDLE, &vk.currentSwapchainImage );
	}
	if ( acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: vkAcquireNextImageKHR returned %d\n", (int)acquireResult );
		return;
	}

	vkResetFences( vk.device, 1, &frame.inFlight );
	vkResetCommandBuffer( frame.commandBuffer, 0 );

	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	vkBeginCommandBuffer( frame.commandBuffer, &beginInfo );

	VkClearValue clearValues[2];
	clearValues[0].color = { { vk.clearColor[0], vk.clearColor[1], vk.clearColor[2], vk.clearColor[3] } };
	clearValues[1].depthStencil = { 1.0f, 0 };

	VkRenderPassBeginInfo rpInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	rpInfo.renderPass = vk.renderPass;
	rpInfo.framebuffer = vk.swapchainFramebuffers[vk.currentSwapchainImage];
	rpInfo.renderArea.extent = vk.swapchainExtent;
	rpInfo.clearValueCount = 2;
	rpInfo.pClearValues = clearValues;

	vkCmdBeginRenderPass( frame.commandBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE );

	VkViewport viewport = { 0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.0f, 1.0f };
	VkRect2D scissor = { { 0, 0 }, vk.swapchainExtent };
	vkCmdSetViewport( frame.commandBuffer, 0, 1, &viewport );
	vkCmdSetScissor( frame.commandBuffer, 0, 1, &scissor );

	vk.activeCommandBuffer = frame.commandBuffer;
	vk.frameActive = true;
	s_uiVertexCursor = 0;
	// Each command buffer starts with no pipeline bound - force the first
	// draw this frame (2D or 3D) to bind one rather than trusting last
	// frame's state.
	vk.lastBoundPipeline = VK_NULL_HANDLE;
}

void RE_EndFrame( int *frontEndMsec, int *backEndMsec )
{
	if ( frontEndMsec ) *frontEndMsec = 0;
	if ( backEndMsec ) *backEndMsec = 0;

	if ( !vk.frameActive )
	{
		return;
	}

	vkFrame_t &frame = vk.frames[vk.currentFrame];
	VkImage swapImage = vk.swapchainImages[vk.currentSwapchainImage];

	vkCmdEndRenderPass( frame.commandBuffer );

	// The render pass finalLayout is TRANSFER_SRC_OPTIMAL (see VK_CreateRenderPass) so the
	// screenshot readback below and the present-layout transition can both use it directly.
	VK_CopySwapchainImageToReadback( frame.commandBuffer, swapImage );

	VkImageMemoryBarrier presentBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	presentBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	presentBarrier.image = swapImage;
	presentBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	presentBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	presentBarrier.dstAccessMask = 0;
	vkCmdPipelineBarrier( frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, nullptr, 0, nullptr, 1, &presentBarrier );

	vkEndCommandBuffer( frame.commandBuffer );

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &frame.imageAvailable;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &frame.commandBuffer;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &frame.renderFinished;

	vkQueueSubmit( vk.graphicsQueue, 1, &submitInfo, frame.inFlight );

	VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &frame.renderFinished;
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &vk.swapchain;
	presentInfo.pImageIndices = &vk.currentSwapchainImage;

	VkResult presentResult = vkQueuePresentKHR( vk.graphicsQueue, &presentInfo );
	if ( presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: vkQueuePresentKHR returned %d\n", (int)presentResult );
	}

	// The readback copy above needs to finish before its memory is read back on the CPU.
	vkQueueWaitIdle( vk.graphicsQueue );

	vk.frameActive = false;
	vk.activeCommandBuffer = VK_NULL_HANDLE;
	vk.currentFrame = ( vk.currentFrame + 1 ) % VK_FRAMES_IN_FLIGHT;
}

// ============================================================================
// Screenshot readback
//
// A persistent host-visible linear-tiling image, sized to match the
// swapchain, that every EndFrame() copies the just-rendered frame into (see
// VK_CopySwapchainImageToReadback above). screenshot_png and GetScreenShot
// both just read pixels back out of this, decoupling them entirely from
// swapchain/present timing.
// ============================================================================

static VkImage s_readbackImage = VK_NULL_HANDLE;
static VkDeviceMemory s_readbackMemory = VK_NULL_HANDLE;
static void *s_readbackMapped = nullptr;
static VkSubresourceLayout s_readbackLayout = {};
static bool s_readbackIsBGRA = true;

static void VK_CreateReadbackImage( void )
{
	VkImageCreateInfo imgInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgInfo.imageType = VK_IMAGE_TYPE_2D;
	imgInfo.extent = { vk.swapchainExtent.width, vk.swapchainExtent.height, 1 };
	imgInfo.mipLevels = 1;
	imgInfo.arrayLayers = 1;
	imgInfo.format = vk.swapchainFormat;
	imgInfo.tiling = VK_IMAGE_TILING_LINEAR;
	imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	vkCreateImage( vk.device, &imgInfo, nullptr, &s_readbackImage );

	VkMemoryRequirements memReq;
	vkGetImageMemoryRequirements( vk.device, s_readbackImage, &memReq );

	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties( vk.physicalDevice, &memProps );
	uint32_t memType = 0;
	VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for ( uint32_t i = 0; i < memProps.memoryTypeCount; i++ )
	{
		if ( (memReq.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want )
		{
			memType = i;
			break;
		}
	}

	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = memType;
	vkAllocateMemory( vk.device, &allocInfo, nullptr, &s_readbackMemory );
	vkBindImageMemory( vk.device, s_readbackImage, s_readbackMemory, 0 );

	VkImageSubresource subres = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
	vkGetImageSubresourceLayout( vk.device, s_readbackImage, &subres, &s_readbackLayout );

	vkMapMemory( vk.device, s_readbackMemory, 0, VK_WHOLE_SIZE, 0, &s_readbackMapped );

	s_readbackIsBGRA = ( vk.swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM || vk.swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB );
}

void VK_DestroyReadbackImage( void )
{
	if ( s_readbackMapped ) { vkUnmapMemory( vk.device, s_readbackMemory ); s_readbackMapped = nullptr; }
	if ( s_readbackImage ) { vkDestroyImage( vk.device, s_readbackImage, nullptr ); s_readbackImage = VK_NULL_HANDLE; }
	if ( s_readbackMemory ) { vkFreeMemory( vk.device, s_readbackMemory, nullptr ); s_readbackMemory = VK_NULL_HANDLE; }
}

void VK_CopySwapchainImageToReadback( VkCommandBuffer cmd, VkImage swapImage )
{
	if ( !s_readbackImage )
	{
		VK_CreateReadbackImage();
	}

	VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.image = s_readbackImage;
	toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	toDst.srcAccessMask = 0;
	toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &toDst );

	VkImageCopy region = {};
	region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.extent = { vk.swapchainExtent.width, vk.swapchainExtent.height, 1 };
	vkCmdCopyImage( cmd, swapImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		s_readbackImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );

	VkImageMemoryBarrier toGeneral = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toGeneral.image = s_readbackImage;
	toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toGeneral.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		0, 0, nullptr, 0, nullptr, 1, &toGeneral );
}

// Fetches one RGB (3 bytes/pixel) pixel from the readback image, converting
// from whatever channel order the swapchain format actually uses.
static void VK_ReadbackPixelRGB( uint32_t x, uint32_t y, byte out[3] )
{
	const byte *row = (const byte *)s_readbackMapped + s_readbackLayout.offset + (VkDeviceSize)y * s_readbackLayout.rowPitch;
	const byte *px = row + (VkDeviceSize)x * 4;
	if ( s_readbackIsBGRA )
	{
		out[0] = px[2]; out[1] = px[1]; out[2] = px[0];
	}
	else
	{
		out[0] = px[0]; out[1] = px[1]; out[2] = px[2];
	}
}

static void R_ScreenshotFilename( char *buf, int bufSize, const char *ext )
{
	Com_sprintf( buf, bufSize, "screenshots/vkshot%s", ext );
}

void R_ScreenShotPNG_f( void )
{
	if ( !s_readbackMapped )
	{
		Com_Printf( "ScreenShot: no frame has been rendered yet\n" );
		return;
	}

	char checkname[MAX_OSPATH] = { 0 };
	qboolean silent = qfalse;

	if ( !strcmp( ri.Cmd_Argv( 1 ), "silent" ) )
		silent = qtrue;

	if ( ri.Cmd_Argc() == 2 && !silent )
	{
		Com_sprintf( checkname, sizeof( checkname ), "screenshots/%s.png", ri.Cmd_Argv( 1 ) );
	}
	else
	{
		R_ScreenshotFilename( checkname, sizeof( checkname ), ".png" );
	}

	uint32_t w = vk.swapchainExtent.width, h = vk.swapchainExtent.height;
	std::vector<byte> rgb( (size_t)w * h * 3 );

	// RE_SavePNG expects row 0 = bottom of the image (it flips internally,
	// matching GL's bottom-up screen readback convention), so feed it rows
	// in that order even though our source is top-down.
	for ( uint32_t y = 0; y < h; y++ )
	{
		uint32_t srcY = h - 1 - y;
		for ( uint32_t x = 0; x < w; x++ )
		{
			VK_ReadbackPixelRGB( x, srcY, &rgb[(size_t)(y * w + x) * 3] );
		}
	}

	RE_SavePNG( checkname, rgb.data(), w, h, 3 );

	if ( !silent )
		Com_Printf( "Wrote %s\n", checkname );
}

void RE_GetScreenShot( byte *buffer, int w, int h )
{
	if ( !s_readbackMapped || w <= 0 || h <= 0 )
	{
		return;
	}

	uint32_t srcW = vk.swapchainExtent.width, srcH = vk.swapchainExtent.height;

	for ( int y = 0; y < h; y++ )
	{
		uint32_t srcY = (uint32_t)( (float)y / h * srcH );
		if ( srcY >= srcH ) srcY = srcH - 1;
		for ( int x = 0; x < w; x++ )
		{
			uint32_t srcX = (uint32_t)( (float)x / w * srcW );
			if ( srcX >= srcW ) srcX = srcW - 1;
			VK_ReadbackPixelRGB( srcX, srcY, &buffer[(size_t)(y * w + x) * 3] );
		}
	}
}
