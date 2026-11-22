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

// cl_dice.cpp -- the datapad's Dice: the Dice Master's rule systems, your character's sheet in them,
// and rolls, through C1B3R (c1b3r/dice.py)
//
// RPMod admins make systems: a die, how far past the DC is critical, traits, classes (their XP by a
// base, by rank in the Order and granted, their starting points and the traits they raise) and
// actions (traits weighted into a modifier, a bonus by class, a default DC). A character joins one
// with a class and spends its XP on traits. C1B3R rolls the dice, never the game, and tells the
// server's games (cmd:dice/roll): the roll pops up in the top left as a whisper's notice does, for
// those in RPMod's chat range of the roller (the system's masters see every roll in it, wherever
// they are), and goes in the console; the Storyteller hears every one.
// A master asks a player for a roll (cmd:dice/ask): a notice, and Roll on the Dice app's banner or
// /dicemaster roll. The Storyteller's Ask for a roll step asks through CL_Dice_Ask.

#include "cl_datapad.h"
#include "cl_games.h"
#include "cl_http.h"
#include "cl_presence.h"
#include "cl_widgets.h"
#include "keys.h"

#include <string>
#include <vector>

#define DS_NAME			33
#define DS_DESC			301
#define DS_MAX_ITEMS	32
#define DS_ROW			18.0f
#define DS_LOG			8
#define DS_ST_ASKS		8
#define DS_MSG_MS		6000

typedef enum {
	DS_LIST,			// the systems
	DS_SHEET,			// a system: your sheet, or its classes to join
	DS_EDIT,			// a system's rules, for its masters
	DS_ITEM,			// a trait, class or action of it
	DS_PLAYERS			// the characters in it, for its masters
} dsPage_t;

typedef enum {
	DT_RULES,
	DT_TRAITS,
	DT_CLASSES,
	DT_ACTIONS,
	DT_NUM
} dsTab_t;

typedef enum {
	DO_NONE,
	DO_JOIN,
	DO_SPEND,
	DO_SAVE,
	DO_DELETE,
	DO_GRANT,
	DO_LOCK,
	DO_ROLL,
	DO_ASK
} dsDoing_t;

typedef struct {
	std::string	name;
	int			n;
} dsNum_t;

typedef struct {
	std::string	name;
	float		w;
} dsWeight_t;

typedef struct {
	char		name[DS_NAME], desc[DS_DESC];
} dsTrait_t;

typedef struct {
	char					name[DS_NAME], desc[DS_DESC];
	int						base;
	std::vector<dsNum_t>	ranks, start;
	std::vector<std::string> allowed;		// none: every trait
} dsClass_t;

typedef struct {
	char					name[DS_NAME], desc[DS_DESC];
	int						dc;
	std::vector<dsWeight_t>	uses;
	std::vector<dsNum_t>	bonus;
} dsAction_t;

typedef struct {
	char					die[16];
	int						margin, natural, cost, rising, cap;
	std::vector<dsTrait_t>	traits;
	std::vector<dsClass_t>	classes;
	std::vector<dsAction_t>	actions;
} dsRules_t;

typedef struct {
	int			id;
	std::string	name, owner, myClass;
	qboolean	master;
} dsSystem_t;

typedef struct {
	std::string	name;
	int			start, bought, level, raisable;
} dsSheetTrait_t;

typedef struct {
	std::string	name, desc;
	int			mod, dc;
} dsSheetAction_t;

typedef struct {
	int			charId, pool, spent, granted, locked, client;
	std::string	name, klass;
} dsPlayer_t;

typedef struct {
	std::string	system, name, action, dice;
	int			mod, total, dc, degree;
	int			time;
} dsRoll_t;

typedef struct {
	int			id;				// C1B3R's, 0 none
	int			system;
	std::string	systemName, action, label, from;
	int			dc, time;
} dsAsk_t;

// the Storyteller's asks: its token, and C1B3R's number for it once it's answered
typedef struct {
	int			token, id;		// id 0 waiting, -1 refused
	char		error[128];
	char		system[64], action[DS_NAME], dc[16], label[96];
	int			client;
	qboolean	sent;
} dsStAsk_t;

static const char *dsDice[] = { "1d20", "1d100", "1d12", "1d10", "1d8", "1d6", "2d6", "3d6", "2d10", "4d6" };
static const float dsWeights[] = { -2.0f, -1.0f, -0.5f, 0.0f, 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f };
static const char *dsRanks[] = { "Initiate", "Padawan", "Knight", "Master" };
static const char *dsDegrees[] = { "^1Critical failure", "^3Failure", "^2Success", "^5Critical success" };

static struct {
	dsPage_t	page;
	float		scroll, content;

	// the systems
	std::vector<dsSystem_t> systems;
	qboolean	listKnown;

	// the one open: its rules, and your sheet in it
	int			sysId;
	std::string	sysName, sysOwner;
	qboolean	master;
	std::vector<std::string> masters;
	dsRules_t	rules;
	qboolean	rulesKnown;
	qboolean	sheetKnown, joined, picking;	// picking: choosing a class (again)
	std::string	klass, rank;
	int			pool, granted, locked;
	std::vector<dsSheetTrait_t> traits;
	std::vector<dsSheetAction_t> actions;
	std::vector<int> want;				// the points bought, as changed here before they're saved
	int			dc;						// a roll's DC, 0 the action's own

	// the editor
	int			editId;					// 0 a new system
	char		editName[49], editMasters[256];
	dsRules_t	edit;
	int			tab, item;
	char		itemWas[DS_NAME];		// the item's name as it was opened: renamed, what uses it goes with it
	char		newRank[DS_NAME];
	qboolean	dirty, leaving, deleting;

	// the players
	std::vector<dsPlayer_t> players;
	qboolean	playersKnown;
	int			grantXp, askAction, askDc;

	char		*focus;					// the text field typed in
	int			focusSize;

	std::vector<dsRoll_t> log;
	dsAsk_t		ask;
	dsStAsk_t	stAsks[DS_ST_ASKS];
	int			stToken, stAsking;		// stAsking: the token whose ask is out

	dsDoing_t	doing;
	char		message[160];
	int			messageTime;
	qboolean	shown;
	int			retryAt;				// a page C1B3R didn't give (not linked yet...): asked again then
} ds;

static httpRequest_t dsListReq, dsGetReq, dsSheetReq, dsDoReq, dsPlayersReq, dsAskReq;
static cvar_t *cl_diceSystem;

static void DS_Message( const char *text ) {
	Q_strncpyz( ds.message, text, sizeof( ds.message ) );
	ds.messageTime = cls.realtime;
}

/*
===============================================================================

Reading C1B3R's answers

===============================================================================
*/

// an answer's lines, each split at its tabs, fields as they were sent
static std::vector<std::vector<std::string>> DS_Lines( const char *answer ) {
	std::vector<std::vector<std::string>> out;

	for ( const char *line = answer; line && *line; ) {
		const char *end = strchr( line, '\n' );
		std::string l( line, end ? end - line : strlen( line ) );
		std::vector<std::string> f;

		for ( size_t start = 0;; ) {
			const size_t tab = l.find( '\t', start );

			f.push_back( CL_Presence_Unfield( l.substr( start, tab == std::string::npos ? std::string::npos : tab - start ).c_str() ) );
			if ( tab == std::string::npos )
				break;
			start = tab + 1;
		}
		out.push_back( f );
		line = end ? end + 1 : NULL;
	}
	return out;
}

static int DS_Int( const std::vector<std::string> &f, size_t i ) {
	return i < f.size() ? atoi( f[i].c_str() ) : 0;
}

static const char *DS_Str( const std::vector<std::string> &f, size_t i ) {
	return i < f.size() ? f[i].c_str() : "";
}

// the "ok" answer of a request done, or why not
static qboolean DS_Ok( const httpRequest_t *req, char *why, int size ) {
	const char *answer = HTTP_Answer( req );

	if ( req->status == 200 && !strncmp( answer, "ok", 2 ) )
		return qtrue;
	if ( !req->status )
		Q_strncpyz( why, "C1B3R didn't answer", size );
	else
		Q_strncpyz( why, answer, size );
	for ( char *nl = strchr( why, '\n' ); nl; nl = NULL )
		*nl = '\0';
	return qfalse;
}

static void DS_Clear( dsRules_t *r ) {
	Q_strncpyz( r->die, "1d20", sizeof( r->die ) );
	r->margin = 10;
	r->natural = 1;
	r->cost = 1;
	r->rising = 0;
	r->cap = 10;
	r->traits.clear();
	r->classes.clear();
	r->actions.clear();
}

static dsClass_t *DS_ClassNamed( dsRules_t *r, const char *name ) {
	for ( size_t i = 0; i < r->classes.size(); i++ ) {
		if ( !Q_stricmp( r->classes[i].name, name ) )
			return &r->classes[i];
	}
	return NULL;
}

static dsAction_t *DS_ActionNamed( dsRules_t *r, const char *name ) {
	for ( size_t i = 0; i < r->actions.size(); i++ ) {
		if ( !Q_stricmp( r->actions[i].name, name ) )
			return &r->actions[i];
	}
	return NULL;
}

static int *DS_NumOf( std::vector<dsNum_t> &list, const char *name, qboolean add ) {
	for ( size_t i = 0; i < list.size(); i++ ) {
		if ( !Q_stricmp( list[i].name.c_str(), name ) )
			return &list[i].n;
	}
	if ( !add )
		return NULL;
	list.push_back( { name, 0 } );
	return &list.back().n;
}

static float *DS_WeightOf( std::vector<dsWeight_t> &list, const char *name, qboolean add ) {
	for ( size_t i = 0; i < list.size(); i++ ) {
		if ( !Q_stricmp( list[i].name.c_str(), name ) )
			return &list[i].w;
	}
	if ( !add )
		return NULL;
	list.push_back( { name, 0.0f } );
	return &list.back().w;
}

