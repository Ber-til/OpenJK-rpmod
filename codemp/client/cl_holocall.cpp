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

// cl_holocall.cpp -- HoloCall: call another player, on any server, through C1B3R (c1b3r/holo.py); each
// shows to the other as a hologram, mirroring their moves, and what each says is read around it
//
// Calling: the datapad Chat's HoloCall in a conversation, or /holocall <name>. It rings the other's
// games (cmd:holo/ring); answered, each side sends its look and motion (/v2/holo/state: its model,
// its legs and torso animations by name, its turn), when they change and once a second, and C1B3R
// tells every game on the other's server (cmd:holo/state), so everyone there with the client draws
// the same hologram. Where it stands is the side it shows to's to say: in front of them to start,
// where they aim with /holocall here (/v2/holo/spot, told to every game on their server). What you
// say in chat in a call goes too (/v2/holo/say), read around the hologram as chat from it, by
// RPMod's chat range.

#include "client.h"
#include "cl_datapad.h"
#include "cl_http.h"
#include "cl_presence.h"
#include "qcommon/cm_public.h"

#include <string>

#define HC_MAX_HOLOS		4
#define HC_SHADER			"powerups/ysaliblueshell"	// the cinematic manager's hologram look
#define HC_KEEPALIVE_MS		1000
#define HC_STATE_GAP_MS		200
#define HC_SPOT_AGAIN_MS	15000	// the spot told again, for those who came since
#define HC_GONE_MS			10000	// a hologram not heard from this long: gone
#define HC_RING_MS			30000
#define HC_AHEAD			80.0f	// a hologram this far in front of whom it shows to

typedef struct {
	qboolean		used;
	int				call;
	char			side;					// whose: 'a' the caller, 'b' who was called
	char			model[MAX_QPATH];
	char			legs[48], torso[48];
	float			yaw, firstYaw;
	qboolean		haveYaw;
	vec3_t			spot;
	float			spotYaw;
	qboolean		haveSpot;
	int				heard;
	clLiveBody_t	body;
} hcHolo_t;

static struct {
	int			call;			// ours, 0 none
	char		side;			// ours in it
	qboolean	active, ringing, calling;
	int			since;			// the ring's or the call's start
	char		otherName[64], otherAccount[64];

	// ours, sent
	char		lastState[256];
	int			lastSent, spotSent;
	vec3_t		mySpot;			// where the other's hologram stands here
	float		mySpotYaw;
	qboolean	spotDue;

	char		message[160];
	int			messageTime;
	qhandle_t	font;
} hc;

static hcHolo_t hcHolos[HC_MAX_HOLOS];
static httpRequest_t hcActReq, hcStateReq, hcSpotReq, hcSayReq;
static int hcActing;			// what hcActReq is: 1 call, 2 answer, 3 decline, 4 end

static void HC_Message( const char *text ) {
	Q_strncpyz( hc.message, text, sizeof( hc.message ) );
	hc.messageTime = cls.realtime;
	Com_Printf( "HoloCall: %s\n", text );
}

static void HC_Notice( const char *text, qboolean ring ) {
	char clean[MAX_STRING_CHARS];

	Q_strncpyz( clean, text, sizeof( clean ) );
	for ( char *c = clean; *c; c++ ) {
		if ( *c == '"' )
			*c = '\'';
	}
	if ( cls.state == CA_ACTIVE )
		CL_QueueCgameCommand( va( "info 6000 2 0 8 \":: %s" S_COLOR_WHITE " ::\"", clean ) );
	if ( ring && !Cvar_VariableIntegerValue( "cl_datapadSilent" ) ) {
		const char *tone = Cvar_VariableString( "cl_chatRingtone" );
		const sfxHandle_t sfx = tone[0] ? S_RegisterSound( tone ) : 0;

		if ( sfx )
			S_StartLocalSound( sfx, CHAN_LOCAL_SOUND );
	}
}

static qboolean HC_Post( httpRequest_t *req, const char *verb, const std::string &fields ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session ) {
		HC_Message( "Not connected to C1B3R" );
		return qfalse;
	}
	if ( req->state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += fields + "}";
	return CL_Presence_Post( req, va( "/v2/holo/%s", verb ), body );
}

static void HC_Reset( void ) {
	hc.call = 0;
	hc.active = hc.ringing = hc.calling = qfalse;
	hc.lastState[0] = '\0';
	hc.spotDue = qfalse;
}

/*
===============================================================================

Holograms

===============================================================================
*/

