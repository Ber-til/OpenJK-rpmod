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

// cl_thepath.cpp -- The Path on the datapad: JEDI's own game of two Masters and their Padawans on a
// five by five board, moved by Techniques (https://www.jediholo.net/archives/documents-2/the-path/).
// C1B3R deals and checks it (path_rules.py); this draws the board from your side, the Techniques,
// and sends the moves.
//
// The board as C1B3R has it: x 0 to 4 left to right and y 0 to 4 as the first seat sees it, its own
// row y 0; it's drawn turned round for the second seat, so your pawns are always at the bottom.

#include "cl_games.h"

#define TP_SIZE		5

typedef struct {
	const char	*name;
	int			count;
	int			moves[4][2];	// (right, forward) from the pawn, from its player's side
} tpTechnique_t;

static const tpTechnique_t tpTechniques[] = {
	{ "Jung",			2, { { 0, 2 }, { 0, -1 } } },
	{ "Jung Ma",		4, { { -1, 1 }, { 1, 1 }, { -1, -1 }, { 1, -1 } } },
	{ "Shun",			4, { { 1, 1 }, { -1, 0 }, { 1, 0 }, { -1, -1 } } },
	{ "Sai",			4, { { -1, 1 }, { -1, 0 }, { 1, 0 }, { 1, -1 } } },
	{ "Flowing Water",	3, { { 1, 1 }, { 2, 0 }, { -1, -1 } } },
	{ "Dulon",			3, { { -1, 1 }, { -2, 0 }, { 1, -1 } } },
	{ "Shii-Cho",		4, { { -1, 1 }, { 1, 1 }, { -1, 0 }, { 1, 0 } } },
	{ "Makashi",		3, { { -1, 1 }, { 1, 1 }, { 0, -1 } } },
	{ "Soresu",			3, { { 0, 1 }, { 1, 0 }, { 0, -1 } } },
	{ "Ataru",			4, { { -2, 1 }, { 2, 1 }, { -1, -1 }, { 1, -1 } } },
	{ "Shien",			3, { { -1, 1 }, { 1, 0 }, { -1, -1 } } },
	{ "Djem So",		3, { { 1, 1 }, { -1, 0 }, { 1, -1 } } },
	{ "Niman",			3, { { 0, 1 }, { -1, 0 }, { 0, -1 } } },
	{ "Sokan",			3, { { 0, 1 }, { -1, 0 }, { 1, 0 } } },
	{ "Jar'Kai",		3, { { 0, 1 }, { -2, 0 }, { 2, 0 } } },
	{ "Trakata",		3, { { 0, 1 }, { -1, -1 }, { 1, -1 } } },
};
#define TP_NUM_TECHS	( (int)ARRAY_LEN( tpTechniques ) )

static const char * const tpRules[] = {
	"^7A JEDI original: two Jedi, a five by five board.",
	"Each has five pawns on the row nearest them: their Master",
	"in the middle, on their Force Nexus, and four Padawans.",
	"",
	"^7Techniques: five different ones are dealt from sixteen,",
	"two to each player and one shared. Each shows the squares",
	"a pawn may go to from where it stands (the dot), forward up.",
	"",
	"^7A turn: pick one of your two Techniques and move one of",
	"your pawns, Master or Padawan, to a square it shows.",
	"Pawns don't block; you can't leave the board or land on",
	"your own pawn. Landing on the other's pawn takes it.",
	"Then the Technique you used is swapped with the shared one:",
	"the other player can take it after their move.",
	"With no move at all, you only swap one.",
	"",
	"^7Winning:",
	"Mastery of Body: take the other's Master.",
	"Mastery of Spirit: your Master reaches their Nexus.",
	"",
	"Click a pawn, then a lit square: a solid one is your first",
	"Technique's, an outlined one your second's. A turn lasts 90 s,",
	"then a move is made for you. Leaving forfeits.",
};

