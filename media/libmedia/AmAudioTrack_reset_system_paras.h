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

audio_format_t AudioTrack_reset_system_samplerate(int samplerate,audio_format_t format,audio_channel_mask_t ChMask,
                                                                    audio_output_flags_t flags,unsigned int *pSampleRateAudiotrak);
void AudioTrack_restore_system_samplerate(audio_format_t format,audio_output_flags_t flags ,unsigned int SampleRateAudiotrake);

}
