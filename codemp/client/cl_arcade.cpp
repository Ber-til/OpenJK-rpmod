/*
===========================================================================
Copyright (C) 2026, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// cl_arcade.cpp -- the datapad's Arcade: four games for one, and a high-score board for each through
// C1B3R, everyone with the client on it
//
// Sarlacc (grow by eating, don't bite yourself), Droid Dash (a Jedi running, jumping droids and
// ducking probes), Skyhopper (a little ship lifted through pylons' gaps) and Bot Blaster (waves of
// droids shot down). Each is played in a field of its own size, fitted into the datapad's screen,
// stepped 60 times a second. A run's score goes to C1B3R when it beats your best there (c1b3r/arcade.py
// checks it could be scored in the run's time: keep the games' speeds and points in step with it).
// A picture of the player's own replaces a game's shapes: gfx/arcade/<game>/<thing> (png, tga, jpg).

#include "cl_games.h"
#include "cl_http.h"
#include "cl_presence.h"
#include "keys.h"

#include <string>

typedef enum {
	AR_SARLACC,
	AR_DASH,
	AR_SKY,
	AR_BLASTER,
	AR_NUM
} arGameNum_t;

typedef enum {
	AP_LIST,
	AP_TITLE,
	AP_PLAY
} arPage_t;

#define AR_TICK_MS		( 1000.0f / 60.0f )
#define AR_DT			( 1.0f / 60.0f )
#define AR_TOP			10
#define AR_POST_RETRY	16000		// C1B3R takes one score every 15 seconds

typedef struct {
	const char	*word;			// C1B3R's name for it, and its pictures' folder
	const char	*title;
	const char	*tagline;
	const char	*rules[5];
	float		fw, fh;			// its field's size, in its own units
	void		( *reset )( void );
	void		( *tick )( void );
	void		( *draw )( void );
	void		( *key )( int key );
} arGame_t;

static struct {
	arPage_t	page;
	int			game;

	// the run
	qboolean	playing, paused, over, waiting;		// waiting: for the first key (Skyhopper)
	int			lastTime;
	float		acc, playMs;
	int			score;
	int			overTime;
	qboolean	newBest;

	// where the field is drawn: its units to the screen's
	float		fx, fy, sx, sy;

	// bests on this machine (cl_arcadeBest), and the board
	int			best[AR_NUM];
	struct {
		int		place, score;
		char	name[64];
	}			top[AR_TOP];
	int			numTop;
	int			boardGame;				// whose board that is, -1 none yet
	int			meBest, mePlace;		// yours on it, 0 none
	char		boardMsg[160];
	char		runMsg[160];			// what C1B3R said of the run's score

	httpRequest_t	boardReq, scoreReq;
	int			boardAsked, scoreGame;
	struct {
		int		game, score, ms, at;	// a score waiting to be sent again, at 0 none
	}			retry;
} ar;

static cvar_t *cl_arcadeBest;

static const float arRed[4] = { 0.86f, 0.22f, 0.2f, 1.0f };
static const float arOrange[4] = { 0.95f, 0.6f, 0.15f, 1.0f };
static const float arGreen[4] = { 0.35f, 0.95f, 0.4f, 1.0f };
static const float arGrey[4] = { 0.62f, 0.64f, 0.68f, 1.0f };
static const float arDark[4] = { 0.25f, 0.27f, 0.3f, 1.0f };
static const float arField[4] = { 0.0f, 0.0f, 0.0f, 0.45f };
static const float arShade[4] = { 0.0f, 0.0f, 0.0f, 0.65f };

/*
===============================================================================

Drawing and keys, in a field's units

===============================================================================
*/

static void AR_Box( float x, float y, float w, float h, const float *color ) {
	DP_Fill( ar.fx + x * ar.sx, ar.fy + y * ar.sy, w * ar.sx, h * ar.sy, color );
}

// the player's own picture of a thing, else its box
static void AR_Sprite( const char *thing, float x, float y, float w, float h, const float *color ) {
	static struct {
		char		path[64];
		qhandle_t	shader;
	} known[32];
	static int numKnown;
	const char *path = va( "gfx/arcade/%s/%s", ar.game >= 0 ? ( ar.game == AR_SARLACC ? "sarlacc" : ar.game == AR_DASH ? "dash"
		: ar.game == AR_SKY ? "sky" : "blaster" ) : "", thing );
	int i;

	for ( i = 0; i < numKnown && strcmp( known[i].path, path ); i++ )
		;
	if ( i == numKnown ) {
		if ( numKnown == (int)ARRAY_LEN( known ) ) {
			AR_Box( x, y, w, h, color );
			return;
		}
		Q_strncpyz( known[i].path, path, sizeof( known[i].path ) );
		known[i].shader = GM_Art( path );
		numKnown++;
	}
	if ( !known[i].shader ) {
		AR_Box( x, y, w, h, color );
		return;
	}
	re->SetColor( NULL );
	re->DrawStretchPic( ar.fx + x * ar.sx, ar.fy + y * ar.sy, w * ar.sx, h * ar.sy, 0, 0, 1, 1, known[i].shader );
}

static qboolean AR_IsUp( int key )		{ return (qboolean)( key == A_CURSOR_UP || key == A_LOW_W || key == A_CAP_W ); }
static qboolean AR_IsDown( int key )	{ return (qboolean)( key == A_CURSOR_DOWN || key == A_LOW_S || key == A_CAP_S ); }
static qboolean AR_IsLeft( int key )	{ return (qboolean)( key == A_CURSOR_LEFT || key == A_LOW_A || key == A_CAP_A ); }
static qboolean AR_IsRight( int key )	{ return (qboolean)( key == A_CURSOR_RIGHT || key == A_LOW_D || key == A_CAP_D ); }
static qboolean AR_IsAction( int key )	{ return (qboolean)( key == A_SPACE || key == A_ENTER || key == A_KP_ENTER ); }

static qboolean AR_Held( qboolean ( *is )( int ) ) {
	static const int keys[] = { A_CURSOR_UP, A_LOW_W, A_CAP_W, A_CURSOR_DOWN, A_LOW_S, A_CAP_S, A_CURSOR_LEFT, A_LOW_A, A_CAP_A,
		A_CURSOR_RIGHT, A_LOW_D, A_CAP_D, A_SPACE, A_ENTER, A_KP_ENTER };

	for ( int i = 0; i < (int)ARRAY_LEN( keys ); i++ ) {
		if ( is( keys[i] ) && Key_IsDown( keys[i] ) )
			return qtrue;
	}
	return qfalse;
}

static qboolean AR_Overlap( float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh ) {
	return (qboolean)( ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah );
}

static void AR_Over( void ) {
	ar.over = qtrue;
	ar.overTime = cls.realtime;
}

/*
===============================================================================

Sarlacc: grow by eating; walls and your own tail are the end

===============================================================================
*/

