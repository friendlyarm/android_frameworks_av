/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "wfd"
#include <utils/Log.h>

#include "source/WifiDisplaySource.h"

#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <media/AudioSystem.h>
#include <media/IMediaPlayerService.h>
#include <media/IRemoteDisplay.h>
#include <media/IRemoteDisplayClient.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <ui/DisplayInfo.h>

#include <media/AudioSystem.h>   //for audio policy

#include <fcntl.h>
#include <cutils/properties.h>

namespace android {

static void usage(const char *me) {
    fprintf(stderr,
            "usage:\n"
            "           %s -l iface[:port]\tcreate a wifi display source\n"
            "               -f(ilename)  \tstream media\n",
            me);
}

int osd_blank(const char *path,int cmd)
{
    int fd;
    char  bcmd[16];
    fd = open(path, O_CREAT|O_RDWR | O_TRUNC, 0644);

    if (fd >= 0) {
        sprintf(bcmd,"%d",cmd);
        write(fd,bcmd,strlen(bcmd));
        close(fd);
        return 0;
    }

    return -1;
}

struct RemoteDisplayClient : public BnRemoteDisplayClient {
    RemoteDisplayClient();

    virtual void onDisplayConnected(
            const sp<IGraphicBufferProducer> &bufferProducer,
            uint32_t width,
            uint32_t height,
            uint32_t flags,
            uint32_t session);

    virtual void onDisplayDisconnected();
    virtual void onDisplayError(int32_t error);

    void waitUntilDone();

protected:
    virtual ~RemoteDisplayClient();

private:
    Mutex mLock;
    Condition mCondition;

    bool mDone;

    sp<SurfaceComposerClient> mComposerClient;
    sp<IGraphicBufferProducer> mSurfaceTexture;
    sp<IBinder> mDisplayBinder;

    DISALLOW_EVIL_CONSTRUCTORS(RemoteDisplayClient);
};

RemoteDisplayClient::RemoteDisplayClient()
    : mDone(false) {
    mComposerClient = new SurfaceComposerClient;
    CHECK_EQ(mComposerClient->initCheck(), (status_t)OK);
}

RemoteDisplayClient::~RemoteDisplayClient() {
}

void RemoteDisplayClient::onDisplayConnected(
        const sp<IGraphicBufferProducer> &bufferProducer,
        uint32_t width,
        uint32_t height,
        uint32_t flags,
        uint32_t /*session*/) {
    ALOGI("onDisplayConnected width=%u, height=%u, flags = 0x%08x",
          width, height, flags);

    if (bufferProducer != NULL) {
        mSurfaceTexture = bufferProducer;
        mDisplayBinder = mComposerClient->createDisplay(
                String8("foo"), false /* secure */);

        SurfaceComposerClient::openGlobalTransaction();
        mComposerClient->setDisplaySurface(mDisplayBinder, mSurfaceTexture);

        Rect layerStackRect(1280, 720);  // XXX fix this.
        Rect displayRect(1280, 720);

        mComposerClient->setDisplayProjection(
                mDisplayBinder, 0 /* 0 degree rotation */,
                layerStackRect,
                displayRect);

        SurfaceComposerClient::closeGlobalTransaction();
    }
}

void RemoteDisplayClient::onDisplayDisconnected() {
    ALOGI("onDisplayDisconnected");

    Mutex::Autolock autoLock(mLock);
    mDone = true;
    mCondition.broadcast();
}

void RemoteDisplayClient::onDisplayError(int32_t error) {
    ALOGI("onDisplayError error=%d", error);

    Mutex::Autolock autoLock(mLock);
    mDone = true;
    mCondition.broadcast();
}

void RemoteDisplayClient::waitUntilDone() {
    Mutex::Autolock autoLock(mLock);
    while (!mDone) {
        mCondition.wait(mLock);
    }
}

static status_t enableAudioSubmix(bool enable) {
    status_t err = AudioSystem::setDeviceConnectionState(
            AUDIO_DEVICE_IN_REMOTE_SUBMIX,
            enable
                ? AUDIO_POLICY_DEVICE_STATE_AVAILABLE
                : AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
            NULL /* device_address */);

    if (err != OK) {
        return err;
    }

    err = AudioSystem::setDeviceConnectionState(
            AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
            enable
                ? AUDIO_POLICY_DEVICE_STATE_AVAILABLE
                : AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
            NULL /* device_address */);

    return err;
}

static void createSource(const AString &addr, int32_t port) {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.player"));
    sp<IMediaPlayerService> service =
        interface_cast<IMediaPlayerService>(binder);

    CHECK(service.get() != NULL);

    enableAudioSubmix(true /* enable */);

    String8 iface;
    iface.append(addr.c_str());
    iface.append(StringPrintf(":%d", port).c_str());

    sp<RemoteDisplayClient> client = new RemoteDisplayClient;
    sp<IRemoteDisplay> display = service->listenForRemoteDisplay(client, iface);

    client->waitUntilDone();

    display->dispose();
    display.clear();

    enableAudioSubmix(false /* enable */);
}

static void createFileSource(
        const AString &addr, int32_t port, const char *path) {
    sp<ANetworkSession> session = new ANetworkSession;
    session->start();

    sp<ALooper> looper = new ALooper;
    looper->start();

    sp<RemoteDisplayClient> client = new RemoteDisplayClient;
    sp<WifiDisplaySource> source = new WifiDisplaySource(session, client, path);
    looper->registerHandler(source);

    AString iface = StringPrintf("%s:%d", addr.c_str(), port);
    CHECK_EQ((status_t)OK, source->start(iface.c_str()));

    client->waitUntilDone();

    source->stop();
}

}  // namespace android