static qboolean DS_Allowed( const dsClass_t *c, const char *trait ) {
	if ( c->allowed.empty() )
		return qtrue;
	for ( size_t i = 0; i < c->allowed.size(); i++ ) {
		if ( !Q_stricmp( c->allowed[i].c_str(), trait ) )
			return qtrue;
	}
	return qfalse;
}

// /v2/dice/get: the system's rules
static void DS_ReadSystem( const char *answer ) {
	const std::vector<std::vector<std::string>> lines = DS_Lines( answer );
	dsRules_t *r = &ds.rules;

	DS_Clear( r );
	ds.masters.clear();
	for ( size_t i = 0; i < lines.size(); i++ ) {
		const std::vector<std::string> &f = lines[i];
		const std::string &k = f[0];

		if ( k == "system" && f.size() >= 5 ) {
			ds.sysName = f[2];
			ds.sysOwner = f[3];
			ds.master = (qboolean)( DS_Int( f, 4 ) != 0 );
		} else if ( k == "rules" && f.size() >= 7 ) {
			Q_strncpyz( r->die, f[1].c_str(), sizeof( r->die ) );
			r->margin = DS_Int( f, 2 );
			r->natural = DS_Int( f, 3 );
			r->cost = DS_Int( f, 4 );
			r->rising = DS_Int( f, 5 );
			r->cap = DS_Int( f, 6 );
		} else if ( k == "master" && f.size() >= 2 ) {
			ds.masters.push_back( f[1] );
		} else if ( k == "trait" && f.size() >= 3 ) {
			dsTrait_t t;

			Q_strncpyz( t.name, f[1].c_str(), sizeof( t.name ) );
			Q_strncpyz( t.desc, f[2].c_str(), sizeof( t.desc ) );
			r->traits.push_back( t );
		} else if ( k == "class" && f.size() >= 5 ) {
			dsClass_t c;

			Q_strncpyz( c.name, f[1].c_str(), sizeof( c.name ) );
			Q_strncpyz( c.desc, f[2].c_str(), sizeof( c.desc ) );
			c.base = DS_Int( f, 3 );
			for ( size_t a = 0; a < f[4].size(); ) {
				const size_t comma = f[4].find( ',', a );
				const std::string t = f[4].substr( a, comma == std::string::npos ? std::string::npos : comma - a );

				if ( !t.empty() )
					c.allowed.push_back( t );
				if ( comma == std::string::npos )
					break;
				a = comma + 1;
			}
			r->classes.push_back( c );
		} else if ( ( k == "rankxp" || k == "start" ) && f.size() >= 4 ) {
			dsClass_t *c = DS_ClassNamed( r, f[1].c_str() );

			if ( c )
				*DS_NumOf( k == "rankxp" ? c->ranks : c->start, f[2].c_str(), qtrue ) = DS_Int( f, 3 );
		} else if ( k == "action" && f.size() >= 4 ) {
			dsAction_t a;

			Q_strncpyz( a.name, f[1].c_str(), sizeof( a.name ) );
			Q_strncpyz( a.desc, f[2].c_str(), sizeof( a.desc ) );
			a.dc = DS_Int( f, 3 );
			r->actions.push_back( a );
		} else if ( k == "uses" && f.size() >= 4 ) {
			dsAction_t *a = DS_ActionNamed( r, f[1].c_str() );

			if ( a )
				*DS_WeightOf( a->uses, f[2].c_str(), qtrue ) = (float)atof( f[3].c_str() );
		} else if ( k == "bonus" && f.size() >= 4 ) {
			dsAction_t *a = DS_ActionNamed( r, f[1].c_str() );

			if ( a )
				*DS_NumOf( a->bonus, f[2].c_str(), qtrue ) = DS_Int( f, 3 );
		}
	}
	ds.rulesKnown = qtrue;
}

// /v2/dice/sheet: your character's sheet, or "none" when it hasn't joined
static void DS_ReadSheet( const char *answer ) {
	const std::vector<std::vector<std::string>> lines = DS_Lines( answer );

	ds.sheetKnown = qtrue;
	ds.joined = qfalse;
	ds.traits.clear();
	ds.actions.clear();
	for ( size_t i = 0; i < lines.size(); i++ ) {
		const std::vector<std::string> &f = lines[i];

		if ( f[0] == "sheet" && f.size() >= 9 ) {
			ds.joined = qtrue;
			ds.klass = f[2];
			ds.pool = DS_Int( f, 3 );
			ds.locked = DS_Int( f, 6 );
			ds.granted = DS_Int( f, 7 );
			ds.rank = f[8];
		} else if ( f[0] == "trait" && f.size() >= 6 ) {
			ds.traits.push_back( { f[1], DS_Int( f, 2 ), DS_Int( f, 3 ), DS_Int( f, 4 ), DS_Int( f, 5 ) } );
		} else if ( f[0] == "action" && f.size() >= 5 ) {
			ds.actions.push_back( { f[1], f[4], DS_Int( f, 2 ), DS_Int( f, 3 ) } );
		}
	}
	ds.want.clear();
	for ( size_t i = 0; i < ds.traits.size(); i++ )
		ds.want.push_back( ds.traits[i].bought );
	ds.picking = (qboolean)!ds.joined;
}

/*
===============================================================================

Asking C1B3R

===============================================================================
*/

static qboolean DS_Post( httpRequest_t *req, const char *verb, const std::string &fields ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session ) {
		DS_Message( "Not connected to C1B3R" );
		return qfalse;
	}
	if ( req->state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += fields + "}";
	return CL_Presence_Post( req, va( "/v2/dice/%s", verb ), body );
}

static std::string DS_SystemField( void ) {
	return va( ",\"system\":%i", ds.sysId );
}

static void DS_AskList( void ) {
	DS_Post( &dsListReq, "systems", "" );
}

static void DS_AskSystem( void ) {
	if ( DS_Post( &dsGetReq, "get", DS_SystemField() ) )
		ds.rulesKnown = qfalse;
	DS_Post( &dsSheetReq, "sheet", DS_SystemField() );
}

static void DS_AskPlayers( void ) {
	DS_Post( &dsPlayersReq, "sheets", DS_SystemField() );
}

// a change sent: its answer read in CL_Dice_Frame
static qboolean DS_Do( dsDoing_t what, const char *verb, const std::string &fields ) {
	if ( dsDoReq.state == HTTP_BUSY ) {
		DS_Message( "A moment: still waiting for C1B3R" );
		return qfalse;
	}
	if ( !DS_Post( &dsDoReq, verb, fields ) )
		return qfalse;
	ds.doing = what;
	return qtrue;
}

static void DS_OpenSystem( int id ) {
	ds.sysId = id;
	ds.page = DS_SHEET;
	ds.scroll = 0;
	ds.rulesKnown = ds.sheetKnown = ds.joined = ds.picking = qfalse;
	ds.dc = 0;
	ds.players.clear();
	ds.playersKnown = qfalse;
	Cvar_Set( "cl_diceSystem", va( "%i", id ) );
	DS_AskSystem();
}

static void DS_Roll( const char *action, int dc, int ask ) {
	std::string fields = DS_SystemField() + ",\"action\":";

	CL_Presence_Json( fields, action );
	if ( dc )
		fields += va( ",\"dc\":%i", dc );
	if ( ask )
		fields += va( ",\"ask\":%i", ask );
	if ( DS_Do( DO_ROLL, "roll", fields ) )
		DS_Message( va( "Rolling %s...", action ) );
}

// the rules as C1B3R takes them
static std::string DS_RulesJson( const dsRules_t *r ) {
	std::string j = "{\"die\":";

	CL_Presence_Json( j, r->die );
	j += va( ",\"margin\":%i,\"natural\":%s,\"cost\":%i,\"rising\":%s,\"cap\":%i,\"traits\":[", r->margin, r->natural ? "true" : "false",
		r->cost, r->rising ? "true" : "false", r->cap );
	for ( size_t i = 0; i < r->traits.size(); i++ ) {
		j += i ? ",{\"name\":" : "{\"name\":";
		CL_Presence_Json( j, r->traits[i].name );
		j += ",\"desc\":";
		CL_Presence_Json( j, r->traits[i].desc );
		j += "}";
	}
	j += "],\"classes\":[";
	for ( size_t i = 0; i < r->classes.size(); i++ ) {
		const dsClass_t &c = r->classes[i];

		j += i ? ",{\"name\":" : "{\"name\":";
		CL_Presence_Json( j, c.name );
		j += ",\"desc\":";
		CL_Presence_Json( j, c.desc );
		j += va( ",\"base\":%i,\"ranks\":{", c.base );
		for ( size_t k = 0, n = 0; k < c.ranks.size(); k++ ) {
			if ( !c.ranks[k].n )
				continue;
			if ( n++ )
				j += ",";
			CL_Presence_Json( j, c.ranks[k].name.c_str() );
			j += va( ":%i", c.ranks[k].n );
		}
		j += "},\"start\":{";
		for ( size_t k = 0, n = 0; k < c.start.size(); k++ ) {
			if ( !c.start[k].n )
				continue;
			if ( n++ )
				j += ",";
			CL_Presence_Json( j, c.start[k].name.c_str() );
			j += va( ":%i", c.start[k].n );
		}
		j += "},\"allowed\":[";
		for ( size_t k = 0; k < c.allowed.size(); k++ ) {
			if ( k )
				j += ",";
			CL_Presence_Json( j, c.allowed[k].c_str() );
		}
		j += "]}";
	}
	j += "],\"actions\":[";
	for ( size_t i = 0; i < r->actions.size(); i++ ) {
		const dsAction_t &a = r->actions[i];

		j += i ? ",{\"name\":" : "{\"name\":";
		CL_Presence_Json( j, a.name );
		j += ",\"desc\":";
		CL_Presence_Json( j, a.desc );
		j += va( ",\"dc\":%i,\"uses\":{", a.dc );
		for ( size_t k = 0, n = 0; k < a.uses.size(); k++ ) {
			if ( a.uses[k].w == 0.0f )
				continue;
			if ( n++ )
				j += ",";
			CL_Presence_Json( j, a.uses[k].name.c_str() );
			j += va( ":%g", a.uses[k].w );
		}
		j += "},\"bonus\":{";
		for ( size_t k = 0, n = 0; k < a.bonus.size(); k++ ) {
			if ( !a.bonus[k].n )
				continue;
			if ( n++ )
				j += ",";
			CL_Presence_Json( j, a.bonus[k].name.c_str() );
			j += va( ":%i", a.bonus[k].n );
		}
		j += "}}";
	}
	return j + "]}";
}

