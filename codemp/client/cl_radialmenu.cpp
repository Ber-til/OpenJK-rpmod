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

// cl_radialmenu.cpp -- radial menus: commands laid round the cursor, picked with the mouse or a number key
//
//   +radial [menu]     on a key: held, the menu opens round the middle of the screen; point the
//                      mouse at an item and let go to run it. A quick tap stays open until a
//                      click, a number key, Esc or the key again
//   radial [menu]      opens it, staying open the same way; an item can open another menu with it
//   radialsettings     makes the menus, fills them and binds keys to them
//
// The menus are kept in radialmenus.txt; the first time there's one for the tools, the one a
// bare +radial opens, one for the saber styles and one for the gestures. While one is open the mouse points instead of turning the view, and
// the number keys and the mouse buttons go to it; moving and every other key work as usual.

#include "client.h"

#define RM_FILE				"radialmenus.txt"
#define RM_COMMAND			"radialsettings"
#define RM_MAX_MENUS		24
#define RM_MAX_ITEMS		10		// the number keys: 1 to 9, then 0
#define RM_NAME_LEN			32
#define RM_LABEL_LEN		32
#define RM_CMD_LEN			256
#define RM_TAP_MS			300		// let go this soon, pointing at nothing, and it stays open
#define RM_SAVE_MS			1000	// typing stops this long before it's saved

// the menu, in 640x480 around its middle; more than six items push the ring out
#define RM_RADIUS			100.0f
#define RM_RADIUS_STEP		15.0f
#define RM_REACH			20.0f	// the pointer goes no further out than the ring and this
#define RM_DEADZONE			24.0f	// nor picks anything nearer in
#define RM_ITEM_W			100.0f
#define RM_ITEM_H			22.0f
#define RM_TEXT_SCALE		0.8f

// the settings
#define RS_ROW_H			13.0f
#define RS_CTRL_H			15.0f
#define RS_LIST_Y			66.0f
#define RS_LIST_H			260.0f
#define RS_MENU_X			16.0f
#define RS_MENU_W			150.0f
#define RS_ITEM_X			176.0f
#define RS_ITEM_W			236.0f
#define RS_EDIT_Y			334.0f
#define RS_PREVIEW_X		522.0f
#define RS_PREVIEW_Y		196.0f
#define RS_PREVIEW_SCALE	0.62f
#define RS_MESSAGE_MS		5000
#define RS_CONFIRM_MS		3000
#define RS_DOUBLECLICK_MS	400

typedef struct {
	char		label[RM_LABEL_LEN];
	char		cmd[RM_CMD_LEN];
} rmItem_t;

typedef struct {
	char		name[RM_NAME_LEN];
	rmItem_t	items[RM_MAX_ITEMS];
	int			numItems;
} rmMenu_t;

static rmMenu_t		rmMenus[RM_MAX_MENUS];
static int			rmNumMenus;
static qboolean		rmLoaded;

// the menu on screen
static struct {
	int			menu;			// -1 if none
	qboolean	held;			// opened by +radial, its key not let go yet
	int			openTime;
	float		px, py;			// the pointer, from the middle
	int			hover;			// the item pointed at, -1 if none
	int			font;
} rm = { -1 };

typedef enum {
	RS_F_NONE,
	RS_F_NAME,
	RS_F_LABEL,
	RS_F_CMD
} rsField_t;

typedef struct {
	int			sel, scroll;
	int			lastClickRow, lastClickTime;
} rsList_t;

// the settings
static struct {
	qboolean	open;
	int			font;
	float		cursorX, cursorY;
	float		clickX, clickY;
	qboolean	click;
	int			wheel;
	rsList_t	menuList, itemList;
	rsField_t	focus;
	char		nameBefore[RM_NAME_LEN];	// the menu's name when its box was clicked, for the binds
	qboolean	binding;		// the next key pressed gets bound
	int			confirmTime;
	char		message[MAX_STRING_CHARS];
	int			messageTime;
	qboolean	dirty;
	int			dirtyTime;
} rs;

static const vec4_t rmWhite		= { 1.0f, 1.0f, 1.0f, 1.0f };
static const vec4_t rmDim		= { 0.7f, 0.7f, 0.7f, 1.0f };
static const vec4_t rmItemBox	= { 0.0f, 0.0f, 0.0f, 0.7f };
static const vec4_t rmItemHover	= { 0.2f, 0.45f, 0.8f, 0.9f };
static const vec4_t rmCenter	= { 0.0f, 0.0f, 0.0f, 0.55f };
static const vec4_t rmRing		= { 1.0f, 1.0f, 1.0f, 0.35f };
static const vec4_t rmPointer	= { 1.0f, 0.8f, 0.3f, 1.0f };
static const vec4_t rmNumber	= { 1.0f, 0.8f, 0.3f, 1.0f };
static const vec4_t rsPanel		= { 0.0f, 0.0f, 0.0f, 0.65f };
static const vec4_t rsPanelLight = { 0.15f, 0.15f, 0.15f, 0.8f };
static const vec4_t rsPanelFocus = { 0.1f, 0.2f, 0.3f, 0.9f };
static const vec4_t rsHighlight	= { 0.2f, 0.45f, 0.8f, 0.8f };
static const vec4_t rsHover		= { 1.0f, 1.0f, 1.0f, 0.12f };
static const vec4_t rsHoverBox	= { 0.25f, 0.25f, 0.25f, 0.9f };
static const vec4_t rsDanger	= { 0.6f, 0.15f, 0.15f, 0.9f };
static const vec4_t rsBorder	= { 0.5f, 0.5f, 0.5f, 0.8f };

/*
===============================================================================

THE MENUS AND THEIR FILE

===============================================================================
*/

static rmMenu_t *RM_AddMenu( const char *name ) {
	rmMenu_t *m;

	if ( rmNumMenus == RM_MAX_MENUS )
		return NULL;
	m = &rmMenus[rmNumMenus++];
	memset( m, 0, sizeof( *m ) );
	Q_strncpyz( m->name, name, sizeof( m->name ) );
	return m;
}

