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

// cl_presence.cpp -- the link to C1B3R: everyone with this client on a server, and what they share
//
// Once loaded into a server the game opens a session with C1B3R (cl_c1b3rUrl, also where feedback
// goes), which is also who's playing with the client, and keeps a poll open that brings
// what the others send as soon as it's sent: the tools' hooks (cl_rphooks.cpp). Everyone with the
// client is connected, and receives.
//
// Sending is for admins, and staff say who: /c1b3r link, logged in to an RPMod account, gets a code
// from C1B3R; /link <code> in Discord ties the game to that Discord account, and staff approve or deny
// it there. The game is handed a token it keeps (cl_c1b3rToken, in the .cfg), which every session
// carries from then on. It sends once approved, while logged in to the RPMod account it was linked
// with: the game reads the account from rpinfo, its reply kept out of the console. /c1b3r unlink, or
// /unlink in Discord, ends it.
//
// The tools send their hooks with CL_Presence_Hook, or as the command c1b3rhook <all|client> cmd:...
// (a story's steps, binds); rpnotify -1 1 cmd:..., as they were made before C1B3R, is still taken.
// Taken here from CL_AddReliableCommand, a hook never reaches the server: it runs here at once and
// goes to C1B3R once, for everyone or the one player it names. With nobody to send it to (a local
// server, no C1B3R) it only runs here. The tools' (map edits, cinematics, stories, NPCs, the sky,
// objectives, combat) are RPMod admins' only, as C1B3R says (relay.py ADMIN_HOOKS): from anyone else
// they're not even run here.

#include "client.h"
#include "cl_http.h"
#include "cl_presence.h"
#include "qcommon/game_version.h"
#include <deque>
#include <string>

#define PS_DEFAULT_URL		"https://c1b3r.keztor.tech"
#define PS_RETRY_MS			30000				// after C1B3R didn't answer
#define PS_NOTE_MS			30000				// "not sent" said at most this often
#define PS_RELAY_MAX		1024				// hooks waiting to go to C1B3R
#define PS_NAME_LEN			128
#define PS_POLL_WAIT_MS		40000				// a poll's answer may take this long: C1B3R holds it 25 s
#define PS_POLL_RETRY_MS	5000				// after a poll that failed
#define PS_HELLO_RETRY_MS	15000
#define PS_ACCOUNT_WAIT_MS	6000				// in game this long before the account is asked: the welcome prints first
#define PS_ACCOUNT_RETRY_MS	30000				// the server didn't answer rpinfo: asked again
#define PS_LINK_WAIT_MS		3000				// a link's code asked about this often, till it's used in Discord

typedef struct {
	int			num;
	char		name[PS_NAME_LEN];
	char		version[32];
	qboolean	linked;
	qboolean	sends;
} psPlayer_t;

typedef struct {
	std::string	text;
	int			to;					// a player, -1 everyone
	int			serverId;			// the load of the map it was made in (C1B3R's server state follows it)
} psHook_t;

static struct {
	char		server[64];			// the server all this is about
	int			joinTime;

	// who has the client here
	qboolean	known;
	psPlayer_t	players[MAX_CLIENTS];
	int			numPlayers;
	int			askNext;			// not before this, after a failure
	qboolean	print;				// /clientusers is waiting for it
	int			noteTime;
} ps;

// the session
static struct {
	qboolean	on;
	char		id[64];
	char		pollId[64];			// the session the open poll is for: one replaced since says nothing
	int			seq;				// the last hook seen
	char		key[512];			// the server, client number, name and token it's for
	char		asked[512];			// a hello going out for these
	int			helloNext;
	int			pollNext;
	qboolean	linked;				// its token is for a link that's pending or approved
	qboolean	approved;
	qboolean	sends;
	qboolean	admin;				// an RPMod admin, as C1B3R last heard from RPMod: the tools' hooks are theirs to send
	int			helloStatus;		// C1B3R's last answer to a hello, for /clientusers
} psv;

// the RPMod account we're logged in to, and linking this game
static struct {
	char		account[64];		// "" logged out, or not known
	qboolean	known;
	qboolean	wanted;				// /c1b3r link waits for the account
	char		code[16];			// the code shown, waited on till it's used in Discord
	int			codeUntil;
	int			waitNext;
} psa;

static std::deque<psHook_t>		psRelayOut;	// hooks going to C1B3R, one at a time
static qboolean			psReplaying;		// a hook of the server's state, replayed to us, being run
static httpRequest_t			psListHttp, psHelloHttp, psPollHttp, psHookHttp, psLinkHttp, psWaitHttp, psByeHttp, psStateHttp, psWebHttp;
static cvar_t					*cl_presenceUrl, *cl_presenceLocal, *cl_c1b3rToken;

/*
===============================================================================

What's around

===============================================================================
*/

// trying it on a server of this computer's: logged in to an account "local"
static qboolean PS_LocalTest( void ) {
	return (qboolean)( com_sv_running && com_sv_running->integer && cl_presenceLocal->integer );
}

static qboolean PS_Active( void ) {
	return (qboolean)( cl_presenceUrl && cl_presenceUrl->string[0] && cls.state == CA_ACTIVE && !clc.demoplaying
		&& ( !( com_sv_running && com_sv_running->integer ) || cl_presenceLocal->integer ) );
}

// the name in a player slot, as the server has it; "" when nobody's there
static const char *PS_SlotName( int num ) {
	int ofs;

	if ( num < 0 || num >= MAX_CLIENTS || !( ofs = cl.gameState.stringOffsets[CS_PLAYERS + num] ) )
		return "";
	return Info_ValueForKey( cl.gameState.stringData + ofs, "n" );
}

static const char *PS_MyName( void ) {
	const char *name = PS_SlotName( clc.clientNum );

	return name[0] ? name : Cvar_VariableString( "name" );
}

