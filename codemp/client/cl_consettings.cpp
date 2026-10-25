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

// cl_consettings.cpp -- /consolesettings: how the console looks
//
// A window over the console, held down while it's open so each change shows as it's made:
// how see-through it is (con_opacity), how big its letters are (con_scale), how far down it
// comes (con_height) and the picture behind it (con_background): the standard one, an image put
// in the consolebg folder, or any image or shader typed in.

#include "client.h"

#define CS_COMMAND		"consolesettings"
#define CS_FOLDER		"consolebg"
#define CS_MAX_IMAGES	64
#define CS_X			12.0f
#define CS_Y			166.0f
#define CS_W			616.0f
#define CS_H			306.0f
#define CS_COL_W		290.0f
#define CS_ROW_H		13.0f
#define CS_CTRL_H		15.0f
#define CS_GAP			5.0f
#define CS_SCALE		0.8f
#define CS_LIST_ROWS	5

static vec4_t csPanel		= { 0.06f, 0.07f, 0.09f, 0.94f };
static vec4_t csLight		= { 0.16f, 0.17f, 0.21f, 1.0f };
static vec4_t csHover		= { 0.24f, 0.26f, 0.32f, 1.0f };
static vec4_t csLit			= { 0.18f, 0.42f, 0.62f, 1.0f };
static vec4_t csBorder		= { 0.35f, 0.38f, 0.45f, 1.0f };
static vec4_t csWhite		= { 1.0f, 1.0f, 1.0f, 1.0f };
static vec4_t csDim			= { 0.65f, 0.67f, 0.72f, 1.0f };

static cvar_t *con_background;

static struct {
	qboolean	open;
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;
	int			wheel;
	qboolean	typing;
	char		typed[MAX_QPATH];

	char		images[CS_MAX_IMAGES][MAX_QPATH];
	int			numImages;
	int			scroll;

	// the picture behind the console, as last registered
	char		shownName[MAX_QPATH];
	qhandle_t	shown;
	char		message[MAX_STRING_CHARS];
	int			messageTime;
} cs;

static void CS_Message( const char *text ) {
	Q_strncpyz( cs.message, text, sizeof( cs.message ) );
	cs.messageTime = cls.realtime;
}

// the picture behind the console: con_background's, or the standard one
qhandle_t CL_ConSettings_Background( void ) {
	if ( !con_background || !con_background->string[0] || !cls.rendererStarted )
		return cls.consoleShader;
	if ( Q_stricmp( cs.shownName, con_background->string ) || !cs.shown ) {
		Q_strncpyz( cs.shownName, con_background->string, sizeof( cs.shownName ) );
		cs.shown = re->RegisterShaderNoMip( con_background->string );
	}
	return cs.shown ? cs.shown : cls.consoleShader;
}

static int QDECL CS_Compare( const void *a, const void *b ) {
	return Q_stricmp( (const char *)a, (const char *)b );
}

// the images in the consolebg folder
static void CS_ListImages( void ) {
	static const char *exts[] = { "jpg", "png", "tga" };

	cs.numImages = 0;
	for ( size_t e = 0; e < ARRAY_LEN( exts ); e++ ) {
		int count = 0;
		char **list = FS_ListFiles( CS_FOLDER, exts[e], &count );

		for ( int i = 0; i < count && cs.numImages < CS_MAX_IMAGES; i++ )
			Com_sprintf( cs.images[cs.numImages++], MAX_QPATH, "%s/%s", CS_FOLDER, list[i] );
		FS_FreeFileList( list );
	}
	qsort( cs.images, cs.numImages, sizeof( cs.images[0] ), CS_Compare );
}

qboolean CL_ConSettings_Active( void ) {
	return cs.open;
}

static void CS_Open( void ) {
	cs.open = qtrue;
	cs.cursorX = CS_X + CS_W * 0.5f;
	cs.cursorY = CS_Y + 60;
	cs.typing = qfalse;
	Q_strncpyz( cs.typed, con_background->string, sizeof( cs.typed ) );
	CS_ListImages();
	// the console's own keys let go: the window takes them, the console stays down to be seen
	Key_SetCatcher( ( Key_GetCatcher() & ~KEYCATCH_CONSOLE ) | KEYCATCH_CONSETTINGS );
}

