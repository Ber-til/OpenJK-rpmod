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

// cl_games.cpp -- what the datapad's games besides Pazaak share: the lobbies on this server, a table
// to sit at or watch, waiting for players, starting, leaving and dealing again; C1B3R deals and
// checks every game (its /v2/games/<game>, see OpenJK-rpmod-c1b3r's tabletop.py), this shows it and
// sends the moves on the C1B3R session (cl_presence.cpp). Each game draws its own table
// (cl_wanga.cpp, cl_thepath.cpp).
//
// The sounds are Jedi Academy's own unless games/sounds/<click|deal|play|turn|win|lose>.wav is in
// a pk3.

#include <string>
#include "cl_games.h"
#include "cl_presence.h"
#include "cl_widgets.h"

#define GM_MAX_LOBBIES		32
#define GM_NAME_LEN			64
#define GM_LIST_MS			3000		// the lobbies asked again this often while they're shown
#define GM_IDLE_WAIT_MS		2000
#define GM_WAIT_MS			40000		// a wait's answer may take this long: C1B3R holds it 25 s
#define GM_MESSAGE_MS		4000

typedef enum {
	GM_MENU,
	GM_LOBBIES,
	GM_RULES,
	GM_GAME
} gmPage_t;

typedef struct {
	char	id[16], status[16], host[64], name[64];
	int		players, seats, spectators;
} gmLobby_t;

typedef struct {
	const gmGame_t	*def;
	gmTable_t		t;
	qboolean		shown;
	gmPage_t		page;
	int				version;
	gmLobby_t		lobbies[GM_MAX_LOBBIES];
	int				numLobbies;
	int				listTime;
	char			newName[GM_NAME_LEN];
	qboolean		focus;				// the new table's name
	int				waitNext;
	char			message[128];
	int				messageTime;
	qboolean		messageBad;
	httpRequest_t	waitHttp, actHttp, listHttp;
	char			actWhat[32];
	int				rulesScroll;
} gmState_t;

static gmState_t gm[2];

static const char *gmSoundNames[GM_NUM_SOUNDS] = { "click", "deal", "play", "turn", "win", "lose" };
static const char *gmSoundDefaults[GM_NUM_SOUNDS] = {
	"sound/interface/button1.wav", "sound/interface/sub_select.wav", "sound/interface/choose_color.wav",
	"sound/interface/update.wav", "sound/interface/secret_area.wav", "sound/interface/ammocon_empty.wav"
};
static sfxHandle_t gmSounds[GM_NUM_SOUNDS];
static qboolean gmSoundsKnown;

static gmState_t *GM_Of( const gmGame_t *game ) {
	return game == &gmPath ? &gm[GM_PATH] : &gm[GM_WANGA];
}

const gmTable_t *GM_Table( const gmGame_t *game ) {
	return &GM_Of( game )->t;
}

qboolean GM_Playing( const gmGame_t *game ) {
	const gmTable_t *t = GM_Table( game );

	return (qboolean)( t->inLobby && !strcmp( t->status, "playing" ) );
}

static qboolean GM_FileExists( const char *path ) {
	return (qboolean)( FS_ReadFile( path, NULL ) > 0 );
}

qhandle_t GM_Art( const char *path ) {
	static const char *exts[] = { "png", "tga", "jpg" };

	for ( int e = 0; e < 3; e++ ) {
		if ( GM_FileExists( va( "%s.%s", path, exts[e] ) ) )
			return re->RegisterShaderNoMip( path );
	}
	return 0;
}

void GM_Sound( gmSound_t s ) {
	if ( !gmSoundsKnown ) {
		gmSoundsKnown = qtrue;
		for ( int i = 0; i < GM_NUM_SOUNDS; i++ ) {
			const char *own = va( "games/sounds/%s.wav", gmSoundNames[i] );

			gmSounds[i] = S_RegisterSound( GM_FileExists( own ) ? own : gmSoundDefaults[i] );
		}
	}
	DP_PlaySound( gmSounds[s] );
}

void GM_Message( const gmGame_t *game, const char *text, qboolean bad ) {
	gmState_t *g = GM_Of( game );

	Q_strncpyz( g->message, text, sizeof( g->message ) );
	for ( char *nl = strchr( g->message, '\n' ); nl; nl = NULL )
		*nl = '\0';
	g->messageTime = cls.realtime;
	g->messageBad = bad;
}

