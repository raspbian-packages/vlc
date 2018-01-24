/*****************************************************************************
 * cast.cpp: Chromecast sout module for vlc
 *****************************************************************************
 * Copyright © 2014-2015 VideoLAN
 *
 * Authors: Adrien Maglo <magsoft@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Steve Lhomme <robux4@videolabs.io>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "chromecast.h"
#include <vlc_dialog.h>

#include <vlc_sout.h>
#include <vlc_block.h>
#include <vlc_modules.h>

#include <cassert>

struct sout_stream_sys_t
{
    sout_stream_sys_t(intf_sys_t * const intf, bool has_video, int port,
                      const char *psz_default_muxer, const char *psz_default_mime)
        : p_out(NULL)
        , default_muxer(psz_default_muxer)
        , default_mime(psz_default_mime)
        , p_intf(intf)
        , b_supports_video(has_video)
        , i_port(port)
        , es_changed( true )
        , transcode_attempt_idx( 0 )
        , previous_state( Authenticating )
    {
        assert(p_intf != NULL);
    }

    ~sout_stream_sys_t()
    {
        sout_StreamChainDelete(p_out, NULL);
        delete p_intf;
    }

    bool canDecodeVideo( vlc_fourcc_t i_codec ) const;
    bool canDecodeAudio( sout_stream_t* p_stream, vlc_fourcc_t i_codec,
                         const audio_format_t* p_fmt ) const;
    bool startSoutChain(sout_stream_t* p_stream);

    sout_stream_t     *p_out;
    std::string        sout;
    const std::string  default_muxer;
    const std::string  default_mime;

    intf_sys_t * const p_intf;
    const bool b_supports_video;
    const int i_port;

    sout_stream_id_sys_t *GetSubId( sout_stream_t*, sout_stream_id_sys_t* );

    bool                               es_changed;
    std::vector<sout_stream_id_sys_t*> streams;
    std::vector<sout_stream_id_sys_t*> out_streams;
    unsigned int                       transcode_attempt_idx;
    States                             previous_state;

private:
    bool UpdateOutput( sout_stream_t * );
};

#define SOUT_CFG_PREFIX "sout-chromecast-"

static const vlc_fourcc_t DEFAULT_TRANSCODE_VIDEO = VLC_CODEC_H264;
static const unsigned int MAX_TRANSCODE_PASS = 3;
static const char DEFAULT_MUXER[] = "avformat{mux=matroska,options={live=1}}}";


/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int Open(vlc_object_t *);
static void Close(vlc_object_t *);

static const char *const ppsz_sout_options[] = {
    "ip", "port",  "http-port", "mux", "mime", "video", NULL
};

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

#define HTTP_PORT_TEXT N_("HTTP port")
#define HTTP_PORT_LONGTEXT N_("This sets the HTTP port of the local server " \
                              "used to stream the media to the Chromecast.")
#define HAS_VIDEO_TEXT N_("Video")
#define HAS_VIDEO_LONGTEXT N_("The Chromecast receiver can receive video.")
#define MUX_TEXT N_("Muxer")
#define MUX_LONGTEXT N_("This sets the muxer used to stream to the Chromecast.")
#define MIME_TEXT N_("MIME content type")
#define MIME_LONGTEXT N_("This sets the media MIME content type sent to the Chromecast.")
#define PERF_TEXT N_( "Performance warning" )
#define PERF_LONGTEXT N_( "Display a performance warning when transcoding" )
#define AUDIO_PASSTHROUGH_TEXT N_( "Enable Audio passthrough" )
#define AUDIO_PASSTHROUGH_LONGTEXT N_( "Disable if your receiver does not support Dolby®" )

enum {
    CONVERSION_QUALITY_HIGH = 0,
    CONVERSION_QUALITY_MEDIUM = 1,
    CONVERSION_QUALITY_LOW = 2,
    CONVERSION_QUALITY_LOWCPU = 3,
};

#if defined (__ANDROID__) || defined (__arm__) || (defined (TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
# define CONVERSION_QUALITY_DEFAULT CONVERSION_QUALITY_LOW
#else
# define CONVERSION_QUALITY_DEFAULT CONVERSION_QUALITY_MEDIUM
#endif

static const int conversion_quality_list[] = {
    CONVERSION_QUALITY_HIGH,
    CONVERSION_QUALITY_MEDIUM,
    CONVERSION_QUALITY_LOW,
    CONVERSION_QUALITY_LOWCPU,
};
static const char *const conversion_quality_list_text[] = {
    N_( "High (high quality and high bandwith)" ),
    N_( "Medium (medium quality and medium bandwidth)" ),
    N_( "Low (low quality and low bandwith)" ),
    N_( "Low CPU (low quality but high bandwith)" ),
};

#define CONVERSION_QUALITY_TEXT N_( "Conversion quality" )
#define CONVERSION_QUALITY_LONGTEXT N_( "Change this option to increase conversion speed or quality." )

#define IP_ADDR_TEXT N_("IP Address")
#define IP_ADDR_LONGTEXT N_("IP Address of the Chromecast.")
#define PORT_TEXT N_("Chromecast port")
#define PORT_LONGTEXT N_("The port used to talk to the Chromecast.")

vlc_module_begin ()

    set_shortname(N_("Chromecast"))
    set_description(N_("Chromecast stream output"))
    set_capability("sout stream", 0)
    add_shortcut("chromecast")
    set_category(CAT_SOUT)
    set_subcategory(SUBCAT_SOUT_STREAM)
    set_callbacks(Open, Close)

    add_string(SOUT_CFG_PREFIX "ip", NULL, IP_ADDR_TEXT, IP_ADDR_LONGTEXT, false)
        change_private()
    add_integer(SOUT_CFG_PREFIX "port", CHROMECAST_CONTROL_PORT, PORT_TEXT, PORT_LONGTEXT, false)
        change_private()
    add_integer(SOUT_CFG_PREFIX "http-port", HTTP_PORT, HTTP_PORT_TEXT, HTTP_PORT_LONGTEXT, false)
    add_bool(SOUT_CFG_PREFIX "video", true, HAS_VIDEO_TEXT, HAS_VIDEO_LONGTEXT, false)
    add_string(SOUT_CFG_PREFIX "mux", DEFAULT_MUXER, MUX_TEXT, MUX_LONGTEXT, false)
    add_string(SOUT_CFG_PREFIX "mime", "video/x-matroska", MIME_TEXT, MIME_LONGTEXT, false)
    add_integer(SOUT_CFG_PREFIX "show-perf-warning", 1, PERF_TEXT, PERF_LONGTEXT, true )
        change_private()
    add_bool(SOUT_CFG_PREFIX "audio-passthrough", true, AUDIO_PASSTHROUGH_TEXT, AUDIO_PASSTHROUGH_LONGTEXT, false )
    add_integer(SOUT_CFG_PREFIX "conversion-quality", CONVERSION_QUALITY_DEFAULT,
                CONVERSION_QUALITY_TEXT, CONVERSION_QUALITY_LONGTEXT, false );
        change_integer_list(conversion_quality_list, conversion_quality_list_text)

vlc_module_end ()


struct sout_stream_id_sys_t
{
    es_format_t           fmt;
    sout_stream_id_sys_t  *p_sub_id;
};

/*****************************************************************************
 * Sout callbacks
 *****************************************************************************/
