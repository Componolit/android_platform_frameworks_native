/*
 * Copyright 2019 The Android Open Source Project
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

#define LOG_TAG "Gralloc4"

#include <hidl/ServiceManagement.h>
#include <hwbinder/IPCThreadState.h>
#include <ui/Gralloc4.h>

#include <inttypes.h>
#include <log/log.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-length-array"
#include <sync/sync.h>
#pragma clang diagnostic pop

using android::hardware::graphics::allocator::V4_0::IAllocator;
using android::hardware::graphics::common::V1_2::BufferUsage;
using android::hardware::graphics::mapper::V4_0::BufferDescriptor;
using android::hardware::graphics::mapper::V4_0::Error;
using android::hardware::graphics::mapper::V4_0::IMapper;

namespace android {

namespace {

static constexpr Error kTransactionError = Error::NO_RESOURCES;

uint64_t getValidUsageBits() {
    static const uint64_t validUsageBits = []() -> uint64_t {
        uint64_t bits = 0;
        for (const auto bit :
             hardware::hidl_enum_range<hardware::graphics::common::V1_2::BufferUsage>()) {
            bits = bits | bit;
        }
        return bits;
    }();
    return validUsageBits;
}

static inline IMapper::Rect sGralloc4Rect(const Rect& rect) {
    IMapper::Rect outRect{};
    outRect.left = rect.left;
    outRect.top = rect.top;
    outRect.width = rect.width();
    outRect.height = rect.height();
    return outRect;
}
static inline void sBufferDescriptorInfo(uint32_t width, uint32_t height,
                                         android::PixelFormat format, uint32_t layerCount,
                                         uint64_t usage,
                                         IMapper::BufferDescriptorInfo* outDescriptorInfo) {
    outDescriptorInfo->width = width;
    outDescriptorInfo->height = height;
    outDescriptorInfo->layerCount = layerCount;
    outDescriptorInfo->format = static_cast<hardware::graphics::common::V1_2::PixelFormat>(format);
    outDescriptorInfo->usage = usage;
}

} // anonymous namespace

void Gralloc4Mapper::preload() {
    android::hardware::preloadPassthroughService<IMapper>();
}

Gralloc4Mapper::Gralloc4Mapper() {
    mMapper = IMapper::getService();
    if (mMapper == nullptr) {
        ALOGI("mapper 4.x is not supported");
        return;
    }
    if (mMapper->isRemote()) {
        LOG_ALWAYS_FATAL("gralloc-mapper must be in passthrough mode");
    }
}

bool Gralloc4Mapper::isLoaded() const {
    return mMapper != nullptr;
}

status_t Gralloc4Mapper::validateBufferDescriptorInfo(
        IMapper::BufferDescriptorInfo* descriptorInfo) const {
    uint64_t validUsageBits = getValidUsageBits();

    if (descriptorInfo->usage & ~validUsageBits) {
        ALOGE("buffer descriptor contains invalid usage bits 0x%" PRIx64,
              descriptorInfo->usage & ~validUsageBits);
        return BAD_VALUE;
    }
    return NO_ERROR;
}

status_t Gralloc4Mapper::createDescriptor(void* bufferDescriptorInfo,
                                          void* outBufferDescriptor) const {
    IMapper::BufferDescriptorInfo* descriptorInfo =
            static_cast<IMapper::BufferDescriptorInfo*>(bufferDescriptorInfo);
    BufferDescriptor* outDescriptor = static_cast<BufferDescriptor*>(outBufferDescriptor);

    status_t status = validateBufferDescriptorInfo(descriptorInfo);
    if (status != NO_ERROR) {
        return status;
    }

    Error error;
    auto hidl_cb = [&](const auto& tmpError, const auto& tmpDescriptor) {
        error = tmpError;
        if (error != Error::NONE) {
            return;
        }
        *outDescriptor = tmpDescriptor;
    };

    hardware::Return<void> ret = mMapper->createDescriptor(*descriptorInfo, hidl_cb);

    return static_cast<status_t>((ret.isOk()) ? error : kTransactionError);
}

status_t Gralloc4Mapper::importBuffer(const hardware::hidl_handle& rawHandle,
                                      buffer_handle_t* outBufferHandle) const {
    Error error;
    auto ret = mMapper->importBuffer(rawHandle, [&](const auto& tmpError, const auto& tmpBuffer) {
        error = tmpError;
        if (error != Error::NONE) {
            return;
        }
        *outBufferHandle = static_cast<buffer_handle_t>(tmpBuffer);
    });

    return static_cast<status_t>((ret.isOk()) ? error : kTransactionError);
}

void Gralloc4Mapper::freeBuffer(buffer_handle_t bufferHandle) const {
    auto buffer = const_cast<native_handle_t*>(bufferHandle);
    auto ret = mMapper->freeBuffer(buffer);

    auto error = (ret.isOk()) ? static_cast<Error>(ret) : kTransactionError;
    ALOGE_IF(error != Error::NONE, "freeBuffer(%p) failed with %d", buffer, error);
}

status_t Gralloc4Mapper::validateBufferSize(buffer_handle_t bufferHandle, uint32_t width,
                                            uint32_t height, android::PixelFormat format,
                                            uint32_t layerCount, uint64_t usage,
                                            uint32_t stride) const {
    IMapper::BufferDescriptorInfo descriptorInfo;
    sBufferDescriptorInfo(width, height, format, layerCount, usage, &descriptorInfo);

    auto buffer = const_cast<native_handle_t*>(bufferHandle);
    auto ret = mMapper->validateBufferSize(buffer, descriptorInfo, stride);

    return static_cast<status_t>((ret.isOk()) ? static_cast<Error>(ret) : kTransactionError);
}

void Gralloc4Mapper::getTransportSize(buffer_handle_t bufferHandle, uint32_t* outNumFds,
                                      uint32_t* outNumInts) const {
    *outNumFds = uint32_t(bufferHandle->numFds);
    *outNumInts = uint32_t(bufferHandle->numInts);

    Error error;
    auto buffer = const_cast<native_handle_t*>(bufferHandle);
    auto ret = mMapper->getTransportSize(buffer,
                                         [&](const auto& tmpError, const auto& tmpNumFds,
                                             const auto& tmpNumInts) {
                                             error = tmpError;
                                             if (error != Error::NONE) {
                                                 return;
                                             }
                                             *outNumFds = tmpNumFds;
                                             *outNumInts = tmpNumInts;
                                         });

    error = (ret.isOk()) ? error : kTransactionError;

    ALOGE_IF(error != Error::NONE, "getTransportSize(%p) failed with %d", buffer, error);
}

status_t Gralloc4Mapper::lock(buffer_handle_t bufferHandle, uint64_t usage, const Rect& bounds,
                              int acquireFence, void** outData, int32_t* outBytesPerPixel,
                              int32_t* outBytesPerStride) const {
    // In Gralloc 4 we can get this info per plane. Clients should check per plane.
    if (outBytesPerPixel) {
        // TODO add support to check per plane
        *outBytesPerPixel = -1;
    }
    if (outBytesPerStride) {
        // TODO add support to check per plane
        *outBytesPerStride = -1;
    }

    auto buffer = const_cast<native_handle_t*>(bufferHandle);

    IMapper::Rect accessRegion = sGralloc4Rect(bounds);

    // put acquireFence in a hidl_handle
    hardware::hidl_handle acquireFenceHandle;
    NATIVE_HANDLE_DECLARE_STORAGE(acquireFenceStorage, 1, 0);
    if (acquireFence >= 0) {
        auto h = native_handle_init(acquireFenceStorage, 1, 0);
        h->data[0] = acquireFence;
        acquireFenceHandle = h;
    }

    Error error;
    auto ret = mMapper->lock(buffer, usage, accessRegion, acquireFenceHandle,
                             [&](const auto& tmpError, const auto& tmpData) {
                                 error = tmpError;
                                 if (error != Error::NONE) {
                                     return;
                                 }
                                 *outData = tmpData;
                             });

    // we own acquireFence even on errors
    if (acquireFence >= 0) {
        close(acquireFence);
    }

    error = (ret.isOk()) ? error : kTransactionError;

    ALOGW_IF(error != Error::NONE, "lock(%p, ...) failed: %d", bufferHandle, error);

    return static_cast<status_t>(error);
}

status_t Gralloc4Mapper::lock(buffer_handle_t /*bufferHandle*/, uint64_t /*usage*/,
                              const Rect& /*bounds*/, int /*acquireFence*/,
                              android_ycbcr* /*ycbcr*/) const {
    // TODO add lockYCbCr support
    return static_cast<status_t>(Error::UNSUPPORTED);
}

