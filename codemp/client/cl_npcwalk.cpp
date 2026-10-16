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

// cl_npcwalk.cpp -- NPCs walking a route, on the screens of everyone with this client
//
// RPMod can only teleport an NPC, so the walk is shown here. An admin's
//   rpnotify -1 1 cmd:npc/walk/<entity number>/<speed>/<x y z>/<x y z>...
// has every client with this one move that NPC from the first spot through the others,
// <speed> units a second, before RPMod's cgame sees it: where it is, which way it faces,
// and its legs walking or running. The admin who sent it teleports the real NPC to each
// spot as it gets there (npc tele), so the server's NPC is never far behind the one seen,
// and players without this client see it hop from spot to spot.
//   cmd:npc/walk/<entity number>/<speed>/loop/<x y z>/<x y z>...
// walks it round and round: from the last spot back to the second, the route's first,
// until it's stopped. Clients from before it walk it once.
//   cmd:npc/walk/<entity number>[,<entity number>...]/stop
// ends them where they are.
//   cmd:npc/group/<speed>[/loop]/<entity number x y z>.../<x y z>...
// walks several NPCs at once, each from where it stands (x y z) as far from each spot as it
// stood from the group's middle, short of walls: the group keeps its shape along the route.
// Their teleports take turns, as the server only takes one command a second.
//
// An admin can also impersonate an NPC: their keys move it, the camera behind it, while they
// stand still; see "Impersonating an NPC" below.
//   npcmanager impersonate <npc> [display name] [local], npcmanager impersonate off
//
// Routes are made in the NPC manager (Manage tab, Walk) and kept in npcmanager/routes:
//   npcmanager walk <npc>[,<npc>...] <route> [walk|run|<speed>] [loop] [local]
//   npcmanager walk <npc>[,<npc>...] stop
//   npcmanager routes
// <npc> is a number or a targetname, a name being every NPC with it; more than one walk
// together. local plays it on this screen only, without sending anything.

#include "client.h"
#include "qcommon/cm_public.h"

#define NW_MAX_WALKS		16
#define NW_ROUTE_DIR		"npcmanager/routes"		// in the game folder
#define NW_ROUTE_EXT		".route"
#define NW_WALK_SPEED		90.0f
#define NW_RUN_SPEED		250.0f
#define NW_RUN_FROM			160.0f		// this fast and up, it runs
#define NW_TURN_MS			250			// the turn at each spot
#define NW_STEP_UP			48.0f		// the floor is looked for from this far above
#define NW_STEP_DOWN		256.0f		// to this far below
#define NW_GROUND_HEIGHT	25.0f		// an NPC's origin over the floor, when it can't be told
#define NW_CATCH_UP			32.0f		// the real NPC this near the end, the walk is over
#define NW_RELEASE_MS		4000		// or this long after it, whoever teleports it
#define NW_DRIVER_MS		10000		// a walk this client sent comes back this soon
#define NW_PENDING_MS		5000		// a name not in the NPC list, the list is waited for this long
#define NW_TRACE_MASK		(CONTENTS_SOLID|CONTENTS_PLAYERCLIP|CONTENTS_TERRAIN)
#define NW_QUIET_MS			4000		// its teleports' cloak and flash are kept off this long after a walk
#define NW_QUIET_NEAR		64.0f		// a teleport flash or cloak sound this near a spot is the walk's
#define NW_MAX_GROUP		NW_MAX_WALKS
#define NW_MAX_HOOK			900			// what rpnotify is trusted to carry

typedef struct {
	qboolean	active;
	int			num;				// the NPC's entity number
	vec3_t		pts[NW_MAX_ROUTE_POINTS + 1];	// where it starts, then the route
	int			arrive[NW_MAX_ROUTE_POINTS + 1];	// ms from the start it's at each
	int			numPts;
	float		speed;
	qboolean	loop;				// round again from the last spot, until stopped
	int			start;				// cl.serverTime it set off, or began this lap
	float		groundOfs;			// its origin over the floor
	qboolean	driver;				// this client teleports the real NPC
	int			teleNext;			// the next spot it gets to
	int			telePending;		// a spot reached, its teleport not sent yet; -1 none
	int			release;			// ms after the end the real NPC is waited for
	int			teleBit;			// its EF_TELEPORT_BIT when it set off, kept while it walks
	qboolean	cloaked;			// it was cloaked when it set off, and stays so
	qboolean	teleBitKnown;
	qboolean	cloakKnown;
	vec3_t		realPos;			// where the server's NPC is
} nwWalk_t;

static nwWalk_t nw[NW_MAX_WALKS];
static int nwActive;				// how many are, to skip the entities quickly
static int nwNextSend;				// cls.realtime the server takes another command
static int nwTurn;					// the walk whose teleport goes first, next time

// cl_npcWalkDebug 1: what changes on a walking NPC as the server teleports it, other than
// where it is and its legs, and the events near it
static cvar_t *cl_npcWalkDebug;
static struct {
	int			num;
	int			serverTime;
	entityState_t	es;
} nwSeen[NW_MAX_WALKS];

static const struct {
	const char	*name;
	size_t		ofs;
} nwDebugFields[] = {
	{ "eType", offsetof( entityState_t, eType ) },
	{ "eFlags", offsetof( entityState_t, eFlags ) },
	{ "eFlags2", offsetof( entityState_t, eFlags2 ) },
	{ "time", offsetof( entityState_t, time ) },
	{ "time2", offsetof( entityState_t, time2 ) },
	{ "origin2[0]", offsetof( entityState_t, origin2[0] ) },
	{ "origin2[1]", offsetof( entityState_t, origin2[1] ) },
	{ "origin2[2]", offsetof( entityState_t, origin2[2] ) },
	{ "angles2[0]", offsetof( entityState_t, angles2[0] ) },
	{ "angles2[1]", offsetof( entityState_t, angles2[1] ) },
	{ "angles2[2]", offsetof( entityState_t, angles2[2] ) },
	{ "bolt1", offsetof( entityState_t, bolt1 ) },
	{ "bolt2", offsetof( entityState_t, bolt2 ) },
	{ "trickedentindex", offsetof( entityState_t, trickedentindex ) },
	{ "trickedentindex2", offsetof( entityState_t, trickedentindex2 ) },
	{ "trickedentindex3", offsetof( entityState_t, trickedentindex3 ) },
	{ "trickedentindex4", offsetof( entityState_t, trickedentindex4 ) },
	{ "fireflag", offsetof( entityState_t, fireflag ) },
	{ "genericenemyindex", offsetof( entityState_t, genericenemyindex ) },
	{ "activeForcePass", offsetof( entityState_t, activeForcePass ) },
	{ "emplacedOwner", offsetof( entityState_t, emplacedOwner ) },
	{ "otherEntityNum", offsetof( entityState_t, otherEntityNum ) },
	{ "otherEntityNum2", offsetof( entityState_t, otherEntityNum2 ) },
	{ "groundEntityNum", offsetof( entityState_t, groundEntityNum ) },
	{ "constantLight", offsetof( entityState_t, constantLight ) },
	{ "loopSound", offsetof( entityState_t, loopSound ) },
	{ "loopIsSoundset", offsetof( entityState_t, loopIsSoundset ) },
	{ "soundSetIndex", offsetof( entityState_t, soundSetIndex ) },
	{ "modelGhoul2", offsetof( entityState_t, modelGhoul2 ) },
	{ "g2radius", offsetof( entityState_t, g2radius ) },
	{ "modelindex", offsetof( entityState_t, modelindex ) },
	{ "modelindex2", offsetof( entityState_t, modelindex2 ) },
	{ "clientNum", offsetof( entityState_t, clientNum ) },
	{ "frame", offsetof( entityState_t, frame ) },
	{ "saberInFlight", offsetof( entityState_t, saberInFlight ) },
	{ "saberEntityNum", offsetof( entityState_t, saberEntityNum ) },
	{ "saberMove", offsetof( entityState_t, saberMove ) },
	{ "forcePowersActive", offsetof( entityState_t, forcePowersActive ) },
	{ "saberHolstered", offsetof( entityState_t, saberHolstered ) },
	{ "isJediMaster", offsetof( entityState_t, isJediMaster ) },
	{ "isPortalEnt", offsetof( entityState_t, isPortalEnt ) },
	{ "solid", offsetof( entityState_t, solid ) },
	{ "event", offsetof( entityState_t, event ) },
	{ "eventParm", offsetof( entityState_t, eventParm ) },
	{ "owner", offsetof( entityState_t, owner ) },
	{ "teamowner", offsetof( entityState_t, teamowner ) },
	{ "shouldtarget", offsetof( entityState_t, shouldtarget ) },
	{ "powerups", offsetof( entityState_t, powerups ) },
	{ "weapon", offsetof( entityState_t, weapon ) },
	{ "legsFlip", offsetof( entityState_t, legsFlip ) },
	{ "torsoFlip", offsetof( entityState_t, torsoFlip ) },
	{ "forceFrame", offsetof( entityState_t, forceFrame ) },
	{ "generic1", offsetof( entityState_t, generic1 ) },
	{ "heldByClient", offsetof( entityState_t, heldByClient ) },
	{ "ragAttach", offsetof( entityState_t, ragAttach ) },
	{ "iModelScale", offsetof( entityState_t, iModelScale ) },
	{ "brokenLimbs", offsetof( entityState_t, brokenLimbs ) },
	{ "boltToPlayer", offsetof( entityState_t, boltToPlayer ) },
	{ "hasLookTarget", offsetof( entityState_t, hasLookTarget ) },
	{ "lookTarget", offsetof( entityState_t, lookTarget ) },
	{ "customRGBA[0]", offsetof( entityState_t, customRGBA[0] ) },
	{ "customRGBA[1]", offsetof( entityState_t, customRGBA[1] ) },
	{ "customRGBA[2]", offsetof( entityState_t, customRGBA[2] ) },
	{ "customRGBA[3]", offsetof( entityState_t, customRGBA[3] ) },
	{ "health", offsetof( entityState_t, health ) },
	{ "maxhealth", offsetof( entityState_t, maxhealth ) },
	{ "npcSaber1", offsetof( entityState_t, npcSaber1 ) },
	{ "npcSaber2", offsetof( entityState_t, npcSaber2 ) },
	{ "surfacesOn", offsetof( entityState_t, surfacesOn ) },
	{ "surfacesOff", offsetof( entityState_t, surfacesOff ) },
	{ "boneIndex1", offsetof( entityState_t, boneIndex1 ) },
	{ "boneIndex2", offsetof( entityState_t, boneIndex2 ) },
	{ "boneIndex3", offsetof( entityState_t, boneIndex3 ) },
	{ "boneIndex4", offsetof( entityState_t, boneIndex4 ) },
	{ "boneOrient", offsetof( entityState_t, boneOrient ) },
	{ "NPC_class", offsetof( entityState_t, NPC_class ) },
	{ "m_iVehicleNum", offsetof( entityState_t, m_iVehicleNum ) },
	{ "userInt1", offsetof( entityState_t, userInt1 ) },
	{ "userInt2", offsetof( entityState_t, userInt2 ) },
	{ "userInt3", offsetof( entityState_t, userInt3 ) },
	{ "userFloat1", offsetof( entityState_t, userFloat1 ) },
	{ "userFloat2", offsetof( entityState_t, userFloat2 ) },
	{ "userFloat3", offsetof( entityState_t, userFloat3 ) }
};