static rmItem_t *RM_AddItem( rmMenu_t *m, const char *label, const char *cmd ) {
	rmItem_t *item;

	if ( !m || m->numItems == RM_MAX_ITEMS )
		return NULL;
	item = &m->items[m->numItems++];
	Q_strncpyz( item->label, label, sizeof( item->label ) );
	Q_strncpyz( item->cmd, cmd, sizeof( item->cmd ) );
	return item;
}

static void RM_Defaults( void ) {
	rmMenu_t *m;

	rmNumMenus = 0;
	m = RM_AddMenu( "tools" );
	RM_AddItem( m, "Models", "modelmanager" );
	RM_AddItem( m, "NPCs", "npcmanager" );
	RM_AddItem( m, "Effects", "effectmanager" );
	RM_AddItem( m, "Shaders", "shadermanager" );
	RM_AddItem( m, "Map editor", "mapedit" );
	RM_AddItem( m, "Storyteller", "storyteller" );
	RM_AddItem( m, "Sounds", "soundmanager" );
	RM_AddItem( m, "Music", "musicmanager" );
	RM_AddItem( m, "Camera", "cincam edit" );
	RM_AddItem( m, "Radial menus", RM_COMMAND );
	m = RM_AddMenu( "styles" );
	RM_AddItem( m, "Fast", "saberstyle fast" );
	RM_AddItem( m, "Medium", "saberstyle medium" );
	RM_AddItem( m, "Strong", "saberstyle strong" );
	RM_AddItem( m, "Desann", "saberstyle desann" );
	RM_AddItem( m, "Tavion", "saberstyle tavion" );
	m = RM_AddMenu( "gestures" );
	RM_AddItem( m, "Taunt", "taunt" );
	RM_AddItem( m, "Bow", "bow" );
	RM_AddItem( m, "Meditate", "meditate" );
	RM_AddItem( m, "Flourish", "flourish" );
	RM_AddItem( m, "Gloat", "gloat" );
}

static void RM_Save( void ) {
	fileHandle_t f = FS_FOpenFileWrite( RM_FILE );

	rs.dirty = qfalse;
	if ( !f ) {
		Com_Printf( S_COLOR_RED "Radial menus: couldn't write " RM_FILE "\n" );
		return;
	}
	FS_Printf( f, "// Radial menus: menu \"<name>\", then its items, item \"<label>\" \"<command>\"\n" );
	for ( int i = 0; i < rmNumMenus; i++ ) {
		FS_Printf( f, "menu \"%s\"\n", rmMenus[i].name );
		for ( int j = 0; j < rmMenus[i].numItems; j++ )
			FS_Printf( f, "item \"%s\" \"%s\"\n", rmMenus[i].items[j].label, rmMenus[i].items[j].cmd );
	}
	FS_FCloseFile( f );
}

// splits a line into words, "quoted" ones whole; the line is cut up in place
static int RM_SplitLine( char *line, char **argv, int max ) {
	int argc = 0;

	while ( *line && argc < max ) {
		while ( *line == ' ' || *line == '\t' )
			line++;
		if ( !*line )
			break;
		if ( *line == '"' ) {
			argv[argc++] = ++line;
			while ( *line && *line != '"' )
				line++;
		} else {
			argv[argc++] = line;
			while ( *line && *line != ' ' && *line != '\t' )
				line++;
		}
		if ( *line )
			*line++ = '\0';
	}
	return argc;
}

static void RM_Load( void ) {
	char *buf, *line, *next, *argv[3];
	rmMenu_t *m = NULL;
	int len;

	if ( rmLoaded )
		return;
	rmLoaded = qtrue;
	rmNumMenus = 0;
	len = FS_ReadFile( RM_FILE, (void **)&buf );
	if ( len < 0 || !buf ) {
		RM_Defaults();
		RM_Save();
		return;
	}
	for ( line = buf; line && *line; line = next ) {
		next = strchr( line, '\n' );
		if ( next )
			*next++ = '\0';
		len = strlen( line );
		if ( len && line[len - 1] == '\r' )
			line[len - 1] = '\0';
		if ( line[0] == '/' && line[1] == '/' )
			continue;
		const int argc = RM_SplitLine( line, argv, ARRAY_LEN( argv ) );
		if ( argc >= 2 && !Q_stricmp( argv[0], "menu" ) )
			m = RM_AddMenu( argv[1] );
		else if ( argc >= 3 && !Q_stricmp( argv[0], "item" ) )
			RM_AddItem( m, argv[1], argv[2] );
	}
	FS_FreeFile( buf );
}

// by name, any case; an empty name is the first one. -1 if none
static int RM_FindMenu( const char *name ) {
	if ( !name[0] )
		return rmNumMenus ? 0 : -1;
	for ( int i = 0; i < rmNumMenus; i++ ) {
		if ( !Q_stricmp( rmMenus[i].name, name ) )
			return i;
	}
	return -1;
}

/*
===============================================================================

THE MENU ON SCREEN

===============================================================================
*/

static void RM_Close( void ) {
	rm.menu = -1;
	rm.held = qfalse;
	rm.hover = -1;
}

static void RM_Open( int menu, qboolean held ) {
	rm.font = re->RegisterFont( "arialnb" );
	if ( !rm.font )
		rm.font = cls.menuFont;
	rm.menu = menu;
	rm.held = held;
	rm.openTime = cls.realtime;
	rm.px = rm.py = 0.0f;
	rm.hover = -1;
}

// runs an item and closes the menu; one that isn't there only closes it
static void RM_Run( int item ) {
	const rmMenu_t *m = rm.menu >= 0 ? &rmMenus[rm.menu] : NULL;

	RM_Close();
	if ( !m || item < 0 || item >= m->numItems || !m->items[item].cmd[0] )
		return;
	Cbuf_AddText( va( "%s\n", m->items[item].cmd ) );
}

