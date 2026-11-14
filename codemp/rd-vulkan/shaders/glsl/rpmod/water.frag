#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: water (r_water). The map's water surfaces (by material or content), drawn alone into a
// depth image of their own, get what JKA's flat scrolling textures lack:
//   ripples      a moving, slightly choppy surface, worked out in the world so it stays put as you move
//   refraction   what's under the water bent by the ripples, more where it's deep
//   depth        deep water darker and tinted, the floor fading out under it
//   reflections  the view mirrored along the rippled surface, more at a glancing look (Fresnel)
// Water that slopes or falls (a slide, a waterfall) flows instead: what's seen through it wavers,
// running downhill, and streaks of light run down it.

#include "view.glsl"

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;	// the view so far
layout(set = 1, binding = 0) uniform sampler2D distanceTexture;	// the solid surfaces' distances
layout(set = 2, binding = 0) uniform sampler2D waterDepth;		// the water surfaces' depth, 1 where none

// rect: the view's pixels; a: tan half fov across and down, strength, far plane
// right, down, fwd: the camera's axes in the world, the camera's x, y, z in w
// b: time in seconds, near plane, ripple size, unused
layout(push_constant) uniform Params {
	vec4 rect;
	vec4 a;
	vec4 right;
	vec4 down;
	vec4 fwd;
	vec4 b;
} pc;

layout(location = 0) out vec4 out_color;

const int STEPS = 24;
const int REFINE = 4;

float Dist( vec2 p )
{
	return texelFetch( distanceTexture, ivec2( ClampToView( p, pc.rect ) ), 0 ).r;
}

vec3 Source( vec2 p )
{
	return texelFetch( sourceTexture, ivec2( ClampToView( p, pc.rect ) ), 0 ).rgb;
}

