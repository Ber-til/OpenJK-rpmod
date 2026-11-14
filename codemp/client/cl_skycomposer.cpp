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

// cl_skycomposer.cpp -- the sky composer: what's in the sky, for the Vulkan renderer to draw
// (rd-vulkan's vk_rpmod_post.cpp, r_skyScene): the sky itself (the map's classic skybox, or one
// composed from the hour: the sun's place, dusk, night and its stars), the clouds, lightning in
// them, and objects: planets, gas giants, moons, suns, a battle station, ringed worlds, nebulae, a
// fleet, and a model of your choice (a ship, a construct: drawn far out that way, behind the map's own
// things). Each object goes where you look as you add it.
//
// A window, like the other tools (/skycomposer), and its words for binds (/skycomposer <word>).
// What you compose shows on your screen; Share sends it to everyone on the server with the client
// (an RPMod admin's, through C1B3R, which keeps it for who joins later), as cmd:skyc: <scene>, and
// Unshare (cmd:skyc/off) takes it back. A shared sky shows in place of your own, but while the
// window is open, where yours shows as you make it.

#include "client.h"
#include "cl_presence.h"
#include "cl_widgets.h"
#include <string>
#include <vector>

#define SC_CMD			"skycomposer"
#define SC_MAX			16

typedef struct {
	char	kind[16];
	float	yaw, pitch, size;
	float	color[3];
	float	seed, tilt, rings;		// a model's: unused, its heading, its turning in degrees a second
	char	model[MAX_QPATH];		// a model's path
} scObject_t;

static const struct {
	const char	*kind;
	const char	*title;
	float		size;			// its radius in degrees, as made
	float		color[3];
	float		rings;
	const char	*what;
} scKinds[] = {
	{ "planet",		"Planet",		8.0f,	{ 0.45f, 0.6f, 0.4f },	0.0f,	"land and seas, a thin glow of air" },
	{ "gas",		"Gas Giant",	12.0f,	{ 0.9f, 0.7f, 0.5f },	0.0f,	"a gas giant, banded" },
	{ "ringed",		"Ringed",		10.0f,	{ 0.85f, 0.75f, 0.55f },	1.0f,	"a gas giant with rings" },
	{ "moon",		"Moon",			4.0f,	{ 0.75f, 0.75f, 0.72f },	0.0f,	"grey and cratered" },
	{ "sun",		"Sun",			2.0f,	{ 1.0f, 0.85f, 0.6f },	0.0f,	"a second sun, glowing" },
	{ "station",	"Station",		5.0f,	{ 0.6f, 0.62f, 0.65f },	0.0f,	"a battle station: plates, a trench, a dish" },
	{ "nebula",		"Nebula",		30.0f,	{ 0.5f, 0.2f, 0.6f },	0.0f,	"coloured gas across the stars" },
	{ "fleet",		"Fleet",		6.0f,	{ 0.55f, 0.57f, 0.6f },	0.0f,	"a few ships, engines lit (rings: more ships)" },
	{ "model",		"Model",		6.0f,	{ 1.0f, 1.0f, 1.0f },	0.0f,	"a model of your choice (.md3): a ship, a construct" },
};

// the models of the sky shown now (yours or the server's), for the scene
typedef struct {
	qhandle_t	handle;
	float		yaw, pitch, size, heading, turn;
} scShown_t;
static std::vector<scShown_t> scShown;

static struct {
	qboolean	composed;		// the composer's own sky in place of the map's skybox
	float		hour;			// -1 none
	float		clouds;			// -1 none
	qboolean	lightning;
	std::vector<scObject_t>	objects;

	qboolean	sharedIn;		// a scene from the server, shown in place of this one
	std::string	shared;
} sc = { qfalse, -1.0f, -1.0f };

// the window
static struct {
	qboolean	open;
	int			font, titleFont;
	sfxHandle_t	clickSound;
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;
	qboolean	held;			// the left button, for dragging a slider
	qboolean	looking;		// the right button: the view turned with the mouse
	float		*drag;			// the slider dragged
	int			wheel;
	int			pick;			// the object picked, -1 none
	int			scroll;
	int			applyAt;		// a change made while dragging: shown again this often at most
	qboolean	dirty;
	qboolean	typing;			// in the model's path
	char		path[MAX_QPATH];
	int			pathPick;
	qboolean	pathBad;		// the picked model's path has no model		// the object the path is of
	float		hourV, cloudsV;	// the hour's and the clouds' sliders: each its own, a drag follows one
	char		message[MAX_STRING_CHARS];
	int			messageTime;
} sw;

static const float swPanel[4]		= { 0.0f, 0.0f, 0.08f, 0.88f };
static const float swBorder[4]		= { 0.5f, 0.5f, 0.7f, 1.0f };
static const float swItem[4]		= { 0.15f, 0.15f, 0.25f, 0.8f };
static const float swItemHover[4]	= { 0.25f, 0.25f, 0.4f, 0.9f };
static const float swItemOn[4]		= { 0.35f, 0.24f, 0.05f, 0.9f };
static const float swOrange[4]		= { 1.0f, 0.682f, 0.0f, 1.0f };
static const float swWhite[4]		= { 1.0f, 1.0f, 1.0f, 1.0f };
static const float swDim[4]			= { 0.6f, 0.6f, 0.7f, 1.0f };
static const float swYellow[4]		= { 1.0f, 0.85f, 0.3f, 1.0f };
static const float swBar[4]			= { 0.3f, 0.3f, 0.45f, 1.0f };

static std::string SC_Scene( void ) {
	std::string out;

	out += sc.composed ? "sky composed " : "sky classic ";
	if ( sc.hour >= 0.0f )
		out += va( "time %g ", sc.hour );
	if ( sc.clouds >= 0.0f )
		out += va( "clouds %g ", sc.clouds );
	if ( sc.lightning )
		out += "lightning 1 ";
	for ( size_t i = 0; i < sc.objects.size(); i++ ) {
		const scObject_t *o = &sc.objects[i];

		if ( !Q_stricmp( o->kind, "model" ) )
			out += va( "; model \"%s\" ", o->model );
		else
			out += va( "; %s ", o->kind );
		out += va( "%.0f %.0f %g %.2g %.2g %.2g %.0f %.0f %.0f ", o->yaw, o->pitch, o->size,
			o->color[0], o->color[1], o->color[2], o->seed, o->tilt, o->rings );
	}
	return out;
}

