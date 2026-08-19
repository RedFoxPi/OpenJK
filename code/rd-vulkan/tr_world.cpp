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
// Loads a .bsp's opaque surfaces, including tessellated curved patches (see
// VK_TessellatePatchQuad) - diffuse texture * baked lightmap (see
// VK_LoadLightmaps) - and draws them frustum-culled, with a real camera. See
// README.md for exactly what that does and does not cover: no dynamic
// lights, no BSP visibility culling, first-.shader-stage texturing only
// (same scope as the 2D UI path, see tr_shader.cpp).
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
	// Combines a surface's diffuse image and its lightmap image (see
	// VK_BuildWorldDescriptorSet) - one set per batch, not per texture,
	// since Vulkan descriptor sets are fixed-shape (both bindings must be
	// filled at once) and a diffuse image can pair with different lightmaps
	// on different surfaces.
	VkDescriptorSet descriptorSet;
	uint32_t firstIndex;
	uint32_t indexCount;
	// World-space AABB, used for view-frustum culling in RE_RenderScene
	// (see VK_AABBOutsideFrustum). Unused/left zeroed for sky faces - the
	// sky is always camera-centered and drawn unconditionally, see
	// RE_RenderScene.
	float mins[3];
	float maxs[3];
};

static std::vector<WorldSurfaceBatch> s_worldSurfaces;
static VkBuffer s_worldVertexBuffer = VK_NULL_HANDLE;
static VkDeviceMemory s_worldVertexBufferMemory = VK_NULL_HANDLE;
static VkBuffer s_worldIndexBuffer = VK_NULL_HANDLE;
static VkDeviceMemory s_worldIndexBufferMemory = VK_NULL_HANDLE;
static bool s_worldLoaded = false;

// Lightmaps (see VK_LoadLightmaps) - owned separately from the regular
// named-image cache in tr_image.cpp since they're anonymous (indexed by
// position in the BSP's LUMP_LIGHTMAPS, not by name) and per-map, not
// per-name-cached across map loads.
static std::vector<image_t *> s_lightmapImages;
// Bound for any surface with no lightmap of its own (dsurface_t.lightmapNum
// < 0 - vertex-lit or fullbright surfaces) so the shader's diffuse*lightmap
// multiply is a no-op rather than requiring a separate unlit code path.
static image_t *s_whiteLightmap = nullptr;

// Skybox (see VK_LoadSky) - 6 faces, reuses WorldSurfaceBatch's shape (one
// combined descriptor set + index range per face) even though it's not a
// BSP surface, since the draw call shape (bind descriptor set, draw indexed)
// is identical.
static std::vector<WorldSurfaceBatch> s_skyFaces;
static VkBuffer s_skyVertexBuffer = VK_NULL_HANDLE;
static VkDeviceMemory s_skyVertexBufferMemory = VK_NULL_HANDLE;
static VkBuffer s_skyIndexBuffer = VK_NULL_HANDLE;
static VkDeviceMemory s_skyIndexBufferMemory = VK_NULL_HANDLE;
static bool s_skyLoaded = false;

// World fog (see VK_LoadWorldFog) - this renderer only supports one global
// fog volume per map (BSP LUMP_FOGS entries with brushNum == -1, matching
// rd-vanilla's own R_LoadFogs/worldData.globalFog convention - see
// tr_bsp.cpp), applied uniformly to all world geometry. Per-brush local fog
// volumes (a fog entry with a real brushNum) are not implemented - see
// README.md.
static bool s_worldFogEnabled = false;
static float s_worldFogColor[3];
static float s_worldFogOpaqueDist;

static void VK_DestroyWorldImage( image_t *img )
{
	if ( !img ) return;
	if ( img->view ) vkDestroyImageView( vk.device, img->view, nullptr );
	if ( img->image ) vkDestroyImage( vk.device, img->image, nullptr );
	if ( img->memory ) vkFreeMemory( vk.device, img->memory, nullptr );
	delete img;
}

