#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: what lies under the sky (vk_rpmod_post.cpp), where the view shows the world:
//   aerial haze    far things fade into the sky's colour, less high up (r_aerialHaze)
//   cloud shadows  the cloud layer's shadows on the ground, drifting with it (r_cloudShadows); not
//                  where something on screen stands between the point and the sun (under a roof)

#include "view.glsl"
#include "skycommon.glsl"

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;
layout(set = 1, binding = 0) uniform sampler2D distanceTexture;

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

// whether the way from P toward the sun is open as far as the screen shows: false under a roof. Only
// what's well above counts (a roof, an awning): a person, a crate, a lamp post standing near would cut
// their shape out of the cloud shadows, a bright patch beside them under thick clouds
const float ROOF_MIN = 160.0;

bool SunOpen( vec3 P, vec3 sunCam )
{
	vec3 Q = P + sunCam * 4.0;
	float stepLen = max( 8.0, P.z * 0.02 );

	for ( int i = 0; i < 16; i++ ) {
		Q += sunCam * stepLen;
		stepLen *= 1.35;
		if ( Q.z <= 1.0 )
			return true;
		const vec2 s = Pixel( Q );
		if ( any( lessThan( s, pc.rect.xy ) ) || any( greaterThan( s, pc.rect.zw ) ) )
			return true;		// off the screen: open, as far as can be told
		const float d = Dist( s );
		if ( d >= pc.a.z * 0.99 )
			continue;			// sky there
		if ( Q.z > d + 4.0 && Q.z < d + max( 64.0, stepLen * 2.0 ) && length( Q - P ) > ROOF_MIN )
			return false;
	}
	return true;
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

	// cloud shadows: the layer over the point toward the sun, by day, where the sun reaches
	if ( pc.b.x > 0.0 && pc.b.w > 0.0 && sun.z > 0.05 ) {
		const vec2 at = W.xy + sun.xy * ( ( cam.z + CLOUD_HEIGHT - W.z ) / sun.z );
		const float dens = CloudDensity( at, pc.b.x, pc.b.yz );
		if ( dens > 0.01 && SunOpen( P, normalize( ToCamera( sun ) ) ) )
			c *= 1.0 - dens * pc.b.w * SkyDay( sun ) * ( 1.0 - 0.5 * pc.c.x );
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