static void SC_Parse( const char *text );
static void SC_ShowModels( const std::string &scene );

// what the renderer draws: the server's scene (but yours while the window's open), else yours, in
// r_skyScene to r_skyScene4 (a setting holds 256 characters), cut between objects
static void SC_Apply( void ) {
	const std::string scene = ( sc.sharedIn && !sw.open ) ? sc.shared : SC_Scene();
	size_t at = 0;

	for ( int i = 0; i < 4; i++ ) {
		size_t len = scene.size() - Q_min( at, scene.size() );

		if ( len > 240 ) {
			const size_t cut = scene.rfind( ';', at + 240 );

			len = ( cut != std::string::npos && cut > at ) ? cut - at : 240;
		}
		Cvar_Set( i ? va( "r_skyScene%i", i + 1 ) : "r_skyScene", scene.substr( Q_min( at, scene.size() ), len ).c_str() );
		at += len;
	}
	SC_ShowModels( scene );
	sw.dirty = qfalse;
	sw.applyAt = cls.realtime;
}

static int SC_Kind( const char *word ) {
	for ( int i = 0; i < (int)ARRAY_LEN( scKinds ); i++ ) {
		if ( !Q_stricmp( word, scKinds[i].kind ) )
			return i;
	}
	return -1;
}

// a scene's text into the composer (the server's, to change it): as the renderer reads it
static void SC_Parse( const char *text ) {
	const char *p = text;

	sc.composed = sc.lightning = qfalse;
	sc.hour = sc.clouds = -1.0f;
	sc.objects.clear();
	while ( p ) {
		const char *token = COM_ParseExt( &p, qtrue );
		int k;

		if ( !token[0] )
			break;
		if ( !Q_stricmp( token, "sky" ) )
			sc.composed = (qboolean)( !Q_stricmp( COM_ParseExt( &p, qfalse ), "composed" ) );
		else if ( !Q_stricmp( token, "time" ) )
			sc.hour = Com_Clamp( 0.0f, 24.0f, atof( COM_ParseExt( &p, qfalse ) ) );
		else if ( !Q_stricmp( token, "clouds" ) )
			sc.clouds = Com_Clamp( 0.0f, 1.0f, atof( COM_ParseExt( &p, qfalse ) ) );
		else if ( !Q_stricmp( token, "lightning" ) )
			sc.lightning = (qboolean)( atoi( COM_ParseExt( &p, qfalse ) ) != 0 );
		else if ( ( k = SC_Kind( token ) ) >= 0 && (int)sc.objects.size() < SC_MAX ) {
			char path[MAX_QPATH] = "";
			if ( !Q_stricmp( token, "model" ) )
				Q_strncpyz( path, COM_ParseExt( &p, qfalse ), sizeof( path ) );
			float v[9] = { 0, 20, scKinds[k].size, scKinds[k].color[0], scKinds[k].color[1], scKinds[k].color[2], 0, 20, scKinds[k].rings };
			scObject_t o;

			for ( int i = 0; i < 9 && p; i++ ) {
				const char *at = p;
				const char *num = COM_ParseExt( &p, qfalse );

				if ( !num[0] || !strchr( "-0123456789.", num[0] ) ) {
					p = at;		// not a number: the next word
					break;
				}
				v[i] = atof( num );
			}
			memset( &o, 0, sizeof( o ) );
			Q_strncpyz( o.kind, scKinds[k].kind, sizeof( o.kind ) );
			o.yaw = v[0];
			o.pitch = v[1];
			o.size = v[2];
			VectorCopy( v + 3, o.color );
			o.seed = v[6];
			o.tilt = v[7];
			o.rings = v[8];
			Q_strncpyz( o.model, path, sizeof( o.model ) );
			sc.objects.push_back( o );
		}
	}
}

// the models of the scene shown, registered for the scene (the rest of it is the renderer's)
static void SC_ShowModels( const std::string &scene ) {
	const qboolean composed = sc.composed, lightning = sc.lightning;
	const float hour = sc.hour, clouds = sc.clouds;
	std::vector<scObject_t> mine;

	scShown.clear();
	if ( scene.find( "model" ) == std::string::npos || cls.state < CA_LOADING )
		return;
	mine.swap( sc.objects );
	SC_Parse( scene.c_str() );
	for ( size_t i = 0; i < sc.objects.size(); i++ ) {
		const scObject_t *o = &sc.objects[i];
		scShown_t m;

		if ( Q_stricmp( o->kind, "model" ) || !o->model[0] )
			continue;
		m.handle = re->RegisterModel( o->model );
		if ( !m.handle )
			continue;
		m.yaw = o->yaw;
		m.pitch = o->pitch;
		m.size = o->size;
		m.heading = o->tilt;
		m.turn = o->rings;
		scShown.push_back( m );
	}
	sc.objects.swap( mine );
	sc.composed = composed;
	sc.lightning = lightning;
	sc.hour = hour;
	sc.clouds = clouds;
}

// the sky's models in the main view: far out where they are, the renderer places them (Vulkan)
void CL_SkyComposer_AddToScene( const refdef_t *fd ) {
	if ( scShown.empty() || ( fd->rdflags & RDF_NOWORLDMODEL ) || Q_stricmp( Cvar_VariableString( "cl_renderer" ), "rd-vulkan" ) )
		return;
	for ( size_t i = 0; i < scShown.size(); i++ ) {
		const scShown_t *m = &scShown[i];
		refEntity_t ent;
		vec3_t angles;

		memset( &ent, 0, sizeof( ent ) );
		ent.reType = RT_MODEL;
		ent.hModel = m->handle;
		VectorSet( ent.angles, -m->pitch, m->yaw, 0.0f );
		ent.radius = DEG2RAD( m->size );
		// facing you at heading 0, turning if it's to
		VectorSet( angles, 0.0f, m->yaw + 180.0f + m->heading + m->turn * cl.serverTime * 0.001f, 0.0f );
		AnglesToAxis( angles, ent.axis );
		VectorCopy( fd->vieworg, ent.origin );
		VectorCopy( fd->vieworg, ent.oldorigin );
		VectorCopy( fd->vieworg, ent.lightingOrigin );
		ent.renderfx = RF_SKYMODEL | RF_NOSHADOW | RF_LIGHTING_ORIGIN;
		ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = ent.shaderRGBA[3] = 255;
		re->AddRefEntityToScene( &ent );
	}
}

