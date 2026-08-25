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

// World weather/particle effects - rain, snow, wind-blown fog/dust/sand -
// see this file's own declarations (tr_local.h) for the full picture of
// what this is a port of and what's deliberately simplified. Everything
// below is organized in the same order rd-vanilla's real tr_WorldEffects.cpp
// is: small math/range helpers, the wind model, the particle-cloud model,
// then the public entry points (VK_WorldEffectCommand's preset table last,
// since it's the biggest single piece and everything above it exists to
// serve it).

#include "../server/exe_headers.h"

#include "tr_local.h"
#include <cmath>
#include <cstdlib>
#include <vector>
#include <string>

////////////////////////////////////////////////////////////////////////////
// Small math/random helpers
////////////////////////////////////////////////////////////////////////////

// This renderer has no other caller for VectorNormalize/MakeNormalVectors
// (rd-vanilla's real versions live in a math file this checkout doesn't
// carry into rd-vulkan's link - see tr_local.h's comment), so small
// self-contained equivalents live here rather than pulling in a whole new
// shared math source file for two functions.
static float VK_WVectorNormalize( float v[3] )
{
	float length = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
	if ( length > 0.0f )
	{
		float inv = 1.0f / length;
		v[0] *= inv; v[1] *= inv; v[2] *= inv;
	}
	return length;
}

// Any valid orthonormal (right, up) basis perpendicular to a given unit
// forward vector - used only to build a "spawn plane" perpendicular to the
// gravity+wind force particles fall along (VK_UpdateParticleCloud). Not
// bit-identical to rd-vanilla's real MakeNormalVectors (whatever tie-
// breaking axis choice it uses internally is not visible from this
// checkout), but any valid orthonormal basis here is visually
// indistinguishable - the spawn plane just needs to be flat and
// perpendicular to forward, not any particular rotation within that plane,
// since particles are scattered randomly across it anyway.
static void VK_WMakeNormalVectors( const float forward[3], float right[3], float up[3] )
{
	if ( fabsf( forward[2] ) < 0.999f )
	{
		right[0] = -forward[1]; right[1] = forward[0]; right[2] = 0.0f;
	}
	else
	{
		right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
	}
	VK_WVectorNormalize( right );
	up[0] = forward[1] * right[2] - forward[2] * right[1];
	up[1] = forward[2] * right[0] - forward[0] * right[2];
	up[2] = forward[0] * right[1] - forward[1] * right[0];
}

static inline float VK_WFloatRand( float mn, float mx )
{
	return mn + ( mx - mn ) * ( (float)rand() / (float)RAND_MAX );
}

static inline int VK_WIntRand( int mn, int mx )
{
	if ( mx <= mn )
	{
		return mn;
	}
	return mn + ( rand() % ( mx - mn + 1 ) );
}

static inline bool VK_WInRange3( const float v[3], const float mins[3], const float maxs[3] )
{
	return v[0] > mins[0] && v[0] < maxs[0]
		&& v[1] > mins[1] && v[1] < maxs[1]
		&& v[2] > mins[2] && v[2] < maxs[2];
}

// Wraps a position that has left [mins,maxs] back in from the opposite
// side (a small inset, not flush against the boundary) - unless it's gone
// more than 500 units past the edge, in which case it's treated as "so far
// out this isn't a meaningful wrap any more" and re-scattered randomly
// across the whole range instead. Exact port of rd-vanilla's real
// SVecRange::Wrap (tr_WorldEffects.cpp) - used for gravity-less clouds
// (spacedust/sand/fog/...) that drift rather than fall.
static void VK_WWrapRange( float v[3], const float mins[3], const float maxs[3] )
{
	for ( int i = 0; i < 3; i++ )
	{
		if ( v[i] <= mins[i] )
		{
			if ( ( mins[i] - v[i] ) > 500.0f )
			{
				v[0] = VK_WFloatRand( mins[0], maxs[0] );
				v[1] = VK_WFloatRand( mins[1], maxs[1] );
				v[2] = VK_WFloatRand( mins[2], maxs[2] );
				return;
			}
			v[i] = maxs[i] - 10.0f;
		}
		if ( v[i] >= maxs[i] )
		{
			if ( ( v[i] - maxs[i] ) > 500.0f )
			{
				v[0] = VK_WFloatRand( mins[0], maxs[0] );
				v[1] = VK_WFloatRand( mins[1], maxs[1] );
				v[2] = VK_WFloatRand( mins[2], maxs[2] );
				return;
			}
			v[i] = mins[i] + 10.0f;
		}
	}
}

// Exact port of rd-vanilla's real WE_ParseVector (tr_WorldEffects.cpp) -
// "( f f f )" with mandatory surrounding parens and spaces (COM_ParseExt's
// own limitation, not this port's).
static bool VK_WParseVector( const char **text, float v[3] )
{
	COM_BeginParseSession();
	const char *token = COM_ParseExt( text, qfalse );
	if ( strcmp( token, "(" ) )
	{
		COM_EndParseSession();
		return false;
	}
	for ( int i = 0; i < 3; i++ )
	{
		token = COM_ParseExt( text, qfalse );
		if ( !token[0] )
		{
			COM_EndParseSession();
			return false;
		}
		v[i] = (float)atof( token );
	}
	token = COM_ParseExt( text, qfalse );
	COM_EndParseSession();
	return strcmp( token, ")" ) == 0;
}

////////////////////////////////////////////////////////////////////////////
// Outside/inside testing
////////////////////////////////////////////////////////////////////////////

