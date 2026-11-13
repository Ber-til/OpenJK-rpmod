#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: screen-space reflections (r_ssr). Floors mirror what's on screen: from each floor
// pixel a ray goes out along the view reflected by the floor and steps through the distances until it
// passes behind something drawn; what's there is the reflection, sharpened by a few halvings.
//
// Which floors: those facing up (from the distances) whose surface is shiny by its material (marble,
// tiles, metal, glass...: the shiny surfaces' own depth, drawn alone, is what the pixel shows); more
// at a glancing look (Fresnel). It fades where the ray leaves the screen or finds nothing.

#include "view.glsl"

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;	// the view so far
layout(set = 1, binding = 0) uniform sampler2D distanceTexture;
layout(set = 2, binding = 0) uniform sampler2D shinyDepth;		// the shiny surfaces' depth, 1 where none

// rect: the view's pixels; a: tan half fov across and down, strength, far plane
// up: the world's up in the camera's space, the near plane
layout(push_constant) uniform Params {
	vec4 rect;
	vec4 a;
	vec4 up;
} pc;

layout(location = 0) out vec4 out_color;

const int STEPS = 32;
const int REFINE = 5;

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

void main()
{
	const vec2 p = floor( gl_FragCoord.xy ) + 0.5;
	const vec3 here = Source( p );
	const float z = Dist( p );

	out_color = vec4( here, 1.0 );
	if ( z >= pc.a.w * 0.99 )
		return;		// the sky

	const vec3 P = ViewPoint( p, z, pc.rect, pc.a.xy );

	// the surface's facing, the nearer neighbour on each side so edges don't bend it
	vec3 dx1 = Point( p + vec2( 1.0, 0.0 ) ) - P, dx0 = P - Point( p - vec2( 1.0, 0.0 ) );
	vec3 dy1 = Point( p + vec2( 0.0, 1.0 ) ) - P, dy0 = P - Point( p - vec2( 0.0, 1.0 ) );
	const vec3 dx = abs( dx1.z ) < abs( dx0.z ) ? dx1 : dx0;
	const vec3 dy = abs( dy1.z ) < abs( dy0.z ) ? dy1 : dy0;
	vec3 N = normalize( cross( dy, dx ) );
	if ( dot( N, P ) > 0.0 )
		N = -N;

	// floors only
	const float floorW = smoothstep( 0.75, 0.95, dot( N, pc.up.xyz ) );
	if ( floorW <= 0.0 )
		return;

	// shiny: a shiny surface is what this pixel shows (its depth there is the view's)
	const float sd = texelFetch( shinyDepth, ivec2( p ), 0 ).r;
	if ( sd >= 0.99999 )
		return;
	const float zNear = pc.up.w, zFar = pc.a.w;
	const float sz = zNear * zFar / ( zFar - sd * ( zFar - zNear ) );
	if ( abs( sz - z ) > max( 2.0, z * 0.01 ) )
		return;
	const float smoothW = 1.0;

	const vec3 V = normalize( P );
	const vec3 R = reflect( V, N );
	// a glancing look reflects more (Schlick, a little from straight on too)
	const float cosT = clamp( dot( -V, N ), 0.0, 1.0 );
	const float fresnel = 0.15 + 0.85 * pow( 1.0 - cosT, 5.0 );

	// steps that grow with the distance, a little turned by the pixel against banding
	float stepLen = max( 4.0, z * 0.02 ) * ( 0.75 + 0.5 * PixelNoise( gl_FragCoord.xy ) );
	vec3 Q = P + N * 1.0, prev = Q;
	bool hit = false;

	for ( int i = 0; i < STEPS; i++ ) {
		prev = Q;
		Q += R * stepLen;
		stepLen *= 1.18;
		if ( Q.z <= 1.0 )
			break;		// back past the camera
		const vec2 q = Pixel( Q );
		if ( any( lessThan( q, pc.rect.xy ) ) || any( greaterThan( q, pc.rect.zw ) ) )
			break;		// off the screen
		const float d = Dist( q );
		const float behind = Q.z - d;
		if ( behind > 0.0 && behind < max( 24.0, stepLen * 2.0 ) ) {
			hit = true;
			break;
		}
	}
	if ( !hit )
		return;

	// halved between the last point in front and the first behind
	for ( int i = 0; i < REFINE; i++ ) {
		const vec3 M = ( prev + Q ) * 0.5;
		if ( M.z - Dist( Pixel( M ) ) > 0.0 )
			Q = M;
		else
			prev = M;
	}
	const vec2 q = Pixel( Q );
	// nothing from the sky's own pixels counts as a hit on something
	if ( Dist( q ) >= pc.a.w * 0.99 )
		return;

	// fades: toward the screen's edges, with the ray's length
	const vec2 size = pc.rect.zw - pc.rect.xy;
	const vec2 edge = min( q - pc.rect.xy, pc.rect.zw - q ) / size;
	const float edgeW = smoothstep( 0.0, 0.08, min( edge.x, edge.y ) );
	const float farW = 1.0 - smoothstep( 600.0, 1200.0, length( Q - P ) );

	const float w = clamp( pc.a.z * floorW * smoothW * fresnel * edgeW * farW, 0.0, 0.85 );
	out_color = vec4( mix( here, Source( q ), w ), 1.0 );
}
