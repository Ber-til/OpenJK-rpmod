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

// cl_saberlight.cpp -- each lightsaber blade lights what's around it in the blade's colour, the
// light pulsing as a light style does and following the blade along its length. Drawn here only,
// nothing is sent: every blade cgame draws is seen here, whoever holds it. The game's own steady
// light at the middle of a blade is left out while this one is on, and gives it its colour: the
// blade's, an RGB one too.
//
//	saberlight [on|off|mine|style <letters>|bright <x>|size <x>]
//	cl_saberLight			0 off (the default), 1 every blade, 2 only your own
//	cl_saberLightStyle		the pulse: letters a (dark) to z (full), one each 50 ms
//	cl_saberLightBright		how bright, 1 as the game's own
//	cl_saberLightSize		how far it reaches, 1 as the game's own

#include "client.h"

#define SL_MAX_BLADES		32
#define SL_MAX_GAMELIGHTS	64
#define SL_MAX_LIGHTS		24		// of the renderer's 32, the rest left to the map and the effects
#define SL_STEP_MS			50
#define SL_STYLE			"tuvwxyzzzyxwvutsrqqrs"
#define SL_GAME_LIGHT_NEAR	2.0f	// the game's light this close to a blade's middle is the blade's

static cvar_t *cl_saberLight;
static cvar_t *cl_saberLightStyle;
static cvar_t *cl_saberLightBright;
static cvar_t *cl_saberLightSize;

typedef struct {
	vec3_t		base, dir;
	float		length;
	vec3_t		rgb;
} slBlade_t;

typedef struct {
	vec3_t		org;
	float		radius, r, g, b;
} slLight_t;

// the base game's blade colours, by their glow shader
static const struct {
	const char	*shader;
	float		rgb[3];
} slColours[] = {
	{ "gfx/effects/sabers/red_glow",	{ 1.0f, 0.2f, 0.2f } },
	{ "gfx/effects/sabers/orange_glow",	{ 1.0f, 0.5f, 0.1f } },
	{ "gfx/effects/sabers/yellow_glow",	{ 1.0f, 1.0f, 0.2f } },
	{ "gfx/effects/sabers/green_glow",	{ 0.2f, 1.0f, 0.2f } },
	{ "gfx/effects/sabers/blue_glow",	{ 0.2f, 0.4f, 1.0f } },
	{ "gfx/effects/sabers/purple_glow",	{ 0.9f, 0.2f, 1.0f } },
};

static struct {
	slBlade_t	blades[SL_MAX_BLADES];
	int			numBlades;
	slLight_t	game[SL_MAX_GAMELIGHTS];	// the game's lights, held until the scene is drawn
	int			numGame;
	qhandle_t	glow[ARRAY_LEN( slColours )];
	qhandle_t	rgbGlow;					// a blade of any colour, its colour the entity's
	qboolean	registered;
} sl;

static void SL_Command_f( void );

void CL_SaberLight_Init( void ) {
	cl_saberLight = Cvar_Get( "cl_saberLight", "0", CVAR_ARCHIVE_ND, "Lightsabers light what's around them, pulsing: 0 off, 1 every blade, 2 only your own" );
	cl_saberLightStyle = Cvar_Get( "cl_saberLightStyle", SL_STYLE, CVAR_ARCHIVE_ND, "The blades' light pulse: letters a (dark) to z (full), one each 50 ms" );
	cl_saberLightBright = Cvar_Get( "cl_saberLightBright", "1", CVAR_ARCHIVE_ND, "How bright a blade's light is, 1 as the game's own" );
	cl_saberLightSize = Cvar_Get( "cl_saberLightSize", "1", CVAR_ARCHIVE_ND, "How far a blade's light reaches, 1 as the game's own" );
	Cmd_AddCommand( "saberlight", SL_Command_f, "Lightsabers light what's around them, pulsing: saberlight [on|off|mine|style <letters>|bright <x>|size <x>]" );
}

void CL_SaberLight_Shutdown( void ) {
	Cmd_RemoveCommand( "saberlight" );
}

// the shaders' handles are the renderer's: looked up again once it's started anew
void CL_SaberLight_RendererRestarted( void ) {
	sl.registered = qfalse;
	sl.numBlades = sl.numGame = 0;
}

