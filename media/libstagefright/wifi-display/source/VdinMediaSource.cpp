/*
 * Copyright (C) 2011 The Android Open Source Project
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
#define LOG_TAG "VdinMediaSource"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <OMX_IVCommon.h>

#include <ui/GraphicBuffer.h>
#include <gui/ISurfaceComposer.h>
#include <gui/IGraphicBufferAlloc.h>
#include <OMX_Component.h>

#include <media/hardware/MetadataBufferType.h>

#include <utils/Log.h>
#include <utils/String8.h>

#include <private/gui/ComposerService.h>

#include "VdinMediaSource.h"

#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <linux/videodev2.h>
#include <hardware/hardware.h>
#include <hardware/aml_screen.h>


namespace android {

static const int64_t VDIN_MEDIA_SOURCE_TIMEOUT_NS = 3000000000LL;

//#define AML_SCREEN_HARDWARE_MODULE_ID  "screen_source"
//#define AML_SCREEN_SOURCE              "screen_source"


static void VdinDataCallBack(void *user, aml_screen_buffer_info_t *buffer){
    VdinMediaSource *source = static_cast<VdinMediaSource *>(user);
    source->dataCallBack(buffer);
    return;
}

VdinMediaSource::VdinMediaSource(uint32_t bufferWidth, uint32_t bufferHeight, bool canvas_mode) :
    mWidth(bufferWidth),
    mHeight(bufferHeight),
    mCurrentTimestamp(0),
    mFrameRate(30),
    mStarted(false),
    mError(false),
    mNumFramesReceived(0),
    mNumFramesEncoded(0),
    mFirstFrameTimestamp(0),
    mMaxAcquiredBufferCount(4),  // XXX double-check the default
    mUseAbsoluteTimestamps(false),
    mCanvasMode(canvas_mode){

    ALOGV("VdinMediaSource: %dx%d canvas_mode:%d", bufferWidth, bufferHeight, canvas_mode);

    if (bufferWidth == 0 || bufferHeight == 0) {
        ALOGE("Invalid dimensions %dx%d", bufferWidth, bufferHeight);
    }
#if USE_VDIN
    mScreenModule = 0;
    mScreenDev = 0;
#endif

    bufferTimeUs = 0;
    mFrameCount = 0;
    mTimeBetweenFrameCaptureUs =0;
    mFirstRead = false;
}

VdinMediaSource::~VdinMediaSource() {
    ALOGV("~VdinMediaSource");
    CHECK(!mStarted);

    reset();
#if USE_VDIN
    if (mScreenDev)
        mScreenDev->common.close((struct hw_device_t *)mScreenDev);
#endif
}

nsecs_t VdinMediaSource::getTimestamp() {
    ALOGV("getTimestamp");
    Mutex::Autolock autoLock(mLock);
    return mCurrentTimestamp;
}

status_t VdinMediaSource::setFrameRate(int32_t fps) {
    ALOGD("setFrameRate fps:%d", fps);
    Mutex::Autolock autoLock(mLock);
    const int MAX_FRAME_RATE = 60;
    if (fps <= 0 || fps > MAX_FRAME_RATE) {
        return BAD_VALUE;
    }
    mFrameRate = fps;
    return OK;
}

bool VdinMediaSource::isMetaDataStoredInVideoBuffers() const {
    ALOGV("isMetaDataStoredInVideoBuffers");
    return true;
}

int32_t VdinMediaSource::getFrameRate( ) {
    ALOGV("getFrameRate %d", mFrameRate);
    Mutex::Autolock autoLock(mLock);
    return mFrameRate;
}

void VdinMediaSource::setVideoRotation(int degree) {
    int angle;

    ALOGI("[%s %d] setVideoRotation degree:%x", __FUNCTION__, __LINE__, degree);

    if (degree == 0) angle = 0;
    else if (degree == 1) angle = 270;
    else if (degree == 2) angle = 180;
    else if (degree == 3) angle = 90;
    else {
        ALOGE("degree is not right");
        return;
    }

    ALOGI("[%s %d] setVideoRotation angle:%x", __FUNCTION__, __LINE__, angle);
#if USE_VDIN
    if (mScreenDev != NULL)
        mScreenDev->ops.set_rotation(mScreenDev, angle);
#endif
}

status_t VdinMediaSource::start(MetaData *params) {
    ALOGD("[%s %d], mFrameRate:%d", __FUNCTION__, __LINE__, mFrameRate);

    Mutex::Autolock autoLock(mLock);

    CHECK(!mStarted);
    status_t err = -1;

    mStartTimeOffsetUs = 0;
    int64_t startTimeUs;
    if (params) {
        if (params->findInt64(kKeyTime, &startTimeUs)) {
            mStartTimeOffsetUs = startTimeUs;
        }
    }

    mStartTimeUs = systemTime(SYSTEM_TIME_MONOTONIC)/1000;
#if USE_VDIN
    if (!mScreenModule) {
        hw_get_module(AML_SCREEN_HARDWARE_MODULE_ID, (const hw_module_t **)&mScreenModule);
        mScreenModule->common.methods->open((const hw_module_t *)mScreenModule, AML_SCREEN_SOURCE, (struct hw_device_t**)&mScreenDev);
    }
    mScreenDev->ops.set_source_type(mScreenDev, WIFI_DISPLAY);
    mScreenDev->ops.set_frame_rate(mScreenDev, mFrameRate);
    mScreenDev->ops.set_format(mScreenDev, mWidth, mHeight, V4L2_PIX_FMT_NV21);
    mScreenDev->ops.setDataCallBack(mScreenDev, VdinDataCallBack, (void*)this);
    mScreenDev->ops.start(mScreenDev);
#else
    mSwitchNum = 0;
#endif
    mError = false;
    mNumFramesReceived = mNumFramesEncoded = 0;
    ALOGV("VdinMediaSource start mStartTimeUs:%lld", mStartTimeUs);
    mStarted = true;
    return OK;
}

status_t VdinMediaSource::setMaxAcquiredBufferCount(size_t count) {
    ALOGV("setMaxAcquiredBufferCount(%d)", count);
    Mutex::Autolock autoLock(mLock);

    CHECK_GT(count, 1);
    mMaxAcquiredBufferCount = count;
    return OK;
}

status_t VdinMediaSource::setUseAbsoluteTimestamps() {
    ALOGV("setUseAbsoluteTimestamps");
    Mutex::Autolock autoLock(mLock);
    mUseAbsoluteTimestamps = true;
    return OK;
}

status_t VdinMediaSource::stop() {
    ALOGV("VdinMediaSource stop");
    reset();
    return OK;
}

sp<MetaData> VdinMediaSource::getFormat() {
    ALOGV("getFormat");

    Mutex::Autolock autoLock(mLock);
    sp<MetaData> meta = new MetaData;

    meta->setInt32(kKeyWidth, mWidth);
    meta->setInt32(kKeyHeight, mHeight);
    meta->setInt32(kKeyColorFormat, OMX_COLOR_FormatYUV420Planar);
    meta->setInt32(kKeyStride, mWidth);
    meta->setInt32(kKeySliceHeight, mHeight);

    meta->setInt32(kKeyFrameRate, mFrameRate);
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RAW);
    return meta;
}

void VdinMediaSource::ConvertYUV420SemiPlanarToYUV420Planar(
        uint8_t *inyuv, uint8_t* outyuv,
        int32_t width, int32_t height) {

    int32_t outYsize = width * height;
    uint32_t *outy =  (uint32_t *) outyuv;
    uint16_t *outcb = (uint16_t *) (outyuv + outYsize);
    uint16_t *outcr = (uint16_t *) (outyuv + outYsize + (outYsize >> 2));

    /* Y copying */
    memcpy(outy, inyuv, outYsize);

    /* U & V copying */
    uint32_t *inyuv_4 = (uint32_t *) (inyuv + outYsize);
    for (int32_t i = height >> 1; i > 0; --i) {
        for (int32_t j = width >> 2; j > 0; --j) {
            uint32_t temp = *inyuv_4++;
            uint32_t tempU = temp & 0xFF;
            tempU = tempU | ((temp >> 8) & 0xFF00);

            uint32_t tempV = (temp >> 8) & 0xFF;
            tempV = tempV | ((temp >> 16) & 0xFF00);

            // Flip U and V
            *outcb++ = tempV;
            *outcr++ = tempU;
        }
    }
}

