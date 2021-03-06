/*****************************************************************************
 * opensles_android.c : audio output for android native code
 *****************************************************************************/
/*****************************************************************************
 * Porting From vlc
 *****************************************************************************/

#include "../dtaudio_android.h"
#include "dt_buffer.h"
#include "dt_lock.h"

#include <assert.h>
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <android/log.h>

// For native audio
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#define OPENSLES_BUFFERS 255 /* maximum number of buffers */
#define OPENSLES_BUFLEN  10   /* ms */
/*
 * 10ms of precision when mesasuring latency should be enough,
 * with 255 buffers we can buffer 2.55s of audio.
 */

#define CHECK_OPENSL_ERROR(msg)                \
    if (unlikely(result != SL_RESULT_SUCCESS)) \
    {                                          \
        goto error;                            \
    }

typedef SLresult (*slCreateEngine_t)(
        SLObjectItf *, SLuint32, const SLEngineOption *, SLuint32,
        const SLInterfaceID *, const SLboolean *);

#define Destroy(a) (*a)->Destroy(a);
#define SetPlayState(a, b) (*a)->SetPlayState(a, b)
#define RegisterCallback(a, b, c) (*a)->RegisterCallback(a, b, c)
#define GetInterface(a, b, c) (*a)->GetInterface(a, b, c)
#define Realize(a, b) (*a)->Realize(a, b)
#define CreateOutputMix(a, b, c, d, e) (*a)->CreateOutputMix(a, b, c, d, e)
#define CreateAudioPlayer(a, b, c, d, e, f, g) \
    (*a)->CreateAudioPlayer(a, b, c, d, e, f, g)
#define Enqueue(a, b, c) (*a)->Enqueue(a, b, c)
#define Clear(a) (*a)->Clear(a)
#define GetState(a, b) (*a)->GetState(a, b)
#define SetPositionUpdatePeriod(a, b) (*a)->SetPositionUpdatePeriod(a, b)
#define SetVolumeLevel(a, b) (*a)->SetVolumeLevel(a, b)
#define SetMute(a, b) (*a)->SetMute(a, b)


//From vlc
#define CLOCK_FREQ INT64_C(1000000)
#define AOUT_MAX_ADVANCE_TIME           (AOUT_MAX_PREPARE_TIME + CLOCK_FREQ)
#define AOUT_MAX_PREPARE_TIME           (2 * CLOCK_FREQ)
#define AOUT_MIN_PREPARE_TIME           AOUT_MAX_PTS_ADVANCE
#define AOUT_MAX_PTS_ADVANCE            (CLOCK_FREQ / 25)
#define AOUT_MAX_PTS_DELAY              (3 * CLOCK_FREQ / 50)
#define AOUT_MAX_RESAMPLING             10


/*****************************************************************************
 *
 *****************************************************************************/
typedef struct aout_sys_t {
    /* OpenSL objects */
    SLObjectItf engineObject;
    SLObjectItf outputMixObject;
    SLAndroidSimpleBufferQueueItf playerBufferQueue;
    SLObjectItf playerObject;
    SLVolumeItf volumeItf;
    SLEngineItf engineEngine;
    SLPlayItf playerPlay;

    /* OpenSL symbols */
    void *p_so_handle;

    slCreateEngine_t slCreateEnginePtr;
    SLInterfaceID SL_IID_ENGINE;
    SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
    SLInterfaceID SL_IID_VOLUME;
    SLInterfaceID SL_IID_PLAY;

    /* audio buffered through opensles */
    uint8_t *buf;
    int samples_per_buf;
    int next_buf;

    int rate;

    /* if we can measure latency already */
    int started;
    int samples;
    dt_buffer_t dbt;
    dt_lock_t lock;
} aout_sys_t;


//====================================
// dtap
//====================================

#ifdef ENABLE_DTAP

#include "dtap_api.h"
typedef struct{
    dtap_context_t ap;
    dt_lock_t lock;
}audio_effect_t;

#endif

#include "../native_log.h"

#define TAG "AO-OPENSL"

/*****************************************************************************
 *
 *****************************************************************************/

