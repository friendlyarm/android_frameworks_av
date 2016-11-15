#define LOG_TAG "AudioTrack"
#include <inttypes.h>
#include <math.h>
#include <sys/resource.h>

#include <audio_utils/primitives.h>
#include <binder/IPCThreadState.h>
#include <media/AudioTrack.h>
#include <utils/Log.h>
#include <private/media/AudioTrackShared.h>
#include <media/IAudioFlinger.h>
#include <media/AudioPolicyHelper.h>
#include <media/AudioResamplerPublic.h>
#include <media/AudioSystem.h>
#include <fcntl.h>

namespace android {

static int amsysfs_get_sysfs_int(const char *path)
{
    int fd;
    int val = 0;
    char  bcmd[16];
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        read(fd, bcmd, sizeof(bcmd));
        val = strtol(bcmd, NULL, 10);
        close(fd);
    } else {
        ALOGE("cat open:%s\n",path);
    }
    return val;
}
static int amsysfs_set_sysfs_int(const char *path, int val)
{
    int fd;
    int bytes;
    char  bcmd[16];
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        sprintf(bcmd, "%d", val);
        bytes = write(fd, bcmd, strlen(bcmd));
        close(fd);
        return 0;
    } else {
        ALOGE("unable to open file %s", path);
    }
    return -1;
}
//TODO:the following to copy defined according to
//         </system/core/include/system/audio.h>
//should add to  </system/core/include/system/audio.h> some day
#define AUDIO_FORMAT_DTS_HD      0x0D000000
#define AUDIO_FORMAT_DTS_MASTER  0x0E000000
#define DEFAULT_SYS_SAMPLERATE   48000

audio_format_t AudioTrack_reset_system_samplerate(int samplerate,audio_format_t format,audio_channel_mask_t ChMask,
                                                                    audio_output_flags_t flags,unsigned int *pSampleRateAudiotrak)
{
    unsigned digital_raw = 0,need_reset_sysfs=0;
    audio_io_handle_t handle = -1;
    if (!(flags & AUDIO_OUTPUT_FLAG_DIRECT))
    {
        if (format ==( audio_format_t)AUDIO_FORMAT_DTS_HD || format == ( audio_format_t)AUDIO_FORMAT_DTS_MASTER)
            format = AUDIO_FORMAT_DTS;
        return format;
    }

    digital_raw = amsysfs_get_sysfs_int("/sys/class/audiodsp/digital_raw");
    if (digital_raw && format == AUDIO_FORMAT_TRUEHD)
    {
        //TRUEHD rawoutput: samplerate should be 192k___now just amadec realized it and go into this case
        if (samplerate == 192000)
        {
            need_reset_sysfs = samplerate;
            amsysfs_set_sysfs_int("/sys/class/audiodsp/digital_codec",7);
        }
    }else if (digital_raw == 1 )
    {

        if (format == AUDIO_FORMAT_AC3 || format == AUDIO_FORMAT_E_AC3)
        {
            //DD rawoutput: samplerate should only be 32k/44.1k/48k___now both amadec and third party can go into this case
            if (samplerate == 32000 || samplerate == 44100 || samplerate == 48000)
            {
                need_reset_sysfs = samplerate % DEFAULT_SYS_SAMPLERATE;
                amsysfs_set_sysfs_int("/sys/class/audiodsp/digital_codec",2);
            }
        }else if(format == AUDIO_FORMAT_DTS)
        {
            //DTS_core rawoutput: samplerate should only be 32k/44.1k/48k___now both amadec and third party can go into this case
            //for amadec    : samplerate has processed to 32k/44.1k/48k  for DTS_core and DTS_M6
            //for third party:
            //          samplerate can be 32k/44.1k/48k using DTS_core, and 88.2k/96k using DTS_M6 for some stream
            //          so we double check it and process it here
             if (samplerate == 32000 || samplerate == 44100 || samplerate == 48000 ||
                 samplerate == 88200 || samplerate == 96000)
             {
                if (samplerate > 48000)
                    samplerate = samplerate >> 1;
                need_reset_sysfs = samplerate % DEFAULT_SYS_SAMPLERATE;
                amsysfs_set_sysfs_int("/sys/class/audiodsp/digital_codec",3);
             }
        }
    }else if (digital_raw == 2) {
         if (format == AUDIO_FORMAT_AC3)
         {
             //DD rawoutput: samplerate should only be 32k/44.1k/48k___now both amadec and third party can go into this case
             if (samplerate == 32000 || samplerate == 44100 || samplerate == 48000)
             {
                 need_reset_sysfs = samplerate % DEFAULT_SYS_SAMPLERATE;
                 amsysfs_set_sysfs_int("/sys/class/audiodsp/digital_codec",2);
             }
         }else if(format == AUDIO_FORMAT_E_AC3)
         {
             //DD+ rawoutput: samplerate should only be 32k/44.1k/48k___now both amadec and third party can go into this case
              if (samplerate == 32000 || samplerate == 44100 || samplerate == 48000)
              {
                  need_reset_sysfs = samplerate % DEFAULT_SYS_SAMPLERATE;
                  amsysfs_set_sysfs_int("/sys/class/audiodsp/digital_codec",4);
              }
         }else if(format == AUDIO_FORMAT_DTS)
         {
             //DTS_core rawoutput: samplerate should only be 32k/44.1k/48k___now both amadec and third party can go into this case
             //for amadec    : samplerate has processed to 32k/44.1k/48k  for DTS_core and DTS_M6
             //for third party:
             //          samplerate can be 32k/44.1k/48k using DTS_core, and 88.2k/96k using DTS_M6 for some stream
             //          so we double check it and process it here
             if (samplerate == 32000 || samplerate == 44100 || samplerate == 48000 ||
                 samplerate == 88200 || samplerate == 96000)
             {
                if (samplerate > 48000)
                    samplerate = samplerate >> 1;
                need_reset_sysfs = samplerate;
                amsysfs_set_sysfs_int("/sys/class/audiodsp/digital_codec",3);
             }
         }else if(format == (audio_format_t)AUDIO_FORMAT_DTS_HD )
         {
             //DTS_HD_FORMAT_PKT raw_output  ___only DTS_M6 decoder can support it now:
             //    Fs_Iec958  may be not processed ,so we process it  here to reset sysfs
             if (samplerate == 192000 ||samplerate ==176400)
             {
                  samplerate >>= 2;
                  need_reset_sysfs = samplerate;
                  amsysfs_set_sysfs_int("/sys/class/audiodsp/digital_codec",5);
             }
             ALOGI("[%s %d]Change format [AUDIO_FORMAT_DTS_HD] to [AUDIO_FORMAT_DTS]\n",__FUNCTION__,__LINE__);
             format = AUDIO_FORMAT_DTS;
         }else if(format == ( audio_format_t)AUDIO_FORMAT_DTS_MASTER)
         {
             //DTS_Master_FORMAT_PKT raw output ___only DTS_M6 decoder can support it now:
             //      Fs_Iec958  may be (176.4k/192k/352.8k/384k/705.6k/768k),so we process it  here to reset sysfs
             if (samplerate == 192000 || samplerate == 384000 || samplerate == 768000 ||
                 samplerate == 176400 || samplerate == 352800 || samplerate == 705600)
             {
                samplerate >>= 2;
                need_reset_sysfs = samplerate;
                amsysfs_set_sysfs_int("/sys/class/audiodsp/digital_codec",8);
             }
             ALOGI("[%s %d]Change format [AUDIO_FORMAT_DTS_MASTER] to [AUDIO_FORMAT_DTS]\n",__FUNCTION__,__LINE__);
             format = AUDIO_FORMAT_DTS;
         }
    }else if(!digital_raw && format == AUDIO_FORMAT_DTS && flags == AUDIO_OUTPUT_FLAG_DIRECT)
    {    //DTSHD 88.2k/96k HD-PCM directoutput ,now just DTS_M6 decoder can support it:
         if (samplerate == 88200 || samplerate == 96000)
         {
             need_reset_sysfs = samplerate;
         }
    }

    if (need_reset_sysfs > 0 && need_reset_sysfs != DEFAULT_SYS_SAMPLERATE)
    {
        *pSampleRateAudiotrak = need_reset_sysfs;
        //for  M8, raw/pcm  output use different HAL, so only check the raw output device
        handle = AudioSystem::getOutput(AUDIO_STREAM_MUSIC,
                                        48000,
                                        AUDIO_FORMAT_AC3, //use AC3 as the format tag for all raw output
                                        AUDIO_CHANNEL_OUT_STEREO,
                                        AUDIO_OUTPUT_FLAG_DIRECT
                                        );
        if (handle > 0) {
            char str[64];
            memset(str,0,sizeof(str));
            sprintf(str,"sampling_rate=%d",need_reset_sysfs);
            AudioSystem::setParameters(handle, String8(str));
            AudioSystem::releaseOutput(handle, AUDIO_STREAM_DEFAULT, AUDIO_SESSION_OUTPUT_STAGE);
            ALOGI("[%s %d]handle/%d reset AudioSysFS/%d for rawoutput Success! [format/0x%x]\n",
                __FUNCTION__,__LINE__,handle,need_reset_sysfs,format);
        }else{
            ALOGI("[%s %d]WARNIN:handle/%d reset AudioSysFS/%d for rawoutput failed! [format/0x%x]\n",
                __FUNCTION__,__LINE__,handle,need_reset_sysfs,format);
        }
    }
    return format;
}