static void DS_Save( void ) {
	std::string fields = ds.editId ? va( ",\"system\":%i,\"name\":", ds.editId ) : ",\"name\":";
	std::string masters = ",\"masters\":[";
	const char *p = ds.editMasters;
	char account[64];
	int n = 0;

	CL_Presence_Json( fields, ds.editName );
	// accounts, a comma or a space between
	while ( *p ) {
		int len = 0;

		while ( *p == ',' || *p == ' ' )
			p++;
		while ( *p && *p != ',' && *p != ' ' && len < (int)sizeof( account ) - 1 )
			account[len++] = *p++;
		account[len] = '\0';
		if ( !len )
			continue;
		if ( n++ )
			masters += ",";
		CL_Presence_Json( masters, account );
	}
	fields += masters + "],\"data\":" + DS_RulesJson( &ds.edit );
	if ( DS_Do( DO_SAVE, "save", fields ) )
		DS_Message( "Saving..." );
}

/*
===============================================================================

What C1B3R tells everyone: rolls and asks

===============================================================================
*/

static const char *DS_PlayerName( int client ) {
	const int ofs = client >= 0 && client < MAX_CLIENTS ? cl.gameState.stringOffsets[CS_PLAYERS + client] : 0;

	return ofs ? Info_ValueForKey( cl.gameState.stringData + ofs, "n" ) : "";
}

// whether RPMod's chat box would show chat from this far (its rpc_chatRange_* cvars): a roll
// heard as chat is, though it's shown as a notice
static qboolean DS_InRange( float distance ) {
	const int mode = Cvar_VariableIntegerValue( "rpc_chatRange_mode" );

	if ( mode == 3 )
		return (qboolean)( distance <= Cvar_VariableValue( "rpc_chatRange_min" ) );
	if ( mode == 2 )
		return (qboolean)( distance <= Cvar_VariableValue( "rpc_chatRange_max" ) );
	return qtrue;
}

// how far that player is from us, as RPMod's chat gives it; out of sight as far as can be
static float DS_Distance( int client ) {
	if ( client == cl.snap.ps.clientNum )
		return 0.0f;
	for ( int i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];

		if ( es->number == client )
			return Distance( cl.snap.ps.origin, es->pos.trBase );
	}
	return 100000.0f;
}

static const char *DS_Degree( int degree ) {
	return dsDegrees[Com_Clampi( 0, 3, degree )];
}

// a message's fields, | between them
static int DS_Split( char *text, char **field, int max ) {
	int n = 0;

	field[n++] = text;
	for ( char *p = text; *p && n < max; p++ ) {
		if ( *p == '|' ) {
			*p = '\0';
			field[n++] = p + 1;
		}
	}
	return n;
}

// cmd:dice/roll/<client>/<id>: <system>|<system name>|<name>|<action>|<dice>|<mod>|<total>|<dc>|<degree>|<ask>|<master>
static void DS_RollHook( int client, const char *message ) {
	char text[MAX_STRING_CHARS], *f[11], line[MAX_STRING_CHARS];
	dsRoll_t roll;
	int mod;

	Q_strncpyz( text, message, sizeof( text ) );
	if ( DS_Split( text, f, 11 ) < 11 )
		return;
	mod = atoi( f[5] );
	roll.system = f[1];
	roll.name = f[2];
	roll.action = f[3];
	roll.dice = f[4];
	roll.mod = mod;
	roll.total = atoi( f[6] );
	roll.dc = atoi( f[7] );
	roll.degree = atoi( f[8] );
	roll.time = cls.realtime;
	ds.log.insert( ds.log.begin(), roll );
	if ( ds.log.size() > DS_LOG )
		ds.log.resize( DS_LOG );

	CL_Storyteller_Roll( client, f[2], f[3], roll.total, roll.dc, roll.degree, atoi( f[9] ) );
	if ( atoi( f[9] ) && atoi( f[9] ) == ds.ask.id )
		ds.ask.id = 0;

	// a notice in the top left, as a whisper's: for whoever is in chat range of the roller, the
	// roller, and the system's masters wherever they are; in the console too
	if ( !atoi( f[10] ) && client != cl.snap.ps.clientNum && !DS_InRange( DS_Distance( client ) ) )
		return;
	Com_sprintf( line, sizeof( line ), "%s" S_COLOR_WHITE " rolls %s: " S_COLOR_YELLOW "%s" S_COLOR_WHITE " %c %i = " S_COLOR_YELLOW "%i"
		S_COLOR_WHITE " vs %i, %s", f[2], f[3], f[4], mod < 0 ? '-' : '+', abs( mod ), roll.total, roll.dc, DS_Degree( roll.degree ) );
	Com_Printf( S_COLOR_CYAN "Dice: " S_COLOR_WHITE "%s\n", line );
	if ( cls.state == CA_ACTIVE )
		CL_QueueCgameCommand( CL_Whisper_InfoNotice( line, 6000 ) );
}

// cmd:dice/ask/<client>/<ask>: <system>|<system name>|<action>|<dc>|<label>|<master's name>
static void DS_AskHook( int client, int askId, const char *message ) {
	char text[MAX_STRING_CHARS], *f[6], notice[MAX_STRING_CHARS];

	if ( client != cl.snap.ps.clientNum )
		return;
	Q_strncpyz( text, message, sizeof( text ) );
	if ( DS_Split( text, f, 6 ) < 6 )
		return;
	ds.ask.id = askId;
	ds.ask.system = atoi( f[0] );
	ds.ask.systemName = f[1];
	ds.ask.action = f[2];
	ds.ask.dc = atoi( f[3] );
	ds.ask.label = f[4];
	ds.ask.from = f[5];
	ds.ask.time = cls.realtime;
	Com_sprintf( notice, sizeof( notice ), "%s" S_COLOR_WHITE " asks you to roll %s (DC %i)%s%s", f[5], f[2], ds.ask.dc, f[4][0] ? ": " : "", f[4] );
	for ( char *q = notice; *q; q++ ) {
		if ( *q == '"' )
			*q = '\'';
	}
	Com_Printf( "%s. Roll it on the datapad's Dice, or /dicemaster roll\n", notice );
	if ( cls.state == CA_ACTIVE )
		CL_QueueCgameCommand( CL_Whisper_InfoNotice( notice, 8000 ) );
}

void CL_Dice_Hook( char **argv, int argc, const char *message ) {
	if ( argc >= 3 && !Q_stricmp( argv[0], "roll" ) )
		DS_RollHook( atoi( argv[1] ), message );
	else if ( argc >= 3 && !Q_stricmp( argv[0], "ask" ) )
		DS_AskHook( atoi( argv[1] ), atoi( argv[2] ), message );
}

// the ask answered: rolled at once, in the system it's for
static void DS_AnswerAsk( void ) {
	if ( !ds.ask.id ) {
		DS_Message( "No one has asked you for a roll" );
		return;
	}
	if ( ds.sysId != ds.ask.system ) {
		ds.sysId = ds.ask.system;
		Cvar_Set( "cl_diceSystem", va( "%i", ds.sysId ) );
	}
	DS_Roll( ds.ask.action.c_str(), 0, ds.ask.id );
}

/*
===============================================================================

The Storyteller's asks

===============================================================================
*/

int CL_Dice_Ask( const char *system, int client, const char *action, const char *dc, const char *label ) {
	dsStAsk_t *a = &ds.stAsks[ds.stToken % DS_ST_ASKS];

	if ( !CL_Presence_Session() )
		return 0;
	memset( a, 0, sizeof( *a ) );
	a->token = ++ds.stToken;
	Q_strncpyz( a->system, system, sizeof( a->system ) );
	Q_strncpyz( a->action, action, sizeof( a->action ) );
	Q_strncpyz( a->dc, dc, sizeof( a->dc ) );
	Q_strncpyz( a->label, label, sizeof( a->label ) );
	a->client = client;
	return a->token;
}

int CL_Dice_AskState( int token, const char **error ) {
	for ( int i = 0; i < DS_ST_ASKS; i++ ) {
		if ( ds.stAsks[i].token == token && token ) {
			*error = ds.stAsks[i].error;
			return ds.stAsks[i].id;
		}
	}
	*error = "the ask was forgotten";
	return -1;
}

// the next of the Storyteller's asks sent, one at a time
static void DS_SendStAsks( void ) {
	if ( dsAskReq.state == HTTP_BUSY )
		return;
	for ( int i = 0; i < DS_ST_ASKS; i++ ) {
		dsStAsk_t *a = &ds.stAsks[i];
		std::string fields = ",\"system\":";

		if ( !a->token || a->sent )
			continue;
		CL_Presence_Json( fields, a->system );
		fields += ",\"action\":";
		CL_Presence_Json( fields, a->action );
		fields += va( ",\"client\":%i", a->client );
		if ( a->dc[0] )
			fields += va( ",\"dc\":%i", atoi( a->dc ) );
		fields += ",\"label\":";
		CL_Presence_Json( fields, a->label );
		if ( DS_Post( &dsAskReq, "ask", fields ) ) {
			a->sent = qtrue;
			ds.stAsking = a->token;
		} else {
			a->sent = qtrue;
			a->id = -1;
			Q_strncpyz( a->error, "Not connected to C1B3R", sizeof( a->error ) );
		}
		return;
	}
}

/*
===============================================================================

The frame: answers read

===============================================================================
*/

