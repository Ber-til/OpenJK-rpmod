#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: the floors' reflections (ssr.frag's colour and strength) laid on the view, softened:
// each pixel's reflection mixed with its neighbours' within two pixels, those at about its own depth
// only (a reflection doesn't bleed past a chair onto the wall behind it), so its outline fades over a
// few pixels instead of stepping.

#include "view.glsl"

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;		// the view so far
layout(set = 1, binding = 0) uniform sampler2D reflectionTexture;	// rgb the reflection, a how strongly
layout(set = 2, binding = 0) uniform sampler2D distanceTexture;

// rect: the view's pixels
layout(push_constant) uniform Params {
	vec4 rect;
} pc;

layout(location = 0) out vec4 out_color;

void main()
{
	const vec2 p = floor( gl_FragCoord.xy ) + 0.5;
	const vec3 here = texelFetch( sourceTexture, ivec2( p ), 0 ).rgb;
	const float zc = texelFetch( distanceTexture, ivec2( p ), 0 ).r;
	const float near = max( 4.0, zc * 0.02 );
	vec3 sum = vec3( 0.0 );
	float strength = 0.0, total = 0.0;

	for ( int y = -2; y <= 2; y++ ) {
		for ( int x = -2; x <= 2; x++ ) {
			const ivec2 q = ivec2( ClampToView( p + vec2( x, y ), pc.rect ) );
			const float z = texelFetch( distanceTexture, q, 0 ).r;
			if ( abs( z - zc ) > near )
				continue;
			const vec4 r = texelFetch( reflectionTexture, q, 0 );
			const float k = exp( -float( x * x + y * y ) * 0.35 );
			sum += r.rgb * r.a * k;
			strength += r.a * k;
			total += k;
		}
	}
	if ( strength <= 0.0001 || total <= 0.0 ) {
		out_color = vec4( here, 1.0 );
		return;
	}
	out_color = vec4( mix( here, sum / strength, strength / total ), 1.0 );
}
