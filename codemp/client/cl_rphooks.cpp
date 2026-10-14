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

// cl_rphooks.cpp -- client hooks carried in RPMod's prints
//
// Until the server can send them itself, an admin's
//   rpnotify -1 1 cmd:<hook>/<args...>: <message>
// reaches clients as a "print" and an "info" (RPMod's banner) whose text starts with
// "cmd:" (rpcentersay's "cp" works too, but the server crashes on its long words). These
// are never shown, in the console, the banner or the middle of the screen: the first one
// goes to the hook named instead.
//
//   cmd:npc/talk/<npc>/<display name>/<name colour>/<emote|none>: <message>
//     (cmd:npc/<npc>/talk/... too, say for talk, and spaces around the parts)
//     shows the message as chat from that name, in the console and the chat
//     box, as a player's chat would be. The emote is for whoever sends it to
//     play with "npc emote"; the storyteller does.
//
//   cmd:objective/<add|done|undone|remove|clear|title>: <text or number>
//     the mission's objectives, listed on the left while the menu (Esc) is up.
//     done strikes one through, undone takes the line off again, remove takes it
//     away; they name it by its text or its number in the list. clear empties the
//     list, or with "done" only takes the struck ones. cmd:objective: <text> adds.
//
//   cmd:cin/<verb>/...
//     a cinematic camera for everyone with this client; see cl_rpcinematic.cpp.
//
//   cmd:map/<verb>/...
//     an admin's map edits, shown to everyone on the map; see cl_mapeditor.cpp.
//     cmd:startcinematic and cmd:endcinematic start and end one too.
//
// Hooks only ever show things: nothing a server sends here runs a command.

#include "client.h"

#define RH_PREFIX		"cmd:"
#define RH_MAX_ARGS		24		// a camera move's spots
// RPMod word-wraps centre prints at about 50 characters a line, and a word longer than
// a line crashes the server: 36 in a row went through, 65 didn't
#define RH_MAX_WORD		40
#define RH_REPEAT_MS	2000	// the same hook again this soon is a copy of it, a print and a cp

#define RH_MAX_OBJECTIVES	12
#define RH_OBJECTIVE_LEN	256
#define RH_OBJ_TITLE		"Objective:"
#define RH_OBJ_X			16.0f
#define RH_OBJ_Y			84.0f		// under RPMod's menu bar and its screenshot buttons
#define RH_OBJ_W			200.0f		// inside the panel
#define RH_OBJ_PAD			6.0f
#define RH_OBJ_ROW_H		14.0f
#define RH_OBJ_SCALE		0.8f
#define RH_OBJ_BULLET		"- "

// NPC talk during a cinematic with the HUD hidden, where the chat box can't be seen
#define RH_MAX_SUBS			3
#define RH_SUB_LEN			512
#define RH_SUB_W			480.0f
#define RH_SUB_SCALE		1.0f
#define RH_SUB_BOTTOM		10.0f		// above the bottom of the screen
#define RH_SUB_FADE_MS		500

typedef enum {
	RH_PASS,		// not a hook, the cgame gets it
	RH_DROP,		// a hook, the cgame never hears of it
	RH_REPLACED		// a hook that hands the cgame another command, already tokenized
} rhResult_t;

// "^1", "1" or "" as a colour code
static const char *RH_Color( const char *in ) {
	static char out[3];

	if ( in[0] == Q_COLOR_ESCAPE && in[1] && in[1] != Q_COLOR_ESCAPE ) {
		out[0] = Q_COLOR_ESCAPE;
		out[1] = in[1];
	} else if ( in[0] && !in[1] && isalnum( (unsigned char)in[0] ) ) {
		out[0] = Q_COLOR_ESCAPE;
		out[1] = in[0];
	} else {
		return S_COLOR_WHITE;
	}
	out[2] = '\0';
	return out;
}

static qboolean RH_IsTalk( const char *verb ) {
	return (qboolean)( !Q_stricmp( verb, "talk" ) || !Q_stricmp( verb, "say" ) );
}

static struct {
	char		text[RH_SUB_LEN];
	int			until;			// cls.realtime it's gone by
}			rhSubs[RH_MAX_SUBS];	// oldest first