static sout_stream_id_sys_t *Add(sout_stream_t *p_stream, const es_format_t *p_fmt)
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    if (!p_sys->b_supports_video)
    {
        if (p_fmt->i_cat != AUDIO_ES)
            return NULL;
    }

    sout_stream_id_sys_t *p_sys_id = (sout_stream_id_sys_t *)malloc( sizeof(sout_stream_id_sys_t) );
    if (p_sys_id != NULL)
    {
        es_format_Copy( &p_sys_id->fmt, p_fmt );
        p_sys_id->p_sub_id = NULL;

        p_sys->streams.push_back( p_sys_id );
        p_sys->es_changed = true;
    }
    return p_sys_id;
}


static void Del(sout_stream_t *p_stream, sout_stream_id_sys_t *id)
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    for (std::vector<sout_stream_id_sys_t*>::iterator it = p_sys->streams.begin();
         it != p_sys->streams.end(); )
    {
        sout_stream_id_sys_t *p_sys_id = *it;
        if ( p_sys_id == id )
        {
            if ( p_sys_id->p_sub_id != NULL )
            {
                sout_StreamIdDel( p_sys->p_out, p_sys_id->p_sub_id );
                for (std::vector<sout_stream_id_sys_t*>::iterator out_it = p_sys->out_streams.begin();
                     out_it != p_sys->out_streams.end(); )
                {
                    if (*out_it == id)
                    {
                        p_sys->out_streams.erase(out_it);
                        p_sys->es_changed = true;
                        break;
                    }
                    out_it++;
                }
            }

            es_format_Clean( &p_sys_id->fmt );
            free( p_sys_id );
            p_sys->streams.erase( it );
            break;
        }
        it++;
    }

    if ( p_sys->out_streams.empty() )
    {
        p_sys->p_intf->requestPlayerStop();

        sout_StreamChainDelete( p_sys->p_out, NULL );
        p_sys->p_out = NULL;
        p_sys->sout = "";
        p_sys->transcode_attempt_idx = 0;
    }
}

