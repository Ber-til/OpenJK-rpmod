#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: FSR 1 upscaling (r_fsr), after AMD's FidelityFX Super Resolution 1.0 (ffx_fsr1.h): the
// picture drawn at a lower resolution, scaled up to the window with its edges kept sharp.
//   FSR_EASU   the upscaling, from the picture into an image the window's size
//   FSR_RCAS   then sharpened, at the window's size
// Both run just before the gamma pass, which then shows the result one to one.

// a: what's drawn of the picture (x, y) and its image's size (z, w); b: the window (x, y), the
// sharpening in stops, 0 the most (z)
layout(push_constant) uniform Params {
	vec4 a;
	vec4 b;
} pc;

layout(set = 0, binding = 0) uniform sampler2D src;

layout(location = 0) out vec4 out_color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

#if defined(FSR_EASU)
#define FSR_EASU_F 1
AF4 FsrEasuRF( AF2 p ) { return textureGather( src, p, 0 ); }
AF4 FsrEasuGF( AF2 p ) { return textureGather( src, p, 1 ); }
AF4 FsrEasuBF( AF2 p ) { return textureGather( src, p, 2 ); }
#else
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF( ASU2 p ) { return texelFetch( src, clamp( p, ASU2( 0 ), ASU2( textureSize( src, 0 ) ) - 1 ), 0 ); }
void FsrRcasInputF( inout AF1 r, inout AF1 g, inout AF1 b ) {}
#endif

#include "ffx_fsr1.h"

void main()
{
#if defined(FSR_EASU)
	// FsrEasuCon, worked out here
	const vec2 viewport = pc.a.xy, size = pc.a.zw, outSize = pc.b.xy;
	const AU4 con0 = AU4( floatBitsToUint( viewport.x / outSize.x ), floatBitsToUint( viewport.y / outSize.y ),
		floatBitsToUint( 0.5 * viewport.x / outSize.x - 0.5 ), floatBitsToUint( 0.5 * viewport.y / outSize.y - 0.5 ) );
	const AU4 con1 = AU4( floatBitsToUint( 1.0 / size.x ), floatBitsToUint( 1.0 / size.y ),
		floatBitsToUint( 1.0 / size.x ), floatBitsToUint( -1.0 / size.y ) );
	const AU4 con2 = AU4( floatBitsToUint( -1.0 / size.x ), floatBitsToUint( 2.0 / size.y ),
		floatBitsToUint( 1.0 / size.x ), floatBitsToUint( 2.0 / size.y ) );
	const AU4 con3 = AU4( floatBitsToUint( 0.0 ), floatBitsToUint( 4.0 / size.y ), 0u, 0u );
	AF3 c;

	FsrEasuF( c, AU2( gl_FragCoord.xy ), con0, con1, con2, con3 );
	out_color = vec4( c, 1.0 );
#else
	// FsrRcasCon: the sharpening from stops to linear
	const AU4 con = AU4( floatBitsToUint( exp2( -pc.b.z ) ), 0u, 0u, 0u );
	AF1 r, g, b;

	FsrRcasF( r, g, b, AU2( gl_FragCoord.xy ), con );
	out_color = vec4( r, g, b, 1.0 );
#endif
}