// as long as it takes to read, the oldest making way when there are too many
static void RH_AddSubtitle( const char *text ) {
	int ms = 3000 + 60 * (int)strlen( text );

	if ( rhSubs[RH_MAX_SUBS - 1].text[0] )
		memmove( &rhSubs[0], &rhSubs[1], sizeof( rhSubs[0] ) * ( RH_MAX_SUBS - 1 ) );
	for ( int i = 0; i < RH_MAX_SUBS; i++ ) {
		if ( !rhSubs[i].text[0] ) {
			Q_strncpyz( rhSubs[i].text, text, sizeof( rhSubs[i].text ) );
			rhSubs[i].until = cls.realtime + Q_min( ms, 10000 );
			break;
		}
	}
}

// cmd:npc/talk/<npc>/<name>/<colour>/<emote>: <message>
static rhResult_t RH_Npc( char **argv, int argc, const char *message ) {
	char cmd[MAX_STRING_CHARS];
	const char *npc, *name, *color;
	int rest;

	// the verb before the NPC, or after it
	if ( argc >= 2 && RH_IsTalk( argv[0] ) ) {
		npc = argv[1];
	} else if ( argc >= 2 && RH_IsTalk( argv[1] ) ) {
		npc = argv[0];
	} else {
		Com_DPrintf( "RP hook: npc wants talk or say, and the NPC\n" );
		return RH_DROP;
	}
	rest = 2;
	name = argc > rest && argv[rest][0] ? argv[rest] : npc;
	color = RH_Color( argc > rest + 1 ? argv[rest + 1] : "" );
	if ( !message[0] )
		return RH_DROP;

	RH_AddSubtitle( va( "%s%s" S_COLOR_WHITE ": %s", color, name, message ) );
	// chat as the game sends it, from right next to us so RPMod's chat range lets it through
	Com_sprintf( cmd, sizeof( cmd ), "chat \"%s%s" S_COLOR_WHITE "\x19: " S_COLOR_GREEN "%s\" 0", color, name, message );
	Cmd_TokenizeString( cmd );
	return RH_REPLACED;
}

static qboolean rhExperimental;

qboolean CL_RpHooks_Experimental( void ) {
	return rhExperimental;
}

void CL_RpHooks_SetExperimental( qboolean on ) {
	rhExperimental = on;
	Com_Printf( "Experimental features %s\n", on ? S_COLOR_YELLOW "on" S_COLOR_WHITE " for this session" : "off" );
}

// a part of the hook, with what would end it early made spaces
static const char *RH_HookPart( const char *in ) {
	static char out[4][MAX_QPATH];
	static int index;
	char *o = out[index++ & 3];
	int len = 0;

	for ( ; *in && len < MAX_QPATH - 1; in++ )
		o[len++] = ( *in == '/' || *in == ':' ) ? ' ' : *in;
	o[len] = '\0';
	return o;
}

// the command that has everyone with this client see the NPC say it: the storyteller's NPC talk
// and the NPC manager's. An empty part would make a // the server takes for a comment
const char *CL_RpHooks_NpcTalkCommand( const char *npc, const char *name, const char *color, const char *emote, const char *text ) {
	static char cmd[MAX_STRING_CHARS];

	Com_sprintf( cmd, sizeof( cmd ), "rpnotify -1 1 cmd:npc/talk/%s/%s/%s/%s: %s", RH_HookPart( npc ), RH_HookPart( name[0] ? name : npc ),
		RH_HookPart( color[0] ? color : "^7" ), emote[0] ? RH_HookPart( emote ) : "none", text );
	return cmd;
}

static struct {
	char		title[RH_OBJECTIVE_LEN];	// "" for RH_OBJ_TITLE
	struct {
		char		text[RH_OBJECTIVE_LEN];
		qboolean	done;
	}			items[RH_MAX_OBJECTIVES];
	int			count;
} rhObj;

// the objective a number in the list or its text names, -1 if none
static int RH_FindObjective( const char *name ) {
	char a[RH_OBJECTIVE_LEN], b[RH_OBJECTIVE_LEN];

	if ( Q_isanumber( name ) && strchr( name, '.' ) == NULL ) {
		const int n = atoi( name );
		return n >= 1 && n <= rhObj.count ? n - 1 : -1;
	}
	Q_strncpyz( a, name, sizeof( a ) );
	Q_StripColor( a );
	for ( int i = 0; i < rhObj.count; i++ ) {
		Q_strncpyz( b, rhObj.items[i].text, sizeof( b ) );
		Q_StripColor( b );
		if ( !Q_stricmp( a, b ) )
			return i;
	}
	return -1;
}

static void RH_RemoveObjective( int i ) {
	memmove( &rhObj.items[i], &rhObj.items[i + 1], ( rhObj.count - i - 1 ) * sizeof( rhObj.items[0] ) );
	rhObj.count--;
}

