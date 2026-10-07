/*
===========================================================================
Copyright (C) 2026, TaystJK contributors

This file is part of the TaystJK source code.

TaystJK is free software; you can redistribute it and/or modify it
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

// cl_modelplacer.cpp -- engine-side map_objects browser and placement tool
//
// Browse every models/map_objects/*.md3 the filesystem can see, preview it,
// fly a free camera around the map with a ghost of the model, then send
// "rpmodel add <model> <x y z> <pitch yaw roll> <scale%> <solid>" to an RPMod server.
//
// This lives in the engine rather than cgame so it works whatever cgame the
// server's mod ships (RPMod loads its own). The cgame keeps drawing the world;
// its main view is re-aimed from the free camera as it is handed to the renderer.

#include "client.h"
#include "qcommon/cm_public.h"

#define MP_ROOT				"models/map_objects"	// where models are listed from
#define MP_SEND_PREFIX		"map_objects"			// what RPMod expects: relative to models/, no extension
#define MP_EXT				".md3"
#define MP_TOGGLE_CMD		"modelplacer"

#define MP_MAX_MODELS		16384
#define MP_MAX_FOLDERS		2048
#define MP_NAME_POOL		(MP_MAX_MODELS * 40)
#define MP_HASH_SIZE		(MP_MAX_MODELS * 2)

#define MP_TRACE_MASK		(CONTENTS_SOLID|CONTENTS_TERRAIN)
#define MP_TRACE_DIST		8192.0f
#define MP_NO_HIT_DIST		256.0f
#define MP_FLY_SPEED		400.0f
#define MP_SCALE_MIN		1
#define MP_SCALE_MAX		1000
#define MP_DOUBLECLICK_MS	400

// virtual 640x480 layout
#define MP_ROW_H			13.0f
#define MP_TEXT_SCALE		0.8f
#define MP_SEARCH_Y			44.0f
#define MP_LIST_Y			66.0f
#define MP_LIST_H			364.0f
#define MP_FOLDER_X			16.0f
#define MP_FOLDER_W			150.0f
#define MP_MODEL_X			172.0f
#define MP_MODEL_W			210.0f
#define MP_PREVIEW_X		388.0f
#define MP_PREVIEW_Y		66.0f
#define MP_PREVIEW_W		236.0f
#define MP_PREVIEW_H		236.0f
#define MP_BUTTON_Y			392.0f
#define MP_BUTTON_H			20.0f
#define MP_VISIBLE_ROWS		((int)(MP_LIST_H / MP_ROW_H))

typedef enum {
	MP_OFF,
	MP_BROWSE,
	MP_PLACE
} mpState_t;

typedef struct mpModel_s {
	const char	*path;		// relative to MP_ROOT, no extension, e.g. "bespin/panels"
	const char	*base;		// file name part of path
	int			folder;
	qboolean	failed;		// the renderer couldn't load it
} mpModel_t;

typedef struct mpFolder_s {
	char		name[MAX_QPATH];	// relative to MP_ROOT, "" for the root itself
	int			first, count;		// range in the sorted model list
} mpFolder_t;

typedef struct mpSnap_s {
	int			angle;		// degrees per rotation step
	int			grid;		// units per nudge / follow snap
} mpSnap_t;

static const mpSnap_t mpSnaps[] = {
	{ 1, 1 },
	{ 5, 4 },
	{ 15, 8 },
	{ 45, 16 },
	{ 90, 32 },
};

static struct {
	mpState_t	state;
	int			font;

	// index, rebuilt the first time the placer opens after cgame (re)starts
	qboolean	indexed;
	int			numModels;
	mpModel_t	models[MP_MAX_MODELS];
	int			numFolders;
	mpFolder_t	folders[MP_MAX_FOLDERS];
	char		pool[MP_NAME_POOL];
	int			poolUsed;
	int			hash[MP_HASH_SIZE];	// model index + 1, 0 = empty

	// browser
	char		search[64];
	int			folder;
	int			view[MP_MAX_MODELS];	// model indices currently listed
	int			numView;
	int			selRow;
	int			modelScroll, folderScroll;
	float		cursorX, cursorY;
	int			lastClickTime, lastClickRow;

	// placement
	int			model;			// index into models, -1 = none chosen yet
	vec3_t		mins, maxs;
	vec3_t		camOrg, camAng;
	vec3_t		origin, angles;
	int			scale;			// percent
	qboolean	solid;
	qboolean	locked;			// qfalse = ghost follows the crosshair
	qboolean	hideHelp;
	int			snap;
	qboolean	held[MAX_KEYS];
	int			lastFrameTime;
	int			sentTime;
	char		lastCmd[MAX_STRING_CHARS];

	// the cgame's last main view, where the free camera starts
	qboolean	haveView;
	vec3_t		viewOrg, viewAng;
	int			viewFrame;		// cls.framecount the main view was last re-aimed on
} mp;

static vec4_t mpWhite		= { 1.0f, 1.0f, 1.0f, 1.0f };
static vec4_t mpRed			= { 1.0f, 0.3f, 0.3f, 1.0f };
static vec4_t mpPanel		= { 0.0f, 0.0f, 0.0f, 0.65f };
static vec4_t mpPanelLight	= { 0.15f, 0.15f, 0.15f, 0.8f };
static vec4_t mpHighlight	= { 0.2f, 0.45f, 0.8f, 0.8f };
static vec4_t mpHover		= { 1.0f, 1.0f, 1.0f, 0.12f };
static vec4_t mpBorder		= { 0.5f, 0.5f, 0.5f, 0.8f };
static vec4_t mpDim			= { 0.7f, 0.7f, 0.7f, 1.0f };
static vec4_t mpAccent		= { 1.0f, 0.8f, 0.3f, 1.0f };

/*
===============================================================================

INDEX

===============================================================================
*/

static int MP_HashName( const char *s ) {
	unsigned int h = 5381;

	while ( *s ) {
		h = h * 33 + (unsigned int)tolower( (unsigned char)*s );
		s++;
	}
	return (int)( h % MP_HASH_SIZE );
}

static const char *MP_PoolString( const char *s ) {
	int len = strlen( s ) + 1;
	char *out;

	if ( mp.poolUsed + len > MP_NAME_POOL )
		return NULL;
	out = mp.pool + mp.poolUsed;
	memcpy( out, s, len );
	mp.poolUsed += len;
	return out;
}

