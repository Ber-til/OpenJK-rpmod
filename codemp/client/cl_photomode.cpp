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

// cl_photomode.cpp -- photo mode: the view taken off the player for a free camera, to take
// pictures. /photomode (or Photo Mode under the Esc menu's Tools): the HUD and chat hidden, your own
// character shown, the game going on. Hold the right mouse button to look, WASD (Space, C) to fly,
// Shift fast, Ctrl slow, Q and E roll, the wheel zooms. A click on the scene focuses there (depth of
// field). The panel: the camera (field of view, roll, depth of field, a frame of an aspect to shoot
// in), the look (the cinematic camera's filters, colour grading, grain, vignette, colour fringes,
// scanlines). Snap (or Enter) saves a PNG at the window's full size, cut to the frame, in
// screenshots/. H hides the panel. Depth of field and the look need Vulkan; the camera works with
// either renderer. The server only sends what's near you, so the camera keeps within reach.
// An idea from a player's report (C1B3R #46).

#include "client.h"
#include "qcommon/cm_public.h"
#include "cl_rpcinematic.h"

#define PM_CMD			"photomode"
#define PM_MAX_DIST		2000.0f		// the camera from the player, at most: further, people aren't sent
#define PM_SPEED		240.0f
#define PM_X			406.0f
#define PM_Y			40.0f
#define PM_W			224.0f
#define PM_H			400.0f
#define PM_IN			( PM_X + 10.0f )
#define PM_IW			( PM_W - 20.0f )
#define PM_CTRL_H		14.0f
#define PM_ROW			17.0f
#define PM_SCALE		0.7f
#define PM_MESSAGE_MS	5000
#define PM_SNAP_FRAMES	4			// drawn without the panel before the shot's taken, on the last
#define PM_CHAT_REACH	64.0f		// Chat's camera: from your eyes, at most
#define PM_CHAT_SHOT	"zzchatshot"	// its shot, screenshots/<this>.png, read by Chat then deleted
#define PM_FRAME_SQUARE	4			// pmFrames' 1:1

// the frames to shoot in: width over height, 0 the screen's own
static const struct { const char *name; float aspect; } pmFrames[] = {
	{ "Screen", 0.0f }, { "16:9", 16.0f / 9.0f }, { "21:9", 21.0f / 9.0f }, { "4:3", 4.0f / 3.0f }, { "1:1", 1.0f }, { "9:16", 9.0f / 16.0f },
};
#define PM_NUM_FRAMES	( (int)ARRAY_LEN( pmFrames ) )

static struct {
	qboolean	open;
	int			font, titleFont;
	sfxHandle_t	clickSound;

	// the camera
	vec3_t		org, ang;
	float		fov, roll;
	qboolean	held[MAX_KEYS];
	int			lastTime;

	// the player's own view, kept as the cgame draws it, to start from
	qboolean	haveView;
	vec3_t		viewOrg, viewAng;
	float		viewFov;
	float		lastW, lastH;			// the last scene's size, for the cursor's ray

	// depth of field: its focus (along the view), what stays sharp either side, how blurred
	qboolean	dof;
	float		focus, range, blur;

	// the look: a filter and its strength, a grade, and the extras over it
	int			filter;
	float		strength;
	float		saturation, contrast, brightness, warmth;
	float		grain, vignette, aberration, scanlines;

	int			frame;					// pmFrames
	qboolean	thirds;					// the rule of thirds' lines in the frame

	// the window
	qboolean	hidden;					// the panel, H
	int			page;					// 0 the camera, 1 the look
	float		cursorX, cursorY;
	qboolean	click, mouseHeld, looking;
	float		clickX, clickY;
	float		*drag;
	int			wheel;
	int			snap;					// 1 to PM_SNAP_FRAMES nothing of ours drawn, the shot taken on the last; then told
	char		snapName[MAX_QPATH];
	char		message[MAX_STRING_CHARS];
	int			messageTime;
	char		thirdPerson[16];		// the player's cg_thirdPerson, put back on closing
	qboolean	chat;					// Chat's camera: within reach, square, a click takes it for a message
	qboolean	chatShot;				// its shot taken: Chat told, not cancelled, on closing
	int			chatFrame;				// the frame the panel had, put back
} ph;

static const float pmPanel[4]		= { 0.0f, 0.0f, 0.08f, 0.85f };
static const float pmBorder[4]		= { 0.5f, 0.5f, 0.7f, 1.0f };
static const float pmItem[4]		= { 0.15f, 0.15f, 0.25f, 0.8f };
static const float pmItemHover[4]	= { 0.25f, 0.25f, 0.4f, 0.9f };
static const float pmItemOn[4]		= { 0.35f, 0.24f, 0.05f, 0.9f };
static const float pmOrange[4]		= { 1.0f, 0.682f, 0.0f, 1.0f };
static const float pmWhite[4]		= { 1.0f, 1.0f, 1.0f, 1.0f };
static const float pmDim[4]			= { 0.6f, 0.6f, 0.7f, 1.0f };
static const float pmBar[4]			= { 0.3f, 0.3f, 0.45f, 1.0f };
static const float pmShade[4]		= { 0.0f, 0.0f, 0.0f, 0.6f };
static const float pmLine[4]		= { 1.0f, 1.0f, 1.0f, 0.35f };

qboolean CL_PhotoMode_Active( void ) {
	return ph.open;
}

