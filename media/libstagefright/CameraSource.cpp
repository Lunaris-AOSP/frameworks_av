/*
 * Copyright (C) 2009 The Android Open Source Project
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

#include <inttypes.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "CameraSource"
#include <utils/Log.h>

#include <OMX_Component.h>
#include <binder/IPCThreadState.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <media/hardware/HardwareAPI.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/CameraSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <camera/Camera.h>
#include <camera/CameraParameters.h>
#include <camera/StringUtils.h>
#include <com_android_graphics_libgui_flags.h>
#include <gui/Surface.h>
#include <gui/Flags.h>
#include <utils/String8.h>
#include <cutils/properties.h>

#if LOG_NDEBUG
#define UNUSED_UNLESS_VERBOSE(x) (void)(x)
#else
#define UNUSED_UNLESS_VERBOSE(x)
#endif

namespace android {

static const int64_t CAMERA_SOURCE_TIMEOUT_NS = 3000000000LL;

static int32_t getColorFormat(const char* colorFormat) {
    if (!colorFormat) {
        ALOGE("Invalid color format");
        return -1;
    }

    if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_YUV420P)) {
       return OMX_COLOR_FormatYUV420Planar;
    }

    if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_YUV422SP)) {
       return OMX_COLOR_FormatYUV422SemiPlanar;
    }

    if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_YUV420SP)) {
        return OMX_COLOR_FormatYUV420SemiPlanar;
    }

    if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_YUV422I)) {
        return OMX_COLOR_FormatYCbYCr;
    }

    if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_RGB565)) {
       return OMX_COLOR_Format16bitRGB565;
    }

    if (!strcmp(colorFormat, "OMX_TI_COLOR_FormatYUV420PackedSemiPlanar")) {
       return OMX_TI_COLOR_FormatYUV420PackedSemiPlanar;
    }

    if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_ANDROID_OPAQUE)) {
        return OMX_COLOR_FormatAndroidOpaque;
    }

    if (!strcmp(colorFormat, "YVU420SemiPlanar")) {
        return OMX_QCOM_COLOR_FormatYVU420SemiPlanar;
    }

    ALOGE("Uknown color format (%s), please add it to "
         "CameraSource::getColorFormat", colorFormat);

    CHECK(!"Unknown color format");
    return -1;
}

// static
CameraSource *CameraSource::CreateFromCamera(
    const sp<hardware::ICamera>& camera,
    const sp<ICameraRecordingProxy>& proxy,
    int32_t cameraId,
    const String16& clientName,
    uid_t clientUid,
    pid_t clientPid,
    Size videoSize,
    int32_t frameRate,
    const sp<SurfaceType>& surface) {

    CameraSource *source = new CameraSource(camera, proxy, cameraId,
            clientName, clientUid, clientPid, videoSize, frameRate, surface);
    return source;
}

CameraSource::CameraSource(
    const sp<hardware::ICamera>& camera,
    const sp<ICameraRecordingProxy>& proxy,
    int32_t cameraId,
    const String16& clientName,
    uid_t clientUid,
    pid_t clientPid,
    Size videoSize,
    int32_t frameRate,
    const sp<SurfaceType>& surface)
    : mCameraFlags(0),
      mNumInputBuffers(0),
      mVideoFrameRate(-1),
      mCamera(0),
      mSurface(surface),
      mNumFramesReceived(0),
      mLastFrameTimestampUs(0),
      mStarted(false),
      mEos(false),
      mNumFramesEncoded(0),
      mTimeBetweenFrameCaptureUs(0),
      mFirstFrameTimeUs(0),
      mStopSystemTimeUs(-1),
      mNumFramesDropped(0),
      mNumGlitches(0),
      mGlitchDurationThresholdUs(200000),
      mCollectStats(false) {
    mVideoSize.width  = -1;
    mVideoSize.height = -1;

    mInitCheck = init(camera, proxy, cameraId,
                    clientName, clientUid, clientPid,
                    videoSize, frameRate);
    if (mInitCheck != OK) releaseCamera();
}

status_t CameraSource::initCheck() const {
    return mInitCheck;
}

status_t CameraSource::isCameraAvailable(
    const sp<hardware::ICamera>& camera, const sp<ICameraRecordingProxy>& proxy,
    int32_t cameraId, const std::string& clientName, uid_t clientUid, pid_t clientPid) {

    if (camera == 0) {
        AttributionSourceState clientAttribution;
        clientAttribution.pid = clientPid;
        clientAttribution.uid = clientUid;
        clientAttribution.deviceId = kDefaultDeviceId;
        clientAttribution.packageName = clientName;
        clientAttribution.token = sp<BBinder>::make();

        mCamera = Camera::connect(cameraId, /*targetSdkVersion*/__ANDROID_API_FUTURE__,
                /*rotationOverride*/hardware::ICameraService::ROTATION_OVERRIDE_NONE,
                /*forceSlowJpegMode*/false, clientAttribution);
        if (mCamera == 0) return -EBUSY;
        mCameraFlags &= ~FLAGS_HOT_CAMERA;
    } else {
        // We get the proxy from Camera, not ICamera. We need to get the proxy
        // to the remote Camera owned by the application. Here mCamera is a
        // local Camera object created by us. We cannot use the proxy from
        // mCamera here.
        mCamera = Camera::create(camera);
        if (mCamera == 0) return -EBUSY;
        mCameraRecordingProxy = proxy;
        mCameraFlags |= FLAGS_HOT_CAMERA;
        mDeathNotifier = new DeathNotifier();
        // isBinderAlive needs linkToDeath to work.
        IInterface::asBinder(mCameraRecordingProxy)->linkToDeath(mDeathNotifier);
    }

    mCamera->lock();

    return OK;
}


