/*
 * Copyright (C) 2022 The Android Open Source Project
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

#define LOG_TAG "DeviceHalAidl"
// #define LOG_NDEBUG 0

#include <algorithm>

#include <aidl/android/hardware/audio/core/BnStreamCallback.h>
#include <aidl/android/hardware/audio/core/BnStreamOutEventCallback.h>
#include <aidl/android/hardware/audio/core/StreamDescriptor.h>
#include <android/binder_ibinder_platform.h>
#include <error/expected_utils.h>
#include <media/AidlConversionCppNdk.h>
#include <media/AidlConversionNdk.h>
#include <media/AidlConversionNdkCpp.h>
#include <media/AidlConversionUtil.h>
#include <mediautils/TimeCheck.h>
#include <system/audio.h>
#include <system/thread_defs.h>

#include <Utils.h>
#include <utils/Log.h>

#include "AidlUtils.h"
#include "DeviceHalAidl.h"
#include "EffectHalAidl.h"
#include "StreamHalAidl.h"

using aidl::android::aidl_utils::statusTFromBinderStatus;
using aidl::android::media::audio::common::Boolean;
using aidl::android::media::audio::common::AudioConfig;
using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioDeviceType;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioLatencyMode;
using aidl::android::media::audio::common::AudioMMapPolicy;
using aidl::android::media::audio::common::AudioMMapPolicyInfo;
using aidl::android::media::audio::common::AudioMMapPolicyType;
using aidl::android::media::audio::common::AudioMode;
using aidl::android::media::audio::common::AudioOutputFlags;
using aidl::android::media::audio::common::AudioPort;
using aidl::android::media::audio::common::AudioPortConfig;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::AudioSource;
using aidl::android::media::audio::common::Float;
using aidl::android::media::audio::common::Int;
using aidl::android::media::audio::common::MicrophoneDynamicInfo;
using aidl::android::media::audio::common::MicrophoneInfo;
using aidl::android::media::audio::IHalAdapterVendorExtension;
using aidl::android::hardware::audio::common::getFrameSizeInBytes;
using aidl::android::hardware::audio::common::isBitPositionFlagSet;
using aidl::android::hardware::audio::common::kDumpFromAudioServerArgument;
using aidl::android::hardware::audio::common::RecordTrackMetadata;
using aidl::android::hardware::audio::common::PlaybackTrackMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::hardware::audio::core::sounddose::ISoundDose;
using aidl::android::hardware::audio::core::AudioPatch;
using aidl::android::hardware::audio::core::AudioRoute;
using aidl::android::hardware::audio::core::IBluetooth;
using aidl::android::hardware::audio::core::IBluetoothA2dp;
using aidl::android::hardware::audio::core::IBluetoothLe;
using aidl::android::hardware::audio::core::IModule;
using aidl::android::hardware::audio::core::ITelephony;
using aidl::android::hardware::audio::core::ModuleDebug;
using aidl::android::hardware::audio::core::VendorParameter;

#define RETURN_IF_MODULE_NOT_INIT(retVal)         \
    if (!isModuleInitialized()) {                 \
        AUGMENT_LOG(E, "module not initialized"); \
        return retVal;                            \
    }

#define RETURN_IF_TELEPHONY_NOT_INIT(retVal)         \
    if (!isTelephonyInitialized()) {                  \
        AUGMENT_LOG(E, "telephony not initialized"); \
        return retVal;                               \
    }

namespace android {

namespace {

static constexpr int32_t kAidlVersion1 = 1;
static constexpr int32_t kAidlVersion2 = 2;
static constexpr int32_t kAidlVersion3 = 3;

// Note: these converters are for types defined in different AIDL files. Although these
// AIDL files are copies of each other, however formally these are different types
// thus we don't use a conversion via a parcelable.
ConversionResult<media::AudioRoute> ndk2cpp_AudioRoute(const AudioRoute& ndk) {
    media::AudioRoute cpp;
    cpp.sourcePortIds.insert(
            cpp.sourcePortIds.end(), ndk.sourcePortIds.begin(), ndk.sourcePortIds.end());
    cpp.sinkPortId = ndk.sinkPortId;
    cpp.isExclusive = ndk.isExclusive;
    return cpp;
}

template<typename T>
std::shared_ptr<T> retrieveSubInterface(const std::shared_ptr<IModule>& module,
        ::ndk::ScopedAStatus (IModule::*getT)(std::shared_ptr<T>*)) {
    if (module != nullptr) {
        std::shared_ptr<T> instance;
        if (auto status = (module.get()->*getT)(&instance); status.isOk()) {
            return instance;
        }
    }
    return nullptr;
}

}  // namespace

DeviceHalAidl::DeviceHalAidl(const std::string& instance, const std::shared_ptr<IModule>& module,
                             const std::shared_ptr<IHalAdapterVendorExtension>& vext)
    : ConversionHelperAidl("DeviceHalAidl", instance),
      mModule(module),
      mTelephony(retrieveSubInterface<ITelephony>(module, &IModule::getTelephony)),
      mBluetooth(retrieveSubInterface<IBluetooth>(module, &IModule::getBluetooth)),
      mBluetoothA2dp(retrieveSubInterface<IBluetoothA2dp>(module, &IModule::getBluetoothA2dp)),
      mBluetoothLe(retrieveSubInterface<IBluetoothLe>(module, &IModule::getBluetoothLe)),
      mSoundDose(retrieveSubInterface<ISoundDose>(module, &IModule::getSoundDose)),
      mVendorExt(vext),
      mMapper(instance, module),
      mMapperAccessor(mMapper, mLock) {}

status_t DeviceHalAidl::getAudioPorts(std::vector<media::audio::common::AudioPort> *ports) {
    std::lock_guard l(mLock);
    return mMapper.getAudioPorts(ports, ndk2cpp_AudioPort);
}

status_t DeviceHalAidl::getAudioRoutes(std::vector<media::AudioRoute> *routes) {
    std::lock_guard l(mLock);
    return mMapper.getAudioRoutes(routes, ndk2cpp_AudioRoute);
}

status_t DeviceHalAidl::getSupportedModes(std::vector<media::audio::common::AudioMode> *modes) {
    AUGMENT_LOG(D);
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    RETURN_IF_TELEPHONY_NOT_INIT(INVALID_OPERATION);

    if (modes == nullptr) {
        AUGMENT_LOG(E, "uninitialized modes");
        return BAD_VALUE;
    }
    std::vector<AudioMode> aidlModes;
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(
                statusTFromBinderStatus(mTelephony->getSupportedAudioModes(&aidlModes)));
    }
    *modes = VALUE_OR_RETURN_STATUS(
            ::aidl::android::convertContainer<std::vector<media::audio::common::AudioMode>>(
                    aidlModes, ndk2cpp_AudioMode));
    return OK;
}

status_t DeviceHalAidl::getSupportedDevices(uint32_t*) {
    // Obsolete.
    return INVALID_OPERATION;
}

status_t DeviceHalAidl::initCheck() {
    AUGMENT_LOG(D);
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    std::lock_guard l(mLock);
    int32_t aidlVersion = 0;
    RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->getInterfaceVersion(&aidlVersion)));
    if (aidlVersion > kAidlVersion3) {
        mHasClipTransitionSupport = true;
    } else {
        AudioParameter parameterKeys;
        parameterKeys.addKey(String8(AudioParameter::keyClipTransitionSupport));
        String8 values;
        auto status = parseAndGetVendorParameters(mVendorExt, mModule, parameterKeys, &values);
        mHasClipTransitionSupport = status == OK && !values.empty();
    }
    return mMapper.initialize();
}

status_t DeviceHalAidl::setVoiceVolume(float volume) {
    AUGMENT_LOG(D, "volume %f", volume);
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    RETURN_IF_TELEPHONY_NOT_INIT(INVALID_OPERATION);

    ITelephony::TelecomConfig inConfig{.voiceVolume = Float{volume}}, outConfig;
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(
                statusTFromBinderStatus(mTelephony->setTelecomConfig(inConfig, &outConfig)));
    }
    AUGMENT_LOG_IF(
            W, outConfig.voiceVolume.has_value() && volume != outConfig.voiceVolume.value().value,
            "the resulting voice volume %f is not the same as requested %f",
            outConfig.voiceVolume.value().value, volume);
    return OK;
}

status_t DeviceHalAidl::setMasterVolume(float volume) {
    AUGMENT_LOG(D, "volume %f", volume);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    std::lock_guard l(mLock);
    return statusTFromBinderStatus(mModule->setMasterVolume(volume));
}

status_t DeviceHalAidl::getMasterVolume(float *volume) {
    AUGMENT_LOG(D);
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (volume == nullptr) {
        AUGMENT_LOG(E, "uninitialized volumes");
        return BAD_VALUE;
    }
    std::lock_guard l(mLock);
    return statusTFromBinderStatus(mModule->getMasterVolume(volume));
}

status_t DeviceHalAidl::setMode(audio_mode_t mode) {
    AUGMENT_LOG(D, "mode %d", mode);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    AudioMode audioMode = VALUE_OR_FATAL(::aidl::android::legacy2aidl_audio_mode_t_AudioMode(mode));
    std::lock_guard l(mLock);
    if (mTelephony != nullptr) {
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mTelephony->switchAudioMode(audioMode)));
    }
    return statusTFromBinderStatus(mModule->updateAudioMode(audioMode));
}

status_t DeviceHalAidl::setMicMute(bool state) {
    AUGMENT_LOG(D, "mute %d", state);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    std::lock_guard l(mLock);
    return statusTFromBinderStatus(mModule->setMicMute(state));
}

status_t DeviceHalAidl::getMicMute(bool *state) {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (state == nullptr) {
        AUGMENT_LOG(E, "uninitialized mute state");
        return BAD_VALUE;
    }
    std::lock_guard l(mLock);
    return statusTFromBinderStatus(mModule->getMicMute(state));
}

status_t DeviceHalAidl::setMasterMute(bool state) {
    AUGMENT_LOG(D, "mute %d", state);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    std::lock_guard l(mLock);
    return statusTFromBinderStatus(mModule->setMasterMute(state));
}

status_t DeviceHalAidl::getMasterMute(bool *state) {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (state == nullptr) {
        AUGMENT_LOG(E, "uninitialized mute state");
        return BAD_VALUE;
    }
    std::lock_guard l(mLock);
    return statusTFromBinderStatus(mModule->getMasterMute(state));
}

status_t DeviceHalAidl::setParameters(const String8& kvPairs) {
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    AudioParameter parameters(kvPairs);
    AUGMENT_LOG(D, "parameters: \"%s\"", parameters.toString().c_str());

    if (status_t status = filterAndUpdateBtA2dpParameters(parameters); status != OK) {
        AUGMENT_LOG(W, "filterAndUpdateBtA2dpParameters failed: %d", status);
    }
    if (status_t status = filterAndUpdateBtHfpParameters(parameters); status != OK) {
        AUGMENT_LOG(W, "filterAndUpdateBtHfpParameters failed: %d", status);
    }
    if (status_t status = filterAndUpdateBtLeParameters(parameters); status != OK) {
        AUGMENT_LOG(W, "filterAndUpdateBtLeParameters failed: %d", status);
    }
    if (status_t status = filterAndUpdateBtScoParameters(parameters); status != OK) {
        AUGMENT_LOG(W, "filterAndUpdateBtScoParameters failed: %d", status);
    }
    if (status_t status = filterAndUpdateScreenParameters(parameters); status != OK) {
        AUGMENT_LOG(W, "filterAndUpdateScreenParameters failed: %d", status);
    }
    if (status_t status = filterAndUpdateTelephonyParameters(parameters); status != OK) {
        AUGMENT_LOG(W, "filterAndUpdateTelephonyParameters failed: %d", status);
    }
    std::lock_guard l(mLock);
    return parseAndSetVendorParameters(mVendorExt, mModule, parameters);
}

status_t DeviceHalAidl::getParameters(const String8& keys, String8 *values) {
    AUGMENT_LOG(D, "keys: \"%s\"", keys.c_str());

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (values == nullptr) {
        AUGMENT_LOG(E, "invalid values");
        return BAD_VALUE;
    }
    AudioParameter parameterKeys(keys), result;
    if (status_t status = filterAndRetrieveBtA2dpParameters(parameterKeys, &result); status != OK) {
        AUGMENT_LOG(W, "filterAndRetrieveBtA2dpParameters failed: %d", status);
    }
    if (status_t status = filterAndRetrieveBtLeParameters(parameterKeys, &result); status != OK) {
        AUGMENT_LOG(W, "filterAndRetrieveBtLeParameters failed: %d", status);
    }
    *values = result.toString();
    std::lock_guard l(mLock);
    return parseAndGetVendorParameters(mVendorExt, mModule, parameterKeys, values);
}

status_t DeviceHalAidl::getInputBufferSize(struct audio_config* config, size_t* size) {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (config == nullptr || size == nullptr) {
        AUGMENT_LOG(E, "invalid config or size");
        return BAD_VALUE;
    }
    constexpr bool isInput = true;
    AudioConfig aidlConfig = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_config_t_AudioConfig(*config, isInput));
    AudioDevice aidlDevice;
    aidlDevice.type.type = AudioDeviceType::IN_DEFAULT;
    AudioSource aidlSource = AudioSource::DEFAULT;
    AudioIoFlags aidlFlags = AudioIoFlags::make<AudioIoFlags::Tag::input>(0);
    AudioPortConfig mixPortConfig;
    Hal2AidlMapper::Cleanups cleanups(mMapperAccessor);
    AudioPatch aidlPatch;
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(mMapper.prepareToOpenStream(
                        0 /*handle*/, aidlDevice, aidlFlags, aidlSource,
                        &cleanups, &aidlConfig, &mixPortConfig, &aidlPatch));
    }
    *config = VALUE_OR_RETURN_STATUS(
            ::aidl::android::aidl2legacy_AudioConfig_audio_config_t(aidlConfig, isInput));
    if (mixPortConfig.id == 0) return BAD_VALUE;  // HAL suggests a different config.
    *size = aidlConfig.frameCount *
            getFrameSizeInBytes(aidlConfig.base.format, aidlConfig.base.channelMask);
    // Do not disarm cleanups to release temporary port configs.
    return OK;
}

