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

// cl_storyteller.cpp -- assemble a mission out of steps, then play it
//
// A story is a list of steps run one after the other: waits, a stop until the
// storyteller says go on, chat, console commands, and the RPMod commands the
// managers send (npc spawn/tele/kill/..., rpmodel add, rpeffect, rpshader remap).
// The panel has the actions to pick from, the story, and the selected step's
// settings. Commands go out paced by the server's flood protection, and a story
// keeps playing with the panel closed. Stories are saved in storyteller/*.story,
// one step a line: its action, 1 or 0 for whether it runs, and its settings.
//
// "When someone comes" waits for a player or NPC to come into a map trigger, or near a
// spot or an effect, or for a door or button to move. A story starting with it can be
// armed: a copy of it waits in the background and plays when that happens, once or
// every time, several at once. Only who the server sends this client can be seen, so
// the storyteller has to be near enough; the server could do it everywhere one day.
//
// This lives in the engine rather than cgame so it works whatever cgame the
// server's mod ships (RPMod loads its own).

#include "client.h"
#include "qcommon/cm_public.h"

#define ST_TOGGLE_CMD		"storyteller"
#define ST_SAVE_DIR			"storyteller"		// in the game folder
#define ST_SAVE_EXT			".story"

#define ST_MAX_STEPS		512
#define ST_MAX_FIELDS		10
#define ST_VALUE_LEN		192
#define ST_MAX_STORIES		512
#define ST_MESSAGE_MS		5000
#define ST_CONFIRM_MS		3000
#define ST_DOUBLECLICK_MS	400
#define ST_TRACE_DIST		8192.0f
#define ST_GROUND_HEIGHT	25.0f	// NPC origins sit this far above the floor

// layout, in 640x480
#define ST_ROW_H			13.0f
#define ST_CTRL_H			15.0f
#define ST_TEXT_SCALE		0.8f
#define ST_TOP_Y			46.0f
#define ST_ACTIONS_X		16.0f
#define ST_ACTIONS_W		150.0f
#define ST_ACTIONS_H		250.0f
#define ST_STEPS_X			176.0f
#define ST_STEPS_W			218.0f
#define ST_STEPS_H			330.0f
#define ST_EDIT_X			404.0f
#define ST_EDIT_W			220.0f
#define ST_EDIT_LABEL_W		64.0f
#define ST_BUTTON_Y			410.0f
#define ST_POPUP_X			170.0f
#define ST_POPUP_W			300.0f

typedef enum {
	ST_K_TEXT,
	ST_K_NUMBER,
	ST_K_CHOICE		// cycles through choices, '|' separated
} stKind_t;

typedef struct {
	const char	*label;
	stKind_t	kind;
	const char	*def;
	const char	*choices;
} stFieldDef_t;

typedef enum {
	ST_A_WAIT,
	ST_A_PAUSE,
	ST_A_TRIGGER,
	ST_A_COMMAND,
	ST_A_SAY,
	ST_A_CENTER,
	ST_A_NPC_TALK,
	ST_A_OBJECTIVE,
	ST_A_NPC_SPAWN,
	ST_A_NPC_TELE,
	ST_A_NPC_CMD,
	ST_A_MODEL,
	ST_A_EFFECT,
	ST_A_SCREEN,
	ST_A_SHADER,
	ST_A_SOUND,
	ST_A_MUSIC,
	ST_A_CIN_START,
	ST_A_CAM_SHOT,
	ST_A_CAM_FOV,
	ST_A_CIN_BARS,
	ST_A_CIN_END,
	ST_NUM_ACTIONS
} stAction_t;

typedef struct {
	const char		*key;			// in story files
	const char		*name;
	const char		*category;
	const char		*desc;
	int				posField;		// first of X Y Z, -1 if it has no spot
	int				yawField;		// -1 if it has none
	int				numFields;
	stFieldDef_t	fields[ST_MAX_FIELDS];
} stActionDef_t;

#define ST_NPC_COMMANDS		"kill|freeze|unfreeze|emote|stop emote|follow me|stop following|dialog|no dialog|hologram|projector|no hologram"

static const stActionDef_t stActions[ST_NUM_ACTIONS] = {
	{ "wait", "Wait", "Flow", "Waits this many seconds before the next step.", -1, -1, 1, {
		{ "Seconds", ST_K_NUMBER, "2", NULL } } },
	{ "pause", "Wait for me", "Flow", "Stops until you press Continue, or run /storyteller continue (bind it to a key). The note shows while it waits.", -1, -1, 1, {
		{ "Note", ST_K_TEXT, "", NULL } } },
	{ "trigger", "When someone comes", "Flow", "Waits until someone comes into a map trigger or near a spot or an effect, or a door moves. A story starting with it can be armed. Only people near you are seen.", 5, -1, 8, {
		{ "Where", ST_K_CHOICE, "map trigger", "map trigger|near a spot|near an effect|door or button" },
		{ "Which", ST_K_TEXT, "", NULL },
		{ "Who", ST_K_CHOICE, "any player", "any player|any NPC|anyone|me|named" },
		{ "Name", ST_K_TEXT, "", NULL },
		{ "Radius", ST_K_NUMBER, "96", NULL },
		{ "X", ST_K_NUMBER, "", NULL },
		{ "Y", ST_K_NUMBER, "", NULL },
		{ "Z", ST_K_NUMBER, "", NULL } } },
	{ "command", "Run a command", "Flow", "Runs it as if typed in the console, e.g. rpcinematic intro, or exec mission.cfg. Separate several with ;", -1, -1, 1, {
		{ "Command", ST_K_TEXT, "", NULL } } },
	{ "say", "Say", "Speech", "Says this in chat, as you.", -1, -1, 1, {
		{ "Text", ST_K_TEXT, "", NULL } } },
	{ "center", "Center message", "Speech", "Shows the text in the middle of the screen with rpcentersay, for everyone (all), or one player. 0 seconds keeps each player's own setting.", -1, -1, 3, {
		{ "Who", ST_K_TEXT, "all", NULL },
		{ "Seconds", ST_K_NUMBER, "3", NULL },
		{ "Text", ST_K_TEXT, "", NULL } } },
	{ "npctalk", "NPC talk", "Speech", "The NPC says it in chat, as a player would, for everyone with this client (sent hidden in an rpnotify; others see the raw line in their console). The emote is played on the NPC with npc emote.", -1, -1, 5, {
		{ "NPC", ST_K_TEXT, "", NULL },
		{ "Name", ST_K_TEXT, "", NULL },
		{ "Name color", ST_K_TEXT, "^7", NULL },
		{ "Emote", ST_K_TEXT, "none", NULL },
		{ "Text", ST_K_TEXT, "", NULL } } },
	{ "objective", "Objective", "Speech", "Adds to the mission's objectives, listed on the left while Esc is up, for everyone with this client (sent hidden in an rpnotify). Done strikes one through; it and undone and remove take its number or its text. Clear takes them all, or with done only the struck ones.", -1, -1, 2, {
		{ "Do", ST_K_CHOICE, "add", "add|done|undone|remove|clear|title" },
		{ "Text", ST_K_TEXT, "", NULL } } },
	{ "npcspawn", "Spawn NPC", "NPCs", "Spawns an NPC at the spot, as the NPC manager does. Give it a targetname to command it in later steps.", 6, 9, 10, {
		{ "Type", ST_K_TEXT, "", NULL },
		{ "Targetname", ST_K_TEXT, "", NULL },
		{ "Team", ST_K_CHOICE, "default", "default|player|enemy" },
		{ "Vehicle", ST_K_CHOICE, "no", "no|yes" },
		{ "Model", ST_K_TEXT, "", NULL },
		{ "Scale", ST_K_NUMBER, "", NULL },
		{ "X", ST_K_NUMBER, "", NULL },
		{ "Y", ST_K_NUMBER, "", NULL },
		{ "Z", ST_K_NUMBER, "", NULL },
		{ "Yaw", ST_K_NUMBER, "0", NULL } } },
	{ "npctele", "Teleport NPC", "NPCs", "Teleports the NPCs with this targetname (or number, or all) to the spot.", 1, 4, 5, {
		{ "Target", ST_K_TEXT, "", NULL },
		{ "X", ST_K_NUMBER, "", NULL },
		{ "Y", ST_K_NUMBER, "", NULL },
		{ "Z", ST_K_NUMBER, "", NULL },
		{ "Yaw", ST_K_NUMBER, "0", NULL } } },
	{ "npccmd", "NPC command", "NPCs", "Runs an npc command on the NPCs with this targetname (or number, or all). Emote and dialog take a value.", -1, -1, 3, {
		{ "Target", ST_K_TEXT, "", NULL },
		{ "Command", ST_K_CHOICE, "kill", ST_NPC_COMMANDS },
		{ "Value", ST_K_TEXT, "", NULL } } },
	{ "model", "Place model", "World", "Places a model at the spot with rpmodel add, as the model manager does. Scale is in percent.", 1, 5, 9, {
		{ "Model", ST_K_TEXT, "", NULL },
		{ "X", ST_K_NUMBER, "", NULL },
		{ "Y", ST_K_NUMBER, "", NULL },
		{ "Z", ST_K_NUMBER, "", NULL },
		{ "Pitch", ST_K_NUMBER, "0", NULL },
		{ "Yaw", ST_K_NUMBER, "0", NULL },
		{ "Roll", ST_K_NUMBER, "0", NULL },
		{ "Scale", ST_K_NUMBER, "100", NULL },
		{ "Solid", ST_K_CHOICE, "yes", "yes|no" } } },
	{ "effect", "Play effect", "World", "Plays an effect at the spot with rpeffect, once or looping.", 2, 6, 8, {
		{ "Effect", ST_K_TEXT, "", NULL },
		{ "Play", ST_K_CHOICE, "once", "once|loop" },
		{ "X", ST_K_NUMBER, "", NULL },
		{ "Y", ST_K_NUMBER, "", NULL },
		{ "Z", ST_K_NUMBER, "", NULL },
		{ "Pitch", ST_K_NUMBER, "0", NULL },
		{ "Yaw", ST_K_NUMBER, "0", NULL },
		{ "Roll", ST_K_NUMBER, "0", NULL } } },
	{ "screen", "Screen effect", "World", "RPMod's alarm, fade out and camera shake, for everyone. Intensity and duration are for the shake.", -1, -1, 3, {
		{ "Effect", ST_K_CHOICE, "fade out", "fade out|fade in|alarm|stop alarm|camera shake" },
		{ "Intensity", ST_K_NUMBER, "5", NULL },
		{ "Duration", ST_K_NUMBER, "2", NULL } } },
	{ "shader", "Remap shader", "World", "Swaps a shader for another with rpshader remap, as the shader manager does.", -1, -1, 3, {
		{ "From", ST_K_TEXT, "", NULL },
		{ "To", ST_K_TEXT, "", NULL },
		{ "Time offset", ST_K_NUMBER, "", NULL } } },
	{ "sound", "Play sound", "World", "Plays a sound with rpsound: once, looping (until removed) or local, to everyone, you, a player or at a spot. Pick it in the sound manager to hear it first.", 4, -1, 7, {
		{ "Sound", ST_K_TEXT, "", NULL },
		{ "Play", ST_K_CHOICE, "once", "once|loop|local" },
		{ "To", ST_K_CHOICE, "everyone", "everyone|me|player|spot" },
		{ "Player", ST_K_TEXT, "", NULL },
		{ "X", ST_K_NUMBER, "", NULL },
		{ "Y", ST_K_NUMBER, "", NULL },
		{ "Z", ST_K_NUMBER, "", NULL } } },
	{ "music", "Change music", "World", "Changes the music for everyone with rpmusic: a file from the music folder, the map's own again, or none. Pick it in the music manager to hear it first.", -1, -1, 2, {
		{ "Do", ST_K_CHOICE, "play", "play|map default|none" },
		{ "Music", ST_K_TEXT, "", NULL } } },
	{ "cinstart", "Start cinematic", "Cinematic", "Takes everyone with this client from their own view to the cinematic camera, starting where each of them looks from. Camera shots start one too. Players can leave it with /cincam stop.", -1, -1, 2, {
		{ "HUD", ST_K_CHOICE, "hide", "hide|show" },
		{ "Bars", ST_K_CHOICE, "on", "on|off" } } },
	{ "camshot", "Camera shot", "Cinematic", "Plays a shot made in the camera editor: Edit shot opens it there (or /cincam edit <name>). Fly where it should go, add spots, see the trail and preview it. Waits for the shot to end unless told not to.", -1, -1, 2, {
		{ "Shot", ST_K_TEXT, "", NULL },
		{ "Wait", ST_K_CHOICE, "until it ends", "until it ends|no" } } },
	{ "camfov", "Camera zoom", "Cinematic", "Zooms to the FOV (like cg_fov: smaller is closer) over the seconds; 0 seconds is at once.", -1, -1, 2, {
		{ "FOV", ST_K_NUMBER, "40", NULL },
		{ "Seconds", ST_K_NUMBER, "2", NULL } } },
	{ "cinbars", "Black bars", "Cinematic", "Slides the black bars in at the top and the bottom, or out. They work without the camera too.", -1, -1, 2, {
		{ "Bars", ST_K_CHOICE, "on", "on|off" },
		{ "Seconds", ST_K_NUMBER, "1", NULL } } },
	{ "cinend", "End cinematic", "Cinematic", "Everyone is back in their own view, and the bars slide out.", -1, -1, 0, {
		{ NULL, ST_K_TEXT, NULL, NULL } } },
};

typedef struct {
	stAction_t	action;
	qboolean	enabled;
	char		values[ST_MAX_FIELDS][ST_VALUE_LEN];
} stStep_t;

typedef struct {
	int			sel, scroll;
	int			lastClickTime, lastClickRow;
} stList_t;

typedef enum {
	ST_POPUP_NONE,
	ST_POPUP_SAVE,
	ST_POPUP_LOAD,
	ST_POPUP_ARMED,
	ST_POPUP_CHOOSE,
	ST_POPUP_EXPERIMENTAL	// the warning before turning them on
} stPopup_t;

// what the choose popup lists
typedef enum {
	ST_CHOOSE_TRIGGER,
	ST_CHOOSE_DOOR,
	ST_CHOOSE_EFFECT
} stChoose_t;

#define ST_MAX_PEOPLE	128

// a story playing: the one being edited, or an armed one
typedef struct {
	qboolean	running;
	int			pc;				// the step being run
	int			stepStart;		// when a wait began, 0 = not yet
	qboolean	waiting;		// at a "wait for me"
	qboolean	goOn;			// told to go on past it
	int			sent;			// commands sent this run
	qboolean	extraDue;		// the step's own command is out, its second one isn't
	int			shotUntil;		// a camera shot plays until then

	// at a "when someone comes": who was in at the last look, so only someone new sets it off
	qboolean	watching;
	int			inside[ST_MAX_PEOPLE];
	int			numInside;
	int			moverTime, moverATime;	// the door's last move
	char		by[MAX_QPATH];			// who set it off
} stRun_t;

typedef enum {
	ST_PICK_NONE,
	ST_PICK_MODEL,
	ST_PICK_EFFECT,
	ST_PICK_SHADER,
	ST_PICK_NPC_SPAWN,
	ST_PICK_NPC_MANAGE,
	ST_PICK_SOUND,
	ST_PICK_MUSIC
} stPick_t;

#define ST_MAX_PICKED	64

#define ST_F_NONE		-1
#define ST_F_SAVE_NAME	-2

// the actions list: a category's header, then its actions
typedef struct {
	const char	*header;		// NULL for an action
	stAction_t	action;
} stActionRow_t;

static struct {
	qboolean	open;
	int			font;

	// input; clicks and the wheel are handled where the control under them is drawn
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;
	int			wheel;
	int			focus;			// field of the selected step, or ST_F_*

	// the story
	stStep_t	steps[ST_MAX_STEPS];
	int			numSteps;
	char		name[MAX_QPATH];	// "" = not saved yet
	qboolean	dirty;
	stList_t	stepList;

	stActionRow_t	actionRows[ST_NUM_ACTIONS * 2];
	int				numActionRows;
	stList_t		actionList;

	// save and load
	stPopup_t	popup;
	char		saveName[MAX_QPATH];
	char		stories[ST_MAX_STORIES][MAX_QPATH];
	int			numStories;
	stList_t	storyList;
	qboolean	confirmNew;
	int			confirmTime;

	// playing
	stRun_t		run;
	int			nextSend;		// for every story: the server's flood protection is per client

	// the armed stories, and the list a trigger step picks from
	stList_t	armedList;
	int			armTimes;			// the "Arm: N times" button's
	stChoose_t	chooseKind;
	stList_t	chooseList;

	// a manager opened from a step: what it sends comes back as steps
	stPick_t	pick;
	int			pickStep;		// the step it was opened from
	char		picked[ST_MAX_PICKED][MAX_STRING_CHARS];
	int			numPicked;

	char		message[MAX_STRING_CHARS];
	int			messageTime;
} st;

static void ST_OpenArmed( void );
static void ST_OpenChoose( stChoose_t kind );

static vec4_t stWhite		= { 1.0f, 1.0f, 1.0f, 1.0f };
static vec4_t stPanel		= { 0.0f, 0.0f, 0.0f, 0.65f };
static vec4_t stPanelOpaque	= { 0.05f, 0.05f, 0.05f, 0.95f };
static vec4_t stPanelLight	= { 0.15f, 0.15f, 0.15f, 0.8f };
static vec4_t stPanelFocus	= { 0.1f, 0.2f, 0.3f, 0.9f };
static vec4_t stHighlight	= { 0.2f, 0.45f, 0.8f, 0.8f };
static vec4_t stHover		= { 1.0f, 1.0f, 1.0f, 0.12f };
static vec4_t stHoverBox	= { 0.25f, 0.25f, 0.25f, 0.9f };
static vec4_t stDanger		= { 0.6f, 0.15f, 0.15f, 0.9f };
static vec4_t stBorder		= { 0.5f, 0.5f, 0.5f, 0.8f };
static vec4_t stDim			= { 0.7f, 0.7f, 0.7f, 1.0f };
static vec4_t stAccent		= { 1.0f, 0.8f, 0.3f, 1.0f };
static vec4_t stRunning		= { 0.2f, 0.6f, 0.25f, 0.8f };

/*
===============================================================================

STEPS

===============================================================================
*/

static void ST_Message( const char *text ) {
	Q_strncpyz( st.message, text, sizeof( st.message ) );
	st.messageTime = cls.realtime;
}

static const stActionDef_t *ST_Def( const stStep_t *s ) {
	return &stActions[s->action];
}

static stStep_t *ST_SelectedStep( void ) {
	if ( st.stepList.sel < 0 || st.stepList.sel >= st.numSteps )
		return NULL;
	return &st.steps[st.stepList.sel];
}

// a choice field's choices, one at a time
static int ST_NumChoices( const char *choices ) {
	int n = 1;

	for ( ; *choices; choices++ ) {
		if ( *choices == '|' )
			n++;
	}
	return n;
}

