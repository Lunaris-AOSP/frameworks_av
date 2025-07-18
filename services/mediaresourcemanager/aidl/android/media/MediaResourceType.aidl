/**
 * Copyright (c) 2019, The Android Open Source Project
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

package android.media;

/**
 * Type enums of media resources.
 *
 * {@hide}
 */
@Backing(type="int")
enum MediaResourceType {
    kUnspecified = 0,
    // Codec resource type as secure or unsecure
    kSecureCodec = 1,
    kNonSecureCodec = 2,
    // Other Codec resource types understood by the frameworks
    kGraphicMemory = 3,
    kCpuBoost = 4,
    kBattery = 5,
    // DRM Session resource type
    kDrmSession = 6,

    // Resources reserved for SW component store
    kSwResourceTypeMin = 0x1000,
    kSwResourceTypeMax = 0x1FFF,

    // Resources reserved for HW component store
    kHwResourceTypeMin = 0x2000,
    kHwResourceTypeMax = 0x2FFF,
}
