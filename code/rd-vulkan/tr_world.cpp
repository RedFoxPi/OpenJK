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
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
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
	// True if this surface's dsurface_t.lightmapNum was < 0 (LIGHTMAP_BY_
	// VERTEX/no lightmap at all - see s_whiteLightmap's comment) - hoth2's
	// snow/terrain shaders are the confirmed real case (shaders/hoth.shader:
	// `q3map_nolightmap` + `rgbGen vertex`, dsurface_t.lightmapNum == -3 in
	// the actual BSP). RE_RenderScene needs this per-batch, not just at load
	// time, to pick the right overbright factor (world.frag's comment):
	// real baked lightmaps are compensated for Quake3's overbright-bits
	// doubling and need camPos.w=2.0, but a surface bound to s_whiteLightmap
	// has no such compensation baked in anywhere - multiplying by 2.0 on top
	// of that white "no-op" lightmap doubled the raw diffuse texture instead
	// of leaving it alone, blowing every vertex-lit surface out to solid
	// white and erasing its own texture detail. A real, confirmed bug (a
	// user directly compared hoth2's terrain against rd-vanilla's textured,
	// non-blown-out version of the same ground). Real per-vertex `rgbGen
	// vertex` colour (drawVert_t::color) is applied now too, for these same
	// vertex-lit surfaces specifically - see "Real per-vertex colour for
	// vertex-lit surfaces" in README.md and WorldVertex::color's own
	// comment (tr_local.h).
	bool vertexLit;
	// This surface's own dsurface_t.fogNum (see VK_LoadWorldFog), or -1 for
	// "not in any fog volume" - an index into s_worldFogs, not a bool/single
	// shared flag, since a map can carry more than one fog (a global one
	// plus any number of local per-brush ones, e.g. vjun1's ship-interior
	// `fog_black`) and different surfaces can each be in a different one,
	// or none at all. Matches rd-vanilla's own per-surface fogIndex exactly
	// - see VK_LoadWorldFog's comment for why trusting the BSP compiler's
	// own assignment here (rather than re-deriving "which fog volume is
	// this surface in" at runtime) is both simpler and more correct than
	// this renderer's previous behaviour of blanket-applying one global fog
	// to literally every surface regardless of what it was actually
	// compiled to be in.
	int fogIndex;
	// This surface's shader's first-stage `tcMod scroll` speed (UV units per
	// second), or 0,0 for the common no-scroll case - see
	// VK_GetShaderTcModScroll's comment (tr_shader.cpp) for the real test
	// case (vjun1's `textures/impdetention/deathcon1a` containment field,
	// 51 surfaces) that motivated this. RE_RenderScene multiplies this by
	// the current simulated time to get the actual per-frame UV offset -
	// storing the raw speed here, not a precomputed offset, is what lets
	// batches sharing the same speed still share one push-constant update
	// per frame (same pattern as vertexLit/fogIndex above).
	float scrollS, scrollT;
	// This surface's shader's first-stage blend mode (VK_GetShaderBlendMode,
	// tr_shader.cpp) - determines which of vk.worldPipeline/
	// worldPipelineAlpha/worldPipelineAdditive draws this batch (see
	// RE_RenderScene). Confirmed against real map data, not assumed: many
	// real, currently-visible surfaces across all four test maps declare a
	// non-opaque first stage (hoth2's `textures/flares/solid_blue`, 283
	// surfaces; yavin1's `textures/yavin/tree1` foliage, 24 surfaces; and
	// others) but were previously drawn through the single opaque pipeline
	// regardless, since this field didn't exist to tell RE_RenderScene
	// otherwise. s_worldSurfaces is sorted by this field after loading
	// (opaque batches first, so translucent ones draw depth-tested against
	// them) - see RE_LoadWorldMap's own sort call.
	vkBlendMode_t blendMode;
	// This surface's shader's first-stage `tcMod scale` multiplier, or
	// 1.0,1.0 (a true no-op) for the common no-scale case - see
	// VK_GetShaderTcModScroll's comment (tr_shader.cpp) for the real test
	// cases that motivated this and how it composes with scrollS/scrollT
	// above when a shader declares both. Deliberately the LAST field, with
	// default member initializers (not 0.0f) rather than inserted earlier
	// alongside scrollS/scrollT: every existing WorldSurfaceBatch
	// constructor call is positional aggregate init (sky faces, real BSP
	// surfaces) supplying exactly the old field count, so appending new
	// fields at the end - not in the middle - is what lets those call
	// sites keep compiling unchanged while correctly picking up the
	// default. 1.0f (not scrollS/scrollT's own correct-by-coincidence
	// 0.0f zero-init default) is required here specifically: 0.0f would
	// multiply every UV to (0,0), not leave it alone.
	float scaleS = 1.0f, scaleT = 1.0f;
	// This surface's shader's first-stage `tcGen environment` (real per-
	// vertex reflection-mapped UV generation, VK_GetShaderTcGenEnvironment,
	// tr_shader.cpp) - false (ordinary UVs) for the common case. Confirmed
	// real, substantial usage: 22 hoth2 surfaces (shiny metal walls, blast
	// panels, doors, the exit beam) and 6 vjun1 surfaces (security glass,
	// env_glass, the imperial square trim). When true, world.vert ignores
	// inUV/pc.uvScale entirely for this batch's vertices and instead
	// computes UVs from the vertex normal and camera position each frame
	// (RB_CalcEnvironmentTexCoords, rd-vanilla's tr_shade_calc.cpp) - see
	// WorldVertex::normal's own comment for where the normal comes from.
	// Deliberately appended last, same positional-aggregate-init reason as
	// scaleS/scaleT above.
	bool envMap = false;
	// This surface's shader's first-stage `alphaFunc` mode
	// (VK_GetShaderAlphaFunc, tr_shader.cpp) - 0 (no test, the common case)
	// for most surfaces. Confirmed real, substantial usage: 7 real yavin1
	// surfaces with NO `blendFunc` at all (grass/vines/tree foliage -
	// previously drawn fully opaque, solid rectangular sprites instead of
	// alpha-tested cutout shapes) plus several hoth2/vjun1 grate shaders.
	// See world.frag's real discard logic. Deliberately appended last, same
	// positional-aggregate-init reason as scaleS/scaleT/envMap above.
	int alphaFunc = 0;
	// This surface's shader's first-stage `tcMod turb` amplitude/phase/
	// frequency (VK_GetShaderTcModTurb, tr_shader.cpp) - amplitude 0.0 (a
	// true no-op, see vkWorldPushConstants_t's own comment) for the common
	// no-turb case. Confirmed real usage: vjun1's electric containment
	// field (`textures/impdetention/deathcon1a`/`deathcon1`, already real
	// for scroll/scale - this closes the remaining per-vertex wobble) and
	// a real water surface, plus yavin1's `cloudlayer_yavin`. Deliberately
	// appended last, same positional-aggregate-init reason as scaleS/
	// scaleT/envMap/alphaFunc above.
	float turbAmplitude = 0.0f, turbPhase = 0.0f, turbFrequency = 0.0f;
	// This surface's shader's first-stage real `depthWrite` override
	// (VK_GetShaderDepthWrite, tr_shader.cpp) - false (the default
	// depth-write-off-while-blended behaviour, see vk.worldPipelineAlpha's
	// own comment) for the common case. Confirmed real, if narrow, usage -
	// see s_shaderDepthWrite's own comment (tr_shader.cpp) for the exact
	// shaders and how each was confirmed. Only ever true for a BLEND_ALPHA
	// batch on this checkout's test maps (see
	// vk.worldPipelineAlphaDepthWrite's own comment for why there's no
	// additive equivalent) - RE_RenderScene's pipeline selection reflects
	// that. Deliberately appended last, same positional-aggregate-init
	// reason as scaleS/scaleT/envMap/alphaFunc/turb above.
	bool depthWrite = false;
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

// World fog (see VK_LoadWorldFog) - one entry per BSP LUMP_FOGS record,
// indexed directly by dsurface_t.fogNum (WorldSurfaceBatch::fogIndex),
// covering both the map's single global fog (a brushNum == -1 entry,
// rd-vanilla's own R_LoadFogs/worldData.globalFog convention - see
// tr_bsp.cpp) and any number of per-brush local fog volumes. opaqueDist of
// 0 means this entry's shader had no real `fogparms` (or wasn't found) -
// treated as "no fog" for any surface that references it, same as an
// unassigned (-1) fogIndex.
struct WorldFogEntry
{
	float color[3];
	float opaqueDist;
};
static std::vector<WorldFogEntry> s_worldFogs;
// Index into s_worldFogs for this map's single global fog, or -1 if it has
// none - kept separately since VK_HasWorldFog/GetWorldFogColor/
// SetWorldFogColor (tr_weather.cpp's fog-colour-override API) and "ranged
// fog" (VK_SetRangedFog below) both only ever apply to the map's own single
// global fog specifically, never a local one, matching rd-vanilla's real
// tr.world->globalFog-scoped behaviour exactly.
static int s_globalFogIndex = -1;

