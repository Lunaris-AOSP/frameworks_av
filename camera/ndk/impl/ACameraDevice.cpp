/*
 * Copyright (C) 2015 The Android Open Source Project
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
#define LOG_TAG "ACameraDevice"

#include <vector>
#include <inttypes.h>
#include <android/hardware/ICameraService.h>
#include <gui/Surface.h>
#include <camera/StringUtils.h>
#include "ACameraDevice.h"
#include "ACameraMetadata.h"
#include "ACaptureRequest.h"
#include "ACameraCaptureSession.h"
#include <com_android_internal_camera_flags.h>

namespace flags = com::android::internal::camera::flags;

ACameraDevice::~ACameraDevice() {
    mDevice->stopLooperAndDisconnect();
}

namespace android {
namespace acam {

using android::hardware::common::fmq::MQDescriptor;

// Static member definitions
const char* CameraDevice::kContextKey        = "Context";
const char* CameraDevice::kDeviceKey         = "Device";
const char* CameraDevice::kErrorCodeKey      = "ErrorCode";
const char* CameraDevice::kCallbackFpKey     = "Callback";
const char* CameraDevice::kSessionSpKey      = "SessionSp";
const char* CameraDevice::kCaptureRequestKey = "CaptureRequest";
const char* CameraDevice::kTimeStampKey      = "TimeStamp";
const char* CameraDevice::kCaptureResultKey  = "CaptureResult";
const char* CameraDevice::kPhysicalCaptureResultKey = "PhysicalCaptureResult";
const char* CameraDevice::kCaptureFailureKey = "CaptureFailure";
const char* CameraDevice::kSequenceIdKey     = "SequenceId";
const char* CameraDevice::kFrameNumberKey    = "FrameNumber";
const char* CameraDevice::kAnwKey            = "Anw";
const char* CameraDevice::kFailingPhysicalCameraId= "FailingPhysicalCameraId";

/**
 * CameraDevice Implementation
 */
CameraDevice::CameraDevice(
        const char* id,
        ACameraDevice_StateCallbacks* cb,
        sp<ACameraMetadata> chars,
        ACameraDevice* wrapper, bool sharedMode) :
        mCameraId(id),
        mAppCallbacks(*cb),
        mChars(chars),
        mServiceCallback(new ServiceCallback(this)),
        mWrapper(wrapper),
        mSharedMode(sharedMode),
        mInError(false),
        mError(ACAMERA_OK),
        mIdle(true),
        mCurrentSession(nullptr) {
    mClosing = false;
    // Setup looper thread to perfrom device callbacks to app
    mCbLooper = new ALooper;
    mCbLooper->setName("C2N-dev-looper");
    status_t err = mCbLooper->start(
            /*runOnCallingThread*/false,
            /*canCallJava*/       true,
            PRIORITY_DEFAULT);
    if (err != OK) {
        ALOGE("%s: Unable to start camera device callback looper: %s (%d)",
                __FUNCTION__, strerror(-err), err);
        setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_DEVICE);
    }
    mHandler = new CallbackHandler(id);
    mCbLooper->registerHandler(mHandler);

    const CameraMetadata& metadata = mChars->getInternalData();
    camera_metadata_ro_entry entry = metadata.find(ANDROID_REQUEST_PARTIAL_RESULT_COUNT);
    if (entry.count != 1) {
        ALOGW("%s: bad count %zu for partial result count", __FUNCTION__, entry.count);
        mPartialResultCount = 1;
    } else {
        mPartialResultCount = entry.data.i32[0];
    }

    entry = metadata.find(ANDROID_LENS_INFO_SHADING_MAP_SIZE);
    if (entry.count != 2) {
        ALOGW("%s: bad count %zu for shading map size", __FUNCTION__, entry.count);
        mShadingMapSize[0] = 0;
        mShadingMapSize[1] = 0;
    } else {
        mShadingMapSize[0] = entry.data.i32[0];
        mShadingMapSize[1] = entry.data.i32[1];
    }

    size_t physicalIdCnt = 0;
    const char*const* physicalCameraIds;
    if (mChars->isLogicalMultiCamera(&physicalIdCnt, &physicalCameraIds)) {
        for (size_t i = 0; i < physicalIdCnt; i++) {
            mPhysicalIds.push_back(physicalCameraIds[i]);
        }
    }
}

CameraDevice::~CameraDevice() { }

void
CameraDevice::postSessionMsgAndCleanup(sp<AMessage>& msg) {
    msg->post();
    msg.clear();
    sp<AMessage> cleanupMsg = new AMessage(kWhatCleanUpSessions, mHandler);
    cleanupMsg->post();
}

// TODO: cached created request?
camera_status_t
CameraDevice::createCaptureRequest(
        ACameraDevice_request_template templateId,
        const ACameraIdList* physicalIdList,
        ACaptureRequest** request) const {
    Mutex::Autolock _l(mDeviceLock);

    if (physicalIdList != nullptr) {
        if (physicalIdList->numCameras > static_cast<int>(mPhysicalIds.size())) {
            ALOGE("%s: physicalIdList size %d exceeds number of available physical cameras %zu",
                    __FUNCTION__, physicalIdList->numCameras, mPhysicalIds.size());
            return ACAMERA_ERROR_INVALID_PARAMETER;
        }
        for (auto i = 0; i < physicalIdList->numCameras; i++) {
            if (physicalIdList->cameraIds[i] == nullptr) {
                ALOGE("%s: physicalId is null!", __FUNCTION__);
                return ACAMERA_ERROR_INVALID_PARAMETER;
            }
            if (mPhysicalIds.end() == std::find(
                    mPhysicalIds.begin(), mPhysicalIds.end(), physicalIdList->cameraIds[i])) {
                ALOGE("%s: Invalid physicalId %s!", __FUNCTION__, physicalIdList->cameraIds[i]);
                return ACAMERA_ERROR_INVALID_PARAMETER;
            }
        }
    }

    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        return ret;
    }
    if (mRemote == nullptr) {
        return ACAMERA_ERROR_CAMERA_DISCONNECTED;
    }
    CameraMetadata rawRequest;
    binder::Status remoteRet = mRemote->createDefaultRequest(templateId, &rawRequest);
    if (remoteRet.serviceSpecificErrorCode() ==
            hardware::ICameraService::ERROR_ILLEGAL_ARGUMENT) {
        ALOGW("Create capture request failed! template %d is not supported on this device",
            templateId);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    } else if (!remoteRet.isOk()) {
        ALOGE("Create capture request failed: %s", remoteRet.toString8().c_str());
        return ACAMERA_ERROR_UNKNOWN;
    }
    ACaptureRequest* outReq = new ACaptureRequest();
    outReq->settings = new ACameraMetadata(rawRequest.release(), ACameraMetadata::ACM_REQUEST);
    if (physicalIdList != nullptr) {
        for (auto i = 0; i < physicalIdList->numCameras; i++) {
            outReq->physicalSettings.emplace(physicalIdList->cameraIds[i],
                    new ACameraMetadata(*(outReq->settings)));
        }
    }
    outReq->targets  = new ACameraOutputTargets();
    *request = outReq;
    return ACAMERA_OK;
}

camera_status_t
CameraDevice::createCaptureSession(
        const ACaptureSessionOutputContainer*       outputs,
        const ACaptureRequest* sessionParameters,
        const ACameraCaptureSession_stateCallbacks* callbacks,
        /*out*/ACameraCaptureSession** session) {
    nsecs_t startTimeNs = systemTime();
    sp<ACameraCaptureSession> currentSession = mCurrentSession.promote();
    Mutex::Autolock _l(mDeviceLock);
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        return ret;
    }

    if (currentSession != nullptr) {
        currentSession->closeByDevice();
        stopRepeatingLocked();
    }

    // Create new session
    ret = configureStreamsLocked(outputs, sessionParameters, startTimeNs);
    if (ret != ACAMERA_OK) {
        ALOGE("Fail to create new session. cannot configure streams");
        return ret;
    }

    ACameraCaptureSession* newSession = new ACameraCaptureSession(
            mNextSessionId++, outputs, callbacks, this);

    // set new session as current session
    newSession->incStrong((void *) ACameraDevice_createCaptureSession);
    mCurrentSession = newSession;
    mFlushing = false;
    *session = newSession;
    return ACAMERA_OK;
}

camera_status_t CameraDevice::isSessionConfigurationSupported(
        const ACaptureSessionOutputContainer* sessionOutputContainer) const {
    Mutex::Autolock _l(mDeviceLock);
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        return ret;
    }

    SessionConfiguration sessionConfiguration(0 /*inputWidth*/, 0 /*inputHeight*/,
            -1 /*inputFormat*/, CAMERA3_STREAM_CONFIGURATION_NORMAL_MODE);
    for (const auto& output : sessionOutputContainer->mOutputs) {
        sp<SurfaceType> surface(nullptr);
        ret = getSurfacefromAnw(output.mWindow, surface);
        if (ret != ACAMERA_OK) {
            ALOGE("Camera device %s failed to extract graphic producer from native window",
                    getId());
            return ret;
        }

        ParcelableSurfaceType pSurface = flagtools::convertSurfaceTypeToParcelable(surface);
        OutputConfiguration outConfig(pSurface, output.mRotation, output.mPhysicalCameraId,
                OutputConfiguration::INVALID_SET_ID, true);

        for (auto& anw : output.mSharedWindows) {
            ret = getSurfacefromAnw(anw, surface);
            if (ret != ACAMERA_OK) {
                ALOGE("Camera device %s failed to extract graphic producer from native window",
                        getId());
                return ret;
            }
            pSurface = flagtools::convertSurfaceTypeToParcelable(surface);
            outConfig.addSurface(pSurface);
        }

        sessionConfiguration.addOutputConfiguration(outConfig);
    }

    bool supported = false;
    binder::Status remoteRet = mRemote->isSessionConfigurationSupported(
            sessionConfiguration, &supported);
    if (remoteRet.serviceSpecificErrorCode() ==
            hardware::ICameraService::ERROR_INVALID_OPERATION) {
        return ACAMERA_ERROR_UNSUPPORTED_OPERATION;
    } else if (!remoteRet.isOk()) {
        return ACAMERA_ERROR_UNKNOWN;
    } else {
        return supported ? ACAMERA_OK : ACAMERA_ERROR_STREAM_CONFIGURE_FAIL;
    }
}

