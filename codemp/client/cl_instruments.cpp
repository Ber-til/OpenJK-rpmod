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

// cl_instruments.cpp -- instruments played in game: from the keyboard (/instrument) or a MIDI file,
// heard where the player stands by everyone near with the client
//
// The instruments are made here, no sound files: each note is worked out the first time it's
// played (a plucked string, a struck bar, a bell...) and kept as a sound of its own
// (S_RegisterSoundFromPCM, "*inst/<instrument>_<note>"), at most IN_CACHE of them, the least used
// given back first. A note plays on the player's body (CHAN_AUTO), IN_VOICES of them at once.
//
// What's played goes to the others through C1B3R, a batch every IN_BATCH_MS:
//   cmd:music/<client>/<seq>: <note>;<note>;...    a note: <instrument letter a-h><note>.<velocity>.<ms into the batch>
//   cmd:music/<client>/stop                         the playing over
// and each game plays a batch IN_DELAY_MS after it came, its notes at their times in it, so the rhythm
// holds whatever the network does. Your own is heard at once.

#include "client.h"
#include "qcommon/cm_public.h"
#include "cl_presence.h"
#include "snd_public.h"

#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

#define IN_RATE			44100
#define IN_MAX_SAMPLES	( IN_RATE * 3 )
#define IN_CACHE		64			// notes kept made
#define IN_VOICES		12			// notes sounding at once
#define IN_LOW			48			// C3
#define IN_HIGH			84			// C6
#define IN_BATCH_MS		120
#define IN_BATCH_NOTES	80
#define IN_DELAY_MS		250
#define IN_LATE_MS		2000
#define IN_PLAYING_MS	2500		// someone's playing until this long after their last notes
#define IN_LIT_MS		180
#define IN_PREFIX		"*inst/"
#define IN_PI			3.14159265f

typedef enum {
	IN_HARP,
	IN_LUTE,
	IN_KEYS,
	IN_FLUTE,
	IN_CHIMES,
	IN_MARIMBA,
	IN_HORN,
	IN_DRUMS,
	IN_NUM
} inInstrument_t;

typedef struct {
	const char	*word;			// its name in commands and sounds, and its model's (models/instruments/<word>)
	const char	*title;
	float		seconds;		// how long a note lasts
	float		gain;
} inDef_t;

static const inDef_t inDefs[IN_NUM] = {
	{ "harp",		"Harp",		2.2f,	0.9f },
	{ "lute",		"Lute",		1.5f,	0.9f },
	{ "keys",		"Keys",		2.0f,	0.8f },
	{ "flute",		"Flute",	1.1f,	0.7f },
	{ "chimes",		"Chimes",	2.6f,	0.7f },
	{ "marimba",	"Marimba",	0.9f,	0.9f },
	{ "horn",		"Horn",		1.0f,	0.65f },
	{ "drums",		"Drums",	1.2f,	0.9f },
};

// the drum kit, a sound to each of the twelve keys of an octave
static const char *inDrumNames[12] = { "kick", "rim", "snare", "clap", "closed hat", "low tom", "open hat", "mid tom", "shaker", "high tom", "cowbell", "crash" };

typedef struct {
	int			instrument, note;
	sfxHandle_t	sfx;
	int			used;
} inCached_t;

typedef struct {
	int			at;				// cls.realtime it plays
	int			client;
	int			instrument, note, velocity;
} inScheduled_t;

typedef struct {
	int			ms;				// into the song
	int			instrument, note, velocity;
} inMidiNote_t;

static struct {
	qboolean	open;
	int			instrument;
	int			octave;			// the keys' octave, -1 to +1 from C3
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;
	qhandle_t	font, titleFont;
	sfxHandle_t	clickSound;
	qboolean	held[MAX_KEYS];
	int			lit[128];		// a key of the keyboard lit until then

	// what we play, gathered to send
	std::string	batch;
	int			batchStart, batchNotes, seq;
	qboolean	sending;		// a performance going: its stop is due when it ends

	// a MIDI file: its name, and where it is on disk ("" in a pk3)
	std::vector<std::string> files;
	std::vector<std::string> filePaths;
	int			file;
	std::vector<inMidiNote_t> song;
	char		songName[MAX_QPATH];
	qboolean	songPlaying;
	int			songStart, songNext, songLength;
	qboolean	followPrograms;

	// others'
	std::vector<inScheduled_t> due;
	int			playingUntil[MAX_CLIENTS];
	int			playingInstrument[MAX_CLIENTS];
	int			emoteAt;					// ours (cl_instrumentEmote): when it was last done

	// the view the world was last drawn from, for the notes over the players' heads
	qboolean	viewKnown;
	vec3_t		viewOrg, viewAxis[3];
	float		tanX, tanY;
	float		viewX, viewY, viewW, viewH;		// on the 640x480 screen

	char		message[160];
	int			messageTime;
} in;

static inCached_t inCache[IN_CACHE];
static short inPcm[IN_MAX_SAMPLES];
static float inWork[IN_MAX_SAMPLES];
static cvar_t *s_volumeInstruments, *cl_instrumentDebug, *cl_instrumentEmote, *cl_instrumentEmoteRepeat, *cl_instrumentNote;

static void IN_Message( const char *text ) {
	Q_strncpyz( in.message, text, sizeof( in.message ) );
	in.messageTime = cls.realtime;
}

/*
===============================================================================

The instruments: a note made

===============================================================================
*/

static unsigned int inSeed = 12345;

static float IN_Noise( void ) {
	inSeed = inSeed * 1664525u + 1013904223u;
	return ( ( inSeed >> 8 ) & 0xffff ) / 32768.0f - 1.0f;
}

static float IN_Freq( int note ) {
	return 440.0f * powf( 2.0f, ( note - 69 ) / 12.0f );
}

// a plucked string (Karplus-Strong): a burst of noise going round a delay as long as one period,
// softened each time. bright 0 a harp's soft pluck, 1 a lute's sharp one
static void IN_Pluck( float *out, int n, float f, float bright, float decay ) {
	const int period = Q_max( 2, (int)( IN_RATE / f + 0.5f ) );
	std::vector<float> line( period );
	float last = 0.0f;

	for ( int i = 0; i < period; i++ ) {
		// the pluck: noise, smoothed for a softer string
		const float x = IN_Noise();

		last = last + ( x - last ) * ( 0.25f + 0.7f * bright );
		line[i] = last;
	}
	for ( int i = 0, p = 0; i < n; i++ ) {
		const int q = ( p + 1 ) % period;
		const float y = line[p];

		out[i] = y;
		line[p] = ( y + line[q] ) * 0.5f * decay;
		p = q;
	}
}

