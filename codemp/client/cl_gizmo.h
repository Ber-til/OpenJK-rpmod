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

// cl_gizmo.h -- the move arrows the managers share; see cl_gizmo.cpp

#pragma once

#define GZ_KEY		A_CAP_J		// shows or hides them, in each manager's free camera

// where a manager's picture is seen from
typedef struct {
	vec3_t		org;
	vec3_t		axis[3];
	float		fovX, fovY;
} gzView_t;

// an arrow held
typedef struct {
	qboolean	on;
	int			axis;			// 0 X, 1 Y, 2 Z
	vec3_t		from;			// where it all was as it was grabbed
	float		startX, startY;	// where the mouse was
	float		sx, sy;			// screen pixels a world unit along the axis
} gzDrag_t;

void GZ_ViewFromAngles( gzView_t *v, const vec3_t org, const vec3_t angles, float fovX, float fovY );
qboolean GZ_Project( const gzView_t *v, const vec3_t point, float *x, float *y );
// the arrows over pivot, hot drawn lit; the one at the mouse, -1 none
void GZ_Draw( const gzView_t *v, const vec3_t pivot, int hot );
int GZ_Hit( const gzView_t *v, const vec3_t pivot, float mx, float my );
// grabbed; then, as the mouse goes, how far along it is (snapped to grid), or where it is
void GZ_Begin( gzDrag_t *drag, const gzView_t *v, const vec3_t pivot, int axis, float mx, float my );
float GZ_Amount( const gzDrag_t *drag, float mx, float my, int grid );
void GZ_Target( const gzDrag_t *drag, float mx, float my, int grid, vec3_t out );
void GZ_DrawCursor( float x, float y );

// the same along any three ways (unit vectors), not the world's; names NULL for X Y Z
void GZ_DrawAxes( const gzView_t *v, const vec3_t pivot, const vec3_t dirs[3], int hot, const char * const *names );
int GZ_HitAxes( const gzView_t *v, const vec3_t pivot, const vec3_t dirs[3], float mx, float my );
void GZ_BeginDir( gzDrag_t *drag, const gzView_t *v, const vec3_t pivot, const vec3_t dir, int axis, float mx, float my );

// rings round three axes (unit vectors), to turn; the one at the mouse, -1 none
void GZ_DrawRings( const gzView_t *v, const vec3_t pivot, const vec3_t axes[3], int hot );
int GZ_HitRings( const gzView_t *v, const vec3_t pivot, const vec3_t axes[3], float mx, float my );
// the mouse's angle round pivot on the screen, and which way a positive turn round axis goes there
float GZ_ScreenAngle( const gzView_t *v, const vec3_t pivot, float mx, float my );
float GZ_RingSign( const gzView_t *v, const vec3_t pivot, const vec3_t axis );
void GZ_Basis( const vec3_t axis, vec3_t u, vec3_t v );
void GZ_AxisAngles( const matrix3_t axis, vec3_t angles );
