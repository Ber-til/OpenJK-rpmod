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

// cl_escpages.h -- the Setup, Renderer and Controls pages the Modern and KOTOR Esc menus draw in place of
// JKA's own (cl_escpages.cpp), and the menu's look they're drawn in (cl_escmenu.cpp)

#pragma once

#include "cl_widgets.h"

enum { ESCPAGE_SETUP, ESCPAGE_RENDERER, ESCPAGE_CONTROLS };

void		CL_EscPages_Open( int page );
void		CL_EscPages_Close( void );
void		CL_EscPages_Draw( int page, float x, float y, float w, float h );
qboolean	CL_EscPages_Key( int page, int key );		// qtrue: the page took it
void		CL_EscPages_Char( int page, int ch );
qboolean	CL_EscPages_Escape( void );				// qtrue: a key waited for, or a field typed in, given up

// the menu's look (cl_escmenu.cpp)
qboolean	EscUI_Kotor( void );
float		EscUI_Wide( float w );
void		EscUI_Fill( float x, float y, float w, float h, const float *color );
void		EscUI_Frame( float x, float y, float w, float h, const float *color );
void		EscUI_Text( float x, float y, float w, const char *text, const float *color, float scale, int align );
qboolean	EscUI_Button( float x, float y, float w, float h, const char *label, int kind );	// kind 0 plain, 1 main, 2 danger
qboolean	EscUI_In( float x, float y, float w, float h );
qboolean	EscUI_Clicked( float x, float y, float w, float h );
void		EscUI_StyleWG( wgStyle_t *style, const float *text, float scale );
void		EscUI_Mouse( float *cursorX, float *cursorY, qboolean **click, float *clickX, float *clickY );
void		EscUI_Sound( qboolean click );
const float	*EscUI_Color( int which );		// 0 text, 1 dim, 2 accent, 3 row picked, 4 row hovered, 5 line, 6 danger
void		EscUI_Restarting( void );		// the video restarts: the menu closes
