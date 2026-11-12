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

// cl_widgets.cpp -- what the tools' windows share: a scrollbar a list can be dragged by, text
// fields with a cursor, a selection, the clipboard, and long text going on below, and the
// - value + control: its value opens a list to pick from, or a field to type a number in.
// Everything is in the 640x480 screen the tools draw in.

#include <string>
#include <vector>
#include "client.h"
#include "keys.h"
#include "cl_widgets.h"

/*
===============================================================================

SCROLLBAR

===============================================================================
*/

static const vec4_t wgTrack = { 0.0f, 0.0f, 0.0f, 0.35f };
static const vec4_t wgThumb = { 0.62f, 0.66f, 0.72f, 0.55f };
static const vec4_t wgThumbHot = { 0.80f, 0.84f, 0.90f, 0.85f };

static struct {
	const void	*id;			// the scroll being dragged, by its address
	float		grab;			// where on the handle it was taken
} wgDrag;

static void WG_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static qboolean WG_MouseDown( void ) {
	return kg.keys[A_MOUSE1].down;
}

// the bar for a view of h out of content, at offset *scroll (0 to content - view); qtrue when shown
static qboolean WG_Bar( const void *id, float x, float y, float h, float content, float view, float *scroll,
	float cursorX, float cursorY, qboolean *click, float clickX, float clickY ) {
	const float range = content - view;
	float thumbH, thumbY;
	qboolean hot;

	if ( range <= 0.0f || h <= 4.0f ) {
		if ( wgDrag.id == id )
			wgDrag.id = NULL;
		return qfalse;
	}
	thumbH = Q_max( 12.0f, h * view / content );
	if ( thumbH > h )
		thumbH = h;
	*scroll = Com_Clamp( 0.0f, range, *scroll );
	thumbY = y + ( h - thumbH ) * ( *scroll / range );

	// taken: on the handle it's dragged from there, beside it the handle comes to it
	if ( click && *click && clickX >= x && clickX < x + WG_SCROLL_W && clickY >= y && clickY < y + h ) {
		*click = qfalse;
		wgDrag.id = id;
		wgDrag.grab = ( clickY >= thumbY && clickY < thumbY + thumbH ) ? clickY - thumbY : thumbH * 0.5f;
	}
	if ( wgDrag.id == id ) {
		if ( !WG_MouseDown() ) {
			wgDrag.id = NULL;
		} else {
			const float along = Com_Clamp( 0.0f, h - thumbH, cursorY - wgDrag.grab - y );

			*scroll = h - thumbH > 0.0f ? range * along / ( h - thumbH ) : 0.0f;
			thumbY = y + along;
		}
	}

	hot = (qboolean)( wgDrag.id == id || ( cursorX >= x && cursorX < x + WG_SCROLL_W && cursorY >= thumbY && cursorY < thumbY + thumbH ) );
	WG_Fill( x, y, WG_SCROLL_W, h, wgTrack );
	WG_Fill( x + 1, thumbY + 1, WG_SCROLL_W - 2, thumbH - 2, hot ? wgThumbHot : wgThumb );
	return qtrue;
}

qboolean WG_Scrollbar( float x, float y, float h, int count, int visible, int *scroll,
	float cursorX, float cursorY, qboolean *click, float clickX, float clickY ) {
	float s = (float)*scroll;
	qboolean shown;

	if ( visible < 1 )
		visible = 1;
	shown = WG_Bar( scroll, x, y, h, (float)count, (float)visible, &s, cursorX, cursorY, click, clickX, clickY );
	if ( shown && wgDrag.id == scroll )
		*scroll = Com_Clampi( 0, Q_max( 0, count - visible ), (int)( s + 0.5f ) );
	return shown;
}

qboolean WG_ScrollbarPixels( float x, float y, float h, float content, float *scroll,
	float cursorX, float cursorY, qboolean *click, float clickX, float clickY ) {
	return WG_Bar( scroll, x, y, h, content, h, scroll, cursorX, cursorY, click, clickX, clickY );
}

/*
===============================================================================

TEXT

What was typed is shown as it is: a ^ is drawn as itself, not taken for a colour.

===============================================================================
*/

#define WG_MAX_LINES		1024	// a field's wrapped lines (a text area's can be long)
#define WG_SHOWN_LINES		10		// grown no taller than this; it scrolls to the cursor
#define WG_DOUBLECLICK_MS	350

static const vec4_t wgSelect = { 0.30f, 0.50f, 0.95f, 0.45f };
static const vec4_t wgCursor = { 1.0f, 1.0f, 1.0f, 0.95f };

// each character's width in a font at scale 1
#define WG_FONT_CACHE	4
static struct {
	int		font;
	float	w[256];
	byte	known[256];
} wgWidths[WG_FONT_CACHE];
static int wgNextWidths;

static float WG_CharW( const wgStyle_t *s, int ch ) {
	int slot = -1;

	ch &= 255;
	for ( int i = 0; i < WG_FONT_CACHE; i++ ) {
		if ( wgWidths[i].font == s->font ) {
			slot = i;
			break;
		}
	}
	if ( slot < 0 ) {
		slot = wgNextWidths;
		wgNextWidths = ( wgNextWidths + 1 ) % WG_FONT_CACHE;
		memset( &wgWidths[slot], 0, sizeof( wgWidths[slot] ) );
		wgWidths[slot].font = s->font;
	}
	if ( !wgWidths[slot].known[ch] ) {
		const char str[2] = { (char)ch, '\0' };

		// a ^ alone isn't a colour; measured big for its fraction of a pixel
		wgWidths[slot].w[ch] = ch < 32 ? 0.0f : re->ext.Font_StrLenPixels( str, s->font, 8.0f ) / 8.0f;
		wgWidths[slot].known[ch] = 1;
	}
	return wgWidths[slot].w[ch] * s->scale;
}

static float WG_Width( const wgStyle_t *s, const char *text, int len ) {
	float w = 0.0f;

	for ( int i = 0; i < len && text[i]; i++ )
		w += WG_CharW( s, (byte)text[i] );
	return w;
}

// len characters at x, the ^ each drawn on its own
static void WG_DrawRun( const wgStyle_t *s, float x, float y, const char *text, int len, const float *color ) {
	char seg[MAX_STRING_CHARS];

	while ( len > 0 && *text ) {
		int n = 0;

		// a new line isn't drawn
		if ( (byte)*text < 32 ) {
			text++;
			len--;
			continue;
		}
		if ( *text == Q_COLOR_ESCAPE ) {
			re->Font_DrawString( (int)x, (int)y, "^", color, s->font, -1, s->scale );
			x += WG_CharW( s, Q_COLOR_ESCAPE );
			text++;
			len--;
			continue;
		}
		while ( n < len && text[n] && text[n] != Q_COLOR_ESCAPE && (byte)text[n] >= 32 && n < (int)sizeof( seg ) - 1 ) {
			seg[n] = text[n];
			n++;
		}
		seg[n] = '\0';
		re->Font_DrawString( (int)x, (int)y, seg, color, s->font, -1, s->scale );
		x += WG_Width( s, seg, n );
		text += n;
		len -= n;
	}
}

// the text in lines of width w: broken after a new line, after a space where it can be, else where it must
typedef struct {
	int		start[WG_MAX_LINES + 1];	// start[num] is the length
	int		num;
} wgLines_t;

