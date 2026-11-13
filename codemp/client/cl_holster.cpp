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

// cl_holster.cpp -- /holster: models worn on your character (a hilt at the hip, a blaster on the
// back), seen by everyone with this client. Kept by C1B3R for your RPMod account (its
// /v2/char/holster, see OpenJK-rpmod-c1b3r's characters.py, which keeps them near the body and not
// too big); everyone else's come with their character (/v2/char/view), asked again when C1B3R says
// it changed (cmd:char/<client>/<version>).
//
// Each attachment is on a tag of the body (*hip_r, *back...), moved, turned and sized from it. It's
// drawn on each player's body as the game draws it: each frame every player is paired with the body
// nearest them of those drawn the frame before, one body each (as custom emotes are), so it follows
// their animation and their custom emote. A model someone doesn't have isn't drawn for them. One
// can hide while its player's saber is lit, or while it's in their hand (a hilt worn at the hip), or
// show only while they hold a datapad up (RPMod's /emlift, BOTH_CIN_26 or 27): one in their hand.

#include <string>
#include <vector>
#include "client.h"
#include "qcommon/cm_public.h"
#include "cl_presence.h"
#include "cl_rpcinematic.h"
#include "cl_widgets.h"
#include "cl_gizmo.h"

#define HS_CMD				"holster"
#define HS_MAX				8			// as C1B3R keeps
#define HS_OFFSET			16.0f
#define HS_SCALE_MIN		0.25f
#define HS_SCALE_MAX		2.0f
#define HS_MAX_MODELS		64
#define HS_MAX_BODIES		64
#define HS_NEAR				64.0f		// a body this near a player is theirs
#define HS_RETRY_MS			15000
#define HS_MAX_TAGS			64
#define HS_MESSAGE_MS		5000

typedef struct {
	char	bolt[32];
	char	model[MAX_QPATH];
	char	source[64];				// the pk3 it's from, "base" for the game's own
	vec3_t	off, ang;
	float	scale;
	int		hide;					// 0 never, 1 while the saber's lit, 2 while it's in hand, 3 shown only in the datapad emote
	// worn with every preset of the character, or only with these (their ids, cl_presets.cpp);
	// neither: not worn. Others' come from C1B3R already picked (all set)
	qboolean	all;
	char		presets[32][9];
	int			numPresets;
} hsItem_t;

#define HS_ITEM_PRESETS		32

#define HS_HIDE_MODES		4
#define HS_HIDE_DATAPAD		3

// datapad models to hold in the hand or lay in the world: the first this game has is used
static const char *hsDatapads[] = {
	"models/items/datapad.md3",
	"items/datapad.md3",
};

// how a body's player is shown: in the datapad emote or not, or everything (the editor's)
enum { HS_EMOTE_NO, HS_EMOTE_DATAPAD, HS_EMOTE_ALL };

// what's known of each player's
typedef struct {
	int			state;				// 0 to ask, 1 asked, 2 known
	int			retryAt;
	char		slotName[MAX_INFO_VALUE];	// whom it's for (all of it: a coloured name is long), another in the slot asked again
	qboolean	linked;				// as C1B3R's list said when it was asked: linked since, asked again
	hsItem_t	items[HS_MAX];
	int			num;
	qboolean	hand;				// a datapad in hand during the datapad emote (their datapad setting)
} hsClient_t;

// a model worn, loaded once for everyone wearing it
typedef struct {
	char			path[MAX_QPATH];
	qhandle_t		model;
	CGhoul2Info_v	*g2;
	qboolean		ok;
} hsModel_t;

static hsClient_t	hsClients[MAX_CLIENTS];
static hsModel_t	hsModels[HS_MAX_MODELS];
static int			hsNumModels;

// the bodies drawn last frame, and whose each is this frame
static struct {
	void	*ghoul2;
	vec3_t	origin;
} hsBodies[HS_MAX_BODIES];
static int		hsNumBodies;
static void		*hsPaired[MAX_CLIENTS];
static int		hsDrawnFrame[MAX_CLIENTS], hsFrame;
static int		hsDebug;		// /holster debug: frames left to report what's drawn and why not

// live editing: the tag the picked one is on, in the world, as it was last drawn (the gizmo's)
static struct {
	int		item;				// the one to keep, while it's drawn; -1 none
	int		of;					// the one it was kept for
	int		frame;				// cls.framecount it was
	vec3_t	org;				// the tag, before the move
	vec3_t	axis[3];			// its axes, the move's and the turn's
} hsKeep = { -1, -1 };

static httpRequest_t hsViewHttp, hsGetHttp, hsSetHttp;
static int hsViewFor = -1;
static cvar_t *cl_holster;

/*
===============================================================================

The models, and putting them on a body

===============================================================================
*/

static hsModel_t *HS_Model( const char *path ) {
	hsModel_t *m;

	for ( int i = 0; i < hsNumModels; i++ ) {
		if ( !Q_stricmp( hsModels[i].path, path ) )
			return hsModels[i].ok ? &hsModels[i] : NULL;
	}
	if ( hsNumModels >= HS_MAX_MODELS || !path[0] )
		return NULL;
	m = &hsModels[hsNumModels++];
	memset( m, 0, sizeof( *m ) );
	Q_strncpyz( m->path, path, sizeof( m->path ) );
	// one this game hasn't: nothing drawn
	if ( FS_ReadFile( path, NULL ) <= 0 )
		return NULL;
	if ( !Q_stricmp( COM_GetExtension( path ), "glm" ) ) {
		if ( re->G2API_InitGhoul2Model( &m->g2, path, 0, 0, 0, 0, 0 ) < 0 || !m->g2 ) {
			m->g2 = NULL;
			return NULL;
		}
	} else if ( !( m->model = re->RegisterModel( path ) ) ) {
		return NULL;
	}
	m->ok = qtrue;
	return m;
}

static void HS_FreeModels( void ) {
	for ( int i = 0; i < hsNumModels; i++ ) {
		if ( hsModels[i].g2 )
			re->G2API_CleanGhoul2Models( &hsModels[i].g2 );
	}
	memset( hsModels, 0, sizeof( hsModels ) );
	hsNumModels = 0;
}

static const char *HS_Vec( const vec3_t v ) {
	return va( "(%.0f %.0f %.0f)", v[0], v[1], v[2] );
}

// a vector in the body's own axes, in the world's
static void HS_ToWorld( const refEntity_t *body, const vec3_t in, vec3_t out ) {
	for ( int k = 0; k < 3; k++ )
		out[k] = body->axis[0][k] * in[0] + body->axis[1][k] * in[1] + body->axis[2][k] * in[2];
}

// each attachment put on the body as it's drawn now; saber: 0 put away, 1 in hand, 2 lit; emote: HS_EMOTE_*
static void HS_AddItems( const hsItem_t *items, int num, void *ghoul2, const refEntity_t *body, int saber, int emote ) {
	vec3_t zero = { 0, 0, 0 };

	if ( !ghoul2 )
		return;
	CGhoul2Info_v &g2 = *(CGhoul2Info_v *)ghoul2;
	if ( !re->G2API_HaveWeGhoul2Models( g2 ) )
		return;
	for ( int i = 0; i < num; i++ ) {
		const hsItem_t *it = &items[i];
		const hsModel_t *m;
		mdxaBone_t bm;
		vec3_t at, axes[3], ang[3], local, v;
		refEntity_t ent;
		int bolt;

		if ( emote != HS_EMOTE_ALL && ( ( it->hide == 1 && saber >= 2 ) || ( it->hide == 2 && saber >= 1 ) ) ) {
			if ( hsDebug )
				Com_Printf( "  %s: hidden (saber %s)\n", it->model, saber >= 2 ? "lit" : "in hand" );
			continue;
		}
		if ( it->hide == HS_HIDE_DATAPAD && emote == HS_EMOTE_NO ) {
			if ( hsDebug )
				Com_Printf( "  %s: hidden (not in the datapad emote)\n", it->model );
			continue;
		}
		if ( !( m = HS_Model( it->model ) ) ) {
			if ( hsDebug )
				Com_Printf( "  %s: model not loaded\n", it->model );
			continue;
		}
		bolt = re->G2API_AddBolt( g2, 0, it->bolt );
		if ( bolt < 0 || !re->G2API_GetBoltMatrix( g2, 0, bolt, &bm, zero, zero, cl.serverTime, NULL, (float *)body->modelScale ) ) {
			if ( hsDebug )
				Com_Printf( "  %s: tag %s not found on the body (%i)\n", it->model, it->bolt, bolt );
			continue;
		}
		// the tag's place and axes, in the body's own
		VectorSet( at, bm.matrix[0][3], bm.matrix[1][3], bm.matrix[2][3] );
		for ( int k = 0; k < 3; k++ ) {
			VectorSet( axes[k], bm.matrix[0][k], bm.matrix[1][k], bm.matrix[2][k] );
			VectorNormalize( axes[k] );
		}
		if ( i == hsKeep.item ) {
			HS_ToWorld( body, at, v );
			VectorAdd( body->origin, v, hsKeep.org );
			for ( int k = 0; k < 3; k++ ) {
				HS_ToWorld( body, axes[k], hsKeep.axis[k] );
				VectorNormalize( hsKeep.axis[k] );
			}
			hsKeep.of = i;
			hsKeep.frame = cls.framecount;
		}
		// moved and turned from the tag
		for ( int k = 0; k < 3; k++ )
			VectorMA( at, it->off[k], axes[k], at );
		AnglesToAxis( it->ang, ang );

		memset( &ent, 0, sizeof( ent ) );
		ent.reType = RT_MODEL;
		HS_ToWorld( body, at, v );
		VectorAdd( body->origin, v, ent.origin );
		VectorCopy( ent.origin, ent.oldorigin );
		for ( int a = 0; a < 3; a++ ) {
			for ( int k = 0; k < 3; k++ )
				local[k] = ang[a][0] * axes[0][k] + ang[a][1] * axes[1][k] + ang[a][2] * axes[2][k];
			HS_ToWorld( body, local, ent.axis[a] );
		}
		if ( m->g2 ) {
			ent.ghoul2 = m->g2;
			VectorSet( ent.modelScale, it->scale, it->scale, it->scale );
		} else {
			ent.hModel = m->model;
			if ( it->scale != 1.0f ) {
				for ( int a = 0; a < 3; a++ )
					VectorScale( ent.axis[a], it->scale, ent.axis[a] );
				ent.nonNormalizedAxes = qtrue;
			}
		}
		// lit as the body is, and only where it's drawn (a mirror, for your own in first person)
		ent.renderfx = ( body->renderfx & ( RF_THIRD_PERSON | RF_FIRST_PERSON | RF_NOSHADOW ) ) | RF_LIGHTING_ORIGIN;
		VectorCopy( body->origin, ent.lightingOrigin );
		ent.radius = 32.0f * it->scale;
		if ( hsDebug )
			Com_Printf( "  %s: drawn at %s on %s (renderfx %i)\n", it->model, HS_Vec( ent.origin ), it->bolt, ent.renderfx );
		re->AddRefEntityToScene( &ent );
	}
}

/*
===============================================================================

Everyone's, from C1B3R

===============================================================================
*/

static int HS_Split( char *line, char **field, int max ) {
	int n = 0;

	for ( char *f = line; f && n < max; n++ ) {
		field[n] = f;
		f = strchr( f, '\t' );
		if ( f )
			*f++ = '\0';
	}
	return n;
}

// holster <bolt> <model> <source> <x y z> <pitch yaw roll> <scale> <hide>, among an answer's lines
static int HS_ReadItems( const char *answer, hsItem_t *items, int max ) {
	std::string all( answer );
	size_t at = 0;
	int num = 0;

	while ( at < all.size() && num < max ) {
		size_t end = all.find( '\n', at );
		char line[1024], *f[10];
		int n;

		if ( end == std::string::npos )
			end = all.size();
		Q_strncpyz( line, all.substr( at, end - at ).c_str(), sizeof( line ) );
		at = end + 1;
		if ( strncmp( line, "holster\t", 8 ) || ( n = HS_Split( line, f, 10 ) ) < 8 )
			continue;
		hsItem_t *it = &items[num++];

		memset( it, 0, sizeof( *it ) );
		Q_strncpyz( it->bolt, CL_Presence_Unfield( f[1] ).c_str(), sizeof( it->bolt ) );
		Q_strncpyz( it->model, CL_Presence_Unfield( f[2] ).c_str(), sizeof( it->model ) );
		Q_strncpyz( it->source, CL_Presence_Unfield( f[3] ).c_str(), sizeof( it->source ) );
		sscanf( f[4], "%f %f %f", &it->off[0], &it->off[1], &it->off[2] );
		sscanf( f[5], "%f %f %f", &it->ang[0], &it->ang[1], &it->ang[2] );
		it->scale = Com_Clamp( HS_SCALE_MIN, HS_SCALE_MAX, (float)atof( f[6] ) );
		it->hide = Com_Clampi( 0, HS_HIDE_MODES - 1, atoi( f[7] ) );
		// yours, for the editor and your own body: what it's worn with; others': picked already
		it->all = qtrue;
		if ( n >= 10 ) {
			char ids[512];

			it->all = (qboolean)( atoi( f[8] ) != 0 );
			Q_strncpyz( ids, f[9], sizeof( ids ) );
			for ( char *id = strtok( ids, "," ); id && it->numPresets < HS_ITEM_PRESETS; id = strtok( NULL, "," ) ) {
				if ( strlen( id ) == 8 )
					Q_strncpyz( it->presets[it->numPresets++], id, sizeof( it->presets[0] ) );
			}
		}
	}
	return num;
}

