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

// cl_rpcinematic.cpp -- a cinematic camera, driven by hooks in RPMod's prints
//
// An admin's rpnotify -1 1 cmd:cin/<verb>/<parts...> takes every player with this
// client from their own view to a camera the story moves around (see cl_rphooks.cpp
// for how the hooks travel). A spot is "x y z [pitch yaw [roll [fov]]]"; a fov is
// cg_fov's, horizontal on a 4:3 screen, so every screen frames the same.
//
//   cmd:cin/start[/hud][/bars]      the camera takes over where the player looks from;
//                                   the HUD is hidden unless hud, bars slide in with bars
//   cmd:cin/path[/linear][/look <x y z>]/<spot>[/<seconds>/<spot>...]
//                                   a shot: the camera cuts to the first spot, then glides
//                                   through the others, each the seconds before it after
//                                   the one before; look keeps it turned to the point
//   cmd:cin/move/<seconds>[/linear]/<spot>[/<seconds>/<spot>...]
//                                   the same from where the camera is; spots without their
//                                   own seconds share the first out
//                                   Before the spots, a path or orbit can also have
//                                   dof <focus> <range> <blur>: what's focus away is sharp,
//                                   give or take range, the rest blurred up to blur (1-10);
//                                   focus 0 is whatever the camera looks at
//                                   in|out <cut|fade|left|right|top|bottom> <seconds> <r g b>:
//                                   the shot fades in from the colour, or is wiped in from
//                                   that side, and out to it at its end; the screen stays
//                                   that colour after an out until the next shot
//   cmd:cin/cut/<spot>              the camera jumps there
//   cmd:cin/orbit/<seconds>[/linear]/<x y z>/<radius>/<height>/<from yaw>/<degrees>[/<fov>[/<end fov>]]
//                                   circles the point looking at it, from the yaw it is
//                                   seen from, round by the degrees (negative goes the
//                                   other way, 720 is twice round); one fov zooms to it
//                                   from where it is, two go from the first to the second
//   cmd:cin/lookat/<x y z>|off      keeps the camera turned to the point
//   cmd:cin/fov/<fov>[/<seconds>]   zooms
//   cmd:cin/bars/on|off[/<seconds>] the black bars, top and bottom
//   cmd:cin/hud/on|off              the HUD; full screen fades and overlays always show
//   cmd:cin/end                     back to the player's own view
//
// Camera commands start a cinematic that hasn't been. rpc_cinematics 0 leaves them all
// out, and /cincam stop leaves the one playing. The server still only sends what is
// near each player, so things far from them may be missing in a shot of somewhere else.
//
// Shots are made in the camera editor, /cincam edit: a free camera that shows each
// spot's view, the trail between them and a camera running along it. They are saved in
// camshots/<name>.cam as their path or orbit hook, which /cincam play and the
// Storyteller's Camera shot send. NPCs walking in the shot follow it there as
//   walk <npc> <route> walk|run <seconds into the shot>
// lines, sent by whoever plays the shot (npcmanager walk, see cl_npcwalk.cpp) and walked
// on the editor's screen in its preview.

#include "client.h"
#include "qcommon/cm_public.h"

#define RC_MAX_KEYS			16
#define RC_BAR_FRAC			0.12f		// of the screen's height, each
#define RC_BAR_MS			1000
#define RC_DEFAULT_FOV		80.0f

#define RC_SHOT_DIR			"camshots"	// in the game folder
#define RC_SHOT_EXT			".cam"
#define RC_MAX_HOOK			900			// RPMod passes the line on in a print
#define RC_SHOT_HEADER		"// camera shot, made with /cincam edit\n"
#define RC_MAX_WALKS		4			// NPCs walking in a shot
#define RC_MAX_QUEUED		16

// an NPC walking a route while a shot plays
typedef struct {
	char		npc[MAX_QPATH];		// targetname or number, commas between several
	char		route[MAX_QPATH];
	qboolean	run;
	int			delay;				// ms into the shot it sets off
} rcWalk_t;

// walks of a shot played, each run when its time comes and, sent, when the server takes it
static struct {
	char		cmd[MAX_STRING_CHARS];
	int			at;					// cls.realtime
	qboolean	local;
} rcQueue[RC_MAX_QUEUED];
static int rcNumQueued;

typedef struct {
	vec3_t	org;
	vec3_t	ang;
	float	fov;		// cg_fov's; 0 is as the key before
	int		ms;			// from the key before
} rcKey_t;

// how a shot comes in or goes: cut, faded from or to a colour, or wiped across from a side
typedef enum {
	RC_TR_CUT,
	RC_TR_FADE,
	RC_TR_LEFT,
	RC_TR_RIGHT,
	RC_TR_TOP,
	RC_TR_BOTTOM,
	RC_NUM_TRANSITIONS
} rcTransType_t;

static const char *rcTransWords[RC_NUM_TRANSITIONS] = { "cut", "fade", "left", "right", "top", "bottom" };

typedef struct {
	int			type;			// rcTransType_t
	int			ms;
	byte		color[3];
} rcTrans_t;

#define RC_DOF_RANGE		128.0f		// what a new depth of field keeps sharp, either side
#define RC_DOF_BLUR			4.0f
#define RC_DOF_MAX_BLUR		10.0f
#define RC_DOF_FOCUS_RATE	6.0f		// an automatic focus follows what's looked at this quickly
#define RC_TRANS_MS			1000
#define RC_WIPE_SOFT		0.06f		// a wipe's edge, soft over this much of the screen

typedef struct {
	qboolean	orbit;
	qboolean	linear;

	// depth of field
	qboolean	dof;
	float		dofFocus;		// 0 is what the camera looks at
	float		dofRange, dofBlur;

	// coming in, going out
	rcTrans_t	in, out;

	// a path
	rcKey_t		keys[RC_MAX_KEYS + 1];		// a move's keys[0] is where it set off from
	int			numKeys;
	qboolean	look;
	vec3_t		lookAt;

	// an orbit
	vec3_t		center;
	float		radius, height, fromYaw, degrees;
	float		fov, endFov;
	int			ms;
} rcShot_t;

typedef struct {
	qboolean	active;
	qboolean	showHud;

	// where the camera is, worked out once a frame
	vec3_t		org, ang;
	float		fovY;
	int			frame;

	// a shot playing, or where it holds
	qboolean	playing;
	rcShot_t	shot;
	int			start;
	qboolean	shotFov;		// the shot zooms, until a fov hook says otherwise
	vec3_t		holdOrg, holdAng;

	// the lookat hook
	qboolean	lookAt;
	vec3_t		lookTarget;

	float		fovFrom, fovTo;
	int			fovStart, fovDuration;

	// the last shot's transitions, its out holding after it; qfalse after a cut
	qboolean	trans;

	// the depth of field, until another shot; dofNow is where an automatic one has got to
	qboolean	dof;
	float		dofFocus, dofRange, dofBlur;
	float		dofNow;
	int			dofTime;

	// the bars, on their own as well
	qboolean	bars;
	float		barsFrom;
	int			barsStart, barsDuration;

	// the player's own view, the last the cgame drew
	qboolean	haveView;
	vec3_t		viewOrg, viewAng;
	float		viewFovY;
} rcState_t;

static rcState_t rc;

static cvar_t *rpc_cinematics;
static cvar_t *rpc_cinThirdPerson;		// 1 while cg_thirdPerson is ours to put back to 0, a crash included

static qboolean CE_Open( void );
static char *CE_TypedText( int *size );
static qboolean RC_DepthOfField( const vec3_t org, const vec3_t ang, qboolean dof, float focus, float *now, int *time,
	float range, float blur );
static void RC_NoDepthOfField( void );

// the cgame only draws the player's own body in third person; the camera is ours either way
static void RC_ThirdPerson( qboolean on ) {
	if ( !rpc_cinThirdPerson )
		return;		// a disconnect before the client is up
	if ( on ) {
		if ( Cvar_VariableIntegerValue( "cg_thirdPerson" ) || rpc_cinThirdPerson->integer )
			return;
		Cvar_Set( "rpc_cinThirdPerson", "1" );
		Cvar_Set( "cg_thirdPerson", "1" );
	} else if ( rpc_cinThirdPerson->integer ) {
		Cvar_Set( "cg_thirdPerson", "0" );
		Cvar_Set( "rpc_cinThirdPerson", "0" );
	}
}

static void RC_SyncThirdPerson( void ) {
	RC_ThirdPerson( (qboolean)( rc.active || CE_Open() ) );
}

static float RC_Ease( float t, qboolean linear ) {
	t = Com_Clamp( 0.0f, 1.0f, t );
	return linear ? t : t * t * ( 3.0f - 2.0f * t );
}

// a fov as cg_fov gives it, horizontal on 4:3, as a vertical one any screen can use
static float RC_FovY( float fovX ) {
	fovX = Com_Clamp( 1.0f, 170.0f, fovX );
	return RAD2DEG( 2.0f * atanf( tanf( DEG2RAD( fovX * 0.5f ) ) * 0.75f ) );
}

static float RC_FovX( float fovY, float width, float height ) {
	return RAD2DEG( 2.0f * atanf( tanf( DEG2RAD( fovY * 0.5f ) ) * width / height ) );
}

static int RC_Round( float v ) {
	return (int)floorf( v + 0.5f );
}

/*
===============================================================================

Shots

===============================================================================
*/

static float RC_CatmullRom( float p0, float p1, float p2, float p3, float t ) {
	return 0.5f * ( 2.0f * p1 + ( p2 - p0 ) * t + ( 2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 ) * t * t
		+ ( 3.0f * p1 - p0 - 3.0f * p2 + p3 ) * t * t * t );
}

static int RC_ShotMs( const rcShot_t *s ) {
	int ms = 0;

	if ( s->orbit )
		return s->ms;
	for ( int i = 1; i < s->numKeys; i++ )
		ms += s->keys[i].ms;
	return ms;
}

// where the shot's camera is, ms into it; the easing is over the whole shot
static void RC_EvalShot( const rcShot_t *s, int ms, vec3_t org, vec3_t ang, float *fov ) {
	const int total = RC_ShotMs( s );
	const float t = total > 0 ? RC_Ease( (float)ms / total, s->linear ) : 1.0f;

	if ( s->orbit ) {
		const float yaw = DEG2RAD( s->fromYaw + s->degrees * t );
		vec3_t dir;

		org[0] = s->center[0] + cosf( yaw ) * s->radius;
		org[1] = s->center[1] + sinf( yaw ) * s->radius;
		org[2] = s->center[2] + s->height;
		VectorSubtract( s->center, org, dir );
		vectoangles( dir, ang );
		ang[ROLL] = 0;
		*fov = s->fov + ( s->endFov - s->fov ) * t;
		return;
	}

	if ( !s->numKeys ) {
		VectorClear( org );
		VectorClear( ang );
		*fov = RC_DEFAULT_FOV;
		return;
	}
	if ( s->numKeys == 1 || total <= 0 ) {
		const rcKey_t *k = &s->keys[s->numKeys - 1];

		VectorCopy( k->org, org );
		VectorCopy( k->ang, ang );
		*fov = k->fov;
	} else {
		// a smooth curve through the keys, each stretch as long as its key says
		const float u = t * total;
		float from = 0;
		int i;

		for ( i = 1; i < s->numKeys - 1 && u > from + s->keys[i].ms; i++ )
			from += s->keys[i].ms;
		const float local = s->keys[i].ms > 0 ? Com_Clamp( 0.0f, 1.0f, ( u - from ) / s->keys[i].ms ) : 1.0f;
		const rcKey_t *k0 = &s->keys[Q_max( i - 2, 0 )], *k1 = &s->keys[i - 1], *k2 = &s->keys[i];
		const rcKey_t *k3 = &s->keys[Q_min( i + 1, s->numKeys - 1 )];

		for ( int j = 0; j < 3; j++ ) {
			org[j] = RC_CatmullRom( k0->org[j], k1->org[j], k2->org[j], k3->org[j], local );
			ang[j] = RC_CatmullRom( k0->ang[j], k1->ang[j], k2->ang[j], k3->ang[j], local );
		}
		*fov = RC_CatmullRom( k0->fov, k1->fov, k2->fov, k3->fov, local );
	}

	if ( s->look ) {
		const float roll = ang[ROLL];
		vec3_t dir;

		VectorSubtract( s->lookAt, org, dir );
		if ( VectorLength( dir ) > 1.0f ) {
			vectoangles( dir, ang );
			ang[ROLL] = roll;
		}
	}
}

// fovs not given are the one before's, the first's the camera's
static void RC_FillFov( rcShot_t *s, float fov ) {
	if ( s->orbit ) {
		if ( s->fov <= 0 )
			s->fov = fov;
		if ( s->endFov <= 0 )
			s->endFov = s->fov;
		return;
	}
	for ( int i = 0; i < s->numKeys; i++ ) {
		if ( s->keys[i].fov <= 0 )
			s->keys[i].fov = i ? s->keys[i - 1].fov : fov;
	}
}

static int RC_SecondsMs( float seconds ) {
	return Q_max( 0, (int)( seconds * 1000.0f + 0.5f ) );
}

static int RC_Ms( const char *seconds ) {
	return RC_SecondsMs( (float)atof( seconds ) );
}

static qboolean RC_IsWord( const char *s ) {
	return (qboolean)( isalpha( (unsigned char)s[0] ) != 0 );
}

// a shot's option word: linear, look, dof, in, out; qtrue if it was one of them
static qboolean RC_ParseOption( const char *p, rcShot_t *s ) {
	if ( !Q_stricmp( p, "linear" ) ) {
		s->linear = qtrue;
		return qtrue;
	}
	if ( !Q_stricmpn( p, "look ", 5 ) ) {
		if ( sscanf( p + 5, "%f %f %f", &s->lookAt[0], &s->lookAt[1], &s->lookAt[2] ) == 3 )
			s->look = qtrue;
		return qtrue;
	}
	if ( !Q_stricmpn( p, "dof ", 4 ) ) {
		float v[3];

		if ( sscanf( p + 4, "%f %f %f", &v[0], &v[1], &v[2] ) == 3 && v[2] > 0.0f ) {
			s->dof = qtrue;
			s->dofFocus = Q_max( 0.0f, v[0] );
			s->dofRange = Q_max( 1.0f, v[1] );
			s->dofBlur = Com_Clamp( 0.0f, RC_DOF_MAX_BLUR, v[2] );
		}
		return qtrue;
	}
	if ( !Q_stricmpn( p, "in ", 3 ) || !Q_stricmpn( p, "out ", 4 ) ) {
		rcTrans_t *tr = !Q_stricmpn( p, "in ", 3 ) ? &s->in : &s->out;
		char word[16];
		float seconds = 0;
		int r = 0, g = 0, b = 0;

		if ( sscanf( strchr( p, ' ' ) + 1, "%15s %f %i %i %i", word, &seconds, &r, &g, &b ) >= 2 ) {
			for ( int i = 0; i < RC_NUM_TRANSITIONS; i++ ) {
				if ( !Q_stricmp( word, rcTransWords[i] ) )
					tr->type = i;
			}
			tr->ms = RC_SecondsMs( seconds );
			tr->color[0] = (byte)Com_Clampi( 0, 255, r );
			tr->color[1] = (byte)Com_Clampi( 0, 255, g );
			tr->color[2] = (byte)Com_Clampi( 0, 255, b );
			if ( !tr->ms )
				tr->type = RC_TR_CUT;
		}
		return qtrue;
	}
	return qfalse;
}

// the options as a hook writes them, each after a /
static void RC_WriteOptions( const rcShot_t *s, char *out, int size ) {
	const rcTrans_t *trs[2] = { &s->in, &s->out };

	if ( s->dof )
		Q_strcat( out, size, va( "/dof %i %i %g", RC_Round( s->dofFocus ), RC_Round( s->dofRange ), s->dofBlur ) );
	for ( int i = 0; i < 2; i++ ) {
		const rcTrans_t *tr = trs[i];

		if ( tr->type != RC_TR_CUT && tr->ms > 0 )
			Q_strcat( out, size, va( "/%s %s %g %i %i %i", i ? "out" : "in", rcTransWords[tr->type], floorf( tr->ms / 100.0f + 0.5f ) / 10.0f,
				tr->color[0], tr->color[1], tr->color[2] ) );
	}
}

// angles turned the short way round from the spot before, so a curve through them doesn't spin
static void RC_Unwrap( rcKey_t *key, const rcKey_t *prev ) {
	for ( int j = 0; j < 3; j++ )
		key->ang[j] = prev->ang[j] + AngleNormalize180( key->ang[j] - prev->ang[j] );
}

