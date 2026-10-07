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

// cl_fighterassist.cpp -- keeps a held takeoff jump from firing a fighter's boost
//
// Fighters read takeoff and boost from the same input: while the ship is slow and
// close to flat ground, holding jump lifts it, anywhere else jump fires the turbo.
// Held through the end of a takeoff, the same press turns into a boost.
//
// This lives in the engine rather than the game modules so it works whatever mod the
// server runs. It mirrors the stock takeoff test (FighterIsLaunching and
// ProcessMoveCommands in game/FighterNPC.c) on the latest snapshot and stops sending
// jump just before the ship leaves the takeoff zone, until the key is let go.
// +vehboost sends jump on its own so the boost can sit on a key of its own.

#include "client.h"
#include "qcommon/cm_public.h"

// mirrored from game/bg_vehicles.h and game/FighterNPC.c
#define FA_MIN_LANDING_SPEED		200.0f
#define FA_MIN_LANDING_SLOPE		0.8f
#define FA_MIN_TAKEOFF_FRACTION		0.7f
#define FA_LAND_MASK				(CONTENTS_SOLID|CONTENTS_MONSTERCLIP|CONTENTS_TERRAIN)

// cutting early only stops the climb a little short, cutting late fires the boost
#define FA_MARGIN					16.0f	// units
#define FA_LEAD_EXTRA_MS			50		// a snapshot interval on top of the ping
#define FA_LEAD_MAX_MS				500

typedef enum {
	FA_IDLE,	// jump is up
	FA_LIFT,	// press began in the takeoff zone, passed through while it lasts
	FA_HOLD,	// takeoff is over, swallowed until the key is let go
	FA_PASS,	// press began outside the takeoff zone, a deliberate boost
} faJump_t;

static struct {
	faJump_t	jump;
	char		vehName[MAX_QPATH];		// the .veh entry below was looked up for this vehicle
	qboolean	isFighter;
	float		landingHeight;
} fa;

static cvar_t *cl_fighterTakeoffGuard;

void CL_FighterAssist_Init( void ) {
	memset( &fa, 0, sizeof( fa ) );
	cl_fighterTakeoffGuard = Cvar_Get( "cl_fighterTakeoffGuard", "1", CVAR_ARCHIVE_ND, "Stop a jump held through a fighter takeoff from firing the boost" );
}

// cgame is going away: the next server can ship different .veh files
void CL_FighterAssist_Shutdown( void ) {
	memset( &fa, 0, sizeof( fa ) );
}

/*
===============================================================================

VEHICLE DATA

===============================================================================
*/

// reads the block named vehName out of one .veh file, the way VEH_LoadVehicle does
static qboolean FA_ParseVehFile( const char *path, const char *vehName ) {
	char *buf;
	const char *p, *token;
	char key[128];

	if ( FS_ReadFile( path, (void **)&buf ) < 0 || !buf )
		return qfalse;

	p = buf;
	COM_BeginParseSession( path );
	while ( 1 ) {
		token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			break;
		if ( Q_stricmp( token, vehName ) ) {
			SkipBracedSection( &p, 0 );
			continue;
		}

		if ( Q_stricmp( COM_ParseExt( &p, qtrue ), "{" ) )
			break;
		while ( 1 ) {
			SkipRestOfLine( &p );
			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] || !Q_stricmp( token, "}" ) )
				break;
			Q_strncpyz( key, token, sizeof( key ) );
			token = COM_ParseExt( &p, qtrue );
			if ( !Q_stricmp( key, "type" ) )
				fa.isFighter = (qboolean)!Q_stricmp( token, "VH_FIGHTER" );
			else if ( !Q_stricmp( key, "landingHeight" ) )
				fa.landingHeight = atof( token );
		}
		FS_FreeFile( buf );
		return qtrue;
	}

	FS_FreeFile( buf );
	return qfalse;
}

static void FA_LookupVehicle( const char *vehName ) {
	char **files;
	int numFiles, i;

	if ( !Q_stricmp( vehName, fa.vehName ) )
		return;

	Q_strncpyz( fa.vehName, vehName, sizeof( fa.vehName ) );
	fa.isFighter = qfalse;
	fa.landingHeight = 0.0f;

	// the game strings every .veh together in this order and takes the first match
	files = FS_ListFiles( "ext_data/vehicles", ".veh", &numFiles );
	for ( i = 0; i < numFiles; i++ ) {
		if ( FA_ParseVehFile( va( "ext_data/vehicles/%s", files[i] ), vehName ) )
			break;
	}
	FS_FreeFileList( files );
}

/*
===============================================================================

SNAPSHOT

===============================================================================
*/

static const entityState_t *FA_SnapEntity( int number ) {
	int i;

	for ( i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
		if ( es->number == number )
			return es;
	}
	return NULL;
}