// Static flares (MST_FLARE surfaces - light-source glow sprites like
// hoth2's landing-beacon lights or vjun1's console/warning lights, not a
// dynamic light system) - see RE_LoadWorldMap's MST_FLARE branch for how
// this is populated from real BSP dsurface_t data (origin/normal - the
// third field ParseFlare reads, a precomputed colour tint, is deliberately
// not carried here; see the parsing site's own comment for why) and
// VK_DrawWorldFlares for how it's drawn every frame. image is never null in
// a stored entry - VK_FindImage failing is a reason not to store one, not
// something the draw loop needs to re-check.
// Always drawn additive (VK_DrawWorldFlares) - see the parsing site's own
// comment (RE_LoadWorldMap) for why no per-shader blend mode is stored here.
struct WorldFlare
{
	float origin[3];
	float normal[3];
	image_t *image;
	float radius;
	// This flare's own real shader name (surf.shaderNum's shaders[].shader,
	// not necessarily the same as image's own name - see
	// RE_LoadWorldMap's own comment on why a flare's image often resolves
	// through a different name entirely) - looked up once here, at load
	// time, so VK_DrawWorldFlares doesn't need to re-derive it every frame.
	// Used to resolve a real rgbGen const/wave override on top of the
	// default view-angle-fade colour below - see VK_DrawWorldFlares's own
	// comment for why most, but not all, real flare shaders need one.
	std::string shaderName;
};
static std::vector<WorldFlare> s_worldFlares;

// "Ranged fog" - a sniper-scope-style widening of the global fog's near/far
// transition distance, ported from rd-vanilla's real RE_SetRangedFog
// (tr_init.cpp) and the fStart/fEnd computation inside
// RB_IterateStagesGeneric (tr_shade.cpp) that consumes it. Real Quake3 fogs
// with an EXP2 (exponential-squared) curve by default and only switches to
// this LINEAR fStart/fEnd model while ranged fog is active - this renderer
// already uses a simplified linear ramp unconditionally (see world.vert's
// own comment), so "ranged fog" here just means shifting that ramp's start
// point away from 0 rather than switching curve shape. Two ways this gets
// set, both ported faithfully: a worldspawn `linFogStart` key (parsed once
// at map load, VK_LoadWorldspawnFogKeys - stored negated, matching
// rd-vanilla's own sign convention for "this specific value is a mapper
// override" vs. a positive value from the API call below) and
// VK_SetRangedFog itself (a real caller: cgame's zoom/scope code via
// RE_SetRangedFog, tr_init.cpp). Confirmed by direct BSP entity-string
// parsing that none of this renderer's four test maps declare a
// `linFogStart` key, and none of the static spawn-point screenshots this
// renderer is verified against would exercise a live call to
// VK_SetRangedFog either - implemented by reading rd-vanilla's real source
// faithfully, but genuinely unverified against real map/gameplay data. See
// README.md.
static float s_rangedFog = 0.0f;
static float s_oldRangedFog = 0.0f; // exact port of rd-vanilla's g_oldRangedFog save/restore quirk
// rd-vanilla's own real default (tr_bsp.cpp) before any worldspawn
// `distanceCull` key is read - used here only by the ranged-fog formula
// above, NOT to also change this renderer's own fixed projection zFar or
// view-frustum culling distance the way rd-vanilla's real tr.distanceCull
// does elsewhere (VK_BuildProjectionMatrix's zFar is a separate, already-
// documented constant - see that function's own comment) - a narrower
// scope than the real field, deliberately: this pass only implements
// distanceCull's effect on ranged fog, not the (much larger, unrelated)
// change to expand it into this renderer's culling/projection code too.
static float s_distanceCull = 12000.0f;

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
	s_worldFlares.clear();
	s_worldFogs.clear();
	s_globalFogIndex = -1;
	s_rangedFog = 0.0f;
	s_oldRangedFog = 0.0f;
	s_distanceCull = 12000.0f;
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
VkDescriptorSet VK_BuildWorldDescriptorSet( VkDescriptorPool pool, image_t *diffuse, image_t *lightmap, bool diffuseClamp )
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
	// Real `clampmap` addressing (VK_GetShaderClampMap's own comment,
	// tr_shader.cpp) - only the diffuse image ever needs this; the lightmap
	// slot always stays REPEAT (real vanilla never clamps `$lightmap`, and
	// lightmap UVs are baked within 0..1 anyway so it wouldn't matter if it
	// did).
	imageInfos[0].sampler = diffuseClamp ? vk.worldSamplerClamp : vk.worldSampler;
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
		// but - see the sky push constant's own comment (RE_RenderScene,
		// this file) - with overbright *disabled* (factor 1.0, not the 2.0
		// real BSP lightmaps need): rd-vanilla's own real sky draw
		// (DrawSkyBox, tr_sky.cpp) sets a flat `tr.identityLight` vertex
		// colour specifically to cancel overbright for the sky, so a real
		// farbox texture displays at its own natural brightness, not
		// doubled. This fallback colour is the intended on-screen grey
		// directly, not pre-halved to compensate for a doubling that no
		// longer happens - an earlier version of this renderer got that
		// backwards (paired the *2.0 rd-vanilla's own sky code deliberately
		// avoids with a pre-halved fallback colour to compensate), which
		// happened to still look reasonable for this synthetic fallback but
		// would have clipped any real map's actual sky texture to solid
		// white - exactly the "much brighter than rd-vanilla" symptom a user
		// directly reported.
		static image_t *s_skyFallbackFace = nullptr;
		if ( !s_skyFallbackFace )
		{
			s_skyFallbackFace = VK_CreateSolidImage( "*skyFallback", 140, 150, 160, 255 );
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
			// `WorldVertex v = {}` above zero-inits colour to black, not
			// white - see WorldVertex::color's own comment for why every
			// non-vertex-lit vertex (sky faces included) needs this set
			// explicitly rather than relying on a default.
			v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
			cpuVerts.push_back( v );
		}

		uint32_t firstIndex = (uint32_t)cpuIndexes.size();
		cpuIndexes.push_back( vertBase + 0 ); cpuIndexes.push_back( vertBase + 1 ); cpuIndexes.push_back( vertBase + 2 );
		cpuIndexes.push_back( vertBase + 0 ); cpuIndexes.push_back( vertBase + 2 ); cpuIndexes.push_back( vertBase + 3 );

		VkDescriptorSet descriptorSet = VK_BuildWorldDescriptorSet( vk.worldDescriptorPool, faces[axis], s_whiteLightmap );
		// vertexLit/fogIndex/scroll/blendMode are meaningless here - the sky
		// draw loop uses its own dedicated push constants (camPos.w=1.0,
		// fogColor.a=0, no scroll) and always vk.skyPipeline, never
		// s_worldSurfaces' per-batch values.
		s_skyFaces.push_back( { descriptorSet, firstIndex, 6u, { 0, 0, 0 }, { 0, 0, 0 }, false, -1, 0.0f, 0.0f, BLEND_OPAQUE } );
	}

	VK_UploadDeviceLocalBuffer( cpuVerts.data(), cpuVerts.size() * sizeof( WorldVertex ),
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s_skyVertexBuffer, &s_skyVertexBufferMemory );
	VK_UploadDeviceLocalBuffer( cpuIndexes.data(), cpuIndexes.size() * sizeof( uint32_t ),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &s_skyIndexBuffer, &s_skyIndexBufferMemory );

	s_skyLoaded = true;
	ri.Printf( PRINT_ALL, "rd-vulkan: loaded sky '%s'%s\n", baseName,
		allFacesFound ? "" : " (using flat fallback colour, real faces not found)" );
}