void CL_Dice_Frame( void ) {
	char why[160];

	if ( HTTP_Poll( &dsListReq ) ) {
		if ( DS_Ok( &dsListReq, why, sizeof( why ) ) ) {
			const std::vector<std::vector<std::string>> lines = DS_Lines( HTTP_Answer( &dsListReq ) );

			ds.systems.clear();
			for ( size_t i = 0; i < lines.size(); i++ ) {
				if ( lines[i][0] == "system" && lines[i].size() >= 6 )
					ds.systems.push_back( { DS_Int( lines[i], 1 ), lines[i][2], lines[i][3], lines[i][5], (qboolean)( DS_Int( lines[i], 4 ) != 0 ) } );
			}
			ds.listKnown = qtrue;
		} else {
			DS_Message( why );
		}
	}
	if ( HTTP_Poll( &dsGetReq ) ) {
		if ( DS_Ok( &dsGetReq, why, sizeof( why ) ) )
			DS_ReadSystem( HTTP_Answer( &dsGetReq ) );
		else
			DS_Message( why );
	}
	if ( HTTP_Poll( &dsSheetReq ) ) {
		if ( DS_Ok( &dsSheetReq, why, sizeof( why ) ) )
			DS_ReadSheet( HTTP_Answer( &dsSheetReq ) );
		else
			DS_Message( why );
	}
	if ( HTTP_Poll( &dsPlayersReq ) ) {
		if ( DS_Ok( &dsPlayersReq, why, sizeof( why ) ) ) {
			const std::vector<std::vector<std::string>> lines = DS_Lines( HTTP_Answer( &dsPlayersReq ) );

			ds.players.clear();
			for ( size_t i = 0; i < lines.size(); i++ ) {
				const std::vector<std::string> &f = lines[i];

				// char <id> <name> <class> <pool> <spent> <granted> <locked> <client here, -1>
				if ( f[0] == "char" && f.size() >= 9 )
					ds.players.push_back( { DS_Int( f, 1 ), DS_Int( f, 4 ), DS_Int( f, 5 ), DS_Int( f, 6 ), DS_Int( f, 7 ), DS_Int( f, 8 ), f[2], f[3] } );
			}
			ds.playersKnown = qtrue;
		} else {
			DS_Message( why );
		}
	}
	if ( HTTP_Poll( &dsDoReq ) ) {
		const dsDoing_t doing = ds.doing;

		ds.doing = DO_NONE;
		if ( !DS_Ok( &dsDoReq, why, sizeof( why ) ) ) {
			DS_Message( why );
		} else if ( doing == DO_SAVE ) {
			const char *tab = strchr( HTTP_Answer( &dsDoReq ), '\t' );

			DS_Message( "Saved" );
			ds.dirty = qfalse;
			ds.editId = tab ? atoi( tab + 1 ) : ds.editId;
			ds.sysId = ds.editId;
			DS_AskSystem();
			DS_AskList();
		} else if ( doing == DO_DELETE ) {
			DS_Message( "Deleted" );
			ds.page = DS_LIST;
			ds.sysId = 0;
			DS_AskList();
		} else if ( doing == DO_JOIN || doing == DO_SPEND ) {
			DS_Message( doing == DO_JOIN ? "Joined" : "Saved" );
			DS_Post( &dsSheetReq, "sheet", DS_SystemField() );
			DS_AskList();
		} else if ( doing == DO_GRANT || doing == DO_LOCK ) {
			DS_AskPlayers();
		} else if ( doing == DO_ROLL ) {
			ds.message[0] = '\0';		// it shows in chat
		}
	}
	if ( HTTP_Poll( &dsAskReq ) ) {
		for ( int i = 0; i < DS_ST_ASKS; i++ ) {
			dsStAsk_t *a = &ds.stAsks[i];

			if ( a->token != ds.stAsking || !a->token )
				continue;
			if ( DS_Ok( &dsAskReq, a->error, sizeof( a->error ) ) ) {
				const char *tab = strchr( HTTP_Answer( &dsAskReq ), '\t' );

				a->id = tab ? atoi( tab + 1 ) : -1;
			} else {
				a->id = -1;
			}
		}
		ds.stAsking = 0;
	}
	DS_SendStAsks();

	// a page still unknown (asked before the game was linked, C1B3R away): asked again now and then
	if ( ds.shown && cls.realtime >= ds.retryAt && dsListReq.state != HTTP_BUSY && dsGetReq.state != HTTP_BUSY && dsSheetReq.state != HTTP_BUSY ) {
		ds.retryAt = cls.realtime + 5000;
		if ( ds.page == DS_LIST && !ds.listKnown )
			DS_AskList();
		else if ( ds.page == DS_SHEET && ds.sysId && ( !ds.rulesKnown || !ds.sheetKnown ) )
			DS_AskSystem();
	}
}

/*
===============================================================================

Drawing: helpers

===============================================================================
*/

static float dsTop, dsBottom;

static qboolean DS_Seen( float y, float h ) {
	return (qboolean)( y >= dsTop && y + h <= dsBottom );
}

static void DS_Style( wgStyle_t *style ) {
	memset( style, 0, sizeof( *style ) );
	style->font = DP_Font();
	style->scale = 0.58f;
	style->lineH = 11;
	style->padX = 3;
	style->padY = 2;
	style->fill = dpScreen;
	style->border = dpGlow;
	style->text = dpText;
	style->dim = dpDim;
}

// a text field, typed in once clicked
static void DS_Field( float x, float y, float w, char *buf, int size, const char *placeholder ) {
	wgStyle_t style;
	float cx, cy;
	qboolean *click;
	const qboolean focused = (qboolean)( ds.focus == buf );

	if ( !DS_Seen( y, DP_CTRL_H ) ) {
		if ( focused )
			ds.focus = NULL;
		return;
	}
	DS_Style( &style );
	DP_Fill( x, y, w, DP_CTRL_H, dpScreen );
	DP_Frame( x, y, w, DP_CTRL_H, focused ? dpWhite : dpGlow );
	WG_Field( &style, x, y, w, DP_CTRL_H, buf, focused, placeholder, DP_CursorX(), DP_CursorY() );
	click = DP_Click( &cx, &cy );
	if ( *click && cx >= x && cx < x + w && cy >= y && cy < y + DP_CTRL_H ) {
		*click = qfalse;
		if ( !focused )
			WG_EditReset();
		ds.focus = buf;
		ds.focusSize = size;
		WG_FieldClick( buf, cx, cy );
	}
}

static void DS_Label( float x, float y, float w, const char *text, const float *color ) {
	if ( DS_Seen( y, 12 ) )
		DP_Text( x, y + 3, w, text, color, DP_Font(), 0.56f, qfalse );
}

static void DS_Head( float x, float y, float w, const char *text ) {
	if ( DS_Seen( y, 14 ) )
		DP_Text( x, y + 2, w, text, dpText, DP_TitleFont(), 0.5f, qfalse );
}

// label  [-] value [+]: qtrue when it changed
static qboolean DS_Number( float x, float y, float w, const char *label, int *value, int low, int high, int step, const char *zero ) {
	const float bw = DP_Wide( 18 ), vw = DP_Wide( 44 );
	const float bx = x + w - bw * 2 - vw;
	int was = *value;

	if ( !DS_Seen( y, DP_CTRL_H ) )
		return qfalse;
	DP_Text( x, y + 3, bx - x - 4, label, dpText, DP_Font(), 0.56f, qfalse );
	if ( DP_Button( bx, y, bw, "-", qfalse ) )
		*value = Q_max( low, *value - step );
	DP_Text( bx + bw, y + 3, vw, !*value && zero ? zero : va( "%i", *value ), dpWhite, DP_Font(), 0.6f, qtrue );
	if ( DP_Button( bx + bw + vw, y, bw, "+", qfalse ) )
		*value = Q_min( high, *value + step );
	return (qboolean)( *value != was );
}

static qboolean DS_Toggle( float x, float y, float w, const char *label, int *value, const char *on, const char *off ) {
	const float bw = DP_Wide( 80 );

	if ( !DS_Seen( y, DP_CTRL_H ) )
		return qfalse;
	DP_Text( x, y + 3, w - bw - 4, label, dpText, DP_Font(), 0.56f, qfalse );
	if ( DP_Button( x + w - bw, y, bw, *value ? on : off, (qboolean)( *value != 0 ) ) ) {
		*value = !*value;
		return qtrue;
	}
	return qfalse;
}

static qboolean DS_ButtonAt( float x, float y, float w, const char *label, qboolean on ) {
	return (qboolean)( DS_Seen( y, DP_CTRL_H ) && DP_Button( x, y, w, label, on ) );
}

// a line of text wrapped to w: its height
static float DS_Wrapped( float x, float y, float w, const char *text, const float *color ) {
	wgStyle_t style;
	int lines;

	DS_Style( &style );
	style.scale = 0.52f;
	style.lineH = 10;
	lines = WG_TextLines( &style, text, w );
	for ( int i = 0; i < lines; i++ ) {
		if ( DS_Seen( y + i * 10, 10 ) )
			WG_DrawText( &style, x, y + i * 10, w, text, i, 1, color );
	}
	return lines * 10.0f;
}

/*
===============================================================================

The systems

===============================================================================
*/

static void DS_NewSystem( void ) {
	ds.editId = 0;
	Q_strncpyz( ds.editName, "", sizeof( ds.editName ) );
	ds.editMasters[0] = '\0';
	DS_Clear( &ds.edit );
	ds.page = DS_EDIT;
	ds.tab = DT_RULES;
	ds.scroll = 0;
	ds.dirty = ds.leaving = ds.deleting = qfalse;
}

static void DS_EditSystem( void ) {
	std::string masters;

	ds.editId = ds.sysId;
	Q_strncpyz( ds.editName, ds.sysName.c_str(), sizeof( ds.editName ) );
	for ( size_t i = 0; i < ds.masters.size(); i++ )
		masters += ( i ? ", " : "" ) + ds.masters[i];
	Q_strncpyz( ds.editMasters, masters.c_str(), sizeof( ds.editMasters ) );
	ds.edit = ds.rules;
	ds.page = DS_EDIT;
	ds.tab = DT_RULES;
	ds.scroll = 0;
	ds.dirty = ds.leaving = ds.deleting = qfalse;
}