// where the view looks now, as a sky direction (the player's angles as the server has them: the
// commands' own differ after a teleport)
static void SC_LookView( float *yaw, float *pitch ) {
	*yaw = AngleNormalize360( cl.snap.ps.viewangles[YAW] );
	*pitch = Com_Clamp( -10.0f, 89.0f, -AngleNormalize180( cl.snap.ps.viewangles[PITCH] ) );
}

// a command's: as given after the word at "from", else where the view looks
static void SC_Look( float *yaw, float *pitch, int from ) {
	if ( Cmd_Argc() > from + 1 ) {
		*yaw = AngleNormalize360( atof( Cmd_Argv( from ) ) );
		*pitch = Com_Clamp( -10.0f, 89.0f, atof( Cmd_Argv( from + 1 ) ) );
		return;
	}
	SC_LookView( yaw, pitch );
}

static int SC_Add( int k, float size, float yaw, float pitch, const char *model ) {
	scObject_t o;

	if ( (int)sc.objects.size() >= SC_MAX )
		return -1;
	memset( &o, 0, sizeof( o ) );
	Q_strncpyz( o.kind, scKinds[k].kind, sizeof( o.kind ) );
	o.yaw = yaw;
	o.pitch = pitch;
	o.size = size > 0.0f ? size : scKinds[k].size;
	VectorCopy( scKinds[k].color, o.color );
	o.seed = (float)( rand() % 100 );
	o.tilt = 20.0f;
	o.rings = scKinds[k].rings;
	if ( !Q_stricmp( scKinds[k].kind, "model" ) ) {
		o.seed = o.tilt = 0.0f;
		if ( model )
			Q_strncpyz( o.model, model, sizeof( o.model ) );
	}
	sc.objects.push_back( o );
	return (int)sc.objects.size() - 1;
}

static qboolean SC_Share( void ) {
	return CL_Presence_Hook( -1, va( "cmd:skyc: %s", SC_Scene().c_str() ) );
}

static qboolean SC_Unshare( void ) {
	return CL_Presence_Hook( -1, "cmd:skyc/off" );
}

static qboolean SC_Drawable( void ) {
	return (qboolean)( !Q_stricmp( Cvar_VariableString( "cl_renderer" ), "rd-vulkan" ) && Cvar_VariableIntegerValue( "r_fbo" ) );
}

/*
===============================================================================

The words, for the console and binds

===============================================================================
*/

static int SC_Index( const char *arg ) {
	const int n = atoi( arg ) - 1;

	if ( !arg[0] || n < 0 || n >= (int)sc.objects.size() ) {
		Com_Printf( "No object %s: /" SC_CMD " list numbers them\n", arg );
		return -1;
	}
	return n;
}

static void SC_List( void ) {
	Com_Printf( "Sky: %s, time %s, clouds %s, lightning %s%s\n", sc.composed ? "composed" : "classic (the map's)",
		sc.hour >= 0.0f ? va( "%g", sc.hour ) : "the map's", sc.clouds >= 0.0f ? va( "%g", sc.clouds ) : "yours (r_clouds)",
		sc.lightning ? "on" : "off", sc.sharedIn ? S_COLOR_YELLOW "  (the server's sky shows in place of this)" : "" );
	for ( size_t i = 0; i < sc.objects.size(); i++ ) {
		const scObject_t *o = &sc.objects[i];

		if ( !Q_stricmp( o->kind, "model" ) )
			Com_Printf( "%2i  model    yaw %3.0f  pitch %3.0f  size %g  %s  heading %.0f  turn %.0f\n", (int)i + 1, o->yaw, o->pitch, o->size,
				o->model[0] ? o->model : "(no model yet)", o->tilt, o->rings );
		else
			Com_Printf( "%2i  %-8s yaw %3.0f  pitch %3.0f  size %g  colour %.2f %.2f %.2f%s\n", (int)i + 1, o->kind, o->yaw, o->pitch, o->size,
				o->color[0], o->color[1], o->color[2], o->rings > 0.0f ? "  rings" : "" );
	}
	if ( !SC_Drawable() )
		Com_Printf( S_COLOR_YELLOW "The sky composer draws with Vulkan and its post effects (cl_renderer rd-vulkan, r_fbo 1)\n" );
}

static void SC_Usage( void ) {
	Com_Printf( "usage: " SC_CMD "                         its window\n" );
	Com_Printf( "       " SC_CMD " list                    what's in your sky\n" );
	Com_Printf( "       " SC_CMD " sky classic|composed    the map's skybox, or a sky of the hour in its place\n" );
	Com_Printf( "       " SC_CMD " time <0-24|off>         the hour: the sun's place, dusk, night and stars\n" );
	Com_Printf( "       " SC_CMD " clouds <0-1|off>        the clouds' cover, 0 clear to 1 overcast\n" );
	Com_Printf( "       " SC_CMD " lightning on|off        lightning in the clouds\n" );
	Com_Printf( "       " SC_CMD " add <kind> [size [yaw pitch]]  an object where you look (or there); kinds:\n" );
	for ( size_t i = 0; i < ARRAY_LEN( scKinds ); i++ )
		Com_Printf( "                %-8s %s\n", scKinds[i].kind, scKinds[i].what );
	Com_Printf( "       " SC_CMD " here <n> [yaw pitch]    object n moved to where you look (or there)\n" );
	Com_Printf( "       " SC_CMD " size <n> <degrees>      its size\n" );
	Com_Printf( "       " SC_CMD " color <n> <r g b>       its colour, 0 to 1 each\n" );
	Com_Printf( "       " SC_CMD " rings <n> on|off        rings round it (a fleet: more ships)\n" );
	Com_Printf( "       " SC_CMD " tilt <n> <degrees>      its rings' tilt\n" );
	Com_Printf( "       " SC_CMD " seed <n> <number>       another one of its kind\n" );
	Com_Printf( "       " SC_CMD " add model <path> [size [yaw pitch]]  a model of your choice (.md3)\n" );
	Com_Printf( "       " SC_CMD " model <n> <path>        a model object's model\n" );
	Com_Printf( "       " SC_CMD " heading <n> <degrees>   which way a model faces (0 toward you)\n" );
	Com_Printf( "       " SC_CMD " turn <n> <degrees>      how fast a model turns, a second\n" );
	Com_Printf( "       " SC_CMD " remove <n> | clear      one gone, or all of it\n" );
	Com_Printf( "       " SC_CMD " share | unshare         to everyone here (RPMod admins), or taken back\n" );
}