static hcHolo_t *HC_Holo( int call, char side, qboolean make ) {
	hcHolo_t *free = NULL;

	for ( int i = 0; i < HC_MAX_HOLOS; i++ ) {
		if ( hcHolos[i].used && hcHolos[i].call == call && hcHolos[i].side == side )
			return &hcHolos[i];
		if ( !hcHolos[i].used && !free )
			free = &hcHolos[i];
	}
	if ( !make || !free )
		return NULL;
	memset( free, 0, sizeof( *free ) );
	free->body.cfg = -1;
	free->used = qtrue;
	free->call = call;
	free->side = side;
	free->heard = cls.realtime;
	return free;
}

static void HC_FreeHolo( hcHolo_t *h ) {
	CL_Extras_FreeLiveBody( &h->body );
	memset( h, 0, sizeof( *h ) );
}

// the call's holograms gone (between frames only: the renderer draws them till the frame's end)
static void HC_FreeCall( int call ) {
	for ( int i = 0; i < HC_MAX_HOLOS; i++ ) {
		if ( hcHolos[i].used && hcHolos[i].call == call )
			HC_FreeHolo( &hcHolos[i] );
	}
}

// in every main view: each hologram at its spot, turned as its player turns, in the hologram's look
void CL_HoloCall_AddToScene( const refdef_t *fd ) {
	const qhandle_t shader = re->RegisterShader( HC_SHADER );

	if ( fd->rdflags & ( RDF_NOWORLDMODEL | RDF_SKYBOXPORTAL | RDF_AUTOMAP ) || cls.state != CA_ACTIVE )
		return;
	for ( int i = 0; i < HC_MAX_HOLOS; i++ ) {
		hcHolo_t *h = &hcHolos[i];
		refEntity_t ent;
		vec3_t ang;

		if ( !h->used || !h->haveSpot || !h->model[0] )
			continue;
		if ( !CL_Extras_LiveBody( &h->body, h->model, h->legs[0] ? h->legs : "BOTH_STAND1", h->torso[0] ? h->torso : "BOTH_STAND1" ) )
			continue;
		VectorSet( ang, 0, AngleNormalize360( h->spotYaw + ( h->haveYaw ? h->yaw - h->firstYaw : 0.0f ) ), 0 );
		memset( &ent, 0, sizeof( ent ) );
		ent.reType = RT_MODEL;
		ent.ghoul2 = h->body.g2;
		VectorCopy( h->spot, ent.origin );
		VectorCopy( h->spot, ent.oldorigin );
		VectorCopy( h->spot, ent.lightingOrigin );
		VectorCopy( ang, ent.angles );
		AnglesToAxis( ang, ent.axis );
		ent.radius = 64.0f;
		ent.customShader = shader;
		ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = ent.shaderRGBA[3] = 255;
		re->AddRefEntityToScene( &ent );
	}
}

/*
===============================================================================

What C1B3R tells: rings, starts, ends, states, spots and words

===============================================================================
*/

static float HC_MyYaw( void ) {
	return cl.snap.ps.viewangles[YAW];
}

// where the other side's hologram goes to start: a step in front of us, facing us
static void HC_PlaceInFront( void ) {
	vec3_t fwd, ang = { 0, HC_MyYaw(), 0 };

	AngleVectors( ang, fwd, NULL, NULL );
	VectorMA( cl.snap.ps.origin, HC_AHEAD, fwd, hc.mySpot );
	hc.mySpotYaw = AngleNormalize360( HC_MyYaw() + 180.0f );
	hc.spotDue = qtrue;
}

static void HC_Ring( int client, int call, const char *message ) {
	char text[MAX_STRING_CHARS], *bar;

	if ( client != cl.snap.ps.clientNum )
		return;
	Q_strncpyz( text, message, sizeof( text ) );
	bar = strchr( text, '|' );
	if ( bar )
		*bar = '\0';
	HC_Reset();
	hc.call = call;
	hc.side = 'b';
	hc.ringing = qtrue;
	hc.since = cls.realtime;
	Q_strncpyz( hc.otherName, text, sizeof( hc.otherName ) );
	Q_strncpyz( hc.otherAccount, bar ? bar + 1 : "", sizeof( hc.otherAccount ) );
	HC_Notice( va( "%s" S_COLOR_WHITE " is calling you (HoloCall): /holocall answer, or the datapad's Chat", hc.otherName ), qtrue );
}