static const char *ST_Choice( const char *choices, int index ) {
	static char buf[64];
	const char *end;

	while ( index-- > 0 ) {
		choices = strchr( choices, '|' );
		if ( !choices )
			return "";
		choices++;
	}
	end = strchr( choices, '|' );
	Q_strncpyz( buf, choices, end ? Q_min( (int)sizeof( buf ), (int)( end - choices ) + 1 ) : (int)sizeof( buf ) );
	return buf;
}

static int ST_ChoiceIndex( const char *choices, const char *value ) {
	const int n = ST_NumChoices( choices );

	for ( int i = 0; i < n; i++ ) {
		if ( !Q_stricmp( ST_Choice( choices, i ), value ) )
			return i;
	}
	return 0;
}

static float ST_Number( const stStep_t *s, int field ) {
	return (float)atof( s->values[field] );
}

static int ST_Round( float v ) {
	return (int)floorf( v + 0.5f );
}

static void ST_SetDefaults( stStep_t *s ) {
	const stActionDef_t *def = ST_Def( s );

	memset( s->values, 0, sizeof( s->values ) );
	for ( int i = 0; i < def->numFields; i++ )
		Q_strncpyz( s->values[i], def->fields[i].def, ST_VALUE_LEN );
}

// where the player stands, and the yaw they look along
static void ST_FillHere( stStep_t *s ) {
	const stActionDef_t *def = ST_Def( s );

	if ( def->posField < 0 || !cl.snap.valid )
		return;
	for ( int i = 0; i < 3; i++ )
		Com_sprintf( s->values[def->posField + i], ST_VALUE_LEN, "%i", ST_Round( cl.snap.ps.origin[i] ) );
	if ( def->yawField >= 0 )
		Com_sprintf( s->values[def->yawField], ST_VALUE_LEN, "%i", ST_Round( AngleNormalize360( cl.viewangles[YAW] ) ) );
}