// the parts of a path or a move, onto the keys there are: [<seconds>] [linear] [look <x y z>],
// then the spots, each after its own seconds if it has them; spots without share the first
// seconds out. A spot without angles keeps the one before's, the first's ang
static void RC_ParseKeys( char **argv, int argc, rcShot_t *s, const vec3_t ang ) {
	const int first = s->numKeys;
	float lead = -1, own = -1;
	int without = 0;

	for ( int i = 0; i < argc; i++ ) {
		const char *p = argv[i];
		float v[7];
		int n;

		if ( RC_IsWord( p ) ) {
			RC_ParseOption( p, s );
			continue;
		}
		n = sscanf( p, "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6] );
		if ( n == 1 ) {
			if ( s->numKeys == first && lead < 0 )
				lead = v[0];
			else
				own = v[0];
			continue;
		}
		if ( n < 3 || s->numKeys > RC_MAX_KEYS )
			continue;

		rcKey_t *key = &s->keys[s->numKeys];
		const rcKey_t *prev = s->numKeys ? &s->keys[s->numKeys - 1] : NULL;

		VectorSet( key->org, v[0], v[1], v[2] );
		VectorCopy( prev ? prev->ang : ang, key->ang );
		if ( n >= 5 ) {
			key->ang[PITCH] = v[3];
			key->ang[YAW] = v[4];
			key->ang[ROLL] = n >= 6 ? v[5] : 0;
		}
		key->fov = n >= 7 ? v[6] : 0;
		key->ms = own >= 0 ? RC_SecondsMs( own ) : -1;
		if ( key->ms < 0 && s->numKeys )
			without++;
		own = -1;
		if ( prev )
			RC_Unwrap( key, prev );
		s->numKeys++;
	}
	if ( s->numKeys )
		s->keys[0].ms = 0;
	for ( int i = 1; i < s->numKeys; i++ ) {
		if ( s->keys[i].ms < 0 )
			s->keys[i].ms = without && lead > 0 ? RC_SecondsMs( lead / without ) : 0;
	}
}

// <seconds>[/linear]/<x y z>/<radius>/<height>/<from yaw>/<degrees>[/<fov>[/<end fov>]]
static qboolean RC_ParseOrbit( char **argv, int argc, rcShot_t *s ) {
	int part = 1;

	if ( argc < 1 )
		return qfalse;
	s->orbit = qtrue;
	s->ms = RC_Ms( argv[0] );
	while ( part < argc && RC_IsWord( argv[part] ) ) {
		RC_ParseOption( argv[part], s );
		part++;
	}
	if ( argc - part < 5 || sscanf( argv[part], "%f %f %f", &s->center[0], &s->center[1], &s->center[2] ) != 3 )
		return qfalse;
	s->radius = (float)atof( argv[part + 1] );
	s->height = (float)atof( argv[part + 2] );
	s->fromYaw = (float)atof( argv[part + 3] );
	s->degrees = (float)atof( argv[part + 4] );
	if ( argc - part > 6 && argv[part + 6][0] ) {
		s->fov = (float)atof( argv[part + 5] );
		s->endFov = (float)atof( argv[part + 6] );
	} else if ( argc - part > 5 && argv[part + 5][0] ) {
		s->endFov = (float)atof( argv[part + 5] );	// from where the camera is
	}
	return qtrue;
}

// a path or an orbit hook's parts after cin, as a shot; fovs not given are left 0
static qboolean RC_ParseShot( char **argv, int argc, rcShot_t *s, const vec3_t ang ) {
	memset( s, 0, sizeof( *s ) );
	if ( argc < 2 )
		return qfalse;
	if ( !Q_stricmp( argv[0], "orbit" ) )
		return RC_ParseOrbit( argv + 1, argc - 1, s );
	if ( Q_stricmp( argv[0], "path" ) )
		return qfalse;
	RC_ParseKeys( argv + 1, argc - 1, s, ang );
	return (qboolean)( s->numKeys > 0 );
}

static float RC_Seconds( int ms ) {
	return floorf( ms / 100.0f + 0.5f ) / 10.0f;
}

// the hook that plays the shot, rounded as it goes out
static void RC_WriteShot( const rcShot_t *s, char *out, int size ) {
	if ( s->orbit ) {
		Com_sprintf( out, size, "cin/orbit/%g%s", RC_Seconds( s->ms ), s->linear ? "/linear" : "" );
		RC_WriteOptions( s, out, size );
		Q_strcat( out, size, va( "/%i %i %i/%i/%i/%i/%i/%i/%i",
			RC_Round( s->center[0] ), RC_Round( s->center[1] ), RC_Round( s->center[2] ), RC_Round( s->radius ), RC_Round( s->height ),
			RC_Round( AngleNormalize360( s->fromYaw ) ), RC_Round( s->degrees ), RC_Round( s->fov ), RC_Round( s->endFov ) ) );
		return;
	}
	Com_sprintf( out, size, "cin/path%s", s->linear ? "/linear" : "" );
	RC_WriteOptions( s, out, size );
	if ( s->look )
		Q_strcat( out, size, va( "/look %i %i %i", RC_Round( s->lookAt[0] ), RC_Round( s->lookAt[1] ), RC_Round( s->lookAt[2] ) ) );
	for ( int i = 0; i < s->numKeys; i++ ) {
		const rcKey_t *k = &s->keys[i];

		if ( i )
			Q_strcat( out, size, va( "/%g", RC_Seconds( k->ms ) ) );
		Q_strcat( out, size, va( "/%i %i %i %i %i %i %i", RC_Round( k->org[0] ), RC_Round( k->org[1] ), RC_Round( k->org[2] ),
			RC_Round( AngleNormalize180( k->ang[PITCH] ) ), RC_Round( AngleNormalize360( k->ang[YAW] ) ),
			RC_Round( AngleNormalize180( k->ang[ROLL] ) ), RC_Round( k->fov ) ) );
	}
}

// a hook cut into its parts at the /, spaces round them trimmed, as cl_rphooks hands them over
static int RC_SplitHook( char *hook, char **argv, int max ) {
	char *p = hook;
	int argc = 0;

	while ( p && argc < max ) {
		char *end = strchr( p, '/' );

		if ( end )
			*end = '\0';
		while ( *p == ' ' )
			p++;
		for ( char *t = p + strlen( p ); t > p && ( t[-1] == ' ' || t[-1] == '\r' || t[-1] == '\n' ); )
			*--t = '\0';
		argv[argc++] = p;
		p = end ? end + 1 : NULL;
	}
	return argc;
}

/*
===============================================================================

The camera

===============================================================================
*/

static float RC_Fov( void ) {
	if ( !rc.fovDuration || cls.realtime - rc.fovStart >= rc.fovDuration )
		return rc.fovTo;
	return rc.fovFrom + ( rc.fovTo - rc.fovFrom ) * RC_Ease( (float)( cls.realtime - rc.fovStart ) / rc.fovDuration, qfalse );
}

static void RC_Update( void ) {
	if ( rc.frame == cls.framecount )
		return;
	rc.frame = cls.framecount;

	if ( rc.playing ) {
		float fov;

		RC_EvalShot( &rc.shot, cls.realtime - rc.start, rc.org, rc.ang, &fov );
		rc.fovY = rc.shotFov ? RC_FovY( fov ) : RC_Fov();
	} else {
		VectorCopy( rc.holdOrg, rc.org );
		VectorCopy( rc.holdAng, rc.ang );
		rc.fovY = RC_Fov();
	}

	if ( rc.lookAt && !( rc.playing && ( rc.shot.orbit || rc.shot.look ) ) ) {
		const float roll = rc.ang[ROLL];
		vec3_t dir;

		VectorSubtract( rc.lookTarget, rc.org, dir );
		if ( VectorLength( dir ) > 1.0f ) {
			vectoangles( dir, rc.ang );
			rc.ang[ROLL] = roll;
		}
	}
}

// the fov a shot starts from, as cg_fov gives it
static float RC_CurrentFov( void ) {
	return RC_FovX( rc.fovY, 4.0f, 3.0f );
}

// the camera holds still where it is now
static void RC_Hold( void ) {
	RC_Update();
	if ( rc.playing && rc.shotFov ) {
		rc.fovTo = rc.fovY;
		rc.fovDuration = 0;
	}
	rc.playing = qfalse;
	rc.shotFov = qfalse;
	VectorCopy( rc.org, rc.holdOrg );
	VectorCopy( rc.ang, rc.holdAng );
	rc.frame = -1;
}

static void RC_Play( const rcShot_t *shot ) {
	rc.shot = *shot;
	rc.playing = qtrue;
	rc.shotFov = qtrue;
	rc.start = cls.realtime;
	rc.frame = -1;
	// its transitions, and its focus until another shot
	rc.trans = qtrue;
	if ( rc.dof != shot->dof || ( shot->dof && shot->dofFocus > 0.0f ) )
		rc.dofNow = shot->dofFocus;
	rc.dof = shot->dof;
	rc.dofFocus = shot->dofFocus;
	rc.dofRange = shot->dofRange;
	rc.dofBlur = shot->dofBlur;
}

static void RC_ZoomTo( float fovY, int ms ) {
	rc.fovFrom = RC_Fov();
	rc.fovTo = fovY;
	rc.fovStart = cls.realtime;
	rc.fovDuration = ms;
}

static float RC_BarsNow( void ) {
	const float to = rc.bars ? 1.0f : 0.0f;

	if ( !rc.barsDuration || cls.realtime - rc.barsStart >= rc.barsDuration )
		return to;
	return rc.barsFrom + ( to - rc.barsFrom ) * RC_Ease( (float)( cls.realtime - rc.barsStart ) / rc.barsDuration, qfalse );
}

// the bars slide from wherever they are, halfway there too
static void RC_Bars( qboolean on, int ms ) {
	rc.barsFrom = RC_BarsNow();
	rc.bars = on;
	rc.barsStart = cls.realtime;
	rc.barsDuration = ms;
}

static void RC_Start( void ) {
	if ( rc.active )
		return;
	rc.active = qtrue;
	rc.showHud = qfalse;
	rc.lookAt = qfalse;
	rc.playing = qfalse;
	rc.shotFov = qfalse;
	rc.trans = qfalse;
	rc.dof = qfalse;
	rc.frame = -1;
	if ( rc.haveView ) {
		VectorCopy( rc.viewOrg, rc.holdOrg );
		VectorCopy( rc.viewAng, rc.holdAng );
		rc.fovTo = rc.viewFovY;
	} else {
		VectorCopy( cl.snap.ps.origin, rc.holdOrg );
		rc.holdOrg[2] += cl.snap.ps.viewheight;
		VectorCopy( cl.viewangles, rc.holdAng );
		rc.fovTo = RC_FovY( RC_DEFAULT_FOV );
	}
	rc.fovDuration = 0;
	RC_SyncThirdPerson();
}

void CL_RpCinematic_Stop( void ) {
	if ( rc.bars )
		RC_Bars( qfalse, RC_BAR_MS / 2 );
	rc.active = qfalse;
	rc.playing = qfalse;
	rc.trans = qfalse;
	rc.dof = qfalse;
	RC_SyncThirdPerson();
}

/*
===============================================================================

Hooks

===============================================================================
*/

// cmd:cin/<verb>/<parts...>, the parts after cin; the settings aren't asked
static void RC_Hook( char **argv, int argc ) {
	const char *verb = argc ? argv[0] : "";
	char **parts = argv + 1;
	const int numParts = argc - 1;
	rcShot_t shot;

	if ( !Q_stricmp( verb, "start" ) ) {
		qboolean bars = qfalse;

		RC_Start();
		for ( int i = 0; i < numParts; i++ ) {
			if ( !Q_stricmp( parts[i], "hud" ) )
				rc.showHud = qtrue;
			else if ( !Q_stricmp( parts[i], "bars" ) )
				bars = qtrue;
		}
		if ( bars )
			RC_Bars( qtrue, RC_BAR_MS );
	} else if ( !Q_stricmp( verb, "end" ) ) {
		CL_RpCinematic_Stop();
	} else if ( !Q_stricmp( verb, "cut" ) ) {
		if ( !numParts )
			return;
		RC_Start();
		RC_Hold();
		memset( &shot, 0, sizeof( shot ) );
		RC_ParseKeys( parts, 1, &shot, rc.ang );
		if ( !shot.numKeys )
			return;
		VectorCopy( shot.keys[0].org, rc.holdOrg );
		VectorCopy( shot.keys[0].ang, rc.holdAng );
		rc.trans = qfalse;
		if ( shot.keys[0].fov > 0 )
			RC_ZoomTo( RC_FovY( shot.keys[0].fov ), 0 );
		rc.frame = -1;
	} else if ( !Q_stricmp( verb, "move" ) ) {
		if ( numParts < 2 )
			return;
		RC_Start();
		RC_Hold();
		memset( &shot, 0, sizeof( shot ) );
		VectorCopy( rc.org, shot.keys[0].org );
		VectorCopy( rc.ang, shot.keys[0].ang );
		shot.keys[0].fov = RC_CurrentFov();
		shot.numKeys = 1;
		RC_ParseKeys( parts, numParts, &shot, rc.ang );
		if ( shot.numKeys < 2 )
			return;
		RC_FillFov( &shot, shot.keys[0].fov );
		RC_Play( &shot );
	} else if ( !Q_stricmp( verb, "path" ) || !Q_stricmp( verb, "orbit" ) ) {
		RC_Start();
		RC_Hold();
		if ( !RC_ParseShot( argv, argc, &shot, rc.ang ) )
			return;
		RC_FillFov( &shot, RC_CurrentFov() );
		// a path is a shot of its own, it looks where it says
		if ( !shot.orbit )
			rc.lookAt = qfalse;
		RC_Play( &shot );
	} else if ( !Q_stricmp( verb, "lookat" ) ) {
		if ( !numParts || !Q_stricmp( parts[0], "off" ) ) {
			RC_Hold();
			rc.lookAt = qfalse;
		} else if ( sscanf( parts[0], "%f %f %f", &rc.lookTarget[0], &rc.lookTarget[1], &rc.lookTarget[2] ) == 3 ) {
			RC_Start();
			rc.lookAt = qtrue;
			rc.frame = -1;
		}
	} else if ( !Q_stricmp( verb, "fov" ) ) {
		if ( !numParts )
			return;
		RC_Start();
		RC_Update();
		if ( rc.shotFov ) {
			rc.fovTo = rc.fovY;
			rc.fovDuration = 0;
			rc.shotFov = qfalse;
		}
		RC_ZoomTo( RC_FovY( (float)atof( parts[0] ) ), numParts > 1 ? RC_Ms( parts[1] ) : 0 );
		rc.frame = -1;
	} else if ( !Q_stricmp( verb, "bars" ) ) {
		RC_Bars( (qboolean)( !numParts || Q_stricmp( parts[0], "off" ) ), numParts > 1 ? RC_Ms( parts[1] ) : RC_BAR_MS );
	} else if ( !Q_stricmp( verb, "hud" ) ) {
		rc.showHud = (qboolean)( !numParts || Q_stricmp( parts[0], "off" ) );
	} else {
		Com_DPrintf( "RP hook: cin wants start, path, move, cut, orbit, lookat, fov, bars, hud or end, not \"%s\"\n", verb );
	}
}

void CL_RpCinematic_Hook( char **argv, int argc ) {
	if ( !rpc_cinematics->integer || cls.state != CA_ACTIVE )
		return;
	RC_Hook( argv, argc );
}

// a whole cin/... hook played on this screen only
static void RC_RunLocal( const char *hook ) {
	char buf[MAX_STRING_CHARS], *argv[RC_MAX_KEYS * 2 + 8];
	int argc;

	Q_strncpyz( buf, hook, sizeof( buf ) );
	argc = RC_SplitHook( buf, argv, ARRAY_LEN( argv ) );
	if ( argc > 1 )
		RC_Hook( argv + 1, argc - 1 );
}

/*
===============================================================================

Shot files

===============================================================================
*/

static qboolean RC_ShotNameChar( int ch ) {
	return (qboolean)( isalnum( ch ) || ch == '_' || ch == '-' );
}

static const char *RC_ShotPath( const char *name ) {
	return va( "%s/%s%s", RC_SHOT_DIR, name, RC_SHOT_EXT );
}

// the Storyteller asks for its steps' shots every frame: the files read, kept until one is saved
#define RC_SHOT_CACHE	16

static struct {
	char	name[MAX_QPATH];
	char	hook[RC_MAX_HOOK + 1];
	int		ms;
	int		used;
} rcShotCache[RC_SHOT_CACHE];

static int rcShotCacheUse;

static const char *RC_WalkCommand( const rcWalk_t *w, qboolean local ) {
	return va( "npcmanager walk %s %s %s%s", w->npc, w->route, w->run ? "run" : "walk", local ? " local" : "" );
}

// the shot's walks set off, each its delay from now; local ones on this screen only
static void RC_QueueWalks( const rcWalk_t *walks, int count, qboolean local ) {
	for ( int i = 0; i < count && rcNumQueued < RC_MAX_QUEUED; i++ ) {
		if ( !walks[i].npc[0] || !walks[i].route[0] )
			continue;
		Q_strncpyz( rcQueue[rcNumQueued].cmd, RC_WalkCommand( &walks[i], local ), sizeof( rcQueue[0].cmd ) );
		rcQueue[rcNumQueued].at = cls.realtime + walks[i].delay;
		rcQueue[rcNumQueued].local = local;
		rcNumQueued++;
	}
}

// the walks due: local ones at once, the others one at a time as the server takes them
static void RC_RunQueue( void ) {
	for ( int i = 0; i < rcNumQueued; ) {
		if ( cls.realtime < rcQueue[i].at || ( !rcQueue[i].local && ( cls.state != CA_ACTIVE || !CL_NpcWalk_CanSend() ) ) ) {
			i++;
			continue;
		}
		Cmd_ExecuteString( rcQueue[i].cmd );
		rcNumQueued--;
		memmove( &rcQueue[i], &rcQueue[i + 1], ( rcNumQueued - i ) * sizeof( rcQueue[0] ) );
	}
}

// a preview's walks called off: the ones still to go, and the NPCs walking back where they are
static void RC_StopLocalWalks( const rcWalk_t *walks, int count ) {
	for ( int i = 0; i < rcNumQueued; ) {
		if ( rcQueue[i].local ) {
			rcNumQueued--;
			memmove( &rcQueue[i], &rcQueue[i + 1], ( rcNumQueued - i ) * sizeof( rcQueue[0] ) );
		} else {
			i++;
		}
	}
	for ( int i = 0; i < count; i++ ) {
		if ( walks[i].npc[0] && cls.state == CA_ACTIVE )
			Cmd_ExecuteString( va( "npcmanager walk %s stop local", walks[i].npc ) );
	}
}

// a shot file's walk lines: walk <npc> <route> walk|run <seconds>
static int RC_ReadWalks( const char *name, rcWalk_t *walks, int max ) {
	char *buf, *line, *next;
	int count = 0;

	if ( FS_ReadFile( RC_ShotPath( name ), (void **)&buf ) <= 0 || !buf )
		return 0;
	for ( line = buf; line && *line && count < max; line = next ) {
		char npc[MAX_QPATH], route[MAX_QPATH], pace[16];
		float seconds = 0;

		next = strchr( line, '\n' );
		if ( next )
			*next++ = '\0';
		if ( Q_stricmpn( line, "walk ", 5 ) || sscanf( line + 5, "%63s %63s %15s %f", npc, route, pace, &seconds ) < 2 )
			continue;
		Q_strncpyz( walks[count].npc, npc, sizeof( walks[0].npc ) );
		Q_strncpyz( walks[count].route, route, sizeof( walks[0].route ) );
		walks[count].run = (qboolean)!Q_stricmp( pace, "run" );
		walks[count].delay = RC_SecondsMs( seconds );
		count++;
	}
	FS_FreeFile( buf );
	return count;
}

// the saved shot's NPCs set off, each when the shot says: sent for everyone, or local
void CL_RpCinematic_ShotWalks( const char *name, qboolean local ) {
	rcWalk_t walks[RC_MAX_WALKS];

	RC_QueueWalks( walks, RC_ReadWalks( name, walks, RC_MAX_WALKS ), local );
}

static void RC_ForgetShots( void ) {
	memset( rcShotCache, 0, sizeof( rcShotCache ) );
}

// a saved shot as its hook and as a shot; qfalse if there is none, or it isn't one
static qboolean RC_ReadShot( const char *name, char *hook, int size, rcShot_t *shot ) {
	char *buf, copy[MAX_STRING_CHARS], *argv[RC_MAX_KEYS * 2 + 8];
	int len, argc;

	if ( !name[0] )
		return qfalse;
	for ( const char *c = name; *c; c++ ) {
		if ( !RC_ShotNameChar( *c ) )
			return qfalse;
	}
	len = FS_ReadFile( RC_ShotPath( name ), (void **)&buf );
	if ( len <= 0 || !buf )
		return qfalse;
	Q_strncpyz( copy, buf, Q_min( (int)sizeof( copy ), len + 1 ) );
	FS_FreeFile( buf );

	// the first line that isn't a comment
	char *line = copy;
	while ( *line && ( *line == '/' && line[1] == '/' ) ) {
		line = strchr( line, '\n' );
		line = line ? line + 1 : copy + strlen( copy );
	}
	char *end = strpbrk( line, "\r\n" );
	if ( end )
		*end = '\0';
	if ( Q_stricmpn( line, "cin/", 4 ) || strstr( line, "//" ) )
		return qfalse;
	Q_strncpyz( hook, line, size );
	argc = RC_SplitHook( line, argv, ARRAY_LEN( argv ) );
	return RC_ParseShot( argv + 1, argc - 1, shot, vec3_origin );
}

// a saved shot's hook and how long it lasts, for /cincam play and the Storyteller
qboolean CL_RpCinematic_ShotHook( const char *name, char *hook, int size, int *ms ) {
	rcShot_t shot;
	int oldest = 0;

	for ( int i = 0; i < RC_SHOT_CACHE; i++ ) {
		if ( rcShotCache[i].used && !Q_stricmp( rcShotCache[i].name, name ) ) {
			rcShotCache[i].used = ++rcShotCacheUse;
			if ( !rcShotCache[i].hook[0] )
				return qfalse;
			Q_strncpyz( hook, rcShotCache[i].hook, size );
			if ( ms )
				*ms = rcShotCache[i].ms;
			return qtrue;
		}
		if ( rcShotCache[i].used < rcShotCache[oldest].used )
			oldest = i;
	}

	// not there is kept too
	Q_strncpyz( rcShotCache[oldest].name, name, sizeof( rcShotCache[oldest].name ) );
	rcShotCache[oldest].used = ++rcShotCacheUse;
	rcShotCache[oldest].hook[0] = '\0';
	if ( !RC_ReadShot( name, rcShotCache[oldest].hook, sizeof( rcShotCache[oldest].hook ), &shot ) ) {
		rcShotCache[oldest].hook[0] = '\0';
		return qfalse;
	}
	rcShotCache[oldest].ms = RC_ShotMs( &shot );
	Q_strncpyz( hook, rcShotCache[oldest].hook, size );
	if ( ms )
		*ms = rcShotCache[oldest].ms;
	return qtrue;
}

/*
===============================================================================

The camera editor

A free camera over the map: right mouse held looks and flies, the panel on the right
has the shot. Everything shown is worked out from the hook the shot sends, so what is
seen here is what the players get.

===============================================================================
*/

#define CE_TOGGLE_CMD		"cincam edit"

#define CE_FLY_SPEED		400.0f
#define CE_PANEL_X			446.0f
#define CE_PANEL_W			186.0f
#define CE_ROW_H			12.0f
#define CE_CTRL_H			15.0f
#define CE_GAP				3.0f
#define CE_TEXT_SCALE		0.7f
#define CE_KEY_ROWS			9
#define CE_MAX_SHOTS		256
#define CE_MESSAGE_MS		4000
#define CE_FRUSTUM			20.0f		// how far out a camera's frame is drawn
#define CE_TRACE_DIST		8192.0f
#define CE_MAX_LINES		900
#define CE_INFO_Y			( RC_BAR_FRAC * SCREEN_HEIGHT + 4 )

typedef struct {
	qboolean	open;
	int			font;
	qboolean	fromStoryteller;

	// the shot
	rcShot_t	shot;
	char		name[MAX_QPATH];
	qboolean	dirty;
	int			sel;				// the key, -1 none
	int			keyScroll;
	qboolean	bars;				// shown, and in the preview
	qboolean	fxPage;				// the panel shows the focus and the transitions, not the spots
	qboolean	npcPage;			// or the NPCs walking in it

	// the NPCs walking in the shot, their routes as last read
	rcWalk_t	walks[RC_MAX_WALKS];
	int			numWalks, walkSel;
	vec3_t		walkPts[RC_MAX_WALKS][NW_MAX_ROUTE_POINTS];
	int			walkNumPts[RC_MAX_WALKS];
	char		walkLoaded[RC_MAX_WALKS][MAX_QPATH];
	qboolean	typingWalk;			// the name box being typed in is the walking NPC's

	// the saved routes, to pick from
	char		routes[64][MAX_QPATH];
	int			numRoutes;
	float		dofNow;				// an automatic focus here, where it's got to
	int			dofTime;

	// the free camera; its fov is cg_fov's
	vec3_t		camOrg, camAng;
	float		camFov;
	qboolean	flying;				// right mouse held
	qboolean	through;			// the camera is the key: flying moves it
	qboolean	held[MAX_KEYS];
	int			lastFrameTime;

	// where the camera's crosshair is
	qboolean	haveAim;
	vec3_t		aim;

	// input; clicks and the wheel go to the control drawn under them
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;
	int			wheel;
	qboolean	typing;				// in the name box

	// saved shots, while the list is up
	qboolean	listing;
	char		shots[CE_MAX_SHOTS][MAX_QPATH];
	int			numShots, shotScroll;

	// a preview on this screen; the cinematic it stood in for comes back after
	qboolean	preview;
	int			previewEnd;
	rcState_t	saved;

	// the view drawn, for the numbers over the keys
	qboolean	haveView;
	vec3_t		viewOrg;
	vec3_t		viewAxis[3];
	float		viewFovX, viewFovY;

	// the scene's lines this frame
	int			numLines;

	char		message[MAX_STRING_CHARS];
	int			messageTime;
} ceState_t;

static ceState_t ce;

static const byte ceWalkColor[4]	= { 190, 110, 255, 255 };

static vec4_t ceWhite		= { 1.0f, 1.0f, 1.0f, 1.0f };
static vec4_t cePanel		= { 0.0f, 0.0f, 0.0f, 0.7f };
static vec4_t cePanelLight	= { 0.15f, 0.15f, 0.15f, 0.85f };
static vec4_t ceFocus		= { 0.1f, 0.2f, 0.3f, 0.9f };
static vec4_t ceHighlight	= { 0.2f, 0.45f, 0.8f, 0.85f };
static vec4_t ceHoverBox	= { 0.28f, 0.28f, 0.28f, 0.9f };
static vec4_t ceBorder		= { 0.5f, 0.5f, 0.5f, 0.8f };
static vec4_t ceDim			= { 0.65f, 0.65f, 0.65f, 1.0f };
static vec4_t ceGuide		= { 1.0f, 1.0f, 1.0f, 0.25f };
static vec4_t ceBarGuide	= { 0.0f, 0.0f, 0.0f, 0.45f };

static const byte ceTrailColor[4]	= { 255, 200, 40, 255 };
static const byte ceKeyColor[4]		= { 235, 235, 235, 255 };
static const byte ceSelColor[4]		= { 60, 220, 255, 255 };
static const byte ceGhostColor[4]	= { 80, 255, 120, 255 };
static const byte ceLookColor[4]	= { 255, 80, 220, 255 };
static const byte ceAimColor[4]		= { 255, 140, 40, 255 };
static const byte ceRayColor[4]		= { 60, 220, 255, 110 };
static const byte ceCircleColor[4]	= { 255, 200, 40, 90 };

static qboolean CE_Open( void ) {
	return ce.open;
}

static void CE_Message( const char *text ) {
	Q_strncpyz( ce.message, text, sizeof( ce.message ) );
	ce.messageTime = cls.realtime;
}

static void CE_Changed( void ) {
	ce.dirty = qtrue;
}

// the shot as the players will get it: written out and read back, rounding and all
static qboolean CE_Shown( rcShot_t *out, char *hook, int size ) {
	char buf[MAX_STRING_CHARS], *argv[RC_MAX_KEYS * 2 + 8];
	int argc;

	if ( !ce.shot.orbit && !ce.shot.numKeys )
		return qfalse;
	RC_WriteShot( &ce.shot, buf, sizeof( buf ) );
	if ( hook )
		Q_strncpyz( hook, buf, size );
	argc = RC_SplitHook( buf, argv, ARRAY_LEN( argv ) );
	if ( !RC_ParseShot( argv + 1, argc - 1, out, vec3_origin ) )
		return qfalse;
	RC_FillFov( out, ce.camFov );
	return qtrue;
}

static void CE_KeyFromCamera( rcKey_t *k ) {
	VectorCopy( ce.camOrg, k->org );
	VectorCopy( ce.camAng, k->ang );
	k->fov = ce.camFov;
}

static void CE_CameraFromKey( const rcKey_t *k ) {
	VectorCopy( k->org, ce.camOrg );
	VectorCopy( k->ang, ce.camAng );
	ce.camAng[PITCH] = Com_Clamp( -89.0f, 89.0f, AngleNormalize180( ce.camAng[PITCH] ) );
	ce.camFov = k->fov > 0 ? k->fov : ce.camFov;
}

static rcKey_t *CE_SelectedKey( void ) {
	if ( ce.shot.orbit || ce.sel < 0 || ce.sel >= ce.shot.numKeys )
		return NULL;
	return &ce.shot.keys[ce.sel];
}

static void CE_Select( int index ) {
	rcKey_t *k;

	ce.sel = ce.shot.numKeys ? Com_Clampi( 0, ce.shot.numKeys - 1, index ) : -1;
	if ( ce.sel < ce.keyScroll )
		ce.keyScroll = ce.sel;
	else if ( ce.sel >= ce.keyScroll + CE_KEY_ROWS )
		ce.keyScroll = ce.sel - CE_KEY_ROWS + 1;
	k = CE_SelectedKey();
	if ( ce.through && k )
		CE_CameraFromKey( k );
	if ( !k )
		ce.through = qfalse;
}

// a key where the camera is, after the selected one
static void CE_AddKey( void ) {
	rcShot_t *s = &ce.shot;
	const int at = ( ce.sel >= 0 && ce.sel < s->numKeys ) ? ce.sel + 1 : s->numKeys;

	if ( s->orbit )
		return;
	if ( s->numKeys >= RC_MAX_KEYS ) {
		CE_Message( S_COLOR_YELLOW "A shot has 16 spots at most: make another shot" );
		return;
	}
	memmove( &s->keys[at + 1], &s->keys[at], ( s->numKeys - at ) * sizeof( s->keys[0] ) );
	s->numKeys++;
	CE_KeyFromCamera( &s->keys[at] );
	if ( !at ) {
		s->keys[0].ms = 0;
		if ( s->numKeys > 1 && !s->keys[1].ms )
			s->keys[1].ms = 2000;
	} else {
		s->keys[at].ms = at > 1 ? s->keys[at - 1].ms : 2000;
		if ( s->keys[at].ms <= 0 )
			s->keys[at].ms = 2000;
	}
	ce.through = qfalse;
	CE_Select( at );
	CE_Changed();
}

static void CE_DeleteKey( void ) {
	rcShot_t *s = &ce.shot;
	const int at = ce.sel;

	if ( !CE_SelectedKey() )
		return;
	memmove( &s->keys[at], &s->keys[at + 1], ( s->numKeys - at - 1 ) * sizeof( s->keys[0] ) );
	s->numKeys--;
	if ( s->numKeys )
		s->keys[0].ms = 0;
	ce.through = qfalse;
	CE_Select( at < s->numKeys ? at : s->numKeys - 1 );
	CE_Changed();
}

static void CE_SetKeyHere( void ) {
	rcKey_t *k = CE_SelectedKey();

	if ( !k )
		return;
	CE_KeyFromCamera( k );
	CE_Changed();
}

static void CE_ToggleThrough( void ) {
	rcKey_t *k = CE_SelectedKey();

	if ( !k ) {
		ce.through = qfalse;
		return;
	}
	ce.through = (qboolean)!ce.through;
	if ( ce.through )
		CE_CameraFromKey( k );
}

static void CE_SetOrbit( qboolean orbit ) {
	rcShot_t *s = &ce.shot;

	if ( s->orbit == orbit )
		return;
	s->orbit = orbit;
	ce.through = qfalse;
	if ( orbit && !s->ms ) {
		// round what the crosshair is on, from where the camera is
		if ( ce.haveAim ) {
			VectorCopy( ce.aim, s->center );
		} else {
			vec3_t forward;

			AngleVectors( ce.camAng, forward, NULL, NULL );
			VectorMA( ce.camOrg, 300.0f, forward, s->center );
		}
		s->radius = 200;
		s->height = 64;
		s->degrees = 360;
		s->ms = 10000;
		s->linear = qtrue;
		s->fov = s->endFov = ce.camFov;
		s->fromYaw = RAD2DEG( atan2f( ce.camOrg[1] - s->center[1], ce.camOrg[0] - s->center[0] ) );
	}
	CE_Changed();
}

// the orbit starts where the camera is
static void CE_OrbitFromHere( void ) {
	rcShot_t *s = &ce.shot;
	const float dx = ce.camOrg[0] - s->center[0], dy = ce.camOrg[1] - s->center[1];

	s->radius = Q_max( 16.0f, sqrtf( dx * dx + dy * dy ) );
	s->height = ce.camOrg[2] - s->center[2];
	s->fromYaw = RAD2DEG( atan2f( dy, dx ) );
	s->fov = s->endFov = ce.camFov;
	CE_Changed();
}

static void CE_New( void ) {
	memset( &ce.shot, 0, sizeof( ce.shot ) );
	ce.numWalks = 0;
	ce.walkSel = -1;
	memset( ce.walkLoaded, 0, sizeof( ce.walkLoaded ) );
	ce.name[0] = '\0';
	ce.dirty = qfalse;
	ce.sel = -1;
	ce.keyScroll = 0;
	ce.through = qfalse;
}

static qboolean CE_Load( const char *name ) {
	char hook[MAX_STRING_CHARS];
	rcShot_t shot;

	if ( !RC_ReadShot( name, hook, sizeof( hook ), &shot ) ) {
		CE_Message( va( S_COLOR_RED "No shot called %s", name ) );
		return qfalse;
	}
	RC_FillFov( &shot, RC_DEFAULT_FOV );
	CE_New();
	ce.shot = shot;
	ce.numWalks = RC_ReadWalks( name, ce.walks, RC_MAX_WALKS );
	ce.walkSel = ce.numWalks ? 0 : -1;
	Q_strncpyz( ce.name, name, sizeof( ce.name ) );
	CE_Select( 0 );
	// the camera a step behind where the shot starts, to see it from
	if ( !shot.orbit && shot.numKeys ) {
		vec3_t forward;

		CE_CameraFromKey( &shot.keys[0] );
		AngleVectors( ce.camAng, forward, NULL, NULL );
		VectorMA( ce.camOrg, -80.0f, forward, ce.camOrg );
		ce.camOrg[2] += 16.0f;
	} else if ( shot.orbit ) {
		vec3_t org, ang;
		float fov;

		RC_EvalShot( &shot, 0, org, ang, &fov );
		VectorCopy( org, ce.camOrg );
		VectorCopy( ang, ce.camAng );
	}
	CE_Message( va( S_COLOR_GREEN "Loaded %s", name ) );
	return qtrue;
}

static qboolean CE_Save( void ) {
	char hook[MAX_STRING_CHARS];
	rcShot_t shown;
	fileHandle_t f;

	if ( !ce.name[0] ) {
		ce.typing = qtrue;
		CE_Message( S_COLOR_YELLOW "Type a name for the shot first" );
		return qfalse;
	}
	if ( !CE_Shown( &shown, hook, sizeof( hook ) ) ) {
		CE_Message( S_COLOR_YELLOW "Nothing to save: add a spot first" );
		return qfalse;
	}
	f = FS_FOpenFileWrite( RC_ShotPath( ce.name ) );
	if ( !f ) {
		CE_Message( va( S_COLOR_RED "Couldn't write %s", RC_ShotPath( ce.name ) ) );
		return qfalse;
	}
	FS_Write( RC_SHOT_HEADER, strlen( RC_SHOT_HEADER ), f );
	FS_Write( hook, strlen( hook ), f );
	FS_Write( "\n", 1, f );
	// the NPCs walking in it, for whoever plays it to send
	for ( int i = 0; i < ce.numWalks; i++ ) {
		const rcWalk_t *w = &ce.walks[i];
		const char *line;

		if ( !w->npc[0] || !w->route[0] )
			continue;
		line = va( "walk %s %s %s %g\n", w->npc, w->route, w->run ? "run" : "walk", RC_Seconds( w->delay ) );
		FS_Write( line, strlen( line ), f );
	}
	FS_FCloseFile( f );
	RC_ForgetShots();
	ce.dirty = qfalse;
	ce.typing = qfalse;
	CE_Message( va( S_COLOR_GREEN "Saved %s", ce.name ) );
	if ( ce.fromStoryteller )
		CL_Storyteller_ShotSaved( ce.name );
	return qtrue;
}

static int QDECL CE_CompareNames( const void *a, const void *b ) {
	return Q_stricmp( (const char *)a, (const char *)b );
}

static void CE_ListShots( void ) {
	char list[16384], *name;
	int count;

	ce.numShots = 0;
	ce.shotScroll = 0;
	count = FS_GetFileList( RC_SHOT_DIR, RC_SHOT_EXT, list, sizeof( list ) );
	name = list;
	for ( int i = 0; i < count && ce.numShots < CE_MAX_SHOTS; i++ ) {
		const int len = strlen( name );

		Q_strncpyz( ce.shots[ce.numShots], name, sizeof( ce.shots[0] ) );
		COM_StripExtension( ce.shots[ce.numShots], ce.shots[ce.numShots], sizeof( ce.shots[0] ) );
		if ( ce.shots[ce.numShots][0] )
			ce.numShots++;
		name += len + 1;
	}
	qsort( ce.shots, ce.numShots, sizeof( ce.shots[0] ), CE_CompareNames );
	ce.listing = qtrue;
}

static void CE_StopPreview( void ) {
	if ( !ce.preview )
		return;
	ce.preview = qfalse;
	RC_StopLocalWalks( ce.walks, ce.numWalks );
	// the cinematic that was on before, or none
	rc = ce.saved;
	rc.frame = -1;
	RC_SyncThirdPerson();
}

// the shot on this screen, from the selected key on
static void CE_Preview( qboolean fromKey ) {
	char hook[MAX_STRING_CHARS];
	rcShot_t shown;

	if ( ce.preview || !CE_Shown( &shown, NULL, 0 ) ) {
		if ( !ce.preview )
			CE_Message( S_COLOR_YELLOW "Add a spot first" );
		return;
	}
	if ( fromKey && !ce.shot.orbit && ce.sel > 0 ) {
		rcShot_t part = ce.shot;

		part.numKeys = ce.shot.numKeys - ce.sel;
		memmove( part.keys, &ce.shot.keys[ce.sel], part.numKeys * sizeof( part.keys[0] ) );
		part.keys[0].ms = 0;
		RC_WriteShot( &part, hook, sizeof( hook ) );
		shown.numKeys = part.numKeys;
		memcpy( shown.keys, part.keys, sizeof( part.keys ) );
	} else {
		RC_WriteShot( &ce.shot, hook, sizeof( hook ) );
	}

	// the NPCs walk on this screen too, from the start of the shot
	if ( !fromKey || ce.shot.orbit || ce.sel <= 0 )
		RC_QueueWalks( ce.walks, ce.numWalks, qtrue );
	ce.saved = rc;
	ce.preview = qtrue;
	ce.flying = qfalse;
	ce.through = qfalse;
	memset( &rc, 0, sizeof( rc ) );
	rc.haveView = qtrue;
	VectorCopy( ce.camOrg, rc.viewOrg );
	VectorCopy( ce.camAng, rc.viewAng );
	rc.viewFovY = RC_FovY( ce.camFov );
	RC_RunLocal( hook );
	if ( ce.bars ) {
		rc.bars = qtrue;
		rc.barsDuration = 0;
	}
	ce.previewEnd = cls.realtime + RC_ShotMs( &shown ) + 1000;
}

static void CE_SendShot( void ) {
	char hook[MAX_STRING_CHARS];
	rcShot_t shown;

	if ( cls.state != CA_ACTIVE )
		return;
	if ( !CE_Shown( &shown, hook, sizeof( hook ) ) ) {
		CE_Message( S_COLOR_YELLOW "Add a spot first" );
		return;
	}
	if ( strlen( hook ) > RC_MAX_HOOK ) {
		CE_Message( S_COLOR_RED "The shot is too long to send: take a spot or two out" );
		return;
	}
	if ( !CL_RpHooks_Experimental() ) {
		CE_Message( S_COLOR_YELLOW RP_EXPERIMENTAL_OFF " in the storyteller or NPC manager" );
		return;
	}
	CL_AddReliableCommand( va( "rpnotify -1 1 cmd:%s", hook ), qfalse );
	RC_QueueWalks( ce.walks, ce.numWalks, qfalse );
	CE_Message( S_COLOR_GREEN "Sent to everyone" );
}

static void CE_ReleaseKeys( void ) {
	memset( ce.held, 0, sizeof( ce.held ) );
	ce.flying = qfalse;
}

// the editor opens on the named shot, or the one it had
void CL_RpCinematic_Edit( const char *name, qboolean fromStoryteller ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Camera editor: join a server first\n" );
		return;
	}
	CL_ModelManager_Close();
	CL_NpcManager_Close();
	CL_ShaderManager_Close();
	CL_EffectManager_Close();
	CL_Storyteller_Close();
	CL_SoundManager_Close();
	CL_MapEditor_Close();

	ce.font = re->RegisterFont( "arialnb" );
	if ( !ce.font )
		ce.font = cls.menuFont;
	if ( !ce.camFov )
		ce.camFov = RC_DEFAULT_FOV;
	// from where the player looks
	if ( rc.haveView ) {
		VectorCopy( rc.viewOrg, ce.camOrg );
		VectorCopy( rc.viewAng, ce.camAng );
	} else {
		VectorCopy( cl.snap.ps.origin, ce.camOrg );
		ce.camOrg[2] += cl.snap.ps.viewheight;
		VectorCopy( cl.viewangles, ce.camAng );
	}
	ce.camAng[ROLL] = 0;
	ce.camAng[PITCH] = Com_Clamp( -89.0f, 89.0f, AngleNormalize180( ce.camAng[PITCH] ) );

	ce.fromStoryteller = fromStoryteller;
	if ( name && name[0] && Q_stricmp( name, ce.name ) ) {
		if ( !CE_Load( name ) ) {
			// a new shot by that name
			CE_New();
			Q_strncpyz( ce.name, name, sizeof( ce.name ) );
			CE_Message( va( "New shot %s: fly to where it starts and press Enter", name ) );
		}
	}
	ce.open = qtrue;
	ce.listing = qfalse;
	ce.typing = qfalse;
	ce.through = qfalse;
	ce.cursorX = CE_PANEL_X - 120;
	ce.cursorY = SCREEN_HEIGHT * 0.5f;
	ce.click = qfalse;
	ce.wheel = 0;
	ce.lastFrameTime = cls.realtime;
	CE_ReleaseKeys();
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_CINEDITOR );
	RC_SyncThirdPerson();
}

