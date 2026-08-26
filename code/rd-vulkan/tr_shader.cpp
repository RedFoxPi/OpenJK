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

// Minimal .shader script support - see tr_local.h's declaration of
// VK_GetShaderBlendMode() for what this does and does not do. This is not
// a port of rd-vanilla's tr_shader.cpp (see rd-vulkan/README.md for why
// files aren't reused directly): it's a small brace-depth-tracking scan
// that only extracts one fact - the first stage's blendFunc - using the
// shared, GL-agnostic COM_ParseExt() tokenizer from qcommon/q_shared.cpp.

#include "../server/exe_headers.h"

#include "tr_local.h"
#include <unordered_map>
#include <string>

static std::unordered_map<std::string, vkBlendMode_t> s_shaderBlendModes;
static bool s_shadersLoaded = false;

struct vkShaderFogParms_t
{
	float color[3];
	float opaqueDist;
};
static std::unordered_map<std::string, vkShaderFogParms_t> s_shaderFogParms;

// First stage's `tcMod scroll <sSpeed> <tSpeed>`, keyed by shader name - see
// VK_GetShaderTcModScroll's own comment for what this drives. Every other
// tcMod type (`rotate`/`scale`/`stretch`/`turb`/`transform`/
// `entityTranslate`) and every rgbGen/alphaGen wave are still unread - see
// rd-vulkan/README.md.
struct vkShaderTcModScroll_t
{
	float sSpeed, tSpeed;
};
static std::unordered_map<std::string, vkShaderTcModScroll_t> s_shaderTcModScroll;

// First stage's `alphaGen portal <range>` numeric argument, keyed by shader
// name - the only alphaGen this renderer reads. It exists purely to drive
// VK_GetShaderPortalRange's flare-quad radius (see that function's comment
// and RB_SurfaceFlare in rd-vanilla's real tr_surface.cpp, which this
// mirrors: `radius = tess.shader->portalRange ? tess.shader->portalRange :
// 30`) - every other alphaGen type (`vertex`, `wave`, `lightingSpecular`,
// plain `portal` with no numeric argument, ...) is still unread.
static std::unordered_map<std::string, float> s_shaderPortalRange;

// First stage's `rgbGen const ( r g b )` colour, keyed by shader name - the
// only rgbGen this renderer reads (every other type - `identityLighting`,
// `vertex`, `wave`, ... - is still unread, matching whatever this renderer's
// existing default colour path already does for that surface). Exists to
// drive RE_LoadWorldMap's per-vertex colour overwrite (tr_world.cpp) for a
// small, real, verified family of additive "dust cloud" decal shaders -
// see VK_GetShaderRgbGenConst's own comment.
struct vkShaderRgbConst_t
{
	float color[3];
};
static std::unordered_map<std::string, vkShaderRgbConst_t> s_shaderRgbConst;

// Whether the first stage declares ANY `tcGen` keyword at all, keyed by
// shader name - not which kind. Exists purely as a safety gate for
// RE_LoadWorldMap's map-image fallback (tr_world.cpp): `tcGen environment`
// (reflection-vector UV generation this renderer doesn't implement) would
// render an actively wrong static texture if a shader using it were ever
// allowed through that fallback, so the fallback needs to know "does this
// shader use *some* non-default UV generation" without needing to actually
// implement any of them - see that fallback's own comment for the real
// env_glass/glass_security_* shaders this excludes.
static std::unordered_map<std::string, bool> s_shaderHasTcGen;

// First stage's `map <path>` argument, keyed by shader name - see
// VK_GetShaderMapImage's comment for why this exists: a shader's own name is
// NOT reliably the same path as its actual base texture (e.g. hoth2's
// `textures/hoth/metal_lg_lt_vertex`, whose first stage is actually `map
// textures/impgarrison/metal_lg_lt` - a vertex-lit variant shader reusing an
// existing texture under a different name, not a special case - see
// rd-vulkan/README.md). Only real file paths are recorded here - `$whiteimage`/
// `$lightmap`/`$lightmapgrid` (generated images, no file to find) are left
// unrecorded, same "nothing to resolve" outcome as a shader with no `map` at
// all in its first stage.
static std::unordered_map<std::string, std::string> s_shaderMapImage;