// rd-vanilla's real COutside builds a per-map 32-unit-cell inside/outside
// grid (tr_WorldEffects.cpp's COutside::Cache/PointOutside), cached to disk,
// so a whole scene's worth of particles can be tested every frame without
// re-touching the collision system - a pure performance optimization over
// what's actually being tested underneath, not a different question. This
// renderer skips the grid/cache entirely and asks the collision system
// directly per particle per frame instead (ri.CM_PointContents) - always
// exactly as correct, just without the caching. At the particle counts real
// presets actually use (hundreds to low thousands, see
// MAX_WEATHER_PARTICLES_PER_CLOUD's comment) this hasn't been a measured
// performance problem for this renderer's existing test scenes; if it ever
// became one for a much larger scene, the cache would be the thing to add
// back, not a reason to skip the query entirely and guess instead.
//
// What IS worth replicating carefully is *which* contents bit means
// "outside" - real COutside::Cache doesn't just test CONTENTS_OUTSIDE, it
// first scans the whole map to see whether this particular map marks its
// (smaller) outdoor pockets as CONTENTS_OUTSIDE against a mostly-indoor
// default, or marks its (smaller) indoor pockets as CONTENTS_INSIDE against
// a mostly-outdoor default (`SWeatherZone::mMarkedOutside`, tr_WorldEffects.cpp) -
// whichever one it finds evidence of first is assumed to be the whole map's
// convention (and it errors if it ever finds evidence of both). Checked
// this directly against real BSP data rather than assuming: parsed the
// LUMP_BRUSHES/LUMP_SHADERS content flags of every map this renderer is
// tested against - academy1 (0 CONTENTS_OUTSIDE, 0 CONTENTS_INSIDE
// brushes), yavin1 (0, 0), hoth2 (0, 16), vjun1 (0, 90). CONTENTS_OUTSIDE
// literally never appears in any of them; CONTENTS_INSIDE, when used at
// all, marks a small number of indoor volumes against an implicit
// "otherwise outdoors" default - exactly the second convention above, with
// no real counterexample in this checkout's map data to weigh against it.
// So: outside means "not solid/underwater, and not inside an explicitly
// CONTENTS_INSIDE-flagged volume" - not "has CONTENTS_OUTSIDE set", which
// would (and, before this was checked against real data, did) read as
// permanently false for hoth2's wide-open, entirely unflagged blizzard
// terrain.
static bool VK_WContentsOutside( int contents )
{
	if ( ( contents & CONTENTS_WATER ) || ( contents & CONTENTS_SOLID ) )
	{
		return false;
	}
	return ( contents & CONTENTS_INSIDE ) == 0;
}

static bool VK_WPointOutside( const float pos[3] )
{
	return VK_WContentsOutside( ri.CM_PointContents( pos, 0 ) );
}

////////////////////////////////////////////////////////////////////////////
// Wind
////////////////////////////////////////////////////////////////////////////

// Exact port of rd-vanilla's real CWindZone, except mTargetVelocityTimeRemaining
// (there, a countdown decremented once per Update() *call* - implicitly
// framerate-dependent, since Update() is called once per rendered frame -
// counts down here in real milliseconds instead) and the velocity
// approach-to-target (there, stepped by at most mMaxDeltaVelocityPerUpdate
// units per *call*) is likewise expressed per second here. Both are
// deliberate, framerate-independent equivalents of a framerate-dependent
// original - see this file's own top comment - not a difference that
// changes what a wind zone visually does (gust changes still land every
// 1-3 real seconds, ramps still smooth out over a fraction of a second),
// just how the timing is measured.
struct VulkanWindZone
{
	bool global = true;
	float boundsMin[3] = { 0, 0, 0 };
	float boundsMax[3] = { 0, 0, 0 };

	float velocityRangeMin[3] = { -1500.0f, -1500.0f, -10.0f };
	float velocityRangeMax[3] = { 1500.0f, 1500.0f, 10.0f };
	int durationMinMs = 1000, durationMaxMs = 2000;
	float maxDeltaVelocityPerSecond = 600.0f; // ~10 units/tick at a ~60Hz assumed original tick rate
	float chanceOfDeadTime = 0.3f;
	int deadTimeMinMs = 1000, deadTimeMaxMs = 3000;

	float currentVelocity[3] = { 0, 0, 0 };
	float targetVelocity[3] = { 0, 0, 0 };
	// -1 = constant (never retargets - "constantwind"/"windzone"), matching
	// rd-vanilla's real sentinel meaning exactly.
	float targetVelocityTimeRemainingMs = 0.0f;

	void Update( float dtMs )
	{
		if ( targetVelocityTimeRemainingMs == -1.0f )
		{
			return;
		}
		if ( targetVelocityTimeRemainingMs <= 0.0f )
		{
			if ( VK_WFloatRand( 0.0f, 1.0f ) < chanceOfDeadTime )
			{
				targetVelocityTimeRemainingMs = (float)VK_WIntRand( deadTimeMinMs, deadTimeMaxMs );
				targetVelocity[0] = targetVelocity[1] = targetVelocity[2] = 0.0f;
			}
			else
			{
				targetVelocityTimeRemainingMs = (float)VK_WIntRand( durationMinMs, durationMaxMs );
				targetVelocity[0] = VK_WFloatRand( velocityRangeMin[0], velocityRangeMax[0] );
				targetVelocity[1] = VK_WFloatRand( velocityRangeMin[1], velocityRangeMax[1] );
				targetVelocity[2] = VK_WFloatRand( velocityRangeMin[2], velocityRangeMax[2] );
			}
		}
		targetVelocityTimeRemainingMs -= dtMs;

		float delta[3] = { targetVelocity[0] - currentVelocity[0], targetVelocity[1] - currentVelocity[1], targetVelocity[2] - currentVelocity[2] };
		float deltaLen = VK_WVectorNormalize( delta );
		float maxStep = maxDeltaVelocityPerSecond * ( dtMs / 1000.0f );
		if ( deltaLen > maxStep )
		{
			deltaLen = maxStep;
		}
		currentVelocity[0] += delta[0] * deltaLen;
		currentVelocity[1] += delta[1] * deltaLen;
		currentVelocity[2] += delta[2] * deltaLen;
	}
};

