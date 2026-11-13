#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: real shadows (r_shadowMaps). The models were drawn from the light into the shadow
// map (their depth only, vk_rpmod_post.cpp); here each pixel of the view finds where its point is in
// that map, and is in shadow where something nearer the light was drawn there. Softened by a ring of
// taps turned by the pixel; multiplied into the view by the pipeline's blending.

#include "view.glsl"

layout(set = 0, binding = 0) uniform sampler2D distanceTexture;
layout(set = 1, binding = 0) uniform sampler2D shadowMap;

// rect: the view's pixels; a: tan half fov across and down, strength, far plane
// b: a texel of the map, softness in texels, the depth bias, unused
// m: the camera's space (x right, y down, z forward) to the light's (x, y to -1..1, depth 0..1)
layout(push_constant) uniform Params {
	vec4 rect;
	vec4 a;
	vec4 b;
	mat4 m;
} pc;

layout(location = 0) out vec4 out_color;

const int TAPS = 12;
const vec2 ring[TAPS] = vec2[](
	vec2( -0.326, -0.406 ), vec2( -0.840, -0.074 ), vec2( -0.696,  0.457 ), vec2( -0.203,  0.621 ),
	vec2(  0.962, -0.195 ), vec2(  0.473, -0.480 ), vec2(  0.519,  0.767 ), vec2(  0.185, -0.893 ),
	vec2(  0.507,  0.064 ), vec2(  0.896,  0.412 ), vec2( -0.322, -0.933 ), vec2( -0.792, -0.598 )
);

void main()
{
	const vec2 p = floor( gl_FragCoord.xy ) + 0.5;
	const float z = texelFetch( distanceTexture, ivec2( p ), 0 ).r;

	out_color = vec4( 1.0 );
	if ( z >= pc.a.w * 0.99 )
		return;		// the sky

	const vec4 c = pc.m * vec4( ViewPoint( p, z, pc.rect, pc.a.xy ), 1.0 );
	const vec2 uv = c.xy * 0.5 + 0.5;
	const float d = c.z - pc.b.z;

	if ( any( lessThan( uv, vec2( 0.0 ) ) ) || any( greaterThan( uv, vec2( 1.0 ) ) ) || d >= 1.0 || d <= 0.0 )
		return;		// outside the map

	// fading out toward the map's edges, so its end doesn't show as a line
	const vec2 edge = min( uv, 1.0 - uv );
	const float fade = smoothstep( 0.0, 0.08, min( edge.x, edge.y ) );

	const float turn = PixelNoise( gl_FragCoord.xy ) * 6.2831853;
	const mat2 rot = mat2( cos( turn ), sin( turn ), -sin( turn ), cos( turn ) );
	const float reach = pc.b.x * pc.b.y;
	float lit = 0.0;

	for ( int i = 0; i < TAPS; i++ ) {
		const float near = texture( shadowMap, uv + rot * ring[i] * reach ).r;
		lit += d <= near ? 1.0 : 0.0;
	}
	const float shade = ( 1.0 - lit / float( TAPS ) ) * fade;

	out_color = vec4( vec3( 1.0 - pc.a.z * shade ), 1.0 );
}