namespace {

class StreamCallbackBase {
  protected:
    explicit StreamCallbackBase(const sp<CallbackBroker>& broker) : mBroker(broker) {}
  public:
    void* getCookie() const { return mCookie; }
    void setCookie(void* cookie) { mCookie = cookie; }
    sp<CallbackBroker> getBroker() const {
        if (void* cookie = mCookie; cookie != nullptr) return mBroker.promote();
        return nullptr;
    }
  private:
    const wp<CallbackBroker> mBroker;
    std::atomic<void*> mCookie;
};

template<class C>
class StreamCallbackBaseHelper {
  protected:
    explicit StreamCallbackBaseHelper(const StreamCallbackBase& base) : mBase(base) {}
    sp<C> getCb(const sp<CallbackBroker>& broker, void* cookie);
    using CbRef = const sp<C>&;
    ndk::ScopedAStatus runCb(const std::function<void(CbRef cb)>& f) {
        if (auto cb = getCb(mBase.getBroker(), mBase.getCookie()); cb != nullptr) f(cb);
        return ndk::ScopedAStatus::ok();
    }
  private:
    const StreamCallbackBase& mBase;
};

template<>
sp<StreamOutHalInterfaceCallback> StreamCallbackBaseHelper<StreamOutHalInterfaceCallback>::getCb(
        const sp<CallbackBroker>& broker, void* cookie) {
    if (broker != nullptr) return broker->getStreamOutCallback(cookie);
    return nullptr;
}

template<>
sp<StreamOutHalInterfaceEventCallback>
StreamCallbackBaseHelper<StreamOutHalInterfaceEventCallback>::getCb(
        const sp<CallbackBroker>& broker, void* cookie) {
    if (broker != nullptr) return broker->getStreamOutEventCallback(cookie);
    return nullptr;
}

template<>
sp<StreamOutHalInterfaceLatencyModeCallback>
StreamCallbackBaseHelper<StreamOutHalInterfaceLatencyModeCallback>::getCb(
        const sp<CallbackBroker>& broker, void* cookie) {
    if (broker != nullptr) return broker->getStreamOutLatencyModeCallback(cookie);
    return nullptr;
}

/*
Note on the callback ownership.

In the Binder ownership model, the server implementation is kept alive
as long as there is any client (proxy object) alive. This is done by
incrementing the refcount of the server-side object by the Binder framework.
When it detects that the last client is gone, it decrements the refcount back.

Thus, it is not needed to keep any references to StreamCallback on our
side (after we have sent an instance to the client), because we are
the server-side. The callback object will be kept alive as long as the HAL server
holds a strong ref to IStreamCallback proxy.
*/

class OutputStreamCallbackAidl : public StreamCallbackBase,
                             public StreamCallbackBaseHelper<StreamOutHalInterfaceCallback>,
                             public ::aidl::android::hardware::audio::core::BnStreamCallback {
  public:
    explicit OutputStreamCallbackAidl(const sp<CallbackBroker>& broker)
            : StreamCallbackBase(broker),
              StreamCallbackBaseHelper<StreamOutHalInterfaceCallback>(
                      *static_cast<StreamCallbackBase*>(this)) {}
    ndk::ScopedAStatus onTransferReady() override {
        return runCb([](CbRef cb) { cb->onWriteReady(); });
    }
    ndk::ScopedAStatus onError() override {
        return runCb([](CbRef cb) { cb->onError(true /*isHardError*/); });
    }
    ndk::ScopedAStatus onDrainReady() override {
        return runCb([](CbRef cb) { cb->onDrainReady(); });
    }
};

class OutputStreamEventCallbackAidl :
            public StreamCallbackBase,
            public StreamCallbackBaseHelper<StreamOutHalInterfaceEventCallback>,
            public StreamCallbackBaseHelper<StreamOutHalInterfaceLatencyModeCallback>,
            public ::aidl::android::hardware::audio::core::BnStreamOutEventCallback {
  public:
    explicit OutputStreamEventCallbackAidl(const sp<CallbackBroker>& broker)
            : StreamCallbackBase(broker),
              StreamCallbackBaseHelper<StreamOutHalInterfaceEventCallback>(
                      *static_cast<StreamCallbackBase*>(this)),
              StreamCallbackBaseHelper<StreamOutHalInterfaceLatencyModeCallback>(
                      *static_cast<StreamCallbackBase*>(this)) {}
    ndk::ScopedAStatus onCodecFormatChanged(const std::vector<uint8_t>& halMetadata) override {
        return StreamCallbackBaseHelper<StreamOutHalInterfaceEventCallback>::runCb(
                [&halMetadata](auto cb) { cb->onCodecFormatChanged(halMetadata); });
    }
    ndk::ScopedAStatus onRecommendedLatencyModeChanged(
            const std::vector<AudioLatencyMode>& in_modes) override {
        auto halModes = VALUE_OR_FATAL(
                ::aidl::android::convertContainer<std::vector<audio_latency_mode_t>>(
                        in_modes,
                        ::aidl::android::aidl2legacy_AudioLatencyMode_audio_latency_mode_t));
        return StreamCallbackBaseHelper<StreamOutHalInterfaceLatencyModeCallback>::runCb(
                [&halModes](auto cb) { cb->onRecommendedLatencyModeChanged(halModes); });
    }
};

}  // namespace