static void WG_Wrap( const wgStyle_t *s, const char *text, float w, wgLines_t *out ) {
	const int len = (int)strlen( text );
	int start = 0;

	out->num = 0;
	while ( out->num < WG_MAX_LINES - 1 ) {
		float x = 0.0f;
		int i = start, brk = -1;
		qboolean hard = qfalse;

		out->start[out->num++] = start;
		while ( i < len ) {
			float cw;

			if ( text[i] == '\n' ) {
				i++;
				hard = qtrue;
				break;
			}
			cw = WG_CharW( s, (byte)text[i] );

			if ( x + cw > w && i > start )
				break;
			x += cw;
			if ( text[i] == ' ' )
				brk = i + 1;
			i++;
		}
		// after a new line, the next starts there: an empty last one after a new line at the end
		if ( hard ) {
			start = i;
			continue;
		}
		if ( i >= len ) {
			out->start[out->num] = len;
			return;
		}
		start = ( brk > start ) ? brk : i;
	}
	out->start[out->num++] = start;
	out->start[out->num] = len;
}

// the line the index is on; an index where one line ends and the next begins is the next's
static int WG_LineOf( const wgLines_t *l, int index ) {
	for ( int i = l->num - 1; i > 0; i-- ) {
		if ( index >= l->start[i] )
			return i;
	}
	return 0;
}

// the index nearest x along a line
static int WG_IndexAlong( const wgStyle_t *s, const char *text, const wgLines_t *l, int line, float x ) {
	const int a = l->start[line], b = l->start[line + 1];
	int end = b;
	float at = 0.0f;

	// the space or new line a line broke after belongs to it, but the cursor goes before it
	if ( line < l->num - 1 && end > a && ( text[end - 1] == ' ' || text[end - 1] == '\n' ) )
		end--;
	for ( int i = a; i < end; i++ ) {
		const float cw = WG_CharW( s, (byte)text[i] );

		if ( x < at + cw * 0.5f )
			return i;
		at += cw;
	}
	return end;
}

/*
===============================================================================

THE FIELD TYPED IN

===============================================================================
*/

static struct {
	char		*buf;			// the field focused, or NULL
	int			cursor, anchor;	// the selection is between them
	qboolean	dragging;
	int			blinkFrom;		// the cursor shows steady after it moved
	int			lastClick;
	int			lastClickAt;
	int			top;			// grown: the first line seen

	// where it was drawn
	wgStyle_t	style;
	float		x, y, w, h;
	qboolean	overlay;		// grown this frame: drawn last
	int			overlayFrame;

	// the text area typed in: new lines go in it, and it scrolls on its own
	char		*areaBuf;
	int			areaCursor, areaLen;	// as last drawn: moved since, it's scrolled to the cursor
	int			areaRows;

	// the last field drawn, focused or not, for a click on it
	char		*lastBuf;
	wgStyle_t	lastStyle;
	float		lastX, lastY, lastW, lastH;
} wg;

static qboolean WG_Ctrl( void ) {
	return (qboolean)( kg.keys[A_CTRL].down || kg.keys[A_CTRL2].down );
}

static qboolean WG_Shift( void ) {
	return (qboolean)( kg.keys[A_SHIFT].down || kg.keys[A_SHIFT2].down );
}

void WG_EditReset( void ) {
	wg.buf = NULL;
	wg.dragging = qfalse;
	wg.top = 0;
}

static void WG_Focus( char *buf ) {
	if ( wg.buf == buf )
		return;
	wg.buf = buf;
	wg.cursor = wg.anchor = (int)strlen( buf );
	wg.dragging = qfalse;
	wg.top = 0;
	wg.blinkFrom = cls.realtime;
}

static void WG_Clamp( const char *buf ) {
	const int len = (int)strlen( buf );

	wg.cursor = Com_Clampi( 0, len, wg.cursor );
	wg.anchor = Com_Clampi( 0, len, wg.anchor );
}

static float WG_TextW( const wgStyle_t *s, float w ) {
	return Q_max( 8.0f, w - s->padX * 2.0f );
}

// the lines the focused field is in, at its width
static void WG_FocusLines( const char *buf, wgLines_t *l ) {
	WG_Wrap( &wg.style, buf, WG_TextW( &wg.style, wg.w ), l );
}

static int WG_ShownLines( int lines ) {
	return Q_min( lines, WG_SHOWN_LINES );
}

float WG_FieldHeight( const wgStyle_t *style, float w, float h, const char *buf, qboolean focused ) {
	wgLines_t l;

	if ( !focused || !buf[0] )
		return h;
	WG_Wrap( style, buf, WG_TextW( style, w ), &l );
	return h + ( WG_ShownLines( l.num ) - 1 ) * style->lineH;
}

// the index under a point in the focused field
static int WG_IndexAt( const char *buf, float px, float py ) {
	wgLines_t l;
	int line;

	WG_FocusLines( buf, &l );
	line = (int)floorf( ( py - wg.y - wg.style.padY ) / wg.style.lineH );
	if ( l.num > 1 )
		line += wg.top;
	line = Com_Clampi( 0, l.num - 1, line );
	return WG_IndexAlong( &wg.style, buf, &l, line, px - wg.x - wg.style.padX );
}

static qboolean WG_WordChar( int ch ) {
	return (qboolean)( ch != ' ' && ch != '\0' );
}

void WG_FieldClick( char *buf, float clickX, float clickY ) {
	if ( wg.lastBuf == buf && wg.buf != buf ) {
		// not focused when it was drawn: it was drawn from its start, on one line
		WG_Focus( buf );
		wg.style = wg.lastStyle;
		wg.x = wg.lastX, wg.y = wg.lastY, wg.w = wg.lastW, wg.h = wg.lastH;
	} else if ( wg.buf != buf ) {
		WG_Focus( buf );
		return;
	}
	WG_Clamp( buf );

	const int at = WG_IndexAt( buf, clickX, clickY );

	// a second click on the same place selects the word
	if ( cls.realtime - wg.lastClick < WG_DOUBLECLICK_MS && at == wg.lastClickAt ) {
		int a = at, b = at;

		while ( a > 0 && WG_WordChar( buf[a - 1] ) )
			a--;
		while ( WG_WordChar( buf[b] ) )
			b++;
		wg.anchor = a;
		wg.cursor = b;
		wg.lastClick = 0;
		wg.dragging = qfalse;
		wg.blinkFrom = cls.realtime;
		return;
	}
	wg.lastClick = cls.realtime;
	wg.lastClickAt = at;
	wg.cursor = at;
	if ( !WG_Shift() )
		wg.anchor = at;
	wg.dragging = qtrue;
	wg.blinkFrom = cls.realtime;
}

// the lines, the selection and the cursor; first the top line shown
static void WG_DrawLines( const char *buf, const wgLines_t *l, int first, int count, float x, float y ) {
	const wgStyle_t *s = &wg.style;
	const int selA = Q_min( wg.cursor, wg.anchor ), selB = Q_max( wg.cursor, wg.anchor );
	const qboolean blink = (qboolean)( ( ( cls.realtime - wg.blinkFrom ) / 500 ) % 2 == 0 );
	const int cursorLine = WG_LineOf( l, wg.cursor );

	for ( int i = first; i < first + count && i < l->num; i++ ) {
		const int a = l->start[i], b = l->start[i + 1];
		const float ly = y + ( i - first ) * s->lineH;
		const float tx = x + s->padX, ty = ly + s->padY;

		if ( selB > a && selA < b ) {
			const int from = Q_max( selA, a ), to = Q_min( selB, b );
			const float sx = tx + WG_Width( s, buf + a, from - a );

			WG_Fill( sx, ty, Q_max( 2.0f, WG_Width( s, buf + from, to - from ) ), s->lineH, wgSelect );
		}
		WG_DrawRun( s, tx, ty, buf + a, b - a, s->text );
		if ( i == cursorLine && blink )
			WG_Fill( tx + WG_Width( s, buf + a, wg.cursor - a ), ty, 1.0f, s->lineH, wgCursor );
	}
}