static float DS_DrawList( float x, float y, float w ) {
	const char *rank = CL_RpAdmin_Rank();

	if ( rank && rank[0] && DS_ButtonAt( x, y, w, "New system", qfalse ) ) {
		DS_NewSystem();
		return y;
	}
	if ( rank && rank[0] )
		y += DP_CTRL_H + 8;
	if ( !ds.listKnown ) {
		DS_Label( x, y, w, !CL_Presence_Session() ? "Not connected to C1B3R" : "Asking C1B3R...", dpDim );
		return y + 14;
	}
	if ( ds.systems.empty() ) {
		DS_Label( x, y, w, "No dice systems yet: RPMod admins make them", dpDim );
		return y + 14;
	}
	for ( size_t i = 0; i < ds.systems.size(); i++ ) {
		const dsSystem_t &sy = ds.systems[i];
		const float h = 34.0f;

		if ( DS_Seen( y, h ) ) {
			DP_Fill( x, y, w, h, DP_InRect( x, y, w, h ) ? dpButtonHover : dpTile );
			DP_Frame( x, y, w, h, dpGlow );
			DP_Text( x + 5, y + 4, w - 10, sy.name.c_str(), dpText, DP_TitleFont(), 0.5f, qfalse );
			DP_Text( x + 5, y + 19, w - 10, va( "%s%s", sy.myClass.empty() ? "Not joined" : va( "Your class: %s", sy.myClass.c_str() ),
				sy.master ? "  -  you run it" : "" ), dpDim, DP_Font(), 0.52f, qfalse );
			if ( DP_Clicked( x, y, w, h ) ) {
				DS_OpenSystem( sy.id );
				return y;
			}
		}
		y += h + 5;
	}
	return y;
}

/*
===============================================================================

A sheet

===============================================================================
*/

static int DS_PointCost( int start, int bought ) {
	int cost = 0;

	if ( !ds.rules.rising )
		return bought * ds.rules.cost;
	for ( int level = start + 1; level <= start + bought; level++ )
		cost += level * ds.rules.cost;
	return cost;
}

static int DS_Spent( void ) {
	int spent = 0;

	for ( size_t i = 0; i < ds.traits.size() && i < ds.want.size(); i++ )
		spent += DS_PointCost( ds.traits[i].start, ds.want[i] );
	return spent;
}

// an action's modifier with the points as they are here
static int DS_Modifier( const char *action ) {
	dsAction_t *a = DS_ActionNamed( &ds.rules, action );
	float total = 0.0f;
	int *bonus;

	if ( !a )
		return 0;
	for ( size_t i = 0; i < ds.traits.size() && i < ds.want.size(); i++ ) {
		const float *w = DS_WeightOf( a->uses, ds.traits[i].name.c_str(), qfalse );

		if ( w )
			total += Q_min( ds.rules.cap, ds.traits[i].start + ds.want[i] ) * *w;
	}
	bonus = DS_NumOf( a->bonus, ds.klass.c_str(), qfalse );
	return (int)floorf( total ) + ( bonus ? *bonus : 0 );
}

static qboolean DS_Changed( void ) {
	for ( size_t i = 0; i < ds.traits.size() && i < ds.want.size(); i++ ) {
		if ( ds.want[i] != ds.traits[i].bought )
			return qtrue;
	}
	return qfalse;
}

static float DS_DrawClasses( float x, float y, float w ) {
	DS_Head( x, y, w, ds.joined ? "Pick another class (your points start again)" : "Pick a class to join" );
	y += 18;
	if ( ds.rules.classes.empty() )
		DS_Label( x, y, w, "This system has no classes yet", dpDim );
	for ( size_t i = 0; i < ds.rules.classes.size(); i++ ) {
		const dsClass_t &c = ds.rules.classes[i];
		std::string ranks;

		for ( size_t k = 0; k < c.ranks.size(); k++ )
			ranks += va( "%s%s %i", ranks.empty() ? "" : ", ", c.ranks[k].name.c_str(), c.ranks[k].n );
		if ( DS_Seen( y, 14 ) )
			DP_Text( x, y + 2, w * 0.7f, c.name, dpWhite, DP_TitleFont(), 0.48f, qfalse );
		if ( DS_ButtonAt( x + w - DP_Wide( 50 ), y, DP_Wide( 50 ), "Join", qfalse ) ) {
			std::string fields = DS_SystemField() + ",\"class\":";

			CL_Presence_Json( fields, c.name );
			DS_Do( DO_JOIN, "join", fields );
		}
		y += DP_CTRL_H + 2;
		if ( c.desc[0] )
			y += DS_Wrapped( x, y, w, c.desc, dpDim ) + 2;
		DS_Label( x, y, w, va( "%i XP%s%s", c.base, ranks.empty() ? "" : ", and by rank: ", ranks.c_str() ), dpText );
		y += 16;
	}
	if ( ds.joined && DS_ButtonAt( x, y, DP_Wide( 70 ), "Keep mine", qfalse ) )
		ds.picking = qfalse;
	return y + DP_CTRL_H + 4;
}

static float DS_DrawSheet( float x, float y, float w ) {
	const int spent = DS_Spent();
	const int left = ds.pool - spent;
	const qboolean changed = DS_Changed();

	DS_Label( x, y, w, va( "%s%s%s", ds.klass.c_str(), ds.rank.empty() ? "" : "  -  ", ds.rank.c_str() ), dpWhite );
	if ( !ds.locked && DS_ButtonAt( x + w - DP_Wide( 74 ), y, DP_Wide( 74 ), "Change class", qfalse ) )
		ds.picking = qtrue;
	y += 18;
	DS_Label( x, y, w, va( "XP: %i left of %i%s", left, ds.pool, ds.granted ? va( " (%i granted)", ds.granted ) : "" ), left < 0 ? dpWhite : dpText );
	y += 14;
	if ( ds.locked ) {
		DS_Label( x, y, w, "A master has locked your sheet", dpDim );
		y += 14;
	}
	y += 4;
	DS_Head( x, y, w, "Traits" );
	y += 18;
	for ( size_t i = 0; i < ds.traits.size() && i < ds.want.size(); i++ ) {
		dsSheetTrait_t &t = ds.traits[i];
		const int level = Q_min( ds.rules.cap, t.start + ds.want[i] );
		const float bw = DP_Wide( 18 );
		const int next = DS_PointCost( t.start, ds.want[i] + 1 ) - DS_PointCost( t.start, ds.want[i] );

		if ( DS_Seen( y, DP_CTRL_H ) ) {
			DP_Text( x, y + 3, w * 0.55f, t.name.c_str(), t.raisable ? dpText : dpDim, DP_Font(), 0.56f, qfalse );
			DP_Text( x + w * 0.55f, y + 3, DP_Wide( 30 ), va( "%i", level ), dpWhite, DP_Font(), 0.6f, qtrue );
			if ( t.raisable && !ds.locked ) {
				if ( ds.want[i] > 0 && DP_Button( x + w - bw * 2 - 4, y, bw, "-", qfalse ) )
					ds.want[i]--;
				if ( level < ds.rules.cap && DP_Button( x + w - bw, y, bw, "+", qfalse ) ) {
					if ( next > left )
						DS_Message( va( "That needs %i XP, and you have %i left", next, left ) );
					else
						ds.want[i]++;
				}
			}
		}
		y += DP_CTRL_H + 2;
	}
	if ( changed ) {
		if ( DS_ButtonAt( x, y, w * 0.48f, "Save points", qfalse ) ) {
			std::string fields = DS_SystemField() + ",\"points\":{";

			for ( size_t i = 0; i < ds.traits.size() && i < ds.want.size(); i++ ) {
				if ( i )
					fields += ",";
				CL_Presence_Json( fields, ds.traits[i].name.c_str() );
				fields += va( ":%i", ds.want[i] );
			}
			DS_Do( DO_SPEND, "spend", fields + "}" );
		}
		if ( DS_ButtonAt( x + w * 0.52f, y, w * 0.48f, "Undo", qfalse ) ) {
			for ( size_t i = 0; i < ds.traits.size() && i < ds.want.size(); i++ )
				ds.want[i] = ds.traits[i].bought;
		}
		y += DP_CTRL_H + 4;
	}
	y += 6;
	DS_Head( x, y, w, "Actions" );
	y += 18;
	DS_Number( x, y, w, "Against DC", &ds.dc, 0, 200, 1, "its own" );
	y += DP_CTRL_H + 4;
	for ( size_t i = 0; i < ds.actions.size(); i++ ) {
		const dsSheetAction_t &a = ds.actions[i];
		const int mod = DS_Modifier( a.name.c_str() );

		if ( DS_Seen( y, DP_CTRL_H ) ) {
			DP_Text( x, y + 3, w * 0.5f, a.name.c_str(), dpText, DP_Font(), 0.56f, qfalse );
			DP_Text( x + w * 0.5f, y + 3, w * 0.25f, va( "%c%i  DC %i", mod < 0 ? '-' : '+', abs( mod ), ds.dc ? ds.dc : a.dc ), dpDim, DP_Font(), 0.54f, qfalse );
			if ( DP_Button( x + w - DP_Wide( 44 ), y, DP_Wide( 44 ), "Roll", qfalse ) ) {
				if ( changed )
					DS_Message( "Save your points first" );
				else
					DS_Roll( a.name.c_str(), ds.dc, 0 );
			}
		}
		y += DP_CTRL_H + 2;
		if ( a.desc.size() && DS_Seen( y, 10 ) )
			y += DS_Wrapped( x + 6, y, w - 6, a.desc.c_str(), dpDim ) + 2;
	}
	return y;
}

static float DS_DrawLog( float x, float y, float w ) {
	if ( ds.log.empty() )
		return y;
	y += 6;
	DS_Head( x, y, w, "Rolls seen" );
	y += 18;
	for ( size_t i = 0; i < ds.log.size(); i++ ) {
		const dsRoll_t &r = ds.log[i];

		if ( DS_Seen( y, 12 ) ) {
			WG_TypedText( x, y, w * 0.4f, r.name.c_str(), dpText, DP_Font(), 0.52f, qfalse, qfalse );
			DP_Text( x + w * 0.4f, y, w * 0.6f, va( "%s %i vs %i %s", r.action.c_str(), r.total, r.dc, DS_Degree( r.degree ) ), dpDim, DP_Font(), 0.52f, qfalse );
		}
		y += 12;
	}
	return y;
}

