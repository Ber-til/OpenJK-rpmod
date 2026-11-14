#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: SMAA anti-aliasing over the finished view (r_smaa), in three passes, after SMAA 1x
// (smaa.glsl): sharper edges than FXAA, without its blur, and far cheaper than MSAA.
//   SMAA_EDGES     where the brightness steps: an edge to the left (r) or above (g)
//   SMAA_WEIGHTS   how much each edge pixel takes from its neighbours, from the edges' shapes
//                  (set 1: the area table, set 2: the search table)
//   (neither)      the view blended with its neighbours by those weights (set 1)

// metrics: 1 / width, 1 / height, width, height of the images
layout(push_constant) uniform Params {
	vec4 metrics;
} pc;

#define SMAA_GLSL_4
#define SMAA_PRESET_HIGH
#define SMAA_RT_METRICS pc.metrics
#include "smaa.glsl"

layout(set = 0, binding = 0) uniform sampler2D tex0;
#if !defined(SMAA_EDGES)
layout(set = 1, binding = 0) uniform sampler2D tex1;
#endif
#if defined(SMAA_WEIGHTS)
layout(set = 2, binding = 0) uniform sampler2D tex2;
#endif

layout(location = 0) out vec4 out_color;

#if defined(SMAA_EDGES)
// SMAALumaEdgeDetectionPS, with no edge written as nothing rather than discarded: the image isn't
// cleared before
vec2 Edges( vec2 texcoord, vec4 offset[3] )
{
	const vec3 weights = vec3( 0.2126, 0.7152, 0.0722 );
	const vec2 threshold = vec2( SMAA_THRESHOLD );
	const float L = dot( min( SMAASamplePoint( tex0, texcoord ).rgb, 1.0 ), weights );
	const float Lleft = dot( min( SMAASamplePoint( tex0, offset[0].xy ).rgb, 1.0 ), weights );
	const float Ltop = dot( min( SMAASamplePoint( tex0, offset[0].zw ).rgb, 1.0 ), weights );
	vec4 delta;

	delta.xy = abs( L - vec2( Lleft, Ltop ) );
	vec2 edges = step( threshold, delta.xy );
	if ( dot( edges, vec2( 1.0 ) ) == 0.0 )
		return vec2( 0.0 );

	const float Lright = dot( min( SMAASamplePoint( tex0, offset[1].xy ).rgb, 1.0 ), weights );
	const float Lbottom = dot( min( SMAASamplePoint( tex0, offset[1].zw ).rgb, 1.0 ), weights );
	delta.zw = abs( L - vec2( Lright, Lbottom ) );
	vec2 maxDelta = max( delta.xy, delta.zw );

	const float Lleftleft = dot( min( SMAASamplePoint( tex0, offset[2].xy ).rgb, 1.0 ), weights );
	const float Ltoptop = dot( min( SMAASamplePoint( tex0, offset[2].zw ).rgb, 1.0 ), weights );
	delta.zw = abs( vec2( Lleft, Ltop ) - vec2( Lleftleft, Ltoptop ) );
	maxDelta = max( maxDelta.xy, delta.zw );
	const float finalDelta = max( maxDelta.x, maxDelta.y );

	edges.xy *= step( finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy );
	return edges;
}
#endif

void main()
{
	const vec2 uv = gl_FragCoord.xy * pc.metrics.xy;

#if defined(SMAA_EDGES)
	vec4 offset[3];

	SMAAEdgeDetectionVS( uv, offset );
	out_color = vec4( Edges( uv, offset ), 0.0, 0.0 );
#elif defined(SMAA_WEIGHTS)
	vec2 pixcoord;
	vec4 offset[3];

	SMAABlendingWeightCalculationVS( uv, pixcoord, offset );
	out_color = SMAABlendingWeightCalculationPS( uv, pixcoord, offset, tex0, tex1, tex2, vec4( 0.0 ) );
#else
	vec4 offset;

	SMAANeighborhoodBlendingVS( uv, offset );
	out_color = SMAANeighborhoodBlendingPS( uv, offset, tex0, tex1 );
#endif
}
