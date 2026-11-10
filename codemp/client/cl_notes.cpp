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

// cl_notes.cpp -- the datapad's Notes app: notes written and kept by C1B3R (its /v2/notes, see
// OpenJK-rpmod-c1b3r), on the C1B3R session (cl_presence.cpp).
//
// Opened from the home screen, they're your character's own. A datapad item has notes of its own,
// opened from the inventory (or with the use key on one placed in the world, cl_worlditems.cpp):
// the datapad shows its Notes alone then, and one placed in the world can be picked up from there.
//
// A list of the notes, and one opened: its title and its text, of many lines. It's saved a moment
// after the typing stops, and when it's left.

#include <string>
#include <vector>
#include "client.h"
#include "cl_datapad.h"
#include "cl_presence.h"
#include "cl_widgets.h"

#define NT_MAX			200			// as C1B3R keeps
#define NT_TITLE		65			// 64 and the end
#define NT_BODY			8001		// 8000 and the end
#define NT_SAVE_MS		1000		// after the typing stops
#define NT_ROW_H		24.0f
#define NT_MESSAGE_MS	4000

typedef struct {
	int		id;
	int		updated;
	char	title[NT_TITLE];
} ntNote_t;

static struct {
	// whose: your character's (both 0), a datapad item's, or one placed in the world's
	int			item, world;
	char		owner[64];

	qboolean	shown;
	qboolean	known;			// the list asked since it was shown, or changed
	qboolean	refused;
	char		refusal[192];
	ntNote_t	notes[NT_MAX];
	int			num;
	int			scroll;

	// the one opened: 0 none, -1 a new one not saved yet
	int			open;
	qboolean	loading;		// its text being asked
	char		title[NT_TITLE];
	char		body[NT_BODY];
	qboolean	dirty;
	int			editTime;
	int			serial;			// another opened: a save's answer for the one before isn't this one's
	int			focus;			// 0 none, 1 the title, 2 the text
	qboolean	confirmDelete;

	char		message[128];
	int			messageTime;
	qboolean	messageBad;
} nt;

static httpRequest_t ntListHttp, ntGetHttp, ntSaveHttp, ntDelHttp;
static int ntSaveSerial, ntGetFor;

// a note left while the last save was still going: saved after it, in turn
typedef struct {
	int			serial;			// the note it was, while opened
	int			id;				// 0 new: given the number the save before gets, if it was the same note
	std::string	whose, title, body;
} ntQueued_t;
static std::vector<ntQueued_t> ntQueue;

/*
===============================================================================

Talking to C1B3R

===============================================================================
*/

void CL_Notes_Message( const char *text, qboolean bad ) {
	Q_strncpyz( nt.message, text, sizeof( nt.message ) );
	for ( char *nl = strchr( nt.message, '\n' ); nl; nl = NULL )
		*nl = '\0';
	nt.messageTime = cls.realtime;
	nt.messageBad = bad;
}

// the owner's fields: none for your character's
static std::string NT_Whose( void ) {
	if ( nt.world > 0 )
		return va( ",\"world\":%i", nt.world );
	if ( nt.item > 0 )
		return va( ",\"item\":%i", nt.item );
	return "";
}

static qboolean NT_Post( httpRequest_t *req, const char *path, const std::string &fields ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session || req->state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += NT_Whose() + fields + "}";
	return CL_Presence_Post( req, path, body );
}

static void NT_AskList( void ) {
	if ( NT_Post( &ntListHttp, "/v2/notes/list", "" ) )
		nt.known = qtrue;
}

// a title for one that has none: its first line, or Untitled
static void NT_TitleOf( char *out, int size ) {
	int n = 0;

	if ( nt.title[0] ) {
		Q_strncpyz( out, nt.title, size );
		return;
	}
	for ( const char *p = nt.body; *p && *p != '\n' && n < size - 1 && n < 40; p++ )
		out[n++] = *p;
	out[n] = '\0';
	if ( !out[0] )
		Q_strncpyz( out, "Untitled", size );
}

static qboolean NT_SaveSend( const ntQueued_t &q ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session || ntSaveHttp.state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += q.whose + va( ",\"id\":%i,\"title\":", q.id );
	CL_Presence_Json( body, q.title.c_str() );
	body += ",\"body\":";
	CL_Presence_Json( body, q.body.c_str() );
	body += "}";
	if ( !CL_Presence_Post( &ntSaveHttp, "/v2/notes/save", body ) )
		return qfalse;
	ntSaveSerial = q.serial;
	return qtrue;
}