static const int MAX_WIND_ZONES = 12;
static std::vector<VulkanWindZone> s_windZones;
static std::vector<int> s_localWindZoneIndices; // indices into s_windZones

static float s_globalWindVelocity[3] = { 0, 0, 0 };
static float s_globalWindDirection[3] = { 1, 0, 0 };
static float s_globalWindSpeed = 0.0f;

////////////////////////////////////////////////////////////////////////////
// Particle cloud
////////////////////////////////////////////////////////////////////////////

struct WeatherParticle
{
	float pos[3] = { 0, 0, 0 };
	float velocity[3] = { 0, 0, 0 };
	float mass = 1.0f;
	float alpha = 0.0f;
	bool rendering = false;
	bool fadeIn = false;
	bool fadeOut = false;
};

// Exact port of rd-vanilla's real CParticleCloud (tr_WorldEffects.cpp) -
// see VK_UpdateParticleCloud/VK_EmitParticleCloud for the physics/rendering
// port itself; this struct just holds the same per-cloud constants under
// the same names/defaults (CParticleCloud::Reset).
struct VulkanParticleCloud
{
	image_t *image = nullptr;
	std::vector<WeatherParticle> particles;
	bool populated = false;

	bool orientWithVelocity = false;
	bool waterParticles = false;

	float spawnPlaneDistance = 500.0f;
	float spawnPlaneSize = 500.0f;
	float spawnRangeMin[3] = { -625.0f, -625.0f, -625.0f };
	float spawnRangeMax[3] = { 625.0f, 625.0f, 625.0f };

	float gravity = 300.0f;
	float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	float width = 1.0f;
	float height = 1.0f;

	int blendMode = 0; // 0 = alpha, 1 = additive (src+src, all channels scaled by alpha)

	float fade = 10.0f;

	float rotationMin = -0.7f, rotationMax = 0.7f;
	float rotationDelta = 0.0f;
	float rotationDeltaTarget = 0.0f;
	float rotationCurrent = 0.0f;
	int rotationChangeTimerMinMs = 500, rotationChangeTimerMaxMs = 2000;
	// -1 = rotation disabled entirely (default - matches real mRotationChangeNext's
	// "-1 means never" sentinel); 0 = pick a rotation immediately on the next update.
	float rotationChangeNextMs = -1.0f;

	float massMin = 5.0f, massMax = 10.0f;
	float frictionInverse = 0.7f;

	bool UseSpawnPlane() const { return gravity != 0.0f; }

	void Initialize( int count, const char *texturePath )
	{
		image = VK_FindImage( texturePath );
		if ( !image )
		{
			ri.Printf( PRINT_WARNING, "rd-vulkan: weather: could not find texture %s\n", texturePath );
			image = vk.whiteImage;
		}
		if ( count > MAX_WEATHER_PARTICLES_PER_CLOUD )
		{
			count = MAX_WEATHER_PARTICLES_PER_CLOUD;
		}
		particles.assign( (size_t)count, WeatherParticle() );
		for ( WeatherParticle &p : particles )
		{
			p.mass = VK_WFloatRand( massMin, massMax );
		}
		populated = false;
	}
};

static const int MAX_PARTICLE_CLOUDS = 5;
static std::vector<VulkanParticleCloud> s_particleClouds;

static bool s_frozen = false;
static bool s_outsideShake = false;
static float s_outsidePain = 0.0f;

static bool s_fogColorTempActive = false;
static float s_fogColorSaved[3] = { 0, 0, 0 };

static int s_lastWeatherTimeMs = -1;

////////////////////////////////////////////////////////////////////////////
// Per-frame simulation
////////////////////////////////////////////////////////////////////////////