// name is a full game path, e.g. "models/map_objects/bespin/panels.md3"
static void MP_AddModel( const char *name, void *ctx ) {
	char path[MAX_QPATH];
	const int rootLen = strlen( MP_ROOT ), extLen = strlen( MP_EXT );
	const char *stored, *slash;
	int h, len;

	len = strlen( name );
	if ( len <= rootLen + 1 + extLen || Q_stricmpn( name, MP_ROOT "/", rootLen + 1 ) )
		return;
	Q_strncpyz( path, name + rootLen + 1, sizeof( path ) );
	path[strlen( path ) - extLen] = '\0';
	for ( char *c = path; *c; c++ ) {
		if ( *c == '\\' )
			*c = '/';
	}

	// the same file can be in several pk3s and folders
	h = MP_HashName( path );
	while ( mp.hash[h] ) {
		if ( !Q_stricmp( mp.models[mp.hash[h] - 1].path, path ) )
			return;
		h = ( h + 1 ) % MP_HASH_SIZE;
	}

	if ( mp.numModels >= MP_MAX_MODELS )
		return;
	stored = MP_PoolString( path );
	if ( !stored )
		return;

	slash = strrchr( stored, '/' );
	mp.models[mp.numModels].path = stored;
	mp.models[mp.numModels].base = slash ? slash + 1 : stored;
	mp.models[mp.numModels].failed = qfalse;
	mp.hash[h] = ++mp.numModels;
}

static int MP_FolderLen( const mpModel_t *m ) {
	return (int)( m->base - m->path ) - ( m->base != m->path ? 1 : 0 );
}

static int QDECL MP_CompareModels( const void *a, const void *b ) {
	const mpModel_t *ma = (const mpModel_t *)a, *mb = (const mpModel_t *)b;
	int la = MP_FolderLen( ma ), lb = MP_FolderLen( mb );
	int cmp = Q_stricmpn( ma->path, mb->path, la < lb ? la : lb );

	// group by folder first so every folder is one contiguous range
	if ( cmp )
		return cmp;
	if ( la != lb )
		return la - lb;
	return Q_stricmp( ma->base, mb->base );
}

static void MP_BuildIndex( void ) {
	int start = Sys_Milliseconds();

	mp.numModels = 0;
	mp.numFolders = 0;
	mp.poolUsed = 0;
	memset( mp.hash, 0, sizeof( mp.hash ) );

	// the pk3 directories are already in memory, only loose folders touch the disk
	FS_ListFilesRecursive( MP_ROOT, MP_EXT, MP_AddModel, NULL );

	qsort( mp.models, mp.numModels, sizeof( mp.models[0] ), MP_CompareModels );

	for ( int i = 0; i < mp.numModels; i++ ) {
		mpModel_t *m = &mp.models[i];
		int len = MP_FolderLen( m );
		mpFolder_t *f = mp.numFolders ? &mp.folders[mp.numFolders - 1] : NULL;

		if ( !f || (int)strlen( f->name ) != len || Q_stricmpn( f->name, m->path, len ) ) {
			if ( mp.numFolders >= MP_MAX_FOLDERS ) {
				mp.numModels = i;
				break;
			}
			f = &mp.folders[mp.numFolders++];
			Q_strncpyz( f->name, m->path, len + 1 < (int)sizeof( f->name ) ? len + 1 : (int)sizeof( f->name ) );
			f->first = i;
			f->count = 0;
		}
		f->count++;
		m->folder = mp.numFolders - 1;
	}

	mp.indexed = qtrue;
	mp.model = -1;
	mp.folder = mp.folderScroll = mp.selRow = mp.modelScroll = 0;
	Com_Printf( "Model placer: indexed %i models in %i folders (%i ms)\n", mp.numModels, mp.numFolders, Sys_Milliseconds() - start );
}

/*
===============================================================================

HELPERS

===============================================================================
*/

static qboolean MP_ContainsNoCase( const char *haystack, const char *needle ) {
	int nlen = strlen( needle );

	if ( !nlen )
		return qtrue;
	for ( ; *haystack; haystack++ ) {
		if ( !Q_stricmpn( haystack, needle, nlen ) )
			return qtrue;
	}
	return qfalse;
}

static void MP_RebuildView( void ) {
	mp.numView = 0;
	if ( mp.search[0] ) {
		char full[MAX_QPATH];

		for ( int i = 0; i < mp.numModels; i++ ) {
			Com_sprintf( full, sizeof( full ), "%s/%s", MP_SEND_PREFIX, mp.models[i].path );
			if ( MP_ContainsNoCase( full, mp.search ) )
				mp.view[mp.numView++] = i;
		}
	} else if ( mp.numFolders ) {
		const mpFolder_t *f = &mp.folders[mp.folder];

		for ( int i = 0; i < f->count; i++ )
			mp.view[mp.numView++] = f->first + i;
	}

	mp.selRow = Com_Clampi( 0, mp.numView ? mp.numView - 1 : 0, mp.selRow );
	mp.modelScroll = Com_Clampi( 0, mp.numView > MP_VISIBLE_ROWS ? mp.numView - MP_VISIBLE_ROWS : 0, mp.modelScroll );
}

static void MP_ScrollToSelection( void ) {
	if ( mp.selRow < mp.modelScroll )
		mp.modelScroll = mp.selRow;
	else if ( mp.selRow >= mp.modelScroll + MP_VISIBLE_ROWS )
		mp.modelScroll = mp.selRow - MP_VISIBLE_ROWS + 1;
}

static void MP_SetFolder( int folder ) {
	if ( !mp.numFolders )
		return;
	mp.folder = Com_Clampi( 0, mp.numFolders - 1, folder );
	mp.search[0] = '\0';
	mp.selRow = 0;
	mp.modelScroll = 0;
	if ( mp.folder < mp.folderScroll )
		mp.folderScroll = mp.folder;
	else if ( mp.folder >= mp.folderScroll + MP_VISIBLE_ROWS )
		mp.folderScroll = mp.folder - MP_VISIBLE_ROWS + 1;
	MP_RebuildView();
}

static int MP_SelectedModel( void ) {
	if ( mp.selRow < 0 || mp.selRow >= mp.numView )
		return -1;
	return mp.view[mp.selRow];
}

// the renderer caches models by name and drops them on map change or vid_restart,
// so look the handle up whenever it is needed instead of keeping it
static qhandle_t MP_RegisterModel( int index ) {
	mpModel_t *m;
	qhandle_t h;

	if ( index < 0 || index >= mp.numModels )
		return 0;
	m = &mp.models[index];
	if ( m->failed )
		return 0;
	h = re->RegisterModel( va( "%s/%s%s", MP_ROOT, m->path, MP_EXT ) );
	if ( !h )
		m->failed = qtrue;
	return h;
}

static const char *MP_ModelName( int index ) {
	if ( index < 0 || index >= mp.numModels )
		return "";
	return va( "%s/%s", MP_SEND_PREFIX, mp.models[index].path );
}

static int MP_NormalizeAngle( float a ) {
	int i = (int)floorf( a + 0.5f ) % 360;
	return i < 0 ? i + 360 : i;
}

