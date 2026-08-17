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

// Static world geometry - the first slice of 3D rendering in this renderer.
// Loads a .bsp's opaque, non-patch surfaces (positions + diffuse UVs only)
// and draws them unlit, unculled, with a real camera. See README.md for
// exactly what that does and does not cover: no lighting, no entities, no
// Ghoul2, no BSP visibility culling, no patches/curves, first-.shader-stage
// texturing only (same scope as the 2D UI path, see tr_shader.cpp).
//
// The BSP lump structs (dheader_t/dshader_t/drawVert_t/dsurface_t) are
// shared, GL-agnostic definitions from qcommon/qfiles.h - unlike Ghoul2
// (see the "Ghoul2 is not reused from rd-vanilla" README section), there's
// no quote-include trap here to avoid, so they're used directly rather than
// redefined. The camera matrix math below, however, IS a fresh
// reimplementation, not a reuse of rd-vanilla/tr_main.cpp's R_RotateForViewer/
// R_SetupProjection: Vulkan's clip-space convention differs from GL's (see
// VK_BuildProjectionMatrix), so reusing that code would need reworking
// regardless of the include problem. The actual numeric formulas and matrix
// index convention are copied from that file, since they're well-understood
// and proven; only the parts that must differ for Vulkan do.

#include "../server/exe_headers.h"

#include "tr_local.h"
#include <cmath>
#include <cstring>
#include <vector>

struct WorldSurfaceBatch
{
	image_t *image;
	uint32_t firstIndex;
	uint32_t indexCount;
};

static std::vector<WorldSurfaceBatch> s_worldSurfaces;
static VkBuffer s_worldVertexBuffer = VK_NULL_HANDLE;
static VkDeviceMemory s_worldVertexBufferMemory = VK_NULL_HANDLE;
static VkBuffer s_worldIndexBuffer = VK_NULL_HANDLE;
static VkDeviceMemory s_worldIndexBufferMemory = VK_NULL_HANDLE;
static bool s_worldLoaded = false;

void VK_ShutdownWorld( void )
{
	if ( s_worldVertexBuffer ) { vkDestroyBuffer( vk.device, s_worldVertexBuffer, nullptr ); s_worldVertexBuffer = VK_NULL_HANDLE; }
	if ( s_worldVertexBufferMemory ) { vkFreeMemory( vk.device, s_worldVertexBufferMemory, nullptr ); s_worldVertexBufferMemory = VK_NULL_HANDLE; }
	if ( s_worldIndexBuffer ) { vkDestroyBuffer( vk.device, s_worldIndexBuffer, nullptr ); s_worldIndexBuffer = VK_NULL_HANDLE; }
	if ( s_worldIndexBufferMemory ) { vkFreeMemory( vk.device, s_worldIndexBufferMemory, nullptr ); s_worldIndexBufferMemory = VK_NULL_HANDLE; }
	s_worldSurfaces.clear();
	s_worldLoaded = false;
}

static void VK_UploadDeviceLocalBuffer( const void *data, VkDeviceSize size, VkBufferUsageFlags usage,
	VkBuffer *outBuffer, VkDeviceMemory *outMemory )
{
	VkBuffer staging;
	VkDeviceMemory stagingMemory;
	VK_CreateBuffer( size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&staging, &stagingMemory );

	void *mapped;
	vkMapMemory( vk.device, stagingMemory, 0, size, 0, &mapped );
	memcpy( mapped, data, (size_t)size );
	vkUnmapMemory( vk.device, stagingMemory );

	VK_CreateBuffer( size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outBuffer, outMemory );

	VkCommandBuffer cmd = VK_BeginOneShotCommands();
	VkBufferCopy copy = { 0, 0, size };
	vkCmdCopyBuffer( cmd, staging, *outBuffer, 1, &copy );
	VK_EndOneShotCommands( cmd );

	vkDestroyBuffer( vk.device, staging, nullptr );
	vkFreeMemory( vk.device, stagingMemory, nullptr );
}