// whether an item is worn with the preset the look is now (every preset's are, always)
static qboolean HS_WornWith( const hsItem_t *it, const char *worn ) {
	if ( it->all )
		return qtrue;
	for ( int i = 0; worn[0] && i < it->numPresets; i++ ) {
		if ( !strcmp( it->presets[i], worn ) )
			return qtrue;
	}
	return qfalse;
}

// your own items worn now, picked here at once as the look changes (others' come picked by C1B3R)
static int HS_OwnWorn( const hsItem_t *in, int num, hsItem_t *out ) {
	const char *worn = CL_Presets_WornId( CL_Characters_ActiveId() );
	int n = 0;

	for ( int i = 0; i < num; i++ ) {
		if ( HS_WornWith( &in[i], worn ) )
			out[n++] = in[i];
	}
	return n;
}

static qboolean HS_Post( httpRequest_t *req, const char *path, const std::string &fields ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session || req->state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += fields + "}";
	return CL_Presence_Post( req, path, body );
}

static hsClient_t *HS_Client( int client ) {
	hsClient_t *c = &hsClients[client];

	// someone else in the slot now
	if ( Q_stricmp( c->slotName, CL_PlayerSlotName( client ) ) ) {
		memset( c, 0, sizeof( *c ) );
		Q_strncpyz( c->slotName, CL_PlayerSlotName( client ), sizeof( c->slotName ) );
	}
	return c;
}

// the next player whose attachments aren't known asked, one at a time
static void HS_FetchFrame( void ) {
	static char session[64];
	const char *now = CL_Presence_Session();

	// a new session (linked since, or C1B3R started again): everyone's asked again
	if ( Q_stricmp( session, now ? now : "" ) ) {
		Q_strncpyz( session, now ? now : "", sizeof( session ) );
		for ( int i = 0; i < MAX_CLIENTS; i++ )
			hsClients[i].state = hsClients[i].retryAt = 0;
	}
	if ( HTTP_Poll( &hsViewHttp ) && hsViewFor >= 0 ) {
		hsClient_t *c = &hsClients[hsViewFor];

		if ( hsViewHttp.status == 200 ) {
			c->num = HS_ReadItems( hsViewHttp.answer, c->items, HS_MAX );
			c->hand = (qboolean)( strstr( va( "\n%s\n", hsViewHttp.answer ), "\ndatapadhand\t1\n" ) != NULL );
			c->state = 2;
		} else {
			c->state = 0;
			c->retryAt = cls.realtime + HS_RETRY_MS;
		}
		hsViewFor = -1;
	}
	if ( hsViewHttp.state == HTTP_BUSY || !CL_Presence_Session() )
		return;
	for ( int i = 0; i < MAX_CLIENTS; i++ ) {
		hsClient_t *c;

		if ( !CL_PlayerSlotName( i )[0] || ( i != clc.clientNum && !CL_Presence_Linked( i ) ) )
			continue;
		c = HS_Client( i );
		// they've linked since theirs was asked: asked again
		if ( c->state == 2 && c->linked != CL_Presence_Linked( i ) )
			c->state = 0;
		if ( c->state || cls.realtime < c->retryAt )
			continue;
		// yours: every item and what it's worn with, picked here as the look changes
		if ( i == clc.clientNum ? HS_Post( &hsViewHttp, "/v2/char/holster/get", "" )
			: HS_Post( &hsViewHttp, "/v2/char/view", va( ",\"client\":%i", i ) ) ) {
			c->state = 1;
			c->linked = CL_Presence_Linked( i );
			hsViewFor = i;
		}
		return;
	}
}

// cmd:char/<client>/<version>: theirs asked again
void CL_Holster_CharHook( char **argv, int argc ) {
	const int client = argc >= 1 && argv[0][0] ? atoi( argv[0] ) : -1;

	if ( client >= 0 && client < MAX_CLIENTS ) {
		hsClients[client].state = 0;
		hsClients[client].retryAt = 0;
	}
}

/*
===============================================================================

On everyone's body, as the game draws it

===============================================================================
*/

static qboolean HE_Live( void );
static void HE_AddLive( void *ghoul2, const refEntity_t *body, int saber );

// where a player is now, and how their saber is: 0 put away, 1 in hand, 2 lit. The player state is
// whoever the view is of (you, or the one you follow); someone not drawn (you while following, a
// spectator, nodraw) has nowhere to wear anything
static qboolean HS_PlayerState( int client, vec3_t origin, int *saber ) {
	if ( !CL_EntityDrawn( client ) )
		return qfalse;
	if ( client == cl.snap.ps.clientNum ) {
		const playerState_t *ps = &cl.snap.ps;

		VectorCopy( ps->origin, origin );
		*saber = ps->weapon != WP_SABER ? 0 : ps->saberHolstered >= 2 || ps->saberInFlight ? 1 : 2;
		return qtrue;
	}
	for ( int i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];

		if ( es->number == client && es->eType == ET_PLAYER ) {
			VectorCopy( es->pos.trBase, origin );
			*saber = es->weapon != WP_SABER ? 0 : es->saberHolstered >= 2 || es->saberInFlight ? 1 : 2;
			return qtrue;
		}
	}
	return qfalse;
}

// a player holding a datapad up (RPMod's /emlift, emlift2): their torso's animation
static int HS_Emote( int client ) {
	int torso = -1;
	const char *name;

	if ( client == cl.snap.ps.clientNum ) {
		torso = cl.snap.ps.torsoAnim;
	} else {
		for ( int i = 0; i < cl.snap.numEntities; i++ ) {
			const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];

			if ( es->number == client && es->eType == ET_PLAYER ) {
				torso = es->torsoAnim;
				break;
			}
		}
	}
	name = CL_EmotePreview_AnimName( torso );
	return name && ( !Q_stricmp( name, "BOTH_CIN_26" ) || !Q_stricmp( name, "BOTH_CIN_27" ) ) ? HS_EMOTE_DATAPAD : HS_EMOTE_NO;
}

// a datapad in a player's hand during the datapad emote: the datapad setting's (cl_datapadHand), not
// the holster's, so it shows whatever they wear and with cl_holster 0. Where it sits on the hand is
// cl_datapadHandPos, "<tag> <x y z> <pitch yaw roll> <scale>", the same for everyone this game draws
static qboolean HS_Hand( int client ) {
	if ( client == clc.clientNum )
		return (qboolean)( Cvar_VariableIntegerValue( "cl_datapadHand" ) != 0 );
	return (qboolean)( hsClients[client].state == 2 && hsClients[client].hand );
}

static qboolean HS_HandItem( hsItem_t *it ) {
	static cvar_t *pos;
	const char *model = CL_Holster_DatapadModel();
	char tag[32];

	if ( !model[0] )
		return qfalse;
	if ( !pos )
		pos = Cvar_Get( "cl_datapadHandPos", "*r_hand 0 0 0 0 0 0 1", CVAR_ARCHIVE_ND,
			"Where the datapad in hand sits: <tag> <x y z> <pitch yaw roll> <scale>, from the tag" );
	memset( it, 0, sizeof( *it ) );
	it->scale = 1.0f;
	tag[0] = '\0';
	sscanf( pos->string, "%31s %f %f %f %f %f %f %f", tag, &it->off[0], &it->off[1], &it->off[2],
		&it->ang[0], &it->ang[1], &it->ang[2], &it->scale );
	Q_strncpyz( it->bolt, tag[0] == '*' ? tag : "*r_hand", sizeof( it->bolt ) );
	Q_strncpyz( it->model, model, sizeof( it->model ) );
	it->scale = Com_Clamp( HS_SCALE_MIN, HS_SCALE_MAX, it->scale );
	return qtrue;
}

// each frame, before it's drawn: every player wearing something paired with one of last frame's
// bodies, the nearest pairs first
void CL_Holster_Frame( void ) {
	qboolean bodyTaken[HS_MAX_BODIES] = { qfalse }, done[MAX_CLIENTS] = { qfalse };
	int owner[HS_MAX_BODIES];
	vec3_t org[MAX_CLIENTS];
	qboolean known[MAX_CLIENTS];
	int saber;

	if ( !cl_holster )
		return;
	HS_FetchFrame();
	hsFrame++;
	if ( hsDebug ) {
		hsDebug--;
		Com_Printf( "holster debug: cl_holster %i, session %s, you are %i, %i bodies last frame\n", cl_holster->integer,
			CL_Presence_Session() ? "yes" : "none", clc.clientNum, hsNumBodies );
		for ( int b = 0; b < hsNumBodies; b++ )
			Com_Printf( "  body %i at %s\n", b, HS_Vec( hsBodies[b].origin ) );
	}
	for ( int c = 0; c < MAX_CLIENTS; c++ ) {
		hsPaired[c] = NULL;
		known[c] = (qboolean)( cls.state == CA_ACTIVE
			&& ( ( cl_holster->integer && ( ( hsClients[c].num && hsClients[c].state == 2 ) || ( c == clc.clientNum && HE_Live() ) ) )
				|| HS_Hand( c ) )
			&& HS_PlayerState( c, org[c], &saber ) );
		if ( hsDebug && CL_PlayerSlotName( c )[0] && ( c == clc.clientNum || hsClients[c].state ) )
			Com_Printf( "  %2i %s" S_COLOR_WHITE ": state %i, %i worn, linked %i, %s\n", c, CL_PlayerSlotName( c ),
				hsClients[c].state, hsClients[c].num, (int)CL_Presence_Linked( c ),
				HS_PlayerState( c, org[c], &saber ) ? HS_Vec( org[c] ) : "not in the snapshot" );
	}
	for ( int b = 0; b < hsNumBodies; b++ )
		owner[b] = CL_BodyOwner( hsBodies[b].origin, HS_NEAR );
	for ( ;; ) {
		float best = HS_NEAR;
		int bc = -1, bb = -1;

		for ( int c = 0; c < MAX_CLIENTS; c++ ) {
			if ( !known[c] || done[c] )
				continue;
			for ( int b = 0; b < hsNumBodies; b++ ) {
				const float d = Distance( org[c], hsBodies[b].origin );

				// only their own body: one nearer someone else (an NPC, another player) is that one's
				if ( !bodyTaken[b] && d < best && owner[b] == c ) {
					best = d;
					bc = c;
					bb = b;
				}
			}
		}
		if ( bc < 0 )
			break;
		done[bc] = qtrue;
		bodyTaken[bb] = qtrue;
		hsPaired[bc] = hsBodies[bb].ghoul2;
		if ( hsDebug )
			Com_Printf( "  %i paired with body %i (%.0f away)\n", bc, bb, best );
	}
	hsNumBodies = 0;
}

// a model on its way to the renderer, drawn already: a body (as the cgame made it, before an emote's
// copy took its place) is noted for next frame's pairing, and whoever it's paired with wears theirs
// on it, once a frame
void CL_Holster_Entity( const refEntity_t *ent, void *bodyGhoul2 ) {
	const char *gla;

	if ( !cl_holster || !ent || ent->reType != RT_MODEL || !ent->ghoul2 || !bodyGhoul2 )
		return;
	CGhoul2Info_v &g2 = *(CGhoul2Info_v *)ent->ghoul2;
	if ( !re->G2API_HaveWeGhoul2Models( g2 ) || !( gla = re->G2API_GetGLAName( g2, 0 ) ) || !Q_stristr( gla, "humanoid" ) )
		return;
	{
		qboolean seen = qfalse;

		for ( int b = 0; b < hsNumBodies && !seen; b++ )
			seen = (qboolean)( hsBodies[b].ghoul2 == bodyGhoul2 );
		if ( !seen && hsNumBodies < HS_MAX_BODIES ) {
			hsBodies[hsNumBodies].ghoul2 = bodyGhoul2;
			VectorCopy( ent->origin, hsBodies[hsNumBodies].origin );
			hsNumBodies++;
		}
	}
	// a shell drawn over a body is the same body again
	if ( ent->customShader )
		return;
	for ( int c = 0; c < MAX_CLIENTS; c++ ) {
		vec3_t org;
		int saber;

		if ( hsPaired[c] != bodyGhoul2 || hsDrawnFrame[c] == hsFrame || !HS_PlayerState( c, org, &saber ) )
			continue;
		hsDrawnFrame[c] = hsFrame;
		if ( hsDebug )
			Com_Printf( "  drawing %i's on body (renderfx %i, gla %s)\n", c, ent->renderfx, gla );
		if ( cl_holster->integer ) {
			if ( c == clc.clientNum && HE_Live() ) {
				HE_AddLive( ent->ghoul2, ent, saber );
			} else if ( c == clc.clientNum ) {
				hsItem_t worn[HS_MAX];

				HS_AddItems( worn, HS_OwnWorn( hsClients[c].items, hsClients[c].num, worn ), ent->ghoul2, ent, saber, HS_Emote( c ) );
			} else {
				HS_AddItems( hsClients[c].items, hsClients[c].num, ent->ghoul2, ent, saber, HS_Emote( c ) );
			}
		}
		if ( HS_Hand( c ) && HS_Emote( c ) == HS_EMOTE_DATAPAD ) {
			const int keep = hsKeep.item;	// not the live editor's picked one
			hsItem_t hand;

			if ( HS_HandItem( &hand ) ) {
				hsKeep.item = -1;
				HS_AddItems( &hand, 1, ent->ghoul2, ent, saber, HS_EMOTE_ALL );
				hsKeep.item = keep;
			}
		}
	}
}