int Gralloc4Mapper::unlock(buffer_handle_t bufferHandle) const {
    auto buffer = const_cast<native_handle_t*>(bufferHandle);

    int releaseFence = -1;
    Error error;
    auto ret = mMapper->unlock(buffer, [&](const auto& tmpError, const auto& tmpReleaseFence) {
        error = tmpError;
        if (error != Error::NONE) {
            return;
        }

        auto fenceHandle = tmpReleaseFence.getNativeHandle();
        if (fenceHandle && fenceHandle->numFds == 1) {
            int fd = dup(fenceHandle->data[0]);
            if (fd >= 0) {
                releaseFence = fd;
            } else {
                ALOGD("failed to dup unlock release fence");
                sync_wait(fenceHandle->data[0], -1);
            }
        }
    });

    if (!ret.isOk()) {
        error = kTransactionError;
    }

    if (error != Error::NONE) {
        ALOGE("unlock(%p) failed with %d", buffer, error);
    }

    return releaseFence;
}

status_t Gralloc4Mapper::isSupported(uint32_t width, uint32_t height, android::PixelFormat format,
                                     uint32_t layerCount, uint64_t usage,
                                     bool* outSupported) const {
    IMapper::BufferDescriptorInfo descriptorInfo;
    sBufferDescriptorInfo(width, height, format, layerCount, usage, &descriptorInfo);

    Error error;
    auto ret = mMapper->isSupported(descriptorInfo,
                                    [&](const auto& tmpError, const auto& tmpSupported) {
                                        error = tmpError;
                                        if (error != Error::NONE) {
                                            return;
                                        }
                                        if (outSupported) {
                                            *outSupported = tmpSupported;
                                        }
                                    });

    if (!ret.isOk()) {
        error = kTransactionError;
    }

    if (error != Error::NONE) {
        ALOGE("isSupported(%u, %u, %d, %u, ...) failed with %d", width, height, format, layerCount,
              error);
    }

    return static_cast<status_t>(error);
}