void WG_Field( const wgStyle_t *style, float x, float y, float w, float h, char *buf, qboolean focused,
	const char *placeholder, float cursorX, float cursorY ) {
	wgLines_t l;

	wg.lastBuf = buf;
	wg.lastStyle = *style;
	wg.lastX = x, wg.lastY = y, wg.lastW = w, wg.lastH = h;

	if ( !focused ) {
		if ( wg.buf == buf )
			WG_EditReset();
		if ( !buf[0] ) {
			if ( placeholder && placeholder[0] )
				re->Font_DrawString( (int)( x + style->padX ), (int)( y + style->padY ), placeholder, style->dim, style->font, -1, style->scale );
			return;
		}
		// the start, cut with ...
		const float room = WG_TextW( style, w );
		const int len = (int)strlen( buf );

		if ( WG_Width( style, buf, len ) <= room ) {
			WG_DrawRun( style, x + style->padX, y + style->padY, buf, len, style->text );
		} else {
			const float dots = WG_Width( style, "...", 3 );
			int n = 0;
			float at = 0.0f;

			while ( n < len && at + WG_CharW( style, (byte)buf[n] ) <= room - dots ) {
				at += WG_CharW( style, (byte)buf[n] );
				n++;
			}
			WG_DrawRun( style, x + style->padX, y + style->padY, buf, n, style->text );
			re->Font_DrawString( (int)( x + style->padX + at ), (int)( y + style->padY ), "...", style->dim, style->font, -1, style->scale );
		}
		return;
	}

	WG_Focus( buf );
	WG_Clamp( buf );
	wg.style = *style;
	wg.x = x, wg.y = y, wg.w = w, wg.h = h;

	if ( wg.dragging ) {
		if ( WG_MouseDown() ) {
			const int at = WG_IndexAt( buf, cursorX, cursorY );

			if ( at != wg.cursor )
				wg.blinkFrom = cls.realtime;
			wg.cursor = at;
		} else {
			wg.dragging = qfalse;
		}
	}

	WG_FocusLines( buf, &l );
	if ( l.num > 1 ) {
		// grown: drawn over everything at the frame's end, scrolled to the cursor
		const int shown = WG_ShownLines( l.num ), line = WG_LineOf( &l, wg.cursor );

		if ( line < wg.top )
			wg.top = line;
		else if ( line >= wg.top + shown )
			wg.top = line - shown + 1;
		wg.top = Com_Clampi( 0, l.num - shown, wg.top );
		wg.overlay = qtrue;
		wg.overlayFrame = cls.framecount;
		return;
	}
	wg.top = 0;
	WG_DrawLines( buf, &l, 0, 1, x, y );
}

static void WG_DrawPopList( void );

void WG_DrawOverlay( void ) {
	wgLines_t l;
	int shown;
	float h;

	WG_DrawPopList();
	if ( !wg.overlay )
		return;
	wg.overlay = qfalse;
	if ( !wg.buf || wg.overlayFrame != cls.framecount )
		return;
	WG_Clamp( wg.buf );
	WG_FocusLines( wg.buf, &l );
	shown = WG_ShownLines( l.num );
	h = wg.h + ( shown - 1 ) * wg.style.lineH;

	WG_Fill( wg.x, wg.y, wg.w, h, wg.style.fill );
	WG_Fill( wg.x, wg.y, wg.w, 1, wg.style.border );
	WG_Fill( wg.x, wg.y + h - 1, wg.w, 1, wg.style.border );
	WG_Fill( wg.x, wg.y, 1, h, wg.style.border );
	WG_Fill( wg.x + wg.w - 1, wg.y, 1, h, wg.style.border );
	WG_DrawLines( wg.buf, &l, wg.top, shown, wg.x, wg.y );
	if ( l.num > shown ) {
		float top = (float)wg.top;

		// how far down it is, not dragged: the cursor leads
		WG_Bar( &wg.top, wg.x + wg.w - WG_SCROLL_W - 1, wg.y + 1, h - 2, (float)l.num, (float)shown, &top,
			-1.0f, -1.0f, NULL, 0.0f, 0.0f );
	}
}

/*
===============================================================================

THE TEXT BOX AND READ-ONLY TEXT

===============================================================================
*/

void WG_TextBox( const wgStyle_t *style, float x, float y, float w, float h, char *buf, qboolean focused,
	const char *placeholder, float cursorX, float cursorY ) {
	const int rows = Q_max( 1, (int)( ( h - style->padY * 2.0f ) / style->lineH ) );
	wgLines_t l;

	wg.lastBuf = buf;
	wg.lastStyle = *style;
	wg.lastX = x, wg.lastY = y, wg.lastW = w, wg.lastH = h;

	if ( !focused ) {
		if ( wg.buf == buf )
			WG_EditReset();
		if ( !buf[0] ) {
			if ( placeholder && placeholder[0] )
				re->Font_DrawString( (int)( x + style->padX ), (int)( y + style->padY ), placeholder, style->dim, style->font, -1, style->scale );
			return;
		}
		WG_Wrap( style, buf, WG_TextW( style, w ), &l );
		for ( int i = 0; i < rows && i < l.num; i++ )
			WG_DrawRun( style, x + style->padX, y + style->padY + i * style->lineH, buf + l.start[i], l.start[i + 1] - l.start[i], style->text );
		return;
	}

	WG_Focus( buf );
	WG_Clamp( buf );
	wg.style = *style;
	wg.x = x, wg.y = y, wg.w = w, wg.h = h;

	if ( wg.dragging ) {
		if ( WG_MouseDown() ) {
			const int at = WG_IndexAt( buf, cursorX, cursorY );

			if ( at != wg.cursor )
				wg.blinkFrom = cls.realtime;
			wg.cursor = at;
		} else {
			wg.dragging = qfalse;
		}
	}

	// scrolled to the cursor
	WG_FocusLines( buf, &l );
	{
		const int line = WG_LineOf( &l, wg.cursor );

		if ( line < wg.top )
			wg.top = line;
		else if ( line >= wg.top + rows )
			wg.top = line - rows + 1;
		wg.top = Com_Clampi( 0, Q_max( 0, l.num - rows ), wg.top );
	}
	WG_DrawLines( buf, &l, wg.top, rows, x, y );
	if ( l.num > rows ) {
		float top = (float)wg.top;

		WG_Bar( &wg.top, x + w - WG_SCROLL_W - 1, y + 1, h - 2, (float)l.num, (float)rows, &top, -1.0f, -1.0f, NULL, 0.0f, 0.0f );
	}
}

