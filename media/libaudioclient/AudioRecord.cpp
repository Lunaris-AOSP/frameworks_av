/*
**
** Copyright 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioRecord"

#include <inttypes.h>
#include <android-base/macros.h>
#include <android-base/stringprintf.h>
#include <sys/resource.h>

#include <audio_utils/format.h>
#include <audiomanager/AudioManager.h>
#include <audiomanager/IAudioManager.h>
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <media/AudioRecord.h>
#include <utils/Log.h>
#include <private/media/AudioTrackShared.h>
#include <processgroup/sched_policy.h>
#include <media/IAudioFlinger.h>
#include <media/MediaMetricsItem.h>
#include <media/TypeConverter.h>

#define WAIT_PERIOD_MS          10

namespace android {

using ::android::base::StringPrintf;
using android::content::AttributionSourceState;
using aidl_utils::statusTFromBinderStatus;

// ---------------------------------------------------------------------------

// static
status_t AudioRecord::getMinFrameCount(
        size_t* frameCount,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask)
{
    if (frameCount == NULL) {
        return BAD_VALUE;
    }

    size_t size;
    status_t status = AudioSystem::getInputBufferSize(sampleRate, format, channelMask, &size);
    if (status != NO_ERROR) {
        ALOGE("%s(): AudioSystem could not query the input buffer size for"
              " sampleRate %u, format %#x, channelMask %#x; status %d",
               __func__, sampleRate, format, channelMask, status);
        return status;
    }

    // We double the size of input buffer for ping pong use of record buffer.
    const auto frameSize = audio_bytes_per_frame(
            audio_channel_count_from_in_mask(channelMask), format);
    if (frameSize == 0 || ((*frameCount = (size * 2) / frameSize) == 0)) {
        ALOGE("%s(): Unsupported configuration: sampleRate %u, format %#x, channelMask %#x",
                __func__, sampleRate, format, channelMask);
        return BAD_VALUE;
    }

    return NO_ERROR;
}

status_t AudioRecord::logIfErrorAndReturnStatus(status_t status, const std::string& errorMessage,
                                                const std::string& func) {
    if (status != NO_ERROR) {
        if (!func.empty()) mMediaMetrics.markError(status, func.c_str());
        ALOGE_IF(!errorMessage.empty(), "%s", errorMessage.c_str());
        reportError(status, AMEDIAMETRICS_PROP_EVENT_VALUE_CREATE, errorMessage.c_str());
    }
    mStatus = status;
    return mStatus;
}

// ---------------------------------------------------------------------------

void AudioRecord::MediaMetrics::gather(const AudioRecord *record)
{
#define MM_PREFIX "android.media.audiorecord." // avoid cut-n-paste errors.

    // Java API 28 entries, do not change.
    mMetricsItem->setCString(MM_PREFIX "encoding", toString(record->mFormat).c_str());
    mMetricsItem->setCString(MM_PREFIX "source", toString(record->mAttributes.source).c_str());
    mMetricsItem->setInt32(MM_PREFIX "latency", (int32_t)record->mLatency); // bad estimate.
    mMetricsItem->setInt32(MM_PREFIX "samplerate", (int32_t)record->mSampleRate);
    mMetricsItem->setInt32(MM_PREFIX "channels", (int32_t)record->mChannelCount);

    // Non-API entries, these can change.
    mMetricsItem->setInt32(MM_PREFIX "portId", (int32_t)record->mPortId);
    mMetricsItem->setInt32(MM_PREFIX "frameCount", (int32_t)record->mFrameCount);
    mMetricsItem->setCString(MM_PREFIX "attributes", toString(record->mAttributes).c_str());
    mMetricsItem->setInt64(MM_PREFIX "channelMask", (int64_t)record->mChannelMask);

    // log total duration recording, including anything currently running.
    int64_t activeNs = 0;
    if (mStartedNs != 0) {
        activeNs = systemTime() - mStartedNs;
    }
    mMetricsItem->setDouble(MM_PREFIX "durationMs", (mDurationNs + activeNs) * 1e-6);
    mMetricsItem->setInt64(MM_PREFIX "startCount", (int64_t)mCount);

    if (mLastError != NO_ERROR) {
        mMetricsItem->setInt32(MM_PREFIX "lastError.code", (int32_t)mLastError);
        mMetricsItem->setCString(MM_PREFIX "lastError.at", mLastErrorFunc.c_str());
    }
    mMetricsItem->setCString(MM_PREFIX "logSessionId", record->mLogSessionId.c_str());
}

static const char *stateToString(bool active) {
    return active ? "ACTIVE" : "STOPPED";
}

// hand the user a snapshot of the metrics.
status_t AudioRecord::getMetrics(mediametrics::Item * &item)
{
    mMediaMetrics.gather(this);
    mediametrics::Item *tmp = mMediaMetrics.dup();
    if (tmp == nullptr) {
        return BAD_VALUE;
    }
    item = tmp;
    return NO_ERROR;
}

AudioRecord::AudioRecord(const AttributionSourceState &client)
    : mClientAttributionSource(client)
{
}

AudioRecord::AudioRecord(
        audio_source_t inputSource,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        const AttributionSourceState& client,
        size_t frameCount,
        const wp<IAudioRecordCallback>& callback,
        uint32_t notificationFrames,
        audio_session_t sessionId,
        transfer_type transferType,
        audio_input_flags_t flags,
        const audio_attributes_t* pAttributes,
        audio_port_handle_t selectedDeviceId,
        audio_microphone_direction_t selectedMicDirection,
        float microphoneFieldDimension)
    : mClientAttributionSource(client)
{
    uid_t uid = VALUE_OR_FATAL(aidl2legacy_int32_t_uid_t(mClientAttributionSource.uid));
    pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(mClientAttributionSource.pid));
    (void)set(inputSource, sampleRate, format, channelMask, frameCount, callback,
            notificationFrames, false /*threadCanCallJava*/, sessionId, transferType, flags,
            uid, pid, pAttributes, selectedDeviceId, selectedMicDirection,
            microphoneFieldDimension);
}

AudioRecord::~AudioRecord()
{
    mMediaMetrics.gather(this);

    mediametrics::LogItem(mMetricsId)
        .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_DTOR)
        .set(AMEDIAMETRICS_PROP_CALLERNAME,
                mCallerName.empty()
                ? AMEDIAMETRICS_PROP_CALLERNAME_VALUE_UNKNOWN
                : mCallerName.c_str())
        .set(AMEDIAMETRICS_PROP_STATUS, (int32_t)mStatus)
        .record();

    stopAndJoinCallbacks(); // checks mStatus

    if (mStatus == NO_ERROR) {
        IInterface::asBinder(mAudioRecord)->unlinkToDeath(mDeathNotifier, this);
        mAudioRecord.clear();
        mCblkMemory.clear();
        mBufferMemory.clear();
        IPCThreadState::self()->flushCommands();
        ALOGV("%s(%d): releasing session id %d",
                __func__, mPortId, mSessionId);
        pid_t pid = VALUE_OR_FATAL(aidl2legacy_int32_t_pid_t(mClientAttributionSource.pid));
        AudioSystem::releaseAudioSessionId(mSessionId, pid);
    }
}

