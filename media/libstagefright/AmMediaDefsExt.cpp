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

#define __STDINT_LIMITS
#define __STDC_LIMIT_MACROS
#include <stdint.h>

#include <media/stagefright/AmMediaDefsExt.h>

namespace android {

const int64_t kUnknownPTS = INT64_MIN;

const char *MEDIA_MIMETYPE_VIDEO_MJPEG = "video/mjpeg";
const char *MEDIA_MIMETYPE_VIDEO_MSMPEG4 = "video/x-msmpeg";
const char *MEDIA_MIMETYPE_VIDEO_SORENSON_SPARK = "video/x-sorenson-spark";
const char *MEDIA_MIMETYPE_VIDEO_WMV = "video/x-ms-wmv";
const char *MEDIA_MIMETYPE_VIDEO_VC1 = "video/vc1";
const char *MEDIA_MIMETYPE_VIDEO_WVC1 = "video/wvc1";

const char *MEDIA_MIMETYPE_VIDEO_VP6 = "video/x-vnd.on2.vp6";
const char *MEDIA_MIMETYPE_VIDEO_VP6F = "video/x-vnd.on2.vp6f";
const char *MEDIA_MIMETYPE_VIDEO_VP6A = "video/x-vnd.on2.vp6a";

const char *MEDIA_MIMETYPE_AUDIO_DTS = "audio/dtshd";
const char *MEDIA_MIMETYPE_AUDIO_MP1 = "audio/mp1";
const char *MEDIA_MIMETYPE_AUDIO_MP2 = "audio/mp2";

const char *MEDIA_MIMETYPE_TEXT_TTML = "application/ttml+xml";

const char *MEDIA_MIMETYPE_CONTAINER_ASF = "video/x-ms-asf";
const char *MEDIA_MIMETYPE_CONTAINER_FLV = "video/x-flv";

}  // namespace android