void WG_TextArea( const wgStyle_t *style, float x, float y, float w, float h, char *buf, qboolean focused,
	const char *placeholder, float cursorX, float cursorY, qboolean *click, float clickX, float clickY ) {
	const int rows = Q_max( 1, (int)( ( h - style->padY * 2.0f ) / style->lineH ) );
	const float textW = w - WG_SCROLL_W - 2;
	wgLines_t l;

	wg.lastBuf = buf;
	wg.lastStyle = *style;
	wg.lastX = x, wg.lastY = y, wg.lastW = textW, wg.lastH = h;

	if ( !focused ) {
		if ( wg.buf == buf )
			WG_EditReset();
		if ( wg.areaBuf == buf )
			wg.areaBuf = NULL;
		if ( !buf[0] ) {
			if ( placeholder && placeholder[0] )
				re->Font_DrawString( (int)( x + style->padX ), (int)( y + style->padY ), placeholder, style->dim, style->font, -1, style->scale );
			return;
		}
		WG_Wrap( style, buf, WG_TextW( style, textW ), &l );
		for ( int i = 0; i < rows && i < l.num; i++ )
			WG_DrawRun( style, x + style->padX, y + style->padY + i * style->lineH, buf + l.start[i], l.start[i + 1] - l.start[i], style->text );
		return;
	}

	if ( wg.areaBuf != buf ) {
		wg.areaBuf = buf;
		wg.areaCursor = -1;
	}
	WG_Focus( buf );
	WG_Clamp( buf );
	wg.style = *style;
	wg.x = x, wg.y = y, wg.w = textW, wg.h = h;
	wg.areaRows = rows;

	if ( wg.dragging ) {
		if ( WG_MouseDown() ) {
			const int at = WG_IndexAt( buf, cursorX, cursorY );

			if ( at != wg.cursor )
				wg.blinkFrom = cls.realtime;
			wg.cursor = at;
		} else {
			wg.dragging = qfalse;
		}
	}

	WG_FocusLines( buf, &l );
	// moved or typed in since: scrolled to the cursor; else where the wheel or the bar left it
	if ( wg.cursor != wg.areaCursor || (int)strlen( buf ) != wg.areaLen ) {
		const int line = WG_LineOf( &l, wg.cursor );

		if ( line < wg.top )
			wg.top = line;
		else if ( line >= wg.top + rows )
			wg.top = line - rows + 1;
		wg.areaCursor = wg.cursor;
		wg.areaLen = (int)strlen( buf );
	}
	{
		float top = (float)wg.top;

		if ( l.num > rows && WG_Bar( &wg.areaRows, x + w - WG_SCROLL_W - 1, y + 1, h - 2, (float)l.num, (float)rows, &top,
			cursorX, cursorY, click, clickX, clickY ) && wgDrag.id == &wg.areaRows )
			wg.top = (int)( top + 0.5f );
	}
	wg.top = Com_Clampi( 0, Q_max( 0, l.num - rows ), wg.top );
	WG_DrawLines( buf, &l, wg.top, rows, x, y );
}

void WG_TextAreaScroll( int lines ) {
	if ( wg.areaBuf && wg.buf == wg.areaBuf )
		wg.top = Q_max( 0, wg.top + lines );
}

int WG_TextLines( const wgStyle_t *style, const char *text, float w ) {
	wgLines_t l;

	if ( !text[0] )
		return 0;
	WG_Wrap( style, text, w, &l );
	return l.num;
}

void WG_DrawText( const wgStyle_t *style, float x, float y, float w, const char *text, int first, int count, const float *color ) {
	wgLines_t l;

	if ( !text[0] )
		return;
	WG_Wrap( style, text, w, &l );
	for ( int i = Q_max( 0, first ); i < first + count && i < l.num; i++ )
		WG_DrawRun( style, x, y + ( i - first ) * style->lineH, text + l.start[i], l.start[i + 1] - l.start[i], color );
}

/*
===============================================================================

EDITING

===============================================================================
*/

static qboolean WG_HasSelection( void ) {
	return (qboolean)( wg.cursor != wg.anchor );
}

static void WG_DeleteSelection( char *buf ) {
	const int a = Q_min( wg.cursor, wg.anchor ), b = Q_max( wg.cursor, wg.anchor );

	memmove( buf + a, buf + b, strlen( buf + b ) + 1 );
	wg.cursor = wg.anchor = a;
}

static qboolean WG_Allowed( int ch, wgAllow_t allow ) {
	if ( ch < 32 || ch == 127 )
		return qfalse;
	return allow ? allow( ch ) : qtrue;
}

// text put in at the cursor over the selection, as much as fits; qtrue if anything changed
static qboolean WG_Insert( char *buf, int size, const char *text, int n, wgAllow_t allow ) {
	qboolean changed = qfalse;
	int len;

	if ( WG_HasSelection() ) {
		WG_DeleteSelection( buf );
		changed = qtrue;
	}
	len = (int)strlen( buf );
	for ( int i = 0; i < n && len < size - 1; i++ ) {
		int ch = (byte)text[i];

		// a text area keeps new lines; anywhere else they're spaces
		if ( ch == '\r' && buf == wg.areaBuf )
			continue;
		if ( ch == '\n' && buf == wg.areaBuf ) {
			if ( allow && !allow( ch ) )
				continue;
		} else {
			if ( ch == '\n' || ch == '\r' || ch == '\t' )
				ch = ' ';
			if ( !WG_Allowed( ch, allow ) )
				continue;
		}
		memmove( buf + wg.cursor + 1, buf + wg.cursor, len - wg.cursor + 1 );
		buf[wg.cursor++] = (char)ch;
		len++;
		changed = qtrue;
	}
	wg.anchor = wg.cursor;
	return changed;
}

#define WG_CLIP_MAX		16384		// a text area's whole text

static void WG_Copy( const char *buf ) {
	static char text[WG_CLIP_MAX];
	const int a = Q_min( wg.cursor, wg.anchor ), b = Q_max( wg.cursor, wg.anchor );

	if ( a == b )
		return;
	Q_strncpyz( text, buf + a, Q_min( (int)sizeof( text ), b - a + 1 ) );
	Sys_SetClipboardData( text );
}

static qboolean WG_Paste( char *buf, int size, wgAllow_t allow ) {
	static char text[WG_CLIP_MAX];
	char *clip = Sys_GetClipboardData(), *c;
	int n = 0;

	if ( !clip )
		return qfalse;
	// as the console takes it: UTF-8 to the game's own letters
	c = clip;
	while ( *c && n < (int)sizeof( text ) - 1 ) {
		const uint32_t utf32 = ConvertUTF8ToUTF32( c, &c );

		text[n++] = (char)ConvertUTF32ToExpectedCharset( utf32 );
	}
	text[n] = '\0';
	Z_Free( clip );
	return WG_Insert( buf, size, text, n, allow );
}

static int WG_WordLeft( const char *buf, int at ) {
	while ( at > 0 && buf[at - 1] == ' ' )
		at--;
	while ( at > 0 && buf[at - 1] != ' ' )
		at--;
	return at;
}

static int WG_WordRight( const char *buf, int at ) {
	while ( buf[at] && buf[at] != ' ' )
		at++;
	while ( buf[at] == ' ' )
		at++;
	return at;
}