camera_status_t CameraDevice::stopStreamingLocked() {
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        ALOGE("%s: camera is in closed or error state %d", __FUNCTION__, ret);
        return ret;
    }
    ret = stopRepeatingLocked();
    if (ret != ACAMERA_OK) {
        ALOGE("%s: error when trying to stop streaming %d", __FUNCTION__, ret);
        return ret;
    }
    for (auto& outputTarget : mPreviewRequestOutputs) {
        ACameraOutputTarget_free(outputTarget);
    }
    mPreviewRequestOutputs.clear();
    if (mPreviewRequest) {
        ACaptureRequest_free(mPreviewRequest);
        mPreviewRequest = nullptr;
    }
    return ACAMERA_OK;
}

camera_status_t CameraDevice::updateOutputConfigurationLocked(ACaptureSessionOutput *output) {
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        return ret;
    }

    if (output == nullptr) {
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }

    if (!output->mIsShared) {
        ALOGE("Error output configuration is not shared");
        return ACAMERA_ERROR_INVALID_OPERATION;
    }

    int32_t streamId = -1;
    for (auto& kvPair : mConfiguredOutputs) {
        if (kvPair.second.first == output->mWindow) {
            streamId = kvPair.first;
            break;
        }
    }
    if (streamId < 0) {
        ALOGE("Error: Invalid output configuration");
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }

    sp<SurfaceType> surface(nullptr);
    ret = getSurfacefromAnw(output->mWindow, surface);
    if (ret != ACAMERA_OK) {
        ALOGE("Camera device %s failed to extract graphic producer from native window",
                getId());
        return ret;
    }

    ParcelableSurfaceType pSurface = flagtools::convertSurfaceTypeToParcelable(surface);
    OutputConfiguration outConfig(pSurface, output->mRotation, output->mPhysicalCameraId,
                                  OutputConfiguration::INVALID_SET_ID, true);

    for (auto& anw : output->mSharedWindows) {
        ret = getSurfacefromAnw(anw, surface);
        if (ret != ACAMERA_OK) {
            ALOGE("Camera device %s failed to extract graphic producer from native window",
                    getId());
            return ret;
        }
        pSurface = flagtools::convertSurfaceTypeToParcelable(surface);
        outConfig.addSurface(pSurface);
    }

    auto remoteRet = mRemote->updateOutputConfiguration(streamId, outConfig);
    if (!remoteRet.isOk()) {
        switch (remoteRet.serviceSpecificErrorCode()) {
            case hardware::ICameraService::ERROR_INVALID_OPERATION:
                ALOGE("Camera device %s invalid operation: %s", getId(),
                        remoteRet.toString8().c_str());
                return ACAMERA_ERROR_INVALID_OPERATION;
                break;
            case hardware::ICameraService::ERROR_ALREADY_EXISTS:
                ALOGE("Camera device %s output surface already exists: %s", getId(),
                        remoteRet.toString8().c_str());
                return ACAMERA_ERROR_INVALID_PARAMETER;
                break;
            case hardware::ICameraService::ERROR_ILLEGAL_ARGUMENT:
                ALOGE("Camera device %s invalid input argument: %s", getId(),
                        remoteRet.toString8().c_str());
                return ACAMERA_ERROR_INVALID_PARAMETER;
                break;
            default:
                ALOGE("Camera device %s failed to add shared output: %s", getId(),
                        remoteRet.toString8().c_str());
                return ACAMERA_ERROR_UNKNOWN;
        }
    }
    mConfiguredOutputs[streamId] = std::make_pair(output->mWindow, outConfig);

    return ACAMERA_OK;
}

camera_status_t CameraDevice::prepareLocked(ANativeWindow *window) {
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        return ret;
    }

    if (window == nullptr) {
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }

    int32_t streamId = -1;
    for (auto& kvPair : mConfiguredOutputs) {
        if (window == kvPair.second.first) {
            streamId = kvPair.first;
            break;
        }
    }
    if (streamId < 0) {
        ALOGE("Error: Invalid output configuration");
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    auto remoteRet = mRemote->prepare(streamId);
    if (!remoteRet.isOk()) {
        // TODO:(b/259735869) Do this check for all other binder calls in the
        // ndk as well.
        if (remoteRet.exceptionCode() != EX_SERVICE_SPECIFIC) {
            ALOGE("Camera device %s failed to prepare output window %p: %s", getId(), window,
                    remoteRet.toString8().c_str());
            return ACAMERA_ERROR_UNKNOWN;

        }
        switch (remoteRet.serviceSpecificErrorCode()) {
            case hardware::ICameraService::ERROR_INVALID_OPERATION:
                ALOGE("Camera device %s invalid operation: %s", getId(),
                        remoteRet.toString8().c_str());
                return ACAMERA_ERROR_INVALID_OPERATION;
                break;
            case hardware::ICameraService::ERROR_ILLEGAL_ARGUMENT:
                ALOGE("Camera device %s invalid input argument: %s", getId(),
                        remoteRet.toString8().c_str());
                return ACAMERA_ERROR_INVALID_PARAMETER;
                break;
            default:
                ALOGE("Camera device %s failed to prepare output window %p: %s", getId(), window,
                        remoteRet.toString8().c_str());
                return ACAMERA_ERROR_UNKNOWN;
        }
    }

    return ACAMERA_OK;
}

camera_status_t
CameraDevice::allocateCaptureRequest(
        const ACaptureRequest* request, /*out*/sp<CaptureRequest>& outReq) {
    camera_status_t ret;
    sp<CaptureRequest> req(new CaptureRequest());
    req->mPhysicalCameraSettings.push_back({getId(),
            request->settings->getInternalData()});
    for (auto& entry : request->physicalSettings) {
        req->mPhysicalCameraSettings.push_back({entry.first,
                entry.second->getInternalData()});
    }
    req->mIsReprocess = false; // NDK does not support reprocessing yet
    req->mContext = request->context;
    req->mSurfaceConverted = true; // set to true, and fill in stream/surface idx to speed up IPC

    for (auto outputTarget : request->targets->mOutputs) {
        ANativeWindow* anw = outputTarget.mWindow;
        sp<Surface> surface;
        ret = getSurfaceFromANativeWindow(anw, surface);
        if (ret != ACAMERA_OK) {
            ALOGE("Bad output target in capture request! ret %d", ret);
            return ret;
        }
        req->mSurfaceList.push_back(surface);

        bool found = false;
        // lookup stream/surface ID
        for (const auto& kvPair : mConfiguredOutputs) {
            int streamId = kvPair.first;
            const OutputConfiguration& outConfig = kvPair.second.second;
            const auto& surfaces = outConfig.getSurfaces();
            for (int surfaceId = 0; surfaceId < (int)surfaces.size(); surfaceId++) {
                if (surfaces[surfaceId] == flagtools::surfaceToSurfaceType(surface)) {
                    found = true;
                    req->mStreamIdxList.push_back(streamId);
                    req->mSurfaceIdxList.push_back(surfaceId);
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        if (!found) {
            ALOGE("Unconfigured output target %p in capture request!", anw);
            return ret;
        }
    }

    outReq = req;
    return ACAMERA_OK;
}

ACaptureRequest*
CameraDevice::allocateACaptureRequest(sp<CaptureRequest>& req, const std::string& deviceId) {
    ACaptureRequest* pRequest = new ACaptureRequest();
    for (auto& entry : req->mPhysicalCameraSettings) {
        CameraMetadata clone = entry.settings;
        if (entry.id == deviceId) {
            pRequest->settings = new ACameraMetadata(clone.release(), ACameraMetadata::ACM_REQUEST);
        } else {
            pRequest->physicalSettings.emplace(entry.id,
                    new ACameraMetadata(clone.release(), ACameraMetadata::ACM_REQUEST));
        }
    }
    pRequest->targets  = new ACameraOutputTargets();
    for (size_t i = 0; i < req->mSurfaceList.size(); i++) {
        ANativeWindow* anw = static_cast<ANativeWindow*>(req->mSurfaceList[i].get());
        ACameraOutputTarget outputTarget(anw);
        pRequest->targets->mOutputs.insert(outputTarget);
    }
    pRequest->context = req->mContext;
    return pRequest;
}

void
CameraDevice::freeACaptureRequest(ACaptureRequest* req) {
    if (req == nullptr) {
        return;
    }
    req->settings.clear();
    req->physicalSettings.clear();
    delete req->targets;
    delete req;
}

void
CameraDevice::notifySessionEndOfLifeLocked(ACameraCaptureSession* session) {
    if (isClosed()) {
        // Device is closing already. do nothing
        return;
    }

    if (mCurrentSession != session) {
        // Session has been replaced by other seesion or device is closed
        return;
    }
    mCurrentSession = nullptr;

    // Should not happen
    if (!session->mIsClosed) {
        ALOGE("Error: unclosed session %p reaches end of life!", session);
        setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_DEVICE);
        return;
    }

    // No new session, unconfigure now
    // Note: The unconfiguration of session won't be accounted for session
    // latency because a stream configuration with 0 streams won't ever become
    // active.
    nsecs_t startTimeNs = systemTime();
    camera_status_t ret = configureStreamsLocked(nullptr, nullptr, startTimeNs);
    if (ret != ACAMERA_OK) {
        ALOGE("Unconfigure stream failed. Device might still be configured! ret %d", ret);
    }
}

void
CameraDevice::disconnectLocked(sp<ACameraCaptureSession>& session) {
    if (mClosing.exchange(true)) {
        // Already closing, just return
        ALOGW("Camera device %s is already closing.", getId());
        return;
    }

    if (mRemote != nullptr) {
        mRemote->disconnect();
    }
    mRemote = nullptr;

    if (session != nullptr) {
        session->closeByDevice();
    }
}

camera_status_t
CameraDevice::stopRepeatingLocked() {
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        ALOGE("Camera %s stop repeating failed! ret %d", getId(), ret);
        return ret;
    }
    if (mRepeatingSequenceId != REQUEST_ID_NONE) {
        int repeatingSequenceId = mRepeatingSequenceId;
        mRepeatingSequenceId = REQUEST_ID_NONE;

        int64_t lastFrameNumber;
        binder::Status remoteRet = mRemote->cancelRequest(repeatingSequenceId, &lastFrameNumber);
        if (remoteRet.serviceSpecificErrorCode() ==
                hardware::ICameraService::ERROR_ILLEGAL_ARGUMENT) {
            ALOGV("Repeating request is already stopped.");
            return ACAMERA_OK;
        } else if (!remoteRet.isOk()) {
            ALOGE("Stop repeating request fails in remote: %s", remoteRet.toString8().c_str());
            return ACAMERA_ERROR_UNKNOWN;
        }
        checkRepeatingSequenceCompleteLocked(repeatingSequenceId, lastFrameNumber);
    }
    return ACAMERA_OK;
}

camera_status_t
CameraDevice::flushLocked(ACameraCaptureSession* session) {
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        ALOGE("Camera %s abort captures failed! ret %d", getId(), ret);
        return ret;
    }

    // This should never happen because creating a new session will close
    // previous one and thus reject any API call from previous session.
    // But still good to check here in case something unexpected happen.
    if (mCurrentSession != session) {
        ALOGE("Camera %s session %p is not current active session!", getId(), session);
        return ACAMERA_ERROR_INVALID_OPERATION;
    }

    if (mFlushing) {
        ALOGW("Camera %s is already aborting captures", getId());
        return ACAMERA_OK;
    }

    mFlushing = true;

    // Send onActive callback to guarantee there is always active->ready transition
    sp<AMessage> msg = new AMessage(kWhatSessionStateCb, mHandler);
    msg->setPointer(kContextKey, session->mUserSessionCallback.context);
    msg->setObject(kSessionSpKey, session);
    msg->setPointer(kCallbackFpKey, (void*) session->mUserSessionCallback.onActive);
    postSessionMsgAndCleanup(msg);

    // If device is already idling, send callback and exit early
    if (mIdle) {
        sp<AMessage> msg = new AMessage(kWhatSessionStateCb, mHandler);
        msg->setPointer(kContextKey, session->mUserSessionCallback.context);
        msg->setObject(kSessionSpKey, session);
        msg->setPointer(kCallbackFpKey, (void*) session->mUserSessionCallback.onReady);
        postSessionMsgAndCleanup(msg);
        mFlushing = false;
        return ACAMERA_OK;
    }

    int64_t lastFrameNumber;
    binder::Status remoteRet = mRemote->flush(&lastFrameNumber);
    if (!remoteRet.isOk()) {
        ALOGE("Abort captures fails in remote: %s", remoteRet.toString8().c_str());
        return ACAMERA_ERROR_UNKNOWN;
    }
    if (mRepeatingSequenceId != REQUEST_ID_NONE) {
        checkRepeatingSequenceCompleteLocked(mRepeatingSequenceId, lastFrameNumber);
    }
    return ACAMERA_OK;
}