static float MP_SnapTo( float v, int grid ) {
	return grid > 1 ? floorf( v / grid + 0.5f ) * grid : floorf( v + 0.5f );
}

static void MP_BuildAxis( matrix3_t axis ) {
	AnglesToAxis( mp.angles, axis );
	if ( mp.scale != 100 ) {
		float s = mp.scale / 100.0f;

		VectorScale( axis[0], s, axis[0] );
		VectorScale( axis[1], s, axis[1] );
		VectorScale( axis[2], s, axis[2] );
	}
}

// corner i of the model bounds, rotated and scaled, relative to the model origin
static void MP_Corner( matrix3_t axis, int i, vec3_t out ) {
	vec3_t local;

	local[0] = ( i & 1 ) ? mp.maxs[0] : mp.mins[0];
	local[1] = ( i & 2 ) ? mp.maxs[1] : mp.mins[1];
	local[2] = ( i & 4 ) ? mp.maxs[2] : mp.mins[2];
	for ( int j = 0; j < 3; j++ )
		out[j] = local[0] * axis[0][j] + local[1] * axis[1][j] + local[2] * axis[2][j];
}

static const char *MP_BuildCommand( void ) {
	return va( "rpmodel add %s %i %i %i %i %i %i %i %i",
		MP_ModelName( mp.model ),
		(int)floorf( mp.origin[0] + 0.5f ), (int)floorf( mp.origin[1] + 0.5f ), (int)floorf( mp.origin[2] + 0.5f ),
		MP_NormalizeAngle( mp.angles[PITCH] ), MP_NormalizeAngle( mp.angles[YAW] ), MP_NormalizeAngle( mp.angles[ROLL] ),
		mp.scale, mp.solid ? 1 : 0 );
}

static qboolean MP_ShiftDown( void ) {
	return (qboolean)( mp.held[A_SHIFT] || mp.held[A_SHIFT2] );
}

static qboolean MP_CtrlDown( void ) {
	return (qboolean)( mp.held[A_CTRL] || mp.held[A_CTRL2] );
}

/*
===============================================================================

STATE CHANGES

===============================================================================
*/

static void MP_ReleaseKeys( void ) {
	memset( mp.held, 0, sizeof( mp.held ) );
}

static void MP_EnterBrowse( void ) {
	mp.state = MP_BROWSE;
	MP_ReleaseKeys();
}

static void MP_EnterPlace( int model ) {
	qhandle_t h = MP_RegisterModel( model );

	if ( !h ) {
		Com_Printf( S_COLOR_YELLOW "Model placer: can't load %s/%s%s\n", MP_ROOT, mp.models[model].path, MP_EXT );
		return;
	}

	mp.model = model;
	re->ModelBounds( h, mp.mins, mp.maxs );
	mp.state = MP_PLACE;
	mp.locked = qfalse;
	mp.lastFrameTime = cls.realtime;
	MP_ReleaseKeys();
}

static void MP_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Model placer: join a server first\n" );
		return;
	}
	if ( !mp.indexed )
		MP_BuildIndex();
	if ( !mp.numModels ) {
		Com_Printf( "Model placer: no models found in %s\n", MP_ROOT );
		return;
	}

	mp.font = re->RegisterFont( "arialnb" );
	if ( !mp.font )
		mp.font = cls.menuFont;

	if ( mp.haveView ) {
		VectorCopy( mp.viewOrg, mp.camOrg );
		VectorCopy( mp.viewAng, mp.camAng );
	} else {
		VectorCopy( cl.snap.ps.origin, mp.camOrg );
		mp.camOrg[2] += cl.snap.ps.viewheight;
		VectorCopy( cl.viewangles, mp.camAng );
	}
	mp.camAng[ROLL] = 0;
	mp.cursorX = SCREEN_WIDTH * 0.5f;
	mp.cursorY = SCREEN_HEIGHT * 0.5f;
	MP_RebuildView();
	MP_EnterBrowse();
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_MODELPLACER );
}

static void MP_Close( void ) {
	mp.state = MP_OFF;
	MP_ReleaseKeys();
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_MODELPLACER );
}

static void MP_Send( void ) {
	if ( mp.model < 0 )
		return;
	Q_strncpyz( mp.lastCmd, MP_BuildCommand(), sizeof( mp.lastCmd ) );
	CL_AddReliableCommand( mp.lastCmd, qfalse );
	Com_Printf( S_COLOR_GREEN "Model placer: " S_COLOR_WHITE "%s\n", mp.lastCmd );
	mp.sentTime = cls.realtime;
}

void CL_ModelPlacer_Init( void ) {
	memset( &mp, 0, sizeof( mp ) );
	mp.model = -1;
	mp.scale = 100;
	mp.solid = qtrue;
	mp.snap = 2;
}

// cgame is going away: map change, disconnect or vid_restart
void CL_ModelPlacer_Shutdown( void ) {
	if ( mp.state != MP_OFF )
		MP_Close();
	// the pure pk3 list can change with the next map or server
	mp.indexed = qfalse;
	mp.haveView = qfalse;
}

/*
===============================================================================

CONSOLE COMMAND

===============================================================================
*/

static void MP_PrintUsage( void ) {
	Com_Printf( "usage: modelplacer                    toggle the model placer (bind a key to it)\n" );
	Com_Printf( "       modelplacer <search>           open the browser filtered by <search>\n" );
	Com_Printf( "       modelplacer scale <percent>    set the scale sent to the server\n" );
	Com_Printf( "       modelplacer solid <0|1>        set whether the model is solid\n" );
	Com_Printf( "       modelplacer angles <p> <y> <r> set the model angles\n" );
	Com_Printf( "       modelplacer origin <x> <y> <z> lock the model at a position\n" );
	Com_Printf( "       modelplacer reindex            rescan the filesystem for models\n" );
	Com_Printf( "       modelplacer close              close the model placer\n" );
}