status_t DeviceHalAidl::openOutputStream(
        audio_io_handle_t handle, audio_devices_t devices,
        audio_output_flags_t flags, struct audio_config* config,
        const char* address,
        sp<StreamOutHalInterface>* outStream,
        const std::vector<playback_track_metadata_v7_t>& sourceMetadata) {
    AUGMENT_LOG(D, "handle: %d devices %0x flags %0x", handle, devices, flags);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (outStream == nullptr || config == nullptr) {
        AUGMENT_LOG(E, "invalid outStream or config");
        return BAD_VALUE;
    }
    constexpr bool isInput = false;
    int32_t aidlHandle = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_io_handle_t_int32_t(handle));
    AudioConfig aidlConfig = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_config_t_AudioConfig(*config, isInput));
    AudioDevice aidlDevice = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_device_AudioDevice(devices, address));
    int32_t aidlOutputFlags = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_output_flags_t_int32_t_mask(flags));
    SourceMetadata aidlMetadata = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_playback_track_metadata_v7_SourceMetadata(sourceMetadata));
    AudioIoFlags aidlFlags = AudioIoFlags::make<AudioIoFlags::Tag::output>(aidlOutputFlags);
    AudioPortConfig mixPortConfig;
    AudioPatch aidlPatch;

    Hal2AidlMapper::Cleanups cleanups(mMapperAccessor);
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(mMapper.prepareToOpenStream(aidlHandle, aidlDevice, aidlFlags,
                        AudioSource::SYS_RESERVED_INVALID /*only needed for input*/,
                        &cleanups, &aidlConfig, &mixPortConfig, &aidlPatch));
    }
    *config = VALUE_OR_RETURN_STATUS(
            ::aidl::android::aidl2legacy_AudioConfig_audio_config_t(aidlConfig, isInput));
    if (mixPortConfig.id == 0) return BAD_VALUE;  // HAL suggests a different config.
    ::aidl::android::hardware::audio::core::IModule::OpenOutputStreamArguments args;
    args.portConfigId = mixPortConfig.id;
    const bool isOffload = isBitPositionFlagSet(
            aidlOutputFlags, AudioOutputFlags::COMPRESS_OFFLOAD);
    const bool isHwAvSync = isBitPositionFlagSet(
            aidlOutputFlags, AudioOutputFlags::HW_AV_SYNC);
    std::shared_ptr<OutputStreamCallbackAidl> streamCb;
    if (isOffload) {
        streamCb = ndk::SharedRefBase::make<OutputStreamCallbackAidl>(this);
        ndk::SpAIBinder binder = streamCb->asBinder();
        AIBinder_setMinSchedulerPolicy(binder.get(), SCHED_NORMAL, ANDROID_PRIORITY_AUDIO);
        AIBinder_setInheritRt(binder.get(), true);
    }
    auto eventCb = ndk::SharedRefBase::make<OutputStreamEventCallbackAidl>(this);
    ndk::SpAIBinder binder = eventCb->asBinder();
    AIBinder_setMinSchedulerPolicy(binder.get(), SCHED_NORMAL, ANDROID_PRIORITY_AUDIO);
    AIBinder_setInheritRt(binder.get(), true);

    if (isOffload || isHwAvSync) {
        args.offloadInfo = aidlConfig.offloadInfo;
    }
    if (isOffload) {
        args.callback = streamCb;
    }
    args.bufferSizeFrames = aidlConfig.frameCount;
    args.eventCallback = eventCb;
    args.sourceMetadata = aidlMetadata;
    ::aidl::android::hardware::audio::core::IModule::OpenOutputStreamReturn ret;
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->openOutputStream(args, &ret)));
    }
    StreamContextAidl context(ret.desc, isOffload, aidlHandle, mHasClipTransitionSupport);
    if (!context.isValid()) {
        AUGMENT_LOG(E, "Failed to created a valid stream context from the descriptor: %s",
                    ret.desc.toString().c_str());
        return NO_INIT;
    }
    auto stream = sp<StreamOutHalAidl>::make(*config, std::move(context), aidlPatch.latenciesMs[0],
            std::move(ret.stream), mVendorExt, this /*callbackBroker*/);
    *outStream = stream;
    /* StreamOutHalInterface* */ void* cbCookie = (*outStream).get();
    {
        std::lock_guard l(mCallbacksLock);
        mCallbacks.emplace(cbCookie, Callbacks{});
    }
    {
        std::lock_guard l(mLock);
        mMapper.addStream(*outStream, mixPortConfig.id, aidlPatch.id);
    }
    if (streamCb) {
        streamCb->setCookie(cbCookie);
        // Although StreamOutHalAidl implements StreamOutHalInterfaceCallback,
        // we always go via the CallbackBroker for consistency.
        setStreamOutCallback(cbCookie, stream);
    }
    eventCb->setCookie(cbCookie);
    cleanups.disarmAll();
    return OK;
}

