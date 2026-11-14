#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: wet surfaces under rain (r_wetSurfaces). Inside a box of the map editor's rain, the
// ground darkens as it soaks, turns shiny, and puddles gather where it's flat: they mirror what's on
// screen, rings from the drops on them. How wet comes from how long it's rained there (and dries after).

#include "view.glsl"

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;	// the view so far
layout(set = 1, binding = 0) uniform sampler2D distanceTexture;

// rect: the view's pixels; a: tan half fov across and down, how wet 0-1, far plane
// right, down, fwd: the camera's axes in the world, the camera's x, y, z in w
// mins, maxs: the rain's box in the world; mins.w the time in seconds, maxs.w the strength
layout(push_constant) uniform Params {
	vec4 rect;
	vec4 a;
	vec4 right;
	vec4 down;
	vec4 fwd;
	vec4 mins;
	vec4 maxs;
} pc;

layout(location = 0) out vec4 out_color;

const int STEPS = 20;
const int REFINE = 4;

float Dist( vec2 p )
{
	return texelFetch( distanceTexture, ivec2( ClampToView( p, pc.rect ) ), 0 ).r;
}

vec3 Point( vec2 p )
{
	return ViewPoint( p, Dist( p ), pc.rect, pc.a.xy );
}

vec3 Source( vec2 p )
{
	return texelFetch( sourceTexture, ivec2( ClampToView( p, pc.rect ) ), 0 ).rgb;
}

vec2 Pixel( vec3 P )
{
	const vec2 ndc = P.xy / ( P.z * pc.a.xy );
	return pc.rect.xy + ( ndc * 0.5 + 0.5 ) * ( pc.rect.zw - pc.rect.xy );
}

vec3 ToWorld( vec3 P )
{
	return vec3( pc.right.w, pc.down.w, pc.fwd.w ) + pc.right.xyz * P.x + pc.down.xyz * P.y + pc.fwd.xyz * P.z;
}

vec3 ToCamera( vec3 v )
{
	return vec3( dot( v, pc.right.xyz ), dot( v, pc.down.xyz ), dot( v, pc.fwd.xyz ) );
}

float Hash( vec2 c )
{
	return fract( sin( dot( c, vec2( 127.1, 311.7 ) ) ) * 43758.5453 );
}

// smooth noise over the ground, for where puddles gather
float Noise( vec2 w )
{
	const vec2 i = floor( w ), f = fract( w );
	const vec2 u = f * f * ( 3.0 - 2.0 * f );

	return mix( mix( Hash( i ), Hash( i + vec2( 1.0, 0.0 ) ), u.x ), mix( Hash( i + vec2( 0.0, 1.0 ) ), Hash( i + vec2( 1.0, 1.0 ) ), u.x ), u.y );
}

// the rings of drops landing on a puddle: a slope to bend its mirror by
vec2 Rings( vec2 w, float t )
{
	vec2 bend = vec2( 0.0 );

	for ( int k = 0; k < 2; k++ ) {
		const vec2 cell = floor( w / 24.0 + k * 0.5 );
		const vec2 centre = ( cell + vec2( Hash( cell ), Hash( cell + 7.3 ) ) - k * 0.5 ) * 24.0;
		const float phase = fract( t * 0.9 + Hash( cell + 3.1 ) );
		const vec2 d = w - centre;
		const float r = length( d );
		const float ring = sin( ( r - phase * 12.0 ) * 1.2 ) * ( 1.0 - phase ) * smoothstep( 12.0, 0.0, abs( r - phase * 12.0 ) );

		if ( r > 0.001 )
			bend += d / r * ring;
	}
	return bend * 0.15;
}