// cmd:objective/<verb>: <text or number>
static rhResult_t RH_Objective( char **argv, int argc, const char *message ) {
	const char *verb = argc >= 1 && argv[0][0] ? argv[0] : "add";
	int i;

	if ( !Q_stricmp( verb, "clear" ) ) {
		if ( Q_stricmp( message, "done" ) ) {
			rhObj.count = 0;
			return RH_DROP;
		}
		for ( i = rhObj.count - 1; i >= 0; i-- ) {
			if ( rhObj.items[i].done )
				RH_RemoveObjective( i );
		}
		return RH_DROP;
	}
	if ( !Q_stricmp( verb, "title" ) ) {
		Q_strncpyz( rhObj.title, message, sizeof( rhObj.title ) );
		return RH_DROP;
	}
	if ( !message[0] )
		return RH_DROP;

	i = RH_FindObjective( message );
	if ( !Q_stricmp( verb, "add" ) ) {
		// already there, it's back to not done
		if ( i >= 0 && !Q_isanumber( message ) ) {
			rhObj.items[i].done = qfalse;
			return RH_DROP;
		}
		if ( rhObj.count == RH_MAX_OBJECTIVES ) {
			Com_DPrintf( "RP hook: objectives full, \"%s\" left out\n", message );
			return RH_DROP;
		}
		Q_strncpyz( rhObj.items[rhObj.count].text, message, sizeof( rhObj.items[0].text ) );
		rhObj.items[rhObj.count].done = qfalse;
		rhObj.count++;
		return RH_DROP;
	}
	if ( i < 0 ) {
		Com_DPrintf( "RP hook: no objective \"%s\"\n", message );
		return RH_DROP;
	}
	if ( !Q_stricmp( verb, "done" ) )
		rhObj.items[i].done = qtrue;
	else if ( !Q_stricmp( verb, "undone" ) || !Q_stricmp( verb, "undo" ) )
		rhObj.items[i].done = qfalse;
	else if ( !Q_stricmp( verb, "remove" ) )
		RH_RemoveObjective( i );
	else
		Com_DPrintf( "RP hook: objective wants add, done, undone, remove, clear or title, not \"%s\"\n", verb );
	return RH_DROP;
}

// past "cmd:" if the text is a hook's, NULL if not; colours and spaces before it are let through
static const char *RH_HookText( const char *text ) {
	while ( *text == ' ' || Q_IsColorString( text ) )
		text += *text == ' ' ? 1 : 2;
	if ( Q_stricmpn( text, RH_PREFIX, strlen( RH_PREFIX ) ) )
		return NULL;
	return text + strlen( RH_PREFIX );
}

// the server word-wraps centre prints, a line break where a space was: back to one line
static void RH_Unwrap( const char *in, char *out, int size ) {
	int len = 0;

	for ( ; *in && len < size - 1; in++ ) {
		if ( *in == '\n' || *in == '\r' ) {
			// next to a space already, it was put in beside it rather than for it
			if ( len && out[len - 1] != ' ' && in[1] != ' ' && in[1] != '\n' && in[1] != '\r' && in[1] )
				out[len++] = ' ';
			continue;
		}
		out[len++] = *in;
	}
	out[len] = '\0';
}

static struct {
	char	text[MAX_STRING_CHARS];
	int		time;
} rhLast;

