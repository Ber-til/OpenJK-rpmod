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

// cl_saberstyle.cpp -- which lightsaber styles saberAttackCycle goes through, and going straight to one
//
//   saberstyles <style> <style> ...   saberAttackCycle goes through these, in this order
//   saberstyles all                   back to the server's own fast, medium, strong, desann, tavion
//   saberstyle <style>                straight to that one: fast, medium, strong, desann or tavion
//
// The server knows nothing but the cycle: one style on, back round to fast after the last, and
// the same command again within 300ms is ignored. Going to a style is cycling until the
// snapshot shows it, one at a time, so the styles on the way are on for a moment. Dual sabers
// and the staff turn a blade on and off with it instead, and are left to do that.

#include "client.h"

#define SS_CYCLE_GAP	400		// ms between two cycles, past the server's 300
#define SS_WAIT_MS		1500	// a cycle the snapshot doesn't show by then got lost, unless an attack holds it up
#define SS_MAX_CYCLES	10		// twice round: the style is one the server doesn't allow

static const char *ssNames[SS_DUAL] = { "", "fast", "medium", "strong", "desann", "tavion" };

static cvar_t *cl_saberStyles;

static struct {
	int		target;			// the style it's going to, 0 if none
	int		levelAtSend;
	int		sentTime;
	int		cycles;
} ss;

// the style on now, 0 if it's not one a cycle goes through: no saber out, dead, following
// someone, or two sabers or a staff
static int SS_Current( void ) {
	const playerState_t *ps = &cl.snap.ps;

	if ( cls.state != CA_ACTIVE || !cl.snap.valid || ps->weapon != WP_SABER || ps->stats[STAT_HEALTH] <= 0
		|| ( ps->pm_flags & PMF_FOLLOW ) || ps->pm_type == PM_SPECTATOR )
		return 0;
	if ( ps->fd.saberAnimLevel < SS_FAST || ps->fd.saberAnimLevel > SS_TAVION )
		return 0;
	return ps->fd.saberAnimLevel;
}

// a style by name, colour or number; 0 if none
static int SS_Parse( const char *name ) {
	static const char *colors[SS_DUAL] = { "", "blue", "yellow", "red", "", "" };

	for ( int i = SS_FAST; i <= SS_TAVION; i++ ) {
		if ( !Q_stricmp( name, ssNames[i] ) || ( colors[i][0] && !Q_stricmp( name, colors[i] ) )
			|| ( Q_isanumber( name ) && atoi( name ) == i ) )
			return i;
	}
	return 0;
}

// the cycle's styles, in order; how many, 0 for the server's own
static int SS_List( int *list ) {
	char buf[MAX_CVAR_VALUE_STRING];
	const char *p = buf, *token;
	int n = 0;

	Q_strncpyz( buf, cl_saberStyles->string, sizeof( buf ) );
	while ( n < SS_TAVION ) {
		int style;

		token = COM_Parse( &p );
		if ( !token[0] )
			break;
		style = SS_Parse( token );
		for ( int i = 0; i < n && style; i++ ) {
			if ( list[i] == style )
				style = 0;
		}
		if ( style )
			list[n++] = style;
	}
	return n;
}

static void SS_GoTo( int style ) {
	const int current = SS_Current();

	if ( !current ) {
		Com_Printf( "Saber style: take out a single saber first\n" );
		return;
	}
	if ( style == current ) {
		ss.target = 0;
		return;
	}
	// a cycle just sent is still waited out: the server would ignore the next one
	ss.target = style;
	ss.cycles = 0;
}

// saberAttackCycle: qfalse leaves it to the server's own cycle
qboolean CL_SaberStyle_Cycle( void ) {
	int list[SS_TAVION], n = SS_List( list ), from, i;

	if ( !n )
		return qfalse;
	from = ss.target ? ss.target : SS_Current();
	if ( !from )
		return qfalse;

	// the one after it in the list; a style not in it goes on to the next one that is
	for ( i = 0; i < n; i++ ) {
		if ( list[i] == from ) {
			SS_GoTo( list[( i + 1 ) % n] );
			return qtrue;
		}
	}
	for ( int step = 1; step < SS_TAVION; step++ ) {
		const int style = ( from - 1 + step ) % SS_TAVION + 1;

		for ( i = 0; i < n; i++ ) {
			if ( list[i] == style ) {
				SS_GoTo( style );
				return qtrue;
			}
		}
	}
	return qtrue;
}