/*
===============================================================================

Talking to C1B3R

===============================================================================
*/

// a string for JSON, quoted: the game's letters past ASCII taken as Latin-1
static void PS_Json( std::string &out, const char *s ) {
	out += '"';
	for ( ; *s; s++ ) {
		const unsigned char c = (unsigned char)*s;

		if ( c == '"' || c == '\\' ) {
			out += '\\';
			out += (char)c;
		} else if ( c < 32 || c >= 127 ) {
			out += va( "\\u%04x", c );
		} else {
			out += (char)c;
		}
	}
	out += '"';
}

static const char *PS_Url( const char *path ) {
	static char url[512];
	int len;

	Q_strncpyz( url, cl_presenceUrl->string, sizeof( url ) );
	len = (int)strlen( url );
	while ( len > 0 && url[len - 1] == '/' )
		url[--len] = '\0';
	Q_strcat( url, sizeof( url ), path );
	return url;
}

static qboolean PS_Send( httpRequest_t *req, const char *path, const std::string &body ) {
	char error[128];

	if ( req->state == HTTP_BUSY )
		return qfalse;
	if ( !HTTP_Start( req, PS_Url( path ), body.c_str(), error, sizeof( error ) ) ) {
		Com_DPrintf( "clientusers: %s\n", error );
		return qfalse;
	}
	return qtrue;
}

// the fields of an answer line, split at its tabs; how many
static int PS_Fields( char *line, char **field, int max ) {
	int n = 0;

	for ( char *f = line; f && n < max; n++ ) {
		field[n] = f;
		f = strpbrk( f, "\t\n" );
		if ( f )
			*f++ = '\0';
	}
	return n;
}

// a field of C1B3R's answer as it was sent: \t \n \\ back, and its UTF-8 back to the game's Latin-1
static std::string PS_Unfield( const char *in ) {
	std::string out;

	for ( const unsigned char *p = (const unsigned char *)in; *p; p++ ) {
		if ( *p == '\\' && p[1] ) {
			p++;
			out += *p == 't' ? '\t' : *p == 'n' ? '\n' : (char)*p;
		} else if ( ( *p & 0xE0 ) == 0xC0 && ( p[1] & 0xC0 ) == 0x80 ) {
			const int c = ( ( *p & 0x1F ) << 6 ) | ( p[1] & 0x3F );

			out += (char)( c < 256 ? c : '?' );
			p++;
		} else {
			out += (char)*p;
		}
	}
	return out;
}

/*
===============================================================================

Who has the client here

===============================================================================
*/

static void PS_AskList( void ) {
	std::string body = "{\"session\":";

	if ( !psv.on || psListHttp.state == HTTP_BUSY || cls.realtime < ps.askNext )
		return;
	PS_Json( body, psv.id );
	body += "}";
	if ( !PS_Send( &psListHttp, "/v2/list", body ) )
		ps.askNext = cls.realtime + PS_RETRY_MS;
}

static const char *PS_Rank( void );

static const char *PS_State( void ) {
	if ( !psv.on )
		return va( S_COLOR_YELLOW "not connected" S_COLOR_WHITE " (%s)", psv.helloStatus ? va( "C1B3R answered %i", psv.helloStatus ) : "C1B3R didn't answer" );
	if ( psv.sends )
		return S_COLOR_GREEN "connected and approved: you may send" S_COLOR_WHITE;
	if ( psv.approved && CL_RpAdmin_OnTemple() && psa.account[0] )
		return va( "connected and approved, but on Temple only Department Head, Council and God send (your rank: %s)",
			PS_Rank()[0] ? PS_Rank() : "none known" );
	if ( psv.approved )
		return va( "connected and approved, but you send only logged in to the RPMod account you linked%s",
			psa.account[0] ? va( " (now: %s)", psa.account ) : " (now: logged out)" );
	if ( psv.linked )
		return "connected and linked: you receive; you send once staff approve your link in Discord";
	return "connected: you receive (admins: /c1b3r link to send)";
}

static void PS_PrintList( void ) {
	Com_Printf( "C1B3R: %s\n", PS_State() );
	if ( !ps.known ) {
		Com_Printf( "Players with this client on %s: not known\n", ps.server );
		return;
	}
	Com_Printf( "Players with this client on %s:\n", ps.server );
	for ( int i = 0; i < ps.numPlayers; i++ ) {
		const psPlayer_t *p = &ps.players[i];

		Com_Printf( "  %2i  %s^7  %s%s%s\n", p->num, p->name, p->version,
			p->linked ? S_COLOR_GREY "  linked" : "", p->sends ? S_COLOR_GREEN "  sends" : "" );
	}
}

// ok, then: player <tab> client <tab> version <tab> linked 0|1 <tab> sends 0|1 <tab> name
static void PS_ReadList( const char *answer ) {
	char line[512];
	const char *at = answer, *end;
	qboolean first = qtrue;

	ps.numPlayers = 0;
	for ( ; *at; at = *end ? end + 1 : end ) {
		char *field[6];
		int n;

		end = strchr( at, '\n' );
		if ( !end )
			end = at + strlen( at );
		Q_strncpyz( line, at, Q_min( (int)sizeof( line ), (int)( end - at ) + 1 ) );
		n = PS_Fields( line, field, 6 );
		if ( first ) {
			first = qfalse;
			if ( n < 1 || strcmp( field[0], "ok" ) )
				return;
			ps.known = qtrue;
			continue;
		}
		if ( n == 6 && !strcmp( field[0], "player" ) && ps.numPlayers < MAX_CLIENTS ) {
			psPlayer_t *p = &ps.players[ps.numPlayers];

			p->num = atoi( field[1] );
			if ( p->num < 0 || p->num >= MAX_CLIENTS )
				continue;
			Q_strncpyz( p->version, field[2], sizeof( p->version ) );
			p->linked = (qboolean)!strcmp( field[3], "1" );
			p->sends = (qboolean)!strcmp( field[4], "1" );
			Q_strncpyz( p->name, PS_Unfield( field[5] ).c_str(), sizeof( p->name ) );
			ps.numPlayers++;
		}
	}
}