// a print or centre print about to reach the cgame: a hook's, or one to show
static rhResult_t RH_Run( const char *text ) {
	char flat[MAX_STRING_CHARS], header[MAX_STRING_CHARS], *argv[RH_MAX_ARGS], *p;
	const char *colon, *message;
	int argc = 0;

	text = RH_HookText( text );
	if ( !text )
		return RH_PASS;
	RH_Unwrap( text, flat, sizeof( flat ) );
	text = flat;

	// sent both ways, it runs once
	if ( rhLast.time && cls.realtime - rhLast.time < RH_REPEAT_MS && !strcmp( rhLast.text, flat ) )
		return RH_DROP;
	Q_strncpyz( rhLast.text, flat, sizeof( rhLast.text ) );
	rhLast.time = cls.realtime;

	// the hook and its arguments, up to the first colon; the message after it
	colon = strchr( text, ':' );
	message = colon ? colon + 1 : "";
	while ( *message == ' ' )
		message++;
	Q_strncpyz( header, text, colon ? Q_min( (int)sizeof( header ), (int)( colon - text ) + 1 ) : (int)sizeof( header ) );

	// the parts may have spaces around them, to keep the words short for the server
	for ( p = header; argc < RH_MAX_ARGS; ) {
		char *end;

		while ( *p == ' ' )
			p++;
		argv[argc++] = p;
		end = strchr( p, '/' );
		if ( end )
			*end = '\0';
		for ( char *t = p + strlen( p ); t > p && t[-1] == ' '; )
			*--t = '\0';
		if ( !end )
			break;
		p = end + 1;
	}

	if ( !Q_stricmp( argv[0], "npc" ) )
		return RH_Npc( argv + 1, argc - 1, message );
	if ( !Q_stricmp( argv[0], "objective" ) || !Q_stricmp( argv[0], "objectives" ) )
		return RH_Objective( argv + 1, argc - 1, message );
	if ( !Q_stricmp( argv[0], "cin" ) || !Q_stricmp( argv[0], "cinematic" ) ) {
		CL_RpCinematic_Hook( argv + 1, argc - 1 );
		return RH_DROP;
	}
	if ( !Q_stricmp( argv[0], "map" ) ) {
		CL_MapEditor_Hook( argv + 1, argc - 1, message );
		return RH_DROP;
	}
	if ( !Q_stricmp( argv[0], "startcinematic" ) || !Q_stricmp( argv[0], "endcinematic" ) ) {
		char *verb = (char *)( !Q_stricmp( argv[0], "startcinematic" ) ? "start" : "end" );

		argv[0] = verb;
		CL_RpCinematic_Hook( argv, argc );
		return RH_DROP;
	}

	Com_DPrintf( "RP hook: unknown hook \"%s\"\n", argv[0] );
	return RH_DROP;
}

// the most characters in a row without a space
int CL_RpHooks_LongestWord( const char *text ) {
	int longest = 0, run = 0;

	for ( ; *text; text++ ) {
		if ( *text == ' ' || *text == '\n' ) {
			run = 0;
			continue;
		}
		if ( ++run > longest )
			longest = run;
	}
	return longest;
}

// a command on its way to the server: qfalse keeps it here, as it would crash RPMod
qboolean CL_RpHooks_AllowCommand( const char *cmd ) {
	int longest;

	if ( Q_stricmpn( cmd, "rpcentersay ", 12 ) )
		return qtrue;
	longest = CL_RpHooks_LongestWord( cmd + 12 );
	if ( longest <= RH_MAX_WORD )
		return qtrue;
	Com_Printf( S_COLOR_RED "Not sent: rpcentersay text with a word of %i characters crashes the server (%i at most); add spaces\n",
		longest, RH_MAX_WORD );
	return qfalse;
}

/*
===============================================================================

Objectives on screen

===============================================================================
*/

static const vec4_t rhObjPanel	= { 0.0f, 0.0f, 0.0f, 0.6f };
static const vec4_t rhObjBorder	= { 1.0f, 1.0f, 1.0f, 0.25f };
static const vec4_t rhObjTitle	= { 1.0f, 0.8f, 0.3f, 1.0f };
static const vec4_t rhObjText	= { 1.0f, 1.0f, 1.0f, 1.0f };
static const vec4_t rhObjDone	= { 0.6f, 0.6f, 0.6f, 0.8f };

static void RH_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

// text broken at spaces to fit the width, each line's colour carried on to the next; how many lines.
// Drawn only with draw, otherwise just counted
static int RH_WrapEx( int font, float x, float y, float w, float scale, float rowH, const char *text, const float *color,
	qboolean strike, qboolean center, qboolean draw ) {
	char line[RH_OBJECTIVE_LEN + 2];
	char colorCode[3] = "";
	int lines = 0;

	while ( *text ) {
		int len = 0, fit = 0, start = strlen( colorCode );

		Q_strncpyz( line, colorCode, sizeof( line ) );
		while ( text[len] && start + len < (int)sizeof( line ) - 1 ) {
			line[start + len] = text[len];
			line[start + len + 1] = '\0';
			if ( re->Font_StrLenPixels( line, font, scale ) > w )
				break;
			if ( text[len] == ' ' || !text[len + 1] )
				fit = len + 1;
			len++;
		}
		if ( !text[len] )
			fit = len;
		if ( !fit )
			fit = Q_max( len, 1 );
		line[start + fit] = '\0';

		if ( draw ) {
			const float ly = y + lines * rowH;
			const float lw = (float)re->Font_StrLenPixels( line, font, scale );
			const float lx = center ? x + ( w - lw ) * 0.5f : x;

			re->Font_DrawString( (int)lx, (int)ly, line, color, font | STYLE_DROPSHADOW, -1, scale );
			if ( strike )
				RH_Fill( lx, ly + re->Font_HeightPixels( font, scale ) * 0.55f, lw, 1.0f, color );
		}
		for ( int i = 0; i < fit; i++ ) {
			if ( Q_IsColorString( text + i ) ) {
				colorCode[0] = text[i];
				colorCode[1] = text[i + 1];
				colorCode[2] = '\0';
			}
		}
		lines++;
		text += fit;
		while ( *text == ' ' )
			text++;
	}
	return lines;
}