static vkBlendMode_t BlendFactorsToMode( const char *src, const char *dst )
{
	if ( !Q_stricmp( src, "GL_ONE" ) && !Q_stricmp( dst, "GL_ONE" ) )
	{
		return BLEND_ADDITIVE;
	}
	// rd-vanilla special-cases exactly this pair as "blending implicitly
	// disabled" (see tr_shader.cpp ParseStage) - an explicit way of saying
	// the same thing a missing blendFunc keyword says.
	if ( !Q_stricmp( src, "GL_ONE" ) && !Q_stricmp( dst, "GL_ZERO" ) )
	{
		return BLEND_OPAQUE;
	}
	return BLEND_ALPHA;
}

// Scans one already-loaded .shader file's text for "<name> { <stages...> }"
// blocks and records each name's first stage's blendFunc. Everything else
// (tcMod, rgbGen, sky/fog params, later stages...) is skipped over via
// brace-depth tracking, not understood.
static void ParseShaderFile( const char *text )
{
	const char *p = text;

	COM_BeginParseSession();

	for ( ;; )
	{
		const char *shaderName = COM_ParseExt( &p, qtrue );
		if ( !shaderName[0] )
		{
			break;
		}
		std::string name = shaderName;

		const char *tok = COM_ParseExt( &p, qtrue );
		if ( strcmp( tok, "{" ) )
		{
			// Not a shader block opener where one was expected - drop both
			// tokens and try to resync on the next one.
			continue;
		}

		int depth = 1;
		bool inFirstStage = false;
		bool sawFirstStage = false;
		// A defined shader's first stage defaults to opaque (no blendFunc
		// keyword = blending disabled, see BLEND_OPAQUE) unless a blendFunc
		// keyword says otherwise below.
		vkBlendMode_t blendMode = BLEND_OPAQUE;
		bool haveFogParms = false;
		vkShaderFogParms_t fogParms = {};
		bool haveTcModScroll = false;
		vkShaderTcModScroll_t tcModScroll = {};
		bool havePortalRange = false;
		float portalRange = 0.0f;
		bool haveRgbConst = false;
		vkShaderRgbConst_t rgbConst = {};
		bool hasTcGen = false;
		std::string mapImage;

		while ( depth > 0 )
		{
			tok = COM_ParseExt( &p, qtrue );
			if ( !tok[0] )
			{
				// Unexpected EOF mid-shader - abandon this entry.
				break;
			}

			if ( !strcmp( tok, "{" ) )
			{
				depth++;
				if ( depth == 2 && !sawFirstStage )
				{
					inFirstStage = true;
				}
				continue;
			}

			if ( !strcmp( tok, "}" ) )
			{
				depth--;
				if ( depth == 1 && inFirstStage )
				{
					inFirstStage = false;
					sawFirstStage = true;
				}
				continue;
			}

			// fogparms is a top-level shader keyword (depth 1, same level
			// as a stage's own opening brace), not something inside a
			// stage - unlike blendFunc below. Syntax: `fogparms ( r g b )
			// opaqueDistance` (a literal parenthesized triple, then a
			// plain number - see fogs.shader for real examples).
			if ( depth == 1 && !Q_stricmp( tok, "fogparms" ) )
			{
				const char *paren = COM_ParseExt( &p, qfalse );
				if ( !strcmp( paren, "(" ) )
				{
					fogParms.color[0] = (float)atof( COM_ParseExt( &p, qfalse ) );
					fogParms.color[1] = (float)atof( COM_ParseExt( &p, qfalse ) );
					fogParms.color[2] = (float)atof( COM_ParseExt( &p, qfalse ) );
					COM_ParseExt( &p, qfalse ); // closing ")"
					fogParms.opaqueDist = (float)atof( COM_ParseExt( &p, qfalse ) );
					haveFogParms = true;
				}
				continue;
			}

			// `map <path>` (or the legacy synonym `clampmap`) is the stage's
			// base image - only the first stage's, and only its first `map`/
			// `clampmap` keyword if a stage somehow has more than one
			// (real shaders don't). `$whiteimage`/`$lightmap`/`$lightmapgrid`
			// are generated placeholders, not files - see mapImage's own
			// comment (s_shaderMapImage) for why those are deliberately left
			// unrecorded rather than treated as a (nonexistent) file path.
			if ( depth == 2 && inFirstStage && mapImage.empty() &&
				( !Q_stricmp( tok, "map" ) || !Q_stricmp( tok, "clampmap" ) ) )
			{
				const char *path = COM_ParseExt( &p, qfalse );
				if ( path[0] && path[0] != '$' )
				{
					mapImage = path;
				}
				continue;
			}

			// tcMod is a per-stage keyword with a type-specific argument
			// count (`scroll <s> <t>`, `rotate <deg/sec>`, `stretch <func>
			// <base> <amp> <phase> <freq>`, ...) - only `scroll`'s own two
			// numeric arguments are understood and recorded here (see
			// VK_GetShaderTcModScroll's comment for why just this one).
			// Every other tcMod type's numeric arguments are deliberately
			// left unconsumed after their type keyword is read - they fall
			// through to the bottom of this loop as ordinary unmatched
			// tokens and are silently skipped, exactly like any other
			// unrecognized keyword already is (blendFunc/map's own
			// specific handling works the same way: anything not
			// specifically intercepted is just ignored, not an error).
			if ( depth == 2 && inFirstStage && !Q_stricmp( tok, "tcMod" ) )
			{
				std::string modType = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( modType.c_str(), "scroll" ) )
				{
					tcModScroll.sSpeed = (float)atof( COM_ParseExt( &p, qfalse ) );
					tcModScroll.tSpeed = (float)atof( COM_ParseExt( &p, qfalse ) );
					haveTcModScroll = true;
				}
				continue;
			}

			// `alphaGen portal <range>` - only the numeric range argument is
			// recorded (see s_shaderPortalRange's own comment); every other
			// alphaGen keyword/argument falls through unconsumed like any
			// other unrecognized token.
			if ( depth == 2 && inFirstStage && !Q_stricmp( tok, "alphaGen" ) )
			{
				std::string a = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( a.c_str(), "portal" ) )
				{
					const char *rangeTok = COM_ParseExt( &p, qfalse );
					if ( rangeTok[0] )
					{
						portalRange = (float)atof( rangeTok );
						havePortalRange = true;
					}
				}
				continue;
			}

			// `rgbGen const ( r g b )` - only this one rgbGen type is
			// recorded (see s_shaderRgbConst's own comment); every other
			// rgbGen keyword (`identityLighting`, `vertex`, `wave`, ...)
			// falls through unconsumed like any other unrecognized token,
			// same "not specifically intercepted" pattern as alphaGen above.
			if ( depth == 2 && inFirstStage && !Q_stricmp( tok, "rgbGen" ) )
			{
				std::string a = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( a.c_str(), "const" ) )
				{
					const char *paren = COM_ParseExt( &p, qfalse );
					if ( !strcmp( paren, "(" ) )
					{
						rgbConst.color[0] = (float)atof( COM_ParseExt( &p, qfalse ) );
						rgbConst.color[1] = (float)atof( COM_ParseExt( &p, qfalse ) );
						rgbConst.color[2] = (float)atof( COM_ParseExt( &p, qfalse ) );
						COM_ParseExt( &p, qfalse ); // closing ")"
						haveRgbConst = true;
					}
				}
				continue;
			}

			// `tcGen <type>` - only whether the keyword appears at all is
			// recorded (see s_shaderHasTcGen's own comment) - no UV
			// generation mode is actually implemented, so the type itself
			// and any following arguments (`tcGen vector` takes two extra
			// vec3s) are deliberately left unconsumed, same as tcMod's own
			// unhandled types above.
			if ( depth == 2 && inFirstStage && !Q_stricmp( tok, "tcGen" ) )
			{
				hasTcGen = true;
				continue;
			}

			if ( depth == 2 && inFirstStage && !Q_stricmp( tok, "blendFunc" ) )
			{
				// COM_ParseExt returns a pointer into its own single static
				// buffer (com_token) - reused on every call. Copying the
				// first token into a local std::string before parsing the
				// second is not just tidiness: without it, parsing `b`
				// silently overwrites the very buffer `a` still points at,
				// so BlendFactorsToMode ends up comparing (b, b) instead of
				// (a, b). Confirmed the hard way - this exact aliasing bug
				// classified every explicit two-token `blendFunc GL_ONE
				// GL_ZERO` shader (the standard "opaque" spelling, used
				// throughout shaders/players.shader) as BLEND_ALPHA instead
				// of BLEND_OPAQUE, which silently defeated
				// RE_LoadWorldMap's/VK_LoadGhoul2Model's BLEND_OPAQUE-gated
				// shader-script fallback for every shader that took this
				// branch - e.g. vjun1's jedi_tf NPC, whose torso skin
				// resolves through exactly such a shader (models/players/
				// jedi_tf/torso_01_clothes) and rendered invisible as a
				// result. A shader with no blendFunc keyword at all (the
				// default-OPAQUE path just below, never entering this
				// block) was never affected, which is why this went
				// unnoticed through hoth2's earlier, similar-looking fix.
				std::string a = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( a.c_str(), "add" ) )
				{
					blendMode = BLEND_ADDITIVE;
				}
				else if ( !Q_stricmp( a.c_str(), "blend" ) )
				{
					blendMode = BLEND_ALPHA;
				}
				else if ( !Q_stricmp( a.c_str(), "filter" ) )
				{
					// GL_DST_COLOR/GL_ZERO (multiply) - no matching pipeline
					// yet, alpha blend is the closest approximation we have.
					blendMode = BLEND_ALPHA;
				}
				else if ( !a.empty() )
				{
					const char *b = COM_ParseExt( &p, qfalse );
					blendMode = BlendFactorsToMode( a.c_str(), b );
				}
			}
		}

		// First definition wins, matching rd-vanilla's "later pk3s can't
		// override earlier-loaded shaders" behavior closely enough for our
		// purposes here.
		if ( s_shaderBlendModes.find( name ) == s_shaderBlendModes.end() )
		{
			s_shaderBlendModes[name] = blendMode;
		}
		if ( haveFogParms && s_shaderFogParms.find( name ) == s_shaderFogParms.end() )
		{
			s_shaderFogParms[name] = fogParms;
		}
		if ( !mapImage.empty() && s_shaderMapImage.find( name ) == s_shaderMapImage.end() )
		{
			s_shaderMapImage[name] = mapImage;
		}
		if ( haveTcModScroll && s_shaderTcModScroll.find( name ) == s_shaderTcModScroll.end() )
		{
			s_shaderTcModScroll[name] = tcModScroll;
		}
		if ( havePortalRange && s_shaderPortalRange.find( name ) == s_shaderPortalRange.end() )
		{
			s_shaderPortalRange[name] = portalRange;
		}
		if ( haveRgbConst && s_shaderRgbConst.find( name ) == s_shaderRgbConst.end() )
		{
			s_shaderRgbConst[name] = rgbConst;
		}
		if ( hasTcGen && s_shaderHasTcGen.find( name ) == s_shaderHasTcGen.end() )
		{
			s_shaderHasTcGen[name] = true;
		}
	}

	COM_EndParseSession();
}