// Exact port of rd-vanilla's real CParticleCloud::Update (tr_WorldEffects.cpp) -
// same physics, same respawn/fade state machine. cameraForward/cameraLeft/
// cameraUp are fd->viewaxis[0]/[1]/[2] (this renderer's own established
// naming - see tr_model.cpp's sprite code - for what rd-vanilla's version
// calls mCameraForward/mCameraLeft/mCameraDown; axis[2] is genuinely "up"
// in this engine's convention regardless of what Raven's own internal
// variable happened to be named).
static void VK_UpdateParticleCloud( VulkanParticleCloud &cloud, float dtMs, float dtSeconds,
	const float cameraPos[3], const float cameraForward[3] )
{
	if ( s_frozen )
	{
		return;
	}

	float force[3] = { s_globalWindVelocity[0], s_globalWindVelocity[1], s_globalWindVelocity[2] - cloud.gravity };
	// (gravity subtracted from Z above matches real code's `force[2] = -gravity` then `force += windVelocity`)

	float range_mins[3] = { cameraPos[0] + cloud.spawnRangeMin[0], cameraPos[1] + cloud.spawnRangeMin[1], cameraPos[2] + cloud.spawnRangeMin[2] };
	float range_maxs[3] = { cameraPos[0] + cloud.spawnRangeMax[0], cameraPos[1] + cloud.spawnRangeMax[1], cameraPos[2] + cloud.spawnRangeMax[2] };

	float spawnPlaneNorm[3] = { 0, 0, -1 };
	float spawnPlaneRight[3] = { 1, 0, 0 };
	float spawnPlaneUp[3] = { 0, 1, 0 };
	bool useSpawnPlane = cloud.UseSpawnPlane();
	if ( useSpawnPlane )
	{
		for ( int dim = 0; dim < 3; dim++ )
		{
			if ( force[dim] > 0.01f )
			{
				range_mins[dim] -= ( cloud.spawnPlaneDistance / 2.0f );
			}
			else if ( force[dim] < -0.01f )
			{
				range_maxs[dim] += ( cloud.spawnPlaneDistance / 2.0f );
			}
		}
		spawnPlaneNorm[0] = force[0]; spawnPlaneNorm[1] = force[1]; spawnPlaneNorm[2] = force[2];
		VK_WVectorNormalize( spawnPlaneNorm );
		VK_WMakeNormalVectors( spawnPlaneNorm, spawnPlaneRight, spawnPlaneUp );
	}

	float particleFade = cloud.fade * dtSeconds;

	for ( WeatherParticle &part : cloud.particles )
	{
		if ( !cloud.populated )
		{
			part.pos[0] = VK_WFloatRand( range_mins[0], range_maxs[0] );
			part.pos[1] = VK_WFloatRand( range_mins[1], range_maxs[1] );
			part.pos[2] = VK_WFloatRand( range_mins[2], range_maxs[2] );
		}

		float partForce[3] = { force[0], force[1], force[2] };
		for ( int wz : s_localWindZoneIndices )
		{
			const VulkanWindZone &zone = s_windZones[wz];
			if ( VK_WInRange3( part.pos, zone.boundsMin, zone.boundsMax ) )
			{
				partForce[0] += zone.currentVelocity[0];
				partForce[1] += zone.currentVelocity[1];
				partForce[2] += zone.currentVelocity[2];
			}
		}
		partForce[0] /= part.mass; partForce[1] /= part.mass; partForce[2] /= part.mass;

		part.velocity[0] = ( part.velocity[0] + partForce[0] ) * cloud.frictionInverse;
		part.velocity[1] = ( part.velocity[1] + partForce[1] ) * cloud.frictionInverse;
		part.velocity[2] = ( part.velocity[2] + partForce[2] ) * cloud.frictionInverse;

		part.pos[0] += part.velocity[0] * dtSeconds;
		part.pos[1] += part.velocity[1] * dtSeconds;
		part.pos[2] += part.velocity[2] * dtSeconds;

		float toCamera[3] = { part.pos[0] - cameraPos[0], part.pos[1] - cameraPos[1], part.pos[2] - cameraPos[2] };
		bool outside = VK_WPointOutside( part.pos );
		bool inRange = VK_WInRange3( part.pos, range_mins, range_maxs );
		float towardCam = toCamera[0] * cameraForward[0] + toCamera[1] * cameraForward[1] + toCamera[2] * cameraForward[2];
		bool inView = outside && inRange && ( towardCam > 0.0f );

		if ( !inRange && !part.rendering )
		{
			part.velocity[0] = part.velocity[1] = part.velocity[2] = 0.0f;
			if ( useSpawnPlane )
			{
				part.pos[0] = cameraPos[0] - spawnPlaneNorm[0] * cloud.spawnPlaneDistance
					+ spawnPlaneRight[0] * VK_WFloatRand( -cloud.spawnPlaneSize, cloud.spawnPlaneSize )
					+ spawnPlaneUp[0] * VK_WFloatRand( -cloud.spawnPlaneSize, cloud.spawnPlaneSize );
				part.pos[1] = cameraPos[1] - spawnPlaneNorm[1] * cloud.spawnPlaneDistance
					+ spawnPlaneRight[1] * VK_WFloatRand( -cloud.spawnPlaneSize, cloud.spawnPlaneSize )
					+ spawnPlaneUp[1] * VK_WFloatRand( -cloud.spawnPlaneSize, cloud.spawnPlaneSize );
				part.pos[2] = cameraPos[2] - spawnPlaneNorm[2] * cloud.spawnPlaneDistance
					+ spawnPlaneRight[2] * VK_WFloatRand( -cloud.spawnPlaneSize, cloud.spawnPlaneSize )
					+ spawnPlaneUp[2] * VK_WFloatRand( -cloud.spawnPlaneSize, cloud.spawnPlaneSize );
			}
			else
			{
				VK_WWrapRange( part.pos, range_mins, range_maxs );
			}
			inRange = true;
		}

		if ( part.rendering && !inView )
		{
			part.fadeIn = false;
			part.fadeOut = true;
		}
		else if ( part.rendering && inView && part.fadeOut )
		{
			part.fadeIn = true;
			part.fadeOut = false;
		}
		else if ( !part.rendering && inView )
		{
			part.rendering = true;
			part.alpha = 0.0f;
			part.fadeIn = true;
			part.fadeOut = false;
		}

		if ( part.rendering )
		{
			if ( part.fadeOut )
			{
				part.alpha -= particleFade;
				if ( part.alpha <= 0.0f )
				{
					part.alpha = 0.0f;
					part.fadeOut = false;
					part.fadeIn = false;
					part.rendering = false;
				}
			}
			else if ( part.fadeIn )
			{
				part.alpha += particleFade;
				if ( part.alpha >= cloud.color[3] )
				{
					part.fadeIn = false;
					part.alpha = cloud.color[3];
				}
			}
		}
	}
	cloud.populated = true;
}

