// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

struct DescriptorUpdateEntry {
    struct Empty {};

    DescriptorUpdateEntry() = default;
    DescriptorUpdateEntry(VkDescriptorImageInfo image_) : image{image_} {}
    DescriptorUpdateEntry(VkDescriptorBufferInfo buffer_) : buffer{buffer_} {}
    DescriptorUpdateEntry(VkBufferView texel_buffer_) : texel_buffer{texel_buffer_} {}

    union {
        Empty empty{};
        VkDescriptorImageInfo image;
        VkDescriptorBufferInfo buffer;
        VkBufferView texel_buffer;
    };
};

class UpdateDescriptorQueue final {
    // This should be plenty for the vast majority of cases. Most desktop platforms only
    // provide up to 3 swapchain images.
    static constexpr size_t FRAMES_IN_FLIGHT = 8;
    static constexpr size_t FRAME_PAYLOAD_SIZE = 0x20000;
    static constexpr size_t PAYLOAD_SIZE = FRAME_PAYLOAD_SIZE * FRAMES_IN_FLIGHT;

public:
    explicit UpdateDescriptorQueue(const Device& device_, Scheduler& scheduler_);
    ~UpdateDescriptorQueue();

    void TickFrame();

    void Acquire();

    const DescriptorUpdateEntry* UpdateData() const noexcept {
        return upload_start;
    }

    void AddSampledImage(int stage, int binding_index, VkImageView image_view, VkSampler sampler) {
        LogAddSampledImage(stage, binding_index, image_view, sampler);

        *(payload_cursor++) = VkDescriptorImageInfo{
            .sampler = sampler,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
    }

    void AddImage(int stage, int binding_index, VkImageView image_view) {
        LogAddImage(stage, binding_index, image_view);

        *(payload_cursor++) = VkDescriptorImageInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
    }

    void AddBuffer(int stage, int binding_index, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
        LogAddBuffer(stage, binding_index, buffer, offset, size);

        *(payload_cursor++) = VkDescriptorBufferInfo{
            .buffer = buffer,
            .offset = offset,
            .range = size,
        };
    }

    void AddTexelBuffer(int stage, int binding_index, VkBufferView texel_buffer) {
        LogAddTexelBuffer(stage, binding_index, texel_buffer);

        *(payload_cursor++) = texel_buffer;
    }

private:
    void LogAddSampledImage(int stage, int binding_index, VkImageView image_view, VkSampler sampler);
    void LogAddImage(int stage, int binding_index, VkImageView image_view);
    void LogAddBuffer(int stage, int binding_index, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size);
    void LogAddTexelBuffer(int stage, int binding_index, VkBufferView texel_buffer);

private:
    const Device& device;
    Scheduler& scheduler;

    size_t frame_index{0};
    DescriptorUpdateEntry* payload_cursor = nullptr;
    DescriptorUpdateEntry* payload_start = nullptr;
    const DescriptorUpdateEntry* upload_start = nullptr;
    std::array<DescriptorUpdateEntry, PAYLOAD_SIZE> payload;
};

// TODO: should these be separate classes instead?
using GuestDescriptorQueue = UpdateDescriptorQueue;
using ComputePassDescriptorQueue = UpdateDescriptorQueue;

} // namespace Vulkan