static void PM_Message( const char *text ) {
	Q_strncpyz( ph.message, text, sizeof( ph.message ) );
	ph.messageTime = cls.realtime;
}

// the look back as it comes: nothing over the picture
static void PM_ResetLook( void ) {
	ph.filter = RC_FI_NONE;
	ph.strength = 100.0f;
	ph.saturation = ph.contrast = ph.brightness = 1.0f;
	ph.warmth = ph.grain = ph.vignette = ph.aberration = ph.scanlines = 0.0f;
}

static qboolean PM_LookChanged( void ) {
	return (qboolean)( ph.filter != RC_FI_NONE || ph.saturation != 1.0f || ph.contrast != 1.0f || ph.brightness != 1.0f || ph.warmth != 0.0f
		|| ph.grain > 0.0f || ph.vignette > 0.0f || ph.aberration > 0.0f || ph.scanlines > 0.0f );
}

/*
===============================================================================

The camera

===============================================================================
*/

static void PM_FromPlayer( void ) {
	if ( ph.haveView ) {
		VectorCopy( ph.viewOrg, ph.org );
		VectorCopy( ph.viewAng, ph.ang );
		ph.fov = ph.viewFov > 1.0f ? ph.viewFov : 90.0f;
	} else {
		VectorCopy( cl.snap.ps.origin, ph.org );
		ph.org[2] += cl.snap.ps.viewheight;
		VectorCopy( cl.viewangles, ph.ang );
		ph.fov = 90.0f;
	}
	ph.ang[ROLL] = 0.0f;
	ph.roll = 0.0f;
}

static void PM_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Photo mode: join a server first\n" );
		return;
	}
	if ( ph.open )
		return;
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
	CL_ChatLog_Close();
	CL_Inventory_Close();
	CL_Inspect_Close();
	CL_Holster_Close();
	CL_Characters_Close();
	CL_SpecialMoves_Close();
	CL_SkyComposer_Close();

	ph.font = re->RegisterFont( "arialnb" );
	if ( !ph.font )
		ph.font = cls.menuFont;
	ph.titleFont = re->RegisterFont( "ergoec" );
	if ( !ph.titleFont )
		ph.titleFont = ph.font;
	ph.clickSound = S_RegisterSound( "sound/interface/button1.wav" );
	PM_FromPlayer();
	memset( ph.held, 0, sizeof( ph.held ) );
	ph.lastTime = cls.realtime;
	ph.cursorX = PM_X - 120.0f;
	ph.cursorY = SCREEN_HEIGHT * 0.5f;
	ph.click = ph.mouseHeld = ph.looking = qfalse;
	ph.drag = NULL;
	ph.snap = 0;
	ph.hidden = qfalse;
	// your own character in the picture
	Q_strncpyz( ph.thirdPerson, Cvar_VariableString( "cg_thirdPerson" ), sizeof( ph.thirdPerson ) );
	Cvar_Set( "cg_thirdPerson", "1" );
	ph.open = qtrue;
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_PHOTOMODE );
	PM_Message( "Right mouse: look; WASD: fly; click the scene to focus" );
}

static void PM_Close( void ) {
	if ( !ph.open )
		return;
	ph.open = qfalse;
	if ( ph.chat ) {
		ph.chat = qfalse;
		ph.frame = ph.chatFrame;
		if ( !ph.chatShot )
			CL_Chat_CameraDone( NULL );
	}
	ph.drag = NULL;
	ph.looking = ph.mouseHeld = qfalse;
	memset( ph.held, 0, sizeof( ph.held ) );
	Cvar_Set( "cg_thirdPerson", ph.thirdPerson[0] ? ph.thirdPerson : "0" );
	if ( re->ext.SetScreenshotCrop )
		re->ext.SetScreenshotCrop( 0.0f, 0.0f, 1.0f, 1.0f );
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_PHOTOMODE );
}

void CL_PhotoMode_Close( void ) {
	PM_Close();
}

// Chat's camera: from your eyes, within reach of them, square; a click takes the picture, handed to
// Chat (CL_Chat_CameraDone), Esc goes back without one
qboolean CL_PhotoMode_OpenForChat( void ) {
	if ( ph.open )
		PM_Close();
	PM_Open();
	if ( !ph.open )
		return qfalse;
	ph.chat = qtrue;
	ph.chatShot = qfalse;
	ph.chatFrame = ph.frame;
	ph.frame = PM_FRAME_SQUARE;
	VectorCopy( cl.snap.ps.origin, ph.org );
	ph.org[2] += cl.snap.ps.viewheight;
	VectorCopy( cl.viewangles, ph.ang );
	ph.ang[ROLL] = 0.0f;
	ph.roll = 0.0f;
	ph.fov = 80.0f;
	PM_Message( "Mouse: look; WASD, Space, C: move, within reach; click: take it; Esc: back" );
	return qtrue;
}

