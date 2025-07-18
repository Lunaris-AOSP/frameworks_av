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

#define LOG_TAG "Hal2AidlMapper"
// #define LOG_NDEBUG 0

#include <algorithm>

#include <media/audiohal/StreamHalInterface.h>
#include <error/expected_utils.h>
#include <system/audio.h>  // For AUDIO_REMOTE_SUBMIX_DEVICE_ADDRESS
#include <Utils.h>
#include <utils/Log.h>

#include "AidlUtils.h"
#include "Hal2AidlMapper.h"

using aidl::android::aidl_utils::statusTFromBinderStatus;
using aidl::android::media::audio::common::AudioChannelLayout;
using aidl::android::media::audio::common::AudioConfig;
using aidl::android::media::audio::common::AudioConfigBase;
using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioDeviceAddress;
using aidl::android::media::audio::common::AudioDeviceDescription;
using aidl::android::media::audio::common::AudioDeviceType;
using aidl::android::media::audio::common::AudioFormatDescription;
using aidl::android::media::audio::common::AudioFormatType;
using aidl::android::media::audio::common::AudioGainConfig;
using aidl::android::media::audio::common::AudioInputFlags;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioOutputFlags;
using aidl::android::media::audio::common::AudioPort;
using aidl::android::media::audio::common::AudioPortConfig;
using aidl::android::media::audio::common::AudioPortDeviceExt;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::AudioPortMixExt;
using aidl::android::media::audio::common::AudioPortMixExtUseCase;
using aidl::android::media::audio::common::AudioProfile;
using aidl::android::media::audio::common::AudioSource;
using aidl::android::media::audio::common::Int;
using aidl::android::hardware::audio::common::isBitPositionFlagSet;
using aidl::android::hardware::audio::common::isDefaultAudioFormat;
using aidl::android::hardware::audio::common::makeBitPositionFlagMask;
using aidl::android::hardware::audio::core::AudioPatch;
using aidl::android::hardware::audio::core::AudioRoute;
using aidl::android::hardware::audio::core::IModule;

namespace android {

namespace {

bool isConfigEqualToPortConfig(const AudioConfig& config, const AudioPortConfig& portConfig) {
    return portConfig.sampleRate.value().value == config.base.sampleRate &&
            portConfig.channelMask.value() == config.base.channelMask &&
            portConfig.format.value() == config.base.format;
}

AudioConfig* setConfigFromPortConfig(AudioConfig* config, const AudioPortConfig& portConfig) {
    config->base.sampleRate = portConfig.sampleRate.value().value;
    config->base.channelMask = portConfig.channelMask.value();
    config->base.format = portConfig.format.value();
    return config;
}

void setPortConfigFromConfig(AudioPortConfig* portConfig, const AudioConfig& config) {
    if (config.base.sampleRate != 0) {
        portConfig->sampleRate = Int{ .value = config.base.sampleRate };
    }
    if (config.base.channelMask != AudioChannelLayout{}) {
        portConfig->channelMask = config.base.channelMask;
    }
    if (config.base.format != AudioFormatDescription{}) {
        portConfig->format = config.base.format;
    }
}

bool containHapticChannel(AudioChannelLayout channel) {
    return channel.getTag() == AudioChannelLayout::Tag::layoutMask &&
            ((channel.get<AudioChannelLayout::Tag::layoutMask>()
                    & AudioChannelLayout::CHANNEL_HAPTIC_A)
                    == AudioChannelLayout::CHANNEL_HAPTIC_A ||
             (channel.get<AudioChannelLayout::Tag::layoutMask>()
                    & AudioChannelLayout::CHANNEL_HAPTIC_B)
                    == AudioChannelLayout::CHANNEL_HAPTIC_B);
}

}  // namespace

Hal2AidlMapper::Hal2AidlMapper(const std::string& instance, const std::shared_ptr<IModule>& module)
    : ConversionHelperAidl("Hal2AidlMapper", instance), mModule(module) {}

void Hal2AidlMapper::addStream(
        const sp<StreamHalInterface>& stream, int32_t mixPortConfigId, int32_t patchId) {
    mStreams.insert(std::pair(stream, std::pair(mixPortConfigId, patchId)));
}

bool Hal2AidlMapper::audioDeviceMatches(const AudioDevice& device, const AudioPort& p) {
    if (p.ext.getTag() != AudioPortExt::Tag::device) return false;
    return p.ext.get<AudioPortExt::Tag::device>().device == device;
}

bool Hal2AidlMapper::audioDeviceMatches(const AudioDevice& device, const AudioPortConfig& p) {
    if (p.ext.getTag() != AudioPortExt::Tag::device) return false;
    if (device.type.type == AudioDeviceType::IN_DEFAULT) {
        return p.portId == mDefaultInputPortId;
    } else if (device.type.type == AudioDeviceType::OUT_DEFAULT) {
        return p.portId == mDefaultOutputPortId;
    }
    return p.ext.get<AudioPortExt::Tag::device>().device == device;
}

status_t Hal2AidlMapper::createOrUpdatePatch(
        const std::vector<AudioPortConfig>& sources,
        const std::vector<AudioPortConfig>& sinks,
        int32_t* patchId, Cleanups* cleanups) {
    auto existingPatchIt = *patchId != 0 ? mPatches.find(*patchId): mPatches.end();
    AudioPatch patch;
    if (existingPatchIt != mPatches.end()) {
        patch = existingPatchIt->second;
        patch.sourcePortConfigIds.clear();
        patch.sinkPortConfigIds.clear();
    }
    // The IDs will be found by 'fillPortConfigs', however the original 'sources' and
    // 'sinks' will not be updated because 'setAudioPatch' only needs IDs. Here we log
    // the source arguments, where only the audio configuration and device specifications
    // are relevant.
    AUGMENT_LOG(D, "patch ID: %d, [disregard IDs] sources: %s, sinks: %s", *patchId,
                ::android::internal::ToString(sources).c_str(),
                ::android::internal::ToString(sinks).c_str());
    auto fillPortConfigs = [&](
            const std::vector<AudioPortConfig>& configs,
            const std::set<int32_t>& destinationPortIds,
            std::vector<int32_t>* ids, std::set<int32_t>* portIds) -> status_t {
        for (const auto& s : configs) {
            AudioPortConfig portConfig;
            if (status_t status = setPortConfig(
                            s, destinationPortIds, &portConfig, cleanups); status != OK) {
                if (s.ext.getTag() == AudioPortExt::mix) {
                    // See b/315528763. Despite that the framework knows the actual format of
                    // the mix port, it still uses the original format. Luckily, there is
                    // the I/O handle which can be used to find the mix port.
                    AUGMENT_LOG(I,
                                "fillPortConfigs: retrying to find a mix port config with"
                                " default configuration");
                    if (auto it = findPortConfig(std::nullopt, s.flags,
                                    s.ext.get<AudioPortExt::mix>().handle);
                            it != mPortConfigs.end()) {
                        portConfig = it->second;
                    } else {
                        const std::string flags =
                                s.flags.has_value() ? s.flags->toString() : "<unspecified>";
                        AUGMENT_LOG(E,
                                    "fillPortConfigs: existing port config for flags %s, "
                                    " handle %d not found",
                                    flags.c_str(), s.ext.get<AudioPortExt::mix>().handle);
                        return BAD_VALUE;
                    }
                } else {
                    return status;
                }
            }
            LOG_ALWAYS_FATAL_IF(portConfig.id == 0,
                                "fillPortConfigs: initial config: %s, port config: %s",
                                s.toString().c_str(), portConfig.toString().c_str());
            ids->push_back(portConfig.id);
            if (portIds != nullptr) {
                portIds->insert(portConfig.portId);
            }
        }
        return OK;
    };
    // When looking up port configs, the destinationPortId is only used for mix ports.
    // Thus, we process device port configs first, and look up the destination port ID from them.
    const bool sourceIsDevice = std::any_of(sources.begin(), sources.end(),
            [](const auto& config) { return config.ext.getTag() == AudioPortExt::device; });
    const bool sinkIsDevice = std::any_of(sinks.begin(), sinks.end(),
            [](const auto& config) { return config.ext.getTag() == AudioPortExt::device; });
    const std::vector<AudioPortConfig>& devicePortConfigs =
            sourceIsDevice ? sources : sinks;
    std::vector<int32_t>* devicePortConfigIds =
            sourceIsDevice ? &patch.sourcePortConfigIds : &patch.sinkPortConfigIds;
    const std::vector<AudioPortConfig>& mixPortConfigs =
            sourceIsDevice ? sinks : sources;
    std::vector<int32_t>* mixPortConfigIds =
            sourceIsDevice ? &patch.sinkPortConfigIds : &patch.sourcePortConfigIds;
    std::set<int32_t> devicePortIds;
    RETURN_STATUS_IF_ERROR(fillPortConfigs(
                    devicePortConfigs, std::set<int32_t>(), devicePortConfigIds, &devicePortIds));
    RETURN_STATUS_IF_ERROR(fillPortConfigs(
                    mixPortConfigs, devicePortIds, mixPortConfigIds, nullptr));
    if (existingPatchIt != mPatches.end()) {
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(
                        mModule->setAudioPatch(patch, &patch)));
        existingPatchIt->second = patch;
    } else {
        bool created = false;
        // When the framework does not specify a patch ID, only the mix port config
        // is used for finding an existing patch. That's because the framework assumes
        // that there can only be one patch for an I/O thread.
        PatchMatch match = sourceIsDevice && sinkIsDevice ?
                MATCH_BOTH : (sourceIsDevice ? MATCH_SINKS : MATCH_SOURCES);
        auto requestedPatch = patch;
        RETURN_STATUS_IF_ERROR(findOrCreatePatch(patch, match,
                                                 &patch, &created));
        // No cleanup of the patch is needed, it is managed by the framework.
        *patchId = patch.id;
        if (!created) {
            requestedPatch.id = patch.id;
            if (patch != requestedPatch) {
                AUGMENT_LOG(I, "Updating transient patch. Current: %s, new: %s",
                            patch.toString().c_str(), requestedPatch.toString().c_str());
                // Since matching may be done by mix port only, update the patch if the device port
                // config has changed.
                patch = requestedPatch;
                RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(
                                mModule->setAudioPatch(patch, &patch)));
                existingPatchIt = mPatches.find(patch.id);
                existingPatchIt->second = patch;
            }
            // The framework might have "created" a patch which already existed due to
            // stream creation. Need to release the ownership from the stream.
            for (auto& s : mStreams) {
                if (s.second.second == patch.id) s.second.second = -1;
            }
        }
    }
    return OK;
}

