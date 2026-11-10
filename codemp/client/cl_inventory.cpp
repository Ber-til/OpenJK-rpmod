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

// cl_inventory.cpp -- /inventory: what your RPMod account carries, as C1B3R keeps it (its /v2/inv,
// see OpenJK-rpmod-c1b3r's characters.py), on the C1B3R session (cl_presence.cpp). A window of its
// own, like the other tools: the items on the left, the one picked on the right.
//
// Every account gets a datapad, a commlink and the hilt of the saber it carries; staff give more,
// in Discord (/item) or here, on the Staff page, when the server says you're an RPMod admin. An item
// can be used (the datapad opens; another datapad on its own notes), shown on Inspect, and given to
// another player on the server whose game is linked. An admin can put a datapad in the world, its
// notes in it, for anyone to pick up (cl_worlditems.cpp).
//
// C1B3R tells every game on the server when a character changes (cmd:char/<client>/<version>):
// yours is asked again then.

#include <string>
#include "client.h"
#include "cl_presence.h"
#include "cl_widgets.h"

#define INV_CMD				"inventory"
#define INV_MAX				100
#define INV_GIVE_RANGE		1024.0f
#define INV_MESSAGE_MS		5000

#define IV_X			40.0f
#define IV_Y			34.0f
#define IV_W			560.0f
#define IV_H			412.0f
#define IV_TOP			( IV_Y + 42 )
#define IV_LIST_X		( IV_X + 12 )
#define IV_LIST_W		210.0f
#define IV_ROW_H		28.0f
#define IV_LIST_H		( IV_ROW_H * 12 )
#define IV_DET_X		( IV_LIST_X + IV_LIST_W + 14 )
#define IV_DET_W		( IV_X + IV_W - 12 - IV_DET_X )
#define IV_VIEW			150.0f
#define IV_CTRL_H		16.0f
#define IV_TEXT			0.8f
#define IV_SMALL		0.65f
#define IV_ITEM			0.6f

static const float ivPanel[4]		= { 0.0f, 0.0f, 0.08f, 0.88f };
static const float ivBorder[4]		= { 0.5f, 0.5f, 0.7f, 1.0f };
static const float ivItem[4]		= { 0.15f, 0.15f, 0.25f, 0.8f };
static const float ivItemHover[4]	= { 0.25f, 0.25f, 0.4f, 0.9f };
static const float ivItemOn[4]		= { 0.35f, 0.24f, 0.05f, 0.9f };
static const float ivField[4]		= { 0.05f, 0.05f, 0.12f, 0.95f };
static const float ivOrange[4]		= { 1.0f, 0.682f, 0.0f, 1.0f };
static const float ivWhite[4]		= { 1.0f, 1.0f, 1.0f, 1.0f };
static const float ivDim[4]			= { 0.6f, 0.6f, 0.7f, 1.0f };
static const float ivYellow[4]		= { 1.0f, 0.85f, 0.3f, 1.0f };
static const float ivGreen[4]		= { 0.4f, 1.0f, 0.4f, 1.0f };

typedef enum {
	INV_MINE,
	INV_STAFF
} invPage_t;

static const char *invKinds[] = { "misc", "datapad", "commlink", "hilt", "ship" };
#define INV_NUM_KINDS	( (int)ARRAY_LEN( invKinds ) )

typedef struct {
	int			id;
	char		kind[16];
	qboolean	shown;			// on Inspect
	char		name[64];
	char		desc[320];
	char		model[MAX_QPATH];
	char		saber[MAX_QPATH];	// a hilt's saber
	qboolean	starter;		// one every character's given
} invItem_t;

static struct {
	qboolean	open;
	int			font, titleFont;
	sfxHandle_t	clickSound;
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;
	int			wheel;			// the wheel's steps for an open list (negative up)

	invPage_t	page;
	qboolean	known;			// asked since it was opened, or changed
	qboolean	refused;		// C1B3R said why not: in refusal
	char		refusal[256];
	invItem_t	items[INV_MAX];
	int			num;
	int			sel;			// the item picked, by id
	int			scroll;

	qboolean	giving;			// the picked item's page shows whom to give it to
	int			giveTo;			// a client number, -1 none
	int			giveScroll;

	// the staff page
	int			target;			// a client number, -1 the account typed
	char		account[64];
	int			kind;
	char		name[64], desc[300], model[MAX_QPATH];
	int			focus;			// 0 none, 1 account, 2 name, 3 description, 4 model
	invItem_t	theirs[INV_MAX];
	int			numTheirs;
	char		theirsOf[64];
	int			theirsScroll;

	char		message[160];
	int			messageTime;
	qboolean	messageBad;
} inv;

// the 3D view of an item's model
static struct {
	char			path[MAX_QPATH];
	qhandle_t		model;
	CGhoul2Info_v	*g2;
	vec3_t			mins, maxs;
	qboolean		tried;		// path looked at: ok if it's shown
	qboolean		ok;
} invView;

static httpRequest_t invListHttp, invActHttp, invTheirsHttp;
static char invActWhat[16];

/*
===============================================================================

Talking to C1B3R

===============================================================================
*/

static void INV_Message( const char *text, qboolean bad ) {
	Q_strncpyz( inv.message, text, sizeof( inv.message ) );
	for ( char *nl = strchr( inv.message, '\n' ); nl; nl = NULL )
		*nl = '\0';
	inv.messageTime = cls.realtime;
	inv.messageBad = bad;
}

static qboolean INV_Post( httpRequest_t *req, const char *path, const std::string &fields ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session ) {
		INV_Message( "Not connected to C1B3R: the inventory needs it (/c1b3r says why)", qtrue );
		return qfalse;
	}
	if ( req->state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += fields + "}";
	return CL_Presence_Post( req, path, body );
}

static std::string INV_Str( const char *name, const char *value ) {
	std::string out = va( ",\"%s\":", name );

	CL_Presence_Json( out, value );
	return out;
}

// one at a time; its answer said in a message
static void INV_Act( const char *what, const char *path, const std::string &fields ) {
	if ( invActHttp.state == HTTP_BUSY ) {
		INV_Message( "Wait: the last one is still going", qtrue );
		return;
	}
	Q_strncpyz( invActWhat, what, sizeof( invActWhat ) );
	INV_Post( &invActHttp, path, fields );
}

static void INV_AskList( void ) {
	if ( invListHttp.state != HTTP_BUSY && INV_Post( &invListHttp, "/v2/inv/list", "" ) )
		inv.known = qtrue;
}

