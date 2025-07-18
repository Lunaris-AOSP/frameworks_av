/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_STREAM_HAL_INTERFACE_H
#define ANDROID_HARDWARE_STREAM_HAL_INTERFACE_H

#include <vector>

#include <android/media/MicrophoneInfoFw.h>
#include <media/audiohal/EffectHalInterface.h>
#include <system/audio.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/String8.h>
#include <utils/Vector.h>

namespace android {

class StreamHalInterface : public virtual RefBase
{
  public:
    // Return size of input/output buffer in bytes for this stream - eg. 4800.
    virtual status_t getBufferSize(size_t *size) = 0;

    // Return the base configuration of the stream:
    //   - channel mask;
    //   - format - e.g. AUDIO_FORMAT_PCM_16_BIT;
    //   - sampling rate in Hz - eg. 44100.
    virtual status_t getAudioProperties(audio_config_base_t *configBase) = 0;

    // Convenience method.
    inline status_t getAudioProperties(
            uint32_t *sampleRate, audio_channel_mask_t *mask, audio_format_t *format) {
        audio_config_base_t config = AUDIO_CONFIG_BASE_INITIALIZER;
        const status_t result = getAudioProperties(&config);
        if (result == NO_ERROR) {
            if (sampleRate != nullptr) *sampleRate = config.sample_rate;
            if (mask != nullptr) *mask = config.channel_mask;
            if (format != nullptr) *format = config.format;
        }
        return result;
    }

    // Set audio stream parameters.
    virtual status_t setParameters(const String8& kvPairs) = 0;

    // Get audio stream parameters.
    virtual status_t getParameters(const String8& keys, String8 *values) = 0;

    // Return the frame size (number of bytes per sample) of a stream.
    virtual status_t getFrameSize(size_t *size) = 0;

    // Add or remove the effect on the stream.
    virtual status_t addEffect(sp<EffectHalInterface> effect) = 0;
    virtual status_t removeEffect(sp<EffectHalInterface> effect) = 0;

    // Put the audio hardware input/output into standby mode.
    virtual status_t standby() = 0;

    virtual status_t dump(int fd, const Vector<String16>& args = {}) = 0;

    // Start a stream operating in mmap mode.
    virtual status_t start() = 0;

    // Stop a stream operating in mmap mode.
    virtual status_t stop() = 0;

    // Retrieve information on the data buffer in mmap mode.
    virtual status_t createMmapBuffer(int32_t minSizeFrames,
                                      struct audio_mmap_buffer_info *info) = 0;

    // Get current read/write position in the mmap buffer
    virtual status_t getMmapPosition(struct audio_mmap_position *position) = 0;

    // Set the priority of the thread that interacts with the HAL
    // (must match the priority of the audioflinger's thread that calls 'read' / 'write')
    virtual status_t setHalThreadPriority(int priority) = 0;

    virtual status_t legacyCreateAudioPatch(const struct audio_port_config& port,
                                            std::optional<audio_source_t> source,
                                            audio_devices_t type) = 0;

    virtual status_t legacyReleaseAudioPatch() = 0;

  protected:
    // Subclasses can not be constructed directly by clients.
    StreamHalInterface() {}

    // The destructor automatically closes the stream.
    virtual ~StreamHalInterface() {}
};

class StreamOutHalInterfaceCallback : public virtual RefBase {
  public:
    virtual void onWriteReady() {}
    virtual void onDrainReady() {}
    virtual void onError(bool /*isHardError*/) {}