// the ring's radius: wide enough that its items don't run into each other
static float RM_Radius( const rmMenu_t *m ) {
	return RM_RADIUS + Q_max( 0, m->numItems - 6 ) * RM_RADIUS_STEP;
}

// the item in the direction of x, y from the middle: the first at the top, the rest clockwise
static int RM_ItemAt( const rmMenu_t *m, float x, float y ) {
	float angle, step;

	if ( !m->numItems || x * x + y * y < RM_DEADZONE * RM_DEADZONE )
		return -1;
	step = 360.0f / m->numItems;
	angle = RAD2DEG( atan2f( x, -y ) );
	if ( angle < 0.0f )
		angle += 360.0f;
	return (int)( ( angle + step * 0.5f ) / step ) % m->numItems;
}

static qboolean RM_CanOpen( void ) {
	return (qboolean)( cls.state == CA_ACTIVE && cls.cgameStarted && !Key_GetCatcher() );
}

// the menu named after the command; a key's +radial has the key and the time after it
static const char *RM_MenuArg( qboolean button ) {
	static char name[MAX_STRING_CHARS];
	int argc = Cmd_Argc();

	if ( button && argc >= 3 && Q_isanumber( Cmd_Argv( argc - 1 ) ) && Q_isanumber( Cmd_Argv( argc - 2 ) ) )
		argc -= 2;
	name[0] = '\0';
	for ( int i = 1; i < argc; i++ )
		Q_strcat( name, sizeof( name ), va( "%s%s", i > 1 ? " " : "", Cmd_Argv( i ) ) );
	return name;
}

// the menu to open, -1 with why not
static int RM_MenuToOpen( const char *name ) {
	int menu;

	RM_Load();
	menu = RM_FindMenu( name );
	if ( menu < 0 ) {
		Com_Printf( name[0] ? "No radial menu \"%s\": " RM_COMMAND " makes them\n" : "No radial menus yet: " RM_COMMAND " makes them\n", name );
		return -1;
	}
	if ( !rmMenus[menu].numItems ) {
		Com_Printf( "Radial menu \"%s\" is empty: " RM_COMMAND " fills it\n", rmMenus[menu].name );
		return -1;
	}
	return menu;
}

static void RM_Down_f( void ) {
	const char *name = RM_MenuArg( qtrue );
	int menu;

	if ( rm.menu >= 0 ) {
		// the key repeating while held
		if ( rm.held )
			return;
		// open and staying: the key again runs what's pointed at, or closes it; another menu's key opens that one
		menu = name[0] ? RM_FindMenu( name ) : rm.menu;
		if ( menu < 0 || menu == rm.menu || !rmMenus[menu].numItems ) {
			RM_Run( rm.hover );
			return;
		}
		RM_Open( menu, qtrue );
		return;
	}
	if ( !RM_CanOpen() )
		return;
	menu = RM_MenuToOpen( name );
	if ( menu >= 0 )
		RM_Open( menu, qtrue );
}

static void RM_Up_f( void ) {
	if ( rm.menu < 0 || !rm.held )
		return;
	rm.held = qfalse;
	if ( rm.hover >= 0 )
		RM_Run( rm.hover );
	else if ( cls.realtime - rm.openTime >= RM_TAP_MS )
		RM_Close();
}

static void RM_Radial_f( void ) {
	int menu;

	if ( rm.menu >= 0 && !Cmd_Argv( 1 )[0] ) {
		RM_Close();
		return;
	}
	if ( !RM_CanOpen() )
		return;
	menu = RM_MenuToOpen( RM_MenuArg( qfalse ) );
	if ( menu >= 0 )
		RM_Open( menu, qfalse );
}

qboolean CL_RadialMenu_Active( void ) {
	return (qboolean)( rm.menu >= 0 );
}

// the mouse points while it's open: qtrue keeps it from turning the view
qboolean CL_RadialMenu_MouseEvent( int dx, int dy ) {
	float len, reach;

	if ( rm.menu < 0 )
		return qfalse;
	rm.px += dx;
	rm.py += dy;
	len = sqrtf( rm.px * rm.px + rm.py * rm.py );
	reach = RM_Radius( &rmMenus[rm.menu] ) + RM_REACH;
	if ( len > reach ) {
		rm.px *= reach / len;
		rm.py *= reach / len;
	}
	rm.hover = RM_ItemAt( &rmMenus[rm.menu], rm.px, rm.py );
	return qtrue;
}

// a key going down while it's open: qtrue if it was the menu's, the number keys and the mouse buttons
qboolean CL_RadialMenu_KeyDown( int key ) {
	if ( rm.menu < 0 )
		return qfalse;
	if ( key >= A_1 && key <= A_9 ) {
		if ( key - A_1 < rmMenus[rm.menu].numItems )
			RM_Run( key - A_1 );
		return qtrue;
	}
	switch ( key ) {
	case A_0:
		if ( rmMenus[rm.menu].numItems == RM_MAX_ITEMS )
			RM_Run( RM_MAX_ITEMS - 1 );
		return qtrue;
	case A_MOUSE1:
		RM_Run( rm.hover );
		return qtrue;
	case A_MOUSE2:
		RM_Close();
		return qtrue;
	case A_MWHEELUP:
	case A_MWHEELDOWN:
		return qtrue;
	default:
		return qfalse;
	}
}

qboolean CL_RadialMenu_Escape( void ) {
	if ( rm.menu < 0 )
		return qfalse;
	RM_Close();
	return qtrue;
}

static void RM_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void RM_Box( float x, float y, float w, float h, const float *fill, const float *border ) {
	if ( fill )
		RM_Fill( x, y, w, h, fill );
	RM_Fill( x, y, w, 1, border );
	RM_Fill( x, y + h - 1, w, 1, border );
	RM_Fill( x, y, 1, h, border );
	RM_Fill( x + w - 1, y, 1, h, border );
}

