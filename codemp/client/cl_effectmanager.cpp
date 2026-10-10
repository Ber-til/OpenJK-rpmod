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

// cl_effectmanager.cpp -- engine-side front end for RPMod's /rpeffect command
//
// Effects tab: every effects/*.efx the filesystem can see, played live in a window
// onto the world, then sent as "rpeffect once|add" at the player, at a spot picked
// with a free camera, or bolted to a tag or bone of a player or NPC. Bolted ones
// are shown on the real player or NPC before they are sent.
// Active tab: the effects playing in the map, read from the server's "rpeffect list"
// reply, to remove one, several or all of them. Presets tab: alarm, fadeout and
// camerashake.
//
// Previews are played by this client's own effects system, so nobody else sees
// them. This lives in the engine rather than cgame so it works whatever cgame the
// server's mod ships (RPMod loads its own).

#include "client.h"
#include "qcommon/cm_public.h"
#include "ghoul2/G2.h"
#include "cl_cgameapi.h"
#include "FXExport.h"
#include "FxScheduler.h"

#define EM_ROOT				"effects"
#define EM_EXT				".efx"
#define EM_TOGGLE_CMD		"effectmanager"
#define EM_LIST_HEADER		"List of all effects currently playing"

#define EM_MAX_EFFECTS		8192
#define EM_MAX_FOLDERS		1024
#define EM_NAME_POOL		( EM_MAX_EFFECTS * 48 )
#define EM_HASH_SIZE		( EM_MAX_EFFECTS * 2 )
#define EM_MAX_ACTIVE		1024
#define EM_MAX_BOLTS		384
#define EM_MAX_CAPTURED		512
#define EM_MAX_QUEUE		256
#define EM_MAX_RETIRED		16
#define EM_FIELD_LEN		64

#define EM_LIST_TIMEOUT		2500	// ms without the list header before giving up on it
#define EM_LIST_LINE_GAP	1500	// ms between list lines before the list is over
#define EM_REPLY_MS			3000	// prints this soon after a command are shown as its reply
#define EM_CONFIRM_MS		3000
#define EM_DOUBLECLICK_MS	400
#define EM_SETTLE_MS		250		// the selection rests this long before the effect is loaded
#define EM_RETIRE_MS		60000	// a model copy outlives the effects bolted to it by this much
#define EM_ONCE_GAP			1000	// pause between plays of a "once" effect in the preview
#define EM_MIN_PERIOD		100
#define EM_TRACE_MASK		(CONTENTS_SOLID|CONTENTS_TERRAIN)
#define EM_TRACE_DIST		8192.0f
#define EM_NO_HIT_DIST		256.0f
#define EM_FLY_SPEED		400.0f
#define EM_TARGET_XY		40.0f	// a model this close to the entity's origin is taken as its own
#define EM_TARGET_Z			80.0f
#define EM_MAX_BOLT_INDEX	127		// the effects system keeps bolt numbers in a char

// layout, in 640x480
#define EM_ROW_H			13.0f
#define EM_CTRL_H			15.0f
#define EM_TEXT_SCALE		0.8f
#define EM_SEARCH_Y			46.0f
#define EM_LIST_Y			66.0f
#define EM_LIST_H			364.0f
#define EM_FOLDER_X			16.0f
#define EM_FOLDER_W			124.0f
#define EM_EFFECT_X			144.0f
#define EM_EFFECT_W			186.0f
#define EM_RIGHT_X			338.0f
#define EM_RIGHT_W			286.0f
#define EM_CTRL_X			( EM_RIGHT_X + 52.0f )
#define EM_CTRL_W			( EM_RIGHT_X + EM_RIGHT_W - EM_CTRL_X )
#define EM_VIEW_Y			46.0f	// the preview window
#define EM_VIEW_H			150.0f
#define EM_BUTTON_Y			410.0f
#define EM_ACTIVE_W			310.0f	// the active effects list
#define EM_PICKER_X			150.0f
#define EM_PICKER_Y			50.0f
#define EM_PICKER_W			340.0f
#define EM_PICKER_H			380.0f
#define EM_PICKER_BOLT_X	60.0f	// the bolt picker, with the model on its right
#define EM_PICKER_PREVIEW_W	200.0f

typedef enum {
	EM_OFF,
	EM_BROWSE,
	EM_PLACE,		// free camera, the effect at the crosshair or on its bolt
	EM_PICK			// active effects labelled on the map, click to check them
} emState_t;

typedef enum {
	EM_TAB_EFFECTS,
	EM_TAB_ACTIVE,
	EM_TAB_PRESETS
} emTab_t;

typedef enum {
	EM_WHERE_ME,	// no coordinates: the server uses the player's origin
	EM_WHERE_POS,
	EM_WHERE_BOLT
} emWhere_t;

typedef enum {
	EM_TARGET_ME,
	EM_TARGET_PLAYER,
	EM_TARGET_NPC
} emTarget_t;

typedef enum {
	EM_PICKER_NONE,
	EM_PICKER_PLAYER,
	EM_PICKER_NPC,
	EM_PICKER_BOLT,
	EM_PICKER_REMOVE_PLAYER,	// "remove bolted" on a player
	EM_PICKER_REMOVE_NPC
} emPicker_t;

typedef enum {
	EM_F_NONE,			// typing goes to the search box of whatever is showing
	EM_F_SEARCH,
	EM_F_PITCH,
	EM_F_YAW,
	EM_F_ROLL,
	EM_F_BOLT,
	EM_F_ACTIVE_SEARCH,
	EM_F_INTENSITY,
	EM_F_DURATION,
	EM_F_PICKER_SEARCH,
	EM_NUM_FIELDS
} emField_t;

typedef enum {
	EM_CONFIRM_NONE,
	EM_CONFIRM_REMOVE,
	EM_CONFIRM_REMOVE_ALL
} emConfirm_t;

typedef struct emEffect_s {
	const char	*path;		// relative to EM_ROOT, no extension, e.g. "scepter/invincibility"
	const char	*base;		// file name part of path
	int			folder;
	int			id;			// in the effects system, 0 = not loaded yet
	qboolean	failed;		// the effects system couldn't load it
} emEffect_t;

typedef struct emFolder_s {
	char		name[MAX_QPATH];	// relative to EM_ROOT, "" for the root itself
	int			first, count;		// range in the sorted effect list
} emFolder_t;

typedef struct emActive_s {
	int			num;
	vec3_t		origin;
	char		path[MAX_QPATH];
	qboolean	checked;
} emActive_t;

typedef struct emList_s {
	int			sel, scroll;
	int			lastClickTime, lastClickRow;
} emList_t;

typedef struct emCaptured_s {
	CGhoul2Info_v	*ghoul2;
	vec3_t			origin;
	vec3_t			scale;
} emCaptured_t;

typedef struct emBolt_s {
	char		name[MAX_QPATH];
	qboolean	tag;		// a *tag surface, not a bone
} emBolt_t;

typedef struct emSnap_s {
	int			angle;		// degrees per rotation step
	int			grid;		// units per nudge / follow snap
} emSnap_t;

static const emSnap_t emSnaps[] = {
	{ 1, 1 },
	{ 5, 2 },
	{ 15, 4 },
	{ 45, 8 },
	{ 90, 16 },
};

static struct {
	emState_t	state;
	emTab_t		tab;
	emPicker_t	picker;
	int			font;

	// input; clicks and the wheel are handled where the control under them is drawn
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;
	int			wheel;
	emField_t	focus;
	char		fields[EM_NUM_FIELDS][EM_FIELD_LEN];
	qboolean	held[MAX_KEYS];

	// index, rebuilt the first time the manager opens after cgame (re)starts
	qboolean	indexed;
	int			numEffects;
	emEffect_t	effects[EM_MAX_EFFECTS];
	int			numFolders;
	emFolder_t	folders[EM_MAX_FOLDERS];
	char		pool[EM_NAME_POOL];
	int			poolUsed;
	int			hash[EM_HASH_SIZE];	// effect index + 1, 0 = empty

	// effects tab
	int			folder;
	int			view[EM_MAX_EFFECTS];	// effect indices currently listed
	int			numView;
	emList_t	folderList, effectList;
	int			effect;					// selected effect, -1 = none
	int			selTime;				// when it was selected
	qboolean	loop;					// "add" rather than "once"
	emWhere_t	where;
	qboolean	havePos;
	vec3_t		pos;
	float		previewDist;			// how far into the preview window the effect plays

	// bolting
	emTarget_t	target;
	int			targetClient;
	int			targetNpc;				// entity number
	char		targetName[MAX_QPATH];	// NPC targetname
	qboolean	targetSeen;				// its model was in the last frame
	CGhoul2Info_v *g2;					// our copy of the target's model, bolted effects hold on to it
	CGhoul2Info_v *g2View;				// another, for the model view: looking at bolts adds them, and
										// effects can only take the first EM_MAX_BOLT_INDEX of them
	char		g2Key[MAX_QPATH * 2];	// what it was copied from
	int			g2Ent;
	vec3_t		g2Scale;
	char		g2Model[MAX_QPATH];		// model/skin, for showing
	vec3_t		g2Mins, g2Maxs;			// around its skeleton
	emBolt_t	bolts[EM_MAX_BOLTS];
	int			numBolts;
	float		modelYaw;				// added by the wheel in the model view
	CGhoul2Info_v *retired[EM_MAX_RETIRED];
	int			retiredTime[EM_MAX_RETIRED];

	// what the cgame drew last frame, for finding the target's model
	emCaptured_t pending[EM_MAX_CAPTURED];
	int			numPending;

	// preview playback
	char		playKey[MAX_QPATH * 3];
	int			nextPlay;
	const char	*playError;

	// active tab
	emActive_t	active[EM_MAX_ACTIVE];
	int			numActive;
	int			activeView[EM_MAX_ACTIVE];
	int			numActiveView;
	emList_t	activeList;
	emConfirm_t	confirm;
	int			confirmTime;
	int			listRequestTime;	// asked for it, waiting for the header, 0 = not waiting
	qboolean	listOurs;			// the list being read was asked for by the manager
	qboolean	listQueued;			// a request is waiting in the queue
	int			listLineTime;		// last list line read, 0 = not reading a list
	int			listTime;			// when a list was last read, 0 = never
	int			refreshTime;		// read the list again at this time, 0 = no

	// picker
	int			pickerView[MAX_CLIENTS > EM_MAX_BOLTS ? MAX_CLIENTS : EM_MAX_BOLTS];
	int			numPickerView;
	emList_t	pickerList;

	// placing
	vec3_t		camOrg, camAng;
	vec3_t		origin;				// where the effect goes
	qboolean	locked;				// qfalse = it follows the crosshair
	qboolean	align;				// face away from the surface under the crosshair
	qboolean	hideHelp;
	int			snap;
	int			lastFrameTime;
	int			viewFrame;			// cls.framecount the main view was last re-aimed on

	// the main view last frame: the cgame's, or the free camera's
	qboolean	haveView;
	vec3_t		viewOrg;
	matrix3_t	viewAxis;
	float		fovX, fovY;

	// commands, spaced out for the server's flood protection
	char		queue[EM_MAX_QUEUE][MAX_STRING_CHARS];
	qboolean	queueEcho[EM_MAX_QUEUE];
	int			numQueue;
	int			nextSend;
	char		lastCmd[MAX_STRING_CHARS];
	int			sentTime;
	char		reply[MAX_STRING_CHARS];
	int			replyTime;
} em;

static vec4_t emWhite		= { 1.0f, 1.0f, 1.0f, 1.0f };
static vec4_t emRed			= { 1.0f, 0.3f, 0.3f, 1.0f };
static vec4_t emPanel		= { 0.0f, 0.0f, 0.0f, 0.65f };
static vec4_t emPanelLight	= { 0.15f, 0.15f, 0.15f, 0.8f };
static vec4_t emPanelFocus	= { 0.1f, 0.2f, 0.3f, 0.9f };
static vec4_t emHighlight	= { 0.2f, 0.45f, 0.8f, 0.8f };
static vec4_t emHover		= { 1.0f, 1.0f, 1.0f, 0.12f };
static vec4_t emHoverBox	= { 0.25f, 0.25f, 0.25f, 0.9f };
static vec4_t emDanger		= { 0.6f, 0.15f, 0.15f, 0.9f };
static vec4_t emBorder		= { 0.5f, 0.5f, 0.5f, 0.8f };
static vec4_t emDim			= { 0.7f, 0.7f, 0.7f, 1.0f };
static vec4_t emAccent		= { 1.0f, 0.8f, 0.3f, 1.0f };

static void EM_RequestList( void );
static void EM_FieldChanged( int f );

/*
===============================================================================

INDEX

===============================================================================
*/

static int EM_HashName( const char *s ) {
	unsigned int h = 5381;

	while ( *s ) {
		h = h * 33 + (unsigned int)tolower( (unsigned char)*s );
		s++;
	}
	return (int)( h % EM_HASH_SIZE );
}

static const char *EM_PoolString( const char *s ) {
	int len = strlen( s ) + 1;
	char *out;

	if ( em.poolUsed + len > EM_NAME_POOL )
		return NULL;
	out = em.pool + em.poolUsed;
	memcpy( out, s, len );
	em.poolUsed += len;
	return out;
}

// name is a full game path, e.g. "effects/scepter/invincibility.efx"
static void EM_AddEffect( const char *name, void *ctx ) {
	char path[MAX_QPATH];
	const int rootLen = strlen( EM_ROOT ), extLen = strlen( EM_EXT );
	const char *stored, *slash;
	int h, len;

	len = strlen( name );
	if ( len <= rootLen + 1 + extLen || Q_stricmpn( name, EM_ROOT "/", rootLen + 1 ) )
		return;
	Q_strncpyz( path, name + rootLen + 1, sizeof( path ) );
	path[strlen( path ) - extLen] = '\0';
	for ( char *c = path; *c; c++ ) {
		if ( *c == '\\' )
			*c = '/';
	}

	// the same file can be in several pk3s and folders
	h = EM_HashName( path );
	while ( em.hash[h] ) {
		if ( !Q_stricmp( em.effects[em.hash[h] - 1].path, path ) )
			return;
		h = ( h + 1 ) % EM_HASH_SIZE;
	}

	if ( em.numEffects >= EM_MAX_EFFECTS )
		return;
	stored = EM_PoolString( path );
	if ( !stored )
		return;

	slash = strrchr( stored, '/' );
	memset( &em.effects[em.numEffects], 0, sizeof( em.effects[0] ) );
	em.effects[em.numEffects].path = stored;
	em.effects[em.numEffects].base = slash ? slash + 1 : stored;
	em.hash[h] = ++em.numEffects;
}

static int EM_FolderLen( const emEffect_t *e ) {
	return (int)( e->base - e->path ) - ( e->base != e->path ? 1 : 0 );
}

static int QDECL EM_CompareEffects( const void *a, const void *b ) {
	const emEffect_t *ea = (const emEffect_t *)a, *eb = (const emEffect_t *)b;
	int la = EM_FolderLen( ea ), lb = EM_FolderLen( eb );
	int cmp = Q_stricmpn( ea->path, eb->path, la < lb ? la : lb );

	// group by folder first so every folder is one contiguous range
	if ( cmp )
		return cmp;
	if ( la != lb )
		return la - lb;
	return Q_stricmp( ea->base, eb->base );
}