static inline int bytesPerSample(dtaudio_output_t *aout) {
    dtaudio_para_t *para = &aout->para;
    return para->dst_channels * para->data_width / 8;
    //return 2 /* S16 */ * 2 /* stereo */;
}

// get us delay
//
static int TimeGet(dtaudio_output_t *aout, int64_t *drift) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;

    SLAndroidSimpleBufferQueueState st;
    SLresult res = GetState(sys->playerBufferQueue, &st);
    if (unlikely(res != SL_RESULT_SUCCESS)) {
        LOGV("Could not query buffer queue state in TimeGet (%lu)", (unsigned long) res);
        return -1;
    }

    dt_lock(&sys->lock);
    bool started = sys->started;
    dt_unlock(&sys->lock);

    if (!started)
        return -1;
    *drift = (CLOCK_FREQ * OPENSLES_BUFLEN * st.count / 1000)
             + sys->samples * CLOCK_FREQ / sys->rate;

    //__android_log_print(ANDROID_LOG_DEBUG, TAG, "latency %lld ms, %d/%d buffers, samples:%d", *drift / 1000,
    //        (int)st.count, OPENSLES_BUFFERS, sys->samples);

    return 0;
}

static void Flush(dtaudio_output_t *aout, bool drain) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;

    if (drain) {
        int64_t delay;
        if (!TimeGet(aout, &delay))
            usleep(delay * 1000);
    } else {
        SetPlayState(sys->playerPlay, SL_PLAYSTATE_STOPPED);
        Clear(sys->playerBufferQueue);
        SetPlayState(sys->playerPlay, SL_PLAYSTATE_PLAYING);

        sys->samples = 0;
        sys->started = 0;
    }
}

#if 0
static int VolumeSet(dtaudio_output_t *aout, float vol)
{
    aout_sys_t *sys = (aout_sys_t)aout->ao_priv;
    if (!sys->volumeItf)
        return -1;

    /* Convert UI volume to linear factor (cube) */
    vol = vol * vol * vol;

    /* millibels from linear amplification */
    int mb = lroundf(2000.f * log10f(vol));
    if (mb < SL_MILLIBEL_MIN)
        mb = SL_MILLIBEL_MIN;
    else if (mb > 0)
        mb = 0; /* maximum supported level could be higher: GetMaxVolumeLevel */

    SLresult r = SetVolumeLevel(aout->sys->volumeItf, mb);
    return (r == SL_RESULT_SUCCESS) ? 0 : -1;
}

static int MuteSet(audio_output_t *aout, bool mute)
{
    if (!aout->sys->volumeItf)
        return -1;

    SLresult r = SetMute(aout->sys->volumeItf, mute);
    return (r == SL_RESULT_SUCCESS) ? 0 : -1;
}
#endif

static void Pause(dtaudio_output_t *aout, bool pause) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;
    SetPlayState(sys->playerPlay,
                 pause ? SL_PLAYSTATE_PAUSED : SL_PLAYSTATE_PLAYING);
}

static int WriteBuffer(dtaudio_output_t *aout) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;
    const int unit_size = sys->samples_per_buf * bytesPerSample(aout);

    /* Check if we can fill at least one buffer unit by chaining blocks */
    if (sys->dbt.level < unit_size) {
        return false;
    }

    SLAndroidSimpleBufferQueueState st;
    SLresult res = GetState(sys->playerBufferQueue, &st);
    if (unlikely(res != SL_RESULT_SUCCESS)) {
        return false;
    }

    if (st.count == OPENSLES_BUFFERS)
        return false;

    int done = 0;
    while (done < unit_size) {
        int cur = buf_level(&sys->dbt);
        if (cur > unit_size - done)
            cur = unit_size - done;

        //memcpy(&sys->buf[unit_size * sys->next_buf + done], b->p_buffer, cur);
        buf_get(&sys->dbt, &sys->buf[unit_size * sys->next_buf + done], cur);
        done += cur;

        if (done == unit_size)
            break;
    }

    SLresult r = Enqueue(sys->playerBufferQueue,
                         &sys->buf[unit_size * sys->next_buf], unit_size);

    sys->samples -= sys->samples_per_buf;
    //__android_log_print(ANDROID_LOG_DEBUG,TAG, "minus sampels, %d minus %d \n",sys->samples, sys->samples_per_buf);

    if (r == SL_RESULT_SUCCESS) {
        if (++sys->next_buf == OPENSLES_BUFFERS)
            sys->next_buf = 0;
        return true;
    } else {
        /* XXX : if writing fails, we don't retry */
        return false;
    }
}

