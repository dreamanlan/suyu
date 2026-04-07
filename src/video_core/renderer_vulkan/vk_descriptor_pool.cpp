// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/polyfill_ranges.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

// Prefer small grow rates to avoid saturating the descriptor pool with barely used pipelines
constexpr size_t SETS_GROW_RATE = 16;
constexpr s32 SCORE_THRESHOLD = 3;

struct DescriptorBank {
    DescriptorBankInfo info;
    std::vector<vk::DescriptorPool> pools;
    bool use_separated{};
};

bool DescriptorBankInfo::IsSuperset(const DescriptorBankInfo& subset) const noexcept {
    return uniform_buffers >= subset.uniform_buffers && storage_buffers >= subset.storage_buffers &&
           texture_buffers >= subset.texture_buffers && image_buffers >= subset.image_buffers &&
           textures >= subset.textures && images >= subset.images;
}

template <typename Descriptors>
static u32 Accumulate(const Descriptors& descriptors) {
    u32 count = 0;
    for (const auto& descriptor : descriptors) {
        count += descriptor.count;
    }
    return count;
}

bool IsSeparated(const Device& device) {
    const auto texture_pool_mode = Settings::values.texture_pool_mode.GetValue();
    switch (texture_pool_mode) {
    case Settings::TexturePoolMode::Automatic:
        // Match the logic from spirv_emit_context.cpp
        if (device.IsTexturePoolSupported()) {
            // Pooled mode uses separated descriptors (VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE + VK_DESCRIPTOR_TYPE_SAMPLER)
            return true;
        } else {
#ifdef __APPLE__
            // Metal: use separated mode to avoid 16-sampler limit
            return true;
#else
            // Traditional combined mode
            return false;
#endif
        }
    case Settings::TexturePoolMode::Combined:
        return false;
    case Settings::TexturePoolMode::Separated:
    case Settings::TexturePoolMode::Pooled:
        // Both Separated and Pooled modes use separated descriptors
        return true;
    }
    return false;
}

static DescriptorBankInfo MakeBankInfo(std::span<const Shader::Info> infos) {
    DescriptorBankInfo bank;
    for (const Shader::Info& info : infos) {
        bank.uniform_buffers += Accumulate(info.constant_buffer_descriptors);
        bank.storage_buffers += Accumulate(info.storage_buffers_descriptors);
        bank.texture_buffers += Accumulate(info.texture_buffer_descriptors);
        bank.image_buffers += Accumulate(info.image_buffer_descriptors);
        bank.textures += Accumulate(info.texture_descriptors);
        bank.images += Accumulate(info.image_descriptors);
    }
    bank.score = bank.uniform_buffers + bank.storage_buffers + bank.texture_buffers +
                 bank.image_buffers + bank.textures + bank.images;
    return bank;
}

static void AllocatePool(const Device& device, DescriptorBank& bank) {
    std::array<VkDescriptorPoolSize, 7> pool_sizes;
    size_t pool_cursor{};
    const u32 sets_per_pool = device.GetSetsPerPool();
    const auto add = [&](VkDescriptorType type, u32 count) {
        if (count > 0) {
            pool_sizes[pool_cursor++] = {
                .type = type,
                .descriptorCount = count * sets_per_pool,
            };
        }
    };
    const auto& info{bank.info};
    add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, info.uniform_buffers);
    add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, info.storage_buffers);
    add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, info.texture_buffers);
    add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, info.image_buffers);

    const bool use_separated = IsSeparated(device) && !info.force_combined;
    bank.use_separated = use_separated;

    if (use_separated) {
        add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, info.textures);
        add(VK_DESCRIPTOR_TYPE_SAMPLER, info.textures);
    } else {
        add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, info.textures);
    }

    add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, info.images);
    bank.pools.push_back(device.GetLogical().CreateDescriptorPool({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = sets_per_pool,
        .poolSizeCount = static_cast<u32>(pool_cursor),
        .pPoolSizes = std::data(pool_sizes),
    }));
}

DescriptorAllocator::DescriptorAllocator(const Device& device_, MasterSemaphore& master_semaphore_,
                                         DescriptorBank& bank_, VkDescriptorSetLayout layout_)
    : ResourcePool(master_semaphore_, SETS_GROW_RATE), device{&device_}, bank{&bank_},
      layout{layout_} {}

VkDescriptorSet DescriptorAllocator::Commit() {
    const size_t index = CommitResource();
    return sets[index / SETS_GROW_RATE][index % SETS_GROW_RATE];
}

void DescriptorAllocator::Allocate(size_t begin, size_t end) {
    sets.push_back(AllocateDescriptors(end - begin));
}

vk::DescriptorSets DescriptorAllocator::AllocateDescriptors(size_t count) {
    const std::vector<VkDescriptorSetLayout> layouts(count, layout);
    VkDescriptorSetAllocateInfo allocate_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = *bank->pools.back(),
        .descriptorSetCount = static_cast<u32>(count),
        .pSetLayouts = layouts.data(),
    };
    vk::DescriptorSets new_sets = bank->pools.back().Allocate(allocate_info);
    if (!new_sets.IsOutOfPoolMemory()) {
        return new_sets;
    }
    // Our current pool is out of memory. Allocate a new one and retry
    AllocatePool(*device, *bank);
    allocate_info.descriptorPool = *bank->pools.back();
    new_sets = bank->pools.back().Allocate(allocate_info);
    if (!new_sets.IsOutOfPoolMemory()) {
        return new_sets;
    }
    // After allocating a new pool, we are out of memory again. We can't handle this from here.
    throw vk::Exception(VK_ERROR_OUT_OF_POOL_MEMORY);
}

DescriptorPool::DescriptorPool(const Device& device_, Scheduler& scheduler)
    : device{device_}, master_semaphore{scheduler.GetMasterSemaphore()} {}

DescriptorPool::~DescriptorPool() = default;

DescriptorAllocator DescriptorPool::Allocator(VkDescriptorSetLayout layout,
                                              std::span<const Shader::Info> infos) {
    return Allocator(layout, MakeBankInfo(infos));
}

DescriptorAllocator DescriptorPool::Allocator(VkDescriptorSetLayout layout,
                                              const Shader::Info& info) {
    return Allocator(layout, MakeBankInfo(std::array{info}));
}

DescriptorAllocator DescriptorPool::Allocator(VkDescriptorSetLayout layout,
                                              const DescriptorBankInfo& info) {
    return DescriptorAllocator(device, master_semaphore, Bank(info), layout);
}

DescriptorBank& DescriptorPool::Bank(const DescriptorBankInfo& reqs) {
    const bool use_separated = IsSeparated(device) && !reqs.force_combined;
    std::shared_lock read_lock{banks_mutex};
    const auto it = std::ranges::find_if(bank_infos, [&](const DescriptorBankInfo& bank) {
        const size_t index = &bank - bank_infos.data();
        const auto& current_bank = *banks[index];
        return current_bank.use_separated == use_separated &&
               std::abs(bank.score - reqs.score) < SCORE_THRESHOLD && bank.IsSuperset(reqs);
    });
    if (it != bank_infos.end()) {
        return *banks[std::distance(bank_infos.begin(), it)].get();
    }
    read_lock.unlock();

    std::unique_lock write_lock{banks_mutex};
    bank_infos.push_back(reqs);

    auto& bank = *banks.emplace_back(std::make_unique<DescriptorBank>());
    bank.info = reqs;
    AllocatePool(device, bank);
    return bank;
}

} // namespace Vulkan
