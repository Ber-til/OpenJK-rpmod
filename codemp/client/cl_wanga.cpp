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

// cl_wanga.cpp -- WANGA on the datapad: a cantina card game of the backworlds, 2 to 10 players at a
// table (UNO's rules). C1B3R deals and checks it (wanga_rules.py); this draws the table and your
// hand and sends the moves. The four syndicates are its colours: Crimson Dawn, the Pyke Syndicate,
// Black Sun and the Hutt Cartel.
//
// The cards are drawn in the datapad's style; art of a player's own in a pk3 is drawn under them
// instead: wanga/cards/<crimson|pyke|blacksun|hutt|wild|back>.png (or .tga, .jpg).

#include "cl_games.h"

#define WN_MAX_HAND		108
#define WN_CARD_W		22.0f
#define WN_CARD_H		32.0f

static const char *wnColorNames[4] = { "Crimson Dawn", "Pyke Syndicate", "Black Sun", "Hutt Cartel" };
static const char *wnColorCodes = "RBGY";
static const float wnColors[4][4] = {
	{ 0.78f, 0.13f, 0.14f, 1.0f },		// Crimson Dawn
	{ 0.20f, 0.42f, 0.88f, 1.0f },		// Pyke Syndicate
	{ 0.16f, 0.58f, 0.30f, 1.0f },		// Black Sun
	{ 0.86f, 0.66f, 0.12f, 1.0f },		// Hutt Cartel
};
static const float wnWild[4] = { 0.12f, 0.12f, 0.14f, 1.0f };
static const char *wnArtNames[6] = { "crimson", "pyke", "blacksun", "hutt", "wild", "back" };

static const char * const wnRules[] = {
	"^7A cantina game of the backworlds, 2 to 10 players.",
	"Seven cards each; get rid of all of yours first.",
	"",
	"^7The syndicates are the colours:",
	"Crimson Dawn (red), Pyke Syndicate (blue),",
	"Black Sun (green), Hutt Cartel (gold).",
	"Each has a 0, two each of 1 to 9, and two of each action.",
	"",
	"^7A turn: play a card on the discards that matches",
	"the syndicate asked for, the number, or the action.",
	"Or draw one: if it can be played, play it or keep it.",
	"",
	"^7The actions:",
	"Ambush: the next player loses their turn.",
	"Double-Cross: play turns the other way (with two players,",
	"an Ambush).",
	"Pay Up +2: the next player draws two and loses their turn.",
	"Smuggler's Run: wild; you name the syndicate asked for.",
	"Bounty +4: wild, and the next player draws four and loses",
	"their turn. Only when you hold nothing of the syndicate.",
	"",
	"^7WANGA!: press it as you play your second-to-last card.",
	"Down to one without calling it, anyone can Catch you before",
	"the next player's move: you draw two.",
	"",
	"A turn lasts 60 s, then you draw one and pass.",
	"The first with no cards wins. Leaving forfeits.",
};

static struct {
	char		top[4];
	int			color;					// the syndicate asked for, -1 any
	int			direction, turn, seconds, secondsAt, winner, drawn, catchable;
	int			numPlayers;
	int			cards[GM_MAX_SEATS];
	qboolean	called[GM_MAX_SEATS], out[GM_MAX_SEATS];
	char		hand[WN_MAX_HAND][4];
	int			handCount;
	qboolean	dealt;

	int			picked;					// a wild picked, waiting for its syndicate; -1 none
	int			handScroll;				// the hand's first row shown
	int			oldTurn, oldWinner, oldTop;
	qhandle_t	art[6];
} wn;

static int WN_Color( char c ) {
	const char *at = strchr( wnColorCodes, c );

	return c && at ? (int)( at - wnColorCodes ) : -1;
}

static qboolean WN_IsWild( const char *card ) {
	return (qboolean)( card[0] == 'W' );
}

// the short label on a card
static const char *WN_Label( const char *card ) {
	if ( !strcmp( card, "W" ) )
		return "RUN";
	if ( !strcmp( card, "W4" ) )
		return "+4";
	switch ( card[1] ) {
	case 'A':	return "AMB";
	case 'X':	return "DBL";
	case 'P':	return "+2";
	default:	return card + 1;
	}
}

