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

// cl_chatrange.cpp -- chat ranges for the console, as RPMod does them for its chat box
//
// RPMod's server ends its "chat", "tchat", "lchat" and "ltchat" commands with how far the
// sender is from this player, in units, where the stock game puts the sender's client
// number. RPMod's cgame fades or drops the chat in its chat box by that distance
// (rpc_chatRange_*), but prints every line to the console as it is. These cvars do the
// same for the console line:
//
//   rpc_chatRangeConsole_mode   0 no ranges, 1 fade by distance, 2 fade and leave out
//                               beyond the max range, 3 only below the min range, no fade
//   rpc_chatRangeConsole_min    up to here chat is solid
//   rpc_chatRangeConsole_max    here and beyond it is at the min alpha (or left out, mode 2)
//   rpc_chatRangeConsole_alpha  the min alpha, in percent
//
// The cgame still gets the command, so its chat box and sound are as before. Only on RPMod
// (fs_game rpmod), as elsewhere the number is a client number.
//
// This lives in the engine rather than cgame so it works whatever cgame the server's
// mod ships (RPMod loads its own).

#include "client.h"

#define CR_MATCH_FRAMES		2		// the cgame prints the chat as it runs the command, this frame or the next

typedef enum {
	CR_MODE_OFF,
	CR_MODE_FADE,
	CR_MODE_FADE_DROP,
	CR_MODE_NEAR_ONLY,
} crMode_t;

static struct {
	char		text[MAX_STRING_CHARS];		// the chat's text, plain, to know its line by
	int			frame;
	qboolean	hide;
	int			fade;						// 1-255 when not hidden
} cr;

static cvar_t *rpc_chatRangeConsole_mode;
static cvar_t *rpc_chatRangeConsole_min;
static cvar_t *rpc_chatRangeConsole_max;
static cvar_t *rpc_chatRangeConsole_alpha;

void CL_ChatRange_Init( void ) {
	memset( &cr, 0, sizeof( cr ) );
	rpc_chatRangeConsole_mode = Cvar_Get( "rpc_chatRangeConsole_mode", "0", CVAR_ARCHIVE_ND,
		"Chat ranges in the console: 0 off, 1 fade by distance, 2 fade and leave out beyond the max range, 3 only below the min range" );
	rpc_chatRangeConsole_min = Cvar_Get( "rpc_chatRangeConsole_min", "128", CVAR_ARCHIVE_ND,
		"Console chat from closer than this many units is solid" );
	rpc_chatRangeConsole_max = Cvar_Get( "rpc_chatRangeConsole_max", "1024", CVAR_ARCHIVE_ND,
		"Console chat from this many units away and beyond is at the min alpha, or left out in mode 2" );
	rpc_chatRangeConsole_alpha = Cvar_Get( "rpc_chatRangeConsole_alpha", "25", CVAR_ARCHIVE_ND,
		"How see-through the farthest console chat gets, in percent" );
}

// cgame is going away
void CL_ChatRange_Shutdown( void ) {
	cr.text[0] = '\0';
}

// the text without colors, the chat escape character or the spaces around it
static void CR_Plain( const char *in, char *out, int size ) {
	int len = 0;

	for ( ; *in && len < size - 1; in++ ) {
		if ( Q_IsColorString( in ) ) {
			in++;
			continue;
		}
		if ( *in == '\x19' || *in == '\n' || *in == '\r' )
			continue;
		out[len++] = *in;
	}
	while ( len && out[len - 1] == ' ' )
		len--;
	out[len] = '\0';
}

// how chat from this far away shows: qfalse to leave it out, else its fade (0 solid)
static qboolean CR_Range( float distance, int *fade ) {
	const int mode = rpc_chatRangeConsole_mode->integer;
	const float min = rpc_chatRangeConsole_min->value;
	const float max = rpc_chatRangeConsole_max->value;
	const float minAlpha = Com_Clamp( 0.0f, 100.0f, rpc_chatRangeConsole_alpha->value ) / 100.0f;
	float frac;

	*fade = 0;
	if ( mode == CR_MODE_NEAR_ONLY )
		return (qboolean)( distance <= min );
	if ( mode == CR_MODE_FADE_DROP && distance > max )
		return qfalse;
	if ( mode != CR_MODE_FADE && mode != CR_MODE_FADE_DROP )
		return qtrue;

	if ( distance <= min )
		return qtrue;
	frac = max > min ? ( distance - min ) / ( max - min ) : 1.0f;
	if ( frac > 1.0f )
		frac = 1.0f;
	*fade = (int)( frac * ( 1.0f - minAlpha ) * 255.0f + 0.5f );
	return qtrue;
}

// a chat command is about to run: what it prints is faded or left out by its distance
void CL_ChatRange_ServerCommand( void ) {
	const char *cmd = Cmd_Argv( 0 ), *num, *text;
	int fade;

	cr.text[0] = '\0';
	if ( rpc_chatRangeConsole_mode->integer == CR_MODE_OFF || Q_stricmp( FS_GetCurrentGameDir(), "rpmod" ) )
		return;

	// "chat" "name: message" <distance>; "lchat" "name" "location" "color" "message" <distance>
	if ( !strcmp( cmd, "chat" ) || !strcmp( cmd, "tchat" ) ) {
		if ( Cmd_Argc() < 3 )
			return;
		text = Cmd_Argv( 1 );
		num = Cmd_Argv( 2 );
	} else if ( !strcmp( cmd, "lchat" ) || !strcmp( cmd, "ltchat" ) ) {
		if ( Cmd_Argc() < 6 )
			return;
		text = Cmd_Argv( 4 );
		num = Cmd_Argv( 5 );
	} else {
		return;
	}
	if ( !isdigit( (unsigned char)num[0] ) )
		return;

	cr.hide = (qboolean)!CR_Range( (float)atoi( num ), &fade );
	cr.fade = fade;
	if ( !cr.hide && !cr.fade )
		return;
	CR_Plain( text, cr.text, sizeof( cr.text ) );
	cr.frame = cls.framecount;
}

// what the cgame prints goes to the console, the line of a chat from far away faded or left out
void CL_ChatRange_CgamePrint( const char *text ) {
	char plain[MAX_STRING_CHARS];

	if ( cr.text[0] && cls.framecount - cr.frame > CR_MATCH_FRAMES )
		cr.text[0] = '\0';
	if ( cr.text[0] ) {
		CR_Plain( text, plain, sizeof( plain ) );
		if ( strstr( plain, cr.text ) ) {
			cr.text[0] = '\0';
			if ( cr.hide )
				return;
			Con_SetPrintFade( cr.fade );
			Com_Printf( "%s", text );
			Con_SetPrintFade( 0 );
			return;
		}
	}
	Com_Printf( "%s", text );
}
