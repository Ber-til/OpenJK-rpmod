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

// cl_mapeditor.cpp -- a map editor on this screen
//
// A free camera over the map, the panel on the right in tabs:
// Map: the map's own entities, triggers boxed in orange and brush entities in blue, with
// all their keys; a brush entity can be hidden.
// Models: models placed here, carried to the crosshair, turned and scaled; and the map's
// own props. Those were melted into the map when it was built, so a click finds the
// triangles under the cursor and the ones joined to them; lifted out, they're hidden in
// the world and drawn as a model of their own, with the light they had.
// Lights: dynamic lights, their colour and reach, and a light style they can follow.
// Triggers: boxes that run a console command when the player walks in.
// Sounds: speakers playing a sound or a piece of music round and round, heard near them
// or everywhere on the map, as a target_speaker would; saved as one.
// Styles: the map's light styles, the switchable lights it was built with, given a
// pattern and a colour of our own.
//
// RPMod has no server side for any of it yet, so only this screen sees the edits. They
// are kept in mapedits/<map>.ent, written as entities a server could spawn later, and
// come back with the map whenever rpc_mapEdits is on. Layouts are more of them kept by
// name, in mapedits/<map>/<name>.ent, to load when wanted.
//
// Shared, an admin's edits go to everyone on the map with this client, hidden in rpnotify
// prints (see cl_rphooks.cpp):
//   cmd:map/begin/<id>/<parts>: <map>      a batch of edits for that map
//   cmd:map/add/<id>/<part>: <edit> | <edit> | ...
//   cmd:map/end/<id>/<parts>               shown in place of the player's own until
//                                          cmd:map/clear: <map>, or another map
// An edit is m (a model), p (a map prop), l (a light), a (a speaker), s (a light style) or
// h (a hidden brush entity), then its numbers; a model's path and a style's letters come last, with
// spaces put in so no word is long, and taken out again. Triggers stay with whoever made
// them: a print never runs a command. Only what's drawn changes: walls hidden still block,
// props moved don't, as the server knows nothing of it. Players who join later see the
// edits once they're shared again.

#include "client.h"
#include "qcommon/cm_public.h"

#define ME_TOGGLE_CMD		"mapedit"
#define ME_DIR				"mapedits"
#define ME_EXT				".ent"
#define ME_HEADER			"// map edits, made with /mapedit; only this screen sees them for now\n"
#define ME_MAX_LAYOUTS		64
#define ME_SHARE_HOOK		"map"
#define ME_SHARE_LEN		700			// a hook's edits, well inside what RPMod passes on
#define ME_SHARE_WORD		32			// longer words go in pieces: RPMod chokes on long ones
#define ME_MAX_SHARE		96			// hooks waiting to go
#define ME_SHARE_BUF		( ME_MAX_SHARE * ( ME_SHARE_LEN + 4 ) )

#define ME_MAX_ENTS			2048
#define ME_MAX_KV			64
#define ME_MAX_ITEMS		256
#define ME_MAX_HIDDEN		64
#define ME_MAX_NAMES		16384
#define ME_NAME_POOL		( ME_MAX_NAMES * 40 )
#define ME_CMD_LEN			256

#define ME_FLY_SPEED		400.0f
#define ME_PANEL_W			236.0f
#define ME_PANEL_X			( SCREEN_WIDTH - ME_PANEL_W )
#define ME_ROW_H			12.0f
#define ME_CTRL_H			15.0f
#define ME_GAP				3.0f
#define ME_TEXT_SCALE		0.7f
#define ME_MESSAGE_MS		4000
#define ME_TRACE_DIST		8192.0f
#define ME_DRAW_DIST		3000.0f		// map entities further than this aren't drawn
#define ME_LABEL_DIST		700.0f		// nor named
#define ME_MAX_LINES		1100
#define ME_MAX_LABELS		48
#define ME_POINT_SIZE		8.0f		// a point entity's cross, and what a click finds it by
#define ME_FLASH_MS			3000
#define ME_MAX_SEEDS		32			// pieces a map prop is found from
#define ME_PROP_MAX_TRIS	20000		// bigger than this it's the map, not a prop
#define ME_PROP_MAX_SIZE	1024.0f
#define ME_MAX_SOUNDS		8192		// sounds and music to pick from
#define ME_SOUND_POOL		( ME_MAX_SOUNDS * 48 )
#define ME_SOUND_NEAR		256.0f		// a speaker's sound is at its loudest this near

typedef enum {
	ME_TAB_MAP,
	ME_TAB_MODELS,
	ME_TAB_LIGHTS,
	ME_TAB_TRIGGERS,
	ME_TAB_SOUNDS,
	ME_TAB_STYLES,
	ME_NUM_TABS
} meTab_t;

static const char *meTabNames[ME_NUM_TABS] = { "Map", "Models", "Lights", "Triggers", "Sounds", "Styles" };

// what can be chosen
typedef enum {
	ME_SEL_NONE,
	ME_SEL_ENT,			// one of the map's own
	ME_SEL_ITEM,		// one of ours
	ME_SEL_STYLE,
	ME_SEL_PROP			// a prop of the map's, not lifted yet
} meSelKind_t;

typedef enum {
	ME_ENT_POINT,
	ME_ENT_TRIGGER,
	ME_ENT_BRUSH
} meEntKind_t;

// the map's entities, from the bsp's entity string
typedef struct {
	int			ofs;				// where its { is in the string
	char		classname[48];
	char		targetname[48];
	int			kind;
	int			bmodel;				// its *N, 0 for none
	qboolean	hasOrigin;
	vec3_t		origin;
	vec3_t		mins, maxs;			// in the world, for a brush entity
	qboolean	hidden;
} meEnt_t;

typedef enum {
	ME_ITEM_MODEL,
	ME_ITEM_LIGHT,
	ME_ITEM_TRIGGER,
	ME_ITEM_PROP,		// a prop of the map's, lifted out
	ME_ITEM_SPEAKER		// a sound playing round and round
} meItemKind_t;

typedef struct {
	int			kind;
	vec3_t		org;

	// a model
	char		model[MAX_QPATH];	// "models/....md3"
	vec3_t		ang;
	float		scale;
	qhandle_t	h;
	qboolean	registered;

	// a light; it follows a light style unless that's -1
	vec3_t		color;
	float		radius;
	int			style;

	// a speaker: its sound, heard everywhere or fading with distance, playing or not
	char		noise[MAX_QPATH];
	qboolean	global;
	qboolean	off;
	sfxHandle_t	sfx;
	qboolean	sfxRegistered;

	// a trigger: a box that size either way from org
	vec3_t		size;
	char		command[ME_CMD_LEN];
	qboolean	once;
	qboolean	inside, fired;

	// a map prop: the pieces it's found from, surface and triangle, where it was, and
	// whether it's gone; org, ang and scale are a model's
	int			seeds[ME_MAX_SEEDS][2];
	int			numSeeds;
	vec3_t		pivot;
	qboolean	removed;
} meItem_t;

// a triangle of the world: its surface, which of the surface's it is, and its corners
typedef struct {
	int			surf, local;
	int			v[3];
} meTri_t;

// a corner where it is, to find the triangles that share it
typedef struct {
	int			x, y, z;
	int			tri;
} meVertKey_t;

typedef struct {
	qboolean	override;
	char		pattern[MAX_QPATH];	// a dark .. z full, one letter each 50 ms
	vec3_t		color;

	// the map's surfaces lit by it
	int			surfaces;
	vec3_t		mins, maxs;
} meStyle_t;

typedef struct {
	const char	*name;
	const char	*pattern;
} meStylePreset_t;

static const meStylePreset_t meStylePresets[] = {
	{ "Steady",		"z" },
	{ "Off",		"a" },
	{ "Half",		"m" },
	{ "Flicker",	"mmnmmommommnonmmonqnmmo" },
	{ "Candle",		"mmmmmaaaaammmmmaaaaaabcdefgabcdefg" },
	{ "Pulse",		"abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcb" },
	{ "Strobe",		"mamamamamama" },
	{ "Blink",		"zzzzzzzzzzaaaaaaaaaa" },
};

static const vec3_t meLightColors[] = {
	{ 1.0f, 1.0f, 1.0f },
	{ 1.0f, 0.8f, 0.55f },
	{ 0.6f, 0.75f, 1.0f },
	{ 1.0f, 0.2f, 0.15f },
	{ 0.2f, 1.0f, 0.3f },
	{ 0.2f, 0.4f, 1.0f },
	{ 1.0f, 0.4f, 1.0f },
};

// a text box being typed in
typedef enum {
	ME_FIELD_NONE,
	ME_FIELD_FILTER,		// the map's entities, by class or name
	ME_FIELD_SEARCH,		// the models
	ME_FIELD_COMMAND,		// a trigger's
	ME_FIELD_PATTERN,		// a light style's
	ME_FIELD_LAYOUT,		// a layout's name
	ME_FIELD_SOUND			// a speaker's sound
} meField_t;

typedef struct {
	qboolean	open;
	int			font;
	int			tab;

	// the map the edits are for, "" before one is loaded
	char		map[MAX_QPATH];
	qboolean	reparse;			// the cgame restarted: the map's own are read again
	const char	*entString;
	meEnt_t		ents[ME_MAX_ENTS];
	int			numEnts;
	qboolean	showTriggers, showBrushes, showPoints;
	char		filter[32];
	int			entScroll;
	char		kv[ME_MAX_KV][2][MAX_QPATH];
	int			numKv, kvScroll, kvEnt;

	// ours
	meItem_t	items[ME_MAX_ITEMS];
	int			numItems;
	int			itemScroll[ME_NUM_TABS];
	meStyle_t	styles[MAX_LIGHT_STYLES];
	qboolean	allStyles;
	int			styleScroll;
	int			flashStyle, flashUntil;
	qhandle_t	hidden[ME_MAX_HIDDEN];
	int			numHidden;
	qboolean	dirty;

	int			selKind, sel;
	int			hoverKind, hover;
	qboolean	grabbing;
	vec3_t		grabFrom;

	// the free camera
	vec3_t		camOrg, camAng;
	float		camFov;
	qboolean	flying;
	qboolean	held[MAX_KEYS];
	int			lastFrameTime;
	qboolean	haveAim;
	vec3_t		aim, aimNormal;

	// the player's view, to start from
	qboolean	havePlayerView;
	vec3_t		playerOrg, playerAng;

	// input; clicks and the wheel go to the control drawn under them
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;
	int			wheel;
	int			field;

	// models to choose from
	qboolean	indexed;
	char		*names[ME_MAX_NAMES];
	int			numNames;
	int			poolUsed;
	char		search[48];
	int			searchScroll;
	char		pickModel[MAX_QPATH];

	// sounds and music to choose from
	qboolean	soundsIndexed;
	char		*sounds[ME_MAX_SOUNDS];
	int			numSounds;
	int			soundPoolUsed;
	int			soundScroll;

	// the view drawn, for picking and labels
	qboolean	haveView;
	vec3_t		viewOrg;
	vec3_t		viewAxis[3];
	float		viewFovX, viewFovY;

	// the world's triangles, for its props
	vec3_t		*bspVerts;
	int			numBspVerts;
	meTri_t		*tris;
	int			numTris;
	int			*surfFirstTri;		// -1 for a surface that isn't in it
	int			numBspSurfs;
	meVertKey_t	*keys;
	int			numKeys;
	int			*triStamp;
	int			stamp;
	byte		*triTaken;			// by a lifted prop
	int			*compTris;			// what was found last
	int			numCompTris;
	qboolean	propsDirty, propsShown;

	// a prop chosen in the map, not lifted yet
	int			candSeeds[ME_MAX_SEEDS][2];
	int			numCandSeeds;
	int			candTris;
	vec3_t		candMins, candMaxs;

	int			numLines;
	char		message[MAX_STRING_CHARS];
	int			messageTime;

	// layouts: edits kept by name, the panel showing them in place of a tab
	qboolean	showLayouts;
	char		layoutName[MAX_QPATH];
	char		layouts[ME_MAX_LAYOUTS][MAX_QPATH];
	int			numLayouts, layoutScroll, layoutSel;
	int			confirmTime;
	qboolean	experimentalBox;	// the warning before turning experimental features on

	// shared: ours going out, a paced hook at a time; someone's coming in
	char		share[ME_MAX_SHARE][ME_SHARE_LEN + 64];
	int			numShare, shareSent, nextShare;
	int			shareId;			// ours, left alone when it comes back
	qboolean	shared;				// what's shown is someone's, not ours
	int			rxId, rxParts, rxGot;
	char		rxBuf[ME_SHARE_BUF];
	int			rxLen;
} meState_t;

static meState_t me;
static char meNamePool[ME_NAME_POOL];
static char meSoundPool[ME_SOUND_POOL];
static cvar_t *rpc_mapEdits;
static cvar_t *rpc_mapShare;

static vec4_t meWhite		= { 1.0f, 1.0f, 1.0f, 1.0f };
static vec4_t mePanel		= { 0.0f, 0.0f, 0.0f, 0.7f };
static vec4_t mePanelLight	= { 0.15f, 0.15f, 0.15f, 0.85f };
static vec4_t meFocus		= { 0.1f, 0.2f, 0.3f, 0.9f };
static vec4_t meHighlight	= { 0.2f, 0.45f, 0.8f, 0.85f };
static vec4_t meHoverBox	= { 0.28f, 0.28f, 0.28f, 0.9f };
static vec4_t meBorder		= { 0.5f, 0.5f, 0.5f, 0.8f };
static vec4_t meDim			= { 0.65f, 0.65f, 0.65f, 1.0f };
static vec4_t mePanelOpaque	= { 0.05f, 0.05f, 0.05f, 0.95f };	// a box over the rest, which would show through

static const byte meTriggerColor[4]	= { 255, 140, 30, 255 };
static const byte meBrushColor[4]	= { 70, 140, 255, 255 };
static const byte mePointColor[4]	= { 90, 230, 110, 255 };
static const byte meHiddenColor[4]	= { 120, 120, 120, 160 };
static const byte meItemColor[4]	= { 60, 220, 255, 255 };
static const byte meOurTrigger[4]	= { 255, 230, 40, 255 };
static const byte meSelColor[4]		= { 255, 255, 255, 255 };
static const byte meAimColor[4]		= { 255, 140, 40, 255 };
static const byte meReachColor[4]	= { 255, 255, 255, 70 };
static const byte meCandColor[4]	= { 255, 90, 200, 255 };
static const byte meSpeakerColor[4]	= { 190, 120, 255, 255 };

static int ME_Round( float f ) {
	return (int)( f < 0 ? f - 0.5f : f + 0.5f );
}

static void ME_Message( const char *text ) {
	Q_strncpyz( me.message, text, sizeof( me.message ) );
	me.messageTime = cls.realtime;
}

static void ME_Changed( void ) {
	me.dirty = qtrue;
}

// the edits show and run outside the editor too, unless rpc_mapEdits is off
static qboolean ME_Shown( void ) {
	return (qboolean)( me.open || ( rpc_mapEdits && rpc_mapEdits->integer ) );
}

static meItem_t *ME_SelectedItem( void ) {
	if ( me.selKind != ME_SEL_ITEM || me.sel < 0 || me.sel >= me.numItems )
		return NULL;
	return &me.items[me.sel];
}

static meEnt_t *ME_SelectedEnt( void ) {
	if ( me.selKind != ME_SEL_ENT || me.sel < 0 || me.sel >= me.numEnts )
		return NULL;
	return &me.ents[me.sel];
}

static int ME_ItemTab( int kind ) {
	return kind == ME_ITEM_MODEL || kind == ME_ITEM_PROP ? ME_TAB_MODELS : kind == ME_ITEM_LIGHT ? ME_TAB_LIGHTS
		: kind == ME_ITEM_SPEAKER ? ME_TAB_SOUNDS : ME_TAB_TRIGGERS;
}

static int ME_TabItemKind( int tab ) {
	return tab == ME_TAB_MODELS ? ME_ITEM_MODEL : tab == ME_TAB_LIGHTS ? ME_ITEM_LIGHT : tab == ME_TAB_SOUNDS ? ME_ITEM_SPEAKER : ME_ITEM_TRIGGER;
}

/*
===============================================================================

The map's own entities

===============================================================================
*/

static void ME_ParseVector( const char *s, vec3_t v ) {
	VectorClear( v );
	sscanf( s, "%f %f %f", &v[0], &v[1], &v[2] );
}

static void ME_ParseEnts( void ) {
	const char *p = CM_EntityString(), *start;
	const int numModels = CM_NumInlineModels();
	char key[MAX_TOKEN_CHARS], *token;

	me.entString = p;
	me.numEnts = 0;
	me.kvEnt = -1;
	if ( !p )
		return;
	COM_BeginParseSession( "mapedit" );
	while ( me.numEnts < ME_MAX_ENTS ) {
		meEnt_t *e = &me.ents[me.numEnts];

		start = p;
		token = COM_ParseExt( &p, qtrue );
		if ( !token[0] || strcmp( token, "{" ) )
			break;
		memset( e, 0, sizeof( *e ) );
		while ( *start && *start != '{' )
			start++;
		e->ofs = start - me.entString;
		for ( ;; ) {
			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] || !strcmp( token, "}" ) )
				break;
			Q_strncpyz( key, token, sizeof( key ) );
			token = COM_ParseExt( &p, qtrue );
			if ( !Q_stricmp( key, "classname" ) )
				Q_strncpyz( e->classname, token, sizeof( e->classname ) );
			else if ( !Q_stricmp( key, "targetname" ) )
				Q_strncpyz( e->targetname, token, sizeof( e->targetname ) );
			else if ( !Q_stricmp( key, "origin" ) ) {
				ME_ParseVector( token, e->origin );
				e->hasOrigin = qtrue;
			} else if ( !Q_stricmp( key, "model" ) && token[0] == '*' )
				e->bmodel = atoi( token + 1 );
		}
		if ( e->bmodel > 0 && e->bmodel < numModels ) {
			CM_ModelBounds( CM_InlineModel( e->bmodel ), e->mins, e->maxs );
			// with an origin the brushes are around it
			if ( e->hasOrigin ) {
				VectorAdd( e->mins, e->origin, e->mins );
				VectorAdd( e->maxs, e->origin, e->maxs );
			}
			e->kind = Q_stricmpn( e->classname, "trigger_", 8 ) ? ME_ENT_BRUSH : ME_ENT_TRIGGER;
		} else {
			e->bmodel = 0;
			e->kind = Q_stricmpn( e->classname, "trigger_", 8 ) ? ME_ENT_POINT : ME_ENT_TRIGGER;
		}
		me.numEnts++;
	}
}

// the chosen entity's keys, read again from the string
static void ME_ReadKeys( int index ) {
	const char *p;
	char *token;

	me.numKv = 0;
	me.kvScroll = 0;
	me.kvEnt = index;
	if ( index < 0 || index >= me.numEnts || me.entString != CM_EntityString() )
		return;
	p = me.entString + me.ents[index].ofs;
	COM_BeginParseSession( "mapedit" );
	token = COM_ParseExt( &p, qtrue );
	if ( strcmp( token, "{" ) )
		return;
	while ( me.numKv < ME_MAX_KV ) {
		token = COM_ParseExt( &p, qtrue );
		if ( !token[0] || !strcmp( token, "}" ) )
			break;
		Q_strncpyz( me.kv[me.numKv][0], token, MAX_QPATH );
		token = COM_ParseExt( &p, qtrue );
		Q_strncpyz( me.kv[me.numKv][1], token, MAX_QPATH );
		me.numKv++;
	}
}

static void ME_EntCenter( const meEnt_t *e, vec3_t out ) {
	if ( e->bmodel ) {
		VectorAdd( e->mins, e->maxs, out );
		VectorScale( out, 0.5f, out );
	} else {
		VectorCopy( e->origin, out );
	}
}

static qboolean ME_EntDrawn( const meEnt_t *e ) {
	if ( !e->bmodel && !e->hasOrigin )
		return qfalse;
	if ( e->kind == ME_ENT_TRIGGER )
		return me.showTriggers;
	if ( e->kind == ME_ENT_BRUSH )
		return me.showBrushes;
	return me.showPoints;
}

static qboolean ME_EntMatches( const meEnt_t *e ) {
	char lower[64], name[96];

	if ( !me.filter[0] )
		return qtrue;
	Q_strncpyz( lower, me.filter, sizeof( lower ) );
	Q_strlwr( lower );
	Com_sprintf( name, sizeof( name ), "%s %s", e->classname, e->targetname );
	Q_strlwr( name );
	return (qboolean)( strstr( name, lower ) != NULL );
}

// a brush entity hidden here: the cgame's drawing of it is left out
static void ME_RebuildHidden( void ) {
	me.numHidden = 0;
	for ( int i = 0; i < me.numEnts && me.numHidden < ME_MAX_HIDDEN; i++ ) {
		if ( me.ents[i].hidden && me.ents[i].bmodel )
			me.hidden[me.numHidden++] = re->RegisterModel( va( "*%i", me.ents[i].bmodel ) );
	}
}

/*
===============================================================================

The map's props: the world's triangles, and which are joined

===============================================================================
*/

static void ME_FreeTris( void ) {
	void **mem[] = { (void **)&me.bspVerts, (void **)&me.tris, (void **)&me.surfFirstTri, (void **)&me.keys,
		(void **)&me.triStamp, (void **)&me.triTaken, (void **)&me.compTris };

	for ( int i = 0; i < (int)ARRAY_LEN( mem ); i++ ) {
		if ( *mem[i] )
			Z_Free( *mem[i] );
		*mem[i] = NULL;
	}
	me.numBspVerts = me.numTris = me.numBspSurfs = me.numKeys = me.numCompTris = 0;
	me.numCandSeeds = 0;
}