static void IN_Render( int instrument, int note, float *out, int n ) {
	const float f = IN_Freq( note );
	const float dt = 1.0f / IN_RATE;

	memset( out, 0, n * sizeof( float ) );
	inSeed = 1000u + note * 31u + instrument * 977u;
	switch ( instrument ) {
	case IN_HARP:
		IN_Pluck( out, n, f, 0.1f, 0.9985f );
		break;

	case IN_LUTE: {
		// a sharper pluck, and the body's wooden ring under it
		IN_Pluck( out, n, f, 0.85f, 0.996f );
		float lp = 0.0f, bp = 0.0f;
		const float g = 2.0f * sinf( IN_PI * 220.0f * dt );

		for ( int i = 0; i < n; i++ ) {
			const float hp = out[i] - lp - 0.5f * bp;

			bp += g * hp;
			lp += g * bp;
			out[i] = out[i] * 0.8f + bp * 0.25f;
		}
		break;
	}

	case IN_KEYS:
		// a struck string's harmonics, the higher dying sooner, a little out of tune as a piano's
		for ( int h = 1; h <= 8; h++ ) {
			const float fh = f * h * sqrtf( 1.0f + 0.0004f * h * h );
			const float amp = 1.0f / powf( (float)h, 1.4f );
			const float k = 1.6f + 0.9f * h;

			if ( fh > IN_RATE * 0.45f )
				break;
			for ( int i = 0; i < n; i++ ) {
				const float t = i * dt;

				out[i] += amp * sinf( 2.0f * IN_PI * fh * t ) * expf( -k * t );
			}
		}
		break;

	case IN_FLUTE: {
		// breath: a soft tone with a touch of its octave, noise over the start, a slow vibrato
		float breath = 0.0f, phase = 0.0f;

		for ( int i = 0; i < n; i++ ) {
			const float t = i * dt;
			const float env = Q_min( 1.0f, t / 0.07f ) * ( t > ( n * dt - 0.25f ) ? Q_max( 0.0f, ( n * dt - t ) / 0.25f ) : 1.0f );
			const float vib = 1.0f + 0.004f * sinf( 2.0f * IN_PI * 5.0f * t ) * Q_min( 1.0f, t / 0.3f );

			phase += 2.0f * IN_PI * f * vib * dt;
			breath += ( IN_Noise() - breath ) * 0.15f;
			out[i] = env * ( sinf( phase ) + 0.2f * sinf( 2.0f * phase ) + 0.05f * sinf( 3.0f * phase )
				+ breath * ( 0.25f * expf( -t * 6.0f ) + 0.05f ) );
		}
		break;
	}

	case IN_CHIMES:
		// a bell: one tone bent by another out of step with it (FM), the bending fading
		for ( int i = 0; i < n; i++ ) {
			const float t = i * dt;
			const float index = 3.0f * expf( -t * 2.5f );

			out[i] = sinf( 2.0f * IN_PI * f * t + index * sinf( 2.0f * IN_PI * f * 3.5f * t ) ) * expf( -t * 1.6f );
		}
		break;

	case IN_MARIMBA:
		// a struck bar: its tone and its fourth partial, both short, and the mallet's knock
		for ( int i = 0; i < n; i++ ) {
			const float t = i * dt;

			out[i] = sinf( 2.0f * IN_PI * f * t ) * expf( -t * 5.0f )
				+ 0.35f * sinf( 2.0f * IN_PI * f * 3.93f * t ) * expf( -t * 14.0f )
				+ 0.3f * IN_Noise() * expf( -t * 300.0f );
		}
		break;

	case IN_HORN: {
		// brass: many harmonics, more of them as the note swells, then fading
		float phase = 0.0f;
		const float len = n * dt;

		for ( int i = 0; i < n; i++ ) {
			const float t = i * dt;
			const float env = Q_min( 1.0f, t / 0.05f ) * ( t > len - 0.2f ? Q_max( 0.0f, ( len - t ) / 0.2f ) : 1.0f );
			const float bright = 3.0f + 9.0f * Q_min( 1.0f, t / 0.12f );
			float v = 0.0f;

			phase += 2.0f * IN_PI * f * dt;
			for ( int h = 1; h <= 14 && f * h < IN_RATE * 0.45f; h++ )
				v += sinf( phase * h ) / h * ( h <= bright ? 1.0f : expf( bright - h ) );
			out[i] = v * env * 0.6f;
		}
		break;
	}

	default: {
		// the drums: one sound to each key of an octave
		const int drum = ( ( note % 12 ) + 12 ) % 12;
		float lp = 0.0f, hp = 0.0f, prev = 0.0f, phase = 0.0f;

		for ( int i = 0; i < n; i++ ) {
			const float t = i * dt;
			const float x = IN_Noise();
			float v = 0.0f;

			lp += ( x - lp ) * 0.25f;
			hp = 0.95f * ( hp + x - prev );
			prev = x;
			switch ( drum ) {
			case 0:		// kick: a thump falling in pitch
				phase += 2.0f * IN_PI * ( 45.0f + 110.0f * expf( -t * 30.0f ) ) * dt;
				v = sinf( phase ) * expf( -t * 7.0f );
				break;
			case 1:		// rim
				v = ( sinf( 2.0f * IN_PI * 1700.0f * t ) * 0.6f + hp * 0.4f ) * expf( -t * 60.0f );
				break;
			case 2:		// snare: a skin's tone and the wires' hiss
				v = sinf( 2.0f * IN_PI * 185.0f * t ) * expf( -t * 20.0f ) * 0.5f + hp * expf( -t * 12.0f ) * 0.7f;
				break;
			case 3:		// clap: three bursts
				v = hp * ( expf( -t * 80.0f ) + expf( -fabsf( t - 0.012f ) * 80.0f ) * 0.8f + ( t > 0.024f ? expf( -( t - 0.024f ) * 18.0f ) : 0.0f ) ) * 0.6f;
				break;
			case 4:		// closed hat
				v = hp * expf( -t * 45.0f ) * 0.5f;
				break;
			case 6:		// open hat
				v = hp * expf( -t * 6.0f ) * 0.45f;
				break;
			case 8:		// shaker
				v = hp * sinf( IN_PI * Q_min( 1.0f, t / 0.12f ) ) * ( t < 0.12f ? 1.0f : 0.0f ) * 0.4f;
				break;
			case 10:	// cowbell: two tones, not in tune
				v = ( ( sinf( 2.0f * IN_PI * 562.0f * t ) > 0.0f ? 1.0f : -1.0f ) + ( sinf( 2.0f * IN_PI * 845.0f * t ) > 0.0f ? 1.0f : -1.0f ) ) * 0.2f * expf( -t * 9.0f );
				break;
			case 11:	// crash
				v = ( hp * 0.6f + lp * 0.2f ) * expf( -t * 2.2f ) * 0.6f;
				break;
			default: {	// toms: low, mid, high
				const float base = drum == 5 ? 95.0f : drum == 7 ? 135.0f : 185.0f;

				phase += 2.0f * IN_PI * base * ( 1.0f + 0.4f * expf( -t * 25.0f ) ) * dt;
				v = sinf( phase ) * expf( -t * 8.0f ) + lp * 0.1f * expf( -t * 30.0f );
				break;
			}
			}
			out[i] = v;
		}
		break;
	}
	}
	// a short fade in and out, so nothing clicks
	for ( int i = 0; i < 64 && i < n; i++ ) {
		out[i] *= i / 64.0f;
		out[n - 1 - i] *= i / 64.0f;
	}
}

static const char *IN_SoundName( int instrument, int note ) {
	return va( IN_PREFIX "%s_%i", inDefs[instrument].word, note );
}

static sfxHandle_t IN_Note( int instrument, int note ) {
	const int n = Com_Clampi( IN_RATE / 4, IN_MAX_SAMPLES, (int)( inDefs[instrument].seconds * IN_RATE ) );
	inCached_t *slot = NULL, *oldest = NULL;
	float peak = 0.0f, scale;

	for ( int i = 0; i < IN_CACHE; i++ ) {
		inCached_t *c = &inCache[i];

		if ( c->sfx && c->instrument == instrument && c->note == note ) {
			c->used = cls.realtime;
			if ( S_SoundReady( c->sfx, IN_SoundName( instrument, note ) ) )
				return c->sfx;
			slot = c;		// given back (a new level): made again
			break;
		}
		if ( !c->sfx && !slot )
			slot = c;
		if ( c->sfx && ( !oldest || c->used < oldest->used ) )
			oldest = c;
	}
	if ( !slot ) {
		slot = oldest;
		if ( S_SoundReady( slot->sfx, IN_SoundName( slot->instrument, slot->note ) ) )
			S_FreeSound( slot->sfx );
	}
	IN_Render( instrument, note, inWork, n );
	for ( int i = 0; i < n; i++ )
		peak = Q_max( peak, fabsf( inWork[i] ) );
	scale = peak > 0.0f ? 30000.0f * inDefs[instrument].gain / peak : 0.0f;
	for ( int i = 0; i < n; i++ )
		inPcm[i] = (short)( inWork[i] * scale );
	slot->instrument = instrument;
	slot->note = note;
	slot->used = cls.realtime;
	slot->sfx = S_RegisterSoundFromPCM( IN_SoundName( instrument, note ), inPcm, n, IN_RATE );
	return slot->sfx;
}