/*
===============================================================================

Talking to C1B3R

===============================================================================
*/

static qboolean GM_Post( gmState_t *g, httpRequest_t *req, const char *what, const std::string &fields ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session ) {
		GM_Message( g->def, va( "Not connected to C1B3R: %s needs it", g->def->title ), qtrue );
		return qfalse;
	}
	if ( req->state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += fields + "}";
	return CL_Presence_Post( req, va( "/v2/games/%s/%s", g->def->word, what ), body );
}

static std::string GM_Field( const char *name, const char *value ) {
	std::string out = va( ",\"%s\":", name );

	CL_Presence_Json( out, value );
	return out;
}

// an action: one at a time; its answer said in a message when it's refused
static void GM_Act( gmState_t *g, const char *what, const std::string &fields ) {
	if ( g->actHttp.state == HTTP_BUSY )
		return;
	Q_strncpyz( g->actWhat, what, sizeof( g->actWhat ) );
	if ( GM_Post( g, &g->actHttp, what, fields ) )
		g->waitNext = 0;
}

void GM_Move( const gmGame_t *game, const char *fields ) {
	GM_Act( GM_Of( game ), "move", fields );
}

static int GM_Split( char *line, char **field, int max ) {
	int n = 0;

	for ( char *f = line; f && n < max; n++ ) {
		field[n] = f;
		f = strchr( f, '\t' );
		if ( f )
			*f++ = '\0';
	}
	return n;
}

static const char *GM_Line( const char *at, char *line, int size ) {
	const char *end = strchr( at, '\n' );
	const int len = end ? (int)( end - at ) : (int)strlen( at );

	Q_strncpyz( line, at, Q_min( len + 1, size ) );
	return end ? end + 1 : at + len;
}

// what C1B3R says of our table: see tabletop.py's wait
static void GM_ReadState( gmState_t *g, const char *answer ) {
	char line[2048], *f[12];
	const char *at = answer;
	const qboolean wasIn = g->t.inLobby;
	int n;

	at = GM_Line( at, line, sizeof( line ) );
	n = GM_Split( line, f, 12 );
	if ( n < 2 || strcmp( f[0], "ok" ) )
		return;
	g->version = atoi( f[1] );
	g->t.inLobby = qfalse;
	g->t.numSeats = 0;
	g->t.numEvents = 0;
	g->def->reset();
	while ( *at ) {
		at = GM_Line( at, line, sizeof( line ) );
		n = GM_Split( line, f, 12 );
		if ( n >= 7 && !strcmp( f[0], "lobby" ) ) {
			g->t.inLobby = qtrue;
			Q_strncpyz( g->t.lobbyId, f[1], sizeof( g->t.lobbyId ) );
			Q_strncpyz( g->t.status, f[2], sizeof( g->t.status ) );
			g->t.you = !strcmp( f[3], "s" ) ? -1 : atoi( f[3] );
			g->t.maxSeats = atoi( f[5] );
			Q_strncpyz( g->t.name, CL_Presence_Unfield( f[6] ).c_str(), sizeof( g->t.name ) );
		} else if ( n >= 3 && !strcmp( f[0], "seat" ) ) {
			const int i = atoi( f[1] );

			if ( i >= 0 && i < GM_MAX_SEATS ) {
				Q_strncpyz( g->t.seats[i], CL_Presence_Unfield( f[2] ).c_str(), sizeof( g->t.seats[i] ) );
				g->t.numSeats = Q_max( g->t.numSeats, i + 1 );
			}
		} else if ( n >= 2 && !strcmp( f[0], "event" ) ) {
			if ( g->t.numEvents < GM_MAX_EVENTS )
				Q_strncpyz( g->t.events[g->t.numEvents++], CL_Presence_Unfield( f[1] ).c_str(), sizeof( g->t.events[0] ) );
		} else if ( n >= 1 ) {
			g->def->line( f, n );
		}
	}
	// to the table as we sit down; back to the lobbies as it's gone
	if ( g->t.inLobby && !wasIn )
		g->page = GM_GAME;
	else if ( !g->t.inLobby && g->page == GM_GAME )
		g->page = GM_LOBBIES;
	if ( g->t.inLobby )
		g->def->read();
}