/**
 * Transcode steps:
 * 0: Accept HEVC/VP9 & all supported audio formats
 * 1: Transcode to h264 & accept all supported audio formats if the video codec
 *    was HEVC/VP9
 * 2: Transcode to H264 & MP3
 *
 * Additionally:
 * - Allow (E)AC3 passthrough depending on the audio-passthrough
 *   config value, except for the final step, where we just give up and transcode
 *   everything.
 * - Disallow multichannel AAC
 *
 * Supported formats: https://developers.google.com/cast/docs/media
 */

bool sout_stream_sys_t::canDecodeVideo( vlc_fourcc_t i_codec ) const
{
    if ( transcode_attempt_idx != 0 )
        return false;
    if ( i_codec == VLC_CODEC_HEVC || i_codec == VLC_CODEC_VP9 )
        return transcode_attempt_idx == 0;
    return i_codec == VLC_CODEC_H264 || i_codec == VLC_CODEC_VP8;
}

bool sout_stream_sys_t::canDecodeAudio( sout_stream_t *p_stream,
                                        vlc_fourcc_t i_codec,
                                        const audio_format_t* p_fmt ) const
{
    if ( transcode_attempt_idx == MAX_TRANSCODE_PASS - 1 )
        return false;
    if ( i_codec == VLC_CODEC_A52 || i_codec == VLC_CODEC_EAC3 )
    {
        return var_InheritBool( p_stream, SOUT_CFG_PREFIX "audio-passthrough" );
    }
    if ( i_codec == VLC_FOURCC('h', 'a', 'a', 'c') ||
            i_codec == VLC_FOURCC('l', 'a', 'a', 'c') ||
            i_codec == VLC_FOURCC('s', 'a', 'a', 'c') ||
            i_codec == VLC_CODEC_MPGA ||
            i_codec == VLC_CODEC_MP4A )
    {
        return p_fmt->i_channels <= 2;
    }
    return i_codec == VLC_CODEC_VORBIS || i_codec == VLC_CODEC_OPUS ||
           i_codec == VLC_CODEC_MP3;
}