static void EM_BuildIndex( void ) {
	int start = Sys_Milliseconds();

	em.numEffects = 0;
	em.numFolders = 0;
	em.poolUsed = 0;
	memset( em.hash, 0, sizeof( em.hash ) );

	FS_ListFilesRecursive( EM_ROOT, EM_EXT, EM_AddEffect, NULL );

	qsort( em.effects, em.numEffects, sizeof( em.effects[0] ), EM_CompareEffects );

	for ( int i = 0; i < em.numEffects; i++ ) {
		emEffect_t *e = &em.effects[i];
		int len = EM_FolderLen( e );
		emFolder_t *f = em.numFolders ? &em.folders[em.numFolders - 1] : NULL;

		if ( !f || (int)strlen( f->name ) != len || Q_stricmpn( f->name, e->path, len ) ) {
			if ( em.numFolders >= EM_MAX_FOLDERS ) {
				em.numEffects = i;
				break;
			}
			f = &em.folders[em.numFolders++];
			Q_strncpyz( f->name, e->path, len + 1 < (int)sizeof( f->name ) ? len + 1 : (int)sizeof( f->name ) );
			f->first = i;
			f->count = 0;
		}
		f->count++;
		e->folder = em.numFolders - 1;
	}

	em.indexed = qtrue;
	em.effect = -1;
	em.folder = 0;
	memset( &em.folderList, 0, sizeof( em.folderList ) );
	memset( &em.effectList, 0, sizeof( em.effectList ) );
	Com_Printf( "Effect manager: indexed %i effects in %i folders (%i ms)\n", em.numEffects, em.numFolders, Sys_Milliseconds() - start );
}

/*
===============================================================================

HELPERS

===============================================================================
*/

static qboolean EM_ContainsNoCase( const char *haystack, const char *needle ) {
	int nlen = strlen( needle );

	if ( !nlen )
		return qtrue;
	for ( ; *haystack; haystack++ ) {
		if ( !Q_stricmpn( haystack, needle, nlen ) )
			return qtrue;
	}
	return qfalse;
}

static const char *EM_PlayerName( int clientNum ) {
	static char name[MAX_QPATH];
	const char *cs;

	if ( clientNum < 0 || clientNum >= MAX_CLIENTS )
		return "";
	cs = cl.gameState.stringData + cl.gameState.stringOffsets[CS_PLAYERS + clientNum];
	if ( !cs[0] )
		return "";
	Q_strncpyz( name, Info_ValueForKey( cs, "n" ), sizeof( name ) );
	Q_StripColor( name );
	return name;
}

static int EM_NormalizeAngle( float a ) {
	int i = (int)floorf( a + 0.5f ) % 360;
	return i < 0 ? i + 360 : i;
}

static float EM_SnapTo( float v, int grid ) {
	return grid > 1 ? floorf( v / grid + 0.5f ) * grid : floorf( v + 0.5f );
}

// a selection of -1 (none) stays none
static void EM_ListClamp( emList_t *list, int count, int visible ) {
	if ( list->sel >= 0 )
		list->sel = Com_Clampi( 0, count ? count - 1 : 0, list->sel );
	list->scroll = Com_Clampi( 0, count > visible ? count - visible : 0, list->scroll );
}

static void EM_ListMove( emList_t *list, int count, int visible, int delta ) {
	if ( !count )
		return;
	list->sel = Com_Clampi( 0, count - 1, list->sel + delta );
	if ( list->sel < list->scroll )
		list->scroll = list->sel;
	else if ( list->sel >= list->scroll + visible )
		list->scroll = list->sel - visible + 1;
}

static const emEffect_t *EM_Effect( void ) {
	return em.effect >= 0 && em.effect < em.numEffects ? &em.effects[em.effect] : NULL;
}

// the angle boxes; empty boxes are 0
static void EM_GetAngles( vec3_t angles ) {
	angles[PITCH] = (float)atof( em.fields[EM_F_PITCH] );
	angles[YAW] = (float)atof( em.fields[EM_F_YAW] );
	angles[ROLL] = (float)atof( em.fields[EM_F_ROLL] );
}

static void EM_SetAngles( const vec3_t angles ) {
	Com_sprintf( em.fields[EM_F_PITCH], EM_FIELD_LEN, "%i", EM_NormalizeAngle( angles[PITCH] ) );
	Com_sprintf( em.fields[EM_F_YAW], EM_FIELD_LEN, "%i", EM_NormalizeAngle( angles[YAW] ) );
	Com_sprintf( em.fields[EM_F_ROLL], EM_FIELD_LEN, "%i", EM_NormalizeAngle( angles[ROLL] ) );
}

static qboolean EM_HaveAngles( void ) {
	vec3_t angles;

	EM_GetAngles( angles );
	return (qboolean)( EM_NormalizeAngle( angles[PITCH] ) || EM_NormalizeAngle( angles[YAW] ) || EM_NormalizeAngle( angles[ROLL] ) );
}

// as RPMod plays it: no angles is straight up
static void EM_Direction( vec3_t dir ) {
	vec3_t angles;

	if ( !EM_HaveAngles() ) {
		VectorSet( dir, 0, 0, 1 );
		return;
	}
	EM_GetAngles( angles );
	AngleVectors( angles, dir, NULL, NULL );
}

static int EM_TargetEntity( void ) {
	switch ( em.target ) {
	case EM_TARGET_PLAYER:	return em.targetClient;
	case EM_TARGET_NPC:		return em.targetNpc;
	default:				return clc.clientNum;
	}
}

static const char *EM_TargetLabel( void ) {
	switch ( em.target ) {
	case EM_TARGET_PLAYER:	return va( "%s " S_COLOR_GREY "(player %i)", EM_PlayerName( em.targetClient ), em.targetClient );
	case EM_TARGET_NPC:		return va( "%s " S_COLOR_GREY "(NPC %i)", em.targetName, em.targetNpc );
	default:				return va( "%s " S_COLOR_GREY "(you)", EM_PlayerName( clc.clientNum ) );
	}
}

// where cgame has the entity this frame
static void EM_LerpData( int entNum, vec3_t origin, vec3_t angles ) {
	TCGGetBoltData *data = (TCGGetBoltData *)cl.mSharedMemory;

	data->mEntityNum = entNum;
	CGVM_GetLerpData();
	VectorCopy( data->mOrigin, origin );
	if ( angles )
		VectorCopy( data->mAngles, angles );
}

// "1000" as sv_floodProtect gives it, how long to wait between commands
static int EM_CommandGap( void ) {
	const char *info = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];
	const char *flood = Info_ValueForKey( info, "sv_floodProtect" );
	int value;

	if ( !flood[0] )
		return 1100;	// not told, play safe
	value = atoi( flood );
	if ( value <= 0 )
		return 0;
	return ( value == 1 ? 1000 : value ) + 100;
}

/*
===============================================================================

VIEWS

===============================================================================
*/

static void EM_RebuildEffectView( void ) {
	const char *search = em.fields[EM_F_SEARCH];

	em.numView = 0;
	if ( search[0] ) {
		for ( int i = 0; i < em.numEffects; i++ ) {
			if ( EM_ContainsNoCase( em.effects[i].path, search ) )
				em.view[em.numView++] = i;
		}
	} else if ( em.numFolders ) {
		const emFolder_t *f = &em.folders[Com_Clampi( 0, em.numFolders - 1, em.folder )];

		for ( int i = 0; i < f->count; i++ )
			em.view[em.numView++] = f->first + i;
	}

	// keep the selected effect selected if it is still listed
	em.effectList.sel = -1;
	for ( int i = 0; i < em.numView; i++ ) {
		if ( em.view[i] == em.effect ) {
			em.effectList.sel = i;
			break;
		}
	}
	EM_ListClamp( &em.effectList, em.numView, (int)( EM_LIST_H / EM_ROW_H ) );
}

static void EM_SetFolder( int folder ) {
	if ( !em.numFolders )
		return;
	em.folder = Com_Clampi( 0, em.numFolders - 1, folder );
	em.folderList.sel = em.folder;
	em.fields[EM_F_SEARCH][0] = '\0';
	em.effectList.sel = em.effectList.scroll = 0;
	EM_ListMove( &em.folderList, em.numFolders, (int)( EM_LIST_H / EM_ROW_H ), 0 );
	EM_RebuildEffectView();
}

static void EM_SelectEffect( int index ) {
	for ( int i = 0; i < em.numView; i++ ) {
		if ( em.view[i] == index ) {
			EM_ListMove( &em.effectList, em.numView, (int)( EM_LIST_H / EM_ROW_H ), i - em.effectList.sel );
			break;
		}
	}
	if ( index == em.effect )
		return;
	em.effect = index;
	em.selTime = cls.realtime;
}

static void EM_RebuildActiveView( void ) {
	const char *search = em.fields[EM_F_ACTIVE_SEARCH];

	em.numActiveView = 0;
	for ( int i = 0; i < em.numActive; i++ ) {
		const emActive_t *a = &em.active[i];

		if ( search[0] && !EM_ContainsNoCase( a->path, search ) && !EM_ContainsNoCase( va( "%i", a->num ), search ) )
			continue;
		em.activeView[em.numActiveView++] = i;
	}
	EM_ListClamp( &em.activeList, em.numActiveView, (int)( EM_LIST_H / EM_ROW_H ) );
}

static int EM_CountChecked( void ) {
	int count = 0;

	for ( int i = 0; i < em.numActive; i++ ) {
		if ( em.active[i].checked )
			count++;
	}
	return count;
}

static int EM_PickerCount( void ) {
	switch ( em.picker ) {
	case EM_PICKER_PLAYER:
	case EM_PICKER_REMOVE_PLAYER:	return MAX_CLIENTS;
	case EM_PICKER_NPC:
	case EM_PICKER_REMOVE_NPC:		return CL_NpcManager_NumNpcs();
	case EM_PICKER_BOLT:			return em.numBolts;
	default:						return 0;
	}
}

// what the row shows and is searched by
static const char *EM_PickerItem( int index ) {
	int num;
	const char *type, *name;

	switch ( em.picker ) {
	case EM_PICKER_PLAYER:
	case EM_PICKER_REMOVE_PLAYER:
		return EM_PlayerName( index );
	case EM_PICKER_NPC:
	case EM_PICKER_REMOVE_NPC:
		if ( !CL_NpcManager_GetNpc( index, &num, &type, &name ) )
			return "";
		return va( "%i %s %s", num, name, type );
	case EM_PICKER_BOLT:
		return em.bolts[index].name;
	default:
		return "";
	}
}

static void EM_RebuildPickerView( void ) {
	const char *search = em.fields[EM_F_PICKER_SEARCH];
	int count = EM_PickerCount();

	em.numPickerView = 0;
	for ( int i = 0; i < count && em.numPickerView < (int)ARRAY_LEN( em.pickerView ); i++ ) {
		const char *item = EM_PickerItem( i );

		if ( item[0] && EM_ContainsNoCase( item, search ) )
			em.pickerView[em.numPickerView++] = i;
	}
	EM_ListClamp( &em.pickerList, em.numPickerView, (int)( ( EM_PICKER_H - 74 ) / EM_ROW_H ) );
}

/*
===============================================================================

THE TARGET'S MODEL

===============================================================================
*/

static void EM_AddBolt( const char *name, qboolean tag ) {
	if ( em.numBolts >= EM_MAX_BOLTS || !name[0] )
		return;
	for ( int i = 0; i < em.numBolts; i++ ) {
		if ( !Q_stricmp( em.bolts[i].name, name ) )
			return;
	}
	Q_strncpyz( em.bolts[em.numBolts].name, name, sizeof( em.bolts[0].name ) );
	em.bolts[em.numBolts].tag = tag;
	em.numBolts++;
}

// the *tag surfaces of a .glm
static void EM_ReadTags( const char *path ) {
	const mdxmHeader_t *header;
	const mdxmHierarchyOffsets_t *offsets;
	byte *buf;
	int len, numSurfaces;

	len = FS_ReadFile( path, (void **)&buf );
	if ( len <= 0 || !buf )
		return;
	header = (const mdxmHeader_t *)buf;
	numSurfaces = len >= (int)sizeof( *header ) ? LittleLong( header->numSurfaces ) : 0;
	if ( LittleLong( header->ident ) == MDXM_IDENT && numSurfaces > 0 && numSurfaces < len / 4
		&& (int)sizeof( *header ) + numSurfaces * 4 <= len ) {
		offsets = (const mdxmHierarchyOffsets_t *)( buf + sizeof( *header ) );
		for ( int i = 0; i < numSurfaces; i++ ) {
			int ofs = (int)sizeof( *header ) + LittleLong( offsets->offsets[i] );
			const mdxmSurfHierarchy_t *surf;

			if ( ofs < 0 || ofs > len - (int)sizeof( mdxmSurfHierarchy_t ) )
				break;
			surf = (const mdxmSurfHierarchy_t *)( buf + ofs );
			if ( surf->name[0] == '*' )
				EM_AddBolt( va( "%.*s", MAX_QPATH - 1, surf->name ), qtrue );
		}
	}
	FS_FreeFile( buf );
}

// the bones of a .gla
static void EM_ReadBones( const char *path ) {
	const mdxaHeader_t *header;
	const mdxaSkelOffsets_t *offsets;
	byte *buf;
	int len, numBones;

	len = FS_ReadFile( path, (void **)&buf );
	if ( len <= 0 || !buf )
		return;
	header = (const mdxaHeader_t *)buf;
	numBones = len >= (int)sizeof( *header ) ? LittleLong( header->numBones ) : 0;
	if ( LittleLong( header->ident ) == MDXA_IDENT && numBones > 0 && numBones < len / 4
		&& (int)sizeof( *header ) + numBones * 4 <= len ) {
		offsets = (const mdxaSkelOffsets_t *)( buf + sizeof( *header ) );
		for ( int i = 0; i < numBones; i++ ) {
			int ofs = (int)sizeof( *header ) + LittleLong( offsets->offsets[i] );
			const mdxaSkel_t *bone;

			if ( ofs < 0 || ofs > len - (int)sizeof( mdxaSkel_t ) )
				break;
			bone = (const mdxaSkel_t *)( buf + ofs );
			EM_AddBolt( va( "%.*s", MAX_QPATH - 1, bone->name ), qfalse );
		}
	}
	FS_FreeFile( buf );
}

static int QDECL EM_CompareBolts( const void *a, const void *b ) {
	const emBolt_t *ba = (const emBolt_t *)a, *bb = (const emBolt_t *)b;

	// tags first, the way RPMod's help lists them
	if ( ba->tag != bb->tag )
		return ba->tag ? -1 : 1;
	return ba->tag ? Q_stricmp( ba->name, bb->name ) : 0;
}

// the point a bolt is at, with the model at the origin facing yaw
static qboolean EM_BoltPointOn( CGhoul2Info_v &ghoul2, const char *name, float yaw, vec3_t out ) {
	mdxaBone_t matrix;
	vec3_t angles;
	int bolt;

	if ( !name[0] )
		return qfalse;
	bolt = re->G2API_AddBolt( ghoul2, 0, name );
	if ( bolt < 0 )
		return qfalse;
	VectorSet( angles, 0, yaw, 0 );
	if ( !re->G2API_GetBoltMatrix( ghoul2, 0, bolt, &matrix, angles, vec3_origin, cl.serverTime, NULL, em.g2Scale ) )
		return qfalse;
	VectorSet( out, matrix.matrix[0][3], matrix.matrix[1][3], matrix.matrix[2][3] );
	return qtrue;
}

static qboolean EM_BoltPoint( const char *name, float yaw, vec3_t out ) {
	return (qboolean)( em.g2View && EM_BoltPointOn( *em.g2View, name, yaw, out ) );
}

// the bolt box names something on the model
static qboolean EM_BoltExists( void ) {
	return (qboolean)( em.g2View && em.fields[EM_F_BOLT][0] && re->G2API_AddBolt( *em.g2View, 0, em.fields[EM_F_BOLT] ) >= 0 );
}

