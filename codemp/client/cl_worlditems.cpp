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

// cl_worlditems.cpp -- things in the world: kept by C1B3R for each server and map (its /v2/world,
// see OpenJK-rpmod-c1b3r), seen by every game linked to it. Four kinds:
//
//   item     an item a player dropped from their inventory (or an admin put there): anyone picks it
//            up. A datapad keeps its notes. A player's drop goes back to them after a day
//   object   a model with a title and a description, and loot to take or not
//   zone     an invisible box: standing in it, the use key reads it (and its loot); or walked into,
//            for the storyteller only (nothing to see or use)
//   body     a player model lying in a pose (a death), searched for its description and loot
//
// Looked at within reach (or stood in, for a zone), the use key opens the reader: the title, the
// description, the loot with Take, Pick up and the notes for an item. A thing with a search time
// is held on: the player works at it (RPMod's emconsole) with a bar. RPMod admins build them with
// /world, an editor over the world with the gizmo, the same keys as the holster's live mode. C1B3R
// decides who's an admin (RPMod's own rank), and tells every game on the server when they change
// (cmd:world/<version>).
//
// A thing can have a story name: the storyteller waits on it being used ("someone uses"), or a
// walk-in zone being walked into ("world zone"), as the map editor's triggers were. Used with
// nothing to read (no description, no loot), it's only reached for and the storyteller told. A
// story not there yet hides it, or shows its Too early message instead.

#include <string>
#include <vector>
#include "client.h"
#include "qcommon/cm_public.h"
#include "cl_presence.h"
#include "cl_widgets.h"
#include "cl_gizmo.h"

#define WT_CMD				"world"
#define WT_MAX				200			// on a map, as C1B3R keeps
#define WT_BODIES			32			// bodies drawn at once: their models are loaded
#define WT_LOOT_MAX			20
#define WT_DESCR			4001
#define WT_REACH			96.0f		// looked at from this near: used
#define WT_PLACE_REACH		128.0f		// dropped or built where you aim, this near
#define WT_NEAR				768.0f		// world near lists the ones this near
#define WT_RETRY_MS			20000
#define WT_WAIT_MS		1500	// C1B3R not yet sure of this load of the map: asked again after this
#define WT_BODY_UP			24.0f		// a player model's origin is this far over its feet
#define WT_CROUCH_KEY		"1021"		// the crouch ending a held search: its own key number
#define WT_CROUCH_MS		120
#define WT_FLY_SPEED		400.0f
#define WT_MESSAGE_MS		5000
#define WT_INV_MAX			100
#define WT_STORY_LEN		48			// a story name: letters, digits, _ - . (as C1B3R takes)
#define WT_EARLY_MS			3000		// a "too early" message shown this long

enum { WT_ITEM, WT_OBJECT, WT_ZONE, WT_BODY, WT_NUM_KINDS };
static const char *wtKinds[WT_NUM_KINDS] = { "item", "object", "zone", "body" };
static const char *wtKindTitles[WT_NUM_KINDS] = { "Item", "Object", "Zone", "Body" };

enum { WT_LOOT_NONE, WT_LOOT_SHARED, WT_LOOT_EACH, WT_NUM_LOOTS };
static const char *wtLoots[WT_NUM_LOOTS] = { "none", "shared", "each" };
static const char *wtLootTitles[WT_NUM_LOOTS] = { "No loot", "Shared", "Each player" };

// a body's poses: deaths held on their last frame, and a few to lie or sit
static const char *wtPoses[] = {
	"BOTH_DEATH1", "BOTH_DEATH2", "BOTH_DEATH4", "BOTH_DEATH5", "BOTH_DEATH9", "BOTH_DEATH10", "BOTH_DEATH13",
	"BOTH_DEATH18", "BOTH_DEAD21", "BOTH_SLEEP1", "BOTH_SIT2", "BOTH_STAND1",
};
#define WT_NUM_POSES	( (int)ARRAY_LEN( wtPoses ) )

// how long one built is kept: never, till the map loads again (-1), or seconds from now
#define WT_TILL_MAP		-1
static const struct { const char *name; int secs; } wtExpiries[] = {
	{ "Never", 0 }, { "Till the map loads again", WT_TILL_MAP }, { "1 hour", 3600 }, { "1 day", 86400 }, { "7 days", 7 * 86400 }, { "30 days", 30 * 86400 },
};
#define WT_NUM_EXPIRIES	( (int)ARRAY_LEN( wtExpiries ) )

typedef struct {
	int			id;					// C1B3R's number for it; -1 one being built, not saved yet
	int			kind;
	int			item;				// the item it is (kind item), 0 none
	char		title[64];
	char		model[MAX_QPATH];
	char		skin[64];
	char		anim[48];
	vec3_t		pos;				// a body's: where its feet lie
	vec3_t		ang;
	float		scale;
	vec3_t		size;				// a zone's half size
	float		usetime;			// seconds the use key's held to open it
	int			loot;
	qboolean	datapad, mine, hasLoot;
	qboolean	text;				// it has a description to read
	qboolean	mapOnly;			// gone when the server's map loads again
	qboolean	ends;				// gone at a time set
	char		story[WT_STORY_LEN];	// the storyteller's name for it, "" none
	qboolean	walk;				// a zone walked into (for the storyteller), not used
	char		verb[25];			// a used zone's word before its title (Hack, Open), "" none
	qboolean	panel;				// a used zone opens its panel; else it's only a trigger for the storyteller

	qboolean	boundsKnown;		// its model's own box, unscaled
	vec3_t		mins, maxs;
} wtThing_t;

typedef struct {
	int			item;
	char		name[64];
	char		kind[16];
	char		model[MAX_QPATH];
	char		descr[320];
	qboolean	taken;
} wtLoot_t;

// one of your items, to put in a thing
typedef struct {
	int			id;
	char		name[64];
	char		kind[16];
	char		model[MAX_QPATH];
	qboolean	starter;
} wtMine_t;

static const float wtPanel[4]		= { 0.0f, 0.0f, 0.08f, 0.88f };
static const float wtBorder[4]		= { 0.5f, 0.5f, 0.7f, 1.0f };
static const float wtRow[4]			= { 0.15f, 0.15f, 0.25f, 0.8f };
static const float wtRowHover[4]	= { 0.25f, 0.25f, 0.4f, 0.9f };
static const float wtRowOn[4]		= { 0.35f, 0.24f, 0.05f, 0.9f };
static const float wtField[4]		= { 0.05f, 0.05f, 0.12f, 0.95f };
static const float wtOrange[4]		= { 1.0f, 0.682f, 0.0f, 1.0f };
static const float wtWhite[4]		= { 1.0f, 1.0f, 1.0f, 1.0f };
static const float wtDim[4]			= { 0.6f, 0.6f, 0.7f, 1.0f };
static const float wtYellow[4]		= { 1.0f, 0.85f, 0.3f, 1.0f };
static const float wtGreen[4]		= { 0.4f, 1.0f, 0.4f, 1.0f };
static const float wtZone[4]		= { 0.3f, 0.9f, 1.0f, 0.9f };
static const float wtZoneSel[4]		= { 1.0f, 0.85f, 0.2f, 1.0f };
static const float wtZoneWalk[4]	= { 0.4f, 1.0f, 0.5f, 0.9f };

#define WT_CTRL_H		16.0f
#define WT_TEXT			0.8f
#define WT_SMALL		0.65f
#define WT_ITEM_SCALE	0.6f
#define WT_LINE_H		13.0f

// what's on this map, and using it
static struct {
	wtThing_t	things[WT_MAX];
	int			num;
	int			version;
	char		mapAsked[MAX_QPATH];	// the map they were asked for
	int			serverId;				// and the load of it: another starts blank
	char		session[64];
	qboolean	want;					// to be asked
	int			retryAt;

	int			aim;					// the one looked at, -1 none
	int			zone;					// the zone stood in, -1 none
	qboolean	useDown;
	int			holdId;					// held on to open: its id, 0 none
	int			holdStart;
	qboolean	holdDone;
	qboolean	holdEmote;
	int			crouchUntil;
	char		earlyText[96];			// a story's "too early", shown instead of using it
	int			earlyTime;
	int			font, titleFont;
	sfxHandle_t	clickSound;
} wt;

// bodies' models, by the thing they're for
static struct {
	int				id;					// 0 a free slot
	qboolean		seen;
	clPosedBody_t	body;
} wtBodies[WT_BODIES];

// the reader: a thing opened
static struct {
	qboolean	open;
	int			id;
	wtThing_t	thing;
	qboolean	loading, known;
	char		descr[WT_DESCR];
	wtLoot_t	loot[WT_LOOT_MAX];
	int			numLoot;
	float		scroll;
	int			lootScroll;
	char		message[160];
	int			messageTime;
	qboolean	messageBad;
} wr;

// the editor
enum { WE_MOVE, WE_TURN, WE_SIZE };
enum { WF_NONE, WF_TITLE, WF_MODEL, WF_SKIN, WF_ANIM, WF_DESCR, WF_STORY, WF_VERB, WF_MAKE_NAME, WF_MAKE_DESCR, WF_MAKE_MODEL };

// the kinds of item made inside a container, as C1B3R has them
static const char *wtMakeKinds[] = { "misc", "datapad", "commlink", "hilt" };	// ships come with 0.11.0
#define WT_NUM_MAKE_KINDS	( (int)ARRAY_LEN( wtMakeKinds ) )
enum { WE_DRAG_NONE = -1, WE_DRAG_GIZMO = 1 };

static const float weMoveSteps[3] = { 1.0f, 4.0f, 16.0f };
static const float weTurnSteps[3] = { 1.0f, 5.0f, 15.0f };
static const vec3_t weWorldAxes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

static struct {
	qboolean	open;
	int			sel;				// the thing edited: its id; -1 a new one, 0 none
	wtThing_t	d;					// what's edited, drawn in its place
	char		descr[WT_DESCR];
	qboolean	descrKnown;			// asked of C1B3R (an old one's description and loot)
	qboolean	changed;
	int			expiry;				// wtExpiries, -1 left as it is
	wtLoot_t	loot[WT_LOOT_MAX];
	int			numLoot;
	qboolean	picking;			// your items listed, to put one in it
	qboolean	making;				// a new item made straight inside it
	char		makeName[64], makeDescr[256], makeModel[MAX_QPATH];
	int			makeKind;			// wtMakeKinds
	qboolean	modelPick;			// the model manager open to choose its model; the editor waits
	int			modelPickFor;		// the thing it's chosen for (we.sel then)
	wtMine_t	mine[WT_INV_MAX];
	int			numMine;
	int			mineScroll;
	int			editLootScroll;		// the editor's "In it" list
	qboolean	confirmRemove;
	int			listScroll;
	int			focus;

	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;
	int			wheel;				// the wheel's steps for an open list (negative up)
	qboolean	mouseDown;
	qboolean	held[MAX_KEYS];

	int			mode;
	int			step;
	qboolean	gzHidden;
	int			dragging;
	gzDrag_t	gzDrag;
	int			gzPart, gzHot;
	vec3_t		gzDir;
	vec3_t		startPos, startAng, startSize;
	vec3_t		startAxes[3];
	float		gzTurned, gzLastAngle, gzSign;

	gzView_t	view;
	qboolean	haveView;
	int			viewFrame;
	qboolean	freeCam;
	vec3_t		camOrg, camAng;
	int			lastFrameTime;

	char		message[160];
	int			messageTime;
	qboolean	messageBad;
} we;

static httpRequest_t wtListHttp, wtActHttp, wtOpenHttp, wtMineHttp, wtDumpHttp;
static char wtDumpWhat[8], wtDumpName[MAX_QPATH];
static char wtActWhat[16];
static int wtOpenFor;					// the reader's or the editor's: its id
static qboolean wtOpenForEditor;

static void WR_Close( void );
static void WE_Close( void );
static void WE_Select( int id );
static void WE_PickFrame( void );

/*
===============================================================================

Talking to C1B3R

===============================================================================
*/

// the map as C1B3R names it: mp/ffa3, not maps/mp/ffa3.bsp
static const char *WT_Map( void ) {
	static char map[MAX_QPATH];
	const char *m = cl.mapname;

	if ( !Q_stricmpn( m, "maps/", 5 ) )
		m += 5;
	Q_strncpyz( map, m, sizeof( map ) );
	COM_StripExtension( map, map, sizeof( map ) );
	return map;
}

// whether to offer building and removing: C1B3R decides, by RPMod's own word for the account. Not
// offered only once the server has said this player has no admin rank; until it's said, it's asked
qboolean CL_WorldItems_Admin( void ) {
	const char *rank = CL_RpAdmin_Rank();

	if ( !rank ) {
		CL_RpAdmin_AskQuietly();
		return qtrue;
	}
	return (qboolean)( rank[0] != '\0' );
}

static qboolean WT_Post( httpRequest_t *req, const char *path, const std::string &fields ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session ) {
		Com_Printf( "World: not connected to C1B3R (/c1b3r says why)\n" );
		return qfalse;
	}
	if ( req->state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += fields + "}";
	return CL_Presence_Post( req, path, body );
}

// one at a time; its answer said
static qboolean WT_Act( const char *what, const char *path, const std::string &fields ) {
	if ( wtActHttp.state == HTTP_BUSY ) {
		Com_Printf( "World: wait, the last one is still going\n" );
		return qfalse;
	}
	Q_strncpyz( wtActWhat, what, sizeof( wtActWhat ) );
	return WT_Post( &wtActHttp, path, fields );
}

static int WT_Split( char *line, char **field, int max ) {
	int n = 0;

	for ( char *f = line; f && n < max; n++ ) {
		field[n] = f;
		f = strchr( f, '\t' );
		if ( f )
			*f++ = '\0';
	}
	return n;
}

// each line of an answer, split at its tabs; the line itself kept long enough for a description
static void WT_Lines( const char *answer, void ( *each )( char **f, int n, void *ctx ), void *ctx ) {
	std::string all( answer );
	size_t at = 0;
	std::vector<char> line;

	while ( at < all.size() ) {
		size_t end = all.find( '\n', at );
		char *f[20];

		if ( end == std::string::npos )
			end = all.size();
		line.assign( all.begin() + at, all.begin() + end );
		line.push_back( '\0' );
		at = end + 1;
		each( f, WT_Split( line.data(), f, 20 ), ctx );
	}
}

static int WT_KindOf( const char *name ) {
	for ( int k = 0; k < WT_NUM_KINDS; k++ ) {
		if ( !Q_stricmp( name, wtKinds[k] ) )
			return k;
	}
	return WT_OBJECT;
}

static int WT_LootOf( const char *name ) {
	for ( int k = 0; k < WT_NUM_LOOTS; k++ ) {
		if ( !Q_stricmp( name, wtLoots[k] ) )
			return k;
	}
	return WT_LOOT_NONE;
}

static void WT_Unfield( char *out, int size, const char *in ) {
	Q_strncpyz( out, CL_Presence_Unfield( in ).c_str(), size );
}

typedef struct {
	qboolean	first;
	qboolean	otherMap;
	qboolean	wait;			// C1B3R doesn't know this load of the map yet: none, asked again soon
} wtListCtx_t;