// the one opened saved, if it changed. Busy: left for later (qfalse), or queued when it's being left
static qboolean NT_Save( qboolean leaving ) {
	ntQueued_t q;
	char title[NT_TITLE];

	if ( !nt.dirty || !nt.open )
		return qtrue;
	NT_TitleOf( title, sizeof( title ) );
	q.serial = nt.serial;
	q.id = nt.open > 0 ? nt.open : 0;
	q.whose = NT_Whose();
	q.title = title;
	q.body = nt.body;
	// one at a time: a new one waits for the save that gives it its number
	if ( ntSaveHttp.state == HTTP_BUSY || !ntQueue.empty() || !NT_SaveSend( q ) ) {
		if ( !leaving )
			return qfalse;
		ntQueue.push_back( q );
	}
	nt.dirty = qfalse;
	return qtrue;
}

static void NT_Close( void ) {
	NT_Save( qtrue );
	nt.open = 0;
	nt.focus = 0;
	nt.loading = qfalse;
	nt.confirmDelete = qfalse;
	nt.title[0] = nt.body[0] = '\0';
	nt.serial++;
	WG_EditReset();
}

static void NT_Open( int id ) {
	NT_Close();
	nt.open = id;
	if ( id > 0 ) {
		nt.loading = qtrue;
		ntGetFor = nt.serial;
		if ( !NT_Post( &ntGetHttp, "/v2/notes/get", va( ",\"id\":%i", id ) ) ) {
			nt.open = 0;
			nt.loading = qfalse;
			CL_Notes_Message( "Wait a moment and try again", qtrue );
		}
	} else {
		nt.focus = 1;
	}
}

// the owner of the notes shown: those shown before saved first
void CL_Notes_Owner( int item, int world, const char *name ) {
	if ( nt.item == item && nt.world == world )
		return;
	NT_Close();
	nt.item = item;
	nt.world = world;
	Q_strncpyz( nt.owner, name ? name : "", sizeof( nt.owner ) );
	nt.num = 0;
	nt.scroll = 0;
	nt.known = qfalse;
	nt.refused = qfalse;
}

// the placed one picked up: an item of yours now, its notes with it
void CL_Notes_PickedUp( int item ) {
	if ( !nt.world )
		return;
	NT_Save( qtrue );
	nt.world = 0;
	nt.item = item;
	nt.known = qfalse;
	CL_Notes_Message( "Picked up: it's in your inventory", qfalse );
}

void CL_Notes_Shown( qboolean shown ) {
	if ( shown ) {
		nt.shown = qtrue;
		nt.known = qfalse;
		return;
	}
	nt.shown = qfalse;
	NT_Close();
}

void CL_Notes_Disconnect( void ) {
	nt.dirty = qfalse;
	NT_Close();
	ntQueue.clear();
	memset( &nt, 0, sizeof( nt ) );
}

// a line's tab-separated fields: how many
static int NT_Split( char *line, char **field, int max ) {
	int n = 0;

	for ( char *f = line; f && n < max; n++ ) {
		field[n] = f;
		f = strchr( f, '\t' );
		if ( f )
			*f++ = '\0';
	}
	return n;
}

// note <id> <updated> <title>, after ok
static void NT_ReadList( const char *answer ) {
	std::string all( answer );
	size_t at = 0;

	nt.num = 0;
	while ( at < all.size() && nt.num < NT_MAX ) {
		size_t end = all.find( '\n', at );
		char line[512], *f[4];

		if ( end == std::string::npos )
			end = all.size();
		Q_strncpyz( line, all.substr( at, end - at ).c_str(), sizeof( line ) );
		at = end + 1;
		if ( strncmp( line, "note\t", 5 ) || NT_Split( line, f, 4 ) < 4 )
			continue;
		ntNote_t *n = &nt.notes[nt.num++];

		n->id = atoi( f[1] );
		n->updated = atoi( f[2] );
		Q_strncpyz( n->title, CL_Presence_Unfield( f[3] ).c_str(), sizeof( n->title ) );
	}
}

// ok <id> <updated> <title>, then body <text>
static void NT_ReadNote( const char *answer ) {
	const char *body = strstr( answer, "\nbody\t" );
	char first[512], *f[4];
	int n = 0;

	while ( answer[n] && answer[n] != '\n' && n < (int)sizeof( first ) - 1 ) {
		first[n] = answer[n];
		n++;
	}
	first[n] = '\0';
	if ( NT_Split( first, f, 4 ) >= 4 )
		Q_strncpyz( nt.title, CL_Presence_Unfield( f[3] ).c_str(), sizeof( nt.title ) );
	if ( body ) {
		std::string text = CL_Presence_Unfield( body + 6 );
		size_t nl = text.find_last_not_of( "\r\n" );

		text = nl == std::string::npos ? "" : text.substr( 0, nl + 1 );
		Q_strncpyz( nt.body, text.c_str(), sizeof( nt.body ) );
	}
}