// the tags and bones it can be bolted to, and how big it is
static void EM_LoadBolts( void ) {
	char gla[MAX_QPATH];
	const char *glm, *anim;
	int numTags = 0;
	vec3_t p;

	em.numBolts = 0;
	if ( !em.g2 )
		return;
	glm = re->G2API_GetModelName( *em.g2, 0 );
	if ( glm && glm[0] )
		EM_ReadTags( glm );
	anim = re->G2API_GetGLAName( *em.g2, 0 );
	if ( anim && anim[0] ) {
		Q_strncpyz( gla, anim, sizeof( gla ) );
		if ( !strchr( gla, '.' ) )
			Q_strcat( gla, sizeof( gla ), ".gla" );
		EM_ReadBones( gla );
	}
	for ( int i = 0; i < em.numBolts; i++ ) {
		if ( em.bolts[i].tag )
			numTags++;
	}
	qsort( em.bolts, em.numBolts, sizeof( em.bolts[0] ), EM_CompareBolts );

	// frame the model view around its bones
	ClearBounds( em.g2Mins, em.g2Maxs );
	for ( int i = numTags; i < em.numBolts; i++ ) {
		if ( EM_BoltPoint( em.bolts[i].name, 0, p ) )
			AddPointToBounds( p, em.g2Mins, em.g2Maxs );
	}
	if ( em.g2Mins[0] > em.g2Maxs[0] ) {
		VectorSet( em.g2Mins, -16, -16, -24 );
		VectorSet( em.g2Maxs, 16, 16, 40 );
	}
	for ( int i = 0; i < 3; i++ ) {
		em.g2Mins[i] -= 8.0f;
		em.g2Maxs[i] += 8.0f;
	}
}

// "kyle/default" out of the model and skin file names
static void EM_DescribeModel( CGhoul2Info_v &ghoul2, char *out, int size ) {
	char model[MAX_QPATH], skin[MAX_QPATH];
	const char *glm = re->G2API_GetModelName( ghoul2, 0 ), *skinName = NULL, *p;
	qhandle_t customSkin = 0, ownSkin = 0;

	Q_strncpyz( model, glm ? glm : "", sizeof( model ) );
	if ( !Q_stricmpn( model, "models/players/", 15 ) ) {
		memmove( model, model + 15, strlen( model + 15 ) + 1 );
		if ( ( p = strchr( model, '/' ) ) != NULL )
			model[p - model] = '\0';
	}

	if ( re->ext.G2API_GetSkins )
		re->ext.G2API_GetSkins( ghoul2, 0, &customSkin, &ownSkin );
	skinName = CL_ShaderManager_SkinName( customSkin ? customSkin : ownSkin );
	skin[0] = '\0';
	if ( skinName ) {
		p = strrchr( skinName, '/' );
		Q_strncpyz( skin, p ? p + 1 : skinName, sizeof( skin ) );
		if ( !Q_stricmpn( skin, "model_", 6 ) )
			memmove( skin, skin + 6, strlen( skin + 6 ) + 1 );
		COM_StripExtension( skin, skin, sizeof( skin ) );
		if ( skin[0] == '|' )
			memmove( skin, skin + 1, strlen( skin + 1 ) + 1 );
	}
	Com_sprintf( out, size, "%s%s%s", model, skin[0] ? "/" : "", skin );
}

static void EM_RetireModel( void ) {
	int slot = 0;

	if ( !em.g2 )
		return;
	// effects already bolted to it keep using it for a while
	for ( int i = 0; i < EM_MAX_RETIRED; i++ ) {
		if ( !em.retired[i] ) {
			slot = i;
			break;
		}
		if ( em.retiredTime[i] < em.retiredTime[slot] )
			slot = i;
	}
	if ( em.retired[slot] )
		re->G2API_CleanGhoul2Models( &em.retired[slot] );
	em.retired[slot] = em.g2;
	em.retiredTime[slot] = cls.realtime;
	em.g2 = NULL;
	em.g2Key[0] = '\0';
	em.numBolts = 0;
	// no effect uses the view's copy
	if ( em.g2View )
		re->G2API_CleanGhoul2Models( &em.g2View );
	em.g2View = NULL;
}

static void EM_FreeModels( void ) {
	if ( em.g2 )
		re->G2API_CleanGhoul2Models( &em.g2 );
	em.g2 = NULL;
	if ( em.g2View )
		re->G2API_CleanGhoul2Models( &em.g2View );
	em.g2View = NULL;
	em.g2Key[0] = '\0';
	em.numBolts = 0;
	for ( int i = 0; i < EM_MAX_RETIRED; i++ ) {
		if ( em.retired[i] )
			re->G2API_CleanGhoul2Models( &em.retired[i] );
		em.retired[i] = NULL;
	}
}

static qboolean EM_WantTarget( void ) {
	if ( em.state == EM_OFF || em.tab != EM_TAB_EFFECTS )
		return qfalse;
	return (qboolean)( em.where == EM_WHERE_BOLT || em.picker == EM_PICKER_BOLT );
}

// finds the target among the models cgame drew this frame, and copies it when it changed
static void EM_UpdateTarget( void ) {
	const emCaptured_t *best = NULL;
	float bestDist = EM_TARGET_XY * EM_TARGET_XY;
	vec3_t org;
	char key[sizeof( em.g2Key )];
	qhandle_t customSkin = 0, ownSkin = 0;
	const char *glm;
	int ent;

	if ( !EM_WantTarget() || !cls.cgameStarted )
		return;
	ent = EM_TargetEntity();
	EM_LerpData( ent, org, NULL );

	for ( int i = 0; i < em.numPending; i++ ) {
		const emCaptured_t *c = &em.pending[i];
		float dx = c->origin[0] - org[0], dy = c->origin[1] - org[1];

		if ( fabsf( c->origin[2] - org[2] ) > EM_TARGET_Z || dx * dx + dy * dy >= bestDist )
			continue;
		bestDist = dx * dx + dy * dy;
		best = c;
	}
	em.targetSeen = (qboolean)( best != NULL );
	if ( !best ) {
		// don't bolt to the last target's copy
		if ( em.g2 && em.g2Ent != ent )
			EM_RetireModel();
		return;
	}

	glm = re->G2API_GetModelName( *best->ghoul2, 0 );
	if ( re->ext.G2API_GetSkins )
		re->ext.G2API_GetSkins( *best->ghoul2, 0, &customSkin, &ownSkin );
	Com_sprintf( key, sizeof( key ), "%i|%s|%i|%i", ent, glm ? glm : "", customSkin, ownSkin );
	if ( em.g2 && !strcmp( key, em.g2Key ) )
		return;

	EM_RetireModel();
	re->G2API_DuplicateGhoul2Instance( *best->ghoul2, &em.g2 );
	re->G2API_DuplicateGhoul2Instance( *best->ghoul2, &em.g2View );
	if ( !em.g2 || !em.g2View ) {
		EM_FreeModels();
		return;
	}
	Q_strncpyz( em.g2Key, key, sizeof( em.g2Key ) );
	em.g2Ent = ent;
	VectorCopy( best->scale, em.g2Scale );
	EM_DescribeModel( *em.g2, em.g2Model, sizeof( em.g2Model ) );
	EM_LoadBolts();
}

/*
===============================================================================

PREVIEW

===============================================================================
*/

// loads the effect once the selection rests on it; 0 if it can't be played (yet)
static int EM_EffectId( emEffect_t *e ) {
	if ( !e || e->failed )
		return 0;
	if ( !e->id ) {
		if ( em.state == EM_BROWSE && cls.realtime - em.selTime < EM_SETTLE_MS )
			return 0;
		e->id = FX_RegisterEffect( va( "%s/%s", EM_ROOT, e->path ) );
		if ( !e->id )
			e->failed = qtrue;
	}
	return e->id;
}

static int EM_PlayPeriod( int id ) {
	int length = 0, repeatDelay = 0;

	if ( !theFxScheduler.GetEffectTiming( id, &length, &repeatDelay ) )
		return 1000;
	if ( !em.loop )
		length += EM_ONCE_GAP;
	return length < EM_MIN_PERIOD ? EM_MIN_PERIOD : length;
}

// the ray through a point of the 2D screen, from the main view last frame
static qboolean EM_ScreenRay( float sx, float sy, vec3_t dir ) {
	float x, y;

	if ( !em.haveView )
		return qfalse;
	x = ( sx / ( SCREEN_WIDTH * 0.5f ) - 1.0f ) * tanf( DEG2RAD( em.fovX * 0.5f ) );
	y = ( sy / ( SCREEN_HEIGHT * 0.5f ) - 1.0f ) * tanf( DEG2RAD( em.fovY * 0.5f ) );
	VectorCopy( em.viewAxis[0], dir );
	VectorMA( dir, -x, em.viewAxis[1], dir );
	VectorMA( dir, -y, em.viewAxis[2], dir );
	VectorNormalize( dir );
	return qtrue;
}

static qboolean EM_Project( const vec3_t point, float *x, float *y ) {
	vec3_t d;
	float forward;

	if ( !em.haveView )
		return qfalse;
	VectorSubtract( point, em.viewOrg, d );
	forward = DotProduct( d, em.viewAxis[0] );
	if ( forward < 4.0f )
		return qfalse;
	*x = SCREEN_WIDTH * 0.5f * ( 1.0f - DotProduct( d, em.viewAxis[1] ) / forward / tanf( DEG2RAD( em.fovX * 0.5f ) ) );
	*y = SCREEN_HEIGHT * 0.5f * ( 1.0f - DotProduct( d, em.viewAxis[2] ) / forward / tanf( DEG2RAD( em.fovY * 0.5f ) ) );
	return qtrue;
}

// the middle of the preview window, out in the world, short of any wall
static qboolean EM_WindowPoint( vec3_t out ) {
	vec3_t dir, end;
	trace_t tr;

	if ( !EM_ScreenRay( EM_RIGHT_X + EM_RIGHT_W * 0.5f, EM_VIEW_Y + EM_VIEW_H * 0.5f, dir ) )
		return qfalse;
	VectorMA( em.viewOrg, em.previewDist, dir, end );
	CM_BoxTrace( &tr, em.viewOrg, end, vec3_origin, vec3_origin, 0, EM_TRACE_MASK, qfalse );
	VectorMA( em.viewOrg, Q_max( 8.0f, em.previewDist * tr.fraction - 8.0f ), dir, out );
	return qtrue;
}

static qboolean EM_BoltExists( void );

static int EM_BoltIndex( void ) {
	if ( !em.g2 || !EM_BoltExists() )
		return -1;
	return re->G2API_AddBolt( *em.g2, 0, em.fields[EM_F_BOLT] );
}

// plays the selected effect where it would go, again and again, on this client only
static void EM_PreviewFrame( void ) {
	emEffect_t *e = em.effect >= 0 && em.effect < em.numEffects ? &em.effects[em.effect] : NULL;
	qboolean bolted = (qboolean)( em.where == EM_WHERE_BOLT );
	char key[sizeof( em.playKey )];
	vec3_t org, dir;
	int id, bolt = -1;

	em.playError = NULL;
	if ( !e || em.picker != EM_PICKER_NONE )
		return;
	if ( em.state == EM_BROWSE ) {
		// bolted effects are shown on the model in the window, and played in the world from "View in world"
		if ( em.tab != EM_TAB_EFFECTS || bolted )
			return;
	} else if ( em.state != EM_PLACE ) {
		return;
	}

	id = EM_EffectId( e );
	if ( !id ) {
		if ( e->failed )
			em.playError = "The effects system couldn't load it";
		return;
	}

	if ( bolted ) {
		bolt = EM_BoltIndex();
		if ( !em.g2 ) {
			em.playError = "Target not in view";
			return;
		}
		if ( bolt < 0 ) {
			em.playError = "Its model has no such bolt";
			return;
		}
		if ( bolt > EM_MAX_BOLT_INDEX ) {
			em.playError = "Too many bolts on this model to show it here";
			return;
		}
	}

	Com_sprintf( key, sizeof( key ), "%i|%i|%i|%i|%s|%s", em.effect, em.loop, bolted, em.state, bolted ? em.g2Key : "", bolted ? em.fields[EM_F_BOLT] : "" );
	if ( strcmp( key, em.playKey ) ) {
		Q_strncpyz( em.playKey, key, sizeof( em.playKey ) );
		em.nextPlay = cls.realtime;
	}
	if ( cls.realtime < em.nextPlay )
		return;
	em.nextPlay = cls.realtime + EM_PlayPeriod( id );

	if ( bolted ) {
		int boltInfo = 0;

		EM_LerpData( em.g2Ent, org, NULL );
		if ( re->G2API_AttachEnt( &boltInfo, *em.g2, 0, bolt, em.g2Ent, 0 ) )
			FX_PlayBoltedEffectID( id, org, boltInfo, em.g2, 0, qtrue );
		return;
	}

	if ( em.state == EM_PLACE )
		VectorCopy( em.origin, org );
	else if ( !EM_WindowPoint( org ) )
		return;
	EM_Direction( dir );
	FX_PlayEffectID( id, org, dir, -1, -1 );
}

/*
===============================================================================

COMMANDS

===============================================================================
*/

static void EM_Queue( const char *cmd, qboolean echo ) {
	if ( em.numQueue >= EM_MAX_QUEUE ) {
		Com_Printf( S_COLOR_YELLOW "Effect manager: too many commands waiting, dropped %s\n", cmd );
		return;
	}
	Q_strncpyz( em.queue[em.numQueue], cmd, sizeof( em.queue[0] ) );
	em.queueEcho[em.numQueue] = echo;
	em.numQueue++;
}

// one command at a time, as fast as the server's flood protection lets them through
static void EM_RunQueue( void ) {
	if ( !em.numQueue || cls.realtime < em.nextSend || cls.state != CA_ACTIVE )
		return;

	CL_AddReliableCommand( em.queue[0], qfalse );
	if ( em.queueEcho[0] ) {
		em.sentTime = cls.realtime;
		Q_strncpyz( em.lastCmd, em.queue[0], sizeof( em.lastCmd ) );
		Com_Printf( S_COLOR_GREEN "Effect manager: " S_COLOR_WHITE "%s\n", em.queue[0] );
	} else if ( !Q_stricmp( em.queue[0], "rpeffect list" ) ) {
		em.listRequestTime = cls.realtime;
		em.listQueued = qfalse;
	}
	em.numQueue--;
	memmove( em.queue[0], em.queue[1], em.numQueue * sizeof( em.queue[0] ) );
	memmove( &em.queueEcho[0], &em.queueEcho[1], em.numQueue * sizeof( em.queueEcho[0] ) );
	em.nextSend = cls.realtime + EM_CommandGap();
}

static void EM_RequestList( void ) {
	em.refreshTime = 0;
	if ( em.listQueued )
		return;
	em.listQueued = qtrue;
	EM_Queue( "rpeffect list", qfalse );
}

// the list again once the commands before it went through
static void EM_SendAndRefresh( const char *cmd ) {
	EM_Queue( cmd, qtrue );
	EM_RequestList();
}

// the command for the selected effect, NULL if something is missing
static const char *EM_BuildCommand( void ) {
	static char cmd[MAX_STRING_CHARS];
	const emEffect_t *e = EM_Effect();
	vec3_t angles;
	const float *pos = NULL;

	if ( !e )
		return NULL;
	Com_sprintf( cmd, sizeof( cmd ), "rpeffect %s %s", em.loop ? "add" : "once", e->path );

	if ( em.where == EM_WHERE_BOLT ) {
		if ( !em.fields[EM_F_BOLT][0] )
			return NULL;
		Q_strcat( cmd, sizeof( cmd ), va( " bolted %s", em.fields[EM_F_BOLT] ) );
		if ( em.target == EM_TARGET_PLAYER ) {
			Q_strcat( cmd, sizeof( cmd ), va( " %i", em.targetClient ) );
		} else if ( em.target == EM_TARGET_NPC ) {
			if ( !em.targetName[0] )
				return NULL;
			Q_strcat( cmd, sizeof( cmd ), va( " npc %s", em.targetName ) );
		}
		return cmd;
	}

	if ( em.state == EM_PLACE )
		pos = em.origin;
	else if ( em.where == EM_WHERE_POS && em.havePos )
		pos = em.pos;

	// positional: angles need the coordinates before them, which 0 0 0 leaves to the server
	if ( pos )
		Q_strcat( cmd, sizeof( cmd ), va( " %i %i %i", (int)floorf( pos[0] + 0.5f ), (int)floorf( pos[1] + 0.5f ), (int)floorf( pos[2] + 0.5f ) ) );
	if ( EM_HaveAngles() ) {
		EM_GetAngles( angles );
		if ( !pos )
			Q_strcat( cmd, sizeof( cmd ), " 0 0 0" );
		Q_strcat( cmd, sizeof( cmd ), va( " %i %i %i", EM_NormalizeAngle( angles[PITCH] ), EM_NormalizeAngle( angles[YAW] ), EM_NormalizeAngle( angles[ROLL] ) ) );
	}
	return cmd;
}