bool sout_stream_sys_t::startSoutChain( sout_stream_t *p_stream )
{
    if ( unlikely( p_out != NULL ) )
    {
        for ( size_t i = 0; i < streams.size(); i++ )
        {
            if ( streams[i]->p_sub_id != NULL )
                sout_StreamIdDel( p_out, streams[i]->p_sub_id );
        }
        sout_StreamChainDelete( p_out, NULL );
    }

    msg_Dbg( p_stream, "Creating chain %s", sout.c_str() );
    p_out = sout_StreamChainNew( p_stream->p_sout, sout.c_str(), NULL, NULL);
    if (p_out == NULL) {
        msg_Dbg(p_stream, "could not create sout chain:%s", sout.c_str());
        return false;
    }

    /* check the streams we can actually add */
    for (std::vector<sout_stream_id_sys_t*>::iterator it = out_streams.begin();
         it != out_streams.end(); )
    {
        sout_stream_id_sys_t *p_sys_id = *it;
        p_sys_id->p_sub_id = sout_StreamIdAdd( p_out, &p_sys_id->fmt );
        if ( p_sys_id->p_sub_id == NULL )
        {
            msg_Err( p_stream, "can't handle %4.4s stream", (char *)&p_sys_id->fmt.i_codec );
            es_format_Clean( &p_sys_id->fmt );
            it = out_streams.erase( it );
        }
        else
            ++it;
    }
    return out_streams.empty() == false;
}

