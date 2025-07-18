/*
 * Copyright (C) 2019 The Android Open Source Project
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
#define LOG_TAG "statsd_audiotrack"
#include <utils/Log.h>

#include <dirent.h>
#include <inttypes.h>
#include <pthread.h>
#include <pwd.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <stats_media_metrics.h>

#include "MediaMetricsService.h"
#include "ValidateId.h"
#include "frameworks/proto_logging/stats/message/mediametrics_message.pb.h"
#include "iface_statsd.h"

namespace android {

bool statsd_audiotrack(const std::shared_ptr<const mediametrics::Item>& item,
        const std::shared_ptr<mediametrics::StatsdLog>& statsdLog)
{
    if (item == nullptr) return false;

    // these go into the statsd wrapper
    const nsecs_t timestamp_nanos = MediaMetricsService::roundTime(item->getTimestamp());
    const std::string package_name = item->getPkgName();
    const int64_t package_version_code = item->getPkgVersionCode();
    const int64_t media_apex_version = 0;

    // the rest into our own proto
    //
    ::android::stats::mediametrics_message::AudioTrackData metrics_proto;

    // flesh out the protobuf we'll hand off with our data
    //

    // Do not change this without changing AudioTrack.cpp collection.

    // optional string streamType;
    std::string stream_type;
    if (item->getString("android.media.audiotrack.streamtype", &stream_type)) {
        metrics_proto.set_stream_type(stream_type);
    }

    // optional string contentType;
    std::string content_type;
    if (item->getString("android.media.audiotrack.type", &content_type)) {
        metrics_proto.set_content_type(content_type);
    }

    // optional string trackUsage;
    std::string track_usage;
    if (item->getString("android.media.audiotrack.usage", &track_usage)) {
        metrics_proto.set_track_usage(track_usage);
    }

    // optional int32 sampleRate;
    int32_t sample_rate = -1;
    if (item->getInt32("android.media.audiotrack.sampleRate", &sample_rate)) {
        metrics_proto.set_sample_rate(sample_rate);
    }

    // optional int64 channelMask;
    int64_t channel_mask = -1;
    if (item->getInt64("android.media.audiotrack.channelMask", &channel_mask)) {
        metrics_proto.set_channel_mask(channel_mask);
    }

    // optional int32 underrunFrames;
    int32_t underrun_frames = -1;
    if (item->getInt32("android.media.audiotrack.underrunFrames", &underrun_frames)) {
        metrics_proto.set_underrun_frames(underrun_frames);
    }

    // optional int32 glitch.startup;
    int32_t startup_glitch = -1;
    // Not currently sent from client.
    if (item->getInt32("android.media.audiotrack.glitch.startup", &startup_glitch)) {
        metrics_proto.set_startup_glitch(startup_glitch);
    }

    // portId (int32)
    int32_t port_id = -1;
    if (item->getInt32("android.media.audiotrack.portId", &port_id)) {
        metrics_proto.set_port_id(port_id);
    }
    // encoding (string)
    std::string encoding;
    if (item->getString("android.media.audiotrack.encoding", &encoding)) {
        metrics_proto.set_encoding(encoding);
    }
    // frameCount (int32)
    int32_t frame_count = -1;
    if (item->getInt32("android.media.audiotrack.frameCount", &frame_count)) {
        metrics_proto.set_frame_count(frame_count);
    }
    // attributes (string)
    std::string attributes;
    if (item->getString("android.media.audiotrack.attributes", &attributes)) {
        metrics_proto.set_attributes(attributes);
    }

    std::string serialized;
    if (!metrics_proto.SerializeToString(&serialized)) {
        ALOGE("Failed to serialize audiotrack metrics");
        return false;
    }

    // Android S
    // log_session_id (string)
    std::string logSessionId;
    (void)item->getString("android.media.audiotrack.logSessionId", &logSessionId);
    const auto log_session_id = mediametrics::ValidateId::get()->validateId(logSessionId);

    const stats::media_metrics::BytesField bf_serialized( serialized.c_str(), serialized.size());
    int result = 0;
    if (__builtin_available(android 33, *)) {
        result = stats::media_metrics::stats_write(
                               stats::media_metrics::MEDIAMETRICS_AUDIOTRACK_REPORTED,
                               timestamp_nanos, package_name.c_str(), package_version_code,
                               media_apex_version,
                               bf_serialized,
                               log_session_id.c_str());
    }
    std::stringstream log;
    log << "result:" << result << " {"
            << " mediametrics_audiotrack_reported:"
            << stats::media_metrics::MEDIAMETRICS_AUDIOTRACK_REPORTED
            << " timestamp_nanos:" << timestamp_nanos
            << " package_name:" << package_name
            << " package_version_code:" << package_version_code
            << " media_apex_version:" << media_apex_version

            << " stream_type:" << stream_type
            << " content_type:" << content_type
            << " track_usage:" << track_usage
            << " sample_rate:" << sample_rate
            << " channel_mask:" << channel_mask
            << " underrun_frames:" << underrun_frames
            << " startup_glitch:" << startup_glitch
            << " port_id:" << port_id
            << " encoding:" << encoding
            << " frame_count:" << frame_count

            << " attributes:" << attributes

            << " log_session_id:" << log_session_id
            << " }";
    statsdLog->log(stats::media_metrics::MEDIAMETRICS_AUDIOTRACK_REPORTED, log.str());
    return true;
}

} // namespace android