#define SN_W		20
#define SN_H		28
#define SN_CELL		10.0f

static struct {
	int		x[SN_W * SN_H], y[SN_W * SN_H];
	int		len;
	int		dir, queue[2], numQueued;
	int		fruitX, fruitY;
	int		fruits;
	float	acc;
} sn;

static const int snDX[4] = { 0, 1, 0, -1 }, snDY[4] = { -1, 0, 1, 0 };

static qboolean SN_OnBody( int x, int y, int from ) {
	for ( int i = from; i < sn.len; i++ ) {
		if ( sn.x[i] == x && sn.y[i] == y )
			return qtrue;
	}
	return qfalse;
}

static qboolean SN_PlaceFruit( void ) {
	for ( int tries = 0; tries < 200; tries++ ) {
		const int x = Q_irand( 0, SN_W - 1 ), y = Q_irand( 0, SN_H - 1 );

		if ( !SN_OnBody( x, y, 0 ) ) {
			sn.fruitX = x;
			sn.fruitY = y;
			return qtrue;
		}
	}
	for ( int y = 0; y < SN_H; y++ ) {
		for ( int x = 0; x < SN_W; x++ ) {
			if ( !SN_OnBody( x, y, 0 ) ) {
				sn.fruitX = x;
				sn.fruitY = y;
				return qtrue;
			}
		}
	}
	return qfalse;		// it fills the field
}

static void SN_Reset( void ) {
	memset( &sn, 0, sizeof( sn ) );
	sn.len = 4;
	for ( int i = 0; i < sn.len; i++ ) {
		sn.x[i] = SN_W / 2 - i;
		sn.y[i] = SN_H / 2;
	}
	sn.dir = 1;
	SN_PlaceFruit();
}

static void SN_Step( void ) {
	int x, y;

	if ( sn.numQueued ) {
		sn.dir = sn.queue[0];
		sn.queue[0] = sn.queue[1];
		sn.numQueued--;
	}
	x = sn.x[0] + snDX[sn.dir];
	y = sn.y[0] + snDY[sn.dir];
	const qboolean eats = (qboolean)( x == sn.fruitX && y == sn.fruitY );
	// the tail's end moves out of the way, unless it grows
	if ( x < 0 || y < 0 || x >= SN_W || y >= SN_H || ( SN_OnBody( x, y, 0 ) && !( !eats && x == sn.x[sn.len - 1] && y == sn.y[sn.len - 1] ) ) ) {
		GM_Sound( GM_SND_LOSE );
		AR_Over();
		return;
	}
	if ( eats && sn.len < SN_W * SN_H )
		sn.len++;
	for ( int i = sn.len - 1; i > 0; i-- ) {
		sn.x[i] = sn.x[i - 1];
		sn.y[i] = sn.y[i - 1];
	}
	sn.x[0] = x;
	sn.y[0] = y;
	if ( eats ) {
		sn.fruits++;
		ar.score += 10;
		GM_Sound( GM_SND_CLICK );
		if ( !SN_PlaceFruit() )
			AR_Over();
	}
}

static void SN_Tick( void ) {
	// 8 steps a second, one more each 5 fruit, 15 at most (C1B3R: 10 points a fruit)
	const float steps = Q_min( 15.0f, 8.0f + (float)( sn.fruits / 5 ) );

	sn.acc += steps * AR_DT;
	while ( sn.acc >= 1.0f && !ar.over ) {
		sn.acc -= 1.0f;
		SN_Step();
	}
}

static void SN_Key( int key ) {
	const int last = sn.numQueued ? sn.queue[sn.numQueued - 1] : sn.dir;
	const int d = AR_IsUp( key ) ? 0 : AR_IsRight( key ) ? 1 : AR_IsDown( key ) ? 2 : AR_IsLeft( key ) ? 3 : -1;

	if ( d < 0 || d == last || d == ( last + 2 ) % 4 || sn.numQueued >= 2 )
		return;
	sn.queue[sn.numQueued++] = d;
}

static void SN_Draw( void ) {
	AR_Sprite( "fruit", sn.fruitX * SN_CELL + 1, sn.fruitY * SN_CELL + 1, SN_CELL - 2, SN_CELL - 2, arRed );
	for ( int i = sn.len - 1; i >= 0; i-- )
		AR_Sprite( i ? "body" : "head", sn.x[i] * SN_CELL + 0.5f, sn.y[i] * SN_CELL + 0.5f, SN_CELL - 1, SN_CELL - 1, i ? dpGlow : dpWhite );
}

/*
===============================================================================

Droid Dash: a Jedi running; jump the droids rolling at you, duck the probes

===============================================================================
*/

#define DH_W			200.0f
#define DH_H			150.0f
#define DH_GROUND		125.0f
#define DH_PX			36.0f
#define DH_UNITS		8.0f		// a metre
#define DH_MAX_OBS		8

typedef enum { DO_ROLLER, DO_PROBE } dhObstacle_t;

static struct {
	float	h, vy;				// above the ground, and how fast up
	qboolean ducking;
	float	dist;				// metres
	struct {
		float			x;
		dhObstacle_t	type;
	}		obs[DH_MAX_OBS];
	int		numObs;
	float	stripe;
} dh;

static float DH_Speed( void ) {
	// metres a second: 10, faster with the run, 30 at most (C1B3R: a metre a point)
	return Q_min( 30.0f, 10.0f + 0.2f * ar.playMs / 1000.0f );
}

static void DH_Reset( void ) {
	memset( &dh, 0, sizeof( dh ) );
}

static void DH_Jump( void ) {
	if ( dh.h <= 0.0f ) {
		dh.vy = 330.0f;
		GM_Sound( GM_SND_PLAY );
	}
}

