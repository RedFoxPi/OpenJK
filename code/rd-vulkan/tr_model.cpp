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

// Ghoul2 (character/weapon model, .glm) rendering - bind pose only. See
// README.md for exactly what that means and what's still missing (skeletal
// animation, bolts/attachments, LOD selection, per-surface on/off overrides,
// gore, tags). The key scope-reducing fact making a first pass this small:
// mdxmVertex_t::vertCoords (mdx_format.h) is already the model's bind-pose
// object-space position - bone weight data only matters for computing how a
// vertex should move *away* from bind pose during animation, so this file
// never touches bone weights, and the .gla skeleton/animation file is never
// even opened.
//
// GLM parsing here is a fresh implementation, not a reuse of rd-vanilla's
// tr_ghoul2.cpp R_LoadMDXM (see CMakeLists.txt's comment on why Ghoul2 isn't
// reused wholesale), but the offset/pointer arithmetic is copied from it
// field-for-field - binary format parsing is exactly the kind of thing
// that's easy to get subtly wrong by rederiving from the struct comments
// alone, so R_LoadMDXM's real, working arithmetic is what's followed here.
//
// Loaded models reuse tr_world.cpp's WorldVertex layout, pipeline, and
// descriptor-set-building helper wholesale: a Ghoul2 surface is, for
// drawing purposes, just another indexed (position, diffuse UV) triangle
// batch with a paired lightmap texture - only here it's always
// vk.whiteImage, since Ghoul2 meshes have no baked lightmap of their own.

#include "../server/exe_headers.h"

#include "tr_local.h"
#include "../rd-common/mdx_format.h"
#include <cstring>
#include <vector>
#include <unordered_map>
#include <string>

struct GhoulSurfaceDraw
{
	VkDescriptorSet descriptorSet;
	uint32_t firstIndex;
	uint32_t indexCount;
};

struct VulkanGhoul2Model
{
	std::vector<GhoulSurfaceDraw> surfaces;
	VkBuffer vertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
	VkBuffer indexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
	// Index into s_skeletons (0 = none/failed to load) - see VulkanSkeleton
	// below. Only used for bolt lookups (G2API_AddBolt/GetBoltMatrix), never
	// for mesh rendering - see this file's header comment on why mesh
	// vertices don't need the skeleton at all.
	int skeletonIndex = 0;
};

// A bind-pose-only skeleton: just bone names and their bind-pose object-
// space matrices (mdxaSkel_t::BasePoseMat, already fully composed - not a
// parent-relative delta needing a hierarchy walk, confirmed against
// rd-vanilla's real G2_bones.cpp usage of it as a similarity-transform
// pivot). No animation frame data is parsed at all (numFrames/ofsFrames/
// ofsCompBonePool in mdxaHeader_t are never read) - consistent with this
// renderer's bind-pose-only scope. This exists solely so a "bolt" (a named
// attachment point other code queries via G2API_AddBolt/GetBoltMatrix, e.g.
// a cutscene camera tracking a head bone) resolves to *something* correct
// for a static pose, rather than the zeroed/failed matrix a stub would
// return - see README.md for the real bug this fixed (a scripted camera
// silently collapsing onto the NPC's own origin because a failed bolt
// lookup was never error-checked by its caller).
struct VulkanBone
{
	std::string name;
	mdxaBone_t basePoseMat;
};
struct VulkanSkeleton
{
	std::vector<VulkanBone> bones;
};
// Index 0 reserved/invalid, same convention as every other cache in this
// file. Keyed by the resolved ".gla" path (mdxmHeader_t::animName + ".gla"),
// not by owning model, since multiple .glm files legitimately share one
// skeleton (e.g. every humanoid player model uses
// models/players/_humanoid/_humanoid.gla).
static std::vector<VulkanSkeleton> s_skeletons;
static std::unordered_map<std::string, int> s_skeletonsByName;