static int RH_Wrap( int font, float x, float y, const char *text, const float *color, qboolean strike, qboolean draw ) {
	return RH_WrapEx( font, x, y, RH_OBJ_W - RH_OBJ_PAD * 2, RH_OBJ_SCALE, RH_OBJ_ROW_H, text, color, strike, qfalse, draw );
}

static int RH_Font( void ) {
	const int font = re->RegisterFont( "arialnb" );
	return font ? font : cls.menuFont;
}

// NPCs talking, at the bottom of the screen: the cinematic hides the chat box with the HUD
void CL_RpHooks_DrawSubtitles( void ) {
	const int font = RH_Font();
	const float rowH = re->Font_HeightPixels( font, RH_SUB_SCALE ) + 2.0f;
	const float x = ( SCREEN_WIDTH - RH_SUB_W ) * 0.5f;
	float y = SCREEN_HEIGHT - RH_SUB_BOTTOM;

	// the newest at the bottom, the ones before it above
	for ( int i = RH_MAX_SUBS - 1; i >= 0; i-- ) {
		vec4_t color = { 1.0f, 1.0f, 1.0f, 1.0f };
		const int left = rhSubs[i].until - cls.realtime;

		if ( !rhSubs[i].text[0] || left <= 0 )
			continue;
		if ( left < RH_SUB_FADE_MS )
			color[3] = (float)left / RH_SUB_FADE_MS;
		y -= RH_WrapEx( font, 0, 0, RH_SUB_W, RH_SUB_SCALE, rowH, rhSubs[i].text, color, qfalse, qtrue, qfalse ) * rowH;
		RH_WrapEx( font, x, y, RH_SUB_W, RH_SUB_SCALE, rowH, rhSubs[i].text, color, qfalse, qtrue, qtrue );
		y -= 2.0f;
	}
}

// the objectives, while the menu is up in game
void CL_RpHooks_Draw( void ) {
	char plain[RH_OBJECTIVE_LEN];
	float x = RH_OBJ_X + RH_OBJ_PAD, y, h;
	int font, lines = 1;

	if ( !rhObj.count || cls.state != CA_ACTIVE || !( Key_GetCatcher() & KEYCATCH_UI ) )
		return;
	font = RH_Font();

	for ( int i = 0; i < rhObj.count; i++ )
		lines += RH_Wrap( font, 0, 0, va( RH_OBJ_BULLET "%s", rhObj.items[i].text ), rhObjText, qfalse, qfalse );
	h = lines * RH_OBJ_ROW_H + RH_OBJ_PAD * 2 + 4;
	RH_Fill( RH_OBJ_X, RH_OBJ_Y, RH_OBJ_W, h, rhObjPanel );
	RH_Fill( RH_OBJ_X, RH_OBJ_Y, RH_OBJ_W, 1, rhObjBorder );
	RH_Fill( RH_OBJ_X, RH_OBJ_Y + h - 1, RH_OBJ_W, 1, rhObjBorder );
	RH_Fill( RH_OBJ_X, RH_OBJ_Y, 1, h, rhObjBorder );
	RH_Fill( RH_OBJ_X + RH_OBJ_W - 1, RH_OBJ_Y, 1, h, rhObjBorder );

	y = RH_OBJ_Y + RH_OBJ_PAD;
	re->Font_DrawString( (int)x, (int)y, rhObj.title[0] ? rhObj.title : RH_OBJ_TITLE, rhObjTitle, font | STYLE_DROPSHADOW, -1, RH_OBJ_SCALE );
	y += RH_OBJ_ROW_H + 4;

	for ( int i = 0; i < rhObj.count; i++ ) {
		if ( rhObj.items[i].done ) {
			// struck through in grey, its own colours left out
			Q_strncpyz( plain, rhObj.items[i].text, sizeof( plain ) );
			Q_StripColor( plain );
			y += RH_Wrap( font, x, y, va( RH_OBJ_BULLET "%s", plain ), rhObjDone, qtrue, qtrue ) * RH_OBJ_ROW_H;
		} else {
			y += RH_Wrap( font, x, y, va( RH_OBJ_BULLET "%s", rhObj.items[i].text ), rhObjText, qfalse, qtrue ) * RH_OBJ_ROW_H;
		}
	}
}