void CL_HoloCall_Hook( char **argv, int argc, const char *message ) {
	const char *kind;
	int call;
	char side;
	hcHolo_t *h;

	if ( argc < 2 || CL_Presence_Replaying() )
		return;
	kind = argv[0];
	if ( !Q_stricmp( kind, "ring" ) ) {
		if ( argc >= 3 )
			HC_Ring( atoi( argv[1] ), atoi( argv[2] ), message );
		return;
	}
	call = atoi( argv[1] );
	side = argc >= 3 && argv[2][0] ? argv[2][0] : 0;
	if ( !Q_stricmp( kind, "start" ) ) {
		char text[MAX_STRING_CHARS], *bar;

		if ( call != hc.call )
			return;
		Q_strncpyz( text, message, sizeof( text ) );
		if ( ( bar = strchr( text, '|' ) ) != NULL )
			*bar = '\0';
		hc.active = qtrue;
		hc.ringing = hc.calling = qfalse;
		hc.since = cls.realtime;
		hc.side = side ? side : hc.side;
		Q_strncpyz( hc.otherName, text, sizeof( hc.otherName ) );
		hc.lastState[0] = '\0';
		HC_PlaceInFront();
		HC_Notice( va( "In a HoloCall with %s" S_COLOR_WHITE ": what you say in chat reaches them", hc.otherName ), qfalse );
		return;
	}
	if ( !Q_stricmp( kind, "end" ) ) {
		if ( call == hc.call ) {
			HC_Notice( va( "HoloCall with %s" S_COLOR_WHITE " over (%s)", hc.otherName[0] ? hc.otherName : "them", message[0] ? message : "ended" ), qfalse );
			HC_Reset();
		}
		HC_FreeCall( call );
		return;
	}
	if ( !side )
		return;
	if ( !Q_stricmp( kind, "state" ) ) {
		char text[MAX_STRING_CHARS], *f[6];
		int n = 0;

		Q_strncpyz( text, message, sizeof( text ) );
		f[n++] = text;
		for ( char *p = text; *p && n < 6; p++ ) {
			if ( *p == '|' ) {
				*p = '\0';
				f[n++] = p + 1;
			}
		}
		if ( n < 6 || !( h = HC_Holo( call, side, qtrue ) ) )
			return;
		Q_strncpyz( h->model, f[0], sizeof( h->model ) );
		for ( char *p = h->model; *p; p++ ) {
			if ( *p == '!' )
				*p = '|';
		}
		Q_strncpyz( h->legs, f[1], sizeof( h->legs ) );
		Q_strncpyz( h->torso, f[2], sizeof( h->torso ) );
		h->yaw = (float)atof( f[5] );
		if ( !h->haveYaw ) {
			h->firstYaw = h->yaw;
			h->haveYaw = qtrue;
		}
		h->heard = cls.realtime;
		return;
	}
	if ( !Q_stricmp( kind, "spot" ) ) {
		if ( !( h = HC_Holo( call, side, qtrue ) ) )
			return;
		if ( sscanf( message, "%f %f %f %f", &h->spot[0], &h->spot[1], &h->spot[2], &h->spotYaw ) == 4 )
			h->haveSpot = qtrue;
		h->heard = cls.realtime;
		return;
	}
	if ( !Q_stricmp( kind, "say" ) ) {
		char text[MAX_STRING_CHARS], *bar, line[MAX_STRING_CHARS];
		float dist = 100000.0f;

		// our own words, back to us on a call within one server: said already
		if ( call == hc.call && side == hc.side )
			return;
		Q_strncpyz( text, message, sizeof( text ) );
		if ( !( bar = strchr( text, '|' ) ) )
			return;
		*bar = '\0';
		h = HC_Holo( call, side, qfalse );
		if ( h && h->haveSpot )
			dist = Distance( cl.snap.ps.origin, h->spot );
		else if ( call != hc.call )
			return;		// someone else's call, its hologram unknown here: not near
		else
			dist = 0.0f;
		Com_sprintf( line, sizeof( line ), S_COLOR_CYAN "[Holo] " S_COLOR_WHITE "%s" S_COLOR_WHITE "\x19: " S_COLOR_GREEN "%s", text, bar + 1 );
		for ( char *q = line; *q; q++ ) {
			if ( *q == '"' )
				*q = '\'';
		}
		if ( cls.state == CA_ACTIVE )
			CL_QueueCgameCommand( va( "chat \"%s\" %i", line, (int)dist ) );
	}
}

/*
===============================================================================

Calling, answering, ending

===============================================================================
*/

