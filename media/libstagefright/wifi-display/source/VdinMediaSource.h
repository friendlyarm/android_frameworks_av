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

#ifndef ANDROID_GUI_VDINMEDIASOURCE_H
#define ANDROID_GUI_VDINMEDIASOURCE_H

#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MediaBuffer.h>

#include <hardware/hardware.h>
#include <hardware/aml_screen.h>

#include <utils/List.h>
#include <utils/RefBase.h>
#include <utils/threads.h>

namespace android {
// ----------------------------------------------------------------------------

#define USE_VDIN 1

class VdinMediaSource : public MediaSource,
                                public MediaBufferObserver {
public:
    VdinMediaSource(uint32_t bufferWidth, uint32_t bufferHeight, bool canvas_mode = false);

    virtual ~VdinMediaSource();

    // For the MediaSource interface for use by StageFrightRecorder:
    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();
    virtual status_t read(MediaBuffer **buffer,
            const ReadOptions *options = NULL);
    virtual sp<MetaData> getFormat();

    // Get / Set the frame rate used for encoding. Default fps = 30
    status_t setFrameRate(int32_t fps) ;
    int32_t getFrameRate( );

    // The call for the StageFrightRecorder to tell us that
    // it is done using the MediaBuffer data so that its state
    // can be set to FREE for dequeuing
    virtual void signalBufferReturned(MediaBuffer* buffer);
    // end of MediaSource interface

    // getTimestamp retrieves the timestamp associated with the image
    // set by the most recent call to read()
    //
    // The timestamp is in nanoseconds, and is monotonically increasing. Its
    // other semantics (zero point, etc) are source-dependent and should be
    // documented by the source.
    int64_t getTimestamp();

    // isMetaDataStoredInVideoBuffers tells the encoder whether we will
    // pass metadata through the buffers. Currently, it is force set to true
    bool isMetaDataStoredInVideoBuffers() const;

    // To be called before start()
    status_t setMaxAcquiredBufferCount(size_t count);

    // To be called before start()
    status_t setUseAbsoluteTimestamps();

    int dataCallBack(aml_screen_buffer_info_t *buffer);

    void setVideoRotation(int degree);

private:

    typedef struct FrameBufferInfo_s{
        long* buf_ptr;
        int canvas;
        int64_t timestampUs;
    }FrameBufferInfo;

    typedef struct FrameDataInfo_s{
        int MetadataBufferType;
        long *buf_ptr;
        int canvasNum;
    }FrameDataInfo;

    status_t reset(void);

    virtual void ConvertYUV420SemiPlanarToYUV420Planar(uint8_t *inyuv, uint8_t* outyuv,int32_t width, int32_t height);

    // The permenent width and height of SMS buffers
    int mWidth;
    int mHeight;

    // mCurrentTimestamp is the timestamp for the current texture. It
    // gets set to mLastQueuedTimestamp each time updateTexImage is called.
    int64_t mCurrentTimestamp;


    // mMutex is the mutex used to prevent concurrent access to the member
    // variables of VdinMediaSource objects. It must be locked whenever the
    // member variables are accessed.
    Mutex mLock;

    ////////////////////////// For MediaSource
    // Set to a default of 30 fps if not specified by the client side
    int32_t mFrameRate;

    // mStarted is a flag to check if the recording is going on
    bool mStarted;

    // mStarted is a flag to check if the recording is going on
    bool mError;

    // mNumFramesReceived indicates the number of frames recieved from
    // the client side
    int mNumFramesReceived;
    // mNumFramesEncoded indicates the number of frames passed on to the
    // encoder
    int mNumFramesEncoded;

    // mFirstFrameTimestamp is the timestamp of the first received frame.
    // It is used to offset the output timestamps so recording starts at time 0.
    int64_t mFirstFrameTimestamp;
    // mStartTimeNs is the start time passed into the source at start, used to
    // offset timestamps.

    int64_t mLastFrameTimestampUs;

    int64_t mStartTimeOffsetUs;

    size_t mMaxAcquiredBufferCount;

    bool mUseAbsoluteTimestamps;

    bool mCanvasMode;

    int64_t mStartTimeUs;
    int64_t bufferTimeUs;
    int64_t mFrameCount;
    Condition mFrameAvailableCondition;
    List<FrameBufferInfo*> mFramesReceived;

    int64_t mTimeBetweenFrameCaptureUs;

    bool mFirstRead;

#if !USE_VDIN
    int mSwitchNum;
#endif
    aml_screen_module_t* mScreenModule;
    aml_screen_device_t* mScreenDev;
};

// ----------------------------------------------------------------------------
}; // namespace android

#endif // ANDROID_GUI_SURFACEMEDIASOURCE_H