void VK_LoadShaderScripts( void )
{
	if ( s_shadersLoaded )
	{
		return;
	}
	s_shadersLoaded = true;

	int numShaderFiles = 0;
	char **shaderFiles = ri.FS_ListFiles( "shaders", ".shader", &numShaderFiles );
	if ( !shaderFiles || !numShaderFiles )
	{
		if ( shaderFiles )
		{
			ri.FS_FreeFileList( shaderFiles );
		}
		return;
	}

	for ( int i = 0; i < numShaderFiles; i++ )
	{
		char filename[MAX_QPATH];
		Com_sprintf( filename, sizeof( filename ), "shaders/%s", shaderFiles[i] );

		char *buffer = nullptr;
		long len = ri.FS_ReadFile( filename, (void **)&buffer );
		if ( !buffer || len <= 0 )
		{
			continue;
		}

		// FS_ReadFile's buffer isn't guaranteed null-terminated - copy into a
		// null-terminated std::string before handing it to COM_ParseExt,
		// which expects a plain C string.
		std::string text( buffer, (size_t)len );
		ri.FS_FreeFile( buffer );

		ParseShaderFile( text.c_str() );
	}

	ri.FS_FreeFileList( shaderFiles );

	ri.Printf( PRINT_ALL, "rd-vulkan: parsed %d .shader file(s), %d blend mode(s) recorded\n",
		numShaderFiles, (int)s_shaderBlendModes.size() );
}