status_t DeviceHalAidl::openInputStream(
        audio_io_handle_t handle, audio_devices_t devices,
        struct audio_config* config, audio_input_flags_t flags,
        const char* address, audio_source_t source,
        audio_devices_t outputDevice, const char* outputDeviceAddress,
        sp<StreamInHalInterface>* inStream) {
    AUGMENT_LOG(D, "handle: %d devices %0x flags %0x", handle, devices, flags);
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (inStream == nullptr || config == nullptr) {
        AUGMENT_LOG(E, "invalid inStream or config");
        return BAD_VALUE;
    }
    constexpr bool isInput = true;
    int32_t aidlHandle = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_io_handle_t_int32_t(handle));
    AudioConfig aidlConfig = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_config_t_AudioConfig(*config, isInput));
    AudioDevice aidlDevice = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_device_AudioDevice(devices, address));
    int32_t aidlInputFlags = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_input_flags_t_int32_t_mask(flags));
    AudioIoFlags aidlFlags = AudioIoFlags::make<AudioIoFlags::Tag::input>(aidlInputFlags);
    AudioSource aidlSource = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_source_t_AudioSource(source));
    AudioPortConfig mixPortConfig;
    AudioPatch aidlPatch;
    Hal2AidlMapper::Cleanups cleanups(mMapperAccessor);
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(mMapper.prepareToOpenStream(
                        aidlHandle, aidlDevice, aidlFlags, aidlSource,
                        &cleanups, &aidlConfig, &mixPortConfig, &aidlPatch));
    }
    *config = VALUE_OR_RETURN_STATUS(
            ::aidl::android::aidl2legacy_AudioConfig_audio_config_t(aidlConfig, isInput));
    if (mixPortConfig.id == 0) return BAD_VALUE;  // HAL suggests a different config.
    ::aidl::android::hardware::audio::core::IModule::OpenInputStreamArguments args;
    args.portConfigId = mixPortConfig.id;
    RecordTrackMetadata aidlTrackMetadata{
        .source = aidlSource, .gain = 1, .channelMask = aidlConfig.base.channelMask };
    if (outputDevice != AUDIO_DEVICE_NONE) {
        aidlTrackMetadata.destinationDevice = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_device_AudioDevice(
                    outputDevice, outputDeviceAddress));
    }
    args.sinkMetadata.tracks.push_back(std::move(aidlTrackMetadata));
    args.bufferSizeFrames = aidlConfig.frameCount;
    ::aidl::android::hardware::audio::core::IModule::OpenInputStreamReturn ret;
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->openInputStream(args, &ret)));
    }
    StreamContextAidl context(
            ret.desc, false /*isAsynchronous*/, aidlHandle, mHasClipTransitionSupport);
    if (!context.isValid()) {
        AUGMENT_LOG(E, "Failed to created a valid stream context from the descriptor: %s",
                    ret.desc.toString().c_str());
        return NO_INIT;
    }
    *inStream = sp<StreamInHalAidl>::make(*config, std::move(context), aidlPatch.latenciesMs[0],
            std::move(ret.stream), mVendorExt, this /*micInfoProvider*/);
    {
        std::lock_guard l(mLock);
        mMapper.addStream(*inStream, mixPortConfig.id, aidlPatch.id);
    }
    cleanups.disarmAll();
    return OK;
}

status_t DeviceHalAidl::supportsAudioPatches(bool* supportsPatches) {
    AUGMENT_LOG(V);
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (supportsPatches == nullptr) {
        AUGMENT_LOG(E, "uninitialized supportsPatches");
        return BAD_VALUE;
    }
    *supportsPatches = true;
    return OK;
}

status_t DeviceHalAidl::createAudioPatch(unsigned int num_sources,
                                         const struct audio_port_config* sources,
                                         unsigned int num_sinks,
                                         const struct audio_port_config* sinks,
                                         audio_patch_handle_t* patch) {
    AUGMENT_LOG(D, "sources: %d sinks %d", num_sources, num_sinks);
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (num_sinks > AUDIO_PATCH_PORTS_MAX || num_sources > AUDIO_PATCH_PORTS_MAX) {
        AUGMENT_LOG(E, "invalid sources %d or sinks %d ", num_sources, num_sinks);
        return BAD_VALUE;
    }

    if (sources == nullptr || sinks == nullptr || patch == nullptr) {
        AUGMENT_LOG(E, "uninitialized sources %d or sinks %d or patches %d", (sources == nullptr),
                    (sinks == nullptr), (patch == nullptr));
        return BAD_VALUE;
    }

    // When the patch handle (*patch) is AUDIO_PATCH_HANDLE_NONE, it means
    // the framework wants to create a new patch. The handle has to be generated
    // by the HAL. Since handles generated this way can only be unique within
    // a HAL module, the framework generates a globally unique handle, and maps
    // it on the <HAL module, patch handle> pair.
    // When the patch handle is set, it meant the framework intends to update
    // an existing patch.
    //
    // This behavior corresponds to HAL module behavior, with the only difference
    // that the HAL module uses `int32_t` for patch IDs. The following assert ensures
    // that both the framework and the HAL use the same value for "no ID":
    static_assert(AUDIO_PATCH_HANDLE_NONE == 0);

    // Upon conversion, mix port configs contain audio configuration, while
    // device port configs contain device address. This data is used to find
    // or create HAL configs.
    std::vector<AudioPortConfig> aidlSources, aidlSinks;
    for (unsigned int i = 0; i < num_sources; ++i) {
        bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(
                        sources[i].role, sources[i].type)) ==
                ::aidl::android::AudioPortDirection::INPUT;
        aidlSources.push_back(VALUE_OR_RETURN_STATUS(
                        ::aidl::android::legacy2aidl_audio_port_config_AudioPortConfig(
                                sources[i], isInput, 0)));
    }
    for (unsigned int i = 0; i < num_sinks; ++i) {
        bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(
                        sinks[i].role, sinks[i].type)) ==
                ::aidl::android::AudioPortDirection::INPUT;
        aidlSinks.push_back(VALUE_OR_RETURN_STATUS(
                        ::aidl::android::legacy2aidl_audio_port_config_AudioPortConfig(
                                sinks[i], isInput, 0)));
    }
    int32_t aidlPatchId = static_cast<int32_t>(*patch);
    Hal2AidlMapper::Cleanups cleanups(mMapperAccessor);
    {
        std::lock_guard l(mLock);
        // Check for patches that only exist for the framework, or have different HAL patch ID.
        if (int32_t aidlHalPatchId = mMapper.findFwkPatch(aidlPatchId); aidlHalPatchId != 0) {
            if (aidlHalPatchId == aidlPatchId) {
                // This patch was previously released by the HAL. Thus we need to pass '0'
                // to the HAL to obtain a new patch.
                int32_t newAidlPatchId = 0;
                RETURN_STATUS_IF_ERROR(mMapper.createOrUpdatePatch(
                                aidlSources, aidlSinks, &newAidlPatchId, &cleanups));
                mMapper.updateFwkPatch(aidlPatchId, newAidlPatchId);
            } else {
                RETURN_STATUS_IF_ERROR(mMapper.createOrUpdatePatch(
                                aidlSources, aidlSinks, &aidlHalPatchId, &cleanups));
            }
        } else {
            RETURN_STATUS_IF_ERROR(mMapper.createOrUpdatePatch(
                            aidlSources, aidlSinks, &aidlPatchId, &cleanups));
        }
    }
    *patch = static_cast<audio_patch_handle_t>(aidlPatchId);
    cleanups.disarmAll();
    return OK;
}