static void SC_Open( void );
static void SC_Close( void );

static void SC_Cmd_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !arg[0] ) {
		if ( sw.open )
			SC_Close();
		else
			SC_Open();
		return;
	}
	if ( !Q_stricmp( arg, "help" ) ) {
		SC_Usage();
		return;
	}
	if ( !Q_stricmp( arg, "list" ) ) {
		SC_List();
		return;
	}
	if ( !Q_stricmp( arg, "sky" ) ) {
		sc.composed = (qboolean)( !Q_stricmp( Cmd_Argv( 2 ), "composed" ) );
	} else if ( !Q_stricmp( arg, "time" ) || !Q_stricmp( arg, "clouds" ) ) {
		const char *v = Cmd_Argv( 2 );
		float *at = !Q_stricmp( arg, "time" ) ? &sc.hour : &sc.clouds;

		if ( !v[0] ) {
			Com_Printf( "usage: " SC_CMD " %s %s\n", arg, at == &sc.hour ? "<0-24|off>" : "<0-1|off>" );
			return;
		}
		*at = !Q_stricmp( v, "off" ) ? -1.0f : Com_Clamp( 0.0f, at == &sc.hour ? 24.0f : 1.0f, atof( v ) );
	} else if ( !Q_stricmp( arg, "lightning" ) ) {
		sc.lightning = (qboolean)( !Q_stricmp( Cmd_Argv( 2 ), "on" ) || atoi( Cmd_Argv( 2 ) ) != 0 );
	} else if ( !Q_stricmp( arg, "add" ) ) {
		const int k = SC_Kind( Cmd_Argv( 2 ) );
		const int a = k >= 0 && !Q_stricmp( scKinds[k].kind, "model" ) ? 4 : 3;	// a model's path first
		float yaw, pitch;

		if ( k < 0 || ( a == 4 && !Cmd_Argv( 3 )[0] ) ) {
			Com_Printf( "usage: " SC_CMD " add <kind> [size [yaw pitch]]: kinds are planet, gas, ringed, moon, sun, station, nebula, fleet\n"
				"       " SC_CMD " add model <path> [size [yaw pitch]]\n" );
			return;
		}
		SC_Look( &yaw, &pitch, a + 1 );
		if ( ( sw.pick = SC_Add( k, Cmd_Argc() > a ? Com_Clamp( 0.1f, 60.0f, atof( Cmd_Argv( a ) ) ) : 0.0f, yaw, pitch, a == 4 ? Cmd_Argv( 3 ) : NULL ) ) < 0 ) {
			Com_Printf( "%i objects at most: remove one first\n", SC_MAX );
			return;
		}
		Com_Printf( "%s added (%i), where you look\n", scKinds[k].kind, (int)sc.objects.size() );
	} else if ( !Q_stricmp( arg, "here" ) || !Q_stricmp( arg, "size" ) || !Q_stricmp( arg, "color" ) || !Q_stricmp( arg, "colour" )
		|| !Q_stricmp( arg, "rings" ) || !Q_stricmp( arg, "tilt" ) || !Q_stricmp( arg, "seed" ) || !Q_stricmp( arg, "remove" )
		|| !Q_stricmp( arg, "model" ) || !Q_stricmp( arg, "heading" ) || !Q_stricmp( arg, "turn" ) ) {
		const int n = SC_Index( Cmd_Argv( 2 ) );
		scObject_t *o;

		if ( n < 0 )
			return;
		o = &sc.objects[n];
		if ( !Q_stricmp( arg, "here" ) )
			SC_Look( &o->yaw, &o->pitch, 3 );
		else if ( !Q_stricmp( arg, "size" ) )
			o->size = Com_Clamp( 0.1f, 60.0f, atof( Cmd_Argv( 3 ) ) );
		else if ( !Q_stricmp( arg, "rings" ) )
			o->rings = ( !Q_stricmp( Cmd_Argv( 3 ), "on" ) || atoi( Cmd_Argv( 3 ) ) > 0 ) ? Q_max( 1.0f, (float)atoi( Cmd_Argv( 3 ) ) ) : 0.0f;
		else if ( !Q_stricmp( arg, "tilt" ) )
			o->tilt = Com_Clamp( -89.0f, 89.0f, atof( Cmd_Argv( 3 ) ) );
		else if ( !Q_stricmp( arg, "seed" ) )
			o->seed = atof( Cmd_Argv( 3 ) );
		else if ( !Q_stricmp( arg, "model" ) )
			Q_strncpyz( o->model, Cmd_Argv( 3 ), sizeof( o->model ) );
		else if ( !Q_stricmp( arg, "heading" ) )
			o->tilt = AngleNormalize360( atof( Cmd_Argv( 3 ) ) );
		else if ( !Q_stricmp( arg, "turn" ) )
			o->rings = Com_Clamp( -90.0f, 90.0f, atof( Cmd_Argv( 3 ) ) );
		else if ( !Q_stricmp( arg, "remove" ) ) {
			sc.objects.erase( sc.objects.begin() + n );
			sw.pick = -1;
		} else {
			for ( int c = 0; c < 3; c++ )
				o->color[c] = Com_Clamp( 0.0f, 4.0f, atof( Cmd_Argv( 3 + c ) ) );
		}
	} else if ( !Q_stricmp( arg, "clear" ) ) {
		sc.composed = sc.lightning = qfalse;
		sc.hour = sc.clouds = -1.0f;
		sc.objects.clear();
		sw.pick = -1;
	} else if ( !Q_stricmp( arg, "share" ) ) {
		if ( SC_Share() )
			Com_Printf( "Your sky shared with everyone here with the client\n" );
		return;
	} else if ( !Q_stricmp( arg, "unshare" ) ) {
		if ( SC_Unshare() )
			Com_Printf( "The shared sky taken back\n" );
		return;
	} else {
		SC_Usage();
		return;
	}
	SC_Apply();
}