void AudioRecord::stopAndJoinCallbacks() {
    // Make sure that callback function exits in the case where
    // it is looping on buffer empty condition in obtainBuffer().
    // Otherwise the callback thread will never exit.
    stop();
    if (mAudioRecordThread != 0) {
        mAudioRecordThread->requestExit();  // see comment in AudioRecord.h
        mProxy->interrupt();
        mAudioRecordThread->requestExitAndWait();
        mAudioRecordThread.clear();
    }

    AutoMutex lock(mLock);
    if (mDeviceCallback != 0 && mInput != AUDIO_IO_HANDLE_NONE) {
        // This may not stop all of these device callbacks!
        // TODO: Add some sort of protection.
        AudioSystem::removeAudioDeviceCallback(this, mInput, mPortId);
        mDeviceCallback.clear();
    }
}
status_t AudioRecord::set(
        audio_source_t inputSource,
        uint32_t sampleRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        size_t frameCount,
        const wp<IAudioRecordCallback>& callback,
        uint32_t notificationFrames,
        bool threadCanCallJava,
        audio_session_t sessionId,
        transfer_type transferType,
        audio_input_flags_t flags,
        uid_t uid,
        pid_t pid,
        const audio_attributes_t* pAttributes,
        audio_port_handle_t selectedDeviceId,
        audio_microphone_direction_t selectedMicDirection,
        float microphoneFieldDimension,
        int32_t maxSharedAudioHistoryMs)
{
    status_t status = NO_ERROR;
    LOG_ALWAYS_FATAL_IF(mInitialized, "%s: should not be called twice", __func__);
    mInitialized = true;
    // Note mPortId is not valid until the track is created, so omit mPortId in ALOG for set.
    ALOGV("%s(): inputSource %d, sampleRate %u, format %#x, channelMask %#x, frameCount %zu, "
          "notificationFrames %u, sessionId %d, transferType %d, flags %#x, attributionSource %s"
          "uid %d, pid %d",
          __func__,
          inputSource, sampleRate, format, channelMask, frameCount, notificationFrames,
          sessionId, transferType, flags, mClientAttributionSource.toString().c_str(), uid, pid);

    // TODO b/182392553: refactor or remove
    pid_t callingPid = IPCThreadState::self()->getCallingPid();
    pid_t myPid = getpid();
    pid_t adjPid = pid;
    if (pid == -1 || (callingPid != myPid)) {
        adjPid = callingPid;
    }
    auto clientAttributionSourcePid = legacy2aidl_pid_t_int32_t(adjPid);
    if (!clientAttributionSourcePid.ok()) {
        return logIfErrorAndReturnStatus(BAD_VALUE,
                                         StringPrintf("%s: received invalid client attribution "
                                                      "source pid, pid: %d, sessionId: %d",
                                                      __func__, pid, sessionId),
                                         __func__);
    }
    mClientAttributionSource.pid = clientAttributionSourcePid.value();
    uid_t adjUid = uid;
    if (uid == -1 || (callingPid != myPid)) {
        adjUid = IPCThreadState::self()->getCallingUid();
    }
    auto clientAttributionSourceUid = legacy2aidl_uid_t_int32_t(adjUid);
    if (!clientAttributionSourceUid.ok()) {
        return logIfErrorAndReturnStatus(BAD_VALUE,
                                         StringPrintf("%s: received invalid client attribution "
                                                      "source uid, pid: %d, session id: %d",
                                                      __func__, pid, sessionId),
                                         __func__);
    }
    mClientAttributionSource.uid = clientAttributionSourceUid.value();

    mTracker.reset(new RecordingActivityTracker());

    sp<IBinder> binder = defaultServiceManager()->checkService(String16("audio"));
    if (binder != nullptr) {
        // Barrier to ensure runtime permission update propagates to audioflinger
        // Must be client-side
        interface_cast<IAudioManager>(binder)->getNativeInterface()->permissionUpdateBarrier();
    }

    mSelectedDeviceId = selectedDeviceId;
    mSelectedMicDirection = selectedMicDirection;
    mSelectedMicFieldDimension = microphoneFieldDimension;
    mMaxSharedAudioHistoryMs = maxSharedAudioHistoryMs;

    // Copy the state variables early so they are available for error reporting.
    if (pAttributes == nullptr) {
        mAttributes = AUDIO_ATTRIBUTES_INITIALIZER;
        mAttributes.source = inputSource;
        if (inputSource == AUDIO_SOURCE_VOICE_COMMUNICATION
                || inputSource == AUDIO_SOURCE_CAMCORDER) {
            mAttributes.flags = static_cast<audio_flags_mask_t>(
                    mAttributes.flags | AUDIO_FLAG_CAPTURE_PRIVATE);
        }
    } else {
        // stream type shouldn't be looked at, this track has audio attributes
        memcpy(&mAttributes, pAttributes, sizeof(audio_attributes_t));
        ALOGV("%s: Building AudioRecord with attributes: source=%d flags=0x%x tags=[%s]",
                __func__, mAttributes.source, mAttributes.flags, mAttributes.tags);
    }
    mSampleRate = sampleRate;
    if (format == AUDIO_FORMAT_DEFAULT) {
        format = AUDIO_FORMAT_PCM_16_BIT;
    }
    if (!audio_is_linear_pcm(format)) {
       // Compressed capture requires direct
       flags = (audio_input_flags_t) (flags | AUDIO_INPUT_FLAG_DIRECT);
       ALOGI("%s(): Format %#x is not linear pcm. Setting DIRECT, using flags %#x", __func__,
             format, flags);
    }
    mFormat = format;
    mChannelMask = channelMask;
    mSessionId = sessionId;
    ALOGV("%s: mSessionId %d", __func__, mSessionId);
    mOrigFlags = mFlags = flags;

    mTransfer = transferType;
    switch (mTransfer) {
    case TRANSFER_DEFAULT:
        if (callback == nullptr || threadCanCallJava) {
            mTransfer = TRANSFER_SYNC;
        } else {
            mTransfer = TRANSFER_CALLBACK;
        }
        break;
    case TRANSFER_CALLBACK:
        if (callback == nullptr) {
            return logIfErrorAndReturnStatus(
                    BAD_VALUE,
                    StringPrintf("%s: Transfer type TRANSFER_CALLBACK but callback == nullptr, "
                                 "pid: %d, session id: %d",
                                 __func__, pid, sessionId),
                    __func__);
        }
        break;
    case TRANSFER_OBTAIN:
    case TRANSFER_SYNC:
        break;
    default:
        return logIfErrorAndReturnStatus(
                BAD_VALUE,
                StringPrintf("%s: Invalid transfer type %d, pid: %d, session id: %d", __func__,
                             mTransfer, pid, sessionId),
                __func__);
    }

    // invariant that mAudioRecord != 0 is true only after set() returns successfully
    if (mAudioRecord != 0) {
        return logIfErrorAndReturnStatus(
                INVALID_OPERATION,
                StringPrintf("%s: Track already in use, pid: %d, session id: %d", __func__, pid,
                             sessionId),
                __func__);
    }

    if (!audio_is_valid_format(mFormat)) {
        return logIfErrorAndReturnStatus(
                BAD_VALUE,
                StringPrintf("%s: Format %#x is not valid, pid: %d, session id: %d", __func__,
                             mFormat, pid, sessionId),
                __func__);
    }

    if (!audio_is_input_channel(mChannelMask)) {
        return logIfErrorAndReturnStatus(
                BAD_VALUE,
                StringPrintf("%s: Invalid channel mask %#x, pid: %d, session id: %d", __func__,
                             mChannelMask, pid, sessionId),
                __func__);
    }

    mChannelCount = audio_channel_count_from_in_mask(mChannelMask);
    mFrameSize = audio_bytes_per_frame(mChannelCount, mFormat);

    // mFrameCount is initialized in createRecord_l
    mReqFrameCount = frameCount;

    mNotificationFramesReq = notificationFrames;
    // mNotificationFramesAct is initialized in createRecord_l

    mCallback = callback;
    if (mCallback != nullptr) {
        mAudioRecordThread = new AudioRecordThread(*this);
        mAudioRecordThread->run("AudioRecord", ANDROID_PRIORITY_AUDIO);
        // thread begins in paused state, and will not reference us until start()
    }

    // create the IAudioRecord
    {
        AutoMutex lock(mLock);
        status = createRecord_l(0 /*epoch*/);
    }

    ALOGV("%s(%d): status %d", __func__, mPortId, status);

    if (status != NO_ERROR) {
        if (mAudioRecordThread != 0) {
            mAudioRecordThread->requestExit();   // see comment in AudioRecord.h
            mAudioRecordThread->requestExitAndWait();
            mAudioRecordThread.clear();
        }
        // bypass error message to avoid logging twice (createRecord_l logs the error).
        mStatus = status;
        return mStatus;
    }

    // TODO: add audio hardware input latency here
    mLatency = (1000LL * mFrameCount) / mSampleRate;
    mMarkerPosition = 0;
    mMarkerReached = false;
    mNewPosition = 0;
    mUpdatePeriod = 0;
    AudioSystem::acquireAudioSessionId(mSessionId, adjPid, adjUid);
    mSequence = 1;
    mObservedSequence = mSequence;
    mInOverrun = false;
    mFramesRead = 0;
    mFramesReadServerOffset = 0;

    return logIfErrorAndReturnStatus(status, "", __func__);
}

// -------------------------------------------------------------------------