int VK_LoadGhoul2Skeleton( const char *animName )
{
	std::string fileName = std::string( animName ) + ".gla";
	auto cached = s_skeletonsByName.find( fileName );
	if ( cached != s_skeletonsByName.end() )
	{
		return cached->second;
	}

	void *buffer = nullptr;
	long len = ri.FS_ReadFile( fileName.c_str(), &buffer );
	if ( !buffer || len <= 0 )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: VK_LoadGhoul2Skeleton: %s not found\n", fileName.c_str() );
		return 0;
	}

	const byte *base = (const byte *)buffer;
	const mdxaHeader_t *hdr = (const mdxaHeader_t *)buffer;
	if ( hdr->ident != MDXA_IDENT || hdr->version != MDXA_VERSION )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: VK_LoadGhoul2Skeleton: %s is not a supported GLA (ident/version mismatch)\n", fileName.c_str() );
		ri.FS_FreeFile( buffer );
		return 0;
	}

	// Bones are a flat but variable-length array (mdxaSkel_t's trailing
	// children[numChildren]), so - same idiom as mdxmSurfHierarchy_t in
	// VK_LoadGhoul2Model below, and confirmed against rd-vanilla's real
	// R_LoadMDXA/G2_Add_Bolt - they're reached through an offset table
	// (mdxaSkelOffsets_t) rather than a fixed-size C array index.
	VulkanSkeleton skel;
	skel.bones.reserve( hdr->numBones );
	const mdxaSkelOffsets_t *offsets = (const mdxaSkelOffsets_t *)( base + sizeof( mdxaHeader_t ) );
	for ( int i = 0; i < hdr->numBones; i++ )
	{
		const mdxaSkel_t *bone = (const mdxaSkel_t *)( base + sizeof( mdxaHeader_t ) + offsets->offsets[i] );
		VulkanBone vb;
		vb.name = bone->name;
		vb.basePoseMat = bone->BasePoseMat;
		skel.bones.push_back( std::move( vb ) );
	}

	int numBones = (int)skel.bones.size(); // hdr/base alias buffer, freed next - can't read hdr->numBones after
	ri.FS_FreeFile( buffer );

	if ( s_skeletons.empty() )
	{
		s_skeletons.emplace_back(); // burn index 0, see the cache comment above
	}
	s_skeletons.push_back( std::move( skel ) );
	int index = (int)s_skeletons.size() - 1;
	s_skeletonsByName[fileName] = index;

	ri.Printf( PRINT_ALL, "rd-vulkan: loaded Ghoul2 skeleton %s: %d bones\n", fileName.c_str(), numBones );

	return index;
}

// Index 0 is reserved/invalid (mirrors CGhoul2Info::mModel's use as a
// gameside-only qhandle_t, and this renderer's other 1-based/0-invalid
// handle conventions - e.g. CVulkanGhoul2InfoArray::New() in tr_init.cpp)
// so a freshly-zeroed CGhoul2Info (mModel == 0) reads as "no model loaded"
// rather than colliding with a real cache entry.
static std::vector<VulkanGhoul2Model> s_ghoul2Models;
// Cache key is fileName+"#"+skin handle (as text), not fileName alone: the
// same .glm loaded with two different skins is two different sets of
// per-surface textures (see VulkanSkin below), so it needs two different
// cached VulkanGhoul2Model entries, each with its own baked descriptor sets.
static std::unordered_map<std::string, int> s_ghoul2ModelsByKey;