// a note sounded on that player (them as they stand now), or on us
static void IN_Sound( int client, int instrument, int note, int velocity ) {
	sfxHandle_t sfx;
	const float vol = Com_Clamp( 0.0f, 1.0f, velocity / 127.0f ) * Com_Clamp( 0.0f, 1.0f, s_volumeInstruments->value );

	if ( cls.state != CA_ACTIVE || vol <= 0.0f || instrument < 0 || instrument >= IN_NUM )
		return;
	note = Com_Clampi( 0, 127, note );
	sfx = IN_Note( instrument, note );
	if ( !sfx )
		return;
	while ( S_CountPlaying( IN_PREFIX ) >= IN_VOICES )
		S_StopOldestPlaying( IN_PREFIX );
	if ( client == cl.snap.ps.clientNum ) {
		S_StartSoundVol( NULL, client, CHAN_AUTO, sfx, vol );
		return;
	}
	for ( int i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];

		if ( es->number == client ) {
			S_StartSoundVol( es->pos.trBase, client, CHAN_AUTO, sfx, vol );
			return;
		}
	}
	// out of sight: too far to hear
}

/*
===============================================================================

What we play, sent

===============================================================================
*/

static void IN_Flush( void ) {
	if ( in.batch.empty() )
		return;
	if ( CL_Presence_Session() )
		CL_Presence_Hook( -1, va( "cmd:music/%i/%i: %s", cl.snap.ps.clientNum, ++in.seq, in.batch.c_str() ) );
	in.batch.clear();
	in.batchNotes = 0;
}

static void IN_StopSending( void ) {
	IN_Flush();
	if ( in.sending && CL_Presence_Session() )
		CL_Presence_Hook( -1, va( "cmd:music/%i/stop", cl.snap.ps.clientNum ) );
	in.sending = qfalse;
}

// a note we play: heard now, and gathered for the others. at: when it's played, on the clock the
// batch keeps (cls.realtime, or the song's)
static void IN_Play( int instrument, int note, int velocity, int at ) {
	IN_Sound( cl.snap.ps.clientNum, instrument, note, velocity );
	if ( note >= 0 && note < 128 )
		in.lit[note] = cls.realtime + IN_LIT_MS;
	if ( in.batch.empty() )
		in.batchStart = at;
	in.batch += va( "%s%c%i.%i.%i", in.batch.empty() ? "" : ";", 'a' + instrument, note, velocity, Q_max( 0, at - in.batchStart ) );
	in.sending = qtrue;
	if ( ++in.batchNotes >= IN_BATCH_NOTES )
		IN_Flush();
}

/*
===============================================================================

Others' playing, heard

===============================================================================
*/

// cmd:music/<client>/<seq>: <notes>, or cmd:music/<client>/stop
void CL_Instruments_Hook( char **argv, int argc, const char *message ) {
	int client;

	if ( argc < 2 )
		return;
	client = atoi( argv[0] );
	if ( client < 0 || client >= MAX_CLIENTS || client == cl.snap.ps.clientNum || CL_Presence_Replaying() )
		return;
	if ( !Q_stricmp( argv[1], "stop" ) ) {
		in.playingUntil[client] = 0;
		return;
	}
	for ( const char *p = message; *p; ) {
		inScheduled_t s;
		int ms;

		while ( *p == ';' || *p == ' ' )
			p++;
		if ( *p < 'a' || *p >= 'a' + IN_NUM )
			break;
		s.instrument = *p++ - 'a';
		s.note = atoi( p );
		p = strchr( p, '.' );
		if ( !p )
			break;
		s.velocity = atoi( ++p );
		p = strchr( p, '.' );
		if ( !p )
			break;
		ms = Com_Clampi( 0, IN_LATE_MS, atoi( ++p ) );
		s.client = client;
		s.at = cls.realtime + IN_DELAY_MS + ms;
		if ( in.due.size() < 2048 )
			in.due.push_back( s );
		in.playingInstrument[client] = s.instrument;
		while ( *p && *p != ';' )
			p++;
	}
	in.playingUntil[client] = cls.realtime + IN_PLAYING_MS;
	if ( cl_instrumentDebug->integer )
		Com_Printf( "Instruments: %s plays (%s)\n", cl.gameState.stringOffsets[CS_PLAYERS + client]
			? Info_ValueForKey( cl.gameState.stringData + cl.gameState.stringOffsets[CS_PLAYERS + client], "n" ) : "?", message );
}

// someone with the client playing now, and what (the look: cl_holster.cpp)
int CL_Instruments_Playing( int client ) {
	if ( client < 0 || client >= MAX_CLIENTS )
		return -1;
	if ( client == cl.snap.ps.clientNum )
		return in.sending && ( in.songPlaying || cls.realtime - in.batchStart < IN_PLAYING_MS || in.open ) ? in.instrument : -1;
	return in.playingUntil[client] > cls.realtime ? in.playingInstrument[client] : -1;
}

/*
===============================================================================

MIDI files

===============================================================================
*/

// the nearest of ours to a General MIDI instrument
static int IN_FromProgram( int program ) {
	if ( program < 8 ) return IN_KEYS;			// pianos
	if ( program < 12 ) return IN_CHIMES;		// celesta, glockenspiel, music box, vibraphone
	if ( program < 16 ) return IN_MARIMBA;		// marimba, xylophone, bells, dulcimer
	if ( program < 24 ) return IN_KEYS;			// organs
	if ( program < 40 ) return IN_LUTE;			// guitars, basses
	if ( program < 56 ) return IN_HARP;			// strings, harp, ensembles
	if ( program < 64 ) return IN_HORN;			// brass
	if ( program < 80 ) return IN_FLUTE;		// reeds, pipes
	if ( program < 104 ) return IN_KEYS;		// synths
	if ( program < 112 ) return IN_LUTE;		// sitar, banjo, shamisen, koto...
	if ( program < 120 ) return IN_MARIMBA;		// percussive
	return IN_CHIMES;
}

// a General MIDI drum to one of our kit's keys (C4 up)
static int IN_FromDrum( int note ) {
	switch ( note ) {
	case 35: case 36:					return 60 + 0;		// kick
	case 37:							return 60 + 1;		// rim
	case 38: case 40:					return 60 + 2;		// snare
	case 39:							return 60 + 3;		// clap
	case 42: case 44:					return 60 + 4;		// closed hat
	case 41: case 43: case 45:			return 60 + 5;		// low tom
	case 46:							return 60 + 6;		// open hat
	case 47: case 48:					return 60 + 7;		// mid tom
	case 69: case 70: case 82:			return 60 + 8;		// shaker
	case 50:							return 60 + 9;		// high tom
	case 56:							return 60 + 10;		// cowbell
	case 49: case 52: case 55: case 57:	return 60 + 11;		// crash
	default:							return 60 + 4;
	}
}

static unsigned int IN_VarLen( const byte *data, int len, int *p ) {
	unsigned int v = 0;

	for ( int i = 0; i < 4 && *p < len; i++ ) {
		const byte b = data[( *p )++];

		v = ( v << 7 ) | ( b & 0x7f );
		if ( !( b & 0x80 ) )
			break;
	}
	return v;
}

typedef struct {
	unsigned int	tick;
	int				tempo;		// microseconds a quarter, or -1 for a note
	int				channel, note, velocity, program;
} inEvent_t;