// ok <version> <map>, then thing <id> <kind> <item> <title> <model> <skin> <anim> <x y z> <p y r>
// <scale> <sx sy sz> <usetime> <loot> <flags> [<story> <walk>]
static void WT_ListLine( char **f, int n, void *ctx ) {
	wtListCtx_t *c = (wtListCtx_t *)ctx;
	wtThing_t *t;

	if ( c->first ) {
		c->first = qfalse;
		if ( n >= 2 )
			wt.version = atoi( f[1] );
		// for another map than this one now (it changed as it was asked): asked again
		if ( n >= 3 && f[2][0] && Q_stricmp( f[2], WT_Map() ) )
			c->otherMap = qtrue;
		if ( n >= 4 && !strcmp( f[3], "wait" ) )
			c->wait = qtrue;
		return;
	}
	if ( c->otherMap || strcmp( f[0], "thing" ) || n < 15 || wt.num >= WT_MAX )
		return;
	t = &wt.things[wt.num++];
	memset( t, 0, sizeof( *t ) );
	t->id = atoi( f[1] );
	t->kind = WT_KindOf( f[2] );
	t->item = atoi( f[3] );
	WT_Unfield( t->title, sizeof( t->title ), f[4] );
	WT_Unfield( t->model, sizeof( t->model ), f[5] );
	WT_Unfield( t->skin, sizeof( t->skin ), f[6] );
	WT_Unfield( t->anim, sizeof( t->anim ), f[7] );
	sscanf( f[8], "%f %f %f", &t->pos[0], &t->pos[1], &t->pos[2] );
	sscanf( f[9], "%f %f %f", &t->ang[0], &t->ang[1], &t->ang[2] );
	t->scale = (float)atof( f[10] );
	if ( t->scale <= 0.0f )
		t->scale = 1.0f;
	sscanf( f[11], "%f %f %f", &t->size[0], &t->size[1], &t->size[2] );
	t->usetime = Q_max( 0.0f, (float)atof( f[12] ) );
	t->loot = WT_LootOf( f[13] );
	t->datapad = (qboolean)( Q_stristr( f[14], "datapad" ) != NULL );
	t->mine = (qboolean)( Q_stristr( f[14], "mine" ) != NULL );
	t->hasLoot = (qboolean)( Q_stristr( f[14], "loot" ) != NULL );
	t->text = (qboolean)( Q_stristr( f[14], "text" ) != NULL );
	t->mapOnly = (qboolean)( Q_stristr( f[14], "maponly" ) != NULL );
	t->ends = (qboolean)( Q_stristr( f[14], "ends" ) != NULL );
	t->panel = qtrue;
	if ( n >= 19 ) {
		WT_Unfield( t->verb, sizeof( t->verb ), f[17] );
		t->panel = (qboolean)( t->kind != WT_ZONE || atoi( f[18] ) );
	}
	if ( n >= 17 ) {
		WT_Unfield( t->story, sizeof( t->story ), f[15] );
		t->walk = (qboolean)( t->kind == WT_ZONE && atoi( f[16] ) );
	}
}

static void WT_FreeBody( int slot ) {
	CL_Extras_FreeBody( &wtBodies[slot].body );
	wtBodies[slot].id = 0;
}

// bodies no longer on the map let go (the one being built kept)
static void WT_KeepBodies( void ) {
	for ( int s = 0; s < WT_BODIES; s++ ) {
		qboolean found = qfalse;

		if ( !wtBodies[s].id )
			continue;
		if ( wtBodies[s].id == -1 && we.open && we.sel == -1 )
			continue;
		for ( int i = 0; i < wt.num && !found; i++ )
			found = (qboolean)( wt.things[i].id == wtBodies[s].id && wt.things[i].kind == WT_BODY );
		if ( !found )
			WT_FreeBody( s );
	}
}

static void WT_ReadList( const char *answer ) {
	wtListCtx_t c = { qtrue, qfalse, qfalse };

	wt.num = 0;
	WT_Lines( answer, WT_ListLine, &c );
	if ( c.otherMap ) {
		wt.num = 0;
		wt.want = qtrue;
	}
	if ( c.wait ) {
		wt.num = 0;
		wt.want = qtrue;
		wt.retryAt = cls.realtime + WT_WAIT_MS;
	}
	WT_KeepBodies();
}

static wtThing_t *WT_Find( int id ) {
	for ( int i = 0; i < wt.num; i++ ) {
		if ( wt.things[i].id == id )
			return &wt.things[i];
	}
	return NULL;
}

// cmd:world/<version>: they changed on this server
void CL_WorldItems_Hook( char **argv, int argc ) {
	if ( argc < 1 || atoi( argv[0] ) != wt.version )
		wt.want = qtrue;
	wt.retryAt = 0;
}

// open <id>: ok <id> <kind> <title>, descr <text>, loot <item> <name> <kind> <model> <descr> <taken>
typedef struct {
	char		*descr;
	wtLoot_t	*loot;
	int			*numLoot;
} wtOpenCtx_t;

// a datapad with no model of its own is shown as the one held in the hand (the holster's)
static void WT_DatapadModel( const char *kind, char *model, int size ) {
	if ( !model[0] && !Q_stricmp( kind, "datapad" ) )
		Q_strncpyz( model, CL_Holster_DatapadModel(), size );
}

static void WT_OpenLine( char **f, int n, void *ctx ) {
	wtOpenCtx_t *c = (wtOpenCtx_t *)ctx;

	if ( !strcmp( f[0], "descr" ) && n >= 2 ) {
		WT_Unfield( c->descr, WT_DESCR, f[1] );
	} else if ( !strcmp( f[0], "loot" ) && n >= 7 && *c->numLoot < WT_LOOT_MAX ) {
		wtLoot_t *l = &c->loot[( *c->numLoot )++];

		memset( l, 0, sizeof( *l ) );
		l->item = atoi( f[1] );
		WT_Unfield( l->name, sizeof( l->name ), f[2] );
		Q_strncpyz( l->kind, f[3], sizeof( l->kind ) );
		WT_Unfield( l->model, sizeof( l->model ), f[4] );
		WT_DatapadModel( l->kind, l->model, sizeof( l->model ) );
		WT_Unfield( l->descr, sizeof( l->descr ), f[5] );
		l->taken = (qboolean)( atoi( f[6] ) != 0 );
	}
}

static void WT_AskOpen( int id, qboolean editor ) {
	if ( wtOpenHttp.state == HTTP_BUSY )
		return;
	if ( WT_Post( &wtOpenHttp, "/v2/world/open", va( ",\"id\":%i", id ) ) ) {
		wtOpenFor = id;
		wtOpenForEditor = editor;
	}
}

// item <id> <kind> <shown> <name> <descr> <model> <data>: yours, to put in a thing
static void WT_MineLine( char **f, int n, void *ctx ) {
	wtMine_t *m;

	if ( strcmp( f[0], "item" ) || n < 8 || we.numMine >= WT_INV_MAX )
		return;
	m = &we.mine[we.numMine];
	memset( m, 0, sizeof( *m ) );
	m->id = atoi( f[1] );
	Q_strncpyz( m->kind, f[2], sizeof( m->kind ) );
	WT_Unfield( m->name, sizeof( m->name ), f[4] );
	WT_Unfield( m->model, sizeof( m->model ), f[6] );
	WT_DatapadModel( m->kind, m->model, sizeof( m->model ) );
	m->starter = (qboolean)( strstr( CL_Presence_Unfield( f[7] ).c_str(), "\"starter\"" ) != NULL );
	if ( !m->starter )
		we.numMine++;
}

/*
===============================================================================

Where they are, and drawn

===============================================================================
*/

static void WT_Eye( vec3_t eye, vec3_t dir ) {
	vec3_t ang;

	VectorCopy( cl.snap.ps.origin, eye );
	eye[2] += cl.snap.ps.viewheight;
	for ( int i = 0; i < 3; i++ )
		ang[i] = cl.viewangles[i] + SHORT2ANGLE( cl.snap.ps.delta_angles[i] );
	AngleVectors( ang, dir, NULL, NULL );
}

static const char *WT_BodyModel( const wtThing_t *t ) {
	const char *model = t->model[0] ? t->model : "kyle";

	if ( !Q_stricmp( COM_GetExtension( model ), "glm" ) || strchr( model, '/' ) || !t->skin[0] )
		return model;
	return va( "%s/%s", model, t->skin );
}

static clPosedBody_t *WT_Body( int id ) {
	int free = -1;

	for ( int s = 0; s < WT_BODIES; s++ ) {
		if ( wtBodies[s].id == id )
			return &wtBodies[s].body;
		if ( !wtBodies[s].id && free < 0 )
			free = s;
	}
	if ( free < 0 )
		return NULL;
	memset( &wtBodies[free], 0, sizeof( wtBodies[free] ) );
	wtBodies[free].id = id;
	wtBodies[free].body.cfg = -1;
	return &wtBodies[free].body;
}

// its model on ent, where it is and turned, sized; qfalse when there's nothing to draw
static qboolean WT_Entity( wtThing_t *t, refEntity_t *ent ) {
	memset( ent, 0, sizeof( *ent ) );
	ent->reType = RT_MODEL;
	if ( t->kind == WT_ZONE )
		return qfalse;
	if ( t->kind == WT_BODY ) {
		clPosedBody_t *b = WT_Body( t->id );

		if ( !b || !CL_Extras_PoseBody( b, WT_BodyModel( t ), t->anim[0] ? t->anim : wtPoses[0] ) )
			return qfalse;
		ent->ghoul2 = b->g2;
		VectorSet( ent->modelScale, t->scale, t->scale, t->scale );
		VectorCopy( t->pos, ent->origin );
		ent->origin[2] += WT_BODY_UP * t->scale;
		ent->radius = 64.0f * t->scale;
	} else {
		const char *model = t->model;

		if ( !model[0] && t->datapad )
			model = CL_Holster_DatapadModel();
		if ( !model[0] || !CL_Holster_ModelEntity( model, ent ) )
			return qfalse;
		VectorCopy( t->pos, ent->origin );
		ent->radius = 32.0f * t->scale;
		if ( !t->boundsKnown ) {
			t->boundsKnown = qtrue;
			if ( ent->hModel ) {
				re->ModelBounds( ent->hModel, t->mins, t->maxs );
			} else if ( CL_GlmBounds( model, t->mins, t->maxs ) ) {
				VectorSet( t->mins, -8, -8, -8 );
				VectorSet( t->maxs, 8, 8, 8 );
			}
		}
	}
	VectorCopy( ent->origin, ent->oldorigin );
	VectorCopy( t->ang, ent->angles );
	AnglesToAxis( t->ang, ent->axis );
	if ( t->scale != 1.0f ) {
		if ( ent->ghoul2 ) {
			VectorSet( ent->modelScale, t->scale, t->scale, t->scale );
		} else {
			for ( int a = 0; a < 3; a++ )
				VectorScale( ent->axis[a], t->scale, ent->axis[a] );
			ent->nonNormalizedAxes = qtrue;
		}
	}
	// lit from a little over the floor it lies on
	ent->renderfx = RF_LIGHTING_ORIGIN;
	VectorCopy( t->pos, ent->lightingOrigin );
	ent->lightingOrigin[2] += 8.0f;
	ent->shaderRGBA[0] = ent->shaderRGBA[1] = ent->shaderRGBA[2] = ent->shaderRGBA[3] = 255;
	return qtrue;
}

// the box it's found in, in the world
static void WT_Box( const wtThing_t *t, vec3_t mins, vec3_t maxs ) {
	if ( t->kind == WT_ZONE ) {
		VectorSubtract( t->pos, t->size, mins );
		VectorAdd( t->pos, t->size, maxs );
		return;
	}
	if ( t->kind == WT_BODY ) {
		const float r = 28.0f * t->scale;

		VectorSet( mins, t->pos[0] - r, t->pos[1] - r, t->pos[2] );
		VectorSet( maxs, t->pos[0] + r, t->pos[1] + r, t->pos[2] + 20.0f * t->scale );
		return;
	}
	if ( !t->boundsKnown ) {
		VectorSet( mins, t->pos[0] - 8, t->pos[1] - 8, t->pos[2] - 2 );
		VectorSet( maxs, t->pos[0] + 8, t->pos[1] + 8, t->pos[2] + 12 );
		return;
	}
	// its own box's corners, turned and sized, and the box round them
	{
		matrix3_t axis;

		AnglesToAxis( t->ang, axis );
		ClearBounds( mins, maxs );
		for ( int c = 0; c < 8; c++ ) {
			const vec3_t corner = { ( c & 1 ) ? t->maxs[0] : t->mins[0], ( c & 2 ) ? t->maxs[1] : t->mins[1], ( c & 4 ) ? t->maxs[2] : t->mins[2] };
			vec3_t p;

			for ( int k = 0; k < 3; k++ )
				p[k] = t->pos[k] + t->scale * ( axis[0][k] * corner[0] + axis[1][k] * corner[1] + axis[2][k] * corner[2] );
			AddPointToBounds( p, mins, maxs );
		}
		// too small to aim at: a little bigger
		for ( int k = 0; k < 3; k++ ) {
			if ( maxs[k] - mins[k] < 8.0f ) {
				const float mid = ( mins[k] + maxs[k] ) * 0.5f;

				mins[k] = mid - 4.0f;
				maxs[k] = mid + 4.0f;
			}
		}
	}
}