// for whom, on the staff page: "client":n or "account":"..."
static std::string INV_Whom( void ) {
	if ( inv.account[0] )
		return INV_Str( "account", inv.account );
	return va( ",\"client\":%i", inv.target );
}

static void INV_AskTheirs( void ) {
	if ( !inv.account[0] && inv.target < 0 ) {
		INV_Message( "Pick a player, or type an account", qtrue );
		return;
	}
	if ( INV_Post( &invTheirsHttp, "/v2/inv/staff", ",\"action\":\"list\"" + INV_Whom() ) )
		Q_strncpyz( inv.theirsOf, inv.account[0] ? inv.account : CL_PlayerSlotName( inv.target ), sizeof( inv.theirsOf ) );
}

// a line's tab-separated fields: how many
static int INV_Split( char *line, char **field, int max ) {
	int n = 0;

	for ( char *f = line; f && n < max; n++ ) {
		field[n] = f;
		f = strchr( f, '\t' );
		if ( f )
			*f++ = '\0';
	}
	return n;
}

// a string's value in C1B3R's JSON of the item, "" when it hasn't it
static void INV_JsonString( const char *json, const char *key, char *out, int size ) {
	const char *at = strstr( json, va( "\"%s\"", key ) );
	int n = 0;

	out[0] = '\0';
	if ( !at || !( at = strchr( at + strlen( key ) + 2, '"' ) ) )
		return;
	for ( at++; *at && *at != '"' && n < size - 1; at++ ) {
		if ( *at == '\\' && at[1] )
			at++;
		out[n++] = *at;
	}
	out[n] = '\0';
}

// item <id> <kind> <shown> <name> <desc> <model> <data json>, after ok
static int INV_ReadItems( const char *answer, invItem_t *items, int max ) {
	std::string all( answer );
	size_t at = 0;
	int num = 0;

	while ( at < all.size() && num < max ) {
		size_t end = all.find( '\n', at );
		char line[2048], *f[8];

		if ( end == std::string::npos )
			end = all.size();
		Q_strncpyz( line, all.substr( at, end - at ).c_str(), sizeof( line ) );
		at = end + 1;
		if ( strncmp( line, "item\t", 5 ) || INV_Split( line, f, 8 ) < 8 )
			continue;
		invItem_t *it = &items[num++];

		memset( it, 0, sizeof( *it ) );
		it->id = atoi( f[1] );
		Q_strncpyz( it->kind, f[2], sizeof( it->kind ) );
		it->shown = (qboolean)( atoi( f[3] ) != 0 );
		Q_strncpyz( it->name, CL_Presence_Unfield( f[4] ).c_str(), sizeof( it->name ) );
		Q_strncpyz( it->desc, CL_Presence_Unfield( f[5] ).c_str(), sizeof( it->desc ) );
		Q_strncpyz( it->model, CL_Presence_Unfield( f[6] ).c_str(), sizeof( it->model ) );
		{
			const std::string data = CL_Presence_Unfield( f[7] );

			INV_JsonString( data.c_str(), "saber", it->saber, sizeof( it->saber ) );
			it->starter = (qboolean)( strstr( data.c_str(), "\"starter\": 1" ) || strstr( data.c_str(), "\"starter\":1" )
				|| strstr( data.c_str(), "\"starter\": true" ) || strstr( data.c_str(), "\"starter\":true" ) );
		}
	}
	return num;
}

static invItem_t *INV_Item( int id ) {
	for ( int i = 0; i < inv.num; i++ ) {
		if ( inv.items[i].id == id )
			return &inv.items[i];
	}
	return NULL;
}

void CL_Inventory_Frame( void ) {
	if ( HTTP_Poll( &invListHttp ) ) {
		if ( invListHttp.status == 200 && !strncmp( invListHttp.answer, "ok", 2 ) ) {
			inv.num = INV_ReadItems( invListHttp.answer, inv.items, INV_MAX );
			inv.refused = qfalse;
			// the one picked given away, or taken: the first instead
			if ( !INV_Item( inv.sel ) ) {
				inv.sel = inv.num ? inv.items[0].id : 0;
				inv.giving = qfalse;
			}
		} else {
			inv.refused = qtrue;
			Q_strncpyz( inv.refusal, invListHttp.answer[0] ? invListHttp.answer : "C1B3R didn't answer", sizeof( inv.refusal ) );
		}
	}
	if ( HTTP_Poll( &invActHttp ) ) {
		if ( invActHttp.status != 200 ) {
			INV_Message( invActHttp.answer[0] ? invActHttp.answer : "C1B3R didn't answer", qtrue );
		} else if ( !strcmp( invActWhat, "give" ) ) {
			INV_Message( va( "Given to %s", CL_PlayerSlotName( inv.giveTo ) ), qfalse );
			inv.giving = qfalse;
		} else if ( !strcmp( invActWhat, "grant" ) ) {
			INV_Message( va( "Granted: %s", inv.name ), qfalse );
			inv.name[0] = inv.desc[0] = inv.model[0] = '\0';
			INV_AskTheirs();
		} else if ( !strcmp( invActWhat, "take" ) ) {
			INV_Message( "Taken", qfalse );
			INV_AskTheirs();
		}
		// what changed comes back as cmd:char too; asked now, so it shows at once
		inv.known = qfalse;
	}
	if ( HTTP_Poll( &invTheirsHttp ) ) {
		if ( invTheirsHttp.status == 200 && !strncmp( invTheirsHttp.answer, "ok", 2 ) ) {
			inv.numTheirs = INV_ReadItems( invTheirsHttp.answer, inv.theirs, INV_MAX );
			inv.theirsScroll = 0;
		} else {
			inv.numTheirs = 0;
			inv.theirsOf[0] = '\0';
			INV_Message( invTheirsHttp.answer[0] ? invTheirsHttp.answer : "C1B3R didn't answer", qtrue );
		}
	}
	if ( inv.open && !inv.known && CL_Presence_Session() )
		INV_AskList();
}

// cmd:char/<client>/<version>: a character changed on this server; ours asked again
void CL_Inventory_CharHook( char **argv, int argc ) {
	if ( argc >= 1 && argv[0][0] && atoi( argv[0] ) == clc.clientNum )
		inv.known = qfalse;
}

/*
===============================================================================

An item's model, turning

===============================================================================
*/

static void INV_FreeView( void ) {
	if ( invView.g2 )
		re->G2API_CleanGhoul2Models( &invView.g2 );
	memset( &invView, 0, sizeof( invView ) );
}