/*
 * Check to see whether the requested video width and height is one
 * of the supported sizes.
 * @param width the video frame width in pixels
 * @param height the video frame height in pixels
 * @param suppportedSizes the vector of sizes that we check against
 * @return true if the dimension (width and height) is supported.
 */
static bool isVideoSizeSupported(
    int32_t width, int32_t height,
    const Vector<Size>& supportedSizes) {

    ALOGV("isVideoSizeSupported");
    for (size_t i = 0; i < supportedSizes.size(); ++i) {
        if (width  == supportedSizes[i].width &&
            height == supportedSizes[i].height) {
            return true;
        }
    }
    return false;
}

/*
 * If the preview and video output is separate, we only set the
 * the video size, and applications should set the preview size
 * to some proper value, and the recording framework will not
 * change the preview size; otherwise, if the video and preview
 * output is the same, we need to set the preview to be the same
 * as the requested video size.
 *
 */
/*
 * Query the camera to retrieve the supported video frame sizes
 * and also to see whether CameraParameters::setVideoSize()
 * is supported or not.
 * @param params CameraParameters to retrieve the information
 * @@param isSetVideoSizeSupported retunrs whether method
 *      CameraParameters::setVideoSize() is supported or not.
 * @param sizes returns the vector of Size objects for the
 *      supported video frame sizes advertised by the camera.
 */
static void getSupportedVideoSizes(
    const CameraParameters& params,
    bool *isSetVideoSizeSupported,
    Vector<Size>& sizes) {

    *isSetVideoSizeSupported = true;
    params.getSupportedVideoSizes(sizes);
    if (sizes.size() == 0) {
        ALOGD("Camera does not support setVideoSize()");
        params.getSupportedPreviewSizes(sizes);
        *isSetVideoSizeSupported = false;
    }
}

/*
 * Check whether the camera has the supported color format
 * @param params CameraParameters to retrieve the information
 * @return OK if no error.
 */
status_t CameraSource::isCameraColorFormatSupported(
        const CameraParameters& params) {
    mColorFormat = getColorFormat(params.get(
            CameraParameters::KEY_VIDEO_FRAME_FORMAT));
    if (mColorFormat == -1) {
        return BAD_VALUE;
    }
    return OK;
}

static int32_t getHighSpeedFrameRate(const CameraParameters& params) {
    const char* hsr = params.get("video-hsr");
    int32_t rate = (hsr != NULL && strncmp(hsr, "off", 3)) ? strtol(hsr, NULL, 10) : 0;
    return std::min(rate, 240);
}

/*
 * Configure the camera to use the requested video size
 * (width and height) and/or frame rate. If both width and
 * height are -1, configuration on the video size is skipped.
 * if frameRate is -1, configuration on the frame rate
 * is skipped. Skipping the configuration allows one to
 * use the current camera setting without the need to
 * actually know the specific values (see Create() method).
 *
 * @param params the CameraParameters to be configured
 * @param width the target video frame width in pixels
 * @param height the target video frame height in pixels
 * @param frameRate the target frame rate in frames per second.
 * @return OK if no error.
 */