status_t Hal2AidlMapper::createOrUpdatePortConfig(
        const AudioPortConfig& requestedPortConfig, AudioPortConfig* result, bool* created) {
    bool applied = false;
    RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->setAudioPortConfig(
                            requestedPortConfig, result, &applied)));
    if (!applied) {
        result->id = 0;
        *created = false;
        return OK;
    }

    int32_t id = result->id;
    if (requestedPortConfig.id != 0 && requestedPortConfig.id != id) {
        LOG_ALWAYS_FATAL("%s: requested port config id %d changed to %d", __func__,
                         requestedPortConfig.id, id);
    }

    auto [_, inserted] = mPortConfigs.insert_or_assign(id, *result);
    *created = inserted;
    return OK;
}

status_t Hal2AidlMapper::createOrUpdatePortConfigRetry(
        const AudioPortConfig& requestedPortConfig, AudioPortConfig* result, bool* created) {
    AudioPortConfig suggestedOrAppliedPortConfig;
    RETURN_STATUS_IF_ERROR(createOrUpdatePortConfig(requestedPortConfig,
                    &suggestedOrAppliedPortConfig, created));
    if (suggestedOrAppliedPortConfig.id == 0) {
        // Try again with the suggested config
        suggestedOrAppliedPortConfig.id = requestedPortConfig.id;
        AudioPortConfig appliedPortConfig;
        RETURN_STATUS_IF_ERROR(createOrUpdatePortConfig(suggestedOrAppliedPortConfig,
                        &appliedPortConfig, created));
        if (appliedPortConfig.id == 0) {
            AUGMENT_LOG(E, "did not apply suggested config %s",
                        suggestedOrAppliedPortConfig.toString().c_str());
            return NO_INIT;
        }
        *result = appliedPortConfig;
    } else {
        *result = suggestedOrAppliedPortConfig;
    }
    return OK;
}

void Hal2AidlMapper::eraseConnectedPort(int32_t portId) {
    mPorts.erase(portId);
    mConnectedPorts.erase(portId);
    if (mDisconnectedPortReplacement.first == portId) {
        const auto& port = mDisconnectedPortReplacement.second;
        mPorts.insert(std::make_pair(port.id, port));
        AUGMENT_LOG(D, "disconnected port replacement: %s", port.toString().c_str());
        mDisconnectedPortReplacement = std::pair<int32_t, AudioPort>();
    }
    updateDynamicMixPorts();
}

status_t Hal2AidlMapper::findOrCreatePatch(
        const AudioPatch& requestedPatch, PatchMatch match, AudioPatch* patch, bool* created) {
    std::set<int32_t> sourcePortConfigIds(requestedPatch.sourcePortConfigIds.begin(),
            requestedPatch.sourcePortConfigIds.end());
    std::set<int32_t> sinkPortConfigIds(requestedPatch.sinkPortConfigIds.begin(),
            requestedPatch.sinkPortConfigIds.end());
    return findOrCreatePatch(sourcePortConfigIds, sinkPortConfigIds, match, patch, created);
}

