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

// cl_inspect.cpp -- /inspect: another player's character, or yours. Their model and sabers turning
// on the left; on the right their character panel (name, species, age... biography) and the items
// they show, as C1B3R keeps them for their RPMod account (its /v2/char/view, by client number on
// this server: their account is never said). Yours is edited here and saved to C1B3R.
//
// Aiming at someone with a character panel shows their character's name under the crosshair, with
// the key that inspects them (cl_inspectHint). C1B3R tells every game on the server when a character
// changes (cmd:char/<client>/<version>): it's asked again then.

#include <string>
#include <vector>
#include "client.h"
#include "cl_presence.h"
#include "cl_widgets.h"

#define INS_CMD				"inspect"
#define INS_RANGE			1024.0f		// whom /inspect picks
#define INS_HINT_RANGE		512.0f		// whom the hint is shown for
#define INS_HINT_WAIT_MS	300			// aimed at this long before C1B3R is asked
#define INS_ITEMS			24
#define INS_MESSAGE_MS		5000

#define IS_X			40.0f
#define IS_Y			34.0f
#define IS_W			560.0f
#define IS_H			412.0f
#define IS_TOP			( IS_Y + 42 )
#define IS_BOTTOM		( IS_Y + IS_H - 28 )
#define IS_MODEL_X		( IS_X + 12 )
#define IS_MODEL_W		190.0f
#define IS_RIGHT_X		( IS_MODEL_X + IS_MODEL_W + 14 )
#define IS_RIGHT_W		( IS_X + IS_W - 12 - IS_RIGHT_X )
#define IS_CTRL_H		16.0f
#define IS_TEXT			0.8f
#define IS_SMALL		0.65f
#define IS_ITEM			0.6f
#define IS_LINE_H		13.0f

static const float isPanel[4]		= { 0.0f, 0.0f, 0.08f, 0.88f };
static const float isBorder[4]		= { 0.5f, 0.5f, 0.7f, 1.0f };
static const float isItem[4]		= { 0.15f, 0.15f, 0.25f, 0.8f };
static const float isItemHover[4]	= { 0.25f, 0.25f, 0.4f, 0.9f };
static const float isItemOn[4]		= { 0.35f, 0.24f, 0.05f, 0.9f };
static const float isField[4]		= { 0.05f, 0.05f, 0.12f, 0.95f };
static const float isOrange[4]		= { 1.0f, 0.682f, 0.0f, 1.0f };
static const float isWhite[4]		= { 1.0f, 1.0f, 1.0f, 1.0f };
static const float isDim[4]			= { 0.6f, 0.6f, 0.7f, 1.0f };
static const float isYellow[4]		= { 1.0f, 0.85f, 0.3f, 1.0f };
static const float isGreen[4]		= { 0.4f, 1.0f, 0.4f, 1.0f };

// the character panel's fields, as C1B3R names them, and their longest (characters.py)
typedef enum {
	IF_NAME, IF_SPECIES, IF_AGE, IF_GENDER, IF_FACTION, IF_TITLE, IF_APPEARANCE, IF_BIOGRAPHY, IF_OOC,
	IF_POSE,		// the emote the model plays: <emote>|<torso animation>|<legs animation>
	IF_BLADE,		// the blades' colour: "<r> <g> <b>", 0-255; "" none lit
	IF_NUM
} insField_t;

static const struct {
	const char	*key;
	const char	*label;
	int			max;
} insFields[IF_NUM] = {
	{ "name",		"Name",			64 },
	{ "species",	"Species",		32 },
	{ "age",		"Age",			16 },
	{ "gender",		"Gender",		16 },
	{ "faction",	"Faction",		64 },
	{ "title",		"Title",		64 },
	{ "appearance",	"Appearance",	500 },
	{ "biography",	"Biography",	2000 },
	{ "ooc",		"OOC note",		200 },
	{ "pose",		"Pose",			96 },
	{ "blade",		"R G B",		16 },
};
#define INS_FIELD_LEN	2001

typedef struct {
	char	kind[16];
	char	name[64];
} insItem_t;

// a character as C1B3R said it
typedef struct {
	qboolean	known;			// answered
	qboolean	none;			// no character: their game isn't linked
	int			version;
	char		fields[IF_NUM][INS_FIELD_LEN];
	insItem_t	items[INS_ITEMS];
	int			numItems;
} insChar_t;

// what the hint knows of each player: whether they've a character, and its name
typedef struct {
	int		state;				// 0 not asked, 1 asked, 2 has one, 3 none
	char	slotName[MAX_INFO_VALUE];	// the player it was for (all of it: a coloured name is long), another in the slot asked again
	qboolean	linked;			// as C1B3R's list said when it was asked: linked since, asked again
	char	charName[64];
} insHint_t;

static struct {
	qboolean	open;
	int			font, titleFont;
	sfxHandle_t	clickSound;
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;

	int			client;			// whom
	insChar_t	ch;
	float		scroll;			// the panel, in pixels
	float		content;

	qboolean	editing;		// yours, its fields typed in
	char		edit[IF_NUM][INS_FIELD_LEN];
	int			focus;			// the field typed in, -1 none

	char		message[160];
	int			messageTime;
	qboolean	messageBad;
} ins;

static insHint_t	insHints[MAX_CLIENTS];
static struct {
	int		aimed, since;		// the player aimed at, from when
	int		asking;				// the client the request is for
} insHint;

static httpRequest_t insViewHttp, insSaveHttp, insHintHttp;
static int insViewFor = -1;		// the client the view request is for
static cvar_t *cl_inspectHint;