/*****************************************************************************
 * Play: play a sound
 *****************************************************************************/
static int Play(dtaudio_output_t *aout, uint8_t *buf, int size) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;
    int ret = 0;
    //__android_log_print(ANDROID_LOG_DEBUG,TAG, "space:%d level:%d  size:%d  \n",buf_space(&sys->dbt), buf_level(&sys->dbt), size);
    if (buf_space(&sys->dbt) > size) {
        ret = buf_put(&sys->dbt, buf, size);
    }
    sys->samples += ret / bytesPerSample(aout);
    //__android_log_print(ANDROID_LOG_DEBUG,TAG, "add sampels, %d add %d \n",sys->samples, ret / bytesPerSample(aout));

    /* Fill OpenSL buffer */
    WriteBuffer(aout); // will read data in callback
    return ret;
}

static void PlayedCallback(SLAndroidSimpleBufferQueueItf caller, void *pContext) {
    (void) caller;
    dtaudio_output_t *aout = pContext;
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;

    assert (caller == sys->playerBufferQueue);
    sys->started = 1;
    //__android_log_print(ANDROID_LOG_DEBUG,TAG, "opensl callback called \n");

}

/*****************************************************************************
 *
 *****************************************************************************/

static SLuint32 convertSampleRate(SLuint32 sr) {
    switch (sr) {
        case 8000:
            return SL_SAMPLINGRATE_8;
        case 11025:
            return SL_SAMPLINGRATE_11_025;
        case 12000:
            return SL_SAMPLINGRATE_12;
        case 16000:
            return SL_SAMPLINGRATE_16;
        case 22050:
            return SL_SAMPLINGRATE_22_05;
        case 24000:
            return SL_SAMPLINGRATE_24;
        case 32000:
            return SL_SAMPLINGRATE_32;
        case 44100:
            return SL_SAMPLINGRATE_44_1;
        case 48000:
            return SL_SAMPLINGRATE_48;
        case 64000:
            return SL_SAMPLINGRATE_64;
        case 88200:
            return SL_SAMPLINGRATE_88_2;
        case 96000:
            return SL_SAMPLINGRATE_96;
        case 192000:
            return SL_SAMPLINGRATE_192;
    }
    return -1;
}

