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

// cl_npcmanager.cpp -- engine-side front end for RPMod's /npc command
//
// Spawn tab: every NPC type in ext_data/npcs/*.npc the filesystem can see, Base (the
// game's own pk3s) or Custom, with targetname, team, model, scale and a spot picked on
// the map, sent as "npc spawn". Picking on the map flies a free camera, so the spot can
// be anywhere, not just in view. Several can be put down there and spawned together,
// one a second as the server's flood protection lets them through, and saved as an NPC
// construct to put down again. The form's settings can be saved as an NPC of their own.
// Manage tab: the NPCs in the map, read from the server's "npc list" reply, and the
// kill/freeze/emote/follow/dialog/tele/hologram/score commands to run on them.
//
// This lives in the engine rather than cgame so it works whatever cgame the
// server's mod ships (RPMod loads its own). The list is read off the "print"
// server commands on their way to the cgame, and kept out of the console when
// the manager asked for it.

#include "client.h"
#include "qcommon/cm_public.h"
#include "ghoul2/G2.h"

#define NM_TOGGLE_CMD		"npcmanager"
#define NM_LIST_HEADER		"List of all NPCs in the map"

#define NM_SAVE_DIR			"npcmanager"		// in the game folder
#define NM_NPC_DIR			NM_SAVE_DIR "/npcs"
#define NM_CONSTRUCT_DIR	NM_SAVE_DIR "/constructs"
#define NM_SAVE_EXT			".cfg"
#define NM_BASE_PAKS		BASEGAME "/assets"	// base/assets0.pk3 to assets3.pk3 are the game's own

#define NM_MAX_TYPES		4096
#define NM_MAX_SKINS		8192
#define NM_MAX_NPCS			1024
#define NM_MAX_FILES		1024
#define NM_MAX_SOURCES		256
#define NM_MAX_SAVED		512
#define NM_MAX_PLACED		256		// put down, not spawned yet; also a construct's NPCs
#define NM_MAX_QUEUE		512
#define NM_CMD_LEN			256
#define NM_POOL_SIZE		( 512 * 1024 )
#define NM_FIELD_LEN		64
#define NM_MESSAGE_MS		5000

#define NM_LIST_TIMEOUT		2500	// ms without the list header before giving up on it
#define NM_LIST_LINE_GAP	1500	// ms between list lines before the list is over
#define NM_REPLY_MS			3000	// prints this soon after a command are shown as its reply
#define NM_REFRESH_DELAY	400		// ms after a kill or tele before reading the list again
#define NM_CONFIRM_MS		3000
#define NM_DOUBLECLICK_MS	400
#define NM_TRACE_MASK		(CONTENTS_SOLID|CONTENTS_TERRAIN)
#define NM_TRACE_DIST		8192.0f
#define NM_GROUND_HEIGHT	25.0f	// NPC origins sit this far above the floor
#define NM_FRONT_DIST		96.0f	// "in front of me"
#define NM_YAW_STEP			15
#define NM_FLY_SPEED		400.0f	// free camera, units a second
#define NM_LABEL_RANGE		32.0f	// a label this close to the crosshair is the one picked

// layout, in 640x480
#define NM_ROW_H			13.0f
#define NM_CTRL_H			15.0f
#define NM_TEXT_SCALE		0.8f
#define NM_LEFT_X			16.0f
#define NM_LEFT_W			250.0f
#define NM_RIGHT_X			276.0f
#define NM_RIGHT_W			348.0f
#define NM_CTRL_X			( NM_RIGHT_X + 68.0f )
#define NM_CTRL_W			( NM_RIGHT_X + NM_RIGHT_W - NM_CTRL_X )
#define NM_SEARCH_Y			46.0f
#define NM_LIST_Y			66.0f
#define NM_LIST_H			364.0f
#define NM_TYPE_LIST_Y		( NM_LIST_Y + 20.0f )	// under the Base / Custom / Saved buttons
#define NM_TYPE_LIST_H		( NM_LIST_H - 20.0f )
#define NM_PLACED_Y			222.0f
#define NM_PLACED_ROWS		7
#define NM_BUTTON_Y			410.0f
#define NM_PICKER_X			150.0f
#define NM_PICKER_Y			50.0f
#define NM_PICKER_W			340.0f
#define NM_PICKER_H			380.0f
#define NM_PICKER_MODELS_X	60.0f	// the model picker, with its preview
#define NM_PICKER_PREVIEW_W	200.0f
#define NM_PREVIEW_X		170.0f
#define NM_PREVIEW_Y		40.0f
#define NM_PREVIEW_W		300.0f
#define NM_PREVIEW_H		400.0f

typedef struct nmType_s {
	const char	*name;		// what "npc spawn" takes
	const char	*model;		// its playerModel[/customSkin], "" if it has none
	qboolean	vehicle;
	// what else changes its look, for the preview
	const char	*surfOff, *surfOn;	// comma separated
	byte		rgba[4];			// customRGBA, white if it has none or a random one
	int			scale;				// percent
	const char	*weapon;			// WP_ name, "" for none
	const char	*saber, *saber2;	// .sab names, "" for none
	int			source;				// where its .npc file is read from, index into sources
	qboolean	custom;				// not from the game's own pk3s
} nmType_t;

typedef struct nmNpc_s {
	int			num;
	vec3_t		origin;
	int			yaw;
	char		type[MAX_QPATH];
	char		name[MAX_QPATH];	// targetname, "" if it has none
} nmNpc_t;

typedef struct nmList_s {
	int			sel, scroll;
	int			lastClickTime, lastClickRow;
} nmList_t;

typedef enum {
	NM_TAB_SPAWN,
	NM_TAB_MANAGE
} nmTab_t;

typedef enum {
	NM_PICK_NONE,
	NM_PICK_SPAWN,		// where to spawn
	NM_PICK_PLACE,		// where to put down what is in hand, as many times as wanted
	NM_PICK_TELE,		// where to teleport the selection to
	NM_PICK_SELECT		// which NPC to select
} nmPick_t;

typedef enum {
	NM_SRC_ALL,
	NM_SRC_BASE,
	NM_SRC_CUSTOM,
	NM_SRC_SAVED,		// saved NPCs and NPC constructs
	NM_SRC_COUNT
} nmSource_t;

static const char *nmSourceLabels[NM_SRC_COUNT] = { "All", "Base", "Custom", "Saved" };

typedef enum {
	NM_SAVING_NONE,
	NM_SAVING_NPC,			// the form's settings
	NM_SAVING_CONSTRUCT		// the NPCs put down
} nmSaving_t;

typedef enum {
	NM_PICKER_NONE,
	NM_PICKER_MODEL,
	NM_PICKER_EMOTE,
	NM_PICKER_PLAYER
} nmPicker_t;

typedef enum {
	NM_SHOW_ALL,
	NM_SHOW_CHARACTERS,
	NM_SHOW_VEHICLES
} nmShow_t;

typedef enum {
	NM_TEAM_DEFAULT,
	NM_TEAM_PLAYER,
	NM_TEAM_ENEMY
} nmTeam_t;

typedef enum {
	NM_APPLY_ONE,		// the selected NPC, by number
	NM_APPLY_NAME,		// every NPC sharing its targetname
	NM_APPLY_ALL
} nmApply_t;

typedef enum {
	NM_F_NONE,			// typing goes to the search box of whatever is showing
	NM_F_TYPE_SEARCH,
	NM_F_TARGETNAME,
	NM_F_MODEL,
	NM_F_SCALE,
	NM_F_NPC_SEARCH,
	NM_F_EMOTE,
	NM_F_DIALOG,
	NM_F_PICKER_SEARCH,
	NM_F_SAVE_NAME,
	NM_F_TELE_X,		// the teleport box, in this order
	NM_F_TELE_Y,
	NM_F_TELE_Z,
	NM_F_TELE_YAW,
	NM_NUM_FIELDS
} nmField_t;

typedef enum {
	NM_CONFIRM_NONE,
	NM_CONFIRM_KILL,
	NM_CONFIRM_SPAWNERS,
	NM_CONFIRM_CLEAR
} nmConfirm_t;

// what one spawn takes: the form's, a saved NPC's, or one of a construct
typedef struct nmSpawn_s {
	char		type[MAX_QPATH];
	qboolean	vehicle;
	char		targetname[NM_FIELD_LEN];
	nmTeam_t	team;
	char		model[NM_FIELD_LEN];	// "" = its own
	char		scale[NM_FIELD_LEN];	// "" = its own
	vec3_t		pos;					// in the map; in a construct, from its middle
	int			yaw;
} nmSpawn_t;

// a file in NM_NPC_DIR or NM_CONSTRUCT_DIR
typedef struct nmSaved_s {
	char		name[MAX_QPATH];
	qboolean	construct;
	int			count;		// a construct's NPCs
	nmSpawn_t	npc;		// a saved NPC's settings
} nmSaved_t;

// the emotes in RPMod's help menu
static const char *nmEmotes[] = {
	"emtalk",
	"emtalk2",
	"emcomm",
	"emknockdown",
	"emknockdown2",
	"emknockdown3",
	"emkneel",
	"emkneel2",
	"emkneel3",
	"emsurrender",
	"emraisehand",
	"emsleep",
	"emconsole",
	"emconsolecomm",
	"emyes",
	"emno",
	"emcomeon",
	"emcomeon2",
	"emhello",
	"embeg",
	"emcower",
	"emnoisy",
	"emwait",
	"ematease",
	"emhips",
	"emreach",
	"emreach2",
	"emlotus",
	"emsit",
	"emsit2",
	"emsit3",
	"emsit4",
	"emsit5",
	"emsit6",
	"emsit7",
	"emsit8",
	"emsit9",
	"emsit10",
	"emsit11",
	"emsit12",
	"emsitconsole",
	"emliedown",
	"emhandsignal1",
	"emhandsignal2",
	"emhandsignal3",
	"emhandsignal4",
	"emhandsignall",
	"emhandsignalr",
	"emgrip",
	"emgrip2",
	"emgrip3",
	"emgrip4",
	"emthrow",
	"embutton",
	"embutton2",
	"emsniper",
	"emknockback",
	"emhurt",
	"emgrab",
	"emgrabbed",
	"emheadtiltl",
	"emshy",
	"emshuffle",
	"embalance",
	"emcheer",
	"emdeath",
	"emdeath2",
	"emdeath3",
	"emdeath4",
	"emdead",
	"emdead2",
	"emdead3",
	"emdodgel",
	"emdodger",
	"embackflip",
	"emsaberkick",
	"emsaberpose",
	"emsaberpose2",
	"emsaberpose3",
	"emsaberpose4",
	"emsaberpose5",
	"emsaberpose6",
	"emsaberfinish",
	"emsaberthrow",
	"emsaberthrow2",
	"empoint",
	"empoint2",
	"emwindy",
	"emsabertaunt",
	"emsabertaunt2",
	"emsabertaunt3",
	"emsabertaunt4",
	"emsaberlock1",
	"emsaberlock2",
	"emcrossarms",
	"emlift",
	"emlift2",
	"empose",
	"empose2",
	"empose3",
	"empose4",
	"emsalute",
	"emlean",
	"emgun",
	"ematease2",
};

static const char *nmTeamArgs[] = { "0", "player", "enemy" };
static const char *nmTeamLabels[] = { "Default", "Player (blue)", "Enemy (red)" };

static struct {
	qboolean	open;
	int			font;
	nmTab_t		tab;
	nmPick_t	pick;
	nmPicker_t	picker;

	// input; clicks and the wheel are handled where the control under them is drawn
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;
	int			wheel;
	nmField_t	focus;
	char		fields[NM_NUM_FIELDS][NM_FIELD_LEN];

	// index, rebuilt the first time the manager opens after cgame (re)starts
	qboolean	indexed;
	char		pool[NM_POOL_SIZE];
	int			poolUsed;
	nmType_t	types[NM_MAX_TYPES];
	int			numTypes;
	const char	*skins[NM_MAX_SKINS];
	int			numSkins;
	const char	*sources[NM_MAX_SOURCES];	// "gamedir/name.pk3", or "gamedir" for loose files
	int			numSources;
	struct {
		const char	*file;					// in ext_data/npcs
		int			source;					// the one in use
	} files[NM_MAX_FILES];
	int			numFiles;

	// saved NPCs and constructs
	nmSaved_t	saved[NM_MAX_SAVED];
	int			numSaved;
	char		appliedSaved[MAX_QPATH];	// the saved NPC whose settings are in the form, "" = none
	char		formBefore[3][NM_FIELD_LEN];	// targetname, model and scale from before it
	nmTeam_t	teamBefore;
	char		constructShown[MAX_QPATH];	// the construct read into constructNpcs
	nmSpawn_t	constructNpcs[NM_MAX_PLACED];
	int			numConstructNpcs;
	nmSaving_t	saving;						// the name box is open
	qboolean	teleBox;					// the box to teleport to typed coordinates is open

	// spawn tab
	nmShow_t	show;
	nmSource_t	source;
	nmTeam_t	team;
	int			typeView[NM_MAX_TYPES + NM_MAX_SAVED];	// type index, or -1 - saved index
	int			numTypeView;
	nmList_t	typeList;
	qboolean	havePos;		// qfalse = in front of the player
	vec3_t		pos;
	int			yaw;

	// put down on the map, waiting for "Spawn all"
	nmSpawn_t	placed[NM_MAX_PLACED];
	int			numPlaced;
	int			placedScroll;
	// what each click puts down: the form's NPC, facing the camera, or a construct
	nmSpawn_t	hand[NM_MAX_PLACED];
	int			numHand;
	qboolean	handConstruct;
	char		handName[MAX_QPATH];

	// spawns go out one at a time, as the server's flood protection lets them through
	char		queue[NM_MAX_QUEUE][NM_CMD_LEN];
	int			numQueue, queueTotal;
	int			nextSend;

	// manage tab
	nmNpc_t		npcs[NM_MAX_NPCS];
	int			numNpcs;
	int			npcView[NM_MAX_NPCS];
	int			numNpcView;
	nmList_t	npcList;
	int			selNum;			// entity number of the selected NPC, -1 = none
	nmApply_t	apply;
	nmConfirm_t	confirm;
	int			confirmTime;

	// reading the list
	int			listRequestTime;	// asked for it, waiting for the header, 0 = not waiting
	qboolean	listOurs;			// the list being read was asked for by the manager
	int			listLineTime;		// last list line read, 0 = not reading a list
	int			listTime;			// when a list was last read, 0 = never
	int			refreshTime;		// read the list again at this time, 0 = no

	// picker
	int			pickerView[NM_MAX_SKINS];
	int			numPickerView;
	nmList_t	pickerList;

	// preview popup
	qboolean	preview;
	char		previewKey[MAX_QPATH * 2];	// type and model the instance below was built for
	CGhoul2Info_v *previewG2;
	CGhoul2Info_v *previewHeld[2];			// what was copied into its slots 1 and 2, kept like cgame does
	qboolean	previewOwnModel;			// the type's own model, not one from the model box
	vec3_t		previewMins, previewMaxs;	// unscaled, from the .glm
	char		previewModel[MAX_QPATH];	// model/skin shown
	const char	*previewError;
	qboolean	previewSkinMissing;
	float		previewYaw;					// added by the wheel

	// picking on the map, with a free camera
	int			pickYaw;		// added to facing the camera
	vec3_t		camOrg, camAng;
	qboolean	held[MAX_KEYS];
	int			lastFrameTime;
	int			viewFrame;		// cls.framecount the camera last moved on
	float		pickCursorX, pickCursorY;	// where the cursor was, back when picking ends

	// the main view last frame: the cgame's, or the free camera's
	qboolean	haveView;
	vec3_t		viewOrg;
	matrix3_t	viewAxis;
	float		fovX, fovY;

	// messages
	char		message[MAX_STRING_CHARS];
	int			messageTime;
	char		lastCmd[MAX_STRING_CHARS];
	int			sentTime;
	char		reply[MAX_STRING_CHARS];
	int			replyTime;
} nm;

static vec4_t nmWhite		= { 1.0f, 1.0f, 1.0f, 1.0f };
static vec4_t nmRed			= { 1.0f, 0.3f, 0.3f, 1.0f };
static vec4_t nmPanel		= { 0.0f, 0.0f, 0.0f, 0.65f };
static vec4_t nmPanelOpaque	= { 0.05f, 0.05f, 0.05f, 0.95f };	// a box over the panel, which would show through
static vec4_t nmPanelLight	= { 0.15f, 0.15f, 0.15f, 0.8f };
static vec4_t nmPanelFocus	= { 0.1f, 0.2f, 0.3f, 0.9f };
static vec4_t nmHighlight	= { 0.2f, 0.45f, 0.8f, 0.8f };
static vec4_t nmHover		= { 1.0f, 1.0f, 1.0f, 0.12f };
static vec4_t nmHoverBox	= { 0.25f, 0.25f, 0.25f, 0.9f };
static vec4_t nmDanger		= { 0.6f, 0.15f, 0.15f, 0.9f };
static vec4_t nmBorder		= { 0.5f, 0.5f, 0.5f, 0.8f };
static vec4_t nmDim			= { 0.7f, 0.7f, 0.7f, 1.0f };
static vec4_t nmAccent		= { 1.0f, 0.8f, 0.3f, 1.0f };
static vec4_t nmPlacedColor	= { 0.4f, 1.0f, 0.5f, 1.0f };

static void NM_RequestList( void );

/*
===============================================================================

INDEX

===============================================================================
*/

static const char *NM_PoolString( const char *s ) {
	int len = strlen( s ) + 1;
	char *out;

	if ( nm.poolUsed + len > NM_POOL_SIZE )
		return NULL;
	out = nm.pool + nm.poolUsed;
	memcpy( out, s, len );
	nm.poolUsed += len;
	return out;
}

// "base/assets<digits>.pk3"; anything else people named assets_something isn't
static qboolean NM_IsBaseSource( const char *source ) {
	const int prefixLen = strlen( NM_BASE_PAKS );
	const char *c = source + prefixLen;

	if ( Q_stricmpn( source, NM_BASE_PAKS, prefixLen ) || !isdigit( (unsigned char)*c ) )
		return qfalse;
	while ( isdigit( (unsigned char)*c ) )
		c++;
	return (qboolean)!Q_stricmp( c, ".pk3" );
}

static int NM_SourceIndex( const char *source ) {
	const char *stored;

	for ( int i = 0; i < nm.numSources; i++ ) {
		if ( !Q_stricmp( nm.sources[i], source ) )
			return i;
	}
	if ( nm.numSources >= NM_MAX_SOURCES || !( stored = NM_PoolString( source ) ) )
		return 0;
	nm.sources[nm.numSources] = stored;
	return nm.numSources++;
}

// name is a full game path, e.g. "ext_data/npcs/jedi.npc"; the first one seen is the one in use
static void NM_AddNpcFile( const char *name, const char *source, void *ctx ) {
	const char *file = name + strlen( "ext_data/npcs/" );

	// the game only reads the folder itself
	if ( strchr( file, '/' ) || strchr( file, '\\' ) || nm.numFiles >= NM_MAX_FILES )
		return;
	for ( int i = 0; i < nm.numFiles; i++ ) {
		if ( !Q_stricmp( nm.files[i].file, file ) )
			return;
	}
	nm.files[nm.numFiles].file = NM_PoolString( file );
	nm.files[nm.numFiles].source = NM_SourceIndex( source );
	if ( nm.files[nm.numFiles].file )
		nm.numFiles++;
}

// 0, "unknown", if it wasn't seen
static int NM_FileSource( const char *file ) {
	for ( int i = 0; i < nm.numFiles; i++ ) {
		if ( !Q_stricmp( nm.files[i].file, file ) )
			return nm.files[i].source;
	}
	return 0;
}

// looks holds what NM_ParseNpcFile read besides the model
static void NM_AddType( const char *name, const char *model, const char *skin, const char *surfOff, const char *surfOn, const nmType_t *looks ) {
	char full[MAX_QPATH];
	nmType_t *t;

	// the game takes the first block with the name, in file list order
	for ( int i = 0; i < nm.numTypes; i++ ) {
		if ( !Q_stricmp( nm.types[i].name, name ) )
			return;
	}
	if ( nm.numTypes >= NM_MAX_TYPES )
		return;

	if ( model[0] && skin[0] && Q_stricmp( skin, "default" ) )
		Com_sprintf( full, sizeof( full ), "%s/%s", model, skin );
	else
		Q_strncpyz( full, model, sizeof( full ) );

	t = &nm.types[nm.numTypes];
	*t = *looks;
	t->name = NM_PoolString( name );
	t->model = NM_PoolString( full );
	t->surfOff = NM_PoolString( surfOff );
	t->surfOn = NM_PoolString( surfOn );
	if ( t->name && t->model && t->surfOff && t->surfOn )
		nm.numTypes++;
}