void CL_ModelPlacer_f( void ) {
	const char *arg = Cmd_Argv( 1 );
	int argc = Cmd_Argc();

	if ( argc < 2 ) {
		if ( mp.state == MP_OFF )
			MP_Open();
		else
			MP_Close();
		return;
	}

	if ( !Q_stricmp( arg, "help" ) || !Q_stricmp( arg, "?" ) ) {
		MP_PrintUsage();
	} else if ( !Q_stricmp( arg, "close" ) ) {
		if ( mp.state != MP_OFF )
			MP_Close();
	} else if ( !Q_stricmp( arg, "reindex" ) ) {
		if ( mp.state == MP_PLACE )
			MP_EnterBrowse();
		MP_BuildIndex();
		MP_RebuildView();
	} else if ( !Q_stricmp( arg, "scale" ) && argc >= 3 ) {
		mp.scale = Com_Clampi( MP_SCALE_MIN, MP_SCALE_MAX, atoi( Cmd_Argv( 2 ) ) );
		Com_Printf( "Model placer: scale %i%%\n", mp.scale );
	} else if ( !Q_stricmp( arg, "solid" ) && argc >= 3 ) {
		mp.solid = (qboolean)( atoi( Cmd_Argv( 2 ) ) != 0 );
		Com_Printf( "Model placer: solid %i\n", mp.solid ? 1 : 0 );
	} else if ( !Q_stricmp( arg, "angles" ) && argc >= 5 ) {
		mp.angles[PITCH] = atof( Cmd_Argv( 2 ) );
		mp.angles[YAW] = atof( Cmd_Argv( 3 ) );
		mp.angles[ROLL] = atof( Cmd_Argv( 4 ) );
	} else if ( !Q_stricmp( arg, "origin" ) && argc >= 5 ) {
		mp.origin[0] = atof( Cmd_Argv( 2 ) );
		mp.origin[1] = atof( Cmd_Argv( 3 ) );
		mp.origin[2] = atof( Cmd_Argv( 4 ) );
		mp.locked = qtrue;
	} else {
		// anything else is a search
		if ( mp.state == MP_OFF )
			MP_Open();
		if ( mp.state == MP_OFF )
			return;
		Q_strncpyz( mp.search, arg, sizeof( mp.search ) );
		mp.selRow = mp.modelScroll = 0;
		MP_RebuildView();
		MP_EnterBrowse();
	}
}

/*
===============================================================================

INPUT

===============================================================================
*/

static qboolean MP_InRect( float x, float y, float w, float h ) {
	return (qboolean)( mp.cursorX >= x && mp.cursorX < x + w && mp.cursorY >= y && mp.cursorY < y + h );
}

// binds don't run while the placer holds the keys, so honour the user's toggle bind here
static qboolean MP_IsToggleKey( int key ) {
	const char *binding;

	// a printable key belongs to the search box while browsing
	if ( mp.state == MP_BROWSE && key >= A_SPACE && key <= A_TILDE )
		return qfalse;
	binding = Key_GetBinding( key );
	return (qboolean)( VALIDSTRING( binding ) && !Q_stricmp( binding, MP_TOGGLE_CMD ) );
}

static void MP_BrowseClick( void ) {
	int row;

	if ( MP_InRect( MP_FOLDER_X, MP_LIST_Y, MP_FOLDER_W, MP_LIST_H ) ) {
		row = mp.folderScroll + (int)( ( mp.cursorY - MP_LIST_Y ) / MP_ROW_H );
		if ( row < mp.numFolders )
			MP_SetFolder( row );
		return;
	}

	if ( MP_InRect( MP_MODEL_X, MP_LIST_Y, MP_MODEL_W, MP_LIST_H ) ) {
		row = mp.modelScroll + (int)( ( mp.cursorY - MP_LIST_Y ) / MP_ROW_H );
		if ( row >= mp.numView )
			return;
		if ( row == mp.lastClickRow && cls.realtime - mp.lastClickTime < MP_DOUBLECLICK_MS ) {
			mp.lastClickTime = 0;
			MP_EnterPlace( mp.view[row] );
			return;
		}
		mp.selRow = row;
		mp.lastClickRow = row;
		mp.lastClickTime = cls.realtime;
		return;
	}

	if ( MP_InRect( MP_PREVIEW_X, MP_BUTTON_Y, MP_PREVIEW_W, MP_BUTTON_H ) && MP_SelectedModel() >= 0 )
		MP_EnterPlace( MP_SelectedModel() );
}

static void MP_BrowseChar( int ch ) {
	int len = strlen( mp.search );

	if ( ch >= 32 && ch < 127 && len < (int)sizeof( mp.search ) - 1 ) {
		mp.search[len] = (char)ch;
		mp.search[len + 1] = '\0';
		mp.selRow = mp.modelScroll = 0;
		MP_RebuildView();
	}
}

static void MP_BrowseKey( int key ) {
	int len;

	switch ( key ) {
	case A_BACKSPACE:
		len = strlen( mp.search );
		if ( len ) {
			mp.search[len - 1] = '\0';
			mp.selRow = mp.modelScroll = 0;
			MP_RebuildView();
		}
		break;
	case A_DELETE:
		mp.search[0] = '\0';
		MP_RebuildView();
		break;
	case A_CURSOR_UP:
		mp.selRow = Com_Clampi( 0, mp.numView - 1, mp.selRow - 1 );
		MP_ScrollToSelection();
		break;
	case A_CURSOR_DOWN:
		mp.selRow = Com_Clampi( 0, mp.numView - 1, mp.selRow + 1 );
		MP_ScrollToSelection();
		break;
	case A_PAGE_UP:
		mp.selRow = Com_Clampi( 0, mp.numView - 1, mp.selRow - MP_VISIBLE_ROWS );
		MP_ScrollToSelection();
		break;
	case A_PAGE_DOWN:
		mp.selRow = Com_Clampi( 0, mp.numView - 1, mp.selRow + MP_VISIBLE_ROWS );
		MP_ScrollToSelection();
		break;
	case A_HOME:
		mp.selRow = 0;
		MP_ScrollToSelection();
		break;
	case A_END:
		mp.selRow = mp.numView ? mp.numView - 1 : 0;
		MP_ScrollToSelection();
		break;
	case A_CURSOR_LEFT:
		MP_SetFolder( mp.search[0] ? mp.folder : mp.folder - 1 );
		break;
	case A_CURSOR_RIGHT:
		MP_SetFolder( mp.search[0] ? mp.folder : mp.folder + 1 );
		break;
	case A_ENTER:
	case A_KP_ENTER:
		if ( MP_SelectedModel() >= 0 )
			MP_EnterPlace( MP_SelectedModel() );
		break;
	case A_TAB:
		if ( mp.model >= 0 )
			MP_EnterPlace( mp.model );
		break;
	case A_MOUSE1:
		MP_BrowseClick();
		break;
	case A_MWHEELUP:
	case A_MWHEELDOWN: {
		int delta = key == A_MWHEELUP ? -3 : 3;

		if ( MP_InRect( MP_FOLDER_X, MP_LIST_Y, MP_FOLDER_W, MP_LIST_H ) )
			mp.folderScroll = Com_Clampi( 0, mp.numFolders > MP_VISIBLE_ROWS ? mp.numFolders - MP_VISIBLE_ROWS : 0, mp.folderScroll + delta );
		else
			mp.modelScroll = Com_Clampi( 0, mp.numView > MP_VISIBLE_ROWS ? mp.numView - MP_VISIBLE_ROWS : 0, mp.modelScroll + delta );
		break;
	}
	default:
		break;
	}
}