static struct {
	qboolean	dealt;
	int			turn, seconds, secondsAt, winner;
	char		how[16];
	int			last[4];					// the last move, -1 none
	int			board[TP_SIZE][TP_SIZE];	// 0 empty, else 1 + seat, + 2 for a Master: 1, 2 Padawans, 3, 4 Masters
	char		techs[2][2][32];
	char		shared[32];

	int			selX, selY;					// the pawn picked, on the board; -1 none
	int			oldTurn, oldWinner;
} tp;

static const float tpSeat0[4] = { 0.35f, 0.75f, 1.0f, 1.0f };
static const float tpSeat1[4] = { 1.0f, 0.45f, 0.35f, 1.0f };
static const float tpLast[4] = { 1.0f, 1.0f, 1.0f, 0.12f };

static const tpTechnique_t *TP_Technique( const char *name ) {
	for ( int i = 0; i < TP_NUM_TECHS; i++ ) {
		if ( !Q_stricmp( tpTechniques[i].name, name ) )
			return &tpTechniques[i];
	}
	return NULL;
}

static int TP_Owner( int x, int y ) {
	return tp.board[x][y] ? ( tp.board[x][y] - 1 ) & 1 : -1;
}

// where a seat's pawn at x, y goes by a Technique: on the board, and not onto its own
static int TP_Targets( int seat, const char *tech, int x, int y, int out[4][2] ) {
	const tpTechnique_t *t = TP_Technique( tech );
	const int s = seat == 0 ? 1 : -1;
	int n = 0;

	if ( !t )
		return 0;
	for ( int i = 0; i < t->count; i++ ) {
		const int tx = x + t->moves[i][0] * s, ty = y + t->moves[i][1] * s;

		if ( tx < 0 || ty < 0 || tx >= TP_SIZE || ty >= TP_SIZE || TP_Owner( tx, ty ) == seat )
			continue;
		out[n][0] = tx;
		out[n][1] = ty;
		n++;
	}
	return n;
}

static qboolean TP_AnyMove( int seat ) {
	for ( int x = 0; x < TP_SIZE; x++ ) {
		for ( int y = 0; y < TP_SIZE; y++ ) {
			int out[4][2];

			if ( TP_Owner( x, y ) == seat && ( TP_Targets( seat, tp.techs[seat][0], x, y, out ) || TP_Targets( seat, tp.techs[seat][1], x, y, out ) ) )
				return qtrue;
		}
	}
	return qfalse;
}

/*
===============================================================================

Reading what C1B3R says

===============================================================================
*/

static void TP_Reset( void ) {
	tp.dealt = qfalse;
	memset( tp.board, 0, sizeof( tp.board ) );
	tp.winner = -1;
}

static void TP_Line( char **f, int n ) {
	if ( n >= 6 && !strcmp( f[0], "path" ) ) {
		tp.dealt = qtrue;
		tp.turn = atoi( f[1] );
		tp.seconds = atoi( f[2] );
		tp.secondsAt = cls.realtime;
		tp.winner = atoi( f[3] );
		Q_strncpyz( tp.how, f[4], sizeof( tp.how ) );
		if ( sscanf( f[5], "%i %i %i %i", &tp.last[0], &tp.last[1], &tp.last[2], &tp.last[3] ) != 4 )
			tp.last[0] = -1;
	} else if ( n >= 5 && !strcmp( f[0], "pawn" ) ) {
		const int x = atoi( f[1] ), y = atoi( f[2] ), seat = atoi( f[3] ) & 1;

		if ( x >= 0 && y >= 0 && x < TP_SIZE && y < TP_SIZE )
			tp.board[x][y] = 1 + seat + ( f[4][0] == 'M' ? 2 : 0 );
	} else if ( n >= 3 && !strcmp( f[0], "tech" ) ) {
		if ( f[1][0] == 's' ) {
			Q_strncpyz( tp.shared, f[2], sizeof( tp.shared ) );
		} else {
			const int seat = atoi( f[1] ) & 1;
			char *comma = strchr( f[2], ',' );

			if ( comma )
				*comma++ = '\0';
			Q_strncpyz( tp.techs[seat][0], f[2], sizeof( tp.techs[0][0] ) );
			Q_strncpyz( tp.techs[seat][1], comma ? comma : "", sizeof( tp.techs[0][1] ) );
		}
	}
}

