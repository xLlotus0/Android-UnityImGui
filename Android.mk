LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := dobby_prebuilt
LOCAL_SRC_FILES := external/Dobby/$(TARGET_ARCH_ABI)/libdobby.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := Android-UnityImGui

LOCAL_SRC_FILES := \
    $(call all-cpp-files-under,source) \
    $(call all-c-files-under,source) \
    $(call all-s-files-under,source) \
    $(call all-cpp-files-under,external/imgui) \
    $(call all-cpp-files-under,external/KittyMemoryEx)

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/source \
    $(LOCAL_PATH)/source/Foundation \
    $(LOCAL_PATH)/external \
    $(LOCAL_PATH)/external/imgui \
    $(LOCAL_PATH)/external/KittyMemoryEx

LOCAL_CPPFLAGS := \
    -std=c++20 \
    -fvisibility=hidden \
    -ffunction-sections \
    -fdata-sections \
    -DkUSE_LOGCAT \
    -DkANDROID_LOG \
    -DkANDROID_LOG_TAG=\"INJECT\" \
    -DkPROJECT_NAME=\"Android-UnityImGui\"

LOCAL_CFLAGS := \
    -fvisibility=hidden \
    -ffunction-sections \
    -fdata-sections \
    -DkUSE_LOGCAT \
    -DkANDROID_LOG \
    -DkANDROID_LOG_TAG=\"INJECT\" \
    -DkPROJECT_NAME=\"Android-UnityImGui\"

LOCAL_LDFLAGS := -Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL
LOCAL_LDLIBS := -landroid -llog -lvulkan
LOCAL_STATIC_LIBRARIES := dobby_prebuilt

include $(BUILD_SHARED_LIBRARY)
