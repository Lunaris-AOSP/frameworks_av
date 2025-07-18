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

//#define LOG_NDEBUG 0
#define LOG_TAG "C2AllocatorGralloc"
#include <utils/Log.h>

#include <mutex>

#include <android_media_codec.h>

#include <aidl/android/hardware/graphics/common/Cta861_3.h>
#include <aidl/android/hardware/graphics/common/PlaneLayoutComponentType.h>
#include <aidl/android/hardware/graphics/common/Smpte2086.h>
#include <android/hardware/graphics/common/1.2/types.h>
#include <cutils/native_handle.h>
#include <drm/drm_fourcc.h>
#include <gralloctypes/Gralloc4.h>
#include <hardware/gralloc.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>

#include <C2AllocatorGralloc.h>
#include <C2Buffer.h>
#include <C2Debug.h>
#include <C2PlatformSupport.h>

using ::android::hardware::hidl_handle;
using PixelFormat4 = ::android::hardware::graphics::common::V1_2::PixelFormat;

namespace android {

namespace /* unnamed */ {
    enum : uint64_t {
        /**
         * Usage mask that is passed through from gralloc to Codec 2.0 usage.
         */
        PASSTHROUGH_USAGE_MASK =
            ~static_cast<uint64_t>(GRALLOC_USAGE_SW_READ_MASK |
                                   GRALLOC_USAGE_SW_WRITE_MASK |
                                   GRALLOC_USAGE_PROTECTED)
    };

    // verify that passthrough mask is within the platform mask
    static_assert((~C2MemoryUsage::PLATFORM_MASK & PASSTHROUGH_USAGE_MASK) == 0, "");
} // unnamed

static bool isAtLeastT() {
    return android_get_device_api_level() >= __ANDROID_API_T__;
}

C2MemoryUsage C2AndroidMemoryUsage::FromGrallocUsage(uint64_t usage) {
    // gralloc does not support WRITE_PROTECTED
    return C2MemoryUsage(
            ((usage & GRALLOC_USAGE_SW_READ_MASK) ? C2MemoryUsage::CPU_READ : 0) |
            ((usage & GRALLOC_USAGE_SW_WRITE_MASK) ? C2MemoryUsage::CPU_WRITE : 0) |
            ((usage & GRALLOC_USAGE_PROTECTED) ? C2MemoryUsage::READ_PROTECTED : 0) |
            (usage & PASSTHROUGH_USAGE_MASK));
}

uint64_t C2AndroidMemoryUsage::asGrallocUsage() const {
    // gralloc does not support WRITE_PROTECTED
    return (((expected & C2MemoryUsage::CPU_READ) ? GRALLOC_USAGE_SW_READ_OFTEN : 0) |
            ((expected & C2MemoryUsage::CPU_WRITE) ? GRALLOC_USAGE_SW_WRITE_OFTEN : 0) |
            ((expected & C2MemoryUsage::READ_PROTECTED) ? GRALLOC_USAGE_PROTECTED : 0) |
            (expected & PASSTHROUGH_USAGE_MASK));
}

namespace /* unnamed */ {

/* ===================================== GRALLOC ALLOCATION ==================================== */
bool native_handle_is_invalid(const native_handle_t *const handle) {
    // perform basic validation of a native handle
    if (handle == nullptr) {
        // null handle is considered valid
        return false;
    }
    return ((size_t)handle->version != sizeof(native_handle_t) ||
            handle->numFds < 0 ||
            handle->numInts < 0 ||
            // for sanity assume handles must occupy less memory than INT_MAX bytes
            handle->numFds > int((INT_MAX - handle->version) / sizeof(int)) - handle->numInts);
}

class C2HandleGralloc : public C2Handle {
private:
    struct ExtraData {
        uint32_t width;
        uint32_t height;
        uint32_t format;
        uint32_t usage_lo;
        uint32_t usage_hi;
        uint32_t stride;
        uint32_t generation;
        uint32_t igbp_id_lo;
        uint32_t igbp_id_hi;
        uint32_t igbp_slot;
        uint32_t magic;
    };

    enum {
        NUM_INTS = sizeof(ExtraData) / sizeof(int),
    };
    const static uint32_t MAGIC = '\xc2gr\x00';

    static
    const ExtraData* GetExtraData(const C2Handle *const handle) {
        if (handle == nullptr
                || native_handle_is_invalid(handle)
                || handle->numInts < NUM_INTS) {
            return nullptr;
        }
        return reinterpret_cast<const ExtraData*>(
                &handle->data[handle->numFds + handle->numInts - NUM_INTS]);
    }

    static
    ExtraData *GetExtraData(C2Handle *const handle) {
        return const_cast<ExtraData *>(GetExtraData(const_cast<const C2Handle *const>(handle)));
    }

public:
    void getIgbpData(uint32_t *generation, uint64_t *igbp_id, uint32_t *igbp_slot) const {
        const ExtraData *ed = GetExtraData(this);
        *generation = ed->generation;
        *igbp_id = unsigned(ed->igbp_id_lo) | uint64_t(unsigned(ed->igbp_id_hi)) << 32;
        *igbp_slot = ed->igbp_slot;
    }

    static bool IsValid(const C2Handle *const o) {
        if (o == nullptr) { // null handle is always valid
            return true;
        }
        const ExtraData *xd = GetExtraData(o);
        // we cannot validate width/height/format/usage without accessing gralloc driver
        return xd != nullptr && xd->magic == MAGIC;
    }

    static C2HandleGralloc* WrapAndMoveNativeHandle(
            const native_handle_t *const handle,
            uint32_t width, uint32_t height, uint32_t format, uint64_t usage,
            uint32_t stride, uint32_t generation, uint64_t igbp_id = 0, uint32_t igbp_slot = 0) {
        //CHECK(handle != nullptr);
        if (native_handle_is_invalid(handle) ||
            handle->numInts > int((INT_MAX - handle->version) / sizeof(int)) - NUM_INTS - handle->numFds) {
            return nullptr;
        }
        ExtraData xd = {
            width, height, format, uint32_t(usage & 0xFFFFFFFF), uint32_t(usage >> 32),
            stride, generation, uint32_t(igbp_id & 0xFFFFFFFF), uint32_t(igbp_id >> 32),
            igbp_slot, MAGIC
        };
        native_handle_t *res = native_handle_create(handle->numFds, handle->numInts + NUM_INTS);
        if (res != nullptr) {
            memcpy(&res->data, &handle->data, sizeof(int) * (handle->numFds + handle->numInts));
            *GetExtraData(res) = xd;
        }
        return reinterpret_cast<C2HandleGralloc *>(res);
    }

    static C2HandleGralloc* WrapNativeHandle(
            const native_handle_t *const handle,
            uint32_t width, uint32_t height, uint32_t format, uint64_t usage,
            uint32_t stride, uint32_t generation, uint64_t igbp_id = 0, uint32_t igbp_slot = 0) {
        if (handle == nullptr) {
            return nullptr;
        }
        native_handle_t *clone = native_handle_clone(handle);
        if (clone == nullptr) {
            return nullptr;
        }
        C2HandleGralloc *res = WrapAndMoveNativeHandle(
                clone, width, height, format, usage, stride, generation, igbp_id, igbp_slot);
        if (res == nullptr) {
            native_handle_close(clone);
        }
        native_handle_delete(clone);
        return res;
    }

    static uint32_t getPixelFormat(const C2Handle *const handle) {
        if (handle == nullptr) {
            return 0;
        }
        const ExtraData *xd = GetExtraData(handle);
        return xd->format;
    }

    static bool MigrateNativeHandle(
            native_handle_t *handle,
            uint32_t generation, uint64_t igbp_id, uint32_t igbp_slot) {
        if (handle == nullptr || !IsValid(handle)) {
            return false;
        }
        ExtraData *ed = GetExtraData(handle);
        if (!ed) return false;
        ed->generation = generation;
        ed->igbp_id_lo = uint32_t(igbp_id & 0xFFFFFFFF);
        ed->igbp_id_hi = uint32_t(igbp_id >> 32);
        ed->igbp_slot = igbp_slot;
        return true;
    }


    static native_handle_t* UnwrapNativeHandle(
            const C2Handle *const handle) {
        const ExtraData *xd = GetExtraData(handle);
        if (xd == nullptr || xd->magic != MAGIC) {
            return nullptr;
        }
        native_handle_t *res = native_handle_create(handle->numFds, handle->numInts - NUM_INTS);
        if (res != nullptr) {
            memcpy(&res->data, &handle->data, sizeof(int) * (res->numFds + res->numInts));
        }
        return res;
    }