/*
===============================================================================

The editor

===============================================================================
*/

// an item renamed: what uses it by name goes with it
static void DS_Renamed( int tab, const char *was, const char *now ) {
	if ( !was[0] || !Q_stricmp( was, now ) )
		return;
	if ( tab == DT_TRAITS ) {
		for ( size_t i = 0; i < ds.edit.classes.size(); i++ ) {
			dsClass_t &c = ds.edit.classes[i];

			for ( size_t k = 0; k < c.start.size(); k++ ) {
				if ( !Q_stricmp( c.start[k].name.c_str(), was ) )
					c.start[k].name = now;
			}
			for ( size_t k = 0; k < c.allowed.size(); k++ ) {
				if ( !Q_stricmp( c.allowed[k].c_str(), was ) )
					c.allowed[k] = now;
			}
		}
		for ( size_t i = 0; i < ds.edit.actions.size(); i++ ) {
			for ( size_t k = 0; k < ds.edit.actions[i].uses.size(); k++ ) {
				if ( !Q_stricmp( ds.edit.actions[i].uses[k].name.c_str(), was ) )
					ds.edit.actions[i].uses[k].name = now;
			}
		}
	} else if ( tab == DT_CLASSES ) {
		for ( size_t i = 0; i < ds.edit.actions.size(); i++ ) {
			for ( size_t k = 0; k < ds.edit.actions[i].bonus.size(); k++ ) {
				if ( !Q_stricmp( ds.edit.actions[i].bonus[k].name.c_str(), was ) )
					ds.edit.actions[i].bonus[k].name = now;
			}
		}
	}
}

static char *DS_ItemName( int tab, int i ) {
	return tab == DT_TRAITS ? ds.edit.traits[i].name : tab == DT_CLASSES ? ds.edit.classes[i].name : ds.edit.actions[i].name;
}

static int DS_ItemCount( int tab ) {
	return tab == DT_TRAITS ? (int)ds.edit.traits.size() : tab == DT_CLASSES ? (int)ds.edit.classes.size() : (int)ds.edit.actions.size();
}

static void DS_OpenItem( int i ) {
	ds.item = i;
	Q_strncpyz( ds.itemWas, DS_ItemName( ds.tab, i ), sizeof( ds.itemWas ) );
	ds.page = DS_ITEM;
	ds.scroll = 0;
	ds.focus = NULL;
}

static void DS_CloseItem( void ) {
	DS_Renamed( ds.tab, ds.itemWas, DS_ItemName( ds.tab, ds.item ) );
	ds.page = DS_EDIT;
	ds.scroll = 0;
	ds.focus = NULL;
}

static float DS_DrawRules( float x, float y, float w ) {
	int die = 0;

	DS_Label( x, y, w, "Name", dpText );
	y += 13;
	DS_Field( x, y, w, ds.editName, sizeof( ds.editName ), "Temple Trials" );
	y += DP_CTRL_H + 6;
	for ( int i = 0; i < (int)ARRAY_LEN( dsDice ); i++ ) {
		if ( !Q_stricmp( dsDice[i], ds.edit.die ) )
			die = i;
	}
	if ( DS_Seen( y, DP_CTRL_H ) ) {
		DP_Text( x, y + 3, w * 0.5f, "Die", dpText, DP_Font(), 0.56f, qfalse );
		if ( DP_Button( x + w - DP_Wide( 80 ), y, DP_Wide( 80 ), va( "< %s >", ds.edit.die ), qfalse ) ) {
			Q_strncpyz( ds.edit.die, dsDice[( die + 1 ) % ARRAY_LEN( dsDice )], sizeof( ds.edit.die ) );
			ds.dirty = qtrue;
		}
	}
	y += DP_CTRL_H + 4;
	ds.dirty = (qboolean)( DS_Number( x, y, w, "Critical: past the DC by", &ds.edit.margin, 1, 100, 1, NULL ) || ds.dirty );
	y += DP_CTRL_H + 4;
	ds.dirty = (qboolean)( DS_Toggle( x, y, w, "Rolling the most or 1: a degree up or down", &ds.edit.natural, "Yes", "No" ) || ds.dirty );
	y += DP_CTRL_H + 4;
	ds.dirty = (qboolean)( DS_Number( x, y, w, "XP a trait point costs", &ds.edit.cost, 1, 100, 1, NULL ) || ds.dirty );
	y += DP_CTRL_H + 4;
	ds.dirty = (qboolean)( DS_Toggle( x, y, w, "Each level dearer (level x cost)", &ds.edit.rising, "Yes", "No" ) || ds.dirty );
	y += DP_CTRL_H + 4;
	ds.dirty = (qboolean)( DS_Number( x, y, w, "Most points in a trait", &ds.edit.cap, 1, 100, 1, NULL ) || ds.dirty );
	y += DP_CTRL_H + 8;
	DS_Label( x, y, w, "Masters besides you: RPMod accounts", dpText );
	y += 13;
	DS_Field( x, y, w, ds.editMasters, sizeof( ds.editMasters ), "account, account" );
	y += DP_CTRL_H + 4;
	y += DS_Wrapped( x, y, w, "A system's masters change it, grant XP, lock sheets and ask players for rolls. Its owner alone says who they are, and deletes it.", dpDim ) + 6;
	if ( ds.editId ) {
		if ( DS_ButtonAt( x, y, DP_Wide( 90 ), ds.deleting ? "Delete: sure?" : "Delete system", ds.deleting ) ) {
			if ( ds.deleting )
				DS_Do( DO_DELETE, "delete", va( ",\"system\":%i", ds.editId ) );
			ds.deleting = (qboolean)!ds.deleting;
		}
		y += DP_CTRL_H + 4;
	}
	return y;
}

static float DS_DrawItems( float x, float y, float w ) {
	static const char *what[DT_NUM] = { "", "trait", "class", "action" };
	const int count = DS_ItemCount( ds.tab );

	for ( int i = 0; i < count; i++ ) {
		const char *name = DS_ItemName( ds.tab, i );

		if ( DS_Seen( y, DP_CTRL_H ) ) {
			if ( DP_Button( x, y, w - DP_Wide( 22 ), name[0] ? name : "(no name)", qfalse ) ) {
				DS_OpenItem( i );
				return y;
			}
			if ( DP_Button( x + w - DP_Wide( 18 ), y, DP_Wide( 18 ), "x", qfalse ) ) {
				if ( ds.tab == DT_TRAITS )
					ds.edit.traits.erase( ds.edit.traits.begin() + i );
				else if ( ds.tab == DT_CLASSES )
					ds.edit.classes.erase( ds.edit.classes.begin() + i );
				else
					ds.edit.actions.erase( ds.edit.actions.begin() + i );
				ds.dirty = qtrue;
				return y;
			}
		}
		y += DP_CTRL_H + 3;
	}
	if ( count < DS_MAX_ITEMS && DS_ButtonAt( x, y + 4, w, va( "Add a %s", what[ds.tab] ), qfalse ) ) {
		if ( ds.tab == DT_TRAITS ) {
			dsTrait_t t;

			memset( &t, 0, sizeof( t ) );
			ds.edit.traits.push_back( t );
		} else if ( ds.tab == DT_CLASSES ) {
			dsClass_t c;

			c.name[0] = c.desc[0] = '\0';
			c.base = 10;
			ds.edit.classes.push_back( c );
		} else {
			dsAction_t a;

			a.name[0] = a.desc[0] = '\0';
			a.dc = 15;
			ds.edit.actions.push_back( a );
		}
		ds.dirty = qtrue;
		DS_OpenItem( count );
		ds.focus = DS_ItemName( ds.tab, count );
		ds.focusSize = DS_NAME;
		WG_EditReset();
	}
	return y + DP_CTRL_H + 8;
}