// adds to a comma separated list, the way NPC_ParseParms collects surfOff/surfOn
static void NM_AddToList( char *list, int size, const char *value ) {
	if ( list[0] )
		Q_strcat( list, size, "," );
	Q_strcat( list, size, value );
}

// reads every block out of one .npc file, the way NPC_ParseParms finds them
static void NM_ParseNpcFile( const char *path, int source ) {
	char *buf;
	const char *p, *token;
	char name[MAX_QPATH], key[64], model[MAX_QPATH], skin[MAX_QPATH];
	char surfOff[1024], surfOn[1024], weapon[64], saber[64], saber2[64];
	nmType_t looks;

	if ( FS_ReadFile( path, (void **)&buf ) < 0 || !buf )
		return;

	p = buf;
	COM_BeginParseSession( path );
	while ( 1 ) {
		token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			break;
		Q_strncpyz( name, token, sizeof( name ) );
		if ( Q_stricmp( COM_ParseExt( &p, qtrue ), "{" ) )
			break;

		model[0] = skin[0] = surfOff[0] = surfOn[0] = weapon[0] = saber[0] = saber2[0] = '\0';
		memset( &looks, 0, sizeof( looks ) );
		looks.rgba[0] = looks.rgba[1] = looks.rgba[2] = looks.rgba[3] = 255;
		looks.scale = 100;
		looks.source = source;
		looks.custom = (qboolean)!NM_IsBaseSource( nm.sources[source] );
		while ( 1 ) {
			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] || !Q_stricmp( token, "}" ) )
				break;
			if ( !Q_stricmp( token, "{" ) ) {
				SkipBracedSection( &p, 1 );
				continue;
			}
			Q_strncpyz( key, token, sizeof( key ) );
			token = COM_ParseExt( &p, qfalse );
			if ( !Q_stricmp( key, "playerModel" ) )
				Q_strncpyz( model, token, sizeof( model ) );
			else if ( !Q_stricmp( key, "customSkin" ) )
				Q_strncpyz( skin, token, sizeof( skin ) );
			else if ( !Q_stricmp( key, "class" ) )
				looks.vehicle = (qboolean)!Q_stricmp( token, "CLASS_VEHICLE" );
			else if ( !Q_stricmp( key, "surfOff" ) )
				NM_AddToList( surfOff, sizeof( surfOff ), token );
			else if ( !Q_stricmp( key, "surfOn" ) )
				NM_AddToList( surfOn, sizeof( surfOn ), token );
			else if ( !Q_stricmp( key, "weapon" ) )
				Q_strncpyz( weapon, token, sizeof( weapon ) );
			else if ( !Q_stricmp( key, "saber" ) )
				Q_strncpyz( saber, token, sizeof( saber ) );
			else if ( !Q_stricmp( key, "saber2" ) )
				Q_strncpyz( saber2, token, sizeof( saber2 ) );
			else if ( !Q_stricmp( key, "scale" ) ) {
				if ( atoi( token ) > 0 )
					looks.scale = atoi( token );
			} else if ( !Q_stricmp( key, "customRGBA" ) && ( isdigit( (unsigned char)token[0] ) ) ) {
				// "r g b [a]"; the named and random ones differ per spawn, so stay white
				looks.rgba[0] = (byte)Com_Clampi( 0, 255, atoi( token ) );
				for ( int i = 1; i < 4; i++ ) {
					token = COM_ParseExt( &p, qfalse );
					if ( !token[0] )
						break;
					looks.rgba[i] = (byte)Com_Clampi( 0, 255, atoi( token ) );
				}
			}
			SkipRestOfLine( &p );
		}
		looks.weapon = NM_PoolString( weapon );
		looks.saber = NM_PoolString( saber );
		looks.saber2 = NM_PoolString( saber2 );
		if ( looks.weapon && looks.saber && looks.saber2 )
			NM_AddType( name, model, skin, surfOff, surfOn, &looks );
	}

	FS_FreeFile( buf );
}

// name is a full game path, e.g. "models/players/kyle/model_default.skin"
static void NM_AddSkin( const char *name, void *ctx ) {
	static const char prefix[] = "models/players/";
	char path[MAX_QPATH], out[MAX_QPATH];
	char *model, *file, *variant, *ext;

	if ( nm.numSkins >= NM_MAX_SKINS || Q_stricmpn( name, prefix, sizeof( prefix ) - 1 ) )
		return;
	Q_strncpyz( path, name + sizeof( prefix ) - 1, sizeof( path ) );
	for ( char *c = path; *c; c++ ) {
		if ( *c == '\\' )
			*c = '/';
	}

	// only <model>/model_<variant>.skin, the ones the game can put on a player
	model = path;
	file = strchr( path, '/' );
	if ( !file || strchr( file + 1, '/' ) )
		return;
	*file++ = '\0';
	if ( Q_stricmpn( file, "model_", 6 ) )
		return;
	variant = file + 6;
	ext = strrchr( variant, '.' );
	if ( ext )
		*ext = '\0';
	if ( !variant[0] )
		return;

	if ( !Q_stricmp( variant, "default" ) )
		Q_strncpyz( out, model, sizeof( out ) );
	else
		Com_sprintf( out, sizeof( out ), "%s/%s", model, variant );
	nm.skins[nm.numSkins] = NM_PoolString( out );
	if ( nm.skins[nm.numSkins] )
		nm.numSkins++;
}

static int QDECL NM_CompareTypes( const void *a, const void *b ) {
	return Q_stricmp( ( (const nmType_t *)a )->name, ( (const nmType_t *)b )->name );
}

static int QDECL NM_CompareStrings( const void *a, const void *b ) {
	return Q_stricmp( *(const char **)a, *(const char **)b );
}

static void NM_BuildIndex( void ) {
	int start = Sys_Milliseconds(), numFiles, out;
	char **files;

	nm.poolUsed = 0;
	nm.numTypes = 0;
	nm.numSkins = 0;
	nm.numSources = 0;
	nm.numFiles = 0;
	NM_SourceIndex( "unknown" );	// 0

	// where each file is read from; the game's own list sets the order the types are taken in
	FS_ListFilesRecursiveFrom( "ext_data/npcs", ".npc", NM_AddNpcFile, NULL );
	files = FS_ListFiles( "ext_data/npcs", ".npc", &numFiles );
	for ( int i = 0; i < numFiles; i++ )
		NM_ParseNpcFile( va( "ext_data/npcs/%s", files[i] ), NM_FileSource( files[i] ) );
	FS_FreeFileList( files );
	qsort( nm.types, nm.numTypes, sizeof( nm.types[0] ), NM_CompareTypes );

	// the same skin can be in several pk3s and folders
	FS_ListFilesRecursive( "models/players", ".skin", NM_AddSkin, NULL );
	qsort( nm.skins, nm.numSkins, sizeof( nm.skins[0] ), NM_CompareStrings );
	for ( int i = out = 0; i < nm.numSkins; i++ ) {
		if ( !out || Q_stricmp( nm.skins[out - 1], nm.skins[i] ) )
			nm.skins[out++] = nm.skins[i];
	}
	nm.numSkins = out;

	nm.indexed = qtrue;
	nm.typeList.sel = nm.typeList.scroll = 0;
	out = 0;
	for ( int i = 0; i < nm.numTypes; i++ ) {
		if ( nm.types[i].custom )
			out++;
	}
	Com_Printf( "NPC manager: indexed %i NPC types, %i of them custom, and %i player models (%i ms)\n",
		nm.numTypes, out, nm.numSkins, Sys_Milliseconds() - start );
}

/*
===============================================================================

HELPERS

===============================================================================
*/

static qboolean NM_ContainsNoCase( const char *haystack, const char *needle ) {
	int nlen = strlen( needle );

	if ( !nlen )
		return qtrue;
	for ( ; *haystack; haystack++ ) {
		if ( !Q_stricmpn( haystack, needle, nlen ) )
			return qtrue;
	}
	return qfalse;
}

static const char *NM_PlayerName( int clientNum ) {
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

static void NM_Message( const char *text ) {
	Q_strncpyz( nm.message, text, sizeof( nm.message ) );
	nm.messageTime = cls.realtime;
}

// the type with the name; one only the server may know, as saved, stands in for a missing one
static const nmType_t *NM_FindType( const char *name, qboolean vehicle ) {
	static nmType_t stub;
	static char stubName[MAX_QPATH];

	for ( int i = 0; i < nm.numTypes; i++ ) {
		if ( !Q_stricmp( nm.types[i].name, name ) )
			return &nm.types[i];
	}
	Q_strncpyz( stubName, name, sizeof( stubName ) );
	memset( &stub, 0, sizeof( stub ) );
	stub.name = stubName;
	stub.model = stub.surfOff = stub.surfOn = stub.weapon = stub.saber = stub.saber2 = "";
	stub.vehicle = vehicle;
	stub.rgba[0] = stub.rgba[1] = stub.rgba[2] = stub.rgba[3] = 255;
	stub.scale = 100;
	stub.custom = qtrue;
	return &stub;
}

// the saved NPC or construct on the selected row, NULL if it's a type
static const nmSaved_t *NM_SelectedSaved( void ) {
	int v;

	if ( nm.typeList.sel < 0 || nm.typeList.sel >= nm.numTypeView )
		return NULL;
	v = nm.typeView[nm.typeList.sel];
	return v < 0 ? &nm.saved[-1 - v] : NULL;
}

// a saved NPC's type for one; NULL on a construct
static const nmType_t *NM_SelectedType( void ) {
	const nmSaved_t *saved = NM_SelectedSaved();

	if ( nm.typeList.sel < 0 || nm.typeList.sel >= nm.numTypeView )
		return NULL;
	if ( !saved )
		return &nm.types[nm.typeView[nm.typeList.sel]];
	return saved->construct ? NULL : NM_FindType( saved->npc.type, saved->npc.vehicle );
}

static const nmNpc_t *NM_FindNpc( int num ) {
	for ( int i = 0; i < nm.numNpcs; i++ ) {
		if ( nm.npcs[i].num == num )
			return &nm.npcs[i];
	}
	return NULL;
}

static const nmNpc_t *NM_SelectedNpc( void ) {
	return nm.selNum >= 0 ? NM_FindNpc( nm.selNum ) : NULL;
}

static int NM_CountNamed( const char *name ) {
	int count = 0;

	for ( int i = 0; i < nm.numNpcs; i++ ) {
		if ( !Q_stricmp( nm.npcs[i].name, name ) )
			count++;
	}
	return count;
}

// where the NPC is now if it's in the snapshot, else where the list saw it
static void NM_NpcOrigin( const nmNpc_t *npc, vec3_t out ) {
	for ( int i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
		if ( es->number == npc->num ) {
			VectorCopy( es->pos.trBase, out );
			return;
		}
	}
	VectorCopy( npc->origin, out );
}

static int NM_NormalizeYaw( float a ) {
	int i = (int)floorf( a + 0.5f ) % 360;
	return i < 0 ? i + 360 : i;
}

// yaw for something at pos to face the camera
static int NM_YawToView( const vec3_t pos ) {
	vec3_t dir, angles;

	VectorSubtract( nm.haveView ? nm.viewOrg : cl.snap.ps.origin, pos, dir );
	dir[2] = 0;
	vectoangles( dir, angles );
	return NM_NormalizeYaw( angles[YAW] );
}

static void NM_InFrontOfPlayer( vec3_t pos, int *yaw ) {
	vec3_t angles, forward;

	VectorSet( angles, 0, cl.viewangles[YAW], 0 );
	AngleVectors( angles, forward, NULL, NULL );
	VectorMA( cl.snap.ps.origin, NM_FRONT_DIST, forward, pos );
	*yaw = NM_NormalizeYaw( cl.viewangles[YAW] + 180.0f );
}

static void NM_ListClamp( nmList_t *list, int count, int visible ) {
	list->sel = Com_Clampi( 0, count ? count - 1 : 0, list->sel );
	list->scroll = Com_Clampi( 0, count > visible ? count - visible : 0, list->scroll );
}

static void NM_ListMove( nmList_t *list, int count, int visible, int delta ) {
	if ( !count )
		return;
	list->sel = Com_Clampi( 0, count - 1, list->sel + delta );
	if ( list->sel < list->scroll )
		list->scroll = list->sel;
	else if ( list->sel >= list->scroll + visible )
		list->scroll = list->sel - visible + 1;
}

// the field typing goes to
static nmField_t NM_ActiveField( void ) {
	if ( nm.picker != NM_PICKER_NONE )
		return NM_F_PICKER_SEARCH;
	if ( nm.saving != NM_SAVING_NONE )
		return NM_F_SAVE_NAME;
	if ( nm.teleBox )
		return nm.focus >= NM_F_TELE_X && nm.focus <= NM_F_TELE_YAW ? nm.focus : NM_F_TELE_X;
	if ( nm.focus != NM_F_NONE )
		return nm.focus;
	return nm.tab == NM_TAB_SPAWN ? NM_F_TYPE_SEARCH : NM_F_NPC_SEARCH;
}

/*
===============================================================================

VIEWS

===============================================================================
*/

static qboolean NM_ShowPasses( qboolean vehicle ) {
	return (qboolean)!( ( nm.show == NM_SHOW_CHARACTERS && vehicle ) || ( nm.show == NM_SHOW_VEHICLES && !vehicle ) );
}

// what was saved first, then the types the filters let through
static void NM_RebuildTypeView( void ) {
	const char *search = nm.fields[NM_F_TYPE_SEARCH];

	nm.numTypeView = 0;
	if ( nm.source == NM_SRC_ALL || nm.source == NM_SRC_SAVED ) {
		for ( int i = 0; i < nm.numSaved; i++ ) {
			const nmSaved_t *s = &nm.saved[i];

			// a construct can hold both kinds
			if ( !s->construct && !NM_ShowPasses( s->npc.vehicle ) )
				continue;
			if ( search[0] && !NM_ContainsNoCase( s->name, search ) && ( s->construct || !NM_ContainsNoCase( s->npc.type, search ) ) )
				continue;
			nm.typeView[nm.numTypeView++] = -1 - i;
		}
	}
	for ( int i = 0; i < nm.numTypes && nm.source != NM_SRC_SAVED; i++ ) {
		const nmType_t *t = &nm.types[i];

		if ( !NM_ShowPasses( t->vehicle ) || ( nm.source == NM_SRC_BASE && t->custom ) || ( nm.source == NM_SRC_CUSTOM && !t->custom ) )
			continue;
		if ( search[0] && !NM_ContainsNoCase( t->name, search ) && !NM_ContainsNoCase( t->model, search ) )
			continue;
		nm.typeView[nm.numTypeView++] = i;
	}
	NM_ListClamp( &nm.typeList, nm.numTypeView, (int)( NM_TYPE_LIST_H / NM_ROW_H ) );
}

static void NM_RebuildNpcView( void ) {
	const char *search = nm.fields[NM_F_NPC_SEARCH];

	nm.numNpcView = 0;
	for ( int i = 0; i < nm.numNpcs; i++ ) {
		const nmNpc_t *n = &nm.npcs[i];

		if ( search[0] && !NM_ContainsNoCase( n->type, search ) && !NM_ContainsNoCase( n->name, search )
			&& !NM_ContainsNoCase( va( "%i", n->num ), search ) )
			continue;
		if ( n->num == nm.selNum )
			nm.npcList.sel = nm.numNpcView;
		nm.npcView[nm.numNpcView++] = i;
	}
	NM_ListClamp( &nm.npcList, nm.numNpcView, (int)( NM_LIST_H / NM_ROW_H ) );
}

static int NM_PickerCount( void ) {
	switch ( nm.picker ) {
	case NM_PICKER_MODEL:	return nm.numSkins;
	case NM_PICKER_EMOTE:	return ARRAY_LEN( nmEmotes );
	case NM_PICKER_PLAYER:	return MAX_CLIENTS;
	default:				return 0;
	}
}

static const char *NM_PickerItem( int index ) {
	switch ( nm.picker ) {
	case NM_PICKER_MODEL:	return nm.skins[index];
	case NM_PICKER_EMOTE:	return nmEmotes[index];
	case NM_PICKER_PLAYER:	return NM_PlayerName( index );
	default:				return "";
	}
}

static void NM_RebuildPickerView( void ) {
	const char *search = nm.fields[NM_F_PICKER_SEARCH];
	int count = NM_PickerCount();

	nm.numPickerView = 0;
	for ( int i = 0; i < count; i++ ) {
		const char *item = NM_PickerItem( i );

		if ( item[0] && NM_ContainsNoCase( item, search ) )
			nm.pickerView[nm.numPickerView++] = i;
	}
	NM_ListClamp( &nm.pickerList, nm.numPickerView, (int)( ( NM_PICKER_H - 74 ) / NM_ROW_H ) );
}

/*
===============================================================================

THE NPC LIST

===============================================================================
*/

// "776: (-1934 -5523  104,   6) my57, named my-57", colors already stripped
static qboolean NM_ParseNpcLine( const char *line ) {
	nmNpc_t *n;
	float x, y, z, yaw;
	int num, used = 0;
	const char *rest, *named;

	if ( sscanf( line, "%d: (%f %f %f, %f)%n", &num, &x, &y, &z, &yaw, &used ) != 5 || !used )
		return qfalse;
	if ( nm.numNpcs >= NM_MAX_NPCS )
		return qtrue;

	n = &nm.npcs[nm.numNpcs];
	n->num = num;
	VectorSet( n->origin, x, y, z );
	n->yaw = NM_NormalizeYaw( yaw );

	rest = line + used;
	while ( *rest == ' ' )
		rest++;
	named = strstr( rest, ", named " );
	if ( named ) {
		Q_strncpyz( n->type, rest, Com_Clampi( 1, sizeof( n->type ), (int)( named - rest ) + 1 ) );
		Q_strncpyz( n->name, named + 8, sizeof( n->name ) );
	} else {
		Q_strncpyz( n->type, rest, sizeof( n->type ) );
		n->name[0] = '\0';
	}
	nm.numNpcs++;
	return qtrue;
}

// every server print goes through here on its way to the cgame; qtrue keeps it out of the console
qboolean CL_NpcManager_ServerPrint( const char *text ) {
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

		if ( !Q_stricmpn( line, NM_LIST_HEADER, strlen( NM_LIST_HEADER ) ) ) {
			// also read a list someone typed /npc list for, just don't hide it
			nm.listOurs = (qboolean)( nm.listRequestTime && cls.realtime - nm.listRequestTime < NM_LIST_TIMEOUT );
			nm.listRequestTime = 0;
			nm.listLineTime = nm.listTime = cls.realtime;
			nm.numNpcs = 0;
			if ( !nm.listOurs )
				swallow = qfalse;
		} else if ( nm.listLineTime && cls.realtime - nm.listLineTime < NM_LIST_LINE_GAP && NM_ParseNpcLine( line ) ) {
			nm.listLineTime = cls.realtime;
			if ( !nm.listOurs )
				swallow = qfalse;
		} else {
			swallow = qfalse;
			if ( nm.open && cls.realtime - nm.sentTime < NM_REPLY_MS ) {
				Q_strncpyz( nm.reply, line, sizeof( nm.reply ) );
				nm.replyTime = cls.realtime;
			}
		}
	}

	if ( nm.open )
		NM_RebuildNpcView();
	return (qboolean)( any && swallow );
}

/*
===============================================================================

COMMANDS

===============================================================================
*/

