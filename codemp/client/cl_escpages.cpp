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

// cl_escpages.cpp -- Setup, Renderer and Controls drawn inside the Modern and KOTOR Esc menus
//
// The game's own pages can't be opened from them: RPMod's UI is closed, and reached through the
// in-game menu (cl_rendermenu.cpp's router) it closed the page on the next click. So the three pages
// are drawn here, as rows the same as theirs: the settings of RPMod's Setup page (ui/jamp/
// ingame_setup.menu), the RENDERER page this client adds to it, and the key bindings and mouse
// settings of JKA's Controls page (ui/jamp/ingame_controls.menu). Labels are the game's own strings
// where it has them. The Classic menu keeps the game's own pages.

#include <string>
#include <vector>
#include "client.h"
#include "cl_escpages.h"
#include "qcommon/stringed_ingame.h"

typedef enum { PR_CHOICE, PR_SLIDER, PR_BIND, PR_FIELD } prType_t;

enum {
	PF_VIDEO	= 1,		// takes effect at a vid_restart: Apply shows
	PF_SOUND	= 2,		// the sound restarts
	PF_LANG		= 4,		// Set language shows
	PF_SWAY		= 8,		// the view's sway: its cfg run
	PF_PITCH	= 16,		// m_pitch's sign
	PF_CUSTOM	= 32,		// only with the custom video mode (r_mode -1)
	PF_VULKAN	= 64,		// the renderer page's: with Vulkan picked
	PF_POSTFX	= 128		// and Post Effects on
};

typedef struct {
	prType_t	type;
	const char	*str;		// the game's string for the label, NULL none
	const char	*label;		// English, when the game hasn't the string
	const char	*cvar;		// the setting, or the command a bind runs
	const char	*list;		// choices: "label|value|label|value"; a label @REF=English is the game's string
	float		min, max;	// a slider's
	int			flags;
} prDef_t;

#define ONOFF	"@MENUS_OFF=Off|0|@MENUS_ON=On|1"
#define NOYES	"@MENUS_NO=No|0|@MENUS_YES=Yes|1"

#define BIND( str, label, cmd )		{ PR_BIND, str, label, cmd, NULL, 0, 0, 0 }
#define ONOFF_ROW( str, label, cvar )	{ PR_CHOICE, str, label, cvar, ONOFF, 0, 0, 0 }
#define TERM						{ PR_CHOICE, NULL, NULL, NULL, NULL, 0, 0, 0 }

static const prDef_t prVideo[] = {
	{ PR_CHOICE, "MENUS_VIDEO_MODE", "Video Mode", "r_mode", "640 x 480|3|800 x 600|4|1024 x 768|6|1152 x 864|7|1280 x 1024|8|"
		"1600 x 1200|9|2048 x 1536|10|2400 x 600|12|Desktop|-2|@MENUS_CUSTOM=Custom|-1", 0, 0, PF_VIDEO },
	{ PR_FIELD, "RPMOD_CUSTOM_WIDTH", "Custom width", "r_customWidth", NULL, 0, 0, PF_VIDEO | PF_CUSTOM },
	{ PR_FIELD, "RPMOD_CUSTOM_HEIGHT", "Custom height", "r_customHeight", NULL, 0, 0, PF_VIDEO | PF_CUSTOM },
	{ PR_CHOICE, "MENUS_COLOR_DEPTH", "Color Depth", "r_colorbits", "@MENUS_DEFAULT=Default|0|@MENUS_16_BIT=16 bit|16|@MENUS_32_BIT=32 bit|32", 0, 0, PF_VIDEO },
	{ PR_CHOICE, "MENUS_FULL_SCREEN", "Full Screen", "r_fullscreen", ONOFF, 0, 0, PF_VIDEO },
	{ PR_CHOICE, "MENUS_GEOMETRIC_DETAIL", "Geometric Detail", "r_lodbias", "@MENUS_LOW=Low|2|@MENUS_MEDIUM=Medium|1|@MENUS_HIGH=High|0", 0, 0, PF_VIDEO },
	{ PR_CHOICE, "MENUS_TEXTURE_DETAIL", "Texture Detail", "r_picmip", "@MENUS_LOW=Low|3|@MENUS_MEDIUM=Medium|2|@MENUS_HIGH=High|1|@MENUS_VERY_HIGH=Very High|0", 0, 0, PF_VIDEO },
	{ PR_CHOICE, "MENUS_TEXTURE_QUALITY", "Texture Quality", "r_texturebits", "@MENUS_DEFAULT=Default|0|@MENUS_16_BIT=16 bit|16|@MENUS_32_BIT=32 bit|32", 0, 0, PF_VIDEO },
	{ PR_CHOICE, "MENUS_TEXTURE_FILTER", "Texture Filter", "r_textureMode", "@MENUS_BILINEAR=Bilinear|GL_LINEAR_MIPMAP_NEAREST|@MENUS_TRILINEAR=Trilinear|GL_LINEAR_MIPMAP_LINEAR", 0, 0, PF_VIDEO },
	{ PR_CHOICE, "MENUS_DETAILED_SHADERS", "Detailed Shaders", "r_detailtextures", ONOFF, 0, 0, PF_VIDEO },
	{ PR_CHOICE, "MENUS_VIDEO_SYNC", "Video Sync", "r_swapInterval", ONOFF, 0, 0, PF_VIDEO },
	TERM
};

static const prDef_t prMoreVideo[] = {
	{ PR_SLIDER, "MENUS_VIDEO_BRIGHTNESS", "Brightness", "r_gamma", NULL, 0.5f, 3.0f, 0 },
	ONOFF_ROW( "MENUS_DYNAMIC_LIGHTS", "Dynamic Lights", "r_dynamiclight" ),
	ONOFF_ROW( "MENUS_WALL_MARKS", "Wall Marks", "cg_marks" ),
	{ PR_SLIDER, "MENUS_ANISOTROPIC_FILTERING", "Anisotropic Filtering", "r_ext_texture_filter_anisotropic", NULL, 1.0f, 16.0f, 0 },
	TERM
};