static float DS_DrawItem( float x, float y, float w ) {
	const int tab = ds.tab;
	char *name = DS_ItemName( tab, ds.item );
	char *desc = tab == DT_TRAITS ? ds.edit.traits[ds.item].desc : tab == DT_CLASSES ? ds.edit.classes[ds.item].desc : ds.edit.actions[ds.item].desc;

	DS_Label( x, y, w, "Name", dpText );
	y += 13;
	DS_Field( x, y, w, name, DS_NAME, tab == DT_TRAITS ? "Force Control" : tab == DT_CLASSES ? "Consular" : "Telekinesis" );
	y += DP_CTRL_H + 4;
	DS_Label( x, y, w, "What it is", dpText );
	y += 13;
	DS_Field( x, y, w, desc, DS_DESC, "" );
	y += DP_CTRL_H + 8;

	if ( tab == DT_CLASSES ) {
		dsClass_t &c = ds.edit.classes[ds.item];

		ds.dirty = (qboolean)( DS_Number( x, y, w, "XP to start with", &c.base, 0, 10000, 5, NULL ) || ds.dirty );
		y += DP_CTRL_H + 8;
		DS_Head( x, y, w, "More XP by rank in the Order" );
		y += 18;
		for ( int i = 0; i < (int)ARRAY_LEN( dsRanks ); i++ ) {
			int *n = DS_NumOf( c.ranks, dsRanks[i], qtrue );

			ds.dirty = (qboolean)( DS_Number( x, y, w, dsRanks[i], n, 0, 10000, 5, NULL ) || ds.dirty );
			y += DP_CTRL_H + 2;
		}
		for ( size_t k = 0; k < c.ranks.size(); k++ ) {
			qboolean usual = qfalse;

			for ( int i = 0; i < (int)ARRAY_LEN( dsRanks ); i++ )
				usual = (qboolean)( usual || !Q_stricmp( dsRanks[i], c.ranks[k].name.c_str() ) );
			if ( usual )
				continue;
			ds.dirty = (qboolean)( DS_Number( x, y, w, c.ranks[k].name.c_str(), &c.ranks[k].n, 0, 10000, 5, NULL ) || ds.dirty );
			y += DP_CTRL_H + 2;
		}
		DS_Field( x, y, w - DP_Wide( 50 ), ds.newRank, sizeof( ds.newRank ), "Another rank, as RPMod names it" );
		if ( ds.newRank[0] && DS_ButtonAt( x + w - DP_Wide( 46 ), y, DP_Wide( 46 ), "Add", qfalse ) ) {
			DS_NumOf( c.ranks, ds.newRank, qtrue );
			ds.newRank[0] = '\0';
			ds.focus = NULL;
		}
		y += DP_CTRL_H + 8;
		DS_Head( x, y, w, "Points it starts with, free" );
		y += 18;
		for ( size_t t = 0; t < ds.edit.traits.size(); t++ ) {
			int *n = DS_NumOf( c.start, ds.edit.traits[t].name, qtrue );

			ds.dirty = (qboolean)( DS_Number( x, y, w, ds.edit.traits[t].name, n, 0, ds.edit.cap, 1, NULL ) || ds.dirty );
			y += DP_CTRL_H + 2;
		}
		y += 6;
		DS_Head( x, y, w, "Traits it raises" );
		y += 16;
		DS_Label( x, y, w, c.allowed.empty() ? "Every one: tick some to keep it to them" : "Only the ticked ones", dpDim );
		y += 14;
		for ( size_t t = 0; t < ds.edit.traits.size(); t++ ) {
			const char *tn = ds.edit.traits[t].name;
			int on = 0;

			for ( size_t k = 0; k < c.allowed.size(); k++ )
				on = on || !Q_stricmp( c.allowed[k].c_str(), tn );
			if ( DS_Toggle( x, y, w, tn, &on, "Ticked", "-" ) ) {
				if ( on ) {
					c.allowed.push_back( tn );
				} else {
					for ( size_t k = 0; k < c.allowed.size(); k++ ) {
						if ( !Q_stricmp( c.allowed[k].c_str(), tn ) ) {
							c.allowed.erase( c.allowed.begin() + k );
							break;
						}
					}
				}
				ds.dirty = qtrue;
			}
			y += DP_CTRL_H + 2;
		}
	} else if ( tab == DT_ACTIONS ) {
		dsAction_t &a = ds.edit.actions[ds.item];

		ds.dirty = (qboolean)( DS_Number( x, y, w, "DC unless told", &a.dc, -100, 1000, 1, NULL ) || ds.dirty );
		y += DP_CTRL_H + 8;
		DS_Head( x, y, w, "Traits added to the roll, times" );
		y += 18;
		for ( size_t t = 0; t < ds.edit.traits.size(); t++ ) {
			float *wt = DS_WeightOf( a.uses, ds.edit.traits[t].name, qtrue );
			int index = 3;

			for ( int i = 0; i < (int)ARRAY_LEN( dsWeights ); i++ ) {
				if ( fabsf( dsWeights[i] - *wt ) < 0.01f )
					index = i;
			}
			if ( DS_Seen( y, DP_CTRL_H ) ) {
				DP_Text( x, y + 3, w * 0.6f, ds.edit.traits[t].name, dpText, DP_Font(), 0.56f, qfalse );
				if ( DP_Button( x + w - DP_Wide( 18 ) * 2 - DP_Wide( 44 ), y, DP_Wide( 18 ), "-", qfalse ) && index > 0 ) {
					*wt = dsWeights[index - 1];
					ds.dirty = qtrue;
				}
				DP_Text( x + w - DP_Wide( 18 ) - DP_Wide( 44 ), y + 3, DP_Wide( 44 ), *wt == 0.0f ? "-" : va( "x%g", *wt ), dpWhite, DP_Font(), 0.6f, qtrue );
				if ( DP_Button( x + w - DP_Wide( 18 ), y, DP_Wide( 18 ), "+", qfalse ) && index < (int)ARRAY_LEN( dsWeights ) - 1 ) {
					*wt = dsWeights[index + 1];
					ds.dirty = qtrue;
				}
			}
			y += DP_CTRL_H + 2;
		}
		y += 6;
		DS_Head( x, y, w, "A bonus by class" );
		y += 18;
		for ( size_t c = 0; c < ds.edit.classes.size(); c++ ) {
			int *n = DS_NumOf( a.bonus, ds.edit.classes[c].name, qtrue );

			ds.dirty = (qboolean)( DS_Number( x, y, w, ds.edit.classes[c].name, n, -50, 50, 1, NULL ) || ds.dirty );
			y += DP_CTRL_H + 2;
		}
		y += 4;
		y += DS_Wrapped( x, y, w, "The roll: the dice, plus each trait's points times its number above (rounded down), plus the class's bonus, against the DC.", dpDim );
	}
	return y;
}

static float DS_DrawEdit( float x, float y, float w ) {
	static const char *tabs[DT_NUM] = { "Rules", "Traits", "Classes", "Actions" };
	const float tw = w / DT_NUM;

	for ( int i = 0; i < DT_NUM; i++ ) {
		if ( DS_ButtonAt( x + tw * i, y, tw - 2, tabs[i], (qboolean)( ds.tab == i ) ) ) {
			ds.tab = i;
			ds.scroll = 0;
			ds.focus = NULL;
		}
	}
	y += DP_CTRL_H + 8;
	return ds.tab == DT_RULES ? DS_DrawRules( x, y, w ) : DS_DrawItems( x, y, w );
}

/*
===============================================================================

The players, for a system's masters

===============================================================================
*/

static float DS_DrawPlayers( float x, float y, float w ) {
	dsAction_t *action;

	if ( !ds.rules.actions.empty() ) {
		ds.askAction = Com_Clampi( 0, (int)ds.rules.actions.size() - 1, ds.askAction );
		action = &ds.rules.actions[ds.askAction];
		if ( DS_Seen( y, DP_CTRL_H ) ) {
			DP_Text( x, y + 3, w * 0.4f, "Ask for", dpText, DP_Font(), 0.56f, qfalse );
			if ( DP_Button( x + w * 0.4f, y, w * 0.6f, va( "< %s >", action->name ), qfalse ) ) {
				ds.askAction = ( ds.askAction + 1 ) % (int)ds.rules.actions.size();
				ds.askDc = 0;
			}
		}
		y += DP_CTRL_H + 3;
		DS_Number( x, y, w, va( "Against DC (its own: %i)", action->dc ), &ds.askDc, 0, 200, 1, "its own" );
		y += DP_CTRL_H + 3;
	} else {
		action = NULL;
	}
	DS_Number( x, y, w, "XP to grant", &ds.grantXp, -100, 100, 1, NULL );
	y += DP_CTRL_H + 8;
	if ( !ds.playersKnown ) {
		DS_Label( x, y, w, "Asking C1B3R...", dpDim );
		return y + 14;
	}
	if ( ds.players.empty() ) {
		DS_Label( x, y, w, "No one has joined yet", dpDim );
		return y + 14;
	}
	for ( size_t i = 0; i < ds.players.size(); i++ ) {
		const dsPlayer_t &p = ds.players[i];
		const float bw = ( w - 6 ) / 3;

		if ( DS_Seen( y, 24 ) ) {
			WG_TypedText( x, y, w * 0.55f, p.name.c_str(), p.client >= 0 ? dpWhite : dpText, DP_Font(), 0.56f, qfalse, qfalse );
			DP_Text( x + w * 0.55f, y, w * 0.45f, va( "%s, %i/%i XP", p.klass.c_str(), p.spent, p.pool ), dpDim, DP_Font(), 0.52f, qfalse );
			DP_Text( x, y + 11, w, p.client >= 0 ? va( "here: %s", DS_PlayerName( p.client ) ) : "not here", dpDim, DP_Font(), 0.5f, qfalse );
		}
		y += 24;
		if ( DS_ButtonAt( x, y, bw, ds.grantXp >= 0 ? va( "+%i XP", ds.grantXp ) : va( "%i XP", ds.grantXp ), qfalse ) && ds.grantXp )
			DS_Do( DO_GRANT, "grant", DS_SystemField() + va( ",\"char\":%i,\"xp\":%i", p.charId, ds.grantXp ) );
		if ( DS_ButtonAt( x + bw + 3, y, bw, p.locked ? "Unlock" : "Lock", (qboolean)( p.locked != 0 ) ) )
			DS_Do( DO_LOCK, "lock", DS_SystemField() + va( ",\"char\":%i,\"locked\":%i", p.charId, p.locked ? 0 : 1 ) );
		if ( action && p.client >= 0 && DS_ButtonAt( x + ( bw + 3 ) * 2, y, bw, "Ask a roll", qfalse ) ) {
			std::string fields = DS_SystemField() + ",\"action\":";

			CL_Presence_Json( fields, action->name );
			fields += va( ",\"client\":%i", p.client );
			if ( ds.askDc )
				fields += va( ",\"dc\":%i", ds.askDc );
			if ( DS_Do( DO_ASK, "ask", fields ) )
				DS_Message( va( "Asked for %s", action->name ) );
		}
		y += DP_CTRL_H + 8;
	}
	return y;
}

/*
===============================================================================

The app

===============================================================================
*/

// a master's ask, over whatever page is on
static float DS_DrawAsk( float x, float y, float w ) {
	const float h = 42.0f;
	std::string text;

	if ( !ds.ask.id )
		return y;
	text = va( "%s^7 asks: roll %s (DC %i)%s%s", ds.ask.from.c_str(), ds.ask.action.c_str(), ds.ask.dc, ds.ask.label.empty() ? "" : ", ", ds.ask.label.c_str() );
	DP_Fill( x, y, w, h, dpButtonHover );
	DP_Frame( x, y, w, h, dpWhite );
	WG_TypedText( x + 4, y + 4, w - 8, text.c_str(), dpWhite, DP_Font(), 0.54f, qfalse, qfalse );
	if ( DP_Button( x + 4, y + 22, w * 0.5f - 6, "Roll", qfalse ) )
		DS_AnswerAsk();
	if ( DP_Button( x + w * 0.5f + 2, y + 22, w * 0.5f - 6, "Not now", qfalse ) )
		ds.ask.id = 0;
	return y + h + 6;
}