// a Standard MIDI File (format 0 or 1) read into notes in time; why not, or NULL
static const char *IN_ReadMidi( const byte *data, int len ) {
	std::vector<inEvent_t> events;
	int division, tracks, p = 14;
	int programs[16] = { 0 };

	in.song.clear();
	if ( len < 14 || memcmp( data, "MThd", 4 ) )
		return "not a MIDI file";
	tracks = ( data[10] << 8 ) | data[11];
	division = ( data[12] << 8 ) | data[13];
	if ( division & 0x8000 )
		return "its time is in SMPTE frames, which isn't read";
	if ( division <= 0 )
		return "it has no time division";
	for ( int t = 0; t < tracks && p + 8 <= len; t++ ) {
		const int size = ( data[p + 4] << 24 ) | ( data[p + 5] << 16 ) | ( data[p + 6] << 8 ) | data[p + 7];
		const int end = Q_min( len, p + 8 + size );
		unsigned int tick = 0;
		int status = 0;

		if ( memcmp( data + p, "MTrk", 4 ) ) {
			p = end;
			continue;
		}
		p += 8;
		while ( p < end ) {
			int b;

			tick += IN_VarLen( data, end, &p );
			if ( p >= end )
				break;
			b = data[p];
			if ( b == 0xff ) {				// meta: the tempo kept, the rest skipped
				const int type = p + 1 < end ? data[p + 1] : 0;
				int l;

				p += 2;
				l = (int)IN_VarLen( data, end, &p );
				if ( type == 0x51 && l == 3 && p + 3 <= end ) {
					inEvent_t e = { tick, ( data[p] << 16 ) | ( data[p + 1] << 8 ) | data[p + 2], 0, 0, 0, 0 };

					events.push_back( e );
				}
				p += l;
				continue;
			}
			if ( b == 0xf0 || b == 0xf7 ) {	// system exclusive
				int l;

				p++;
				l = (int)IN_VarLen( data, end, &p );
				p += l;
				continue;
			}
			if ( b & 0x80 ) {
				status = b;
				p++;
			}
			if ( !status )
				break;
			switch ( status & 0xf0 ) {
			case 0x90:
			case 0x80:
				if ( p + 2 > end )
					break;
				if ( ( status & 0xf0 ) == 0x90 && data[p + 1] > 0 ) {
					inEvent_t e = { tick, -1, status & 0x0f, data[p], data[p + 1], programs[status & 0x0f] };

					events.push_back( e );
				}
				p += 2;
				break;
			case 0xc0:
				if ( p < end )
					programs[status & 0x0f] = data[p];
				p += 1;
				break;
			case 0xd0:
				p += 1;
				break;
			default:	// aftertouch, controllers, pitch bend
				p += 2;
				break;
			}
		}
		p = end;
	}
	// in time: tempo changes first at a tick, then ticks turned into ms through them
	std::stable_sort( events.begin(), events.end(), []( const inEvent_t &a, const inEvent_t &b ) {
		return a.tick < b.tick || ( a.tick == b.tick && a.tempo >= 0 && b.tempo < 0 );
	} );
	{
		double ms = 0.0, msPerTick = 500.0 / division;
		unsigned int last = 0;

		for ( size_t i = 0; i < events.size(); i++ ) {
			const inEvent_t &e = events[i];

			ms += ( e.tick - last ) * msPerTick;
			last = e.tick;
			if ( e.tempo >= 0 ) {
				msPerTick = e.tempo / 1000.0 / division;
				continue;
			}
			inMidiNote_t n;

			n.ms = (int)ms;
			n.velocity = e.velocity;
			if ( e.channel == 9 ) {
				n.instrument = IN_DRUMS;
				n.note = IN_FromDrum( e.note );
			} else {
				n.instrument = in.followPrograms ? IN_FromProgram( e.program ) : -1;
				n.note = e.note;
				while ( n.note < IN_LOW )
					n.note += 12;
				while ( n.note > IN_HIGH )
					n.note -= 12;
			}
			in.song.push_back( n );
		}
	}
	if ( in.song.empty() )
		return "it has no notes";
	in.songLength = in.song.back().ms;
	return NULL;
}

// one more to the list, unless it has it (the same name in another folder)
static void IN_AddMidi( const char *name, const char *osPath ) {
	for ( size_t i = 0; i < in.files.size(); i++ ) {
		if ( !Q_stricmp( in.files[i].c_str(), name ) )
			return;
	}
	in.files.push_back( name );
	in.filePaths.push_back( osPath );
}

// the midi folders of the game's folders, read from the disk itself: a long name (the game's paths
// stop at 64 letters) and a pure server (which hides loose files) don't keep them out; then the
// ones in pk3s
static void IN_ListMidi( void ) {
	const char *game = Cvar_VariableString( "fs_game" );
	const char *roots[] = { Cvar_VariableString( "fs_homepath" ), Cvar_VariableString( "fs_basepath" ) };
	const char *games[] = { game[0] ? game : BASEGAME, BASEGAME };
	std::vector<std::pair<std::string, std::string>> found;
	int count;
	char **list;

	in.files.clear();
	in.filePaths.clear();
	for ( int r = 0; r < 2; r++ ) {
		for ( int g = 0; g < 2; g++ ) {
			char dir[MAX_OSPATH];
			const char *exts[] = { ".mid", ".midi" };

			if ( !roots[r][0] || ( g == 1 && !Q_stricmp( games[0], games[1] ) ) )
				continue;
			Com_sprintf( dir, sizeof( dir ), "%s/%s/midi", roots[r], games[g] );
			for ( int e = 0; e < 2; e++ ) {
				list = Sys_ListFiles( dir, exts[e], NULL, &count, qfalse );
				for ( int i = 0; list && i < count; i++ )
					found.push_back( std::make_pair( std::string( list[i] ), std::string( va( "%s/%s", dir, list[i] ) ) ) );
				if ( list )
					Sys_FreeFileList( list );
			}
		}
	}
	list = FS_ListFiles( "midi", ".mid", &count );
	for ( int i = 0; list && i < count; i++ )
		found.push_back( std::make_pair( std::string( list[i] ), std::string() ) );
	if ( list )
		FS_FreeFileList( list );
	std::sort( found.begin(), found.end(), []( const std::pair<std::string, std::string> &a, const std::pair<std::string, std::string> &b ) {
		return Q_stricmp( a.first.c_str(), b.first.c_str() ) < 0;
	} );
	for ( size_t i = 0; i < found.size(); i++ )
		IN_AddMidi( found[i].first.c_str(), found[i].second.c_str() );
	in.file = Com_Clampi( 0, Q_max( 0, (int)in.files.size() - 1 ), in.file );
}

// a file read from the disk, whole; its length, 0 if it can't be (*data freed with Z_Free)
static int IN_ReadDisk( const char *osPath, byte **data ) {
	FILE *f = fopen( osPath, "rb" );
	long len;

	*data = NULL;
	if ( !f )
		return 0;
	fseek( f, 0, SEEK_END );
	len = ftell( f );
	fseek( f, 0, SEEK_SET );
	if ( len <= 0 || len > 8 * 1024 * 1024 ) {
		fclose( f );
		return 0;
	}
	*data = (byte *)Z_Malloc( (int)len, TAG_TEMP_WORKSPACE, qfalse );
	if ( fread( *data, 1, (size_t)len, f ) != (size_t)len ) {
		fclose( f );
		Z_Free( *data );
		*data = NULL;
		return 0;
	}
	fclose( f );
	return (int)len;
}

static void IN_StopSong( void ) {
	if ( !in.songPlaying )
		return;
	in.songPlaying = qfalse;
	IN_StopSending();
}