static void GM_ReadLobbies( gmState_t *g, const char *answer ) {
	char line[512], *f[10];
	const char *at = answer;

	g->numLobbies = 0;
	at = GM_Line( at, line, sizeof( line ) );
	if ( strncmp( line, "ok", 2 ) )
		return;
	while ( *at && g->numLobbies < GM_MAX_LOBBIES ) {
		gmLobby_t *l = &g->lobbies[g->numLobbies];

		at = GM_Line( at, line, sizeof( line ) );
		if ( GM_Split( line, f, 10 ) < 8 || strcmp( f[0], "lobby" ) )
			continue;
		Q_strncpyz( l->id, f[1], sizeof( l->id ) );
		Q_strncpyz( l->status, f[2], sizeof( l->status ) );
		l->players = atoi( f[3] );
		l->seats = atoi( f[4] );
		l->spectators = atoi( f[5] );
		Q_strncpyz( l->host, CL_Presence_Unfield( f[6] ).c_str(), sizeof( l->host ) );
		Q_strncpyz( l->name, CL_Presence_Unfield( f[7] ).c_str(), sizeof( l->name ) );
		g->numLobbies++;
	}
}

static void GM_FrameOne( gmState_t *g ) {
	if ( HTTP_Poll( &g->waitHttp ) ) {
		if ( g->waitHttp.status == 200 ) {
			GM_ReadState( g, g->waitHttp.answer );
			g->waitNext = g->t.inLobby ? 0 : cls.realtime + GM_IDLE_WAIT_MS;
		} else {
			g->waitNext = cls.realtime + GM_IDLE_WAIT_MS;
		}
	}
	if ( HTTP_Poll( &g->actHttp ) ) {
		if ( g->actHttp.status != 200 )
			GM_Message( g->def, g->actHttp.answer[0] ? g->actHttp.answer : "C1B3R didn't answer", qtrue );
		else if ( !strcmp( g->actWhat, "leave" ) )
			g->listTime = 0;
		g->waitNext = 0;
	}
	if ( HTTP_Poll( &g->listHttp ) && g->listHttp.status == 200 )
		GM_ReadLobbies( g, g->listHttp.answer );

	if ( !g->shown || !CL_Presence_Session() )
		return;
	if ( g->waitHttp.state != HTTP_BUSY && cls.realtime >= g->waitNext ) {
		g->waitHttp.waitMs = GM_WAIT_MS;
		if ( !GM_Post( g, &g->waitHttp, "wait", va( ",\"after\":%i", g->t.inLobby ? g->version : 0 ) ) )
			g->waitNext = cls.realtime + GM_IDLE_WAIT_MS;
	}
	if ( g->page == GM_LOBBIES && cls.realtime - g->listTime >= GM_LIST_MS && g->listHttp.state != HTTP_BUSY
		&& GM_Post( g, &g->listHttp, "lobbies", "" ) )
		g->listTime = cls.realtime;
}

void CL_Games_Frame( void ) {
	GM_FrameOne( &gm[GM_WANGA] );
	GM_FrameOne( &gm[GM_PATH] );
}

void CL_Games_Shown( int which, qboolean shown ) {
	gmState_t *g = &gm[which & 1];

	if ( shown && !g->shown ) {
		g->waitNext = 0;
		g->listTime = 0;
		if ( g->page != GM_GAME )
			g->page = GM_MENU;
		if ( g->def->reload )
			g->def->reload();
	}
	g->shown = shown;
	g->focus = qfalse;
}

/*
===============================================================================

The pages

===============================================================================
*/

static void GM_Title( gmState_t *g, const dpRect_t *s, float *y, const char *title, qboolean back ) {
	const float x = s->x + DP_Wide( 8 );

	if ( back && DP_Button( x, *y, DP_Wide( 50 ), "< Back", qfalse ) ) {
		GM_Sound( GM_SND_CLICK );
		g->page = GM_MENU;
		g->focus = qfalse;
	}
	DP_Text( x + ( back ? DP_Wide( 58 ) : 0 ), *y, s->w, title, dpText, DP_TitleFont(), 0.62f, qfalse );
	*y += DP_CTRL_H + 8;
}