// Exact port of rd-vanilla's real CParticleCloud::Render's vertex math
// (tr_WorldEffects.cpp) - writes one quad (6 vertices, two triangles) per
// FLAG_RENDER particle into vk.weatherVertexBufferMapped at *cursor,
// stopping early (not writing partial data) if the buffer is full. Returns
// the number of quads actually written, so the caller only issues a draw
// call when there's something to draw.
static uint32_t VK_EmitParticleCloud( VulkanParticleCloud &cloud, float dtSeconds,
	const float camLeftAxis[3], const float camUpAxis[3], uint32_t *cursor )
{
	// mCameraLeft/mCameraDown (rd-vanilla's naming) start as the *unit* view
	// axes here, scaled by width/height below - matches real Update()'s own
	// order (rotation, if any, is applied to the unit axes before scaling).
	float camLeft[3] = { camLeftAxis[0], camLeftAxis[1], camLeftAxis[2] };
	float camUp[3] = { camUpAxis[0], camUpAxis[1], camUpAxis[2] };

	if ( cloud.rotationChangeNextMs != -1.0f )
	{
		if ( cloud.rotationChangeNextMs <= 0.0f )
		{
			cloud.rotationDeltaTarget = VK_WFloatRand( cloud.rotationMin, cloud.rotationMax );
			cloud.rotationChangeNextMs = (float)VK_WIntRand( cloud.rotationChangeTimerMinMs, cloud.rotationChangeTimerMaxMs );
		}
		cloud.rotationChangeNextMs -= dtSeconds * 1000.0f;

		float diff = cloud.rotationDeltaTarget - cloud.rotationDelta;
		if ( fabsf( diff ) > 0.01f )
		{
			cloud.rotationDelta += diff; // snaps to target - see this file's comment on the real algorithm
		}
		cloud.rotationCurrent += cloud.rotationDelta * dtSeconds;
		float s = sinf( cloud.rotationCurrent );
		float c = cosf( cloud.rotationCurrent );

		float tempLeft[3] = { camLeft[0], camLeft[1], camLeft[2] };
		camLeft[0] = camLeft[0] * ( c * cloud.width ) - camUp[0] * ( s * cloud.width );
		camLeft[1] = camLeft[1] * ( c * cloud.width ) - camUp[1] * ( s * cloud.width );
		camLeft[2] = camLeft[2] * ( c * cloud.width ) - camUp[2] * ( s * cloud.width );

		camUp[0] = camUp[0] * ( c * cloud.height ) + tempLeft[0] * ( s * cloud.height );
		camUp[1] = camUp[1] * ( c * cloud.height ) + tempLeft[1] * ( s * cloud.height );
		camUp[2] = camUp[2] * ( c * cloud.height ) + tempLeft[2] * ( s * cloud.height );
	}
	else
	{
		camLeft[0] *= cloud.width; camLeft[1] *= cloud.width; camLeft[2] *= cloud.width;
		camUp[0] *= cloud.height; camUp[1] *= cloud.height; camUp[2] *= cloud.height;
	}

	float leftPlusUp[3] = { camLeft[0] - camUp[0], camLeft[1] - camUp[1], camLeft[2] - camUp[2] };
	float leftMinusUp[3] = { camLeft[0] + camUp[0], camLeft[1] + camUp[1], camLeft[2] + camUp[2] };

	uint32_t written = 0;
	for ( WeatherParticle &part : cloud.particles )
	{
		if ( !part.rendering )
		{
			continue;
		}
		if ( *cursor + 6 > WEATHER_VERTEX_BUFFER_CAPACITY )
		{
			break;
		}

		const float *useLeftPlusUp = leftPlusUp;
		const float *useLeftMinusUp = leftMinusUp;
		float orientedLeftPlusUp[3], orientedLeftMinusUp[3];
		if ( cloud.orientWithVelocity )
		{
			float dir[3] = { part.velocity[0], part.velocity[1], part.velocity[2] };
			VK_WVectorNormalize( dir );
			float thisUp[3] = { dir[0] * ( -cloud.height ), dir[1] * ( -cloud.height ), dir[2] * ( -cloud.height ) };
			orientedLeftPlusUp[0] = camLeft[0] - thisUp[0];
			orientedLeftPlusUp[1] = camLeft[1] - thisUp[1];
			orientedLeftPlusUp[2] = camLeft[2] - thisUp[2];
			orientedLeftMinusUp[0] = camLeft[0] + thisUp[0];
			orientedLeftMinusUp[1] = camLeft[1] + thisUp[1];
			orientedLeftMinusUp[2] = camLeft[2] + thisUp[2];
			useLeftPlusUp = orientedLeftPlusUp;
			useLeftMinusUp = orientedLeftMinusUp;
		}

		float vcolor[4];
		if ( cloud.blendMode == 0 )
		{
			vcolor[0] = cloud.color[0]; vcolor[1] = cloud.color[1]; vcolor[2] = cloud.color[2];
			vcolor[3] = part.alpha;
		}
		else
		{
			vcolor[0] = cloud.color[0] * part.alpha; vcolor[1] = cloud.color[1] * part.alpha; vcolor[2] = cloud.color[2] * part.alpha;
			vcolor[3] = cloud.color[3] * part.alpha;
		}

		float corners[4][3];
		for ( int i = 0; i < 3; i++ )
		{
			corners[0][i] = part.pos[i] - useLeftMinusUp[i]; // left bottom
			corners[1][i] = part.pos[i] - useLeftPlusUp[i];  // right bottom
			corners[2][i] = part.pos[i] + useLeftMinusUp[i]; // right top
			corners[3][i] = part.pos[i] + useLeftPlusUp[i];  // left top
		}
		static const float cornerUv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		static const int winding[6] = { 0, 1, 2, 0, 2, 3 };

		PolyVertex *out = (PolyVertex *)vk.weatherVertexBufferMapped + *cursor;
		for ( int i = 0; i < 6; i++ )
		{
			int c = winding[i];
			out[i].pos[0] = corners[c][0];
			out[i].pos[1] = corners[c][1];
			out[i].pos[2] = corners[c][2];
			out[i].uv[0] = cornerUv[c][0];
			out[i].uv[1] = cornerUv[c][1];
			out[i].color[0] = vcolor[0];
			out[i].color[1] = vcolor[1];
			out[i].color[2] = vcolor[2];
			out[i].color[3] = vcolor[3];
		}
		*cursor += 6;
		written++;
	}
	return written;
}

////////////////////////////////////////////////////////////////////////////
// Public entry points
////////////////////////////////////////////////////////////////////////////