// Humanoid player/NPC models (unlike weapon/droid models) ship every
// surface's mdxmSurfHierarchy_t::shader field EMPTY - their actual per-
// surface textures come entirely from an external .skin file (a simple
// "surfacename,shaderpath" text format, one pair per line) applied at
// runtime, exactly like rd-vanilla's real skin system (see rd-vanilla's
// tr_skin.cpp for the format this parser matches - CommaParse/comment
// handling/three-part "|" head|torso|lower macro skins are NOT implemented
// here, just the common single-file case). Without this, VK_LoadGhoul2Model
// would resolve zero images for any humanoid model and skip every surface -
// this was confirmed against real game data (kyle/model.glm has 82 surfaces,
// all with shader=="") before this was added.
struct VulkanSkin
{
	std::unordered_map<std::string, std::string> surfaceShaders; // lowercased surface name -> shader path
};
// Index 0 reserved as "no skin" (empty map - every surface falls back to its
// own embedded mdxmSurfHierarchy_t::shader, unchanged behavior for models
// that don't need one, e.g. weapons).
static std::vector<VulkanSkin> s_skins;
static std::unordered_map<std::string, int> s_skinsByName;

int VK_RegisterSkin( const char *name )
{
	if ( !name || !name[0] )
	{
		return 0;
	}
	auto cached = s_skinsByName.find( name );
	if ( cached != s_skinsByName.end() )
	{
		return cached->second;
	}

	void *buffer = nullptr;
	long len = ri.FS_ReadFile( name, &buffer );
	if ( !buffer || len <= 0 )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: VK_RegisterSkin: %s not found\n", name );
		return 0;
	}

	VulkanSkin skin;
	const char *text = (const char *)buffer;
	const char *lineStart = text;
	for ( const char *p = text; ; p++ )
	{
		if ( *p == '\n' || *p == '\0' )
		{
			std::string line( lineStart, p - lineStart );
			lineStart = p + 1;

			size_t comma = line.find( ',' );
			if ( comma != std::string::npos )
			{
				std::string surfName = line.substr( 0, comma );
				std::string shaderName = line.substr( comma + 1 );
				// Strip trailing '\r' (CRLF line endings) and any stray
				// whitespace CommaParse's real tokenizer would have skipped.
				while ( !shaderName.empty() && (unsigned char)shaderName.back() <= ' ' ) shaderName.pop_back();
				for ( char &c : surfName ) c = (char)tolower( (unsigned char)c );

				if ( !surfName.empty() && !shaderName.empty() && surfName.compare( 0, 4, "tag_" ) != 0 )
				{
					skin.surfaceShaders[surfName] = shaderName;
				}
			}

			if ( *p == '\0' ) break;
		}
	}
	ri.FS_FreeFile( buffer );

	if ( s_skins.empty() )
	{
		s_skins.emplace_back(); // burn index 0, see the cache comment above
	}
	s_skins.push_back( std::move( skin ) );
	int index = (int)s_skins.size() - 1;
	s_skinsByName[name] = index;
	return index;
}

// Per-frame queue of entities added via RE_AddRefEntityToScene, drawn by
// VK_DrawGhoul2Entities from RE_RenderScene (tr_world.cpp) and cleared by
// RE_ClearScene. Only RT_MODEL entities carrying a Ghoul2 model actually
// draw anything - everything else (sprites, beams, polys, ...) is silently
// ignored, see README.md.
static std::vector<refEntity_t> s_sceneEntities;
static const size_t MAX_SCENE_ENTITIES = 256;

static const VulkanSkin s_emptySkin;