static void DH_Tick( void ) {
	const float speed = DH_Speed(), move = speed * DH_UNITS * AR_DT;
	const qboolean held = (qboolean)( AR_Held( AR_IsUp ) || AR_Held( AR_IsAction ) );
	float g;

	dh.ducking = AR_Held( AR_IsDown );
	// held, a jump goes higher; ducking in the air brings you down
	g = dh.vy > 0.0f && held ? 620.0f : 1300.0f;
	if ( dh.ducking && dh.h > 0.0f )
		g *= 2.0f;
	dh.h += dh.vy * AR_DT;
	dh.vy -= g * AR_DT;
	if ( dh.h <= 0.0f ) {
		dh.h = 0.0f;
		dh.vy = 0.0f;
	}
	dh.dist += speed * AR_DT;
	ar.score = (int)dh.dist;
	dh.stripe = fmodf( dh.stripe + move, 20.0f );

	// the droids come on; a new one past a gap that grows with the speed
	for ( int i = 0; i < dh.numObs; i++ )
		dh.obs[i].x -= move;
	while ( dh.numObs && dh.obs[0].x < -24.0f ) {
		memmove( dh.obs, dh.obs + 1, sizeof( dh.obs[0] ) * --dh.numObs );
	}
	const float lastX = dh.numObs ? dh.obs[dh.numObs - 1].x : 0.0f;
	if ( dh.numObs < DH_MAX_OBS && DH_W + 10.0f - lastX > Q_max( 90.0f, speed * DH_UNITS * Q_flrand( 0.75f, 1.5f ) ) && Q_irand( 0, 20 ) == 0 ) {
		dh.obs[dh.numObs].x = DH_W + 10.0f;
		dh.obs[dh.numObs].type = dh.dist > 150.0f && Q_irand( 0, 3 ) == 0 ? DO_PROBE : DO_ROLLER;
		dh.numObs++;
	}

	// hit: a little forgiving at the edges
	const float ph = dh.ducking ? 12.0f : 24.0f;
	for ( int i = 0; i < dh.numObs; i++ ) {
		const qboolean probe = (qboolean)( dh.obs[i].type == DO_PROBE );
		const float oy = probe ? 17.0f : 0.0f, ow = probe ? 16.0f : 14.0f, oh = probe ? 10.0f : 14.0f;

		if ( AR_Overlap( DH_PX + 2, dh.h + 1, 6, ph - 2, dh.obs[i].x + 2, oy + 1, ow - 4, oh - 2 ) ) {
			GM_Sound( GM_SND_LOSE );
			AR_Over();
			return;
		}
	}
}

static void DH_Key( int key ) {
	if ( AR_IsUp( key ) || AR_IsAction( key ) )
		DH_Jump();
}

static void DH_Draw( void ) {
	const float ph = dh.ducking ? 12.0f : 24.0f;
	const float top = DH_GROUND - dh.h - ph;

	AR_Box( 0, DH_GROUND, DH_W, 1.5f, dpGlow );
	for ( float x = -dh.stripe; x < DH_W; x += 20.0f )
		AR_Box( Q_max( 0.0f, x ), DH_GROUND + 6, 8, 1, dpDim );
	// the Jedi: robe, head, the saber held back
	AR_Sprite( dh.ducking ? "jedi_duck" : "jedi", DH_PX, top, 10, ph, dpText );
	if ( !dh.ducking ) {
		AR_Box( DH_PX + 2, top - 7, 6, 6, dpText );
		AR_Box( DH_PX - 6, top + 6, 8, 1.5f, arGreen );
	}
	for ( int i = 0; i < dh.numObs; i++ ) {
		const float x = dh.obs[i].x;

		if ( dh.obs[i].type == DO_PROBE ) {
			AR_Sprite( "probe", x, DH_GROUND - 27, 16, 10, arDark );
			AR_Box( x + 3, DH_GROUND - 17, 1, 6, arGrey );
			AR_Box( x + 12, DH_GROUND - 17, 1, 6, arGrey );
			AR_Box( x + 2, DH_GROUND - 24, 3, 3, arRed );
		} else {
			AR_Sprite( "droid", x, DH_GROUND - 14, 14, 14, arGrey );
			AR_Box( x + 3, DH_GROUND - 10, 3, 3, arRed );
		}
	}
}

/*
===============================================================================

Skyhopper: a lift with each key, through the pylons' gaps

===============================================================================
*/

#define SK_W			200.0f
#define SK_H			300.0f
#define SK_SHIPX		50.0f
#define SK_SPEED		90.0f		// units a second
#define SK_SPACING		117.0f		// a pylon every 1.3 seconds (C1B3R: a point each)
#define SK_PYLON_W		26.0f
#define SK_MAX			4

static struct {
	float	y, vy;
	struct {
		float		x, gapY, gapH;
		qboolean	passed;
	}		pylons[SK_MAX];
	int		numPylons;
} sk;

static void SK_Reset( void ) {
	memset( &sk, 0, sizeof( sk ) );
	sk.y = SK_H * 0.45f;
	ar.waiting = qtrue;
}

static void SK_Lift( void ) {
	if ( ar.waiting ) {
		ar.waiting = qfalse;
		ar.lastTime = cls.realtime;
	}
	sk.vy = -250.0f;
	GM_Sound( GM_SND_PLAY );
}

static void SK_Tick( void ) {
	sk.vy = Q_min( 420.0f, sk.vy + 820.0f * AR_DT );
	sk.y += sk.vy * AR_DT;
	if ( sk.y < 0.0f ) {
		sk.y = 0.0f;
		sk.vy = 0.0f;
	}
	for ( int i = 0; i < sk.numPylons; i++ ) {
		sk.pylons[i].x -= SK_SPEED * AR_DT;
		if ( !sk.pylons[i].passed && sk.pylons[i].x + SK_PYLON_W < SK_SHIPX ) {
			sk.pylons[i].passed = qtrue;
			ar.score++;
			GM_Sound( GM_SND_CLICK );
		}
	}
	while ( sk.numPylons && sk.pylons[0].x < -SK_PYLON_W - 2 )
		memmove( sk.pylons, sk.pylons + 1, sizeof( sk.pylons[0] ) * --sk.numPylons );
	if ( sk.numPylons < SK_MAX && ( !sk.numPylons || sk.pylons[sk.numPylons - 1].x < SK_W + 10.0f - SK_SPACING ) ) {
		const float gap = Q_max( 64.0f, 88.0f - ar.score * 0.6f );

		sk.pylons[sk.numPylons].x = SK_W + 10.0f;
		sk.pylons[sk.numPylons].gapH = gap;
		sk.pylons[sk.numPylons].gapY = Q_flrand( 30.0f + gap * 0.5f, SK_H - 40.0f - gap * 0.5f );
		sk.pylons[sk.numPylons].passed = qfalse;
		sk.numPylons++;
	}
	// the ground, or a pylon
	if ( sk.y + 10.0f >= SK_H - 8.0f ) {
		GM_Sound( GM_SND_LOSE );
		AR_Over();
		return;
	}
	for ( int i = 0; i < sk.numPylons; i++ ) {
		const float top = sk.pylons[i].gapY - sk.pylons[i].gapH * 0.5f, bottom = sk.pylons[i].gapY + sk.pylons[i].gapH * 0.5f;

		if ( AR_Overlap( SK_SHIPX + 1, sk.y + 1, 14, 8, sk.pylons[i].x, 0, SK_PYLON_W, top )
			|| AR_Overlap( SK_SHIPX + 1, sk.y + 1, 14, 8, sk.pylons[i].x, bottom, SK_PYLON_W, SK_H - bottom ) ) {
			GM_Sound( GM_SND_LOSE );
			AR_Over();
			return;
		}
	}
}

static void SK_Key( int key ) {
	if ( AR_IsUp( key ) || AR_IsAction( key ) || key == A_MOUSE1 )
		SK_Lift();
}