// flown: the keys held, within reach of the player
void CL_PhotoMode_Frame( void ) {
	vec3_t forward, right, move, player, d;
	float dt, speed, len;

	if ( !ph.open )
		return;
	if ( !( Key_GetCatcher() & KEYCATCH_PHOTOMODE ) || cls.state != CA_ACTIVE ) {
		PM_Close();
		return;
	}
	dt = Com_Clamp( 0.0f, 0.1f, ( cls.realtime - ph.lastTime ) * 0.001f );
	ph.lastTime = cls.realtime;

	AngleVectors( ph.ang, forward, right, NULL );
	VectorClear( move );
	if ( ph.held['w'] || ph.held['W'] )	VectorAdd( move, forward, move );
	if ( ph.held['s'] || ph.held['S'] )	VectorSubtract( move, forward, move );
	if ( ph.held['d'] || ph.held['D'] )	VectorAdd( move, right, move );
	if ( ph.held['a'] || ph.held['A'] )	VectorSubtract( move, right, move );
	if ( ph.held[A_SPACE] )				move[2] += 1.0f;
	if ( ph.held['c'] || ph.held['C'] )	move[2] -= 1.0f;
	speed = PM_SPEED;
	if ( ph.held[A_SHIFT] || ph.held[A_SHIFT2] )
		speed *= 3.0f;
	if ( ph.held[A_CTRL] || ph.held[A_CTRL2] )
		speed *= 0.2f;
	if ( VectorNormalize( move ) > 0.0f )
		VectorMA( ph.org, speed * dt, move, ph.org );
	if ( ph.held['q'] || ph.held['Q'] )
		ph.roll = Com_Clamp( -90.0f, 90.0f, ph.roll - 45.0f * dt );
	if ( ph.held['e'] || ph.held['E'] )
		ph.roll = Com_Clamp( -90.0f, 90.0f, ph.roll + 45.0f * dt );

	// within reach of the player: further, the server doesn't send who's there
	VectorCopy( cl.snap.ps.origin, player );
	if ( ph.chat ) {
		// Chat's: within an arm's reach of your eyes, and never through a wall
		static const vec3_t mins = { -4, -4, -4 }, maxs = { 4, 4, 4 };
		trace_t tr;

		player[2] += cl.snap.ps.viewheight;
		VectorSubtract( ph.org, player, d );
		len = VectorLength( d );
		if ( len > PM_CHAT_REACH )
			VectorMA( player, PM_CHAT_REACH / len, d, ph.org );
		CM_BoxTrace( &tr, player, ph.org, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_PLAYERCLIP, qfalse );
		VectorCopy( tr.endpos, ph.org );
		return;
	}
	VectorSubtract( ph.org, player, d );
	len = VectorLength( d );
	if ( len > PM_MAX_DIST )
		VectorMA( player, PM_MAX_DIST / len, d, ph.org );
}

static void PM_View( vec3_t angles ) {
	VectorCopy( ph.ang, angles );
	angles[ROLL] = ph.roll;
}

static float PM_FovY( float fovX, float w, float h ) {
	return RAD2DEG( 2.0f * atanf( tanf( DEG2RAD( fovX * 0.5f ) ) * h / w ) );
}

// the camera's view in place of the player's; the HUD's 3D bits left out. qtrue if drawn here
qboolean CL_PhotoMode_RenderScene( const refdef_t *fd ) {
	refdef_t view;
	qboolean dof = qfalse, look = qfalse;
	vec3_t angles;

	if ( fd->rdflags & RDF_AUTOMAP )
		return qfalse;
	if ( !ph.open ) {
		if ( !( fd->rdflags & ( RDF_NOWORLDMODEL | RDF_SKYBOXPORTAL ) ) ) {
			ph.haveView = qtrue;
			VectorCopy( fd->vieworg, ph.viewOrg );
			VectorCopy( fd->viewangles, ph.viewAng );
			ph.viewFov = fd->fov_x;
		}
		return qfalse;
	}
	if ( fd->rdflags & RDF_NOWORLDMODEL )
		return qtrue;

	view = *fd;
	PM_View( angles );
	view.fov_x = ph.fov;
	view.fov_y = PM_FovY( ph.fov, (float)fd->width, (float)fd->height );
	VectorCopy( angles, view.viewangles );
	AnglesToAxis( angles, view.viewaxis );
	ph.lastW = (float)fd->width;
	ph.lastH = (float)fd->height;
	// the sky portal keeps its own origin but looks the way the camera does
	if ( !( fd->rdflags & RDF_SKYBOXPORTAL ) ) {
		VectorCopy( ph.org, view.vieworg );
		view.viewContents = CM_PointContents( ph.org, 0 );
		// the snapshot's area mask is the player's, the camera can be elsewhere
		memset( view.areamask, 0, sizeof( view.areamask ) );
		if ( ph.dof && !ph.chat && re->ext.SetDepthOfField ) {
			re->ext.SetDepthOfField( ph.focus, ph.range, ph.blur );
			dof = qtrue;
		}
		if ( PM_LookChanged() && !ph.chat && re->ext.SetScreenFilter ) {
			screenFilter_t f;

			RC_PhotoFilter( &f, ph.filter, ph.filter != RC_FI_NONE ? ph.strength / 100.0f : 1.0f, ph.saturation, ph.contrast, ph.brightness, ph.warmth );
			f.grain = Q_max( f.grain, ph.grain );
			f.vignette = Q_max( f.vignette, ph.vignette );
			f.aberration = Q_max( f.aberration, ph.aberration );
			if ( ph.scanlines > f.scanlines ) {
				f.scanlines = ph.scanlines;
				if ( f.scanlineCount <= 0.0f )
					f.scanlineCount = 240.0f;
			}
			re->ext.SetScreenFilter( &f );
			look = qtrue;
		}
	}
	re->RenderScene( &view );
	if ( dof )
		RC_NoDepthOfField();
	if ( look )
		RC_NoFilter();
	return qtrue;
}