/*
===============================================================================

The session and the poll

===============================================================================
*/

static const char *PS_Token( void ) {
	return cl_c1b3rToken->string;
}

// what C1B3R says this session may do: linked, approved, sends, and the tools' as an RPMod admin
static void PS_SetState( const char *linked, const char *approved, const char *sends, const char *admin ) {
	const qboolean wasSending = psv.sends;

	psv.linked = (qboolean)!strcmp( linked, "1" );
	psv.approved = (qboolean)!strcmp( approved, "1" );
	psv.sends = (qboolean)!strcmp( sends, "1" );
	// a C1B3R from before it said (the VPS until it's updated): whoever sends, as then
	psv.admin = admin ? (qboolean)!strcmp( admin, "1" ) : psv.sends;
	if ( psv.sends && !wasSending && PS_Token()[0] )
		Com_Printf( S_COLOR_GREEN "C1B3R: " S_COLOR_WHITE "you may send what the tools share\n" );
}

// the RPMod admin rank, as rpstatus said (on Temple it's asked by itself): only some send there
static const char *PS_Rank( void ) {
	const char *rank = PS_LocalTest() ? "God" : CL_RpAdmin_Rank();

	return rank ? rank : "";
}

// a session for where we are: made when there's none, or the server, number, name, account, rank or link changed
static void PS_MapFields( std::string &body, int serverId );

static void PS_Hello( void ) {
	char key[512];
	std::string body;

	if ( psHelloHttp.state == HTTP_BUSY || cls.realtime < psv.helloNext )
		return;
	Com_sprintf( key, sizeof( key ), "%s|%i|%s|%s|%s|%s|%s", ps.server, clc.clientNum, PS_MyName(), PS_Token(), psa.account, PS_Rank(),
		Cvar_VariableString( "saber1" ) );
	if ( psv.on && !strcmp( key, psv.key ) )
		return;
	body = "{\"key\":";
	PS_Json( body, CL_Feedback_Key() );
	body += ",\"server\":";
	PS_Json( body, ps.server );
	body += va( ",\"client\":%i,\"name\":", clc.clientNum );
	PS_Json( body, PS_MyName() );
	body += ",\"version\":";
	PS_Json( body, GIT_TAG );
	if ( PS_Token()[0] ) {
		body += ",\"token\":";
		PS_Json( body, PS_Token() );
	}
	body += ",\"account\":";
	PS_Json( body, psa.account );
	body += ",\"rank\":";
	PS_Json( body, PS_Rank() );
	// the saber carried: the inventory's hilt is it (C1B3R's characters.py)
	body += ",\"saber\":";
	PS_Json( body, Cvar_VariableString( "saber1" ) );
	PS_MapFields( body, cl.serverId );
	body += "}";
	if ( !PS_Send( &psHelloHttp, "/v2/hello", body ) ) {
		psv.helloNext = cls.realtime + PS_HELLO_RETRY_MS;
		return;
	}
	Q_strncpyz( psv.asked, key, sizeof( psv.asked ) );
}

// which map, and which load of it, this game is in: C1B3R clears the server's state when it's
// another (serverstate.py)
static void PS_MapFields( std::string &body, int serverId ) {
	const char *info = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];

	if ( !serverId )
		return;
	body += va( ",\"serverId\":\"%i\",\"map\":", serverId );
	PS_Json( body, Info_ValueForKey( info, "mapname" ) );
}

// the poll kept open: C1B3R answers it when there's a hook for us, or after a while with none
static void PS_Poll( void ) {
	std::string body = "{\"session\":";

	if ( !psv.on || psPollHttp.state == HTTP_BUSY || cls.realtime < psv.pollNext )
		return;
	PS_Json( body, psv.id );
	body += va( ",\"after\":%i", psv.seq );
	PS_MapFields( body, cl.serverId );
	body += "}";
	psPollHttp.waitMs = PS_POLL_WAIT_MS;
	if ( !PS_Send( &psPollHttp, "/v2/poll", body ) )
		psv.pollNext = cls.realtime + PS_POLL_RETRY_MS;
	else
		Q_strncpyz( psv.pollId, psv.id, sizeof( psv.pollId ) );
}

// ok <tab> seq, then: hook <tab> seq <tab> from client <tab> text
static void PS_ReadPoll( const char *answer ) {
	std::string all( answer );
	size_t at = 0;
	int last = psv.seq;
	qboolean first = qtrue;

	while ( at < all.size() ) {
		size_t end = all.find( '\n', at );
		std::string line;

		if ( end == std::string::npos )
			end = all.size();
		line = all.substr( at, end - at );
		at = end + 1;
		if ( first ) {
			first = qfalse;
			if ( line.compare( 0, 3, "ok\t" ) )
				return;
			last = Q_max( last, atoi( line.c_str() + 3 ) );
			continue;
		}
		// what we may do now: staff may have approved or taken it back since
		if ( !line.compare( 0, 6, "state\t" ) ) {
			char buf[64], *field[5];
			int n;

			Q_strncpyz( buf, line.c_str(), sizeof( buf ) );
			if ( ( n = PS_Fields( buf, field, 5 ) ) >= 4 )
				PS_SetState( field[1], field[2], field[3], n >= 5 ? field[4] : NULL );
			continue;
		}
		// someone new on the server: its state, as C1B3R keeps it (serverstate.py), before the hooks
		if ( !line.compare( 0, 7, "replay\t" ) ) {
			const size_t a = line.find( '\t', 7 );

			if ( a != std::string::npos ) {
				psReplaying = qtrue;
				CL_RpHooks_Deliver( PS_Unfield( line.c_str() + a + 1 ).c_str() );
				psReplaying = qfalse;
			}
			continue;
		}
		if ( !line.compare( 0, 5, "hook\t" ) ) {
			const size_t a = line.find( '\t', 5 ), b = a == std::string::npos ? a : line.find( '\t', a + 1 );
			const int seq = atoi( line.c_str() + 5 );

			if ( b == std::string::npos || seq <= psv.seq )
				continue;
			psv.seq = seq;
			CL_RpHooks_Deliver( PS_Unfield( line.c_str() + b + 1 ).c_str() );
		}
	}
	psv.seq = Q_max( psv.seq, last );
}