void RE_LoadWorldMap( const char *name )
{
	VK_ShutdownWorld();

	void *buffer = nullptr;
	long len = ri.FS_ReadFile( name, &buffer );
	if ( !buffer || len <= 0 )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: RE_LoadWorldMap: %s not found\n", name );
		return;
	}

	const dheader_t *header = (const dheader_t *)buffer;
	if ( header->ident != BSP_IDENT || header->version != BSP_VERSION )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: RE_LoadWorldMap: %s is not a supported BSP (ident/version mismatch)\n", name );
		ri.FS_FreeFile( buffer );
		return;
	}

	const byte *base = (const byte *)buffer;
	auto lumpData = [&]( int lumpIndex ) -> const byte * { return base + header->lumps[lumpIndex].fileofs; };
	auto lumpCount = [&]( int lumpIndex, size_t elemSize ) -> int { return header->lumps[lumpIndex].filelen / (int)elemSize; };

	const dshader_t *shaders = (const dshader_t *)lumpData( LUMP_SHADERS );
	int numShaders = lumpCount( LUMP_SHADERS, sizeof( dshader_t ) );

	const drawVert_t *verts = (const drawVert_t *)lumpData( LUMP_DRAWVERTS );
	int numVerts = lumpCount( LUMP_DRAWVERTS, sizeof( drawVert_t ) );

	const int *indexes = (const int *)lumpData( LUMP_DRAWINDEXES );
	int numIndexes = lumpCount( LUMP_DRAWINDEXES, sizeof( int ) );

	const dsurface_t *surfaces = (const dsurface_t *)lumpData( LUMP_SURFACES );
	int numSurfaces = lumpCount( LUMP_SURFACES, sizeof( dsurface_t ) );

	std::vector<WorldVertex> cpuVerts( (size_t)numVerts );
	for ( int i = 0; i < numVerts; i++ )
	{
		cpuVerts[i].pos[0] = verts[i].xyz[0];
		cpuVerts[i].pos[1] = verts[i].xyz[1];
		cpuVerts[i].pos[2] = verts[i].xyz[2];
		cpuVerts[i].uv[0] = verts[i].st[0];
		cpuVerts[i].uv[1] = verts[i].st[1];
	}

	std::vector<uint32_t> cpuIndexes;
	cpuIndexes.reserve( (size_t)numIndexes );

	for ( int i = 0; i < numSurfaces; i++ )
	{
		const dsurface_t &surf = surfaces[i];
		// Patches (curved surfaces) need tessellation and flares need their
		// own draw path - neither is implemented yet, skip rather than draw
		// garbage geometry from their raw control-point data.
		if ( surf.surfaceType != MST_PLANAR && surf.surfaceType != MST_TRIANGLE_SOUP )
		{
			continue;
		}
		if ( surf.shaderNum < 0 || surf.shaderNum >= numShaders || surf.numIndexes <= 0 )
		{
			continue;
		}

		// SURF_NODRAW (game/surfaceflags.h - transitively available here via
		// q_shared.h, no extra include needed) marks editor-only geometry
		// (caulk, clip, trigger volumes, ...) that the real engine never
		// generates a drawsurface for at all. Without this check every one
		// of academy1's caulk/clip/nodraw shaders (3 of its 16) fails
		// VK_FindImage and paints a stray white polygon - not a
		// missing-texture bug, a missing "should this surface draw at all"
		// check. SURF_SKY: a textured polygon is actively misleading for
		// sky - it's supposed to be a portal/skybox, not a surface with its
		// own diffuse map, so skip it here too rather than draw the sky
		// shader's (usually unrelated or missing) base image as a wall.
		if ( shaders[surf.shaderNum].surfaceFlags & ( SURF_NODRAW | SURF_SKY ) )
		{
			continue;
		}

		image_t *img = VK_FindImage( shaders[surf.shaderNum].shader );
		if ( !img )
		{
			// Unlike RE_StretchPic's out-of-range-handle case (which really
			// does mean "caller asked for white"), a failed lookup here
			// means the shader has no plain image of its own - typically a
			// stages-only effect shader (fog/dust volumes, decals) whose
			// name doesn't resolve to a file because it's meant to be built
			// from its stages' own `map` references, which this renderer
			// doesn't parse for world geometry (see README.md). Those are
			// usually explicitly translucent (surfaceparm trans/nonopaque -
			// e.g. academy1's textures/common/dark_dust) and this renderer
			// has no blend pipeline for world geometry yet (see
			// VK_CreateWorldPipeline), so drawing them as an *opaque white*
			// quad is actively wrong, not just imprecise - it was covering
			// large parts of the screen. Skip the surface instead, same
			// "invisible beats wrong" call as the RE_RegisterShaderNoMip
			// videologo fix in tr_image.cpp.
			continue;
		}

		uint32_t firstIndex = (uint32_t)cpuIndexes.size();
		for ( int j = 0; j < surf.numIndexes; j++ )
		{
			// BSP drawIndexes are surface-local (0-based within the
			// surface's own [firstVert, firstVert+numVerts) range) - offset
			// by firstVert to get indices into the single combined vertex
			// buffer this renderer uploads for the whole world.
			cpuIndexes.push_back( (uint32_t)( surf.firstVert + indexes[surf.firstIndex + j] ) );
		}

		s_worldSurfaces.push_back( { img, firstIndex, (uint32_t)surf.numIndexes } );
	}

	ri.FS_FreeFile( buffer );

	if ( cpuVerts.empty() || cpuIndexes.empty() )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: RE_LoadWorldMap: %s has no drawable static surfaces\n", name );
		return;
	}

	VK_UploadDeviceLocalBuffer( cpuVerts.data(), cpuVerts.size() * sizeof( WorldVertex ),
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s_worldVertexBuffer, &s_worldVertexBufferMemory );
	VK_UploadDeviceLocalBuffer( cpuIndexes.data(), cpuIndexes.size() * sizeof( uint32_t ),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &s_worldIndexBuffer, &s_worldIndexBufferMemory );

	s_worldLoaded = true;

	ri.Printf( PRINT_ALL, "rd-vulkan: loaded %s: %d draw batches, %d verts, %d indexes (skipped patches/flares)\n",
		name, (int)s_worldSurfaces.size(), numVerts, (int)cpuIndexes.size() );
}