status_t AudioRecord::start(AudioSystem::sync_event_t event, audio_session_t triggerSession)
{
    const int64_t beginNs = systemTime();
    ALOGV("%s(%d): sync event %d trigger session %d", __func__, mPortId, event, triggerSession);
    AutoMutex lock(mLock);

    status_t status = NO_ERROR;
    mediametrics::Defer defer([&] {
        mediametrics::LogItem(mMetricsId)
            .set(AMEDIAMETRICS_PROP_CALLERNAME,
                    mCallerName.empty()
                    ? AMEDIAMETRICS_PROP_CALLERNAME_VALUE_UNKNOWN
                    : mCallerName.c_str())
            .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_START)
            .set(AMEDIAMETRICS_PROP_EXECUTIONTIMENS, (int64_t)(systemTime() - beginNs))
            .set(AMEDIAMETRICS_PROP_STATE, stateToString(mActive))
            .set(AMEDIAMETRICS_PROP_STATUS, (int32_t)status)
            .record(); });

    if (mActive) {
        return status;
    }

    // discard data in buffer
    const uint32_t framesFlushed = mProxy->flush();
    mFramesReadServerOffset -= mFramesRead + framesFlushed;
    mFramesRead = 0;
    mProxy->clearTimestamp();  // timestamp is invalid until next server push
    mPreviousTimestamp.clear();
    mTimestampRetrogradePositionReported = false;
    mTimestampRetrogradeTimeReported = false;

    // reset current position as seen by client to 0
    mProxy->setEpoch(mProxy->getEpoch() - mProxy->getPosition());
    // force refresh of remaining frames by processAudioBuffer() as last
    // read before stop could be partial.
    mRefreshRemaining = true;

    mNewPosition = mProxy->getPosition() + mUpdatePeriod;
    int32_t flags = android_atomic_acquire_load(&mCblk->mFlags);

    // we reactivate markers (mMarkerPosition != 0) as the position is reset to 0.
    // This is legacy behavior.  This is not done in stop() to avoid a race condition
    // where the last marker event is issued twice.
    mMarkerReached = false;
    // mActive is checked by restoreRecord_l
    mActive = true;

    if (!(flags & CBLK_INVALID)) {
        status = statusTFromBinderStatus(mAudioRecord->start(event, triggerSession));
        if (status == DEAD_OBJECT) {
            flags |= CBLK_INVALID;
        }
    }
    if (flags & CBLK_INVALID) {
        status = restoreRecord_l("start");
    }

    // Call these directly because we are already holding the lock.
    mAudioRecord->setPreferredMicrophoneDirection(mSelectedMicDirection);
    mAudioRecord->setPreferredMicrophoneFieldDimension(mSelectedMicFieldDimension);

    if (status != NO_ERROR) {
        mActive = false;
        ALOGE("%s(%d): status %d", __func__, mPortId, status);
        mMediaMetrics.markError(status, __FUNCTION__);
    } else {
        mTracker->recordingStarted();
        sp<AudioRecordThread> t = mAudioRecordThread;
        if (t != 0) {
            t->resume();
        } else {
            mPreviousPriority = getpriority(PRIO_PROCESS, 0);
            get_sched_policy(0, &mPreviousSchedulingGroup);
            androidSetThreadPriority(0, ANDROID_PRIORITY_AUDIO);
        }

        // we've successfully started, log that time
        mMediaMetrics.logStart(systemTime());
    }
    return status;
}

void AudioRecord::stop()
{
    const int64_t beginNs = systemTime();
    AutoMutex lock(mLock);
    mediametrics::Defer defer([&] {
        mediametrics::LogItem(mMetricsId)
            .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_STOP)
            .set(AMEDIAMETRICS_PROP_EXECUTIONTIMENS, (int64_t)(systemTime() - beginNs))
            .set(AMEDIAMETRICS_PROP_STATE, stateToString(mActive))
            .record(); });

    ALOGV("%s(%d): mActive:%d\n", __func__, mPortId, mActive);
    if (!mActive) {
        return;
    }

    mActive = false;
    mProxy->interrupt();
    mAudioRecord->stop();
    mTracker->recordingStopped();

    // Note: legacy handling - stop does not clear record marker and
    // periodic update position; we update those on start().

    sp<AudioRecordThread> t = mAudioRecordThread;
    if (t != 0) {
        t->pause();
    } else {
        setpriority(PRIO_PROCESS, 0, mPreviousPriority);
        set_sched_policy(0, mPreviousSchedulingGroup);
    }

    // we've successfully started, log that time
    mMediaMetrics.logStop(systemTime());
}

bool AudioRecord::stopped() const
{
    AutoMutex lock(mLock);
    return !mActive;
}

status_t AudioRecord::setMarkerPosition(uint32_t marker)
{
    AutoMutex lock(mLock);
    // The only purpose of setting marker position is to get a callback
    if (mCallback == nullptr) {
        return INVALID_OPERATION;
    }

    mMarkerPosition = marker;
    mMarkerReached = false;

    sp<AudioRecordThread> t = mAudioRecordThread;
    if (t != 0) {
        t->wake();
    }
    return NO_ERROR;
}

uint32_t AudioRecord::getHalSampleRate() const
{
    return mHalSampleRate;
}

uint32_t AudioRecord::getHalChannelCount() const
{
    return mHalChannelCount;
}

audio_format_t AudioRecord::getHalFormat() const
{
    return mHalFormat;
}

status_t AudioRecord::getMarkerPosition(uint32_t *marker) const
{
    if (marker == NULL) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    mMarkerPosition.getValue(marker);

    return NO_ERROR;
}

status_t AudioRecord::setPositionUpdatePeriod(uint32_t updatePeriod)
{
    AutoMutex lock(mLock);
    // The only purpose of setting position update period is to get a callback
    if (mCallback == nullptr) {
        return INVALID_OPERATION;
    }

    mNewPosition = mProxy->getPosition() + updatePeriod;
    mUpdatePeriod = updatePeriod;

    sp<AudioRecordThread> t = mAudioRecordThread;
    if (t != 0) {
        t->wake();
    }
    return NO_ERROR;
}

status_t AudioRecord::getPositionUpdatePeriod(uint32_t *updatePeriod) const
{
    if (updatePeriod == NULL) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    *updatePeriod = mUpdatePeriod;

    return NO_ERROR;
}

status_t AudioRecord::getPosition(uint32_t *position) const
{
    if (position == NULL) {
        return BAD_VALUE;
    }

    AutoMutex lock(mLock);
    mProxy->getPosition().getValue(position);

    return NO_ERROR;
}

uint32_t AudioRecord::getInputFramesLost() const
{
    // no need to check mActive, because if inactive this will return 0, which is what we want
    return AudioSystem::getInputFramesLost(getInputPrivate());
}

status_t AudioRecord::getTimestamp(ExtendedTimestamp *timestamp)
{
    if (timestamp == nullptr) {
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    status_t status = mProxy->getTimestamp(timestamp);
    if (status == OK) {
        timestamp->mPosition[ExtendedTimestamp::LOCATION_CLIENT] = mFramesRead;
        timestamp->mTimeNs[ExtendedTimestamp::LOCATION_CLIENT] = 0;
        if (!audio_is_linear_pcm(mFormat)) {
            // Don't do retrograde corrections or server offset if track is
            // compressed
            return OK;
        }
        // server side frame offset in case AudioRecord has been restored.
        for (int i = ExtendedTimestamp::LOCATION_SERVER;
                i < ExtendedTimestamp::LOCATION_MAX; ++i) {
            if (timestamp->mTimeNs[i] >= 0) {
                timestamp->mPosition[i] += mFramesReadServerOffset;
            }
        }

        bool timestampRetrogradeTimeReported = false;
        bool timestampRetrogradePositionReported = false;
        for (int i = 0; i < ExtendedTimestamp::LOCATION_MAX; ++i) {
            if (timestamp->mTimeNs[i] >= 0 && mPreviousTimestamp.mTimeNs[i] >= 0) {
                if (timestamp->mTimeNs[i] < mPreviousTimestamp.mTimeNs[i]) {
                    if (!mTimestampRetrogradeTimeReported) {
                        ALOGD("%s: retrograde time adjusting [%d] current:%lld to previous:%lld",
                                __func__, i, (long long)timestamp->mTimeNs[i],
                                (long long)mPreviousTimestamp.mTimeNs[i]);
                        timestampRetrogradeTimeReported = true;
                    }
                    timestamp->mTimeNs[i] = mPreviousTimestamp.mTimeNs[i];
                }
                if (timestamp->mPosition[i] < mPreviousTimestamp.mPosition[i]) {
                    if (!mTimestampRetrogradePositionReported) {
                        ALOGD("%s: retrograde position"
                                " adjusting [%d] current:%lld to previous:%lld",
                                __func__, i, (long long)timestamp->mPosition[i],
                                (long long)mPreviousTimestamp.mPosition[i]);
                        timestampRetrogradePositionReported = true;
                    }
                    timestamp->mPosition[i] = mPreviousTimestamp.mPosition[i];
                }
            }
        }
        mPreviousTimestamp = *timestamp;
        if (timestampRetrogradeTimeReported) {
            mTimestampRetrogradeTimeReported = true;
        }
        if (timestampRetrogradePositionReported) {
            mTimestampRetrogradePositionReported = true;
        }
    }
    return status;
}

// ---- Explicit Routing ---------------------------------------------------
status_t AudioRecord::setInputDevice(audio_port_handle_t deviceId) {
    AutoMutex lock(mLock);
    ALOGV("%s(%d): deviceId=%d mSelectedDeviceId=%d",
            __func__, mPortId, deviceId, mSelectedDeviceId);
    const int64_t beginNs = systemTime();
    mediametrics::Defer defer([&] {
        mediametrics::LogItem(mMetricsId)
                .set(AMEDIAMETRICS_PROP_CALLERNAME,
                     mCallerName.empty()
                     ? AMEDIAMETRICS_PROP_CALLERNAME_VALUE_UNKNOWN
                     : mCallerName.c_str())
                .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_SETPREFERREDDEVICE)
                .set(AMEDIAMETRICS_PROP_EXECUTIONTIMENS, (int64_t)(systemTime() - beginNs))
                .set(AMEDIAMETRICS_PROP_SELECTEDDEVICEID, (int32_t)deviceId)
                .record(); });

    if (mSelectedDeviceId != deviceId) {
        mSelectedDeviceId = deviceId;
        if (mStatus == NO_ERROR) {
            if (mActive) {
                if (getFirstDeviceId(mRoutedDeviceIds) != mSelectedDeviceId) {
                    // stop capture so that audio policy manager does not reject the new instance
                    // start request as only one capture can be active at a time.
                    if (mAudioRecord != 0) {
                        mAudioRecord->stop();
                    }
                    android_atomic_or(CBLK_INVALID, &mCblk->mFlags);
                    mProxy->interrupt();
                }
            } else {
                // if the track is idle, try to restore now and
                // defer to next start if not possible
                if (restoreRecord_l("setInputDevice") != OK) {
                    android_atomic_or(CBLK_INVALID, &mCblk->mFlags);
                }
            }
        }
    }
    return NO_ERROR;
}