  protected:
    StreamOutHalInterfaceCallback() = default;
    virtual ~StreamOutHalInterfaceCallback() = default;
};

class StreamOutHalInterfaceEventCallback : public virtual RefBase {
public:
    virtual void onCodecFormatChanged(const std::vector<uint8_t>& metadataBs) = 0;

protected:
    StreamOutHalInterfaceEventCallback() = default;
    virtual ~StreamOutHalInterfaceEventCallback() = default;
};

class StreamOutHalInterfaceLatencyModeCallback : public virtual RefBase {
public:
    /**
     * Called with the new list of supported latency modes when a change occurs.
     */
    virtual void onRecommendedLatencyModeChanged(std::vector<audio_latency_mode_t> modes) = 0;

protected:
    StreamOutHalInterfaceLatencyModeCallback() = default;
    virtual ~StreamOutHalInterfaceLatencyModeCallback() = default;
};

/**
 * On position reporting. There are two methods: 'getRenderPosition' and
 * 'getPresentationPosition'. The first difference is that they may have a
 * time offset because "render" position relates to what happens between
 * ADSP and DAC, while "observable" position is relative to the external
 * observer. The second difference is that 'getRenderPosition' always
 * resets on standby (for all types of stream data) according to its
 * definition. Since the original C definition of 'getRenderPosition' used
 * 32-bit frame counters, and also because in complex playback chains that
 * include wireless devices the "observable" position has more practical
 * meaning, 'getRenderPosition' does not exist in the AIDL HAL interface.
 * The table below summarizes frame count behavior for 'getPresentationPosition':
 *
 *               | Mixed      | Direct       | Direct
 *               |            | non-offload  | offload
 * ==============|============|==============|==============
 *  PCM          | Continuous |              |
 *               |            |              |
 *               |            |              |
 * --------------|------------| Continuous†  |
 *  Bitstream    |            |              | Reset on
 *  encapsulated |            |              | flush, drain
 *  into PCM     |            |              | and standby
 *               | Not        |              |
 * --------------| supported  |--------------|
 *  Bitstream    |            | Reset on     |
 *               |            | flush, drain |
 *               |            | and standby  |
 *               |            |              |
 *
 * † - on standby, reset of the frame count happens at the framework level.
 */
class StreamOutHalInterface : public virtual StreamHalInterface {
  public:
    // Return the audio hardware driver estimated latency in milliseconds.
    virtual status_t getLatency(uint32_t *latency) = 0;

    // Use this method in situations where audio mixing is done in the hardware.
    virtual status_t setVolume(float left, float right) = 0;

    // Selects the audio presentation (if available).
    virtual status_t selectPresentation(int presentationId, int programId) = 0;

    // Write audio buffer to driver.
    virtual status_t write(const void *buffer, size_t bytes, size_t *written) = 0;

    // Return the number of audio frames written by the audio dsp to DAC since
    // the output has exited standby.
    virtual status_t getRenderPosition(uint64_t *dspFrames) = 0;

    // Set the callback for notifying completion of non-blocking write and drain.
    // The callback must be owned by someone else. The output stream does not own it
    // to avoid strong pointer loops.
    virtual status_t setCallback(wp<StreamOutHalInterfaceCallback> callback) = 0;

    // Returns whether pause and resume operations are supported.
    virtual status_t supportsPauseAndResume(bool *supportsPause, bool *supportsResume) = 0;

    // Notifies to the audio driver to resume playback following a pause.
    virtual status_t pause() = 0;

    // Notifies to the audio driver to resume playback following a pause.
    virtual status_t resume() = 0;

    // Returns whether drain operation is supported.
    virtual status_t supportsDrain(bool *supportsDrain) = 0;

    // Requests notification when data buffered by the driver/hardware has been played.
    virtual status_t drain(bool earlyNotify) = 0;

    // Notifies to the audio driver to flush (that is, drop) the queued data. Stream must
    // already be paused before calling 'flush'.
    virtual status_t flush() = 0;

    // Return a recent count of the number of audio frames presented to an external observer.
    // This excludes frames which have been written but are still in the pipeline. See the
    // table at the start of the 'StreamOutHalInterface' for the specification of the frame
    // count behavior w.r.t. 'flush', 'drain' and 'standby' operations.
    virtual status_t getPresentationPosition(uint64_t *frames, struct timespec *timestamp) = 0;

    // Notifies the HAL layer that the framework considers the current playback as completed.
    virtual status_t presentationComplete() = 0;

    struct SourceMetadata {
        std::vector<playback_track_metadata_v7_t> tracks;
    };

    /**
     * Called when the metadata of the stream's source has been changed.
     * @param sourceMetadata Description of the audio that is played by the clients.
     */
    virtual status_t updateSourceMetadata(const SourceMetadata& sourceMetadata) = 0;