static void TP_Read( void ) {
	const gmTable_t *t = GM_Table( &gmPath );

	if ( !tp.dealt )
		return;
	if ( tp.selX >= 0 && TP_Owner( tp.selX, tp.selY ) != t->you )
		tp.selX = tp.selY = -1;
	if ( tp.winner >= 0 && tp.oldWinner < 0 )
		GM_Sound( t->you >= 0 && tp.winner == t->you ? GM_SND_WIN : GM_SND_LOSE );
	else if ( tp.turn != tp.oldTurn )
		GM_Sound( t->you >= 0 && tp.turn == t->you ? GM_SND_TURN : GM_SND_PLAY );
	tp.oldTurn = tp.turn;
	tp.oldWinner = tp.winner;
}

/*
===============================================================================

Drawing

===============================================================================
*/

// a Technique's card: its squares on a small 5x5, the dot where the pawn stands; flipped, as the
// player on the far side moves by it
static void TP_DrawTechnique( float x, float y, float w, float h, const char *name, qboolean flipped, qboolean lit, const char *label ) {
	const tpTechnique_t *t = TP_Technique( name );
	const float cw = w / TP_SIZE, ch = h / TP_SIZE;

	DP_Fill( x, y, w, h, dpTile );
	for ( int i = 1; i < TP_SIZE; i++ ) {
		DP_Fill( x + i * cw, y, 1, h, dpButton );
		DP_Fill( x, y + i * ch, w, 1, dpButton );
	}
	DP_Fill( x + 2 * cw + cw * 0.25f, y + 2 * ch + ch * 0.25f, cw * 0.5f, ch * 0.5f, dpWhite );
	for ( int i = 0; t && i < t->count; i++ ) {
		const int s = flipped ? -1 : 1;
		const int col = 2 + t->moves[i][0] * s, row = 2 - t->moves[i][1] * s;

		DP_Fill( x + col * cw + 1, y + row * ch + 1, cw - 2, ch - 2, dpText );
	}
	DP_Frame( x, y, w, h, lit ? dpWhite : dpGlow );
	DP_Text( x - DP_Wide( 10 ), y + h + 1, w + DP_Wide( 20 ), name, lit ? dpWhite : dpText, DP_Font(), 0.48f, qtrue );
	if ( label )
		DP_Text( x - DP_Wide( 10 ), y + h + 10, w + DP_Wide( 20 ), label, dpDim, DP_Font(), 0.42f, qtrue );
}

static void TP_Move( int tech, int fx, int fy, int tx, int ty ) {
	GM_Sound( GM_SND_PLAY );
	GM_Move( &gmPath, va( ",\"action\":\"move\",\"tech\":%i,\"from\":[%i,%i],\"to\":[%i,%i]", tech, fx, fy, tx, ty ) );
	tp.selX = tp.selY = -1;
}

