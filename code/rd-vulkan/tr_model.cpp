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

// Ghoul2 (character/weapon model, .glm) rendering, including real per-bone
// skeletal mesh skinning and live, time-driven animation playback - see
// README.md's "Skeletal animation" section for the full picture: what's
// real (bone hierarchy composition, frame decompression, weighted-blend
// skinning, G2API_SetBoneAnim/GetBoneAnim driving an actual per-instance
// current frame over time) and what's still a deliberate simplification
// (a single whole-skeleton animation track per model instance rather than
// independently-blended per-bone-subtree tracks, no animation blending/
// crossfade, no LOD selection, no per-surface on/off overrides, no gore).
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
#include "../qcommon/matcomp.h"
#include <cmath>
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

// One vertex's CPU-side skinning inputs, parallel (same index) to the
// uploaded vertex buffer's contents - VK_SkinGhoul2Model re-derives that
// buffer's positions from these every time a model's pose changes, instead
// of the fixed-at-load-time WorldVertex the bind-pose-only version of this
// file baked once and never touched again. boneIndex entries are already
// remapped from the surface-local indices G2_GetVertBoneIndex returns to
// global skeleton bone indices (via that surface's own ofsBoneReferences
// table) at load time, once, rather than needing that indirection redone
// on every skin - see VK_LoadGhoul2Model.
struct GhoulSkinVertex
{
	float bindPos[3];
	float uv[2];
	int numWeights;
	int boneIndex[iMAX_G2_BONEWEIGHTS_PER_VERT];
	float boneWeight[iMAX_G2_BONEWEIGHTS_PER_VERT];
};

// How many independent poses one cached model's vertex buffer can hold at
// once - see VulkanGhoul2Model::vertexBuffer's comment for why a model
// needs more than one. Chosen generously for "a handful of the same NPC
// visible at once" test scenes, not derived from any hard engine limit.
static const uint32_t GHOUL2_SKIN_SLOTS_PER_MODEL = 8;

struct VulkanGhoul2Model
{
	std::vector<GhoulSurfaceDraw> surfaces;
	// Sized for GHOUL2_SKIN_SLOTS_PER_MODEL independent copies of this
	// model's vertex data, not just one - now that animation is live (see
	// VK_SetGhoul2BoneAnim), two entities sharing this one cached model
	// (keyed by file+skin - see s_ghoul2ModelsByKey's comment, not by
	// entity) can legitimately be at two different frames in the same
	// drawn scene, and a *single* shared buffer would be a real bug, not
	// just an inefficiency: every vkCmdDrawIndexed recorded against it
	// would end up reading whichever entity's CPU skin write happened to
	// land last, because a Vulkan command buffer's recorded commands don't
	// actually execute until the whole buffer is submitted - by then every
	// CPU memcpy this frame has already happened. Writing each entity's
	// skin to its own byte range (see nextSkinSlot) instead of the same one
	// avoids that entirely. Host-visible/coherent and persistently mapped
	// (unlike tr_world.cpp's device-local geometry, which never changes
	// post-upload), since VK_SkinGhoul2Model rewrites a slot's contents
	// from the CPU on every draw, not just once at load time.
	VkBuffer vertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
	void *vertexBufferMapped = nullptr;
	VkBuffer indexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
	// Index into s_skeletons (0 = none/failed to load) - see VulkanSkeleton
	// below. Used both for bolt lookups (G2API_AddBolt/GetBoltMatrix) and,
	// now, real per-frame mesh skinning (VK_SkinGhoul2Model) - see this
	// file's header comment for why that used to not be true.
	int skeletonIndex = 0;
	// Parallel to one slot's worth of the vertex buffer's contents - see
	// GhoulSkinVertex. skinSource.size() vertices per slot.
	std::vector<GhoulSkinVertex> skinSource;
	// Round-robins across this model's GHOUL2_SKIN_SLOTS_PER_MODEL vertex-
	// buffer slots, one per drawn sub-model instance this frame - reset to
	// 0 at the top of VK_DrawGhoul2Entities. Wrapping past
	// GHOUL2_SKIN_SLOTS_PER_MODEL distinct simultaneous instances of the
	// *same* cached model in one frame reuses an already-drawn-this-frame
	// slot (a stale-for-one-frame pose on an instance beyond the 8th, not a
	// crash or corruption) - accepted as a rare-scene edge case rather than
	// an unbounded dynamic allocation.
	uint32_t nextSkinSlot = 0;
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
	int parent; // -1 = root - needed for VK_ComputeGhoul2Pose's hierarchy walk
};
struct VulkanSkeleton
{
	std::vector<VulkanBone> bones;
	int numFrames = 0;
	// The whole .gla file, kept resident (unlike the original bind-pose-only
	// version of this struct, which read out bone names/BasePoseMat and
	// freed the file immediately) - animation needs on-demand access to
	// mdxaHeader_t::ofsFrames/ofsCompBonePool for an arbitrary frame at an
	// arbitrary later time (see VK_ComputeGhoul2Pose), not just the fixed
	// bind-pose data read once at load time. Always re-derive the header
	// pointer from fileData.data() rather than caching it separately - a
	// moved (not copied) std::vector keeps its heap buffer, so this stays
	// valid across s_skeletons reallocating, but a cached raw pointer
	// computed before that move would not be worth the risk for no benefit.
	std::vector<byte> fileData;
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
		vb.parent = bone->parent;
		skel.bones.push_back( std::move( vb ) );
	}

	int numBones = (int)skel.bones.size();
	skel.numFrames = hdr->numFrames;
	// Copy the whole file (not just bones) into our own buffer before
	// freeing FS's copy - VK_ComputeGhoul2Pose needs ofsFrames/
	// ofsCompBonePool later, at arbitrary times this function has already
	// returned from, so the FS-owned buffer (freed below, same as every
	// other loader in this file) can't be the one it reads from.
	skel.fileData.assign( base, base + len );
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

// Per-frame queue of entities added via RE_AddRefEntityToScene, cleared by
// RE_ClearScene. RT_MODEL entities carrying a Ghoul2 model are drawn by
// VK_DrawGhoul2Entities; every other refEntityType_t (RT_SPRITE,
// RT_ORIENTED_QUAD, RT_SABER_GLOW, RT_BEAM, RT_LINE, RT_CYLINDER,
// RT_ELECTRICITY, RT_LATHE, RT_CLOUDS) is drawn by VK_DrawScenePolys (same
// function that draws RE_AddPolyToScene polys - see its comment for why),
// except RT_PORTALSURFACE, which real rd-vanilla doesn't draw either (see
// README.md). Runtime polys (RE_AddPolyToScene) are a separate queue, not
// a refEntity_t at all - see s_scenePolys below.
static std::vector<refEntity_t> s_sceneEntities;
static const size_t MAX_SCENE_ENTITIES = 256;

// Per-frame queue of polys added via RE_AddPolyToScene (particles, sparks,
// decals, ...), drawn by VK_DrawScenePolys from RE_RenderScene and cleared
// by RE_ClearScene. verts is a copy, not a pointer into the caller's own
// buffer - rd-vanilla's real RE_AddPolyToScene (tr_scene.cpp) also copies
// immediately for the same reason: callers routinely reuse/free their
// polyVert_t buffer right after this call returns, well before the scene
// actually gets drawn.
struct VulkanScenePoly
{
	qhandle_t hShader;
	std::vector<polyVert_t> verts;
};
static std::vector<VulkanScenePoly> s_scenePolys;
// Same order of magnitude as rd-vanilla's real MAX_POLYS (tr_local.h) - not
// meant to be a hard engine-parity number, just a generous per-frame cap.
static const size_t MAX_SCENE_POLYS = 2048;

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
	std::vector<GhoulSkinVertex> skinSource;

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
				// Surface-local weight bone indices (G2_GetVertBoneIndex)
				// are indexes into THIS array, not the skeleton directly -
				// confirmed against rd-vanilla's real R_AddGHOULSurfaces
				// (tr_ghoul2.cpp: `piBoneReferences[G2_GetVertBoneIndex(v,k)]`).
				// Remapped to global skeleton bone indices once here, at
				// load time, so VK_SkinGhoul2Model never needs this table.
				const int *boneReferences = (const int *)( (const byte *)surf + surf->ofsBoneReferences );

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

					GhoulSkinVertex sv = {};
					sv.bindPos[0] = verts[v].vertCoords[0];
					sv.bindPos[1] = verts[v].vertCoords[1];
					sv.bindPos[2] = verts[v].vertCoords[2];
					sv.uv[0] = texCoords[v].texCoords[0];
					sv.uv[1] = texCoords[v].texCoords[1];
					sv.numWeights = G2_GetVertWeights( &verts[v] );
					float totalWeight = 0.0f;
					for ( int k = 0; k < sv.numWeights && k < iMAX_G2_BONEWEIGHTS_PER_VERT; k++ )
					{
						int localBoneIndex = G2_GetVertBoneIndex( &verts[v], k );
						sv.boneIndex[k] = ( localBoneIndex >= 0 && localBoneIndex < surf->numBoneReferences )
							? boneReferences[localBoneIndex] : 0;
						sv.boneWeight[k] = G2_GetVertBoneWeight( &verts[v], k, totalWeight, sv.numWeights );
					}
					skinSource.push_back( sv );
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
	model.skinSource = std::move( skinSource );

	// Sized for GHOUL2_SKIN_SLOTS_PER_MODEL independent slots - see
	// VulkanGhoul2Model::vertexBuffer's comment for why one slot isn't
	// enough once animation is live. Host-visible/coherent and persistently
	// mapped, not device-local like tr_world.cpp's static geometry -
	// VK_SkinGhoul2Model rewrites a slot's contents directly from the CPU
	// on every draw, so there's no benefit to device-local memory's faster
	// GPU-side read here, only the cost of a staging-buffer round trip on
	// every re-skin. Only slot 0 is initialized here (the raw bind pose,
	// cpuVerts) - every slot is always fully rewritten by VK_SkinGhoul2Model
	// before anything reads it, so the rest starting uninitialized is fine.
	VkDeviceSize slotSize = cpuVerts.size() * sizeof( WorldVertex );
	VkDeviceSize vbSize = slotSize * GHOUL2_SKIN_SLOTS_PER_MODEL;
	VK_CreateBuffer( vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&model.vertexBuffer, &model.vertexBufferMemory );
	vkMapMemory( vk.device, model.vertexBufferMemory, 0, vbSize, 0, &model.vertexBufferMapped );
	memcpy( model.vertexBufferMapped, cpuVerts.data(), (size_t)slotSize );

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

