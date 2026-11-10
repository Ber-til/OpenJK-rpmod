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

// cl_gizmo.cpp -- arrows along X, Y and Z over what's picked, in the model, effect and
// cinematic managers: one dragged with the mouse moves it along that axis only
//
// Each manager keeps its own view (where it looks from, how wide) and a gzDrag_t; it draws the
// arrows over its picture, asks which one is under a click, then each frame of the drag how far
// along the axis the mouse has gone. The arrows can go along any three ways instead of the world's
// (the holster's, along a tag's own), and rings round three axes turn what's picked.

#include "client.h"
#include "cl_gizmo.h"

#define GZ_LEN_FRAC		0.16f		// the arrows are this much of how far off they are
#define GZ_LEN_MIN		8.0f
#define GZ_LEN_MAX		320.0f
#define GZ_GRAB			7.0f		// an arrow is grabbed this near, on the screen
#define GZ_DOT			1.6f		// how thick they're drawn

static const vec4_t gzColors[3] = {
	{ 1.0f, 0.3f, 0.3f, 1.0f },		// X
	{ 0.35f, 1.0f, 0.35f, 1.0f },	// Y
	{ 0.35f, 0.55f, 1.0f, 1.0f },	// Z
};
static const vec4_t gzHot = { 1.0f, 0.9f, 0.2f, 1.0f };
static const char *gzNames[3] = { "X", "Y", "Z" };

qboolean GZ_Project( const gzView_t *v, const vec3_t point, float *x, float *y ) {
	vec3_t d;
	float forward;

	VectorSubtract( point, v->org, d );
	forward = DotProduct( d, v->axis[0] );
	if ( forward < 2.0f || v->fovX <= 0.0f || v->fovY <= 0.0f )
		return qfalse;
	*x = SCREEN_WIDTH * 0.5f * ( 1.0f - DotProduct( d, v->axis[1] ) / forward / tanf( DEG2RAD( v->fovX * 0.5f ) ) );
	*y = SCREEN_HEIGHT * 0.5f * ( 1.0f - DotProduct( d, v->axis[2] ) / forward / tanf( DEG2RAD( v->fovY * 0.5f ) ) );
	return qtrue;
}

void GZ_ViewFromAngles( gzView_t *v, const vec3_t org, const vec3_t angles, float fovX, float fovY ) {
	VectorCopy( org, v->org );
	AnglesToAxis( angles, v->axis );
	v->fovX = fovX;
	v->fovY = fovY;
}

// how long the arrows are in the world, to look the same size wherever it is
static float GZ_Length( const gzView_t *v, const vec3_t pivot ) {
	return Com_Clamp( GZ_LEN_MIN, GZ_LEN_MAX, Distance( v->org, pivot ) * GZ_LEN_FRAC );
}

static const vec3_t gzWorld[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

// where an arrow along dir starts and ends on the screen; qfalse if it can't be seen
static qboolean GZ_Arrow( const gzView_t *v, const vec3_t pivot, const vec3_t dir, float *x0, float *y0, float *x1, float *y1 ) {
	vec3_t tip;

	VectorMA( pivot, GZ_Length( v, pivot ), dir, tip );
	return (qboolean)( GZ_Project( v, pivot, x0, y0 ) && GZ_Project( v, tip, x1, y1 ) );
}

static void GZ_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
}

static void GZ_Line( float x0, float y0, float x1, float y1, const float *color ) {
	const float len = sqrtf( ( x1 - x0 ) * ( x1 - x0 ) + ( y1 - y0 ) * ( y1 - y0 ) );
	const int steps = Com_Clampi( 1, 600, (int)( len / 0.8f ) );

	for ( int i = 0; i <= steps; i++ ) {
		const float f = (float)i / steps;

		GZ_Fill( x0 + ( x1 - x0 ) * f - GZ_DOT * 0.5f, y0 + ( y1 - y0 ) * f - GZ_DOT * 0.5f, GZ_DOT, GZ_DOT, color );
	}
}

// the nearest the point comes to the segment, on the screen
static float GZ_SegmentDist( float px, float py, float x0, float y0, float x1, float y1 ) {
	const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
	float f = len2 > 0.0001f ? ( ( px - x0 ) * dx + ( py - y0 ) * dy ) / len2 : 0.0f;

	f = Com_Clamp( 0.0f, 1.0f, f );
	return sqrtf( Square( px - ( x0 + dx * f ) ) + Square( py - ( y0 + dy * f ) ) );
}