static qboolean SL_On( void ) {
	return (qboolean)( cl_saberLight && cl_saberLight->integer > 0 && cls.state == CA_ACTIVE );
}

static void SL_Command_f( void ) {
	const char *what = Cmd_Argv( 1 );

	if ( !what[0] ) {
		Com_Printf( "Saber light: %s, style %s, bright x%g, size x%g\n",
			!cl_saberLight->integer ? "off" : cl_saberLight->integer == 2 ? "your own blades" : "every blade",
			cl_saberLightStyle->string[0] ? cl_saberLightStyle->string : "steady", cl_saberLightBright->value, cl_saberLightSize->value );
		Com_Printf( "  saberlight on | off | mine | style <letters a-z> | bright <x> | size <x>\n" );
		return;
	}
	if ( !Q_stricmp( what, "on" ) || !Q_stricmp( what, "all" ) )
		Cvar_Set( "cl_saberLight", "1" );
	else if ( !Q_stricmp( what, "off" ) )
		Cvar_Set( "cl_saberLight", "0" );
	else if ( !Q_stricmp( what, "mine" ) )
		Cvar_Set( "cl_saberLight", "2" );
	else if ( !Q_stricmp( what, "style" ) ) {
		char style[MAX_CVAR_VALUE_STRING];
		int n = 0;

		// only the letters, small; none is the pulse as it was
		for ( const char *p = Cmd_Argv( 2 ); *p && n < (int)sizeof( style ) - 1; p++ ) {
			const int c = tolower( *p );

			if ( c >= 'a' && c <= 'z' )
				style[n++] = (char)c;
		}
		style[n] = '\0';
		Cvar_Set( "cl_saberLightStyle", n ? style : SL_STYLE );
	} else if ( !Q_stricmp( what, "bright" ) && Cmd_Argc() > 2 )
		Cvar_SetValue( "cl_saberLightBright", Com_Clamp( 0.1f, 4.0f, atof( Cmd_Argv( 2 ) ) ) );
	else if ( !Q_stricmp( what, "size" ) && Cmd_Argc() > 2 )
		Cvar_SetValue( "cl_saberLightSize", Com_Clamp( 0.25f, 3.0f, atof( Cmd_Argv( 2 ) ) ) );
	else {
		Com_Printf( "saberlight on | off | mine | style <letters a-z> | bright <x> | size <x>\n" );
		return;
	}
	if ( cl_saberLight->integer && !Cvar_VariableIntegerValue( "r_dynamiclight" ) )
		Com_Printf( S_COLOR_YELLOW "Saber light needs dynamic lights: r_dynamiclight 1\n" );
}

// a blade's colour, when the game gives it no light of its own: an RGB blade's tint, else its glow's
static void SL_BladeColour( const refEntity_t *ent, vec3_t rgb ) {
	const byte *c = ent->shaderRGBA;

	if ( !sl.registered ) {
		for ( size_t i = 0; i < ARRAY_LEN( slColours ); i++ )
			sl.glow[i] = re->RegisterShader( slColours[i].shader );
		sl.rgbGlow = re->RegisterShader( "gfx/effects/sabers/rgb_glow" );
		sl.registered = qtrue;
	}
	if ( ( ent->customShader && ent->customShader == sl.rgbGlow )
		|| ( ( c[0] || c[1] || c[2] ) && !( c[0] == 255 && c[1] == 255 && c[2] == 255 ) ) ) {
		VectorSet( rgb, c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f );
		return;
	}
	for ( size_t i = 0; i < ARRAY_LEN( slColours ); i++ ) {
		if ( ent->customShader && ent->customShader == sl.glow[i] ) {
			VectorCopy( slColours[i].rgb, rgb );
			return;
		}
	}
	VectorSet( rgb, 1.0f, 1.0f, 1.0f );
}

// an entity on its way to the renderer: a blade's noted, to light around it
void CL_SaberLight_Entity( const refEntity_t *ent ) {
	slBlade_t *b;

	if ( ent->reType != RT_SABER_GLOW || !SL_On() || sl.numBlades >= SL_MAX_BLADES || ent->saberLength < 1.0f )
		return;
	// only your own: the blade nearer you than anyone else
	if ( cl_saberLight->integer == 2 && CL_ShaderManager_NearestPlayer( ent->origin ) != clc.clientNum )
		return;
	b = &sl.blades[sl.numBlades++];
	VectorCopy( ent->origin, b->base );
	VectorCopy( ent->axis[0], b->dir );
	VectorNormalize( b->dir );
	b->length = ent->saberLength;
	SL_BladeColour( ent, b->rgb );
}