// where the crosshair points, facing the player
static qboolean ST_FillAim( stStep_t *s ) {
	const stActionDef_t *def = ST_Def( s );
	vec3_t start, forward, end, pos;
	trace_t tr;

	if ( def->posField < 0 || !cl.snap.valid )
		return qfalse;
	VectorCopy( cl.snap.ps.origin, start );
	start[2] += cl.snap.ps.viewheight;
	AngleVectors( cl.viewangles, forward, NULL, NULL );
	VectorMA( start, ST_TRACE_DIST, forward, end );
	CM_BoxTrace( &tr, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
	if ( tr.fraction >= 1.0f || tr.allsolid )
		return qfalse;

	VectorCopy( tr.endpos, pos );
	// an NPC stands on the floor, the rest sits on the surface
	if ( s->action == ST_A_NPC_SPAWN || s->action == ST_A_NPC_TELE ) {
		VectorMA( pos, 16.0f, tr.plane.normal, pos );
		if ( tr.plane.normal[2] > 0.7f )
			pos[2] = tr.endpos[2] + ST_GROUND_HEIGHT;
	}
	for ( int i = 0; i < 3; i++ )
		Com_sprintf( s->values[def->posField + i], ST_VALUE_LEN, "%i", ST_Round( pos[i] ) );
	if ( def->yawField >= 0 )
		Com_sprintf( s->values[def->yawField], ST_VALUE_LEN, "%i", ST_Round( AngleNormalize360( cl.viewangles[YAW] + 180.0f ) ) );
	return qtrue;
}

static qboolean ST_TriggerNear( const stStep_t *s ) {
	return (qboolean)!Q_stricmpn( s->values[0], "near", 4 );
}

static qboolean ST_TriggerEffect( const stStep_t *s ) {
	return (qboolean)!Q_stricmp( s->values[0], "near an effect" );
}

static qboolean ST_TriggerDoor( const stStep_t *s ) {
	return (qboolean)!Q_stricmp( s->values[0], "door or button" );
}

// "*12" as the map's inline model 12, 0 if it isn't one
static int ST_ModelNum( const char *which ) {
	const int m = atoi( which[0] == '*' ? which + 1 : which );

	return ( m > 0 && m < CM_NumInlineModels() ) ? m : 0;
}

// a "when someone comes" step shows only what its kind of trigger uses
static qboolean ST_FieldShown( const stStep_t *s, int field ) {
	if ( s->action == ST_A_SOUND ) {
		if ( field == 3 )
			return (qboolean)!Q_stricmp( s->values[2], "player" );
		if ( field >= 4 )
			return (qboolean)!Q_stricmp( s->values[2], "spot" );
		return qtrue;
	}
	if ( s->action == ST_A_MUSIC )
		return (qboolean)( field != 1 || !Q_stricmp( s->values[0], "play" ) );
	if ( s->action != ST_A_TRIGGER )
		return qtrue;
	switch ( field ) {
	case 1:		return (qboolean)( !ST_TriggerNear( s ) || ST_TriggerEffect( s ) );
	case 2:		return (qboolean)!ST_TriggerDoor( s );
	case 3:		return (qboolean)( !ST_TriggerDoor( s ) && !Q_stricmp( s->values[2], "named" ) );
	case 4:
	case 5:
	case 6:
	case 7:		return ST_TriggerNear( s );
	default:	return qtrue;
	}
}

static qboolean ST_HavePos( const stStep_t *s ) {
	const stActionDef_t *def = ST_Def( s );

	if ( s->action == ST_A_TRIGGER && !ST_TriggerNear( s ) )
		return qtrue;
	if ( s->action == ST_A_SOUND && Q_stricmp( s->values[2], "spot" ) )
		return qtrue;

	if ( def->posField < 0 )
		return qtrue;
	for ( int i = 0; i < 3; i++ ) {
		if ( !s->values[def->posField + i][0] )
			return qfalse;
	}
	return qtrue;
}

static void ST_GetPos( const stStep_t *s, vec3_t pos ) {
	const stActionDef_t *def = ST_Def( s );

	for ( int i = 0; i < 3; i++ )
		pos[i] = ST_Number( s, def->posField + i );
}

static const char *ST_PosArgs( const stStep_t *s ) {
	vec3_t pos;

	ST_GetPos( s, pos );
	return va( "%i %i %i", ST_Round( pos[0] ), ST_Round( pos[1] ), ST_Round( pos[2] ) );
}

// a part of a hook's header: its separators would cut it up
// RPMod's server crashes on a centre print word longer than a line: the storyteller keeps them
// well under the client's own limit
#define ST_MAX_WORD		30

// the server breaks centre prints into lines and a line loses the colour before it, so each word
// gets the colour it's in: "^5this is ^7a test" goes as "^5this ^5is ^7a ^7test". Long words
// are broken up with spaces
static const char *ST_CenterText( const char *in ) {
	static char out[MAX_STRING_CHARS];
	char color[2] = { 0, 0 };
	int len = 0, run = 0;
	qboolean wordStart = qtrue;

	for ( ; *in && len < (int)sizeof( out ) - 5; in++ ) {
		if ( *in == ' ' ) {
			out[len++] = ' ';
			run = 0;
			wordStart = qtrue;
			continue;
		}
		if ( run >= ST_MAX_WORD ) {
			out[len++] = ' ';
			run = 0;
			wordStart = qtrue;
		}
		if ( wordStart && color[0] && !Q_IsColorString( in ) ) {
			out[len++] = color[0];
			out[len++] = color[1];
			run += 2;
		}
		wordStart = qfalse;
		if ( Q_IsColorString( in ) ) {
			color[0] = in[0];
			color[1] = in[1];
			out[len++] = *in++;
			run++;
		}
		out[len++] = *in;
		run++;
	}
	out[len] = '\0';
	return out;
}

static qboolean ST_HasEmote( const stStep_t *s ) {
	return (qboolean)( s->values[3][0] && Q_stricmp( s->values[3], "none" ) );
}

// sent as cmd: hooks, which only players with this client see
static qboolean ST_Experimental( stAction_t action ) {
	switch ( action ) {
	case ST_A_NPC_TALK:
	case ST_A_OBJECTIVE:
	case ST_A_CIN_START:
	case ST_A_CAM_SHOT:
	case ST_A_CAM_FOV:
	case ST_A_CIN_BARS:
	case ST_A_CIN_END:
		return qtrue;
	default:
		return qfalse;
	}
}

// an action from the list can go in the story
static qboolean ST_CanAdd( stAction_t action ) {
	if ( !ST_Experimental( action ) || CL_RpHooks_Experimental() )
		return qtrue;
	ST_Message( S_COLOR_YELLOW RP_EXPERIMENTAL_OFF ", top right" );
	return qfalse;
}

// what a step sends after its command, NULL if nothing
static const char *ST_StepExtra( const stStep_t *s ) {
	static char cmd[MAX_STRING_CHARS];

	if ( s->action != ST_A_NPC_TALK || !ST_HasEmote( s ) )
		return NULL;
	Com_sprintf( cmd, sizeof( cmd ), "npc emote %s %s", s->values[3], s->values[0] );
	return cmd;
}

// what a step sends: a server command, or with *local a console command; NULL and why if it can't
static const char *ST_StepCommand( const stStep_t *s, qboolean *local, const char **error ) {
	static char cmd[MAX_STRING_CHARS];
	const char ( *v )[ST_VALUE_LEN] = s->values;
	const char *value;
	vec3_t pos;

	*local = qfalse;
	*error = NULL;
	cmd[0] = '\0';

	if ( ST_Experimental( s->action ) && !CL_RpHooks_Experimental() ) {
		*error = RP_EXPERIMENTAL_OFF;
		return NULL;
	}
	if ( !ST_HavePos( s ) ) {
		*error = "Set the spot: X, Y and Z";
		return NULL;
	}

	switch ( s->action ) {
	case ST_A_WAIT:
	case ST_A_PAUSE:
		return NULL;

	case ST_A_TRIGGER:
		if ( !ST_TriggerNear( s ) && !ST_ModelNum( v[1] ) ) {
			*error = ST_TriggerDoor( s ) ? "Pick the door or button: aim at it, or pick it from the list"
				: "Pick the trigger: stand in it, or pick it from the list";
			return NULL;
		}
		if ( !ST_TriggerDoor( s ) && !Q_stricmp( v[2], "named" ) && !v[3][0] ) {
			*error = "Type the player's or the NPC's name, or the NPC's number";
			return NULL;
		}
		return NULL;

	case ST_A_SAY:
		if ( !v[0][0] ) {
			*error = "Type what to say";
			return NULL;
		}
		Com_sprintf( cmd, sizeof( cmd ), "say %s", v[0] );
		return cmd;

	case ST_A_CENTER:
		if ( !v[2][0] ) {
			*error = "Type the text to show";
			return NULL;
		}
		Com_sprintf( cmd, sizeof( cmd ), "rpcentersay %s %i %s", v[0][0] ? v[0] : "all", ST_Round( ST_Number( s, 1 ) ), ST_CenterText( v[2] ) );
		return cmd;

	case ST_A_NPC_TALK:
		if ( !v[0][0] ) {
			*error = "Type the NPC's targetname";
			return NULL;
		}
		if ( !v[4][0] ) {
			*error = "Type what the NPC says";
			return NULL;
		}
		return CL_RpHooks_NpcTalkCommand( v[0], v[1], v[2], ST_HasEmote( s ) ? v[3] : "", v[4] );

	case ST_A_OBJECTIVE:
		if ( !v[1][0] && Q_stricmp( v[0], "clear" ) && Q_stricmp( v[0], "title" ) ) {
			*error = !Q_stricmp( v[0], "add" ) ? "Type the objective" : "Type the objective's number or text";
			return NULL;
		}
		if ( v[1][0] )
			Com_sprintf( cmd, sizeof( cmd ), "rpnotify -1 1 cmd:objective/%s: %s", v[0], v[1] );
		else
			Com_sprintf( cmd, sizeof( cmd ), "rpnotify -1 1 cmd:objective/%s", v[0] );
		return cmd;

	case ST_A_CIN_START:
		Com_sprintf( cmd, sizeof( cmd ), "rpnotify -1 1 cmd:cin/start%s%s", !Q_stricmp( v[0], "show" ) ? "/hud" : "",
			!Q_stricmp( v[1], "on" ) ? "/bars" : "" );
		return cmd;

	case ST_A_CAM_SHOT: {
		static char why[MAX_STRING_CHARS];
		char hook[MAX_STRING_CHARS];

		if ( !v[0][0] ) {
			*error = "Name the shot, then Edit shot to make it";
			return NULL;
		}
		if ( !CL_RpCinematic_ShotHook( v[0], hook, sizeof( hook ), NULL ) ) {
			Com_sprintf( why, sizeof( why ), "No shot called %s yet: Edit shot makes it", v[0] );
			*error = why;
			return NULL;
		}
		Com_sprintf( cmd, sizeof( cmd ), "rpnotify -1 1 cmd:%s", hook );
		return cmd;
	}

	case ST_A_CAM_FOV:
		if ( ST_Number( s, 0 ) <= 0 ) {
			*error = "Set the FOV, e.g. 40 to zoom in, 80 for the usual";
			return NULL;
		}
		Com_sprintf( cmd, sizeof( cmd ), "rpnotify -1 1 cmd:cin/fov/%g/%g", ST_Number( s, 0 ), ST_Number( s, 1 ) );
		return cmd;

	case ST_A_CIN_BARS:
		Com_sprintf( cmd, sizeof( cmd ), "rpnotify -1 1 cmd:cin/bars/%s/%g", !Q_stricmp( v[0], "off" ) ? "off" : "on", ST_Number( s, 1 ) );
		return cmd;

	case ST_A_CIN_END:
		return "rpnotify -1 1 cmd:cin/end";

	case ST_A_COMMAND:
		if ( !v[0][0] ) {
			*error = "Type the command";
			return NULL;
		}
		*local = qtrue;
		Q_strncpyz( cmd, v[0], sizeof( cmd ) );
		return cmd;

	case ST_A_NPC_SPAWN:
		if ( !v[0][0] ) {
			*error = "Type the NPC type, e.g. stormtrooper";
			return NULL;
		}
		ST_GetPos( s, pos );
		return CL_NpcManager_SpawnCommand( v[0], (qboolean)!Q_stricmp( v[3], "yes" ), v[1], v[2], v[4], v[5], pos,
			ST_Round( AngleNormalize360( ST_Number( s, 9 ) ) ) );

	case ST_A_NPC_TELE:
		if ( !v[0][0] ) {
			*error = "Type the target: a targetname, a number or all";
			return NULL;
		}
		Com_sprintf( cmd, sizeof( cmd ), "npc tele %s %i %s", ST_PosArgs( s ), ST_Round( AngleNormalize360( ST_Number( s, 4 ) ) ), v[0] );
		return cmd;

	case ST_A_NPC_CMD:
		if ( !v[0][0] ) {
			*error = "Type the target: a targetname, a number or all";
			return NULL;
		}
		value = v[1];
		if ( ( !Q_stricmp( value, "emote" ) || !Q_stricmp( value, "dialog" ) ) && !v[2][0] ) {
			*error = !Q_stricmp( value, "emote" ) ? "Type the emote, e.g. emsit" : "Type the dialog";
			return NULL;
		}
		if ( !Q_stricmp( value, "kill" ) )					Com_sprintf( cmd, sizeof( cmd ), "npc kill %s", v[0] );
		else if ( !Q_stricmp( value, "freeze" ) )			Com_sprintf( cmd, sizeof( cmd ), "npc freeze %s 1", v[0] );
		else if ( !Q_stricmp( value, "unfreeze" ) )			Com_sprintf( cmd, sizeof( cmd ), "npc freeze %s 0", v[0] );
		else if ( !Q_stricmp( value, "emote" ) )			Com_sprintf( cmd, sizeof( cmd ), "npc emote %s %s", v[2], v[0] );
		else if ( !Q_stricmp( value, "stop emote" ) )		Com_sprintf( cmd, sizeof( cmd ), "npc emote none %s", v[0] );
		else if ( !Q_stricmp( value, "follow me" ) )		Com_sprintf( cmd, sizeof( cmd ), "npc follow %i %s", clc.clientNum, v[0] );
		else if ( !Q_stricmp( value, "stop following" ) )	Com_sprintf( cmd, sizeof( cmd ), "npc follow none %s", v[0] );
		else if ( !Q_stricmp( value, "dialog" ) )			Com_sprintf( cmd, sizeof( cmd ), "npc dialog %s %s", v[2], v[0] );
		else if ( !Q_stricmp( value, "no dialog" ) )		Com_sprintf( cmd, sizeof( cmd ), "npc dialog none %s", v[0] );
		else if ( !Q_stricmp( value, "hologram" ) )			Com_sprintf( cmd, sizeof( cmd ), "npc hologram normal %s", v[0] );
		else if ( !Q_stricmp( value, "projector" ) )		Com_sprintf( cmd, sizeof( cmd ), "npc hologram projector %s", v[0] );
		else												Com_sprintf( cmd, sizeof( cmd ), "npc hologram off %s", v[0] );
		return cmd;

	case ST_A_MODEL:
		if ( !v[0][0] ) {
			*error = "Type the model, e.g. models/map_objects/imperial/crate_banded.md3";
			return NULL;
		}
		Com_sprintf( cmd, sizeof( cmd ), "rpmodel add %s %s %i %i %i %i %i", v[0], ST_PosArgs( s ),
			ST_Round( AngleNormalize360( ST_Number( s, 4 ) ) ), ST_Round( AngleNormalize360( ST_Number( s, 5 ) ) ),
			ST_Round( AngleNormalize360( ST_Number( s, 6 ) ) ), v[7][0] ? ST_Round( ST_Number( s, 7 ) ) : 100,
			Q_stricmp( v[8], "no" ) ? 1 : 0 );
		return cmd;

	case ST_A_EFFECT:
		if ( !v[0][0] ) {
			*error = "Type the effect, e.g. env/fire";
			return NULL;
		}
		Com_sprintf( cmd, sizeof( cmd ), "rpeffect %s %s %s %i %i %i", !Q_stricmp( v[1], "loop" ) ? "add" : "once", v[0], ST_PosArgs( s ),
			ST_Round( AngleNormalize360( ST_Number( s, 5 ) ) ), ST_Round( AngleNormalize360( ST_Number( s, 6 ) ) ),
			ST_Round( AngleNormalize360( ST_Number( s, 7 ) ) ) );
		return cmd;

	case ST_A_SCREEN:
		value = v[0];
		if ( !Q_stricmp( value, "fade out" ) )			Q_strncpyz( cmd, "rpeffect fadeout", sizeof( cmd ) );
		else if ( !Q_stricmp( value, "fade in" ) )		Q_strncpyz( cmd, "rpeffect remove fadeout", sizeof( cmd ) );
		else if ( !Q_stricmp( value, "alarm" ) )		Q_strncpyz( cmd, "rpeffect alarm", sizeof( cmd ) );
		else if ( !Q_stricmp( value, "stop alarm" ) )	Q_strncpyz( cmd, "rpeffect remove alarm", sizeof( cmd ) );
		else {
			if ( !v[1][0] || !v[2][0] ) {
				*error = "Set the shake's intensity and duration";
				return NULL;
			}
			Com_sprintf( cmd, sizeof( cmd ), "rpeffect camerashake %s %s", v[1], v[2] );
		}
		return cmd;

	case ST_A_SOUND: {
		const char *verb = !Q_stricmp( v[1], "loop" ) ? "add" : !Q_stricmp( v[1], "local" ) ? "local" : "once";
		const int len = strlen( v[0] );

		if ( !v[0][0] ) {
			*error = "Type the sound, e.g. ambience/thunder1.wav, or pick it in the sound manager";
			return NULL;
		}
		// the engine plays an mp3 sound once and no more, so a looping one would go quiet
		if ( !strcmp( verb, "add" ) && len > 4 && !Q_stricmp( v[0] + len - 4, ".mp3" ) ) {
			*error = "An mp3 only plays once in this engine: loop a .wav, or play this once";
			return NULL;
		}
		if ( !Q_stricmp( v[2], "player" ) ) {
			if ( !v[3][0] ) {
				*error = "Type the player's number";
				return NULL;
			}
			Com_sprintf( cmd, sizeof( cmd ), "rpsound %s %s %s", verb, v[0], v[3] );
		} else if ( !Q_stricmp( v[2], "spot" ) ) {
			if ( strcmp( verb, "add" ) ) {
				*error = "Only a looping sound plays at a spot";
				return NULL;
			}
			Com_sprintf( cmd, sizeof( cmd ), "rpsound add %s %s", v[0], ST_PosArgs( s ) );
		} else if ( !Q_stricmp( v[2], "me" ) ) {
			if ( !strcmp( verb, "add" ) ) {
				*error = "A looping sound goes to everyone, a player or a spot";
				return NULL;
			}
			Com_sprintf( cmd, sizeof( cmd ), "rpsound %s %s", verb, v[0] );
		} else {
			if ( !strcmp( verb, "local" ) ) {
				*error = "A local sound goes to you or a player";
				return NULL;
			}
			Com_sprintf( cmd, sizeof( cmd ), "rpsound %s %s all", verb, v[0] );
		}
		return cmd;
	}

	case ST_A_MUSIC:
		if ( !Q_stricmp( v[0], "map default" ) )
			return "rpmusic default";
		if ( !Q_stricmp( v[0], "none" ) )
			return "rpmusic none";
		if ( !v[1][0] ) {
			*error = "Type the music, e.g. kejim/impbaseb_explore.mp3, or pick it in the music manager";
			return NULL;
		}
		Com_sprintf( cmd, sizeof( cmd ), "rpmusic %s", v[1] );
		return cmd;

	case ST_A_SHADER:
		if ( !v[0][0] || !v[1][0] ) {
			*error = "Type the shader to swap, and the one to put instead";
			return NULL;
		}
		if ( v[2][0] )
			Com_sprintf( cmd, sizeof( cmd ), "rpshader remap %s %s %s", v[0], v[1], v[2] );
		else
			Com_sprintf( cmd, sizeof( cmd ), "rpshader remap %s %s", v[0], v[1] );
		return cmd;

	default:
		return NULL;
	}
}

/*
===============================================================================

When someone comes

===============================================================================
*/

#define ST_MAX_MAPENTS	1024
#define ST_MAX_ARMED	16
#define ST_MAX_ARM_TIMES	99

// the map's brush entities, read from its entity string: triggers, doors, buttons...
typedef struct {
	int			model;
	char		classname[48];
	char		targetname[48];
	qboolean	trigger;
	vec3_t		mins, maxs;
} stMapEnt_t;

static struct {
	char		map[MAX_QPATH];
	stMapEnt_t	ents[ST_MAX_MAPENTS];
	int			count;
} stMap;

// a story armed: a copy of it plays each time its first step is set off
typedef struct {
	char		name[MAX_QPATH];
	char		map[MAX_QPATH];		// it goes with the map
	stStep_t	*steps;
	int			numSteps;
	int			limit;				// times it plays, 0 = every time
	int			times;				// set off so far
	stRun_t		run;
} stArmed_t;

static stArmed_t	stArmed[ST_MAX_ARMED];
static int			stNumArmed;

// a player or an NPC the snapshot has
typedef struct {
	int			num;
	qboolean	npc;
	vec3_t		org;
} stPerson_t;

static void ST_ScanMap( void ) {
	char classname[48], targetname[48], model[16], key[64];
	const char *p, *token;

	if ( !cl.mapname[0] || !Q_stricmp( stMap.map, cl.mapname ) )
		return;
	Q_strncpyz( stMap.map, cl.mapname, sizeof( stMap.map ) );
	stMap.count = 0;

	p = CM_EntityString();
	while ( p ) {
		token = COM_Parse( &p );
		if ( !token[0] )
			break;
		if ( strcmp( token, "{" ) )
			continue;
		classname[0] = targetname[0] = model[0] = '\0';
		for ( ;; ) {
			token = COM_Parse( &p );
			if ( !token[0] || !strcmp( token, "}" ) )
				break;
			Q_strncpyz( key, token, sizeof( key ) );
			token = COM_Parse( &p );
			if ( !Q_stricmp( key, "classname" ) )
				Q_strncpyz( classname, token, sizeof( classname ) );
			else if ( !Q_stricmp( key, "targetname" ) )
				Q_strncpyz( targetname, token, sizeof( targetname ) );
			else if ( !Q_stricmp( key, "model" ) )
				Q_strncpyz( model, token, sizeof( model ) );
		}
		if ( model[0] != '*' || !ST_ModelNum( model ) || stMap.count >= ST_MAX_MAPENTS )
			continue;

		stMapEnt_t *e = &stMap.ents[stMap.count++];
		e->model = ST_ModelNum( model );
		Q_strncpyz( e->classname, classname, sizeof( e->classname ) );
		Q_strncpyz( e->targetname, targetname, sizeof( e->targetname ) );
		e->trigger = (qboolean)!Q_stricmpn( classname, "trigger_", 8 );
		CM_ModelBounds( CM_InlineModel( e->model ), e->mins, e->maxs );
	}
}

static const stMapEnt_t *ST_FindEnt( int model ) {
	ST_ScanMap();
	for ( int i = 0; i < stMap.count; i++ ) {
		if ( stMap.ents[i].model == model )
			return &stMap.ents[i];
	}
	return NULL;
}

// "trigger_multiple door1 (*12)"
static const char *ST_EntLabel( const char *which ) {
	const int m = ST_ModelNum( which );
	const stMapEnt_t *e = m ? ST_FindEnt( m ) : NULL;

	if ( !e )
		return which[0] ? which : "?";
	return va( "%s%s%s (*%i)", e->classname, e->targetname[0] ? " " : "", e->targetname, e->model );
}

static const char *ST_WhoText( const stStep_t *s ) {
	const char *who = s->values[2];

	if ( !Q_stricmp( who, "any NPC" ) )		return "an NPC";
	if ( !Q_stricmp( who, "anyone" ) )		return "anyone";
	if ( !Q_stricmp( who, "me" ) )			return "you";
	if ( !Q_stricmp( who, "named" ) )		return s->values[3][0] ? s->values[3] : "someone named";
	return "a player";
}

static const char *ST_TriggerSummary( const stStep_t *s ) {
	if ( ST_TriggerDoor( s ) )
		return va( "When %s moves", ST_EntLabel( s->values[1] ) );
	if ( ST_TriggerEffect( s ) )
		return va( "When %s comes near %s", ST_WhoText( s ), s->values[1][0] ? s->values[1] : "an effect" );
	if ( ST_TriggerNear( s ) )
		return va( "When %s comes within %g of %s %s %s", ST_WhoText( s ), ST_Number( s, 4 ) > 0 ? ST_Number( s, 4 ) : 96.0f,
			s->values[5], s->values[6], s->values[7] );
	return va( "When %s comes into %s", ST_WhoText( s ), ST_EntLabel( s->values[1] ) );
}

// a player's box, near enough for an NPC's too
static const vec3_t stPersonMins = { -15, -15, -24 };
static const vec3_t stPersonMaxs = { 15, 15, 40 };

static qboolean ST_InTrigger( int model, const vec3_t org ) {
	trace_t tr;

	CM_BoxTrace( &tr, org, org, stPersonMins, stPersonMaxs, CM_InlineModel( model ), -1, qfalse );
	return (qboolean)( tr.startsolid || tr.allsolid );
}

// the smallest of the map's triggers the player stands in, 0 if none
static int ST_TriggerHere( void ) {
	float best = 0;
	int model = 0;

	ST_ScanMap();
	for ( int i = 0; i < stMap.count; i++ ) {
		const stMapEnt_t *e = &stMap.ents[i];
		vec3_t size;
		float volume;

		if ( !e->trigger || !ST_InTrigger( e->model, cl.snap.ps.origin ) )
			continue;
		VectorSubtract( e->maxs, e->mins, size );
		volume = size[0] * size[1] * size[2];
		if ( !model || volume < best ) {
			best = volume;
			model = e->model;
		}
	}
	return model;
}

static const entityState_t *ST_SnapEntity( int i ) {
	return &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
}

// the door, button or other brush entity the crosshair is on, 0 if none
static int ST_AimedMover( void ) {
	vec3_t start, end, forward;
	trace_t tr;
	float best;
	int model = 0;

	VectorCopy( cl.snap.ps.origin, start );
	start[2] += cl.snap.ps.viewheight;
	AngleVectors( cl.viewangles, forward, NULL, NULL );
	VectorMA( start, 8192, forward, end );
	CM_BoxTrace( &tr, start, end, vec3_origin, vec3_origin, 0, MASK_SOLID, qfalse );
	best = tr.fraction;

	ST_ScanMap();
	for ( int i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = ST_SnapEntity( i );
		const stMapEnt_t *e;

		if ( es->solid != SOLID_BMODEL || !ST_ModelNum( va( "%i", es->modelindex ) ) )
			continue;
		e = ST_FindEnt( es->modelindex );
		if ( e && e->trigger )
			continue;
		CM_TransformedBoxTrace( &tr, start, end, vec3_origin, vec3_origin, CM_InlineModel( es->modelindex ), -1,
			es->pos.trBase, es->apos.trBase, qfalse );
		if ( tr.fraction <= best && tr.fraction < 1.0f ) {
			best = tr.fraction;
			model = es->modelindex;
		}
	}
	return model;
}

// the players and NPCs this client can see, the player first
static int ST_People( stPerson_t *out, int max ) {
	int n = 0;

	if ( !cl.snap.valid )
		return 0;
	if ( cl.snap.ps.pm_type != PM_SPECTATOR ) {
		out[n].num = cl.snap.ps.clientNum;
		out[n].npc = qfalse;
		VectorCopy( cl.snap.ps.origin, out[n].org );
		n++;
	}
	for ( int i = 0; i < cl.snap.numEntities && n < max; i++ ) {
		const entityState_t *es = ST_SnapEntity( i );

		if ( es->number == cl.snap.ps.clientNum )
			continue;
		if ( es->eType == ET_PLAYER && es->number < MAX_CLIENTS )
			out[n].npc = qfalse;
		else if ( es->eType == ET_NPC )
			out[n].npc = qtrue;
		else
			continue;
		out[n].num = es->number;
		VectorCopy( es->pos.trBase, out[n].org );
		n++;
	}
	return n;
}

// a player's name, an NPC's targetname or type
static void ST_PersonName( const stPerson_t *p, char *out, int size ) {
	if ( !p->npc ) {
		const char *cs = cl.gameState.stringData + cl.gameState.stringOffsets[CS_PLAYERS + p->num];
		Q_strncpyz( out, Info_ValueForKey( cs, "n" ), size );
		return;
	}
	for ( int i = 0; i < CL_NpcManager_NumNpcs(); i++ ) {
		const char *type, *name;
		int num;

		if ( CL_NpcManager_GetNpc( i, &num, &type, &name ) && num == p->num ) {
			Q_strncpyz( out, name[0] ? name : type, size );
			return;
		}
	}
	Com_sprintf( out, size, "NPC %i", p->num );
}

// a name as typed: a number is an NPC's, a name is matched without colours, in part from 3 letters
static qboolean ST_NameIs( const stPerson_t *p, const char *want ) {
	char name[MAX_QPATH], typed[MAX_QPATH];

	if ( !want[0] )
		return qfalse;
	if ( Q_isanumber( want ) )
		return (qboolean)( p->npc && p->num == atoi( want ) );
	ST_PersonName( p, name, sizeof( name ) );
	Q_StripColor( name );
	Q_strncpyz( typed, want, sizeof( typed ) );
	Q_StripColor( typed );
	if ( !Q_stricmp( name, typed ) )
		return qtrue;
	return (qboolean)( strlen( typed ) >= 3 && Q_stristr( name, typed ) );
}

static qboolean ST_Counts( const stStep_t *s, const stPerson_t *p ) {
	const char *who = s->values[2];

	if ( !Q_stricmp( who, "any NPC" ) )		return p->npc;
	if ( !Q_stricmp( who, "anyone" ) )		return qtrue;
	if ( !Q_stricmp( who, "me" ) )			return (qboolean)( !p->npc && p->num == cl.snap.ps.clientNum );
	if ( !Q_stricmp( who, "named" ) )		return ST_NameIs( p, s->values[3] );
	return (qboolean)!p->npc;
}

static qboolean ST_Inside( const stStep_t *s, const vec3_t org ) {
	if ( ST_TriggerNear( s ) ) {
		vec3_t pos;
		const float radius = ST_Number( s, 4 ) > 0 ? ST_Number( s, 4 ) : 96.0f;

		ST_GetPos( s, pos );
		return (qboolean)( Distance( org, pos ) <= radius );
	}
	return ST_InTrigger( ST_ModelNum( s->values[1] ), org );
}

static const entityState_t *ST_FindMover( int model ) {
	for ( int i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = ST_SnapEntity( i );

		if ( es->solid == SOLID_BMODEL && es->modelindex == model )
			return es;
	}
	return NULL;
}

static int ST_CommandGap( void );

// a "when someone comes" step, looked at once a frame: qtrue when someone it counts has
// come in since the last look, or the door started a move. The first look only sees
// who is in already
static qboolean ST_TriggerFired( stRun_t *r, const stStep_t *s ) {
	stPerson_t people[ST_MAX_PEOPLE];
	int now[ST_MAX_PEOPLE], numNow = 0, n;
	qboolean fired = qfalse;

	if ( !cl.snap.valid )
		return qfalse;
	r->by[0] = '\0';

	if ( ST_TriggerDoor( s ) ) {
		const entityState_t *es = ST_FindMover( ST_ModelNum( s->values[1] ) );

		// out of sight: it's looked at afresh when it's back
		if ( !es ) {
			r->watching = qfalse;
			return qfalse;
		}
		fired = (qboolean)( r->watching && ( es->pos.trTime != r->moverTime || es->apos.trTime != r->moverATime )
			&& ( es->pos.trType != TR_STATIONARY || es->apos.trType != TR_STATIONARY ) );
		r->moverTime = es->pos.trTime;
		r->moverATime = es->apos.trTime;
		r->watching = qtrue;
		return fired;
	}

	// a named NPC is found in the NPC list, asked for once as it starts watching
	if ( !r->watching && !Q_stricmp( s->values[2], "named" ) && !Q_isanumber( s->values[3] ) ) {
		CL_NpcManager_RequestList();
		st.nextSend = Q_max( st.nextSend, cls.realtime + ST_CommandGap() );
	}

	n = ST_People( people, ST_MAX_PEOPLE );
	for ( int i = 0; i < n; i++ ) {
		qboolean was = qfalse;

		if ( !ST_Counts( s, &people[i] ) || !ST_Inside( s, people[i].org ) )
			continue;
		now[numNow++] = people[i].num;
		for ( int j = 0; j < r->numInside; j++ ) {
			if ( r->inside[j] == people[i].num )
				was = qtrue;
		}
		if ( !was && r->watching && !fired ) {
			fired = qtrue;
			ST_PersonName( &people[i], r->by, sizeof( r->by ) );
		}
	}
	memcpy( r->inside, now, numNow * sizeof( now[0] ) );
	r->numInside = numNow;
	r->watching = qtrue;
	return fired;
}

// one line about a step, for the story list
static const char *ST_StepSummary( const stStep_t *s ) {
	static char buf[MAX_STRING_CHARS];
	const stActionDef_t *def = ST_Def( s );

	switch ( s->action ) {
	case ST_A_WAIT:
		Com_sprintf( buf, sizeof( buf ), "Wait %s s", s->values[0][0] ? s->values[0] : "0" );
		break;
	case ST_A_PAUSE:
		Com_sprintf( buf, sizeof( buf ), "Wait for me%s%s", s->values[0][0] ? ": " : "", s->values[0] );
		break;
	case ST_A_TRIGGER:
		Q_strncpyz( buf, ST_TriggerSummary( s ), sizeof( buf ) );
		break;
	case ST_A_SOUND:
		Com_sprintf( buf, sizeof( buf ), "Sound %s (%s, %s%s%s)", s->values[0][0] ? s->values[0] : "?", s->values[1], s->values[2],
			!Q_stricmp( s->values[2], "player" ) ? " " : "", !Q_stricmp( s->values[2], "player" ) ? s->values[3] : "" );
		break;
	case ST_A_MUSIC:
		Com_sprintf( buf, sizeof( buf ), "Music: %s", !Q_stricmp( s->values[0], "play" ) ? ( s->values[1][0] ? s->values[1] : "?" ) : s->values[0] );
		break;
	case ST_A_NPC_SPAWN:
		Com_sprintf( buf, sizeof( buf ), "Spawn %s%s%s", s->values[0][0] ? s->values[0] : "?",
			s->values[1][0] ? " as " : "", s->values[1] );
		break;
	case ST_A_NPC_CMD:
		Com_sprintf( buf, sizeof( buf ), "NPC %s %s%s%s", s->values[0][0] ? s->values[0] : "?", s->values[1],
			s->values[2][0] ? " " : "", s->values[2] );
		break;
	case ST_A_NPC_TELE:
		Com_sprintf( buf, sizeof( buf ), "Teleport %s", s->values[0][0] ? s->values[0] : "?" );
		break;
	case ST_A_NPC_TALK:
		Com_sprintf( buf, sizeof( buf ), "%s: %s", s->values[1][0] ? s->values[1] : s->values[0][0] ? s->values[0] : "?", s->values[4] );
		break;
	case ST_A_OBJECTIVE:
		Com_sprintf( buf, sizeof( buf ), "Objective %s%s%s", s->values[0], s->values[1][0] ? ": " : "", s->values[1] );
		break;
	case ST_A_CIN_START:
		Com_sprintf( buf, sizeof( buf ), "Start cinematic (HUD %s, bars %s)", s->values[0], s->values[1] );
		break;
	case ST_A_CAM_SHOT:
		Com_sprintf( buf, sizeof( buf ), "Camera shot %s%s", s->values[0][0] ? s->values[0] : "?",
			!Q_stricmp( s->values[1], "no" ) ? " (goes on)" : "" );
		break;
	case ST_A_CAM_FOV:
		Com_sprintf( buf, sizeof( buf ), "Camera zoom to %s in %s s", s->values[0], s->values[1] );
		break;
	case ST_A_CIN_BARS:
		Com_sprintf( buf, sizeof( buf ), "Black bars %s", s->values[0] );
		break;
	case ST_A_CENTER:
		Com_sprintf( buf, sizeof( buf ), "Center: %s", s->values[2] );
		break;
	case ST_A_EFFECT:
		Com_sprintf( buf, sizeof( buf ), "Effect %s (%s)", s->values[0][0] ? s->values[0] : "?", s->values[1] );
		break;
	case ST_A_SCREEN:
		Com_sprintf( buf, sizeof( buf ), "Screen: %s", s->values[0] );
		break;
	case ST_A_SHADER:
		Com_sprintf( buf, sizeof( buf ), "Remap %s", s->values[0][0] ? s->values[0] : "?" );
		break;
	default:
		Com_sprintf( buf, sizeof( buf ), "%s%s%s", def->name, s->values[0][0] ? ": " : "", s->values[0] );
		break;
	}
	return buf;
}

static qboolean ST_StepUsable( const stStep_t *s ) {
	qboolean local;
	const char *error;

	if ( s->action == ST_A_WAIT || s->action == ST_A_PAUSE )
		return qtrue;
	ST_StepCommand( s, &local, &error );
	return (qboolean)!error;
}

static void ST_Changed( void ) {
	st.dirty = qtrue;
}

static void ST_SelectStep( int index ) {
	st.stepList.sel = index;
	st.focus = ST_F_NONE;
}

// a new step of this action, after the selected one
static void ST_AddStep( stAction_t action ) {
	int at;
	stStep_t *s;

	if ( st.numSteps >= ST_MAX_STEPS ) {
		ST_Message( S_COLOR_RED "The story is full" );
		return;
	}
	at = ( st.stepList.sel >= 0 && st.stepList.sel < st.numSteps ) ? st.stepList.sel + 1 : st.numSteps;
	memmove( &st.steps[at + 1], &st.steps[at], ( st.numSteps - at ) * sizeof( st.steps[0] ) );
	st.numSteps++;
	if ( st.run.running && st.run.pc >= at )
		st.run.pc++;

	s = &st.steps[at];
	s->action = action;
	s->enabled = qtrue;
	ST_SetDefaults( s );
	// most spots are near where the storyteller stands
	ST_FillHere( s );
	ST_SelectStep( at );
	ST_Changed();
}

static void ST_DeleteStep( void ) {
	const int at = st.stepList.sel;

	if ( at < 0 || at >= st.numSteps )
		return;
	memmove( &st.steps[at], &st.steps[at + 1], ( st.numSteps - at - 1 ) * sizeof( st.steps[0] ) );
	st.numSteps--;
	if ( st.run.running && st.run.pc > at )
		st.run.pc--;
	ST_SelectStep( at < st.numSteps ? at : st.numSteps - 1 );
	ST_Changed();
}

static void ST_CopyStep( void ) {
	const int at = st.stepList.sel;

	if ( at < 0 || at >= st.numSteps || st.numSteps >= ST_MAX_STEPS )
		return;
	memmove( &st.steps[at + 1], &st.steps[at], ( st.numSteps - at ) * sizeof( st.steps[0] ) );
	st.numSteps++;
	if ( st.run.running && st.run.pc > at )
		st.run.pc++;
	ST_SelectStep( at + 1 );
	ST_Changed();
}

static void ST_MoveStep( int delta ) {
	const int at = st.stepList.sel, to = at + delta;
	stStep_t tmp;

	if ( at < 0 || at >= st.numSteps || to < 0 || to >= st.numSteps )
		return;
	tmp = st.steps[at];
	st.steps[at] = st.steps[to];
	st.steps[to] = tmp;
	if ( st.run.running ) {
		if ( st.run.pc == at )
			st.run.pc = to;
		else if ( st.run.pc == to )
			st.run.pc = at;
	}
	ST_SelectStep( to );
	ST_Changed();
}

static void ST_New( void ) {
	st.run.running = qfalse;
	st.numSteps = 0;
	st.name[0] = '\0';
	st.dirty = qfalse;
	memset( &st.stepList, 0, sizeof( st.stepList ) );
	st.stepList.sel = -1;
	st.focus = ST_F_NONE;
}

/*
===============================================================================

PLAYING

===============================================================================
*/

static int ST_CommandGap( void ) {
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

static void ST_Stop( const char *why ) {
	if ( !st.run.running )
		return;
	st.run.running = qfalse;
	st.run.waiting = qfalse;
	if ( why ) {
		ST_Message( why );
		Com_Printf( "Storyteller: %s\n", why );
	}
}

static void ST_StartRun( stRun_t *r, int from ) {
	memset( r, 0, sizeof( *r ) );
	r->running = qtrue;
	r->pc = from;
}

static void ST_Run( int from ) {
	if ( cls.state != CA_ACTIVE ) {
		ST_Message( S_COLOR_RED "Join a server first" );
		return;
	}
	if ( from < 0 || from >= st.numSteps ) {
		ST_Message( S_COLOR_YELLOW "Nothing to play: add steps first" );
		return;
	}
	ST_StartRun( &st.run, from );
	ST_Message( from ? va( S_COLOR_GREEN "Playing from step %i", from + 1 ) : S_COLOR_GREEN "Playing" );
}

static qboolean ST_AnyWaiting( void ) {
	if ( st.run.running && st.run.waiting )
		return qtrue;
	for ( int i = 0; i < stNumArmed; i++ ) {
		if ( stArmed[i].run.running && stArmed[i].run.waiting )
			return qtrue;
	}
	return qfalse;
}

// every story at a "wait for me" goes on
static void ST_Continue( void ) {
	if ( st.run.running && st.run.waiting )
		st.run.goOn = qtrue;
	for ( int i = 0; i < stNumArmed; i++ ) {
		if ( stArmed[i].run.running && stArmed[i].run.waiting )
			stArmed[i].run.goOn = qtrue;
	}
}

// a frame of a story playing; qtrue when it got to its end. An armed one is named in the console
static qboolean ST_Play( stRun_t *r, stStep_t *steps, int numSteps, const char *armed ) {
	char prefix[MAX_QPATH + 32];
	const char *cmd, *error;
	qboolean local;
	stStep_t *s;

	if ( armed )
		Com_sprintf( prefix, sizeof( prefix ), "Storyteller (%s): ", armed );
	else
		Q_strncpyz( prefix, "Storyteller: ", sizeof( prefix ) );

	while ( r->running ) {
		if ( cls.realtime < r->shotUntil )
			return qfalse;
		if ( r->pc >= numSteps )
			return qtrue;
		s = &steps[r->pc];
		if ( !s->enabled ) {
			r->pc++;
			continue;
		}

		switch ( s->action ) {
		case ST_A_WAIT:
			if ( !r->stepStart )
				r->stepStart = cls.realtime;
			if ( cls.realtime - r->stepStart < (int)( ST_Number( s, 0 ) * 1000.0f ) )
				return qfalse;
			r->stepStart = 0;
			r->pc++;
			continue;

		case ST_A_PAUSE:
			r->waiting = qtrue;
			if ( !r->goOn )
				return qfalse;
			r->waiting = r->goOn = qfalse;
			r->pc++;
			continue;

		case ST_A_TRIGGER:
			ST_StepCommand( s, &local, &error );
			if ( error ) {
				Com_Printf( S_COLOR_YELLOW "%sstep %i skipped: %s\n", prefix, r->pc + 1, error );
				r->pc++;
				continue;
			}
			if ( !ST_TriggerFired( r, s ) )
				return qfalse;
			Com_Printf( S_COLOR_GREEN "%s" S_COLOR_WHITE "step %i set off%s%s\n", prefix, r->pc + 1, r->by[0] ? " by " : "", r->by );
			r->watching = qfalse;
			r->numInside = 0;
			r->pc++;
			continue;

		default:
			if ( cls.realtime < st.nextSend )
				return qfalse;
			if ( r->extraDue ) {
				r->extraDue = qfalse;
				cmd = ST_StepExtra( s );
				if ( cmd ) {
					Com_Printf( S_COLOR_GREEN "%s" S_COLOR_WHITE "%s\n", prefix, cmd );
					CL_AddReliableCommand( cmd, qfalse );
					r->sent++;
					st.nextSend = cls.realtime + ST_CommandGap();
				}
				r->pc++;
				return qfalse;
			}
			cmd = ST_StepCommand( s, &local, &error );
			if ( !cmd ) {
				Com_Printf( S_COLOR_YELLOW "%sstep %i skipped: %s\n", prefix, r->pc + 1, error ? error : "nothing to send" );
				r->pc++;
				continue;
			}
			Com_Printf( S_COLOR_GREEN "%s" S_COLOR_WHITE "%s\n", prefix, cmd );
			if ( local ) {
				Cbuf_AddText( cmd );
				Cbuf_AddText( "\n" );
			} else {
				CL_AddReliableCommand( cmd, qfalse );
			}
			r->sent++;
			// a console command may well send one too
			st.nextSend = cls.realtime + ST_CommandGap();
			if ( s->action == ST_A_CAM_SHOT && Q_stricmp( s->values[1], "no" ) ) {
				char hook[MAX_STRING_CHARS];
				int ms;

				if ( CL_RpCinematic_ShotHook( s->values[0], hook, sizeof( hook ), &ms ) )
					r->shotUntil = cls.realtime + ms;
			}
			if ( ST_StepExtra( s ) )
				r->extraDue = qtrue;
			else
				r->pc++;
			return qfalse;
		}
	}
	return qfalse;
}

// what keeps a story from being armed, NULL if nothing
static const char *ST_ArmProblem( const stStep_t *steps, int numSteps ) {
	const char *error;
	qboolean local;

	for ( int i = 0; i < numSteps; i++ ) {
		if ( !steps[i].enabled )
			continue;
		if ( steps[i].action != ST_A_TRIGGER )
			return "its first step has to be \"When someone comes\"";
		ST_StepCommand( &steps[i], &local, &error );
		return error;
	}
	return "it has no steps";
}

static void ST_Disarm( int index ) {
	Z_Free( stArmed[index].steps );
	stNumArmed--;
	memmove( &stArmed[index], &stArmed[index + 1], ( stNumArmed - index ) * sizeof( stArmed[0] ) );
}

static int ST_FindArmed( const char *name ) {
	for ( int i = 0; i < stNumArmed; i++ ) {
		if ( !Q_stricmp( stArmed[i].name, name ) )
			return i;
	}
	return -1;
}

// "once", "every time" or "3 times"
static const char *ST_ArmedHow( int limit ) {
	static char out[32];

	if ( !limit )
		return "every time";
	if ( limit == 1 )
		return "once";
	Com_sprintf( out, sizeof( out ), "%i times", limit );
	return out;
}

// an armed one, its plays and how many there were
static const char *ST_ArmedState( const stArmed_t *a ) {
	static char out[64];

	if ( a->limit > 1 )
		Com_sprintf( out, sizeof( out ), "%s, %i left", ST_ArmedHow( a->limit ), a->limit - a->times );
	else
		Com_sprintf( out, sizeof( out ), "%s, set off %i time%s", ST_ArmedHow( a->limit ), a->times, a->times == 1 ? "" : "s" );
	return out;
}

// a copy of the story waits for its first step, to play limit times (0 every time); one by that
// name already armed is replaced
static qboolean ST_Arm( const char *name, const stStep_t *steps, int numSteps, int limit ) {
	const char *why = ST_ArmProblem( steps, numSteps );
	int i = ST_FindArmed( name );
	stArmed_t *a;

	if ( cls.state != CA_ACTIVE )
		why = "join a server first";
	if ( why ) {
		ST_Message( va( S_COLOR_YELLOW "Can't arm %s: %s", name, why ) );
		Com_Printf( S_COLOR_YELLOW "Storyteller: can't arm %s: %s\n", name, why );
		return qfalse;
	}
	if ( i >= 0 ) {
		Z_Free( stArmed[i].steps );
	} else {
		if ( stNumArmed >= ST_MAX_ARMED ) {
			ST_Message( va( S_COLOR_YELLOW "%i stories are armed already: disarm one first", ST_MAX_ARMED ) );
			Com_Printf( S_COLOR_YELLOW "Storyteller: %i stories are armed already\n", ST_MAX_ARMED );
			return qfalse;
		}
		i = stNumArmed++;
	}
	a = &stArmed[i];
	memset( a, 0, sizeof( *a ) );
	Q_strncpyz( a->name, name, sizeof( a->name ) );
	Q_strncpyz( a->map, cl.mapname, sizeof( a->map ) );
	a->steps = (stStep_t *)Z_Malloc( numSteps * sizeof( stStep_t ), TAG_GENERAL, qfalse );
	memcpy( a->steps, steps, numSteps * sizeof( stStep_t ) );
	a->numSteps = numSteps;
	a->limit = limit;
	ST_StartRun( &a->run, 0 );
	ST_Message( va( S_COLOR_GREEN "Armed %s: it plays %s", name, !limit ? "every time it's set off" : limit == 1 ? "the next time it's set off"
		: va( "the next %i times it's set off", limit ) ) );
	Com_Printf( S_COLOR_GREEN "Storyteller: " S_COLOR_WHITE "armed %s (%s)\n", name, ST_ArmedHow( limit ) );
	return qtrue;
}

static qboolean ST_AtTrigger( const stRun_t *r, const stStep_t *steps, int numSteps ) {
	return (qboolean)( r->pc < numSteps && steps[r->pc].action == ST_A_TRIGGER && cls.realtime >= r->shotUntil );
}

static void ST_RunArmed( void ) {
	for ( int i = 0; i < stNumArmed; i++ ) {
		stArmed_t *a = &stArmed[i];

		// the triggers are the map's
		if ( Q_stricmp( a->map, cl.mapname ) ) {
			Com_Printf( S_COLOR_YELLOW "Storyteller: %s disarmed: the map changed\n", a->name );
			ST_Disarm( i-- );
			continue;
		}
		if ( !a->run.running )
			ST_StartRun( &a->run, 0 );
		if ( !ST_Play( &a->run, a->steps, a->numSteps, a->name ) )
			continue;
		a->times++;
		if ( !a->limit ) {
			Com_Printf( S_COLOR_GREEN "Storyteller (%s): " S_COLOR_WHITE "played, armed again\n", a->name );
			ST_StartRun( &a->run, 0 );
		} else if ( a->times < a->limit ) {
			Com_Printf( S_COLOR_GREEN "Storyteller (%s): " S_COLOR_WHITE "played %i of %i times, armed again\n", a->name, a->times, a->limit );
			ST_StartRun( &a->run, 0 );
		} else {
			Com_Printf( S_COLOR_GREEN "Storyteller (%s): " S_COLOR_WHITE "played, disarmed\n", a->name );
			ST_Disarm( i-- );
		}
	}
}

// every frame in game, open or not
static void ST_RunFrame( void ) {
	if ( st.run.running && ST_Play( &st.run, st.steps, st.numSteps, NULL ) )
		ST_Stop( va( S_COLOR_GREEN "The story is over (%i command%s sent)", st.run.sent, st.run.sent == 1 ? "" : "s" ) );
	ST_RunArmed();
}

/*
===============================================================================

FILES

===============================================================================
*/

static qboolean ST_NameChar( int ch ) {
	return (qboolean)( isalnum( ch ) || ch == '_' || ch == '-' );
}

static const char *ST_StoryPath( const char *name ) {
	return va( "%s/%s%s", ST_SAVE_DIR, name, ST_SAVE_EXT );
}

static void ST_WriteLine( fileHandle_t f, const char *text ) {
	FS_Write( text, strlen( text ), f );
}

static qboolean ST_Save( const char *name ) {
	fileHandle_t f;

	if ( !name[0] )
		return qfalse;
	f = FS_FOpenFileWrite( ST_StoryPath( name ) );
	if ( !f ) {
		ST_Message( va( S_COLOR_RED "Couldn't write %s", ST_StoryPath( name ) ) );
		return qfalse;
	}
	ST_WriteLine( f, "// Storyteller story: action, 1 if it runs, then its settings\n" );
	for ( int i = 0; i < st.numSteps; i++ ) {
		const stStep_t *s = &st.steps[i];
		const stActionDef_t *def = ST_Def( s );

		ST_WriteLine( f, va( "%s %i", def->key, s->enabled ? 1 : 0 ) );
		for ( int j = 0; j < def->numFields; j++ )
			ST_WriteLine( f, va( " \"%s\"", s->values[j] ) );
		ST_WriteLine( f, "\n" );
	}
	FS_FCloseFile( f );
	Q_strncpyz( st.name, name, sizeof( st.name ) );
	st.dirty = qfalse;
	ST_Message( va( S_COLOR_GREEN "Saved %s (%i step%s)", name, st.numSteps, st.numSteps == 1 ? "" : "s" ) );
	return qtrue;
}

// splits a line into words, "quoted" ones whole; the line is cut up in place
static int ST_SplitLine( char *line, char **argv, int max ) {
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

// a saved story's steps; how many, -1 if there is none
static int ST_ReadStory( const char *name, stStep_t *steps, int max, int *skipped ) {
	char *buf, *line, *next, *argv[ST_MAX_FIELDS + 2];
	int len, argc, count = 0;
	stStep_t *s;

	*skipped = 0;
	len = FS_ReadFile( ST_StoryPath( name ), (void **)&buf );
	if ( len < 0 || !buf )
		return -1;

	for ( line = buf; line && *line; line = next ) {
		next = strchr( line, '\n' );
		if ( next )
			*next++ = '\0';
		len = strlen( line );
		if ( len && line[len - 1] == '\r' )
			line[len - 1] = '\0';
		if ( !line[0] || ( line[0] == '/' && line[1] == '/' ) )
			continue;

		argc = ST_SplitLine( line, argv, ARRAY_LEN( argv ) );
		if ( argc < 2 )
			continue;
		int action = -1;
		for ( int i = 0; i < ST_NUM_ACTIONS; i++ ) {
			if ( !Q_stricmp( argv[0], stActions[i].key ) )
				action = i;
		}
		if ( action < 0 || count >= max ) {
			( *skipped )++;
			continue;
		}
		s = &steps[count++];
		memset( s, 0, sizeof( *s ) );
		s->action = (stAction_t)action;
		s->enabled = (qboolean)( atoi( argv[1] ) != 0 );
		ST_SetDefaults( s );
		for ( int i = 2; i < argc && i - 2 < ST_Def( s )->numFields; i++ )
			Q_strncpyz( s->values[i - 2], argv[i], ST_VALUE_LEN );
	}
	FS_FreeFile( buf );
	return count;
}

static qboolean ST_Load( const char *name ) {
	int skipped;

	if ( FS_ReadFile( ST_StoryPath( name ), NULL ) < 0 ) {
		ST_Message( va( S_COLOR_RED "No story called %s", name ) );
		return qfalse;
	}
	ST_New();
	st.numSteps = Q_max( 0, ST_ReadStory( name, st.steps, ST_MAX_STEPS, &skipped ) );

	Q_strncpyz( st.name, name, sizeof( st.name ) );
	st.dirty = qfalse;
	st.stepList.sel = st.numSteps ? 0 : -1;
	if ( skipped )
		ST_Message( va( S_COLOR_YELLOW "Loaded %s; %i line%s not understood", name, skipped, skipped == 1 ? "" : "s" ) );
	else
		ST_Message( va( S_COLOR_GREEN "Loaded %s (%i step%s)", name, st.numSteps, st.numSteps == 1 ? "" : "s" ) );
	return qtrue;
}

// a saved story armed, by the console
static void ST_ArmSaved( const char *name, int limit ) {
	stStep_t *steps = (stStep_t *)Z_Malloc( ST_MAX_STEPS * sizeof( stStep_t ), TAG_GENERAL, qfalse );
	int skipped;
	const int count = ST_ReadStory( name, steps, ST_MAX_STEPS, &skipped );

	if ( count < 0 )
		Com_Printf( "Storyteller: no story called %s\n", name );
	else
		ST_Arm( name, steps, count, limit );
	Z_Free( steps );
}
static int QDECL ST_CompareNames( const void *a, const void *b ) {
	return Q_stricmp( (const char *)a, (const char *)b );
}

static void ST_RefreshStories( void ) {
	char list[16384], *name;
	int count;

	st.numStories = 0;
	count = FS_GetFileList( ST_SAVE_DIR, ST_SAVE_EXT, list, sizeof( list ) );
	name = list;
	for ( int i = 0; i < count && st.numStories < ST_MAX_STORIES; i++ ) {
		const int len = strlen( name );
		char *ext;

		Q_strncpyz( st.stories[st.numStories], name, MAX_QPATH );
		ext = strstr( st.stories[st.numStories], ST_SAVE_EXT );
		if ( ext )
			*ext = '\0';
		if ( st.stories[st.numStories][0] )
			st.numStories++;
		name += len + 1;
	}
	qsort( st.stories, st.numStories, sizeof( st.stories[0] ), ST_CompareNames );
}

static void ST_OpenSave( void ) {
	st.popup = ST_POPUP_SAVE;
	Q_strncpyz( st.saveName, st.name, sizeof( st.saveName ) );
	st.focus = ST_F_SAVE_NAME;
}

static void ST_OpenLoad( void ) {
	ST_RefreshStories();
	st.popup = ST_POPUP_LOAD;
	memset( &st.storyList, 0, sizeof( st.storyList ) );
	st.storyList.lastClickRow = -1;
	st.focus = ST_F_NONE;
}

static void ST_DoSave( void ) {
	if ( !st.saveName[0] ) {
		ST_Message( S_COLOR_YELLOW "Type a name" );
		return;
	}
	if ( ST_Save( st.saveName ) ) {
		st.popup = ST_POPUP_NONE;
		st.focus = ST_F_NONE;
	}
}

/*
===============================================================================

OPEN / CLOSE

===============================================================================
*/

static void ST_BuildActionRows( void ) {
	const char *category = NULL;

	st.numActionRows = 0;
	for ( int i = 0; i < ST_NUM_ACTIONS; i++ ) {
		if ( !category || Q_stricmp( category, stActions[i].category ) ) {
			category = stActions[i].category;
			st.actionRows[st.numActionRows].header = category;
			st.numActionRows++;
		}
		st.actionRows[st.numActionRows].header = NULL;
		st.actionRows[st.numActionRows].action = (stAction_t)i;
		st.numActionRows++;
	}
}

static qboolean ST_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Storyteller: join a server first\n" );
		return qfalse;
	}
	CL_ModelManager_Close();
	CL_NpcManager_Close();
	CL_ShaderManager_Close();
	CL_EffectManager_Close();
	CL_RpCinematic_EditorClose();
	CL_SoundManager_Close();

	st.font = re->RegisterFont( "arialnb" );
	if ( !st.font )
		st.font = cls.menuFont;
	if ( !st.numActionRows ) {
		ST_BuildActionRows();
		st.actionList.sel = 1;
		st.actionList.lastClickRow = -1;
	}

	st.open = qtrue;
	st.popup = ST_POPUP_NONE;
	st.focus = ST_F_NONE;
	st.cursorX = SCREEN_WIDTH * 0.5f;
	st.cursorY = SCREEN_HEIGHT * 0.5f;
	st.click = qfalse;
	st.wheel = 0;
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_STORYTELLER );
	return qtrue;
}

