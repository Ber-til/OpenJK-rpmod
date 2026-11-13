#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: volumetric light (r_volumetric). Dynamic lights glowing in the air: for each pixel,
// the light each one sheds along the ray from the eye to what the pixel shows, added to the view by
// the pipeline's blending.
//
// Light in the air falls off as the square of the distance, softened at the light's middle so it
// never runs away: 1/(d^2 + k^2), k a part of the light's radius. Along a ray s*D, d^2 = t^2 + e with
// t = s - D.L and e the ray's nearest distance to the light squared, so the light shed is an
// arctangent, exact between where the ray enters the light's sphere and where it leaves it or meets
// the surface. No marching, no noise; the glow gathers round each light rather than filling its
// whole radius.

#include "view.glsl"

layout(set = 0, binding = 0) uniform sampler2D distTexture;

layout(push_constant) uniform Push {
	vec4 rect;			// the view's pixels, x0 y0 x1 y1
	vec4 a;				// tanHalf.xy, strength, how many lights
	vec4 light[3];		// view space x right y down z forward, radius
	vec4 color[3];		// rgb, unused
};

layout(location = 0) out vec4 out_color;

#define SOFT	0.12	// k, as a part of the radius
#define PI		3.14159265

void main()
{
	const vec2 p = gl_FragCoord.xy;
	const float z = texelFetch( distTexture, ivec2( p ), 0 ).r;
	const vec3 P = ViewPoint( p, z, rect, a.xy );
	const float T = length( P );
	const vec3 D = P / max( T, 0.001 );
	vec3 sum = vec3( 0.0 );

	for ( int i = 0; i < int( a.w ); i++ ) {
		const vec3 L = light[i].xyz;
		const float r = light[i].w, rr = r * r;
		const float b = dot( D, L ), c = dot( L, L );
		const float disc = rr - ( c - b * b );

		if ( disc <= 0.0 )
			continue;
		const float h = sqrt( disc );
		const float s0 = max( b - h, 0.0 ), s1 = min( b + h, T );

		if ( s1 <= s0 )
			continue;
		// k/PI: about 1 for a ray straight through the light's middle, whatever its radius
		const float k = SOFT * r;
		const float se = sqrt( max( c - b * b, 0.0 ) + k * k );
		sum += color[i].rgb * ( ( atan( ( s1 - b ) / se ) - atan( ( s0 - b ) / se ) ) / se * ( k / PI ) );
	}

	out_color = vec4( sum * a.z, 0.0 );
}
