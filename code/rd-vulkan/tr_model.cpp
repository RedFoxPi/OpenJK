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
#include <array>
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
	// This surface's original mdxmSurfHierarchy_t index (thisSurfaceIndex) -
	// -1 for VulkanStaticModel's surfaces (plain .md3, no Ghoul2 surface
	// hierarchy to index into). Lets VK_DrawGhoul2Entities cross-reference a
	// live CGhoul2Info::mSlist runtime on/off override (G2API_SetSurfaceOnOff,
	// tr_init.cpp) against this baked draw-call list without needing to
	// re-bake per-instance geometry - see that function's own comment.
	int surfIndex;
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
	// Surface name (lowercased) and baked-default G2SURFACEFLAG_* flags,
	// indexed by the same original mdxmSurfHierarchy_t index
	// (thisSurfaceIndex) GhoulSurfaceDraw::surfIndex carries - lets
	// G2API_SetSurfaceOnOff (tr_init.cpp) resolve a surface name to an
	// index and read its baked default flags without re-parsing the .glm.
	// Empty for a VulkanStaticModel (plain .md3, no surface hierarchy).
	std::vector<std::string> surfaceNames;
	std::vector<unsigned int> surfaceFlags;
	// Bind-pose "tag triangle" (bindPos + bone weights, no UV) for every
	// G2SURFACEFLAG_ISBOLT surface, keyed by its original surface index -
	// see VK_GetGhoul2SurfaceBoltMatrix's own comment for what this drives,
	// and VK_LoadGhoul2Model's own capture site for why it's exactly 3
	// verts, no more.
	std::unordered_map<int, std::array<GhoulSkinVertex, 3>> tagTriangles;
	// This model's own default .gla's name (mdxmHeader_t::animName, no
	// extension) - kept so VK_ResolveGhoul2SkeletonIndex can look it up in
	// the per-level animation-file-override handle space (see
	// VK_FindGhoul2AnimHandle's own comment for why that's a name lookup,
	// not a handle baked in once here at load time: this model may load
	// long before - or, in principle, after - anything ever explicitly
	// precaches its skeleton by name).
	std::string baseAnimName;
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
	// The real .gla's own precomputed inverse (mdxaSkel_t::BasePoseMatInv,
	// mdx_format.h - "inverse, to save run-time calc", baked in at model-
	// compile time, not derived here) - needed by VK_SetGhoul2BoneAngles to
	// reproduce real rd-vanilla's G2_Generate_Matrix similarity-transform
	// formula (BasePoseMat * rotationFromAngles * BasePoseMatInv) for a
	// bone-angle override, the same "sandwich by bind pose" pattern surface
	// bolts' own formula uses elsewhere in this file.
	mdxaBone_t basePoseMatInv;
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
		vb.basePoseMatInv = bone->BasePoseMatInv;
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

// Per-level animation-file overrides (G2API_SetAnimIndex/GetAnimIndex,
// CGhoul2Info::animModelIndexOffset - tr_init.cpp/game code) need a handle
// space real rd-vanilla builds via ordinary RE_RegisterModel calls, and
// `mdxmHeader_t::animIndex + offset` (real G2_API.cpp) just adds `offset`
// to a base .gla's own handle to land on whichever .gla was registered
// `offset` calls later - concretely, 0 for the base "_humanoid.gla" every
// player/NPC model defaults to, 1 for that SAME map's own
// "_humanoid_<mapname>.gla" cinematic-animation override (confirmed real,
// present for every one of this checkout's 4 fixed test maps:
// _humanoid_academy1.gla, _humanoid_hoth2.gla, _humanoid_vjun1.gla,
// _humanoid_yavin1.gla all ship in the real game data, each holding extra
// animations - not present in the base file - that a level's own scripted
// cutscene NPCs specifically need).
//
// Deliberately NOT the same handle space as VK_LoadGhoul2Skeleton's own
// per-.gla cache, even though both ultimately load through it - an earlier
// version of this code registered a .glm's own default .gla into this same
// handle sequence as a side effect of ordinary model loading (mirroring how
// real vanilla's RE_RegisterModel is genuinely one shared global handle
// space for every model of every kind), and that broke real, load-bearing
// game code: NPC_stats.cpp's G_ParseAnimFileSet asserts its *second*
// precache call (the cinematic GLA) returns exactly its first call's handle
// (the standard GLA) plus one - a real, fragile invariant real vanilla's
// own comment flags ("double check this always comes first!"). Confirmed
// by a real crash on academy1: humanoid player/NPC models (luke, kyle,
// rebel_pilot - all using "_humanoid.gla") and the wholly unrelated
// protocol droid (its own "protocol.gla") all load during the earlier UI
// menu-precache phase, before Game Initialization ever runs
// G_ParseAnimFileSet - "protocol.gla" claimed a handle in between the two
// calls this invariant depends on being adjacent, and the assert fired.
// Real vanilla avoids this collision because G_ParseAnimFileSet's own
// explicit precache calls are the *only* thing that ever touches this
// specific handle space for the "_humanoid" family - ordinary per-model
// default-skeleton resolution never does. Keeping the two spaces separate
// here (matching that real isolation, not real vanilla's literal shared-
// qhandle_t mechanism) reproduces the same observable behavior without the
// same fragility.
static std::unordered_map<std::string, int> s_animHandlesByName;
static std::vector<int> s_animHandleSkeletonIndex; // handle N -> s_skeletons index (handles are 1-based; index 0 unused)

// Explicit precache only - the "find or create" side, called exclusively
// from G2API_PrecacheGhoul2Model (tr_init.cpp). Never called as a side
// effect of loading an ordinary Ghoul2 model's own default skeleton - see
// this cache's own comment for why that distinction is load-bearing.
int VK_PrecacheGhoul2AnimHandle( const char *animNameNoExt )
{
	if ( !animNameNoExt || !animNameNoExt[0] )
	{
		return 0;
	}
	auto cached = s_animHandlesByName.find( animNameNoExt );
	if ( cached != s_animHandlesByName.end() )
	{
		return cached->second;
	}
	// Real RE_RegisterModel-for-a-.gla semantics: a file that doesn't exist
	// gets no handle at all (0, falsy) rather than reserving a slot for it -
	// this is the expected, common case for any map with no cinematic-
	// specific GLA of its own (most non-humanoid models, and any humanoid
	// map that doesn't need level-specific cutscene animations).
	int skeletonIndex = VK_LoadGhoul2Skeleton( animNameNoExt );
	if ( skeletonIndex <= 0 )
	{
		return 0;
	}
	if ( s_animHandleSkeletonIndex.empty() )
	{
		s_animHandleSkeletonIndex.push_back( 0 ); // burn handle 0, same convention as every other cache here
	}
	s_animHandleSkeletonIndex.push_back( skeletonIndex );
	int handle = (int)s_animHandleSkeletonIndex.size() - 1;
	s_animHandlesByName[animNameNoExt] = handle;
	return handle;
}

// Called from VK_ShutdownWorld (tr_world.cpp), which already runs at the
// start of every RE_LoadWorldMap - i.e. once per real level load, matching
// the real game code's own per-level lifecycle for the exact data this
// handle space exists to support. A second, real crash from the same
// underlying fragility this handle space's own comment already documents
// (see VK_PrecacheGhoul2AnimHandle's comment above, "a real crash on
// academy1"): that fix isolated this handle space from ordinary model
// loading *within* one level, but never reset it *across* levels, so it
// kept growing for the lifetime of the process. On a real multi-map
// campaign playthrough (not something the render-regression harness, which
// only ever loads one fresh map per process, can exercise), the SECOND
// humanoid-family map a player reaches hits exactly the same class of bug:
// "models/players/_humanoid/_humanoid.gla" is already cached from the
// first map (a cache hit, returning that old handle), while this new
// map's own "_humanoid_<mapname>.gla" cinematic override is a genuine
// first-time miss - allocated wherever the counter has grown to since,
// not necessarily immediately after the old cached handle - breaking
// G_ParseAnimFileSet's real `cineGLAIndex == normalGLAIndex+1` assertion
// exactly as before. Confirmed by a real user-reported crash: `Assertion
// 'cineGLAIndex == normalGLAIndex+1' failed` loading
// models/players/_humanoid_yavin1b/_humanoid_yavin1b.gla, consistent with
// yavin1 being reached as a later map in the same running session (the
// game's own level.knownAnimFileSets - the thing this handle numbering
// must stay adjacent-compatible with - already resets to empty every
// level; this renderer's own numbering needs to do the same). Does NOT
// clear VK_LoadGhoul2Skeleton's own by-filename cache (a deliberately
// separate space, see this handle space's own comment) - a .gla already
// parsed on an earlier level (e.g. the near-universal "_humanoid.gla"
// itself) is still found there and reused instantly, not re-parsed; only
// the numbering built on top of it restarts.
void VK_ResetGhoul2AnimHandles( void )
{
	s_animHandlesByName.clear();
	s_animHandleSkeletonIndex.clear();
}

// Lookup-only counterpart, for VK_ResolveGhoul2SkeletonIndex below: does a
// Ghoul2 model's own default .gla (by name, not by any handle baked in at
// .glm-load time - see that function's own comment for why a lookup, not a
// stored handle) happen to *already* be registered in this handle space?
// Never creates an entry - a model whose skeleton was never explicitly
// precached (no map-specific override exists for it, or this model simply
// isn't a "_humanoid" family model at all) correctly finds nothing here,
// same as real vanilla's own mdxmHeader_t::animIndex would never get set
// for such a model either.
static int VK_FindGhoul2AnimHandle( const char *animNameNoExt )
{
	if ( !animNameNoExt || !animNameNoExt[0] )
	{
		return 0;
	}
	auto it = s_animHandlesByName.find( animNameNoExt );
	return it != s_animHandlesByName.end() ? it->second : 0;
}

static int VK_Ghoul2AnimHandleSkeletonIndex( int handle )
{
	if ( handle <= 0 || (size_t)handle >= s_animHandleSkeletonIndex.size() )
	{
		return 0;
	}
	return s_animHandleSkeletonIndex[handle];
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
// tr_skin.cpp for the format this parser matches). Without this,
// VK_LoadGhoul2Model would resolve zero images for any humanoid model and
// skip every surface - this was confirmed against real game data
// (kyle/model.glm has 82 surfaces, all with shader=="") before this was
// added.
struct VulkanSkin
{
	std::unordered_map<std::string, std::string> surfaceShaders; // lowercased surface name -> shader path
};
// Index 0 reserved as "no skin" (empty map - every surface falls back to its
// own embedded mdxmSurfHierarchy_t::shader, unchanged behavior for models
// that don't need one, e.g. weapons).
static std::vector<VulkanSkin> s_skins;
static std::unordered_map<std::string, int> s_skinsByName;

// Parses one real .skin file's "surfacename,shaderpath" lines into skin,
// merging rather than replacing (a three-part composite skin - see
// VK_RegisterSkin - calls this once per part, into the same VulkanSkin, just
// like rd-vanilla's real RE_RegisterIndividualSkin keeps writing into the
// same tr.skins[hSkin] across all three calls). Mirrors that function's
// "_off" convention too: a line whose surface name ends in "_off" either
// means "this is a redundant off-marker, already covered by the real
// surface being skipped elsewhere" (shader literally "*off" - skip the line
// entirely) or means "the surface normally named without _off should use
// this shader" (any other shader - strip the suffix before storing). Returns
// false only on a real read failure (file not found) - a per-line parse
// issue is silently skipped, same as rd-vanilla's tokenizer would do for a
// malformed line.
static bool VK_ParseSkinFile( const char *fileName, VulkanSkin &skin )
{
	void *buffer = nullptr;
	long len = ri.FS_ReadFile( fileName, &buffer );
	if ( !buffer || len <= 0 )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: VK_RegisterSkin: %s not found\n", fileName );
		return false;
	}

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
				// whitespace CommaParse's real tokenizer would have skipped,
				// on BOTH ends of BOTH tokens - real vanilla's CommaParse
				// (tr_skin.cpp) skips leading whitespace before every token it
				// reads, not just trailing. A real, confirmed-substantial bug
				// without this: `models/players/tauntaun/model_wild.skin` (the
				// skin hoth2's wild tauntaun NPCs actually use) writes
				// "torso, models/players/tauntaun/tauntaun_body_bare_back.tga"
				// - a space *after* the comma, unlike the more common
				// no-space convention (e.g. this same model's own
				// model_default.skin) - leaving a leading space on every one
				// of its real shader names when only trailing whitespace was
				// stripped, so every surface's VK_FindImage lookup failed on
				// a name that was never a real file to begin with, and this
				// specific NPC never drew a single surface (see README.md).
				while ( !shaderName.empty() && (unsigned char)shaderName.back() <= ' ' ) shaderName.pop_back();
				while ( !shaderName.empty() && (unsigned char)shaderName.front() <= ' ' ) shaderName.erase( shaderName.begin() );
				while ( !surfName.empty() && (unsigned char)surfName.back() <= ' ' ) surfName.pop_back();
				for ( char &c : surfName ) c = (char)tolower( (unsigned char)c );

				if ( surfName.size() > 4 && surfName.compare( surfName.size() - 4, 4, "_off" ) == 0 )
				{
					if ( shaderName == "*off" )
					{
						surfName.clear(); // redundant off-marker, skip below
					}
					else
					{
						surfName.resize( surfName.size() - 4 );
					}
				}

				if ( !surfName.empty() && !shaderName.empty() && surfName.compare( 0, 4, "tag_" ) != 0 )
				{
					skin.surfaceShaders[surfName] = shaderName;
				}
			}

			if ( *p == '\0' ) break;
		}
	}
	ri.FS_FreeFile( buffer );
	return true;
}