void VK_ShutdownWorld( void )
{
	if ( s_worldVertexBuffer ) { vkDestroyBuffer( vk.device, s_worldVertexBuffer, nullptr ); s_worldVertexBuffer = VK_NULL_HANDLE; }
	if ( s_worldVertexBufferMemory ) { vkFreeMemory( vk.device, s_worldVertexBufferMemory, nullptr ); s_worldVertexBufferMemory = VK_NULL_HANDLE; }
	if ( s_worldIndexBuffer ) { vkDestroyBuffer( vk.device, s_worldIndexBuffer, nullptr ); s_worldIndexBuffer = VK_NULL_HANDLE; }
	if ( s_worldIndexBufferMemory ) { vkFreeMemory( vk.device, s_worldIndexBufferMemory, nullptr ); s_worldIndexBufferMemory = VK_NULL_HANDLE; }
	for ( image_t *img : s_lightmapImages ) VK_DestroyWorldImage( img );
	s_lightmapImages.clear();
	VK_DestroyWorldImage( s_whiteLightmap );
	s_whiteLightmap = nullptr;
	if ( s_skyVertexBuffer ) { vkDestroyBuffer( vk.device, s_skyVertexBuffer, nullptr ); s_skyVertexBuffer = VK_NULL_HANDLE; }
	if ( s_skyVertexBufferMemory ) { vkFreeMemory( vk.device, s_skyVertexBufferMemory, nullptr ); s_skyVertexBufferMemory = VK_NULL_HANDLE; }
	if ( s_skyIndexBuffer ) { vkDestroyBuffer( vk.device, s_skyIndexBuffer, nullptr ); s_skyIndexBuffer = VK_NULL_HANDLE; }
	if ( s_skyIndexBufferMemory ) { vkFreeMemory( vk.device, s_skyIndexBufferMemory, nullptr ); s_skyIndexBufferMemory = VK_NULL_HANDLE; }
	s_skyFaces.clear();
	s_skyLoaded = false;
	s_worldFogEnabled = false;
	// Descriptor sets allocated per-batch below come from this pool, not
	// individually tracked - reclaim them all at once rather than freeing
	// one by one (vk.worldDescriptorPool wasn't created with
	// VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, so individual
	// vkFreeDescriptorSets calls aren't valid on it anyway).
	if ( vk.worldDescriptorPool ) vkResetDescriptorPool( vk.device, vk.worldDescriptorPool, 0 );
	s_worldSurfaces.clear();
	s_worldLoaded = false;
}