static void EM_Send( void ) {
	const char *cmd = EM_BuildCommand();

	if ( !cmd )
		return;
	if ( em.state == EM_PLACE && em.where != EM_WHERE_BOLT ) {
		// what was placed is the position from now on
		VectorCopy( em.origin, em.pos );
		em.havePos = qtrue;
		em.where = EM_WHERE_POS;
	}
	if ( em.loop )
		EM_SendAndRefresh( cmd );
	else
		EM_Queue( cmd, qtrue );
}

/*
===============================================================================

THE ACTIVE LIST

===============================================================================
*/

// "109: ( 2233 3289 -23) effects/scepter/invincibility", colors already stripped
static qboolean EM_ParseActiveLine( const char *line, const emActive_t *old, int numOld ) {
	emActive_t *a;
	float x, y, z;
	int num, used = 0;
	const char *rest;

	if ( sscanf( line, "%d: (%f %f %f)%n", &num, &x, &y, &z, &used ) != 4 || !used )
		return qfalse;
	if ( em.numActive >= EM_MAX_ACTIVE )
		return qtrue;

	a = &em.active[em.numActive];
	a->num = num;
	VectorSet( a->origin, x, y, z );
	rest = line + used;
	while ( *rest == ' ' )
		rest++;
	Q_strncpyz( a->path, rest, sizeof( a->path ) );
	a->checked = qfalse;
	for ( int i = 0; i < numOld; i++ ) {
		if ( old[i].num == num ) {
			a->checked = old[i].checked;
			break;
		}
	}
	em.numActive++;
	return qtrue;
}

// every server print goes through here on its way to the cgame; qtrue keeps it out of the console
qboolean CL_EffectManager_ServerPrint( const char *text ) {
	static emActive_t old[EM_MAX_ACTIVE];
	static int numOld;
	char buf[MAX_STRING_CHARS], *line, *next;
	qboolean swallow = qtrue, any = qfalse;

	if ( cls.state != CA_ACTIVE )
		return qfalse;

	Q_strncpyz( buf, text, sizeof( buf ) );
	for ( line = buf; line; line = next ) {
		next = strchr( line, '\n' );
		if ( next )
			*next++ = '\0';
		Q_StripColor( line );
		while ( *line == ' ' || *line == '\t' )
			line++;
		for ( int len = strlen( line ); len > 0 && ( line[len - 1] == ' ' || line[len - 1] == '\r' ); len-- )
			line[len - 1] = '\0';
		if ( !line[0] )
			continue;
		any = qtrue;

		if ( !Q_stricmpn( line, EM_LIST_HEADER, strlen( EM_LIST_HEADER ) ) ) {
			// also read a list someone typed /rpeffect list for, just don't hide it
			em.listOurs = (qboolean)( em.listRequestTime && cls.realtime - em.listRequestTime < EM_LIST_TIMEOUT );
			em.listRequestTime = 0;
			em.listLineTime = em.listTime = cls.realtime;
			// the checks stay on the effects that are still there
			memcpy( old, em.active, em.numActive * sizeof( em.active[0] ) );
			numOld = em.numActive;
			em.numActive = 0;
			if ( !em.listOurs )
				swallow = qfalse;
		} else if ( em.listLineTime && cls.realtime - em.listLineTime < EM_LIST_LINE_GAP && EM_ParseActiveLine( line, old, numOld ) ) {
			em.listLineTime = cls.realtime;
			if ( !em.listOurs )
				swallow = qfalse;
		} else {
			swallow = qfalse;
			if ( em.state != EM_OFF && cls.realtime - em.sentTime < EM_REPLY_MS ) {
				Q_strncpyz( em.reply, line, sizeof( em.reply ) );
				em.replyTime = cls.realtime;
			}
		}
	}

	if ( em.state != EM_OFF )
		EM_RebuildActiveView();
	return (qboolean)( any && swallow );
}

static void EM_RemoveChecked( void ) {
	int count = 0;

	for ( int i = 0; i < em.numActive; i++ ) {
		if ( em.active[i].checked ) {
			EM_Queue( va( "rpeffect remove %i", em.active[i].num ), qtrue );
			count++;
		}
	}
	if ( count )
		EM_RequestList();
}

/*
===============================================================================

STATE CHANGES

===============================================================================
*/

static void EM_ReleaseKeys( void ) {
	memset( em.held, 0, sizeof( em.held ) );
}

static void EM_SetTab( emTab_t tab ) {
	em.tab = tab;
	em.focus = EM_F_NONE;
	em.picker = EM_PICKER_NONE;
	em.confirm = EM_CONFIRM_NONE;
	if ( tab == EM_TAB_ACTIVE )
		EM_RequestList();
}

static void EM_OpenPicker( emPicker_t picker ) {
	em.picker = picker;
	em.fields[EM_F_PICKER_SEARCH][0] = '\0';
	em.pickerList.sel = em.pickerList.scroll = 0;
	em.modelYaw = 0.0f;
	if ( picker == EM_PICKER_NPC || picker == EM_PICKER_REMOVE_NPC )
		CL_NpcManager_RequestList();
	EM_RebuildPickerView();
	// start on the bolt already chosen
	if ( picker == EM_PICKER_BOLT ) {
		for ( int i = 0; i < em.numPickerView; i++ ) {
			if ( !Q_stricmp( em.bolts[em.pickerView[i]].name, em.fields[EM_F_BOLT] ) ) {
				EM_ListMove( &em.pickerList, em.numPickerView, (int)( ( EM_PICKER_H - 74 ) / EM_ROW_H ), i );
				break;
			}
		}
	}
}

static void EM_ChoosePicker( int index ) {
	int num;
	const char *type, *name;

	switch ( em.picker ) {
	case EM_PICKER_PLAYER:
		em.target = EM_TARGET_PLAYER;
		em.targetClient = index;
		break;
	case EM_PICKER_NPC:
		if ( !CL_NpcManager_GetNpc( index, &num, &type, &name ) || !name[0] )
			return;	// RPMod finds NPCs by targetname
		em.target = EM_TARGET_NPC;
		em.targetNpc = num;
		Q_strncpyz( em.targetName, name, sizeof( em.targetName ) );
		break;
	case EM_PICKER_BOLT:
		Q_strncpyz( em.fields[EM_F_BOLT], em.bolts[index].name, EM_FIELD_LEN );
		break;
	case EM_PICKER_REMOVE_PLAYER:
		EM_SendAndRefresh( va( "rpeffect remove bolted %i", index ) );
		break;
	case EM_PICKER_REMOVE_NPC:
		if ( !CL_NpcManager_GetNpc( index, &num, &type, &name ) || !name[0] )
			return;
		EM_SendAndRefresh( va( "rpeffect remove bolted npc %s", name ) );
		break;
	default:
		break;
	}
	em.picker = EM_PICKER_NONE;
}

static void EM_EnterBrowse( void ) {
	em.state = EM_BROWSE;
	em.focus = EM_F_NONE;
	EM_ReleaseKeys();
}

static void EM_EnterPlace( void ) {
	if ( !EM_Effect() )
		return;

	em.picker = EM_PICKER_NONE;
	em.focus = EM_F_NONE;
	if ( em.haveView ) {
		VectorCopy( em.viewOrg, em.camOrg );
		vectoangles( em.viewAxis[0], em.camAng );
	} else {
		VectorCopy( cl.snap.ps.origin, em.camOrg );
		em.camOrg[2] += cl.snap.ps.viewheight;
		VectorCopy( cl.viewangles, em.camAng );
	}

	if ( em.where == EM_WHERE_BOLT ) {
		// in front of the target, looking at it
		vec3_t org, angles, forward, dir;

		EM_LerpData( EM_TargetEntity(), org, angles );
		angles[PITCH] = angles[ROLL] = 0;
		AngleVectors( angles, forward, NULL, NULL );
		VectorMA( org, 110.0f, forward, em.camOrg );
		em.camOrg[2] += 20.0f;
		VectorSubtract( org, em.camOrg, dir );
		vectoangles( dir, em.camAng );
	} else if ( em.where == EM_WHERE_POS && em.havePos ) {
		VectorCopy( em.pos, em.origin );
		em.locked = qtrue;
	} else {
		em.locked = qfalse;
	}
	if ( em.camAng[PITCH] > 180.0f )
		em.camAng[PITCH] -= 360.0f;
	em.camAng[ROLL] = 0;
	em.state = EM_PLACE;
	em.lastFrameTime = cls.realtime;
	em.playKey[0] = '\0';
	EM_ReleaseKeys();
}

static void EM_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Effect manager: join a server first\n" );
		return;
	}
	CL_ModelManager_Close();
	CL_NpcManager_Close();
	CL_ShaderManager_Close();
	if ( !em.indexed ) {
		EM_BuildIndex();
		EM_RebuildEffectView();
	}

	em.font = re->RegisterFont( "arialnb" );
	if ( !em.font )
		em.font = cls.menuFont;

	em.cursorX = SCREEN_WIDTH * 0.5f;
	em.cursorY = SCREEN_HEIGHT * 0.5f;
	em.click = qfalse;
	em.wheel = 0;
	em.picker = EM_PICKER_NONE;
	em.playKey[0] = '\0';
	EM_RebuildEffectView();
	EM_EnterBrowse();
	EM_SetTab( em.tab );
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_EFFECTMANAGER );
}

static void EM_Close( void ) {
	em.state = EM_OFF;
	em.picker = EM_PICKER_NONE;
	em.numPending = 0;
	EM_ReleaseKeys();
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_EFFECTMANAGER );
}

void CL_EffectManager_Close( void ) {
	if ( em.state != EM_OFF )
		EM_Close();
}

void CL_EffectManager_Init( void ) {
	memset( &em, 0, sizeof( em ) );
	em.effect = -1;
	em.previewDist = 160.0f;
	em.snap = 2;
	Q_strncpyz( em.fields[EM_F_BOLT], "*r_hand", EM_FIELD_LEN );
	Q_strncpyz( em.fields[EM_F_INTENSITY], "5", EM_FIELD_LEN );
	Q_strncpyz( em.fields[EM_F_DURATION], "2", EM_FIELD_LEN );
}

// cgame is going away: map change, disconnect or vid_restart
void CL_EffectManager_Shutdown( void ) {
	CL_EffectManager_Close();
	// effects bolted to our copies die with the cgame's effects system
	EM_FreeModels();
	for ( int i = 0; i < em.numEffects; i++ ) {
		em.effects[i].id = 0;
		em.effects[i].failed = qfalse;
	}
	// the pure pk3 list can change with the next map or server
	em.indexed = qfalse;
	em.haveView = qfalse;
	em.numActive = em.numActiveView = 0;
	em.numQueue = 0;
	em.listRequestTime = em.listLineTime = em.listTime = em.refreshTime = 0;
	em.listQueued = qfalse;
	em.havePos = qfalse;
	em.target = EM_TARGET_ME;
}

qboolean CL_EffectManager_Active( void ) {
	return (qboolean)( em.state != EM_OFF );
}

/*
===============================================================================

CONSOLE COMMAND

===============================================================================
*/

void CL_EffectManager_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( Cmd_Argc() < 2 ) {
		if ( em.state == EM_OFF )
			EM_Open();
		else
			EM_Close();
		return;
	}

	if ( !Q_stricmp( arg, "active" ) || !Q_stricmp( arg, "presets" ) || !Q_stricmp( arg, "effects" ) ) {
		emTab_t tab = !Q_stricmp( arg, "active" ) ? EM_TAB_ACTIVE : !Q_stricmp( arg, "presets" ) ? EM_TAB_PRESETS : EM_TAB_EFFECTS;

		if ( em.state == EM_OFF ) {
			em.tab = tab;
			EM_Open();
		} else {
			EM_EnterBrowse();
			EM_SetTab( tab );
		}
	} else if ( !Q_stricmp( arg, "close" ) ) {
		CL_EffectManager_Close();
	} else if ( !Q_stricmp( arg, "place" ) ) {
		if ( em.state == EM_OFF )
			EM_Open();
		if ( em.state != EM_OFF && EM_Effect() ) {
			EM_SetTab( EM_TAB_EFFECTS );
			EM_EnterPlace();
		}
	} else if ( !Q_stricmp( arg, "bolt" ) && Cmd_Argc() >= 3 ) {
		em.where = EM_WHERE_BOLT;
		em.target = EM_TARGET_ME;
		Q_strncpyz( em.fields[EM_F_BOLT], Cmd_Argv( 2 ), EM_FIELD_LEN );
	} else if ( !Q_stricmp( arg, "reindex" ) ) {
		if ( em.state == EM_PLACE )
			EM_EnterBrowse();
		EM_BuildIndex();
		EM_RebuildEffectView();
	} else if ( !Q_stricmp( arg, "help" ) || !Q_stricmp( arg, "?" ) ) {
		Com_Printf( "usage: effectmanager              toggle the effect manager (bind a key to it)\n" );
		Com_Printf( "       effectmanager <search>     open the effects filtered by <search>\n" );
		Com_Printf( "       effectmanager active       open on the effects playing in the map\n" );
		Com_Printf( "       effectmanager presets      open on alarm, fadeout and camerashake\n" );
		Com_Printf( "       effectmanager place        place the selected effect in the world\n" );
		Com_Printf( "       effectmanager bolt <tag>   bolt the selected effect to your own tag or bone\n" );
		Com_Printf( "       effectmanager reindex      rescan the filesystem for effects\n" );
		Com_Printf( "       effectmanager close        close the effect manager\n" );
	} else {
		// anything else is a search
		if ( em.state == EM_OFF ) {
			em.tab = EM_TAB_EFFECTS;
			EM_Open();
		}
		if ( em.state == EM_OFF )
			return;
		EM_EnterBrowse();
		EM_SetTab( EM_TAB_EFFECTS );
		Q_strncpyz( em.fields[EM_F_SEARCH], arg, EM_FIELD_LEN );
		EM_FieldChanged( EM_F_SEARCH );
	}
}

/*
===============================================================================

INPUT

===============================================================================
*/

static qboolean EM_InRect( float x, float y, float w, float h ) {
	return (qboolean)( em.cursorX >= x && em.cursorX < x + w && em.cursorY >= y && em.cursorY < y + h );
}

static qboolean EM_ShiftDown( void ) {
	return (qboolean)( em.held[A_SHIFT] || em.held[A_SHIFT2] );
}

static qboolean EM_CtrlDown( void ) {
	return (qboolean)( em.held[A_CTRL] || em.held[A_CTRL2] );
}

// the field typing goes to
static emField_t EM_ActiveField( void ) {
	if ( em.picker != EM_PICKER_NONE )
		return EM_F_PICKER_SEARCH;
	if ( em.focus != EM_F_NONE )
		return em.focus;
	if ( em.tab == EM_TAB_ACTIVE )
		return EM_F_ACTIVE_SEARCH;
	if ( em.tab == EM_TAB_PRESETS )
		return EM_F_NONE;
	return EM_F_SEARCH;
}

