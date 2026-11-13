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

// cl_characters.cpp -- /characters: the characters of your RPMod account, kept by C1B3R.
//
// Each character has its own inventory, character panel (Inspect), holster and presets, and keeps
// what this game puts back when it's played: the look (a preset's userinfo), the datapad's settings,
// the Effect Replacer's swaps, the player shaders (Mine), the language character and the saber
// styles. Play as one and all of it comes back; C1B3R serves its inventory, panel and holster.
//
// While a character is played, what's changed here is kept with it: every few seconds the game's
// state is compared with what was sent last, and sent again when it differs. Only once this game
// is known to hold that character (it played it, or it was made from it here): another computer's
// settings never overwrite a character it hasn't played.
//
// For anyone whose game is linked to their RPMod account; no admin rank needed.

#include <string>
#include <vector>
#include "client.h"
#include "cl_widgets.h"
#include "cl_presence.h"

#define CH_CMD				"characters"
#define CH_MAX				10			// as C1B3R keeps
#define CH_NAME_LEN			129			// with its colour codes (RPMod's ^a-^z too); 32 letters shown at most
#define CH_SYNC_MS			5000
#define CH_MESSAGE_MS		5000

#define CH_X				20.0f
#define CH_Y				24.0f
#define CH_W				600.0f
#define CH_H				432.0f
#define CH_TOP				( CH_Y + 40 )
#define CH_A_X				( CH_X + 12 )
#define CH_A_W				170.0f
#define CH_B_X				( CH_A_X + CH_A_W + 12 )
#define CH_B_W				190.0f
#define CH_C_X				( CH_B_X + CH_B_W + 12 )
#define CH_C_W				( CH_X + CH_W - 12 - CH_C_X )
#define CH_ROW_H			22.0f
#define CH_CTRL_H			16.0f
#define CH_TEXT				0.8f
#define CH_SMALL			0.65f

static const float chPanel[4]		= { 0.0f, 0.0f, 0.08f, 0.88f };
static const float chBorder[4]		= { 0.5f, 0.5f, 0.7f, 1.0f };
static const float chItem[4]		= { 0.15f, 0.15f, 0.25f, 0.8f };
static const float chItemHover[4]	= { 0.25f, 0.25f, 0.4f, 0.9f };
static const float chItemOn[4]		= { 0.35f, 0.24f, 0.05f, 0.9f };
static const float chField[4]		= { 0.05f, 0.05f, 0.12f, 0.95f };
static const float chOrange[4]		= { 1.0f, 0.682f, 0.0f, 1.0f };
static const float chWhite[4]		= { 1.0f, 1.0f, 1.0f, 1.0f };
static const float chDim[4]			= { 0.6f, 0.6f, 0.7f, 1.0f };
static const float chYellow[4]		= { 1.0f, 0.85f, 0.3f, 1.0f };
static const float chGreen[4]		= { 0.4f, 1.0f, 0.4f, 1.0f };

// the datapad's settings a character keeps, as C1B3R names them
static const struct {
	const char	*key, *cvar;
} chDatapad[] = {
	{ "x", "cl_datapadX" }, { "y", "cl_datapadY" }, { "color", "cl_datapadColor" }, { "emote", "cl_datapadEmote" }, { "hand", "cl_datapadHand" },
};

typedef struct {
	int			id;
	char		name[CH_NAME_LEN];
	qboolean	active;
	char		tag[12];		// main, secondary or extra; "" C1B3R didn't say
} chChar_t;

typedef struct {
	std::string					title;
	qboolean					favorite;
	std::vector<std::string>	values;		// CL_Presets_Keys' order
	std::string					id;			// its own (8 hex), "" from before ids
	std::string					label;		// its player's own name for it, "" none
} chPreset_t;

// a character's data, as C1B3R's lines gave it
typedef struct {
	int							id;
	std::vector<std::string>	look;		// CL_Presets_Keys' order, "" not kept
	std::vector<chPreset_t>		presets;
	std::string					datapad[ARRAY_LEN( chDatapad )];
	qboolean					hasDatapad[ARRAY_LEN( chDatapad )];
	std::vector<std::string>	fxFrom, fxTo, shFrom, shTo;
	std::string					language, saberStyles;
	qboolean					hasLanguage, hasStyles;
	qboolean					inspect, hasInspect;	// others may Inspect it
	std::string					has;		// the parts it keeps, by comma ("" none: a new, empty one)
} chData_t;

enum { CH_TYPE_NONE, CH_TYPE_NEW, CH_TYPE_RENAME, CH_TYPE_DELETE, CH_TYPE_SETUP };

// the right column: what the picked one keeps, or sharing it
enum { CH_SIDE_SUITE, CH_SIDE_SHARE };

#define CH_SHARE_MAX		16			// players at once, as C1B3R takes
#define CH_OFFERS			8

// a copy of someone's character offered to you
typedef struct {
	int		id;
	char	name[CH_NAME_LEN];
	char	from[MAX_NETNAME];
	int		items;
} chOffer_t;

static struct {
	qboolean	open;
	int			font, titleFont;
	sfxHandle_t	clickSound;
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;

	chChar_t	chars[CH_MAX];
	int			num;
	qboolean	known;			// the list came
	char		refusal[256];	// why C1B3R won't say, "" none
	int			activeId;
	char		listFor[64];	// the session the list was asked on
	int			sel;			// a character's id, picked in the list

	chData_t	data;			// the picked one's, when data.id == sel
	int			applyId;		// played once its data is here (0 none)

	int			typing;			// CH_TYPE_*
	char		field[CH_NAME_LEN];
	qboolean	newExtra;		// a new one: tagged extra (shared, for missions), else secondary
	qboolean	setupGo;		// Enter on the setup page: set up as the button does
	char		setupTold[64];	// the session told once that it has no character yet

	std::string	lastSent;		// what was sent for the character played, to send it only when changed
	int			nextSync;

	char		message[160];
	int			messageTime;
	qboolean	messageBad;
	float		yaw;

	int			side;			// CH_SIDE_*

	// sharing a copy: the players picked, and what C1B3R said for each
	qboolean	picked[MAX_CLIENTS];
	char		shareSaid[MAX_CLIENTS][48];
	float		shareScroll;

	chOffer_t	offers[CH_OFFERS];
	int			numOffers;
} ch;

static httpRequest_t chListHttp, chGetHttp, chActHttp, chSyncHttp, chShareHttp, chOffersHttp, chOfferActHttp;
static char chActWhat[32];
static int chActId;
static cvar_t *cl_characterHeld;	// "<account>/<id>": the character this game's settings are

/*
===============================================================================

Talking to C1B3R

===============================================================================
*/

static void CH_Message( const char *text, qboolean bad ) {
	Q_strncpyz( ch.message, text, sizeof( ch.message ) );
	for ( char *nl = strchr( ch.message, '\n' ); nl; nl = NULL )
		*nl = '\0';
	ch.messageTime = cls.realtime;
	ch.messageBad = bad;
}

static qboolean CH_Post( httpRequest_t *req, const char *path, const std::string &fields ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session || req->state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += fields + "}";
	return CL_Presence_Post( req, path, body );
}

static std::string CH_Str( const char *name, const char *value ) {
	std::string out = va( ",\"%s\":", name );

	CL_Presence_Json( out, value );
	return out;
}

static const char *CH_Account( void ) {
	const char *name = NULL;
	int when;

	return CL_Feedback_Account( &name, &when ) == 1 && name ? name : "";
}

// this game's settings are the character's (it was played here, or made from them)
static qboolean CH_Held( int id ) {
	return (qboolean)( id && cl_characterHeld && CH_Account()[0]
		&& !Q_stricmp( cl_characterHeld->string, va( "%s/%i", CH_Account(), id ) ) );
}

static void CH_Hold( int id ) {
	Cvar_Set( "cl_characterHeld", id ? va( "%s/%i", CH_Account(), id ) : "" );
	ch.lastSent.clear();
	ch.nextSync = cls.realtime + 1000;
}

static int CH_Split( char *line, char **field, int max ) {
	int n = 0;

	for ( char *f = line; f && n < max; n++ ) {
		field[n] = f;
		f = strchr( f, '\t' );
		if ( f )
			*f++ = '\0';
	}
	return n;
}

// an answer's lines, each split by tabs and unescaped, to fn
template <typename F> static void CH_Lines( const char *answer, F fn ) {
	std::string all( answer );
	size_t at = 0;

	while ( at < all.size() ) {
		size_t end = all.find( '\n', at );
		std::vector<std::string> fields;
		char line[4096], *f[16];
		int n;

		if ( end == std::string::npos )
			end = all.size();
		Q_strncpyz( line, all.substr( at, end - at ).c_str(), sizeof( line ) );
		at = end + 1;
		n = CH_Split( line, f, 16 );
		for ( int i = 0; i < n; i++ )
			fields.push_back( i ? CL_Presence_Unfield( f[i] ) : std::string( f[i] ) );
		if ( n )
			fn( fields );
	}
}