// a line of text centred on x, cut to fit w
static void RM_CenterText( int font, float x, float y, float w, float scale, const char *text, const float *color ) {
	char buf[RM_CMD_LEN];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 1 && re->Font_StrLenPixels( buf, font, scale ) > w )
		buf[--len] = '\0';
	re->Font_DrawString( (int)( x - re->Font_StrLenPixels( buf, font, scale ) * 0.5f ), (int)y, buf, color, font | STYLE_DROPSHADOW, -1, scale );
}

// the ring of items round cx, cy; pointer draws the mouse's, hover is lit
static void RM_DrawMenu( const rmMenu_t *m, int font, float cx, float cy, float scale, int hover, qboolean pointer ) {
	const float radius = RM_Radius( m ) * scale, w = RM_ITEM_W * scale, h = RM_ITEM_H * scale;
	const float textScale = RM_TEXT_SCALE * scale, textH = (float)re->Font_HeightPixels( font, textScale );

	// a dotted ring inside the items
	for ( int i = 0; i < 48; i++ ) {
		const float a = DEG2RAD( i * 7.5f );

		RM_Fill( cx + sinf( a ) * radius * 0.55f - 1, cy - cosf( a ) * radius * 0.55f - 1, 2, 2, rmRing );
	}
	RM_Box( cx - 50 * scale, cy - 16 * scale, 100 * scale, 32 * scale, rmCenter, rmRing );
	RM_CenterText( font, cx, cy - 13 * scale, 96 * scale, textScale, m->name, rmDim );
	if ( hover >= 0 && hover < m->numItems )
		RM_CenterText( font, cx, cy + 1 * scale, 96 * scale, textScale, m->items[hover].label, rmWhite );

	for ( int i = 0; i < m->numItems; i++ ) {
		const float a = DEG2RAD( i * 360.0f / m->numItems );
		const float x = cx + sinf( a ) * radius - w * 0.5f, y = cy - cosf( a ) * radius - h * 0.5f;

		RM_Box( x, y, w, h, i == hover ? rmItemHover : rmItemBox, i == hover ? rmWhite : rmRing );
		RM_CenterText( font, x + w * 0.5f + 5 * scale, y + ( h - textH ) * 0.5f, w - 16 * scale, textScale, m->items[i].label, rmWhite );
		re->Font_DrawString( (int)( x + 3 * scale ), (int)( y + ( h - textH ) * 0.5f ), va( "%i", ( i + 1 ) % 10 ), rmNumber,
			font | STYLE_DROPSHADOW, -1, textScale * 0.8f );
	}

	if ( pointer ) {
		// dots out to it, and the pointer
		for ( int i = 1; i <= 4; i++ )
			RM_Fill( cx + rm.px * i / 5.0f - 1, cy + rm.py * i / 5.0f - 1, 2, 2, rmPointer );
		RM_Fill( cx + rm.px - 3, cy + rm.py - 3, 6, 6, rmPointer );
	}
}

void CL_RadialMenu_Frame( void ) {
	// a menu, the console or a tool over it closes it
	if ( rm.menu >= 0 && ( cls.state != CA_ACTIVE || Key_GetCatcher() ) )
		RM_Close();
	if ( rs.open && ( cls.state != CA_ACTIVE || ( Key_GetCatcher() & KEYCATCH_ENGINETOOLS & ~KEYCATCH_RADIALMENU ) ) )
		CL_RadialMenu_CloseSettings();
	if ( rs.dirty && cls.realtime - rs.dirtyTime >= RM_SAVE_MS && rs.focus == RS_F_NONE )
		RM_Save();
}

void CL_RadialMenu_Draw( void ) {
	if ( rm.menu >= 0 )
		RM_DrawMenu( &rmMenus[rm.menu], rm.font, SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.5f, 1.0f, rm.hover, qtrue );
}

/*
===============================================================================

THE SETTINGS

===============================================================================
*/

static void RS_Message( const char *text ) {
	Q_strncpyz( rs.message, text, sizeof( rs.message ) );
	rs.messageTime = cls.realtime;
}

static void RS_Changed( void ) {
	rs.dirty = qtrue;
	rs.dirtyTime = cls.realtime;
}

static rmMenu_t *RS_Menu( void ) {
	return rs.menuList.sel >= 0 && rs.menuList.sel < rmNumMenus ? &rmMenus[rs.menuList.sel] : NULL;
}

static rmItem_t *RS_Item( void ) {
	rmMenu_t *m = RS_Menu();

	return m && rs.itemList.sel >= 0 && rs.itemList.sel < m->numItems ? &m->items[rs.itemList.sel] : NULL;
}

static char *RS_FieldText( rsField_t f, int *size ) {
	rmMenu_t *m = RS_Menu();
	rmItem_t *item = RS_Item();

	switch ( f ) {
	case RS_F_NAME:		*size = RM_NAME_LEN; return m ? m->name : NULL;
	case RS_F_LABEL:	*size = RM_LABEL_LEN; return item ? item->label : NULL;
	case RS_F_CMD:		*size = RM_CMD_LEN; return item ? item->cmd : NULL;
	default:			*size = 0; return NULL;
	}
}

// the binds that open a menu: "+radial <name>" or "radial <name>"
static qboolean RS_IsMenuBind( const char *binding, const char *name ) {
	if ( !VALIDSTRING( binding ) )
		return qfalse;
	if ( *binding == '+' )
		binding++;
	if ( Q_stricmpn( binding, "radial", 6 ) || ( binding[6] && binding[6] != ' ' ) )
		return qfalse;
	binding += 6;
	while ( *binding == ' ' )
		binding++;
	return (qboolean)( !Q_stricmp( binding, name ) || ( !binding[0] && rmNumMenus && !Q_stricmp( name, rmMenus[0].name ) ) );
}

// "F, MOUSE3" for the keys that open it
static const char *RS_MenuKeys( const char *name ) {
	static char keys[MAX_STRING_CHARS];

	keys[0] = '\0';
	for ( int i = 0; i < MAX_KEYS; i++ ) {
		if ( RS_IsMenuBind( Key_GetBinding( i ), name ) )
			Q_strcat( keys, sizeof( keys ), va( "%s%s", keys[0] ? ", " : "", Key_KeynumToString( i ) ) );
	}
	return keys;
}