// its whole name, for the one on top
static const char *WN_Name( const char *card ) {
	if ( !strcmp( card, "W" ) )
		return "Smuggler's Run";
	if ( !strcmp( card, "W4" ) )
		return "Bounty +4";
	switch ( card[1] ) {
	case 'A':	return "Ambush";
	case 'X':	return "Double-Cross";
	case 'P':	return "Pay Up +2";
	default:	return va( "%s", card + 1 );
	}
}

// what can be played now, as the rules have it (for lighting them: C1B3R checks)
static qboolean WN_Playable( const char *card ) {
	if ( !strcmp( card, "W" ) )
		return qtrue;
	if ( !strcmp( card, "W4" ) ) {
		if ( wn.color < 0 )
			return qtrue;
		for ( int i = 0; i < wn.handCount; i++ ) {
			if ( !WN_IsWild( wn.hand[i] ) && WN_Color( wn.hand[i][0] ) == wn.color )
				return qfalse;
		}
		return qtrue;
	}
	if ( wn.color < 0 )
		return qtrue;
	return (qboolean)( WN_Color( card[0] ) == wn.color || ( !WN_IsWild( wn.top ) && !strcmp( card + 1, wn.top + 1 ) ) );
}

/*
===============================================================================

Reading what C1B3R says

===============================================================================
*/

static void WN_Reset( void ) {
	if ( !wn.dealt )
		wn.picked = -1;
	wn.dealt = qfalse;
	wn.numPlayers = 0;
	wn.handCount = 0;
	wn.winner = wn.drawn = wn.catchable = -1;
}

static void WN_Line( char **f, int n ) {
	if ( n >= 9 && !strcmp( f[0], "table" ) ) {
		wn.dealt = qtrue;
		Q_strncpyz( wn.top, f[1], sizeof( wn.top ) );
		wn.color = WN_Color( f[2][0] );
		wn.direction = atoi( f[3] );
		wn.turn = atoi( f[4] );
		wn.seconds = atoi( f[5] );
		wn.secondsAt = cls.realtime;
		wn.winner = atoi( f[6] );
		wn.drawn = atoi( f[7] );
		wn.catchable = atoi( f[8] );
	} else if ( n >= 5 && !strcmp( f[0], "player" ) ) {
		const int i = atoi( f[1] );

		if ( i >= 0 && i < GM_MAX_SEATS ) {
			wn.cards[i] = atoi( f[2] );
			wn.called[i] = (qboolean)( atoi( f[3] ) != 0 );
			wn.out[i] = (qboolean)( atoi( f[4] ) != 0 );
			wn.numPlayers = Q_max( wn.numPlayers, i + 1 );
		}
	} else if ( n >= 2 && !strcmp( f[0], "hand" ) ) {
		char *c = f[1];

		wn.handCount = 0;
		while ( c && *c && wn.handCount < WN_MAX_HAND ) {
			char *next = strchr( c, ',' );

			if ( next )
				*next++ = '\0';
			Q_strncpyz( wn.hand[wn.handCount++], c, sizeof( wn.hand[0] ) );
			c = next;
		}
	}
}

static void WN_Read( void ) {
	const gmTable_t *t = GM_Table( &gmWanga );
	const int top = wn.dealt ? ( wn.top[0] << 8 ) | wn.top[1] : 0;

	if ( !wn.dealt )
		return;
	if ( wn.picked >= wn.handCount || ( wn.drawn >= 0 && wn.picked >= 0 && wn.picked != wn.drawn ) || wn.turn != GM_Table( &gmWanga )->you )
		wn.picked = -1;
	if ( wn.winner >= 0 && wn.oldWinner < 0 )
		GM_Sound( t->you >= 0 && wn.winner == t->you ? GM_SND_WIN : GM_SND_LOSE );
	else if ( wn.turn != wn.oldTurn && t->you >= 0 && wn.turn == t->you )
		GM_Sound( GM_SND_TURN );
	else if ( top != wn.oldTop )
		GM_Sound( GM_SND_PLAY );
	wn.oldTurn = wn.turn;
	wn.oldWinner = wn.winner;
	wn.oldTop = top;
}

static void WN_Reload( void ) {
	wn.picked = -1;
	for ( int i = 0; i < 6; i++ )
		wn.art[i] = GM_Art( va( "wanga/cards/%s", wnArtNames[i] ) );
}

/*
===============================================================================

Drawing

===============================================================================
*/

