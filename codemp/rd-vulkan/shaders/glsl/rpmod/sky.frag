#version 450
#extension GL_GOOGLE_include_directive : enable

// OpenJK-rpmod: the sky, taken further (vk_rpmod_post.cpp), where the view shows sky:
//   sharpening   the map's skybox, often a small picture blown up, crisper (r_skySharpen)
//   atmosphere   a sky of the sun's place in place of the map's (the composer's "sky composed"): blue by day,
//                red and orange at dusk, deep blue and stars at night; the time from the sky
//                composer, else the map's sun
//   objects      the sky composer's (/skycomposer): planets, gas giants, moons, suns, a battle
//                station, rings, nebulae, a fleet, all drawn here from their numbers
//   clouds       a layer of them over the world, drifting with the wind, lit by the sun, darker in
//                rain, lightning in them (r_clouds, or the composer's)

#include "view.glsl"
#include "skycommon.glsl"

layout(set = 0, binding = 0) uniform sampler2D sourceTexture;	// the view so far
layout(set = 1, binding = 0) uniform sampler2D distanceTexture;
layout(set = 2, binding = 0) uniform sampler2D objectTable;		// four texels an object

// rect: the view's pixels; a: tan half fov across and down, far plane, time in seconds
// right, down, fwd: the camera's axes in the world, its x, y, z in w
// sun: toward the sun in the world, w the atmosphere's blend 0 to 1
// b: cloud cover 0 to 1, the wind's drift x and y, how many objects
// c: sharpening, rain 0 to 1, lightning 0 to 1, stars 0 to 1
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

#define OBJ_PLANET		0
#define OBJ_GAS			1
#define OBJ_MOON		2
#define OBJ_SUN			3
#define OBJ_STATION		4
#define OBJ_NEBULA		5
#define OBJ_FLEET		6
#define OBJ_RINGED		7

const float CLOUD_HEIGHT = 2600.0;

vec3 Source( vec2 p )
{
	return texelFetch( sourceTexture, ivec2( ClampToView( p, pc.rect ) ), 0 ).rgb;
}

float Dist( vec2 p )
{
	return texelFetch( distanceTexture, ivec2( ClampToView( p, pc.rect ) ), 0 ).r;
}

bool IsSky( vec2 p )
{
	return Dist( p ) >= pc.a.z * 0.99;
}

vec3 ToWorld( vec3 v )
{
	return pc.right.xyz * v.x + pc.down.xyz * v.y + pc.fwd.xyz * v.z;
}

// a direction's own frame: right and up across it
void Frame( vec3 n, out vec3 r, out vec3 u )
{
	r = normalize( cross( abs( n.z ) < 0.95 ? vec3( 0.0, 0.0, 1.0 ) : vec3( 1.0, 0.0, 0.0 ), n ) );
	u = cross( n, r );
}

// the object: where (a direction), its size (radius in radians), colour, kind, seed, tilt, and two more
struct Obj {
	vec3 dir;
	float size;
	vec3 color;
	int kind;
	float seed;
	float tilt;
	float extra;
	float extra2;
};

Obj ObjectAt( int i )
{
	Obj o;
	const vec4 t0 = texelFetch( objectTable, ivec2( i * 4, 0 ), 0 );
	const vec4 t1 = texelFetch( objectTable, ivec2( i * 4 + 1, 0 ), 0 );
	const vec4 t2 = texelFetch( objectTable, ivec2( i * 4 + 2, 0 ), 0 );

	o.dir = normalize( t0.xyz );
	o.size = t0.w;
	o.color = t1.rgb;
	o.kind = int( t1.a + 0.5 );
	o.seed = t2.x;
	o.tilt = t2.y;
	o.extra = t2.z;
	o.extra2 = t2.w;
	return o;
}

