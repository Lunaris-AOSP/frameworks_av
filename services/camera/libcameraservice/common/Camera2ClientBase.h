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

#ifndef ANDROID_SERVERS_CAMERA_CAMERA2CLIENT_BASE_H
#define ANDROID_SERVERS_CAMERA_CAMERA2CLIENT_BASE_H

#include "common/CameraDeviceBase.h"
#include "camera/CameraMetadata.h"
#include "camera/CaptureResult.h"
#include "utils/CameraServiceProxyWrapper.h"
#include "utils/AttributionAndPermissionUtils.h"
#include "CameraServiceWatchdog.h"

namespace android {

class IMemory;

class CameraService;

template <typename TClientBase>
class Camera2ClientBase :
        public TClientBase,
        public NotificationListener
{
public:
    typedef typename TClientBase::TCamCallbacks TCamCallbacks;

    /**
     * Base binder interface (see ICamera/ICameraDeviceUser for details)
     */
    virtual status_t       connect(const sp<TCamCallbacks>& callbacks);
    virtual binder::Status disconnect();

    /**
     * Interface used by CameraService
     */

    // TODO: too many params, move into a ClientArgs<T>
    Camera2ClientBase(const sp<CameraService>& cameraService,
                      const sp<TCamCallbacks>& remoteCallback,
                      std::shared_ptr<CameraServiceProxyWrapper> cameraServiceProxyWrapper,
                      std::shared_ptr<AttributionAndPermissionUtils> attributionAndPermissionUtils,
                      const AttributionSourceState& clientAttribution, int callingPid,
                      bool systemNativeClient, const std::string& cameraId, int api1CameraId,
                      int cameraFacing, int sensorOrientation, int servicePid,
                      bool overrideForPerfClass, int rotationOverride,  bool sharedMode,
                      bool isVendorClient, bool legacyClient = false);
    virtual ~Camera2ClientBase();

    virtual status_t      initialize(sp<CameraProviderManager> manager,
            const std::string& monitorTags) override;
    virtual status_t      dumpClient(int fd, const Vector<String16>& args) override;
    virtual status_t      startWatchingTags(const std::string &tags, int out) override;
    virtual status_t      stopWatchingTags(int out) override;
    virtual status_t      dumpWatchedEventsToVector(std::vector<std::string> &out) override;

    /**
     * NotificationListener implementation
     */

    virtual void          notifyError(int32_t errorCode,
                                      const CaptureResultExtras& resultExtras);
    virtual void          notifyPhysicalCameraChange(const std::string &physicalId) override;
    // Returns errors on app ops permission failures
    virtual status_t      notifyActive(float maxPreviewFps);
    virtual void          notifyIdle(int64_t /*requestCount*/, int64_t /*resultErrorCount*/,
                                     bool /*deviceError*/,
                                     std::pair<int32_t, int32_t> /*mostRequestedFpsRange*/,
                                     const std::vector<hardware::CameraStreamStats>&) {}
    virtual void          notifyShutter(const CaptureResultExtras& resultExtras,
                                        nsecs_t timestamp);
    virtual void          notifyAutoFocus(uint8_t newState, int triggerId);
    virtual void          notifyAutoExposure(uint8_t newState, int triggerId);
    virtual void          notifyAutoWhitebalance(uint8_t newState,
                                                 int triggerId);
    virtual void          notifyPrepared(int streamId);
    virtual void          notifyRequestQueueEmpty();
    virtual void          notifyRepeatingRequestError(long lastFrameNumber);
    virtual void          notifyClientSharedAccessPriorityChanged(bool primaryClient) override;

    void                  notifyIdleWithUserTag(int64_t requestCount, int64_t resultErrorCount,
                                     bool deviceError,
                                     std::pair<int32_t, int32_t> mostRequestedFpsRange,
                                     const std::vector<hardware::CameraStreamStats>& streamStats,
                                     const std::string& userTag, int videoStabilizationMode,
                                     bool usedUltraWide, bool usedZoomOverride);

    int                   getCameraId() const;
    const sp<CameraDeviceBase>&
                          getCameraDevice();
    int                   getCameraDeviceVersion() const;
    const sp<CameraService>&
                          getCameraService();

    /**
     * Interface used by independent components of CameraClient2Base.
     */

    // Simple class to ensure that access to TCamCallbacks is serialized
    // by requiring mRemoteCallbackLock to be locked before access to
    // mRemoteCallback is possible.
    class SharedCameraCallbacks {
      public:
        class Lock {
          public:
            explicit Lock(SharedCameraCallbacks &client);
            ~Lock();
            sp<TCamCallbacks> &mRemoteCallback;
          private:
            SharedCameraCallbacks &mSharedClient;
        };
        explicit SharedCameraCallbacks(const sp<TCamCallbacks>& client);
        SharedCameraCallbacks& operator=(const sp<TCamCallbacks>& client);
        void clear();
      private:
        sp<TCamCallbacks> mRemoteCallback;
        mutable Mutex mRemoteCallbackLock;
    } mSharedCameraCallbacks;

    status_t      injectCamera(const std::string& injectedCamId,
                               sp<CameraProviderManager> manager) override;
    status_t      stopInjection() override;

    status_t      injectSessionParams(const CameraMetadata& sessionParams) override;

protected:

    // The PID provided in the constructor call
    pid_t mInitialClientPid;
    bool mOverrideForPerfClass = false;
    bool mLegacyClient = false;
    bool mIsVendorClient = false;
    std::shared_ptr<CameraServiceProxyWrapper> mCameraServiceProxyWrapper;

    virtual sp<IBinder> asBinderWrapper() {
        return IInterface::asBinder(this);
    }

    virtual status_t      dumpDevice(int fd, const Vector<String16>& args);

    /** Binder client interface-related private members */

    // Mutex that must be locked by methods implementing the binder client
    // interface. Ensures serialization between incoming client calls.
    // All methods in this class hierarchy that append 'L' to the name assume
    // that mBinderSerializationLock is locked when they're called
    mutable Mutex         mBinderSerializationLock;

    /** CameraDeviceBase instance wrapping HAL3+ entry */

    // Note: This was previously set to const to avoid mDevice being updated -
    // b/112639939 (update of sp<> is racy) during dumpDevice (which is important to be lock free
    // for debugging purpose). The const has been removed since CameraDeviceBase
    // needs to be set during initializeImpl(). This must not be set / cleared
    // anywhere else.
    sp<CameraDeviceBase>  mDevice;

    /** Utility members */

    // Verify that caller is the owner of the camera
    status_t              checkPid(const char *checkLocation) const;

    virtual void          detachDevice();

    bool                  mDeviceActive;

    const int             mApi1CameraId; // -1 if client is API2

private:
    template<typename TProviderPtr>
    status_t              initializeImpl(TProviderPtr providerPtr, const std::string& monitorTags);

    binder::Status disconnectImpl();

};

}; // namespace android

#endif