static void SK_Draw( void ) {
	for ( int i = 0; i < sk.numPylons; i++ ) {
		const float x = sk.pylons[i].x, top = sk.pylons[i].gapY - sk.pylons[i].gapH * 0.5f, bottom = sk.pylons[i].gapY + sk.pylons[i].gapH * 0.5f;

		AR_Sprite( "pylon_top", x, 0, SK_PYLON_W, top, arDark );
		AR_Sprite( "pylon", x, bottom, SK_PYLON_W, SK_H - 8.0f - bottom, arDark );
		AR_Box( x - 2, top - 5, SK_PYLON_W + 4, 5, dpGlow );
		AR_Box( x - 2, bottom, SK_PYLON_W + 4, 5, dpGlow );
	}
	AR_Box( 0, SK_H - 8.0f, SK_W, 8.0f, dpDim );
	// the ship: a hull, its wings tilted with its climb
	const float tilt = Com_Clamp( -3.0f, 3.0f, sk.vy / 80.0f );
	AR_Sprite( "ship", SK_SHIPX, sk.y + 2, 16, 6, dpWhite );
	AR_Box( SK_SHIPX + 2, sk.y - 1 + tilt, 8, 2, dpGlow );
	AR_Box( SK_SHIPX + 2, sk.y + 9 - tilt, 8, 2, dpGlow );
	AR_Box( SK_SHIPX - 3, sk.y + 4, 3, 2, arOrange );
	if ( ar.waiting )
		DP_Text( ar.fx, ar.fy + SK_H * 0.62f * ar.sy, SK_W * ar.sx, "Space, Up or a click to fly", dpText, DP_Font(), 0.6f, qtrue );
}

/*
===============================================================================

Bot Blaster: waves of droids coming down, shot before they land

===============================================================================
*/

#define BB_W			200.0f
#define BB_H			300.0f
#define BB_PY			276.0f
#define BB_COLS			6
#define BB_ROWS			5
#define BB_MAX_BOLTS	16
#define BB_MAX_SHOTS	12

static struct {
	float	px;
	float	heat, cooldown;
	qboolean overheated;
	int		lives;
	float	hurt;							// seconds still unhurtable
	int		wave;
	struct {
		int		hp, points;				// hp 0: gone
	}		droids[BB_ROWS][BB_COLS];
	int		rows;
	float	ox, oy, dir;
	struct { float x, y; } bolts[BB_MAX_BOLTS], shots[BB_MAX_SHOTS];
	int		numBolts, numShots;
} bb;

static void BB_Wave( void ) {
	bb.wave++;
	bb.rows = Q_min( BB_ROWS, 2 + bb.wave );
	for ( int r = 0; r < BB_ROWS; r++ ) {
		for ( int c = 0; c < BB_COLS; c++ ) {
			// droidekas at the top from the third wave (two hits), battle droids, probes at the bottom
			const qboolean deka = (qboolean)( r == 0 && bb.wave >= 3 );

			bb.droids[r][c].hp = r < bb.rows ? ( deka ? 2 : 1 ) : 0;
			bb.droids[r][c].points = deka ? 50 : r < bb.rows - 1 ? 20 : 10;
		}
	}
	bb.ox = 12.0f;
	bb.oy = 30.0f;
	bb.dir = 1.0f;
	bb.numShots = 0;
}

static void BB_Reset( void ) {
	memset( &bb, 0, sizeof( bb ) );
	bb.px = BB_W * 0.5f - 8.0f;
	bb.lives = 3;
	BB_Wave();
}

static void BB_Tick( void ) {
	const float droidSpeed = Q_min( 70.0f, 18.0f + 6.0f * bb.wave );
	int alive = 0, minC = BB_COLS, maxC = -1, maxR = -1;

	// you: move, fire while held (5 a second, heating; overheated, cooled down to 40 before firing again)
	if ( AR_Held( AR_IsLeft ) )
		bb.px -= 140.0f * AR_DT;
	if ( AR_Held( AR_IsRight ) )
		bb.px += 140.0f * AR_DT;
	bb.px = Com_Clamp( 0.0f, BB_W - 16.0f, bb.px );
	bb.cooldown -= AR_DT;
	bb.heat = Q_max( 0.0f, bb.heat - 25.0f * AR_DT );
	if ( bb.overheated && bb.heat < 40.0f )
		bb.overheated = qfalse;
	if ( ( AR_Held( AR_IsAction ) || AR_Held( AR_IsUp ) ) && bb.cooldown <= 0.0f && !bb.overheated && bb.numBolts < BB_MAX_BOLTS ) {
		bb.bolts[bb.numBolts].x = bb.px + 7.0f;
		bb.bolts[bb.numBolts].y = BB_PY - 6.0f;
		bb.numBolts++;
		bb.cooldown = 0.2f;
		bb.heat += 14.0f;
		if ( bb.heat >= 100.0f )
			bb.overheated = qtrue;
		GM_Sound( GM_SND_DEAL );
	}
	if ( bb.hurt > 0.0f )
		bb.hurt -= AR_DT;

	// the droids: across, and down a step at each side
	for ( int r = 0; r < BB_ROWS; r++ ) {
		for ( int c = 0; c < BB_COLS; c++ ) {
			if ( bb.droids[r][c].hp > 0 ) {
				alive++;
				minC = Q_min( minC, c );
				maxC = Q_max( maxC, c );
				maxR = Q_max( maxR, r );
			}
		}
	}
	if ( !alive ) {
		ar.score += 100 * Q_min( bb.wave, 10 );
		GM_Sound( GM_SND_WIN );
		BB_Wave();
		return;
	}
	bb.ox += bb.dir * droidSpeed * AR_DT;
	if ( bb.ox + minC * 28.0f < 2.0f || bb.ox + maxC * 28.0f + 16.0f > BB_W - 2.0f ) {
		bb.dir = -bb.dir;
		bb.ox += bb.dir * droidSpeed * AR_DT;
		bb.oy += 8.0f;
	}
	if ( bb.oy + maxR * 22.0f + 12.0f >= BB_PY - 6.0f ) {
		GM_Sound( GM_SND_LOSE );
		AR_Over();
		return;
	}

	// your bolts
	for ( int i = 0; i < bb.numBolts; i++ ) {
		qboolean gone = qfalse;

		bb.bolts[i].y -= 320.0f * AR_DT;
		for ( int r = 0; r < BB_ROWS && !gone; r++ ) {
			for ( int c = 0; c < BB_COLS && !gone; c++ ) {
				if ( bb.droids[r][c].hp > 0 && AR_Overlap( bb.bolts[i].x, bb.bolts[i].y, 2, 6, bb.ox + c * 28.0f, bb.oy + r * 22.0f, 16, 12 ) ) {
					gone = qtrue;
					if ( !--bb.droids[r][c].hp ) {
						ar.score += bb.droids[r][c].points;
						GM_Sound( GM_SND_CLICK );
					}
				}
			}
		}
		if ( gone || bb.bolts[i].y < -6.0f ) {
			bb.bolts[i] = bb.bolts[--bb.numBolts];
			i--;
		}
	}

	// theirs: the lowest droid of a column fires, more often each wave
	if ( bb.numShots < BB_MAX_SHOTS && Q_flrand( 0.0f, 1.0f ) < Q_min( 2.5f, 0.5f + 0.25f * bb.wave ) * AR_DT ) {
		const int c = Q_irand( minC, maxC );

		for ( int r = BB_ROWS - 1; r >= 0; r-- ) {
			if ( bb.droids[r][c].hp > 0 ) {
				bb.shots[bb.numShots].x = bb.ox + c * 28.0f + 7.0f;
				bb.shots[bb.numShots].y = bb.oy + r * 22.0f + 12.0f;
				bb.numShots++;
				break;
			}
		}
	}
	for ( int i = 0; i < bb.numShots; i++ ) {
		bb.shots[i].y += Q_min( 200.0f, 110.0f + 10.0f * bb.wave ) * AR_DT;
		if ( bb.hurt <= 0.0f && AR_Overlap( bb.shots[i].x, bb.shots[i].y, 2, 6, bb.px + 1, BB_PY, 14, 10 ) ) {
			bb.shots[i] = bb.shots[--bb.numShots];
			if ( --bb.lives <= 0 ) {
				GM_Sound( GM_SND_LOSE );
				AR_Over();
				return;
			}
			GM_Sound( GM_SND_TURN );
			bb.hurt = 1.5f;
			i--;
			continue;
		}
		if ( bb.shots[i].y > BB_H ) {
			bb.shots[i] = bb.shots[--bb.numShots];
			i--;
		}
	}
}