// a menu's name done with: no name or one taken goes back, and its binds follow a new one
static void RS_CommitName( void ) {
	rmMenu_t *m = RS_Menu();

	if ( !m || !strcmp( m->name, rs.nameBefore ) )
		return;
	for ( int i = 0; i < rmNumMenus; i++ ) {
		if ( &rmMenus[i] != m && !Q_stricmp( rmMenus[i].name, m->name ) ) {
			RS_Message( va( S_COLOR_YELLOW "There's a menu called %s already", m->name ) );
			Q_strncpyz( m->name, rs.nameBefore, sizeof( m->name ) );
			return;
		}
	}
	if ( !m->name[0] ) {
		Q_strncpyz( m->name, rs.nameBefore, sizeof( m->name ) );
		return;
	}
	for ( int i = 0; i < MAX_KEYS; i++ ) {
		const char *binding = Key_GetBinding( i );

		if ( RS_IsMenuBind( binding, rs.nameBefore ) && Q_stricmp( binding, "+radial" ) && Q_stricmp( binding, "radial" ) )
			Key_SetBinding( i, va( "%sradial %s", binding[0] == '+' ? "+" : "", m->name ) );
	}
	RS_Changed();
}

static void RS_SetFocus( rsField_t f ) {
	if ( rs.focus == RS_F_NAME && f != RS_F_NAME )
		RS_CommitName();
	if ( f == RS_F_NAME && rs.focus != RS_F_NAME && RS_Menu() )
		Q_strncpyz( rs.nameBefore, RS_Menu()->name, sizeof( rs.nameBefore ) );
	rs.focus = f;
}

static void RS_SelectMenu( int menu ) {
	RS_SetFocus( RS_F_NONE );
	rs.binding = qfalse;
	rs.menuList.sel = menu;
	rs.itemList.sel = RS_Menu() && RS_Menu()->numItems ? 0 : -1;
	rs.itemList.scroll = 0;
}

static void RS_NewMenu( void ) {
	char name[RM_NAME_LEN];
	int n = rmNumMenus + 1;

	if ( rmNumMenus == RM_MAX_MENUS ) {
		RS_Message( va( S_COLOR_YELLOW "%i menus at most", RM_MAX_MENUS ) );
		return;
	}
	do {
		Com_sprintf( name, sizeof( name ), "menu%i", n++ );
	} while ( RM_FindMenu( name ) >= 0 );
	RM_AddMenu( name );
	RS_SelectMenu( rmNumMenus - 1 );
	RS_SetFocus( RS_F_NAME );
	RS_Changed();
}

static void RS_DeleteMenu( void ) {
	const int i = rs.menuList.sel;

	if ( !RS_Menu() )
		return;
	RS_SetFocus( RS_F_NONE );
	RS_Message( va( "Deleted %s", rmMenus[i].name ) );
	memmove( &rmMenus[i], &rmMenus[i + 1], ( rmNumMenus - i - 1 ) * sizeof( rmMenus[0] ) );
	rmNumMenus--;
	RS_SelectMenu( Q_min( i, rmNumMenus - 1 ) );
	RS_Changed();
}

static void RS_AddItem( void ) {
	rmMenu_t *m = RS_Menu();

	if ( !m )
		return;
	if ( !RM_AddItem( m, "New item", "" ) ) {
		RS_Message( va( S_COLOR_YELLOW "%i items at most, one for each number key", RM_MAX_ITEMS ) );
		return;
	}
	rs.itemList.sel = m->numItems - 1;
	RS_SetFocus( RS_F_LABEL );
	RS_Changed();
}

static void RS_RemoveItem( void ) {
	rmMenu_t *m = RS_Menu();
	const int i = rs.itemList.sel;

	if ( !RS_Item() )
		return;
	RS_SetFocus( RS_F_NONE );
	memmove( &m->items[i], &m->items[i + 1], ( m->numItems - i - 1 ) * sizeof( m->items[0] ) );
	m->numItems--;
	rs.itemList.sel = Q_min( i, m->numItems - 1 );
	RS_Changed();
}

static void RS_MoveItem( int delta ) {
	rmMenu_t *m = RS_Menu();
	const int i = rs.itemList.sel, j = i + delta;
	rmItem_t swap;

	if ( !RS_Item() || j < 0 || j >= m->numItems )
		return;
	swap = m->items[i];
	m->items[i] = m->items[j];
	m->items[j] = swap;
	rs.itemList.sel = j;
	RS_Changed();
}

static void RS_Unbind( void ) {
	rmMenu_t *m = RS_Menu();
	int n = 0;

	if ( !m )
		return;
	for ( int i = 0; i < MAX_KEYS; i++ ) {
		if ( RS_IsMenuBind( Key_GetBinding( i ), m->name ) ) {
			Key_SetBinding( i, "" );
			n++;
		}
	}
	RS_Message( n ? va( "Unbound %i key%s", n, n == 1 ? "" : "s" ) : "No keys open it" );
}

void CL_RadialMenu_CloseSettings( void ) {
	if ( !rs.open )
		return;
	RS_SetFocus( RS_F_NONE );
	rs.open = qfalse;
	rs.binding = qfalse;
	if ( rs.dirty )
		RM_Save();
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_RADIALMENU );
}

static void RS_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Radial menus: join a server first\n" );
		return;
	}
	CL_ModelManager_Close();
	CL_NpcManager_Close();
	CL_ShaderManager_Close();
	CL_EffectManager_Close();
	CL_MapEditor_Close();
	CL_Storyteller_Close();
	CL_RpCinematic_EditorClose();
	CL_SoundManager_Close();
	RM_Close();
	RM_Load();

	rs.font = re->RegisterFont( "arialnb" );
	if ( !rs.font )
		rs.font = cls.menuFont;
	rs.open = qtrue;
	rs.focus = RS_F_NONE;
	rs.binding = qfalse;
	rs.cursorX = SCREEN_WIDTH * 0.5f;
	rs.cursorY = SCREEN_HEIGHT * 0.5f;
	rs.click = qfalse;
	rs.wheel = 0;
	if ( !RS_Menu() )
		RS_SelectMenu( rmNumMenus ? 0 : -1 );
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_RADIALMENU );
}