audio_port_handle_t AudioRecord::getInputDevice() {
    AutoMutex lock(mLock);
    return mSelectedDeviceId;
}

// must be called with mLock held
void AudioRecord::updateRoutedDeviceIds_l()
{
    // if the record is inactive, do not update actual device as the input stream maybe routed
    // from a device not relevant to this client because of other active use cases.
    if (!mActive) {
        return;
    }
    if (mInput != AUDIO_IO_HANDLE_NONE) {
        DeviceIdVector deviceIds;
        status_t result = AudioSystem::getDeviceIdsForIo(mInput, deviceIds);
        if (result != OK) {
            ALOGW("%s: getDeviceIdsForIo returned: %d", __func__, result);
        }
        if (!deviceIds.empty()) {
            mRoutedDeviceIds = deviceIds;
        }
     }
}

DeviceIdVector AudioRecord::getRoutedDeviceIds() {
    AutoMutex lock(mLock);
    updateRoutedDeviceIds_l();
    return mRoutedDeviceIds;
}

status_t AudioRecord::dump(int fd, const Vector<String16>& args __unused) const
{
    String8 result;

    result.append(" AudioRecord::dump\n");
    result.appendFormat("  id(%d) status(%d), active(%d), session Id(%d)\n",
                        mPortId, mStatus, mActive, mSessionId);
    result.appendFormat("  flags(%#x), req. flags(%#x), audio source(%d)\n",
                        mFlags, mOrigFlags, mAttributes.source);
    result.appendFormat("  format(%#x), channel mask(%#x), channel count(%u), sample rate(%u)\n",
                  mFormat, mChannelMask, mChannelCount, mSampleRate);
    result.appendFormat("  frame count(%zu), req. frame count(%zu)\n",
                  mFrameCount, mReqFrameCount);
    result.appendFormat("  notif. frame count(%u), req. notif. frame count(%u)\n",
             mNotificationFramesAct, mNotificationFramesReq);
    result.appendFormat("  input(%d), latency(%u), selected device Id(%d)\n",
                        mInput, mLatency, mSelectedDeviceId);
    result.appendFormat("  routed device Ids(%s), mic direction(%d) mic field dimension(%f)",
                        toString(mRoutedDeviceIds).c_str(), mSelectedMicDirection,
                        mSelectedMicFieldDimension);
    ::write(fd, result.c_str(), result.size());
    return NO_ERROR;
}

// -------------------------------------------------------------------------
// TODO Move this macro to a common header file for enum to string conversion in audio framework.
#define MEDIA_CASE_ENUM(name) case name: return #name
const char * AudioRecord::convertTransferToText(transfer_type transferType) {
    switch (transferType) {
        MEDIA_CASE_ENUM(TRANSFER_DEFAULT);
        MEDIA_CASE_ENUM(TRANSFER_CALLBACK);
        MEDIA_CASE_ENUM(TRANSFER_OBTAIN);
        MEDIA_CASE_ENUM(TRANSFER_SYNC);
        default:
            return "UNRECOGNIZED";
    }
}