// a player's on a body drawn elsewhere (Inspect's): ctx is their client number
void CL_Holster_DrawOn( void *ctx, void *ghoul2, const refEntity_t *body ) {
	const int client = (int)(intptr_t)ctx;

	if ( client >= 0 && client < MAX_CLIENTS && hsClients[client].state == 2 ) {
		hsItem_t worn[HS_MAX];

		if ( client == clc.clientNum )
			HS_AddItems( worn, HS_OwnWorn( hsClients[client].items, hsClients[client].num, worn ), ghoul2, body, 0, HS_EMOTE_NO );
		else
			HS_AddItems( hsClients[client].items, hsClients[client].num, ghoul2, body, 0, HS_EMOTE_NO );
	}
}

// what a player wears that this game hasn't: "<model> (<pk3>), ..." for Inspect, "" none
const char *CL_Holster_Missing( int client ) {
	static char out[512];

	out[0] = '\0';
	if ( client < 0 || client >= MAX_CLIENTS || hsClients[client].state != 2 )
		return out;
	for ( int i = 0; i < hsClients[client].num; i++ ) {
		const hsItem_t *it = &hsClients[client].items[i];

		if ( HS_Model( it->model ) )
			continue;
		Q_strcat( out, sizeof( out ), va( "%s%s (%s)", out[0] ? ", " : "", COM_SkipPath( (char *)it->model ),
			it->source[0] ? it->source : "a custom pk3" ) );
	}
	return out;
}

// a datapad model this game has, to hold or to place in the world: "" none
const char *CL_Holster_DatapadModel( void ) {
	for ( int i = 0; i < (int)ARRAY_LEN( hsDatapads ); i++ ) {
		if ( HS_Model( hsDatapads[i] ) )
			return hsDatapads[i];
	}
	return "";
}

// a model of the cache set on ent (a ghoul2 one or not); qfalse when this game hasn't it
qboolean CL_Holster_ModelEntity( const char *path, refEntity_t *ent ) {
	const hsModel_t *m = HS_Model( path );

	if ( !m )
		return qfalse;
	if ( m->g2 ) {
		ent->ghoul2 = m->g2;
		VectorSet( ent->modelScale, 1, 1, 1 );
	} else {
		ent->hModel = m->model;
	}
	return qtrue;
}

/*
===============================================================================

The editor: the models there are

===============================================================================
*/

typedef struct {
	std::string	path;
	std::string	source;			// the pk3's name, "base" for the game's own
	bool		custom;
} hsFile_t;

static std::vector<hsFile_t>	hsFiles;
static std::vector<int>			hsShown;		// hsFiles, filtered

// "base/assets<digits>.pk3": the game's own
static qboolean HS_IsBase( const char *source ) {
	const char *c;

	if ( Q_stricmpn( source, BASEGAME "/assets", strlen( BASEGAME "/assets" ) ) )
		return qfalse;
	for ( c = source + strlen( BASEGAME "/assets" ); isdigit( (unsigned char)*c ); c++ )
		;
	return (qboolean)!Q_stricmp( c, ".pk3" );
}

static void HS_AddFile( const char *name, const char *source, void *ctx ) {
	const char *base = COM_SkipPath( (char *)name );
	const int len = (int)strlen( base );
	hsFile_t f;

	// bodies aren't worn; a model's lower details (name_1, name_2) aren't listed apart
	if ( !Q_stricmpn( name, "models/players/", 15 ) || ( len > 6 && base[len - 6] == '_' && isdigit( (unsigned char)base[len - 5] ) ) )
		return;
	for ( const hsFile_t &other : hsFiles ) {
		if ( !Q_stricmp( other.path.c_str(), name ) )
			return;
	}
	f.path = name;
	f.custom = !HS_IsBase( source ? source : "" );
	f.source = f.custom ? ( source && strrchr( source, '/' ) ? strrchr( source, '/' ) + 1 : source ? source : "" ) : "base";
	hsFiles.push_back( f );
}

static void HS_BuildFiles( void ) {
	if ( !hsFiles.empty() )
		return;
	FS_ListFilesRecursiveFrom( "models", ".md3", HS_AddFile, NULL );
	FS_ListFilesRecursiveFrom( "models", ".glm", HS_AddFile, NULL );
}

static const char *HS_SourceOf( const char *path ) {
	for ( const hsFile_t &f : hsFiles ) {
		if ( !Q_stricmp( f.path.c_str(), path ) )
			return f.source.c_str();
	}
	return "";
}

/*
===============================================================================

The editor

===============================================================================
*/

#define HE_X			20.0f
#define HE_Y			24.0f
#define HE_W			600.0f
#define HE_H			432.0f
#define HE_TOP			( HE_Y + 40 )
#define HE_BOTTOM		( HE_Y + HE_H - 30 )
#define HE_A_X			( HE_X + 12 )
#define HE_A_W			160.0f
#define HE_B_X			( HE_A_X + HE_A_W + 12 )
#define HE_B_W			190.0f
#define HE_C_X			( HE_B_X + HE_B_W + 12 )
#define HE_C_W			( HE_X + HE_W - 12 - HE_C_X )
#define HE_PREVIEW_H	190.0f
#define HE_ROW_H		22.0f
#define HE_FILE_H		15.0f
#define HE_CTRL_H		16.0f
#define HE_TEXT			0.8f
#define HE_SMALL		0.65f
#define HE_ITEM			0.6f

static const float hePanel[4]		= { 0.0f, 0.0f, 0.08f, 0.88f };
static const float heBorder[4]		= { 0.5f, 0.5f, 0.7f, 1.0f };
static const float heItem[4]		= { 0.15f, 0.15f, 0.25f, 0.8f };
static const float heItemHover[4]	= { 0.25f, 0.25f, 0.4f, 0.9f };
static const float heItemOn[4]		= { 0.35f, 0.24f, 0.05f, 0.9f };
static const float heField[4]		= { 0.05f, 0.05f, 0.12f, 0.95f };
static const float heOrange[4]		= { 1.0f, 0.682f, 0.0f, 1.0f };
static const float heWhite[4]		= { 1.0f, 1.0f, 1.0f, 1.0f };
static const float heDim[4]			= { 0.6f, 0.6f, 0.7f, 1.0f };
static const float heYellow[4]		= { 1.0f, 0.85f, 0.3f, 1.0f };
static const float heGreen[4]		= { 0.4f, 1.0f, 0.4f, 1.0f };

enum { HE_ALL, HE_BASE, HE_CUSTOM, HE_NUM_FILTERS };
static const char *heFilters[HE_NUM_FILTERS] = { "All", "Base", "Custom" };

// the sliders: x y z, pitch yaw roll, scale
enum { HS_X, HS_Y, HS_Z, HS_PITCH, HS_YAW, HS_ROLL, HS_SIZE, HS_NUM_SLIDERS };
static const struct {
	const char	*label;
	float		min, max, step;
} heSliders[HS_NUM_SLIDERS] = {
	{ "Forward",	-HS_OFFSET, HS_OFFSET, 0.25f },
	{ "Left",		-HS_OFFSET, HS_OFFSET, 0.25f },
	{ "Up",			-HS_OFFSET, HS_OFFSET, 0.25f },
	{ "Pitch",		-180.0f, 180.0f, 5.0f },
	{ "Yaw",		-180.0f, 180.0f, 5.0f },
	{ "Roll",		-180.0f, 180.0f, 5.0f },
	{ "Size",		HS_SCALE_MIN, HS_SCALE_MAX, 0.05f },
};

static const char *heHideNames[HS_HIDE_MODES] = { "Always shown", "Hidden: saber lit", "Hidden: saber in hand", "Only in the datapad emote" };

static struct {
	qboolean	open;
	int			font, titleFont;
	sfxHandle_t	clickSound;
	float		cursorX, cursorY;
	qboolean	click, mouseDown;
	float		clickX, clickY;
	int			wheel;				// the wheel's steps for an open list (negative up)

	qboolean	loaded;			// yours asked from C1B3R since it opened
	qboolean	refused;
	char		refusal[256];
	qboolean	changed;		// not saved yet
	hsItem_t	items[HS_MAX];
	int			num;
	int			sel;			// -1 none

	char		search[48];
	qboolean	typing;
	int			filter;
	int			fileScroll;

	char		tags[HS_MAX_TAGS][MAX_QPATH];
	int			numTags;
	char		tagsOf[MAX_QPATH];
	int			presetScroll;	// the picked one's presets, scrolled

	int			dragging;		// a slider, or -2 the preview turned; -1 none
	int			lastSlider;		// the arrows nudge it
	float		yaw;

	char		message[160];
	int			messageTime;
	qboolean	messageBad;

	// live: a small panel, what's edited worn on you in the world, the camera turned around you
	qboolean	live;
	char		camWas[4][32];	// cg_thirdPerson, its angle, range and height as they were

	// live, as the other editors: the keys held, the view the world was drawn from, a free camera,
	// and the gizmo (arrows to move, rings to turn) on the picked one
	qboolean	held[MAX_KEYS];
	qboolean	haveView;
	gzView_t	view;
	int			viewFrame;
	qboolean	freeCam;
	vec3_t		camOrg, camAng;
	int			lastFrameTime;
	int			gzMode;			// HE_MOVE, HE_TURN
	qboolean	gzWorldAxes;	// the world's, not the tag's
	qboolean	gzHidden;
	int			step;			// in heMoveSteps, heTurnSteps
	int			gzHot, gzPart;	// under the mouse, and held; -1 none
	gzDrag_t	gzDrag;
	vec3_t		gzDir;			// the arrow's way, or the ring's axis, in the world
	vec3_t		gzStartOff;		// the picked one as it was grabbed
	vec3_t		gzStartAxes[3];	// its axes in the world then
	float		gzTurned, gzLastAngle, gzSign;
} he;

enum { HE_MOVE, HE_TURN };
#define HE_DRAG_GIZMO	-4
#define HE_FLY_SPEED	120.0f		// units a second: near work
static const float heMoveSteps[3] = { 0.25f, 1.0f, 4.0f };
static const float heTurnSteps[3] = { 1.0f, 5.0f, 15.0f };
static const vec3_t heWorldAxes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
static const char * const heTagNames[3] = { "Fwd", "Left", "Up" };	// as the sliders call them

static const char *heCamCvars[4] = { "cg_thirdPerson", "cg_thirdPersonAngle", "cg_thirdPersonRange", "cg_thirdPersonVertOffset" };

static void HE_Message( const char *text, qboolean bad ) {
	Q_strncpyz( he.message, text, sizeof( he.message ) );
	for ( char *nl = strchr( he.message, '\n' ); nl; nl = NULL )
		*nl = '\0';
	he.messageTime = cls.realtime;
	he.messageBad = bad;
}

static void HE_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void HE_Box( float x, float y, float w, float h, const float *fill ) {
	HE_Fill( x, y, w, h, fill );
	HE_Fill( x, y, w, 1, heBorder );
	HE_Fill( x, y + h - 1, w, 1, heBorder );
	HE_Fill( x, y, 1, h, heBorder );
	HE_Fill( x + w - 1, y, 1, h, heBorder );
}

static void HE_Text( float x, float y, float w, const char *text, const float *color, int font, float scale, qboolean centred ) {
	WG_TypedText( x, y, w, text, color, font, scale, centred, qtrue );
}

static qboolean HE_InRect( float x, float y, float w, float h ) {
	if ( WG_PopupHover( he.cursorX, he.cursorY ) )
		return qfalse;
	return (qboolean)( he.cursorX >= x && he.cursorX < x + w && he.cursorY >= y && he.cursorY < y + h );
}

static qboolean HE_Clicked( float x, float y, float w, float h ) {
	if ( !he.click || he.clickX < x || he.clickX >= x + w || he.clickY < y || he.clickY >= y + h )
		return qfalse;
	he.click = qfalse;
	if ( he.clickSound )
		S_StartLocalSound( he.clickSound, CHAN_LOCAL_SOUND );
	return qtrue;
}