static void GM_DrawMenu( gmState_t *g, const dpRect_t *s ) {
	const float x = s->x + DP_Wide( 30 ), w = s->w - DP_Wide( 60 );
	float y = s->y + DP_STATUS_H + 6;

	if ( DP_Button( s->x + DP_Wide( 8 ), y, DP_Wide( 56 ), "< Games", qfalse ) ) {
		GM_Sound( GM_SND_CLICK );
		CL_Datapad_ToGames();
		return;
	}
	y += 20;
	DP_Text( s->x, y, s->w, g->def->title, dpText, DP_TitleFont(), 1.1f, qtrue );
	y += 30;
	DP_Text( s->x + DP_Wide( 8 ), y, s->w - DP_Wide( 16 ), g->def->tagline, dpDim, DP_Font(), 0.6f, qtrue );
	y += 30;
	if ( DP_Button( x, y, w, "Play: tables on this server", qfalse ) ) {
		g->page = GM_LOBBIES;
		g->listTime = 0;
	}
	y += DP_CTRL_H + 8;
	if ( DP_Button( x, y, w, "Rules", qfalse ) ) {
		g->page = GM_RULES;
		g->rulesScroll = 0;
	}
	if ( g->t.inLobby ) {
		y += DP_CTRL_H + 8;
		if ( DP_Button( x, y, w, "Back to my table", qfalse ) )
			g->page = GM_GAME;
	}
}

static void GM_DrawRules( gmState_t *g, const dpRect_t *s ) {
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 16 );
	float y = s->y + DP_STATUS_H + 4;
	const int fit = (int)( ( s->y + s->h - 40 - y - DP_CTRL_H - 8 ) / 12 );

	GM_Title( g, s, &y, "Rules", qtrue );
	g->rulesScroll = Com_Clampi( 0, Q_max( 0, g->def->numRules - fit ), g->rulesScroll );
	for ( int i = g->rulesScroll; i < g->def->numRules && i < g->rulesScroll + fit; i++ ) {
		const char *line = g->def->rules[i];

		DP_Text( x, y, w - DP_Wide( 8 ), line, line[0] == '^' ? dpText : dpDim, DP_Font(), 0.56f, qfalse );
		y += 12;
	}
	if ( g->def->numRules > fit ) {
		const float by = s->y + s->h - 40;
		const float half = ( w - DP_Wide( 4 ) ) * 0.5f;

		DP_ScrollBar( x + w - DP_Wide( 4 ), s->y + DP_STATUS_H + 4 + DP_CTRL_H + 8, fit * 12.0f, (float)g->rulesScroll, (float)g->def->numRules - fit );
		if ( DP_Button( x, by, half, "Up", qfalse ) )
			g->rulesScroll -= fit - 2;
		if ( DP_Button( x + half + DP_Wide( 4 ), by, half, "Down", qfalse ) )
			g->rulesScroll += fit - 2;
	}
}

// a text field on the datapad, in its colours
static void GM_Field( gmState_t *g, float x, float y, float w, char *buf, const char *placeholder ) {
	wgStyle_t style;

	memset( &style, 0, sizeof( style ) );
	style.font = DP_Font();
	style.scale = 0.62f;
	style.lineH = 12;
	style.padX = 3;
	style.padY = 2;
	style.fill = dpScreen;
	style.border = dpGlow;
	style.text = dpText;
	style.dim = dpDim;
	DP_Fill( x, y, w, DP_CTRL_H, dpButton );
	DP_Frame( x, y, w, DP_CTRL_H, g->focus ? dpWhite : dpGlow );
	WG_Field( &style, x, y, w, DP_CTRL_H, buf, g->focus, placeholder, DP_CursorX(), DP_CursorY() );
	if ( DP_Clicked( x, y, w, DP_CTRL_H ) ) {
		if ( !g->focus )
			WG_EditReset();
		g->focus = qtrue;
		WG_FieldClick( buf, DP_CursorX(), DP_CursorY() );
	}
}