camera_status_t
CameraDevice::waitUntilIdleLocked() {
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        ALOGE("Wait until camera %s idle failed! ret %d", getId(), ret);
        return ret;
    }

    if (mRepeatingSequenceId != REQUEST_ID_NONE) {
        ALOGE("Camera device %s won't go to idle when there is repeating request!", getId());
        return ACAMERA_ERROR_INVALID_OPERATION;
    }

    binder::Status remoteRet = mRemote->waitUntilIdle();
    if (!remoteRet.isOk()) {
        ALOGE("Camera device %s waitUntilIdle failed: %s", getId(), remoteRet.toString8().c_str());
        // TODO: define a function to convert status_t -> camera_status_t
        return ACAMERA_ERROR_UNKNOWN;
    }

    return ACAMERA_OK;
}

camera_status_t CameraDevice::getSurfacefromAnw(ANativeWindow* anw, sp<SurfaceType>& out) {
    sp<Surface> surface;
    camera_status_t ret = getSurfaceFromANativeWindow(anw, surface);
    if (ret != ACAMERA_OK) {
        return ret;
    }
    out = flagtools::surfaceToSurfaceType(surface);
    return ACAMERA_OK;
}

camera_status_t
CameraDevice::getSurfaceFromANativeWindow(
        ANativeWindow* anw, sp<Surface>& out) {
    if (anw == nullptr) {
        ALOGE("Error: output ANativeWindow is null");
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    int value;
    int err = (*anw->query)(anw, NATIVE_WINDOW_CONCRETE_TYPE, &value);
    if (err != OK || value != NATIVE_WINDOW_SURFACE) {
        ALOGE("Error: ANativeWindow is not backed by Surface!");
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    sp<Surface> surface(static_cast<Surface*>(anw));
    out = surface;
    return ACAMERA_OK;
}

camera_status_t
CameraDevice::configureStreamsLocked(const ACaptureSessionOutputContainer* outputs,
        const ACaptureRequest* sessionParameters, nsecs_t startTimeNs) {
    ACaptureSessionOutputContainer emptyOutput;
    if (outputs == nullptr) {
        outputs = &emptyOutput;
    }

    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        return ret;
    }

    std::set<std::pair<ANativeWindow*, OutputConfiguration>> outputSet;
    for (const auto& outConfig : outputs->mOutputs) {
        ANativeWindow* anw = outConfig.mWindow;
        sp<SurfaceType> surface(nullptr);
        ret = getSurfacefromAnw(anw, surface);
        if (ret != ACAMERA_OK) {
            return ret;
        }
        // Surface sharing cannot be enabled when a camera has been opened
        // in shared mode.
        if (flags::camera_multi_client() && mSharedMode && outConfig.mIsShared) {
            return ACAMERA_ERROR_INVALID_PARAMETER;
        }
        ParcelableSurfaceType pSurface = flagtools::convertSurfaceTypeToParcelable(surface);
        outputSet.insert(std::make_pair(
                anw,
                OutputConfiguration(pSurface, outConfig.mRotation, outConfig.mPhysicalCameraId,
                                    OutputConfiguration::INVALID_SET_ID, outConfig.mIsShared)));
    }
    auto addSet = outputSet;
    std::vector<int> deleteList;

    // Determine which streams need to be created, which to be deleted
    for (auto& kvPair : mConfiguredOutputs) {
        int streamId = kvPair.first;
        auto& outputPair = kvPair.second;
        if (outputSet.count(outputPair) == 0) {
            deleteList.push_back(streamId); // Need to delete a no longer needed stream
        } else {
            addSet.erase(outputPair);        // No need to add already existing stream
        }
    }

    ret = stopRepeatingLocked();
    if (ret != ACAMERA_OK) {
        ALOGE("Camera device %s stop repeating failed, ret %d", getId(), ret);
        return ret;
    }

    // If device is opened in shared mode, there can be multiple clients accessing the
    // camera device. So do not wait for idle if the device is opened in shared mode.
    if ((!flags::camera_multi_client()) || (!mSharedMode)) {
        ret = waitUntilIdleLocked();
        if (ret != ACAMERA_OK) {
            ALOGE("Camera device %s wait until idle failed, ret %d", getId(), ret);
            return ret;
        }
    }

    // Send onReady to previous session
    // CurrentSession will be updated after configureStreamLocked, so here
    // mCurrentSession is the session to be replaced by a new session
    if (!mIdle && mCurrentSession != nullptr) {
        if (mBusySession != mCurrentSession) {
            ALOGE("Current session != busy session");
            setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_DEVICE);
            return ACAMERA_ERROR_CAMERA_DEVICE;
        }
        sp<AMessage> msg = new AMessage(kWhatSessionStateCb, mHandler);
        msg->setPointer(kContextKey, mBusySession->mUserSessionCallback.context);
        msg->setObject(kSessionSpKey, mBusySession);
        msg->setPointer(kCallbackFpKey, (void*) mBusySession->mUserSessionCallback.onReady);
        mBusySession.clear();
        postSessionMsgAndCleanup(msg);
    }
    mIdle = true;

    binder::Status remoteRet = mRemote->beginConfigure();
    if (!remoteRet.isOk()) {
        ALOGE("Camera device %s begin configure failed: %s", getId(), remoteRet.toString8().c_str());
        return ACAMERA_ERROR_UNKNOWN;
    }

    // delete to-be-deleted streams
    for (auto streamId : deleteList) {
        remoteRet = mRemote->deleteStream(streamId);
        if (!remoteRet.isOk()) {
            ALOGE("Camera device %s failed to remove stream %d: %s", getId(), streamId,
                    remoteRet.toString8().c_str());
            return ACAMERA_ERROR_UNKNOWN;
        }
        mConfiguredOutputs.erase(streamId);
    }

    // add new streams
    for (const auto& outputPair : addSet) {
        int streamId;
        remoteRet = mRemote->createStream(outputPair.second, &streamId);
        if (!remoteRet.isOk()) {
            ALOGE("Camera device %s failed to create stream: %s", getId(),
                    remoteRet.toString8().c_str());
            return ACAMERA_ERROR_UNKNOWN;
        }
        mConfiguredOutputs.insert(std::make_pair(streamId, outputPair));
    }

    CameraMetadata params;
    if ((sessionParameters != nullptr) && (sessionParameters->settings != nullptr)) {
        params.append(sessionParameters->settings->getInternalData());
    }
    std::vector<int> offlineStreamIds;
    remoteRet = mRemote->endConfigure(/*isConstrainedHighSpeed*/ false, params,
            ns2ms(startTimeNs), &offlineStreamIds);
    if (remoteRet.serviceSpecificErrorCode() == hardware::ICameraService::ERROR_ILLEGAL_ARGUMENT) {
        ALOGE("Camera device %s cannnot support app output configuration: %s", getId(),
                remoteRet.toString8().c_str());
        return ACAMERA_ERROR_STREAM_CONFIGURE_FAIL;
    } else if (!remoteRet.isOk()) {
        ALOGE("Camera device %s end configure failed: %s", getId(), remoteRet.toString8().c_str());
        return ACAMERA_ERROR_UNKNOWN;
    }

    return ACAMERA_OK;
}

