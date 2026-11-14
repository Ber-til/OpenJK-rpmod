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
// (rd-vulkan's vk_rpmod_post.cpp, r_skyScene): the hour (the sun's place, dusk, night and its
// stars), the clouds, lightning in them, and objects: planets, gas giants, moons, suns, a battle
// station, ringed worlds, nebulae, a fleet. Each object goes where you look as you add it.
//
// What you compose shows on your screen at once; /skycomposer share sends it to everyone on the
// server with the client (an RPMod admin's, through C1B3R, which keeps it for who joins later), as
// cmd:skyc: <scene>, and cmd:skyc/off takes it back. A scene shared is shown in place of your own
// until it's taken back.

#include "client.h"
#include "cl_presence.h"
#include <string>
#include <vector>

#define SC_CMD			"skycomposer"
#define SC_MAX			16

typedef struct {
	char	kind[16];
	float	yaw, pitch, size;
	float	color[3];
	float	seed, tilt, rings;
} scObject_t;

static const struct {
	const char	*kind;
	float		size;			// its radius in degrees, as made
	float		color[3];
	float		rings;
	const char	*what;
} scKinds[] = {
	{ "planet",		8.0f,	{ 0.45f, 0.6f, 0.4f },	0.0f,	"land and seas, a thin glow of air" },
	{ "gas",		12.0f,	{ 0.9f, 0.7f, 0.5f },	0.0f,	"a gas giant, banded" },
	{ "ringed",		10.0f,	{ 0.85f, 0.75f, 0.55f },	1.0f,	"a gas giant with rings" },
	{ "moon",		4.0f,	{ 0.75f, 0.75f, 0.72f },	0.0f,	"grey and cratered" },
	{ "sun",		2.0f,	{ 1.0f, 0.85f, 0.6f },	0.0f,	"a second sun, glowing" },
	{ "station",	5.0f,	{ 0.6f, 0.62f, 0.65f },	0.0f,	"a battle station: plates, a trench, a dish" },
	{ "nebula",		30.0f,	{ 0.5f, 0.2f, 0.6f },	0.0f,	"coloured gas across the stars" },
	{ "fleet",		6.0f,	{ 0.55f, 0.57f, 0.6f },	0.0f,	"a few ships, engines lit (rings: more ships)" },
};

static struct {
	float		hour;			// -1 none
	float		clouds;			// -1 none
	qboolean	lightning;
	std::vector<scObject_t>	objects;

	qboolean	sharedIn;		// a scene from the server shown in place of this one
	std::string	shared;
} sc = { -1.0f, -1.0f };

static std::string SC_Scene( void ) {
	std::string out;

	if ( sc.hour >= 0.0f )
		out += va( "time %g ", sc.hour );
	if ( sc.clouds >= 0.0f )
		out += va( "clouds %g ", sc.clouds );
	if ( sc.lightning )
		out += "lightning 1 ";
	for ( size_t i = 0; i < sc.objects.size(); i++ ) {
		const scObject_t *o = &sc.objects[i];

		out += va( "; %s %.0f %.0f %g %.2g %.2g %.2g %.0f %.0f %.0f ", o->kind, o->yaw, o->pitch, o->size,
			o->color[0], o->color[1], o->color[2], o->seed, o->tilt, o->rings );
	}
	return out;
}

// what the renderer draws: the server's scene, else this one, in r_skyScene to r_skyScene4 (a setting
// holds 256 characters), cut between objects
static void SC_Apply( void ) {
	const std::string scene = sc.sharedIn ? sc.shared : SC_Scene();
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
}

static int SC_Kind( const char *word ) {
	for ( int i = 0; i < (int)ARRAY_LEN( scKinds ); i++ ) {
		if ( !Q_stricmp( word, scKinds[i].kind ) )
			return i;
	}
	return -1;
}

// where the view looks now, as a sky direction (the player's angles as the server has them: the
// commands' own differ after a teleport); or as given after the word at "from"
static void SC_Look( float *yaw, float *pitch, int from ) {
	if ( Cmd_Argc() > from + 1 ) {
		*yaw = AngleNormalize360( atof( Cmd_Argv( from ) ) );
		*pitch = Com_Clamp( -10.0f, 89.0f, atof( Cmd_Argv( from + 1 ) ) );
		return;
	}
	*yaw = AngleNormalize360( cl.snap.ps.viewangles[YAW] );
	*pitch = Com_Clamp( -10.0f, 89.0f, -AngleNormalize180( cl.snap.ps.viewangles[PITCH] ) );
}

static int SC_Index( const char *arg ) {
	const int n = atoi( arg ) - 1;

	if ( !arg[0] || n < 0 || n >= (int)sc.objects.size() ) {
		Com_Printf( "No object %s: /" SC_CMD " list numbers them\n", arg );
		return -1;
	}
	return n;
}

