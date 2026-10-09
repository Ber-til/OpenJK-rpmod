/*
===========================================================================
Copyright (C) 2026 RPMod contributors

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

// cl_cin_video.cpp -- plays mp4, webm, mkv and mov cinematics through FFmpeg
//
// A cinematic named "intro" plays video/intro.mp4 (or .webm, .mkv, .mov) when
// one exists, and video/intro.roq otherwise, so a server can ship both and
// clients without this code still get the RoQ. FFmpeg is loaded at run time:
// without its libraries every cinematic is a RoQ, as before.

#include "client.h"
#include "snd_local.h"

#ifdef USE_FFMPEG_VIDEO

#include <deque>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include "sys/sys_loadlib.h"

#define VIDEO_MAX_SIZE		2048	// longer sides are scaled down to this
#define VIDEO_AUDIO_LEAD	0.12	// seconds of sound handed to the mixer ahead of the picture
#define VIDEO_AUDIO_QUEUE	0.5		// seconds of sound decoded ahead
#define VIDEO_MAX_PACKETS	1024	// video packets read ahead while looking for sound

static const char *videoExtensions[] = { ".mp4", ".webm", ".mkv", ".mov" };

/*
===============================================================================

LIBRARIES

===============================================================================
*/

#define FF_FUNC( name ) decltype( &::name ) name

static struct {
	qboolean	tried, loaded;
	void		*libs[5];

	FF_FUNC( av_frame_alloc );
	FF_FUNC( av_frame_free );
	FF_FUNC( av_frame_unref );
	FF_FUNC( av_frame_move_ref );
	FF_FUNC( av_malloc );
	FF_FUNC( av_free );
	FF_FUNC( av_log_set_level );
	FF_FUNC( av_channel_layout_default );
	FF_FUNC( av_channel_layout_uninit );

	FF_FUNC( swr_alloc_set_opts2 );
	FF_FUNC( swr_init );
	FF_FUNC( swr_convert );
	FF_FUNC( swr_get_out_samples );
	FF_FUNC( swr_free );

	FF_FUNC( sws_getCachedContext );
	FF_FUNC( sws_scale );
	FF_FUNC( sws_freeContext );

	FF_FUNC( avcodec_find_decoder );
	FF_FUNC( avcodec_alloc_context3 );
	FF_FUNC( avcodec_parameters_to_context );
	FF_FUNC( avcodec_open2 );
	FF_FUNC( avcodec_send_packet );
	FF_FUNC( avcodec_receive_frame );
	FF_FUNC( avcodec_flush_buffers );
	FF_FUNC( avcodec_free_context );
	FF_FUNC( av_packet_alloc );
	FF_FUNC( av_packet_free );
	FF_FUNC( av_packet_unref );
	FF_FUNC( av_packet_move_ref );

	FF_FUNC( avformat_alloc_context );
	FF_FUNC( avformat_open_input );
	FF_FUNC( avformat_find_stream_info );
	FF_FUNC( av_find_best_stream );
	FF_FUNC( av_read_frame );
	FF_FUNC( av_seek_frame );
	FF_FUNC( avformat_close_input );
	FF_FUNC( avio_alloc_context );
	FF_FUNC( avio_context_free );
} ff;

// the library each function is in, and the file name of that library for the
// version of the headers this was built with, so the structures match
enum { FF_AVUTIL, FF_SWRESAMPLE, FF_SWSCALE, FF_AVCODEC, FF_AVFORMAT };

static const char *FF_LibraryName( int lib ) {
	static const struct { const char *name; int major; } libs[] = {
		{ "avutil", LIBAVUTIL_VERSION_MAJOR },
		{ "swresample", LIBSWRESAMPLE_VERSION_MAJOR },
		{ "swscale", LIBSWSCALE_VERSION_MAJOR },
		{ "avcodec", LIBAVCODEC_VERSION_MAJOR },
		{ "avformat", LIBAVFORMAT_VERSION_MAJOR },
	};
#if defined( _WIN32 )
	return va( "%s-%d.dll", libs[lib].name, libs[lib].major );
#elif defined( __APPLE__ )
	return va( "lib%s.%d.dylib", libs[lib].name, libs[lib].major );
#else
	return va( "lib%s.so.%d", libs[lib].name, libs[lib].major );
#endif
}