static void ME_KeyFor( const vec3_t p, meVertKey_t *k ) {
	// an eighth of a unit apart is the same corner
	k->x = ME_Round( p[0] * 8.0f );
	k->y = ME_Round( p[1] * 8.0f );
	k->z = ME_Round( p[2] * 8.0f );
}

static int ME_CompareKey( const meVertKey_t *a, const meVertKey_t *b ) {
	if ( a->x != b->x )
		return a->x < b->x ? -1 : 1;
	if ( a->y != b->y )
		return a->y < b->y ? -1 : 1;
	if ( a->z != b->z )
		return a->z < b->z ? -1 : 1;
	return 0;
}

static int QDECL ME_CompareKeys( const void *a, const void *b ) {
	return ME_CompareKey( (const meVertKey_t *)a, (const meVertKey_t *)b );
}

static int QDECL ME_CompareInts( const void *a, const void *b ) {
	return *(const int *)a - *(const int *)b;
}

static qboolean ME_LumpOk( const dheader_t *header, int lump, int len, int size ) {
	const int ofs = LittleLong( header->lumps[lump].fileofs ), filelen = LittleLong( header->lumps[lump].filelen );

	return (qboolean)( ofs >= 0 && filelen >= 0 && ofs + filelen <= len && filelen % size == 0 );
}

// the world's flat and model triangles; curves and the sky aren't props
static void ME_BuildTris( const byte *buf, int len ) {
	const dheader_t *header = (const dheader_t *)buf;
	const dsurface_t *surfs;
	const drawVert_t *verts;
	const int *indexes;
	const dshader_t *shaders;
	const dmodel_t *world;
	int numIndexes, numShaders, first, last, count = 0;

	ME_FreeTris();
	if ( !ME_LumpOk( header, LUMP_SURFACES, len, sizeof( dsurface_t ) ) || !ME_LumpOk( header, LUMP_DRAWVERTS, len, sizeof( drawVert_t ) )
		|| !ME_LumpOk( header, LUMP_DRAWINDEXES, len, sizeof( int ) ) || !ME_LumpOk( header, LUMP_SHADERS, len, sizeof( dshader_t ) )
		|| !ME_LumpOk( header, LUMP_MODELS, len, sizeof( dmodel_t ) ) || !LittleLong( header->lumps[LUMP_MODELS].filelen ) )
		return;
	surfs = (const dsurface_t *)( buf + LittleLong( header->lumps[LUMP_SURFACES].fileofs ) );
	me.numBspSurfs = LittleLong( header->lumps[LUMP_SURFACES].filelen ) / sizeof( dsurface_t );
	verts = (const drawVert_t *)( buf + LittleLong( header->lumps[LUMP_DRAWVERTS].fileofs ) );
	me.numBspVerts = LittleLong( header->lumps[LUMP_DRAWVERTS].filelen ) / sizeof( drawVert_t );
	indexes = (const int *)( buf + LittleLong( header->lumps[LUMP_DRAWINDEXES].fileofs ) );
	numIndexes = LittleLong( header->lumps[LUMP_DRAWINDEXES].filelen ) / sizeof( int );
	shaders = (const dshader_t *)( buf + LittleLong( header->lumps[LUMP_SHADERS].fileofs ) );
	numShaders = LittleLong( header->lumps[LUMP_SHADERS].filelen ) / sizeof( dshader_t );
	world = (const dmodel_t *)( buf + LittleLong( header->lumps[LUMP_MODELS].fileofs ) );
	first = Com_Clampi( 0, me.numBspSurfs, LittleLong( world->firstSurface ) );
	last = Com_Clampi( first, me.numBspSurfs, first + LittleLong( world->numSurfaces ) );

	// which surfaces, and how many triangles
	me.surfFirstTri = (int *)Z_Malloc( Q_max( 1, me.numBspSurfs ) * sizeof( int ), TAG_GENERAL, qfalse );
	for ( int i = 0; i < me.numBspSurfs; i++ ) {
		const dsurface_t *ds = &surfs[i];
		const int type = LittleLong( ds->surfaceType ), shader = LittleLong( ds->shaderNum );
		const int fi = LittleLong( ds->firstIndex ), ni = LittleLong( ds->numIndexes );
		const int fv = LittleLong( ds->firstVert ), nv = LittleLong( ds->numVerts );

		me.surfFirstTri[i] = -1;
		if ( i < first || i >= last || ( type != MST_PLANAR && type != MST_TRIANGLE_SOUP ) )
			continue;
		if ( shader < 0 || shader >= numShaders || ( LittleLong( shaders[shader].surfaceFlags ) & ( SURF_SKY | SURF_NODRAW ) ) )
			continue;
		if ( fi < 0 || ni < 3 || fi + ni > numIndexes || fv < 0 || nv <= 0 || fv + nv > me.numBspVerts )
			continue;
		me.surfFirstTri[i] = count;
		count += ni / 3;
	}
	if ( !count ) {
		ME_FreeTris();
		return;
	}

	me.bspVerts = (vec3_t *)Z_Malloc( me.numBspVerts * sizeof( vec3_t ), TAG_GENERAL, qfalse );
	for ( int i = 0; i < me.numBspVerts; i++ ) {
		me.bspVerts[i][0] = LittleFloat( verts[i].xyz[0] );
		me.bspVerts[i][1] = LittleFloat( verts[i].xyz[1] );
		me.bspVerts[i][2] = LittleFloat( verts[i].xyz[2] );
	}
	me.tris = (meTri_t *)Z_Malloc( count * sizeof( meTri_t ), TAG_GENERAL, qfalse );
	me.keys = (meVertKey_t *)Z_Malloc( count * 3 * sizeof( meVertKey_t ), TAG_GENERAL, qfalse );
	me.triStamp = (int *)Z_Malloc( count * sizeof( int ), TAG_GENERAL, qtrue );
	me.triTaken = (byte *)Z_Malloc( count, TAG_GENERAL, qtrue );
	me.compTris = (int *)Z_Malloc( count * sizeof( int ), TAG_GENERAL, qfalse );
	me.stamp = 0;

	for ( int i = 0; i < me.numBspSurfs; i++ ) {
		const dsurface_t *ds = &surfs[i];
		const int fi = LittleLong( ds->firstIndex ), ni = LittleLong( ds->numIndexes );
		const int fv = LittleLong( ds->firstVert ), nv = LittleLong( ds->numVerts );

		if ( me.surfFirstTri[i] < 0 )
			continue;
		for ( int t = 0; t < ni / 3; t++ ) {
			meTri_t *tri = &me.tris[me.numTris];

			tri->surf = i;
			tri->local = t;
			for ( int c = 0; c < 3; c++ ) {
				const int v = LittleLong( indexes[fi + t * 3 + c] );

				tri->v[c] = fv + ( v >= 0 && v < nv ? v : 0 );
				ME_KeyFor( me.bspVerts[tri->v[c]], &me.keys[me.numKeys] );
				me.keys[me.numKeys++].tri = me.numTris;
			}
			me.numTris++;
		}
	}
	qsort( me.keys, me.numKeys, sizeof( me.keys[0] ), ME_CompareKeys );
}