// a walk just over: the last teleports may still come, their cloak and flash are kept off
static struct {
	int			num;
	vec3_t		pts[NW_MAX_ROUTE_POINTS + 1];
	int			numPts;
	vec3_t		realPos;
	qboolean	cloaked;
	int			until;				// cl.serverTime; 0 when none
} nwQuiet[NW_MAX_WALKS];

// the walk this client sent, to know it when it comes back
static struct {
	int			num;
	int			time;
} nwSent;

// a walk waiting for the NPC list, its NPC given by name
static struct {
	char		target[MAX_QPATH];
	char		route[MAX_QPATH];
	float		speed;
	qboolean	local;
	qboolean	loop;
	int			time;				// 0 when none
} nwPending;

/*
===============================================================================

Routes on disk

===============================================================================
*/

static const char *NW_RoutePath( const char *name ) {
	return va( "%s/%s%s", NW_ROUTE_DIR, name, NW_ROUTE_EXT );
}

qboolean CL_NpcWalk_SaveRoute( const char *name, const vec3_t *pts, int numPts, float speed ) {
	const char *path = NW_RoutePath( name );
	fileHandle_t f = FS_FOpenFileWrite( path );
	const char *line;

	if ( !f ) {
		Com_Printf( S_COLOR_RED "Couldn't write %s\n", path );
		return qfalse;
	}
	line = va( "// NPC route, made in the NPC manager on %s\n// npcmanager walk <npc> %s [walk|run|<speed>] walks it\nspeed %g\n",
		Info_ValueForKey( cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO], "mapname" ), name, speed );
	FS_Write( line, strlen( line ), f );
	for ( int i = 0; i < numPts; i++ ) {
		line = va( "point %i %i %i\n", (int)floorf( pts[i][0] + 0.5f ), (int)floorf( pts[i][1] + 0.5f ), (int)floorf( pts[i][2] + 0.5f ) );
		FS_Write( line, strlen( line ), f );
	}
	FS_FCloseFile( f );
	return qtrue;
}

// the route's spots and speed; -1 if there's no such route
int CL_NpcWalk_LoadRoute( const char *name, vec3_t *pts, int maxPts, float *speed ) {
	char *buf;
	const char *p;
	int numPts = 0;

	if ( FS_ReadFile( NW_RoutePath( name ), (void **)&buf ) < 0 || !buf )
		return -1;
	*speed = NW_WALK_SPEED;
	for ( p = buf; *p; ) {
		char line[256];
		int len = 0;
		vec3_t v;

		while ( *p && *p != '\n' && len < (int)sizeof( line ) - 1 )
			line[len++] = *p++;
		line[len] = '\0';
		while ( *p && *p != '\n' )
			p++;
		if ( *p )
			p++;
		if ( !Q_stricmpn( line, "speed ", 6 ) )
			*speed = (float)atof( line + 6 );
		else if ( !Q_stricmpn( line, "point ", 6 ) && numPts < maxPts && sscanf( line + 6, "%f %f %f", &v[0], &v[1], &v[2] ) == 3 )
			VectorCopy( v, pts[numPts++] );
	}
	FS_FreeFile( buf );
	return numPts;
}

/*
===============================================================================

Walking

===============================================================================
*/

// the floor under a spot, as an NPC's origin would sit on it; qfalse if there's none near
static qboolean NW_Ground( const vec3_t pos, float *z ) {
	static const vec3_t mins = { -8, -8, 0 }, maxs = { 8, 8, 0 };
	vec3_t start, end;
	trace_t tr;

	VectorCopy( pos, start );
	start[2] += NW_STEP_UP;
	VectorCopy( pos, end );
	end[2] -= NW_STEP_DOWN;
	CM_BoxTrace( &tr, start, end, mins, maxs, 0, NW_TRACE_MASK, qfalse );
	if ( tr.allsolid || tr.startsolid || tr.fraction >= 1.0f )
		return qfalse;
	*z = tr.endpos[2];
	return qtrue;
}

static float NW_Yaw( const vec3_t from, const vec3_t to ) {
	vec3_t dir, angles;

	VectorSubtract( to, from, dir );
	dir[2] = 0;
	vectoangles( dir, angles );
	return angles[YAW];
}

// which way it faces leaving spot i (arriving, at the last)
static float NW_SpotYaw( const nwWalk_t *w, int i ) {
	if ( i + 1 < w->numPts )
		return NW_Yaw( w->pts[i], w->pts[i + 1] );
	return i > 0 ? NW_Yaw( w->pts[i - 1], w->pts[i] ) : 0.0f;
}

// where it is t ms after setting off, which way it faces, and how it moves; qfalse once it's there
static qboolean NW_At( const nwWalk_t *w, int t, vec3_t pos, float *yaw, vec3_t velocity ) {
	int i;

	VectorClear( velocity );
	if ( t <= 0 ) {
		VectorCopy( w->pts[0], pos );
		*yaw = NW_SpotYaw( w, 0 );
		return qtrue;
	}
	if ( t >= w->arrive[w->numPts - 1] ) {
		VectorCopy( w->pts[w->numPts - 1], pos );
		*yaw = NW_SpotYaw( w, w->numPts - 1 );
		return qfalse;
	}
	for ( i = 1; i < w->numPts - 1 && t >= w->arrive[i]; i++ )
		;
	// on its way from spot i-1 to spot i
	{
		const int span = w->arrive[i] - w->arrive[i - 1];
		const float frac = span > 0 ? (float)( t - w->arrive[i - 1] ) / span : 1.0f;
		const float segYaw = NW_Yaw( w->pts[i - 1], w->pts[i] );
		vec3_t dir;

		VectorSubtract( w->pts[i], w->pts[i - 1], dir );
		VectorMA( w->pts[i - 1], frac, dir, pos );
		VectorNormalize( dir );
		VectorScale( dir, w->speed, velocity );
		*yaw = segYaw;
		// turning the corner it just came round
		if ( i > 1 && t - w->arrive[i - 1] < NW_TURN_MS )
			*yaw = LerpAngle( NW_Yaw( w->pts[i - 2], w->pts[i - 1] ), segYaw, (float)( t - w->arrive[i - 1] ) / NW_TURN_MS );
	}
	return qtrue;
}

static void NW_Stop( nwWalk_t *w ) {
	if ( w->active ) {
		nwActive--;
		nwQuiet[w - nw].num = w->num;
		memcpy( nwQuiet[w - nw].pts, w->pts, sizeof( w->pts ) );
		nwQuiet[w - nw].numPts = w->numPts;
		VectorCopy( w->realPos, nwQuiet[w - nw].realPos );
		nwQuiet[w - nw].cloaked = w->cloaked;
		nwQuiet[w - nw].until = cl.serverTime + NW_QUIET_MS;
	}
	w->active = qfalse;
}

static nwWalk_t *NW_Find( int num ) {
	for ( int i = 0; i < NW_MAX_WALKS; i++ ) {
		if ( nw[i].active && nw[i].num == num )
			return &nw[i];
	}
	return NULL;
}

static void NW_Times( nwWalk_t *w ) {
	for ( int i = 0; i < w->numPts; i++ )
		w->arrive[i] = i ? w->arrive[i - 1] + (int)( Distance( w->pts[i - 1], w->pts[i] ) / w->speed * 1000.0f ) : 0;
}

// a walk round and round: at the last spot, another lap from it back to the route's first
static void NW_Lap( nwWalk_t *w, int serverTime ) {
	while ( w->loop && serverTime - w->start >= w->arrive[w->numPts - 1] ) {
		const int lap = w->arrive[w->numPts - 1];

		// the last spot's teleport is the lap's first, the same place
		if ( w->telePending >= 0 || w->teleNext < w->numPts )
			w->telePending = 0;
		w->teleNext = 1;
		VectorCopy( w->pts[w->numPts - 1], w->pts[0] );
		NW_Times( w );
		w->start += lap;
		if ( w->arrive[w->numPts - 1] <= 0 ) {
			w->loop = qfalse;	// every spot the same: nowhere to go round
			break;
		}
	}
}