// out = a applied first, then b (i.e. out transforms a point by a, then by
// b) - arithmetic copied verbatim from rd-vanilla's real
// Multiply_3x4Matrix (tr_ghoul2.cpp), not rederived: this is exactly the
// operation real skeletal hierarchy composition needs (child = parent ∘
// child-local-delta) and getting a 3x4 affine multiply's row/column
// convention subtly backwards is easy to do and hard to notice by eye.
static void VK_Multiply3x4Matrix( mdxaBone_t *out, const mdxaBone_t *b, const mdxaBone_t *a )
{
	out->matrix[0][0] = ( b->matrix[0][0] * a->matrix[0][0] ) + ( b->matrix[0][1] * a->matrix[1][0] ) + ( b->matrix[0][2] * a->matrix[2][0] );
	out->matrix[0][1] = ( b->matrix[0][0] * a->matrix[0][1] ) + ( b->matrix[0][1] * a->matrix[1][1] ) + ( b->matrix[0][2] * a->matrix[2][1] );
	out->matrix[0][2] = ( b->matrix[0][0] * a->matrix[0][2] ) + ( b->matrix[0][1] * a->matrix[1][2] ) + ( b->matrix[0][2] * a->matrix[2][2] );
	out->matrix[0][3] = ( b->matrix[0][0] * a->matrix[0][3] ) + ( b->matrix[0][1] * a->matrix[1][3] ) + ( b->matrix[0][2] * a->matrix[2][3] ) + b->matrix[0][3];
	out->matrix[1][0] = ( b->matrix[1][0] * a->matrix[0][0] ) + ( b->matrix[1][1] * a->matrix[1][0] ) + ( b->matrix[1][2] * a->matrix[2][0] );
	out->matrix[1][1] = ( b->matrix[1][0] * a->matrix[0][1] ) + ( b->matrix[1][1] * a->matrix[1][1] ) + ( b->matrix[1][2] * a->matrix[2][1] );
	out->matrix[1][2] = ( b->matrix[1][0] * a->matrix[0][2] ) + ( b->matrix[1][1] * a->matrix[1][2] ) + ( b->matrix[1][2] * a->matrix[2][2] );
	out->matrix[1][3] = ( b->matrix[1][0] * a->matrix[0][3] ) + ( b->matrix[1][1] * a->matrix[1][3] ) + ( b->matrix[1][2] * a->matrix[2][3] ) + b->matrix[1][3];
	out->matrix[2][0] = ( b->matrix[2][0] * a->matrix[0][0] ) + ( b->matrix[2][1] * a->matrix[1][0] ) + ( b->matrix[2][2] * a->matrix[2][0] );
	out->matrix[2][1] = ( b->matrix[2][0] * a->matrix[0][1] ) + ( b->matrix[2][1] * a->matrix[1][1] ) + ( b->matrix[2][2] * a->matrix[2][1] );
	out->matrix[2][2] = ( b->matrix[2][0] * a->matrix[0][2] ) + ( b->matrix[2][1] * a->matrix[1][2] ) + ( b->matrix[2][2] * a->matrix[2][2] );
	out->matrix[2][3] = ( b->matrix[2][0] * a->matrix[0][3] ) + ( b->matrix[2][1] * a->matrix[1][3] ) + ( b->matrix[2][2] * a->matrix[2][3] ) + b->matrix[2][3];
}

// Per-(frame,bone) index into the compressed bone pool - a little-endian
// packed 3-byte int (mdxaIndex_t), formula copied verbatim from
// rd-vanilla's real G2_GetBonePoolIndex (tr_ghoul2.cpp).
static int VK_GetGhoul2BonePoolIndex( const mdxaHeader_t *header, int frame, int bone )
{
	int offset = ( frame * header->numBones * 3 ) + ( bone * 3 );
	const mdxaIndex_t *index = (const mdxaIndex_t *)( (const byte *)header + header->ofsFrames + offset );
	return ( index->iIndex[2] << 16 ) + ( index->iIndex[1] << 8 ) + index->iIndex[0];
}

// Computes every bone's object-space pose matrix for one animation frame of
// one skeleton - the input to both mesh skinning (VK_SkinGhoul2Model, added
// in a follow-up commit) and (eventually) a real, animated
// G2API_GetBoltMatrix. No blending between two frames/animations, no
// per-bone angle overrides, no ragdoll, no smoothing - see README.md for
// the deliberate scope cut versus rd-vanilla's real, much larger
// CBoneCache/G2_TransformBone (tr_ghoul2.cpp).
//
// Deliberately NOT using mdxaSkel_t::BasePoseMat/BasePoseMatInv anywhere in
// this path, even though that looks suspicious at first glance for
// something computing an object-space pose - confirmed against
// rd-vanilla's real mesh-skinning code (R_AddGHOULSurfaces, tr_ghoul2.cpp):
// it applies CBoneCache::EvalRender()'s result - exactly this function's
// per-bone hierarchy composition, with no BasePoseMatInv step - directly to
// mdxmVertex_t::vertCoords. BasePoseMat/BasePoseMatInv are only used
// elsewhere for bolt queries against a fixed reference pose and an
// optional, cvar-gated "unsquash" renormalization pass - not the ordinary
// animated-mesh-skinning path this function feeds.
static void VK_ComputeGhoul2BoneRecursive( const VulkanSkeleton &skel, const mdxaHeader_t *header, int frame,
	int boneIndex, std::vector<mdxaBone_t> &outBones, std::vector<bool> &computed )
{
	if ( computed[boneIndex] )
	{
		return;
	}

	mdxaBone_t delta;
	const mdxaCompQuatBone_t *pool = (const mdxaCompQuatBone_t *)( (const byte *)header + header->ofsCompBonePool );
	int poolIndex = VK_GetGhoul2BonePoolIndex( header, frame, boneIndex );
	MC_UnCompressQuat( delta.matrix, pool[poolIndex].Comp );

	int parent = skel.bones[boneIndex].parent;
	if ( parent < 0 )
	{
		outBones[boneIndex] = delta;
	}
	else
	{
		VK_ComputeGhoul2BoneRecursive( skel, header, frame, parent, outBones, computed );
		VK_Multiply3x4Matrix( &outBones[boneIndex], &outBones[parent], &delta );
	}
	computed[boneIndex] = true;
}

void VK_ComputeGhoul2Pose( int skeletonIndex, int frame, std::vector<mdxaBone_t> &outBones )
{
	outBones.clear();
	if ( skeletonIndex <= 0 || (size_t)skeletonIndex >= s_skeletons.size() )
	{
		return;
	}
	const VulkanSkeleton &skel = s_skeletons[skeletonIndex];
	if ( skel.numFrames <= 0 )
	{
		return;
	}
	if ( frame < 0 || frame >= skel.numFrames )
	{
		frame = 0;
	}

	const mdxaHeader_t *header = (const mdxaHeader_t *)skel.fileData.data();
	int numBones = (int)skel.bones.size();
	outBones.resize( numBones );
	std::vector<bool> computed( numBones, false );
	for ( int i = 0; i < numBones; i++ )
	{
		VK_ComputeGhoul2BoneRecursive( skel, header, frame, i, outBones, computed );
	}
}

// Live animation state per Ghoul2 model instance, driven by
// G2API_SetBoneAnim (tr_init.cpp's thin wrapper calls into
// VK_SetGhoul2BoneAnim below) - a deliberate simplification of the real
// engine's design: one whole-skeleton animation track per CGhoul2Info
// instance, not independently-blended per-bone-subtree tracks (the real
// game calls SetBoneAnim separately for e.g. "lower_lumbar"/"upper_lumbar"/
// "thoracic" to blend legs/torso/arms independently - see bg_panimate.cpp).
// Here the *last* SetBoneAnim call for a given instance simply wins for the
// whole skeleton, regardless of which bone name it targeted - a visibly
// cruder result than real independent limb blending, but a real, live,
// correctly time-driven animation instead of a permanently frozen single
// static frame. No animation blending/crossfade between two clips, and no
// sub-frame interpolation between two adjacent whole frames either (both
// real, visible-quality features of rd-vanilla's G2_TimingModel this
// intentionally doesn't reproduce - see README.md).
struct VulkanGhoul2AnimState
{
	int startFrame = 0;
	int endFrame = 0;
	int startTime = 0;
	int pauseTime = 0; // 0 = not paused
	float animSpeed = 0.0f;
	int flags = 0;
};
// Keyed by CGhoul2Info identity - the exact pointer game code calls
// G2API_SetBoneAnim with (e.g. &gent->ghoul2[gent->playerModel] in
// bg_panimate.cpp). refEntity_t::ghoul2 (tr_types.h) is a pointer to the
// game-owned CGhoul2Info_v, not a per-frame copy, so &(*ent.ghoul2)[slot]
// in VK_DrawGhoul2Entities below is the same address on every frame for
// the same in-game entity/sub-model.
static std::unordered_map<const CGhoul2Info *, VulkanGhoul2AnimState> s_ghoul2AnimState;

// BONE_ANIM_OVERRIDE_LOOP's real value (game/ghoul2_shared.h) - not
// included from here (a game-side header this renderer doesn't otherwise
// depend on) since this is the only flag bit this simplified
// implementation actually interprets.
static const int VK_BONE_ANIM_OVERRIDE_LOOP = 0x0010;

// The G2_TimingModel formula (rd-vanilla/tr_ghoul2.cpp), copied verbatim
// except for the parts this renderer doesn't implement (blending between
// two animations, sub-frame lerp, reverse/negative animSpeed playback -
// all real, visible-quality features left out, see README.md): frame
// advances at animSpeed frames per 50ms of elapsed time. Confirmed against
// the real caller convention (bg_panimate.cpp: `animSpeed = 50.0f /
// curAnim.frameLerp * timeScaleMod`, i.e. the caller pre-scales animSpeed
// so this formula's "/ 50" always means real elapsed milliseconds divided
// by the clip's real per-frame duration, not a made-up constant).
static float VK_ComputeGhoul2AnimFrame( const VulkanGhoul2AnimState &state, int currentTime )
{
	int effectiveTime = state.pauseTime ? state.pauseTime : currentTime;
	float time = (float)( effectiveTime - state.startTime ) / 50.0f;
	if ( time < 0.0f )
	{
		time = 0.0f;
	}
	float frame = (float)state.startFrame + time * state.animSpeed;

	int animSize = state.endFrame - state.startFrame;
	if ( animSize > 0 && state.animSpeed > 0.0f && frame > (float)state.endFrame )
	{
		if ( state.flags & VK_BONE_ANIM_OVERRIDE_LOOP )
		{
			frame = (float)state.startFrame + fmodf( frame - (float)state.startFrame, (float)animSize );
		}
		else
		{
			frame = (float)state.endFrame;
		}
	}
	return frame;
}

void VK_SetGhoul2BoneAnim( const CGhoul2Info *ghlInfo, int startFrame, int endFrame, int flags, float animSpeed, int startTime )
{
	if ( !ghlInfo )
	{
		return;
	}
	VulkanGhoul2AnimState &state = s_ghoul2AnimState[ghlInfo];
	state.startFrame = startFrame;
	state.endFrame = endFrame;
	state.startTime = startTime;
	state.pauseTime = 0;
	state.animSpeed = animSpeed;
	state.flags = flags;
}

bool VK_GetGhoul2BoneAnim( const CGhoul2Info *ghlInfo, int currentTime, float *currentFrame, int *startFrame, int *endFrame, int *flags, float *animSpeed )
{
	auto it = s_ghoul2AnimState.find( ghlInfo );
	if ( it == s_ghoul2AnimState.end() )
	{
		return false;
	}
	const VulkanGhoul2AnimState &state = it->second;
	if ( startFrame ) *startFrame = state.startFrame;
	if ( endFrame ) *endFrame = state.endFrame;
	if ( flags ) *flags = state.flags;
	if ( animSpeed ) *animSpeed = state.animSpeed;
	if ( currentFrame ) *currentFrame = VK_ComputeGhoul2AnimFrame( state, currentTime );
	return true;
}

bool VK_PauseGhoul2BoneAnim( const CGhoul2Info *ghlInfo, int currentTime )
{
	auto it = s_ghoul2AnimState.find( ghlInfo );
	if ( it == s_ghoul2AnimState.end() )
	{
		return false;
	}
	it->second.pauseTime = currentTime;
	return true;
}

bool VK_IsGhoul2BoneAnimPaused( const CGhoul2Info *ghlInfo )
{
	auto it = s_ghoul2AnimState.find( ghlInfo );
	return it != s_ghoul2AnimState.end() && it->second.pauseTime != 0;
}

bool VK_StopGhoul2BoneAnim( const CGhoul2Info *ghlInfo )
{
	return s_ghoul2AnimState.erase( ghlInfo ) > 0;
}