// ms the server's flood protection wants between commands
static int NM_CommandGap( void ) {
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

static void NM_Send( const char *cmd, qboolean echo ) {
	CL_AddReliableCommand( cmd, qfalse );
	// the queue waits for this one too
	nm.nextSend = cls.realtime + NM_CommandGap();
	if ( echo ) {
		// the list asked for behind the user's back has no reply to show
		nm.sentTime = cls.realtime;
		Q_strncpyz( nm.lastCmd, cmd, sizeof( nm.lastCmd ) );
		Com_Printf( S_COLOR_GREEN "NPC manager: " S_COLOR_WHITE "%s\n", cmd );
	}
}

static void NM_RequestList( void ) {
	// it would take a spawn's place; read it once they're all out
	if ( nm.numQueue ) {
		nm.refreshTime = cls.realtime;
		return;
	}
	nm.listRequestTime = cls.realtime;
	nm.refreshTime = 0;
	NM_Send( "npc list", qfalse );
}

// the list again once what was just sent has had time to happen, and the server takes commands
static void NM_RefreshSoon( void ) {
	nm.refreshTime = Q_max( cls.realtime + NM_REFRESH_DELAY, nm.nextSend );
}

static void NM_Queue( const char *cmd ) {
	if ( nm.numQueue >= NM_MAX_QUEUE ) {
		Com_Printf( S_COLOR_YELLOW "NPC manager: too many spawns waiting, dropped %s\n", cmd );
		return;
	}
	Q_strncpyz( nm.queue[nm.numQueue++], cmd, sizeof( nm.queue[0] ) );
	nm.queueTotal++;
}

static void NM_RunQueue( void ) {
	if ( !nm.numQueue ) {
		nm.queueTotal = 0;
		return;
	}
	if ( cls.realtime < nm.nextSend || cls.state != CA_ACTIVE )
		return;
	NM_Send( nm.queue[0], qtrue );
	nm.numQueue--;
	memmove( nm.queue[0], nm.queue[1], nm.numQueue * sizeof( nm.queue[0] ) );
	if ( !nm.numQueue )
		NM_RefreshSoon();
}

// the NPC list for the other tools, read quietly
void CL_NpcManager_RequestList( void ) {
	if ( cls.state == CA_ACTIVE )
		NM_RequestList();
}

qboolean CL_NpcManager_Waiting( void ) {
	return (qboolean)( nm.listRequestTime && cls.realtime - nm.listRequestTime < NM_LIST_TIMEOUT );
}

int CL_NpcManager_NumNpcs( void ) {
	return nm.numNpcs;
}

qboolean CL_NpcManager_GetNpc( int index, int *num, const char **type, const char **name ) {
	if ( index < 0 || index >= nm.numNpcs )
		return qfalse;
	*num = nm.npcs[index].num;
	*type = nm.npcs[index].type;
	*name = nm.npcs[index].name;
	return qtrue;
}

// "npc spawn" for one NPC; at its pos and yaw with atPos, else in front of the player
static const char *NM_SpawnCommand( const nmSpawn_t *s, qboolean atPos ) {
	static char cmd[MAX_STRING_CHARS];
	const nmType_t *t = NM_FindType( s->type, s->vehicle );
	qboolean coords = (qboolean)( atPos || s->scale[0] );
	int need, yaw;
	vec3_t pos;

	// the arguments are positional, so everything before the last one given has to be there
	if ( coords )
		need = 3;
	else if ( s->model[0] )
		need = 3;
	else if ( s->team != NM_TEAM_DEFAULT )
		need = 2;
	else if ( s->targetname[0] )
		need = 1;
	else
		need = 0;

	Com_sprintf( cmd, sizeof( cmd ), "npc spawn %s%s", s->vehicle ? "vehicle " : "", s->type );
	if ( need >= 1 )
		Q_strcat( cmd, sizeof( cmd ), va( " %s", s->targetname[0] ? s->targetname : s->type ) );
	if ( need >= 2 )
		Q_strcat( cmd, sizeof( cmd ), va( " %s", nmTeamArgs[s->team] ) );
	if ( need >= 3 ) {
		// no custom model: the NPC's own, so it looks the way it would without one
		Q_strcat( cmd, sizeof( cmd ), va( " %s", s->model[0] ? s->model : t->model[0] ? t->model : s->type ) );
	}
	if ( coords ) {
		if ( atPos ) {
			VectorCopy( s->pos, pos );
			yaw = s->yaw;
		} else {
			NM_InFrontOfPlayer( pos, &yaw );
		}
		Q_strcat( cmd, sizeof( cmd ), va( " %i %i %i %i", (int)floorf( pos[0] + 0.5f ), (int)floorf( pos[1] + 0.5f ), (int)floorf( pos[2] + 0.5f ), yaw ) );
	}
	if ( s->scale[0] )
		Q_strcat( cmd, sizeof( cmd ), va( " %s", s->scale ) );
	return cmd;
}

// the NPC the form describes, at the spot picked for it
static qboolean NM_FormSpawn( nmSpawn_t *out ) {
	const nmType_t *t = NM_SelectedType();

	memset( out, 0, sizeof( *out ) );
	if ( !t )
		return qfalse;
	Q_strncpyz( out->type, t->name, sizeof( out->type ) );
	out->vehicle = t->vehicle;
	Q_strncpyz( out->targetname, nm.fields[NM_F_TARGETNAME], sizeof( out->targetname ) );
	out->team = nm.team;
	Q_strncpyz( out->model, nm.fields[NM_F_MODEL], sizeof( out->model ) );
	Q_strncpyz( out->scale, nm.fields[NM_F_SCALE], sizeof( out->scale ) );
	VectorCopy( nm.pos, out->pos );
	out->yaw = nm.yaw;
	return qtrue;
}

static const char *NM_BuildSpawn( void ) {
	nmSpawn_t s;

	if ( !NM_FormSpawn( &s ) )
		return NULL;
	return NM_SpawnCommand( &s, nm.havePos );
}

static void NM_Spawn( void ) {
	const char *cmd = NM_BuildSpawn();

	if ( !cmd )
		return;
	NM_Queue( cmd );
	NM_RunQueue();
}

static void NM_SpawnPlaced( void ) {
	if ( !nm.numPlaced )
		return;
	for ( int i = 0; i < nm.numPlaced; i++ )
		NM_Queue( NM_SpawnCommand( &nm.placed[i], qtrue ) );
	NM_Message( va( S_COLOR_GREEN "Spawning %i NPCs, about one a second", nm.numPlaced ) );
	nm.numPlaced = nm.placedScroll = 0;
	NM_RunQueue();
}

/*
===============================================================================

PREVIEW

===============================================================================
*/

static void NM_PreviewFree( void ) {
	if ( nm.previewG2 )
		re->G2API_CleanGhoul2Models( &nm.previewG2 );
	nm.previewG2 = NULL;
	for ( int i = 0; i < 2; i++ ) {
		if ( nm.previewHeld[i] )
			re->G2API_CleanGhoul2Models( &nm.previewHeld[i] );
		nm.previewHeld[i] = NULL;
	}
	nm.previewKey[0] = '\0';
}

// the model is freed when the next frame starts: the popup's buttons close it
// mid-frame, after its scene went to the renderer still pointing at the model
static void NM_ClosePreview( void ) {
	nm.preview = qfalse;
}

// the scale box, or the type's own scale, in percent. The .npc scale is a
// percentage; small numbers in the box are taken as a multiplier (1.5 = 150%)
static int NM_PreviewScale( const nmType_t *t ) {
	const char *s = nm.fields[NM_F_SCALE];
	float f = (float)atof( s );

	if ( !s[0] || f <= 0.0f )
		return t->scale;
	return (int)( f <= 10.0f ? f * 100.0f + 0.5f : f );
}

// the bind pose bounds of a .glm's first LOD, or why it can't be shown
static const char *NM_GlmBounds( const char *path, vec3_t mins, vec3_t maxs ) {
	byte *buf;
	const mdxmHeader_t *header;
	const mdxmLODSurfOffset_t *indexes;
	const char *error = NULL;
	int len, numSurfaces, ofsIndexes;

	len = FS_ReadFile( path, (void **)&buf );
	if ( len <= 0 || !buf )
		return "Model not found";

	ClearBounds( mins, maxs );
	header = (const mdxmHeader_t *)buf;
	numSurfaces = len >= (int)sizeof( *header ) ? LittleLong( header->numSurfaces ) : 0;
	ofsIndexes = len >= (int)sizeof( *header ) ? LittleLong( header->ofsLODs ) + (int)sizeof( mdxmLOD_t ) : 0;
	if ( len < (int)sizeof( *header ) || LittleLong( header->ident ) != MDXM_IDENT || LittleLong( header->numLODs ) < 1
		|| numSurfaces < 1 || numSurfaces > len / 4 || ofsIndexes < (int)sizeof( *header ) || ofsIndexes > len - numSurfaces * 4 ) {
		error = "Not a Ghoul2 model";
	} else {
		indexes = (const mdxmLODSurfOffset_t *)( buf + ofsIndexes );
		for ( int i = 0; i < numSurfaces && !error; i++ ) {
			const mdxmSurface_t *surf;
			const mdxmVertex_t *verts;
			int ofsSurf = ofsIndexes + LittleLong( indexes->offsets[i] ), numVerts, ofsVerts;

			if ( ofsSurf < 0 || ofsSurf > len - (int)sizeof( mdxmSurface_t ) ) {
				error = "Damaged model file";
				break;
			}
			surf = (const mdxmSurface_t *)( buf + ofsSurf );
			numVerts = LittleLong( surf->numVerts );
			ofsVerts = ofsSurf + LittleLong( surf->ofsVerts );
			if ( numVerts < 0 || numVerts > len / (int)sizeof( mdxmVertex_t ) || ofsVerts < 0 || ofsVerts > len - numVerts * (int)sizeof( mdxmVertex_t ) ) {
				error = "Damaged model file";
				break;
			}
			verts = (const mdxmVertex_t *)( buf + ofsVerts );
			for ( int j = 0; j < numVerts; j++ ) {
				vec3_t p;

				for ( int k = 0; k < 3; k++ )
					p[k] = LittleFloat( verts[j].vertCoords[k] );
				AddPointToBounds( p, mins, maxs );
			}
		}
		if ( !error && mins[0] > maxs[0] )
			error = "Model has no vertices";
	}

	FS_FreeFile( buf );
	return error;
}

// turns a comma separated list of surfaces on or off
static void NM_PreviewSurfaces( const char *list, int flags ) {
	char name[MAX_QPATH];
	const char *p = list;

	while ( *p ) {
		int len = 0;

		while ( *p == ',' || *p == ' ' || *p == '\t' )
			p++;
		while ( *p && *p != ',' ) {
			if ( len < (int)sizeof( name ) - 1 )
				name[len++] = *p;
			p++;
		}
		while ( len && ( name[len - 1] == ' ' || name[len - 1] == '\t' ) )
			len--;
		name[len] = '\0';
		if ( name[0] )
			re->G2API_SetSurfaceOnOff( *nm.previewG2, name, flags );
	}
}

// loops BOTH_STAND1 from the animation.cfg next to the model's .gla, if it has one
static void NM_PreviewAnimate( void ) {
	char path[MAX_QPATH], *buf, *slash;
	const char *gla = re->G2API_GetGLAName( *nm.previewG2, 0 ), *p, *token;

	if ( !gla || !gla[0] )
		return;
	Q_strncpyz( path, gla, sizeof( path ) );
	slash = strrchr( path, '/' );
	if ( !slash )
		return;
	slash[1] = '\0';
	Q_strcat( path, sizeof( path ), "animation.cfg" );
	if ( FS_ReadFile( path, (void **)&buf ) <= 0 || !buf )
		return;

	p = buf;
	COM_BeginParseSession( path );
	while ( 1 ) {
		int first, num;
		float fps;

		token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			break;
		if ( Q_stricmp( token, "BOTH_STAND1" ) ) {
			SkipRestOfLine( &p );
			continue;
		}
		first = atoi( COM_ParseExt( &p, qfalse ) );
		num = atoi( COM_ParseExt( &p, qfalse ) );
		COM_ParseExt( &p, qfalse );	// loop frames
		fps = fabsf( (float)atof( COM_ParseExt( &p, qfalse ) ) );
		// the same speed cgame plays it at: 50 / ( 1000 / fps )
		if ( num > 0 && fps > 0.0f ) {
			re->G2API_SetBoneAnim( *nm.previewG2, 0, "model_root", first, first + num, BONE_ANIM_OVERRIDE_LOOP,
				fps / 20.0f, re->G2API_GetTime( cl.serverTime ), -1, -1 );
		}
		break;
	}
	FS_FreeFile( buf );
}

// what NPCs hold, as bg_misc.c's items give cgame its weapon models
static const struct {
	const char	*weapon, *model;
} nmWeaponModels[] = {
	{ "WP_STUN_BATON",		"models/weapons2/stun_baton/baton_w.glm" },
	{ "WP_BRYAR_PISTOL",	"models/weapons2/blaster_pistol/blaster_pistol_w.glm" },
	{ "WP_CONCUSSION",		"models/weapons2/concussion/c_rifle_w.glm" },
	{ "WP_BRYAR_OLD",		"models/weapons2/briar_pistol/briar_pistol_w.glm" },
	{ "WP_BLASTER",			"models/weapons2/blaster_r/blaster_w.glm" },
	{ "WP_DISRUPTOR",		"models/weapons2/disruptor/disruptor_w.glm" },
	{ "WP_BOWCASTER",		"models/weapons2/bowcaster/bowcaster_w.glm" },
	{ "WP_REPEATER",		"models/weapons2/heavy_repeater/heavy_repeater_w.glm" },
	{ "WP_DEMP2",			"models/weapons2/demp2/demp2_w.glm" },
	{ "WP_FLECHETTE",		"models/weapons2/golan_arms/golan_arms_w.glm" },
	{ "WP_ROCKET_LAUNCHER",	"models/weapons2/merr_sonn/merr_sonn_w.glm" },
	{ "WP_THERMAL",			"models/weapons2/thermal/thermal_w.glm" },
	{ "WP_TRIP_MINE",		"models/weapons2/laser_trap/laser_trap_w.glm" },
	{ "WP_DET_PACK",		"models/weapons2/detpack/det_pack_proj.glm" },
};

// a saber's hilt, from the first block with its name in ext_data/sabers/*.sab
static void NM_SaberModel( const char *saber, char *out, int size ) {
	char **files, *buf, name[MAX_QPATH], key[64];
	const char *p, *token;
	int numFiles;
	qboolean found = qfalse;

	Q_strncpyz( out, "models/weapons2/saber/saber_w.glm", size );	// DEFAULT_SABER_MODEL
	files = FS_ListFiles( "ext_data/sabers", ".sab", &numFiles );
	for ( int i = 0; i < numFiles && !found; i++ ) {
		const char *path = va( "ext_data/sabers/%s", files[i] );

		if ( FS_ReadFile( path, (void **)&buf ) <= 0 || !buf )
			continue;
		p = buf;
		COM_BeginParseSession( path );
		while ( !found ) {
			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] )
				break;
			Q_strncpyz( name, token, sizeof( name ) );
			if ( Q_stricmp( COM_ParseExt( &p, qtrue ), "{" ) )
				break;
			if ( Q_stricmp( name, saber ) ) {
				SkipBracedSection( &p, 1 );
				continue;
			}
			found = qtrue;
			while ( 1 ) {
				token = COM_ParseExt( &p, qtrue );
				if ( !token[0] || !Q_stricmp( token, "}" ) )
					break;
				Q_strncpyz( key, token, sizeof( key ) );
				token = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( key, "saberModel" ) && token[0] )
					Q_strncpyz( out, token, size );
				SkipRestOfLine( &p );
			}
		}
		FS_FreeFile( buf );
	}
	FS_FreeFileList( files );
}

// puts a weapon model in the preview's slot 1 or 2, held by the given hand bolt
static void NM_PreviewHold( const char *model, int slot, int bolt ) {
	CGhoul2Info_v **weapon = &nm.previewHeld[slot - 1];

	if ( bolt < 0 || FS_ReadFile( model, NULL ) <= 0 )
		return;
	if ( re->G2API_InitGhoul2Model( weapon, model, 0, 0, 0, 0, 0 ) >= 0 && *weapon ) {
		re->G2API_SetBoltInfo( **weapon, 0, bolt );	// bolt on model 0, the body
		re->G2API_CopySpecificG2Model( **weapon, 0, *nm.previewG2, slot );
	}
}

// the .npc's weapon, or its sabers, in the NPC's hands
static void NM_PreviewWeapons( const nmType_t *t ) {
	char saber[MAX_QPATH];

	if ( !Q_stricmp( t->weapon, "WP_SABER" ) ) {
		NM_SaberModel( t->saber[0] ? t->saber : "Kyle", saber, sizeof( saber ) );	// DEFAULT_SABER
		NM_PreviewHold( saber, 1, re->G2API_AddBolt( *nm.previewG2, 0, "*r_hand" ) );
		if ( t->saber2[0] ) {
			NM_SaberModel( t->saber2, saber, sizeof( saber ) );
			NM_PreviewHold( saber, 2, re->G2API_AddBolt( *nm.previewG2, 0, "*l_hand" ) );
		}
		return;
	}
	for ( size_t i = 0; i < ARRAY_LEN( nmWeaponModels ); i++ ) {
		if ( !Q_stricmp( t->weapon, nmWeaponModels[i].weapon ) ) {
			NM_PreviewHold( nmWeaponModels[i].model, 1, re->G2API_AddBolt( *nm.previewG2, 0, "*r_hand" ) );
			return;
		}
	}
}

// (re)builds the model when the type or the model box changed
// t's model as the spawn would send it, or pick, a model/skin from the model picker
static void NM_PreviewUpdate( const nmType_t *t, const char *pick ) {
	char key[sizeof( nm.previewKey )], name[MAX_QPATH], glm[MAX_QPATH], skinPath[MAX_QPATH];
	const char *model = nm.fields[NM_F_MODEL], *skin;
	char *slash;
	qhandle_t hSkin;

	if ( pick ) {
		model = pick;
		nm.previewOwnModel = qfalse;
	} else if ( !t ) {
		NM_PreviewFree();
		nm.previewError = "Pick an NPC type on the left";
		return;
	} else {
		// what the spawn sends: vehicles always use their own
		nm.previewOwnModel = (qboolean)( t->vehicle || !model[0] );
		if ( nm.previewOwnModel )
			model = t->model[0] ? t->model : t->name;
	}
	Com_sprintf( key, sizeof( key ), "%s|%s", t ? t->name : "", model );
	if ( !Q_stricmp( key, nm.previewKey ) )
		return;

	NM_PreviewFree();
	Q_strncpyz( nm.previewKey, key, sizeof( nm.previewKey ) );
	Q_strncpyz( nm.previewModel, model, sizeof( nm.previewModel ) );
	nm.previewError = NULL;
	nm.previewSkinMissing = qfalse;

	// "model" or "model/skin"
	Q_strncpyz( name, model, sizeof( name ) );
	slash = strchr( name, '/' );
	skin = "default";
	if ( slash ) {
		*slash = '\0';
		if ( slash[1] )
			skin = slash + 1;
	}

	Com_sprintf( glm, sizeof( glm ), "models/players/%s/model.glm", name );
	nm.previewError = NM_GlmBounds( glm, nm.previewMins, nm.previewMaxs );
	if ( nm.previewError )
		return;

	if ( strchr( skin, '|' ) ) {
		Com_sprintf( skinPath, sizeof( skinPath ), "models/players/%s/|%s", name, skin );
	} else {
		Com_sprintf( skinPath, sizeof( skinPath ), "models/players/%s/model_%s.skin", name, skin );
		if ( FS_ReadFile( skinPath, NULL ) <= 0 ) {
			nm.previewSkinMissing = qtrue;
			Com_sprintf( skinPath, sizeof( skinPath ), "models/players/%s/model_default.skin", name );
		}
	}
	hSkin = re->RegisterSkin( skinPath );

	if ( re->G2API_InitGhoul2Model( &nm.previewG2, glm, 0, hSkin, 0, 0, 0 ) < 0 || !nm.previewG2 ) {
		NM_PreviewFree();
		Q_strncpyz( nm.previewKey, key, sizeof( nm.previewKey ) );	// don't retry every frame
		nm.previewError = "Couldn't load the model";
		return;
	}
	re->G2API_SetSkin( *nm.previewG2, 0, hSkin, hSkin );
	// the .npc's surfaces are for its own model
	if ( nm.previewOwnModel ) {
		NM_PreviewSurfaces( t->surfOff, G2SURFACEFLAG_OFF );
		NM_PreviewSurfaces( t->surfOn, 0 );
	}
	NM_PreviewAnimate();
	if ( t && !t->vehicle )
		NM_PreviewWeapons( t );
}