// must be called with mLock held
status_t AudioRecord::createRecord_l(const Modulo<uint32_t> &epoch)
{
    const int64_t beginNs = systemTime();
    const sp<IAudioFlinger>& audioFlinger = AudioSystem::get_audio_flinger();
    IAudioFlinger::CreateRecordInput input;
    IAudioFlinger::CreateRecordOutput output;
    [[maybe_unused]] audio_session_t originalSessionId;
    void *iMemPointer;
    audio_track_cblk_t* cblk;
    status_t status;
    static const int32_t kMaxCreateAttempts = 3;
    int32_t remainingAttempts = kMaxCreateAttempts;

    if (audioFlinger == 0) {
        return logIfErrorAndReturnStatus(
                NO_INIT, StringPrintf("%s(%d): Could not get audioflinger", __func__, mPortId), "");
    }

    // mFlags (not mOrigFlags) is modified depending on whether fast request is accepted.
    // After fast request is denied, we will request again if IAudioRecord is re-created.

    // Now that we have a reference to an I/O handle and have not yet handed it off to AudioFlinger,
    // we must release it ourselves if anything goes wrong.

    // Client can only express a preference for FAST.  Server will perform additional tests.
    if (mFlags & AUDIO_INPUT_FLAG_FAST) {
        bool useCaseAllowed =
            // any of these use cases:
            // use case 1: callback transfer mode
            (mTransfer == TRANSFER_CALLBACK) ||
            // use case 2: blocking read mode
            // The default buffer capacity at 48 kHz is 2048 frames, or ~42.6 ms.
            // That's enough for double-buffering with our standard 20 ms rule of thumb for
            // the minimum period of a non-SCHED_FIFO thread.
            // This is needed so that AAudio apps can do a low latency non-blocking read from a
            // callback running with SCHED_FIFO.
            (mTransfer == TRANSFER_SYNC) ||
            // use case 3: obtain/release mode
            (mTransfer == TRANSFER_OBTAIN);
        if (!useCaseAllowed) {
            ALOGD("%s(%d): AUDIO_INPUT_FLAG_FAST denied, incompatible transfer = %s",
                  __func__, mPortId,
                  convertTransferToText(mTransfer));
            mFlags = (audio_input_flags_t) (mFlags & ~(AUDIO_INPUT_FLAG_FAST |
                    AUDIO_INPUT_FLAG_RAW));
        }
    }

    input.attr = mAttributes;
    input.config.sample_rate = mSampleRate;
    input.config.channel_mask = mChannelMask;
    input.config.format = mFormat;
    input.clientInfo.attributionSource = mClientAttributionSource;
    input.clientInfo.clientTid = -1;
    if (mFlags & AUDIO_INPUT_FLAG_FAST) {
        if (mAudioRecordThread != 0) {
            input.clientInfo.clientTid = mAudioRecordThread->getTid();
        }
    }
    input.riid = mTracker->getRiid();

    input.flags = mFlags;
    // The notification frame count is the period between callbacks, as suggested by the client
    // but moderated by the server.  For record, the calculations are done entirely on server side.
    input.frameCount = mReqFrameCount;
    input.notificationFrameCount = mNotificationFramesReq;
    input.selectedDeviceId = mSelectedDeviceId;
    input.sessionId = mSessionId;
    originalSessionId = mSessionId;
    input.maxSharedAudioHistoryMs = mMaxSharedAudioHistoryMs;

    do {
        media::CreateRecordResponse response;
        auto aidlInput = input.toAidl();
        if (!aidlInput.ok()) {
            return logIfErrorAndReturnStatus(
                    BAD_VALUE,
                    StringPrintf("%s(%d): Could not create record due to invalid input", __func__,
                                 mPortId),
                    "");
        }
        status = audioFlinger->createRecord(aidlInput.value(), response);

        auto recordOutput = IAudioFlinger::CreateRecordOutput::fromAidl(response);
        if (!recordOutput.ok()) {
            return logIfErrorAndReturnStatus(
                    BAD_VALUE,
                    StringPrintf("%s(%d): Could not create record output due to invalid response",
                                 __func__, mPortId),
                    "");
        }
        output = recordOutput.value();
        if (status == NO_ERROR) {
            break;
        }
        if (status != FAILED_TRANSACTION || --remainingAttempts <= 0) {
            return logIfErrorAndReturnStatus(
                    status,
                    StringPrintf("%s(%d): AudioFlinger could not create record track, status: %d",
                                 __func__, mPortId, status),
                    "");
        }
        // FAILED_TRANSACTION happens under very specific conditions causing a state mismatch
        // between audio policy manager and audio flinger during the input stream open sequence
        // and can be recovered by retrying.
        // Leave time for race condition to clear before retrying and randomize delay
        // to reduce the probability of concurrent retries in locked steps.
        usleep((20 + rand() % 30) * 10000);
    } while (1);

    ALOG_ASSERT(output.audioRecord != 0);

    // AudioFlinger now owns the reference to the I/O handle,
    // so we are no longer responsible for releasing it.

    mAwaitBoost = false;
    if (output.flags & AUDIO_INPUT_FLAG_FAST) {
        ALOGI("%s(%d): AUDIO_INPUT_FLAG_FAST successful; frameCount %zu -> %zu",
              __func__, mPortId,
              mReqFrameCount, output.frameCount);
        mAwaitBoost = true;
    }
    mFlags = output.flags;
    mRoutedDeviceIds = { output.selectedDeviceId };
    mSessionId = output.sessionId;
    mSampleRate = output.sampleRate;
    mServerConfig = output.serverConfig;
    mServerFrameSize = audio_bytes_per_frame(
            audio_channel_count_from_in_mask(mServerConfig.channel_mask), mServerConfig.format);
    mServerSampleSize = audio_bytes_per_sample(mServerConfig.format);
    mHalSampleRate = output.halConfig.sample_rate;
    mHalChannelCount = audio_channel_count_from_in_mask(output.halConfig.channel_mask);
    mHalFormat = output.halConfig.format;

    if (output.cblk == 0) {
        return logIfErrorAndReturnStatus(
                NO_INIT, StringPrintf("%s(%d): Could not get control block", __func__, mPortId),
                "");
    }
    // TODO: Using unsecurePointer() has some associated security pitfalls
    //       (see declaration for details).
    //       Either document why it is safe in this case or address the
    //       issue (e.g. by copying).
    iMemPointer = output.cblk ->unsecurePointer();
    if (iMemPointer == NULL) {
        return logIfErrorAndReturnStatus(
                NO_INIT,
                StringPrintf("%s(%d): Could not get control block pointer", __func__, mPortId), "");
    }
    cblk = static_cast<audio_track_cblk_t*>(iMemPointer);

    // Starting address of buffers in shared memory.
    // The buffers are either immediately after the control block,
    // or in a separate area at discretion of server.
    void *buffers;
    if (output.buffers == 0) {
        buffers = cblk + 1;
    } else {
        // TODO: Using unsecurePointer() has some associated security pitfalls
        //       (see declaration for details).
        //       Either document why it is safe in this case or address the
        //       issue (e.g. by copying).
        buffers = output.buffers->unsecurePointer();
        if (buffers == NULL) {
            return logIfErrorAndReturnStatus(
                    NO_INIT,
                    StringPrintf("%s(%d): Could not get buffer pointer", __func__, mPortId), "");
        }
    }

    // invariant that mAudioRecord != 0 is true only after set() returns successfully
    if (mAudioRecord != 0) {
        IInterface::asBinder(mAudioRecord)->unlinkToDeath(mDeathNotifier, this);
        mDeathNotifier.clear();
    }
    mAudioRecord = output.audioRecord;
    mCblkMemory = output.cblk;
    mBufferMemory = output.buffers;
    IPCThreadState::self()->flushCommands();

    mCblk = cblk;
    // note that output.frameCount is the (possibly revised) value of mReqFrameCount
    if (output.frameCount < mReqFrameCount || (mReqFrameCount == 0 && output.frameCount == 0)) {
        ALOGW("%s(%d): Requested frameCount %zu but received frameCount %zu",
              __func__, output.portId,
              mReqFrameCount,  output.frameCount);
    }

    // Make sure that application is notified with sufficient margin before overrun.
    // The computation is done on server side.
    if (mNotificationFramesReq > 0 && output.notificationFrameCount != mNotificationFramesReq) {
        ALOGW("%s(%d): Server adjusted notificationFrames from %u to %zu for frameCount %zu",
                __func__, output.portId,
                mNotificationFramesReq, output.notificationFrameCount, output.frameCount);
    }
    mNotificationFramesAct = (uint32_t)output.notificationFrameCount;
    if (mServerConfig.format != mFormat && mCallback != nullptr) {
        mFormatConversionBufRaw = std::make_unique<uint8_t[]>(mNotificationFramesAct * mFrameSize);
        mFormatConversionBuffer.raw = mFormatConversionBufRaw.get();
    }

    //mInput != input includes the case where mInput == AUDIO_IO_HANDLE_NONE for first creation
    if (mDeviceCallback != 0) {
        if (mInput != AUDIO_IO_HANDLE_NONE) {
            AudioSystem::removeAudioDeviceCallback(this, mInput, mPortId);
        }
        AudioSystem::addAudioDeviceCallback(this, output.inputId, output.portId);
    }

    if (!mSharedAudioPackageName.empty()) {
        mAudioRecord->shareAudioHistory(mSharedAudioPackageName, mSharedAudioStartMs);
    }

    mPortId = output.portId;
    // We retain a copy of the I/O handle, but don't own the reference
    mInput = output.inputId;
    mRefreshRemaining = true;

    mFrameCount = output.frameCount;
    // If IAudioRecord is re-created, don't let the requested frameCount
    // decrease.  This can confuse clients that cache frameCount().
    if (mFrameCount > mReqFrameCount) {
        mReqFrameCount = mFrameCount;
    }

    // update proxy
    mProxy = new AudioRecordClientProxy(cblk, buffers, mFrameCount, mServerFrameSize);
    mProxy->setEpoch(epoch);
    mProxy->setMinimum(mNotificationFramesAct);

    mDeathNotifier = new DeathNotifier(this);
    IInterface::asBinder(mAudioRecord)->linkToDeath(mDeathNotifier, this);

    mMetricsId = std::string(AMEDIAMETRICS_KEY_PREFIX_AUDIO_RECORD) + std::to_string(mPortId);
    mediametrics::LogItem(mMetricsId)
        .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_CREATE)
        .set(AMEDIAMETRICS_PROP_EXECUTIONTIMENS, (int64_t)(systemTime() - beginNs))
        // the following are immutable (at least until restore)
        .set(AMEDIAMETRICS_PROP_FLAGS, toString(mFlags).c_str())
        .set(AMEDIAMETRICS_PROP_ORIGINALFLAGS, toString(mOrigFlags).c_str())
        .set(AMEDIAMETRICS_PROP_SESSIONID, (int32_t)mSessionId)
        .set(AMEDIAMETRICS_PROP_TRACKID, mPortId)
        .set(AMEDIAMETRICS_PROP_LOGSESSIONID, mLogSessionId)
        .set(AMEDIAMETRICS_PROP_SOURCE, toString(mAttributes.source).c_str())
        .set(AMEDIAMETRICS_PROP_THREADID, (int32_t)output.inputId)
        .set(AMEDIAMETRICS_PROP_SELECTEDDEVICEID, (int32_t)mSelectedDeviceId)
        .set(AMEDIAMETRICS_PROP_ROUTEDDEVICEID, (int32_t)(getFirstDeviceId(mRoutedDeviceIds)))
        .set(AMEDIAMETRICS_PROP_ROUTEDDEVICEIDS, toString(mRoutedDeviceIds).c_str())
        .set(AMEDIAMETRICS_PROP_ENCODING, toString(mFormat).c_str())
        .set(AMEDIAMETRICS_PROP_CHANNELMASK, (int32_t)mChannelMask)
        .set(AMEDIAMETRICS_PROP_FRAMECOUNT, (int32_t)mFrameCount)
        .set(AMEDIAMETRICS_PROP_SAMPLERATE, (int32_t)mSampleRate)
        // the following are NOT immutable
        .set(AMEDIAMETRICS_PROP_STATE, stateToString(mActive))
        .set(AMEDIAMETRICS_PROP_STATUS, (int32_t)status)
        .set(AMEDIAMETRICS_PROP_SELECTEDMICDIRECTION, (int32_t)mSelectedMicDirection)
        .set(AMEDIAMETRICS_PROP_SELECTEDMICFIELDDIRECTION, (double)mSelectedMicFieldDimension)
        .record();

    // sp<IAudioTrack> track destructor will cause releaseOutput() to be called by AudioFlinger
    return logIfErrorAndReturnStatus(status, "", "");
}

// Report error associated with the event and some configuration details.
void AudioRecord::reportError(status_t status, const char *event, const char *message) const
{
    if (status == NO_ERROR) return;
    // We report error on the native side because some callers do not come
    // from Java.
    // Ensure these variables are initialized in set().
    mediametrics::LogItem(AMEDIAMETRICS_KEY_AUDIO_RECORD_ERROR)
        .set(AMEDIAMETRICS_PROP_EVENT, event)
        .set(AMEDIAMETRICS_PROP_STATUS, (int32_t)status)
        .set(AMEDIAMETRICS_PROP_STATUSMESSAGE, message)
        .set(AMEDIAMETRICS_PROP_ORIGINALFLAGS, toString(mOrigFlags).c_str())
        .set(AMEDIAMETRICS_PROP_SESSIONID, (int32_t)mSessionId)
        .set(AMEDIAMETRICS_PROP_SOURCE, toString(mAttributes.source).c_str())
        .set(AMEDIAMETRICS_PROP_SELECTEDDEVICEID, (int32_t)mSelectedDeviceId)
        .set(AMEDIAMETRICS_PROP_ENCODING, toString(mFormat).c_str())
        .set(AMEDIAMETRICS_PROP_CHANNELMASK, (int32_t)mChannelMask)
        .set(AMEDIAMETRICS_PROP_FRAMECOUNT, (int32_t)mFrameCount)
        .set(AMEDIAMETRICS_PROP_SAMPLERATE, (int32_t)mSampleRate)
        .record();
}