// the first key at a corner
static int ME_FindKey( const meVertKey_t *k ) {
	int lo = 0, hi = me.numKeys;

	while ( lo < hi ) {
		const int mid = ( lo + hi ) / 2;

		if ( ME_CompareKey( &me.keys[mid], k ) < 0 )
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static int ME_SeedTri( int surf, int local ) {
	int tri;

	if ( !me.tris || surf < 0 || surf >= me.numBspSurfs || me.surfFirstTri[surf] < 0 )
		return -1;
	tri = me.surfFirstTri[surf] + local;
	if ( local < 0 || tri >= me.numTris || me.tris[tri].surf != surf )
		return -1;
	return tri;
}

static qboolean ME_TooBig( const vec3_t mins, const vec3_t maxs ) {
	for ( int i = 0; i < 3; i++ ) {
		if ( maxs[i] - mins[i] > ME_PROP_MAX_SIZE )
			return qtrue;
	}
	return qfalse;
}

// a triangle and all joined to it by corners, onto compTris; qfalse if it's more than a prop
static qboolean ME_Grow( int seed, qboolean skipTaken, vec3_t mins, vec3_t maxs ) {
	int head = me.numCompTris;
	const int start = head;

	if ( me.triStamp[seed] == me.stamp )
		return qtrue;
	me.triStamp[seed] = me.stamp;
	me.compTris[me.numCompTris++] = seed;
	while ( head < me.numCompTris ) {
		const meTri_t *t = &me.tris[me.compTris[head++]];

		for ( int c = 0; c < 3; c++ ) {
			meVertKey_t key;

			AddPointToBounds( me.bspVerts[t->v[c]], mins, maxs );
			ME_KeyFor( me.bspVerts[t->v[c]], &key );
			for ( int k = ME_FindKey( &key ); k < me.numKeys && !ME_CompareKey( &me.keys[k], &key ); k++ ) {
				const int n = me.keys[k].tri;

				if ( me.triStamp[n] == me.stamp || ( skipTaken && me.triTaken[n] ) )
					continue;
				me.triStamp[n] = me.stamp;
				me.compTris[me.numCompTris++] = n;
			}
		}
		if ( me.numCompTris - start > ME_PROP_MAX_TRIS || ME_TooBig( mins, maxs ) )
			return qfalse;
	}
	return qtrue;
}

// the triangles of a prop found from its pieces, in order, onto compTris
static qboolean ME_CollectSeeds( const int ( *seeds )[2], int numSeeds, qboolean skipTaken, vec3_t mins, vec3_t maxs ) {
	me.stamp++;
	me.numCompTris = 0;
	ClearBounds( mins, maxs );
	if ( !me.tris )
		return qfalse;
	for ( int i = 0; i < numSeeds; i++ ) {
		const int tri = ME_SeedTri( seeds[i][0], seeds[i][1] );

		if ( tri >= 0 && !ME_Grow( tri, skipTaken, mins, maxs ) )
			return qfalse;
	}
	qsort( me.compTris, me.numCompTris, sizeof( me.compTris[0] ), ME_CompareInts );
	return (qboolean)( me.numCompTris > 0 );
}

// the nearest of the world's triangles along the ray, -1 none; what's lifted is gone
static int ME_TriHit( const vec3_t org, const vec3_t dir, float *dist ) {
	float best = ME_TRACE_DIST;
	int hit = -1;

	for ( int i = 0; i < me.numTris; i++ ) {
		const meTri_t *t = &me.tris[i];
		vec3_t e1, e2, pv, tv, qv;
		float det, u, v, d;

		if ( me.triTaken[i] )
			continue;
		VectorSubtract( me.bspVerts[t->v[1]], me.bspVerts[t->v[0]], e1 );
		VectorSubtract( me.bspVerts[t->v[2]], me.bspVerts[t->v[0]], e2 );
		CrossProduct( dir, e2, pv );
		det = DotProduct( e1, pv );
		if ( fabsf( det ) < 0.000001f )
			continue;
		VectorSubtract( org, me.bspVerts[t->v[0]], tv );
		u = DotProduct( tv, pv ) / det;
		if ( u < 0.0f || u > 1.0f )
			continue;
		CrossProduct( tv, e1, qv );
		v = DotProduct( dir, qv ) / det;
		if ( v < 0.0f || u + v > 1.0f )
			continue;
		d = DotProduct( e2, qv ) / det;
		if ( d > 0.1f && d < best ) {
			best = d;
			hit = i;
		}
	}
	*dist = best;
	return hit;
}

// what's lifted out is hidden in the world: all of them, each time one changes
static void ME_ApplyProps( void ) {
	int *surfs, *locals, n = 0;

	me.propsDirty = qfalse;
	me.propsShown = ME_Shown();
	if ( !me.tris )
		return;
	memset( me.triTaken, 0, me.numTris );
	if ( me.propsShown ) {
		for ( int i = 0; i < me.numItems; i++ ) {
			const meItem_t *it = &me.items[i];
			vec3_t mins, maxs;

			if ( it->kind != ME_ITEM_PROP || !ME_CollectSeeds( it->seeds, it->numSeeds, qfalse, mins, maxs ) )
				continue;
			for ( int j = 0; j < me.numCompTris; j++ )
				me.triTaken[me.compTris[j]] = 1;
		}
	}
	if ( !re->ext.HideWorldTriangles )
		return;
	for ( int i = 0; i < me.numTris; i++ ) {
		if ( me.triTaken[i] )
			n++;
	}
	surfs = (int *)Z_Malloc( ( n + 1 ) * sizeof( int ) * 2, TAG_GENERAL, qfalse );
	locals = surfs + n + 1;
	n = 0;
	for ( int i = 0; i < me.numTris; i++ ) {
		if ( me.triTaken[i] ) {
			surfs[n] = me.tris[i].surf;
			locals[n++] = me.tris[i].local;
		}
	}
	re->ext.HideWorldTriangles( surfs, locals, n );
	Z_Free( surfs );
}

// a lifted prop's model: the triangles it was, around where it stood
static void ME_BuildProp( meItem_t *it ) {
	vec3_t mins, maxs;
	int *surfs, *locals;

	it->registered = qtrue;
	it->h = 0;
	if ( !re->ext.WorldPieceModel || !ME_CollectSeeds( it->seeds, it->numSeeds, qfalse, mins, maxs ) )
		return;
	VectorSet( it->pivot, ( mins[0] + maxs[0] ) * 0.5f, ( mins[1] + maxs[1] ) * 0.5f, mins[2] );
	surfs = (int *)Z_Malloc( me.numCompTris * sizeof( int ) * 2, TAG_GENERAL, qfalse );
	locals = surfs + me.numCompTris;
	for ( int i = 0; i < me.numCompTris; i++ ) {
		surfs[i] = me.tris[me.compTris[i]].surf;
		locals[i] = me.tris[me.compTris[i]].local;
	}
	it->h = re->ext.WorldPieceModel( surfs, locals, me.numCompTris, it->pivot );
	Z_Free( surfs );
}

/*
===============================================================================

Light styles: which the map's surfaces use, and ours over the server's

===============================================================================
*/

// the bsp's surfaces say which styles light them
static void ME_ScanStyles( void ) {
	const dheader_t *header;
	const dsurface_t *surfs;
	const drawVert_t *verts;
	int len, numSurfs, numVerts;
	byte *buf;

	for ( int i = 0; i < MAX_LIGHT_STYLES; i++ ) {
		me.styles[i].surfaces = 0;
		ClearBounds( me.styles[i].mins, me.styles[i].maxs );
	}
	ME_FreeTris();
	len = FS_ReadFile( cl.mapname, (void **)&buf );
	if ( len < (int)sizeof( dheader_t ) || !buf ) {
		if ( buf )
			FS_FreeFile( buf );
		return;
	}
	header = (const dheader_t *)buf;
	if ( LittleLong( header->ident ) != BSP_IDENT || LittleLong( header->version ) != BSP_VERSION
		|| LittleLong( header->lumps[LUMP_SURFACES].fileofs ) + LittleLong( header->lumps[LUMP_SURFACES].filelen ) > len
		|| LittleLong( header->lumps[LUMP_DRAWVERTS].fileofs ) + LittleLong( header->lumps[LUMP_DRAWVERTS].filelen ) > len ) {
		FS_FreeFile( buf );
		return;
	}
	surfs = (const dsurface_t *)( buf + LittleLong( header->lumps[LUMP_SURFACES].fileofs ) );
	numSurfs = LittleLong( header->lumps[LUMP_SURFACES].filelen ) / sizeof( dsurface_t );
	verts = (const drawVert_t *)( buf + LittleLong( header->lumps[LUMP_DRAWVERTS].fileofs ) );
	numVerts = LittleLong( header->lumps[LUMP_DRAWVERTS].filelen ) / sizeof( drawVert_t );

	for ( int i = 0; i < numSurfs; i++ ) {
		const dsurface_t *s = &surfs[i];
		const int first = LittleLong( s->firstVert ), count = LittleLong( s->numVerts );
		qboolean seen[MAX_LIGHT_STYLES] = { qfalse };

		for ( int j = 0; j < MAXLIGHTMAPS * 2; j++ ) {
			const int style = j < MAXLIGHTMAPS ? s->lightmapStyles[j] : s->vertexStyles[j - MAXLIGHTMAPS];
			meStyle_t *st;

			// 0 is everything's normal light
			if ( style <= LS_NORMAL || style >= MAX_LIGHT_STYLES || seen[style] )
				continue;
			seen[style] = qtrue;
			st = &me.styles[style];
			st->surfaces++;
			if ( first < 0 || count < 0 || first + count > numVerts )
				continue;
			for ( int v = 0; v < count; v++ ) {
				vec3_t p;

				p[0] = LittleFloat( verts[first + v].xyz[0] );
				p[1] = LittleFloat( verts[first + v].xyz[1] );
				p[2] = LittleFloat( verts[first + v].xyz[2] );
				AddPointToBounds( p, st->mins, st->maxs );
			}
		}
	}
	// and its triangles, for the props
	ME_BuildTris( buf, len );
	FS_FreeFile( buf );
}

// the server's pattern for a style, its red one
static const char *ME_ServerPattern( int style ) {
	const int index = CS_LIGHT_STYLES + style * 3;
	const int ofs = cl.gameState.stringOffsets[index];

	return ofs ? cl.gameState.stringData + ofs : "";
}

static int ME_PatternValue( const char *pattern ) {
	const int len = strlen( pattern );
	int c;

	if ( !len )
		return 255;
	c = pattern[( Q_max( 0, cl.serverTime ) / 50 ) % len];
	return Com_Clampi( 0, 255, ( c - 'a' ) * 255 / ( 'z' - 'a' ) );
}

static int ME_PackColor( int r, int g, int b ) {
	byteAlias_t ba;

	ba.b[0] = (byte)Com_Clampi( 0, 255, r );
	ba.b[1] = (byte)Com_Clampi( 0, 255, g );
	ba.b[2] = (byte)Com_Clampi( 0, 255, b );
	ba.b[3] = 255;
	return ba.i;
}

// what the cgame sets a style to each frame, ours instead when it has one
int CL_MapEditor_LightStyle( int style, int color ) {
	const meStyle_t *s;
	int v;

	if ( style < 0 || style >= MAX_LIGHT_STYLES )
		return color;
	// found by blinking
	if ( me.open && style == me.flashStyle && cls.realtime < me.flashUntil ) {
		v = ( ( cls.realtime / 150 ) & 1 ) ? 255 : 0;
		return ME_PackColor( v, v, v );
	}
	s = &me.styles[style];
	if ( !ME_Shown() || !s->override )
		return color;
	v = ME_PatternValue( s->pattern );
	return ME_PackColor( ME_Round( v * s->color[0] ), ME_Round( v * s->color[1] ), ME_Round( v * s->color[2] ) );
}

/*
===============================================================================

Our things

===============================================================================
*/

static void ME_RegisterItem( meItem_t *it ) {
	if ( it->kind == ME_ITEM_PROP && !it->registered ) {
		ME_BuildProp( it );
		return;
	}
	if ( it->kind != ME_ITEM_MODEL || it->registered )
		return;
	it->registered = qtrue;
	it->h = it->model[0] ? re->RegisterModel( it->model ) : 0;
}

static void ME_ForgetHandles( void ) {
	for ( int i = 0; i < me.numItems; i++ ) {
		me.items[i].registered = qfalse;
		me.items[i].h = 0;
		me.items[i].sfxRegistered = qfalse;
		me.items[i].sfx = 0;
	}
	me.numHidden = 0;
	// the renderer's world comes back whole
	me.propsDirty = qtrue;
}

static void ME_Clear( void ) {
	me.numItems = 0;
	for ( int i = 0; i < MAX_LIGHT_STYLES; i++ ) {
		me.styles[i].override = qfalse;
		me.styles[i].pattern[0] = '\0';
		VectorSet( me.styles[i].color, 1, 1, 1 );
	}
	for ( int i = 0; i < me.numEnts; i++ )
		me.ents[i].hidden = qfalse;
	me.numHidden = 0;
	me.selKind = ME_SEL_NONE;
	me.sel = -1;
	me.grabbing = qfalse;
	me.dirty = qfalse;
	me.propsDirty = qtrue;
}

static void ME_Select( int kind, int index ) {
	me.grabbing = qfalse;
	me.selKind = kind;
	me.sel = index;
	if ( kind == ME_SEL_ENT )
		ME_ReadKeys( index );
}

static meItem_t *ME_NewItem( int kind ) {
	meItem_t *it;

	if ( me.numItems >= ME_MAX_ITEMS ) {
		ME_Message( S_COLOR_YELLOW "That's as many as there can be" );
		return NULL;
	}
	it = &me.items[me.numItems];
	memset( it, 0, sizeof( *it ) );
	it->kind = kind;
	it->scale = 1.0f;
	it->style = -1;
	it->radius = 200.0f;
	VectorSet( it->color, 1.0f, 0.8f, 0.55f );
	VectorSet( it->size, 32, 32, 32 );
	it->once = qfalse;
	return it;
}

// where a new thing goes: on what the crosshair is on, or in front of the camera
static void ME_PlaceAt( meItem_t *it, const vec3_t at, const vec3_t normal ) {
	switch ( it->kind ) {
	case ME_ITEM_MODEL:
	case ME_ITEM_PROP:
		VectorCopy( at, it->org );
		break;
	case ME_ITEM_LIGHT:
	case ME_ITEM_SPEAKER:
		VectorMA( at, 16.0f, normal, it->org );
		break;
	default:
		// a box sits on a floor, stands out from a wall
		if ( normal[2] > 0.7f ) {
			VectorCopy( at, it->org );
			it->org[2] += it->size[2];
		} else {
			VectorMA( at, Q_max( it->size[0], it->size[1] ), normal, it->org );
		}
		break;
	}
}

static void ME_AddItem( int kind ) {
	meItem_t *it;
	vec3_t forward, at, normal;
	trace_t tr;

	if ( kind == ME_ITEM_MODEL && !me.pickModel[0] ) {
		ME_Message( S_COLOR_YELLOW "Pick a model from the list first" );
		return;
	}
	if ( !( it = ME_NewItem( kind ) ) )
		return;
	if ( kind == ME_ITEM_MODEL ) {
		Q_strncpyz( it->model, me.pickModel, sizeof( it->model ) );
		it->ang[YAW] = AngleNormalize360( ME_Round( ( me.camAng[YAW] + 180.0f ) / 15.0f ) * 15.0f );
	}
	// what the middle of the screen is on
	AngleVectors( me.camAng, forward, NULL, NULL );
	VectorMA( me.camOrg, ME_TRACE_DIST, forward, at );
	CM_BoxTrace( &tr, me.camOrg, at, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
	if ( tr.fraction < 1.0f && !tr.allsolid ) {
		VectorCopy( tr.endpos, at );
		VectorCopy( tr.plane.normal, normal );
	} else {
		VectorMA( me.camOrg, 128.0f, forward, at );
		VectorSet( normal, 0, 0, 1 );
	}
	ME_PlaceAt( it, at, normal );
	me.numItems++;
	ME_Select( ME_SEL_ITEM, me.numItems - 1 );
	me.tab = ME_ItemTab( kind );
	ME_Changed();
}

static void ME_CopySelected( void ) {
	meItem_t *src = ME_SelectedItem(), *it;

	if ( !src || !( it = ME_NewItem( src->kind ) ) )
		return;
	*it = *src;
	it->org[0] += 16.0f;
	it->org[1] += 16.0f;
	it->inside = it->fired = qfalse;
	me.numItems++;
	ME_Select( ME_SEL_ITEM, me.numItems - 1 );
	ME_Changed();
}

static void ME_DeleteSelected( void ) {
	const meItem_t *it = ME_SelectedItem();

	if ( !it )
		return;
	if ( it->kind == ME_ITEM_PROP )
		me.propsDirty = qtrue;
	memmove( &me.items[me.sel], &me.items[me.sel + 1], ( me.numItems - me.sel - 1 ) * sizeof( me.items[0] ) );
	me.numItems--;
	me.selKind = ME_SEL_NONE;
	me.sel = -1;
	me.grabbing = qfalse;
	ME_Changed();
}

static void ME_Grab( void ) {
	meItem_t *it = ME_SelectedItem();

	if ( me.grabbing ) {
		me.grabbing = qfalse;
		return;
	}
	if ( !it ) {
		ME_Message( S_COLOR_YELLOW "Only what was placed here can be moved" );
		return;
	}
	VectorCopy( it->org, me.grabFrom );
	me.grabbing = qtrue;
}

static void ME_CancelGrab( void ) {
	meItem_t *it = ME_SelectedItem();

	if ( me.grabbing && it )
		VectorCopy( me.grabFrom, it->org );
	me.grabbing = qfalse;
}

// a click on the world found a prop of the map's
static qboolean ME_ChooseProp( int tri ) {
	if ( tri < 0 )
		return qfalse;
	me.candSeeds[0][0] = me.tris[tri].surf;
	me.candSeeds[0][1] = me.tris[tri].local;
	if ( !ME_CollectSeeds( me.candSeeds, 1, qtrue, me.candMins, me.candMaxs ) )
		return qfalse;
	me.numCandSeeds = 1;
	me.candTris = me.numCompTris;
	me.grabbing = qfalse;
	me.selKind = ME_SEL_PROP;
	me.sel = 0;
	me.tab = ME_TAB_MODELS;
	return qtrue;
}

// pieces next to the prop that are inside what it covers: a lamp's glass, a table's legs
static void ME_TakeTouching( void ) {
	meItem_t *it = ME_SelectedItem();
	int ( *seeds )[2] = me.candSeeds, *numSeeds = &me.numCandSeeds, added = 0;
	vec3_t mins, maxs, size;

	if ( it && it->kind == ME_ITEM_PROP ) {
		seeds = it->seeds;
		numSeeds = &it->numSeeds;
	} else if ( me.selKind != ME_SEL_PROP ) {
		return;
	}
	if ( !ME_CollectSeeds( seeds, *numSeeds, qfalse, mins, maxs ) )
		return;
	VectorSubtract( maxs, mins, size );
	for ( int i = 0; i < 3; i++ ) {
		mins[i] -= 8.0f + size[i] * 0.25f;
		maxs[i] += 8.0f + size[i] * 0.25f;
	}
	for ( int i = 0; i < me.numTris && *numSeeds < ME_MAX_SEEDS; i++ ) {
		const meTri_t *t = &me.tris[i];
		const int start = me.numCompTris;
		vec3_t pmins, pmaxs;
		qboolean inside = qtrue;

		// not ours already, not another's
		if ( me.triStamp[i] == me.stamp || me.triTaken[i] )
			continue;
		for ( int c = 0; c < 3 && inside; c++ ) {
			for ( int a = 0; a < 3; a++ ) {
				if ( me.bspVerts[t->v[c]][a] < mins[a] || me.bspVerts[t->v[c]][a] > maxs[a] )
					inside = qfalse;
			}
		}
		if ( !inside )
			continue;
		ClearBounds( pmins, pmaxs );
		if ( ME_Grow( i, qtrue, pmins, pmaxs ) ) {
			for ( int a = 0; a < 3; a++ ) {
				if ( pmins[a] < mins[a] || pmaxs[a] > maxs[a] )
					inside = qfalse;
			}
		} else {
			inside = qfalse;
		}
		if ( !inside ) {
			// stays stamped, so it isn't tried again
			me.numCompTris = start;
			continue;
		}
		seeds[*numSeeds][0] = t->surf;
		seeds[*numSeeds][1] = t->local;
		( *numSeeds )++;
		added++;
	}
	if ( me.selKind == ME_SEL_PROP ) {
		ME_CollectSeeds( me.candSeeds, me.numCandSeeds, qtrue, me.candMins, me.candMaxs );
		me.candTris = me.numCompTris;
	}
	if ( it ) {
		it->registered = qfalse;
		me.propsDirty = qtrue;
		ME_Changed();
	}
	ME_Message( added ? va( "Took %i more piece%s", added, added == 1 ? "" : "s" ) : "Nothing else close enough to take" );
}

// the chosen prop out of the map, a model of its own: where it was, or gone
static void ME_LiftProp( qboolean removed, qboolean grab ) {
	meItem_t *it;

	if ( me.selKind != ME_SEL_PROP )
		return;
	if ( !re->ext.WorldPieceModel || !re->ext.HideWorldTriangles ) {
		ME_Message( S_COLOR_YELLOW "Lifting the map's props needs the vanilla renderer" );
		return;
	}
	if ( !( it = ME_NewItem( ME_ITEM_PROP ) ) )
		return;
	memcpy( it->seeds, me.candSeeds, sizeof( it->seeds ) );
	it->numSeeds = me.numCandSeeds;
	ME_BuildProp( it );
	if ( !it->h ) {
		ME_Message( S_COLOR_YELLOW "Couldn't make a model of it" );
		return;
	}
	VectorCopy( it->pivot, it->org );
	it->removed = removed;
	me.numItems++;
	ME_Select( ME_SEL_ITEM, me.numItems - 1 );
	me.tab = ME_TAB_MODELS;
	me.propsDirty = qtrue;
	ME_Changed();
	ME_Message( removed ? "Removed from the map; Shown brings it back" : "Lifted out: it moves like a model now" );
	if ( grab )
		ME_Grab();
}

/*
===============================================================================

Saving and loading

===============================================================================
*/

// maps/mp/ffa3.bsp: mp/ffa3
static const char *ME_MapBase( void ) {
	static char name[MAX_QPATH];

	Q_strncpyz( name, !Q_stricmpn( cl.mapname, "maps/", 5 ) ? cl.mapname + 5 : cl.mapname, sizeof( name ) );
	COM_StripExtension( name, name, sizeof( name ) );
	return name;
}

// mapedits/mp/ffa3.ent
static const char *ME_Path( void ) {
	return va( ME_DIR "/%s" ME_EXT, ME_MapBase() );
}

// mapedits/mp/ffa3/<name>.ent
static const char *ME_LayoutDir( void ) {
	return va( ME_DIR "/%s", ME_MapBase() );
}

static const char *ME_LayoutPath( const char *name ) {
	return va( ME_DIR "/%s/%s" ME_EXT, ME_MapBase(), name );
}

static qboolean ME_LayoutChar( int ch ) {
	return (qboolean)( isalnum( ch ) || ch == '_' || ch == '-' );
}

static qboolean ME_LayoutNameOk( const char *name ) {
	if ( !name[0] )
		return qfalse;
	for ( ; *name; name++ ) {
		if ( !ME_LayoutChar( *name ) )
			return qfalse;
	}
	return qtrue;
}

static void ME_Print( fileHandle_t f, const char *key, const char *value ) {
	FS_Printf( f, "\"%s\" \"%s\"\n", key, value );
}

static const char *ME_Vec( const vec3_t v ) {
	return va( "%g %g %g", ME_Round( v[0] * 10.0f ) / 10.0f, ME_Round( v[1] * 10.0f ) / 10.0f, ME_Round( v[2] * 10.0f ) / 10.0f );
}

// the edits written to the file; how many, -1 if it couldn't be
static int ME_SaveTo( const char *path ) {
	fileHandle_t f;
	int count = 0;

	if ( !me.map[0] )
		return -1;
	f = FS_FOpenFileWrite( path );
	if ( !f ) {
		ME_Message( S_COLOR_RED "Couldn't write the file" );
		return -1;
	}
	FS_Printf( f, "%s", ME_HEADER );
	for ( int i = 0; i < me.numItems; i++ ) {
		const meItem_t *it = &me.items[i];

		FS_Printf( f, "{\n" );
		if ( it->kind == ME_ITEM_MODEL ) {
			// a misc_model_static is drawn by the cgame from the entity string
			ME_Print( f, "classname", "misc_model_static" );
			ME_Print( f, "model", it->model );
			ME_Print( f, "origin", ME_Vec( it->org ) );
			ME_Print( f, "angles", ME_Vec( it->ang ) );
			if ( it->scale != 1.0f )
				ME_Print( f, "modelscale", va( "%g", it->scale ) );
		} else if ( it->kind == ME_ITEM_PROP ) {
			char seeds[MAX_STRING_CHARS] = "";

			for ( int j = 0; j < it->numSeeds; j++ )
				Q_strcat( seeds, sizeof( seeds ), va( "%s%i:%i", j ? " " : "", it->seeds[j][0], it->seeds[j][1] ) );
			// the map's own, found again by its pieces
			ME_Print( f, "classname", "rpm_prop" );
			ME_Print( f, "seeds", seeds );
			ME_Print( f, "from", ME_Vec( it->pivot ) );
			ME_Print( f, "origin", ME_Vec( it->org ) );
			ME_Print( f, "angles", ME_Vec( it->ang ) );
			if ( it->scale != 1.0f )
				ME_Print( f, "modelscale", va( "%g", it->scale ) );
			if ( it->removed )
				ME_Print( f, "removed", "1" );
		} else if ( it->kind == ME_ITEM_SPEAKER ) {
			// looped on or off, and global: a target_speaker's spawnflags
			ME_Print( f, "classname", "target_speaker" );
			ME_Print( f, "origin", ME_Vec( it->org ) );
			ME_Print( f, "noise", it->noise );
			ME_Print( f, "spawnflags", va( "%i", ( it->off ? 2 : 1 ) | ( it->global ? 4 : 0 ) ) );
		} else if ( it->kind == ME_ITEM_LIGHT ) {
			ME_Print( f, "classname", "rpm_light" );
			ME_Print( f, "origin", ME_Vec( it->org ) );
			ME_Print( f, "radius", va( "%i", ME_Round( it->radius ) ) );
			ME_Print( f, "_color", va( "%g %g %g", it->color[0], it->color[1], it->color[2] ) );
			if ( it->style >= 0 )
				ME_Print( f, "style", va( "%i", it->style ) );
		} else {
			ME_Print( f, "classname", "rpm_trigger" );
			ME_Print( f, "origin", ME_Vec( it->org ) );
			ME_Print( f, "size", ME_Vec( it->size ) );
			ME_Print( f, "command", it->command );
			if ( it->once )
				ME_Print( f, "once", "1" );
		}
		FS_Printf( f, "}\n" );
		count++;
	}
	for ( int i = 0; i < MAX_LIGHT_STYLES; i++ ) {
		const meStyle_t *s = &me.styles[i];

		if ( !s->override )
			continue;
		FS_Printf( f, "{\n" );
		ME_Print( f, "classname", "rpm_lightstyle" );
		ME_Print( f, "style", va( "%i", i ) );
		ME_Print( f, "value", s->pattern );
		ME_Print( f, "_color", va( "%g %g %g", s->color[0], s->color[1], s->color[2] ) );
		FS_Printf( f, "}\n" );
		count++;
	}
	for ( int i = 0; i < me.numEnts; i++ ) {
		if ( !me.ents[i].hidden )
			continue;
		FS_Printf( f, "{\n" );
		ME_Print( f, "classname", "rpm_hide" );
		ME_Print( f, "model", va( "*%i", me.ents[i].bmodel ) );
		FS_Printf( f, "}\n" );
		count++;
	}
	FS_FCloseFile( f );
	return count;
}

// the map's own file, what comes back with it
static qboolean ME_Save( void ) {
	const int count = ME_SaveTo( ME_Path() );

	if ( count < 0 )
		return qfalse;
	me.dirty = qfalse;
	ME_Message( va( S_COLOR_GREEN "Saved %i edits to %s", count, ME_Path() ) );
	return qtrue;
}

typedef struct {
	char	key[MAX_QPATH];
	char	value[ME_CMD_LEN];
} meKv_t;

static const char *ME_Value( const meKv_t *kv, int num, const char *key, const char *def ) {
	for ( int i = 0; i < num; i++ ) {
		if ( !Q_stricmp( kv[i].key, key ) )
			return kv[i].value;
	}
	return def;
}

static void ME_LoadEntity( const meKv_t *kv, int num ) {
	const char *classname = ME_Value( kv, num, "classname", "" );
	meItem_t *it;

	if ( !Q_stricmp( classname, "misc_model_static" ) ) {
		if ( !( it = ME_NewItem( ME_ITEM_MODEL ) ) )
			return;
		Q_strncpyz( it->model, ME_Value( kv, num, "model", "" ), sizeof( it->model ) );
		ME_ParseVector( ME_Value( kv, num, "origin", "0 0 0" ), it->org );
		ME_ParseVector( ME_Value( kv, num, "angles", "0 0 0" ), it->ang );
		it->scale = Com_Clamp( 0.05f, 50.0f, atof( ME_Value( kv, num, "modelscale", "1" ) ) );
		me.numItems++;
	} else if ( !Q_stricmp( classname, "rpm_prop" ) ) {
		const char *p = ME_Value( kv, num, "seeds", "" );

		if ( !( it = ME_NewItem( ME_ITEM_PROP ) ) )
			return;
		while ( *p && it->numSeeds < ME_MAX_SEEDS ) {
			int surf, local;

			while ( *p == ' ' )
				p++;
			if ( sscanf( p, "%i:%i", &surf, &local ) != 2 )
				break;
			it->seeds[it->numSeeds][0] = surf;
			it->seeds[it->numSeeds][1] = local;
			it->numSeeds++;
			while ( *p && *p != ' ' )
				p++;
		}
		ME_ParseVector( ME_Value( kv, num, "from", "0 0 0" ), it->pivot );
		ME_ParseVector( ME_Value( kv, num, "origin", "0 0 0" ), it->org );
		ME_ParseVector( ME_Value( kv, num, "angles", "0 0 0" ), it->ang );
		it->scale = Com_Clamp( 0.05f, 50.0f, atof( ME_Value( kv, num, "modelscale", "1" ) ) );
		it->removed = (qboolean)( atoi( ME_Value( kv, num, "removed", "0" ) ) != 0 );
		if ( it->numSeeds )
			me.numItems++;
	} else if ( !Q_stricmp( classname, "target_speaker" ) ) {
		const int flags = atoi( ME_Value( kv, num, "spawnflags", "1" ) );

		if ( !( it = ME_NewItem( ME_ITEM_SPEAKER ) ) )
			return;
		ME_ParseVector( ME_Value( kv, num, "origin", "0 0 0" ), it->org );
		Q_strncpyz( it->noise, ME_Value( kv, num, "noise", "" ), sizeof( it->noise ) );
		it->off = (qboolean)( ( flags & 2 ) && !( flags & 1 ) );
		it->global = (qboolean)( ( flags & 4 ) != 0 );
		me.numItems++;
	} else if ( !Q_stricmp( classname, "rpm_light" ) ) {
		if ( !( it = ME_NewItem( ME_ITEM_LIGHT ) ) )
			return;
		ME_ParseVector( ME_Value( kv, num, "origin", "0 0 0" ), it->org );
		ME_ParseVector( ME_Value( kv, num, "_color", "1 1 1" ), it->color );
		it->radius = Com_Clamp( 8.0f, 2000.0f, atof( ME_Value( kv, num, "radius", "200" ) ) );
		it->style = Com_Clampi( -1, MAX_LIGHT_STYLES - 1, atoi( ME_Value( kv, num, "style", "-1" ) ) );
		me.numItems++;
	} else if ( !Q_stricmp( classname, "rpm_trigger" ) ) {
		if ( !( it = ME_NewItem( ME_ITEM_TRIGGER ) ) )
			return;
		ME_ParseVector( ME_Value( kv, num, "origin", "0 0 0" ), it->org );
		ME_ParseVector( ME_Value( kv, num, "size", "32 32 32" ), it->size );
		for ( int i = 0; i < 3; i++ )
			it->size[i] = Com_Clamp( 1.0f, 4096.0f, it->size[i] );
		Q_strncpyz( it->command, ME_Value( kv, num, "command", "" ), sizeof( it->command ) );
		it->once = (qboolean)( atoi( ME_Value( kv, num, "once", "0" ) ) != 0 );
		me.numItems++;
	} else if ( !Q_stricmp( classname, "rpm_lightstyle" ) ) {
		const int style = atoi( ME_Value( kv, num, "style", "-1" ) );

		if ( style < 0 || style >= MAX_LIGHT_STYLES )
			return;
		me.styles[style].override = qtrue;
		Q_strncpyz( me.styles[style].pattern, ME_Value( kv, num, "value", "z" ), sizeof( me.styles[style].pattern ) );
		ME_ParseVector( ME_Value( kv, num, "_color", "1 1 1" ), me.styles[style].color );
	} else if ( !Q_stricmp( classname, "rpm_hide" ) ) {
		const char *model = ME_Value( kv, num, "model", "" );
		const int bmodel = model[0] == '*' ? atoi( model + 1 ) : 0;

		for ( int i = 0; i < me.numEnts; i++ ) {
			if ( bmodel && me.ents[i].bmodel == bmodel )
				me.ents[i].hidden = qtrue;
		}
	}
}

// the edits in the file in place of the ones shown; qfalse if there's no file
static qboolean ME_LoadFrom( const char *path ) {
	static meKv_t kv[32];
	const char *p;
	char *buf, *token;
	int num;

	ME_Clear();
	me.shared = qfalse;
	if ( FS_ReadFile( path, (void **)&buf ) <= 0 || !buf ) {
		ME_RebuildHidden();
		return qfalse;
	}
	p = buf;
	COM_BeginParseSession( "mapedit" );
	for ( ;; ) {
		token = COM_ParseExt( &p, qtrue );
		if ( !token[0] || strcmp( token, "{" ) )
			break;
		num = 0;
		for ( ;; ) {
			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] || !strcmp( token, "}" ) )
				break;
			if ( num < (int)ARRAY_LEN( kv ) )
				Q_strncpyz( kv[num].key, token, sizeof( kv[num].key ) );
			token = COM_ParseExt( &p, qtrue );
			if ( num < (int)ARRAY_LEN( kv ) ) {
				Q_strncpyz( kv[num].value, token, sizeof( kv[num].value ) );
				num++;
			}
		}
		ME_LoadEntity( kv, num );
	}
	FS_FreeFile( buf );
	ME_RebuildHidden();
	return qtrue;
}

static void ME_Load( void ) {
	ME_LoadFrom( ME_Path() );
}

/*
===============================================================================

Layouts

===============================================================================
*/

static void ME_ListLayouts( void ) {
	char list[16384], *p;
	int count;

	me.numLayouts = 0;
	if ( !me.map[0] )
		return;
	count = FS_GetFileList( ME_LayoutDir(), ME_EXT, list, sizeof( list ) );
	for ( p = list; count-- > 0 && me.numLayouts < ME_MAX_LAYOUTS; p += strlen( p ) + 1 ) {
		Q_strncpyz( me.layouts[me.numLayouts], p, sizeof( me.layouts[0] ) );
		COM_StripExtension( me.layouts[me.numLayouts], me.layouts[me.numLayouts], sizeof( me.layouts[0] ) );
		if ( me.layouts[me.numLayouts][0] )
			me.numLayouts++;
	}
	if ( me.layoutSel >= me.numLayouts )
		me.layoutSel = -1;
}

static void ME_SaveLayout( const char *name ) {
	int count;

	if ( !ME_LayoutNameOk( name ) ) {
		ME_Message( S_COLOR_YELLOW "Name it with letters, numbers, - and _" );
		return;
	}
	count = ME_SaveTo( ME_LayoutPath( name ) );
	if ( count < 0 )
		return;
	ME_Message( va( S_COLOR_GREEN "Saved %i edits as %s", count, name ) );
	ME_ListLayouts();
}

static void ME_LoadLayout( const char *name ) {
	if ( !ME_LayoutNameOk( name ) || !ME_LoadFrom( ME_LayoutPath( name ) ) ) {
		ME_Message( va( S_COLOR_YELLOW "No layout %s", name ) );
		ME_Load();
		return;
	}
	// what's shown isn't what comes back with the map until it's saved
	me.dirty = qtrue;
	ME_Message( va( S_COLOR_GREEN "Loaded %s: Save keeps it as this map's edits", name ) );
}

static void ME_DeleteLayout( const char *name ) {
	if ( !ME_LayoutNameOk( name ) )
		return;
	FS_HomeRemove( ME_LayoutPath( name ) );
	ME_Message( va( "Deleted %s", name ) );
	ME_ListLayouts();
}

/*
===============================================================================

Sharing: our edits to everyone on the map with this client, theirs to us

===============================================================================
*/

static void ME_CheckMap( void );

// what the server lets through: one command a second, unless it says otherwise
static int ME_CommandGap( void ) {
	const char *info = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];
	const char *flood = Info_ValueForKey( info, "sv_floodProtect" );
	int value;

	if ( !flood[0] )
		return 1100;
	value = atoi( flood );
	if ( value <= 0 )
		return 0;
	return ( value == 1 ? 1000 : value ) + 100;
}