void CL_RpCinematic_EditorClose( void ) {
	if ( !ce.open )
		return;
	CE_StopPreview();
	ce.open = qfalse;
	ce.typing = qfalse;
	ce.listing = qfalse;
	ce.through = qfalse;
	CE_ReleaseKeys();
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_CINEDITOR );
	RC_SyncThirdPerson();
}

qboolean CL_RpCinematic_EditorActive( void ) {
	return ce.open;
}

/*
===============================================================================

Editor input

===============================================================================
*/

static qboolean CE_Held( int upper ) {
	return (qboolean)( ce.held[upper] || ce.held[upper + ( 'a' - 'A' )] );
}

static qboolean CE_ShiftDown( void ) {
	return (qboolean)( ce.held[A_SHIFT] || ce.held[A_SHIFT2] );
}

static qboolean CE_CtrlDown( void ) {
	return (qboolean)( ce.held[A_CTRL] || ce.held[A_CTRL2] );
}

static qboolean CE_IsToggleKey( int key ) {
	const char *binding;

	if ( ce.typing && key >= A_SPACE && key <= A_TILDE )
		return qfalse;
	binding = Key_GetBinding( key );
	return (qboolean)( VALIDSTRING( binding ) && !Q_stricmp( binding, CE_TOGGLE_CMD ) );
}