status_t DeviceHalAidl::releaseAudioPatch(audio_patch_handle_t patch) {
    AUGMENT_LOG(D, "patch: %d", patch);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    static_assert(AUDIO_PATCH_HANDLE_NONE == 0);
    if (patch == AUDIO_PATCH_HANDLE_NONE) {
        return BAD_VALUE;
    }
    std::lock_guard l(mLock);
    // Check for patches that only exist for the framework, or have different HAL patch ID.
    int32_t aidlPatchId = static_cast<int32_t>(patch);
    if (int32_t aidlHalPatchId = mMapper.findFwkPatch(aidlPatchId); aidlHalPatchId != 0) {
        if (aidlHalPatchId == aidlPatchId) {
            // This patch was previously released by the HAL, just need to finish its removal.
            mMapper.eraseFwkPatch(aidlPatchId);
            return OK;
        } else {
            // This patch has a HAL patch ID which is different
            aidlPatchId = aidlHalPatchId;
        }
    }
    RETURN_STATUS_IF_ERROR(mMapper.releaseAudioPatch(aidlPatchId));
    return OK;
}

status_t DeviceHalAidl::getAudioPort(struct audio_port* port) {
    AUGMENT_LOG(V);
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (port == nullptr) {
        AUGMENT_LOG(E, "port not initialized");
        return BAD_VALUE;
    }
    audio_port_v7 portV7;
    audio_populate_audio_port_v7(port, &portV7);
    RETURN_STATUS_IF_ERROR(getAudioPort(&portV7));
    return audio_populate_audio_port(&portV7, port) ? OK : BAD_VALUE;
}

status_t DeviceHalAidl::getAudioPort(struct audio_port_v7 *port) {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (port == nullptr) {
        AUGMENT_LOG(E, "port not initialized");
        return BAD_VALUE;
    }
    bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(port->role, port->type)) ==
            ::aidl::android::AudioPortDirection::INPUT;
    auto aidlPort = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_port_v7_AudioPort(*port, isInput));
    if (aidlPort.ext.getTag() != AudioPortExt::device) {
        AUGMENT_LOG(E, "provided port is not a device port %s", aidlPort.toString().c_str());
        return BAD_VALUE;
    }
    const auto& matchDevice = aidlPort.ext.get<AudioPortExt::device>().device;
    // It seems that we don't have to call HAL since all valid ports have been added either
    // during initialization, or while handling connection of an external device.
    const int32_t fwkId = aidlPort.id;
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(mMapper.getAudioPortCached(matchDevice, &aidlPort));
    }
    aidlPort.id = fwkId;
    *port = VALUE_OR_RETURN_STATUS(::aidl::android::aidl2legacy_AudioPort_audio_port_v7(
                    aidlPort, isInput));
    return OK;
}

status_t DeviceHalAidl::getAudioMixPort(const struct audio_port_v7 *devicePort,
                                        struct audio_port_v7 *mixPort) {
    AUGMENT_LOG(D);
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);

    if (devicePort == nullptr || mixPort == nullptr || devicePort->type != AUDIO_PORT_TYPE_DEVICE ||
        mixPort->type != AUDIO_PORT_TYPE_MIX) {
        AUGMENT_LOG(E, "invalid device or mix port");
        return BAD_VALUE;
    }
    const int32_t aidlHandle = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_io_handle_t_int32_t(mixPort->ext.mix.handle));
    AudioPort port;
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(mMapper.getAudioMixPort(aidlHandle, &port));
    }
    const bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(
            mixPort->role, mixPort->type)) == ::aidl::android::AudioPortDirection::INPUT;
    *mixPort = VALUE_OR_RETURN_STATUS(::aidl::android::aidl2legacy_AudioPort_audio_port_v7(
            port, isInput));
    return OK;
}

status_t DeviceHalAidl::setAudioPortConfig(const struct audio_port_config* config) {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (config == nullptr) {
        AUGMENT_LOG(E, "config not initialized");
        return BAD_VALUE;
    }
    bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(
                    config->role, config->type)) == ::aidl::android::AudioPortDirection::INPUT;
    AudioPortConfig requestedPortConfig = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_port_config_AudioPortConfig(
                    *config, isInput, 0 /*portId*/));
    AudioPortConfig portConfig;
    std::lock_guard l(mLock);
    return mMapper.setPortConfig(requestedPortConfig, std::set<int32_t>(), &portConfig);
}

MicrophoneInfoProvider::Info const* DeviceHalAidl::getMicrophoneInfo() {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT({});
    std::lock_guard l(mLock);
    if (mMicrophones.status == Microphones::Status::UNKNOWN) {
        TIME_CHECK();
        std::vector<MicrophoneInfo> aidlInfo;
        status_t status = statusTFromBinderStatus(mModule->getMicrophones(&aidlInfo));
        if (status == OK) {
            mMicrophones.status = Microphones::Status::QUERIED;
            mMicrophones.info = std::move(aidlInfo);
        } else if (status == INVALID_OPERATION) {
            mMicrophones.status = Microphones::Status::NOT_SUPPORTED;
        } else {
            AUGMENT_LOG(E, "Unexpected status from HAL: %d", status);
            return {};
        }
    }
    if (mMicrophones.status == Microphones::Status::QUERIED) {
        return &mMicrophones.info;
    }
    return {};  // NOT_SUPPORTED
}

status_t DeviceHalAidl::getMicrophones(
        std::vector<audio_microphone_characteristic_t>* microphones) {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (microphones == nullptr) {
        AUGMENT_LOG(E, "microphones not initialized");
        return BAD_VALUE;
    }
    auto staticInfo = getMicrophoneInfo();
    if (!staticInfo) return INVALID_OPERATION;
    std::vector<MicrophoneDynamicInfo> emptyDynamicInfo;
    emptyDynamicInfo.reserve(staticInfo->size());
    std::transform(staticInfo->begin(), staticInfo->end(), std::back_inserter(emptyDynamicInfo),
            [](const auto& info) { return MicrophoneDynamicInfo{ .id = info.id }; });
    *microphones = VALUE_OR_RETURN_STATUS(
            ::aidl::android::convertContainers<std::vector<audio_microphone_characteristic_t>>(
                    *staticInfo, emptyDynamicInfo,
                    ::aidl::android::aidl2legacy_MicrophoneInfos_audio_microphone_characteristic_t)
    );
    return OK;
}

