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

// First stage's `tcMod scroll <sSpeed> <tSpeed>` and `tcMod scale <sx>
// <sy>`, keyed by shader name - see VK_GetShaderTcModScroll's own comment
// for what this drives. Every other tcMod type (`rotate`/`stretch`/`turb`/
// `transform`/`entityTranslate`) and every rgbGen/alphaGen wave are still
// unread - see rd-vulkan/README.md.
//
// sSpeed/tSpeed are stored already corrected for declaration order against
// `tcMod scale` on the same stage, not the raw per-second speed a bare
// `tcMod scroll` line would suggest - real Quake3 composes a stage's tcMod
// keywords as an ordered matrix stack (each one transforms the *output* of
// whichever came before it), so `tcMod scale` then `tcMod scroll` leaves
// the scroll offset unscaled (scale applies to the base UV only), while
// `tcMod scroll` then `tcMod scale` scales the scroll offset too (scale
// applies to everything after it, including the moving part). Baking that
// distinction into this stored speed once, at parse time, means the
// render-time formula stays the same simple `uv*scale + speed*time` in
// both cases - see ParseShaderFile's own comment at the `tcMod` parsing
// site for the actual order-tracking logic, and VK_GetShaderTcModScroll's
// comment for real confirmed examples of both orderings.
struct vkShaderTcModScroll_t
{
	float sSpeed, tSpeed;
	float scaleS = 1.0f, scaleT = 1.0f; // identity (no visible change) when no `tcMod scale` was declared
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

// First stage's `rgbGen wave sin <base> <amp> <phase> <freq>`, keyed by
// shader name - `sin` only (see VK_GetShaderRgbWave's own comment for why:
// the only real wave function this checkout's own flare shaders use).
// Unlike `rgbGen const`, this needs to be a real live per-frame value
// (real rd-vanilla's own EvalWaveForm/RB_CalcWaveColor re-evaluate it every
// draw, not a fixed colour baked once), so this only stores the raw wave
// parameters - VK_DrawWorldFlares (tr_world.cpp) evaluates the actual
// formula at draw time.
struct vkShaderRgbWave_t
{
	float base, amp, phase, freq;
};
static std::unordered_map<std::string, vkShaderRgbWave_t> s_shaderRgbWave;

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

// Whether the first stage's `tcGen` is specifically `environment` (real
// per-vertex reflection-vector UV generation, RB_CalcEnvironmentTexCoords in
// rd-vanilla's tr_shade_calc.cpp), keyed by shader name. Confirmed real,
// substantial usage on this renderer's own test maps' actual world BSP
// surfaces (not just present somewhere in the broader shader library): 22
// hoth2 surfaces (shiny metal walls, blast panels, doors, the exit beam) and
// 6 vjun1 surfaces (security glass, env_glass, the imperial square trim) all
// declare `tcGen environment` on their first stage. See
// VK_GetShaderTcGenEnvironment and world.vert/RE_LoadWorldMap for the actual
// implementation - this map only records which shaders need it.
static std::unordered_map<std::string, bool> s_shaderTcGenEnvironment;

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
		bool haveTcModScale = false;
		// True only if `tcMod scale` was seen before `tcMod scroll` on this
		// stage - see vkShaderTcModScroll_t's own comment for why this
		// changes what gets stored. Meaningless (never read) when only one
		// of the two is present.
		bool tcModScaleBeforeScroll = false;
		vkShaderTcModScroll_t tcModScroll = {};
		bool havePortalRange = false;
		float portalRange = 0.0f;
		bool haveRgbConst = false;
		vkShaderRgbConst_t rgbConst = {};
		bool haveRgbWave = false;
		vkShaderRgbWave_t rgbWave = {};
		bool hasTcGen = false;
		bool tcGenEnvironment = false;
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
			// count (`scroll <s> <t>`, `scale <sx> <sy>`, `rotate <deg/sec>`,
			// `stretch <func> <base> <amp> <phase> <freq>`, ...) - only
			// `scroll`'s and `scale`'s own numeric arguments are understood
			// and recorded here (see VK_GetShaderTcModScroll's comment for
			// why just these two). Every other tcMod type's numeric
			// arguments are deliberately left unconsumed after their type
			// keyword is read - they fall through to the bottom of this
			// loop as ordinary unmatched tokens and are silently skipped,
			// exactly like any other unrecognized keyword already is
			// (blendFunc/map's own specific handling works the same way:
			// anything not specifically intercepted is just ignored, not an
			// error).
			if ( depth == 2 && inFirstStage && !Q_stricmp( tok, "tcMod" ) )
			{
				std::string modType = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( modType.c_str(), "scroll" ) )
				{
					tcModScroll.sSpeed = (float)atof( COM_ParseExt( &p, qfalse ) );
					tcModScroll.tSpeed = (float)atof( COM_ParseExt( &p, qfalse ) );
					haveTcModScroll = true;
					// See vkShaderTcModScroll_t's own comment - scale was
					// already seen on this stage means scale came first.
					if ( haveTcModScale )
					{
						tcModScaleBeforeScroll = true;
					}
				}
				else if ( !Q_stricmp( modType.c_str(), "scale" ) )
				{
					tcModScroll.scaleS = (float)atof( COM_ParseExt( &p, qfalse ) );
					tcModScroll.scaleT = (float)atof( COM_ParseExt( &p, qfalse ) );
					haveTcModScale = true;
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

			// `rgbGen const ( r g b )` and `rgbGen wave <func> <base> <amp>
			// <phase> <freq>` (sin only - see s_shaderRgbWave's own
			// comment) are recorded (see s_shaderRgbConst's own comment for
			// const); every other rgbGen keyword (`identityLighting`,
			// `vertex`, non-sin wave functions, ...) falls through
			// unconsumed like any other unrecognized token, same "not
			// specifically intercepted" pattern as alphaGen above.
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
				else if ( !Q_stricmp( a.c_str(), "wave" ) )
				{
					std::string func = COM_ParseExt( &p, qfalse );
					float base = (float)atof( COM_ParseExt( &p, qfalse ) );
					float amp = (float)atof( COM_ParseExt( &p, qfalse ) );
					float phase = (float)atof( COM_ParseExt( &p, qfalse ) );
					float freq = (float)atof( COM_ParseExt( &p, qfalse ) );
					if ( !Q_stricmp( func.c_str(), "sin" ) )
					{
						rgbWave.base = base;
						rgbWave.amp = amp;
						rgbWave.phase = phase;
						rgbWave.freq = freq;
						haveRgbWave = true;
					}
				}
				continue;
			}

