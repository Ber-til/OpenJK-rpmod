/*
===========================================================================
Copyright (C) 2026 OpenJK-rpmod contributors

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

// cl_dismember.cpp -- a body cut for the story, on every screen with this client
//
// RPMod's Jedi don't die: fakeDeath knocks them out for a while, so the server never cuts them. An
// RPMod admin cuts one here: /dismember <who> <part> [fall|stay] [die|<animation>], or restore.
// It goes through C1B3R to everyone with this client (cmd:dismember/<num>/<part>/<fall>/<animation
// or ->, cmd:dismember/<num>/restore), and C1B3R keeps it for who joins later until the map loads
// again (serverstate.py).
//
// The body is cut as JKA's cgame cuts one (cg_ents.c): the part's surfaces off on the model the cgame
// draws, the stump's cap on; the right arm or hand cut, its player's own game drops their weapon
// (rpdrop weapon), unless it's a saber. With fall, a copy of the model rooted at the part drops from the cut,
// tumbles and lies where it lands; with an animation (die: BOTH_DEATH1) they play it once and stay on
// its last frame, through the cinematic animations (cl_rpcinematic.cpp). Restore puts it all back.

#include "client.h"
#include "qcommon/cm_public.h"
#include "cl_rpcinematic.h"
#include "cl_presence.h"

#define DM_MAX_CUT		32			// bodies cut at once
#define DM_MAX_LIMBS	24			// limbs lying about
#define DM_NEAR			64.0f		// a body drawn this near the entity is its
#define DM_LIMB_MS		( 10 * 60 * 1000 )	// a limb lies this long at most
#define DM_GRAVITY		800.0f
#define DM_CRANDOM()	flrand( -1.0f, 1.0f )

typedef enum { DM_HEAD, DM_TORSO, DM_LARM, DM_RARM, DM_RHAND, DM_LLEG, DM_RLEG, DM_NUM_PARTS } dmPart_t;

// each part as the cgame cuts it: the surface (variants found as it does), the one it's cut from,
// the caps (%s the surfaces' names), the bone it turns on, and the tag where it was cut
static const struct {
	const char	*word;
	const char	*limb, *stub;
	qboolean	variants;
	const char	*limbCap, *stubCap;
	const char	*bone, *tag;
} dmParts[DM_NUM_PARTS] = {
	{ "head",	"head",		"torso",	qfalse,	"head_cap_torso",	"torso_cap_head",	"cranium",	"*head_cap_torso" },
	{ "torso",	"torso",	"hips",		qfalse,	"torso_cap_hips",	"hips_cap_torso",	"thoracic",	"*torso_cap_hips" },
	{ "larm",	"l_arm",	"torso",	qtrue,	"%s_cap_torso",		"%s_cap_l_arm",		"lradius",	"*l_arm_cap_torso" },
	{ "rarm",	"r_arm",	"torso",	qtrue,	"%s_cap_torso",		"%s_cap_r_arm",		"rradius",	"*r_arm_cap_torso" },
	{ "rhand",	"r_hand",	"r_arm",	qtrue,	"%s_cap_r_arm",		"%s_cap_r_hand",	"rhand",	"*r_hand_cap_r_arm" },
	{ "lleg",	"l_leg",	"hips",		qtrue,	"%s_cap_hips",		"%s_cap_l_leg",		"ltibia",	"*l_leg_cap_hips" },
	{ "rleg",	"r_leg",	"hips",		qtrue,	"%s_cap_hips",		"%s_cap_r_leg",		"rtibia",	"*r_leg_cap_hips" },
};

typedef struct {
	int			num;				// the entity, -1 a free slot
	int			cut;				// parts cut (1 << dmPart_t)
	int			fall;				// of those, the ones still to drop from the body when it's next drawn
	int			restore;			// parts to put back the next time it's drawn
	int			restoreUntil;
} dmBody_t;

typedef struct {
	qboolean		active;
	int				num, part;
	CGhoul2Info_v	*g2;
	vec3_t			org, vel, ang, spin;
	float			scale;
	qboolean		resting;
	int				start;
} dmLimb_t;

static dmBody_t	dmBodies[DM_MAX_CUT];
static dmLimb_t	dmLimbs[DM_MAX_LIMBS];

static int DM_PartOf( const char *word ) {
	for ( int i = 0; i < DM_NUM_PARTS; i++ ) {
		if ( !Q_stricmp( word, dmParts[i].word ) )
			return i;
	}
	// a few other words for them
	if ( !Q_stricmp( word, "waist" ) || !Q_stricmp( word, "body" ) )		return DM_TORSO;
	if ( !Q_stricmp( word, "leftarm" ) )		return DM_LARM;
	if ( !Q_stricmp( word, "rightarm" ) )		return DM_RARM;
	if ( !Q_stricmp( word, "hand" ) || !Q_stricmp( word, "righthand" ) )	return DM_RHAND;
	if ( !Q_stricmp( word, "leftleg" ) )		return DM_LLEG;
	if ( !Q_stricmp( word, "rightleg" ) )		return DM_RLEG;
	return -1;
}

static dmBody_t *DM_Body( int num, qboolean make ) {
	dmBody_t *free = NULL;

	for ( int i = 0; i < DM_MAX_CUT; i++ ) {
		if ( dmBodies[i].num == num && ( dmBodies[i].cut || dmBodies[i].restore ) )
			return &dmBodies[i];
		if ( !free && !dmBodies[i].cut && !dmBodies[i].restore )
			free = &dmBodies[i];
	}
	if ( !make || !free )
		return NULL;
	memset( free, 0, sizeof( *free ) );
	free->num = num;
	return free;
}

static void DM_FreeLimb( dmLimb_t *l ) {
	if ( l->g2 )
		re->G2API_CleanGhoul2Models( &l->g2 );
	memset( l, 0, sizeof( *l ) );
}

static void DM_FreeLimbsOf( int num ) {
	for ( int i = 0; i < DM_MAX_LIMBS; i++ ) {
		if ( dmLimbs[i].active && dmLimbs[i].num == num )
			DM_FreeLimb( &dmLimbs[i] );
	}
}

// a part's surface on this model: its own name, or the first variant of it that's on (as the cgame's
// BG_GetRootSurfNameWithVariant)
static void DM_Surface( CGhoul2Info_v &g2, const char *root, qboolean variants, char *out, int size ) {
	if ( !variants || !re->G2API_GetSurfaceRenderStatus( g2, 0, root ) ) {
		Q_strncpyz( out, root, size );
		return;
	}
	for ( int i = 0; i < 5; i++ ) {
		Com_sprintf( out, size, "%s%c", root, 'a' + i );
		if ( !re->G2API_GetSurfaceRenderStatus( g2, 0, out ) )
			return;
	}
	Q_strncpyz( out, root, size );
}

static void DM_Names( CGhoul2Info_v &g2, int part, char *limb, char *limbCap, char *stubCap, int size ) {
	char stub[MAX_QPATH];

	DM_Surface( g2, dmParts[part].limb, dmParts[part].variants, limb, size );
	DM_Surface( g2, dmParts[part].stub, dmParts[part].variants, stub, sizeof( stub ) );
	Com_sprintf( limbCap, size, dmParts[part].limbCap, limb );
	Com_sprintf( stubCap, size, dmParts[part].stubCap, stub );
}

// the part's limb, dropping from where it was cut on this body
static void DM_Drop( const refEntity_t *ent, int num, int part ) {
	char limb[MAX_QPATH], limbCap[MAX_QPATH], stubCap[MAX_QPATH];
	CGhoul2Info_v &body = *(CGhoul2Info_v *)ent->ghoul2;
	dmLimb_t *l = NULL;
	mdxaBone_t matrix;
	vec3_t angles, scale;
	int bolt;

	for ( int i = 0; i < DM_MAX_LIMBS && !l; i++ ) {
		if ( !dmLimbs[i].active )
			l = &dmLimbs[i];
	}
	if ( !l )
		return;
	DM_Names( body, part, limb, limbCap, stubCap, sizeof( limb ) );
	re->G2API_DuplicateGhoul2Instance( body, &l->g2 );
	if ( !l->g2 )
		return;
	l->active = qtrue;
	l->num = num;
	l->part = part;
	l->start = cls.realtime;
	l->scale = ent->modelScale[0] > 0.0f ? ent->modelScale[0] : 1.0f;
	// from the cut, as the body stands now
	vectoangles( ent->axis[0], angles );
	VectorCopy( ent->modelScale, scale );
	if ( !scale[0] )
		VectorSet( scale, 1, 1, 1 );
	VectorCopy( ent->origin, l->org );
	bolt = re->G2API_AddBolt( body, 0, dmParts[part].tag );
	if ( bolt >= 0 && re->G2API_GetBoltMatrix( body, 0, bolt, &matrix, angles, ent->origin, cl.serverTime, NULL, scale ) )
		VectorSet( l->org, matrix.matrix[0][3], matrix.matrix[1][3], matrix.matrix[2][3] );
	VectorSet( l->ang, 0, angles[YAW], 0 );
	// rooted at the part, turning on its bone, its own cap on
	re->G2API_SetRootSurface( *l->g2, 0, limb );
	re->G2API_SetNewOrigin( *l->g2, re->G2API_AddBolt( *l->g2, 0, dmParts[part].bone ) );
	re->G2API_SetSurfaceOnOff( *l->g2, limbCap, 0 );
	// a little push off the body, and a turn
	VectorSet( l->vel, DM_CRANDOM() * 60.0f, DM_CRANDOM() * 60.0f, flrand( 80.0f, 160.0f ) );
	VectorSet( l->spin, DM_CRANDOM() * 300.0f, DM_CRANDOM() * 200.0f, DM_CRANDOM() * 300.0f );
}

// a model on its way to the renderer: a body cut, as it's cut (and its limbs dropped the first time)
void CL_Dismember_Entity( const refEntity_t *ent ) {
	int nearest = -2;
	const char *gla;

	if ( !ent || ent->reType != RT_MODEL || !ent->ghoul2 )
		return;
	for ( int i = 0; i < DM_MAX_CUT; i++ ) {
		dmBody_t *b = &dmBodies[i];
		vec3_t org;

		if ( ( !b->cut && !b->restore ) || !RC_EntityOrigin( b->num, org ) || Distance( org, ent->origin ) > DM_NEAR )
			continue;
		// someone else nearer is who it is: two standing close each keep their own body
		if ( nearest == -2 )
			nearest = CL_BodyOwner( ent->origin, DM_NEAR );
		if ( nearest != b->num )
			continue;
		CGhoul2Info_v &g2 = *(CGhoul2Info_v *)ent->ghoul2;
		if ( !re->G2API_HaveWeGhoul2Models( g2 ) || !( gla = re->G2API_GetGLAName( g2, 0 ) ) || !Q_stristr( gla, "humanoid" ) )
			return;		// a weapon, a saber
		for ( int part = 0; part < DM_NUM_PARTS; part++ ) {
			char limb[MAX_QPATH], limbCap[MAX_QPATH], stubCap[MAX_QPATH];

			if ( !( ( b->cut | b->restore ) & ( 1 << part ) ) )
				continue;
			DM_Names( g2, part, limb, limbCap, stubCap, sizeof( limb ) );
			if ( b->cut & ( 1 << part ) ) {
				if ( b->fall & ( 1 << part ) ) {
					DM_Drop( ent, b->num, part );
					b->fall &= ~( 1 << part );
				}
				re->G2API_SetSurfaceOnOff( g2, limb, G2SURFACEFLAG_NODESCENDANTS );
				re->G2API_SetSurfaceOnOff( g2, stubCap, 0 );
			} else {
				// put back: whole again, the cap off
				re->G2API_SetSurfaceOnOff( g2, limb, 0 );
				re->G2API_SetSurfaceOnOff( g2, stubCap, G2SURFACEFLAG_OFF );
			}
		}
		b->restore = 0;
		return;
	}
}

// the limbs lying about: falling, tumbling, at rest
void CL_Dismember_AddToScene( const refdef_t *fd ) {
	const float dt = Com_Clamp( 0.0f, 0.1f, cls.frametime / 1000.0f );

	if ( !fd || ( fd->rdflags & RDF_NOWORLDMODEL ) || cls.state != CA_ACTIVE )
		return;
	for ( int i = 0; i < DM_MAX_LIMBS; i++ ) {
		dmLimb_t *l = &dmLimbs[i];
		refEntity_t ent;

		if ( !l->active )
			continue;
		if ( cls.realtime - l->start > DM_LIMB_MS || !l->g2 ) {
			DM_FreeLimb( l );
			continue;
		}
		if ( !l->resting ) {
			const vec3_t mins = { -4, -4, -2 }, maxs = { 4, 4, 2 };
			vec3_t to;
			trace_t tr;

			l->vel[2] -= DM_GRAVITY * dt;
			VectorMA( l->org, dt, l->vel, to );
			CM_BoxTrace( &tr, l->org, to, mins, maxs, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
			VectorCopy( tr.endpos, l->org );
			VectorMA( l->ang, dt, l->spin, l->ang );
			if ( tr.fraction < 1.0f ) {
				// a bounce, smaller each time, till it lies there, flat
				const float into = DotProduct( l->vel, tr.plane.normal );

				VectorMA( l->vel, -1.6f * into, tr.plane.normal, l->vel );
				VectorScale( l->vel, 0.45f, l->vel );
				VectorScale( l->spin, 0.5f, l->spin );
				if ( tr.plane.normal[2] > 0.7f && VectorLength( l->vel ) < 40.0f ) {
					l->resting = qtrue;
					l->ang[PITCH] = l->part == DM_HEAD || l->part == DM_TORSO ? l->ang[PITCH] * 0.2f : 90.0f;
					l->ang[ROLL] = 0;
				}
			}
		}
		memset( &ent, 0, sizeof( ent ) );
		ent.reType = RT_MODEL;
		ent.ghoul2 = l->g2;
		VectorCopy( l->org, ent.origin );
		VectorCopy( l->org, ent.oldorigin );
		VectorCopy( l->org, ent.lightingOrigin );
		AnglesToAxis( l->ang, ent.axis );
		VectorSet( ent.modelScale, l->scale, l->scale, l->scale );
		if ( l->scale != 1.0f ) {
			for ( int k = 0; k < 3; k++ )
				VectorScale( ent.axis[k], l->scale, ent.axis[k] );
			ent.nonNormalizedAxes = qtrue;
		}
		ent.radius = 48.0f;
		ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = ent.shaderRGBA[3] = 255;
		re->AddRefEntityToScene( &ent );
	}
}

// cmd:dismember/<num>/<part>/<fall 0|1>/<animation or ->, cmd:dismember/<num>/restore
void CL_Dismember_Hook( char **argv, int argc ) {
	const qboolean replay = CL_Presence_Replaying();
	dmBody_t *b;
	int num, part;

	if ( argc < 2 || cls.state != CA_ACTIVE )
		return;
	num = atoi( argv[0] );
	if ( num < 0 || num >= MAX_GENTITIES )
		return;
	if ( !Q_stricmp( argv[1], "restore" ) ) {
		DM_FreeLimbsOf( num );
		CL_RpCinematic_ReleaseAnim( num );
		if ( ( b = DM_Body( num, qfalse ) ) != NULL ) {
			b->restore = b->cut;
			b->cut = b->fall = 0;
			b->restoreUntil = cls.realtime + 10000;
		}
		return;
	}
	if ( ( part = DM_PartOf( argv[1] ) ) < 0 || !( b = DM_Body( num, qtrue ) ) )
		return;
	b->cut |= 1 << part;
	b->restore &= ~( 1 << part );
	// my own weapon hand cut off: I drop what I hold, as RPMod has it (not again for a replay). Not a
	// saber: RPMod's drop leaves the server holding it as thrown, its hilt bouncing at every strike
	if ( num == clc.clientNum && ( part == DM_RARM || part == DM_RHAND ) && !replay && cl.snap.ps.weapon != WP_SABER )
		CL_AddReliableCommand( "rpdrop weapon", qfalse );
	// one who joins later sees it cut, not falling again
	if ( argc >= 3 && atoi( argv[2] ) && !replay )
		b->fall |= 1 << part;
	if ( argc >= 4 && argv[3][0] && strcmp( argv[3], "-" ) )
		CL_RpCinematic_HoldAnim( num, argv[3], replay );
}

// /dismember <who> <part> [fall|stay] [die|<animation>], /dismember <who> restore
static void DM_Cmd_f( void ) {
	int nums[MAX_CLIENTS], count, part = -1;
	const char *what = Cmd_Argv( 2 );
	qboolean fall = qtrue;
	const char *anim = "-";

	if ( Cmd_Argc() < 3 ) {
		Com_Printf( "usage: dismember <who> <part> [fall|stay] [die|<animation>]   cut a body, for everyone with this client\n"
			"       dismember <who> restore                                 put it back whole\n"
			"who: a player's number or name, me, an NPC's targetname (commas between several)\n"
			"part: head, torso, larm, rarm, rhand, lleg, rleg\n"
			"fall (the default): the part drops and lies there; stay: it's only gone\n"
			"die: they fall with a death animation (BOTH_DEATH1) and stay down; or name the animation\n"
			"For RPMod admins, through C1B3R; restore, or the map loading again, puts them back.\n" );
		return;
	}
	if ( Q_stricmp( what, "restore" ) && ( part = DM_PartOf( what ) ) < 0 ) {
		Com_Printf( "dismember: no part %s (head, torso, larm, rarm, rhand, lleg, rleg)\n", what );
		return;
	}
	for ( int i = 3; i < Cmd_Argc(); i++ ) {
		const char *arg = Cmd_Argv( i );

		if ( !Q_stricmp( arg, "fall" ) )
			fall = qtrue;
		else if ( !Q_stricmp( arg, "stay" ) || !Q_stricmp( arg, "nofall" ) )
			fall = qfalse;
		else if ( !Q_stricmp( arg, "die" ) )
			anim = "BOTH_DEATH1";
		else
			anim = arg;
	}
	count = RC_AnimTargets( Cmd_Argv( 1 ), nums, ARRAY_LEN( nums ) );
	if ( !count ) {
		Com_Printf( "dismember: nobody called %s here\n", Cmd_Argv( 1 ) );
		return;
	}
	for ( int i = 0; i < count; i++ ) {
		if ( part < 0 )
			CL_Presence_Hook( -1, va( "cmd:dismember/%i/restore", nums[i] ) );
		else
			CL_Presence_Hook( -1, va( "cmd:dismember/%i/%s/%i/%s", nums[i], dmParts[part].word, fall ? 1 : 0, anim ) );
	}
}

// a new map, or another load of it: every body whole again, the limbs gone
void CL_Dismember_Clear( void ) {
	for ( int i = 0; i < DM_MAX_LIMBS; i++ ) {
		if ( dmLimbs[i].active )
			DM_FreeLimb( &dmLimbs[i] );
	}
	for ( int i = 0; i < DM_MAX_CUT; i++ ) {
		dmBodies[i].cut = dmBodies[i].fall = dmBodies[i].restore = 0;
		dmBodies[i].num = -1;
	}
}

// the renderer started again: the limbs' models are gone with it, dropped (not freed)
void CL_Dismember_RendererRestarted( void ) {
	memset( dmLimbs, 0, sizeof( dmLimbs ) );
}

void CL_Dismember_Init( void ) {
	memset( dmLimbs, 0, sizeof( dmLimbs ) );
	CL_Dismember_Clear();
	Cmd_AddCommand( "dismember", DM_Cmd_f, "Cut a player's or an NPC's body for the story, for everyone with this client (RPMod admins)" );
}

void CL_Dismember_Shutdown( void ) {
	Cmd_RemoveCommand( "dismember" );
	CL_Dismember_Clear();
}