static const prDef_t prSound[] = {
	{ PR_SLIDER, "MENUS_EFFECTS_VOLUME", "Effects Volume", "s_volume", NULL, 0, 1, 0 },
	{ PR_SLIDER, "MENUS_MUSIC_VOLUME", "Music Volume", "s_musicvolume", NULL, 0, 1, 0 },
	{ PR_SLIDER, "MENUS_VOICE_VOLUME", "Voice Volume", "s_volumeVoice", NULL, 0, 1, 0 },
	{ PR_CHOICE, "MENUS_SOUND_QUALITY", "Sound Quality", "s_khz", "@MENUS_LOW=Low|11|@MENUS_HIGH=High|22", 0, 0, PF_SOUND },
	{ PR_CHOICE, "MENUS_EAX", "EAX", "s_UseOpenAL", ONOFF, 0, 0, PF_SOUND },
	TERM
};

static const prDef_t prOptions[] = {
	ONOFF_ROW( "MENUS_DRAW_CROSSHAIR", "Draw Crosshair", "cg_drawcrosshair" ),
	ONOFF_ROW( "MENUS_IDENTIFY_TARGET", "Identify Target", "cg_drawCrosshairNames" ),
	ONOFF_ROW( "MENUS_FORCE_PLAYER_MODELS", "Force Player Models", "cg_forceModel" ),
	ONOFF_ROW( "MENUS_DEFER_PLAYER_MODELS", "Defer Player Models", "cg_deferPlayers" ),
	{ PR_CHOICE, "MENUS_TEAM_CHATS_ONLY", "Team Chats Only", "cg_teamChatsOnly", NOYES, 0, 0, 0 },
	ONOFF_ROW( "MENUS_SIMPLE_ITEMS", "Simple Items", "cg_simpleItems" ),
	ONOFF_ROW( "MENUS_TEAM_OVERLAY", "Team Overlay", "cg_drawTeamOverlay" ),
	{ PR_CHOICE, "MENUS_VIEW_SWAYING", "View Swaying", "ui_disableWeaponSway", "@MENUS_ON=On|0|@MENUS_OFF=Off|1", 0, 0, PF_SWAY },
	{ PR_CHOICE, "MENUS_FOOTSTEPS", "Footsteps", "cg_footsteps", "@MENUS_OFF=Off|0|@MENUS_SOUNDS=Sounds|1|"
		"@MENUS_SOUNDS_AND_EFFECTS=Sounds and Effects|2|@MENUS_SOUNDS_EFFECTS_GRAPHICS=Sounds, Effects, Graphics|3", 0, 0, 0 },
	{ PR_CHOICE, "MENUS_TEXT", "Text", "se_language", NULL, 0, 0, PF_LANG },		// the game's languages
	{ PR_CHOICE, "MENUS_VOICE", "Voice", "s_language", "English|english|@MENUS_LANG_FRENCH=French|francais|Deutsch|deutsch|Espanol|espanol", 0, 0, PF_LANG },
	TERM
};

static const prDef_t prMove[] = {
	BIND( "MENUS_WALK_FORWARD", "Walk Forward", "+forward" ),
	BIND( "MENUS_BACKPEDAL", "Backpedal", "+back" ),
	BIND( "MENUS_TURN_LEFT", "Turn Left", "+left" ),
	BIND( "MENUS_TURN_RIGHT", "Turn Right", "+right" ),
	BIND( "MENUS_RUN_WALK", "Run/Walk", "+speed" ),
	BIND( "MENUS_STEP_LEFT", "Step Left", "+moveleft" ),
	BIND( "MENUS_STEP_RIGHT", "Step Right", "+moveright" ),
	BIND( "MENUS_SIDESTEP", "Sidestep", "+strafe" ),
	BIND( "MENUS_UP_JUMP", "Up/Jump", "+moveup" ),
	BIND( "MENUS_DOWN_CROUCH", "Down/Crouch", "+movedown" ),
	TERM
};

static const prDef_t prInteract[] = {
	BIND( "MENUS_ATTACK", "Attack", "+attack" ),
	BIND( "MENUS_ALT_ATTACK", "Alt Attack", "+altattack" ),
	BIND( "MENUS_LIGHTSABER_STYLE", "Lightsaber Style", "saberAttackCycle" ),
	BIND( "MENUS_USE", "Use", "+use" ),
	BIND( "MENUS_USE_HELD_ITEM", "Use Held Item", "+button2" ),
	BIND( "MENUS_INV_NEXT", "Next Inventory", "invnext" ),
	BIND( "MENUS_INV_PREV", "Previous Inventory", "invprev" ),
	BIND( "MENUS_LOOK_UP", "Look Up", "+lookup" ),
	BIND( "MENUS_LOOK_DOWN", "Look Down", "+lookdown" ),
	BIND( "MENUS_MOUSE_LOOK", "Mouse Look", "+mlook" ),
	BIND( "MENUS_CENTER_VIEW", "Center View", "centerview" ),
	TERM
};

static const prDef_t prWeapons[] = {
	BIND( "MENUS_MELEE_LIGHTSABER", "Melee/Lightsaber", "weapon 1" ),
	BIND( "MENUS_PISTOL", "Pistol", "weapon 2" ),
	BIND( "MENUS_RIFLE", "Rifle", "weapon 3" ),
	BIND( "MENUS_DISRUPTOR_RIFLE", "Disruptor Rifle", "weapon 4" ),
	BIND( "MENUS_BOWCASTER", "Bowcaster", "weapon 5" ),
	BIND( "MENUS_HEAVY_REPEATER", "Heavy Repeater", "weapon 6" ),
	BIND( "MENUS_DEMP_2", "DEMP 2", "weapon 7" ),
	BIND( "MENUS_FLECHETTE", "Flechette", "weapon 8" ),
	BIND( "MENUS_CONC_RIFLE_SETUP", "Concussion Rifle", "weapon 13" ),
	BIND( "MENUS_MERR_SONN", "Merr-Sonn Launcher", "weapon 9" ),
	BIND( "MENUS_THROWABLE_WEAPONS", "Throwable Weapons", "weapon 10" ),
	BIND( "MENUS_NEXT_WEAPON", "Next Weapon", "weapnext" ),
	BIND( "MENUS_PREVIOUS_WEAPON", "Previous Weapon", "weapprev" ),
	TERM
};