// moves along the world axis closest to the camera direction, so nudges stay on the grid
static void MP_Nudge( qboolean forwardAxis, int sign ) {
	vec3_t forward, right;
	float yaw = DEG2RAD( floorf( mp.camAng[YAW] / 90.0f + 0.5f ) * 90.0f );
	int grid = mpSnaps[mp.snap].grid;

	VectorSet( forward, cosf( yaw ), sinf( yaw ), 0 );
	VectorSet( right, sinf( yaw ), -cosf( yaw ), 0 );

	mp.locked = qtrue;
	VectorMA( mp.origin, (float)( sign * grid ), forwardAxis ? forward : right, mp.origin );
	mp.origin[0] = floorf( mp.origin[0] + 0.5f );
	mp.origin[1] = floorf( mp.origin[1] + 0.5f );
}

static void MP_Rotate( int axis, int sign ) {
	int step = mpSnaps[mp.snap].angle;

	mp.angles[axis] = (float)MP_NormalizeAngle( MP_SnapTo( mp.angles[axis] + sign * step, step ) );
}

static void MP_ChangeScale( int sign ) {
	int step = MP_ShiftDown() ? 25 : 5;

	mp.scale = Com_Clampi( MP_SCALE_MIN, MP_SCALE_MAX, mp.scale + sign * step );
}

static void MP_PlaceKey( int key ) {
	switch ( key ) {
	case A_MOUSE1:
	case A_ENTER:
	case A_KP_ENTER:
		MP_Send();
		break;
	case A_MOUSE2:
	case A_CAP_F:
		mp.locked = (qboolean)!mp.locked;
		break;
	case A_MWHEELUP:	MP_Rotate( YAW, 1 );	break;
	case A_MWHEELDOWN:	MP_Rotate( YAW, -1 );	break;
	case A_CAP_Z:		MP_Rotate( PITCH, -1 );	break;
	case A_CAP_X:		MP_Rotate( PITCH, 1 );	break;
	case A_CAP_Q:		MP_Rotate( ROLL, -1 );	break;
	case A_CAP_E:		MP_Rotate( ROLL, 1 );	break;
	case A_CAP_R:
		VectorClear( mp.angles );
		break;
	case A_BACKSPACE:
		VectorClear( mp.angles );
		mp.scale = 100;
		break;
	case A_OPEN_SQUARE:
	case A_MINUS:
	case A_KP_MINUS:
		MP_ChangeScale( -1 );
		break;
	case A_CLOSE_SQUARE:
	case A_EQUALS:
	case A_PLUS:
	case A_KP_PLUS:
		MP_ChangeScale( 1 );
		break;
	case A_CAP_T:
		mp.solid = (qboolean)!mp.solid;
		break;
	case A_CAP_G:
		mp.snap = ( mp.snap + 1 ) % ARRAY_LEN( mpSnaps );
		break;
	case A_CAP_H:
		mp.hideHelp = (qboolean)!mp.hideHelp;
		break;
	case A_CURSOR_UP:		MP_Nudge( qtrue, 1 );	break;
	case A_CURSOR_DOWN:		MP_Nudge( qtrue, -1 );	break;
	case A_CURSOR_RIGHT:	MP_Nudge( qfalse, 1 );	break;
	case A_CURSOR_LEFT:		MP_Nudge( qfalse, -1 );	break;
	case A_PAGE_UP:
		mp.locked = qtrue;
		mp.origin[2] += mpSnaps[mp.snap].grid;
		break;
	case A_PAGE_DOWN:
		mp.locked = qtrue;
		mp.origin[2] -= mpSnaps[mp.snap].grid;
		break;
	case A_TAB:
		MP_EnterBrowse();
		break;
	default:
		break;
	}
}

qboolean CL_ModelPlacer_Active( void ) {
	return (qboolean)( mp.state != MP_OFF );
}

void CL_ModelPlacer_KeyEvent( int key, qboolean down ) {
	if ( mp.state == MP_OFF )
		return;

	if ( key >= 0 && key < MAX_KEYS )
		mp.held[key] = down;
	if ( !down )
		return;

	if ( MP_IsToggleKey( key ) ) {
		MP_Close();
		return;
	}

	if ( mp.state == MP_BROWSE )
		MP_BrowseKey( key );
	else
		MP_PlaceKey( key );
}

void CL_ModelPlacer_CharEvent( int ch ) {
	if ( mp.state == MP_BROWSE )
		MP_BrowseChar( ch );
}

void CL_ModelPlacer_MouseEvent( int dx, int dy ) {
	if ( mp.state == MP_BROWSE ) {
		mp.cursorX = Com_Clamp( 0, SCREEN_WIDTH, mp.cursorX + dx );
		mp.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, mp.cursorY + dy );
	} else if ( mp.state == MP_PLACE ) {
		mp.camAng[YAW] -= dx * cl_sensitivity->value * m_yaw->value;
		mp.camAng[PITCH] = Com_Clamp( -89.0f, 89.0f, mp.camAng[PITCH] + dy * cl_sensitivity->value * m_pitch->value );
	}
}

// escape steps back from placing to the browser, and closes the browser
void CL_ModelPlacer_Escape( void ) {
	if ( mp.state == MP_PLACE )
		MP_EnterBrowse();
	else if ( mp.state == MP_BROWSE )
		MP_Close();
}

/*
===============================================================================

3D VIEW

===============================================================================
*/

static void MP_UpdateCamera( float dt ) {
	vec3_t forward, right, move;
	float speed;

	AngleVectors( mp.camAng, forward, right, NULL );
	VectorClear( move );
	if ( mp.held[A_CAP_W] )		VectorAdd( move, forward, move );
	if ( mp.held[A_CAP_S] )		VectorSubtract( move, forward, move );
	if ( mp.held[A_CAP_D] )		VectorAdd( move, right, move );
	if ( mp.held[A_CAP_A] )		VectorSubtract( move, right, move );
	if ( mp.held[A_SPACE] )		move[2] += 1.0f;
	if ( mp.held[A_CAP_C] )		move[2] -= 1.0f;

	if ( VectorNormalize( move ) > 0.0f ) {
		speed = MP_FLY_SPEED;
		if ( MP_ShiftDown() )
			speed *= 3.0f;
		if ( MP_CtrlDown() )
			speed *= 0.25f;
		VectorMA( mp.camOrg, speed * dt, move, mp.camOrg );
	}
}