// a lit sphere's surface, kind by kind: its colour at a point (n its facing, the sphere's own frame)
vec3 SphereSurface( Obj o, vec3 n )
{
	const vec2 uv = vec2( atan( n.x, n.z ), asin( clamp( n.y, -1.0, 1.0 ) ) );

	if ( o.kind == OBJ_GAS || o.kind == OBJ_RINGED ) {
		// bands, swirled a little
		const float band = n.y * 6.0 + SkyFbm( uv * vec2( 3.0, 12.0 ) + o.seed, 4 ) * 1.6;
		return o.color * ( 0.75 + 0.35 * sin( band * 3.0 ) ) * ( 0.85 + 0.3 * SkyNoise( vec2( band * 4.0, o.seed ) ) );
	}
	if ( o.kind == OBJ_MOON ) {
		// grey, cratered
		const float n1 = SkyFbm( uv * 6.0 + o.seed, 5 );
		const float craters = smoothstep( 0.62, 0.7, SkyNoise( uv * 14.0 + o.seed * 3.0 ) );
		return o.color * ( 0.55 + 0.6 * n1 ) * ( 1.0 - craters * 0.35 );
	}
	if ( o.kind == OBJ_STATION ) {
		// grey plates, the trench round the middle, the dish up north
		float shade = 0.55 + 0.25 * SkyNoise( floor( uv * vec2( 40.0, 30.0 ) ) + o.seed );
		shade *= 1.0 - 0.5 * smoothstep( 0.035, 0.0, abs( n.y ) );
		const vec3 dishDir = normalize( vec3( 0.45, 0.55, 0.7 ) );
		const float dish = dot( n, dishDir );
		if ( dish > 0.93 )
			shade *= 0.55 + 0.45 * smoothstep( 0.985, 0.93, dish );
		return o.color * shade;
	}
	// a planet: seas and land, a little cloud
	const float land = SkyFbm( uv * 2.5 + o.seed, 5 );
	vec3 c = land > 0.52 ? mix( o.color, o.color * vec3( 0.8, 0.75, 0.6 ) + vec3( 0.12, 0.1, 0.05 ), smoothstep( 0.52, 0.7, land ) )
		: o.color * vec3( 0.35, 0.5, 0.9 ) * 0.7;
	c = mix( c, vec3( 0.95 ), smoothstep( 0.6, 0.8, SkyFbm( uv * 4.0 + o.seed + 9.0, 4 ) ) * 0.6 );
	return c;
}