// Not static - reused by tr_model.cpp for Ghoul2 vertex/index buffers, see
// tr_local.h.
void VK_UploadDeviceLocalBuffer( const void *data, VkDeviceSize size, VkBufferUsageFlags usage,
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

// Lightmaps are stored as a flat array of fixed-size (LIGHTMAP_WIDTH x
// LIGHTMAP_HEIGHT) RGB888 images, one after another - no header, no count
// field, just divide the lump length by one image's byte size. Expanded to
// RGBA here since VK_UploadImage (tr_image.cpp) - reused as-is, this is a
// plain texture upload once expanded - expects 4 bytes/pixel like every
// other image this renderer handles.
static void VK_LoadLightmaps( const byte *lumpData, int lumpLen )
{
	const int imageSize = LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3;
	int count = imageSize > 0 ? lumpLen / imageSize : 0;

	std::vector<byte> rgba( (size_t)LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 4 );
	for ( int i = 0; i < count; i++ )
	{
		const byte *rgb = lumpData + (size_t)i * imageSize;
		for ( int p = 0; p < LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT; p++ )
		{
			rgba[(size_t)p * 4 + 0] = rgb[(size_t)p * 3 + 0];
			rgba[(size_t)p * 4 + 1] = rgb[(size_t)p * 3 + 1];
			rgba[(size_t)p * 4 + 2] = rgb[(size_t)p * 3 + 2];
			rgba[(size_t)p * 4 + 3] = 255;
		}

		image_t *img = new image_t();
		VK_UploadImage( img, rgba.data(), LIGHTMAP_WIDTH, LIGHTMAP_HEIGHT );
		s_lightmapImages.push_back( img );
	}

	byte white[4] = { 255, 255, 255, 255 };
	s_whiteLightmap = new image_t();
	VK_UploadImage( s_whiteLightmap, white, 1, 1 );
}

// One combined (diffuse, lightmap) descriptor set per surface batch - see
// WorldSurfaceBatch's comment for why this can't be cached per-texture the
// way the UI path's single-binding descriptor sets are (image_t::
// descriptorSet, unused here): the same diffuse image can legitimately pair
// with different lightmaps on different surfaces. Not static - also used by
// tr_model.cpp for Ghoul2 surfaces (paired with vk.whiteImage, since Ghoul2
// meshes have no lightmap of their own), see tr_local.h. Takes the pool
// explicitly (rather than always using vk.worldDescriptorPool) since Ghoul2
// models need their own pool with an independent lifetime - see
// vkGlobals_t::ghoul2DescriptorPool's comment.
VkDescriptorSet VK_BuildWorldDescriptorSet( VkDescriptorPool pool, image_t *diffuse, image_t *lightmap )
{
	VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	alloc.descriptorPool = pool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &vk.worldDescriptorSetLayout;

	VkDescriptorSet set = VK_NULL_HANDLE;
	VK_Check( vkAllocateDescriptorSets( vk.device, &alloc, &set ), "vkAllocateDescriptorSets (world)" );

	VkDescriptorImageInfo imageInfos[2] = {};
	imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imageInfos[0].imageView = diffuse->view;
	imageInfos[0].sampler = vk.worldSampler;
	imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imageInfos[1].imageView = lightmap->view;
	imageInfos[1].sampler = vk.worldSampler;

	VkWriteDescriptorSet writes[2] = {};
	writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	writes[0].dstSet = set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[0].pImageInfo = &imageInfos[0];
	writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	writes[1].dstSet = set;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pImageInfo = &imageInfos[1];

	vkUpdateDescriptorSets( vk.device, 2, writes, 0, nullptr );
	return set;
}

// Standard Quake3 skybox: 6 face images named <baseName>_rt/_lf/_bk/_ft/_up/
// _dn, forming a box always centered on the camera (see RE_RenderScene) so
// it reads as infinitely distant. Face vertex XYZ/UV formulas are copied
// from rd-vanilla's tr_sky.cpp MakeSkyVec, evaluated only at the four
// s,t = +-1 corners - that function's actual job is subdividing/warping the
// sky into SKY_SUBDIVISIONS quads per face for a smoother dome and to
// texture-warp against the sky brush's real shape; this renderer draws each
// face as one flat quad instead (visible seams at the box edges, no
// per-brush warping) for a first pass, but uses the exact same corner
// formula so face orientation (which image goes on which side, right way
// up) matches rd-vanilla exactly rather than being guessed and gotten
// subtly wrong.
static void VK_LoadSky( const char *baseName )
{
	static const char *suffixes[6] = { "rt", "lf", "bk", "ft", "up", "dn" };
	static const int st_to_vec[6][3] = {
		{  3, -1,  2 }, { -3,  1,  2 },
		{  1,  3,  2 }, { -1, -3,  2 },
		{ -2, -1,  3 }, {  2, -1, -3 },
	};

	image_t *faces[6];
	bool allFacesFound = true;
	for ( int i = 0; i < 6; i++ )
	{
		char faceName[MAX_QPATH];
		Com_sprintf( faceName, sizeof( faceName ), "%s_%s", baseName, suffixes[i] );
		faces[i] = VK_FindImage( faceName );
		if ( !faces[i] )
		{
			ri.Printf( PRINT_WARNING, "rd-vulkan: sky face '%s' not found\n", faceName );
			allFacesFound = false;
			break;
		}
	}

	if ( !allFacesFound )
	{
		// Not every sky shader has 6 real face images to find in the first
		// place - `skyParms - <cloudheight> -` (a literal dash for the
		// farbox parameter) is valid Quake3 shader syntax for a fog/portal-
		// only sky with no textured cubemap at all (e.g. hoth2's
		// 'textures/skies/hoth', confirmed by reading skies.shader - no
		// hoth*_rt/_lf/etc face files exist in any assets*.pk3). This
		// renderer doesn't parse .shader skyParms (see README.md), so it
		// can't recover that shader's real fog colour or tell a genuinely
		// missing/misnamed face apart from an intentionally textureless sky
		// - but in both cases, falling all the way back to "no sky at all"
		// (leaving the Vulkan clear colour, pure black) reads far worse than
		// this actually looks in vanilla: those levels still show a lit,
		// foggy backdrop, not a black void. A flat neutral overcast-gray box
		// is a much closer approximation than black, even though it isn't
		// the shader's real colour.
		// Sky faces are drawn through the same world.frag as everything else,
		// paired with s_whiteLightmap (full white) - see the *2.0 "overbright
		// bits" approximation there. That doubles this image's colour before
		// it hits the screen, so halve the intended on-screen grey (~140,150,
		// 160) here or it clips to solid white, same mistake as the first
		// attempt at this fallback.
		static image_t *s_skyFallbackFace = nullptr;
		if ( !s_skyFallbackFace )
		{
			s_skyFallbackFace = VK_CreateSolidImage( "*skyFallback", 70, 75, 80, 255 );
		}
		for ( int i = 0; i < 6; i++ )
		{
			faces[i] = s_skyFallbackFace;
		}
	}

	// Must be well inside [zNear, zFar] (see VK_BuildProjectionMatrix) so the
	// box is never near-or-far-plane clipped regardless of view direction;
	// its actual size is otherwise irrelevant since it's always camera-
	// centered, so a fixed constant well clear of both planes is fine.
	const float boxSize = 2048.0f;
	static const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };

	std::vector<WorldVertex> cpuVerts;
	std::vector<uint32_t> cpuIndexes;
	cpuVerts.reserve( 24 );
	cpuIndexes.reserve( 36 );

	for ( int axis = 0; axis < 6; axis++ )
	{
		uint32_t vertBase = (uint32_t)cpuVerts.size();
		for ( int c = 0; c < 4; c++ )
		{
			float s = corners[c][0], t = corners[c][1];
			float b[3] = { s * boxSize, t * boxSize, boxSize };

			WorldVertex v = {};
			for ( int j = 0; j < 3; j++ )
			{
				int k = st_to_vec[axis][j];
				v.pos[j] = ( k < 0 ) ? -b[-k - 1] : b[k - 1];
			}
			// Bug found and fixed (see README.md): this used to stop at
			// (s+1)*0.5/(t+1)*0.5, dropping rd-vanilla's real MakeSkyVec's
			// (tr_sky.cpp) final `t = 1.0 - t` step - a real, separate V-flip
			// on top of the (t+1)*0.5 remap, not just a naming coincidence.
			// Without it the sky rendered upside down (tree-line texture
			// detail appearing near the top of each face instead of the
			// bottom).
			v.uv[0] = ( s + 1.0f ) * 0.5f;
			v.uv[1] = 1.0f - ( t + 1.0f ) * 0.5f;
			v.lightmapUV[0] = 0.0f;
			v.lightmapUV[1] = 0.0f;
			cpuVerts.push_back( v );
		}

		uint32_t firstIndex = (uint32_t)cpuIndexes.size();
		cpuIndexes.push_back( vertBase + 0 ); cpuIndexes.push_back( vertBase + 1 ); cpuIndexes.push_back( vertBase + 2 );
		cpuIndexes.push_back( vertBase + 0 ); cpuIndexes.push_back( vertBase + 2 ); cpuIndexes.push_back( vertBase + 3 );

		VkDescriptorSet descriptorSet = VK_BuildWorldDescriptorSet( vk.worldDescriptorPool, faces[axis], s_whiteLightmap );
		s_skyFaces.push_back( { descriptorSet, firstIndex, 6u, { 0, 0, 0 }, { 0, 0, 0 } } );
	}

	VK_UploadDeviceLocalBuffer( cpuVerts.data(), cpuVerts.size() * sizeof( WorldVertex ),
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s_skyVertexBuffer, &s_skyVertexBufferMemory );
	VK_UploadDeviceLocalBuffer( cpuIndexes.data(), cpuIndexes.size() * sizeof( uint32_t ),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &s_skyIndexBuffer, &s_skyIndexBufferMemory );

	s_skyLoaded = true;
	ri.Printf( PRINT_ALL, "rd-vulkan: loaded sky '%s'%s\n", baseName,
		allFacesFound ? "" : " (using flat fallback colour, real faces not found)" );
}