static void SC_List( void ) {
	Com_Printf( "Sky: time %s, clouds %s, lightning %s%s\n", sc.hour >= 0.0f ? va( "%g", sc.hour ) : "the map's",
		sc.clouds >= 0.0f ? va( "%g", sc.clouds ) : "yours (r_clouds)", sc.lightning ? "on" : "off",
		sc.sharedIn ? S_COLOR_YELLOW "  (the server's scene shows in place of this)" : "" );
	for ( size_t i = 0; i < sc.objects.size(); i++ ) {
		const scObject_t *o = &sc.objects[i];

		Com_Printf( "%2i  %-8s yaw %3.0f  pitch %3.0f  size %g  colour %.2f %.2f %.2f%s\n", (int)i + 1, o->kind, o->yaw, o->pitch, o->size,
			o->color[0], o->color[1], o->color[2], o->rings > 0.0f ? "  rings" : "" );
	}
	if ( Q_stricmp( Cvar_VariableString( "cl_renderer" ), "rd-vulkan" ) || !Cvar_VariableIntegerValue( "r_fbo" ) )
		Com_Printf( S_COLOR_YELLOW "The sky composer draws with Vulkan and its post effects (cl_renderer rd-vulkan, r_fbo 1)\n" );
}

static void SC_Usage( void ) {
	Com_Printf( "usage: " SC_CMD " list                    what's in your sky\n" );
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
	Com_Printf( "       " SC_CMD " remove <n> | clear      one gone, or all of it\n" );
	Com_Printf( "       " SC_CMD " share | unshare         to everyone here (RPMod admins), or taken back\n" );
}

static void SC_Cmd_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !arg[0] ) {
		SC_Usage();
		SC_List();
		return;
	}
	if ( !Q_stricmp( arg, "list" ) ) {
		SC_List();
		return;
	}
	if ( !Q_stricmp( arg, "time" ) || !Q_stricmp( arg, "clouds" ) ) {
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
		scObject_t o;

		if ( k < 0 ) {
			Com_Printf( "usage: " SC_CMD " add <kind> [size]: kinds are planet, gas, ringed, moon, sun, station, nebula, fleet\n" );
			return;
		}
		if ( (int)sc.objects.size() >= SC_MAX ) {
			Com_Printf( "%i objects at most: remove one first\n", SC_MAX );
			return;
		}
		memset( &o, 0, sizeof( o ) );
		Q_strncpyz( o.kind, scKinds[k].kind, sizeof( o.kind ) );
		SC_Look( &o.yaw, &o.pitch, 4 );
		o.size = Cmd_Argc() > 3 ? Com_Clamp( 0.1f, 60.0f, atof( Cmd_Argv( 3 ) ) ) : scKinds[k].size;
		VectorCopy( scKinds[k].color, o.color );
		o.seed = (float)( rand() % 100 );
		o.tilt = 20.0f;
		o.rings = scKinds[k].rings;
		sc.objects.push_back( o );
		Com_Printf( "%s added (%i), where you look\n", o.kind, (int)sc.objects.size() );
	} else if ( !Q_stricmp( arg, "here" ) || !Q_stricmp( arg, "size" ) || !Q_stricmp( arg, "color" ) || !Q_stricmp( arg, "colour" )
		|| !Q_stricmp( arg, "rings" ) || !Q_stricmp( arg, "tilt" ) || !Q_stricmp( arg, "seed" ) || !Q_stricmp( arg, "remove" ) ) {
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
		else if ( !Q_stricmp( arg, "remove" ) )
			sc.objects.erase( sc.objects.begin() + n );
		else {
			for ( int c = 0; c < 3; c++ )
				o->color[c] = Com_Clamp( 0.0f, 4.0f, atof( Cmd_Argv( 3 + c ) ) );
		}
	} else if ( !Q_stricmp( arg, "clear" ) ) {
		sc.hour = sc.clouds = -1.0f;
		sc.lightning = qfalse;
		sc.objects.clear();
	} else if ( !Q_stricmp( arg, "share" ) ) {
		const std::string scene = SC_Scene();

		if ( scene.empty() ) {
			Com_Printf( "Nothing to share: compose the sky first\n" );
			return;
		}
		if ( CL_Presence_Hook( -1, va( "cmd:skyc: %s", scene.c_str() ) ) )
			Com_Printf( "Your sky shared with everyone here with the client\n" );
		return;
	} else if ( !Q_stricmp( arg, "unshare" ) ) {
		if ( CL_Presence_Hook( -1, "cmd:skyc/off" ) )
			Com_Printf( "The shared sky taken back\n" );
		return;
	} else {
		SC_Usage();
		return;
	}
	SC_Apply();
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
	sc.sharedIn = qfalse;
	sc.shared.clear();
	SC_Apply();
}

void CL_SkyComposer_Init( void ) {
	Cmd_AddCommand( SC_CMD, SC_Cmd_f, "The sky composer: the hour, clouds, lightning, planets, moons, a fleet... (Vulkan)" );
}

void CL_SkyComposer_Shutdown( void ) {
	Cmd_RemoveCommand( SC_CMD );
}