// binds don't run while the manager holds the keys, so honour the user's toggle bind here
static qboolean EM_IsToggleKey( int key ) {
	const char *binding;

	// a printable key belongs to the text box
	if ( em.state == EM_BROWSE && EM_ActiveField() != EM_F_NONE && key >= A_SPACE && key <= A_TILDE )
		return qfalse;
	binding = Key_GetBinding( key );
	return (qboolean)( VALIDSTRING( binding ) && !Q_stricmp( binding, EM_TOGGLE_CMD ) );
}

static qboolean EM_FieldAllows( emField_t f, int ch ) {
	if ( f == EM_F_PITCH || f == EM_F_YAW || f == EM_F_ROLL )
		return (qboolean)( ( ch >= '0' && ch <= '9' ) || ch == '-' || ch == '.' );
	if ( f == EM_F_INTENSITY || f == EM_F_DURATION )
		return (qboolean)( ( ch >= '0' && ch <= '9' ) || ch == '.' );
	// goes on the command line as a single argument
	if ( f == EM_F_BOLT )
		return (qboolean)( ch > ' ' && ch < 127 && ch != '"' && ch != ';' );
	return (qboolean)( ch >= ' ' && ch < 127 );
}

static void EM_FieldChanged( int f ) {
	switch ( f ) {
	case EM_F_SEARCH:
		em.effectList.sel = em.effectList.scroll = 0;
		EM_RebuildEffectView();
		if ( em.numView )
			EM_SelectEffect( em.view[0] );
		break;
	case EM_F_ACTIVE_SEARCH:
		em.activeList.scroll = 0;
		EM_RebuildActiveView();
		break;
	case EM_F_PICKER_SEARCH:
		em.pickerList.sel = em.pickerList.scroll = 0;
		EM_RebuildPickerView();
		break;
	default:
		break;
	}
}

static void EM_BrowseListKey( int key ) {
	emList_t *list;
	int count, visible, delta;

	if ( em.picker != EM_PICKER_NONE ) {
		list = &em.pickerList;
		count = em.numPickerView;
		visible = (int)( ( EM_PICKER_H - 74 ) / EM_ROW_H );
	} else if ( em.tab == EM_TAB_EFFECTS ) {
		if ( key == A_CURSOR_LEFT || key == A_CURSOR_RIGHT ) {
			if ( !em.fields[EM_F_SEARCH][0] ) {
				EM_SetFolder( em.folder + ( key == A_CURSOR_LEFT ? -1 : 1 ) );
				if ( em.numView )
					EM_SelectEffect( em.view[0] );
			}
			return;
		}
		list = &em.effectList;
		count = em.numView;
		visible = (int)( EM_LIST_H / EM_ROW_H );
	} else if ( em.tab == EM_TAB_ACTIVE ) {
		list = &em.activeList;
		count = em.numActiveView;
		visible = (int)( EM_LIST_H / EM_ROW_H );
	} else {
		return;
	}

	switch ( key ) {
	case A_CURSOR_UP:	delta = -1; break;
	case A_CURSOR_DOWN:	delta = 1; break;
	case A_PAGE_UP:		delta = -visible; break;
	case A_PAGE_DOWN:	delta = visible; break;
	case A_HOME:		delta = -count; break;
	case A_END:			delta = count; break;
	default:			return;
	}
	EM_ListMove( list, count, visible, delta );
	if ( list == &em.effectList && list->sel < em.numView )
		EM_SelectEffect( em.view[list->sel] );
}

static void EM_BrowseKey( int key ) {
	emField_t f = EM_ActiveField();
	int len;

	switch ( key ) {
	case A_MOUSE1:
		em.click = qtrue;
		em.clickX = em.cursorX;
		em.clickY = em.cursorY;
		break;
	case A_MWHEELUP:
		em.wheel -= 3;
		break;
	case A_MWHEELDOWN:
		em.wheel += 3;
		break;
	case A_BACKSPACE:
		if ( f == EM_F_NONE )
			break;
		len = strlen( em.fields[f] );
		if ( len ) {
			em.fields[f][len - 1] = '\0';
			EM_FieldChanged( f );
		}
		break;
	case A_DELETE:
		if ( f == EM_F_NONE )
			break;
		em.fields[f][0] = '\0';
		EM_FieldChanged( f );
		break;
	case A_TAB:
		if ( em.picker == EM_PICKER_NONE )
			EM_SetTab( (emTab_t)( ( em.tab + 1 ) % 3 ) );
		break;
	case A_ENTER:
	case A_KP_ENTER:
		if ( em.picker != EM_PICKER_NONE ) {
			if ( em.pickerList.sel < em.numPickerView )
				EM_ChoosePicker( em.pickerView[em.pickerList.sel] );
		} else if ( em.tab == EM_TAB_EFFECTS ) {
			EM_Send();
		}
		break;
	default:
		EM_BrowseListKey( key );
		break;
	}
}

static void EM_Rotate( int axis, int sign ) {
	vec3_t angles;
	int step = emSnaps[em.snap].angle;

	EM_GetAngles( angles );
	angles[axis] = (float)EM_NormalizeAngle( EM_SnapTo( angles[axis] + sign * step, step ) );
	EM_SetAngles( angles );
	em.align = qfalse;
}

// moves along the world axis closest to the camera direction, so nudges stay on the grid
static void EM_Nudge( qboolean forwardAxis, int sign ) {
	vec3_t forward, right;
	float yaw = DEG2RAD( floorf( em.camAng[YAW] / 90.0f + 0.5f ) * 90.0f );
	int grid = emSnaps[em.snap].grid;

	VectorSet( forward, cosf( yaw ), sinf( yaw ), 0 );
	VectorSet( right, sinf( yaw ), -cosf( yaw ), 0 );
	em.locked = qtrue;
	VectorMA( em.origin, (float)( sign * grid ), forwardAxis ? forward : right, em.origin );
	em.origin[0] = floorf( em.origin[0] + 0.5f );
	em.origin[1] = floorf( em.origin[1] + 0.5f );
}

static void EM_PlaceKey( int key ) {
	const qboolean bolted = (qboolean)( em.where == EM_WHERE_BOLT );

	switch ( key ) {
	case A_MOUSE1:
	case A_ENTER:
	case A_KP_ENTER:
		EM_Send();
		break;
	case A_CAP_M:
		em.loop = (qboolean)!em.loop;
		break;
	case A_CAP_H:
		em.hideHelp = (qboolean)!em.hideHelp;
		break;
	case A_TAB:
		EM_EnterBrowse();
		break;
	default:
		break;
	}
	if ( bolted )
		return;

	switch ( key ) {
	case A_MOUSE2:
	case A_CAP_F:
		em.locked = (qboolean)!em.locked;
		break;
	case A_MWHEELUP:	EM_Rotate( YAW, 1 );	break;
	case A_MWHEELDOWN:	EM_Rotate( YAW, -1 );	break;
	case A_CAP_Z:		EM_Rotate( PITCH, -1 );	break;
	case A_CAP_X:		EM_Rotate( PITCH, 1 );	break;
	case A_CAP_Q:		EM_Rotate( ROLL, -1 );	break;
	case A_CAP_E:		EM_Rotate( ROLL, 1 );	break;
	case A_CAP_R:
	case A_BACKSPACE:
		em.fields[EM_F_PITCH][0] = em.fields[EM_F_YAW][0] = em.fields[EM_F_ROLL][0] = '\0';
		em.align = qfalse;
		break;
	case A_CAP_N:
		em.align = (qboolean)!em.align;
		break;
	case A_CAP_G:
		em.snap = ( em.snap + 1 ) % ARRAY_LEN( emSnaps );
		break;
	case A_CURSOR_UP:		EM_Nudge( qtrue, 1 );	break;
	case A_CURSOR_DOWN:		EM_Nudge( qtrue, -1 );	break;
	case A_CURSOR_RIGHT:	EM_Nudge( qfalse, 1 );	break;
	case A_CURSOR_LEFT:		EM_Nudge( qfalse, -1 );	break;
	case A_PAGE_UP:
		em.locked = qtrue;
		em.origin[2] += emSnaps[em.snap].grid;
		break;
	case A_PAGE_DOWN:
		em.locked = qtrue;
		em.origin[2] -= emSnaps[em.snap].grid;
		break;
	default:
		break;
	}
}

// the active effect whose label is under the cursor gets checked or unchecked
static void EM_PickClick( void ) {
	float best = 24.0f * 24.0f, x, y;
	int found = -1;

	for ( int i = 0; i < em.numActive; i++ ) {
		if ( !EM_Project( em.active[i].origin, &x, &y ) )
			continue;
		x -= em.cursorX;
		y -= em.cursorY;
		if ( x * x + y * y < best ) {
			best = x * x + y * y;
			found = i;
		}
	}
	if ( found >= 0 )
		em.active[found].checked = (qboolean)!em.active[found].checked;
}

void CL_EffectManager_KeyEvent( int key, qboolean down ) {
	if ( em.state == EM_OFF )
		return;

	if ( key >= 0 && key < MAX_KEYS )
		em.held[key] = down;
	if ( !down )
		return;

	if ( EM_IsToggleKey( key ) ) {
		EM_Close();
		return;
	}

	switch ( em.state ) {
	case EM_BROWSE:
		EM_BrowseKey( key );
		break;
	case EM_PLACE:
		EM_PlaceKey( key );
		break;
	case EM_PICK:
		if ( key == A_MOUSE1 )
			EM_PickClick();
		else if ( key == A_MOUSE2 || key == A_ENTER || key == A_KP_ENTER )
			EM_EnterBrowse();
		break;
	default:
		break;
	}
}

void CL_EffectManager_CharEvent( int ch ) {
	emField_t f;
	int len;

	if ( em.state != EM_BROWSE )
		return;
	f = EM_ActiveField();
	if ( f == EM_F_NONE )
		return;
	len = strlen( em.fields[f] );
	if ( EM_FieldAllows( f, ch ) && len < EM_FIELD_LEN - 1 ) {
		em.fields[f][len] = (char)ch;
		em.fields[f][len + 1] = '\0';
		EM_FieldChanged( f );
	}
}

void CL_EffectManager_MouseEvent( int dx, int dy ) {
	if ( em.state == EM_PLACE ) {
		em.camAng[YAW] -= dx * cl_sensitivity->value * m_yaw->value;
		em.camAng[PITCH] = Com_Clamp( -89.0f, 89.0f, em.camAng[PITCH] + dy * cl_sensitivity->value * m_pitch->value );
		return;
	}
	em.cursorX = Com_Clamp( 0, SCREEN_WIDTH, em.cursorX + dx );
	em.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, em.cursorY + dy );
}

// escape backs out one level: picker, a text box, the world, then the manager
void CL_EffectManager_Escape( void ) {
	if ( em.state == EM_PLACE || em.state == EM_PICK )
		EM_EnterBrowse();
	else if ( em.picker != EM_PICKER_NONE )
		em.picker = EM_PICKER_NONE;
	else if ( em.focus != EM_F_NONE )
		em.focus = EM_F_NONE;
	else
		EM_Close();
}

/*
===============================================================================

3D VIEW

===============================================================================
*/

static void EM_UpdateCamera( float dt ) {
	vec3_t forward, right, move;
	float speed;

	AngleVectors( em.camAng, forward, right, NULL );
	VectorClear( move );
	if ( em.held[A_CAP_W] )		VectorAdd( move, forward, move );
	if ( em.held[A_CAP_S] )		VectorSubtract( move, forward, move );
	if ( em.held[A_CAP_D] )		VectorAdd( move, right, move );
	if ( em.held[A_CAP_A] )		VectorSubtract( move, right, move );
	if ( em.held[A_SPACE] )		move[2] += 1.0f;
	if ( em.held[A_CAP_C] )		move[2] -= 1.0f;

	if ( VectorNormalize( move ) > 0.0f ) {
		speed = EM_FLY_SPEED;
		if ( EM_ShiftDown() )
			speed *= 3.0f;
		if ( EM_CtrlDown() )
			speed *= 0.25f;
		VectorMA( em.camOrg, speed * dt, move, em.camOrg );
	}
}

// put the effect where the crosshair meets the world, just off that surface
static void EM_FollowCrosshair( void ) {
	vec3_t forward, end, angles;
	trace_t tr;
	int grid = emSnaps[em.snap].grid;

	AngleVectors( em.camAng, forward, NULL, NULL );
	VectorMA( em.camOrg, EM_TRACE_DIST, forward, end );
	CM_BoxTrace( &tr, em.camOrg, end, vec3_origin, vec3_origin, 0, EM_TRACE_MASK, qfalse );

	if ( tr.fraction < 1.0f && !tr.startsolid && !( tr.surfaceFlags & SURF_SKY ) ) {
		VectorMA( tr.endpos, 1.0f, tr.plane.normal, em.origin );
		// snap along the surface, not off it
		for ( int i = 0; i < 3; i++ ) {
			if ( fabsf( tr.plane.normal[i] ) < 0.7f )
				em.origin[i] = EM_SnapTo( em.origin[i], grid );
		}
		if ( em.align ) {
			vectoangles( tr.plane.normal, angles );
			EM_SetAngles( angles );
		}
	} else {
		VectorMA( em.camOrg, EM_NO_HIT_DIST, forward, em.origin );
		for ( int i = 0; i < 3; i++ )
			em.origin[i] = EM_SnapTo( em.origin[i], grid );
	}
}

static void EM_ExpireModels( void ) {
	for ( int i = 0; i < EM_MAX_RETIRED; i++ ) {
		if ( em.retired[i] && cls.realtime - em.retiredTime[i] > EM_RETIRE_MS ) {
			re->G2API_CleanGhoul2Models( &em.retired[i] );
			em.retired[i] = NULL;
		}
	}
}

// called every frame before the cgame draws
void CL_EffectManager_Frame( void ) {
	float dt;

	// queued commands go out even with the manager closed
	EM_RunQueue();
	EM_ExpireModels();

	if ( em.state == EM_OFF )
		return;
	if ( !( Key_GetCatcher() & KEYCATCH_EFFECTMANAGER ) ) {
		// something else cleared the catchers
		em.state = EM_OFF;
		EM_ReleaseKeys();
		return;
	}

	// a list we asked for that never came, or one that is due
	if ( em.listRequestTime && cls.realtime - em.listRequestTime > EM_LIST_TIMEOUT ) {
		em.listRequestTime = 0;
		EM_RebuildActiveView();
	}
	if ( em.refreshTime && cls.realtime >= em.refreshTime )
		EM_RequestList();

	dt = Com_Clamp( 0.0f, 0.1f, ( cls.realtime - em.lastFrameTime ) / 1000.0f );
	em.lastFrameTime = cls.realtime;
	if ( em.state == EM_PLACE ) {
		EM_UpdateCamera( dt );
		if ( em.where != EM_WHERE_BOLT && !em.locked )
			EM_FollowCrosshair();
	}

	EM_PreviewFrame();
}

// a thin bar both ways round, as the model placer draws its box
static void EM_AddEdge( const vec3_t a, const vec3_t b, const byte *color ) {
	polyVert_t verts[4], back[4];
	vec3_t dir, toCam, side, mid;
	float width;

	VectorSubtract( b, a, dir );
	VectorAdd( a, b, mid );
	VectorScale( mid, 0.5f, mid );
	VectorSubtract( em.camOrg, mid, toCam );
	width = 0.3f + VectorLength( toCam ) * 0.001f;
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
	for ( int i = 0; i < 4; i++ )
		back[i] = verts[3 - i];
	re->AddPolyToScene( cls.whiteShader, 4, verts, 1 );
	re->AddPolyToScene( cls.whiteShader, 4, back, 1 );
}