// Loads every one of this map's BSP LUMP_FOGS entries - both its single
// global fog (brushNum == -1, rd-vanilla's own R_LoadFogs/tr_bsp.cpp
// convention for "not bounded to a specific brush, covers the whole map" -
// confirmed by reading that function directly, not guessed) and any number
// of per-brush local fog volumes (a real brushNum, bounded to one convex
// region - e.g. vjun1's ship-interior `textures/fogs/fog_black`) - into
// s_worldFogs, indexed exactly as the BSP itself indexes them so
// dsurface_t.fogNum (WorldSurfaceBatch::fogIndex, set per-surface below in
// RE_LoadWorldMap) can look an entry up directly with no remapping needed.
//
// Deliberately does NOT re-derive a local fog's world-space bounds from its
// brush's planes the way rd-vanilla's real R_LoadFogs does (bounds[0]/
// bounds[1], read off the brush's first 6 - always axial-first by
// convention - sides): this renderer's fog is a simplified per-vertex
// distance-from-camera ramp (see world.vert's own comment), not a
// signed-distance-to-boundary-plane test, so it never needs to ask "is this
// point inside the volume" at draw time at all - trusting the BSP
// compiler's own per-surface fogNum assignment (confirmed by direct BSP
// parsing: every one of a map's surfaces already carries the fogNum of
// whichever single fog volume, local or global, it was compiled into) is
// both sufficient and exactly what determines which surfaces get fogged
// with which fog's colour in real Quake3 too - re-deriving that from brush
// geometry at load time would be redundant, not more correct.
static void VK_LoadWorldFog( const byte *fogData, int fogDataLen )
{
	int numFogs = fogDataLen / (int)sizeof( dfog_t );
	const dfog_t *fogs = (const dfog_t *)fogData;

	s_worldFogs.assign( (size_t)numFogs, WorldFogEntry{ { 0.0f, 0.0f, 0.0f }, 0.0f } );

	for ( int i = 0; i < numFogs; i++ )
	{
		float color[3];
		float opaqueDist;
		if ( !VK_GetShaderFogParms( fogs[i].shader, color, &opaqueDist ) )
		{
			continue;
		}
		s_worldFogs[i].color[0] = color[0];
		s_worldFogs[i].color[1] = color[1];
		s_worldFogs[i].color[2] = color[2];
		s_worldFogs[i].opaqueDist = opaqueDist;

		if ( fogs[i].brushNum == -1 )
		{
			s_globalFogIndex = i;
			ri.Printf( PRINT_ALL, "rd-vulkan: loaded global fog '%s' colour (%.2f %.2f %.2f) opaque dist %.0f\n",
				fogs[i].shader, color[0], color[1], color[2], opaqueDist );
		}
		else
		{
			ri.Printf( PRINT_ALL, "rd-vulkan: loaded local fog '%s' (brush %d) colour (%.2f %.2f %.2f) opaque dist %.0f\n",
				fogs[i].shader, fogs[i].brushNum, color[0], color[1], color[2], opaqueDist );
		}
	}
}

// Parses the map's worldspawn entity (BSP LUMP_ENTITIES's first entity
// block is always worldspawn - same convention rd-vanilla's own
// R_LoadEntities/tr_bsp.cpp relies on) for the two keys that feed this
// renderer's "ranged fog" - see s_rangedFog's own comment above for what
// that is and why it's implemented but unverified. `entityString` must be
// null-terminated - callers should NOT hand this the raw BSP lump pointer
// directly (the file continues past the lump's end with unrelated binary
// data, not guaranteed to be null-terminated at the boundary).
static void VK_LoadWorldspawnFogKeys( const char *entityString )
{
	s_distanceCull = 12000.0f;
	s_rangedFog = 0.0f;
	s_oldRangedFog = 0.0f;

	const char *text = entityString;
	COM_BeginParseSession();
	const char *token = COM_ParseExt( &text, qtrue );
	if ( strcmp( token, "{" ) != 0 )
	{
		COM_EndParseSession();
		return;
	}
	for ( ;; )
	{
		token = COM_ParseExt( &text, qtrue );
		if ( !token[0] || !strcmp( token, "}" ) )
		{
			break;
		}
		char key[MAX_QPATH];
		Q_strncpyz( key, token, sizeof( key ) );
		token = COM_ParseExt( &text, qtrue );
		if ( !token[0] )
		{
			break;
		}
		if ( !Q_stricmp( key, "distanceCull" ) )
		{
			s_distanceCull = (float)atof( token );
		}
		else if ( !Q_stricmp( key, "linFogStart" ) )
		{
			s_rangedFog = -(float)atof( token );
		}
	}
	COM_EndParseSession();
}

void VK_SetRangedFog( float dist )
{
	if ( s_rangedFog <= 0.0f )
	{
		s_oldRangedFog = s_rangedFog;
	}
	s_rangedFog = dist;
	if ( s_rangedFog == 0.0f && s_oldRangedFog )
	{
		s_rangedFog = s_oldRangedFog;
	}
}