// the saber a hilt is: the one it was made for, or the one carried
static const char *INV_SaberOf( const invItem_t *it ) {
	return it->saber[0] ? it->saber : Cvar_VariableString( "saber1" );
}

// the model an item is shown with: its own, or a hilt's saber's
static const char *INV_ModelOf( const invItem_t *it ) {
	static clSaber_t saber;
	static char saberOf[MAX_QPATH];

	if ( it->model[0] )
		return it->model;
	// a datapad: the one held in the hand (the holster's)
	if ( !Q_stricmp( it->kind, "datapad" ) )
		return CL_Holster_DatapadModel();
	if ( Q_stricmp( it->kind, "hilt" ) )
		return "";
	if ( Q_stricmp( INV_SaberOf( it ), saberOf ) ) {
		Q_strncpyz( saberOf, INV_SaberOf( it ), sizeof( saberOf ) );
		if ( !CL_Saber_Find( saberOf, &saber ) )
			memset( &saber, 0, sizeof( saber ) );
	}
	return saber.model;
}

static void INV_SetView( const char *path ) {
	if ( invView.tried && !Q_stricmp( invView.path, path ) )
		return;
	INV_FreeView();
	invView.tried = qtrue;
	Q_strncpyz( invView.path, path, sizeof( invView.path ) );
	if ( !path[0] || FS_ReadFile( path, NULL ) <= 0 )
		return;
	if ( !Q_stricmp( COM_GetExtension( path ), "glm" ) ) {
		if ( CL_GlmBounds( path, invView.mins, invView.maxs ) )
			return;
		if ( re->G2API_InitGhoul2Model( &invView.g2, path, 0, 0, 0, 0, 0 ) < 0 || !invView.g2 ) {
			invView.g2 = NULL;
			return;
		}
	} else {
		invView.model = re->RegisterModel( path );
		if ( !invView.model )
			return;
		re->ModelBounds( invView.model, invView.mins, invView.maxs );
	}
	invView.ok = qtrue;
}

/*
===============================================================================

Drawing: the tools' look

===============================================================================
*/

static void INV_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void INV_Box( float x, float y, float w, float h, const float *fill ) {
	INV_Fill( x, y, w, h, fill );
	INV_Fill( x, y, w, 1, ivBorder );
	INV_Fill( x, y + h - 1, w, 1, ivBorder );
	INV_Fill( x, y, 1, h, ivBorder );
	INV_Fill( x + w - 1, y, 1, h, ivBorder );
}

// one line, colour codes in it drawn as colours (RPMod's too); cut with ... to fit w
static void INV_Text( float x, float y, float w, const char *text, const float *color, int font, float scale, qboolean centred ) {
	WG_TypedText( x, y, w, text, color, font, scale, centred, qtrue );
}

static qboolean INV_InRect( float x, float y, float w, float h ) {
	if ( WG_PopupHover( inv.cursorX, inv.cursorY ) )
		return qfalse;
	return (qboolean)( inv.cursorX >= x && inv.cursorX < x + w && inv.cursorY >= y && inv.cursorY < y + h );
}

static qboolean INV_Clicked( float x, float y, float w, float h ) {
	if ( !inv.click || inv.clickX < x || inv.clickX >= x + w || inv.clickY < y || inv.clickY >= y + h )
		return qfalse;
	inv.click = qfalse;
	if ( inv.clickSound )
		S_StartLocalSound( inv.clickSound, CHAN_LOCAL_SOUND );
	return qtrue;
}

// the shared - value + control, in the window's look and with its mouse (the wheel for an open list)
static void INV_PopStyle( wgStyle_t *style ) {
	memset( style, 0, sizeof( *style ) );
	style->font = inv.font;
	style->scale = IV_TEXT;
	style->lineH = IV_CTRL_H;
	style->padX = 4.0f;
	style->padY = 2.0f;
	style->fill = ivItem;
	style->border = ivBorder;
	style->text = ivWhite;
	style->dim = ivDim;
}

static void INV_PopMouse( wgMouse_t *m ) {
	const qboolean over = WG_PopupHover( inv.cursorX, inv.cursorY );

	m->cursorX = over ? -1.0f : inv.cursorX;
	m->cursorY = over ? -1.0f : inv.cursorY;
	m->click = &inv.click;
	m->clickX = inv.clickX;
	m->clickY = inv.clickY;
	m->wheel = WG_PopupOpen() ? &inv.wheel : NULL;
}

static qboolean INV_Button( float x, float y, float w, const char *label, qboolean on, qboolean enabled ) {
	INV_Box( x, y, w, IV_CTRL_H, on ? ivItemOn : enabled && INV_InRect( x, y, w, IV_CTRL_H ) ? ivItemHover : ivItem );
	INV_Text( x, y + 2, w, label, enabled ? ivWhite : ivDim, inv.font, IV_TEXT, qtrue );
	return enabled ? INV_Clicked( x, y, w, IV_CTRL_H ) : qfalse;
}

static void INV_Style( wgStyle_t *style ) {
	memset( style, 0, sizeof( *style ) );
	style->font = inv.font;
	style->scale = IV_TEXT;
	style->lineH = 13.0f;
	style->padX = 4;
	style->padY = 2;
	style->fill = ivField;
	style->border = ivBorder;
	style->text = ivWhite;
	style->dim = ivDim;
}

// wrapped text at x, y in w, at most maxH tall: the height it took
static float INV_Para( float x, float y, float w, float maxH, const char *text, const float *color ) {
	wgStyle_t style;
	int lines;

	INV_Style( &style );
	lines = Q_min( WG_TextLines( &style, text, w ), (int)( maxH / style.lineH ) );
	WG_DrawText( &style, x, y, w, text, 0, lines, color );
	return lines * style.lineH;
}

// a text field, known by its number
static void INV_Field( float x, float y, float w, float h, char *buf, int which, const char *placeholder, qboolean box ) {
	wgStyle_t style;

	INV_Style( &style );
	INV_Box( x, y, w, h, ivField );
	if ( inv.focus == which )
		INV_Fill( x, y + h - 1, w, 1, ivOrange );		// the one typed in
	if ( box )
		WG_TextBox( &style, x, y, w, h, buf, (qboolean)( inv.focus == which ), placeholder, inv.cursorX, inv.cursorY );
	else
		WG_Field( &style, x, y, w, h, buf, (qboolean)( inv.focus == which ), placeholder, inv.cursorX, inv.cursorY );
	// a field grows down while it's typed in; a box doesn't
	if ( INV_Clicked( x, y, w, box ? h : WG_FieldHeight( &style, w, h, buf, (qboolean)( inv.focus == which ) ) ) ) {
		if ( inv.focus != which )
			WG_EditReset();
		inv.focus = which;
		WG_FieldClick( buf, inv.clickX, inv.clickY );
	}
}

