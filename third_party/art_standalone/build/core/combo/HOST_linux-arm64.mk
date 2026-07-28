#
# Copyright (C) 2006 The Android Open Source Project
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
#

# Configuration for builds hosted on linux-arm64.
# Included by combo/select.mk

TARGET_ARCH_VARIANT := armv8

HOST_CC  := gcc
HOST_CXX := g++
HOST_AR  := ar

# gcc location for clang; to be updated when clang is updated
#HOST_TOOLCHAIN_FOR_CLANG := prebuilts/gcc/linux-x86/host/x86_64-linux-glibc2.15-4.8/

HOST_GLOBAL_CFLAGS += -Wa,--noexecstack -I/usr/include/valgrind/
HOST_GLOBAL_LDFLAGS += -Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now

ifneq ($(strip $(BUILD_HOST_static)),)
# Statically-linked binaries are desirable for sandboxed environment
HOST_GLOBAL_LDFLAGS += -static
endif # BUILD_HOST_static

HOST_GLOBAL_CFLAGS += -fPIC \
  -no-canonical-prefixes \
  -include $(call select-android-config-h,linux-any)

# TODO: Set _FORTIFY_SOURCE=2. Bug 20558757.
HOST_GLOBAL_CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fstack-protector

# Workaround differences in inttypes.h between host and target.
# See bug 12708004.
HOST_GLOBAL_CFLAGS += -D__STDC_FORMAT_MACROS -D__STDC_CONSTANT_MACROS

HOST_NO_UNDEFINED_LDFLAGS := -Wl,--no-undefined


# $(1): The file to check
define get-file-size
stat --format "%s" "$(1)" | tr -d '\n'
endef