/*
===============================================================================

The window: the sky's settings at the top, the objects in the middle, the one picked under them

===============================================================================
*/

#define SW_X			368.0f
#define SW_Y			14.0f
#define SW_W			262.0f
#define SW_H			452.0f
#define SW_IN			( SW_X + 10 )
#define SW_IW			( SW_W - 20 )
#define SW_ROW			16.0f
#define SW_CTRL_H		14.0f
#define SW_SCALE		0.7f
#define SW_MESSAGE_MS	5000
#define SW_LIST_ROWS	5

static void SW_Message( const char *text ) {
	Q_strncpyz( sw.message, text, sizeof( sw.message ) );
	sw.messageTime = cls.realtime;
}

static void SW_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void SW_Box( float x, float y, float w, float h, const float *fill ) {
	SW_Fill( x, y, w, h, fill );
	SW_Fill( x, y, w, 1, swBorder );
	SW_Fill( x, y + h - 1, w, 1, swBorder );
	SW_Fill( x, y, 1, h, swBorder );
	SW_Fill( x + w - 1, y, 1, h, swBorder );
}

static void SW_Text( float x, float y, float w, const char *text, const float *color, int font, float scale, qboolean centred ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 3 && re->Font_StrLenPixels( buf, font, scale ) > w ) {
		len--;
		buf[len - 3] = buf[len - 2] = buf[len - 1] = '.';
		buf[len] = '\0';
	}
	if ( centred )
		x += ( w - re->Font_StrLenPixels( buf, font, scale ) ) * 0.5f;
	re->Font_DrawString( (int)x, (int)y, buf, color, font | STYLE_DROPSHADOW, -1, scale );
}

static qboolean SW_InRect( float x, float y, float w, float h ) {
	return (qboolean)( sw.cursorX >= x && sw.cursorX < x + w && sw.cursorY >= y && sw.cursorY < y + h );
}

static qboolean SW_Clicked( float x, float y, float w, float h ) {
	if ( !sw.click || sw.clickX < x || sw.clickX >= x + w || sw.clickY < y || sw.clickY >= y + h )
		return qfalse;
	sw.click = qfalse;
	if ( sw.clickSound )
		S_StartLocalSound( sw.clickSound, CHAN_LOCAL_SOUND );
	return qtrue;
}

static qboolean SW_Button( float x, float y, float w, const char *label, qboolean on, qboolean enabled ) {
	SW_Box( x, y, w, SW_CTRL_H, on ? swItemOn : enabled && SW_InRect( x, y, w, SW_CTRL_H ) ? swItemHover : swItem );
	SW_Text( x, y + 1, w, label, enabled ? swWhite : swDim, sw.font, SW_SCALE, qtrue );
	return enabled ? SW_Clicked( x, y, w, SW_CTRL_H ) : qfalse;
}

// a value dragged along a bar, or stepped with the wheel over it; qtrue when it changed
static qboolean SW_Slider( float x, float y, float w, const char *label, float *value, float min, float max, float step, const char *fmt, qboolean enabled ) {
	const float labelW = 58.0f, valueW = 38.0f, bx = x + labelW, bw = w - labelW - valueW;
	const float old = *value;
	float frac;

	SW_Text( x, y + 1, labelW - 4, label, enabled ? swDim : swBar, sw.font, SW_SCALE, qfalse );
	if ( enabled && SW_Clicked( bx - 3, y, bw + 6, SW_CTRL_H ) )
		sw.drag = value;
	if ( sw.drag == value ) {
		if ( sw.held && enabled )
			*value = min + ( max - min ) * Com_Clamp( 0.0f, 1.0f, ( sw.cursorX - bx ) / bw );
		else
			sw.drag = NULL;
	}
	if ( enabled && sw.wheel && SW_InRect( bx - 3, y, bw + 6, SW_CTRL_H ) ) {
		*value -= sw.wheel * step;
		sw.wheel = 0;
	}
	if ( *value != old )
		*value = Com_Clamp( min, max, step * floorf( *value / step + 0.5f ) );
	frac = Com_Clamp( 0.0f, 1.0f, ( *value - min ) / ( max - min ) );
	SW_Box( bx, y + 5, bw, 5, swBar );
	SW_Fill( bx + ( bw - 4 ) * frac, y + 1, 4, SW_CTRL_H - 2, enabled ? swWhite : swDim );
	SW_Text( bx + bw + 4, y + 1, valueW - 4, va( fmt, *value ), enabled ? swWhite : swDim, sw.font, SW_SCALE, qfalse );
	return (qboolean)( *value != old );
}

static void SC_Close( void ) {
	if ( !sw.open )
		return;
	sw.open = qfalse;
	sw.drag = NULL;
	sw.looking = sw.held = sw.typing = qfalse;
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_SKYCOMPOSER );
	SC_Apply();		// the server's sky again, if there's one
}

void CL_SkyComposer_Close( void ) {
	SC_Close();
}

static void SC_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Sky composer: join a server first\n" );
		return;
	}
	if ( sw.open )
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

	sw.font = re->RegisterFont( "arialnb" );
	if ( !sw.font )
		sw.font = cls.menuFont;
	sw.titleFont = re->RegisterFont( "ergoec" );
	if ( !sw.titleFont )
		sw.titleFont = sw.font;
	sw.clickSound = S_RegisterSound( "sound/interface/button1.wav" );
	sw.cursorX = SW_X - 120.0f;
	sw.cursorY = SCREEN_HEIGHT * 0.5f;
	sw.click = sw.held = sw.looking = qfalse;
	sw.drag = NULL;
	if ( sw.pick >= (int)sc.objects.size() )
		sw.pick = -1;
	sw.open = qtrue;
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_SKYCOMPOSER );
	SC_Apply();		// yours, as you make it
}