status_t CameraSource::configureCamera(
        CameraParameters* params,
        int32_t width, int32_t height,
        int32_t frameRate) {
    ALOGV("configureCamera");
    Vector<Size> sizes;
    bool isSetVideoSizeSupportedByCamera = true;
    getSupportedVideoSizes(*params, &isSetVideoSizeSupportedByCamera, sizes);
    bool isCameraParamChanged = false;
    if (width != -1 && height != -1) {
        if (!isVideoSizeSupported(width, height, sizes)) {
            ALOGE("Video dimension (%dx%d) is unsupported", width, height);
            return BAD_VALUE;
        }
        if (isSetVideoSizeSupportedByCamera) {
            params->setVideoSize(width, height);
        } else {
            params->setPreviewSize(width, height);
        }
        isCameraParamChanged = true;
    } else if ((width == -1 && height != -1) ||
               (width != -1 && height == -1)) {
        // If one and only one of the width and height is -1
        // we reject such a request.
        ALOGE("Requested video size (%dx%d) is not supported", width, height);
        return BAD_VALUE;
    } else {  // width == -1 && height == -1
        // Do not configure the camera.
        // Use the current width and height value setting from the camera.
    }

    if (frameRate != -1) {
        CHECK(frameRate > 0 && frameRate <= 240);
        const char* supportedFrameRates =
                params->get(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES);
        CHECK(supportedFrameRates != NULL);
        ALOGV("Supported frame rates: %s", supportedFrameRates);
        if (getHighSpeedFrameRate(*params)) {
            ALOGI("Use default 30fps for HighSpeed %dfps", frameRate);
            frameRate = 30;
        }
        char buf[4];
        snprintf(buf, 4, "%d", frameRate);
        if (strstr(supportedFrameRates, buf) == NULL) {
            ALOGE("Requested frame rate (%d) is not supported: %s",
                frameRate, supportedFrameRates);
            return BAD_VALUE;
        }

        // The frame rate is supported, set the camera to the requested value.
        params->setPreviewFrameRate(frameRate);
        isCameraParamChanged = true;
    } else {  // frameRate == -1
        // Do not configure the camera.
        // Use the current frame rate value setting from the camera
    }

    if (isCameraParamChanged) {
        // Either frame rate or frame size needs to be changed.
        String8 s = params->flatten();
        if (OK != mCamera->setParameters(s)) {
            ALOGE("Could not change settings."
                 " Someone else is using camera %p?", mCamera.get());
            return -EBUSY;
        }
    }
    return OK;
}

/*
 * Check whether the requested video frame size
 * has been successfully configured or not. If both width and height
 * are -1, check on the current width and height value setting
 * is performed.
 *
 * @param params CameraParameters to retrieve the information
 * @param the target video frame width in pixels to check against
 * @param the target video frame height in pixels to check against
 * @return OK if no error
 */
status_t CameraSource::checkVideoSize(
        const CameraParameters& params,
        int32_t width, int32_t height) {

    ALOGV("checkVideoSize");
    // The actual video size is the same as the preview size
    // if the camera hal does not support separate video and
    // preview output. In this case, we retrieve the video
    // size from preview.
    int32_t frameWidthActual = -1;
    int32_t frameHeightActual = -1;
    Vector<Size> sizes;
    params.getSupportedVideoSizes(sizes);
    if (sizes.size() == 0) {
        // video size is the same as preview size
        params.getPreviewSize(&frameWidthActual, &frameHeightActual);
    } else {
        // video size may not be the same as preview
        params.getVideoSize(&frameWidthActual, &frameHeightActual);
    }
    if (frameWidthActual < 0 || frameHeightActual < 0) {
        ALOGE("Failed to retrieve video frame size (%dx%d)",
                frameWidthActual, frameHeightActual);
        return UNKNOWN_ERROR;
    }

    // Check the actual video frame size against the target/requested
    // video frame size.
    if (width != -1 && height != -1) {
        if (frameWidthActual != width || frameHeightActual != height) {
            ALOGE("Failed to set video frame size to %dx%d. "
                    "The actual video size is %dx%d ", width, height,
                    frameWidthActual, frameHeightActual);
            return UNKNOWN_ERROR;
        }
    }

    // Good now.
    mVideoSize.width = frameWidthActual;
    mVideoSize.height = frameHeightActual;
    return OK;
}

/*
 * Check the requested frame rate has been successfully configured or not.
 * If the target frameRate is -1, check on the current frame rate value
 * setting is performed.
 *
 * @param params CameraParameters to retrieve the information
 * @param the target video frame rate to check against
 * @return OK if no error.
 */
status_t CameraSource::checkFrameRate(
        const CameraParameters& params,
        int32_t frameRate) {

    ALOGV("checkFrameRate");
    int32_t frameRateActual = params.getPreviewFrameRate();
    if (frameRateActual < 0) {
        ALOGE("Failed to retrieve preview frame rate (%d)", frameRateActual);
        return UNKNOWN_ERROR;
    }
    int32_t highSpeedRate = getHighSpeedFrameRate(params);
    frameRateActual = highSpeedRate ? highSpeedRate : frameRateActual;

    // Check the actual video frame rate against the target/requested
    // video frame rate.
    if (frameRate != -1 && (frameRateActual - frameRate) != 0) {
        ALOGE("Failed to set preview frame rate to %d fps. The actual "
                "frame rate is %d", frameRate, frameRateActual);
        return UNKNOWN_ERROR;
    }

    // Good now.
    mVideoFrameRate = frameRateActual;
    return OK;
}

