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
#define CH_NAME_LEN			33
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
	{ "x", "cl_datapadX" }, { "y", "cl_datapadY" }, { "color", "cl_datapadColor" }, { "emote", "cl_datapadEmote" },
};

typedef struct {
	int			id;
	char		name[CH_NAME_LEN];
	qboolean	active;
} chChar_t;

typedef struct {
	std::string					title;
	qboolean					favorite;
	std::vector<std::string>	values;		// CL_Presets_Keys' order
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
	std::string					has;		// the parts it keeps, by comma ("" none: a new, empty one)
} chData_t;

enum { CH_TYPE_NONE, CH_TYPE_NEW, CH_TYPE_RENAME, CH_TYPE_DELETE };

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

	std::string	lastSent;		// what was sent for the character played, to send it only when changed
	int			nextSync;

	char		message[160];
	int			messageTime;
	qboolean	messageBad;
	float		yaw;
} ch;

static httpRequest_t chListHttp, chGetHttp, chActHttp, chSyncHttp;
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
			out += va( ",\"favorite\":%i,\"values\":{", favorite ? 1 : 0 );
			for ( int k = 0; k < numKeys; k++ ) {
				out += k ? ",\"" : "\"";
				out += keys[k];
				out += "\":";
				CL_Presence_Json( out, CL_Presets_Get( i, k, &date, &favorite, &character ) );
			}
			out += "}}";
		}
	}
	out += "],\"datapad\":{";
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
	out += "}";
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
		CL_Presets_Import( d.id, d.presets[i].title.c_str(), pv.data(), d.presets[i].favorite );
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
				if ( c->active )
					ch.activeId = c->id;
			} );
			ch.known = qtrue;
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
			CH_Apply( ch.data );
			CH_Hold( ch.applyId );
			ch.applyId = 0;
		}
	}
	if ( HTTP_Poll( &chActHttp ) ) {
		const qboolean ok = (qboolean)( chActHttp.status == 200 );

		if ( !ok ) {
			CH_Message( chActHttp.answer[0] ? chActHttp.answer : "C1B3R didn't answer", qtrue );
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
		CH_Text( x + 4, y + 4, w - 60, c->name, on || hover ? chWhite : chOrange, CH_TEXT, qfalse );
		if ( c->active )
			CH_Text( x + w - 56, y + 5, 52, "playing", chGreen, CH_SMALL, qfalse );
		if ( CH_Clicked( x, y, w, CH_ROW_H - 2 ) && ch.sel != c->id ) {
			ch.sel = c->id;
			ch.typing = CH_TYPE_NONE;
			CH_AskData( c->id );
		}
	}
	y += 4;

	// a name being typed: what it's for, and its buttons
	if ( ch.typing == CH_TYPE_NEW ) {
		CH_Text( x, y, w, "A new character, called:", chDim, CH_SMALL, qfalse );
		CH_DrawField( x, y + 12, w, "its name" );
		y += 12 + CH_CTRL_H + 4;
		if ( CH_Button( x, y, w, "From what I wear and use now", qfalse, (qboolean)( ch.field[0] != 0 ) ) ) {
			CH_Act( "create", 0, "/v2/chars/create", CH_Str( "name", ch.field ) + ",\"data\":" + CH_Snapshot( -1 ) );
			ch.typing = CH_TYPE_NONE;
		}
		y += CH_CTRL_H + 3;
		if ( CH_Button( x, y, w, "Only my look", qfalse, (qboolean)( ch.field[0] != 0 ) ) ) {
			CH_Act( "create", 0, "/v2/chars/create", CH_Str( "name", ch.field ) + ",\"data\":" + CH_LookOnly() );
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

		if ( CH_Button( x, y, bw, "New", qfalse, (qboolean)( ch.num < CH_MAX ) ) )
			CH_Type( CH_TYPE_NEW, "" );
		if ( CH_Button( x + bw + 4, y, bw, "Rename", qfalse, (qboolean)( sel != NULL ) ) )
			CH_Type( CH_TYPE_RENAME, sel->name );
		if ( CH_Button( x + 2 * ( bw + 4 ), y, bw, "Delete", qfalse, (qboolean)( sel && ch.num > 1 ) ) )
			CH_Type( CH_TYPE_DELETE, "" );
	}
}

// the picked one: its look turning, and playing it
static void CH_DrawPicked( void ) {
	const float x = CH_B_X, w = CH_B_W, h = 250;
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
			CL_Presets_DrawCharacter( model, s1 >= 0 ? ch.data.look[s1].c_str() : "", s2 >= 0 ? ch.data.look[s2].c_str() : "",
				NULL, NULL, NULL, x + 1, y + 1, w - 2, h - 2, ch.yaw + (float)( cls.realtime % 24000 ) * 0.015f, NULL, NULL );
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
}

// a tool for the character played
static void CH_Tool( float x, float *y, float w, const char *label, const char *cmd, qboolean enabled ) {
	if ( CH_Button( x, *y, w, label, qfalse, enabled ) ) {
		CL_Characters_Close();
		Cbuf_AddText( va( "%s\n", cmd ) );
	}
	*y += CH_CTRL_H + 3;
}

// what the picked one keeps, and the tools for each part
static void CH_DrawSuite( void ) {
	const float x = CH_C_X, w = CH_C_W;
	const chChar_t *sel = CH_Find( ch.sel );
	const qboolean playing = (qboolean)( sel && sel->active );
	float y = CH_TOP;

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

void CL_Characters_Close( void ) {
	if ( !ch.open )
		return;
	ch.open = qfalse;
	ch.typing = CH_TYPE_NONE;
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
		if ( key == A_ENTER || key == A_KP_ENTER )
			return;		// the buttons say what it's for
		WG_EditKey( ch.field, sizeof( ch.field ), key, NULL );
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

// the name typed in first, then closed
void CL_Characters_Escape( void ) {
	if ( ch.typing ) {
		ch.typing = CH_TYPE_NONE;
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
	ch.lastSent.clear();
}

// a hook said the account's characters changed (cmd:char for this player): the list asked again
void CL_Characters_CharHook( char **argv, int argc ) {
	if ( argc >= 1 && atoi( argv[0] ) == clc.clientNum && CL_Presence_Session() )
		CH_AskList();
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
	Com_Printf( "usage: " CH_CMD "               opens your characters, or closes them\n" );
	Com_Printf( "       " CH_CMD " list          lists them\n" );
	Com_Printf( "       " CH_CMD " play <name>   plays one: its look, settings, inventory, panel and holster\n" );
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
