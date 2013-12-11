# Copyright (C) 2012 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# Will build audio.usb.*.so lib where naming is taken from build-target.
# If USB-header is present in audio_policy.conf then this lib will be linked
# and used for USB Audio.

LOCAL_MODULE := audio.usb.$(TARGET_DEVICE)
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw

LOCAL_SRC_FILES := audio_hw.c
LOCAL_C_INCLUDES += external/tinyalsa/include
LOCAL_SHARED_LIBRARIES := liblog libcutils libtinyalsa

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

