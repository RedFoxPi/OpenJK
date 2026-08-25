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

vkBlendMode_t VK_GetShaderBlendMode( const char *name )
{
	VK_LoadShaderScripts();

	auto it = s_shaderBlendModes.find( VK_StripShaderNameExtension( name ) );
	if ( it != s_shaderBlendModes.end() )
	{
		return it->second;
	}
	return BLEND_ALPHA;
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