static const prDef_t prForce1[] = {
	BIND( "MENUS_FORCE_PUSH", "Force Push", "force_throw" ),
	BIND( "MENUS_FORCE_PULL", "Force Pull", "force_pull" ),
	BIND( "MENUS_FORCE_SPEED", "Force Speed", "force_speed" ),
	BIND( "MENUS_FORCE_SIGHT", "Force Sight", "force_seeing" ),
	BIND( "MENUS_USE_FORCE_POWER", "Use Force Power", "+useforce" ),
	BIND( "MENUS_FORCE_NEXT", "Next Force Power", "forcenext" ),
	BIND( "MENUS_FORCE_PREVIOUS", "Previous Force Power", "forceprev" ),
	TERM
};

static const prDef_t prForce2[] = {
	BIND( "MENUS_FORCE_PROTECT", "Force Protect", "force_protect" ),
	BIND( "MENUS_FORCE_ABSORB", "Force Absorb", "force_absorb" ),
	BIND( "MENUS_FORCE_HEAL", "Force Heal", "force_heal" ),
	BIND( "MENUS_HEAL_OTHER", "Heal Other", "force_healother" ),
	BIND( "MENUS_FORCE_MINDTRICK", "Mind Trick", "force_distract" ),
	BIND( "MENUS_FORCE_GRIP", "Force Grip", "+force_grip" ),
	BIND( "MENUS_FORCE_DRAIN", "Force Drain", "+force_drain" ),
	BIND( "MENUS_FORCE_LIGHTNING", "Force Lightning", "+force_lightning" ),
	BIND( "MENUS_RAGE", "Force Rage", "force_rage" ),
	BIND( "MENUS_TEAM_POWER", "Team Power", "force_forcepowerother" ),
	TERM
};

static const prDef_t prMouse[] = {
	{ PR_CHOICE, "MENUS_FREE_LOOK", "Free Look", "cl_freelook", NOYES, 0, 0, 0 },
	{ PR_SLIDER, "MENUS_SENSITIVITY", "Sensitivity", "sensitivity", NULL, 2.0f, 30.0f, 0 },
	{ PR_CHOICE, "MENUS_INVERT_PITCH", "Invert Pitch", "m_pitch", NOYES, 0, 0, PF_PITCH },
	{ PR_CHOICE, "MENUS_SMOOTH_MOUSE", "Smooth Mouse", "m_filter", NOYES, 0, 0, 0 },
	{ PR_CHOICE, "MENUS_ENABLE_JOYSTICK", "Enable Joystick", "in_joystick", NOYES, 0, 0, 0 },
	{ PR_SLIDER, "MENUS_JOYSTICK_THRESHOLD", "Joystick Threshold", "joy_threshold", NULL, 0.05f, 0.75f, 0 },
	{ PR_CHOICE, "MENUS_X_AXIS_AS_BUTTONS", "X Axis as Buttons", "joy_xbutton", NOYES, 0, 0, 0 },
	{ PR_CHOICE, "MENUS_Y_AXIS_AS_BUTTONS", "Y Axis as Buttons", "joy_ybutton", NOYES, 0, 0, 0 },
	TERM
};

static const prDef_t prOther[] = {
	{ PR_CHOICE, "MENUS_ALWAYS_RUN", "Always Run", "cl_run", NOYES, 0, 0, 0 },
	{ PR_CHOICE, "MENUS_AUTO_SWITCH", "Auto Switch", "cg_autoswitch", "@MENUS_DON_T_SWITCH=Don't switch|0|"
		"@MENUS_BEST_SAFE_WEAPON=Best safe weapon|1|@MENUS_ALWAYS_BEST_WEAPON=Always best weapon|2", 0, 0, 0 },
	BIND( "MENUS_CHAT", "Chat", "messagemode" ),
	BIND( "MENUS_TEAM_CHAT", "Team Chat", "messagemode2" ),
	BIND( "MENUS_VC_CONTROLS", "Voice Chat", "voicechat" ),
	BIND( "MENUS_AUTOMAP", "Automap", "automap_toggle" ),
	BIND( "MENUS_SHOW_SCORES", "Show Scores", "+scores" ),
	BIND( "MENUS_SABER_CHALLENGE", "Saber Challenge", "engage_duel" ),
	BIND( "MENUS_3RD_PERSON", "3rd Person", "cg_thirdperson !" ),
	BIND( "MENUS_TAUNT", "Taunt", "taunt" ),
	BIND( "MENUS_BOW", "Bow", "bow" ),
	BIND( "MENUS_MEDITATE", "Meditate", "meditate" ),
	BIND( "MENUS_FLOURISH", "Flourish", "flourish" ),
	BIND( "MENUS_GLOAT", "Gloat", "gloat" ),
	TERM
};

typedef struct {
	const char		*name;
	const prDef_t	*rows;		// NULL: a tab drawn on its own (Defaults)
} prTab_t;

static const prTab_t prSetupTabs[] = {
	{ "Video", prVideo }, { "More Video", prMoreVideo }, { "Sound", prSound }, { "Options", prOptions }, { "Defaults", NULL },
};
static const prTab_t prControlTabs[] = {
	{ "Move", prMove }, { "Interact", prInteract }, { "Weapons", prWeapons }, { "Force 1", prForce1 }, { "Force 2", prForce2 },
	{ "Mouse", prMouse }, { "Other", prOther },
};

// a row as the page has it, the renderer's made from cl_rendermenu.cpp's table
typedef struct {
	prType_t	type;
	std::string	label, cvar, list, desc;
	float		min, max;
	int			flags;
} prRow_t;

#define PR_ROW_H	20.0f