status_t Hal2AidlMapper::findOrCreatePatch(
        const std::set<int32_t>& sourcePortConfigIds, const std::set<int32_t>& sinkPortConfigIds,
        PatchMatch match, AudioPatch* patch, bool* created) {
    auto patchIt = findPatch(sourcePortConfigIds, sinkPortConfigIds, match);
    if (patchIt == mPatches.end()) {
        AudioPatch requestedPatch, appliedPatch;
        requestedPatch.sourcePortConfigIds.insert(requestedPatch.sourcePortConfigIds.end(),
                sourcePortConfigIds.begin(), sourcePortConfigIds.end());
        requestedPatch.sinkPortConfigIds.insert(requestedPatch.sinkPortConfigIds.end(),
                sinkPortConfigIds.begin(), sinkPortConfigIds.end());
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->setAudioPatch(
                                requestedPatch, &appliedPatch)));
        patchIt = mPatches.insert(mPatches.end(), std::make_pair(appliedPatch.id, appliedPatch));
        *created = true;
    } else {
        *created = false;
    }
    *patch = patchIt->second;
    return OK;
}

status_t Hal2AidlMapper::findOrCreateDevicePortConfig(
        const AudioDevice& device, const AudioConfig* config, const AudioGainConfig* gainConfig,
        AudioPortConfig* portConfig, bool* created) {
    if (auto portConfigIt = findPortConfig(device); portConfigIt == mPortConfigs.end()) {
        auto portsIt = findPort(device);
        if (portsIt == mPorts.end()) {
            AUGMENT_LOG(E, "device port for device %s is not found", device.toString().c_str());
            return BAD_VALUE;
        }
        AudioPortConfig requestedPortConfig;
        requestedPortConfig.portId = portsIt->first;
        if (config != nullptr) {
            setPortConfigFromConfig(&requestedPortConfig, *config);
        }
        if (gainConfig != nullptr) {
            requestedPortConfig.gain = *gainConfig;
        }
        return createOrUpdatePortConfigRetry(requestedPortConfig, portConfig, created);
    } else {
        AudioPortConfig requestedPortConfig = portConfigIt->second;
        if (config != nullptr) {
            setPortConfigFromConfig(&requestedPortConfig, *config);
        }
        if (gainConfig != nullptr) {
            requestedPortConfig.gain = *gainConfig;
        }

        if (requestedPortConfig != portConfigIt->second) {
            return createOrUpdatePortConfigRetry(requestedPortConfig, portConfig, created);
        } else {
            *portConfig = portConfigIt->second;
            *created = false;
        }
    }
    return OK;
}

status_t Hal2AidlMapper::findOrCreateMixPortConfig(
        const AudioConfig& config, const std::optional<AudioIoFlags>& flags, int32_t ioHandle,
        AudioSource source, const std::set<int32_t>& destinationPortIds,
        AudioPortConfig* portConfig, bool* created) {
    if (auto portConfigIt = findPortConfig(config, flags, ioHandle);
            portConfigIt == mPortConfigs.end() && flags.has_value()) {
        // These input flags get removed one by one in this order when retrying port finding.
        std::vector<AudioInputFlags> optionalInputFlags {
            AudioInputFlags::FAST, AudioInputFlags::RAW, AudioInputFlags::VOIP_TX };
        // For remote submix input, retry with direct input flag removed as the remote submix
        // input is not expected to manipulate the contents of the audio stream.
        if (mRemoteSubmixIn.has_value()) {
            optionalInputFlags.push_back(AudioInputFlags::DIRECT);
        }
        auto optionalInputFlagsIt = optionalInputFlags.begin();
        AudioIoFlags matchFlags = flags.value();
        auto portsIt = findPort(config, matchFlags, destinationPortIds);
        while (portsIt == mPorts.end() && matchFlags.getTag() == AudioIoFlags::Tag::input
                && optionalInputFlagsIt != optionalInputFlags.end()) {
            if (!isBitPositionFlagSet(
                            matchFlags.get<AudioIoFlags::Tag::input>(), *optionalInputFlagsIt)) {
                ++optionalInputFlagsIt;
                continue;
            }
            matchFlags.set<AudioIoFlags::Tag::input>(matchFlags.get<AudioIoFlags::Tag::input>() &
                    ~makeBitPositionFlagMask(*optionalInputFlagsIt++));
            portsIt = findPort(config, matchFlags, destinationPortIds);
            AUGMENT_LOG(I,
                        "mix port for config %s, flags %s was not found"
                        "retried with flags %s",
                        config.toString().c_str(), flags.value().toString().c_str(),
                        matchFlags.toString().c_str());
        }
        // These output flags get removed one by one in this order when retrying port finding.
        std::vector<AudioOutputFlags> optionalOutputFlags { };
        // For remote submix output, retry with these output flags removed one by one:
        // 1. DIRECT: remote submix outputs are expected not to manipulate the contents of the
        //            audio stream.
        // 2. IEC958_NONAUDIO: remote submix outputs are not connected to ALSA and do not require
        //                     non audio signalling.
        if (mRemoteSubmixOut.has_value()) {
            optionalOutputFlags.push_back(AudioOutputFlags::DIRECT);
            optionalOutputFlags.push_back(AudioOutputFlags::IEC958_NONAUDIO);
        }
        auto optionalOutputFlagsIt = optionalOutputFlags.begin();
        matchFlags = flags.value();
        while (portsIt == mPorts.end() && matchFlags.getTag() == AudioIoFlags::Tag::output
                && optionalOutputFlagsIt != optionalOutputFlags.end()) {
            if (!isBitPositionFlagSet(
                            matchFlags.get<AudioIoFlags::Tag::output>(),*optionalOutputFlagsIt)) {
                ++optionalOutputFlagsIt;
                continue;
            }
            matchFlags.set<AudioIoFlags::Tag::output>(matchFlags.get<AudioIoFlags::Tag::output>() &
                    ~makeBitPositionFlagMask(*optionalOutputFlagsIt++));
            portsIt = findPort(config, matchFlags, destinationPortIds);
            AUGMENT_LOG(I,
                        "mix port for config %s, flags %s was not found"
                        "retried with flags %s",
                        config.toString().c_str(), flags.value().toString().c_str(),
                        matchFlags.toString().c_str());
        }

        if (portsIt == mPorts.end()) {
            AUGMENT_LOG(E, "mix port for config %s, flags %s is not found",
                        config.toString().c_str(), matchFlags.toString().c_str());
            return BAD_VALUE;
        }
        AudioPortConfig requestedPortConfig;
        requestedPortConfig.portId = portsIt->first;
        setPortConfigFromConfig(&requestedPortConfig, config);
        requestedPortConfig.flags = portsIt->second.flags;
        requestedPortConfig.ext = AudioPortMixExt{ .handle = ioHandle };
        if (matchFlags.getTag() == AudioIoFlags::Tag::input
                && source != AudioSource::SYS_RESERVED_INVALID) {
            requestedPortConfig.ext.get<AudioPortExt::Tag::mix>().usecase =
                    AudioPortMixExtUseCase::make<AudioPortMixExtUseCase::Tag::source>(source);
        }
        return createOrUpdatePortConfig(requestedPortConfig, portConfig, created);
    } else if (portConfigIt == mPortConfigs.end() && !flags.has_value()) {
        AUGMENT_LOG(W,
                    "mix port config for %s, handle %d not found "
                    "and was not created as flags are not specified",
                    config.toString().c_str(), ioHandle);
        return BAD_VALUE;
    } else {
        AudioPortConfig requestedPortConfig = portConfigIt->second;
        setPortConfigFromConfig(&requestedPortConfig, config);

        AudioPortMixExt& mixExt = requestedPortConfig.ext.get<AudioPortExt::Tag::mix>();
        if (mixExt.usecase.getTag() == AudioPortMixExtUseCase::Tag::source &&
                source != AudioSource::SYS_RESERVED_INVALID) {
            mixExt.usecase.get<AudioPortMixExtUseCase::Tag::source>() = source;
        }

        if (requestedPortConfig != portConfigIt->second) {
            return createOrUpdatePortConfig(requestedPortConfig, portConfig, created);
        } else {
            *portConfig = portConfigIt->second;
            *created = false;
        }
    }
    return OK;
}