void CL_Storyteller_Close( void ) {
	if ( !st.open )
		return;
	st.open = qfalse;
	st.popup = ST_POPUP_NONE;
	st.focus = ST_F_NONE;
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_STORYTELLER );
}

void CL_Storyteller_Open( void ) {
	if ( !st.open )
		ST_Open();
}

// the camera editor saved a shot it was opened on: the step takes the name it went under
void CL_Storyteller_ShotSaved( const char *name ) {
	stStep_t *s = ST_SelectedStep();

	if ( s && s->action == ST_A_CAM_SHOT && Q_stricmp( s->values[0], name ) ) {
		Q_strncpyz( s->values[0], name, ST_VALUE_LEN );
		ST_Changed();
	}
}

/*
===============================================================================

Picking with the managers

===============================================================================
*/

// a step's manager, opened on the world; what it would send the server comes back here
static void ST_StartPick( stPick_t pick ) {
	st.pick = pick;
	st.pickStep = st.stepList.sel;
	st.numPicked = 0;
	if ( pick == ST_PICK_MODEL )
		CL_ModelManager_OpenForStory();
	else if ( pick == ST_PICK_EFFECT )
		CL_EffectManager_OpenForStory();
	else if ( pick == ST_PICK_SHADER )
		CL_ShaderManager_OpenForStory();
	else if ( pick == ST_PICK_SOUND )
		CL_SoundManager_OpenForStory();
	else if ( pick == ST_PICK_MUSIC )
		CL_MusicManager_OpenForStory();
	else
		CL_NpcManager_OpenForStory( (qboolean)( pick == ST_PICK_NPC_MANAGE ) );
}