static void TP_Draw( const dpRect_t *s, float y, float bottom ) {
	const gmTable_t *t = GM_Table( &gmPath );
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 16 );
	const int me = t->you == 1 ? 1 : 0, them = 1 - me;
	const qboolean over = (qboolean)( tp.winner >= 0 || !strcmp( t->status, "over" ) );
	const qboolean myTurn = (qboolean)( t->you >= 0 && !over && tp.turn == t->you );
	const float card = DP_Wide( 34 ), cardH = 34.0f;
	int targets[2][4][2], numTargets[2] = { 0, 0 };

	if ( !tp.dealt )
		return;

	// whose turn, the clock
	{
		const int left = Q_max( 0, tp.seconds - ( cls.realtime - tp.secondsAt ) / 1000 );
		const char *how = !strcmp( tp.how, "body" ) ? "Mastery of Body" : !strcmp( tp.how, "spirit" ) ? "Mastery of Spirit" : "the other left";
		const char *who = over ? ( tp.winner >= 0 ? va( "%s wins: %s", t->seats[tp.winner], how ) : "It's over" )
			: myTurn ? "Your turn" : va( "%s's turn", t->seats[tp.turn & 1] );

		DP_Text( x, y, w, va( "%s%s", who, over ? "" : va( "   %is", left ) ), over || myTurn ? dpWhite : dpText, DP_Font(), 0.62f, qfalse );
	}
	y += 14;

	// theirs, at the top: as they move by them, turned round
	DP_Text( x, y + cardH * 0.4f, DP_Wide( 70 ), t->seats[them], them == 0 ? tpSeat0 : tpSeat1, DP_Font(), 0.55f, qfalse );
	for ( int i = 0; i < 2; i++ )
		TP_DrawTechnique( x + DP_Wide( 80 ) + i * ( card + DP_Wide( 22 ) ), y, card, cardH, tp.techs[them][i], qtrue, qfalse, NULL );
	y += cardH + 14;

	// the pawn picked: where each of our Techniques takes it
	if ( myTurn && tp.selX >= 0 ) {
		for ( int i = 0; i < 2; i++ )
			numTargets[i] = TP_Targets( me, tp.techs[me][i], tp.selX, tp.selY, targets[i] );
	}

	// the board, our side at the bottom; the shared Technique beside it
	{
		const float cell = DP_Wide( 30 ), cellH = 30.0f, bx = x, by = y;

		for ( int row = 0; row < TP_SIZE; row++ ) {
			for ( int col = 0; col < TP_SIZE; col++ ) {
				// the square's place on the board, as we see it
				const int bxs = me == 0 ? col : TP_SIZE - 1 - col, bys = me == 0 ? TP_SIZE - 1 - row : row;
				const float cx = bx + col * cell, cy = by + row * cellH;
				const int pawn = tp.board[bxs][bys];
				int how = -1;

				DP_Fill( cx, cy, cell, cellH, ( row + col ) & 1 ? dpTile : dpButton );
				if ( tp.last[0] >= 0 && ( ( tp.last[0] == bxs && tp.last[1] == bys ) || ( tp.last[2] == bxs && tp.last[3] == bys ) ) )
					DP_Fill( cx, cy, cell, cellH, tpLast );
				// a Nexus: the middle of each back row
				if ( bxs == 2 && ( bys == 0 || bys == TP_SIZE - 1 ) )
					DP_Frame( cx + 2, cy + 2, cell - 4, cellH - 4, bys == 0 ? tpSeat0 : tpSeat1 );
				if ( pawn ) {
					const float *color = ( ( pawn - 1 ) & 1 ) ? tpSeat1 : tpSeat0;

					if ( pawn >= 3 ) {
						DP_Fill( cx + cell * 0.18f, cy + cellH * 0.18f, cell * 0.64f, cellH * 0.64f, color );
						DP_Text( cx, cy + cellH * 0.5f - 6, cell, "M", dpScreen, DP_TitleFont(), 0.55f, qtrue );
					} else {
						DP_Fill( cx + cell * 0.3f, cy + cellH * 0.3f, cell * 0.4f, cellH * 0.4f, color );
					}
				}
				if ( tp.selX == bxs && tp.selY == bys )
					DP_Frame( cx, cy, cell, cellH, dpWhite );
				for ( int i = 0; i < 2 && how < 0; i++ ) {
					for ( int k = 0; k < numTargets[i]; k++ ) {
						if ( targets[i][k][0] == bxs && targets[i][k][1] == bys )
							how = i;
					}
				}
				if ( how == 0 )
					DP_Fill( cx + cell * 0.38f, cy + cellH * 0.38f, cell * 0.24f, cellH * 0.24f, dpWhite );
				else if ( how == 1 )
					DP_Frame( cx + cell * 0.32f, cy + cellH * 0.32f, cell * 0.36f, cellH * 0.36f, dpWhite );
				DP_Frame( cx, cy, cell, cellH, dpGlow );

				if ( myTurn && DP_Clicked( cx, cy, cell, cellH ) ) {
					if ( how >= 0 ) {
						TP_Move( how, tp.selX, tp.selY, bxs, bys );
					} else if ( TP_Owner( bxs, bys ) == me ) {
						GM_Sound( GM_SND_CLICK );
						tp.selX = bxs;
						tp.selY = bys;
					} else {
						tp.selX = tp.selY = -1;
					}
				}
			}
		}
		// the shared one, beside the board, as we'd move by it
		TP_DrawTechnique( bx + cell * TP_SIZE + DP_Wide( 14 ), by + cellH * 1.4f, card, cardH, tp.shared, qfalse, qfalse, "shared" );
		y += TP_SIZE * cellH + 4;
	}

	// ours, at the bottom
	DP_Text( x, y + cardH * 0.4f, DP_Wide( 70 ), t->you >= 0 ? va( "%s (you)", t->seats[me] ) : t->seats[me], me == 0 ? tpSeat0 : tpSeat1, DP_Font(), 0.55f, qfalse );
	for ( int i = 0; i < 2; i++ )
		TP_DrawTechnique( x + DP_Wide( 80 ) + i * ( card + DP_Wide( 22 ) ), y, card, cardH, tp.techs[me][i], qfalse, (qboolean)( numTargets[i] > 0 ),
			t->you >= 0 ? ( i == 0 ? "solid" : "outlined" ) : NULL );
	y += cardH + 22;

	// what happened lately
	if ( y + 24 < bottom ) {
		DP_Fill( x, y, w, 1, dpGlow );
		GM_DrawEvents( &gmPath, x, y + 2, w, Q_min( 3, (int)( ( bottom - y - 4 ) / 11 ) ) );
	}

	// nothing to move: a Technique swapped only
	if ( myTurn && !TP_AnyMove( me ) ) {
		const float half = ( w - DP_Wide( 4 ) ) * 0.5f, sy = bottom - DP_CTRL_H;

		for ( int i = 0; i < 2; i++ ) {
			if ( DP_Button( x + i * ( half + DP_Wide( 4 ) ), sy, half, va( "Only swap %s", tp.techs[me][i] ), qfalse ) )
				GM_Move( &gmPath, va( ",\"action\":\"pass\",\"tech\":%i", i ) );
		}
	}
}