// the ray's way into the box: how far, -1 when it misses
static float WT_RayBox( const vec3_t org, const vec3_t dir, const vec3_t mins, const vec3_t maxs, float reach ) {
	float tmin = 0.0f, tmax = reach;

	for ( int i = 0; i < 3; i++ ) {
		float t1, t2;

		if ( fabsf( dir[i] ) < 0.0001f ) {
			if ( org[i] < mins[i] || org[i] > maxs[i] )
				return -1.0f;
			continue;
		}
		t1 = ( mins[i] - org[i] ) / dir[i];
		t2 = ( maxs[i] - org[i] ) / dir[i];
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
	return tmin;
}

// a story not there yet hides it
static qboolean WT_Hidden( const wtThing_t *t ) {
	return (qboolean)( t->story[0] && CL_Storyteller_UseLock( t->story, NULL ) == 1 );
}

// the one looked at within reach (not a zone), not behind a wall; -1 none
static int WT_Aimed( void ) {
	vec3_t eye, dir, end, mins, maxs;
	trace_t tr;
	float best;
	int found = -1;

	if ( !wt.num )
		return -1;
	WT_Eye( eye, dir );
	VectorMA( eye, WT_REACH, dir, end );
	CM_BoxTrace( &tr, eye, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
	best = tr.fraction * WT_REACH + 8.0f;
	for ( int i = 0; i < wt.num; i++ ) {
		float t;

		if ( wt.things[i].kind == WT_ZONE || WT_Hidden( &wt.things[i] ) )
			continue;
		WT_Box( &wt.things[i], mins, maxs );
		t = WT_RayBox( eye, dir, mins, maxs, WT_REACH );
		if ( t >= 0.0f && t < best ) {
			best = t;
			found = i;
		}
	}
	return found;
}

// the zone you stand in (the smallest, when they overlap); -1 none
static int WT_ZoneIn( void ) {
	vec3_t pmins, pmaxs, mins, maxs;
	float best = 0.0f;
	int found = -1;

	VectorSet( pmins, cl.snap.ps.origin[0] - 15, cl.snap.ps.origin[1] - 15, cl.snap.ps.origin[2] - 24 );
	VectorSet( pmaxs, cl.snap.ps.origin[0] + 15, cl.snap.ps.origin[1] + 15, cl.snap.ps.origin[2] + 40 );
	for ( int i = 0; i < wt.num; i++ ) {
		const wtThing_t *t = &wt.things[i];
		qboolean inside = qtrue;
		float volume;

		if ( t->kind != WT_ZONE || t->walk || WT_Hidden( t ) )
			continue;
		WT_Box( t, mins, maxs );
		for ( int k = 0; k < 3 && inside; k++ )
			inside = (qboolean)( pmaxs[k] >= mins[k] && pmins[k] <= maxs[k] );
		volume = t->size[0] * t->size[1] * t->size[2];
		if ( inside && ( found < 0 || volume < best ) ) {
			best = volume;
			found = i;
		}
	}
	return found;
}

// the storyteller's: the story name of the thing looked at or the zone stood in (walk: the walk-in
// zone stood in); "" it has none, NULL nothing there
const char *CL_WorldItems_AimedStory( qboolean walk ) {
	int i;

	if ( cls.state != CA_ACTIVE )
		return NULL;
	if ( walk ) {
		const char *found = NULL;

		for ( i = 0; i < wt.num; i++ ) {
			vec3_t mins, maxs;
			const float *o = cl.snap.ps.origin;

			if ( !wt.things[i].walk )
				continue;
			WT_Box( &wt.things[i], mins, maxs );
			if ( o[0] + 15 >= mins[0] && o[0] - 15 <= maxs[0] && o[1] + 15 >= mins[1] && o[1] - 15 <= maxs[1]
				&& o[2] + 40 >= mins[2] && o[2] - 24 <= maxs[2] && ( !found || wt.things[i].story[0] ) )
				found = wt.things[i].story;
		}
		return found;
	}
	i = WT_Aimed();
	if ( i < 0 )
		i = WT_ZoneIn();
	return i >= 0 ? wt.things[i].story : NULL;
}

// the storyteller's: a walk-in zone's box, by its story name (the first, when several share it)
qboolean CL_WorldItems_ZoneBox( const char *story, vec3_t mins, vec3_t maxs ) {
	if ( !story || !story[0] )
		return qfalse;
	for ( int i = 0; i < wt.num; i++ ) {
		if ( wt.things[i].walk && !Q_stricmp( wt.things[i].story, story ) ) {
			WT_Box( &wt.things[i], mins, maxs );
			return qtrue;
		}
	}
	return qfalse;
}

// the storyteller's: the map's story names, each once; walk-in zones only, or the things used
int CL_WorldItems_Stories( char names[][48], int max, qboolean walkOnly ) {
	int n = 0;

	for ( int i = 0; i < wt.num && n < max; i++ ) {
		const wtThing_t *t = &wt.things[i];
		qboolean seen = qfalse;

		if ( !t->story[0] || t->walk != walkOnly )
			continue;
		for ( int k = 0; k < n && !seen; k++ )
			seen = (qboolean)!Q_stricmp( names[k], t->story );
		if ( !seen )
			Q_strncpyz( names[n++], t->story, 48 );
	}
	return n;
}

// each one where it lies, in every main view; the one edited as it's being edited
void CL_WorldItems_AddToScene( const refdef_t *fd ) {
	if ( fd->rdflags & ( RDF_NOWORLDMODEL | RDF_SKYBOXPORTAL | RDF_AUTOMAP ) || cls.state != CA_ACTIVE )
		return;
	for ( int i = 0; i < wt.num; i++ ) {
		wtThing_t *t = &wt.things[i];
		refEntity_t ent;

		if ( we.open && we.sel == t->id )
			t = &we.d;
		if ( WT_Entity( t, &ent ) )
			re->AddRefEntityToScene( &ent );
		if ( t == &we.d ) {
			wt.things[i].boundsKnown = we.d.boundsKnown;
			VectorCopy( we.d.mins, wt.things[i].mins );
			VectorCopy( we.d.maxs, wt.things[i].maxs );
		}
	}
	if ( we.open && we.sel == -1 ) {
		refEntity_t ent;

		if ( WT_Entity( &we.d, &ent ) )
			re->AddRefEntityToScene( &ent );
	}
}

void CL_WorldItems_Disconnect( void ) {
	WR_Close();
	WE_Close();
	wt.num = 0;
	wt.version = 0;
	wt.mapAsked[0] = wt.session[0] = '\0';
	wt.serverId = 0;
	wt.want = qfalse;
	wt.aim = wt.zone = -1;
	wt.holdId = 0;
	for ( int s = 0; s < WT_BODIES; s++ )
		WT_FreeBody( s );
}

// the cgame's going, and the renderer's models with it
void CL_WorldItems_Shutdown( void ) {
	CL_WorldItems_Disconnect();
}

// the renderer started again: the bodies' models are gone, dropped (not freed)
void CL_WorldItems_RendererRestarted( void ) {
	memset( wtBodies, 0, sizeof( wtBodies ) );
	for ( int i = 0; i < wt.num; i++ )
		wt.things[i].boundsKnown = qfalse;
	we.d.boundsKnown = qfalse;
}

/*
===============================================================================

Used: looked at or stood in, the use key; held on for a search

===============================================================================
*/

static const char *WT_Name( const wtThing_t *t ) {
	if ( t->title[0] )
		return t->title;
	return t->kind == WT_BODY ? "A body" : t->datapad ? "Datapad" : t->kind == WT_ZONE ? "Here" : "Something";
}

static void WR_Open( const wtThing_t *t ) {
	if ( we.open )
		return;
	memset( &wr, 0, sizeof( wr ) );
	wr.open = qtrue;
	wr.id = t->id;
	wr.thing = *t;
	wr.loading = qtrue;
	WT_AskOpen( t->id, qfalse );
	if ( !( Key_GetCatcher() & KEYCATCH_WORLD ) )
		Key_SetCatcher( Key_GetCatcher() | KEYCATCH_WORLD );
	we.cursorX = SCREEN_WIDTH * 0.5f;
	we.cursorY = SCREEN_HEIGHT * 0.5f;
	we.click = qfalse;
}

// held: the player works at it (RPMod's emconsole) until it's opened or let go
static void WT_HoldEmote( qboolean on ) {
	if ( on == wt.holdEmote )
		return;
	wt.holdEmote = on;
	if ( cls.state != CA_ACTIVE )
		return;
	if ( on ) {
		CL_AddReliableCommand( "emconsole", qfalse );
		return;
	}
	// ended as the player would: a tap of crouch
	Cbuf_AddText( "+movedown " WT_CROUCH_KEY "\n" );
	wt.crouchUntil = cls.realtime + WT_CROUCH_MS;
}

static int WT_Target( void ) {
	return wt.aim >= 0 ? wt.aim : wt.zone;
}

// nothing to read: a zone that's only a trigger, or a thing with a story name, no description and
// no loot, is only used
static qboolean WT_OnlyUsed( const wtThing_t *t ) {
	if ( t->kind == WT_ZONE && !t->panel )
		return qtrue;
	return (qboolean)( t->story[0] && t->kind != WT_ITEM && !t->text && t->loot == WT_LOOT_NONE );
}

// used: the storyteller told, and read unless there's nothing to; reached for if not
static void WT_Use( const wtThing_t *t ) {
	if ( t->story[0] )
		CL_Storyteller_TriggerUsed( t->story );
	if ( !WT_OnlyUsed( t ) )
		WR_Open( t );
	else if ( t->usetime <= 0.0f )
		CL_AddReliableCommand( "emreach", qfalse );
}

// pressed on too early for a story: its message instead (qtrue), not used
static qboolean WT_TooEarly( const wtThing_t *t ) {
	const char *text = NULL;

	if ( !t->story[0] || CL_Storyteller_UseLock( t->story, &text ) != 2 )
		return qfalse;
	Q_strncpyz( wt.earlyText, text && text[0] ? text : "Not yet.", sizeof( wt.earlyText ) );
	wt.earlyTime = cls.realtime;
	return qtrue;
}

static void WT_UseFrame( void ) {
	const qboolean useDown = (qboolean)( ( cl.cmds[cl.cmdNumber & CMD_MASK].buttons & BUTTON_USE ) != 0 );
	int target;

	if ( wt.crouchUntil && cls.realtime >= wt.crouchUntil ) {
		wt.crouchUntil = 0;
		Cbuf_AddText( "-movedown " WT_CROUCH_KEY "\n" );
	}
	if ( cls.state != CA_ACTIVE || cl.snap.ps.pm_type == PM_SPECTATOR || cl.snap.ps.stats[STAT_HEALTH] <= 0 || Key_GetCatcher() ) {
		wt.aim = wt.zone = -1;
		wt.holdId = 0;
		WT_HoldEmote( qfalse );
		wt.useDown = useDown;
		return;
	}
	wt.aim = WT_Aimed();
	wt.zone = WT_ZoneIn();
	target = WT_Target();
	if ( useDown && target >= 0 ) {
		const wtThing_t *t = &wt.things[target];

		if ( !wt.useDown ) {
			wt.holdId = t->id;
			wt.holdStart = cls.realtime;
			wt.holdDone = qfalse;
			if ( WT_TooEarly( t ) ) {
				wt.holdDone = qtrue;
			} else if ( t->usetime <= 0.0f ) {
				WT_Use( t );
				wt.holdDone = qtrue;
			} else {
				WT_HoldEmote( qtrue );
			}
		} else if ( wt.holdId != t->id ) {
			wt.holdId = 0;		// looked away: let go
		}
		if ( wt.holdId == t->id && !wt.holdDone && t->usetime > 0.0f && cls.realtime - wt.holdStart >= t->usetime * 1000.0f ) {
			wt.holdDone = qtrue;
			WT_HoldEmote( qfalse );
			WT_Use( t );
		}
	} else {
		wt.holdId = 0;
	}
	if ( wt.holdEmote && ( !wt.holdId || wt.holdDone ) )
		WT_HoldEmote( qfalse );
	wt.useDown = useDown;
}

static void WT_LoadFonts( void ) {
	if ( wt.font )
		return;
	wt.font = re->RegisterFont( "arialnb" );
	if ( !wt.font )
		wt.font = cls.menuFont;
	wt.titleFont = re->RegisterFont( "ergoec" );
	if ( !wt.titleFont )
		wt.titleFont = wt.font;
	wt.clickSound = S_RegisterSound( "sound/interface/button1.wav" );
}

// the one looked at or stood in: which key opens it, and a held search's bar
static void WT_DrawPrompt( void ) {
	const int key = Key_GetKey( "+use" ), target = WT_Target();
	const wtThing_t *t;
	const char *verb;

	if ( wt.earlyTime && cls.realtime - wt.earlyTime < WT_EARLY_MS && !Key_GetCatcher() ) {
		float c[4] = { 1.0f, 0.85f, 0.3f, 1.0f };

		c[3] = Com_Clamp( 0.0f, 1.0f, ( WT_EARLY_MS - ( cls.realtime - wt.earlyTime ) ) / 800.0f );
		WG_TypedText( 0, SCREEN_HEIGHT * 0.5f + 70, SCREEN_WIDTH, wt.earlyText, c, wt.font, 0.8f, qtrue, qtrue );
	}
	if ( target < 0 || Key_GetCatcher() )
		return;
	t = &wt.things[target];
	// its own verb, if it has one, and its title as it's written; a zone with none, the title alone
	if ( t->verb[0] || t->kind == WT_ZONE ) {
		WG_TypedText( 0, SCREEN_HEIGHT * 0.5f + 34, SCREEN_WIDTH, va( S_COLOR_YELLOW "[%s]" S_COLOR_WHITE " %s%s%s",
			key >= 0 ? Key_KeynumToString( key ) : "use", t->verb, t->verb[0] ? " " : "", WT_Name( t ) ), wtWhite, wt.font, 0.8f, qtrue, qtrue );
	} else {
		verb = WT_OnlyUsed( t ) ? "Use" : t->kind == WT_ITEM ? "Look at" : t->kind == WT_BODY || t->loot != WT_LOOT_NONE ? "Search" : "Read";
		WG_TypedText( 0, SCREEN_HEIGHT * 0.5f + 34, SCREEN_WIDTH, va( S_COLOR_YELLOW "[%s]" S_COLOR_WHITE " %s \"%s\"",
			key >= 0 ? Key_KeynumToString( key ) : "use", verb, WT_Name( t ) ), wtWhite, wt.font, 0.8f, qtrue, qtrue );
	}
	if ( wt.holdId == t->id && !wt.holdDone && t->usetime > 0.0f ) {
		const float f = Com_Clamp( 0.0f, 1.0f, ( cls.realtime - wt.holdStart ) / ( t->usetime * 1000.0f ) );
		const float w = 120.0f, x = ( SCREEN_WIDTH - w ) * 0.5f, y = SCREEN_HEIGHT * 0.5f + 52;

		re->SetColor( wtField );
		re->DrawStretchPic( x, y, w, 5, 0, 0, 0, 0, cls.whiteShader );
		re->SetColor( wtOrange );
		re->DrawStretchPic( x, y, w * f, 5, 0, 0, 0, 0, cls.whiteShader );
		re->SetColor( NULL );
	}
}

/*
===============================================================================

The frame: asked of C1B3R, answers read

===============================================================================
*/

static void WR_Message( const char *text, qboolean bad ) {
	Q_strncpyz( wr.message, text, sizeof( wr.message ) );
	wr.messageTime = cls.realtime;
	wr.messageBad = bad;
}

static void WE_Message( const char *text, qboolean bad ) {
	Q_strncpyz( we.message, text, sizeof( we.message ) );
	we.messageTime = cls.realtime;
	we.messageBad = bad;
}

static void WT_Said( const char *text, qboolean bad ) {
	if ( we.open )
		WE_Message( text, bad );
	else if ( wr.open )
		WR_Message( text, bad );
	Com_Printf( "World: %s\n", text );
}

// saved: how long it stays is what was picked, shown as saved from now on
static void WE_Saved( void ) {
	if ( we.expiry >= 0 && we.expiry < WT_NUM_EXPIRIES ) {
		we.d.mapOnly = (qboolean)( wtExpiries[we.expiry].secs == WT_TILL_MAP );
		we.d.ends = (qboolean)( wtExpiries[we.expiry].secs > 0 );
	}
}

static void WT_ActDone( void ) {
	const char *answer = wtActHttp.answer[0] ? wtActHttp.answer : "C1B3R didn't answer";
	const int number = wtActHttp.answer[2] == '\t' ? atoi( wtActHttp.answer + 3 ) : 0;

	if ( wtActHttp.status != 200 ) {
		char why[160];

		Q_strncpyz( why, answer, sizeof( why ) );
		if ( strchr( why, '\n' ) )
			*strchr( why, '\n' ) = '\0';
		WT_Said( why, qtrue );
		if ( !strcmp( wtActWhat, "pickup" ) )
			CL_Notes_Message( why, qtrue );
	} else if ( !strcmp( wtActWhat, "drop" ) ) {
		WT_Said( "Dropped: it goes back to you in a day if nobody picks it up", qfalse );
	} else if ( !strcmp( wtActWhat, "pickup" ) ) {
		WT_Said( "Picked up: it's in your inventory", qfalse );
		CL_Notes_PickedUp( number );
		WR_Close();
	} else if ( !strcmp( wtActWhat, "take" ) ) {
		WT_Said( "Taken: it's in your inventory", qfalse );
		if ( wr.open )
			WT_AskOpen( wr.id, qfalse );
	} else if ( !strcmp( wtActWhat, "build" ) ) {
		WT_Said( "Built", qfalse );
		WE_Saved();
		if ( we.open && we.sel == -1 && number > 0 ) {
			for ( int s = 0; s < WT_BODIES; s++ ) {
				if ( wtBodies[s].id == -1 )
					wtBodies[s].id = number;
			}
			we.sel = number;
			we.d.id = number;
			we.changed = qfalse;
			we.expiry = -1;
		}
	} else if ( !strcmp( wtActWhat, "edit" ) ) {
		WT_Said( "Saved", qfalse );
		WE_Saved();
		we.changed = qfalse;
		we.expiry = -1;
	} else if ( !strcmp( wtActWhat, "lootnew" ) ) {
		WT_Said( va( "Made in it: %s", we.makeName ), qfalse );
		we.makeName[0] = we.makeDescr[0] = we.makeModel[0] = '\0';
		we.making = qfalse;
		if ( we.open && we.sel > 0 )
			WT_AskOpen( we.sel, qtrue );
	} else if ( !strcmp( wtActWhat, "lootadd" ) || !strcmp( wtActWhat, "lootremove" ) ) {
		WT_Said( !strcmp( wtActWhat, "lootadd" ) ? "Put in it" : "Taken out: back in your inventory", qfalse );
		if ( we.open && we.sel > 0 )
			WT_AskOpen( we.sel, qtrue );
		// still picking: what you carry asked again, the one put in gone from it
		if ( we.picking )
			WT_Post( &wtMineHttp, "/v2/inv/list", "" );
	} else if ( !strcmp( wtActWhat, "remove" ) ) {
		WT_Said( "Removed", qfalse );
		if ( we.open )
			WE_Select( 0 );
	}
	wt.want = qtrue;
	wt.retryAt = 0;
}

/*
===============================================================================

RPDUMPS: the map's things kept with one by C1B3R, and built again when it's loaded

===============================================================================
*/

static void WT_DumpDone( void ) {
	const int count = wtDumpHttp.answer[2] == '\t' ? atoi( wtDumpHttp.answer + 3 ) : 0;
	char why[160];

	if ( wtDumpHttp.status == 200 && !strncmp( wtDumpHttp.answer, "ok", 2 ) ) {
		if ( !strcmp( wtDumpWhat, "save" ) )
			Com_Printf( "World: %i world thing%s kept with the dump %s\n", count, count == 1 ? "" : "s", wtDumpName );
		else if ( !strcmp( wtDumpWhat, "load" ) )
			Com_Printf( "World: the map's world things swapped for the %i kept with %s\n", count, wtDumpName );
		return;
	}
	Q_strncpyz( why, wtDumpHttp.answer[0] ? wtDumpHttp.answer : "C1B3R didn't answer", sizeof( why ) );
	if ( strchr( why, '\n' ) )
		*strchr( why, '\n' ) = '\0';
	if ( wtDumpHttp.status == 404 )
		Com_Printf( S_COLOR_YELLOW "World: %s; the world things here stay\n", why );
	else
		Com_Printf( S_COLOR_YELLOW "World: rpdump %s %s, world things: %s\n", wtDumpWhat, wtDumpName, why );
}

// rpdump save <what> <name>, load <name> or delete <name>, on its way to the server: the world
// things go with it
void CL_WorldItems_DumpCommand( void ) {
	const char *verb = Cmd_Argv( 1 ), *name;
	std::string f;

	if ( Q_stricmp( Cmd_Argv( 0 ), "rpdump" ) || Cmd_Argc() < 3 || !CL_Presence_Session() || !CL_WorldItems_Admin() )
		return;
	if ( !Q_stricmp( verb, "save" ) )
		name = Cmd_Argv( Cmd_Argc() >= 4 ? 3 : 2 );
	else if ( !Q_stricmp( verb, "load" ) || !Q_stricmp( verb, "delete" ) )
		name = Cmd_Argv( 2 );
	else
		return;
	if ( wtDumpHttp.state == HTTP_BUSY ) {
		Com_Printf( S_COLOR_YELLOW "World: still busy with the last rpdump; the world things don't go with this one\n" );
		return;
	}
	Q_strncpyz( wtDumpWhat, verb, sizeof( wtDumpWhat ) );
	Q_strlwr( wtDumpWhat );
	Q_strncpyz( wtDumpName, name, sizeof( wtDumpName ) );
	f += ",\"name\":";
	CL_Presence_Json( f, name );
	WT_Post( &wtDumpHttp, va( "/v2/world/dump/%s", wtDumpWhat ), f );
}

// which load of which map this game's in, as the presence says it (C1B3R's server state)
static std::string WT_LoadFields( void ) {
	const char *info = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];
	std::string f;

	if ( !cl.serverId )
		return f;
	f = va( ",\"serverId\":\"%i\",\"map\":", cl.serverId );
	CL_Presence_Json( f, Info_ValueForKey( info, "mapname" ) );
	return f;
}

void CL_WorldItems_Frame( void ) {
	const char *session = CL_Presence_Session();

	// another session, map or load of it: asked again, and another map or load starts blank, so what
	// went with the last isn't shown till C1B3R says what's here now
	if ( Q_stricmp( wt.session, session ? session : "" ) || Q_stricmp( wt.mapAsked, WT_Map() ) || wt.serverId != cl.serverId ) {
		if ( Q_stricmp( wt.mapAsked, WT_Map() ) || wt.serverId != cl.serverId ) {
			wt.num = 0;
			WR_Close();
			WE_Close();
			WT_KeepBodies();
		}
		Q_strncpyz( wt.session, session ? session : "", sizeof( wt.session ) );
		Q_strncpyz( wt.mapAsked, WT_Map(), sizeof( wt.mapAsked ) );
		wt.serverId = cl.serverId;
		wt.want = qtrue;
		wt.retryAt = 0;
	}
	if ( HTTP_Poll( &wtListHttp ) ) {
		if ( wtListHttp.status == 200 && !strncmp( wtListHttp.answer, "ok", 2 ) ) {
			WT_ReadList( wtListHttp.answer );
		} else {
			// not linked, or C1B3R without them: none shown, asked again later
			wt.num = 0;
			WT_KeepBodies();
			wt.retryAt = cls.realtime + WT_RETRY_MS;
			wt.want = qtrue;
		}
	}
	if ( HTTP_Poll( &wtActHttp ) )
		WT_ActDone();
	if ( HTTP_Poll( &wtDumpHttp ) )
		WT_DumpDone();
	if ( HTTP_Poll( &wtOpenHttp ) ) {
		const qboolean ok = (qboolean)( wtOpenHttp.status == 200 && !strncmp( wtOpenHttp.answer, "ok", 2 ) );

		if ( wtOpenForEditor && we.open && we.sel == wtOpenFor ) {
			static char asked[WT_DESCR];
			// asked again after loot changed: what was typed already stays
			wtOpenCtx_t c = { we.descrKnown ? asked : we.descr, we.loot, &we.numLoot };

			we.numLoot = 0;
			if ( ok ) {
				c.descr[0] = '\0';
				WT_Lines( wtOpenHttp.answer, WT_OpenLine, &c );
				we.descrKnown = qtrue;
			} else {
				WE_Message( wtOpenHttp.answer[0] ? wtOpenHttp.answer : "C1B3R didn't answer", qtrue );
			}
		} else if ( !wtOpenForEditor && wr.open && wr.id == wtOpenFor ) {
			wtOpenCtx_t c = { wr.descr, wr.loot, &wr.numLoot };

			wr.loading = qfalse;
			wr.numLoot = 0;
			if ( ok ) {
				wr.descr[0] = '\0';
				WT_Lines( wtOpenHttp.answer, WT_OpenLine, &c );
				wr.known = qtrue;
			} else {
				WR_Message( wtOpenHttp.answer[0] ? wtOpenHttp.answer : "C1B3R didn't answer", qtrue );
			}
		}
	}
	if ( HTTP_Poll( &wtMineHttp ) ) {
		we.numMine = 0;
		if ( wtMineHttp.status == 200 )
			WT_Lines( wtMineHttp.answer, WT_MineLine, NULL );
		else
			WE_Message( "Your inventory couldn't be read", qtrue );
	}
	if ( wt.want && session && wt.mapAsked[0] && cls.realtime >= wt.retryAt && wtListHttp.state != HTTP_BUSY
		&& WT_Post( &wtListHttp, "/v2/world/list", WT_LoadFields() ) )
		wt.want = qfalse;
	WE_PickFrame();
	WT_UseFrame();
}

/*
===============================================================================

Drawing: the windows' look

===============================================================================
*/

static void WT_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void WT_FrameBox( float x, float y, float w, float h, const float *fill ) {
	WT_Fill( x, y, w, h, fill );
	WT_Fill( x, y, w, 1, wtBorder );
	WT_Fill( x, y + h - 1, w, 1, wtBorder );
	WT_Fill( x, y, 1, h, wtBorder );
	WT_Fill( x + w - 1, y, 1, h, wtBorder );
}

static void WT_Text( float x, float y, float w, const char *text, const float *color, int font, float scale, qboolean centred ) {
	WG_TypedText( x, y, w, text, color, font, scale, centred, qtrue );
}

static qboolean WT_InRect( float x, float y, float w, float h ) {
	if ( WG_PopupHover( we.cursorX, we.cursorY ) )
		return qfalse;
	return (qboolean)( we.cursorX >= x && we.cursorX < x + w && we.cursorY >= y && we.cursorY < y + h );
}

static qboolean WT_Clicked( float x, float y, float w, float h ) {
	if ( !we.click || we.clickX < x || we.clickX >= x + w || we.clickY < y || we.clickY >= y + h )
		return qfalse;
	we.click = qfalse;
	if ( wt.clickSound )
		S_StartLocalSound( wt.clickSound, CHAN_LOCAL_SOUND );
	return qtrue;
}

static qboolean WT_Button( float x, float y, float w, const char *label, qboolean on, qboolean enabled ) {
	WT_FrameBox( x, y, w, WT_CTRL_H, on ? wtRowOn : enabled && WT_InRect( x, y, w, WT_CTRL_H ) ? wtRowHover : wtRow );
	WT_Text( x, y + 2, w, label, enabled ? wtWhite : wtDim, wt.font, WT_TEXT, qtrue );
	return enabled ? WT_Clicked( x, y, w, WT_CTRL_H ) : qfalse;
}

static void WT_Style( wgStyle_t *style ) {
	memset( style, 0, sizeof( *style ) );
	style->font = wt.font;
	style->scale = WT_TEXT;
	style->lineH = WT_LINE_H;
	style->padX = 4;
	style->padY = 2;
	style->fill = wtField;
	style->border = wtBorder;
	style->text = wtWhite;
	style->dim = wtDim;
}

// the editor's mouse for the shared - value + control (the wheel only for an open list)
static void WE_PopMouse( wgMouse_t *m ) {
	const qboolean over = WG_PopupHover( we.cursorX, we.cursorY );

	m->cursorX = over ? -1.0f : we.cursorX;
	m->cursorY = over ? -1.0f : we.cursorY;
	m->click = &we.click;
	m->clickX = we.clickX;
	m->clickY = we.clickY;
	m->wheel = WG_PopupOpen() ? &we.wheel : NULL;
}

static void WT_Cursor( void ) {
	re->SetColor( NULL );
	re->DrawStretchPic( we.cursorX, we.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

// a thing on its own, turning, in a box: an object's model or a body
static void WT_DrawPreview( wtThing_t *t, float x, float y, float w, float h ) {
	const float xScale = cls.glconfig.vidWidth / (float)SCREEN_WIDTH, yScale = cls.glconfig.vidHeight / (float)SCREEN_HEIGHT;
	wtThing_t copy = *t;
	refdef_t refdef;
	refEntity_t ent;
	vec3_t center, angles, fwd;
	float radius, fovX, fovY = 30.0f, dist;

	WT_FrameBox( x, y, w, h, wtField );
	VectorClear( copy.pos );
	VectorSet( copy.ang, 0, (float)( cls.realtime % 12000 ) * 0.03f, 0 );
	if ( !WT_Entity( &copy, &ent ) ) {
		WT_Text( x, y + h * 0.5f - 6, w, t->kind == WT_ZONE ? "A place, no model" : "No model to show", wtDim, wt.font, WT_SMALL, qtrue );
		return;
	}
	t->boundsKnown = copy.boundsKnown;
	VectorCopy( copy.mins, t->mins );
	VectorCopy( copy.maxs, t->maxs );
	if ( t->kind == WT_BODY ) {
		VectorSet( center, 0, 0, 8.0f * t->scale );
		radius = 40.0f * t->scale;
	} else if ( copy.boundsKnown ) {
		vec3_t size, mid;
		matrix3_t axis;

		// its own middle, turned and sized with it
		VectorAdd( copy.mins, copy.maxs, mid );
		VectorScale( mid, 0.5f, mid );
		VectorSubtract( copy.maxs, copy.mins, size );
		radius = Q_max( 2.0f, VectorLength( size ) * 0.5f * t->scale );
		AnglesToAxis( copy.ang, axis );
		for ( int k = 0; k < 3; k++ )
			center[k] = t->scale * ( axis[0][k] * mid[0] + axis[1][k] * mid[1] + axis[2][k] * mid[2] );
	} else {
		VectorClear( center );
		radius = 16.0f * t->scale;
	}
	VectorSubtract( ent.origin, center, ent.origin );
	VectorCopy( ent.origin, ent.oldorigin );
	ent.renderfx = RF_NOSHADOW;

	memset( &refdef, 0, sizeof( refdef ) );
	refdef.x = (int)( x * xScale ) + 1;
	refdef.y = (int)( y * yScale ) + 1;
	refdef.width = (int)( w * xScale ) - 2;
	refdef.height = (int)( h * yScale ) - 2;
	fovX = RAD2DEG( 2.0f * atanf( tanf( DEG2RAD( fovY * 0.5f ) ) * refdef.width / (float)refdef.height ) );
	refdef.fov_x = fovX;
	refdef.fov_y = fovY;
	VectorSet( angles, 25.0f, 180.0f, 0 );
	AnglesToAxis( angles, refdef.viewaxis );
	dist = radius / sinf( DEG2RAD( Q_min( fovX, fovY ) * 0.5f ) ) * 1.05f;
	AngleVectors( angles, fwd, NULL, NULL );
	VectorScale( fwd, -dist, refdef.vieworg );
	refdef.rdflags = RDF_NOWORLDMODEL;
	refdef.time = cl.serverTime;
	re->ClearScene();
	re->AddRefEntityToScene( &ent );
	re->RenderScene( &refdef );
}

/*
===============================================================================

The reader: a thing opened

===============================================================================
*/

#define WR_LOOT_H		40.0f		// a loot row: its turning model, its name, Take
#define WR_LOOT_ROWS	4
#define WR_X		60.0f
#define WR_Y		44.0f
#define WR_W		520.0f
#define WR_H		392.0f
#define WR_VIEW		170.0f

static void WR_Close( void ) {
	if ( !wr.open )
		return;
	wr.open = qfalse;
	if ( !we.open )
		Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_WORLD );
}

// /world read: what you look at, or the zone you stand in
static void WR_ReadAimed( void ) {
	int target;

	if ( cls.state != CA_ACTIVE ) {
		Com_Printf( "World: join a server first\n" );
		return;
	}
	wt.aim = WT_Aimed();
	wt.zone = WT_ZoneIn();
	target = WT_Target();
	if ( target < 0 ) {
		Com_Printf( "World: look at something near you, or stand in a zone (world near lists them)\n" );
		return;
	}
	if ( WT_OnlyUsed( &wt.things[target] ) ) {
		Com_Printf( "World: nothing to read there, it's only used\n" );
		return;
	}
	WR_Open( &wt.things[target] );
}

static void WR_Draw( void ) {
	const float lx = WR_X + 12, rx = lx + WR_VIEW + 14, rw = WR_X + WR_W - 12 - rx;
	const float bottom = WR_Y + WR_H - 30;
	wtThing_t *live = WT_Find( wr.id );
	float y;

	if ( live )
		wr.thing = *live;
	WT_FrameBox( WR_X, WR_Y, WR_W, WR_H, wtPanel );
	WT_Text( lx, WR_Y + 8, WR_W - 100, WT_Name( &wr.thing ), wtOrange, wt.titleFont, WT_ITEM_SCALE + 0.2f, qfalse );
	if ( WT_Button( WR_X + WR_W - 12 - 60, WR_Y + 10, 60, "Close", qfalse, qtrue ) ) {
		WR_Close();
		return;
	}
	if ( !live && !wr.loading ) {
		WT_Text( WR_X, WR_Y + 180, WR_W, "It isn't there any more", wtDim, wt.font, WT_TEXT, qtrue );
		return;
	}

	// left: what it looks like, and what's done with it
	y = WR_Y + 40;
	WT_DrawPreview( &wr.thing, lx, y, WR_VIEW, WR_VIEW );
	y += WR_VIEW + 8;
	WT_Text( lx, y, WR_VIEW, va( "%s%s", wtKindTitles[wr.thing.kind], wr.thing.mine ? " (yours)" : "" ), wtDim, wt.font, WT_SMALL, qfalse );
	y += 16;
	if ( wr.thing.kind == WT_ITEM ) {
		if ( WT_Button( lx, y, WR_VIEW, "Pick up", qfalse, (qboolean)( wtActHttp.state != HTTP_BUSY ) ) )
			WT_Act( "pickup", "/v2/world/pickup", va( ",\"id\":%i", wr.id ) );
		y += WT_CTRL_H + 4;
		if ( wr.thing.datapad && WT_Button( lx, y, WR_VIEW, "Read its notes", qfalse, qtrue ) ) {
			const wtThing_t t = wr.thing;

			WR_Close();
			CL_Datapad_OpenItem( t.item, WT_Name( &t ), t.id );
			return;
		}
		if ( wr.thing.datapad )
			y += WT_CTRL_H + 4;
	}
	if ( CL_WorldItems_Admin() && WT_Button( lx, y, WR_VIEW, "Edit (admins)", qfalse, qtrue ) ) {
		const int id = wr.id;

		WR_Close();
		Cbuf_AddText( va( WT_CMD " edit %i\n", id ) );
		return;
	}

	// right: the description, then the loot
	y = WR_Y + 40;
	if ( wr.loading ) {
		WT_Text( rx, y, rw, "Asking C1B3R...", wtDim, wt.font, WT_TEXT, qfalse );
	} else {
		wgStyle_t style;
		const int lootRows = Q_min( WR_LOOT_ROWS, wr.numLoot );
		const float lootH = wr.numLoot ? lootRows * WR_LOOT_H + 22.0f : 0.0f;
		const float textBottom = bottom - lootH;
		const char *text = wr.descr[0] ? wr.descr : "Nothing more is written about it.";
		const int visible = (int)( ( textBottom - y ) / WT_LINE_H );
		int lines, first;

		WT_Style( &style );
		lines = WG_TextLines( &style, text, rw - WG_SCROLL_W - 4 );

		{
			int scroll = (int)wr.scroll;

			WG_Scrollbar( rx + rw - WG_SCROLL_W, y, visible * WT_LINE_H, lines, visible, &scroll, we.cursorX, we.cursorY, &we.click, we.clickX, we.clickY );
			wr.scroll = (float)Com_Clampi( 0, Q_max( 0, lines - visible ), scroll );
		}
		first = (int)wr.scroll;
		WG_DrawText( &style, rx, y, rw - WG_SCROLL_W - 4, text, first, Q_min( visible, lines - first ), wr.descr[0] ? wtWhite : wtDim );
		if ( wr.numLoot ) {
			float ly = textBottom + 6;

			WT_Text( rx, ly, rw, va( "%s", wr.thing.loot == WT_LOOT_EACH ? "To take (one each)" : "To take" ), wtOrange, wt.font, WT_TEXT, qfalse );
			ly += 16;
			{
				const qboolean bar = (qboolean)( wr.numLoot > lootRows );
				const float iw = bar ? rw - WG_SCROLL_W - 2 : rw;

				if ( bar ) {
					WG_Scrollbar( rx + rw - WG_SCROLL_W, ly, lootRows * WR_LOOT_H - 2, wr.numLoot, lootRows, &wr.lootScroll,
						we.cursorX, we.cursorY, &we.click, we.clickX, we.clickY );
				}
				wr.lootScroll = Com_Clampi( 0, Q_max( 0, wr.numLoot - lootRows ), wr.lootScroll );
				for ( int r = 0; r < lootRows; r++ ) {
					const int i = wr.lootScroll + r;
					const wtLoot_t *l = &wr.loot[i];
					const float tx = l->model[0] ? rx + WR_LOOT_H + 4 : rx + 6;

					WT_FrameBox( rx, ly, iw, WR_LOOT_H - 2, wtRow );
					// its own model, turning, when it has one
					if ( l->model[0] ) {
						wtThing_t view;

						memset( &view, 0, sizeof( view ) );
						view.kind = WT_OBJECT;
						view.scale = 1.0f;
						Q_strncpyz( view.model, l->model, sizeof( view.model ) );
						WT_DrawPreview( &view, rx + 1, ly + 1, WR_LOOT_H - 4, WR_LOOT_H - 4 );
					}
					WT_Text( tx, ly + 4, iw - ( tx - rx ) - 84, l->name, l->taken ? wtDim : wtWhite, wt.font, WT_TEXT, qfalse );
					WT_Text( tx, ly + 18, iw - ( tx - rx ) - 84, l->descr[0] ? l->descr : l->kind, wtDim, wt.font, WT_SMALL, qfalse );
					if ( l->taken )
						WT_Text( rx + iw - 80, ly + ( WR_LOOT_H - 2 ) * 0.5f - 5, 74, "Taken", wtDim, wt.font, WT_SMALL, qtrue );
					else if ( WT_Button( rx + iw - 80, ly + ( WR_LOOT_H - 2 - WT_CTRL_H ) * 0.5f, 74, "Take", qfalse, (qboolean)( wtActHttp.state != HTTP_BUSY ) ) )
						WT_Act( "take", "/v2/world/take", va( ",\"id\":%i,\"item\":%i", wr.id, l->item ) );
					ly += WR_LOOT_H;
				}
			}
		}
	}
	if ( wr.message[0] && cls.realtime - wr.messageTime < WT_MESSAGE_MS )
		WT_Text( lx, WR_Y + WR_H - 22, WR_W - 24, wr.message, wr.messageBad ? wtYellow : wtGreen, wt.font, WT_TEXT, qfalse );
	else
		WT_Text( lx, WR_Y + WR_H - 22, WR_W - 24, "Esc closes it", wtDim, wt.font, WT_SMALL, qfalse );
}

/*
===============================================================================

The editor: the view, a free camera, the gizmo

===============================================================================
*/

#define WE_W		244.0f
#define WE_X		( SCREEN_WIDTH - WE_W - 8 )
#define WE_Y		16.0f
#define WE_H		( SCREEN_HEIGHT - 32.0f )

static qboolean WE_InPanel( float x, float y ) {
	return (qboolean)( x >= WE_X && x < WE_X + WE_W && y >= WE_Y && y < WE_Y + WE_H );
}

static qboolean WE_ShiftDown( void ) {
	return (qboolean)( we.held[A_SHIFT] || we.held[A_SHIFT2] );
}

static qboolean WE_CtrlDown( void ) {
	return (qboolean)( we.held[A_CTRL] || we.held[A_CTRL2] );
}

static void WE_DropDrag( void ) {
	we.dragging = WE_DRAG_NONE;
	we.gzPart = we.gzHot = -1;
}

static float WE_Step( qboolean keys ) {
	float step = ( we.mode == WE_TURN ? weTurnSteps : weMoveSteps )[we.step];

	if ( keys && WE_ShiftDown() )
		step *= 4.0f;
	if ( keys && WE_CtrlDown() )
		step *= 0.25f;
	return step;
}

static void WE_KeepView( const refdef_t *fd ) {
	VectorCopy( fd->vieworg, we.view.org );
	for ( int k = 0; k < 3; k++ )
		VectorCopy( fd->viewaxis[k], we.view.axis[k] );
	we.view.fovX = fd->fov_x;
	we.view.fovY = fd->fov_y;
	we.haveView = qtrue;
}

// every cgame scene passes through here first while the editor's open: the world's view kept for
// the gizmo, or the free camera's put in its place; qtrue if it was rendered
qboolean CL_WorldItems_RenderScene( const refdef_t *fd ) {
	refdef_t view;

	if ( !we.open || ( fd->rdflags & ( RDF_NOWORLDMODEL | RDF_AUTOMAP ) ) )
		return qfalse;
	if ( fd->rdflags & RDF_SKYBOXPORTAL ) {
		if ( !we.freeCam )
			return qfalse;
		view = *fd;
		AnglesToAxis( we.camAng, view.viewaxis );
		VectorCopy( we.camAng, view.viewangles );
		re->RenderScene( &view );
		return qtrue;
	}
	if ( we.viewFrame == cls.framecount )
		return qfalse;
	we.viewFrame = cls.framecount;
	if ( !we.freeCam ) {
		WE_KeepView( fd );
		return qfalse;
	}
	view = *fd;
	VectorCopy( we.camOrg, view.vieworg );
	VectorCopy( we.camAng, view.viewangles );
	AnglesToAxis( we.camAng, view.viewaxis );
	view.viewContents = CM_PointContents( we.camOrg, 0 );
	memset( view.areamask, 0, sizeof( view.areamask ) );
	WE_KeepView( &view );
	re->RenderScene( &view );
	return qtrue;
}

static void WE_FreeCam( qboolean on ) {
	WE_DropDrag();
	if ( on && !we.haveView ) {
		WE_Message( "Wait a moment: the world isn't drawn yet", qtrue );
		return;
	}
	we.freeCam = on;
	if ( on ) {
		VectorCopy( we.view.org, we.camOrg );
		vectoangles( we.view.axis[0], we.camAng );
		we.camAng[ROLL] = 0.0f;
		we.camAng[PITCH] = AngleNormalize180( we.camAng[PITCH] );
	}
}

static void WE_UpdateCamera( float dt ) {
	vec3_t forward, right, move;

	AngleVectors( we.camAng, forward, right, NULL );
	VectorClear( move );
	if ( we.held[A_CAP_W] )		VectorAdd( move, forward, move );
	if ( we.held[A_CAP_S] )		VectorSubtract( move, forward, move );
	if ( we.held[A_CAP_D] )		VectorAdd( move, right, move );
	if ( we.held[A_CAP_A] )		VectorSubtract( move, right, move );
	if ( we.held[A_SPACE] )		move[2] += 1.0f;
	if ( we.held[A_CAP_C] )		move[2] -= 1.0f;
	if ( VectorNormalize( move ) > 0.0f ) {
		float speed = WT_FLY_SPEED;

		if ( WE_ShiftDown() )
			speed *= 3.0f;
		if ( WE_CtrlDown() )
			speed *= 0.25f;
		VectorMA( we.camOrg, speed * dt, move, we.camOrg );
	}
}

// where the gizmo is: an object's or a zone's middle, a body's feet
static void WE_Pivot( vec3_t pivot ) {
	VectorCopy( we.d.pos, pivot );
}

static qboolean WE_Editing( void ) {
	return (qboolean)( we.open && we.sel != 0 );
}

static void WE_Changed( void ) {
	we.changed = qtrue;
	we.confirmRemove = qfalse;
}

static void WE_TurnBy( const vec3_t axis, float degrees ) {
	matrix3_t r;

	for ( int a = 0; a < 3; a++ )
		RotatePointAroundVector( r[a], axis, we.startAxes[a], degrees );
	GZ_AxisAngles( r, we.d.ang );
	for ( int k = 0; k < 3; k++ )
		we.d.ang[k] = AngleNormalize180( roundf( we.d.ang[k] * 10.0f ) / 10.0f );
	WE_Changed();
}

// the gizmo on the thing edited: dragged, hovered, grabbed by a click outside the panel, drawn
static void WE_GizmoFrame( void ) {
	vec3_t pivot;
	int lit;

	if ( we.gzHidden || !WE_Editing() || !we.haveView || ( we.d.kind == WT_ITEM && we.mode == WE_SIZE ) ) {
		WE_DropDrag();
		return;
	}
	if ( we.mode == WE_SIZE && we.d.kind != WT_ZONE )
		we.mode = WE_MOVE;
	WE_Pivot( pivot );
	if ( we.dragging == WE_DRAG_GIZMO ) {
		const float step = WE_Step( qfalse );

		if ( we.mode == WE_MOVE ) {
			const float amount = roundf( GZ_Amount( &we.gzDrag, we.cursorX, we.cursorY, 0 ) / step ) * step;

			VectorMA( we.startPos, amount, we.gzDir, we.d.pos );
			WE_Changed();
		} else if ( we.mode == WE_SIZE ) {
			const float amount = roundf( GZ_Amount( &we.gzDrag, we.cursorX, we.cursorY, 0 ) / step ) * step;

			we.d.size[we.gzPart] = Com_Clamp( 4.0f, 1024.0f, we.startSize[we.gzPart] + amount );
			WE_Changed();
		} else {
			const float now = GZ_ScreenAngle( &we.view, pivot, we.cursorX, we.cursorY );
			float delta = now - we.gzLastAngle;

			while ( delta > M_PI )
				delta -= 2.0f * M_PI;
			while ( delta < -M_PI )
				delta += 2.0f * M_PI;
			we.gzTurned += delta;
			we.gzLastAngle = now;
			WE_TurnBy( we.gzDir, roundf( we.gzSign * RAD2DEG( we.gzTurned ) / step ) * step );
		}
		WE_Pivot( pivot );
	}
	we.gzHot = -1;
	if ( we.dragging == WE_DRAG_NONE && !WE_InPanel( we.cursorX, we.cursorY ) )
		we.gzHot = we.mode == WE_TURN ? GZ_HitRings( &we.view, pivot, weWorldAxes, we.cursorX, we.cursorY )
			: GZ_HitAxes( &we.view, pivot, weWorldAxes, we.cursorX, we.cursorY );
	if ( we.click && we.dragging == WE_DRAG_NONE && !WE_InPanel( we.clickX, we.clickY ) ) {
		const int part = we.mode == WE_TURN ? GZ_HitRings( &we.view, pivot, weWorldAxes, we.clickX, we.clickY )
			: GZ_HitAxes( &we.view, pivot, weWorldAxes, we.clickX, we.clickY );

		if ( part >= 0 ) {
			we.click = qfalse;
			we.dragging = WE_DRAG_GIZMO;
			we.gzPart = part;
			VectorCopy( weWorldAxes[part], we.gzDir );
			VectorCopy( we.d.pos, we.startPos );
			VectorCopy( we.d.ang, we.startAng );
			VectorCopy( we.d.size, we.startSize );
			if ( we.mode == WE_TURN ) {
				AnglesToAxis( we.d.ang, we.startAxes );
				we.gzTurned = 0.0f;
				we.gzLastAngle = GZ_ScreenAngle( &we.view, pivot, we.clickX, we.clickY );
				we.gzSign = GZ_RingSign( &we.view, pivot, we.gzDir );
			} else {
				GZ_BeginDir( &we.gzDrag, &we.view, pivot, we.gzDir, part, we.clickX, we.clickY );
			}
		}
	}
	lit = we.dragging == WE_DRAG_GIZMO ? we.gzPart : we.gzHot;
	if ( we.mode == WE_TURN )
		GZ_DrawRings( &we.view, pivot, weWorldAxes, lit );
	else
		GZ_DrawAxes( &we.view, pivot, weWorldAxes, lit, NULL );
}

// a line in the world, drawn over the screen (a zone's edges)
static void WE_Line( const vec3_t a, const vec3_t b, const float *color ) {
	float x0, y0, x1, y1, len;
	int steps;

	if ( !GZ_Project( &we.view, a, &x0, &y0 ) || !GZ_Project( &we.view, b, &x1, &y1 ) )
		return;
	len = sqrtf( ( x1 - x0 ) * ( x1 - x0 ) + ( y1 - y0 ) * ( y1 - y0 ) );
	steps = Com_Clampi( 1, 400, (int)( len / 1.5f ) );
	re->SetColor( color );
	for ( int i = 0; i <= steps; i++ ) {
		const float f = (float)i / steps;

		re->DrawStretchPic( x0 + ( x1 - x0 ) * f - 0.75f, y0 + ( y1 - y0 ) * f - 0.75f, 1.5f, 1.5f, 0, 0, 0, 0, cls.whiteShader );
	}
	re->SetColor( NULL );
}

static void WE_DrawZone( const wtThing_t *t, const float *color ) {
	vec3_t mins, maxs, c[8];

	WT_Box( t, mins, maxs );
	for ( int i = 0; i < 8; i++ )
		VectorSet( c[i], ( i & 1 ) ? maxs[0] : mins[0], ( i & 2 ) ? maxs[1] : mins[1], ( i & 4 ) ? maxs[2] : mins[2] );
	for ( int i = 0; i < 8; i++ ) {
		for ( int k = 0; k < 3; k++ ) {
			const int j = i | ( 1 << k );

			if ( j != i )
				WE_Line( c[i], c[j], color );
		}
	}
}

// the zones on the map (they can't be seen otherwise), the one edited lit
static void WE_DrawZones( void ) {
	if ( !we.haveView )
		return;
	for ( int i = 0; i < wt.num; i++ ) {
		if ( wt.things[i].kind == WT_ZONE && wt.things[i].id != we.sel )
			WE_DrawZone( &wt.things[i], wt.things[i].walk ? wtZoneWalk : wtZone );
	}
	if ( WE_Editing() && we.d.kind == WT_ZONE )
		WE_DrawZone( &we.d, wtZoneSel );
}

// a step with the keys: moved along the world's axis nearest the camera's way, turned, or sized
static void WE_KeyStep( int key ) {
	const float step = WE_Step( qtrue );
	const float sign = key == A_CURSOR_LEFT || key == A_CURSOR_UP || key == A_PAGE_UP ? 1.0f : -1.0f;

	if ( !WE_Editing() )
		return;
	if ( we.mode == WE_TURN ) {
		const int a = key == A_CURSOR_LEFT || key == A_CURSOR_RIGHT ? YAW : key == A_CURSOR_UP || key == A_CURSOR_DOWN ? PITCH : ROLL;

		we.d.ang[a] = AngleNormalize180( roundf( ( we.d.ang[a] + sign * step ) * 10.0f ) / 10.0f );
		WE_Changed();
		return;
	}
	if ( we.mode == WE_SIZE && we.d.kind == WT_ZONE ) {
		const int k = key == A_CURSOR_UP || key == A_CURSOR_DOWN ? 0 : key == A_CURSOR_LEFT || key == A_CURSOR_RIGHT ? 1 : 2;

		we.d.size[k] = Com_Clamp( 4.0f, 1024.0f, we.d.size[k] + sign * step );
		WE_Changed();
		return;
	}
	{
		vec3_t angles, want;
		float yaw;

		vectoangles( we.haveView ? we.view.axis[0] : vec3_origin, angles );
		yaw = DEG2RAD( floorf( angles[YAW] / 90.0f + 0.5f ) * 90.0f );
		switch ( key ) {
		case A_CURSOR_UP:		VectorSet( want, cosf( yaw ), sinf( yaw ), 0 ); break;
		case A_CURSOR_DOWN:		VectorSet( want, -cosf( yaw ), -sinf( yaw ), 0 ); break;
		case A_CURSOR_RIGHT:	VectorSet( want, sinf( yaw ), -cosf( yaw ), 0 ); break;
		case A_CURSOR_LEFT:		VectorSet( want, -sinf( yaw ), cosf( yaw ), 0 ); break;
		case A_PAGE_UP:			VectorSet( want, 0, 0, 1 ); break;
		default:				VectorSet( want, 0, 0, -1 ); break;
		}
		VectorMA( we.d.pos, step, want, we.d.pos );
		for ( int k = 0; k < 3; k++ )
			we.d.pos[k] = roundf( we.d.pos[k] * 10.0f ) / 10.0f;
		WE_Changed();
	}
}

/*
===============================================================================

The editor: open, picked, built, saved

===============================================================================
*/

static void WE_Close( void ) {
	if ( !we.open )
		return;
	WG_PopupClose();
	we.open = qfalse;
	we.focus = WF_NONE;
	we.freeCam = qfalse;
	WE_DropDrag();
	WG_EditReset();
	for ( int s = 0; s < WT_BODIES; s++ ) {
		if ( wtBodies[s].id == -1 )
			WT_FreeBody( s );
	}
	we.sel = 0;
	if ( !wr.open )
		Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_WORLD );
}

static void WE_Open( void ) {
	if ( cls.state != CA_ACTIVE ) {
		Com_Printf( "World: join a server first\n" );
		return;
	}
	if ( !CL_WorldItems_Admin() ) {
		Com_Printf( "World: building is for RPMod admins; anyone drops an item from /inventory\n" );
		return;
	}
	WR_Close();
	if ( we.open )
		return;
	we.open = qtrue;
	we.sel = 0;
	we.focus = WF_NONE;
	we.dragging = WE_DRAG_NONE;
	we.gzHot = we.gzPart = -1;
	we.freeCam = qfalse;
	we.haveView = qfalse;
	we.picking = we.making = qfalse;
	we.modelPick = qfalse;
	we.cursorX = SCREEN_WIDTH * 0.5f;
	we.cursorY = SCREEN_HEIGHT * 0.5f;
	we.click = qfalse;
	memset( we.held, 0, sizeof( we.held ) );
	we.lastFrameTime = cls.realtime;
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_WORLD );
}