// Loads this map's global fog volume, if any - the BSP's LUMP_FOGS entry
// with brushNum == -1 (rd-vanilla's own R_LoadFogs/tr_bsp.cpp convention for
// "not bounded to a specific brush, covers the whole map" - confirmed by
// reading that function directly, not guessed), naming a shader whose
// fogparms line gives the actual colour/opaque distance (see
// VK_GetShaderFogParms, tr_shader.cpp). Per-brush local fog volumes (a fog
// entry with a real brushNum, bounded to one convex region) are not
// implemented - see README.md; this renderer only supports one fog, applied
// uniformly to the whole world, which is the common case for outdoor/
// weather levels like hoth2 that wrap the entire map in one fog brush.
static void VK_LoadWorldFog( const byte *fogData, int fogDataLen )
{
	int numFogs = fogDataLen / (int)sizeof( dfog_t );
	const dfog_t *fogs = (const dfog_t *)fogData;

	for ( int i = 0; i < numFogs; i++ )
	{
		if ( fogs[i].brushNum != -1 )
		{
			continue;
		}

		float color[3];
		float opaqueDist;
		if ( VK_GetShaderFogParms( fogs[i].shader, color, &opaqueDist ) )
		{
			s_worldFogColor[0] = color[0];
			s_worldFogColor[1] = color[1];
			s_worldFogColor[2] = color[2];
			s_worldFogOpaqueDist = opaqueDist;
			s_worldFogEnabled = true;
			ri.Printf( PRINT_ALL, "rd-vulkan: loaded global fog '%s' colour (%.2f %.2f %.2f) opaque dist %.0f\n",
				fogs[i].shader, color[0], color[1], color[2], opaqueDist );
		}
		return; // rd-vanilla's own R_LoadFogs only allows one global fog per map
	}
}

// Fixed subdivision level for MST_PATCH tessellation (see VK_TessellatePatchQuad
// below) - a deliberate first-pass simplification of rd-vanilla's real
// R_SubdividePatchToGrid (tr_curve.cpp), which adaptively subdivides only as
// much as a curve's actual curvature needs (checked against the r_subdivisions
// cvar's error tolerance, in world units). This always subdivides every patch
// to the same fixed 8x8-quad resolution regardless of size or flatness -
// visually fine (a flat "curve" just tessellates into coplanar quads, wasted
// but not wrong) at the cost of some avoidable triangle overdraw on large or
// nearly-flat patches. Same "simplify the algorithm, keep the math faithful"
// tradeoff as the flat (non-subdivided) skybox box elsewhere in this file.
static const int PATCH_SUBDIVISIONS = 8;