// The frame to skin a model instance to right now - VK_ComputeGhoul2Pose's
// frame argument. No recorded animation state (SetBoneAnim was never
// called for this instance) falls back to frame 0, the prior static
// behavior - see README.md's frame-0 caveat for what that does and doesn't
// mean.
int VK_GetGhoul2PoseFrame( const CGhoul2Info *ghlInfo, int currentTime )
{
	auto it = s_ghoul2AnimState.find( ghlInfo );
	if ( it == s_ghoul2AnimState.end() )
	{
		return 0;
	}
	return (int)VK_ComputeGhoul2AnimFrame( it->second, currentTime );
}

// Recomputes one model instance's vertex buffer for a given pose - linear
// blend skinning, weighted sum of (bone.matrix applied to bindPos) over
// each vertex's up-to-4 weighted bones. Formula (including applying the
// pose matrix directly to bindPos with no extra bind-pose-inverse step, and
// the 1/2/generic-N weight cases) copied verbatim from rd-vanilla's real
// R_AddGHOULSurfaces (tr_ghoul2.cpp) - see VK_ComputeGhoul2Pose's comment
// for why no BasePoseMatInv belongs here. A model with no valid skeleton
// (pose.empty()) falls back to unskinned bind pose - the original
// behavior, still correct for e.g. single-bone weapon models attached via
// a bolt rather than posed themselves.
// Writes into the given slot's byte range of model.vertexBufferMapped
// (slot < GHOUL2_SKIN_SLOTS_PER_MODEL) rather than always slot 0 - see
// VulkanGhoul2Model::vertexBuffer's comment for why a single shared slot
// stopped being safe once animation went live.
static void VK_SkinGhoul2Model( VulkanGhoul2Model &model, const std::vector<mdxaBone_t> &pose, uint32_t slot )
{
	WorldVertex *out = (WorldVertex *)model.vertexBufferMapped + (size_t)slot * model.skinSource.size();
	for ( size_t v = 0; v < model.skinSource.size(); v++ )
	{
		const GhoulSkinVertex &sv = model.skinSource[v];
		float pos[3] = { 0.0f, 0.0f, 0.0f };

		if ( !pose.empty() )
		{
			for ( int k = 0; k < sv.numWeights; k++ )
			{
				int boneIndex = sv.boneIndex[k];
				if ( boneIndex < 0 || (size_t)boneIndex >= pose.size() )
				{
					continue;
				}
				const mdxaBone_t &m = pose[boneIndex];
				float w = sv.boneWeight[k];
				pos[0] += w * ( m.matrix[0][0] * sv.bindPos[0] + m.matrix[0][1] * sv.bindPos[1] + m.matrix[0][2] * sv.bindPos[2] + m.matrix[0][3] );
				pos[1] += w * ( m.matrix[1][0] * sv.bindPos[0] + m.matrix[1][1] * sv.bindPos[1] + m.matrix[1][2] * sv.bindPos[2] + m.matrix[1][3] );
				pos[2] += w * ( m.matrix[2][0] * sv.bindPos[0] + m.matrix[2][1] * sv.bindPos[1] + m.matrix[2][2] * sv.bindPos[2] + m.matrix[2][3] );
			}
		}
		else
		{
			pos[0] = sv.bindPos[0];
			pos[1] = sv.bindPos[1];
			pos[2] = sv.bindPos[2];
		}

		out[v].pos[0] = pos[0];
		out[v].pos[1] = pos[1];
		out[v].pos[2] = pos[2];
		out[v].uv[0] = sv.uv[0];
		out[v].uv[1] = sv.uv[1];
		out[v].lightmapUV[0] = 0.0f;
		out[v].lightmapUV[1] = 0.0f;
	}
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
	// Stale CGhoul2Info* keys (game-owned instances this renderer never
	// allocated, freed on the game side across a map load) could otherwise
	// collide with a genuinely different instance later allocated at the
	// same address - see s_ghoul2AnimState's comment.
	s_ghoul2AnimState.clear();
	// Descriptor sets built above come from vk.ghoul2DescriptorPool, not
	// individually tracked - reclaim them all at once, same reasoning as
	// tr_world.cpp's VK_ShutdownWorld and vk.worldDescriptorPool.
	if ( vk.ghoul2DescriptorPool ) vkResetDescriptorPool( vk.device, vk.ghoul2DescriptorPool, 0 );
}

void RE_ClearScene( void )
{
	s_sceneEntities.clear();
	s_scenePolys.clear();
}

void RE_AddRefEntityToScene( const refEntity_t *re )
{
	if ( !re || s_sceneEntities.size() >= MAX_SCENE_ENTITIES )
	{
		return;
	}
	s_sceneEntities.push_back( *re );
}

void RE_AddPolyToScene( qhandle_t hShader, int numVerts, const polyVert_t *verts )
{
	// hShader == 0 means "no shader" (RE_RegisterShader's failure return,
	// same convention RE_StretchPic's image lookup uses) - never a valid
	// poly. numVerts < 3 can't form a triangle fan at all.
	if ( !hShader || numVerts < 3 || !verts || s_scenePolys.size() >= MAX_SCENE_POLYS )
	{
		return;
	}
	VulkanScenePoly poly;
	poly.hShader = hShader;
	poly.verts.assign( verts, verts + numVerts );
	s_scenePolys.push_back( std::move( poly ) );
}

// Binds the right vk.polyPipeline* variant for img's blend mode (only if
// it's not already bound - same "skip redundant binds" convention as
// RE_StretchPic/VK_DrawGhoul2Entities), binds img's descriptor set, and
// draws vertexCount non-indexed vertices starting at firstVertex within
// vk.polyVertexBuffer. Shared by the RE_AddPolyToScene fan-expansion loop
// and every quad/tube loop below - they only differ in how they fill
// PolyVertex data, not in how they get drawn. forcedPipeline overrides the
// img->blendMode lookup entirely when set (VK_NULL_HANDLE, the default,
// means "look it up normally") - RT_SABER_GLOW and RT_BEAM need this
// because rd-vanilla's real RB_SurfaceSaberGlow/RB_SurfaceBeam
// (tr_surface.cpp) both hardcode additive blending via GL_State
// unconditionally, ignoring whatever the entity's own shader/customShader
// would have specified.
static void VK_DrawPolyRange( VkCommandBuffer cmd, image_t *img, uint32_t firstVertex, uint32_t vertexCount,
	VkPipeline forcedPipeline = VK_NULL_HANDLE )
{
	VkPipeline pipeline = forcedPipeline;
	if ( !pipeline )
	{
		pipeline = vk.polyPipeline;
		if ( img->blendMode == BLEND_ADDITIVE )
		{
			pipeline = vk.polyPipelineAdditive;
		}
		else if ( img->blendMode == BLEND_OPAQUE )
		{
			pipeline = vk.polyPipelineOpaque;
		}
	}
	if ( pipeline != vk.lastBoundPipeline )
	{
		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
		vk.lastBoundPipeline = pipeline;
	}

	// Reusing img->descriptorSet (built once at upload time against
	// vk.uiDescriptorSetLayout/vk.uiSampler - VK_UploadImage, tr_image.cpp)
	// against vk.polyPipelineLayout works because that layout's one set
	// layout is vk.uiDescriptorSetLayout too - see vkGlobals_t::
	// polyPipelineLayout's comment (tr_local.h).
	vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.polyPipelineLayout,
		0, 1, &img->descriptorSet, 0, nullptr );

	VkDeviceSize offset = (VkDeviceSize)firstVertex * sizeof( PolyVertex );
	vkCmdBindVertexBuffers( cmd, 0, 1, &vk.polyVertexBuffer, &offset );
	vkCmdDraw( cmd, vertexCount, 1, 0, 0 );
}

// Writes one screen-facing/oriented quad (RT_SPRITE, RT_ORIENTED_QUAD) into
// vk.polyVertexBufferMapped at *cursor as 6 non-indexed vertices, advancing
// *cursor by 6. Corner positions and winding match rd-vanilla's real
// RB_AddQuadStampExt (tr_surface.cpp) exactly: corners at
// origin +-left +-up with triangles (0,1,3) and (3,1,2) - re-expressed here
// as a plain 6-vertex sequence in that same order since this renderer's
// poly path has no index buffer (see poly.vert's comment). color is the
// entity's shaderRGBA, constant across all 4 corners, same as rd-vanilla.
static void VK_EmitQuadStamp( const float origin[3], const float left[3], const float up[3],
	const byte color[4], uint32_t *cursor )
{
	float corners[4][3];
	for ( int i = 0; i < 3; i++ )
	{
		corners[0][i] = origin[i] + left[i] + up[i];
		corners[1][i] = origin[i] - left[i] + up[i];
		corners[2][i] = origin[i] - left[i] - up[i];
		corners[3][i] = origin[i] + left[i] - up[i];
	}
	static const float cornerUv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	static const int winding[6] = { 0, 1, 3, 3, 1, 2 };

	PolyVertex *out = (PolyVertex *)vk.polyVertexBufferMapped + *cursor;
	for ( int i = 0; i < 6; i++ )
	{
		int c = winding[i];
		out[i].pos[0] = corners[c][0];
		out[i].pos[1] = corners[c][1];
		out[i].pos[2] = corners[c][2];
		out[i].uv[0] = cornerUv[c][0];
		out[i].uv[1] = cornerUv[c][1];
		out[i].color[0] = color[0] / 255.0f;
		out[i].color[1] = color[1] / 255.0f;
		out[i].color[2] = color[2] / 255.0f;
		out[i].color[3] = color[3] / 255.0f;
	}
	*cursor += 6;
}

// RT_ELECTRICITY support (lightning bolts) - see VK_DrawScenePolys' own
// RT_ELECTRICITY loop for the top-level entry point and README.md's "Lightning
// (electricity) ref entities" section for the full algorithm explanation.
// Split into three functions mirroring rd-vanilla's real
// DoLine2/ApplyShape/DoBoltSeg (tr_surface.cpp) one-for-one, rather than
// flattened, because the real functions are genuinely mutually recursive
// (ApplyShape calls itself; DoBoltSeg calls ApplyShape and, when forking,
// itself) - flattening would obscure the real algorithm's shape rather
// than simplify it.

// Emits one DoLine2-equivalent quad (2 triangles: (0,1,2),(2,1,3), the same
// winding DoLine/RT_LINE above use) with two independent widths (sradius at
// start, eradius at end, since a bolt segment tapers) and two independent
// texture-coordinate V values - real DoLine2's exact vertex/uv layout,
// copied rather than reusing VK_EmitQuadStamp (which assumes one shared
// width and a fixed 0/1 UV square, neither true here). Draws immediately
// with its own VK_DrawPolyRange call, same one-draw-call-per-emitted-quad
// style already established for RT_SABER_GLOW above, rather than batching -
// simplest given how deeply nested and data-dependent this call site is.
static void VK_EmitElectricityQuad( VkCommandBuffer cmd, image_t *img,
	const float start[3], const float end[3], const float right[3],
	float startWidth, float endWidth, float tcStart, float tcEnd,
	const byte color[4], uint32_t *cursor )
{
	if ( *cursor + 6 > POLY_VERTEX_BUFFER_CAPACITY )
	{
		// out of per-frame scratch space - drop this and every later quad
		// rather than corrupt the buffer, same convention as everywhere else.
		return;
	}

	float p0[3], p1[3], p2[3], p3[3];
	VectorMA( start, startWidth, right, p0 );
	VectorMA( start, -startWidth, right, p1 );
	VectorMA( end, endWidth, right, p2 );
	VectorMA( end, -endWidth, right, p3 );

	const float *corners[6] = { p0, p1, p2, p2, p1, p3 };
	const float cornerUv[6][2] = {
		{ 0, tcStart }, { 1, tcStart }, { 0, tcEnd },
		{ 0, tcEnd }, { 1, tcStart }, { 1, tcEnd },
	};

	PolyVertex *out = (PolyVertex *)vk.polyVertexBufferMapped + *cursor;
	for ( int i = 0; i < 6; i++ )
	{
		out[i].pos[0] = corners[i][0];
		out[i].pos[1] = corners[i][1];
		out[i].pos[2] = corners[i][2];
		out[i].uv[0] = cornerUv[i][0];
		out[i].uv[1] = cornerUv[i][1];
		out[i].color[0] = color[0] / 255.0f;
		out[i].color[1] = color[1] / 255.0f;
		out[i].color[2] = color[2] / 255.0f;
		out[i].color[3] = color[3] / 255.0f;
	}

	VK_DrawPolyRange( cmd, img, *cursor, 6 );
	*cursor += 6;
}