static void BB_Key( int key ) {
}

static void BB_Draw( void ) {
	for ( int r = 0; r < BB_ROWS; r++ ) {
		for ( int c = 0; c < BB_COLS; c++ ) {
			const float x = bb.ox + c * 28.0f, y = bb.oy + r * 22.0f;

			if ( bb.droids[r][c].hp <= 0 )
				continue;
			if ( bb.droids[r][c].points == 50 ) {
				AR_Sprite( "droideka", x, y, 16, 12, bb.droids[r][c].hp > 1 ? arOrange : arRed );
				AR_Box( x + 6, y + 4, 4, 3, arDark );
			} else if ( bb.droids[r][c].points == 20 ) {
				AR_Sprite( "droid", x + 3, y, 10, 12, arGrey );
				AR_Box( x + 5, y + 2, 6, 2, arDark );
			} else {
				AR_Sprite( "probe", x + 1, y + 2, 14, 8, arDark );
				AR_Box( x + 6, y + 4, 3, 3, arRed );
			}
		}
	}
	for ( int i = 0; i < bb.numBolts; i++ )
		AR_Box( bb.bolts[i].x, bb.bolts[i].y, 2, 6, arGreen );
	for ( int i = 0; i < bb.numShots; i++ )
		AR_Box( bb.shots[i].x, bb.shots[i].y, 2, 6, arRed );
	// you, blinking while unhurtable
	if ( bb.hurt <= 0.0f || ( cls.realtime / 100 ) % 2 ) {
		AR_Sprite( "ship", bb.px, BB_PY + 3, 16, 7, dpWhite );
		AR_Box( bb.px + 6, BB_PY, 4, 4, dpGlow );
	}
	// lives, the wave, the heat
	for ( int i = 0; i < bb.lives; i++ )
		AR_Box( 4 + i * 10.0f, BB_H - 6, 7, 3, dpWhite );
	AR_Box( BB_W - 54, BB_H - 6, 50, 3, dpTile );
	AR_Box( BB_W - 54, BB_H - 6, 50 * Q_min( 1.0f, bb.heat / 100.0f ), 3, bb.overheated ? arRed : arOrange );
	DP_Text( ar.fx, ar.fy + 4 * ar.sy, BB_W * ar.sx, va( "Wave %i", bb.wave ), dpDim, DP_Font(), 0.5f, qtrue );
}

/*
===============================================================================

The games

===============================================================================
*/

static const arGame_t arGames[AR_NUM] = {
	{ "sarlacc", "Sarlacc", "Eat, grow, and don't bite your own tail",
		{ "Arrows or WASD turn it.", "Each fruit: 10 points, and longer.", "Faster every 5 fruit.", "The walls and your own body are the end.", NULL },
		SN_W * SN_CELL, SN_H * SN_CELL, SN_Reset, SN_Tick, SN_Draw, SN_Key },
	{ "dash", "Droid Dash", "Run, jump the droids, duck the probes",
		{ "Space or Up jumps: hold it to jump higher.", "Down or S ducks under the probes.", "A metre a point; faster the longer you run.", NULL },
		DH_W, DH_H, DH_Reset, DH_Tick, DH_Draw, DH_Key },
	{ "sky", "Skyhopper", "Fly the pylons' gaps",
		{ "Space, Up or a click lifts the ship.", "A point for each pylon passed.", "The gaps close as you go.", "The ground and the pylons are the end.", NULL },
		SK_W, SK_H, SK_Reset, SK_Tick, SK_Draw, SK_Key },
	{ "blaster", "Bot Blaster", "Shoot the droids down before they land",
		{ "Left and right move; Space or Up fires.", "Firing heats the blaster: let it cool.", "Probes 10, droids 20, droidekas 50 (two hits).", "A wave cleared: 100 a wave. Three lives.", NULL },
		BB_W, BB_H, BB_Reset, BB_Tick, BB_Draw, BB_Key },
};

/*
===============================================================================

Bests and the board, through C1B3R

===============================================================================
*/

static void AR_LoadBests( void ) {
	const char *s = cl_arcadeBest->string;

	for ( int i = 0; i < AR_NUM; i++ ) {
		ar.best[i] = Q_max( 0, atoi( s ) );
		while ( *s && *s != ' ' )
			s++;
		while ( *s == ' ' )
			s++;
	}
}

static void AR_SaveBests( void ) {
	Cvar_Set( "cl_arcadeBest", va( "%i %i %i %i", ar.best[0], ar.best[1], ar.best[2], ar.best[3] ) );
}

static qboolean AR_Post( httpRequest_t *req, const char *what, const std::string &fields ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session || req->state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += fields + "}";
	return CL_Presence_Post( req, va( "/v2/arcade/%s", what ), body );
}

