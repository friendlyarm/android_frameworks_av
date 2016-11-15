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

#ifndef AM_MEDIA_EXTRACTOR_PLUGIN_H_

#define AM_MEDIA_EXTRACTOR_PLUGIN_H_

#include <utils/StrongPointer.h>

namespace android {

class AMessage;
class DataSource;
class MediaExtractor;
class String8;

bool sniffAmExtFormat(
        const sp<DataSource> &source, String8 *mimeType,
        float *confidence, sp<AMessage> *meta);

sp<MediaExtractor> createAmMediaExtractor(
        const sp<DataSource> &source, const char *mime);

}  // namespace android

#endif  // AM_MEDIA_EXTRACTOR_PLUGIN_H_