static int Start(dtaudio_output_t *aout) {
    SLresult result;

    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;
    dtaudio_para_t *para = &aout->para;

    // configure audio source - this defines the number of samples you can enqueue.
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
            OPENSLES_BUFFERS
    };

    int mask;

    if (para->dst_channels > 1)
        mask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
    else
        mask = SL_SPEAKER_FRONT_CENTER;


    SLDataFormat_PCM format_pcm;
    format_pcm.formatType = SL_DATAFORMAT_PCM;
    format_pcm.numChannels = para->dst_channels;
    //format_pcm.samplesPerSec    = ((SLuint32) para->dst_samplerate * 1000) ;
    format_pcm.samplesPerSec = ((SLuint32) convertSampleRate(para->dst_samplerate));
    format_pcm.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
    format_pcm.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
    format_pcm.channelMask = mask;
    format_pcm.endianness = SL_BYTEORDER_LITTLEENDIAN;

    SLDataSource audioSrc = {&loc_bufq, &format_pcm};

    // configure audio sink
    SLDataLocator_OutputMix loc_outmix = {
            SL_DATALOCATOR_OUTPUTMIX,
            sys->outputMixObject
    };
    SLDataSink audioSnk = {&loc_outmix, NULL};

    //create audio player
    const SLInterfaceID ids2[] = {sys->SL_IID_ANDROIDSIMPLEBUFFERQUEUE, sys->SL_IID_VOLUME};
    static const SLboolean req2[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    result = CreateAudioPlayer(sys->engineEngine, &sys->playerObject, &audioSrc,
                               &audioSnk, sizeof(ids2) / sizeof(*ids2),
                               ids2, req2);
    if (unlikely(result != SL_RESULT_SUCCESS)) { // error
        return -1;
        /* Try again with a more sensible samplerate */
#if 0
        fmt->i_rate = 44100;
        format_pcm.samplesPerSec = ((SLuint32) 44100 * 1000) ;
        result = CreateAudioPlayer(sys->engineEngine, &sys->playerObject, &audioSrc,
                &audioSnk, sizeof(ids2) / sizeof(*ids2),
                ids2, req2);
#endif
    }
    CHECK_OPENSL_ERROR("Failed to create audio player");

    result = Realize(sys->playerObject, SL_BOOLEAN_FALSE);
    CHECK_OPENSL_ERROR("Failed to realize player object.");

    result = GetInterface(sys->playerObject, sys->SL_IID_PLAY, &sys->playerPlay);
    CHECK_OPENSL_ERROR("Failed to get player interface.");

    result = GetInterface(sys->playerObject, sys->SL_IID_VOLUME, &sys->volumeItf);
    CHECK_OPENSL_ERROR("failed to get volume interface.");

    result = GetInterface(sys->playerObject, sys->SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                          &sys->playerBufferQueue);
    CHECK_OPENSL_ERROR("Failed to get buff queue interface");

    result = RegisterCallback(sys->playerBufferQueue, PlayedCallback,
                              (void *) aout);
    CHECK_OPENSL_ERROR("Failed to register buff queue callback.");

    // set the player's state to playing
    result = SetPlayState(sys->playerPlay, SL_PLAYSTATE_PLAYING);
    CHECK_OPENSL_ERROR("Failed to switch to playing state");

    /* XXX: rounding shouldn't affect us at normal sampling rate */
    sys->rate = para->dst_samplerate;
    sys->samples_per_buf = OPENSLES_BUFLEN * para->dst_samplerate / 1000;
    sys->buf = malloc(OPENSLES_BUFFERS * sys->samples_per_buf * bytesPerSample(aout));
    if (!sys->buf)
        goto error;

    sys->started = 0;
    sys->next_buf = 0;

    sys->samples = 0;
    SetPositionUpdatePeriod(sys->playerPlay, AOUT_MIN_PREPARE_TIME * 1000 / CLOCK_FREQ);
    return 0;

    error:
    if (sys->playerObject) {
        Destroy(sys->playerObject);
        sys->playerObject = NULL;
    }

    return -1;
}

static void Stop(dtaudio_output_t *aout) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;

    SetPlayState(sys->playerPlay, SL_PLAYSTATE_STOPPED);
    //Flush remaining buffers if any.
    Clear(sys->playerBufferQueue);

    free(sys->buf);

    Destroy(sys->playerObject);
    sys->playerObject = NULL;
    free(sys);
    sys = NULL;
}

/*****************************************************************************
 *
 *****************************************************************************/
static void Close(dtaudio_output_t *aout) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;

    Destroy(sys->outputMixObject);
    Destroy(sys->engineObject);
    dlclose(sys->p_so_handle);
    //vlc_mutex_destroy(&sys->lock);
    free(sys);
}

