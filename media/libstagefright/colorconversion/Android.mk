LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:=                     \
        ColorConverter.cpp            \
        SoftwareRenderer.cpp

LOCAL_C_INCLUDES := \
		$(TOP)/system/core/include \
		$(TOP)/hardware/libhardware/include \
		$(TOP)/hardware/samsung_slsi/slsiap/include \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/hardware/msm7k

LOCAL_MODULE:= libstagefright_color_conversion

include $(BUILD_STATIC_LIBRARY)