// the fighter the local player is flying, NULL on foot, as a passenger or in anything else
static const entityState_t *FA_PilotedFighter( void ) {
	const playerState_t *ps = &cl.snap.ps;
	const entityState_t *veh;
	const char *model;

	if ( cls.state != CA_ACTIVE || !cl.snap.valid || clc.demoplaying )
		return NULL;
	if ( !ps->m_iVehicleNum || ps->generic1 )	// generic1 is the passenger seat
		return NULL;

	veh = FA_SnapEntity( ps->m_iVehicleNum );
	if ( !veh || veh->eType != ET_NPC || veh->modelindex <= 0 || veh->modelindex >= MAX_MODELS )
		return NULL;

	// vehicles send their .veh name as their model
	model = cl.gameState.stringData + cl.gameState.stringOffsets[CS_MODELS + veh->modelindex];
	if ( model[0] != '$' )
		return NULL;

	FA_LookupVehicle( model + 1 );
	return fa.isFighter ? veh : NULL;
}

static void FA_EntityOrigin( const entityState_t *es, vec3_t origin ) {
	const trajectory_t *tr = &es->pos;
	int time = cl.snap.serverTime;

	switch ( tr->trType ) {
	case TR_LINEAR_STOP:
		if ( time > tr->trTime + tr->trDuration )
			time = tr->trTime + tr->trDuration;
		// fall through
	case TR_LINEAR:
		if ( time < tr->trTime )
			time = tr->trTime;
		VectorMA( tr->trBase, ( time - tr->trTime ) * 0.001f, tr->trDelta, origin );
		break;
	default:
		VectorCopy( tr->trBase, origin );
		break;
	}
}

// would the game still treat a held jump as taking off, by the time this command runs?
static qboolean FA_InTakeoffZone( const entityState_t *veh, const usercmd_t *cmd ) {
	const playerState_t *vps = &cl.snap.vps;
	vec3_t mins, maxs, end, origin;
	float height;
	int lead, i;
	trace_t tr, entTr;

	if ( fa.landingHeight <= 0.0f || vps->speed > FA_MIN_LANDING_SPEED )
		return qfalse;

	// pushing forward, the game lets go of the takeoff 70% of the way up
	height = fa.landingHeight;
	if ( cmd->forwardmove > 0 )
		height *= FA_MIN_TAKEOFF_FRACTION;

	// the snapshot is a ping old, and this command runs another ping later
	lead = cl.snap.ping + FA_LEAD_EXTRA_MS;
	if ( lead > FA_LEAD_MAX_MS )
		lead = FA_LEAD_MAX_MS;
	if ( vps->velocity[2] > 0.0f )
		height -= vps->velocity[2] * lead * 0.001f;
	height -= FA_MARGIN;
	if ( height <= 0.0f )
		return qfalse;

	// the vehicle's bbox, packed the way SV_LinkEntity sends it
	VectorClear( mins );
	VectorClear( maxs );
	if ( veh->solid && veh->solid != SOLID_BMODEL ) {
		const int x = veh->solid & 255;
		const int zd = ( veh->solid >> 8 ) & 255;
		const int zu = ( ( veh->solid >> 16 ) & 255 ) - 32;
		VectorSet( mins, -x, -x, -zd );
		VectorSet( maxs, x, x, zu );
	}

	VectorCopy( vps->origin, end );
	end[2] -= height;
	CM_BoxTrace( &tr, vps->origin, end, mins, maxs, 0, FA_LAND_MASK, qfalse );

	// lifts, platforms and other brush entities count as ground too
	for ( i = 0; i < cl.snap.numEntities && !tr.allsolid; i++ ) {
		const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
		if ( es->solid != SOLID_BMODEL )
			continue;
		FA_EntityOrigin( es, origin );
		CM_TransformedBoxTrace( &entTr, vps->origin, end, mins, maxs, CM_InlineModel( es->modelindex ), FA_LAND_MASK, origin, es->apos.trBase, qfalse );
		if ( entTr.allsolid || entTr.fraction < tr.fraction )
			tr = entTr;
	}

	return (qboolean)( tr.fraction < 1.0f && tr.plane.normal[2] >= FA_MIN_LANDING_SLOPE );
}

/*
===============================================================================

COMMAND FILTER

===============================================================================
*/

void CL_FighterAssist_FilterCmd( usercmd_t *cmd, qboolean boost ) {
	const entityState_t *veh = FA_PilotedFighter();

	if ( !veh ) {
		fa.jump = FA_IDLE;
		return;
	}

	if ( cmd->upmove <= 0 ) {
		fa.jump = FA_IDLE;
	} else if ( cl_fighterTakeoffGuard->integer ) {
		if ( fa.jump == FA_IDLE )
			fa.jump = FA_InTakeoffZone( veh, cmd ) ? FA_LIFT : FA_PASS;
		else if ( fa.jump == FA_LIFT && !FA_InTakeoffZone( veh, cmd ) )
			fa.jump = FA_HOLD;

		if ( fa.jump == FA_HOLD )
			cmd->upmove = 0;
	}

	if ( boost )
		cmd->upmove = 127;
}