int VK_LoadGhoul2Model( const char *fileName, int skinHandle )
{
	std::string cacheKey = std::string( fileName ) + "#" + std::to_string( skinHandle );
	auto cached = s_ghoul2ModelsByKey.find( cacheKey );
	if ( cached != s_ghoul2ModelsByKey.end() )
	{
		return cached->second;
	}
	const VulkanSkin &skin = ( skinHandle > 0 && (size_t)skinHandle < s_skins.size() ) ? s_skins[skinHandle] : s_emptySkin;

	void *buffer = nullptr;
	long len = ri.FS_ReadFile( fileName, &buffer );
	if ( !buffer || len <= 0 )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: VK_LoadGhoul2Model: %s not found\n", fileName );
		return 0;
	}

	const byte *base = (const byte *)buffer;
	const mdxmHeader_t *hdr = (const mdxmHeader_t *)buffer;
	if ( hdr->ident != MDXM_IDENT || hdr->version != MDXM_VERSION )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: VK_LoadGhoul2Model: %s is not a supported GLM (ident/version mismatch)\n", fileName );
		ri.FS_FreeFile( buffer );
		return 0;
	}

	// For bolt lookups only (G2API_AddBolt/GetBoltMatrix) - see
	// VulkanSkeleton's comment. animName has no extension (matches
	// R_LoadMDXM's real `va("%s.gla", mdxm->animName)` convention).
	int skeletonIndex = VK_LoadGhoul2Skeleton( hdr->animName );

	// Surface hierarchy: one variable-length mdxmSurfHierarchy_t per surface
	// (mdx_format.h), giving each surface's name, shader name (usually empty
	// for humanoid models - see the skin comment above), and
	// G2SURFACEFLAG_OFF state. Indexed by mdxmSurface_t::thisSurfaceIndex
	// below, not by walk order - carcass doesn't guarantee those match, per
	// R_LoadMDXM.
	std::vector<std::string> surfaceNames( hdr->numSurfaces );
	std::vector<std::string> surfaceShaders( hdr->numSurfaces );
	std::vector<unsigned int> surfaceFlags( hdr->numSurfaces, 0 );
	const mdxmSurfHierarchy_t *surfInfo = (const mdxmSurfHierarchy_t *)( base + hdr->ofsSurfHierarchy );
	for ( int i = 0; i < hdr->numSurfaces; i++ )
	{
		if ( i < (int)surfaceShaders.size() )
		{
			std::string surfName = surfInfo->name;
			for ( char &c : surfName ) c = (char)tolower( (unsigned char)c );
			surfaceNames[i] = surfName;
			surfaceShaders[i] = surfInfo->shader;
			surfaceFlags[i] = surfInfo->flags;
		}
		// Struct is variable-sized (numChildren-dependent childIndexes[]) -
		// this pointer-arithmetic idiom (offsetting by the address of the
		// N-th array element of a null pointer) is copied verbatim from
		// R_LoadMDXM, which is itself carcass/tr_ghoul2.cpp's own idiom for
		// "sizeof a variable-length struct instance".
		surfInfo = (const mdxmSurfHierarchy_t *)( (const byte *)surfInfo +
			(intptr_t)( &((mdxmSurfHierarchy_t *)0)->childIndexes[ surfInfo->numChildren ] ) );
	}

	// LOD 0 only - the highest-detail level and the first one ofsLODs points
	// at. No LOD selection (see README.md): every Ghoul2 entity always draws
	// at full detail regardless of distance.
	const mdxmLOD_t *lod = (const mdxmLOD_t *)( base + hdr->ofsLODs );
	const mdxmSurface_t *surf = (const mdxmSurface_t *)
		( (const byte *)lod + sizeof( mdxmLOD_t ) + (size_t)hdr->numSurfaces * sizeof( mdxmLODSurfOffset_t ) );

	std::vector<WorldVertex> cpuVerts;
	std::vector<uint32_t> cpuIndexes;
	std::vector<GhoulSurfaceDraw> drawSurfaces;

	for ( int i = 0; i < hdr->numSurfaces; i++ )
	{
		int surfIndex = surf->thisSurfaceIndex;
		bool skip = ( surfIndex < 0 || surfIndex >= hdr->numSurfaces )
			|| ( surfaceFlags[surfIndex] & G2SURFACEFLAG_OFF )
			|| surf->numVerts <= 0 || surf->numTriangles <= 0;

		if ( !skip )
		{
			// A skin's per-surface override takes priority over the .glm's
			// own (usually empty, for humanoid models) embedded shader name -
			// same precedence as rd-vanilla's real skin system.
			const std::string *shaderName = &surfaceShaders[surfIndex];
			auto skinOverride = skin.surfaceShaders.find( surfaceNames[surfIndex] );
			if ( skinOverride != skin.surfaceShaders.end() )
			{
				shaderName = &skinOverride->second;
			}
			image_t *img = VK_FindImage( shaderName->c_str() );
			if ( img )
			{
				const mdxmVertex_t *verts = (const mdxmVertex_t *)( (const byte *)surf + surf->ofsVerts );
				// Texture coords are a separate array immediately following
				// the vertex array, not interleaved into mdxmVertex_t itself
				// (mdx_format.h's comment: "seperated ... for cache reasons").
				const mdxmVertexTexCoord_t *texCoords = (const mdxmVertexTexCoord_t *)&verts[surf->numVerts];

				uint32_t vertBase = (uint32_t)cpuVerts.size();
				for ( int v = 0; v < surf->numVerts; v++ )
				{
					WorldVertex wv = {};
					wv.pos[0] = verts[v].vertCoords[0];
					wv.pos[1] = verts[v].vertCoords[1];
					wv.pos[2] = verts[v].vertCoords[2];
					wv.uv[0] = texCoords[v].texCoords[0];
					wv.uv[1] = texCoords[v].texCoords[1];
					wv.lightmapUV[0] = 0.0f;
					wv.lightmapUV[1] = 0.0f;
					cpuVerts.push_back( wv );
				}

				const mdxmTriangle_t *tris = (const mdxmTriangle_t *)( (const byte *)surf + surf->ofsTriangles );
				uint32_t firstIndex = (uint32_t)cpuIndexes.size();
				for ( int t = 0; t < surf->numTriangles; t++ )
				{
					cpuIndexes.push_back( vertBase + (uint32_t)tris[t].indexes[0] );
					cpuIndexes.push_back( vertBase + (uint32_t)tris[t].indexes[1] );
					cpuIndexes.push_back( vertBase + (uint32_t)tris[t].indexes[2] );
				}

				// See vkGlobals_t::ghoul2DescriptorPool's comment for why
				// this can't use vk.worldDescriptorPool. vk.whiteImage (not
				// a per-model white texture) since it's already owned with
				// exactly the lifetime this needs - created once at renderer
				// init, destroyed once at renderer shutdown, unaffected by
				// world reloads (see tr_init.cpp).
				VkDescriptorSet descriptorSet = VK_BuildWorldDescriptorSet( vk.ghoul2DescriptorPool, img, vk.whiteImage );
				drawSurfaces.push_back( { descriptorSet, firstIndex, (uint32_t)surf->numTriangles * 3 } );
			}
			// No image resolved - same "skip rather than draw an opaque
			// white quad" call as tr_world.cpp's RE_LoadWorldMap, and for
			// the same reason (stages-only effect shaders with no plain
			// image of their own).
		}

		surf = (const mdxmSurface_t *)( (const byte *)surf + surf->ofsEnd );
	}

	ri.FS_FreeFile( buffer );

	if ( cpuVerts.empty() || cpuIndexes.empty() )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: VK_LoadGhoul2Model: %s has no drawable surfaces\n", fileName );
		return 0;
	}

	VulkanGhoul2Model model;
	model.surfaces = std::move( drawSurfaces );
	model.skeletonIndex = skeletonIndex;
	VK_UploadDeviceLocalBuffer( cpuVerts.data(), cpuVerts.size() * sizeof( WorldVertex ),
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &model.vertexBuffer, &model.vertexBufferMemory );
	VK_UploadDeviceLocalBuffer( cpuIndexes.data(), cpuIndexes.size() * sizeof( uint32_t ),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &model.indexBuffer, &model.indexBufferMemory );

	if ( s_ghoul2Models.empty() )
	{
		s_ghoul2Models.emplace_back(); // burn index 0, see the cache comment above
	}
	s_ghoul2Models.push_back( std::move( model ) );
	int index = (int)s_ghoul2Models.size() - 1;
	s_ghoul2ModelsByKey[cacheKey] = index;

	ri.Printf( PRINT_ALL, "rd-vulkan: loaded Ghoul2 model %s (skin %d): %d surfaces, %d verts, %d indexes\n",
		fileName, skinHandle, (int)s_ghoul2Models[index].surfaces.size(), (int)cpuVerts.size(), (int)cpuIndexes.size() );

	return index;
}