static void NM_RenderPreview( const nmType_t *t, float x, float y, float w, float h ) {
	refdef_t refdef;
	refEntity_t ent;
	vec3_t mins, maxs, center, forward, angles;
	float s = NM_PreviewScale( t ) / 100.0f, lift, xScale, yScale, fovX, fovY, halfH, radius, dist;

	// as cgame does: scaled characters are lifted so their feet stay on the ground
	lift = t->vehicle ? 0.0f : 24.0f * ( s - 1.0f );

	// frame the bigger of the scaled and unscaled model, so a smaller NPC shows smaller
	for ( int i = 0; i < 3; i++ ) {
		float off = i == 2 ? lift : 0.0f;

		mins[i] = Q_min( nm.previewMins[i], nm.previewMins[i] * s + off );
		maxs[i] = Q_max( nm.previewMaxs[i], nm.previewMaxs[i] * s + off );
	}
	// it turns around its origin
	radius = sqrtf( Square( Q_max( fabsf( mins[0] ), fabsf( maxs[0] ) ) ) + Square( Q_max( fabsf( mins[1] ), fabsf( maxs[1] ) ) ) );
	halfH = ( maxs[2] - mins[2] ) * 0.5f;
	VectorSet( center, 0, 0, ( mins[2] + maxs[2] ) * 0.5f );

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
	ent.ghoul2 = nm.previewG2;
	ent.renderfx = RF_NOSHADOW;
	VectorSet( ent.angles, 0, (float)( cls.realtime % 12000 ) * 0.03f + nm.previewYaw, 0 );	// a turn every 12 seconds
	AnglesToAxis( ent.angles, ent.axis );
	ent.origin[2] = lift;
	if ( s != 1.0f )
		VectorSet( ent.modelScale, s, s, s );
	memcpy( ent.shaderRGBA, t->rgba, sizeof( ent.shaderRGBA ) );
	ent.radius = radius + halfH;

	re->ClearScene();
	re->AddRefEntityToScene( &ent );
	re->RenderScene( &refdef );
}

// what the manage commands are run on, NULL if it can't be. Not va(): the
// manage tab keeps both answers across a frame of drawing
static const char *NM_ApplyTarget( qboolean allowAll ) {
	static char target[2][MAX_QPATH];
	const nmNpc_t *npc = NM_SelectedNpc();
	char *out = target[allowAll ? 1 : 0];

	switch ( nm.apply ) {
	case NM_APPLY_ALL:
		return allowAll ? "all" : NULL;
	case NM_APPLY_NAME:
		if ( !npc || !npc->name[0] )
			return NULL;
		Q_strncpyz( out, npc->name, sizeof( target[0] ) );
		return out;
	default:
		if ( !npc )
			return NULL;
		Com_sprintf( out, sizeof( target[0] ), "%i", npc->num );
		return out;
	}
}

static void NM_SendManage( const char *cmd, qboolean refresh ) {
	NM_Send( cmd, qtrue );
	if ( refresh )
		NM_RefreshSoon();
}

static void NM_Teleport( const vec3_t pos, int yaw ) {
	const char *target = NM_ApplyTarget( qfalse );

	if ( !target )
		return;
	NM_SendManage( va( "npc tele %i %i %i %i %s", (int)floorf( pos[0] + 0.5f ), (int)floorf( pos[1] + 0.5f ), (int)floorf( pos[2] + 0.5f ), yaw, target ), qtrue );
}

static void NM_SetTeleFields( const vec3_t pos, int yaw ) {
	for ( int i = 0; i < 3; i++ )
		Com_sprintf( nm.fields[NM_F_TELE_X + i], NM_FIELD_LEN, "%i", (int)floorf( pos[i] + 0.5f ) );
	Com_sprintf( nm.fields[NM_F_TELE_YAW], NM_FIELD_LEN, "%i", NM_NormalizeYaw( (float)yaw ) );
}

// the box to type where to teleport to, starting from where the selected NPC is
static void NM_OpenTeleBox( void ) {
	const nmNpc_t *npc = NM_SelectedNpc();
	vec3_t org;

	if ( npc ) {
		NM_NpcOrigin( npc, org );
		NM_SetTeleFields( org, npc->yaw );
	}
	nm.teleBox = qtrue;
	nm.focus = NM_F_TELE_X;
}

static void NM_TeleportToFields( void ) {
	vec3_t pos;

	for ( int i = 0; i < 3; i++ ) {
		if ( !nm.fields[NM_F_TELE_X + i][0] ) {
			NM_Message( S_COLOR_YELLOW "Fill in X, Y and Z" );
			return;
		}
		pos[i] = (float)atof( nm.fields[NM_F_TELE_X + i] );
	}
	NM_Teleport( pos, NM_NormalizeYaw( (float)atof( nm.fields[NM_F_TELE_YAW] ) ) );
	nm.teleBox = qfalse;
	nm.focus = NM_F_NONE;
}

/*
===============================================================================

SAVED NPCS AND CONSTRUCTS

Plain .cfg files in the game folder. Exec'd, the first line hands the file to the
manager, which reads it whole; the lines after it do nothing on their own.

===============================================================================
*/

static qboolean NM_SaveNameChar( int ch ) {
	return (qboolean)( ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) || ( ch >= '0' && ch <= '9' ) || ch == '_' || ch == '-' );
}

static const char *NM_SavePath( qboolean construct, const char *name ) {
	return va( "%s/%s%s", construct ? NM_CONSTRUCT_DIR : NM_NPC_DIR, name, NM_SAVE_EXT );
}

static void NM_WriteLine( fileHandle_t f, const char *text ) {
	FS_Write( text, strlen( text ), f );
}

// splits a line into words, "quoted" ones whole; the line is cut up in place
static int NM_SplitLine( char *line, char **argv, int max ) {
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

typedef qboolean ( *nmLineParser_t )( char **argv, int argc, void *out );

// the lines of a saved file parse turns into entries of out, each size big; -1 if there's no file
static int NM_ReadLines( const char *path, nmLineParser_t parse, void *out, size_t size, int max ) {
	char *buf, *line, *next, *argv[16];
	int count = 0, argc;

	if ( FS_ReadFile( path, (void **)&buf ) < 0 || !buf )
		return -1;
	for ( line = buf; line && count < max; line = next ) {
		next = strchr( line, '\n' );
		if ( next )
			*next++ = '\0';
		for ( char *c = line; *c; c++ ) {
			if ( *c == '\r' )
				*c = '\0';
		}
		argc = NM_SplitLine( line, argv, ARRAY_LEN( argv ) );
		if ( parse( argv, argc, (byte *)out + count * size ) )
			count++;
	}
	FS_FreeFile( buf );
	return count;
}

// the six words of a spawn: <type> <vehicle> <targetname> <team> <model> <scale>, "-" for none
static const char *NM_SpawnArgs( const nmSpawn_t *s ) {
	return va( "%s %i %s %s %s %s", s->type, s->vehicle ? 1 : 0, s->targetname[0] ? s->targetname : "-",
		nmTeamArgs[s->team], s->model[0] ? s->model : "-", s->scale[0] ? s->scale : "-" );
}

static void NM_ParseSpawnArgs( char **argv, nmSpawn_t *out ) {
	memset( out, 0, sizeof( *out ) );
	Q_strncpyz( out->type, argv[0], sizeof( out->type ) );
	out->vehicle = (qboolean)( atoi( argv[1] ) != 0 );
	Q_strncpyz( out->targetname, strcmp( argv[2], "-" ) ? argv[2] : "", sizeof( out->targetname ) );
	for ( int i = 0; i < (int)ARRAY_LEN( nmTeamArgs ); i++ ) {
		if ( !Q_stricmp( argv[3], nmTeamArgs[i] ) )
			out->team = (nmTeam_t)i;
	}
	Q_strncpyz( out->model, strcmp( argv[4], "-" ) ? argv[4] : "", sizeof( out->model ) );
	Q_strncpyz( out->scale, strcmp( argv[5], "-" ) ? argv[5] : "", sizeof( out->scale ) );
}

static qboolean NM_ParseSavedNpc( char **argv, int argc, void *out ) {
	// npcmanager savednpc <six words>
	if ( argc < 8 || Q_stricmp( argv[0], NM_TOGGLE_CMD ) || Q_stricmp( argv[1], "savednpc" ) )
		return qfalse;
	NM_ParseSpawnArgs( argv + 2, (nmSpawn_t *)out );
	return qtrue;
}

static qboolean NM_ParseConstructNpc( char **argv, int argc, void *to ) {
	nmSpawn_t *out = (nmSpawn_t *)to;

	// npcmanager constructnpc <six words> <x y z from the middle> <yaw>
	if ( argc < 12 || Q_stricmp( argv[0], NM_TOGGLE_CMD ) || Q_stricmp( argv[1], "constructnpc" ) )
		return qfalse;
	NM_ParseSpawnArgs( argv + 2, out );
	VectorSet( out->pos, atof( argv[8] ), atof( argv[9] ), atof( argv[10] ) );
	out->yaw = NM_NormalizeYaw( atof( argv[11] ) );
	return qtrue;
}

static int NM_ReadConstruct( const char *name, nmSpawn_t *out ) {
	return NM_ReadLines( NM_SavePath( qtrue, name ), NM_ParseConstructNpc, out, sizeof( *out ), NM_MAX_PLACED );
}

static int QDECL NM_CompareSaved( const void *a, const void *b ) {
	return Q_stricmp( ( (const nmSaved_t *)a )->name, ( (const nmSaved_t *)b )->name );
}

static void NM_RefreshSaved( void ) {
	nmSpawn_t *scratch = (nmSpawn_t *)Z_Malloc( sizeof( nmSpawn_t ) * NM_MAX_PLACED, TAG_TEMP_WORKSPACE, qfalse );

	nm.numSaved = 0;
	for ( int construct = 0; construct < 2; construct++ ) {
		char **list;
		int count;

		list = FS_ListFiles( construct ? NM_CONSTRUCT_DIR : NM_NPC_DIR, NM_SAVE_EXT, &count );
		for ( int i = 0; i < count && nm.numSaved < NM_MAX_SAVED; i++ ) {
			nmSaved_t *s = &nm.saved[nm.numSaved];

			memset( s, 0, sizeof( *s ) );
			Q_strncpyz( s->name, list[i], sizeof( s->name ) );
			if ( strlen( s->name ) > strlen( NM_SAVE_EXT ) )
				s->name[strlen( s->name ) - strlen( NM_SAVE_EXT )] = '\0';
			s->construct = (qboolean)construct;
			if ( construct )
				s->count = NM_ReadConstruct( s->name, scratch );
			else if ( NM_ReadLines( NM_SavePath( qfalse, s->name ), NM_ParseSavedNpc, &s->npc, sizeof( s->npc ), 1 ) < 1 )
				continue;	// not one of ours
			nm.numSaved++;
		}
		FS_FreeFileList( list );
	}
	Z_Free( scratch );
	qsort( nm.saved, nm.numSaved, sizeof( nm.saved[0] ), NM_CompareSaved );
	// appliedSaved stays: it's by name, and the form still holds it
	nm.constructShown[0] = '\0';
}

// the selected construct's NPCs, read once per construct
static void NM_ReadShownConstruct( const nmSaved_t *s ) {
	if ( !Q_stricmp( nm.constructShown, s->name ) )
		return;
	Q_strncpyz( nm.constructShown, s->name, sizeof( nm.constructShown ) );
	nm.numConstructNpcs = Q_max( 0, NM_ReadConstruct( s->name, nm.constructNpcs ) );
}

static int NM_FindSaved( qboolean construct, const char *name ) {
	for ( int i = 0; i < nm.numSaved; i++ ) {
		if ( nm.saved[i].construct == construct && !Q_stricmp( nm.saved[i].name, name ) )
			return i;
	}
	return -1;
}

// a saved NPC's settings go in the form while its row is selected; leaving it puts back what was there
static void NM_ApplySelection( void ) {
	const nmSaved_t *s = NM_SelectedSaved();
	const char *name = s && !s->construct ? s->name : "";

	if ( !Q_stricmp( name, nm.appliedSaved ) )
		return;
	if ( nm.appliedSaved[0] ) {
		Q_strncpyz( nm.fields[NM_F_TARGETNAME], nm.formBefore[0], NM_FIELD_LEN );
		Q_strncpyz( nm.fields[NM_F_MODEL], nm.formBefore[1], NM_FIELD_LEN );
		Q_strncpyz( nm.fields[NM_F_SCALE], nm.formBefore[2], NM_FIELD_LEN );
		nm.team = nm.teamBefore;
	}
	if ( name[0] ) {
		Q_strncpyz( nm.formBefore[0], nm.fields[NM_F_TARGETNAME], NM_FIELD_LEN );
		Q_strncpyz( nm.formBefore[1], nm.fields[NM_F_MODEL], NM_FIELD_LEN );
		Q_strncpyz( nm.formBefore[2], nm.fields[NM_F_SCALE], NM_FIELD_LEN );
		nm.teamBefore = nm.team;
		Q_strncpyz( nm.fields[NM_F_TARGETNAME], s->npc.targetname, NM_FIELD_LEN );
		Q_strncpyz( nm.fields[NM_F_MODEL], s->npc.model, NM_FIELD_LEN );
		Q_strncpyz( nm.fields[NM_F_SCALE], s->npc.scale, NM_FIELD_LEN );
		nm.team = s->npc.team;
	}
	Q_strncpyz( nm.appliedSaved, name, sizeof( nm.appliedSaved ) );
}

// shows a saved NPC or construct in the list, selected
static void NM_SelectSaved( qboolean construct, const char *name ) {
	const int index = NM_FindSaved( construct, name );

	if ( index < 0 )
		return;
	nm.fields[NM_F_TYPE_SEARCH][0] = '\0';
	nm.show = NM_SHOW_ALL;
	if ( nm.source != NM_SRC_ALL )
		nm.source = NM_SRC_SAVED;
	NM_RebuildTypeView();
	for ( int i = 0; i < nm.numTypeView; i++ ) {
		if ( nm.typeView[i] == -1 - index ) {
			nm.typeList.sel = i;
			NM_ListMove( &nm.typeList, nm.numTypeView, (int)( NM_TYPE_LIST_H / NM_ROW_H ), 0 );
			break;
		}
	}
	NM_ApplySelection();
}

static qboolean NM_SaveNpc( const char *name ) {
	const char *path;
	fileHandle_t f;
	nmSpawn_t s;

	if ( !NM_FormSpawn( &s ) )
		return qfalse;
	path = NM_SavePath( qfalse, name );
	f = FS_FOpenFileWrite( path );
	if ( !f ) {
		NM_Message( va( S_COLOR_RED "Couldn't write %s", path ) );
		return qfalse;
	}
	NM_WriteLine( f, va( "// NPC manager saved NPC: %s\n", name ) );
	NM_WriteLine( f, "// Shown under Saved in the NPC manager's spawn tab; exec this file to put it in the form\n" );
	NM_WriteLine( f, "// savednpc <type> <vehicle> <targetname> <team> <model or model/skin> <scale>, - for none\n" );
	NM_WriteLine( f, va( "npcmanager loadnpc %s\n", name ) );
	NM_WriteLine( f, va( "npcmanager savednpc %s\n", NM_SpawnArgs( &s ) ) );
	FS_FCloseFile( f );
	NM_RefreshSaved();
	NM_Message( va( S_COLOR_GREEN "Saved %s as %s", s.type, NM_SavePath( qfalse, name ) ) );
	return qtrue;
}

// what was put down, relative to the middle of its floor
static qboolean NM_SaveConstruct( const char *name ) {
	vec3_t mins, maxs, anchor;
	const char *path;
	fileHandle_t f;

	if ( !nm.numPlaced )
		return qfalse;
	ClearBounds( mins, maxs );
	for ( int i = 0; i < nm.numPlaced; i++ )
		AddPointToBounds( nm.placed[i].pos, mins, maxs );
	VectorSet( anchor, floorf( ( mins[0] + maxs[0] ) * 0.5f + 0.5f ), floorf( ( mins[1] + maxs[1] ) * 0.5f + 0.5f ), floorf( mins[2] + 0.5f ) );

	path = NM_SavePath( qtrue, name );
	f = FS_FOpenFileWrite( path );
	if ( !f ) {
		NM_Message( va( S_COLOR_RED "Couldn't write %s", path ) );
		return qfalse;
	}
	NM_WriteLine( f, va( "// NPC manager construct: %i NPCs, made on %s\n", nm.numPlaced,
		Info_ValueForKey( cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO], "mapname" ) ) );
	NM_WriteLine( f, "// Shown under Saved in the NPC manager's spawn tab; exec this file to pick it up and put it down\n" );
	NM_WriteLine( f, "// constructnpc <type> <vehicle> <targetname> <team> <model> <scale> <x y z from the middle> <yaw>\n" );
	NM_WriteLine( f, va( "npcmanager loadconstruct %s\n", name ) );
	for ( int i = 0; i < nm.numPlaced; i++ ) {
		const nmSpawn_t *s = &nm.placed[i];

		NM_WriteLine( f, va( "npcmanager constructnpc %s %i %i %i %i\n", NM_SpawnArgs( s ),
			(int)floorf( s->pos[0] - anchor[0] + 0.5f ), (int)floorf( s->pos[1] - anchor[1] + 0.5f ), (int)floorf( s->pos[2] - anchor[2] + 0.5f ), s->yaw ) );
	}
	FS_FCloseFile( f );
	NM_RefreshSaved();
	NM_Message( va( S_COLOR_GREEN "Saved %i NPCs as %s", nm.numPlaced, NM_SavePath( qtrue, name ) ) );
	return qtrue;
}

static void NM_StartSaving( nmSaving_t saving ) {
	const nmType_t *t = NM_SelectedType();
	const char *offer = "";

	if ( saving == NM_SAVING_NPC ) {
		if ( !t )
			return;
		offer = nm.fields[NM_F_TARGETNAME][0] ? nm.fields[NM_F_TARGETNAME] : t->name;
	} else if ( !nm.numPlaced ) {
		return;
	}
	Q_strncpyz( nm.fields[NM_F_SAVE_NAME], offer, NM_FIELD_LEN );
	for ( char *c = nm.fields[NM_F_SAVE_NAME]; *c; c++ ) {
		if ( !NM_SaveNameChar( *c ) )
			*c = '_';
	}
	nm.saving = saving;
	nm.focus = NM_F_NONE;
}

static void NM_DoSave( void ) {
	const char *name = nm.fields[NM_F_SAVE_NAME];

	if ( name[0] && ( nm.saving == NM_SAVING_NPC ? NM_SaveNpc( name ) : NM_SaveConstruct( name ) ) )
		nm.saving = NM_SAVING_NONE;
}

/*
===============================================================================

STATE CHANGES

===============================================================================
*/

static void NM_SetTab( nmTab_t tab ) {
	nm.tab = tab;
	nm.focus = NM_F_NONE;
	nm.teleBox = qfalse;
	if ( tab != NM_TAB_SPAWN )
		NM_ClosePreview();
	nm.confirm = NM_CONFIRM_NONE;
	if ( tab == NM_TAB_MANAGE )
		NM_RequestList();
}

static void NM_OpenPicker( nmPicker_t picker ) {
	nm.picker = picker;
	nm.previewYaw = 0.0f;
	nm.fields[NM_F_PICKER_SEARCH][0] = '\0';
	nm.pickerList.sel = nm.pickerList.scroll = 0;
	NM_RebuildPickerView();
}

static void NM_ChoosePicker( int index ) {
	const char *item = NM_PickerItem( index ), *target;

	switch ( nm.picker ) {
	case NM_PICKER_MODEL:
		Q_strncpyz( nm.fields[NM_F_MODEL], item, sizeof( nm.fields[NM_F_MODEL] ) );
		break;
	case NM_PICKER_EMOTE:
		Q_strncpyz( nm.fields[NM_F_EMOTE], item, sizeof( nm.fields[NM_F_EMOTE] ) );
		target = NM_ApplyTarget( qtrue );
		if ( target )
			NM_SendManage( va( "npc emote %s %s", item, target ), qfalse );
		break;
	case NM_PICKER_PLAYER:
		target = NM_ApplyTarget( qtrue );
		if ( target )
			NM_SendManage( va( "npc follow %i %s", index, target ), qfalse );
		break;
	default:
		break;
	}
	nm.picker = NM_PICKER_NONE;
}