static void RS_Settings_f( void ) {
	if ( rs.open )
		CL_RadialMenu_CloseSettings();
	else
		RS_Open();
}

static void RS_ListMove( int delta ) {
	rmMenu_t *m = RS_Menu();

	if ( !m || !m->numItems )
		return;
	rs.itemList.sel = Com_Clampi( 0, m->numItems - 1, ( rs.itemList.sel < 0 ? 0 : rs.itemList.sel ) + delta );
}

void CL_RadialMenu_KeyEvent( int key, qboolean down ) {
	const char *binding;
	char *text;
	int size;

	if ( !rs.open || !down )
		return;

	// waiting on a key to bind: the mouse's two buttons still click
	if ( rs.binding && key != A_MOUSE1 && key != A_MOUSE2 && RS_Menu() ) {
		rs.binding = qfalse;
		Key_SetBinding( key, va( "+radial %s", RS_Menu()->name ) );
		RS_Message( va( S_COLOR_GREEN "%s opens %s: hold it, point, let go", Key_KeynumToString( key ), RS_Menu()->name ) );
		return;
	}

	// binds don't run while this holds the keys, so honour the one that toggles it here
	binding = Key_GetBinding( key );
	if ( !( rs.focus != RS_F_NONE && key >= A_SPACE && key <= A_TILDE ) && VALIDSTRING( binding ) && !Q_stricmp( binding, RM_COMMAND ) ) {
		CL_RadialMenu_CloseSettings();
		return;
	}

	switch ( key ) {
	case A_MOUSE1:
		rs.click = qtrue;
		rs.clickX = rs.cursorX;
		rs.clickY = rs.cursorY;
		return;
	case A_MWHEELUP:
		rs.wheel -= 3;
		return;
	case A_MWHEELDOWN:
		rs.wheel += 3;
		return;
	case A_ENTER:
	case A_KP_ENTER:
	case A_TAB:
		// on to the next box
		RS_SetFocus( rs.focus == RS_F_LABEL ? RS_F_CMD : RS_F_NONE );
		return;
	case A_BACKSPACE:
		text = RS_FieldText( rs.focus, &size );
		if ( text && text[0] ) {
			text[strlen( text ) - 1] = '\0';
			RS_Changed();
		}
		return;
	case A_CURSOR_UP:
		if ( rs.focus == RS_F_NONE )
			RS_ListMove( -1 );
		return;
	case A_CURSOR_DOWN:
		if ( rs.focus == RS_F_NONE )
			RS_ListMove( 1 );
		return;
	default:
		return;
	}
}

void CL_RadialMenu_CharEvent( int ch ) {
	char *text;
	int size, len;

	text = RS_FieldText( rs.focus, &size );
	if ( !rs.open || !text || ch < ' ' || ch >= 127 )
		return;
	// the file keeps them in quotes; a name is in binds too
	if ( ch == '"' || ( rs.focus == RS_F_NAME && ch == ';' ) )
		return;
	len = strlen( text );
	if ( len < size - 1 ) {
		text[len] = (char)ch;
		text[len + 1] = '\0';
		RS_Changed();
	}
}

void CL_RadialMenu_SettingsMouseEvent( int dx, int dy ) {
	rs.cursorX = Com_Clamp( 0, SCREEN_WIDTH, rs.cursorX + dx );
	rs.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, rs.cursorY + dy );
}

// escape backs out one level: the key waited on, a text box, then the settings
void CL_RadialMenu_SettingsEscape( void ) {
	if ( rs.binding )
		rs.binding = qfalse;
	else if ( rs.focus != RS_F_NONE )
		RS_SetFocus( RS_F_NONE );
	else
		CL_RadialMenu_CloseSettings();
}

static qboolean RS_InRect( float x, float y, float w, float h ) {
	return (qboolean)( rs.cursorX >= x && rs.cursorX < x + w && rs.cursorY >= y && rs.cursorY < y + h );
}

static void RS_Text( float x, float y, const char *text, const float *color ) {
	re->Font_DrawString( (int)x, (int)y, text, color, rs.font | STYLE_DROPSHADOW, -1, RM_TEXT_SCALE );
}

static float RS_TextWidth( const char *text ) {
	return (float)re->Font_StrLenPixels( text, rs.font, RM_TEXT_SCALE );
}

static void RS_TextClipped( float x, float y, float w, const char *text, const float *color ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 3 && RS_TextWidth( buf ) > w ) {
		len--;
		buf[len - 3] = '.';
		buf[len - 2] = '.';
		buf[len - 1] = '.';
		buf[len] = '\0';
	}
	RS_Text( x, y, buf, color );
}

// eats this frame's click if it landed in the rect
static qboolean RS_Clicked( float x, float y, float w, float h ) {
	if ( !rs.click || rs.clickX < x || rs.clickX >= x + w || rs.clickY < y || rs.clickY >= y + h )
		return qfalse;
	rs.click = qfalse;
	return qtrue;
}

static qboolean RS_Button( float x, float y, float w, const char *label, qboolean selected, qboolean enabled ) {
	const float *fill = selected ? rsHighlight : ( enabled && RS_InRect( x, y, w, RS_CTRL_H ) ) ? rsHoverBox : rsPanelLight;

	RM_Box( x, y, w, RS_CTRL_H, fill, rsBorder );
	RS_Text( x + ( w - RS_TextWidth( label ) ) * 0.5f, y + 2, label, enabled ? rmWhite : rmDim );
	return (qboolean)( RS_Clicked( x, y, w, RS_CTRL_H ) && enabled );
}

