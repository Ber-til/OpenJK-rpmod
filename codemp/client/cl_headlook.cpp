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

// cl_headlook.cpp -- +headlook: held, the mouse turns your head and your view, your body staying as it
// faces; let go, they come back to it. Others with the client see your head turn: it's sent through
// C1B3R as cmd:head/<client>/<yaw>/<pitch> a few times a second while it's turned, and added to your
// body's neck and head (cervical, cranium) on their screens, as the game sets them. Each body is
// known by where it was drawn the frame before (CL_BodyOwner).
//
// cl_lookLean (cl_cgameapi.cpp) is the other half: players leaning with their look.

#include "client.h"
#include "cl_presence.h"
#include "cl_rpcinematic.h"

#define HL_MAX_YAW			75.0f
#define HL_MAX_PITCH		50.0f
#define HL_BACK_RATE		8.0f		// let go: back to the body this fast (per second, eased)
#define HL_FOLLOW_RATE		12.0f		// others' heads move to what they sent this fast
#define HL_SEND_MS			200			// sent at most this often while it changes
#define HL_SEND_STEP		1.5f		// and only when it's moved this much
#define HL_STALE_MS			3000		// someone's head heard nothing of this long goes back
#define HL_MAX_BODIES		64
#define HL_NEAR				64.0f

static struct {
	qboolean	held;
	float		yaw, pitch;			// yours, from the body's way
	float		sentYaw, sentPitch;
	int			sentTime;
	qboolean	sentAny;			// something not straight ahead sent, so the end of it goes too
	int			lastTime;

	struct {
		float	yaw, pitch;			// as drawn
		float	toYaw, toPitch;		// as they sent it
		int		heard;
	} others[MAX_CLIENTS];

	// the bodies drawn, whose they are: this frame's, and last frame's (the bones are set before the
	// body is drawn)
	struct {
		void	*g2;
		int		client;
	} bodies[2][HL_MAX_BODIES];
	int			numBodies[2];
	int			bodyFrame;
} hl;

static cvar_t *cl_headLookView;

static void HL_Down_f( void ) {
	hl.held = qtrue;
}

static void HL_Up_f( void ) {
	hl.held = qfalse;
}

qboolean CL_HeadLook_Held( void ) {
	return (qboolean)( hl.held && cls.state == CA_ACTIVE );
}

// the mouse's turn while it's held: the head's, not the body's
void CL_HeadLook_Turn( float yaw, float pitch ) {
	hl.yaw = Com_Clamp( -HL_MAX_YAW, HL_MAX_YAW, hl.yaw + yaw );
	hl.pitch = Com_Clamp( -HL_MAX_PITCH, HL_MAX_PITCH, hl.pitch + pitch );
}

static float HL_Ease( float v, float to, float rate, float dt ) {
	const float k = 1.0f - expf( -rate * dt );

	v += ( to - v ) * k;
	return fabsf( v - to ) < 0.05f ? to : v;
}

static void HL_Send( void ) {
	const qboolean straight = (qboolean)( !hl.yaw && !hl.pitch );

	if ( straight && !hl.sentAny )
		return;
	if ( cls.realtime - hl.sentTime < HL_SEND_MS )
		return;
	if ( !straight && fabsf( hl.yaw - hl.sentYaw ) < HL_SEND_STEP && fabsf( hl.pitch - hl.sentPitch ) < HL_SEND_STEP )
		return;
	hl.sentTime = cls.realtime;
	hl.sentYaw = hl.yaw;
	hl.sentPitch = hl.pitch;
	hl.sentAny = (qboolean)!straight;
	CL_Presence_Hook( -1, va( "cmd:head/%i/%.0f/%.0f", clc.clientNum, hl.yaw, hl.pitch ) );
}

void CL_HeadLook_Frame( void ) {
	const float dt = hl.lastTime ? Com_Clamp( 0.0f, 0.1f, ( cls.realtime - hl.lastTime ) / 1000.0f ) : 0.0f;

	hl.lastTime = cls.realtime;
	if ( cls.state != CA_ACTIVE )
		return;
	if ( !CL_HeadLook_Held() ) {
		hl.yaw = HL_Ease( hl.yaw, 0.0f, HL_BACK_RATE, dt );
		hl.pitch = HL_Ease( hl.pitch, 0.0f, HL_BACK_RATE, dt );
	}
	HL_Send();
	for ( int c = 0; c < MAX_CLIENTS; c++ ) {
		if ( hl.others[c].heard && cls.realtime - hl.others[c].heard > HL_STALE_MS )
			hl.others[c].toYaw = hl.others[c].toPitch = 0.0f;
		hl.others[c].yaw = HL_Ease( hl.others[c].yaw, hl.others[c].toYaw, HL_FOLLOW_RATE, dt );
		hl.others[c].pitch = HL_Ease( hl.others[c].pitch, hl.others[c].toPitch, HL_FOLLOW_RATE, dt );
	}
}