// each kind's picture, drawn in its square
static void INV_Icon( const char *kind, float x, float y, float size ) {
	INV_Box( x, y, size, size, ivField );
	if ( !Q_stricmp( kind, "datapad" ) ) {
		INV_Fill( x + size * 0.28f, y + size * 0.15f, size * 0.44f, size * 0.7f, ivOrange );
		INV_Fill( x + size * 0.34f, y + size * 0.22f, size * 0.32f, size * 0.4f, ivField );
	} else if ( !Q_stricmp( kind, "commlink" ) ) {
		INV_Fill( x + size * 0.36f, y + size * 0.35f, size * 0.28f, size * 0.5f, ivOrange );
		INV_Fill( x + size * 0.56f, y + size * 0.12f, size * 0.06f, size * 0.25f, ivWhite );
	} else if ( !Q_stricmp( kind, "hilt" ) ) {
		INV_Fill( x + size * 0.44f, y + size * 0.38f, size * 0.12f, size * 0.5f, ivOrange );
		INV_Fill( x + size * 0.4f, y + size * 0.32f, size * 0.2f, size * 0.08f, ivWhite );
		INV_Fill( x + size * 0.47f, y + size * 0.1f, size * 0.06f, size * 0.22f, ivGreen );
	} else if ( !Q_stricmp( kind, "ship" ) ) {
		INV_Fill( x + size * 0.18f, y + size * 0.46f, size * 0.64f, size * 0.12f, ivOrange );
		INV_Fill( x + size * 0.42f, y + size * 0.25f, size * 0.16f, size * 0.5f, ivWhite );
	} else {
		INV_Fill( x + size * 0.24f, y + size * 0.32f, size * 0.52f, size * 0.44f, ivOrange );
		INV_Fill( x + size * 0.24f, y + size * 0.32f, size * 0.52f, size * 0.08f, ivWhite );
	}
}

// a row of a list: its picture, name and what it is; qtrue when clicked
static qboolean INV_Row( float x, float y, float w, const invItem_t *it, qboolean on ) {
	const qboolean hover = INV_InRect( x, y, w, IV_ROW_H - 2 );

	INV_Box( x, y, w, IV_ROW_H - 2, on ? ivItemOn : hover ? ivItemHover : ivItem );
	INV_Icon( it->kind, x + 3, y + 3, IV_ROW_H - 8 );
	INV_Text( x + IV_ROW_H, y + 2, w - IV_ROW_H - 4, it->name, on || hover ? ivWhite : ivOrange, inv.titleFont, IV_ITEM, qfalse );
	INV_Text( x + IV_ROW_H, y + 14, w - IV_ROW_H - 4, va( "%s%s", it->kind, it->shown ? "  -  on Inspect" : "" ), ivDim, inv.font, IV_SMALL, qfalse );
	return INV_Clicked( x, y, w, IV_ROW_H - 2 );
}

// a list of items in its box, scrolled: the one clicked, -1 none
static int INV_List( float x, float y, float w, float h, const invItem_t *items, int num, int *scroll, int on ) {
	const int visible = (int)( h / IV_ROW_H );
	const qboolean bar = WG_Scrollbar( x + w - WG_SCROLL_W, y, h, num, visible, scroll, inv.cursorX, inv.cursorY, &inv.click, inv.clickX, inv.clickY );
	const float rw = w - ( bar ? WG_SCROLL_W + 2 : 0 );
	int clicked = -1;

	*scroll = Com_Clampi( 0, Q_max( 0, num - visible ), *scroll );
	for ( int i = *scroll; i < num && i < *scroll + visible; i++, y += IV_ROW_H ) {
		if ( INV_Row( x, y, rw, &items[i], (qboolean)( items[i].id == on ) ) )
			clicked = i;
	}
	return clicked;
}

static void INV_DrawView( float x, float y, float w, float h ) {
	const float xScale = cls.glconfig.vidWidth / (float)SCREEN_WIDTH, yScale = cls.glconfig.vidHeight / (float)SCREEN_HEIGHT;
	refdef_t refdef;
	refEntity_t ent;
	vec3_t center, size, angles;
	float radius, fovX, fovY = 30.0f;

	INV_Box( x, y, w, h, ivField );
	if ( !invView.ok ) {
		INV_Text( x, y + h * 0.5f - 6, w, invView.path[0] ? "You don't have its model" : "No model", ivDim, inv.font, IV_SMALL, qtrue );
		return;
	}
	VectorAdd( invView.mins, invView.maxs, center );
	VectorScale( center, 0.5f, center );
	VectorSubtract( invView.maxs, invView.mins, size );
	radius = Q_max( 2.0f, VectorLength( size ) * 0.5f );

	memset( &refdef, 0, sizeof( refdef ) );
	refdef.x = (int)( x * xScale ) + 1;
	refdef.y = (int)( y * yScale ) + 1;
	refdef.width = (int)( w * xScale ) - 2;
	refdef.height = (int)( h * yScale ) - 2;
	fovX = RAD2DEG( 2.0f * atanf( tanf( DEG2RAD( fovY * 0.5f ) ) * refdef.width / (float)refdef.height ) );
	refdef.fov_x = fovX;
	refdef.fov_y = fovY;
	VectorSet( angles, 10.0f, 180.0f, 0 );
	AnglesToAxis( angles, refdef.viewaxis );
	{
		const float dist = radius / sinf( DEG2RAD( Q_min( fovX, fovY ) * 0.5f ) ) * 1.05f;
		vec3_t fwd;

		AngleVectors( angles, fwd, NULL, NULL );
		VectorScale( fwd, -dist, refdef.vieworg );
	}
	refdef.rdflags = RDF_NOWORLDMODEL;
	refdef.time = cl.serverTime;

	// turning about its middle, which stays where the camera looks
	memset( &ent, 0, sizeof( ent ) );
	ent.reType = RT_MODEL;
	ent.renderfx = RF_NOSHADOW;
	VectorSet( ent.angles, 0, (float)( cls.realtime % 10000 ) * 0.036f, 0 );
	AnglesToAxis( ent.angles, ent.axis );
	for ( int k = 0; k < 3; k++ )
		ent.origin[k] = -( ent.axis[0][k] * center[0] + ent.axis[1][k] * center[1] + ent.axis[2][k] * center[2] );
	ent.radius = radius;
	if ( invView.g2 )
		ent.ghoul2 = invView.g2;
	else
		ent.hModel = invView.model;

	re->ClearScene();
	re->AddRefEntityToScene( &ent );
	re->RenderScene( &refdef );
}