/*
 * Initialize the CameraSource to so that it becomes
 * ready for providing the video input streams as requested.
 * @param camera the camera object used for the video source
 * @param cameraId if camera == 0, use camera with this id
 *      as the video source
 * @param videoSize the target video frame size. If both
 *      width and height in videoSize is -1, use the current
 *      width and heigth settings by the camera
 * @param frameRate the target frame rate in frames per second.
 *      if it is -1, use the current camera frame rate setting.
 * @param storeMetaDataInVideoBuffers request to store meta
 *      data or real YUV data in video buffers. Request to
 *      store meta data in video buffers may not be honored
 *      if the source does not support this feature.
 *
 * @return OK if no error.
 */
status_t CameraSource::init(
        const sp<hardware::ICamera>& camera,
        const sp<ICameraRecordingProxy>& proxy,
        int32_t cameraId,
        const String16& clientName,
        uid_t clientUid,
        pid_t clientPid,
        Size videoSize,
        int32_t frameRate) {

    ALOGV("init");
    status_t err = OK;
    int64_t token = IPCThreadState::self()->clearCallingIdentity();
    err = initWithCameraAccess(camera, proxy, cameraId, clientName, clientUid, clientPid,
                               videoSize, frameRate);
    IPCThreadState::self()->restoreCallingIdentity(token);
    return err;
}

void CameraSource::createVideoBufferMemoryHeap(size_t size, uint32_t bufferCount) {
    mMemoryHeapBase = new MemoryHeapBase(size * bufferCount, 0,
            "StageFright-CameraSource-BufferHeap");
    for (uint32_t i = 0; i < bufferCount; i++) {
        mMemoryBases.push_back(new MemoryBase(mMemoryHeapBase, i * size, size));
    }
}

status_t CameraSource::initBufferQueue(uint32_t width, uint32_t height,
        uint32_t format, android_dataspace dataSpace, uint32_t bufferCount) {
    ALOGV("initBufferQueue");

    if (mVideoBufferConsumer != nullptr || mVideoBufferProducer != nullptr) {
        ALOGE("%s: Buffer queue already exists", __FUNCTION__);
        return ALREADY_EXISTS;
    }

    uint32_t usage = GRALLOC_USAGE_SW_READ_OFTEN;
    if (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
        usage = GRALLOC_USAGE_HW_VIDEO_ENCODER;
    }

    bufferCount += kConsumerBufferCount;

    sp<Surface> surface;
    std::tie(mVideoBufferConsumer, surface) =
            BufferItemConsumer::create(usage, bufferCount);
    mVideoBufferConsumer->setName(String8::format("StageFright-CameraSource"));
#if WB_LIBCAMERASERVICE_WITH_DEPENDENCIES
    mVideoBufferProducer = surface;
#else
    mVideoBufferProducer = surface->getIGraphicBufferProducer();
#endif  // WB_LIBCAMERASERVICE_WITH_DEPENDENCIES

    status_t res = mVideoBufferConsumer->setDefaultBufferSize(width, height);
    if (res != OK) {
        ALOGE("%s: Could not set buffer dimensions %dx%d: %s (%d)", __FUNCTION__, width, height,
                strerror(-res), res);
        return res;
    }

    res = mVideoBufferConsumer->setDefaultBufferFormat(format);
    if (res != OK) {
        ALOGE("%s: Could not set buffer format %d: %s (%d)", __FUNCTION__, format,
                strerror(-res), res);
        return res;
    }

    res = mVideoBufferConsumer->setDefaultBufferDataSpace(dataSpace);
    if (res != OK) {
        ALOGE("%s: Could not set data space %d: %s (%d)", __FUNCTION__, dataSpace,
                strerror(-res), res);
        return res;
    }

    res = mCamera->setVideoTarget(mVideoBufferProducer);
    if (res != OK) {
        ALOGE("%s: Failed to set video target: %s (%d)", __FUNCTION__, strerror(-res), res);
        return res;
    }

    // Create memory heap to store buffers as VideoNativeMetadata.
    createVideoBufferMemoryHeap(sizeof(VideoNativeMetadata), bufferCount);

    mBufferQueueListener = new BufferQueueListener(mVideoBufferConsumer, this);
    res = mBufferQueueListener->run("CameraSource-BufferQueueListener");
    if (res != OK) {
        ALOGE("%s: Could not run buffer queue listener thread: %s (%d)", __FUNCTION__,
                strerror(-res), res);
        return res;
    }

    return OK;
}