// a word, a space put in after each / and every ME_SHARE_WORD characters
static void ME_AppendSplit( char *out, int size, const char *word ) {
	int len = strlen( out ), run = 0;

	for ( ; *word && len < size - 2; word++ ) {
		out[len++] = *word;
		run++;
		if ( word[1] && ( *word == '/' || run >= ME_SHARE_WORD ) ) {
			out[len++] = ' ';
			run = 0;
		}
	}
	out[len] = '\0';
}

static void ME_QueueShare( const char *hook ) {
	if ( me.numShare < ME_MAX_SHARE )
		Q_strncpyz( me.share[me.numShare++], hook, sizeof( me.share[0] ) );
}

// one edit for a hook; "" if it isn't shared
static const char *ME_ShareItem( const meItem_t *it ) {
	static char out[ME_SHARE_LEN];

	out[0] = '\0';
	if ( it->kind == ME_ITEM_MODEL ) {
		Q_strcat( out, sizeof( out ), va( "m %s", ME_Vec( it->org ) ) );
		Q_strcat( out, sizeof( out ), va( " %s %g ", ME_Vec( it->ang ), it->scale ) );
		ME_AppendSplit( out, sizeof( out ), it->model );
	} else if ( it->kind == ME_ITEM_PROP ) {
		Q_strcat( out, sizeof( out ), va( "p %s", ME_Vec( it->pivot ) ) );
		Q_strcat( out, sizeof( out ), va( " %s", ME_Vec( it->org ) ) );
		Q_strcat( out, sizeof( out ), va( " %s %g %i", ME_Vec( it->ang ), it->scale, it->removed ? 1 : 0 ) );
		for ( int j = 0; j < it->numSeeds; j++ )
			Q_strcat( out, sizeof( out ), va( " %i:%i", it->seeds[j][0], it->seeds[j][1] ) );
	} else if ( it->kind == ME_ITEM_LIGHT ) {
		Q_strcat( out, sizeof( out ), va( "l %s %i %g %g %g %i", ME_Vec( it->org ), ME_Round( it->radius ),
			it->color[0], it->color[1], it->color[2], it->style ) );
	} else if ( it->kind == ME_ITEM_SPEAKER && it->noise[0] ) {
		Q_strcat( out, sizeof( out ), va( "a %s %i ", ME_Vec( it->org ), ( it->off ? 2 : 1 ) | ( it->global ? 4 : 0 ) ) );
		ME_AppendSplit( out, sizeof( out ), it->noise );
	}
	return out;
}

// everything shown, to everyone: begin, the edits a hook at a time, end
static void ME_Share( void ) {
	char part[ME_SHARE_LEN + 1] = "";
	const char *edits[ME_MAX_ITEMS + MAX_LIGHT_STYLES + ME_MAX_HIDDEN];
	static char styleText[MAX_LIGHT_STYLES][MAX_QPATH * 2 + 32];
	static char itemText[ME_MAX_ITEMS][ME_SHARE_LEN];
	static char hideText[ME_MAX_HIDDEN][16];
	int numHide = 0;
	int numEdits = 0, numParts = 0, first;

	if ( cls.state != CA_ACTIVE || !me.map[0] )
		return;
	if ( !CL_RpHooks_Experimental() ) {
		ME_Message( S_COLOR_YELLOW RP_EXPERIMENTAL_OFF ", in Layouts" );
		return;
	}
	if ( me.shareSent < me.numShare ) {
		ME_Message( S_COLOR_YELLOW "Still sending the last share" );
		return;
	}

	for ( int i = 0; i < me.numItems; i++ ) {
		const char *edit = ME_ShareItem( &me.items[i] );

		if ( edit[0] ) {
			Q_strncpyz( itemText[i], edit, sizeof( itemText[i] ) );
			edits[numEdits++] = itemText[i];
		}
	}
	for ( int i = 0; i < MAX_LIGHT_STYLES; i++ ) {
		const meStyle_t *s = &me.styles[i];

		if ( !s->override )
			continue;
		Com_sprintf( styleText[i], sizeof( styleText[i] ), "s %i %g %g %g ", i, s->color[0], s->color[1], s->color[2] );
		ME_AppendSplit( styleText[i], sizeof( styleText[i] ), s->pattern[0] ? s->pattern : "z" );
		edits[numEdits++] = styleText[i];
	}
	for ( int i = 0; i < me.numEnts && numHide < ME_MAX_HIDDEN; i++ ) {
		if ( me.ents[i].hidden && me.ents[i].bmodel ) {
			Com_sprintf( hideText[numHide], sizeof( hideText[0] ), "h %i", me.ents[i].bmodel );
			edits[numEdits++] = hideText[numHide++];
		}
	}

	me.numShare = me.shareSent = 0;
	me.shareId = 1 + ( ( cls.realtime ^ ( rand() << 8 ) ) & 0xffff );
	first = me.numShare;
	ME_QueueShare( "" );		// the begin, once the parts are counted
	for ( int i = 0; i < numEdits; i++ ) {
		if ( part[0] && strlen( part ) + strlen( edits[i] ) + 3 > ME_SHARE_LEN ) {
			ME_QueueShare( va( "cmd:" ME_SHARE_HOOK "/add/%i/%i: %s", me.shareId, ++numParts, part ) );
			part[0] = '\0';
		}
		Q_strcat( part, sizeof( part ), va( "%s%s", part[0] ? " | " : "", edits[i] ) );
	}
	if ( part[0] )
		ME_QueueShare( va( "cmd:" ME_SHARE_HOOK "/add/%i/%i: %s", me.shareId, ++numParts, part ) );
	ME_QueueShare( va( "cmd:" ME_SHARE_HOOK "/end/%i/%i", me.shareId, numParts ) );
	Com_sprintf( me.share[first], sizeof( me.share[0] ), "cmd:" ME_SHARE_HOOK "/begin/%i/%i: %s", me.shareId, numParts, me.map );
	if ( me.numShare == ME_MAX_SHARE ) {
		me.numShare = 0;
		ME_Message( S_COLOR_RED "Too many edits to share at once" );
		return;
	}
	me.nextShare = 0;
	ME_Message( va( S_COLOR_GREEN "Sharing %i edits in %i hooks", numEdits, me.numShare ) );
}

// everyone back to their own edits
static void ME_Unshare( void ) {
	if ( cls.state != CA_ACTIVE || !me.map[0] )
		return;
	if ( !CL_RpHooks_Experimental() ) {
		ME_Message( S_COLOR_YELLOW RP_EXPERIMENTAL_OFF ", in Layouts" );
		return;
	}
	// what's still going isn't wanted now
	me.numShare = me.shareSent = 0;
	ME_QueueShare( va( "cmd:" ME_SHARE_HOOK "/clear: %s", me.map ) );
	me.nextShare = 0;
	ME_Message( "Taking the shared edits away from everyone" );
}

static void ME_RunShare( void ) {
	if ( me.shareSent >= me.numShare || cls.realtime < me.nextShare )
		return;
	CL_AddReliableCommand( va( "rpnotify -1 1 %s", me.share[me.shareSent++] ), qfalse );
	me.nextShare = cls.realtime + ME_CommandGap();
	if ( me.shareSent == me.numShare )
		ME_Message( S_COLOR_GREEN "Shared with everyone on the map with this client" );
}

// the edit's words put back together: spaces were put in to keep them short
static void ME_Unsplit( const char *in, char *out, int size ) {
	int len = 0;

	for ( ; *in && len < size - 1; in++ ) {
		if ( *in != ' ' )
			out[len++] = *in;
	}
	out[len] = '\0';
}

// one shared edit, as an entity from the file
static void ME_ApplyShared( const char *edit ) {
	meKv_t kv[8];
	float f[12];
	int n = 0, used = 0;
	char rest[MAX_QPATH * 2];

	while ( *edit == ' ' )
		edit++;
	memset( kv, 0, sizeof( kv ) );
#define ME_KV( k, v ) ( Q_strncpyz( kv[n].key, k, sizeof( kv[n].key ) ), Q_strncpyz( kv[n].value, v, sizeof( kv[n].value ) ), n++ )
	switch ( *edit ) {
	case 'm':
		if ( sscanf( edit + 1, "%f %f %f %f %f %f %f %n", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &used ) < 7 || !used )
			return;
		ME_Unsplit( edit + 1 + used, rest, sizeof( rest ) );
		ME_KV( "classname", "misc_model_static" );
		ME_KV( "model", rest );
		ME_KV( "origin", va( "%g %g %g", f[0], f[1], f[2] ) );
		ME_KV( "angles", va( "%g %g %g", f[3], f[4], f[5] ) );
		ME_KV( "modelscale", va( "%g", f[6] ) );
		break;
	case 'p':
		if ( sscanf( edit + 1, "%f %f %f %f %f %f %f %f %f %f %f %n", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8],
			&f[9], &f[10], &used ) < 11 || !used )
			return;
		ME_KV( "classname", "rpm_prop" );
		ME_KV( "from", va( "%g %g %g", f[0], f[1], f[2] ) );
		ME_KV( "origin", va( "%g %g %g", f[3], f[4], f[5] ) );
		ME_KV( "angles", va( "%g %g %g", f[6], f[7], f[8] ) );
		ME_KV( "modelscale", va( "%g", f[9] ) );
		ME_KV( "removed", f[10] != 0.0f ? "1" : "0" );
		ME_KV( "seeds", edit + 1 + used );
		break;
	case 'l':
		if ( sscanf( edit + 1, "%f %f %f %f %f %f %f %f", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7] ) < 8 )
			return;
		ME_KV( "classname", "rpm_light" );
		ME_KV( "origin", va( "%g %g %g", f[0], f[1], f[2] ) );
		ME_KV( "radius", va( "%g", f[3] ) );
		ME_KV( "_color", va( "%g %g %g", f[4], f[5], f[6] ) );
		ME_KV( "style", va( "%i", (int)f[7] ) );
		break;
	case 'a':
		if ( sscanf( edit + 1, "%f %f %f %f %n", &f[0], &f[1], &f[2], &f[3], &used ) < 4 || !used )
			return;
		ME_Unsplit( edit + 1 + used, rest, sizeof( rest ) );
		ME_KV( "classname", "target_speaker" );
		ME_KV( "origin", va( "%g %g %g", f[0], f[1], f[2] ) );
		ME_KV( "spawnflags", va( "%i", (int)f[3] ) );
		ME_KV( "noise", rest );
		break;
	case 's':
		if ( sscanf( edit + 1, "%f %f %f %f %n", &f[0], &f[1], &f[2], &f[3], &used ) < 4 || !used )
			return;
		ME_Unsplit( edit + 1 + used, rest, sizeof( rest ) );
		ME_KV( "classname", "rpm_lightstyle" );
		ME_KV( "style", va( "%i", (int)f[0] ) );
		ME_KV( "_color", va( "%g %g %g", f[1], f[2], f[3] ) );
		ME_KV( "value", rest );
		break;
	case 'h':
		if ( sscanf( edit + 1, "%f", &f[0] ) < 1 )
			return;
		ME_KV( "classname", "rpm_hide" );
		ME_KV( "model", va( "*%i", (int)f[0] ) );
		break;
	default:
		return;
	}
#undef ME_KV
	ME_LoadEntity( kv, n );
}

// the batch in, in place of what's shown
static void ME_ApplyBatch( void ) {
	char *p = me.rxBuf, *end;
	int before;

	// ours aren't lost under theirs
	if ( me.dirty && !me.shared ) {
		ME_Save();
		Com_Printf( "Map editor: your edits were saved before the shared ones came in\n" );
	}
	ME_Clear();
	for ( ; *p; p = end ) {
		end = strchr( p, '|' );
		if ( end )
			*end++ = '\0';
		else
			end = p + strlen( p );
		ME_ApplyShared( p );
	}
	ME_RebuildHidden();
	me.shared = qtrue;
	me.dirty = qfalse;
	before = me.rxParts - me.rxGot;
	ME_Message( va( S_COLOR_GREEN "Map edits shared with you: %i%s", me.numItems, before > 0 ? va( S_COLOR_YELLOW ", %i parts missing", before ) : "" ) );
	Com_Printf( "%s\n", me.message );
	me.rxId = 0;
}

// cmd:map/<verb>/<id>/<n>: <text>
void CL_MapEditor_Hook( char **argv, int argc, const char *message ) {
	const char *verb = argc >= 1 ? argv[0] : "";
	const int id = argc >= 2 ? atoi( argv[1] ) : 0;

	if ( !rpc_mapShare->integer || cls.state != CA_ACTIVE )
		return;
	ME_CheckMap();
	if ( !me.map[0] )
		return;

	if ( !Q_stricmp( verb, "clear" ) ) {
		if ( me.shared && !Q_stricmp( message, me.map ) ) {
			ME_Load();
			ME_Message( "The shared map edits were taken away" );
			Com_Printf( "%s\n", me.message );
		}
		return;
	}
	// ours, back from the server
	if ( !id || id == me.shareId )
		return;
	if ( !Q_stricmp( verb, "begin" ) ) {
		me.rxId = Q_stricmp( message, me.map ) ? 0 : id;
		me.rxParts = argc >= 3 ? atoi( argv[2] ) : 0;
		me.rxGot = 0;
		me.rxLen = 0;
		me.rxBuf[0] = '\0';
	} else if ( !Q_stricmp( verb, "add" ) && id == me.rxId ) {
		const int len = strlen( message );

		if ( me.rxLen + len + 2 < (int)sizeof( me.rxBuf ) ) {
			memcpy( me.rxBuf + me.rxLen, message, len );
			me.rxLen += len;
			me.rxBuf[me.rxLen++] = '|';
			me.rxBuf[me.rxLen] = '\0';
		}
		me.rxGot++;
	} else if ( !Q_stricmp( verb, "end" ) && id == me.rxId ) {
		ME_ApplyBatch();
	}
}

// a new map: its entities, its styles, and the edits made for it
static void ME_CheckMap( void ) {
	if ( !cl.mapname[0] )
		return;
	if ( !Q_stricmp( me.map, cl.mapname ) ) {
		// the same map back, after a vid_restart or a map_restart
		if ( me.reparse ) {
			qboolean hidden[ME_MAX_ENTS];
			const int num = me.numEnts;

			for ( int i = 0; i < num; i++ )
				hidden[i] = me.ents[i].hidden;
			ME_ParseEnts();
			for ( int i = 0; i < num && i < me.numEnts; i++ )
				me.ents[i].hidden = hidden[i];
			ME_RebuildHidden();
			me.reparse = qfalse;
		}
		return;
	}
	if ( me.dirty && me.map[0] )
		Com_Printf( S_COLOR_YELLOW "Map editor: the edits for %s weren't saved\n", me.map );
	me.reparse = qfalse;
	Q_strncpyz( me.map, cl.mapname, sizeof( me.map ) );
	me.numShare = me.shareSent = 0;
	me.rxId = 0;
	me.layoutSel = -1;
	ME_ParseEnts();
	ME_ScanStyles();
	ME_Load();
	ME_ListLayouts();
	me.entScroll = me.styleScroll = 0;
	memset( me.itemScroll, 0, sizeof( me.itemScroll ) );
}

/*
===============================================================================

Models to choose from

===============================================================================
*/

static void ME_AddName( const char *name, void *ctx ) {
	const int len = strlen( name ) + 1;
	char *stored;

	if ( me.numNames >= ME_MAX_NAMES || me.poolUsed + len > ME_NAME_POOL || !Q_stricmpn( name, "models/players/", 15 ) )
		return;
	stored = meNamePool + me.poolUsed;
	me.poolUsed += len;
	Q_strncpyz( stored, name, len );
	for ( char *c = stored; *c; c++ )
		*c = ( *c == '\\' ) ? '/' : tolower( *c );
	me.names[me.numNames++] = stored;
}

static int QDECL ME_CompareNames( const void *a, const void *b ) {
	return strcmp( *(const char **)a, *(const char **)b );
}

static void ME_IndexModels( void ) {
	int kept = 0;

	me.numNames = me.poolUsed = 0;
	FS_ListFilesRecursive( "models", ".md3", ME_AddName, NULL );
	qsort( me.names, me.numNames, sizeof( me.names[0] ), ME_CompareNames );
	// a model in a pk3 and loose shows once
	for ( int i = 0; i < me.numNames; i++ ) {
		if ( kept && !strcmp( me.names[kept - 1], me.names[i] ) )
			continue;
		me.names[kept++] = me.names[i];
	}
	me.numNames = kept;
	me.indexed = qtrue;
}

/*
===============================================================================

Opening and closing

===============================================================================
*/

static void ME_ReleaseKeys( void ) {
	memset( me.held, 0, sizeof( me.held ) );
	me.flying = qfalse;
}

void CL_MapEditor_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Map editor: join a server first\n" );
		return;
	}
	// for RPMod's admins: whoever else can't run what it makes
	if ( !CL_RpAdmin_Allow( "Map editor", "mapedit" ) )
		return;
	CL_ModelManager_Close();
	CL_NpcManager_Close();
	CL_ShaderManager_Close();
	CL_EffectManager_Close();
	CL_Storyteller_Close();
	CL_RpCinematic_EditorClose();
	CL_SoundManager_Close();
	ME_CheckMap();
	if ( !me.indexed )
		ME_IndexModels();

	me.font = re->RegisterFont( "arialnb" );
	if ( !me.font )
		me.font = cls.menuFont;
	if ( me.havePlayerView ) {
		VectorCopy( me.playerOrg, me.camOrg );
		VectorCopy( me.playerAng, me.camAng );
	} else {
		VectorCopy( cl.snap.ps.origin, me.camOrg );
		me.camOrg[2] += cl.snap.ps.viewheight;
		VectorCopy( cl.viewangles, me.camAng );
	}
	me.camAng[ROLL] = 0;
	me.camAng[PITCH] = Com_Clamp( -89.0f, 89.0f, AngleNormalize180( me.camAng[PITCH] ) );

	me.open = qtrue;
	me.field = ME_FIELD_NONE;
	me.grabbing = qfalse;
	me.cursorX = ME_PANEL_X - 120;
	me.cursorY = SCREEN_HEIGHT * 0.5f;
	me.click = qfalse;
	me.wheel = 0;
	me.lastFrameTime = cls.realtime;
	ME_ReleaseKeys();
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_MAPEDITOR );
}

void CL_MapEditor_Close( void ) {
	if ( !me.open )
		return;
	ME_CancelGrab();
	me.open = qfalse;
	me.field = ME_FIELD_NONE;
	ME_ReleaseKeys();
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_MAPEDITOR );
	if ( me.dirty )
		Com_Printf( S_COLOR_YELLOW "Map editor: the edits aren't saved yet; /mapedit save keeps them\n" );
}

qboolean CL_MapEditor_Active( void ) {
	return me.open;
}

/*
===============================================================================

Picking: what's under the cursor or the crosshair

===============================================================================
*/

// from the camera through the cursor, or straight ahead while flying
static void ME_CursorRay( float x, float y, vec3_t dir ) {
	if ( me.flying || !me.haveView ) {
		AngleVectors( me.camAng, dir, NULL, NULL );
		return;
	}
	VectorCopy( me.viewAxis[0], dir );
	VectorMA( dir, ( 1.0f - 2.0f * x / SCREEN_WIDTH ) * tanf( DEG2RAD( me.viewFovX * 0.5f ) ), me.viewAxis[1], dir );
	VectorMA( dir, ( 1.0f - 2.0f * y / SCREEN_HEIGHT ) * tanf( DEG2RAD( me.viewFovY * 0.5f ) ), me.viewAxis[2], dir );
	VectorNormalize( dir );
}

// how far along the ray it enters the box, -1 if it misses
static float ME_RayBox( const vec3_t org, const vec3_t dir, const vec3_t mins, const vec3_t maxs ) {
	float tmin = 0.0f, tmax = ME_TRACE_DIST;

	for ( int i = 0; i < 3; i++ ) {
		if ( fabsf( dir[i] ) < 0.0001f ) {
			if ( org[i] < mins[i] || org[i] > maxs[i] )
				return -1.0f;
			continue;
		}
		{
			float t1 = ( mins[i] - org[i] ) / dir[i], t2 = ( maxs[i] - org[i] ) / dir[i];

			if ( t1 > t2 ) {
				const float t = t1;
				t1 = t2;
				t2 = t;
			}
			tmin = Q_max( tmin, t1 );
			tmax = Q_min( tmax, t2 );
			if ( tmin > tmax )
				return -1.0f;
		}
	}
	return tmin;
}