// Real ApplyShape (tr_surface.cpp): recursively splits [start,end] into a
// jittered ternary "fractal" tree - each level picks two random points
// (CreateShape's sh1/sh2, real Q_flrand values, the global RNG, not
// e->frame-seeded like DoBoltSeg's jitter below) biased off the ideal
// straight line and off to either side (rt/up, MakeNormalVectors of the
// segment's own direction), then recurses into 3 child segments one level
// shallower, terminating (drawing one quad via VK_EmitElectricityQuad,
// rd-vanilla's real DoLine2) at count == 0. sradius/eradius are the bolt's
// width at each end, interpolated 2/3-1/3 into rads1/rads2 for the child
// segments exactly as the real code does. startPerc/endPerc are texture-V
// coordinates tracking how far along the *original*, pre-split segment
// this leaf quad falls, so the texture doesn't restart at every split.
static void VK_ApplyElectricityShape( VkCommandBuffer cmd, image_t *img,
	const float start[3], const float end[3], const float right[3],
	float sradius, float eradius, int count, float startPerc, float endPerc,
	const byte color[4], uint32_t *cursor )
{
	if ( count < 1 )
	{
		VK_EmitElectricityQuad( cmd, img, start, end, right, sradius, eradius, startPerc, endPerc, color, cursor );
		return;
	}

	// CreateShape, inlined (real tr_surface.cpp keeps sh1/sh2 as module-
	// static scratch reused across calls; here they're plain locals -
	// every use happens before any recursive call could overwrite them, so
	// this is behaviorally identical without needing shared mutable state).
	float sh1[3], sh2[3];
	sh1[0] = 0.66f;
	sh1[1] = 0.08f + Q_flrand( -1.0f, 1.0f ) * 0.02f;
	sh1[2] = 0.08f + Q_flrand( -1.0f, 1.0f ) * 0.02f;
	sh2[0] = 0.33f;
	sh2[1] = -sh1[1] + Q_flrand( -1.0f, 1.0f ) * 0.02f;
	sh2[2] = -sh1[2] + Q_flrand( -1.0f, 1.0f ) * 0.02f;

	float fwd[3];
	VectorSubtract( end, start, fwd );
	float dis = VectorNormalize( fwd ) * 0.7f;
	float rt[3], up[3];
	MakeNormalVectors( fwd, rt, up );

	float perc = sh1[0];
	float point1[3];
	VectorScale( start, perc, point1 );
	VectorMA( point1, 1.0f - perc, end, point1 );
	VectorMA( point1, dis * sh1[1], rt, point1 );
	VectorMA( point1, dis * sh1[2], up, point1 );

	float rads1 = sradius * 0.666f + eradius * 0.333f;
	float rads2 = sradius * 0.333f + eradius * 0.666f;

	VK_ApplyElectricityShape( cmd, img, start, point1, right, sradius, rads1, count - 1,
		startPerc, startPerc * 0.666f + endPerc * 0.333f, color, cursor );

	perc = sh2[0];
	float point2[3];
	VectorScale( start, perc, point2 );
	VectorMA( point2, 1.0f - perc, end, point2 );
	VectorMA( point2, dis * sh2[1], rt, point2 );
	VectorMA( point2, dis * sh2[2], up, point2 );

	VK_ApplyElectricityShape( cmd, img, point2, point1, right, rads1, rads2, count - 1,
		startPerc * 0.333f + endPerc * 0.666f, startPerc * 0.666f + endPerc * 0.333f, color, cursor );
	VK_ApplyElectricityShape( cmd, img, point2, end, right, rads2, eradius, count - 1,
		startPerc * 0.333f + endPerc * 0.666f, endPerc, color, cursor );
}

// Real DoBoltSeg (tr_surface.cpp): walks from start to end in fixed 16-unit
// steps, each step jittering "off" (a running, ever-accumulating offset -
// not reset per step) by a random amount seeded from rngSeed (the entity's
// own e->frame, threaded through by pointer exactly like the real code
// threads &e->frame - the one part of this whole algorithm that's
// per-entity-deterministic rather than using the engine's global RNG), then
// calls VK_ApplyElectricityShape once per step for the segment from the
// previous step's point to this one. chaosScale is e->angles[0] (RF_FORKED/
// RF_TAPERED bolts overload angles[0] as a jitter multiplier - see
// refEntity_t, rd-common/tr_types.h); topLevelEnd is the *entity's* real
// endpoint (constant across the whole recursive call tree for one entity,
// unlike the `end` parameter which is only this particular segment's local
// end) - forking needs it because a fork's random destination is always
// biased toward the bolt's true target, not toward whatever sub-segment
// happened to spawn the fork.
static void VK_DoElectricityBoltSeg( VkCommandBuffer cmd, image_t *img,
	const float start[3], const float end[3], const float right[3], float radius,
	const float topLevelEnd[3], int renderfx, float chaosScale,
	int *rngSeed, int *forkBudget, int lodCount, int recursionDepth,
	const byte color[4], uint32_t *cursor )
{
	float fwd[3];
	VectorSubtract( end, start, fwd );
	float dis = VectorNormalize( fwd );
	if ( dis > 2000.0f )
	{
		// "freaky long" - real code's own comment, same clamp.
		dis = 2000.0f;
	}
	float rt[3], up[3];
	MakeNormalVectors( fwd, rt, up );

	float old[3];
	VectorCopy( start, old );
	float oldRadius = radius, newRadius = radius;
	float off[3] = { 10.0f, 10.0f, 10.0f };
	float oldPerc = 0.0f;

	// Defensive addition, not part of the real algorithm: real DoBoltSeg's
	// forking is already self-limiting (forkBudget only ever decreases, 3
	// per top-level RT_ELECTRICITY entity - see the real f_count), so this
	// cap on *recursion depth* specifically only matters if forkBudget were
	// ever raised without also raising this, and guards against a future
	// edit doing that by accident rather than a real observed problem.
	const int kMaxForkDepth = 8;

	for ( float i = 16.0f; i <= dis; i += 16.0f )
	{
		float perc = ( i + 16.0f > dis ) ? 1.0f : ( i / dis );

		float temp[3];
		VectorScale( fwd, Q_crandom( rngSeed ) * 3.0f, temp );
		VectorMA( temp, Q_crandom( rngSeed ) * 7.0f * chaosScale, rt, temp );
		VectorMA( temp, Q_crandom( rngSeed ) * 7.0f * chaosScale, up, temp );
		VectorAdd( off, temp, off );

		float cur[3];
		VectorAdd( start, off, cur );
		VectorScale( cur, 1.0f - perc, cur );
		VectorMA( cur, perc, end, cur );

		if ( renderfx & RF_TAPERED )
		{
			oldRadius = radius * ( 1.0f - oldPerc * oldPerc );
			newRadius = radius * ( 1.0f - perc * perc );
		}

		VK_ApplyElectricityShape( cmd, img, cur, old, right, newRadius, oldRadius,
			lodCount, 0.0f, 1.0f, color, cursor );

		if ( ( renderfx & RF_FORKED ) && *forkBudget > 0 && recursionDepth < kMaxForkDepth &&
			Q_random( rngSeed ) > 0.93f && ( 1.0f - perc ) > 0.8f )
		{
			( *forkBudget )--;

			float newDest[3];
			VectorAdd( cur, topLevelEnd, newDest );
			VectorScale( newDest, 0.5f, newDest );
			for ( int t = 0; t < 3; t++ )
			{
				newDest[t] += Q_crandom( rngSeed ) * 80.0f;
			}

			VK_DoElectricityBoltSeg( cmd, img, cur, newDest, right, newRadius,
				topLevelEnd, renderfx, chaosScale, rngSeed, forkBudget, lodCount,
				recursionDepth + 1, color, cursor );
		}

		VectorCopy( cur, old );
		oldPerc = perc;
	}
}