static const char *NT_Answer( const httpRequest_t *req ) {
	return req->answer[0] ? req->answer : "C1B3R didn't answer";
}

void CL_Notes_Frame( void ) {
	if ( HTTP_Poll( &ntListHttp ) ) {
		if ( ntListHttp.status == 200 && !strncmp( ntListHttp.answer, "ok", 2 ) ) {
			NT_ReadList( ntListHttp.answer );
			nt.refused = qfalse;
		} else {
			nt.refused = qtrue;
			nt.num = 0;
			Q_strncpyz( nt.refusal, NT_Answer( &ntListHttp ), sizeof( nt.refusal ) );
		}
	}
	if ( HTTP_Poll( &ntGetHttp ) && ntGetFor == nt.serial ) {
		nt.loading = qfalse;
		if ( ntGetHttp.status == 200 && !strncmp( ntGetHttp.answer, "ok", 2 ) ) {
			NT_ReadNote( ntGetHttp.answer );
			nt.focus = 2;
		} else {
			CL_Notes_Message( NT_Answer( &ntGetHttp ), qtrue );
			nt.open = 0;
		}
	}
	if ( HTTP_Poll( &ntSaveHttp ) ) {
		if ( ntSaveHttp.status == 200 && !strncmp( ntSaveHttp.answer, "ok", 2 ) ) {
			const int id = ntSaveHttp.answer[2] == '\t' ? atoi( ntSaveHttp.answer + 3 ) : 0;

			// a new one has its number now: still open, or waiting to be saved again
			if ( ntSaveSerial == nt.serial && nt.open < 0 && id > 0 )
				nt.open = id;
			for ( ntQueued_t &q : ntQueue ) {
				if ( q.serial == ntSaveSerial && !q.id )
					q.id = id;
			}
		} else {
			CL_Notes_Message( va( "Not saved: %s", NT_Answer( &ntSaveHttp ) ), qtrue );
			// a note left already (the datapad closed): said where it's seen, not lost without a word
			if ( ntSaveSerial == nt.serial )
				nt.dirty = qtrue;
			else
				Com_Printf( S_COLOR_YELLOW "Notes: a note wasn't saved: %s\n", NT_Answer( &ntSaveHttp ) );
			nt.editTime = cls.realtime + 5000;		// not again at once
		}
		nt.known = qfalse;
	}
	if ( HTTP_Poll( &ntDelHttp ) ) {
		if ( ntDelHttp.status != 200 )
			CL_Notes_Message( NT_Answer( &ntDelHttp ), qtrue );
		else
			CL_Notes_Message( "Deleted", qfalse );
		nt.known = qfalse;
	}
	// those left first, then the one open a moment after the typing stops
	if ( !ntQueue.empty() && ntSaveHttp.state != HTTP_BUSY && CL_Presence_Session() && NT_SaveSend( ntQueue.front() ) )
		ntQueue.erase( ntQueue.begin() );
	if ( nt.dirty && cls.realtime - nt.editTime >= NT_SAVE_MS )
		NT_Save( qfalse );
	if ( nt.shown && !nt.known && CL_Presence_Session() && ntListHttp.state != HTTP_BUSY )
		NT_AskList();
}

/*
===============================================================================

Drawing

===============================================================================
*/

static void NT_Style( wgStyle_t *style ) {
	memset( style, 0, sizeof( *style ) );
	style->font = DP_Font();
	style->scale = 0.62f;
	style->lineH = 11.5f;
	style->padX = 3;
	style->padY = 2;
	style->fill = dpScreen;
	style->border = dpGlow;
	style->text = dpWhite;
	style->dim = dpDim;
}

static void NT_Delete( void ) {
	if ( nt.open > 0 && !NT_Post( &ntDelHttp, "/v2/notes/delete", va( ",\"id\":%i", nt.open ) ) ) {
		CL_Notes_Message( "Wait a moment and try again", qtrue );
		return;
	}
	nt.dirty = qfalse;
	NT_Close();
}