void
CameraDevice::setRemoteDevice(sp<hardware::camera2::ICameraDeviceUser> remote) {
    Mutex::Autolock _l(mDeviceLock);
    mRemote = remote;
}

bool CameraDevice::setDeviceMetadataQueues() {
    if (mRemote == nullptr) {
        ALOGE("mRemote must not be null while trying to fetch metadata queues");
        return false;
    }
    MQDescriptor<int8_t, SynchronizedReadWrite> resMqDescriptor;
    binder::Status ret = mRemote->getCaptureResultMetadataQueue(&resMqDescriptor);
    if (!ret.isOk()) {
        ALOGE("Transaction error trying to get capture result metadata queue");
        return false;
    }
    mCaptureResultMetadataQueue = std::make_unique<ResultMetadataQueue>(resMqDescriptor);
    if (!mCaptureResultMetadataQueue->isValid()) {
        ALOGE("Empty fmq from cameraserver");
        mCaptureResultMetadataQueue = nullptr;
        return false;
    }

    return true;
}

camera_status_t
CameraDevice::checkCameraClosedOrErrorLocked() const {
    if (mRemote == nullptr) {
        ALOGE("%s: camera device already closed", __FUNCTION__);
        return ACAMERA_ERROR_CAMERA_DISCONNECTED;
    }
    if (mInError) {// triggered by onDeviceError
        ALOGE("%s: camera device has encountered a serious error", __FUNCTION__);
        return mError;
    }
    return ACAMERA_OK;
}

void
CameraDevice::setCameraDeviceErrorLocked(camera_status_t error) {
    mInError = true;
    mError = error;
    return;
}

void
CameraDevice::FrameNumberTracker::updateTracker(int64_t frameNumber, bool isError) {
    ALOGV("updateTracker frame %" PRId64 " isError %d", frameNumber, isError);
    if (isError) {
        mFutureErrorSet.insert(frameNumber);
    } else if (frameNumber <= mCompletedFrameNumber) {
        ALOGE("Frame number %" PRId64 " decreased! current fn %" PRId64,
                frameNumber, mCompletedFrameNumber);
        return;
    } else {
        if (frameNumber != mCompletedFrameNumber + 1) {
            ALOGE("Frame number out of order. Expect %" PRId64 " but get %" PRId64,
                    mCompletedFrameNumber + 1, frameNumber);
            // Do not assert as in java implementation
        }
        mCompletedFrameNumber = frameNumber;
    }
    update();
}

void
CameraDevice::FrameNumberTracker::update() {
    for (auto it = mFutureErrorSet.begin(); it != mFutureErrorSet.end();) {
        int64_t errorFrameNumber = *it;
        if (errorFrameNumber == mCompletedFrameNumber + 1) {
            mCompletedFrameNumber++;
            it = mFutureErrorSet.erase(it);
        } else if (errorFrameNumber <= mCompletedFrameNumber) {
            // This should not happen, but deal with it anyway
            ALOGE("Completd frame number passed through current frame number!");
            // erase the old error since it's no longer useful
            it = mFutureErrorSet.erase(it);
        } else {
            // Normal requests hasn't catched up error frames, just break
            break;
        }
    }
    ALOGV("Update complete frame %" PRId64, mCompletedFrameNumber);
}

void
CameraDevice::onCaptureErrorLocked(
        int32_t errorCode,
        const CaptureResultExtras& resultExtras) {
    int sequenceId = resultExtras.requestId;
    int64_t frameNumber = resultExtras.frameNumber;
    int32_t burstId = resultExtras.burstId;
    auto it = mSequenceCallbackMap.find(sequenceId);
    if (it == mSequenceCallbackMap.end()) {
        ALOGE("%s: Error: capture sequence index %d not found!",
                __FUNCTION__, sequenceId);
        setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_SERVICE);
        return;
    }

    CallbackHolder cbh = (*it).second;
    sp<ACameraCaptureSession> session = cbh.mSession;
    if ((size_t) burstId >= cbh.mRequests.size()) {
        ALOGE("%s: Error: request index %d out of bound (size %zu)",
                __FUNCTION__, burstId, cbh.mRequests.size());
        setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_SERVICE);
        return;
    }
    sp<CaptureRequest> request = cbh.mRequests[burstId];

    // Handle buffer error
    if (errorCode == hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_BUFFER) {
        int32_t streamId = resultExtras.errorStreamId;
        ACameraCaptureSession_captureCallback_bufferLost onBufferLost =
                cbh.mOnCaptureBufferLost;
        auto outputPairIt = mConfiguredOutputs.find(streamId);
        if (outputPairIt == mConfiguredOutputs.end()) {
            ALOGE("%s: Error: stream id %d does not exist", __FUNCTION__, streamId);
            setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_SERVICE);
            return;
        }

        const auto& outSurfaces = outputPairIt->second.second.getSurfaces();
        for (const auto& outSurface : outSurfaces) {
            for (const auto& surface : request->mSurfaceList) {
                if ( outSurface == surface
#if not WB_LIBCAMERASERVICE_WITH_DEPENDENCIES
                                ->getIGraphicBufferProducer()
#endif
                                            ) {
                    ANativeWindow* anw = static_cast<ANativeWindow*>(surface.get());
                    ALOGV("Camera %s Lost output buffer for ANW %p frame %" PRId64,
                            getId(), anw, frameNumber);

                    sp<AMessage> msg = new AMessage(kWhatCaptureBufferLost, mHandler);
                    msg->setPointer(kContextKey, cbh.mContext);
                    msg->setObject(kSessionSpKey, session);
                    msg->setPointer(kCallbackFpKey, (void*) onBufferLost);
                    msg->setObject(kCaptureRequestKey, request);
                    msg->setPointer(kAnwKey, (void*) anw);
                    msg->setInt64(kFrameNumberKey, frameNumber);
                    postSessionMsgAndCleanup(msg);
                }
            }
        }
    } else { // Handle other capture failures
        // Fire capture failure callback if there is one registered
        ACameraCaptureSession_captureCallback_failed onError = cbh.mOnCaptureFailed;
        sp<CameraCaptureFailure> failure(new CameraCaptureFailure());
        failure->frameNumber = frameNumber;
        // TODO: refine this when implementing flush
        failure->reason      = CAPTURE_FAILURE_REASON_ERROR;
        failure->sequenceId  = sequenceId;
        failure->wasImageCaptured = (errorCode ==
                hardware::camera2::ICameraDeviceCallbacks::ERROR_CAMERA_RESULT);

        sp<AMessage> msg = new AMessage(cbh.mIsLogicalCameraCallback ? kWhatLogicalCaptureFail :
                kWhatCaptureFail, mHandler);
        msg->setPointer(kContextKey, cbh.mContext);
        msg->setObject(kSessionSpKey, session);
        if (cbh.mIsLogicalCameraCallback) {
            if (resultExtras.errorPhysicalCameraId.size() > 0) {
                String8 cameraId = toString8(resultExtras.errorPhysicalCameraId);
                msg->setString(kFailingPhysicalCameraId, cameraId.c_str(), cameraId.size());
            }
            msg->setPointer(kCallbackFpKey, (void*) cbh.mOnLogicalCameraCaptureFailed);
        } else {
            msg->setPointer(kCallbackFpKey, (void*) onError);
        }
        msg->setObject(kCaptureRequestKey, request);
        msg->setObject(kCaptureFailureKey, failure);
        postSessionMsgAndCleanup(msg);

        // Update tracker
        mFrameNumberTracker.updateTracker(frameNumber, /*isError*/true);
        checkAndFireSequenceCompleteLocked();
    }
    return;
}

void CameraDevice::stopLooperAndDisconnect() {
    Mutex::Autolock _l(mDeviceLock);
    sp<ACameraCaptureSession> session = mCurrentSession.promote();
    if (!isClosed()) {
        disconnectLocked(session);
    }
    mCurrentSession = nullptr;

    if (mCbLooper != nullptr) {
      mCbLooper->unregisterHandler(mHandler->id());
      mCbLooper->stop();
    }
    mCbLooper.clear();
    mHandler.clear();
}

CameraDevice::CallbackHandler::CallbackHandler(const char* id) : mId(id) {
}