status_t CameraSource::initWithCameraAccess(
        const sp<hardware::ICamera>& camera,
        const sp<ICameraRecordingProxy>& proxy,
        int32_t cameraId,
        const String16& clientName,
        uid_t clientUid,
        pid_t clientPid,
        Size videoSize,
        int32_t frameRate) {
    ALOGV("initWithCameraAccess");
    status_t err = OK;

    if ((err = isCameraAvailable(camera, proxy, cameraId,
            toStdString(clientName), clientUid, clientPid)) != OK) {
        ALOGE("Camera connection could not be established.");
        return err;
    }
    CameraParameters params(mCamera->getParameters());
    if ((err = isCameraColorFormatSupported(params)) != OK) {
        return err;
    }

    // Set the camera to use the requested video frame size
    // and/or frame rate.
    if ((err = configureCamera(&params,
                    videoSize.width, videoSize.height,
                    frameRate))) {
        return err;
    }

    // Check on video frame size and frame rate.
    CameraParameters newCameraParams(mCamera->getParameters());
    if ((err = checkVideoSize(newCameraParams,
                videoSize.width, videoSize.height)) != OK) {
        return err;
    }
    if ((err = checkFrameRate(newCameraParams, frameRate)) != OK) {
        return err;
    }

    // Set the preview display. Skip this if mSurface is null because
    // applications may already set a surface to the camera.
    if (mSurface != NULL) {
        // Surface may be set incorrectly or could already be used even if we just
        // passed the lock/unlock check earlier by calling mCamera->setParameters().
        if ((err = mCamera->setPreviewTarget(mSurface)) != OK) {
            return err;
        }
    }

    // Use buffer queue to receive video buffers from camera
    err = mCamera->setVideoBufferMode(hardware::ICamera::VIDEO_BUFFER_MODE_BUFFER_QUEUE);
    if (err != OK) {
        ALOGE("%s: Setting video buffer mode to VIDEO_BUFFER_MODE_BUFFER_QUEUE failed: "
                "%s (err=%d)", __FUNCTION__, strerror(-err), err);
        return err;
    }

    int64_t glitchDurationUs = (1000000LL / mVideoFrameRate);
    if (glitchDurationUs > mGlitchDurationThresholdUs) {
        mGlitchDurationThresholdUs = glitchDurationUs;
    }

    // XXX: query camera for the stride and slice height
    // when the capability becomes available.
    mMeta = new MetaData;
    mMeta->setCString(kKeyMIMEType,  MEDIA_MIMETYPE_VIDEO_RAW);
    mMeta->setInt32(kKeyColorFormat, mColorFormat);
    mMeta->setInt32(kKeyWidth,       mVideoSize.width);
    mMeta->setInt32(kKeyHeight,      mVideoSize.height);
    mMeta->setInt32(kKeyStride,      mVideoSize.width);
    mMeta->setInt32(kKeySliceHeight, mVideoSize.height);
    mMeta->setInt32(kKeyFrameRate,   mVideoFrameRate);
    return OK;
}

CameraSource::~CameraSource() {
    if (mStarted) {
        reset();
    } else if (mInitCheck == OK) {
        // Camera is initialized but because start() is never called,
        // the lock on Camera is never released(). This makes sure
        // Camera's lock is released in this case.
        releaseCamera();
    }
}

status_t CameraSource::startCameraRecording() {
    ALOGV("startCameraRecording");
    // Reset the identity to the current thread because media server owns the
    // camera and recording is started by the applications. The applications
    // will connect to the camera in ICameraRecordingProxy::startRecording.
    int64_t token = IPCThreadState::self()->clearCallingIdentity();
    status_t err;

    // Initialize buffer queue.
    err = initBufferQueue(mVideoSize.width, mVideoSize.height, mEncoderFormat,
            (android_dataspace_t)mEncoderDataSpace,
            mNumInputBuffers > 0 ? mNumInputBuffers : 1);
    if (err != OK) {
        ALOGE("%s: Failed to initialize buffer queue: %s (err=%d)", __FUNCTION__,
                strerror(-err), err);
        return err;
    }

    // Start data flow
    err = OK;
    if (mCameraFlags & FLAGS_HOT_CAMERA) {
        mCamera->unlock();
        mCamera.clear();
        if ((err = mCameraRecordingProxy->startRecording()) != OK) {
            ALOGE("Failed to start recording, received error: %s (%d)",
                    strerror(-err), err);
        }
    } else {
        mCamera->startRecording();
        if (!mCamera->recordingEnabled()) {
            err = -EINVAL;
            ALOGE("Failed to start recording");
        }
    }
    IPCThreadState::self()->restoreCallingIdentity(token);
    return err;
}

status_t CameraSource::start(MetaData *meta) {
    ALOGV("start");
    CHECK(!mStarted);
    if (mInitCheck != OK) {
        ALOGE("CameraSource is not initialized yet");
        return mInitCheck;
    }

    if (property_get_bool("media.stagefright.record-stats", false)) {
        mCollectStats = true;
    }

    mStartTimeUs = 0;
    mNumInputBuffers = 0;
    mEncoderFormat = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
    mEncoderDataSpace = mBufferDataSpace = HAL_DATASPACE_V0_BT709;

    if (meta) {
        int64_t startTimeUs;
        if (meta->findInt64(kKeyTime, &startTimeUs)) {
            mStartTimeUs = startTimeUs;
        }

        int32_t nBuffers;
        if (meta->findInt32(kKeyNumBuffers, &nBuffers)) {
            CHECK_GT(nBuffers, 0);
            mNumInputBuffers = nBuffers;
        }

        // apply encoder color format if specified
        if (meta->findInt32(kKeyPixelFormat, &mEncoderFormat)) {
            ALOGI("Using encoder format: %#x", mEncoderFormat);
        }
        if (meta->findInt32(kKeyColorSpace, &mEncoderDataSpace)) {
            ALOGI("Using encoder data space: %#x", mEncoderDataSpace);
            mBufferDataSpace = mEncoderDataSpace;
        }
    }

    status_t err;
    if ((err = startCameraRecording()) == OK) {
        mStarted = true;
    }

    return err;
}

