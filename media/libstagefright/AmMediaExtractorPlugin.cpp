/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "AmMediaExtractorPlugin"
#include <utils/Log.h>

#include <dlfcn.h>

#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/AmMediaExtractorPlugin.h>
#include <utils/String8.h>

namespace android {

static void *gAmLibHandle = dlopen("libamffmpegadapter.so", RTLD_NOW);

bool sniffAmExtFormat(
        const sp<DataSource> &source, String8 *mimeType,
        float *confidence, sp<AMessage> *meta) {
    if (gAmLibHandle) {
        DataSource::SnifferFunc sniffAmExtFormat =
                (DataSource::SnifferFunc)dlsym(
                        gAmLibHandle, "sniffAmExtFormat");
        if (sniffAmExtFormat) {
            return (*sniffAmExtFormat)(source, mimeType, confidence, meta);
        }
    }
    return false;
}

sp<MediaExtractor> createAmMediaExtractor(
        const sp<DataSource> &source, const char *mime) {
    if (gAmLibHandle) {
        typedef sp<MediaExtractor> (*CreateAmExtractor)(
                const sp<DataSource> &source, const char *mime);
        CreateAmExtractor createAmExtractor =
                (CreateAmExtractor)dlsym(
                        gAmLibHandle, "createAmMediaExtractor");
        if (createAmExtractor) {
            sp<MediaExtractor> AmExtractor =
                    (*createAmExtractor)(source, mime);
            if (AmExtractor != NULL) {
                return AmExtractor;
            }
        }
    }
    return NULL;
}

}  // android