status_t Hal2AidlMapper::findOrCreatePortConfig(
        const AudioPortConfig& requestedPortConfig, const std::set<int32_t>& destinationPortIds,
        AudioPortConfig* portConfig, bool* created) {
    using Tag = AudioPortExt::Tag;
    if (requestedPortConfig.ext.getTag() == Tag::mix) {
        if (const auto& p = requestedPortConfig;
                !p.sampleRate.has_value() || !p.channelMask.has_value() ||
                !p.format.has_value()) {
            AUGMENT_LOG(W, "provided mix port config is not fully specified: %s",
                        p.toString().c_str());
            return BAD_VALUE;
        }
        AudioConfig config;
        setConfigFromPortConfig(&config, requestedPortConfig);
        AudioSource source = requestedPortConfig.ext.get<Tag::mix>().usecase.getTag() ==
                AudioPortMixExtUseCase::Tag::source ?
                requestedPortConfig.ext.get<Tag::mix>().usecase.
                get<AudioPortMixExtUseCase::Tag::source>() : AudioSource::SYS_RESERVED_INVALID;
        return findOrCreateMixPortConfig(config, requestedPortConfig.flags,
                requestedPortConfig.ext.get<Tag::mix>().handle, source, destinationPortIds,
                portConfig, created);
    } else if (requestedPortConfig.ext.getTag() == Tag::device) {
        const auto& p = requestedPortConfig;
        const bool hasAudioConfig =
                p.sampleRate.has_value() && p.channelMask.has_value() && p.format.has_value();
        const bool hasGainConfig = p.gain.has_value();
        if (hasAudioConfig || hasGainConfig) {
            AudioConfig config, *configPtr = nullptr;
            if (hasAudioConfig) {
                setConfigFromPortConfig(&config, requestedPortConfig);
                configPtr = &config;
            }
            const AudioGainConfig* gainConfigPtr = nullptr;
            if (hasGainConfig) gainConfigPtr = &(*(p.gain));
            return findOrCreateDevicePortConfig(
                    requestedPortConfig.ext.get<Tag::device>().device, configPtr, gainConfigPtr,
                    portConfig, created);
        } else {
            AUGMENT_LOG(D, "device port config does not have audio or gain config specified");
            return findOrCreateDevicePortConfig(
                    requestedPortConfig.ext.get<Tag::device>().device, nullptr /*config*/,
                    nullptr /*gainConfig*/, portConfig, created);
        }
    }
    AUGMENT_LOG(W, "unsupported audio port config: %s", requestedPortConfig.toString().c_str());
    return BAD_VALUE;
}

status_t Hal2AidlMapper::findPortConfig(const AudioDevice& device, AudioPortConfig* portConfig) {
    if (auto it = findPortConfig(device); it != mPortConfigs.end()) {
        *portConfig = it->second;
        return OK;
    }
    AUGMENT_LOG(E, "could not find a device port config for device %s", device.toString().c_str());
    return BAD_VALUE;
}

Hal2AidlMapper::Patches::iterator Hal2AidlMapper::findPatch(
        const std::set<int32_t>& sourcePortConfigIds, const std::set<int32_t>& sinkPortConfigIds,
        PatchMatch match) {
    return std::find_if(mPatches.begin(), mPatches.end(),
            [&](const auto& pair) {
                const auto& p = pair.second;
                std::set<int32_t> patchSrcs(
                        p.sourcePortConfigIds.begin(), p.sourcePortConfigIds.end());
                std::set<int32_t> patchSinks(
                        p.sinkPortConfigIds.begin(), p.sinkPortConfigIds.end());
                switch (match) {
                    case MATCH_SOURCES:
                        return sourcePortConfigIds == patchSrcs;
                    case MATCH_SINKS:
                        return sinkPortConfigIds == patchSinks;
                    case MATCH_BOTH:
                        return sourcePortConfigIds == patchSrcs && sinkPortConfigIds == patchSinks;
                }
            });
}

Hal2AidlMapper::Ports::iterator Hal2AidlMapper::findPort(const AudioDevice& device) {
    if (device.type.type == AudioDeviceType::IN_DEFAULT) {
        return mPorts.find(mDefaultInputPortId);
    } else if (device.type.type == AudioDeviceType::OUT_DEFAULT) {
        return mPorts.find(mDefaultOutputPortId);
    }
    if (device.address.getTag() != AudioDeviceAddress::id ||
            !device.address.get<AudioDeviceAddress::id>().empty()) {
        return std::find_if(mPorts.begin(), mPorts.end(),
                [&](const auto& pair) { return audioDeviceMatches(device, pair.second); });
    }
    // For connection w/o an address, two ports can be found: the template port,
    // and a connected port (if exists). Make sure we return the connected port.
    Hal2AidlMapper::Ports::iterator portIt = mPorts.end();
    for (auto it = mPorts.begin(); it != mPorts.end(); ++it) {
        if (audioDeviceMatches(device, it->second)) {
            if (mConnectedPorts.find(it->first) != mConnectedPorts.end()) {
                return it;
            } else {
                // Will return 'it' if there is no connected port.
                portIt = it;
            }
        }
    }
    return portIt;
}

