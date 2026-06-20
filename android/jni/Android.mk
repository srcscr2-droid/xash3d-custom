LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := xash
LOCAL_SRC_FILES := dummy.c

include $(BUILD_SHARED_LIBRARY)