static int Open(dtaudio_output_t *aout) {
    aout_sys_t *sys;
    SLresult result;

    dtaudio_para_t *para = &aout->para;
    sys = (aout_sys_t *) malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
        return -1;

    sys->p_so_handle = dlopen("libOpenSLES.so", RTLD_NOW);
    if (sys->p_so_handle == NULL) {
        goto error;
    }

    sys->slCreateEnginePtr = dlsym(sys->p_so_handle, "slCreateEngine");
    if (unlikely(sys->slCreateEnginePtr == NULL)) {
        goto error;
    }

#define OPENSL_DLSYM(dest, name)                       \
    do {                                                       \
        const SLInterfaceID *sym = dlsym(sys->p_so_handle, "SL_IID_"name);        \
        if (unlikely(sym == NULL))                             \
        {                                                      \
            goto error;                                        \
        }                                                      \
        sys->dest = *sym;                                           \
    } while(0)

    OPENSL_DLSYM(SL_IID_ANDROIDSIMPLEBUFFERQUEUE, "ANDROIDSIMPLEBUFFERQUEUE");
    OPENSL_DLSYM(SL_IID_ENGINE, "ENGINE");
    OPENSL_DLSYM(SL_IID_PLAY, "PLAY");
    OPENSL_DLSYM(SL_IID_VOLUME, "VOLUME");
#undef OPENSL_DLSYM

    // create engine
    result = sys->slCreateEnginePtr(&sys->engineObject, 0, NULL, 0, NULL, NULL);
    CHECK_OPENSL_ERROR("Failed to create engine");

    // realize the engine in synchronous mode
    result = Realize(sys->engineObject, SL_BOOLEAN_FALSE);
    CHECK_OPENSL_ERROR("Failed to realize engine");

    // get the engine interface, needed to create other objects
    result = GetInterface(sys->engineObject, sys->SL_IID_ENGINE, &sys->engineEngine);
    CHECK_OPENSL_ERROR("Failed to get the engine interface");

    // create output mix, with environmental reverb specified as a non-required interface
    const SLInterfaceID ids1[] = {sys->SL_IID_VOLUME};
    const SLboolean req1[] = {SL_BOOLEAN_FALSE};
    result = CreateOutputMix(sys->engineEngine, &sys->outputMixObject, 1, ids1, req1);
    CHECK_OPENSL_ERROR("Failed to create output mix");

    // realize the output mix in synchronous mode
    result = Realize(sys->outputMixObject, SL_BOOLEAN_FALSE);
    CHECK_OPENSL_ERROR("Failed to realize output mix");

    dt_lock_init(&sys->lock, NULL);

    if (buf_init(&sys->dbt, para->dst_samplerate * 4 / 10) < 0) // 100ms
        return -1;
    aout->ao_priv = (void *) sys;
    return 0;

    error:
    if (sys->outputMixObject)
        Destroy(sys->outputMixObject);
    if (sys->engineObject)
        Destroy(sys->engineObject);
    if (sys->p_so_handle)
        dlclose(sys->p_so_handle);
    free(sys);
    return -1;
}

#ifdef ENABLE_DTAP
int dtap_change_effect(ao_wrapper_t *wrapper, int id)
{
    audio_effect_t *ae = (audio_effect_t *)wrapper->ao_priv;
    __android_log_print(ANDROID_LOG_INFO, TAG, "change audio effect from: %d to %d \n", ae->ap.para.item, id);
    dt_lock(&ae->lock);
    ae->ap.para.item = id;
    dtap_update(&ae->ap);
    dtap_init(&ae->ap);
    dt_unlock(&ae->lock);
    return 0;
}
#endif

static int ao_opensl_init(dtaudio_output_t *aout, dtaudio_para_t *para) {
    if (Open(aout) == -1)
        return -1;
    Start(aout);

#ifdef ENABLE_DTAP
    ao_wrapper_t *wrapper = aout->wrapper;
    audio_effect_t *ae = (audio_effect_t *)malloc(sizeof(audio_effect_t));
    wrapper->ao_priv = ae;
    ae->ap.para.samplerate = para->samplerate;
    ae->ap.para.channels = para->channels;
    ae->ap.para.data_width = para->data_width;
    ae->ap.para.type = DTAP_EFFECT_EQ;
    ae->ap.para.item = EQ_EFFECT_NORMAL;
    dtap_init(&ae->ap);
    dt_lock_init(&ae->lock, NULL);
#endif
    return 0;
}

static int ao_opensl_write(dtaudio_output_t *aout, uint8_t *buf, int size) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;
    ao_wrapper_t *wrapper = aout->wrapper;
    int ret = 0;

#ifdef ENABLE_DTAP
    audio_effect_t *ae = (audio_effect_t *)wrapper->ao_priv;
    dt_lock(&ae->lock);
    dtap_frame_t frame;
    frame.in = buf;
    frame.in_size = size;
    if(ae->ap.para.item != EQ_EFFECT_NORMAL)
    {
        dtap_process(&ae->ap, &frame);
    }
    dt_unlock(&ae->lock);