// a new walk for the NPC, in place of one it's on; NULL if it can't
static nwWalk_t *NW_Start( int num, float speed, qboolean loop, const vec3_t *pts, int numPts, qboolean driver ) {
	nwWalk_t *w = NW_Find( num );
	float ground;
	qboolean cloaked = qfalse, cloakKnown = qfalse;

	if ( numPts < 2 || speed <= 0.0f )
		return NULL;
	// cloaked or not before the teleports began, not by one of them
	if ( w ) {
		cloaked = w->cloaked;
		cloakKnown = w->cloakKnown;
	} else {
		for ( int i = 0; i < NW_MAX_WALKS; i++ ) {
			if ( nwQuiet[i].until && cl.serverTime < nwQuiet[i].until && nwQuiet[i].num == num ) {
				cloaked = nwQuiet[i].cloaked;
				cloakKnown = qtrue;
			}
		}
	}
	if ( !w ) {
		for ( int i = 0; i < NW_MAX_WALKS && !w; i++ ) {
			if ( !nw[i].active )
				w = &nw[i];
		}
		if ( !w ) {
			Com_Printf( S_COLOR_YELLOW "NPC walk: too many NPCs walking at once\n" );
			return NULL;
		}
		nwActive++;
	}
	memset( w, 0, sizeof( *w ) );
	w->active = qtrue;
	w->num = num;
	w->speed = speed;
	w->loop = (qboolean)( loop && numPts > 2 );
	w->numPts = Q_min( numPts, NW_MAX_ROUTE_POINTS + 1 );
	for ( int i = 0; i < w->numPts; i++ )
		VectorCopy( pts[i], w->pts[i] );
	NW_Times( w );
	w->start = cl.serverTime;
	w->groundOfs = NW_GROUND_HEIGHT;
	if ( NW_Ground( pts[0], &ground ) )
		w->groundOfs = Com_Clamp( 0.0f, 128.0f, pts[0][2] - ground );
	w->driver = driver;
	w->teleNext = 1;
	w->telePending = -1;
	w->cloaked = cloaked;
	w->cloakKnown = cloakKnown;
	w->release = NW_RELEASE_MS;
	nwQuiet[w - nw].until = 0;
	return w;
}

// RPMod's npc tele cloaks the NPC, teleports it, then uncloaks it, with the cloak sounds
static qboolean NW_IsTeleportEvent( int event, int parm ) {
	event &= ~EV_EVENT_BITS;
	if ( event == EV_PLAYER_TELEPORT_IN || event == EV_PLAYER_TELEPORT_OUT )
		return qtrue;
	if ( event == EV_GENERAL_SOUND && parm > 0 && parm < MAX_SOUNDS ) {
		const int ofs = cl.gameState.stringOffsets[CS_SOUNDS + parm];

		return (qboolean)( ofs && Q_stristr( cl.gameState.stringData + ofs, "cloak" ) != NULL );
	}
	return qfalse;
}

// the NPC, or one of the spots, walked by now or just now
static qboolean NW_ImpNear( int num, const vec3_t org );

static qboolean NW_WalkedBy( int num, const vec3_t org, int serverTime ) {
	if ( NW_ImpNear( num, org ) )
		return qtrue;
	for ( int i = 0; i < NW_MAX_WALKS; i++ ) {
		const vec3_t *pts;
		const float *realPos;
		int numPts;

		if ( nw[i].active ) {
			pts = nw[i].pts;
			numPts = nw[i].numPts;
			realPos = nw[i].realPos;
			if ( nw[i].num == num )
				return qtrue;
		} else if ( nwQuiet[i].until && serverTime < nwQuiet[i].until ) {
			pts = nwQuiet[i].pts;
			numPts = nwQuiet[i].numPts;
			realPos = nwQuiet[i].realPos;
			if ( nwQuiet[i].num == num )
				return qtrue;
		} else {
			continue;
		}
		if ( Distance( realPos, org ) < NW_QUIET_NEAR )
			return qtrue;
		for ( int j = 0; j < numPts; j++ ) {
			if ( Distance( pts[j], org ) < NW_QUIET_NEAR )
				return qtrue;
		}
	}
	return qfalse;
}

static void NW_Debug( const entityState_t *es, int serverTime ) {
	if ( !cl_npcWalkDebug )
		cl_npcWalkDebug = Cvar_Get( "cl_npcWalkDebug", "0", 0, "Print what changes on NPCs walking a route" );
	if ( !cl_npcWalkDebug->integer )
		return;
	if ( es->eType > ET_EVENTS ) {
		for ( int i = 0; i < NW_MAX_WALKS; i++ ) {
			if ( nw[i].active && nwSeen[i].num == nw[i].num && Distance( nwSeen[i].es.pos.trBase, es->pos.trBase ) < 256.0f )
				Com_Printf( "npcwalk %i @%i: event %i (parm %i, clientNum %i) near it\n", nw[i].num, serverTime,
					es->eType - ET_EVENTS, es->eventParm, es->clientNum );
		}
		return;
	}
	for ( int i = 0; i < NW_MAX_WALKS; i++ ) {
		if ( !nw[i].active || nw[i].num != es->number )
			continue;
		if ( nwSeen[i].num == es->number && nwSeen[i].serverTime != serverTime ) {
			if ( serverTime - nwSeen[i].serverTime > 200 )
				Com_Printf( "npcwalk %i @%i: not in the snapshots for %i ms\n", es->number, serverTime, serverTime - nwSeen[i].serverTime );
			for ( size_t f = 0; f < ARRAY_LEN( nwDebugFields ); f++ ) {
				const int was = *(const int *)( (const byte *)&nwSeen[i].es + nwDebugFields[f].ofs );
				const int now = *(const int *)( (const byte *)es + nwDebugFields[f].ofs );

				if ( was != now )
					Com_Printf( "npcwalk %i @%i: %s %i -> %i (0x%x -> 0x%x)\n", es->number, serverTime, nwDebugFields[f].name, was, now, was, now );
			}
		}
		nwSeen[i].num = es->number;
		nwSeen[i].serverTime = serverTime;
		nwSeen[i].es = *es;
	}
}

// a snapshot's entity on its way to the cgame: an NPC walking is where the walk has it; qfalse
// when the cgame isn't to see it at all, the flash of the teleports that bring the real NPC along
static qboolean NW_ImpEntity( entityState_t *es, int serverTime );
static qboolean NW_GlideEntity( entityState_t *es, int serverTime );
static void NW_LetGoEntity( entityState_t *es, int serverTime );

qboolean CL_NpcWalk_Entity( entityState_t *es, int serverTime ) {
	nwWalk_t *w;
	vec3_t pos, velocity;
	float yaw, ground;
	int t;

	if ( nwActive )
		NW_Debug( es, serverTime );
	if ( es->eType > ET_EVENTS ) {
		if ( NW_IsTeleportEvent( es->eType - ET_EVENTS, es->eventParm ) && NW_WalkedBy( es->clientNum, es->pos.trBase, serverTime ) )
			return qfalse;
		return qtrue;
	}
	// impersonated, here or by someone else, or just let go of
	if ( NW_ImpEntity( es, serverTime ) || NW_GlideEntity( es, serverTime ) )
		return qtrue;
	NW_LetGoEntity( es, serverTime );
	if ( !nwActive || !( w = NW_Find( es->number ) ) ) {
		// just let go: the last teleport's uncloak may be yet to come
		for ( int i = 0; i < NW_MAX_WALKS; i++ ) {
			if ( nwQuiet[i].until && serverTime < nwQuiet[i].until && nwQuiet[i].num == es->number && !nw[i].active ) {
				if ( !nwQuiet[i].cloaked )
					es->powerups &= ~( 1 << PW_CLOAKED );
				if ( NW_IsTeleportEvent( es->event, es->eventParm ) )
					es->event = 0;
			}
		}
		return qtrue;
	}
	// the teleports neither cloak it, flash on it nor stop it being drawn smoothly
	if ( !w->teleBitKnown ) {
		w->teleBit = es->eFlags & EF_TELEPORT_BIT;
		w->teleBitKnown = qtrue;
	}
	if ( !w->cloakKnown ) {
		w->cloaked = (qboolean)( ( es->powerups & ( 1 << PW_CLOAKED ) ) != 0 );
		w->cloakKnown = qtrue;
	}
	if ( NW_IsTeleportEvent( es->event, es->eventParm ) )
		es->event = 0;
	es->eFlags = ( es->eFlags & ~EF_TELEPORT_BIT ) | w->teleBit;
	if ( !w->cloaked )
		es->powerups &= ~( 1 << PW_CLOAKED );
	VectorCopy( es->pos.trBase, w->realPos );
	NW_Lap( w, serverTime );
	t = serverTime - w->start;
	if ( !NW_At( w, t, pos, &yaw, velocity ) ) {
		// there: the real NPC is let back once it has caught up
		if ( Distance( es->pos.trBase, pos ) < NW_CATCH_UP || t > w->arrive[w->numPts - 1] + w->release ) {
			NW_Stop( w );
			return qtrue;
		}
	} else {
		const int anim = w->speed >= NW_RUN_FROM ? BOTH_RUN1 : BOTH_WALK1;

		// the torso too when it's doing what the legs are, standing; a weapon's pose is kept
		if ( es->torsoAnim == es->legsAnim )
			es->torsoAnim = anim;
		es->legsAnim = anim;
	}
	if ( NW_Ground( pos, &ground ) )
		pos[2] = ground + w->groundOfs;

	es->pos.trType = TR_INTERPOLATE;
	es->pos.trTime = serverTime;
	VectorCopy( pos, es->pos.trBase );
	VectorCopy( velocity, es->pos.trDelta );
	VectorCopy( pos, es->origin );
	es->apos.trType = TR_INTERPOLATE;
	VectorSet( es->apos.trBase, 0, yaw, 0 );
	VectorClear( es->apos.trDelta );
	VectorSet( es->angles, 0, yaw, 0 );
	es->speed = VectorLength( velocity );
	return qtrue;
}

static void NW_AddNumber( int *nums, int *count, int max, int num ) {
	for ( int i = 0; i < *count; i++ ) {
		if ( nums[i] == num )
			return;
	}
	if ( *count < max )
		nums[( *count )++] = num;
}