// a small cross where the effect goes, and which way it faces
static void EM_AddMarker( void ) {
	static const byte crossColor[4] = { 255, 255, 255, 255 };
	static const byte dirColor[4] = { 255, 200, 64, 255 };
	static const byte sentColor[4] = { 64, 255, 96, 255 };
	vec3_t a, b, dir;

	for ( int i = 0; i < 3; i++ ) {
		VectorCopy( em.origin, a );
		VectorCopy( em.origin, b );
		a[i] -= 6.0f;
		b[i] += 6.0f;
		EM_AddEdge( a, b, crossColor );
	}
	EM_Direction( dir );
	VectorMA( em.origin, 32.0f, dir, b );
	EM_AddEdge( em.origin, b, cls.realtime - em.sentTime < 300 ? sentColor : dirColor );
}

// every entity cgame adds; models are kept to find the bolt target among them
void CL_EffectManager_AddEntity( const refEntity_t *ent ) {
	emCaptured_t *c;

	if ( em.state == EM_OFF || !ent->ghoul2 || ent->reType != RT_MODEL || em.numPending >= EM_MAX_CAPTURED )
		return;
	if ( !re->G2API_HaveWeGhoul2Models( *(CGhoul2Info_v *)ent->ghoul2 ) )
		return;
	c = &em.pending[em.numPending++];
	c->ghoul2 = (CGhoul2Info_v *)ent->ghoul2;
	VectorCopy( ent->origin, c->origin );
	VectorCopy( ent->modelScale, c->scale );
}

// the view weapon would float where the player stands
qboolean CL_EffectManager_FilterEntity( const refEntity_t *ent ) {
	return (qboolean)( em.state == EM_PLACE && ( ent->renderfx & RF_FIRST_PERSON ) );
}

// the free camera, for the effects system to cull against
qboolean CL_EffectManager_Camera( vec3_t origin, vec3_t angles ) {
	if ( em.state != EM_PLACE )
		return qfalse;
	VectorCopy( em.camOrg, origin );
	VectorCopy( em.camAng, angles );
	return qtrue;
}

static void EM_KeepView( const refdef_t *fd ) {
	VectorCopy( fd->vieworg, em.viewOrg );
	VectorCopy( fd->viewaxis[0], em.viewAxis[0] );
	VectorCopy( fd->viewaxis[1], em.viewAxis[1] );
	VectorCopy( fd->viewaxis[2], em.viewAxis[2] );
	em.fovX = fd->fov_x;
	em.fovY = fd->fov_y;
	em.haveView = qtrue;
}

// every cgame scene passes through here before reaching the renderer; qtrue if it was rendered
qboolean CL_EffectManager_RenderScene( const refdef_t *fd ) {
	refdef_t view;

	if ( em.state == EM_OFF ) {
		em.numPending = 0;
		return qfalse;
	}
	if ( fd->rdflags & ( RDF_NOWORLDMODEL | RDF_AUTOMAP ) ) {
		em.numPending = 0;
		return qfalse;
	}

	if ( fd->rdflags & RDF_SKYBOXPORTAL ) {
		em.numPending = 0;
		if ( em.state != EM_PLACE )
			return qfalse;
		// the sky portal keeps its own origin but looks the way the camera does
		view = *fd;
		AnglesToAxis( em.camAng, view.viewaxis );
		VectorCopy( em.camAng, view.viewangles );
		re->RenderScene( &view );
		return qtrue;
	}

	// the main view: the models in it are all in, and still alive
	EM_UpdateTarget();
	em.numPending = 0;

	if ( em.state != EM_PLACE ) {
		EM_KeepView( fd );
		return qfalse;
	}

	view = *fd;
	if ( em.viewFrame != cls.framecount ) {
		em.viewFrame = cls.framecount;
		if ( em.where != EM_WHERE_BOLT )
			EM_AddMarker();
	}
	VectorCopy( em.camOrg, view.vieworg );
	VectorCopy( em.camAng, view.viewangles );
	AnglesToAxis( em.camAng, view.viewaxis );
	view.viewContents = CM_PointContents( em.camOrg, 0 );
	// the snapshot's area mask is from the player's position, the camera can be anywhere
	memset( view.areamask, 0, sizeof( view.areamask ) );
	EM_KeepView( &view );
	re->RenderScene( &view );
	return qtrue;
}

/*
===============================================================================

2D

===============================================================================
*/

static void EM_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void EM_Box( float x, float y, float w, float h, const float *fill ) {
	if ( fill )
		EM_Fill( x, y, w, h, fill );
	EM_Fill( x, y, w, 1, emBorder );
	EM_Fill( x, y + h - 1, w, 1, emBorder );
	EM_Fill( x, y, 1, h, emBorder );
	EM_Fill( x + w - 1, y, 1, h, emBorder );
}

static void EM_Text( float x, float y, const char *text, const float *color ) {
	re->Font_DrawString( (int)x, (int)y, text, color, em.font | STYLE_DROPSHADOW, -1, EM_TEXT_SCALE );
}

static float EM_TextWidth( const char *text ) {
	return (float)re->Font_StrLenPixels( text, em.font, EM_TEXT_SCALE );
}

static void EM_TextClipped( float x, float y, float w, const char *text, const float *color ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 3 && EM_TextWidth( buf ) > w ) {
		len--;
		buf[len - 3] = '.';
		buf[len - 2] = '.';
		buf[len - 1] = '.';
		buf[len] = '\0';
	}
	EM_Text( x, y, buf, color );
}

// eats this frame's click if it landed in the rect
static qboolean EM_Clicked( float x, float y, float w, float h ) {
	if ( !em.click || em.clickX < x || em.clickX >= x + w || em.clickY < y || em.clickY >= y + h )
		return qfalse;
	em.click = qfalse;
	return qtrue;
}

static qboolean EM_Button( float x, float y, float w, const char *label, qboolean selected, qboolean enabled ) {
	const float *fill = selected ? emHighlight : ( enabled && EM_InRect( x, y, w, EM_CTRL_H ) ) ? emHoverBox : emPanelLight;

	EM_Box( x, y, w, EM_CTRL_H, fill );
	EM_Text( x + ( w - EM_TextWidth( label ) ) * 0.5f, y + 2, label, enabled ? emWhite : emDim );
	return (qboolean)( EM_Clicked( x, y, w, EM_CTRL_H ) && enabled );
}

// a button that has to be clicked twice
static qboolean EM_DangerButton( float x, float y, float w, const char *label, emConfirm_t id, qboolean enabled ) {
	qboolean armed = (qboolean)( em.confirm == id && cls.realtime - em.confirmTime < EM_CONFIRM_MS );
	const char *text = armed ? "Click again" : label;

	EM_Box( x, y, w, EM_CTRL_H, armed ? emDanger : ( enabled && EM_InRect( x, y, w, EM_CTRL_H ) ) ? emHoverBox : emPanelLight );
	EM_Text( x + ( w - EM_TextWidth( text ) ) * 0.5f, y + 2, text, enabled ? emWhite : emDim );
	if ( !EM_Clicked( x, y, w, EM_CTRL_H ) || !enabled )
		return qfalse;
	if ( armed ) {
		em.confirm = EM_CONFIRM_NONE;
		return qtrue;
	}
	em.confirm = id;
	em.confirmTime = cls.realtime;
	return qfalse;
}

static void EM_Field( emField_t f, float x, float y, float w, const char *placeholder ) {
	qboolean focused = (qboolean)( EM_ActiveField() == f );
	const char *text = em.fields[f];
	char buf[EM_FIELD_LEN + 2];

	EM_Box( x, y, w, EM_CTRL_H, focused ? emPanelFocus : emPanelLight );
	if ( !text[0] && !focused ) {
		EM_TextClipped( x + 4, y + 2, w - 8, placeholder, emDim );
	} else {
		// keep the end in view
		Com_sprintf( buf, sizeof( buf ), "%s%s", text, focused && ( ( cls.realtime >> 8 ) & 1 ) ? "_" : "" );
		const char *shown = buf;
		while ( shown[1] && EM_TextWidth( shown ) > w - 8 )
			shown++;
		EM_Text( x + 4, y + 2, shown, emWhite );
	}
	if ( EM_Clicked( x, y, w, EM_CTRL_H ) )
		em.focus = ( f == EM_F_SEARCH || f == EM_F_ACTIVE_SEARCH || f == EM_F_PICKER_SEARCH ) ? EM_F_NONE : f;
}

static void EM_Label( float x, float y, const char *label ) {
	EM_Text( x, y + 2, label, emDim );
}

typedef void ( *emRowFunc_t )( int row, float x, float y, float w );

// returns the row double-clicked, -1 if none; *clicked gets the row clicked once
static int EM_List( emList_t *list, int count, float x, float y, float w, float h, emRowFunc_t drawRow, int *clicked ) {
	const int visible = (int)( h / EM_ROW_H );
	int row, picked = -1;

	if ( clicked )
		*clicked = -1;
	if ( em.wheel && EM_InRect( x, y, w, h ) ) {
		list->scroll += em.wheel;
		em.wheel = 0;
	}
	EM_ListClamp( list, count, visible );

	if ( EM_Clicked( x, y, w, h ) ) {
		row = list->scroll + (int)( ( em.clickY - y ) / EM_ROW_H );
		if ( row < count ) {
			if ( row == list->lastClickRow && cls.realtime - list->lastClickTime < EM_DOUBLECLICK_MS ) {
				list->lastClickTime = 0;
				picked = row;
			} else {
				list->lastClickRow = row;
				list->lastClickTime = cls.realtime;
			}
			list->sel = row;
			if ( clicked )
				*clicked = row;
		}
	}

	EM_Box( x, y, w, h, emPanelLight );
	for ( int i = 0; i < visible; i++ ) {
		float ry = y + i * EM_ROW_H;

		row = list->scroll + i;
		if ( row >= count )
			break;
		if ( row == list->sel )
			EM_Fill( x + 1, ry, w - 2, EM_ROW_H, emHighlight );
		else if ( EM_InRect( x, ry, w, EM_ROW_H ) )
			EM_Fill( x + 1, ry, w - 2, EM_ROW_H, emHover );
		drawRow( row, x + 4, ry + 1, w - 8 );
	}
	return picked;
}

static void EM_DrawFolderRow( int row, float x, float y, float w ) {
	EM_TextClipped( x, y, w, va( "%s " S_COLOR_GREY "(%i)", em.folders[row].name[0] ? em.folders[row].name : "(root)", em.folders[row].count ),
		em.fields[EM_F_SEARCH][0] ? emDim : emWhite );
}

static void EM_DrawEffectRow( int row, float x, float y, float w ) {
	const emEffect_t *e = &em.effects[em.view[row]];

	EM_TextClipped( x, y, w, em.fields[EM_F_SEARCH][0] ? e->path : e->base, e->failed ? emRed : emWhite );
}

static void EM_DrawCheck( float x, float y, qboolean checked ) {
	EM_Box( x, y + 1, 9, 9, emPanel );
	if ( checked )
		EM_Fill( x + 2, y + 3, 5, 5, emAccent );
}

static void EM_DrawActiveRow( int row, float x, float y, float w ) {
	const emActive_t *a = &em.active[em.activeView[row]];

	EM_DrawCheck( x, y, a->checked );
	EM_Text( x + 14, y, va( S_COLOR_CYAN "%i", a->num ), emWhite );
	EM_TextClipped( x + 44, y, w - 44, va( "%s " S_COLOR_GREY "%i u", a->path, (int)Distance( a->origin, cl.snap.ps.origin ) ), emWhite );
}

static void EM_DrawPickerRow( int row, float x, float y, float w ) {
	const int index = em.pickerView[row];
	int num;
	const char *type, *name;

	switch ( em.picker ) {
	case EM_PICKER_PLAYER:
	case EM_PICKER_REMOVE_PLAYER:
		EM_TextClipped( x, y, w, va( S_COLOR_CYAN "%i " S_COLOR_WHITE "%s%s", index, EM_PlayerName( index ), index == clc.clientNum ? S_COLOR_GREY " (you)" : "" ), emWhite );
		break;
	case EM_PICKER_NPC:
	case EM_PICKER_REMOVE_NPC:
		if ( !CL_NpcManager_GetNpc( index, &num, &type, &name ) )
			break;
		if ( name[0] )
			EM_TextClipped( x, y, w, va( S_COLOR_CYAN "%i " S_COLOR_WHITE "%s " S_COLOR_GREY "%s", num, name, type ), emWhite );
		else
			EM_TextClipped( x, y, w, va( "%i %s  (no targetname, can't be named)", num, type ), emDim );
		break;
	case EM_PICKER_BOLT:
		EM_TextClipped( x, y, w, va( "%s%s", em.bolts[index].name, em.bolts[index].tag ? "" : S_COLOR_GREY "  bone" ), emWhite );
		break;
	default:
		break;
	}
}

static void EM_DrawCommandLines( const char *preview ) {
	if ( preview )
		EM_TextClipped( EM_RIGHT_X, EM_BUTTON_Y - 30, EM_RIGHT_W, va( S_COLOR_GREY "%s", preview ), emWhite );
	if ( em.lastCmd[0] ) {
		EM_TextClipped( EM_RIGHT_X, EM_BUTTON_Y - 17, EM_RIGHT_W,
			va( "%sSent: %s", cls.realtime - em.sentTime < 1500 ? S_COLOR_GREEN : S_COLOR_GREY, em.lastCmd ), emWhite );
	} else if ( em.numQueue ) {
		EM_TextClipped( EM_RIGHT_X, EM_BUTTON_Y - 17, EM_RIGHT_W, va( S_COLOR_GREY "%i commands waiting for the server's flood protection", em.numQueue ), emWhite );
	}
}

// the target's model in a box, turning, with the bolt marked on it
static void EM_RenderModel( float x, float y, float w, float h, const char *bolt ) {
	refdef_t refdef;
	refEntity_t ent;
	vec3_t center, forward, angles, p, d;
	float xScale, yScale, fovX, fovY, radius, halfH, dist;
	const float yaw = (float)( cls.realtime % 12000 ) * 0.03f + em.modelYaw;	// a turn every 12 seconds

	if ( !em.g2View )
		return;

	radius = sqrtf( Square( Q_max( fabsf( em.g2Mins[0] ), fabsf( em.g2Maxs[0] ) ) ) + Square( Q_max( fabsf( em.g2Mins[1] ), fabsf( em.g2Maxs[1] ) ) ) );
	halfH = ( em.g2Maxs[2] - em.g2Mins[2] ) * 0.5f;
	VectorSet( center, 0, 0, ( em.g2Mins[2] + em.g2Maxs[2] ) * 0.5f );

	xScale = cls.glconfig.vidWidth / (float)SCREEN_WIDTH;
	yScale = cls.glconfig.vidHeight / (float)SCREEN_HEIGHT;
	memset( &refdef, 0, sizeof( refdef ) );
	refdef.x = (int)( x * xScale );
	refdef.y = (int)( y * yScale );
	refdef.width = (int)( w * xScale );
	refdef.height = (int)( h * yScale );
	fovY = 30.0f;
	fovX = RAD2DEG( 2.0f * atanf( tanf( DEG2RAD( fovY * 0.5f ) ) * refdef.width / (float)refdef.height ) );
	refdef.fov_x = fovX;
	refdef.fov_y = fovY;
	dist = Q_max( halfH / tanf( DEG2RAD( fovY * 0.5f ) ), radius / tanf( DEG2RAD( fovX * 0.5f ) ) ) * 1.1f + radius;

	VectorSet( angles, 8.0f, 180.0f, 0 );
	AngleVectors( angles, forward, NULL, NULL );
	VectorMA( center, -dist, forward, refdef.vieworg );
	AnglesToAxis( angles, refdef.viewaxis );
	refdef.rdflags = RDF_NOWORLDMODEL;
	refdef.time = cl.serverTime;

	memset( &ent, 0, sizeof( ent ) );
	ent.reType = RT_MODEL;
	ent.ghoul2 = em.g2View;
	ent.renderfx = RF_NOSHADOW;
	VectorSet( ent.angles, 0, yaw, 0 );
	AnglesToAxis( ent.angles, ent.axis );
	VectorCopy( em.g2Scale, ent.modelScale );
	ent.radius = radius + halfH;

	re->ClearScene();
	re->AddRefEntityToScene( &ent );
	re->RenderScene( &refdef );

	// the bolt, drawn over the model so the body can't hide it
	if ( bolt && EM_BoltPoint( bolt, yaw, p ) ) {
		float sx, sy, fwd;

		VectorSubtract( p, refdef.vieworg, d );
		fwd = DotProduct( d, refdef.viewaxis[0] );
		if ( fwd > 1.0f ) {
			sx = x + w * 0.5f * ( 1.0f - DotProduct( d, refdef.viewaxis[1] ) / fwd / tanf( DEG2RAD( fovX * 0.5f ) ) );
			sy = y + h * 0.5f * ( 1.0f - DotProduct( d, refdef.viewaxis[2] ) / fwd / tanf( DEG2RAD( fovY * 0.5f ) ) );
			if ( sx > x && sx < x + w && sy > y && sy < y + h ) {
				const float s = ( cls.realtime / 300 ) % 2 ? 5.0f : 4.0f;

				EM_Fill( sx - s - 1, sy - s - 1, s * 2 + 2, s * 2 + 2, emPanel );
				EM_Fill( sx - s, sy - s, s * 2, s * 2, emAccent );
			}
		}
	}
}