// No per-frame entity/poly/light list to clear yet (RE_AddRefEntityToScene
// et al. are still stubs in tr_init.cpp) - the world itself is static and
// always drawn by RE_RenderScene regardless, so there's nothing to do here
// yet. Kept as a real (empty) function rather than folded away so the
// refexport_t entry point has an obvious place to grow into.
void RE_ClearScene( void )
{
}

// Byte-for-byte copy of rd-vanilla's tr_main.cpp myGlMultMatrix (not
// included, see file header) - kept identical since transform math is easy
// to get subtly wrong by rederiving. IMPORTANT, and easy to get backwards
// (this renderer's first attempt did): when a/b/out are read as GLSL-style
// column-major matrices (data[col*4+row], the layout a mat4 push constant
// actually gets), this computes out = b * a, not out = a * b - the
// arguments are effectively swapped relative to normal multiplication
// notation. Callers must pass (secondTransformApplied, firstTransformApplied)
// to get the usual "apply first, then second" composition - see
// VK_BuildViewMatrix and RE_RenderScene's mvp computation for both call
// sites and why each argument order is what it is.
static void VK_MultiplyMatrix( const float *a, const float *b, float *out )
{
	for ( int i = 0; i < 4; i++ )
	{
		for ( int j = 0; j < 4; j++ )
		{
			out[i * 4 + j] =
				a[i * 4 + 0] * b[0 * 4 + j]
				+ a[i * 4 + 1] * b[1 * 4 + j]
				+ a[i * 4 + 2] * b[2 * 4 + j]
				+ a[i * 4 + 3] * b[3 * 4 + j];
		}
	}
}

// World-to-camera-space matrix from refdef_t's origin/axis, using the same
// construction as rd-vanilla's R_RotateForViewer (tr_main.cpp): rotate by
// the view axes, translate by -origin, then apply a fixed rotation from
// Quake's "looking down +X" world convention into a GL-style "looking down
// -Z" camera space. That fixed rotation is convention-only (not a GL call),
// so it applies to Vulkan's camera space the same way.
static void VK_BuildViewMatrix( const refdef_t *fd, float *out )
{
	float viewer[16] = {};
	viewer[0] = fd->viewaxis[0][0]; viewer[4] = fd->viewaxis[0][1]; viewer[8] = fd->viewaxis[0][2];
	viewer[1] = fd->viewaxis[1][0]; viewer[5] = fd->viewaxis[1][1]; viewer[9] = fd->viewaxis[1][2];
	viewer[2] = fd->viewaxis[2][0]; viewer[6] = fd->viewaxis[2][1]; viewer[10] = fd->viewaxis[2][2];
	viewer[12] = -fd->vieworg[0] * viewer[0] - fd->vieworg[1] * viewer[4] - fd->vieworg[2] * viewer[8];
	viewer[13] = -fd->vieworg[0] * viewer[1] - fd->vieworg[1] * viewer[5] - fd->vieworg[2] * viewer[9];
	viewer[14] = -fd->vieworg[0] * viewer[2] - fd->vieworg[1] * viewer[6] - fd->vieworg[2] * viewer[10];
	viewer[15] = 1;

	static const float flip[16] = {
		0, 0, -1, 0,
		-1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 0, 1
	};

	// Want: out = flip * viewer (apply viewer's world->quake-camera-space
	// rotation/translation first, then flip into GL/Vulkan axes). Per
	// VK_MultiplyMatrix's comment, that means passing (viewer, flip).
	VK_MultiplyMatrix( viewer, flip, out );
}