void AudioTrack_restore_system_samplerate(audio_format_t format,audio_output_flags_t flags ,unsigned int SampleRateAudiotrake)
{
    audio_io_handle_t handle = -1;
    if (!(flags & AUDIO_OUTPUT_FLAG_DIRECT) ||
        !(format==AUDIO_FORMAT_DTS || format == AUDIO_FORMAT_AC3 || format == AUDIO_FORMAT_E_AC3 || format == AUDIO_FORMAT_TRUEHD) ||
        (SampleRateAudiotrake == DEFAULT_SYS_SAMPLERATE) ){
        return;
    }
    handle = AudioSystem::getOutput(AUDIO_STREAM_MUSIC,
                                48000,
                                AUDIO_FORMAT_AC3, //use AC3 as the format tag for all raw output
                                AUDIO_CHANNEL_OUT_STEREO,
                                AUDIO_OUTPUT_FLAG_DIRECT
                                );
    if (handle > 0) {
        char str[64];
        memset(str,0,sizeof(str));
        sprintf(str,"sampling_rate=%d",DEFAULT_SYS_SAMPLERATE);
        AudioSystem::setParameters(handle, String8(str));
        AudioSystem::releaseOutput(handle, AUDIO_STREAM_DEFAULT, AUDIO_SESSION_OUTPUT_STAGE);
        ALOGI("[%s %d]handle/%d resetore AudioSysFs to %d success![format/0x%x flags/0x%x StreamFS/%d]\n",
                __FUNCTION__,__LINE__,handle,DEFAULT_SYS_SAMPLERATE,format,flags,SampleRateAudiotrake);
    }else{
        ALOGI("[%s %d]WARNIN: handle/%d resetore AudioSysFs failed!\n",__FUNCTION__,__LINE__,handle);
    }
    amsysfs_set_sysfs_int("/sys/class/audiodsp/digital_codec",0);
}

}