// put the ghost where the crosshair meets the world, resting on that surface
static void MP_FollowCrosshair( void ) {
	vec3_t forward, end, normal, corner;
	matrix3_t axis;
	trace_t tr;
	float minDot = 0.0f, d;
	int grid = mpSnaps[mp.snap].grid;

	AngleVectors( mp.camAng, forward, NULL, NULL );
	VectorMA( mp.camOrg, MP_TRACE_DIST, forward, end );
	CM_BoxTrace( &tr, mp.camOrg, end, vec3_origin, vec3_origin, 0, MP_TRACE_MASK, qfalse );

	if ( tr.fraction < 1.0f && !tr.startsolid && !( tr.surfaceFlags & SURF_SKY ) ) {
		VectorCopy( tr.plane.normal, normal );

		MP_BuildAxis( axis );
		for ( int i = 0; i < 8; i++ ) {
			MP_Corner( axis, i, corner );
			d = DotProduct( corner, normal );
			if ( i == 0 || d < minDot )
				minDot = d;
		}
		VectorMA( tr.endpos, -minDot, normal, mp.origin );

		// snap along the surface, keep the offset that makes it rest on it
		for ( int i = 0; i < 3; i++ ) {
			if ( fabsf( normal[i] ) < 0.7f )
				mp.origin[i] = MP_SnapTo( mp.origin[i], grid );
		}
	} else {
		VectorMA( mp.camOrg, MP_NO_HIT_DIST, forward, mp.origin );
		for ( int i = 0; i < 3; i++ )
			mp.origin[i] = MP_SnapTo( mp.origin[i], grid );
	}
}

// called every frame before the cgame draws
void CL_ModelPlacer_Frame( void ) {
	float dt;

	if ( mp.state == MP_OFF )
		return;

	if ( !( Key_GetCatcher() & KEYCATCH_MODELPLACER ) ) {
		// something else cleared the catchers
		mp.state = MP_OFF;
		MP_ReleaseKeys();
		return;
	}

	dt = Com_Clamp( 0.0f, 0.1f, ( cls.realtime - mp.lastFrameTime ) / 1000.0f );
	mp.lastFrameTime = cls.realtime;

	if ( mp.state == MP_PLACE ) {
		MP_UpdateCamera( dt );
		if ( !mp.locked )
			MP_FollowCrosshair();
	}
}

static void MP_AddEdge( const vec3_t a, const vec3_t b, const byte *color ) {
	polyVert_t verts[4], back[4];
	vec3_t dir, toCam, side, mid;
	float width;

	VectorSubtract( b, a, dir );
	VectorAdd( a, b, mid );
	VectorScale( mid, 0.5f, mid );
	VectorSubtract( mp.camOrg, mid, toCam );
	width = 0.4f + VectorLength( toCam ) * 0.0015f;
	CrossProduct( dir, toCam, side );
	if ( VectorNormalize( side ) <= 0.0f )
		return;
	VectorScale( side, width, side );

	VectorAdd( a, side, verts[0].xyz );
	VectorAdd( b, side, verts[1].xyz );
	VectorSubtract( b, side, verts[2].xyz );
	VectorSubtract( a, side, verts[3].xyz );
	for ( int i = 0; i < 4; i++ ) {
		verts[i].st[0] = ( i == 1 || i == 2 ) ? 1.0f : 0.0f;
		verts[i].st[1] = ( i >= 2 ) ? 1.0f : 0.0f;
		memcpy( verts[i].modulate, color, sizeof( verts[i].modulate ) );
	}
	// both windings, the white shader is single sided
	for ( int i = 0; i < 4; i++ )
		back[i] = verts[3 - i];
	re->AddPolyToScene( cls.whiteShader, 4, verts, 1 );
	re->AddPolyToScene( cls.whiteShader, 4, back, 1 );
}