static qboolean FF_Load( void ) {
	if ( ff.tried )
		return ff.loaded;
	ff.tried = qtrue;

	for ( int i = 0; i < 5; i++ ) {
		const char *name = FF_LibraryName( i );
		ff.libs[i] = Sys_LoadDll( name, qtrue );
		if ( !ff.libs[i] ) {
			Com_Printf( "Video cinematics off: %s not found, only RoQ will play\n", name );
			return qfalse;
		}
	}

	qboolean ok = qtrue;
#define FF_LOAD( lib, name ) \
	if ( ( *(void **)&ff.name = Sys_LoadFunction( ff.libs[lib], #name ) ) == NULL ) { \
		Com_Printf( "Video cinematics off: %s has no %s\n", FF_LibraryName( lib ), #name ); \
		ok = qfalse; \
	}

	FF_LOAD( FF_AVUTIL, av_frame_alloc );
	FF_LOAD( FF_AVUTIL, av_frame_free );
	FF_LOAD( FF_AVUTIL, av_frame_unref );
	FF_LOAD( FF_AVUTIL, av_frame_move_ref );
	FF_LOAD( FF_AVUTIL, av_malloc );
	FF_LOAD( FF_AVUTIL, av_free );
	FF_LOAD( FF_AVUTIL, av_log_set_level );
	FF_LOAD( FF_AVUTIL, av_channel_layout_default );
	FF_LOAD( FF_AVUTIL, av_channel_layout_uninit );

	FF_LOAD( FF_SWRESAMPLE, swr_alloc_set_opts2 );
	FF_LOAD( FF_SWRESAMPLE, swr_init );
	FF_LOAD( FF_SWRESAMPLE, swr_convert );
	FF_LOAD( FF_SWRESAMPLE, swr_get_out_samples );
	FF_LOAD( FF_SWRESAMPLE, swr_free );

	FF_LOAD( FF_SWSCALE, sws_getCachedContext );
	FF_LOAD( FF_SWSCALE, sws_scale );
	FF_LOAD( FF_SWSCALE, sws_freeContext );

	FF_LOAD( FF_AVCODEC, avcodec_find_decoder );
	FF_LOAD( FF_AVCODEC, avcodec_alloc_context3 );
	FF_LOAD( FF_AVCODEC, avcodec_parameters_to_context );
	FF_LOAD( FF_AVCODEC, avcodec_open2 );
	FF_LOAD( FF_AVCODEC, avcodec_send_packet );
	FF_LOAD( FF_AVCODEC, avcodec_receive_frame );
	FF_LOAD( FF_AVCODEC, avcodec_flush_buffers );
	FF_LOAD( FF_AVCODEC, avcodec_free_context );
	FF_LOAD( FF_AVCODEC, av_packet_alloc );
	FF_LOAD( FF_AVCODEC, av_packet_free );
	FF_LOAD( FF_AVCODEC, av_packet_unref );
	FF_LOAD( FF_AVCODEC, av_packet_move_ref );

	FF_LOAD( FF_AVFORMAT, avformat_alloc_context );
	FF_LOAD( FF_AVFORMAT, avformat_open_input );
	FF_LOAD( FF_AVFORMAT, avformat_find_stream_info );
	FF_LOAD( FF_AVFORMAT, av_find_best_stream );
	FF_LOAD( FF_AVFORMAT, av_read_frame );
	FF_LOAD( FF_AVFORMAT, av_seek_frame );
	FF_LOAD( FF_AVFORMAT, avformat_close_input );
	FF_LOAD( FF_AVFORMAT, avio_alloc_context );
	FF_LOAD( FF_AVFORMAT, avio_context_free );
#undef FF_LOAD

	if ( !ok )
		return qfalse;

	ff.av_log_set_level( AV_LOG_QUIET );
	ff.loaded = qtrue;
	return qtrue;
}

/*
===============================================================================

FILE ACCESS

===============================================================================
*/

struct cinVideo_s {
	fileHandle_t		f;
	FILE				*direct;		// a pk3 entry stored without compression, read with real seeks
	int					base;			// where that entry starts in the pk3
	int64_t				size, pos;

	AVIOContext			*io;
	AVFormatContext		*fmt;
	AVCodecContext		*vdec, *adec;
	int					vstream, astream;
	double				vtb;			// video time base, in seconds
	int64_t				vstart;			// timestamp of the first picture, 0 is the start
	SwsContext			*sws;
	SwrContext			*swr;
	AVFrame				*frame, *shown, *aframe;
	AVPacket			*pkt;
	std::deque<AVPacket *> vpackets;

	qboolean			haveFrame;		// frame holds the next picture, not shown yet
	qboolean			sentFlush, demuxEnd, videoEnd;
	double				framePts;

	int					rate;			// sound sample rate, stereo 16 bit
	std::vector<short>	pcm;			// decoded sound not handed to the mixer yet
	size_t				pcmRead;		// in shorts
	int64_t				pushed;			// sample pairs handed to the mixer since the start

	int					width, height;
	byte				*rgba;
};

static int Video_Read( void *opaque, uint8_t *buf, int size ) {
	cinVideo_t *v = (cinVideo_t *)opaque;
	int64_t left = v->size - v->pos;

	if ( left <= 0 )
		return AVERROR_EOF;
	if ( size > left )
		size = (int)left;

	int n = v->direct ? (int)fread( buf, 1, size, v->direct ) : FS_Read( buf, size, v->f );
	if ( n <= 0 )
		return AVERROR_EOF;
	v->pos += n;
	return n;
}

static int64_t Video_Seek( void *opaque, int64_t offset, int whence ) {
	cinVideo_t *v = (cinVideo_t *)opaque;
	int64_t to;

	if ( whence & AVSEEK_SIZE )
		return v->size;

	switch ( whence & ~AVSEEK_FORCE ) {
	case SEEK_SET: to = offset; break;
	case SEEK_CUR: to = v->pos + offset; break;
	case SEEK_END: to = v->size + offset; break;
	default: return -1;
	}
	if ( to < 0 || to > v->size )
		return -1;
	if ( to == v->pos )
		return to;

	if ( v->direct ) {
		if ( fseek( v->direct, (long)( v->base + to ), SEEK_SET ) )
			return -1;
	} else {
		FS_Seek( v->f, (long)to, FS_SEEK_SET );
	}
	v->pos = to;
	return to;
}

static qboolean Video_OpenFile( cinVideo_t *v, const char *name ) {
	char	pak[MAX_OSPATH];
	int		len;

	len = FS_FOpenFileRead( name, &v->f, qtrue );
	if ( len <= 0 || !v->f ) {
		v->f = 0;
		return qfalse;
	}
	v->size = len;

	// seeking in a compressed pk3 entry reads it again from the start, so read
	// stored ones straight from the pk3 instead
	if ( FS_PakFileStored( v->f, pak, sizeof( pak ), &v->base ) ) {
		v->direct = fopen( pak, "rb" );
		if ( v->direct && fseek( v->direct, v->base, SEEK_SET ) ) {
			fclose( v->direct );
			v->direct = NULL;
		}
	} else if ( FS_IsZipFile( v->f ) ) {
		Com_DPrintf( "%s is compressed in its pk3; store it uncompressed for smooth seeking and looping\n", name );
	}
	return qtrue;
}

/*
===============================================================================

DECODING

===============================================================================
*/

static AVCodecContext *Video_OpenDecoder( AVStream *st ) {
	const AVCodec *codec = ff.avcodec_find_decoder( st->codecpar->codec_id );
	if ( !codec )
		return NULL;

	AVCodecContext *ctx = ff.avcodec_alloc_context3( codec );
	if ( !ctx )
		return NULL;
	if ( ff.avcodec_parameters_to_context( ctx, st->codecpar ) < 0 ) {
		ff.avcodec_free_context( &ctx );
		return NULL;
	}
	ctx->thread_count = 0;	// as many as there are cores
	ctx->pkt_timebase = st->time_base;
	if ( ff.avcodec_open2( ctx, codec, NULL ) < 0 ) {
		ff.avcodec_free_context( &ctx );
		return NULL;
	}
	return ctx;
}

void CIN_VideoClose( cinVideo_t *v ) {
	if ( !v )
		return;

	for ( size_t i = 0; i < v->vpackets.size(); i++ )
		ff.av_packet_free( &v->vpackets[i] );
	if ( v->pkt )
		ff.av_packet_free( &v->pkt );
	if ( v->frame )
		ff.av_frame_free( &v->frame );
	if ( v->shown )
		ff.av_frame_free( &v->shown );
	if ( v->aframe )
		ff.av_frame_free( &v->aframe );
	if ( v->sws )
		ff.sws_freeContext( v->sws );
	if ( v->swr )
		ff.swr_free( &v->swr );
	if ( v->vdec )
		ff.avcodec_free_context( &v->vdec );
	if ( v->adec )
		ff.avcodec_free_context( &v->adec );
	if ( v->fmt )
		ff.avformat_close_input( &v->fmt );
	if ( v->io ) {
		ff.av_free( v->io->buffer );
		ff.avio_context_free( &v->io );
	}
	if ( v->direct )
		fclose( v->direct );
	if ( v->f )
		FS_FCloseFile( v->f );
	if ( v->rgba )
		Z_Free( v->rgba );
	delete v;
}

static cinVideo_t *Video_Open( const char *name ) {
	cinVideo_t *v = new cinVideo_t();
	v->vstream = v->astream = -1;

	if ( !Video_OpenFile( v, name ) ) {
		delete v;
		return NULL;
	}

	const int ioSize = 65536;
	byte *ioBuffer = (byte *)ff.av_malloc( ioSize );
	v->io = ioBuffer ? ff.avio_alloc_context( ioBuffer, ioSize, 0, v, Video_Read, NULL, Video_Seek ) : NULL;
	v->fmt = ff.avformat_alloc_context();
	if ( !v->io || !v->fmt ) {
		if ( !v->io && ioBuffer )
			ff.av_free( ioBuffer );
		CIN_VideoClose( v );
		return NULL;
	}
	v->fmt->pb = v->io;
	v->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

	if ( ff.avformat_open_input( &v->fmt, name, NULL, NULL ) < 0 ) {
		// it frees the context it was given when it fails
		v->fmt = NULL;
		Com_Printf( S_COLOR_YELLOW "Can't play %s: not a video FFmpeg can read\n", name );
		CIN_VideoClose( v );
		return NULL;
	}
	ff.avformat_find_stream_info( v->fmt, NULL );

	v->vstream = ff.av_find_best_stream( v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0 );
	if ( v->vstream < 0 || ( v->vdec = Video_OpenDecoder( v->fmt->streams[v->vstream] ) ) == NULL ) {
		Com_Printf( S_COLOR_YELLOW "Can't play %s: no video stream this build can decode\n", name );
		CIN_VideoClose( v );
		return NULL;
	}
	v->vtb = av_q2d( v->fmt->streams[v->vstream]->time_base );
	if ( v->fmt->streams[v->vstream]->start_time != AV_NOPTS_VALUE )
		v->vstart = v->fmt->streams[v->vstream]->start_time;

	v->astream = ff.av_find_best_stream( v->fmt, AVMEDIA_TYPE_AUDIO, -1, v->vstream, NULL, 0 );
	if ( v->astream >= 0 ) {
		v->adec = Video_OpenDecoder( v->fmt->streams[v->astream] );
		if ( v->adec ) {
			AVChannelLayout stereo;
			ff.av_channel_layout_default( &stereo, 2 );
			v->rate = v->adec->sample_rate > 0 ? v->adec->sample_rate : 44100;
			if ( ff.swr_alloc_set_opts2( &v->swr, &stereo, AV_SAMPLE_FMT_S16, v->rate,
					&v->adec->ch_layout, v->adec->sample_fmt, v->adec->sample_rate, 0, NULL ) < 0
				|| ff.swr_init( v->swr ) < 0 ) {
				if ( v->swr )
					ff.swr_free( &v->swr );
				ff.avcodec_free_context( &v->adec );
			}
			ff.av_channel_layout_uninit( &stereo );
		}
		if ( !v->adec ) {
			Com_Printf( S_COLOR_YELLOW "%s: can't decode its sound, playing it silent\n", name );
			v->astream = -1;
		}
	}

	// the size it is drawn at, scaled down when a side is too long for a texture
	v->width = v->vdec->width;
	v->height = v->vdec->height;
	if ( v->width <= 0 || v->height <= 0 ) {
		CIN_VideoClose( v );
		return NULL;
	}
	if ( v->width > VIDEO_MAX_SIZE || v->height > VIDEO_MAX_SIZE ) {
		float scale = (float)VIDEO_MAX_SIZE / Q_max( v->width, v->height );
		v->width = Q_max( 2, (int)( v->width * scale ) & ~1 );
		v->height = Q_max( 2, (int)( v->height * scale ) & ~1 );
	}
	v->rgba = (byte *)Z_Malloc( v->width * v->height * 4, TAG_GENERAL, qtrue );

	v->frame = ff.av_frame_alloc();
	v->shown = ff.av_frame_alloc();
	v->aframe = ff.av_frame_alloc();
	v->pkt = ff.av_packet_alloc();
	if ( !v->frame || !v->shown || !v->aframe || !v->pkt ) {
		CIN_VideoClose( v );
		return NULL;
	}
	return v;
}

cinVideo_t *CIN_VideoOpen( const char *name ) {
	char		base[MAX_QPATH], file[MAX_QPATH];
	cinVideo_t	*v;

	if ( !FF_Load() )
		return NULL;

	COM_StripExtension( name, base, sizeof( base ) );
	for ( int i = 0; i < (int)ARRAY_LEN( videoExtensions ); i++ ) {
		Com_sprintf( file, sizeof( file ), "%s%s", base, videoExtensions[i] );
		if ( ( v = Video_Open( file ) ) != NULL ) {
			Com_DPrintf( "CIN_VideoOpen: playing %s\n", file );
			return v;
		}
	}
	return NULL;
}

int CIN_VideoWidth( const cinVideo_t *v ) { return v->width; }
int CIN_VideoHeight( const cinVideo_t *v ) { return v->height; }
byte *CIN_VideoBuffer( const cinVideo_t *v ) { return v->rgba; }

static double Video_QueuedSound( const cinVideo_t *v ) {
	return (double)( ( v->pcm.size() - v->pcmRead ) / 2 ) / v->rate;
}

static void Video_DecodeSound( cinVideo_t *v, AVPacket *pkt ) {
	if ( ff.avcodec_send_packet( v->adec, pkt ) < 0 )
		return;

	while ( ff.avcodec_receive_frame( v->adec, v->aframe ) >= 0 ) {
		int room = ff.swr_get_out_samples( v->swr, v->aframe->nb_samples );
		if ( room > 0 ) {
			size_t at = v->pcm.size();
			v->pcm.resize( at + room * 2 );
			uint8_t *out = (uint8_t *)&v->pcm[at];
			int got = ff.swr_convert( v->swr, &out, room, (const uint8_t **)v->aframe->extended_data, v->aframe->nb_samples );
			v->pcm.resize( at + Q_max( got, 0 ) * 2 );
		}
		ff.av_frame_unref( v->aframe );
	}
}

// reads one packet: sound is decoded now, pictures wait in a queue for their time
static qboolean Video_ReadPacket( cinVideo_t *v ) {
	if ( v->demuxEnd )
		return qfalse;

	if ( ff.av_read_frame( v->fmt, v->pkt ) < 0 ) {
		v->demuxEnd = qtrue;
		if ( v->adec )
			Video_DecodeSound( v, NULL );
		return qfalse;
	}

	if ( v->pkt->stream_index == v->vstream ) {
		AVPacket *queued = ff.av_packet_alloc();
		if ( queued ) {
			ff.av_packet_move_ref( queued, v->pkt );
			v->vpackets.push_back( queued );
		}
	} else if ( v->pkt->stream_index == v->astream ) {
		Video_DecodeSound( v, v->pkt );
	}
	ff.av_packet_unref( v->pkt );
	return qtrue;
}

// the next picture into v->frame; qfalse when there are none left
static qboolean Video_NextFrame( cinVideo_t *v ) {
	for ( ;; ) {
		int r = ff.avcodec_receive_frame( v->vdec, v->frame );
		if ( r >= 0 ) {
			int64_t ts = v->frame->best_effort_timestamp;
			if ( ts == AV_NOPTS_VALUE )
				ts = v->frame->pts;
			v->framePts = ts == AV_NOPTS_VALUE ? v->framePts : ( ts - v->vstart ) * v->vtb;
			return qtrue;
		}
		if ( r != AVERROR( EAGAIN ) )
			return qfalse;

		while ( v->vpackets.empty() && Video_ReadPacket( v ) )
			;
		if ( v->vpackets.empty() ) {
			if ( v->sentFlush )
				return qfalse;
			ff.avcodec_send_packet( v->vdec, NULL );
			v->sentFlush = qtrue;
			continue;
		}

		AVPacket *pkt = v->vpackets.front();
		v->vpackets.pop_front();
		ff.avcodec_send_packet( v->vdec, pkt );
		ff.av_packet_free( &pkt );
	}
}

static void Video_Convert( cinVideo_t *v, AVFrame *frame ) {
	v->sws = ff.sws_getCachedContext( v->sws, frame->width, frame->height, (AVPixelFormat)frame->format,
		v->width, v->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL );
	if ( !v->sws )
		return;

	uint8_t *dst[4] = { v->rgba, NULL, NULL, NULL };
	int stride[4] = { v->width * 4, 0, 0, 0 };
	ff.sws_scale( v->sws, frame->data, frame->linesize, 0, frame->height, dst, stride );
}

static void Video_PushSound( cinVideo_t *v, double time, qboolean silent ) {
	if ( !v->adec )
		return;

	// fell behind, a hitch or the window was in the background: skip to now
	double end = (double)v->pushed / v->rate;
	if ( end < time - 0.1 ) {
		int64_t skip = (int64_t)( ( time - end ) * v->rate );
		size_t queued = ( v->pcm.size() - v->pcmRead ) / 2;
		if ( skip > (int64_t)queued )
			skip = queued;
		v->pcmRead += (size_t)skip * 2;
		v->pushed += skip;
	}

	int64_t want = (int64_t)( ( time + VIDEO_AUDIO_LEAD ) * v->rate ) - v->pushed;
	size_t queued = ( v->pcm.size() - v->pcmRead ) / 2;
	if ( want > (int64_t)queued )
		want = queued;
	if ( want > 0 ) {
		if ( !silent )
			S_RawSamples( (int)want, v->rate, 2, 2, (const byte *)&v->pcm[v->pcmRead], s_volume->value, 1 );
		v->pcmRead += (size_t)want * 2;
		v->pushed += want;
	}

	if ( v->pcmRead > 65536 ) {
		v->pcm.erase( v->pcm.begin(), v->pcm.begin() + v->pcmRead );
		v->pcmRead = 0;
	}
}

/*
==================
CIN_VideoUpdate

Brings the picture and the sound up to time seconds since the start. Sets
*newFrame when the picture changed; returns qfalse once everything played.
==================
*/
qboolean CIN_VideoUpdate( cinVideo_t *v, double time, qboolean silent, qboolean *newFrame ) {
	qboolean due = qfalse;

	*newFrame = qfalse;

	// sound comes in its own packets, read far enough ahead to always have some
	while ( v->adec && !v->demuxEnd && Video_QueuedSound( v ) + (double)v->pushed / v->rate < time + VIDEO_AUDIO_QUEUE
		&& v->vpackets.size() < VIDEO_MAX_PACKETS ) {
		Video_ReadPacket( v );
	}

	// decode up to the last picture that is due, show only that one
	while ( !v->videoEnd ) {
		if ( !v->haveFrame ) {
			if ( !Video_NextFrame( v ) ) {
				v->videoEnd = qtrue;
				break;
			}
			v->haveFrame = qtrue;
		}
		if ( v->framePts > time )
			break;
		ff.av_frame_unref( v->shown );
		ff.av_frame_move_ref( v->shown, v->frame );
		v->haveFrame = qfalse;
		due = qtrue;
	}
	if ( due ) {
		Video_Convert( v, v->shown );
		*newFrame = qtrue;
	}

	Video_PushSound( v, time, silent );

	if ( !v->videoEnd )
		return qtrue;
	// the picture is done, let the sound finish
	return (qboolean)( v->adec && ( !v->demuxEnd || Video_QueuedSound( v ) > 0 ) );
}

void CIN_VideoRewind( cinVideo_t *v ) {
	ff.av_seek_frame( v->fmt, -1, 0, AVSEEK_FLAG_BACKWARD );
	ff.avcodec_flush_buffers( v->vdec );
	if ( v->adec )
		ff.avcodec_flush_buffers( v->adec );

	for ( size_t i = 0; i < v->vpackets.size(); i++ )
		ff.av_packet_free( &v->vpackets[i] );
	v->vpackets.clear();
	ff.av_frame_unref( v->frame );

	v->haveFrame = v->sentFlush = v->demuxEnd = v->videoEnd = qfalse;
	v->framePts = 0;
	v->pcm.clear();
	v->pcmRead = 0;
	v->pushed = 0;
}

#else // !USE_FFMPEG_VIDEO

cinVideo_t *CIN_VideoOpen( const char *name ) { return NULL; }
void CIN_VideoClose( cinVideo_t *v ) { }
int CIN_VideoWidth( const cinVideo_t *v ) { return 0; }
int CIN_VideoHeight( const cinVideo_t *v ) { return 0; }
byte *CIN_VideoBuffer( const cinVideo_t *v ) { return NULL; }
qboolean CIN_VideoUpdate( cinVideo_t *v, double time, qboolean silent, qboolean *newFrame ) { *newFrame = qfalse; return qfalse; }
void CIN_VideoRewind( cinVideo_t *v ) { }

#endif // USE_FFMPEG_VIDEO