/*
===============================================================================

Talking to C1B3R

===============================================================================
*/

static void INS_Message( const char *text, qboolean bad ) {
	Q_strncpyz( ins.message, text, sizeof( ins.message ) );
	for ( char *nl = strchr( ins.message, '\n' ); nl; nl = NULL )
		*nl = '\0';
	ins.messageTime = cls.realtime;
	ins.messageBad = bad;
}

static qboolean INS_Post( httpRequest_t *req, const char *path, const std::string &fields ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session || req->state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += fields + "}";
	return CL_Presence_Post( req, path, body );
}

static void INS_AskView( void ) {
	if ( insViewHttp.state == HTTP_BUSY )
		return;
	if ( INS_Post( &insViewHttp, "/v2/char/view", va( ",\"client\":%i", ins.client ) ) )
		insViewFor = ins.client;
}

// a line's tab-separated fields: how many
static int INS_Split( char *line, char **field, int max ) {
	int n = 0;

	for ( char *f = line; f && n < max; n++ ) {
		field[n] = f;
		f = strchr( f, '\t' );
		if ( f )
			*f++ = '\0';
	}
	return n;
}

// "none", or ok <version>, then: field <name> <value>, holster ..., item <id> <kind> <shown> <name> ...
static void INS_ReadView( const char *answer, insChar_t *ch ) {
	std::string all( answer );
	size_t at = 0;
	qboolean first = qtrue;

	memset( ch, 0, sizeof( *ch ) );
	ch->known = qtrue;
	while ( at < all.size() ) {
		size_t end = all.find( '\n', at );
		std::string text;
		char *f[8];

		if ( end == std::string::npos )
			end = all.size();
		text = all.substr( at, end - at );
		at = end + 1;
		std::vector<char> line( text.begin(), text.end() );
		line.push_back( '\0' );
		const int n = INS_Split( line.data(), f, 8 );

		if ( first ) {
			first = qfalse;
			if ( strcmp( f[0], "ok" ) ) {
				ch->none = qtrue;
				return;
			}
			ch->version = n > 1 ? atoi( f[1] ) : 0;
			continue;
		}
		if ( !strcmp( f[0], "field" ) && n >= 3 ) {
			for ( int i = 0; i < IF_NUM; i++ ) {
				if ( !strcmp( f[1], insFields[i].key ) )
					Q_strncpyz( ch->fields[i], CL_Presence_Unfield( f[2] ).c_str(), sizeof( ch->fields[i] ) );
			}
		} else if ( !strcmp( f[0], "item" ) && n >= 5 && ch->numItems < INS_ITEMS ) {
			insItem_t *it = &ch->items[ch->numItems++];

			Q_strncpyz( it->kind, f[2], sizeof( it->kind ) );
			Q_strncpyz( it->name, CL_Presence_Unfield( f[4] ).c_str(), sizeof( it->name ) );
		}
	}
}

// the panel saved: every field, empty ones too
static void INS_Save( void ) {
	std::string fields = ",\"fields\":{";

	for ( int i = 0; i < IF_NUM; i++ ) {
		fields += va( "%s\"%s\":", i ? "," : "", insFields[i].key );
		CL_Presence_Json( fields, ins.edit[i] );
	}
	fields += "}";
	if ( !INS_Post( &insSaveHttp, "/v2/char/profile/set", fields ) )
		INS_Message( CL_Presence_Session() ? "Wait: still saving" : "Not connected to C1B3R", qtrue );
}

/*
===============================================================================

The hint under the crosshair

===============================================================================
*/

static insHint_t *INS_HintFor( int client ) {
	insHint_t *h = &insHints[client];

	// someone else in the slot now, or they've linked since
	if ( Q_stricmp( h->slotName, CL_PlayerSlotName( client ) ) || h->linked != CL_Presence_Linked( client ) ) {
		memset( h, 0, sizeof( *h ) );
		Q_strncpyz( h->slotName, CL_PlayerSlotName( client ), sizeof( h->slotName ) );
		h->linked = CL_Presence_Linked( client );
	}
	return h;
}

static void INS_HintFrame( void ) {
	const int aimed = cls.state == CA_ACTIVE ? CL_AimedPlayer( INS_HINT_RANGE ) : -1;

	if ( aimed != insHint.aimed ) {
		insHint.aimed = aimed;
		insHint.since = cls.realtime;
	}
	if ( aimed < 0 || !cl_inspectHint->integer || cls.realtime - insHint.since < INS_HINT_WAIT_MS )
		return;
	if ( INS_HintFor( aimed )->state == 0 && insHintHttp.state != HTTP_BUSY
		&& INS_Post( &insHintHttp, "/v2/char/view", va( ",\"client\":%i", aimed ) ) ) {
		insHints[aimed].state = 1;
		insHint.asking = aimed;
	}
}

void CL_Inspect_DrawHint( void ) {
	const int key = Key_GetKey( INS_CMD );
	const insHint_t *h;
	const char *text;
	vec4_t color = { 1.0f, 1.0f, 1.0f, 0.85f };

	if ( !cl_inspectHint || !cl_inspectHint->integer || cls.state != CA_ACTIVE || insHint.aimed < 0 || Key_GetCatcher()
		|| !Cvar_VariableIntegerValue( "cg_draw2D" ) || cls.realtime - insHint.since < INS_HINT_WAIT_MS )
		return;
	h = &insHints[insHint.aimed];
	if ( h->state != 2 )
		return;
	text = va( "%s" S_COLOR_GREY "   %s: inspect", h->charName[0] ? h->charName : "A character", key >= 0 ? Key_KeynumToString( key ) : "/" INS_CMD );
	WG_TypedText( 0, SCREEN_HEIGHT / 2 + 28, SCREEN_WIDTH, text, color, cls.menuFont, 0.6f, qtrue, qtrue );
}