void CL_ConSettings_Close( void ) {
	if ( !cs.open )
		return;
	cs.open = qfalse;
	cs.typing = qfalse;
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_CONSETTINGS );
}

static void CS_Command_f( void ) {
	if ( cs.open )
		CL_ConSettings_Close();
	else
		CS_Open();
}

void CL_ConSettings_Init( void ) {
	con_background = Cvar_Get( "con_background", "", CVAR_ARCHIVE_ND, "The picture behind the console: an image or a shader, empty for the standard one" );
	Cmd_AddCommand( CS_COMMAND, CS_Command_f, "How the console looks: how see-through, its letters' size, how far down, the picture behind it" );
	memset( &cs, 0, sizeof( cs ) );
}

void CL_ConSettings_Shutdown( void ) {
	CL_ConSettings_Close();
	Cmd_RemoveCommand( CS_COMMAND );
	cs.shown = 0;
	cs.shownName[0] = '\0';
}

// the renderer started again: the picture is registered anew
void CL_ConSettings_RendererRestarted( void ) {
	Con_FontsLost();
	cs.shown = 0;
	cs.shownName[0] = '\0';
}

/*
===============================================================================

The window

===============================================================================
*/

static void CS_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void CS_Box( float x, float y, float w, float h, const float *fill ) {
	CS_Fill( x, y, w, h, fill );
	CS_Fill( x, y, w, 1, csBorder );
	CS_Fill( x, y + h - 1, w, 1, csBorder );
	CS_Fill( x, y, 1, h, csBorder );
	CS_Fill( x + w - 1, y, 1, h, csBorder );
}

static float CS_TextWidth( const char *text ) {
	return (float)re->Font_StrLenPixels( text, cls.menuFont, CS_SCALE );
}

static void CS_Text( float x, float y, const char *text, const float *color ) {
	re->Font_DrawString( (int)x, (int)y, text, color, cls.menuFont, -1, CS_SCALE );
}

static void CS_TextClipped( float x, float y, float w, const char *text, const float *color ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	for ( len = strlen( buf ); len > 0 && CS_TextWidth( buf ) > w; len-- )
		buf[len - 1] = '\0';
	CS_Text( x, y, buf, color );
}

static qboolean CS_In( float x, float y, float w, float h ) {
	return (qboolean)( cs.cursorX >= x && cs.cursorX < x + w && cs.cursorY >= y && cs.cursorY < y + h );
}

static qboolean CS_Clicked( float x, float y, float w, float h ) {
	if ( !cs.click || cs.clickX < x || cs.clickX >= x + w || cs.clickY < y || cs.clickY >= y + h )
		return qfalse;
	cs.click = qfalse;
	return qtrue;
}

static qboolean CS_Button( float x, float y, float w, const char *label, qboolean lit ) {
	CS_Box( x, y, w, CS_CTRL_H, lit ? csLit : CS_In( x, y, w, CS_CTRL_H ) ? csHover : csLight );
	CS_Text( x + ( w - CS_TextWidth( label ) ) * 0.5f, y + 2, label, csWhite );
	return CS_Clicked( x, y, w, CS_CTRL_H );
}

// a value with - and + either side, the wheel over it too; how many steps it went
static int CS_Stepper( float x, float y, float w, const char *label, const char *value ) {
	const float lw = 86.0f, vx = x + lw + 18, vw = w - lw - 36;
	int steps = 0;

	CS_Text( x, y + 2, label, csDim );
	if ( CS_Button( x + lw, y, 16, "-", qfalse ) )
		steps--;
	CS_Box( vx, y, vw, CS_CTRL_H, csLight );
	CS_Text( vx + ( vw - CS_TextWidth( value ) ) * 0.5f, y + 2, value, csWhite );
	if ( CS_Button( x + w - 16, y, 16, "+", qfalse ) )
		steps++;
	if ( cs.wheel && CS_In( x, y, w, CS_CTRL_H ) ) {
		steps -= cs.wheel;
		cs.wheel = 0;
	}
	return steps;
}