static void NM_ReleaseKeys( void ) {
	memset( nm.held, 0, sizeof( nm.held ) );
}

// fly from where the player looks; the crosshair in the middle is what gets picked
static void NM_StartPick( nmPick_t pick ) {
	nm.pick = pick;
	nm.pickYaw = 0;
	nm.focus = NM_F_NONE;
	if ( nm.haveView ) {
		VectorCopy( nm.viewOrg, nm.camOrg );
		vectoangles( nm.viewAxis[0], nm.camAng );
	} else {
		VectorCopy( cl.snap.ps.origin, nm.camOrg );
		nm.camOrg[2] += cl.snap.ps.viewheight;
		VectorCopy( cl.viewangles, nm.camAng );
	}
	if ( nm.camAng[PITCH] > 180.0f )
		nm.camAng[PITCH] -= 360.0f;
	nm.camAng[ROLL] = 0;
	nm.lastFrameTime = cls.realtime;
	nm.pickCursorX = nm.cursorX;
	nm.pickCursorY = nm.cursorY;
	nm.cursorX = SCREEN_WIDTH * 0.5f;
	nm.cursorY = SCREEN_HEIGHT * 0.5f;
	NM_ReleaseKeys();
}

// back to the panel, the cursor where it was
static void NM_EndPick( void ) {
	if ( nm.pick == NM_PICK_NONE )
		return;
	nm.pick = NM_PICK_NONE;
	nm.cursorX = nm.pickCursorX;
	nm.cursorY = nm.pickCursorY;
	NM_ReleaseKeys();
}

// the form's NPC in hand, to put down as many as wanted
static void NM_StartPlace( void ) {
	if ( !NM_FormSpawn( &nm.hand[0] ) )
		return;
	VectorClear( nm.hand[0].pos );
	nm.numHand = 1;
	nm.handConstruct = qfalse;
	Q_strncpyz( nm.handName, nm.hand[0].type, sizeof( nm.handName ) );
	NM_StartPick( NM_PICK_PLACE );
}

// a saved construct in hand
static void NM_StartConstruct( const char *name ) {
	int count = NM_ReadConstruct( name, nm.hand );

	if ( count <= 0 ) {
		NM_Message( va( S_COLOR_YELLOW "%s %s", count < 0 ? "No construct" : "No NPCs in", NM_SavePath( qtrue, name ) ) );
		return;
	}
	nm.numHand = count;
	nm.handConstruct = qtrue;
	Q_strncpyz( nm.handName, name, sizeof( nm.handName ) );
	NM_StartPick( NM_PICK_PLACE );
}

// where an NPC in hand goes with the hand at anchor: one alone faces the camera, a
// construct turns about its middle by the wheel
static void NM_HandSpot( int i, const vec3_t anchor, vec3_t pos, int *yaw ) {
	const nmSpawn_t *h = &nm.hand[i];
	float a, c, s;

	if ( !nm.handConstruct ) {
		VectorCopy( anchor, pos );
		*yaw = NM_NormalizeYaw( NM_YawToView( anchor ) + nm.pickYaw );
		return;
	}
	a = DEG2RAD( (float)nm.pickYaw );
	c = cosf( a );
	s = sinf( a );
	pos[0] = anchor[0] + h->pos[0] * c - h->pos[1] * s;
	pos[1] = anchor[1] + h->pos[0] * s + h->pos[1] * c;
	pos[2] = anchor[2] + h->pos[2];
	*yaw = NM_NormalizeYaw( (float)( h->yaw + nm.pickYaw ) );
}

// what is in hand goes down where anchor is, waiting to be spawned
static void NM_PutDown( const vec3_t anchor ) {
	for ( int i = 0; i < nm.numHand; i++ ) {
		nmSpawn_t *p;

		if ( nm.numPlaced >= NM_MAX_PLACED ) {
			NM_Message( S_COLOR_YELLOW "That's as many as can wait at once; spawn them first" );
			break;
		}
		p = &nm.placed[nm.numPlaced++];
		*p = nm.hand[i];
		NM_HandSpot( i, anchor, p->pos, &p->yaw );
	}
}

static qboolean NM_Open( nmTab_t tab ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "NPC manager: join a server first\n" );
		return qfalse;
	}
	CL_ModelManager_Close();
	CL_ShaderManager_Close();
	CL_EffectManager_Close();
	if ( !nm.indexed )
		NM_BuildIndex();

	nm.font = re->RegisterFont( "arialnb" );
	if ( !nm.font )
		nm.font = cls.menuFont;

	nm.open = qtrue;
	nm.pick = NM_PICK_NONE;
	nm.picker = NM_PICKER_NONE;
	nm.saving = NM_SAVING_NONE;
	nm.preview = qfalse;
	nm.cursorX = SCREEN_WIDTH * 0.5f;
	nm.cursorY = SCREEN_HEIGHT * 0.5f;
	nm.click = qfalse;
	nm.wheel = 0;
	NM_RefreshSaved();
	NM_RebuildTypeView();
	NM_SetTab( tab );
	if ( tab != NM_TAB_MANAGE )
		NM_RequestList();	// for the map labels
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_NPCMANAGER );
	return qtrue;
}

void CL_NpcManager_Close( void ) {
	if ( !nm.open )
		return;
	nm.open = qfalse;
	nm.pick = NM_PICK_NONE;
	nm.picker = NM_PICKER_NONE;
	nm.saving = NM_SAVING_NONE;
	nm.teleBox = qfalse;
	nm.preview = qfalse;
	NM_ReleaseKeys();
	NM_PreviewFree();	// never called while drawing
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_NPCMANAGER );
}

void CL_NpcManager_Init( void ) {
	memset( &nm, 0, sizeof( nm ) );
	nm.selNum = -1;
}

// cgame is going away: map change, disconnect or vid_restart
void CL_NpcManager_Shutdown( void ) {
	CL_NpcManager_Close();
	NM_PreviewFree();	// the renderer may be going too
	// the pure pk3 list can change with the next map or server
	nm.indexed = qfalse;
	nm.haveView = qfalse;
	nm.numNpcs = nm.numNpcView = 0;
	nm.listRequestTime = nm.listLineTime = nm.listTime = nm.refreshTime = 0;
	nm.havePos = qfalse;
	nm.selNum = -1;
	// spots in this map, for this server
	nm.numPlaced = nm.placedScroll = 0;
	nm.numQueue = nm.queueTotal = 0;
}

qboolean CL_NpcManager_Active( void ) {
	return nm.open;
}

// every frame in game, open or not: the spawns still to go, and the list
void CL_NpcManager_Frame( void ) {
	NM_RunQueue();

	// a list we asked for that never came, or one that is due
	if ( nm.listRequestTime && cls.realtime - nm.listRequestTime > NM_LIST_TIMEOUT ) {
		nm.listRequestTime = 0;
		nm.numNpcs = 0;
		NM_RebuildNpcView();
	}
	if ( nm.refreshTime && cls.realtime >= nm.refreshTime && !nm.numQueue && cls.realtime >= nm.nextSend )
		NM_RequestList();
}

/*
===============================================================================

CONSOLE COMMAND

===============================================================================
*/

void CL_NpcManager_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( Cmd_Argc() < 2 ) {
		if ( nm.open )
			CL_NpcManager_Close();
		else
			NM_Open( nm.tab );
		return;
	}

	if ( !Q_stricmp( arg, "spawn" ) ) {
		if ( nm.open )
			NM_SetTab( NM_TAB_SPAWN );
		else
			NM_Open( NM_TAB_SPAWN );
	} else if ( !Q_stricmp( arg, "manage" ) ) {
		if ( nm.open )
			NM_SetTab( NM_TAB_MANAGE );
		else
			NM_Open( NM_TAB_MANAGE );
	} else if ( !Q_stricmp( arg, "close" ) ) {
		CL_NpcManager_Close();
	} else if ( !Q_stricmp( arg, "reindex" ) ) {
		NM_BuildIndex();
		NM_RefreshSaved();
		NM_RebuildTypeView();
	} else if ( !Q_stricmp( arg, "loadnpc" ) && Cmd_Argc() >= 3 ) {
		char name[MAX_QPATH];

		Q_strncpyz( name, Cmd_Argv( 2 ), sizeof( name ) );
		COM_StripExtension( name, name, sizeof( name ) );
		if ( !nm.open && !NM_Open( NM_TAB_SPAWN ) )
			return;
		NM_SetTab( NM_TAB_SPAWN );
		NM_RefreshSaved();
		if ( NM_FindSaved( qfalse, name ) < 0 )
			Com_Printf( S_COLOR_YELLOW "NPC manager: no saved NPC %s\n", NM_SavePath( qfalse, name ) );
		NM_SelectSaved( qfalse, name );
	} else if ( !Q_stricmp( arg, "loadconstruct" ) && Cmd_Argc() >= 3 ) {
		char name[MAX_QPATH];

		Q_strncpyz( name, Cmd_Argv( 2 ), sizeof( name ) );
		COM_StripExtension( name, name, sizeof( name ) );
		if ( !nm.open && !NM_Open( NM_TAB_SPAWN ) )
			return;
		NM_SetTab( NM_TAB_SPAWN );
		NM_StartConstruct( name );
		if ( nm.pick == NM_PICK_NONE )
			Com_Printf( "%s\n", nm.message );
	} else if ( !Q_stricmp( arg, "savednpc" ) || !Q_stricmp( arg, "constructnpc" ) ) {
		// the lines of a saved file being exec'd; its first line read them all
	} else {
		Com_Printf( "usage: npcmanager                       toggle the NPC manager (bind a key to it)\n" );
		Com_Printf( "       npcmanager spawn                 open on the spawn tab\n" );
		Com_Printf( "       npcmanager manage                open on the NPCs in the map\n" );
		Com_Printf( "       npcmanager loadnpc <name>        put a saved NPC in the spawn form\n" );
		Com_Printf( "       npcmanager loadconstruct <name>  pick up an NPC construct to put down\n" );
		Com_Printf( "       npcmanager reindex               rescan the filesystem for NPC types and models\n" );
		Com_Printf( "       npcmanager close                 close the NPC manager\n" );
		Com_Printf( "Saved NPCs are in the %s folder of the game folder, constructs in %s; both can be exec'd\n", NM_NPC_DIR, NM_CONSTRUCT_DIR );
	}
}

/*
===============================================================================

MAP VIEW

===============================================================================
*/

static qboolean NM_Flying( void ) {
	return (qboolean)( nm.open && nm.pick != NM_PICK_NONE && ( Key_GetCatcher() & KEYCATCH_NPCMANAGER ) );
}

static void NM_UpdateCamera( void ) {
	vec3_t forward, right, move;
	float dt, speed;

	dt = Com_Clamp( 0.0f, 0.1f, ( cls.realtime - nm.lastFrameTime ) / 1000.0f );
	nm.lastFrameTime = cls.realtime;

	AngleVectors( nm.camAng, forward, right, NULL );
	VectorClear( move );
	if ( nm.held[A_CAP_W] )		VectorAdd( move, forward, move );
	if ( nm.held[A_CAP_S] )		VectorSubtract( move, forward, move );
	if ( nm.held[A_CAP_D] )		VectorAdd( move, right, move );
	if ( nm.held[A_CAP_A] )		VectorSubtract( move, right, move );
	if ( nm.held[A_SPACE] )		move[2] += 1.0f;
	if ( nm.held[A_CAP_C] )		move[2] -= 1.0f;

	if ( VectorNormalize( move ) > 0.0f ) {
		speed = NM_FLY_SPEED;
		if ( nm.held[A_SHIFT] || nm.held[A_SHIFT2] )
			speed *= 3.0f;
		if ( nm.held[A_CTRL] || nm.held[A_CTRL2] )
			speed *= 0.25f;
		VectorMA( nm.camOrg, speed * dt, move, nm.camOrg );
	}
}

static void NM_KeepView( const refdef_t *fd ) {
	VectorCopy( fd->vieworg, nm.viewOrg );
	VectorCopy( fd->viewaxis[0], nm.viewAxis[0] );
	VectorCopy( fd->viewaxis[1], nm.viewAxis[1] );
	VectorCopy( fd->viewaxis[2], nm.viewAxis[2] );
	nm.fovX = fd->fov_x;
	nm.fovY = fd->fov_y;
	nm.haveView = qtrue;
}

// every cgame scene passes through here before reaching the renderer; qtrue if it was rendered
qboolean CL_NpcManager_RenderScene( const refdef_t *fd ) {
	refdef_t view;

	if ( fd->rdflags & ( RDF_NOWORLDMODEL | RDF_AUTOMAP ) )
		return qfalse;

	if ( fd->rdflags & RDF_SKYBOXPORTAL ) {
		if ( !NM_Flying() )
			return qfalse;
		// the sky portal keeps its own origin but looks the way the camera does
		view = *fd;
		AnglesToAxis( nm.camAng, view.viewaxis );
		VectorCopy( nm.camAng, view.viewangles );
		re->RenderScene( &view );
		return qtrue;
	}

	if ( !NM_Flying() ) {
		NM_KeepView( fd );
		return qfalse;
	}

	if ( nm.viewFrame != cls.framecount ) {
		nm.viewFrame = cls.framecount;
		NM_UpdateCamera();
	}
	view = *fd;
	VectorCopy( nm.camOrg, view.vieworg );
	VectorCopy( nm.camAng, view.viewangles );
	AnglesToAxis( nm.camAng, view.viewaxis );
	view.viewContents = CM_PointContents( nm.camOrg, 0 );
	// the snapshot's area mask is from the player's position, the camera can be anywhere
	memset( view.areamask, 0, sizeof( view.areamask ) );
	NM_KeepView( &view );
	re->RenderScene( &view );
	return qtrue;
}

// the free camera, for the effects system to cull against
qboolean CL_NpcManager_Camera( vec3_t origin, vec3_t angles ) {
	if ( !NM_Flying() )
		return qfalse;
	VectorCopy( nm.camOrg, origin );
	VectorCopy( nm.camAng, angles );
	return qtrue;
}

// the view weapon would float where the player stands
qboolean CL_NpcManager_FilterEntity( const refEntity_t *ent ) {
	return (qboolean)( NM_Flying() && ( ent->renderfx & RF_FIRST_PERSON ) );
}

static qboolean NM_Project( const vec3_t point, float *x, float *y ) {
	vec3_t d;
	float forward;

	if ( !nm.haveView )
		return qfalse;
	VectorSubtract( point, nm.viewOrg, d );
	forward = DotProduct( d, nm.viewAxis[0] );
	if ( forward < 4.0f )
		return qfalse;
	*x = SCREEN_WIDTH * 0.5f * ( 1.0f - DotProduct( d, nm.viewAxis[1] ) / forward / tanf( DEG2RAD( nm.fovX * 0.5f ) ) );
	*y = SCREEN_HEIGHT * 0.5f * ( 1.0f - DotProduct( d, nm.viewAxis[2] ) / forward / tanf( DEG2RAD( nm.fovY * 0.5f ) ) );
	return qtrue;
}

// the world under the cursor; out is where an NPC's origin would go
static qboolean NM_TraceCursor( vec3_t out ) {
	vec3_t dir, end;
	float x, y;
	trace_t tr;

	if ( !nm.haveView )
		return qfalse;
	x = ( nm.cursorX / ( SCREEN_WIDTH * 0.5f ) - 1.0f ) * tanf( DEG2RAD( nm.fovX * 0.5f ) );
	y = ( nm.cursorY / ( SCREEN_HEIGHT * 0.5f ) - 1.0f ) * tanf( DEG2RAD( nm.fovY * 0.5f ) );
	VectorCopy( nm.viewAxis[0], dir );
	VectorMA( dir, -x, nm.viewAxis[1], dir );
	VectorMA( dir, -y, nm.viewAxis[2], dir );
	VectorNormalize( dir );
	VectorMA( nm.viewOrg, NM_TRACE_DIST, dir, end );

	CM_BoxTrace( &tr, nm.viewOrg, end, vec3_origin, vec3_origin, 0, NM_TRACE_MASK, qfalse );
	if ( tr.fraction >= 1.0f || tr.allsolid )
		return qfalse;

	// off the floor by its own height, out of walls by a bit
	VectorMA( tr.endpos, 16.0f, tr.plane.normal, out );
	if ( tr.plane.normal[2] > 0.7f )
		out[2] = tr.endpos[2] + NM_GROUND_HEIGHT;
	return qtrue;
}

/*
===============================================================================

INPUT

===============================================================================
*/

static qboolean NM_InRect( float x, float y, float w, float h ) {
	return (qboolean)( nm.cursorX >= x && nm.cursorX < x + w && nm.cursorY >= y && nm.cursorY < y + h );
}

// binds don't run while the manager holds the keys, so honour the user's toggle bind here
static qboolean NM_IsToggleKey( int key ) {
	const char *binding;

	// a printable key belongs to the text box
	if ( nm.pick == NM_PICK_NONE && key >= A_SPACE && key <= A_TILDE )
		return qfalse;
	binding = Key_GetBinding( key );
	return (qboolean)( VALIDSTRING( binding ) && !Q_stricmp( binding, NM_TOGGLE_CMD ) );
}

// the NPC whose label is under the cursor, -1 = none
static int NM_NpcAtCursor( void ) {
	float best = NM_LABEL_RANGE * NM_LABEL_RANGE, x, y;
	vec3_t org;
	int found = -1;

	for ( int i = 0; i < nm.numNpcs; i++ ) {
		NM_NpcOrigin( &nm.npcs[i], org );
		org[2] += 40.0f;
		if ( !NM_Project( org, &x, &y ) )
			continue;
		x -= nm.cursorX;
		y -= nm.cursorY;
		if ( x * x + y * y < best ) {
			best = x * x + y * y;
			found = i;
		}
	}
	return found;
}

// the selected NPC by its label on the map
static void NM_SelectAtCursor( void ) {
	int i = NM_NpcAtCursor();

	if ( i < 0 )
		return;
	// a filter could hide it in the list
	nm.fields[NM_F_NPC_SEARCH][0] = '\0';
	nm.selNum = nm.npcs[i].num;
	NM_RebuildNpcView();
	NM_ListMove( &nm.npcList, nm.numNpcView, (int)( NM_LIST_H / NM_ROW_H ), 0 );
}

static void NM_PickClick( void ) {
	vec3_t pos;

	if ( nm.pick == NM_PICK_SELECT ) {
		// a miss keeps flying, to look for it elsewhere
		if ( NM_NpcAtCursor() >= 0 ) {
			NM_SelectAtCursor();
			NM_EndPick();
		}
		return;
	}
	if ( !NM_TraceCursor( pos ) )
		return;
	if ( nm.pick == NM_PICK_PLACE ) {
		// stays in hand for the next one
		NM_PutDown( pos );
		return;
	}
	if ( nm.pick == NM_PICK_SPAWN ) {
		VectorCopy( pos, nm.pos );
		nm.yaw = NM_NormalizeYaw( NM_YawToView( pos ) + nm.pickYaw );
		nm.havePos = qtrue;
	} else if ( nm.pick == NM_PICK_TELE ) {
		NM_Teleport( pos, NM_NormalizeYaw( NM_YawToView( pos ) + nm.pickYaw ) );
	}
	NM_EndPick();
}

static qboolean NM_FieldAllows( nmField_t f, int ch ) {
	if ( f == NM_F_SAVE_NAME )
		return NM_SaveNameChar( ch );
	if ( f >= NM_F_TELE_X && f <= NM_F_TELE_YAW )
		return (qboolean)( ( ch >= '0' && ch <= '9' ) || ch == '-' || ch == '.' );
	if ( f == NM_F_SCALE )
		return (qboolean)( ( ch >= '0' && ch <= '9' ) || ch == '.' );
	// these go on the command line as single arguments
	if ( f == NM_F_TARGETNAME || f == NM_F_MODEL || f == NM_F_EMOTE || f == NM_F_DIALOG )
		return (qboolean)( ch > ' ' && ch < 127 && ch != '"' && ch != ';' );
	return (qboolean)( ch >= ' ' && ch < 127 );
}

