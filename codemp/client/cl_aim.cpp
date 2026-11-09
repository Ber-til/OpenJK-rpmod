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

// cl_aim.cpp -- what the crosshair is on: a ray from the main view against people's boxes, a wall
// in between hiding them. The NPC names (cl_npcnames.cpp), giving items (cl_inventory.cpp) and
// Inspect use it.

#include "client.h"
#include "qcommon/cm_public.h"

static struct {
	qboolean	have;
	vec3_t		org, fwd;
} aim;

// the main view, from the scene the cgame draws
void CL_Aim_View( const refdef_t *fd ) {
	if ( ( fd->rdflags & RDF_NOWORLDMODEL ) || fd->width < cls.glconfig.vidWidth / 2 )
		return;
	VectorCopy( fd->vieworg, aim.org );
	VectorCopy( fd->viewaxis[0], aim.fwd );
	aim.have = qtrue;
}

void CL_Aim_Disconnect( void ) {
	aim.have = qfalse;
}

// how far along the ray it enters the box, -1 if it misses or it's further than range
float CL_Aim_RayBox( const vec3_t org, const vec3_t dir, const vec3_t mins, const vec3_t maxs, float range ) {
	float tNear = 0.0f, tFar = range;

	for ( int k = 0; k < 3; k++ ) {
		if ( fabs( dir[k] ) < 0.00001f ) {
			if ( org[k] < mins[k] || org[k] > maxs[k] )
				return -1.0f;
			continue;
		}
		float t1 = ( mins[k] - org[k] ) / dir[k];
		float t2 = ( maxs[k] - org[k] ) / dir[k];
		if ( t1 > t2 ) {
			const float t = t1;
			t1 = t2;
			t2 = t;
		}
		tNear = Q_max( tNear, t1 );
		tFar = Q_min( tFar, t2 );
		if ( tNear > tFar )
			return -1.0f;
	}
	return tNear;
}

static const entityState_t *AIM_Ent( int i ) {
	return &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
}

// the entity the crosshair is on, of those want takes (giving where it stands), -1 none
int CL_Aim_Pick( qboolean ( *want )( const entityState_t *es, vec3_t org ), float range ) {
	int best = -1;
	float bestT = range;

	if ( !aim.have || !cl.snap.valid )
		return -1;
	for ( int i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = AIM_Ent( i );
		vec3_t org, mins, maxs;
		float t;

		if ( ( es->eFlags & ( EF_DEAD | EF_NODRAW ) ) || es->number < 0 || es->number >= MAX_GENTITIES || !want( es, org ) )
			continue;
		// a person's box, a bit generous
		VectorSet( mins, org[0] - 18.0f, org[1] - 18.0f, org[2] - 24.0f );
		VectorSet( maxs, org[0] + 18.0f, org[1] + 18.0f, org[2] + 44.0f );
		t = CL_Aim_RayBox( aim.org, aim.fwd, mins, maxs, bestT );
		if ( t < 0.0f || t >= bestT )
			continue;
		best = es->number;
		bestT = t;
	}
	if ( best >= 0 ) {
		trace_t tr;
		vec3_t end;

		// a wall in between hides it
		VectorMA( aim.org, bestT, aim.fwd, end );
		CM_BoxTrace( &tr, aim.org, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse );
		if ( tr.fraction < 1.0f )
			best = -1;
	}
	return best;
}

static qboolean AIM_Player( const entityState_t *es, vec3_t org ) {
	if ( es->eType != ET_PLAYER || es->number >= MAX_CLIENTS || es->number == clc.clientNum )
		return qfalse;
	VectorCopy( es->pos.trBase, org );
	return qtrue;
}

// the other player the crosshair is on, within range: their client number, -1 none
int CL_AimedPlayer( float range ) {
	return CL_Aim_Pick( AIM_Player, range );
}

// a player's name as the server has it, colours kept: "" for an empty slot
const char *CL_PlayerSlotName( int client ) {
	const char *info;
	int offset;

	if ( client < 0 || client >= MAX_CLIENTS )
		return "";
	offset = cl.gameState.stringOffsets[CS_PLAYERS + client];
	if ( !offset )
		return "";
	info = cl.gameState.stringData + offset;
	return Info_ValueForKey( info, "n" );
}