static qboolean ST_PickOpen( void ) {
	switch ( st.pick ) {
	case ST_PICK_MODEL:		return CL_ModelManager_Active();
	case ST_PICK_EFFECT:	return CL_EffectManager_Active();
	case ST_PICK_SHADER:	return CL_ShaderManager_Active();
	case ST_PICK_NPC_SPAWN:
	case ST_PICK_NPC_MANAGE:	return CL_NpcManager_Active();
	case ST_PICK_SOUND:		return CL_SoundManager_Active();
	case ST_PICK_MUSIC:		return CL_MusicManager_Active();
	default:				return qfalse;
	}
}

// a manager opened from a step: what it sends goes in the story
qboolean CL_Storyteller_Picking( void ) {
	return (qboolean)( st.pick != ST_PICK_NONE && ST_PickOpen() );
}

// a manager about to send a command: qtrue keeps it for the story. Lists only read, they go
qboolean CL_Storyteller_Capture( const char *cmd ) {
	const char *last;

	if ( st.pick == ST_PICK_NONE || !ST_PickOpen() )
		return qfalse;
	last = strrchr( cmd, ' ' );
	if ( last && !Q_stricmp( last, " list" ) )
		return qfalse;
	if ( st.numPicked < ST_MAX_PICKED )
		Q_strncpyz( st.picked[st.numPicked++], cmd, sizeof( st.picked[0] ) );
	return qtrue;
}

static qboolean ST_IsNumber( const char *s ) {
	if ( *s == '-' || *s == '+' )
		s++;
	if ( !*s )
		return qfalse;
	for ( ; *s; s++ ) {
		if ( !isdigit( (unsigned char)*s ) && *s != '.' )
			return qfalse;
	}
	return qtrue;
}

static qboolean ST_AllNumbers( char **argv, int from, int to ) {
	for ( int i = from; i < to; i++ ) {
		if ( !ST_IsNumber( argv[i] ) )
			return qfalse;
	}
	return qtrue;
}

static void ST_Join( char **argv, int from, int to, char *out, int size ) {
	out[0] = '\0';
	for ( int i = from; i < to; i++ )
		Q_strcat( out, size, va( i > from ? " %s" : "%s", argv[i] ) );
}

// the NPC manager's commands: npc spawn, tele, and the ones the NPC command step runs
static qboolean ST_NpcStep( char **argv, int argc, stStep_t *s ) {
	const char *verb = argv[1], *target = argv[argc - 1], *command = NULL;
	char value[ST_VALUE_LEN] = "";

	// npc spawn [vehicle] <type> [<targetname> [<team> [<model> [<x y z yaw> [<scale>]]]]]
	if ( !Q_stricmp( verb, "spawn" ) ) {
		int at = 2;
		qboolean vehicle = qfalse;

		if ( !Q_stricmp( argv[at], "vehicle" ) && argc > 3 ) {
			vehicle = qtrue;
			at++;
		}
		if ( argc - at > 9 || ( argc - at >= 5 && !ST_AllNumbers( argv, at + 4, argc ) ) )
			return qfalse;
		s->action = ST_A_NPC_SPAWN;
		ST_SetDefaults( s );
		Q_strncpyz( s->values[0], argv[at], ST_VALUE_LEN );
		Q_strncpyz( s->values[3], vehicle ? "yes" : "no", ST_VALUE_LEN );
		if ( argc > at + 1 )
			Q_strncpyz( s->values[1], argv[at + 1], ST_VALUE_LEN );
		if ( argc > at + 2 )
			Q_strncpyz( s->values[2], !Q_stricmp( argv[at + 2], "player" ) || !Q_stricmp( argv[at + 2], "enemy" ) ? argv[at + 2] : "default", ST_VALUE_LEN );
		if ( argc > at + 3 )
			Q_strncpyz( s->values[4], argv[at + 3], ST_VALUE_LEN );
		if ( argc >= at + 8 ) {
			for ( int i = 0; i < 4; i++ )
				Q_strncpyz( s->values[6 + i], argv[at + 4 + i], ST_VALUE_LEN );
		} else {
			// in front of the player: where the storyteller stands now
			ST_FillHere( s );
		}
		if ( argc == at + 9 )
			Q_strncpyz( s->values[5], argv[at + 8], ST_VALUE_LEN );
		return qtrue;
	}

	// npc tele <x y z yaw> <target>
	if ( !Q_stricmp( verb, "tele" ) ) {
		if ( argc != 7 || !ST_AllNumbers( argv, 2, 6 ) )
			return qfalse;
		s->action = ST_A_NPC_TELE;
		Q_strncpyz( s->values[0], target, ST_VALUE_LEN );
		for ( int i = 0; i < 4; i++ )
			Q_strncpyz( s->values[1 + i], argv[2 + i], ST_VALUE_LEN );
		return qtrue;
	}

	if ( !Q_stricmp( verb, "kill" ) && argc == 3 && Q_stricmp( target, "spawners" ) ) {
		command = "kill";
	} else if ( !Q_stricmp( verb, "freeze" ) && argc == 4 && ( !strcmp( argv[3], "1" ) || !strcmp( argv[3], "0" ) ) ) {
		command = !strcmp( argv[3], "1" ) ? "freeze" : "unfreeze";
		target = argv[2];
	} else if ( ( !Q_stricmp( verb, "emote" ) || !Q_stricmp( verb, "dialog" ) ) && argc >= 4 ) {
		// the emote or dialog, then the target
		ST_Join( argv, 2, argc - 1, value, sizeof( value ) );
		if ( !Q_stricmp( value, "none" ) ) {
			command = !Q_stricmp( verb, "emote" ) ? "stop emote" : "no dialog";
			value[0] = '\0';
		} else {
			command = !Q_stricmp( verb, "emote" ) ? "emote" : "dialog";
		}
	} else if ( !Q_stricmp( verb, "follow" ) && argc == 4 ) {
		if ( !Q_stricmp( argv[2], "none" ) )
			command = "stop following";
		else if ( atoi( argv[2] ) == clc.clientNum && ST_IsNumber( argv[2] ) )
			command = "follow me";
	} else if ( !Q_stricmp( verb, "hologram" ) && argc == 4 ) {
		if ( !Q_stricmp( argv[2], "normal" ) )			command = "hologram";
		else if ( !Q_stricmp( argv[2], "projector" ) )	command = "projector";
		else if ( !Q_stricmp( argv[2], "off" ) )		command = "no hologram";
	}
	if ( !command )
		return qfalse;
	s->action = ST_A_NPC_CMD;
	Q_strncpyz( s->values[0], target, ST_VALUE_LEN );
	Q_strncpyz( s->values[1], command, ST_VALUE_LEN );
	Q_strncpyz( s->values[2], value, ST_VALUE_LEN );
	return qtrue;
}

// rpnotify -1 1 cmd:npc/talk/<npc>/<name>/<colour>/<emote>: <text>, as NPC talk sends it
static qboolean ST_TalkStep( const char *cmd, stStep_t *s ) {
	static const char prefix[] = "rpnotify -1 1 cmd:npc/talk/";
	const char *p = cmd + strlen( prefix ), *colon;

	if ( Q_stricmpn( cmd, prefix, strlen( prefix ) ) || !( colon = strchr( p, ':' ) ) )
		return qfalse;
	s->action = ST_A_NPC_TALK;
	ST_SetDefaults( s );
	// NPC, name, colour and emote, then the text
	for ( int part = 0; part < 4 && p < colon; part++ ) {
		const char *end = p;

		while ( end < colon && *end != '/' )
			end++;
		Q_strncpyz( s->values[part], p, Q_min( ST_VALUE_LEN, (int)( end - p ) + 1 ) );
		p = end < colon ? end + 1 : end;
	}
	for ( p = colon + 1; *p == ' '; p++ )
		;
	Q_strncpyz( s->values[4], p, ST_VALUE_LEN );
	return qtrue;
}

// a manager's command as a step of its own kind, or one that runs it as it is
static void ST_StepFromCommand( const char *cmd, stStep_t *s ) {
	char line[MAX_STRING_CHARS];
	char *argv[16];
	int argc;

	memset( s, 0, sizeof( *s ) );
	s->enabled = qtrue;
	Q_strncpyz( line, cmd, sizeof( line ) );
	argc = ST_SplitLine( line, argv, ARRAY_LEN( argv ) );

	// rpmodel add <model> <x y z> <pitch yaw roll> <scale> <solid>
	if ( argc == 10 && !Q_stricmp( argv[0], "rpmodel" ) && !Q_stricmp( argv[1], "add" ) && ST_AllNumbers( argv, 3, 10 ) ) {
		s->action = ST_A_MODEL;
		for ( int i = 0; i < 8; i++ )
			Q_strncpyz( s->values[i], argv[i + 2], ST_VALUE_LEN );
		Q_strncpyz( s->values[8], atoi( argv[9] ) ? "yes" : "no", ST_VALUE_LEN );
		return;
	}
	// rpeffect once|add <effect> [<x y z> [<pitch yaw roll>]]; one at the player, with no spot or
	// 0 0 0, is where the storyteller stands now
	if ( ( argc == 3 || argc == 6 || argc == 9 ) && !Q_stricmp( argv[0], "rpeffect" )
		&& ( !Q_stricmp( argv[1], "once" ) || !Q_stricmp( argv[1], "add" ) ) && ST_AllNumbers( argv, 3, argc ) ) {
		s->action = ST_A_EFFECT;
		Q_strncpyz( s->values[0], argv[2], ST_VALUE_LEN );
		Q_strncpyz( s->values[1], !Q_stricmp( argv[1], "add" ) ? "loop" : "once", ST_VALUE_LEN );
		for ( int i = 0; i < 6; i++ )
			Q_strncpyz( s->values[i + 2], i + 3 < argc ? argv[i + 3] : "0", ST_VALUE_LEN );
		if ( argc == 3 || ( !atof( argv[3] ) && !atof( argv[4] ) && !atof( argv[5] ) ) ) {
			char yaw[ST_VALUE_LEN];

			// its own angles, not the way the storyteller looks
			Q_strncpyz( yaw, s->values[6], sizeof( yaw ) );
			ST_FillHere( s );
			Q_strncpyz( s->values[6], yaw, ST_VALUE_LEN );
		}
		return;
	}
	// the effect manager's presets
	if ( argc >= 2 && !Q_stricmp( argv[0], "rpeffect" ) ) {
		const char *screen = NULL;

		if ( argc == 2 && !Q_stricmp( argv[1], "fadeout" ) )										screen = "fade out";
		else if ( argc == 3 && !Q_stricmp( argv[1], "remove" ) && !Q_stricmp( argv[2], "fadeout" ) )	screen = "fade in";
		else if ( argc == 2 && !Q_stricmp( argv[1], "alarm" ) )										screen = "alarm";
		else if ( argc == 3 && !Q_stricmp( argv[1], "remove" ) && !Q_stricmp( argv[2], "alarm" ) )	screen = "stop alarm";
		else if ( argc == 4 && !Q_stricmp( argv[1], "camerashake" ) )								screen = "camera shake";
		if ( screen ) {
			s->action = ST_A_SCREEN;
			ST_SetDefaults( s );
			Q_strncpyz( s->values[0], screen, ST_VALUE_LEN );
			if ( argc == 4 ) {
				Q_strncpyz( s->values[1], argv[2], ST_VALUE_LEN );
				Q_strncpyz( s->values[2], argv[3], ST_VALUE_LEN );
			}
			return;
		}
	}
	if ( argc >= 3 && !Q_stricmp( argv[0], "npc" ) && ST_NpcStep( argv, argc, s ) )
		return;
	if ( ST_TalkStep( cmd, s ) )
		return;
	// rpsound once|add|local <sound> [all | -1 | <player> | <x y z>]
	if ( argc >= 3 && !Q_stricmp( argv[0], "rpsound" )
		&& ( !Q_stricmp( argv[1], "once" ) || !Q_stricmp( argv[1], "add" ) || !Q_stricmp( argv[1], "local" ) )
		&& ( argc <= 4 || ( argc == 6 && ST_AllNumbers( argv, 3, 6 ) ) ) ) {
		s->action = ST_A_SOUND;
		ST_SetDefaults( s );
		Q_strncpyz( s->values[0], argv[2], ST_VALUE_LEN );
		Q_strncpyz( s->values[1], !Q_stricmp( argv[1], "add" ) ? "loop" : !Q_stricmp( argv[1], "local" ) ? "local" : "once", ST_VALUE_LEN );
		if ( argc == 3 ) {
			Q_strncpyz( s->values[2], !Q_stricmp( argv[1], "add" ) ? "everyone" : "me", ST_VALUE_LEN );
		} else if ( argc == 4 && ( !Q_stricmp( argv[3], "all" ) || !strcmp( argv[3], "-1" ) ) ) {
			Q_strncpyz( s->values[2], "everyone", ST_VALUE_LEN );
		} else if ( argc == 4 ) {
			Q_strncpyz( s->values[2], "player", ST_VALUE_LEN );
			Q_strncpyz( s->values[3], argv[3], ST_VALUE_LEN );
		} else {
			Q_strncpyz( s->values[2], "spot", ST_VALUE_LEN );
			for ( int i = 0; i < 3; i++ )
				Q_strncpyz( s->values[4 + i], argv[3 + i], ST_VALUE_LEN );
		}
		return;
	}
	// rpmusic <music> | default | reset | none
	if ( argc == 2 && !Q_stricmp( argv[0], "rpmusic" ) ) {
		s->action = ST_A_MUSIC;
		ST_SetDefaults( s );
		if ( !Q_stricmp( argv[1], "default" ) || !Q_stricmp( argv[1], "reset" ) ) {
			Q_strncpyz( s->values[0], "map default", ST_VALUE_LEN );
		} else if ( !Q_stricmp( argv[1], "none" ) ) {
			Q_strncpyz( s->values[0], "none", ST_VALUE_LEN );
		} else {
			Q_strncpyz( s->values[0], "play", ST_VALUE_LEN );
			Q_strncpyz( s->values[1], argv[1], ST_VALUE_LEN );
		}
		return;
	}
	// rpshader remap <from> <to> [<time offset>]
	if ( ( argc == 4 || argc == 5 ) && !Q_stricmp( argv[0], "rpshader" ) && !Q_stricmp( argv[1], "remap" ) ) {
		s->action = ST_A_SHADER;
		Q_strncpyz( s->values[0], argv[2], ST_VALUE_LEN );
		Q_strncpyz( s->values[1], argv[3], ST_VALUE_LEN );
		if ( argc == 5 )
			Q_strncpyz( s->values[2], argv[4], ST_VALUE_LEN );
		return;
	}
	s->action = ST_A_COMMAND;
	Q_strncpyz( s->values[0], cmd, ST_VALUE_LEN );
}