bool sout_stream_sys_t::UpdateOutput( sout_stream_t *p_stream )
{
    assert( p_stream->p_sys == this );

    if ( !es_changed )
        return true;

    es_changed = false;

    bool canRemux = true;
    vlc_fourcc_t i_codec_video = 0, i_codec_audio = 0;
    const es_format_t *p_original_audio = NULL;
    const es_format_t *p_original_video = NULL;
    out_streams.clear();

    for (std::vector<sout_stream_id_sys_t*>::iterator it = streams.begin(); it != streams.end(); ++it)
    {
        const es_format_t *p_es = &(*it)->fmt;
        if (p_es->i_cat == AUDIO_ES && p_original_audio == NULL)
        {
            if ( !canDecodeAudio( p_stream, p_es->i_codec, &p_es->audio ) )
            {
                msg_Dbg( p_stream, "can't remux audio track %d codec %4.4s", p_es->i_id, (const char*)&p_es->i_codec );
                canRemux = false;
            }
            else if (i_codec_audio == 0)
                i_codec_audio = p_es->i_codec;
            p_original_audio = p_es;
            out_streams.push_back(*it);
        }
        else if (b_supports_video)
        {
            if (p_es->i_cat == VIDEO_ES && p_original_video == NULL)
            {
                if (!canDecodeVideo( p_es->i_codec ))
                {
                    msg_Dbg( p_stream, "can't remux video track %d codec %4.4s",
                             p_es->i_id, (const char*)&p_es->i_codec );
                    canRemux = false;
                }
                else if (i_codec_video == 0)
                    i_codec_video = p_es->i_codec;
                p_original_video = p_es;
                out_streams.push_back(*it);
            }
            /* TODO: else handle ttml/webvtt */
        }
    }

    if (out_streams.empty())
        return true;

    std::stringstream ssout;
    if ( !canRemux )
    {
        if ( var_InheritInteger( p_stream, SOUT_CFG_PREFIX "show-perf-warning" ) )
        {
            int res = vlc_dialog_wait_question( p_stream,
                          VLC_DIALOG_QUESTION_WARNING,
                         _("Cancel"), _("OK"), _("Ok, Don't warn me again"),
                         _("Performance warning"),
                         _("Casting this video requires conversion. "
                           "This conversion can use all the available power and "
                           "could quickly drain your battery." ) );
            if ( res <= 0 )
                 return false;
            if ( res == 2 )
                config_PutInt(p_stream, SOUT_CFG_PREFIX "show-perf-warning", 0 );
        }

        static const char video_maxres_hd[] = "maxwidth=1920,maxheight=1080";
        static const char video_maxres_720p[] = "maxwidth=1280,maxheight=720";
        static const char video_x264_preset_veryfast[] = "veryfast";
        static const char video_x264_preset_ultrafast[] = "ultrafast";

        const int i_quality = var_InheritInteger( p_stream, SOUT_CFG_PREFIX "conversion-quality" );
        const char *psz_video_maxres;
        const char *psz_video_x264_preset;
        unsigned i_video_x264_crf_hd, i_video_x264_crf_720p;
        bool b_audio_mp3;

        switch ( i_quality )
        {
            case CONVERSION_QUALITY_HIGH:
                psz_video_maxres = video_maxres_hd;
                i_video_x264_crf_hd = i_video_x264_crf_720p = 21;
                psz_video_x264_preset = video_x264_preset_veryfast;
                b_audio_mp3 = false;
                break;
            case CONVERSION_QUALITY_MEDIUM:
                psz_video_maxres = video_maxres_hd;
                i_video_x264_crf_hd = 23;
                i_video_x264_crf_720p = 21;
                psz_video_x264_preset = video_x264_preset_veryfast;
                b_audio_mp3 = false;
                break;
            case CONVERSION_QUALITY_LOW:
                psz_video_maxres = video_maxres_720p;
                i_video_x264_crf_hd = i_video_x264_crf_720p = 23;
                psz_video_x264_preset = video_x264_preset_veryfast;
                b_audio_mp3 = true;
                break;
            default:
            case CONVERSION_QUALITY_LOWCPU:
                psz_video_maxres = video_maxres_720p;
                i_video_x264_crf_hd = i_video_x264_crf_720p = 23;
                psz_video_x264_preset = video_x264_preset_ultrafast;
                b_audio_mp3 = true;
                break;
        }

        /* TODO: provide audio samplerate and channels */
        ssout << "transcode{";
        char s_fourcc[5];
        if ( i_codec_audio == 0 && p_original_audio )
        {
            if ( !b_audio_mp3
              && p_original_audio->audio.i_channels > 2 && module_exists( "vorbis" ) )
                i_codec_audio = VLC_CODEC_VORBIS;
            else
                i_codec_audio = VLC_CODEC_MP3;

            msg_Dbg( p_stream, "Converting audio to %.4s", (const char*)&i_codec_audio );
            ssout << "acodec=";
            vlc_fourcc_to_char( i_codec_audio, s_fourcc );
            s_fourcc[4] = '\0';
            ssout << s_fourcc << ',';
            if( i_codec_audio == VLC_CODEC_VORBIS )
                ssout << "aenc=vorbis{quality=6},";
        }
        if ( b_supports_video && i_codec_video == 0 )
        {
            i_codec_video = DEFAULT_TRANSCODE_VIDEO;
            msg_Dbg( p_stream, "Converting video to %.4s", (const char*)&i_codec_video );
            ssout << "vcodec=";
            vlc_fourcc_to_char( i_codec_video, s_fourcc );
            s_fourcc[4] = '\0';
            ssout << s_fourcc << ',' << psz_video_maxres << ',';

            const video_format_t *p_vid =
                p_original_video ? &p_original_video->video : NULL;
            const bool b_hdres = p_vid == NULL || p_vid->i_height == 0 || p_vid->i_height >= 800;
            unsigned i_video_x264_crf = b_hdres ? i_video_x264_crf_hd : i_video_x264_crf_720p;

            if( p_vid == NULL
             || p_vid->i_frame_rate == 0 || p_vid->i_frame_rate_base == 0
             || ( p_vid->i_frame_rate / p_vid->i_frame_rate_base ) > 30 )
            {
                /* Even force 24fps if the frame rate is unknown */
                msg_Warn( p_stream, "lowering frame rate to 24fps" );
                ssout << "fps=24,";
            }

            if( i_codec_video == VLC_CODEC_H264 )
            {
                if ( module_exists("x264") )
                    ssout << "venc=x264{preset=" << psz_video_x264_preset
                          << ",crf=" << i_video_x264_crf << "},";
            }
        }
        ssout << "}:";
    }
    std::string mime;
    if ( !b_supports_video && default_muxer == DEFAULT_MUXER )
        mime = "audio/x-matroska";
    else if ( i_codec_audio == VLC_CODEC_VORBIS &&
              i_codec_video == VLC_CODEC_VP8 &&
              default_muxer == DEFAULT_MUXER )
        mime = "video/webm";
    else
        mime = default_mime;

    ssout << "http{dst=:" << i_port << "/stream"
          << ",mux=" << default_muxer
          << ",access=http{mime=" << mime << "}}";

    sout = ssout.str();

    if ( startSoutChain( p_stream ) )
    {
        /* tell the chromecast to load the content */
        p_intf->setHasInput( mime );
    }
    else
    {
        p_intf->requestPlayerStop();

        sout_StreamChainDelete( p_out, NULL );
        p_out = NULL;
        sout = "";
    }
    return true;
}