void CameraDevice::CallbackHandler::onMessageReceived(
        const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatOnDisconnected:
        case kWhatOnError:
        case kWhatSessionStateCb:
        case kWhatCaptureStart:
        case kWhatCaptureStart2:
        case kWhatCaptureResult:
        case kWhatLogicalCaptureResult:
        case kWhatCaptureFail:
        case kWhatLogicalCaptureFail:
        case kWhatCaptureSeqEnd:
        case kWhatCaptureSeqAbort:
        case kWhatCaptureBufferLost:
        case kWhatPreparedCb:
        case kWhatClientSharedAccessPriorityChanged:
            ALOGV("%s: Received msg %d", __FUNCTION__, msg->what());
            break;
        case kWhatCleanUpSessions:
            mCachedSessions.clear();
            return;
        default:
            ALOGE("%s:Error: unknown device callback %d", __FUNCTION__, msg->what());
            return;
    }
    // Check the common part of all message
    void* context;
    bool found = msg->findPointer(kContextKey, &context);
    if (!found) {
        ALOGE("%s: Cannot find callback context!", __FUNCTION__);
        return;
    }
    switch (msg->what()) {
        case kWhatOnDisconnected:
        {
            ACameraDevice* dev;
            found = msg->findPointer(kDeviceKey, (void**) &dev);
            if (!found || dev == nullptr) {
                ALOGE("%s: Cannot find device pointer!", __FUNCTION__);
                return;
            }
            ACameraDevice_StateCallback onDisconnected;
            found = msg->findPointer(kCallbackFpKey, (void**) &onDisconnected);
            if (!found) {
                ALOGE("%s: Cannot find onDisconnected!", __FUNCTION__);
                return;
            }
            if (onDisconnected == nullptr) {
                return;
            }
            (*onDisconnected)(context, dev);
            break;
        }

        case kWhatClientSharedAccessPriorityChanged:
        {
            if (!flags::camera_multi_client()) {
                break;
            }
            ACameraDevice* dev;
            found = msg->findPointer(kDeviceKey, (void**) &dev);
            if (!found || dev == nullptr) {
                ALOGE("%s: Cannot find device pointer!", __FUNCTION__);
                return;
            }
            ACameraDevice_ClientSharedAccessPriorityChangedCallback
                    onClientSharedAccessPriorityChanged;
            found = msg->findPointer(kCallbackFpKey, (void**) &onClientSharedAccessPriorityChanged);
            if (!found) {
                ALOGE("%s: Cannot find onClientSharedAccessPriorityChanged!", __FUNCTION__);
                return;
            }
            if (onClientSharedAccessPriorityChanged == nullptr) {
                return;
            }
            (*onClientSharedAccessPriorityChanged)(context, dev, dev->isPrimaryClient());
            break;
        }

        case kWhatOnError:
        {
            ACameraDevice* dev;
            found = msg->findPointer(kDeviceKey, (void**) &dev);
            if (!found || dev == nullptr) {
                ALOGE("%s: Cannot find device pointer!", __FUNCTION__);
                return;
            }
            ACameraDevice_ErrorStateCallback onError;
            found = msg->findPointer(kCallbackFpKey, (void**) &onError);
            if (!found) {
                ALOGE("%s: Cannot find onError!", __FUNCTION__);
                return;
            }
            int errorCode;
            found = msg->findInt32(kErrorCodeKey, &errorCode);
            if (!found) {
                ALOGE("%s: Cannot find error code!", __FUNCTION__);
                return;
            }
            if (onError == nullptr) {
                return;
            }
            (*onError)(context, dev, errorCode);
            break;
        }
        case kWhatSessionStateCb:
        case kWhatCaptureStart:
        case kWhatCaptureStart2:
        case kWhatCaptureResult:
        case kWhatLogicalCaptureResult:
        case kWhatCaptureFail:
        case kWhatLogicalCaptureFail:
        case kWhatCaptureSeqEnd:
        case kWhatCaptureSeqAbort:
        case kWhatCaptureBufferLost:
        case kWhatPreparedCb:
        {
            sp<RefBase> obj;
            found = msg->findObject(kSessionSpKey, &obj);
            if (!found || obj == nullptr) {
                ALOGE("%s: Cannot find session pointer!", __FUNCTION__);
                return;
            }
            sp<ACameraCaptureSession> session(static_cast<ACameraCaptureSession*>(obj.get()));
            mCachedSessions.push(session);
            sp<CaptureRequest> requestSp = nullptr;
            switch (msg->what()) {
                case kWhatCaptureStart:
                case kWhatCaptureStart2:
                case kWhatCaptureResult:
                case kWhatLogicalCaptureResult:
                case kWhatCaptureFail:
                case kWhatLogicalCaptureFail:
                case kWhatCaptureBufferLost:
                    found = msg->findObject(kCaptureRequestKey, &obj);
                    if (!found) {
                        ALOGE("%s: Cannot find capture request!", __FUNCTION__);
                        return;
                    }
                    requestSp = static_cast<CaptureRequest*>(obj.get());
                    break;
            }

            switch (msg->what()) {
                case kWhatSessionStateCb:
                {
                    ACameraCaptureSession_stateCallback onState;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onState);
                    if (!found) {
                        ALOGE("%s: Cannot find state callback!", __FUNCTION__);
                        return;
                    }
                    if (onState == nullptr) {
                        return;
                    }
                    (*onState)(context, session.get());
                    break;
                }
                case kWhatPreparedCb:
                {
                    ACameraCaptureSession_prepareCallback onWindowPrepared;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onWindowPrepared);
                    if (!found) {
                        ALOGE("%s: Cannot find window prepared callback!", __FUNCTION__);
                        return;
                    }
                    if (onWindowPrepared == nullptr) {
                        return;
                    }
                    ANativeWindow* anw;
                    found = msg->findPointer(kAnwKey, (void**) &anw);
                    if (!found) {
                        ALOGE("%s: Cannot find ANativeWindow: %d!", __FUNCTION__, __LINE__);
                        return;
                    }
                    (*onWindowPrepared)(context, anw, session.get());
                    break;
                }
                case kWhatCaptureStart:
                {
                    ACameraCaptureSession_captureCallback_start onStart;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onStart);
                    if (!found) {
                        ALOGE("%s: Cannot find capture start callback!", __FUNCTION__);
                        return;
                    }
                    if (onStart == nullptr) {
                        return;
                    }
                    int64_t timestamp;
                    found = msg->findInt64(kTimeStampKey, &timestamp);
                    if (!found) {
                        ALOGE("%s: Cannot find timestamp!", __FUNCTION__);
                        return;
                    }
                    ACaptureRequest* request = allocateACaptureRequest(requestSp, mId);
                    (*onStart)(context, session.get(), request, timestamp);
                    freeACaptureRequest(request);
                    break;
                }
                case kWhatCaptureStart2:
                {
                    ACameraCaptureSession_captureCallback_startV2 onStart2;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onStart2);
                    if (!found) {
                        ALOGE("%s: Cannot find capture startV2 callback!", __FUNCTION__);
                        return;
                    }
                    if (onStart2 == nullptr) {
                        return;
                    }
                    int64_t timestamp;
                    found = msg->findInt64(kTimeStampKey, &timestamp);
                    if (!found) {
                        ALOGE("%s: Cannot find timestamp!", __FUNCTION__);
                        return;
                    }
                    int64_t frameNumber;
                    found = msg->findInt64(kFrameNumberKey, &frameNumber);
                    if (!found) {
                        ALOGE("%s: Cannot find frame number!", __FUNCTION__);
                        return;
                    }

                    ACaptureRequest* request = allocateACaptureRequest(requestSp, mId);
                    (*onStart2)(context, session.get(), request, timestamp, frameNumber);
                    freeACaptureRequest(request);
                    break;
                }
                case kWhatCaptureResult:
                {
                    ACameraCaptureSession_captureCallback_result onResult;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onResult);
                    if (!found) {
                        ALOGE("%s: Cannot find capture result callback!", __FUNCTION__);
                        return;
                    }
                    if (onResult == nullptr) {
                        return;
                    }

                    found = msg->findObject(kCaptureResultKey, &obj);
                    if (!found) {
                        ALOGE("%s: Cannot find capture result!", __FUNCTION__);
                        return;
                    }
                    sp<ACameraMetadata> result(static_cast<ACameraMetadata*>(obj.get()));
                    ACaptureRequest* request = allocateACaptureRequest(requestSp, mId);
                    (*onResult)(context, session.get(), request, result.get());
                    freeACaptureRequest(request);
                    break;
                }
                case kWhatLogicalCaptureResult:
                {
                    ACameraCaptureSession_logicalCamera_captureCallback_result onResult;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onResult);
                    if (!found) {
                        ALOGE("%s: Cannot find logicalCamera capture result callback!",
                                __FUNCTION__);
                        return;
                    }
                    if (onResult == nullptr) {
                        return;
                    }

                    found = msg->findObject(kCaptureResultKey, &obj);
                    if (!found) {
                        ALOGE("%s: Cannot find capture result!", __FUNCTION__);
                        return;
                    }
                    sp<ACameraMetadata> result(static_cast<ACameraMetadata*>(obj.get()));

                    found = msg->findObject(kPhysicalCaptureResultKey, &obj);
                    if (!found) {
                        ALOGE("%s: Cannot find physical capture result!", __FUNCTION__);
                        return;
                    }
                    sp<ACameraPhysicalCaptureResultInfo> physicalResult(
                            static_cast<ACameraPhysicalCaptureResultInfo*>(obj.get()));
                    std::vector<PhysicalCaptureResultInfo>& physicalResultInfo =
                            physicalResult->mPhysicalResultInfo;

                    std::vector<std::string> physicalCameraIds;
                    std::vector<sp<ACameraMetadata>> physicalMetadataCopy;
                    for (size_t i = 0; i < physicalResultInfo.size(); i++) {
                        String8 physicalId8 = toString8(physicalResultInfo[i].mPhysicalCameraId);
                        physicalCameraIds.push_back(physicalId8.c_str());

                        CameraMetadata clone =
                                physicalResultInfo[i].
                                        mCameraMetadataInfo.get<CameraMetadataInfo::metadata>();
                        clone.update(ANDROID_SYNC_FRAME_NUMBER,
                                &physicalResult->mFrameNumber, /*data_count*/1);
                        sp<ACameraMetadata> metadata =
                                new ACameraMetadata(clone.release(), ACameraMetadata::ACM_RESULT);
                        physicalMetadataCopy.push_back(metadata);
                    }

                    std::vector<const char*> physicalCameraIdPtrs;
                    std::vector<const ACameraMetadata*> physicalMetadataCopyPtrs;
                    for (size_t i = 0; i < physicalResultInfo.size(); i++) {
                        physicalCameraIdPtrs.push_back(physicalCameraIds[i].c_str());
                        physicalMetadataCopyPtrs.push_back(physicalMetadataCopy[i].get());
                    }

                    ACaptureRequest* request = allocateACaptureRequest(requestSp, mId);
                    (*onResult)(context, session.get(), request, result.get(),
                            physicalResultInfo.size(), physicalCameraIdPtrs.data(),
                            physicalMetadataCopyPtrs.data());
                    freeACaptureRequest(request);
                    break;
                }
                case kWhatCaptureFail:
                {
                    ACameraCaptureSession_captureCallback_failed onFail;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onFail);
                    if (!found) {
                        ALOGE("%s: Cannot find capture fail callback!", __FUNCTION__);
                        return;
                    }
                    if (onFail == nullptr) {
                        return;
                    }

                    found = msg->findObject(kCaptureFailureKey, &obj);
                    if (!found) {
                        ALOGE("%s: Cannot find capture failure!", __FUNCTION__);
                        return;
                    }
                    sp<CameraCaptureFailure> failureSp(
                            static_cast<CameraCaptureFailure*>(obj.get()));
                    ACameraCaptureFailure* failure =
                            static_cast<ACameraCaptureFailure*>(failureSp.get());
                    ACaptureRequest* request = allocateACaptureRequest(requestSp, mId);
                    (*onFail)(context, session.get(), request, failure);
                    freeACaptureRequest(request);
                    break;
                }
                case kWhatLogicalCaptureFail:
                {
                    ACameraCaptureSession_logicalCamera_captureCallback_failed onFail;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onFail);
                    if (!found) {
                        ALOGE("%s: Cannot find capture fail callback!", __FUNCTION__);
                        return;
                    }
                    if (onFail == nullptr) {
                        return;
                    }

                    found = msg->findObject(kCaptureFailureKey, &obj);
                    if (!found) {
                        ALOGE("%s: Cannot find capture failure!", __FUNCTION__);
                        return;
                    }
                    sp<CameraCaptureFailure> failureSp(
                            static_cast<CameraCaptureFailure*>(obj.get()));
                    ALogicalCameraCaptureFailure failure;
                    AString physicalCameraId;
                    found = msg->findString(kFailingPhysicalCameraId, &physicalCameraId);
                    if (found && !physicalCameraId.empty()) {
                        failure.physicalCameraId = physicalCameraId.c_str();
                    } else {
                        failure.physicalCameraId = nullptr;
                    }
                    failure.captureFailure = *failureSp;
                    ACaptureRequest* request = allocateACaptureRequest(requestSp, mId);
                    (*onFail)(context, session.get(), request, &failure);
                    freeACaptureRequest(request);
                    break;
                }
                case kWhatCaptureSeqEnd:
                {
                    ACameraCaptureSession_captureCallback_sequenceEnd onSeqEnd;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onSeqEnd);
                    if (!found) {
                        ALOGE("%s: Cannot find sequence end callback!", __FUNCTION__);
                        return;
                    }
                    if (onSeqEnd == nullptr) {
                        return;
                    }
                    int seqId;
                    found = msg->findInt32(kSequenceIdKey, &seqId);
                    if (!found) {
                        ALOGE("%s: Cannot find frame number!", __FUNCTION__);
                        return;
                    }
                    int64_t frameNumber;
                    found = msg->findInt64(kFrameNumberKey, &frameNumber);
                    if (!found) {
                        ALOGE("%s: Cannot find frame number!", __FUNCTION__);
                        return;
                    }
                    (*onSeqEnd)(context, session.get(), seqId, frameNumber);
                    break;
                }
                case kWhatCaptureSeqAbort:
                {
                    ACameraCaptureSession_captureCallback_sequenceAbort onSeqAbort;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onSeqAbort);
                    if (!found) {
                        ALOGE("%s: Cannot find sequence end callback!", __FUNCTION__);
                        return;
                    }
                    if (onSeqAbort == nullptr) {
                        return;
                    }
                    int seqId;
                    found = msg->findInt32(kSequenceIdKey, &seqId);
                    if (!found) {
                        ALOGE("%s: Cannot find frame number!", __FUNCTION__);
                        return;
                    }
                    (*onSeqAbort)(context, session.get(), seqId);
                    break;
                }
                case kWhatCaptureBufferLost:
                {
                    ACameraCaptureSession_captureCallback_bufferLost onBufferLost;
                    found = msg->findPointer(kCallbackFpKey, (void**) &onBufferLost);
                    if (!found) {
                        ALOGE("%s: Cannot find buffer lost callback!", __FUNCTION__);
                        return;
                    }
                    if (onBufferLost == nullptr) {
                        return;
                    }

                    ANativeWindow* anw;
                    found = msg->findPointer(kAnwKey, (void**) &anw);
                    if (!found) {
                        ALOGE("%s: Cannot find ANativeWindow!", __FUNCTION__);
                        return;
                    }

                    int64_t frameNumber;
                    found = msg->findInt64(kFrameNumberKey, &frameNumber);
                    if (!found) {
                        ALOGE("%s: Cannot find frame number!", __FUNCTION__);
                        return;
                    }

                    ACaptureRequest* request = allocateACaptureRequest(requestSp, mId);
                    (*onBufferLost)(context, session.get(), request, anw, frameNumber);
                    freeACaptureRequest(request);
                    break;
                }
            }
            break;
        }
    }
}