// the session gone at C1B3R (restarted, or let go): a new one
static void PS_Lost( void ) {
	psv.on = qfalse;
	psv.key[0] = '\0';
	psv.linked = psv.approved = psv.sends = qfalse;
}

/*
===============================================================================

The RPMod account, and linking this game in Discord

===============================================================================
*/

// the account read from RPMod now and then: as we come in and when we log in or out
static void PS_AccountFrame( void ) {
	const char *account;
	int state, answerTime;

	if ( PS_LocalTest() ) {
		Q_strncpyz( psa.account, "local", sizeof( psa.account ) );
		psa.known = qtrue;
		return;
	}
	if ( cls.realtime - ps.joinTime < PS_ACCOUNT_WAIT_MS )
		return;
	state = CL_Feedback_Account( &account, &answerTime );
	if ( state == 1 || state == 0 ) {
		Q_strncpyz( psa.account, state == 1 ? account : "", sizeof( psa.account ) );
		psa.known = qtrue;
		return;
	}
	psa.known = qfalse;
	// not asked yet, or the server didn't answer a while ago: rpinfo, its reply kept out of the console
	if ( state == -1 || ( state == -3 && cls.realtime - answerTime >= PS_ACCOUNT_RETRY_MS ) )
		CL_Feedback_AskAccount();
}

static void PS_LinkFrame( void ) {
	std::string body;

	// /c1b3r link, once the account's known
	if ( psa.wanted && psa.known && psLinkHttp.state != HTTP_BUSY ) {
		psa.wanted = qfalse;
		if ( !psa.account[0] ) {
			Com_Printf( S_COLOR_YELLOW "C1B3R: log in to your RPMod account first, then /c1b3r link\n" );
		} else {
			body = "{\"key\":";
			PS_Json( body, CL_Feedback_Key() );
			body += ",\"account\":";
			PS_Json( body, psa.account );
			body += ",\"name\":";
			PS_Json( body, PS_MyName() );
			body += "}";
			if ( !PS_Send( &psLinkHttp, "/v2/link", body ) )
				Com_Printf( S_COLOR_YELLOW "C1B3R: couldn't ask for a code; try again in a moment\n" );
		}
	}
	// the code shown: asked about until it's used in Discord, or runs out
	if ( !psa.code[0] || psWaitHttp.state == HTTP_BUSY || cls.realtime < psa.waitNext )
		return;
	if ( cls.realtime >= psa.codeUntil ) {
		psa.code[0] = '\0';
		Com_Printf( S_COLOR_YELLOW "C1B3R: the code ran out; /c1b3r link for another\n" );
		return;
	}
	body = "{\"key\":";
	PS_Json( body, CL_Feedback_Key() );
	body += ",\"code\":";
	PS_Json( body, psa.code );
	body += "}";
	PS_Send( &psWaitHttp, "/v2/link/wait", body );
	psa.waitNext = cls.realtime + PS_LINK_WAIT_MS;
}

static void PS_Unlink( void ) {
	std::string body = "{\"key\":";

	if ( !PS_Token()[0] ) {
		Com_Printf( "C1B3R: this game isn't linked\n" );
		return;
	}
	PS_Json( body, CL_Feedback_Key() );
	body += ",\"token\":";
	PS_Json( body, PS_Token() );
	body += "}";
	PS_Send( &psByeHttp, "/v2/unlink", body );
	Cvar_Set( "cl_c1b3rToken", "" );
	psv.key[0] = '\0';	// a hello without it
	psv.linked = psv.approved = psv.sends = qfalse;
	Com_Printf( "C1B3R: unlinked; this game receives only\n" );
}

/*
===============================================================================

Hooks going out

===============================================================================
*/

static void PS_Note( const char *why ) {
	if ( ps.noteTime && cls.realtime - ps.noteTime < PS_NOTE_MS )
		return;
	ps.noteTime = cls.realtime;
	Com_Printf( S_COLOR_YELLOW "Hooks: shown only here, %s\n", why );
}

static void PS_RelayFrame( void ) {
	std::string body = "{\"session\":";
	psHook_t h;

	if ( !psv.on || psHookHttp.state == HTTP_BUSY || psRelayOut.empty() )
		return;
	h = psRelayOut.front();
	PS_Json( body, psv.id );
	body += ",\"text\":";
	PS_Json( body, h.text.c_str() );
	if ( h.to >= 0 )
		body += va( ",\"to\":[%i]", h.to );
	PS_MapFields( body, h.serverId );
	body += "}";
	if ( PS_Send( &psHookHttp, "/v2/hook", body ) )
		psRelayOut.pop_front();
}