// the camera, for the effects to be culled against
qboolean CL_PhotoMode_Camera( vec3_t origin, vec3_t angles ) {
	if ( !ph.open )
		return qfalse;
	VectorCopy( ph.org, origin );
	PM_View( angles );
	return qtrue;
}

// the cgame's 2D (the HUD, chat, the crosshair) hidden
qboolean CL_PhotoMode_Hide2D( void ) {
	return ph.open;
}

// where the cursor points into the scene
static void PM_CursorRay( vec3_t dir ) {
	vec3_t angles, axis[3];
	const float w = ph.lastW > 0.0f ? ph.lastW : (float)cls.glconfig.vidWidth, h = ph.lastH > 0.0f ? ph.lastH : (float)cls.glconfig.vidHeight;
	const float tx = tanf( DEG2RAD( ph.fov * 0.5f ) ), ty = tanf( DEG2RAD( PM_FovY( ph.fov, w, h ) * 0.5f ) );
	const float nx = ph.cursorX / SCREEN_WIDTH * 2.0f - 1.0f, ny = ph.cursorY / SCREEN_HEIGHT * 2.0f - 1.0f;

	PM_View( angles );
	AnglesToAxis( angles, axis );
	VectorCopy( axis[0], dir );
	VectorMA( dir, -nx * tx, axis[1], dir );
	VectorMA( dir, -ny * ty, axis[2], dir );
	VectorNormalize( dir );
}