/*
===============================================================================

Each frame, and C1B3R's word that a character changed

===============================================================================
*/

void CL_Inspect_Frame( void ) {
	if ( HTTP_Poll( &insViewHttp ) ) {
		if ( insViewHttp.status == 200 && ins.open && insViewFor == ins.client ) {
			INS_ReadView( insViewHttp.answer, &ins.ch );
			// the hint knows too
			if ( ins.client >= 0 && ins.client < MAX_CLIENTS ) {
				insHint_t *h = INS_HintFor( ins.client );

				h->state = ins.ch.none ? 3 : 2;
				Q_strncpyz( h->charName, ins.ch.fields[IF_NAME], sizeof( h->charName ) );
			}
		} else if ( insViewHttp.status != 200 && ins.open ) {
			ins.ch.known = qtrue;
			ins.ch.none = qtrue;
			INS_Message( insViewHttp.answer[0] ? insViewHttp.answer : "C1B3R didn't answer", qtrue );
		}
	}
	if ( HTTP_Poll( &insSaveHttp ) ) {
		if ( insSaveHttp.status == 200 ) {
			INS_Message( "Saved", qfalse );
			ins.editing = qfalse;
			ins.focus = -1;
			INS_AskView();
		} else {
			INS_Message( insSaveHttp.answer[0] ? insSaveHttp.answer : "C1B3R didn't answer", qtrue );
		}
	}
	if ( HTTP_Poll( &insHintHttp ) && insHint.asking >= 0 && insHint.asking < MAX_CLIENTS ) {
		insHint_t *h = &insHints[insHint.asking];

		if ( insHintHttp.status == 200 ) {
			static insChar_t ch;

			INS_ReadView( insHintHttp.answer, &ch );
			h->state = ch.none ? 3 : 2;
			Q_strncpyz( h->charName, ch.fields[IF_NAME], sizeof( h->charName ) );
		} else {
			h->state = 3;
		}
	}
	if ( ins.open && !ins.ch.known && CL_Presence_Session() )
		INS_AskView();
	if ( cl_inspectHint )
		INS_HintFrame();
}

// cmd:char/<client>/<version>: a character on this server changed
void CL_Inspect_CharHook( char **argv, int argc ) {
	int client;

	if ( argc < 1 || !argv[0][0] )
		return;
	client = atoi( argv[0] );
	if ( client < 0 || client >= MAX_CLIENTS )
		return;
	insHints[client].state = 0;
	if ( ins.open && ins.client == client && !ins.editing && ( argc < 2 || atoi( argv[1] ) != ins.ch.version ) )
		INS_AskView();
}

/*
===============================================================================

Drawing: the tools' look

===============================================================================
*/

static void INS_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void INS_Box( float x, float y, float w, float h, const float *fill ) {
	INS_Fill( x, y, w, h, fill );
	INS_Fill( x, y, w, 1, isBorder );
	INS_Fill( x, y + h - 1, w, 1, isBorder );
	INS_Fill( x, y, 1, h, isBorder );
	INS_Fill( x + w - 1, y, 1, h, isBorder );
}

// one line, colour codes in it drawn as colours (RPMod's too); cut with ... to fit w
static void INS_Text( float x, float y, float w, const char *text, const float *color, int font, float scale, qboolean centred ) {
	WG_TypedText( x, y, w, text, color, font, scale, centred, qtrue );
}

static qboolean INS_InRect( float x, float y, float w, float h ) {
	return (qboolean)( ins.cursorX >= x && ins.cursorX < x + w && ins.cursorY >= y && ins.cursorY < y + h );
}

static qboolean INS_Clicked( float x, float y, float w, float h ) {
	if ( !ins.click || ins.clickX < x || ins.clickX >= x + w || ins.clickY < y || ins.clickY >= y + h )
		return qfalse;
	ins.click = qfalse;
	if ( ins.clickSound )
		S_StartLocalSound( ins.clickSound, CHAN_LOCAL_SOUND );
	return qtrue;
}

static qboolean INS_Button( float x, float y, float w, const char *label, qboolean on, qboolean enabled ) {
	INS_Box( x, y, w, IS_CTRL_H, on ? isItemOn : enabled && INS_InRect( x, y, w, IS_CTRL_H ) ? isItemHover : isItem );
	INS_Text( x, y + 2, w, label, enabled ? isWhite : isDim, ins.font, IS_TEXT, qtrue );
	return enabled ? INS_Clicked( x, y, w, IS_CTRL_H ) : qfalse;
}

static void INS_Style( wgStyle_t *style ) {
	memset( style, 0, sizeof( *style ) );
	style->font = ins.font;
	style->scale = IS_TEXT;
	style->lineH = IS_LINE_H;
	style->padX = 4;
	style->padY = 2;
	style->fill = isField;
	style->border = isBorder;
	style->text = isWhite;
	style->dim = isDim;
}