// the shared - value + control, in the editor's look and with its mouse
static void HE_PopStyle( wgStyle_t *style, const float *fill ) {
	memset( style, 0, sizeof( *style ) );
	style->font = he.font;
	style->scale = HE_TEXT;
	style->lineH = HE_CTRL_H;
	style->padX = 4.0f;
	style->padY = 2.0f;
	style->fill = fill;
	style->border = heBorder;
	style->text = heWhite;
	style->dim = heDim;
}

static void HE_PopMouse( wgMouse_t *m ) {
	const qboolean over = WG_PopupHover( he.cursorX, he.cursorY );

	m->cursorX = over ? -1.0f : he.cursorX;
	m->cursorY = over ? -1.0f : he.cursorY;
	m->click = &he.click;
	m->clickX = he.clickX;
	m->clickY = he.clickY;
	m->wheel = &he.wheel;
}

static const char *HE_TagAt( int i, void *ctx ) {
	return i >= 0 && i < he.numTags ? he.tags[i] : "";
}

static qboolean HE_Button( float x, float y, float w, const char *label, qboolean on, qboolean enabled ) {
	HE_Box( x, y, w, HE_CTRL_H, on ? heItemOn : enabled && HE_InRect( x, y, w, HE_CTRL_H ) ? heItemHover : heItem );
	HE_Text( x, y + 2, w, label, enabled ? heWhite : heDim, he.font, HE_TEXT, qtrue );
	return enabled ? HE_Clicked( x, y, w, HE_CTRL_H ) : qfalse;
}

// your model's tags, read again when it's another model
static void HE_LoadTags( void ) {
	char model[MAX_QPATH], *slash;

	Q_strncpyz( model, CL_PlayerInfo( clc.clientNum, "model" ), sizeof( model ) );
	if ( ( slash = strchr( model, '/' ) ) != NULL )
		*slash = '\0';
	if ( !Q_stricmp( model, he.tagsOf ) )
		return;
	Q_strncpyz( he.tagsOf, model, sizeof( he.tagsOf ) );
	he.numTags = CL_GlmTags( va( "models/players/%s/model.glm", model ), he.tags, HS_MAX_TAGS );
}

static qboolean HE_HasTag( const char *tag ) {
	for ( int i = 0; i < he.numTags; i++ ) {
		if ( !Q_stricmp( he.tags[i], tag ) )
			return qtrue;
	}
	return qfalse;
}

// a new attachment: the model given, on the hip or the back if the model has them
static void HE_Add( const char *model, int hide ) {
	hsItem_t *it;
	static const char *spots[] = { "*hip_r", "*hip_l", "*back", "*chestg", "*r_hand" };

	if ( he.num >= HS_MAX ) {
		HE_Message( va( "%i at most", HS_MAX ), qtrue );
		return;
	}
	it = &he.items[he.num];
	memset( it, 0, sizeof( *it ) );
	Q_strncpyz( it->model, model, sizeof( it->model ) );
	Q_strncpyz( it->source, HS_SourceOf( model ), sizeof( it->source ) );
	Q_strncpyz( it->bolt, he.numTags ? he.tags[0] : "*hip_r", sizeof( it->bolt ) );
	for ( int i = (int)ARRAY_LEN( spots ) - 1; i >= 0; i-- ) {
		if ( HE_HasTag( spots[i] ) )
			Q_strncpyz( it->bolt, spots[i], sizeof( it->bolt ) );
	}
	it->scale = 1.0f;
	it->hide = hide;
	// worn with the preset you're in, or with every preset when the look is none of them
	{
		const char *worn = CL_Presets_WornId( CL_Characters_ActiveId() );

		if ( worn[0] )
			Q_strncpyz( it->presets[it->numPresets++], worn, sizeof( it->presets[0] ) );
		else
			it->all = qtrue;
	}
	he.sel = he.num++;
	he.changed = qtrue;
}

static float *HE_SliderValue( hsItem_t *it, int s ) {
	return s <= HS_Z ? &it->off[s] : s <= HS_ROLL ? &it->ang[s - HS_PITCH] : &it->scale;
}

static void HE_Save( void ) {
	std::string fields = ",\"items\":[";

	for ( int i = 0; i < he.num; i++ ) {
		const hsItem_t *it = &he.items[i];

		fields += i ? ",{\"bolt\":" : "{\"bolt\":";
		CL_Presence_Json( fields, it->bolt );
		fields += ",\"model\":";
		CL_Presence_Json( fields, it->model );
		fields += ",\"source\":";
		CL_Presence_Json( fields, it->source );
		fields += va( ",\"off\":[%.2f,%.2f,%.2f],\"ang\":[%.1f,%.1f,%.1f],\"scale\":%.2f,\"hide\":%i,\"all\":%i,\"presets\":[",
			it->off[0], it->off[1], it->off[2], it->ang[0], it->ang[1], it->ang[2], it->scale, it->hide, it->all ? 1 : 0 );
		for ( int k = 0; k < it->numPresets; k++ )
			fields += va( "%s\"%s\"", k ? "," : "", it->presets[k] );
		fields += "]}";
	}
	fields += "]";
	if ( !HS_Post( &hsSetHttp, "/v2/char/holster/set", fields ) )
		HE_Message( CL_Presence_Session() ? "Wait: still saving" : "Not connected to C1B3R", qtrue );
}

// the preview's: what's being edited, on your body there
static void HE_DrawOnPreview( void *ctx, void *ghoul2, const refEntity_t *body ) {
	HS_AddItems( he.items, he.num, ghoul2, body, 0, HS_EMOTE_ALL );
}

// a slider: its label, its track, its value; dragged, or clicked along
static void HE_Slider( float x, float y, float w, int s, hsItem_t *it ) {
	const float label = 46, valueW = 40, tx = x + label, tw = w - label - valueW;
	float *v = HE_SliderValue( it, s );
	const float t = ( *v - heSliders[s].min ) / ( heSliders[s].max - heSliders[s].min );

	HE_Text( x, y + 1, label, heSliders[s].label, s == he.lastSlider ? heOrange : heDim, he.font, HE_SMALL, qfalse );
	HE_Fill( tx, y + 6, tw, 3, heItem );
	// the middle (nothing moved) marked
	if ( heSliders[s].min < 0 )
		HE_Fill( tx + tw * 0.5f, y + 3, 1, 9, heBorder );
	HE_Fill( tx + tw * t - 3, y + 2, 6, 11, he.dragging == s ? heWhite : heOrange );
	HE_Text( x + w - valueW + 4, y + 1, valueW - 4, s == HS_SIZE ? va( "%.2f", *v ) : s >= HS_PITCH ? va( "%.0f", *v ) : va( "%.2f", *v ),
		heWhite, he.font, HE_SMALL, qfalse );
	if ( HE_Clicked( tx - 4, y, tw + 8, 14 ) ) {
		he.dragging = s;
		he.lastSlider = s;
	}
	if ( he.dragging == s ) {
		const float f = Com_Clamp( 0.0f, 1.0f, ( he.cursorX - tx ) / tw );
		float value = heSliders[s].min + f * ( heSliders[s].max - heSliders[s].min );

		value = roundf( value / heSliders[s].step ) * heSliders[s].step;
		if ( value != *v ) {
			*v = Com_Clamp( heSliders[s].min, heSliders[s].max, value );
			he.changed = qtrue;
		}
	}
}

static float HE_DrawWornOn( float x, float y, float w );

// the picked one's presets: every preset, or a ticked list of the character's (to the bottom given)
static qboolean HE_HasPreset( const hsItem_t *it, const char *id ) {
	for ( int k = 0; k < it->numPresets; k++ ) {
		if ( !strcmp( it->presets[k], id ) )
			return qtrue;
	}
	return qfalse;
}

static void HE_TogglePreset( hsItem_t *it, const char *id ) {
	for ( int k = 0; k < it->numPresets; k++ ) {
		if ( !strcmp( it->presets[k], id ) ) {
			memmove( it->presets[k], it->presets[k + 1], ( it->numPresets - k - 1 ) * sizeof( it->presets[0] ) );
			it->numPresets--;
			return;
		}
	}
	if ( it->numPresets < HS_ITEM_PRESETS )
		Q_strncpyz( it->presets[it->numPresets++], id, sizeof( it->presets[0] ) );
}

// the character's presets that can be added to the picked one, by their index in the presets list
static int heAddable[256];

static const char *HE_AddableAt( int i, void *ctx ) {
	return i == 0 ? "Add it to a preset..." : CL_Presets_Title( heAddable[i - 1] );
}

static void HE_DrawWornWith( float x, float y, float w, float bottom ) {
	hsItem_t *it = &he.items[he.sel];
	const int character = CL_Characters_ActiveId();
	const char *worn = CL_Presets_WornId( character );
	int addable = 0, total = 0, rows;
	const float rowH = HE_CTRL_H + 2;

	HE_Text( x, y, w, "Worn with", heOrange, he.font, HE_SMALL, qfalse );
	y += 12;
	if ( HE_Button( x, y, w, it->all ? "[x] Every preset" : "[  ] Every preset", it->all, qtrue ) ) {
		it->all = (qboolean)!it->all;
		he.changed = qtrue;
	}
	y += rowH;
	if ( it->all )
		return;
	// the character's presets it isn't worn with yet, in one list to pick from
	for ( int i = 0; i < CL_Presets_Count() && addable < (int)ARRAY_LEN( heAddable ); i++ ) {
		const char *date;
		qboolean favorite;
		int owner;

		CL_Presets_Get( i, 0, &date, &favorite, &owner );
		if ( owner != character || !character || !CL_Presets_Id( i )[0] )
			continue;
		total++;
		if ( !HE_HasPreset( it, CL_Presets_Id( i ) ) )
			heAddable[addable++] = i;
	}
	if ( !total ) {
		HE_Text( x, y, w, character ? "This character has no presets yet" : "Presets need your characters (C1B3R)", heDim, he.font, 0.55f, qfalse );
		return;
	}
	if ( addable ) {
		static int pick;
		wgStyle_t style;
		wgMouse_t m;

		HE_PopStyle( &style, heItem );
		HE_PopMouse( &m );
		pick = 0;
		if ( WG_Choice( &style, &m, x, y, w, HE_CTRL_H, &pick, addable + 1, HE_AddableAt, NULL, qfalse ) && pick > 0
			&& it->numPresets < HS_ITEM_PRESETS ) {
			Q_strncpyz( it->presets[it->numPresets++], CL_Presets_Id( heAddable[pick - 1] ), sizeof( it->presets[0] ) );
			he.changed = qtrue;
		}
	} else {
		HE_Text( x, y + 2, w, "On every one of its presets", heDim, he.font, 0.55f, qfalse );
	}
	y += rowH + 2;
	// those it's worn with: x takes one off; the one you're in now marked
	if ( !it->numPresets ) {
		HE_Text( x, y, w, "Not worn yet: add it to a preset", heYellow, he.font, 0.55f, qfalse );
		return;
	}
	rows = Q_max( 1, (int)( ( bottom - y ) / rowH ) );
	he.presetScroll = Com_Clampi( 0, Q_max( 0, it->numPresets - rows ), he.presetScroll );
	if ( it->numPresets > rows ) {
		WG_Scrollbar( x + w - WG_SCROLL_W, y, rows * rowH - 2, it->numPresets, rows, &he.presetScroll, he.cursorX, he.cursorY, &he.click, he.clickX, he.clickY );
		w -= WG_SCROLL_W + 2;
	}
	for ( int r = 0; r < rows && he.presetScroll + r < it->numPresets; r++, y += rowH ) {
		const int k = he.presetScroll + r;
		const char *title = "(a preset since deleted)";

		for ( int i = 0; i < CL_Presets_Count(); i++ ) {
			if ( !strcmp( CL_Presets_Id( i ), it->presets[k] ) )
				title = CL_Presets_Title( i );
		}
		HE_Box( x, y, w - 22, HE_CTRL_H, heField );
		HE_Text( x + 4, y + 2, w - 30, va( "%s%s", title, !strcmp( it->presets[k], worn ) ? S_COLOR_WHITE " (now)" : "" ), heWhite, he.font, HE_SMALL, qfalse );
		if ( HE_Button( x + w - 20, y, 20, "x", qfalse, qtrue ) ) {
			HE_TogglePreset( it, it->presets[k] );
			he.changed = qtrue;
			return;
		}
	}
}