// the window: the world with the effect playing in it, or the target with its bolt
static void EM_DrawWindow( void ) {
	const float x = EM_RIGHT_X, y = EM_VIEW_Y, w = EM_RIGHT_W, h = EM_VIEW_H;
	const emEffect_t *e = EM_Effect();

	if ( em.where == EM_WHERE_BOLT ) {
		EM_Box( x, y, w, h, emPanelLight );
		if ( em.wheel && EM_InRect( x, y, w, h ) ) {
			em.modelYaw += em.wheel * 5.0f;	// 15 degrees a notch
			em.wheel = 0;
		}
		if ( em.g2 && em.targetSeen ) {
			EM_RenderModel( x + 1, y + 1, w - 2, h - 2, em.fields[EM_F_BOLT] );
			EM_TextClipped( x + 6, y + h - 16, w - 12, va( S_COLOR_GREY "%s", em.g2Model ), emWhite );
		} else {
			EM_TextClipped( x + 8, y + 8, w - 16, "The target isn't in view.", emRed );
			EM_TextClipped( x + 8, y + 8 + EM_ROW_H, w - 16, "Its model shows here once it is drawn:", emDim );
			EM_TextClipped( x + 8, y + 8 + EM_ROW_H * 2, w - 16, "get it on screen, or use third person for yourself.", emDim );
		}
		return;
	}

	// left clear, the effect plays in the world behind it
	EM_Box( x, y, w, h, NULL );
	if ( em.wheel && EM_InRect( x, y, w, h ) ) {
		em.previewDist = Com_Clamp( 32.0f, 2048.0f, em.previewDist * ( em.wheel > 0 ? 1.25f : 0.8f ) );
		em.wheel = 0;
	}
	if ( !e )
		EM_TextClipped( x + 6, y + 4, w - 12, "Pick an effect on the left", emDim );
	else if ( em.playError )
		EM_TextClipped( x + 6, y + 4, w - 12, em.playError, emRed );
	EM_TextClipped( x + 6, y + h - 16, w - 12, va( S_COLOR_GREY "Live, %i units away   Wheel nearer/farther", (int)em.previewDist ), emWhite );
}

static void EM_DrawEffects( void ) {
	const emEffect_t *e;
	const char *cmd;
	float y, x;
	int picked, clicked;
	int length = 0, repeatDelay = 0;

	// folders and effects
	EM_Field( EM_F_SEARCH, EM_FOLDER_X, EM_SEARCH_Y, EM_FOLDER_W + EM_EFFECT_W + 4, "Type to search all effects" );
	em.folderList.sel = em.fields[EM_F_SEARCH][0] ? -1 : em.folder;
	EM_List( &em.folderList, em.numFolders, EM_FOLDER_X, EM_LIST_Y, EM_FOLDER_W, EM_LIST_H, EM_DrawFolderRow, &clicked );
	if ( clicked >= 0 ) {
		EM_SetFolder( clicked );
		if ( em.numView )
			EM_SelectEffect( em.view[0] );
	}
	picked = EM_List( &em.effectList, em.numView, EM_EFFECT_X, EM_LIST_Y, EM_EFFECT_W, EM_LIST_H, EM_DrawEffectRow, &clicked );
	if ( clicked >= 0 && clicked < em.numView )
		EM_SelectEffect( em.view[clicked] );
	if ( !em.numView )
		EM_Text( EM_EFFECT_X + 4, EM_LIST_Y + 1, em.numEffects ? "No effects match" : "No effects found", emDim );
	e = EM_Effect();

	EM_DrawWindow();

	y = EM_VIEW_Y + EM_VIEW_H + 4;
	if ( e ) {
		const char *timing = "";

		if ( e->id && theFxScheduler.GetEffectTiming( e->id, &length, &repeatDelay ) )
			timing = va( S_COLOR_GREY "   lasts %.1f s", length / 1000.0f );
		EM_TextClipped( EM_RIGHT_X, y, EM_RIGHT_W, va( S_COLOR_YELLOW "%s%s", e->path, timing ), emWhite );
	}

	y += 16;
	EM_Label( EM_RIGHT_X, y, "Play" );
	if ( EM_Button( EM_CTRL_X, y, 74, "Once", (qboolean)!em.loop, qtrue ) )
		em.loop = qfalse;
	if ( EM_Button( EM_CTRL_X + 78, y, 74, "Loop (add)", em.loop, qtrue ) )
		em.loop = qtrue;

	y += 20;
	EM_Label( EM_RIGHT_X, y, "Where" );
	if ( EM_Button( EM_CTRL_X, y, 74, "At me", (qboolean)( em.where == EM_WHERE_ME ), qtrue ) )
		em.where = EM_WHERE_ME;
	if ( EM_Button( EM_CTRL_X + 78, y, 74, "Position", (qboolean)( em.where == EM_WHERE_POS ), qtrue ) ) {
		em.where = EM_WHERE_POS;
		if ( !em.havePos )
			EM_EnterPlace();
	}
	if ( EM_Button( EM_CTRL_X + 156, y, 72, "Bolted", (qboolean)( em.where == EM_WHERE_BOLT ), qtrue ) )
		em.where = EM_WHERE_BOLT;

	y += 20;
	if ( em.where == EM_WHERE_BOLT ) {
		EM_Label( EM_RIGHT_X, y, "On" );
		if ( EM_Button( EM_CTRL_X, y, 74, "Me", (qboolean)( em.target == EM_TARGET_ME ), qtrue ) )
			em.target = EM_TARGET_ME;
		if ( EM_Button( EM_CTRL_X + 78, y, 74, "Player...", (qboolean)( em.target == EM_TARGET_PLAYER ), qtrue ) )
			EM_OpenPicker( EM_PICKER_PLAYER );
		if ( EM_Button( EM_CTRL_X + 156, y, 72, "NPC...", (qboolean)( em.target == EM_TARGET_NPC ), qtrue ) )
			EM_OpenPicker( EM_PICKER_NPC );
		y += 18;
		EM_TextClipped( EM_CTRL_X, y, EM_CTRL_W, EM_TargetLabel(), emWhite );

		y += 16;
		EM_Label( EM_RIGHT_X, y, "Bolt" );
		EM_Field( EM_F_BOLT, EM_CTRL_X, y, EM_CTRL_W - 104, "*tag or bone" );
		if ( EM_Button( EM_CTRL_X + EM_CTRL_W - 100, y, 100, "Tags & bones...", qfalse, (qboolean)( em.g2 != NULL && em.numBolts > 0 ) ) )
			EM_OpenPicker( EM_PICKER_BOLT );
		y += 18;
		if ( em.g2View && em.fields[EM_F_BOLT][0] && !EM_BoltExists() )
			EM_TextClipped( EM_CTRL_X, y, EM_CTRL_W, va( "%s has no %s", em.g2Model, em.fields[EM_F_BOLT] ), emRed );
	} else {
		if ( em.where == EM_WHERE_POS ) {
			EM_Label( EM_RIGHT_X, y, "At" );
			if ( EM_Button( EM_CTRL_X, y, 110, em.havePos ? "Pick again..." : "Pick in world...", qfalse, (qboolean)( e != NULL ) ) )
				EM_EnterPlace();
			x = EM_CTRL_X + 116;
			EM_TextClipped( x, y + 2, EM_RIGHT_X + EM_RIGHT_W - x, em.havePos
				? va( S_COLOR_GREY "%i %i %i", (int)em.pos[0], (int)em.pos[1], (int)em.pos[2] )
				: S_COLOR_GREY "not picked yet", emWhite );
		} else {
			EM_TextClipped( EM_CTRL_X, y + 2, EM_CTRL_W, S_COLOR_GREY "Where you stand when it is sent", emWhite );
		}

		y += 20;
		EM_Label( EM_RIGHT_X, y, "Angles" );
		EM_Field( EM_F_PITCH, EM_CTRL_X, y, 44, "pitch" );
		EM_Field( EM_F_YAW, EM_CTRL_X + 48, y, 44, "yaw" );
		EM_Field( EM_F_ROLL, EM_CTRL_X + 96, y, 44, "roll" );
		if ( EM_Button( EM_CTRL_X + 144, y, 84, "Straight up", qfalse, EM_HaveAngles() ) )
			em.fields[EM_F_PITCH][0] = em.fields[EM_F_YAW][0] = em.fields[EM_F_ROLL][0] = '\0';
		y += 18;
		EM_TextClipped( EM_CTRL_X, y, EM_CTRL_W, S_COLOR_GREY "Empty or 0 0 0 points it straight up", emWhite );
	}

	cmd = EM_BuildCommand();
	EM_DrawCommandLines( cmd ? va( "Will send: %s", cmd ) : em.where == EM_WHERE_BOLT && em.target == EM_TARGET_NPC && !em.targetName[0] ? "Pick an NPC with a targetname" : NULL );
	if ( EM_Button( EM_RIGHT_X, EM_BUTTON_Y, EM_RIGHT_W - 124, "Send  (Enter)", qfalse, (qboolean)( cmd != NULL ) ) )
		EM_Send();
	if ( EM_Button( EM_RIGHT_X + EM_RIGHT_W - 120, EM_BUTTON_Y, 120, em.where == EM_WHERE_BOLT ? "View in world..." : "Place in world...", qfalse, (qboolean)( e != NULL ) )
		|| ( picked >= 0 && e ) )
		EM_EnterPlace();
}

static void EM_DrawActive( void ) {
	const emActive_t *a = NULL;
	float y, w;
	int checked = EM_CountChecked(), clicked;

	EM_Field( EM_F_ACTIVE_SEARCH, EM_FOLDER_X, EM_SEARCH_Y, EM_ACTIVE_W, "Type to filter by number or effect" );
	EM_List( &em.activeList, em.numActiveView, EM_FOLDER_X, EM_LIST_Y, EM_ACTIVE_W, EM_LIST_H, EM_DrawActiveRow, &clicked );
	if ( clicked >= 0 && clicked < em.numActiveView )
		em.active[em.activeView[clicked]].checked = (qboolean)!em.active[em.activeView[clicked]].checked;
	if ( !em.numActiveView ) {
		EM_Text( EM_FOLDER_X + 4, EM_LIST_Y + 1,
			em.listRequestTime || em.listQueued ? "Asking the server..." : em.numActive ? "No effects match" : "No effects playing", emDim );
	}
	if ( em.activeList.sel < em.numActiveView )
		a = &em.active[em.activeView[em.activeList.sel]];

	const float rx = EM_FOLDER_X + EM_ACTIVE_W + 10, rw = SCREEN_WIDTH - 16 - rx, cx = rx + 66;

	y = EM_SEARCH_Y;
	if ( EM_Button( rx, y, 70, "Refresh", qfalse, qtrue ) )
		EM_RequestList();
	if ( EM_Button( rx + 74, y, 104, "Check on map...", qfalse, (qboolean)( em.numActive > 0 ) ) ) {
		em.state = EM_PICK;
		em.focus = EM_F_NONE;
	}
	if ( em.listTime )
		EM_TextClipped( rx + 184, y + 2, rw - 184, va( S_COLOR_GREY "%i, %is ago", em.numActive, ( cls.realtime - em.listTime ) / 1000 ), emWhite );

	y = EM_LIST_Y;
	EM_Box( rx, y, rw, 34, emPanelLight );
	if ( a ) {
		EM_TextClipped( rx + 6, y + 4, rw - 12, va( S_COLOR_CYAN "%i  " S_COLOR_YELLOW "%s", a->num, a->path ), emWhite );
		EM_TextClipped( rx + 6, y + 18, rw - 12, va( S_COLOR_GREY "at %i %i %i, %i units from you",
			(int)a->origin[0], (int)a->origin[1], (int)a->origin[2], (int)Distance( a->origin, cl.snap.ps.origin ) ), emWhite );
	} else {
		EM_Text( rx + 6, y + 4, "Click effects to check them", emDim );
	}

	w = ( rw - 66 - 8 ) / 3.0f;
	y += 42;
	EM_Label( rx, y, "Check" );
	if ( EM_Button( cx, y, w, "All", qfalse, (qboolean)( em.numActiveView > 0 ) ) ) {
		for ( int i = 0; i < em.numActiveView; i++ )
			em.active[em.activeView[i]].checked = qtrue;
	}
	if ( EM_Button( cx + w + 4, y, w, "None", qfalse, (qboolean)( checked > 0 ) ) ) {
		for ( int i = 0; i < em.numActive; i++ )
			em.active[i].checked = qfalse;
	}
	if ( EM_Button( cx + ( w + 4 ) * 2, y, w, "Invert", qfalse, (qboolean)( em.numActiveView > 0 ) ) ) {
		for ( int i = 0; i < em.numActiveView; i++ )
			em.active[em.activeView[i]].checked = (qboolean)!em.active[em.activeView[i]].checked;
	}

	y += 24;
	EM_Label( rx, y, "Remove" );
	if ( EM_DangerButton( cx, y, rw - 66, checked ? va( "Remove the %i checked", checked ) : "Remove the checked", EM_CONFIRM_REMOVE, (qboolean)( checked > 0 ) ) )
		EM_RemoveChecked();
	y += 20;
	if ( EM_DangerButton( cx, y, rw - 66, "Remove every effect", EM_CONFIRM_REMOVE_ALL, qtrue ) )
		EM_SendAndRefresh( "rpeffect remove all" );

	y += 26;
	EM_Label( rx, y, "Bolted on" );
	if ( EM_Button( cx, y, w, "Me", qfalse, qtrue ) )
		EM_SendAndRefresh( "rpeffect remove bolted" );
	if ( EM_Button( cx + w + 4, y, w, "Player...", qfalse, qtrue ) )
		EM_OpenPicker( EM_PICKER_REMOVE_PLAYER );
	if ( EM_Button( cx + ( w + 4 ) * 2, y, w, "NPC...", qfalse, qtrue ) )
		EM_OpenPicker( EM_PICKER_REMOVE_NPC );
	y += 18;
	EM_TextClipped( cx, y, rw - 66, S_COLOR_GREY "removes the effects bolted to them", emWhite );

	if ( em.lastCmd[0] ) {
		EM_TextClipped( rx, EM_BUTTON_Y - 17, rw,
			va( "%sSent: %s", cls.realtime - em.sentTime < 1500 ? S_COLOR_GREEN : S_COLOR_GREY, em.lastCmd ), emWhite );
	}
	if ( em.numQueue )
		EM_TextClipped( rx, EM_BUTTON_Y, rw, va( S_COLOR_GREY "%i commands waiting for the server's flood protection", em.numQueue ), emWhite );
}