status_t AudioRecord::obtainBuffer(Buffer* audioBuffer, int32_t waitCount, size_t *nonContig)
{
    if (audioBuffer == NULL) {
        if (nonContig != NULL) {
            *nonContig = 0;
        }
        return BAD_VALUE;
    }
    if (mTransfer != TRANSFER_OBTAIN) {
        audioBuffer->frameCount = 0;
        audioBuffer->mSize = 0;
        audioBuffer->raw = NULL;
        if (nonContig != NULL) {
            *nonContig = 0;
        }
        return INVALID_OPERATION;
    }

    const struct timespec *requested;
    struct timespec timeout;
    if (waitCount == -1) {
        requested = &ClientProxy::kForever;
    } else if (waitCount == 0) {
        requested = &ClientProxy::kNonBlocking;
    } else if (waitCount > 0) {
        time_t ms = WAIT_PERIOD_MS * (time_t) waitCount;
        timeout.tv_sec = ms / 1000;
        timeout.tv_nsec = (long) (ms % 1000) * 1000000;
        requested = &timeout;
    } else {
        ALOGE("%s(%d): invalid waitCount %d", __func__, mPortId, waitCount);
        requested = NULL;
    }
    return obtainBuffer(audioBuffer, requested, NULL /*elapsed*/, nonContig);
}

status_t AudioRecord::obtainBuffer(Buffer* audioBuffer, const struct timespec *requested,
        struct timespec *elapsed, size_t *nonContig)
{
    // previous and new IAudioRecord sequence numbers are used to detect track re-creation
    uint32_t oldSequence = 0;

    Proxy::Buffer buffer;
    status_t status = NO_ERROR;

    static const int32_t kMaxTries = 5;
    int32_t tryCounter = kMaxTries;

    do {
        // obtainBuffer() is called with mutex unlocked, so keep extra references to these fields to
        // keep them from going away if another thread re-creates the track during obtainBuffer()
        sp<AudioRecordClientProxy> proxy;
        sp<IMemory> iMem;
        sp<IMemory> bufferMem;
        {
            // start of lock scope
            AutoMutex lock(mLock);

            // did previous obtainBuffer() fail due to media server death or voluntary invalidation?
            if (status == DEAD_OBJECT) {
                // re-create track, unless someone else has already done so
                if (mSequence == oldSequence) {
                    if (!audio_is_linear_pcm(mFormat)) {
                        // If compressed capture, don't attempt to restore the track.
                        // Return a DEAD_OBJECT error and let the caller recreate.
                        tryCounter = 0;
                    } else {
                        status = restoreRecord_l("obtainBuffer");
                    }
                    if (status != NO_ERROR) {
                        buffer.mFrameCount = 0;
                        buffer.mRaw = NULL;
                        buffer.mNonContig = 0;
                        break;
                    }
                }
            }
            oldSequence = mSequence;

            // Keep the extra references
            proxy = mProxy;
            iMem = mCblkMemory;
            bufferMem = mBufferMemory;

            // Non-blocking if track is stopped
            if (!mActive) {
                requested = &ClientProxy::kNonBlocking;
            }

        }   // end of lock scope

        buffer.mFrameCount = audioBuffer->frameCount;
        // FIXME starts the requested timeout and elapsed over from scratch
        status = proxy->obtainBuffer(&buffer, requested, elapsed);

    } while ((status == DEAD_OBJECT) && (tryCounter-- > 0));

    audioBuffer->frameCount = buffer.mFrameCount;
    audioBuffer->mSize = buffer.mFrameCount * mServerFrameSize;
    audioBuffer->raw = buffer.mRaw;
    audioBuffer->sequence = oldSequence;
    if (nonContig != NULL) {
        *nonContig = buffer.mNonContig;
    }
    return status;
}

void AudioRecord::releaseBuffer(const Buffer* audioBuffer)
{
    // FIXME add error checking on mode, by adding an internal version

    size_t stepCount = audioBuffer->frameCount;
    if (stepCount == 0) {
        return;
    }

    Proxy::Buffer buffer;
    buffer.mFrameCount = stepCount;
    buffer.mRaw = audioBuffer->raw;

    AutoMutex lock(mLock);
    if (audioBuffer->sequence != mSequence) {
        // This Buffer came from a different IAudioRecord instance, so ignore the releaseBuffer
        ALOGD("%s is no-op due to IAudioRecord sequence mismatch %u != %u",
                __func__, audioBuffer->sequence, mSequence);
        return;
    }
    mInOverrun = false;
    mProxy->releaseBuffer(&buffer);

    // the server does not automatically disable recorder on overrun, so no need to restart
}

audio_io_handle_t AudioRecord::getInputPrivate() const
{
    AutoMutex lock(mLock);
    return mInput;
}

status_t AudioRecord::setParameters(const String8& keyValuePairs) {
    AutoMutex lock(mLock);
    if (mInput == AUDIO_IO_HANDLE_NONE || mAudioRecord == nullptr) {
        return NO_INIT;
    }
    return statusTFromBinderStatus(mAudioRecord->setParameters(keyValuePairs.c_str()));
}

String8 AudioRecord::getParameters(const String8& keys) {
    AutoMutex lock(mLock);
    return mInput != AUDIO_IO_HANDLE_NONE
               ? AudioSystem::getParameters(mInput, keys)
               : String8();
}

// -------------------------------------------------------------------------

ssize_t AudioRecord::read(void* buffer, size_t userSize, bool blocking)
{
    if (mTransfer != TRANSFER_SYNC) {
        return INVALID_OPERATION;
    }

    if (ssize_t(userSize) < 0 || (buffer == NULL && userSize != 0)) {
        // Validation. user is most-likely passing an error code, and it would
        // make the return value ambiguous (actualSize vs error).
        ALOGE("%s(%d) (buffer=%p, size=%zu (%zu)",
                __func__, mPortId, buffer, userSize, userSize);
        return BAD_VALUE;
    }

    ssize_t read = 0;
    Buffer audioBuffer;

    while (userSize >= mFrameSize) {
        audioBuffer.frameCount = userSize / mFrameSize;

        status_t err = obtainBuffer(&audioBuffer,
                blocking ? &ClientProxy::kForever : &ClientProxy::kNonBlocking);
        if (err < 0) {
            if (read > 0) {
                break;
            }
            if (err == TIMED_OUT || err == -EINTR) {
                err = WOULD_BLOCK;
            }
            return ssize_t(err);
        }

        size_t bytesRead = audioBuffer.frameCount * mFrameSize;
        if (audio_is_linear_pcm(mFormat)) {
            memcpy_by_audio_format(buffer, mFormat, audioBuffer.raw, mServerConfig.format,
                                audioBuffer.mSize / mServerSampleSize);
        } else {
            memcpy(buffer, audioBuffer.raw, audioBuffer.mSize);
        }
        buffer = ((char *) buffer) + bytesRead;
        userSize -= bytesRead;
        read += bytesRead;

        releaseBuffer(&audioBuffer);
    }
    if (read > 0) {
        mFramesRead += read / mFrameSize;
        // mFramesReadTime = systemTime(SYSTEM_TIME_MONOTONIC); // not provided at this time.
    }
    return read;
}

// -------------------------------------------------------------------------