static void DS_Back( void ) {
	ds.focus = NULL;
	ds.scroll = 0;
	switch ( ds.page ) {
	case DS_ITEM:
		DS_CloseItem();
		break;
	case DS_EDIT:
		if ( ds.dirty && !ds.leaving ) {
			ds.leaving = qtrue;
			DS_Message( "Not saved: Back again leaves it as it was" );
			break;
		}
		ds.dirty = ds.leaving = qfalse;
		ds.page = ds.editId ? DS_SHEET : DS_LIST;
		break;
	case DS_PLAYERS:
		ds.page = DS_SHEET;
		break;
	case DS_SHEET:
		ds.page = DS_LIST;
		DS_AskList();
		break;
	default:
		CL_Datapad_ToGames();
		break;
	}
}

void CL_Dice_Draw( const dpRect_t *s ) {
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 16 );
	float y = s->y + DP_STATUS_H + 6, end;
	const char *title;

	if ( DP_Button( x, y, DP_Wide( 50 ), ds.page == DS_LIST ? "< Home" : ds.page == DS_ITEM ? "< Done" : "< Back", qfalse ) ) {
		if ( ds.page == DS_LIST ) {
			ds.focus = NULL;
			CL_Datapad_Home();
		} else {
			DS_Back();
		}
		return;
	}
	title = ds.page == DS_LIST ? "Dice" : ds.page == DS_EDIT ? ( ds.editId ? "Edit system" : "New system" )
		: ds.page == DS_ITEM ? ( ds.tab == DT_TRAITS ? "Trait" : ds.tab == DT_CLASSES ? "Class" : "Action" )
		: ds.page == DS_PLAYERS ? "Players" : ds.sysName.c_str();
	if ( ds.page == DS_EDIT || ds.page == DS_ITEM ) {
		DP_Text( x + DP_Wide( 56 ), y + 1, w - DP_Wide( 110 ), title, dpText, DP_TitleFont(), 0.55f, qfalse );
		if ( DP_Button( x + w - DP_Wide( 50 ), y, DP_Wide( 50 ), "Save", (qboolean)ds.dirty ) ) {
			if ( ds.page == DS_ITEM )
				DS_CloseItem();
			DS_Save();
		}
	} else if ( ds.page == DS_SHEET && ds.master && ds.rulesKnown ) {
		DP_Text( x + DP_Wide( 56 ), y + 1, w - DP_Wide( 160 ), title, dpText, DP_TitleFont(), 0.55f, qfalse );
		if ( DP_Button( x + w - DP_Wide( 100 ), y, DP_Wide( 48 ), "Edit", qfalse ) ) {
			DS_EditSystem();
			return;
		}
		if ( DP_Button( x + w - DP_Wide( 50 ), y, DP_Wide( 50 ), "Players", qfalse ) ) {
			ds.page = DS_PLAYERS;
			ds.scroll = 0;
			ds.playersKnown = qfalse;
			DS_AskPlayers();
			return;
		}
	} else {
		DP_Text( x + DP_Wide( 56 ), y + 1, w - DP_Wide( 56 ), title, dpText, DP_TitleFont(), 0.55f, qfalse );
	}
	y += DP_CTRL_H + 6;
	y = DS_DrawAsk( x, y, w );

	// the page, scrolled
	dsTop = y;
	dsBottom = s->y + s->h - 18;
	ds.scroll = Com_Clamp( 0.0f, Q_max( 0.0f, ds.content - ( dsBottom - dsTop ) ), ds.scroll );
	y -= ds.scroll;
	switch ( ds.page ) {
	case DS_LIST:
		end = DS_DrawList( x, y, w );
		break;
	case DS_SHEET:
		if ( !ds.rulesKnown || !ds.sheetKnown ) {
			DS_Label( x, y, w, ds.message[0] && cls.realtime - ds.messageTime < DS_MSG_MS ? ds.message : "Asking C1B3R...", dpDim );
			end = y + 14;
		} else if ( ds.picking ) {
			end = DS_DrawClasses( x, y, w );
		} else {
			end = DS_DrawSheet( x, y, w );
		}
		end = DS_DrawLog( x, end, w );
		break;
	case DS_EDIT:
		end = DS_DrawEdit( x, y, w );
		break;
	case DS_ITEM:
		end = ds.item < DS_ItemCount( ds.tab ) ? DS_DrawItem( x, y, w ) : y;
		break;
	default:
		end = DS_DrawPlayers( x, y, w );
		break;
	}
	ds.content = end + ds.scroll - dsTop;
	if ( ds.content > dsBottom - dsTop )
		DP_ScrollBar( x + w + DP_Wide( 3 ), dsTop, dsBottom - dsTop, ds.scroll, ds.content );
	if ( ds.message[0] && cls.realtime - ds.messageTime < DS_MSG_MS )
		DP_Text( x, s->y + s->h - 14, w, ds.message, dpWhite, DP_Font(), 0.52f, qfalse );
	WG_DrawOverlay();
}

void CL_Dice_Shown( qboolean shown ) {
	ds.shown = shown;
	ds.focus = NULL;
	if ( !shown )
		return;
	if ( ds.page == DS_LIST || !ds.listKnown )
		DS_AskList();
	if ( ds.page == DS_SHEET && ds.sysId )
		DS_AskSystem();
}

qboolean CL_Dice_Key( int key ) {
	if ( key == A_MWHEELUP || key == A_MWHEELDOWN ) {
		ds.scroll = Q_max( 0.0f, ds.scroll + ( key == A_MWHEELUP ? -24.0f : 24.0f ) );
		return qtrue;
	}
	if ( !ds.focus )
		return qfalse;
	if ( key == A_ENTER || key == A_KP_ENTER || key == A_TAB ) {
		ds.focus = NULL;
		return qtrue;
	}
	if ( WG_EditKey( ds.focus, ds.focusSize, key, NULL ) & WG_EDIT_CHANGED )
		ds.dirty = qtrue;
	return qtrue;
}

void CL_Dice_Char( int ch ) {
	if ( ds.focus && WG_EditChar( ds.focus, ds.focusSize, ch, NULL ) )
		ds.dirty = qtrue;
}

// /datapad dice [system]: opened on the systems, or on one by its number or name
void CL_Dice_Open( const char *system ) {
	if ( !system[0] ) {
		ds.page = DS_LIST;
		return;
	}
	if ( Q_isanumber( system ) ) {
		DS_OpenSystem( atoi( system ) );
		return;
	}
	for ( size_t i = 0; i < ds.systems.size(); i++ ) {
		if ( !Q_stricmp( ds.systems[i].name.c_str(), system ) ) {
			DS_OpenSystem( ds.systems[i].id );
			return;
		}
	}
	DS_Message( va( "No system called %s", system ) );
}

/*
===============================================================================

The command

===============================================================================
*/

static void DS_Dicemaster_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !arg[0] ) {
		Cbuf_AddText( "datapad dice\n" );
		return;
	}
	if ( !Q_stricmp( arg, "roll" ) ) {
		char action[DS_NAME * 2];
		int dc = 0, last = Cmd_Argc() - 1;

		if ( Cmd_Argc() < 3 ) {
			DS_AnswerAsk();
			if ( ds.message[0] )
				Com_Printf( "%s\n", ds.message );
			return;
		}
		if ( last >= 3 && Q_isanumber( Cmd_Argv( last ) ) ) {
			dc = atoi( Cmd_Argv( last ) );
			last--;
		}
		action[0] = '\0';
		for ( int i = 2; i <= last; i++ )
			Q_strcat( action, sizeof( action ), va( "%s%s", i > 2 ? " " : "", Cmd_Argv( i ) ) );
		if ( !ds.sysId )
			ds.sysId = cl_diceSystem->integer;
		if ( !ds.sysId ) {
			Com_Printf( "Pick a system first: open the datapad's Dice, or /dicemaster system <number>\n" );
			return;
		}
		DS_Roll( action, dc, 0 );
		return;
	}
	if ( !Q_stricmp( arg, "edit" ) || !Q_stricmp( arg, "players" ) ) {
		if ( !ds.sysId || !ds.rulesKnown || !ds.master ) {
			Com_Printf( "Open a system you run first: /datapad dice <system>\n" );
			return;
		}
		if ( !Q_stricmp( arg, "edit" ) ) {
			DS_EditSystem();
		} else {
			ds.page = DS_PLAYERS;
			ds.scroll = 0;
			ds.playersKnown = qfalse;
			DS_AskPlayers();
		}
		Cbuf_AddText( "datapad dice\n" );
		return;
	}
	if ( !Q_stricmp( arg, "system" ) && Cmd_Argc() >= 3 ) {
		DS_OpenSystem( atoi( Cmd_Argv( 2 ) ) );
		Com_Printf( "Dice: system %i\n", ds.sysId );
		return;
	}
	Com_Printf( "usage: dicemaster                      the datapad's Dice\n" );
	Com_Printf( "       dicemaster roll                 roll what a master asked you for\n" );
	Com_Printf( "       dicemaster roll <action> [dc]   roll an action of your system, against its DC or this one\n" );
	Com_Printf( "       dicemaster system <number>      the system you roll in\n" );
	Com_Printf( "       dicemaster edit|players         a system you run: its rules, or the characters in it\n" );
}

void CL_Dice_Init( void ) {
	cl_diceSystem = Cvar_Get( "cl_diceSystem", "0", CVAR_ARCHIVE_ND, "The Dice Master system you last opened: the one /dicemaster roll rolls in" );
	ds.sysId = cl_diceSystem->integer;
	ds.grantXp = 5;
	Cmd_AddCommand( "dicemaster", DS_Dicemaster_f, "The Dice Master: rule systems, your sheet and rolls, on the datapad" );
}