static void IN_PlaySong( const char *nameIn ) {
	char name[MAX_OSPATH], path[MAX_OSPATH];
	void *data = NULL;
	byte *disk = NULL;
	int len = 0;
	const char *why;

	IN_StopSong();
	Q_strncpyz( name, nameIn, sizeof( name ) );
	// one of the list's, by its name, with or without .mid
	if ( in.files.empty() )
		IN_ListMidi();
	for ( size_t i = 0; i < in.files.size(); i++ ) {
		const std::string &f = in.files[i];

		if ( !Q_stricmp( f.c_str(), name ) || !Q_stricmp( f.c_str(), va( "%s.mid", name ) ) || !Q_stricmp( f.c_str(), va( "%s.midi", name ) ) ) {
			Q_strncpyz( name, f.c_str(), sizeof( name ) );
			if ( !in.filePaths[i].empty() )
				len = IN_ReadDisk( in.filePaths[i].c_str(), &disk );
			break;
		}
	}
	if ( !disk ) {
		Com_sprintf( path, sizeof( path ), "midi/%s%s", name, Q_stristr( name, ".mid" ) ? "" : ".mid" );
		if ( strlen( path ) < MAX_QPATH )
			len = FS_ReadFile( path, &data );
	}
	if ( len <= 0 || ( !data && !disk ) ) {
		IN_Message( va( "Can't read %s (MIDI files go in the midi folder of your rpmod folder)", name ) );
		Com_Printf( "Instruments: can't read midi/%s\n", name );
		return;
	}
	why = IN_ReadMidi( disk ? disk : (const byte *)data, len );
	if ( disk )
		Z_Free( disk );
	else
		FS_FreeFile( data );
	if ( why ) {
		IN_Message( va( "%s: %s", name, why ) );
		Com_Printf( "Instruments: %s: %s\n", name, why );
		return;
	}
	Q_strncpyz( in.songName, name, sizeof( in.songName ) );
	in.songPlaying = qtrue;
	in.songStart = cls.realtime;
	in.songNext = 0;
	Com_Printf( "Instruments: playing %s, %i notes, %i:%02i\n", name, (int)in.song.size(), in.songLength / 60000, ( in.songLength / 1000 ) % 60 );
}

/*
===============================================================================

The frame

===============================================================================
*/

void CL_Instruments_Frame( void ) {
	// the song's notes due by now, at their own times in the batch
	if ( in.songPlaying ) {
		const int now = cls.realtime - in.songStart;

		while ( in.songNext < (int)in.song.size() && in.song[in.songNext].ms <= now ) {
			const inMidiNote_t &n = in.song[in.songNext++];

			if ( !in.batch.empty() && n.ms - in.batchStart >= IN_BATCH_MS )
				IN_Flush();
			IN_Play( n.instrument >= 0 ? n.instrument : in.instrument, n.note, n.velocity, n.ms );
		}
		if ( !in.batch.empty() && now - in.batchStart >= IN_BATCH_MS )
			IN_Flush();
		if ( in.songNext >= (int)in.song.size() && now > in.songLength + 500 ) {
			in.songPlaying = qfalse;
			IN_StopSending();
			IN_Message( va( "%s is over", in.songName ) );
		}
	} else if ( !in.batch.empty() && cls.realtime - in.batchStart >= IN_BATCH_MS ) {
		IN_Flush();
	}
	// played on the keys and then left a while: over
	if ( in.sending && !in.songPlaying && in.batch.empty() && cls.realtime - in.batchStart > IN_PLAYING_MS && !in.open )
		IN_StopSending();

	// others' notes, at their time
	for ( size_t i = 0; i < in.due.size(); ) {
		if ( in.due[i].at <= cls.realtime ) {
			if ( cls.realtime - in.due[i].at < IN_LATE_MS )
				IN_Sound( in.due[i].client, in.due[i].instrument, in.due[i].note, in.due[i].velocity );
			in.due.erase( in.due.begin() + i );
		} else {
			i++;
		}
	}
	// our emote as we play (cl_instrumentEmote): as we start, and again as often as said
	if ( cls.state == CA_ACTIVE ) {
		const int me = cl.snap.ps.clientNum;

		if ( cl_instrumentEmote->string[0] && CL_Instruments_Playing( me ) >= 0 ) {
			if ( !in.emoteAt || ( cl_instrumentEmoteRepeat->value > 0.0f
				&& cls.realtime - in.emoteAt >= (int)( Q_max( 2.0f, cl_instrumentEmoteRepeat->value ) * 1000.0f ) ) ) {
				in.emoteAt = cls.realtime;
				Cbuf_AddText( va( "%s\n", cl_instrumentEmote->string[0] == '/' || cl_instrumentEmote->string[0] == '\\'
					? cl_instrumentEmote->string + 1 : cl_instrumentEmote->string ) );
			}
		} else {
			in.emoteAt = 0;
		}
	}
	// keys let go
	for ( int k = 0; k < MAX_KEYS; k++ ) {
		if ( in.held[k] && !Key_IsDown( k ) )
			in.held[k] = qfalse;
	}
}

// the model held playing it: models/instruments/<word>.glm or .md3, "" none (none come with the game)
const char *CL_Instruments_Model( int instrument ) {
	static char found[IN_NUM][MAX_QPATH];
	static qboolean looked[IN_NUM];

	if ( instrument < 0 || instrument >= IN_NUM )
		return "";
	if ( !looked[instrument] ) {
		looked[instrument] = qtrue;
		if ( FS_FileExists( va( "models/instruments/%s.glm", inDefs[instrument].word ) ) )
			Com_sprintf( found[instrument], MAX_QPATH, "models/instruments/%s.glm", inDefs[instrument].word );
		else if ( FS_FileExists( va( "models/instruments/%s.md3", inDefs[instrument].word ) ) )
			Com_sprintf( found[instrument], MAX_QPATH, "models/instruments/%s.md3", inDefs[instrument].word );
	}
	return found[instrument];
}

void CL_Instruments_Disconnect( void ) {
	in.viewKnown = qfalse;
	in.songPlaying = in.sending = qfalse;
	in.batch.clear();
	in.due.clear();
	memset( in.playingUntil, 0, sizeof( in.playingUntil ) );
}

/*
===============================================================================

A note over the head of whoever plays, where the chat balloon would be

===============================================================================
*/

static void IN_Fill( float x, float y, float w, float h, const float *color );

#define IN_NOTE_RANGE	1500.0f
#define IN_NOTE_HEIGHT	44.0f		// above a player's origin: just over the head

// the view the world is drawn from this frame (from the cgame's scene)
void CL_Instruments_View( const refdef_t *fd ) {
	if ( !fd || ( fd->rdflags & RDF_NOWORLDMODEL ) || fd->width <= 0 || cls.glconfig.vidWidth <= 0 )
		return;
	VectorCopy( fd->vieworg, in.viewOrg );
	for ( int k = 0; k < 3; k++ )
		VectorCopy( fd->viewaxis[k], in.viewAxis[k] );
	in.tanX = tanf( DEG2RAD( fd->fov_x * 0.5f ) );
	in.tanY = tanf( DEG2RAD( fd->fov_y * 0.5f ) );
	in.viewX = fd->x * (float)SCREEN_WIDTH / cls.glconfig.vidWidth;
	in.viewY = fd->y * (float)SCREEN_HEIGHT / cls.glconfig.vidHeight;
	in.viewW = fd->width * (float)SCREEN_WIDTH / cls.glconfig.vidWidth;
	in.viewH = fd->height * (float)SCREEN_HEIGHT / cls.glconfig.vidHeight;
	in.viewKnown = qtrue;
}

// a point in the world on the 640x480 screen; its distance in front
static qboolean IN_ToScreen( const vec3_t p, float *x, float *y, float *depth ) {
	vec3_t d;
	float fwd;

	VectorSubtract( p, in.viewOrg, d );
	fwd = DotProduct( d, in.viewAxis[0] );
	if ( fwd < 8.0f || in.tanX <= 0.0f || in.tanY <= 0.0f )
		return qfalse;
	*x = in.viewX + in.viewW * 0.5f * ( 1.0f - DotProduct( d, in.viewAxis[1] ) / fwd / in.tanX );
	*y = in.viewY + in.viewH * 0.5f * ( 1.0f - DotProduct( d, in.viewAxis[2] ) / fwd / in.tanY );
	*depth = fwd;
	return (qboolean)( *x > -20 && *x < SCREEN_WIDTH + 20 && *y > -20 && *y < SCREEN_HEIGHT + 20 );
}

