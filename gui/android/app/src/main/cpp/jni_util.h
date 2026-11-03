/*
 * Pinnacle Studio 500-USB open driver
 * Copyright (C) 2026 Jonas Cz.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/* Shared helpers of the JNI layer (libmarvin-jni.so). */

#ifndef MARVIN_JNI_UTIL_H
#define MARVIN_JNI_UTIL_H

#include <jni.h>
#include <stddef.h>
#include <android/log.h>

#define MJ_TAG "MarvinCapture"
#define MJ_LOGI(...) __android_log_print(ANDROID_LOG_INFO, MJ_TAG, __VA_ARGS__)
#define MJ_LOGW(...) __android_log_print(ANDROID_LOG_WARN, MJ_TAG, __VA_ARGS__)
#define MJ_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, MJ_TAG, __VA_ARGS__)

/* Every native method is a static method of jonascz.marvincapture.Core. */
#define MJ_FN(ret, name) JNIEXPORT ret JNICALL Java_jonascz_marvincapture_Core_##name

/* UTF-8 (the core's strings) -> java.lang.String, through UTF-16: NewStringUTF wants modified
 * UTF-8 and aborts under CheckJNI on a sequence a truncated core text can end with. Invalid
 * bytes become U+FFFD. NULL is "". */
jstring mj_string(JNIEnv *env, const char *utf8);

/* java.lang.String -> UTF-8 into out (cap bytes, always terminated). NULL gives "". Returns
 * 0, or -1 if it did not fit (out holds the truncated, still valid, prefix then). */
int mj_utf8(JNIEnv *env, jstring s, char *out, size_t cap);

#endif