int VK_FindGhoul2Bone( int modelCacheIndex, const char *boneName )
{
	if ( modelCacheIndex <= 0 || (size_t)modelCacheIndex >= s_ghoul2Models.size() || !boneName )
	{
		return -1;
	}
	int skeletonIndex = s_ghoul2Models[modelCacheIndex].skeletonIndex;
	if ( skeletonIndex <= 0 || (size_t)skeletonIndex >= s_skeletons.size() )
	{
		return -1;
	}
	const std::vector<VulkanBone> &bones = s_skeletons[skeletonIndex].bones;
	for ( size_t i = 0; i < bones.size(); i++ )
	{
		if ( !Q_stricmp( bones[i].name.c_str(), boneName ) )
		{
			return (int)i;
		}
	}
	return -1;
}

bool VK_GetGhoul2BoneBasePoseMat( int modelCacheIndex, int boneIndex, mdxaBone_t *out )
{
	if ( modelCacheIndex <= 0 || (size_t)modelCacheIndex >= s_ghoul2Models.size() || !out )
	{
		return false;
	}
	int skeletonIndex = s_ghoul2Models[modelCacheIndex].skeletonIndex;
	if ( skeletonIndex <= 0 || (size_t)skeletonIndex >= s_skeletons.size() )
	{
		return false;
	}
	const std::vector<VulkanBone> &bones = s_skeletons[skeletonIndex].bones;
	if ( boneIndex < 0 || (size_t)boneIndex >= bones.size() )
	{
		return false;
	}
	*out = bones[boneIndex].basePoseMat;
	return true;
}