// a button that has to be clicked twice
static qboolean RS_DangerButton( float x, float y, float w, const char *label, qboolean enabled ) {
	const qboolean armed = (qboolean)( rs.confirmTime && cls.realtime - rs.confirmTime < RS_CONFIRM_MS );
	const char *text = armed ? "Click again" : label;

	RM_Box( x, y, w, RS_CTRL_H, armed ? rsDanger : ( enabled && RS_InRect( x, y, w, RS_CTRL_H ) ) ? rsHoverBox : rsPanelLight, rsBorder );
	RS_Text( x + ( w - RS_TextWidth( text ) ) * 0.5f, y + 2, text, enabled ? rmWhite : rmDim );
	if ( !RS_Clicked( x, y, w, RS_CTRL_H ) || !enabled )
		return qfalse;
	if ( armed ) {
		rs.confirmTime = 0;
		return qtrue;
	}
	rs.confirmTime = cls.realtime;
	return qfalse;
}

static void RS_Field( rsField_t f, float x, float y, float w, const char *placeholder ) {
	const qboolean focused = (qboolean)( rs.focus == f );
	char buf[RM_CMD_LEN + 2];
	int size;
	const char *text = RS_FieldText( f, &size );

	RM_Box( x, y, w, RS_CTRL_H, focused ? rsPanelFocus : rsPanelLight, rsBorder );
	if ( !text ) {
		RS_TextClipped( x + 4, y + 2, w - 8, placeholder, rmDim );
		return;
	}
	if ( !text[0] && !focused ) {
		RS_TextClipped( x + 4, y + 2, w - 8, placeholder, rmDim );
	} else {
		// keep the end in view
		Com_sprintf( buf, sizeof( buf ), "%s%s", text, focused && ( ( cls.realtime >> 8 ) & 1 ) ? "_" : "" );
		const char *shown = buf;
		while ( shown[1] && RS_TextWidth( shown ) > w - 8 )
			shown++;
		RS_Text( x + 4, y + 2, shown, rmWhite );
	}
	if ( RS_Clicked( x, y, w, RS_CTRL_H ) )
		RS_SetFocus( f );
}

static void RS_Label( float x, float y, const char *label ) {
	RS_Text( x, y + 2, label, rmDim );
}

typedef void ( *rsRowFunc_t )( int row, float x, float y, float w );

// *clicked gets the row clicked, -1 if none
static void RS_List( rsList_t *list, int count, float x, float y, float w, float h, rsRowFunc_t drawRow, int *clicked ) {
	const int visible = (int)( h / RS_ROW_H );
	int row;

	*clicked = -1;
	if ( rs.wheel && RS_InRect( x, y, w, h ) ) {
		list->scroll += rs.wheel;
		rs.wheel = 0;
	}
	if ( list->sel >= 0 && list->sel < list->scroll )
		list->scroll = list->sel;
	if ( list->sel >= list->scroll + visible )
		list->scroll = list->sel - visible + 1;
	if ( list->scroll > count - visible )
		list->scroll = count - visible;
	if ( list->scroll < 0 )
		list->scroll = 0;

	if ( RS_Clicked( x, y, w, h ) ) {
		row = list->scroll + (int)( ( rs.clickY - y ) / RS_ROW_H );
		if ( row < count )
			*clicked = row;
	}

	RM_Box( x, y, w, h, rsPanelLight, rsBorder );
	for ( int i = 0; i < visible; i++ ) {
		const float ry = y + i * RS_ROW_H;

		row = list->scroll + i;
		if ( row >= count )
			break;
		if ( row == list->sel )
			RM_Fill( x + 1, ry, w - 2, RS_ROW_H, rsHighlight );
		else if ( RS_InRect( x, ry, w, RS_ROW_H ) )
			RM_Fill( x + 1, ry, w - 2, RS_ROW_H, rsHover );
		drawRow( row, x + 4, ry + 1, w - 8 );
	}
}

static void RS_DrawMenuRow( int row, float x, float y, float w ) {
	RS_TextClipped( x, y, w - 24, rmMenus[row].name, rmWhite );
	RS_Text( x + w - 18, y, va( S_COLOR_GREY "%i", rmMenus[row].numItems ), rmWhite );
}

static void RS_DrawItemRow( int row, float x, float y, float w ) {
	const rmItem_t *item = &RS_Menu()->items[row];

	RS_Text( x, y, va( S_COLOR_YELLOW "%i", ( row + 1 ) % 10 ), rmWhite );
	RS_TextClipped( x + 12, y, 90, item->label, rmWhite );
	RS_TextClipped( x + 108, y, w - 108, item->cmd[0] ? item->cmd : "(no command)", rmDim );
}

static void RS_DrawMenus( void ) {
	rmMenu_t *m = RS_Menu();
	const char *keys;
	float y = RS_EDIT_Y;
	int clicked;

	RS_Label( RS_MENU_X, 48, "Menus" );
	RS_List( &rs.menuList, rmNumMenus, RS_MENU_X, RS_LIST_Y, RS_MENU_W, RS_LIST_H, RS_DrawMenuRow, &clicked );
	if ( clicked >= 0 && clicked != rs.menuList.sel )
		RS_SelectMenu( clicked );

	if ( RS_Button( RS_MENU_X, y, 72, "New", qfalse, (qboolean)( rmNumMenus < RM_MAX_MENUS ) ) )
		RS_NewMenu();
	if ( RS_DangerButton( RS_MENU_X + 78, y, 72, "Delete", (qboolean)( m != NULL ) ) )
		RS_DeleteMenu();
	y += 20;
	RS_Label( RS_MENU_X, y, "Name" );
	RS_Field( RS_F_NAME, RS_MENU_X + 34, y, RS_MENU_W - 34, "the menu's name" );
	y += 20;
	if ( RS_Button( RS_MENU_X, y, 98, rs.binding ? "Press a key..." : "Bind a key", rs.binding, (qboolean)( m != NULL ) ) ) {
		RS_SetFocus( RS_F_NONE );
		rs.binding = qtrue;
	}
	if ( RS_Button( RS_MENU_X + 104, y, 46, "Unbind", qfalse, (qboolean)( m != NULL ) ) )
		RS_Unbind();
	y += 20;
	if ( m ) {
		keys = RS_MenuKeys( m->name );
		RS_TextClipped( RS_MENU_X, y + 2, RS_MENU_W, keys[0] ? va( "Key: " S_COLOR_GREEN "%s", keys ) : S_COLOR_GREY "No key opens it", rmWhite );
	}
}