void CL_SaberStyle_Frame( void ) {
	int current;

	if ( !ss.target )
		return;
	current = SS_Current();
	if ( !current || current == ss.target ) {
		ss.target = 0;
		return;
	}
	if ( ss.sentTime && cls.realtime - ss.sentTime < SS_CYCLE_GAP )
		return;
	// the last one hasn't shown yet; during an attack the server holds it until the attack ends
	if ( ss.sentTime && current == ss.levelAtSend && ( cls.realtime - ss.sentTime < SS_WAIT_MS || cl.snap.ps.weaponTime > 0 ) )
		return;
	if ( ss.cycles >= SS_MAX_CYCLES ) {
		Com_Printf( "Saber style: couldn't get to %s, the server may not let you use it\n", ssNames[ss.target] );
		ss.target = 0;
		return;
	}
	cl.gcmdSendValue = qtrue;
	cl.gcmdValue = GENCMD_SABERATTACKCYCLE;
	ss.levelAtSend = current;
	ss.sentTime = cls.realtime;
	ss.cycles++;
}

static void SS_PrintList( void ) {
	int list[SS_TAVION], n = SS_List( list );

	if ( !n ) {
		Com_Printf( "saberAttackCycle goes through the server's styles\n" );
		return;
	}
	Com_Printf( "saberAttackCycle goes through:" );
	for ( int i = 0; i < n; i++ )
		Com_Printf( " %s", ssNames[list[i]] );
	Com_Printf( "\n" );
}

static void SS_Styles_f( void ) {
	char buf[MAX_CVAR_VALUE_STRING] = "";
	int list[SS_TAVION], n = 0;

	if ( Cmd_Argc() < 2 ) {
		SS_PrintList();
		Com_Printf( "usage: saberstyles <style> <style> ...   cycle through these: fast, medium, strong, desann, tavion\n"
			"       saberstyles all                   the server's own cycle\n" );
		return;
	}
	if ( !Q_stricmp( Cmd_Argv( 1 ), "all" ) || !Q_stricmp( Cmd_Argv( 1 ), "clear" ) || !Q_stricmp( Cmd_Argv( 1 ), "reset" ) ) {
		Cvar_Set( "cl_saberStyles", "" );
		SS_PrintList();
		return;
	}
	for ( int i = 1; i < Cmd_Argc() && n < SS_TAVION; i++ ) {
		const int style = SS_Parse( Cmd_Argv( i ) );
		qboolean twice = qfalse;

		if ( !style ) {
			Com_Printf( "Saber style: no style \"%s\"; fast, medium, strong, desann or tavion\n", Cmd_Argv( i ) );
			return;
		}
		for ( int j = 0; j < n; j++ ) {
			if ( list[j] == style )
				twice = qtrue;
		}
		if ( twice )
			continue;
		list[n++] = style;
		Q_strcat( buf, sizeof( buf ), va( "%s%s", buf[0] ? " " : "", ssNames[style] ) );
	}
	Cvar_Set( "cl_saberStyles", buf );
	SS_PrintList();
}

static void SS_Style_f( void ) {
	const int style = SS_Parse( Cmd_Argv( 1 ) );

	if ( !style ) {
		const int current = SS_Current();

		if ( current )
			Com_Printf( "Saber style: %s\n", ssNames[current] );
		Com_Printf( "usage: saberstyle <style>   straight to fast, medium, strong, desann or tavion\n" );
		return;
	}
	SS_GoTo( style );
}

void CL_SaberStyle_Init( void ) {
	memset( &ss, 0, sizeof( ss ) );
	cl_saberStyles = Cvar_Get( "cl_saberStyles", "", CVAR_ARCHIVE_ND, "The lightsaber styles saberAttackCycle goes through, in order; empty for all of them" );
	Cmd_AddCommand( "saberstyles", SS_Styles_f, "Set which lightsaber styles saberAttackCycle goes through" );
	Cmd_AddCommand( "saberstyle", SS_Style_f, "Switch straight to a lightsaber style" );
}