// a light cgame adds: held until the scene's drawn, so the one it gives a blade can be left out.
// qtrue if it's held
qboolean CL_SaberLight_GameLight( const vec3_t org, float radius, float r, float g, float b ) {
	slLight_t *l;

	if ( !SL_On() )
		return qfalse;
	if ( sl.numGame >= SL_MAX_GAMELIGHTS )
		return qfalse;
	l = &sl.game[sl.numGame++];
	VectorCopy( org, l->org );
	l->radius = radius;
	l->r = r;
	l->g = g;
	l->b = b;
	return qtrue;
}

void CL_SaberLight_ClearScene( void ) {
	sl.numBlades = sl.numGame = 0;
}

// the pulse now, 0 dark .. 1 full: the style's letters run smoothly into each other
static float SL_Pulse( int phase ) {
	const char *style = cl_saberLightStyle->string;
	const int len = (int)strlen( style );
	float pos, frac, a, b;
	int i;

	if ( !len )
		return 1.0f;
	pos = ( Q_max( 0, cls.realtime ) / (float)SL_STEP_MS ) + phase;
	i = (int)pos;
	frac = pos - i;
	a = Com_Clamp( 0.0f, 1.0f, ( tolower( style[i % len] ) - 'a' ) / (float)( 'z' - 'a' ) );
	b = Com_Clamp( 0.0f, 1.0f, ( tolower( style[( i + 1 ) % len] ) - 'a' ) / (float)( 'z' - 'a' ) );
	return a + ( b - a ) * frac;
}

// before the scene's drawn: the game's lights but a blade's, then each blade's own along it
void CL_SaberLight_RenderScene( void ) {
	int budget = SL_MAX_LIGHTS, perBlade;

	if ( !sl.numGame && !sl.numBlades )
		return;
	for ( int i = 0; i < sl.numGame; i++ ) {
		const slLight_t *l = &sl.game[i];
		qboolean blades = qfalse;

		// the blade's own: left out, its colour the blade's
		for ( int j = 0; j < sl.numBlades; j++ ) {
			vec3_t mid;

			VectorMA( sl.blades[j].base, sl.blades[j].length * 0.5f, sl.blades[j].dir, mid );
			if ( Distance( mid, l->org ) < SL_GAME_LIGHT_NEAR ) {
				VectorSet( sl.blades[j].rgb, l->r, l->g, l->b );
				blades = qtrue;
			}
		}
		if ( !blades )
			re->AddLightToScene( l->org, l->radius, l->r, l->g, l->b );
	}
	// more lights to a blade while there are few blades, one each when there are many
	perBlade = sl.numBlades ? Com_Clampi( 1, 3, budget / sl.numBlades ) : 1;
	for ( int j = 0; j < sl.numBlades && budget > 0; j++ ) {
		const slBlade_t *bl = &sl.blades[j];
		const int n = Q_min( budget, bl->length > 40.0f ? perBlade : bl->length > 16.0f ? Q_min( 2, perBlade ) : 1 );
		// the blades out of step with each other, and a little flicker as a blade has
		const float pulse = SL_Pulse( j * 7 ) * ( 0.96f + 0.04f * Q_flrand( 0.0f, 1.0f ) );
		const float size = Com_Clamp( 0.25f, 3.0f, cl_saberLightSize->value );
		const float bright = Com_Clamp( 0.1f, 4.0f, cl_saberLightBright->value ) * pulse * ( n > 1 ? 1.4f / n : 1.0f );
		const float radius = bl->length * 1.4f * size * ( n > 1 ? 0.8f : 1.0f );

		for ( int i = 0; i < n; i++ ) {
			vec3_t at;

			VectorMA( bl->base, bl->length * ( i + 0.5f ) / n, bl->dir, at );
			re->AddLightToScene( at, radius, bl->rgb[0] * bright, bl->rgb[1] * bright, bl->rgb[2] * bright );
		}
		budget -= n;
	}
	sl.numBlades = sl.numGame = 0;
}