Hal2AidlMapper::Ports::iterator Hal2AidlMapper::findPort(
            const AudioConfig& config, const AudioIoFlags& flags,
            const std::set<int32_t>& destinationPortIds) {
    auto channelMaskMatches = [](const std::vector<AudioChannelLayout>& channelMasks,
                                 const AudioChannelLayout& channelMask) {
        // Return true when 1) the channel mask is none and none of the channel mask from the
        // collection contains haptic channel mask, or 2) the channel mask collection contains
        // the queried channel mask.
        return (channelMask.getTag() == AudioChannelLayout::none &&
                        std::none_of(channelMasks.begin(), channelMasks.end(),
                                     containHapticChannel)) ||
                std::find(channelMasks.begin(), channelMasks.end(), channelMask)
                    != channelMasks.end();
    };
    auto belongsToProfile = [&config, &channelMaskMatches](const AudioProfile& prof) {
        return (isDefaultAudioFormat(config.base.format) || prof.format == config.base.format) &&
                channelMaskMatches(prof.channelMasks, config.base.channelMask) &&
                (config.base.sampleRate == 0 ||
                        std::find(prof.sampleRates.begin(), prof.sampleRates.end(),
                                config.base.sampleRate) != prof.sampleRates.end());
    };
    static const std::vector<AudioOutputFlags> kOptionalOutputFlags{AudioOutputFlags::BIT_PERFECT};
    int optionalFlags = 0;
    auto flagMatches = [&flags, &optionalFlags](const AudioIoFlags& portFlags) {
        // Ports should be able to match if the optional flags are not requested.
        return portFlags == flags ||
               (portFlags.getTag() == AudioIoFlags::Tag::output &&
                        AudioIoFlags::make<AudioIoFlags::Tag::output>(
                                portFlags.get<AudioIoFlags::Tag::output>() &
                                        ~optionalFlags) == flags);
    };
    auto matcher = [&](const auto& pair) {
        const auto& p = pair.second;
        return p.ext.getTag() == AudioPortExt::Tag::mix &&
                flagMatches(p.flags) &&
                (destinationPortIds.empty() ||
                        std::any_of(destinationPortIds.begin(), destinationPortIds.end(),
                                [&](const int32_t destId) { return mRoutingMatrix.count(
                                            std::make_pair(p.id, destId)) != 0; })) &&
                (p.profiles.empty() ||
                        std::find_if(p.profiles.begin(), p.profiles.end(), belongsToProfile) !=
                        p.profiles.end()); };
    auto result = std::find_if(mPorts.begin(), mPorts.end(), matcher);
    if (result == mPorts.end() && flags.getTag() == AudioIoFlags::Tag::output) {
        auto optionalOutputFlagsIt = kOptionalOutputFlags.begin();
        while (result == mPorts.end() && optionalOutputFlagsIt != kOptionalOutputFlags.end()) {
            if (isBitPositionFlagSet(
                        flags.get<AudioIoFlags::Tag::output>(), *optionalOutputFlagsIt)) {
                // If the flag is set by the request, it must be matched.
                ++optionalOutputFlagsIt;
                continue;
            }
            optionalFlags |= makeBitPositionFlagMask(*optionalOutputFlagsIt++);
            result = std::find_if(mPorts.begin(), mPorts.end(), matcher);
            AUGMENT_LOG(I,
                        "port for config %s, flags %s was not found "
                        "retried with excluding optional flags %#x",
                        config.toString().c_str(), flags.toString().c_str(), optionalFlags);
        }
    }
    return result;
}

Hal2AidlMapper::PortConfigs::iterator Hal2AidlMapper::findPortConfig(const AudioDevice& device) {
    return std::find_if(mPortConfigs.begin(), mPortConfigs.end(),
            [&](const auto& pair) { return audioDeviceMatches(device, pair.second); });
}

Hal2AidlMapper::PortConfigs::iterator Hal2AidlMapper::findPortConfig(
            const std::optional<AudioConfig>& config,
            const std::optional<AudioIoFlags>& flags,
            int32_t ioHandle) {
    using Tag = AudioPortExt::Tag;
    return std::find_if(mPortConfigs.begin(), mPortConfigs.end(),
            [&](const auto& pair) {
                const auto& p = pair.second;
                LOG_ALWAYS_FATAL_IF(p.ext.getTag() == Tag::mix &&
                        (!p.sampleRate.has_value() || !p.channelMask.has_value() ||
                                !p.format.has_value() || !p.flags.has_value()),
                        "%s: stored mix port config is not fully specified: %s",
                        __func__, p.toString().c_str());
                return p.ext.getTag() == Tag::mix &&
                        (!config.has_value() ||
                                isConfigEqualToPortConfig(config.value(), p)) &&
                        (!flags.has_value() || p.flags.value() == flags.value()) &&
                        p.ext.template get<Tag::mix>().handle == ioHandle; });
}

status_t Hal2AidlMapper::getAudioMixPort(int32_t ioHandle, AudioPort* port) {
    auto it = findPortConfig(std::nullopt /*config*/, std::nullopt /*flags*/, ioHandle);
    if (it == mPortConfigs.end()) {
        AUGMENT_LOG(E, "cannot find mix port config for handle %u", ioHandle);
        return BAD_VALUE;
    }
    return updateAudioPort(it->second.portId, port);
}

status_t Hal2AidlMapper::getAudioPortCached(
        const ::aidl::android::media::audio::common::AudioDevice& device,
        ::aidl::android::media::audio::common::AudioPort* port) {
    if (auto portsIt = findPort(device); portsIt != mPorts.end()) {
        *port = portsIt->second;
        return OK;
    }
    AUGMENT_LOG(E, "device port for device %s is not found", device.toString().c_str());
    return BAD_VALUE;
}

status_t Hal2AidlMapper::initialize() {
    std::vector<AudioPort> ports;
    RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->getAudioPorts(&ports)));
    AUGMENT_LOG_IF(W, ports.empty(), "returned an empty list of audio ports");
    mDefaultInputPortId = mDefaultOutputPortId = -1;
    const int defaultDeviceFlag = 1 << AudioPortDeviceExt::FLAG_INDEX_DEFAULT_DEVICE;
    for (auto it = ports.begin(); it != ports.end(); ) {
        const auto& port = *it;
        if (port.ext.getTag() != AudioPortExt::Tag::device) {
            ++it;
            continue;
        }
        const AudioPortDeviceExt& deviceExt = port.ext.get<AudioPortExt::Tag::device>();
        if ((deviceExt.flags & defaultDeviceFlag) != 0) {
            if (port.flags.getTag() == AudioIoFlags::Tag::input) {
                mDefaultInputPortId = port.id;
            } else if (port.flags.getTag() == AudioIoFlags::Tag::output) {
                mDefaultOutputPortId = port.id;
            }
        }
        // For compatibility with HIDL, hide "template" remote submix ports from ports list.
        if (const auto& devDesc = deviceExt.device;
                (devDesc.type.type == AudioDeviceType::IN_SUBMIX ||
                        devDesc.type.type == AudioDeviceType::OUT_SUBMIX) &&
                devDesc.type.connection == AudioDeviceDescription::CONNECTION_VIRTUAL) {
            if (devDesc.type.type == AudioDeviceType::IN_SUBMIX) {
                mRemoteSubmixIn = port;
            } else {
                mRemoteSubmixOut = port;
            }
            it = ports.erase(it);
        } else {
            ++it;
        }
    }
    if (mRemoteSubmixIn.has_value() != mRemoteSubmixOut.has_value()) {
        AUGMENT_LOG(E,
                    "The configuration only has input or output remote submix device, "
                    "must have both");
        mRemoteSubmixIn.reset();
        mRemoteSubmixOut.reset();
    }
    if (mRemoteSubmixIn.has_value()) {
        AudioPort connectedRSubmixIn = *mRemoteSubmixIn;
        connectedRSubmixIn.ext.get<AudioPortExt::Tag::device>().device.address =
                AUDIO_REMOTE_SUBMIX_DEVICE_ADDRESS;
        AUGMENT_LOG(D, "connecting remote submix input");
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->connectExternalDevice(
                                connectedRSubmixIn, &connectedRSubmixIn)));
        // The template port for the remote submix input couldn't be "default" because it is not
        // attached. The connected port can now be made default because we never disconnect it.
        if (mDefaultInputPortId == -1) {
            mDefaultInputPortId = connectedRSubmixIn.id;
        }
        ports.push_back(std::move(connectedRSubmixIn));

        // Remote submix output must not be connected until the framework actually starts
        // using it, however for legacy compatibility we need to provide an "augmented template"
        // port with an address and profiles. It is obtained by connecting the output and then
        // immediately disconnecting it. This is a cheap operation as we don't open any streams.
        AudioPort tempConnectedRSubmixOut = *mRemoteSubmixOut;
        tempConnectedRSubmixOut.ext.get<AudioPortExt::Tag::device>().device.address =
                AUDIO_REMOTE_SUBMIX_DEVICE_ADDRESS;
        AUGMENT_LOG(D, "temporarily connecting and disconnecting remote submix output");
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->connectExternalDevice(
                                tempConnectedRSubmixOut, &tempConnectedRSubmixOut)));
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->disconnectExternalDevice(
                                tempConnectedRSubmixOut.id)));
        tempConnectedRSubmixOut.id = mRemoteSubmixOut->id;
        ports.push_back(std::move(tempConnectedRSubmixOut));
    }

    AUGMENT_LOG(I, "default port ids: input %d, output %d", mDefaultInputPortId,
                mDefaultOutputPortId);
    std::transform(ports.begin(), ports.end(), std::inserter(mPorts, mPorts.end()),
            [](const auto& p) { return std::make_pair(p.id, p); });
    RETURN_STATUS_IF_ERROR(updateRoutes());
    std::vector<AudioPortConfig> portConfigs;
    RETURN_STATUS_IF_ERROR(
            statusTFromBinderStatus(mModule->getAudioPortConfigs(&portConfigs)));  // OK if empty
    std::transform(portConfigs.begin(), portConfigs.end(),
            std::inserter(mPortConfigs, mPortConfigs.end()),
            [](const auto& p) { return std::make_pair(p.id, p); });
    std::transform(mPortConfigs.begin(), mPortConfigs.end(),
            std::inserter(mInitialPortConfigIds, mInitialPortConfigIds.end()),
            [](const auto& pcPair) { return pcPair.first; });
    std::vector<AudioPatch> patches;
    RETURN_STATUS_IF_ERROR(
            statusTFromBinderStatus(mModule->getAudioPatches(&patches)));  // OK if empty
    std::transform(patches.begin(), patches.end(),
            std::inserter(mPatches, mPatches.end()),
            [](const auto& p) { return std::make_pair(p.id, p); });
    return OK;
}