void VK_InitWorldEffects( void )
{
	s_particleClouds.clear();
	s_windZones.clear();
	s_localWindZoneIndices.clear();
	s_frozen = false;
	s_outsideShake = false;
	s_outsidePain = 0.0f;
	s_fogColorTempActive = false;
	s_globalWindVelocity[0] = s_globalWindVelocity[1] = s_globalWindVelocity[2] = 0.0f;
	s_globalWindDirection[0] = 1.0f; s_globalWindDirection[1] = 0.0f; s_globalWindDirection[2] = 0.0f;
	s_globalWindSpeed = 0.0f;
	s_lastWeatherTimeMs = -1;
}

void VK_ShutdownWorldEffects( void )
{
	VK_InitWorldEffects();
}

void VK_AddWeatherZone( vec3_t mins, vec3_t maxs )
{
	// No-op - see this function's own declaration comment (tr_local.h).
	(void)mins; (void)maxs;
}

void VK_DrawWeatherEffects( const float *mvp, const refdef_t *fd )
{
	if ( s_particleClouds.empty() || !vk.frameActive )
	{
		return;
	}

	int dtMsInt = 0;
	if ( s_lastWeatherTimeMs >= 0 )
	{
		dtMsInt = fd->time - s_lastWeatherTimeMs;
	}
	s_lastWeatherTimeMs = fd->time;
	if ( dtMsInt < 1 ) dtMsInt = 1;
	if ( dtMsInt > 1000 ) dtMsInt = 1000;
	float dtMs = (float)dtMsInt;
	float dtSeconds = dtMs / 1000.0f;

	if ( !s_frozen )
	{
		s_globalWindVelocity[0] = s_globalWindVelocity[1] = s_globalWindVelocity[2] = 0.0f;
		for ( VulkanWindZone &wz : s_windZones )
		{
			wz.Update( dtMs );
			if ( wz.global )
			{
				s_globalWindVelocity[0] += wz.currentVelocity[0];
				s_globalWindVelocity[1] += wz.currentVelocity[1];
				s_globalWindVelocity[2] += wz.currentVelocity[2];
			}
		}
		s_globalWindDirection[0] = s_globalWindVelocity[0];
		s_globalWindDirection[1] = s_globalWindVelocity[1];
		s_globalWindDirection[2] = s_globalWindVelocity[2];
		s_globalWindSpeed = VK_WVectorNormalize( s_globalWindDirection );
	}

	VkCommandBuffer cmd = vk.activeCommandBuffer;

	uint32_t cursor = 0;
	int lastBlendMode = -1;
	image_t *lastImage = nullptr;
	uint32_t drawStart = 0;

	auto flushDraw = [&]( uint32_t drawEnd )
	{
		if ( drawEnd <= drawStart || !lastImage )
		{
			return;
		}
		VkPipeline pipeline = ( lastBlendMode == 0 ) ? vk.polyPipeline : vk.polyPipelineAdditive;
		if ( pipeline != vk.lastBoundPipeline )
		{
			vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
			vk.lastBoundPipeline = pipeline;
		}
		vkCmdPushConstants( cmd, vk.polyPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof( float ) * 16, mvp );
		vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.polyPipelineLayout,
			0, 1, &lastImage->descriptorSet, 0, nullptr );
		VkDeviceSize offset = (VkDeviceSize)drawStart * sizeof( PolyVertex );
		vkCmdBindVertexBuffers( cmd, 0, 1, &vk.weatherVertexBuffer, &offset );
		vkCmdDraw( cmd, drawEnd - drawStart, 1, 0, 0 );
	};

	for ( VulkanParticleCloud &cloud : s_particleClouds )
	{
		VK_UpdateParticleCloud( cloud, dtMs, dtSeconds, fd->vieworg, fd->viewaxis[0] );

		if ( cloud.image != lastImage || cloud.blendMode != lastBlendMode )
		{
			flushDraw( cursor );
			drawStart = cursor;
			lastImage = cloud.image;
			lastBlendMode = cloud.blendMode;
		}

		VK_EmitParticleCloud( cloud, dtSeconds, fd->viewaxis[1], fd->viewaxis[2], &cursor );
	}
	flushDraw( cursor );
}