static float ME_RayPoint( const vec3_t org, const vec3_t dir, const vec3_t p, float size ) {
	vec3_t mins, maxs;

	VectorSet( mins, p[0] - size, p[1] - size, p[2] - size );
	VectorSet( maxs, p[0] + size, p[1] + size, p[2] + size );
	return ME_RayBox( org, dir, mins, maxs );
}

static void ME_ItemBounds( meItem_t *it, vec3_t mins, vec3_t maxs ) {
	if ( it->kind == ME_ITEM_TRIGGER ) {
		VectorSubtract( it->org, it->size, mins );
		VectorAdd( it->org, it->size, maxs );
		return;
	}
	if ( it->kind == ME_ITEM_MODEL || it->kind == ME_ITEM_PROP ) {
		ME_RegisterItem( it );
		if ( it->h ) {
			vec3_t m0, m1, axis[3];

			// its own box, turned: the box around that
			re->ModelBounds( it->h, m0, m1 );
			AnglesToAxis( it->ang, axis );
			ClearBounds( mins, maxs );
			for ( int i = 0; i < 8; i++ ) {
				vec3_t p;

				VectorCopy( it->org, p );
				VectorMA( p, ( ( i & 1 ) ? m1[0] : m0[0] ) * it->scale, axis[0], p );
				VectorMA( p, ( ( i & 2 ) ? m1[1] : m0[1] ) * it->scale, axis[1], p );
				VectorMA( p, ( ( i & 4 ) ? m1[2] : m0[2] ) * it->scale, axis[2], p );
				AddPointToBounds( p, mins, maxs );
			}
			if ( maxs[0] - mins[0] > 1.0f )
				return;
		}
	}
	VectorSet( mins, it->org[0] - ME_POINT_SIZE, it->org[1] - ME_POINT_SIZE, it->org[2] - ME_POINT_SIZE );
	VectorSet( maxs, it->org[0] + ME_POINT_SIZE, it->org[1] + ME_POINT_SIZE, it->org[2] + ME_POINT_SIZE );
}

// where the ray meets one of ours; a model by its own box, turned as it is
static float ME_ItemRay( meItem_t *it, const vec3_t org, const vec3_t dir ) {
	vec3_t mins, maxs;

	ME_RegisterItem( it );
	if ( ( it->kind == ME_ITEM_MODEL || it->kind == ME_ITEM_PROP ) && it->h ) {
		vec3_t axis[3], d, localOrg, localDir;

		re->ModelBounds( it->h, mins, maxs );
		AnglesToAxis( it->ang, axis );
		VectorSubtract( org, it->org, d );
		for ( int i = 0; i < 3; i++ ) {
			localOrg[i] = DotProduct( d, axis[i] ) / it->scale;
			localDir[i] = DotProduct( dir, axis[i] ) / it->scale;
		}
		return ME_RayBox( localOrg, localDir, mins, maxs );
	}
	ME_ItemBounds( it, mins, maxs );
	return ME_RayBox( org, dir, mins, maxs );
}

// the nearest thing the ray meets: ours first when they overlap, they're smaller
static qboolean ME_Pick( const vec3_t org, const vec3_t dir, int *kind, int *index ) {
	float best = ME_TRACE_DIST;
	trace_t tr;
	vec3_t end;

	*kind = ME_SEL_NONE;
	*index = -1;
	// not through walls
	VectorMA( org, ME_TRACE_DIST, dir, end );
	CM_BoxTrace( &tr, org, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
	best = tr.fraction * ME_TRACE_DIST + 8.0f;

	for ( int i = 0; i < me.numItems; i++ ) {
		float t;

		// a removed prop is found in the list only
		if ( me.items[i].kind == ME_ITEM_PROP && me.items[i].removed )
			continue;
		t = ME_ItemRay( &me.items[i], org, dir );
		if ( t >= 0.0f && t < best ) {
			best = t;
			*kind = ME_SEL_ITEM;
			*index = i;
		}
	}
	if ( *kind != ME_SEL_NONE )
		return qtrue;
	for ( int i = 0; i < me.numEnts; i++ ) {
		const meEnt_t *e = &me.ents[i];
		float t;

		if ( !ME_EntDrawn( e ) )
			continue;
		if ( e->bmodel ) {
			t = ME_RayBox( org, dir, e->mins, e->maxs );
			// the box we're in is found only by its walls
			if ( t == 0.0f )
				continue;
		} else {
			t = ME_RayPoint( org, dir, e->origin, ME_POINT_SIZE );
		}
		if ( t >= 0.0f && t < best ) {
			best = t;
			*kind = ME_SEL_ENT;
			*index = i;
		}
	}
	return (qboolean)( *kind != ME_SEL_NONE );
}

static void ME_ScrollToSelection( void ) {
	if ( me.selKind == ME_SEL_ENT ) {
		int row = 0;

		for ( int i = 0; i < me.sel; i++ ) {
			if ( ME_EntMatches( &me.ents[i] ) )
				row++;
		}
		me.entScroll = Q_max( 0, row - 4 );
	}
}

static void ME_ClickWorld( float x, float y ) {
	vec3_t dir;
	int kind, index;

	if ( me.grabbing ) {
		me.grabbing = qfalse;
		ME_Changed();
		return;
	}
	ME_CursorRay( x, y, dir );
	if ( !ME_Pick( me.camOrg, dir, &kind, &index ) || ( kind == ME_SEL_ENT && me.ents[index].kind == ME_ENT_TRIGGER ) ) {
		float dist;

		// a prop of the map's before a trigger it's standing in
		if ( ME_ChooseProp( ME_TriHit( me.camOrg, dir, &dist ) ) )
			return;
		if ( kind == ME_SEL_NONE ) {
			me.selKind = ME_SEL_NONE;
			me.sel = -1;
			return;
		}
	}
	ME_Select( kind, index );
	if ( kind == ME_SEL_ITEM )
		me.tab = ME_ItemTab( me.items[index].kind );
	else
		me.tab = ME_TAB_MAP;
	ME_ScrollToSelection();
}

// the camera in front of a spot, looking at it
static void ME_GoTo( const vec3_t spot, float size ) {
	vec3_t forward;

	AngleVectors( me.camAng, forward, NULL, NULL );
	VectorMA( spot, -( 96.0f + Q_min( size, 1024.0f ) ), forward, me.camOrg );
}

/*
===============================================================================

Editor input

===============================================================================
*/

static qboolean ME_Held( int upper ) {
	return (qboolean)( me.held[upper] || me.held[upper + ( 'a' - 'A' )] );
}

static qboolean ME_ShiftDown( void ) {
	return (qboolean)( me.held[A_SHIFT] || me.held[A_SHIFT2] );
}

static qboolean ME_CtrlDown( void ) {
	return (qboolean)( me.held[A_CTRL] || me.held[A_CTRL2] );
}

static qboolean ME_IsToggleKey( int key ) {
	const char *binding;

	if ( me.field && key >= A_SPACE && key <= A_TILDE )
		return qfalse;
	binding = Key_GetBinding( key );
	return (qboolean)( VALIDSTRING( binding ) && !Q_stricmp( binding, ME_TOGGLE_CMD ) );
}

static char *ME_FieldBuffer( int *size ) {
	meItem_t *it = ME_SelectedItem();

	switch ( me.field ) {
	case ME_FIELD_FILTER:
		*size = sizeof( me.filter );
		return me.filter;
	case ME_FIELD_SEARCH:
		*size = sizeof( me.search );
		return me.search;
	case ME_FIELD_COMMAND:
		if ( !it || it->kind != ME_ITEM_TRIGGER )
			return NULL;
		*size = sizeof( it->command );
		return it->command;
	case ME_FIELD_PATTERN:
		if ( me.selKind != ME_SEL_STYLE || me.sel < 0 || me.sel >= MAX_LIGHT_STYLES )
			return NULL;
		*size = sizeof( me.styles[me.sel].pattern );
		return me.styles[me.sel].pattern;
	case ME_FIELD_LAYOUT:
		*size = sizeof( me.layoutName );
		return me.layoutName;
	case ME_FIELD_SOUND:
		if ( !it || it->kind != ME_ITEM_SPEAKER )
			return NULL;
		*size = sizeof( it->noise );
		return it->noise;
	default:
		return NULL;
	}
}

static void ME_FieldEdited( void ) {
	if ( me.field == ME_FIELD_FILTER )
		me.entScroll = 0;
	else if ( me.field == ME_FIELD_SEARCH )
		me.searchScroll = 0;
	else if ( me.field != ME_FIELD_LAYOUT )
		ME_Changed();
	// another sound: heard as typed, once it's one
	if ( me.field == ME_FIELD_SOUND && ME_SelectedItem() ) {
		ME_SelectedItem()->sfxRegistered = qfalse;
		me.soundScroll = 0;
	}
}

void CL_MapEditor_KeyEvent( int key, qboolean down ) {
	if ( !me.open )
		return;

	if ( key >= 0 && key < MAX_KEYS )
		me.held[key] = down;
	if ( !down ) {
		if ( key == A_MOUSE2 )
			me.flying = qfalse;
		return;
	}
	if ( ME_IsToggleKey( key ) ) {
		CL_MapEditor_Close();
		return;
	}
	// the warning only takes clicks
	if ( me.experimentalBox ) {
		if ( key == A_MOUSE1 ) {
			me.click = qtrue;
			me.clickX = me.cursorX;
			me.clickY = me.cursorY;
		}
		return;
	}

	switch ( key ) {
	case A_MOUSE1:
		me.click = qtrue;
		me.clickX = me.cursorX;
		me.clickY = me.cursorY;
		return;
	case A_MOUSE2:
		me.flying = qtrue;
		me.field = ME_FIELD_NONE;
		return;
	case A_MWHEELUP:
		if ( me.flying )
			me.camFov = Com_Clamp( 10.0f, 130.0f, me.camFov - 5.0f );
		else
			me.wheel--;
		return;
	case A_MWHEELDOWN:
		if ( me.flying )
			me.camFov = Com_Clamp( 10.0f, 130.0f, me.camFov + 5.0f );
		else
			me.wheel++;
		return;
	default:
		break;
	}
	if ( me.flying ) {
		// carrying something while flying: G drops it
		if ( ( key == A_CAP_G || key == A_LOW_G ) && me.grabbing ) {
			me.grabbing = qfalse;
			ME_Changed();
		}
		return;
	}

	if ( me.field ) {
		int size;
		char *buf = ME_FieldBuffer( &size );
		const int len = buf ? strlen( buf ) : 0;

		if ( key == A_BACKSPACE && len ) {
			buf[len - 1] = '\0';
			ME_FieldEdited();
		} else if ( key == A_ENTER || key == A_KP_ENTER ) {
			if ( me.field == ME_FIELD_LAYOUT )
				ME_SaveLayout( me.layoutName );
			me.field = ME_FIELD_NONE;
		}
		return;
	}

	switch ( key ) {
	case A_CAP_G:
	case A_LOW_G:
		if ( me.selKind == ME_SEL_PROP ) {
			ME_LiftProp( qfalse, qtrue );
			break;
		}
		ME_Grab();
		if ( !me.grabbing )
			ME_Changed();
		break;
	case A_DELETE:
	case A_BACKSPACE:
		if ( me.selKind == ME_SEL_PROP ) {
			ME_LiftProp( qtrue, qfalse );
		} else if ( ME_SelectedItem() && ME_SelectedItem()->kind == ME_ITEM_PROP ) {
			// a map prop is removed, Put back undoes it
			ME_SelectedItem()->removed = qtrue;
			ME_Changed();
		} else {
			ME_DeleteSelected();
		}
		break;
	case A_CAP_S:
	case A_LOW_S:
		if ( ME_CtrlDown() )
			ME_Save();
		break;
	case A_CAP_D:
	case A_LOW_D:
		if ( ME_CtrlDown() )
			ME_CopySelected();
		break;
	default:
		break;
	}
}

static qboolean ME_FieldChar( int field, int ch ) {
	switch ( field ) {
	case ME_FIELD_PATTERN:
		return (qboolean)( ch >= 'a' && ch <= 'z' );
	case ME_FIELD_LAYOUT:
		return ME_LayoutChar( ch );
	case ME_FIELD_COMMAND:
		// an entity's value can't hold a quote
		return (qboolean)( ch >= ' ' && ch < 127 && ch != '"' );
	default:
		return (qboolean)( ch > ' ' && ch < 127 && ch != '"' );
	}
}

void CL_MapEditor_CharEvent( int ch ) {
	int size, len;
	char *buf;

	if ( !me.open || !me.field || me.flying || me.experimentalBox )
		return;
	if ( me.field == ME_FIELD_PATTERN && ch >= 'A' && ch <= 'Z' )
		ch += 'a' - 'A';
	if ( !ME_FieldChar( me.field, ch ) || !( buf = ME_FieldBuffer( &size ) ) )
		return;
	len = strlen( buf );
	if ( len < size - 1 ) {
		buf[len] = (char)ch;
		buf[len + 1] = '\0';
		ME_FieldEdited();
	}
}

void CL_MapEditor_MouseEvent( int dx, int dy ) {
	if ( !me.open )
		return;
	if ( me.flying ) {
		me.camAng[YAW] -= dx * cl_sensitivity->value * m_yaw->value;
		me.camAng[PITCH] = Com_Clamp( -89.0f, 89.0f, me.camAng[PITCH] + dy * cl_sensitivity->value * m_pitch->value );
	} else {
		me.cursorX = Com_Clamp( 0, SCREEN_WIDTH, me.cursorX + dx );
		me.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, me.cursorY + dy );
	}
}

// escape backs out one level: the text box, carrying, the selection, then the editor
void CL_MapEditor_Escape( void ) {
	if ( me.experimentalBox ) {
		me.experimentalBox = qfalse;
	} else if ( me.field ) {
		me.field = ME_FIELD_NONE;
	} else if ( me.grabbing ) {
		ME_CancelGrab();
	} else if ( me.selKind != ME_SEL_NONE ) {
		me.selKind = ME_SEL_NONE;
		me.sel = -1;
	} else {
		CL_MapEditor_Close();
	}
}

/*
===============================================================================

Every frame

===============================================================================
*/

// our triggers: walking into one runs its command
static void ME_RunTriggers( void ) {
	vec3_t mins, maxs;

	if ( me.open || !ME_Shown() || cl.snap.ps.pm_type == PM_SPECTATOR || cl.snap.ps.stats[STAT_HEALTH] <= 0 ) {
		for ( int i = 0; i < me.numItems; i++ )
			me.items[i].inside = qfalse;
		return;
	}
	// the player's box
	VectorSet( mins, cl.snap.ps.origin[0] - 15, cl.snap.ps.origin[1] - 15, cl.snap.ps.origin[2] - 24 );
	VectorSet( maxs, cl.snap.ps.origin[0] + 15, cl.snap.ps.origin[1] + 15, cl.snap.ps.origin[2] + 40 );
	for ( int i = 0; i < me.numItems; i++ ) {
		meItem_t *it = &me.items[i];
		qboolean inside = qtrue;

		if ( it->kind != ME_ITEM_TRIGGER )
			continue;
		for ( int j = 0; j < 3; j++ ) {
			if ( maxs[j] < it->org[j] - it->size[j] || mins[j] > it->org[j] + it->size[j] )
				inside = qfalse;
		}
		if ( inside && !it->inside && it->command[0] && !( it->once && it->fired ) ) {
			it->fired = qtrue;
			Cbuf_AddText( va( "%s\n", it->command ) );
		}
		it->inside = inside;
	}
}

static void ME_EditorFrame( void ) {
	vec3_t forward, right, move, end, dir;
	float dt, speed;
	trace_t tr;

	if ( !( Key_GetCatcher() & KEYCATCH_MAPEDITOR ) ) {
		// something else cleared the catchers
		ME_CancelGrab();
		me.open = qfalse;
		me.field = ME_FIELD_NONE;
		ME_ReleaseKeys();
		return;
	}

	dt = Com_Clamp( 0.0f, 0.1f, ( cls.realtime - me.lastFrameTime ) / 1000.0f );
	me.lastFrameTime = cls.realtime;

	if ( me.flying ) {
		AngleVectors( me.camAng, forward, right, NULL );
		VectorClear( move );
		if ( ME_Held( A_CAP_W ) )	VectorAdd( move, forward, move );
		if ( ME_Held( A_CAP_S ) )	VectorSubtract( move, forward, move );
		if ( ME_Held( A_CAP_D ) )	VectorAdd( move, right, move );
		if ( ME_Held( A_CAP_A ) )	VectorSubtract( move, right, move );
		if ( me.held[A_SPACE] )		move[2] += 1.0f;
		if ( ME_Held( A_CAP_C ) )	move[2] -= 1.0f;
		speed = ME_FLY_SPEED;
		if ( ME_ShiftDown() )
			speed *= 3.0f;
		if ( ME_CtrlDown() )
			speed *= 0.25f;
		if ( VectorNormalize( move ) > 0.0f )
			VectorMA( me.camOrg, speed * dt, move, me.camOrg );
	}

	// what the crosshair, or the cursor, is on
	ME_CursorRay( me.cursorX, me.cursorY, dir );
	if ( me.flying || me.cursorX < ME_PANEL_X ) {
		VectorMA( me.camOrg, ME_TRACE_DIST, dir, end );
		CM_BoxTrace( &tr, me.camOrg, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
		me.haveAim = (qboolean)( tr.fraction < 1.0f && !tr.allsolid );
		VectorCopy( tr.endpos, me.aim );
		VectorCopy( tr.plane.normal, me.aimNormal );
	}
	if ( !me.flying && me.cursorX < ME_PANEL_X && !me.grabbing )
		ME_Pick( me.camOrg, dir, &me.hoverKind, &me.hover );
	else
		me.hoverKind = ME_SEL_NONE;

	// carried to the crosshair
	if ( me.grabbing ) {
		meItem_t *it = ME_SelectedItem();

		if ( !it )
			me.grabbing = qfalse;
		else if ( me.haveAim )
			ME_PlaceAt( it, me.aim, me.aimNormal );
	}
}

void CL_MapEditor_Frame( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted )
		return;
	ME_CheckMap();
	if ( me.numHidden == 0 ) {
		for ( int i = 0; i < me.numEnts; i++ ) {
			if ( me.ents[i].hidden ) {
				ME_RebuildHidden();
				break;
			}
		}
	}
	if ( me.propsDirty || me.propsShown != ME_Shown() )
		ME_ApplyProps();
	ME_RunTriggers();
	ME_RunShare();
	if ( me.open )
		ME_EditorFrame();
}

/*
===============================================================================

The scene: our things always, the editor's lines while it's open

===============================================================================
*/

static void ME_Line( const vec3_t a, const vec3_t b, const byte *color ) {
	refEntity_t ent;
	vec3_t mid, toCam;

	if ( me.numLines >= ME_MAX_LINES )
		return;
	me.numLines++;
	VectorAdd( a, b, mid );
	VectorScale( mid, 0.5f, mid );
	VectorSubtract( mid, me.camOrg, toCam );

	memset( &ent, 0, sizeof( ent ) );
	ent.reType = RT_LINE;
	VectorCopy( a, ent.origin );
	VectorCopy( b, ent.oldorigin );
	ent.radius = 0.3f + VectorLength( toCam ) * 0.0015f;
	ent.customShader = cls.whiteShader;
	memcpy( ent.shaderRGBA, color, sizeof( ent.shaderRGBA ) );
	ent.renderfx = RF_NOSHADOW;
	re->AddRefEntityToScene( &ent );
}

static void ME_Cross( const vec3_t p, float size, const byte *color ) {
	for ( int i = 0; i < 3; i++ ) {
		vec3_t a, b;

		VectorCopy( p, a );
		VectorCopy( p, b );
		a[i] -= size;
		b[i] += size;
		ME_Line( a, b, color );
	}
}

static void ME_BoxLines( const vec3_t mins, const vec3_t maxs, const byte *color ) {
	vec3_t c[8];

	for ( int i = 0; i < 8; i++ )
		VectorSet( c[i], ( i & 1 ) ? maxs[0] : mins[0], ( i & 2 ) ? maxs[1] : mins[1], ( i & 4 ) ? maxs[2] : mins[2] );
	for ( int i = 0; i < 8; i++ ) {
		for ( int bit = 1; bit < 8; bit <<= 1 ) {
			if ( !( i & bit ) )
				ME_Line( c[i], c[i | bit], color );
		}
	}
}

static void ME_Circle( const vec3_t center, float radius, int axis, const byte *color ) {
	vec3_t prev, p;

	for ( int i = 0; i <= 32; i++ ) {
		const float a = DEG2RAD( i * 11.25f );
		const int u = ( axis + 1 ) % 3, v = ( axis + 2 ) % 3;

		VectorCopy( center, p );
		p[u] += cosf( a ) * radius;
		p[v] += sinf( a ) * radius;
		if ( i )
			ME_Line( prev, p, color );
		VectorCopy( p, prev );
	}
}

static void ME_ItemAxis( const meItem_t *it, matrix3_t axis ) {
	AnglesToAxis( it->ang, axis );
	if ( it->scale != 1.0f ) {
		for ( int i = 0; i < 3; i++ )
			VectorScale( axis[i], it->scale, axis[i] );
	}
}

// a light's colour now, with its style
static void ME_LightNow( const meItem_t *it, vec3_t color ) {
	VectorCopy( it->color, color );
	if ( it->style >= 0 && it->style < MAX_LIGHT_STYLES ) {
		color4ub_t c;

		re->GetLightStyle( it->style, c );
		color[0] *= c[0] / 255.0f;
		color[1] *= c[1] / 255.0f;
		color[2] *= c[2] / 255.0f;
	}
}