static struct {
	int			page, tab;
	std::vector<prRow_t>	rows;		// the tab's
	std::vector<int>		shown;		// those shown now
	int			sel;					// in shown
	float		scroll, content;
	int			capture;				// the row a key is waited for, -1 none
	int			field;					// the row typed in, -1 none
	char		fieldBuf[16];
	int			drag;					// the slider dragged, -1 none
	qboolean	confirm;				// Defaults asked
	std::string	langText, langVoice;	// as they were when the page opened
	char		say[128];
	int			sayTime;
} pr = { 0, 0, {}, {}, 0, 0, 0, -1, -1, "", -1 };

/*
===============================================================================

Settings

===============================================================================
*/

// the game's string, or the English
static const char *PR_Str( const char *ref, const char *english ) {
	static char out[4][128];
	static int which;
	const char *s = ref ? SE_GetString( ref ) : "";
	char *o = out[which++ & 3];
	int len;

	Q_strncpyz( o, s && s[0] ? s : english, sizeof( out[0] ) );
	len = (int)strlen( o );
	while ( len > 0 && ( o[len - 1] == ':' || o[len - 1] == ' ' ) )
		o[--len] = '\0';
	return o;
}

// a setting as it is, or as it will be after a vid_restart
static const char *PR_Get( const char *cvar ) {
	const char *latched = Cvar_LatchedString( cvar );

	return latched ? latched : Cvar_VariableString( cvar );
}

static qboolean PR_Number( const char *s, double *v ) {
	char *end;

	if ( !s || !s[0] )
		return qfalse;
	*v = strtod( s, &end );
	return (qboolean)( *end == '\0' );
}

static qboolean PR_Same( const char *a, const char *b ) {
	double x, y;

	if ( PR_Number( a, &x ) && PR_Number( b, &y ) )
		return (qboolean)( fabs( x - y ) < 0.0001 );
	return (qboolean)!Q_stricmp( a, b );
}

// a choice's labels and values: "label|value|..."
static int PR_Choices( const prRow_t *row, std::vector<std::string> &labels, std::vector<std::string> &values ) {
	const std::string &l = row->list;
	size_t at = 0;
	int part = 0;

	labels.clear();
	values.clear();
	while ( at <= l.size() && !l.empty() ) {
		size_t end = l.find( '|', at );
		std::string word = l.substr( at, end == std::string::npos ? std::string::npos : end - at );

		if ( part++ & 1 ) {
			values.push_back( word );
		} else if ( !word.empty() && word[0] == '@' ) {
			const size_t eq = word.find( '=' );

			labels.push_back( PR_Str( word.substr( 1, eq == std::string::npos ? std::string::npos : eq - 1 ).c_str(),
				eq == std::string::npos ? word.c_str() + 1 : word.c_str() + eq + 1 ) );
		} else {
			labels.push_back( word );
		}
		if ( end == std::string::npos )
			break;
		at = end + 1;
	}
	if ( labels.size() > values.size() )
		labels.resize( values.size() );
	return (int)values.size();
}

// the row's value, as its list has it
static std::string PR_Value( const prRow_t *row ) {
	if ( row->flags & PF_PITCH )
		return Cvar_VariableValue( "m_pitch" ) < 0 ? "1" : "0";
	return PR_Get( row->cvar.c_str() );
}

static void PR_Say( const char *text ) {
	Q_strncpyz( pr.say, text, sizeof( pr.say ) );
	pr.sayTime = cls.realtime;
}

static void PR_Set( const prRow_t *row, const char *value ) {
	if ( row->flags & PF_PITCH ) {
		const float size = fabsf( Cvar_VariableValue( "m_pitch" ) );

		Cvar_Set( "m_pitch", va( "%g", ( atoi( value ) ? -1.0f : 1.0f ) * ( size > 0.0f ? size : 0.022f ) ) );
		Cvar_Set( "ui_mousePitch", value );
		return;
	}
	Cvar_Set( row->cvar.c_str(), value );
	if ( row->flags & PF_SWAY )
		Cbuf_AddText( atoi( value ) ? "exec noMotion.cfg\n" : "exec restoreMotion.cfg\n" );
	if ( row->flags & PF_SOUND ) {
		Cbuf_AddText( "snd_restart\n" );
		PR_Say( "The sound restarts" );
	}
}

static void PR_Step( const prRow_t *row, int dir ) {
	if ( row->type == PR_CHOICE ) {
		std::vector<std::string> labels, values;
		const int n = PR_Choices( row, labels, values );
		const std::string now = PR_Value( row );
		int at = -1;

		if ( !n )
			return;
		for ( int i = 0; i < n; i++ ) {
			if ( PR_Same( now.c_str(), values[i].c_str() ) )
				at = i;
		}
		at = at < 0 ? 0 : ( at + dir + n ) % n;
		PR_Set( row, values[at].c_str() );
	} else if ( row->type == PR_SLIDER ) {
		const float step = ( row->max - row->min ) / 20.0f;
		const float v = Com_Clamp( row->min, row->max, Cvar_VariableValue( row->cvar.c_str() ) + step * dir );

		Cvar_Set( row->cvar.c_str(), va( "%g", v ) );
	}
}

/*
===============================================================================

Key bindings

===============================================================================
*/

// the keys bound to a command, up to two; how many there are
static int PR_Keys( const char *cmd, int keys[2] ) {
	int n = 0;

	keys[0] = keys[1] = -1;
	for ( int k = 0; k < MAX_KEYS; k++ ) {
		const char *b = Key_GetBinding( k );

		if ( b && b[0] && !Q_stricmp( b, cmd ) ) {
			if ( n < 2 )
				keys[n] = k;
			n++;
		}
	}
	return n;
}

static void PR_Unbind( const char *cmd ) {
	for ( int k = 0; k < MAX_KEYS; k++ ) {
		const char *b = Key_GetBinding( k );

		if ( b && b[0] && !Q_stricmp( b, cmd ) )
			Key_SetBinding( k, "" );
	}
}