// A .shader block's own name never carries a file extension, but callers
// routinely look one up by a name that does - most commonly a .skin file's
// own surface-override lines, which are free to write a shader reference
// exactly like a texture path complete with extension (e.g. jedi_tf's real
// torso_a1.skin: `torsoa,models/players/jedi_tf/torso_01_clothes.tga`) even
// when that "shader" is actually a .shader script block named without one
// (`models/players/jedi_tf/torso_01_clothes { ... }`, shaders/
// players.shader). rd-vanilla's real R_FindShader/R_FindShaderByName both
// unconditionally COM_StripExtension the incoming name before comparing
// against defined shader names for exactly this reason - confirmed the hard
// way here: without this, VK_GetShaderBlendMode/VK_GetShaderMapImage below
// looked up "...torso_01_clothes.tga" against a map keyed by
// "...torso_01_clothes", missed every time, and silently dropped that NPC's
// torso (and every other surface sharing this same extension-on-a-shader-
// name skin convention) - not a hoth2-style missing-fallback gap, this one
// had the right fallback already, just never actually matched anything.
static std::string VK_StripShaderNameExtension( const char *name )
{
	char stripped[MAX_QPATH];
	COM_StripExtension( name, stripped, sizeof( stripped ) );
	return stripped;
}

vkBlendMode_t VK_GetShaderBlendMode( const char *name, vkBlendMode_t notFoundDefault )
{
	VK_LoadShaderScripts();

	auto it = s_shaderBlendModes.find( VK_StripShaderNameExtension( name ) );
	if ( it != s_shaderBlendModes.end() )
	{
		return it->second;
	}
	return notFoundDefault;
}