static void AR_AskBoard( int game ) {
	std::string fields = ",\"game\":";

	if ( ar.boardGame != game ) {
		ar.numTop = 0;
		ar.meBest = ar.mePlace = 0;
	}
	ar.boardGame = game;
	if ( !CL_Presence_Session() ) {
		Q_strncpyz( ar.boardMsg, "Not connected to C1B3R: no board for now", sizeof( ar.boardMsg ) );
		return;
	}
	CL_Presence_Json( fields, arGames[game].word );
	if ( AR_Post( &ar.boardReq, "board", fields ) ) {
		ar.boardAsked = game;
		if ( !ar.numTop )
			Q_strncpyz( ar.boardMsg, "Asking C1B3R...", sizeof( ar.boardMsg ) );
	}
}

static void AR_SendScore( int game, int score, int ms ) {
	std::string fields = ",\"game\":";

	if ( !CL_Presence_Session() ) {
		Q_strncpyz( ar.runMsg, "Not connected to C1B3R: kept on this machine", sizeof( ar.runMsg ) );
		return;
	}
	CL_Presence_Json( fields, arGames[game].word );
	fields += va( ",\"score\":%i,\"ms\":%i", score, ms );
	if ( AR_Post( &ar.scoreReq, "score", fields ) ) {
		ar.scoreGame = game;
		ar.retry.game = game;
		ar.retry.score = score;
		ar.retry.ms = ms;
		ar.retry.at = 0;
		Q_strncpyz( ar.runMsg, "Sending it to the board...", sizeof( ar.runMsg ) );
	}
}

// the run's over: a best kept, and sent to the board when it beats yours there
static void AR_RunOver( void ) {
	const int game = ar.game, score = ar.score;
	const int boardBest = ar.boardGame == game ? ar.meBest : 0;

	ar.newBest = (qboolean)( score > ar.best[game] );
	ar.runMsg[0] = '\0';
	if ( ar.newBest ) {
		ar.best[game] = score;
		AR_SaveBests();
		GM_Sound( GM_SND_WIN );
	}
	if ( score > 0 && ( ar.newBest || score > boardBest ) )
		AR_SendScore( game, score, (int)ar.playMs );
}

static void AR_ReadBoard( const char *answer ) {
	ar.numTop = 0;
	ar.meBest = ar.mePlace = 0;
	ar.boardMsg[0] = '\0';
	for ( const char *line = answer; line && *line; ) {
		const char *end = strchr( line, '\n' );
		std::string l( line, end ? end - line : strlen( line ) );
		std::string f[4];
		int n = 0;

		for ( size_t start = 0; n < 4; n++ ) {
			const size_t tab = l.find( '\t', start );

			f[n] = l.substr( start, tab == std::string::npos ? std::string::npos : tab - start );
			if ( tab == std::string::npos ) {
				n++;
				break;
			}
			start = tab + 1;
		}
		if ( f[0] == "top" && n >= 4 && ar.numTop < AR_TOP ) {
			ar.top[ar.numTop].place = atoi( f[1].c_str() );
			ar.top[ar.numTop].score = atoi( f[2].c_str() );
			Q_strncpyz( ar.top[ar.numTop].name, CL_Presence_Unfield( f[3].c_str() ).c_str(), sizeof( ar.top[0].name ) );
			ar.numTop++;
		} else if ( f[0] == "me" && n >= 3 ) {
			ar.meBest = atoi( f[1].c_str() );
			ar.mePlace = atoi( f[2].c_str() );
		}
		line = end ? end + 1 : NULL;
	}
	if ( !ar.numTop )
		Q_strncpyz( ar.boardMsg, "No one on the board yet: be the first", sizeof( ar.boardMsg ) );
}

static void AR_Message( char *out, int size, const httpRequest_t *req ) {
	const char *answer = HTTP_Answer( req );

	if ( !req->status ) {
		Q_strncpyz( out, "C1B3R didn't answer", size );
		return;
	}
	Q_strncpyz( out, answer, size );
	for ( char *nl = strchr( out, '\n' ); nl; nl = NULL )
		*nl = '\0';
}

void CL_Arcade_Frame( void ) {
	if ( HTTP_Poll( &ar.boardReq ) ) {
		if ( ar.boardReq.status == 200 && !strncmp( HTTP_Answer( &ar.boardReq ), "ok", 2 ) ) {
			if ( ar.boardAsked == ar.boardGame )
				AR_ReadBoard( HTTP_Answer( &ar.boardReq ) );
		} else {
			AR_Message( ar.boardMsg, sizeof( ar.boardMsg ), &ar.boardReq );
		}
	}
	if ( HTTP_Poll( &ar.scoreReq ) ) {
		const char *answer = HTTP_Answer( &ar.scoreReq );

		if ( ar.scoreReq.status == 200 && !strncmp( answer, "ok", 2 ) ) {
			const char *place = strrchr( answer, '\t' );

			Q_strncpyz( ar.runMsg, va( "On the board: #%i", place ? atoi( place + 1 ) : 0 ), sizeof( ar.runMsg ) );
			AR_AskBoard( ar.scoreGame );
		} else if ( ar.scoreReq.status == 429 ) {
			// two bests close together: the second sent once C1B3R takes it
			ar.retry.at = cls.realtime + AR_POST_RETRY;
			Q_strncpyz( ar.runMsg, "Sent in a few seconds...", sizeof( ar.runMsg ) );
		} else {
			AR_Message( ar.runMsg, sizeof( ar.runMsg ), &ar.scoreReq );
		}
	}
	if ( ar.retry.at && cls.realtime >= ar.retry.at && ar.scoreReq.state != HTTP_BUSY ) {
		ar.retry.at = 0;
		AR_SendScore( ar.retry.game, ar.retry.score, ar.retry.ms );
	}
}

/*
===============================================================================

The pages

===============================================================================
*/

static void AR_Start( void ) {
	ar.page = AP_PLAY;
	ar.playing = qtrue;
	ar.paused = ar.over = ar.waiting = ar.newBest = qfalse;
	ar.score = 0;
	ar.acc = ar.playMs = 0.0f;
	ar.lastTime = cls.realtime;
	ar.runMsg[0] = '\0';
	arGames[ar.game].reset();
}

static void AR_OpenGame( int game ) {
	ar.game = game;
	ar.page = AP_TITLE;
	ar.playing = qfalse;
	AR_AskBoard( game );
}