static void RS_DrawItems( void ) {
	rmMenu_t *m = RS_Menu();
	const qboolean hasItem = (qboolean)( RS_Item() != NULL );
	float y = RS_EDIT_Y;
	int clicked;

	RS_Label( RS_ITEM_X, 48, m ? va( "Items of %s", m->name ) : "Items" );
	if ( m ) {
		RS_List( &rs.itemList, m->numItems, RS_ITEM_X, RS_LIST_Y, RS_ITEM_W, RS_LIST_H, RS_DrawItemRow, &clicked );
		if ( clicked >= 0 && clicked != rs.itemList.sel ) {
			RS_SetFocus( RS_F_NONE );
			rs.itemList.sel = clicked;
		}
	} else {
		RM_Box( RS_ITEM_X, RS_LIST_Y, RS_ITEM_W, RS_LIST_H, rsPanelLight, rsBorder );
	}

	if ( RS_Button( RS_ITEM_X, y, 56, "Add", qfalse, (qboolean)( m && m->numItems < RM_MAX_ITEMS ) ) )
		RS_AddItem();
	if ( RS_DangerButton( RS_ITEM_X + 60, y, 64, "Remove", hasItem ) )
		RS_RemoveItem();
	if ( RS_Button( RS_ITEM_X + 128, y, 50, "Up", qfalse, (qboolean)( hasItem && rs.itemList.sel > 0 ) ) )
		RS_MoveItem( -1 );
	if ( RS_Button( RS_ITEM_X + 182, y, 54, "Down", qfalse, (qboolean)( hasItem && rs.itemList.sel < m->numItems - 1 ) ) )
		RS_MoveItem( 1 );
	y += 20;
	RS_Label( RS_ITEM_X, y, "Label" );
	RS_Field( RS_F_LABEL, RS_ITEM_X + 58, y, RS_ITEM_W - 58, hasItem ? "what it shows" : "pick an item" );
	y += 20;
	RS_Label( RS_ITEM_X, y, "Command" );
	RS_Field( RS_F_CMD, RS_ITEM_X + 58, y, SCREEN_WIDTH - 16 - RS_ITEM_X - 58, hasItem ? "what it runs, as typed in the console" : "pick an item" );
	y += 20;
	RS_Text( RS_ITEM_X + 58, y + 2, S_COLOR_GREY "Like saberstyle tavion, say hello, taunt, or radial <menu> to open another", rmWhite );
}

void CL_RadialMenu_DrawSettings( void ) {
	rmMenu_t *m;

	if ( !rs.open )
		return;
	m = RS_Menu();

	RM_Fill( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, rsPanel );
	RS_Text( RS_MENU_X, 14, "Radial Menus", rmWhite );
	if ( rs.message[0] && cls.realtime - rs.messageTime < RS_MESSAGE_MS )
		RS_TextClipped( RS_MENU_X, 29, SCREEN_WIDTH - RS_MENU_X * 2, rs.message, rmWhite );
	else if ( rs.binding && m )
		RS_TextClipped( RS_MENU_X, 29, SCREEN_WIDTH - RS_MENU_X * 2, va( S_COLOR_YELLOW "Press the key to open %s with (Esc: never mind)", m->name ), rmWhite );

	RS_DrawMenus();
	RS_DrawItems();

	// how it will look, the item picked lit
	RS_Label( RS_PREVIEW_X - 100, 48, "Preview" );
	if ( m ) {
		const float scale = Q_min( RS_PREVIEW_SCALE, 110.0f / ( RM_Radius( m ) + RM_ITEM_W * 0.5f ) );

		RM_DrawMenu( m, rs.font, RS_PREVIEW_X, RS_PREVIEW_Y, scale, rs.itemList.sel, qfalse );
	}

	RS_Text( RS_MENU_X, 440, S_COLOR_GREY "In game: hold the key, point the mouse at one and let go, or press its number. Tap the key to keep it open", rmWhite );
	RS_Text( RS_MENU_X, 452, va( S_COLOR_GREY "Esc back / close   %s", Key_GetKey( RM_COMMAND ) >= 0 ? "Your " RM_COMMAND " bind closes"
		: "Tip: bind a key to " RM_COMMAND ), rmWhite );

	// a click on nothing leaves the text box
	if ( rs.click )
		RS_SetFocus( RS_F_NONE );
	rs.click = qfalse;
	rs.wheel = 0;

	re->DrawStretchPic( rs.cursorX, rs.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

/*
===============================================================================

STARTING AND STOPPING

===============================================================================
*/

void CL_RadialMenu_Init( void ) {
	memset( &rs, 0, sizeof( rs ) );
	rs.menuList.sel = rs.itemList.sel = -1;
	RM_Close();
	rmLoaded = qfalse;
	Cmd_AddCommand( "+radial", RM_Down_f, "Hold to open a radial menu, let go to run what's pointed at" );
	Cmd_AddCommand( "-radial", RM_Up_f, NULL );
	Cmd_AddCommand( "radial", RM_Radial_f, "Open a radial menu, or close the one open" );
	Cmd_AddCommand( RM_COMMAND, RS_Settings_f, "Make radial menus and bind keys to them" );
}

// cgame is going away: map change, disconnect or vid_restart
void CL_RadialMenu_Shutdown( void ) {
	RM_Close();
	CL_RadialMenu_CloseSettings();
}