void CL_HoloCall_Start( const char *who ) {
	std::string fields = ",\"to\":";

	if ( hc.call ) {
		HC_Message( hc.active ? "You're in a HoloCall already: /holocall end" : "A HoloCall is ringing already" );
		return;
	}
	CL_Presence_Json( fields, who );
	if ( HC_Post( &hcActReq, "call", fields ) ) {
		hcActing = 1;
		Q_strncpyz( hc.otherAccount, who, sizeof( hc.otherAccount ) );
		HC_Message( va( "Calling %s...", who ) );
	}
}

static void HC_Answer( qboolean accept ) {
	if ( !hc.ringing ) {
		HC_Message( "No one is calling you" );
		return;
	}
	if ( HC_Post( &hcActReq, "answer", va( ",\"call\":%i,\"accept\":%s", hc.call, accept ? "true" : "false" ) ) ) {
		hcActing = accept ? 2 : 3;
		if ( !accept ) {
			HC_Reset();
			HC_Message( "Declined" );
		}
	}
}

static void HC_End( void ) {
	if ( !hc.call ) {
		HC_Message( "You're not in a HoloCall" );
		return;
	}
	HC_Post( &hcActReq, "end", va( ",\"call\":%i", hc.call ) );
	hcActing = 4;
	HC_FreeCall( hc.call );
	HC_Reset();
	HC_Message( "Hung up" );
}