// the NPCs a target names: numbers and targetnames with commas between them, a name being
// every NPC with it in the NPC manager's list; how many, -1 if a name isn't in the list
static int NW_Resolve( const char *target, int *nums, int max ) {
	int count = 0;

	for ( const char *p = target; *p; ) {
		char item[MAX_QPATH];
		const char *type, *name;
		qboolean found = qfalse;
		int len = 0, num;

		while ( *p && *p != ',' ) {
			if ( len < (int)sizeof( item ) - 1 )
				item[len++] = *p;
			p++;
		}
		item[len] = '\0';
		if ( *p )
			p++;
		if ( !item[0] )
			continue;
		if ( Q_isanumber( item ) && !strchr( item, '.' ) ) {
			NW_AddNumber( nums, &count, max, atoi( item ) );
			continue;
		}
		for ( int i = 0; i < CL_NpcManager_NumNpcs(); i++ ) {
			if ( CL_NpcManager_GetNpc( i, &num, &type, &name ) && !Q_stricmp( name, item ) ) {
				NW_AddNumber( nums, &count, max, num );
				found = qtrue;
			}
		}
		if ( !found )
			return -1;
	}
	return count;
}

static int NW_CommandGap( void );

// the server takes another command now: the walks' and impersonation's are spaced out, and so
// are they from any other this client sent, the flood protection ignoring one too soon
static qboolean NW_CanSend( void ) {
	return (qboolean)( cls.state == CA_ACTIVE && cls.realtime >= nwNextSend && cls.realtime - CL_LastCommandTime() >= NW_CommandGap() );
}

static int NW_CommandGap( void ) {
	const char *flood = Info_ValueForKey( cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO], "sv_floodProtect" );
	int value;

	if ( !flood[0] )
		return 1100;	// not told, play safe
	value = atoi( flood );
	if ( value <= 0 )
		return 0;
	return ( value == 1 ? 1000 : value ) + 100;
}

/*
===============================================================================

Impersonating an NPC

The player stays where they are; their keys move the NPC instead, on this screen at once,
the camera behind it, and the real one is teleported after it as often as the server takes
a command. cmd:npc/puppet/<number>/on tells the other clients with this one to glide it
from teleport to teleport, walking or running, instead of it hopping; /off ends that.

===============================================================================
*/

#define NW_IMP_CAM_RANGE	110.0f		// the camera this far behind it
#define NW_IMP_CAM_HEIGHT	56.0f		// looking from this far over the floor it stands on
#define NW_IMP_TURN			720.0f		// degrees a second it turns to where it goes
#define NW_IMP_SEND_MOVE	16.0f		// moved this far, the real one is sent after it
#define NW_IMP_SEND_YAW		15.0f		// or turned this far
#define NW_IMP_STEP			18.0f		// a step this high it walks up
#define NW_MAX_GLIDES		8
#define NW_GLIDE_FAST		600.0f		// a teleport further than this in a command's time is one
#define NW_IMP_ANNOUNCE_MS	15000		// the hook's on again this often, for a copy lost or someone come since
#define NW_IMP_SPOTS		4			// teleported from and to lately, their flashes the impersonation's
#define NW_IMP_MIN_GAP		250			// ms between its teleports however often the server takes commands
#define NW_GLIDE_KEEP_MS	400			// legs kept walking this long past a glide, the next teleport due

typedef enum {
	NW_IMP_SENT,			// nothing left to send but where it goes
	NW_IMP_HOOK_ON,			// the others told to glide it, before it's teleported
	NW_IMP_LAST_TELE,		// stopped: where it was left, then the hook's off
	NW_IMP_HOOK_OFF
} nwImpSend_t;

// the time between the impersonation's commands: as often as the server takes them, and not
// so often as to flood it with teleports when it takes any number
static int NW_ImpGap( void ) {
	return Q_max( NW_CommandGap(), NW_IMP_MIN_GAP );
}

// cl_npcWalkDebug 1: what the impersonation sends, and how the NPCs others impersonate glide
static qboolean NW_ImpDebug( void ) {
	if ( !cl_npcWalkDebug )
		cl_npcWalkDebug = Cvar_Get( "cl_npcWalkDebug", "0", 0, "Print what changes on NPCs walking a route" );
	return (qboolean)( cl_npcWalkDebug->integer != 0 );
}

typedef struct {
	int			teleBit;			// its EF_TELEPORT_BIT when it was taken, kept
	qboolean	cloaked;			// cloaked when it was taken, and stays so
	qboolean	known;
} nwFx_t;

static struct {
	qboolean	active;
	qboolean	ending;				// let go: the real one is brought to where it was left
	qboolean	local;				// nothing sent
	int			num;
	qboolean	known;				// where it is taken from a snapshot yet
	vec3_t		pos;
	float		yaw;
	float		speed;				// how fast it goes now
	vec3_t		dir;				// and where
	float		groundOfs;
	vec3_t		realPos;			// where the server's NPC is
	vec3_t		sentPos;			// where it was last teleported to
	float		sentYaw;
	vec3_t		spots[NW_IMP_SPOTS];	// the ones before
	int			numSpots;
	int			nextAnnounce;		// cls.realtime the hook's on goes again
	vec3_t		frozen;				// the player's own view angles, kept while they stand there
	float		forward, right;		// what the keys ask, -1 to 1
	qboolean	run;
	int			lastTime;			// cls.realtime it last moved
	int			endTime;			// cls.realtime it's let go of, whatever happens
	nwImpSend_t	send;
	nwFx_t		fx;
} nwImp;

// an NPC someone else impersonates, glided from teleport to teleport
static struct {
	qboolean	active;
	int			num;
	qboolean	known;
	vec3_t		raw;				// where the server last put it
	vec3_t		prevRaw;			// and before
	int			rawTime;			// serverTime it did, the time between teleports telling the glide's
	vec3_t		lastVelocity;		// of the last glide, the legs kept walking until the next
	vec3_t		from, to;
	int			start, dur;			// serverTime
	float		groundOfs;
	int			until;				// serverTime it's let go of, once /off; 0 while on
	nwFx_t		fx;
} nwGlide[NW_MAX_GLIDES];

// an NPC impersonated or glided just let go of: its last teleport's cloak and flash may be yet to come
static struct {
	int			num;
	vec3_t		spots[2];			// where that teleport is from and to
	int			until;				// serverTime; 0 when none
	nwFx_t		fx;
} nwLetGo[NW_MAX_GLIDES + 1];

static cvar_t *cl_npcImpThirdPerson;	// 1 while cg_thirdPerson is ours to put back to 0, a crash included

// the cgame only draws the player's own body in third person; the camera is ours either way
static void NW_ImpThirdPerson( qboolean on ) {
	if ( !cl_npcImpThirdPerson )
		cl_npcImpThirdPerson = Cvar_Get( "cl_npcImpThirdPerson", "0", CVAR_ARCHIVE_ND | CVAR_INTERNAL, "" );
	if ( on ) {
		if ( Cvar_VariableIntegerValue( "cg_thirdPerson" ) || cl_npcImpThirdPerson->integer )
			return;
		Cvar_Set( "cl_npcImpThirdPerson", "1" );
		Cvar_Set( "cg_thirdPerson", "1" );
	} else if ( cl_npcImpThirdPerson->integer ) {
		Cvar_Set( "cg_thirdPerson", "0" );
		Cvar_Set( "cl_npcImpThirdPerson", "0" );
	}
}

// the teleports that bring it along neither cloak it, flash on it nor stop it being drawn smoothly
static void NW_Quieten( entityState_t *es, nwFx_t *fx ) {
	if ( !fx->known ) {
		fx->teleBit = es->eFlags & EF_TELEPORT_BIT;
		fx->cloaked = (qboolean)( ( es->powerups & ( 1 << PW_CLOAKED ) ) != 0 );
		fx->known = qtrue;
	}
	if ( NW_IsTeleportEvent( es->event, es->eventParm ) )
		es->event = 0;
	es->eFlags = ( es->eFlags & ~EF_TELEPORT_BIT ) | fx->teleBit;
	if ( !fx->cloaked )
		es->powerups &= ~( 1 << PW_CLOAKED );
}

// the NPC where it's shown, moving as it is, its legs too
static void NW_Place( entityState_t *es, const vec3_t pos, float yaw, const vec3_t velocity, int serverTime ) {
	const float speed = VectorLength( velocity );

	if ( speed > 1.0f ) {
		const int anim = speed >= NW_RUN_FROM ? BOTH_RUN1 : BOTH_WALK1;

		// the torso too when it's doing what the legs are, standing; a weapon's pose is kept
		if ( es->torsoAnim == es->legsAnim )
			es->torsoAnim = anim;
		es->legsAnim = anim;
	}
	es->pos.trType = TR_INTERPOLATE;
	es->pos.trTime = serverTime;
	VectorCopy( pos, es->pos.trBase );
	VectorCopy( velocity, es->pos.trDelta );
	VectorCopy( pos, es->origin );
	es->apos.trType = TR_INTERPOLATE;
	VectorSet( es->apos.trBase, 0, yaw, 0 );
	VectorClear( es->apos.trDelta );
	VectorSet( es->angles, 0, yaw, 0 );
	es->speed = speed;
}

static void NW_LetGo( int num, const nwFx_t *fx, const vec3_t a, const vec3_t b, int serverTime ) {
	int slot = 0;

	for ( int i = 0; i < NW_MAX_GLIDES + 1; i++ ) {
		if ( nwLetGo[i].num == num || serverTime >= nwLetGo[i].until ) {
			slot = i;
			break;
		}
	}
	nwLetGo[slot].num = num;
	nwLetGo[slot].fx = *fx;
	VectorCopy( a, nwLetGo[slot].spots[0] );
	VectorCopy( b, nwLetGo[slot].spots[1] );
	nwLetGo[slot].until = serverTime + NW_QUIET_MS;
}

// one just let go of: where the server has it, without its last teleport's cloak or flash
static void NW_LetGoEntity( entityState_t *es, int serverTime ) {
	for ( int i = 0; i < NW_MAX_GLIDES + 1; i++ ) {
		if ( nwLetGo[i].until && serverTime < nwLetGo[i].until && nwLetGo[i].num == es->number )
			NW_Quieten( es, &nwLetGo[i].fx );
	}
}

static float NW_GroundOfs( const vec3_t pos ) {
	float ground;

	if ( NW_Ground( pos, &ground ) )
		return Com_Clamp( 0.0f, 128.0f, pos[2] - ground );
	return NW_GROUND_HEIGHT;
}