// Parses and applies one weather command - exact token-for-token port of
// rd-vanilla's real R_WorldEffectCommand (tr_WorldEffects.cpp), including
// every preset's exact constants (particle counts, texture paths, gravity,
// width/height, colours, fade rates) - these are gameplay/visual tuning
// values, not something to approximate.
void VK_WorldEffectCommand( const char *command )
{
	if ( !command )
	{
		return;
	}

	COM_BeginParseSession();
	const char *token = COM_ParseExt( &command, qfalse );
	if ( !token[0] )
	{
		COM_EndParseSession();
		return;
	}

	if ( !Q_stricmp( token, "clear" ) )
	{
		s_particleClouds.clear();
		s_windZones.clear();
		s_localWindZoneIndices.clear();
	}
	else if ( !Q_stricmp( token, "freeze" ) )
	{
		s_frozen = !s_frozen;
	}
	else if ( !Q_stricmp( token, "zone" ) )
	{
		float mins[3], maxs[3];
		if ( VK_WParseVector( &command, mins ) && VK_WParseVector( &command, maxs ) )
		{
			VK_AddWeatherZone( mins, maxs );
		}
	}
	else if ( !Q_stricmp( token, "wind" ) )
	{
		if ( (int)s_windZones.size() < MAX_WIND_ZONES )
		{
			s_windZones.push_back( VulkanWindZone() );
		}
	}
	else if ( !Q_stricmp( token, "constantwind" ) )
	{
		if ( (int)s_windZones.size() < MAX_WIND_ZONES )
		{
			VulkanWindZone wz;
			if ( !VK_WParseVector( &command, wz.currentVelocity ) )
			{
				wz.currentVelocity[0] = 0.0f; wz.currentVelocity[1] = 800.0f; wz.currentVelocity[2] = 0.0f;
			}
			wz.targetVelocityTimeRemainingMs = -1.0f;
			s_windZones.push_back( wz );
		}
	}
	else if ( !Q_stricmp( token, "gustingwind" ) )
	{
		if ( (int)s_windZones.size() < MAX_WIND_ZONES )
		{
			VulkanWindZone wz;
			wz.velocityRangeMin[0] = -3000.0f; wz.velocityRangeMin[1] = -3000.0f; wz.velocityRangeMin[2] = -100.0f;
			wz.velocityRangeMax[0] = 3000.0f; wz.velocityRangeMax[1] = 3000.0f; wz.velocityRangeMax[2] = 100.0f;
			wz.durationMinMs = 1000; wz.durationMaxMs = 3000;
			wz.chanceOfDeadTime = 0.5f;
			wz.deadTimeMinMs = 2000; wz.deadTimeMaxMs = 4000;
			s_windZones.push_back( wz );
		}
	}
	else if ( !Q_stricmp( token, "windzone" ) )
	{
		if ( (int)s_windZones.size() < MAX_WIND_ZONES )
		{
			VulkanWindZone wz;
			wz.global = false;
			if ( !VK_WParseVector( &command, wz.boundsMin ) || !VK_WParseVector( &command, wz.boundsMax ) )
			{
				COM_EndParseSession();
				return;
			}
			if ( !VK_WParseVector( &command, wz.currentVelocity ) )
			{
				wz.currentVelocity[0] = 0.0f; wz.currentVelocity[1] = 800.0f; wz.currentVelocity[2] = 0.0f;
			}
			wz.targetVelocityTimeRemainingMs = -1.0f;
			s_windZones.push_back( wz );
			s_localWindZoneIndices.push_back( (int)s_windZones.size() - 1 );
		}
	}
	else if ( !Q_stricmp( token, "lightrain" ) )
	{
		if ( (int)s_particleClouds.size() < MAX_PARTICLE_CLOUDS )
		{
			VulkanParticleCloud c;
			c.Initialize( 500, "gfx/world/rain.jpg" );
			c.height = 80.0f; c.width = 1.2f; c.gravity = 2000.0f;
			c.blendMode = 1; c.fade = 100.0f;
			c.color[0] = c.color[1] = c.color[2] = c.color[3] = 0.5f;
			c.orientWithVelocity = true; c.waterParticles = true;
			s_particleClouds.push_back( std::move( c ) );
		}
	}
	else if ( !Q_stricmp( token, "rain" ) )
	{
		if ( (int)s_particleClouds.size() < MAX_PARTICLE_CLOUDS )
		{
			VulkanParticleCloud c;
			c.Initialize( 1000, "gfx/world/rain.jpg" );
			c.height = 80.0f; c.width = 1.2f; c.gravity = 2000.0f;
			c.blendMode = 1; c.fade = 100.0f;
			c.color[0] = c.color[1] = c.color[2] = c.color[3] = 0.5f;
			c.orientWithVelocity = true; c.waterParticles = true;
			s_particleClouds.push_back( std::move( c ) );
		}
	}
	else if ( !Q_stricmp( token, "acidrain" ) )
	{
		if ( (int)s_particleClouds.size() < MAX_PARTICLE_CLOUDS )
		{
			VulkanParticleCloud c;
			c.Initialize( 1000, "gfx/world/rain.jpg" );
			c.height = 80.0f; c.width = 2.0f; c.gravity = 2000.0f;
			c.blendMode = 1; c.fade = 100.0f;
			c.color[0] = 0.34f; c.color[1] = 0.70f; c.color[2] = 0.34f; c.color[3] = 0.70f;
			c.orientWithVelocity = true; c.waterParticles = true;
			s_particleClouds.push_back( std::move( c ) );
			s_outsidePain = 0.1f;
		}
	}
	else if ( !Q_stricmp( token, "heavyrain" ) )
	{
		if ( (int)s_particleClouds.size() < MAX_PARTICLE_CLOUDS )
		{
			VulkanParticleCloud c;
			c.Initialize( 1000, "gfx/world/rain.jpg" );
			c.height = 80.0f; c.width = 1.2f; c.gravity = 2800.0f;
			c.blendMode = 1; c.fade = 15.0f;
			c.color[0] = c.color[1] = c.color[2] = c.color[3] = 0.5f;
			c.orientWithVelocity = true; c.waterParticles = true;
			s_particleClouds.push_back( std::move( c ) );
		}
	}
	else if ( !Q_stricmp( token, "snow" ) )
	{
		if ( (int)s_particleClouds.size() < MAX_PARTICLE_CLOUDS )
		{
			VulkanParticleCloud c;
			c.Initialize( 1000, "gfx/effects/snowflake1.bmp" );
			c.blendMode = 1;
			c.rotationChangeNextMs = 0.0f;
			c.color[0] = c.color[1] = c.color[2] = c.color[3] = 0.75f;
			c.waterParticles = true;
			s_particleClouds.push_back( std::move( c ) );
		}
	}
	else if ( !Q_stricmp( token, "spacedust" ) )
	{
		token = COM_ParseExt( &command, qfalse );
		int count = atoi( token );
		if ( (int)s_particleClouds.size() < MAX_PARTICLE_CLOUDS )
		{
			VulkanParticleCloud c;
			c.Initialize( count, "gfx/effects/snowpuff1.tga" );
			c.height = 1.2f; c.width = 1.2f; c.gravity = 0.0f;
			c.blendMode = 1;
			c.rotationChangeNextMs = 0.0f;
			c.color[0] = c.color[1] = c.color[2] = c.color[3] = 0.75f;
			c.waterParticles = true;
			c.massMin = 10.0f; c.massMax = 30.0f;
			c.spawnRangeMin[0] = c.spawnRangeMin[1] = c.spawnRangeMin[2] = -1500.0f;
			c.spawnRangeMax[0] = c.spawnRangeMax[1] = c.spawnRangeMax[2] = 1500.0f;
			s_particleClouds.push_back( std::move( c ) );
		}
	}
	else if ( !Q_stricmp( token, "sand" ) )
	{
		if ( (int)s_particleClouds.size() < MAX_PARTICLE_CLOUDS )
		{
			VulkanParticleCloud c;
			c.Initialize( 400, "gfx/effects/alpha_smoke2b.tga" );
			c.gravity = 0.0f; c.width = 70.0f; c.height = 70.0f;
			c.color[0] = 0.9f; c.color[1] = 0.6f; c.color[2] = 0.0f; c.color[3] = 0.5f;
			c.fade = 5.0f;
			c.massMin = 10.0f; c.massMax = 30.0f;
			c.spawnRangeMin[2] = -150.0f; c.spawnRangeMax[2] = 150.0f;
			c.rotationChangeNextMs = 0.0f;
			s_particleClouds.push_back( std::move( c ) );
		}
	}
	else if ( !Q_stricmp( token, "fog" ) )
	{
		if ( (int)s_particleClouds.size() < MAX_PARTICLE_CLOUDS )
		{
			VulkanParticleCloud c;
			c.Initialize( 60, "gfx/effects/alpha_smoke2b.tga" );
			c.blendMode = 1;
			c.gravity = 0.0f; c.width = 70.0f; c.height = 70.0f;
			c.color[0] = c.color[1] = c.color[2] = c.color[3] = 0.2f;
			c.fade = 5.0f;
			c.massMin = 10.0f; c.massMax = 30.0f;
			c.spawnRangeMin[2] = -150.0f; c.spawnRangeMax[2] = 150.0f;
			c.rotationChangeNextMs = 0.0f;
			s_particleClouds.push_back( std::move( c ) );
		}
	}
	else if ( !Q_stricmp( token, "heavyrainfog" ) )
	{
		if ( (int)s_particleClouds.size() < MAX_PARTICLE_CLOUDS )
		{
			VulkanParticleCloud c;
			c.Initialize( 70, "gfx/effects/alpha_smoke2b.tga" );
			c.blendMode = 1;
			c.gravity = 0.0f; c.width = 100.0f; c.height = 100.0f;
			c.color[0] = c.color[1] = c.color[2] = c.color[3] = 0.3f;
			c.fade = 1.0f;
			c.massMin = 5.0f; c.massMax = 10.0f;
			c.spawnRangeMin[0] = c.spawnRangeMin[1] = -( c.spawnPlaneDistance * 1.25f );
			c.spawnRangeMax[0] = c.spawnRangeMax[1] = ( c.spawnPlaneDistance * 1.25f );
			c.spawnRangeMin[2] = -150.0f; c.spawnRangeMax[2] = 150.0f;
			c.rotationChangeNextMs = 0.0f;
			s_particleClouds.push_back( std::move( c ) );
		}
	}
	else if ( !Q_stricmp( token, "light_fog" ) )
	{
		if ( (int)s_particleClouds.size() < MAX_PARTICLE_CLOUDS )
		{
			VulkanParticleCloud c;
			c.Initialize( 40, "gfx/effects/alpha_smoke2b.tga" );
			c.blendMode = 1;
			c.gravity = 0.0f; c.width = 100.0f; c.height = 100.0f;
			c.color[0] = 0.19f; c.color[1] = 0.6f; c.color[2] = 0.7f; c.color[3] = 0.12f;
			c.fade = 0.10f;
			c.massMin = 10.0f; c.massMax = 30.0f;
			c.spawnRangeMin[2] = -150.0f; c.spawnRangeMax[2] = 150.0f;
			c.rotationChangeNextMs = 0.0f;
			s_particleClouds.push_back( std::move( c ) );
		}
	}
	else if ( !Q_stricmp( token, "outsideshake" ) )
	{
		s_outsideShake = !s_outsideShake;
	}
	else if ( !Q_stricmp( token, "outsidepain" ) )
	{
		s_outsidePain = ( s_outsidePain != 0.0f ) ? 0.0f : 1.0f;
	}
	else
	{
		ri.Printf( PRINT_WARNING, "rd-vulkan: weather: unrecognized command '%s'\n", token );
	}
	COM_EndParseSession();
}