// where we aim: the other's hologram stands there now (on the floor under it, or what we aim at)
static void HC_Here( void ) {
	vec3_t start, end, fwd, ang;
	trace_t tr;

	if ( !hc.active ) {
		HC_Message( "You're not in a HoloCall" );
		return;
	}
	VectorCopy( cl.snap.ps.origin, start );
	start[2] += cl.snap.ps.viewheight;
	VectorCopy( cl.snap.ps.viewangles, ang );
	AngleVectors( ang, fwd, NULL, NULL );
	VectorMA( start, 512.0f, fwd, end );
	CM_BoxTrace( &tr, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
	// on the floor where it hit (a pedestal's top): stood there as a player stands, 24 up
	VectorCopy( tr.endpos, end );
	VectorCopy( tr.endpos, start );
	end[2] -= 256.0f;
	start[2] += 4.0f;
	CM_BoxTrace( &tr, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
	VectorCopy( tr.endpos, hc.mySpot );
	hc.mySpot[2] += 24.0f;
	hc.mySpotYaw = AngleNormalize360( HC_MyYaw() + 180.0f );
	hc.spotDue = qtrue;
	HC_Message( va( "%s" S_COLOR_WHITE "'s hologram moved where you aim", hc.otherName ) );
}

// what we say in chat, in a call: to them too
void CL_HoloCall_Outgoing( const char *cmd ) {
	std::string fields = ",\"text\":";
	char text[MAX_STRING_CHARS];
	int len;

	if ( !hc.active || Q_stricmpn( cmd, "say ", 4 ) )
		return;
	Q_strncpyz( text, cmd + 4, sizeof( text ) );
	len = strlen( text );
	if ( len >= 2 && text[0] == '"' && text[len - 1] == '"' ) {
		text[len - 1] = '\0';
		memmove( text, text + 1, len - 1 );
	}
	if ( !text[0] )
		return;
	CL_Presence_Json( fields, text );
	if ( !HC_Post( &hcSayReq, "say", va( ",\"call\":%i%s", hc.call, fields.c_str() ) ) )
		HC_Message( "Not said to the call: a moment" );
}

/*
===============================================================================

The frame

===============================================================================
*/

// our look and motion now: model|legs|torso|legs flip|torso flip|yaw
static void HC_State( char *out, int size ) {
	const int me = cl.snap.ps.clientNum;
	const int ofs = cl.gameState.stringOffsets[CS_PLAYERS + me];
	const char *legs = CL_EmotePreview_AnimName( cl.snap.ps.legsAnim ), *torso = CL_EmotePreview_AnimName( cl.snap.ps.torsoAnim );
	char model[MAX_QPATH];

	Q_strncpyz( model, ofs ? Info_ValueForKey( cl.gameState.stringData + ofs, "model" ) : "kyle", sizeof( model ) );
	// a model's skins by part ("model/head|torso|legs"): its | sent as !, the fields being |'s
	for ( char *p = model; *p; p++ ) {
		if ( *p == '|' )
			*p = '!';
	}
	Com_sprintf( out, size, "%s|%s|%s|%i|%i|%i", model, legs ? legs : "BOTH_STAND1", torso ? torso : "BOTH_STAND1",
		cl.snap.ps.legsFlip ? 1 : 0, cl.snap.ps.torsoFlip ? 1 : 0, ( (int)( HC_MyYaw() / 10.0f ) ) * 10 );
}

void CL_HoloCall_Frame( void ) {
	char why[160];

	if ( HTTP_Poll( &hcActReq ) ) {
		const char *answer = HTTP_Answer( &hcActReq );

		if ( hcActReq.status == 200 && !strncmp( answer, "ok", 2 ) ) {
			if ( hcActing == 1 ) {
				const char *tab = strchr( answer, '\t' ), *tab2 = tab ? strchr( tab + 1, '\t' ) : NULL;

				hc.call = tab ? atoi( tab + 1 ) : 0;
				hc.side = 'a';
				hc.calling = (qboolean)( hc.call != 0 );
				hc.since = cls.realtime;
				if ( tab2 ) {
					Q_strncpyz( hc.otherName, tab2 + 1, sizeof( hc.otherName ) );
					for ( char *nl = strchr( hc.otherName, '\n' ); nl; nl = NULL )
						*nl = '\0';
				}
				HC_Message( va( "Ringing %s" S_COLOR_WHITE "...", hc.otherName ) );
			}
		} else {
			Q_strncpyz( why, hcActReq.status ? answer : "C1B3R didn't answer", sizeof( why ) );
			for ( char *nl = strchr( why, '\n' ); nl; nl = NULL )
				*nl = '\0';
			if ( hcActing == 1 || hcActing == 2 ) {
				HC_Reset();
				HC_Message( why );
			}
		}
		hcActing = 0;
	}
	HTTP_Poll( &hcStateReq );
	HTTP_Poll( &hcSpotReq );
	if ( HTTP_Poll( &hcSayReq ) && hcSayReq.status != 200 ) {
		Q_strncpyz( why, hcSayReq.status ? HTTP_Answer( &hcSayReq ) : "C1B3R didn't answer", sizeof( why ) );
		HC_Message( why );
	}

	// a ring unanswered
	if ( ( hc.ringing || hc.calling ) && cls.realtime - hc.since > HC_RING_MS + 5000 ) {
		HC_Message( hc.ringing ? "Missed a HoloCall" : "No answer" );
		HC_Reset();
	}
	// ours sent: on a change, else now and then
	if ( hc.active && cls.state == CA_ACTIVE && cl.snap.valid ) {
		char state[256];

		HC_State( state, sizeof( state ) );
		if ( ( strcmp( state, hc.lastState ) && cls.realtime - hc.lastSent >= HC_STATE_GAP_MS ) || cls.realtime - hc.lastSent >= HC_KEEPALIVE_MS ) {
			std::string fields = va( ",\"call\":%i,\"data\":", hc.call );

			CL_Presence_Json( fields, state );
			if ( HC_Post( &hcStateReq, "state", fields ) ) {
				Q_strncpyz( hc.lastState, state, sizeof( hc.lastState ) );
				hc.lastSent = cls.realtime;
			}
		}
		if ( ( hc.spotDue || cls.realtime - hc.spotSent > HC_SPOT_AGAIN_MS )
			&& HC_Post( &hcSpotReq, "spot", va( ",\"call\":%i,\"spot\":\"%.1f %.1f %.1f %.1f\"", hc.call, hc.mySpot[0], hc.mySpot[1], hc.mySpot[2], hc.mySpotYaw ) ) ) {
			hc.spotDue = qfalse;
			hc.spotSent = cls.realtime;
		}
	}
	// holograms not heard from a while: gone
	for ( int i = 0; i < HC_MAX_HOLOS; i++ ) {
		if ( hcHolos[i].used && cls.realtime - hcHolos[i].heard > HC_GONE_MS )
			HC_FreeHolo( &hcHolos[i] );
	}
}

void CL_HoloCall_Disconnect( void ) {
	if ( hc.call && CL_Presence_Session() )
		HC_Post( &hcActReq, "end", va( ",\"call\":%i", hc.call ) );
	HC_Reset();
	for ( int i = 0; i < HC_MAX_HOLOS; i++ ) {
		if ( hcHolos[i].used )
			HC_FreeHolo( &hcHolos[i] );
	}
}

/*
===============================================================================

On screen: a line under the top right while in a call, and the datapad Chat's bar

===============================================================================
*/

void CL_HoloCall_Draw( void ) {
	static const float color[4] = { 0.45f, 0.8f, 1.0f, 0.9f };
	const char *text;

	if ( cls.state != CA_ACTIVE || !( hc.active || hc.ringing || hc.calling ) )
		return;
	if ( !hc.font )
		hc.font = re->RegisterFont( "arialnb" );
	text = hc.active ? va( "HoloCall: %s", hc.otherName ) : hc.ringing ? va( "HoloCall from %s: /holocall answer", hc.otherName )
		: va( "Calling %s...", hc.otherName[0] ? hc.otherName : hc.otherAccount );
	re->Font_DrawString( (int)( SCREEN_WIDTH - 8 - re->Font_StrLenPixels( text, hc.font ? hc.font : cls.menuFont, 0.5f ) ), 120, text, color,
		hc.font ? hc.font : cls.menuFont, -1, 0.5f );
}

// on the datapad's Chat, over its page: a ring to answer, or the call going on; the height it took
float CL_HoloCall_DrawBar( float x, float y, float w ) {
	const float h = 34.0f;

	if ( !hc.ringing && !hc.active && !hc.calling )
		return 0.0f;
	DP_Fill( x, y, w, h, dpButtonHover );
	DP_Frame( x, y, w, h, dpWhite );
	DP_Text( x + 4, y + 3, w - 8, hc.active ? va( "In a HoloCall with %s", hc.otherName ) : hc.ringing ? va( "%s is calling (HoloCall)", hc.otherName )
		: va( "Calling %s...", hc.otherName[0] ? hc.otherName : hc.otherAccount ), dpWhite, DP_Font(), 0.55f, qfalse );
	if ( hc.ringing ) {
		if ( DP_Button( x + 4, y + 16, w * 0.5f - 6, "Answer", qfalse ) )
			HC_Answer( qtrue );
		if ( DP_Button( x + w * 0.5f + 2, y + 16, w * 0.5f - 6, "Decline", qfalse ) )
			HC_Answer( qfalse );
	} else if ( hc.active ) {
		if ( DP_Button( x + 4, y + 16, w * 0.5f - 6, "Move it here", qfalse ) )
			HC_Here();
		if ( DP_Button( x + w * 0.5f + 2, y + 16, w * 0.5f - 6, "End", qfalse ) )
			HC_End();
	} else if ( DP_Button( x + 4, y + 16, w - 8, "Hang up", qfalse ) ) {
		HC_End();
	}
	return h + 4;
}

/*
===============================================================================

The command

===============================================================================
*/

static void HC_HoloCall_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !Q_stricmp( arg, "answer" ) ) {
		HC_Answer( qtrue );
	} else if ( !Q_stricmp( arg, "decline" ) ) {
		HC_Answer( qfalse );
	} else if ( !Q_stricmp( arg, "end" ) || !Q_stricmp( arg, "hangup" ) ) {
		HC_End();
	} else if ( !Q_stricmp( arg, "here" ) ) {
		HC_Here();
	} else if ( !Q_stricmp( arg, "say" ) && Cmd_Argc() >= 3 ) {
		if ( !hc.active ) {
			HC_Message( "You're not in a HoloCall" );
			return;
		}
		{
			std::string fields = ",\"text\":";

			CL_Presence_Json( fields, Cmd_ArgsFrom( 2 ) );
			HC_Post( &hcSayReq, "say", va( ",\"call\":%i%s", hc.call, fields.c_str() ) );
		}
	} else if ( arg[0] ) {
		CL_HoloCall_Start( Cmd_ArgsFrom( 1 ) );
	} else {
		Com_Printf( "usage: holocall <account>          call that player (their RPMod account, as the datapad's Chat lists them)\n" );
		Com_Printf( "       holocall answer|decline     a call ringing\n" );
		Com_Printf( "       holocall here               their hologram moved where you aim\n" );
		Com_Printf( "       holocall say <text>         said to the call only\n" );
		Com_Printf( "       holocall end                hang up\n" );
		if ( hc.call )
			Com_Printf( "%s\n", hc.active ? va( "In a HoloCall with %s", hc.otherName ) : hc.ringing ? va( "%s is calling", hc.otherName ) : "Calling..." );
	}
}

void CL_HoloCall_Init( void ) {
	memset( &hc, 0, sizeof( hc ) );
	Cmd_AddCommand( "holocall", HC_HoloCall_f, "HoloCall: call another player, on any server, and see and talk to them as a hologram" );
}
