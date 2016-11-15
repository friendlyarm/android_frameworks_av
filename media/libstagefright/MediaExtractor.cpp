/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaExtractor"
#include <utils/Log.h>

#include "include/AMRExtractor.h"
#include "include/MP3Extractor.h"
#include "include/MPEG4Extractor.h"
#include "include/WAVExtractor.h"
#include "include/OggExtractor.h"
#include "include/MPEG2PSExtractor.h"
#include "include/MPEG2TSExtractor.h"
#include "include/DRMExtractor.h"
#include "include/WVMExtractor.h"
#include "include/FLACExtractor.h"
#include "include/AACExtractor.h"
#include "include/ADIFExtractor.h"
#include "include/ADTSExtractor.h"
#include "include/LATMExtractor.h"
#include "include/AsfExtractor.h"
#include "include/SStreamingExtractor.h"
#include "include/DDPExtractor.h"
#include "include/DtshdExtractor.h"
#include "include/AIFFExtractor.h"
#include "include/THDExtractor.h"

#include "matroska/MatroskaExtractor.h"

#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MetaData.h>
#include <utils/String8.h>

#ifdef USE_AM_SOFT_DEMUXER_CODEC
#include <media/stagefright/AmMediaExtractorPlugin.h>
#endif

namespace android {

sp<MetaData> MediaExtractor::getMetaData() {
    return new MetaData;
}

uint32_t MediaExtractor::flags() const {
    return CAN_SEEK_BACKWARD | CAN_SEEK_FORWARD | CAN_PAUSE | CAN_SEEK;
}

#ifdef USE_AM_SOFT_DEMUXER_CODEC
//static
sp<MediaExtractor> MediaExtractor::CreateEx(const sp<DataSource> &dataSource, bool isHEVC)
{
    float confidence = 0;
    String8 mime("");
    sp<AMessage> meta(NULL);
    if (!dataSource->sniff(&mime, &confidence, &meta)) {
        confidence = 0;
    }

    float am_confidence = 0;
    String8 am_mime("");
    sp<AMessage> am_meta(NULL);
    if(!sniffAmExtFormat(dataSource, &am_mime, &am_confidence, &am_meta)) {
        am_confidence = 0;
    }
 
    ALOGI("[%s %d]for WMA: force useing Amffmpeg extractor[am_confidence/%f confidence/%f ammine/%s mime/%s]\n",
         __FUNCTION__,__LINE__,am_confidence,confidence,am_mime.string(),mime.string());
    if(!strcmp(mime.string(),MEDIA_MIMETYPE_AUDIO_WMA)&& confidence>0 &&
       !strcmp(am_mime.string(),MEDIA_MIMETYPE_CONTAINER_ASF)&&am_confidence>0)
    {   //since amffpeg extractor is well performaced,why not use it,any quesion for this modification,contact me-->BUG#94436
        confidence=0;
    }

    sp<MediaExtractor> extractor = NULL;
    if(am_confidence > confidence || isHEVC) {     // if hevc/h.265, use ffmpeg extractor anyhow.
        mime = am_mime;
        extractor = createAmMediaExtractor(dataSource, mime.string());
    }

    if(NULL == extractor.get()) {
        extractor = MediaExtractor::Create(dataSource, mime.string());
    }

    return extractor;
}
#endif

// static
sp<MediaExtractor> MediaExtractor::Create(
        const sp<DataSource> &source, const char *mime) {
    sp<AMessage> meta;

    String8 tmp;
    if (mime == NULL) {
        float confidence;
        if (!source->sniff(&tmp, &confidence, &meta)) {
            ALOGV("FAILED to autodetect media content.");

            return NULL;
        }

        mime = tmp.string();
        ALOGV("Autodetected media content as '%s' with confidence %.2f",
             mime, confidence);
    }

    bool isDrm = false;
    // DRM MIME type syntax is "drm+type+original" where
    // type is "es_based" or "container_based" and
    // original is the content's cleartext MIME type
    if (!strncmp(mime, "drm+", 4)) {
        const char *originalMime = strchr(mime+4, '+');
        if (originalMime == NULL) {
            // second + not found
            return NULL;
        }
        ++originalMime;
        if (!strncmp(mime, "drm+es_based+", 13)) {
            // DRMExtractor sets container metadata kKeyIsDRM to 1
            return new DRMExtractor(source, originalMime);
        } else if (!strncmp(mime, "drm+container_based+", 20)) {
            mime = originalMime;
            isDrm = true;
        } else {
            return NULL;
        }
    }

    MediaExtractor *ret = NULL;
    if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG4)
            || !strcasecmp(mime, "audio/mp4")) {
        ret = new MPEG4Extractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG)) {
        ret = new MP3Extractor(source, meta);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_NB)
            || !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_WB)) {
        ret = new AMRExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_FLAC)) {
        ret = new FLACExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_WAV)) {
        ret = new WAVExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_OGG)) {
        ret = new OggExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MATROSKA)) {
        ret = new MatroskaExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG2TS)) {
        ret = new MPEG2TSExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_WVM)) {
        // Return now.  WVExtractor should not have the DrmFlag set in the block below.
        return new WVMExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC_ADTS)) {
        ret = new AACExtractor(source, meta);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC_ADIF)) {
        ret = new ADIFExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC_LATM)) {
        ret = new LATMExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_ADTS_PROFILE)) {
        ret = new ADTSExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG2PS)) {
        ret = new MPEG2PSExtractor(source);
    } else if(!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_WMA)||!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_WMAPRO)){
        ret = new AsfExtractor(source);
    }else if(!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_DTSHD)){
        ret = new DtshdExtractor(source);
    } else if(!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_AIFF)){
        ret = new AIFFExtractor(source);
    } else if(!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_TRUEHD)){
        ret = new THDExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_DDP)) {
        ret = new DDPExtractor(source);
    }

    if (ret != NULL) {
       if (isDrm) {
           ret->setDrmFlag(true);
       } else {
           ret->setDrmFlag(false);
       }
    }

    return ret;
}

}  // namespace android