// objective: lists them; objective <verb> [text]: sends one to everyone, hidden in an rpnotify
static void CL_Objective_f( void ) {
	static const char *verbs[] = { "add", "done", "undone", "remove", "clear", "title" };
	const char *verb = Cmd_Argv( 1 ), *text = Cmd_ArgsFrom( 2 );
	qboolean known = qfalse;

	if ( Cmd_Argc() < 2 || !Q_stricmp( verb, "list" ) ) {
		Com_Printf( "%s\n", rhObj.title[0] ? rhObj.title : RH_OBJ_TITLE );
		for ( int i = 0; i < rhObj.count; i++ ) {
			char plain[RH_OBJECTIVE_LEN];

			if ( !rhObj.items[i].done ) {
				Com_Printf( "%2i. %s\n", i + 1, rhObj.items[i].text );
				continue;
			}
			Q_strncpyz( plain, rhObj.items[i].text, sizeof( plain ) );
			Q_StripColor( plain );
			Com_Printf( "%2i. " S_COLOR_GREY "%s (done)\n", i + 1, plain );
		}
		if ( !rhObj.count )
			Com_Printf( "  none\n" );
		return;
	}

	for ( size_t i = 0; i < ARRAY_LEN( verbs ); i++ ) {
		if ( !Q_stricmp( verb, verbs[i] ) )
			known = qtrue;
	}
	if ( !known || ( !text[0] && Q_stricmp( verb, "clear" ) && Q_stricmp( verb, "title" ) ) ) {
		Com_Printf( "objective [list] : the objectives\n"
			"objective add <text> : a new one, for everyone with this client\n"
			"objective done|undone|remove <number or text> : strike one through, take the line off, or take it away\n"
			"objective clear [done] : all of them, or the struck ones\n"
			"objective title [text] : the heading, \"" RH_OBJ_TITLE "\" if none\n" );
		return;
	}
	if ( cls.state != CA_ACTIVE ) {
		Com_Printf( "Not connected to a server.\n" );
		return;
	}
	// the server takes a // for a comment
	if ( strstr( text, "//" ) ) {
		Com_Printf( S_COLOR_YELLOW "Not sent: the server would cut the text at the //\n" );
		return;
	}
	CL_AddReliableCommand( text[0] ? va( "rpnotify -1 1 " RH_PREFIX "objective/%s: %s", verb, text )
		: va( "rpnotify -1 1 " RH_PREFIX "objective/%s", verb ), qfalse );
}

void CL_RpHooks_Init( void ) {
	memset( &rhObj, 0, sizeof( rhObj ) );
	Cmd_AddCommand( "objective", CL_Objective_f, "Lists the mission objectives, or sends one to everyone with this client" );
}

void CL_RpHooks_Shutdown( void ) {
	Cmd_RemoveCommand( "objective" );
}

// a new server is a new story
void CL_RpHooks_Disconnect( void ) {
	memset( &rhObj, 0, sizeof( rhObj ) );
	memset( &rhLast, 0, sizeof( rhLast ) );
	memset( &rhSubs, 0, sizeof( rhSubs ) );
}

// the server command just tokenized: qfalse keeps it from the cgame
qboolean CL_RpHooks_ServerCommand( qboolean *replaced ) {
	const char *cmd = Cmd_Argv( 0 );
	rhResult_t result;

	*replaced = qfalse;
	// rpnotify sends its text to the console (print) and to RPMod's banner (info), rpcentersay
	// to the middle of the screen (cp); the first to arrive runs the hook, the copies are hidden
	if ( strcmp( cmd, "print" ) && strcmp( cmd, "cp" ) && strcmp( cmd, "cps" ) && strcmp( cmd, "info" ) )
		return qtrue;
	for ( int i = 1; i < Cmd_Argc(); i++ ) {
		if ( !RH_HookText( Cmd_Argv( i ) ) )
			continue;
		result = RH_Run( Cmd_Argv( i ) );
		*replaced = (qboolean)( result == RH_REPLACED );
		return (qboolean)( result != RH_DROP );
	}
	return qtrue;
}