static float SW_DrawSky( float y ) {
	SW_Text( SW_IN, y, SW_IW, "THE SKY", swOrange, sw.titleFont, SW_SCALE + 0.05f, qfalse );
	y += SW_ROW;
	if ( SW_Button( SW_IN, y, SW_IW * 0.5f - 2, "Classic (the map's)", (qboolean)!sc.composed, qtrue ) && sc.composed ) {
		sc.composed = qfalse;
		sw.dirty = qtrue;
	}
	if ( SW_Button( SW_IN + SW_IW * 0.5f + 2, y, SW_IW * 0.5f - 2, "Composed", sc.composed, qtrue ) && !sc.composed ) {
		sc.composed = qtrue;
		sw.dirty = qtrue;
	}
	y += SW_ROW + 2;

	// the hour: none, the map's sun
	sw.hourV = sc.hour < 0.0f ? 0.0f : sc.hour;
	if ( SW_Slider( SW_IN, y, SW_IW - 52, "Hour", &sw.hourV, 0.0f, 24.0f, 0.25f, sc.hour < 0.0f ? "map" : "%.1f", qtrue ) ) {
		sc.hour = sw.hourV;
		sw.dirty = qtrue;
	}
	if ( SW_Button( SW_IN + SW_IW - 48, y, 48, "Map's", (qboolean)( sc.hour < 0.0f ), qtrue ) && sc.hour >= 0.0f ) {
		sc.hour = -1.0f;
		sw.dirty = qtrue;
	}
	y += SW_ROW;
	sw.cloudsV = sc.clouds < 0.0f ? 0.0f : sc.clouds;
	if ( SW_Slider( SW_IN, y, SW_IW - 52, "Clouds", &sw.cloudsV, 0.0f, 1.0f, 0.05f, sc.clouds < 0.0f ? "theirs" : "%.2f", qtrue ) ) {
		sc.clouds = sw.cloudsV;
		sw.dirty = qtrue;
	}
	if ( SW_Button( SW_IN + SW_IW - 48, y, 48, "Theirs", (qboolean)( sc.clouds < 0.0f ), qtrue ) && sc.clouds >= 0.0f ) {
		sc.clouds = -1.0f;
		sw.dirty = qtrue;
	}
	y += SW_ROW;
	if ( SW_Button( SW_IN, y, SW_IW, sc.lightning ? "Lightning: on" : "Lightning: off", sc.lightning, qtrue ) ) {
		sc.lightning = (qboolean)!sc.lightning;
		sw.dirty = qtrue;
	}
	return y + SW_ROW + 6;
}

static float SW_DrawObjects( float y ) {
	const int n = (int)sc.objects.size();
	const float bw = ( SW_IW - 6 ) / 4.0f;

	SW_Text( SW_IN, y, SW_IW, va( "OBJECTS  %i of %i", n, SC_MAX ), swOrange, sw.titleFont, SW_SCALE + 0.05f, qfalse );
	y += SW_ROW;
	// add one where you look
	for ( int k = 0; k < (int)ARRAY_LEN( scKinds ); k++ ) {
		const float bx = SW_IN + ( k % 4 ) * ( bw + 2 ), by = y + ( k / 4 ) * ( SW_CTRL_H + 2 );

		if ( SW_Button( bx, by, bw, scKinds[k].title, qfalse, (qboolean)( n < SC_MAX ) ) ) {
			float yaw, pitch;

			SC_LookView( &yaw, &pitch );
			sw.pick = SC_Add( k, 0.0f, yaw, pitch, NULL );
			sw.dirty = qtrue;
			if ( !Q_stricmp( scKinds[k].kind, "model" ) && sw.pick >= 0 ) {
				sw.path[0] = '\0';
				sw.pathPick = sw.pick;
				sw.typing = qtrue;
				WG_EditReset();
				SW_Message( "Type the model's path, then Enter" );
			} else
				SW_Message( va( "%s added where you look", scKinds[k].title ) );
		}
	}
	y += ( ( ARRAY_LEN( scKinds ) + 3 ) / 4 ) * ( SW_CTRL_H + 2 ) + 4;

	// the list
	{
		const float listH = SW_LIST_ROWS * SW_ROW;
		const qboolean bar = WG_Scrollbar( SW_IN + SW_IW - WG_SCROLL_W, y, listH, n, SW_LIST_ROWS, &sw.scroll,
			sw.cursorX, sw.cursorY, &sw.click, sw.clickX, sw.clickY );
		const float w = SW_IW - ( bar ? WG_SCROLL_W + 2 : 0 );

		sw.scroll = Com_Clampi( 0, Q_max( 0, n - SW_LIST_ROWS ), sw.scroll );
		if ( !n )
			SW_Text( SW_IN, y + 2, SW_IW, "None yet: look where it should be, then add it", swDim, sw.font, SW_SCALE, qfalse );
		for ( int i = sw.scroll; i < n && i < sw.scroll + SW_LIST_ROWS; i++ ) {
			const scObject_t *o = &sc.objects[i];
			const float ry = y + ( i - sw.scroll ) * SW_ROW;
			const qboolean on = (qboolean)( i == sw.pick ), hover = SW_InRect( SW_IN, ry, w, SW_ROW - 2 );
			const int k = SC_Kind( o->kind );

			SW_Box( SW_IN, ry, w, SW_ROW - 2, on ? swItemOn : hover ? swItemHover : swItem );
			SW_Text( SW_IN + 4, ry + 1, w - 8, va( "%i  %s   yaw %.0f  pitch %.0f", i + 1, k >= 0 ? scKinds[k].title : o->kind, o->yaw, o->pitch ),
				on || hover ? swWhite : swOrange, sw.font, SW_SCALE, qfalse );
			if ( SW_Clicked( SW_IN, ry, w, SW_ROW - 2 ) )
				sw.pick = i;
		}
		y += listH + 4;
	}
	return y;
}

