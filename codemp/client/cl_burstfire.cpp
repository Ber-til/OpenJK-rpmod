// cl_burstfire.cpp -- burst fire for the E-11, a client option (cl_burstFire)
//
// Held, the E-11's alt fire (right click) doesn't fire on and on: it charges a moment, sends a burst
// of a few shots (the alt fire's own spray), then cools down before the next. Only your own input is
// shaped, as special moves do it: the server fires as it always does, at its own rate, and nothing
// changes for anyone else. Shots are counted off the ammo the server says is left, so the burst
// follows RPMod's fire rate and cost, whatever they are. While it charges, the muzzle glows and the
// charge sounds as the Bryar pistol's alt fire does (your own screen: the gun's tag_flash in first
// person, the weapon's flash bolt on your body in third).
//
// An idea from a player (C1B3R report #21).

#include "client.h"

#define BF_CHARGE_MS		400			// right click held this long before the burst
#define BF_SHOTS			4			// shots in a burst
#define BF_BURST_MAX_MS		1200		// a burst ends by then whatever was counted (unlimited ammo)
#define BF_COOL_MS			1500		// then nothing until this has passed

enum { BF_IDLE, BF_CHARGING, BF_FIRING, BF_COOLING };

static struct {
	int		state;
	int		since;				// cls.realtime the state began
	int		shots;
	int		ammo;				// the blaster ammo when last looked at
} bf;

static cvar_t *cl_burstFire;

#define BF_GLOW_SHADER		"gfx/effects/bryarFrontFlash"	// the Bryar's charge glow
#define BF_CHARGE_SOUND		"sound/weapons/bryar/altcharge.wav"

// the muzzle this frame, as the gun's drawn
static struct {
	qboolean	firstPerson, body;
	vec3_t		fpOrg, bodyOrg;
	float		bodyDist;
} bfMuzzle;

static const float bfCharge[4] = { 1.0f, 0.62f, 0.12f, 0.9f };
static const float bfCool[4] = { 0.65f, 0.65f, 0.7f, 0.75f };
static const float bfBack[4] = { 0.0f, 0.0f, 0.0f, 0.55f };

static void BF_Set( int state ) {
	bf.state = state;
	bf.since = cls.realtime;
	if ( state == BF_CHARGING ) {
		const sfxHandle_t sfx = S_RegisterSound( BF_CHARGE_SOUND );

		if ( sfx )
			S_StartLocalSound( sfx, CHAN_WEAPON );
	}
	if ( state == BF_FIRING ) {
		bf.shots = 0;
		bf.ammo = cl.snap.ps.ammo[AMMO_BLASTER];
	}
}

// whether a burst may run now: the E-11 in hand, playing, nothing else holding the keys
static qboolean BF_Ready( void ) {
	const playerState_t *ps = &cl.snap.ps;

	return (qboolean)( cl_burstFire && cl_burstFire->integer && cls.state == CA_ACTIVE && cl.snap.valid && !clc.demoplaying
		&& !Key_GetCatcher() && ps->pm_type == PM_NORMAL && ps->stats[STAT_HEALTH] > 0 && ps->weapon == WP_BLASTER );
}

// each command: right click held back while charging and cooling, let through for the burst
void CL_BurstFire_FilterCmd( usercmd_t *cmd ) {
	const qboolean held = (qboolean)( ( cmd->buttons & BUTTON_ALT_ATTACK ) != 0 );

	if ( !BF_Ready() ) {
		bf.state = BF_IDLE;
		return;
	}
	switch ( bf.state ) {
	case BF_IDLE:
		if ( held )
			BF_Set( BF_CHARGING );
		break;
	case BF_CHARGING:
		if ( !held )
			bf.state = BF_IDLE;			// let go before it charged: nothing fired
		else if ( cls.realtime - bf.since >= BF_CHARGE_MS )
			BF_Set( BF_FIRING );
		break;
	case BF_FIRING: {
		const int ammo = cl.snap.ps.ammo[AMMO_BLASTER];

		if ( ammo < bf.ammo )
			bf.shots++;
		bf.ammo = ammo;
		if ( bf.shots >= BF_SHOTS || cls.realtime - bf.since >= BF_BURST_MAX_MS || ammo <= 0 )
			BF_Set( BF_COOLING );
		break;
	}
	case BF_COOLING:
		if ( cls.realtime - bf.since >= BF_COOL_MS )
			BF_Set( held ? BF_CHARGING : BF_IDLE );
		break;
	}
	// out of ammo the game's own empty click goes through; otherwise only the burst fires
	if ( bf.state == BF_FIRING )
		cmd->buttons |= BUTTON_ALT_ATTACK;
	else if ( cl.snap.ps.ammo[AMMO_BLASTER] > 0 )
		cmd->buttons &= ~BUTTON_ALT_ATTACK;
}