// the object over the sky behind it (bg), seen along d; the sky as it was where it isn't
vec3 DrawObject( Obj o, vec3 d, vec3 sun, vec3 bg )
{
	vec3 r, u;
	Frame( o.dir, r, u );
	const float cosA = dot( d, o.dir );
	if ( cosA <= 0.0 )
		return bg;
	// where d falls on the object's disc, in its radii
	const vec2 q = vec2( dot( d, r ), dot( d, u ) ) / cosA / tan( o.size );
	const float rr = dot( q, q );

	if ( o.kind == OBJ_NEBULA ) {
		const float n = SkyFbm( q * 1.5 + o.seed, 5 ) * smoothstep( 1.0, 0.2, sqrt( rr ) );
		return bg + o.color * pow( n, 2.0 ) * 1.4;
	}
	if ( o.kind == OBJ_SUN ) {
		const float rd = sqrt( rr );
		vec3 c = bg + o.color * ( exp( -rd * 2.5 ) * 0.6 + exp( -rd * 0.6 ) * 0.15 );
		if ( rd < 1.0 )
			c = mix( c, o.color * 3.0, smoothstep( 1.0, 0.92, rd ) );
		return c;
	}
	if ( o.kind == OBJ_FLEET ) {
		// a few wedges, their engines lit, drifting slowly
		vec3 c = bg;
		const int ships = 3 + int( o.extra );
		for ( int s = 0; s < 12; s++ ) {
			if ( s >= ships )
				break;
			const vec2 at = vec2( SkyHash( vec2( o.seed * 13.0 + 1.0, s * 7.0 + 3.0 ) ) - 0.5, SkyHash( vec2( s * 11.0 + 5.0, o.seed * 3.0 + 9.0 ) ) - 0.5 ) * 1.6
				+ vec2( pc.a.w * 0.003 * o.extra2, 0.0 );
			const float sz = 0.12 + 0.12 * SkyHash( vec2( s * 7.0 + 2.0, o.seed * 5.0 + 1.0 ) );
			const vec2 l = ( q - at ) / sz;
			// a wedge: its point forward (+x), its back at -1
			if ( l.x > -1.0 && l.x < 1.0 && abs( l.y ) < ( 1.0 - l.x ) * 0.32 ) {
				c = mix( c, o.color * ( 0.35 + 0.25 * step( 0.0, l.y ) ), 0.95 );
				if ( l.x < -0.9 && abs( l.y ) < 0.4 )
					c += vec3( 0.5, 0.7, 1.0 ) * 1.5;
			}
		}
		return c;
	}

	// a sphere, its rings behind and in front
	vec3 c = bg;
	const float ringIn = 1.35, ringOut = 2.2;
	const float ct = cos( o.tilt ), st = sin( o.tilt );
	const bool rings = o.kind == OBJ_RINGED || o.extra > 0.0;
	float ringA = 0.0;
	vec3 ringC = vec3( 0.0 );
	if ( rings ) {
		// the ring plane seen tilted: an ellipse
		const vec2 e = vec2( q.x, q.y / max( 0.08, abs( st ) ) );
		const float er = length( e );
		if ( er > ringIn && er < ringOut ) {
			ringA = ( 0.55 + 0.4 * SkyNoise( vec2( er * 30.0, o.seed ) ) ) * smoothstep( ringIn, ringIn + 0.05, er ) * smoothstep( ringOut, ringOut - 0.1, er );
			ringC = o.color * vec3( 1.0, 0.95, 0.85 );
		}
	}
	const bool ringInFront = ( q.y * st ) < 0.0;
	if ( rings && !ringInFront )
		c = mix( c, ringC, ringA * 0.85 );
	if ( rr < 1.0 ) {
		// the sphere's facing, lit by the sun (as seen from here, roughly)
		const vec3 n = vec3( q.x, q.y, sqrt( 1.0 - rr ) );
		// the sun in the sphere's frame (its z toward the camera): a crescent when it's behind the sphere
		const vec3 sunHere = normalize( vec3( dot( sun, r ), dot( sun, u ), -dot( sun, o.dir ) ) );
		// a little light on the dark side too, as a world in the sky is never quite black
		float lit = clamp( dot( n, sunHere ) * 1.1 + 0.18, 0.0, 1.0 );
		if ( o.kind == OBJ_STATION )
			lit = max( lit, 0.15 );
		vec3 s = SphereSurface( o, n ) * lit;
		// a thin glow of air at the rim of a planet
		if ( o.kind == OBJ_PLANET )
			s += o.color * vec3( 0.5, 0.7, 1.0 ) * pow( 1.0 - n.z, 3.0 ) * lit * 0.8;
		c = mix( c, s, smoothstep( 1.0, 0.985, rr ) );
	}
	if ( rings && ringInFront )
		c = mix( c, ringC * 0.9, ringA * 0.85 );
	return c;
}

// stars: fixed in the sky, twinkling
vec3 Stars( vec3 d )
{
	const vec3 a = abs( d );
	vec2 face;
	float id;

	if ( a.x >= a.y && a.x >= a.z ) { face = d.yz / a.x; id = d.x > 0.0 ? 0.0 : 1.0; }
	else if ( a.y >= a.z ) { face = d.xz / a.y; id = d.y > 0.0 ? 2.0 : 3.0; }
	else { face = d.xy / a.z; id = d.z > 0.0 ? 4.0 : 5.0; }
	const vec2 cell = floor( face * 220.0 );
	const float h = SkyHash( cell + vec2( id * 977.0, id * 431.0 ) );
	if ( h < 0.992 )
		return vec3( 0.0 );
	const vec2 f = fract( face * 220.0 ) - 0.5;
	const float bright = ( h - 0.992 ) / 0.008;
	const float twinkle = 0.75 + 0.25 * sin( pc.a.w * ( 2.0 + bright * 5.0 ) + h * 100.0 );
	return vec3( 0.9, 0.95, 1.0 ) * bright * twinkle * smoothstep( 0.35, 0.0, length( f ) ) * 1.5;
}