// Looks up a fog shader's `fogparms` (see VK_LoadWorldFog, tr_world.cpp,
// the only caller - BSP LUMP_FOGS entries reference a shader by name for
// its color/opaque-distance, not a texture). Returns false (color/dist
// left untouched) if the shader wasn't found or never declared fogparms.
bool VK_GetShaderFogParms( const char *name, float color[3], float *opaqueDist )
{
	VK_LoadShaderScripts();

	auto it = s_shaderFogParms.find( VK_StripShaderNameExtension( name ) );
	if ( it == s_shaderFogParms.end() )
	{
		return false;
	}
	color[0] = it->second.color[0];
	color[1] = it->second.color[1];
	color[2] = it->second.color[2];
	*opaqueDist = it->second.opaqueDist;
	return true;
}

// Looks up a shader's first stage's `tcMod scroll <sSpeed> <tSpeed>` (units
// per second, added directly to the diffuse UV - see RE_LoadWorldMap/
// RE_RenderScene, tr_world.cpp, the only caller). Returns false (speeds
// left untouched) if the shader wasn't found or its first stage never
// declared a scroll - the common case, and the caller's default (no
// scroll) is exactly correct for it. Real, visible test case: vjun1's
// `textures/impdetention/deathcon1a` (a containment-field texture, 51 real
// world surfaces), confirmed by direct BSP/.shader parsing, not assumed.
bool VK_GetShaderTcModScroll( const char *name, float *sSpeed, float *tSpeed )
{
	VK_LoadShaderScripts();

	auto it = s_shaderTcModScroll.find( VK_StripShaderNameExtension( name ) );
	if ( it == s_shaderTcModScroll.end() )
	{
		return false;
	}
	*sSpeed = it->second.sSpeed;
	*tSpeed = it->second.tSpeed;
	return true;
}