// what the manager sent, into the story: the first fills the step it was opened from if it's
// that kind, the rest go after it
static void ST_TakePicked( void ) {
	int from = st.pickStep;

	if ( from < 0 || from >= st.numSteps )
		from = st.numSteps - 1;
	ST_SelectStep( from );
	for ( int i = 0; i < st.numPicked; i++ ) {
		stStep_t step;

		ST_StepFromCommand( st.picked[i], &step );
		if ( i == 0 && from >= 0 && st.steps[from].action == step.action ) {
			step.enabled = st.steps[from].enabled;
			st.steps[from] = step;
			ST_Changed();
			continue;
		}
		if ( st.numSteps >= ST_MAX_STEPS ) {
			ST_Message( S_COLOR_RED "The story is full" );
			break;
		}
		ST_AddStep( step.action );
		st.steps[st.stepList.sel] = step;
	}
	ST_Message( va( S_COLOR_GREEN "%i step%s from the manager", st.numPicked, st.numPicked == 1 ? "" : "s" ) );
}

// every frame: once the manager has sent something, or was closed, back to the story
static void ST_PickFrame( void ) {
	if ( st.pick == ST_PICK_NONE )
		return;
	if ( ST_PickOpen() && !st.numPicked )
		return;
	CL_ModelManager_Close();
	CL_EffectManager_Close();
	CL_ShaderManager_Close();
	CL_NpcManager_Close();
	CL_SoundManager_Close();
	st.pick = ST_PICK_NONE;
	// another tool opened from it stays
	if ( !( Key_GetCatcher() & KEYCATCH_ENGINETOOLS ) && ST_Open() && st.numPicked )
		ST_TakePicked();
	st.numPicked = 0;
}

void CL_Storyteller_Init( void ) {
	memset( &st, 0, sizeof( st ) );
	st.stepList.sel = -1;
	st.stepList.lastClickRow = -1;
	st.focus = ST_F_NONE;
}

// cgame is going away: map change, disconnect or vid_restart; the story stays, the play doesn't
void CL_Storyteller_Shutdown( void ) {
	CL_Storyteller_Close();
	ST_Stop( S_COLOR_YELLOW "Stopped: left the map" );
	// armed ones wait on their first step again, if the map is still theirs
	for ( int i = 0; i < stNumArmed; i++ )
		stArmed[i].run.running = qfalse;
}

qboolean CL_Storyteller_Active( void ) {
	return st.open;
}

void CL_Storyteller_Frame( void ) {
	ST_PickFrame();
	ST_RunFrame();
}

/*
===============================================================================

CONSOLE COMMAND

===============================================================================
*/

void CL_Storyteller_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !arg[0] ) {
		if ( st.open )
			CL_Storyteller_Close();
		else
			ST_Open();
	} else if ( !Q_stricmp( arg, "close" ) ) {
		CL_Storyteller_Close();
	} else if ( !Q_stricmp( arg, "continue" ) ) {
		if ( !ST_AnyWaiting() )
			Com_Printf( "Storyteller: nothing is waiting for you\n" );
		ST_Continue();
	} else if ( !Q_stricmp( arg, "play" ) ) {
		if ( Cmd_Argc() >= 3 && !ST_Load( Cmd_Argv( 2 ) ) ) {
			Com_Printf( "Storyteller: no story called %s\n", Cmd_Argv( 2 ) );
			return;
		}
		ST_Run( 0 );
	} else if ( !Q_stricmp( arg, "stop" ) ) {
		if ( st.run.running )
			ST_Stop( S_COLOR_YELLOW "Stopped" );
	} else if ( !Q_stricmp( arg, "load" ) && Cmd_Argc() >= 3 ) {
		if ( ST_Load( Cmd_Argv( 2 ) ) )
			Com_Printf( "Storyteller: loaded %s, %i steps\n", st.name, st.numSteps );
		else
			Com_Printf( "Storyteller: no story called %s\n", Cmd_Argv( 2 ) );
	} else if ( !Q_stricmp( arg, "save" ) && Cmd_Argc() >= 3 ) {
		const char *name = Cmd_Argv( 2 );
		for ( const char *c = name; *c; c++ ) {
			if ( !ST_NameChar( *c ) ) {
				Com_Printf( "Storyteller: names take letters, digits, _ and -\n" );
				return;
			}
		}
		if ( ST_Save( name ) )
			Com_Printf( "Storyteller: saved %s\n", name );
	} else if ( !Q_stricmp( arg, "arm" ) && Cmd_Argc() >= 3 ) {
		const char *how = Cmd_Argv( 3 );

		// once, again for every time, or a number of times
		if ( !how[0] )
			ST_ArmSaved( Cmd_Argv( 2 ), 1 );
		else if ( !Q_stricmp( how, "again" ) )
			ST_ArmSaved( Cmd_Argv( 2 ), 0 );
		else if ( ST_IsNumber( how ) && atoi( how ) >= 1 )
			ST_ArmSaved( Cmd_Argv( 2 ), atoi( how ) );
		else
			Com_Printf( "Storyteller: arm it once, again (every time), or a number of times\n" );
	} else if ( !Q_stricmp( arg, "disarm" ) ) {
		const char *name = Cmd_Argv( 2 );
		const int i = ST_FindArmed( name );

		if ( !name[0] || !Q_stricmp( name, "all" ) ) {
			while ( stNumArmed )
				ST_Disarm( 0 );
			Com_Printf( "Storyteller: nothing is armed now\n" );
		} else if ( i < 0 ) {
			Com_Printf( "Storyteller: %s isn't armed\n", name );
		} else {
			ST_Disarm( i );
			Com_Printf( "Storyteller: %s disarmed\n", name );
		}
	} else if ( !Q_stricmp( arg, "armed" ) ) {
		Com_Printf( "Storyteller: %i armed\n", stNumArmed );
		for ( int i = 0; i < stNumArmed; i++ ) {
			const stArmed_t *a = &stArmed[i];
			Com_Printf( "  %s: %s, %s\n", a->name, ST_ArmedState( a ),
				ST_AtTrigger( &a->run, a->steps, a->numSteps ) ? "waiting" : "playing" );
		}
	} else if ( !Q_stricmp( arg, "list" ) ) {
		ST_RefreshStories();
		Com_Printf( "Storyteller: %i stor%s\n", st.numStories, st.numStories == 1 ? "y" : "ies" );
		for ( int i = 0; i < st.numStories; i++ )
			Com_Printf( "  %s\n", st.stories[i] );
	} else {
		Com_Printf( "usage: storyteller                  toggle the storyteller (bind a key to it)\n" );
		Com_Printf( "       storyteller play [name]      play the story, or load that one and play it\n" );
		Com_Printf( "       storyteller continue         go on past a \"wait for me\" step (bind a key to it)\n" );
		Com_Printf( "       storyteller stop             stop playing\n" );
		Com_Printf( "       storyteller load <name>      load a saved story\n" );
		Com_Printf( "       storyteller save <name>      save the story\n" );
		Com_Printf( "       storyteller list             list the saved stories\n" );
		Com_Printf( "       storyteller arm <name> [again|<times>]  arm a saved story: it plays when its first step\n" );
		Com_Printf( "                                    is set off, once, every time with again, or that many times\n" );
		Com_Printf( "       storyteller disarm [name|all]  disarm one, or all\n" );
		Com_Printf( "       storyteller armed            list the armed stories\n" );
		Com_Printf( "       storyteller close            close the storyteller\n" );
	}
}

/*
===============================================================================

INPUT

===============================================================================
*/

static qboolean ST_InRect( float x, float y, float w, float h ) {
	return (qboolean)( st.cursorX >= x && st.cursorX < x + w && st.cursorY >= y && st.cursorY < y + h );
}

// binds don't run while the storyteller holds the keys, so honour the user's toggle bind here
static qboolean ST_IsToggleKey( int key ) {
	const char *binding;

	// a printable key belongs to the text box
	if ( st.focus != ST_F_NONE && key >= A_SPACE && key <= A_TILDE )
		return qfalse;
	binding = Key_GetBinding( key );
	return (qboolean)( VALIDSTRING( binding ) && !Q_stricmp( binding, ST_TOGGLE_CMD ) );
}

// the text being typed in, NULL if none
static char *ST_FocusText( int *size, stKind_t *kind ) {
	stStep_t *s;

	*kind = ST_K_TEXT;
	if ( st.focus == ST_F_SAVE_NAME ) {
		*size = sizeof( st.saveName );
		return st.saveName;
	}
	s = ST_SelectedStep();
	if ( st.focus < 0 || !s || st.focus >= ST_Def( s )->numFields || ST_Def( s )->fields[st.focus].kind == ST_K_CHOICE )
		return NULL;
	*size = ST_VALUE_LEN;
	*kind = ST_Def( s )->fields[st.focus].kind;
	return s->values[st.focus];
}

// the next text field of the step, or none past the last
static void ST_NextField( void ) {
	stStep_t *s = ST_SelectedStep();

	if ( !s || st.focus < 0 )
		return;
	for ( int i = st.focus + 1; i < ST_Def( s )->numFields; i++ ) {
		if ( ST_Def( s )->fields[i].kind != ST_K_CHOICE && ST_FieldShown( s, i ) ) {
			st.focus = i;
			return;
		}
	}
	st.focus = ST_F_NONE;
}

static void ST_ListMove( stList_t *list, int count, int delta ) {
	if ( !count )
		return;
	list->sel = Com_Clampi( 0, count - 1, list->sel + delta );
}

void CL_Storyteller_KeyEvent( int key, qboolean down ) {
	int size;
	stKind_t kind;
	char *text;

	if ( !st.open || !down )
		return;

	if ( ST_IsToggleKey( key ) ) {
		CL_Storyteller_Close();
		return;
	}
	if ( st.popup == ST_POPUP_EXPERIMENTAL && key != A_MOUSE1 )
		return;

	switch ( key ) {
	case A_MOUSE1:
		st.click = qtrue;
		st.clickX = st.cursorX;
		st.clickY = st.cursorY;
		return;
	case A_MWHEELUP:
		st.wheel -= 3;
		return;
	case A_MWHEELDOWN:
		st.wheel += 3;
		return;
	default:
		break;
	}

	text = ST_FocusText( &size, &kind );
	if ( text ) {
		switch ( key ) {
		case A_BACKSPACE:
			if ( text[0] ) {
				text[strlen( text ) - 1] = '\0';
				if ( st.focus != ST_F_SAVE_NAME )
					ST_Changed();
			}
			return;
		case A_ENTER:
		case A_KP_ENTER:
			if ( st.focus == ST_F_SAVE_NAME )
				ST_DoSave();
			else
				st.focus = ST_F_NONE;
			return;
		case A_TAB:
			if ( st.focus != ST_F_SAVE_NAME )
				ST_NextField();
			return;
		default:
			return;
		}
	}

	if ( st.popup == ST_POPUP_LOAD ) {
		switch ( key ) {
		case A_CURSOR_UP:
			ST_ListMove( &st.storyList, st.numStories, -1 );
			break;
		case A_CURSOR_DOWN:
			ST_ListMove( &st.storyList, st.numStories, 1 );
			break;
		case A_ENTER:
		case A_KP_ENTER:
			if ( st.storyList.sel >= 0 && st.storyList.sel < st.numStories && ST_Load( st.stories[st.storyList.sel] ) )
				st.popup = ST_POPUP_NONE;
			break;
		default:
			break;
		}
		return;
	}
	if ( st.popup != ST_POPUP_NONE )
		return;

	switch ( key ) {
	case A_CURSOR_UP:
		if ( Key_IsDown( A_CTRL ) )
			ST_MoveStep( -1 );
		else
			ST_SelectStep( Com_Clampi( 0, st.numSteps - 1, st.stepList.sel - 1 ) );
		break;
	case A_CURSOR_DOWN:
		if ( Key_IsDown( A_CTRL ) )
			ST_MoveStep( 1 );
		else if ( st.numSteps )
			ST_SelectStep( Com_Clampi( 0, st.numSteps - 1, st.stepList.sel + 1 ) );
		break;
	case A_DELETE:
		ST_DeleteStep();
		break;
	case A_ENTER:
	case A_KP_ENTER:
		if ( st.actionList.sel >= 0 && st.actionList.sel < st.numActionRows && !st.actionRows[st.actionList.sel].header
			&& ST_CanAdd( st.actionRows[st.actionList.sel].action ) )
			ST_AddStep( st.actionRows[st.actionList.sel].action );
		break;
	default:
		break;
	}
}

void CL_Storyteller_CharEvent( int ch ) {
	int size, len;
	stKind_t kind;
	char *text;

	if ( !st.open )
		return;
	text = ST_FocusText( &size, &kind );
	if ( !text || ch < ' ' || ch == '"' || ch > 126 )
		return;
	if ( st.focus == ST_F_SAVE_NAME && !ST_NameChar( ch ) )
		return;
	if ( kind == ST_K_NUMBER && !isdigit( ch ) && ch != '-' && ch != '.' )
		return;
	len = strlen( text );
	if ( len >= size - 1 )
		return;
	text[len] = (char)ch;
	text[len + 1] = '\0';
	if ( st.focus != ST_F_SAVE_NAME )
		ST_Changed();
}

void CL_Storyteller_MouseEvent( int dx, int dy ) {
	st.cursorX = Com_Clamp( 0, SCREEN_WIDTH, st.cursorX + dx );
	st.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, st.cursorY + dy );
}

// escape backs out one level: a popup, a text box, then the storyteller
void CL_Storyteller_Escape( void ) {
	if ( st.popup != ST_POPUP_NONE ) {
		st.popup = ST_POPUP_NONE;
		st.focus = ST_F_NONE;
	} else if ( st.focus != ST_F_NONE ) {
		st.focus = ST_F_NONE;
	} else {
		CL_Storyteller_Close();
	}
}

/*
===============================================================================

2D

===============================================================================
*/

static void ST_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void ST_Box( float x, float y, float w, float h, const float *fill ) {
	ST_Fill( x, y, w, h, fill );
	ST_Fill( x, y, w, 1, stBorder );
	ST_Fill( x, y + h - 1, w, 1, stBorder );
	ST_Fill( x, y, 1, h, stBorder );
	ST_Fill( x + w - 1, y, 1, h, stBorder );
}

static void ST_Text( float x, float y, const char *text, const float *color ) {
	re->Font_DrawString( (int)x, (int)y, text, color, st.font | STYLE_DROPSHADOW, -1, ST_TEXT_SCALE );
}

static float ST_TextWidth( const char *text ) {
	return (float)re->Font_StrLenPixels( text, st.font, ST_TEXT_SCALE );
}

static void ST_TextClipped( float x, float y, float w, const char *text, const float *color ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 3 && ST_TextWidth( buf ) > w ) {
		len--;
		buf[len - 3] = '.';
		buf[len - 2] = '.';
		buf[len - 1] = '.';
		buf[len] = '\0';
	}
	ST_Text( x, y, buf, color );
}

// what was typed, colour codes shown rather than applied: each ^ is drawn on its own
static float ST_RawWidth( const char *text ) {
	char seg[MAX_STRING_CHARS];
	float w = 0;

	while ( *text ) {
		int len = 0;
		if ( *text == Q_COLOR_ESCAPE ) {
			w += ST_TextWidth( "^" );
			text++;
			continue;
		}
		while ( text[len] && text[len] != Q_COLOR_ESCAPE && len < (int)sizeof( seg ) - 1 ) {
			seg[len] = text[len];
			len++;
		}
		seg[len] = '\0';
		w += ST_TextWidth( seg );
		text += len;
	}
	return w;
}

static void ST_RawText( float x, float y, const char *text, const float *color ) {
	char seg[MAX_STRING_CHARS];

	while ( *text ) {
		int len = 0;
		if ( *text == Q_COLOR_ESCAPE ) {
			ST_Text( x, y, "^", color );
			x += ST_TextWidth( "^" );
			text++;
			continue;
		}
		while ( text[len] && text[len] != Q_COLOR_ESCAPE && len < (int)sizeof( seg ) - 1 ) {
			seg[len] = text[len];
			len++;
		}
		seg[len] = '\0';
		ST_Text( x, y, seg, color );
		x += ST_TextWidth( seg );
		text += len;
	}
}

// text broken into lines at spaces; returns how many lines it took. raw shows colour codes
static int ST_TextWrappedEx( float x, float y, float w, const char *text, const float *color, int maxLines, qboolean raw ) {
	char line[MAX_STRING_CHARS];
	int lines = 0;

	while ( *text && lines < maxLines ) {
		int len = 0, fit = 0;

		while ( text[len] && len < (int)sizeof( line ) - 1 ) {
			line[len] = text[len];
			line[len + 1] = '\0';
			if ( ( raw ? ST_RawWidth( line ) : ST_TextWidth( line ) ) > w )
				break;
			if ( text[len] == ' ' || !text[len + 1] )
				fit = len + 1;
			len++;
		}
		if ( !text[len] )
			fit = len;
		if ( !fit )
			fit = Q_max( len, 1 );
		Q_strncpyz( line, text, fit + 1 );
		if ( raw )
			ST_RawText( x, y + lines * ST_ROW_H, line, color );
		else
			ST_Text( x, y + lines * ST_ROW_H, line, color );
		lines++;
		text += fit;
		while ( *text == ' ' )
			text++;
	}
	return lines;
}

static int ST_TextWrapped( float x, float y, float w, const char *text, const float *color, int maxLines ) {
	return ST_TextWrappedEx( x, y, w, text, color, maxLines, qfalse );
}