static void NM_FieldChanged( nmField_t f ) {
	switch ( f ) {
	case NM_F_TYPE_SEARCH:
		nm.typeList.sel = nm.typeList.scroll = 0;
		NM_RebuildTypeView();
		break;
	case NM_F_NPC_SEARCH:
		nm.npcList.scroll = 0;
		NM_RebuildNpcView();
		break;
	case NM_F_PICKER_SEARCH:
		nm.pickerList.sel = nm.pickerList.scroll = 0;
		NM_RebuildPickerView();
		break;
	default:
		break;
	}
}

static void NM_ListKey( int key ) {
	nmList_t *list;
	int count, visible, delta;

	if ( nm.picker != NM_PICKER_NONE ) {
		list = &nm.pickerList;
		count = nm.numPickerView;
		visible = (int)( ( NM_PICKER_H - 74 ) / NM_ROW_H );
	} else if ( nm.tab == NM_TAB_SPAWN ) {
		list = &nm.typeList;
		count = nm.numTypeView;
		visible = (int)( NM_TYPE_LIST_H / NM_ROW_H );
	} else {
		list = &nm.npcList;
		count = nm.numNpcView;
		visible = (int)( NM_LIST_H / NM_ROW_H );
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
	NM_ListMove( list, count, visible, delta );
	if ( list == &nm.npcList && list->sel < nm.numNpcView )
		nm.selNum = nm.npcs[nm.npcView[list->sel]].num;
}

// Enter or a double-click on the list: a construct is picked up, anything else spawns
static void NM_SpawnOrPlace( void ) {
	const nmSaved_t *s = NM_SelectedSaved();

	if ( s && s->construct )
		NM_StartConstruct( s->name );
	else
		NM_Spawn();
}

void CL_NpcManager_KeyEvent( int key, qboolean down ) {
	nmField_t f;
	int len;

	if ( !nm.open )
		return;
	// held keys fly the camera
	if ( key >= 0 && key < MAX_KEYS )
		nm.held[key] = down;
	if ( !down )
		return;

	if ( NM_IsToggleKey( key ) ) {
		CL_NpcManager_Close();
		return;
	}

	if ( nm.pick != NM_PICK_NONE ) {
		switch ( key ) {
		case A_MOUSE1:
			NM_PickClick();
			break;
		case A_MOUSE2:
			NM_EndPick();
			break;
		case A_MWHEELUP:
			nm.pickYaw += NM_YAW_STEP;
			break;
		case A_MWHEELDOWN:
			nm.pickYaw -= NM_YAW_STEP;
			break;
		case A_BACKSPACE:
			// takes back the last one put down
			if ( nm.pick == NM_PICK_PLACE && nm.numPlaced )
				nm.numPlaced--;
			break;
		case A_ENTER:
		case A_KP_ENTER:
			if ( nm.pick == NM_PICK_PLACE ) {
				NM_SpawnPlaced();
				NM_EndPick();
			}
			break;
		default:
			break;
		}
		return;
	}

	f = NM_ActiveField();
	if ( nm.saving != NM_SAVING_NONE && nm.picker == NM_PICKER_NONE ) {
		// only the name box takes keys
		switch ( key ) {
		case A_ENTER:
		case A_KP_ENTER:
			NM_DoSave();
			return;
		case A_MOUSE1:
		case A_BACKSPACE:
		case A_DELETE:
			break;
		default:
			return;
		}
	}
	if ( nm.teleBox && nm.picker == NM_PICKER_NONE ) {
		// only the teleport box takes keys
		switch ( key ) {
		case A_ENTER:
		case A_KP_ENTER:
			NM_TeleportToFields();
			return;
		case A_TAB:
			// the next box, Shift the one before
			nm.focus = (nmField_t)( NM_F_TELE_X + ( f - NM_F_TELE_X + ( nm.held[A_SHIFT] || nm.held[A_SHIFT2] ? 3 : 1 ) ) % 4 );
			return;
		case A_MOUSE1:
		case A_BACKSPACE:
		case A_DELETE:
			break;
		default:
			return;
		}
	}
	if ( nm.preview && nm.picker == NM_PICKER_NONE ) {
		// nothing to type in; the keys go to the popup and the type list behind it
		switch ( key ) {
		case A_BACKSPACE:
		case A_DELETE:
		case A_TAB:
			return;
		case A_ENTER:
		case A_KP_ENTER:
			NM_Spawn();
			NM_ClosePreview();
			return;
		default:
			break;
		}
	}
	switch ( key ) {
	case A_MOUSE1:
		nm.click = qtrue;
		nm.clickX = nm.cursorX;
		nm.clickY = nm.cursorY;
		break;
	case A_MWHEELUP:
		nm.wheel -= 3;
		break;
	case A_MWHEELDOWN:
		nm.wheel += 3;
		break;
	case A_BACKSPACE:
		len = strlen( nm.fields[f] );
		if ( len ) {
			nm.fields[f][len - 1] = '\0';
			NM_FieldChanged( f );
		}
		break;
	case A_DELETE:
		nm.fields[f][0] = '\0';
		NM_FieldChanged( f );
		break;
	case A_TAB:
		if ( nm.picker == NM_PICKER_NONE )
			NM_SetTab( nm.tab == NM_TAB_SPAWN ? NM_TAB_MANAGE : NM_TAB_SPAWN );
		break;
	case A_ENTER:
	case A_KP_ENTER:
		if ( nm.picker != NM_PICKER_NONE ) {
			if ( nm.pickerList.sel < nm.numPickerView )
				NM_ChoosePicker( nm.pickerView[nm.pickerList.sel] );
		} else if ( f == NM_F_EMOTE || f == NM_F_DIALOG ) {
			const char *target = NM_ApplyTarget( qtrue );

			if ( target && nm.fields[f][0] )
				NM_SendManage( va( "npc %s %s %s", f == NM_F_EMOTE ? "emote" : "dialog", nm.fields[f], target ), qfalse );
		} else if ( nm.tab == NM_TAB_SPAWN ) {
			NM_SpawnOrPlace();
		}
		break;
	default:
		NM_ListKey( key );
		break;
	}
}

void CL_NpcManager_CharEvent( int ch ) {
	nmField_t f;
	int len;

	if ( !nm.open || nm.pick != NM_PICK_NONE || ( nm.preview && nm.picker == NM_PICKER_NONE ) )
		return;
	f = NM_ActiveField();
	len = strlen( nm.fields[f] );
	if ( NM_FieldAllows( f, ch ) && len < NM_FIELD_LEN - 1 ) {
		nm.fields[f][len] = (char)ch;
		nm.fields[f][len + 1] = '\0';
		NM_FieldChanged( f );
	}
}

void CL_NpcManager_MouseEvent( int dx, int dy ) {
	if ( nm.pick != NM_PICK_NONE ) {
		nm.camAng[YAW] -= dx * cl_sensitivity->value * m_yaw->value;
		nm.camAng[PITCH] = Com_Clamp( -89.0f, 89.0f, nm.camAng[PITCH] + dy * cl_sensitivity->value * m_pitch->value );
		return;
	}
	nm.cursorX = Com_Clamp( 0, SCREEN_WIDTH, nm.cursorX + dx );
	nm.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, nm.cursorY + dy );
}

// escape backs out one level: picker, picking, a text box, then the manager
void CL_NpcManager_Escape( void ) {
	if ( nm.picker != NM_PICKER_NONE )
		nm.picker = NM_PICKER_NONE;
	else if ( nm.saving != NM_SAVING_NONE )
		nm.saving = NM_SAVING_NONE;
	else if ( nm.teleBox )
		nm.teleBox = qfalse, nm.focus = NM_F_NONE;
	else if ( nm.preview )
		NM_ClosePreview();
	else if ( nm.pick != NM_PICK_NONE )
		NM_EndPick();
	else if ( nm.focus != NM_F_NONE )
		nm.focus = NM_F_NONE;
	else
		CL_NpcManager_Close();
}

/*
===============================================================================

2D

===============================================================================
*/

static void NM_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void NM_Box( float x, float y, float w, float h, const float *fill ) {
	NM_Fill( x, y, w, h, fill );
	NM_Fill( x, y, w, 1, nmBorder );
	NM_Fill( x, y + h - 1, w, 1, nmBorder );
	NM_Fill( x, y, 1, h, nmBorder );
	NM_Fill( x + w - 1, y, 1, h, nmBorder );
}

static void NM_Text( float x, float y, const char *text, const float *color ) {
	re->Font_DrawString( (int)x, (int)y, text, color, nm.font | STYLE_DROPSHADOW, -1, NM_TEXT_SCALE );
}

static float NM_TextWidth( const char *text ) {
	return (float)re->Font_StrLenPixels( text, nm.font, NM_TEXT_SCALE );
}

static void NM_TextClipped( float x, float y, float w, const char *text, const float *color ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 3 && NM_TextWidth( buf ) > w ) {
		len--;
		buf[len - 3] = '.';
		buf[len - 2] = '.';
		buf[len - 1] = '.';
		buf[len] = '\0';
	}
	NM_Text( x, y, buf, color );
}

// eats this frame's click if it landed in the rect
static qboolean NM_Clicked( float x, float y, float w, float h ) {
	if ( !nm.click || nm.clickX < x || nm.clickX >= x + w || nm.clickY < y || nm.clickY >= y + h )
		return qfalse;
	nm.click = qfalse;
	return qtrue;
}

static qboolean NM_Button( float x, float y, float w, const char *label, qboolean selected, qboolean enabled ) {
	const float *fill = selected ? nmHighlight : ( enabled && NM_InRect( x, y, w, NM_CTRL_H ) ) ? nmHoverBox : nmPanelLight;

	NM_Box( x, y, w, NM_CTRL_H, fill );
	NM_Text( x + ( w - NM_TextWidth( label ) ) * 0.5f, y + 2, label, enabled ? nmWhite : nmDim );
	return (qboolean)( NM_Clicked( x, y, w, NM_CTRL_H ) && enabled );
}

// a button that has to be clicked twice
static qboolean NM_DangerButton( float x, float y, float w, const char *label, nmConfirm_t id, qboolean enabled ) {
	qboolean armed = (qboolean)( nm.confirm == id && cls.realtime - nm.confirmTime < NM_CONFIRM_MS );
	const char *text = armed ? "Click again" : label;

	NM_Box( x, y, w, NM_CTRL_H, armed ? nmDanger : ( enabled && NM_InRect( x, y, w, NM_CTRL_H ) ) ? nmHoverBox : nmPanelLight );
	NM_Text( x + ( w - NM_TextWidth( text ) ) * 0.5f, y + 2, text, enabled ? nmWhite : nmDim );
	if ( !NM_Clicked( x, y, w, NM_CTRL_H ) || !enabled )
		return qfalse;
	if ( armed ) {
		nm.confirm = NM_CONFIRM_NONE;
		return qtrue;
	}
	nm.confirm = id;
	nm.confirmTime = cls.realtime;
	return qfalse;
}

static void NM_Field( nmField_t f, float x, float y, float w, const char *placeholder ) {
	qboolean focused = (qboolean)( NM_ActiveField() == f );
	const char *text = nm.fields[f];
	char buf[NM_FIELD_LEN + 2];

	NM_Box( x, y, w, NM_CTRL_H, focused ? nmPanelFocus : nmPanelLight );
	if ( !text[0] && !focused ) {
		NM_TextClipped( x + 4, y + 2, w - 8, placeholder, nmDim );
	} else {
		// keep the end in view
		Com_sprintf( buf, sizeof( buf ), "%s%s", text, focused && ( ( cls.realtime >> 8 ) & 1 ) ? "_" : "" );
		const char *shown = buf;
		while ( shown[1] && NM_TextWidth( shown ) > w - 8 )
			shown++;
		NM_Text( x + 4, y + 2, shown, nmWhite );
	}
	if ( NM_Clicked( x, y, w, NM_CTRL_H ) )
		nm.focus = ( f == NM_F_TYPE_SEARCH || f == NM_F_NPC_SEARCH || f == NM_F_PICKER_SEARCH || f == NM_F_SAVE_NAME ) ? NM_F_NONE : f;
}

static void NM_Label( float y, const char *label ) {
	NM_Text( NM_RIGHT_X, y + 2, label, nmDim );
}

typedef void ( *nmRowFunc_t )( int row, float x, float y, float w );

// returns the row double-clicked, -1 if none
static int NM_List( nmList_t *list, int count, float x, float y, float w, float h, nmRowFunc_t drawRow ) {
	const int visible = (int)( h / NM_ROW_H );
	int row, picked = -1;

	if ( nm.wheel && NM_InRect( x, y, w, h ) ) {
		list->scroll += nm.wheel;
		nm.wheel = 0;
	}
	NM_ListClamp( list, count, visible );

	if ( NM_Clicked( x, y, w, h ) ) {
		row = list->scroll + (int)( ( nm.clickY - y ) / NM_ROW_H );
		if ( row < count ) {
			if ( row == list->lastClickRow && cls.realtime - list->lastClickTime < NM_DOUBLECLICK_MS ) {
				list->lastClickTime = 0;
				picked = row;
			} else {
				list->lastClickRow = row;
				list->lastClickTime = cls.realtime;
			}
			list->sel = row;
		}
	}

	NM_Box( x, y, w, h, nmPanelLight );
	for ( int i = 0; i < visible; i++ ) {
		float ry = y + i * NM_ROW_H;

		row = list->scroll + i;
		if ( row >= count )
			break;
		if ( row == list->sel )
			NM_Fill( x + 1, ry, w - 2, NM_ROW_H, nmHighlight );
		else if ( NM_InRect( x, ry, w, NM_ROW_H ) )
			NM_Fill( x + 1, ry, w - 2, NM_ROW_H, nmHover );
		drawRow( row, x + 4, ry + 1, w - 8 );
	}
	return picked;
}

static void NM_DrawTypeRow( int row, float x, float y, float w ) {
	const int v = nm.typeView[row];
	const nmType_t *t;
	const nmSaved_t *s;

	if ( v < 0 ) {
		s = &nm.saved[-1 - v];
		if ( s->construct )
			NM_TextClipped( x, y, w, va( S_COLOR_CYAN "%s " S_COLOR_GREY "(construct, %i NPCs)", s->name, s->count ), nmWhite );
		else
			NM_TextClipped( x, y, w, va( S_COLOR_CYAN "%s " S_COLOR_GREY "(saved %s)", s->name, s->npc.type ), nmWhite );
		return;
	}
	t = &nm.types[v];
	if ( t->custom || t->vehicle )
		NM_TextClipped( x, y, w, va( "%s " S_COLOR_GREY "(%s%s%s)", t->name, t->custom ? "custom" : "", t->custom && t->vehicle ? " " : "", t->vehicle ? "vehicle" : "" ), nmWhite );
	else
		NM_TextClipped( x, y, w, t->name, nmWhite );
}

// "Base" or "Custom, from <pk3>"
static const char *NM_SourceLabel( const nmType_t *t ) {
	if ( !t->custom )
		return va( "Base, from %s", nm.sources[t->source] );
	return t->source ? va( "Custom, from %s", nm.sources[t->source] ) : "Custom, not in your files";
}

static void NM_DrawNpcRow( int row, float x, float y, float w ) {
	const nmNpc_t *n = &nm.npcs[nm.npcView[row]];

	NM_Text( x, y, va( S_COLOR_CYAN "%i", n->num ), nmWhite );
	NM_TextClipped( x + 34, y, w - 34, n->name[0] ? va( "%s " S_COLOR_GREY "%s", n->name, n->type ) : va( S_COLOR_GREY "%s", n->type ), nmWhite );
}

static void NM_DrawPickerRow( int row, float x, float y, float w ) {
	const int index = nm.pickerView[row];

	if ( nm.picker == NM_PICKER_PLAYER )
		NM_TextClipped( x, y, w, va( S_COLOR_CYAN "%i " S_COLOR_WHITE "%s%s", index, NM_PickerItem( index ), index == clc.clientNum ? S_COLOR_GREY " (you)" : "" ), nmWhite );
	else
		NM_TextClipped( x, y, w, NM_PickerItem( index ), nmWhite );
}

static void NM_DrawCommandLines( const char *preview ) {
	if ( nm.numQueue ) {
		NM_TextClipped( NM_RIGHT_X, NM_BUTTON_Y - 30, NM_RIGHT_W, va( S_COLOR_YELLOW "Spawning, %i of %i to go (the server takes about one a second)",
			nm.numQueue, nm.queueTotal ), nmWhite );
	} else if ( preview ) {
		NM_TextClipped( NM_RIGHT_X, NM_BUTTON_Y - 30, NM_RIGHT_W, va( S_COLOR_GREY "%s", preview ), nmWhite );
	}
	if ( nm.lastCmd[0] ) {
		NM_TextClipped( NM_RIGHT_X, NM_BUTTON_Y - 17, NM_RIGHT_W,
			va( "%sSent: %s", cls.realtime - nm.sentTime < 1500 ? S_COLOR_GREEN : S_COLOR_GREY, nm.lastCmd ), nmWhite );
	}
}

// the NPCs put down and waiting, and what can be done with them
static void NM_DrawPlaced( void ) {
	const float y = NM_PLACED_Y, by = y + 18, bh = NM_PLACED_ROWS * NM_ROW_H + 4, ay = by + bh + 4;
	const qboolean any = (qboolean)( nm.numPlaced > 0 );
	int remove = -1;

	NM_Label( y, "Put down" );
	if ( NM_Button( NM_CTRL_X, y, 140, "Place several on map...", qfalse, (qboolean)( NM_SelectedType() != NULL ) ) )
		NM_StartPlace();
	NM_TextClipped( NM_CTRL_X + 146, y + 2, NM_CTRL_W - 146,
		any ? va( S_COLOR_YELLOW "%i waiting to spawn", nm.numPlaced ) : S_COLOR_GREY "none waiting", nmWhite );

	NM_Box( NM_RIGHT_X, by, NM_RIGHT_W, bh, nmPanelLight );
	if ( nm.wheel && NM_InRect( NM_RIGHT_X, by, NM_RIGHT_W, bh ) ) {
		nm.placedScroll += nm.wheel;
		nm.wheel = 0;
	}
	nm.placedScroll = Com_Clampi( 0, Q_max( 0, nm.numPlaced - NM_PLACED_ROWS ), nm.placedScroll );
	if ( !any ) {
		NM_TextClipped( NM_RIGHT_X + 4, by + 3, NM_RIGHT_W - 8, S_COLOR_GREY "Put NPCs down on the map, then spawn them all", nmWhite );
		NM_TextClipped( NM_RIGHT_X + 4, by + 3 + NM_ROW_H, NM_RIGHT_W - 8, S_COLOR_GREY "at once, or save them as a construct", nmWhite );
	}
	for ( int i = 0; i < NM_PLACED_ROWS; i++ ) {
		const int row = nm.placedScroll + i;
		const float ry = by + 2 + i * NM_ROW_H, xx = NM_RIGHT_X + NM_RIGHT_W - 20;
		const nmSpawn_t *p;

		if ( row >= nm.numPlaced )
			break;
		p = &nm.placed[row];
		NM_TextClipped( NM_RIGHT_X + 4, ry, NM_RIGHT_W - 28, va( "%s %s" S_COLOR_GREY "at %i %i %i, yaw %i", p->type,
			p->targetname[0] ? va( S_COLOR_CYAN "%s ", p->targetname ) : "", (int)p->pos[0], (int)p->pos[1], (int)p->pos[2], p->yaw ), nmWhite );
		// takes it back
		if ( NM_InRect( xx, ry, 16, NM_ROW_H ) )
			NM_Fill( xx, ry, 16, NM_ROW_H, nmHoverBox );
		NM_Text( xx + 5, ry, S_COLOR_RED "x", nmWhite );
		if ( NM_Clicked( xx, ry, 16, NM_ROW_H ) )
			remove = row;
	}
	if ( remove >= 0 ) {
		memmove( &nm.placed[remove], &nm.placed[remove + 1], ( nm.numPlaced - remove - 1 ) * sizeof( nm.placed[0] ) );
		nm.numPlaced--;
	}

	if ( NM_Button( NM_RIGHT_X, ay, 130, any ? va( "Spawn all %i", nm.numPlaced ) : "Spawn all", qfalse, any ) )
		NM_SpawnPlaced();
	if ( NM_DangerButton( NM_RIGHT_X + 134, ay, 70, "Clear", NM_CONFIRM_CLEAR, any ) )
		nm.numPlaced = nm.placedScroll = 0;
	if ( NM_Button( NM_RIGHT_X + 208, ay, NM_RIGHT_W - 208, "Save as construct...", qfalse, any ) )
		NM_StartSaving( NM_SAVING_CONSTRUCT );
}