    static const C2HandleGralloc* Import(
            const C2Handle *const handle,
            uint32_t *width, uint32_t *height, uint32_t *format,
            uint64_t *usage, uint32_t *stride,
            uint32_t *generation, uint64_t *igbp_id, uint32_t *igbp_slot) {
        const ExtraData *xd = GetExtraData(handle);
        if (xd == nullptr) {
            return nullptr;
        }
        *width = xd->width;
        *height = xd->height;
        *format = xd->format;
        *usage = xd->usage_lo | (uint64_t(xd->usage_hi) << 32);
        *stride = xd->stride;
        *generation = xd->generation;
        *igbp_id = xd->igbp_id_lo | (uint64_t(xd->igbp_id_hi) << 32);
        *igbp_slot = xd->igbp_slot;
        return reinterpret_cast<const C2HandleGralloc *>(handle);
    }
};

class C2HandleAhwb : public C2Handle {
private:
    // TODO: remove extradata and use AHardwareBuffer directly.
    struct ExtraData {
        uint32_t width;
        uint32_t height;
        uint32_t format;
        uint32_t usage_lo;
        uint32_t usage_hi;
        uint32_t stride;
        uint32_t origId_lo;
        uint32_t origId_hi;
        uint32_t magic;
    };

    enum {
        NUM_INTS = sizeof(ExtraData) / sizeof(int),
    };
    const static uint32_t MAGIC = '\xc2hw\x00';

    static
    const ExtraData* GetExtraData(const C2Handle *const handle) {
        if (handle == nullptr
                || native_handle_is_invalid(handle)
                || handle->numInts < NUM_INTS) {
            return nullptr;
        }
        return reinterpret_cast<const ExtraData*>(
                &handle->data[handle->numFds + handle->numInts - NUM_INTS]);
    }

    static
    ExtraData *GetExtraData(C2Handle *const handle) {
        return const_cast<ExtraData *>(GetExtraData(const_cast<const C2Handle *const>(handle)));
    }

public:
    void getOrigId(uint64_t *origId) const {
        const ExtraData *ed = GetExtraData(this);
        *origId = unsigned(ed->origId_lo) | uint64_t(unsigned(ed->origId_hi)) << 32;
    }

    static bool IsValid(const C2Handle *const o) {
        if (o == nullptr) { // null handle is always valid
            return true;
        }
        const ExtraData *xd = GetExtraData(o);
        // we cannot validate width/height/format/usage without accessing gralloc driver
        return xd != nullptr && xd->magic == MAGIC;
    }

    static C2HandleAhwb* WrapAndMoveNativeHandle(
            const native_handle_t *const handle,
            uint32_t width, uint32_t height, uint32_t format, uint64_t usage,
            uint32_t stride, uint64_t origId) {
        //CHECK(handle != nullptr);
        if (native_handle_is_invalid(handle) || handle->numInts >
                int((INT_MAX - handle->version) / sizeof(int)) - NUM_INTS - handle->numFds) {
            return nullptr;
        }
        ExtraData xd = {
            width, height, format, uint32_t(usage & 0xFFFFFFFF), uint32_t(usage >> 32),
            stride,  uint32_t(origId & 0xFFFFFFFF), uint32_t(origId >> 32), MAGIC
        };
        native_handle_t *res = native_handle_create(handle->numFds, handle->numInts + NUM_INTS);
        if (res != nullptr) {
            memcpy(&res->data, &handle->data, sizeof(int) * (handle->numFds + handle->numInts));
            *GetExtraData(res) = xd;
        }
        return reinterpret_cast<C2HandleAhwb *>(res);
    }

    static uint32_t getPixelFormat(const C2Handle *const handle) {
        if (handle == nullptr) {
            return 0;
        }
        const ExtraData *xd = GetExtraData(handle);
        return xd->format;
    }

    static C2HandleAhwb* WrapNativeHandle(
            const native_handle_t *const handle,
            uint32_t width, uint32_t height, uint32_t format, uint64_t usage,
            uint32_t stride, uint64_t origId) {
        if (handle == nullptr) {
            return nullptr;
        }
        native_handle_t *clone = native_handle_clone(handle);
        if (clone == nullptr) {
            return nullptr;
        }
        C2HandleAhwb *res = WrapAndMoveNativeHandle(
                clone, width, height, format, usage, stride, origId);
        if (res == nullptr) {
            native_handle_close(clone);
        }
        native_handle_delete(clone);
        return res;
    }

    static native_handle_t* UnwrapNativeHandle(
            const C2Handle *const handle) {
        const ExtraData *xd = GetExtraData(handle);
        if (xd == nullptr || xd->magic != MAGIC) {
            return nullptr;
        }
        native_handle_t *res = native_handle_create(handle->numFds, handle->numInts - NUM_INTS);
        if (res != nullptr) {
            memcpy(&res->data, &handle->data, sizeof(int) * (res->numFds + res->numInts));
        }
        return res;
    }

