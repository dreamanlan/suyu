// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <variant>
#include <boost/container/static_vector.hpp>

#include "common/logging/log.h"
#include "core/memory/debug_script/DbgScpHook.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

UpdateDescriptorQueue::UpdateDescriptorQueue(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {
    payload_start = payload.data();
    payload_cursor = payload.data();
}

UpdateDescriptorQueue::~UpdateDescriptorQueue() = default;

void UpdateDescriptorQueue::TickFrame() {
    if (++frame_index >= FRAMES_IN_FLIGHT) {
        frame_index = 0;
    }
    payload_start = payload.data() + frame_index * FRAME_PAYLOAD_SIZE;
    payload_cursor = payload_start;
}

void UpdateDescriptorQueue::Acquire() {
    // Minimum number of entries required.
    // This is the maximum number of entries a single draw call might use.
    static constexpr size_t MIN_ENTRIES = 0x400;

    if (std::distance(payload_start, payload_cursor) + MIN_ENTRIES >= FRAME_PAYLOAD_SIZE) {
        LOG_WARNING(Render_Vulkan, "Payload overflow, waiting for worker thread");
        scheduler.WaitWorker();
        payload_cursor = payload_start;
    }
    upload_start = payload_cursor;
}

void UpdateDescriptorQueue::LogAddSampledImage(VkImageView image_view, VkSampler sampler) {
    bool log = false;
    auto&& pThis = this;
    DBGSCP_HOOK_VOID("UpdateDescriptorQueue::LogAddSampledImage", log, pThis, image_view, sampler);

    if (log) {
        LOG_INFO(Render_Vulkan, "image_view:{:016x} sampler:{:016x} layout:{}",
                 reinterpret_cast<u64>(image_view), reinterpret_cast<u64>(sampler),
                 VK_IMAGE_LAYOUT_GENERAL);
    }
}

void UpdateDescriptorQueue::LogAddImage(VkImageView image_view) {
    bool log = false;
    auto&& pThis = this;
    DBGSCP_HOOK_VOID("UpdateDescriptorQueue::LogAddImage", log, pThis, image_view);

    if (log) {
        LOG_INFO(Render_Vulkan, "image_view:{:016x} sampler:{:016x} layout:{}",
                 reinterpret_cast<u64>(image_view), reinterpret_cast<u64>(VK_NULL_HANDLE),
                 VK_IMAGE_LAYOUT_GENERAL);
    }
}

void UpdateDescriptorQueue::LogAddBuffer(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
    bool log = false;
    auto&& pThis = this;
    DBGSCP_HOOK_VOID("UpdateDescriptorQueue::LogAddBuffer", log, pThis, buffer, offset, size);

    if (log) {
        LOG_INFO(Render_Vulkan, "buffer:{:016x} offset:0x{:x} range:{}", reinterpret_cast<u64>(buffer), offset, size);
    }
}

void UpdateDescriptorQueue::LogAddTexelBuffer(VkBufferView texel_buffer) {
    bool log = false;
    auto&& pThis = this;
    DBGSCP_HOOK_VOID("UpdateDescriptorQueue::LogAddTexelBuffer", log, pThis, texel_buffer);

    if (log) {
        LOG_INFO(Render_Vulkan, "buffer view:{:016x}", reinterpret_cast<u64>(texel_buffer));
    }
}

} // namespace Vulkan