int GZ_HitAxes( const gzView_t *v, const vec3_t pivot, const vec3_t dirs[3], float mx, float my ) {
	float best = GZ_GRAB;
	int found = -1;

	for ( int a = 0; a < 3; a++ ) {
		float x0, y0, x1, y1, d;

		if ( !GZ_Arrow( v, pivot, dirs[a], &x0, &y0, &x1, &y1 ) )
			continue;
		// from a little out of the middle, where they all meet
		d = GZ_SegmentDist( mx, my, x0 + ( x1 - x0 ) * 0.15f, y0 + ( y1 - y0 ) * 0.15f, x1, y1 );
		if ( d < best ) {
			best = d;
			found = a;
		}
	}
	return found;
}

int GZ_Hit( const gzView_t *v, const vec3_t pivot, float mx, float my ) {
	return GZ_HitAxes( v, pivot, gzWorld, mx, my );
}

void GZ_DrawAxes( const gzView_t *v, const vec3_t pivot, const vec3_t dirs[3], int hot, const char * const *names ) {
	float cx, cy;

	if ( !GZ_Project( v, pivot, &cx, &cy ) )
		return;
	for ( int a = 0; a < 3; a++ ) {
		const float *color = a == hot ? gzHot : gzColors[a];
		float x0, y0, x1, y1;

		if ( !GZ_Arrow( v, pivot, dirs[a], &x0, &y0, &x1, &y1 ) )
			continue;
		GZ_Line( x0, y0, x1, y1, color );
		GZ_Fill( x1 - 3, y1 - 3, 6, 6, color );
		re->Font_DrawString( (int)( x1 + 5 ), (int)( y1 - 6 ), names ? names[a] : gzNames[a], color, cls.menuFont | STYLE_DROPSHADOW, -1, 0.7f );
	}
	GZ_Fill( cx - 2, cy - 2, 4, 4, colorWhite );
	re->SetColor( NULL );
}

void GZ_Draw( const gzView_t *v, const vec3_t pivot, int hot ) {
	GZ_DrawAxes( v, pivot, gzWorld, hot, NULL );
}

void GZ_BeginDir( gzDrag_t *drag, const gzView_t *v, const vec3_t pivot, const vec3_t dir, int axis, float mx, float my ) {
	float x0, y0, x1, y1;
	const float len = GZ_Length( v, pivot );

	memset( drag, 0, sizeof( *drag ) );
	drag->axis = axis;
	drag->startX = mx;
	drag->startY = my;
	VectorCopy( pivot, drag->from );
	if ( !GZ_Arrow( v, pivot, dir, &x0, &y0, &x1, &y1 ) )
		return;
	// screen pixels a world unit along the axis
	drag->sx = ( x1 - x0 ) / len;
	drag->sy = ( y1 - y0 ) / len;
	drag->on = (qboolean)( drag->sx * drag->sx + drag->sy * drag->sy > 1e-6f );
}

void GZ_Begin( gzDrag_t *drag, const gzView_t *v, const vec3_t pivot, int axis, float mx, float my ) {
	GZ_BeginDir( drag, v, pivot, gzWorld[axis], axis, mx, my );
}

float GZ_Amount( const gzDrag_t *drag, float mx, float my, int grid ) {
	const float s2 = drag->sx * drag->sx + drag->sy * drag->sy;
	float amount;

	if ( !drag->on || s2 <= 1e-6f )
		return 0.0f;
	amount = ( ( mx - drag->startX ) * drag->sx + ( my - drag->startY ) * drag->sy ) / s2;
	if ( grid > 1 )
		amount = floorf( amount / grid + 0.5f ) * grid;
	return amount;
}

void GZ_Target( const gzDrag_t *drag, float mx, float my, int grid, vec3_t out ) {
	VectorCopy( drag->from, out );
	out[drag->axis] += GZ_Amount( drag, mx, my, grid );
}