// Perspective projection, same x/y formulas as rd-vanilla's R_SetupProjection
// (tr_main.cpp) but with the z row rederived for Vulkan's [0,1] depth-clip
// range instead of GL's [-1,1] (GL: z_ndc = row2.v / -z_view with row2 =
// (-(zFar+zNear)/depth, -2*zFar*zNear/depth); Vulkan needs (z_ndc+1)/2, i.e.
// new_row2 = 0.5*row2 + 0.5*row3 where row3 = (0,0,-1,0) - works out to the
// standard Vulkan-clip perspective z row below). The x/y rows are untouched
// by that change. Y-flip (Vulkan NDC is Y-down, GL is Y-up) is handled
// separately via a negative-height VkViewport in RE_RenderScene, not here.
static void VK_BuildProjectionMatrix( const refdef_t *fd, float *out )
{
	const float zNear = 4.0f;
	// No dynamic far-clip/visBounds computation yet (see README.md) - a
	// generous fixed distance is safe for a first pass, just less efficient
	// depth precision-wise than rd-vanilla's per-frame computed zFar.
	const float zFar = 4096.0f;

	float ymax = zNear * tanf( fd->fov_y * (float)M_PI / 360.0f );
	float ymin = -ymax;
	float xmax = zNear * tanf( fd->fov_x * (float)M_PI / 360.0f );
	float xmin = -xmax;

	float width = xmax - xmin;
	float height = ymax - ymin;
	float depth = zFar - zNear;

	memset( out, 0, sizeof( float ) * 16 );
	out[0] = 2 * zNear / width;
	out[8] = ( xmax + xmin ) / width;
	out[5] = 2 * zNear / height;
	out[9] = ( ymax + ymin ) / height;
	out[10] = -zFar / depth;
	out[14] = -zFar * zNear / depth;
	out[11] = -1;
}

void RE_RenderScene( const refdef_t *fd )
{
	if ( !vk.frameActive || !s_worldLoaded || ( fd->rdflags & RDF_NOWORLDMODEL ) )
	{
		return;
	}

	VkCommandBuffer cmd = vk.activeCommandBuffer;

	float viewMatrix[16], projMatrix[16], mvp[16];
	VK_BuildViewMatrix( fd, viewMatrix );
	VK_BuildProjectionMatrix( fd, projMatrix );
	// Want: mvp = projection * view (apply view first, then projection).
	// Per VK_MultiplyMatrix's comment, that means passing (view, projection).
	VK_MultiplyMatrix( viewMatrix, projMatrix, mvp );

	// Vulkan's NDC is Y-down where GL's is Y-up; a negative-height viewport
	// (core since Vulkan 1.1, which this renderer already requires) flips Y
	// back without touching the projection matrix, so VK_BuildProjectionMatrix
	// above can otherwise mirror rd-vanilla's formulas directly.
	VkViewport viewport = { (float)fd->x, (float)( fd->y + fd->height ), (float)fd->width, -(float)fd->height, 0.0f, 1.0f };
	VkRect2D scissor = { { fd->x, fd->y }, { (uint32_t)fd->width, (uint32_t)fd->height } };
	vkCmdSetViewport( cmd, 0, 1, &viewport );
	vkCmdSetScissor( cmd, 0, 1, &scissor );

	if ( vk.worldPipeline != vk.lastBoundPipeline )
	{
		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.worldPipeline );
		vk.lastBoundPipeline = vk.worldPipeline;
	}

	vkCmdPushConstants( cmd, vk.worldPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof( mvp ), mvp );

	VkDeviceSize vertexOffset = 0;
	vkCmdBindVertexBuffers( cmd, 0, 1, &s_worldVertexBuffer, &vertexOffset );
	vkCmdBindIndexBuffer( cmd, s_worldIndexBuffer, 0, VK_INDEX_TYPE_UINT32 );

	for ( const WorldSurfaceBatch &batch : s_worldSurfaces )
	{
		vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.worldPipelineLayout,
			0, 1, &batch.image->descriptorSet, 0, nullptr );
		vkCmdDrawIndexed( cmd, batch.indexCount, 1, batch.firstIndex, 0, 0 );
	}

	// Restore the full-screen viewport/scissor for any 2D drawing
	// (RE_StretchPic) that follows this scene render within the same frame -
	// it doesn't set its own, it relies on whatever RE_BeginFrame or the
	// last RE_RenderScene left bound.
	VkViewport fullViewport = { 0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.0f, 1.0f };
	VkRect2D fullScissor = { { 0, 0 }, vk.swapchainExtent };
	vkCmdSetViewport( cmd, 0, 1, &fullViewport );
	vkCmdSetScissor( cmd, 0, 1, &fullScissor );
}