// Returns the world-space distance before which fog should not apply yet
// (world.frag's fogStart.x) for one batch's fog - 0 (the common case, ramp
// starts right at the camera) unless this batch is in the map's single
// global fog AND ranged fog is active, exact port of the fStart half of
// rd-vanilla's real RB_IterateStagesGeneric (tr_shade.cpp) - see
// s_rangedFog's own comment for the full picture and why it's unverified.
static float VK_ComputeRangedFogStart( int fogIndex, float opaqueDist )
{
	if ( fogIndex != s_globalFogIndex || s_rangedFog == 0.0f )
	{
		return 0.0f;
	}
	float fStart = opaqueDist;
	if ( s_rangedFog < 0.0f )
	{
		fStart = -s_rangedFog;
		float fEnd = opaqueDist;
		if ( fStart >= fEnd )
		{
			fStart = fEnd - 1.0f;
		}
	}
	else if ( ( s_distanceCull - fStart ) < s_rangedFog )
	{
		fStart = s_distanceCull - s_rangedFog;
		if ( fStart < 16.0f )
		{
			fStart = 16.0f;
		}
	}
	return fStart;
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
					out.color[0] += w * p.color[0];
					out.color[1] += w * p.color[1];
					out.color[2] += w * p.color[2];
					out.color[3] += w * p.color[3];
					// No real confirmed patch surface uses tcGen environment
					// on this renderer's test maps (all 28 real matches are
					// planar wall/glass surfaces), but interpolating anyway
					// costs nothing and keeps patches consistent with every
					// other WorldVertex field here rather than silently
					// leaving this one at zero.
					out.normal[0] += w * p.normal[0];
					out.normal[1] += w * p.normal[1];
					out.normal[2] += w * p.normal[2];
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
	// std::string, not the raw lump pointer directly - see
	// VK_LoadWorldspawnFogKeys' own comment on why it needs a guaranteed
	// null terminator.
	std::string entityLumpStr( (const char *)lumpData( LUMP_ENTITIES ), (size_t)header->lumps[LUMP_ENTITIES].filelen );
	VK_LoadWorldspawnFogKeys( entityLumpStr.c_str() );

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
		// See WorldVertex::normal's own comment - always copied straight
		// from the BSP regardless of whether this vertex ends up on a
		// tcGen-environment surface.
		cpuVerts[i].normal[0] = verts[i].normal[0];
		cpuVerts[i].normal[1] = verts[i].normal[1];
		cpuVerts[i].normal[2] = verts[i].normal[2];
		// White (no-op) by default - overwritten below with this vertex's
		// real baked colour, but only for surfaces the per-surface loop
		// below determines are vertex-lit. See WorldVertex::color's own
		// comment for why every other vertex deliberately keeps this
		// hardcoded value instead of its own BSP-baked one.
		cpuVerts[i].color[0] = cpuVerts[i].color[1] = cpuVerts[i].color[2] = cpuVerts[i].color[3] = 1.0f;
	}

	std::vector<uint32_t> cpuIndexes;
	cpuIndexes.reserve( (size_t)numIndexes );

	for ( int i = 0; i < numSurfaces; i++ )
	{
		const dsurface_t &surf = surfaces[i];
		// Flares (MST_FLARE) have their own data shape - lightmapOrigin/
		// lightmapVecs[2] hold a world-space point+normal, not real vertex/
		// index geometry - so they're parsed into s_worldFlares here rather
		// than falling into the shared vertex/index path below (which every
		// other branch of this loop feeds). See VK_DrawWorldFlares
		// (RE_RenderScene's caller) for how they're actually drawn - real
		// per-pixel depth-tested camera-facing quads, ported from
		// rd-vanilla's real RB_SurfaceFlare (tr_surface.cpp), not a copy of
		// its own single-point RB_TestZFlare pre-test (see that function's
		// own comment for why this renderer doesn't need an equivalent).
		// ParseFlare's real third field (lightmapVecs[0], a precomputed
		// colour tint) is deliberately not read: rd-vanilla's own
		// RB_SurfaceFlare never uses it as an actual draw colour either - it
		// only overwrites color[]/[3] with its own view-angle fade
		// (`color[0]=color[1]=color[2]=d*255`) before ever considering
		// per-vertex colour, and that only matters for a shader explicitly
		// using `rgbGen exact_vertex` (or `alphaGen vertex` for alpha) -
		// neither appears on any of this checkout's 3 real flare shaders
		// (flares.shader/gfx.shader - see rd-vulkan/README.md).
		if ( surf.surfaceType == MST_FLARE )
		{
			if ( surf.shaderNum >= 0 && surf.shaderNum < numShaders &&
				!( shaders[surf.shaderNum].surfaceFlags & SURF_NODRAW ) )
			{
				image_t *flareImg = VK_FindImage( shaders[surf.shaderNum].shader );
				if ( !flareImg )
				{
					// Confirmed real and non-rare, not a hypothetical: of
					// this checkout's 3 real flare shaders, only
					// gfx/misc/flare's own name is directly a texture file -
					// textures/flares/flare_blue_pulse (hoth2) and
					// flare_bluehue (vjun1) both only resolve through their
					// first stage's own `map` (flares.shader), and are the
					// majority of real flare surfaces on both maps (55/98 on
					// hoth2, 29/45 on vjun1 - see rd-vulkan/README.md).
					// Unlike RE_LoadWorldMap's opaque-world-surface fallback
					// just below (gated to BLEND_OPAQUE specifically, see
					// its own comment for why), this one is unconditional:
					// a flare quad always draws through this renderer's own
					// dedicated alpha/additive poly pipeline, never the
					// opaque world one, so there's no equivalent "silently
					// wrong opaque wash" failure mode to gate against here.
					const char *mapImage = VK_GetShaderMapImage( shaders[surf.shaderNum].shader );
					if ( mapImage )
					{
						flareImg = VK_FindImage( mapImage );
					}
				}
				if ( flareImg )
				{
					WorldFlare flare;
					flare.origin[0] = surf.lightmapOrigin[0];
					flare.origin[1] = surf.lightmapOrigin[1];
					flare.origin[2] = surf.lightmapOrigin[2];
					flare.normal[0] = surf.lightmapVecs[2][0];
					flare.normal[1] = surf.lightmapVecs[2][1];
					flare.normal[2] = surf.lightmapVecs[2][2];
					flare.image = flareImg;
					// Always additive, regardless of what VK_GetShaderBlendMode
					// would classify this shader's own blendFunc as - not just
					// a default for the "no blendFunc keyword" case (unlike
					// ordinary world geometry's BLEND_OPAQUE default). Checked
					// against real data first: hoth2's flare_blue_pulse (55 of
					// its 98 real flare surfaces) declares `blendFunc GL_ONE
					// GL_ONE_MINUS_SRC_COLOR` - a softer "screen"-style
					// additive our binary blend-mode classifier can't
					// represent, so BlendFactorsToMode's fallback maps it to
					// BLEND_ALPHA instead. Tried that classified value first
					// and it was actively wrong, not just imprecise: standard
					// src-alpha blending an opaque-alpha glow texture whose
					// RGB has been faded near-black by this flare's own view-
					// angle fade (d, below) paints a solid black square over
					// the sky rather than a dim glow - confirmed directly in
					// a real hoth2 capture (rows of solid black boxes along
					// the horizon where the beacon flares are). Additive can't
					// produce that failure mode at any fade value (it only
					// ever brightens, never overwrites), and every one of
					// this checkout's 3 real flare shaders (flares.shader/
					// gfx.shader) is visually a glow effect - so additive is
					// the strictly-safer approximation for all of them, not
					// just the one (gfx/misc/flare) whose own blendFunc
					// already says so.
					flare.radius = VK_GetShaderPortalRange( shaders[surf.shaderNum].shader );
					flare.shaderName = shaders[surf.shaderNum].shader;
					s_worldFlares.push_back( flare );
				}
			}
			continue;
		}
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
		// See WorldSurfaceBatch::envMap's own comment - only ever set true
		// below, and only for the specific fallback case where the resolved
		// image genuinely came from a `tcGen environment` first stage (the
		// common case, where `img` above already resolved directly by the
		// shader's own name matching a real texture file, is drawing that
		// surface's ordinary *second*-stage base texture - real vanilla's
		// own composited look for these shaders - with correct ordinary
		// UVs already; forcing reflection UVs onto that image would be
		// wrong, not an improvement).
		bool envMap = false;
		if ( !img )
		{
			// A shader's own name is not reliably the same path as its real
			// base texture - see VK_GetShaderMapImage's comment (tr_shader.cpp)
			// for the confirmed real-world case that motivated this fallback
			// (hoth2's terrain, ~40% of its opaque surfaces silently dropped
			// before this fix, not a handful of edge cases). Gated on
			// BLEND_OPAQUE (no blendFunc keyword in the first stage) -
			// confirmed the hard way: an earlier version of this fallback
			// applied unconditionally and made academy1's
			// textures/common/dark_dust (`clampmap textures/common/gradient`,
			// `blendFunc GL_ONE GL_ONE`) resolve to a real image and cover
			// large parts of the screen as an opaque wash, exactly the
			// failure mode the comment below already warned about for
			// shaders with no `map` at all - a real shader that legitimately
			// has one is no safer to draw opaque if it's meant to be
			// additive/blended. Only surfaces whose shader is genuinely
			// opaque, just differently-named than its texture (hoth2's
			// vertex-lit terrain, not a translucent effect), take this path.
			// If the shader has no recorded map at all, or isn't opaque, we
			// fall back to "no direct image at all" - genuinely true for a
			// stages-only effect shader (fog/dust volumes, decals) built
			// entirely from its stages' own `map` references in a way this
			// renderer doesn't parse for world geometry (see README.md).
			// This renderer DOES have real blend pipelines for world
			// geometry now (see WorldSurfaceBatch::blendMode,
			// VK_CreateWorldPipeline) - the OPAQUE-only gate here stayed
			// deliberately narrower than that for a long time, after
			// checking real shaders that would otherwise take this path:
			// several (`textures/common/env_glass`, `.../glass_security_hex`)
			// use `tcGen environment` on their first stage - a reflection-
			// vector UV generation mode this renderer didn't implement at
			// all - so resolving their fallback `map` and sampling it with
			// the surface's own baked UV would render an actively wrong
			// static texture, not a translucent glass look. Left unresolved
			// rather than drawn wrong, same "invisible beats wrong" call as
			// the RE_RegisterShaderNoMip videologo fix in tr_image.cpp.
			//
			// Update: real per-vertex reflection UV generation for `tcGen
			// environment` IS now implemented (see WorldSurfaceBatch::envMap
			// below and world.vert) - the "actively wrong static texture"
			// problem the comment above describes no longer applies when
			// this specific fallback image came from a tcGen-environment
			// first stage, so that case is now explicitly let through
			// alongside the no-tcGen-at-all case. Any OTHER tcGen type
			// (`tcGen lightmap`/`tcGen vector`, neither with a single real
			// per-map match on this renderer's test maps as of this writing)
			// stays blocked exactly as before.
			//
			// Widened to also allow BLEND_ADDITIVE through, but ONLY when
			// the shader has no `tcGen` at all (VK_ShaderHasTcGen) OR its
			// tcGen is specifically `environment` - env_glass/
			// glass_security_* are ALSO additive-classified (their first
			// stage really is `blendFunc GL_ONE GL_ONE`, checked directly
			// against real shader data, not assumed different from the
			// dust-cloud shaders below), so blend mode alone can't be the
			// gate; tcGen presence is the actual root cause the comment
			// above already identified. Confirmed real, safe targets before
			// widening: `textures/common/dark_dust`/`tan_gradient`/
			// `dark_orange`/`blue_gradient` (all `clampmap textures/common/
			// gradient`, `blendFunc GL_ONE GL_ONE`, `rgbGen const (r g b)`,
			// no tcGen at all) - 71 real surfaces on academy1 alone via
			// dark_dust (see "World geometry blend modes" above), previously
			// invisible outright rather than just wrongly-blended.
			vkBlendMode_t fallbackBlendMode = VK_GetShaderBlendMode( shaders[surf.shaderNum].shader );
			bool fallbackTcGenEnvironment = VK_GetShaderTcGenEnvironment( shaders[surf.shaderNum].shader );
			bool safeForMapImageFallback = fallbackBlendMode == BLEND_OPAQUE ||
				( fallbackBlendMode == BLEND_ADDITIVE &&
					( !VK_ShaderHasTcGen( shaders[surf.shaderNum].shader ) || fallbackTcGenEnvironment ) );
			if ( safeForMapImageFallback )
			{
				const char *mapImage = VK_GetShaderMapImage( shaders[surf.shaderNum].shader );
				if ( mapImage )
				{
					img = VK_FindImage( mapImage );
					if ( img && fallbackTcGenEnvironment )
					{
						envMap = true;
					}
				}
			}
		}
		if ( !img )
		{
			continue;
		}

		image_t *lightmap = s_whiteLightmap;
		int lightmapNum = surf.lightmapNum[0];
		if ( lightmapNum >= 0 && (size_t)lightmapNum < s_lightmapImages.size() )
		{
			lightmap = s_lightmapImages[lightmapNum];
		}

		// Real baked per-vertex colour (drawVert_t::color, style 0) for
		// vertex-lit surfaces only - see WorldVertex::color's own comment
		// for why every other surface keeps the white default already set
		// above. Must happen before the isPatch/else block below, which
		// reads cpuVerts (directly for triangle-soup/planar surfaces, via
		// captured control points for patches) - both cases pick up the
		// real colour automatically as long as it's written here first.
		if ( lightmapNum < 0 )
		{
			for ( int v = 0; v < surf.numVerts; v++ )
			{
				const byte *c = verts[surf.firstVert + v].color[0];
				WorldVertex &wv = cpuVerts[surf.firstVert + v];
				wv.color[0] = c[0] / 255.0f;
				wv.color[1] = c[1] / 255.0f;
				wv.color[2] = c[2] / 255.0f;
				wv.color[3] = c[3] / 255.0f;
			}
		}

		// `rgbGen const (r g b)` (see VK_GetShaderRgbGenConst's own comment,
		// tr_shader.cpp) - a real shader-declared fixed tint, applied after
		// (and overriding) whatever the vertex-lit block above just set:
		// real rgbGen evaluation replaces the surface's colour source
		// entirely rather than layering with it, and this checkout's only
		// real users of `const` (the additive dust-cloud family the map-
		// image fallback above was just widened for) are all
		// `q3map_nolightmap` anyway, so there's no real baked colour this
		// would ever incorrectly discard. Alpha is left alone (real
		// `rgbGen const` only ever sets RGB) - the white-init default's 1.0
		// stays correct for these opaque-alpha, additive-blended shaders.
		float rgbConst[3];
		if ( VK_GetShaderRgbGenConst( shaders[surf.shaderNum].shader, rgbConst ) )
		{
			for ( int v = 0; v < surf.numVerts; v++ )
			{
				WorldVertex &wv = cpuVerts[surf.firstVert + v];
				wv.color[0] = rgbConst[0];
				wv.color[1] = rgbConst[1];
				wv.color[2] = rgbConst[2];
			}
		}

		// dsurface_t.fogNum directly indexes s_worldFogs (see VK_LoadWorldFog's
		// comment) - -1 means "not in any fog volume", and an out-of-range or
		// opaqueDist==0 entry (shader had no real fogparms) is treated the
		// same way, so RE_RenderScene only ever needs to check fogIndex >= 0.
		int fogIndex = -1;
		if ( surf.fogNum >= 0 && (size_t)surf.fogNum < s_worldFogs.size() && s_worldFogs[surf.fogNum].opaqueDist > 0.0f )
		{
			fogIndex = surf.fogNum;
		}

		// See WorldSurfaceBatch::scrollS/scrollT/scaleS/scaleT's own
		// comments - 0,0/1,1 (no lookup hit) is exactly correct for the
		// vast majority of shaders that never declare a tcMod scroll or
		// scale at all.
		float scrollS = 0.0f, scrollT = 0.0f, scaleS = 1.0f, scaleT = 1.0f;
		VK_GetShaderTcModScroll( shaders[surf.shaderNum].shader, &scrollS, &scrollT, &scaleS, &scaleT );

		// BLEND_OPAQUE, not VK_GetShaderBlendMode's own default
		// (BLEND_ALPHA, correct for the 2D UI path's bare-image case) - a
		// shader with no `.shader` script at all is the common case for an
		// ordinary wall/floor texture and must stay opaque, matching real
		// Quake3's own implicit-shader behaviour for world geometry. See
		// WorldSurfaceBatch::blendMode's own comment for the real surfaces
		// this affects.
		vkBlendMode_t blendMode = VK_GetShaderBlendMode( shaders[surf.shaderNum].shader, BLEND_OPAQUE );

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

		VkDescriptorSet descriptorSet = VK_BuildWorldDescriptorSet( vk.worldDescriptorPool, img, lightmap,
			VK_GetShaderClampMap( shaders[surf.shaderNum].shader ) );
		// See WorldSurfaceBatch::turbAmplitude's own comment - 0,0,0 (no
		// lookup hit) is exactly correct for the vast majority of shaders
		// that never declare a tcMod turb at all.
		float turbAmplitude = 0.0f, turbPhase = 0.0f, turbFrequency = 0.0f;
		VK_GetShaderTcModTurb( shaders[surf.shaderNum].shader, &turbAmplitude, &turbPhase, &turbFrequency );
		s_worldSurfaces.push_back( { descriptorSet, firstIndex, (uint32_t)( cpuIndexes.size() - firstIndex ),
			{ mins[0], mins[1], mins[2] }, { maxs[0], maxs[1], maxs[2] }, lightmapNum < 0, fogIndex,
			scrollS, scrollT, blendMode, scaleS, scaleT, envMap,
			VK_GetShaderAlphaFunc( shaders[surf.shaderNum].shader ),
			turbAmplitude, turbPhase, turbFrequency,
			VK_GetShaderDepthWrite( shaders[surf.shaderNum].shader ) } );
	}

	ri.FS_FreeFile( buffer );

	if ( cpuVerts.empty() || cpuIndexes.empty() )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: RE_LoadWorldMap: %s has no drawable static surfaces\n", name );
		return;
	}

	// Opaque batches first, then alpha, then additive - so RE_RenderScene's
	// single pass over s_worldSurfaces naturally draws all opaque geometry
	// (depth write on) before any translucent batch is depth-tested against
	// it, switching vk.worldPipeline/worldPipelineAlpha/worldPipelineAdditive
	// at most twice per frame rather than per-batch. A stable sort, not a
	// full depth sort between translucent surfaces themselves - see
	// vkGlobals_t::worldPipelineAlpha's own comment (tr_local.h) for why
	// that's an accepted simplification here, same as this renderer's
	// existing runtime-poly rendering. Reordering s_worldSurfaces itself is
	// safe: each batch's firstIndex/indexCount are offsets into the shared
	// vertex/index buffers uploaded below, independent of this vector's own
	// order.
	std::stable_sort( s_worldSurfaces.begin(), s_worldSurfaces.end(),
		[]( const WorldSurfaceBatch &a, const WorldSurfaceBatch &b )
		{
			auto rank = []( vkBlendMode_t m ) { return m == BLEND_OPAQUE ? 0 : ( m == BLEND_ALPHA ? 1 : 2 ); };
			if ( rank( a.blendMode ) != rank( b.blendMode ) )
			{
				return rank( a.blendMode ) < rank( b.blendMode );
			}
			// Secondary key: group real `depthWrite` batches (see
			// WorldSurfaceBatch::depthWrite's own comment) to the end of the
			// BLEND_ALPHA run, keeping RE_RenderScene's pipeline-switch count
			// close to its documented handful-per-frame bound instead of
			// scattering worldPipelineAlphaDepthWrite switches throughout it.
			return a.depthWrite < b.depthWrite;
		} );

	VK_UploadDeviceLocalBuffer( cpuVerts.data(), cpuVerts.size() * sizeof( WorldVertex ),
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s_worldVertexBuffer, &s_worldVertexBufferMemory );
	VK_UploadDeviceLocalBuffer( cpuIndexes.data(), cpuIndexes.size() * sizeof( uint32_t ),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &s_worldIndexBuffer, &s_worldIndexBufferMemory );

	s_worldLoaded = true;

	ri.Printf( PRINT_ALL, "rd-vulkan: loaded %s: %d draw batches, %d verts, %d indexes, %d lightmaps, %d flares\n",
		name, (int)s_worldSurfaces.size(), (int)cpuVerts.size(), (int)cpuIndexes.size(), (int)s_lightmapImages.size(),
		(int)s_worldFlares.size() );
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
	// depth precision-wise than rd-vanilla's per-frame computed zFar. This
	// was 4096 originally ("generous" for academy1's compact indoor
	// courtyard, the scene it was tuned against) but far too short for a
	// real open outdoor level: hoth2's BSP bounds span roughly 25400 units
	// corner-to-corner, so any camera more than 4096 units from a piece of
	// visible terrain - trivially true for most of an open snow field, not
	// an edge case - had that terrain silently far-plane-clipped every
	// frame, confirmed directly (raising this value made previously-missing
	// terrain reappear) while chasing a user report that hoth2 rendered as
	// almost entirely flat grey. 65536 comfortably covers hoth2's actual
	// measured worst case (camera at one corner, geometry at the opposite
	// one) with margin for other maps, without being large enough to cause
	// obviously bad depth-buffer precision for near geometry.
	const float zFar = 65536.0f;

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