// a saved construct's NPCs, in place of the form
static void NM_DrawConstruct( const nmSaved_t *s ) {
	const float lh = NM_PLACED_Y - 6 - ( NM_LIST_Y + 42 );
	const int rows = (int)( ( lh - 4 ) / NM_ROW_H );
	float y = NM_LIST_Y;

	NM_ReadShownConstruct( s );
	NM_Box( NM_RIGHT_X, y, NM_RIGHT_W, 34, nmPanelLight );
	NM_TextClipped( NM_RIGHT_X + 6, y + 4, NM_RIGHT_W - 12, va( "%s " S_COLOR_GREY "NPC construct", s->name ), nmAccent );
	NM_TextClipped( NM_RIGHT_X + 6, y + 18, NM_RIGHT_W - 12, va( S_COLOR_GREY "%i NPCs, from %s", nm.numConstructNpcs, NM_SavePath( qtrue, s->name ) ), nmWhite );

	y += 42;
	NM_Box( NM_RIGHT_X, y, NM_RIGHT_W, lh, nmPanelLight );
	for ( int i = 0; i < nm.numConstructNpcs && i < rows; i++ ) {
		const nmSpawn_t *n = &nm.constructNpcs[i];

		if ( i == rows - 1 && nm.numConstructNpcs > rows ) {
			NM_Text( NM_RIGHT_X + 4, y + 2 + i * NM_ROW_H, va( S_COLOR_GREY "and %i more", nm.numConstructNpcs - i ), nmWhite );
			break;
		}
		NM_TextClipped( NM_RIGHT_X + 4, y + 2 + i * NM_ROW_H, NM_RIGHT_W - 8, va( "%s %s" S_COLOR_GREY "%s%s%s", n->type,
			n->targetname[0] ? va( S_COLOR_CYAN "%s ", n->targetname ) : "", nmTeamLabels[n->team],
			n->model[0] ? ", model " : "", n->model ), nmWhite );
	}
}

static void NM_DrawSpawn( void ) {
	const float bw = ( NM_LEFT_W - 4.0f * ( NM_SRC_COUNT - 1 ) ) / NM_SRC_COUNT;
	const nmType_t *t;
	const nmSaved_t *saved;
	const char *cmd;
	float y, x;
	int picked;

	NM_Field( NM_F_TYPE_SEARCH, NM_LEFT_X, NM_SEARCH_Y, NM_LEFT_W, "Type to search NPC types" );
	// where they come from
	for ( int i = 0; i < NM_SRC_COUNT; i++ ) {
		if ( NM_Button( NM_LEFT_X + i * ( bw + 4 ), NM_LIST_Y, bw, nmSourceLabels[i], (qboolean)( nm.source == i ), qtrue ) ) {
			nm.source = (nmSource_t)i;
			nm.typeList.sel = nm.typeList.scroll = 0;
		}
	}
	NM_RebuildTypeView();
	picked = NM_List( &nm.typeList, nm.numTypeView, NM_LEFT_X, NM_TYPE_LIST_Y, NM_LEFT_W, NM_TYPE_LIST_H, NM_DrawTypeRow );
	if ( !nm.numTypeView ) {
		NM_Text( NM_LEFT_X + 4, NM_TYPE_LIST_Y + 1, nm.source == NM_SRC_SAVED && !nm.fields[NM_F_TYPE_SEARCH][0]
			? "Nothing saved yet" : "No NPC types match", nmDim );
	}
	NM_ApplySelection();
	t = NM_SelectedType();
	saved = NM_SelectedSaved();

	y = NM_SEARCH_Y;
	NM_Label( y, "Show" );
	if ( NM_Button( NM_CTRL_X, y, 80, "All", (qboolean)( nm.show == NM_SHOW_ALL ), qtrue ) )
		nm.show = NM_SHOW_ALL;
	if ( NM_Button( NM_CTRL_X + 84, y, 80, "Characters", (qboolean)( nm.show == NM_SHOW_CHARACTERS ), qtrue ) )
		nm.show = NM_SHOW_CHARACTERS;
	if ( NM_Button( NM_CTRL_X + 168, y, 80, "Vehicles", (qboolean)( nm.show == NM_SHOW_VEHICLES ), qtrue ) )
		nm.show = NM_SHOW_VEHICLES;

	if ( saved && saved->construct ) {
		NM_DrawConstruct( saved );
		NM_DrawPlaced();
		NM_DrawCommandLines( NULL );
		if ( NM_Button( NM_RIGHT_X, NM_BUTTON_Y, NM_RIGHT_W, "Pick it up to put it down...  (Enter)", qfalse, (qboolean)( saved->count > 0 ) ) || picked >= 0 )
			NM_StartConstruct( saved->name );
		return;
	}

	y = NM_LIST_Y;
	NM_Box( NM_RIGHT_X, y, NM_RIGHT_W, 34, nmPanelLight );
	if ( t && saved ) {
		NM_TextClipped( NM_RIGHT_X + 6, y + 4, NM_RIGHT_W - 12, va( "%s " S_COLOR_GREY "saved NPC", saved->name ), nmAccent );
		NM_TextClipped( NM_RIGHT_X + 6, y + 18, NM_RIGHT_W - 12,
			va( S_COLOR_GREY "%s %s   %s", t->name, t->vehicle ? "vehicle" : "character", NM_SourceLabel( t ) ), nmWhite );
	} else if ( t ) {
		NM_TextClipped( NM_RIGHT_X + 6, y + 4, NM_RIGHT_W - 12, t->name, nmAccent );
		NM_TextClipped( NM_RIGHT_X + 6, y + 18, NM_RIGHT_W - 12,
			va( S_COLOR_GREY "%s   model %s   %s", t->vehicle ? "Vehicle" : "Character", t->model[0] ? t->model : "(none)", NM_SourceLabel( t ) ), nmWhite );
	} else {
		NM_Text( NM_RIGHT_X + 6, y + 4, "Pick an NPC type on the left", nmDim );
	}

	y += 44;
	NM_Label( y, "Targetname" );
	NM_Field( NM_F_TARGETNAME, NM_CTRL_X, y, NM_CTRL_W, "optional, names it for later commands" );

	y += 22;
	NM_Label( y, "Team" );
	for ( int i = 0; i < 3; i++ ) {
		if ( NM_Button( NM_CTRL_X + i * 94, y, 90, nmTeamLabels[i], (qboolean)( nm.team == i ), qtrue ) )
			nm.team = (nmTeam_t)i;
	}

	y += 22;
	NM_Label( y, "Model" );
	NM_Field( NM_F_MODEL, NM_CTRL_X, y, NM_CTRL_W - 74, t && t->vehicle ? "ignored on vehicles" : "optional, model or model/skin" );
	if ( NM_Button( NM_CTRL_X + NM_CTRL_W - 70, y, 70, "Browse...", qfalse, (qboolean)( nm.numSkins > 0 ) ) )
		NM_OpenPicker( NM_PICKER_MODEL );

	y += 22;
	NM_Label( y, "Scale" );
	NM_Field( NM_F_SCALE, NM_CTRL_X, y, 80, "default" );

	y += 22;
	NM_Label( y, "Location" );
	if ( NM_Button( NM_CTRL_X, y, 100, "In front of me", (qboolean)!nm.havePos, qtrue ) )
		nm.havePos = qfalse;
	if ( NM_Button( NM_CTRL_X + 104, y, 100, "Pick on map...", nm.havePos, qtrue ) )
		NM_StartPick( NM_PICK_SPAWN );
	if ( nm.havePos ) {
		x = NM_CTRL_X + 210;
		NM_TextClipped( x, y + 2, NM_RIGHT_X + NM_RIGHT_W - x, va( S_COLOR_GREY "%i %i %i, yaw %i",
			(int)nm.pos[0], (int)nm.pos[1], (int)nm.pos[2], nm.yaw ), nmWhite );
	}

	NM_DrawPlaced();

	cmd = NM_BuildSpawn();
	NM_DrawCommandLines( cmd ? va( "Will send: %s", cmd ) : NULL );
	if ( NM_Button( NM_RIGHT_X, NM_BUTTON_Y, NM_RIGHT_W - 188, "Spawn  (Enter)", qfalse, (qboolean)( t != NULL ) ) || picked >= 0 )
		NM_Spawn();
	if ( NM_Button( NM_RIGHT_X + NM_RIGHT_W - 184, NM_BUTTON_Y, 90, "Save NPC...", qfalse, (qboolean)( t != NULL ) ) )
		NM_StartSaving( NM_SAVING_NPC );
	if ( NM_Button( NM_RIGHT_X + NM_RIGHT_W - 90, NM_BUTTON_Y, 90, "Preview...", qfalse, (qboolean)( t != NULL ) ) ) {
		nm.preview = qtrue;
		nm.previewYaw = 0.0f;
		nm.focus = NM_F_NONE;
	}
}

// typing where to teleport to
static void NM_DrawTeleBox( void ) {
	static const char *labels[] = { "X", "Y", "Z", "Yaw" };
	const float w = 300, h = 118, x = ( SCREEN_WIDTH - w ) * 0.5f, y = 160, fw = ( w - 16 - 3 * 6 ) / 4;
	const nmNpc_t *npc = NM_SelectedNpc();
	const char *target = NM_ApplyTarget( qfalse );
	qboolean filled = qtrue;

	NM_Box( x, y, w, h, nmPanelOpaque );
	if ( !target )
		NM_Text( x + 8, y + 6, "Teleport to:", nmWhite );
	else if ( nm.apply == NM_APPLY_NAME )
		NM_TextClipped( x + 8, y + 6, w - 16, va( "Teleport the NPCs named " S_COLOR_YELLOW "%s" S_COLOR_WHITE " to:", target ), nmWhite );
	else
		NM_TextClipped( x + 8, y + 6, w - 16, va( "Teleport NPC " S_COLOR_CYAN "%s" S_COLOR_WHITE "%s to:", target, npc ? va( " (%s)", npc->type ) : "" ), nmWhite );

	for ( int i = 0; i < 4; i++ ) {
		const float fx = x + 8 + i * ( fw + 6 );

		NM_Text( fx, y + 22, labels[i], nmDim );
		NM_Field( (nmField_t)( NM_F_TELE_X + i ), fx, y + 34, fw, i == 3 ? "0" : "" );
		if ( i < 3 && !nm.fields[NM_F_TELE_X + i][0] )
			filled = qfalse;
	}

	// starting points
	if ( NM_Button( x + 8, y + 56, 138, "Where it is now", qfalse, (qboolean)( npc != NULL ) ) ) {
		vec3_t org;

		NM_NpcOrigin( npc, org );
		NM_SetTeleFields( org, npc->yaw );
	}
	if ( NM_Button( x + w - 146, y + 56, 138, "Where I am", qfalse, qtrue ) )
		NM_SetTeleFields( cl.snap.ps.origin, (int)cl.viewangles[YAW] );
	NM_Text( x + 8, y + 76, S_COLOR_GREY "Tab next box   Enter teleports   Esc cancels", nmWhite );

	if ( NM_Button( x + 8, y + h - 24, 120, "Teleport  (Enter)", qfalse, (qboolean)( target && filled ) ) ) {
		NM_TeleportToFields();
		return;
	}
	if ( NM_Button( x + w - 108, y + h - 24, 100, "Cancel  (Esc)", qfalse, qtrue ) ) {
		nm.teleBox = qfalse;
		nm.focus = NM_F_NONE;
		return;
	}
	if ( NM_Clicked( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT ) && !NM_InRect( x, y, w, h ) ) {
		nm.teleBox = qfalse;
		nm.focus = NM_F_NONE;
	}
}

// the name box for saving the form's NPC or what was put down
static void NM_DrawSaving( void ) {
	const float w = 340, h = 88, x = ( SCREEN_WIDTH - w ) * 0.5f, y = 170;
	const qboolean construct = (qboolean)( nm.saving == NM_SAVING_CONSTRUCT );
	const char *name = nm.fields[NM_F_SAVE_NAME];

	NM_Box( x, y, w, h, nmPanelOpaque );
	NM_TextClipped( x + 8, y + 6, w - 16, construct ? va( "Save the %i NPCs put down as a construct named:", nm.numPlaced )
		: "Save the form's NPC, with its settings, as:", nmWhite );
	NM_Field( NM_F_SAVE_NAME, x + 8, y + 22, w - 16, "letters, numbers, - and _" );
	if ( name[0] && FS_FileExists( NM_SavePath( construct, name ) ) )
		NM_TextClipped( x + 8, y + 42, w - 16, S_COLOR_YELLOW "One with this name is there already; saving replaces it", nmWhite );
	else
		NM_TextClipped( x + 8, y + 42, w - 16, va( S_COLOR_GREY "Goes in %s, shown under Saved", construct ? NM_CONSTRUCT_DIR : NM_NPC_DIR ), nmWhite );
	if ( NM_Button( x + 8, y + h - 24, 100, "Save  (Enter)", qfalse, (qboolean)( name[0] != '\0' ) ) ) {
		NM_DoSave();
		return;
	}
	if ( NM_Button( x + w - 108, y + h - 24, 100, "Cancel  (Esc)", qfalse, qtrue ) ) {
		nm.saving = NM_SAVING_NONE;
		return;
	}
	if ( NM_Clicked( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT ) && !NM_InRect( x, y, w, h ) )
		nm.saving = NM_SAVING_NONE;
}

// the NPC as it would spawn; Up/Down go through the types behind it
static void NM_DrawPreview( void ) {
	const float x = NM_PREVIEW_X, y = NM_PREVIEW_Y, w = NM_PREVIEW_W, h = NM_PREVIEW_H;
	const float vx = x + 8, vy = y + 22, vw = w - 16, vh = h - 136;
	const nmType_t *t = NM_SelectedType();
	float ly = vy + vh + 6;

	NM_PreviewUpdate( t, NULL );

	NM_Box( x, y, w, h, nmPanel );
	NM_Fill( x + 1, y + 1, w - 2, h - 2, nmPanel );
	NM_TextClipped( x + 8, y + 6, w - 16, t ? va( "Preview: " S_COLOR_YELLOW "%s", t->name ) : "Preview", nmWhite );
	NM_Box( vx, vy, vw, vh, nmPanelLight );
	if ( nm.wheel && NM_InRect( vx, vy, vw, vh ) ) {
		nm.previewYaw += nm.wheel * 5.0f;	// 15 degrees a notch
		nm.wheel = 0;
	}
	if ( t && nm.previewG2 )
		NM_RenderPreview( t, vx + 1, vy + 1, vw - 2, vh - 2 );
	else if ( nm.previewError )
		NM_TextClipped( vx + 8, vy + 8, vw - 16, nm.previewError, nmRed );

	if ( t ) {
		int scale = NM_PreviewScale( t );

		NM_TextClipped( vx, ly, vw, va( S_COLOR_GREY "Model  " S_COLOR_WHITE "%s%s", nm.previewModel,
			nm.previewOwnModel ? S_COLOR_GREY "  (its own)" : "" ), nmWhite );
		ly += NM_ROW_H;
		if ( nm.previewSkinMissing ) {
			NM_TextClipped( vx, ly, vw, "Skin not found, showing the default one", nmRed );
			ly += NM_ROW_H;
		}
		NM_TextClipped( vx, ly, vw, va( S_COLOR_GREY "Scale  " S_COLOR_WHITE "%i%%", scale ), nmWhite );
		ly += NM_ROW_H;
		if ( t->rgba[0] != 255 || t->rgba[1] != 255 || t->rgba[2] != 255 ) {
			NM_TextClipped( vx, ly, vw, va( S_COLOR_GREY "Tint   " S_COLOR_WHITE "%i %i %i", t->rgba[0], t->rgba[1], t->rgba[2] ), nmWhite );
			ly += NM_ROW_H;
		}
		if ( !t->vehicle && t->weapon[0] && Q_stricmp( t->weapon, "WP_NONE" ) ) {
			const char *held = t->weapon;

			if ( !Q_stricmp( t->weapon, "WP_SABER" ) )
				held = va( "WP_SABER  %s%s%s", t->saber[0] ? t->saber : "Kyle", t->saber2[0] ? " + " : "", t->saber2 );
			NM_TextClipped( vx, ly, vw, va( S_COLOR_GREY "Weapon " S_COLOR_WHITE "%s", held ), nmWhite );
			ly += NM_ROW_H;
		}
	}
	NM_Text( vx, y + h - 40, S_COLOR_GREY "Wheel turns it   Up/Down change type", nmWhite );

	if ( NM_Button( x + 8, y + h - 24, 120, "Spawn  (Enter)", qfalse, (qboolean)( t != NULL ) ) ) {
		NM_Spawn();
		NM_ClosePreview();
		return;
	}
	if ( NM_Button( x + w - 108, y + h - 24, 100, "Close  (Esc)", qfalse, qtrue ) ) {
		NM_ClosePreview();
		return;
	}
	if ( NM_Clicked( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT ) && !NM_InRect( x, y, w, h ) )
		NM_ClosePreview();
}