int WG_EditKey( char *buf, int size, int key, wgAllow_t allow ) {
	const qboolean ctrl = WG_Ctrl(), shift = WG_Shift();
	const int len = (int)strlen( buf ), letter = keynames[key].lower;
	int to = -1;

	if ( wg.buf != buf )
		WG_Focus( buf );
	WG_Clamp( buf );

	if ( ctrl && letter == 'a' ) {
		wg.anchor = 0;
		wg.cursor = len;
		wg.blinkFrom = cls.realtime;
		return WG_EDIT_MOVED;
	}
	if ( ( ctrl && letter == 'c' ) || ( ctrl && key == A_INSERT ) ) {
		WG_Copy( buf );
		return WG_EDIT_MOVED;
	}
	if ( ctrl && letter == 'x' ) {
		if ( !WG_HasSelection() )
			return WG_EDIT_MOVED;
		WG_Copy( buf );
		WG_DeleteSelection( buf );
		wg.blinkFrom = cls.realtime;
		return WG_EDIT_CHANGED;
	}
	if ( ( ctrl && letter == 'v' ) || ( shift && ( key == A_INSERT || key == A_KP_0 ) ) ) {
		wg.blinkFrom = cls.realtime;
		return WG_Paste( buf, size, allow ) ? WG_EDIT_CHANGED : WG_EDIT_MOVED;
	}

	// a text area: Enter's a new line, the page keys a page
	if ( buf == wg.areaBuf && ( key == A_ENTER || key == A_KP_ENTER ) ) {
		wg.blinkFrom = cls.realtime;
		return WG_Insert( buf, size, "\n", 1, allow ) ? WG_EDIT_CHANGED : WG_EDIT_MOVED;
	}
	if ( buf == wg.areaBuf && ( key == A_PAGE_UP || key == A_PAGE_DOWN || key == A_KP_9 || key == A_KP_3 ) && wg.style.font && wg.w > 0.0f ) {
		wgLines_t l;
		const int page = Q_max( 1, wg.areaRows - 1 );
		int line, next;

		WG_FocusLines( buf, &l );
		line = WG_LineOf( &l, wg.cursor );
		next = Com_Clampi( 0, l.num - 1, line + ( key == A_PAGE_UP || key == A_KP_9 ? -page : page ) );
		to = WG_IndexAlong( &wg.style, buf, &l, next, WG_Width( &wg.style, buf + l.start[line], wg.cursor - l.start[line] ) );
		wg.cursor = to;
		if ( !shift )
			wg.anchor = to;
		wg.blinkFrom = cls.realtime;
		return WG_EDIT_MOVED;
	}

	switch ( key ) {
	case A_BACKSPACE:
	case A_DELETE:
		wg.blinkFrom = cls.realtime;
		if ( WG_HasSelection() ) {
			WG_DeleteSelection( buf );
			return WG_EDIT_CHANGED;
		}
		if ( key == A_BACKSPACE ) {
			if ( !wg.cursor )
				return WG_EDIT_MOVED;
			wg.anchor = ctrl ? WG_WordLeft( buf, wg.cursor ) : wg.cursor - 1;
		} else {
			if ( wg.cursor >= len )
				return WG_EDIT_MOVED;
			wg.anchor = ctrl ? WG_WordRight( buf, wg.cursor ) : wg.cursor + 1;
		}
		WG_DeleteSelection( buf );
		return WG_EDIT_CHANGED;

	case A_CURSOR_LEFT:
		if ( WG_HasSelection() && !shift )
			to = Q_min( wg.cursor, wg.anchor );
		else
			to = ctrl ? WG_WordLeft( buf, wg.cursor ) : Q_max( 0, wg.cursor - 1 );
		break;
	case A_CURSOR_RIGHT:
		if ( WG_HasSelection() && !shift )
			to = Q_max( wg.cursor, wg.anchor );
		else
			to = ctrl ? WG_WordRight( buf, wg.cursor ) : Q_min( len, wg.cursor + 1 );
		break;
	case A_HOME:
	case A_END:
	case A_CURSOR_UP:
	case A_CURSOR_DOWN: {
		wgLines_t l;
		const qboolean onLines = (qboolean)( wg.style.font && wg.w > 0.0f );
		int line = 0;

		if ( onLines ) {
			WG_FocusLines( buf, &l );
			line = WG_LineOf( &l, wg.cursor );
		} else {
			l.num = 1;
			l.start[0] = 0;
			l.start[1] = len;
		}
		if ( key == A_HOME ) {
			to = ctrl ? 0 : l.start[line];
		} else if ( key == A_END ) {
			to = ctrl ? len : ( line < l.num - 1 ? l.start[line + 1] - ( buf[l.start[line + 1] - 1] == ' ' || buf[l.start[line + 1] - 1] == '\n' ? 1 : 0 ) : len );
		} else {
			const int next = line + ( key == A_CURSOR_UP ? -1 : 1 );

			// one line: up and down are the tool's
			if ( l.num < 2 )
				return WG_EDIT_NONE;
			if ( next < 0 ) {
				to = 0;
			} else if ( next >= l.num ) {
				to = len;
			} else {
				const float x = WG_Width( &wg.style, buf + l.start[line], wg.cursor - l.start[line] );
				to = WG_IndexAlong( &wg.style, buf, &l, next, x );
			}
		}
		break;
	}
	default:
		return WG_EDIT_NONE;
	}

	wg.cursor = to;
	if ( !shift )
		wg.anchor = to;
	wg.blinkFrom = cls.realtime;
	return WG_EDIT_MOVED;
}

qboolean WG_EditChar( char *buf, int size, int ch, wgAllow_t allow ) {
	const char c = (char)ch;

	if ( wg.buf != buf )
		WG_Focus( buf );
	WG_Clamp( buf );
	if ( !WG_Allowed( (byte)c, allow ) )
		return qfalse;
	wg.blinkFrom = cls.realtime;
	return WG_Insert( buf, size, &c, 1, allow );
}

#define WG_MAX_RUNS		32

void WG_TypedText( float x, float y, float w, const char *text, const float *color, int font, float scale, qboolean centred, qboolean shadow ) {
	static char runs[WG_MAX_RUNS][MAX_STRING_CHARS / 4];
	float colors[WG_MAX_RUNS][4], total = 0;
	int n = 0, len = 0;

	memcpy( colors[0], color, sizeof( colors[0] ) );
	runs[0][0] = '\0';
	for ( const char *p = text; *p; p++ ) {
		const float *c = p[0] == Q_COLOR_ESCAPE && p[1] ? Con_TypedColor( p ) : NULL;

		if ( c ) {
			// a new colour: a new run, unless this one's still empty
			if ( len && n < WG_MAX_RUNS - 1 ) {
				runs[n][len] = '\0';
				n++;
				len = 0;
			}
			memcpy( colors[n], c, sizeof( float ) * 3 );
			colors[n][3] = color[3];
			p++;
			continue;
		}
		if ( len < (int)sizeof( runs[0] ) - 4 )
			runs[n][len++] = *p;
	}
	runs[n][len] = '\0';
	n++;

	for ( int i = 0; i < n; i++ )
		total += re->Font_StrLenPixels( runs[i], font, scale );
	// too long: the end cut off, ... in its place
	if ( total > w ) {
		const float dots = re->Font_StrLenPixels( "...", font, scale );

		while ( n > 0 && total + dots > w ) {
			const int l = (int)strlen( runs[n - 1] );

			if ( !l ) {
				n--;
				continue;
			}
			total -= re->Font_StrLenPixels( runs[n - 1], font, scale );
			runs[n - 1][l - 1] = '\0';
			total += re->Font_StrLenPixels( runs[n - 1], font, scale );
		}
		if ( n > 0 ) {
			Q_strcat( runs[n - 1], sizeof( runs[0] ), "..." );
			total += dots;
		}
	}
	if ( centred )
		x += ( w - total ) * 0.5f;
	for ( int i = 0; i < n; i++ ) {
		re->Font_DrawString( (int)x, (int)y, runs[i], colors[i], font | ( shadow ? STYLE_DROPSHADOW : 0 ), -1, scale );
		x += re->Font_StrLenPixels( runs[i], font, scale );
	}
}
/*
===============================================================================

THE - VALUE + CONTROL

A choice or a number, stepped with - and + (and the wheel over it). Its value, clicked, opens a
list of every choice under it (over it near the screen's bottom), or turns into a field the number
is typed in. One is open at a time, known by the value's address; its list is drawn over
everything by WG_DrawOverlay. A pick or a typed number is taken when the control is drawn next.

===============================================================================
*/