// the NPC this client moves: qtrue if this is it, put where it's been moved to
static qboolean NW_ImpEntity( entityState_t *es, int serverTime ) {
	vec3_t velocity;

	if ( !nwImp.active || es->number != nwImp.num )
		return qfalse;
	NW_Quieten( es, &nwImp.fx );
	VectorCopy( es->pos.trBase, nwImp.realPos );
	if ( !nwImp.known ) {
		VectorCopy( es->pos.trBase, nwImp.pos );
		VectorCopy( es->pos.trBase, nwImp.sentPos );
		nwImp.yaw = nwImp.sentYaw = es->apos.trBase[YAW];
		nwImp.groundOfs = NW_GroundOfs( nwImp.pos );
		nwImp.known = qtrue;
	}
	// let go: the real one is back once it's where this one was left
	if ( nwImp.ending && ( cls.realtime > nwImp.endTime
		|| ( nwImp.send == NW_IMP_SENT && Distance( es->pos.trBase, nwImp.pos ) < NW_CATCH_UP ) ) ) {
		nwImp.active = qfalse;
		NW_LetGo( nwImp.num, &nwImp.fx, nwImp.spots[0], nwImp.sentPos, serverTime );
		return qtrue;
	}
	VectorScale( nwImp.dir, nwImp.speed, velocity );
	NW_Place( es, nwImp.pos, nwImp.yaw, velocity, serverTime );
	return qtrue;
}

static float NW_TurnTo( float from, float to, float step ) {
	const float delta = AngleDelta( to, from );

	if ( fabsf( delta ) <= step )
		return to;
	return AngleNormalize360( from + ( delta > 0.0f ? step : -step ) );
}

// moved along, sliding along walls and up steps, never off a floor it can't see
static void NW_SlideMove( vec3_t pos, const vec3_t delta, float groundOfs ) {
	vec3_t mins = { -15, -15, 0 }, maxs = { 15, 15, 0 }, move, end, was;
	float ground;
	trace_t tr;

	// from a step's height over the floor to a head's
	mins[2] = NW_IMP_STEP - groundOfs;
	maxs[2] = mins[2] + 40.0f;
	VectorCopy( pos, was );
	VectorCopy( delta, move );
	for ( int i = 0; i < 3; i++ ) {
		float into;

		VectorAdd( pos, move, end );
		CM_BoxTrace( &tr, pos, end, mins, maxs, 0, NW_TRACE_MASK, qfalse );
		if ( tr.startsolid || tr.allsolid ) {
			// already in something: out of it the way it goes
			VectorCopy( end, pos );
			break;
		}
		VectorCopy( tr.endpos, pos );
		if ( tr.fraction >= 1.0f )
			break;
		// what's left of the move, along the wall
		VectorScale( move, 1.0f - tr.fraction, move );
		into = DotProduct( move, tr.plane.normal );
		VectorMA( move, -into, tr.plane.normal, move );
		move[2] = 0.0f;
		if ( VectorLength( move ) < 0.5f )
			break;
	}
	if ( NW_Ground( pos, &ground ) )
		pos[2] = ground + groundOfs;
	else
		VectorCopy( was, pos );
}

// the keys move it, the way the camera looks
static void NW_ImpMove( float dt ) {
	vec3_t angles, forward, right, wish, delta;

	VectorSet( angles, 0, cl.viewangles[YAW], 0 );
	AngleVectors( angles, forward, right, NULL );
	VectorScale( forward, nwImp.forward, wish );
	VectorMA( wish, nwImp.right, right, wish );
	wish[2] = 0.0f;
	if ( VectorNormalize( wish ) < 0.1f ) {
		nwImp.speed = 0.0f;
		return;
	}
	nwImp.speed = nwImp.run ? NW_RUN_SPEED : NW_WALK_SPEED;
	VectorCopy( wish, nwImp.dir );
	nwImp.yaw = NW_TurnTo( nwImp.yaw, RAD2DEG( atan2f( wish[1], wish[0] ) ), NW_IMP_TURN * dt );
	VectorScale( wish, nwImp.speed * dt, delta );
	NW_SlideMove( nwImp.pos, delta, nwImp.groundOfs );
}

static void NW_ImpTele( void ) {
	// where it's teleported from, its flash there kept off too
	memmove( &nwImp.spots[1], &nwImp.spots[0], sizeof( nwImp.spots[0] ) * ( NW_IMP_SPOTS - 1 ) );
	VectorCopy( nwImp.sentPos, nwImp.spots[0] );
	nwImp.numSpots = Q_min( nwImp.numSpots + 1, NW_IMP_SPOTS );
	CL_AddReliableCommand( va( "npc tele %i %i %i %i %i", (int)floorf( nwImp.pos[0] + 0.5f ), (int)floorf( nwImp.pos[1] + 0.5f ),
		(int)floorf( nwImp.pos[2] + 0.5f ), (int)AngleNormalize360( nwImp.yaw ), nwImp.num ), qfalse );
	VectorCopy( nwImp.pos, nwImp.sentPos );
	nwImp.sentYaw = nwImp.yaw;
	nwNextSend = cls.realtime + NW_ImpGap();
	if ( NW_ImpDebug() )
		Com_Printf( "npcimp %i: tele %.0f %.0f %.0f yaw %.0f @%i\n", nwImp.num, nwImp.pos[0], nwImp.pos[1], nwImp.pos[2], nwImp.yaw, cls.realtime );
}

// commands this client sends while impersonating, held for their turn: one too soon after
// another would be ignored by the server, chat included
#define NW_MAX_HELD			4
static struct {
	char		cmd[NW_MAX_HELD][MAX_STRING_CHARS];
	int			num;
	qboolean	releasing;
} nwHeld;

// a command about to go to the server; qtrue if it waits its turn instead
qboolean CL_NpcWalk_HoldCommand( const char *cmd ) {
	if ( nwHeld.releasing || !nwImp.active || nwImp.local || cls.state != CA_ACTIVE )
		return qfalse;
	if ( ( !nwHeld.num && NW_CanSend() ) || nwHeld.num >= NW_MAX_HELD )
		return qfalse;
	// chat, said as the NPC or not; anything else goes as it would
	if ( Q_stricmpn( cmd, "say", 3 ) && Q_stricmpn( cmd, "tell ", 5 ) && Q_stricmpn( cmd, "rpnotify ", 9 ) )
		return qfalse;
	if ( NW_ImpDebug() )
		Com_Printf( "npcimp: held %s\n", cmd );
	Q_strncpyz( nwHeld.cmd[nwHeld.num++], cmd, sizeof( nwHeld.cmd[0] ) );
	return qtrue;
}

// the first one held goes, when the server takes it; qtrue if it went
static qboolean NW_ReleaseHeld( void ) {
	if ( !nwHeld.num || !NW_CanSend() )
		return qfalse;
	nwHeld.releasing = qtrue;
	CL_AddReliableCommand( nwHeld.cmd[0], qfalse );
	nwHeld.releasing = qfalse;
	memmove( nwHeld.cmd[0], nwHeld.cmd[1], sizeof( nwHeld.cmd[0] ) * ( NW_MAX_HELD - 1 ) );
	nwHeld.num--;
	nwNextSend = cls.realtime + NW_ImpGap();
	return qtrue;
}

// every frame: it moves, and the real one is sent after it; qtrue if a command went
static qboolean NW_ImpFrame( void ) {
	const float dt = Com_Clamp( 0.0f, 0.1f, ( cls.realtime - nwImp.lastTime ) * 0.001f );

	if ( !nwImp.active )
		return qfalse;
	nwImp.lastTime = cls.realtime;
	if ( !nwImp.ending && nwImp.known && cls.state == CA_ACTIVE )
		NW_ImpMove( dt );
	if ( nwImp.local || !nwImp.known || !NW_CanSend() )
		return qfalse;
	switch ( nwImp.send ) {
	case NW_IMP_HOOK_ON:
		CL_AddReliableCommand( va( "rpnotify -1 1 cmd:npc/puppet/%i/on", nwImp.num ), qfalse );
		nwNextSend = cls.realtime + NW_ImpGap();
		if ( NW_ImpDebug() )
			Com_Printf( "npcimp %i: hook on @%i\n", nwImp.num, cls.realtime );
		nwImp.nextAnnounce = cls.realtime + NW_IMP_ANNOUNCE_MS;
		nwImp.send = NW_IMP_SENT;
		return qtrue;
	case NW_IMP_LAST_TELE:
		NW_ImpTele();
		nwImp.send = NW_IMP_HOOK_OFF;
		return qtrue;
	case NW_IMP_HOOK_OFF:
		CL_AddReliableCommand( va( "rpnotify -1 1 cmd:npc/puppet/%i/off", nwImp.num ), qfalse );
		nwNextSend = cls.realtime + NW_ImpGap();
		if ( NW_ImpDebug() )
			Com_Printf( "npcimp %i: hook off @%i\n", nwImp.num, cls.realtime );
		nwImp.send = NW_IMP_SENT;
		return qtrue;
	default:
		if ( nwImp.ending )
			return qfalse;
		if ( Distance( nwImp.pos, nwImp.sentPos ) > NW_IMP_SEND_MOVE || fabsf( AngleDelta( nwImp.yaw, nwImp.sentYaw ) ) > NW_IMP_SEND_YAW
			|| ( nwImp.speed <= 0.0f && ( Distance( nwImp.pos, nwImp.sentPos ) > 1.0f || fabsf( AngleDelta( nwImp.yaw, nwImp.sentYaw ) ) > 1.0f ) ) ) {
			NW_ImpTele();
			return qtrue;
		}
		// still: the hook again, for a client that missed it
		if ( cls.realtime >= nwImp.nextAnnounce ) {
			nwImp.send = NW_IMP_HOOK_ON;
			return NW_ImpFrame();
		}
		return qfalse;
	}
}