void CameraSource::stopCameraRecording() {
    ALOGV("stopCameraRecording");
    if (mCameraFlags & FLAGS_HOT_CAMERA) {
        if (mCameraRecordingProxy != 0) {
            mCameraRecordingProxy->stopRecording();
        }
    } else {
        if (mCamera != 0) {
            mCamera->stopRecording();
        }
    }
}

void CameraSource::releaseCamera() {
    ALOGV("releaseCamera");
    sp<Camera> camera;
    bool coldCamera = false;
    {
        Mutex::Autolock autoLock(mLock);
        // get a local ref and clear ref to mCamera now
        camera = mCamera;
        mCamera.clear();
        coldCamera = (mCameraFlags & FLAGS_HOT_CAMERA) == 0;
    }

    if (camera != 0) {
        int64_t token = IPCThreadState::self()->clearCallingIdentity();
        if (coldCamera) {
            ALOGV("Camera was cold when we started, stopping preview");
            camera->stopPreview();
            camera->disconnect();
        }
        camera->unlock();
        IPCThreadState::self()->restoreCallingIdentity(token);
    }

    {
        Mutex::Autolock autoLock(mLock);
        if (mCameraRecordingProxy != 0) {
            IInterface::asBinder(mCameraRecordingProxy)->unlinkToDeath(mDeathNotifier);
            mCameraRecordingProxy.clear();
        }
        mCameraFlags = 0;
    }
}

status_t CameraSource::reset() {
    ALOGD("reset: E");

    {
        Mutex::Autolock autoLock(mLock);
        mStarted = false;
        mEos = false;
        mStopSystemTimeUs = -1;
        mFrameAvailableCondition.signal();

        int64_t token;
        bool isTokenValid = false;
        if (mCamera != 0) {
            token = IPCThreadState::self()->clearCallingIdentity();
            isTokenValid = true;
        }
        releaseQueuedFrames();
        while (!mFramesBeingEncoded.empty()) {
            if (NO_ERROR !=
                mFrameCompleteCondition.waitRelative(mLock,
                        mTimeBetweenFrameCaptureUs * 1000LL + CAMERA_SOURCE_TIMEOUT_NS)) {
                ALOGW("Timed out waiting for outstanding frames being encoded: %zu",
                    mFramesBeingEncoded.size());
            }
        }
        stopCameraRecording();
        if (isTokenValid) {
            IPCThreadState::self()->restoreCallingIdentity(token);
        }

        if (mCollectStats) {
            ALOGI("Frames received/encoded/dropped: %d/%d/%d in %" PRId64 " us",
                    mNumFramesReceived, mNumFramesEncoded, mNumFramesDropped,
                    mLastFrameTimestampUs - mFirstFrameTimeUs);
        }

        if (mNumGlitches > 0) {
            ALOGW("%d long delays between neighboring video frames", mNumGlitches);
        }

        CHECK_EQ(mNumFramesReceived, mNumFramesEncoded + mNumFramesDropped);
    }

    if (mBufferQueueListener != nullptr) {
        mBufferQueueListener->requestExit();
        mBufferQueueListener->join();
        mBufferQueueListener.clear();
    }

    mVideoBufferConsumer.clear();
    mVideoBufferProducer.clear();
    releaseCamera();

    ALOGD("reset: X");
    return OK;
}

void CameraSource::releaseRecordingFrame(const sp<IMemory>& frame) {
    ALOGV("releaseRecordingFrame");

    // Return the buffer to buffer queue in VIDEO_BUFFER_MODE_BUFFER_QUEUE mode.
    ssize_t offset;
    size_t size;
    sp<IMemoryHeap> heap = frame->getMemory(&offset, &size);
    if (heap->getHeapID() != mMemoryHeapBase->getHeapID()) {
        ALOGE("%s: Mismatched heap ID, ignoring release (got %x, expected %x)", __FUNCTION__,
                heap->getHeapID(), mMemoryHeapBase->getHeapID());
        return;
    }

    VideoNativeMetadata *payload = reinterpret_cast<VideoNativeMetadata*>(
        (uint8_t*)heap->getBase() + offset);

    // Find the corresponding buffer item for the native window buffer.
    ssize_t index = mReceivedBufferItemMap.indexOfKey(payload->pBuffer);
    if (index == NAME_NOT_FOUND) {
        ALOGE("%s: Couldn't find buffer item for %p", __FUNCTION__, payload->pBuffer);
        return;
    }

    BufferItem buffer = mReceivedBufferItemMap.valueAt(index);
    mReceivedBufferItemMap.removeItemsAt(index);
    mVideoBufferConsumer->releaseBuffer(buffer);
    mMemoryBases.push_back(frame);
    mMemoryBaseAvailableCond.signal();
}