#define WG_POP_ROWS		10			// a list's rows shown; it scrolls for more
#define WG_POP_MAX		1024		// choices listed
#define WG_POP_MAX_W	300.0f

enum { WG_POP_NONE, WG_POP_LIST, WG_POP_NUMBER };

static const vec4_t wgPopBack = { 0.05f, 0.06f, 0.08f, 0.97f };
static const vec4_t wgPopHover = { 1.0f, 1.0f, 1.0f, 0.12f };

static struct {
	int			kind;
	const void	*id;
	float		ox, oy;				// where its control was when it opened: tools pass the same local
									// for several controls, so the address alone doesn't tell them apart
	int			seenFrame;			// the control was drawn then
	wgStyle_t	style;
	float		bx, by, bw, bh;		// its value box

	// a list
	std::vector<std::string>	labels;
	int			current;			// the value now
	int			sel;				// lit: the keys move it, the mouse over a row too
	int			scroll;
	int			pick;				// picked, taken when the control is drawn next; -1 none
	float		lx, ly, lw, lh, rowH;
	int			rows;
	qboolean	held;				// a click on it, held from the tool for it
	float		heldX, heldY;
	float		cursorX, cursorY, lastX, lastY;

	// a number
	char		buf[32];
	qboolean	apply;				// typed: set when the control is drawn next
} wgPop;

// the popup open is this control's: the same id, opened where it's drawn
static qboolean WG_PopIs( int kind, const void *id, float x, float y ) {
	return (qboolean)( wgPop.kind == kind && wgPop.id == id && fabsf( wgPop.ox - x ) < 0.5f && fabsf( wgPop.oy - y ) < 0.5f );
}

static void WG_PopClose( void ) {
	if ( wgPop.kind == WG_POP_NUMBER && wg.buf == wgPop.buf )
		WG_EditReset();
	wgPop.kind = WG_POP_NONE;
	wgPop.id = NULL;
	wgPop.labels.clear();
	wgPop.held = qfalse;
	wgPop.apply = qfalse;
	wgPop.pick = -1;
}

static qboolean WG_PtIn( float px, float py, float x, float y, float w, float h ) {
	return (qboolean)( px >= x && px < x + w && py >= y && py < y + h );
}

qboolean WG_PopupOpen( void ) {
	return (qboolean)( wgPop.kind != WG_POP_NONE );
}

qboolean WG_PopupTyping( void ) {
	return (qboolean)( wgPop.kind == WG_POP_NUMBER );
}

qboolean WG_PopupHover( float x, float y ) {
	return (qboolean)( wgPop.kind == WG_POP_LIST && WG_PtIn( x, y, wgPop.lx, wgPop.ly, wgPop.lw, wgPop.lh ) );
}

void WG_PopupClose( void ) {
	WG_PopClose();
}

void WG_PopupBegin( wgMouse_t *m ) {
	if ( wgPop.kind == WG_POP_NONE )
		return;
	// its control not drawn last frame (another page, the tool closed): gone
	if ( wgPop.seenFrame < cls.framecount - 1 ) {
		WG_PopClose();
		return;
	}
	wgPop.cursorX = m->cursorX;
	wgPop.cursorY = m->cursorY;
	if ( wgPop.kind == WG_POP_LIST ) {
		const qboolean over = WG_PtIn( m->cursorX, m->cursorY, wgPop.lx, wgPop.ly, wgPop.lw, wgPop.lh );

		if ( over && m->wheel && *m->wheel ) {
			wgPop.scroll += *m->wheel;
			*m->wheel = 0;
		}
		if ( m->click && *m->click ) {
			if ( WG_PtIn( m->clickX, m->clickY, wgPop.lx, wgPop.ly, wgPop.lw, wgPop.lh ) ) {
				wgPop.held = qtrue;
				wgPop.heldX = m->clickX;
				wgPop.heldY = m->clickY;
				*m->click = qfalse;
			} else if ( !WG_PtIn( m->clickX, m->clickY, wgPop.bx, wgPop.by, wgPop.bw, wgPop.bh ) ) {
				// elsewhere: closed, and the click goes on to what's there
				WG_PopClose();
			}
		}
	} else if ( m->click && *m->click && !WG_PtIn( m->clickX, m->clickY, wgPop.bx, wgPop.by, wgPop.bw, wgPop.bh ) ) {
		// a number: a click elsewhere sets what was typed
		wgPop.apply = qtrue;
	}
}

// the lit row kept in view
static void WG_PopShowSel( void ) {
	const int count = (int)wgPop.labels.size();

	wgPop.sel = Com_Clampi( 0, Q_max( 0, count - 1 ), wgPop.sel );
	if ( wgPop.sel < wgPop.scroll )
		wgPop.scroll = wgPop.sel;
	else if ( wgPop.sel >= wgPop.scroll + wgPop.rows )
		wgPop.scroll = wgPop.sel - wgPop.rows + 1;
}

static qboolean WG_NumberChar( int ch ) {
	return (qboolean)( ( ch >= '0' && ch <= '9' ) || ch == '.' || ch == '-' || ch == ',' );
}

qboolean WG_PopupKey( int key ) {
	if ( wgPop.kind == WG_POP_NONE )
		return qfalse;
	// the mouse is the tool's: its clicks come to the popup through WG_PopupBegin
	if ( ( key >= A_MOUSE1 && key <= A_MOUSE5 ) || key == A_MWHEELUP || key == A_MWHEELDOWN )
		return qfalse;
	if ( key == A_ESCAPE ) {
		WG_PopClose();
		return qtrue;
	}
	if ( wgPop.kind == WG_POP_NUMBER ) {
		if ( key == A_ENTER || key == A_KP_ENTER || key == A_TAB )
			wgPop.apply = qtrue;
		else
			WG_EditKey( wgPop.buf, sizeof( wgPop.buf ), key, WG_NumberChar );
		return qtrue;
	}
	switch ( key ) {
	case A_CURSOR_UP:	case A_KP_8:	wgPop.sel--; break;
	case A_CURSOR_DOWN:	case A_KP_2:	wgPop.sel++; break;
	case A_PAGE_UP:		case A_KP_9:	wgPop.sel -= wgPop.rows; break;
	case A_PAGE_DOWN:	case A_KP_3:	wgPop.sel += wgPop.rows; break;
	case A_HOME:		case A_KP_7:	wgPop.sel = 0; break;
	case A_END:			case A_KP_1:	wgPop.sel = (int)wgPop.labels.size() - 1; break;
	case A_ENTER:
	case A_KP_ENTER:
		wgPop.pick = wgPop.sel;
		return qtrue;
	case A_TAB:
		WG_PopClose();
		return qtrue;
	default:
		return qtrue;
	}
	WG_PopShowSel();
	return qtrue;
}