// c1b3rhook <who> cmd:..., or rpnotify <who> <seconds> cmd:...: the hook, and who it's for (-1 everyone);
// NULL if it isn't one
static const char *PS_HookCommand( const char *cmd, int *to ) {
	char who[32];
	const char *p = cmd;
	int skip = 2;		// rpnotify's who and seconds, from before

	// c1b3rhook <all|client> cmd:..., as the tools write their hooks; or the rpnotify that carried them
	// before C1B3R (binds, stories): never sent to the server either way
	if ( !Q_stricmpn( p, "c1b3rhook ", 10 ) ) {
		p += 10;
		skip = 1;
	} else if ( !Q_stricmpn( p, "rpnotify ", 9 ) ) {
		p += 9;
	} else {
		return NULL;
	}
	while ( *p == ' ' )
		p++;
	Q_strncpyz( who, p, sizeof( who ) );
	for ( char *c = who; *c; c++ ) {
		if ( *c == ' ' ) {
			*c = '\0';
			break;
		}
	}
	// past who (and the seconds)
	for ( int i = 0; i < skip; i++ ) {
		while ( *p && *p != ' ' )
			p++;
		while ( *p == ' ' )
			p++;
	}
	if ( Q_stricmpn( p, "cmd:", 4 ) )
		return NULL;
	*to = Q_isanumber( who ) && atoi( who ) >= 0 ? atoi( who ) : -1;
	return p;
}

// the tools' hooks, which change what everyone sees: RPMod admins' only (C1B3R's relay.py ADMIN_HOOKS)
static qboolean PS_AdminHook( const char *text ) {
	static const char *tools[] = { "map", "combat", "story", "cine", "cin", "cinematic", "anim", "startcinematic", "endcinematic",
		"objective", "objectives", "npc", "npcnames", "sky", "dismember" };
	char kind[32];
	int n = 0;

	for ( const char *p = text + 4; *p && *p != '/' && *p != ':' && n < (int)sizeof( kind ) - 1; p++ )
		kind[n++] = *p;
	kind[n] = '\0';
	for ( size_t i = 0; i < ARRAY_LEN( tools ); i++ ) {
		if ( !Q_stricmp( kind, tools[i] ) )
			return qtrue;
	}
	return qfalse;
}

// for the datapad's apps (cl_pazaak.cpp): the session, and requests on it
// a player on this server whose game is linked to C1B3R, as its list last said
// the hook run now is the server's state replayed to someone new (a cut already made, not made now)
qboolean CL_Presence_Replaying( void ) {
	return psReplaying;
}

// this game is linked to a Discord account (/c1b3r link done: waiting for approval, or approved)
qboolean CL_Presence_GameLinked( void ) {
	return (qboolean)( PS_Token()[0] != '\0' );
}

qboolean CL_Presence_Linked( int client ) {
	for ( int i = 0; i < ps.numPlayers; i++ ) {
		if ( ps.players[i].num == client )
			return ps.players[i].linked;
	}
	return qfalse;
}

const char *CL_Presence_Session( void ) {
	return psv.on && PS_Active() ? psv.id : NULL;
}

qboolean CL_Presence_Post( httpRequest_t *req, const char *path, const std::string &body ) {
	return PS_Send( req, path, body );
}

void CL_Presence_Json( std::string &out, const char *s ) {
	PS_Json( out, s );
}

std::string CL_Presence_Unfield( const char *in ) {
	return PS_Unfield( in );
}

// from CL_AddReliableCommand: qtrue for a hook, taken here and never sent to the server
qboolean CL_Presence_Outgoing( const char *cmd ) {
	int to;
	const char *text = PS_HookCommand( cmd, &to );

	if ( !text )
		return qfalse;
	CL_Presence_Hook( to, text );
	return qtrue;
}

// a hook (cmd:...) to everyone with this client here (to -1) or one player, through C1B3R; shown here
// too. The tools' only from an RPMod admin: not even here otherwise, as it goes nowhere else
qboolean CL_Presence_Hook( int to, const char *text ) {
	if ( !text || Q_stricmpn( text, "cmd:", 4 ) )
		return qfalse;
	// (with no C1B3R for this server, a local one, it runs here as ever: it can't reach anyone else)
	if ( PS_AdminHook( text ) && to != clc.clientNum && PS_Active() && !( psv.on && psv.sends && psv.admin ) ) {
		if ( !ps.noteTime || cls.realtime - ps.noteTime >= PS_NOTE_MS ) {
			ps.noteTime = cls.realtime;
			Com_Printf( S_COLOR_YELLOW "%s\n", !psv.on || !psv.sends ? "Not done: the tools go through C1B3R, for RPMod admins whose game is linked (/c1b3r)"
				: "Not done: the tools are for RPMod admins (RPMod's word for your account)" );
		}
		return qfalse;
	}
	// run here, as everyone else gets it
	if ( to < 0 || to == clc.clientNum )
		CL_RpHooks_Deliver( text );
	if ( to == clc.clientNum || !PS_Active() )
		return qtrue;
	if ( !psv.on ) {
		PS_Note( "not sent: not connected to C1B3R" );
		return qtrue;
	}
	if ( !psv.sends ) {
		PS_Note( psv.approved && CL_RpAdmin_OnTemple() && psa.account[0] ? "not sent: on Temple only Department Head, Council and God send"
			: psv.approved ? "not sent: log in to the RPMod account you linked"
			: psv.linked ? "not sent: staff haven't approved your link in Discord yet"
			: "not sent: sending is for admins whose game is linked (/c1b3r link)" );
		return qtrue;
	}
	if ( (int)psRelayOut.size() >= PS_RELAY_MAX ) {
		PS_Note( "not sent: too many waiting to go to C1B3R" );
		return qtrue;
	}
	psRelayOut.push_back( psHook_t{ text, to, cl.serverId } );
	return qtrue;
}

/*
===============================================================================

Each frame

===============================================================================
*/

