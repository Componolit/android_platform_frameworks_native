LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

ifdef BOARD_WLAN_DEVICE
LOCAL_CFLAGS := -DFWDUMP_$(BOARD_WLAN_DEVICE)
endif

LOCAL_SRC_FILES := dumpstate.cpp utils.cpp

LOCAL_MODULE := dumpstate

LOCAL_SHARED_LIBRARIES := libcutils libdebuggerd_client liblog libselinux libbase libhardware_legacy
# ZipArchive support, the order matters here to get all symbols.
LOCAL_STATIC_LIBRARIES := libziparchive libz libcrypto_static
LOCAL_HAL_STATIC_LIBRARIES := libdumpstate
LOCAL_CFLAGS += -Wall -Werror -Wno-unused-parameter
LOCAL_INIT_RC := dumpstate.rc

include $(BUILD_EXECUTABLE)