CameraDevice::CallbackHolder::CallbackHolder(
        sp<ACameraCaptureSession>          session,
        const Vector<sp<CaptureRequest> >& requests,
        bool                               isRepeating,
        ACameraCaptureSession_captureCallbacks* cbs) :
        mSession(session), mRequests(requests),
        mIsRepeating(isRepeating),
        mIsLogicalCameraCallback(false),
        mIs2Callback(false) {
    initCaptureCallbacks(cbs);

    if (cbs != nullptr) {
        mOnCaptureCompleted = cbs->onCaptureCompleted;
        mOnCaptureFailed = cbs->onCaptureFailed;
    }
}

CameraDevice::CallbackHolder::CallbackHolder(
        sp<ACameraCaptureSession>          session,
        const Vector<sp<CaptureRequest> >& requests,
        bool                               isRepeating,
        ACameraCaptureSession_logicalCamera_captureCallbacks* lcbs) :
        mSession(session), mRequests(requests),
        mIsRepeating(isRepeating),
        mIsLogicalCameraCallback(true),
        mIs2Callback(false) {
    initCaptureCallbacks(lcbs);

    if (lcbs != nullptr) {
        mOnLogicalCameraCaptureCompleted = lcbs->onLogicalCameraCaptureCompleted;
        mOnLogicalCameraCaptureFailed = lcbs->onLogicalCameraCaptureFailed;
    }
}

CameraDevice::CallbackHolder::CallbackHolder(
        sp<ACameraCaptureSession>          session,
        const Vector<sp<CaptureRequest> >& requests,
        bool                               isRepeating,
        ACameraCaptureSession_captureCallbacksV2* cbs) :
        mSession(session), mRequests(requests),
        mIsRepeating(isRepeating),
        mIsLogicalCameraCallback(false),
        mIs2Callback(true) {
    initCaptureCallbacksV2(cbs);

    if (cbs != nullptr) {
        mOnCaptureCompleted = cbs->onCaptureCompleted;
        mOnCaptureFailed = cbs->onCaptureFailed;
    }
}

CameraDevice::CallbackHolder::CallbackHolder(
        sp<ACameraCaptureSession>          session,
        const Vector<sp<CaptureRequest> >& requests,
        bool                               isRepeating,
        ACameraCaptureSession_logicalCamera_captureCallbacksV2* lcbs) :
        mSession(session), mRequests(requests),
        mIsRepeating(isRepeating),
        mIsLogicalCameraCallback(true),
        mIs2Callback(true) {
    initCaptureCallbacksV2(lcbs);

    if (lcbs != nullptr) {
        mOnLogicalCameraCaptureCompleted = lcbs->onLogicalCameraCaptureCompleted;
        mOnLogicalCameraCaptureFailed = lcbs->onLogicalCameraCaptureFailed;
    }
}

void
CameraDevice::checkRepeatingSequenceCompleteLocked(
    const int sequenceId, const int64_t lastFrameNumber) {
    ALOGV("Repeating seqId %d lastFrameNumer %" PRId64, sequenceId, lastFrameNumber);
    if (lastFrameNumber == NO_FRAMES_CAPTURED) {
        if (mSequenceCallbackMap.count(sequenceId) == 0) {
            ALOGW("No callback found for sequenceId %d", sequenceId);
            return;
        }
        // remove callback holder from callback map
        auto cbIt = mSequenceCallbackMap.find(sequenceId);
        CallbackHolder cbh = cbIt->second;
        mSequenceCallbackMap.erase(cbIt);
        // send seq aborted callback
        sp<AMessage> msg = new AMessage(kWhatCaptureSeqAbort, mHandler);
        msg->setPointer(kContextKey, cbh.mContext);
        msg->setObject(kSessionSpKey, cbh.mSession);
        msg->setPointer(kCallbackFpKey, (void*) cbh.mOnCaptureSequenceAborted);
        msg->setInt32(kSequenceIdKey, sequenceId);
        postSessionMsgAndCleanup(msg);
    } else {
        // Use mSequenceLastFrameNumberMap to track
        mSequenceLastFrameNumberMap.insert(std::make_pair(sequenceId, lastFrameNumber));

        // Last frame might have arrived. Check now
        checkAndFireSequenceCompleteLocked();
    }
}