static void CS_Step( cvar_t *cv, int steps, float step, float min, float max ) {
	if ( !steps )
		return;
	Cvar_SetValue( cv->name, Com_Clamp( min, max, floorf( ( cv->value + steps * step ) / step + 0.5f ) * step ) );
}

static void CS_SetBackground( const char *name ) {
	Cvar_Set( "con_background", name );
	Q_strncpyz( cs.typed, name, sizeof( cs.typed ) );
	CS_Message( name[0] ? va( "Behind the console: %s", name ) : "Behind the console: the standard picture" );
}

// plain text's colours to pick from, "" white
static const struct {
	const char	*name, *rgb;
} csTextColors[] = {
	{ "White", "" }, { "Soft white", "235 235 225" }, { "Light grey", "200 200 200" }, { "Light yellow", "255 245 170" },
	{ "Amber", "255 200 90" }, { "Light green", "170 255 170" }, { "Light blue", "170 210 255" },
};

static int CS_TextColor( void ) {
	extern cvar_t *con_textColor;

	for ( size_t i = 0; i < ARRAY_LEN( csTextColors ); i++ ) {
		if ( !Q_stricmp( con_textColor->string, csTextColors[i].rgb ) )
			return (int)i;
	}
	return -1;
}

// one of a few: steps go round
static void CS_Cycle( cvar_t *cv, int steps, int count ) {
	if ( steps )
		Cvar_SetValue( cv->name, (float)( ( ( cv->integer + steps ) % count + count ) % count ) );
}

