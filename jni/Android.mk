LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := SSAO_Complete
LOCAL_SRC_FILES := SSAO_Complete.cpp
LOCAL_LDLIBS := -llog -lGLESv3 -ldl -lm
LOCAL_CPPFLAGS := -std=c++17 -O3 -ffast-math -fno-exceptions
LOCAL_CFLAGS := -DNDEBUG
include $(BUILD_SHARED_LIBRARY)
