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

			if ( depth == 2 && inFirstStage && !Q_stricmp( tok, "blendFunc" ) )
			{
				const char *a = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( a, "add" ) )
				{
					blendMode = BLEND_ADDITIVE;
				}
				else if ( !Q_stricmp( a, "blend" ) )
				{
					blendMode = BLEND_ALPHA;
				}
				else if ( !Q_stricmp( a, "filter" ) )
				{
					// GL_DST_COLOR/GL_ZERO (multiply) - no matching pipeline
					// yet, alpha blend is the closest approximation we have.
					blendMode = BLEND_ALPHA;
				}
				else if ( a[0] )
				{
					const char *b = COM_ParseExt( &p, qfalse );
					blendMode = BlendFactorsToMode( a, b );
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

vkBlendMode_t VK_GetShaderBlendMode( const char *name )
{
	VK_LoadShaderScripts();

	auto it = s_shaderBlendModes.find( name );
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

	auto it = s_shaderFogParms.find( name );
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