sout_stream_id_sys_t *sout_stream_sys_t::GetSubId( sout_stream_t *p_stream,
                                                   sout_stream_id_sys_t *id )
{
    size_t i;

    assert( p_stream->p_sys == this );

    if ( UpdateOutput( p_stream ) == false )
        return NULL;

    for (i = 0; i < streams.size(); ++i)
    {
        if ( id == (sout_stream_id_sys_t*) streams[i] )
            return streams[i]->p_sub_id;
    }

    msg_Err( p_stream, "unknown stream ID" );
    return NULL;
}

static int Send(sout_stream_t *p_stream, sout_stream_id_sys_t *id,
                block_t *p_buffer)
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    id = p_sys->GetSubId( p_stream, id );
    if ( id == NULL )
    {
        block_Release( p_buffer );
        return VLC_EGENERIC;
    }
    States s = p_sys->p_intf->state();
    if ( p_sys->previous_state != s )
    {
        if ( s == LoadFailed && p_sys->es_changed == false )
        {
            if ( p_sys->transcode_attempt_idx > MAX_TRANSCODE_PASS - 1 )
            {
                msg_Err( p_stream, "All attempts failed. Giving up." );
                block_Release( p_buffer );
                return VLC_EGENERIC;
            }
            p_sys->transcode_attempt_idx++;
            p_sys->es_changed = true;
            msg_Warn( p_stream, "Load failed detected. Switching to next "
                     "configuration index: %u", p_sys->transcode_attempt_idx );
        }
        else if ( s == Playing || s == Paused )
        {
            msg_Dbg( p_stream, "Playback started: Current configuration (%u) "
                     "accepted", p_sys->transcode_attempt_idx );
        }
        p_sys->previous_state = s;
    }

    return sout_StreamIdSend(p_sys->p_out, id, p_buffer);
}

static void Flush( sout_stream_t *p_stream, sout_stream_id_sys_t *id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    id = p_sys->GetSubId( p_stream, id );
    if ( id == NULL )
        return;

    /* a seek on the Chromecast flushes its buffers */
    p_sys->p_intf->requestPlayerSeek( VLC_TS_INVALID );

    sout_StreamFlush( p_sys->p_out, id );
}

static int Control(sout_stream_t *p_stream, int i_query, va_list args)
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    if (i_query == SOUT_STREAM_EMPTY)
    {
        bool *b = va_arg( args, bool * );
        /* check if the Chromecast to be done playing */
        *b = p_sys->p_intf == NULL || p_sys->p_intf->isFinishedPlaying();
        return VLC_SUCCESS;
    }

    if ( !p_sys->p_out->pf_control )
        return VLC_EGENERIC;

    return p_sys->p_out->pf_control( p_sys->p_out, i_query, args );
}