static void CE_Zoom( float delta ) {
	ce.camFov = Com_Clamp( 10.0f, 130.0f, ce.camFov + delta );
}

void CL_RpCinematic_EditorKeyEvent( int key, qboolean down ) {
	if ( !ce.open )
		return;

	if ( key >= 0 && key < MAX_KEYS )
		ce.held[key] = down;
	if ( !down ) {
		if ( key == A_MOUSE2 )
			ce.flying = qfalse;
		return;
	}

	if ( ce.preview ) {
		if ( key == A_MOUSE1 || key == A_MOUSE2 || key == A_CAP_P || key == A_LOW_P || key == A_SPACE || key == A_ENTER )
			CE_StopPreview();
		return;
	}
	if ( CE_IsToggleKey( key ) ) {
		CL_RpCinematic_EditorClose();
		return;
	}

	switch ( key ) {
	case A_MOUSE1:
		ce.click = qtrue;
		ce.clickX = ce.cursorX;
		ce.clickY = ce.cursorY;
		return;
	case A_MOUSE2:
		ce.flying = qtrue;
		ce.typing = qfalse;
		return;
	case A_MWHEELUP:
		if ( ce.flying )
			CE_Zoom( CE_CtrlDown() ? -1.0f : -5.0f );
		else
			ce.wheel--;
		return;
	case A_MWHEELDOWN:
		if ( ce.flying )
			CE_Zoom( CE_CtrlDown() ? 1.0f : 5.0f );
		else
			ce.wheel++;
		return;
	default:
		break;
	}
	if ( ce.flying )
		return;		// flying keys

	if ( ce.typing ) {
		int size;
		char *text = CE_TypedText( &size );
		const int len = text ? strlen( text ) : 0;

		if ( key == A_BACKSPACE && len ) {
			text[len - 1] = '\0';
			CE_Changed();
		} else if ( ( key == A_ENTER || key == A_KP_ENTER ) && ce.typingWalk ) {
			ce.typing = qfalse;
		} else if ( key == A_ENTER || key == A_KP_ENTER ) {
			CE_Save();
		}
		return;
	}

	switch ( key ) {
	case A_ENTER:
	case A_KP_ENTER:
		CE_AddKey();
		break;
	case A_DELETE:
	case A_BACKSPACE:
		CE_DeleteKey();
		break;
	case A_CURSOR_UP:
		CE_Select( ce.sel - 1 );
		break;
	case A_CURSOR_DOWN:
		CE_Select( ce.sel + 1 );
		break;
	case A_CAP_F:
	case A_LOW_F:
		CE_ToggleThrough();
		break;
	case A_CAP_P:
	case A_LOW_P:
		CE_Preview( qfalse );
		break;
	default:
		break;
	}
}

// what's being typed in: the shot's name, or the walking NPC's; NULL if nothing
static char *CE_TypedText( int *size ) {
	if ( !ce.typing )
		return NULL;
	if ( ce.typingWalk ) {
		if ( ce.walkSel < 0 || ce.walkSel >= ce.numWalks )
			return NULL;
		*size = sizeof( ce.walks[0].npc );
		return ce.walks[ce.walkSel].npc;
	}
	*size = sizeof( ce.name );
	return ce.name;
}

void CL_RpCinematic_EditorCharEvent( int ch ) {
	int size, len;
	char *text;

	if ( !ce.open || ce.flying || !( text = CE_TypedText( &size ) ) )
		return;
	// an NPC can be several, commas between
	if ( !RC_ShotNameChar( ch ) && !( ce.typingWalk && ch == ',' ) )
		return;
	len = strlen( text );
	if ( len < size - 1 ) {
		text[len] = (char)ch;
		text[len + 1] = '\0';
		CE_Changed();
	}
}

void CL_RpCinematic_EditorMouseEvent( int dx, int dy ) {
	if ( !ce.open || ce.preview )
		return;
	if ( ce.flying ) {
		ce.camAng[YAW] -= dx * cl_sensitivity->value * m_yaw->value;
		ce.camAng[PITCH] = Com_Clamp( -89.0f, 89.0f, ce.camAng[PITCH] + dy * cl_sensitivity->value * m_pitch->value );
	} else {
		ce.cursorX = Com_Clamp( 0, SCREEN_WIDTH, ce.cursorX + dx );
		ce.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, ce.cursorY + dy );
	}
}

// escape backs out one level: the name box, the preview, the shot list, then the editor
void CL_RpCinematic_EditorEscape( void ) {
	if ( ce.preview ) {
		CE_StopPreview();
	} else if ( ce.typing ) {
		ce.typing = qfalse;
	} else if ( ce.listing ) {
		ce.listing = qfalse;
	} else {
		const qboolean back = ce.fromStoryteller;

		CL_RpCinematic_EditorClose();
		if ( back )
			CL_Storyteller_Open();
	}
}