static void CH_AskList( void ) {
	if ( CH_Post( &chListHttp, "/v2/chars/list", "" ) )
		Q_strncpyz( ch.listFor, CL_Presence_Session(), sizeof( ch.listFor ) );
}

static void CH_AskData( int id ) {
	if ( id && chGetHttp.state != HTTP_BUSY )
		CH_Post( &chGetHttp, "/v2/chars/get", va( ",\"id\":%i", id ) );
}

static void CH_AskOffers( void ) {
	CH_Post( &chOffersHttp, "/v2/chars/offers", "" );
}

/*
===============================================================================

What a character keeps: taken from the game, and put back

===============================================================================
*/

// the game's state now, as a character's data (JSON); presets: the character's own
static std::string CH_Snapshot( int id ) {
	const char **keys;
	const int numKeys = CL_Presets_Keys( &keys );
	std::string out = "{\"preset\":{";

	for ( int k = 0; k < numKeys; k++ ) {
		out += k ? ",\"" : "\"";
		out += keys[k];
		out += "\":";
		CL_Presence_Json( out, Cvar_VariableString( keys[k] ) );
	}
	out += "},\"presets\":[";
	{
		int n = 0;

		for ( int i = 0; i < CL_Presets_Count(); i++ ) {
			const char *date;
			qboolean favorite;
			int character;

			CL_Presets_Get( i, 0, &date, &favorite, &character );
			if ( character != id )
				continue;
			out += n++ ? ",{\"title\":" : "{\"title\":";
			CL_Presence_Json( out, date );
			out += ",\"label\":";
			CL_Presence_Json( out, CL_Presets_Label( i ) );
			out += va( ",\"id\":\"%s\",\"favorite\":%i,\"values\":{", CL_Presets_Id( i ), favorite ? 1 : 0 );
			for ( int k = 0; k < numKeys; k++ ) {
				out += k ? ",\"" : "\"";
				out += keys[k];
				out += "\":";
				CL_Presence_Json( out, CL_Presets_Get( i, k, &date, &favorite, &character ) );
			}
			out += "}}";
		}
	}
	// the preset the look is now: the holster items worn with it (cl_holster.cpp)
	out += va( "],\"wornPreset\":\"%s\",\"datapad\":{", CL_Presets_WornId( id ) );
	for ( int i = 0; i < (int)ARRAY_LEN( chDatapad ); i++ )
		out += va( "%s\"%s\":%g", i ? "," : "", chDatapad[i].key, Cvar_VariableValue( chDatapad[i].cvar ) );
	out += "},\"fxmine\":[";
	for ( int i = 0; i < CL_EffectSwap_Count(); i++ ) {
		const char *from, *to;

		if ( !CL_EffectSwap_Get( i, &from, &to ) )
			continue;
		out += i ? ",[" : "[";
		CL_Presence_Json( out, from );
		out += ",";
		CL_Presence_Json( out, to );
		out += "]";
	}
	out += "],\"minesh\":[";
	for ( int i = 0; i < CL_ShaderManager_MineCount(); i++ ) {
		const char *from, *to;

		if ( !CL_ShaderManager_MineGet( i, &from, &to ) )
			continue;
		out += i ? ",[" : "[";
		CL_Presence_Json( out, from );
		out += ",";
		CL_Presence_Json( out, to );
		out += "]";
	}
	out += "],\"language\":";
	CL_Presence_Json( out, CL_Languages_Character() );
	out += ",\"saberStyles\":";
	CL_Presence_Json( out, Cvar_VariableString( "cl_saberStyles" ) );
	out += va( ",\"inspect\":%i}", Cvar_VariableIntegerValue( "cl_inspectOthers" ) ? 1 : 0 );
	return out;
}

// only the look (a new character from nothing more)
static std::string CH_LookOnly( void ) {
	const char **keys;
	const int numKeys = CL_Presets_Keys( &keys );
	std::string out = "{\"preset\":{";

	for ( int k = 0; k < numKeys; k++ ) {
		out += k ? ",\"" : "\"";
		out += keys[k];
		out += "\":";
		CL_Presence_Json( out, Cvar_VariableString( keys[k] ) );
	}
	return out + "}}";
}

static int CH_KeyIndex( const char *key ) {
	const char **keys;
	const int numKeys = CL_Presets_Keys( &keys );

	for ( int k = 0; k < numKeys; k++ ) {
		if ( !Q_stricmp( keys[k], key ) )
			return k;
	}
	return -1;
}

static void CH_ReadData( const char *answer ) {
	const char **keys;
	const int numKeys = CL_Presets_Keys( &keys );
	chData_t &d = ch.data;

	d.id = 0;
	d.look.assign( numKeys, "" );
	d.presets.clear();
	d.fxFrom.clear();
	d.fxTo.clear();
	d.shFrom.clear();
	d.shTo.clear();
	d.language.clear();
	d.saberStyles.clear();
	d.hasLanguage = d.hasStyles = qfalse;
	d.inspect = qtrue;
	d.hasInspect = qfalse;
	d.has.clear();
	for ( int i = 0; i < (int)ARRAY_LEN( chDatapad ); i++ ) {
		d.datapad[i].clear();
		d.hasDatapad[i] = qfalse;
	}
	CH_Lines( answer, [&]( const std::vector<std::string> &f ) {
		const std::string &kind = f[0];

		if ( kind == "char" && f.size() >= 2 ) {
			d.id = atoi( f[1].c_str() );
		} else if ( kind == "has" ) {
			d.has = f.size() >= 2 && f[1].size() ? "," + f[1] + "," : "";
		} else if ( kind == "preset" && f.size() >= 3 ) {
			const int k = CH_KeyIndex( f[1].c_str() );

			if ( k >= 0 )
				d.look[k] = f[2];
		} else if ( kind == "cpreset" && (int)f.size() >= 3 + numKeys ) {
			chPreset_t p;

			p.title = f[1];
			p.favorite = (qboolean)( atoi( f[2].c_str() ) != 0 );
			p.values.assign( f.begin() + 3, f.begin() + 3 + numKeys );
			if ( (int)f.size() > 3 + numKeys )
				p.id = f[3 + numKeys];
			if ( (int)f.size() > 4 + numKeys )
				p.label = f[4 + numKeys];
			d.presets.push_back( p );
		} else if ( kind == "datapad" && f.size() >= 3 ) {
			for ( int i = 0; i < (int)ARRAY_LEN( chDatapad ); i++ ) {
				if ( f[1] == chDatapad[i].key ) {
					d.datapad[i] = f[2];
					d.hasDatapad[i] = qtrue;
				}
			}
		} else if ( ( kind == "fxmine" || kind == "minesh" ) && f.size() >= 3 ) {
			( kind == "fxmine" ? d.fxFrom : d.shFrom ).push_back( f[1] );
			( kind == "fxmine" ? d.fxTo : d.shTo ).push_back( f[2] );
		} else if ( kind == "language" ) {
			d.language = f.size() >= 2 ? f[1] : "";
			d.hasLanguage = qtrue;
		} else if ( kind == "saberStyles" ) {
			d.saberStyles = f.size() >= 2 ? f[1] : "";
			d.hasStyles = qtrue;
		} else if ( kind == "inspect" ) {
			d.inspect = (qboolean)( f.size() < 2 || atoi( f[1].c_str() ) != 0 );
			d.hasInspect = qtrue;
		}
	} );
}

static qboolean CH_Has( const chData_t &d, const char *part ) {
	return (qboolean)( d.has.find( va( ",%s,", part ) ) != std::string::npos );
}