// wrapped text from *y, moved down past it; only its lines between top and bottom drawn
static void INS_Para( float x, float *y, float w, const char *text, const float *color, float top, float bottom ) {
	wgStyle_t style;
	int lines, first, count;

	INS_Style( &style );
	lines = WG_TextLines( &style, text, w );
	first = *y < top ? (int)ceilf( ( top - *y ) / IS_LINE_H ) : 0;
	count = Q_min( lines - first, (int)( ( bottom - Q_max( *y, top ) ) / IS_LINE_H ) );
	if ( count > 0 && first < lines )
		WG_DrawText( &style, x, *y + first * IS_LINE_H, w, text, first, count, color );
	*y += lines * IS_LINE_H;
}

// one line, drawn only when it's between top and bottom
static void INS_Line( float x, float *y, float w, const char *text, const float *color, int font, float scale, float h, float top, float bottom ) {
	if ( *y >= top && *y + h <= bottom )
		INS_Text( x, *y, w, text, color, font, scale, qfalse );
	*y += h;
}

// a text field, known by its number
static void INS_Field( float x, float y, float w, float h, int which, qboolean box ) {
	wgStyle_t style;
	char *buf = ins.edit[which];
	const qboolean focused = (qboolean)( ins.focus == which );

	INS_Style( &style );
	INS_Box( x, y, w, h, isField );
	if ( focused )
		INS_Fill( x, y + h - 1, w, 1, isOrange );		// the one typed in
	if ( box )
		WG_TextBox( &style, x, y, w, h, buf, focused, insFields[which].label, ins.cursorX, ins.cursorY );
	else
		WG_Field( &style, x, y, w, h, buf, focused, insFields[which].label, ins.cursorX, ins.cursorY );
	// a field grows down while it's typed in; a box doesn't
	if ( INS_Clicked( x, y, w, box ? h : WG_FieldHeight( &style, w, h, buf, focused ) ) ) {
		if ( !focused )
			WG_EditReset();
		ins.focus = which;
		WG_FieldClick( buf, ins.clickX, ins.clickY );
	}
}

// each kind's picture, small
static void INS_Icon( const char *kind, float x, float y, float size ) {
	INS_Box( x, y, size, size, isField );
	if ( !Q_stricmp( kind, "hilt" ) ) {
		INS_Fill( x + size * 0.44f, y + size * 0.38f, size * 0.12f, size * 0.5f, isOrange );
		INS_Fill( x + size * 0.47f, y + size * 0.1f, size * 0.06f, size * 0.26f, isGreen );
	} else if ( !Q_stricmp( kind, "datapad" ) ) {
		INS_Fill( x + size * 0.28f, y + size * 0.15f, size * 0.44f, size * 0.7f, isOrange );
	} else {
		INS_Fill( x + size * 0.26f, y + size * 0.3f, size * 0.48f, size * 0.44f, isOrange );
	}
}

// a pose's animations: <emote>|<torso>|<legs>; "" when it has none
static void INS_PoseAnims( const char *pose, char *torso, char *legs ) {
	const char *a = strchr( pose, '|' ), *b = a ? strchr( a + 1, '|' ) : NULL;

	torso[0] = legs[0] = '\0';
	if ( !a )
		return;
	Q_strncpyz( torso, a + 1, b ? Q_min( MAX_QPATH, (int)( b - a ) ) : MAX_QPATH );
	if ( b )
		Q_strncpyz( legs, b + 1, MAX_QPATH );
}

// its emote's name: "" when it's the stance
static void INS_PoseEmote( const char *pose, char *out, int size ) {
	const char *a = strchr( pose, '|' );

	Q_strncpyz( out, pose, a ? Q_min( size, (int)( a - pose ) + 1 ) : size );
}

// "r g b", 0-255, as 0..1: qfalse when it isn't one
static qboolean INS_BladeRGB( const char *text, vec3_t rgb ) {
	int r, g, b;

	if ( sscanf( text, "%i %i %i", &r, &g, &b ) != 3 )
		return qfalse;
	VectorSet( rgb, Com_Clampi( 0, 255, r ) / 255.0f, Com_Clampi( 0, 255, g ) / 255.0f, Com_Clampi( 0, 255, b ) / 255.0f );
	return qtrue;
}

static qboolean INS_IsMe( void ) {
	return (qboolean)( ins.client == clc.clientNum );
}

/*
===============================================================================

The window

===============================================================================
*/