static void WN_DrawBack( float x, float y, float w, float h ) {
	if ( wn.art[5] ) {
		re->SetColor( NULL );
		re->DrawStretchPic( x, y, w, h, 0, 0, 1, 1, wn.art[5] );
	} else {
		DP_Fill( x, y, w, h, wnWild );
		for ( int i = 0; i < 4; i++ )
			DP_Fill( x + w * 0.2f + i * w * 0.15f, y + h * 0.3f, w * 0.1f, h * 0.4f, wnColors[i] );
	}
	DP_Frame( x, y, w, h, dpGlow );
}

// a card: its syndicate's colour (a wild's four), its label; lit when it can be played
static void WN_DrawCard( float x, float y, float w, float h, const char *card, qboolean lit, float scale ) {
	const int c = WN_Color( card[0] );
	const qhandle_t art = WN_IsWild( card ) ? wn.art[4] : c >= 0 ? wn.art[c] : 0;

	if ( art ) {
		re->SetColor( NULL );
		re->DrawStretchPic( x, y, w, h, 0, 0, 1, 1, art );
	} else if ( WN_IsWild( card ) ) {
		DP_Fill( x, y, w, h, wnWild );
		DP_Fill( x + 2, y + 2, ( w - 4 ) * 0.5f, ( h - 4 ) * 0.5f, wnColors[0] );
		DP_Fill( x + w * 0.5f, y + 2, ( w - 4 ) * 0.5f, ( h - 4 ) * 0.5f, wnColors[1] );
		DP_Fill( x + 2, y + h * 0.5f, ( w - 4 ) * 0.5f, ( h - 4 ) * 0.5f, wnColors[2] );
		DP_Fill( x + w * 0.5f, y + h * 0.5f, ( w - 4 ) * 0.5f, ( h - 4 ) * 0.5f, wnColors[3] );
		DP_Fill( x + w * 0.18f, y + h * 0.32f, w * 0.64f, h * 0.36f, wnWild );
	} else {
		DP_Fill( x, y, w, h, c >= 0 ? wnColors[c] : wnWild );
		DP_Fill( x + w * 0.15f, y + h * 0.28f, w * 0.7f, h * 0.44f, wnWild );
	}
	DP_Text( x, y + h * 0.5f - 6 * scale, w, WN_Label( card ), dpWhite, DP_TitleFont(), 0.5f * scale, qtrue );
	DP_Frame( x, y, w, h, lit ? dpWhite : dpGlow );
	if ( lit )
		DP_Frame( x - 1, y - 1, w + 2, h + 2, dpWhite );
}

static void WN_Play( int index, int color ) {
	if ( color >= 0 )
		GM_Move( &gmWanga, va( ",\"action\":\"play\",\"card\":%i,\"color\":\"%c\"", index, wnColorCodes[color] ) );
	else
		GM_Move( &gmWanga, va( ",\"action\":\"play\",\"card\":%i", index ) );
	wn.picked = -1;
}