// a speaker's sound, registered when first wanted; 0 if there's none to play. The game keeps
// MP3s of sound/chars to stream, and a stream can't loop
static sfxHandle_t ME_SpeakerSound( meItem_t *it ) {
	if ( !it->sfxRegistered ) {
		it->sfxRegistered = qtrue;
		it->sfx = 0;
		if ( it->noise[0] && Q_stricmpn( it->noise, "sound/chars/", 12 ) && FS_ReadFile( it->noise, NULL ) > 0 )
			it->sfx = S_RegisterSound( it->noise );
	}
	return it->sfx;
}

// the speakers, round and round: near them, or as loud everywhere, where the listener's ear
// is. Each frame, as the cgame hands its own looping sounds to the mixer
void CL_MapEditor_Sounds( const vec3_t ear ) {
	if ( cls.state != CA_ACTIVE || !ME_Shown() || !me.map[0] || Q_stricmp( me.map, cl.mapname ) )
		return;
	for ( int i = 0; i < me.numItems; i++ ) {
		meItem_t *it = &me.items[i];
		sfxHandle_t sfx;

		if ( it->kind != ME_ITEM_SPEAKER || it->off || !( sfx = ME_SpeakerSound( it ) ) )
			continue;
		S_AddLoopingSound( ENTITYNUM_WORLD, it->global ? ear : it->org, vec3_origin, sfx );
	}
}

// the models and lights, in every main view
static void ME_AddItems( void ) {
	for ( int i = 0; i < me.numItems; i++ ) {
		meItem_t *it = &me.items[i];

		if ( it->kind == ME_ITEM_MODEL || it->kind == ME_ITEM_PROP ) {
			refEntity_t ent;

			ME_RegisterItem( it );
			if ( !it->h || ( it->kind == ME_ITEM_PROP && it->removed ) )
				continue;
			memset( &ent, 0, sizeof( ent ) );
			ent.hModel = it->h;
			VectorCopy( it->org, ent.origin );
			VectorCopy( it->org, ent.oldorigin );
			VectorCopy( it->org, ent.lightingOrigin );
			ME_ItemAxis( it, ent.axis );
			ent.nonNormalizedAxes = (qboolean)( it->scale != 1.0f );
			re->AddRefEntityToScene( &ent );
		} else if ( it->kind == ME_ITEM_LIGHT ) {
			vec3_t color;

			ME_LightNow( it, color );
			if ( color[0] + color[1] + color[2] > 0.01f )
				re->AddLightToScene( it->org, it->radius, color[0], color[1], color[2] );
		}
	}
}

typedef struct {
	int		index;
	float	dist;
} meNear_t;

static int QDECL ME_CompareNear( const void *a, const void *b ) {
	const float d = ( (const meNear_t *)a )->dist - ( (const meNear_t *)b )->dist;

	return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static qboolean ME_IsSel( int kind, int index ) {
	return (qboolean)( me.selKind == kind && me.sel == index );
}

static qboolean ME_IsHover( int kind, int index ) {
	return (qboolean)( me.hoverKind == kind && me.hover == index );
}

static void ME_AddEditorLines( void ) {
	static meNear_t near_[ME_MAX_ENTS];
	int numNear = 0;

	me.numLines = 0;
	if ( me.haveAim && ( me.flying || me.grabbing ) )
		ME_Cross( me.aim, 4.0f, meAimColor );

	// ours first, they're what's worked on
	for ( int i = 0; i < me.numItems; i++ ) {
		meItem_t *it = &me.items[i];
		const qboolean sel = ME_IsSel( ME_SEL_ITEM, i ), hover = ME_IsHover( ME_SEL_ITEM, i );
		vec3_t mins, maxs;

		if ( it->kind == ME_ITEM_TRIGGER ) {
			ME_ItemBounds( it, mins, maxs );
			ME_BoxLines( mins, maxs, sel || hover ? meSelColor : meOurTrigger );
		} else if ( it->kind == ME_ITEM_SPEAKER ) {
			ME_Cross( it->org, sel || hover ? 10.0f : 6.0f, sel || hover ? meSelColor : it->off ? meHiddenColor : meSpeakerColor );
			if ( sel && !it->global )
				ME_Circle( it->org, ME_SOUND_NEAR, 2, meReachColor );
		} else if ( it->kind == ME_ITEM_LIGHT ) {
			byte color[4];
			vec3_t c;

			ME_LightNow( it, c );
			color[0] = (byte)Com_Clampi( 0, 255, ME_Round( it->color[0] * 255 ) );
			color[1] = (byte)Com_Clampi( 0, 255, ME_Round( it->color[1] * 255 ) );
			color[2] = (byte)Com_Clampi( 0, 255, ME_Round( it->color[2] * 255 ) );
			color[3] = 255;
			ME_Cross( it->org, sel || hover ? 10.0f : 6.0f, sel || hover ? meSelColor : color );
			if ( sel ) {
				// how far it reaches
				for ( int a = 0; a < 3; a++ )
					ME_Circle( it->org, it->radius, a, meReachColor );
			}
		} else {
			ME_Cross( it->org, 4.0f, it->kind == ME_ITEM_PROP && it->removed ? meHiddenColor : meItemColor );
			if ( sel || hover ) {
				ME_ItemBounds( it, mins, maxs );
				if ( it->h ) {
					vec3_t m0, m1;

					// its own box, unturned
					re->ModelBounds( it->h, m0, m1 );
					VectorMA( it->org, it->scale, m0, mins );
					VectorMA( it->org, it->scale, m1, maxs );
				}
				ME_BoxLines( mins, maxs, meSelColor );
			}
		}
	}

	// a prop of the map's, chosen
	if ( me.selKind == ME_SEL_PROP )
		ME_BoxLines( me.candMins, me.candMaxs, meCandColor );

	// the map's, nearest first while there are lines to spare
	for ( int i = 0; i < me.numEnts; i++ ) {
		const meEnt_t *e = &me.ents[i];
		vec3_t center;
		float dist;

		if ( !ME_EntDrawn( e ) )
			continue;
		ME_EntCenter( e, center );
		dist = Distance( center, me.camOrg );
		if ( e->bmodel ) {
			// a big box is seen from further
			vec3_t size;

			VectorSubtract( e->maxs, e->mins, size );
			dist = Q_max( 0.0f, dist - VectorLength( size ) * 0.5f );
		}
		if ( dist > ME_DRAW_DIST && !ME_IsSel( ME_SEL_ENT, i ) )
			continue;
		near_[numNear].index = i;
		near_[numNear].dist = ME_IsSel( ME_SEL_ENT, i ) ? -1.0f : dist;
		numNear++;
	}
	qsort( near_, numNear, sizeof( near_[0] ), ME_CompareNear );
	for ( int n = 0; n < numNear && me.numLines < ME_MAX_LINES; n++ ) {
		const int i = near_[n].index;
		const meEnt_t *e = &me.ents[i];
		const qboolean mark = (qboolean)( ME_IsSel( ME_SEL_ENT, i ) || ME_IsHover( ME_SEL_ENT, i ) );
		const byte *color = mark ? meSelColor : e->hidden ? meHiddenColor
			: e->kind == ME_ENT_TRIGGER ? meTriggerColor : e->kind == ME_ENT_BRUSH ? meBrushColor : mePointColor;

		if ( e->bmodel )
			ME_BoxLines( e->mins, e->maxs, color );
		else
			ME_Cross( e->origin, ME_POINT_SIZE, color );
	}
}

// the cgame's main view, before it goes to the renderer: our things are added to it
void CL_MapEditor_AddToScene( const refdef_t *fd ) {
	if ( fd->rdflags & ( RDF_NOWORLDMODEL | RDF_SKYBOXPORTAL | RDF_AUTOMAP ) )
		return;
	if ( !me.open ) {
		me.havePlayerView = qtrue;
		VectorCopy( fd->vieworg, me.playerOrg );
		VectorCopy( fd->viewangles, me.playerAng );
	}
	if ( !ME_Shown() || !me.map[0] || Q_stricmp( me.map, cl.mapname ) )
		return;
	ME_AddItems();
	if ( me.open )
		ME_AddEditorLines();
}

// the editor's camera in place of the player's. qtrue if drawn here or left out
qboolean CL_MapEditor_RenderScene( const refdef_t *fd ) {
	refdef_t view;

	if ( !me.open )
		return qfalse;
	// the HUD's 3D bits, heads and items
	if ( fd->rdflags & RDF_NOWORLDMODEL )
		return qtrue;
	if ( fd->rdflags & RDF_AUTOMAP )
		return qfalse;

	view = *fd;
	view.fov_x = me.camFov;
	view.fov_y = RAD2DEG( 2.0f * atanf( tanf( DEG2RAD( me.camFov * 0.5f ) ) * fd->height / (float)fd->width ) );
	VectorCopy( me.camAng, view.viewangles );
	AnglesToAxis( me.camAng, view.viewaxis );
	// the sky portal keeps its own origin but looks the way the camera does
	if ( !( fd->rdflags & RDF_SKYBOXPORTAL ) ) {
		VectorCopy( me.camOrg, view.vieworg );
		view.viewContents = CM_PointContents( me.camOrg, 0 );
		// the snapshot's area mask is from the player's position, the camera can be anywhere
		memset( view.areamask, 0, sizeof( view.areamask ) );
		me.haveView = qtrue;
		VectorCopy( view.vieworg, me.viewOrg );
		VectorCopy( view.viewaxis[0], me.viewAxis[0] );
		VectorCopy( view.viewaxis[1], me.viewAxis[1] );
		VectorCopy( view.viewaxis[2], me.viewAxis[2] );
		me.viewFovX = view.fov_x;
		me.viewFovY = view.fov_y;
	}
	re->RenderScene( &view );
	return qtrue;
}

// the camera, for the effects system to cull against
qboolean CL_MapEditor_Camera( vec3_t origin, vec3_t angles ) {
	if ( !me.open )
		return qfalse;
	VectorCopy( me.camOrg, origin );
	VectorCopy( me.camAng, angles );
	return qtrue;
}

// what the cgame adds that's left out: a hidden brush entity, and the view weapon
// floating where the player stands while the camera is elsewhere
qboolean CL_MapEditor_FilterEntity( const refEntity_t *ent ) {
	if ( me.open && ( ent->renderfx & RF_FIRST_PERSON ) )
		return qtrue;
	if ( ME_Shown() && ent->reType == RT_MODEL ) {
		for ( int i = 0; i < me.numHidden; i++ ) {
			if ( me.hidden[i] && ent->hModel == me.hidden[i] )
				return qtrue;
		}
	}
	return qfalse;
}

// the cgame's HUD is left out while editing, but what covers the whole screen still shows
qboolean CL_MapEditor_Hide2D( float x, float y, float w, float h ) {
	if ( !me.open )
		return qfalse;
	return (qboolean)!( x <= 0.0f && y <= 0.0f && x + w >= SCREEN_WIDTH && y + h >= SCREEN_HEIGHT );
}

/*
===============================================================================

The panel

===============================================================================
*/

static void ME_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void ME_Box( float x, float y, float w, float h, const float *fill ) {
	ME_Fill( x, y, w, h, fill );
	ME_Fill( x, y, w, 1, meBorder );
	ME_Fill( x, y + h - 1, w, 1, meBorder );
	ME_Fill( x, y, 1, h, meBorder );
	ME_Fill( x + w - 1, y, 1, h, meBorder );
}

static float ME_TextWidth( const char *text ) {
	return (float)re->Font_StrLenPixels( text, me.font, ME_TEXT_SCALE );
}

static void ME_Text( float x, float y, const char *text, const float *color ) {
	re->Font_DrawString( (int)x, (int)y, text, color, me.font | STYLE_DROPSHADOW, -1, ME_TEXT_SCALE );
}

static void ME_TextClipped( float x, float y, float w, const char *text, const float *color ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 3 && ME_TextWidth( buf ) > w ) {
		len--;
		buf[len - 3] = '.';
		buf[len - 2] = '.';
		buf[len - 1] = '.';
		buf[len] = '\0';
	}
	ME_Text( x, y, buf, color );
}

// the end of a long name, which is the part that tells it apart
static void ME_TextTail( float x, float y, float w, const char *text, const float *color ) {
	const char *p = text;

	while ( *p && ME_TextWidth( p ) > w )
		p++;
	if ( p != text && p[0] && p[1] && p[2] )
		ME_Text( x, y, va( "...%s", p + 3 ), color );
	else
		ME_Text( x, y, p, color );
}

static qboolean ME_InRect( float x, float y, float w, float h ) {
	return (qboolean)( !me.flying && me.cursorX >= x && me.cursorX < x + w && me.cursorY >= y && me.cursorY < y + h );
}

// eats this frame's click if it landed in the rect
static qboolean ME_Clicked( float x, float y, float w, float h ) {
	if ( !me.click || me.clickX < x || me.clickX >= x + w || me.clickY < y || me.clickY >= y + h )
		return qfalse;
	me.click = qfalse;
	return qtrue;
}

static qboolean ME_Button( float x, float y, float w, const char *label, qboolean selected, qboolean enabled ) {
	const float *fill = selected ? meHighlight : ( enabled && ME_InRect( x, y, w, ME_CTRL_H ) ) ? meHoverBox : mePanelLight;

	ME_Box( x, y, w, ME_CTRL_H, fill );
	ME_Text( x + ( w - ME_TextWidth( label ) ) * 0.5f, y + 2, label, enabled ? meWhite : meDim );
	return (qboolean)( ME_Clicked( x, y, w, ME_CTRL_H ) && enabled );
}

// a value with - and + either side, the wheel over it too; how many steps it went
static int ME_Stepper( float x, float y, float w, const char *label, const char *value ) {
	const float lw = 62.0f, vx = x + lw + 17, vw = w - lw - 34;
	int steps = 0;

	ME_Text( x, y + 2, label, meDim );
	if ( ME_Button( x + lw, y, 16, "-", qfalse, qtrue ) )
		steps--;
	ME_Box( vx, y, vw, ME_CTRL_H, mePanelLight );
	ME_Text( vx + ( vw - ME_TextWidth( value ) ) * 0.5f, y + 2, value, meWhite );
	if ( ME_Button( x + w - 16, y, 16, "+", qfalse, qtrue ) )
		steps++;
	if ( me.wheel && ME_InRect( x, y, w, ME_CTRL_H ) ) {
		steps -= me.wheel;
		me.wheel = 0;
	}
	return steps;
}

// Shift goes in big steps, Ctrl in small ones
static float ME_Step( float step ) {
	if ( ME_ShiftDown() )
		return step * 5.0f;
	if ( ME_CtrlDown() )
		return step * 0.2f;
	return step;
}

static void ME_Adjust( float *value, int steps, float step, float min, float max ) {
	if ( !steps )
		return;
	*value = Com_Clamp( min, max, *value + steps * ME_Step( step ) );
	ME_Changed();
}

static void ME_AdjustAngle( float *value, int steps, float step ) {
	if ( !steps )
		return;
	*value = AngleNormalize360( *value + steps * ME_Step( step ) );
	ME_Changed();
}

// a text box; a click on it starts typing
static void ME_TextBox( float x, float y, float w, int field, const char *text, const char *empty ) {
	const qboolean typing = (qboolean)( me.field == field );
	const char *label = text[0] || typing ? va( "%s%s", text, typing && ( ( cls.realtime >> 8 ) & 1 ) ? "_" : "" )
		: va( S_COLOR_GREY "%s", empty );

	ME_Box( x, y, w, ME_CTRL_H, typing ? meFocus : mePanelLight );
	if ( typing )
		ME_TextTail( x + 4, y + 2, w - 8, label, meWhite );
	else
		ME_TextClipped( x + 4, y + 2, w - 8, label, meWhite );
	if ( ME_Clicked( x, y, w, ME_CTRL_H ) )
		me.field = field;
}

// a scrolling list; returns the row clicked, -1 none
static int ME_ListRows( float x, float y, float w, int rows, int count, int *scroll ) {
	if ( me.wheel && ME_InRect( x, y, w, rows * ME_ROW_H ) ) {
		*scroll += me.wheel;
		me.wheel = 0;
	}
	*scroll = Com_Clampi( 0, Q_max( 0, count - rows ), *scroll );
	ME_Box( x, y, w, rows * ME_ROW_H + 2, mePanelLight );
	for ( int i = 0; i < rows && *scroll + i < count; i++ ) {
		const float ry = y + 1 + i * ME_ROW_H;

		if ( ME_InRect( x, ry, w, ME_ROW_H ) )
			ME_Fill( x + 1, ry, w - 2, ME_ROW_H, meHoverBox );
	}
	if ( me.click && me.clickX >= x && me.clickX < x + w && me.clickY >= y + 1 && me.clickY < y + 1 + rows * ME_ROW_H ) {
		const int row = (int)( ( me.clickY - y - 1 ) / ME_ROW_H );

		if ( *scroll + row < count ) {
			me.click = qfalse;
			return *scroll + row;
		}
	}
	return -1;
}

static void ME_RowHighlight( float x, float y, float w, int row, int scroll, int rows ) {
	if ( row >= scroll && row < scroll + rows )
		ME_Fill( x + 1, y + 1 + ( row - scroll ) * ME_ROW_H, w - 2, ME_ROW_H, meHighlight );
}

static const char *ME_EntName( const meEnt_t *e ) {
	if ( e->targetname[0] )
		return va( "%s " S_COLOR_GREY "%s", e->classname, e->targetname );
	return e->classname[0] ? e->classname : "(no class)";
}

static void ME_DrawMapTab( float x, float y, float w ) {
	const float third = ( w - 8 ) / 3.0f;
	const int rows = 12, kvRows = 13;
	static int matches[ME_MAX_ENTS];
	meEnt_t *e;
	int numMatches = 0, clicked, selRow = -1;

	if ( ME_Button( x, y, third, "Triggers", me.showTriggers, qtrue ) )
		me.showTriggers = (qboolean)!me.showTriggers;
	if ( ME_Button( x + third + 4, y, third, "Brushes", me.showBrushes, qtrue ) )
		me.showBrushes = (qboolean)!me.showBrushes;
	if ( ME_Button( x + ( third + 4 ) * 2, y, third, "Points", me.showPoints, qtrue ) )
		me.showPoints = (qboolean)!me.showPoints;
	y += ME_CTRL_H + ME_GAP;
	ME_TextBox( x, y, w, ME_FIELD_FILTER, me.filter, "type to find a class or name" );
	y += ME_CTRL_H + ME_GAP;

	for ( int i = 0; i < me.numEnts; i++ ) {
		if ( ME_EntMatches( &me.ents[i] ) ) {
			if ( ME_IsSel( ME_SEL_ENT, i ) )
				selRow = numMatches;
			matches[numMatches++] = i;
		}
	}
	ME_Text( x, y, va( "Entities (%i of %i)", numMatches, me.numEnts ), meWhite );
	y += ME_ROW_H + 2;
	clicked = ME_ListRows( x, y, w, rows, numMatches, &me.entScroll );
	if ( selRow >= 0 )
		ME_RowHighlight( x, y, w, selRow, me.entScroll, rows );
	for ( int r = 0; r < rows && me.entScroll + r < numMatches; r++ ) {
		const meEnt_t *row = &me.ents[matches[me.entScroll + r]];

		ME_TextClipped( x + 4, y + 1 + r * ME_ROW_H, w - 8, va( "%s%i " S_COLOR_WHITE "%s",
			row->kind == ME_ENT_TRIGGER ? S_COLOR_ORANGE : row->kind == ME_ENT_BRUSH ? S_COLOR_CYAN : S_COLOR_GREEN,
			matches[me.entScroll + r], ME_EntName( row ) ), meWhite );
	}
	if ( clicked >= 0 )
		ME_Select( ME_SEL_ENT, matches[clicked] );
	y += rows * ME_ROW_H + 8;

	e = ME_SelectedEnt();
	if ( !e ) {
		ME_Text( x, y, S_COLOR_GREY "Click one in the world or the list", meWhite );
		return;
	}
	{
		const float half = ( w - 4 ) * 0.5f;
		vec3_t center, size;

		ME_EntCenter( e, center );
		VectorSubtract( e->maxs, e->mins, size );
		if ( ME_Button( x, y, half, "Go to it", qfalse, (qboolean)( e->bmodel || e->hasOrigin ) ) )
			ME_GoTo( center, e->bmodel ? VectorLength( size ) * 0.5f : 0.0f );
		if ( ME_Button( x + half + 4, y, half, e->hidden ? "Hidden here" : "Hide here", e->hidden, (qboolean)( e->bmodel != 0 ) ) ) {
			e->hidden = (qboolean)!e->hidden;
			ME_RebuildHidden();
			ME_Changed();
		}
		y += ME_CTRL_H + ME_GAP;
	}
	if ( me.kvEnt != me.sel )
		ME_ReadKeys( me.sel );
	clicked = ME_ListRows( x, y, w, kvRows, me.numKv, &me.kvScroll );
	for ( int r = 0; r < kvRows && me.kvScroll + r < me.numKv; r++ ) {
		const float ry = y + 1 + r * ME_ROW_H;

		ME_TextClipped( x + 4, ry, 70, me.kv[me.kvScroll + r][0], meDim );
		ME_TextClipped( x + 78, ry, w - 82, me.kv[me.kvScroll + r][1], meWhite );
	}
	// a click on a target finds what it targets
	if ( clicked >= 0 && ( !Q_stricmp( me.kv[clicked][0], "target" ) || !Q_stricmp( me.kv[clicked][0], "target2" ) ) ) {
		for ( int i = 0; i < me.numEnts; i++ ) {
			if ( me.ents[i].targetname[0] && !Q_stricmp( me.ents[i].targetname, me.kv[clicked][1] ) ) {
				ME_Select( ME_SEL_ENT, i );
				ME_ScrollToSelection();
				break;
			}
		}
	}
	y += kvRows * ME_ROW_H + 4;
	ME_Text( x, y, S_COLOR_GREY "The server runs these; a click on a target finds it", meWhite );
}