void VK_DrawScenePolys( const float *mvp, const refdef_t *fd )
{
	if ( ( s_scenePolys.empty() && s_sceneEntities.empty() ) || !vk.frameActive )
	{
		return;
	}

	VkCommandBuffer cmd = vk.activeCommandBuffer;

	// mvp is the same for every poly/sprite this scene (world-space
	// geometry, no per-entity model matrix like Ghoul2's) - push once
	// rather than per draw. Push constants aren't tied to whichever
	// pipeline is bound at push time, only to the layout used by the
	// eventual draw call, and every variant below shares
	// vk.polyPipelineLayout.
	vkCmdPushConstants( cmd, vk.polyPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof( float ) * 16, mvp );

	// Reset once per scene, not once per poly/sprite - see POLY_VERTEX_
	// BUFFER_CAPACITY's comment (tr_local.h): unlike the UI path's
	// per-frame, many-interleaved-draws cursor, RE_RenderScene draws the
	// whole queue in one pass here. Shared by both loops below (rather than
	// each resetting its own) because both write into the same
	// vk.polyVertexBuffer during command *recording*, but the GPU only
	// reads it at *execution* time, after every write below has already
	// happened - two independently-reset cursors could have the sprite
	// loop's writes silently clobber data an earlier poly draw call still
	// needs, the same class of bug documented in README.md's "Live
	// animation" section for Ghoul2's per-instance skin buffers.
	uint32_t cursor = 0;

	for ( const VulkanScenePoly &poly : s_scenePolys )
	{
		// Triangle fan (vertex 0 is the pivot) -> triangle list: (0, i+1, i+2)
		// for i in [0, numVerts-2), same convention as rd-vanilla's real
		// RB_SurfacePolychain (tr_surface.cpp) - see poly.vert's comment for
		// why this renderer expands on the CPU instead of using fan topology.
		int numTris = (int)poly.verts.size() - 2;
		int numOutVerts = numTris * 3;
		if ( numOutVerts <= 0 )
		{
			continue;
		}
		if ( cursor + (uint32_t)numOutVerts > POLY_VERTEX_BUFFER_CAPACITY )
		{
			// out of per-frame scratch space - drop the rest rather than corrupt the buffer
			break;
		}

		image_t *img = VK_GetImageByHandle( poly.hShader );
		if ( !img )
		{
			continue;
		}

		PolyVertex *out = (PolyVertex *)vk.polyVertexBufferMapped + cursor;
		int outIdx = 0;
		for ( int i = 0; i < numTris; i++ )
		{
			const polyVert_t *fanVerts[3] = { &poly.verts[0], &poly.verts[i + 1], &poly.verts[i + 2] };
			for ( int v = 0; v < 3; v++ )
			{
				const polyVert_t &src = *fanVerts[v];
				PolyVertex &dst = out[outIdx++];
				dst.pos[0] = src.xyz[0];
				dst.pos[1] = src.xyz[1];
				dst.pos[2] = src.xyz[2];
				dst.uv[0] = src.st[0];
				dst.uv[1] = src.st[1];
				// modulate is byte 0-255 (polyVert_t, rd-common/tr_types.h) -
				// see PolyVertex's comment (tr_local.h) for why this is the
				// one place that conversion happens.
				dst.color[0] = src.modulate[0] / 255.0f;
				dst.color[1] = src.modulate[1] / 255.0f;
				dst.color[2] = src.modulate[2] / 255.0f;
				dst.color[3] = src.modulate[3] / 255.0f;
			}
		}

		VK_DrawPolyRange( cmd, img, cursor, (uint32_t)numOutVerts );
		cursor += (uint32_t)numOutVerts;
	}

	// RT_SPRITE (camera-facing billboard) and RT_ORIENTED_QUAD (quad facing
	// a fixed direction, ent.axis[0]) - both a single quad_stamp using the
	// entity's own hShader (here: customShader, see R_AddEntitySurfaces'
	// real dispatch in tr_main.cpp for why it's that field and not hModel),
	// radius, rotation, and shaderRGBA. Every other refEntityType_t is still
	// ignored (see s_sceneEntities' comment).
	//
	// One-time-ish diagnostic (mirrors VK_DrawGhoul2Entities' own
	// s_debugEntityLogsRemaining): confirms sprite/quad entities are
	// actually reaching this function, independent of whether the result is
	// visually obvious in any one screenshot.
	static int s_debugSpriteLogsRemaining = 3;
	int drawnSpriteCount = 0, skippedThirdPersonCount = 0;

	for ( const refEntity_t &ent : s_sceneEntities )
	{
		if ( ent.reType != RT_SPRITE && ent.reType != RT_ORIENTED_QUAD )
		{
			continue;
		}
		// Real R_AddEntitySurfaces (tr_main.cpp) skips these entities
		// entirely when the current view is third-person-only and not a
		// portal/mirror ("self blood sprites, talk balloons, etc should not
		// be drawn in the primary view"). This renderer has no portal/
		// mirror support (see README.md), so its one and only rendered view
		// is always that primary, non-portal view - the condition is
		// unconditionally true here, not situationally skipped.
		if ( ent.renderfx & RF_THIRD_PERSON )
		{
			skippedThirdPersonCount++;
			continue;
		}
		if ( cursor + 6 > POLY_VERTEX_BUFFER_CAPACITY )
		{
			break;
		}

		image_t *img = VK_GetImageByHandle( ent.customShader );
		if ( !img )
		{
			continue;
		}

		float left[3], up[3];
		float radius = ent.radius;
		if ( ent.reType == RT_SPRITE )
		{
			// Camera-facing: left/up come from the view's own axes (axis[1]/
			// axis[2] - see VK_BuildViewMatrix's comment in tr_world.cpp for
			// why fd->viewaxis uses that same [forward, left, up] layout),
			// exactly like rd-vanilla's real RB_SurfaceSprite
			// (tr_surface.cpp) uses backEnd.viewParms.ori.axis[1]/[2].
			if ( ent.rotation == 0.0f )
			{
				VectorScale( fd->viewaxis[1], radius, left );
				VectorScale( fd->viewaxis[2], radius, up );
			}
			else
			{
				float ang = (float)M_PI * ent.rotation / 180.0f;
				float s = sinf( ang ), c = cosf( ang );
				VectorScale( fd->viewaxis[1], c * radius, left );
				VectorMA( left, -s * radius, fd->viewaxis[2], left );
				VectorScale( fd->viewaxis[2], c * radius, up );
				VectorMA( up, s * radius, fd->viewaxis[1], up );
			}
		}
		else // RT_ORIENTED_QUAD
		{
			// Fixed orientation: left/up are perpendiculars of the entity's
			// own axis[0] (its facing direction), not the camera's - exactly
			// rd-vanilla's real RB_SurfaceOrientedQuad (tr_surface.cpp),
			// MakeNormalVectors and all (shared/qcommon/q_math.c - the same
			// real function, not reimplemented here).
			MakeNormalVectors( ent.axis[0], left, up );
			if ( ent.rotation == 0.0f )
			{
				VectorScale( left, radius, left );
				VectorScale( up, radius, up );
			}
			else
			{
				float ang = (float)M_PI * ent.rotation / 180.0f;
				float s = sinf( ang ), c = cosf( ang );
				float tempLeft[3], tempUp[3];
				VectorScale( left, c * radius, tempLeft );
				VectorMA( tempLeft, -s * radius, up, tempLeft );
				VectorScale( up, c * radius, tempUp );
				VectorMA( tempUp, s * radius, left, up );
				VectorCopy( tempLeft, left );
			}
		}

		uint32_t firstVertex = cursor;
		VK_EmitQuadStamp( ent.origin, left, up, ent.shaderRGBA, &cursor );
		VK_DrawPolyRange( cmd, img, firstVertex, 6 );
		drawnSpriteCount++;
	}

	if ( s_debugSpriteLogsRemaining > 0 && ( drawnSpriteCount > 0 || skippedThirdPersonCount > 0 ) )
	{
		s_debugSpriteLogsRemaining--;
		ri.Printf( PRINT_ALL, "rd-vulkan: sprite/oriented-quad entities: %d drawn, %d skipped (third person)\n",
			drawnSpriteCount, skippedThirdPersonCount );
	}

	// RT_SABER_GLOW - every lightsaber blade's soft glow (the blade core
	// itself is a separate, already-working RT_MODEL/Ghoul2 or world
	// surface; this is only the additive halo around it). rd-vanilla's real
	// RB_SurfaceSaberGlow (tr_surface.cpp) is a short loop of DoSprite calls
	// (itself just RB_AddQuadStamp with rotation 0, i.e. exactly
	// VK_EmitQuadStamp with left/up from the *camera's* axes, same as
	// RT_SPRITE above) marching outward along the blade's own axis[0] from
	// the tip (ent.saberLength, the same union member as ent.rotation -
	// see refEntity_t, rd-common/tr_types.h) back to the hilt, growing the
	// sprite radius slightly each step, plus one final bigger "hilt glow"
	// sprite at the entity's origin. Both DoSprite and the real code's
	// GL_Bind/GL_State hardcode tr.whiteImage and additive blending
	// unconditionally - the entity's own customShader is never resolved or
	// bound for this reType, so this renderer does the same (vk.whiteImage,
	// forcedPipeline = vk.polyPipelineAdditive) rather than looking one up
	// that real vanilla would have ignored anyway.
	static int s_debugSaberGlowLogsRemaining = 3;
	int drawnSaberGlowCount = 0;
	for ( const refEntity_t &ent : s_sceneEntities )
	{
		if ( ent.reType != RT_SABER_GLOW )
		{
			continue;
		}
		if ( ent.renderfx & RF_THIRD_PERSON )
		{
			continue;
		}

		float radius = ent.radius;
		float i = ent.saberLength;
		// Real code's step (radius * 0.65f) only shrinks as far as radius
		// grows, never reaching zero or flipping sign for any sane
		// saberLength/radius - but nothing here stops a corrupt or
		// pathological entity (radius <= 0) from making the step size
		// non-positive, which would spin i > 0 forever. This cap is a
		// defensive addition, not part of the real algorithm: real
		// saberLength/radius values need nowhere near this many segments
		// (typically a few dozen), so it never triggers in practice.
		const int kMaxSaberGlowSegments = 256;
		for ( int seg = 0; seg < kMaxSaberGlowSegments && i > 0.0f; seg++ )
		{
			if ( cursor + 6 > POLY_VERTEX_BUFFER_CAPACITY )
			{
				break;
			}
			float segOrigin[3];
			VectorMA( ent.origin, i, ent.axis[0], segOrigin );
			float left[3], up[3];
			VectorScale( fd->viewaxis[1], radius, left );
			VectorScale( fd->viewaxis[2], radius, up );
			uint32_t firstVertex = cursor;
			VK_EmitQuadStamp( segOrigin, left, up, ent.shaderRGBA, &cursor );
			VK_DrawPolyRange( cmd, vk.whiteImage, firstVertex, 6, vk.polyPipelineAdditive );
			i -= radius * 0.65f;
			radius += 0.017f;
		}

		if ( cursor + 6 <= POLY_VERTEX_BUFFER_CAPACITY )
		{
			// Big hilt sprite - real code re-rolls a small random size each
			// frame (Q_flrand(0,1)*0.25), a deliberate subtle pulse per its
			// own comment, not something to make deterministic.
			float hiltRadius = 5.5f + Q_flrand( 0.0f, 1.0f ) * 0.25f;
			float left[3], up[3];
			VectorScale( fd->viewaxis[1], hiltRadius, left );
			VectorScale( fd->viewaxis[2], hiltRadius, up );
			uint32_t firstVertex = cursor;
			VK_EmitQuadStamp( ent.origin, left, up, ent.shaderRGBA, &cursor );
			VK_DrawPolyRange( cmd, vk.whiteImage, firstVertex, 6, vk.polyPipelineAdditive );
		}
		drawnSaberGlowCount++;
	}

	if ( s_debugSaberGlowLogsRemaining > 0 && drawnSaberGlowCount > 0 )
	{
		s_debugSaberGlowLogsRemaining--;
		ri.Printf( PRINT_ALL, "rd-vulkan: RT_SABER_GLOW entities drawn: %d\n", drawnSaberGlowCount );
	}

	// RT_BEAM - a 6-sided open tube from ent.origin to ent.oldorigin,
	// flat-colored by ent.skinNum (0/1/2 = red/green/blue, the real
	// RB_SurfaceBeam switch's exact values copied verbatim) and additively
	// blended over a hardcoded white texture, same "ignore customShader"
	// pattern as RT_SABER_GLOW above. NUM_BEAM_SEGS, PerpendicularVector,
	// and RotatePointAroundVector are all copied from the real
	// RB_SurfaceBeam (tr_surface.cpp) - the latter two are the real shared
	// functions (shared/qcommon/q_math.c), not reimplemented here.
	//
	// One real quirk preserved verbatim, not "fixed": rd-vanilla's
	// RB_SurfaceEntity dispatch never applies an entity transform for any
	// non-RT_MODEL reType (R_RotateForEntity, tr_main.cpp, returns the
	// plain view/world matrix unchanged for those), and RB_SurfaceBeam's
	// start_points are never translated by ent.origin (see its own
	// commented-out `// VectorAdd( start_points[i], origin, ... )`) - so a
	// real RT_BEAM always renders anchored near world-space (0,0,0),
	// using only the *direction* from origin to oldorigin, never their
	// actual position. Surprising, but this renderer's job is to
	// reproduce rd-vanilla's real behavior, not to guess at a "more
	// correct" one it doesn't actually have.
	static int s_debugBeamLogsRemaining = 3;
	int drawnBeamCount = 0;
	const int kBeamSegs = 6;
	for ( const refEntity_t &ent : s_sceneEntities )
	{
		if ( ent.reType != RT_BEAM )
		{
			continue;
		}
		if ( ent.renderfx & RF_THIRD_PERSON )
		{
			continue;
		}

		float direction[3];
		VectorSubtract( ent.oldorigin, ent.origin, direction );
		float normalizedDirection[3];
		VectorCopy( direction, normalizedDirection );
		if ( VectorNormalize( normalizedDirection ) == 0.0f )
		{
			continue;
		}

		float perp[3];
		PerpendicularVector( perp, normalizedDirection );
		VectorScale( perp, 4.0f, perp );

		float startPoints[kBeamSegs][3], endPoints[kBeamSegs][3];
		for ( int i = 0; i < kBeamSegs; i++ )
		{
			RotatePointAroundVector( startPoints[i], normalizedDirection, perp, ( 360.0f / kBeamSegs ) * i );
			VectorAdd( startPoints[i], direction, endPoints[i] );
		}

		// Closed triangle-strip ring, i in [0, kBeamSegs] inclusive (the
		// extra iteration closes the tube back to segment 0) - matching the
		// real qglBegin(GL_TRIANGLE_STRIP) loop's vertex order exactly, just
		// re-expressed as an explicit strip-to-list expansion below since
		// this renderer's poly path has no strip topology (see poly.vert's
		// comment). Triangle winding isn't preserved from the strip's
		// alternating parity because it doesn't need to be: VK_
		// CreatePolyPipeline disables backface culling entirely for this
		// pipeline (tr_init.cpp), so both windings render identically.
		const int numStripVerts = 2 * ( kBeamSegs + 1 );
		float stripVerts[numStripVerts][3];
		for ( int i = 0; i <= kBeamSegs; i++ )
		{
			VectorCopy( startPoints[i % kBeamSegs], stripVerts[2 * i] );
			VectorCopy( endPoints[i % kBeamSegs], stripVerts[2 * i + 1] );
		}

		int numOutVerts = ( numStripVerts - 2 ) * 3;
		if ( cursor + (uint32_t)numOutVerts > POLY_VERTEX_BUFFER_CAPACITY )
		{
			break;
		}

		byte color[4];
		switch ( ent.skinNum )
		{
			case 1: color[0] = 0;   color[1] = 255; color[2] = 0;   break; // green
			case 2: color[0] = 128; color[1] = 128; color[2] = 255; break; // blue (0.5, 0.5, 1)
			case 0:
			default: color[0] = 255; color[1] = 0; color[2] = 0; break; // red
		}
		color[3] = 255;

		PolyVertex *out = (PolyVertex *)vk.polyVertexBufferMapped + cursor;
		int outIdx = 0;
		for ( int k = 0; k + 2 < numStripVerts; k++ )
		{
			for ( int v = 0; v < 3; v++ )
			{
				PolyVertex &dst = out[outIdx++];
				dst.pos[0] = stripVerts[k + v][0];
				dst.pos[1] = stripVerts[k + v][1];
				dst.pos[2] = stripVerts[k + v][2];
				// UV doesn't matter (flat white texture) - real code never
				// specified texcoords for this immediate-mode draw either.
				dst.uv[0] = 0.0f;
				dst.uv[1] = 0.0f;
				dst.color[0] = color[0] / 255.0f;
				dst.color[1] = color[1] / 255.0f;
				dst.color[2] = color[2] / 255.0f;
				dst.color[3] = color[3] / 255.0f;
			}
		}

		VK_DrawPolyRange( cmd, vk.whiteImage, cursor, (uint32_t)numOutVerts, vk.polyPipelineAdditive );
		cursor += (uint32_t)numOutVerts;
		drawnBeamCount++;
	}

	if ( s_debugBeamLogsRemaining > 0 && drawnBeamCount > 0 )
	{
		s_debugBeamLogsRemaining--;
		ri.Printf( PRINT_ALL, "rd-vulkan: RT_BEAM entities drawn: %d\n", drawnBeamCount );
	}

	// RT_LINE - a single flat quad from ent.origin to ent.oldorigin, always
	// facing the viewer edge-on (its "up"/width axis is perpendicular to
	// both the view origin and the line itself, not the view's own up
	// vector - see the cross-product below), used for tracer-like effects.
	// Unlike RT_SABER_GLOW/RT_BEAM above, this one *does* resolve and bind
	// the entity's real customShader (it's in R_AddEntitySurfaces' normal
	// "shader = R_GetShaderByHandle(...)" bucket, tr_main.cpp, not the
	// hardcoded-white-additive group), so it goes through VK_DrawPolyRange
	// the same un-forced way RT_SPRITE does. Copied from rd-vanilla's real
	// RB_SurfaceLine/DoLine (tr_surface.cpp).
	static int s_debugLineLogsRemaining = 3;
	int drawnLineCount = 0;
	for ( const refEntity_t &ent : s_sceneEntities )
	{
		if ( ent.reType != RT_LINE )
		{
			continue;
		}
		if ( ent.renderfx & RF_THIRD_PERSON )
		{
			continue;
		}
		if ( cursor + 6 > POLY_VERTEX_BUFFER_CAPACITY )
		{
			break;
		}

		image_t *img = VK_GetImageByHandle( ent.customShader );
		if ( !img )
		{
			continue;
		}

		// "right" (really the line's width axis) = normalize(cross(origin -
		// vieworg, oldorigin - vieworg)) - real RB_SurfaceLine's exact
		// construction, not an arbitrary perpendicular: it keeps the quad
		// edge-on to the viewer regardless of the line's own orientation.
		float v1[3], v2[3], right[3];
		VectorSubtract( ent.origin, fd->vieworg, v1 );
		VectorSubtract( ent.oldorigin, fd->vieworg, v2 );
		CrossProduct( v1, v2, right );
		VectorNormalize( right );

		float p0[3], p1[3], p2[3], p3[3];
		VectorMA( ent.origin, ent.radius, right, p0 );
		VectorMA( ent.origin, -ent.radius, right, p1 );
		VectorMA( ent.oldorigin, ent.radius, right, p2 );
		VectorMA( ent.oldorigin, -ent.radius, right, p3 );

		// Real DoLine's exact vertex/uv order and triangles (0,1,2),(2,1,3).
		const float *corners[6] = { p0, p1, p2, p2, p1, p3 };
		const float cornerUv[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };

		PolyVertex *out = (PolyVertex *)vk.polyVertexBufferMapped + cursor;
		for ( int i = 0; i < 6; i++ )
		{
			out[i].pos[0] = corners[i][0];
			out[i].pos[1] = corners[i][1];
			out[i].pos[2] = corners[i][2];
			out[i].uv[0] = cornerUv[i][0];
			out[i].uv[1] = cornerUv[i][1];
			out[i].color[0] = ent.shaderRGBA[0] / 255.0f;
			out[i].color[1] = ent.shaderRGBA[1] / 255.0f;
			out[i].color[2] = ent.shaderRGBA[2] / 255.0f;
			out[i].color[3] = ent.shaderRGBA[3] / 255.0f;
		}

		VK_DrawPolyRange( cmd, img, cursor, 6 );
		cursor += 6;
		drawnLineCount++;
	}

	if ( s_debugLineLogsRemaining > 0 && drawnLineCount > 0 )
	{
		s_debugLineLogsRemaining--;
		ri.Printf( PRINT_ALL, "rd-vulkan: RT_LINE entities drawn: %d\n", drawnLineCount );
	}

	// RT_CYLINDER - a tapered tube (cone when one end's radius is small
	// enough, plain cylinder otherwise) between ent.origin (bottom) and
	// ent.oldorigin (top), radius ent.radius at the bottom and ent.backlerp
	// at the top (real code overloads backlerp - normally a frame-lerp
	// fraction - as the top radius for this reType; see refEntity_t,
	// rd-common/tr_types.h, and RB_SurfaceCylinder's own comment). Like
	// RT_LINE above, this uses the entity's real customShader (same
	// R_AddEntitySurfaces bucket), not a hardcoded texture/blend.
	//
	// Segment count is view-distance-adaptive in the real code
	// (RB_SurfaceCylinder/RB_SurfaceCone, tr_surface.cpp) - copied
	// verbatim rather than this renderer's usual "fixed subdivision"
	// simplification (see "3D world geometry" in README.md for that
	// precedent with MST_PATCH) because it's cheap per-draw-call math, not
	// an asset-time tessellation choice.
	static int s_debugCylinderLogsRemaining = 3;
	int drawnCylinderCount = 0;
	const int kMaxCylinderSegments = 40; // real NUM_CYLINDER_SEGMENTS
	for ( const refEntity_t &ent : s_sceneEntities )
	{
		if ( ent.reType != RT_CYLINDER )
		{
			continue;
		}
		if ( ent.renderfx & RF_THIRD_PERSON )
		{
			continue;
		}

		image_t *img = VK_GetImageByHandle( ent.customShader );
		if ( !img )
		{
			continue;
		}

		float midpoint[3];
		VectorAdd( ent.origin, ent.oldorigin, midpoint );
		VectorScale( midpoint, 0.5f, midpoint );
		VectorSubtract( midpoint, fd->vieworg, midpoint );
		float length = VectorNormalize( midpoint );
		length *= fd->fov_x / 90.0f;

		int segments = (int)( kMaxCylinderSegments * ( 1.0f - length / 2048.0f ) );
		if ( segments < 8 ) segments = 8;
		if ( segments > kMaxCylinderSegments ) segments = kMaxCylinderSegments;

		float vr[3], vu[3];
		MakeNormalVectors( ent.axis[0], vr, vu );
		float detail = 360.0f / (float)segments;
		float texDetail = 1.0f / (float)segments;

		// One end sufficiently tapered -> treat as a cone (half the
		// triangles, better texture mapping at the point) - same threshold
		// as the real RB_SurfaceCylinder's early-out into RB_SurfaceCone.
		bool isCone = !( ent.radius < 0.3f && ent.backlerp < 0.3f ) && ( ent.radius < 0.3f || ent.backlerp < 0.3f );

		int numOutVerts = isCone ? segments * 3 : segments * 6;
		if ( cursor + (uint32_t)numOutVerts > POLY_VERTEX_BUFFER_CAPACITY )
		{
			break;
		}

		PolyVertex *out = (PolyVertex *)vk.polyVertexBufferMapped + cursor;
		int outIdx = 0;

		if ( isCone )
		{
			float vuScaled[3], base[3], tapered[3];
			if ( ent.radius < ent.backlerp )
			{
				VectorScale( vu, ent.backlerp, vuScaled );
				VectorCopy( ent.origin, base );
				VectorCopy( ent.oldorigin, tapered );
			}
			else
			{
				VectorScale( vu, ent.radius, vuScaled );
				VectorCopy( ent.origin, tapered );
				VectorCopy( ent.oldorigin, base );
			}

			float ring[kMaxCylinderSegments][3];
			for ( int i = 0; i < segments; i++ )
			{
				RotatePointAroundVector( ring[i], ent.axis[0], vuScaled, detail * i );
				VectorAdd( ring[i], base, ring[i] );
			}

			// Real indices reference (ring[i], tapered, ring[i+1]) per
			// segment (ring wraps to ring[0] on the last one) - one
			// triangle each, the "half as many indices as the cylinder"
			// the real comment describes.
			for ( int i = 0; i < segments; i++ )
			{
				const float *ringNext = ring[( i + 1 ) % segments];
				out[outIdx].pos[0] = ring[i][0]; out[outIdx].pos[1] = ring[i][1]; out[outIdx].pos[2] = ring[i][2];
				out[outIdx].uv[0] = texDetail * i; out[outIdx].uv[1] = 1.0f;
				outIdx++;
				out[outIdx].pos[0] = tapered[0]; out[outIdx].pos[1] = tapered[1]; out[outIdx].pos[2] = tapered[2];
				out[outIdx].uv[0] = texDetail * i + texDetail * 0.5f; out[outIdx].uv[1] = 0.0f;
				outIdx++;
				out[outIdx].pos[0] = ringNext[0]; out[outIdx].pos[1] = ringNext[1]; out[outIdx].pos[2] = ringNext[2];
				out[outIdx].uv[0] = texDetail * ( i + 1 ); out[outIdx].uv[1] = 1.0f;
				outIdx++;
			}
		}
		else
		{
			float vuTop[3], vuBottom[3];
			VectorScale( vu, ent.backlerp, vuTop );
			VectorScale( vu, ent.radius, vuBottom );

			float upper[kMaxCylinderSegments][3], lower[kMaxCylinderSegments][3];
			for ( int i = 0; i < segments; i++ )
			{
				RotatePointAroundVector( upper[i], ent.axis[0], vuTop, detail * i );
				VectorAdd( upper[i], ent.origin, upper[i] );
				RotatePointAroundVector( lower[i], ent.axis[0], vuBottom, detail * i );
				VectorAdd( lower[i], ent.oldorigin, lower[i] );
			}

			// Real indices per segment: (upper[i],lower[i],upper[i+1]) and
			// (upper[i+1],lower[i],lower[i+1]) - the same ring-quad-to-two-
			// triangles pattern as RT_BEAM above, just built directly
			// instead of via a strip.
			for ( int i = 0; i < segments; i++ )
			{
				const float *upperNext = upper[( i + 1 ) % segments];
				const float *lowerNext = lower[( i + 1 ) % segments];
				const float *quad[6] = { upper[i], lower[i], upperNext, upperNext, lower[i], lowerNext };
				float uCoord[6] = { texDetail * i, texDetail * i, texDetail * ( i + 1 ), texDetail * ( i + 1 ), texDetail * i, texDetail * ( i + 1 ) };
				float vCoord[6] = { 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f };
				for ( int v = 0; v < 6; v++ )
				{
					out[outIdx].pos[0] = quad[v][0]; out[outIdx].pos[1] = quad[v][1]; out[outIdx].pos[2] = quad[v][2];
					out[outIdx].uv[0] = uCoord[v]; out[outIdx].uv[1] = vCoord[v];
					outIdx++;
				}
			}
		}

		for ( int i = 0; i < numOutVerts; i++ )
		{
			out[i].color[0] = ent.shaderRGBA[0] / 255.0f;
			out[i].color[1] = ent.shaderRGBA[1] / 255.0f;
			out[i].color[2] = ent.shaderRGBA[2] / 255.0f;
			out[i].color[3] = ent.shaderRGBA[3] / 255.0f;
		}

		VK_DrawPolyRange( cmd, img, cursor, (uint32_t)numOutVerts );
		cursor += (uint32_t)numOutVerts;
		drawnCylinderCount++;
	}

	if ( s_debugCylinderLogsRemaining > 0 && drawnCylinderCount > 0 )
	{
		s_debugCylinderLogsRemaining--;
		ri.Printf( PRINT_ALL, "rd-vulkan: RT_CYLINDER entities drawn: %d\n", drawnCylinderCount );
	}

	// RT_ELECTRICITY - procedural lightning bolts. See VK_DoElectricityBoltSeg/
	// VK_ApplyElectricityShape/VK_EmitElectricityQuad above for the real
	// algorithm (copied from rd-vanilla's DoBoltSeg/ApplyShape/DoLine2,
	// tr_surface.cpp) and README.md's own section for the full explanation.
	// This top-level entry mirrors the real RB_SurfaceElectricity: compute
	// start/end (with optional RF_GROW animation shortening the visible
	// bolt over time), the view-relative "right" width axis (same
	// cross-product construction as RT_LINE above), reset the fork budget
	// to 3, and hand off to the recursive walk. Like RT_LINE/RT_CYLINDER,
	// this resolves the entity's real customShader (same
	// R_AddEntitySurfaces bucket), not a hardcoded texture.
	static int s_debugElectricityLogsRemaining = 3;
	int drawnElectricityCount = 0;
	for ( const refEntity_t &ent : s_sceneEntities )
	{
		if ( ent.reType != RT_ELECTRICITY )
		{
			continue;
		}
		if ( ent.renderfx & RF_THIRD_PERSON )
		{
			continue;
		}

		image_t *img = VK_GetImageByHandle( ent.customShader );
		if ( !img )
		{
			continue;
		}

		float start[3];
		VectorCopy( ent.origin, start );

		float fwd[3];
		VectorSubtract( ent.oldorigin, start, fwd );
		float dis = VectorNormalize( fwd );

		float perc = 1.0f;
		if ( ent.renderfx & RF_GROW )
		{
			// Real code divides by e->angles[1] (the bolt's grow duration,
			// another angles[] field reused as a scalar - see chaosScale's
			// comment above for the same pattern on angles[0]) with no
			// zero-guard; real code has this same division-by-zero
			// exposure (fd->time - e->endTime over 0), so a bolt entity
			// with angles[1] == 0 is already a malformed one in vanilla,
			// not a case this renderer needs to defend against specially.
			perc = 1.0f - ( ent.endTime - fd->time ) / ent.angles[1];
			if ( perc > 1.0f ) perc = 1.0f;
			else if ( perc < 0.0f ) perc = 0.0f;
		}

		float end[3];
		VectorMA( start, perc * dis, fwd, end );

		float v1[3], v2[3], right[3];
		VectorSubtract( start, fd->vieworg, v1 );
		VectorSubtract( end, fd->vieworg, v2 );
		CrossProduct( v1, v2, right );
		VectorNormalize( right );

		// r_lodbias isn't clamped in real code's "2 - r_lodbias->integer"
		// formula (ApplyShape's recursion depth, i.e. 3^lodCount leaf
		// quads) - a very negative r_lodbias would be a real, if
		// self-inflicted, way to hang real vanilla too. Clamped here as a
		// defensive addition (same spirit as RT_SABER_GLOW's segment cap
		// above): [0,6] still comfortably covers every real quality
		// setting (r_lodbias is normally 0-3, *reducing* detail) while
		// capping worst-case leaf count at 3^6 = 729 quads per bolt-walk
		// step instead of unbounded.
		int lodCount = 2 - r_lodbias->integer;
		if ( lodCount < 0 ) lodCount = 0;
		if ( lodCount > 6 ) lodCount = 6;

		int rngSeed = ent.frame;
		int forkBudget = 3; // real f_count's per-entity reset value

		VK_DoElectricityBoltSeg( cmd, img, start, end, right, ent.radius,
			end, ent.renderfx, ent.angles[0], &rngSeed, &forkBudget, lodCount, 0,
			ent.shaderRGBA, &cursor );
		drawnElectricityCount++;
	}

	if ( s_debugElectricityLogsRemaining > 0 && drawnElectricityCount > 0 )
	{
		s_debugElectricityLogsRemaining--;
		ri.Printf( PRINT_ALL, "rd-vulkan: RT_ELECTRICITY entities drawn: %d\n", drawnElectricityCount );
	}

	// RT_LATHE - a cubic-Bezier profile curve revolved ("lathed") a full
	// circle around a fixed world Z axis anchored at ent.origin. Copied
	// from rd-vanilla's real RB_SurfaceLathe (tr_surface.cpp). The profile
	// is a 2D (radius, height) curve with 4 control points - ent.axis[0]
	// (start), ent.axis[1]/ent.axis[2] (the two Bezier handles), and
	// ent.oldorigin (end) - only their X/Y components are used, so despite
	// the names these are curve-shape data here, not an orientation (same
	// "reused per-reType" pattern as every union/axis[]/backlerp field
	// elsewhere in this whole ref-entity series). ent.endTime optionally
	// grows the visible profile length over the real second before it
	// (real code: `d = 1 - (endTime - time)/1000`); ent.frame - here a real
	// timestamp of a recent hit, not an RNG seed like RT_ELECTRICITY's use
	// of the same field - drives a brief post-hit texture "pain" wobble
	// that decays over one second, using the entity's own floatTime
	// (`fd->time * 0.001 - ent.shaderTime`, matching the real per-entity
	// `backEnd.refdef.floatTime` computation, tr_backend.cpp - the first
	// ref-entity type in this series that actually needs it). Segment
	// counts (both along the profile and around the lathe) are LOD-scaled
	// by r_lodbias exactly like the real code - already clamped to a sane
	// [1,4] range by the real formula itself, so unlike RT_ELECTRICITY's
	// unclamped one, no extra defensive cap is needed here. Like
	// RT_LINE/RT_CYLINDER/RT_ELECTRICITY, this resolves the entity's real
	// customShader.
	static int s_debugLatheLogsRemaining = 3;
	int drawnLatheCount = 0;
	for ( const refEntity_t &ent : s_sceneEntities )
	{
		if ( ent.reType != RT_LATHE )
		{
			continue;
		}
		if ( ent.renderfx & RF_THIRD_PERSON )
		{
			continue;
		}

		image_t *img = VK_GetImageByHandle( ent.customShader );
		if ( !img )
		{
			continue;
		}

		float d = 1.0f;
		if ( ent.endTime != 0.0f && ent.endTime > (float)fd->time )
		{
			d = 1.0f - ( ent.endTime - (float)fd->time ) / 1000.0f;
		}
		float entityFloatTime = (float)fd->time * 0.001f - ent.shaderTime;
		float pain = 0.0f;
		if ( ent.frame != 0 && (float)ent.frame + 1000.0f > (float)fd->time )
		{
			pain = ( (float)fd->time - (float)ent.frame ) / 1000.0f;
			pain = ( 1.0f - pain ) * 0.08f;
		}

		int lod = r_lodbias->integer + 1;
		if ( lod > 4 ) lod = 4;
		if ( lod < 1 ) lod = 1;
		float bezierStep = 0.05f * lod;
		float latheStepDeg = 10.0f * lod;

		float lOldPt[2] = { ent.axis[0][0], ent.axis[0][1] };
		bool overflowed = false;

		for ( float mu = 0.0f; mu <= 1.01f * d && !overflowed; mu += bezierStep )
		{
			float mum1 = 1.0f - mu;
			float mum13 = mum1 * mum1 * mum1;
			float mu3 = mu * mu * mu;
			float group1 = 3.0f * mu * mum1 * mum1;
			float group2 = 3.0f * mu * mu * mum1;

			float lOldPt2[2];
			for ( int i = 0; i < 2; i++ )
			{
				lOldPt2[i] = mum13 * ent.axis[0][i] + group1 * ent.axis[1][i] + group2 * ent.axis[2][i] + mu3 * ent.oldorigin[i];
			}

			float oldPt[2] = { lOldPt[0], 0.0f };
			float oldPt2[2] = { lOldPt2[0], 0.0f };

			for ( float t = latheStepDeg; t <= 360.0f; t += latheStepDeg )
			{
				float s = sinf( DEG2RAD( t ) );
				float c = cosf( DEG2RAD( t ) );
				float pt[2] = { lOldPt[0] * c, lOldPt[0] * s };
				float pt2[2] = { lOldPt2[0] * c, lOldPt2[0] * s };

				if ( cursor + 6 > POLY_VERTEX_BUFFER_CAPACITY )
				{
					overflowed = true;
					break;
				}

				// Real code's texture-V wobble truncates this dot-product
				// to an int before adding floatTime (`int i = pt[0]*0.1f +
				// pt[1]*0.1f;`) - a real precision-losing quirk, preserved
				// rather than "fixed" (see README.md's policy on this).
				float corner[4][3] = {
					{ oldPt[0], oldPt[1], lOldPt[1] },
					{ oldPt2[0], oldPt2[1], lOldPt2[1] },
					{ pt[0], pt[1], lOldPt[1] },
					{ pt2[0], pt2[1], lOldPt2[1] },
				};
				float uv[4][2] = {
					{ ( t - latheStepDeg ) / 360.0f, mu - bezierStep + cosf( (float)(int)( oldPt[0] * 0.1f + oldPt[1] * 0.1f ) + entityFloatTime ) * pain },
					{ ( t - latheStepDeg ) / 360.0f, mu + cosf( (float)(int)( oldPt2[0] * 0.1f + oldPt2[1] * 0.1f ) + entityFloatTime ) * pain },
					{ t / 360.0f, mu - bezierStep + cosf( (float)(int)( pt[0] * 0.1f + pt[1] * 0.1f ) + entityFloatTime ) * pain },
					{ t / 360.0f, mu + cosf( (float)(int)( pt2[0] * 0.1f + pt2[1] * 0.1f ) + entityFloatTime ) * pain },
				};
				// Real indices (vbase,vbase+1,vbase+3),(vbase+3,vbase+2,vbase).
				const int order[6] = { 0, 1, 3, 3, 2, 0 };

				uint32_t firstVertex = cursor;
				PolyVertex *out = (PolyVertex *)vk.polyVertexBufferMapped + cursor;
				for ( int v = 0; v < 6; v++ )
				{
					int c4 = order[v];
					out[v].pos[0] = ent.origin[0] + corner[c4][0];
					out[v].pos[1] = ent.origin[1] + corner[c4][1];
					out[v].pos[2] = ent.origin[2] + corner[c4][2];
					out[v].uv[0] = uv[c4][0];
					out[v].uv[1] = uv[c4][1];
					out[v].color[0] = ent.shaderRGBA[0] / 255.0f;
					out[v].color[1] = ent.shaderRGBA[1] / 255.0f;
					out[v].color[2] = ent.shaderRGBA[2] / 255.0f;
					out[v].color[3] = ent.shaderRGBA[3] / 255.0f;
				}
				cursor += 6;
				VK_DrawPolyRange( cmd, img, firstVertex, 6 );

				oldPt[0] = pt[0]; oldPt[1] = pt[1];
				oldPt2[0] = pt2[0]; oldPt2[1] = pt2[1];
			}

			lOldPt[0] = lOldPt2[0]; lOldPt[1] = lOldPt2[1];
		}
		drawnLatheCount++;
	}

	if ( s_debugLatheLogsRemaining > 0 && drawnLatheCount > 0 )
	{
		s_debugLatheLogsRemaining--;
		ri.Printf( PRINT_ALL, "rd-vulkan: RT_LATHE entities drawn: %d\n", drawnLatheCount );
	}

	// RT_CLOUDS - a disk (default) or tube (RF_GROW) built from a small
	// fixed-size strip of (position, alpha, curve-height) keyframes lathed
	// around a full circle, 30 degrees per step. Copied from rd-vanilla's
	// real RB_SurfaceClouds (tr_surface.cpp). ent.radius/ent.rotation
	// define the outer/inner radius (`stripDef[i]*(radius-rotation) +
	// rotation`) and ent.backlerp scales the curve height - RF_GROW negates
	// backlerp ("needs to be reversed", the real comment) and switches to
	// the 6-keyframe tube table instead of the 4-keyframe disk one. Color
	// is a real, deliberate quirk worth keeping exactly as rd-vanilla has
	// it, not "fixed": RGB all come from `shaderRGBA[0]` (the red channel
	// only) times the keyframe's own alpha value, while the vertex alpha
	// itself stays constant at `shaderRGBA[3]` - the shape fades to black
	// at its edges rather than fading transparent, only reading right
	// under additive-style blending where black is already invisible.
	// Texture coords are the vertex's final world-space X/Y scaled by 0.1,
	// not a lathe-angle wraparound like RT_LATHE above. Like the rest of
	// this bucket, resolves the entity's real customShader.
	static int s_debugCloudsLogsRemaining = 3;
	int drawnCloudsCount = 0;
	for ( const refEntity_t &ent : s_sceneEntities )
	{
		if ( ent.reType != RT_CLOUDS )
		{
			continue;
		}
		if ( ent.renderfx & RF_THIRD_PERSON )
		{
			continue;
		}

		image_t *img = VK_GetImageByHandle( ent.customShader );
		if ( !img )
		{
			continue;
		}

		static const float diskStripDef[4] = { 0.0f, 0.4f, 0.7f, 1.0f };
		static const float diskAlphaDef[4] = { 1.0f, 1.0f, 0.4f, 0.0f };
		static const float diskCurveDef[4] = { 0.0f, 0.0f, 0.008f, 0.02f };
		static const float tubeStripDef[6] = { 0.0f, 0.05f, 0.1f, 0.5f, 0.7f, 1.0f };
		static const float tubeAlphaDef[6] = { 0.0f, 0.45f, 1.0f, 1.0f, 0.45f, 0.0f };
		static const float tubeCurveDef[6] = { 0.0f, 0.004f, 0.006f, 0.01f, 0.006f, 0.0f };

		const float *stripDef, *alphaDef, *curveDef;
		int count;
		float backlerp = ent.backlerp;
		if ( ent.renderfx & RF_GROW )
		{
			stripDef = tubeStripDef; alphaDef = tubeAlphaDef; curveDef = tubeCurveDef;
			count = 6;
			backlerp = -backlerp; // "needs to be reversed" - real comment
		}
		else
		{
			stripDef = diskStripDef; alphaDef = diskAlphaDef; curveDef = diskCurveDef;
			count = 4;
		}

		const float latheStepDeg = 30.0f;
		bool overflowed = false;

		for ( int i = 0; i < count - 1 && !overflowed; i++ )
		{
			float oldPt[3] = { ( stripDef[i] * ( ent.radius - ent.rotation ) ) + ent.rotation, 0.0f, curveDef[i] * ent.radius * backlerp };
			float oldPt2[3] = { ( stripDef[i + 1] * ( ent.radius - ent.rotation ) ) + ent.rotation, 0.0f, curveDef[i + 1] * ent.radius * backlerp };

			for ( float t = latheStepDeg; t <= 360.0f; t += latheStepDeg )
			{
				float pt[3], pt2[3];
				if ( t < 360.0f )
				{
					float s = sinf( DEG2RAD( latheStepDeg ) );
					float c = cosf( DEG2RAD( latheStepDeg ) );
					pt[0] = c * oldPt[0] - s * oldPt[1];
					pt[1] = s * oldPt[0] + c * oldPt[1];
					pt[2] = oldPt[2];
					pt2[0] = c * oldPt2[0] - s * oldPt2[1];
					pt2[1] = s * oldPt2[0] + c * oldPt2[1];
					pt2[2] = oldPt2[2];
				}
				else
				{
					// Last segment glues directly back to the def points
					// rather than continuing the incremental rotation, to
					// avoid a visible seam from accumulated float error -
					// same real reasoning as the comment in RB_SurfaceClouds.
					pt[0] = ( stripDef[i] * ( ent.radius - ent.rotation ) ) + ent.rotation; pt[1] = 0.0f; pt[2] = curveDef[i] * ent.radius * backlerp;
					pt2[0] = ( stripDef[i + 1] * ( ent.radius - ent.rotation ) ) + ent.rotation; pt2[1] = 0.0f; pt2[2] = curveDef[i + 1] * ent.radius * backlerp;
				}

				if ( cursor + 6 > POLY_VERTEX_BUFFER_CAPACITY )
				{
					overflowed = true;
					break;
				}

				float world[4][3];
				VectorAdd( ent.origin, oldPt, world[0] );
				VectorAdd( ent.origin, oldPt2, world[1] );
				VectorAdd( ent.origin, pt, world[2] );
				VectorAdd( ent.origin, pt2, world[3] );
				float vertexAlphaDef[4] = { alphaDef[i], alphaDef[i + 1], alphaDef[i], alphaDef[i + 1] };
				const int order[6] = { 0, 1, 3, 3, 2, 0 };

				uint32_t firstVertex = cursor;
				PolyVertex *out = (PolyVertex *)vk.polyVertexBufferMapped + cursor;
				for ( int v = 0; v < 6; v++ )
				{
					int c4 = order[v];
					out[v].pos[0] = world[c4][0];
					out[v].pos[1] = world[c4][1];
					out[v].pos[2] = world[c4][2];
					out[v].uv[0] = world[c4][0] * 0.1f;
					out[v].uv[1] = world[c4][1] * 0.1f;
					float rgb = ( ent.shaderRGBA[0] * vertexAlphaDef[c4] ) / 255.0f;
					out[v].color[0] = rgb;
					out[v].color[1] = rgb;
					out[v].color[2] = rgb;
					out[v].color[3] = ent.shaderRGBA[3] / 255.0f;
				}
				cursor += 6;
				VK_DrawPolyRange( cmd, img, firstVertex, 6 );

				VectorCopy( pt, oldPt );
				VectorCopy( pt2, oldPt2 );
			}
		}
		drawnCloudsCount++;
	}

	if ( s_debugCloudsLogsRemaining > 0 && drawnCloudsCount > 0 )
	{
		s_debugCloudsLogsRemaining--;
		ri.Printf( PRINT_ALL, "rd-vulkan: RT_CLOUDS entities drawn: %d\n", drawnCloudsCount );
	}
}