status_t VdinMediaSource::read(MediaBuffer **buffer, const ReadOptions * /*options*/) {
    Mutex::Autolock autoLock(mLock);

    int64_t cur_pts = 0;
    FrameDataInfo frameDataInfo;
    int ret = 0;
    int count = 0;
    FrameBufferInfo* frame = NULL;
    *buffer = NULL;

    while (mStarted && mFramesReceived.empty()) {
        if (NO_ERROR !=
            mFrameAvailableCondition.waitRelative(mLock,
                mTimeBetweenFrameCaptureUs * 1000LL + VDIN_MEDIA_SOURCE_TIMEOUT_NS)) {
            if (count >= 10) {
                ALOGE("Wait too long for vdin media source frames");
                return ERROR_END_OF_STREAM;
            }
            ALOGE("Timed out waiting for incoming vdin media source frames: %lld us", mLastFrameTimestampUs);
            return -EAGAIN;
        }
    }
    if (!mStarted) {
        return OK;
    }

    if (mFirstRead == false) {
        while (!mFramesReceived.empty()) {
            frame = *mFramesReceived.begin();
            mFramesReceived.erase(mFramesReceived.begin());
            mScreenDev->ops.release_buffer(mScreenDev, frame->buf_ptr);
        }
        mFirstRead = true;
        return -EAGAIN;
    }

    frame = *mFramesReceived.begin();
    mFramesReceived.erase(mFramesReceived.begin());

    MediaBuffer *tBuffer = new MediaBuffer((mCanvasMode == true)?(sizeof(FrameBufferInfo)):(mWidth*mHeight*3/2));
    if (mCanvasMode == false) {
        mScreenDev->ops.release_buffer(mScreenDev, frame->buf_ptr);
    }else{
        frameDataInfo.MetadataBufferType = kMetadataBufferTypeCanvasSource;
        frameDataInfo.buf_ptr            = frame->buf_ptr;
        frameDataInfo.canvasNum          = frame->canvas;
        //ALOGV("dataCallBack buf_ptr:%x canvas:%x", frame->buf_ptr, frame->canvas);
        memcpy((uint8_t *)tBuffer->data(), &frameDataInfo, sizeof(FrameDataInfo));
        cur_pts = (int64_t)frame->timestampUs;
    }
    *buffer = tBuffer;
    (*buffer)->setObserver(this);
    (*buffer)->add_ref();

    int64_t timeNow64;
    struct timeval timeNow;
    gettimeofday(&timeNow, NULL);
    timeNow64 = (int64_t)timeNow.tv_sec*1000*1000 + (int64_t)timeNow.tv_usec;

    (*buffer)->meta_data()->setInt64(kKeyTime, cur_pts);

    mFrameCount++;
    if (frame)
        delete frame;
    return OK;
}