void main()
{
	const vec2 p = floor( gl_FragCoord.xy ) + 0.5;
	const vec3 here = Source( p );
	const float z = Dist( p );

	out_color = vec4( here, 1.0 );
	if ( z >= pc.a.w * 0.99 || pc.a.z <= 0.0 )
		return;

	const vec3 P = ViewPoint( p, z, pc.rect, pc.a.xy );
	const vec3 W = ToWorld( P );
	// in the rain's box, a little fade at its sides
	const vec3 inside = min( W - pc.mins.xyz, pc.maxs.xyz - W );
	if ( min( inside.x, min( inside.y, inside.z ) ) < 0.0 )
		return;
	const float boxW = smoothstep( 0.0, 32.0, min( inside.x, inside.y ) );

	// facing up: the ground, not the walls
	vec3 dx1 = Point( p + vec2( 1.0, 0.0 ) ) - P, dx0 = P - Point( p - vec2( 1.0, 0.0 ) );
	vec3 dy1 = Point( p + vec2( 0.0, 1.0 ) ) - P, dy0 = P - Point( p - vec2( 0.0, 1.0 ) );
	const vec3 dx = abs( dx1.z ) < abs( dx0.z ) ? dx1 : dx0;
	const vec3 dy = abs( dy1.z ) < abs( dy0.z ) ? dy1 : dy0;
	vec3 N = normalize( cross( dy, dx ) );
	if ( dot( N, P ) > 0.0 )
		N = -N;
	const vec3 upCam = ToCamera( vec3( 0.0, 0.0, 1.0 ) );
	const float facing = dot( N, upCam );
	const float wet = pc.a.z * boxW;

	// soaked: darker, more so on the ground, a little on the walls
	vec3 c = here * mix( 1.0, 0.68, wet * mix( 0.35, 1.0, smoothstep( 0.3, 0.8, facing ) ) );
	const float groundW = smoothstep( 0.75, 0.95, facing );
	if ( groundW <= 0.0 ) {
		out_color = vec4( c, 1.0 );
		return;
	}

	// puddles where the noise is low, more of them the wetter it is; the rest a damp sheen
	const float n = Noise( W.xy / 160.0 ) * 0.7 + Noise( W.xy / 48.0 ) * 0.3;
	const float puddle = smoothstep( 0.62 - 0.3 * wet, 0.5 - 0.3 * wet, n ) * wet;
	const float t = pc.mins.w;
	vec3 Nr = N;
	if ( puddle > 0.0 )
		Nr = normalize( N + ToCamera( vec3( Rings( W.xy, t ), 0.0 ) ) * puddle );

	const vec3 V = normalize( P );
	const vec3 R = reflect( V, Nr );
	const float cosT = clamp( dot( -V, Nr ), 0.0, 1.0 );
	const float fresnel = 0.04 + 0.96 * pow( 1.0 - cosT, 5.0 );
	float stepLen = max( 4.0, z * 0.02 ) * ( 0.75 + 0.5 * PixelNoise( gl_FragCoord.xy ) );
	vec3 Q = P + N * 1.0, prev = Q;
	bool hit = false;

	for ( int i = 0; i < STEPS; i++ ) {
		prev = Q;
		Q += R * stepLen;
		stepLen *= 1.2;
		if ( Q.z <= 1.0 )
			break;
		const vec2 s = Pixel( Q );
		if ( any( lessThan( s, pc.rect.xy ) ) || any( greaterThan( s, pc.rect.zw ) ) )
			break;
		const float behind = Q.z - Dist( s );
		if ( behind > 0.0 && behind < max( 24.0, stepLen * 2.0 ) ) {
			hit = true;
			break;
		}
	}
	if ( hit ) {
		for ( int i = 0; i < REFINE; i++ ) {
			const vec3 M = ( prev + Q ) * 0.5;
			if ( M.z - Dist( Pixel( M ) ) > 0.0 )
				Q = M;
			else
				prev = M;
		}
		const vec2 s = Pixel( Q );
		if ( Dist( s ) < pc.a.w * 0.99 ) {
			const vec2 size = pc.rect.zw - pc.rect.xy;
			const vec2 edge = min( s - pc.rect.xy, pc.rect.zw - s ) / size;
			const float edgeW = smoothstep( 0.0, 0.08, min( edge.x, edge.y ) );
			const float farW = 1.0 - smoothstep( 600.0, 1200.0, length( Q - P ) );
			// a puddle mirrors, wet ground only shines a little
			const float shine = mix( 0.25 * wet, 0.9, puddle );
			const float w = clamp( pc.maxs.w * shine * fresnel * edgeW * farW * groundW, 0.0, 0.85 );
			c = mix( c, Source( s ), w );
		}
	}
	out_color = vec4( c, 1.0 );
}