// Splits a three-part composite skin macro ("models/players/jedi_tf/
// |head_a1|torso_a1|lower_a1") into its three real .skin file paths -
// exact port of rd-vanilla's real RE_SplitSkins (tr_skin.cpp), which this
// engine's NPC-customization system (randomized/selectable head+torso+
// lower-body skin combinations, e.g. the generic "jedi_tf"/"jedi_hm"/...
// models used for filler NPCs) relies on. Returns false (leaving the out
// params untouched) if name has no '|' or is missing one of the two
// required separators.
static bool VK_SplitCompositeSkinName( const std::string &name, std::string &head, std::string &torso, std::string &lower )
{
	size_t bar1 = name.find( '|' );
	if ( bar1 == std::string::npos )
	{
		return false;
	}
	size_t bar2 = name.find( '|', bar1 + 1 );
	if ( bar2 == std::string::npos )
	{
		return false;
	}
	size_t bar3 = name.find( '|', bar2 + 1 );
	if ( bar3 == std::string::npos )
	{
		return false;
	}
	std::string base = name.substr( 0, bar1 );
	head = base + name.substr( bar1 + 1, bar2 - bar1 - 1 ) + ".skin";
	torso = base + name.substr( bar2 + 1, bar3 - bar2 - 1 ) + ".skin";
	lower = base + name.substr( bar3 + 1 ) + ".skin";
	return true;
}

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

	VulkanSkin skin;
	std::string head, torso, lower;
	bool ok;
	if ( VK_SplitCompositeSkinName( name, head, torso, lower ) )
	{
		// Same de-duplication as RE_RegisterSkin: a part shared verbatim
		// with an already-parsed one (very common - jedi_tf's generic
		// customizations often reuse the same file for two of the three
		// slots) is only read once, but that's a pure optimization here -
		// re-parsing it again into the same map would be harmless, just
		// redundant, since later identical-content writes overwrite
		// identically. Kept anyway for a direct match against the real
		// function's structure and log output.
		ok = VK_ParseSkinFile( head.c_str(), skin );
		if ( ok && torso != head )
		{
			ok = VK_ParseSkinFile( torso.c_str(), skin );
		}
		if ( ok && lower != head && lower != torso )
		{
			ok = VK_ParseSkinFile( lower.c_str(), skin );
		}
	}
	else
	{
		ok = VK_ParseSkinFile( name, skin );
	}
	if ( !ok )
	{
		return 0;
	}

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
	// R_LoadMDXM's real `va("%s.gla", mdxm->animName)` convention). Kept
	// (not just used transiently here) as VulkanGhoul2Model::baseAnimName -
	// see VK_ResolveGhoul2SkeletonIndex's own comment for why a per-level
	// animation-file override needs to look this up by name, on demand,
	// rather than a handle baked in once at load time.
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
	std::unordered_map<int, std::array<GhoulSkinVertex, 3>> tagTriangles;

	for ( int i = 0; i < hdr->numSurfaces; i++ )
	{
		int surfIndex = surf->thisSurfaceIndex;
		bool isBolt = surfIndex >= 0 && surfIndex < hdr->numSurfaces &&
			( surfaceFlags[surfIndex] & G2SURFACEFLAG_ISBOLT );
		bool skip = ( surfIndex < 0 || surfIndex >= hdr->numSurfaces )
			|| ( surfaceFlags[surfIndex] & G2SURFACEFLAG_OFF )
			|| isBolt
			|| surf->numVerts <= 0 || surf->numTriangles <= 0;

		// A bolt-only surface (G2SURFACEFLAG_ISBOLT, name convention "*foo" -
		// confirmed real, directly parsed from real .glm data: every one of
		// kyle/model.glm's 45 asterisk-prefixed surfaces carries this exact
		// flag, always with a real 3-vert/1-triangle "tag triangle" and
		// always an empty shader name - Carcass emits these purely as
		// attachment-point geometry, never meant to be drawn as a mesh) -
		// see VK_GetGhoul2SurfaceBoltMatrix's own comment for what this data
		// is captured for. Never entered the draw path anyway even before
		// this (its empty shader name always failed VK_FindImage, and its
		// BLEND_ALPHA default - not BLEND_OPAQUE - kept it out of the
		// map-image fallback too, so this was never a visible bug) - but it
		// was still uselessly attempting that resolution every load, and
		// without this capture a surface-bolt query has no triangle to
		// compute a matrix from at all.
		if ( isBolt && surf->numVerts >= 3 )
		{
			const mdxmVertex_t *tagVerts = (const mdxmVertex_t *)( (const byte *)surf + surf->ofsVerts );
			const int *tagBoneReferences = (const int *)( (const byte *)surf + surf->ofsBoneReferences );
			std::array<GhoulSkinVertex, 3> tri = {};
			for ( int t = 0; t < 3; t++ )
			{
				GhoulSkinVertex &sv = tri[t];
				sv.bindPos[0] = tagVerts[t].vertCoords[0];
				sv.bindPos[1] = tagVerts[t].vertCoords[1];
				sv.bindPos[2] = tagVerts[t].vertCoords[2];
				sv.numWeights = G2_GetVertWeights( &tagVerts[t] );
				float totalWeight = 0.0f;
				for ( int k = 0; k < sv.numWeights && k < iMAX_G2_BONEWEIGHTS_PER_VERT; k++ )
				{
					int localBoneIndex = G2_GetVertBoneIndex( &tagVerts[t], k );
					sv.boneIndex[k] = ( localBoneIndex >= 0 && localBoneIndex < surf->numBoneReferences )
						? tagBoneReferences[localBoneIndex] : 0;
					sv.boneWeight[k] = G2_GetVertBoneWeight( &tagVerts[t], k, totalWeight, sv.numWeights );
				}
			}
			tagTriangles[surfIndex] = tri;
		}

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
			if ( !img )
			{
				// Same fallback, same reasoning, as RE_LoadWorldMap's identical
				// gap (tr_world.cpp) - confirmed here against a second, real
				// case: vjun1's jedi_tf NPC (Ghoul2, not world BSP) has its
				// torso skin-overridden to "models/players/jedi_tf/
				// torso_01_clothes", which isn't a texture file at all (no
				// matching .tga/.jpg/.png ships), only a .shader script -
				// `models/players/jedi_tf/torso_01_clothes { { map .../
				// torso_01 blendFunc GL_ONE GL_ZERO ... } }` (shaders/
				// players.shader) - that resolves opaquely to the real
				// torso_01 texture. Without this, VK_FindImage's direct
				// lookup fails and the surface is silently dropped, which is
				// exactly what made her torso (and a couple of other
				// surfaces sharing this same indirection, e.g. "..._skin")
				// invisible while her arms/hands/legs - skin-overridden to
				// shaders that happen to share their file name directly,
				// e.g. "torso_01_arms" - rendered fine. Gated on BLEND_OPAQUE
				// for the same reason as the world-geometry fallback: not
				// safe to assume for a shader that's actually translucent.
				if ( VK_GetShaderBlendMode( shaderName->c_str() ) == BLEND_OPAQUE )
				{
					const char *mapImage = VK_GetShaderMapImage( shaderName->c_str() );
					if ( mapImage )
					{
						img = VK_FindImage( mapImage );
					}
				}
			}
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
					// `WorldVertex wv = {}` above zero-inits colour to
					// black, not white - see WorldVertex::color's own
					// comment (tr_local.h) for why Ghoul2 meshes need this
					// set explicitly.
					wv.color[0] = wv.color[1] = wv.color[2] = wv.color[3] = 1.0f;
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
				drawSurfaces.push_back( { descriptorSet, firstIndex, (uint32_t)surf->numTriangles * 3, surfIndex } );
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
	model.baseAnimName = hdr->animName;
	model.skinSource = std::move( skinSource );
	model.surfaceNames = std::move( surfaceNames );
	model.surfaceFlags = std::move( surfaceFlags );
	model.tagTriangles = std::move( tagTriangles );

	// Sized for GHOUL2_SKIN_SLOTS_PER_MODEL independent slots - see
	// VulkanGhoul2Model::vertexBuffer's comment for why one slot isn't
	// enough once animation is live. Host-visible/coherent and persistently
	// mapped, not device-local like tr_world.cpp's static geometry -
	// VK_SkinGhoul2Model rewrites a slot's contents directly from the CPU
	// on every draw, so there's no benefit to device-local memory's faster
	// GPU-side read here, only the cost of a staging-buffer round trip on
	// every re-skin. Only slot 0 is initialized here (the raw bind pose,
	// cpuVerts) - every slot is always fully rewritten by VK_SkinGhoul2Model
	// before anything reads it (pos/uv/lightmapUV/color, all four - a real
	// bug once had color as the one silent exception, since it never
	// changes per-pose and so was easy to forget when writing this
	// function; see VK_SkinGhoul2Model's own comment for the real visual
	// bug that caused), so the rest starting uninitialized is fine.
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

// See this function's own declaration comment (tr_local.h).
int VK_FindGhoul2SurfaceIndex( int modelCacheIndex, const char *surfaceName, unsigned int *outBaseFlags )
{
	if ( modelCacheIndex <= 0 || (size_t)modelCacheIndex >= s_ghoul2Models.size() || !surfaceName )
	{
		return -1;
	}
	const VulkanGhoul2Model &model = s_ghoul2Models[modelCacheIndex];
	for ( size_t i = 0; i < model.surfaceNames.size(); i++ )
	{
		if ( !Q_stricmp( model.surfaceNames[i].c_str(), surfaceName ) )
		{
			if ( outBaseFlags )
			{
				*outBaseFlags = model.surfaceFlags[i];
			}
			return (int)i;
		}
	}
	return -1;
}

// Non-Ghoul2 static models (.md3) - map set pieces (misc_model_static),
// weapon world models, gibs/props. Confirmed real-world motivation: vjun1's
// opening cutscene camera sits inside a cockpit interior
// (models/map_objects/cinematics/raven_cockpit.md3) that isn't Ghoul2 and
// isn't world BSP geometry either - the nearest real BSP surfaces to that
// camera position are ~1300 units away (the level's sky shader), so before
// this, the scene rendered as a flat gray/sky background with the seated
// NPCs (real Ghoul2 entities, drawn correctly) floating in open air and no
// visible cabin at all. Reuses tr_world.cpp's WorldVertex layout/pipeline/
// descriptor-set helper wholesale, same as Ghoul2 (see this file's header
// comment) - here paired with vk.whiteImage in the lightmap slot exactly
// like Ghoul2, for the same reason (no baked lightmap of its own).
//
// Deliberately simpler than Ghoul2: single LOD (MD3's ofsSurfaces already
// points at LOD 0, the first and highest-detail one - MD3 LODs are whole
// separate model chunks the game selects between, not something this
// renderer implements) and single frame (frame 0's XyzNormals entry only -
// an MD3 with real vertex animation, e.g. an old-style weapon muzzle flash
// model, would freeze on its bind pose instead of animating, needing
// something like VK_SkinGhoul2Model's per-frame reskin to fix - not
// implemented since raven_cockpit.md3, the confirmed real case this was
// built for, is a single static set piece with exactly one frame). Static,
// device-local geometry, uploaded once - unlike Ghoul2, nothing here ever
// changes per-instance, so every entity referencing the same cached model
// just draws the one shared vertex/index buffer with its own entity model
// matrix (VK_DrawGhoul2Entities), no per-slot vertexBuffer needed.
struct VulkanStaticModel
{
	std::vector<GhoulSurfaceDraw> surfaces;
	VkBuffer vertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
	VkBuffer indexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
	// Real bind-pose bounding box, frame 0's md3Frame_t::bounds (already
	// real-world float units, not vertex-scaled shorts like xyz/normal data
	// - no MD3_XYZ_SCALE needed here) - see VK_ModelBounds's own comment
	// (this file) for the real caller this exists for.
	float mins[3] = { 0.0f, 0.0f, 0.0f };
	float maxs[3] = { 0.0f, 0.0f, 0.0f };
};
// Index 0 reserved/invalid, same convention as s_ghoul2Models.
static std::vector<VulkanStaticModel> s_staticModels;
static std::unordered_map<std::string, int> s_staticModelsByName;

int VK_LoadMD3Model( const char *fileName )
{
	auto cached = s_staticModelsByName.find( fileName );
	if ( cached != s_staticModelsByName.end() )
	{
		return cached->second;
	}

	void *buffer = nullptr;
	long len = ri.FS_ReadFile( fileName, &buffer );
	if ( !buffer || len <= 0 )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: VK_LoadMD3Model: %s not found\n", fileName );
		return 0;
	}

	const byte *base = (const byte *)buffer;
	const md3Header_t *hdr = (const md3Header_t *)buffer;
	if ( hdr->ident != MD3_IDENT || hdr->version != MD3_VERSION )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: VK_LoadMD3Model: %s is not a supported MD3 (ident/version mismatch)\n", fileName );
		ri.FS_FreeFile( buffer );
		return 0;
	}

	std::vector<WorldVertex> cpuVerts;
	std::vector<uint32_t> cpuIndexes;
	std::vector<GhoulSurfaceDraw> drawSurfaces;

	const md3Surface_t *surf = (const md3Surface_t *)( base + hdr->ofsSurfaces );
	for ( int i = 0; i < hdr->numSurfaces; i++ )
	{
		if ( surf->numVerts > 0 && surf->numTriangles > 0 && surf->numShaders > 0 )
		{
			// First shader only - the same "one texture per surface" scope this
			// renderer already applies to world/Ghoul2 geometry (no multi-stage
			// compositing). Unlike a .shader script's own name (RE_LoadWorldMap's
			// VK_GetShaderMapImage fallback, tr_world.cpp), an MD3 surface's
			// shader name is overwhelmingly already a direct texture path in
			// practice - not worth threading that fallback through here too
			// until a real model turns up that actually needs it.
			const md3Shader_t *shaders = (const md3Shader_t *)( (const byte *)surf + surf->ofsShaders );
			image_t *img = VK_FindImage( shaders[0].name );
			if ( img )
			{
				const md3St_t *st = (const md3St_t *)( (const byte *)surf + surf->ofsSt );
				// Frame 0 only - see this cache's own comment. XyzNormals is
				// laid out [numFrames][numVerts]; frame 0 is just its first
				// numVerts entries, no extra offset needed.
				const md3XyzNormal_t *xyz = (const md3XyzNormal_t *)( (const byte *)surf + surf->ofsXyzNormals );

				uint32_t vertBase = (uint32_t)cpuVerts.size();
				for ( int v = 0; v < surf->numVerts; v++ )
				{
					WorldVertex wv = {};
					wv.pos[0] = xyz[v].xyz[0] * (float)MD3_XYZ_SCALE;
					wv.pos[1] = xyz[v].xyz[1] * (float)MD3_XYZ_SCALE;
					wv.pos[2] = xyz[v].xyz[2] * (float)MD3_XYZ_SCALE;
					wv.uv[0] = st[v].st[0];
					wv.uv[1] = st[v].st[1];
					wv.lightmapUV[0] = 0.0f;
					wv.lightmapUV[1] = 0.0f;
					// See the other `WorldVertex wv = {}` site above (Ghoul2
					// meshes) for why this needs to be white, not the
					// zero-inited black.
					wv.color[0] = wv.color[1] = wv.color[2] = wv.color[3] = 1.0f;
					cpuVerts.push_back( wv );
				}

				const md3Triangle_t *tris = (const md3Triangle_t *)( (const byte *)surf + surf->ofsTriangles );
				uint32_t firstIndex = (uint32_t)cpuIndexes.size();
				for ( int t = 0; t < surf->numTriangles; t++ )
				{
					cpuIndexes.push_back( vertBase + (uint32_t)tris[t].indexes[0] );
					cpuIndexes.push_back( vertBase + (uint32_t)tris[t].indexes[1] );
					cpuIndexes.push_back( vertBase + (uint32_t)tris[t].indexes[2] );
				}

				// Same pool/white-lightmap reasoning as Ghoul2's identical call
				// (see vkGlobals_t::ghoul2DescriptorPool's comment) - a static
				// model is cached by filename and expected to survive a world
				// reload exactly like a Ghoul2 model does.
				VkDescriptorSet descriptorSet = VK_BuildWorldDescriptorSet( vk.ghoul2DescriptorPool, img, vk.whiteImage );
				// -1: plain .md3 statics have no Ghoul2 surface hierarchy to
				// index into - see GhoulSurfaceDraw::surfIndex's own comment.
				drawSurfaces.push_back( { descriptorSet, firstIndex, (uint32_t)surf->numTriangles * 3, -1 } );
			}
			// No image resolved - skip rather than draw an opaque white quad,
			// same call as tr_world.cpp's RE_LoadWorldMap and VK_LoadGhoul2Model
			// above.
		}

		surf = (const md3Surface_t *)( (const byte *)surf + surf->ofsEnd );
	}

	// Real R_ModelBounds formula (rd-vanilla's tr_model.cpp): frame 0's
	// md3Frame_t::bounds, the first entry at hdr->ofsFrames - read here,
	// before the buffer is freed below, and stashed on VulkanStaticModel
	// for VK_ModelBounds (this file's own comment there) to hand back later.
	float mdlMins[3], mdlMaxs[3];
	{
		const md3Frame_t *frame0 = (const md3Frame_t *)( base + hdr->ofsFrames );
		VectorCopy( frame0->bounds[0], mdlMins );
		VectorCopy( frame0->bounds[1], mdlMaxs );
	}

	ri.FS_FreeFile( buffer );

	if ( cpuVerts.empty() || cpuIndexes.empty() )
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: VK_LoadMD3Model: %s has no drawable surfaces\n", fileName );
		return 0;
	}

	VulkanStaticModel model;
	model.surfaces = std::move( drawSurfaces );
	VectorCopy( mdlMins, model.mins );
	VectorCopy( mdlMaxs, model.maxs );
	VK_UploadDeviceLocalBuffer( cpuVerts.data(), cpuVerts.size() * sizeof( WorldVertex ),
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &model.vertexBuffer, &model.vertexBufferMemory );
	VK_UploadDeviceLocalBuffer( cpuIndexes.data(), cpuIndexes.size() * sizeof( uint32_t ),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &model.indexBuffer, &model.indexBufferMemory );

	if ( s_staticModels.empty() )
	{
		s_staticModels.emplace_back(); // burn index 0, same convention as s_ghoul2Models
	}
	s_staticModels.push_back( std::move( model ) );
	int index = (int)s_staticModels.size() - 1;
	s_staticModelsByName[fileName] = index;

	ri.Printf( PRINT_ALL, "rd-vulkan: loaded static model %s: %d surfaces, %d verts, %d indexes\n",
		fileName, (int)s_staticModels[index].surfaces.size(), (int)cpuVerts.size(), (int)cpuIndexes.size() );

	return index;
}