void VdinMediaSource::signalBufferReturned(MediaBuffer *buffer) {

    Mutex::Autolock autoLock(mLock);

    if (mCanvasMode == true) {
        FrameDataInfo frameDataInfo;
        memcpy(&frameDataInfo, (uint8_t *)buffer->data(), sizeof(FrameDataInfo));
#if USE_VDIN
        mScreenDev->ops.release_buffer(mScreenDev, frameDataInfo.buf_ptr);
#endif
    }

    buffer->setObserver(0);
    buffer->release();
    ++mNumFramesEncoded;
}

int VdinMediaSource::dataCallBack(aml_screen_buffer_info_t *buffer) {
    int ret = NO_ERROR;
    if ((mStarted) && (mError == false)) {
        if (buffer == NULL || (buffer->buffer_mem == 0)) {
            ALOGE("aquire_buffer fail, ptr:0x%x", buffer);
            return BAD_VALUE;
        }
        if ((mCanvasMode == true) && (buffer->buffer_canvas == 0)) {
            mError = true;
            ALOGE("Could get canvas info from device!");
            return BAD_VALUE;
        }

        int64_t timestampUs = systemTime(SYSTEM_TIME_MONOTONIC)/1000;
        mLastFrameTimestampUs = timestampUs;
        if (mNumFramesReceived == 0) {
            mFirstFrameTimestamp = timestampUs;
            // Initial delay
            if (mStartTimeOffsetUs > 0) {
                if (timestampUs-mStartTimeUs < mStartTimeOffsetUs) {
#if USE_VDIN
                    mScreenDev->ops.release_buffer(mScreenDev, buffer->buffer_mem);
#endif
                    return NO_ERROR;
                }
                mStartTimeOffsetUs = timestampUs - mStartTimeUs;
            }
        }

        FrameBufferInfo* frame = new FrameBufferInfo;
        if (!frame) {
#if USE_VDIN
            mScreenDev->ops.release_buffer(mScreenDev, buffer->buffer_mem);
#endif
            mError = true;
            ALOGE("Could Alloc Frame!");
            return BAD_VALUE;
        }

        ++mNumFramesReceived;
        {
            Mutex::Autolock autoLock(mLock);
            frame->buf_ptr = buffer->buffer_mem;
            frame->canvas = buffer->buffer_canvas;
            //frame->timestampUs = mStartTimeOffsetUs + (timestampUs - mFirstFrameTimestamp);
            frame->timestampUs = (int64_t)buffer->tv_sec*1000*1000 + (int64_t)buffer->tv_usec;
            mFramesReceived.push_back(frame);
            mFrameAvailableCondition.signal();
        }
    }
    return ret;
}

status_t VdinMediaSource::reset(void) {
    ALOGD("VdinMediaSource::reset E");

    FrameBufferInfo* frame = NULL;

    if (!mStarted) {
        ALOGD("VdinMediaSource::reset X Do nothing");
        return OK;
    }

    {
        Mutex::Autolock autoLock(mLock);
        mStarted = false;
    }

#if USE_VDIN
    mScreenDev->ops.stop(mScreenDev);
#endif

    {
        Mutex::Autolock autoLock(mLock);
        mFrameAvailableCondition.signal();
        while (!mFramesReceived.empty()) {
            frame = *mFramesReceived.begin();
            mFramesReceived.erase(mFramesReceived.begin());
            delete frame;
        }
    }
    ALOGD("VdinMediaSource::reset X");
    return OK;
}
} // end of namespace android