// C1B3R's answers
static void PS_PollRequests( void ) {
	if ( HTTP_Poll( &psHelloHttp ) ) {
		char *field[7];
		const int n = PS_Fields( psHelloHttp.answer, field, 7 );

		psv.helloStatus = psHelloHttp.status;
		Com_DPrintf( "clientusers: C1B3R hello %i\n", psHelloHttp.status );
		if ( psHelloHttp.status == 200 && n >= 6 && !strcmp( field[0], "ok" ) && !Q_stricmpn( psv.asked, ps.server, strlen( ps.server ) ) ) {
			psv.on = qtrue;
			Q_strncpyz( psv.id, field[1], sizeof( psv.id ) );
			psv.seq = atoi( field[2] );
			PS_SetState( field[3], field[4], field[5], n >= 7 ? field[6] : NULL );
			// a token C1B3R no longer knows (unlinked in Discord, denied): forgotten
			if ( PS_Token()[0] && !psv.linked ) {
				Com_Printf( S_COLOR_YELLOW "C1B3R: " S_COLOR_WHITE "this game's link was ended (denied, revoked or unlinked); /c1b3r link to link it again\n" );
				Cvar_Set( "cl_c1b3rToken", "" );
				Q_strncpyz( psv.asked, va( "%s|%i|%s||%s|%s|%s", ps.server, clc.clientNum, PS_MyName(), psa.account, PS_Rank(),
					Cvar_VariableString( "saber1" ) ), sizeof( psv.asked ) );
			}
			Q_strncpyz( psv.key, psv.asked, sizeof( psv.key ) );
			psv.pollNext = 0;
			ps.askNext = 0;
			PS_AskList();
		} else {
			psv.helloNext = cls.realtime + PS_HELLO_RETRY_MS;
		}
	}
	if ( HTTP_Poll( &psPollHttp ) ) {
		if ( !psv.on || strcmp( psv.pollId, psv.id ) ) {
			// for a session replaced since (a new map, a link): the new one polls now
			psv.pollNext = 0;
		} else if ( psPollHttp.status == 200 ) {
			PS_ReadPoll( psPollHttp.answer );
			psv.pollNext = 0;
		} else if ( psPollHttp.status == 404 ) {
			PS_Lost();
		} else {
			psv.pollNext = cls.realtime + PS_POLL_RETRY_MS;
		}
	}
	if ( HTTP_Poll( &psHookHttp ) && psHookHttp.status != 200 ) {
		Com_DPrintf( "clientusers: C1B3R didn't take a hook (%i)\n", psHookHttp.status );
		if ( psHookHttp.status == 403 ) {
			// the tools' refused (no RPMod admin rank), or all of it (the link taken back): the next
			// poll's state says which
			PS_Note( "not sent: C1B3R refused it (/c1b3r shows why)" );
		} else if ( psHookHttp.status == 404 ) {
			PS_Lost();
		}
	}
	// C1B3R's web pages, signed in as this game's Discord user
	if ( HTTP_Poll( &psWebHttp ) ) {
		char *field[2];
		const int n = PS_Fields( psWebHttp.answer, field, 2 );

		if ( psWebHttp.status == 200 && n == 2 && !strcmp( field[0], "ok" ) ) {
			if ( Sys_OpenPath( field[1] ) )
				Com_Printf( S_COLOR_GREEN "C1B3R: " S_COLOR_WHITE "your browser opened C1B3R's web pages, signed in\n" );
			else
				Com_Printf( "C1B3R: couldn't open a browser; type /web in Discord for a link\n" );
		} else {
			Com_Printf( S_COLOR_YELLOW "C1B3R: no web link (%i): %s\n", psWebHttp.status, psWebHttp.answer );
		}
	}
	if ( HTTP_Poll( &psLinkHttp ) ) {
		char *field[3];
		const int n = PS_Fields( psLinkHttp.answer, field, 3 );

		if ( psLinkHttp.status == 200 && n == 3 && !strcmp( field[0], "ok" ) ) {
			Q_strncpyz( psa.code, field[1], sizeof( psa.code ) );
			psa.codeUntil = cls.realtime + atoi( field[2] ) * 1000;
			psa.waitNext = cls.realtime + PS_LINK_WAIT_MS;
			// the browser: log in with Discord there, nothing to type; /link <code> in Discord otherwise
			{
				const char *page = PS_Url( va( "/link?code=%s", psa.code ) );
				const qboolean opened = (qboolean)( ( !Q_stricmpn( page, "https://", 8 ) || !Q_stricmpn( page, "http://", 7 ) ) && Sys_OpenPath( page ) );

				if ( opened )
					Com_Printf( S_COLOR_GREEN "C1B3R: " S_COLOR_WHITE "your browser opened: press " S_COLOR_CYAN "Continue with Discord" S_COLOR_WHITE
						" there; staff then approve it\n" );
				else
					Com_Printf( S_COLOR_GREEN "C1B3R: " S_COLOR_WHITE "open %s in your browser and log in with Discord; staff then approve it\n", page );
				Com_Printf( "C1B3R: or, in Discord, type " S_COLOR_CYAN "/link %s" S_COLOR_WHITE " within %i minutes\n", psa.code, atoi( field[2] ) / 60 );
			}
		} else {
			Com_Printf( S_COLOR_YELLOW "C1B3R: no code (%i): %s\n", psLinkHttp.status, psLinkHttp.answer );
		}
	}
	if ( HTTP_Poll( &psWaitHttp ) && psa.code[0] ) {
		char *field[2];
		const int n = PS_Fields( psWaitHttp.answer, field, 2 );

		if ( psWaitHttp.status == 200 && n == 2 && !strcmp( field[0], "ok" ) && strlen( field[1] ) == 32 ) {
			// kept in the .cfg: every session from now on carries it
			Cvar_Set( "cl_c1b3rToken", field[1] );
			psa.code[0] = '\0';
			psv.key[0] = '\0';
			Com_Printf( S_COLOR_GREEN "C1B3R: " S_COLOR_WHITE "linked with your Discord; you'll send once staff approve it there\n" );
		} else if ( psWaitHttp.status == 404 ) {
			psa.code[0] = '\0';
			Com_Printf( S_COLOR_YELLOW "C1B3R: the code ran out; /c1b3r link for another\n" );
		}
	}
	if ( HTTP_Poll( &psListHttp ) ) {
		if ( psListHttp.status == 200 ) {
			PS_ReadList( psListHttp.answer );
		} else {
			Com_DPrintf( "clientusers: C1B3R didn't say who has the client (%i)\n", psListHttp.status );
			ps.askNext = cls.realtime + PS_RETRY_MS;
		}
		if ( ps.print ) {
			ps.print = qfalse;
			PS_PrintList();
		}
	}
	HTTP_Poll( &psByeHttp );
}