    static const C2HandleAhwb* Import(
            const C2Handle *const handle,
            uint32_t *width, uint32_t *height, uint32_t *format,
            uint64_t *usage, uint32_t *stride,
            uint64_t *origId) {
        const ExtraData *xd = GetExtraData(handle);
        if (xd == nullptr) {
            return nullptr;
        }
        *width = xd->width;
        *height = xd->height;
        *format = xd->format;
        *usage = xd->usage_lo | (uint64_t(xd->usage_hi) << 32);
        *stride = xd->stride;
        *origId = xd->origId_lo | (uint64_t(xd->origId_hi) << 32);
        return reinterpret_cast<const C2HandleAhwb *>(handle);
    }
};

static
c2_status_t Gralloc4Mapper_lock(native_handle_t *handle, uint64_t usage, const Rect& bounds,
        C2PlanarLayout *layout, uint8_t **addr) {
    GraphicBufferMapper &mapper = GraphicBufferMapper::get();

    std::vector<ui::PlaneLayout> planes;
    // this method is only supported on Gralloc 4 or later
    status_t err = mapper.getPlaneLayouts(handle, &planes);
    if (err != NO_ERROR || planes.empty()) {
        return C2_CANNOT_DO;
    }

    uint8_t *pointer = nullptr;
    err = mapper.lock(handle, usage, bounds, (void **)&pointer);
    if (err != NO_ERROR || pointer == nullptr) {
        return C2_CORRUPTED;
    }

    using aidl::android::hardware::graphics::common::PlaneLayoutComponentType;
    using aidl::android::hardware::graphics::common::PlaneLayoutComponent;

    layout->type = C2PlanarLayout::TYPE_YUV;
    layout->numPlanes = 0;
    layout->rootPlanes = 0;

    for (const ui::PlaneLayout &plane : planes) {
        layout->rootPlanes++;
        uint32_t lastOffsetInBits = 0;
        uint32_t rootIx = layout->numPlanes;

        for (const PlaneLayoutComponent &component : plane.components) {
            if (!gralloc4::isStandardPlaneLayoutComponentType(component.type)) {
                mapper.unlock(handle);
                return C2_CANNOT_DO;
            }

            uint32_t rightShiftBits = component.offsetInBits - lastOffsetInBits;
            uint32_t allocatedDepthInBits = component.sizeInBits + rightShiftBits;
            C2PlanarLayout::plane_index_t planeId;
            C2PlaneInfo::channel_t channel;

            switch (static_cast<PlaneLayoutComponentType>(component.type.value)) {
                case PlaneLayoutComponentType::Y:
                    planeId = C2PlanarLayout::PLANE_Y;
                    channel = C2PlaneInfo::CHANNEL_Y;
                    break;
                case PlaneLayoutComponentType::CB:
                    planeId = C2PlanarLayout::PLANE_U;
                    channel = C2PlaneInfo::CHANNEL_CB;
                    break;
                case PlaneLayoutComponentType::CR:
                    planeId = C2PlanarLayout::PLANE_V;
                    channel = C2PlaneInfo::CHANNEL_CR;
                    break;
                default:
                    mapper.unlock(handle);
                    return C2_CORRUPTED;
            }

            addr[planeId] = pointer + plane.offsetInBytes + (component.offsetInBits / 8);
            layout->planes[planeId] = {
                channel,                                                // channel
                static_cast<int32_t>(plane.sampleIncrementInBits / 8),  // colInc
                static_cast<int32_t>(plane.strideInBytes),              // rowInc
                static_cast<uint32_t>(plane.horizontalSubsampling),     // mColSampling
                static_cast<uint32_t>(plane.verticalSubsampling),       // mRowSampling
                allocatedDepthInBits,                                   // allocatedDepth (bits)
                static_cast<uint32_t>(component.sizeInBits),            // bitDepth (bits)
                rightShiftBits,                                         // rightShift (bits)
                C2PlaneInfo::NATIVE,                                    // endianness
                rootIx,                                                 // rootIx
                static_cast<uint32_t>(component.offsetInBits / 8),      // offset (bytes)
            };

            layout->numPlanes++;
            lastOffsetInBits = component.offsetInBits + component.sizeInBits;
        }
    }
    return C2_OK;
}

static c2_status_t PopulatePlaneLayout(
        buffer_handle_t buffer,
        const Rect &rect,
        uint32_t format,
        uint64_t grallocUsage,
        uint32_t stride,
        C2PlanarLayout *layout /* nonnull */, uint8_t **addr /* nonnull */) {
    // 'NATIVE' on Android means LITTLE_ENDIAN
    constexpr C2PlaneInfo::endianness_t kEndianness = C2PlaneInfo::NATIVE;

    // Try to resolve IMPLEMENTATION_DEFINED format to accurate format if
    // possible.
    uint32_t fourCc;
    if (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED &&
        !GraphicBufferMapper::get().getPixelFormatFourCC(buffer, &fourCc)) {
        switch (fourCc)  {
            case DRM_FORMAT_XBGR8888:
                 format = static_cast<uint32_t>(PixelFormat4::RGBX_8888);
                 break;
            case DRM_FORMAT_ABGR8888:
                 format = static_cast<uint32_t>(PixelFormat4::RGBA_8888);
                 break;
            default:
                 break;
        }
    }

    switch (format) {
        case static_cast<uint32_t>(PixelFormat4::RGBA_1010102): {
            // TRICKY: this is used for media as YUV444 in the case when it is queued directly to a
            // Surface. In all other cases it is RGBA. We don't know which case it is here, so
            // default to YUV for now.
            void *pointer = nullptr;
            // TODO: fence
            status_t err = GraphicBufferMapper::get().lock(
                    const_cast<native_handle_t *>(buffer), grallocUsage, rect, &pointer);
            if (err) {
                ALOGE("failed transaction: lock(RGBA_1010102)");
                return C2_CORRUPTED;
            }
            // treat as 32-bit values
            addr[C2PlanarLayout::PLANE_Y] = (uint8_t *)pointer;
            addr[C2PlanarLayout::PLANE_U] = (uint8_t *)pointer;
            addr[C2PlanarLayout::PLANE_V] = (uint8_t *)pointer;
            addr[C2PlanarLayout::PLANE_A] = (uint8_t *)pointer;
            layout->type = C2PlanarLayout::TYPE_YUVA;
            layout->numPlanes = 4;
            layout->rootPlanes = 1;
            layout->planes[C2PlanarLayout::PLANE_Y] = {
                C2PlaneInfo::CHANNEL_Y,         // channel
                4,                              // colInc
                static_cast<int32_t>(4 * stride), // rowInc
                1,                              // mColSampling
                1,                              // mRowSampling
                32,                             // allocatedDepth
                10,                             // bitDepth
                10,                             // rightShift
                C2PlaneInfo::LITTLE_END,        // endianness
                C2PlanarLayout::PLANE_Y,        // rootIx
                0,                              // offset
            };
            layout->planes[C2PlanarLayout::PLANE_U] = {
                C2PlaneInfo::CHANNEL_CB,         // channel
                4,                              // colInc
                static_cast<int32_t>(4 * stride), // rowInc
                1,                              // mColSampling
                1,                              // mRowSampling
                32,                             // allocatedDepth
                10,                             // bitDepth
                0,                              // rightShift
                C2PlaneInfo::LITTLE_END,        // endianness
                C2PlanarLayout::PLANE_Y,        // rootIx
                0,                              // offset
            };
            layout->planes[C2PlanarLayout::PLANE_V] = {
                C2PlaneInfo::CHANNEL_CR,         // channel
                4,                              // colInc
                static_cast<int32_t>(4 * stride), // rowInc
                1,                              // mColSampling
                1,                              // mRowSampling
                32,                             // allocatedDepth
                10,                             // bitDepth
                20,                             // rightShift
                C2PlaneInfo::LITTLE_END,        // endianness
                C2PlanarLayout::PLANE_Y,        // rootIx
                0,                              // offset
            };
            layout->planes[C2PlanarLayout::PLANE_A] = {
                C2PlaneInfo::CHANNEL_A,         // channel
                4,                              // colInc
                static_cast<int32_t>(4 * stride), // rowInc
                1,                              // mColSampling
                1,                              // mRowSampling
                32,                             // allocatedDepth
                2,                              // bitDepth
                30,                             // rightShift
                C2PlaneInfo::LITTLE_END,        // endianness
                C2PlanarLayout::PLANE_Y,        // rootIx
                0,                              // offset
            };
            break;
        }

        case static_cast<uint32_t>(PixelFormat4::RGBA_8888):
            // TODO: alpha channel
            // fall-through
        case static_cast<uint32_t>(PixelFormat4::RGBX_8888): {
            void *pointer = nullptr;
            // TODO: fence
            status_t err = GraphicBufferMapper::get().lock(
                    const_cast<native_handle_t*>(buffer), grallocUsage, rect, &pointer);
            if (err) {
                ALOGE("failed transaction: lock(RGBA_8888)");
                return C2_CORRUPTED;
            }
            addr[C2PlanarLayout::PLANE_R] = (uint8_t *)pointer;
            addr[C2PlanarLayout::PLANE_G] = (uint8_t *)pointer + 1;
            addr[C2PlanarLayout::PLANE_B] = (uint8_t *)pointer + 2;
            layout->type = C2PlanarLayout::TYPE_RGB;
            layout->numPlanes = 3;
            layout->rootPlanes = 1;
            layout->planes[C2PlanarLayout::PLANE_R] = {
                C2PlaneInfo::CHANNEL_R,         // channel
                4,                              // colInc
                static_cast<int32_t>(4 * stride), // rowInc
                1,                              // mColSampling
                1,                              // mRowSampling
                8,                              // allocatedDepth
                8,                              // bitDepth
                0,                              // rightShift
                C2PlaneInfo::NATIVE,            // endianness
                C2PlanarLayout::PLANE_R,        // rootIx
                0,                              // offset
            };
            layout->planes[C2PlanarLayout::PLANE_G] = {
                C2PlaneInfo::CHANNEL_G,         // channel
                4,                              // colInc
                static_cast<int32_t>(4 * stride), // rowInc
                1,                              // mColSampling
                1,                              // mRowSampling
                8,                              // allocatedDepth
                8,                              // bitDepth
                0,                              // rightShift
                C2PlaneInfo::NATIVE,            // endianness
                C2PlanarLayout::PLANE_R,        // rootIx
                1,                              // offset
            };
            layout->planes[C2PlanarLayout::PLANE_B] = {
                C2PlaneInfo::CHANNEL_B,         // channel
                4,                              // colInc
                static_cast<int32_t>(4 * stride), // rowInc
                1,                              // mColSampling
                1,                              // mRowSampling
                8,                              // allocatedDepth
                8,                              // bitDepth
                0,                              // rightShift
                C2PlaneInfo::NATIVE,            // endianness
                C2PlanarLayout::PLANE_R,        // rootIx
                2,                              // offset
            };
            break;
        }

        case static_cast<uint32_t>(PixelFormat4::BLOB): {
            void *pointer = nullptr;
            // TODO: fence
            status_t err = GraphicBufferMapper::get().lock(
                    const_cast<native_handle_t*>(buffer), grallocUsage, rect, &pointer);
            if (err) {
                ALOGE("failed transaction: lock(BLOB)");
                return C2_CORRUPTED;
            }
            *addr = (uint8_t *)pointer;
            break;
        }

        case static_cast<uint32_t>(PixelFormat4::YCBCR_422_SP):
            // fall-through
        case static_cast<uint32_t>(PixelFormat4::YCRCB_420_SP):
            // fall-through
        case static_cast<uint32_t>(PixelFormat4::YCBCR_422_I):
            // fall-through
        case static_cast<uint32_t>(PixelFormat4::YCBCR_420_888):
            // fall-through
        case static_cast<uint32_t>(PixelFormat4::YV12): {
            android_ycbcr ycbcrLayout;

            status_t err = GraphicBufferMapper::get().lockYCbCr(
                    const_cast<native_handle_t*>(buffer), grallocUsage, rect, &ycbcrLayout);
            if (err) {
                ALOGE("failed transaction: lockYCbCr (err=%d)", err);
                return C2_CORRUPTED;
            }
            if (!ycbcrLayout.y || !ycbcrLayout.cb || !ycbcrLayout.cr
                    || ycbcrLayout.ystride == 0
                    || ycbcrLayout.cstride == 0
                    || ycbcrLayout.chroma_step == 0) {
                ALOGE("invalid layout: lockYCbCr (y=%s cb=%s cr=%s "
                        "ystride=%zu cstride=%zu chroma_step=%zu)",
                        ycbcrLayout.y ? "(non-null)" : "(null)",
                        ycbcrLayout.cb ? "(non-null)" : "(null)",
                        ycbcrLayout.cr ? "(non-null)" : "(null)",
                        ycbcrLayout.ystride, ycbcrLayout.cstride, ycbcrLayout.chroma_step);
                return C2_CORRUPTED;
            }

            addr[C2PlanarLayout::PLANE_Y] = (uint8_t *)ycbcrLayout.y;
            addr[C2PlanarLayout::PLANE_U] = (uint8_t *)ycbcrLayout.cb;
            addr[C2PlanarLayout::PLANE_V] = (uint8_t *)ycbcrLayout.cr;
            layout->type = C2PlanarLayout::TYPE_YUV;
            layout->numPlanes = 3;
            layout->rootPlanes = 3;
            layout->planes[C2PlanarLayout::PLANE_Y] = {
                C2PlaneInfo::CHANNEL_Y,         // channel
                1,                              // colInc
                (int32_t)ycbcrLayout.ystride,   // rowInc
                1,                              // mColSampling
                1,                              // mRowSampling
                8,                              // allocatedDepth
                8,                              // bitDepth
                0,                              // rightShift
                C2PlaneInfo::NATIVE,            // endianness
                C2PlanarLayout::PLANE_Y,        // rootIx
                0,                              // offset
            };
            layout->planes[C2PlanarLayout::PLANE_U] = {
                C2PlaneInfo::CHANNEL_CB,          // channel
                (int32_t)ycbcrLayout.chroma_step, // colInc
                (int32_t)ycbcrLayout.cstride,     // rowInc
                2,                                // mColSampling
                2,                                // mRowSampling
                8,                                // allocatedDepth
                8,                                // bitDepth
                0,                                // rightShift
                C2PlaneInfo::NATIVE,              // endianness
                C2PlanarLayout::PLANE_U,          // rootIx
                0,                                // offset
            };
            layout->planes[C2PlanarLayout::PLANE_V] = {
                C2PlaneInfo::CHANNEL_CR,          // channel
                (int32_t)ycbcrLayout.chroma_step, // colInc
                (int32_t)ycbcrLayout.cstride,     // rowInc
                2,                                // mColSampling
                2,                                // mRowSampling
                8,                                // allocatedDepth
                8,                                // bitDepth
                0,                                // rightShift
                C2PlaneInfo::NATIVE,              // endianness
                C2PlanarLayout::PLANE_V,          // rootIx
                0,                                // offset
            };
            break;
        }

        case static_cast<uint32_t>(PixelFormat4::YCBCR_P010): {
            // In Android T, P010 is relaxed to allow arbitrary stride for the Y and UV planes,
            // try locking with the gralloc4 mapper first.
            c2_status_t status = Gralloc4Mapper_lock(
                    const_cast<native_handle_t*>(buffer), grallocUsage, rect, layout, addr);
            if (status == C2_OK) {
                break;
            }

            void *pointer = nullptr;
            status_t err = GraphicBufferMapper::get().lock(
                    const_cast<native_handle_t *>(buffer), grallocUsage, rect, &pointer);
            if (err) {
                ALOGE("failed transaction: lock(YCBCR_P010)");
                return C2_CORRUPTED;
            }
            addr[C2PlanarLayout::PLANE_Y] = (uint8_t *)pointer;
            addr[C2PlanarLayout::PLANE_U] = (uint8_t *)pointer + stride * 2 * rect.height();
            addr[C2PlanarLayout::PLANE_V] = addr[C2PlanarLayout::PLANE_U] + 2;
            layout->type = C2PlanarLayout::TYPE_YUV;
            layout->numPlanes = 3;
            layout->rootPlanes = 2;
            layout->planes[C2PlanarLayout::PLANE_Y] = {
                C2PlaneInfo::CHANNEL_Y,         // channel
                2,                              // colInc
                static_cast<int32_t>(2 * stride), // rowInc
                1,                              // mColSampling
                1,                              // mRowSampling
                16,                             // allocatedDepth
                10,                             // bitDepth
                6,                              // rightShift
                kEndianness,                    // endianness
                C2PlanarLayout::PLANE_Y,        // rootIx
                0,                              // offset
            };
            layout->planes[C2PlanarLayout::PLANE_U] = {
                C2PlaneInfo::CHANNEL_CB,        // channel
                4,                              // colInc
                static_cast<int32_t>(2 * stride), // rowInc
                2,                              // mColSampling
                2,                              // mRowSampling
                16,                             // allocatedDepth
                10,                             // bitDepth
                6,                              // rightShift
                kEndianness,                    // endianness
                C2PlanarLayout::PLANE_U,        // rootIx
                0,                              // offset
            };
            layout->planes[C2PlanarLayout::PLANE_V] = {
                C2PlaneInfo::CHANNEL_CR,        // channel
                4,                              // colInc
                static_cast<int32_t>(2 * stride), // rowInc
                2,                              // mColSampling
                2,                              // mRowSampling
                16,                             // allocatedDepth
                10,                             // bitDepth
                6,                              // rightShift
                kEndianness,                    // endianness
                C2PlanarLayout::PLANE_U,        // rootIx
                2,                              // offset
            };
            break;
        }

        default: {
            // We don't know what it is, let's try to lock it with gralloc4
            android_ycbcr ycbcrLayout;
            if (isAtLeastT()) {
                c2_status_t status = Gralloc4Mapper_lock(
                        const_cast<native_handle_t*>(buffer), grallocUsage, rect, layout, addr);
                if (status == C2_OK) {
                    break;
                }
            }

            // fallback to lockYCbCr
            status_t err = GraphicBufferMapper::get().lockYCbCr(
                    const_cast<native_handle_t*>(buffer), grallocUsage, rect, &ycbcrLayout);
            if (err == OK && ycbcrLayout.y && ycbcrLayout.cb && ycbcrLayout.cr
                    && ycbcrLayout.ystride > 0
                    && ycbcrLayout.cstride > 0
                    && ycbcrLayout.chroma_step > 0) {
                addr[C2PlanarLayout::PLANE_Y] = (uint8_t *)ycbcrLayout.y;
                addr[C2PlanarLayout::PLANE_U] = (uint8_t *)ycbcrLayout.cb;
                addr[C2PlanarLayout::PLANE_V] = (uint8_t *)ycbcrLayout.cr;
                layout->type = C2PlanarLayout::TYPE_YUV;
                layout->numPlanes = 3;
                layout->rootPlanes = 3;
                layout->planes[C2PlanarLayout::PLANE_Y] = {
                    C2PlaneInfo::CHANNEL_Y,         // channel
                    1,                              // colInc
                    (int32_t)ycbcrLayout.ystride,   // rowInc
                    1,                              // mColSampling
                    1,                              // mRowSampling
                    8,                              // allocatedDepth
                    8,                              // bitDepth
                    0,                              // rightShift
                    C2PlaneInfo::NATIVE,            // endianness
                    C2PlanarLayout::PLANE_Y,        // rootIx
                    0,                              // offset
                };
                layout->planes[C2PlanarLayout::PLANE_U] = {
                    C2PlaneInfo::CHANNEL_CB,          // channel
                    (int32_t)ycbcrLayout.chroma_step, // colInc
                    (int32_t)ycbcrLayout.cstride,     // rowInc
                    2,                                // mColSampling
                    2,                                // mRowSampling
                    8,                                // allocatedDepth
                    8,                                // bitDepth
                    0,                                // rightShift
                    C2PlaneInfo::NATIVE,              // endianness
                    C2PlanarLayout::PLANE_U,          // rootIx
                    0,                                // offset
                };
                layout->planes[C2PlanarLayout::PLANE_V] = {
                    C2PlaneInfo::CHANNEL_CR,          // channel
                    (int32_t)ycbcrLayout.chroma_step, // colInc
                    (int32_t)ycbcrLayout.cstride,     // rowInc
                    2,                                // mColSampling
                    2,                                // mRowSampling
                    8,                                // allocatedDepth
                    8,                                // bitDepth
                    0,                                // rightShift
                    C2PlaneInfo::NATIVE,              // endianness
                    C2PlanarLayout::PLANE_V,          // rootIx
                    0,                                // offset
                };
                break;
            }

            // We really don't know what this is; lock the buffer and pass it through ---
            // the client may know how to interpret it.

            // unlock previous allocation if it was successful
            if (err == OK) {
                err = GraphicBufferMapper::get().unlock(buffer);
                if (err) {
                    ALOGE("failed transaction: unlock");
                    return C2_CORRUPTED;
                }
            }

            void *pointer = nullptr;
            err = GraphicBufferMapper::get().lock(
                    const_cast<native_handle_t *>(buffer), grallocUsage, rect, &pointer);
            if (err) {
                ALOGE("failed transaction: lock(??? %x)", format);
                return C2_CORRUPTED;
            }
            addr[0] = (uint8_t *)pointer;
            layout->type = C2PlanarLayout::TYPE_UNKNOWN;
            layout->numPlanes = 1;
            layout->rootPlanes = 1;
            layout->planes[0] = {
                // TODO: CHANNEL_UNKNOWN?
                C2PlaneInfo::channel_t(0xFF),   // channel
                1,                              // colInc
                int32_t(stride),               // rowInc
                1,                              // mColSampling
                1,                              // mRowSampling
                8,                              // allocatedDepth
                8,                              // bitDepth
                0,                              // rightShift
                C2PlaneInfo::NATIVE,            // endianness
                0,                              // rootIx
                0,                              // offset
            };
            break;
        }
    }
    return C2_OK;
}

static void HandleInterleavedPlanes(
        C2PlanarLayout *layout /* nonnull */, uint8_t **addr /* nonnull */) {
    if (layout->type == C2PlanarLayout::TYPE_YUV && layout->rootPlanes == 3) {
        intptr_t uvOffset = addr[C2PlanarLayout::PLANE_V] - addr[C2PlanarLayout::PLANE_U];
        intptr_t uvColInc = layout->planes[C2PlanarLayout::PLANE_U].colInc;
        if (uvOffset > 0 && uvOffset < uvColInc) {
            layout->rootPlanes = 2;
            layout->planes[C2PlanarLayout::PLANE_V].rootIx = C2PlanarLayout::PLANE_U;
            layout->planes[C2PlanarLayout::PLANE_V].offset = uvOffset;
        } else if (uvOffset < 0 && uvOffset > -uvColInc) {
            layout->rootPlanes = 2;
            layout->planes[C2PlanarLayout::PLANE_U].rootIx = C2PlanarLayout::PLANE_V;
            layout->planes[C2PlanarLayout::PLANE_U].offset = -uvOffset;
        }
    }
}

} // unnamed namespace


native_handle_t *UnwrapNativeCodec2GrallocHandle(const C2Handle *const handle) {
    if (handle == nullptr) {
        return nullptr;
    }
    if (C2AllocatorGralloc::CheckHandle(handle)) {
        return C2HandleGralloc::UnwrapNativeHandle(handle);
    }
    if (C2AllocatorAhwb::CheckHandle(handle)) {
        return C2HandleAhwb::UnwrapNativeHandle(handle);
    }
    ALOGE("tried to unwrap non c2 compatible handle");
    return nullptr;
}

C2Handle *WrapNativeCodec2GrallocHandle(
        const native_handle_t *const handle,
        uint32_t width, uint32_t height, uint32_t format, uint64_t usage, uint32_t stride,
        uint32_t generation, uint64_t igbp_id, uint32_t igbp_slot) {
    return C2HandleGralloc::WrapNativeHandle(handle, width, height, format, usage, stride,
                                             generation, igbp_id, igbp_slot);
}

uint32_t ExtractFormatFromCodec2GrallocHandle(const C2Handle *const handle) {
    if (C2AllocatorGralloc::CheckHandle(handle)) {
        return C2HandleGralloc::getPixelFormat(handle);
    }
    if (C2AllocatorAhwb::CheckHandle(handle)) {
        return C2HandleAhwb::getPixelFormat(handle);
    }
    ALOGE("tried to extract pixelformat from non c2 compatible handle");
    return 0;
}

bool ExtractMetadataFromCodec2GrallocHandle(
        const C2Handle *const handle,
        uint32_t *width, uint32_t *height, uint32_t *format, uint64_t *usage, uint32_t *stride) {
    if (handle == nullptr) {
        ALOGE("ExtractMetadata from nullptr");
        return false;
    }
    if (C2AllocatorGralloc::CheckHandle(handle)) {
        uint32_t generation;
        uint64_t igbp_id;
        uint32_t igbp_slot;
        (void)C2HandleGralloc::Import(handle, width, height, format, usage, stride,
                                      &generation, &igbp_id, &igbp_slot);
        return true;
    }
    if (C2AllocatorAhwb::CheckHandle(handle)) {
        uint64_t origId;
        (void)C2HandleAhwb::Import(handle, width, height, format, usage, stride, &origId);
        return true;
    }
    ALOGE("ExtractMetadata from non compatible handle");
    return false;
}

bool MigrateNativeCodec2GrallocHandle(
        native_handle_t *handle,
        uint32_t generation, uint64_t igbp_id, uint32_t igbp_slot) {
    return C2HandleGralloc::MigrateNativeHandle(handle, generation, igbp_id, igbp_slot);
}

using ::aidl::android::hardware::graphics::common::Cta861_3;
using ::aidl::android::hardware::graphics::common::Smpte2086;

namespace {

class GrallocBuffer {
public:
    GrallocBuffer(const C2Handle *const handle) : mBuffer(nullptr) {
        GraphicBufferMapper& mapper = GraphicBufferMapper::get();

        // Unwrap raw buffer handle from the C2Handle
        native_handle_t *nh = UnwrapNativeCodec2GrallocHandle(handle);
        if (!nh) {
            ALOGE("handle is not compatible to any gralloc C2Handle types");
            return;
        }
        // Import the raw handle so IMapper can use the buffer. The imported
        // handle must be freed when the client is done with the buffer.
        status_t status = mapper.importBufferNoValidate(
                nh,
                &mBuffer);

        if (status != OK) {
            ALOGE("Failed to import buffer. Status: %d.", status);
            return;
        }

        // TRICKY: UnwrapNativeCodec2GrallocHandle creates a new handle but
        //         does not clone the fds. Thus we need to delete the handle
        //         without closing it.
        native_handle_delete(nh);
    }