// a thing picked to edit (0: back to the list)
static void WE_Select( int id ) {
	const wtThing_t *t = id > 0 ? WT_Find( id ) : NULL;

	WE_DropDrag();
	WG_EditReset();
	we.focus = WF_NONE;
	we.picking = we.making = qfalse;
	we.confirmRemove = qfalse;
	we.changed = qfalse;
	we.expiry = -1;
	we.numLoot = 0;
	we.editLootScroll = 0;
	we.descr[0] = '\0';
	we.descrKnown = qfalse;
	if ( !t ) {
		we.sel = 0;
		return;
	}
	we.sel = id;
	we.d = *t;
	WT_AskOpen( id, qtrue );
}

// where you aim within reach, a hair off what it hits; where you stand (at your feet) if nothing's there
static void WT_AimPoint( vec3_t out, float *yaw ) {
	vec3_t eye, dir, end, ang;
	trace_t tr;

	WT_Eye( eye, dir );
	VectorMA( eye, WT_PLACE_REACH, dir, end );
	CM_BoxTrace( &tr, eye, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
	if ( tr.fraction < 1.0f && !tr.startsolid ) {
		VectorMA( tr.endpos, 1.0f, tr.plane.normal, out );
	} else {
		vec3_t down;

		VectorCopy( cl.snap.ps.origin, end );
		VectorCopy( cl.snap.ps.origin, down );
		down[2] -= 128.0f;
		CM_BoxTrace( &tr, end, down, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
		VectorCopy( tr.endpos, out );
		out[2] += 1.0f;
	}
	vectoangles( dir, ang );
	*yaw = AngleNormalize180( ang[YAW] + 180.0f );
}

// a new one where you aim, not saved till Save
static void WE_New( int kind, qboolean container, qboolean walk ) {
	float yaw;

	WE_Select( 0 );
	memset( &we.d, 0, sizeof( we.d ) );
	we.d.id = -1;
	we.d.kind = kind;
	we.d.scale = 1.0f;
	we.d.loot = container ? WT_LOOT_SHARED : WT_LOOT_NONE;
	we.d.panel = qtrue;
	WT_AimPoint( we.d.pos, &yaw );
	we.d.ang[YAW] = yaw;
	if ( kind == WT_ZONE ) {
		VectorSet( we.d.size, 48, 48, 40 );
		we.d.pos[2] += we.d.size[2];
		we.d.ang[YAW] = 0;
		we.d.walk = walk;
		Q_strncpyz( we.d.title, walk ? "Walked into" : "A place", sizeof( we.d.title ) );
	} else if ( kind == WT_BODY ) {
		Q_strncpyz( we.d.model, "kyle", sizeof( we.d.model ) );
		Q_strncpyz( we.d.anim, wtPoses[0], sizeof( we.d.anim ) );
		Q_strncpyz( we.d.title, container ? "A body to search" : "A body", sizeof( we.d.title ) );
		we.d.usetime = 2.0f;
	} else {
		Q_strncpyz( we.d.title, container ? "A container" : "Something", sizeof( we.d.title ) );
	}
	we.sel = -1;
	we.descr[0] = '\0';
	we.descrKnown = qtrue;
	we.changed = qtrue;
	we.expiry = 1;		// till the map loads again
	we.mode = WE_MOVE;
}

static void WE_AddVec( std::string &out, const char *name, const vec3_t v ) {
	out += va( ",\"%s\":[%.1f,%.1f,%.1f]", name, v[0], v[1], v[2] );
}

static void WE_AddString( std::string &out, const char *name, const char *value ) {
	out += va( ",\"%s\":", name );
	CL_Presence_Json( out, value );
}

static void WE_Save( void ) {
	std::string f;

	if ( !WE_Editing() )
		return;
	if ( we.sel > 0 )
		f += va( ",\"id\":%i", we.sel );
	WE_AddVec( f, "pos", we.d.pos );
	WE_AddVec( f, "angles", we.d.ang );
	f += va( ",\"scale\":%.2f", we.d.scale );
	if ( we.d.kind != WT_ITEM ) {
		f += va( ",\"kind\":\"%s\"", wtKinds[we.d.kind] );
		WE_AddString( f, "title", we.d.title );
		WE_AddString( f, "descr", we.descr );
		WE_AddString( f, "model", we.d.model );
		WE_AddString( f, "skin", we.d.skin );
		WE_AddString( f, "anim", we.d.anim );
		WE_AddVec( f, "size", we.d.size );
		f += va( ",\"usetime\":%.1f,\"loot\":\"%s\"", we.d.usetime, wtLoots[we.d.loot] );
		WE_AddString( f, "story", we.d.story );
		WE_AddString( f, "verb", we.d.verb );
		if ( we.d.kind == WT_ZONE )
			f += va( ",\"walk\":%i,\"panel\":%i", we.d.walk ? 1 : 0, we.d.panel ? 1 : 0 );
	}
	if ( we.expiry >= 0 ) {
		const int secs = wtExpiries[we.expiry].secs;

		f += va( ",\"expires\":%i,\"maponly\":%i", secs == WT_TILL_MAP ? 0 : secs, secs == WT_TILL_MAP ? 1 : 0 );
	}
	if ( we.sel == -1 ) {
		f += ",\"item\":0";
		WT_Act( "build", "/v2/world/build", f );
	} else {
		WT_Act( "edit", "/v2/world/edit", f );
	}
}

/*
===============================================================================

The editor: its panel

===============================================================================
*/

// the model manager's choice, for the thing still edited
static void WE_ModelPicked( const char *path, void *ctx ) {
	if ( !we.open || !we.modelPick || we.sel != we.modelPickFor )
		return;
	Q_strncpyz( we.d.model, path, sizeof( we.d.model ) );
	WE_Changed();
	WE_Message( va( "Model: %s", path ), qfalse );
}

// the model manager's browser over the editor, which gives it the keys till it closes
static void WE_PickModel( void ) {
	we.focus = WF_NONE;
	WG_EditReset();
	WE_DropDrag();
	we.modelPick = qtrue;
	we.modelPickFor = we.sel;
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_WORLD );
	if ( !CL_ModelManager_Pick( WE_ModelPicked, NULL ) ) {
		we.modelPick = qfalse;
		Key_SetCatcher( Key_GetCatcher() | KEYCATCH_WORLD );
		WE_Message( "The model manager couldn't open", qtrue );
	}
}

// the browser closed, a model chosen or not: the editor takes the keys back
static void WE_PickFrame( void ) {
	if ( !we.modelPick || CL_ModelManager_Active() )
		return;
	we.modelPick = qfalse;
	if ( !we.open )
		return;
	memset( we.held, 0, sizeof( we.held ) );
	we.click = we.mouseDown = qfalse;
	we.lastFrameTime = cls.realtime;
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_WORLD );
}