status_t DeviceHalAidl::addDeviceEffect(
        const struct audio_port_config *device, sp<EffectHalInterface> effect) {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (device == nullptr || effect == nullptr) {
        AUGMENT_LOG(E, "device or effect not initialized");
        return BAD_VALUE;
    }
    bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(
                    device->role, device->type)) == ::aidl::android::AudioPortDirection::INPUT;
    auto requestedPortConfig = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_port_config_AudioPortConfig(
                    *device, isInput, 0));
    if (requestedPortConfig.ext.getTag() != AudioPortExt::Tag::device) {
        AUGMENT_LOG(E, "provided port config is not a device port config: %s",
                    requestedPortConfig.toString().c_str());
        return BAD_VALUE;
    }
    AudioPortConfig devicePortConfig;
    Hal2AidlMapper::Cleanups cleanups(mMapperAccessor);
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(mMapper.setPortConfig(
                    requestedPortConfig, {} /*destinationPortIds*/, &devicePortConfig, &cleanups));
    }
    auto aidlEffect = sp<effect::EffectHalAidl>::cast(effect);
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(
                mModule->addDeviceEffect(devicePortConfig.id, aidlEffect->getIEffect())));
    }
    cleanups.disarmAll();
    return OK;
}
status_t DeviceHalAidl::removeDeviceEffect(
        const struct audio_port_config *device, sp<EffectHalInterface> effect) {
    AUGMENT_LOG(D);
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (device == nullptr || effect == nullptr) {
        AUGMENT_LOG(E, "device or effect not initialized");
        return BAD_VALUE;
    }
    bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(
                    device->role, device->type)) == ::aidl::android::AudioPortDirection::INPUT;
    auto requestedPortConfig = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_port_config_AudioPortConfig(
                    *device, isInput, 0));
    if (requestedPortConfig.ext.getTag() != AudioPortExt::Tag::device) {
        AUGMENT_LOG(E, "provided port config is not a device port config: %s",
                    requestedPortConfig.toString().c_str());
        return BAD_VALUE;
    }
    AudioPortConfig devicePortConfig;
    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(mMapper.findPortConfig(
                        requestedPortConfig.ext.get<AudioPortExt::Tag::device>().device,
                        &devicePortConfig));
    }
    auto aidlEffect = sp<effect::EffectHalAidl>::cast(effect);
    std::lock_guard l(mLock);
    return statusTFromBinderStatus(mModule->removeDeviceEffect(
                    devicePortConfig.id, aidlEffect->getIEffect()));
}

status_t DeviceHalAidl::getMmapPolicyInfos(
        media::audio::common::AudioMMapPolicyType policyType,
        std::vector<media::audio::common::AudioMMapPolicyInfo>* policyInfos) {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);

    AudioMMapPolicyType mmapPolicyType =
            VALUE_OR_RETURN_STATUS(cpp2ndk_AudioMMapPolicyType(policyType));

    std::vector<AudioMMapPolicyInfo> mmapPolicyInfos;

    {
        std::lock_guard l(mLock);
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(
                        mModule->getMmapPolicyInfos(mmapPolicyType, &mmapPolicyInfos)));
    }

    *policyInfos = VALUE_OR_RETURN_STATUS(
            convertContainer<std::vector<media::audio::common::AudioMMapPolicyInfo>>(
                mmapPolicyInfos, ndk2cpp_AudioMMapPolicyInfo));
    return OK;
}

int32_t DeviceHalAidl::getAAudioMixerBurstCount() {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    int32_t mixerBurstCount = 0;
    std::lock_guard l(mLock);
    return mModule->getAAudioMixerBurstCount(&mixerBurstCount).isOk() ? mixerBurstCount : 0;
}

int32_t DeviceHalAidl::getAAudioHardwareBurstMinUsec() {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    int32_t hardwareBurstMinUsec = 0;
    std::lock_guard l(mLock);
    return mModule->getAAudioHardwareBurstMinUsec(&hardwareBurstMinUsec).isOk() ?
            hardwareBurstMinUsec : 0;
}

error::Result<audio_hw_sync_t> DeviceHalAidl::getHwAvSync() {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    int32_t aidlHwAvSync;
    std::lock_guard l(mLock);
    RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->generateHwAvSyncId(&aidlHwAvSync)));
    return VALUE_OR_RETURN_STATUS(
            ::aidl::android::aidl2legacy_int32_t_audio_hw_sync_t(aidlHwAvSync));
}

status_t DeviceHalAidl::dump(int fd, const Vector<String16>& args) {
    TIME_CHECK();
    if (!isModuleInitialized()) return NO_INIT;
    Vector<String16> newArgs = args;
    newArgs.push(String16(kDumpFromAudioServerArgument));
    std::lock_guard l(mLock);
    return mModule->dump(fd, Args(newArgs).args(), newArgs.size());
}

status_t DeviceHalAidl::supportsBluetoothVariableLatency(bool* supports) {
    AUGMENT_LOG(D);

    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (supports == nullptr) {
        return BAD_VALUE;
    }
    std::lock_guard l(mLock);
    return statusTFromBinderStatus(mModule->supportsVariableLatency(supports));
}

status_t DeviceHalAidl::getSoundDoseInterface([[maybe_unused]] const std::string& module,
                                              ::ndk::SpAIBinder* soundDoseBinder) {
    AUGMENT_LOG(V);
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);

    if (soundDoseBinder == nullptr) {
        return BAD_VALUE;
    }
    if (mSoundDose == nullptr) {
        AUGMENT_LOG(E, "failed to retrieve the sound dose interface");
        return NO_INIT;
    }

    *soundDoseBinder = mSoundDose->asBinder();
    if (soundDoseBinder == nullptr) {
        AUGMENT_LOG(E, "failed to return the sound dose interface not implemented");
        return NO_INIT;
    }

    AUGMENT_LOG(I, "using audio AIDL HAL sound dose interface");
    return OK;
}

status_t DeviceHalAidl::prepareToDisconnectExternalDevice(const struct audio_port_v7* port) {
    AUGMENT_LOG(V);
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (port == nullptr) {
        AUGMENT_LOG(E, "port not initialized");
        return BAD_VALUE;
    }
    const bool isInput =
            VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(port->role, port->type)) ==
            ::aidl::android::AudioPortDirection::INPUT;
    AudioPort aidlPort = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_port_v7_AudioPort(*port, isInput));
    if (aidlPort.ext.getTag() != AudioPortExt::device) {
        AUGMENT_LOG(E, "provided port is not a device port: %s", aidlPort.toString().c_str());
        return BAD_VALUE;
    }

    AUGMENT_LOG(D, "device %s", aidlPort.toString().c_str());

    status_t status = NO_ERROR;
    {
        std::lock_guard l(mLock);
        status = mMapper.prepareToDisconnectExternalDevice(aidlPort);
    }
    if (status == UNKNOWN_TRANSACTION) {
        // If there is not AIDL API defined for `prepareToDisconnectExternalDevice`.
        // Call `setConnectedState` instead.
        RETURN_STATUS_IF_ERROR(setConnectedState(port, false /*connected*/));
        std::lock_guard l(mLock);
        mDeviceDisconnectionNotified.insert(port->id);
        // Return that there was no error as otherwise the disconnection procedure will not be
        // considered complete for upper layers, and 'setConnectedState' will not be called again
        return OK;
    } else {
        return status;
    }
}

status_t DeviceHalAidl::setConnectedState(const struct audio_port_v7 *port, bool connected) {
    AUGMENT_LOG(V);
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    if (port == nullptr) {
        AUGMENT_LOG(E, "port not initialized");
        return BAD_VALUE;
    }
    if (!connected) {
        std::lock_guard l(mLock);
        if (mDeviceDisconnectionNotified.erase(port->id) > 0) {
            // For device disconnection, APM will first call `prepareToDisconnectExternalDevice`
            // and then call `setConnectedState`. If `prepareToDisconnectExternalDevice` doesn't
            // exit, `setConnectedState` will be called when calling
            // `prepareToDisconnectExternalDevice`. Do not call to the HAL if previous call is
            // successful. Also remove the cache here to avoid a large cache after a long run.
            return OK;
        }
    }
    bool isInput = VALUE_OR_RETURN_STATUS(::aidl::android::portDirection(port->role, port->type)) ==
            ::aidl::android::AudioPortDirection::INPUT;
    AudioPort aidlPort = VALUE_OR_RETURN_STATUS(
            ::aidl::android::legacy2aidl_audio_port_v7_AudioPort(*port, isInput));
    if (aidlPort.ext.getTag() != AudioPortExt::device) {
        AUGMENT_LOG(E, "provided port is not a device port: %s", aidlPort.toString().c_str());
        return BAD_VALUE;
    }
    AUGMENT_LOG(D, "connected %d port: %s", connected, aidlPort.toString().c_str());
    std::lock_guard l(mLock);
    return mMapper.setDevicePortConnectedState(aidlPort, connected);
}