static void HE_DrawList( void ) {
	const float x = HE_A_X, w = HE_A_W;
	float y = HE_TOP;

	HE_Text( x, y, w, va( "Worn (%i of %i)", he.num, HS_MAX ), heOrange, he.font, HE_TEXT, qfalse );
	y += 16;
	for ( int i = 0; i < HS_MAX; i++, y += HE_ROW_H ) {
		const qboolean on = (qboolean)( i == he.sel ), hover = HE_InRect( x, y, w, HE_ROW_H - 2 );

		if ( i >= he.num ) {
			HE_Box( x, y, w, HE_ROW_H - 2, heField );
			continue;
		}
		HE_Box( x, y, w, HE_ROW_H - 2, on ? heItemOn : hover ? heItemHover : heItem );
		HE_Text( x + 4, y + 1, w - 8, COM_SkipPath( he.items[i].model ), on || hover ? heWhite : heOrange, he.font, HE_SMALL, qfalse );
		if ( !he.items[i].all && !he.items[i].numPresets )
			HE_Text( x + 4, y + 10, w - 8, "not worn: pick its presets", heYellow, he.font, 0.55f, qfalse );
		else
			HE_Text( x + 4, y + 10, w - 8, he.items[i].bolt, heDim, he.font, 0.55f, qfalse );
		if ( HE_Clicked( x, y, w, HE_ROW_H - 2 ) )
			he.sel = i;
	}
	y += 4;
	{
		const float bw = ( w - 4 ) * 0.5f;
		clSaber_t saber;

		if ( HE_Button( x, y, bw, "Remove", qfalse, (qboolean)( he.sel >= 0 && he.sel < he.num ) ) ) {
			memmove( &he.items[he.sel], &he.items[he.sel + 1], ( he.num - he.sel - 1 ) * sizeof( hsItem_t ) );
			he.num--;
			he.sel = he.num ? Q_min( he.sel, he.num - 1 ) : -1;
			he.changed = qtrue;
		}
		if ( HE_Button( x + bw + 4, y, bw, "Your hilt", qfalse,
			(qboolean)( he.num < HS_MAX && CL_Saber_Find( CL_PlayerInfo( clc.clientNum, "st" ), &saber ) && saber.model[0] ) ) )
			HE_Add( saber.model, 1 );
	}
	y += HE_CTRL_H + 12;

	// the picked one: where it's worn, and when it hides
	if ( he.sel < 0 || he.sel >= he.num ) {
		HE_Text( x, y, w, "Pick a model in the middle to wear it", heDim, he.font, HE_SMALL, qfalse );
		return;
	}
	y = HE_DrawWornOn( x, y, w );
	HE_DrawWornWith( x, y, w, HE_BOTTOM );
}

// the picked one's tag and hiding
static float HE_DrawWornOn( float x, float y, float w ) {
	{
		hsItem_t *it = &he.items[he.sel];
		int at = -1;

		for ( int i = 0; i < he.numTags; i++ ) {
			if ( !Q_stricmp( he.tags[i], it->bolt ) )
				at = i;
		}
		wgStyle_t style;
		wgMouse_t m;

		HE_Text( x, y, w, "Worn on", heOrange, he.font, HE_SMALL, qfalse );
		y += 12;
		// the model's tags: - and + step, the value opens the list
		HE_PopStyle( &style, heField );
		HE_PopMouse( &m );
		if ( he.numTags > 0 ) {
			int pick = at;

			if ( WG_Choice( &style, &m, x, y, w, HE_CTRL_H, &pick, he.numTags, HE_TagAt, NULL, qtrue ) && pick >= 0 ) {
				Q_strncpyz( it->bolt, he.tags[pick], sizeof( it->bolt ) );
				he.changed = qtrue;
			}
			// one the model hasn't: its name, in yellow
			if ( at < 0 )
				HE_Text( x + HE_CTRL_H, y + 2, w - HE_CTRL_H * 2, it->bolt, heYellow, he.font, HE_TEXT, qtrue );
		} else {
			HE_Box( x, y, w, HE_CTRL_H, heField );
			HE_Text( x, y + 2, w, it->bolt, heYellow, he.font, HE_TEXT, qtrue );
		}
		y += HE_CTRL_H + 6;
		// when it hides
		HE_PopStyle( &style, it->hide ? heItemOn : heItem );
		if ( WG_ChoiceList( &style, &m, x, y, w, HE_CTRL_H, &it->hide, heHideNames, HS_HIDE_MODES, qtrue ) )
			he.changed = qtrue;
		y += HE_CTRL_H + 6;
		if ( at < 0 ) {
			HE_Text( x, y, w, "Your model hasn't this tag", heYellow, he.font, HE_SMALL, qfalse );
			y += 12;
		}
	}
	return y;
}

static void HE_FilterFiles( void ) {
	hsShown.clear();
	for ( int i = 0; i < (int)hsFiles.size(); i++ ) {
		const hsFile_t &f = hsFiles[i];

		if ( ( he.filter == HE_BASE && f.custom ) || ( he.filter == HE_CUSTOM && !f.custom ) )
			continue;
		if ( he.search[0] && !Q_stristr( f.path.c_str(), he.search ) )
			continue;
		hsShown.push_back( i );
	}
}

static void HE_DrawFiles( void ) {
	const float x = HE_B_X, w = HE_B_W, top = HE_TOP + 2 * ( HE_CTRL_H + 4 ), h = HE_BOTTOM - top;
	const int visible = (int)( h / HE_FILE_H );
	wgStyle_t style;
	qboolean bar;
	float y = HE_TOP, rw;

	// the search, and which models
	memset( &style, 0, sizeof( style ) );
	style.font = he.font;
	style.scale = HE_TEXT;
	style.lineH = 13.0f;
	style.padX = 4;
	style.padY = 2;
	style.fill = heField;
	style.border = heBorder;
	style.text = heWhite;
	style.dim = heDim;
	HE_Box( x, y, w, HE_CTRL_H, heField );
	if ( he.typing )
		HE_Fill( x, y + HE_CTRL_H - 1, w, 1, heOrange );
	WG_Field( &style, x, y, w, HE_CTRL_H, he.search, he.typing, "Search models", he.cursorX, he.cursorY );
	if ( HE_Clicked( x, y, w, HE_CTRL_H ) ) {
		if ( !he.typing )
			WG_EditReset();
		he.typing = qtrue;
		WG_FieldClick( he.search, he.clickX, he.clickY );
	}
	y += HE_CTRL_H + 4;
	{
		const float fw = ( w - 2 * 3 ) / HE_NUM_FILTERS;

		for ( int f = 0; f < HE_NUM_FILTERS; f++ ) {
			if ( HE_Button( x + f * ( fw + 3 ), y, fw, heFilters[f], (qboolean)( he.filter == f ), qtrue ) ) {
				he.filter = f;
				he.fileScroll = 0;
			}
		}
	}

	HE_FilterFiles();
	bar = WG_Scrollbar( x + w - WG_SCROLL_W, top, h, (int)hsShown.size(), visible, &he.fileScroll, he.cursorX, he.cursorY, &he.click, he.clickX, he.clickY );
	rw = w - ( bar ? WG_SCROLL_W + 2 : 0 );
	he.fileScroll = Com_Clampi( 0, Q_max( 0, (int)hsShown.size() - visible ), he.fileScroll );
	if ( hsShown.empty() )
		HE_Text( x, top + 8, w, hsFiles.empty() ? "No models found" : "None matches", heDim, he.font, HE_TEXT, qtrue );
	y = top;
	for ( int i = he.fileScroll; i < (int)hsShown.size() && i < he.fileScroll + visible; i++, y += HE_FILE_H ) {
		const hsFile_t &f = hsFiles[hsShown[i]];
		const qboolean worn = (qboolean)( he.sel >= 0 && he.sel < he.num && !Q_stricmp( he.items[he.sel].model, f.path.c_str() ) );
		const qboolean hover = HE_InRect( x, y, rw, HE_FILE_H - 1 );
		const char *dir = f.path.c_str() + 7;	// after models/

		if ( worn || hover )
			HE_Fill( x, y, rw, HE_FILE_H - 1, worn ? heItemOn : heItemHover );
		HE_Text( x + 3, y + 1, rw - 6, va( "%s" S_COLOR_GREY "  %.*s", COM_SkipPath( (char *)f.path.c_str() ),
			(int)( COM_SkipPath( (char *)f.path.c_str() ) - dir ), dir ), f.custom ? heYellow : heWhite, he.font, HE_SMALL, qfalse );
		// a click puts it on: on the one picked, or as a new one
		if ( HE_Clicked( x, y, rw, HE_FILE_H - 1 ) ) {
			he.typing = qfalse;
			if ( he.sel >= 0 && he.sel < he.num ) {
				Q_strncpyz( he.items[he.sel].model, f.path.c_str(), sizeof( he.items[0].model ) );
				Q_strncpyz( he.items[he.sel].source, f.source.c_str(), sizeof( he.items[0].source ) );
				he.changed = qtrue;
			} else {
				HE_Add( f.path.c_str(), 0 );
			}
		}
	}
	HE_Text( x, HE_BOTTOM + 4, w, "Yellow: from a custom pk3 (others need it too)", heDim, he.font, 0.55f, qfalse );
}

static void HE_DrawPreview( void ) {
	const float x = HE_C_X, w = HE_C_W;
	char model[MAX_QPATH], saber1[MAX_QPATH], saber2[MAX_QPATH];
	float y = HE_TOP;

	Q_strncpyz( model, CL_PlayerInfo( clc.clientNum, "model" ), sizeof( model ) );
	Q_strncpyz( saber1, CL_PlayerInfo( clc.clientNum, "st" ), sizeof( saber1 ) );
	Q_strncpyz( saber2, CL_PlayerInfo( clc.clientNum, "st2" ), sizeof( saber2 ) );
	HE_Box( x, y, w, HE_PREVIEW_H, heField );
	// dragged: turned as the mouse goes
	if ( he.dragging == -2 ) {
		he.yaw += ( he.cursorX - he.clickX ) * 1.5f;
		he.clickX = he.cursorX;
	}
	CL_Presets_DrawCharacter( model, saber1, saber2, NULL, NULL, NULL, x + 1, y + 1, w - 2, HE_PREVIEW_H - 2, he.yaw, HE_DrawOnPreview, NULL );
	HE_Text( x, y + HE_PREVIEW_H - 13, w, "drag to turn", heDim, he.font, 0.5f, qtrue );
	if ( HE_Clicked( x, y, w, HE_PREVIEW_H ) ) {
		he.dragging = -2;
		he.clickX = he.cursorX;
	}
	y += HE_PREVIEW_H + 8;
	if ( he.sel < 0 || he.sel >= he.num )
		return;
	for ( int s = 0; s < HS_NUM_SLIDERS; s++, y += 18 ) {
		if ( s == HS_PITCH || s == HS_SIZE )
			y += 4;
		HE_Slider( x, y, w, s, &he.items[he.sel] );
	}
	y += 4;
	if ( HE_Button( x, y, w, "Centre it on the tag", qfalse, qtrue ) ) {
		VectorClear( he.items[he.sel].off );
		VectorClear( he.items[he.sel].ang );
		he.items[he.sel].scale = 1.0f;
		he.changed = qtrue;
	}
}

static void HE_Close( void );

static qboolean HE_Live( void ) {
	return (qboolean)( he.open && he.live && he.loaded );
}

// what's being edited, on your body in the world
static void HE_AddLive( void *ghoul2, const refEntity_t *body, int saber ) {
	// the picked one's tag kept for the gizmo
	hsKeep.item = he.sel;
	HS_AddItems( he.items, he.num, ghoul2, body, saber, HS_EMOTE_ALL );
	hsKeep.item = -1;
}

static void HE_LiveCam( int i, float value ) {
	Cvar_Set( heCamCvars[i], va( "%g", value ) );
}

/*
===============================================================================

Live: the view, a free camera, the gizmo and the keys

===============================================================================
*/

static qboolean HE_InPanel( float x, float y );

static void HE_DropDrag( void ) {
	if ( he.dragging == HE_DRAG_GIZMO )
		he.dragging = -1;
	he.gzPart = he.gzHot = -1;
}

static qboolean HE_ShiftDown( void ) {
	return (qboolean)( he.held[A_SHIFT] || he.held[A_SHIFT2] );
}

static qboolean HE_CtrlDown( void ) {
	return (qboolean)( he.held[A_CTRL] || he.held[A_CTRL2] );
}

// a step as G set it, Shift for 4 of them, Ctrl for a quarter
static float HE_Step( qboolean keys ) {
	float step = ( he.gzMode == HE_TURN ? heTurnSteps : heMoveSteps )[he.step];

	if ( keys && HE_ShiftDown() )
		step *= 4.0f;
	if ( keys && HE_CtrlDown() )
		step *= 0.25f;
	return step;
}

static const char *HE_StepText( void ) {
	return he.gzMode == HE_TURN ? va( "%g deg", heTurnSteps[he.step] ) : va( "%g unit%s", heMoveSteps[he.step], heMoveSteps[he.step] == 1.0f ? "" : "s" );
}

static void HE_KeepView( const refdef_t *fd ) {
	VectorCopy( fd->vieworg, he.view.org );
	for ( int k = 0; k < 3; k++ )
		VectorCopy( fd->viewaxis[k], he.view.axis[k] );
	he.view.fovX = fd->fov_x;
	he.view.fovY = fd->fov_y;
	he.haveView = qtrue;
}

