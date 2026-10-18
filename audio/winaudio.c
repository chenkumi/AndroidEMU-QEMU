/*
 * QEMU "simple" Windows audio driver
 *
 * Copyright (c) 2007 The Android Open Source Project
 * Copyright (c) 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "sysemu/sysemu.h"
#include "audio.h"

#define AUDIO_CAP "winaudio"
#include "audio_int.h"


#include <windows.h>
#include <mmsystem.h>
#include "audio_win_int.h"

/* define DEBUG to 1 to dump audio debugging info at runtime to stderr */
#define  DEBUG  0

#if 1
#  define  D_ACTIVE    1
#else
#  define  D_ACTIVE  DEBUG
#endif

#if DEBUG
#  define  D(...)   do{ if (D_ACTIVE) printf(__VA_ARGS__); } while(0)
#else
#  define  D(...)   ((void)0)
#endif

static struct {
    int nb_samples;
} conf = {
    1024
};

#if DEBUG
int64_t  start_time;
int64_t  last_time;
#endif

#define  NUM_OUT_BUFFERS  8  /* must be at least 2 */

/** COMMON UTILITIES
 **/

#if 1
static void
dump_mmerror( const char*  func, MMRESULT  error )
{
    const char*  reason = NULL;

    fprintf(stderr, "%s returned error: ", func);
    switch (error) {
            case MMSYSERR_ALLOCATED:   reason="specified resource is already allocated"; break;
            case MMSYSERR_BADDEVICEID: reason="bad device id"; break;
            case MMSYSERR_NODRIVER:    reason="no driver is present"; break;
            case MMSYSERR_NOMEM:       reason="unable to allocate or lock memory"; break;
            case WAVERR_BADFORMAT:     reason="unsupported waveform-audio format"; break;
            case WAVERR_SYNC:          reason="device is synchronous"; break;
            default:
                    fprintf(stderr, "unknown(%d)\n", error);
    }
    if (reason)
            fprintf(stderr, "%s\n", reason);
}
#else
#  define  dump_mmerror(func,error)  ((void)0)
#endif


/** AUDIO OUT
 **/

typedef struct WinAudioOutDevice {
    HWAVEOUT          waveout;
    unsigned char*    buffer_bytes;
    WAVEHDR           buffers[ NUM_OUT_BUFFERS ];
    bool              retired;
} WinAudioOutDevice;

typedef struct WinAudioOut {
    HWVoiceOut        hw;
    WinAudioOutDevice* device;
    int               silence;
    int               write_errors;
    int               write_index;   /* starting first writable buffer      */
    int               write_pos;     /* position in current writable buffer */
    int               write_size;    /* size in bytes of each buffer        */
} WinAudioOut;

/* Output device ownership is serialized by the audio core, not a callback.
 * One slot bounds retained resources even if a driver permanently rejects close.
 * Its headers/PCM must survive audio_template.h freeing the HWVoiceOut. */
static WinAudioOutDevice* winaudio_out_device;

static bool
winaudio_out_can_write (WinAudioOut *s)
{
    if (!s->device) {
        return false;
    }
    /* WinMM owns dwFlags while queued. Poll on the existing audio timer; never
     * overwrite the current header even if another header completed first. */
    return !(*(volatile DWORD *)&s->device->buffers[s->write_index].dwFlags &
             WHDR_INQUEUE);
}

static bool
winaudio_out_release_device (WinAudioOutDevice *device)
{
    int i;
    MMRESULT result;
    if (device->waveout) {
        result = waveOutReset(device->waveout);
        if (result != MMSYSERR_NOERROR) {
            dump_mmerror("waveOutReset()", result);
            return false;
        }
        for (i = 0; i < NUM_OUT_BUFFERS; ++i) {
            if (device->buffers[i].dwFlags & WHDR_PREPARED) {
                result = waveOutUnprepareHeader(device->waveout,
                    &device->buffers[i], sizeof(device->buffers[i]));
                if (result != MMSYSERR_NOERROR) {
                    dump_mmerror("waveOutUnprepareHeader()", result);
                    return false;
                }
            }
        }
        result = waveOutClose(device->waveout);
        if (result != MMSYSERR_NOERROR) {
            dump_mmerror("waveOutClose()", result);
            return false;
        }
        device->waveout = NULL;
    }
    g_free(device->buffer_bytes);
    g_free(device);
    return true;
}