nsecs_t AudioRecord::processAudioBuffer()
{
    mLock.lock();
    const sp<IAudioRecordCallback> callback = mCallback.promote();
    if (!callback) {
        mCallback = nullptr;
        mLock.unlock();
        return NS_NEVER;
    }
    if (mAwaitBoost) {
        mAwaitBoost = false;
        mLock.unlock();
        static const int32_t kMaxTries = 5;
        int32_t tryCounter = kMaxTries;
        uint32_t pollUs = 10000;
        do {
            int policy = sched_getscheduler(0) & ~SCHED_RESET_ON_FORK;
            if (policy == SCHED_FIFO || policy == SCHED_RR) {
                break;
            }
            usleep(pollUs);
            pollUs <<= 1;
        } while (tryCounter-- > 0);
        if (tryCounter < 0) {
            ALOGE("%s(%d): did not receive expected priority boost on time", __func__, mPortId);
        }
        // Run again immediately
        return 0;
    }

    // Can only reference mCblk while locked
    int32_t flags = android_atomic_and(~CBLK_OVERRUN, &mCblk->mFlags);

    // Check for track invalidation
    if (flags & CBLK_INVALID) {
        (void) restoreRecord_l("processAudioBuffer");
        mLock.unlock();
        // Run again immediately, but with a new IAudioRecord
        return 0;
    }

    bool active = mActive;

    // Manage overrun callback, must be done under lock to avoid race with releaseBuffer()
    bool newOverrun = false;
    if (flags & CBLK_OVERRUN) {
        if (!mInOverrun) {
            mInOverrun = true;
            newOverrun = true;
        }
    }

    // Get current position of server
    Modulo<uint32_t> position(mProxy->getPosition());

    // Manage marker callback
    bool markerReached = false;
    Modulo<uint32_t> markerPosition(mMarkerPosition);
    // FIXME fails for wraparound, need 64 bits
    if (!mMarkerReached && markerPosition.value() > 0 && position >= markerPosition) {
        mMarkerReached = markerReached = true;
    }

    // Determine the number of new position callback(s) that will be needed, while locked
    size_t newPosCount = 0;
    Modulo<uint32_t> newPosition(mNewPosition);
    uint32_t updatePeriod = mUpdatePeriod;
    // FIXME fails for wraparound, need 64 bits
    if (updatePeriod > 0 && position >= newPosition) {
        newPosCount = ((position - newPosition).value() / updatePeriod) + 1;
        mNewPosition += updatePeriod * newPosCount;
    }

    // Cache other fields that will be needed soon
    uint32_t notificationFrames = mNotificationFramesAct;
    if (mRefreshRemaining) {
        mRefreshRemaining = false;
        mRemainingFrames = notificationFrames;
        mRetryOnPartialBuffer = false;
    }
    size_t misalignment = mProxy->getMisalignment();
    uint32_t sequence = mSequence;

    // These fields don't need to be cached, because they are assigned only by set():
    //      mTransfer, mCallback, mUserData, mSampleRate, mFrameSize

    mLock.unlock();

    // perform callbacks while unlocked
    if (newOverrun) {
        callback->onOverrun();

    }
    if (markerReached) {
        callback->onMarker(markerPosition.value());
    }
    while (newPosCount > 0) {
        callback->onNewPos(newPosition.value());
        newPosition += updatePeriod;
        newPosCount--;
    }
    if (mObservedSequence != sequence) {
        mObservedSequence = sequence;
        callback->onNewIAudioRecord();
    }

    // if inactive, then don't run me again until re-started
    if (!active) {
        return NS_INACTIVE;
    }

    // Compute the estimated time until the next timed event (position, markers)
    uint32_t minFrames = ~0;
    if (!markerReached && position < markerPosition) {
        minFrames = (markerPosition - position).value();
    }
    if (updatePeriod > 0) {
        uint32_t remaining = (newPosition - position).value();
        if (remaining < minFrames) {
            minFrames = remaining;
        }
    }

    // If > 0, poll periodically to recover from a stuck server.  A good value is 2.
    static const uint32_t kPoll = 0;
    if (kPoll > 0 && mTransfer == TRANSFER_CALLBACK && kPoll * notificationFrames < minFrames) {
        minFrames = kPoll * notificationFrames;
    }

    // Convert frame units to time units
    nsecs_t ns = NS_WHENEVER;
    if (minFrames != (uint32_t) ~0) {
        // This "fudge factor" avoids soaking CPU, and compensates for late progress by server
        static const nsecs_t kFudgeNs = 10000000LL; // 10 ms
        ns = ((minFrames * 1000000000LL) / mSampleRate) + kFudgeNs;
    }

    // If not supplying data by EVENT_MORE_DATA, then we're done
    if (mTransfer != TRANSFER_CALLBACK) {
        return ns;
    }

    struct timespec timeout;
    const struct timespec *requested = &ClientProxy::kForever;
    if (ns != NS_WHENEVER) {
        timeout.tv_sec = ns / 1000000000LL;
        timeout.tv_nsec = ns % 1000000000LL;
        ALOGV("%s(%d): timeout %ld.%03d",
                __func__, mPortId, timeout.tv_sec, (int) timeout.tv_nsec / 1000000);
        requested = &timeout;
    }

    size_t readFrames = 0;
    while (mRemainingFrames > 0) {

        Buffer audioBuffer;
        audioBuffer.frameCount = mRemainingFrames;
        size_t nonContig;
        status_t err = obtainBuffer(&audioBuffer, requested, NULL, &nonContig);
        LOG_ALWAYS_FATAL_IF((err != NO_ERROR) != (audioBuffer.frameCount == 0),
                "%s(%d): obtainBuffer() err=%d frameCount=%zu",
                __func__, mPortId, err, audioBuffer.frameCount);
        requested = &ClientProxy::kNonBlocking;
        size_t avail = audioBuffer.frameCount + nonContig;
        ALOGV("%s(%d): obtainBuffer(%u) returned %zu = %zu + %zu err %d",
                __func__, mPortId, mRemainingFrames, avail, audioBuffer.frameCount, nonContig, err);
        if (err != NO_ERROR) {
            if (err == TIMED_OUT || err == WOULD_BLOCK || err == -EINTR) {
                break;
            }
            ALOGE("%s(%d): Error %d obtaining an audio buffer, giving up.",
                    __func__, mPortId, err);
            return NS_NEVER;
        }

        if (mRetryOnPartialBuffer) {
            mRetryOnPartialBuffer = false;
            if (avail < mRemainingFrames) {
                int64_t myns = ((mRemainingFrames - avail) *
                        1100000000LL) / mSampleRate;
                if (ns < 0 || myns < ns) {
                    ns = myns;
                }
                return ns;
            }
        }

        Buffer* buffer = &audioBuffer;
        if (mServerConfig.format != mFormat) {
            buffer = &mFormatConversionBuffer;
            buffer->frameCount = audioBuffer.frameCount;
            buffer->mSize = buffer->frameCount * mFrameSize;
            buffer->sequence = audioBuffer.sequence;
            memcpy_by_audio_format(buffer->raw, mFormat, audioBuffer.raw,
                                   mServerConfig.format, audioBuffer.size() / mServerSampleSize);
        }

        const size_t reqSize = buffer->size();
        const size_t readSize = callback->onMoreData(*buffer);
        buffer->mSize = readSize;

        // Validate on returned size
        if (ssize_t(readSize) < 0 || readSize > reqSize) {
            ALOGE("%s(%d):  EVENT_MORE_DATA requested %zu bytes but callback returned %zd bytes",
                    __func__, mPortId, reqSize, ssize_t(readSize));
            return NS_NEVER;
        }

        if (readSize == 0) {
            // The callback is done consuming buffers
            // Keep this thread going to handle timed events and
            // still try to provide more data in intervals of WAIT_PERIOD_MS
            // but don't just loop and block the CPU, so wait
            return WAIT_PERIOD_MS * 1000000LL;
        }

        size_t releasedFrames = readSize / mFrameSize;
        audioBuffer.frameCount = releasedFrames;
        mRemainingFrames -= releasedFrames;
        if (misalignment >= releasedFrames) {
            misalignment -= releasedFrames;
        } else {
            misalignment = 0;
        }

        releaseBuffer(&audioBuffer);
        readFrames += releasedFrames;

        // FIXME here is where we would repeat EVENT_MORE_DATA again on same advanced buffer
        // if callback doesn't like to accept the full chunk
        if (readSize < reqSize) {
            continue;
        }

        // There could be enough non-contiguous frames available to satisfy the remaining request
        if (mRemainingFrames <= nonContig) {
            continue;
        }

#if 0
        // This heuristic tries to collapse a series of EVENT_MORE_DATA that would total to a
        // sum <= notificationFrames.  It replaces that series by at most two EVENT_MORE_DATA
        // that total to a sum == notificationFrames.
        if (0 < misalignment && misalignment <= mRemainingFrames) {
            mRemainingFrames = misalignment;
            return (mRemainingFrames * 1100000000LL) / mSampleRate;
        }
#endif

    }
    if (readFrames > 0) {
        AutoMutex lock(mLock);
        mFramesRead += readFrames;
        // mFramesReadTime = systemTime(SYSTEM_TIME_MONOTONIC); // not provided at this time.
    }
    mRemainingFrames = notificationFrames;
    mRetryOnPartialBuffer = true;

    // A lot has transpired since ns was calculated, so run again immediately and re-calculate
    return 0;
}

status_t AudioRecord::restoreRecord_l(const char *from)
{
    status_t result = NO_ERROR;  // logged: make sure to set this before returning.
    const int64_t beginNs = systemTime();
    mediametrics::Defer defer([&] {
        mediametrics::LogItem(mMetricsId)
            .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_RESTORE)
            .set(AMEDIAMETRICS_PROP_EXECUTIONTIMENS, (int64_t)(systemTime() - beginNs))
            .set(AMEDIAMETRICS_PROP_STATE, stateToString(mActive))
            .set(AMEDIAMETRICS_PROP_STATUS, (int32_t)result)
            .set(AMEDIAMETRICS_PROP_WHERE, from)
            .record(); });

    ALOGW("%s(%d) called from %s()", __func__, mPortId, from);
    ++mSequence;

    const int INITIAL_RETRIES = 3;
    int retries = INITIAL_RETRIES;