// Looks up a flare shader's `alphaGen portal <range>` numeric argument (see
// RE_LoadWorldMap's MST_FLARE handling, tr_world.cpp, the only caller).
// Returns rd-vanilla's own RB_SurfaceFlare default (30) if the shader wasn't
// found or never declared it - exactly matching real behavior for a flare
// shader with no alphaGen at all (the common case: only flare_blue_pulse of
// this checkout's 3 real flare shaders declares one, at 50).
float VK_GetShaderPortalRange( const char *name )
{
	VK_LoadShaderScripts();

	auto it = s_shaderPortalRange.find( VK_StripShaderNameExtension( name ) );
	if ( it == s_shaderPortalRange.end() )
	{
		return 30.0f;
	}
	return it->second;
}

// Looks up a shader's first stage's `rgbGen const ( r g b )` colour (see
// RE_LoadWorldMap's per-vertex colour overwrite, tr_world.cpp, the only
// caller). Returns false (color left untouched) if the shader wasn't found
// or never declared one - the common case, matching the caller's default
// (whatever colour the surface already had - white, or real baked vertex
// colour) exactly.
bool VK_GetShaderRgbGenConst( const char *name, float color[3] )
{
	VK_LoadShaderScripts();

	auto it = s_shaderRgbConst.find( VK_StripShaderNameExtension( name ) );
	if ( it == s_shaderRgbConst.end() )
	{
		return false;
	}
	color[0] = it->second.color[0];
	color[1] = it->second.color[1];
	color[2] = it->second.color[2];
	return true;
}

// Whether a shader's first stage declares any `tcGen` keyword at all (see
// RE_LoadWorldMap's map-image-fallback safety gate, tr_world.cpp, the only
// caller, and s_shaderHasTcGen's own comment for why).
bool VK_ShaderHasTcGen( const char *name )
{
	VK_LoadShaderScripts();

	return s_shaderHasTcGen.find( VK_StripShaderNameExtension( name ) ) != s_shaderHasTcGen.end();
}

// Looks up a shader's first stage's `map`/`clampmap` path - the only
// caller, RE_LoadWorldMap's opaque-world-surface loop (tr_world.cpp), uses
// this as a *fallback* when a shader's own name doesn't directly resolve to
// a texture file (VK_FindImage(shaders[surf.shaderNum].shader) failed):
// plenty of real shaders, not just stages-only effect shaders, reuse an
// existing texture under a differently-named shader (e.g. a `_vertex`
// variant for `rgbGen exactVertex` terrain blending - hoth2's
// `textures/hoth/metal_lg_lt_vertex`, whose first stage is actually `map
// textures/impgarrison/metal_lg_lt`). Treating every such shader-name lookup
// failure as "no direct image, presumably a translucent effect shader, skip
// this surface" (the assumption RE_LoadWorldMap otherwise makes) was
// silently discarding ~40% of hoth2's opaque terrain surfaces - a real,
// confirmed bug (a user reported the character's own torso and the whole
// level reading as flat grey), not a deliberate simplification like the
// genuine effect-shader skip is. Returns nullptr if the shader was never
// seen, or was seen but its first stage has no real file `map` (a bare
// stages-only effect shader, or one using a generated `$whiteimage`/
// `$lightmap` image - see s_shaderMapImage's own comment for why those
// aren't recorded) - RE_LoadWorldMap's existing "skip the surface" handling
// is still exactly correct for that case, this fallback only helps the
// cases where a real file exists under a different name than the shader.
const char *VK_GetShaderMapImage( const char *name )
{
	VK_LoadShaderScripts();

	auto it = s_shaderMapImage.find( VK_StripShaderNameExtension( name ) );
	if ( it == s_shaderMapImage.end() )
	{
		return nullptr;
	}
	return it->second.c_str();
}