/*
===============================================================================

Your items

===============================================================================
*/

// the players on this server but you, the one aimed at first
static int INV_Players( int *out, int max ) {
	const int aimed = CL_AimedPlayer( INV_GIVE_RANGE );
	int n = 0;

	if ( aimed >= 0 && CL_PlayerSlotName( aimed )[0] )
		out[n++] = aimed;
	for ( int c = 0; c < MAX_CLIENTS && n < max; c++ ) {
		if ( c != clc.clientNum && c != aimed && CL_PlayerSlotName( c )[0] )
			out[n++] = c;
	}
	return n;
}

static void INV_Close( void );

// what using it does: the datapad opens; qfalse when it's not for using
static qboolean INV_Usable( const invItem_t *it ) {
	return (qboolean)!Q_stricmp( it->kind, "datapad" );
}

// your own datapad opens as it is; another opens on its own notes
static void INV_Use( const invItem_t *it ) {
	if ( !Q_stricmp( it->kind, "datapad" ) ) {
		INV_Close();
		if ( it->starter )
			Cbuf_AddText( "datapad open\n" );
		else
			CL_Datapad_OpenItem( it->id, it->name, 0 );
	}
}

// one anyone can drop in the world (not the ones every character has)
static qboolean INV_Placeable( const invItem_t *it ) {
	return (qboolean)!it->starter;
}

// whom to give it to: the players here, then the button
static void INV_DrawGive( const invItem_t *it, float x, float y, float w, float bottom ) {
	const int aimed = CL_AimedPlayer( INV_GIVE_RANGE );
	int players[MAX_CLIENTS], num, visible;

	INV_Text( x, y, w, "Give it to whom? Their game must be linked too.", ivYellow, inv.font, IV_SMALL, qfalse );
	y += 14;
	num = INV_Players( players, MAX_CLIENTS );
	if ( !num ) {
		INV_Text( x, y + 6, w, "Nobody else is on the server", ivDim, inv.font, IV_TEXT, qfalse );
	} else {
		const float listH = bottom - y - IV_CTRL_H - 8;
		qboolean bar;
		float rw;

		visible = Q_max( 1, (int)( listH / 18 ) );
		bar = WG_Scrollbar( x + w - WG_SCROLL_W, y, visible * 18, num, visible, &inv.giveScroll, inv.cursorX, inv.cursorY, &inv.click, inv.clickX, inv.clickY );
		rw = w - ( bar ? WG_SCROLL_W + 2 : 0 );
		inv.giveScroll = Com_Clampi( 0, Q_max( 0, num - visible ), inv.giveScroll );
		for ( int i = inv.giveScroll; i < num && i < inv.giveScroll + visible; i++ ) {
			const int c = players[i];
			const float ry = y + ( i - inv.giveScroll ) * 18;
			const qboolean on = (qboolean)( c == inv.giveTo ), hover = INV_InRect( x, ry, rw, 16 );

			INV_Box( x, ry, rw, 16, on ? ivItemOn : hover ? ivItemHover : ivItem );
			INV_Text( x + 6, ry + 1, rw - 70, CL_PlayerSlotName( c ), ivWhite, inv.font, IV_TEXT, qfalse );
			if ( c == aimed )
				INV_Text( x + rw - 64, ry + 2, 60, "aimed at", ivDim, inv.font, IV_SMALL, qfalse );
			if ( INV_Clicked( x, ry, rw, 16 ) )
				inv.giveTo = c;
		}
	}
	y = bottom - IV_CTRL_H;
	if ( INV_Button( x, y, 70, "Cancel", qfalse, qtrue ) )
		inv.giving = qfalse;
	{
		const qboolean can = (qboolean)( inv.giveTo >= 0 && CL_PlayerSlotName( inv.giveTo )[0] );

		if ( INV_Button( x + 76, y, w - 76, can ? va( "Give it to %s", CL_PlayerSlotName( inv.giveTo ) ) : "Pick someone", can, can ) )
			INV_Act( "give", "/v2/inv/give", va( ",\"item\":%i,\"client\":%i", it->id, inv.giveTo ) );
	}
}

static void INV_DrawDetail( void ) {
	const float x = IV_DET_X, w = IV_DET_W, bottom = IV_TOP + IV_LIST_H;
	invItem_t *it = INV_Item( inv.sel );
	float y = IV_TOP, ty;

	INV_Box( x - 6, IV_TOP - 4, w + 12, IV_LIST_H + 8, ivItem );
	if ( !it )
		return;
	INV_SetView( INV_ModelOf( it ) );
	INV_DrawView( x, y, IV_VIEW, IV_VIEW );

	// beside the model: what it is, and what's done with it
	{
		const float tx = x + IV_VIEW + 10, tw = w - IV_VIEW - 10;

		ty = y;
		INV_Text( tx, ty, tw, it->name, ivOrange, inv.titleFont, IV_ITEM + 0.1f, qfalse );
		ty += 22;
		INV_Text( tx, ty, tw, va( "Kind: %s", it->kind ), ivDim, inv.font, IV_TEXT, qfalse );
		ty += 14;
		if ( !Q_stricmp( it->kind, "hilt" ) ) {
			clSaber_t saber;

			INV_Text( tx, ty, tw, va( "Saber: %s", CL_Saber_Find( INV_SaberOf( it ), &saber ) ? saber.name : INV_SaberOf( it ) ),
				ivDim, inv.font, IV_TEXT, qfalse );
			ty += 14;
		}
		ty = y + IV_VIEW - ( INV_Placeable( it ) ? 4 : 3 ) * ( IV_CTRL_H + 4 ) + 4;
		if ( INV_Usable( it ) && INV_Button( tx, ty, tw, Q_stricmp( it->kind, "datapad" ) || it->starter ? "Use" : "Open its notes", qfalse, qtrue ) ) {
			INV_Use( it );
			return;
		}
		ty += IV_CTRL_H + 4;
		if ( INV_Placeable( it ) ) {
			if ( INV_Button( tx, ty, tw, "Drop", qfalse, qtrue ) ) {
				const int id = it->id;
				char name[64];

				Q_strncpyz( name, it->name, sizeof( name ) );
				INV_Close();
				CL_WorldItems_Drop( id, name );
				return;
			}
			ty += IV_CTRL_H + 4;
		}
		if ( INV_Button( tx, ty, tw, it->shown ? "Shown on Inspect" : "Hidden from Inspect", it->shown, qtrue ) )
			INV_Act( "show", "/v2/inv/show", va( ",\"item\":%i,\"shown\":%i", it->id, it->shown ? 0 : 1 ) );
		ty += IV_CTRL_H + 4;
		if ( INV_Button( tx, ty, tw, "Give...", inv.giving, qtrue ) ) {
			inv.giving = (qboolean)!inv.giving;
			inv.giveTo = CL_AimedPlayer( INV_GIVE_RANGE );
			inv.giveScroll = 0;
		}
	}

	// under it: its description, or whom to give it to
	y += IV_VIEW + 10;
	if ( inv.giving )
		INV_DrawGive( it, x, y, w, bottom );
	else if ( it->desc[0] )
		INV_Para( x, y, w, bottom - y, it->desc, ivWhite );
	else
		INV_Text( x, y, w, "No description", ivDim, inv.font, IV_TEXT, qfalse );
}