// a field typed in, known by which
static void WE_Field( float x, float y, float w, int which, char *buf, const char *placeholder ) {
	wgStyle_t style;
	const qboolean focused = (qboolean)( we.focus == which );

	WT_Style( &style );
	WT_FrameBox( x, y, w, WT_CTRL_H, wtField );
	if ( focused )
		WT_Fill( x, y + WT_CTRL_H - 1, w, 1, wtOrange );
	WG_Field( &style, x, y, w, WT_CTRL_H, buf, focused, placeholder, we.cursorX, we.cursorY );
	if ( WT_Clicked( x, y, w, WG_FieldHeight( &style, w, WT_CTRL_H, buf, focused ) ) ) {
		if ( !focused )
			WG_EditReset();
		we.focus = which;
		WG_FieldClick( buf, we.clickX, we.clickY );
	}
}

// what a story name takes, as C1B3R does
static qboolean WT_StoryChar( int ch ) {
	return (qboolean)( isalnum( ch ) || ch == '_' || ch == '-' || ch == '.' );
}

static char *WE_FocusBuf( int *size ) {
	switch ( we.focus ) {
	case WF_TITLE:	*size = sizeof( we.d.title ); return we.d.title;
	case WF_MODEL:	*size = sizeof( we.d.model ); return we.d.model;
	case WF_SKIN:	*size = sizeof( we.d.skin ); return we.d.skin;
	case WF_ANIM:	*size = sizeof( we.d.anim ); return we.d.anim;
	case WF_DESCR:	*size = sizeof( we.descr ); return we.descr;
	case WF_STORY:	*size = sizeof( we.d.story ); return we.d.story;
	case WF_VERB:	*size = sizeof( we.d.verb ); return we.d.verb;
	case WF_MAKE_NAME:	*size = sizeof( we.makeName ); return we.makeName;
	case WF_MAKE_DESCR:	*size = sizeof( we.makeDescr ); return we.makeDescr;
	case WF_MAKE_MODEL:	*size = sizeof( we.makeModel ); return we.makeModel;
	default:		return NULL;
	}
}