    ~GrallocBuffer() {
        GraphicBufferMapper& mapper = GraphicBufferMapper::get();
        if (mBuffer) {
            // Free the imported buffer handle. This does not release the
            // underlying buffer itself.
            mapper.freeBuffer(mBuffer);
        }
    }

    buffer_handle_t get() const { return mBuffer; }
    operator bool() const { return (mBuffer != nullptr); }
private:
    buffer_handle_t mBuffer;
};

}  // namspace

c2_status_t GetHdrMetadataFromGralloc4Handle(
        const C2Handle *const handle,
        std::shared_ptr<C2StreamHdrStaticMetadataInfo::input> *staticInfo,
        std::shared_ptr<C2StreamHdrDynamicMetadataInfo::input> *dynamicInfo) {
    c2_status_t err = C2_OK;
    GraphicBufferMapper& mapper = GraphicBufferMapper::get();
    GrallocBuffer buffer(handle);
    if (!buffer) {
        // Gralloc4 not supported; nothing to do
        return err;
    }
    if (staticInfo) {
        ALOGV("Grabbing static HDR info from gralloc metadata");
        staticInfo->reset(new C2StreamHdrStaticMetadataInfo::input(0u));
        memset(&(*staticInfo)->mastering, 0, sizeof((*staticInfo)->mastering));
        (*staticInfo)->maxCll = 0;
        (*staticInfo)->maxFall = 0;

        std::optional<Smpte2086> smpte2086;
        status_t status = mapper.getSmpte2086(buffer.get(), &smpte2086);
        if (status != OK || !smpte2086) {
            err = C2_CORRUPTED;
        } else {
            if (smpte2086) {
                  (*staticInfo)->mastering.red.x    = smpte2086->primaryRed.x;
                  (*staticInfo)->mastering.red.y    = smpte2086->primaryRed.y;
                  (*staticInfo)->mastering.green.x  = smpte2086->primaryGreen.x;
                  (*staticInfo)->mastering.green.y  = smpte2086->primaryGreen.y;
                  (*staticInfo)->mastering.blue.x   = smpte2086->primaryBlue.x;
                  (*staticInfo)->mastering.blue.y   = smpte2086->primaryBlue.y;
                  (*staticInfo)->mastering.white.x  = smpte2086->whitePoint.x;
                  (*staticInfo)->mastering.white.y  = smpte2086->whitePoint.y;

                  (*staticInfo)->mastering.maxLuminance = smpte2086->maxLuminance;
                  (*staticInfo)->mastering.minLuminance = smpte2086->minLuminance;
            }
        }

        std::optional<Cta861_3> cta861_3;
        status = mapper.getCta861_3(buffer.get(), &cta861_3);
        if (status != OK || !cta861_3) {
            err = C2_CORRUPTED;
        } else {
            if (cta861_3) {
                  (*staticInfo)->maxCll   = cta861_3->maxContentLightLevel;
                  (*staticInfo)->maxFall  = cta861_3->maxFrameAverageLightLevel;
            }
        }
    }

    if (err != C2_OK) {
        staticInfo->reset();
    }

    if (dynamicInfo) {
        ALOGV("Grabbing dynamic HDR info from gralloc metadata");
        dynamicInfo->reset();
        std::optional<std::vector<uint8_t>> vec;
        status_t status = mapper.getSmpte2094_40(buffer.get(), &vec);
        if (status != OK || !vec) {
            dynamicInfo->reset();
            err = C2_CORRUPTED;
        } else {
            if (vec) {
                *dynamicInfo = C2StreamHdrDynamicMetadataInfo::input::AllocShared(
                      vec->size(), 0u, C2Config::HDR_DYNAMIC_METADATA_TYPE_SMPTE_2094_40);
                memcpy((*dynamicInfo)->m.data, vec->data(), vec->size());
            }
        }
    }

    return err;
}

c2_status_t SetMetadataToGralloc4Handle(
        android_dataspace_t dataSpace,
        const std::shared_ptr<const C2StreamHdrStaticMetadataInfo::output> &staticInfo,
        const std::shared_ptr<const C2StreamHdrDynamicMetadataInfo::output> &dynamicInfo,
        const C2Handle *const handle) {
    c2_status_t err = C2_OK;
    GraphicBufferMapper& mapper = GraphicBufferMapper::get();
    GrallocBuffer buffer(handle);
    if (!buffer) {
        // Gralloc4 not supported; nothing to do
        return err;
    }
    // Use V0 dataspaces for Gralloc4+
    if (android::media::codec::provider_->dataspace_v0_partial()) {
        ColorUtils::convertDataSpaceToV0(dataSpace);
    }
    status_t status = mapper.setDataspace(buffer.get(), static_cast<ui::Dataspace>(dataSpace));
    if (status != OK) {
       err = C2_CORRUPTED;
    }
    if (staticInfo && *staticInfo) {
        ALOGV("Setting static HDR info as gralloc metadata");
        std::optional<Smpte2086> smpte2086 = Smpte2086{
            {staticInfo->mastering.red.x, staticInfo->mastering.red.y},
            {staticInfo->mastering.green.x, staticInfo->mastering.green.y},
            {staticInfo->mastering.blue.x, staticInfo->mastering.blue.y},
            {staticInfo->mastering.white.x, staticInfo->mastering.white.y},
            staticInfo->mastering.maxLuminance,
            staticInfo->mastering.minLuminance,
        };
        if (0.0 <= smpte2086->primaryRed.x && smpte2086->primaryRed.x <= 1.0
                && 0.0 <= smpte2086->primaryRed.y && smpte2086->primaryRed.y <= 1.0
                && 0.0 <= smpte2086->primaryGreen.x && smpte2086->primaryGreen.x <= 1.0
                && 0.0 <= smpte2086->primaryGreen.y && smpte2086->primaryGreen.y <= 1.0
                && 0.0 <= smpte2086->primaryBlue.x && smpte2086->primaryBlue.x <= 1.0
                && 0.0 <= smpte2086->primaryBlue.y && smpte2086->primaryBlue.y <= 1.0
                && 0.0 <= smpte2086->whitePoint.x && smpte2086->whitePoint.x <= 1.0
                && 0.0 <= smpte2086->whitePoint.y && smpte2086->whitePoint.y <= 1.0
                && 0.0 <= smpte2086->maxLuminance && 0.0 <= smpte2086->minLuminance) {
            status = mapper.setSmpte2086(buffer.get(), smpte2086);
            if (status != OK) {
                err = C2_CORRUPTED;
            }
        }
        std::optional<Cta861_3> cta861_3 = Cta861_3{
            staticInfo->maxCll,
            staticInfo->maxFall,
        };
        if (0.0 <= cta861_3->maxContentLightLevel && 0.0 <= cta861_3->maxFrameAverageLightLevel) {
            status = mapper.setCta861_3(buffer.get(), cta861_3);
            if (status != OK) {
                err = C2_CORRUPTED;
            }
        }
    }
    if (dynamicInfo && *dynamicInfo && dynamicInfo->flexCount() > 0) {
        ALOGV("Setting dynamic HDR info as gralloc metadata");
        if (dynamicInfo->m.type_ == C2Config::HDR_DYNAMIC_METADATA_TYPE_SMPTE_2094_40) {
            std::optional<std::vector<uint8_t>> smpte2094_40 = std::vector<uint8_t>();
            smpte2094_40->resize(dynamicInfo->flexCount());
            memcpy(smpte2094_40->data(), dynamicInfo->m.data, dynamicInfo->flexCount());

            status = mapper.setSmpte2094_40(buffer.get(), smpte2094_40);
            if (status != OK) {
                err = C2_CORRUPTED;
            }
        } else {
            err = C2_BAD_VALUE;
        }
    }

    return err;
}


class C2AllocationGralloc : public C2GraphicAllocation {
public:
    virtual ~C2AllocationGralloc() override;