// the character's data put back in the game: only the parts it keeps
static void CH_Apply( const chData_t &d ) {
	const char **keys;
	const int numKeys = CL_Presets_Keys( &keys );
	std::vector<const char *> values, from, to;

	for ( int k = 0; k < numKeys; k++ )
		values.push_back( k < (int)d.look.size() ? d.look[k].c_str() : "" );
	CL_Presets_SetAppearance( values.data() );
	for ( size_t i = 0; i < d.presets.size(); i++ ) {
		std::vector<const char *> pv;

		for ( int k = 0; k < numKeys; k++ )
			pv.push_back( d.presets[i].values[k].c_str() );
		CL_Presets_Import( d.id, d.presets[i].title.c_str(), pv.data(), d.presets[i].favorite, d.presets[i].id.c_str(), d.presets[i].label.c_str() );
	}
	for ( int i = 0; i < (int)ARRAY_LEN( chDatapad ); i++ ) {
		if ( d.hasDatapad[i] )
			Cvar_Set( chDatapad[i].cvar, d.datapad[i].c_str() );
	}
	for ( size_t i = 0; i < d.fxFrom.size(); i++ ) {
		from.push_back( d.fxFrom[i].c_str() );
		to.push_back( d.fxTo[i].c_str() );
	}
	if ( CH_Has( d, "fxmine" ) )
		CL_EffectSwap_Replace( from.data(), to.data(), (int)from.size() );
	from.clear();
	to.clear();
	for ( size_t i = 0; i < d.shFrom.size(); i++ ) {
		from.push_back( d.shFrom[i].c_str() );
		to.push_back( d.shTo[i].c_str() );
	}
	if ( CH_Has( d, "minesh" ) )
		CL_ShaderManager_MineReplace( from.data(), to.data(), (int)from.size() );
	if ( d.hasLanguage && d.language.size() && Q_stricmp( d.language.c_str(), CL_Languages_Character() ) )
		Cbuf_AddText( va( "language character %s\n", d.language.c_str() ) );
	if ( d.hasStyles )
		Cvar_Set( "cl_saberStyles", d.saberStyles.c_str() );
	// kept from Inspect or not: one that never said is open to it
	Cvar_Set( "cl_inspectOthers", !d.hasInspect || d.inspect ? "1" : "0" );
}

static const chChar_t *CH_Find( int id ) {
	for ( int i = 0; i < ch.num; i++ ) {
		if ( ch.chars[i].id == id )
			return &ch.chars[i];
	}
	return NULL;
}

/*
===============================================================================

Each frame: answers, and keeping the one played up to date

===============================================================================
*/

void CL_Characters_Frame( void ) {
	const char *session = CL_Presence_Session();

	if ( cls.state != CA_ACTIVE )
		return;
	// a new session (linked since, or C1B3R again): the list asked again
	if ( session && Q_stricmp( session, ch.listFor ) && chListHttp.state != HTTP_BUSY )
		CH_AskList();
	if ( HTTP_Poll( &chListHttp ) ) {
		if ( chListHttp.status == 200 ) {
			ch.num = 0;
			ch.activeId = 0;
			ch.refusal[0] = '\0';
			CH_Lines( chListHttp.answer, [&]( const std::vector<std::string> &f ) {
				if ( f[0] != "char" || f.size() < 4 || ch.num >= CH_MAX )
					return;
				chChar_t *c = &ch.chars[ch.num++];

				c->id = atoi( f[1].c_str() );
				Q_strncpyz( c->name, f[2].c_str(), sizeof( c->name ) );
				c->active = (qboolean)( f[3] == "1" );
				Q_strncpyz( c->tag, f.size() >= 6 ? f[5].c_str() : "", sizeof( c->tag ) );
				if ( c->active )
					ch.activeId = c->id;
			} );
			ch.known = qtrue;
			if ( !ch.num && Q_stricmp( ch.setupTold, ch.listFor ) ) {
				Q_strncpyz( ch.setupTold, ch.listFor, sizeof( ch.setupTold ) );
				Com_Printf( "Characters: your game is linked; set up your character with " S_COLOR_GREEN "/" CH_CMD "\n" );
			}
			if ( !CH_Find( ch.sel ) )
				ch.sel = ch.activeId;
			if ( ch.open && ch.data.id != ch.sel )
				CH_AskData( ch.sel );
			// the one played, not held here: an empty one (new, or made for an account from before
			// characters) takes this game's settings
			else if ( ch.activeId && !CH_Held( ch.activeId ) )
				CH_AskData( ch.activeId );
		} else {
			ch.known = qfalse;
			ch.num = ch.activeId = 0;
			Q_strncpyz( ch.refusal, chListHttp.answer[0] ? chListHttp.answer : "C1B3R didn't answer", sizeof( ch.refusal ) );
			for ( char *nl = strchr( ch.refusal, '\n' ); nl; nl = NULL )
				*nl = '\0';
		}
	}
	if ( HTTP_Poll( &chGetHttp ) && chGetHttp.status == 200 ) {
		CH_ReadData( chGetHttp.answer );
		if ( ch.data.id == ch.activeId && !ch.applyId && ch.data.has.empty() && !CH_Held( ch.activeId ) && CH_Account()[0] )
			CH_Hold( ch.activeId );
		if ( ch.applyId && ch.data.id == ch.applyId ) {
			const int played = ch.applyId;

			CH_Apply( ch.data );
			CH_Hold( played );
			ch.applyId = 0;
		}
	}
	if ( HTTP_Poll( &chActHttp ) ) {
		const qboolean ok = (qboolean)( chActHttp.status == 200 );

		if ( !ok ) {
			CH_Message( chActHttp.answer[0] ? chActHttp.answer : "C1B3R didn't answer", qtrue );
		} else if ( !strcmp( chActWhat, "setup" ) ) {
			// the first: main, played now; what's worn now its first preset, and this game's settings its own
			const int id = atoi( strchr( chActHttp.answer, '\t' ) ? strchr( chActHttp.answer, '\t' ) + 1 : "0" );

			if ( id ) {
				ch.sel = ch.activeId = id;
				CL_Presets_AddCurrent( id );
				CH_Hold( id );
				ch.nextSync = cls.realtime;		// sent now, its preset with it
			}
			ch.typing = CH_TYPE_NONE;
			CH_Message( "Set up: you play it now, with its own inventory, holster and panel", qfalse );
		} else if ( !strcmp( chActWhat, "create" ) ) {
			ch.sel = atoi( strchr( chActHttp.answer, '\t' ) ? strchr( chActHttp.answer, '\t' ) + 1 : "0" );
			CH_Message( "Made: Play as it to become it", qfalse );
		} else if ( !strcmp( chActWhat, "select" ) ) {
			ch.activeId = chActId;
			CH_Message( va( "You play %s now", CH_Find( chActId ) ? CH_Find( chActId )->name : "it" ), qfalse );
			// its data put back once it's here
			ch.applyId = chActId;
			if ( ch.data.id == chActId ) {
				CH_Apply( ch.data );
				CH_Hold( chActId );
				ch.applyId = 0;
			} else {
				CH_AskData( chActId );
			}
		} else if ( !strcmp( chActWhat, "delete" ) ) {
			CH_Message( "Deleted", qfalse );
			ch.sel = 0;
		} else if ( !strcmp( chActWhat, "tag" ) ) {
			CH_Message( "Tag changed", qfalse );
		} else if ( !strcmp( chActWhat, "capture" ) ) {
			CH_Hold( chActId );
			CH_Message( "Kept: what you wear and use now is this character's", qfalse );
		} else {
			CH_Message( "Done", qfalse );
		}
		if ( ok )
			CH_AskList();
		ch.data.id = 0;		// asked again
	}
	HTTP_Poll( &chSyncHttp );

	// a copy shared: what C1B3R said for each player
	if ( HTTP_Poll( &chShareHttp ) ) {
		if ( chShareHttp.status != 200 ) {
			CH_Message( chShareHttp.answer[0] ? chShareHttp.answer : "C1B3R didn't answer", qtrue );
		} else {
			int sent = 0, skipped = 0;

			CH_Lines( chShareHttp.answer, [&]( const std::vector<std::string> &f ) {
				const int c = f.size() >= 2 ? atoi( f[1].c_str() ) : -1;

				if ( c < 0 || c >= MAX_CLIENTS )
					return;
				if ( f[0] == "sent" ) {
					Q_strncpyz( ch.shareSaid[c], S_COLOR_GREEN "offered", sizeof( ch.shareSaid[c] ) );
					ch.picked[c] = qfalse;
					sent++;
				} else if ( f[0] == "skip" ) {
					Q_strncpyz( ch.shareSaid[c], va( S_COLOR_YELLOW "%s", f.size() >= 3 ? f[2].c_str() : "not sent" ), sizeof( ch.shareSaid[c] ) );
					skipped++;
				}
			} );
			CH_Message( skipped ? va( "Offered to %i, not to %i: see the list", sent, skipped ) : va( "Offered to %i: they choose to take it", sent ),
				(qboolean)( skipped > 0 ) );
		}
	}

	// copies offered to you
	if ( HTTP_Poll( &chOffersHttp ) && chOffersHttp.status == 200 ) {
		ch.numOffers = 0;
		CH_Lines( chOffersHttp.answer, [&]( const std::vector<std::string> &f ) {
			if ( f[0] != "offer" || f.size() < 5 || ch.numOffers >= CH_OFFERS )
				return;
			chOffer_t *o = &ch.offers[ch.numOffers++];

			o->id = atoi( f[1].c_str() );
			Q_strncpyz( o->name, f[2].c_str(), sizeof( o->name ) );
			Q_strncpyz( o->from, f[3].c_str(), sizeof( o->from ) );
			o->items = atoi( f[4].c_str() );
		} );
	}
	if ( HTTP_Poll( &chOfferActHttp ) ) {
		if ( chOfferActHttp.status != 200 ) {
			CH_Message( chOfferActHttp.answer[0] ? chOfferActHttp.answer : "C1B3R didn't answer", qtrue );
		} else if ( !strncmp( chOfferActHttp.answer, "ok\t", 3 ) ) {
			const char *name = strchr( chOfferActHttp.answer + 3, '\t' );

			ch.sel = atoi( chOfferActHttp.answer + 3 );
			CH_Message( va( "Yours now: %s" S_COLOR_GREEN ". Play as it to become it", name ? CL_Presence_Unfield( name + 1 ).c_str() : "the copy" ), qfalse );
			CH_AskList();
		} else {
			CH_Message( "Declined", qfalse );
		}
		CH_AskOffers();
	}

	// another preset worn (one applied, or the look changed by hand): sent soon, not in 5 s, so
	// the holster others see follows
	if ( session && ch.known && CH_Held( ch.activeId ) ) {
		static char lastWorn[9];
		const char *worn = CL_Presets_WornId( ch.activeId );

		if ( strcmp( worn, lastWorn ) ) {
			Q_strncpyz( lastWorn, worn, sizeof( lastWorn ) );
			ch.nextSync = Q_min( ch.nextSync, cls.realtime + 500 );
		}
	}
	// the character played, kept up to date when this game holds it
	if ( session && ch.known && CH_Held( ch.activeId ) && cls.realtime >= ch.nextSync && chSyncHttp.state != HTTP_BUSY ) {
		const std::string now = CH_Snapshot( ch.activeId );

		ch.nextSync = cls.realtime + CH_SYNC_MS;
		if ( now != ch.lastSent && CH_Post( &chSyncHttp, "/v2/chars/update", va( ",\"id\":%i,\"data\":%s", ch.activeId, now.c_str() ) ) )
			ch.lastSent = now;
	}
}