#endif

    dt_lock(&sys->lock);
    ret = Play(aout, buf, size);
    dt_unlock(&sys->lock);
    return ret;
}

static int ao_opensl_pause(dtaudio_output_t *aout) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;
    dt_lock(&sys->lock);
    Pause(aout, 1);
    dt_unlock(&sys->lock);
    return 0;
}

static int ao_opensl_resume(dtaudio_output_t *aout) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;
    dt_lock(&sys->lock);
    Pause(aout, 0);
    dt_unlock(&sys->lock);
    return 0;
}

static int ao_opensl_level(dtaudio_output_t *aout) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;
    dt_lock(&sys->lock);
    int level = sys->samples * bytesPerSample(aout);
    const int unit_size = sys->samples_per_buf * bytesPerSample(aout);
    SLAndroidSimpleBufferQueueState st;
    if (!sys->started)
        goto END;
    SLresult res = GetState(sys->playerBufferQueue, &st);
    if (unlikely(res != SL_RESULT_SUCCESS)) {
        goto END;
    }
    level += st.count * unit_size;
    //__android_log_print(ANDROID_LOG_DEBUG,TAG, "opensl level:%d  st.count:%d sample:%d:%d \n",level, (int)st.count, sys->samples);
    END:
    dt_unlock(&sys->lock);
    return level;
}

static int64_t ao_opensl_get_latency(dtaudio_output_t *aout) {
    int64_t latency;
    int ret = 0;
    int level = 0;
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;

#if 1
    TimeGet(aout, &latency);
    if (latency == -1)
        return 0;
    latency = 9 * latency / 100;
#else
    dtaudio_para_t *para = &aout->para;
    level = ao_opensl_level(aout);
    int sample_num;
    float pts_ratio = 0.0;
    pts_ratio = (double) 90000 / para->dst_samplerate;
    sample_num = level / bytesPerSample(aout);
    latency += (sample_num * pts_ratio);
#endif
    //__android_log_print(ANDROID_LOG_DEBUG,TAG, "opensl latency, level:%d latency:%lld \n",level, latency);
    return latency;
}

static int ao_opensl_stop(dtaudio_output_t *aout) {
    aout_sys_t *sys = (aout_sys_t *) aout->ao_priv;
    ao_wrapper_t *wrapper = aout->wrapper;
#ifdef ENABLE_DTAP
    audio_effect_t *ae = (audio_effect_t *)wrapper->ao_priv;
    dt_lock(&ae->lock);
    memset(&ae->ap, 0 , sizeof(dtap_context_t));
    dtap_release(&ae->ap);    
    dt_unlock(&ae->lock);
#endif

    dt_lock(&sys->lock);
    Stop(aout);
    dt_unlock(&sys->lock);
    return 0;
}

static int ao_opensl_get_volume(ao_wrapper_t *ao) {
    return 0;
}

static int ao_opensl_set_volume(ao_wrapper_t *ao, int value) {
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "opensl setvolume %d \n", value);
    return 0;
}

const char *ao_opensl_name = "OPENSL AO";

void ao_opensl_setup(ao_wrapper_t *ao) {
    if (!ao) return;
    ao->id = AO_ID_OPENSL;
    ao->name = ao_opensl_name;
    ao->ao_init = ao_opensl_init;
    ao->ao_pause = ao_opensl_pause;
    ao->ao_resume = ao_opensl_resume;
    ao->ao_stop = ao_opensl_stop;
    ao->ao_write = ao_opensl_write;
    ao->ao_level = ao_opensl_level;
    ao->ao_latency = ao_opensl_get_latency;
    ao->ao_get_volume = ao_opensl_get_volume;
    ao->ao_set_volume = ao_opensl_set_volume;
    return;
}

ao_wrapper_t ao_opensl_ops = {
        .id = AO_ID_OPENSL,
        .name = "opensl es",
        .ao_init = ao_opensl_init,
        .ao_pause = ao_opensl_pause,
        .ao_resume = ao_opensl_resume,
        .ao_stop = ao_opensl_stop,
        .ao_write = ao_opensl_write,
        .ao_level = ao_opensl_level,
        .ao_latency = ao_opensl_get_latency,
};
