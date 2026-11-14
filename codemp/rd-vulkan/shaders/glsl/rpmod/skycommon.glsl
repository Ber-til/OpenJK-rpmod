// OpenJK-rpmod: shared by sky.frag and aerial.frag: the sky's colour from the sun's place (a cheap
// atmosphere: blue by day, red and orange low at dusk, deep blue and stars by night), and the cloud
// layer's thickness over a point of the world.
//
// World space here: x, y across, z up. sun: toward the sun, its height (sin of elevation) in z.

// an integer hash: sin() of big numbers loses its precision on the GPU, and its noise turned to shards
float SkyHash( vec2 c )
{
	uvec2 q = uvec2( ivec2( floor( c ) ) ) * uvec2( 1597334673u, 3812015801u );
	uint n = ( q.x ^ q.y ) * 1597334673u;

	n ^= n >> 16;
	n *= 2246822519u;
	n ^= n >> 13;
	return float( n ) * ( 1.0 / 4294967296.0 );
}

float SkyNoise( vec2 w )
{
	const vec2 i = floor( w ), f = fract( w );
	const vec2 u = f * f * f * ( f * ( f * 6.0 - 15.0 ) + 10.0 );

	return mix( mix( SkyHash( i ), SkyHash( i + vec2( 1.0, 0.0 ) ), u.x ), mix( SkyHash( i + vec2( 0.0, 1.0 ) ), SkyHash( i + vec2( 1.0, 1.0 ) ), u.x ), u.y );
}

float SkyFbm( vec2 w, int octaves )
{
	float sum = 0.0, amp = 0.5;

	for ( int i = 0; i < octaves; i++ ) {
		sum += SkyNoise( w ) * amp;
		w = mat2( 1.6, 1.2, -1.2, 1.6 ) * w + 17.3;
		amp *= 0.5;
	}
	return sum;
}

// how much day it is (1 the sun up, 0 night), and dusk (the sun near the horizon)
float SkyDay( vec3 sun )
{
	return smoothstep( -0.12, 0.15, sun.z );
}

float SkyDusk( vec3 sun )
{
	return smoothstep( -0.15, 0.02, sun.z ) * ( 1.0 - smoothstep( 0.05, 0.35, sun.z ) );
}

// the sun's own light, white high up, orange low
vec3 SunColor( vec3 sun )
{
	return mix( vec3( 1.0, 0.45, 0.2 ), vec3( 1.0, 0.97, 0.9 ), smoothstep( 0.0, 0.4, sun.z ) ) * SkyDay( sun );
}

// the sky that way, without the sun's disc
vec3 SkyColor( vec3 d, vec3 sun )
{
	const float day = SkyDay( sun ), dusk = SkyDusk( sun );
	const float mu = dot( d, sun );
	const vec3 zenith = mix( vec3( 0.006, 0.01, 0.03 ), vec3( 0.18, 0.38, 0.78 ), day );
	vec3 horizon = mix( vec3( 0.02, 0.03, 0.06 ), vec3( 0.62, 0.76, 0.93 ), day );
	const float h = pow( 1.0 - clamp( d.z, 0.0, 1.0 ), 4.0 );

	// dusk: the horizon toward the sun reddens, the rest of it pinker
	horizon = mix( horizon, vec3( 1.0, 0.42, 0.16 ), dusk * ( 0.35 + 0.65 * pow( max( mu, 0.0 ), 3.0 ) ) );
	vec3 c = mix( zenith, horizon, h );
	// a soft glow on the sun's side, wider at dusk (no bright one round it: the composer's Sun objects
	// are the suns seen)
	c += SunColor( sun ) * pow( max( mu, 0.0 ), 6.0 ) * ( 0.12 + 0.25 * dusk );
	// under the horizon: the ground's dim reflection of it
	if ( d.z < 0.0 )
		c = mix( c, horizon * 0.35, smoothstep( 0.0, -0.2, d.z ) );
	return c;
}

// the cloud layer's thickness, 0 to 1, over a point of the world (its x and y, in world units)
// cover: 0 clear to 1 overcast; wind: how far the clouds have drifted
float CloudDensity( vec2 w, float cover, vec2 wind )
{
	// the shapes bent by a coarser noise, so they billow rather than tile
	vec2 q = ( w + wind ) / 2600.0;
	q += vec2( SkyFbm( q * 0.7 + 3.1, 3 ), SkyFbm( q * 0.7 + 8.7, 3 ) ) * 0.9;
	const float n = SkyFbm( q, 6 );

	return smoothstep( 1.0 - cover * 0.9 - 0.12, 1.1 - cover * 0.75, n );
}