// the character played (its C1B3R id), 0 none known: Presets keeps presets by it
int CL_Characters_ActiveId( void ) {
	return ch.known ? ch.activeId : 0;
}

// the character played's name, "" none known
const char *CL_Characters_ActiveName( void ) {
	const chChar_t *c = ch.known ? CH_Find( ch.activeId ) : NULL;

	return c ? c->name : "";
}

/*
===============================================================================

The window

===============================================================================
*/

static void CH_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void CH_Box( float x, float y, float w, float h, const float *fill ) {
	CH_Fill( x, y, w, h, fill );
	CH_Fill( x, y, w, 1, chBorder );
	CH_Fill( x, y + h - 1, w, 1, chBorder );
	CH_Fill( x, y, 1, h, chBorder );
	CH_Fill( x + w - 1, y, 1, h, chBorder );
}

static void CH_Text( float x, float y, float w, const char *text, const float *color, float scale, qboolean centred ) {
	WG_TypedText( x, y, w, text, color, ch.font, scale, centred, qtrue );
}

static qboolean CH_InRect( float x, float y, float w, float h ) {
	return (qboolean)( ch.cursorX >= x && ch.cursorX < x + w && ch.cursorY >= y && ch.cursorY < y + h );
}

static qboolean CH_Clicked( float x, float y, float w, float h ) {
	if ( !ch.click || ch.clickX < x || ch.clickX >= x + w || ch.clickY < y || ch.clickY >= y + h )
		return qfalse;
	ch.click = qfalse;
	if ( ch.clickSound )
		S_StartLocalSound( ch.clickSound, CHAN_LOCAL_SOUND );
	return qtrue;
}

static qboolean CH_Button( float x, float y, float w, const char *label, qboolean on, qboolean enabled ) {
	CH_Box( x, y, w, CH_CTRL_H, on ? chItemOn : enabled && CH_InRect( x, y, w, CH_CTRL_H ) ? chItemHover : chItem );
	CH_Text( x, y + 2, w, label, enabled ? chWhite : chDim, CH_TEXT, qtrue );
	return enabled ? CH_Clicked( x, y, w, CH_CTRL_H ) : qfalse;
}

static void CH_Act( const char *what, int id, const char *path, const std::string &fields ) {
	if ( chActHttp.state == HTTP_BUSY ) {
		CH_Message( "Wait: the last one is still going", qtrue );
		return;
	}
	Q_strncpyz( chActWhat, what, sizeof( chActWhat ) );
	chActId = id;
	CH_Post( &chActHttp, path, fields );
}

// a name typed in, for a new one, a new name, or to delete one
static qboolean CH_DrawField( float x, float y, float w, const char *hint ) {
	wgStyle_t style;

	memset( &style, 0, sizeof( style ) );
	style.font = ch.font;
	style.scale = CH_TEXT;
	style.lineH = 13.0f;
	style.padX = 4;
	style.padY = 2;
	style.fill = chField;
	style.border = chBorder;
	style.text = chWhite;
	style.dim = chDim;
	CH_Box( x, y, w, CH_CTRL_H, chField );
	CH_Fill( x, y + CH_CTRL_H - 1, w, 1, chOrange );
	WG_Field( &style, x, y, w, CH_CTRL_H, ch.field, qtrue, hint, ch.cursorX, ch.cursorY );
	if ( CH_Clicked( x, y, w, CH_CTRL_H ) )
		WG_FieldClick( ch.field, ch.clickX, ch.clickY );
	return qtrue;
}

static void CH_Type( int what, const char *start ) {
	ch.typing = what;
	Q_strncpyz( ch.field, start, sizeof( ch.field ) );
	WG_EditReset();
}

static qboolean CH_IsMain( const chChar_t *c ) {
	return (qboolean)( c && !Q_stricmp( c->tag, "main" ) );
}

static qboolean CH_IsExtra( const chChar_t *c ) {
	return (qboolean)( c && !Q_stricmp( c->tag, "extra" ) );
}

// a tag as shown
static const char *CH_TagLabel( const char *tag ) {
	return !Q_stricmp( tag, "main" ) ? "Main" : !Q_stricmp( tag, "extra" ) ? "Extra" : !Q_stricmp( tag, "secondary" ) ? "Secondary" : "";
}

static void CH_Check( float x, float y, float w, const char *label, qboolean on, qboolean enabled ) {
	CH_Box( x, y + 1, 12, 12, chField );
	if ( on )
		CH_Fill( x + 3, y + 4, 6, 6, enabled ? chOrange : chDim );
	CH_Text( x + 18, y, w - 18, label, enabled ? chWhite : chDim, CH_SMALL, qfalse );
}