// their character panel, scrolled
static void INS_DrawPanel( void ) {
	const float x = IS_RIGHT_X, w = IS_RIGHT_W, top = IS_TOP, bottom = IS_BOTTOM - ( INS_IsMe() ? IS_CTRL_H + 8 : 0 );
	const insChar_t *ch = &ins.ch;
	float y;
	char line[256];

	if ( !CL_Presence_Session() ) {
		INS_Text( x, top + 40, w, "Not connected to C1B3R", isOrange, ins.titleFont, IS_ITEM + 0.1f, qtrue );
		INS_Text( x, top + 64, w, "Character panels are kept there: /c1b3r says why", isDim, ins.font, IS_TEXT, qtrue );
		return;
	}
	if ( !ch->known ) {
		INS_Text( x, top + 40, w, "Asking C1B3R...", isDim, ins.font, IS_TEXT, qtrue );
		return;
	}
	if ( ch->none ) {
		INS_Text( x, top + 40, w, "No character panel", isOrange, ins.titleFont, IS_ITEM + 0.1f, qtrue );
		INS_Text( x, top + 64, w, INS_IsMe() ? "Link your game to C1B3R (/c1b3r link) to have one" : "Their game isn't linked to C1B3R, or hasn't this client",
			isDim, ins.font, IS_TEXT, qtrue );
		return;
	}

	ins.scroll = Com_Clamp( 0, Q_max( 0.0f, ins.content - ( bottom - top ) ), ins.scroll );
	y = top - ins.scroll;
	INS_Line( x, &y, w, ch->fields[IF_NAME][0] ? ch->fields[IF_NAME] : "No name given", ch->fields[IF_NAME][0] ? isOrange : isDim,
		ins.titleFont, IS_ITEM + 0.15f, 24, top, bottom );
	if ( ch->fields[IF_TITLE][0] || ch->fields[IF_FACTION][0] ) {
		Com_sprintf( line, sizeof( line ), "%s%s%s", ch->fields[IF_TITLE], ch->fields[IF_TITLE][0] && ch->fields[IF_FACTION][0] ? ", " : "",
			ch->fields[IF_FACTION] );
		INS_Line( x, &y, w, line, isWhite, ins.font, IS_TEXT, 16, top, bottom );
	}
	line[0] = '\0';
	for ( int i = IF_SPECIES; i <= IF_GENDER; i++ ) {
		if ( ch->fields[i][0] )
			Q_strcat( line, sizeof( line ), va( "%s" S_COLOR_GREY "%s: " S_COLOR_WHITE "%s", line[0] ? "     " : "", insFields[i].label, ch->fields[i] ) );
	}
	if ( line[0] )
		INS_Line( x, &y, w, line, isWhite, ins.font, IS_TEXT, 16, top, bottom );
	y += 6;
	for ( int i = IF_APPEARANCE; i <= IF_BIOGRAPHY; i++ ) {
		if ( !ch->fields[i][0] )
			continue;
		INS_Line( x, &y, w, insFields[i].label, isOrange, ins.font, IS_TEXT, 14, top, bottom );
		INS_Para( x + 6, &y, w - 6, ch->fields[i], isWhite, top, bottom );
		y += 8;
	}
	if ( ch->numItems ) {
		INS_Line( x, &y, w, "Carries", isOrange, ins.font, IS_TEXT, 16, top, bottom );
		for ( int i = 0; i < ch->numItems; i++ ) {
			if ( y >= top && y + 16 <= bottom ) {
				INS_Icon( ch->items[i].kind, x + 6, y, 14 );
				INS_Text( x + 26, y, w - 26, ch->items[i].name, isWhite, ins.font, IS_TEXT, qfalse );
			}
			y += 17;
		}
		y += 6;
	}
	if ( CL_Holster_Missing( ins.client )[0] ) {
		INS_Line( x, &y, w, "Worn, but you haven't the model", isDim, ins.font, IS_TEXT, 14, top, bottom );
		INS_Para( x + 6, &y, w - 6, CL_Holster_Missing( ins.client ), isDim, top, bottom );
		y += 8;
	}
	if ( ch->fields[IF_OOC][0] ) {
		INS_Line( x, &y, w, "OOC", isDim, ins.font, IS_TEXT, 14, top, bottom );
		INS_Para( x + 6, &y, w - 6, ch->fields[IF_OOC], isDim, top, bottom );
	}
	if ( y + ins.scroll - top <= 34 && !ch->numItems )
		INS_Line( x, &y, w, INS_IsMe() ? "Nothing written yet: Edit fills it in" : "Nothing written yet", isDim, ins.font, IS_TEXT, 16, top, bottom );
	ins.content = y + ins.scroll - top;
	if ( ins.content > bottom - top ) {
		const float h = bottom - top;

		INS_Fill( x + w + 4, top, 3, h, isItem );
		INS_Fill( x + w + 4, top + h * ins.scroll / ins.content, 3, Q_max( 8.0f, h * h / ins.content ), isOrange );
	}
}

// the next or the last emote for the pose; the stance before the first
static void INS_StepPose( int dir ) {
	char emote[64];
	const int count = CL_EmotePreview_Count();
	int at = -1;

	INS_PoseEmote( ins.edit[IF_POSE], emote, sizeof( emote ) );
	for ( int i = 0; i < count && emote[0]; i++ ) {
		if ( !Q_stricmp( CL_EmotePreview_Name( i ), emote ) )
			at = i;
	}
	at += dir;
	if ( at < -1 )
		at = count - 1;
	else if ( at >= count )
		at = -1;
	if ( at < 0 ) {
		ins.edit[IF_POSE][0] = '\0';
		return;
	}
	{
		const char *name = CL_EmotePreview_Name( at ), *torso = CL_EmotePreview_Anim( name ), *legs = CL_EmotePreview_Legs( name );

		Com_sprintf( ins.edit[IF_POSE], insFields[IF_POSE].max + 1, "%s|%s|%s", name, torso ? torso : "", legs ? legs : "" );
	}
}