// as JKA's Controls page: a first or second key added; a third, or the same again, is the only one
static void PR_Bind( const char *cmd, int key ) {
	int keys[2];
	const int n = PR_Keys( cmd, keys );

	if ( !( n == 0 || ( n == 1 && keys[0] != key ) ) )
		PR_Unbind( cmd );
	Key_SetBinding( key, cmd );
}

static std::string PR_KeysText( const char *cmd ) {
	int keys[2];
	std::string out;

	if ( !PR_Keys( cmd, keys ) )
		return "";
	for ( int i = 0; i < 2 && keys[i] >= 0; i++ ) {
		if ( i )
			out += va( "  %s  ", PR_Str( "MENUS_KEYBIND_OR", "or" ) );
		out += Key_KeynumToString( keys[i] );
	}
	return out;
}

/*
===============================================================================

The rows

===============================================================================
*/

static int PR_NumTabs( void ) {
	return pr.page == ESCPAGE_SETUP ? (int)ARRAY_LEN( prSetupTabs ) : pr.page == ESCPAGE_CONTROLS ? (int)ARRAY_LEN( prControlTabs ) : 0;
}

static const prTab_t *PR_Tab( int tab ) {
	if ( pr.page == ESCPAGE_SETUP )
		return &prSetupTabs[tab];
	if ( pr.page == ESCPAGE_CONTROLS )
		return &prControlTabs[tab];
	return NULL;
}

// "cvarFloatList { "Off" 0 "On" 1 }" or cvarStrList's (commas between): "Off|0|On|1"
static std::string PR_MenuList( const char *list ) {
	std::string out;
	const char *p = list ? strchr( list, '{' ) : NULL;
	char token[MAX_TOKEN_CHARS];

	if ( !p )
		return out;
	p++;
	for ( ;; ) {
		const char *t = COM_ParseExt( &p, qtrue );

		if ( !t[0] || !strcmp( t, "}" ) )
			break;
		if ( !strcmp( t, "," ) )
			continue;
		Q_strncpyz( token, t, sizeof( token ) );
		if ( !out.empty() )
			out += "|";
		out += token;
	}
	return out;
}

static void PR_BuildRows( void ) {
	pr.rows.clear();
	if ( pr.page == ESCPAGE_RENDERER ) {
		for ( int i = 0; ; i++ ) {
			const char *label, *desc, *list;
			int show;
			const char *ui = CL_RenderMenu_RendererRow( i, &label, &desc, &list, &show );
			prRow_t row;

			if ( !ui )
				break;
			if ( !ui[0] )
				continue;		// the classic page's buttons between its two pages
			row.type = PR_CHOICE;
			row.label = PR_Str( NULL, label );
			row.cvar = ui;
			row.list = PR_MenuList( list );
			row.desc = desc ? desc : "";
			row.min = row.max = 0;
			row.flags = show == 1 ? PF_VULKAN : show == 2 ? PF_POSTFX : 0;
			pr.rows.push_back( row );
		}
		return;
	}
	{
		const prTab_t *tab = PR_Tab( pr.tab );

		for ( const prDef_t *d = tab ? tab->rows : NULL; d && d->label; d++ ) {
			prRow_t row;

			row.type = d->type;
			row.label = PR_Str( d->str, d->label );
			row.cvar = d->cvar;
			row.list = d->list ? d->list : "";
			row.min = d->min;
			row.max = d->max;
			row.flags = d->flags;
			// the game's text languages, as it has them
			if ( !Q_stricmp( d->cvar, "se_language" ) ) {
				for ( int l = 0; l < SE_GetNumLanguages(); l++ ) {
					std::string name = SE_GetLanguageName( l );

					if ( name.empty() )
						continue;
					if ( !row.list.empty() )
						row.list += "|";
					row.list += std::string( 1, (char)toupper( (unsigned char)name[0] ) ) + name.substr( 1 ) + "|" + name;
				}
			}
			pr.rows.push_back( row );
		}
	}
}

// the rows shown now: the custom size with the custom mode; Vulkan's with Vulkan picked
static void PR_BuildShown( void ) {
	const qboolean custom = (qboolean)( atoi( PR_Get( "r_mode" ) ) == -1 );
	const qboolean vulkan = (qboolean)!Q_stricmp( Cvar_VariableString( "ui_rd_renderer" ), "rd-vulkan" );
	const qboolean postfx = (qboolean)( vulkan && Cvar_VariableIntegerValue( "ui_rd_fbo" ) );

	pr.shown.clear();
	for ( int i = 0; i < (int)pr.rows.size(); i++ ) {
		const int f = pr.rows[i].flags;

		if ( ( f & PF_CUSTOM ) && !custom )
			continue;
		if ( ( f & PF_VULKAN ) && !vulkan )
			continue;
		if ( ( f & PF_POSTFX ) && !postfx )
			continue;
		pr.shown.push_back( i );
	}
	pr.sel = Com_Clampi( 0, Q_max( 0, (int)pr.shown.size() - 1 ), pr.sel );
}

static void PR_EndField( qboolean keep ) {
	if ( pr.field < 0 )
		return;
	if ( keep && pr.fieldBuf[0] && pr.field < (int)pr.rows.size() )
		Cvar_Set( pr.rows[pr.field].cvar.c_str(), pr.fieldBuf );
	pr.field = -1;
}

static void PR_PickTab( int tab ) {
	const int n = PR_NumTabs();

	if ( !n )
		return;
	PR_EndField( qtrue );
	pr.tab = ( tab + n ) % n;
	pr.sel = 0;
	pr.scroll = 0;
	pr.capture = -1;
	pr.drag = -1;
	pr.confirm = qfalse;
	PR_BuildRows();
	EscUI_Sound( qfalse );
}

void CL_EscPages_Open( int page ) {
	pr.page = page;
	pr.tab = 0;
	pr.sel = 0;
	pr.scroll = 0;
	pr.capture = pr.field = pr.drag = -1;
	pr.confirm = qfalse;
	pr.say[0] = '\0';
	pr.langText = Cvar_VariableString( "se_language" );
	pr.langVoice = Cvar_VariableString( "s_language" );
	if ( page == ESCPAGE_RENDERER )
		CL_RenderMenu_RendererSync();
	PR_BuildRows();
}