static void GM_DrawLobbies( gmState_t *g, const dpRect_t *s ) {
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 16 ), bw = DP_Wide( 46 );
	float y = s->y + DP_STATUS_H + 4;

	GM_Title( g, s, &y, "Tables on this server", qtrue );
	for ( int i = 0; i < g->numLobbies && i < 10; i++ ) {
		const gmLobby_t *l = &g->lobbies[i];
		const qboolean open = (qboolean)( !strcmp( l->status, "waiting" ) && l->players < l->seats );

		DP_Fill( x, y, w, 26, dpTile );
		DP_Text( x + DP_Wide( 4 ), y + 2, w - bw - DP_Wide( 10 ), l->name, dpText, DP_Font(), 0.62f, qfalse );
		DP_Text( x + DP_Wide( 4 ), y + 14, w - bw - DP_Wide( 10 ),
			va( "%s's, %i of %i%s%s", l->host, l->players, l->seats, !strcmp( l->status, "waiting" ) ? ", waiting" : ", playing",
				l->spectators ? va( ", %i watching", l->spectators ) : "" ), dpDim, DP_Font(), 0.55f, qfalse );
		if ( DP_Button( x + w - bw - DP_Wide( 3 ), y + 5, bw, open ? "Join" : "Watch", qfalse ) ) {
			GM_Sound( GM_SND_CLICK );
			GM_Act( g, open ? "join" : "spectate", GM_Field( "lobby", l->id ) );
		}
		y += 30;
	}
	if ( !g->numLobbies )
		DP_Text( x, y + 4, w, "No one's playing here yet: start a table below.", dpDim, DP_Font(), 0.6f, qfalse );

	y = s->y + s->h - ( DP_CTRL_H + 6 ) * 2 - 60;
	DP_Fill( x, y - 6, w, 1, dpGlow );
	DP_Text( x, y, w, "Start a table", dpText, DP_Font(), 0.62f, qfalse );
	y += 14;
	GM_Field( g, x, y, w, g->newName, "its name (optional)" );
	y += DP_CTRL_H + 6;
	if ( DP_Button( x, y, w, "Start it and wait for players", qfalse ) ) {
		GM_Sound( GM_SND_CLICK );
		GM_Act( g, "create", GM_Field( "name", g->newName ) );
		g->focus = qfalse;
	}
}

void GM_DrawEvents( const gmGame_t *game, float x, float y, float w, int lines ) {
	const gmTable_t *t = GM_Table( game );

	for ( int i = Q_max( 0, t->numEvents - lines ), line = 0; i < t->numEvents; i++, line++ )
		DP_Text( x, y + line * 11, w, t->events[i], dpDim, DP_Font(), 0.55f, qfalse );
}

// the table: waiting for players, or the game as its own file draws it, and leaving under it
static void GM_DrawGame( gmState_t *g, const dpRect_t *s ) {
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 16 );
	const float by = s->y + s->h - DP_CTRL_H - 40;
	const qboolean host = (qboolean)( g->t.you == 0 );
	float y = s->y + DP_STATUS_H + 4;

	if ( !strcmp( g->t.status, "waiting" ) ) {
		const float half = ( w - DP_Wide( 4 ) ) * 0.5f;

		GM_Title( g, s, &y, g->t.name, qfalse );
		DP_Text( x, y, w, va( "Players: %i of %i", g->t.numSeats, g->t.maxSeats ), dpText, DP_Font(), 0.62f, qfalse );
		y += 14;
		for ( int i = 0; i < g->t.numSeats; i++ ) {
			DP_Text( x + DP_Wide( 6 ), y, w, va( "%s%s%s", g->t.seats[i], i == 0 ? "  (host)" : "", i == g->t.you ? "  (you)" : "" ),
				i == g->t.you ? dpWhite : dpDim, DP_Font(), 0.6f, qfalse );
			y += 12;
		}
		y += 10;
		DP_Text( x, y, w, host ? ( g->t.numSeats >= 2 ? "Start when everyone's here." : "Waiting for someone to join..." )
			: "Waiting for the host to start...", dpText, DP_Font(), 0.62f, qfalse );
		if ( host && g->t.maxSeats > 2 && DP_Button( x, by, half, "Start", (qboolean)( g->t.numSeats >= 2 ) ) && g->t.numSeats >= 2 ) {
			GM_Sound( GM_SND_DEAL );
			GM_Act( g, "start", "" );
		}
		if ( DP_Button( x + ( host && g->t.maxSeats > 2 ? half + DP_Wide( 4 ) : 0 ), by, host && g->t.maxSeats > 2 ? half : w,
			host ? "Close the table" : "Leave", qfalse ) )
			GM_Act( g, "leave", "" );
		return;
	}

	g->def->draw( s, y, by - 4 );

	// under the table: leaving, or dealing again
	{
		const float half = ( w - DP_Wide( 4 ) ) * 0.5f;
		const qboolean over = (qboolean)!strcmp( g->t.status, "over" );

		if ( over && host ) {
			if ( DP_Button( x, by, half, "Play again", qfalse ) ) {
				GM_Sound( GM_SND_DEAL );
				GM_Act( g, "again", "" );
			}
			if ( DP_Button( x + half + DP_Wide( 4 ), by, half, "Leave the table", qfalse ) )
				GM_Act( g, "leave", "" );
		} else if ( DP_Button( x, by, w, g->t.you < 0 ? "Stop watching" : over ? "Leave the table" : "Leave (you forfeit)", qfalse ) ) {
			GM_Act( g, "leave", "" );
		}
	}
}