// a command about to go to the server: impersonating, the keys are the NPC's and the player
// stands still, looking where they did
void CL_NpcWalk_FilterCmd( usercmd_t *cmd ) {
	if ( !nwImp.active || nwImp.ending )
		return;
	nwImp.forward = cmd->forwardmove / 127.0f;
	nwImp.right = cmd->rightmove / 127.0f;
	nwImp.run = (qboolean)( abs( cmd->forwardmove ) > 64 || abs( cmd->rightmove ) > 64 );
	cmd->forwardmove = cmd->rightmove = cmd->upmove = 0;
	cmd->buttons &= BUTTON_TALK;
	cmd->generic_cmd = 0;
	for ( int i = 0; i < 3; i++ )
		cmd->angles[i] = ANGLE2SHORT( nwImp.frozen[i] );
}

// the camera behind it, where the mouse looks; qfalse when not impersonating
qboolean CL_NpcWalk_Camera( vec3_t origin, vec3_t angles ) {
	static const vec3_t mins = { -4, -4, -4 }, maxs = { 4, 4, 4 };
	vec3_t target, forward, end;
	trace_t tr;

	if ( !nwImp.active || nwImp.ending || !nwImp.known )
		return qfalse;
	VectorCopy( nwImp.pos, target );
	target[2] += NW_IMP_CAM_HEIGHT - nwImp.groundOfs;
	VectorCopy( cl.viewangles, angles );
	AngleVectors( angles, forward, NULL, NULL );
	VectorMA( target, -NW_IMP_CAM_RANGE, forward, end );
	CM_BoxTrace( &tr, target, end, mins, maxs, 0, MASK_SOLID, qfalse );
	VectorCopy( tr.endpos, origin );
	return qtrue;
}

// the cgame's main view, from behind the NPC instead; qtrue if it was drawn here
qboolean CL_NpcWalk_RenderScene( const refdef_t *fd ) {
	refdef_t view;
	vec3_t origin, angles;

	if ( fd->rdflags & ( RDF_NOWORLDMODEL | RDF_AUTOMAP ) )
		return qfalse;
	if ( !CL_NpcWalk_Camera( origin, angles ) )
		return qfalse;
	view = *fd;
	VectorCopy( angles, view.viewangles );
	AnglesToAxis( angles, view.viewaxis );
	// the sky portal keeps its own origin but looks the way the camera does
	if ( !( fd->rdflags & RDF_SKYBOXPORTAL ) ) {
		VectorCopy( origin, view.vieworg );
		view.viewContents = CM_PointContents( origin, 0 );
		// the snapshot's area mask is from the player's position, the camera can be anywhere
		memset( view.areamask, 0, sizeof( view.areamask ) );
	}
	re->RenderScene( &view );
	return qtrue;
}

qboolean CL_NpcWalk_Impersonating( void ) {
	return (qboolean)( nwImp.active && !nwImp.ending );
}

// the player takes the NPC over; qfalse if it isn't near enough to
qboolean CL_NpcWalk_Impersonate( int num, qboolean local ) {
	nwWalk_t *w = NW_Find( num );
	qboolean seen = qfalse;

	for ( int i = 0; i < cl.snap.numEntities && !seen; i++ )
		seen = (qboolean)( cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )].number == num );
	if ( !seen ) {
		Com_Printf( S_COLOR_YELLOW "NPC manager: NPC %i isn't near you; go where you can see it first\n", num );
		return qfalse;
	}
	if ( nwImp.active && !nwImp.ending )
		CL_NpcWalk_StopImpersonating();
	// a walk it's on ends where it is
	if ( w )
		NW_Stop( w );
	memset( &nwImp, 0, sizeof( nwImp ) );
	nwImp.active = qtrue;
	nwImp.num = num;
	nwImp.local = local;
	nwImp.lastTime = cls.realtime;
	VectorCopy( cl.viewangles, nwImp.frozen );
	NW_ImpThirdPerson( qtrue );
	// the others told first, when the server takes it
	nwImp.send = local ? NW_IMP_SENT : NW_IMP_HOOK_ON;
	return qtrue;
}

// the player is themselves again; the real NPC is brought to where it was left
void CL_NpcWalk_StopImpersonating( void ) {
	if ( !nwImp.active || nwImp.ending )
		return;
	nwImp.ending = qtrue;
	nwImp.speed = 0.0f;
	// never told, nothing to take back
	nwImp.send = nwImp.local || nwImp.send == NW_IMP_HOOK_ON ? NW_IMP_SENT : NW_IMP_LAST_TELE;
	nwImp.endTime = cls.realtime + NW_ImpGap() * 3 + NW_RELEASE_MS;
	VectorCopy( nwImp.frozen, cl.viewangles );
	NW_ImpThirdPerson( qfalse );
}

// where a glided NPC is shown at serverTime
static void NW_GlidePos( const int g, int serverTime, vec3_t pos ) {
	const float frac = nwGlide[g].dur > 0 ? Com_Clamp( 0.0f, 1.0f, (float)( serverTime - nwGlide[g].start ) / nwGlide[g].dur ) : 1.0f;
	vec3_t delta;

	VectorSubtract( nwGlide[g].to, nwGlide[g].from, delta );
	VectorMA( nwGlide[g].from, frac, delta, pos );
}

// an NPC someone else impersonates: from where it's shown to where the server last put it, in
// the time until the next teleport; qtrue if this is one
static qboolean NW_GlideEntity( entityState_t *es, int serverTime ) {
	vec3_t pos, velocity;
	float yaw, ground;
	int g;

	for ( g = 0; g < NW_MAX_GLIDES; g++ ) {
		if ( nwGlide[g].active && nwGlide[g].num == es->number )
			break;
	}
	if ( g == NW_MAX_GLIDES )
		return qfalse;
	NW_Quieten( es, &nwGlide[g].fx );
	if ( !nwGlide[g].known ) {
		VectorCopy( es->pos.trBase, nwGlide[g].raw );
		VectorCopy( es->pos.trBase, nwGlide[g].prevRaw );
		VectorCopy( es->pos.trBase, nwGlide[g].from );
		VectorCopy( es->pos.trBase, nwGlide[g].to );
		nwGlide[g].dur = 0;
		nwGlide[g].rawTime = serverTime;
		nwGlide[g].groundOfs = NW_GroundOfs( es->pos.trBase );
		nwGlide[g].known = qtrue;
	} else if ( Distance( es->pos.trBase, nwGlide[g].raw ) > 1.0f ) {
		const int since = serverTime - nwGlide[g].rawTime, gap = NW_ImpGap();

		VectorCopy( nwGlide[g].raw, nwGlide[g].prevRaw );
		NW_GlidePos( g, serverTime, nwGlide[g].from );
		VectorCopy( es->pos.trBase, nwGlide[g].to );
		VectorCopy( es->pos.trBase, nwGlide[g].raw );
		nwGlide[g].start = serverTime;
		// as long as it took since the last one, the next as far behind; after a while
		// standing, the server's pace
		nwGlide[g].dur = since > 0 && since < gap * 2 + 500 ? since : gap;
		nwGlide[g].rawTime = serverTime;
		// too far for the time: a real teleport
		if ( Distance( nwGlide[g].from, nwGlide[g].to ) > NW_GLIDE_FAST * nwGlide[g].dur * 0.001f )
			nwGlide[g].dur = 0;
		if ( NW_ImpDebug() )
			Com_Printf( "npcglide %i: teleported %.0f units, %i ms since the last: glides %i ms\n", es->number,
				Distance( nwGlide[g].from, nwGlide[g].to ), since, nwGlide[g].dur );
	}
	NW_GlidePos( g, serverTime, pos );
	VectorClear( velocity );
	yaw = es->apos.trBase[YAW];
	if ( nwGlide[g].dur > 0 && serverTime < nwGlide[g].start + nwGlide[g].dur ) {
		VectorSubtract( nwGlide[g].to, nwGlide[g].from, velocity );
		velocity[2] = 0.0f;
		VectorScale( velocity, 1000.0f / nwGlide[g].dur, velocity );
		VectorCopy( velocity, nwGlide[g].lastVelocity );
		if ( VectorLength( velocity ) > 1.0f )
			yaw = NW_Yaw( nwGlide[g].from, nwGlide[g].to );
	} else if ( nwGlide[g].dur > 0 && serverTime < nwGlide[g].start + nwGlide[g].dur + NW_GLIDE_KEEP_MS ) {
		// there, the next teleport about due: still walking, not stopping between each
		VectorCopy( nwGlide[g].lastVelocity, velocity );
		if ( VectorLength( velocity ) > 1.0f )
			yaw = NW_Yaw( nwGlide[g].from, nwGlide[g].to );
	} else if ( nwGlide[g].until && serverTime > nwGlide[g].until ) {
		// let go, and still: the server's from now
		nwGlide[g].active = qfalse;
		NW_LetGo( nwGlide[g].num, &nwGlide[g].fx, nwGlide[g].prevRaw, nwGlide[g].raw, serverTime );
		return qtrue;
	}
	if ( NW_Ground( pos, &ground ) )
		pos[2] = ground + nwGlide[g].groundOfs;
	NW_Place( es, pos, yaw, velocity, serverTime );
	return qtrue;
}

// cmd:npc/puppet/<number>/on|off
void CL_NpcWalk_PuppetHook( char **argv, int argc ) {
	int num, slot = -1;

	if ( argc < 2 || !Q_isanumber( argv[0] ) ) {
		Com_DPrintf( "RP hook: npc puppet wants the NPC's number, and on or off\n" );
		return;
	}
	num = atoi( argv[0] );
	if ( NW_ImpDebug() )
		Com_Printf( "npcglide %i: hook %s%s\n", num, argv[1], nwImp.active && nwImp.num == num ? " (mine, not glided)" : "" );
	for ( int g = 0; g < NW_MAX_GLIDES; g++ ) {
		if ( nwGlide[g].active && nwGlide[g].num == num ) {
			if ( !Q_stricmp( argv[1], "off" ) )
				nwGlide[g].until = cl.serverTime + NW_ImpGap() * 2 + NW_GLIDE_KEEP_MS;
			else
				nwGlide[g].until = 0;
			return;
		}
		if ( !nwGlide[g].active && slot < 0 )
			slot = g;
	}
	// this client's own is moved here, not glided
	if ( Q_stricmp( argv[1], "on" ) || slot < 0 || ( nwImp.active && nwImp.num == num ) )
		return;
	memset( &nwGlide[slot], 0, sizeof( nwGlide[slot] ) );
	nwGlide[slot].active = qtrue;
	nwGlide[slot].num = num;
}