retry:
    mFlags = mOrigFlags;

    // if the new IAudioRecord is created, createRecord_l() will modify the
    // following member variables: mAudioRecord, mCblkMemory, mCblk, mBufferMemory.
    // It will also delete the strong references on previous IAudioRecord and IMemory
    Modulo<uint32_t> position(mProxy->getPosition());
    mNewPosition = position + mUpdatePeriod;
    result = createRecord_l(position);

    if (result == NO_ERROR) {
        if (mActive) {
            // callback thread or sync event hasn't changed
            // FIXME this fails if we have a new AudioFlinger instance
            result = statusTFromBinderStatus(mAudioRecord->start(
                AudioSystem::SYNC_EVENT_SAME, AUDIO_SESSION_NONE));
        }
        mFramesReadServerOffset = mFramesRead; // server resets to zero so we need an offset.
    }

    if (result != NO_ERROR) {
        ALOGW("%s(%d): failed status %d, retries %d", __func__, mPortId, result, retries);
        if (--retries > 0) {
            // leave time for an eventual race condition to clear before retrying
            usleep(500000);
            goto retry;
        }
        // if no retries left, set invalid bit to force restoring at next occasion
        // and avoid inconsistent active state on client and server sides
        if (mCblk != nullptr) {
            android_atomic_or(CBLK_INVALID, &mCblk->mFlags);
        }
    }

    return result;
}

status_t AudioRecord::addAudioDeviceCallback(const sp<AudioSystem::AudioDeviceCallback>& callback)
{
    if (callback == 0) {
        ALOGW("%s(%d): adding NULL callback!", __func__, mPortId);
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    if (mDeviceCallback.unsafe_get() == callback.get()) {
        ALOGW("%s(%d): adding same callback!", __func__, mPortId);
        return INVALID_OPERATION;
    }
    status_t status = NO_ERROR;
    if (mInput != AUDIO_IO_HANDLE_NONE) {
        if (mDeviceCallback != 0) {
            ALOGW("%s(%d): callback already present!", __func__, mPortId);
            AudioSystem::removeAudioDeviceCallback(this, mInput, mPortId);
        }
        status = AudioSystem::addAudioDeviceCallback(this, mInput, mPortId);
    }
    mDeviceCallback = callback;
    return status;
}

status_t AudioRecord::removeAudioDeviceCallback(
        const sp<AudioSystem::AudioDeviceCallback>& callback)
{
    if (callback == 0) {
        ALOGW("%s(%d): removing NULL callback!", __func__, mPortId);
        return BAD_VALUE;
    }
    AutoMutex lock(mLock);
    if (mDeviceCallback.unsafe_get() != callback.get()) {
        ALOGW("%s(%d): removing different callback!", __func__, mPortId);
        return INVALID_OPERATION;
    }
    mDeviceCallback.clear();
    if (mInput != AUDIO_IO_HANDLE_NONE) {
        AudioSystem::removeAudioDeviceCallback(this, mInput, mPortId);
    }
    return NO_ERROR;
}

void AudioRecord::onAudioDeviceUpdate(audio_io_handle_t audioIo,
                                      const DeviceIdVector& deviceIds)
{
    sp<AudioSystem::AudioDeviceCallback> callback;
    {
        AutoMutex lock(mLock);
        if (audioIo != mInput) {
            return;
        }
        callback = mDeviceCallback.promote();
        // only update device if the record is active as route changes due to other use cases are
        // irrelevant for this client
        if (mActive) {
            mRoutedDeviceIds = deviceIds;
        }
    }
    if (callback.get() != nullptr) {
        callback->onAudioDeviceUpdate(mInput, mRoutedDeviceIds);
    }
}

// -------------------------------------------------------------------------

status_t AudioRecord::getActiveMicrophones(std::vector<media::MicrophoneInfoFw>* activeMicrophones)
{
    AutoMutex lock(mLock);
    return statusTFromBinderStatus(mAudioRecord->getActiveMicrophones(activeMicrophones));
}

status_t AudioRecord::setPreferredMicrophoneDirection(audio_microphone_direction_t direction)
{
    AutoMutex lock(mLock);
    if (mSelectedMicDirection == direction) {
        // NOP
        return OK;
    }

    mSelectedMicDirection = direction;
    if (mAudioRecord == 0) {
        // the internal AudioRecord hasn't be created yet, so just stash the attribute.
        return OK;
    } else {
        return statusTFromBinderStatus(mAudioRecord->setPreferredMicrophoneDirection(direction));
    }
}

status_t AudioRecord::setPreferredMicrophoneFieldDimension(float zoom) {
    AutoMutex lock(mLock);
    if (mSelectedMicFieldDimension == zoom) {
        // NOP
        return OK;
    }

    mSelectedMicFieldDimension = zoom;
    if (mAudioRecord == 0) {
        // the internal AudioRecord hasn't be created yet, so just stash the attribute.
        return OK;
    } else {
        return statusTFromBinderStatus(mAudioRecord->setPreferredMicrophoneFieldDimension(zoom));
    }
}

void AudioRecord::setLogSessionId(const char *logSessionId)
{
    AutoMutex lock(mLock);
    if (logSessionId == nullptr) logSessionId = "";  // an empty string is an unset session id.
    if (mLogSessionId == logSessionId) return;

     mLogSessionId = logSessionId;
     mediametrics::LogItem(mMetricsId)
         .set(AMEDIAMETRICS_PROP_EVENT, AMEDIAMETRICS_PROP_EVENT_VALUE_SETLOGSESSIONID)
         .set(AMEDIAMETRICS_PROP_LOGSESSIONID, logSessionId)
         .record();
}

status_t AudioRecord::shareAudioHistory(const std::string& sharedPackageName,
                                        int64_t sharedStartMs)
{
    AutoMutex lock(mLock);
    if (mAudioRecord == 0) {
        return NO_INIT;
    }
    status_t status = statusTFromBinderStatus(
            mAudioRecord->shareAudioHistory(sharedPackageName, sharedStartMs));
    if (status == NO_ERROR) {
        mSharedAudioPackageName = sharedPackageName;
        mSharedAudioStartMs = sharedStartMs;
    }
    return status;
}

// =========================================================================

void AudioRecord::DeathNotifier::binderDied(const wp<IBinder>& who __unused)
{
    sp<AudioRecord> audioRecord = mAudioRecord.promote();
    if (audioRecord != 0) {
        AutoMutex lock(audioRecord->mLock);
        audioRecord->mProxy->binderDied();
    }
}

// =========================================================================

AudioRecord::AudioRecordThread::AudioRecordThread(AudioRecord& receiver)
    : Thread(true /* bCanCallJava */)  // binder recursion on restoreRecord_l() may call Java.
    , mReceiver(receiver), mPaused(true), mPausedInt(false), mPausedNs(0LL),
      mIgnoreNextPausedInt(false)
{
}

AudioRecord::AudioRecordThread::~AudioRecordThread()
{
}

bool AudioRecord::AudioRecordThread::threadLoop()
{
    {
        AutoMutex _l(mMyLock);
        if (mPaused) {
            // TODO check return value and handle or log
            mMyCond.wait(mMyLock);
            // caller will check for exitPending()
            return true;
        }
        if (mIgnoreNextPausedInt) {
            mIgnoreNextPausedInt = false;
            mPausedInt = false;
        }
        if (mPausedInt) {
            if (mPausedNs > 0) {
                // TODO check return value and handle or log
                (void) mMyCond.waitRelative(mMyLock, mPausedNs);
            } else {
                // TODO check return value and handle or log
                mMyCond.wait(mMyLock);
            }
            mPausedInt = false;
            return true;
        }
    }
    if (exitPending()) {
        return false;
    }
    nsecs_t ns =  mReceiver.processAudioBuffer();
    switch (ns) {
    case 0:
        return true;
    case NS_INACTIVE:
        pauseInternal();
        return true;
    case NS_NEVER:
        return false;
    case NS_WHENEVER:
        // Event driven: call wake() when callback notifications conditions change.
        ns = INT64_MAX;
        FALLTHROUGH_INTENDED;
    default:
        LOG_ALWAYS_FATAL_IF(ns < 0, "%s() returned %lld", __func__, (long long)ns);
        pauseInternal(ns);
        return true;
    }
}

void AudioRecord::AudioRecordThread::requestExit()
{
    // must be in this order to avoid a race condition
    Thread::requestExit();
    resume();
}

void AudioRecord::AudioRecordThread::pause()
{
    AutoMutex _l(mMyLock);
    mPaused = true;
}

void AudioRecord::AudioRecordThread::resume()
{
    AutoMutex _l(mMyLock);
    mIgnoreNextPausedInt = true;
    if (mPaused || mPausedInt) {
        mPaused = false;
        mPausedInt = false;
        mMyCond.signal();
    }
}

void AudioRecord::AudioRecordThread::wake()
{
    AutoMutex _l(mMyLock);
    if (!mPaused) {
        // wake() might be called while servicing a callback - ignore the next
        // pause time and call processAudioBuffer.
        mIgnoreNextPausedInt = true;
        if (mPausedInt && mPausedNs > 0) {
            // audio record is active and internally paused with timeout.
            mPausedInt = false;
            mMyCond.signal();
        }
    }
}

void AudioRecord::AudioRecordThread::pauseInternal(nsecs_t ns)
{
    AutoMutex _l(mMyLock);
    mPausedInt = true;
    mPausedNs = ns;
}

// -------------------------------------------------------------------------

} // namespace android