// the things on the map, nearest first; a click picks one
static void WE_DrawList( float x, float y, float w, float bottom ) {
	int order[WT_MAX], num = wt.num, visible;
	const float rowH = 26.0f;

	for ( int i = 0; i < num; i++ )
		order[i] = i;
	// nearest first
	for ( int i = 1; i < num; i++ ) {
		const int o = order[i];
		const float d = DistanceSquared( wt.things[o].pos, cl.snap.ps.origin );
		int j = i - 1;

		while ( j >= 0 && DistanceSquared( wt.things[order[j]].pos, cl.snap.ps.origin ) > d ) {
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = o;
	}
	if ( !num ) {
		WT_Text( x, y + 10, w, CL_Presence_Session() ? "Nothing on this map yet" : "Not connected to C1B3R", wtDim, wt.font, WT_TEXT, qtrue );
		return;
	}
	visible = (int)( ( bottom - y ) / rowH );
	if ( WG_Scrollbar( x + w - WG_SCROLL_W, y, visible * rowH, num, visible, &we.listScroll, we.cursorX, we.cursorY, &we.click, we.clickX, we.clickY ) )
		w -= WG_SCROLL_W + 2;
	we.listScroll = Com_Clampi( 0, Q_max( 0, num - visible ), we.listScroll );
	for ( int r = 0; r < visible && we.listScroll + r < num; r++ ) {
		const wtThing_t *t = &wt.things[order[we.listScroll + r]];
		const float ry = y + r * rowH;

		WT_FrameBox( x, ry, w, rowH - 2, WT_InRect( x, ry, w, rowH - 2 ) ? wtRowHover : wtRow );
		WT_Text( x + 5, ry + 1, w - 10, WT_Name( t ), wtWhite, wt.font, WT_TEXT, qfalse );
		WT_Text( x + 5, ry + 13, w - 10, va( "#%i %s%s%s%s%s, %.0f away", t->id, t->walk ? "Walk-in zone" : wtKindTitles[t->kind],
			t->loot != WT_LOOT_NONE ? " with loot" : "", t->story[0] ? ", story " : "", t->story, t->mapOnly ? ", till the map loads" : "", Distance( t->pos, cl.snap.ps.origin ) ), wtDim, wt.font, WT_SMALL, qfalse );
		if ( WT_Clicked( x, ry, w, rowH - 2 ) )
			WE_Select( t->id );
	}
}

// - value + : a number stepped, or typed (its value clicked)
static qboolean WE_Stepper( float x, float y, float w, const char *label, const char *value, float *v, float step, float min, float max ) {
	wgStyle_t style;
	wgMouse_t m;

	WT_Text( x, y + 2, w * 0.42f, label, wtDim, wt.font, WT_SMALL, qfalse );
	WT_Style( &style );
	style.fill = wtRow;
	WE_PopMouse( &m );
	if ( !WG_NumberAt( v, &style, &m, x + w * 0.42f, y, w * 0.58f, WT_CTRL_H, v, step, min, max, 1.0f, value ) )
		return qfalse;
	WE_Changed();
	return qtrue;
}

static const char *WE_MakeKindAt( int i, void *ctx ) {
	return i >= 0 && i < WT_NUM_MAKE_KINDS ? wtMakeKinds[i] : "";
}

// how long it stays: as it was saved first (left so), then wtExpiries
static const char *WE_StaysAt( int i, void *ctx ) {
	if ( i <= 0 )
		return we.sel == -1 ? "as it is" : we.d.mapOnly ? "As saved: till the map loads" : we.d.ends ? "As saved: till its time" : "As saved: never";
	return i - 1 < WT_NUM_EXPIRIES ? wtExpiries[i - 1].name : "";
}

// your items, one picked to go in the thing: the whole panel's, each with its model turning
static void WE_DrawPicking( float x, float y, float w, float bottom ) {
	const float rowH = 34.0f, view = 30.0f, bw = 56.0f;
	int visible;

	WT_Text( x, y, w, "Put one of your items in it:", wtOrange, wt.font, WT_TEXT, qfalse );
	y += 16;
	if ( wtMineHttp.state == HTTP_BUSY && !we.numMine ) {
		WT_Text( x, y, w, "Asking C1B3R...", wtDim, wt.font, WT_SMALL, qfalse );
		return;
	}
	if ( !we.numMine ) {
		WT_Text( x, y, w, "You carry nothing to put in (starter items stay with you)", wtDim, wt.font, WT_SMALL, qfalse );
		return;
	}
	visible = Q_max( 1, (int)( ( bottom - y ) / rowH ) );
	if ( WG_Scrollbar( x + w - WG_SCROLL_W, y, visible * rowH, we.numMine, visible, &we.mineScroll, we.cursorX, we.cursorY, &we.click, we.clickX, we.clickY ) )
		w -= WG_SCROLL_W + 2;
	we.mineScroll = Com_Clampi( 0, Q_max( 0, we.numMine - visible ), we.mineScroll );
	for ( int r = 0; r < visible && we.mineScroll + r < we.numMine; r++ ) {
		const wtMine_t *m = &we.mine[we.mineScroll + r];
		const float ry = y + r * rowH;
		float tx = x + 4;

		WT_FrameBox( x, ry, w, rowH - 2, wtField );
		if ( m->model[0] ) {
			wtThing_t look;

			memset( &look, 0, sizeof( look ) );
			look.kind = WT_OBJECT;
			look.scale = 1.0f;
			Q_strncpyz( look.model, m->model, sizeof( look.model ) );
			WT_DrawPreview( &look, x + 1, ry + 1, view, view );
			tx = x + view + 5;
		}
		WT_Text( tx, ry + 4, w - ( tx - x ) - bw - 6, m->name, wtWhite, wt.font, WT_TEXT, qfalse );
		WT_Text( tx, ry + 18, w - ( tx - x ) - bw - 6, m->kind, wtDim, wt.font, WT_SMALL, qfalse );
		if ( WT_Button( x + w - bw - 3, ry + ( rowH - 2 - WT_CTRL_H ) * 0.5f, bw, "Put in", qfalse, (qboolean)( wtActHttp.state != HTTP_BUSY ) ) )
			WT_Act( "lootadd", "/v2/world/loot/add", va( ",\"id\":%i,\"item\":%i", we.sel, m->id ) );
	}
}

// a new item made straight inside it: its name, kind, description and model
static void WE_DrawMaking( float x, float y, float w ) {
	const float row = WT_CTRL_H + 3;

	WT_Text( x, y, w, "Make a new item in it:", wtOrange, wt.font, WT_TEXT, qfalse );
	y += 16;
	WE_Field( x, y, w, WF_MAKE_NAME, we.makeName, "Its name" );
	y += row;
	WT_Text( x, y + 2, w * 0.42f, "Kind", wtDim, wt.font, WT_SMALL, qfalse );
	{
		wgStyle_t style;
		wgMouse_t m;

		WT_Style( &style );
		WE_PopMouse( &m );
		WG_Choice( &style, &m, x + w * 0.42f, y, w * 0.58f, WT_CTRL_H, &we.makeKind, WT_NUM_MAKE_KINDS, WE_MakeKindAt, NULL, qtrue );
	}
	y += row;
	WE_Field( x, y, w, WF_MAKE_DESCR, we.makeDescr, "Its description" );
	y += row;
	WE_Field( x, y, w, WF_MAKE_MODEL, we.makeModel, "Model (empty: its kind's own)" );
	y += row + 4;
	if ( WT_Button( x, y, w, wtActHttp.state == HTTP_BUSY ? "Making..." : "Make it", qtrue, (qboolean)( we.makeName[0] && wtActHttp.state != HTTP_BUSY ) ) ) {
		std::string f = va( ",\"id\":%i,\"kind\":\"%s\"", we.sel, wtMakeKinds[we.makeKind] );

		f += ",\"name\":";
		CL_Presence_Json( f, we.makeName );
		f += ",\"descr\":";
		CL_Presence_Json( f, we.makeDescr );
		f += ",\"model\":";
		CL_Presence_Json( f, we.makeModel );
		WT_Act( "lootnew", "/v2/world/loot/new", f );
	}
	y += row;
	WT_Text( x, y, w, "Like what you put in from your inventory, it's lost", wtDim, wt.font, WT_SMALL, qfalse );
	WT_Text( x, y + 11, w, "with the container unless someone takes it out", wtDim, wt.font, WT_SMALL, qfalse );
}

static void WE_DrawEdit( float x, float y, float w, float bottom ) {
	wtThing_t *d = &we.d;
	const float half = ( w - 4 ) * 0.5f, row = WT_CTRL_H + 3;

	const qboolean looted = (qboolean)( d->kind != WT_ITEM && we.sel > 0 && d->loot != WT_LOOT_NONE );

	if ( !looted )
		we.picking = we.making = qfalse;
	if ( WT_Button( x, y, 60, we.picking || we.making ? "< Back" : "< List", qfalse, qtrue ) ) {
		if ( we.picking || we.making )
			we.picking = we.making = qfalse;
		else
			WE_Select( 0 );
		return;
	}
	WT_Text( x + 66, y + 2, w - 66, we.sel == -1 ? va( "New %s", wtKinds[d->kind] ) : va( "%s #%i", wtKindTitles[d->kind], we.sel ),
		wtOrange, wt.font, WT_TEXT, qfalse );
	y += row + 3;
	// putting things in: the panel is the list of what you carry
	if ( we.picking ) {
		WE_DrawPicking( x, y, w, bottom );
		return;
	}
	if ( we.making ) {
		WE_DrawMaking( x, y, w );
		return;
	}

	if ( d->kind == WT_ITEM ) {
		WT_Text( x, y, w, va( "An item: \"%s\"", WT_Name( d ) ), wtWhite, wt.font, WT_TEXT, qfalse );
		y += 14;
		WT_Text( x, y, w, "It's moved and turned here; its name and", wtDim, wt.font, WT_SMALL, qfalse );
		WT_Text( x, y + 11, w, "description are the item's own", wtDim, wt.font, WT_SMALL, qfalse );
		y += 28;
	} else {
		WE_Field( x, y, w, WF_TITLE, d->title, "Its title" );
		y += row;
		WE_Field( x, y, w, WF_STORY, d->story, d->walk ? "Story name (the storyteller's world zone)" : "Story name (the storyteller waits on its use)" );
		y += row;
		if ( d->kind != WT_ZONE ) {
			WE_Field( x, y, w, WF_VERB, d->verb, va( "Verb (empty: %s)", d->kind == WT_BODY || d->loot != WT_LOOT_NONE ? "Search" : "Read" ) );
			y += row;
		}
		if ( d->kind == WT_ZONE ) {
			if ( WT_Button( x, y, half, "Used (+use)", (qboolean)!d->walk, qtrue ) && d->walk ) {
				d->walk = qfalse;
				WE_Changed();
			}
			if ( WT_Button( x + half + 4, y, half, "Walked into", d->walk, qtrue ) && !d->walk ) {
				d->walk = qtrue;
				d->loot = WT_LOOT_NONE;
				d->usetime = 0.0f;
				WE_Changed();
			}
			y += row;
			// a used one: the word before its title, and whether it opens anything
			if ( !d->walk ) {
				WE_Field( x, y, half, WF_VERB, d->verb, "Verb (Hack, Open...)" );
				if ( WT_Button( x + half + 4, y, half, d->panel ? "Opens its panel" : "Only a trigger", qfalse, qtrue ) ) {
					d->panel = (qboolean)!d->panel;
					if ( !d->panel )
						d->loot = WT_LOOT_NONE;
					WE_Changed();
				}
				y += row;
			}
		}
		if ( d->kind == WT_ZONE && !d->walk && !d->panel ) {
			WT_Text( x, y, w, "Opens nothing: the storyteller's \"uses a", wtDim, wt.font, WT_SMALL, qfalse );
			WT_Text( x, y + 11, w, "world trigger\" hears it used, by its story name", wtDim, wt.font, WT_SMALL, qfalse );
			y += 28;
		}
		if ( d->walk ) {
			WT_Text( x, y, w, "Nothing to see or use: a storyteller's", wtDim, wt.font, WT_SMALL, qfalse );
			WT_Text( x, y + 11, w, "\"world zone\" waits for people in it", wtDim, wt.font, WT_SMALL, qfalse );
			y += 28;
		}
		if ( d->kind != WT_ZONE ) {
			if ( d->kind == WT_BODY ) {
				WE_Field( x, y, w, WF_MODEL, d->model, "Player model (kyle) or a .glm" );
			} else {
				WE_Field( x, y, w - 52, WF_MODEL, d->model, "Model: Pick... or type a path" );
				if ( WT_Button( x + w - 48, y, 48, "Pick...", qfalse, qtrue ) )
					WE_PickModel();
			}
			y += row;
		}
		if ( d->kind == WT_BODY ) {
			WE_Field( x, y, half, WF_SKIN, d->skin, "Skin (default)" );
			WE_Field( x + half + 4, y, half - 36, WF_ANIM, d->anim, "Pose" );
			// the poses in a list: a click on v opens it, the one picked typed in the field
			{
				static int pose;
				wgStyle_t style;
				wgMouse_t m;

				WT_Style( &style );
				style.fill = wtRow;
				WE_PopMouse( &m );
				m.wheel = NULL;
				pose = -1;
				if ( WG_ChoiceBox( &style, &m, x + w - 32, y, 32, WT_CTRL_H, &pose, wtPoses, WT_NUM_POSES ) && pose >= 0 ) {
					Q_strncpyz( d->anim, wtPoses[pose], sizeof( d->anim ) );
					WE_Changed();
				}
				WT_Text( x + w - 32, y + 2, 32, "v", wtWhite, wt.font, WT_TEXT, qtrue );
			}
			y += row;
		}
		// the description
		if ( !d->walk && ( d->kind != WT_ZONE || d->panel ) ) {
			wgStyle_t style;
			const int lootRows = looted ? Q_min( 4, Q_max( 1, we.numLoot ) ) : 0;
			// what's drawn under it: loot choice, search, stays, size, place, modes, bring, the loot, save
			const float under = 11 + row * 3 + ( d->kind == WT_ZONE ? 14 : row ) + 14 + row + row + 4
				+ ( looted ? 13 + lootRows * row + row : d->loot != WT_LOOT_NONE && we.sel == -1 ? 14 : 0 ) + WT_CTRL_H + 6;
			const float h = Com_Clamp( 36.0f, 110.0f, bottom - y - 4 - under );

			WT_Style( &style );
			WT_FrameBox( x, y, w, h, wtField );
			if ( we.focus == WF_DESCR )
				WT_Fill( x, y + h - 1, w, 1, wtOrange );
			if ( !we.descrKnown ) {
				WT_Text( x + 4, y + 4, w - 8, "Asking C1B3R...", wtDim, wt.font, WT_SMALL, qfalse );
			} else {
				WG_TextArea( &style, x, y, w, h, we.descr, (qboolean)( we.focus == WF_DESCR ), "What's read: its description",
					we.cursorX, we.cursorY, &we.click, we.clickX, we.clickY );
				if ( WT_Clicked( x, y, w, h ) ) {
					if ( we.focus != WF_DESCR )
						WG_EditReset();
					we.focus = WF_DESCR;
					WG_FieldClick( we.descr, we.clickX, we.clickY );
				}
			}
			y += h + 4;
		}
		// loot, the search, how long it stays
		if ( !d->walk && ( d->kind != WT_ZONE || d->panel ) ) {
			WT_Text( x, y, w, d->kind == WT_BODY ? "Container: loot on the body" : "Container: loot inside", wtDim, wt.font, WT_SMALL, qfalse );
			y += 11;
			for ( int l = 0; l < WT_NUM_LOOTS; l++ ) {
				const float bw = ( w - 8 ) / 3.0f;

				if ( WT_Button( x + l * ( bw + 4 ), y, bw, wtLootTitles[l], (qboolean)( d->loot == l ), qtrue ) && d->loot != l ) {
					d->loot = l;
					WE_Changed();
				}
			}
			y += row;
		}
		if ( !d->walk ) {
			WE_Stepper( x, y, w, d->kind == WT_ZONE && !d->panel ? "Hold use" : "Search (hold use)", d->usetime > 0.0f ? va( "%g s", d->usetime ) : "none", &d->usetime, 0.5f, 0.0f, 10.0f );
			y += row;
		}
		{
			static int stays;
			wgStyle_t style;
			wgMouse_t m;

			WT_Text( x, y + 2, w * 0.42f, "Stays", wtDim, wt.font, WT_SMALL, qfalse );
			WT_Style( &style );
			style.fill = we.expiry >= 0 ? wtRowOn : wtRow;
			WE_PopMouse( &m );
			stays = we.expiry + 1;
			if ( WG_Choice( &style, &m, x + w * 0.42f, y, w * 0.58f, WT_CTRL_H, &stays, WT_NUM_EXPIRIES + 1, WE_StaysAt, NULL, qtrue ) ) {
				we.expiry = stays - 1;
				WE_Changed();
			}
			y += row;
		}
	}
	// size and place
	if ( d->kind == WT_ZONE ) {
		WT_Text( x, y + 2, w, va( "Size %.0f x %.0f x %.0f (Size mode: arrows drag it)", d->size[0] * 2, d->size[1] * 2, d->size[2] * 2 ),
			wtDim, wt.font, WT_SMALL, qfalse );
		y += 14;
	} else {
		WE_Stepper( x, y, w, "Size", va( "%.2f", d->scale ), &d->scale, 0.05f, 0.1f, 8.0f );
		y += row;
	}
	WT_Text( x, y + 1, w, va( "At %.0f %.0f %.0f   turned %.0f %.0f %.0f", d->pos[0], d->pos[1], d->pos[2], d->ang[PITCH], d->ang[YAW], d->ang[ROLL] ),
		wtDim, wt.font, WT_SMALL, qfalse );
	y += 14;
	{
		const int modes = d->kind == WT_ZONE ? 3 : 2;
		const float bw = ( w - 4 * ( modes - 1 ) ) / modes;
		static const char *names[3] = { "Move", "Turn", "Size" };

		for ( int m = 0; m < modes; m++ ) {
			if ( WT_Button( x + m * ( bw + 4 ), y, bw, names[m], (qboolean)( we.mode == m ), qtrue ) ) {
				WE_DropDrag();
				we.mode = m;
			}
		}
		y += row;
	}
	if ( WT_Button( x, y, half, "Bring it here", qfalse, qtrue ) ) {
		float yaw;

		WT_AimPoint( d->pos, &yaw );
		if ( d->kind == WT_ZONE )
			d->pos[2] += d->size[2];
		WE_Changed();
	}
	if ( WT_Button( x + half + 4, y, half, we.freeCam ? "Your view (F)" : "Free camera (F)", we.freeCam, qtrue ) )
		WE_FreeCam( (qboolean)!we.freeCam );
	y += row + 4;

	// what's in it
	if ( looted ) {
		const float listBottom = bottom - WT_CTRL_H - 6 - row;		// the put button and the save row under it
		const int fit = Q_max( 1, (int)( ( listBottom - y - 13 ) / row ) );
		float lw = w;

		WT_Text( x, y, w, we.numLoot ? va( "In it (%i):", we.numLoot ) : "Nothing in it yet", wtOrange, wt.font, WT_SMALL, qfalse );
		y += 13;
		if ( we.numLoot && WG_Scrollbar( x + w - WG_SCROLL_W, y, fit * row - 3, we.numLoot, fit, &we.editLootScroll,
				we.cursorX, we.cursorY, &we.click, we.clickX, we.clickY ) )
			lw -= WG_SCROLL_W + 2;
		we.editLootScroll = Com_Clampi( 0, Q_max( 0, we.numLoot - fit ), we.editLootScroll );
		for ( int r = 0; r < fit && we.editLootScroll + r < we.numLoot; r++ ) {
			const wtLoot_t *l = &we.loot[we.editLootScroll + r];
			float nx = x;

			if ( l->model[0] ) {
				wtThing_t look;

				memset( &look, 0, sizeof( look ) );
				look.kind = WT_OBJECT;
				look.scale = 1.0f;
				Q_strncpyz( look.model, l->model, sizeof( look.model ) );
				WT_DrawPreview( &look, x, y, WT_CTRL_H, WT_CTRL_H );
				nx += WT_CTRL_H + 3;
			}
			WT_Text( nx, y + 2, lw - 24 - ( nx - x ), va( "%s%s", l->name, l->taken ? " (taken)" : "" ), wtWhite, wt.font, WT_SMALL, qfalse );
			if ( WT_Button( x + lw - 20, y, 20, "x", qfalse, (qboolean)( wtActHttp.state != HTTP_BUSY ) ) )
				WT_Act( "lootremove", "/v2/world/loot/remove", va( ",\"id\":%i,\"item\":%i", we.sel, l->item ) );
			y += row;
		}
		if ( WT_Button( x + half + 4, Q_min( y, listBottom ), half, "Make a new item", qfalse, qtrue ) ) {
			we.making = qtrue;
			we.focus = WF_MAKE_NAME;
			WG_EditReset();
		}
		if ( WT_Button( x, Q_min( y, listBottom ), half, "From your inventory", qfalse, qtrue ) ) {
			we.picking = qtrue;
			we.mineScroll = 0;
			we.numMine = 0;
			WT_Post( &wtMineHttp, "/v2/inv/list", "" );
		}
	} else if ( d->kind != WT_ITEM && we.sel == -1 && d->loot != WT_LOOT_NONE ) {
		WT_Text( x, y, w, "Save it first, then put things in it", wtDim, wt.font, WT_SMALL, qfalse );
	}

	// saved, or taken away
	{
		const float by = bottom - WT_CTRL_H;

		if ( we.sel == -1 ) {
			if ( WT_Button( x, by, w, wtActHttp.state == HTTP_BUSY ? "Building..." : "Build it", qtrue, (qboolean)( wtActHttp.state != HTTP_BUSY ) ) )
				WE_Save();
		} else if ( d->kind == WT_ITEM ) {
			if ( WT_Button( x, by, ( w - 8 ) / 3, "Save", we.changed, we.changed ) )
				WE_Save();
			if ( WT_Button( x + ( w - 8 ) / 3 + 4, by, ( w - 8 ) / 3, "Give back", qfalse, qtrue ) )
				WT_Act( "remove", "/v2/world/remove", va( ",\"id\":%i,\"back\":1", we.sel ) );
			if ( WT_Button( x + ( ( w - 8 ) / 3 + 4 ) * 2, by, ( w - 8 ) / 3, we.confirmRemove ? "Sure?" : "Delete", we.confirmRemove, qtrue ) ) {
				if ( we.confirmRemove )
					WT_Act( "remove", "/v2/world/remove", va( ",\"id\":%i,\"back\":0", we.sel ) );
				we.confirmRemove = (qboolean)!we.confirmRemove;
			}
		} else {
			if ( WT_Button( x, by, half, wtActHttp.state == HTTP_BUSY ? "Saving..." : "Save", we.changed, we.changed ) )
				WE_Save();
			if ( WT_Button( x + half + 4, by, half, we.confirmRemove ? "Sure? Remove" : "Remove", we.confirmRemove, qtrue ) ) {
				if ( we.confirmRemove )
					WT_Act( "remove", "/v2/world/remove", va( ",\"id\":%i,\"back\":0", we.sel ) );
				we.confirmRemove = (qboolean)!we.confirmRemove;
			}
		}
	}
}

static void WE_Draw( void ) {
	const float x = WE_X + 8, w = WE_W - 16, bottom = WE_Y + WE_H - 40;
	float y = WE_Y + 8;
	const float dt = Com_Clamp( 0.0f, 0.1f, ( cls.realtime - we.lastFrameTime ) / 1000.0f );

	we.lastFrameTime = cls.realtime;
	{
		wgMouse_t m;

		WE_PopMouse( &m );
		WG_PopupBegin( &m );
		if ( WG_PopupTyping() )
			we.focus = WF_NONE;
	}
	if ( !we.mouseDown )
		we.dragging = WE_DRAG_NONE;
	if ( we.freeCam )
		WE_UpdateCamera( dt );
	// first, under the panel: the zones, and a click outside it on an arrow or a ring is the gizmo's
	WE_DrawZones();
	WE_GizmoFrame();
	WT_FrameBox( WE_X, WE_Y, WE_W, WE_H, wtPanel );
	WT_Text( x, y, w - 60, "WORLD", wtOrange, wt.titleFont, WT_ITEM_SCALE + 0.15f, qfalse );
	if ( WT_Button( WE_X + WE_W - 8 - 52, y + 2, 52, "Close", qfalse, qtrue ) ) {
		WE_Close();
		return;
	}
	y += 20;
	WT_Text( x, y, w, va( "%s: %i here", WT_Map(), wt.num ), wtDim, wt.font, WT_SMALL, qfalse );
	y += 16;

	if ( !WE_Editing() ) {
		const float half = ( w - 4 ) * 0.5f;

		if ( WT_Button( x, y, half, "New object", qfalse, qtrue ) )
			WE_New( WT_OBJECT, qfalse, qfalse );
		if ( WT_Button( x + half + 4, y, half, "New zone", qfalse, qtrue ) )
			WE_New( WT_ZONE, qfalse, qfalse );
		y += WT_CTRL_H + 4;
		if ( WT_Button( x, y, half, "New body", qfalse, qtrue ) )
			WE_New( WT_BODY, qfalse, qfalse );
		if ( WT_Button( x + half + 4, y, half, "New looted body", qfalse, qtrue ) )
			WE_New( WT_BODY, qtrue, qfalse );
		y += WT_CTRL_H + 4;
		if ( WT_Button( x, y, half, "New container", qfalse, qtrue ) )
			WE_New( WT_OBJECT, qtrue, qfalse );
		if ( WT_Button( x + half + 4, y, half, "New walk-in zone", qfalse, qtrue ) )
			WE_New( WT_ZONE, qfalse, qtrue );
		y += WT_CTRL_H + 8;
		WE_DrawList( x, y, w, bottom );
	} else {
		WE_DrawEdit( x, y, w, bottom );
	}

	// the keys, and what was said
	if ( we.message[0] && cls.realtime - we.messageTime < WT_MESSAGE_MS )
		WT_Text( x, WE_Y + WE_H - 34, w, we.message, we.messageBad ? wtYellow : wtGreen, wt.font, WT_SMALL, qfalse );
	else if ( WE_Editing() && we.changed )
		WT_Text( x, WE_Y + WE_H - 34, w, we.sel == -1 ? "Not built yet" : "Not saved yet", wtYellow, wt.font, WT_SMALL, qfalse );
	WT_Text( x, WE_Y + WE_H - 21, w, va( "Tab mode, G step %g, J gizmo, F camera", ( we.mode == WE_TURN ? weTurnSteps : weMoveSteps )[we.step] ),
		wtDim, wt.font, 0.55f, qfalse );
	WT_Text( x, WE_Y + WE_H - 12, w, "Arrows, PgUp/PgDn nudge; RMB looks, WASD flies", wtDim, wt.font, 0.55f, qfalse );
}

void CL_WorldItems_Draw( void ) {
	WT_LoadFonts();
	if ( we.open && we.modelPick )
		return;		// the model manager's browser is over it
	if ( we.open ) {
		WE_Draw();
		we.click = qfalse;
		we.wheel = 0;
		WG_DrawOverlay();
		WT_Cursor();
		return;
	}
	if ( wr.open ) {
		WR_Draw();
		we.click = qfalse;
		WT_Cursor();
		return;
	}
	WT_DrawPrompt();
}

/*
===============================================================================

Keys and the mouse, while the reader or the editor is open

===============================================================================
*/

void CL_WorldItems_KeyEvent( int key, qboolean down ) {
	if ( !we.open && !wr.open )
		return;
	if ( key >= 0 && key < MAX_KEYS )
		we.held[key] = down;
	if ( key == A_MOUSE1 ) {
		we.mouseDown = down;
		if ( down ) {
			we.click = qtrue;
			we.clickX = we.cursorX;
			we.clickY = we.cursorY;
		} else {
			we.dragging = WE_DRAG_NONE;
		}
		return;
	}
	if ( !down )
		return;
	if ( wr.open ) {
		if ( key == A_MWHEELUP )
			wr.scroll = Q_max( 0.0f, wr.scroll - 3 );
		else if ( key == A_MWHEELDOWN )
			wr.scroll += 3;		// held in range as it's drawn
		return;
	}
	// an open list or number first: the wheel scrolls it
	if ( WG_PopupOpen() && ( key == A_MWHEELUP || key == A_MWHEELDOWN ) ) {
		we.wheel += key == A_MWHEELUP ? -1 : 1;
		return;
	}
	if ( WG_PopupKey( key ) )
		return;
	// typing in a field
	if ( we.focus != WF_NONE ) {
		int size;
		char *buf = WE_FocusBuf( &size );

		if ( we.focus == WF_DESCR && ( key == A_MWHEELUP || key == A_MWHEELDOWN ) ) {
			WG_TextAreaScroll( key == A_MWHEELUP ? -3 : 3 );
			return;
		}
		if ( we.focus != WF_DESCR && ( key == A_ENTER || key == A_KP_ENTER || key == A_TAB ) ) {
			we.focus = WF_NONE;
			WG_EditReset();
			return;
		}
		if ( buf && WG_EditKey( buf, size, key, we.focus == WF_STORY ? WT_StoryChar : NULL ) == WG_EDIT_CHANGED ) {
			WE_Changed();
			// a model or skin typed: its box found again
			if ( we.focus == WF_MODEL || we.focus == WF_SKIN )
				we.d.boundsKnown = qfalse;
		}
		return;
	}
	switch ( key ) {
	case A_TAB:
		WE_DropDrag();
		we.mode = ( we.mode + 1 ) % ( we.d.kind == WT_ZONE && WE_Editing() ? 3 : 2 );
		break;
	case A_CAP_G:
		we.step = ( we.step + 1 ) % 3;
		break;
	case GZ_KEY:
		WE_DropDrag();
		we.gzHidden = (qboolean)!we.gzHidden;
		break;
	case A_CAP_F:
		WE_FreeCam( (qboolean)!we.freeCam );
		break;
	case A_CURSOR_LEFT: case A_CURSOR_RIGHT: case A_CURSOR_UP: case A_CURSOR_DOWN: case A_PAGE_UP: case A_PAGE_DOWN:
		WE_KeyStep( key );
		break;
	case A_MWHEELUP:
	case A_MWHEELDOWN:
		if ( we.freeCam ) {
			vec3_t forward;

			AngleVectors( we.camAng, forward, NULL, NULL );
			VectorMA( we.camOrg, key == A_MWHEELUP ? 16.0f : -16.0f, forward, we.camOrg );
		} else if ( !WE_Editing() ) {
			we.listScroll = Q_max( 0, we.listScroll + ( key == A_MWHEELUP ? -2 : 2 ) );
		}
		break;
	default:
		break;
	}
}

void CL_WorldItems_CharEvent( int ch ) {
	int size;
	char *buf;

	if ( we.open && WG_PopupChar( ch ) )
		return;
	if ( !we.open || we.focus == WF_NONE || !( buf = WE_FocusBuf( &size ) ) )
		return;
	if ( we.focus != WF_DESCR && ( ch == '\r' || ch == '\n' ) )
		return;
	if ( WG_EditChar( buf, size, ch, we.focus == WF_STORY ? WT_StoryChar : NULL ) ) {
		WE_Changed();
		if ( we.focus == WF_MODEL || we.focus == WF_SKIN )
			we.d.boundsKnown = qfalse;
	}
}

void CL_WorldItems_MouseEvent( int dx, int dy ) {
	// the free camera looks while the right button's held, the cursor stays
	if ( we.open && we.freeCam && we.held[A_MOUSE2] ) {
		we.camAng[YAW] -= dx * cl_sensitivity->value * m_yaw->value;
		we.camAng[PITCH] = Com_Clamp( -89.0f, 89.0f, we.camAng[PITCH] + dy * cl_sensitivity->value * m_pitch->value );
		return;
	}
	we.cursorX = Com_Clamp( 0, SCREEN_WIDTH, we.cursorX + dx );
	we.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, we.cursorY + dy );
}