void VK_ShutdownGhoul2Models( void )
{
	for ( VulkanGhoul2Model &model : s_ghoul2Models )
	{
		if ( model.vertexBuffer ) vkDestroyBuffer( vk.device, model.vertexBuffer, nullptr );
		if ( model.vertexBufferMemory ) vkFreeMemory( vk.device, model.vertexBufferMemory, nullptr );
		if ( model.indexBuffer ) vkDestroyBuffer( vk.device, model.indexBuffer, nullptr );
		if ( model.indexBufferMemory ) vkFreeMemory( vk.device, model.indexBufferMemory, nullptr );
	}
	s_ghoul2Models.clear();
	s_ghoul2ModelsByKey.clear();
	s_sceneEntities.clear();
	// Descriptor sets built above come from vk.ghoul2DescriptorPool, not
	// individually tracked - reclaim them all at once, same reasoning as
	// tr_world.cpp's VK_ShutdownWorld and vk.worldDescriptorPool.
	if ( vk.ghoul2DescriptorPool ) vkResetDescriptorPool( vk.device, vk.ghoul2DescriptorPool, 0 );
}

void RE_ClearScene( void )
{
	s_sceneEntities.clear();
}

void RE_AddRefEntityToScene( const refEntity_t *re )
{
	if ( !re || s_sceneEntities.size() >= MAX_SCENE_ENTITIES )
	{
		return;
	}
	s_sceneEntities.push_back( *re );
}