std::set<int32_t> Hal2AidlMapper::getPatchIdsByPortId(int32_t portId) {
    std::set<int32_t> result;
    for (const auto& [patchId, patch] : mPatches) {
        for (int32_t id : patch.sourcePortConfigIds) {
            if (portConfigBelongsToPort(id, portId)) {
                result.insert(patchId);
                break;
            }
        }
        for (int32_t id : patch.sinkPortConfigIds) {
            if (portConfigBelongsToPort(id, portId)) {
                result.insert(patchId);
                break;
            }
        }
    }
    return result;
}

status_t Hal2AidlMapper::prepareToDisconnectExternalDevice(const AudioPort& devicePort) {
    auto portsIt = findPort(devicePort.ext.get<AudioPortExt::device>().device);
    if (portsIt == mPorts.end()) {
        return BAD_VALUE;
    }
    return statusTFromBinderStatus(mModule->prepareToDisconnectExternalDevice(portsIt->second.id));
}

status_t Hal2AidlMapper::prepareToOpenStream(
        int32_t ioHandle, const AudioDevice& device, const AudioIoFlags& flags,
        AudioSource source, Cleanups* cleanups, AudioConfig* config,
        AudioPortConfig* mixPortConfig, AudioPatch* patch) {
    AUGMENT_LOG(D, "handle %d, device %s, flags %s, source %s, config %s, mixport config %s",
                ioHandle, device.toString().c_str(), flags.toString().c_str(),
                toString(source).c_str(), config->toString().c_str(),
                mixPortConfig->toString().c_str());
    resetUnusedPatchesAndPortConfigs();
    const AudioConfig initialConfig = *config;
    // Find / create AudioPortConfigs for the device port and the mix port,
    // then find / create a patch between them, and open a stream on the mix port.
    AudioPortConfig devicePortConfig;
    bool created = false;
    RETURN_STATUS_IF_ERROR(findOrCreateDevicePortConfig(device, config, nullptr /*gainConfig*/,
                    &devicePortConfig, &created));
    LOG_ALWAYS_FATAL_IF(devicePortConfig.id == 0);
    if (created) {
        cleanups->add(&Hal2AidlMapper::resetPortConfig, devicePortConfig.id);
    }
    status_t status = prepareToOpenStreamHelper(ioHandle, devicePortConfig.portId,
            devicePortConfig.id, flags, source, initialConfig, cleanups, config,
            mixPortConfig, patch);
    if (status != OK && !(mRemoteSubmixOut.has_value() &&
                initialConfig.base.format.type != AudioFormatType::PCM)) {
        // If using the client-provided config did not work out for establishing a mix port config
        // or patching, try with the device port config. Note that in general device port config and
        // mix port config are not required to be the same, however they must match if the HAL
        // module can't perform audio stream conversions.
        AudioConfig deviceConfig = initialConfig;
        if (setConfigFromPortConfig(&deviceConfig, devicePortConfig)->base != initialConfig.base) {
            AUGMENT_LOG(D, "retrying with device port config: %s",
                        devicePortConfig.toString().c_str());
            status = prepareToOpenStreamHelper(ioHandle, devicePortConfig.portId,
                    devicePortConfig.id, flags, source, initialConfig, cleanups,
                    &deviceConfig, mixPortConfig, patch);
            if (status == OK) {
                *config = deviceConfig;
            }
        }
    }
    return status;
}