// a teleport's flash or cloak sound at an NPC impersonated, or by it
static qboolean NW_ImpNear( int num, const vec3_t org ) {
	if ( nwImp.active ) {
		if ( nwImp.num == num || Distance( org, nwImp.realPos ) < NW_QUIET_NEAR || Distance( org, nwImp.sentPos ) < NW_QUIET_NEAR )
			return qtrue;
		for ( int i = 0; i < nwImp.numSpots; i++ ) {
			if ( Distance( org, nwImp.spots[i] ) < NW_QUIET_NEAR )
				return qtrue;
		}
	}
	for ( int g = 0; g < NW_MAX_GLIDES; g++ ) {
		if ( nwGlide[g].active && ( nwGlide[g].num == num || Distance( org, nwGlide[g].from ) < NW_QUIET_NEAR
			|| Distance( org, nwGlide[g].raw ) < NW_QUIET_NEAR || Distance( org, nwGlide[g].prevRaw ) < NW_QUIET_NEAR ) )
			return qtrue;
	}
	for ( int i = 0; i < NW_MAX_GLIDES + 1; i++ ) {
		if ( nwLetGo[i].until && cl.serverTime < nwLetGo[i].until && ( nwLetGo[i].num == num
			|| Distance( org, nwLetGo[i].spots[0] ) < NW_QUIET_NEAR || Distance( org, nwLetGo[i].spots[1] ) < NW_QUIET_NEAR ) )
			return qtrue;
	}
	return qfalse;
}

// the sender teleports the real NPC to each spot it gets to; the flood protection may make it
// skip one, never the last
void CL_NpcWalk_Frame( void ) {
	// what the player said goes first, then the impersonated one
	if ( NW_ReleaseHeld() || NW_ImpFrame() )
		return;
	// the list read since the walk asked for it: the name is in it, or it isn't in the map
	if ( nwPending.time && cls.state == CA_ACTIVE
		&& ( CL_NpcManager_ListTime() > nwPending.time || cls.realtime - nwPending.time > NW_PENDING_MS ) ) {
		nwPending.time = 0;
		int nums[NW_MAX_GROUP];

		if ( NW_Resolve( nwPending.target, nums, NW_MAX_GROUP ) <= 0 )
			Com_Printf( S_COLOR_YELLOW "NPC walk: no NPC %s in the map\n", nwPending.target );
		else
			Cmd_ExecuteString( va( "npcmanager walk %s %s %g%s%s", nwPending.target, nwPending.route, nwPending.speed,
			nwPending.loop ? " loop" : "", nwPending.local ? " local" : "" ) );
	}
	if ( !nwActive )
		return;
	for ( int i = 0; i < NW_MAX_WALKS; i++ ) {
		nwWalk_t *w = &nw[i];
		int t;

		if ( !w->active )
			continue;
		NW_Lap( w, cl.serverTime );
		t = cl.serverTime - w->start;
		if ( t > w->arrive[w->numPts - 1] + w->release * 2 ) {
			NW_Stop( w );
			continue;
		}
		if ( !w->driver )
			continue;
		while ( w->teleNext < w->numPts && t >= w->arrive[w->teleNext] )
			w->telePending = w->teleNext++;
	}
	// one teleport at a time, the walks taking turns: the flood protection counts the
	// client's commands, not each NPC's
	if ( !NW_CanSend() )
		return;
	for ( int n = 0; n < NW_MAX_WALKS; n++ ) {
		nwWalk_t *w = &nw[( nwTurn + n ) % NW_MAX_WALKS];
		const float *p;

		if ( !w->active || !w->driver || w->telePending < 0 )
			continue;
		p = w->pts[w->telePending];
		CL_AddReliableCommand( va( "npc tele %i %i %i %i %i", (int)floorf( p[0] + 0.5f ), (int)floorf( p[1] + 0.5f ), (int)floorf( p[2] + 0.5f ),
			(int)AngleNormalize360( NW_SpotYaw( w, w->telePending ) ), w->num ), qfalse );
		w->telePending = -1;
		nwNextSend = cls.realtime + NW_CommandGap();
		nwTurn = (int)( w - nw + 1 ) % NW_MAX_WALKS;
		break;
	}
}

void CL_NpcWalk_Reset( void ) {
	// a map change or a crash while impersonating: the player's own view back
	if ( nwImp.active && !nwImp.ending )
		VectorCopy( nwImp.frozen, cl.viewangles );
	memset( &nwImp, 0, sizeof( nwImp ) );
	memset( nwGlide, 0, sizeof( nwGlide ) );
	memset( nwLetGo, 0, sizeof( nwLetGo ) );
	nwHeld.num = 0;
	NW_ImpThirdPerson( qfalse );
	memset( nw, 0, sizeof( nw ) );
	memset( nwQuiet, 0, sizeof( nwQuiet ) );
	memset( nwSeen, 0, sizeof( nwSeen ) );
	nwActive = 0;
	nwNextSend = nwTurn = 0;
	nwSent.time = 0;
	nwPending.time = 0;
}

/*
===============================================================================

The hook, and the command

===============================================================================
*/

// a walk this client sent, come back: it teleports the real NPC
static qboolean NW_Driver( int num ) {
	if ( !nwSent.time || nwSent.num != num || cls.realtime - nwSent.time >= NW_DRIVER_MS )
		return qfalse;
	nwSent.time = 0;
	return qtrue;
}

// the NPC stays where it is now, and the real one is brought there
static void NW_Halt( int num, qboolean driver ) {
	nwWalk_t *w = NW_Find( num );
	vec3_t pos, velocity;
	float yaw;

	if ( !w )
		return;
	NW_Lap( w, cl.serverTime );
	NW_At( w, cl.serverTime - w->start, pos, &yaw, velocity );
	VectorCopy( pos, w->pts[0] );
	VectorCopy( pos, w->pts[1] );
	w->numPts = 2;
	w->loop = qfalse;
	w->arrive[0] = w->arrive[1] = 0;
	w->start = cl.serverTime;
	w->driver = driver;
	w->teleNext = 1;
	w->telePending = -1;
}

// where one of a group goes at a spot: as far from it as it stood from the group's middle,
// short of a wall in between
void CL_NpcWalk_GroupSpot( const vec3_t spot, const vec3_t offset, vec3_t out ) {
	static const vec3_t mins = { -15, -15, -8 }, maxs = { 15, 15, 8 };
	vec3_t end;
	trace_t tr;

	VectorAdd( spot, offset, end );
	CM_BoxTrace( &tr, spot, end, mins, maxs, 0, NW_TRACE_MASK, qfalse );
	VectorCopy( tr.startsolid || tr.allsolid ? end : tr.endpos, out );
}

// the middle of where a group stands, the spot it's walked by
void CL_NpcWalk_GroupMiddle( const vec3_t *starts, int count, vec3_t middle ) {
	VectorClear( middle );
	for ( int i = 0; i < count; i++ )
		VectorAdd( middle, starts[i], middle );
	if ( count )
		VectorScale( middle, 1.0f / count, middle );
}

// the group sets off: each from where it stands, through the spots as far from each as it
// stood from the middle
static void NW_StartGroup( const int *nums, const vec3_t *starts, int count, const vec3_t *route, int numRoute,
	float speed, qboolean loop, qboolean driver ) {
	vec3_t middle;

	CL_NpcWalk_GroupMiddle( starts, count, middle );
	for ( int m = 0; m < count; m++ ) {
		vec3_t pts[NW_MAX_ROUTE_POINTS + 1], offset;
		nwWalk_t *w;

		VectorSubtract( starts[m], middle, offset );
		offset[2] = 0.0f;
		VectorCopy( starts[m], pts[0] );
		for ( int i = 0; i < numRoute; i++ )
			CL_NpcWalk_GroupSpot( route[i], offset, pts[i + 1] );
		w = NW_Start( nums[m], speed, loop, pts, numRoute + 1, driver );
		// the last teleports wait their turn
		if ( w )
			w->release = NW_RELEASE_MS + count * NW_CommandGap();
	}
}

// how many numbers an argument has, up to four
static int NW_Numbers( const char *arg, float *out ) {
	return sscanf( arg, "%f %f %f %f", &out[0], &out[1], &out[2], &out[3] );
}

// cmd:npc/group/<speed>[/loop]/<number x y z>.../<x y z>...
void CL_NpcWalk_GroupHook( char **argv, int argc ) {
	vec3_t route[NW_MAX_ROUTE_POINTS], starts[NW_MAX_GROUP];
	int nums[NW_MAX_GROUP], count = 0, numRoute = 0;
	qboolean loop = qfalse;

	if ( argc < 3 || !Q_isanumber( argv[0] ) ) {
		Com_DPrintf( "RP hook: npc group wants the speed, the NPCs and the spots\n" );
		return;
	}
	for ( int i = 1; i < argc; i++ ) {
		float v[4];
		const int n = NW_Numbers( argv[i], v );

		if ( !Q_stricmp( argv[i], "loop" ) ) {
			loop = qtrue;
		} else if ( n == 4 && count < NW_MAX_GROUP ) {
			nums[count] = (int)v[0];
			VectorSet( starts[count], v[1], v[2], v[3] );
			count++;
		} else if ( n == 3 && numRoute < NW_MAX_ROUTE_POINTS ) {
			VectorSet( route[numRoute], v[0], v[1], v[2] );
			numRoute++;
		}
	}
	if ( !count || !numRoute )
		return;
	NW_StartGroup( nums, starts, count, route, numRoute, (float)atof( argv[0] ), loop, NW_Driver( nums[0] ) );
}