// each model drawn while it charges: the gun in your view (its tag_flash), or your body's weapon
// (model 1 on it, bolt 0 its flash, as the game bolts it)
void CL_BurstFire_Entity( const refEntity_t *ent ) {
	if ( bf.state != BF_CHARGING || !ent || ent->reType != RT_MODEL || !BF_Ready() )
		return;
	if ( ( ent->renderfx & RF_FIRST_PERSON ) && ent->hModel && !bfMuzzle.firstPerson ) {
		orientation_t tag;

		if ( re->LerpTag( &tag, ent->hModel, ent->oldframe, ent->frame, 1.0f - ent->backlerp, "tag_flash" ) ) {
			VectorCopy( ent->origin, bfMuzzle.fpOrg );
			for ( int i = 0; i < 3; i++ )
				VectorMA( bfMuzzle.fpOrg, tag.origin[i], ent->axis[i], bfMuzzle.fpOrg );
			bfMuzzle.firstPerson = qtrue;
		}
		return;
	}
	if ( ent->ghoul2 && !( ent->renderfx & RF_FIRST_PERSON ) && !ent->customShader ) {
		CGhoul2Info_v &g2 = *(CGhoul2Info_v *)ent->ghoul2;
		CGhoul2Info_v *g2p = &g2;
		const float dist = Distance( ent->origin, cl.snap.ps.origin );
		mdxaBone_t m;
		vec3_t angles, scale;

		if ( dist > 64.0f || ( bfMuzzle.body && dist >= bfMuzzle.bodyDist ) || !re->G2API_HaveWeGhoul2Models( g2 )
			|| !re->G2API_HasGhoul2ModelOnIndex( &g2p, 1 ) )
			return;
		vectoangles( ent->axis[0], angles );
		VectorCopy( ent->modelScale, scale );
		if ( !scale[0] )
			VectorSet( scale, 1, 1, 1 );
		if ( re->G2API_GetBoltMatrix( g2, 1, 0, &m, angles, ent->origin, cl.serverTime, NULL, scale ) ) {
			VectorSet( bfMuzzle.bodyOrg, m.matrix[0][3], m.matrix[1][3], m.matrix[2][3] );
			bfMuzzle.bodyDist = dist;
			bfMuzzle.body = qtrue;
		}
	}
}

// the main view about to be drawn: the glow at the muzzle, growing with the charge, flickering as the Bryar's
void CL_BurstFire_AddToScene( const refdef_t *fd ) {
	static qhandle_t glow;
	refEntity_t sprite;
	float val;

	if ( fd->rdflags & RDF_NOWORLDMODEL )
		return;
	if ( bf.state == BF_CHARGING && BF_Ready() && ( bfMuzzle.firstPerson || bfMuzzle.body ) ) {
		if ( !glow )
			glow = re->RegisterShader( BF_GLOW_SHADER );
		val = Com_Clamp( 0.0f, 1.0f, (float)( cls.realtime - bf.since ) / BF_CHARGE_MS ) + flrand( 0.0f, 0.5f );
		memset( &sprite, 0, sizeof( sprite ) );
		sprite.reType = RT_SPRITE;
		VectorCopy( bfMuzzle.firstPerson ? bfMuzzle.fpOrg : bfMuzzle.bodyOrg, sprite.origin );
		VectorCopy( sprite.origin, sprite.oldorigin );
		sprite.radius = 3.0f * val;
		sprite.rotation = flrand( 0.0f, 360.0f );
		sprite.customShader = glow;
		sprite.shaderRGBA[0] = sprite.shaderRGBA[1] = sprite.shaderRGBA[2] = 255;
		sprite.shaderRGBA[3] = 178;
		if ( bfMuzzle.firstPerson )
			sprite.renderfx = RF_FIRST_PERSON | RF_DEPTHHACK;
		if ( glow )
			re->AddRefEntityToScene( &sprite );
	}
	memset( &bfMuzzle, 0, sizeof( bfMuzzle ) );
}

// a bar under the crosshair: filling while it charges, emptying while it cools
void CL_BurstFire_Draw( void ) {
	const float bw = 120.0f, bx = ( SCREEN_WIDTH - bw ) * 0.5f, by = SCREEN_HEIGHT * 0.5f + 36;
	float frac;
	const float *color;

	if ( bf.state != BF_CHARGING && bf.state != BF_COOLING )
		return;
	if ( !BF_Ready() )
		return;
	if ( bf.state == BF_CHARGING ) {
		frac = Com_Clamp( 0.0f, 1.0f, (float)( cls.realtime - bf.since ) / BF_CHARGE_MS );
		color = bfCharge;
	} else {
		frac = 1.0f - Com_Clamp( 0.0f, 1.0f, (float)( cls.realtime - bf.since ) / BF_COOL_MS );
		color = bfCool;
	}
	SCR_FillRect( bx, by, bw, 5, bfBack );
	SCR_FillRect( bx + 1, by + 1, ( bw - 2 ) * frac, 3, color );
}

static void BF_Command_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( !Q_stricmp( arg, "on" ) )
		Cvar_Set( "cl_burstFire", "1" );
	else if ( !Q_stricmp( arg, "off" ) )
		Cvar_Set( "cl_burstFire", "0" );
	else
		Cvar_Set( "cl_burstFire", cl_burstFire->integer ? "0" : "1" );
	Com_Printf( "Burst fire %s" S_COLOR_WHITE ": %s\n", cl_burstFire->integer ? S_COLOR_GREEN "on" : S_COLOR_YELLOW "off",
		cl_burstFire->integer ? va( "the E-11's alt fire charges %.1f s, fires %i shots, then cools %.1f s (only your own fire)",
			BF_CHARGE_MS / 1000.0f, BF_SHOTS, BF_COOL_MS / 1000.0f ) : "the E-11's alt fire as the game has it" );
}

void CL_BurstFire_Init( void ) {
	memset( &bf, 0, sizeof( bf ) );
	cl_burstFire = Cvar_Get( "cl_burstFire", "1", CVAR_ARCHIVE_ND,
		"The E-11's alt fire in bursts: a short charge, a few shots, a cool down (your own fire only)" );
	Cmd_AddCommand( "burstfire", BF_Command_f, "Burst fire for the E-11's alt fire: on, off, or switched" );
}