static void CH_DrawList( void ) {
	const float x = CH_A_X, w = CH_A_W;
	float y = CH_TOP;
	const chChar_t *sel = CH_Find( ch.sel );

	CH_Text( x, y, w, va( "Your characters (%i of %i)", ch.num, CH_MAX ), chOrange, CH_TEXT, qfalse );
	y += 16;
	for ( int i = 0; i < CH_MAX; i++, y += CH_ROW_H ) {
		const chChar_t *c = &ch.chars[i];
		qboolean on, hover;

		if ( i >= ch.num ) {
			CH_Box( x, y, w, CH_ROW_H - 2, chField );
			continue;
		}
		on = (qboolean)( c->id == ch.sel );
		hover = CH_InRect( x, y, w, CH_ROW_H - 2 );
		CH_Box( x, y, w, CH_ROW_H - 2, on ? chItemOn : hover ? chItemHover : chItem );
		CH_Text( x + 4, y + 4, w - 100, c->name, on || hover ? chWhite : chOrange, CH_TEXT, qfalse );
		if ( c->tag[0] && Q_stricmp( c->tag, "secondary" ) )
			CH_Text( x + w - 96, y + 5, 40, CH_TagLabel( c->tag ), CH_IsMain( c ) ? chOrange : chDim, CH_SMALL, qfalse );
		if ( c->active )
			CH_Text( x + w - 56, y + 5, 52, "playing", chGreen, CH_SMALL, qfalse );
		if ( CH_Clicked( x, y, w, CH_ROW_H - 2 ) && ch.sel != c->id ) {
			ch.sel = c->id;
			ch.typing = CH_TYPE_NONE;
			if ( ch.side != CH_SIDE_SHARE )
				ch.side = CH_SIDE_SUITE;
			CH_AskData( c->id );
		}
	}
	y += 4;

	// a name being typed: what it's for, and its buttons
	if ( ch.typing == CH_TYPE_NEW ) {
		CH_Text( x, y, w, "A new character, called:", chDim, CH_SMALL, qfalse );
		CH_DrawField( x, y + 12, w, "its name" );
		y += 12 + CH_CTRL_H + 4;
		{
			const float bw = ( w - 4 ) * 0.5f;

			if ( CH_Button( x, y, bw, "Secondary", (qboolean)!ch.newExtra, qtrue ) )
				ch.newExtra = qfalse;
			if ( CH_Button( x + bw + 4, y, bw, "Extra", ch.newExtra, qtrue ) )
				ch.newExtra = qtrue;
			y += CH_CTRL_H + 1;
			CH_Text( x, y, w, ch.newExtra ? "For missions: can be shared, items too" : "Yours alone: never shared", chDim, CH_SMALL, qfalse );
			y += 13;
		}
		if ( CH_Button( x, y, w, "From what I wear and use now", qfalse, (qboolean)( ch.field[0] != 0 ) ) ) {
			CH_Act( "create", 0, "/v2/chars/create", CH_Str( "name", ch.field ) + CH_Str( "tag", ch.newExtra ? "extra" : "secondary" )
				+ ",\"data\":" + CH_Snapshot( -1 ) );
			ch.typing = CH_TYPE_NONE;
		}
		y += CH_CTRL_H + 3;
		if ( CH_Button( x, y, w, "Only my look", qfalse, (qboolean)( ch.field[0] != 0 ) ) ) {
			CH_Act( "create", 0, "/v2/chars/create", CH_Str( "name", ch.field ) + CH_Str( "tag", ch.newExtra ? "extra" : "secondary" )
				+ ",\"data\":" + CH_LookOnly() );
			ch.typing = CH_TYPE_NONE;
		}
		y += CH_CTRL_H + 3;
		if ( CH_Button( x, y, w, "Cancel", qfalse, qtrue ) )
			ch.typing = CH_TYPE_NONE;
		return;
	}
	if ( ch.typing == CH_TYPE_RENAME && sel ) {
		CH_Text( x, y, w, va( "%s" S_COLOR_WHITE ", called from now on:", sel->name ), chDim, CH_SMALL, qfalse );
		CH_DrawField( x, y + 12, w, "its new name" );
		y += 12 + CH_CTRL_H + 4;
		if ( CH_Button( x, y, ( w - 4 ) * 0.5f, "Rename", qfalse, (qboolean)( ch.field[0] != 0 ) ) ) {
			CH_Act( "rename", sel->id, "/v2/chars/update", va( ",\"id\":%i", sel->id ) + CH_Str( "name", ch.field ) );
			ch.typing = CH_TYPE_NONE;
		}
		if ( CH_Button( x + ( w + 4 ) * 0.5f, y, ( w - 4 ) * 0.5f, "Cancel", qfalse, qtrue ) )
			ch.typing = CH_TYPE_NONE;
		return;
	}
	if ( ch.typing == CH_TYPE_DELETE && sel ) {
		const qboolean same = (qboolean)( !Q_stricmp( ch.field, sel->name ) );

		CH_Text( x, y, w, S_COLOR_YELLOW "Its items, panel and holster go too.", chWhite, CH_SMALL, qfalse );
		CH_Text( x, y + 11, w, va( "Type %s" S_COLOR_WHITE " to delete it:", sel->name ), chDim, CH_SMALL, qfalse );
		CH_DrawField( x, y + 23, w, "its name" );
		y += 23 + CH_CTRL_H + 4;
		if ( CH_Button( x, y, ( w - 4 ) * 0.5f, "Delete", qfalse, same ) ) {
			CH_Act( "delete", sel->id, "/v2/chars/delete", va( ",\"id\":%i", sel->id ) );
			ch.typing = CH_TYPE_NONE;
		}
		if ( CH_Button( x + ( w + 4 ) * 0.5f, y, ( w - 4 ) * 0.5f, "Cancel", qfalse, qtrue ) )
			ch.typing = CH_TYPE_NONE;
		return;
	}
	{
		const float bw = ( w - 8 ) / 3.0f;

		if ( CH_Button( x, y, bw, "New", qfalse, (qboolean)( ch.num < CH_MAX ) ) ) {
			CH_Type( CH_TYPE_NEW, "" );
			ch.newExtra = qfalse;
		}
		if ( CH_Button( x + bw + 4, y, bw, "Rename", qfalse, (qboolean)( sel != NULL ) ) )
			CH_Type( CH_TYPE_RENAME, sel->name );
		if ( CH_Button( x + 2 * ( bw + 4 ), y, bw, "Delete", qfalse, (qboolean)( sel && ch.num > 1 && !CH_IsMain( sel ) ) ) )
			CH_Type( CH_TYPE_DELETE, "" );
	}
	y += CH_CTRL_H + 10;

	// copies others offered you: taken as a new character of yours, or turned down
	if ( ch.numOffers ) {
		CH_Text( x, y, w, va( "Offered to you (%i)", ch.numOffers ), chOrange, CH_TEXT, qfalse );
		y += 15;
		for ( int i = 0; i < ch.numOffers && y + 30 <= CH_Y + CH_H - 24; i++, y += 34 ) {
			const chOffer_t *o = &ch.offers[i];
			const qboolean busy = (qboolean)( chOfferActHttp.state == HTTP_BUSY );

			CH_Box( x, y, w, 32, chItem );
			CH_Text( x + 4, y + 2, w - 8, va( "%s" S_COLOR_GREY "  from %s" S_COLOR_GREY "%s", o->name, o->from,
				o->items ? va( ", %i item%s", o->items, o->items == 1 ? "" : "s" ) : "" ), chWhite, CH_SMALL, qfalse );
			if ( CH_Button( x + 4, y + 14, ( w - 12 ) * 0.5f, "Take it", qfalse, (qboolean)( !busy && ch.num < CH_MAX ) ) )
				CH_Post( &chOfferActHttp, "/v2/chars/accept", va( ",\"offer\":%i", o->id ) );
			if ( CH_Button( x + 8 + ( w - 12 ) * 0.5f, y + 14, ( w - 12 ) * 0.5f, "No thanks", qfalse, (qboolean)!busy ) )
				CH_Post( &chOfferActHttp, "/v2/chars/decline", va( ",\"offer\":%i", o->id ) );
		}
	}
}