// thepath move <technique 0|1> <from x> <from y> <to x> <to y> | swap <technique 0|1>
static qboolean TP_Command( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !Q_stricmp( arg, "move" ) && Cmd_Argc() > 6 ) {
		TP_Move( atoi( Cmd_Argv( 2 ) ), atoi( Cmd_Argv( 3 ) ), atoi( Cmd_Argv( 4 ) ), atoi( Cmd_Argv( 5 ) ), atoi( Cmd_Argv( 6 ) ) );
		return qtrue;
	}
	if ( !Q_stricmp( arg, "swap" ) && Cmd_Argc() > 2 ) {
		GM_Move( &gmPath, va( ",\"action\":\"pass\",\"tech\":%i", atoi( Cmd_Argv( 2 ) ) ) );
		return qtrue;
	}
	return qfalse;
}

static void TP_Reload( void ) {
	tp.selX = tp.selY = -1;
}

const gmGame_t gmPath = {
	"path", "THE PATH", "A JEDI original for two: Masters, Padawans, Techniques.",
	tpRules, (int)ARRAY_LEN( tpRules ),
	TP_Reset, TP_Line, TP_Read, TP_Draw, TP_Reload, TP_Command,
	"move <technique 0|1> <from x> <from y> <to x> <to y> | swap <technique 0|1>    (x, y as the first seat sees the board)"
};