status_t Hal2AidlMapper::prepareToOpenStreamHelper(
        int32_t ioHandle, int32_t devicePortId, int32_t devicePortConfigId,
        const AudioIoFlags& flags, AudioSource source, const AudioConfig& initialConfig,
        Cleanups* cleanups, AudioConfig* config, AudioPortConfig* mixPortConfig,
        AudioPatch* patch) {
    const bool isInput = flags.getTag() == AudioIoFlags::Tag::input;
    bool created = false;
    RETURN_STATUS_IF_ERROR(findOrCreateMixPortConfig(*config, flags, ioHandle, source,
                    std::set<int32_t>{devicePortId}, mixPortConfig, &created));
    if (created) {
        cleanups->add(&Hal2AidlMapper::resetPortConfig, mixPortConfig->id);
    }
    setConfigFromPortConfig(config, *mixPortConfig);
    bool retryWithSuggestedConfig = false;   // By default, let the framework to retry.
    if (mixPortConfig->id == 0 && config->base == AudioConfigBase{}) {
        // The HAL proposes a default config, can retry here.
        retryWithSuggestedConfig = true;
    } else if (isInput && config->base != initialConfig.base) {
        // If the resulting config is different, we must stop and provide the config to the
        // framework so that it can retry.
        mixPortConfig->id = 0;
    } else if (!isInput && mixPortConfig->id == 0 &&
                    (initialConfig.base.format.type == AudioFormatType::PCM ||
                            !isBitPositionFlagSet(flags.get<AudioIoFlags::output>(),
                                    AudioOutputFlags::DIRECT) ||
                            isBitPositionFlagSet(flags.get<AudioIoFlags::output>(),
                                    AudioOutputFlags::COMPRESS_OFFLOAD))) {
        // The framework does not retry opening non-direct PCM and IEC61937 outputs, need to retry
        // here (see 'AudioHwDevice::openOutputStream').
        retryWithSuggestedConfig = true;
    }
    if (mixPortConfig->id == 0 && retryWithSuggestedConfig) {
        AUGMENT_LOG(D, "retrying to find/create a mix port config using config %s",
                    config->toString().c_str());
        RETURN_STATUS_IF_ERROR(findOrCreateMixPortConfig(*config, flags, ioHandle, source,
                        std::set<int32_t>{devicePortId}, mixPortConfig, &created));
        if (created) {
            cleanups->add(&Hal2AidlMapper::resetPortConfig, mixPortConfig->id);
        }
        setConfigFromPortConfig(config, *mixPortConfig);
    }
    if (mixPortConfig->id == 0) {
        AUGMENT_LOG(D, "returning suggested config for the stream: %s",
                    config->toString().c_str());
        return OK;
    }
    if (isInput) {
        RETURN_STATUS_IF_ERROR(findOrCreatePatch(
                        {devicePortConfigId}, {mixPortConfig->id}, MATCH_BOTH, patch, &created));
    } else {
        RETURN_STATUS_IF_ERROR(findOrCreatePatch(
                        {mixPortConfig->id}, {devicePortConfigId}, MATCH_BOTH, patch, &created));
    }
    if (created) {
        cleanups->add(&Hal2AidlMapper::resetPatch, patch->id);
    }
    if (config->frameCount <= 0) {
        config->frameCount = patch->minimumStreamBufferSizeFrames;
    }
    return OK;
}

status_t Hal2AidlMapper::setPortConfig(
        const AudioPortConfig& requestedPortConfig, const std::set<int32_t>& destinationPortIds,
        AudioPortConfig* portConfig, Cleanups* cleanups) {
    bool created = false;
    RETURN_STATUS_IF_ERROR(findOrCreatePortConfig(
                    requestedPortConfig, destinationPortIds, portConfig, &created));
    if (created && cleanups != nullptr) {
        cleanups->add(&Hal2AidlMapper::resetPortConfig, portConfig->id);
    }
    return OK;
}

status_t Hal2AidlMapper::releaseAudioPatch(int32_t patchId) {
    return releaseAudioPatches({patchId});
}

// Note: does not reset port configs.
status_t Hal2AidlMapper::releaseAudioPatch(Patches::iterator it) {
    const int32_t patchId = it->first;
    AUGMENT_LOG(D, "patchId %d", patchId);
    if (ndk::ScopedAStatus status = mModule->resetAudioPatch(patchId); !status.isOk()) {
        AUGMENT_LOG(E, "error while resetting patch %d: %s", patchId,
                    status.getDescription().c_str());
        return statusTFromBinderStatus(status);
    }
    mPatches.erase(it);
    for (auto it = mFwkPatches.begin(); it != mFwkPatches.end(); ++it) {
        if (it->second == patchId) {
            mFwkPatches.erase(it);
            break;
        }
    }
    return OK;
}

status_t Hal2AidlMapper::releaseAudioPatches(const std::set<int32_t>& patchIds) {
    status_t result = OK;
    for (const auto patchId : patchIds) {
        if (auto it = mPatches.find(patchId); it != mPatches.end()) {
            releaseAudioPatch(it);
        } else {
            AUGMENT_LOG(E, "patch id %d not found", patchId);
            result = BAD_VALUE;
        }
    }
    resetUnusedPortConfigs();
    return result;
}

void Hal2AidlMapper::resetPortConfig(int32_t portConfigId) {
    if (auto it = mPortConfigs.find(portConfigId); it != mPortConfigs.end()) {
        AUGMENT_LOG(D, "%s", it->second.toString().c_str());
        if (ndk::ScopedAStatus status = mModule->resetAudioPortConfig(portConfigId);
                !status.isOk()) {
            AUGMENT_LOG(E, "error while resetting port config %d: %s", portConfigId,
                        status.getDescription().c_str());
            return;
        }
        mPortConfigs.erase(it);
        return;
    }
    AUGMENT_LOG(E, "port config id %d not found", portConfigId);
}

void Hal2AidlMapper::resetUnusedPatchesAndPortConfigs() {
    // Since patches can be created independently of streams via 'createOrUpdatePatch',
    // here we only clean up patches for released streams.
    std::set<int32_t> patchesToRelease;
    for (auto it = mStreams.begin(); it != mStreams.end(); ) {
        if (auto streamSp = it->first.promote(); streamSp) {
            ++it;
        } else {
            if (const int32_t patchId = it->second.second; patchId != -1) {
                patchesToRelease.insert(patchId);
            }
            it = mStreams.erase(it);
        }
    }
    // 'releaseAudioPatches' also resets unused port configs.
    releaseAudioPatches(patchesToRelease);
}

void Hal2AidlMapper::resetUnusedPortConfigs() {
    // The assumption is that port configs are used to create patches
    // (or to open streams, but that involves creation of patches, too). Thus,
    // orphaned port configs can and should be reset.
    std::set<int32_t> portConfigIdsToReset;
    std::transform(mPortConfigs.begin(), mPortConfigs.end(),
            std::inserter(portConfigIdsToReset, portConfigIdsToReset.end()),
            [](const auto& pcPair) { return pcPair.first; });
    for (const auto& p : mPatches) {
        for (int32_t id : p.second.sourcePortConfigIds) portConfigIdsToReset.erase(id);
        for (int32_t id : p.second.sinkPortConfigIds) portConfigIdsToReset.erase(id);
    }
    for (int32_t id : mInitialPortConfigIds) {
        portConfigIdsToReset.erase(id);
    }
    for (const auto& s : mStreams) {
        portConfigIdsToReset.erase(s.second.first);
    }
    for (const auto& portConfigId : portConfigIdsToReset) {
        resetPortConfig(portConfigId);
    }
}