static void WN_Draw( const dpRect_t *s, float y, float bottom ) {
	const gmTable_t *t = GM_Table( &gmWanga );
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 16 );
	const qboolean over = (qboolean)( wn.winner >= 0 || !strcmp( t->status, "over" ) );
	const qboolean myTurn = (qboolean)( t->you >= 0 && !over && wn.turn == t->you && !wn.out[t->you] );

	if ( !wn.dealt )
		return;

	// whose turn, the clock, the way it goes
	{
		const int left = Q_max( 0, wn.seconds - ( cls.realtime - wn.secondsAt ) / 1000 );
		const char *who = over ? ( wn.winner >= 0 ? va( "%s wins!", t->seats[wn.winner] ) : "It's over" )
			: myTurn ? "Your turn" : va( "%s's turn", t->seats[wn.turn] );

		DP_Text( x, y, w, va( "%s   %s%s", t->name, who, over ? "" : va( "   %is   %s", left, wn.direction > 0 ? ">>" : "<<" ) ),
			over || myTurn ? dpWhite : dpText, DP_Font(), 0.62f, qfalse );
	}
	y += 14;

	// the players: two columns, five rows
	{
		const float cw = ( w - DP_Wide( 4 ) ) * 0.5f;

		for ( int i = 0; i < wn.numPlayers; i++ ) {
			const float px = x + ( i / 5 ) * ( cw + DP_Wide( 4 ) ), py = y + ( i % 5 ) * 11;
			const qboolean turn = (qboolean)( !over && wn.turn == i );
			const char *tag = wn.out[i] ? " (left)" : wn.cards[i] == 1 && wn.called[i] ? " WANGA!" : "";

			if ( turn )
				DP_Fill( px, py, cw, 11, dpButton );
			DP_Text( px + 2, py, cw - DP_Wide( 22 ), va( "%s%s%s", t->seats[i], i == t->you ? " (you)" : "", tag ),
				wn.out[i] ? dpDim : turn ? dpWhite : dpText, DP_Font(), 0.55f, qfalse );
			DP_Text( px + cw - DP_Wide( 20 ), py, DP_Wide( 18 ), va( "%i", wn.cards[i] ), wn.cards[i] <= 1 ? dpWhite : dpDim, DP_Font(), 0.55f, qfalse );
		}
		y += 5 * 11 + 6;
	}

	// the discards and the draw pile
	{
		const float bw = DP_Wide( WN_CARD_W * 1.6f ), bh = WN_CARD_H * 1.6f, cx = s->x + s->w * 0.5f;

		WN_DrawBack( cx - bw - DP_Wide( 10 ), y, bw, bh );
		WN_DrawCard( cx + DP_Wide( 10 ), y, bw, bh, wn.top, qfalse, 1.5f );
		DP_Text( cx + DP_Wide( 14 ) + bw, y + 4, s->x + s->w - cx - bw - DP_Wide( 20 ), WN_Name( wn.top ), dpText, DP_Font(), 0.56f, qfalse );
		if ( wn.color >= 0 ) {
			DP_Fill( cx + DP_Wide( 14 ) + bw, y + 20, DP_Wide( 10 ), 10, wnColors[wn.color] );
			DP_Text( cx + DP_Wide( 28 ) + bw, y + 19, s->x + s->w - cx - bw - DP_Wide( 34 ), wnColorNames[wn.color], dpDim, DP_Font(), 0.52f, qfalse );
		} else {
			DP_Text( cx + DP_Wide( 14 ) + bw, y + 19, s->x + s->w - cx - bw - DP_Wide( 20 ), "any syndicate", dpDim, DP_Font(), 0.52f, qfalse );
		}
		y += bh + 6;
	}

	// what happened lately
	DP_Fill( x, y, w, 1, dpGlow );
	GM_DrawEvents( &gmWanga, x, y + 2, w, 2 );
	y += 26;

	if ( t->you < 0 ) {
		DP_Text( x, y, w, "Watching", dpDim, DP_Font(), 0.62f, qfalse );
		return;
	}

	// a wild picked: the syndicate it asks for
	if ( wn.picked >= 0 && myTurn ) {
		const float sw = ( w - DP_Wide( 9 ) ) / 4.0f;

		DP_Text( x, y, w, "Name the syndicate:", dpText, DP_Font(), 0.6f, qfalse );
		y += 12;
		for ( int i = 0; i < 4; i++ ) {
			const float bx = x + i * ( sw + DP_Wide( 3 ) );

			DP_Fill( bx, y, sw, DP_CTRL_H, wnColors[i] );
			DP_Frame( bx, y, sw, DP_CTRL_H, DP_InRect( bx, y, sw, DP_CTRL_H ) ? dpWhite : dpGlow );
			DP_Text( bx, y + 2, sw, wnColorNames[i], dpWhite, DP_Font(), 0.48f, qtrue );
			if ( DP_Clicked( bx, y, sw, DP_CTRL_H ) )
				WN_Play( wn.picked, i );
		}
		y += DP_CTRL_H + 6;
	}

	// your hand: rows of cards, the playable ones lit on your turn; a click plays one
	{
		const float cw = DP_Wide( WN_CARD_W ), ch = WN_CARD_H, gap = DP_Wide( 3 );
		const int perRow = Q_max( 1, (int)( ( w + gap ) / ( cw + gap ) ) );
		const float buttons = DP_CTRL_H + 6;
		const int rows = Q_max( 1, (int)( ( bottom - y - buttons ) / ( ch + 4 ) ) );
		const int totalRows = ( wn.handCount + perRow - 1 ) / perRow;

		wn.handScroll = Com_Clampi( 0, Q_max( 0, totalRows - rows ), wn.handScroll );
		for ( int r = 0; r < rows; r++ ) {
			for ( int c = 0; c < perRow; c++ ) {
				const int i = ( wn.handScroll + r ) * perRow + c;
				const float cx = x + c * ( cw + gap ), cy = y + r * ( ch + 4 );
				qboolean can;

				if ( i >= wn.handCount )
					break;
				can = (qboolean)( myTurn && ( wn.drawn < 0 || wn.drawn == i ) && WN_Playable( wn.hand[i] ) );
				WN_DrawCard( cx, cy, cw, ch, wn.hand[i], (qboolean)( can && ( DP_InRect( cx, cy, cw, ch ) || wn.picked == i ) ), 1.0f );
				if ( myTurn && !can )
					DP_Fill( cx, cy, cw, ch, dpScreen );		// dimmed
				if ( can && DP_Clicked( cx, cy, cw, ch ) ) {
					GM_Sound( GM_SND_CLICK );
					if ( WN_IsWild( wn.hand[i] ) )
						wn.picked = wn.picked == i ? -1 : i;
					else
						WN_Play( i, -1 );
				}
			}
		}
		if ( totalRows > rows ) {
			const float ax = x + w - DP_Wide( 14 );

			if ( DP_Button( ax, y, DP_Wide( 14 ), "^", qfalse ) )
				wn.handScroll--;
			if ( DP_Button( ax, y + rows * ( ch + 4 ) - DP_CTRL_H - 4, DP_Wide( 14 ), "v", qfalse ) )
				wn.handScroll++;
		}
		y = bottom - DP_CTRL_H;
	}

	// draw, keep, call and catch
	if ( !over ) {
		const float bw = ( w - DP_Wide( 9 ) ) / 4.0f;
		const qboolean canCall = (qboolean)( !wn.called[t->you] && ( wn.cards[t->you] == 1 || ( wn.cards[t->you] == 2 && myTurn ) ) );
		const qboolean canCatch = (qboolean)( wn.catchable >= 0 && wn.catchable != t->you );

		if ( wn.drawn >= 0 && myTurn ) {
			if ( DP_Button( x, y, bw, "Keep it", qfalse ) )
				GM_Move( &gmWanga, ",\"action\":\"keep\"" );
		} else if ( DP_Button( x, y, bw, "Draw", (qboolean)myTurn ) && myTurn ) {
			GM_Sound( GM_SND_DEAL );
			GM_Move( &gmWanga, ",\"action\":\"draw\"" );
		}
		if ( DP_Button( x + bw + DP_Wide( 3 ), y, bw * 2 + DP_Wide( 3 ), "WANGA!", canCall ) && canCall )
			GM_Move( &gmWanga, ",\"action\":\"call\"" );
		if ( DP_Button( x + ( bw + DP_Wide( 3 ) ) * 3, y, bw, "Catch", canCatch ) && canCatch )
			GM_Move( &gmWanga, ",\"action\":\"catch\"" );
	} else {
		DP_Text( x, y + 2, w, wn.winner == t->you ? "You win!" : wn.winner >= 0 ? va( "%s wins", t->seats[wn.winner] ) : "", dpWhite, DP_Font(), 0.7f, qfalse );
	}
}

// wanga play <card> [R|B|G|Y] | draw | keep | call | catch
static qboolean WN_Command( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !Q_stricmp( arg, "play" ) ) {
		WN_Play( atoi( Cmd_Argv( 2 ) ), Cmd_Argc() > 3 ? WN_Color( toupper( Cmd_Argv( 3 )[0] ) ) : -1 );
		return qtrue;
	}
	if ( !Q_stricmp( arg, "draw" ) || !Q_stricmp( arg, "keep" ) || !Q_stricmp( arg, "call" ) || !Q_stricmp( arg, "catch" ) ) {
		GM_Move( &gmWanga, va( ",\"action\":\"%s\"", Q_strlwr( (char *)va( "%s", arg ) ) ) );
		return qtrue;
	}
	return qfalse;
}

const gmGame_t gmWanga = {
	"wanga", "WANGA", "A cantina card game: first out of cards wins. 2 to 10.",
	wnRules, (int)ARRAY_LEN( wnRules ),
	WN_Reset, WN_Line, WN_Read, WN_Draw, WN_Reload, WN_Command,
	"play <card> [R|B|G|Y] | draw | keep | call | catch"
};