static void NT_DrawList( float x, float y, float w, float bottom ) {
	float cx, cy;
	qboolean *click;
	const int visible = Q_max( 1, (int)( ( bottom - y ) / NT_ROW_H ) );
	qboolean bar;
	float rw;

	if ( !nt.num ) {
		DP_Text( x, y + 30, w, ntListHttp.state == HTTP_BUSY || !nt.known ? "Asking C1B3R..." : "No notes yet: New starts one",
			dpDim, DP_Font(), 0.62f, qtrue );
		return;
	}
	click = DP_Click( &cx, &cy );
	bar = WG_Scrollbar( x + w - WG_SCROLL_W, y, visible * NT_ROW_H, nt.num, visible, &nt.scroll, DP_CursorX(), DP_CursorY(), click, cx, cy );
	rw = w - ( bar ? WG_SCROLL_W + 2 : 0 );
	nt.scroll = Com_Clampi( 0, Q_max( 0, nt.num - visible ), nt.scroll );
	for ( int i = nt.scroll; i < nt.num && i < nt.scroll + visible; i++ ) {
		const ntNote_t *n = &nt.notes[i];
		const float ry = y + ( i - nt.scroll ) * NT_ROW_H;
		const qboolean hover = DP_InRect( x, ry, rw, NT_ROW_H - 2 );

		DP_Fill( x, ry, rw, NT_ROW_H - 2, hover ? dpButtonHover : dpButton );
		DP_Frame( x, ry, rw, NT_ROW_H - 2, dpGlow );
		DP_Text( x + 4, ry + 2, rw - 8, n->title[0] ? n->title : "Untitled", hover ? dpWhite : dpText, DP_Font(), 0.62f, qfalse );
		DP_Text( x + 4, ry + 12, rw - 8, n->updated > 0 ? DP_JediDate( n->updated ) : "", dpDim, DP_Font(), 0.5f, qfalse );
		if ( DP_Clicked( x, ry, rw, NT_ROW_H - 2 ) )
			NT_Open( n->id );
	}
}

static void NT_DrawNote( float x, float y, float w, float bottom ) {
	wgStyle_t style;
	float cx, cy;
	qboolean *click;
	const float bodyH = bottom - y - DP_CTRL_H - 18;

	NT_Style( &style );
	if ( nt.loading ) {
		DP_Text( x, y + 30, w, "Opening...", dpDim, DP_Font(), 0.62f, qtrue );
		return;
	}
	// the title
	DP_Fill( x, y, w, DP_CTRL_H, dpScreen );
	DP_Frame( x, y, w, DP_CTRL_H, nt.focus == 1 ? dpWhite : dpGlow );
	WG_Field( &style, x, y, w, DP_CTRL_H, nt.title, (qboolean)( nt.focus == 1 ), "Title", DP_CursorX(), DP_CursorY() );
	click = DP_Click( &cx, &cy );
	if ( *click && cx >= x && cx < x + w && cy >= y && cy < y + WG_FieldHeight( &style, w, DP_CTRL_H, nt.title, (qboolean)( nt.focus == 1 ) ) ) {
		*click = qfalse;
		if ( nt.focus != 1 )
			WG_EditReset();
		nt.focus = 1;
		WG_FieldClick( nt.title, cx, cy );
	}
	y += DP_CTRL_H + 4;

	// the text
	DP_Fill( x, y, w, bodyH, dpScreen );
	DP_Frame( x, y, w, bodyH, nt.focus == 2 ? dpWhite : dpGlow );
	WG_TextArea( &style, x, y, w, bodyH, nt.body, (qboolean)( nt.focus == 2 ), "Write here", DP_CursorX(), DP_CursorY(), click, cx, cy );
	if ( *click && cx >= x && cx < x + w && cy >= y && cy < y + bodyH ) {
		*click = qfalse;
		if ( nt.focus != 2 )
			WG_EditReset();
		nt.focus = 2;
		WG_FieldClick( nt.body, cx, cy );
	}
	y += bodyH + 3;

	// how it is
	DP_Text( x, y, w * 0.6f, ntSaveHttp.state == HTTP_BUSY ? "Saving..." : nt.dirty ? "Not saved yet" : nt.open > 0 ? "Saved" : "Not saved: write something",
		dpDim, DP_Font(), 0.55f, qfalse );
	DP_Text( x + w * 0.6f, y, w * 0.4f, va( "%i / %i", (int)strlen( nt.body ), NT_BODY - 1 ), dpDim, DP_Font(), 0.55f, qfalse );
	WG_DrawOverlay();
}