// the picked one: its look turning, and playing it
static void CH_DrawPicked( void ) {
	const float x = CH_B_X, w = CH_B_W, h = 180;
	const chChar_t *sel = CH_Find( ch.sel );
	float y = CH_TOP;

	CH_Box( x, y, w, h, chField );
	if ( !sel ) {
		CH_Text( x, y + h * 0.5f - 6, w, "Pick a character", chDim, CH_TEXT, qtrue );
		return;
	}
	if ( ch.data.id != sel->id ) {
		CH_Text( x, y + h * 0.5f - 6, w, "Asking C1B3R...", chDim, CH_TEXT, qtrue );
	} else {
		const int m = CH_KeyIndex( "model" ), s1 = CH_KeyIndex( "saber1" ), s2 = CH_KeyIndex( "saber2" );
		const char *model = m >= 0 && ch.data.look[m].size() ? ch.data.look[m].c_str() : NULL;

		if ( model ) {
			const char *saber1 = s1 >= 0 ? ch.data.look[s1].c_str() : "", *saber2 = s2 >= 0 ? ch.data.look[s2].c_str() : "";

			CL_Presets_DrawCharacter( model, saber1, saber2, NULL, NULL, NULL, x + 1, y + 1, w - 2, h - 2,
				ch.yaw + (float)( cls.realtime % 24000 ) * 0.015f, NULL, NULL );
			if ( CL_Presets_CharacterError() )
				CH_Text( x, y + h - 26, w, CL_Presets_CharacterError(), chYellow, CH_SMALL, qtrue );
		} else {
			CH_Text( x, y + h * 0.5f - 6, w, "No look kept yet", chDim, CH_TEXT, qtrue );
		}
	}
	y += h + 6;
	CH_Text( x, y, w, sel->name, chOrange, CH_TEXT + 0.1f, qtrue );
	y += 18;
	if ( sel->active ) {
		CH_Text( x, y + 2, w, "You play this one", chGreen, CH_TEXT, qtrue );
		if ( !CH_Held( sel->id ) && ch.data.id == sel->id ) {
			CH_Text( x, y + 18, w, S_COLOR_YELLOW "This game's settings aren't its yet:", chWhite, CH_SMALL, qtrue );
			if ( CH_Button( x, y + 31, w, "Put its settings on me", qfalse, (qboolean)( chActHttp.state != HTTP_BUSY ) ) ) {
				CH_Apply( ch.data );
				CH_Hold( sel->id );
				CH_Message( va( "%s" S_COLOR_GREEN "'s settings are on you now", sel->name ), qfalse );
			}
		}
	} else if ( CH_Button( x, y, w, "Play as this character", qfalse, (qboolean)( chActHttp.state != HTTP_BUSY ) ) ) {
		// what's changed on the one played kept first
		if ( CH_Held( ch.activeId ) && chSyncHttp.state != HTTP_BUSY )
			CH_Post( &chSyncHttp, "/v2/chars/update", va( ",\"id\":%i,\"data\":%s", ch.activeId, CH_Snapshot( ch.activeId ).c_str() ) );
		CH_Act( "select", sel->id, "/v2/chars/select", va( ",\"id\":%i", sel->id ) );
	}

	// its tag, and sharing a copy
	y = CH_TOP + h + 6 + 18 + 50;
	// main: the first, its own; secondary: its own too; extra: for missions, shared with its items
	if ( CH_IsMain( sel ) || !sel->tag[0] ) {
		CH_Text( x, y + 2, w, sel->tag[0] ? "Tag: " S_COLOR_WHITE "Main" S_COLOR_GREY "  your first, never shared" : "Tag: " S_COLOR_GREY "unknown",
			chOrange, CH_SMALL, qfalse );
	} else {
		const float bw = ( w - 34 - 4 ) * 0.5f;
		const qboolean extra = CH_IsExtra( sel ), busy = (qboolean)( chActHttp.state == HTTP_BUSY );

		CH_Text( x, y + 2, 32, "Tag:", chOrange, CH_SMALL, qfalse );
		if ( CH_Button( x + 34, y, bw, "Secondary", (qboolean)!extra, (qboolean)( extra && !busy ) ) )
			CH_Act( "tag", sel->id, "/v2/chars/tag", va( ",\"id\":%i", sel->id ) + CH_Str( "tag", "secondary" ) );
		if ( CH_Button( x + 34 + bw + 4, y, bw, "Extra", extra, (qboolean)( !extra && !busy ) ) )
			CH_Act( "tag", sel->id, "/v2/chars/tag", va( ",\"id\":%i", sel->id ) + CH_Str( "tag", "extra" ) );
	}
	y += CH_CTRL_H + 4;
	if ( CH_IsExtra( sel ) ) {
		if ( CH_Button( x, y, w, va( "Share a copy with players%s", ch.side == CH_SIDE_SHARE ? "..." : "" ), (qboolean)( ch.side == CH_SIDE_SHARE ), qtrue ) ) {
			ch.side = ch.side == CH_SIDE_SHARE ? CH_SIDE_SUITE : CH_SIDE_SHARE;
			memset( ch.shareSaid, 0, sizeof( ch.shareSaid ) );
			ch.shareScroll = 0;
		}
	} else {
		if ( ch.side == CH_SIDE_SHARE )
			ch.side = CH_SIDE_SUITE;
		CH_Text( x, y + 2, w, "Only extra characters can be shared", chDim, CH_SMALL, qtrue );
	}
	y += CH_CTRL_H + 5;

	// kept from Inspect or not: changed on the one played (its settings are this game's)
	if ( ch.data.id == sel->id ) {
		const qboolean held = (qboolean)( sel->active && CH_Held( sel->id ) );
		const qboolean on = held ? (qboolean)( Cvar_VariableIntegerValue( "cl_inspectOthers" ) != 0 ) : (qboolean)( !ch.data.hasInspect || ch.data.inspect );

		CH_Check( x, y, w, held ? "Others can Inspect this character" : "Others can Inspect it " S_COLOR_GREY "(play as it to change)", on, held );
		if ( held && CH_Clicked( x, y, w, 14 ) )
			Cvar_Set( "cl_inspectOthers", on ? "0" : "1" );		// sent with its settings within seconds
	}
}

// a tool for the character played
static void CH_Tool( float x, float *y, float w, const char *label, const char *cmd, qboolean enabled ) {
	if ( CH_Button( x, *y, w, label, qfalse, enabled ) ) {
		CL_Characters_Close();
		Cbuf_AddText( va( "%s\n", cmd ) );
	}
	*y += CH_CTRL_H + 3;
}

#define CH_SHARE_ROW		18.0f
#define CH_SHARE_ROWS		13

// the players a copy can go to: the one aimed at first, then the rest; you left out
static int CH_SharePlayers( int *out ) {
	const int aimed = CL_AimedPlayer( 1024.0f );
	int n = 0;

	if ( aimed >= 0 && aimed != clc.clientNum && CL_PlayerSlotName( aimed )[0] )
		out[n++] = aimed;
	for ( int c = 0; c < MAX_CLIENTS; c++ ) {
		if ( c != clc.clientNum && c != aimed && CL_PlayerSlotName( c )[0] )
			out[n++] = c;
	}
	return n;
}

static void CH_ShareSend( int id ) {
	std::string fields = va( ",\"id\":%i,\"clients\":[", id );
	int n = 0;

	for ( int c = 0; c < MAX_CLIENTS; c++ ) {
		if ( !ch.picked[c] || !CL_PlayerSlotName( c )[0] )
			continue;
		fields += va( "%s%i", n ? "," : "", c );
		n++;
		ch.shareSaid[c][0] = '\0';
	}
	fields += "]";
	if ( !n ) {
		CH_Message( "Tick the players first", qtrue );
		return;
	}
	if ( n > CH_SHARE_MAX ) {
		CH_Message( va( "At most %i at once", CH_SHARE_MAX ), qtrue );
		return;
	}
	if ( !CH_Post( &chShareHttp, "/v2/chars/share", fields ) )
		CH_Message( "Wait: still sending", qtrue );
}

// sharing a copy of the picked one: players ticked, then sent; each says yes or no to it
static void CH_DrawShare( const chChar_t *sel ) {
	const float x = CH_C_X, w = CH_C_W;
	int players[MAX_CLIENTS], n, picked = 0, first;
	float y = CH_TOP;

	n = CH_SharePlayers( players );
	for ( int c = 0; c < MAX_CLIENTS; c++ ) {
		if ( ch.picked[c] && !CL_PlayerSlotName( c )[0] )
			ch.picked[c] = qfalse;		// they left
		picked += ch.picked[c] ? 1 : 0;
	}
	CH_Text( x, y, w, va( "Share a copy of %s", sel->name ), chOrange, CH_TEXT, qfalse );
	y += 14;
	CH_Text( x, y, w, "Its look, settings, panel, holster and items.", chDim, CH_SMALL, qfalse );
	y += 16;

	ch.shareScroll = Com_Clamp( 0, (float)Q_max( 0, n - CH_SHARE_ROWS ), ch.shareScroll );
	first = (int)ch.shareScroll;
	if ( !n )
		CH_Text( x, y + 30, w, "Nobody else is here", chDim, CH_TEXT, qtrue );
	for ( int i = first; i < n && i < first + CH_SHARE_ROWS; i++ ) {
		const int c = players[i];
		const float ry = y + ( i - first ) * CH_SHARE_ROW;
		const qboolean linked = CL_Presence_Linked( c ), hover = CH_InRect( x, ry, w, CH_SHARE_ROW - 2 );

		CH_Box( x, ry, w, CH_SHARE_ROW - 2, ch.picked[c] ? chItemOn : hover && linked ? chItemHover : chItem );
		// the box ticked
		CH_Box( x + 3, ry + 3, 10, 10, chField );
		if ( ch.picked[c] )
			CH_Fill( x + 5, ry + 5, 6, 6, chOrange );
		CH_Text( x + 18, ry + 2, w * 0.55f, va( "%s%s", CL_PlayerSlotName( c ), i == 0 && c == CL_AimedPlayer( 1024.0f ) ? S_COLOR_GREY " (aimed)" : "" ),
			linked ? chWhite : chDim, CH_SMALL, qfalse );
		CH_Text( x + w * 0.55f + 22, ry + 2, w * 0.45f - 24, ch.shareSaid[c][0] ? ch.shareSaid[c] : linked ? "" : S_COLOR_GREY "no C1B3R link",
			chWhite, CH_SMALL, qfalse );
		if ( linked && CH_Clicked( x, ry, w, CH_SHARE_ROW - 2 ) )
			ch.picked[c] = (qboolean)!ch.picked[c];
	}
	if ( n > CH_SHARE_ROWS )
		CH_Text( x, y + CH_SHARE_ROWS * CH_SHARE_ROW, w, va( "%i-%i of %i: the wheel scrolls", first + 1, Q_min( n, first + CH_SHARE_ROWS ), n ),
			chDim, CH_SMALL, qtrue );

	y = CH_Y + CH_H - 48;
	{
		const float bw = ( w - 8 ) / 3.0f;

		if ( CH_Button( x, y, bw, "All linked", qfalse, qtrue ) ) {
			for ( int i = 0; i < n; i++ )
				ch.picked[players[i]] = CL_Presence_Linked( players[i] );
		}
		if ( CH_Button( x + bw + 4, y, bw, "None", qfalse, (qboolean)( picked > 0 ) ) )
			memset( ch.picked, 0, sizeof( ch.picked ) );
		if ( CH_Button( x + 2 * ( bw + 4 ), y, bw, chShareHttp.state == HTTP_BUSY ? "Sending..." : va( "Send to %i", picked ), qtrue,
			(qboolean)( picked > 0 && chShareHttp.state != HTTP_BUSY ) ) )
			CH_ShareSend( sel->id );
	}
}