    virtual c2_status_t map(
            C2Rect c2Rect, C2MemoryUsage usage, C2Fence *fence,
            C2PlanarLayout *layout /* nonnull */, uint8_t **addr /* nonnull */) override;
    virtual c2_status_t unmap(
            uint8_t **addr /* nonnull */, C2Rect rect, C2Fence *fence /* nullable */) override;
    virtual C2Allocator::id_t getAllocatorId() const override { return mAllocatorId; }
    virtual const C2Handle *handle() const override { return mLockedHandle ? : mHandle; }
    virtual bool equals(const std::shared_ptr<const C2GraphicAllocation> &other) const override;

    // internal methods
    // |handle| will be moved.

    C2AllocationGralloc(
              uint32_t width, uint32_t height,
              uint32_t format, uint32_t layerCount,
              uint64_t grallocUsage, uint32_t stride,
              hidl_handle &hidlHandle,
              const C2HandleGralloc *const handle,
              C2Allocator::id_t allocatorId);
    int dup() const;
    c2_status_t status() const;

private:
    const uint32_t mWidth;
    const uint32_t mHeight;
    const uint32_t mFormat;
    const uint32_t mLayerCount;
    const uint64_t mGrallocUsage;
    const uint32_t mStride;
    const hidl_handle mHidlHandle;
    const C2HandleGralloc *mHandle;
    buffer_handle_t mBuffer;
    const C2HandleGralloc *mLockedHandle;
    bool mLocked;
    C2Allocator::id_t mAllocatorId;
    std::mutex mMappedLock;
};

C2AllocationGralloc::C2AllocationGralloc(
          uint32_t width, uint32_t height,
          uint32_t format, uint32_t layerCount,
          uint64_t grallocUsage, uint32_t stride,
          hidl_handle &hidlHandle,
          const C2HandleGralloc *const handle,
          C2Allocator::id_t allocatorId)
    : C2GraphicAllocation(width, height),
      mWidth(width),
      mHeight(height),
      mFormat(format),
      mLayerCount(layerCount),
      mGrallocUsage(grallocUsage),
      mStride(stride),
      mHidlHandle(std::move(hidlHandle)),
      mHandle(handle),
      mBuffer(nullptr),
      mLockedHandle(nullptr),
      mLocked(false),
      mAllocatorId(allocatorId) {
}

C2AllocationGralloc::~C2AllocationGralloc() {
    if (mBuffer && mLocked) {
        // implementation ignores address and rect
        uint8_t* addr[C2PlanarLayout::MAX_NUM_PLANES] = {};
        unmap(addr, C2Rect(), nullptr);
    }
    if (mBuffer) {
        status_t err = GraphicBufferMapper::get().freeBuffer(mBuffer);
        if (err) {
            ALOGE("failed transaction: freeBuffer");
        }
    }
    if (mHandle) {
        native_handle_delete(
                const_cast<native_handle_t *>(reinterpret_cast<const native_handle_t *>(mHandle)));
    }
    if (mLockedHandle) {
        native_handle_delete(
                const_cast<native_handle_t *>(
                        reinterpret_cast<const native_handle_t *>(mLockedHandle)));
    }
}

c2_status_t C2AllocationGralloc::map(
        C2Rect c2Rect, C2MemoryUsage usage, C2Fence *fence,
        C2PlanarLayout *layout /* nonnull */, uint8_t **addr /* nonnull */) {
    const Rect rect{(int32_t)c2Rect.left, (int32_t)c2Rect.top,
                    (int32_t)(c2Rect.left + c2Rect.width) /* right */,
                    (int32_t)(c2Rect.top + c2Rect.height) /* bottom */};

    uint64_t grallocUsage = static_cast<C2AndroidMemoryUsage>(usage).asGrallocUsage();
    ALOGV("mapping buffer with usage %#llx => %#llx",
          (long long)usage.expected, (long long)grallocUsage);

    // TODO
    (void)fence;

    std::lock_guard<std::mutex> lock(mMappedLock);
    if (mBuffer && mLocked) {
        ALOGD("already mapped");
        return C2_DUPLICATE;
    }
    if (!layout || !addr) {
        ALOGD("wrong param");
        return C2_BAD_VALUE;
    }

    if (!mBuffer) {
        status_t err = GraphicBufferMapper::get().importBuffer(
                            mHidlHandle.getNativeHandle(), mWidth, mHeight, mLayerCount,
                            mFormat, mGrallocUsage, mStride, &mBuffer);
        if (err) {
            ALOGE("failed transaction: importBuffer");
            return C2_CORRUPTED;
        }
        if (mBuffer == nullptr) {
            ALOGD("importBuffer returned null buffer");
            return C2_CORRUPTED;
        }
        uint32_t generation = 0;
        uint64_t igbp_id = 0;
        uint32_t igbp_slot = 0;
        if (mHandle) {
            mHandle->getIgbpData(&generation, &igbp_id, &igbp_slot);
        }

        mLockedHandle = C2HandleGralloc::WrapAndMoveNativeHandle(
                mBuffer, mWidth, mHeight, mFormat, mGrallocUsage,
                mStride, generation, igbp_id, igbp_slot);
    }

    c2_status_t ret = PopulatePlaneLayout(
            mBuffer, rect, mFormat, grallocUsage, mStride, layout, addr);
    if (ret != C2_OK) {
        return ret;
    }
    mLocked = true;

    HandleInterleavedPlanes(layout, addr);

    ALOGV("C2AllocationGralloc::map: layout: type=%d numPlanes=%d rootPlanes=%d",
          layout->type, layout->numPlanes, layout->rootPlanes);
    for (int i = 0; i < layout->numPlanes; ++i) {
        const C2PlaneInfo &plane = layout->planes[i];
        ALOGV("C2AllocationGralloc::map: plane[%d]: colInc=%d rowInc=%d rootIx=%u offset=%u",
              i, plane.colInc, plane.rowInc, plane.rootIx, plane.offset);
    }

    return C2_OK;
}

c2_status_t C2AllocationGralloc::unmap(
        uint8_t **addr, C2Rect rect, C2Fence *fence /* nullable */) {
    // TODO: check addr and size, use fence
    (void)addr;
    (void)rect;
    (void)fence;

    std::lock_guard<std::mutex> lock(mMappedLock);
    // TODO: fence
    status_t err = GraphicBufferMapper::get().unlock(mBuffer);
    if (err) {
        ALOGE("failed transaction: unlock");
        return C2_CORRUPTED;
    }

    mLocked = false;
    return C2_OK;
}

bool C2AllocationGralloc::equals(const std::shared_ptr<const C2GraphicAllocation> &other) const {
    return other && other->handle() == handle();
}

/* ===================================== GRALLOC ALLOCATOR ==================================== */
class C2AllocatorGralloc::Impl {
public:
    Impl(id_t id, bool bufferQueue);