// every frame before the cgame draws: flying, and the key that goes with the camera
static void CE_Frame( void ) {
	vec3_t forward, right, move, end;
	float dt, speed;
	trace_t tr;

	if ( !ce.open )
		return;
	if ( !( Key_GetCatcher() & KEYCATCH_CINEDITOR ) ) {
		// something else cleared the catchers
		CE_StopPreview();
		ce.open = qfalse;
		CE_ReleaseKeys();
		RC_SyncThirdPerson();
		return;
	}

	dt = Com_Clamp( 0.0f, 0.1f, ( cls.realtime - ce.lastFrameTime ) / 1000.0f );
	ce.lastFrameTime = cls.realtime;

	if ( ce.preview ) {
		if ( cls.realtime >= ce.previewEnd )
			CE_StopPreview();
		return;
	}

	if ( ce.flying ) {
		AngleVectors( ce.camAng, forward, right, NULL );
		VectorClear( move );
		if ( CE_Held( A_CAP_W ) )	VectorAdd( move, forward, move );
		if ( CE_Held( A_CAP_S ) )	VectorSubtract( move, forward, move );
		if ( CE_Held( A_CAP_D ) )	VectorAdd( move, right, move );
		if ( CE_Held( A_CAP_A ) )	VectorSubtract( move, right, move );
		if ( ce.held[A_SPACE] )		move[2] += 1.0f;
		if ( CE_Held( A_CAP_C ) )	move[2] -= 1.0f;
		speed = CE_FLY_SPEED;
		if ( CE_ShiftDown() )
			speed *= 3.0f;
		if ( CE_CtrlDown() )
			speed *= 0.25f;
		if ( VectorNormalize( move ) > 0.0f )
			VectorMA( ce.camOrg, speed * dt, move, ce.camOrg );
		// roll, and R levels it
		if ( CE_Held( A_CAP_Q ) )
			ce.camAng[ROLL] -= 45.0f * dt;
		if ( CE_Held( A_CAP_E ) )
			ce.camAng[ROLL] += 45.0f * dt;
		if ( CE_Held( A_CAP_R ) )
			ce.camAng[ROLL] = 0;
	}

	if ( ce.through ) {
		rcKey_t *k = CE_SelectedKey();

		if ( !k ) {
			ce.through = qfalse;
		} else if ( !VectorCompare( k->org, ce.camOrg ) || !VectorCompare( k->ang, ce.camAng ) || k->fov != ce.camFov ) {
			CE_KeyFromCamera( k );
			CE_Changed();
		}
	}

	// what the crosshair is on
	AngleVectors( ce.camAng, forward, NULL, NULL );
	VectorMA( ce.camOrg, CE_TRACE_DIST, forward, end );
	CM_BoxTrace( &tr, ce.camOrg, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
	ce.haveAim = (qboolean)( tr.fraction < 1.0f && !tr.allsolid );
	VectorCopy( tr.endpos, ce.aim );
}

/*
===============================================================================

Editor scene: the trail, the cameras and what they look at

===============================================================================
*/

static void CE_Line( const vec3_t a, const vec3_t b, const byte *color ) {
	refEntity_t ent;
	vec3_t mid, toCam;

	if ( ce.numLines >= CE_MAX_LINES )
		return;
	ce.numLines++;
	VectorAdd( a, b, mid );
	VectorScale( mid, 0.5f, mid );
	VectorSubtract( mid, ce.camOrg, toCam );

	memset( &ent, 0, sizeof( ent ) );
	ent.reType = RT_LINE;
	VectorCopy( a, ent.origin );
	VectorCopy( b, ent.oldorigin );
	ent.radius = 0.3f + VectorLength( toCam ) * 0.0015f;
	ent.customShader = cls.whiteShader;
	memcpy( ent.shaderRGBA, color, sizeof( ent.shaderRGBA ) );
	ent.renderfx = RF_NOSHADOW;
	re->AddRefEntityToScene( &ent );
}

static void CE_Cross( const vec3_t p, float size, const byte *color ) {
	for ( int i = 0; i < 3; i++ ) {
		vec3_t a, b;

		VectorCopy( p, a );
		VectorCopy( p, b );
		a[i] -= size;
		b[i] += size;
		CE_Line( a, b, color );
	}
}

// a camera: its frame as the screen is shaped, lines to it, and a mark on top
static void CE_Camera( const vec3_t org, const vec3_t ang, float fov, const byte *color ) {
	const float fovY = RC_FovY( fov );
	const float aspect = cls.glconfig.vidHeight ? (float)cls.glconfig.vidWidth / cls.glconfig.vidHeight : 4.0f / 3.0f;
	const float h = CE_FRUSTUM * tanf( DEG2RAD( fovY * 0.5f ) ), w = h * aspect;
	vec3_t axis[3], center, c[4], top;

	AnglesToAxis( ang, axis );
	VectorMA( org, CE_FRUSTUM, axis[0], center );
	for ( int i = 0; i < 4; i++ ) {
		VectorMA( center, ( i == 0 || i == 3 ) ? w : -w, axis[1], c[i] );
		VectorMA( c[i], i < 2 ? h : -h, axis[2], c[i] );
	}
	for ( int i = 0; i < 4; i++ ) {
		CE_Line( org, c[i], color );
		CE_Line( c[i], c[( i + 1 ) & 3], color );
	}
	VectorMA( center, h * 1.5f, axis[2], top );
	CE_Line( c[0], top, color );
	CE_Line( c[1], top, color );
}

// where a camera looks to, as far as the first wall
static void CE_Ray( const vec3_t org, const vec3_t ang, const byte *color ) {
	vec3_t forward, end;
	trace_t tr;

	AngleVectors( ang, forward, NULL, NULL );
	VectorMA( org, CE_TRACE_DIST, forward, end );
	CM_BoxTrace( &tr, org, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
	if ( tr.allsolid )
		return;
	VectorMA( org, CE_FRUSTUM, forward, end );
	CE_Line( end, tr.endpos, color );
	if ( tr.fraction < 1.0f )
		CE_Cross( tr.endpos, 6.0f, color );
}

static void CE_AddScene( void ) {
	rcShot_t s;
	vec3_t org, ang, prev;
	float fov;
	int total, samples;

	ce.numLines = 0;
	if ( ce.haveAim && !ce.flying )
		CE_Cross( ce.aim, 4.0f, ceAimColor );

	// where the NPCs walk: from where each stands through its route's spots
	for ( int i = 0; i < ce.numWalks; i++ ) {
		const rcWalk_t *wk = &ce.walks[i];
		const int num = wk->npc[0] ? CL_NpcWalk_Target( wk->npc ) : -1;
		vec3_t from;
		float speed;

		if ( Q_stricmp( ce.walkLoaded[i], wk->route ) ) {
			ce.walkNumPts[i] = wk->route[0] ? Q_max( 0, CL_NpcWalk_LoadRoute( wk->route, ce.walkPts[i], NW_MAX_ROUTE_POINTS, &speed ) ) : 0;
			Q_strncpyz( ce.walkLoaded[i], wk->route, sizeof( ce.walkLoaded[0] ) );
		}
		if ( !ce.walkNumPts[i] )
			continue;
		if ( num >= 0 && CL_NpcManager_NpcOrigin( num, from ) )
			CE_Line( from, ce.walkPts[i][0], ceWalkColor );
		for ( int j = 0; j < ce.walkNumPts[i]; j++ ) {
			if ( j )
				CE_Line( ce.walkPts[i][j - 1], ce.walkPts[i][j], ceWalkColor );
			CE_Cross( ce.walkPts[i][j], 3.0f, ceWalkColor );
		}
	}
	if ( !CE_Shown( &s, NULL, 0 ) )
		return;
	total = RC_ShotMs( &s );

	// the trail, and a tick each second along it
	if ( s.orbit || s.numKeys > 1 ) {
		samples = Com_Clampi( 24, 240, total / 40 );
		RC_EvalShot( &s, 0, prev, ang, &fov );
		for ( int i = 1; i <= samples; i++ ) {
			RC_EvalShot( &s, total * i / samples, org, ang, &fov );
			CE_Line( prev, org, ceTrailColor );
			VectorCopy( org, prev );
		}
		for ( int ms = 1000; ms < total; ms += 1000 ) {
			vec3_t a, b;

			RC_EvalShot( &s, ms, org, ang, &fov );
			VectorCopy( org, a );
			VectorCopy( org, b );
			a[2] -= 3.0f;
			b[2] += 3.0f;
			CE_Line( a, b, ceTrailColor );
		}
	}

	if ( s.orbit ) {
		// the whole circle faintly, the centre, and where it starts
		for ( int i = 0; i < 48; i++ ) {
			vec3_t a, b;
			const float y0 = DEG2RAD( i * 7.5f ), y1 = DEG2RAD( ( i + 1 ) * 7.5f );

			VectorSet( a, s.center[0] + cosf( y0 ) * s.radius, s.center[1] + sinf( y0 ) * s.radius, s.center[2] + s.height );
			VectorSet( b, s.center[0] + cosf( y1 ) * s.radius, s.center[1] + sinf( y1 ) * s.radius, s.center[2] + s.height );
			CE_Line( a, b, ceCircleColor );
		}
		CE_Cross( s.center, 10.0f, ceLookColor );
		RC_EvalShot( &s, 0, org, ang, &fov );
		CE_Camera( org, ang, fov, ceSelColor );
		CE_Line( org, s.center, ceRayColor );
	} else {
		for ( int i = 0; i < s.numKeys; i++ ) {
			const rcKey_t *k = &s.keys[i];
			vec3_t look;

			if ( ( ce.through && i == ce.sel ) || Distance( k->org, ce.camOrg ) < CE_FRUSTUM )
				continue;	// the camera is in it
			VectorCopy( k->ang, look );
			if ( s.look ) {
				vec3_t dir;

				VectorSubtract( s.lookAt, k->org, dir );
				vectoangles( dir, look );
				look[ROLL] = k->ang[ROLL];
			}
			CE_Camera( k->org, look, k->fov, i == ce.sel ? ceSelColor : ceKeyColor );
			if ( i == ce.sel ) {
				if ( s.look )
					CE_Line( k->org, s.lookAt, ceRayColor );
				else
					CE_Ray( k->org, look, ceRayColor );
			}
		}
		if ( s.look )
			CE_Cross( s.lookAt, 10.0f, ceLookColor );
	}

	// a camera running along the shot, as fast as it will
	if ( total > 0 ) {
		const int ms = cls.realtime % ( total + 1000 );

		RC_EvalShot( &s, Q_min( ms, total ), org, ang, &fov );
		CE_Camera( org, ang, fov, ceGhostColor );
	}
}

static void CE_RenderScene( const refdef_t *fd ) {
	refdef_t view = *fd;
	const float fovY = RC_FovY( ce.camFov );

	view.fov_y = fovY;
	view.fov_x = RC_FovX( fovY, (float)fd->width, (float)fd->height );
	VectorCopy( ce.camAng, view.viewangles );
	AnglesToAxis( ce.camAng, view.viewaxis );
	// the sky portal keeps its own origin but looks the way the camera does
	if ( !( fd->rdflags & RDF_SKYBOXPORTAL ) ) {
		VectorCopy( ce.camOrg, view.vieworg );
		view.viewContents = CM_PointContents( ce.camOrg, 0 );
		memset( view.areamask, 0, sizeof( view.areamask ) );
		CE_AddScene();
		ce.haveView = qtrue;
		VectorCopy( view.vieworg, ce.viewOrg );
		VectorCopy( view.viewaxis[0], ce.viewAxis[0] );
		VectorCopy( view.viewaxis[1], ce.viewAxis[1] );
		VectorCopy( view.viewaxis[2], ce.viewAxis[2] );
		ce.viewFovX = view.fov_x;
		ce.viewFovY = view.fov_y;
		if ( ( ce.fxPage || ce.through ) && !ce.listing && RC_DepthOfField( ce.camOrg, ce.camAng, ce.shot.dof, ce.shot.dofFocus,
			&ce.dofNow, &ce.dofTime, ce.shot.dofRange, ce.shot.dofBlur ) ) {
			re->RenderScene( &view );
			RC_NoDepthOfField();
			return;
		}
	}
	re->RenderScene( &view );
}

/*
===============================================================================

Editor panel

===============================================================================
*/

static void CE_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void CE_Box( float x, float y, float w, float h, const float *fill ) {
	CE_Fill( x, y, w, h, fill );
	CE_Fill( x, y, w, 1, ceBorder );
	CE_Fill( x, y + h - 1, w, 1, ceBorder );
	CE_Fill( x, y, 1, h, ceBorder );
	CE_Fill( x + w - 1, y, 1, h, ceBorder );
}

static float CE_TextWidth( const char *text ) {
	return (float)re->Font_StrLenPixels( text, ce.font, CE_TEXT_SCALE );
}

static void CE_Text( float x, float y, const char *text, const float *color ) {
	re->Font_DrawString( (int)x, (int)y, text, color, ce.font | STYLE_DROPSHADOW, -1, CE_TEXT_SCALE );
}

static void CE_TextClipped( float x, float y, float w, const char *text, const float *color ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 3 && CE_TextWidth( buf ) > w ) {
		len--;
		buf[len - 3] = '.';
		buf[len - 2] = '.';
		buf[len - 1] = '.';
		buf[len] = '\0';
	}
	CE_Text( x, y, buf, color );
}

static qboolean CE_InRect( float x, float y, float w, float h ) {
	return (qboolean)( !ce.flying && ce.cursorX >= x && ce.cursorX < x + w && ce.cursorY >= y && ce.cursorY < y + h );
}

// eats this frame's click if it landed in the rect
static qboolean CE_Clicked( float x, float y, float w, float h ) {
	if ( !ce.click || ce.clickX < x || ce.clickX >= x + w || ce.clickY < y || ce.clickY >= y + h )
		return qfalse;
	ce.click = qfalse;
	return qtrue;
}

static qboolean CE_Button( float x, float y, float w, const char *label, qboolean selected, qboolean enabled ) {
	const float *fill = selected ? ceHighlight : ( enabled && CE_InRect( x, y, w, CE_CTRL_H ) ) ? ceHoverBox : cePanelLight;

	CE_Box( x, y, w, CE_CTRL_H, fill );
	CE_Text( x + ( w - CE_TextWidth( label ) ) * 0.5f, y + 2, label, enabled ? ceWhite : ceDim );
	return (qboolean)( CE_Clicked( x, y, w, CE_CTRL_H ) && enabled );
}

// a value with - and + either side, the wheel over it too; how many steps it went
static int CE_Stepper( float x, float y, float w, const char *label, const char *value ) {
	const float lw = 62.0f, vx = x + lw + 17, vw = w - lw - 34;
	int steps = 0;

	CE_Text( x, y + 2, label, ceDim );
	if ( CE_Button( x + lw, y, 16, "-", qfalse, qtrue ) )
		steps--;
	CE_Box( vx, y, vw, CE_CTRL_H, cePanelLight );
	CE_Text( vx + ( vw - CE_TextWidth( value ) ) * 0.5f, y + 2, value, ceWhite );
	if ( CE_Button( x + w - 16, y, 16, "+", qfalse, qtrue ) )
		steps++;
	if ( ce.wheel && CE_InRect( x, y, w, CE_CTRL_H ) ) {
		steps -= ce.wheel;
		ce.wheel = 0;
	}
	return steps;
}

// Shift goes in big steps, Ctrl in small ones
static float CE_Step( float step ) {
	if ( CE_ShiftDown() )
		return step * 5.0f;
	if ( CE_CtrlDown() )
		return step * 0.2f;
	return step;
}

static void CE_Adjust( float *value, int steps, float step, float min, float max ) {
	if ( !steps )
		return;
	*value = Com_Clamp( min, max, *value + steps * CE_Step( step ) );
	CE_Changed();
}

static void CE_AdjustMs( int *ms, int steps, float seconds, int min ) {
	if ( !steps )
		return;
	*ms = Q_max( min, RC_SecondsMs( RC_Seconds( *ms ) + steps * CE_Step( seconds ) ) );
	CE_Changed();
}

static void CE_DrawShotList( float x, float y, float w ) {
	const int rows = 24;

	CE_Text( x, y, va( "Saved shots (%i)", ce.numShots ), ceWhite );
	y += CE_ROW_H + 2;
	if ( ce.wheel && CE_InRect( x, y, w, rows * CE_ROW_H ) ) {
		ce.shotScroll += ce.wheel;
		ce.wheel = 0;
	}
	ce.shotScroll = Com_Clampi( 0, Q_max( 0, ce.numShots - rows ), ce.shotScroll );
	CE_Box( x, y, w, rows * CE_ROW_H + 2, cePanelLight );
	for ( int i = 0; i < rows && ce.shotScroll + i < ce.numShots; i++ ) {
		const char *name = ce.shots[ce.shotScroll + i];
		const float ry = y + 1 + i * CE_ROW_H;

		if ( CE_InRect( x, ry, w, CE_ROW_H ) )
			CE_Fill( x + 1, ry, w - 2, CE_ROW_H, ceHoverBox );
		if ( !Q_stricmp( name, ce.name ) )
			CE_Fill( x + 1, ry, w - 2, CE_ROW_H, ceHighlight );
		CE_TextClipped( x + 4, ry, w - 8, name, ceWhite );
		if ( CE_Clicked( x, ry, w, CE_ROW_H ) && CE_Load( name ) )
			ce.listing = qfalse;
	}
	if ( !ce.numShots )
		CE_Text( x + 4, y + 2, S_COLOR_GREY "none yet", ceWhite );
	y += rows * CE_ROW_H + 8;
	if ( CE_Button( x, y, w, "Back", qfalse, qtrue ) )
		ce.listing = qfalse;
}

static void CE_DrawPathPanel( float x, float y, float w ) {
	rcShot_t *s = &ce.shot;
	const float half = ( w - 4 ) * 0.5f;
	rcKey_t *k;

	// looking at a point the whole way
	CE_Text( x, y + 2, "Look at", ceDim );
	if ( CE_Button( x + 62, y, 36, s->look ? "on" : "off", s->look, qtrue ) ) {
		s->look = (qboolean)!s->look;
		if ( s->look && VectorCompare( s->lookAt, vec3_origin ) && ce.haveAim )
			VectorCopy( ce.aim, s->lookAt );
		CE_Changed();
	}
	if ( CE_Button( x + 102, y, w - 102, "Set to crosshair", qfalse, ce.haveAim ) ) {
		VectorCopy( ce.aim, s->lookAt );
		s->look = qtrue;
		CE_Changed();
	}
	y += CE_CTRL_H + CE_GAP + 2;

	// the spots
	CE_Text( x, y, va( "Spots (%i)" S_COLOR_GREY "   %.1f s in all", s->numKeys, RC_ShotMs( s ) / 1000.0f ), ceWhite );
	y += CE_ROW_H + 2;
	if ( ce.wheel && CE_InRect( x, y, w, CE_KEY_ROWS * CE_ROW_H ) ) {
		ce.keyScroll += ce.wheel;
		ce.wheel = 0;
	}
	ce.keyScroll = Com_Clampi( 0, Q_max( 0, s->numKeys - CE_KEY_ROWS ), ce.keyScroll );
	CE_Box( x, y, w, CE_KEY_ROWS * CE_ROW_H + 2, cePanelLight );
	for ( int i = 0; i < CE_KEY_ROWS && ce.keyScroll + i < s->numKeys; i++ ) {
		const int index = ce.keyScroll + i;
		const rcKey_t *key = &s->keys[index];
		const float ry = y + 1 + i * CE_ROW_H;
		char row[64];

		if ( index == ce.sel )
			CE_Fill( x + 1, ry, w - 2, CE_ROW_H, ceHighlight );
		else if ( CE_InRect( x, ry, w, CE_ROW_H ) )
			CE_Fill( x + 1, ry, w - 2, CE_ROW_H, ceHoverBox );
		if ( index )
			Com_sprintf( row, sizeof( row ), "%i   after %.1f s", index + 1, RC_Seconds( key->ms ) );
		else
			Com_sprintf( row, sizeof( row ), "1   start" );
		CE_Text( x + 4, ry, row, ceWhite );
		Com_sprintf( row, sizeof( row ), "fov %i%s", RC_Round( key->fov ),
			RC_Round( AngleNormalize180( key->ang[ROLL] ) ) ? va( "  roll %i", RC_Round( AngleNormalize180( key->ang[ROLL] ) ) ) : "" );
		CE_Text( x + w - 4 - CE_TextWidth( row ), ry, row, ceDim );
		if ( CE_Clicked( x, ry, w, CE_ROW_H ) )
			CE_Select( index );
	}
	if ( !s->numKeys )
		CE_Text( x + 4, y + 2, S_COLOR_GREY "Fly to where the shot starts, Enter", ceWhite );
	y += CE_KEY_ROWS * CE_ROW_H + 6;

	if ( CE_Button( x, y, half, "Add spot here", qfalse, (qboolean)( s->numKeys < RC_MAX_KEYS ) ) )
		CE_AddKey();
	k = CE_SelectedKey();
	if ( CE_Button( x + half + 4, y, half, "Move spot here", qfalse, (qboolean)( k && !ce.through ) ) )
		CE_SetKeyHere();
	y += CE_CTRL_H + CE_GAP;
	if ( CE_Button( x, y, half, "Delete spot", qfalse, (qboolean)( k != NULL ) ) )
		CE_DeleteKey();
	if ( CE_Button( x + half + 4, y, half, ce.through ? "Through it (F)" : "Look through (F)", ce.through, (qboolean)( k != NULL ) ) )
		CE_ToggleThrough();
	y += CE_CTRL_H + CE_GAP + 2;

	if ( !k )
		return;
	if ( ce.sel ) {
		const int steps = CE_Stepper( x, y, w, "Seconds", va( "%.1f", RC_Seconds( k->ms ) ) );
		CE_AdjustMs( &k->ms, steps, 0.5f, 100 );
	} else {
		CE_Text( x, y + 2, S_COLOR_GREY "The shot starts here", ceWhite );
	}
	y += CE_CTRL_H + CE_GAP;
	{
		const int steps = CE_Stepper( x, y, w, "FOV", va( "%i", RC_Round( k->fov ) ) );
		CE_Adjust( &k->fov, steps, 5.0f, 10.0f, 130.0f );
		if ( steps && ce.through )
			ce.camFov = k->fov;
	}
	y += CE_CTRL_H + CE_GAP;
	{
		float roll = AngleNormalize180( k->ang[ROLL] );
		const int steps = CE_Stepper( x, y, w, "Roll", va( "%i", RC_Round( roll ) ) );

		if ( steps ) {
			CE_Adjust( &roll, steps, 5.0f, -180.0f, 180.0f );
			k->ang[ROLL] = roll;
			if ( ce.through )
				ce.camAng[ROLL] = roll;
		}
	}
}

static void CE_DrawOrbitPanel( float x, float y, float w ) {
	rcShot_t *s = &ce.shot;
	int steps;

	CE_Text( x, y + 2, "Round", ceDim );
	if ( CE_Button( x + 62, y, w - 62, "Set to crosshair", qfalse, ce.haveAim ) ) {
		VectorCopy( ce.aim, s->center );
		CE_Changed();
	}
	y += CE_CTRL_H + CE_GAP;
	CE_Text( x, y + 2, va( S_COLOR_GREY "%i %i %i", RC_Round( s->center[0] ), RC_Round( s->center[1] ), RC_Round( s->center[2] ) ), ceWhite );
	y += CE_ROW_H + CE_GAP;

	steps = CE_Stepper( x, y, w, "Seconds", va( "%.1f", RC_Seconds( s->ms ) ) );
	CE_AdjustMs( &s->ms, steps, 1.0f, 100 );
	y += CE_CTRL_H + CE_GAP;
	steps = CE_Stepper( x, y, w, "Degrees", va( "%i", RC_Round( s->degrees ) ) );
	CE_Adjust( &s->degrees, steps, 45.0f, -3600.0f, 3600.0f );
	y += CE_CTRL_H + CE_GAP;
	steps = CE_Stepper( x, y, w, "Radius", va( "%i", RC_Round( s->radius ) ) );
	CE_Adjust( &s->radius, steps, 16.0f, 16.0f, 8192.0f );
	y += CE_CTRL_H + CE_GAP;
	steps = CE_Stepper( x, y, w, "Height", va( "%i", RC_Round( s->height ) ) );
	CE_Adjust( &s->height, steps, 16.0f, -4096.0f, 4096.0f );
	y += CE_CTRL_H + CE_GAP;
	steps = CE_Stepper( x, y, w, "From yaw", va( "%i", RC_Round( AngleNormalize360( s->fromYaw ) ) ) );
	CE_Adjust( &s->fromYaw, steps, 15.0f, -3600.0f, 3600.0f );
	y += CE_CTRL_H + CE_GAP;
	steps = CE_Stepper( x, y, w, "Start FOV", va( "%i", RC_Round( s->fov ) ) );
	CE_Adjust( &s->fov, steps, 5.0f, 10.0f, 130.0f );
	y += CE_CTRL_H + CE_GAP;
	steps = CE_Stepper( x, y, w, "End FOV", va( "%i", RC_Round( s->endFov ) ) );
	CE_Adjust( &s->endFov, steps, 5.0f, 10.0f, 130.0f );
	y += CE_CTRL_H + CE_GAP + 2;
	if ( CE_Button( x, y, w, "Start from the camera", qfalse, qtrue ) )
		CE_OrbitFromHere();
}

static const char *ceTransNames[RC_NUM_TRANSITIONS] = { "Cut", "Fade", "Wipe from the left", "Wipe from the right", "Wipe from the top", "Wipe from the bottom" };

static const struct {
	const char	*name;
	byte		rgb[3];
} ceTransColors[] = {
	{ "Black", { 0, 0, 0 } },
	{ "White", { 255, 255, 255 } },
	{ "Red", { 190, 25, 25 } },
	{ "Orange", { 235, 130, 20 } },
	{ "Yellow", { 240, 215, 40 } },
	{ "Green", { 35, 170, 60 } },
	{ "Cyan", { 40, 200, 220 } },
	{ "Blue", { 30, 70, 210 } },
	{ "Purple", { 140, 50, 190 } }
};

// a transition's kind, length and colour; where the next row goes
static float CE_DrawTransition( float x, float y, float w, const char *label, rcTrans_t *tr ) {
	int color = -1, steps;

	CE_Text( x, y + 2, label, ceDim );
	// a click for the next kind
	if ( CE_Button( x + 62, y, w - 62, ceTransNames[tr->type], (qboolean)( tr->type != RC_TR_CUT ), qtrue ) ) {
		tr->type = ( tr->type + 1 ) % RC_NUM_TRANSITIONS;
		if ( tr->type != RC_TR_CUT && !tr->ms )
			tr->ms = RC_TRANS_MS;
		CE_Changed();
	}
	y += CE_CTRL_H + CE_GAP;
	if ( tr->type == RC_TR_CUT )
		return y;
	steps = CE_Stepper( x, y, w, "Seconds", va( "%.1f", RC_Seconds( tr->ms ) ) );
	CE_AdjustMs( &tr->ms, steps, 0.25f, 100 );
	y += CE_CTRL_H + CE_GAP;

	// the colour: a swatch, a click for the next
	for ( int i = 0; i < (int)ARRAY_LEN( ceTransColors ); i++ ) {
		if ( !memcmp( ceTransColors[i].rgb, tr->color, 3 ) )
			color = i;
	}
	CE_Text( x, y + 2, "Color", ceDim );
	{
		const vec4_t swatch = { tr->color[0] / 255.0f, tr->color[1] / 255.0f, tr->color[2] / 255.0f, 1.0f };

		CE_Box( x + 62, y, 18, CE_CTRL_H, ceBorder );
		CE_Fill( x + 63, y + 1, 16, CE_CTRL_H - 2, swatch );
	}
	if ( CE_Button( x + 84, y, w - 84, color >= 0 ? ceTransColors[color].name : va( "%i %i %i", tr->color[0], tr->color[1], tr->color[2] ), qfalse, qtrue ) ) {
		color = ( color + 1 ) % ARRAY_LEN( ceTransColors );
		memcpy( tr->color, ceTransColors[color].rgb, 3 );
		CE_Changed();
	}
	return y + CE_CTRL_H + CE_GAP;
}

// the shot's depth of field and how it comes in and goes out
static void CE_DrawFxPanel( float x, float y, float w ) {
	rcShot_t *s = &ce.shot;
	const float half = ( w - 4 ) * 0.5f;
	int steps;

	CE_Text( x, y, "Depth of field", ceWhite );
	y += CE_ROW_H + 2;
	CE_Text( x, y + 2, "Blur", ceDim );
	if ( CE_Button( x + 62, y, w - 62, s->dof ? "On: away from the focus" : "Off: all sharp", s->dof, (qboolean)( re->ext.SetDepthOfField != NULL ) ) ) {
		s->dof = (qboolean)!s->dof;
		if ( s->dof && s->dofRange <= 0.0f )
			s->dofRange = RC_DOF_RANGE;
		if ( s->dof && s->dofBlur <= 0.0f )
			s->dofBlur = RC_DOF_BLUR;
		CE_Changed();
	}
	y += CE_CTRL_H + CE_GAP;
	if ( s->dof ) {
		steps = CE_Stepper( x, y, w, "Focus", s->dofFocus > 0.0f ? va( "%i away", RC_Round( s->dofFocus ) ) : "what it sees" );
		CE_Adjust( &s->dofFocus, steps, 16.0f, 0.0f, 8192.0f );
		y += CE_CTRL_H + CE_GAP;
		if ( CE_Button( x, y, half, "What it sees", (qboolean)( s->dofFocus <= 0.0f ), qtrue ) ) {
			s->dofFocus = 0.0f;
			CE_Changed();
		}
		if ( CE_Button( x + half + 4, y, half, "The crosshair", qfalse, ce.haveAim ) ) {
			s->dofFocus = Q_max( 1.0f, Distance( ce.camOrg, ce.aim ) );
			CE_Changed();
		}
		y += CE_CTRL_H + CE_GAP;
		steps = CE_Stepper( x, y, w, "Sharp", va( "%i either side", RC_Round( s->dofRange ) ) );
		CE_Adjust( &s->dofRange, steps, 16.0f, 8.0f, 4096.0f );
		y += CE_CTRL_H + CE_GAP;
		steps = CE_Stepper( x, y, w, "Strength", va( "%i", RC_Round( s->dofBlur ) ) );
		CE_Adjust( &s->dofBlur, steps, 1.0f, 1.0f, RC_DOF_MAX_BLUR );
		y += CE_CTRL_H + CE_GAP;
		CE_Text( x, y, S_COLOR_GREY "Shown here as it is set", ceWhite );
		y += CE_ROW_H;
	} else if ( !re->ext.SetDepthOfField ) {
		CE_Text( x, y, S_COLOR_GREY "This renderer has none", ceWhite );
		y += CE_ROW_H;
	}
	y += 6;

	CE_Text( x, y, "Transitions", ceWhite );
	y += CE_ROW_H + 2;
	y = CE_DrawTransition( x, y, w, "Coming in", &s->in );
	y = CE_DrawTransition( x, y, w, "Going out", &s->out );
	if ( s->out.type != RC_TR_CUT )
		CE_Text( x, y, S_COLOR_GREY "The colour stays until the next shot", ceWhite );
}

static void CE_ListRoutes( void ) {
	char **files;
	int count;

	files = FS_ListFiles( "npcmanager/routes", ".route", &count );
	ce.numRoutes = 0;
	for ( int i = 0; i < count && ce.numRoutes < (int)ARRAY_LEN( ce.routes ); i++ )
		COM_StripExtension( files[i], ce.routes[ce.numRoutes++], sizeof( ce.routes[0] ) );
	FS_FreeFileList( files );
}

// the NPC whose head the crosshair is nearest, by its targetname if it has one; NULL if none near
static const char *CE_NpcAtCrosshair( void ) {
	float best = 96.0f;
	int found = -1;

	if ( !ce.haveAim )
		return NULL;
	for ( int i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
		vec3_t mid;

		if ( es->eType != ET_NPC )
			continue;
		VectorCopy( es->pos.trBase, mid );
		if ( Distance( mid, ce.aim ) < best ) {
			best = Distance( mid, ce.aim );
			found = es->number;
		}
	}
	if ( found < 0 )
		return NULL;
	for ( int i = 0; i < CL_NpcManager_NumNpcs(); i++ ) {
		const char *type, *name;
		int num;

		if ( CL_NpcManager_GetNpc( i, &num, &type, &name ) && num == found && name[0] )
			return name;
	}
	return va( "%i", found );
}

// the walks: which NPC, which route, how fast, how far into the shot
static void CE_DrawNpcPanel( float x, float y, float w ) {
	const float half = ( w - 4 ) * 0.5f;
	rcWalk_t *walk;
	int steps;

	CE_Text( x, y, va( "NPCs walking (%i)", ce.numWalks ), ceWhite );
	y += CE_ROW_H + 2;
	CE_Box( x, y, w, RC_MAX_WALKS * CE_ROW_H + 2, cePanelLight );
	for ( int i = 0; i < ce.numWalks; i++ ) {
		const rcWalk_t *wk = &ce.walks[i];
		const float ry = y + 1 + i * CE_ROW_H;

		if ( i == ce.walkSel )
			CE_Fill( x + 1, ry, w - 2, CE_ROW_H, ceHighlight );
		else if ( CE_InRect( x, ry, w, CE_ROW_H ) )
			CE_Fill( x + 1, ry, w - 2, CE_ROW_H, ceHoverBox );
		CE_TextClipped( x + 4, ry, w - 8, va( "%s" S_COLOR_GREY "  %s, %s at %.1f s", wk->npc[0] ? wk->npc : S_COLOR_YELLOW "no NPC",
			wk->route[0] ? wk->route : S_COLOR_YELLOW "no route", wk->run ? "runs" : "walks", RC_Seconds( wk->delay ) ), ceWhite );
		if ( CE_Clicked( x, ry, w, CE_ROW_H ) ) {
			ce.walkSel = i;
			ce.typing = qfalse;
		}
	}
	if ( !ce.numWalks )
		CE_Text( x + 4, y + 2, S_COLOR_GREY "None: Add one", ceWhite );
	y += RC_MAX_WALKS * CE_ROW_H + 6;

	if ( CE_Button( x, y, half, "Add", qfalse, (qboolean)( ce.numWalks < RC_MAX_WALKS ) ) ) {
		walk = &ce.walks[ce.numWalks];
		memset( walk, 0, sizeof( *walk ) );
		// the NPC the crosshair is on, and the first route, to start from
		if ( CE_NpcAtCrosshair() )
			Q_strncpyz( walk->npc, CE_NpcAtCrosshair(), sizeof( walk->npc ) );
		if ( ce.numRoutes )
			Q_strncpyz( walk->route, ce.routes[0], sizeof( walk->route ) );
		ce.walkLoaded[ce.numWalks][0] = '\0';
		ce.walkSel = ce.numWalks++;
		CE_Changed();
	}
	if ( CE_Button( x + half + 4, y, half, "Remove", qfalse, (qboolean)( ce.walkSel >= 0 && ce.walkSel < ce.numWalks ) ) ) {
		ce.numWalks--;
		memmove( &ce.walks[ce.walkSel], &ce.walks[ce.walkSel + 1], ( ce.numWalks - ce.walkSel ) * sizeof( ce.walks[0] ) );
		memset( ce.walkLoaded, 0, sizeof( ce.walkLoaded ) );
		ce.walkSel = Q_min( ce.walkSel, ce.numWalks - 1 );
		ce.typing = qfalse;
		CE_Changed();
	}
	y += CE_CTRL_H + CE_GAP + 4;
	if ( ce.walkSel < 0 || ce.walkSel >= ce.numWalks ) {
		CE_Text( x, y, S_COLOR_GREY "Routes are made in the NPC manager:", ceWhite );
		CE_Text( x, y + CE_ROW_H, S_COLOR_GREY "Manage tab, Walk, Route on map", ceWhite );
		return;
	}
	walk = &ce.walks[ce.walkSel];

	// the NPC: typed, or the one at the crosshair
	{
		const qboolean typing = (qboolean)( ce.typing && ce.typingWalk );
		const float bx = x + 62, bw = w - 62;

		CE_Text( x, y + 2, "NPC", ceDim );
		CE_Box( bx, y, bw, CE_CTRL_H, typing ? ceFocus : cePanelLight );
		CE_TextClipped( bx + 4, y + 2, bw - 8, walk->npc[0] || typing ? va( "%s%s", walk->npc, typing && ( ( cls.realtime >> 8 ) & 1 ) ? "_" : "" )
			: S_COLOR_GREY "targetname or number", ceWhite );
		if ( CE_Clicked( bx, y, bw, CE_CTRL_H ) ) {
			ce.typing = qtrue;
			ce.typingWalk = qtrue;
		}
	}
	y += CE_CTRL_H + CE_GAP;
	if ( CE_Button( x + 62, y, w - 62, "The one at the crosshair", qfalse, (qboolean)( CE_NpcAtCrosshair() != NULL ) ) ) {
		Q_strncpyz( walk->npc, CE_NpcAtCrosshair(), sizeof( walk->npc ) );
		ce.typing = qfalse;
		CE_Changed();
	}
	y += CE_CTRL_H + CE_GAP;

	// the route: - and + go through the saved ones
	{
		int at = -1;

		for ( int i = 0; i < ce.numRoutes; i++ ) {
			if ( !Q_stricmp( ce.routes[i], walk->route ) )
				at = i;
		}
		steps = CE_Stepper( x, y, w, "Route", walk->route[0] ? walk->route : "none saved" );
		if ( steps && ce.numRoutes ) {
			at = ( ( at < 0 ? 0 : at + steps ) % ce.numRoutes + ce.numRoutes ) % ce.numRoutes;
			Q_strncpyz( walk->route, ce.routes[at], sizeof( walk->route ) );
			CE_Changed();
		}
	}
	y += CE_CTRL_H + CE_GAP;
	CE_Text( x, y + 2, "Pace", ceDim );
	if ( CE_Button( x + 62, y, w - 62, walk->run ? "Runs" : "Walks", qfalse, qtrue ) ) {
		walk->run = (qboolean)!walk->run;
		CE_Changed();
	}
	y += CE_CTRL_H + CE_GAP;
	steps = CE_Stepper( x, y, w, "Sets off", va( "%.1f s in", RC_Seconds( walk->delay ) ) );
	CE_AdjustMs( &walk->delay, steps, 0.5f, 0 );
	y += CE_CTRL_H + CE_GAP + 4;
	CE_Text( x, y, S_COLOR_GREY "Preview walks them here.", ceWhite );
	CE_Text( x, y + CE_ROW_H, S_COLOR_GREY "Played for everyone, sent then;", ceWhite );
	CE_Text( x, y + CE_ROW_H * 2, S_COLOR_GREY "a second apart at the soonest", ceWhite );
}

static qboolean CE_Project( const vec3_t point, float *x, float *y ) {
	vec3_t d;
	float forward;

	if ( !ce.haveView )
		return qfalse;
	VectorSubtract( point, ce.viewOrg, d );
	forward = DotProduct( d, ce.viewAxis[0] );
	if ( forward < 4.0f )
		return qfalse;
	*x = SCREEN_WIDTH * 0.5f * ( 1.0f - DotProduct( d, ce.viewAxis[1] ) / forward / tanf( DEG2RAD( ce.viewFovX * 0.5f ) ) );
	*y = SCREEN_HEIGHT * 0.5f * ( 1.0f - DotProduct( d, ce.viewAxis[2] ) / forward / tanf( DEG2RAD( ce.viewFovY * 0.5f ) ) );
	return qtrue;
}

// the frame every screen shares, and where the bars will cover
static void CE_DrawGuides( void ) {
	const float aspect = cls.glconfig.vidHeight ? (float)cls.glconfig.vidWidth / cls.glconfig.vidHeight : 4.0f / 3.0f;
	const float cx = SCREEN_WIDTH * 0.5f, cy = SCREEN_HEIGHT * 0.5f;

	if ( ce.bars ) {
		const float h = RC_BAR_FRAC * SCREEN_HEIGHT;

		CE_Fill( 0, 0, SCREEN_WIDTH, h, ceBarGuide );
		CE_Fill( 0, SCREEN_HEIGHT - h, SCREEN_WIDTH, h, ceBarGuide );
	}
	// a wide screen sees more at the sides than a 4:3 one
	if ( aspect > 4.0f / 3.0f + 0.01f ) {
		const float w = SCREEN_WIDTH * ( 4.0f / 3.0f ) / aspect;

		CE_Fill( cx - w * 0.5f, 0, 1, SCREEN_HEIGHT, ceGuide );
		CE_Fill( cx + w * 0.5f, 0, 1, SCREEN_HEIGHT, ceGuide );
	}
	CE_Fill( cx - 6, cy, 13, 1, ceWhite );
	CE_Fill( cx, cy - 6, 1, 13, ceWhite );
}

static void CE_DrawEditor( void ) {
	const float x = CE_PANEL_X + 6, w = CE_PANEL_W - 12;
	const float half = ( w - 4 ) * 0.5f, third = ( w - 8 ) / 3.0f;
	char hook[MAX_STRING_CHARS];
	rcShot_t shown;
	float y = 8;

	if ( ce.preview ) {
		CE_Text( 8, SCREEN_HEIGHT - 16, S_COLOR_GREY "Preview: Esc, P or a click stops it", ceWhite );
		return;
	}

	CE_DrawGuides();

	// numbers over the spots
	if ( !ce.shot.orbit ) {
		for ( int i = 0; i < ce.shot.numKeys; i++ ) {
			float sx, sy;

			if ( ( ce.through && i == ce.sel ) || !CE_Project( ce.shot.keys[i].org, &sx, &sy ) )
				continue;
			if ( sx < 0 || sx > SCREEN_WIDTH || sy < 0 || sy > SCREEN_HEIGHT )
				continue;
			CE_Text( sx + 4, sy - 14, va( "%s%i", i == ce.sel ? S_COLOR_CYAN : "", i + 1 ), ceWhite );
		}
	}

	// top left, under the console's lines: what this is, how it went
	if ( ce.through )
		CE_Text( 8, CE_INFO_Y, va( "Camera editor" S_COLOR_CYAN "   looking through spot %i: flying moves it", ce.sel + 1 ), ceWhite );
	else
		CE_Text( 8, CE_INFO_Y, "Camera editor", ceWhite );
	CE_Text( 8, CE_INFO_Y + CE_ROW_H, va( S_COLOR_GREY "FOV %i%s", RC_Round( ce.camFov ),
		RC_Round( AngleNormalize180( ce.camAng[ROLL] ) ) ? va( "   roll %i", RC_Round( AngleNormalize180( ce.camAng[ROLL] ) ) ) : "" ), ceWhite );
	if ( ce.message[0] && cls.realtime - ce.messageTime < CE_MESSAGE_MS )
		CE_TextClipped( 8, CE_INFO_Y + CE_ROW_H * 2, CE_PANEL_X - 16, ce.message, ceWhite );

	// bottom left: the keys
	{
		static const char *help[] = {
			"Hold right mouse: look and fly   WASD  Space/C up/down  Shift fast  Ctrl slow",
			"While flying: wheel zooms   Q/E roll   R levels",
			"Enter add a spot   Del delete   Up/Down pick   F look through it   P preview",
			"Esc back"
		};
		for ( int i = 0; i < (int)ARRAY_LEN( help ); i++ )
			CE_Text( 8, SCREEN_HEIGHT - 12 - ( ARRAY_LEN( help ) - 1 - i ) * CE_ROW_H, va( S_COLOR_GREY "%s", help[i] ), ceWhite );
	}

	CE_Fill( CE_PANEL_X, 0, CE_PANEL_W, SCREEN_HEIGHT, cePanel );

	// the name and the files
	CE_Text( x, y, "Shot", ceWhite );
	{
		const float bx = x + 32, bw = w - 32;
		const qboolean typing = (qboolean)( ce.typing && !ce.typingWalk );
		const char *label = ce.name[0] || typing ? va( "%s%s%s", ce.name, typing && ( ( cls.realtime >> 8 ) & 1 ) ? "_" : "",
			ce.dirty ? S_COLOR_YELLOW " *" : "" ) : S_COLOR_GREY "click to name it";

		CE_Box( bx, y - 2, bw, CE_CTRL_H, typing ? ceFocus : cePanelLight );
		CE_TextClipped( bx + 4, y, bw - 8, label, ceWhite );
		if ( CE_Clicked( bx, y - 2, bw, CE_CTRL_H ) ) {
			ce.typing = qtrue;
			ce.typingWalk = qfalse;
		}
	}
	y += CE_CTRL_H + CE_GAP;
	if ( CE_Button( x, y, third, "Save", qfalse, qtrue ) )
		CE_Save();
	if ( CE_Button( x + third + 4, y, third, "Open", ce.listing, qtrue ) ) {
		if ( ce.listing )
			ce.listing = qfalse;
		else
			CE_ListShots();
	}
	if ( CE_Button( x + ( third + 4 ) * 2, y, third, "New", qfalse, qtrue ) ) {
		CE_New();
		CE_Message( "New shot: fly to where it starts and press Enter" );
	}
	y += CE_CTRL_H + CE_GAP + 6;

	if ( ce.listing ) {
		CE_DrawShotList( x, y, w );
	} else {
		// the kind of shot, and how it moves
		if ( CE_Button( x, y, half, "Path", (qboolean)!ce.shot.orbit, qtrue ) )
			CE_SetOrbit( qfalse );
		if ( CE_Button( x + half + 4, y, half, "Orbit", ce.shot.orbit, qtrue ) )
			CE_SetOrbit( qtrue );
		y += CE_CTRL_H + CE_GAP;
		if ( CE_Button( x, y, half, ce.shot.linear ? "Steady speed" : "Eases in and out", qfalse, qtrue ) ) {
			ce.shot.linear = (qboolean)!ce.shot.linear;
			CE_Changed();
		}
		if ( CE_Button( x + half + 4, y, half, ce.bars ? "Bars shown" : "No bars", ce.bars, qtrue ) )
			ce.bars = (qboolean)!ce.bars;
		y += CE_CTRL_H + CE_GAP;
		// what the panel shows of it
		if ( CE_Button( x, y, third, ce.shot.orbit ? "Orbit" : "Spots", (qboolean)( !ce.fxPage && !ce.npcPage ), qtrue ) )
			ce.fxPage = ce.npcPage = qfalse;
		if ( CE_Button( x + third + 4, y, third, "Focus, fades", ce.fxPage, qtrue ) ) {
			ce.fxPage = qtrue;
			ce.npcPage = qfalse;
		}
		if ( CE_Button( x + ( third + 4 ) * 2, y, third, ce.numWalks ? va( "NPCs (%i)", ce.numWalks ) : "NPCs", ce.npcPage, qtrue ) ) {
			ce.npcPage = qtrue;
			ce.fxPage = qfalse;
			CE_ListRoutes();
		}
		y += CE_CTRL_H + CE_GAP + 6;

		if ( ce.npcPage )
			CE_DrawNpcPanel( x, y, w );
		else if ( ce.fxPage )
			CE_DrawFxPanel( x, y, w );
		else if ( ce.shot.orbit )
			CE_DrawOrbitPanel( x, y, w );
		else
			CE_DrawPathPanel( x, y, w );

		// try it, send it
		y = SCREEN_HEIGHT - 4 - ( CE_CTRL_H + CE_GAP ) * 2 - CE_ROW_H;
		{
			const qboolean have = CE_Shown( &shown, hook, sizeof( hook ) );
			const int len = have ? strlen( hook ) : 0;

			if ( len > RC_MAX_HOOK )
				CE_Text( x, y, S_COLOR_RED "Too long to send: fewer spots", ceWhite );
			else if ( have )
				CE_Text( x, y, va( S_COLOR_GREY "%.1f s, %i characters to send", RC_ShotMs( &shown ) / 1000.0f, len ), ceWhite );
			y += CE_ROW_H;
			if ( CE_Button( x, y, half, "Preview (P)", qfalse, have ) )
				CE_Preview( qfalse );
			if ( CE_Button( x + half + 4, y, half, va( "From spot %i", ce.sel + 1 ), qfalse,
				(qboolean)( have && !ce.shot.orbit && ce.sel > 0 ) ) )
				CE_Preview( qtrue );
			y += CE_CTRL_H + CE_GAP;
			if ( CE_Button( x, y, half, "Play for everyone", qfalse, (qboolean)( have && len <= RC_MAX_HOOK && CL_RpHooks_Experimental() ) ) )
				CE_SendShot();
			if ( CE_Button( x + half + 4, y, half, "End for everyone", qfalse, CL_RpHooks_Experimental() ) ) {
				CL_AddReliableCommand( "rpnotify -1 1 cmd:cin/end", qfalse );
				CE_Message( S_COLOR_GREEN "Sent the end to everyone" );
			}
		}
	}

	// a click on nothing leaves the name box
	if ( ce.click )
		ce.typing = qfalse;
	ce.click = qfalse;
	ce.wheel = 0;

	if ( !ce.flying )
		re->DrawStretchPic( ce.cursorX, ce.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

/*
===============================================================================

On the way to the renderer

===============================================================================
*/

qboolean CL_RpCinematic_Active( void ) {
	return rc.active;
}

// the HUD hidden, chat box and all: what's said shows as subtitles instead
qboolean CL_RpCinematic_Subtitles( void ) {
	return (qboolean)( rc.active && !rc.showHud );
}

// the editor's own camera is what's seen, not a preview
static qboolean CE_Viewing( void ) {
	return (qboolean)( ce.open && !ce.preview );
}

// the cgame's 2D is left out while the HUD is hidden, but what covers the whole screen,
// fades and overlays, still shows. Text is never the whole screen
qboolean CL_RpCinematic_Hide2D( float x, float y, float w, float h ) {
	if ( !CE_Viewing() && ( !rc.active || rc.showHud ) )
		return qfalse;
	return (qboolean)!( x <= 0.0f && y <= 0.0f && x + w >= SCREEN_WIDTH && y + h >= SCREEN_HEIGHT );
}

// how far off what the camera looks at is: the world, or a player or NPC in the way
static float RC_LookDistance( const vec3_t org, const vec3_t ang ) {
	vec3_t forward, end;
	trace_t tr;
	float best;

	AngleVectors( ang, forward, NULL, NULL );
	VectorMA( org, 8192.0f, forward, end );
	CM_BoxTrace( &tr, org, end, vec3_origin, vec3_origin, 0, MASK_SOLID, qfalse );
	best = tr.fraction * 8192.0f;
	// the player too, drawn in third person, though the snapshot doesn't list them
	for ( int i = -1; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = i >= 0 ? &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )] : NULL;
		vec3_t to, mid;
		float along;

		if ( es && es->eType != ET_PLAYER && es->eType != ET_NPC )
			continue;
		VectorCopy( es ? es->pos.trBase : cl.snap.ps.origin, mid );
		mid[2] += 16.0f;
		VectorSubtract( mid, org, to );
		along = DotProduct( to, forward );
		if ( along <= 8.0f || along >= best )
			continue;
		VectorMA( to, -along, forward, to );
		if ( VectorLength( to ) < 36.0f )
			best = along;
	}
	return best;
}

// the depth of field the camera at org has now; qfalse if none
static qboolean RC_DepthOfField( const vec3_t org, const vec3_t ang, qboolean dof, float focus, float *now, int *time,
	float range, float blur ) {
	if ( !dof || !re->ext.SetDepthOfField )
		return qfalse;
	if ( focus <= 0.0f ) {
		const float to = RC_LookDistance( org, ang );
		const float dt = Com_Clamp( 0.0f, 0.2f, ( cls.realtime - *time ) * 0.001f );

		// eased onto what's looked at, not snapping from one thing to the next
		if ( *now <= 0.0f || dt <= 0.0f )
			*now = to;
		else
			*now += ( to - *now ) * Com_Clamp( 0.0f, 1.0f, dt * RC_DOF_FOCUS_RATE );
		*time = cls.realtime;
		focus = *now;
	}
	re->ext.SetDepthOfField( focus, range, blur );
	return qtrue;
}

static void RC_NoDepthOfField( void ) {
	if ( re->ext.SetDepthOfField )
		re->ext.SetDepthOfField( 0.0f, 1.0f, 0.0f );
}

// a strip of a wipe, from a0 to a1 of the way across from the side it comes from
static void RC_WipeStrip( int type, float a0, float a1 ) {
	a0 = Com_Clamp( 0.0f, 1.0f, a0 );
	a1 = Com_Clamp( 0.0f, 1.0f, a1 );
	if ( a1 <= a0 )
		return;
	switch ( type ) {
	case RC_TR_LEFT:
		re->DrawStretchPic( a0 * SCREEN_WIDTH, 0, ( a1 - a0 ) * SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 0, cls.whiteShader );
		break;
	case RC_TR_RIGHT:
		re->DrawStretchPic( ( 1.0f - a1 ) * SCREEN_WIDTH, 0, ( a1 - a0 ) * SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 0, cls.whiteShader );
		break;
	case RC_TR_TOP:
		re->DrawStretchPic( 0, a0 * SCREEN_HEIGHT, SCREEN_WIDTH, ( a1 - a0 ) * SCREEN_HEIGHT, 0, 0, 0, 0, cls.whiteShader );
		break;
	default:
		re->DrawStretchPic( 0, ( 1.0f - a1 ) * SCREEN_HEIGHT, SCREEN_WIDTH, ( a1 - a0 ) * SCREEN_HEIGHT, 0, 0, 0, 0, cls.whiteShader );
		break;
	}
}

// a transition's colour over the screen: faded in by how far it is, or wiped across, its edge
// sweeping from the side; coming in, the picture is behind the edge, going out, the colour is
static void RC_DrawCover( const rcTrans_t *tr, float progress, qboolean in ) {
	const int steps = 12;
	const float soft = RC_WIPE_SOFT, step = soft / steps;
	// the edge, from just before the side to just past the other one, for its soft part to
	// come on and leave the screen too
	const float e = progress * ( 1.0f + soft ) - soft;
	vec4_t color = { tr->color[0] / 255.0f, tr->color[1] / 255.0f, tr->color[2] / 255.0f, 1.0f };

	if ( tr->type == RC_TR_FADE ) {
		color[3] = in ? 1.0f - progress : progress;
		if ( color[3] <= 0.0f )
			return;
		re->SetColor( color );
		re->DrawStretchPic( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0, 0, 0, cls.whiteShader );
		re->SetColor( NULL );
		return;
	}
	re->SetColor( color );
	if ( in )
		RC_WipeStrip( tr->type, e + soft, 1.0f );
	else
		RC_WipeStrip( tr->type, 0.0f, e );
	for ( int i = 0; i < steps; i++ ) {
		color[3] = in ? ( i + 0.5f ) / steps : 1.0f - ( i + 0.5f ) / steps;
		re->SetColor( color );
		RC_WipeStrip( tr->type, e + i * step, e + ( i + 1 ) * step );
	}
	re->SetColor( NULL );
}

// the shot playing coming in or going out, ms into it
static void RC_DrawTransitions( const rcShot_t *shot, int ms ) {
	const int total = RC_ShotMs( shot );

	if ( shot->in.type != RC_TR_CUT && shot->in.ms > 0 && ms < shot->in.ms )
		RC_DrawCover( &shot->in, RC_Ease( (float)ms / shot->in.ms, qfalse ), qtrue );
	else if ( shot->out.type != RC_TR_CUT && shot->out.ms > 0 && ms >= total - shot->out.ms )
		RC_DrawCover( &shot->out, RC_Ease( (float)( ms - ( total - shot->out.ms ) ) / shot->out.ms, qfalse ), qfalse );
}

// the cgame's main view: the player's is kept to start from, the camera's drawn instead.
// qtrue if it was drawn here, or left out
qboolean CL_RpCinematic_RenderScene( const refdef_t *fd ) {
	refdef_t view;

	// the HUD's 3D bits, heads and items
	if ( fd->rdflags & RDF_NOWORLDMODEL )
		return (qboolean)( CE_Viewing() || ( rc.active && !rc.showHud ) );
	if ( fd->rdflags & RDF_AUTOMAP )
		return qfalse;

	if ( !rc.active && !( fd->rdflags & RDF_SKYBOXPORTAL ) ) {
		rc.haveView = qtrue;
		VectorCopy( fd->vieworg, rc.viewOrg );
		VectorCopy( fd->viewangles, rc.viewAng );
		rc.viewFovY = fd->fov_y;
	}
	if ( CE_Viewing() ) {
		CE_RenderScene( fd );
		return qtrue;
	}
	if ( !rc.active )
		return qfalse;

	RC_Update();
	view = *fd;
	view.fov_y = rc.fovY;
	view.fov_x = RC_FovX( rc.fovY, (float)fd->width, (float)fd->height );
	VectorCopy( rc.ang, view.viewangles );
	AnglesToAxis( rc.ang, view.viewaxis );
	// the sky portal keeps its own origin but looks the way the camera does
	if ( !( fd->rdflags & RDF_SKYBOXPORTAL ) ) {
		VectorCopy( rc.org, view.vieworg );
		view.viewContents = CM_PointContents( rc.org, 0 );
		// the snapshot's area mask is from the player's position, the camera can be anywhere
		memset( view.areamask, 0, sizeof( view.areamask ) );
		if ( RC_DepthOfField( rc.org, rc.ang, rc.dof, rc.dofFocus, &rc.dofNow, &rc.dofTime, rc.dofRange, rc.dofBlur ) ) {
			re->RenderScene( &view );
			RC_NoDepthOfField();
			return qtrue;
		}
	}
	re->RenderScene( &view );
	return qtrue;
}

// the camera, for the effects system to cull against
qboolean CL_RpCinematic_Camera( vec3_t origin, vec3_t angles ) {
	if ( CE_Viewing() ) {
		VectorCopy( ce.camOrg, origin );
		VectorCopy( ce.camAng, angles );
		return qtrue;
	}
	if ( !rc.active )
		return qfalse;
	RC_Update();
	VectorCopy( rc.org, origin );
	VectorCopy( rc.ang, angles );
	return qtrue;
}

// what the cgame adds to the scene, NULL to leave it out: a view weapon would float where
// the player stands, the frame before third person takes over
const refEntity_t *CL_RpCinematic_Entity( const refEntity_t *ent, refEntity_t *copy ) {
	if ( ( rc.active || ce.open ) && ( ent->renderfx & RF_FIRST_PERSON ) )
		return NULL;
	return ent;
}

void CL_RpCinematic_Frame( void ) {
	RC_RunQueue();
	CE_Frame();
}

// the black bars, over the scene and under the menus, then the editor
void CL_RpCinematic_Draw( void ) {
	static const vec4_t black = { 0.0f, 0.0f, 0.0f, 1.0f };
	const float h = RC_BarsNow() * RC_BAR_FRAC * SCREEN_HEIGHT;

	if ( h >= 0.5f && !CE_Viewing() ) {
		re->SetColor( black );
		re->DrawStretchPic( 0, 0, SCREEN_WIDTH, h, 0, 0, 0, 0, cls.whiteShader );
		re->DrawStretchPic( 0, SCREEN_HEIGHT - h, SCREEN_WIDTH, h, 0, 0, 0, 0, cls.whiteShader );
		re->SetColor( NULL );
	}
	// the shot fading or wiping in or out, over the bars too
	if ( rc.active && rc.trans && !CE_Viewing() )
		RC_DrawTransitions( &rc.shot, cls.realtime - rc.start );
	if ( CL_RpCinematic_Subtitles() && !CE_Viewing() )
		CL_RpHooks_DrawSubtitles();
	if ( ce.open )
		CE_DrawEditor();
}

/*
===============================================================================

Commands

===============================================================================
*/

// cincam here: the spot of the view as a hook writes it; cincam stop: leave the cinematic;
// cincam edit [shot]: the camera editor; cincam [local] play <shot>: a saved shot;
// cincam local <verb> [parts]: try it on this screen only; cincam <verb> [parts]: send it
static void CL_CinCam_f( void ) {
	const char *verb = Cmd_Argv( 1 ), *rest = Cmd_ArgsFrom( 2 );
	qboolean local = qfalse;
	char hook[MAX_STRING_CHARS];

	if ( !Q_stricmp( verb, "here" ) ) {
		vec3_t org, ang;
		float fovY;

		if ( ce.open ) {
			VectorCopy( ce.camOrg, org );
			VectorCopy( ce.camAng, ang );
			fovY = RC_FovY( ce.camFov );
		} else if ( rc.active ) {
			RC_Update();
			VectorCopy( rc.org, org );
			VectorCopy( rc.ang, ang );
			fovY = rc.fovY;
		} else if ( rc.haveView ) {
			VectorCopy( rc.viewOrg, org );
			VectorCopy( rc.viewAng, ang );
			fovY = rc.viewFovY;
		} else {
			Com_Printf( "Join a server first.\n" );
			return;
		}
		Com_Printf( "%i %i %i %i %i %i %i\n", RC_Round( org[0] ), RC_Round( org[1] ), RC_Round( org[2] ),
			RC_Round( AngleNormalize180( ang[PITCH] ) ), RC_Round( AngleNormalize360( ang[YAW] ) ),
			RC_Round( AngleNormalize180( ang[ROLL] ) ), RC_Round( RC_FovX( fovY, 4.0f, 3.0f ) ) );
		return;
	}
	if ( !Q_stricmp( verb, "stop" ) ) {
		CL_RpCinematic_Stop();
		return;
	}
	if ( !Q_stricmp( verb, "edit" ) ) {
		if ( ce.open && Cmd_Argc() < 3 )
			CL_RpCinematic_EditorClose();
		else
			CL_RpCinematic_Edit( Cmd_Argv( 2 ), qfalse );
		return;
	}
	if ( !Q_stricmp( verb, "local" ) ) {
		local = qtrue;
		verb = Cmd_Argv( 2 );
		rest = Cmd_ArgsFrom( 3 );
	}
	if ( !verb[0] ) {
		Com_Printf( "cincam edit [shot] : the camera editor, to make shots in\n"
			"cincam [local] play <shot> : a shot made there (local: on this screen only, to try it)\n"
			"cincam here : the spot you look from, as x y z pitch yaw roll fov\n"
			"cincam stop : leave the cinematic, on this screen\n"
			"cincam [local] start [/hud] [/bars] : the cinematic starts\n"
			"cincam [local] path [/linear] [/look <x y z>] /<spot> [/<seconds> /<spot>...]\n"
			"cincam [local] move <seconds> [/linear] /<spot> [/<seconds> /<spot>...]\n"
			"cincam [local] cut <spot>\n"
			"cincam [local] orbit <seconds> [/linear] /<x y z> /<radius> /<height> /<from yaw> /<degrees> [/<fov> [/<end fov>]]\n"
			"cincam [local] lookat <x y z>|off\n"
			"cincam [local] fov <fov> [/<seconds>]\n"
			"cincam [local] bars on|off [/<seconds>]\n"
			"cincam [local] hud on|off\n"
			"cincam [local] end\n"
			"A spot is x y z [pitch yaw [roll [fov]]]; parts are separated by /\n"
			"Before its spots a path or orbit can have /dof <focus|0 for what it sees> <range> <blur 1-10>\n"
			"and /in|out <cut|fade|left|right|top|bottom> <seconds> <r g b>\n" );
		return;
	}
	if ( cls.state != CA_ACTIVE ) {
		Com_Printf( "Not connected to a server.\n" );
		return;
	}

	if ( !Q_stricmp( verb, "play" ) ) {
		const char *name = local ? Cmd_Argv( 3 ) : Cmd_Argv( 2 );

		if ( !CL_RpCinematic_ShotHook( name, hook, sizeof( hook ), NULL ) ) {
			Com_Printf( "No shot called \"%s\" in %s/\n", name, RC_SHOT_DIR );
			return;
		}
	} else {
		Com_sprintf( hook, sizeof( hook ), rest[0] ? "cin/%s/%s" : "cin/%s", verb, rest );
	}
	if ( local ) {
		RC_RunLocal( hook );
		if ( !Q_stricmp( verb, "play" ) )
			CL_RpCinematic_ShotWalks( Cmd_Argv( 3 ), qtrue );
		return;
	}
	if ( !CL_RpHooks_Experimental() ) {
		Com_Printf( S_COLOR_YELLOW "Not sent: " RP_EXPERIMENTAL_OFF " in the storyteller or NPC manager\n" );
		return;
	}
	// the server takes a // for a comment
	if ( strstr( hook, "//" ) ) {
		Com_Printf( S_COLOR_YELLOW "Not sent: an empty part would make a // the server cuts the line at\n" );
		return;
	}
	CL_AddReliableCommand( va( "rpnotify -1 1 cmd:%s", hook ), qfalse );
	if ( !Q_stricmp( verb, "play" ) )
		CL_RpCinematic_ShotWalks( Cmd_Argv( 2 ), qfalse );
}

void CL_RpCinematic_Init( void ) {
	memset( &rc, 0, sizeof( rc ) );
	memset( &ce, 0, sizeof( ce ) );
	ce.sel = -1;
	ce.bars = qtrue;
	ce.camFov = RC_DEFAULT_FOV;
	RC_ForgetShots();
	rpc_cinematics = Cvar_Get( "rpc_cinematics", "1", CVAR_ARCHIVE_ND, "Let cinematics sent through RPMod take over the camera" );
	rpc_cinThirdPerson = Cvar_Get( "rpc_cinThirdPerson", "0", CVAR_ARCHIVE_ND | CVAR_INTERNAL, "" );
	// a cinematic was cut short by a crash: first person back
	RC_ThirdPerson( qfalse );
	Cmd_AddCommand( "cincam", CL_CinCam_f, "The cinematic camera: make shots in its editor, send them, or leave the one playing" );
}

void CL_RpCinematic_Shutdown( void ) {
	Cmd_RemoveCommand( "cincam" );
}

// leaving the map or the server ends it, bars, editor and all
void CL_RpCinematic_Reset( void ) {
	CL_RpCinematic_EditorClose();
	rc.active = qfalse;
	rc.playing = qfalse;
	rc.bars = qfalse;
	rc.barsDuration = 0;
	rc.haveView = qfalse;
	RC_SyncThirdPerson();
}