    // Returns the Dual Mono mode presentation setting.
    virtual status_t getDualMonoMode(audio_dual_mono_mode_t* mode) = 0;

    // Sets the Dual Mono mode presentation on the output device.
    virtual status_t setDualMonoMode(audio_dual_mono_mode_t mode) = 0;

    // Returns the Audio Description Mix level in dB.
    virtual status_t getAudioDescriptionMixLevel(float* leveldB) = 0;

    // Sets the Audio Description Mix level in dB.
    virtual status_t setAudioDescriptionMixLevel(float leveldB) = 0;

    // Retrieves current playback rate parameters.
    virtual status_t getPlaybackRateParameters(audio_playback_rate_t* playbackRate) = 0;

    // Sets the playback rate parameters that control playback behavior.
    virtual status_t setPlaybackRateParameters(const audio_playback_rate_t& playbackRate) = 0;

    virtual status_t setEventCallback(const sp<StreamOutHalInterfaceEventCallback>& callback) = 0;

    /**
     * Indicates the requested latency mode for this output stream.
     *
     * The requested mode can be one of the modes returned by
     * getRecommendedLatencyModes() API.
     *
     * @param mode the requested latency mode.
     * @return operation completion status.
     */
    virtual status_t setLatencyMode(audio_latency_mode_t mode) = 0;

    /**
     * Indicates which latency modes are currently supported on this output stream.
     * If the transport protocol (e.g Bluetooth A2DP) used by this output stream to reach
     * the output device supports variable latency modes, the HAL indicates which
     * modes are currently supported.
     * The framework can then call setLatencyMode() with one of the supported modes to select
     * the desired operation mode.
     *
     * @param modes currrently supported latency modes.
     * @return operation completion status.
     */
    virtual status_t getRecommendedLatencyModes(std::vector<audio_latency_mode_t> *modes) = 0;

    /**
     * Set the callback interface for notifying changes in supported latency modes.
     *
     * Calling this method with a null pointer will result in releasing
     * the callback.
     *
     * @param callback the registered callback or null to unregister.
     * @return operation completion status.
     */
    virtual status_t setLatencyModeCallback(
            const sp<StreamOutHalInterfaceLatencyModeCallback>& callback) = 0;

    /**
     * Signal the end of audio output, interrupting an ongoing 'write' operation.
     */
    virtual status_t exit() = 0;

  protected:
    virtual ~StreamOutHalInterface() {}
};

class StreamInHalInterface : public virtual StreamHalInterface {
  public:
    // Set the input gain for the audio driver.
    virtual status_t setGain(float gain) = 0;

    // Read audio buffer in from driver.
    virtual status_t read(void *buffer, size_t bytes, size_t *read) = 0;

    // Return the amount of input frames lost in the audio driver.
    virtual status_t getInputFramesLost(uint32_t *framesLost) = 0;

    // Return a recent count of the number of audio frames received and
    // the clock time associated with that frame count.
    // The count must not reset to zero when a PCM input enters standby.
    virtual status_t getCapturePosition(int64_t *frames, int64_t *time) = 0;

    // Get active microphones
    virtual status_t getActiveMicrophones(std::vector<media::MicrophoneInfoFw> *microphones) = 0;

    // Set direction for capture processing
    virtual status_t setPreferredMicrophoneDirection(audio_microphone_direction_t) = 0;

    // Set zoom factor for capture stream
    virtual status_t setPreferredMicrophoneFieldDimension(float zoom) = 0;

    struct SinkMetadata {
        std::vector<record_track_metadata_v7_t> tracks;
    };
    /**
     * Called when the metadata of the stream's sink has been changed.
     * @param sinkMetadata Description of the audio that is suggested by the clients.
     */
    virtual status_t updateSinkMetadata(const SinkMetadata& sinkMetadata) = 0;

  protected:
    virtual ~StreamInHalInterface() {}
};

} // namespace android

#endif // ANDROID_HARDWARE_STREAM_HAL_INTERFACE_H
