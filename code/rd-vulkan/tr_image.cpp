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

// Registers "shaders" as plain images - this renderer does not implement
// the .shader script (multi-stage/blend-mode) system yet, so a registered
// name is just decoded (via the shared, GL-agnostic R_LoadImage) and
// uploaded as a single RGBA texture. This covers the common case of a UI
// element that references an image file directly; it does not yet produce
// correct output for a shader with multiple stages, animation, or special
// blend modes.

#include "../server/exe_headers.h"

#include "tr_local.h"

extern void VK_CreateBuffer( VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
	VkBuffer *buffer, VkDeviceMemory *memory );
extern VkCommandBuffer VK_BeginOneShotCommands( void );
extern void VK_EndOneShotCommands( VkCommandBuffer cmd );

static void VK_TransitionImageLayout( VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout )
{
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	VkPipelineStageFlags srcStage, dstStage;

	if ( oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL )
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if ( oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL )
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
	else
	{
		ri.Error( ERR_FATAL, "rd-vulkan: unsupported image layout transition\n" );
		return;
	}

	vkCmdPipelineBarrier( cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier );
}

void VK_UploadImage( image_t *img, const byte *pixels, int width, int height )
{
	VkDeviceSize imageSize = (VkDeviceSize)width * height * 4;

	VkBuffer staging;
	VkDeviceMemory stagingMemory;
	VK_CreateBuffer( imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&staging, &stagingMemory );

	void *mapped;
	vkMapMemory( vk.device, stagingMemory, 0, imageSize, 0, &mapped );
	memcpy( mapped, pixels, (size_t)imageSize );
	vkUnmapMemory( vk.device, stagingMemory );

	VkImageCreateInfo imgInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgInfo.imageType = VK_IMAGE_TYPE_2D;
	imgInfo.extent = { (uint32_t)width, (uint32_t)height, 1 };
	imgInfo.mipLevels = 1;
	imgInfo.arrayLayers = 1;
	imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;

	if ( vkCreateImage( vk.device, &imgInfo, nullptr, &img->image ) != VK_SUCCESS )
	{
		ri.Error( ERR_FATAL, "rd-vulkan: vkCreateImage failed\n" );
	}

	VkMemoryRequirements memReq;
	vkGetImageMemoryRequirements( vk.device, img->image, &memReq );

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
	vkAllocateMemory( vk.device, &allocInfo, nullptr, &img->memory );
	vkBindImageMemory( vk.device, img->image, img->memory, 0 );

	VkCommandBuffer cmd = VK_BeginOneShotCommands();
	VK_TransitionImageLayout( cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

	VkBufferImageCopy region = {};
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
	vkCmdCopyBufferToImage( cmd, staging, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );

	VK_TransitionImageLayout( cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
	VK_EndOneShotCommands( cmd );

	vkDestroyBuffer( vk.device, staging, nullptr );
	vkFreeMemory( vk.device, stagingMemory, nullptr );

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = img->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCreateImageView( vk.device, &viewInfo, nullptr, &img->view );

	VkDescriptorSetAllocateInfo dsAlloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsAlloc.descriptorPool = vk.uiDescriptorPool;
	dsAlloc.descriptorSetCount = 1;
	dsAlloc.pSetLayouts = &vk.uiDescriptorSetLayout;
	vkAllocateDescriptorSets( vk.device, &dsAlloc, &img->descriptorSet );

	VkDescriptorImageInfo imgDescInfo = {};
	imgDescInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imgDescInfo.imageView = img->view;
	imgDescInfo.sampler = vk.uiSampler;

	VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = img->descriptorSet;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imgDescInfo;
	vkUpdateDescriptorSets( vk.device, 1, &write, 0, nullptr );

	img->width = width;
	img->height = height;
}

image_t *VK_CreateSolidImage( const char *name, byte r, byte g, byte b, byte a )
{
	image_t *img = new image_t();
	img->name = name;
	byte pixel[4] = { r, g, b, a };
	VK_UploadImage( img, pixel, 1, 1 );
	return img;
}

image_t *VK_FindImage( const char *name )
{
	auto it = vk.imagesByName.find( name );
	if ( it != vk.imagesByName.end() )
	{
		return vk.images[it->second];
	}

	byte *pic = nullptr;
	int width = 0, height = 0;
	R_LoadImage( name, &pic, &width, &height );
	if ( !pic )
	{
		return nullptr;
	}

	image_t *img = new image_t();
	img->name = name;
	VK_UploadImage( img, pic, width, height );
	R_Free( pic );

	qhandle_t handle = (qhandle_t)vk.images.size();
	vk.images.push_back( img );
	vk.imagesByName[name] = handle;

	return img;
}

void VK_ShutdownImages( void )
{
	for ( image_t *img : vk.images )
	{
		if ( !img ) continue;
		if ( img->view ) vkDestroyImageView( vk.device, img->view, nullptr );
		if ( img->image ) vkDestroyImage( vk.device, img->image, nullptr );
		if ( img->memory ) vkFreeMemory( vk.device, img->memory, nullptr );
		delete img;
	}
	vk.images.clear();
	vk.imagesByName.clear();
}

qhandle_t RE_RegisterShaderNoMip( const char *name )
{
	image_t *img = VK_FindImage( name );
	if ( !img )
	{
		// Not "0" (draws opaque white) - a failed lookup (e.g. a videoMap or
		// other .shader-script-only reference, see tr_local.h) should be
		// invisible, not paint a solid white rectangle over the menu.
		return vk.imagesByName[vk.transparentImage->name];
	}
	return vk.imagesByName[img->name];
}

qhandle_t RE_RegisterShader( const char *name )
{
	return RE_RegisterShaderNoMip( name );
}