// under your model, while it's edited: the pose it plays and its blades' colour
static void INS_DrawLook( float y ) {
	static const struct {
		const char	*name;
		int			r, g, b;
	} swatches[] = {
		{ "red", 255, 40, 40 }, { "orange", 255, 128, 20 }, { "yellow", 255, 240, 40 }, { "green", 40, 255, 40 },
		{ "blue", 40, 100, 255 }, { "purple", 190, 40, 255 }, { "white", 255, 255, 255 },
	};
	const float x = IS_MODEL_X, w = IS_MODEL_W, label = 38;
	char emote[64];
	vec3_t rgb;

	// the pose: one of RPMod's emotes, or the stance
	INS_PoseEmote( ins.edit[IF_POSE], emote, sizeof( emote ) );
	INS_Text( x, y + 2, label, "Pose", isOrange, ins.font, IS_SMALL, qfalse );
	if ( INS_Button( x + label, y, 18, "<", qfalse, qtrue ) )
		INS_StepPose( -1 );
	INS_Box( x + label + 20, y, w - label - 40, IS_CTRL_H, isField );
	INS_Text( x + label + 20, y + 2, w - label - 40, emote[0] ? emote : "Stance", emote[0] ? isWhite : isDim, ins.font, IS_TEXT, qtrue );
	if ( INS_Button( x + w - 18, y, 18, ">", qfalse, qtrue ) )
		INS_StepPose( 1 );
	y += IS_CTRL_H + 5;

	// the blades: typed, or picked from the swatches; Off for none lit
	INS_Text( x, y + 2, label, "Blade", isOrange, ins.font, IS_SMALL, qfalse );
	INS_Field( x + label, y, w - label - 64, IS_CTRL_H, IF_BLADE, qfalse );
	if ( INS_BladeRGB( ins.edit[IF_BLADE], rgb ) ) {
		const float c[4] = { rgb[0], rgb[1], rgb[2], 1.0f };

		INS_Box( x + w - 60, y, 22, IS_CTRL_H, c );
	}
	if ( INS_Button( x + w - 34, y, 34, "Off", (qboolean)!ins.edit[IF_BLADE][0], qtrue ) ) {
		ins.edit[IF_BLADE][0] = '\0';
		if ( ins.focus == IF_BLADE )
			ins.focus = -1;
	}
	y += IS_CTRL_H + 5;
	{
		const float sw = ( w - ( ARRAY_LEN( swatches ) - 1 ) * 3 ) / ARRAY_LEN( swatches );

		for ( int i = 0; i < (int)ARRAY_LEN( swatches ); i++ ) {
			const float sx = x + i * ( sw + 3 );
			const float c[4] = { swatches[i].r / 255.0f, swatches[i].g / 255.0f, swatches[i].b / 255.0f, 1.0f };

			INS_Box( sx, y, sw, 12, c );
			if ( INS_InRect( sx, y, sw, 12 ) )
				INS_Fill( sx, y + 11, sw, 1, isWhite );
			if ( INS_Clicked( sx, y, sw, 12 ) )
				Com_sprintf( ins.edit[IF_BLADE], insFields[IF_BLADE].max + 1, "%i %i %i", swatches[i].r, swatches[i].g, swatches[i].b );
		}
	}
}

// yours, typed in
static void INS_DrawEdit( void ) {
	const float x = IS_RIGHT_X, w = IS_RIGHT_W, half = ( w - 8 ) * 0.5f;
	float y = IS_TOP;

	INS_Field( x, y, w, IS_CTRL_H, IF_NAME, qfalse );
	y += IS_CTRL_H + 5;
	INS_Field( x, y, half, IS_CTRL_H, IF_SPECIES, qfalse );
	INS_Field( x + half + 8, y, half, IS_CTRL_H, IF_AGE, qfalse );
	y += IS_CTRL_H + 5;
	INS_Field( x, y, half, IS_CTRL_H, IF_GENDER, qfalse );
	INS_Field( x + half + 8, y, half, IS_CTRL_H, IF_FACTION, qfalse );
	y += IS_CTRL_H + 5;
	INS_Field( x, y, w, IS_CTRL_H, IF_TITLE, qfalse );
	y += IS_CTRL_H + 9;
	INS_Text( x, y, w, "Appearance", isOrange, ins.font, IS_SMALL, qfalse );
	y += 12;
	INS_Field( x, y, w, 56, IF_APPEARANCE, qtrue );
	y += 61;
	INS_Text( x, y, w, va( "Biography" S_COLOR_GREY "  (%i of %i)", (int)strlen( ins.edit[IF_BIOGRAPHY] ), insFields[IF_BIOGRAPHY].max ),
		isOrange, ins.font, IS_SMALL, qfalse );
	y += 12;
	INS_Field( x, y, w, IS_BOTTOM - IS_CTRL_H - 8 - 50 - y, IF_BIOGRAPHY, qtrue );
	y = IS_BOTTOM - IS_CTRL_H - 8 - 44;
	INS_Text( x, y, w, "OOC note: anything out of character (how to reach you, what you're up for)", isOrange, ins.font, IS_SMALL, qfalse );
	y += 12;
	INS_Field( x, y, w, IS_CTRL_H, IF_OOC, qfalse );
}

static void INS_Close( void ) {
	if ( !ins.open )
		return;
	ins.open = qfalse;
	ins.editing = qfalse;
	ins.focus = -1;
	CL_Presets_FreeCharacter();
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_INSPECT );
}

void CL_Inspect_Close( void ) {
	INS_Close();
}

static void INS_StartEdit( void ) {
	ins.editing = qtrue;
	ins.focus = IF_NAME;
	WG_EditReset();
	for ( int i = 0; i < IF_NUM; i++ )
		Q_strncpyz( ins.edit[i], ins.ch.fields[i], insFields[i].max + 1 );
}