static void INV_DrawMine( void ) {
	if ( !CL_Presence_Session() ) {
		INV_Text( IV_X, IV_TOP + 120, IV_W, "Not connected to C1B3R", ivOrange, inv.titleFont, IV_ITEM + 0.1f, qtrue );
		INV_Text( IV_X, IV_TOP + 146, IV_W, "Your inventory is kept there: /c1b3r says why it isn't connected", ivDim, inv.font, IV_TEXT, qtrue );
		return;
	}
	if ( inv.refused ) {
		INV_Text( IV_X, IV_TOP + 110, IV_W, "Your inventory needs your game linked", ivOrange, inv.titleFont, IV_ITEM + 0.1f, qtrue );
		INV_Para( IV_X + 100, IV_TOP + 136, IV_W - 200, 80, inv.refusal, ivDim );
		return;
	}
	if ( !inv.num ) {
		INV_Text( IV_X, IV_TOP + 120, IV_W, invListHttp.state == HTTP_BUSY || !inv.known ? "Asking C1B3R..." : "You carry nothing",
			ivDim, inv.font, IV_TEXT, qtrue );
		return;
	}
	if ( !INV_Item( inv.sel ) )
		inv.sel = inv.items[0].id;
	{
		const int clicked = INV_List( IV_LIST_X, IV_TOP, IV_LIST_W, IV_LIST_H, inv.items, inv.num, &inv.scroll, inv.sel );

		if ( clicked >= 0 && inv.items[clicked].id != inv.sel ) {
			inv.sel = inv.items[clicked].id;
			inv.giving = qfalse;
		}
	}
	INV_DrawDetail();
}

/*
===============================================================================

The staff page

===============================================================================
*/

// the staff page's players to pick from, as this frame has them
static int invPickPlayers[MAX_CLIENTS], invPickNum;

static const char *INV_PlayerAt( int i, void *ctx ) {
	static char name[MAX_NAME_LENGTH * 2];

	if ( i < 0 || i >= invPickNum )
		return "";
	Q_strncpyz( name, CL_PlayerSlotName( invPickPlayers[i] ), sizeof( name ) );
	Q_StripColor( name );
	return name;
}

static void INV_DrawStaff( void ) {
	const float x = IV_LIST_X, w = 250, label = 76, fx = x + label, fw = w - label;
	const float rx = x + w + 14, rw = IV_X + IV_W - 12 - rx;
	float y = IV_TOP;

	// whom it's for: a player here (aimed at first), or an account by its name
	INV_Text( x, y + 2, label, "Player", ivOrange, inv.font, IV_TEXT, qfalse );
	{
		// the players here (aimed at first), picked from a list; kept by client number
		static int pick;
		wgStyle_t style;
		wgMouse_t m;

		invPickNum = INV_Players( invPickPlayers, MAX_CLIENTS );
		for ( pick = 0; pick < invPickNum && invPickPlayers[pick] != inv.target; pick++ )
			;
		if ( pick == invPickNum )
			pick = -1;
		INV_PopStyle( &style );
		INV_PopMouse( &m );
		if ( !invPickNum ) {
			INV_Button( fx, y, fw, "Nobody else here", qfalse, qfalse );
		} else if ( WG_Choice( &style, &m, fx, y, fw, IV_CTRL_H, &pick, invPickNum, INV_PlayerAt, NULL, qtrue ) && pick >= 0 ) {
			inv.target = invPickPlayers[pick];
			inv.account[0] = '\0';
			inv.focus = 0;
		}
		if ( invPickNum && pick < 0 )
			INV_Text( fx + IV_CTRL_H, y + 2, fw - IV_CTRL_H * 2, "Pick one", ivDim, inv.font, IV_TEXT, qtrue );
	}
	y += IV_CTRL_H + 4;
	INV_Text( x, y + 2, label, "or account", ivOrange, inv.font, IV_TEXT, qfalse );
	INV_Field( fx, y, fw, IV_CTRL_H, inv.account, 1, "an RPMod account", qfalse );
	y += IV_CTRL_H + 14;

	INV_Text( x, y + 2, label, "Kind", ivOrange, inv.font, IV_TEXT, qfalse );
	{
		wgStyle_t style;
		wgMouse_t m;

		INV_PopStyle( &style );
		INV_PopMouse( &m );
		WG_ChoiceBox( &style, &m, fx, y, fw, IV_CTRL_H, &inv.kind, invKinds, INV_NUM_KINDS );
	}
	y += IV_CTRL_H + 4;
	INV_Text( x, y + 2, label, "Name", ivOrange, inv.font, IV_TEXT, qfalse );
	INV_Field( fx, y, fw, IV_CTRL_H, inv.name, 2, "what it's called", qfalse );
	y += IV_CTRL_H + 4;
	INV_Text( x, y + 2, label, "Description", ivOrange, inv.font, IV_TEXT, qfalse );
	INV_Field( fx, y, fw, 70, inv.desc, 3, "what it is (optional)", qtrue );
	y += 74;
	INV_Text( x, y + 2, label, "Model", ivOrange, inv.font, IV_TEXT, qfalse );
	INV_Field( fx, y, fw, IV_CTRL_H, inv.model, 4, "models/... (optional)", qfalse );
	y += IV_CTRL_H + 14;

	if ( INV_Button( fx, y, fw, "Grant", qfalse, qtrue ) ) {
		inv.focus = 0;
		if ( !inv.account[0] && inv.target < 0 )
			INV_Message( "Pick a player, or type an account", qtrue );
		else if ( !inv.name[0] )
			INV_Message( "Give it a name", qtrue );
		else
			INV_Act( "grant", "/v2/inv/staff", ",\"action\":\"grant\"" + INV_Whom() + INV_Str( "kind", invKinds[inv.kind] )
				+ INV_Str( "name", inv.name ) + INV_Str( "desc", inv.desc ) + INV_Str( "model", inv.model ) );
	}
	y += IV_CTRL_H + 14;
	INV_Para( x, y, w, 60, "Whom it's for: a player on this server, or any account by its name. "
		"Grants and takes are logged. Staff can also use /item in Discord.", ivDim );

	// on the right: what they carry, to take back
	INV_Box( rx - 6, IV_TOP - 4, rw + 12, IV_LIST_H + 8, ivItem );
	if ( INV_Button( rx, IV_TOP, rw, inv.theirsOf[0] ? va( "%s's items (%i): ask again", inv.theirsOf, inv.numTheirs ) : "Their items", qfalse, qtrue ) ) {
		inv.focus = 0;
		INV_AskTheirs();
	}
	if ( inv.theirsOf[0] && !inv.numTheirs ) {
		INV_Text( rx, IV_TOP + 30, rw, "They carry nothing", ivDim, inv.font, IV_TEXT, qtrue );
	} else if ( inv.numTheirs ) {
		const float ly = IV_TOP + IV_CTRL_H + 8, lh = IV_LIST_H - IV_CTRL_H - 8;
		const int visible = (int)( lh / IV_ROW_H );

		inv.theirsScroll = Com_Clampi( 0, Q_max( 0, inv.numTheirs - visible ), inv.theirsScroll );
		for ( int i = inv.theirsScroll; i < inv.numTheirs && i < inv.theirsScroll + visible; i++ ) {
			const float ry = ly + ( i - inv.theirsScroll ) * IV_ROW_H;

			INV_Row( rx, ry, rw - 58, &inv.theirs[i], qfalse );
			if ( INV_Button( rx + rw - 54, ry + 5, 54, "Take", qfalse, qtrue ) )
				INV_Act( "take", "/v2/inv/staff", va( ",\"action\":\"take\",\"item\":%i", inv.theirs[i].id ) );
		}
	}
}