// what the picked one keeps, and the tools for each part
static void CH_DrawSuite( void ) {
	const float x = CH_C_X, w = CH_C_W;
	const chChar_t *sel = CH_Find( ch.sel );
	const qboolean playing = (qboolean)( sel && sel->active );
	float y = CH_TOP;

	if ( sel && ch.side == CH_SIDE_SHARE ) {
		CH_DrawShare( sel );
		return;
	}

	CH_Text( x, y, w, "What it keeps", chOrange, CH_TEXT, qfalse );
	y += 16;
	if ( sel && ch.data.id == sel->id ) {
		const int m = CH_KeyIndex( "model" ), s1 = CH_KeyIndex( "saber1" );
		const chData_t &d = ch.data;

		CH_Text( x, y, w, va( "Look: %s", m >= 0 && d.look[m].size() ? d.look[m].c_str() : S_COLOR_GREY "none" ), chWhite, CH_SMALL, qfalse );
		CH_Text( x, y + 11, w, va( "Saber: %s", s1 >= 0 && d.look[s1].size() ? d.look[s1].c_str() : S_COLOR_GREY "none" ), chWhite, CH_SMALL, qfalse );
		CH_Text( x, y + 22, w, va( "%i presets, %i effect swaps, %i player shaders", (int)d.presets.size(), (int)d.fxFrom.size(),
			(int)d.shFrom.size() ), chWhite, CH_SMALL, qfalse );
		CH_Text( x, y + 33, w, va( "Language: %s", d.language.size() ? d.language.c_str() : S_COLOR_GREY "none" ), chWhite, CH_SMALL, qfalse );
		CH_Text( x, y + 44, w, "Its own inventory, panel and holster", chDim, CH_SMALL, qfalse );
	}
	y += 62;

	CH_Text( x, y, w, playing ? "Change it" : "Play as it to change it", chOrange, CH_TEXT, qfalse );
	y += 16;
	if ( CH_Button( x, y, w, "Keep what I wear and use now", qfalse, (qboolean)( playing && chActHttp.state != HTTP_BUSY ) ) )
		CH_Act( "capture", sel->id, "/v2/chars/update", va( ",\"id\":%i,\"data\":%s", sel->id, CH_Snapshot( sel->id ).c_str() ) );
	y += CH_CTRL_H + 8;
	CH_Tool( x, &y, w, "Look and presets", "presets", playing );
	CH_Tool( x, &y, w, "Inventory", "inventory", playing );
	CH_Tool( x, &y, w, "Character panel (Inspect)", "inspect edit", playing );
	CH_Tool( x, &y, w, "Holster", "holster", playing );
	CH_Tool( x, &y, w, "Datapad settings", "datapad settings", playing );
	CH_Tool( x, &y, w, "Effect Replacer", "effectreplacer", playing );
	CH_Tool( x, &y, w, "Player shaders (Shader Manager)", "shadermanager", playing );
	CH_Tool( x, &y, w, "Languages", "languages", playing );
}

// none yet: the first set up, named as you are now (or as typed), its look what you wear now
static void CH_DrawSetup( void ) {
	const float pw = 170, ph = 230, fw = 260, gap = 24;
	const float px = CH_X + ( CH_W - pw - gap - fw ) * 0.5f, fx = px + pw + gap;
	const qboolean busy = (qboolean)( chActHttp.state == HTTP_BUSY );
	float y = CH_Y + 60;

	if ( ch.typing != CH_TYPE_SETUP )
		CH_Type( CH_TYPE_SETUP, Cvar_VariableString( "name" ) );		// colours and all
	CH_Text( CH_X, y, CH_W, "Set up your character", chOrange, 1.0f, qtrue );
	y += 28;
	CH_Text( CH_X, y, CH_W, "Your game is linked, and you have no character yet. Your first is your main one:", chDim, CH_TEXT, qtrue );
	CH_Text( CH_X, y + 14, CH_W, "it gets the lightsaber hilt and the starter set, and its own inventory, holster and panel.", chDim, CH_TEXT, qtrue );
	y += 44;

	CH_Box( px, y, pw, ph, chItem );
	CL_Presets_DrawPortrait( px + 1, y + 1, pw - 2, ph - 2, ch.yaw );
	if ( CH_Clicked( px, y, pw, ph ) )
		ch.yaw += 45.0f;

	CH_Text( fx, y, fw, "Its name", chOrange, CH_TEXT, qfalse );
	CH_Text( fx, y + 15, fw, "As you're called now: change it if you like", chDim, CH_SMALL, qfalse );
	CH_DrawField( fx, y + 28, fw, "its name" );
	CH_Text( fx, y + 28 + CH_CTRL_H + 4, 60, "Shown as", chDim, CH_SMALL, qfalse );
	CH_Text( fx + 60, y + 28 + CH_CTRL_H + 2, fw - 60, ch.field, chWhite, CH_TEXT, qfalse );
	y += 18;
	CH_Text( fx, y + 28 + CH_CTRL_H + 10, fw, "Its look: what you wear now (left),", chDim, CH_SMALL, qfalse );
	CH_Text( fx, y + 28 + CH_CTRL_H + 21, fw, "kept as its first preset.", chDim, CH_SMALL, qfalse );
	if ( ( CH_Button( fx, y + 28 + CH_CTRL_H + 42, fw, busy ? "Setting it up..." : "Create", qfalse, (qboolean)( ch.field[0] && !busy ) )
		|| ( ch.setupGo && ch.field[0] && !busy ) ) )
		CH_Act( "setup", 0, "/v2/chars/create", CH_Str( "name", ch.field ) + ",\"data\":" + CH_Snapshot( -1 ) );
	ch.setupGo = qfalse;
	CH_Text( fx, y + 28 + 2 * CH_CTRL_H + 52, fw, "More characters come later, from this window.", chDim, CH_SMALL, qfalse );
}

void CL_Characters_Draw( void ) {
	if ( !ch.open )
		return;
	CH_Box( CH_X, CH_Y, CH_W, CH_H, chPanel );
	CH_Text( CH_A_X, CH_Y + 8, 200, "CHARACTERS", chOrange, 1.0f, qfalse );
	CH_Text( CH_A_X + 130, CH_Y + 12, 340, "Each its own look, inventory, holster, panel and settings", chDim, CH_SMALL, qfalse );
	if ( CH_Button( CH_X + CH_W - 12 - 56, CH_Y + 10, 56, "Close", qfalse, qtrue ) ) {
		CL_Characters_Close();
		return;
	}
	if ( !CL_Presence_Session() || ( !ch.known && ch.refusal[0] ) ) {
		CH_Text( CH_X, CH_Y + 180, CH_W, !CL_Presence_Session() ? "Not connected to C1B3R" : "Your characters need your game linked",
			chOrange, 1.0f, qtrue );
		CH_Text( CH_X, CH_Y + 206, CH_W, !CL_Presence_Session() ? "They're kept there: /c1b3r says why it isn't connected" : ch.refusal,
			chDim, CH_TEXT, qtrue );
	} else if ( !ch.known ) {
		CH_Text( CH_X, CH_Y + 190, CH_W, "Asking C1B3R...", chDim, CH_TEXT, qtrue );
	} else if ( !ch.num ) {
		CH_DrawSetup();
	} else {
		CH_DrawList();
		CH_DrawPicked();
		CH_DrawSuite();
	}
	if ( ch.message[0] && cls.realtime - ch.messageTime < CH_MESSAGE_MS )
		CH_Text( CH_A_X, CH_Y + CH_H - 20, CH_W - 24, ch.message, ch.messageBad ? chYellow : chGreen, CH_TEXT, qfalse );
	ch.click = qfalse;
	WG_DrawOverlay();
	re->SetColor( NULL );
	re->DrawStretchPic( ch.cursorX, ch.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

/*
===============================================================================

Opening, closing, keys

===============================================================================
*/

static void CH_Open( void );

// a tool that's a character's (inventory, holster, panel): when the game's linked and has none yet,
// said, and the setup opened; qtrue then (the tool stays shut)
qboolean CL_Characters_NeedSetup( const char *tool ) {
	if ( !CL_Presence_Session() || !ch.known || ch.num )
		return qfalse;
	Com_Printf( "%s: set up your character first\n", tool );
	if ( !ch.open )
		CH_Open();
	return qtrue;
}

void CL_Characters_Close( void ) {
	if ( !ch.open )
		return;
	ch.open = qfalse;
	ch.typing = CH_TYPE_NONE;
	ch.side = CH_SIDE_SUITE;
	CL_Presets_FreeCharacter();
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_CHARACTERS );
}

static void CH_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Characters: join a server first\n" );
		return;
	}
	if ( ch.open )
		return;
	CL_ModelManager_Close();
	CL_NpcManager_Close();
	CL_ShaderManager_Close();
	CL_EffectManager_Close();
	CL_Storyteller_Close();
	CL_RpCinematic_EditorClose();
	CL_MapEditor_Close();
	CL_SoundManager_Close();
	CL_AnimManager_Close();
	CL_Presets_Close();
	CL_AssetManager_Close();
	CL_RadialMenu_CloseSettings();
	CL_Languages_Close();
	CL_SpecialMoves_Close();
	CL_ChatLog_Close();
	CL_Inventory_Close();
	CL_Inspect_Close();
	CL_Holster_Close();
	CL_Datapad_Close();

	ch.font = re->RegisterFont( "arialnb" );
	if ( !ch.font )
		ch.font = cls.menuFont;
	ch.titleFont = re->RegisterFont( "ergoec" );
	ch.clickSound = S_RegisterSound( "sound/interface/button1.wav" );
	ch.cursorX = SCREEN_WIDTH * 0.5f;
	ch.cursorY = SCREEN_HEIGHT * 0.5f;
	ch.click = qfalse;
	ch.typing = CH_TYPE_NONE;
	ch.yaw = 160.0f;
	ch.data.id = 0;
	ch.open = qtrue;
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_CHARACTERS );
	CH_AskList();
	CH_AskOffers();
}