// where a ray meets a box: how far along, -1 not
static float PM_RayBox( const vec3_t org, const vec3_t dir, const vec3_t mins, const vec3_t maxs ) {
	float tmin = 0.0f, tmax = 1e9f;

	for ( int i = 0; i < 3; i++ ) {
		if ( fabsf( dir[i] ) < 1e-6f ) {
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

// a click on the scene: focused on what's there, a person before the wall behind them
static void PM_FocusAt( void ) {
	vec3_t dir, end, forward, mins, maxs, angles;
	float best;
	trace_t tr;

	PM_CursorRay( dir );
	VectorMA( ph.org, 16384.0f, dir, end );
	CM_BoxTrace( &tr, ph.org, end, vec3_origin, vec3_origin, 0, MASK_SOLID, qfalse );
	best = tr.fraction * 16384.0f;
	// the people and NPCs sent: the player's box round each
	for ( int i = -1; i < cl.snap.numEntities; i++ ) {
		const float *o;
		float t;

		if ( i < 0 ) {
			o = cl.snap.ps.origin;
		} else {
			const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];

			if ( es->eType != ET_PLAYER && es->eType != ET_NPC )
				continue;
			o = es->pos.trBase;
		}
		VectorSet( mins, o[0] - 16.0f, o[1] - 16.0f, o[2] - 24.0f );
		VectorSet( maxs, o[0] + 16.0f, o[1] + 16.0f, o[2] + 40.0f );
		t = PM_RayBox( ph.org, dir, mins, maxs );
		if ( t > 0.0f && t < best )
			best = t;
	}
	if ( best >= 16000.0f ) {
		PM_Message( "Nothing there to focus on" );
		return;
	}
	// the focus is along the view, as depth of field measures it
	PM_View( angles );
	AngleVectors( angles, forward, NULL, NULL );
	ph.focus = Com_Clamp( 8.0f, 8192.0f, best * DotProduct( dir, forward ) );
	ph.dof = qtrue;
	if ( ph.range <= 0.0f )
		ph.range = RC_DOF_RANGE;
	if ( ph.blur <= 0.0f )
		ph.blur = RC_DOF_BLUR;
	PM_Message( va( "Focused %i units away", (int)ph.focus ) );
}

/*
===============================================================================

The window

===============================================================================
*/

static void PM_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void PM_Box( float x, float y, float w, float h, const float *fill ) {
	PM_Fill( x, y, w, h, fill );
	PM_Fill( x, y, w, 1, pmBorder );
	PM_Fill( x, y + h - 1, w, 1, pmBorder );
	PM_Fill( x, y, 1, h, pmBorder );
	PM_Fill( x + w - 1, y, 1, h, pmBorder );
}

static void PM_Text( float x, float y, float w, const char *text, const float *color, qboolean centred ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 3 && re->Font_StrLenPixels( buf, ph.font, PM_SCALE ) > w ) {
		len--;
		buf[len - 3] = buf[len - 2] = buf[len - 1] = '.';
		buf[len] = '\0';
	}
	if ( centred )
		x += ( w - re->Font_StrLenPixels( buf, ph.font, PM_SCALE ) ) * 0.5f;
	re->Font_DrawString( (int)x, (int)y, buf, color, ph.font | STYLE_DROPSHADOW, -1, PM_SCALE );
}

static qboolean PM_InRect( float x, float y, float w, float h ) {
	return (qboolean)( ph.cursorX >= x && ph.cursorX < x + w && ph.cursorY >= y && ph.cursorY < y + h );
}

static qboolean PM_Clicked( float x, float y, float w, float h ) {
	if ( !ph.click || ph.clickX < x || ph.clickX >= x + w || ph.clickY < y || ph.clickY >= y + h )
		return qfalse;
	ph.click = qfalse;
	if ( ph.clickSound )
		S_StartLocalSound( ph.clickSound, CHAN_LOCAL_SOUND );
	return qtrue;
}

static qboolean PM_Button( float x, float y, float w, const char *label, qboolean on, qboolean enabled ) {
	PM_Box( x, y, w, PM_CTRL_H, on ? pmItemOn : enabled && PM_InRect( x, y, w, PM_CTRL_H ) ? pmItemHover : pmItem );
	PM_Text( x, y + 1, w, label, enabled ? pmWhite : pmDim, qtrue );
	return enabled ? PM_Clicked( x, y, w, PM_CTRL_H ) : qfalse;
}

// a value dragged along a bar, or stepped with the wheel over it
static void PM_Slider( float y, const char *label, float *value, float min, float max, float step, const char *fmt, qboolean enabled ) {
	const float labelW = 64.0f, valueW = 40.0f, bx = PM_IN + labelW, bw = PM_IW - labelW - valueW;
	const float old = *value;
	float frac;

	PM_Text( PM_IN, y + 1, labelW - 4, label, enabled ? pmDim : pmBar, qfalse );
	if ( enabled && PM_Clicked( bx - 3, y, bw + 6, PM_CTRL_H ) )
		ph.drag = value;
	if ( ph.drag == value ) {
		if ( ph.mouseHeld && enabled )
			*value = min + ( max - min ) * Com_Clamp( 0.0f, 1.0f, ( ph.cursorX - bx ) / bw );
		else
			ph.drag = NULL;
	}
	if ( enabled && ph.wheel && PM_InRect( bx - 3, y, bw + 6, PM_CTRL_H ) ) {
		*value -= ph.wheel * step;
		ph.wheel = 0;
	}
	if ( *value != old )
		*value = Com_Clamp( min, max, step * floorf( *value / step + 0.5f ) );
	frac = Com_Clamp( 0.0f, 1.0f, ( *value - min ) / ( max - min ) );
	PM_Box( bx, y + 5, bw, 5, pmBar );
	PM_Fill( bx + ( bw - 4 ) * frac, y + 1, 4, PM_CTRL_H - 2, enabled ? pmWhite : pmDim );
	PM_Text( bx + bw + 4, y + 1, valueW - 4, va( fmt, *value ), enabled ? pmWhite : pmDim, qfalse );
}

static void PM_Heading( float y, const char *text ) {
	PM_Text( PM_IN, y, PM_IW, text, pmOrange, qfalse );
}

// the frame shot in, in the 640x480 screen, from the window's own shape
static void PM_FrameRect( float *x0, float *y0, float *x1, float *y1 ) {
	const float aspect = pmFrames[Com_Clampi( 0, PM_NUM_FRAMES - 1, ph.frame )].aspect;
	const float screen = cls.glconfig.vidHeight > 0 ? (float)cls.glconfig.vidWidth / cls.glconfig.vidHeight : 4.0f / 3.0f;

	*x0 = *y0 = 0.0f;
	*x1 = 1.0f;
	*y1 = 1.0f;
	if ( aspect <= 0.0f )
		return;
	if ( aspect > screen ) {
		const float h = screen / aspect;

		*y0 = ( 1.0f - h ) * 0.5f;
		*y1 = *y0 + h;
	} else {
		const float w = aspect / screen;

		*x0 = ( 1.0f - w ) * 0.5f;
		*x1 = *x0 + w;
	}
}

// outside the frame darkened, the thirds' lines in it
static void PM_DrawFrame( void ) {
	float x0, y0, x1, y1;

	PM_FrameRect( &x0, &y0, &x1, &y1 );
	x0 *= SCREEN_WIDTH;
	x1 *= SCREEN_WIDTH;
	y0 *= SCREEN_HEIGHT;
	y1 *= SCREEN_HEIGHT;
	if ( y0 > 0.0f ) {
		PM_Fill( 0, 0, SCREEN_WIDTH, y0, pmShade );
		PM_Fill( 0, y1, SCREEN_WIDTH, SCREEN_HEIGHT - y1, pmShade );
	}
	if ( x0 > 0.0f ) {
		PM_Fill( 0, 0, x0, SCREEN_HEIGHT, pmShade );
		PM_Fill( x1, 0, SCREEN_WIDTH - x1, SCREEN_HEIGHT, pmShade );
	}
	if ( ph.thirds ) {
		for ( int i = 1; i < 3; i++ ) {
			PM_Fill( x0 + ( x1 - x0 ) * i / 3.0f, y0, 1, y1 - y0, pmLine );
			PM_Fill( x0, y0 + ( y1 - y0 ) * i / 3.0f, x1 - x0, 1, pmLine );
		}
	}
}

// the picture saved: next frame, with nothing of ours over it, cut to the frame
static void PM_Snap( void ) {
	qtime_t t;

	if ( ph.chat ) {
		Q_strncpyz( ph.snapName, PM_CHAT_SHOT, sizeof( ph.snapName ) );
		FS_HomeRemove( "screenshots/" PM_CHAT_SHOT ".png" );
		ph.snap = 1;
		return;
	}
	Com_RealTime( &t );
	Com_sprintf( ph.snapName, sizeof( ph.snapName ), "photo_%04i%02i%02i_%02i%02i%02i", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
		t.tm_hour, t.tm_min, t.tm_sec );
	// another in the same second: numbered
	for ( int n = 2; n < 100 && FS_FileExists( va( "screenshots/%s.png", ph.snapName ) ); n++ ) {
		char base[MAX_QPATH];

		Com_sprintf( base, sizeof( base ), "photo_%04i%02i%02i_%02i%02i%02i_%i", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
			t.tm_hour, t.tm_min, t.tm_sec, n );
		Q_strncpyz( ph.snapName, base, sizeof( ph.snapName ) );
	}
	ph.snap = 1;
}

static void PM_TakeSnap( void ) {
	float x0, y0, x1, y1;

	PM_FrameRect( &x0, &y0, &x1, &y1 );
	if ( re->ext.SetScreenshotCrop )
		re->ext.SetScreenshotCrop( x0, y0, x1, y1 );
	Cbuf_ExecuteText( EXEC_NOW, va( "screenshot_png %s\n", ph.snapName ) );
	ph.snap = PM_SNAP_FRAMES + 1;
}

static float PM_DrawCameraPage( float y ) {
	const float half = ( PM_IW - 4 ) * 0.5f, sixth = ( PM_IW - 10 ) / 6.0f;
	const qboolean canDof = (qboolean)( re->ext.SetDepthOfField != NULL );

	PM_Heading( y, "Camera" );
	y += PM_ROW;
	PM_Slider( y, "Zoom (FOV)", &ph.fov, 10.0f, 130.0f, 1.0f, "%.0f", qtrue );
	y += PM_ROW;
	PM_Slider( y, "Roll (Q, E)", &ph.roll, -90.0f, 90.0f, 1.0f, "%.0f", qtrue );
	y += PM_ROW;
	if ( PM_Button( PM_IN, y, PM_IW, "Back to my view", qfalse, qtrue ) )
		PM_FromPlayer();
	y += PM_ROW + 6;

	PM_Heading( y, "Depth of field" );
	y += PM_ROW;
	if ( !canDof ) {
		PM_Text( PM_IN, y, PM_IW, "With Vulkan only", pmDim, qfalse );
		y += PM_ROW + 6;
	} else {
		if ( PM_Button( PM_IN, y, PM_IW, ph.dof ? "On: click the scene to focus" : "Off: click the scene to focus", ph.dof, qtrue ) ) {
			ph.dof = (qboolean)!ph.dof;
			if ( ph.range <= 0.0f )
				ph.range = RC_DOF_RANGE;
			if ( ph.blur <= 0.0f )
				ph.blur = RC_DOF_BLUR;
			if ( ph.focus <= 0.0f )
				ph.focus = 256.0f;
		}
		y += PM_ROW;
		PM_Slider( y, "Focus", &ph.focus, 8.0f, 4096.0f, 8.0f, "%.0f", ph.dof );
		y += PM_ROW;
		PM_Slider( y, "Sharp +-", &ph.range, 0.0f, 1024.0f, 8.0f, "%.0f", ph.dof );
		y += PM_ROW;
		PM_Slider( y, "Blur", &ph.blur, 0.5f, RC_DOF_MAX_BLUR, 0.5f, "%.1f", ph.dof );
		y += PM_ROW + 6;
	}

	PM_Heading( y, "Frame" );
	y += PM_ROW;
	for ( int i = 0; i < PM_NUM_FRAMES; i++ ) {
		if ( PM_Button( PM_IN + i * ( sixth + 2 ), y, sixth, pmFrames[i].name, (qboolean)( ph.frame == i ), qtrue ) )
			ph.frame = i;
	}
	y += PM_ROW;
	if ( PM_Button( PM_IN, y, half, "Thirds' lines", ph.thirds, qtrue ) )
		ph.thirds = (qboolean)!ph.thirds;
	return y + PM_ROW;
}

static float PM_DrawLookPage( float y ) {
	const float third = ( PM_IW - 8 ) / 3.0f;
	const qboolean can = (qboolean)( re->ext.SetScreenFilter != NULL );

	if ( !can ) {
		PM_Text( PM_IN, y, PM_IW, "Filters and grading: with Vulkan only", pmDim, qfalse );
		return y + PM_ROW;
	}
	PM_Heading( y, "Filter" );
	y += PM_ROW;
	if ( PM_Button( PM_IN, y, 20, "<", qfalse, qtrue ) )
		ph.filter = ( ph.filter + RC_NUM_FILTERS - 1 ) % RC_NUM_FILTERS;
	PM_Box( PM_IN + 24, y, PM_IW - 48, PM_CTRL_H, pmItem );
	PM_Text( PM_IN + 24, y + 1, PM_IW - 48, RC_FilterName( ph.filter ), pmWhite, qtrue );
	if ( PM_Button( PM_IN + PM_IW - 20, y, 20, ">", qfalse, qtrue ) )
		ph.filter = ( ph.filter + 1 ) % RC_NUM_FILTERS;
	y += PM_ROW;
	PM_Slider( y, "Strength", &ph.strength, 0.0f, 100.0f, 5.0f, "%.0f%%", (qboolean)( ph.filter != RC_FI_NONE ) );
	y += PM_ROW + 6;

	PM_Heading( y, "Colour" );
	y += PM_ROW;
	PM_Slider( y, "Saturation", &ph.saturation, 0.0f, 2.0f, 0.05f, "%.2f", qtrue );
	y += PM_ROW;
	PM_Slider( y, "Contrast", &ph.contrast, 0.5f, 1.8f, 0.05f, "%.2f", qtrue );
	y += PM_ROW;
	PM_Slider( y, "Brightness", &ph.brightness, 0.4f, 1.8f, 0.05f, "%.2f", qtrue );
	y += PM_ROW;
	PM_Slider( y, "Warmth", &ph.warmth, -1.0f, 1.0f, 0.05f, "%.2f", qtrue );
	y += PM_ROW + 6;

	PM_Heading( y, "Lens" );
	y += PM_ROW;
	PM_Slider( y, "Grain", &ph.grain, 0.0f, 0.3f, 0.01f, "%.2f", qtrue );
	y += PM_ROW;
	PM_Slider( y, "Vignette", &ph.vignette, 0.0f, 1.0f, 0.05f, "%.2f", qtrue );
	y += PM_ROW;
	PM_Slider( y, "Fringes", &ph.aberration, 0.0f, 4.0f, 0.1f, "%.1f", qtrue );
	y += PM_ROW;
	PM_Slider( y, "Scanlines", &ph.scanlines, 0.0f, 0.5f, 0.02f, "%.2f", qtrue );
	y += PM_ROW + 4;
	if ( PM_Button( PM_IN + third * 2 + 8, y, third, "Reset", qfalse, PM_LookChanged() ) )
		PM_ResetLook();
	return y + PM_ROW;
}

void CL_PhotoMode_Draw( void ) {
	const float half = ( PM_IW - 4 ) * 0.5f;
	float y;

	if ( !ph.open )
		return;
	// the shot: this frame drawn without any of ours
	// a few frames without them: OpenGL reads the picture at once, from a buffer a frame or two old;
	// Vulkan at the frame's end. The shot's taken on the last
	if ( ph.snap >= 1 && ph.snap < PM_SNAP_FRAMES ) {
		ph.snap++;
		return;
	}
	if ( ph.snap == PM_SNAP_FRAMES ) {
		PM_TakeSnap();
		return;
	}
	if ( ph.snap > PM_SNAP_FRAMES ) {
		ph.snap = 0;
		// Chat's: handed to it (the file may be written at this frame's end), the camera closed
		if ( ph.chat ) {
			ph.chatShot = qtrue;
			CL_Chat_CameraDone( "screenshots/" PM_CHAT_SHOT ".png" );
			PM_Close();
			return;
		}
		PM_Message( va( "Saved screenshots/%s.png", ph.snapName ) );
		Com_Printf( "Photo mode: saved screenshots/%s.png\n", ph.snapName );
	}
	PM_DrawFrame();
	// Chat's camera: the frame, and what to do, no panel nor cursor
	if ( ph.chat ) {
		float x0, y0, x1, y1;

		PM_FrameRect( &x0, &y0, &x1, &y1 );
		PM_Fill( x0 * SCREEN_WIDTH, y0 * SCREEN_HEIGHT, ( x1 - x0 ) * SCREEN_WIDTH, 1, pmLine );
		PM_Fill( x0 * SCREEN_WIDTH, y1 * SCREEN_HEIGHT - 1, ( x1 - x0 ) * SCREEN_WIDTH, 1, pmLine );
		PM_Fill( x0 * SCREEN_WIDTH, y0 * SCREEN_HEIGHT, 1, ( y1 - y0 ) * SCREEN_HEIGHT, pmLine );
		PM_Fill( x1 * SCREEN_WIDTH - 1, y0 * SCREEN_HEIGHT, 1, ( y1 - y0 ) * SCREEN_HEIGHT, pmLine );
		PM_Text( x0 * SCREEN_WIDTH, y1 * SCREEN_HEIGHT - 18, ( x1 - x0 ) * SCREEN_WIDTH,
			"Click: take the picture   Mouse: look   WASD: move   Esc: back", pmWhite, qtrue );
		ph.click = qfalse;
		ph.wheel = 0;
		return;
	}
	if ( !ph.hidden ) {
		PM_Box( PM_X, PM_Y, PM_W, PM_H, pmPanel );
		re->Font_DrawString( (int)PM_IN, (int)( PM_Y + 6 ), "PHOTO MODE", pmOrange, ph.titleFont | STYLE_DROPSHADOW, -1, PM_SCALE + 0.15f );
		if ( PM_Button( PM_X + PM_W - 10 - 48, PM_Y + 7, 48, "Close", qfalse, qtrue ) ) {
			PM_Close();
			return;
		}
		y = PM_Y + 28;
		if ( PM_Button( PM_IN, y, half, "Camera", (qboolean)( ph.page == 0 ), qtrue ) )
			ph.page = 0;
		if ( PM_Button( PM_IN + half + 4, y, half, "Look", (qboolean)( ph.page == 1 ), qtrue ) )
			ph.page = 1;
		y += PM_ROW + 6;
		if ( ph.page == 0 )
			PM_DrawCameraPage( y );
		else
			PM_DrawLookPage( y );

		y = PM_Y + PM_H - 40;
		if ( PM_Button( PM_IN, y, PM_IW, "Snap (Enter)", qfalse, (qboolean)( ph.snap == 0 ) ) )
			PM_Snap();
		y += PM_ROW + 2;
		if ( ph.message[0] && cls.realtime - ph.messageTime < PM_MESSAGE_MS )
			PM_Text( PM_IN, y, PM_IW, ph.message, pmWhite, qfalse );
		else
			PM_Text( PM_IN, y, PM_IW, "H hides this; right mouse looks, WASD flies", pmDim, qfalse );
	}

	// a click left over is on the scene: focus there
	if ( ph.click && !( !ph.hidden && PM_InRect( PM_X, PM_Y, PM_W, PM_H ) ) && ph.clickX < SCREEN_WIDTH ) {
		if ( re->ext.SetDepthOfField )
			PM_FocusAt();
	}
	ph.click = qfalse;
	ph.wheel = 0;
	if ( !ph.looking ) {
		re->SetColor( NULL );
		re->DrawStretchPic( ph.cursorX, ph.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
	}
}

void CL_PhotoMode_KeyEvent( int key, qboolean down ) {
	if ( key >= 0 && key < MAX_KEYS )
		ph.held[key] = down;
	if ( !ph.open )
		return;
	// Chat's camera: a click or Enter takes it
	if ( ph.chat ) {
		if ( down && ( key == A_MOUSE1 || key == A_ENTER || key == A_KP_ENTER ) && ph.snap == 0 )
			PM_Snap();
		return;
	}
	if ( key == A_MOUSE1 ) {
		ph.mouseHeld = down;
		if ( down ) {
			ph.click = qtrue;
			ph.clickX = ph.cursorX;
			ph.clickY = ph.cursorY;
		}
		return;
	}
	if ( key == A_MOUSE2 ) {
		ph.looking = down;
		return;
	}
	if ( !down )
		return;
	if ( key == A_MWHEELUP || key == A_MWHEELDOWN ) {
		const int step = key == A_MWHEELUP ? -1 : 1;

		// over the panel it moves what's under it; on the scene it zooms
		if ( !ph.hidden && PM_InRect( PM_X, PM_Y, PM_W, PM_H ) )
			ph.wheel += step;
		else
			ph.fov = Com_Clamp( 10.0f, 130.0f, ph.fov + step * 3.0f );
		return;
	}
	if ( key == 'h' || key == 'H' ) {
		ph.hidden = (qboolean)!ph.hidden;
		return;
	}
	if ( ( key == A_ENTER || key == A_KP_ENTER ) && ph.snap == 0 ) {
		PM_Snap();
		return;
	}
	{
		const char *binding = Key_GetBinding( key );

		if ( VALIDSTRING( binding ) && !Q_stricmp( binding, PM_CMD ) )
			PM_Close();
	}
}

// the cursor, or with the right button held the camera, turned as the mouse would
void CL_PhotoMode_MouseEvent( int dx, int dy ) {
	if ( ph.looking || ph.chat ) {
		const float sens = Cvar_VariableValue( "sensitivity" ) * ( ph.fov / 90.0f );

		ph.ang[YAW] -= Cvar_VariableValue( "m_yaw" ) * dx * sens;
		ph.ang[PITCH] = Com_Clamp( -89.0f, 89.0f, ph.ang[PITCH] + Cvar_VariableValue( "m_pitch" ) * dy * sens );
		return;
	}
	ph.cursorX = Com_Clamp( 0, SCREEN_WIDTH, ph.cursorX + dx );
	ph.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, ph.cursorY + dy );
}

void CL_PhotoMode_Escape( void ) {
	PM_Close();
}

static void PM_Cmd_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !Q_stricmp( arg, "snap" ) ) {
		if ( ph.open && ph.snap == 0 )
			PM_Snap();
		return;
	}
	if ( !Q_stricmp( arg, "frame" ) ) {
		for ( int i = 0; i < PM_NUM_FRAMES; i++ ) {
			if ( !Q_stricmp( Cmd_Argv( 2 ), pmFrames[i].name ) )
				ph.frame = i;
		}
		return;
	}
	// a filter by its name as the panel shows it (Noir, Sepia, Old film...), quoted when it has spaces
	if ( !Q_stricmp( arg, "filter" ) ) {
		for ( int i = 0; i < RC_NUM_FILTERS; i++ ) {
			if ( !Q_stricmp( Cmd_ArgsFrom( 2 ), RC_FilterName( i ) ) )
				ph.filter = i;
		}
		return;
	}
	// depth of field focused this far along the view; 0 off
	if ( !Q_stricmp( arg, "focus" ) ) {
		const float units = (float)atof( Cmd_Argv( 2 ) );

		ph.dof = (qboolean)( units > 0.0f );
		if ( ph.dof )
			ph.focus = Com_Clamp( 8.0f, 8192.0f, units );
		return;
	}
	if ( !Q_stricmp( arg, "off" ) || ( !arg[0] && ph.open ) )
		PM_Close();
	else
		PM_Open();
}

void CL_PhotoMode_Init( void ) {
	memset( &ph, 0, sizeof( ph ) );
	PM_ResetLook();
	ph.focus = 256.0f;
	ph.range = RC_DOF_RANGE;
	ph.blur = RC_DOF_BLUR;
	Cmd_AddCommand( PM_CMD, PM_Cmd_f, "Photo mode: a free camera to take pictures, the HUD hidden, with depth of field, filters and a frame (photomode snap takes one)" );
}

void CL_PhotoMode_Shutdown( void ) {
	PM_Close();
	Cmd_RemoveCommand( PM_CMD );
}