// drawn over the console, so what changes shows
void CL_ConSettings_Draw( void ) {
	extern cvar_t *con_opacity, *con_scale, *con_height, *con_timestamps, *con_notifytime;
	extern cvar_t *con_lineSpacing, *con_textShadow, *con_bgDarken, *con_margin, *con_textColor, *con_contrast, *con_colorblind,
		*con_noFade, *con_font, *con_notifyScale;
	static const char *shadows[] = { "None", "A shadow", "An outline" };
	static const char *contrasts[] = { "As sent", "Dark colours lit", "All in the text colour" };
	static const char *blinds[] = { "Off", "Red-green", "Blue-yellow" };
	const float x = CS_X + 10, w = CS_COL_W, x2 = CS_X + CS_W - 10 - CS_COL_W;
	float y = CS_Y + 6, y2;
	int steps;

	if ( !cs.open )
		return;
	if ( !( Key_GetCatcher() & KEYCATCH_CONSETTINGS ) ) {
		cs.open = qfalse;
		return;
	}
	CS_Box( CS_X, CS_Y, CS_W, CS_H, csPanel );
	CS_Text( x, y, "Console settings", csWhite );
	if ( CS_Button( CS_X + CS_W - 10 - 84 - 110, y - 2, 110, "Everything as it was", qfalse ) ) {
		static const char *defaults[][2] = {
			{ "con_opacity", "1.0" }, { "con_scale", "1" }, { "con_height", "0.5" }, { "con_lineSpacing", "0" }, { "con_textShadow", "0" },
			{ "con_bgDarken", "0" }, { "con_margin", "0" }, { "con_textColor", "" }, { "con_contrast", "0" }, { "con_colorblind", "0" },
			{ "con_noFade", "0" }, { "con_font", "0" }, { "con_notifyScale", "0" }, { "con_notifytime", "3" }, { "con_timestamps", "0" },
		};

		for ( size_t i = 0; i < ARRAY_LEN( defaults ); i++ )
			Cvar_Set( defaults[i][0], defaults[i][1] );
		CS_SetBackground( "" );
	}
	if ( CS_Button( CS_X + CS_W - 10 - 80, y - 2, 80, "Close (Esc)", qfalse ) ) {
		CL_ConSettings_Close();
		return;
	}
	y += CS_CTRL_H + CS_GAP + 2;
	y2 = y;

	// left: how it's laid out and drawn
	steps = CS_Stepper( x, y, w, "See-through", va( "%i%% solid", (int)( Com_Clamp( 0.0f, 1.0f, con_opacity->value ) * 100.0f + 0.5f ) ) );
	CS_Step( con_opacity, steps, 0.05f, 0.0f, 1.0f );
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x, y, w, "Letters", va( "x%.1f", con_scale->value > 0.0f ? con_scale->value : 1.0f ) );
	CS_Step( con_scale, steps, 0.1f, 0.5f, 3.0f );
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x, y, w, "Font", Con_FontName( con_font->integer ) );
	CS_Cycle( con_font, steps, Con_NumFonts() );
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x, y, w, "Behind letters", shadows[Com_Clampi( 0, 2, con_textShadow->integer )] );
	CS_Cycle( con_textShadow, steps, 3 );
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x, y, w, "Line spacing", con_lineSpacing->value > 0.0f ? va( "+%i%% of a letter", (int)( con_lineSpacing->value * 100.0f + 0.5f ) ) : "none" );
	CS_Step( con_lineSpacing, steps, 0.1f, 0.0f, 1.5f );
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x, y, w, "Margins", con_margin->integer > 0 ? va( "%i letters each side", con_margin->integer ) : "none" );
	CS_Step( con_margin, steps, 1.0f, 0.0f, 30.0f );
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x, y, w, "Comes down", va( "%i%% of the screen", (int)( Com_Clamp( 0.1f, 1.0f, con_height->value ) * 100.0f + 0.5f ) ) );
	CS_Step( con_height, steps, 0.05f, 0.1f, 1.0f );
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x, y, w, "Timestamps", con_timestamps->integer ? "shown" : "hidden" );
	CS_Cycle( con_timestamps, steps, 2 );
	y += CS_CTRL_H + CS_GAP + 4;

	// the picture behind it
	CS_Text( x, y, "Behind it", csDim );
	CS_TextClipped( x + 76, y, w - 76, va( S_COLOR_GREY "images in %s/ are listed", CS_FOLDER ), csWhite );
	y += CS_ROW_H + 6;
	if ( cs.wheel && CS_In( x, y, w, CS_LIST_ROWS * CS_ROW_H ) ) {
		cs.scroll += cs.wheel;
		cs.wheel = 0;
	}
	cs.scroll = Com_Clampi( 0, Q_max( 0, cs.numImages + 1 - CS_LIST_ROWS ), cs.scroll );
	CS_Box( x, y, w, CS_LIST_ROWS * CS_ROW_H + 2, csLight );
	for ( int i = -1; i < cs.numImages; i++ ) {
		const int r = i + 1 - cs.scroll;
		const float ry = y + 1 + r * CS_ROW_H;
		const char *name = i < 0 ? "" : cs.images[i];
		const qboolean on = (qboolean)!Q_stricmp( name, con_background->string );

		if ( r < 0 || r >= CS_LIST_ROWS )
			continue;
		if ( on )
			CS_Fill( x + 1, ry, w - 2, CS_ROW_H, csLit );
		else if ( CS_In( x, ry, w, CS_ROW_H ) )
			CS_Fill( x + 1, ry, w - 2, CS_ROW_H, csHover );
		CS_TextClipped( x + 4, ry, w - 8, i < 0 ? "The standard picture" : COM_SkipPath( (char *)name ), csWhite );
		if ( CS_Clicked( x, ry, w, CS_ROW_H ) )
			CS_SetBackground( name );
	}
	y += CS_LIST_ROWS * CS_ROW_H + 2 + CS_GAP;
	CS_Box( x, y, w - 44 - 64, CS_CTRL_H, cs.typing ? csLit : csLight );
	CS_TextClipped( x + 4, y + 2, w - 116, cs.typed[0] || cs.typing ? va( "%s%s", cs.typed, cs.typing && ( ( cls.realtime >> 8 ) & 1 ) ? "_" : "" )
		: S_COLOR_GREY "or type an image or a shader", csWhite );
	if ( CS_Clicked( x, y, w - 44 - 64, CS_CTRL_H ) )
		cs.typing = qtrue;
	if ( CS_Button( x + w - 104, y, 40, "Use", qfalse ) ) {
		cs.typing = qfalse;
		CS_SetBackground( cs.typed );
	}
	if ( CS_Button( x + w - 60, y, 60, "Read again", qfalse ) )
		CS_ListImages();

	// right: colours, and the lines over the game
	y = y2;
	steps = CS_Stepper( x2, y, w, "Text colour", CS_TextColor() >= 0 ? csTextColors[CS_TextColor()].name : con_textColor->string );
	if ( steps ) {
		const int n = (int)ARRAY_LEN( csTextColors );

		Cvar_Set( "con_textColor", csTextColors[( ( Q_max( 0, CS_TextColor() ) + steps ) % n + n ) % n].rgb );
	}
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x2, y, w, "Contrast", contrasts[Com_Clampi( 0, 2, con_contrast->integer )] );
	CS_Cycle( con_contrast, steps, 3 );
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x2, y, w, "Colour-blind", blinds[Com_Clampi( 0, 2, con_colorblind->integer )] );
	CS_Cycle( con_colorblind, steps, 3 );
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x2, y, w, "Fading", con_noFade->integer ? "nothing faded" : "far chat faded" );
	CS_Cycle( con_noFade, steps, 2 );
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x2, y, w, "Darker behind", con_bgDarken->value > 0.0f ? va( "%i%%", (int)( con_bgDarken->value * 100.0f + 0.5f ) ) : "no" );
	CS_Step( con_bgDarken, steps, 0.1f, 0.0f, 0.9f );
	y += CS_CTRL_H + CS_GAP + 8;

	CS_Text( x2, y, "Lines over the game", csWhite );
	y += CS_ROW_H + 4;
	steps = CS_Stepper( x2, y, w, "Letters", con_notifyScale->value > 0.0f ? va( "x%.2g", con_notifyScale->value ) : "as the console's" );
	CS_Step( con_notifyScale, steps, 0.25f, 0.0f, 4.0f );
	y += CS_CTRL_H + CS_GAP;
	steps = CS_Stepper( x2, y, w, "Stay", va( "%i s", con_notifytime->integer ) );
	CS_Step( con_notifytime, steps, 1.0f, 1.0f, 30.0f );
	y += CS_CTRL_H + CS_GAP + 8;

	CS_TextClipped( x2, y, w, S_COLOR_GREY "The console behind this window shows each change", csWhite );
	y += CS_ROW_H + 4;
	if ( cs.message[0] && cls.realtime - cs.messageTime < 5000 )
		CS_TextClipped( x2, y, w, cs.message, csWhite );

	cs.click = qfalse;
	cs.wheel = 0;
	re->SetColor( NULL );
	re->DrawStretchPic( cs.cursorX, cs.cursorY, 24, 24, 0, 0, 1, 1, cls.cursorShader );
}