/*****************************************************************************
 * Open: connect to the Chromecast and initialize the sout
 *****************************************************************************/
static int Open(vlc_object_t *p_this)
{
    sout_stream_t *p_stream = reinterpret_cast<sout_stream_t*>(p_this);
    sout_stream_sys_t *p_sys = NULL;
    intf_sys_t *p_intf = NULL;
    char *psz_ip = NULL;
    char *psz_mux = NULL;
    char *psz_var_mime = NULL;
    sout_stream_t *p_sout = NULL;
    bool b_supports_video = true;
    int i_local_server_port;
    int i_device_port;
    std::stringstream ss;

    vlc_interrupt_t *p_interrupt = vlc_interrupt_create();
    if (unlikely(p_interrupt == NULL))
        goto error;

    config_ChainParse(p_stream, SOUT_CFG_PREFIX, ppsz_sout_options, p_stream->p_cfg);

    psz_ip = var_GetNonEmptyString( p_stream, SOUT_CFG_PREFIX "ip");
    if ( psz_ip == NULL )
    {
        msg_Err( p_this, "missing Chromecast IP address" );
        goto error;
    }

    i_device_port = var_InheritInteger(p_stream, SOUT_CFG_PREFIX "port");
    i_local_server_port = var_InheritInteger(p_stream, SOUT_CFG_PREFIX "http-port");

    try
    {
        p_intf = new intf_sys_t( p_this, i_local_server_port, psz_ip, i_device_port, p_interrupt );
    }
    catch (const std::runtime_error& err )
    {
        msg_Err( p_this, "cannot load the Chromecast controller (%s)", err.what() );
        goto error;
    }
    catch (const std::bad_alloc& )
    {
        p_intf = NULL;
        goto error;
    }

    p_interrupt = NULL;

    psz_mux = var_GetNonEmptyString(p_stream, SOUT_CFG_PREFIX "mux");
    if (psz_mux == NULL)
    {
        goto error;
    }
    psz_var_mime = var_GetNonEmptyString(p_stream, SOUT_CFG_PREFIX "mime");
    if (psz_var_mime == NULL)
        goto error;

    /* check if we can open the proper sout */
    ss << "http{dst=:" << i_local_server_port << "/stream"
       << ",mux=" << psz_mux
       << ",access=http{mime=" << psz_var_mime << "}}";

    p_sout = sout_StreamChainNew( p_stream->p_sout, ss.str().c_str(), NULL, NULL);
    if (p_sout == NULL) {
        msg_Dbg(p_stream, "could not create sout chain:%s", ss.str().c_str());
        goto error;
    }
    sout_StreamChainDelete( p_sout, NULL );

    b_supports_video = var_GetBool(p_stream, SOUT_CFG_PREFIX "video");

    p_sys = new(std::nothrow) sout_stream_sys_t( p_intf, b_supports_video, i_local_server_port,
                                                 psz_mux, psz_var_mime );
    if (unlikely(p_sys == NULL))
        goto error;

    // Set the sout callbacks.
    p_stream->pf_add     = Add;
    p_stream->pf_del     = Del;
    p_stream->pf_send    = Send;
    p_stream->pf_flush   = Flush;
    p_stream->pf_control = Control;

    p_stream->p_sys = p_sys;
    free(psz_ip);
    free(psz_mux);
    free(psz_var_mime);
    return VLC_SUCCESS;

error:
    if (p_interrupt)
        vlc_interrupt_destroy(p_interrupt);
    delete p_intf;
    free(psz_ip);
    free(psz_mux);
    free(psz_var_mime);
    delete p_sys;
    return VLC_EGENERIC;
}

/*****************************************************************************
 * Close: destroy interface
 *****************************************************************************/
static void Close(vlc_object_t *p_this)
{
    sout_stream_t *p_stream = reinterpret_cast<sout_stream_t*>(p_this);

    delete p_stream->p_sys;
}