static void MP_AddGhost( void ) {
	static const int edges[12][2] = {
		{ 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
		{ 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
		{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
	};
	static const byte sentColor[4] = { 255, 255, 255, 255 };
	static const byte solidColor[4] = { 64, 255, 96, 255 };
	static const byte nonSolidColor[4] = { 255, 200, 64, 255 };
	const byte *color;
	vec3_t corners[8];
	refEntity_t ent;

	memset( &ent, 0, sizeof( ent ) );
	ent.hModel = MP_RegisterModel( mp.model );
	if ( !ent.hModel )
		return;
	VectorCopy( mp.origin, ent.origin );
	VectorCopy( mp.origin, ent.oldorigin );
	VectorCopy( mp.origin, ent.lightingOrigin );
	MP_BuildAxis( ent.axis );
	ent.nonNormalizedAxes = (qboolean)( mp.scale != 100 );
	ent.renderfx = RF_NOSHADOW;
	re->AddRefEntityToScene( &ent );

	if ( cls.realtime - mp.sentTime < 300 )
		color = sentColor;	// flash after sending
	else
		color = mp.solid ? solidColor : nonSolidColor;

	for ( int i = 0; i < 8; i++ ) {
		MP_Corner( ent.axis, i, corners[i] );
		VectorAdd( corners[i], mp.origin, corners[i] );
	}
	for ( int i = 0; i < 12; i++ )
		MP_AddEdge( corners[edges[i][0]], corners[edges[i][1]], color );
}

// every cgame entity passes through here; the view weapon would float where the player stands
qboolean CL_ModelPlacer_FilterEntity( const refEntity_t *ent ) {
	return (qboolean)( mp.state != MP_OFF && ( ent->renderfx & RF_FIRST_PERSON ) );
}

// every cgame scene passes through here before reaching the renderer
void CL_ModelPlacer_RenderScene( const refdef_t *fd ) {
	refdef_t view;

	if ( fd->rdflags & ( RDF_NOWORLDMODEL | RDF_AUTOMAP ) ) {
		re->RenderScene( fd );
		return;
	}

	if ( fd->rdflags & RDF_SKYBOXPORTAL ) {
		// the sky portal keeps its own origin but looks the way the camera does
		if ( mp.state != MP_OFF ) {
			view = *fd;
			AnglesToAxis( mp.camAng, view.viewaxis );
			VectorCopy( mp.camAng, view.viewangles );
			re->RenderScene( &view );
			return;
		}
		re->RenderScene( fd );
		return;
	}

	// the main view
	if ( mp.state == MP_OFF ) {
		VectorCopy( fd->vieworg, mp.viewOrg );
		vectoangles( fd->viewaxis[0], mp.viewAng );
		mp.haveView = qtrue;
		re->RenderScene( fd );
		return;
	}

	if ( mp.viewFrame == cls.framecount ) {
		re->RenderScene( fd );
		return;
	}
	mp.viewFrame = cls.framecount;

	view = *fd;
	VectorCopy( mp.camOrg, view.vieworg );
	VectorCopy( mp.camAng, view.viewangles );
	AnglesToAxis( mp.camAng, view.viewaxis );
	view.viewContents = CM_PointContents( mp.camOrg, 0 );
	// the snapshot's area mask is from the player's position, the camera can be anywhere
	memset( view.areamask, 0, sizeof( view.areamask ) );

	if ( mp.state == MP_PLACE && mp.model >= 0 )
		MP_AddGhost();

	re->RenderScene( &view );
}

/*
===============================================================================

2D

===============================================================================
*/

static void MP_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void MP_Box( float x, float y, float w, float h, const float *fill ) {
	MP_Fill( x, y, w, h, fill );
	MP_Fill( x, y, w, 1, mpBorder );
	MP_Fill( x, y + h - 1, w, 1, mpBorder );
	MP_Fill( x, y, 1, h, mpBorder );
	MP_Fill( x + w - 1, y, 1, h, mpBorder );
}

static void MP_Text( float x, float y, const char *text, const float *color ) {
	re->Font_DrawString( (int)x, (int)y, text, color, mp.font | STYLE_DROPSHADOW, -1, MP_TEXT_SCALE );
}

static void MP_TextClipped( float x, float y, float w, const char *text, const float *color ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 3 && re->Font_StrLenPixels( buf, mp.font, MP_TEXT_SCALE ) > w ) {
		len--;
		buf[len - 3] = '.';
		buf[len - 2] = '.';
		buf[len - 1] = '.';
		buf[len] = '\0';
	}
	MP_Text( x, y, buf, color );
}

static void MP_DrawPreview( int index ) {
	refdef_t refdef;
	refEntity_t ent;
	vec3_t mins, maxs, center, rotCenter, forward, angles;
	float radius, fovY, dist, xScale, yScale;
	qhandle_t hModel = MP_RegisterModel( index );

	MP_Box( MP_PREVIEW_X, MP_PREVIEW_Y, MP_PREVIEW_W, MP_PREVIEW_H, mpPanelLight );
	if ( !hModel ) {
		if ( index >= 0 )
			MP_Text( MP_PREVIEW_X + 8, MP_PREVIEW_Y + 8, "Can't load model", mpRed );
		return;
	}

	re->ModelBounds( hModel, mins, maxs );
	VectorAdd( mins, maxs, center );
	VectorScale( center, 0.5f, center );
	radius = Distance( mins, maxs ) * 0.5f;
	if ( radius < 1.0f )
		radius = 1.0f;

	memset( &ent, 0, sizeof( ent ) );
	ent.hModel = hModel;
	ent.renderfx = RF_NOSHADOW;
	VectorSet( angles, 0, (float)( cls.realtime % 10000 ) * 0.036f, 0 );	// one turn every 10 seconds
	AnglesToAxis( angles, ent.axis );
	for ( int i = 0; i < 3; i++ )
		rotCenter[i] = center[0] * ent.axis[0][i] + center[1] * ent.axis[1][i] + center[2] * ent.axis[2][i];

	xScale = cls.glconfig.vidWidth / (float)SCREEN_WIDTH;
	yScale = cls.glconfig.vidHeight / (float)SCREEN_HEIGHT;

	memset( &refdef, 0, sizeof( refdef ) );
	refdef.x = (int)( ( MP_PREVIEW_X + 1 ) * xScale );
	refdef.y = (int)( ( MP_PREVIEW_Y + 1 ) * yScale );
	refdef.width = (int)( ( MP_PREVIEW_W - 2 ) * xScale );
	refdef.height = (int)( ( MP_PREVIEW_H - 2 ) * yScale );
	fovY = 40.0f;
	refdef.fov_y = fovY;
	refdef.fov_x = RAD2DEG( 2.0f * atanf( tanf( DEG2RAD( fovY * 0.5f ) ) * refdef.width / (float)refdef.height ) );
	if ( refdef.fov_x < fovY )
		fovY = refdef.fov_x;
	dist = radius / sinf( DEG2RAD( fovY * 0.5f ) );

	VectorSet( angles, 20.0f, 180.0f, 0 );
	AngleVectors( angles, forward, NULL, NULL );
	VectorMA( rotCenter, -dist, forward, refdef.vieworg );
	AnglesToAxis( angles, refdef.viewaxis );
	refdef.rdflags = RDF_NOWORLDMODEL;
	refdef.time = cl.serverTime;

	re->ClearScene();
	re->AddRefEntityToScene( &ent );
	re->RenderScene( &refdef );
}

static void MP_DrawBrowser( void ) {
	char buf[MAX_STRING_CHARS];
	int row, sel = MP_SelectedModel();
	float y;

	MP_Fill( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, mpPanel );

	MP_Text( MP_FOLDER_X, 16, va( "Model Placer  " S_COLOR_GREY "%i models in %i folders", mp.numModels, mp.numFolders ), mpWhite );
	MP_Text( MP_FOLDER_X, 28, S_COLOR_GREY "Type to search across all folders. Pick a model and press Enter.", mpWhite );

	// search box
	MP_Box( MP_FOLDER_X, MP_SEARCH_Y, SCREEN_WIDTH - MP_FOLDER_X * 2, 16, mpPanelLight );
	Com_sprintf( buf, sizeof( buf ), "Search: %s%s", mp.search, ( cls.realtime >> 8 ) & 1 ? "_" : "" );
	MP_Text( MP_FOLDER_X + 4, MP_SEARCH_Y + 3, buf, mp.search[0] ? mpWhite : mpDim );

	// folders
	MP_Box( MP_FOLDER_X, MP_LIST_Y, MP_FOLDER_W, MP_LIST_H, mpPanelLight );
	for ( int i = 0; i < MP_VISIBLE_ROWS; i++ ) {
		row = mp.folderScroll + i;
		if ( row >= mp.numFolders )
			break;
		y = MP_LIST_Y + i * MP_ROW_H;
		if ( row == mp.folder && !mp.search[0] )
			MP_Fill( MP_FOLDER_X + 1, y, MP_FOLDER_W - 2, MP_ROW_H, mpHighlight );
		else if ( MP_InRect( MP_FOLDER_X, y, MP_FOLDER_W, MP_ROW_H ) )
			MP_Fill( MP_FOLDER_X + 1, y, MP_FOLDER_W - 2, MP_ROW_H, mpHover );
		MP_TextClipped( MP_FOLDER_X + 4, y + 1, MP_FOLDER_W - 8,
			va( "%s " S_COLOR_GREY "(%i)", mp.folders[row].name[0] ? mp.folders[row].name : "(root)", mp.folders[row].count ),
			mp.search[0] ? mpDim : mpWhite );
	}

	// models
	MP_Box( MP_MODEL_X, MP_LIST_Y, MP_MODEL_W, MP_LIST_H, mpPanelLight );
	if ( !mp.numView )
		MP_Text( MP_MODEL_X + 4, MP_LIST_Y + 1, "No models match", mpDim );
	for ( int i = 0; i < MP_VISIBLE_ROWS; i++ ) {
		const mpModel_t *m;

		row = mp.modelScroll + i;
		if ( row >= mp.numView )
			break;
		m = &mp.models[mp.view[row]];
		y = MP_LIST_Y + i * MP_ROW_H;
		if ( row == mp.selRow )
			MP_Fill( MP_MODEL_X + 1, y, MP_MODEL_W - 2, MP_ROW_H, mpHighlight );
		else if ( MP_InRect( MP_MODEL_X, y, MP_MODEL_W, MP_ROW_H ) )
			MP_Fill( MP_MODEL_X + 1, y, MP_MODEL_W - 2, MP_ROW_H, mpHover );
		MP_TextClipped( MP_MODEL_X + 4, y + 1, MP_MODEL_W - 8, mp.search[0] ? m->path : m->base, m->failed ? mpRed : mpWhite );
	}

	// preview and details
	MP_DrawPreview( sel );
	y = MP_PREVIEW_Y + MP_PREVIEW_H + 6;
	if ( sel >= 0 ) {
		qhandle_t h = MP_RegisterModel( sel );

		MP_TextClipped( MP_PREVIEW_X, y, MP_PREVIEW_W, MP_ModelName( sel ), mpWhite );
		if ( h ) {
			vec3_t mins, maxs;

			re->ModelBounds( h, mins, maxs );
			MP_Text( MP_PREVIEW_X, y + MP_ROW_H, va( S_COLOR_GREY "Size %i x %i x %i units",
				(int)( maxs[0] - mins[0] ), (int)( maxs[1] - mins[1] ), (int)( maxs[2] - mins[2] ) ), mpWhite );
		}
		MP_Text( MP_PREVIEW_X, y + MP_ROW_H * 2, va( S_COLOR_GREY "Scale %i%%  Solid %s", mp.scale, mp.solid ? "yes" : "no" ), mpWhite );

		MP_Box( MP_PREVIEW_X, MP_BUTTON_Y, MP_PREVIEW_W, MP_BUTTON_H,
			MP_InRect( MP_PREVIEW_X, MP_BUTTON_Y, MP_PREVIEW_W, MP_BUTTON_H ) ? mpHighlight : mpPanelLight );
		MP_Text( MP_PREVIEW_X + 8, MP_BUTTON_Y + 5, "Place this model  (Enter)", mpWhite );
	}

	MP_Text( MP_FOLDER_X, 440, S_COLOR_GREY "Up/Down select   Left/Right folder   Wheel scroll   Enter or double-click place", mpWhite );
	Com_sprintf( buf, sizeof( buf ), S_COLOR_GREY "%s%s   Esc close", mp.model >= 0 ? "Tab back to placing   " : "",
		Key_GetKey( MP_TOGGLE_CMD ) >= 0 ? "Your modelplacer bind closes" : "Tip: bind a key to modelplacer" );
	MP_Text( MP_FOLDER_X, 452, buf, mpWhite );

	re->DrawStretchPic( mp.cursorX, mp.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

static void MP_DrawPlace( void ) {
	static const char *help[] = {
		"LMB / Enter    send rpmodel add",
		"RMB / F        lock or follow crosshair",
		"Wheel          yaw",
		"Z / X          pitch",
		"Q / E          roll",
		"R              reset angles",
		"[ ] or - +     scale (Shift x5)",
		"T              toggle solid",
		"G              cycle snap",
		"Arrows PgUp/Dn nudge (locks)",
		"Backspace      reset angles + scale",
		"WASD Space C   fly (Shift fast, Ctrl slow)",
		"Tab / Esc      back to the browser",
		"H              hide this help",
	};
	const float x = 10, w = 250, cx = SCREEN_WIDTH * 0.5f, cy = SCREEN_HEIGHT * 0.5f;
	float y = 10;

	// crosshair
	MP_Fill( cx - 6, cy - 0.5f, 12, 1, mpWhite );
	MP_Fill( cx - 0.5f, cy - 6, 1, 12, mpWhite );

	MP_Box( x, y, w, 7 * MP_ROW_H + 8, mpPanel );
	y += 4;
	MP_Text( x + 6, y, va( "Model Placer  %s", mp.locked ? S_COLOR_YELLOW "LOCKED" : S_COLOR_GREEN "FOLLOWING" ), mpWhite ); y += MP_ROW_H;
	MP_TextClipped( x + 6, y, w - 12, MP_ModelName( mp.model ), mpAccent ); y += MP_ROW_H;
	MP_Text( x + 6, y, va( "Origin  %i %i %i", (int)floorf( mp.origin[0] + 0.5f ), (int)floorf( mp.origin[1] + 0.5f ), (int)floorf( mp.origin[2] + 0.5f ) ), mpWhite ); y += MP_ROW_H;
	MP_Text( x + 6, y, va( "Angles  %i %i %i", MP_NormalizeAngle( mp.angles[PITCH] ), MP_NormalizeAngle( mp.angles[YAW] ), MP_NormalizeAngle( mp.angles[ROLL] ) ), mpWhite ); y += MP_ROW_H;
	MP_Text( x + 6, y, va( "Scale   %i%%", mp.scale ), mpWhite ); y += MP_ROW_H;
	MP_Text( x + 6, y, va( "Solid   %s", mp.solid ? S_COLOR_GREEN "yes" : S_COLOR_YELLOW "no" ), mpWhite ); y += MP_ROW_H;
	MP_Text( x + 6, y, va( "Snap    %i deg / %i units", mpSnaps[mp.snap].angle, mpSnaps[mp.snap].grid ), mpWhite ); y += MP_ROW_H;

	if ( !mp.hideHelp ) {
		y += 8;
		MP_Box( x, y, w, ARRAY_LEN( help ) * MP_ROW_H + 8, mpPanel );
		y += 4;
		for ( size_t i = 0; i < ARRAY_LEN( help ); i++, y += MP_ROW_H )
			MP_Text( x + 6, y, help[i], mpDim );
	}

	// what will be sent, and what was sent last
	MP_Box( 10, SCREEN_HEIGHT - 40, SCREEN_WIDTH - 20, 30, mpPanel );
	MP_TextClipped( 16, SCREEN_HEIGHT - 37, SCREEN_WIDTH - 32, MP_BuildCommand(), mpWhite );
	if ( mp.lastCmd[0] ) {
		MP_TextClipped( 16, SCREEN_HEIGHT - 24, SCREEN_WIDTH - 32,
			va( "%sSent: %s", cls.realtime - mp.sentTime < 1500 ? S_COLOR_GREEN : S_COLOR_GREY, mp.lastCmd ), mpWhite );
	}
}

// drawn over the cgame, under the UI and console
void CL_ModelPlacer_Draw( void ) {
	if ( mp.state == MP_BROWSE )
		MP_DrawBrowser();
	else if ( mp.state == MP_PLACE )
		MP_DrawPlace();
}