// the list of ours of a kind, and add, copy, delete; the item chosen, NULL none
static meItem_t *ME_DrawItemList( float *py, float x, float w, int kind, const char *addLabel ) {
	const int tab = ME_ItemTab( kind ), rows = 6;
	const float third = ( w - 8 ) / 3.0f;
	static int matches[ME_MAX_ITEMS];
	int numMatches = 0, clicked, selRow = -1;
	float y = *py;
	meItem_t *it;

	for ( int i = 0; i < me.numItems; i++ ) {
		if ( me.items[i].kind == kind || ( kind == ME_ITEM_MODEL && me.items[i].kind == ME_ITEM_PROP ) ) {
			if ( ME_IsSel( ME_SEL_ITEM, i ) )
				selRow = numMatches;
			matches[numMatches++] = i;
		}
	}
	clicked = ME_ListRows( x, y, w, rows, numMatches, &me.itemScroll[tab] );
	if ( selRow >= 0 )
		ME_RowHighlight( x, y, w, selRow, me.itemScroll[tab], rows );
	for ( int r = 0; r < rows && me.itemScroll[tab] + r < numMatches; r++ ) {
		const meItem_t *row = &me.items[matches[me.itemScroll[tab] + r]];
		const float ry = y + 1 + r * ME_ROW_H;
		const char *text;

		if ( row->kind == ME_ITEM_PROP )
			text = va( "%i  map prop%s", r + me.itemScroll[tab] + 1, row->removed ? S_COLOR_GREY " (removed)" : "" );
		else if ( kind == ME_ITEM_MODEL )
			text = va( "%i  %s", r + me.itemScroll[tab] + 1, COM_SkipPath( (char *)row->model ) );
		else if ( kind == ME_ITEM_LIGHT )
			text = va( "%i  reach %i%s", r + me.itemScroll[tab] + 1, ME_Round( row->radius ), row->style >= 0 ? va( ", style %i", row->style ) : "" );
		else if ( kind == ME_ITEM_SPEAKER )
			text = va( "%i  %s" S_COLOR_GREY "  %s%s", r + me.itemScroll[tab] + 1, row->noise[0] ? COM_SkipPath( (char *)row->noise ) : S_COLOR_YELLOW "no sound",
				row->global ? "everywhere" : "here", row->off ? ", stopped" : "" );
		else
			text = va( "%i  %s", r + me.itemScroll[tab] + 1, row->command[0] ? row->command : S_COLOR_GREY "no command" );
		ME_TextClipped( x + 4, ry, w - 8, text, meWhite );
	}
	if ( !numMatches )
		ME_Text( x + 4, y + 2, S_COLOR_GREY "None yet", meWhite );
	if ( clicked >= 0 )
		ME_Select( ME_SEL_ITEM, matches[clicked] );
	y += rows * ME_ROW_H + 6;

	it = ME_SelectedItem();
	if ( it && it->kind != kind && !( kind == ME_ITEM_MODEL && it->kind == ME_ITEM_PROP ) )
		it = NULL;
	if ( ME_Button( x, y, third, addLabel, qfalse, qtrue ) ) {
		ME_AddItem( kind );
		it = ME_SelectedItem();
	}
	if ( ME_Button( x + third + 4, y, third, "Copy", qfalse, (qboolean)( it != NULL ) ) ) {
		ME_CopySelected();
		it = ME_SelectedItem();
	}
	// a map prop goes back where it was
	if ( ME_Button( x + ( third + 4 ) * 2, y, third, it && it->kind == ME_ITEM_PROP ? "Put back" : "Delete", qfalse, (qboolean)( it != NULL ) ) ) {
		ME_DeleteSelected();
		it = NULL;
	}
	y += ME_CTRL_H + ME_GAP;
	if ( it ) {
		const float half = ( w - 4 ) * 0.5f;
		vec3_t mins, maxs, size;

		if ( ME_Button( x, y, half, me.grabbing ? "Carrying (G)" : "Move (G)", me.grabbing, qtrue ) ) {
			ME_Grab();
			if ( !me.grabbing )
				ME_Changed();
		}
		ME_ItemBounds( it, mins, maxs );
		VectorSubtract( maxs, mins, size );
		if ( ME_Button( x + half + 4, y, half, "Go to it", qfalse, qtrue ) )
			ME_GoTo( it->org, VectorLength( size ) * 0.5f );
		y += ME_CTRL_H + ME_GAP;
		{
			const int steps = ME_Stepper( x, y, w, "Height", va( "%i", ME_Round( it->org[2] ) ) );

			ME_Adjust( &it->org[2], steps, 4.0f, -65536.0f, 65536.0f );
		}
		y += ME_CTRL_H + ME_GAP;
	}
	*py = y;
	return it;
}

// a prop of the map's, chosen with a click: lift it out, or take it away
static void ME_DrawPropChoice( float *py, float x, float w ) {
	const float third = ( w - 8 ) / 3.0f;
	vec3_t size;
	float y = *py;

	VectorSubtract( me.candMaxs, me.candMins, size );
	ME_Text( x, y, va( "Map prop" S_COLOR_GREY "   %i triangles, %i x %i x %i", me.candTris,
		ME_Round( size[0] ), ME_Round( size[1] ), ME_Round( size[2] ) ), meWhite );
	y += ME_ROW_H + 2;
	if ( ME_Button( x, y, third, "Lift out (G)", qfalse, qtrue ) )
		ME_LiftProp( qfalse, qfalse );
	if ( ME_Button( x + third + 4, y, third, "Remove (Del)", qfalse, qtrue ) )
		ME_LiftProp( qtrue, qfalse );
	if ( ME_Button( x + ( third + 4 ) * 2, y, third, "Take more", qfalse, (qboolean)( me.numCandSeeds < ME_MAX_SEEDS ) ) )
		ME_TakeTouching();
	y += ME_CTRL_H + ME_GAP;
	ME_Text( x, y, S_COLOR_GREY "Take more adds loose pieces inside it", meWhite );
	y += ME_ROW_H + 6;
	*py = y;
}

static void ME_DrawModelsTab( float x, float y, float w ) {
	meItem_t *it;
	const int rows = 9;
	static int matches[ME_MAX_NAMES];
	int numMatches = 0, clicked;
	char lower[sizeof( me.search )];

	if ( me.selKind == ME_SEL_PROP )
		ME_DrawPropChoice( &y, x, w );
	it = ME_DrawItemList( &y, x, w, ME_ITEM_MODEL, "Add" );
	if ( it && it->kind == ME_ITEM_PROP ) {
		const float half = ( w - 4 ) * 0.5f;

		if ( ME_Button( x, y, half, it->removed ? "Removed" : "Shown", (qboolean)!it->removed, qtrue ) ) {
			it->removed = (qboolean)!it->removed;
			ME_Changed();
		}
		if ( ME_Button( x + half + 4, y, half, "Take more", qfalse, (qboolean)( it->numSeeds < ME_MAX_SEEDS ) ) )
			ME_TakeTouching();
		y += ME_CTRL_H + ME_GAP;
	}

	if ( it ) {
		int steps;

		steps = ME_Stepper( x, y, w, "Yaw", va( "%i", ME_Round( it->ang[YAW] ) ) );
		ME_AdjustAngle( &it->ang[YAW], steps, 15.0f );
		y += ME_CTRL_H + ME_GAP;
		steps = ME_Stepper( x, y, w, "Pitch", va( "%i", ME_Round( AngleNormalize180( it->ang[PITCH] ) ) ) );
		ME_AdjustAngle( &it->ang[PITCH], steps, 15.0f );
		y += ME_CTRL_H + ME_GAP;
		steps = ME_Stepper( x, y, w, "Roll", va( "%i", ME_Round( AngleNormalize180( it->ang[ROLL] ) ) ) );
		ME_AdjustAngle( &it->ang[ROLL], steps, 15.0f );
		y += ME_CTRL_H + ME_GAP;
		steps = ME_Stepper( x, y, w, "Scale", va( "%.2f", it->scale ) );
		ME_Adjust( &it->scale, steps, 0.1f, 0.05f, 50.0f );
		y += ME_CTRL_H + ME_GAP + 4;
	} else {
		y += 4;
	}

	// the models there are
	ME_TextBox( x, y, w, ME_FIELD_SEARCH, me.search, "type to find a model" );
	y += ME_CTRL_H + ME_GAP;
	Q_strncpyz( lower, me.search, sizeof( lower ) );
	Q_strlwr( lower );
	for ( int i = 0; i < me.numNames; i++ ) {
		if ( !lower[0] || strstr( me.names[i] + 7, lower ) )
			matches[numMatches++] = i;
	}
	clicked = ME_ListRows( x, y, w, rows, numMatches, &me.searchScroll );
	for ( int r = 0; r < rows && me.searchScroll + r < numMatches; r++ ) {
		const char *name = me.names[matches[me.searchScroll + r]];

		if ( !Q_stricmp( name, me.pickModel ) )
			ME_Fill( x + 1, y + 1 + r * ME_ROW_H, w - 2, ME_ROW_H, meHighlight );
		ME_TextTail( x + 4, y + 1 + r * ME_ROW_H, w - 8, name + 7, meWhite );
	}
	if ( clicked >= 0 ) {
		Q_strncpyz( me.pickModel, me.names[matches[clicked]], sizeof( me.pickModel ) );
		// the one chosen takes it
		if ( it && it->kind == ME_ITEM_MODEL ) {
			Q_strncpyz( it->model, me.pickModel, sizeof( it->model ) );
			it->registered = qfalse;
			ME_Changed();
		}
	}
	y += rows * ME_ROW_H + 4;
	ME_TextTail( x, y, w, me.pickModel[0] ? va( "Adds %s", me.pickModel + 7 ) : "Pick one, then Add puts it at the crosshair", meDim );
}

static void ME_DrawLightsTab( float x, float y, float w ) {
	meItem_t *it = ME_DrawItemList( &y, x, w, ME_ITEM_LIGHT, "Add" );
	const float sw = ( w - 4.0f * ( ARRAY_LEN( meLightColors ) - 1 ) ) / ARRAY_LEN( meLightColors );
	int steps;

	if ( !it ) {
		ME_Text( x, y + 4, S_COLOR_GREY "Add puts a light at the crosshair", meWhite );
		ME_Text( x, y + 4 + ME_ROW_H, S_COLOR_GREY "Only dynamic lights can be added at runtime", meWhite );
		return;
	}
	steps = ME_Stepper( x, y, w, "Reach", va( "%i", ME_Round( it->radius ) ) );
	ME_Adjust( &it->radius, steps, 25.0f, 8.0f, 2000.0f );
	y += ME_CTRL_H + ME_GAP;
	steps = ME_Stepper( x, y, w, "Red", va( "%i", ME_Round( it->color[0] * 255 ) ) );
	ME_Adjust( &it->color[0], steps, 0.1f, 0.0f, 1.0f );
	y += ME_CTRL_H + ME_GAP;
	steps = ME_Stepper( x, y, w, "Green", va( "%i", ME_Round( it->color[1] * 255 ) ) );
	ME_Adjust( &it->color[1], steps, 0.1f, 0.0f, 1.0f );
	y += ME_CTRL_H + ME_GAP;
	steps = ME_Stepper( x, y, w, "Blue", va( "%i", ME_Round( it->color[2] * 255 ) ) );
	ME_Adjust( &it->color[2], steps, 0.1f, 0.0f, 1.0f );
	y += ME_CTRL_H + ME_GAP;
	// the colours most lights are
	for ( int i = 0; i < (int)ARRAY_LEN( meLightColors ); i++ ) {
		const float sx = x + i * ( sw + 4 );
		vec4_t c;

		VectorCopy( meLightColors[i], c );
		c[3] = 1.0f;
		ME_Box( sx, y, sw, ME_CTRL_H, ME_InRect( sx, y, sw, ME_CTRL_H ) ? meHoverBox : mePanelLight );
		ME_Fill( sx + 3, y + 3, sw - 6, ME_CTRL_H - 6, c );
		if ( ME_Clicked( sx, y, sw, ME_CTRL_H ) ) {
			VectorCopy( meLightColors[i], it->color );
			ME_Changed();
		}
	}
	y += ME_CTRL_H + ME_GAP;
	steps = ME_Stepper( x, y, w, "Style", it->style >= 0 ? va( "%i", it->style ) : "none" );
	if ( steps ) {
		it->style = Com_Clampi( -1, MAX_LIGHT_STYLES - 1, it->style + steps );
		ME_Changed();
	}
	y += ME_CTRL_H + ME_GAP;
	ME_Text( x, y, S_COLOR_GREY "With a style it flickers or pulses as that does;", meWhite );
	ME_Text( x, y + ME_ROW_H, S_COLOR_GREY "set the style's pattern in Styles", meWhite );
}

static void ME_DrawTriggersTab( float x, float y, float w ) {
	meItem_t *it = ME_DrawItemList( &y, x, w, ME_ITEM_TRIGGER, "Add" );
	const float half = ( w - 4 ) * 0.5f;
	int steps;

	if ( !it ) {
		ME_Text( x, y + 4, S_COLOR_GREY "A box that runs a console command", meWhite );
		ME_Text( x, y + 4 + ME_ROW_H, S_COLOR_GREY "when you walk into it, editor closed", meWhite );
		return;
	}
	steps = ME_Stepper( x, y, w, "Width", va( "%i", ME_Round( it->size[0] * 2 ) ) );
	ME_Adjust( &it->size[0], steps, 8.0f, 1.0f, 4096.0f );
	y += ME_CTRL_H + ME_GAP;
	steps = ME_Stepper( x, y, w, "Depth", va( "%i", ME_Round( it->size[1] * 2 ) ) );
	ME_Adjust( &it->size[1], steps, 8.0f, 1.0f, 4096.0f );
	y += ME_CTRL_H + ME_GAP;
	steps = ME_Stepper( x, y, w, "Tall", va( "%i", ME_Round( it->size[2] * 2 ) ) );
	ME_Adjust( &it->size[2], steps, 8.0f, 1.0f, 4096.0f );
	y += ME_CTRL_H + ME_GAP + 4;
	ME_Text( x, y, "Runs", meDim );
	y += ME_ROW_H;
	ME_TextBox( x, y, w, ME_FIELD_COMMAND, it->command, "a console command, e.g. play sound/..." );
	y += ME_CTRL_H + ME_GAP;
	if ( ME_Button( x, y, half, it->once ? "Once" : "Every time", it->once, qtrue ) ) {
		it->once = (qboolean)!it->once;
		it->fired = qfalse;
		ME_Changed();
	}
	if ( ME_Button( x + half + 4, y, half, "Try it now", qfalse, (qboolean)( it->command[0] != '\0' ) ) )
		Cbuf_AddText( va( "%s\n", it->command ) );
	y += ME_CTRL_H + ME_GAP;
	if ( it->once && it->fired ) {
		if ( ME_Button( x, y, w, "Done; let it run again", qfalse, qtrue ) )
			it->fired = qfalse;
	}
}

static void ME_AddSound( const char *name, const char *source, void *ctx ) {
	char path[MAX_QPATH];
	const int len = strlen( name );

	if ( me.numSounds >= ME_MAX_SOUNDS || me.soundPoolUsed + len + 1 > ME_SOUND_POOL || len >= MAX_QPATH )
		return;
	// streamed, these can't loop
	if ( !Q_stricmpn( name, "sound/chars/", 12 ) )
		return;
	Q_strncpyz( path, name, sizeof( path ) );
	for ( char *c = path; *c; c++ ) {
		if ( *c == '\\' )
			*c = '/';
	}
	me.sounds[me.numSounds++] = strcpy( meSoundPool + me.soundPoolUsed, path );
	me.soundPoolUsed += len + 1;
}

static int QDECL ME_CompareSounds( const void *a, const void *b ) {
	return Q_stricmp( *(const char * const *)a, *(const char * const *)b );
}

// the music first, then the sounds, each found once
static void ME_IndexSounds( void ) {
	static const char *roots[] = { "music", "sound" };
	static const char *exts[] = { ".mp3", ".wav" };
	int count = 0;

	me.numSounds = me.soundPoolUsed = 0;
	for ( int r = 0; r < (int)ARRAY_LEN( roots ); r++ ) {
		const int from = me.numSounds;

		for ( int e = 0; e < (int)ARRAY_LEN( exts ); e++ )
			FS_ListFilesRecursiveFrom( roots[r], exts[e], ME_AddSound, NULL );
		qsort( me.sounds + from, me.numSounds - from, sizeof( me.sounds[0] ), ME_CompareSounds );
		// the same file in several pk3s
		count = from;
		for ( int i = from; i < me.numSounds; i++ ) {
			if ( count == from || Q_stricmp( me.sounds[i], me.sounds[count - 1] ) )
				me.sounds[count++] = me.sounds[i];
		}
		me.numSounds = count;
	}
	me.soundsIndexed = qtrue;
}

static void ME_DrawSoundsTab( float x, float y, float w ) {
	meItem_t *it = ME_DrawItemList( &y, x, w, ME_ITEM_SPEAKER, "Add" );
	const float half = ( w - 4 ) * 0.5f;
	static int matches[ME_MAX_SOUNDS];
	int numMatches = 0, clicked;
	const int rows = 10;

	if ( !it ) {
		ME_Text( x, y + 4, S_COLOR_GREY "A speaker at the crosshair, playing a sound", meWhite );
		ME_Text( x, y + 4 + ME_ROW_H, S_COLOR_GREY "or music round and round, as a target_speaker", meWhite );
		return;
	}
	if ( ME_Button( x, y, half, it->global ? "Heard everywhere" : "Heard near it", it->global, qtrue ) ) {
		it->global = (qboolean)!it->global;
		ME_Changed();
	}
	if ( ME_Button( x + half + 4, y, half, it->off ? "Stopped" : "Playing", (qboolean)!it->off, qtrue ) ) {
		it->off = (qboolean)!it->off;
		ME_Changed();
	}
	y += ME_CTRL_H + ME_GAP + 4;

	// the sound: typed, or picked from the ones that match what's typed
	ME_Text( x, y, va( "Sound%s", it->noise[0] && !ME_SpeakerSound( it ) ? S_COLOR_RED "  not found" : "" ), meDim );
	y += ME_ROW_H;
	ME_TextBox( x, y, w, ME_FIELD_SOUND, it->noise, "type to look for one, e.g. music/" );
	y += ME_CTRL_H + ME_GAP;
	if ( !me.soundsIndexed )
		ME_IndexSounds();
	for ( int i = 0; i < me.numSounds; i++ ) {
		if ( !it->noise[0] || Q_stristr( me.sounds[i], it->noise ) )
			matches[numMatches++] = i;
	}
	clicked = ME_ListRows( x, y, w, rows, numMatches, &me.soundScroll );
	for ( int r = 0; r < rows && me.soundScroll + r < numMatches; r++ ) {
		const char *name = me.sounds[matches[me.soundScroll + r]];

		if ( !Q_stricmp( name, it->noise ) )
			ME_RowHighlight( x, y, w, r + me.soundScroll, me.soundScroll, rows );
		ME_TextClipped( x + 4, y + 1 + r * ME_ROW_H, w - 8, name, meWhite );
	}
	if ( !numMatches )
		ME_Text( x + 4, y + 2, S_COLOR_GREY "None match", meWhite );
	if ( clicked >= 0 ) {
		Q_strncpyz( it->noise, me.sounds[matches[clicked]], sizeof( it->noise ) );
		it->sfxRegistered = qfalse;
		me.field = ME_FIELD_NONE;
		ME_Changed();
	}
	y += rows * ME_ROW_H + 6;
	ME_Text( x, y, va( S_COLOR_GREY "%i of %i: the music, then the sounds", numMatches, me.numSounds ), meWhite );
	ME_Text( x, y + ME_ROW_H, S_COLOR_GREY "Shared, everyone with this client hears it", meWhite );
}