static void NM_DrawManage( void ) {
	const nmNpc_t *npc;
	const char *target, *targetAll;
	float y, w;
	int named;
	vec3_t org;

	NM_Field( NM_F_NPC_SEARCH, NM_LEFT_X, NM_SEARCH_Y, NM_LEFT_W, "Type to filter by number, type or name" );
	NM_List( &nm.npcList, nm.numNpcView, NM_LEFT_X, NM_LIST_Y, NM_LEFT_W, NM_LIST_H, NM_DrawNpcRow );
	if ( nm.npcList.sel < nm.numNpcView )
		nm.selNum = nm.npcs[nm.npcView[nm.npcList.sel]].num;
	if ( !nm.numNpcView ) {
		NM_Text( NM_LEFT_X + 4, NM_LIST_Y + 1,
			nm.listRequestTime ? "Asking the server..." : nm.numNpcs ? "No NPCs match" : "No NPCs in the map", nmDim );
	}
	npc = NM_SelectedNpc();

	y = NM_SEARCH_Y;
	if ( NM_Button( NM_RIGHT_X, y, 80, "Refresh", qfalse, qtrue ) )
		NM_RequestList();
	if ( NM_Button( NM_RIGHT_X + 84, y, 100, "Select on map...", qfalse, (qboolean)( nm.numNpcs > 0 ) ) )
		NM_StartPick( NM_PICK_SELECT );
	if ( nm.listTime ) {
		NM_TextClipped( NM_RIGHT_X + 192, y + 2, NM_RIGHT_W - 192,
			va( S_COLOR_GREY "%i NPCs, %is ago", nm.numNpcs, ( cls.realtime - nm.listTime ) / 1000 ), nmWhite );
	}

	y = NM_LIST_Y;
	NM_Box( NM_RIGHT_X, y, NM_RIGHT_W, 34, nmPanelLight );
	if ( npc ) {
		NM_NpcOrigin( npc, org );
		NM_TextClipped( NM_RIGHT_X + 6, y + 4, NM_RIGHT_W - 12,
			va( S_COLOR_CYAN "%i  " S_COLOR_YELLOW "%s  " S_COLOR_WHITE "%s", npc->num, npc->type, npc->name[0] ? npc->name : S_COLOR_GREY "(no targetname)" ), nmWhite );
		NM_TextClipped( NM_RIGHT_X + 6, y + 18, NM_RIGHT_W - 12, va( S_COLOR_GREY "at %i %i %i, yaw %i, %i units from you",
			(int)org[0], (int)org[1], (int)org[2], npc->yaw, (int)Distance( org, cl.snap.ps.origin ) ), nmWhite );
	} else {
		NM_Text( NM_RIGHT_X + 6, y + 4, "Pick an NPC on the left, or on the map", nmDim );
	}

	// what the commands run on
	y += 42;
	named = npc && npc->name[0] ? NM_CountNamed( npc->name ) : 0;
	NM_Label( y, "Apply to" );
	if ( NM_Button( NM_CTRL_X, y, 80, "This NPC", (qboolean)( nm.apply == NM_APPLY_ONE ), (qboolean)( npc != NULL ) ) )
		nm.apply = NM_APPLY_ONE;
	if ( NM_Button( NM_CTRL_X + 84, y, 110, named ? va( "Named alike (%i)", named ) : "Named alike", (qboolean)( nm.apply == NM_APPLY_NAME ), (qboolean)( named > 0 ) ) )
		nm.apply = NM_APPLY_NAME;
	if ( NM_Button( NM_CTRL_X + 198, y, 82, "All NPCs", (qboolean)( nm.apply == NM_APPLY_ALL ), qtrue ) )
		nm.apply = NM_APPLY_ALL;
	target = NM_ApplyTarget( qfalse );
	targetAll = NM_ApplyTarget( qtrue );

	w = 66;
	y += 24;
	NM_Label( y, "Kill" );
	if ( NM_DangerButton( NM_CTRL_X, y, w * 2 + 4, nm.apply == NM_APPLY_ALL ? "Kill all NPCs" : "Kill", NM_CONFIRM_KILL, (qboolean)( targetAll != NULL ) ) )
		NM_SendManage( va( "npc kill %s", targetAll ), qtrue );
	if ( NM_DangerButton( NM_CTRL_X + ( w + 4 ) * 2, y, w * 2 + 4, "Kill all spawners", NM_CONFIRM_SPAWNERS, qtrue ) )
		NM_SendManage( "npc kill spawners", qtrue );

	y += 20;
	NM_Label( y, "Freeze" );
	if ( NM_Button( NM_CTRL_X, y, w, "Freeze", qfalse, (qboolean)( targetAll != NULL ) ) )
		NM_SendManage( va( "npc freeze %s 1", targetAll ), qfalse );
	if ( NM_Button( NM_CTRL_X + w + 4, y, w, "Unfreeze", qfalse, (qboolean)( targetAll != NULL ) ) )
		NM_SendManage( va( "npc freeze %s 0", targetAll ), qfalse );
	if ( NM_Button( NM_CTRL_X + ( w + 4 ) * 2, y, w, "Toggle", qfalse, (qboolean)( targetAll != NULL ) ) )
		NM_SendManage( va( "npc freeze %s !", targetAll ), qfalse );

	y += 20;
	NM_Label( y, "Emote" );
	NM_Field( NM_F_EMOTE, NM_CTRL_X, y, w * 2 + 4, "emote name" );
	if ( NM_Button( NM_CTRL_X + ( w + 4 ) * 2, y, w, "List...", qfalse, qtrue ) )
		NM_OpenPicker( NM_PICKER_EMOTE );
	if ( NM_Button( NM_CTRL_X + ( w + 4 ) * 3, y, 40, "Set", qfalse, (qboolean)( targetAll != NULL && nm.fields[NM_F_EMOTE][0] ) ) )
		NM_SendManage( va( "npc emote %s %s", nm.fields[NM_F_EMOTE], targetAll ), qfalse );
	if ( NM_Button( NM_CTRL_X + ( w + 4 ) * 3 + 44, y, 40, "Stop", qfalse, (qboolean)( targetAll != NULL ) ) )
		NM_SendManage( va( "npc emote none %s", targetAll ), qfalse );

	y += 20;
	NM_Label( y, "Follow" );
	if ( NM_Button( NM_CTRL_X, y, w, "Me", qfalse, (qboolean)( targetAll != NULL ) ) )
		NM_SendManage( va( "npc follow %i %s", clc.clientNum, targetAll ), qfalse );
	if ( NM_Button( NM_CTRL_X + w + 4, y, w, "Player...", qfalse, (qboolean)( targetAll != NULL ) ) )
		NM_OpenPicker( NM_PICKER_PLAYER );
	if ( NM_Button( NM_CTRL_X + ( w + 4 ) * 2, y, w, "Stop", qfalse, (qboolean)( targetAll != NULL ) ) )
		NM_SendManage( va( "npc follow none %s", targetAll ), qfalse );

	y += 20;
	NM_Label( y, "Dialog" );
	NM_Field( NM_F_DIALOG, NM_CTRL_X, y, w * 2 + 4, "dialog name" );
	if ( NM_Button( NM_CTRL_X + ( w + 4 ) * 2, y, w, "Set", qfalse, (qboolean)( targetAll != NULL && nm.fields[NM_F_DIALOG][0] ) ) )
		NM_SendManage( va( "npc dialog %s %s", nm.fields[NM_F_DIALOG], targetAll ), qfalse );
	if ( NM_Button( NM_CTRL_X + ( w + 4 ) * 3, y, w, "None", qfalse, (qboolean)( targetAll != NULL ) ) )
		NM_SendManage( va( "npc dialog none %s", targetAll ), qfalse );

	y += 20;
	NM_Label( y, "Teleport" );
	if ( NM_Button( NM_CTRL_X, y, 88, "Pick on map...", qfalse, (qboolean)( target != NULL ) ) )
		NM_StartPick( NM_PICK_TELE );
	if ( NM_Button( NM_CTRL_X + 92, y, 88, "In front of me", qfalse, (qboolean)( target != NULL ) ) ) {
		vec3_t pos;
		int yaw;

		NM_InFrontOfPlayer( pos, &yaw );
		NM_Teleport( pos, yaw );
	}
	if ( NM_Button( NM_CTRL_X + 184, y, NM_CTRL_W - 184, "Coordinates...", qfalse, (qboolean)( target != NULL ) ) )
		NM_OpenTeleBox();
	if ( nm.apply == NM_APPLY_ALL )
		NM_Text( NM_CTRL_X + ( w + 4 ) * 4, y + 2, S_COLOR_GREY "not on all", nmWhite );

	y += 20;
	NM_Label( y, "Hologram" );
	if ( NM_Button( NM_CTRL_X, y, w, "Normal", qfalse, (qboolean)( targetAll != NULL ) ) )
		NM_SendManage( va( "npc hologram normal %s", targetAll ), qfalse );
	if ( NM_Button( NM_CTRL_X + w + 4, y, w, "Projector", qfalse, (qboolean)( targetAll != NULL ) ) )
		NM_SendManage( va( "npc hologram projector %s", targetAll ), qfalse );
	if ( NM_Button( NM_CTRL_X + ( w + 4 ) * 2, y, w, "Off", qfalse, (qboolean)( targetAll != NULL ) ) )
		NM_SendManage( va( "npc hologram off %s", targetAll ), qfalse );

	y += 20;
	NM_Label( y, "Score" );
	if ( NM_Button( NM_CTRL_X, y, w * 2 + 4, "Show kills", qfalse, qtrue ) ) {
		// only takes a targetname, everything otherwise
		if ( nm.apply == NM_APPLY_NAME && npc && npc->name[0] )
			NM_SendManage( va( "npc score %s", npc->name ), qfalse );
		else
			NM_SendManage( "npc score", qfalse );
	}

	NM_DrawCommandLines( NULL );
}

// the model highlighted in the model picker, with the selected type's scale and tint
static void NM_DrawPickerPreview( float x, float y, float w, float h ) {
	static nmType_t plain;
	const nmType_t *t = NM_SelectedType(), *looks = t;
	const char *pick = nm.pickerList.sel < nm.numPickerView ? NM_PickerItem( nm.pickerView[nm.pickerList.sel] ) : NULL;

	// vehicles ignore the model box, so show the model as a character would wear it
	if ( !looks || looks->vehicle ) {
		plain.name = "";
		plain.rgba[0] = plain.rgba[1] = plain.rgba[2] = plain.rgba[3] = 255;
		plain.scale = 100;
		plain.weapon = plain.saber = plain.saber2 = "";
		looks = &plain;
	}

	NM_Box( x, y, w, h, nmPanelLight );
	if ( !pick ) {
		NM_PreviewFree();
		return;
	}
	NM_PreviewUpdate( looks, pick );
	if ( nm.wheel && NM_InRect( x, y, w, h ) ) {
		nm.previewYaw += nm.wheel * 5.0f;	// 15 degrees a notch
		nm.wheel = 0;
	}
	if ( nm.previewG2 )
		NM_RenderPreview( looks, x + 1, y + 1, w - 2, h - 2 );
	else if ( nm.previewError )
		NM_TextClipped( x + 6, y + 6, w - 12, nm.previewError, nmRed );
	if ( nm.previewSkinMissing )
		NM_TextClipped( x + 6, y + h - 16, w - 12, "Skin not found, showing default", nmRed );
}

static void NM_DrawPicker( void ) {
	static const char *titles[] = { "", "Pick a model", "Pick an emote", "Pick a player to follow" };
	// the model picker is wider, with a preview of the highlighted model on the right
	const qboolean models = (qboolean)( nm.picker == NM_PICKER_MODEL );
	const float x = models ? NM_PICKER_MODELS_X : NM_PICKER_X, y = NM_PICKER_Y, h = NM_PICKER_H;
	const float w = NM_PICKER_W + ( models ? NM_PICKER_PREVIEW_W : 0.0f ), lw = NM_PICKER_W - 16;
	int picked;

	NM_Box( x, y, w, h, nmPanel );
	NM_Fill( x + 1, y + 1, w - 2, h - 2, nmPanel );
	NM_Text( x + 8, y + 6, titles[nm.picker], nmWhite );
	NM_Field( NM_F_PICKER_SEARCH, x + 8, y + 22, lw, "Type to search" );
	if ( nm.picker == NM_PICKER_PLAYER )
		NM_RebuildPickerView();	// people come and go
	picked = NM_List( &nm.pickerList, nm.numPickerView, x + 8, y + 42, lw, h - 74, NM_DrawPickerRow );
	if ( models )
		NM_DrawPickerPreview( x + lw + 16, y + 22, NM_PICKER_PREVIEW_W - 8, h - 54 );
	if ( NM_Button( x + 8, y + h - 24, 100, "Choose  (Enter)", qfalse, (qboolean)( nm.numPickerView > 0 ) ) )
		picked = nm.pickerList.sel;
	if ( NM_Button( x + w - 108, y + h - 24, 100, "Cancel  (Esc)", qfalse, qtrue ) ) {
		nm.picker = NM_PICKER_NONE;
		return;
	}
	if ( picked >= 0 && picked < nm.numPickerView )
		NM_ChoosePicker( nm.pickerView[picked] );
	else if ( NM_Clicked( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT ) && !NM_InRect( x, y, w, h ) )
		nm.picker = NM_PICKER_NONE;
}

// numbers and names over the NPCs in view
static void NM_DrawLabels( void ) {
	// picking one, the label under the crosshair lights up
	const int hovered = nm.pick == NM_PICK_SELECT ? NM_NpcAtCursor() : -1;
	float x, y, tw;
	vec3_t org;

	for ( int i = 0; i < nm.numNpcs; i++ ) {
		const nmNpc_t *n = &nm.npcs[i];
		const char *label = n->name[0] ? va( "%i %s", n->num, n->name ) : va( "%i %s", n->num, n->type );
		qboolean sel = (qboolean)( n->num == nm.selNum || i == hovered );

		NM_NpcOrigin( n, org );
		org[2] += 40.0f;
		if ( !NM_Project( org, &x, &y ) || x < 0 || x > SCREEN_WIDTH || y < 0 || y > SCREEN_HEIGHT )
			continue;
		tw = NM_TextWidth( label );
		NM_Box( x - tw * 0.5f - 3, y - 7, tw + 6, 14, sel ? nmHighlight : nmPanel );
		NM_Text( x - tw * 0.5f, y - 6, label, sel ? nmAccent : nmWhite );
	}
}

// a dotted line on screen, the dots as close however long it is
static void NM_DottedLine( float x1, float y1, float x2, float y2, const float *color ) {
	const int dots = Com_Clampi( 2, 120, (int)( sqrtf( Square( x2 - x1 ) + Square( y2 - y1 ) ) / 4.0f ) );

	for ( int i = 0; i <= dots; i++ )
		NM_Fill( x1 + ( x2 - x1 ) * i / dots - 1, y1 + ( y2 - y1 ) * i / dots - 1, 2, 2, color );
}

// a spot an NPC goes: a pole from its feet to its head, which way it faces, and a label
static void NM_DrawMarker( const vec3_t pos, int yaw, const char *label, const float *color ) {
	vec3_t feet, head, angles, forward, end;
	float fx, fy, hx, hy, x, y, ex, ey, tw;

	VectorCopy( pos, feet );
	feet[2] -= NM_GROUND_HEIGHT;
	VectorCopy( pos, head );
	head[2] += 40.0f;
	if ( !NM_Project( feet, &fx, &fy ) || !NM_Project( head, &hx, &hy ) || !NM_Project( pos, &x, &y ) )
		return;
	NM_DottedLine( fx, fy, hx, hy, color );
	VectorSet( angles, 0, (float)yaw, 0 );
	AngleVectors( angles, forward, NULL, NULL );
	VectorMA( pos, 32.0f, forward, end );
	if ( NM_Project( end, &ex, &ey ) )
		NM_DottedLine( x, y, ex, ey, color );
	tw = NM_TextWidth( label );
	NM_Box( hx - tw * 0.5f - 3, hy - 16, tw + 6, 14, nmPanel );
	NM_Text( hx - tw * 0.5f, hy - 15, label, color );
}

static const char *NM_MarkerLabel( const nmSpawn_t *s ) {
	return s->targetname[0] ? va( "%s %s", s->type, s->targetname ) : s->type;
}

static void NM_DrawPick( void ) {
	const char *what;
	vec3_t pos, end, angles, forward;
	float x, y, ex, ey;
	int yaw;

	NM_DrawLabels();
	// the ones put down, waiting
	for ( int i = 0; i < nm.numPlaced; i++ )
		NM_DrawMarker( nm.placed[i].pos, nm.placed[i].yaw, NM_MarkerLabel( &nm.placed[i] ), nmPlacedColor );

	if ( nm.pick == NM_PICK_SELECT ) {
		what = "Aim at an NPC's label and click to select it.   Right-click or Esc cancels";
	} else if ( nm.pick == NM_PICK_PLACE ) {
		// what a click puts down, and where
		if ( NM_TraceCursor( pos ) ) {
			for ( int i = 0; i < nm.numHand; i++ ) {
				vec3_t spot;

				NM_HandSpot( i, pos, spot, &yaw );
				NM_DrawMarker( spot, yaw, NM_MarkerLabel( &nm.hand[i] ), nmAccent );
			}
		}
		what = va( "Click to put down %s%s, again for more.   Wheel turns it.   Backspace takes the last back",
			nm.handConstruct ? "the construct " : "", nm.handName );
	} else {
		if ( NM_TraceCursor( pos ) && NM_Project( pos, &x, &y ) ) {
			// where it goes and which way it will face
			yaw = NM_NormalizeYaw( NM_YawToView( pos ) + nm.pickYaw );
			VectorSet( angles, 0, (float)yaw, 0 );
			AngleVectors( angles, forward, NULL, NULL );
			VectorMA( pos, 32.0f, forward, end );
			NM_Fill( x - 3, y - 3, 6, 6, nmAccent );
			if ( NM_Project( end, &ex, &ey ) ) {
				for ( int i = 1; i <= 8; i++ )
					NM_Fill( x + ( ex - x ) * i / 8.0f - 1, y + ( ey - y ) * i / 8.0f - 1, 2, 2, nmAccent );
			}
		}
		what = nm.pick == NM_PICK_SPAWN
			? "Aim and click where to spawn it.   Wheel turns it.   Right-click or Esc cancels"
			: "Aim and click where to teleport to.   Wheel turns it.   Right-click or Esc cancels";
	}

	NM_Box( 10, 10, SCREEN_WIDTH - 20, nm.pick == NM_PICK_PLACE ? 43 : 30, nmPanel );
	NM_TextClipped( 16, 13, SCREEN_WIDTH - 32, what, nmWhite );
	NM_TextClipped( 16, 26, SCREEN_WIDTH - 32, S_COLOR_GREY "Mouse looks   WASD fly   Space/C up/down   Shift faster   Ctrl slower", nmWhite );
	if ( nm.pick == NM_PICK_PLACE ) {
		NM_TextClipped( 16, 39, SCREEN_WIDTH - 32, va( "%s" S_COLOR_WHITE "   Enter spawns them all   Right-click or Esc goes back, keeping them",
			nm.numPlaced ? va( S_COLOR_GREEN "%i put down", nm.numPlaced ) : S_COLOR_GREY "None put down yet" ), nmWhite );
	}

	// the crosshair, what a click picks
	NM_Fill( nm.cursorX - 8, nm.cursorY - 0.5f, 6, 1, nmWhite );
	NM_Fill( nm.cursorX + 2, nm.cursorY - 0.5f, 6, 1, nmWhite );
	NM_Fill( nm.cursorX - 0.5f, nm.cursorY - 8, 1, 6, nmWhite );
	NM_Fill( nm.cursorX - 0.5f, nm.cursorY + 2, 1, 6, nmWhite );
}

// drawn over the cgame, under the UI and console
void CL_NpcManager_Draw( void ) {
	char buf[MAX_STRING_CHARS];

	if ( !nm.open )
		return;

	if ( !nm.preview && nm.picker != NM_PICKER_MODEL && nm.previewG2 )
		NM_PreviewFree();

	if ( nm.pick != NM_PICK_NONE ) {
		NM_DrawPick();
		return;
	}

	// the picker, the name box and the preview sit on top and get the clicks first, but are drawn last
	qboolean pickerOpen = (qboolean)( nm.picker != NM_PICKER_NONE );
	qboolean savingOpen = (qboolean)( nm.saving != NM_SAVING_NONE && !pickerOpen );
	qboolean teleOpen = (qboolean)( nm.teleBox && !pickerOpen && !savingOpen );
	qboolean previewOpen = (qboolean)( nm.preview && !pickerOpen && !savingOpen && !teleOpen );
	qboolean click = nm.click;
	int wheel = nm.wheel;
	if ( pickerOpen || savingOpen || teleOpen || previewOpen )
		nm.click = qfalse, nm.wheel = 0;

	NM_Fill( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, nmPanel );
	NM_Text( NM_LEFT_X, 14, "NPC Manager", nmWhite );
	if ( NM_Button( 110, 12, 70, "Spawn", (qboolean)( nm.tab == NM_TAB_SPAWN ), qtrue ) )
		NM_SetTab( NM_TAB_SPAWN );
	if ( NM_Button( 184, 12, 70, "Manage", (qboolean)( nm.tab == NM_TAB_MANAGE ), qtrue ) )
		NM_SetTab( NM_TAB_MANAGE );

	// the server's answer to the last command
	if ( nm.reply[0] && cls.realtime - nm.replyTime < 8000 )
		NM_TextClipped( NM_RIGHT_X, 14, NM_RIGHT_W, va( S_COLOR_YELLOW "Server: " S_COLOR_WHITE "%s", nm.reply ), nmWhite );
	// and the manager's own
	if ( nm.message[0] && cls.realtime - nm.messageTime < NM_MESSAGE_MS )
		NM_TextClipped( NM_RIGHT_X, 29, NM_RIGHT_W, nm.message, nmWhite );

	if ( nm.tab == NM_TAB_SPAWN )
		NM_DrawSpawn();
	else
		NM_DrawManage();

	NM_Text( NM_LEFT_X, 440, S_COLOR_GREY "Up/Down select   Wheel scroll   Tab switch tab   Click a box to type in it", nmWhite );
	Com_sprintf( buf, sizeof( buf ), S_COLOR_GREY "Esc back / close   %s",
		Key_GetKey( NM_TOGGLE_CMD ) >= 0 ? "Your npcmanager bind closes" : "Tip: bind a key to npcmanager" );
	NM_Text( NM_LEFT_X, 452, buf, nmWhite );

	if ( pickerOpen && nm.picker != NM_PICKER_NONE ) {
		nm.click = click;
		nm.wheel = wheel;
		NM_DrawPicker();
	} else if ( savingOpen && nm.saving != NM_SAVING_NONE ) {
		nm.click = click;
		nm.wheel = wheel;
		NM_DrawSaving();
	} else if ( teleOpen && nm.teleBox && nm.tab == NM_TAB_MANAGE ) {
		nm.click = click;
		nm.wheel = wheel;
		NM_DrawTeleBox();
	} else if ( previewOpen && nm.preview && nm.tab == NM_TAB_SPAWN ) {
		nm.click = click;
		nm.wheel = wheel;
		NM_DrawPreview();
	}

	// a click on nothing leaves the text box
	if ( nm.click )
		nm.focus = NM_F_NONE;
	nm.click = qfalse;
	nm.wheel = 0;

	re->DrawStretchPic( nm.cursorX, nm.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}