void CL_Inspect_Draw( void ) {
	char model[MAX_QPATH], saber1[MAX_QPATH], saber2[MAX_QPATH];
	const char *error;

	if ( !ins.open )
		return;
	// they left
	if ( !CL_PlayerSlotName( ins.client )[0] ) {
		INS_Close();
		return;
	}
	INS_Box( IS_X, IS_Y, IS_W, IS_H, isPanel );
	INS_Text( IS_MODEL_X, IS_Y + 8, 120, INS_IsMe() ? "YOU" : "INSPECT", isOrange, ins.titleFont, IS_ITEM + 0.2f, qfalse );
	INS_Text( IS_MODEL_X + 110, IS_Y + 12, IS_W - 200, CL_PlayerSlotName( ins.client ), isWhite, ins.font, IS_TEXT, qfalse );
	if ( INS_Button( IS_X + IS_W - 12 - 56, IS_Y + 10, 56, "Close", qfalse, qtrue ) ) {
		INS_Close();
		return;
	}

	// their model, turning slowly: posed, its blades lit, as their panel says (yours as you type it)
	Q_strncpyz( model, CL_PlayerInfo( ins.client, "model" ), sizeof( model ) );
	Q_strncpyz( saber1, CL_PlayerInfo( ins.client, "st" ), sizeof( saber1 ) );
	Q_strncpyz( saber2, CL_PlayerInfo( ins.client, "st2" ), sizeof( saber2 ) );
	{
		const char *pose = ins.editing ? ins.edit[IF_POSE] : ins.ch.fields[IF_POSE];
		const char *blade = ins.editing ? ins.edit[IF_BLADE] : ins.ch.fields[IF_BLADE];
		const float modelBottom = ins.editing ? IS_BOTTOM - 74 : IS_BOTTOM;
		char torso[MAX_QPATH], legs[MAX_QPATH];
		vec3_t rgb;
		const qboolean lit = INS_BladeRGB( blade, rgb );

		INS_PoseAnims( pose, torso, legs );
		INS_Box( IS_MODEL_X, IS_TOP - 4, IS_MODEL_W, modelBottom - IS_TOP + 4, isField );
		CL_Presets_DrawCharacter( model, saber1, saber2, torso, legs, lit ? rgb : NULL, IS_MODEL_X + 1, IS_TOP - 3, IS_MODEL_W - 2,
			modelBottom - IS_TOP + 2, (float)( cls.realtime % 20000 ) * 0.018f, CL_Holster_DrawOn, (void *)(intptr_t)ins.client );
		if ( ( error = CL_Presets_CharacterError() ) != NULL )
			INS_Text( IS_MODEL_X, ( IS_TOP + modelBottom ) * 0.5f, IS_MODEL_W, error, isDim, ins.font, IS_SMALL, qtrue );
		if ( ins.editing )
			INS_DrawLook( modelBottom + 6 );
	}

	if ( ins.editing )
		INS_DrawEdit();
	else
		INS_DrawPanel();

	// yours: edited and saved here
	if ( INS_IsMe() && ins.ch.known && !ins.ch.none ) {
		const float by = IS_BOTTOM - IS_CTRL_H;

		if ( ins.editing ) {
			if ( INS_Button( IS_RIGHT_X, by, 80, "Cancel", qfalse, qtrue ) ) {
				ins.editing = qfalse;
				ins.focus = -1;
			}
			if ( INS_Button( IS_RIGHT_X + IS_RIGHT_W - 120, by, 120, insSaveHttp.state == HTTP_BUSY ? "Saving..." : "Save", qtrue, qtrue ) ) {
				ins.focus = -1;
				INS_Save();
			}
		} else if ( INS_Button( IS_RIGHT_X + IS_RIGHT_W - 120, by, 120, "Edit", qfalse, qtrue ) ) {
			INS_StartEdit();
		}
	}

	if ( ins.message[0] && cls.realtime - ins.messageTime < INS_MESSAGE_MS )
		INS_Text( IS_MODEL_X, IS_Y + IS_H - 20, IS_W - 24, ins.message, ins.messageBad ? isYellow : isGreen, ins.font, IS_TEXT, qfalse );
	else if ( !ins.editing )
		INS_Text( IS_MODEL_X, IS_Y + IS_H - 20, IS_W - 24, INS_IsMe() ? "Everyone with this client sees this when they inspect you."
			: "Kept by C1B3R for their RPMod account.", isDim, ins.font, IS_TEXT, qfalse );
	ins.click = qfalse;
	WG_DrawOverlay();
	re->SetColor( NULL );
	re->DrawStretchPic( ins.cursorX, ins.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

static void INS_Open( int client ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Inspect: join a server first\n" );
		return;
	}
	if ( client < 0 || client >= MAX_CLIENTS || !CL_PlayerSlotName( client )[0] ) {
		Com_Printf( "Inspect: nobody's there\n" );
		return;
	}
	if ( !ins.open ) {
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
		CL_Holster_Close();
		CL_Characters_Close();
		CL_Datapad_Close();

		ins.font = re->RegisterFont( "arialnb" );
		if ( !ins.font )
			ins.font = cls.menuFont;
		ins.titleFont = re->RegisterFont( "ergoec" );
		if ( !ins.titleFont )
			ins.titleFont = ins.font;
		ins.clickSound = S_RegisterSound( "sound/interface/button1.wav" );
		ins.cursorX = SCREEN_WIDTH * 0.5f;
		ins.cursorY = SCREEN_HEIGHT * 0.5f;
		Key_SetCatcher( Key_GetCatcher() | KEYCATCH_INSPECT );
	}
	ins.open = qtrue;
	ins.click = qfalse;
	ins.client = client;
	ins.scroll = 0;
	ins.editing = qfalse;
	ins.focus = -1;
	memset( &ins.ch, 0, sizeof( ins.ch ) );
	if ( !CL_Presence_Session() )
		ins.ch.known = qtrue;
}

/*
===============================================================================

Keys, the mouse, and the command

===============================================================================
*/

static qboolean INS_IsToggleKey( int key ) {
	const char *binding = Key_GetBinding( key );

	return (qboolean)( VALIDSTRING( binding ) && !Q_stricmp( binding, INS_CMD ) );
}

void CL_Inspect_KeyEvent( int key, qboolean down ) {
	if ( !ins.open || !down )
		return;
	if ( key == A_MOUSE1 ) {
		ins.click = qtrue;
		ins.clickX = ins.cursorX;
		ins.clickY = ins.cursorY;
		return;
	}
	if ( ins.editing && ins.focus >= 0 ) {
		char *buf = ins.edit[ins.focus];
		const int size = insFields[ins.focus].max + 1;
		const qboolean box = (qboolean)( ins.focus == IF_APPEARANCE || ins.focus == IF_BIOGRAPHY );

		if ( ( key == A_ENTER || key == A_KP_ENTER ) && box ) {
			WG_EditChar( buf, size, '\n', NULL );
		} else if ( key == A_TAB ) {
			do
				ins.focus = ( ins.focus + ( kg.keys[A_SHIFT].down ? IF_NUM - 1 : 1 ) ) % IF_NUM;
			while ( ins.focus == IF_POSE );
			WG_EditReset();
		} else if ( key == A_ENTER || key == A_KP_ENTER ) {
			ins.focus = -1;
			WG_EditReset();
		} else {
			WG_EditKey( buf, size, key, NULL );
		}
		return;
	}
	if ( INS_IsToggleKey( key ) ) {
		INS_Close();
		return;
	}
	switch ( key ) {
	case A_MWHEELUP:
	case A_CURSOR_UP:
		ins.scroll -= IS_LINE_H * 3;
		break;
	case A_MWHEELDOWN:
	case A_CURSOR_DOWN:
		ins.scroll += IS_LINE_H * 3;		// held in range as it's drawn
		break;
	default:
		break;
	}
}

void CL_Inspect_CharEvent( int ch ) {
	if ( ins.open && ins.editing && ins.focus >= 0 )
		WG_EditChar( ins.edit[ins.focus], insFields[ins.focus].max + 1, ch, NULL );
}

void CL_Inspect_MouseEvent( int dx, int dy ) {
	ins.cursorX = Com_Clamp( 0, SCREEN_WIDTH, ins.cursorX + dx );
	ins.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, ins.cursorY + dy );
}