static void ME_DrawStylesTab( float x, float y, float w ) {
	const int rows = 10;
	const float half = ( w - 4 ) * 0.5f, quarter = ( w - 12 ) / 4.0f;
	static int matches[MAX_LIGHT_STYLES];
	int numMatches = 0, clicked, selRow = -1;
	meStyle_t *s;

	if ( ME_Button( x, y, w, me.allStyles ? "Showing all 64" : "Showing the map's", me.allStyles, qtrue ) )
		me.allStyles = (qboolean)!me.allStyles;
	y += ME_CTRL_H + ME_GAP;
	for ( int i = 1; i < MAX_LIGHT_STYLES; i++ ) {
		if ( me.allStyles || me.styles[i].surfaces || me.styles[i].override ) {
			if ( ME_IsSel( ME_SEL_STYLE, i ) )
				selRow = numMatches;
			matches[numMatches++] = i;
		}
	}
	clicked = ME_ListRows( x, y, w, rows, numMatches, &me.styleScroll );
	if ( selRow >= 0 )
		ME_RowHighlight( x, y, w, selRow, me.styleScroll, rows );
	for ( int r = 0; r < rows && me.styleScroll + r < numMatches; r++ ) {
		const int i = matches[me.styleScroll + r];
		const meStyle_t *row = &me.styles[i];
		const char *pattern = row->override ? row->pattern : ME_ServerPattern( i );

		ME_TextClipped( x + 4, y + 1 + r * ME_ROW_H, w - 8, va( "%s%i  " S_COLOR_GREY "%i surfaces  %s%s",
			row->override ? S_COLOR_YELLOW : S_COLOR_WHITE, i, row->surfaces, row->override ? S_COLOR_YELLOW : S_COLOR_WHITE,
			pattern[0] ? pattern : "steady" ), meWhite );
	}
	if ( !numMatches )
		ME_TextClipped( x + 4, y + 2, w - 8, S_COLOR_GREY "No switchable lights were built into this map", meWhite );
	if ( clicked >= 0 )
		ME_Select( ME_SEL_STYLE, matches[clicked] );
	y += rows * ME_ROW_H + 8;

	if ( me.selKind != ME_SEL_STYLE || me.sel < 1 || me.sel >= MAX_LIGHT_STYLES ) {
		ME_Text( x, y, S_COLOR_GREY "A style changes the map's lights built with it,", meWhite );
		ME_Text( x, y + ME_ROW_H, S_COLOR_GREY "and our lights set to follow it", meWhite );
		return;
	}
	s = &me.styles[me.sel];
	ME_Text( x, y, va( "Style %i" S_COLOR_GREY "   the server's: %s", me.sel, ME_ServerPattern( me.sel )[0] ? ME_ServerPattern( me.sel ) : "steady" ), meWhite );
	y += ME_ROW_H + 2;
	if ( ME_Button( x, y, half, s->override ? "Ours" : "The server's", s->override, qtrue ) ) {
		s->override = (qboolean)!s->override;
		if ( s->override && !s->pattern[0] ) {
			Q_strncpyz( s->pattern, ME_ServerPattern( me.sel )[0] ? ME_ServerPattern( me.sel ) : "z", sizeof( s->pattern ) );
			VectorSet( s->color, 1, 1, 1 );
		}
		ME_Changed();
	}
	if ( ME_Button( x + half + 4, y, half, "Blink it", (qboolean)( me.flashStyle == me.sel && cls.realtime < me.flashUntil ), qtrue ) ) {
		me.flashStyle = me.sel;
		me.flashUntil = cls.realtime + ME_FLASH_MS;
	}
	y += ME_CTRL_H + ME_GAP;
	if ( ME_Button( x, y, w, "Go to its lights", qfalse, (qboolean)( s->surfaces > 0 ) ) ) {
		vec3_t center, size;

		VectorAdd( s->mins, s->maxs, center );
		VectorScale( center, 0.5f, center );
		VectorSubtract( s->maxs, s->mins, size );
		ME_GoTo( center, VectorLength( size ) * 0.5f );
	}
	y += ME_CTRL_H + ME_GAP + 4;
	if ( !s->override )
		return;

	ME_TextBox( x, y, w, ME_FIELD_PATTERN, s->pattern, "letters a (dark) to z (full)" );
	y += ME_CTRL_H + ME_GAP;
	for ( int i = 0; i < (int)ARRAY_LEN( meStylePresets ); i++ ) {
		const float bx = x + ( i % 4 ) * ( quarter + 4 ), by = y + ( i / 4 ) * ( ME_CTRL_H + ME_GAP );

		if ( ME_Button( bx, by, quarter, meStylePresets[i].name, (qboolean)!strcmp( s->pattern, meStylePresets[i].pattern ), qtrue ) ) {
			Q_strncpyz( s->pattern, meStylePresets[i].pattern, sizeof( s->pattern ) );
			ME_Changed();
		}
	}
	y += ( ME_CTRL_H + ME_GAP ) * 2;
	{
		int steps = ME_Stepper( x, y, w, "Red", va( "%i%%", ME_Round( s->color[0] * 100 ) ) );

		ME_Adjust( &s->color[0], steps, 0.1f, 0.0f, 1.0f );
		y += ME_CTRL_H + ME_GAP;
		steps = ME_Stepper( x, y, w, "Green", va( "%i%%", ME_Round( s->color[1] * 100 ) ) );
		ME_Adjust( &s->color[1], steps, 0.1f, 0.0f, 1.0f );
		y += ME_CTRL_H + ME_GAP;
		steps = ME_Stepper( x, y, w, "Blue", va( "%i%%", ME_Round( s->color[2] * 100 ) ) );
		ME_Adjust( &s->color[2], steps, 0.1f, 0.0f, 1.0f );
	}
}

static qboolean ME_Project( const vec3_t point, float *x, float *y ) {
	vec3_t d;
	float forward;

	if ( !me.haveView )
		return qfalse;
	VectorSubtract( point, me.viewOrg, d );
	forward = DotProduct( d, me.viewAxis[0] );
	if ( forward < 4.0f )
		return qfalse;
	*x = SCREEN_WIDTH * 0.5f * ( 1.0f - DotProduct( d, me.viewAxis[1] ) / forward / tanf( DEG2RAD( me.viewFovX * 0.5f ) ) );
	*y = SCREEN_HEIGHT * 0.5f * ( 1.0f - DotProduct( d, me.viewAxis[2] ) / forward / tanf( DEG2RAD( me.viewFovY * 0.5f ) ) );
	return (qboolean)( *x >= 0 && *x <= ME_PANEL_X && *y >= 0 && *y <= SCREEN_HEIGHT );
}

// names over what's near, and over what's chosen or under the cursor
static void ME_DrawLabels( void ) {
	int labels = 0;
	float sx, sy;

	for ( int i = 0; i < me.numEnts && labels < ME_MAX_LABELS; i++ ) {
		const meEnt_t *e = &me.ents[i];
		const qboolean mark = (qboolean)( ME_IsSel( ME_SEL_ENT, i ) || ME_IsHover( ME_SEL_ENT, i ) );
		vec3_t center;

		if ( !ME_EntDrawn( e ) )
			continue;
		ME_EntCenter( e, center );
		if ( !mark && Distance( center, me.camOrg ) > ME_LABEL_DIST )
			continue;
		if ( !ME_Project( center, &sx, &sy ) )
			continue;
		ME_Text( sx + 4, sy - 12, va( "%s%s", mark ? S_COLOR_WHITE : S_COLOR_GREY, ME_EntName( e ) ), meWhite );
		labels++;
	}
	for ( int i = 0; i < me.numItems; i++ ) {
		const meItem_t *it = &me.items[i];
		const qboolean mark = (qboolean)( ME_IsSel( ME_SEL_ITEM, i ) || ME_IsHover( ME_SEL_ITEM, i ) );

		if ( !mark || !ME_Project( it->org, &sx, &sy ) )
			continue;
		if ( it->kind == ME_ITEM_MODEL )
			ME_Text( sx + 4, sy - 12, COM_SkipPath( (char *)it->model ), meWhite );
		else if ( it->kind == ME_ITEM_PROP )
			ME_Text( sx + 4, sy - 12, it->removed ? "map prop (removed)" : "map prop", meWhite );
		else if ( it->kind == ME_ITEM_LIGHT )
			ME_Text( sx + 4, sy - 12, "light", meWhite );
		else
			ME_Text( sx + 4, sy - 12, it->inside ? "trigger (you're in it)" : "trigger", meWhite );
	}
}

// the edits kept by name, and shared with everyone
static void ME_DrawLayouts( float x, float y, float w ) {
	const float half = ( w - 4 ) * 0.5f;
	const qboolean armed = (qboolean)( me.confirmTime && cls.realtime - me.confirmTime < 3000 );
	const qboolean experimental = CL_RpHooks_Experimental();
	const qboolean sending = (qboolean)( me.shareSent < me.numShare );
	const int rows = 10;
	int clicked;

	ME_Text( x, y, "Layouts, kept by name", meWhite );
	y += ME_ROW_H + 2;
	ME_TextBox( x, y, w - 64, ME_FIELD_LAYOUT, me.layoutName, "a name for these edits" );
	if ( ME_Button( x + w - 60, y, 60, "Save as", qfalse, ME_LayoutNameOk( me.layoutName ) ) )
		ME_SaveLayout( me.layoutName );
	y += ME_CTRL_H + ME_GAP;
	clicked = ME_ListRows( x, y, w, rows, me.numLayouts, &me.layoutScroll );
	if ( me.layoutSel >= 0 )
		ME_RowHighlight( x, y, w, me.layoutSel, me.layoutScroll, rows );
	for ( int r = 0; r < rows && me.layoutScroll + r < me.numLayouts; r++ )
		ME_TextClipped( x + 4, y + 1 + r * ME_ROW_H, w - 8, me.layouts[me.layoutScroll + r], meWhite );
	if ( !me.numLayouts )
		ME_Text( x + 4, y + 1, S_COLOR_GREY "None for this map yet", meWhite );
	if ( clicked >= 0 ) {
		me.layoutSel = clicked;
		me.confirmTime = 0;
		Q_strncpyz( me.layoutName, me.layouts[clicked], sizeof( me.layoutName ) );
	}
	y += rows * ME_ROW_H + 6;
	if ( ME_Button( x, y, half, "Load", qfalse, (qboolean)( me.layoutSel >= 0 ) ) )
		ME_LoadLayout( me.layouts[me.layoutSel] );
	if ( ME_Button( x + half + 4, y, half, armed ? "Click again" : "Delete", armed, (qboolean)( me.layoutSel >= 0 ) ) ) {
		if ( armed ) {
			me.confirmTime = 0;
			ME_DeleteLayout( me.layouts[me.layoutSel] );
		} else {
			me.confirmTime = cls.realtime;
		}
	}
	y += ME_CTRL_H + ME_GAP;
	if ( ME_Button( x, y, w, "Back to this map's saved edits", qfalse, qtrue ) ) {
		ME_Load();
		ME_Message( "Back to the saved edits" );
	}
	y += ME_CTRL_H + ME_GAP + 10;

	ME_Text( x, y, "Everyone on the map with this client", meWhite );
	y += ME_ROW_H + 2;
	if ( ME_Button( x, y, w, RP_EXPERIMENTAL_LABEL, experimental, qtrue ) ) {
		if ( experimental ) {
			CL_RpHooks_SetExperimental( qfalse );
			ME_Message( "Experimental features off" );
		} else {
			me.experimentalBox = qtrue;
			me.field = ME_FIELD_NONE;
		}
	}
	y += ME_CTRL_H + ME_GAP;
	// off, either of them asks first
	if ( ME_Button( x, y, half, sending ? va( "Sending %i/%i", me.shareSent, me.numShare ) : "Share", sending, (qboolean)!sending ) ) {
		if ( experimental )
			ME_Share();
		else
			me.experimentalBox = qtrue;
	}
	if ( ME_Button( x + half + 4, y, half, "Unshare", qfalse, qtrue ) ) {
		if ( experimental )
			ME_Unshare();
		else
			me.experimentalBox = qtrue;
	}
	y += ME_CTRL_H + ME_GAP;
	if ( me.shared ) {
		ME_TextClipped( x, y, w, S_COLOR_CYAN "Showing edits shared with you", meWhite );
		y += ME_ROW_H + 2;
	}
	ME_Text( x, y, S_COLOR_GREY "Models, props, lights, styles and", meWhite );
	ME_Text( x, y + ME_ROW_H, S_COLOR_GREY "hidden brushes go; triggers stay here.", meWhite );
	ME_Text( x, y + ME_ROW_H * 2, S_COLOR_GREY "Only the look changes: no collision.", meWhite );
	ME_Text( x, y + ME_ROW_H * 3, S_COLOR_GREY "Who joins later needs it shared again.", meWhite );
}

// the warning, over the world: OK turns experimental features on for the session
static void ME_DrawExperimentalBox( void ) {
	const float w = 320, h = 120, x = ( ME_PANEL_X - w ) * 0.5f, y = 160;
	char text[MAX_STRING_CHARS], *line = text;
	float ly = y + 24;

	ME_Box( x, y, w, h, mePanelOpaque );
	ME_Text( x + 8, y + 6, S_COLOR_YELLOW "Experimental features", meWhite );
	// wrapped by words
	Q_strncpyz( text, RP_EXPERIMENTAL_WARNING, sizeof( text ) );
	while ( *line && ly < y + h - 34 ) {
		char *end = line, *lastSpace = NULL, keep;

		while ( *end ) {
			if ( *end == ' ' ) {
				keep = *end;
				*end = '\0';
				const qboolean fits = (qboolean)( ME_TextWidth( line ) <= w - 16 );
				*end = keep;
				if ( !fits )
					break;
				lastSpace = end;
			}
			end++;
		}
		if ( *end && lastSpace )
			end = lastSpace;
		keep = *end;
		*end = '\0';
		ME_Text( x + 8, ly, line, meWhite );
		*end = keep;
		line = *end ? end + 1 : end;
		ly += ME_ROW_H;
	}
	if ( ME_Button( x + 8, y + h - 24, 80, "OK", qfalse, qtrue ) ) {
		CL_RpHooks_SetExperimental( qtrue );
		ME_Message( S_COLOR_YELLOW "Experimental features on, for this session" );
		me.experimentalBox = qfalse;
		return;
	}
	if ( ME_Button( x + w - 88, y + h - 24, 80, "Cancel", qfalse, qtrue ) )
		me.experimentalBox = qfalse;
}

static void ME_DrawEditor( void ) {
	const float x = ME_PANEL_X + 6, w = ME_PANEL_W - 12;
	const float third = ( w - 8 ) / 3.0f, tabW = ( w - 4.0f * ( ME_NUM_TABS - 1 ) ) / ME_NUM_TABS;
	// the warning sits on top and gets the clicks first, but is drawn last
	const qboolean popup = me.experimentalBox, click = me.click;
	const int wheel = me.wheel;
	float y = 8;

	if ( popup )
		me.click = qfalse, me.wheel = 0;

	ME_DrawLabels();

	// top left: what this is, how it went
	ME_Text( 8, 40, va( "Map editor" S_COLOR_GREY "   %s   fov %i", me.map, ME_Round( me.camFov ) ), meWhite );
	if ( me.grabbing )
		ME_Text( 8, 40 + ME_ROW_H, S_COLOR_CYAN "Carrying: it goes where you point; click or G puts it down, Esc puts it back", meWhite );
	if ( me.message[0] && cls.realtime - me.messageTime < ME_MESSAGE_MS )
		ME_TextClipped( 8, 40 + ME_ROW_H * 2, ME_PANEL_X - 16, me.message, meWhite );
	ME_Fill( ME_PANEL_X * 0.5f - 6, SCREEN_HEIGHT * 0.5f, 13, 1, meDim );
	ME_Fill( ME_PANEL_X * 0.5f, SCREEN_HEIGHT * 0.5f - 6, 1, 13, meDim );

	// bottom left: the keys
	{
		static const char *help[] = {
			"Hold right mouse: look and fly   WASD  Space/C up/down",
			"Shift fast  Ctrl slow  wheel zooms while flying",
			"Click: pick, a map prop too   G: move it   Del: delete   Ctrl+D: copy",
			"Ctrl+S: save   Esc: back"
		};
		for ( int i = 0; i < (int)ARRAY_LEN( help ); i++ )
			ME_Text( 8, SCREEN_HEIGHT - 12 - ( ARRAY_LEN( help ) - 1 - i ) * ME_ROW_H, va( S_COLOR_GREY "%s", help[i] ), meWhite );
	}

	ME_Fill( ME_PANEL_X, 0, ME_PANEL_W, SCREEN_HEIGHT, mePanel );
	ME_Text( x, y, va( "Edits%s", me.shared ? S_COLOR_CYAN " (shared with you)" : me.dirty ? S_COLOR_YELLOW " (not saved)" : "" ), meWhite );
	y += ME_ROW_H + 2;
	if ( ME_Button( x, y, third, "Save", qfalse, qtrue ) )
		ME_Save();
	if ( ME_Button( x + third + 4, y, third, "Layouts", me.showLayouts, qtrue ) ) {
		me.showLayouts = (qboolean)!me.showLayouts;
		me.field = ME_FIELD_NONE;
		if ( me.showLayouts )
			ME_ListLayouts();
	}
	if ( ME_Button( x + ( third + 4 ) * 2, y, third, rpc_mapEdits->integer ? "Always on" : "Editor only", (qboolean)( rpc_mapEdits->integer != 0 ), qtrue ) )
		Cvar_SetValue( "rpc_mapEdits", rpc_mapEdits->integer ? 0 : 1 );
	y += ME_CTRL_H + ME_GAP + 4;

	for ( int i = 0; i < ME_NUM_TABS; i++ ) {
		if ( ME_Button( x + i * ( tabW + 4 ), y, tabW, meTabNames[i], (qboolean)( me.tab == i && !me.showLayouts ), qtrue ) ) {
			me.tab = i;
			me.showLayouts = qfalse;
			me.field = ME_FIELD_NONE;
		}
	}
	y += ME_CTRL_H + ME_GAP + 6;

	if ( me.showLayouts )
		ME_DrawLayouts( x, y, w );
	else switch ( me.tab ) {
	case ME_TAB_MAP:		ME_DrawMapTab( x, y, w );		break;
	case ME_TAB_MODELS:		ME_DrawModelsTab( x, y, w );	break;
	case ME_TAB_LIGHTS:		ME_DrawLightsTab( x, y, w );	break;
	case ME_TAB_TRIGGERS:	ME_DrawTriggersTab( x, y, w );	break;
	case ME_TAB_SOUNDS:		ME_DrawSoundsTab( x, y, w );	break;
	default:				ME_DrawStylesTab( x, y, w );	break;
	}

	if ( popup && me.experimentalBox ) {
		me.click = click;
		me.wheel = wheel;
		ME_DrawExperimentalBox();
		me.click = qfalse;
	}

	// a click that's left: on the world it picks, on the panel it leaves the text box
	if ( me.click ) {
		me.field = ME_FIELD_NONE;
		if ( me.clickX < ME_PANEL_X )
			ME_ClickWorld( me.clickX, me.clickY );
	}
	me.click = qfalse;
	me.wheel = 0;

	if ( !me.flying )
		re->DrawStretchPic( me.cursorX, me.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

void CL_MapEditor_Draw( void ) {
	if ( me.open )
		ME_DrawEditor();
}

/*
===============================================================================

Commands

===============================================================================
*/

// mapedit: the editor; mapedit save|reload: the edits; mapedit on|off: shown outside it
void CL_MapEditor_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !arg[0] ) {
		if ( me.open )
			CL_MapEditor_Close();
		else
			CL_MapEditor_Open();
		return;
	}
	if ( !Q_stricmp( arg, "on" ) || !Q_stricmp( arg, "off" ) ) {
		Cvar_Set( "rpc_mapEdits", Q_stricmp( arg, "on" ) ? "0" : "1" );
		return;
	}
	if ( cls.state != CA_ACTIVE ) {
		Com_Printf( "Not on a map.\n" );
		return;
	}
	ME_CheckMap();
	if ( !Q_stricmp( arg, "save" ) ) {
		if ( Cmd_Argv( 2 )[0] )
			ME_SaveLayout( Cmd_Argv( 2 ) );
		else
			ME_Save();
		Com_Printf( "%s\n", me.message );
	} else if ( !Q_stricmp( arg, "load" ) && Cmd_Argv( 2 )[0] ) {
		ME_LoadLayout( Cmd_Argv( 2 ) );
		Com_Printf( "%s\n", me.message );
	} else if ( !Q_stricmp( arg, "reload" ) ) {
		ME_Load();
		Com_Printf( "Map edits reloaded from %s\n", ME_Path() );
	} else if ( !Q_stricmp( arg, "layouts" ) ) {
		ME_ListLayouts();
		Com_Printf( "Layouts for %s, in %s:\n", me.map, ME_LayoutDir() );
		for ( int i = 0; i < me.numLayouts; i++ )
			Com_Printf( "  %s\n", me.layouts[i] );
		if ( !me.numLayouts )
			Com_Printf( "  none\n" );
	} else if ( !Q_stricmp( arg, "share" ) ) {
		ME_Share();
		Com_Printf( "%s\n", me.message );
	} else if ( !Q_stricmp( arg, "unshare" ) ) {
		ME_Unshare();
		Com_Printf( "%s\n", me.message );
	} else {
		Com_Printf( "mapedit : open or close the map editor\n"
			"mapedit save [name] : keep the edits, in %s, or as a layout of that name\n"
			"mapedit load <name> : a layout in place of the edits shown\n"
			"mapedit layouts : the layouts kept for this map\n"
			"mapedit reload : back to the saved edits\n"
			"mapedit share : the edits to everyone on the map with this client (experimental)\n"
			"mapedit unshare : everyone back to their own\n"
			"mapedit on|off : show them outside the editor too, or not\n", ME_Path() );
	}
}

void CL_MapEditor_Init( void ) {
	memset( &me, 0, sizeof( me ) );
	me.sel = -1;
	me.camFov = 90.0f;
	me.flashStyle = -1;
	me.kvEnt = -1;
	me.showTriggers = me.showBrushes = qtrue;
	me.showPoints = qtrue;
	for ( int i = 0; i < MAX_LIGHT_STYLES; i++ )
		VectorSet( me.styles[i].color, 1, 1, 1 );
	rpc_mapEdits = Cvar_Get( "rpc_mapEdits", "1", CVAR_ARCHIVE_ND, "Show the map edits made with /mapedit outside the editor too" );
	rpc_mapShare = Cvar_Get( "rpc_mapShare", "1", CVAR_ARCHIVE_ND, "Show the map edits an admin shares with everyone" );
	me.layoutSel = -1;
}

// the cgame is going, maybe to another map: the renderer's handles go with it
void CL_MapEditor_Shutdown( void ) {
	CL_MapEditor_Close();
	ME_ForgetHandles();
	me.haveView = qfalse;
	me.havePlayerView = qfalse;
	// the pure pk3 list can change with the next map or server
	me.indexed = qfalse;
	// the map's own are read again when it's up; ours stay if it's this map again
	me.reparse = qtrue;
	me.numCandSeeds = 0;
	if ( me.selKind == ME_SEL_PROP )
		me.selKind = ME_SEL_NONE;
}