// everything about the server forgotten: a new one, or none
static void PS_Reset( void ) {
	psRelayOut.clear();
	ps.known = qfalse;
	ps.numPlayers = 0;
	ps.askNext = 0;
	psa.known = qfalse;
	psa.account[0] = '\0';
}

// /c1b3r state's answer: what C1B3R keeps of this server, and who changed it
static void PS_StateAnswer( void ) {
	std::string all;
	size_t at = 0;
	int n = 0;

	if ( !HTTP_Poll( &psStateHttp ) )
		return;
	if ( psStateHttp.status != 200 ) {
		Com_Printf( "C1B3R: %s", psStateHttp.answer[0] ? psStateHttp.answer : "no answer\n" );
		return;
	}
	all = psStateHttp.answer;
	if ( !all.compare( 0, 3, "ok\t" ) ) {
		Com_Printf( "C1B3R: the server's shared state is cleared (%s kinds taken back for everyone)\n", all.c_str() + 3 );
		return;
	}
	while ( at < all.size() ) {
		size_t end = all.find( '\n', at );
		char line[1024], *f[6];

		if ( end == std::string::npos )
			end = all.size();
		Q_strncpyz( line, all.substr( at, end - at ).c_str(), sizeof( line ) );
		at = end + 1;
		if ( strncmp( line, "entry\t", 6 ) || PS_Fields( line, f, 6 ) < 6 )
			continue;
		{
			const time_t when = (time_t)atoi( f[4] );
			const struct tm *t = localtime( &when );

			Com_Printf( "%02i:%02i  %-8s %s" S_COLOR_WHITE " (%s)  %s\n", t ? t->tm_hour : 0, t ? t->tm_min : 0, f[1],
				PS_Unfield( f[3] ).c_str(), f[2], PS_Unfield( f[5] ).c_str() );
		}
		n++;
	}
	Com_Printf( n ? "C1B3R: %i shown to everyone with the client here, and to who joins later\n"
		: "C1B3R: nothing shared on this server now\n", n );
}

void CL_Presence_Frame( void ) {
	const char *server;

	if ( !cl_presenceUrl )
		return;
	PS_PollRequests();
	PS_StateAnswer();
	if ( !PS_Active() ) {
		if ( ps.server[0] )
			PS_Reset();
		ps.server[0] = '\0';
		return;
	}
	server = NET_AdrToString( &clc.serverAddress );
	if ( Q_stricmp( server, ps.server ) ) {
		PS_Reset();
		Q_strncpyz( ps.server, server, sizeof( ps.server ) );
		ps.joinTime = cls.realtime;
		// a session is for one server: another, made when it's known
		PS_Lost();
		psv.helloNext = 0;
	}
	PS_AccountFrame();
	// the session waits for the account the first time, so it says it at once
	if ( psa.known || psv.on || cls.realtime - ps.joinTime >= PS_ACCOUNT_WAIT_MS * 3 )
		PS_Hello();
	PS_Poll();
	PS_LinkFrame();
	PS_RelayFrame();
}

// leaving the server: the session ended, rather than left to run out
void CL_Presence_Disconnect( void ) {
	if ( psv.on && cl_presenceUrl && cl_presenceUrl->string[0] ) {
		std::string bye = "{\"session\":";

		PS_Json( bye, psv.id );
		bye += "}";
		PS_Send( &psByeHttp, "/v2/bye", bye );
	}
	PS_Lost();
	PS_Reset();
	ps.server[0] = '\0';
}

static void PS_ClientUsers_f( void ) {
	if ( !PS_Active() ) {
		Com_Printf( "clientusers: only on a server, with cl_c1b3rUrl set\n" );
		return;
	}
	if ( !psv.on ) {
		Com_Printf( "C1B3R: %s\n", PS_State() );
		return;
	}
	ps.print = qtrue;
	ps.askNext = 0;
	if ( psListHttp.state == HTTP_BUSY )
		return;
	PS_AskList();
}

// /c1b3r: your link; /c1b3r link: link this game in Discord; /c1b3r unlink
// c1b3rhook <all|client> cmd:...: typed, bound or a story's command run here
static void PS_HookCmd_f( void ) {
	const char *who = Cmd_Argv( 1 );
	const char *text = Cmd_ArgsFrom( 2 );

	if ( Cmd_Argc() < 3 || Q_stricmpn( text, "cmd:", 4 ) ) {
		Com_Printf( "usage: c1b3rhook <all|client number> cmd:<kind>/...\n" );
		return;
	}
	CL_Presence_Hook( Q_isanumber( who ) && atoi( who ) >= 0 ? atoi( who ) : -1, text );
}