// a field typed in first, then the edit (kept for Save), then closed
void CL_Inspect_Escape( void ) {
	if ( ins.editing && ins.focus >= 0 ) {
		ins.focus = -1;
		WG_EditReset();
		return;
	}
	INS_Close();
}

void CL_Inspect_Disconnect( void ) {
	INS_Close();
	memset( insHints, 0, sizeof( insHints ) );
	insHint.aimed = insHint.asking = -1;
}

// a player by their client number, or a part of their name (colours left out)
static int INS_FindPlayer( const char *arg ) {
	char want[MAX_NETNAME], name[MAX_NETNAME];

	if ( arg[0] >= '0' && arg[0] <= '9' )
		return atoi( arg );
	Q_strncpyz( want, arg, sizeof( want ) );
	Q_StripColor( want );
	for ( int c = 0; c < MAX_CLIENTS; c++ ) {
		Q_strncpyz( name, CL_PlayerSlotName( c ), sizeof( name ) );
		Q_StripColor( name );
		if ( name[0] && Q_stristr( name, want ) )
			return c;
	}
	return -1;
}

static void INS_Inspect_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !arg[0] ) {
		const int aimed = CL_AimedPlayer( INS_RANGE );

		// the key again closes it
		if ( ins.open ) {
			INS_Close();
			return;
		}
		INS_Open( aimed >= 0 ? aimed : clc.clientNum );
	} else if ( !Q_stricmp( arg, "me" ) ) {
		INS_Open( clc.clientNum );
	} else if ( !Q_stricmp( arg, "edit" ) ) {
		INS_Open( clc.clientNum );
		if ( ins.open && ins.ch.known && !ins.ch.none )
			INS_StartEdit();
	} else if ( !Q_stricmp( arg, "help" ) || !Q_stricmp( arg, "?" ) ) {
		Com_Printf( "usage: " INS_CMD "                 the player you aim at, or yourself (again: closes it)\n" );
		Com_Printf( "       " INS_CMD " me              your own character panel, to edit it\n" );
		Com_Printf( "       " INS_CMD " <client|name>   a player by their client number or a part of their name\n" );
		Com_Printf( "cl_inspectHint 0|1: their character's name under the crosshair when you aim at them\n" );
	} else {
		const int client = INS_FindPlayer( Cmd_Args() );

		if ( client < 0 )
			Com_Printf( "Inspect: no player called %s\n", Cmd_Args() );
		else
			INS_Open( client );
	}
}

void CL_Inspect_Init( void ) {
	memset( &ins, 0, sizeof( ins ) );
	ins.focus = -1;
	insHint.aimed = insHint.asking = -1;
	cl_inspectHint = Cvar_Get( "cl_inspectHint", "1", CVAR_ARCHIVE_ND, "Aiming at a player who has a character panel shows their character's name and the key that inspects them" );
	Cmd_AddCommand( INS_CMD, INS_Inspect_f, "A player's character: their model, character panel and what they show (yours, to edit it)" );
}

void CL_Inspect_Shutdown( void ) {
	INS_Close();
}
