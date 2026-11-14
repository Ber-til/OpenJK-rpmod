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

// cl_games.h -- the datapad's games besides Pazaak (WANGA, The Path): what they share, in cl_games.cpp,
// and what each game gives it

#pragma once

#include "cl_datapad.h"

#define GM_MAX_SEATS	10
#define GM_MAX_EVENTS	6

typedef enum {
	GM_SND_CLICK, GM_SND_DEAL, GM_SND_PLAY, GM_SND_TURN, GM_SND_WIN, GM_SND_LOSE, GM_NUM_SOUNDS
} gmSound_t;

// a game: its word in C1B3R's /v2/games/<word>, how it's called and told, and what it draws
typedef struct gmGame_s {
	const char	*word;
	const char	*title;
	const char	*tagline;
	const char	* const *rules;
	int			numRules;
	void		( *reset )( void );						// a state coming in: the game's own lines forgotten
	void		( *line )( char **field, int count );	// one of the game's own lines
	void		( *read )( void );						// the state read: its sounds
	void		( *draw )( const dpRect_t *s, float y, float bottom );	// the game dealt, from y to bottom
	void		( *reload )( void );					// the art and sounds, as the app is shown
	qboolean	( *command )( void );					// its own moves from the console (Cmd_Argv 1 on): qtrue if one
	const char	*usage;									// they, for the usage line
} gmGame_t;

// the table we're at, as C1B3R last said
typedef struct {
	qboolean	inLobby;
	char		lobbyId[16], status[16], name[64];
	int			you;						// our seat, -1 watching
	int			numSeats, maxSeats;
	char		seats[GM_MAX_SEATS][64];
	char		events[GM_MAX_EVENTS][128];
	int			numEvents;
} gmTable_t;

extern const gmGame_t gmWanga, gmPath;

const gmTable_t	*GM_Table( const gmGame_t *game );
void		GM_Move( const gmGame_t *game, const char *fields );		// ,"action":"..." and the rest
void		GM_Sound( gmSound_t s );
void		GM_Message( const gmGame_t *game, const char *text, qboolean bad );
qboolean	GM_Playing( const gmGame_t *game );		// dealt and not over
void		GM_DrawEvents( const gmGame_t *game, float x, float y, float w, int lines );
qhandle_t	GM_Art( const char *path );				// a picture of the player's own (png, tga, jpg), 0 none

// the datapad's hooks: which is GM_WANGA or GM_PATH
#define GM_WANGA	0
#define GM_PATH		1
void		CL_Games_Draw( int which, const dpRect_t *screen );
void		CL_Games_Shown( int which, qboolean shown );
qboolean	CL_Games_Key( int which, int key );
void		CL_Games_Char( int which, int ch );
void		CL_Games_Frame( void );
void		CL_Games_Init( void );