static float SW_DrawPicked( float y ) {
	scObject_t *o;
	int changed = 0;
	int k;

	if ( sw.pick < 0 || sw.pick >= (int)sc.objects.size() ) {
		SW_Text( SW_IN, y, SW_IW, "Pick an object to change it", swDim, sw.font, SW_SCALE, qfalse );
		return y + SW_ROW;
	}
	o = &sc.objects[sw.pick];
	k = SC_Kind( o->kind );
	SW_Text( SW_IN, y, SW_IW, va( "%s %i: %s", k >= 0 ? scKinds[k].title : o->kind, sw.pick + 1, k >= 0 ? scKinds[k].what : "" ),
		swOrange, sw.font, SW_SCALE, qfalse );
	y += SW_ROW;
	if ( SW_Button( SW_IN, y, SW_IW * 0.5f - 2, "Place where I look", qfalse, qtrue ) ) {
		SC_LookView( &o->yaw, &o->pitch );
		changed = 1;
	}
	if ( SW_Button( SW_IN + SW_IW * 0.5f + 2, y, SW_IW * 0.25f - 3, "New look", qfalse, qtrue ) ) {
		o->seed = (float)( rand() % 1000 );
		changed = 1;
	}
	if ( SW_Button( SW_IN + SW_IW * 0.75f + 1, y, SW_IW * 0.25f - 1, "Remove", qfalse, qtrue ) ) {
		sc.objects.erase( sc.objects.begin() + sw.pick );
		sw.pick = -1;
		sw.dirty = qtrue;
		return y + SW_ROW;
	}
	y += SW_ROW + 2;
	changed |= SW_Slider( SW_IN, y, SW_IW, "Yaw", &o->yaw, 0.0f, 360.0f, 1.0f, "%.0f", qtrue );
	y += SW_ROW;
	changed |= SW_Slider( SW_IN, y, SW_IW, "Height", &o->pitch, -10.0f, 89.0f, 1.0f, "%.0f", qtrue );
	y += SW_ROW;
	changed |= SW_Slider( SW_IN, y, SW_IW, "Size", &o->size, 0.5f, 45.0f, 0.5f, "%.1f", qtrue );
	y += SW_ROW;
	if ( !Q_stricmp( o->kind, "model" ) ) {
		wgStyle_t style;

		if ( sw.pathPick != sw.pick ) {
			Q_strncpyz( sw.path, o->model, sizeof( sw.path ) );
			sw.pathPick = sw.pick;
			sw.typing = qfalse;
			sw.pathBad = (qboolean)( o->model[0] && !re->RegisterModel( o->model ) );
		}
		memset( &style, 0, sizeof( style ) );
		style.font = sw.font;
		style.scale = SW_SCALE;
		style.lineH = 13.0f;
		style.padX = 4;
		style.padY = 1;
		style.fill = swItem;
		style.border = swBorder;
		style.text = swWhite;
		style.dim = swDim;
		SW_Box( SW_IN, y, SW_IW, SW_CTRL_H, swItem );
		if ( sw.typing )
			SW_Fill( SW_IN, y + SW_CTRL_H - 1, SW_IW, 1, swOrange );
		WG_Field( &style, SW_IN, y, SW_IW, SW_CTRL_H, sw.path, sw.typing, "models/.../name.md3, Enter", sw.cursorX, sw.cursorY );
		if ( SW_Clicked( SW_IN, y, SW_IW, SW_CTRL_H ) ) {
			if ( !sw.typing )
				WG_EditReset();
			sw.typing = qtrue;
			WG_FieldClick( sw.path, sw.clickX, sw.clickY );
		} else if ( sw.click && sw.typing ) {
			sw.typing = qfalse;		// a click elsewhere: the path as typed
			Q_strncpyz( o->model, sw.path, sizeof( o->model ) );
			sw.pathBad = (qboolean)( o->model[0] && !re->RegisterModel( o->model ) );
			changed = 1;
		}
		y += SW_ROW + 2;
		if ( sw.pathBad ) {
			SW_Text( SW_IN, y, SW_IW, "No such model", swYellow, sw.font, SW_SCALE, qfalse );
			y += SW_ROW;
		}
		changed |= SW_Slider( SW_IN, y, SW_IW, "Heading", &o->tilt, 0.0f, 360.0f, 1.0f, "%.0f", qtrue );
		y += SW_ROW;
		changed |= SW_Slider( SW_IN, y, SW_IW, "Turning", &o->rings, -30.0f, 30.0f, 0.5f, "%.1f", qtrue );
		y += SW_ROW;
		if ( changed )
			sw.dirty = qtrue;
		return y;
	}
	changed |= SW_Slider( SW_IN, y, SW_IW - 20, "Red", &o->color[0], 0.0f, 1.0f, 0.02f, "%.2f", qtrue );
	changed |= SW_Slider( SW_IN, y + SW_ROW, SW_IW - 20, "Green", &o->color[1], 0.0f, 1.0f, 0.02f, "%.2f", qtrue );
	changed |= SW_Slider( SW_IN, y + SW_ROW * 2, SW_IW - 20, "Blue", &o->color[2], 0.0f, 1.0f, 0.02f, "%.2f", qtrue );
	{
		const float swatch[4] = { o->color[0], o->color[1], o->color[2], 1.0f };

		SW_Box( SW_IN + SW_IW - 16, y, 16, SW_ROW * 3 - 2, swatch );
	}
	y += SW_ROW * 3;
	if ( !Q_stricmp( o->kind, "fleet" ) ) {
		changed |= SW_Slider( SW_IN, y, SW_IW, "Ships", &o->rings, 0.0f, 9.0f, 1.0f, "%.0f", qtrue );
		y += SW_ROW;
	} else if ( !Q_stricmp( o->kind, "planet" ) || !Q_stricmp( o->kind, "gas" ) || !Q_stricmp( o->kind, "ringed" ) || !Q_stricmp( o->kind, "moon" ) ) {
		if ( SW_Button( SW_IN, y, SW_IW * 0.35f, o->rings > 0.0f ? "Rings: on" : "Rings: off", (qboolean)( o->rings > 0.0f ), qtrue ) ) {
			o->rings = o->rings > 0.0f ? 0.0f : 1.0f;
			changed = 1;
		}
		if ( o->rings > 0.0f )
			changed |= SW_Slider( SW_IN + SW_IW * 0.35f + 4, y, SW_IW * 0.65f - 4, "Tilt", &o->tilt, -89.0f, 89.0f, 1.0f, "%.0f", qtrue );
		y += SW_ROW;
	}
	if ( changed )
		sw.dirty = qtrue;
	return y;
}