// a quaver drawn at x, y (its head's bottom left), size its height
static void IN_DrawQuaver( float x, float y, float size, const float *color ) {
	const float head = size * 0.36f, stem = Q_max( 1.0f, size * 0.1f );

	// the head: a slanted oval, in a few rows
	for ( int r = 0; r < 4; r++ ) {
		const float t = ( r + 0.5f ) / 4.0f;
		const float w = head * 1.25f * sqrtf( 1.0f - ( 2.0f * t - 1.0f ) * ( 2.0f * t - 1.0f ) * 0.75f );

		IN_Fill( x + ( head * 1.3f - w ) * 0.5f + ( 1.0f - t ) * head * 0.15f, y - head + r * head / 4.0f, w, head / 4.0f + 0.5f, color );
	}
	// the stem, up the head's right, and the flag off its top
	IN_Fill( x + head * 1.3f - stem, y - size, stem, size - head * 0.5f, color );
	for ( int r = 0; r < 4; r++ ) {
		const float fy = y - size + r * size * 0.09f;

		IN_Fill( x + head * 1.3f, fy, size * ( 0.32f - 0.05f * r ), Q_max( 1.0f, size * 0.09f ), color );
	}
}

static float inNoteMaxY = SCREEN_HEIGHT;		// your own while the keyboard's open: kept above it

static void IN_DrawNoteAt( const vec3_t head, int client ) {
	static const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.65f };
	static const float note[4] = { 1.0f, 0.82f, 0.35f, 1.0f };
	trace_t tr;
	float x, y, depth, size, bob;

	if ( !IN_ToScreen( head, &x, &y, &depth ) || depth > IN_NOTE_RANGE )
		return;
	// behind a wall: not shown
	CM_BoxTrace( &tr, in.viewOrg, head, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
	if ( tr.fraction < 0.98f )
		return;
	size = Com_Clamp( 5.0f, 13.0f, 3600.0f / depth );	// about 13 over you close up, smaller farther
	bob = sinf( cls.realtime * 0.004f + client ) * size * 0.12f;
	x -= size * 0.35f;
	y = Q_min( y - bob, inNoteMaxY );
	IN_DrawQuaver( x + 1.0f, y + 1.0f, size, shadow );
	IN_DrawQuaver( x, y, size, note );
}

// over everyone playing now: you too in third person
// yours, in third person: over the keyboard's top while it's open (the keyboard covers your head)
static void IN_DrawOwnNote( float maxY ) {
	const int me = cl.snap.ps.clientNum;
	vec3_t head;

	if ( !in.viewKnown || cls.state != CA_ACTIVE || !cl.snap.valid || cl_instrumentNote->integer != 1
		|| CL_Instruments_Playing( me ) < 0 || !Cvar_VariableIntegerValue( "cg_thirdPerson" ) )
		return;
	VectorCopy( cl.snap.ps.origin, head );
	head[2] += IN_NOTE_HEIGHT;
	inNoteMaxY = maxY;
	IN_DrawNoteAt( head, me );
	inNoteMaxY = SCREEN_HEIGHT;
}

void CL_Instruments_DrawNotes( void ) {
	const int me = cl.snap.ps.clientNum;

	if ( !in.viewKnown || cls.state != CA_ACTIVE || !cl.snap.valid || !cl_instrumentNote->integer )
		return;
	if ( !in.open )
		IN_DrawOwnNote( SCREEN_HEIGHT );
	for ( int i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
		vec3_t head;

		if ( es->number >= MAX_CLIENTS || es->number == me || CL_Instruments_Playing( es->number ) < 0 )
			continue;
		VectorCopy( es->pos.trBase, head );
		head[2] += IN_NOTE_HEIGHT;
		IN_DrawNoteAt( head, es->number );
	}
}

/*
===============================================================================

The panel

===============================================================================
*/

#define IN_X		20.0f
#define IN_W		600.0f
#define IN_Y		286.0f
#define IN_H		188.0f
#define IN_KEY_Y	( IN_Y + 96.0f )
#define IN_KEY_H	84.0f

static const float inPanel[4] = { 0.04f, 0.05f, 0.07f, 0.88f };
static const float inBorder[4] = { 0.95f, 0.6f, 0.2f, 0.9f };
static const float inText[4] = { 0.92f, 0.92f, 0.92f, 1.0f };
static const float inDim[4] = { 0.6f, 0.6f, 0.65f, 1.0f };
static const float inOrange[4] = { 1.0f, 0.65f, 0.2f, 1.0f };
static const float inButton[4] = { 0.14f, 0.15f, 0.2f, 0.95f };
static const float inButtonOn[4] = { 0.45f, 0.28f, 0.08f, 0.95f };
static const float inWhite[4] = { 0.9f, 0.9f, 0.88f, 1.0f };
static const float inBlack[4] = { 0.08f, 0.08f, 0.1f, 1.0f };
static const float inLit[4] = { 1.0f, 0.6f, 0.15f, 1.0f };

// the computer's keys: the lower row an octave from the keys' octave, the upper row the next
static const struct {
	int		key, alt;		// lower case, upper case
	int		offset;			// semitones above the keys' octave
	char	label;
} inKeymap[] = {
	{ A_LOW_Z, A_CAP_Z, 0, 'Z' }, { A_LOW_S, A_CAP_S, 1, 'S' }, { A_LOW_X, A_CAP_X, 2, 'X' }, { A_LOW_D, A_CAP_D, 3, 'D' },
	{ A_LOW_C, A_CAP_C, 4, 'C' }, { A_LOW_V, A_CAP_V, 5, 'V' }, { A_LOW_G, A_CAP_G, 6, 'G' }, { A_LOW_B, A_CAP_B, 7, 'B' },
	{ A_LOW_H, A_CAP_H, 8, 'H' }, { A_LOW_N, A_CAP_N, 9, 'N' }, { A_LOW_J, A_CAP_J, 10, 'J' }, { A_LOW_M, A_CAP_M, 11, 'M' },
	{ A_COMMA, A_COMMA, 12, ',' },
	{ A_LOW_Q, A_CAP_Q, 12, 'Q' }, { A_2, A_2, 13, '2' }, { A_LOW_W, A_CAP_W, 14, 'W' }, { A_3, A_3, 15, '3' },
	{ A_LOW_E, A_CAP_E, 16, 'E' }, { A_LOW_R, A_CAP_R, 17, 'R' }, { A_5, A_5, 18, '5' }, { A_LOW_T, A_CAP_T, 19, 'T' },
	{ A_6, A_6, 20, '6' }, { A_LOW_Y, A_CAP_Y, 21, 'Y' }, { A_7, A_7, 22, '7' }, { A_LOW_U, A_CAP_U, 23, 'U' },
	{ A_LOW_I, A_CAP_I, 24, 'I' },
};

static int IN_Base( void ) {
	int base = 60 + in.octave * 12 - 12;

	if ( Key_IsDown( A_SHIFT ) )
		base += 12;
	if ( Key_IsDown( A_CTRL ) )
		base -= 12;
	return base;
}

static void IN_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void IN_Box( float x, float y, float w, float h, const float *fill, const float *border ) {
	IN_Fill( x, y, w, h, fill );
	IN_Fill( x, y, w, 1, border );
	IN_Fill( x, y + h - 1, w, 1, border );
	IN_Fill( x, y, 1, h, border );
	IN_Fill( x + w - 1, y, 1, h, border );
}

static void IN_Text( float x, float y, float w, const char *text, const float *color, int font, float scale, qboolean centred ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 3 && re->Font_StrLenPixels( buf, font, scale ) > w ) {
		len--;
		buf[len - 3] = buf[len - 2] = buf[len - 1] = '.';
		buf[len] = '\0';
	}
	if ( centred )
		x += ( w - re->Font_StrLenPixels( buf, font, scale ) ) * 0.5f;
	re->Font_DrawString( (int)x, (int)y, buf, color, font, -1, scale );
}