status_t DeviceHalAidl::setSimulateDeviceConnections(bool enabled) {
    AUGMENT_LOG(V);
    TIME_CHECK();
    RETURN_IF_MODULE_NOT_INIT(NO_INIT);
    std::lock_guard l(mLock);
    mMapper.resetUnusedPatchesAndPortConfigs();
    ModuleDebug debug{ .simulateDeviceConnections = enabled };
    status_t status = statusTFromBinderStatus(mModule->setModuleDebug(debug));
    // This is important to log as it affects HAL behavior.
    if (status == OK) {
        AUGMENT_LOG(I, "set enabled: %d", enabled);
    } else {
        AUGMENT_LOG(W, "set enabled to %d failed: %d", enabled, status);
    }
    return status;
}

status_t DeviceHalAidl::filterAndRetrieveBtA2dpParameters(
        AudioParameter &keys, AudioParameter *result) {
    if (String8 key = String8(AudioParameter::keyReconfigA2dpSupported); keys.containsKey(key)) {
        keys.remove(key);
        std::lock_guard l(mLock);
        if (mBluetoothA2dp != nullptr) {
            bool supports;
            RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(
                            mBluetoothA2dp->supportsOffloadReconfiguration(&supports)));
            result->addInt(key, supports ? 1 : 0);
        } else {
            AUGMENT_LOG(I, "no IBluetoothA2dp");
            result->addInt(key, 0);
        }
    }
    return OK;
}

status_t DeviceHalAidl::filterAndRetrieveBtLeParameters(
        AudioParameter &keys, AudioParameter *result) {
    if (String8 key = String8(AudioParameter::keyReconfigLeSupported); keys.containsKey(key)) {
        keys.remove(key);
        std::lock_guard l(mLock);
        if (mBluetoothLe != nullptr) {
            bool supports;
            RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(
                            mBluetoothLe->supportsOffloadReconfiguration(&supports)));
            result->addInt(key, supports ? 1 : 0);
        } else {
            AUGMENT_LOG(I, "no mBluetoothLe");
            result->addInt(key, 0);
        }
    }
    return OK;
}

status_t DeviceHalAidl::filterAndUpdateBtA2dpParameters(AudioParameter &parameters) {
    std::optional<bool> a2dpEnabled;
    std::optional<std::vector<VendorParameter>> reconfigureOffload;
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyBtA2dpSuspended),
            [&a2dpEnabled, this](const String8& trueOrFalse) {
                if (trueOrFalse == AudioParameter::valueTrue) {
                    a2dpEnabled = false;  // 'suspended' == true
                    return OK;
                } else if (trueOrFalse == AudioParameter::valueFalse) {
                    a2dpEnabled = true;  // 'suspended' == false
                    return OK;
                }
                AUGMENT_LOG(E, "setParameters: parameter key \"%s\" has invalid value \"%s\"",
                            AudioParameter::keyBtA2dpSuspended, trueOrFalse.c_str());
                return BAD_VALUE;
            }));
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyReconfigA2dp),
            [&](const String8& value) -> status_t {
                std::vector<VendorParameter> result;
                RETURN_STATUS_IF_ERROR(
                        statusTFromBinderStatus(mVendorExt->parseBluetoothA2dpReconfigureOffload(
                                std::string(value.c_str()), &result)));
                reconfigureOffload = std::move(result);
                return OK;
            }));
    std::lock_guard l(mLock);
    if (mBluetoothA2dp != nullptr && a2dpEnabled.has_value()) {
        return statusTFromBinderStatus(mBluetoothA2dp->setEnabled(a2dpEnabled.value()));
    }
    if (mBluetoothA2dp != nullptr && reconfigureOffload.has_value()) {
        return statusTFromBinderStatus(mBluetoothA2dp->reconfigureOffload(
                        reconfigureOffload.value()));
    }
    return OK;
}

status_t DeviceHalAidl::filterAndUpdateBtHfpParameters(AudioParameter &parameters) {
    IBluetooth::HfpConfig hfpConfig;
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyBtHfpEnable),
            [&hfpConfig, this](const String8& trueOrFalse) {
                if (trueOrFalse == AudioParameter::valueTrue) {
                    hfpConfig.isEnabled = Boolean{.value = true};
                    return OK;
                } else if (trueOrFalse == AudioParameter::valueFalse) {
                    hfpConfig.isEnabled = Boolean{.value = false};
                    return OK;
                }
                AUGMENT_LOG(E, "setParameters: parameter key \"%s\" has invalid value \"%s\"",
                            AudioParameter::keyBtHfpEnable, trueOrFalse.c_str());
                return BAD_VALUE;
            }));
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<int>(
            parameters, String8(AudioParameter::keyBtHfpSamplingRate),
            [&hfpConfig](int sampleRate) {
                return sampleRate > 0 ? hfpConfig.sampleRate = Int{.value = sampleRate},
                                        OK : BAD_VALUE;
            }));
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<int>(
            parameters, String8(AudioParameter::keyBtHfpVolume), [&hfpConfig](int volume0to15) {
                if (volume0to15 >= 0 && volume0to15 <= 15) {
                    hfpConfig.volume = Float{.value = volume0to15 / 15.0f};
                    return OK;
                }
                return BAD_VALUE;
            }));
    std::lock_guard l(mLock);
    if (mBluetooth != nullptr && hfpConfig != IBluetooth::HfpConfig{}) {
        IBluetooth::HfpConfig newHfpConfig;
        return statusTFromBinderStatus(mBluetooth->setHfpConfig(hfpConfig, &newHfpConfig));
    }
    return OK;
}

status_t DeviceHalAidl::filterAndUpdateBtLeParameters(AudioParameter &parameters) {
    std::optional<bool> leEnabled;
    std::optional<std::vector<VendorParameter>> reconfigureOffload;
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyBtLeSuspended),
            [&leEnabled, this](const String8& trueOrFalse) {
                if (trueOrFalse == AudioParameter::valueTrue) {
                    leEnabled = false;  // 'suspended' == true
                    return OK;
                } else if (trueOrFalse == AudioParameter::valueFalse) {
                    leEnabled = true;  // 'suspended' == false
                    return OK;
                }
                AUGMENT_LOG(E, "setParameters: parameter key \"%s\" has invalid value \"%s\"",
                            AudioParameter::keyBtLeSuspended, trueOrFalse.c_str());
                return BAD_VALUE;
            }));
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyReconfigLe),
            [&](const String8& value) -> status_t {
                if (mVendorExt != nullptr) {
                    std::vector<VendorParameter> result;
                    RETURN_STATUS_IF_ERROR(
                            statusTFromBinderStatus(mVendorExt->parseBluetoothLeReconfigureOffload(
                                    std::string(value.c_str()), &result)));
                    reconfigureOffload = std::move(result);
                } else {
                    reconfigureOffload = std::vector<VendorParameter>();
                }
                return OK;
            }));
    std::lock_guard l(mLock);
    if (mBluetoothLe != nullptr && leEnabled.has_value()) {
        return statusTFromBinderStatus(mBluetoothLe->setEnabled(leEnabled.value()));
    }
    if (mBluetoothLe != nullptr && reconfigureOffload.has_value()) {
        return statusTFromBinderStatus(
                mBluetoothLe->reconfigureOffload(reconfigureOffload.value()));
    }
    return OK;
}