void CameraSource::releaseQueuedFrames() {
    List<sp<IMemory> >::iterator it;
    while (!mFramesReceived.empty()) {
        it = mFramesReceived.begin();
        releaseRecordingFrame(*it);
        mFramesReceived.erase(it);
        ++mNumFramesDropped;
    }
}

sp<MetaData> CameraSource::getFormat() {
    return mMeta;
}

void CameraSource::releaseOneRecordingFrame(const sp<IMemory>& frame) {
    releaseRecordingFrame(frame);
}

void CameraSource::signalBufferReturned(MediaBufferBase *buffer) {
    ALOGV("signalBufferReturned: %p", buffer->data());
    Mutex::Autolock autoLock(mLock);
    for (List<sp<IMemory> >::iterator it = mFramesBeingEncoded.begin();
         it != mFramesBeingEncoded.end(); ++it) {
        if ((*it)->unsecurePointer() ==  buffer->data()) {
            releaseOneRecordingFrame((*it));
            mFramesBeingEncoded.erase(it);
            ++mNumFramesEncoded;
            buffer->setObserver(0);
            buffer->release();
            mFrameCompleteCondition.signal();
            return;
        }
    }
    CHECK(!"signalBufferReturned: bogus buffer");
}

status_t CameraSource::read(
        MediaBufferBase **buffer, const ReadOptions *options) {
    ALOGV("read");

    *buffer = NULL;

    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        return ERROR_UNSUPPORTED;
    }

    sp<IMemory> frame;
    int64_t frameTime;

    {
        Mutex::Autolock autoLock(mLock);
        while (mStarted && !mEos && mFramesReceived.empty()) {
            if (NO_ERROR !=
                mFrameAvailableCondition.waitRelative(mLock,
                    mTimeBetweenFrameCaptureUs * 1000LL + CAMERA_SOURCE_TIMEOUT_NS)) {
                if (mCameraRecordingProxy != 0 &&
                    !IInterface::asBinder(mCameraRecordingProxy)->isBinderAlive()) {
                    ALOGW("camera recording proxy is gone");
                    return ERROR_END_OF_STREAM;
                }
                ALOGW("Timed out waiting for incoming camera video frames: %" PRId64 " us",
                    mLastFrameTimestampUs);
            }
        }
        if (!mStarted) {
            return OK;
        }
        if (mFramesReceived.empty()) {
            return ERROR_END_OF_STREAM;
        }
        frame = *mFramesReceived.begin();
        mFramesReceived.erase(mFramesReceived.begin());

        frameTime = *mFrameTimes.begin();
        mFrameTimes.erase(mFrameTimes.begin());
        mFramesBeingEncoded.push_back(frame);
        // TODO: Using unsecurePointer() has some associated security pitfalls
        //       (see declaration for details).
        //       Either document why it is safe in this case or address the
        //       issue (e.g. by copying).
        *buffer = new MediaBuffer(frame->unsecurePointer(), frame->size());
        (*buffer)->setObserver(this);
        (*buffer)->add_ref();
        (*buffer)->meta_data().setInt64(kKeyTime, frameTime);
        if (mBufferDataSpace != mEncoderDataSpace) {
            ALOGD("Data space updated to %x", mBufferDataSpace);
            (*buffer)->meta_data().setInt32(kKeyColorSpace, mBufferDataSpace);
            mEncoderDataSpace = mBufferDataSpace;
        }
    }
    return OK;
}

status_t CameraSource::setStopTimeUs(int64_t stopTimeUs) {
    Mutex::Autolock autoLock(mLock);
    ALOGV("Set stoptime: %lld us", (long long)stopTimeUs);

    if (stopTimeUs < -1) {
        ALOGE("Invalid stop time %lld us", (long long)stopTimeUs);
        return BAD_VALUE;
    } else if (stopTimeUs == -1) {
        ALOGI("reset stopTime to be -1");
    }

    mStopSystemTimeUs = stopTimeUs;
    return OK;
}