    id_t getId() const {
        return mTraits->id;
    }

    C2String getName() const {
        return mTraits->name;
    }

    std::shared_ptr<const C2Allocator::Traits> getTraits() const {
        return mTraits;
    }

    c2_status_t newGraphicAllocation(
            uint32_t width, uint32_t height, uint32_t format, const C2MemoryUsage &usage,
            std::shared_ptr<C2GraphicAllocation> *allocation);

    c2_status_t priorGraphicAllocation(
            const C2Handle *handle,
            std::shared_ptr<C2GraphicAllocation> *allocation);

    c2_status_t status() const { return mInit; }

private:
    std::shared_ptr<C2Allocator::Traits> mTraits;
    c2_status_t mInit;
    const bool mBufferQueue;
};

void _UnwrapNativeCodec2GrallocMetadata(
        const C2Handle *const handle,
        uint32_t *width, uint32_t *height, uint32_t *format,uint64_t *usage, uint32_t *stride,
        uint32_t *generation, uint64_t *igbp_id, uint32_t *igbp_slot) {
    if (C2AllocatorGralloc::CheckHandle(handle)) {
        (void)C2HandleGralloc::Import(handle, width, height, format, usage, stride,
                                      generation, igbp_id, igbp_slot);
        return;
    }
    if (C2AllocatorAhwb::CheckHandle(handle)) {
        uint64_t origId;
        (void)C2HandleAhwb::Import(handle, width, height, format, usage, stride, &origId);
        return;
    }
    ALOGE("Tried to extract metadata from non c2 compatible handle");
}

C2AllocatorGralloc::Impl::Impl(id_t id, bool bufferQueue)
    : mInit(C2_OK), mBufferQueue(bufferQueue) {
    // TODO: get this from allocator
    C2MemoryUsage minUsage = { 0, 0 }, maxUsage = { ~(uint64_t)0, ~(uint64_t)0 };
    Traits traits = { "android.allocator.gralloc", id, C2Allocator::GRAPHIC, minUsage, maxUsage };
    mTraits = std::make_shared<C2Allocator::Traits>(traits);
}

c2_status_t C2AllocatorGralloc::Impl::newGraphicAllocation(
        uint32_t width, uint32_t height, uint32_t format, const C2MemoryUsage &usage,
        std::shared_ptr<C2GraphicAllocation> *allocation) {
    uint64_t grallocUsage = static_cast<C2AndroidMemoryUsage>(usage).asGrallocUsage();
    ALOGV("allocating buffer with usage %#llx => %#llx",
          (long long)usage.expected, (long long)grallocUsage);

    buffer_handle_t buffer;

    uint32_t stride = 0;

    status_t err = GraphicBufferAllocator::get().allocateRawHandle(width, height, format,
            1u /* layer count */, grallocUsage, &buffer, &stride, "C2GrallocAllocation");
    if (err) {
        ALOGE("failed transaction: allocate");
        return C2_CORRUPTED;
    }

    hidl_handle hidlHandle;
    hidlHandle.setTo(const_cast<native_handle_t*>(buffer), true);

    allocation->reset(new C2AllocationGralloc(
            width, height, format, 1u /* layer count */, grallocUsage, stride, hidlHandle,
            C2HandleGralloc::WrapAndMoveNativeHandle(
                    hidlHandle, width, height,
                    format, grallocUsage, stride,
                    0, 0, mBufferQueue ? ~0 : 0),
            mTraits->id));
    return C2_OK;
}

c2_status_t C2AllocatorGralloc::Impl::priorGraphicAllocation(
        const C2Handle *handle,
        std::shared_ptr<C2GraphicAllocation> *allocation) {

    uint32_t generation;
    uint64_t igbp_id;
    uint32_t igbp_slot;

    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t layerCount = 1;
    uint64_t grallocUsage;
    uint32_t stride;

    const C2HandleGralloc *grallocHandle = C2HandleGralloc::Import(
            handle, &width, &height, &format, &grallocUsage, &stride,
            &generation, &igbp_id, &igbp_slot);
    if (grallocHandle == nullptr) {
        return C2_BAD_VALUE;
    }

    hidl_handle hidlHandle;
    hidlHandle.setTo(C2HandleGralloc::UnwrapNativeHandle(grallocHandle), true);

    allocation->reset(new C2AllocationGralloc(
            width, height, format, layerCount,
            grallocUsage, stride, hidlHandle, grallocHandle, mTraits->id));
    return C2_OK;
}

C2AllocatorGralloc::C2AllocatorGralloc(id_t id, bool bufferQueue)
        : mImpl(new Impl(id, bufferQueue)) {}

C2AllocatorGralloc::~C2AllocatorGralloc() { delete mImpl; }

C2Allocator::id_t C2AllocatorGralloc::getId() const {
    return mImpl->getId();
}

C2String C2AllocatorGralloc::getName() const {
    return mImpl->getName();
}

std::shared_ptr<const C2Allocator::Traits> C2AllocatorGralloc::getTraits() const {
    return mImpl->getTraits();
}

c2_status_t C2AllocatorGralloc::newGraphicAllocation(
        uint32_t width, uint32_t height, uint32_t format, C2MemoryUsage usage,
        std::shared_ptr<C2GraphicAllocation> *allocation) {
    return mImpl->newGraphicAllocation(width, height, format, usage, allocation);
}

c2_status_t C2AllocatorGralloc::priorGraphicAllocation(
        const C2Handle *handle,
        std::shared_ptr<C2GraphicAllocation> *allocation) {
    return mImpl->priorGraphicAllocation(handle, allocation);
}

c2_status_t C2AllocatorGralloc::status() const {
    return mImpl->status();
}

// static
bool C2AllocatorGralloc::CheckHandle(const C2Handle* const o) {
    return C2HandleGralloc::IsValid(o);
}


C2Handle *WrapNativeCodec2AhwbHandle(
        const native_handle_t *const handle,
        uint32_t width, uint32_t height, uint32_t format, uint64_t usage, uint32_t stride,
        uint64_t origId) {
    return C2HandleAhwb::WrapNativeHandle(handle, width, height, format, usage, stride,
                                          origId);
}

class C2AllocationAhwb : public C2GraphicAllocation {
public:
    virtual ~C2AllocationAhwb() override;