// cmd:npc/walk/<entity number>/<speed>[/loop]/<x y z>/<x y z>...,
// cmd:npc/walk/<number>[,<number>...]/stop
void CL_NpcWalk_Hook( char **argv, int argc ) {
	vec3_t pts[NW_MAX_ROUTE_POINTS + 1];
	int num, numPts = 0;
	qboolean driver, loop = qfalse;

	if ( argc >= 2 && !Q_stricmp( argv[1], "stop" ) ) {
		int nums[NW_MAX_GROUP], count = 0;

		for ( const char *p = argv[0]; *p; ) {
			if ( *p >= '0' && *p <= '9' )
				NW_AddNumber( nums, &count, NW_MAX_GROUP, atoi( p ) );
			while ( *p >= '0' && *p <= '9' )
				p++;
			while ( *p && ( *p < '0' || *p > '9' ) )
				p++;
		}
		driver = count ? NW_Driver( nums[0] ) : qfalse;
		for ( int i = 0; i < count; i++ )
			NW_Halt( nums[i], driver );
		return;
	}
	if ( argc < 2 || !Q_isanumber( argv[0] ) ) {
		Com_DPrintf( "RP hook: npc walk wants the NPC's number, and its speed or stop\n" );
		return;
	}
	num = atoi( argv[0] );
	driver = NW_Driver( num );
	for ( int i = 2; i < argc && numPts < NW_MAX_ROUTE_POINTS + 1; i++ ) {
		if ( !Q_stricmp( argv[i], "loop" ) )
			loop = qtrue;
		else if ( sscanf( argv[i], "%f %f %f", &pts[numPts][0], &pts[numPts][1], &pts[numPts][2] ) == 3 )
			numPts++;
	}
	NW_Start( num, (float)atof( argv[1] ), loop, pts, numPts, driver );
}

// where the NPC is now: in the snapshot if it's near, else where the NPC manager's list saw it
static qboolean NW_NpcOrigin( int num, vec3_t out ) {
	for ( int i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];

		if ( es->number == num ) {
			VectorCopy( es->pos.trBase, out );
			return qtrue;
		}
	}
	return CL_NpcManager_NpcOrigin( num, out );
}

static float NW_ParseSpeed( const char *s, float route ) {
	if ( !Q_stricmp( s, "walk" ) )
		return NW_WALK_SPEED;
	if ( !Q_stricmp( s, "run" ) )
		return NW_RUN_SPEED;
	if ( Q_isanumber( s ) && atof( s ) > 0.0 )
		return (float)atof( s );
	return route;
}

static void NW_ListRoutes( void ) {
	char **files;
	int count;

	files = FS_ListFiles( NW_ROUTE_DIR, NW_ROUTE_EXT, &count );
	Com_Printf( "NPC routes in %s:\n", NW_ROUTE_DIR );
	for ( int i = 0; i < count; i++ ) {
		char name[MAX_QPATH];

		COM_StripExtension( files[i], name, sizeof( name ) );
		Com_Printf( "  %s\n", name );
	}
	if ( !count )
		Com_Printf( "  none yet: make one in the NPC manager's Manage tab, Walk\n" );
	FS_FreeFileList( files );
}

// npcmanager walk <npc>[,<npc>...] <route> [walk|run|<speed>] [loop] [local],
// npcmanager walk <npc>[,<npc>...] stop, npcmanager routes; argument 1 is walk or routes
void CL_NpcWalk_Command( void ) {
	vec3_t pts[NW_MAX_ROUTE_POINTS + 1];
	const char *target = Cmd_Argv( 2 ), *route = Cmd_Argv( 3 );
	qboolean local = qfalse, loop = qfalse;
	float speed, routeSpeed;
	int nums[NW_MAX_GROUP], count, num, numPts;
	char hook[MAX_STRING_CHARS];

	if ( !Q_stricmp( Cmd_Argv( 1 ), "routes" ) ) {
		NW_ListRoutes();
		return;
	}
	if ( Cmd_Argc() < 4 ) {
		Com_Printf( "usage: npcmanager walk <npc>[,<npc>...] <route> [walk|run|<speed>] [loop] [local]   npcmanager walk <npc>[,<npc>...] stop\n" );
		return;
	}
	for ( int i = 4; i < Cmd_Argc(); i++ ) {
		if ( !Q_stricmp( Cmd_Argv( i ), "local" ) )
			local = qtrue;
		else if ( !Q_stricmp( Cmd_Argv( i ), "loop" ) )
			loop = qtrue;
	}
	if ( cls.state != CA_ACTIVE ) {
		Com_Printf( "NPC walk: not in game\n" );
		return;
	}
	// rpnotify and npc tele are an admin's
	if ( !local && !CL_RpAdmin_Allow( "NPC manager", "npcmanager" ) )
		return;
	if ( !local && !CL_RpHooks_Experimental() ) {
		Com_Printf( S_COLOR_YELLOW "NPC walks are experimental: press TOGGLE EXPERIMENTAL in the npcmanager's Manage tab first, or add local to see it on your screen only\n" );
		return;
	}

	count = NW_Resolve( target, nums, NW_MAX_GROUP );
	if ( count < 0 ) {
		// a name the list doesn't have yet, an NPC spawned just now: the list is read first
		if ( nwPending.time ) {
			Com_Printf( S_COLOR_YELLOW "NPC walk: still looking for an NPC\n" );
			return;
		}
		Q_strncpyz( nwPending.target, target, sizeof( nwPending.target ) );
		Q_strncpyz( nwPending.route, route, sizeof( nwPending.route ) );
		nwPending.speed = Cmd_Argc() > 4 ? NW_ParseSpeed( Cmd_Argv( 4 ), 0.0f ) : 0.0f;
		nwPending.local = local;
		nwPending.loop = loop;
		nwPending.time = cls.realtime;
		CL_NpcManager_RequestList();
		return;
	}
	if ( !count ) {
		Com_Printf( S_COLOR_YELLOW "NPC walk: no NPC given\n" );
		return;
	}
	num = nums[0];

	if ( !Q_stricmp( route, "stop" ) ) {
		if ( local ) {
			for ( int i = 0; i < count; i++ ) {
				nwWalk_t *w = NW_Find( nums[i] );

				if ( w )
					NW_Stop( w );
			}
			return;
		}
		Com_sprintf( hook, sizeof( hook ), "rpnotify -1 1 cmd:npc/walk/%i", num );
		for ( int i = 1; i < count; i++ )
			Q_strcat( hook, sizeof( hook ), va( ",%i", nums[i] ) );
		Q_strcat( hook, sizeof( hook ), "/stop" );
		nwSent.num = num;
		nwSent.time = cls.realtime;
		nwNextSend = cls.realtime + NW_CommandGap();
		CL_AddReliableCommand( hook, qfalse );
		return;
	}

	numPts = CL_NpcWalk_LoadRoute( route, pts + 1, NW_MAX_ROUTE_POINTS, &routeSpeed );
	if ( numPts < 0 ) {
		Com_Printf( S_COLOR_YELLOW "NPC walk: no route %s; npcmanager routes lists them\n", NW_RoutePath( route ) );
		return;
	}
	if ( !numPts ) {
		Com_Printf( S_COLOR_YELLOW "NPC walk: no spots in %s\n", NW_RoutePath( route ) );
		return;
	}
	speed = Cmd_Argc() > 4 ? NW_ParseSpeed( Cmd_Argv( 4 ), routeSpeed ) : routeSpeed;
	if ( speed <= 0.0f )
		speed = routeSpeed > 0.0f ? routeSpeed : NW_WALK_SPEED;

	if ( count > 1 ) {
		vec3_t starts[NW_MAX_GROUP];
		int known = 0;

		// each from where it stands; one nowhere to be found stays out of it
		for ( int i = 0; i < count; i++ ) {
			if ( NW_NpcOrigin( nums[i], starts[known] ) )
				nums[known++] = nums[i];
			else
				Com_Printf( S_COLOR_YELLOW "NPC walk: NPC %i isn't in the NPC list, it stays\n", nums[i] );
		}
		if ( !known )
			return;
		if ( local ) {
			NW_StartGroup( nums, starts, known, pts + 1, numPts, speed, loop, qfalse );
			return;
		}
		Com_sprintf( hook, sizeof( hook ), "rpnotify -1 1 cmd:npc/group/%g%s", speed, loop ? "/loop" : "" );
		for ( int i = 0; i < known; i++ )
			Q_strcat( hook, sizeof( hook ), va( "/%i %i %i %i", nums[i], (int)floorf( starts[i][0] + 0.5f ), (int)floorf( starts[i][1] + 0.5f ), (int)floorf( starts[i][2] + 0.5f ) ) );
		for ( int i = 1; i <= numPts; i++ )
			Q_strcat( hook, sizeof( hook ), va( "/%i %i %i", (int)floorf( pts[i][0] + 0.5f ), (int)floorf( pts[i][1] + 0.5f ), (int)floorf( pts[i][2] + 0.5f ) ) );
		if ( strlen( hook ) > NW_MAX_HOOK ) {
			Com_Printf( S_COLOR_YELLOW "NPC walk: too many NPCs and spots for one message; fewer spots, or fewer NPCs at once\n" );
			return;
		}
		nwSent.num = nums[0];
		nwSent.time = cls.realtime;
		nwNextSend = cls.realtime + NW_CommandGap();
		CL_AddReliableCommand( hook, qfalse );
		return;
	}
	// from where it stands; out of sight, from the route's first spot
	if ( !NW_NpcOrigin( num, pts[0] ) )
		VectorCopy( pts[1], pts[0] );

	if ( local ) {
		NW_Start( num, speed, loop, pts, numPts + 1, qfalse );
		return;
	}
	Com_sprintf( hook, sizeof( hook ), "rpnotify -1 1 cmd:npc/walk/%i/%g%s", num, speed, loop ? "/loop" : "" );
	for ( int i = 0; i <= numPts; i++ )
		Q_strcat( hook, sizeof( hook ), va( "/%i %i %i", (int)floorf( pts[i][0] + 0.5f ), (int)floorf( pts[i][1] + 0.5f ), (int)floorf( pts[i][2] + 0.5f ) ) );
	nwSent.num = num;
	nwSent.time = cls.realtime;
	nwNextSend = cls.realtime + NW_CommandGap();
	CL_AddReliableCommand( hook, qfalse );
}
