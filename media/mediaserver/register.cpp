/*
 * Copyright (C) 2013 The Android Open Source Project
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
#define LOG_TAG "Register"
#include <utils/Log.h>
#include <cutils/properties.h>
#include <utils/threads.h>
#include <utils/KeyedVector.h>

#include "RegisterExtensions.h"
#include "../libmediaplayerservice/SharedLibrary.h"

namespace android {

static sp<SharedLibrary> libamlogicmedia;
static sp<SharedLibrary> libamlogicScreenMediaSource;

static bool  LoadAndInitAmlogicMediaFactory(void)
{
    sp<SharedLibrary> mLibrary;
    int err;
    String8 name("libmedia_amlogic.so");
    mLibrary = new SharedLibrary(name);
    if (!*mLibrary) {
       ALOGE("load libmedia_amlogic.so for amlogicmedia failed:%s", mLibrary->lastError());
       return false;
    }
    typedef int (*init_fun)(void);

    init_fun init =
        (init_fun)mLibrary->lookup("_ZN7android23AmlogicMediaFactoryInitEv");

    if (init == NULL) {
       ALOGE("AmlogicMediaFactoryInit failed:%s", mLibrary->lastError());
       mLibrary.clear();
       return false;
    }
    err=init();
    if (err != 0) {
        ALOGE("AmlogicMediaFactoryInit failed:%s", mLibrary->lastError());
        return false;
    }

    init = (init_fun)mLibrary->lookup("_ZN7android35AmlogicMetadataRetrieverFactoryInitEv");

    if (init == NULL) {
       ALOGE("AmlogicMetadataRetrieverFactoryInit failed:%s", mLibrary->lastError());
       return false;
    }
    err = init();
    if (err != 0) {
        ALOGE("AmlogicMetadataRetrieverFactoryInit failed:%s", mLibrary->lastError());
        return false;
    }

   libamlogicmedia =mLibrary;
   return true;
}

static bool LoadAndInitAmlogicScreenMediaSource()
{
    sp<SharedLibrary> mLibrary;
    int err;
    String8 name("libstagefright_screenmediasource.so");
    mLibrary = new SharedLibrary(name);
    if (!*mLibrary) {
        ALOGE("load libstagefright_screenmediasource.so for libstagefright_screenmediasource failed:%s", mLibrary->lastError());
        return false;
    }
    typedef int (*init_fun)(void);

    init_fun init = (init_fun)mLibrary->lookup("_ZN7android28AmlogicScreenMediaSourceInitEv");

    if (init == NULL) {
        ALOGE("AmlogicScreenMediaSourceInit failed:%s", mLibrary->lastError());
        mLibrary.clear();
        return false;
    }
    err=init();
    if (err != 0) {
        ALOGE("AmlogicScreenMediaSourceInit failed:%s", mLibrary->lastError());
        return false;
    }
    libamlogicScreenMediaSource = mLibrary;
    return true;
}
}

void registerExtensions()
{
    android::LoadAndInitAmlogicMediaFactory();
    android::LoadAndInitAmlogicScreenMediaSource();
}