void CL_SkyComposer_Draw( void ) {
	float y;

	if ( !sw.open )
		return;
	SW_Box( SW_X, SW_Y, SW_W, SW_H, swPanel );
	SW_Text( SW_IN, SW_Y + 6, 160, "SKY COMPOSER", swOrange, sw.titleFont, SW_SCALE + 0.15f, qfalse );
	if ( SW_Button( SW_X + SW_W - 10 - 48, SW_Y + 7, 48, "Close", qfalse, qtrue ) ) {
		SC_Close();
		return;
	}
	y = SW_Y + 26;
	if ( !SC_Drawable() ) {
		SW_Text( SW_IN, y, SW_IW, "Seen with Vulkan and its post effects only", swYellow, sw.font, SW_SCALE, qfalse );
		y += SW_ROW;
	} else if ( sc.sharedIn ) {
		SW_Text( SW_IN, y, SW_IW, "The server has a sky: yours shows while this is open", swYellow, sw.font, SW_SCALE, qfalse );
		y += SW_ROW;
	}

	y = SW_DrawSky( y );
	y = SW_DrawObjects( y );
	y = SW_DrawPicked( y );

	// for everyone, or not
	y = SW_Y + SW_H - 44;
	if ( SW_Button( SW_IN, y, SW_IW / 3.0f - 2, "Share", qfalse, qtrue ) && SC_Share() )
		SW_Message( "Your sky shown to everyone here with the client" );
	if ( SW_Button( SW_IN + SW_IW / 3.0f + 1, y, SW_IW / 3.0f - 2, "Unshare", qfalse, qtrue ) && SC_Unshare() )
		SW_Message( "The shared sky taken back" );
	if ( SW_Button( SW_IN + 2 * SW_IW / 3.0f + 2, y, SW_IW / 3.0f - 2, sc.sharedIn ? "Edit the server's" : "Clear", qfalse, qtrue ) ) {
		if ( sc.sharedIn ) {
			SC_Parse( sc.shared.c_str() );
			SW_Message( "The server's sky, to change and share again" );
		} else {
			sc.composed = sc.lightning = qfalse;
			sc.hour = sc.clouds = -1.0f;
			sc.objects.clear();
		}
		sw.pick = -1;
		sw.dirty = qtrue;
	}
	y += SW_ROW + 2;
	if ( sw.message[0] && cls.realtime - sw.messageTime < SW_MESSAGE_MS )
		SW_Text( SW_IN, y, SW_IW, sw.message, swWhite, sw.font, SW_SCALE, qfalse );
	else
		SW_Text( SW_IN, y, SW_IW, "Hold the right mouse button to look around", swDim, sw.font, SW_SCALE, qfalse );

	// shown as it's made, a few times a second at most while dragging (the renderer waits on it)
	if ( sw.dirty && ( !sw.drag || cls.realtime - sw.applyAt > 120 ) )
		SC_Apply();
	sw.click = qfalse;
	sw.wheel = 0;
	WG_DrawOverlay();
	if ( !sw.looking ) {
		re->SetColor( NULL );
		re->DrawStretchPic( sw.cursorX, sw.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
	}
}

void CL_SkyComposer_KeyEvent( int key, qboolean down ) {
	if ( !sw.open )
		return;
	if ( key == A_MOUSE1 ) {
		sw.held = down;
		if ( down ) {
			sw.click = qtrue;
			sw.clickX = sw.cursorX;
			sw.clickY = sw.cursorY;
		}
		return;
	}
	if ( key == A_MOUSE2 ) {
		sw.looking = down;
		return;
	}
	if ( !down )
		return;
	if ( sw.typing ) {
		if ( key == A_ENTER || key == A_KP_ENTER || key == A_TAB ) {
			sw.typing = qfalse;
			WG_EditReset();
			if ( sw.pathPick >= 0 && sw.pathPick < (int)sc.objects.size() ) {
				Q_strncpyz( sc.objects[sw.pathPick].model, sw.path, sizeof( sc.objects[sw.pathPick].model ) );
				sw.pathBad = (qboolean)( sw.path[0] && !re->RegisterModel( sw.path ) );
				sw.dirty = qtrue;
			}
		} else
			WG_EditKey( sw.path, sizeof( sw.path ), key, NULL );
		return;
	}
	if ( key == A_MWHEELUP )
		sw.wheel--;
	else if ( key == A_MWHEELDOWN )
		sw.wheel++;
	else {
		const char *binding = Key_GetBinding( key );

		if ( VALIDSTRING( binding ) && !Q_stricmp( binding, SC_CMD ) )
			SC_Close();
	}
}

// the cursor, or with the right button held the view, turned as the mouse would
void CL_SkyComposer_MouseEvent( int dx, int dy ) {
	if ( sw.looking ) {
		const float sens = Cvar_VariableValue( "sensitivity" );

		cl.viewangles[YAW] -= Cvar_VariableValue( "m_yaw" ) * dx * sens;
		cl.viewangles[PITCH] = Com_Clamp( -89.0f, 89.0f, cl.viewangles[PITCH] + Cvar_VariableValue( "m_pitch" ) * dy * sens );
		return;
	}
	sw.cursorX = Com_Clamp( 0, SCREEN_WIDTH, sw.cursorX + dx );
	sw.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, sw.cursorY + dy );
}

void CL_SkyComposer_CharEvent( int ch ) {
	if ( sw.open && sw.typing )
		WG_EditChar( sw.path, sizeof( sw.path ), ch, NULL );
}

void CL_SkyComposer_Escape( void ) {
	if ( sw.typing ) {
		sw.typing = qfalse;
		sw.pathPick = -1;		// the path as it was
		WG_EditReset();
		return;
	}
	SC_Close();
}

// cmd:skyc: <scene>, cmd:skyc/off
void CL_SkyComposer_Hook( char **argv, int argc, const char *message ) {
	if ( argc >= 1 && !Q_stricmp( argv[0], "off" ) ) {
		sc.sharedIn = qfalse;
		sc.shared.clear();
	} else {
		sc.sharedIn = qtrue;
		sc.shared = message ? message : "";
	}
	SC_Apply();
}

// a new server: its own sky, the scene composed here kept to show again
void CL_SkyComposer_Disconnect( void ) {
	SC_Close();
	sc.sharedIn = qfalse;
	sc.shared.clear();
	SC_Apply();
}

void CL_SkyComposer_Init( void ) {
	memset( &sw, 0, sizeof( sw ) );
	sw.pick = sw.pathPick = -1;
	Cmd_AddCommand( SC_CMD, SC_Cmd_f, "The sky composer: the map's sky or one of the hour, clouds, lightning, planets, moons, a fleet... (Vulkan)" );
}

void CL_SkyComposer_Shutdown( void ) {
	SC_Close();
	Cmd_RemoveCommand( SC_CMD );
}