			// `tcGen <type>` - whether the keyword appears at all is recorded
			// (see s_shaderHasTcGen's own comment) and the type itself is
			// checked for `environment` specifically (see
			// s_shaderTcGenEnvironment's comment - the one tcGen mode this
			// renderer actually implements). Any further arguments a type
			// might take (`tcGen vector` takes two extra vec3s) are
			// deliberately left unconsumed, same as tcMod's own unhandled
			// types above - harmless, since they don't match any recognized
			// keyword on the next iterations either.
			if ( depth == 2 && inFirstStage && !Q_stricmp( tok, "tcGen" ) )
			{
				hasTcGen = true;
				const char *type = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( type, "environment" ) )
				{
					tcGenEnvironment = true;
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
		if ( ( haveTcModScroll || haveTcModScale ) && s_shaderTcModScroll.find( name ) == s_shaderTcModScroll.end() )
		{
			// `tcMod scroll` then `tcMod scale` (scaleBeforeScroll false,
			// the default) scales the moving part too - see
			// vkShaderTcModScroll_t's own comment for the full reasoning.
			// Baking that into the stored speed here, once, keeps the
			// render-time formula (world.vert) the same simple
			// `uv*scale + speed*time` regardless of which order a given
			// shader actually declared them in.
			if ( haveTcModScroll && haveTcModScale && !tcModScaleBeforeScroll )
			{
				tcModScroll.sSpeed *= tcModScroll.scaleS;
				tcModScroll.tSpeed *= tcModScroll.scaleT;
			}
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
		if ( haveRgbWave && s_shaderRgbWave.find( name ) == s_shaderRgbWave.end() )
		{
			s_shaderRgbWave[name] = rgbWave;
		}
		if ( hasTcGen && s_shaderHasTcGen.find( name ) == s_shaderHasTcGen.end() )
		{
			s_shaderHasTcGen[name] = true;
		}
		if ( tcGenEnvironment && s_shaderTcGenEnvironment.find( name ) == s_shaderTcGenEnvironment.end() )
		{
			s_shaderTcGenEnvironment[name] = true;
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
// per second, added directly to the diffuse UV after scaleS/scaleT below is
// applied - see RE_LoadWorldMap/RE_RenderScene, tr_world.cpp, the only
// caller) and `tcMod scale <sx> <sy>` (a constant multiplier on the diffuse
// UV - real Quake3 recomputes this every frame via its tcMod matrix stack,
// but since it never varies with time, baking it in once here produces
// pixel-identical output). Returns false (both left untouched) if the
// shader wasn't found or its first stage declared neither - the common
// case, and the caller's defaults (no scroll, scaleS/T = 1.0 identity) are
// exactly correct for it. Real, visible test cases confirmed by direct
// BSP/.shader parsing, not assumed: vjun1's
// `textures/impdetention/deathcon1a` (scroll, 51 real world surfaces) and
// hoth2's `textures/hoth/rock_huge_snow`/`textures/hoth/at_at_leg` (scale
// only, `tcMod scale 0.5 0.5`/`tcMod scale 4 4`).
bool VK_GetShaderTcModScroll( const char *name, float *sSpeed, float *tSpeed, float *scaleS, float *scaleT )
{
	VK_LoadShaderScripts();

	auto it = s_shaderTcModScroll.find( VK_StripShaderNameExtension( name ) );
	if ( it == s_shaderTcModScroll.end() )
	{
		return false;
	}
	*sSpeed = it->second.sSpeed;
	*tSpeed = it->second.tSpeed;
	*scaleS = it->second.scaleS;
	*scaleT = it->second.scaleT;
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

// Looks up a shader's first stage's `rgbGen const ( r g b )` colour - two
// real callers now: RE_LoadWorldMap's per-vertex colour overwrite for
// ordinary world surfaces (tr_world.cpp), and VK_DrawWorldFlares
// (tr_world.cpp) for flares, confirmed real via vjun1's own
// `textures/flares/flare_bluehue` (29 of 45 real flare surfaces on that
// map - see rd-vulkan/README.md). Returns false (color left untouched) if
// the shader wasn't found or never declared one - the common case,
// matching each caller's own default (whatever colour the surface already
// had - white/baked vertex colour for ordinary surfaces, the flare's own
// view-angle fade for flares) exactly.
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

// Looks up a shader's first stage's `rgbGen wave sin <base> <amp> <phase>
// <freq>` (VK_DrawWorldFlares, tr_world.cpp, the only caller) - `sin` only:
// a grep across every real flare shader in this checkout's own game data
// found exactly one wave function ever used on a first-stage rgbGen wave
// (`square`/`triangle`/`sawtooth`/`inverseSawtooth`/`noise` are all real
// rd-vanilla features - G2_bones.cpp... no, tr_shade_calc.cpp's
// TableForFunc - with zero exercised callers found here), so only that one
// is implemented, matching this renderer's usual evidence-scoped approach.
// Confirmed real and heavily exercised: hoth2's own
// `textures/flares/flare_blue_pulse` (55 of 98 real flare surfaces on that
// map, more than half - see rd-vulkan/README.md), `rgbGen wave sin 0.5 1
// 0.2 0.5`. Returns false (all four out-params left untouched) if the
// shader wasn't found or never declared a sine wave - the common case.
// The caller evaluates the actual time-varying formula itself (this just
// returns the raw parameters) - see VK_DrawWorldFlares's own comment for
// why, unlike `rgbGen const`, this can't be resolved to a single fixed
// value once here.
bool VK_GetShaderRgbWave( const char *name, float *base, float *amp, float *phase, float *freq )
{
	VK_LoadShaderScripts();

	auto it = s_shaderRgbWave.find( VK_StripShaderNameExtension( name ) );
	if ( it == s_shaderRgbWave.end() )
	{
		return false;
	}
	*base = it->second.base;
	*amp = it->second.amp;
	*phase = it->second.phase;
	*freq = it->second.freq;
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

// Whether a shader's first stage declares `tcGen environment` specifically
// (see s_shaderTcGenEnvironment's own comment). RE_LoadWorldMap uses this to
// flag matching WorldSurfaceBatch entries for the real per-vertex reflection
// UV generation (world.vert) instead of ordinary baked/scrolled/scaled UVs.
bool VK_GetShaderTcGenEnvironment( const char *name )
{
	VK_LoadShaderScripts();

	return s_shaderTcGenEnvironment.find( VK_StripShaderNameExtension( name ) ) != s_shaderTcGenEnvironment.end();
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