static qboolean IN_InRect( float x, float y, float w, float h ) {
	return (qboolean)( in.cursorX >= x && in.cursorX < x + w && in.cursorY >= y && in.cursorY < y + h );
}

static qboolean IN_Clicked( float x, float y, float w, float h ) {
	if ( !in.click || in.clickX < x || in.clickX >= x + w || in.clickY < y || in.clickY >= y + h )
		return qfalse;
	in.click = qfalse;
	if ( in.clickSound )
		S_StartLocalSound( in.clickSound, CHAN_LOCAL_SOUND );
	return qtrue;
}

static qboolean IN_Button( float x, float y, float w, const char *label, qboolean on ) {
	IN_Box( x, y, w, 16, on ? inButtonOn : inButton, IN_InRect( x, y, w, 16 ) ? inOrange : inDim );
	IN_Text( x, y + 3, w, label, inText, in.font, 0.55f, qtrue );
	return IN_Clicked( x, y, w, 16 );
}

static qboolean IN_IsBlack( int note ) {
	const int k = note % 12;

	return (qboolean)( k == 1 || k == 3 || k == 6 || k == 8 || k == 10 );
}

// the keyboard, C3 to C6: lit where a note sounds, the computer's keys on theirs; a key clicked plays
static void IN_DrawKeyboard( void ) {
	const int whites = 22;
	const float ww = ( IN_W - 20 ) / whites, x0 = IN_X + 10;
	const int base = IN_Base();
	float wx[128];
	int clickedNote = -1;

	for ( int n = IN_LOW, w = 0; n <= IN_HIGH; n++ ) {
		if ( IN_IsBlack( n ) )
			continue;
		wx[n] = x0 + w * ww;
		IN_Box( wx[n], IN_KEY_Y, ww - 1, IN_KEY_H, in.lit[n] > cls.realtime ? inLit : inWhite, inBlack );
		if ( n % 12 == 0 )
			IN_Text( wx[n], IN_KEY_Y + IN_KEY_H - 12, ww - 1, va( "C%i", n / 12 - 1 ), inDim, in.font, 0.45f, qtrue );
		if ( in.instrument == IN_DRUMS && n >= 60 && n < 72 )
			IN_Text( wx[n], IN_KEY_Y + IN_KEY_H - 24, ww - 1, inDrumNames[n - 60], inBlack, in.font, 0.3f, qtrue );
		w++;
	}
	for ( int n = IN_LOW; n <= IN_HIGH; n++ ) {
		if ( !IN_IsBlack( n ) )
			continue;
		wx[n] = wx[n - 1] + ww * 0.68f;
		IN_Fill( wx[n], IN_KEY_Y, ww * 0.62f, IN_KEY_H * 0.6f, in.lit[n] > cls.realtime ? inLit : inBlack );
	}
	// the computer's keys, on the notes they play now
	for ( int i = 0; i < (int)ARRAY_LEN( inKeymap ); i++ ) {
		const int n = base + inKeymap[i].offset;

		// the comma is the upper row's Q again
		if ( n < IN_LOW || n > IN_HIGH || inKeymap[i].label == ',' )
			continue;
		if ( IN_IsBlack( n ) )
			IN_Text( wx[n], IN_KEY_Y + IN_KEY_H * 0.6f - 12, ww * 0.62f, va( "%c", inKeymap[i].label ), inOrange, in.font, 0.42f, qtrue );
		else
			IN_Text( wx[n], IN_KEY_Y + IN_KEY_H * 0.62f + ( i >= 13 ? 0 : 10 ) - 4, ww - 1, va( "%c", inKeymap[i].label ), inOrange, in.font, 0.42f, qtrue );
	}
	// a click: a black key over the white ones
	if ( in.click && in.clickY >= IN_KEY_Y && in.clickY < IN_KEY_Y + IN_KEY_H ) {
		for ( int n = IN_LOW; n <= IN_HIGH && clickedNote < 0; n++ ) {
			if ( IN_IsBlack( n ) && in.clickX >= wx[n] && in.clickX < wx[n] + ww * 0.62f && in.clickY < IN_KEY_Y + IN_KEY_H * 0.6f )
				clickedNote = n;
		}
		for ( int n = IN_LOW; n <= IN_HIGH && clickedNote < 0; n++ ) {
			if ( !IN_IsBlack( n ) && in.clickX >= wx[n] && in.clickX < wx[n] + ww )
				clickedNote = n;
		}
		in.click = qfalse;
		if ( clickedNote >= 0 )
			IN_Play( in.instrument, clickedNote, 100, cls.realtime );
	}
}

static void IN_Close( void ) {
	if ( !in.open )
		return;
	in.open = qfalse;
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_INSTRUMENT );
	if ( !in.songPlaying )
		IN_StopSending();
}

void CL_Instruments_Close( void ) {
	IN_Close();
}

static void IN_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Instruments: join a server first\n" );
		return;
	}
	if ( in.open )
		return;
	CL_SpecialMoves_Close();
	in.font = re->RegisterFont( "arialnb" );
	if ( !in.font )
		in.font = cls.menuFont;
	in.titleFont = re->RegisterFont( "ergoec" );
	if ( !in.titleFont )
		in.titleFont = in.font;
	in.clickSound = S_RegisterSound( "sound/interface/button1.wav" );
	in.cursorX = SCREEN_WIDTH * 0.5f;
	in.cursorY = IN_Y + 40;
	in.click = qfalse;
	memset( in.held, 0, sizeof( in.held ) );
	IN_ListMidi();
	in.open = qtrue;
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_INSTRUMENT );
}