// a label's first letter or digit, past colours and spaces
static int WG_FirstLetter( const char *s ) {
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			s += 2;
			continue;
		}
		if ( isalnum( (unsigned char)*s ) )
			return tolower( (unsigned char)*s );
		s++;
	}
	return 0;
}

qboolean WG_PopupChar( int ch ) {
	if ( wgPop.kind == WG_POP_NONE )
		return qfalse;
	if ( wgPop.kind == WG_POP_NUMBER ) {
		if ( ch >= 32 && ch < 127 )
			WG_EditChar( wgPop.buf, sizeof( wgPop.buf ), ch, WG_NumberChar );
		return qtrue;
	}
	// a letter: the next one starting with it
	ch = tolower( ch & 255 );
	if ( isalnum( ch ) ) {
		const int count = (int)wgPop.labels.size();

		for ( int n = 1; n <= count; n++ ) {
			const int i = ( wgPop.sel + n ) % count;

			if ( WG_FirstLetter( wgPop.labels[i].c_str() ) == ch ) {
				wgPop.sel = i;
				WG_PopShowSel();
				break;
			}
		}
	}
	return qtrue;
}

// text in w, cut with ... when it's longer
static void WG_PopText( const wgStyle_t *s, float x, float y, float w, const char *text, const float *color, qboolean center ) {
	char buf[256];
	int len;
	float tw;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = (int)strlen( buf );
	tw = (float)re->Font_StrLenPixels( buf, s->font, s->scale );
	while ( len > 3 && tw > w ) {
		len--;
		buf[len - 3] = '.';
		buf[len - 2] = '.';
		buf[len - 1] = '.';
		buf[len] = '\0';
		tw = (float)re->Font_StrLenPixels( buf, s->font, s->scale );
	}
	if ( center )
		x += ( w - tw ) * 0.5f;
	re->Font_DrawString( (int)x, (int)y, buf, color, s->font, -1, s->scale );
}

static void WG_PopBox( float x, float y, float w, float h, const float *fill, const float *border ) {
	if ( fill )
		WG_Fill( x, y, w, h, fill );
	if ( border ) {
		WG_Fill( x, y, w, 1, border );
		WG_Fill( x, y + h - 1, w, 1, border );
		WG_Fill( x, y, 1, h, border );
		WG_Fill( x + w - 1, y, 1, h, border );
	}
}

// a choice drawn as one box, no - and + (WG_ChoiceBox): its value opens the list
static qboolean wgBare;

// - and +, the value between; how many steps they (or the wheel) went, and whether the value
// was clicked
static int WG_Steps( const wgStyle_t *st, const wgMouse_t *m, float x, float y, float w, float h, const char *shown,
	qboolean open, qboolean *valueClicked ) {
	const float bw = wgBare ? 0.0f : h, vx = wgBare ? x : x + bw + 1, vw = wgBare ? w : w - bw * 2 - 2;
	const qboolean live = (qboolean)( m->click != NULL );
	int steps = 0;

	*valueClicked = qfalse;
	for ( int b = 0; b < 2 && !wgBare; b++ ) {
		const float bx = b ? x + w - bw : x;

		WG_PopBox( bx, y, bw, h, st->fill, st->border );
		if ( live && WG_PtIn( m->cursorX, m->cursorY, bx, y, bw, h ) )
			WG_Fill( bx + 1, y + 1, bw - 2, h - 2, wgPopHover );
		WG_PopText( st, bx, y + st->padY, bw, b ? "+" : "-", st->text, qtrue );
		if ( live && *m->click && WG_PtIn( m->clickX, m->clickY, bx, y, bw, h ) ) {
			*m->click = qfalse;
			steps += b ? 1 : -1;
		}
	}
	WG_PopBox( vx, y, vw, h, open ? wgPopBack : st->fill, st->border );
	if ( live && !open && WG_PtIn( m->cursorX, m->cursorY, vx, y, vw, h ) )
		WG_Fill( vx + 1, y + 1, vw - 2, h - 2, wgPopHover );
	if ( shown )
		WG_PopText( st, vx + 2, y + st->padY, vw - 4, shown, st->text, qtrue );
	if ( live && *m->click && WG_PtIn( m->clickX, m->clickY, vx, y, vw, h ) ) {
		*m->click = qfalse;
		*valueClicked = qtrue;
	}
	// the wheel over it
	if ( live && !open && m->wheel && *m->wheel && WG_PtIn( m->cursorX, m->cursorY, x, y, w, h ) ) {
		steps -= *m->wheel;
		*m->wheel = 0;
	}
	return steps;
}

static void WG_PopPlace( const wgStyle_t *st, float x, float y, float w, float h ) {
	const float bw = wgBare ? 0.0f : h;

	wgPop.style = *st;
	wgPop.bx = wgBare ? x : x + bw + 1;
	wgPop.by = y;
	wgPop.bw = wgBare ? w : w - bw * 2 - 2;
	wgPop.bh = h;
	wgPop.seenFrame = cls.framecount;
}

qboolean WG_Choice( const wgStyle_t *st, const wgMouse_t *m, float x, float y, float w, float h,
	int *index, int count, wgLabel_t label, void *ctx, qboolean wrap ) {
	const qboolean open = WG_PopIs( WG_POP_LIST, index, x, y );
	const int before = *index;
	qboolean valueClicked;
	int steps;

	if ( open ) {
		// what's listed, as it is now
		const int n = Q_min( count, WG_POP_MAX );

		WG_PopPlace( st, x, y, w, h );
		wgPop.labels.resize( n );
		for ( int i = 0; i < n; i++ ) {
			const char *l = label( i, ctx );

			wgPop.labels[i] = l ? l : "";
		}
		if ( wgPop.pick >= 0 ) {
			if ( wgPop.pick < count )
				*index = wgPop.pick;
			WG_PopClose();
		}
	}
	steps = WG_Steps( st, m, x, y, w, h, *index >= 0 && *index < count ? label( *index, ctx ) : "", open, &valueClicked );
	if ( steps && count > 0 ) {
		if ( wrap )
			*index = ( ( *index + steps ) % count + count ) % count;
		else
			*index = Com_Clampi( 0, count - 1, *index + steps );
	}
	if ( valueClicked && count > 0 ) {
		if ( WG_PopIs( WG_POP_LIST, index, x, y ) ) {
			WG_PopClose();
		} else {
			const int n = Q_min( count, WG_POP_MAX );
			float widest = 0.0f;

			WG_PopClose();
			wgPop.kind = WG_POP_LIST;
			wgPop.id = index;
			wgPop.ox = x;
			wgPop.oy = y;
			wgPop.pick = -1;
			wgPop.labels.resize( n );
			for ( int i = 0; i < n; i++ ) {
				const char *l = label( i, ctx );

				wgPop.labels[i] = l ? l : "";
				widest = Q_max( widest, (float)re->Font_StrLenPixels( wgPop.labels[i].c_str(), st->font, st->scale ) );
			}
			WG_PopPlace( st, x, y, w, h );
			wgPop.current = wgPop.sel = Com_Clampi( 0, n - 1, *index );
			wgPop.rows = Q_min( n, WG_POP_ROWS );
			wgPop.rowH = h;
			wgPop.lw = Com_Clamp( wgPop.bw, WG_POP_MAX_W, widest + st->padX * 2 + 6 + ( n > wgPop.rows ? WG_SCROLL_W + 2 : 0 ) );
			wgPop.lh = wgPop.rows * wgPop.rowH + 2;
			wgPop.lx = Q_min( wgPop.bx, SCREEN_WIDTH - 2 - wgPop.lw );
			wgPop.ly = wgPop.by + wgPop.bh + 1;
			if ( wgPop.ly + wgPop.lh > SCREEN_HEIGHT - 2 )
				wgPop.ly = Q_max( 2.0f, wgPop.by - wgPop.lh - 1 );
			wgPop.scroll = 0;
			wgPop.lastX = m->cursorX;
			wgPop.lastY = m->cursorY;
			WG_PopShowSel();
			// the current one in the middle when there's room
			wgPop.scroll = Com_Clampi( 0, Q_max( 0, n - wgPop.rows ), wgPop.sel - wgPop.rows / 2 );
		}
	}
	return (qboolean)( *index != before );
}