static int
winaudio_out_write (SWVoiceOut *sw, void *buf, int len)
{
    return audio_pcm_sw_write (sw, buf, len);
}

static void
winaudio_out_fini (HWVoiceOut *hw)
{
    WinAudioOut *s = (WinAudioOut *)hw;
    WinAudioOutDevice *device = s->device;
    s->device = NULL;
    if (!device) {
        return;
    }
    device->retired = true;
    if (winaudio_out_release_device(device)) {
        winaudio_out_device = NULL;
    }
}


static int
winaudio_out_init (HWVoiceOut *hw, struct audsettings *as, void* drv_opaque)
{
    WinAudioOut*   s = (WinAudioOut*) hw;
    MMRESULT       result;
    WAVEFORMATEX   format;
    int            i, samples_size;
    WinAudioOutDevice *device;

    s->device = NULL;
    if (winaudio_out_device) {
        if (!winaudio_out_device->retired ||
            !winaudio_out_release_device(winaudio_out_device)) {
            AUD_log(AUDIO_CAP, "waveOut device is still owned; refusing another allocation\n");
            return -1;
        }
        winaudio_out_device = NULL;
    }

    if (conf.nb_samples <= 0 || conf.nb_samples > 65536 ||
        (as->fmt != AUD_FMT_U8 && as->fmt != AUD_FMT_S16 &&
         as->fmt != AUD_FMT_S32) ||
        waveformat_from_audio_settings(&format, as)) {
        return -1;
    }
    s->silence = as->fmt == AUD_FMT_U8 ? 0x80 : 0;
    device = g_malloc0(sizeof(*device));
    if (!device) {
        return -1;
    }
    winaudio_out_device = s->device = device;

    /* The audio core already polls run_out. No callback may retain the voice. */
    result = waveOutOpen(&device->waveout, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    if ( result != MMSYSERR_NOERROR ) {
        dump_mmerror( "qemu: winaudio: waveOutOpen()", result);
            goto fail;
    }

    samples_size    = format.nBlockAlign * conf.nb_samples;
    device->buffer_bytes = g_malloc( NUM_OUT_BUFFERS * samples_size );
    if (device->buffer_bytes == NULL) {
            fprintf(stderr, "not enough memory for Windows audio buffers\n");
            goto fail;
    }

    for (i = 0; i < NUM_OUT_BUFFERS; i++) {
        device->buffers[i].lpData = (LPSTR)(device->buffer_bytes + i*samples_size);
        device->buffers[i].dwBufferLength = samples_size;

        result = waveOutPrepareHeader(device->waveout, &device->buffers[i],
                                      sizeof(device->buffers[i]));
        if ( result != MMSYSERR_NOERROR ) {
            dump_mmerror("waveOutPrepareHeader()", result);
            goto fail;
        }
    }

#if DEBUG
    /* Check the sound device we retrieved */
    {
        WAVEOUTCAPS caps;

        result = waveOutGetDevCaps((UINT_PTR) device->waveout, &caps, sizeof(caps));
        if ( result != MMSYSERR_NOERROR ) {
            dump_mmerror("waveOutGetDevCaps()", result);
        } else
            printf("Audio out device: %s\n", caps.szPname);
    }
#endif

    audio_pcm_init_info (&hw->info, as);
    hw->samples = conf.nb_samples*2;

    s->write_index = 0;
    s->write_pos   = 0;
    s->write_size  = samples_size;
    AUD_log(AUDIO_CAP, "waveOut initialized rate=%lu channels=%u bits=%u buffers=%d frames=%d\n",
            format.nSamplesPerSec, format.nChannels, format.wBitsPerSample,
            NUM_OUT_BUFFERS, conf.nb_samples);
    fflush(stderr);
    return 0;
fail:
    winaudio_out_fini(hw);
    return -1;
}


static int
winaudio_out_run (HWVoiceOut *hw, int live)
{
    WinAudioOut*  s      = (WinAudioOut*) hw;
    int           played = 0;
    int           buffer_new_samples = 0;
    int           has_buffer;

    if (!live) {
        return 0;
    }

    has_buffer = winaudio_out_can_write(s);

    if (has_buffer) {
        while (live > 0) {
            WAVEHDR*      wav_buffer  = s->device->buffers + s->write_index;
            int           wav_bytes   = (s->write_size - s->write_pos);
            int           wav_samples = audio_MIN(wav_bytes >> hw->info.shift, live);
            int           hw_samples  = audio_MIN(hw->samples - hw->rpos, live);
            struct st_sample*  src         = hw->mix_buf + hw->rpos;
            uint8_t*      dst         = (uint8_t*)wav_buffer->lpData + s->write_pos;

            if (wav_samples > hw_samples) {
                    wav_samples = hw_samples;
            }

            wav_bytes = wav_samples << hw->info.shift;

            //D("run_out: buffer:%d pos:%d size:%d wsamples:%d wbytes:%d live:%d rpos:%d hwsamples:%d\n", s->write_index,
            //   s->write_pos, s->write_size, wav_samples, wav_bytes, live, hw->rpos, hw->samples);
            hw->clip (dst, src, wav_samples);
            hw->rpos += wav_samples;
            if (hw->rpos >= hw->samples)
                    hw->rpos -= hw->samples;

            live         -= wav_samples;
            played       += wav_samples;
            buffer_new_samples += wav_samples;
            s->write_pos += wav_bytes;
            if (s->write_pos == s->write_size) {
                MMRESULT result;
                result = waveOutWrite(s->device->waveout, wav_buffer, sizeof(*wav_buffer));
                if (result != MMSYSERR_NOERROR) {
                    if (s->write_errors < 4) {
                        ++s->write_errors;
                        dump_mmerror("waveOutWrite()", result);
                    }
                    /* Keep rejected PCM live in the core for the next tick.
                     * Earlier calls' partial prefix and accepted blocks stay. */
                    s->write_pos -= buffer_new_samples << hw->info.shift;
                    hw->rpos -= buffer_new_samples;
                    if (hw->rpos < 0) {
                        hw->rpos += hw->samples;
                    }
                    played -= buffer_new_samples;
                    return played;
                }
                buffer_new_samples = 0;
                s->write_pos    = 0;
                s->write_index += 1;
                if (s->write_index == NUM_OUT_BUFFERS)
                    s->write_index = 0;

                if (!winaudio_out_can_write(s)) {
                        live = 0;
                }
            }
        }

    }
    return played;
}

static int
winaudio_out_ctl (HWVoiceOut *hw, int cmd, ...)
{
    WinAudioOut*  s = (WinAudioOut*) hw;
    if (!s->device) {
        return 0;
    }

    switch (cmd) {
    case VOICE_ENABLE:
        waveOutRestart(s->device->waveout);
        break;

    case VOICE_DISABLE:
        waveOutPause(s->device->waveout);
        break;
    }
    return 0;
}

/** AUDIO IN
 **/

#define  NUM_IN_BUFFERS  2

typedef struct WinAudioIn {
    HWVoiceIn         hw;
    HWAVEIN           wavein;
    CRITICAL_SECTION  lock;
    unsigned char*    buffer_bytes;
    WAVEHDR           buffers[ NUM_IN_BUFFERS ];
    int               read_index;
    int               read_count;
    int               read_pos;
    int               read_size;
} WinAudioIn;

/* The Win32 callback that is called when a buffer has finished playing */
static void CALLBACK
winaudio_in_buffer_done (HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance,
                         DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    WinAudioIn*  s = (WinAudioIn*) dwInstance;

    /* Only service "buffer done playing" messages */
    if ( uMsg != WIM_DATA )
        return;

    /* Signal that we are done playing a buffer */
    EnterCriticalSection( &s->lock );
    if (s->read_count < NUM_IN_BUFFERS)
        s->read_count += 1;
        //D(".%c",s->read_count + '0'); fflush(stdout);
    LeaveCriticalSection( &s->lock );
}

static void
winaudio_in_fini (HWVoiceIn *hw)
{
    WinAudioIn*  s = (WinAudioIn*) hw;
    int           i;

    if (s->wavein) {
        waveInReset(s->wavein);
            s->wavein = 0;
    }

    for ( i=0; i<NUM_IN_BUFFERS; ++i ) {
        if ( s->buffers[i].dwUser != 0xFFFF ) {
            waveInUnprepareHeader(
                s->wavein, &s->buffers[i], sizeof(s->buffers[i]) );
                s->buffers[i].dwUser = 0xFFFF;
        }
    }

    if (s->buffer_bytes != NULL) {
        g_free(s->buffer_bytes);
        s->buffer_bytes = NULL;
    }

    if (s->wavein) {
        waveInClose(s->wavein);
        s->wavein = NULL;
    }
}


static int
winaudio_in_init (HWVoiceIn *hw, struct audsettings *as, void* drv_opaque)
{
    WinAudioIn*   s = (WinAudioIn*) hw;
    MMRESULT       result;
    WAVEFORMATEX   format;
    int            shift, i, samples_size;

    s->wavein = NULL;
    InitializeCriticalSection( &s->lock );
    for (i = 0; i < NUM_IN_BUFFERS; i++) {
            s->buffers[i].dwUser = 0xFFFF;
    }
    s->buffer_bytes = NULL;

    /* compute desired wave input format */
    format.wFormatTag      = WAVE_FORMAT_PCM;
    format.nChannels       = as->nchannels;
    format.nSamplesPerSec  = as->freq;
    format.nAvgBytesPerSec = as->freq*as->nchannels;

    switch (as->fmt) {
        case AUD_FMT_S8:   shift = 0; break;
        case AUD_FMT_U8:   shift = 0; break;
        case AUD_FMT_S16:  shift = 1; break;
        case AUD_FMT_U16:  shift = 1; break;
        default:
            fprintf(stderr, "qemu: winaudio: Bad input audio format: %d\n",
                    as->fmt);
                return -1;
    }

    format.nAvgBytesPerSec = (format.nSamplesPerSec * format.nChannels) << shift;
    format.nBlockAlign     = format.nChannels << shift;
    format.wBitsPerSample  = 8 << shift;
    format.cbSize          = 0;

    /* open the wave in device */
    result = waveInOpen( &s->wavein, WAVE_MAPPER, &format,
                         (DWORD_PTR)winaudio_in_buffer_done, (DWORD_PTR) hw,
                         CALLBACK_FUNCTION);
    if ( result != MMSYSERR_NOERROR ) {
        dump_mmerror( "qemu: winaudio: waveInOpen()", result);
            return -1;
    }

    samples_size    = format.nBlockAlign * conf.nb_samples;
    s->buffer_bytes = g_malloc( NUM_IN_BUFFERS * samples_size );
    if (s->buffer_bytes == NULL) {
            waveInClose( s->wavein );
            s->wavein = NULL;
            fprintf(stderr, "not enough memory for Windows audio buffers\n");
            return -1;
    }

    for (i = 0; i < NUM_IN_BUFFERS; i++) {
        memset( &s->buffers[i], 0, sizeof(s->buffers[i]) );
        s->buffers[i].lpData         = (LPSTR)(s->buffer_bytes + i*samples_size);
        s->buffers[i].dwBufferLength = samples_size;
        s->buffers[i].dwFlags        = WHDR_DONE;

        result = waveInPrepareHeader( s->wavein, &s->buffers[i],
                               sizeof(s->buffers[i]) );
        if ( result != MMSYSERR_NOERROR ) {
                dump_mmerror("waveInPrepareHeader()", result);
                return -1;
        }

        result = waveInAddBuffer( s->wavein, &s->buffers[i],
                              sizeof(s->buffers[i]) );
        if ( result != MMSYSERR_NOERROR ) {
            dump_mmerror("waveInAddBuffer()", result);
            return -1;
        }
    }

#if DEBUG
    /* Check the sound device we retrieved */
    {
        WAVEINCAPS caps;

        result = waveInGetDevCaps((UINT) s->wavein, &caps, sizeof(caps));
        if ( result != MMSYSERR_NOERROR ) {
            dump_mmerror("waveInGetDevCaps()", result);
        } else
            printf("Audio in device: %s\n", caps.szPname);
    }
#endif

    audio_pcm_init_info (&hw->info, as);
    hw->samples = conf.nb_samples*2;

    s->read_index = 0;
    s->read_count = 0;
    s->read_pos   = 0;
    s->read_size  = samples_size;
    return 0;
}


/* report the number of captured samples to the audio subsystem */
static int
winaudio_in_run (HWVoiceIn *hw)
{
    WinAudioIn*  s        = (WinAudioIn*) hw;
    int          captured = 0;
    int          has_buffer;
    int          live = hw->samples - hw->total_samples_captured;

    if (!live) {
#if 0
        static int  counter;
        if (++counter == 100) {
            D("0"); fflush(stdout);
            counter = 0;
        }
#endif
            return 0;
    }

    EnterCriticalSection( &s->lock );
    has_buffer = (s->read_count > 0);
    LeaveCriticalSection( &s->lock );

    if (has_buffer > 0) {
        while (live > 0) {
            WAVEHDR*      wav_buffer  = s->buffers + s->read_index;
            int           wav_bytes   = (s->read_size - s->read_pos);
            int           wav_samples = audio_MIN(wav_bytes >> hw->info.shift, live);
            int           hw_samples  = audio_MIN(hw->samples - hw->wpos, live);
            struct st_sample*  dst    = hw->conv_buf + hw->wpos;
            uint8_t*      src         = (uint8_t*)wav_buffer->lpData + s->read_pos;

            if (wav_samples > hw_samples) {
                wav_samples = hw_samples;
            }

            wav_bytes = wav_samples << hw->info.shift;

            D("%s: buffer:%d pos:%d size:%d wsamples:%d wbytes:%d live:%d wpos:%d hwsamples:%d\n",
               __FUNCTION__, s->read_index, s->read_pos, s->read_size, wav_samples, wav_bytes, live,
               hw->wpos, hw->samples);

            hw->conv(dst, src, wav_samples);

            hw->wpos += wav_samples;
            if (hw->wpos >= hw->samples)
                hw->wpos -= hw->samples;

            live        -= wav_samples;
            captured    += wav_samples;
            s->read_pos += wav_bytes;
            if (s->read_pos == s->read_size) {
                s->read_pos    = 0;
                s->read_index += 1;
                if (s->read_index == NUM_IN_BUFFERS)
                    s->read_index = 0;

                waveInAddBuffer( s->wavein, wav_buffer, sizeof(*wav_buffer) );

                EnterCriticalSection( &s->lock );
                if (--s->read_count == 0) {
                    live = 0;
                }
                LeaveCriticalSection( &s->lock );
            }
        }
    }
    return  captured;
}


static int
winaudio_in_read (SWVoiceIn *sw, void *buf, int len)
{
    int  ret = audio_pcm_sw_read (sw, buf, len);
    if (ret > 0)
        D("%s: (%d) returned %d\n", __FUNCTION__, len, ret);
    return ret;
}


static int
winaudio_in_ctl (HWVoiceIn *hw, int cmd, ...)
{
	WinAudioIn*  s = (WinAudioIn*) hw;

    switch (cmd) {
    case VOICE_ENABLE:
        D("%s: enable audio in\n", __FUNCTION__);
        waveInStart( s->wavein );
        break;

    case VOICE_DISABLE:
        D("%s: disable audio in\n", __FUNCTION__);
        waveInStop( s->wavein );
        break;
    }
    return 0;
}

/** AUDIO STATE
 **/

typedef struct WinAudioState {
    int  dummy;
} WinAudioState;

static WinAudioState  g_winaudio;

static void*
winaudio_init(void)
{
    WinAudioState*  s = &g_winaudio;

#if DEBUG
    start_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    last_time  = 0;
#endif

    return s;
}


static void
winaudio_fini (void *opaque)
{
    if (winaudio_out_device && winaudio_out_device->retired) {
        if (winaudio_out_release_device(winaudio_out_device)) {
            winaudio_out_device = NULL;
        } else {
            AUD_log(AUDIO_CAP, "waveOut driver retains one device until recovery or process exit\n");
        }
    }
}

static struct audio_option winaudio_options[] = {
    {"SAMPLES", AUD_OPT_INT, &conf.nb_samples,
     "Size of Windows audio buffer in samples", NULL, 0},
    {NULL, 0, NULL, NULL, NULL, 0}
};

static struct audio_pcm_ops winaudio_pcm_ops = {
    winaudio_out_init,
    winaudio_out_fini,
    winaudio_out_run,
    winaudio_out_write,
    winaudio_out_ctl,

    winaudio_in_init,
    winaudio_in_fini,
    winaudio_in_run,
    winaudio_in_read,
    winaudio_in_ctl
};

static struct audio_driver winaudio_audio_driver = {
    .name           = "winaudio",
    .descr          = "Windows wave audio",
    .options        = winaudio_options,
    .init           = winaudio_init,
    .fini           = winaudio_fini,
    .pcm_ops        = &winaudio_pcm_ops,
    .can_be_default = 0,
    .max_voices_out = 1,
    .max_voices_in  = 0,
    .voice_size_out = sizeof (WinAudioOut),
    .voice_size_in  = 0
};

/* Explicit candidate only; preserve the normal DirectSound default. */
static void register_audio_winaudio(void)
{
    audio_driver_register(&winaudio_audio_driver);
}
type_init(register_audio_winaudio);