static qboolean CH_IsToggleKey( int key ) {
	const char *binding = Key_GetBinding( key );

	return (qboolean)( VALIDSTRING( binding ) && ( !Q_stricmp( binding, CH_CMD ) || !Q_stricmp( binding, "character" ) ) );
}

void CL_Characters_KeyEvent( int key, qboolean down ) {
	if ( !ch.open || !down )
		return;
	if ( key == A_MOUSE1 ) {
		ch.click = qtrue;
		ch.clickX = ch.cursorX;
		ch.clickY = ch.cursorY;
		return;
	}
	if ( ch.typing ) {
		if ( key == A_ENTER || key == A_KP_ENTER ) {
			ch.setupGo = (qboolean)( ch.typing == CH_TYPE_SETUP );
			return;		// the buttons say what it's for (setting up: Create)
		}
		WG_EditKey( ch.field, sizeof( ch.field ), key, NULL );
		return;
	}
	if ( ch.side == CH_SIDE_SHARE && ( key == A_MWHEELUP || key == A_MWHEELDOWN ) ) {
		ch.shareScroll += key == A_MWHEELUP ? -3 : 3;		// held in range as it's drawn
		return;
	}
	if ( CH_IsToggleKey( key ) )
		CL_Characters_Close();
}

void CL_Characters_CharEvent( int ch_ ) {
	if ( ch.open && ch.typing )
		WG_EditChar( ch.field, sizeof( ch.field ), ch_, NULL );
}

void CL_Characters_MouseEvent( int dx, int dy ) {
	ch.cursorX = Com_Clamp( 0, SCREEN_WIDTH, ch.cursorX + dx );
	ch.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, ch.cursorY + dy );
}

// the name typed in first, then sharing, then closed
void CL_Characters_Escape( void ) {
	if ( ch.typing == CH_TYPE_SETUP ) {
		CL_Characters_Close();
		return;
	}
	if ( ch.typing ) {
		ch.typing = CH_TYPE_NONE;
		return;
	}
	if ( ch.side != CH_SIDE_SUITE ) {
		ch.side = CH_SIDE_SUITE;
		return;
	}
	CL_Characters_Close();
}

void CL_Characters_Disconnect( void ) {
	CL_Characters_Close();
	ch.known = qfalse;
	ch.num = ch.activeId = ch.sel = 0;
	ch.listFor[0] = '\0';
	ch.data.id = 0;
	ch.applyId = 0;
	ch.numOffers = 0;
	memset( ch.picked, 0, sizeof( ch.picked ) );
	ch.lastSent.clear();
}

// a hook said the account's characters changed (cmd:char for this player): the list asked again
void CL_Characters_CharHook( char **argv, int argc ) {
	if ( argc >= 1 && atoi( argv[0] ) == clc.clientNum && CL_Presence_Session() )
		CH_AskList();
}

// cmd:charoffer/<client>/<offer>: someone offers this player a copy of their character
void CL_Characters_OfferHook( char **argv, int argc ) {
	if ( argc < 1 || atoi( argv[0] ) != clc.clientNum || !CL_Presence_Session() )
		return;
	Com_Printf( S_COLOR_CYAN "Characters: " S_COLOR_WHITE "a copy of a character is offered to you: /" CH_CMD " to take it or not\n" );
	CH_AskOffers();
}

static void CH_Characters_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !arg[0] ) {
		if ( ch.open )
			CL_Characters_Close();
		else
			CH_Open();
		return;
	}
	if ( !Q_stricmp( arg, "list" ) ) {
		if ( !ch.known ) {
			Com_Printf( "Characters: not known yet%s%s\n", ch.refusal[0] ? ": " : "", ch.refusal );
			return;
		}
		for ( int i = 0; i < ch.num; i++ )
			Com_Printf( "%2i  %s" S_COLOR_WHITE "%s%s\n", ch.chars[i].id, ch.chars[i].name, ch.chars[i].active ? S_COLOR_GREEN "  (playing)" : "",
				ch.chars[i].active && !CH_Held( ch.chars[i].id ) ? S_COLOR_YELLOW "  this game's settings aren't its" : "" );
		return;
	}
	if ( !Q_stricmp( arg, "play" ) && Cmd_Argc() >= 3 ) {
		const char *name = Cmd_ArgsFrom( 2 );

		for ( int i = 0; i < ch.num; i++ ) {
			if ( !Q_stricmp( ch.chars[i].name, name ) || ( Q_isanumber( name ) && atoi( name ) == ch.chars[i].id ) ) {
				if ( CH_Held( ch.activeId ) && chSyncHttp.state != HTTP_BUSY )
					CH_Post( &chSyncHttp, "/v2/chars/update", va( ",\"id\":%i,\"data\":%s", ch.activeId, CH_Snapshot( ch.activeId ).c_str() ) );
				CH_Act( "select", ch.chars[i].id, "/v2/chars/select", va( ",\"id\":%i", ch.chars[i].id ) );
				return;
			}
		}
		Com_Printf( "Characters: no character %s (" CH_CMD " list)\n", name );
		return;
	}
	if ( !Q_stricmp( arg, "offers" ) ) {
		if ( !ch.numOffers )
			Com_Printf( "Characters: no copies offered to you (asked again)\n" );
		for ( int i = 0; i < ch.numOffers; i++ )
			Com_Printf( "%i  %s" S_COLOR_WHITE "  from %s" S_COLOR_WHITE ", %i items\n", i + 1, ch.offers[i].name, ch.offers[i].from, ch.offers[i].items );
		CH_AskOffers();
		return;
	}
	if ( ( !Q_stricmp( arg, "accept" ) || !Q_stricmp( arg, "decline" ) ) && Cmd_Argc() >= 3 ) {
		const int n = atoi( Cmd_Argv( 2 ) ) - 1;

		if ( n < 0 || n >= ch.numOffers ) {
			Com_Printf( "Characters: no offer %s (" CH_CMD " offers)\n", Cmd_Argv( 2 ) );
			return;
		}
		if ( !CH_Post( &chOfferActHttp, !Q_stricmp( arg, "accept" ) ? "/v2/chars/accept" : "/v2/chars/decline", va( ",\"offer\":%i", ch.offers[n].id ) ) )
			Com_Printf( "Characters: wait, still answering the last one\n" );
		return;
	}
	Com_Printf( "usage: " CH_CMD "                  opens your characters, or closes them\n" );
	Com_Printf( "       " CH_CMD " list             lists them\n" );
	Com_Printf( "       " CH_CMD " play <name>      plays one: its look, settings, inventory, panel and holster\n" );
	Com_Printf( "       " CH_CMD " offers           copies of characters offered to you\n" );
	Com_Printf( "       " CH_CMD " accept|decline <n>   takes one as a new character of yours, or turns it down\n" );
}

void CL_Characters_Init( void ) {
	cl_characterHeld = Cvar_Get( "cl_characterHeld", "", CVAR_ARCHIVE_ND | CVAR_INTERNAL,
		"The character (account/id) this game's look and settings are, kept up to date on C1B3R" );
	Cmd_AddCommand( CH_CMD, CH_Characters_f, "Your characters: each its own look, inventory, holster, panel and settings" );
	Cmd_AddCommand( "character", CH_Characters_f, "Your characters (the same as /characters)" );
}

void CL_Characters_Shutdown( void ) {
	CL_Characters_Close();
}