// every cgame scene passes through here first: the world's view kept for the gizmo, or the free
// camera's put in its place; qtrue if it was rendered
qboolean CL_Holster_RenderScene( const refdef_t *fd ) {
	refdef_t view;

	if ( !HE_Live() || ( fd->rdflags & ( RDF_NOWORLDMODEL | RDF_AUTOMAP ) ) )
		return qfalse;
	if ( fd->rdflags & RDF_SKYBOXPORTAL ) {
		// the sky portal keeps its own origin but looks the way the camera does
		if ( !he.freeCam )
			return qfalse;
		view = *fd;
		AnglesToAxis( he.camAng, view.viewaxis );
		VectorCopy( he.camAng, view.viewangles );
		re->RenderScene( &view );
		return qtrue;
	}
	// the main view, once a frame
	if ( he.viewFrame == cls.framecount )
		return qfalse;
	he.viewFrame = cls.framecount;
	if ( !he.freeCam ) {
		HE_KeepView( fd );
		return qfalse;
	}
	view = *fd;
	VectorCopy( he.camOrg, view.vieworg );
	VectorCopy( he.camAng, view.viewangles );
	AnglesToAxis( he.camAng, view.viewaxis );
	view.viewContents = CM_PointContents( he.camOrg, 0 );
	// the snapshot's area mask is from where you stand, the camera can be anywhere
	memset( view.areamask, 0, sizeof( view.areamask ) );
	HE_KeepView( &view );
	re->RenderScene( &view );
	return qtrue;
}

static void HE_FreeCam( qboolean on ) {
	HE_DropDrag();
	if ( on && !he.haveView ) {
		HE_Message( "Wait a moment: the world isn't drawn yet", qtrue );
		return;
	}
	he.freeCam = on;
	if ( on ) {
		// from where the camera is now
		VectorCopy( he.view.org, he.camOrg );
		vectoangles( he.view.axis[0], he.camAng );
		he.camAng[ROLL] = 0.0f;
		he.camAng[PITCH] = AngleNormalize180( he.camAng[PITCH] );
	}
}

static void HE_UpdateCamera( float dt ) {
	vec3_t forward, right, move;
	float speed;

	AngleVectors( he.camAng, forward, right, NULL );
	VectorClear( move );
	if ( he.held[A_CAP_W] )		VectorAdd( move, forward, move );
	if ( he.held[A_CAP_S] )		VectorSubtract( move, forward, move );
	if ( he.held[A_CAP_D] )		VectorAdd( move, right, move );
	if ( he.held[A_CAP_A] )		VectorSubtract( move, right, move );
	if ( he.held[A_SPACE] )		move[2] += 1.0f;
	if ( he.held[A_CAP_C] )		move[2] -= 1.0f;
	if ( VectorNormalize( move ) > 0.0f ) {
		speed = HE_FLY_SPEED;
		if ( HE_ShiftDown() )
			speed *= 3.0f;
		if ( HE_CtrlDown() )
			speed *= 0.25f;
		VectorMA( he.camOrg, speed * dt, move, he.camOrg );
	}
}

// the picked one's tag as it was drawn this frame or the last; qfalse if it wasn't (hidden, not
// in view, no tag)
static qboolean HE_HaveTag( void ) {
	return (qboolean)( HE_Live() && he.sel >= 0 && he.sel < he.num && he.haveView
		&& hsKeep.of == he.sel && hsKeep.frame >= cls.framecount - 1 );
}

static void HE_Pivot( const hsItem_t *it, vec3_t pivot ) {
	VectorCopy( hsKeep.org, pivot );
	for ( int k = 0; k < 3; k++ )
		VectorMA( pivot, it->off[k], hsKeep.axis[k], pivot );
}

// the picked one's own axes in the world, as HS_AddItems turns it
static void HE_ItemAxes( const hsItem_t *it, vec3_t out[3] ) {
	matrix3_t r;

	AnglesToAxis( it->ang, r );
	for ( int a = 0; a < 3; a++ ) {
		for ( int k = 0; k < 3; k++ )
			out[a][k] = r[a][0] * hsKeep.axis[0][k] + r[a][1] * hsKeep.axis[1][k] + r[a][2] * hsKeep.axis[2][k];
	}
}

// the arrows' ways, or the rings' axes: the tag's (the item's own, for rings) or the world's
static void HE_GizmoAxes( const hsItem_t *it, vec3_t out[3] ) {
	if ( he.gzWorldAxes ) {
		for ( int k = 0; k < 3; k++ )
			VectorCopy( heWorldAxes[k], out[k] );
	} else if ( he.gzMode == HE_TURN ) {
		HE_ItemAxes( it, out );
	} else {
		for ( int k = 0; k < 3; k++ )
			VectorCopy( hsKeep.axis[k], out[k] );
	}
}

// moved by d in the world from where it was (from, along the tag's axes), held near the tag
static void HE_MoveBy( hsItem_t *it, const vec3_t from, const vec3_t d ) {
	qboolean limit = qfalse;

	for ( int k = 0; k < 3; k++ ) {
		float v = from[k] + DotProduct( d, hsKeep.axis[k] );

		if ( fabsf( v ) > HS_OFFSET ) {
			v = v > 0 ? HS_OFFSET : -HS_OFFSET;
			limit = qtrue;
		}
		it->off[k] = roundf( v * 100.0f ) / 100.0f;
	}
	he.changed = qtrue;
	if ( limit )
		HE_Message( va( "At the limit: %g units from the tag", HS_OFFSET ), qtrue );
}

// turned by degrees round axis (in the world) from the axes it had (in the world)
static void HE_TurnBy( hsItem_t *it, const vec3_t start[3], const vec3_t axis, float degrees ) {
	matrix3_t r;
	vec3_t n;

	for ( int a = 0; a < 3; a++ ) {
		RotatePointAroundVector( n, axis, start[a], degrees );
		for ( int j = 0; j < 3; j++ )
			r[a][j] = DotProduct( n, hsKeep.axis[j] );
	}
	GZ_AxisAngles( r, it->ang );
	for ( int k = 0; k < 3; k++ )
		it->ang[k] = AngleNormalize180( roundf( it->ang[k] * 10.0f ) / 10.0f );
	he.changed = qtrue;
}

// the gizmo on the picked one: dragged, hovered, grabbed by a click outside the panel, drawn
static void HE_GizmoFrame( void ) {
	hsItem_t *it;
	vec3_t pivot, axes[3];
	int lit;

	if ( he.gzHidden || !HE_HaveTag() ) {
		HE_DropDrag();
		return;
	}
	it = &he.items[he.sel];
	HE_Pivot( it, pivot );
	if ( he.dragging == HE_DRAG_GIZMO ) {
		const float step = HE_Step( qfalse );

		if ( he.gzMode == HE_MOVE ) {
			vec3_t d;

			VectorScale( he.gzDir, roundf( GZ_Amount( &he.gzDrag, he.cursorX, he.cursorY, 0 ) / step ) * step, d );
			HE_MoveBy( it, he.gzStartOff, d );
		} else {
			const float now = GZ_ScreenAngle( &he.view, pivot, he.cursorX, he.cursorY );
			float delta = now - he.gzLastAngle;

			// added up a frame at a time, so it goes on round past half a turn
			while ( delta > M_PI )
				delta -= 2.0f * M_PI;
			while ( delta < -M_PI )
				delta += 2.0f * M_PI;
			he.gzTurned += delta;
			he.gzLastAngle = now;
			HE_TurnBy( it, he.gzStartAxes, he.gzDir, roundf( he.gzSign * RAD2DEG( he.gzTurned ) / step ) * step );
		}
		HE_Pivot( it, pivot );
	}
	HE_GizmoAxes( it, axes );
	he.gzHot = -1;
	if ( he.dragging == -1 && !HE_InPanel( he.cursorX, he.cursorY ) )
		he.gzHot = he.gzMode == HE_MOVE ? GZ_HitAxes( &he.view, pivot, axes, he.cursorX, he.cursorY )
			: GZ_HitRings( &he.view, pivot, axes, he.cursorX, he.cursorY );
	// grabbed
	if ( he.click && he.dragging == -1 && !HE_InPanel( he.clickX, he.clickY ) ) {
		const int part = he.gzMode == HE_MOVE ? GZ_HitAxes( &he.view, pivot, axes, he.clickX, he.clickY )
			: GZ_HitRings( &he.view, pivot, axes, he.clickX, he.clickY );

		if ( part >= 0 ) {
			he.click = qfalse;
			he.dragging = HE_DRAG_GIZMO;
			he.gzPart = part;
			VectorCopy( axes[part], he.gzDir );
			VectorCopy( it->off, he.gzStartOff );
			if ( he.gzMode == HE_MOVE ) {
				GZ_BeginDir( &he.gzDrag, &he.view, pivot, he.gzDir, part, he.clickX, he.clickY );
			} else {
				HE_ItemAxes( it, he.gzStartAxes );
				he.gzTurned = 0.0f;
				he.gzLastAngle = GZ_ScreenAngle( &he.view, pivot, he.clickX, he.clickY );
				he.gzSign = GZ_RingSign( &he.view, pivot, he.gzDir );
			}
		}
	}
	lit = he.dragging == HE_DRAG_GIZMO ? he.gzPart : he.gzHot;
	if ( he.gzMode == HE_MOVE )
		GZ_DrawAxes( &he.view, pivot, axes, lit, he.gzWorldAxes ? NULL : heTagNames );
	else
		GZ_DrawRings( &he.view, pivot, axes, lit );
}

// a step with the keys: moved toward where the camera looks (the tag's axis nearest it, or the
// world's), or turned
static void HE_KeyStep( int key ) {
	hsItem_t *it;
	const float step = HE_Step( qtrue );

	if ( he.sel < 0 || he.sel >= he.num )
		return;
	it = &he.items[he.sel];
	if ( he.gzMode == HE_TURN ) {
		const int a = key == A_CURSOR_LEFT || key == A_CURSOR_RIGHT ? YAW : key == A_CURSOR_UP || key == A_CURSOR_DOWN ? PITCH : ROLL;
		const float sign = key == A_CURSOR_LEFT || key == A_CURSOR_UP || key == A_PAGE_UP ? 1.0f : -1.0f;

		it->ang[a] = AngleNormalize180( roundf( ( it->ang[a] + sign * step ) * 10.0f ) / 10.0f );
		he.changed = qtrue;
		return;
	}
	if ( !HE_HaveTag() ) {
		// not drawn now: along the tag's axes as the sliders are
		const int k = key == A_CURSOR_UP || key == A_CURSOR_DOWN ? 0 : key == A_CURSOR_LEFT || key == A_CURSOR_RIGHT ? 1 : 2;
		const float sign = key == A_CURSOR_UP || key == A_CURSOR_LEFT || key == A_PAGE_UP ? 1.0f : -1.0f;

		it->off[k] = Com_Clamp( -HS_OFFSET, HS_OFFSET, it->off[k] + sign * step );
		he.changed = qtrue;
		return;
	}
	{
		vec3_t angles, forward, right, want, d;
		float yaw;

		// the camera's way, square to the world
		vectoangles( he.view.axis[0], angles );
		yaw = DEG2RAD( floorf( angles[YAW] / 90.0f + 0.5f ) * 90.0f );
		VectorSet( forward, cosf( yaw ), sinf( yaw ), 0 );
		VectorSet( right, sinf( yaw ), -cosf( yaw ), 0 );
		switch ( key ) {
		case A_CURSOR_UP:		VectorCopy( forward, want ); break;
		case A_CURSOR_DOWN:		VectorScale( forward, -1.0f, want ); break;
		case A_CURSOR_RIGHT:	VectorCopy( right, want ); break;
		case A_CURSOR_LEFT:		VectorScale( right, -1.0f, want ); break;
		case A_PAGE_UP:			VectorSet( want, 0, 0, 1 ); break;
		default:				VectorSet( want, 0, 0, -1 ); break;
		}
		if ( he.gzWorldAxes ) {
			VectorScale( want, step, d );
		} else {
			int best = 0;

			for ( int k = 1; k < 3; k++ ) {
				if ( fabsf( DotProduct( want, hsKeep.axis[k] ) ) > fabsf( DotProduct( want, hsKeep.axis[best] ) ) )
					best = k;
			}
			VectorScale( hsKeep.axis[best], DotProduct( want, hsKeep.axis[best] ) >= 0.0f ? step : -step, d );
		}
		HE_MoveBy( it, it->off, d );
	}
}