void CL_Instruments_Draw( void ) {
	const float x = IN_X + 10;
	float y = IN_Y + 8;

	if ( !in.open )
		return;
	IN_Box( IN_X, IN_Y, IN_W, IN_H, inPanel, inBorder );
	IN_Text( x, y, 120, "INSTRUMENT", inOrange, in.titleFont, 0.6f, qfalse );
	if ( IN_Button( IN_X + IN_W - 66, y - 2, 56, "Close", qfalse ) ) {
		IN_Close();
		return;
	}
	// the instruments
	for ( int i = 0; i < IN_NUM; i++ ) {
		if ( IN_Button( x + 100 + i * 51, y - 2, 49, inDefs[i].title, (qboolean)( in.instrument == i ) ) )
			in.instrument = i;
	}
	y += 24;
	// the octave, the volume, and who hears it
	IN_Text( x, y + 3, 60, "Octave", inText, in.font, 0.55f, qfalse );
	if ( IN_Button( x + 50, y, 20, "-", qfalse ) )
		in.octave = Q_max( -1, in.octave - 1 );
	IN_Text( x + 72, y + 3, 30, va( "%+i", in.octave ), inText, in.font, 0.55f, qtrue );
	if ( IN_Button( x + 104, y, 20, "+", qfalse ) )
		in.octave = Q_min( 1, in.octave + 1 );
	IN_Text( x + 140, y + 3, 60, "Volume", inText, in.font, 0.55f, qfalse );
	if ( IN_Button( x + 190, y, 20, "-", qfalse ) )
		Cvar_SetValue( "s_volumeInstruments", Q_max( 0.0f, s_volumeInstruments->value - 0.1f ) );
	IN_Text( x + 212, y + 3, 34, va( "%i%%", (int)( s_volumeInstruments->value * 100.0f + 0.5f ) ), inText, in.font, 0.55f, qtrue );
	if ( IN_Button( x + 248, y, 20, "+", qfalse ) )
		Cvar_SetValue( "s_volumeInstruments", Q_min( 1.0f, s_volumeInstruments->value + 0.1f ) );
	IN_Text( x + 290, y + 3, IN_W - 310, !CL_Presence_Session() ? "Not on C1B3R: only you hear it"
		: !CL_Presence_GameLinked() ? "Your game isn't linked (/c1b3r link): only you hear it" : "Heard by those near you with the client",
		inDim, in.font, 0.5f, qfalse );
	y += 24;
	// a MIDI file
	IN_Text( x, y + 3, 40, "MIDI", inText, in.font, 0.55f, qfalse );
	if ( in.files.empty() ) {
		IN_Text( x + 50, y + 3, IN_W - 80, "No files: put .mid files in the midi folder of your rpmod folder", inDim, in.font, 0.5f, qfalse );
	} else {
		if ( IN_Button( x + 50, y, 20, "<", qfalse ) )
			in.file = ( in.file + (int)in.files.size() - 1 ) % (int)in.files.size();
		IN_Text( x + 74, y + 3, 196, in.files[in.file].c_str(), inText, in.font, 0.55f, qtrue );
		if ( IN_Button( x + 272, y, 20, ">", qfalse ) )
			in.file = ( in.file + 1 ) % (int)in.files.size();
		if ( IN_Button( x + 298, y, 50, in.songPlaying ? "Stop" : "Play", in.songPlaying ) ) {
			if ( in.songPlaying )
				IN_StopSong();
			else
				IN_PlaySong( in.files[in.file].c_str() );
		}
		if ( IN_Button( x + 354, y, 150, in.followPrograms ? "The file's instruments" : "All on this instrument", in.followPrograms ) )
			in.followPrograms = (qboolean)!in.followPrograms;
		if ( in.songPlaying ) {
			const int t = cls.realtime - in.songStart;

			IN_Text( x + 510, y + 3, 80, va( "%i:%02i / %i:%02i", t / 60000, ( t / 1000 ) % 60, in.songLength / 60000, ( in.songLength / 1000 ) % 60 ),
				inText, in.font, 0.5f, qfalse );
		}
	}
	y += 22;
	IN_Text( x, y + 2, IN_W - 20, in.message[0] && cls.realtime - in.messageTime < 5000 ? in.message
		: "Z S X D C V G B H N J M , and Q 2 W 3 E R 5 T 6 Y 7 U I play; Shift an octave up, Ctrl down; Esc closes",
		in.message[0] && cls.realtime - in.messageTime < 5000 ? inOrange : inDim, in.font, 0.48f, qfalse );
	IN_DrawKeyboard();
	in.click = qfalse;
	IN_DrawOwnNote( IN_Y - 4.0f );
	// the cursor
	IN_Fill( in.cursorX - 1, in.cursorY - 6, 2, 13, inOrange );
	IN_Fill( in.cursorX - 6, in.cursorY - 1, 13, 2, inOrange );
}

void CL_Instruments_KeyEvent( int key, qboolean down ) {
	if ( !in.open || !down )
		return;
	if ( key == A_MOUSE1 ) {
		in.click = qtrue;
		in.clickX = in.cursorX;
		in.clickY = in.cursorY;
		return;
	}
	if ( key < 0 || key >= MAX_KEYS || in.held[key] )
		return;		// held: no note again till it's let go
	for ( int i = 0; i < (int)ARRAY_LEN( inKeymap ); i++ ) {
		if ( key == inKeymap[i].key || key == inKeymap[i].alt ) {
			const int note = IN_Base() + inKeymap[i].offset;

			in.held[key] = qtrue;
			if ( note >= IN_LOW - 12 && note <= IN_HIGH + 12 )
				IN_Play( in.instrument, note, 100, cls.realtime );
			return;
		}
	}
	if ( key == A_CURSOR_LEFT )
		in.instrument = ( in.instrument + IN_NUM - 1 ) % IN_NUM;
	else if ( key == A_CURSOR_RIGHT )
		in.instrument = ( in.instrument + 1 ) % IN_NUM;
	else if ( key == A_CURSOR_UP )
		in.octave = Q_min( 1, in.octave + 1 );
	else if ( key == A_CURSOR_DOWN )
		in.octave = Q_max( -1, in.octave - 1 );
}

void CL_Instruments_MouseEvent( int dx, int dy ) {
	in.cursorX = Com_Clamp( 0, SCREEN_WIDTH, in.cursorX + dx );
	in.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, in.cursorY + dy );
}

void CL_Instruments_Escape( void ) {
	IN_Close();
}

/*
===============================================================================

The command

===============================================================================
*/

static int IN_Named( const char *word ) {
	for ( int i = 0; i < IN_NUM; i++ ) {
		if ( !Q_stricmp( word, inDefs[i].word ) || !Q_stricmp( word, inDefs[i].title ) )
			return i;
	}
	return -1;
}

static void IN_Instrument_f( void ) {
	const char *arg = Cmd_Argv( 1 );
	int named;

	if ( !arg[0] ) {
		if ( in.open )
			IN_Close();
		else
			IN_Open();
		return;
	}
	if ( ( named = IN_Named( arg ) ) >= 0 ) {
		in.instrument = named;
		IN_Open();
		return;
	}
	if ( !Q_stricmp( arg, "midi" ) && Cmd_Argc() >= 3 ) {
		IN_PlaySong( Cmd_ArgsFrom( 2 ) );
		return;
	}
	if ( !Q_stricmp( arg, "stop" ) ) {
		IN_StopSong();
		IN_StopSending();
		return;
	}
	if ( !Q_stricmp( arg, "list" ) ) {
		IN_ListMidi();
		Com_Printf( "%i MIDI file(s) in midi/:\n", (int)in.files.size() );
		for ( size_t i = 0; i < in.files.size(); i++ )
			Com_Printf( "  %s\n", in.files[i].c_str() );
		return;
	}
	if ( !Q_stricmp( arg, "note" ) && Cmd_Argc() >= 3 ) {
		if ( cls.state != CA_ACTIVE )
			return;
		IN_Play( in.instrument, atoi( Cmd_Argv( 2 ) ), Cmd_Argc() >= 4 ? Com_Clampi( 1, 127, atoi( Cmd_Argv( 3 ) ) ) : 100, cls.realtime );
		return;
	}
	Com_Printf( "usage: instrument                     the instrument's keyboard, or it put away\n" );
	Com_Printf( "       instrument <harp|lute|keys|flute|chimes|marimba|horn|drums>   that one, on its keyboard\n" );
	Com_Printf( "       instrument note <0-127> [velocity]   a note played (60 is middle C), for binds\n" );
	Com_Printf( "       instrument midi <file>         a MIDI file from midi/ played; instrument stop ends it\n" );
	Com_Printf( "       instrument list                the MIDI files there\n" );
}

void CL_Instruments_Init( void ) {
	memset( inCache, 0, sizeof( inCache ) );
	in.instrument = IN_HARP;
	in.followPrograms = qtrue;
	s_volumeInstruments = Cvar_Get( "s_volumeInstruments", "0.8", CVAR_ARCHIVE_ND, "How loud instruments are played, yours and others' (0-1)" );
	cl_instrumentEmote = Cvar_Get( "cl_instrumentEmote", "", CVAR_ARCHIVE_ND,
		"Instruments: a command done as you start playing, an emote (emsit, or one of your custom emotes); \"\" none" );
	cl_instrumentEmoteRepeat = Cvar_Get( "cl_instrumentEmoteRepeat", "0", CVAR_ARCHIVE_ND,
		"Instruments: cl_instrumentEmote done again every this many seconds while you play (2 at least), for an emote that ends by itself; 0 once" );
	cl_instrumentNote = Cvar_Get( "cl_instrumentNote", "1", CVAR_ARCHIVE_ND,
		"Instruments: the music note over the head of whoever plays: 1 everyone's, 2 others' only, 0 none (on your screen only)" );
	cl_instrumentDebug = Cvar_Get( "cl_instrumentDebug", "0", CVAR_TEMP, "Instruments: a console line for each batch of others' notes" );
	Cmd_AddCommand( "instrument", IN_Instrument_f, "Play an instrument: on the keyboard, or a MIDI file, heard by those near you with the client" );
}