void CL_Notes_Draw( const dpRect_t *s ) {
	const float pad = DP_Wide( 6 ), x = s->x + pad, w = s->w - pad * 2, bottom = s->y + s->h - 16;
	float y = s->y + DP_STATUS_H + 4;

	// the top row: whose, and what's done
	if ( nt.open ) {
		if ( DP_Button( x, y, w * 0.34f, "< Notes", qfalse ) ) {
			NT_Close();
			return;
		}
		if ( DP_Button( x + w * 0.66f, y, w * 0.34f, nt.confirmDelete ? "Sure? Delete" : "Delete", nt.confirmDelete ) ) {
			if ( nt.confirmDelete ) {
				NT_Delete();
				return;
			}
			nt.confirmDelete = qtrue;
		}
	} else {
		// whose they are: a datapad's own, or the character's
		const char *who = CL_Characters_ActiveName();
		const char *title = nt.item > 0 || nt.world > 0 ? va( "Datapad \"%s\"", nt.owner[0] ? nt.owner : "Datapad" )
			: who[0] ? va( "Your notes (%s)", who ) : "Your notes";

		DP_Text( x, y + 1, w * 0.62f, title, dpText, DP_TitleFont(), 0.5f, qfalse );
		if ( DP_Button( x + w * 0.66f, y, w * 0.34f, "New note", qfalse ) && CL_Presence_Session() && !nt.refused ) {
			if ( nt.num >= NT_MAX )
				CL_Notes_Message( va( "%i notes at most", NT_MAX ), qtrue );
			else
				NT_Open( -1 );
		}
	}
	y += DP_CTRL_H + 6;

	// one placed in the world: picked up from here
	if ( nt.world > 0 && !nt.open ) {
		if ( DP_Button( x, y, w, "Pick it up", qfalse ) )
			CL_WorldItems_PickUp( nt.world );
		y += DP_CTRL_H + 6;
	}

	if ( !CL_Presence_Session() ) {
		DP_Text( x, y + 20, w, "Not connected to C1B3R", dpText, DP_Font(), 0.7f, qtrue );
		DP_Text( x, y + 36, w, "Notes need your game linked to C1B3R", dpDim, DP_Font(), 0.58f, qtrue );
	} else if ( nt.refused && !nt.open ) {
		wgStyle_t style;

		NT_Style( &style );
		DP_Text( x, y + 20, w, "Notes need your game linked to C1B3R", dpText, DP_Font(), 0.62f, qtrue );
		WG_DrawText( &style, x, y + 38, w, nt.refusal, 0, 6, dpDim );
	} else if ( nt.open ) {
		NT_DrawNote( x, y, w, bottom );
	} else {
		NT_DrawList( x, y, w, bottom );
	}

	if ( nt.message[0] && cls.realtime - nt.messageTime < NT_MESSAGE_MS )
		DP_Text( x, s->y + s->h - 13, w, nt.message, nt.messageBad ? dpWhite : dpText, DP_Font(), 0.58f, qfalse );
}

/*
===============================================================================

Keys

===============================================================================
*/

static char *NT_Focused( int *size ) {
	if ( !nt.open || nt.loading )
		return NULL;
	if ( nt.focus == 1 ) {
		*size = sizeof( nt.title );
		return nt.title;
	}
	if ( nt.focus == 2 ) {
		*size = sizeof( nt.body );
		return nt.body;
	}
	return NULL;
}

static void NT_Changed( void ) {
	nt.dirty = qtrue;
	nt.editTime = cls.realtime;
	nt.confirmDelete = qfalse;
}

qboolean CL_Notes_Key( int key ) {
	int size;
	char *buf = NT_Focused( &size );

	if ( key == A_MWHEELUP || key == A_MWHEELDOWN ) {
		const int dir = key == A_MWHEELUP ? -1 : 1;

		if ( nt.open && nt.focus == 2 )
			WG_TextAreaScroll( dir * 3 );
		else if ( !nt.open )
			nt.scroll = Q_max( 0, nt.scroll + dir );
		return qtrue;
	}
	if ( !buf )
		return qfalse;
	// the title: Enter or Tab on to the text
	if ( nt.focus == 1 && ( key == A_ENTER || key == A_KP_ENTER || key == A_TAB ) ) {
		nt.focus = 2;
		WG_EditReset();
		return qtrue;
	}
	if ( nt.focus == 2 && key == A_TAB ) {
		nt.focus = 1;
		WG_EditReset();
		return qtrue;
	}
	if ( WG_EditKey( buf, size, key, NULL ) == WG_EDIT_CHANGED )
		NT_Changed();
	// typing: none of the keys go on to the datapad's binds
	return qtrue;
}

void CL_Notes_Char( int ch ) {
	int size;
	char *buf = NT_Focused( &size );

	if ( buf && WG_EditChar( buf, size, ch, NULL ) )
		NT_Changed();
}