// Draws every static MST_FLARE surface loaded by RE_LoadWorldMap - camera-
// facing quads, one draw call per contiguous same-image run
// (s_worldFlares isn't sorted for this, but real maps only ever use 1-2
// distinct flare shaders - see rd-vulkan/README.md - so this rarely batches
// worse than a full sort would buy). Ported from rd-vanilla's real
// RB_SurfaceFlare (tr_surface.cpp): same "push 3 units off the surface along
// its normal", same view-angle intensity fade
// (`d = -DotProduct(dir, normal)`), same distance-scaled radius clamped to a
// 5-unit floor. What's NOT ported is RB_TestZFlare, real rd-vanilla's own
// single-point glReadPixels-against-the-depth-buffer pre-test - deliberately
// so, not just skipped: vk.polyPipeline/polyPipelineAdditive (reused here
// wholesale, like tr_weather.cpp's particles) already render with depth-test
// on and depth-write off, so a flare occluded by a wall already fails the
// real per-pixel depth test the GPU performs anyway - a strictly
// finer-grained equivalent (whole-quad partial occlusion falls out for
// free) of what RB_TestZFlare's single sample point approximates, without
// needing a CPU-side readback at all. Not independently confirmed by eye
// against a real occluded flare in this checkout's own captures (none of
// the 4 test maps' fixed spawn cameras happens to frame a flare from behind
// an intervening wall) - the depth-test mechanism itself is standard Vulkan
// pipeline state already exercised correctly by every other draw call in
// this renderer, not new or flare-specific.
//
// The view-angle fade `d` is only the DEFAULT colour, not the final one for
// every real flare shader - a real, previously-mis-assumed gap this
// renderer's own earlier comments got wrong (see WorldFlare's own history):
// `RB_SurfaceFlare` writes `d` into the tessellation buffer's per-vertex
// colour input, but that's only what real rd-vanilla's `rgbGen vertex`
// actually reads back out - `rgbGen const`/`rgbGen wave` (real
// `RB_IterateStagesGeneric`/`RB_CalcWaveColor`, tr_shade.cpp/
// tr_shade_calc.cpp) *replace* the colour outright, discarding `d`
// entirely, regardless of what RB_SurfaceFlare wrote. Confirmed which of
// this checkout's 3 real flare shaders needs which: `gfx/misc/flare`
// declares `rgbGen vertex` (the `d`-fade default below is exactly correct
// for it), but `textures/flares/flare_blue_pulse` (55 of hoth2's 98 real
// flare surfaces) declares `rgbGen wave sin 0.5 1 0.2 0.5` and
// `textures/flares/flare_bluehue` (29 of vjun1's 45) declares `rgbGen
// const ( 0.784314 0.843137 0.917647 )` - both majority cases on their
// respective maps, both silently rendering the wrong (fade-based, not
// pulsing/tinted) colour before this fix.
void VK_DrawWorldFlares( const float *mvp, const refdef_t *fd )
{
	if ( s_worldFlares.empty() || !vk.frameActive )
	{
		return;
	}

	VkCommandBuffer cmd = vk.activeCommandBuffer;

	static const float cornerUv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	static const int winding[6] = { 0, 1, 2, 0, 2, 3 };

	uint32_t cursor = 0;
	image_t *lastImage = nullptr;
	uint32_t drawStart = 0;
	// For rgbGen wave's own time-varying formula below - same seconds-since-
	// map-start convention RE_RenderScene's own tcMod scroll offset already
	// uses.
	float timeSeconds = (float)fd->time / 1000.0f;

	auto flushDraw = [&]( uint32_t drawEnd )
	{
		if ( drawEnd <= drawStart || !lastImage )
		{
			return;
		}
		// Always additive - see WorldFlare's own comment for why every
		// flare uses this one pipeline regardless of its real shader's
		// blendFunc.
		if ( vk.polyPipelineAdditive != vk.lastBoundPipeline )
		{
			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.polyPipelineAdditive );
			vk.lastBoundPipeline = vk.polyPipelineAdditive;
		}
		vkCmdPushConstants( cmd, vk.polyPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof( float ) * 16, mvp );
		vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.polyPipelineLayout,
			0, 1, &lastImage->descriptorSet, 0, nullptr );
		VkDeviceSize offset = (VkDeviceSize)drawStart * sizeof( PolyVertex );
		vkCmdBindVertexBuffers( cmd, 0, 1, &vk.flareVertexBuffer, &offset );
		vkCmdDraw( cmd, drawEnd - drawStart, 1, 0, 0 );
	};

	for ( const WorldFlare &flare : s_worldFlares )
	{
		if ( cursor + 6 > FLARE_VERTEX_BUFFER_CAPACITY )
		{
			break;
		}

		float origin[3] = {
			flare.origin[0] + flare.normal[0] * 3.0f,
			flare.origin[1] + flare.normal[1] * 3.0f,
			flare.origin[2] + flare.normal[2] * 3.0f,
		};
		float dir[3] = { origin[0] - fd->vieworg[0], origin[1] - fd->vieworg[1], origin[2] - fd->vieworg[2] };
		float dist = sqrtf( dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2] );
		if ( dist > 0.0f )
		{
			dir[0] /= dist; dir[1] /= dist; dir[2] /= dist;
		}

		float d = -( dir[0] * flare.normal[0] + dir[1] * flare.normal[1] + dir[2] * flare.normal[2] );
		if ( d < 0.0f )
		{
			d = -d;
		}

		// Real rgbGen const/wave override, when this flare's own shader
		// declares one - see this function's own comment above for why
		// most, but not all, real flare shaders need this. Falls back to
		// the view-angle fade (d,d,d) computed above, correct for
		// `gfx/misc/flare`'s own `rgbGen vertex` and for any flare shader
		// with no rgbGen keyword at all.
		float colorR = d, colorG = d, colorB = d;
		float rgbConst[3];
		float waveBase, waveAmp, wavePhase, waveFreq;
		if ( VK_GetShaderRgbGenConst( flare.shaderName.c_str(), rgbConst ) )
		{
			colorR = rgbConst[0];
			colorG = rgbConst[1];
			colorB = rgbConst[2];
		}
		else if ( VK_GetShaderRgbWave( flare.shaderName.c_str(), &waveBase, &waveAmp, &wavePhase, &waveFreq ) )
		{
			// Real EvalWaveFormClamped/WAVEVALUE (tr_shade_calc.cpp):
			// base + amp*sin(2*pi*(phase + time*freq)), clamped to [0,1] -
			// not rederived, ported directly.
			float glow = waveBase + waveAmp * sinf( 2.0f * (float)M_PI * ( wavePhase + waveFreq * timeSeconds ) );
			if ( glow < 0.0f ) glow = 0.0f;
			if ( glow > 1.0f ) glow = 1.0f;
			colorR = colorG = colorB = glow;
		}

		float radius = flare.radius;
		if ( dist < 512.0f )
		{
			radius = radius * dist / 512.0f;
		}
		if ( radius < 5.0f )
		{
			radius = 5.0f;
		}

		if ( flare.image != lastImage )
		{
			flushDraw( cursor );
			drawStart = cursor;
			lastImage = flare.image;
		}

		float left[3] = { fd->viewaxis[1][0] * radius, fd->viewaxis[1][1] * radius, fd->viewaxis[1][2] * radius };
		float up[3] = { fd->viewaxis[2][0] * radius, fd->viewaxis[2][1] * radius, fd->viewaxis[2][2] * radius };
		float leftPlusUp[3] = { left[0] - up[0], left[1] - up[1], left[2] - up[2] };
		float leftMinusUp[3] = { left[0] + up[0], left[1] + up[1], left[2] + up[2] };

		float corners[4][3];
		for ( int i = 0; i < 3; i++ )
		{
			corners[0][i] = origin[i] - leftMinusUp[i];
			corners[1][i] = origin[i] - leftPlusUp[i];
			corners[2][i] = origin[i] + leftMinusUp[i];
			corners[3][i] = origin[i] + leftPlusUp[i];
		}

		PolyVertex *out = (PolyVertex *)vk.flareVertexBufferMapped + cursor;
		for ( int i = 0; i < 6; i++ )
		{
			int c = winding[i];
			out[i].pos[0] = corners[c][0];
			out[i].pos[1] = corners[c][1];
			out[i].pos[2] = corners[c][2];
			out[i].uv[0] = cornerUv[c][0];
			out[i].uv[1] = cornerUv[c][1];
			// colorR/G/B already resolved above (real rgbGen const/wave
			// override, or the view-angle fade default) - alphaGen still
			// isn't applied (see s_worldFlares' own comment), alpha stays
			// 1.0 so the texture's own alpha (a flare glow's real falloff
			// shape) survives the poly.frag modulate untouched.
			out[i].color[0] = colorR;
			out[i].color[1] = colorG;
			out[i].color[2] = colorB;
			out[i].color[3] = 1.0f;
		}
		cursor += 6;
	}
	flushDraw( cursor );
}

