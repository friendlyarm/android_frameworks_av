LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                 \
        HEVC_utils.cpp

LOCAL_C_INCLUDES:= \
	$(TOP)/external/ffmpeg/ \
	$(TOP)/frameworks/av/media/libstagefright \

LOCAL_MODULE:= libstagefright_hevcutils

LOCAL_SHARED_LIBRARIES := \
        libamffmpeg \

include $(BUILD_STATIC_LIBRARY)