void CL_ConSettings_KeyEvent( int key, qboolean down ) {
	int len;

	if ( !cs.open || !down )
		return;
	switch ( key ) {
	case A_MOUSE1:
		cs.click = qtrue;
		cs.clickX = cs.cursorX;
		cs.clickY = cs.cursorY;
		if ( cs.typing && !( cs.cursorX >= CS_X && cs.cursorX < CS_X + CS_W ) )
			cs.typing = qfalse;
		return;
	case A_MWHEELUP:
		cs.wheel--;
		return;
	case A_MWHEELDOWN:
		cs.wheel++;
		return;
	case A_ENTER:
	case A_KP_ENTER:
		if ( cs.typing ) {
			cs.typing = qfalse;
			CS_SetBackground( cs.typed );
		}
		return;
	case A_BACKSPACE:
		len = strlen( cs.typed );
		if ( cs.typing && len )
			cs.typed[len - 1] = '\0';
		return;
	default:
		return;
	}
}

void CL_ConSettings_CharEvent( int ch ) {
	const int len = strlen( cs.typed );

	if ( !cs.open || !cs.typing || ch < ' ' || ch > '~' || ch == '"' || ch == ';' )
		return;
	if ( len < (int)sizeof( cs.typed ) - 1 ) {
		cs.typed[len] = (char)ch;
		cs.typed[len + 1] = '\0';
	}
}

void CL_ConSettings_MouseEvent( int dx, int dy ) {
	cs.cursorX = Com_Clamp( 0, SCREEN_WIDTH, cs.cursorX + dx );
	cs.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, cs.cursorY + dy );
}

void CL_ConSettings_Escape( void ) {
	if ( cs.typing )
		cs.typing = qfalse;
	else
		CL_ConSettings_Close();
}