// the picture on a game's card
static void AR_CardPicture( int game, float x, float y, float w, float h ) {
	DP_Fill( x, y, w, h, arField );
	DP_Frame( x, y, w, h, dpGlow );
	switch ( game ) {
	case AR_SARLACC:
		for ( int i = 0; i < 5; i++ )
			DP_Fill( x + w * ( 0.15f + 0.12f * i ), y + h * 0.5f, w * 0.11f, h * 0.11f, i == 4 ? dpWhite : dpGlow );
		DP_Fill( x + w * 0.75f, y + h * 0.25f, w * 0.11f, h * 0.11f, arRed );
		break;
	case AR_DASH:
		DP_Fill( x + w * 0.1f, y + h * 0.78f, w * 0.8f, 1, dpGlow );
		DP_Fill( x + w * 0.25f, y + h * 0.35f, w * 0.1f, h * 0.3f, dpText );
		DP_Fill( x + w * 0.6f, y + h * 0.58f, w * 0.16f, h * 0.2f, arGrey );
		break;
	case AR_SKY:
		DP_Fill( x + w * 0.6f, y, w * 0.18f, h * 0.3f, arDark );
		DP_Fill( x + w * 0.6f, y + h * 0.65f, w * 0.18f, h * 0.35f, arDark );
		DP_Fill( x + w * 0.2f, y + h * 0.45f, w * 0.2f, h * 0.1f, dpWhite );
		break;
	default:
		for ( int i = 0; i < 3; i++ )
			DP_Fill( x + w * ( 0.15f + 0.25f * i ), y + h * 0.2f, w * 0.16f, h * 0.14f, arGrey );
		DP_Fill( x + w * 0.45f, y + h * 0.5f, w * 0.03f, h * 0.15f, arGreen );
		DP_Fill( x + w * 0.38f, y + h * 0.78f, w * 0.18f, h * 0.1f, dpWhite );
		break;
	}
}

static void AR_DrawList( const dpRect_t *s ) {
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 16 );
	float y = s->y + DP_STATUS_H + 6;

	if ( DP_Button( x, y, DP_Wide( 56 ), "< Games", qfalse ) ) {
		GM_Sound( GM_SND_CLICK );
		CL_Datapad_ToGames();
		return;
	}
	DP_Text( x + DP_Wide( 64 ), y, w, "Arcade", dpText, DP_TitleFont(), 0.62f, qfalse );
	y += DP_CTRL_H + 10;
	for ( int i = 0; i < AR_NUM; i++ ) {
		const float h = 58.0f;
		const qboolean over = DP_InRect( x, y, w, h );

		DP_Fill( x, y, w, h, over ? dpButtonHover : dpTile );
		DP_Frame( x, y, w, h, dpGlow );
		AR_CardPicture( i, x + DP_Wide( 6 ), y + 6, DP_Wide( 46 ), h - 12 );
		DP_Text( x + DP_Wide( 60 ), y + 7, w - DP_Wide( 64 ), arGames[i].title, dpText, DP_TitleFont(), 0.55f, qfalse );
		DP_Text( x + DP_Wide( 60 ), y + 24, w - DP_Wide( 64 ), arGames[i].tagline, dpDim, DP_Font(), 0.5f, qfalse );
		DP_Text( x + DP_Wide( 60 ), y + 38, w - DP_Wide( 64 ), ar.best[i] ? va( "Your best: %i", ar.best[i] ) : "Not played yet", dpText, DP_Font(), 0.5f, qfalse );
		if ( DP_Clicked( x, y, w, h ) ) {
			GM_Sound( GM_SND_CLICK );
			AR_OpenGame( i );
			return;
		}
		y += h + 6;
	}
}

static void AR_DrawBoard( float x, float y, float w, float bottom ) {
	DP_Text( x, y, w, "Board", dpText, DP_TitleFont(), 0.5f, qfalse );
	y += 16;
	for ( int i = 0; i < ar.numTop && y + 12 <= bottom; i++ ) {
		const float *c = ar.mePlace && ar.top[i].score == ar.meBest && ar.top[i].place == ar.mePlace ? dpWhite : dpText;

		DP_Text( x, y, DP_Wide( 22 ), va( "%i.", ar.top[i].place ), dpDim, DP_Font(), 0.55f, qfalse );
		DP_Text( x + DP_Wide( 22 ), y, w - DP_Wide( 80 ), ar.top[i].name, c, DP_Font(), 0.55f, qfalse );
		DP_Text( x + w - DP_Wide( 56 ), y, DP_Wide( 56 ), va( "%i", ar.top[i].score ), c, DP_Font(), 0.55f, qtrue );
		y += 12;
	}
	if ( ar.boardMsg[0] && y + 12 <= bottom ) {
		DP_Text( x, y, w, ar.boardMsg, dpDim, DP_Font(), 0.52f, qfalse );
		y += 12;
	}
	y += 4;
	if ( y + 12 <= bottom ) {
		if ( ar.mePlace )
			DP_Text( x, y, w, va( "You: #%i with %i", ar.mePlace, ar.meBest ), dpText, DP_Font(), 0.55f, qfalse );
		else if ( ar.best[ar.game] )
			DP_Text( x, y, w, va( "Your best here: %i. Link your game (/c1b3r link) to put it on the board", ar.best[ar.game] ), dpDim, DP_Font(), 0.5f, qfalse );
	}
}

static void AR_DrawTitle( const dpRect_t *s ) {
	const arGame_t *g = &arGames[ar.game];
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 16 );
	float y = s->y + DP_STATUS_H + 6;

	if ( DP_Button( x, y, DP_Wide( 50 ), "< Back", qfalse ) ) {
		GM_Sound( GM_SND_CLICK );
		ar.page = AP_LIST;
		return;
	}
	y += DP_CTRL_H + 6;
	DP_Text( s->x, y, s->w, g->title, dpText, DP_TitleFont(), 0.9f, qtrue );
	y += 26;
	for ( int i = 0; i < (int)ARRAY_LEN( g->rules ) && g->rules[i]; i++ ) {
		DP_Text( x, y, w, g->rules[i], dpDim, DP_Font(), 0.52f, qfalse );
		y += 11;
	}
	y += 6;
	if ( DP_Button( x + DP_Wide( 30 ), y, w - DP_Wide( 60 ), "Play", qfalse ) ) {
		GM_Sound( GM_SND_CLICK );
		AR_Start();
		return;
	}
	y += DP_CTRL_H + 12;
	AR_DrawBoard( x, y, w, s->y + s->h - 6 );
}