/*
===============================================================================

The window

===============================================================================
*/

static void INV_Close( void ) {
	if ( !inv.open )
		return;
	WG_PopupClose();
	inv.open = qfalse;
	inv.focus = 0;
	inv.giving = qfalse;
	INV_FreeView();
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_INVENTORY );
}

void CL_Inventory_Close( void ) {
	INV_Close();
}

static void INV_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Inventory: join a server first\n" );
		return;
	}
	if ( inv.open )
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
	CL_Inspect_Close();
	CL_Holster_Close();
	CL_Characters_Close();
	CL_Datapad_Close();

	inv.font = re->RegisterFont( "arialnb" );
	if ( !inv.font )
		inv.font = cls.menuFont;
	inv.titleFont = re->RegisterFont( "ergoec" );
	if ( !inv.titleFont )
		inv.titleFont = inv.font;
	inv.clickSound = S_RegisterSound( "sound/interface/button1.wav" );
	inv.cursorX = SCREEN_WIDTH * 0.5f;
	inv.cursorY = SCREEN_HEIGHT * 0.5f;
	inv.click = qfalse;
	inv.known = qfalse;
	inv.giving = qfalse;
	inv.open = qtrue;
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_INVENTORY );
}

void CL_Inventory_Draw( void ) {
	const char *rank = CL_RpAdmin_Rank();
	const qboolean staff = (qboolean)( rank && rank[0] );

	if ( !inv.open )
		return;
	{
		wgMouse_t m;

		INV_PopMouse( &m );
		WG_PopupBegin( &m );
		if ( WG_PopupTyping() )
			inv.focus = 0;
	}
	if ( inv.page == INV_STAFF && !staff )
		inv.page = INV_MINE;
	INV_Box( IV_X, IV_Y, IV_W, IV_H, ivPanel );
	INV_Text( IV_LIST_X, IV_Y + 8, 300, inv.page == INV_STAFF ? "INVENTORY: STAFF" : "INVENTORY", ivOrange, inv.titleFont, IV_ITEM + 0.2f, qfalse );
	if ( inv.page == INV_MINE && inv.num )
		INV_Text( IV_LIST_X + 200, IV_Y + 13, 120, va( "%i item%s", inv.num, inv.num == 1 ? "" : "s" ), ivDim, inv.font, IV_TEXT, qfalse );
	if ( INV_Button( IV_X + IV_W - 12 - 56, IV_Y + 10, 56, "Close", qfalse, qtrue ) ) {
		INV_Close();
		return;
	}
	if ( staff && INV_Button( IV_X + IV_W - 12 - 56 - 6 - 90, IV_Y + 10, 90, inv.page == INV_STAFF ? "Your items" : "Staff", (qboolean)( inv.page == INV_STAFF ), qtrue ) ) {
		inv.page = inv.page == INV_STAFF ? INV_MINE : INV_STAFF;
		inv.focus = 0;
	}

	if ( inv.page == INV_STAFF )
		INV_DrawStaff();
	else
		INV_DrawMine();

	if ( inv.message[0] && cls.realtime - inv.messageTime < INV_MESSAGE_MS )
		INV_Text( IV_LIST_X, IV_Y + IV_H - 20, IV_W - 24, inv.message, inv.messageBad ? ivYellow : ivGreen, inv.font, IV_TEXT, qfalse );
	else if ( inv.page == INV_MINE )
		INV_Text( IV_LIST_X, IV_Y + IV_H - 20, IV_W - 24, "Kept by C1B3R for your RPMod account. Items shown on Inspect are what others see you carry.",
			ivDim, inv.font, IV_TEXT, qfalse );
	inv.click = qfalse;
	inv.wheel = 0;
	WG_DrawOverlay();
	re->SetColor( NULL );
	re->DrawStretchPic( inv.cursorX, inv.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

/*
===============================================================================

Keys, the mouse, and the command

===============================================================================
*/

static char *INV_FocusedBuf( int *size ) {
	switch ( inv.focus ) {
	case 1: *size = sizeof( inv.account );	return inv.account;
	case 2: *size = sizeof( inv.name );		return inv.name;
	case 3: *size = sizeof( inv.desc );		return inv.desc;
	case 4: *size = sizeof( inv.model );	return inv.model;
	default: return NULL;
	}
}

static qboolean INV_IsToggleKey( int key ) {
	const char *binding = Key_GetBinding( key );

	return (qboolean)( VALIDSTRING( binding ) && !Q_stricmp( binding, INV_CMD ) );
}

void CL_Inventory_KeyEvent( int key, qboolean down ) {
	int size;
	char *buf;

	if ( !inv.open || !down )
		return;
	if ( key == A_MOUSE1 ) {
		inv.click = qtrue;
		inv.clickX = inv.cursorX;
		inv.clickY = inv.cursorY;
		return;
	}
	// an open list first: the wheel scrolls it
	if ( WG_PopupOpen() && ( key == A_MWHEELUP || key == A_MWHEELDOWN ) ) {
		inv.wheel += key == A_MWHEELUP ? -1 : 1;
		return;
	}
	if ( WG_PopupKey( key ) )
		return;
	buf = INV_FocusedBuf( &size );
	if ( buf ) {
		if ( ( key == A_ENTER || key == A_KP_ENTER ) && inv.focus == 3 ) {
			WG_EditChar( buf, size, '\n', NULL );
		} else if ( key == A_ENTER || key == A_KP_ENTER || key == A_TAB ) {
			inv.focus = key == A_TAB ? inv.focus % 4 + 1 : 0;
			WG_EditReset();
		} else {
			if ( inv.focus == 1 && WG_EditKey( buf, size, key, NULL ) == WG_EDIT_CHANGED )
				inv.target = -1;		// an account typed: it's for them
			else if ( inv.focus != 1 )
				WG_EditKey( buf, size, key, NULL );
		}
		return;
	}
	if ( INV_IsToggleKey( key ) ) {
		INV_Close();
		return;
	}
	switch ( key ) {
	case A_MWHEELUP:
	case A_CURSOR_UP:
		if ( inv.page == INV_STAFF )
			inv.theirsScroll = Q_max( 0, inv.theirsScroll - 1 );
		else if ( inv.giving )
			inv.giveScroll = Q_max( 0, inv.giveScroll - 1 );
		else
			inv.scroll = Q_max( 0, inv.scroll - 1 );
		break;
	case A_MWHEELDOWN:
	case A_CURSOR_DOWN:
		// held in range as they're drawn
		if ( inv.page == INV_STAFF )
			inv.theirsScroll++;
		else if ( inv.giving )
			inv.giveScroll++;
		else
			inv.scroll++;
		break;
	default:
		break;
	}
}

void CL_Inventory_CharEvent( int ch ) {
	int size;
	char *buf = INV_FocusedBuf( &size );

	if ( inv.open && WG_PopupChar( ch ) )
		return;
	if ( !buf )
		return;
	if ( inv.focus == 1 )
		inv.target = -1;
	WG_EditChar( buf, size, ch, NULL );
}

void CL_Inventory_MouseEvent( int dx, int dy ) {
	inv.cursorX = Com_Clamp( 0, SCREEN_WIDTH, inv.cursorX + dx );
	inv.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, inv.cursorY + dy );
}

// a field typed in first, then whom to give to, then closed
void CL_Inventory_Escape( void ) {
	if ( WG_PopupOpen() ) {
		WG_PopupClose();
		return;
	}
	if ( inv.focus ) {
		inv.focus = 0;
		WG_EditReset();
		return;
	}
	if ( inv.giving ) {
		inv.giving = qfalse;
		return;
	}
	INV_Close();
}

// the item picked in the window (not one every character has): its id, and its name in name; 0 none
int CL_Inventory_Picked( char *name, int size ) {
	const invItem_t *it = INV_Item( inv.sel );

	if ( !it || it->starter || inv.page != INV_MINE )
		return 0;
	Q_strncpyz( name, it->name, size );
	return it->id;
}

void CL_Inventory_Disconnect( void ) {
	INV_Close();
	inv.giveTo = inv.target = -1;
	inv.numTheirs = 0;
	inv.theirsOf[0] = '\0';
	inv.page = INV_MINE;
}

// the renderer's models are gone with it: the view's dropped, not freed
void CL_Inventory_RendererRestarted( void ) {
	memset( &invView, 0, sizeof( invView ) );
}

static void INV_Inventory_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !arg[0] ) {
		if ( inv.open )
			INV_Close();
		else
			INV_Open();
	} else if ( !Q_stricmp( arg, "staff" ) ) {
		INV_Open();
		inv.page = INV_STAFF;
	} else if ( !Q_stricmp( arg, "list" ) ) {
		for ( int i = 0; i < inv.num; i++ )
			Com_Printf( "#%i  %-9s %s%s\n", inv.items[i].id, inv.items[i].kind, inv.items[i].name, inv.items[i].shown ? "  (shown)" : "" );
		if ( !inv.num )
			Com_Printf( "Nothing known yet: open it once (/" INV_CMD ")\n" );
	} else if ( !Q_stricmp( arg, "give" ) && Cmd_Argc() >= 3 ) {
		const int client = Cmd_Argc() >= 4 ? atoi( Cmd_Argv( 3 ) ) : CL_AimedPlayer( INV_GIVE_RANGE );

		if ( client < 0 ) {
			Com_Printf( "Aim at someone, or name their client number: " INV_CMD " give <item> <client>\n" );
			return;
		}
		inv.giveTo = client;
		INV_Act( "give", "/v2/inv/give", va( ",\"item\":%i,\"client\":%i", atoi( Cmd_Argv( 2 ) ), client ) );
	} else if ( !Q_stricmp( arg, "show" ) || !Q_stricmp( arg, "hide" ) ) {
		INV_Act( "show", "/v2/inv/show", va( ",\"item\":%i,\"shown\":%i", atoi( Cmd_Argv( 2 ) ), !Q_stricmp( arg, "show" ) ) );
	} else {
		Com_Printf( "usage: " INV_CMD "                       opens your inventory, or closes it\n" );
		Com_Printf( "       " INV_CMD " staff                 opens it on the staff page (RPMod admins)\n" );
		Com_Printf( "       " INV_CMD " list                  your items, as last asked\n" );
		Com_Printf( "       " INV_CMD " give <item> [client]  to the player you aim at, or that client\n" );
		Com_Printf( "       " INV_CMD " show|hide <item>      on Inspect, or not\n" );
	}
}

void CL_Inventory_Init( void ) {
	memset( &inv, 0, sizeof( inv ) );
	inv.giveTo = inv.target = -1;
	Cmd_AddCommand( INV_CMD, INV_Inventory_f, "Your inventory, kept by C1B3R: what you carry, shown on Inspect and given to others" );
}

void CL_Inventory_Shutdown( void ) {
	INV_Close();
}