void GZ_DrawCursor( float x, float y ) {
	re->SetColor( NULL );
	re->DrawStretchPic( x, y, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

/*
===============================================================================

Rings: turning round three axes

===============================================================================
*/

#define GZ_RING_SEGS	48
#define GZ_RING_FRAC	0.8f		// as big as the arrows are long, about

// two ways square to axis and each other, the ring's plane
void GZ_Basis( const vec3_t axis, vec3_t u, vec3_t v ) {
	vec3_t other;

	VectorSet( other, fabsf( axis[2] ) < 0.9f ? 0 : 1, 0, fabsf( axis[2] ) < 0.9f ? 1 : 0 );
	CrossProduct( axis, other, u );
	VectorNormalize( u );
	CrossProduct( axis, u, v );
}

// AnglesToAxis the other way round; at a pitch of 90 the roll is left at 0 and the yaw does it all
void GZ_AxisAngles( const matrix3_t axis, vec3_t angles ) {
	const float pitch = asinf( Com_Clamp( -1.0f, 1.0f, -axis[0][2] ) );

	angles[PITCH] = RAD2DEG( pitch );
	if ( cosf( pitch ) > 1e-4f ) {
		angles[YAW] = RAD2DEG( atan2f( axis[0][1], axis[0][0] ) );
		angles[ROLL] = RAD2DEG( atan2f( axis[1][2], axis[2][2] ) );
	} else {
		angles[YAW] = RAD2DEG( atan2f( -axis[1][0], axis[1][1] ) );
		angles[ROLL] = 0.0f;
	}
}

// a ring's points on the screen; qfalse if it can't all be seen
static qboolean GZ_Ring( const gzView_t *v, const vec3_t pivot, const vec3_t axis, float *xs, float *ys ) {
	const float radius = GZ_Length( v, pivot ) * GZ_RING_FRAC;
	vec3_t u, w, p;

	GZ_Basis( axis, u, w );
	for ( int i = 0; i <= GZ_RING_SEGS; i++ ) {
		const float a = i * 2.0f * M_PI / GZ_RING_SEGS;

		VectorMA( pivot, radius * cosf( a ), u, p );
		VectorMA( p, radius * sinf( a ), w, p );
		if ( !GZ_Project( v, p, &xs[i], &ys[i] ) )
			return qfalse;
	}
	return qtrue;
}

int GZ_HitRings( const gzView_t *v, const vec3_t pivot, const vec3_t axes[3], float mx, float my ) {
	float xs[GZ_RING_SEGS + 1], ys[GZ_RING_SEGS + 1], best = GZ_GRAB;
	int found = -1;

	for ( int k = 0; k < 3; k++ ) {
		if ( !GZ_Ring( v, pivot, axes[k], xs, ys ) )
			continue;
		for ( int i = 0; i < GZ_RING_SEGS; i++ ) {
			const float d = GZ_SegmentDist( mx, my, xs[i], ys[i], xs[i + 1], ys[i + 1] );

			if ( d < best ) {
				best = d;
				found = k;
			}
		}
	}
	return found;
}

void GZ_DrawRings( const gzView_t *v, const vec3_t pivot, const vec3_t axes[3], int hot ) {
	float xs[GZ_RING_SEGS + 1], ys[GZ_RING_SEGS + 1], cx, cy;

	if ( !GZ_Project( v, pivot, &cx, &cy ) )
		return;
	for ( int k = 0; k < 3; k++ ) {
		const float *color = k == hot ? gzHot : gzColors[k];

		if ( !GZ_Ring( v, pivot, axes[k], xs, ys ) )
			continue;
		for ( int i = 0; i < GZ_RING_SEGS; i++ )
			GZ_Line( xs[i], ys[i], xs[i + 1], ys[i + 1], color );
	}
	GZ_Fill( cx - 2, cy - 2, 4, 4, colorWhite );
	re->SetColor( NULL );
}

float GZ_ScreenAngle( const gzView_t *v, const vec3_t pivot, float mx, float my ) {
	float cx, cy;

	if ( !GZ_Project( v, pivot, &cx, &cy ) )
		return 0.0f;
	return atan2f( my - cy, mx - cx );
}

// which way round on the screen a turn by RotatePointAroundVector's positive degrees goes: 1 as the
// screen angle grows, -1 against
float GZ_RingSign( const gzView_t *v, const vec3_t pivot, const vec3_t axis ) {
	vec3_t u, w, p0, p1;
	float cx, cy, x0, y0, x1, y1;

	GZ_Basis( axis, u, w );
	VectorAdd( pivot, u, p0 );
	RotatePointAroundVector( p1, axis, u, 10.0f );
	VectorAdd( pivot, p1, p1 );
	if ( !GZ_Project( v, pivot, &cx, &cy ) || !GZ_Project( v, p0, &x0, &y0 ) || !GZ_Project( v, p1, &x1, &y1 ) )
		return 1.0f;
	return ( ( x0 - cx ) * ( y1 - cy ) - ( y0 - cy ) * ( x1 - cx ) ) >= 0.0f ? 1.0f : -1.0f;
}