    virtual c2_status_t map(
            C2Rect c2Rect, C2MemoryUsage usage, C2Fence *fence,
            C2PlanarLayout *layout /* nonnull */, uint8_t **addr /* nonnull */) override;
    virtual c2_status_t unmap(
            uint8_t **addr /* nonnull */, C2Rect rect, C2Fence *fence /* nullable */) override;
    virtual C2Allocator::id_t getAllocatorId() const override { return mAllocatorId; }
    virtual const C2Handle *handle() const override { return mLockedHandle ? : mHandle; }
    virtual bool equals(const std::shared_ptr<const C2GraphicAllocation> &other) const override;

    // internal methods
    // |handle| will be moved.

    C2AllocationAhwb(
              uint32_t width, uint32_t height,
              uint32_t format, uint32_t layerCount,
              uint64_t grallocUsage, uint32_t stride,
              const C2HandleAhwb *const handle,
              C2Allocator::id_t allocatorId);
    int dup() const;
    c2_status_t status() const;

private:
    const uint32_t mWidth;
    const uint32_t mHeight;
    const uint32_t mFormat;
    const uint32_t mLayerCount;
    const uint64_t mGrallocUsage;
    const uint32_t mStride;
    const native_handle_t *mRawHandle;
    const C2HandleAhwb *mHandle;
    buffer_handle_t mBuffer;
    const C2HandleAhwb *mLockedHandle;
    bool mLocked;
    C2Allocator::id_t mAllocatorId;
    std::mutex mMappedLock;
};

C2AllocationAhwb::C2AllocationAhwb(
          uint32_t width, uint32_t height,
          uint32_t format, uint32_t layerCount,
          uint64_t grallocUsage, uint32_t stride,
          const C2HandleAhwb *const handle,
          C2Allocator::id_t allocatorId)
    : C2GraphicAllocation(width, height),
      mWidth(width),
      mHeight(height),
      mFormat(format),
      mLayerCount(layerCount),
      mGrallocUsage(grallocUsage),
      mStride(stride),
      mRawHandle(C2HandleAhwb::UnwrapNativeHandle(handle)),
      mHandle(handle),
      mBuffer(nullptr),
      mLockedHandle(nullptr),
      mLocked(false),
      mAllocatorId(allocatorId) {
}

C2AllocationAhwb::~C2AllocationAhwb() {
    if (mBuffer && mLocked) {
        // implementation ignores address and rect
        uint8_t* addr[C2PlanarLayout::MAX_NUM_PLANES] = {};
        unmap(addr, C2Rect(), nullptr);
    }
    if (mBuffer) {
        status_t err = GraphicBufferMapper::get().freeBuffer(mBuffer);
        if (err) {
            ALOGE("failed transaction: freeBuffer");
        }
    }
    if (mRawHandle) {
        native_handle_close(
                const_cast<native_handle_t *>(
                        reinterpret_cast<const native_handle_t *>(mRawHandle)));
        native_handle_delete(
                const_cast<native_handle_t *>(
                        reinterpret_cast<const native_handle_t *>(mRawHandle)));
    }
    if (mHandle) {
        native_handle_delete(
                const_cast<native_handle_t *>(reinterpret_cast<const native_handle_t *>(mHandle)));
    }
    if (mLockedHandle) {
        native_handle_delete(
                const_cast<native_handle_t *>(
                        reinterpret_cast<const native_handle_t *>(mLockedHandle)));
    }
}

c2_status_t C2AllocationAhwb::map(
        C2Rect c2Rect, C2MemoryUsage usage, C2Fence *fence,
        C2PlanarLayout *layout /* nonnull */, uint8_t **addr /* nonnull */) {
    const Rect rect{(int32_t)c2Rect.left, (int32_t)c2Rect.top,
                    (int32_t)(c2Rect.left + c2Rect.width) /* right */,
                    (int32_t)(c2Rect.top + c2Rect.height) /* bottom */};

    uint64_t grallocUsage = static_cast<C2AndroidMemoryUsage>(usage).asGrallocUsage();
    ALOGV("mapping buffer with usage %#llx => %#llx",
          (long long)usage.expected, (long long)grallocUsage);

    // TODO
    (void)fence;

    std::lock_guard<std::mutex> lock(mMappedLock);
    if (mBuffer && mLocked) {
        ALOGD("already mapped");
        return C2_DUPLICATE;
    }
    if (!layout || !addr) {
        ALOGD("wrong param");
        return C2_BAD_VALUE;
    }

    if (!mBuffer) {
        // TODO: libui/libgui dependency removal (b/214400477)
        status_t err = GraphicBufferMapper::get().importBuffer(
                            mRawHandle, mWidth, mHeight, mLayerCount,
                            mFormat, mGrallocUsage, mStride, &mBuffer);
        if (err) {
            ALOGE("failed transaction: importBuffer");
            return C2_CORRUPTED;
        }
        if (mBuffer == nullptr) {
            ALOGD("importBuffer returned null buffer");
            return C2_CORRUPTED;
        }
        uint64_t origId = 0;
        if (mHandle) {
            mHandle->getOrigId(&origId);
        }

        mLockedHandle = C2HandleAhwb::WrapAndMoveNativeHandle(
                mBuffer, mWidth, mHeight, mFormat, mGrallocUsage,
                mStride, origId);
    }

    c2_status_t ret = PopulatePlaneLayout(
            mBuffer, rect, mFormat, grallocUsage, mStride, layout, addr);
    if (ret != C2_OK) {
        return ret;
    }
    mLocked = true;

    HandleInterleavedPlanes(layout, addr);

    ALOGV("C2AllocationGralloc::map: layout: type=%d numPlanes=%d rootPlanes=%d",
          layout->type, layout->numPlanes, layout->rootPlanes);
    for (int i = 0; i < layout->numPlanes; ++i) {
        const C2PlaneInfo &plane = layout->planes[i];
        ALOGV("C2AllocationGralloc::map: plane[%d]: colInc=%d rowInc=%d rootIx=%u offset=%u",
              i, plane.colInc, plane.rowInc, plane.rootIx, plane.offset);
    }

    return C2_OK;
}

c2_status_t C2AllocationAhwb::unmap(
        uint8_t **addr, C2Rect rect, C2Fence *fence /* nullable */) {
    // TODO: check addr and size, use fence
    (void)addr;
    (void)rect;
    (void)fence;

    std::lock_guard<std::mutex> lock(mMappedLock);
    // TODO: fence
    status_t err = GraphicBufferMapper::get().unlock(mBuffer);
    if (err) {
        ALOGE("failed transaction: unlock");
        return C2_CORRUPTED;
    }

    mLocked = false;
    return C2_OK;
}

bool C2AllocationAhwb::equals(const std::shared_ptr<const C2GraphicAllocation> &other) const {
    return other && other->handle() == handle();
}

/* ===================================== AHARDWAREBUFFER ALLOCATOR ============================= */
class C2AllocatorAhwb::Impl {
public:
    Impl(id_t id);