// the live keys; qtrue if it was one
static qboolean HE_LiveKey( int key ) {
	switch ( key ) {
	case A_TAB:
		HE_DropDrag();
		if ( HE_ShiftDown() ) {
			if ( he.num > 1 )
				he.sel = ( he.sel + 1 ) % he.num;
		} else {
			he.gzMode = he.gzMode == HE_MOVE ? HE_TURN : HE_MOVE;
		}
		return qtrue;
	case A_CAP_L:
		HE_DropDrag();
		he.gzWorldAxes = (qboolean)!he.gzWorldAxes;
		return qtrue;
	case A_CAP_G:
		he.step = ( he.step + 1 ) % 3;
		return qtrue;
	case GZ_KEY:
		HE_DropDrag();
		he.gzHidden = (qboolean)!he.gzHidden;
		return qtrue;
	case A_CAP_F:
		HE_FreeCam( (qboolean)!he.freeCam );
		return qtrue;
	case A_OPEN_BRACKET:
	case A_CLOSE_BRACKET:
		if ( he.sel >= 0 && he.sel < he.num ) {
			float *v = &he.items[he.sel].scale;
			const float step = heSliders[HS_SIZE].step * ( HE_ShiftDown() ? 4.0f : 1.0f );

			*v = Com_Clamp( HS_SCALE_MIN, HS_SCALE_MAX, roundf( ( *v + ( key == A_OPEN_BRACKET ? -step : step ) ) * 100.0f ) / 100.0f );
			he.changed = qtrue;
		}
		return qtrue;
	case A_CURSOR_LEFT:
	case A_CURSOR_RIGHT:
	case A_CURSOR_UP:
	case A_CURSOR_DOWN:
	case A_PAGE_UP:
	case A_PAGE_DOWN:
		HE_KeyStep( key );
		return qtrue;
	case A_MWHEELUP:
	case A_MWHEELDOWN:
		if ( he.freeCam ) {
			vec3_t forward;

			AngleVectors( he.camAng, forward, NULL, NULL );
			VectorMA( he.camOrg, key == A_MWHEELUP ? 8.0f : -8.0f, forward, he.camOrg );
		} else {
			HE_LiveCam( 2, Com_Clamp( 25.0f, 200.0f, Cvar_VariableValue( "cg_thirdPersonRange" ) + ( key == A_MWHEELUP ? -8 : 8 ) ) );
		}
		return qtrue;
	case A_CAP_W: case A_CAP_A: case A_CAP_S: case A_CAP_D: case A_CAP_C: case A_SPACE:
	case A_MOUSE2:
		return qtrue;		// held: the free camera's
	default:
		return qfalse;
	}
}

static void HE_LiveEnter( void ) {
	if ( he.live )
		return;
	for ( int i = 0; i < 4; i++ )
		Q_strncpyz( he.camWas[i], Cvar_VariableString( heCamCvars[i] ), sizeof( he.camWas[i] ) );
	he.live = qtrue;
	he.typing = qfalse;
	he.dragging = -1;
	he.freeCam = qfalse;
	he.haveView = qfalse;
	he.gzHot = he.gzPart = -1;
	// in front of you, a little to the side, near
	HE_LiveCam( 0, 1 );
	HE_LiveCam( 1, 150 );
	HE_LiveCam( 2, 60 );
	HE_LiveCam( 3, 0 );
}

// the camera as it was
static void HE_LiveLeave( void ) {
	if ( !he.live )
		return;
	he.live = qfalse;
	he.dragging = -1;
	he.freeCam = qfalse;
	he.gzHot = he.gzPart = -1;
	for ( int i = 0; i < 4; i++ )
		Cvar_Set( heCamCvars[i], he.camWas[i] );
}

#define HE_LIVE_W		190.0f
#define HE_LIVE_X		( SCREEN_WIDTH - HE_LIVE_W - 10 )
#define HE_LIVE_Y		24.0f
#define HE_LIVE_H		( SCREEN_HEIGHT - 48.0f )

static qboolean HE_InPanel( float x, float y ) {
	return he.live ? (qboolean)( x >= HE_LIVE_X && x < HE_LIVE_X + HE_LIVE_W && y >= HE_LIVE_Y && y < HE_LIVE_Y + HE_LIVE_H )
		: (qboolean)( x >= HE_X && x < HE_X + HE_W && y >= HE_Y && y < HE_Y + HE_H );
}

// the small panel: the picked one, its tag, hiding and sliders; the world round it turns the camera
static void HE_DrawLive( void ) {
	const float x = HE_LIVE_X + 8, w = HE_LIVE_W - 16;
	float y = HE_LIVE_Y + 8;

	// first, under the panel: a click outside it on an arrow or a ring is the gizmo's
	HE_GizmoFrame();
	HE_Box( HE_LIVE_X, HE_LIVE_Y, HE_LIVE_W, HE_LIVE_H, hePanel );
	HE_Text( x, y, w, "HOLSTER  " S_COLOR_WHITE "live", heOrange, he.titleFont, HE_ITEM, qfalse );
	y += 22;
	if ( HE_Button( x, y, ( w - 4 ) * 0.5f, "Full editor", qfalse, qtrue ) ) {
		HE_LiveLeave();
		return;
	}
	if ( HE_Button( x + ( w + 4 ) * 0.5f, y, ( w - 4 ) * 0.5f, "Close", qfalse, qtrue ) ) {
		HE_Close();
		return;
	}
	y += HE_CTRL_H + 10;
	if ( !he.loaded ) {
		HE_Text( x, y, w, "Asking C1B3R...", heDim, he.font, HE_SMALL, qfalse );
		y += 14;
	} else if ( he.sel < 0 || he.sel >= he.num ) {
		HE_Text( x, y, w, "Nothing worn yet: add it in the full editor", heDim, he.font, HE_SMALL, qfalse );
	} else {
		hsItem_t *it = &he.items[he.sel];

		// which one: < name >
		if ( HE_Button( x, y, 18, "<", qfalse, (qboolean)( he.num > 1 ) ) )
			he.sel = ( he.sel - 1 + he.num ) % he.num;
		HE_Box( x + 20, y, w - 40, HE_CTRL_H, heField );
		HE_Text( x + 22, y + 2, w - 44, va( "%i/%i  %s", he.sel + 1, he.num, COM_SkipPath( it->model ) ), heWhite, he.font, HE_SMALL, qtrue );
		if ( HE_Button( x + w - 18, y, 18, ">", qfalse, (qboolean)( he.num > 1 ) ) )
			he.sel = ( he.sel + 1 ) % he.num;
		y += HE_CTRL_H + 8;
		y = HE_DrawWornOn( x, y, w );
		HE_Text( x, y, w, it->all ? "Worn with every preset" : it->numPresets ? va( "Worn with %i preset%s", it->numPresets, it->numPresets == 1 ? "" : "s" )
			: "Not worn: pick its presets (full editor)", it->all || it->numPresets ? heDim : heYellow, he.font, 0.55f, qfalse );
		y += 12;
		for ( int s = 0; s < HS_NUM_SLIDERS; s++, y += 18 ) {
			if ( s == HS_PITCH || s == HS_SIZE )
				y += 4;
			HE_Slider( x, y, w, s, &he.items[he.sel] );
		}
		y += 4;
		if ( HE_Button( x, y, w, "Centre it on the tag", qfalse, qtrue ) ) {
			VectorClear( it->off );
			VectorClear( it->ang );
			it->scale = 1.0f;
			he.changed = qtrue;
		}
		y += HE_CTRL_H + 6;
	}
	// the gizmo's mode, its axes, the camera
	{
		const float bw = ( w - 8 ) / 3;

		if ( HE_Button( x, y, bw, he.gzMode == HE_MOVE ? "Move" : "Turn", qfalse, qtrue ) ) {
			HE_DropDrag();
			he.gzMode = he.gzMode == HE_MOVE ? HE_TURN : HE_MOVE;
		}
		if ( HE_Button( x + bw + 4, y, bw, he.gzWorldAxes ? "World axes" : "Tag axes", he.gzWorldAxes, qtrue ) ) {
			HE_DropDrag();
			he.gzWorldAxes = (qboolean)!he.gzWorldAxes;
		}
		if ( HE_Button( x + 2 * ( bw + 4 ), y, bw, he.freeCam ? "Free cam" : "Orbit cam", he.freeCam, qtrue ) )
			HE_FreeCam( (qboolean)!he.freeCam );
		y += HE_CTRL_H + 6;
	}
	HE_Text( x, y, w, he.gzHidden ? "Gizmo hidden: J shows it" : "Drag an arrow to move, a ring to turn", heDim, he.font, 0.55f, qfalse );
	HE_Text( x, y + 10, w, va( "Tab move/turn, L axes, G step (%s)", HE_StepText() ), heDim, he.font, 0.55f, qfalse );
	HE_Text( x, y + 20, w, "Arrows, PgUp/PgDn a step; [ ] size", heDim, he.font, 0.55f, qfalse );
	HE_Text( x, y + 30, w, he.freeCam ? "F orbit; hold RMB to look, WASD Space C" : "F free camera; drag the world to turn,", heDim, he.font, 0.55f, qfalse );
	HE_Text( x, y + 40, w, he.freeCam ? "to fly (Shift fast, Ctrl slow)" : "the wheel to come nearer", heDim, he.font, 0.55f, qfalse );

	// the bottom: what saving said, Revert and Save
	{
		const float by = HE_LIVE_Y + HE_LIVE_H - 24, bw = ( w - 4 ) * 0.5f;

		if ( he.message[0] && cls.realtime - he.messageTime < HS_MESSAGE_MS )
			HE_Text( x, by - 26, w, he.message, he.messageBad ? heYellow : heGreen, he.font, HE_SMALL, qfalse );
		else if ( he.changed )
			HE_Text( x, by - 26, w, "Not saved yet", heYellow, he.font, HE_SMALL, qfalse );
		if ( HE_Button( x, by, bw, "Revert", qfalse, he.changed ) ) {
			he.loaded = qfalse;
			HS_Post( &hsGetHttp, "/v2/char/holster/get", "" );
		}
		if ( HE_Button( x + bw + 4, by, bw, hsSetHttp.state == HTTP_BUSY ? "Saving..." : "Save", he.changed, (qboolean)( he.changed && he.loaded ) ) )
			HE_Save();
	}

	// a drag on the world: the camera turned round you (the free camera looks with the right button)
	if ( he.click && !he.freeCam && !HE_InPanel( he.clickX, he.clickY ) ) {
		he.click = qfalse;
		he.dragging = -3;
	}
	if ( he.dragging == -3 ) {
		HE_LiveCam( 1, AngleNormalize180( Cvar_VariableValue( "cg_thirdPersonAngle" ) - ( he.cursorX - he.clickX ) * 0.6f ) );
		HE_LiveCam( 3, Com_Clamp( -60.0f, 60.0f, Cvar_VariableValue( "cg_thirdPersonVertOffset" ) + ( he.cursorY - he.clickY ) * 0.3f ) );
		he.clickX = he.cursorX;
		he.clickY = he.cursorY;
	}
}

static void HE_Close( void ) {
	if ( !he.open )
		return;
	HE_LiveLeave();
	WG_PopupClose();
	he.open = qfalse;
	he.typing = qfalse;
	he.dragging = -1;
	memset( he.held, 0, sizeof( he.held ) );
	CL_Presets_FreeCharacter();
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_HOLSTER );
}

void CL_Holster_Close( void ) {
	HE_Close();
}

