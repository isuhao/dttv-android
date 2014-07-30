// dtp_api.cpp
// TOP-LEVEL API provided for DtPlayer.java

const static int MEDIA_PREPARED = 1;
const static int MEDIA_PLAYBACK_COMPLETE = 2;
const static int MEDIA_BUFFERING_UPDATE = 3;
const static int MEDIA_SEEK_COMPLETE = 4;
const static int MEDIA_SET_VIDEO_SIZE = 5;
const static int MEDIA_ERROR = 100;
const static int MEDIA_INFO = 200;
const static int MEDIA_CACHE = 300;
const static int MEDIA_HW_ERROR = 400;
const static int MEDIA_TIMED_TEXT = 1000;
const static int MEDIA_CACHING_UPDATE = 2000;

#include "dtp_native_api.h"

extern "C"{
#include <android/log.h>
#include <stdlib.h>

#include <GLES/gl.h>
#include <GLES/glext.h>

#include "render_android.h"
#include "dt_lock.h"

#define DEBUG_TAG "DTP-API"
}

namespace android {

DTPlayer::DTPlayer()
    :status(0),
     mCurrentPosition(-1),
     mDuration(-1),
     mDtpHandle(NULL),
     mDisplayHeight(0),
     mDisplayWidth(0)
{
    memset(&media_info,0,sizeof(dt_media_info_t));
    __android_log_print(ANDROID_LOG_DEBUG, DEBUG_TAG, "DTPLAYER Constructor called \n");
}

DTPlayer::~DTPlayer()
{
    __android_log_print(ANDROID_LOG_DEBUG, DEBUG_TAG, "DTPLAYER Destructor called \n");
}

int DTPlayer::setDataSource(const char *file_name)
{
    render_init();
    dtplayer_para_t para;
    para.no_audio = para.no_video = para.no_sub = -1;
	para.height = para.width = -1;
	para.loop_mode = 0;
	para.audio_index = para.video_index = para.sub_index = -1;
	para.update_cb = NULL;
	para.sync_enable = -1;
    
    memcpy(mUrl,file_name,strlen(file_name));
	para.file_name = mUrl;
	para.update_cb = updatePlayerState;
	//para.no_audio=1;
	//para.no_video=1;
	para.width = -1;
	para.height = -1;

    void *handle = mDtpHandle;
    if(handle != NULL)
    {
	    __android_log_print(ANDROID_LOG_DEBUG, DEBUG_TAG, "last player is running\n");
        return -1;
    }

    handle = dtplayer_init(&para);
    if (!handle)
    {
	    __android_log_print(ANDROID_LOG_DEBUG, DEBUG_TAG, "player init failed \n");
        return -1;
    }
    //get media info
	dt_media_info_t info;
	int ret = dtplayer_get_mediainfo(handle, &info);
	if(ret < 0)
	{
	    __android_log_print(ANDROID_LOG_DEBUG, DEBUG_TAG, "Get mediainfo failed, quit \n");
		return -1;
	}
    //update dtPlayer info with mediainfo

	memcpy(&media_info,&info,sizeof(dt_media_info_t));
	__android_log_print(ANDROID_LOG_DEBUG, DEBUG_TAG, "Get Media Info Ok,filesize:%lld fulltime:%lld S \n",info.file_size,info.duration);
    
    mDtpHandle = handle;
    status = PLAYER_INITED;
}

int DTPlayer::prePare()
{
    if(status != PLAYER_INITED)
        return -1;
    status = PLAYER_PREPARED;
    return 0;
}

int DTPlayer::prePareAsync()
{
    if(status != PLAYER_INITED)
        return -1;
    status = PLAYER_PREPARED;
    return 0;
}

int DTPlayer::start()
{
    int ret = 0;
    void *handle = mDtpHandle;

    if(status < PLAYER_PREPARED)
        return -1;

    if(!handle)
        return -1;
    ret = dtplayer_start(handle);
    if(ret < 0)
        return -1;
    status = PLAYER_RUNNING;
    return 0;
}

int DTPlayer::pause()
{
    void *handle = mDtpHandle;
    if(!handle)
        return -1;
    
    if(status == PLAYER_RUNNING)
    {
        dtplayer_pause(handle);
        status = PLAYER_PAUSED;
    }
    if(status == PLAYER_PAUSED)
    {
        dtplayer_resume(handle);
        status = PLAYER_RUNNING;
    }

    return 0;
}

int DTPlayer::seekTo(int pos) // ms
{
    void *handle = mDtpHandle;
    if(!handle)
        return -1;
    
    if(status == PLAYER_RUNNING || status == PLAYER_STATUS_PAUSED)
    {
        dtplayer_seekto(handle,pos);
        status = PLAYER_SEEKING;
    }

    return 0; 
}

int DTPlayer::stop()
{
    void *handle = mDtpHandle;
    if(!handle)
        return -1;
    dtplayer_stop(handle);
    status = PLAYER_STOPPED;

    return 0;
}

int DTPlayer::release()
{
    return stop();
}

int DTPlayer::reset()
{
    //do nothing
    return 0;
}

int DTPlayer::getVideoWidth()
{
    void *handle = mDtpHandle;
    if(!handle)
        return -1;
    return -1; 
}

int DTPlayer::getVideoHeight()
{
    void *handle = mDtpHandle;
    if(!handle)
        return -1;
    return -1; 
}

int DTPlayer::isPlaying()
{
    void *handle = mDtpHandle;
    if(!handle)
        return 0;
    if(status == PLAYER_RUNNING || status == PLAYER_SEEKING || status == PLAYER_PAUSED)
        return 1;
    return 0;
}

int DTPlayer::getCurrentPosition()
{
    void *handle = mDtpHandle;
    if(!handle)
        return 0;
    return mCurrentPosition;

}

int DTPlayer::getDuration()
{
    void *handle = mDtpHandle;
    if(!handle)
        return 0;
    return mDuration;

}

int DTPlayer::updatePlayerState(player_state_t *state)
{
    //Fixme -- callback to java
    
    return 0;
}


extern "C"{

int update_frame(uint8_t *buf, int size)
{
    return 0;
}


}

} // end namespace android