typedef struct {
	const char	**names;
} wgNames_t;

static const char *WG_NameAt( int i, void *ctx ) {
	return ( (wgNames_t *)ctx )->names[i];
}

qboolean WG_ChoiceList( const wgStyle_t *st, const wgMouse_t *m, float x, float y, float w, float h,
	int *index, const char **names, int count, qboolean wrap ) {
	wgNames_t ctx = { names };

	return WG_Choice( st, m, x, y, w, h, index, count, WG_NameAt, &ctx, wrap );
}

// a choice as one box, no - and +: clicking it opens the list (the wheel over it still steps)
qboolean WG_ChoiceBox( const wgStyle_t *st, const wgMouse_t *m, float x, float y, float w, float h,
	int *index, const char **names, int count ) {
	qboolean changed;

	wgBare = qtrue;
	changed = WG_ChoiceList( st, m, x, y, w, h, index, names, count, qtrue );
	wgBare = qfalse;
	return changed;
}

qboolean WG_NumberAt( const void *id, const wgStyle_t *st, const wgMouse_t *m, float x, float y, float w, float h,
	float *v, float step, float min, float max, float scale, const char *shown ) {
	const qboolean open = WG_PopIs( WG_POP_NUMBER, id, x, y );
	const float before = *v;
	qboolean valueClicked;
	int steps;

	if ( !scale )
		scale = 1.0f;
	if ( open ) {
		WG_PopPlace( st, x, y, w, h );
		if ( wgPop.apply ) {
			char text[sizeof( wgPop.buf )];

			Q_strncpyz( text, wgPop.buf, sizeof( text ) );
			for ( char *c = text; *c; c++ ) {
				if ( *c == ',' )
					*c = '.';
			}
			if ( text[0] && text[0] != '.' && strcmp( text, "-" ) )
				*v = Com_Clamp( min, max, (float)atof( text ) / scale );
			WG_PopClose();
		}
	}
	if ( WG_PopIs( WG_POP_NUMBER, id, x, y ) ) {
		const float bw = h, vx = x + bw + 1, vw = w - bw * 2 - 2;

		// typed in: - and + still step it; a click in it moves the cursor
		steps = WG_Steps( st, m, x, y, w, h, NULL, qtrue, &valueClicked );
		WG_Field( st, vx, y, vw, h, wgPop.buf, qtrue, NULL, m->cursorX, m->cursorY );
		if ( valueClicked )
			WG_FieldClick( wgPop.buf, m->clickX, m->clickY );
		if ( steps ) {
			*v = Com_Clamp( min, max, *v + steps * step );
			Com_sprintf( wgPop.buf, sizeof( wgPop.buf ), "%g", *v * scale );
			WG_EditReset();
		}
		return (qboolean)( *v != before );
	}
	steps = WG_Steps( st, m, x, y, w, h, shown, qfalse, &valueClicked );
	if ( steps )
		*v = Com_Clamp( min, max, *v + steps * step );
	if ( valueClicked ) {
		int len;

		WG_PopClose();
		wgPop.kind = WG_POP_NUMBER;
		wgPop.id = id;
		wgPop.ox = x;
		wgPop.oy = y;
		WG_PopPlace( st, x, y, w, h );
		Com_sprintf( wgPop.buf, sizeof( wgPop.buf ), "%g", *v * scale );
		// all of it chosen: what's typed takes its place
		WG_Focus( wgPop.buf );
		len = (int)strlen( wgPop.buf );
		wg.anchor = 0;
		wg.cursor = len;
	}
	return (qboolean)( *v != before );
}

qboolean WG_Number( const wgStyle_t *st, const wgMouse_t *m, float x, float y, float w, float h,
	float *v, float step, float min, float max, float scale, const char *shown ) {
	return WG_NumberAt( v, st, m, x, y, w, h, v, step, min, max, scale, shown );
}

// an open list, over everything: drawn by WG_DrawOverlay
static void WG_DrawPopList( void ) {
	const wgStyle_t *st = &wgPop.style;
	const int count = (int)wgPop.labels.size();
	const qboolean bar = (qboolean)( count > wgPop.rows );
	const float rw = wgPop.lw - 2 - ( bar ? WG_SCROLL_W + 1 : 0 );
	int scroll;

	if ( wgPop.kind != WG_POP_LIST || wgPop.seenFrame != cls.framecount || !count )
		return;
	wgPop.scroll = Com_Clampi( 0, Q_max( 0, count - wgPop.rows ), wgPop.scroll );
	WG_PopBox( wgPop.lx, wgPop.ly, wgPop.lw, wgPop.lh, wgPopBack, st->border );
	if ( bar ) {
		scroll = wgPop.scroll;
		WG_Scrollbar( wgPop.lx + wgPop.lw - WG_SCROLL_W - 1, wgPop.ly + 1, wgPop.lh - 2, count, wgPop.rows, &scroll,
			wgPop.cursorX, wgPop.cursorY, &wgPop.held, wgPop.heldX, wgPop.heldY );
		wgPop.scroll = scroll;
	}
	// the mouse moved over a row: it's lit
	if ( ( wgPop.cursorX != wgPop.lastX || wgPop.cursorY != wgPop.lastY )
		&& WG_PtIn( wgPop.cursorX, wgPop.cursorY, wgPop.lx + 1, wgPop.ly + 1, rw, wgPop.rows * wgPop.rowH ) ) {
		wgPop.sel = wgPop.scroll + (int)( ( wgPop.cursorY - wgPop.ly - 1 ) / wgPop.rowH );
		wgPop.sel = Com_Clampi( 0, count - 1, wgPop.sel );
	}
	wgPop.lastX = wgPop.cursorX;
	wgPop.lastY = wgPop.cursorY;
	for ( int r = 0; r < wgPop.rows && wgPop.scroll + r < count; r++ ) {
		const int i = wgPop.scroll + r;
		const float ry = wgPop.ly + 1 + r * wgPop.rowH;

		if ( i == wgPop.current )
			WG_Fill( wgPop.lx + 1, ry, rw, wgPop.rowH, wgSelect );
		if ( i == wgPop.sel )
			WG_Fill( wgPop.lx + 1, ry, rw, wgPop.rowH, wgPopHover );
		WG_PopText( st, wgPop.lx + 1 + st->padX, ry + st->padY, rw - st->padX * 2, wgPop.labels[i].c_str(),
			i == wgPop.current ? st->text : st->dim ? st->dim : st->text, qfalse );
		if ( wgPop.held && WG_PtIn( wgPop.heldX, wgPop.heldY, wgPop.lx + 1, ry, rw, wgPop.rowH ) ) {
			wgPop.held = qfalse;
			wgPop.pick = i;
		}
	}
	wgPop.held = qfalse;
}