// the water surface's distance at a pixel, 0 where there's none
float WaterDist( vec2 p )
{
	const float d = texelFetch( waterDepth, ivec2( ClampToView( p, pc.rect ) ), 0 ).r;
	const float zNear = pc.b.y, zFar = pc.a.w;

	if ( d >= 0.99999 )
		return 0.0;
	return zNear * zFar / ( zFar - d * ( zFar - zNear ) );
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

// the surface's height over a point of the world: a few waves of different ways and speeds
float Height( vec2 w, float t )
{
	const float s = 1.0 / max( 4.0, pc.b.z );
	float h = 0.0;

	h += sin( dot( w, vec2( 0.8, 0.6 ) ) * s * 6.0 + t * 1.3 ) * 0.5;
	h += sin( dot( w, vec2( -0.5, 0.9 ) ) * s * 9.0 + t * 1.9 ) * 0.3;
	h += sin( dot( w, vec2( 0.3, -0.95 ) ) * s * 15.0 - t * 2.6 ) * 0.15;
	h += sin( dot( w, vec2( -0.9, -0.4 ) ) * s * 23.0 + t * 3.4 ) * 0.08;
	return h;
}

float FlowHash( vec2 c )
{
	return fract( sin( dot( c, vec2( 127.1, 311.7 ) ) ) * 43758.5453 );
}

float FlowNoise( vec2 w )
{
	const vec2 i = floor( w ), f = fract( w );
	const vec2 u = f * f * ( 3.0 - 2.0 * f );

	return mix( mix( FlowHash( i ), FlowHash( i + vec2( 1.0, 0.0 ) ), u.x ), mix( FlowHash( i + vec2( 0.0, 1.0 ) ), FlowHash( i + vec2( 1.0, 1.0 ) ), u.x ), u.y );
}

// sloping or falling water at p, at the world point W
vec3 Flowing( vec2 p, vec3 here, vec3 W, float wz )
{
	const float t = pc.b.x;
	const float strength = clamp( pc.a.z, 0.0, 2.0 );
	// the world's own lines, not the surface's (a waterfall is made of many facets, each facing a
	// little differently): across along the ground, down by height
	const float u = ( W.x + W.y ) * 0.7071, v = -W.z;
	// the flow: noise stretched along it, running down
	const vec2 q1 = vec2( u / 20.0, v / 60.0 - t * 2.2 );
	const vec2 q2 = vec2( u / 11.0 + 3.7, v / 35.0 - t * 3.1 );
	const vec2 wobble = vec2( FlowNoise( q1 ) - 0.5, FlowNoise( q2 ) - 0.5 );
	const float px = ( pc.rect.w - pc.rect.y ) * 0.004 * strength;
	const vec2 q = p + wobble * px;
	vec3 c = here;

	if ( WaterDist( q ) > 0.0 && Dist( q ) > wz - 4.0 )
		c = mix( here, Source( q ), clamp( strength, 0.0, 1.0 ) );
	// streaks of light running down it
	const float streak = smoothstep( 0.62, 0.95, FlowNoise( vec2( u / 6.0, v / 90.0 - t * 2.8 ) ) );
	c += vec3( 0.10, 0.11, 0.12 ) * streak * strength;
	return c;
}

// its facing in the world, from the waves' slopes
vec3 Normal( vec2 w, float t )
{
	const float e = 2.0, k = 0.35;
	const float h = Height( w, t );
	const float hx = Height( w + vec2( e, 0.0 ), t ) - h;
	const float hy = Height( w + vec2( 0.0, e ), t ) - h;

	return normalize( vec3( -hx * k * 4.0 / e, -hy * k * 4.0 / e, 1.0 ) );
}

void main()
{
	const vec2 p = floor( gl_FragCoord.xy ) + 0.5;
	const vec3 here = Source( p );
	const float wz = WaterDist( p );

	out_color = vec4( here, 1.0 );
	if ( wz <= 0.0 )
		return;
	// water behind something solid isn't seen
	const float z = Dist( p );
	if ( wz > z + max( 2.0, z * 0.01 ) )
		return;

	const vec3 P = ViewPoint( p, wz, pc.rect, pc.a.xy );
	const vec3 W = ToWorld( P );
	const vec3 V = normalize( P );

	// flat water ripples and mirrors; water that slopes or falls flows
	const vec3 upCam = ToCamera( vec3( 0.0, 0.0, 1.0 ) );
	{
		// each way, the neighbour on the same surface (the nearer in depth, and water), a few pixels
		// off: a triangle's edge, the water's own edge or two layers of it fighting don't bend the facing
		const float o = 4.0;
		const float zr = WaterDist( p + vec2( o, 0.0 ) ), zl = WaterDist( p - vec2( o, 0.0 ) );
		const float zd = WaterDist( p + vec2( 0.0, o ) ), zu = WaterDist( p - vec2( 0.0, o ) );
		const bool useR = zl <= 0.0 || ( zr > 0.0 && abs( zr - wz ) <= abs( zl - wz ) );
		const bool useD = zu <= 0.0 || ( zd > 0.0 && abs( zd - wz ) <= abs( zu - wz ) );
		const vec3 dx = useR ? ViewPoint( p + vec2( o, 0.0 ), zr, pc.rect, pc.a.xy ) - P : P - ViewPoint( p - vec2( o, 0.0 ), zl, pc.rect, pc.a.xy );
		const vec3 dy = useD ? ViewPoint( p + vec2( 0.0, o ), zd, pc.rect, pc.a.xy ) - P : P - ViewPoint( p - vec2( 0.0, o ), zu, pc.rect, pc.a.xy );
		vec3 Nf = normalize( cross( dy, dx ) );
		if ( dot( Nf, P ) > 0.0 )
			Nf = -Nf;
		if ( dot( Nf, upCam ) < 0.7 ) {
			out_color = vec4( Flowing( p, here, W, wz ), 1.0 );
			return;
		}
	}

	const float t = pc.b.x;
	const vec3 Nw = Normal( W.xy, t );
	vec3 N = ToCamera( Nw );
	if ( dot( N, P ) > 0.0 )
		N = -N;

	// refraction: what's under it, bent by the ripples, more where it's deep (but never what's in front)
	const float depth = max( 0.0, z - wz );
	const float bend = min( 1.0, depth / 64.0 ) * ( pc.rect.w - pc.rect.y ) * 0.02;
	vec2 q = p + Nw.xy * bend;
	vec3 under = here;
	if ( WaterDist( q ) > 0.0 && Dist( q ) > wz )
		under = Source( q );

	// the deeper, the darker and more tinted: a blue-green water, the floor fading out
	const float absorb = 1.0 - exp( -depth / 220.0 );
	under = mix( under, under * vec3( 0.55, 0.75, 0.8 ) + vec3( 0.01, 0.03, 0.04 ), absorb );

	// reflection along the rippled surface, steps that grow with the distance
	const vec3 R = reflect( V, N );
	const float cosT = clamp( dot( -V, N ), 0.0, 1.0 );
	const float fresnel = 0.02 + 0.98 * pow( 1.0 - cosT, 5.0 );
	float stepLen = max( 4.0, wz * 0.02 ) * ( 0.75 + 0.5 * PixelNoise( gl_FragCoord.xy ) );
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

	vec3 reflected;
	float reflectW;
	if ( hit ) {
		for ( int i = 0; i < REFINE; i++ ) {
			const vec3 M = ( prev + Q ) * 0.5;
			if ( M.z - Dist( Pixel( M ) ) > 0.0 )
				Q = M;
			else
				prev = M;
		}
		const vec2 s = Pixel( Q );
		const vec2 size = pc.rect.zw - pc.rect.xy;
		const vec2 edge = min( s - pc.rect.xy, pc.rect.zw - s ) / size;
		reflected = Source( s );
		reflectW = smoothstep( 0.0, 0.08, min( edge.x, edge.y ) ) * ( 1.0 - smoothstep( 800.0, 1600.0, length( Q - P ) ) );
	} else {
		// nothing found: what lies that way at the top of the view, likely sky, more faintly
		const vec2 s = clamp( Pixel( P + R * 4096.0 ), pc.rect.xy + 0.5, pc.rect.zw - 0.5 );
		reflected = Source( vec2( s.x, min( s.y, p.y ) ) );
		reflectW = 0.5;
	}

	const float strength = clamp( pc.a.z, 0.0, 2.0 );
	vec3 c = mix( here, under, clamp( strength, 0.0, 1.0 ) );
	c = mix( c, reflected, clamp( fresnel * reflectW * strength, 0.0, 0.9 ) );
	// a little light caught on the wave tops
	c += vec3( 0.05 ) * pow( max( 0.0, dot( reflect( V, N ), upCam ) ), 24.0 ) * strength;
	out_color = vec4( c, 1.0 );
}