// eats this frame's click if it landed in the rect
static qboolean ST_Clicked( float x, float y, float w, float h ) {
	if ( !st.click || st.clickX < x || st.clickX >= x + w || st.clickY < y || st.clickY >= y + h )
		return qfalse;
	st.click = qfalse;
	return qtrue;
}

static qboolean ST_Button( float x, float y, float w, const char *label, qboolean selected, qboolean enabled ) {
	const float *fill = selected ? stHighlight : ( enabled && ST_InRect( x, y, w, ST_CTRL_H ) ) ? stHoverBox : stPanelLight;

	ST_Box( x, y, w, ST_CTRL_H, fill );
	ST_Text( x + ( w - ST_TextWidth( label ) ) * 0.5f, y + 2, label, enabled ? stWhite : stDim );
	return (qboolean)( ST_Clicked( x, y, w, ST_CTRL_H ) && enabled );
}

static void ST_TextBox( float x, float y, float w, const char *text, qboolean focused, const char *placeholder ) {
	char buf[ST_VALUE_LEN + 2];

	ST_Box( x, y, w, ST_CTRL_H, focused ? stPanelFocus : stPanelLight );
	if ( !text[0] && !focused ) {
		ST_TextClipped( x + 4, y + 2, w - 8, placeholder, stDim );
		return;
	}
	// keep the end in view
	Com_sprintf( buf, sizeof( buf ), "%s%s", text, focused && ( ( cls.realtime >> 8 ) & 1 ) ? "_" : "" );
	const char *shown = buf;
	while ( shown[1] && ST_RawWidth( shown ) > w - 8 )
		shown++;
	ST_RawText( x + 4, y + 2, shown, stWhite );
}

typedef void ( *stRowFunc_t )( int row, float x, float y, float w );

// returns the row double-clicked, -1 if none
static int ST_List( stList_t *list, int count, float x, float y, float w, float h, stRowFunc_t drawRow, const float *rowFill( int row ) ) {
	const int visible = (int)( h / ST_ROW_H );
	int row, picked = -1;

	if ( st.wheel && ST_InRect( x, y, w, h ) ) {
		list->scroll += st.wheel;
		st.wheel = 0;
	}
	// keep the selection in view when it moved by key
	if ( list->sel >= 0 && list->sel < list->scroll )
		list->scroll = list->sel;
	list->scroll = Com_Clampi( 0, Q_max( 0, count - visible ), list->scroll );

	if ( ST_Clicked( x, y, w, h ) ) {
		row = list->scroll + (int)( ( st.clickY - y ) / ST_ROW_H );
		if ( row < count ) {
			if ( row == list->lastClickRow && cls.realtime - list->lastClickTime < ST_DOUBLECLICK_MS ) {
				list->lastClickTime = 0;
				picked = row;
			} else {
				list->lastClickRow = row;
				list->lastClickTime = cls.realtime;
			}
			list->sel = row;
		}
	}

	ST_Box( x, y, w, h, stPanelLight );
	for ( int i = 0; i < visible; i++ ) {
		const float ry = y + i * ST_ROW_H;
		const float *fill;

		row = list->scroll + i;
		if ( row >= count )
			break;
		fill = rowFill ? rowFill( row ) : NULL;
		if ( row == list->sel )
			ST_Fill( x + 1, ry, w - 2, ST_ROW_H, stHighlight );
		else if ( fill )
			ST_Fill( x + 1, ry, w - 2, ST_ROW_H, fill );
		else if ( ST_InRect( x, ry, w, ST_ROW_H ) )
			ST_Fill( x + 1, ry, w - 2, ST_ROW_H, stHover );
		drawRow( row, x + 4, ry + 1, w - 8 );
	}
	return picked;
}

static void ST_DrawActionRow( int row, float x, float y, float w ) {
	const stActionRow_t *r = &st.actionRows[row];

	if ( r->header )
		ST_Text( x, y, va( S_COLOR_YELLOW "%s", r->header ), stWhite );
	else if ( ST_Experimental( r->action ) && !CL_RpHooks_Experimental() )
		ST_TextClipped( x + 8, y, w - 8, va( S_COLOR_GREY "%s", stActions[r->action].name ), stWhite );
	else
		ST_TextClipped( x + 8, y, w - 8, stActions[r->action].name, stWhite );
}

static void ST_DrawStepRow( int row, float x, float y, float w ) {
	const stStep_t *s = &st.steps[row];
	const float *color = !s->enabled ? stDim : ST_StepUsable( s ) ? stWhite : stAccent;
	char num[16];

	// not va(): building the step's command goes through it too
	Com_sprintf( num, sizeof( num ), "%i", row + 1 );
	const char *summary = ST_StepSummary( s );

	ST_Text( x + 18 - ST_TextWidth( num ), y, num, stDim );
	ST_TextClipped( x + 24, y, w - 24, s->enabled ? summary : va( "(off) %s", summary ), color );
}

static const float *ST_StepFill( int row ) {
	return ( st.run.running && row == st.run.pc ) ? stRunning : NULL;
}

static void ST_DrawActions( void ) {
	const float listY = ST_TOP_Y + 14;
	const stActionRow_t *r;
	int picked;

	ST_Text( ST_ACTIONS_X, ST_TOP_Y, "Actions", stDim );
	picked = ST_List( &st.actionList, st.numActionRows, ST_ACTIONS_X, listY, ST_ACTIONS_W, ST_ACTIONS_H, ST_DrawActionRow, NULL );
	// headers aren't picked
	if ( st.actionList.sel >= 0 && st.actionList.sel < st.numActionRows && st.actionRows[st.actionList.sel].header
		&& st.actionList.sel + 1 < st.numActionRows )
		st.actionList.sel++;
	if ( picked >= 0 && !st.actionRows[picked].header && ST_CanAdd( st.actionRows[picked].action ) )
		ST_AddStep( st.actionRows[picked].action );

	if ( st.actionList.sel < 0 || st.actionList.sel >= st.numActionRows )
		return;
	r = &st.actionRows[st.actionList.sel];
	if ( r->header )
		return;
	const qboolean off = (qboolean)( ST_Experimental( r->action ) && !CL_RpHooks_Experimental() );

	if ( ST_Button( ST_ACTIONS_X, listY + ST_ACTIONS_H + 4, ST_ACTIONS_W, va( "Add \"%s\"", stActions[r->action].name ), qfalse, (qboolean)!off ) )
		ST_AddStep( r->action );
	ST_TextWrapped( ST_ACTIONS_X, listY + ST_ACTIONS_H + 24, ST_ACTIONS_W,
		off ? va( S_COLOR_YELLOW "Experimental, off: " RP_EXPERIMENTAL_LABEL " turns it on. " S_COLOR_GREY "%s", stActions[r->action].desc )
		: stActions[r->action].desc, stDim, 7 );
}

static void ST_DrawSteps( void ) {
	const float listY = ST_TOP_Y + 14, by = listY + ST_STEPS_H + 4, bw = ( ST_STEPS_W - 12 ) / 4;
	stStep_t *s = ST_SelectedStep();
	int picked;

	ST_Text( ST_STEPS_X, ST_TOP_Y, va( "Story" S_COLOR_GREY "  %i step%s", st.numSteps, st.numSteps == 1 ? "" : "s" ), stWhite );
	if ( st.numSteps ) {
		const int before = st.stepList.sel;
		picked = ST_List( &st.stepList, st.numSteps, ST_STEPS_X, listY, ST_STEPS_W, ST_STEPS_H, ST_DrawStepRow, ST_StepFill );
		if ( st.stepList.sel != before )
			st.focus = ST_F_NONE;
		// double-click plays from there
		if ( picked >= 0 )
			ST_Run( picked );
	} else {
		ST_Box( ST_STEPS_X, listY, ST_STEPS_W, ST_STEPS_H, stPanelLight );
		ST_TextWrapped( ST_STEPS_X + 8, listY + 8, ST_STEPS_W - 16,
			"A blank slate. Pick an action on the left and click Add, or double-click it: steps go after the selected one, and play from top to bottom.", stDim, 6 );
	}

	if ( ST_Button( ST_STEPS_X, by, bw, "Up", qfalse, (qboolean)( s && st.stepList.sel > 0 ) ) )
		ST_MoveStep( -1 );
	if ( ST_Button( ST_STEPS_X + bw + 4, by, bw, "Down", qfalse, (qboolean)( s && st.stepList.sel < st.numSteps - 1 ) ) )
		ST_MoveStep( 1 );
	if ( ST_Button( ST_STEPS_X + ( bw + 4 ) * 2, by, bw, "Copy", qfalse, (qboolean)( s != NULL ) ) )
		ST_CopyStep();
	if ( ST_Button( ST_STEPS_X + ( bw + 4 ) * 3, by, bw, "Delete", qfalse, (qboolean)( s != NULL ) ) )
		ST_DeleteStep();
}

static void ST_DrawEdit( void ) {
	stStep_t *s = ST_SelectedStep();
	const stActionDef_t *def;
	const float fx = ST_EDIT_X + ST_EDIT_LABEL_W, fw = ST_EDIT_W - ST_EDIT_LABEL_W;
	float y = ST_TOP_Y + 14;
	const char *cmd, *error;
	qboolean local;

	if ( !s ) {
		ST_Text( ST_EDIT_X, ST_TOP_Y, "Step", stDim );
		return;
	}
	def = ST_Def( s );
	ST_Text( ST_EDIT_X, ST_TOP_Y, va( "Step %i" S_COLOR_GREY "  %s", st.stepList.sel + 1, def->name ), stWhite );

	for ( int i = 0; i < def->numFields; y += ST_FieldShown( s, i ) ? ST_CTRL_H + 3 : 0, i++ ) {
		const stFieldDef_t *f = &def->fields[i];

		if ( !ST_FieldShown( s, i ) )
			continue;

		ST_Text( ST_EDIT_X, y + 2, f->label, stDim );
		if ( f->kind == ST_K_CHOICE ) {
			// click for the next one, right side for the one before
			const int n = ST_NumChoices( f->choices ), at = ST_ChoiceIndex( f->choices, s->values[i] );
			if ( ST_Button( fx, y, fw - 20, s->values[i], qfalse, qtrue ) ) {
				Q_strncpyz( s->values[i], ST_Choice( f->choices, ( at + 1 ) % n ), ST_VALUE_LEN );
				ST_Changed();
			}
			if ( ST_Button( fx + fw - 18, y, 18, "<", qfalse, qtrue ) ) {
				Q_strncpyz( s->values[i], ST_Choice( f->choices, ( at + n - 1 ) % n ), ST_VALUE_LEN );
				ST_Changed();
			}
			continue;
		}
		ST_TextBox( fx, y, fw, s->values[i], (qboolean)( st.focus == i ), f->kind == ST_K_NUMBER ? "number" : "type here" );
		if ( ST_Clicked( fx, y, fw, ST_CTRL_H ) )
			st.focus = i;
	}

	if ( s->action == ST_A_TRIGGER && !ST_TriggerNear( s ) ) {
		const qboolean door = ST_TriggerDoor( s );
		const float bw = ( ST_EDIT_W - 4 ) * 0.5f;

		if ( ST_Button( ST_EDIT_X, y, bw, door ? "The one I aim at" : "The one I stand in", qfalse, qtrue ) ) {
			const int m = door ? ST_AimedMover() : ST_TriggerHere();

			if ( m ) {
				Com_sprintf( s->values[1], ST_VALUE_LEN, "*%i", m );
				ST_Changed();
			} else {
				ST_Message( door ? S_COLOR_YELLOW "Aim at a door or button first" : S_COLOR_YELLOW "Stand in one of the map's triggers first" );
			}
		}
		if ( ST_Button( ST_EDIT_X + bw + 4, y, bw, "Pick from the list", qfalse, qtrue ) )
			ST_OpenChoose( door ? ST_CHOOSE_DOOR : ST_CHOOSE_TRIGGER );
		y += ST_CTRL_H + 6;
	}
	if ( s->action == ST_A_TRIGGER && ST_TriggerEffect( s ) ) {
		if ( ST_Button( ST_EDIT_X, y, ST_EDIT_W, "Pick an effect in the map", qfalse, qtrue ) )
			ST_OpenChoose( ST_CHOOSE_EFFECT );
		y += ST_CTRL_H + 6;
	}

	if ( def->posField >= 0 && ST_FieldShown( s, def->posField ) ) {
		const float bw = ( ST_EDIT_W - 4 ) * 0.5f;
		if ( ST_Button( ST_EDIT_X, y, bw, "Where I stand", qfalse, qtrue ) ) {
			ST_FillHere( s );
			ST_Changed();
		}
		if ( ST_Button( ST_EDIT_X + bw + 4, y, bw, "Where I aim", qfalse, qtrue ) ) {
			if ( ST_FillAim( s ) )
				ST_Changed();
			else
				ST_Message( S_COLOR_YELLOW "Aim at a surface first" );
		}
		y += ST_CTRL_H + 6;
	}

	if ( s->action == ST_A_MODEL || s->action == ST_A_EFFECT || s->action == ST_A_SCREEN || s->action == ST_A_SHADER ) {
		const char *label = s->action == ST_A_MODEL ? "Place it with the model manager"
			: s->action == ST_A_SHADER ? "Pick it with the shader manager" : "Pick it in the effect manager";

		if ( ST_Button( ST_EDIT_X, y, ST_EDIT_W, label, qfalse, qtrue ) ) {
			ST_StartPick( s->action == ST_A_MODEL ? ST_PICK_MODEL : s->action == ST_A_SHADER ? ST_PICK_SHADER : ST_PICK_EFFECT );
			return;
		}
		y += ST_CTRL_H + 6;
	}

	if ( s->action == ST_A_SOUND || s->action == ST_A_MUSIC ) {
		if ( ST_Button( ST_EDIT_X, y, ST_EDIT_W, s->action == ST_A_SOUND ? "Pick it in the sound manager" : "Pick it in the music manager", qfalse, qtrue ) ) {
			ST_StartPick( s->action == ST_A_SOUND ? ST_PICK_SOUND : ST_PICK_MUSIC );
			return;
		}
		y += ST_CTRL_H + 6;
	}

	if ( s->action == ST_A_NPC_SPAWN || s->action == ST_A_NPC_TELE || s->action == ST_A_NPC_CMD ) {
		if ( ST_Button( ST_EDIT_X, y, ST_EDIT_W, s->action == ST_A_NPC_SPAWN ? "Place it with the NPC manager" : "Pick it in the NPC manager", qfalse, qtrue ) ) {
			ST_StartPick( s->action == ST_A_NPC_SPAWN ? ST_PICK_NPC_SPAWN : ST_PICK_NPC_MANAGE );
			return;
		}
		y += ST_CTRL_H + 6;
	}

	if ( s->action == ST_A_CAM_SHOT ) {
		if ( ST_Button( ST_EDIT_X, y, ST_EDIT_W, "Edit shot in the camera editor", qfalse, qtrue ) ) {
			// a new one gets a name of its own
			if ( !s->values[0][0] ) {
				Com_sprintf( s->values[0], ST_VALUE_LEN, "shot%i", st.stepList.sel + 1 );
				ST_Changed();
			}
			CL_RpCinematic_Edit( s->values[0], qtrue );
			return;
		}
		y += ST_CTRL_H + 6;
	}

	if ( ST_Button( ST_EDIT_X, y, ST_EDIT_W, s->enabled ? "Runs  (click to skip it)" : "Skipped  (click to run it)", (qboolean)!s->enabled, qtrue ) ) {
		s->enabled = (qboolean)!s->enabled;
		ST_Changed();
	}
	y += ST_CTRL_H + 8;

	// what it will do
	cmd = ST_StepCommand( s, &local, &error );
	if ( error ) {
		ST_TextWrapped( ST_EDIT_X, y, ST_EDIT_W, va( S_COLOR_YELLOW "%s", error ), stWhite, 3 );
	} else if ( cmd ) {
		int lines;
		ST_Text( ST_EDIT_X, y, local ? "Runs:" : "Sends:", stDim );
		lines = ST_TextWrappedEx( ST_EDIT_X, y + ST_ROW_H, ST_EDIT_W, cmd, stWhite, 5, qtrue );
		cmd = ST_StepExtra( s );
		if ( cmd ) {
			y += ST_ROW_H * ( lines + 1 ) + 4;
			ST_Text( ST_EDIT_X, y, "Then:", stDim );
			ST_TextWrappedEx( ST_EDIT_X, y + ST_ROW_H, ST_EDIT_W, cmd, stWhite, 2, qtrue );
		}
	} else if ( s->action == ST_A_TRIGGER ) {
		const int lines = ST_TextWrapped( ST_EDIT_X, y, ST_EDIT_W, va( "%s, then goes on.", ST_TriggerSummary( s ) ), stDim, 3 );
		ST_TextWrapped( ST_EDIT_X, y + ( lines + 1 ) * ST_ROW_H, ST_EDIT_W, ST_TriggerDoor( s )
			? "Whoever opens it: your game can't see who used it."
			: "Someone already in has to leave and come back. Only who your game can see counts.", stDim, 3 );
	} else if ( s->action == ST_A_WAIT ) {
		ST_TextWrapped( ST_EDIT_X, y, ST_EDIT_W, va( "Waits %.1f seconds, then goes on.", ST_Number( s, 0 ) ), stDim, 2 );
	} else if ( s->action == ST_A_PAUSE ) {
		ST_TextWrapped( ST_EDIT_X, y, ST_EDIT_W, "Waits until you press Continue, or run /storyteller continue.", stDim, 3 );
	}
}

static void ST_DrawBar( void ) {
	const float y = ST_BUTTON_Y + 22;
	stStep_t *s = ST_SelectedStep();

	if ( ST_Button( ST_ACTIONS_X, y, 48, st.confirmNew && cls.realtime - st.confirmTime < ST_CONFIRM_MS ? "Sure?" : "New", qfalse, (qboolean)( st.numSteps > 0 ) ) ) {
		if ( !st.dirty || ( st.confirmNew && cls.realtime - st.confirmTime < ST_CONFIRM_MS ) ) {
			ST_New();
			st.confirmNew = qfalse;
			ST_Message( "New story" );
		} else {
			st.confirmNew = qtrue;
			st.confirmTime = cls.realtime;
			ST_Message( S_COLOR_YELLOW "Not saved: click again to start over anyway" );
		}
	}
	if ( ST_Button( ST_ACTIONS_X + 52, y, 48, "Save", qfalse, (qboolean)( st.numSteps > 0 ) ) )
		ST_OpenSave();
	if ( ST_Button( ST_ACTIONS_X + 104, y, 46, "Load", qfalse, qtrue ) )
		ST_OpenLoad();

	if ( !st.run.running ) {
		if ( ST_Button( ST_STEPS_X, y, 106, "Play", qfalse, (qboolean)( st.numSteps > 0 ) ) )
			ST_Run( 0 );
		if ( ST_Button( ST_STEPS_X + 112, y, 106, "Play from here", qfalse, (qboolean)( s != NULL ) ) )
			ST_Run( st.stepList.sel );
	} else {
		if ( ST_Button( ST_STEPS_X, y, 106, "Stop", qfalse, qtrue ) )
			ST_Stop( S_COLOR_YELLOW "Stopped" );
		if ( ST_Button( ST_STEPS_X + 112, y, 106, "Continue", (qboolean)st.run.waiting, (qboolean)st.run.waiting ) )
			ST_Continue();
	}
}