void
CameraDevice::checkAndFireSequenceCompleteLocked() {
    int64_t completedFrameNumber = mFrameNumberTracker.getCompletedFrameNumber();
    auto it = mSequenceLastFrameNumberMap.begin();
    while (it != mSequenceLastFrameNumberMap.end()) {
        int sequenceId = it->first;
        int64_t lastFrameNumber = it->second.lastFrameNumber;

        if (mRemote == nullptr) {
            ALOGW("Camera %s closed while checking sequence complete", getId());
            return;
        }
        ALOGV("%s: seq %d's last frame number %" PRId64 ", completed %" PRId64,
                __FUNCTION__, sequenceId, lastFrameNumber, completedFrameNumber);
        if (!it->second.isSequenceCompleted) {
            // Check if there is callback for this sequence
            // This should not happen because we always register callback (with nullptr inside)
            if (mSequenceCallbackMap.count(sequenceId) == 0) {
                ALOGW("No callback found for sequenceId %d", sequenceId);
            }

            if (lastFrameNumber <= completedFrameNumber) {
                ALOGV("Mark sequenceId %d as sequence completed", sequenceId);
                it->second.isSequenceCompleted = true;
            }

        }

        if (it->second.isSequenceCompleted && it->second.isInflightCompleted) {
            sendCaptureSequenceCompletedLocked(sequenceId, lastFrameNumber);

            it = mSequenceLastFrameNumberMap.erase(it);
            ALOGV("%s: Remove holder for sequenceId %d", __FUNCTION__, sequenceId);
        } else {
            ++it;
        }
    }
}

void
CameraDevice::removeCompletedCallbackHolderLocked(int64_t lastCompletedRegularFrameNumber) {
    auto it = mSequenceLastFrameNumberMap.begin();
    while (it != mSequenceLastFrameNumberMap.end()) {
        int sequenceId = it->first;
        int64_t lastFrameNumber = it->second.lastFrameNumber;

        if (mRemote == nullptr) {
            ALOGW("Camera %s closed while checking sequence complete", getId());
            return;
        }

        ALOGV("%s: seq %d's last frame number %" PRId64
                ", completed inflight frame number %" PRId64,
                __FUNCTION__, sequenceId, lastFrameNumber,
                lastCompletedRegularFrameNumber);
        if (lastFrameNumber <= lastCompletedRegularFrameNumber) {
            if (it->second.isSequenceCompleted) {
                sendCaptureSequenceCompletedLocked(sequenceId, lastFrameNumber);

                it = mSequenceLastFrameNumberMap.erase(it);
                ALOGV("%s: Remove holder for sequenceId %d", __FUNCTION__, sequenceId);
            } else {
                ALOGV("Mark sequenceId %d as inflight completed", sequenceId);
                it->second.isInflightCompleted = true;
                ++it;
            }
        } else {
            ++it;
        }
    }
}

/**
  * Camera service callback implementation
  */
binder::Status
CameraDevice::ServiceCallback::onDeviceError(
        int32_t errorCode,
        const CaptureResultExtras& resultExtras) {
    ALOGD("Device error received, code %d, frame number %" PRId64 ", request ID %d, subseq ID %d",
            errorCode, resultExtras.frameNumber, resultExtras.requestId, resultExtras.burstId);
    binder::Status ret = binder::Status::ok();
    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return ret; // device has been closed
    }

    sp<ACameraCaptureSession> session = dev->mCurrentSession.promote();
    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->mRemote == nullptr) {
        return ret; // device has been closed
    }
    switch (errorCode) {
        case ERROR_CAMERA_DISCONNECTED:
        {
            // Camera is disconnected, close the session and expect no more callbacks
            if (session != nullptr) {
                session->closeByDevice();
            }
            dev->mCurrentSession = nullptr;
            sp<AMessage> msg = new AMessage(kWhatOnDisconnected, dev->mHandler);
            msg->setPointer(kContextKey, dev->mAppCallbacks.context);
            msg->setPointer(kDeviceKey, (void*) dev->getWrapper());
            msg->setPointer(kCallbackFpKey, (void*) dev->mAppCallbacks.onDisconnected);
            msg->post();
            break;
        }
        default:
            ALOGE("Unknown error from camera device: %d", errorCode);
            [[fallthrough]];
        case ERROR_CAMERA_DEVICE:
        case ERROR_CAMERA_SERVICE:
        {
            int32_t errorVal = ::ERROR_CAMERA_DEVICE;
            // We keep this switch since this block might be encountered with
            // more than just 2 states. The default fallthrough could have us
            // handling more unmatched error cases.
            switch (errorCode) {
                case ERROR_CAMERA_DEVICE:
                    dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_DEVICE);
                    break;
                case ERROR_CAMERA_SERVICE:
                    dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_SERVICE);
                    errorVal = ::ERROR_CAMERA_SERVICE;
                    break;
                default:
                    dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_UNKNOWN);
                    break;
            }
            sp<AMessage> msg = new AMessage(kWhatOnError, dev->mHandler);
            msg->setPointer(kContextKey, dev->mAppCallbacks.context);
            msg->setPointer(kDeviceKey, (void*) dev->getWrapper());
            msg->setPointer(kCallbackFpKey, (void*) dev->mAppCallbacks.onError);
            msg->setInt32(kErrorCodeKey, errorVal);
            msg->post();
            break;
        }
        case ERROR_CAMERA_REQUEST:
        case ERROR_CAMERA_RESULT:
        case ERROR_CAMERA_BUFFER:
            dev->onCaptureErrorLocked(errorCode, resultExtras);
            break;
    }
    return ret;
}

binder::Status
CameraDevice::ServiceCallback::onClientSharedAccessPriorityChanged(bool primaryClient) {
    ALOGV("onClientSharedAccessPriorityChanged received. primaryClient = %d", primaryClient);
    binder::Status ret = binder::Status::ok();
    if (!flags::camera_multi_client()) {
        return ret;
    }
    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return ret; // device has been closed
    }
    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->isClosed() || dev->mRemote == nullptr) {
        return ret;
    }
    dev->setPrimaryClient(primaryClient);
    sp<AMessage> msg = new AMessage(kWhatClientSharedAccessPriorityChanged, dev->mHandler);
    msg->setPointer(kContextKey, dev->mAppCallbacks.context);
    msg->setPointer(kDeviceKey, (void*) dev->getWrapper());
    msg->setPointer(kCallbackFpKey, (void*) dev->mAppCallbacks.onClientSharedAccessPriorityChanged);
    msg->post();

    return binder::Status::ok();
}

binder::Status
CameraDevice::ServiceCallback::onDeviceIdle() {
    ALOGV("Camera is now idle");
    binder::Status ret = binder::Status::ok();
    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return ret; // device has been closed
    }

    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->isClosed() || dev->mRemote == nullptr) {
        return ret;
    }

    dev->removeCompletedCallbackHolderLocked(
             std::numeric_limits<int64_t>::max()/*lastCompletedRegularFrameNumber*/);

    if (dev->mIdle) {
        // Already in idle state. Possibly other thread did waitUntilIdle
        return ret;
    }

    if (dev->mCurrentSession != nullptr) {
        ALOGE("onDeviceIdle sending state cb");
        if (dev->mBusySession != dev->mCurrentSession) {
            ALOGE("Current session != busy session");
            dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_DEVICE);
            return ret;
        }

        sp<AMessage> msg = new AMessage(kWhatSessionStateCb, dev->mHandler);
        msg->setPointer(kContextKey, dev->mBusySession->mUserSessionCallback.context);
        msg->setObject(kSessionSpKey, dev->mBusySession);
        msg->setPointer(kCallbackFpKey, (void*) dev->mBusySession->mUserSessionCallback.onReady);
        // Make sure we clear the sp first so the session destructor can
        // only happen on handler thread (where we don't hold device/session lock)
        dev->mBusySession.clear();
        dev->postSessionMsgAndCleanup(msg);
    }
    dev->mIdle = true;
    dev->mFlushing = false;
    return ret;
}

binder::Status
CameraDevice::ServiceCallback::onCaptureStarted(
        const CaptureResultExtras& resultExtras,
        int64_t timestamp) {
    binder::Status ret = binder::Status::ok();
    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return ret; // device has been closed
    }
    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->isClosed() || dev->mRemote == nullptr) {
        return ret;
    }

    dev->removeCompletedCallbackHolderLocked(
            resultExtras.lastCompletedRegularFrameNumber);

    int sequenceId = resultExtras.requestId;
    int32_t burstId = resultExtras.burstId;
    int64_t frameNumber = resultExtras.frameNumber;

    auto it = dev->mSequenceCallbackMap.find(sequenceId);
    if (it != dev->mSequenceCallbackMap.end()) {
        CallbackHolder cbh = (*it).second;
        bool v2Callback = cbh.mIs2Callback;
        ACameraCaptureSession_captureCallback_start onStart = cbh.mOnCaptureStarted;
        ACameraCaptureSession_captureCallback_startV2 onStart2 = cbh.mOnCaptureStarted2;
        sp<ACameraCaptureSession> session = cbh.mSession;
        if ((size_t) burstId >= cbh.mRequests.size()) {
            ALOGE("%s: Error: request index %d out of bound (size %zu)",
                    __FUNCTION__, burstId, cbh.mRequests.size());
            dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_SERVICE);
        }
        sp<CaptureRequest> request = cbh.mRequests[burstId];
        sp<AMessage> msg = nullptr;
        if (v2Callback) {
            msg = new AMessage(kWhatCaptureStart2, dev->mHandler);
            msg->setPointer(kCallbackFpKey, (void*) onStart2);
        } else {
            msg = new AMessage(kWhatCaptureStart, dev->mHandler);
            msg->setPointer(kCallbackFpKey, (void *)onStart);
        }
        msg->setPointer(kContextKey, cbh.mContext);
        msg->setObject(kSessionSpKey, session);
        msg->setObject(kCaptureRequestKey, request);
        msg->setInt64(kTimeStampKey, timestamp);
        msg->setInt64(kFrameNumberKey, frameNumber);
        dev->postSessionMsgAndCleanup(msg);
    }
    return ret;
}