status_t Hal2AidlMapper::setDevicePortConnectedState(const AudioPort& devicePort, bool connected) {
    AUGMENT_LOG(D, "state %s, device %s", (connected ? "connected" : "disconnected"),
                devicePort.toString().c_str());
    resetUnusedPatchesAndPortConfigs();
    if (connected) {
        AudioDevice matchDevice = devicePort.ext.get<AudioPortExt::device>().device;
        std::optional<AudioPort> templatePort;
        auto erasePortAfterConnectionIt = mPorts.end();
        // Connection of remote submix out with address "0" is a special case. Since there is
        // already an "augmented template" port with this address in mPorts, we need to replace
        // it with a connected port.
        // Connection of remote submix outs with any other address is done as usual except that
        // the template port is in `mRemoteSubmixOut`.
        if (mRemoteSubmixOut.has_value() && matchDevice.type.type == AudioDeviceType::OUT_SUBMIX) {
            if (matchDevice.address == AudioDeviceAddress::make<AudioDeviceAddress::id>(
                            AUDIO_REMOTE_SUBMIX_DEVICE_ADDRESS)) {
                erasePortAfterConnectionIt = findPort(matchDevice);
            }
            templatePort = mRemoteSubmixOut;
        } else if (mRemoteSubmixIn.has_value() &&
                matchDevice.type.type == AudioDeviceType::IN_SUBMIX) {
            templatePort = mRemoteSubmixIn;
        } else {
            // Reset the device address to find the "template" port.
            matchDevice.address = AudioDeviceAddress::make<AudioDeviceAddress::id>();
        }
        if (!templatePort.has_value()) {
            auto portsIt = findPort(matchDevice);
            if (portsIt == mPorts.end()) {
                // Since 'setConnectedState' is called for all modules, it is normal when the device
                // port not found in every one of them.
                return BAD_VALUE;
            } else {
                AUGMENT_LOG(D, "device port for device %s found", matchDevice.toString().c_str());
            }
            templatePort = portsIt->second;
        }

        // Use the ID of the "template" port, use all the information from the provided port.
        AudioPort connectedPort = devicePort;
        connectedPort.id = templatePort->id;
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->connectExternalDevice(
                                connectedPort, &connectedPort)));
        const auto [it, inserted] = mPorts.insert(std::make_pair(connectedPort.id, connectedPort));
        LOG_ALWAYS_FATAL_IF(
                !inserted, "%s duplicate port ID received from HAL: %s, existing port: %s",
                __func__, connectedPort.toString().c_str(), it->second.toString().c_str());
        mConnectedPorts.insert(connectedPort.id);
        if (erasePortAfterConnectionIt != mPorts.end()) {
            mPorts.erase(erasePortAfterConnectionIt);
        }
    } else {  // !connected
        AudioDevice matchDevice = devicePort.ext.get<AudioPortExt::device>().device;
        auto portsIt = findPort(matchDevice);
        if (portsIt == mPorts.end()) {
            // Since 'setConnectedState' is called for all modules, it is normal when the device
            // port not found in every one of them.
            return BAD_VALUE;
        } else {
            AUGMENT_LOG(D, "device port for device %s found", matchDevice.toString().c_str());
        }

        // Disconnection of remote submix out with address "0" is a special case. We need to replace
        // the connected port entry with the "augmented template".
        const int32_t portId = portsIt->second.id;
        if (mRemoteSubmixOut.has_value() && matchDevice.type.type == AudioDeviceType::OUT_SUBMIX &&
                matchDevice.address == AudioDeviceAddress::make<AudioDeviceAddress::id>(
                        AUDIO_REMOTE_SUBMIX_DEVICE_ADDRESS)) {
            mDisconnectedPortReplacement = std::make_pair(portId, *mRemoteSubmixOut);
            auto& port = mDisconnectedPortReplacement.second;
            port.ext.get<AudioPortExt::Tag::device>().device = matchDevice;
            port.profiles = portsIt->second.profiles;
        }

        // Patches may still exist, the framework may reset or update them later.
        // For disconnection to succeed, need to release these patches first.
        if (std::set<int32_t> patchIdsToRelease = getPatchIdsByPortId(portId);
                !patchIdsToRelease.empty()) {
            FwkPatches releasedPatches;
            status_t status = OK;
            for (int32_t patchId : patchIdsToRelease) {
                if (auto it = mPatches.find(patchId); it != mPatches.end()) {
                    if (status = releaseAudioPatch(it); status != OK) break;
                    releasedPatches.insert(std::make_pair(patchId, patchId));
                }
            }
            resetUnusedPortConfigs();
            // Patches created by Hal2AidlMapper during stream creation and not "claimed"
            // by the framework must not be surfaced to it.
            for (auto& s : mStreams) {
                if (auto it = releasedPatches.find(s.second.second); it != releasedPatches.end()) {
                    releasedPatches.erase(it);
                }
            }
            mFwkPatches.merge(releasedPatches);
            LOG_ALWAYS_FATAL_IF(!releasedPatches.empty(),
                    "mFwkPatches already contains some of released patches");
            if (status != OK) return status;
        }
        RETURN_STATUS_IF_ERROR(statusTFromBinderStatus(mModule->disconnectExternalDevice(portId)));
        eraseConnectedPort(portId);
    }
    return updateRoutes();
}

status_t Hal2AidlMapper::updateAudioPort(int32_t portId, AudioPort* port) {
    const status_t status = statusTFromBinderStatus(mModule->getAudioPort(portId, port));
    if (status == OK) {
        auto portIt = mPorts.find(portId);
        if (portIt != mPorts.end()) {
            if (port->ext.getTag() == AudioPortExt::Tag::mix && portIt->second != *port) {
                mDynamicMixPortIds.insert(portId);
            }
            portIt->second = *port;
        } else {
            AUGMENT_LOG(W, "port(%d) returned successfully from the HAL but not it is not cached",
                        portId);
        }
    }
    return status;
}

status_t Hal2AidlMapper::updateRoutes() {
    RETURN_STATUS_IF_ERROR(
            statusTFromBinderStatus(mModule->getAudioRoutes(&mRoutes)));
    AUGMENT_LOG_IF(W, mRoutes.empty(), "returned an empty list of audio routes");
    if (mRemoteSubmixIn.has_value()) {
        // Remove mentions of the template remote submix input from routes.
        int32_t rSubmixInId = mRemoteSubmixIn->id;
        // Remove mentions of the template remote submix out only if it is not in mPorts
        // (that means there is a connected port in mPorts).
        int32_t rSubmixOutId = mPorts.find(mRemoteSubmixOut->id) == mPorts.end() ?
                mRemoteSubmixOut->id : -1;
        for (auto it = mRoutes.begin(); it != mRoutes.end();) {
            auto& route = *it;
            if (route.sinkPortId == rSubmixOutId) {
                it = mRoutes.erase(it);
                continue;
            }
            if (auto routeIt = std::find(route.sourcePortIds.begin(), route.sourcePortIds.end(),
                            rSubmixInId); routeIt != route.sourcePortIds.end()) {
                route.sourcePortIds.erase(routeIt);
                if (route.sourcePortIds.empty()) {
                    it = mRoutes.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
    mRoutingMatrix.clear();
    for (const auto& r : mRoutes) {
        for (auto portId : r.sourcePortIds) {
            mRoutingMatrix.emplace(r.sinkPortId, portId);
            mRoutingMatrix.emplace(portId, r.sinkPortId);
        }
    }
    return OK;
}

void Hal2AidlMapper::updateDynamicMixPorts() {
    const auto dynamicMixPortIds = mDynamicMixPortIds;
    for (int32_t portId : dynamicMixPortIds) {
        if (auto it = mPorts.find(portId); it != mPorts.end()) {
            updateAudioPort(portId, &it->second);
        } else {
            // This must not happen
            AUGMENT_LOG(E, "cannot find port for id=%d", portId);
        }
    }
}

} // namespace android