// typing stopped first, then a thing let go, then closed
void CL_WorldItems_Escape( void ) {
	if ( wr.open ) {
		WR_Close();
		return;
	}
	if ( WG_PopupOpen() ) {
		WG_PopupClose();
		return;
	}
	if ( we.focus != WF_NONE ) {
		we.focus = WF_NONE;
		WG_EditReset();
		return;
	}
	if ( we.picking || we.making ) {
		we.picking = we.making = qfalse;
		return;
	}
	WE_Close();
}

/*
===============================================================================

Dropped from the inventory, picked up, and the command

===============================================================================
*/

// an item of yours put down where you aim (or at your feet), facing you; anyone picks it up
void CL_WorldItems_Drop( int item, const char *name ) {
	vec3_t at;
	float yaw;

	if ( cls.state != CA_ACTIVE ) {
		Com_Printf( "World: join a server first\n" );
		return;
	}
	if ( item <= 0 ) {
		Com_Printf( "World: pick an item in /inventory to drop\n" );
		return;
	}
	WT_AimPoint( at, &yaw );
	WT_Act( "drop", "/v2/world/drop", va( ",\"item\":%i,\"pos\":[%.1f,%.1f,%.1f],\"yaw\":%.0f", item, at[0], at[1], at[2], yaw ) );
	if ( name && name[0] )
		Com_Printf( "World: dropping %s...\n", name );
}