// cmd:head/<client>/<yaw>/<pitch>
void CL_HeadLook_Hook( char **argv, int argc ) {
	const int c = argc >= 1 ? atoi( argv[0] ) : -1;

	if ( argc < 3 || c < 0 || c >= MAX_CLIENTS || c == clc.clientNum )
		return;
	hl.others[c].toYaw = Com_Clamp( -HL_MAX_YAW, HL_MAX_YAW, (float)atof( argv[1] ) );
	hl.others[c].toPitch = Com_Clamp( -HL_MAX_PITCH, HL_MAX_PITCH, (float)atof( argv[2] ) );
	hl.others[c].heard = cls.realtime;
}

// the view turned by your head, while it is
qboolean CL_HeadLook_View( const refdef_t *fd, refdef_t *out ) {
	vec3_t delta, d[3], axis[3];

	if ( ( !hl.yaw && !hl.pitch ) || ( fd->rdflags & RDF_NOWORLDMODEL ) || cls.state != CA_ACTIVE )
		return qfalse;
	// in third person the camera stays, by default: you watch your character look round
	if ( !cl_headLookView->integer || ( cl_headLookView->integer == 1 && Cvar_VariableIntegerValue( "cg_thirdPerson" ) ) )
		return qfalse;
	VectorSet( delta, hl.pitch, hl.yaw, 0.0f );
	AnglesToAxis( delta, d );
	*out = *fd;
	VectorCopy( fd->viewaxis[0], axis[0] );
	VectorCopy( fd->viewaxis[1], axis[1] );
	VectorCopy( fd->viewaxis[2], axis[2] );
	for ( int i = 0; i < 3; i++ ) {
		for ( int k = 0; k < 3; k++ )
			out->viewaxis[i][k] = d[i][0] * axis[0][k] + d[i][1] * axis[1][k] + d[i][2] * axis[2][k];
	}
	return qtrue;
}

// a body on its way to the renderer: whose it is, for its bones next frame
void CL_HeadLook_Entity( const refEntity_t *ent, void *bodyGhoul2 ) {
	int slot;

	if ( !ent || ent->reType != RT_MODEL || !bodyGhoul2 )
		return;
	if ( hl.bodyFrame != cls.framecount ) {
		hl.bodyFrame = cls.framecount;
		hl.numBodies[1] = hl.numBodies[0];
		memcpy( hl.bodies[1], hl.bodies[0], sizeof( hl.bodies[0] ) );
		hl.numBodies[0] = 0;
	}
	if ( hl.numBodies[0] >= HL_MAX_BODIES )
		return;
	slot = hl.numBodies[0]++;
	hl.bodies[0][slot].g2 = bodyGhoul2;
	hl.bodies[0][slot].client = CL_BodyOwner( ent->origin, HL_NEAR );
}

static int HL_Owner( void *g2 ) {
	for ( int f = 0; f < 2; f++ ) {
		for ( int i = 0; i < hl.numBodies[f]; i++ ) {
			if ( hl.bodies[f][i].g2 == g2 )
				return hl.bodies[f][i].client;
		}
	}
	return -1;
}

// the game setting a body's neck or head: the head's turn added, shared between them
qboolean CL_HeadLook_Bone( void *g2, const char *bone, const vec3_t in, vec3_t out ) {
	float yaw, pitch, share;
	int c;

	if ( !bone || ( Q_stricmp( bone, "cranium" ) && Q_stricmp( bone, "cervical" ) ) )
		return qfalse;
	c = HL_Owner( g2 );
	if ( c < 0 )
		return qfalse;
	if ( c == clc.clientNum ) {
		yaw = hl.yaw;
		pitch = hl.pitch;
	} else {
		yaw = hl.others[c].yaw;
		pitch = hl.others[c].pitch;
	}
	if ( !yaw && !pitch )
		return qfalse;
	share = !Q_stricmp( bone, "cranium" ) ? 0.6f : 0.4f;
	VectorCopy( in, out );
	out[PITCH] += pitch * share;		// as the view's: down positive
	out[YAW] += yaw * share;
	return qtrue;
}

void CL_HeadLook_Disconnect( void ) {
	const qboolean held = hl.held;

	memset( &hl, 0, sizeof( hl ) );
	hl.held = held;
}

void CL_HeadLook_Init( void ) {
	Cmd_AddCommand( "+headlook", HL_Down_f, "Held: the mouse turns your head and view, not your body (others with the client see your head turn)" );
	Cmd_AddCommand( "-headlook", HL_Up_f );
	cl_headLookView = Cvar_Get( "cl_headLookView", "1", CVAR_ARCHIVE_ND, "+headlook turns your view with your head: 0 never, 1 in first person, 2 in third person too" );
}