void CL_EscPages_Close( void ) {
	PR_EndField( qtrue );
	pr.capture = pr.drag = -1;
	pr.confirm = qfalse;
}

/*
===============================================================================

Drawn

===============================================================================
*/

static qboolean PR_VideoPending( void ) {
	for ( const prDef_t *d = prVideo; d->label; d++ ) {
		const char *latched = Cvar_LatchedString( d->cvar );

		if ( latched && Q_stricmp( latched, Cvar_VariableString( d->cvar ) ) )
			return qtrue;
	}
	return qfalse;
}

static void PR_DrawControl( const prRow_t *row, int index, float x, float y, float w ) {
	float cursorX, cursorY, clickX, clickY;
	qboolean *click;
	const float h = PR_ROW_H - 4;

	EscUI_Mouse( &cursorX, &cursorY, &click, &clickX, &clickY );
	switch ( row->type ) {
	case PR_CHOICE: {
		std::vector<std::string> labels, values;
		const int n = PR_Choices( row, labels, values );
		const std::string now = PR_Value( row );
		const float aw = EscUI_Wide( 16 );
		const char *shown = now.c_str();

		for ( int i = 0; i < n; i++ ) {
			if ( PR_Same( now.c_str(), values[i].c_str() ) )
				shown = labels[i].c_str();
		}
		if ( EscUI_Button( x, y, aw, h, "<", 0 ) )
			PR_Step( row, -1 );
		EscUI_Text( x + aw, y + 2, w - aw * 2, shown, EscUI_Color( 2 ), 0.66f, 1 );
		if ( EscUI_Clicked( x + aw, y, w - aw * 2, h ) )
			PR_Step( row, 1 );
		if ( EscUI_Button( x + w - aw, y, aw, h, ">", 0 ) )
			PR_Step( row, 1 );
		break;
	}
	case PR_SLIDER: {
		const float value = Cvar_VariableValue( row->cvar.c_str() );
		const float numW = EscUI_Wide( 40 ), tw = w - numW - 6;
		const float frac = row->max > row->min ? Com_Clamp( 0, 1, ( value - row->min ) / ( row->max - row->min ) ) : 0;
		const float ty = y + h * 0.5f;

		EscUI_Fill( x, ty - 1, tw, 2, EscUI_Color( 5 ) );
		EscUI_Fill( x, ty - 1, tw * frac, 2, EscUI_Color( 2 ) );
		EscUI_Fill( x + tw * frac - EscUI_Wide( 3 ), ty - 6, EscUI_Wide( 6 ), 12, EscUI_In( x - 4, y, tw + 8, h ) || pr.drag == index
			? EscUI_Color( 0 ) : EscUI_Color( 2 ) );
		EscUI_Text( x + tw + 6, y + 2, numW, row->max - row->min > 10 ? va( "%.1f", value ) : va( "%.2f", value ), EscUI_Color( 1 ), 0.62f, 0 );
		if ( EscUI_Clicked( x - 4, y, tw + 8, h ) )
			pr.drag = index;
		if ( pr.drag == index ) {
			if ( !Key_IsDown( A_MOUSE1 ) ) {
				pr.drag = -1;
			} else {
				const float range = row->max - row->min;
				float v = row->min + Com_Clamp( 0, 1, ( cursorX - x ) / tw ) * range;

				v = range > 10 ? floorf( v * 2.0f + 0.5f ) / 2.0f : floorf( v * 100.0f + 0.5f ) / 100.0f;
				Cvar_Set( row->cvar.c_str(), va( "%g", v ) );
			}
		}
		break;
	}
	case PR_BIND: {
		const qboolean waiting = (qboolean)( pr.capture == index );
		const std::string keys = PR_KeysText( row->cvar.c_str() );

		EscUI_Fill( x, y, w, h, waiting ? EscUI_Color( 3 ) : EscUI_Color( 4 ) );
		EscUI_Frame( x, y, w, h, waiting ? EscUI_Color( 2 ) : EscUI_Color( 5 ) );
		EscUI_Text( x + 4, y + 2, w - 8, waiting ? "Press a key..." : keys.empty() ? "---" : keys.c_str(),
			waiting ? EscUI_Color( 2 ) : keys.empty() ? EscUI_Color( 1 ) : EscUI_Color( 0 ), 0.66f, 1 );
		if ( EscUI_Clicked( x, y, w, h ) ) {
			PR_EndField( qtrue );
			pr.capture = index;
		}
		break;
	}
	case PR_FIELD: {
		wgStyle_t style;
		const qboolean focused = (qboolean)( pr.field == index );

		EscUI_StyleWG( &style, EscUI_Color( 0 ), 0.66f );
		EscUI_Fill( x, y, w, h, EscUI_Color( 4 ) );
		EscUI_Frame( x, y, w, h, focused ? EscUI_Color( 2 ) : EscUI_Color( 5 ) );
		if ( focused ) {
			WG_Field( &style, x, y, w, h, pr.fieldBuf, qtrue, "", cursorX, cursorY );
		} else {
			char now[16];

			Q_strncpyz( now, PR_Get( row->cvar.c_str() ), sizeof( now ) );
			WG_Field( &style, x, y, w, h, now, qfalse, "", cursorX, cursorY );
		}
		if ( EscUI_Clicked( x, y, w, h ) ) {
			if ( !focused ) {
				PR_EndField( qtrue );
				pr.field = index;
				Q_strncpyz( pr.fieldBuf, PR_Get( row->cvar.c_str() ), sizeof( pr.fieldBuf ) );
				WG_EditReset();
			}
			WG_FieldClick( pr.fieldBuf, clickX, clickY );
		}
		break;
	}
	}
}