// Evaluates a single biquadratic Bezier "sub-patch" (3x3 control points) at a
// fixed (PATCH_SUBDIVISIONS+1)^2 grid of parameter values and appends the
// resulting vertices/triangles to cpuVerts/cpuIndexes, updating mins/maxs as
// it goes. The math itself - not just the position, but interpolating a
// vertex's UV and lightmap UV through the exact same weights - is the
// standard biquadratic Bezier surface formula (tensor product of two
// quadratic Bernstein bases), the same curve family rd-vanilla's recursive
// midpoint-bisection (LerpDrawVert-based) subdivision in tr_curve.cpp
// produces - quadratic Bezier subdivision is exact, not approximate, so a
// closed-form basis-function evaluation at a fixed parameter grid traces
// precisely the same surface, just sampled at fixed rather than adaptive
// density. ctrl is indexed [row][col], matching R_SubdividePatchToGrid's own
// ctrl[j][i] = points[j*width+i] convention (row = height axis, col = width
// axis) - see this function's only caller for how a BSP patch's flat
// patchWidth*patchHeight control array maps into a 3x3 ctrl for each
// sub-patch.
static void VK_TessellatePatchQuad( const WorldVertex ctrl[3][3], int level,
	std::vector<WorldVertex> &cpuVerts, std::vector<uint32_t> &cpuIndexes,
	float mins[3], float maxs[3] )
{
	uint32_t vertBase = (uint32_t)cpuVerts.size();
	int stride = level + 1;

	for ( int row = 0; row <= level; row++ )
	{
		float v = (float)row / (float)level;
		float bv[3] = { ( 1 - v ) * ( 1 - v ), 2 * v * ( 1 - v ), v * v };
		for ( int col = 0; col <= level; col++ )
		{
			float u = (float)col / (float)level;
			float bu[3] = { ( 1 - u ) * ( 1 - u ), 2 * u * ( 1 - u ), u * u };

			WorldVertex out = {};
			for ( int i = 0; i < 3; i++ )
			{
				for ( int j = 0; j < 3; j++ )
				{
					float w = bv[i] * bu[j];
					const WorldVertex &p = ctrl[i][j];
					out.pos[0] += w * p.pos[0];
					out.pos[1] += w * p.pos[1];
					out.pos[2] += w * p.pos[2];
					out.uv[0] += w * p.uv[0];
					out.uv[1] += w * p.uv[1];
					out.lightmapUV[0] += w * p.lightmapUV[0];
					out.lightmapUV[1] += w * p.lightmapUV[1];
				}
			}

			for ( int k = 0; k < 3; k++ )
			{
				if ( out.pos[k] < mins[k] ) mins[k] = out.pos[k];
				if ( out.pos[k] > maxs[k] ) maxs[k] = out.pos[k];
			}
			cpuVerts.push_back( out );
		}
	}

	// Two triangles per quad cell of the tessellated grid. Winding is not
	// verified against rd-vanilla's own convention - harmless since world
	// geometry draws with VK_CULL_MODE_NONE (see VK_CreateWorldPipeline's
	// "no culling" comment in tr_init.cpp), so both winding directions
	// render regardless.
	for ( int row = 0; row < level; row++ )
	{
		for ( int col = 0; col < level; col++ )
		{
			uint32_t i0 = vertBase + row * stride + col;
			uint32_t i1 = vertBase + row * stride + col + 1;
			uint32_t i2 = vertBase + ( row + 1 ) * stride + col;
			uint32_t i3 = vertBase + ( row + 1 ) * stride + col + 1;
			cpuIndexes.push_back( i0 ); cpuIndexes.push_back( i2 ); cpuIndexes.push_back( i1 );
			cpuIndexes.push_back( i1 ); cpuIndexes.push_back( i2 ); cpuIndexes.push_back( i3 );
		}
	}
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

	VK_LoadLightmaps( lumpData( LUMP_LIGHTMAPS ), header->lumps[LUMP_LIGHTMAPS].filelen );
	VK_LoadWorldFog( lumpData( LUMP_FOGS ), header->lumps[LUMP_FOGS].filelen );

	// This renderer doesn't parse .shader `skyparms` (see README.md's notes
	// on .shader script scope), so it uses the sky-flagged shader's own name
	// as the skybox basename directly - matches the common case (e.g.
	// academy1's textures/skies/yavin, whose _rt/_lf/.../_dn faces are named
	// after the shader itself with no skyparms override needed) but would
	// miss a level whose .shader script points skyparms at a differently-
	// named basename. First sky shader found wins; a BSP normally has one.
	for ( int i = 0; i < numShaders; i++ )
	{
		if ( shaders[i].surfaceFlags & SURF_SKY )
		{
			VK_LoadSky( shaders[i].shader );
			break;
		}
	}

	std::vector<WorldVertex> cpuVerts( (size_t)numVerts );
	for ( int i = 0; i < numVerts; i++ )
	{
		cpuVerts[i].pos[0] = verts[i].xyz[0];
		cpuVerts[i].pos[1] = verts[i].xyz[1];
		cpuVerts[i].pos[2] = verts[i].xyz[2];
		cpuVerts[i].uv[0] = verts[i].st[0];
		cpuVerts[i].uv[1] = verts[i].st[1];
		// Lightmap UV set 0 - the other 3 (deluxe maps / secondary styles)
		// aren't used by this renderer, see WorldVertex's comment.
		cpuVerts[i].lightmapUV[0] = verts[i].lightmap[0][0];
		cpuVerts[i].lightmapUV[1] = verts[i].lightmap[0][1];
	}

	std::vector<uint32_t> cpuIndexes;
	cpuIndexes.reserve( (size_t)numIndexes );

	for ( int i = 0; i < numSurfaces; i++ )
	{
		const dsurface_t &surf = surfaces[i];
		// Flares (MST_FLARE) need their own draw path, not implemented yet -
		// skip rather than draw garbage geometry from their raw data. Patches
		// (MST_PATCH, curved surfaces) ARE handled below, tessellated via
		// VK_TessellatePatchQuad - see that function's comment.
		bool isPatch = surf.surfaceType == MST_PATCH;
		if ( !isPatch && surf.surfaceType != MST_PLANAR && surf.surfaceType != MST_TRIANGLE_SOUP )
		{
			continue;
		}
		if ( surf.shaderNum < 0 || surf.shaderNum >= numShaders )
		{
			continue;
		}
		if ( isPatch )
		{
			// A patch's numVerts should be exactly patchWidth*patchHeight (its
			// flat control-point array) - guard against malformed/truncated
			// data rather than reading past the surface's own vertex range.
			// Must be at least a single 3x3 sub-patch (a lone control point
			// or a 1-wide/1-tall strip can't form one).
			if ( surf.patchWidth < 3 || surf.patchHeight < 3 ||
				surf.numVerts < surf.patchWidth * surf.patchHeight )
			{
				continue;
			}
		}
		else if ( surf.numIndexes <= 0 )
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

		image_t *lightmap = s_whiteLightmap;
		int lightmapNum = surf.lightmapNum[0];
		if ( lightmapNum >= 0 && (size_t)lightmapNum < s_lightmapImages.size() )
		{
			lightmap = s_lightmapImages[lightmapNum];
		}

		uint32_t firstIndex = (uint32_t)cpuIndexes.size();
		// AABB from the generated/surface vertex range, for view-frustum
		// culling in RE_RenderScene - cheap to compute once here vs.
		// re-deriving it (or worse, testing every vertex) every frame.
		float mins[3] = { 1e30f, 1e30f, 1e30f };
		float maxs[3] = { -1e30f, -1e30f, -1e30f };

		if ( isPatch )
		{
			// Decompose the flat patchWidth*patchHeight control array into
			// its (patchWidth-1)/2 x (patchHeight-1)/2 overlapping 3x3
			// sub-patches (adjacent sub-patches share an edge row/column of
			// control points, the standard Quake3 "biquadratic patch mesh"
			// convention) and tessellate each independently - see
			// VK_TessellatePatchQuad. cpuVerts is captured into a local
			// ctrl[3][3] by value before any push_back, so the reference
			// stays valid even though VK_TessellatePatchQuad appends to the
			// same vector it's reading control points out of.
			int numPatchesX = ( surf.patchWidth - 1 ) / 2;
			int numPatchesY = ( surf.patchHeight - 1 ) / 2;
			for ( int py = 0; py < numPatchesY; py++ )
			{
				for ( int px = 0; px < numPatchesX; px++ )
				{
					WorldVertex ctrl[3][3];
					for ( int ci = 0; ci < 3; ci++ )
					{
						for ( int cj = 0; cj < 3; cj++ )
						{
							int gx = px * 2 + cj;
							int gy = py * 2 + ci;
							ctrl[ci][cj] = cpuVerts[surf.firstVert + gy * surf.patchWidth + gx];
						}
					}
					VK_TessellatePatchQuad( ctrl, PATCH_SUBDIVISIONS, cpuVerts, cpuIndexes, mins, maxs );
				}
			}
		}
		else
		{
			for ( int j = 0; j < surf.numIndexes; j++ )
			{
				// BSP drawIndexes are surface-local (0-based within the
				// surface's own [firstVert, firstVert+numVerts) range) -
				// offset by firstVert to get indices into the single
				// combined vertex buffer this renderer uploads for the
				// whole world.
				cpuIndexes.push_back( (uint32_t)( surf.firstVert + indexes[surf.firstIndex + j] ) );
			}
			for ( int v = 0; v < surf.numVerts; v++ )
			{
				const float *p = cpuVerts[surf.firstVert + v].pos;
				for ( int k = 0; k < 3; k++ )
				{
					if ( p[k] < mins[k] ) mins[k] = p[k];
					if ( p[k] > maxs[k] ) maxs[k] = p[k];
				}
			}
		}

		VkDescriptorSet descriptorSet = VK_BuildWorldDescriptorSet( vk.worldDescriptorPool, img, lightmap );
		s_worldSurfaces.push_back( { descriptorSet, firstIndex, (uint32_t)( cpuIndexes.size() - firstIndex ),
			{ mins[0], mins[1], mins[2] }, { maxs[0], maxs[1], maxs[2] } } );
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

	ri.Printf( PRINT_ALL, "rd-vulkan: loaded %s: %d draw batches, %d verts, %d indexes, %d lightmaps (skipped flares)\n",
		name, (int)s_worldSurfaces.size(), (int)cpuVerts.size(), (int)cpuIndexes.size(), (int)s_lightmapImages.size() );
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
// sites and why each argument order is what it is. Not static - tr_model.cpp
// reuses it for entity model matrices rather than rederiving the same
// easy-to-get-backwards math, see tr_local.h.
void VK_MultiplyMatrix( const float *a, const float *b, float *out )
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

// Standard Gribb-Hartmann plane extraction from a combined view-projection
// matrix, adjusted for Vulkan's [0,1] depth-clip range instead of GL's
// [-1,1] (only the near plane's formula differs between the two - see the
// comment inline below). Each plane is (a,b,c,d) such that a point is
// "inside" when a*x + b*y + c*z + d >= 0. mvp is column-major (data[col*4+
// row], the same convention VK_BuildProjectionMatrix/VK_MultiplyMatrix use
// throughout this file), so "row i" of the matrix is { mvp[i], mvp[4+i],
// mvp[8+i], mvp[12+i] }, not four consecutive elements.
static void VK_ExtractFrustumPlanes( const float *mvp, float planes[6][4] )
{
	float row0[4] = { mvp[0], mvp[4], mvp[8], mvp[12] };
	float row1[4] = { mvp[1], mvp[5], mvp[9], mvp[13] };
	float row2[4] = { mvp[2], mvp[6], mvp[10], mvp[14] };
	float row3[4] = { mvp[3], mvp[7], mvp[11], mvp[15] };

	for ( int i = 0; i < 4; i++ ) planes[0][i] = row3[i] + row0[i]; // left
	for ( int i = 0; i < 4; i++ ) planes[1][i] = row3[i] - row0[i]; // right
	for ( int i = 0; i < 4; i++ ) planes[2][i] = row3[i] + row1[i]; // bottom
	for ( int i = 0; i < 4; i++ ) planes[3][i] = row3[i] - row1[i]; // top
	// Near: Vulkan's clip-space z ranges [0,1] (not GL's [-1,1]), and z_clip
	// >= 0 is exactly row2.v >= 0 - no row3 term needed, unlike GL's near
	// plane (row3+row2) or Vulkan's own far plane (row3-row2) below.
	for ( int i = 0; i < 4; i++ ) planes[4][i] = row2[i];           // near
	for ( int i = 0; i < 4; i++ ) planes[5][i] = row3[i] - row2[i]; // far
}

// True if the AABB is entirely on the outside of at least one frustum plane
// (a real intersection/inside case returns false, including partial overlap
// - this is a conservative "definitely not visible" test, not an exact
// visibility test, which is exactly what culling needs: false negatives
// here would incorrectly hide visible geometry, false positives just mean
// an off-screen box gets submitted anyway, harmless).
static bool VK_AABBOutsideFrustum( const float mins[3], const float maxs[3], const float planes[6][4] )
{
	for ( int i = 0; i < 6; i++ )
	{
		const float *p = planes[i];
		// The AABB corner most positive along this plane's normal - if even
		// that corner is outside, every other corner is too.
		float x = ( p[0] >= 0 ) ? maxs[0] : mins[0];
		float y = ( p[1] >= 0 ) ? maxs[1] : mins[1];
		float z = ( p[2] >= 0 ) ? maxs[2] : mins[2];
		if ( p[0] * x + p[1] * y + p[2] * z + p[3] < 0 )
		{
			return true;
		}
	}
	return false;
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

	// Sky, drawn first with depth test/write both off (vk.skyPipeline) so it
	// never occludes or is occluded by anything - normal depth-tested world
	// geometry drawn afterward naturally overdraws it wherever real geometry
	// exists. Uses a translation-stripped view matrix (origin forced to 0,
	// same rotation) so the box is always camera-centered regardless of
	// player position, i.e. reads as infinitely distant.
	if ( s_skyLoaded )
	{
		refdef_t skyFd = *fd;
		VectorClear( skyFd.vieworg );
		float skyView[16], skyMvp[16];
		VK_BuildViewMatrix( &skyFd, skyView );
		VK_MultiplyMatrix( skyView, projMatrix, skyMvp );

		if ( vk.skyPipeline != vk.lastBoundPipeline )
		{
			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.skyPipeline );
			vk.lastBoundPipeline = vk.skyPipeline;
		}

		// Sky never gets fogged (fogColor.a = 0 disables the mix in
		// world.frag) - it's meant to read as infinitely distant, and this
		// renderer's flat-colour fog fallback for farbox-less skies (see
		// VK_LoadSky) already approximates the same "hazy backdrop" look
		// fog would otherwise add here. camPos is irrelevant with fog off,
		// left zeroed. Push the whole struct, not just mvp - an undersized
		// push leaves camPos/fogColor holding whatever a previous draw call
		// in this command buffer wrote there (Vulkan push constants persist
		// across draws until overwritten), not zero.
		vkWorldPushConstants_t skyPush = {};
		memcpy( skyPush.mvp, skyMvp, sizeof( skyMvp ) );
		vkCmdPushConstants( cmd, vk.worldPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			0, sizeof( skyPush ), &skyPush );

		VkDeviceSize skyVertexOffset = 0;
		vkCmdBindVertexBuffers( cmd, 0, 1, &s_skyVertexBuffer, &skyVertexOffset );
		vkCmdBindIndexBuffer( cmd, s_skyIndexBuffer, 0, VK_INDEX_TYPE_UINT32 );

		for ( const WorldSurfaceBatch &face : s_skyFaces )
		{
			vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.worldPipelineLayout,
				0, 1, &face.descriptorSet, 0, nullptr );
			vkCmdDrawIndexed( cmd, face.indexCount, 1, face.firstIndex, 0, 0 );
		}
	}

	if ( vk.worldPipeline != vk.lastBoundPipeline )
	{
		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.worldPipeline );
		vk.lastBoundPipeline = vk.worldPipeline;
	}

	vkWorldPushConstants_t worldPush = {};
	memcpy( worldPush.mvp, mvp, sizeof( mvp ) );
	worldPush.camPos[0] = fd->vieworg[0];
	worldPush.camPos[1] = fd->vieworg[1];
	worldPush.camPos[2] = fd->vieworg[2];
	if ( s_worldFogEnabled )
	{
		worldPush.fogColor[0] = s_worldFogColor[0];
		worldPush.fogColor[1] = s_worldFogColor[1];
		worldPush.fogColor[2] = s_worldFogColor[2];
		worldPush.fogColor[3] = s_worldFogOpaqueDist;
	}
	vkCmdPushConstants( cmd, vk.worldPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof( worldPush ), &worldPush );

	VkDeviceSize vertexOffset = 0;
	vkCmdBindVertexBuffers( cmd, 0, 1, &s_worldVertexBuffer, &vertexOffset );
	vkCmdBindIndexBuffer( cmd, s_worldIndexBuffer, 0, VK_INDEX_TYPE_UINT32 );

	float frustumPlanes[6][4];
	VK_ExtractFrustumPlanes( mvp, frustumPlanes );

	static int s_debugCullLogsRemaining = 3;
	int culledCount = 0;

	for ( const WorldSurfaceBatch &batch : s_worldSurfaces )
	{
		if ( VK_AABBOutsideFrustum( batch.mins, batch.maxs, frustumPlanes ) )
		{
			culledCount++;
			continue;
		}
		vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.worldPipelineLayout,
			0, 1, &batch.descriptorSet, 0, nullptr );
		vkCmdDrawIndexed( cmd, batch.indexCount, 1, batch.firstIndex, 0, 0 );
	}

	if ( s_debugCullLogsRemaining > 0 )
	{
		s_debugCullLogsRemaining--;
		ri.Printf( PRINT_ALL, "rd-vulkan: frustum culling: %d/%d world batches culled\n",
			culledCount, (int)s_worldSurfaces.size() );
	}

	// Ghoul2 entities (tr_model.cpp) - drawn while the scene's 3D viewport/
	// scissor (set above) is still active, same mvp (entities apply their own
	// model matrix on top of it, see VK_DrawGhoul2Entities). fd->time drives
	// each entity's live animation frame (VK_GetGhoul2PoseFrame).
	VK_DrawGhoul2Entities( mvp, fd->time );

	// Restore the full-screen viewport/scissor for any 2D drawing
	// (RE_StretchPic) that follows this scene render within the same frame -
	// it doesn't set its own, it relies on whatever RE_BeginFrame or the
	// last RE_RenderScene left bound.
	VkViewport fullViewport = { 0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.0f, 1.0f };
	VkRect2D fullScissor = { { 0, 0 }, vk.swapchainExtent };
	vkCmdSetViewport( cmd, 0, 1, &fullViewport );
	vkCmdSetScissor( cmd, 0, 1, &fullScissor );
}