status_t DeviceHalAidl::filterAndUpdateBtScoParameters(AudioParameter &parameters) {
    IBluetooth::ScoConfig scoConfig;
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyBtSco),
            [&scoConfig, this](const String8& onOrOff) {
                if (onOrOff == AudioParameter::valueOn) {
                    scoConfig.isEnabled = Boolean{.value = true};
                    return OK;
                } else if (onOrOff == AudioParameter::valueOff) {
                    scoConfig.isEnabled = Boolean{.value = false};
                    return OK;
                }
                AUGMENT_LOG(E, "setParameters: parameter key \"%s\" has invalid value \"%s\"",
                            AudioParameter::keyBtSco, onOrOff.c_str());
                return BAD_VALUE;
            }));
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyBtScoHeadsetName),
            [&scoConfig](const String8& name) {
                scoConfig.debugName = name;
                return OK;
            }));
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyBtNrec),
            [&scoConfig, this](const String8& onOrOff) {
                if (onOrOff == AudioParameter::valueOn) {
                    scoConfig.isNrecEnabled = Boolean{.value = true};
                    return OK;
                } else if (onOrOff == AudioParameter::valueOff) {
                    scoConfig.isNrecEnabled = Boolean{.value = false};
                    return OK;
                }
                AUGMENT_LOG(E, "setParameters: parameter key \"%s\" has invalid value \"%s\"",
                            AudioParameter::keyBtNrec, onOrOff.c_str());
                return BAD_VALUE;
            }));
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyBtScoWb),
            [&scoConfig, this](const String8& onOrOff) {
                if (onOrOff == AudioParameter::valueOn) {
                    scoConfig.mode = IBluetooth::ScoConfig::Mode::SCO_WB;
                    return OK;
                } else if (onOrOff == AudioParameter::valueOff) {
                    scoConfig.mode = IBluetooth::ScoConfig::Mode::SCO;
                    return OK;
                }
                AUGMENT_LOG(E, "setParameters: parameter key \"%s\" has invalid value \"%s\"",
                            AudioParameter::keyBtScoWb, onOrOff.c_str());
                return BAD_VALUE;
            }));
    std::lock_guard l(mLock);
    if (mBluetooth != nullptr && scoConfig != IBluetooth::ScoConfig{}) {
        IBluetooth::ScoConfig newScoConfig;
        return statusTFromBinderStatus(mBluetooth->setScoConfig(scoConfig, &newScoConfig));
    }
    return OK;
}

status_t DeviceHalAidl::filterAndUpdateScreenParameters(AudioParameter &parameters) {
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyScreenState),
            [&, this](const String8& onOrOff) -> status_t {
                std::optional<bool> isTurnedOn;
                if (onOrOff == AudioParameter::valueOn) {
                    isTurnedOn = true;
                } else if (onOrOff == AudioParameter::valueOff) {
                    isTurnedOn = false;
                }
                if (!isTurnedOn.has_value()) {
                    AUGMENT_LOG(E, "setParameters: parameter key \"%s\" has invalid value \"%s\"",
                                AudioParameter::keyScreenState, onOrOff.c_str());
                    return BAD_VALUE;
                }
                std::lock_guard l(mLock);
                return statusTFromBinderStatus(mModule->updateScreenState(isTurnedOn.value()));
            }));
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<int>(
            parameters, String8(AudioParameter::keyScreenRotation),
            [&, this](int rotationDegrees) -> status_t {
                IModule::ScreenRotation rotation;
                switch (rotationDegrees) {
                    case 0:
                        rotation = IModule::ScreenRotation::DEG_0;
                        break;
                    case 90:
                        rotation = IModule::ScreenRotation::DEG_90;
                        break;
                    case 180:
                        rotation = IModule::ScreenRotation::DEG_180;
                        break;
                    case 270:
                        rotation = IModule::ScreenRotation::DEG_270;
                        break;
                    default:
                        AUGMENT_LOG(E, "setParameters: parameter key \"%s\" has invalid value %d",
                                    AudioParameter::keyScreenRotation, rotationDegrees);
                        return BAD_VALUE;
                }
                std::lock_guard l(mLock);
                return statusTFromBinderStatus(mModule->updateScreenRotation(rotation));
            }));
    return OK;
}

status_t DeviceHalAidl::filterAndUpdateTelephonyParameters(AudioParameter &parameters) {
    using TtyMode = ITelephony::TelecomConfig::TtyMode;
    ITelephony::TelecomConfig telConfig;
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyTtyMode),
            [&telConfig, this](const String8& mode) {
                if (mode == AudioParameter::valueTtyModeOff) {
                    telConfig.ttyMode = TtyMode::OFF;
                    return OK;
                } else if (mode == AudioParameter::valueTtyModeFull) {
                    telConfig.ttyMode = TtyMode::FULL;
                    return OK;
                } else if (mode == AudioParameter::valueTtyModeHco) {
                    telConfig.ttyMode = TtyMode::HCO;
                    return OK;
                } else if (mode == AudioParameter::valueTtyModeVco) {
                    telConfig.ttyMode = TtyMode::VCO;
                    return OK;
                }
                AUGMENT_LOG(E, "setParameters: parameter key \"%s\" has invalid value \"%s\"",
                            AudioParameter::keyTtyMode, mode.c_str());
                return BAD_VALUE;
            }));
    (void)VALUE_OR_RETURN_STATUS(filterOutAndProcessParameter<String8>(
            parameters, String8(AudioParameter::keyHacSetting),
            [&telConfig, this](const String8& onOrOff) {
                if (onOrOff == AudioParameter::valueHacOn) {
                    telConfig.isHacEnabled = Boolean{.value = true};
                    return OK;
                } else if (onOrOff == AudioParameter::valueHacOff) {
                    telConfig.isHacEnabled = Boolean{.value = false};
                    return OK;
                }
                AUGMENT_LOG(E, "setParameters: parameter key \"%s\" has invalid value \"%s\"",
                            AudioParameter::keyHacSetting, onOrOff.c_str());
                return BAD_VALUE;
            }));
    std::lock_guard l(mLock);
    if (mTelephony != nullptr && telConfig != ITelephony::TelecomConfig{}) {
        ITelephony::TelecomConfig newTelConfig;
        return statusTFromBinderStatus(mTelephony->setTelecomConfig(telConfig, &newTelConfig));
    }
    return OK;
}

void DeviceHalAidl::clearCallbacks(void* cookie) {
    std::lock_guard l(mCallbacksLock);
    mCallbacks.erase(cookie);
}

sp<StreamOutHalInterfaceCallback> DeviceHalAidl::getStreamOutCallback(void* cookie) {
    return getCallbackImpl(cookie, &Callbacks::out);
}

void DeviceHalAidl::setStreamOutCallback(
        void* cookie, const sp<StreamOutHalInterfaceCallback>& cb) {
    setCallbackImpl(cookie, &Callbacks::out, cb);
}

sp<StreamOutHalInterfaceEventCallback> DeviceHalAidl::getStreamOutEventCallback(
        void* cookie) {
    return getCallbackImpl(cookie, &Callbacks::event);
}

void DeviceHalAidl::setStreamOutEventCallback(
        void* cookie, const sp<StreamOutHalInterfaceEventCallback>& cb) {
    setCallbackImpl(cookie, &Callbacks::event, cb);
}

sp<StreamOutHalInterfaceLatencyModeCallback> DeviceHalAidl::getStreamOutLatencyModeCallback(
        void* cookie) {
    return getCallbackImpl(cookie, &Callbacks::latency);
}

void DeviceHalAidl::setStreamOutLatencyModeCallback(
        void* cookie, const sp<StreamOutHalInterfaceLatencyModeCallback>& cb) {
    setCallbackImpl(cookie, &Callbacks::latency, cb);
}

template <class C>
sp<C> DeviceHalAidl::getCallbackImpl(void* cookie, wp<C> DeviceHalAidl::Callbacks::* field) {
    wp<C> result;
    {
        std::lock_guard l(mCallbacksLock);
        if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
            result = (it->second).*field;
        }
    }
    return result.promote();
}
template<class C>
void DeviceHalAidl::setCallbackImpl(
        void* cookie, wp<C> DeviceHalAidl::Callbacks::* field, const sp<C>& cb) {
    std::lock_guard l(mCallbacksLock);
    if (auto it = mCallbacks.find(cookie); it != mCallbacks.end()) {
        (it->second).*field = cb;
    }
}

} // namespace android