static void ST_DrawStoryRow( int row, float x, float y, float w ) {
	ST_TextClipped( x, y, w, st.stories[row], stWhite );
}

static void ST_OpenArmed( void ) {
	st.popup = ST_POPUP_ARMED;
	st.focus = ST_F_NONE;
	st.armedList.sel = stNumArmed ? 0 : -1;
	st.armedList.scroll = 0;
	st.armedList.lastClickRow = -1;
	if ( st.armTimes < 2 )
		st.armTimes = 2;
}

static void ST_OpenChoose( stChoose_t kind ) {
	ST_ScanMap();
	st.chooseKind = kind;
	st.chooseList.sel = -1;
	st.chooseList.scroll = 0;
	st.chooseList.lastClickRow = -1;
	if ( kind == ST_CHOOSE_EFFECT ) {
		CL_EffectManager_RequestList();
		st.nextSend = Q_max( st.nextSend, cls.realtime + ST_CommandGap() );
	}
	st.popup = ST_POPUP_CHOOSE;
	st.focus = ST_F_NONE;
}

static int stChooseRows[ST_MAX_MAPENTS];

// what the choose popup lists now: indexes into the map's entities, or into RPMod's effect list
static int ST_ChooseRows( void ) {
	int count = 0;

	if ( st.chooseKind == ST_CHOOSE_EFFECT ) {
		for ( int i = 0; i < CL_EffectManager_NumActive() && count < ST_MAX_MAPENTS; i++ )
			stChooseRows[count++] = i;
		return count;
	}
	for ( int i = 0; i < stMap.count; i++ ) {
		if ( stMap.ents[i].trigger == (qboolean)( st.chooseKind == ST_CHOOSE_TRIGGER ) )
			stChooseRows[count++] = i;
	}
	return count;
}

static void ST_DrawChooseRow( int row, float x, float y, float w ) {
	if ( st.chooseKind == ST_CHOOSE_EFFECT ) {
		const char *path;
		vec3_t org;
		int num;

		if ( CL_EffectManager_GetActive( stChooseRows[row], &num, &path, org ) )
			ST_TextClipped( x, y, w, va( "%i  %s" S_COLOR_GREY "  %i %i %i", num, path, ST_Round( org[0] ), ST_Round( org[1] ), ST_Round( org[2] ) ), stWhite );
		return;
	}
	const stMapEnt_t *e = &stMap.ents[stChooseRows[row]];
	vec3_t mid;

	VectorAdd( e->mins, e->maxs, mid );
	VectorScale( mid, 0.5f, mid );
	ST_TextClipped( x, y, w, va( "*%i  %s%s%s" S_COLOR_GREY "  at %i %i %i", e->model, e->classname, e->targetname[0] ? " " : "", e->targetname,
		ST_Round( mid[0] ), ST_Round( mid[1] ), ST_Round( mid[2] ) ), stWhite );
}

// the selected step takes the row picked
static void ST_ChooseApply( int row ) {
	stStep_t *s = ST_SelectedStep();

	if ( !s || s->action != ST_A_TRIGGER )
		return;
	if ( st.chooseKind == ST_CHOOSE_EFFECT ) {
		const char *path;
		vec3_t org;
		int num;

		if ( !CL_EffectManager_GetActive( stChooseRows[row], &num, &path, org ) )
			return;
		Q_strncpyz( s->values[1], path, ST_VALUE_LEN );
		for ( int i = 0; i < 3; i++ )
			Com_sprintf( s->values[5 + i], ST_VALUE_LEN, "%i", ST_Round( org[i] ) );
	} else {
		Com_sprintf( s->values[1], ST_VALUE_LEN, "*%i", stMap.ents[stChooseRows[row]].model );
	}
	ST_Changed();
}

static void ST_DrawChoosePopup( void ) {
	static const char *titles[] = { "Pick a trigger", "Pick a door or button", "Pick an effect" };
	const float x = ST_POPUP_X, y = 80, w = ST_POPUP_W, h = 320;
	const int count = ST_ChooseRows();
	int picked = -1;

	ST_Box( x, y, w, h, stPanelOpaque );
	ST_Text( x + 8, y + 8, va( "%s" S_COLOR_GREY "  %i", titles[st.chooseKind], count ), stWhite );
	if ( !count ) {
		ST_TextWrapped( x + 8, y + 30, w - 16, st.chooseKind == ST_CHOOSE_EFFECT
			? "Asking the server for the map's effects... If none come, put some there with the effect manager."
			: st.chooseKind == ST_CHOOSE_TRIGGER ? "This map has no triggers." : "This map has no doors or buttons.", stDim, 3 );
	} else {
		picked = ST_List( &st.chooseList, count, x + 8, y + 26, w - 16, h - 60, ST_DrawChooseRow, NULL );
	}
	if ( ST_Button( x + 8, y + h - 22, 80, "Pick", qfalse, (qboolean)( st.chooseList.sel >= 0 && st.chooseList.sel < count ) ) )
		picked = st.chooseList.sel;
	if ( ST_Button( x + w - 88, y + h - 22, 80, "Cancel", qfalse, qtrue ) )
		st.popup = ST_POPUP_NONE;
	if ( picked >= 0 && picked < count ) {
		ST_ChooseApply( picked );
		st.popup = ST_POPUP_NONE;
	}
}

static void ST_DrawArmedRow( int row, float x, float y, float w ) {
	const stArmed_t *a = &stArmed[row];

	ST_TextClipped( x, y, w, va( "%s" S_COLOR_GREY "  %s, %s", a->name, ST_ArmedState( a ), ST_AtTrigger( &a->run, a->steps, a->numSteps ) ? "waiting" : S_COLOR_GREEN "playing" ), stWhite );
}

static void ST_DrawArmedPopup( void ) {
	const float x = ST_POPUP_X, y = 80, w = ST_POPUP_W, h = 320;
	const char *why = ST_ArmProblem( st.steps, st.numSteps );
	const char *name = st.name[0] ? st.name : "unsaved story";
	const qboolean haveSel = (qboolean)( st.armedList.sel >= 0 && st.armedList.sel < stNumArmed );

	ST_Box( x, y, w, h, stPanelOpaque );
	ST_Text( x + 8, y + 8, va( "Armed stories" S_COLOR_GREY "  %i", stNumArmed ), stWhite );
	ST_TextWrapped( x + 8, y + 26, w - 16, va( "Arm %s: a copy of it plays by itself when its first step is set off.", name ), stDim, 2 );
	if ( why ) {
		ST_TextWrapped( x + 8, y + 54, w - 16, va( S_COLOR_YELLOW "Can't: %s", why ), stWhite, 2 );
	} else {
		const float bw = ( w - 20 ) * 0.5f;

		if ( ST_Button( x + 8, y + 54, bw, "Arm: once", qfalse, qtrue ) )
			ST_Arm( name, st.steps, st.numSteps, 1 );
		if ( ST_Button( x + 12 + bw, y + 54, bw, "Arm: every time", qfalse, qtrue ) )
			ST_Arm( name, st.steps, st.numSteps, 0 );
		// a number of times, set with - and +
		if ( ST_Button( x + 8, y + 76, 22, "-", qfalse, (qboolean)( st.armTimes > 2 ) ) )
			st.armTimes--;
		ST_Text( x + 8 + ( bw - ST_TextWidth( va( "%i", st.armTimes ) ) ) * 0.5f, y + 78, va( "%i", st.armTimes ), stWhite );
		if ( ST_Button( x + bw - 14, y + 76, 22, "+", qfalse, (qboolean)( st.armTimes < ST_MAX_ARM_TIMES ) ) )
			st.armTimes++;
		if ( ST_Button( x + 12 + bw, y + 76, bw, va( "Arm: %i times", st.armTimes ), qfalse, qtrue ) )
			ST_Arm( name, st.steps, st.numSteps, st.armTimes );
	}

	if ( !stNumArmed )
		ST_Text( x + 8, y + 106, S_COLOR_GREY "Nothing armed yet", stWhite );
	else
		ST_List( &st.armedList, stNumArmed, x + 8, y + 104, w - 16, h - 162, ST_DrawArmedRow, NULL );
	ST_TextWrapped( x + 8, y + h - 52, w - 16, S_COLOR_GREY "A copy: arm it again after changing it. Disarmed when the map changes.", stWhite, 2 );

	if ( ST_Button( x + 8, y + h - 22, 80, "Disarm", qfalse, haveSel ) ) {
		ST_Message( va( "Disarmed %s", stArmed[st.armedList.sel].name ) );
		ST_Disarm( st.armedList.sel );
		if ( st.armedList.sel >= stNumArmed )
			st.armedList.sel = stNumArmed - 1;
	}
	if ( ST_Button( x + 92, y + h - 22, 80, "Disarm all", qfalse, (qboolean)( stNumArmed > 0 ) ) ) {
		while ( stNumArmed )
			ST_Disarm( 0 );
		st.armedList.sel = -1;
		ST_Message( "Nothing is armed now" );
	}
	if ( ST_Button( x + w - 88, y + h - 22, 80, "Close", qfalse, qtrue ) )
		st.popup = ST_POPUP_NONE;
}

static void ST_DrawExperimentalPopup( void ) {
	const float x = ST_POPUP_X, y = 160, w = ST_POPUP_W, h = 120;

	ST_Box( x, y, w, h, stPanelOpaque );
	ST_Text( x + 8, y + 8, S_COLOR_YELLOW "Experimental features", stWhite );
	ST_TextWrapped( x + 8, y + 26, w - 16, RP_EXPERIMENTAL_WARNING, stWhite, 5 );
	if ( ST_Button( x + 8, y + h - 22, 80, "OK", qfalse, qtrue ) ) {
		CL_RpHooks_SetExperimental( qtrue );
		ST_Message( S_COLOR_YELLOW "Experimental features on, for this session" );
		st.popup = ST_POPUP_NONE;
	}
	if ( ST_Button( x + w - 88, y + h - 22, 80, "Cancel", qfalse, qtrue ) )
		st.popup = ST_POPUP_NONE;
}

static void ST_DrawPopup( void ) {
	const float x = ST_POPUP_X, w = ST_POPUP_W;

	if ( st.popup == ST_POPUP_EXPERIMENTAL ) {
		ST_DrawExperimentalPopup();
		return;
	}

	if ( st.popup == ST_POPUP_ARMED ) {
		ST_DrawArmedPopup();
		return;
	}
	if ( st.popup == ST_POPUP_CHOOSE ) {
		ST_DrawChoosePopup();
		return;
	}

	if ( st.popup == ST_POPUP_SAVE ) {
		const float y = 180, h = 90;
		ST_Box( x, y, w, h, stPanelOpaque );
		ST_Text( x + 8, y + 8, "Save the story as", stWhite );
		ST_TextBox( x + 8, y + 26, w - 16, st.saveName, (qboolean)( st.focus == ST_F_SAVE_NAME ), "name" );
		if ( ST_Clicked( x + 8, y + 26, w - 16, ST_CTRL_H ) )
			st.focus = ST_F_SAVE_NAME;
		ST_Text( x + 8, y + 46, va( S_COLOR_GREY "In %s/, letters, digits, _ and -", ST_SAVE_DIR ), stWhite );
		if ( ST_Button( x + 8, y + h - 22, 80, "Save", qfalse, (qboolean)( st.saveName[0] != '\0' ) ) )
			ST_DoSave();
		if ( ST_Button( x + w - 88, y + h - 22, 80, "Cancel", qfalse, qtrue ) ) {
			st.popup = ST_POPUP_NONE;
			st.focus = ST_F_NONE;
		}
		return;
	}

	const float y = 80, h = 320;
	int picked;
	ST_Box( x, y, w, h, stPanelOpaque );
	ST_Text( x + 8, y + 8, va( "Load a story" S_COLOR_GREY "  %i saved", st.numStories ), stWhite );
	if ( !st.numStories ) {
		ST_TextWrapped( x + 8, y + 30, w - 16, va( "No stories saved yet: they go in %s/ in your game folder.", ST_SAVE_DIR ), stDim, 3 );
		picked = -1;
	} else {
		picked = ST_List( &st.storyList, st.numStories, x + 8, y + 26, w - 16, h - 60, ST_DrawStoryRow, NULL );
	}
	if ( st.dirty )
		ST_Text( x + 8, y + h - 44, S_COLOR_YELLOW "The story open now isn't saved", stWhite );
	if ( ST_Button( x + 8, y + h - 22, 80, "Load", qfalse, (qboolean)( st.storyList.sel >= 0 && st.storyList.sel < st.numStories ) ) )
		picked = st.storyList.sel;
	if ( ST_Button( x + w - 88, y + h - 22, 80, "Cancel", qfalse, qtrue ) )
		st.popup = ST_POPUP_NONE;
	if ( picked >= 0 && picked < st.numStories && ST_Load( st.stories[picked] ) )
		st.popup = ST_POPUP_NONE;
}

static const char *ST_RunStatus( const stRun_t *r, const stStep_t *steps, int numSteps ) {
	const stStep_t *s;

	if ( !r->running )
		return NULL;
	if ( cls.realtime < r->shotUntil )
		return va( S_COLOR_GREEN "Step %i/%i: the shot plays, %.1f s left", r->pc, numSteps, ( r->shotUntil - cls.realtime ) / 1000.0f );
	if ( r->pc >= numSteps )
		return S_COLOR_GREEN "Finishing";
	s = &steps[r->pc];
	if ( r->waiting )
		return va( S_COLOR_YELLOW "Waiting for you at step %i%s%s", r->pc + 1, s->values[0][0] ? ": " : "", s->values[0] );
	if ( s->action == ST_A_WAIT && r->stepStart ) {
		const float left = ST_Number( s, 0 ) - ( cls.realtime - r->stepStart ) / 1000.0f;
		return va( S_COLOR_GREEN "Step %i/%i: waiting %.1f s", r->pc + 1, numSteps, left > 0 ? left : 0.0f );
	}
	return va( S_COLOR_GREEN "Step %i/%i: %s", r->pc + 1, numSteps, ST_StepSummary( s ) );
}

static const char *ST_Status( void ) {
	return ST_RunStatus( &st.run, st.steps, st.numSteps );
}

// while playing with the panel closed: where the story is, and how to go on
// a story's line at the top; how tall it was
static float ST_DrawRunLine( float y, const char *label, const char *status, qboolean waiting ) {
	ST_Fill( 160, y, 320, waiting ? 30 : 17, stPanel );
	ST_TextClipped( 166, y + 2, 308, va( "%s: %s", label, status ), stWhite );
	if ( waiting ) {
		const int key = Key_GetKey( ST_TOGGLE_CMD " continue" );
		ST_TextClipped( 166, y + 15, 308, key >= 0 ? va( S_COLOR_GREY "Press %s to go on", Key_KeynumToString( key ) )
			: S_COLOR_GREY "/storyteller continue goes on (bind a key to it)", stWhite );
	}
	return waiting ? 32.0f : 19.0f;
}

// while playing with the panel closed: where the stories are, and how to go on. Armed ones
// show once set off
static void ST_DrawPlaying( void ) {
	const char *status = ST_Status();
	float y = 4;

	if ( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) )
		return;
	if ( status )
		y += ST_DrawRunLine( y, "Storyteller", status, st.run.waiting );
	for ( int i = 0; i < stNumArmed; i++ ) {
		const stArmed_t *a = &stArmed[i];

		if ( !a->run.running || ST_AtTrigger( &a->run, a->steps, a->numSteps ) )
			continue;
		status = ST_RunStatus( &a->run, a->steps, a->numSteps );
		if ( status )
			y += ST_DrawRunLine( y, a->name, status, a->run.waiting );
	}
}

void CL_Storyteller_Draw( void ) {
	char buf[MAX_STRING_CHARS];
	const char *status;

	if ( !st.open ) {
		if ( !st.font ) {
			st.font = re->RegisterFont( "arialnb" );
			if ( !st.font )
				st.font = cls.menuFont;
		}
		ST_DrawPlaying();
		return;
	}

	// the popup sits on top and gets the clicks first, but is drawn last
	const qboolean popupOpen = (qboolean)( st.popup != ST_POPUP_NONE );
	const qboolean click = st.click;
	const int wheel = st.wheel;
	if ( popupOpen )
		st.click = qfalse, st.wheel = 0;

	ST_Fill( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, stPanel );
	ST_Text( ST_ACTIONS_X, 14, "Storyteller", stWhite );
	ST_TextClipped( ST_ACTIONS_X + 80, 14, 200, st.name[0] ? va( "%s%s", st.name, st.dirty ? S_COLOR_YELLOW " *" : "" )
		: va( S_COLOR_GREY "unsaved story%s", st.dirty ? S_COLOR_YELLOW " *" : "" ), stWhite );
	status = ST_Status();
	if ( status )
		ST_TextClipped( ST_EDIT_X - 100, 14, ST_EDIT_W + 100 - 76 - 124, status, stWhite );
	if ( ST_Button( ST_EDIT_X + ST_EDIT_W - 74 - 120, 12, 120, RP_EXPERIMENTAL_LABEL, CL_RpHooks_Experimental(), qtrue ) ) {
		if ( CL_RpHooks_Experimental() ) {
			CL_RpHooks_SetExperimental( qfalse );
			ST_Message( "Experimental features off" );
		} else {
			st.popup = ST_POPUP_EXPERIMENTAL;
			st.focus = ST_F_NONE;
		}
	}
	if ( ST_Button( ST_EDIT_X + ST_EDIT_W - 70, 12, 70, va( "Armed (%i)", stNumArmed ), (qboolean)( stNumArmed > 0 ), qtrue ) )
		ST_OpenArmed();
	if ( st.message[0] && cls.realtime - st.messageTime < ST_MESSAGE_MS )
		ST_TextClipped( ST_STEPS_X, 29, ST_STEPS_W + ST_EDIT_W, st.message, stWhite );

	ST_DrawActions();
	ST_DrawSteps();
	ST_DrawEdit();
	ST_DrawBar();

	ST_Text( ST_EDIT_X, 428, S_COLOR_GREY "Up/Down select   Ctrl+Up/Down move", stWhite );
	ST_Text( ST_EDIT_X, 440, S_COLOR_GREY "Del delete   Tab next box", stWhite );
	Com_sprintf( buf, sizeof( buf ), S_COLOR_GREY "Esc back / close%s", Key_GetKey( ST_TOGGLE_CMD ) >= 0 ? "" : "   Bind a key to storyteller" );
	ST_Text( ST_EDIT_X, 452, buf, stWhite );

	if ( popupOpen && st.popup != ST_POPUP_NONE ) {
		st.click = click;
		st.wheel = wheel;
		ST_DrawPopup();
	}

	// a click on nothing leaves the text box
	if ( st.click && st.focus != ST_F_SAVE_NAME )
		st.focus = ST_F_NONE;
	st.click = qfalse;
	st.wheel = 0;

	re->DrawStretchPic( st.cursorX, st.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}