static void AR_DrawPlay( const dpRect_t *s ) {
	const arGame_t *g = &arGames[ar.game];
	const float x = s->x + DP_Wide( 6 ), w = s->w - DP_Wide( 12 );
	float y = s->y + DP_STATUS_H + 6;

	// step the run, 60 times a second; a hitch doesn't skip it ahead
	if ( !ar.paused && !ar.over ) {
		int dt = cls.realtime - ar.lastTime;

		dt = Com_Clampi( 0, 100, dt );
		if ( !ar.waiting ) {
			ar.acc += dt;
			while ( ar.acc >= AR_TICK_MS && !ar.over ) {
				ar.acc -= AR_TICK_MS;
				ar.playMs += AR_TICK_MS;
				g->tick();
			}
			if ( ar.over )
				AR_RunOver();
		}
	}
	ar.lastTime = cls.realtime;

	if ( DP_Button( x, y, DP_Wide( 50 ), "< Back", qfalse ) ) {
		GM_Sound( GM_SND_CLICK );
		ar.page = AP_TITLE;
		ar.playing = qfalse;
		AR_AskBoard( ar.game );
		return;
	}
	DP_Text( x + DP_Wide( 56 ), y + 2, w - DP_Wide( 56 ), va( "%s   %i   best %i", g->title, ar.score, Q_max( ar.best[ar.game], ar.score ) ),
		dpText, DP_Font(), 0.58f, qfalse );
	y += DP_CTRL_H + 6;

	// the field, at its own shape, as big as fits
	{
		const float h = s->y + s->h - 6 - y;
		const float wide = DP_Wide( 1.0f );
		const float scale = Q_min( ( w / wide ) / g->fw, h / g->fh );
		const float fw = g->fw * scale * wide, fh = g->fh * scale;

		ar.sx = scale * wide;
		ar.sy = scale;
		ar.fx = x + ( w - fw ) * 0.5f;
		ar.fy = y + ( h - fh ) * 0.5f;
		DP_Fill( ar.fx, ar.fy, fw, fh, arField );
		DP_Frame( ar.fx, ar.fy, fw, fh, dpGlow );
		g->draw();

		if ( ar.over ) {
			// centred on the field, however short it is (Droid Dash's)
			const float block = 24 + 24 + 14 + ( ar.runMsg[0] ? 12 : 0 ) + 8 + DP_CTRL_H + 18;
			float oy = ar.fy + Q_max( 4.0f, ( fh - block ) * 0.5f );
			const float bw = Q_min( fw * 0.4f, DP_Wide( 80 ) );

			DP_Fill( ar.fx, ar.fy, fw, fh, arShade );
			DP_Text( ar.fx, oy, fw, "Game over", dpText, DP_TitleFont(), 0.8f, qtrue );
			oy += 24;
			DP_Text( ar.fx, oy, fw, va( "%i", ar.score ), dpWhite, DP_TitleFont(), 0.8f, qtrue );
			oy += 24;
			DP_Text( ar.fx, oy, fw, ar.newBest ? "New best!" : va( "Your best: %i", ar.best[ar.game] ), ar.newBest ? dpWhite : dpDim, DP_Font(), 0.6f, qtrue );
			oy += 14;
			if ( ar.runMsg[0] ) {
				DP_Text( ar.fx + DP_Wide( 4 ), oy, fw - DP_Wide( 8 ), ar.runMsg, dpDim, DP_Font(), 0.52f, qtrue );
				oy += 12;
			}
			oy += 8;
			if ( DP_Button( ar.fx + fw * 0.5f - bw - DP_Wide( 3 ), oy, bw, "Again", qfalse ) ) {
				GM_Sound( GM_SND_CLICK );
				AR_Start();
			} else if ( DP_Button( ar.fx + fw * 0.5f + DP_Wide( 3 ), oy, bw, "Board", qfalse ) ) {
				GM_Sound( GM_SND_CLICK );
				ar.page = AP_TITLE;
				ar.playing = qfalse;
				AR_AskBoard( ar.game );
			}
			if ( ar.page == AP_PLAY && oy + DP_CTRL_H + 16 <= ar.fy + fh )
				DP_Text( ar.fx, oy + DP_CTRL_H + 6, fw, "Enter: again", dpDim, DP_Font(), 0.5f, qtrue );
		} else if ( ar.paused ) {
			DP_Fill( ar.fx, ar.fy, fw, fh, arShade );
			DP_Text( ar.fx, ar.fy + fh * 0.42f, fw, "Paused", dpText, DP_TitleFont(), 0.8f, qtrue );
			DP_Text( ar.fx, ar.fy + fh * 0.42f + 26, fw, "A key or a click goes on", dpDim, DP_Font(), 0.55f, qtrue );
			if ( DP_Clicked( ar.fx, ar.fy, fw, fh ) )
				ar.paused = qfalse;
		} else if ( DP_Clicked( ar.fx, ar.fy, fw, fh ) ) {
			g->key( A_MOUSE1 );
		}
	}
}

void CL_Arcade_Draw( const dpRect_t *s ) {
	if ( ar.page == AP_PLAY )
		AR_DrawPlay( s );
	else if ( ar.page == AP_TITLE )
		AR_DrawTitle( s );
	else
		AR_DrawList( s );
}

// off the datapad's screen (folded, put away, another app): a run waits
void CL_Arcade_Shown( qboolean shown ) {
	if ( !shown && ar.page == AP_PLAY && !ar.over && !ar.waiting )
		ar.paused = qtrue;
	ar.lastTime = cls.realtime;
	if ( shown && ar.page == AP_TITLE )
		AR_AskBoard( ar.game );
}

qboolean CL_Arcade_Key( int key ) {
	const qboolean gameKey = (qboolean)( AR_IsUp( key ) || AR_IsDown( key ) || AR_IsLeft( key ) || AR_IsRight( key ) || AR_IsAction( key )
		|| key == A_LOW_P || key == A_CAP_P );

	if ( ar.page == AP_TITLE ) {
		if ( AR_IsAction( key ) ) {
			AR_Start();
			return qtrue;
		}
		return qfalse;
	}
	if ( ar.page != AP_PLAY )
		return qfalse;
	if ( ar.over ) {
		// a moment first, so the key that ended it doesn't start another
		if ( ( key == A_ENTER || key == A_KP_ENTER || key == A_SPACE ) && cls.realtime - ar.overTime > 600 )
			AR_Start();
		return gameKey;
	}
	if ( ar.paused ) {
		ar.paused = qfalse;
		ar.lastTime = cls.realtime;
		return qtrue;
	}
	if ( key == A_LOW_P || key == A_CAP_P ) {
		if ( !ar.waiting )
			ar.paused = qtrue;
		return qtrue;
	}
	arGames[ar.game].key( key );
	return gameKey;
}

// datapad arcade [game [play]]: on its list, a game's page, or a run of it started
void CL_Arcade_Open( const char *game, qboolean play ) {
	for ( int i = 0; game && game[0] && i < AR_NUM; i++ ) {
		if ( !Q_stricmp( game, arGames[i].word ) || !Q_stricmp( game, arGames[i].title ) ) {
			AR_OpenGame( i );
			if ( play )
				AR_Start();
			return;
		}
	}
	if ( ar.page != AP_PLAY )
		ar.page = AP_LIST;
}

void CL_Arcade_Init( void ) {
	memset( &ar, 0, sizeof( ar ) );
	ar.boardGame = -1;
	cl_arcadeBest = Cvar_Get( "cl_arcadeBest", "0 0 0 0", CVAR_ARCHIVE_ND, "Your best scores in the datapad Arcade: Sarlacc, Droid Dash, Skyhopper, Bot Blaster" );
	AR_LoadBests();
}
