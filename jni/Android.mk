LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := SSAO
LOCAL_SRC_FILES := main.cpp
LOCAL_LDLIBS    := -llog -lGLESv2
LOCAL_CPPFLAGS  := -std=c++17 -O2 -DNDEBUG
LOCAL_ARM_MODE  := arm
include $(BUILD_SHARED_LIBRARY)