// Real R_ModelBounds (rd-vanilla's tr_model.cpp) dispatches on the model's
// actual type - a real .md3 handle returns frame 0's baked bounds, a BSP
// inline submodel (*N, not registered by this renderer at all - see
// RE_RegisterModel's own comment, tr_init.cpp) returns its brush bounds,
// and anything else (including every real Ghoul2 handle) falls through to
// zero. Only the .md3 case is real here, matching VK_STATIC_MODEL_HANDLE_
// BASE-offset handles from RE_RegisterModel - every other handle (Ghoul2,
// the generic "1" fallback) correctly gets zero too, exactly like real
// vanilla's own model->md3[0]==nullptr branch does for a genuine Ghoul2
// model_t. Confirmed real, exercised caller: CG_CreateMiscEnts
// (cg_main.cpp) computes every misc_model_static entity's cull radius from
// this - e.g. vjun1's raven_cockpit.md3 (see VulkanStaticModel's own
// comment for that model's story) - previously always zero regardless of
// the model's real size.
bool VK_ModelBounds( qhandle_t handle, float mins[3], float maxs[3] )
{
	if ( handle < VK_STATIC_MODEL_HANDLE_BASE )
	{
		return false;
	}
	int index = handle - VK_STATIC_MODEL_HANDLE_BASE;
	if ( index <= 0 || (size_t)index >= s_staticModels.size() )
	{
		return false;
	}
	VectorCopy( s_staticModels[index].mins, mins );
	VectorCopy( s_staticModels[index].maxs, maxs );
	return true;
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

const char *VK_GetGhoul2GLAName( int modelCacheIndex )
{
	if ( modelCacheIndex <= 0 || (size_t)modelCacheIndex >= s_ghoul2Models.size() )
	{
		return nullptr;
	}
	int skeletonIndex = s_ghoul2Models[modelCacheIndex].skeletonIndex;
	if ( skeletonIndex <= 0 || (size_t)skeletonIndex >= s_skeletons.size() )
	{
		return nullptr;
	}
	const mdxaHeader_t *hdr = (const mdxaHeader_t *)s_skeletons[skeletonIndex].fileData.data();
	return hdr->name;
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

// Real per-instance animation-file-override resolution (G2API_SetAnimIndex,
// CGhoul2Info::animModelIndexOffset) - see VK_PrecacheGhoul2AnimHandle's own
// comment for the real mechanism this ports. Every place that computes a
// live animated pose (not the fixed bind pose - VK_GetGhoul2BoneBasePoseMat
// above deliberately doesn't call this, since a per-level override .gla
// shares its base file's bone hierarchy/rest pose, only adding extra
// animation frames) needs to use the *overridden* skeleton, not always
// model.skeletonIndex, or an NPC using an offset .gla's exclusive animation
// (e.g. a scripted cutscene gesture only academy1's own
// _humanoid_academy1.gla contains) would read frame data from the wrong
// file entirely. Looks up the model's own base .gla by *name*
// (VK_FindGhoul2AnimHandle), not a handle cached at .glm-load time - this
// model may well have loaded before anything ever explicitly precached its
// skeleton's name (see that function's own comment for the real crash this
// avoids). Falls back to the model's own default skeleton whenever no
// override is active (the overwhelmingly common case: animModelIndexOffset
// stays 0 unless a caller explicitly set it), this model's own skeleton was
// never explicitly precached at all (most non-"_humanoid" models), or the
// offset doesn't resolve to a loaded skeleton (points past whatever was
// actually registered).
static int VK_ResolveGhoul2SkeletonIndex( const VulkanGhoul2Model &model, const CGhoul2Info *ghlInfo )
{
	if ( ghlInfo && ghlInfo->animModelIndexOffset != 0 )
	{
		int baseHandle = VK_FindGhoul2AnimHandle( model.baseAnimName.c_str() );
		if ( baseHandle > 0 )
		{
			int altSkeletonIndex = VK_Ghoul2AnimHandleSkeletonIndex( baseHandle + ghlInfo->animModelIndexOffset );
			if ( altSkeletonIndex > 0 )
			{
				return altSkeletonIndex;
			}
		}
	}
	return model.skeletonIndex;
}

// Same contract as VK_GetGhoul2BoneBasePoseMat, but the bone's *currently
// animated* world-space (relative to model root) matrix instead of its
// fixed rest pose - reuses VK_ComputeGhoul2Pose, the same per-frame
// hierarchy walk VK_DrawGhoul2Entities already runs for skinning, so a
// bolted bone (e.g. a cutscene camera's tracking tag) reports the NPC's
// actual current pose rather than a static bind-pose orientation. See
// G2API_GetBoltMatrix's comment (tr_init.cpp) for why this is needed:
// without it, academy1's per-shot cutscene camera - bolted to this NPC and
// re-queried every frame by cg_camera.cpp's CGCam_FollowUpdate - was
// visibly pointed in the wrong direction despite the NPC's own skeleton
// pose matching rd-vanilla frame-for-frame (confirmed via the G2ANIM debug
// tool, README.md's character-animation investigation).
bool VK_GetGhoul2BoneCurrentPoseMat( int modelCacheIndex, const CGhoul2Info *ghlInfo, int boneIndex, int currentTime, mdxaBone_t *out )
{
	if ( modelCacheIndex <= 0 || (size_t)modelCacheIndex >= s_ghoul2Models.size() || !ghlInfo || !out )
	{
		return false;
	}
	int skeletonIndex = VK_ResolveGhoul2SkeletonIndex( s_ghoul2Models[modelCacheIndex], ghlInfo );
	if ( skeletonIndex <= 0 || (size_t)skeletonIndex >= s_skeletons.size() )
	{
		return false;
	}
	std::vector<mdxaBone_t> pose;
	VK_ComputeGhoul2Pose( skeletonIndex, ghlInfo, currentTime, pose );
	if ( boneIndex < 0 || (size_t)boneIndex >= pose.size() )
	{
		return false;
	}
	*out = pose[boneIndex];
	return true;
}

// A surface bolt's current world-space (relative to model root) matrix -
// the surface-name counterpart to VK_GetGhoul2BoneCurrentPoseMat above,
// for a bolt whose CGhoul2Info::boltInfo_t has a real surfaceNumber
// instead of a boneNumber (see G2API_AddBolt's own comment, tr_init.cpp,
// for why a name can resolve to either). Ported from rd-vanilla's real
// G2_ProcessSurfaceBolt2's "normal model tag" branch (tr_ghoul2.cpp) -
// only that branch, not its sibling for AddSurface's barycentric
// procedurally-generated tags (G2SURFACEFLAG_GENERATED), which this
// renderer doesn't implement AddSurface for at all (see README.md).
//
// The formula: skin the surface's own "tag triangle" (its first 3 raw
// verts - VK_LoadGhoul2Model's tagTriangles capture, the ONLY thing this
// needs from a G2SURFACEFLAG_ISBOLT surface, since it has no real mesh to
// draw) through the current pose using the exact same per-vertex weighted-
// bone-transform arithmetic VK_SkinGhoul2Model already uses for ordinary
// mesh vertices, then build an orthonormal basis from that triangle's
// sides - real rd-vanilla's own comment calls the two side vectors it
// uses "longest"/"shortest", but `iG2_TRISIDE_LONGEST`/`_SHORTEST`
// (mdx_format.h) are fixed constants (0 and 2), not a runtime edge-length
// comparison, so this is a fixed, mechanical formula, not a heuristic.
bool VK_GetGhoul2SurfaceBoltMatrix( int modelCacheIndex, int surfIndex, const CGhoul2Info *ghlInfo, int currentTime, mdxaBone_t *out )
{
	if ( modelCacheIndex <= 0 || (size_t)modelCacheIndex >= s_ghoul2Models.size() || !ghlInfo || !out )
	{
		return false;
	}
	const VulkanGhoul2Model &model = s_ghoul2Models[modelCacheIndex];
	auto it = model.tagTriangles.find( surfIndex );
	if ( it == model.tagTriangles.end() )
	{
		return false;
	}
	int skeletonIndex = VK_ResolveGhoul2SkeletonIndex( model, ghlInfo );
	if ( skeletonIndex <= 0 || (size_t)skeletonIndex >= s_skeletons.size() )
	{
		return false;
	}
	std::vector<mdxaBone_t> pose;
	VK_ComputeGhoul2Pose( skeletonIndex, ghlInfo, currentTime, pose );

	float pTri[3][3];
	for ( int t = 0; t < 3; t++ )
	{
		const GhoulSkinVertex &sv = it->second[t];
		pTri[t][0] = pTri[t][1] = pTri[t][2] = 0.0f;
		for ( int k = 0; k < sv.numWeights; k++ )
		{
			int boneIndex = sv.boneIndex[k];
			if ( boneIndex < 0 || (size_t)boneIndex >= pose.size() )
			{
				continue;
			}
			const mdxaBone_t &m = pose[boneIndex];
			float w = sv.boneWeight[k];
			pTri[t][0] += w * ( m.matrix[0][0] * sv.bindPos[0] + m.matrix[0][1] * sv.bindPos[1] + m.matrix[0][2] * sv.bindPos[2] + m.matrix[0][3] );
			pTri[t][1] += w * ( m.matrix[1][0] * sv.bindPos[0] + m.matrix[1][1] * sv.bindPos[1] + m.matrix[1][2] * sv.bindPos[2] + m.matrix[1][3] );
			pTri[t][2] += w * ( m.matrix[2][0] * sv.bindPos[0] + m.matrix[2][1] * sv.bindPos[1] + m.matrix[2][2] * sv.bindPos[2] + m.matrix[2][3] );
		}
	}

	float sides[3][3];
	for ( int j = 0; j < 3; j++ )
	{
		int n = ( j + 1 ) % 3;
		sides[j][0] = pTri[n][0] - pTri[j][0];
		sides[j][1] = pTri[n][1] - pTri[j][1];
		sides[j][2] = pTri[n][2] - pTri[j][2];
	}

	auto normalize = []( float v[3] )
	{
		float len = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
		if ( len > 0.0f )
		{
			v[0] /= len; v[1] /= len; v[2] /= len;
		}
	};

	float axis0[3] = { sides[0][0], sides[0][1], sides[0][2] };
	float axis1[3] = { sides[2][0], sides[2][1], sides[2][2] };
	normalize( axis0 );
	normalize( axis1 );

	// Project axis0 to be exactly perpendicular to axis1.
	float d = axis0[0] * axis1[0] + axis0[1] * axis1[1] + axis0[2] * axis1[2];
	axis0[0] -= d * axis1[0]; axis0[1] -= d * axis1[1]; axis0[2] -= d * axis1[2];
	normalize( axis0 );

	float axis2[3] = {
		sides[0][1] * sides[2][2] - sides[0][2] * sides[2][1],
		sides[0][2] * sides[2][0] - sides[0][0] * sides[2][2],
		sides[0][0] * sides[2][1] - sides[0][1] * sides[2][0],
	};
	normalize( axis2 );

	// MDX_TAG_ORIGIN (tr_ghoul2.cpp) = 2, the third tag vertex.
	out->matrix[0][3] = pTri[2][0];
	out->matrix[1][3] = pTri[2][1];
	out->matrix[2][3] = pTri[2][2];

	out->matrix[0][0] = axis1[0]; out->matrix[0][1] = axis0[0]; out->matrix[0][2] = -axis2[0];
	out->matrix[1][0] = axis1[1]; out->matrix[1][1] = axis0[1]; out->matrix[1][2] = -axis2[1];
	out->matrix[2][0] = axis1[2]; out->matrix[2][1] = axis0[2]; out->matrix[2][2] = -axis2[2];
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

// Everything VK_ComputeGhoul2BoneRecursive needs to know to compose one
// bone's *local* delta matrix for this draw - the output of running
// rd-vanilla's real G2_TimingModel (see VK_Ghoul2TimingModel below) plus,
// when a blend is in progress, the frozen old-animation pose to cross-fade
// from. Two-frame sub-frame interpolation (currentFrame/newFrame/backlerp)
// and animation-to-animation blending (blendFrame/blendLerpFrame/
// blendWeight) are both real, verified-against-rd-vanilla features now, not
// scope cuts - see VK_ResolveGhoul2BonePose's comment for how this is
// filled in, and VK_ComputeGhoul2BoneRecursive for exactly how these fields
// combine (mirrors G2_TransformBone's tbone[0..5] arithmetic, tr_ghoul2.cpp).
struct VulkanGhoul2BonePose
{
	int currentFrame = 0;
	int newFrame = 0;
	float backlerp = 0.0f; // weight for newFrame; (1-backlerp) for currentFrame
	bool hasBlend = false;
	int blendFrame = 0;      // floor of the old animation's captured position
	int blendLerpFrame = 0;  // the old animation's captured "next" frame
	float blendFrameLerp = 0.0f; // fractional part of the captured position
	float blendWeight = 0.0f;    // 0 = fully old pose, 1 = fully new pose
	// Bone-angle override (G2API_SetBoneAngles/Index, BONE_ANGLES_POSTMULT -
	// see VK_SetGhoul2BoneAngles's own comment) - applied in
	// VK_ComputeGhoul2BoneRecursive as a post-multiply on top of the normal
	// animated matrix above, exactly like real rd-vanilla's own POSTMULT
	// step. Looked up by this exact bone index, never inherited from an
	// ancestor - see VK_ResolveGhoul2BonePose's own comment for why this is
	// a separate lookup from the animation-track walk above it.
	bool hasAngleOverride = false;
	mdxaBone_t angleOverrideMatrix;
};

// Defined further below, alongside the rest of the live per-bone animation
// state it reads (VulkanGhoul2AnimState/s_ghoul2AnimState) - forward
// declared here since VK_ComputeGhoul2Pose needs it and this function
// (bone-matrix composition) predates that state in file order.
static VulkanGhoul2BonePose VK_ResolveGhoul2BonePose( const VulkanSkeleton &skel, const CGhoul2Info *ghlInfo, int boneIndex, int currentTime );

// Uncompresses one bone's pool entry for one frame - just
// VK_GetGhoul2BonePoolIndex + MC_UnCompressQuat, factored out since the
// blend/lerp math below needs up to four of these per bone instead of one.
static void VK_UncompressGhoul2Bone( const mdxaHeader_t *header, int numFrames, int frame, int bone, mdxaBone_t &out )
{
	if ( frame < 0 || frame >= numFrames )
	{
		frame = 0;
	}
	const mdxaCompQuatBone_t *pool = (const mdxaCompQuatBone_t *)( (const byte *)header + header->ofsCompBonePool );
	int poolIndex = VK_GetGhoul2BonePoolIndex( header, frame, bone );
	MC_UnCompressQuat( out.matrix, pool[poolIndex].Comp );
}

// out = (1-bWeight)*a + bWeight*b, component-wise over the whole 3x4
// matrix - exactly rd-vanilla's real per-component "backlerp*X +
// frontlerp*Y" loops (G2_TransformBone), not a quaternion slerp; the real
// engine does the same simplification, so this isn't a corner cut relative
// to it.
static void VK_LerpGhoul2BoneMatrix( mdxaBone_t &out, const mdxaBone_t &a, const mdxaBone_t &b, float bWeight )
{
	float aWeight = 1.0f - bWeight;
	const float *fa = &a.matrix[0][0];
	const float *fb = &b.matrix[0][0];
	float *fo = &out.matrix[0][0];
	for ( int i = 0; i < 12; i++ )
	{
		fo[i] = aWeight * fa[i] + bWeight * fb[i];
	}
}

// rd-vanilla's real fixed root-bone seed matrix (RootMatrix(), tr_ghoul2.cpp)
// - see VK_ComputeGhoul2BoneRecursive's own comment on its root-bone branch
// for the full story of what this is and why it's needed unconditionally.
// File-scope (not local to that branch) so VK_ComputeGhoul2Pose can also
// use it as the default `attachBase` for an unattached (or root, index 0)
// sub-model - see that function's own comment.
static const mdxaBone_t s_g2RootRotation = { { { 0.0f, -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } } };

static void VK_ComputeGhoul2BoneRecursive( const VulkanSkeleton &skel, const mdxaHeader_t *header, const std::vector<VulkanGhoul2BonePose> &bonePose,
	int boneIndex, std::vector<mdxaBone_t> &outBones, std::vector<bool> &computed, const mdxaBone_t &rootBase )
{
	if ( computed[boneIndex] )
	{
		return;
	}

	const VulkanGhoul2BonePose &pose = bonePose[boneIndex];
	int numFrames = skel.numFrames;

	// New animation's pose right now - a straight uncompress if this frame
	// falls exactly on a whole frame (backlerp == 0, e.g. a paused anim or
	// one that just started), otherwise interpolated between currentFrame
	// and newFrame by backlerp - mirrors G2_TransformBone's `if
	// (!TB.backlerp)` fast path versus its general two-frame lerp branch.
	mdxaBone_t newPose;
	if ( pose.backlerp == 0.0f )
	{
		VK_UncompressGhoul2Bone( header, numFrames, pose.currentFrame, boneIndex, newPose );
	}
	else
	{
		mdxaBone_t a, b;
		VK_UncompressGhoul2Bone( header, numFrames, pose.newFrame, boneIndex, a );
		VK_UncompressGhoul2Bone( header, numFrames, pose.currentFrame, boneIndex, b );
		VK_LerpGhoul2BoneMatrix( newPose, b, a, pose.backlerp );
	}

	mdxaBone_t delta;
	if ( pose.hasBlend )
	{
		// The previous animation's pose, frozen at the exact continuous
		// frame position it was at when this new animation started -
		// captured once (VK_SetGhoul2BoneAnim) and never re-evaluated
		// over time, matching rd-vanilla's real G2_Set_Bone_Anim_Index/
		// G2_TransformBone: interpolating blendFrame/blendLerpFrame by
		// blendFrameLerp here reproduces that snapshot every draw, not a
		// live second animation.
		mdxaBone_t oldA, oldB, oldPose;
		VK_UncompressGhoul2Bone( header, numFrames, pose.blendFrame, boneIndex, oldA );
		VK_UncompressGhoul2Bone( header, numFrames, pose.blendLerpFrame, boneIndex, oldB );
		VK_LerpGhoul2BoneMatrix( oldPose, oldA, oldB, pose.blendFrameLerp );
		VK_LerpGhoul2BoneMatrix( delta, oldPose, newPose, pose.blendWeight );
	}
	else
	{
		delta = newPose;
	}

	int parent = skel.bones[boneIndex].parent;
	if ( parent < 0 )
	{
		// rd-vanilla seeds every root bone's hierarchy walk with a fixed
		// matrix it calls `identityMatrix` (tr_ghoul2.cpp) - a name that's
		// misleading enough to cost a whole investigation (see README.md's
		// character-animation section): it is NOT the mathematical identity,
		// it's a fixed 90-degree rotation, `{{0,-1,0,0},{1,0,0,0},{0,0,1,0}}`
		// (RootMatrix() returns exactly this for the normal, non-
		// GHOUL2_NEWORIGIN case - confirmed by directly printing its real
		// runtime value). It's the fixed remap from the .gla/.glm's own
		// modeling convention into the engine's world convention - every
		// Ghoul2 model needs it, unconditionally, same as e.g.
		// VK_BuildViewMatrix's own fixed camera-convention "flip" constant
		// elsewhere in this file, not something ent.axis or any per-instance
		// state already accounts for. Using a true identity here instead
		// (as this renderer did until now) left every Ghoul2 model rotated
		// 90 degrees short of rd-vanilla's orientation at the root - normally
		// masked by the rest of the mesh still reading as "a person," but
		// glaringly visible on any shot where the camera's own facing isn't
		// also off by the same 90 degrees to compensate. `rootBase` is this
		// same constant for an ordinary (unattached) model - see
		// VK_ComputeGhoul2Pose's own comment for the one real case it's
		// something else instead (a bolt matrix, for a model-to-model
		// attached sub-model - "Ghoul2 model-to-model attachment").
		VK_Multiply3x4Matrix( &outBones[boneIndex], &rootBase, &delta );
	}
	else
	{
		VK_ComputeGhoul2BoneRecursive( skel, header, bonePose, parent, outBones, computed, rootBase );
		VK_Multiply3x4Matrix( &outBones[boneIndex], &outBones[parent], &delta );
	}

	// Bone-angle override (G2API_SetBoneAngles/Index, BONE_ANGLES_POSTMULT -
	// see VK_SetGhoul2BoneAngles's own comment) - applied AFTER the normal
	// composition above regardless of root/non-root branch, exactly matching
	// real rd-vanilla's own structure (tr_ghoul2.cpp's per-bone transform:
	// the POSTMULT check runs unconditionally after the if/else-if(child)
	// block, not inside either branch).
	if ( pose.hasAngleOverride )
	{
		mdxaBone_t tempMatrix = outBones[boneIndex];
		VK_Multiply3x4Matrix( &outBones[boneIndex], &tempMatrix, &pose.angleOverrideMatrix );
	}
	computed[boneIndex] = true;
}

void VK_ComputeGhoul2Pose( int skeletonIndex, const CGhoul2Info *ghlInfo, int currentTime, std::vector<mdxaBone_t> &outBones, const mdxaBone_t *attachBase )
{
	const mdxaBone_t &rootBase = attachBase ? *attachBase : s_g2RootRotation;
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

	const mdxaHeader_t *header = (const mdxaHeader_t *)skel.fileData.data();
	int numBones = (int)skel.bones.size();
	outBones.resize( numBones );
	std::vector<bool> computed( numBones, false );

	// Each bone picks up whichever track actually controls it (itself or
	// the nearest ancestor with one set - see VK_ResolveGhoul2BonePose's
	// comment) *before* the hierarchy composition pass below, so a child
	// bone's parent is composed using the parent's own (possibly
	// different) resolved pose, not the child's - exactly the real
	// engine's per-region behavior, not a single flattened frame applied
	// uniformly to every bone.
	std::vector<VulkanGhoul2BonePose> bonePose( numBones );
	for ( int i = 0; i < numBones; i++ )
	{
		bonePose[i] = VK_ResolveGhoul2BonePose( skel, ghlInfo, i, currentTime );
	}
	for ( int i = 0; i < numBones; i++ )
	{
		VK_ComputeGhoul2BoneRecursive( skel, header, bonePose, i, outBones, computed, rootBase );
	}
}

// Live animation state per Ghoul2 model instance *and bone*, driven by
// G2API_SetBoneAnim (tr_init.cpp's thin wrappers call into
// VK_SetGhoul2BoneAnim below). This used to be one shared whole-skeleton
// track per CGhoul2Info instance regardless of which bone a SetBoneAnim
// call targeted - a real, confirmed-by-direct-comparison-against-vanilla
// bug, not just a visual simplification: the real game calls SetBoneAnim
// separately per body region (bg_panimate.cpp calls it for `bodyBone`
// (legs, generally near the skeleton root) and `torsBone` (upper body,
// e.g. "lower_lumbar") independently, and *queries* GetBoneAnim
// separately per region too, e.g. `bodyAnimating`/`torsAnimating`
// checking each region's own completion) - collapsing them into one
// shared slot meant the second SetBoneAnim call silently overwrote the
// first, so a later GetBoneAnim query for the *first* region's bone
// returned the *second* region's timing entirely. Since game/ICARUS logic
// (e.g. "wait until this animation finishes") branches on exactly that
// query, a wrong answer doesn't just look cruder, it desyncs downstream
// script timing - confirmed as the actual cause of camera cuts landing at
// completely different points between this renderer and vanilla at the
// same simulated time (see README.md's "Character animation: per-bone
// state" section for the side-by-side comparison that led here).
//
// Still a real, deliberate simplification versus the true engine, kept
// for the same reasons as before: no cross-fade/blend *between* two
// animations sharing overlapping bones, and no sub-frame interpolation
// between two adjacent whole frames (both real, visible-quality features
// of rd-vanilla's G2_TimingModel this doesn't reproduce - see README.md).
// What changed is that the *bone-region* separation itself - which track
// controls which part of the skeleton - is now real, not simulated by a
// single flattened track, via a hierarchy walk in VK_ComputeGhoul2Pose:
// each bone uses the nearest track set on itself or an ancestor, matching
// the real engine's own bone-tree-based resolution (mentioned, but not
// implemented, in this renderer's earlier "Live animation" checkpoint).
struct VulkanGhoul2AnimState
{
	int startFrame = 0;
	int endFrame = 0;
	int startTime = 0;
	int pauseTime = 0; // 0 = not paused
	float animSpeed = 0.0f;
	int flags = 0;
	// Blend-from state (BONE_ANIM_BLEND, game/ghoul2_shared.h 0x0080) -
	// captured once, in VK_SetGhoul2BoneAnim, at the exact moment a new
	// animation is set on top of an already-animating bone with a nonzero
	// blendTime. blendFrame/blendLerpFrame are NOT re-evaluated as time
	// passes - they're a frozen snapshot of where the *previous* animation
	// was, matching rd-vanilla's real G2_Set_Bone_Anim_Index/
	// G2_TransformBone exactly (see VK_SetGhoul2BoneAnim's comment for the
	// capture arithmetic, and VK_ComputeGhoul2BoneRecursive for how the
	// snapshot is cross-faded against the live new animation).
	// blendDurationMs == 0 means "no active blend for this SetBoneAnim
	// call" - blendStartTime is meaningless in that case.
	float blendFrame = 0.0f;
	int blendLerpFrame = 0;
	int blendStartTime = 0;
	int blendDurationMs = 0;
};
// Outer key: CGhoul2Info identity - the exact pointer game code calls
// G2API_SetBoneAnim with (e.g. &gent->ghoul2[gent->playerModel] in
// bg_panimate.cpp). refEntity_t::ghoul2 (tr_types.h) is a pointer to the
// game-owned CGhoul2Info_v, not a per-frame copy, so &(*ent.ghoul2)[slot]
// in VK_DrawGhoul2Entities below is the same address on every frame for
// the same in-game entity/sub-model. Inner key: the real skeleton bone
// index this track was set on (see VK_ResolveGhoul2AnimBone's comment,
// tr_init.cpp, for how By-name G2API calls resolve to this same space) -
// -1 is an ordinary map key here, not a special "whole skeleton" bucket,
// used only when a caller's bone name/index genuinely couldn't be
// resolved.
static std::unordered_map<const CGhoul2Info *, std::unordered_map<int, VulkanGhoul2AnimState>> s_ghoul2AnimState;

// Bone-angle overrides (G2API_SetBoneAngles/SetBoneAnglesIndex,
// BONE_ANGLES_POSTMULT only - see VK_SetGhoul2BoneAngles's own comment for
// why this is the one flag combination real game code actually uses).
// Deliberately a separate map from s_ghoul2AnimState above, not folded into
// VulkanGhoul2AnimState, even though real rd-vanilla's boneInfo_t
// (game/ghoul2_shared.h) stores both an animation track AND an angle-
// override matrix in the very same per-bone-index struct - this renderer's
// own VulkanGhoul2AnimState already exists purely for the animation side
// and adding unrelated override-matrix fields to it would blur what each
// one is for; a bone can have an animation-state entry, an angle-override
// entry, both, or neither, completely independently, matching real
// behavior either way. Same (CGhoul2Info*, boneIndex) key space as
// s_ghoul2AnimState.
struct VulkanGhoul2AngleOverride
{
	// Precomputed once, at G2API_SetBoneAngles(Index) call time - not
	// recomputed per frame - exactly matching real rd-vanilla's own
	// G2_Generate_Matrix, which likewise bakes this once into
	// boneInfo_t::matrix rather than re-deriving it from the raw angles on
	// every draw. See VK_SetGhoul2BoneAngles's own comment for the formula.
	mdxaBone_t matrix;
};
static std::unordered_map<const CGhoul2Info *, std::unordered_map<int, VulkanGhoul2AngleOverride>> s_ghoul2AngleOverride;

// Real flag bit values (game/ghoul2_shared.h) - not included from here (a
// game-side header this renderer doesn't otherwise depend on).
// OVERRIDE_FREEZE already includes the OVERRIDE bit (0x48 = 0x40 + 0x08);
// bg_panimate.cpp only ever sets LOOP or FREEZE (never plain OVERRIDE
// alone or a negative/reverse animSpeed - its own comment says as much),
// so those are the only two "ran off the end" behaviors this needs to
// resolve, though VK_Ghoul2TimingModel still ports the reverse-playback
// branches faithfully rather than assuming they're unreachable.
static const int VK_BONE_ANIM_OVERRIDE_LOOP = 0x0010;
static const int VK_BONE_ANIM_OVERRIDE_FREEZE = 0x0048;
static const int VK_BONE_ANIM_BLEND = 0x0080;

// Exact port of rd-vanilla's real G2_TimingModel (tr_ghoul2.cpp): given a
// bone's animation state and the current time, decides which two whole
// frames to interpolate between (currentFrame/newFrame) and by how much
// (backlerp - the weight on newFrame; 1-backlerp on currentFrame, matching
// VK_ComputeGhoul2BoneRecursive's convention). Ported branch-for-branch,
// including the reverse-playback (animSpeed<0) and zero-length
// (startFrame==endFrame) cases real callers don't currently exercise, and
// the exact same "currentFrame+backlerp is always the continuous,
// wrapped/clamped frame position" invariant real code relies on elsewhere
// (G2_Get_Bone_Anim_Index) - VK_Ghoul2CurrentFramePosition below depends on
// that invariant to capture a blend-from snapshot. Frame indices are
// clamped into [0, numFrames) at the end rather than asserting, matching
// this codebase's practice of defensive clamps over release-mode crashes -
// real inputs never actually violate the real function's bounds asserts,
// so this is a safety net, not a behavior change.
static void VK_Ghoul2TimingModel( const VulkanGhoul2AnimState &state, int currentTime, int numFrames, int &currentFrame, int &newFrame, float &backlerp )
{
	float animSpeed = state.animSpeed;
	float time = (float)( ( state.pauseTime ? state.pauseTime : currentTime ) - state.startTime ) / 50.0f;
	if ( time < 0.0f )
	{
		time = 0.0f;
	}
	float newFrame_g = (float)state.startFrame + ( time * animSpeed );

	int animSize = state.endFrame - state.startFrame;
	float endFrame = (float)state.endFrame;
	currentFrame = state.startFrame;
	newFrame = state.startFrame;
	backlerp = 0.0f;

	if ( animSize )
	{
		bool ranOff = ( animSpeed > 0.0f && newFrame_g > endFrame - 1.0f ) ||
			( animSpeed < 0.0f && newFrame_g < endFrame + 1.0f );
		if ( ranOff )
		{
			if ( state.flags & VK_BONE_ANIM_OVERRIDE_LOOP )
			{
				if ( animSpeed < 0.0f )
				{
					if ( newFrame_g < endFrame + 1.0f && newFrame_g >= endFrame )
					{
						backlerp = ( endFrame + 1.0f ) - newFrame_g;
						currentFrame = (int)endFrame;
						newFrame = state.startFrame;
					}
					else
					{
						if ( newFrame_g <= endFrame + 1.0f )
						{
							newFrame_g = endFrame + fmodf( newFrame_g - endFrame, (float)animSize ) - (float)animSize;
						}
						backlerp = ceilf( newFrame_g ) - newFrame_g;
						currentFrame = (int)ceilf( newFrame_g );
						newFrame = ( currentFrame <= endFrame + 1.0f ) ? state.startFrame : currentFrame - 1;
					}
				}
				else
				{
					if ( newFrame_g > endFrame - 1.0f && newFrame_g < endFrame )
					{
						backlerp = newFrame_g - (int)newFrame_g;
						currentFrame = (int)newFrame_g;
						newFrame = state.startFrame;
					}
					else
					{
						if ( newFrame_g >= endFrame )
						{
							newFrame_g = endFrame + fmodf( newFrame_g - endFrame, (float)animSize ) - (float)animSize;
						}
						backlerp = newFrame_g - (int)newFrame_g;
						currentFrame = (int)newFrame_g;
						newFrame = ( newFrame_g >= endFrame - 1.0f ) ? state.startFrame : currentFrame + 1;
					}
				}
			}
			else if ( ( state.flags & VK_BONE_ANIM_OVERRIDE_FREEZE ) == VK_BONE_ANIM_OVERRIDE_FREEZE )
			{
				currentFrame = ( animSpeed > 0.0f ) ? state.endFrame - 1 : state.endFrame + 1;
				newFrame = currentFrame;
				backlerp = 0.0f;
			}
			// else: a plain BONE_ANIM_OVERRIDE with neither LOOP nor FREEZE
			// - real code clears the anim's flags entirely here ("turn it
			// off"). Not reproduced (no real caller hits this - see this
			// function's own comment) - falls through keeping the
			// startFrame/no-lerp defaults set above.
		}
		else
		{
			if ( animSpeed > 0.0f )
			{
				currentFrame = (int)newFrame_g;
				backlerp = newFrame_g - currentFrame;
				newFrame = currentFrame + 1;
				if ( newFrame >= (int)endFrame )
				{
					newFrame = ( state.flags & VK_BONE_ANIM_OVERRIDE_LOOP ) ? state.startFrame : state.endFrame - 1;
				}
			}
			else
			{
				backlerp = ceilf( newFrame_g ) - newFrame_g;
				currentFrame = (int)ceilf( newFrame_g );
				if ( currentFrame > state.startFrame )
				{
					currentFrame = state.startFrame;
					newFrame = currentFrame;
					backlerp = 0.0f;
				}
				else
				{
					newFrame = currentFrame - 1;
					if ( (float)newFrame < endFrame + 1.0f )
					{
						newFrame = ( state.flags & VK_BONE_ANIM_OVERRIDE_LOOP ) ? state.startFrame : state.endFrame + 1;
					}
				}
			}
		}
	}
	else
	{
		currentFrame = ( animSpeed < 0.0f ) ? state.endFrame + 1 : state.endFrame - 1;
		if ( currentFrame < 0 )
		{
			currentFrame = 0;
		}
		newFrame = currentFrame;
		backlerp = 0.0f;
	}

	if ( numFrames > 0 )
	{
		if ( currentFrame < 0 || currentFrame >= numFrames ) currentFrame = 0;
		if ( newFrame < 0 || newFrame >= numFrames ) newFrame = 0;
	}
}

// The continuous (fractional) frame position VK_Ghoul2TimingModel's
// currentFrame/backlerp describe - real callers (G2_Get_Bone_Anim_Index)
// report this as GetBoneAnim's currentFrame output, and also use it
// internally to snapshot a blend-from position in G2_Set_Bone_Anim_Index -
// see VK_SetGhoul2BoneAnim's comment.
static float VK_Ghoul2CurrentFramePosition( const VulkanGhoul2AnimState &state, int currentTime, int numFrames )
{
	int currentFrame, newFrame;
	float backlerp;
	VK_Ghoul2TimingModel( state, currentTime, numFrames, currentFrame, newFrame, backlerp );
	return (float)currentFrame + backlerp;
}

// numFrames for whatever skeleton ghlInfo->mModel currently resolves to -
// needed by VK_SetGhoul2BoneAnim purely to keep VK_Ghoul2TimingModel's
// frame-index clamp meaningful when capturing a blend snapshot; 0 (an
// otherwise-invalid frame count) is a safe "don't clamp" sentinel for a not
// -yet-resolvable model, matching VK_Ghoul2TimingModel's own `numFrames > 0`
// guard.
static int VK_GetGhoul2NumFrames( const CGhoul2Info *ghlInfo )
{
	if ( !ghlInfo || ghlInfo->mModel <= 0 || (size_t)ghlInfo->mModel >= s_ghoul2Models.size() )
	{
		return 0;
	}
	int skeletonIndex = VK_ResolveGhoul2SkeletonIndex( s_ghoul2Models[ghlInfo->mModel], ghlInfo );
	if ( skeletonIndex <= 0 || (size_t)skeletonIndex >= s_skeletons.size() )
	{
		return 0;
	}
	return s_skeletons[skeletonIndex].numFrames;
}

// setFrame/blendTime are real parameters now (previously discarded by
// tr_init.cpp's G2API_SetBoneAnim/SetBoneAnimIndex - see those functions'
// comments), not new API surface: G2API_SetBoneAnimIndex has always taken
// them, this renderer just ignored them.
//
// setFrame (-1 = "start fresh at startFrame") lets a caller keep an
// animation's current position across a SetBoneAnim call that doesn't
// actually change anything but the flags/blendTime - bg_panimate.cpp does
// this on every PM_SetAnimFinal call that re-affirms an anim already
// playing, passing its own already-queried currentFrame back in so the
// anim doesn't visibly restart from frame 0. Ported via the same algebra
// as rd-vanilla's real G2_Set_Bone_Anim_Index: solve startTime such that
// VK_Ghoul2TimingModel would report exactly setFrame right now.
//
// blendTime (only meaningful with BONE_ANIM_BLEND set in flags) captures
// the *previous* state's continuous frame position - via
// VK_Ghoul2CurrentFramePosition, i.e. running the timing model on the
// about-to-be-overwritten state - as a frozen (blendFrame, blendLerpFrame)
// snapshot pair, exactly like real G2_Set_Bone_Anim_Index: blendLerpFrame
// is the snapshot's "next" frame (or the same frame twice for reverse
// playback), wrapped/clamped against the *old* state's own endFrame/loop
// flag. If there was no previous state on this bone (nothing to blend
// from), blending is silently dropped - matches real code's "hmm, we
// weren't animating on this bone" branch.
void VK_SetGhoul2BoneAnim( const CGhoul2Info *ghlInfo, int boneIndex, int startFrame, int endFrame, int flags, float animSpeed, int startTime, float setFrame, int blendTime )
{
	if ( !ghlInfo )
	{
		return;
	}
	auto &boneMap = s_ghoul2AnimState[ghlInfo];
	auto existing = boneMap.find( boneIndex );
	int numFrames = VK_GetGhoul2NumFrames( ghlInfo );

	float blendFrame = 0.0f;
	int blendLerpFrame = 0;
	int blendDurationMs = 0;
	int blendStartTime = startTime;
	if ( ( flags & VK_BONE_ANIM_BLEND ) && blendTime > 0 )
	{
		if ( existing != boneMap.end() )
		{
			const VulkanGhoul2AnimState &old = existing->second;
			if ( old.blendDurationMs > 0 && old.blendStartTime == startTime )
			{
				// Replacing a blend that was itself set up this same
				// instant (hasn't actually started yet) - just extend its
				// duration, keep the snapshot it already captured.
				blendFrame = old.blendFrame;
				blendLerpFrame = old.blendLerpFrame;
				blendStartTime = old.blendStartTime;
				blendDurationMs = blendTime;
			}
			else
			{
				float currentFrame = VK_Ghoul2CurrentFramePosition( old, startTime, numFrames );
				if ( old.animSpeed < 0.0f )
				{
					blendFrame = floorf( currentFrame );
					blendLerpFrame = (int)floorf( currentFrame );
				}
				else
				{
					blendFrame = currentFrame;
					blendLerpFrame = (int)currentFrame + 1;
					if ( blendFrame >= (float)old.endFrame )
					{
						blendFrame = ( old.flags & VK_BONE_ANIM_OVERRIDE_LOOP ) ? (float)old.startFrame : (float)old.endFrame - 1.0f;
					}
					if ( blendLerpFrame >= old.endFrame )
					{
						blendLerpFrame = ( old.flags & VK_BONE_ANIM_OVERRIDE_LOOP ) ? old.startFrame : old.endFrame - 1;
					}
				}
				blendDurationMs = blendTime;
				blendStartTime = startTime;
			}
		}
		// else: nothing to blend from - blendDurationMs stays 0, dropping
		// the blend for this call, same as real code.
	}

	VulkanGhoul2AnimState &state = boneMap[boneIndex];
	state.startFrame = startFrame;
	state.endFrame = endFrame;
	state.animSpeed = animSpeed;
	state.pauseTime = 0;
	state.flags = flags;
	state.blendFrame = blendFrame;
	state.blendLerpFrame = blendLerpFrame;
	state.blendStartTime = blendStartTime;
	state.blendDurationMs = blendDurationMs;

	if ( setFrame != -1.0f && animSpeed != 0.0f )
	{
		state.startTime = (int)( (float)startTime - ( ( ( setFrame - (float)startFrame ) * 50.0f ) / animSpeed ) );
	}
	else
	{
		state.startTime = startTime;
	}
}

bool VK_GetGhoul2BoneAnim( const CGhoul2Info *ghlInfo, int boneIndex, int currentTime, float *currentFrame, int *startFrame, int *endFrame, int *flags, float *animSpeed )
{
	auto instIt = s_ghoul2AnimState.find( ghlInfo );
	if ( instIt == s_ghoul2AnimState.end() )
	{
		return false;
	}
	auto boneIt = instIt->second.find( boneIndex );
	if ( boneIt == instIt->second.end() )
	{
		return false;
	}
	const VulkanGhoul2AnimState &state = boneIt->second;
	if ( startFrame ) *startFrame = state.startFrame;
	if ( endFrame ) *endFrame = state.endFrame;
	if ( flags ) *flags = state.flags;
	if ( animSpeed ) *animSpeed = state.animSpeed;
	if ( currentFrame ) *currentFrame = VK_Ghoul2CurrentFramePosition( state, currentTime, VK_GetGhoul2NumFrames( ghlInfo ) );
	return true;
}

bool VK_PauseGhoul2BoneAnim( const CGhoul2Info *ghlInfo, int boneIndex, int currentTime )
{
	auto instIt = s_ghoul2AnimState.find( ghlInfo );
	if ( instIt == s_ghoul2AnimState.end() )
	{
		return false;
	}
	auto boneIt = instIt->second.find( boneIndex );
	if ( boneIt == instIt->second.end() )
	{
		return false;
	}
	boneIt->second.pauseTime = currentTime;
	return true;
}

bool VK_IsGhoul2BoneAnimPaused( const CGhoul2Info *ghlInfo, int boneIndex )
{
	auto instIt = s_ghoul2AnimState.find( ghlInfo );
	if ( instIt == s_ghoul2AnimState.end() )
	{
		return false;
	}
	auto boneIt = instIt->second.find( boneIndex );
	return boneIt != instIt->second.end() && boneIt->second.pauseTime != 0;
}

bool VK_StopGhoul2BoneAnim( const CGhoul2Info *ghlInfo, int boneIndex )
{
	auto instIt = s_ghoul2AnimState.find( ghlInfo );
	if ( instIt == s_ghoul2AnimState.end() )
	{
		return false;
	}
	return instIt->second.erase( boneIndex ) > 0;
}

// Real bit value (game/ghoul2_shared.h) - not included from here (a
// game-side header this renderer doesn't otherwise depend on). The only
// BONE_ANGLES_* flag any real call site in this game's own code ever
// passes - PREMULT and REPLACE are both real rd-vanilla features (see
// G2_Generate_Matrix/tr_ghoul2.cpp) with zero exercised callers found
// anywhere in code/game or code/cgame (a grep across every
// G2API_SetBoneAngles(Index) call site turned up POSTMULT and nothing
// else), so - same evidence-scoped approach as this renderer's other
// features - only POSTMULT is implemented; a flags value without it is a
// silent no-op rather than a guess at unexercised behavior.
static const int VK_BONE_ANGLES_POSTMULT = 0x0002;

// Real G2API_SetBoneAngles(Index) (tr_init.cpp's thin wrappers call into
// this) - ports rd-vanilla's real G2_Generate_Matrix (G2_bones.cpp)
// POSTMULT branch exactly, not rederived: build a rotation matrix from
// `angles` (a PITCH/YAW/ROLL Euler triple) after remapping its three
// components onto the bone-local up/left/forward axes the caller asked
// for (real callers pick different remaps depending on which real skeleton
// axis a given bone's "forward" happens to point down - e.g. a foot bone's
// local axes aren't aligned the same way a head bone's are), then convert
// that rotation into the bone's own local coordinate frame via a
// similarity transform sandwiching it between the bone's BasePoseMat and
// BasePoseMatInv (the exact "conjugate by bind pose" pattern
// VK_GetGhoul2SurfaceBoltMatrix's own formula also uses, for the same
// underlying reason: a rotation expressed in world/parent space needs this
// conversion to apply correctly regardless of the bone's own orientation
// within the skeleton). Precomputed once here - not re-derived every frame
// - exactly like real rd-vanilla bakes it into boneInfo_t::matrix once at
// G2_Generate_Matrix call time; VK_ComputeGhoul2BoneRecursive just reads
// the stored result.
bool VK_SetGhoul2BoneAngles( const CGhoul2Info *ghlInfo, int boneIndex, const vec3_t angles, int flags, Eorientations up, Eorientations left, Eorientations forward )
{
	if ( !ghlInfo || boneIndex < 0 )
	{
		return false;
	}
	if ( !( flags & VK_BONE_ANGLES_POSTMULT ) )
	{
		return false;
	}
	if ( ghlInfo->mModel <= 0 || (size_t)ghlInfo->mModel >= s_ghoul2Models.size() )
	{
		return false;
	}
	int skeletonIndex = s_ghoul2Models[ghlInfo->mModel].skeletonIndex;
	if ( skeletonIndex <= 0 || (size_t)skeletonIndex >= s_skeletons.size() )
	{
		return false;
	}
	const std::vector<VulkanBone> &bones = s_skeletons[skeletonIndex].bones;
	if ( (size_t)boneIndex >= bones.size() )
	{
		return false;
	}
	const VulkanBone &bone = bones[boneIndex];

	// Real angles[] convention is [PITCH, YAW, ROLL] (indices 0/1/2) - same
	// as every other Euler-angle use in this codebase. `default: break;`
	// (ORIGIN, the enum's zero value) leaves that component at its
	// zero-initialized default - real G2_Generate_Matrix leaves the
	// corresponding local uninitialized instead, but no real caller ever
	// passes ORIGIN for up/left/forward (confirmed by the same grep as
	// VK_BONE_ANGLES_POSTMULT's own comment), so this never diverges from
	// real behavior in practice - it's a defined-behavior safety net, not a
	// deliberate reinterpretation.
	vec3_t newAngles = { 0.0f, 0.0f, 0.0f };
	switch ( up )
	{
		case NEGATIVE_X: newAngles[1] = angles[2] + 180.0f; break;
		case POSITIVE_X: newAngles[1] = angles[2]; break;
		case NEGATIVE_Y: newAngles[1] = angles[0]; break;
		case POSITIVE_Y: newAngles[1] = angles[0]; break;
		case NEGATIVE_Z: newAngles[1] = angles[1] + 180.0f; break;
		case POSITIVE_Z: newAngles[1] = angles[1]; break;
		default: break;
	}
	switch ( left )
	{
		case NEGATIVE_X: newAngles[0] = angles[2]; break;
		case POSITIVE_X: newAngles[0] = angles[2] + 180.0f; break;
		case NEGATIVE_Y: newAngles[0] = angles[0]; break;
		case POSITIVE_Y: newAngles[0] = angles[0] + 180.0f; break;
		case NEGATIVE_Z: newAngles[0] = angles[1]; break;
		case POSITIVE_Z: newAngles[0] = angles[1]; break;
		default: break;
	}
	switch ( forward )
	{
		case NEGATIVE_X: newAngles[2] = angles[2]; break;
		case POSITIVE_X: newAngles[2] = angles[2]; break;
		case NEGATIVE_Y: newAngles[2] = angles[0]; break;
		case POSITIVE_Y: newAngles[2] = angles[0] + 180.0f; break;
		case NEGATIVE_Z: newAngles[2] = angles[1]; break;
		case POSITIVE_Z: newAngles[2] = angles[1] + 180.0f; break;
		default: break;
	}

	// Create_Matrix (G2_misc.cpp): AnglesToAxis into a translation-less
	// mdxaBone_t, column c = axis[c] - same construction
	// G2API_GetBoltMatrix's own world-matrix build already uses
	// (tr_init.cpp), just with a zero translation instead of a position.
	matrix3_t axis;
	AnglesToAxis( newAngles, axis );
	mdxaBone_t rot = {};
	for ( int row = 0; row < 3; row++ )
	{
		rot.matrix[row][0] = axis[0][row];
		rot.matrix[row][1] = axis[1][row];
		rot.matrix[row][2] = axis[2][row];
		rot.matrix[row][3] = 0.0f;
	}

	mdxaBone_t temp1;
	VK_Multiply3x4Matrix( &temp1, &rot, &bone.basePoseMatInv );
	VulkanGhoul2AngleOverride &override_ = s_ghoul2AngleOverride[ghlInfo][boneIndex];
	VK_Multiply3x4Matrix( &override_.matrix, &bone.basePoseMat, &temp1 );
	return true;
}

// The full pose (sub-frame-interpolated, and mid-blend if applicable) to
// skin one specific bone with right now, resolved by walking from
// boneIndex up its parent chain (starting at itself) until a bone is found
// with its own explicit track set on this instance - the real engine's own
// bone-tree resolution rule (a track set on e.g. "lower_lumbar" only
// controls that bone and its descendants; everything else keeps whatever
// *its* nearest ancestor track says, typically a whole-body track set on a
// bone near the skeleton root). Falls back to a static frame-0 pose (this
// renderer's original "never animated" default) if no bone from boneIndex
// up to the root has any track at all - see README.md's frame-0 caveat.
static VulkanGhoul2BonePose VK_ResolveGhoul2BonePose( const VulkanSkeleton &skel, const CGhoul2Info *ghlInfo, int boneIndex, int currentTime )
{
	VulkanGhoul2BonePose pose;
	auto instIt = s_ghoul2AnimState.find( ghlInfo );
	if ( instIt != s_ghoul2AnimState.end() )
	{
		int walk = boneIndex;
		while ( walk >= 0 )
		{
			auto boneIt = instIt->second.find( walk );
			if ( boneIt != instIt->second.end() )
			{
				const VulkanGhoul2AnimState &state = boneIt->second;
				VK_Ghoul2TimingModel( state, currentTime, skel.numFrames, pose.currentFrame, pose.newFrame, pose.backlerp );

				// A blend is only actually applied for the window of time it
				// covers - once elapsed time reaches blendDurationMs the
				// snapshot is retired and the bone plays the new animation
				// outright, matching real G2_TransformBone's
				// `blendTime>=0.0f && blendTime<boneList[...].blendTime` gate.
				if ( state.blendDurationMs > 0 )
				{
					int elapsed = currentTime - state.blendStartTime;
					if ( elapsed >= 0 && elapsed < state.blendDurationMs )
					{
						pose.hasBlend = true;
						pose.blendFrame = (int)state.blendFrame;
						pose.blendLerpFrame = state.blendLerpFrame;
						pose.blendFrameLerp = state.blendFrame - (float)pose.blendFrame;
						pose.blendWeight = (float)elapsed / (float)state.blendDurationMs;
					}
				}
				break;
			}
			walk = skel.bones[walk].parent;
		}
	}

	// Bone-angle override lookup - a separate concern from the animation-
	// track walk above (see VulkanGhoul2BonePose::hasAngleOverride's own
	// comment for why this is keyed by boneIndex directly, not inherited).
	auto overrideInstIt = s_ghoul2AngleOverride.find( ghlInfo );
	if ( overrideInstIt != s_ghoul2AngleOverride.end() )
	{
		auto overrideBoneIt = overrideInstIt->second.find( boneIndex );
		if ( overrideBoneIt != overrideInstIt->second.end() )
		{
			pose.hasAngleOverride = true;
			pose.angleOverrideMatrix = overrideBoneIt->second.matrix;
		}
	}
	return pose;
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
		// Real, confirmed bug: this function did not write `color` at all,
		// even though the comment at this model's vertexBuffer allocation
		// site (VK_LoadGhoul2Model) claims "every slot is always fully
		// rewritten by VK_SkinGhoul2Model before anything reads it" - that
		// was true for every field except this one. Slot 0 happens to look
		// correct only because VK_LoadGhoul2Model's own initial `memcpy`
		// seeds it from cpuVerts (color already 1,1,1,1 there - see
		// WorldVertex::color's own comment) - every OTHER slot's colour
		// channel is whatever the freshly Vulkan-allocated host-visible
		// memory happened to contain (commonly zero-initialized, at least
		// under Mesa's lavapipe), silently multiplying that instance's
		// entire diffuse texture to solid black. Confirmed the real,
		// concrete cause of 2 of hoth2's 3 simultaneously-visible
		// WildTauntaun NPCs (all three sharing one cached model+skin, so
		// only the one drawn from skin-slot 0 this frame looked right)
		// rendering as flat black silhouettes instead of their real
		// texture - see README.md.
		out[v].color[0] = 1.0f;
		out[v].color[1] = 1.0f;
		out[v].color[2] = 1.0f;
		out[v].color[3] = 1.0f;
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
	// Static (.md3) models - see VulkanStaticModel's comment for why these
	// share vk.ghoul2DescriptorPool and this same shutdown/lifetime with
	// Ghoul2 models rather than getting their own.
	for ( VulkanStaticModel &model : s_staticModels )
	{
		if ( model.vertexBuffer ) vkDestroyBuffer( vk.device, model.vertexBuffer, nullptr );
		if ( model.vertexBufferMemory ) vkFreeMemory( vk.device, model.vertexBufferMemory, nullptr );
		if ( model.indexBuffer ) vkDestroyBuffer( vk.device, model.indexBuffer, nullptr );
		if ( model.indexBufferMemory ) vkFreeMemory( vk.device, model.indexBufferMemory, nullptr );
	}
	s_staticModels.clear();
	s_staticModelsByName.clear();
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

// Permanent debug tool (see r_ghoul2AnimDebug's comment, tr_local.h):
// prints one line per animated bone-track on every drawn Ghoul2 entity,
// rate-limited per (instance, bone) to once every 200ms of *simulated*
// time (currentTime) rather than real elapsed time, so two separate runs
// (or, more to the point, this renderer and rd-vanilla running the exact
// same scripted scene) produce directly comparable log timing rather than
// one flooded with every single frame of a looping animation. Output
// format is deliberately identical to rd-vanilla's own matching
// "r_ghoul2animdebug" print (tr_ghoul2.cpp) - same field order and units,
// bone *name* rather than a bare index (indices happen to already agree
// between the two renderers for a shared skeleton, but names remove any
// doubt and stay readable) - specifically so the two renderers' logs can
// be grepped for the same entity (matched by world-space origin, the one
// identifier both sides can agree on - see this project's
// character-animation investigation, README.md, for why origin and not a
// pointer/handle) and `diff`d directly against each other, rather than
// re-deriving one-off temporary instrumentation every time a new
// animation discrepancy needs chasing.
static void VK_DebugLogGhoul2Anim( const refEntity_t &ent, const CGhoul2Info *ghlInfo, const VulkanSkeleton *skel, int currentTime )
{
	if ( !r_ghoul2AnimDebug->integer || !ghlInfo )
	{
		return;
	}
	auto instIt = s_ghoul2AnimState.find( ghlInfo );
	if ( instIt == s_ghoul2AnimState.end() )
	{
		return;
	}
	static std::unordered_map<const CGhoul2Info *, std::unordered_map<int, int>> s_lastPrintTime;
	auto &perBone = s_lastPrintTime[ghlInfo];
	int numFrames = VK_GetGhoul2NumFrames( ghlInfo );
	for ( auto &kv : instIt->second )
	{
		int boneIndex = kv.first;
		const VulkanGhoul2AnimState &state = kv.second;
		auto lastIt = perBone.find( boneIndex );
		if ( lastIt != perBone.end() && currentTime - lastIt->second < 200 )
		{
			continue;
		}
		perBone[boneIndex] = currentTime;
		const char *boneName = "?";
		if ( skel && boneIndex >= 0 && (size_t)boneIndex < skel->bones.size() )
		{
			boneName = skel->bones[boneIndex].name.c_str();
		}
		float pos = VK_Ghoul2CurrentFramePosition( state, currentTime, numFrames );
		ri.Printf( PRINT_ALL, "G2ANIM t=%d pos=(%.0f,%.0f,%.0f) bone=%s start=%d end=%d cur=%.2f speed=%.2f flags=0x%x\n",
			currentTime, ent.origin[0], ent.origin[1], ent.origin[2], boneName,
			state.startFrame, state.endFrame, pos, state.animSpeed, state.flags );
	}
}

// Last currentTime this renderer actually drew a Ghoul2 scene at - the only
// per-frame animation clock this renderer has, needed by
// VK_GetGhoul2BoneCurrentPoseMat (G2API_GetBoltMatrix's real caller,
// cg_camera.cpp's CGCam_FollowUpdate, has no currentTime of its own to pass
// in; the real engine's equivalent just re-evaluates the instance's own live
// animation state, which amounts to the same "whatever time we're currently
// rendering at" value). One render-call of lag behind the very next
// RE_RenderScene is an unavoidable, harmless consequence of camera-bolt
// queries happening src-side before this frame's scene is actually drawn -
// see G2API_GetBoltMatrix's comment (tr_init.cpp) for why this replaced a
// bind-pose-only bolt matrix that made cutscene cameras track a
// rest-pose orientation instead of the NPC's actual current pose.
int g_vkGhoul2LastRenderTime = 0;

void VK_DrawGhoul2Entities( const float *mvp, int currentTime )
{
	g_vkGhoul2LastRenderTime = currentTime;
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
	int drawnEntityCount = 0, drawnSubModelCount = 0, drawnStaticModelCount = 0;
	int rtModelCount = 0;

	for ( const refEntity_t &ent : s_sceneEntities )
	{
		if ( ent.reType != RT_MODEL )
		{
			continue;
		}
		if ( !ent.ghoul2 || ent.ghoul2->size() <= 0 )
		{
			// Static (.md3) model entity - see VulkanStaticModel's comment
			// (this file) for what this is and the confirmed real-world case
			// it fixes (vjun1's cockpit interior). Reuses the exact same
			// entity-matrix/push-constant/world-pipeline draw shape the
			// Ghoul2 branch below uses, just against one shared vertex/index
			// buffer instead of a per-instance skin slot - see
			// VK_LoadMD3Model's comment for why no skinning is needed here.
			int modelIndex = ent.hModel - VK_STATIC_MODEL_HANDLE_BASE;
			if ( modelIndex <= 0 || (size_t)modelIndex >= s_staticModels.size() )
			{
				continue;
			}
			VulkanStaticModel &model = s_staticModels[modelIndex];
			if ( model.surfaces.empty() )
			{
				continue;
			}

			float staticModel_[16] = {};
			staticModel_[0] = ent.axis[0][0]; staticModel_[4] = ent.axis[1][0]; staticModel_[8] = ent.axis[2][0]; staticModel_[12] = ent.origin[0];
			staticModel_[1] = ent.axis[0][1]; staticModel_[5] = ent.axis[1][1]; staticModel_[9] = ent.axis[2][1]; staticModel_[13] = ent.origin[1];
			staticModel_[2] = ent.axis[0][2]; staticModel_[6] = ent.axis[1][2]; staticModel_[10] = ent.axis[2][2]; staticModel_[14] = ent.origin[2];
			staticModel_[15] = 1.0f;
			float staticEntityMvp[16];
			VK_MultiplyMatrix( staticModel_, mvp, staticEntityMvp );

			// Same push-constant shape/reasoning as the Ghoul2 branch below
			// (no baked lightmap, so camPos.w=1.0 not world geometry's 2.0;
			// not fogged, so fogColor.a stays 0).
			vkWorldPushConstants_t staticPush = {};
			memcpy( staticPush.mvp, staticEntityMvp, sizeof( staticEntityMvp ) );
			staticPush.camPos[3] = 1.0f;
			// Identity, not zero-init's 0,0 - see vkWorldPushConstants_t's
			// own comment (tr_local.h). tcMod scale isn't wired up for
			// static (.md3) models, same scope as tcMod scroll.
			staticPush.uvScale[0] = 1.0f;
			staticPush.uvScale[1] = 1.0f;
			vkCmdPushConstants( cmd, vk.worldPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0, sizeof( staticPush ), &staticPush );

			VkDeviceSize vertexOffset = 0;
			vkCmdBindVertexBuffers( cmd, 0, 1, &model.vertexBuffer, &vertexOffset );
			vkCmdBindIndexBuffer( cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT32 );
			for ( const GhoulSurfaceDraw &surface : model.surfaces )
			{
				vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.worldPipelineLayout,
					0, 1, &surface.descriptorSet, 0, nullptr );
				vkCmdDrawIndexed( cmd, surface.indexCount, 1, surface.firstIndex, 0, 0 );
			}
			drawnStaticModelCount++;
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
			// Real rd-vanilla's own per-entity skin override (tr_ghoul2.cpp's
			// R_AddGhoul2Surfaces: `if (ent->e.customSkin) skin =
			// R_GetSkinByHandle(ent->e.customSkin); else if (ghoul2[i].mSkin >
			// 0) ...`) takes priority over whatever skin this sub-model was
			// loaded with via G2API_SetSkin, and is the ONLY skin mechanism a
			// great many real NPCs ever use at all - G2API_SetSkin (this
			// renderer's only skin path until now) is real vanilla's own
			// *player*-model-specific convenience (g_client.cpp's
			// G_SetG2PlayerModel always calls it right after
			// G2API_InitGhoul2Model), while a NPC's single-piece creature/
			// droid/prop model (no separate head/torso/legs to individually
			// re-skin) is set up purely by the CLIENT stamping this plain,
			// already-networked refEntity_t::customSkin field directly - no
			// G2API_SetSkin call ever happens for it. Without this, any such
			// model - loaded once at G2API_InitGhoul2Model time with skin 0,
			// same as "no skin at all" - resolves every one of its (usually
			// entirely skin-file-dependent, embedded-shader-name-empty)
			// surfaces to nothing and silently never draws: confirmed the
			// real, concrete cause of hoth2's tauntaun/wampa NPCs being
			// completely invisible (every one of tauntaun's 12 real surfaces
			// has an empty embedded shader name per its own .glm data, relying
			// entirely on `models/players/tauntaun/model_default.skin` for
			// every texture - a skin this renderer's own G2API_InitGhoul2Model
			// deliberately never applies, by design, matching real vanilla's
			// own comment there). Re-resolves through the same
			// VK_LoadGhoul2Model cache (keyed by fileName+skinHandle) used
			// everywhere else, so this costs a cache lookup, not a re-parse,
			// once the correctly-skinned variant has been loaded the first
			// time.
			int modelIndex = ent.customSkin
				? VK_LoadGhoul2Model( g2Instance.mFileName, (int)ent.customSkin )
				: g2Instance.mModel;
			if ( modelIndex <= 0 || (size_t)modelIndex >= s_ghoul2Models.size() )
			{
				continue;
			}
			VulkanGhoul2Model &model = s_ghoul2Models[modelIndex];
			if ( model.surfaces.empty() )
			{
				continue;
			}

			// Live, per-instance, per-bone-region animation pose (see
			// VK_ComputeGhoul2Pose's comment) - &g2Instance is the same
			// CGhoul2Info identity real game code calls G2API_SetBoneAnim
			// with, so every bone resolves to genuinely that instance's own
			// current track for whichever body region controls it, not
			// shared with any other entity using the same cached model, and
			// not flattened onto one whole-skeleton frame either.
			uint32_t skinSlot = model.nextSkinSlot;
			model.nextSkinSlot = ( model.nextSkinSlot + 1 ) % GHOUL2_SKIN_SLOTS_PER_MODEL;

			// Model-to-model attachment (G2API_AttachG2Model, tr_init.cpp):
			// mModelBoltLink, when set, replaces this sub-model's normal
			// fixed root-rotation seed matrix (s_g2RootRotation) with the
			// bolted-to sibling sub-model's own *current* bolt matrix -
			// same dispatch (bone-bolt vs. surface-bolt) and same
			// kG2ModelShift/kG2BoltShift decode as G2API_GetBoltMatrix
			// (tr_init.cpp) uses for the equivalent encode. Real
			// rd-vanilla (tr_ghoul2.cpp's main render dispatch loop) never
			// does this for sub-model index 0 - the first sub-model is
			// always the root/body and always uses the fixed seed matrix,
			// even if some caller mistakenly set its mModelBoltLink.
			mdxaBone_t attachBase;
			bool hasAttachBase = false;
			if ( subModelIdx != 0 && g2Instance.mModelBoltLink != -1 )
			{
				int boltMod = ( g2Instance.mModelBoltLink >> kG2ModelShift ) & kG2ModelAnd;
				int boltNum = ( g2Instance.mModelBoltLink >> kG2BoltShift ) & kG2BoltAnd;
				if ( boltMod >= 0 && boltMod < ent.ghoul2->size() )
				{
					const CGhoul2Info &toInstance = (*ent.ghoul2)[boltMod];
					if ( boltNum >= 0 && (size_t)boltNum < toInstance.mBltlist.size() )
					{
						if ( toInstance.mBltlist[boltNum].boneNumber >= 0 )
						{
							hasAttachBase = VK_GetGhoul2BoneCurrentPoseMat( (int)toInstance.mModel, &toInstance,
								toInstance.mBltlist[boltNum].boneNumber, currentTime, &attachBase );
						}
						else if ( toInstance.mBltlist[boltNum].surfaceNumber >= 0 )
						{
							hasAttachBase = VK_GetGhoul2SurfaceBoltMatrix( (int)toInstance.mModel, toInstance.mBltlist[boltNum].surfaceNumber,
								&toInstance, currentTime, &attachBase );
						}
					}
				}
			}
			// VK_ResolveGhoul2SkeletonIndex: a per-level animation-file
			// override (G2API_SetAnimIndex - see that function's own
			// comment, tr_model.cpp) shares this model's own bind pose/
			// bone hierarchy, only swapping which .gla's *animation frame
			// data* poses are actually read from - so the override applies
			// here (live pose), not to the bind-pose/skinning-weight data
			// VK_SkinGhoul2Model reads from model.skinSource, which stays
			// tied to the base skeleton regardless.
			int poseSkeletonIndex = VK_ResolveGhoul2SkeletonIndex( model, &g2Instance );
			std::vector<mdxaBone_t> pose;
			VK_ComputeGhoul2Pose( poseSkeletonIndex, &g2Instance, currentTime, pose, hasAttachBase ? &attachBase : nullptr );
			VK_SkinGhoul2Model( model, pose, skinSlot );
			VK_DebugLogGhoul2Anim( ent, &g2Instance,
				( poseSkeletonIndex > 0 && (size_t)poseSkeletonIndex < s_skeletons.size() ) ? &s_skeletons[poseSkeletonIndex] : nullptr,
				currentTime );

			// Full push constant struct (mvp + camPos + fogColor), both
			// stages - matching vk.worldPipelineLayout's actual range (see
			// tr_init.cpp) exactly, not just the mvp-sized/vertex-only push
			// this used before the fog work touched that layout. Ghoul2
			// models aren't fogged (see README.md), so fogColor.a stays 0
			// (disabling the mix in world.frag) same as the sky's own push
			// in tr_world.cpp. camPos.w is the overbright factor (world.
			// frag's comment) - 1.0, NOT world/sky's 2.0: Ghoul2 surfaces are
			// paired with a plain white placeholder in the lightmap slot
			// (VK_BuildWorldDescriptorSet's ghoul2 call site, this file),
			// not a real baked-and-overbright-compensated lightmap, so the
			// *2.0 world geometry needs would just silently render every
			// character twice as bright as its own diffuse texture - this
			// was a real, confirmed bug (a user directly noticed Vulkan's
			// screenshots reading much brighter than rd-vanilla's), not a
			// deliberate simplification.
			vkWorldPushConstants_t entityPush = {};
			memcpy( entityPush.mvp, entityMvp, sizeof( entityMvp ) );
			entityPush.camPos[3] = 1.0f;
			// Identity, not zero-init's 0,0 - see vkWorldPushConstants_t's
			// own comment (tr_local.h). tcMod scale isn't wired up for
			// Ghoul2 models, same scope as tcMod scroll.
			entityPush.uvScale[0] = 1.0f;
			entityPush.uvScale[1] = 1.0f;
			vkCmdPushConstants( cmd, vk.worldPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				0, sizeof( entityPush ), &entityPush );

			VkDeviceSize vertexOffset = (VkDeviceSize)skinSlot * model.skinSource.size() * sizeof( WorldVertex );
			vkCmdBindVertexBuffers( cmd, 0, 1, &model.vertexBuffer, &vertexOffset );
			vkCmdBindIndexBuffer( cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT32 );

			for ( const GhoulSurfaceDraw &surface : model.surfaces )
			{
				// This instance's own runtime on/off override (if any),
				// checked against the baked-at-load-time decision this
				// draw call already represents (a surface skipped via
				// mdxmSurfHierarchy_t's own default G2SURFACEFLAG_OFF -
				// see VK_LoadGhoul2Model's skip check - never became a
				// GhoulSurfaceDraw entry at all, so isn't reachable here
				// regardless of mSlist). See G2API_SetSurfaceOnOff's own
				// comment (tr_init.cpp) for what populates mSlist and why
				// NODESCENDANTS' recursive hide isn't applied here either.
				bool overriddenOff = false;
				for ( const surfaceInfo_t &entry : g2Instance.mSlist )
				{
					if ( entry.surface == surface.surfIndex )
					{
						overriddenOff = ( entry.offFlags & G2SURFACEFLAG_OFF ) != 0;
						break;
					}
				}
				if ( overriddenOff )
				{
					continue;
				}
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
		ri.Printf( PRINT_ALL, "rd-vulkan: ghoul2: %d/%d scene entities drew %d sub-model(s), %d static model(s)\n",
			drawnEntityCount, rtModelCount, drawnSubModelCount, drawnStaticModelCount );
	}
}