static void PR_DrawDefaults( float x, float y, float w, float h ) {
	wgStyle_t style;
	const char *text = "Every setting back to the game's defaults: video, sound, game options and the mouse, as when the game "
		"was first started. Your key bindings stay; the defaults' own are added. The video restarts.";
	int lines;

	EscUI_StyleWG( &style, EscUI_Color( 0 ), 0.68f );
	lines = WG_TextLines( &style, text, w - 20 );
	WG_DrawText( &style, x + 10, y + 10, w - 20, text, 0, lines, EscUI_Color( 0 ) );
	y += 20 + lines * style.lineH;
	if ( !pr.confirm ) {
		if ( EscUI_Button( x + 10, y, EscUI_Wide( 170 ), 22, "Reset to the defaults", 2 ) )
			pr.confirm = qtrue;
		return;
	}
	EscUI_Text( x + 10, y + 4, w - 20, "Sure? This can't be undone.", EscUI_Color( 6 ), 0.7f, 0 );
	y += 26;
	if ( EscUI_Button( x + 10, y, EscUI_Wide( 110 ), 22, "Reset", 2 ) ) {
		// as JKA's resetdefaults: the cvars, the defaults' config, the video
		Cbuf_AddText( "cvar_restart\nexec mpdefault.cfg\nvid_restart\n" );
		EscUI_Restarting();
		return;
	}
	if ( EscUI_Button( x + 20 + EscUI_Wide( 110 ), y, EscUI_Wide( 110 ), 22, "Keep them", 0 ) )
		pr.confirm = qfalse;
}

void CL_EscPages_Draw( int page, float x, float y, float w, float h ) {
	const int tabs = PR_NumTabs();
	const prTab_t *tab = tabs ? PR_Tab( pr.tab ) : NULL;
	const float tabsH = tabs ? 20.0f : 0.0f, footH = 28.0f;
	const float top = y + tabsH + ( tabs ? 6.0f : 0.0f ), viewH = y + h - footH - top;
	float cursorX, cursorY, clickX, clickY;
	qboolean *click;

	if ( page != pr.page )
		CL_EscPages_Open( page );
	EscUI_Mouse( &cursorX, &cursorY, &click, &clickX, &clickY );

	// the tabs along the top
	if ( tabs ) {
		const float gap = 3.0f, tw = ( w - gap * ( tabs - 1 ) ) / tabs;

		for ( int t = 0; t < tabs; t++ ) {
			if ( EscUI_Button( x + t * ( tw + gap ), y, tw, tabsH, PR_Tab( t )->name, t == pr.tab ? 1 : 0 ) && t != pr.tab )
				PR_PickTab( t );
		}
	}

	// a field clicked away from: what's typed kept
	if ( pr.field >= 0 && *click ) {
		qboolean inside = qfalse;

		for ( int i = 0; i < (int)pr.shown.size(); i++ ) {
			const float ry = top + i * PR_ROW_H - pr.scroll;

			if ( pr.shown[i] == pr.field && clickY >= ry && clickY < ry + PR_ROW_H && clickX >= x + w * 0.46f )
				inside = qtrue;
		}
		if ( !inside )
			PR_EndField( qtrue );
	}

	if ( tab && !tab->rows ) {
		PR_DrawDefaults( x, top, w, viewH );
	} else {
		PR_BuildShown();
		pr.content = pr.shown.size() * PR_ROW_H;
		{
			const qboolean bar = WG_ScrollbarPixels( x + w - WG_SCROLL_W, top, viewH, pr.content, &pr.scroll, cursorX, cursorY,
				click, clickX, clickY );
			const float rw = w - ( bar ? WG_SCROLL_W + 4 : 0 ), labelW = rw * 0.46f;

			pr.scroll = Com_Clamp( 0, Q_max( 0.0f, pr.content - viewH ), pr.scroll );
			for ( int i = 0; i < (int)pr.shown.size(); i++ ) {
				const prRow_t *row = &pr.rows[pr.shown[i]];
				const float ry = top + i * PR_ROW_H - pr.scroll;
				const qboolean on = (qboolean)( i == pr.sel );

				if ( ry < top - 0.5f || ry + PR_ROW_H > top + viewH + 0.5f )
					continue;
				if ( on || EscUI_In( x, ry, rw, PR_ROW_H - 2 ) )
					EscUI_Fill( x, ry, rw, PR_ROW_H - 2, on ? EscUI_Color( 3 ) : EscUI_Color( 4 ) );
				EscUI_Text( x + 6, ry + 3, labelW - 10, row->label.c_str(), EscUI_Color( 0 ), 0.66f, 0 );
				PR_DrawControl( row, pr.shown[i], x + labelW, ry + 1, rw - labelW - 4 );
				// a click anywhere else on it picks it
				if ( EscUI_Clicked( x, ry, rw, PR_ROW_H - 2 ) )
					pr.sel = i;
			}
			if ( pr.shown.empty() )
				EscUI_Text( x, top + 10, w, "Nothing to set here", EscUI_Color( 1 ), 0.7f, 1 );
		}
	}

	// along the bottom: what applies the changes, or what's to know
	{
		const float fy = y + h - footH + 6, bw = EscUI_Wide( 150 );
		const char *note = "";

		if ( pr.say[0] && cls.realtime - pr.sayTime < 4000 ) {
			note = pr.say;
		} else if ( pr.capture >= 0 ) {
			note = "Press a key for it   Esc: cancel   Backspace: no key";
		} else if ( page == ESCPAGE_CONTROLS ) {
			note = "Click a row, then press its key";
		} else if ( page == ESCPAGE_RENDERER && pr.sel < (int)pr.shown.size() ) {
			note = pr.rows[pr.shown[pr.sel]].desc.c_str();
		}

		if ( page == ESCPAGE_SETUP && pr.tab == 0 && PR_VideoPending() ) {
			if ( EscUI_Button( x + w - bw, fy, bw, 20, "Apply (restarts video)", 1 ) ) {
				Cbuf_AddText( "vid_restart\n" );
				EscUI_Restarting();
				return;
			}
			if ( !note[0] )
				note = "Changes here take effect when applied";
		} else if ( page == ESCPAGE_RENDERER && CL_RenderMenu_RendererChanged() ) {
			if ( EscUI_Button( x + w - bw, fy, bw, 20, "Apply (restarts video)", 1 ) ) {
				Cbuf_AddText( "renderersettings apply\nvid_restart\n" );
				EscUI_Restarting();
				return;
			}
		} else if ( page == ESCPAGE_SETUP && pr.tab == 3
			&& ( Q_stricmp( pr.langText.c_str(), Cvar_VariableString( "se_language" ) ) || Q_stricmp( pr.langVoice.c_str(), Cvar_VariableString( "s_language" ) ) ) ) {
			if ( EscUI_Button( x + w - bw, fy, bw, 20, PR_Str( "MENUS_SET_LANGUAGE", "Set Language" ), 1 ) ) {
				Cbuf_AddText( "ui_load\nsnd_restart\n" );
				pr.langText = Cvar_VariableString( "se_language" );
				pr.langVoice = Cvar_VariableString( "s_language" );
				PR_Say( "The language is set" );
			}
		} else {
			EscUI_Text( x, fy + 3, w, note, EscUI_Color( 1 ), 0.6f, 0 );
			return;
		}
		EscUI_Text( x, fy + 3, w - bw - 8, note, EscUI_Color( 1 ), 0.6f, 0 );
	}
}