bool CameraSource::shouldSkipFrameLocked(int64_t timestampUs) {
    if (!mStarted || (mNumFramesReceived == 0 && timestampUs < mStartTimeUs)) {
        ALOGV("Drop frame at %lld/%lld us", (long long)timestampUs, (long long)mStartTimeUs);
        return true;
    }

    if (mStopSystemTimeUs != -1 && timestampUs >= mStopSystemTimeUs) {
        ALOGV("Drop Camera frame at %lld  stop time: %lld us",
                (long long)timestampUs, (long long)mStopSystemTimeUs);
        mEos = true;
        mFrameAvailableCondition.signal();
        return true;
    }

    // May need to skip frame or modify timestamp. Currently implemented
    // by the subclass CameraSourceTimeLapse.
    if (skipCurrentFrame(timestampUs)) {
        return true;
    }

    if (mNumFramesReceived > 0) {
        if (timestampUs <= mLastFrameTimestampUs) {
            ALOGW("Dropping frame with backward timestamp %lld (last %lld)",
                    (long long)timestampUs, (long long)mLastFrameTimestampUs);
            return true;
        }
        if (timestampUs - mLastFrameTimestampUs > mGlitchDurationThresholdUs) {
            ++mNumGlitches;
        }
    }

    mLastFrameTimestampUs = timestampUs;
    if (mNumFramesReceived == 0) {
        mFirstFrameTimeUs = timestampUs;
        // Initial delay
        if (mStartTimeUs > 0) {
            if (timestampUs < mStartTimeUs) {
                // Frame was captured before recording was started
                // Drop it without updating the statistical data.
                return true;
            }
            mStartTimeUs = timestampUs - mStartTimeUs;
        }
    }

    return false;
}

CameraSource::BufferQueueListener::BufferQueueListener(const sp<BufferItemConsumer>& consumer,
        const sp<CameraSource>& cameraSource) {
    mConsumer = consumer;
    mConsumer->setFrameAvailableListener(this);
    mCameraSource = cameraSource;
}

void CameraSource::BufferQueueListener::onFrameAvailable(const BufferItem& /*item*/) {
    ALOGV("%s: onFrameAvailable", __FUNCTION__);

    Mutex::Autolock l(mLock);

    if (!mFrameAvailable) {
        mFrameAvailable = true;
        mFrameAvailableSignal.signal();
    }
}

bool CameraSource::BufferQueueListener::threadLoop() {
    if (mConsumer == nullptr || mCameraSource == nullptr) {
        return false;
    }

    {
        Mutex::Autolock l(mLock);
        while (!mFrameAvailable) {
            if (mFrameAvailableSignal.waitRelative(mLock, kFrameAvailableTimeout) == TIMED_OUT) {
                return true;
            }
        }
        mFrameAvailable = false;
    }

    BufferItem buffer;
    while (mConsumer->acquireBuffer(&buffer, 0) == OK) {
        mCameraSource->processBufferQueueFrame(buffer);
    }

    return true;
}

void CameraSource::processBufferQueueFrame(BufferItem& buffer) {
    Mutex::Autolock autoLock(mLock);

    int64_t timestampUs = buffer.mTimestamp / 1000;
    if (shouldSkipFrameLocked(timestampUs)) {
        mVideoBufferConsumer->releaseBuffer(buffer);
        return;
    }

    while (mMemoryBases.empty()) {
        if (mMemoryBaseAvailableCond.waitRelative(mLock, kMemoryBaseAvailableTimeoutNs) ==
                TIMED_OUT) {
            ALOGW("Waiting on an available memory base timed out. Dropping a recording frame.");
            mVideoBufferConsumer->releaseBuffer(buffer);
            return;
        }
    }

    ++mNumFramesReceived;

    // Find a available memory slot to store the buffer as VideoNativeMetadata.
    sp<IMemory> data = *mMemoryBases.begin();
    mMemoryBases.erase(mMemoryBases.begin());
    mBufferDataSpace = buffer.mDataSpace;

    ssize_t offset;
    size_t size;
    sp<IMemoryHeap> heap = data->getMemory(&offset, &size);
    VideoNativeMetadata *payload = reinterpret_cast<VideoNativeMetadata*>(
        (uint8_t*)heap->getBase() + offset);
    memset(payload, 0, sizeof(VideoNativeMetadata));
    payload->eType = kMetadataBufferTypeANWBuffer;
    payload->pBuffer = buffer.mGraphicBuffer->getNativeBuffer();
    payload->nFenceFd = -1;

    // Add the mapping so we can find the corresponding buffer item to release to the buffer queue
    // when the encoder returns the native window buffer.
    mReceivedBufferItemMap.add(payload->pBuffer, buffer);

    mFramesReceived.push_back(data);
    int64_t timeUs = mStartTimeUs + (timestampUs - mFirstFrameTimeUs);
    mFrameTimes.push_back(timeUs);
    ALOGV("initial delay: %" PRId64 ", current time stamp: %" PRId64,
        mStartTimeUs, timeUs);
    mFrameAvailableCondition.signal();
}

MetadataBufferType CameraSource::metaDataStoredInVideoBuffers() const {
    ALOGV("metaDataStoredInVideoBuffers");

    return kMetadataBufferTypeANWBuffer;
}

void CameraSource::DeathNotifier::binderDied(const wp<IBinder>& who __unused) {
    ALOGI("Camera recording proxy died");
}

}  // namespace android