void VK_DrawGhoul2Entities( const float *mvp )
{
	if ( s_sceneEntities.empty() || !vk.frameActive )
	{
		return;
	}

	VkCommandBuffer cmd = vk.activeCommandBuffer;

	// Ghoul2 meshes reuse the world pipeline/vertex format wholesale (see
	// this file's header comment) rather than a dedicated model pipeline -
	// it's already bound from RE_RenderScene's world-surface pass, but bind
	// again defensively in case that ever stops being guaranteed.
	if ( vk.worldPipeline != vk.lastBoundPipeline )
	{
		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.worldPipeline );
		vk.lastBoundPipeline = vk.worldPipeline;
	}

	// One-time-ish diagnostic (mirrors tr_world.cpp's frustum-culling debug
	// print): confirms entities are actually reaching this function and
	// being matched to a loaded model, independent of whether the result is
	// visually obvious in any one screenshot (e.g. the player's own weapon
	// viewmodel isn't drawn every frame of every scene).
	static int s_debugEntityLogsRemaining = 3;
	int drawnEntityCount = 0, drawnSubModelCount = 0;
	int rtModelCount = 0;

	for ( const refEntity_t &ent : s_sceneEntities )
	{
		if ( ent.reType != RT_MODEL || !ent.ghoul2 || ent.ghoul2->size() <= 0 )
		{
			continue;
		}
		rtModelCount++;

		// Object-to-world matrix from the entity's origin/axis, same
		// column-major construction as rd-vanilla's tr_main.cpp
		// R_RotateForEntity: column c is basis vector axis[c], column 3 is
		// origin - the inverse-direction transform from VK_BuildViewMatrix's
		// world-to-camera construction (tr_world.cpp), which instead stores
		// axis vectors as rows because it needs the opposite (world-to-
		// local) rotation. Same transform for every sub-model below - a
		// single entity's Ghoul2 vector can hold several (body + weapon +
		// saber blade, ...), all attached at the entity's own origin/axis.
		float model_[16] = {};
		model_[0] = ent.axis[0][0]; model_[4] = ent.axis[1][0]; model_[8] = ent.axis[2][0]; model_[12] = ent.origin[0];
		model_[1] = ent.axis[0][1]; model_[5] = ent.axis[1][1]; model_[9] = ent.axis[2][1]; model_[13] = ent.origin[1];
		model_[2] = ent.axis[0][2]; model_[6] = ent.axis[1][2]; model_[10] = ent.axis[2][2]; model_[14] = ent.origin[2];
		model_[15] = 1.0f;

		// Want: entityMvp = mvp * model (apply the entity's model matrix
		// first, then the already-computed view-projection). Per
		// VK_MultiplyMatrix's comment (tr_world.cpp), that means passing
		// (model, mvp).
		float entityMvp[16];
		VK_MultiplyMatrix( model_, mvp, entityMvp );

		bool drewAnySubModel = false;
		for ( int slot = 0; slot < ent.ghoul2->size(); slot++ )
		{
			int modelIndex = (*ent.ghoul2)[slot].mModel;
			if ( modelIndex <= 0 || (size_t)modelIndex >= s_ghoul2Models.size() )
			{
				continue;
			}
			const VulkanGhoul2Model &model = s_ghoul2Models[modelIndex];
			if ( model.surfaces.empty() )
			{
				continue;
			}

			vkCmdPushConstants( cmd, vk.worldPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof( entityMvp ), entityMvp );

			VkDeviceSize vertexOffset = 0;
			vkCmdBindVertexBuffers( cmd, 0, 1, &model.vertexBuffer, &vertexOffset );
			vkCmdBindIndexBuffer( cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT32 );

			for ( const GhoulSurfaceDraw &surface : model.surfaces )
			{
				vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.worldPipelineLayout,
					0, 1, &surface.descriptorSet, 0, nullptr );
				vkCmdDrawIndexed( cmd, surface.indexCount, 1, surface.firstIndex, 0, 0 );
			}
			drawnSubModelCount++;
			drewAnySubModel = true;
		}
		if ( drewAnySubModel )
		{
			drawnEntityCount++;
		}
	}

	if ( s_debugEntityLogsRemaining > 0 )
	{
		s_debugEntityLogsRemaining--;
		ri.Printf( PRINT_ALL, "rd-vulkan: ghoul2: %d/%d scene entities drew %d sub-model(s)\n",
			drawnEntityCount, rtModelCount, drawnSubModelCount );
	}
}