static void EM_DrawPresets( void ) {
	const float x = EM_FOLDER_X, cx = x + 90;
	float y = EM_SEARCH_Y + 4;

	EM_Text( x, y, S_COLOR_GREY "RPMod's ready-made effects, for everyone on the server.", emWhite );

	y += 24;
	EM_Label( x, y, "Alarm" );
	if ( EM_Button( cx, y, 100, "Start", qfalse, qtrue ) )
		EM_Queue( "rpeffect alarm", qtrue );
	if ( EM_Button( cx + 104, y, 100, "Stop", qfalse, qtrue ) )
		EM_Queue( "rpeffect remove alarm", qtrue );
	EM_Text( cx + 214, y + 2, S_COLOR_GREY "full screen red alert, with two background sounds", emWhite );

	y += 22;
	EM_Label( x, y, "Fade out" );
	if ( EM_Button( cx, y, 100, "Fade out", qfalse, qtrue ) )
		EM_Queue( "rpeffect fadeout", qtrue );
	if ( EM_Button( cx + 104, y, 100, "Fade back in", qfalse, qtrue ) )
		EM_Queue( "rpeffect remove fadeout", qtrue );
	EM_Text( cx + 214, y + 2, S_COLOR_GREY "the screen fades to black until it is removed", emWhite );

	y += 22;
	EM_Label( x, y, "Camera shake" );
	EM_Field( EM_F_INTENSITY, cx, y, 60, "intensity" );
	EM_Field( EM_F_DURATION, cx + 64, y, 60, "seconds" );
	if ( EM_Button( cx + 128, y, 76, "Shake", qfalse, (qboolean)( em.fields[EM_F_INTENSITY][0] && em.fields[EM_F_DURATION][0] ) ) )
		EM_Queue( va( "rpeffect camerashake %s %s", em.fields[EM_F_INTENSITY], em.fields[EM_F_DURATION] ), qtrue );
	EM_Text( cx + 214, y + 2, S_COLOR_GREY "intensity, then how long in seconds", emWhite );

	if ( em.lastCmd[0] ) {
		EM_TextClipped( x, y + 34, SCREEN_WIDTH - 32,
			va( "%sSent: %s", cls.realtime - em.sentTime < 1500 ? S_COLOR_GREEN : S_COLOR_GREY, em.lastCmd ), emWhite );
	}
}

// the bolt highlighted in the bolt picker, on the target's model
static void EM_DrawPickerModel( float x, float y, float w, float h ) {
	const char *bolt = em.pickerList.sel < em.numPickerView ? em.bolts[em.pickerView[em.pickerList.sel]].name : NULL;

	EM_Box( x, y, w, h, emPanelLight );
	if ( em.wheel && EM_InRect( x, y, w, h ) ) {
		em.modelYaw += em.wheel * 5.0f;
		em.wheel = 0;
	}
	if ( em.g2 )
		EM_RenderModel( x + 1, y + 1, w - 2, h - 2, bolt );
	EM_TextClipped( x + 6, y + h - 16, w - 12, va( S_COLOR_GREY "%s", em.g2Model ), emWhite );
}

static void EM_DrawPicker( void ) {
	static const char *titles[] = { "", "Bolt to which player?", "Bolt to which NPC?", "Pick a tag or bone",
		"Remove the effects bolted to which player?", "Remove the effects bolted to which NPC?" };
	const qboolean bolts = (qboolean)( em.picker == EM_PICKER_BOLT );
	const float x = bolts ? EM_PICKER_BOLT_X : EM_PICKER_X, y = EM_PICKER_Y, h = EM_PICKER_H;
	const float w = EM_PICKER_W + ( bolts ? EM_PICKER_PREVIEW_W : 0.0f ), lw = EM_PICKER_W - 16;
	int picked;

	EM_Box( x, y, w, h, emPanel );
	EM_Fill( x + 1, y + 1, w - 2, h - 2, emPanel );
	EM_Text( x + 8, y + 6, titles[em.picker], emWhite );
	EM_Field( EM_F_PICKER_SEARCH, x + 8, y + 22, lw, "Type to search" );
	if ( !bolts )
		EM_RebuildPickerView();	// people and NPCs come and go
	picked = EM_List( &em.pickerList, em.numPickerView, x + 8, y + 42, lw, h - 74, EM_DrawPickerRow, NULL );
	if ( !em.numPickerView ) {
		EM_Text( x + 12, y + 43, ( em.picker == EM_PICKER_NPC || em.picker == EM_PICKER_REMOVE_NPC ) && CL_NpcManager_Waiting()
			? "Asking the server..." : "Nothing to pick", emDim );
	}
	if ( bolts )
		EM_DrawPickerModel( x + lw + 16, y + 22, EM_PICKER_PREVIEW_W - 8, h - 54 );
	if ( EM_Button( x + 8, y + h - 24, 100, "Choose  (Enter)", qfalse, (qboolean)( em.numPickerView > 0 ) ) )
		picked = em.pickerList.sel;
	if ( EM_Button( x + w - 108, y + h - 24, 100, "Cancel  (Esc)", qfalse, qtrue ) ) {
		em.picker = EM_PICKER_NONE;
		return;
	}
	if ( picked >= 0 && picked < em.numPickerView )
		EM_ChoosePicker( em.pickerView[picked] );
	else if ( EM_Clicked( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT ) && !EM_InRect( x, y, w, h ) )
		em.picker = EM_PICKER_NONE;
}

static void EM_DrawBrowse( void ) {
	char buf[MAX_STRING_CHARS];
	const qboolean window = (qboolean)( em.tab == EM_TAB_EFFECTS && em.where != EM_WHERE_BOLT );

	// the picker sits on top and gets the clicks first, but is drawn last
	qboolean pickerOpen = (qboolean)( em.picker != EM_PICKER_NONE );
	qboolean click = em.click;
	int wheel = em.wheel;
	if ( pickerOpen )
		em.click = qfalse, em.wheel = 0;

	// dim everything but the preview window, where the world shows through
	if ( window ) {
		EM_Fill( 0, 0, SCREEN_WIDTH, EM_VIEW_Y, emPanel );
		EM_Fill( 0, EM_VIEW_Y + EM_VIEW_H, SCREEN_WIDTH, SCREEN_HEIGHT - EM_VIEW_Y - EM_VIEW_H, emPanel );
		EM_Fill( 0, EM_VIEW_Y, EM_RIGHT_X, EM_VIEW_H, emPanel );
		EM_Fill( EM_RIGHT_X + EM_RIGHT_W, EM_VIEW_Y, SCREEN_WIDTH - EM_RIGHT_X - EM_RIGHT_W, EM_VIEW_H, emPanel );
	} else {
		EM_Fill( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, emPanel );
	}

	EM_Text( EM_FOLDER_X, 14, "Effect Manager", emWhite );
	if ( EM_Button( 112, 12, 64, "Effects", (qboolean)( em.tab == EM_TAB_EFFECTS ), qtrue ) )
		EM_SetTab( EM_TAB_EFFECTS );
	if ( EM_Button( 180, 12, 64, "Active", (qboolean)( em.tab == EM_TAB_ACTIVE ), qtrue ) )
		EM_SetTab( EM_TAB_ACTIVE );
	if ( EM_Button( 248, 12, 64, "Presets", (qboolean)( em.tab == EM_TAB_PRESETS ), qtrue ) )
		EM_SetTab( EM_TAB_PRESETS );

	// the server's answer to the last command
	if ( em.reply[0] && cls.realtime - em.replyTime < 8000 )
		EM_TextClipped( EM_RIGHT_X, 14, EM_RIGHT_W, va( S_COLOR_YELLOW "Server: " S_COLOR_WHITE "%s", em.reply ), emWhite );

	switch ( em.tab ) {
	case EM_TAB_EFFECTS:	EM_DrawEffects();	break;
	case EM_TAB_ACTIVE:		EM_DrawActive();	break;
	default:				EM_DrawPresets();	break;
	}

	if ( em.tab == EM_TAB_EFFECTS )
		EM_Text( EM_FOLDER_X, 440, S_COLOR_GREY "Up/Down effect   Left/Right folder   Enter send   Double-click place/view in world   Tab switch tab", emWhite );
	else
		EM_Text( EM_FOLDER_X, 440, S_COLOR_GREY "Up/Down select   Wheel scroll   Tab switch tab   Click a box to type in it", emWhite );
	Com_sprintf( buf, sizeof( buf ), S_COLOR_GREY "Esc back / close   %s",
		Key_GetKey( EM_TOGGLE_CMD ) >= 0 ? "Your effectmanager bind closes" : "Tip: bind a key to effectmanager" );
	EM_Text( EM_FOLDER_X, 452, buf, emWhite );

	if ( pickerOpen && em.picker != EM_PICKER_NONE ) {
		em.click = click;
		em.wheel = wheel;
		EM_DrawPicker();
	}

	// a click on nothing leaves the text box
	if ( em.click )
		em.focus = EM_F_NONE;
	em.click = qfalse;
	em.wheel = 0;

	re->DrawStretchPic( em.cursorX, em.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

static void EM_DrawPlace( void ) {
	static const char *help[] = {
		"LMB / Enter    send",
		"RMB / F        lock or follow crosshair",
		"Wheel          yaw",
		"Z / X          pitch",
		"Q / E          roll",
		"R              straight up (no angles)",
		"N              face away from surfaces",
		"M              once / loop",
		"G              cycle snap",
		"Arrows PgUp/Dn nudge (locks)",
		"WASD Space C   fly (Shift fast, Ctrl slow)",
		"Tab / Esc      back to the manager",
		"H              hide this help",
	};
	static const char *boltHelp[] = {
		"LMB / Enter    send",
		"M              once / loop",
		"WASD Space C   fly (Shift fast, Ctrl slow)",
		"Tab / Esc      back to the manager",
		"H              hide this help",
	};
	const qboolean bolted = (qboolean)( em.where == EM_WHERE_BOLT );
	const float x = 10, w = 260, cx = SCREEN_WIDTH * 0.5f, cy = SCREEN_HEIGHT * 0.5f;
	const emEffect_t *e = EM_Effect();
	const char *cmd = EM_BuildCommand();
	float y = 10;
	vec3_t angles;
	int lines = bolted ? 5 : 6;

	if ( !bolted ) {
		EM_Fill( cx - 6, cy - 0.5f, 12, 1, emWhite );
		EM_Fill( cx - 0.5f, cy - 6, 1, 12, emWhite );
	}

	EM_Box( x, y, w, lines * EM_ROW_H + 8, emPanel );
	y += 4;
	if ( bolted )
		EM_Text( x + 6, y, va( "Effect Manager  " S_COLOR_CYAN "BOLTED  %s", em.loop ? "loop" : "once" ), emWhite );
	else
		EM_Text( x + 6, y, va( "Effect Manager  %s  " S_COLOR_WHITE "%s", em.locked ? S_COLOR_YELLOW "LOCKED" : S_COLOR_GREEN "FOLLOWING", em.loop ? "loop" : "once" ), emWhite );
	y += EM_ROW_H;
	EM_TextClipped( x + 6, y, w - 12, e ? e->path : "", emAccent ); y += EM_ROW_H;
	if ( bolted ) {
		EM_TextClipped( x + 6, y, w - 12, va( "On   %s", EM_TargetLabel() ), emWhite ); y += EM_ROW_H;
		EM_TextClipped( x + 6, y, w - 12, va( "Bolt %s", em.fields[EM_F_BOLT] ), emWhite ); y += EM_ROW_H;
		EM_TextClipped( x + 6, y, w - 12, em.playError ? va( S_COLOR_RED "%s", em.playError ) : va( S_COLOR_GREY "%s", em.g2Model ), emWhite ); y += EM_ROW_H;
	} else {
		EM_GetAngles( angles );
		EM_Text( x + 6, y, va( "Origin  %i %i %i", (int)floorf( em.origin[0] + 0.5f ), (int)floorf( em.origin[1] + 0.5f ), (int)floorf( em.origin[2] + 0.5f ) ), emWhite ); y += EM_ROW_H;
		EM_Text( x + 6, y, EM_HaveAngles() ? va( "Angles  %i %i %i%s", EM_NormalizeAngle( angles[PITCH] ), EM_NormalizeAngle( angles[YAW] ), EM_NormalizeAngle( angles[ROLL] ), em.align ? S_COLOR_GREY "  (surface)" : "" )
			: "Angles  straight up", emWhite ); y += EM_ROW_H;
		EM_Text( x + 6, y, va( "Snap    %i deg / %i units", emSnaps[em.snap].angle, emSnaps[em.snap].grid ), emWhite ); y += EM_ROW_H;
		if ( em.playError )
			EM_TextClipped( x + 6, y, w - 12, em.playError, emRed );
		y += EM_ROW_H;
	}

	if ( !em.hideHelp ) {
		const char **lines2 = bolted ? boltHelp : help;
		const int count = bolted ? (int)ARRAY_LEN( boltHelp ) : (int)ARRAY_LEN( help );

		y += 8;
		EM_Box( x, y, w, count * EM_ROW_H + 8, emPanel );
		y += 4;
		for ( int i = 0; i < count; i++, y += EM_ROW_H )
			EM_Text( x + 6, y, lines2[i], emDim );
	}

	// what will be sent, and what was sent last
	EM_Box( 10, SCREEN_HEIGHT - 40, SCREEN_WIDTH - 20, 30, emPanel );
	EM_TextClipped( 16, SCREEN_HEIGHT - 37, SCREEN_WIDTH - 32, cmd ? cmd : S_COLOR_GREY "Nothing to send yet", emWhite );
	if ( em.lastCmd[0] ) {
		EM_TextClipped( 16, SCREEN_HEIGHT - 24, SCREEN_WIDTH - 32,
			va( "%sSent: %s", cls.realtime - em.sentTime < 1500 ? S_COLOR_GREEN : S_COLOR_GREY, em.lastCmd ), emWhite );
	}
}

// numbers over the active effects in view, checked ones highlighted
static void EM_DrawPick( void ) {
	float x, y, tw;

	for ( int i = 0; i < em.numActive; i++ ) {
		const emActive_t *a = &em.active[i];
		const char *label = va( "%i %s", a->num, a->path );

		if ( !EM_Project( a->origin, &x, &y ) || x < 0 || x > SCREEN_WIDTH || y < 0 || y > SCREEN_HEIGHT )
			continue;
		tw = EM_TextWidth( label );
		EM_Fill( x - 2, y - 2, 4, 4, a->checked ? emAccent : emWhite );
		EM_Box( x - tw * 0.5f - 3, y - 20, tw + 6, 14, a->checked ? emHighlight : emPanel );
		EM_Text( x - tw * 0.5f, y - 19, label, a->checked ? emAccent : emWhite );
	}

	EM_Box( 10, 10, SCREEN_WIDTH - 20, 18, emPanel );
	EM_TextClipped( 16, 13, SCREEN_WIDTH - 32, va( "Click an effect to check or uncheck it (%i checked).   Right-click, Enter or Esc goes back", EM_CountChecked() ), emWhite );
	re->DrawStretchPic( em.cursorX, em.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

// drawn over the cgame, under the UI and console
void CL_EffectManager_Draw( void ) {
	switch ( em.state ) {
	case EM_BROWSE:	EM_DrawBrowse();	break;
	case EM_PLACE:	EM_DrawPlace();		break;
	case EM_PICK:	EM_DrawPick();		break;
	default:		break;
	}
}