binder::Status
CameraDevice::ServiceCallback::onResultReceived(
        const CameraMetadataInfo &resultMetadata,
        const CaptureResultExtras& resultExtras,
        const std::vector<PhysicalCaptureResultInfo>& physicalResultInfos) {
    binder::Status ret = binder::Status::ok();

    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return ret; // device has been closed
    }

    int sequenceId = resultExtras.requestId;
    int64_t frameNumber = resultExtras.frameNumber;
    int32_t burstId = resultExtras.burstId;
    bool    isPartialResult = (resultExtras.partialResultCount < dev->mPartialResultCount);
    if (!isPartialResult) {
        ALOGV("SeqId %d frame %" PRId64 " result arrive.", sequenceId, frameNumber);
    }

    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->mRemote == nullptr) {
        return ret; // device has been disconnected
    }

    if (dev->isClosed()) {
        if (!isPartialResult) {
            dev->mFrameNumberTracker.updateTracker(frameNumber, /*isError*/false);
        }
        // early return to avoid callback sent to closed devices
        return ret;
    }

    CameraMetadata metadataCopy;
    camera_status_t status = readOneResultMetadata(resultMetadata,
            dev->mCaptureResultMetadataQueue.get(), &metadataCopy);
    if (status != ACAMERA_OK) {
        ALOGE("%s: result metadata couldn't be converted", __FUNCTION__);
        return ret;
    }
    metadataCopy.update(ANDROID_LENS_INFO_SHADING_MAP_SIZE, dev->mShadingMapSize, /*data_count*/2);
    metadataCopy.update(ANDROID_SYNC_FRAME_NUMBER, &frameNumber, /*data_count*/1);

    auto it = dev->mSequenceCallbackMap.find(sequenceId);
    if (it != dev->mSequenceCallbackMap.end()) {
        CallbackHolder cbh = (*it).second;
        sp<ACameraCaptureSession> session = cbh.mSession;
        if ((size_t) burstId >= cbh.mRequests.size()) {
            ALOGE("%s: Error: request index %d out of bound (size %zu)",
                    __FUNCTION__, burstId, cbh.mRequests.size());
            dev->setCameraDeviceErrorLocked(ACAMERA_ERROR_CAMERA_SERVICE);
        }
        sp<CaptureRequest> request = cbh.mRequests[burstId];
        sp<ACameraMetadata> result(new ACameraMetadata(
                metadataCopy.release(), ACameraMetadata::ACM_RESULT));

        std::vector<PhysicalCaptureResultInfo> localPhysicalResult;
        localPhysicalResult.resize(physicalResultInfos.size());
        for (size_t i = 0; i < physicalResultInfos.size(); i++) {
            CameraMetadata physicalMetadata;
            localPhysicalResult[i].mPhysicalCameraId = physicalResultInfos[i].mPhysicalCameraId;
            status = readOneResultMetadata(physicalResultInfos[i].mCameraMetadataInfo,
                    dev->mCaptureResultMetadataQueue.get(),
                    &physicalMetadata);
            if (status != ACAMERA_OK) {
                ALOGE("%s: physical camera result metadata couldn't be converted", __FUNCTION__);
                return ret;
            }
            localPhysicalResult[i].mCameraMetadataInfo.set<CameraMetadataInfo::metadata>(
                    std::move(physicalMetadata));
        }
        sp<ACameraPhysicalCaptureResultInfo> physicalResult(
                new ACameraPhysicalCaptureResultInfo(localPhysicalResult, frameNumber));

        sp<AMessage> msg = new AMessage(
                cbh.mIsLogicalCameraCallback ? kWhatLogicalCaptureResult : kWhatCaptureResult,
                dev->mHandler);
        msg->setPointer(kContextKey, cbh.mContext);
        msg->setObject(kSessionSpKey, session);
        msg->setObject(kCaptureRequestKey, request);
        msg->setObject(kCaptureResultKey, result);
        if (isPartialResult) {
            msg->setPointer(kCallbackFpKey,
                    (void *)cbh.mOnCaptureProgressed);
        } else if (cbh.mIsLogicalCameraCallback) {
            msg->setPointer(kCallbackFpKey,
                    (void *)cbh.mOnLogicalCameraCaptureCompleted);
            msg->setObject(kPhysicalCaptureResultKey, physicalResult);
        } else {
            msg->setPointer(kCallbackFpKey,
                    (void *)cbh.mOnCaptureCompleted);
        }
        dev->postSessionMsgAndCleanup(msg);
    }

    if (!isPartialResult) {
        dev->mFrameNumberTracker.updateTracker(frameNumber, /*isError*/false);
        dev->checkAndFireSequenceCompleteLocked();
    }

    return ret;
}

binder::Status
CameraDevice::ServiceCallback::onPrepared(int streamId) {
    ALOGV("%s: callback for stream id %d", __FUNCTION__, streamId);
    binder::Status ret = binder::Status::ok();
    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return ret; // device has been closed
    }
    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->isClosed() || dev->mRemote == nullptr) {
        return ret;
    }
    auto it = dev->mConfiguredOutputs.find(streamId);
    if (it == dev->mConfiguredOutputs.end()) {
        ALOGE("%s: stream id %d does not exist", __FUNCTION__ , streamId);
        return ret;
    }
    sp<ACameraCaptureSession> session = dev->mCurrentSession.promote();
    if (session == nullptr) {
        ALOGE("%s: Session is dead already", __FUNCTION__ );
        return ret;
    }
    // We've found the window corresponding to the surface id.
    ANativeWindow *window = it->second.first;
    sp<AMessage> msg = new AMessage(kWhatPreparedCb, dev->mHandler);
    msg->setPointer(kContextKey, session->mPreparedCb.context);
    msg->setPointer(kAnwKey, window);
    msg->setObject(kSessionSpKey, session);
    msg->setPointer(kCallbackFpKey, (void *)session->mPreparedCb.onWindowPrepared);
    dev->postSessionMsgAndCleanup(msg);

    return binder::Status::ok();
}

binder::Status
CameraDevice::ServiceCallback::onRequestQueueEmpty() {
    // onRequestQueueEmpty not yet implemented in NDK
    return binder::Status::ok();
}

binder::Status
CameraDevice::ServiceCallback::onRepeatingRequestError(
        int64_t lastFrameNumber, int32_t stoppedSequenceId) {
    binder::Status ret = binder::Status::ok();

    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return ret; // device has been closed
    }

    Mutex::Autolock _l(dev->mDeviceLock);

    int repeatingSequenceId = dev->mRepeatingSequenceId;
    if (stoppedSequenceId == repeatingSequenceId) {
        dev->mRepeatingSequenceId = REQUEST_ID_NONE;
    }

    dev->checkRepeatingSequenceCompleteLocked(repeatingSequenceId, lastFrameNumber);

    return ret;
}

void
CameraDevice::sendCaptureSequenceCompletedLocked(int sequenceId, int64_t lastFrameNumber) {
    auto cbIt = mSequenceCallbackMap.find(sequenceId);
    if (cbIt != mSequenceCallbackMap.end()) {
        CallbackHolder cbh = cbIt->second;
        mSequenceCallbackMap.erase(cbIt);

        // send seq complete callback
        sp<AMessage> msg = new AMessage(kWhatCaptureSeqEnd, mHandler);
        msg->setPointer(kContextKey, cbh.mContext);
        msg->setObject(kSessionSpKey, cbh.mSession);
        msg->setPointer(kCallbackFpKey, (void*) cbh.mOnCaptureSequenceCompleted);
        msg->setInt32(kSequenceIdKey, sequenceId);
        msg->setInt64(kFrameNumberKey, lastFrameNumber);

        // Clear the session sp before we send out the message
        // This will guarantee the rare case where the message is processed
        // before cbh goes out of scope and causing we call the session
        // destructor while holding device lock
        cbh.mSession.clear();
        postSessionMsgAndCleanup(msg);
    } else {
        // Check if there is callback for this sequence
        // This should not happen because we always register callback (with nullptr inside)
        ALOGW("No callback found for sequenceId %d", sequenceId);
    }
}

camera_status_t CameraDevice::ServiceCallback::readOneResultMetadata(
        const CameraMetadataInfo& resultInfo, ResultMetadataQueue* metadataQueue,
        CameraMetadata* metadata) {
    if (metadataQueue == nullptr || metadata == nullptr) {
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    if (resultInfo.getTag() == CameraMetadataInfo::fmqSize) {
        int64_t metadataSize = resultInfo.get<CameraMetadataInfo::fmqSize>();
        auto metadataVec = std::make_unique<int8_t []>(metadataSize);
        bool read = metadataQueue->read(reinterpret_cast<int8_t*>(metadataVec.get()), metadataSize);
        if (!read) {
            ALOGE("%s capture request settings could't be read from fmq", __FUNCTION__);
            return ACAMERA_ERROR_UNKNOWN;
        }
        *metadata = CameraMetadata(reinterpret_cast<camera_metadata_t *>(metadataVec.release()));
    } else {
        *metadata =
                resultInfo.get<CameraMetadataInfo::metadata>();
    }

    return ACAMERA_OK;
}

} // namespace acam
} // namespace android