/*
===============================================================================

Keys

===============================================================================
*/

static qboolean PR_DigitsOnly( int ch ) {
	return (qboolean)( ch >= '0' && ch <= '9' );
}

qboolean CL_EscPages_Key( int page, int key ) {
	const prTab_t *tab = PR_NumTabs() ? PR_Tab( pr.tab ) : NULL;
	const prRow_t *row = pr.sel < (int)pr.shown.size() ? &pr.rows[pr.shown[pr.sel]] : NULL;

	if ( page != pr.page )
		return qfalse;
	// a key for a binding, whatever it is: the mouse's buttons and wheel too
	if ( pr.capture >= 0 ) {
		const char *was = Key_GetBinding( key );

		if ( key == A_BACKSPACE || key == A_DELETE ) {
			PR_Unbind( pr.rows[pr.capture].cvar.c_str() );
		} else if ( was && !Q_stricmp( was, "toggleconsole" ) ) {
			PR_Say( "That key opens the console: pick another" );
			return qtrue;
		} else {
			PR_Bind( pr.rows[pr.capture].cvar.c_str(), key );
		}
		pr.capture = -1;
		EscUI_Sound( qtrue );
		return qtrue;
	}
	// typing in a field
	if ( pr.field >= 0 ) {
		if ( key == A_ENTER || key == A_KP_ENTER || key == A_TAB ) {
			PR_EndField( qtrue );
			return qtrue;
		}
		if ( key == A_MOUSE1 || key == A_MOUSE2 )
			return qfalse;
		WG_EditKey( pr.fieldBuf, sizeof( pr.fieldBuf ), key, PR_DigitsOnly );
		return qtrue;
	}
	switch ( key ) {
	case A_MWHEELUP:
	case A_MWHEELDOWN:
		pr.scroll += key == A_MWHEELUP ? -40.0f : 40.0f;
		return qtrue;
	case A_TAB:
		PR_PickTab( pr.tab + ( Key_IsDown( A_SHIFT ) ? -1 : 1 ) );
		return qtrue;
	case A_CURSOR_UP:
	case A_CURSOR_DOWN:
		if ( !pr.shown.empty() ) {
			pr.sel = Com_Clampi( 0, (int)pr.shown.size() - 1, pr.sel + ( key == A_CURSOR_UP ? -1 : 1 ) );
			EscUI_Sound( qfalse );
			// kept in view: the list is about 14 rows; the draw clamps the rest
			if ( pr.sel * PR_ROW_H < pr.scroll )
				pr.scroll = pr.sel * PR_ROW_H;
			else if ( ( pr.sel + 1 ) * PR_ROW_H > pr.scroll + PR_ROW_H * 12 )
				pr.scroll = ( pr.sel + 1 ) * PR_ROW_H - PR_ROW_H * 12;
		}
		return qtrue;
	case A_CURSOR_LEFT:
	case A_CURSOR_RIGHT:
		if ( tab && !tab->rows ) {
			PR_PickTab( pr.tab + ( key == A_CURSOR_LEFT ? -1 : 1 ) );
			return qtrue;
		}
		if ( row && ( row->type == PR_CHOICE || row->type == PR_SLIDER ) ) {
			PR_Step( row, key == A_CURSOR_LEFT ? -1 : 1 );
			EscUI_Sound( qtrue );
		}
		return qtrue;
	case A_ENTER:
	case A_KP_ENTER:
		if ( !row )
			return qtrue;
		EscUI_Sound( qtrue );
		if ( row->type == PR_BIND ) {
			pr.capture = pr.shown[pr.sel];
		} else if ( row->type == PR_FIELD ) {
			pr.field = pr.shown[pr.sel];
			Q_strncpyz( pr.fieldBuf, PR_Get( row->cvar.c_str() ), sizeof( pr.fieldBuf ) );
			WG_EditReset();
		} else if ( row->type == PR_CHOICE ) {
			PR_Step( row, 1 );
		}
		return qtrue;
	case A_DELETE:
		if ( row && row->type == PR_BIND )
			PR_Unbind( row->cvar.c_str() );
		return qtrue;
	default:
		return qfalse;
	}
}

void CL_EscPages_Char( int page, int ch ) {
	if ( page != pr.page || pr.capture >= 0 || pr.field < 0 )
		return;
	WG_EditChar( pr.fieldBuf, sizeof( pr.fieldBuf ), ch, PR_DigitsOnly );
}

// Esc: the key waited for, the field, or Defaults' question given up first
qboolean CL_EscPages_Escape( void ) {
	if ( pr.capture >= 0 ) {
		pr.capture = -1;
		return qtrue;
	}
	if ( pr.field >= 0 ) {
		PR_EndField( qfalse );
		return qtrue;
	}
	if ( pr.confirm ) {
		pr.confirm = qfalse;
		return qtrue;
	}
	return qfalse;
}
