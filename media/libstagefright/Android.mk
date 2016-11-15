LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

include frameworks/av/media/libstagefright/codecs/common/Config.mk

USE_AM_SOFT_DEMUXER_CODEC := true
LOCAL_SRC_FILES:=                         \
        ACodec.cpp                        \
        AACExtractor.cpp                  \
        ADIFExtractor.cpp                  \
        ADTSExtractor.cpp                  \
        LATMExtractor.cpp                  \
        THDExtractor.cpp                  \
        AACWriter.cpp                     \
        AMRExtractor.cpp                  \
        AMRWriter.cpp                     \
        AudioPlayer.cpp                   \
        AudioSource.cpp                   \
        AwesomePlayer.cpp                 \
        CameraSource.cpp                  \
        CameraSourceTimeLapse.cpp         \
        ClockEstimator.cpp                \
        CodecBase.cpp                     \
        DataSource.cpp                    \
        DataURISource.cpp                 \
        DRMExtractor.cpp                  \
        ESDS.cpp                          \
        FileSource.cpp                    \
        FLACExtractor.cpp                 \
        HTTPBase.cpp                      \
        JPEGSource.cpp                    \
        MP3Extractor.cpp                  \
        MPEG2TSWriter.cpp                 \
        MPEG4Extractor.cpp                \
        MPEG4Writer.cpp                   \
        MediaAdapter.cpp                  \
        MediaBuffer.cpp                   \
        MediaBufferGroup.cpp              \
        MediaCodec.cpp                    \
        MediaCodecList.cpp                \
        MediaCodecSource.cpp              \
        MediaDefs.cpp                     \
        MediaExtractor.cpp                \
        http/MediaHTTP.cpp                \
        MediaMuxer.cpp                    \
        MediaSource.cpp                   \
        MetaData.cpp                      \
        NuCachedSource2.cpp               \
        NuMediaExtractor.cpp              \
        OMXClient.cpp                     \
        OMXCodec.cpp                      \
        OggExtractor.cpp                  \
        SampleIterator.cpp                \
        SampleTable.cpp                   \
        SkipCutBuffer.cpp                 \
        StagefrightMediaScanner.cpp       \
        StagefrightMetadataRetriever.cpp  \
        SurfaceMediaSource.cpp            \
        ThrottledSource.cpp               \
        TimeSource.cpp                    \
        TimedEventQueue.cpp               \
        Utils.cpp                         \
        VBRISeeker.cpp                    \
        WAVExtractor.cpp                  \
        WVMExtractor.cpp                  \
        XINGSeeker.cpp                    \
        avc_utils.cpp                     \
        AsfExtractor/ASFExtractor.cpp     \
	DtshdExtractor.cpp  \
        AIFFExtractor.cpp                 \

LOCAL_SRC_FILES +=                         \
	DDPExtractor.cpp
LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include/media/ \
        $(TOP)/frameworks/av/include/media/stagefright/timedtext \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/external/flac/include \
        $(TOP)/external/tremolo \
        $(TOP)/external/openssl/include \
        $(TOP)/external/libvpx/libwebm \
        $(TOP)/system/netd/include \
        $(TOP)/external/icu/icu4c/source/common \
        $(TOP)/external/icu/icu4c/source/i18n \
	$(LOCAL_PATH)/codecs/adif/include

ifeq ($(BUILD_WITH_AMLOGIC_PLAYER),true)
    AMPLAYER_APK_DIR=$(TOP)/vendor/amlogic/frameworks/av/LibPlayer/
    LOCAL_C_INCLUDES += \
        $(AMPLAYER_APK_DIR)/amplayer/player/include     \
        $(AMPLAYER_APK_DIR)/amplayer/control/include    \
        $(AMPLAYER_APK_DIR)/amadec/include              \
        $(AMPLAYER_APK_DIR)/amcodec/include             \
        $(AMPLAYER_APK_DIR)/amavutils/include           \
        $(AMPLAYER_APK_DIR)/amvdec/include           \
        $(AMPLAYER_APK_DIR)/amffmpeg/
endif

LOCAL_SHARED_LIBRARIES := \
        libbinder \
        libcamera_client \
        libcutils \
        libdl \
        libdrmframework \
        libexpat \
        libgui \
        libicui18n \
        libicuuc \
        liblog \
        libmedia \
        libnetd_client \
        libopus \
        libsonivox \
        libssl \
        libstagefright_omx \
        libstagefright_yuv \
        libsync \
        libui \
        libutils \
        libvorbisidec \
        libz \
        libpowermanager

LOCAL_STATIC_LIBRARIES := \
        libstagefright_color_conversion \
        libstagefright_aacenc \
        libstagefright_matroska \
        libstagefright_webm \
        libstagefright_timedtext \
        libvpx \
        libwebm \
        libstagefright_mpeg2ts \
        libstagefright_id3 \
        libFLAC \
        libmedia_helper \
        libstagefright_hevcutils

LOCAL_SHARED_LIBRARIES += \
        libstagefright_enc_common \
        libstagefright_avc_common \
        libstagefright_foundation \
        libdl \
        libamffmpeg

LOCAL_STATIC_LIBRARIES += \
	libstagefright_adifdec
LOCAL_C_INCLUDES+= \
	$(TOP)/frameworks/av/media/libstagefright/include  \
	$(TOP)/frameworks/av/media/libmediaplayerservice  \
	$(TOP)/external/ffmpeg

LOCAL_CFLAGS += -Wno-multichar

  LOCAL_CFLAGS += -DDOLBY_UDC

ifdef DOLBY_DS1_UDC
  LOCAL_CFLAGS += -DDOLBY_DS1_UDC
endif

ifdef DOLBY_PULSE
  LOCAL_CFLAGS += -DDOLBY_PULSE
endif #DOLBY_PULSE

#ifdef DOLBY_UDC_MULTICHANNEL
  LOCAL_CFLAGS += -DDOLBY_UDC_MULTICHANNEL
#endif #DOLBY_UDC_MULTICHANNEL

ifdef DOLBY_DAP
    ifdef DOLBY_DAP_OPENSLES
        LOCAL_CFLAGS += -DDOLBY_DAP_OPENSLES
    endif
endif #DOLBY_END

ifeq ($(USE_AM_SOFT_DEMUXER_CODEC),true)
LOCAL_SRC_FILES += \
        AmMediaDefsExt.cpp                \
        AmMediaExtractorPlugin.cpp        \

LOCAL_CFLAGS += -DUSE_AM_SOFT_DEMUXER_CODEC

LOCAL_C_INCLUDES  += \
        $(TOP)/external/ffmpeg/
endif

LOCAL_MODULE:= libstagefright

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