void CL_WorldItems_PickUp( int world ) {
	WT_Act( "pickup", "/v2/world/pickup", va( ",\"id\":%i", world ) );
}

static void WT_Near( void ) {
	int shown = 0;

	for ( int i = 0; i < wt.num; i++ ) {
		const wtThing_t *t = &wt.things[i];
		const float d = Distance( t->pos, cl.snap.ps.origin );

		if ( d > WT_NEAR )
			continue;
		Com_Printf( "#%-4i %-7s %s" S_COLOR_WHITE "  (%.0f away%s)\n", t->id, wtKinds[t->kind], WT_Name( t ), d,
			t->loot != WT_LOOT_NONE ? ", loot" : "" );
		shown++;
	}
	if ( !shown && wt.num )
		Com_Printf( "World: nothing near you (%i on this map)\n", wt.num );
	else if ( !shown )
		Com_Printf( "World: nothing on this map%s\n", CL_Presence_Session() ? "" : " (not connected to C1B3R)" );
}

// datapad place|remove|drop, the old names: what they are now
void CL_WorldItems_Command( const char *what, const char *args ) {
	if ( !Q_stricmp( what, "place" ) ) {
		Com_Printf( "World: datapad place is gone. Drop a datapad you carry from /inventory, or build things with /" WT_CMD " (admins)\n" );
	} else if ( !Q_stricmp( what, "remove" ) ) {
		Com_Printf( "World: datapad remove is gone. /" WT_CMD " lists what's here: pick one to give it back or remove it (admins)\n" );
	} else {
		Com_Printf( "World: datapad drop is /" WT_CMD " near now\n" );
		WT_Near();
	}
}

static void WT_World_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !arg[0] ) {
		if ( we.open )
			WE_Close();
		else
			WE_Open();
	} else if ( !Q_stricmp( arg, "read" ) ) {
		WR_ReadAimed();
	} else if ( !Q_stricmp( arg, "drop" ) ) {
		char name[64];
		const int item = CL_Inventory_Picked( name, sizeof( name ) );

		if ( item <= 0 )
			Com_Printf( "World: pick the item to drop in /inventory first (not one every character has)\n" );
		else
			CL_WorldItems_Drop( item, name );
	} else if ( !Q_stricmp( arg, "near" ) ) {
		WT_Near();
	} else if ( !Q_stricmp( arg, "edit" ) && Cmd_Argc() > 2 ) {
		const int id = atoi( Cmd_Argv( 2 ) );

		WE_Open();
		if ( we.open ) {
			if ( WT_Find( id ) )
				WE_Select( id );
			else
				WE_Message( va( "No #%i on this map", id ), qtrue );
		}
	} else {
		Com_Printf( "usage: " WT_CMD "              the world editor (RPMod admins): objects, zones, bodies, containers\n" );
		Com_Printf( "       " WT_CMD " read         reads what you look at, or the zone you stand in (the use key does too)\n" );
		Com_Printf( "       " WT_CMD " drop         drops the item picked in /inventory where you aim\n" );
		Com_Printf( "       " WT_CMD " near         what's near you\n" );
		Com_Printf( "       " WT_CMD " edit <id>    the editor on that one\n" );
	}
}

void CL_WorldItems_Init( void ) {
	memset( &wt, 0, sizeof( wt ) );
	memset( &we, 0, sizeof( we ) );
	memset( &wr, 0, sizeof( wr ) );
	wt.aim = wt.zone = -1;
	we.step = 1;
	we.dragging = WE_DRAG_NONE;
	we.gzHot = we.gzPart = -1;
	Cmd_AddCommand( WT_CMD, WT_World_f, "Things in the world: dropped items, objects, zones and bodies to read and search, kept by C1B3R" );
}