void CL_Holster_Draw( void ) {
	if ( !he.open )
		return;
	{
		wgMouse_t m;

		HE_PopMouse( &m );
		WG_PopupBegin( &m );
	}
	if ( !he.mouseDown )
		he.dragging = -1;
	HE_LoadTags();
	if ( he.live && CL_Presence_Session() && !he.refused ) {
		const float dt = Com_Clamp( 0.0f, 0.1f, ( cls.realtime - he.lastFrameTime ) / 1000.0f );

		he.lastFrameTime = cls.realtime;
		if ( he.freeCam )
			HE_UpdateCamera( dt );
		HE_DrawLive();
		he.click = qfalse;
		he.wheel = 0;
		WG_DrawOverlay();
		re->SetColor( NULL );
		re->DrawStretchPic( he.cursorX, he.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
		return;
	}
	HE_LiveLeave();
	HE_Box( HE_X, HE_Y, HE_W, HE_H, hePanel );
	HE_Text( HE_A_X, HE_Y + 8, 200, "HOLSTER", heOrange, he.titleFont, HE_ITEM + 0.2f, qfalse );
	HE_Text( HE_A_X + 120, HE_Y + 12, 300, "Models worn on your character, seen by everyone with this client", heDim, he.font, HE_SMALL, qfalse );
	if ( HE_Button( HE_X + HE_W - 12 - 56, HE_Y + 10, 56, "Close", qfalse, qtrue ) ) {
		HE_Close();
		return;
	}

	if ( !CL_Presence_Session() || he.refused ) {
		HE_Text( HE_X, HE_Y + 180, HE_W, !CL_Presence_Session() ? "Not connected to C1B3R" : "Your holster needs your game linked",
			heOrange, he.titleFont, HE_ITEM + 0.1f, qtrue );
		HE_Text( HE_X, HE_Y + 206, HE_W, !CL_Presence_Session() ? "Your holster is kept there: /c1b3r says why it isn't connected" : he.refusal,
			heDim, he.font, HE_TEXT, qtrue );
	} else if ( !he.loaded ) {
		HE_Text( HE_X, HE_Y + 190, HE_W, "Asking C1B3R...", heDim, he.font, HE_TEXT, qtrue );
	} else {
		HE_DrawList();
		HE_DrawFiles();
		HE_DrawPreview();
		{
			const float by = HE_Y + HE_H - 22;

			if ( HE_Button( HE_X + HE_W - 12 - 100, by, 100, hsSetHttp.state == HTTP_BUSY ? "Saving..." : "Save", he.changed, he.changed ) )
				HE_Save();
			if ( HE_Button( HE_X + HE_W - 12 - 100 - 6 - 70, by, 70, "Revert", qfalse, he.changed ) ) {
				he.loaded = qfalse;
				HS_Post( &hsGetHttp, "/v2/char/holster/get", "" );
			}
			if ( HE_Button( HE_X + HE_W - 12 - 100 - 6 - 70 - 6 - 90, by, 90, "Live on you", qfalse, qtrue ) )
				HE_LiveEnter();
		}
	}

	if ( he.message[0] && cls.realtime - he.messageTime < HS_MESSAGE_MS )
		HE_Text( HE_A_X, HE_Y + HE_H - 20, HE_W - 310, he.message, he.messageBad ? heYellow : heGreen, he.font, HE_TEXT, qfalse );
	else if ( he.changed )
		HE_Text( HE_A_X, HE_Y + HE_H - 20, HE_W - 310, "Not saved yet: Save puts it on for everyone", heYellow, he.font, HE_TEXT, qfalse );
	he.click = qfalse;
	he.wheel = 0;
	WG_DrawOverlay();
	re->SetColor( NULL );
	re->DrawStretchPic( he.cursorX, he.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

// yours from C1B3R, and what saving said
static void HE_Frame( void ) {
	if ( HTTP_Poll( &hsGetHttp ) ) {
		if ( hsGetHttp.status == 200 && !strncmp( hsGetHttp.answer, "ok", 2 ) ) {
			he.num = HS_ReadItems( hsGetHttp.answer, he.items, HS_MAX );
			he.sel = he.num ? 0 : -1;
			he.loaded = qtrue;
			he.refused = qfalse;
			he.changed = qfalse;
		} else {
			he.refused = qtrue;
			Q_strncpyz( he.refusal, hsGetHttp.answer[0] ? hsGetHttp.answer : "C1B3R didn't answer", sizeof( he.refusal ) );
			for ( char *nl = strchr( he.refusal, '\n' ); nl; nl = NULL )
				*nl = '\0';
		}
	}
	if ( HTTP_Poll( &hsSetHttp ) ) {
		if ( hsSetHttp.status == 200 ) {
			HE_Message( "Saved: everyone with this client sees it now", qfalse );
			he.changed = qfalse;
		} else {
			HE_Message( hsSetHttp.answer[0] ? hsSetHttp.answer : "C1B3R didn't answer", qtrue );
		}
	}
	if ( he.open && !he.loaded && !he.refused && hsGetHttp.state != HTTP_BUSY && CL_Presence_Session() )
		HS_Post( &hsGetHttp, "/v2/char/holster/get", "" );
}

void CL_Holster_PollFrame( void ) {
	HE_Frame();
}

static void HE_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Holster: join a server first\n" );
		return;
	}
	if ( he.open || CL_Characters_NeedSetup( "Holster" ) )
		return;
	CL_ModelManager_Close();
	CL_NpcManager_Close();
	CL_ShaderManager_Close();
	CL_EffectManager_Close();
	CL_Storyteller_Close();
	CL_RpCinematic_EditorClose();
	CL_MapEditor_Close();
	CL_SoundManager_Close();
	CL_AnimManager_Close();
	CL_Presets_Close();
	CL_AssetManager_Close();
	CL_RadialMenu_CloseSettings();
	CL_Languages_Close();
	CL_SpecialMoves_Close();
	CL_ChatLog_Close();
	CL_Inventory_Close();
	CL_Inspect_Close();
	CL_Characters_Close();
	CL_Datapad_Close();

	he.font = re->RegisterFont( "arialnb" );
	if ( !he.font )
		he.font = cls.menuFont;
	he.titleFont = re->RegisterFont( "ergoec" );
	if ( !he.titleFont )
		he.titleFont = he.font;
	he.clickSound = S_RegisterSound( "sound/interface/button1.wav" );
	he.cursorX = SCREEN_WIDTH * 0.5f;
	he.cursorY = SCREEN_HEIGHT * 0.5f;
	he.click = he.mouseDown = qfalse;
	he.dragging = he.lastSlider = -1;
	he.loaded = he.refused = he.changed = qfalse;
	he.typing = qfalse;
	he.tagsOf[0] = '\0';
	he.yaw = 160.0f;
	HS_BuildFiles();
	he.open = qtrue;
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_HOLSTER );
}

/*
===============================================================================

Keys, the mouse, and the command

===============================================================================
*/

static qboolean HE_IsToggleKey( int key ) {
	const char *binding = Key_GetBinding( key );

	return (qboolean)( VALIDSTRING( binding ) && !Q_stricmp( binding, HS_CMD ) );
}

void CL_Holster_KeyEvent( int key, qboolean down ) {
	if ( !he.open )
		return;
	if ( key >= 0 && key < MAX_KEYS )
		he.held[key] = down;
	if ( key == A_MOUSE1 ) {
		he.mouseDown = down;
		if ( down ) {
			he.click = qtrue;
			he.clickX = he.cursorX;
			he.clickY = he.cursorY;
		} else {
			he.dragging = -1;
		}
		return;
	}
	if ( !down )
		return;
	// an open list or number first: the wheel scrolls it
	if ( WG_PopupOpen() && ( key == A_MWHEELUP || key == A_MWHEELDOWN ) ) {
		he.wheel += key == A_MWHEELUP ? -1 : 1;
		return;
	}
	if ( WG_PopupKey( key ) )
		return;
	if ( he.typing ) {
		if ( key == A_ENTER || key == A_KP_ENTER || key == A_TAB ) {
			he.typing = qfalse;
			WG_EditReset();
		} else if ( WG_EditKey( he.search, sizeof( he.search ), key, NULL ) == WG_EDIT_CHANGED ) {
			he.fileScroll = 0;
		}
		return;
	}
	if ( HE_IsToggleKey( key ) ) {
		HE_Close();
		return;
	}
	if ( HE_Live() && HE_LiveKey( key ) )
		return;
	switch ( key ) {
	case A_MWHEELUP:
	case A_MWHEELDOWN:
		if ( he.live ) {
			HE_LiveCam( 2, Com_Clamp( 25.0f, 200.0f, Cvar_VariableValue( "cg_thirdPersonRange" ) + ( key == A_MWHEELUP ? -8 : 8 ) ) );
			break;
		}
		if ( key == A_MWHEELUP )
			he.fileScroll = Q_max( 0, he.fileScroll - 3 );
		else
			he.fileScroll += 3;		// held in range as it's drawn
		break;
	case A_CURSOR_LEFT:
	case A_CURSOR_RIGHT:
		// the last slider touched, a step
		if ( he.lastSlider >= 0 && he.sel >= 0 && he.sel < he.num ) {
			float *v = HE_SliderValue( &he.items[he.sel], he.lastSlider );

			*v = Com_Clamp( heSliders[he.lastSlider].min, heSliders[he.lastSlider].max,
				*v + ( key == A_CURSOR_LEFT ? -1 : 1 ) * heSliders[he.lastSlider].step );
			he.changed = qtrue;
		}
		break;
	case A_CURSOR_UP:
	case A_CURSOR_DOWN:
		if ( he.num )
			he.sel = Com_Clampi( 0, he.num - 1, he.sel + ( key == A_CURSOR_UP ? -1 : 1 ) );
		break;
	default:
		break;
	}
}

void CL_Holster_CharEvent( int ch ) {
	if ( he.open && WG_PopupChar( ch ) )
		return;
	if ( he.open && he.typing && WG_EditChar( he.search, sizeof( he.search ), ch, NULL ) )
		he.fileScroll = 0;
}

void CL_Holster_MouseEvent( int dx, int dy ) {
	// the free camera looks while the right button's held, the cursor stays
	if ( HE_Live() && he.freeCam && he.held[A_MOUSE2] ) {
		he.camAng[YAW] -= dx * cl_sensitivity->value * m_yaw->value;
		he.camAng[PITCH] = Com_Clamp( -89.0f, 89.0f, he.camAng[PITCH] + dy * cl_sensitivity->value * m_pitch->value );
		return;
	}
	he.cursorX = Com_Clamp( 0, SCREEN_WIDTH, he.cursorX + dx );
	he.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, he.cursorY + dy );
}

// the search typed in first, then closed (what's not saved stays, till it's opened again)
void CL_Holster_Escape( void ) {
	if ( WG_PopupOpen() ) {
		WG_PopupClose();
		return;
	}
	if ( he.typing ) {
		he.typing = qfalse;
		WG_EditReset();
		return;
	}
	HE_Close();
}

void CL_Holster_Disconnect( void ) {
	HE_Close();
	memset( hsClients, 0, sizeof( hsClients ) );
	memset( hsPaired, 0, sizeof( hsPaired ) );
	hsNumBodies = 0;
	hsViewFor = -1;
	HS_FreeModels();
	// the next server's files may differ
	hsFiles.clear();
	hsShown.clear();
}

// the renderer's models are gone with it: dropped, not freed
void CL_Holster_RendererRestarted( void ) {
	memset( hsModels, 0, sizeof( hsModels ) );
	hsNumModels = 0;
	hsNumBodies = 0;
}

static void HS_Holster_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !arg[0] ) {
		if ( he.open )
			HE_Close();
		else
			HE_Open();
	} else if ( !Q_stricmp( arg, "list" ) ) {
		for ( int c = 0; c < MAX_CLIENTS; c++ ) {
			if ( hsClients[c].state == 2 && hsClients[c].num )
				Com_Printf( "%2i %s" S_COLOR_WHITE ": %i worn%s\n", c, CL_PlayerSlotName( c ), hsClients[c].num,
					hsPaired[c] ? "" : " (not drawn now)" );
		}
	} else if ( !Q_stricmp( arg, "live" ) ) {
		if ( !he.open )
			HE_Open();
		if ( he.open )
			HE_LiveEnter();
	} else if ( !Q_stricmp( arg, "debug" ) ) {
		hsDebug = 2;
	} else {
		Com_Printf( "usage: " HS_CMD "          opens the holster editor, or closes it\n" );
		Com_Printf( "       " HS_CMD " live     the editor's small panel, what you edit worn on you in the world:\n" );
		Com_Printf( "                    arrows to move it and rings to turn it, a free camera (F)\n" );
		Com_Printf( "       " HS_CMD " list     who wears something here\n" );
		Com_Printf( "cl_holster 0|1: everyone's holster drawn, yours too\n" );
	}
}

void CL_Holster_Init( void ) {
	memset( &he, 0, sizeof( he ) );
	he.sel = he.dragging = he.lastSlider = -1;
	he.gzHot = he.gzPart = -1;
	he.step = 1;
	cl_holster = Cvar_Get( "cl_holster", "1", CVAR_ARCHIVE_ND, "Models players wear on their character (/holster), drawn on them; 0 none" );
	Cmd_AddCommand( HS_CMD, HS_Holster_f, "Models worn on your character (a hilt at the hip), seen by everyone with this client" );
}

void CL_Holster_Shutdown( void ) {
	HE_Close();		// the camera put back too
}

/*
===============================================================================

On the presets window's cards

===============================================================================
*/

// the holster of the character played: the editor's, while it's open, else the one saved
static const hsItem_t *HS_OwnItems( int *num ) {
	const hsClient_t *me = clc.clientNum >= 0 && clc.clientNum < MAX_CLIENTS ? &hsClients[clc.clientNum] : NULL;

	if ( he.open ) {
		*num = he.num;
		return he.items;
	}
	if ( !me || me->state != 2 ) {
		*num = 0;
		return NULL;
	}
	*num = me->num;
	return me->items;
}

// what it wears with this preset (its id): every preset's, and those worn with it
static int HS_ForPreset( const char *id, hsItem_t *out ) {
	int num, n = 0;
	const hsItem_t *items = HS_OwnItems( &num );

	for ( int i = 0; i < num; i++ ) {
		if ( HS_WornWith( &items[i], id ) )
			out[n++] = items[i];
	}
	return n;
}

int CL_Holster_CountForPreset( const char *id ) {
	hsItem_t worn[HS_MAX];

	return id && id[0] ? HS_ForPreset( id, worn ) : 0;
}

// a preset card's character: what its holster wears with that preset, on it (ctx: the preset's id)
void CL_Holster_DrawForPreset( void *ctx, void *ghoul2, const refEntity_t *body ) {
	hsItem_t worn[HS_MAX];
	const char *id = (const char *)ctx;

	if ( id && id[0] )
		HS_AddItems( worn, HS_ForPreset( id, worn ), ghoul2, body, 0, HS_EMOTE_NO );
}