    id_t getId() const {
        return mTraits->id;
    }

    C2String getName() const {
        return mTraits->name;
    }

    std::shared_ptr<const C2Allocator::Traits> getTraits() const {
        return mTraits;
    }

    c2_status_t newGraphicAllocation(
            uint32_t width, uint32_t height, uint32_t format, const C2MemoryUsage &usage,
            std::shared_ptr<C2GraphicAllocation> *allocation);

    c2_status_t priorGraphicAllocation(
            const C2Handle *handle,
            std::shared_ptr<C2GraphicAllocation> *allocation);

    c2_status_t status() const { return mInit; }

private:
    std::shared_ptr<C2Allocator::Traits> mTraits;
    c2_status_t mInit;
};

C2AllocatorAhwb::Impl::Impl(id_t id)
    : mInit(C2_OK) {
    // TODO: get this from allocator
    C2MemoryUsage minUsage = { 0, 0 }, maxUsage = { ~(uint64_t)0, ~(uint64_t)0 };
    Traits traits = { "android.allocator.ahwb", id, C2Allocator::GRAPHIC, minUsage, maxUsage };
    mTraits = std::make_shared<C2Allocator::Traits>(traits);
}

c2_status_t C2AllocatorAhwb::Impl::newGraphicAllocation(
        uint32_t width, uint32_t height, uint32_t format, const C2MemoryUsage &usage,
        std::shared_ptr<C2GraphicAllocation> *allocation) {
    // TODO: for client side usage
    // HAL side Ahwb allocation should be done via IGBA currently.
    (void) width;
    (void) height;
    (void) format;
    (void) usage;
    (void) allocation;
    return C2_OMITTED;
}

c2_status_t C2AllocatorAhwb::Impl::priorGraphicAllocation(
        const C2Handle *handle,
        std::shared_ptr<C2GraphicAllocation> *allocation) {

    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t layerCount = 1;
    uint64_t grallocUsage;
    uint32_t stride;
    uint64_t origId;

    const C2HandleAhwb *ahwbHandle = C2HandleAhwb::Import(
            handle, &width, &height, &format, &grallocUsage, &stride, &origId);
    if (ahwbHandle == nullptr) {
        return C2_BAD_VALUE;
    }

    allocation->reset(new C2AllocationAhwb(
            width, height, format, layerCount,
            grallocUsage, stride, ahwbHandle, mTraits->id));
    return C2_OK;
}

C2AllocatorAhwb::C2AllocatorAhwb(id_t id)
        : mImpl(new Impl(id)) {}

C2AllocatorAhwb::~C2AllocatorAhwb() { delete mImpl; }

C2Allocator::id_t C2AllocatorAhwb::getId() const {
    return mImpl->getId();
}

C2String C2AllocatorAhwb::getName() const {
    return mImpl->getName();
}

std::shared_ptr<const C2Allocator::Traits> C2AllocatorAhwb::getTraits() const {
    return mImpl->getTraits();
}

c2_status_t C2AllocatorAhwb::newGraphicAllocation(
        uint32_t width, uint32_t height, uint32_t format, C2MemoryUsage usage,
        std::shared_ptr<C2GraphicAllocation> *allocation) {
    return mImpl->newGraphicAllocation(width, height, format, usage, allocation);
}

c2_status_t C2AllocatorAhwb::priorGraphicAllocation(
        const C2Handle *handle,
        std::shared_ptr<C2GraphicAllocation> *allocation) {
    return mImpl->priorGraphicAllocation(handle, allocation);
}

c2_status_t C2AllocatorAhwb::status() const {
    return mImpl->status();
}

// static
bool C2AllocatorAhwb::CheckHandle(const C2Handle* const o) {
    return C2HandleAhwb::IsValid(o);
}
} // namespace android