Gralloc4Allocator::Gralloc4Allocator(const Gralloc4Mapper& mapper) : mMapper(mapper) {
    mAllocator = IAllocator::getService();
    if (mAllocator == nullptr) {
        ALOGW("allocator 3.x is not supported");
        return;
    }
}

bool Gralloc4Allocator::isLoaded() const {
    return mAllocator != nullptr;
}

std::string Gralloc4Allocator::dumpDebugInfo() const {
    std::string debugInfo;

    mAllocator->dumpDebugInfo([&](const auto& tmpDebugInfo) { debugInfo = tmpDebugInfo.c_str(); });

    return debugInfo;
}

status_t Gralloc4Allocator::allocate(uint32_t width, uint32_t height, android::PixelFormat format,
                                     uint32_t layerCount, uint64_t usage, uint32_t bufferCount,
                                     uint32_t* outStride, buffer_handle_t* outBufferHandles) const {
    IMapper::BufferDescriptorInfo descriptorInfo;
    sBufferDescriptorInfo(width, height, format, layerCount, usage, &descriptorInfo);

    BufferDescriptor descriptor;
    status_t error = mMapper.createDescriptor(static_cast<void*>(&descriptorInfo),
                                              static_cast<void*>(&descriptor));
    if (error != NO_ERROR) {
        return error;
    }

    auto ret = mAllocator->allocate(descriptor, bufferCount,
                                    [&](const auto& tmpError, const auto& tmpStride,
                                        const auto& tmpBuffers) {
                                        error = static_cast<status_t>(tmpError);
                                        if (tmpError != Error::NONE) {
                                            return;
                                        }

                                        // import buffers
                                        for (uint32_t i = 0; i < bufferCount; i++) {
                                            error = mMapper.importBuffer(tmpBuffers[i],
                                                                         &outBufferHandles[i]);
                                            if (error != NO_ERROR) {
                                                for (uint32_t j = 0; j < i; j++) {
                                                    mMapper.freeBuffer(outBufferHandles[j]);
                                                    outBufferHandles[j] = nullptr;
                                                }
                                                return;
                                            }
                                        }
                                        *outStride = tmpStride;
                                    });

    // make sure the kernel driver sees BC_FREE_BUFFER and closes the fds now
    hardware::IPCThreadState::self()->flushCommands();

    return (ret.isOk()) ? error : static_cast<status_t>(kTransactionError);
}

} // namespace android