void RE_RenderScene( const refdef_t *fd )
{
	// RDF_SKYBOXPORTAL: real Quake3/JKA calls RE_RenderScene a *second* time
	// per frame with this flag set, from an entirely different camera placed
	// elsewhere in the map (a mapper-configured "portal sky" - a miniature
	// separate scene, e.g. distant mountains or a cityscape, meant to be
	// composited into just the sky/background of the real scene that's
	// rendered right after it - see RDF_SKYBOXPORTAL's own comment,
	// tr_types.h: "the [DRAWSKYBOX flag] says to draw it or not"). This
	// renderer doesn't implement that compositing (see README.md's "Proper
	// sky rendering" - no RDF_SKYBOXPORTAL support), but until this check
	// existed it didn't IGNORE the call either: it drew this second scene as
	// an entirely ordinary full opaque scene, straight into the same
	// framebuffer, with its own real world geometry from that unrelated
	// camera position. Confirmed the real, concrete cause of yavin1's
	// opening cockpit scene showing a hole full of unrelated outdoor jungle
	// terrain where a solid interior wall belongs: this portal call (from a
	// camera positioned out in that exact jungle) draws first every frame,
	// then gets mostly but not entirely painted over when the real cockpit
	// scene renders second immediately after - confirmed by a temporary
	// debug print (removed before committing) of both calls' real
	// `refdef_t` contents each frame, matching the real
	// RDF_SKYBOXPORTAL/RDF_DRAWSKYBOX bit values exactly. Skipping the call
	// outright - not attempting a translation this renderer couldn't
	// correctly composite into just a sky background anyway - leaves the
	// framebuffer exactly as it was before this call, which the
	// immediately-following real scene render (same frame, same target, its
	// own full opaque geometry and skybox) always completely overwrites
	// regardless - so there is no missing content, only a correctly-scoped
	// no-op instead of an actively wrong second scene. See README.md.
	if ( fd->rdflags & RDF_SKYBOXPORTAL )
	{
		return;
	}
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
		// fog would otherwise add here. camPos.xyz is irrelevant with fog
		// off, left zeroed - but camPos.w (the overbright factor, see
		// world.frag's comment) is NOT irrelevant, and is deliberately 1.0
		// here, NOT the 2.0 real BSP-lightmapped world geometry needs:
		// rd-vanilla's own real sky draw (DrawSkyBox, tr_sky.cpp) sets a flat
		// `tr.identityLight` vertex colour specifically to cancel overbright
		// for sky faces, so a farbox texture shows at its own natural
		// brightness rather than doubled - sky is paired with a white
		// "lightmap" purely so it can reuse this same descriptor-set/shader
		// plumbing (VK_LoadSky), not because it should be shaded like a real
		// baked-and-overbright-compensated lightmapped surface. Getting this
		// backwards silently clipped any map's real sky texture toward solid
		// white - a user-reported "Vulkan looks much brighter than
		// rd-vanilla" symptom, not a subtle one. Push the whole struct, not
		// just mvp - an undersized push leaves camPos/fogColor holding
		// whatever a previous draw call in this command buffer wrote there
		// (Vulkan push constants persist across draws until overwritten),
		// not zero.
		vkWorldPushConstants_t skyPush = {};
		memcpy( skyPush.mvp, skyMvp, sizeof( skyMvp ) );
		skyPush.camPos[3] = 1.0f;
		// Identity, not zero-init's 0,0 - see vkWorldPushConstants_t's own
		// comment (tr_local.h). Sky doesn't support tcMod scale (not a real
		// .shader-driven surface the same way BSP geometry is).
		skyPush.uvScale[0] = 1.0f;
		skyPush.uvScale[1] = 1.0f;
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
	// Common-case default (real baked lightmaps) - overridden per-batch
	// below for vertex-lit surfaces (WorldSurfaceBatch::vertexLit's
	// comment).
	worldPush.camPos[3] = 2.0f; // overbright factor - see world.frag's comment
	// fogColor/fogStart left zeroed (no fog) here - each batch's own fog
	// assignment, or lack of one, is applied per-batch in the draw loop
	// below via batch.fogIndex, matching real Quake3's per-surface
	// dsurface_t.fogNum rather than blanket-applying one fog to the whole
	// world regardless of what each surface actually compiled into (see
	// WorldSurfaceBatch::fogIndex's comment).
	// uvScale must be set explicitly to the identity (1,1), not left at
	// zero-init's 0,0 - see vkWorldPushConstants_t's own comment (tr_local.h)
	// for why a missed uvScale init is a silent wrong-texture bug, not a
	// crash.
	worldPush.uvScale[0] = 1.0f;
	worldPush.uvScale[1] = 1.0f;
	// uvScale.z is the tcGen-environment flag (see the per-batch loop below,
	// and vkWorldPushConstants_t's own comment) - 0.0 (ordinary UVs) is the
	// correct starting state here, same explicit-not-implicit-zero
	// discipline as uvScale.xy above.
	worldPush.uvScale[2] = 0.0f;
	// uvScale.w is this batch's alphaFunc mode (see the per-batch loop
	// below, and vkWorldPushConstants_t's own comment) - 0.0 (no test) is
	// the correct starting state here, same explicit-not-implicit-zero
	// discipline as uvScale.xy/z above.
	worldPush.uvScale[3] = 0.0f;
	// turb.xy is this batch's tcMod-turb amplitude/precomputed-now (see the
	// per-batch loop below, and vkWorldPushConstants_t's own comment) - 0,0
	// (amplitude 0 is a true no-op regardless of the second component) is
	// the correct starting state here, same explicit-not-implicit-zero
	// discipline as uvScale above.
	worldPush.turb[0] = 0.0f;
	worldPush.turb[1] = 0.0f;
	vkCmdPushConstants( cmd, vk.worldPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof( worldPush ), &worldPush );

	VkDeviceSize vertexOffset = 0;
	vkCmdBindVertexBuffers( cmd, 0, 1, &s_worldVertexBuffer, &vertexOffset );
	vkCmdBindIndexBuffer( cmd, s_worldIndexBuffer, 0, VK_INDEX_TYPE_UINT32 );

	float frustumPlanes[6][4];
	VK_ExtractFrustumPlanes( mvp, frustumPlanes );

	static int s_debugCullLogsRemaining = 3;
	int culledCount = 0;

	// worldPush.camPos[3] (overbright factor) already holds the common case
	// (2.0, real baked lightmaps) from the push issued above this loop, and
	// fogColor/fogStart already hold "no fog"/"no scroll" (all zero) - only
	// re-issue the push when a batch's vertexLit/fogIndex/scroll speed
	// actually differs from the last one drawn, rather than unconditionally
	// per-batch. See WorldSurfaceBatch::vertexLit/fogIndex/scrollS's own
	// comments for why they can't just share one fixed value across the
	// whole draw. fd->time (ms) drives the scroll offset - see
	// vkWorldPushConstants_t's own comment (tr_local.h) for why that
	// multiply happens here, once per distinct scroll speed, rather than in
	// world.vert every vertex.
	bool currentPushIsVertexLit = false;
	int currentFogIndex = -1;
	float currentScrollS = 0.0f, currentScrollT = 0.0f;
	float currentScaleS = 1.0f, currentScaleT = 1.0f;
	bool currentEnvMap = false;
	int currentAlphaFunc = 0;
	float currentTurbAmplitude = 0.0f, currentTurbPhase = 0.0f, currentTurbFrequency = 0.0f;
	float timeSeconds = (float)fd->time / 1000.0f;
	for ( const WorldSurfaceBatch &batch : s_worldSurfaces )
	{
		if ( VK_AABBOutsideFrustum( batch.mins, batch.maxs, frustumPlanes ) )
		{
			culledCount++;
			continue;
		}
		// s_worldSurfaces is sorted by blend mode (RE_LoadWorldMap), so this
		// switches at most a handful of times per frame in practice, not
		// per-batch - see WorldSurfaceBatch::blendMode's own comment. Real
		// `depthWrite` batches (WorldSurfaceBatch::depthWrite - see its own
		// comment) aren't separately sorted out from the rest of the
		// BLEND_ALPHA range, so on a map with one of the 3 real matches this
		// closes, expect one or two extra switches into/out of
		// worldPipelineAlphaDepthWrite rather than a strict two-total bound -
		// an accepted, minor cost for how rare real usage is, not worth a
		// second sort key over.
		VkPipeline pipeline = ( batch.blendMode == BLEND_ALPHA )
			? ( batch.depthWrite ? vk.worldPipelineAlphaDepthWrite : vk.worldPipelineAlpha )
			: ( batch.blendMode == BLEND_ADDITIVE ) ? vk.worldPipelineAdditive : vk.worldPipeline;
		if ( pipeline != vk.lastBoundPipeline )
		{
			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
			vk.lastBoundPipeline = pipeline;
		}
		if ( batch.vertexLit != currentPushIsVertexLit || batch.fogIndex != currentFogIndex ||
			batch.scrollS != currentScrollS || batch.scrollT != currentScrollT ||
			batch.scaleS != currentScaleS || batch.scaleT != currentScaleT ||
			batch.envMap != currentEnvMap || batch.alphaFunc != currentAlphaFunc ||
			batch.turbAmplitude != currentTurbAmplitude || batch.turbPhase != currentTurbPhase ||
			batch.turbFrequency != currentTurbFrequency )
		{
			currentPushIsVertexLit = batch.vertexLit;
			currentFogIndex = batch.fogIndex;
			currentScrollS = batch.scrollS;
			currentScrollT = batch.scrollT;
			currentScaleS = batch.scaleS;
			currentScaleT = batch.scaleT;
			currentEnvMap = batch.envMap;
			currentAlphaFunc = batch.alphaFunc;
			currentTurbAmplitude = batch.turbAmplitude;
			currentTurbPhase = batch.turbPhase;
			currentTurbFrequency = batch.turbFrequency;
			worldPush.camPos[3] = currentPushIsVertexLit ? 1.0f : 2.0f;
			worldPush.uvScale[0] = currentScaleS;
			worldPush.uvScale[1] = currentScaleT;
			// See vkWorldPushConstants_t's own comment (tr_local.h) - uvScale.z
			// doubles as the tcGen-environment flag for this batch (1.0 =
			// generate reflection UVs in world.vert, ignoring uvScale.xy/inUV
			// entirely; 0.0 = the common ordinary-UV case).
			worldPush.uvScale[2] = currentEnvMap ? 1.0f : 0.0f;
			// uvScale.w is this batch's real alphaFunc mode (0=none, 1=GT0,
			// 2=LT128, 3=GE128, 4=GE192 - see VK_GetShaderAlphaFunc's own
			// comment, tr_shader.cpp) - world.frag discards fragments that
			// fail the corresponding real per-mode alpha comparison.
			worldPush.uvScale[3] = (float)currentAlphaFunc;
			// turb.x/y are this batch's real tcMod-turb amplitude and
			// precomputed `phase + time*frequency` (real rd-vanilla's own
			// RB_CalcTurbulentTexCoords, tr_shade_calc.cpp) - see
			// vkWorldPushConstants_t's own comment (tr_local.h).
			worldPush.turb[0] = currentTurbAmplitude;
			worldPush.turb[1] = currentTurbPhase + currentTurbFrequency * timeSeconds;
			if ( currentFogIndex >= 0 )
			{
				const WorldFogEntry &fog = s_worldFogs[currentFogIndex];
				worldPush.fogColor[0] = fog.color[0];
				worldPush.fogColor[1] = fog.color[1];
				worldPush.fogColor[2] = fog.color[2];
				worldPush.fogColor[3] = fog.opaqueDist;
				worldPush.fogStart[0] = VK_ComputeRangedFogStart( currentFogIndex, fog.opaqueDist );
			}
			else
			{
				worldPush.fogColor[0] = worldPush.fogColor[1] = worldPush.fogColor[2] = worldPush.fogColor[3] = 0.0f;
				worldPush.fogStart[0] = 0.0f;
			}
			worldPush.fogStart[1] = currentScrollS * timeSeconds;
			worldPush.fogStart[2] = currentScrollT * timeSeconds;
			vkCmdPushConstants( cmd, vk.worldPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0, sizeof( worldPush ), &worldPush );
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

	// Runtime polys (RE_AddPolyToScene) and sprite/oriented-quad entities -
	// world-space so they use the same mvp directly (no per-entity model
	// matrix, unlike Ghoul2 above); fd is passed for its viewaxis, which
	// camera-facing RT_SPRITE entities need. See VK_DrawScenePolys
	// (tr_model.cpp).
	VK_DrawScenePolys( mvp, fd );

	// Static world flares (MST_FLARE, this file's own VK_DrawWorldFlares) -
	// drawn after Ghoul2/scene polys, same reasoning as rd-vanilla's real
	// sort order (flares' SF_FLARE surface type sorts near the very end of a
	// frame's draw list) - so they layer on top of everything solid the
	// depth buffer already holds, which is exactly what their own depth-
	// tested-but-not-depth-writing draw state needs already present.
	VK_DrawWorldFlares( mvp, fd );

	// World weather/particle effects (tr_weather.cpp) - same call-site
	// convention as rd-vanilla's real RE_RenderWorldEffects (tr_scene.cpp:
	// called once per rendered scene, right after everything else), so a
	// portal/mirror's separate scene render gets its own weather draw too,
	// matching real behavior.
	VK_DrawWeatherEffects( mvp, fd );

	// Restore the full-screen viewport/scissor for any 2D drawing
	// (RE_StretchPic) that follows this scene render within the same frame -
	// it doesn't set its own, it relies on whatever RE_BeginFrame or the
	// last RE_RenderScene left bound.
	VkViewport fullViewport = { 0, 0, (float)vk.swapchainExtent.width, (float)vk.swapchainExtent.height, 0.0f, 1.0f };
	VkRect2D fullScissor = { { 0, 0 }, vk.swapchainExtent };
	vkCmdSetViewport( cmd, 0, 1, &fullViewport );
	vkCmdSetScissor( cmd, 0, 1, &fullScissor );
}

// Used only by tr_weather.cpp's VK_SetTempGlobalFogColor - see that
// function's own comment (tr_local.h) for why weather needs to reach into
// this file's fog state via accessors instead of touching s_worldFogs
// directly. All three only ever look at the map's single global fog
// (s_globalFogIndex), matching rd-vanilla's own tr.world->globalFog-scoped
// behaviour - a local fog volume's colour is never overridden by this.
bool VK_HasWorldFog( void )
{
	return s_globalFogIndex >= 0;
}

void VK_GetWorldFogColor( float outColor[3] )
{
	if ( s_globalFogIndex < 0 )
	{
		return;
	}
	outColor[0] = s_worldFogs[s_globalFogIndex].color[0];
	outColor[1] = s_worldFogs[s_globalFogIndex].color[1];
	outColor[2] = s_worldFogs[s_globalFogIndex].color[2];
}

void VK_SetWorldFogColor( const float color[3] )
{
	if ( s_globalFogIndex < 0 )
	{
		return;
	}
	s_worldFogs[s_globalFogIndex].color[0] = color[0];
	s_worldFogs[s_globalFogIndex].color[1] = color[1];
	s_worldFogs[s_globalFogIndex].color[2] = color[2];
}