void CL_Games_Draw( int which, const dpRect_t *s ) {
	gmState_t *g = &gm[which & 1];

	switch ( g->page ) {
	case GM_LOBBIES:	GM_DrawLobbies( g, s );	break;
	case GM_RULES:		GM_DrawRules( g, s );	break;
	case GM_GAME:		GM_DrawGame( g, s );	break;
	default:			GM_DrawMenu( g, s );	break;
	}
	if ( g->message[0] && cls.realtime - g->messageTime < GM_MESSAGE_MS )
		DP_Text( s->x + DP_Wide( 8 ), s->y + s->h - 20, s->w - DP_Wide( 16 ), g->message, g->messageBad ? dpWhite : dpText, DP_Font(), 0.6f, qfalse );
	WG_DrawOverlay();
}

qboolean CL_Games_Key( int which, int key ) {
	gmState_t *g = &gm[which & 1];

	if ( !g->focus )
		return qfalse;
	if ( key == A_ENTER || key == A_KP_ENTER || key == A_TAB ) {
		g->focus = qfalse;
		return qtrue;
	}
	WG_EditKey( g->newName, GM_NAME_LEN, key, NULL );
	return qtrue;
}

void CL_Games_Char( int which, int ch ) {
	gmState_t *g = &gm[which & 1];

	if ( g->focus )
		WG_EditChar( g->newName, GM_NAME_LEN, ch, NULL );
}

// the same as the buttons, from the console or a bind: <game> create [name] | join <id> | watch <id> |
// start | again | leave | page <menu|lobbies|rules>, and the game's own moves
static void GM_Command( gmState_t *g ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !Q_stricmp( arg, "create" ) ) {
		GM_Act( g, "create", GM_Field( "name", Cmd_ArgsFrom( 2 ) ) );
	} else if ( !Q_stricmp( arg, "join" ) || !Q_stricmp( arg, "watch" ) ) {
		GM_Act( g, !Q_stricmp( arg, "join" ) ? "join" : "spectate", GM_Field( "lobby", Cmd_Argv( 2 ) ) );
	} else if ( !Q_stricmp( arg, "start" ) || !Q_stricmp( arg, "again" ) || !Q_stricmp( arg, "leave" ) ) {
		GM_Act( g, Q_strlwr( (char *)va( "%s", arg ) ), "" );
	} else if ( !Q_stricmp( arg, "page" ) ) {
		const char *page = Cmd_Argv( 2 );

		g->page = !Q_stricmp( page, "lobbies" ) ? GM_LOBBIES : !Q_stricmp( page, "rules" ) ? GM_RULES : g->t.inLobby ? GM_GAME : GM_MENU;
		g->listTime = 0;
	} else if ( g->def->command && g->def->command() ) {
		// the game's own move
	} else {
		Com_Printf( "usage: %s create [name] | join <table> | watch <table> | start | again | leave | page menu|lobbies|rules\n", g->def->word );
		Com_Printf( "       %s %s\n", g->def->word, g->def->usage );
		Com_Printf( "%s\n", g->t.inLobby ? va( "At table %s (%s)", g->t.lobbyId, g->t.status ) : "Not at a table" );
	}
}

static void GM_Wanga_f( void ) {
	GM_Command( &gm[GM_WANGA] );
}

static void GM_Path_f( void ) {
	GM_Command( &gm[GM_PATH] );
}

void CL_Games_Init( void ) {
	memset( gm, 0, sizeof( gm ) );
	gm[GM_WANGA].def = &gmWanga;
	gm[GM_PATH].def = &gmPath;
	for ( int i = 0; i < 2; i++ )
		gm[i].t.you = -1;
	Cmd_AddCommand( "wanga", GM_Wanga_f, "WANGA's tables and moves, as its buttons on the datapad (datapad wanga opens it)" );
	Cmd_AddCommand( "thepath", GM_Path_f, "The Path's tables and moves, as its buttons on the datapad (datapad path opens it)" );
}