void VK_DrawGhoul2Entities( const float *mvp, int currentTime )
{
	if ( s_sceneEntities.empty() || !vk.frameActive )
	{
		return;
	}

	VkCommandBuffer cmd = vk.activeCommandBuffer;

	// Reset every cached model's skin-slot round-robin once per drawn scene
	// (not once per sub-model draw below) - see VulkanGhoul2Model::
	// vertexBuffer's comment for why each drawn sub-model instance needs
	// its own slot within a shared cache entry's buffer this frame.
	for ( VulkanGhoul2Model &model : s_ghoul2Models )
	{
		model.nextSkinSlot = 0;
	}

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
		for ( int subModelIdx = 0; subModelIdx < ent.ghoul2->size(); subModelIdx++ )
		{
			const CGhoul2Info &g2Instance = (*ent.ghoul2)[subModelIdx];
			int modelIndex = g2Instance.mModel;
			if ( modelIndex <= 0 || (size_t)modelIndex >= s_ghoul2Models.size() )
			{
				continue;
			}
			VulkanGhoul2Model &model = s_ghoul2Models[modelIndex];
			if ( model.surfaces.empty() )
			{
				continue;
			}

			// Live, per-instance animation frame (see VK_GetGhoul2PoseFrame's
			// comment) - &g2Instance is the same CGhoul2Info identity real
			// game code calls G2API_SetBoneAnim with, so this is genuinely
			// that instance's own current frame, not shared with any other
			// entity that happens to use the same cached model.
			int targetFrame = VK_GetGhoul2PoseFrame( &g2Instance, currentTime );
			uint32_t skinSlot = model.nextSkinSlot;
			model.nextSkinSlot = ( model.nextSkinSlot + 1 ) % GHOUL2_SKIN_SLOTS_PER_MODEL;
			std::vector<mdxaBone_t> pose;
			VK_ComputeGhoul2Pose( model.skeletonIndex, targetFrame, pose );
			VK_SkinGhoul2Model( model, pose, skinSlot );

			// Full push constant struct (mvp + camPos + fogColor), both
			// stages - matching vk.worldPipelineLayout's actual range (see
			// tr_init.cpp) exactly, not just the mvp-sized/vertex-only push
			// this used before the fog work touched that layout. Ghoul2
			// models aren't fogged (see README.md), so fogColor.a stays 0
			// (disabling the mix in world.frag) same as the sky's own push
			// in tr_world.cpp.
			vkWorldPushConstants_t entityPush = {};
			memcpy( entityPush.mvp, entityMvp, sizeof( entityMvp ) );
			vkCmdPushConstants( cmd, vk.worldPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0, sizeof( entityPush ), &entityPush );

			VkDeviceSize vertexOffset = (VkDeviceSize)skinSlot * model.skinSource.size() * sizeof( WorldVertex );
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
