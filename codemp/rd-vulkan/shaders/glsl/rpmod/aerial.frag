#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: what lies under the sky (vk_rpmod_post.cpp), where the view shows the world:
//   aerial haze    far things fade into the sky's colour, less high up (r_aerialHaze)
//   cloud shadows  the cloud layer's shadows on the ground, drifting with it, soft (r_cloudShadows);
//                  not under a roof (the sun map: the map traced from the sun)

#include "view.glsl"
#include "skycommon.glsl"

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;
layout(set = 1, binding = 0) uniform sampler2D distanceTexture;
layout(set = 2, binding = 0) uniform sampler2D sunMap;			// where the sun reaches

// rect: the view's pixels; a: tan half fov across and down, far plane, time in seconds
// right, down, fwd: the camera's axes in the world, its x, y, z in w
// sun: toward the sun in the world, w the haze's thickness
// b: cloud cover 0 to 1, the wind's drift x and y, the cloud shadows' strength
// c: rain 0 to 1, where the haze starts (world units), unused, unused
layout(push_constant) uniform Params {
	vec4 rect;
	vec4 a;
	vec4 right;
	vec4 down;
	vec4 fwd;
	vec4 sun;
	vec4 b;
	vec4 c;
} pc;

layout(location = 0) out vec4 out_color;

const float CLOUD_HEIGHT = 2600.0;

float Dist( vec2 p )
{
	return texelFetch( distanceTexture, ivec2( ClampToView( p, pc.rect ) ), 0 ).r;
}

vec3 ToWorld( vec3 v )
{
	return pc.right.xyz * v.x + pc.down.xyz * v.y + pc.fwd.xyz * v.z;
}

vec3 ToCamera( vec3 v )
{
	return vec3( dot( v, pc.right.xyz ), dot( v, pc.down.xyz ), dot( v, pc.fwd.xyz ) );
}

vec2 Pixel( vec3 P )
{
	const vec2 ndc = P.xy / ( P.z * pc.a.xy );
	return pc.rect.xy + ( ndc * 0.5 + 0.5 ) * ( pc.rect.zw - pc.rect.xy );
}

// the sun map (vk_rpmod_post.cpp RP_SunMap): row 0 its header, then how far the sun's light goes
// down through each texel. How much of the sun reaches W, 0 under a roof to 1 in the open, soft at
// the edges: four bilinear looks around it
float Header( int i )
{
	return texelFetch( sunMap, ivec2( i, 0 ), 0 ).r;
}

float OpenAt( ivec2 t, float d, float bias, int w, int h )
{
	if ( t.x < 0 || t.y < 0 || t.x >= w || t.y >= h )
		return 1.0;
	return d <= texelFetch( sunMap, ivec2( t.x, t.y + 1 ), 0 ).r + bias ? 1.0 : 0.0;
}

float SunReach( vec3 W, vec3 L )
{
	const float texel = Header( 2 );
	const int w = int( Header( 3 ) ), h = int( Header( 4 ) );
	const vec3 f = -L;
	const vec3 hint = abs( f.z ) > 0.99 ? vec3( 1.0, 0.0, 0.0 ) : vec3( 0.0, 0.0, 1.0 );
	const vec3 up = normalize( hint - dot( hint, f ) * f );
	const vec3 left = cross( up, f );
	const vec2 at = vec2( ( dot( W, left ) - Header( 0 ) ) / texel, ( dot( W, up ) - Header( 1 ) ) / texel ) - 0.5;
	const float d = dot( W, f );
	const float bias = texel * 0.75 + 24.0;
	float sum = 0.0;

	for ( int k = 0; k < 4; k++ ) {
		const vec2 q = at + vec2( ( k & 1 ) == 0 ? -0.5 : 0.5, ( k & 2 ) == 0 ? -0.5 : 0.5 );
		const ivec2 t = ivec2( floor( q ) );
		const vec2 fr = q - floor( q );

		sum += mix( mix( OpenAt( t, d, bias, w, h ), OpenAt( t + ivec2( 1, 0 ), d, bias, w, h ), fr.x ),
			mix( OpenAt( t + ivec2( 0, 1 ), d, bias, w, h ), OpenAt( t + ivec2( 1, 1 ), d, bias, w, h ), fr.x ), fr.y );
	}
	return sum * 0.25;
}

// the clouds' shadow: their broad shapes, softer than the clouds seen (fewer octaves, a wider edge)
float CloudShadow( vec2 w, float cover, vec2 wind )
{
	vec2 q = ( w + wind ) / 2600.0;
	q += vec2( SkyFbm( q * 0.7 + 3.1, 3 ), SkyFbm( q * 0.7 + 8.7, 3 ) ) * 0.9;
	const float n = SkyFbm( q, 3 );

	return smoothstep( 1.0 - cover * 0.9 - 0.25, 1.1 - cover * 0.75 + 0.1, n );
}

void main()
{
	const vec2 p = floor( gl_FragCoord.xy ) + 0.5;
	const float z = Dist( p );
	vec3 c = texelFetch( sourceTexture, ivec2( ClampToView( p, pc.rect ) ), 0 ).rgb;

	out_color = vec4( c, 1.0 );
	if ( z >= pc.a.z * 0.99 )
		return;		// the sky's own

	const vec3 P = ViewPoint( p, z, pc.rect, pc.a.xy );
	const vec3 cam = vec3( pc.right.w, pc.down.w, pc.fwd.w );
	const vec3 W = cam + ToWorld( P );
	const vec3 sun = normalize( pc.sun.xyz );

	// cloud shadows: the layer over the point toward the sun the light comes from (the sky composer's
	// Sun, else its hour, else the map's: the sun map's), by day, where the sun reaches
	if ( pc.b.x > 0.0 && pc.b.w > 0.0 ) {
		const vec3 L = normalize( vec3( Header( 5 ), Header( 6 ), Header( 7 ) ) );
		if ( L.z > 0.05 ) {
			const vec2 at = W.xy + L.xy * ( ( cam.z + CLOUD_HEIGHT - W.z ) / L.z );
			const float dens = CloudShadow( at, pc.b.x, pc.b.yz );
			if ( dens > 0.01 )
				c *= 1.0 - dens * pc.b.w * SkyDay( L ) * ( 1.0 - 0.5 * pc.c.x ) * SunReach( W, L );
		}
	}

	// haze: thicker the farther, thinner high up, the sky's colour that way
	if ( pc.sun.w > 0.0 ) {
		const float far = max( 0.0, length( P ) - pc.c.y );
		const float height = exp( -max( 0.0, W.z - cam.z + 256.0 ) / 3000.0 );
		const float amount = ( 1.0 - exp( -far * pc.sun.w / 10000.0 ) ) * height;
		const vec3 dir = normalize( ToWorld( P ) );
		const vec3 haze = SkyColor( normalize( vec3( dir.xy, max( dir.z, 0.02 ) ) ), sun ) + vec3( 0.03 ) * pc.c.x;
		c = mix( c, haze, clamp( amount, 0.0, 0.85 ) );
	}

	out_color = vec4( c, 1.0 );
}