int main(int argc, char **argv) {
    using namespace android;

    ProcessState::self()->startThreadPool();

    DataSource::RegisterDefaultSniffers();

    AString connectToHost;
    int32_t connectToPort = -1;
    AString uri;
    AString source_ip;

    property_set("media.libplayer.fastswitch", "2");

    char cmd[128];

    AString listenOnAddr;
    int32_t listenOnPort = -1;

    AString path;

    int res;
    int wfd_sorce_mode_enabled = 0;

    while ((res = getopt(argc, argv, "hc:l:u:i:")) >= 0) {
    //while ((res = getopt(argc, argv, "hl:f:")) >= 0) {
        switch (res) {
            case 'f':
            {
                path = optarg;
                break;
            }
            case 'i':
            {
                source_ip = optarg;
                wfd_sorce_mode_enabled = 1;
                break;
            }
            case 'l':
            {
                const char *colonPos = strrchr(optarg, ':');

                if (colonPos == NULL) {
                    listenOnAddr = optarg;
                    listenOnPort = WifiDisplaySource::kWifiDisplayDefaultPort;
                } else {
                    listenOnAddr.setTo(optarg, colonPos - optarg);

                    char *end;
                    listenOnPort = strtol(colonPos + 1, &end, 10);

                    if (*end != '\0' || end == colonPos + 1
                            || listenOnPort < 1 || listenOnPort > 65535) {
                        fprintf(stderr, "Illegal port specified.\n");
                        exit(1);
                    }
                }
                break;
            }
            case '?':
            case 'h':
            default:
                usage(argv[0]);
                exit(1);
        }
    }

    if (listenOnPort >= 0) {
        createSource(listenOnAddr, listenOnPort);
        exit(0);
    }

    if (connectToPort < 0 && uri.empty() && source_ip.empty()) {
        fprintf(stderr,
                "You need to select either source host or uri.\n");

        exit(1);
    }

    if (connectToPort >= 0 && !uri.empty()) {
        fprintf(stderr,
                "You need to either connect to a wfd host or an rtsp url, "
                "not both.\n");
        exit(1);
    }

    sp<ANetworkSession> session = new ANetworkSession;
    session->start();

    sp<ALooper> looper = new ALooper;
    sp<IRemoteDisplayClient> client = NULL;
    sp<WifiDisplaySource> source;

    if (wfd_sorce_mode_enabled) {
        fprintf(stderr, "!!! RUN AS WFD SOURCE !!!\n");

        AudioSystem::setDeviceConnectionState(AUDIO_DEVICE_IN_REMOTE_SUBMIX, AUDIO_POLICY_DEVICE_STATE_AVAILABLE, 0);
        AudioSystem::setDeviceConnectionState(AUDIO_DEVICE_OUT_REMOTE_SUBMIX, AUDIO_POLICY_DEVICE_STATE_AVAILABLE, 0);

        source = new WifiDisplaySource(session, client);
        looper->registerHandler(source);

        source->start(source_ip.c_str());

        looper->start(false /* runOnCallingThread */);
    }

    fprintf(stderr, "input q/Q to quit process\n");

    do {
        memset(cmd, 0, sizeof(cmd));
        scanf("%s", cmd);
        fprintf(stderr, "cmd=%s\n",cmd);

        if ((strcmp(cmd,"q") == 0) || (strcmp(cmd,"Q") == 0))
            break;

        usleep(200000);
    } while(1);

    fprintf(stderr, "release resource\n");

    if (wfd_sorce_mode_enabled) {
        source->stop();
        AudioSystem::setDeviceConnectionState(AUDIO_DEVICE_IN_REMOTE_SUBMIX, AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE, 0);
        AudioSystem::setDeviceConnectionState(AUDIO_DEVICE_OUT_REMOTE_SUBMIX, AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE, 0);
    } else {
        osd_blank("/sys/class/graphics/fb0/blank",0);//clear all osd0 ,don't need it on APK
        osd_blank("/sys/class/graphics/fb1/blank",0);//clear all osd1 ,don't need it on APK
    }

    looper->stop();
    session->stop();
    fprintf(stderr, "release resource done\n");
    return 0;
}