static void PS_C1b3r_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !Q_stricmp( arg, "link" ) ) {
		if ( !PS_Active() || !psv.on ) {
			Com_Printf( "C1B3R: join a server first, connected to C1B3R\n" );
			return;
		}
		psa.wanted = qtrue;
		if ( !psa.known )
			Com_Printf( "C1B3R: checking your RPMod account...\n" );
		return;
	}
	if ( !Q_stricmp( arg, "unlink" ) ) {
		PS_Unlink();
		return;
	}
	// for RPMod admins: what C1B3R keeps of this server for who joins later, or all of it taken back
	if ( !Q_stricmp( arg, "state" ) ) {
		std::string body = "{\"session\":";

		if ( !PS_Active() || !psv.on ) {
			Com_Printf( "C1B3R: join a server first, connected to C1B3R\n" );
			return;
		}
		if ( psStateHttp.state == HTTP_BUSY )
			return;
		PS_Json( body, psv.id );
		body += "}";
		PS_Send( &psStateHttp, !Q_stricmp( Cmd_Argv( 2 ), "clear" ) ? "/v2/state/clear" : "/v2/state/list", body );
		return;
	}
	// C1B3R's web pages in the browser, signed in: this game's link stands for its Discord user
	if ( !Q_stricmp( arg, "web" ) ) {
		std::string body = "{\"session\":";

		if ( !PS_Active() || !psv.on ) {
			Com_Printf( "C1B3R: join a server first, connected to C1B3R\n" );
			return;
		}
		if ( !PS_Token()[0] ) {
			Com_Printf( "C1B3R: this game isn't linked: /c1b3r link first\n" );
			return;
		}
		if ( psWebHttp.state == HTTP_BUSY )
			return;
		PS_Json( body, psv.id );
		body += "}";
		PS_Send( &psWebHttp, "/v2/web/link", body );
		return;
	}
	// Discord's page to add C1B3R to your account, so /link works in a DM without its server
	if ( !Q_stricmp( arg, "discord" ) ) {
		const char *url = PS_Url( "/discord" );

		if ( Q_stricmpn( url, "https://", 8 ) && Q_stricmpn( url, "http://", 7 ) ) {
			Com_Printf( "C1B3R: cl_c1b3rUrl isn't a web address\n" );
		} else if ( !Sys_OpenPath( url ) ) {
			Com_Printf( "C1B3R: couldn't open a browser; go to %s\n", url );
		} else {
			Com_Printf( "C1B3R: opened %s\n", url );
		}
		return;
	}
	Com_Printf( "C1B3R: %s\n", PS_Active() ? PS_State() : "not on a server" );
	Com_Printf( "This game: %s\n", PS_Token()[0] ? "linked in Discord" : "not linked" );
	if ( psa.code[0] )
		Com_Printf( "Waiting for " S_COLOR_CYAN "/link %s" S_COLOR_WHITE " in Discord\n", psa.code );
	Com_Printf( "RPMod account: %s\n", !psa.known ? "not known yet" : psa.account[0] ? psa.account : "logged out" );
	Com_Printf( "c1b3r link : link this game (logged in to your RPMod account), then /link <code> in Discord\n"
		"c1b3r web : open C1B3R's web pages in your browser, signed in (a linked game)\n"
		"c1b3r discord : add C1B3R to your Discord account, for /link in any DM\n"
		"c1b3r unlink : end the link\n"
		"c1b3r state [clear] : (RPMod admins) what's shared on this server for who joins later, and who did it; clear takes it back\n" );
}

// C1B3R's one address, for everything that talks to it. It was two before (cl_presenceUrl and
// cl_feedbackUrl): one of those set to an address of its own is carried over the first time
cvar_t *CL_C1b3rUrl( void ) {
	static cvar_t *url;

	if ( !url ) {
		const qboolean fresh = (qboolean)( Cvar_Flags( "cl_c1b3rUrl" ) == CVAR_NONEXISTENT );

		url = Cvar_Get( "cl_c1b3rUrl", PS_DEFAULT_URL, CVAR_ARCHIVE_ND | CVAR_PROTECTED,
			"C1B3R's address: the link to everyone with this client on the server, what the tools share, /feedback, the datapad's apps" );
		if ( fresh ) {
			const char *olds[] = { "cl_presenceUrl", "cl_feedbackUrl" };

			for ( int i = 0; i < (int)ARRAY_LEN( olds ); i++ ) {
				const char *old = Cvar_VariableString( olds[i] );

				if ( old[0] && Q_stricmp( old, PS_DEFAULT_URL ) ) {
					Cvar_Set( "cl_c1b3rUrl", old );
					break;
				}
			}
		}
	}
	return url;
}

void CL_Presence_Init( void ) {
	cl_presenceUrl = CL_C1b3rUrl();
	cl_presenceLocal = Cvar_Get( "cl_presenceLocal", "0", CVAR_TEMP | CVAR_INTERNAL, "Talk to C1B3R on a server of your own too, logged in as a test admin, to try it" );
	cl_c1b3rToken = Cvar_Get( "cl_c1b3rToken", "", CVAR_ARCHIVE_ND | CVAR_PROTECTED | CVAR_NORESTART,
		"This game's link to C1B3R, made with /c1b3r link: keep it to yourself" );
	Cmd_AddCommand( "clientusers", PS_ClientUsers_f, "Your link to C1B3R, and who on this server has this client" );
	Cmd_AddCommand( "c1b3rhook", PS_HookCmd_f, "c1b3rhook <all|client> cmd:<...>: a hook to everyone with this client (or one player), through C1B3R" );
	Cmd_AddCommand( "c1b3r", PS_C1b3r_f, "Your link to C1B3R; c1b3r link links this game in Discord, for admins to send; c1b3r web opens C1B3R's web pages signed in; c1b3r discord adds C1B3R to your Discord account; c1b3r unlink ends it" );
}

void CL_Presence_Shutdown( void ) {
	Cmd_RemoveCommand( "clientusers" );
	Cmd_RemoveCommand( "c1b3r" );
	Cmd_RemoveCommand( "c1b3rhook" );
}
