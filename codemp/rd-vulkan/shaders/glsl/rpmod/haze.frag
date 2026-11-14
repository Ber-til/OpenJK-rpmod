#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: heat haze (r_heatHaze). Around what burns (fire, flames, explosions, thrusters, lava:
// effects found by their shader's name), the air shimmers: what's behind them wavers, more above them
// as heat rises, never what's in front of them. Up to six sources a pass.

#include "view.glsl"

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;	// the view so far
layout(set = 1, binding = 0) uniform sampler2D distanceTexture;

// rect: the view's pixels; a: time in seconds, strength in pixels, sources, unused
// src: each source on the screen: x, y, its radius in pixels, its distance
layout(push_constant) uniform Params {
	vec4 rect;
	vec4 a;
	vec4 src[6];
} pc;

layout(location = 0) out vec4 out_color;

float Hash( vec2 c )
{
	return fract( sin( dot( c, vec2( 127.1, 311.7 ) ) ) * 43758.5453 );
}

float Noise( vec2 w )
{
	const vec2 i = floor( w ), f = fract( w );
	const vec2 u = f * f * ( 3.0 - 2.0 * f );

	return mix( mix( Hash( i ), Hash( i + vec2( 1.0, 0.0 ) ), u.x ), mix( Hash( i + vec2( 0.0, 1.0 ) ), Hash( i + vec2( 1.0, 1.0 ) ), u.x ), u.y );
}

void main()
{
	const vec2 p = floor( gl_FragCoord.xy ) + 0.5;
	const float z = texelFetch( distanceTexture, ivec2( ClampToView( p, pc.rect ) ), 0 ).r;
	float weight = 0.0, scale = 1.0;

	for ( int i = 0; i < int( pc.a.z ) && i < 6; i++ ) {
		const vec4 s = pc.src[i];
		// the column of hot air: wider and longer above the source than below it
		vec2 d = ( p - s.xy ) / s.z;
		d.y = d.y < 0.0 ? d.y * 0.5 : d.y * 1.5;
		const float r = length( d );

		if ( r >= 1.0 || z < s.w - 48.0 )
			continue;	// outside it, or in front of it
		const float w = ( 1.0 - r ) * ( 1.0 - r );
		if ( w > weight ) {
			weight = w;
			scale = s.z;
		}
	}
	if ( weight <= 0.0 ) {
		out_color = vec4( texelFetch( sourceTexture, ivec2( p ), 0 ).rgb, 1.0 );
		return;
	}

	// the shimmer: noise rising through the column, its size after the source's
	const float t = pc.a.x;
	const vec2 q = p / max( 8.0, scale * 0.08 );
	const vec2 offset = vec2( Noise( q + vec2( 0.0, t * 2.5 ) ), Noise( q * 1.3 + vec2( 4.1, t * 3.1 ) ) ) - 0.5;
	const vec2 at = ClampToView( p + offset * 2.0 * pc.a.y * weight, pc.rect );

	out_color = vec4( texture( sourceTexture, at / vec2( textureSize( sourceTexture, 0 ) ) ).rgb, 1.0 );
}