void main()
{
	const vec2 p = floor( gl_FragCoord.xy ) + 0.5;
	vec3 c = Source( p );

	out_color = vec4( c, 1.0 );
	if ( !IsSky( p ) )
		return;

	// the skybox sharpened, from its sky neighbours only
	if ( pc.c.x > 0.0 ) {
		vec3 sum = vec3( 0.0 );
		float n = 0.0;
		const vec2 o[4] = vec2[4]( vec2( 1.0, 0.0 ), vec2( -1.0, 0.0 ), vec2( 0.0, 1.0 ), vec2( 0.0, -1.0 ) );
		for ( int i = 0; i < 4; i++ ) {
			if ( IsSky( p + o[i] ) ) {
				sum += Source( p + o[i] );
				n += 1.0;
			}
		}
		if ( n > 0.0 )
			c = max( c + ( c - sum / n ) * pc.c.x, 0.0 );
	}

	const vec3 d = normalize( ToWorld( ViewPoint( p, 1.0, pc.rect, pc.a.xy ) ) );
	const vec3 sun = normalize( pc.sun.xyz );
	const float night = 1.0 - SkyDay( sun );

	// the atmosphere over the map's sky, stars coming out at night
	if ( pc.sun.w > 0.0 ) {
		vec3 atmo = SkyColor( d, sun );
		if ( d.z > 0.0 )
			atmo += Stars( d ) * night * pc.c.w;
		// no sun's disc of its own: the composer's Sun objects are the suns seen
		c = mix( c, atmo, pc.sun.w );
	}

	// the composer's objects, before the clouds
	for ( int i = 0; i < int( pc.b.w ) && i < 16; i++ )
		c = DrawObject( ObjectAt( i ), d, sun, c );

	// the clouds: where the view meets their layer, faded toward the horizon
	if ( pc.b.x > 0.0 && d.z > 0.01 ) {
		const vec3 cam = vec3( pc.right.w, pc.down.w, pc.fwd.w );
		const vec2 at = cam.xy + d.xy * ( CLOUD_HEIGHT / d.z );
		const vec2 wind = pc.b.yz;
		const float dens = CloudDensity( at, pc.b.x, wind );
		if ( dens > 0.0 ) {
			// light through the cloud toward the sun: thicker, darker
			float toward = 0.0;
			const vec2 step = normalize( sun.xy + 1e-4 ) * 300.0;
			for ( int k = 1; k <= 3; k++ )
				toward += CloudDensity( at + step * float( k ), pc.b.x, wind );
			const float light = exp( -toward * 0.9 ) * ( 0.4 + 0.6 * SkyDay( sun ) );
			const vec3 ambient = SkyColor( vec3( 0.0, 0.0, 1.0 ), sun ) * 0.9 + vec3( 0.02 );
			vec3 cloud = mix( ambient * 0.55, SunColor( sun ) * 1.1 + ambient * 0.4, light );
			// the sun's side brighter, as light comes through their edges
			cloud += SunColor( sun ) * pow( max( dot( d, sun ), 0.0 ), 8.0 ) * ( 1.0 - dens ) * 0.6;
			// rain: heavy and grey, lightning lighting them up from inside
			cloud = mix( cloud, cloud * vec3( 0.45, 0.47, 0.52 ), pc.c.y );
			cloud += vec3( 0.75, 0.8, 1.0 ) * pc.c.z * dens * ( 0.5 + 0.5 * SkyNoise( at / 1500.0 ) );
			const float fade = smoothstep( 0.01, 0.2, d.z );
			c = mix( c, cloud, clamp( dens * 1.15, 0.0, 1.0 ) * fade );
		}
	}

	out_color = vec4( c, 1.0 );
}