bool VK_GetWindVector( vec3_t windVector, vec3_t atPoint )
{
	(void)atPoint;
	VectorCopy( s_globalWindDirection, windVector );
	return true;
}

bool VK_GetWindGusting( vec3_t atPoint )
{
	(void)atPoint;
	return s_globalWindSpeed > 300.0f;
}

bool VK_IsOutside( vec3_t pos )
{
	return VK_WPointOutside( pos );
}

float VK_IsOutsideCausingPain( vec3_t pos )
{
	if ( s_outsidePain != 0.0f && VK_WPointOutside( pos ) )
	{
		return s_outsidePain;
	}
	return 0.0f;
}

float VK_GetChanceOfSaberFizz( void )
{
	float chance = 0.0f;
	int numWater = 0;
	for ( const VulkanParticleCloud &c : s_particleClouds )
	{
		if ( c.waterParticles )
		{
			chance += ( c.gravity / 20000.0f );
			numWater++;
		}
	}
	return numWater ? ( chance / numWater ) : 0.0f;
}

bool VK_IsShaking( vec3_t pos )
{
	return s_outsideShake && VK_WPointOutside( pos );
}

bool VK_SetTempGlobalFogColor( vec3_t color )
{
	if ( !VK_HasWorldFog() )
	{
		return true;
	}
	if ( color[0] || color[1] || color[2] )
	{
		if ( !s_fogColorTempActive )
		{
			VK_GetWorldFogColor( s_fogColorSaved );
			s_fogColorTempActive = true;
		}
		VK_SetWorldFogColor( color );
	}
	else if ( s_fogColorTempActive )
	{
		s_fogColorTempActive = false;
		VK_SetWorldFogColor( s_fogColorSaved );
	}
	return true;
}
