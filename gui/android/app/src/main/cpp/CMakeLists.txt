# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# libmarvin-jni.so: the app's JNI layer (pin_api.h wrappers, the preview renderer, the audio
# monitor). Built by scripts/build-android-native.sh with the NDK's toolchain file, after the
# core, against the libmarvin-core.so that ships next to it; Gradle only packages the result.

cmake_minimum_required(VERSION 3.22)
project(marvin_jni C)

set(PIN_SOURCE_DIR "" CACHE PATH "The repository root (for src/api/pin_api.h)")
set(PIN_CORE_LIBRARY "" CACHE FILEPATH "The libmarvin-core.so to link against")
if(NOT EXISTS "${PIN_SOURCE_DIR}/src/api/pin_api.h" OR NOT EXISTS "${PIN_CORE_LIBRARY}")
    message(FATAL_ERROR "Set PIN_SOURCE_DIR (repository root) and PIN_CORE_LIBRARY (libmarvin-core.so)")
endif()

add_library(marvin-core SHARED IMPORTED)
set_target_properties(marvin-core PROPERTIES
    IMPORTED_LOCATION "${PIN_CORE_LIBRARY}"
    IMPORTED_SONAME "libmarvin-core.so"
    INTERFACE_INCLUDE_DIRECTORIES "${PIN_SOURCE_DIR}/src/api")

add_library(marvin-jni SHARED marvin_jni.c preview.c audio.c)
set_target_properties(marvin-jni PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_VISIBILITY_PRESET hidden)   # only the JNIEXPORT Java_* functions are exported
target_compile_options(marvin-jni PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
target_link_libraries(marvin-jni PRIVATE marvin-core android log EGL GLESv3 aaudio)
# 16 KB pages (Android 15+ devices, Google Play); the NDK's default since r28, stated anyway.
target_link_options(marvin-jni PRIVATE -Wl,-z,max-page-size=16384 -Wl,--no-undefined -Wl,--gc-sections)
