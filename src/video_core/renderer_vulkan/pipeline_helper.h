// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/texture_cache/types.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

using Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS;

// Sampler key for separated sampler approach (cross-platform)
// Used to identify unique samplers across textures for deduplication
struct SamplerKey {
    u32 cbuf_index;
    u32 cbuf_offset;
    u32 secondary_cbuf_index;
    u32 secondary_cbuf_offset;
    bool has_secondary;

    bool operator<(const SamplerKey& other) const {
        return std::tie(cbuf_index, cbuf_offset, secondary_cbuf_index,
                      secondary_cbuf_offset, has_secondary) <
               std::tie(other.cbuf_index, other.cbuf_offset, other.secondary_cbuf_index,
                      other.secondary_cbuf_offset, other.has_secondary);
    }
};

class DescriptorLayoutBuilder {
public:
    DescriptorLayoutBuilder(const Device& device_) : device{&device_} {}

    bool CanUsePushDescriptor() const noexcept {
#ifdef __APPLE__
        return device->IsKhrPushDescriptorSupported() &&
               num_descriptors <= device->MaxPushDescriptors();
#else
        return device->IsKhrPushDescriptorSupported() &&
               num_descriptors <= device->MaxPushDescriptors();
#endif
    }

    vk::DescriptorSetLayout CreateDescriptorSetLayout(bool use_push_descriptor) const {
        if (bindings.empty()) {
            return nullptr;
        }
        const VkDescriptorSetLayoutCreateFlags flags =
            use_push_descriptor ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
        return device->GetLogical().CreateDescriptorSetLayout({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = flags,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
    }

    vk::DescriptorUpdateTemplate CreateTemplate(VkDescriptorSetLayout descriptor_set_layout,
                                                VkPipelineLayout pipeline_layout,
                                                bool use_push_descriptor) const {
        if (entries.empty()) {
            return nullptr;
        }
        const VkDescriptorUpdateTemplateType type =
            use_push_descriptor ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
                                : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        return device->GetLogical().CreateDescriptorUpdateTemplate({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .descriptorUpdateEntryCount = static_cast<u32>(entries.size()),
            .pDescriptorUpdateEntries = entries.data(),
            .templateType = type,
            .descriptorSetLayout = descriptor_set_layout,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .pipelineLayout = pipeline_layout,
            .set = 0,
        });
    }

    vk::PipelineLayout CreatePipelineLayout(VkDescriptorSetLayout descriptor_set_layout) const {
        using Shader::Backend::SPIRV::RenderAreaLayout;
        using Shader::Backend::SPIRV::RescalingLayout;
        const u32 size_offset = is_compute ? sizeof(RescalingLayout::down_factor) : 0u;
        const VkPushConstantRange range{
            .stageFlags = static_cast<VkShaderStageFlags>(
                is_compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS),
            .offset = 0,
            .size = static_cast<u32>(sizeof(RescalingLayout)) - size_offset +
                    static_cast<u32>(sizeof(RenderAreaLayout)),
        };
        return device->GetLogical().CreatePipelineLayout({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = descriptor_set_layout ? 1U : 0U,
            .pSetLayouts = bindings.empty() ? nullptr : &descriptor_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
    }

    void Add(const Shader::Info& info, VkShaderStageFlags stage) {
#ifdef __APPLE__
        if (stage == VK_SHADER_STAGE_GEOMETRY_BIT) {
            return;
        }
#endif
        is_compute |= (stage & VK_SHADER_STAGE_COMPUTE_BIT) != 0;

        Add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, stage, info.constant_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage, info.storage_buffers_descriptors);
        Add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, stage, info.texture_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, stage, info.image_buffer_descriptors);

        // Determine texture organization mode (must match EmitContext logic in spirv_emit_context.cpp)
        const auto setting = Settings::values.texture_pool_mode.GetValue();

        switch (setting) {
        case Settings::TexturePoolMode::Automatic:
            // Auto-select: prefer Pooled if supported, else platform default
            if (device->IsTexturePoolSupported()) {
                AddPooledTextures(stage, info);
            } else {
#ifdef __APPLE__
                AddSeparatedTexturesAndSamplers(stage, info);
#else
                Add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage, info.texture_descriptors);
#endif
            }
            break;
        case Settings::TexturePoolMode::Combined:
            Add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage, info.texture_descriptors);
            break;
        case Settings::TexturePoolMode::Separated:
            AddSeparatedTexturesAndSamplers(stage, info);
            break;
        case Settings::TexturePoolMode::Pooled:
            if (device->IsTexturePoolSupported()) {
                AddPooledTextures(stage, info);
            } else {
                // Fallback to Separated if Pooled not supported
                AddSeparatedTexturesAndSamplers(stage, info);
            }
            break;
        }

        Add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, stage, info.image_descriptors);
    }

    void DumpInfo(std::ostream& os)const {
        os << std::dec;
        os << std::endl;
        os << "binding count:";
        os << binding;
        os << " descriptor num:";
        os << num_descriptors;
        for(auto&& b : bindings){
            os << std::endl;
            os << " binding:" << b.binding;
            os << " type:" << b.descriptorType;
            os << " count:" << b.descriptorCount;
            os << std::hex;
            os << " flags:0x" << b.stageFlags;
            os << std::dec;
        }
        for(auto&& e : entries){
            os << std::endl;
            os << " dstBinding:" << e.dstBinding;
            os << " type:" << e.descriptorType;
            os << " count:" << e.descriptorCount;
            os << std::hex;
            os << " offset:0x" << e.offset;
            os << std::dec;
            os << " stride:" << e.stride;
        }
    }

private:
    struct SamplerSource {
        u32 cbuf_index;
        u32 cbuf_offset;
        u32 secondary_cbuf_index;
        u32 secondary_cbuf_offset;
        bool has_secondary;
    };

    // Separated textures and samplers mode (cross-platform)
    void AddSeparatedTexturesAndSamplers(VkShaderStageFlags stage, const Shader::Info& info) {
#ifdef __APPLE__
        if (stage == VK_SHADER_STAGE_GEOMETRY_BIT) {
            return;
        }
#endif
        const auto& descriptors = info.texture_descriptors;

        // Collect unique samplers based on TextureSamplerInfo to match SPIR-V generation
        // Use a map to keep track of the first occurrence of each unique sampler info
        std::map<Shader::TextureSamplerInfo, SamplerSource> unique_samplers;

        for (const auto& desc : descriptors) {
            Shader::TextureSamplerInfo sampler_info{};
            if (desc.sampler_index < info.sampler_descriptors.size() &&
                info.sampler_descriptors[desc.sampler_index].has_value()) {
                sampler_info = *info.sampler_descriptors[desc.sampler_index];
            }

            if (unique_samplers.find(sampler_info) == unique_samplers.end()) {
                unique_samplers[sampler_info] = {
                    desc.cbuf_index,
                    desc.cbuf_offset,
                    desc.secondary_cbuf_index,
                    desc.secondary_cbuf_offset,
                    desc.has_secondary,
                };
            }
        }

        // Add SAMPLED_IMAGE descriptors for textures
        const size_t num{descriptors.size()};
        for (size_t i = 0; i < num; ++i) {
            bindings.push_back({
                .binding = binding,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .descriptorCount = descriptors[i].count,
                .stageFlags = stage,
                .pImmutableSamplers = nullptr,
            });
            entries.push_back({
                .dstBinding = binding,
                .dstArrayElement = 0,
                .descriptorCount = descriptors[i].count,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .offset = offset,
                .stride = sizeof(DescriptorUpdateEntry),
            });
            ++binding;
            num_descriptors += descriptors[i].count;
            // Each texture in the descriptor needs one DescriptorUpdateEntry
            offset += sizeof(DescriptorUpdateEntry) * descriptors[i].count;
        }

        // Add separate SAMPLER descriptors (must iterate in same order as PushImageDescriptors)
        for (const auto& [key, source] : unique_samplers) {
            (void)key, (void)source;
            bindings.push_back({
                .binding = binding,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = stage,
                .pImmutableSamplers = nullptr,
            });
            entries.push_back({
                .dstBinding = binding,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .offset = offset,
                .stride = sizeof(DescriptorUpdateEntry),
            });
            ++binding;
            ++num_descriptors;
            offset += sizeof(DescriptorUpdateEntry);
        }
    }

    void AddPooledTextures(VkShaderStageFlags stage, const Shader::Info& info) {
#ifdef __APPLE__
        if (stage == VK_SHADER_STAGE_GEOMETRY_BIT) {
            return;
        }
#endif
        const auto& descriptors = info.texture_descriptors;
        if (descriptors.empty()) {
            return;
        }

        // Group textures by type, depth, and multisample
        using TextureGroupKey = std::tuple<Shader::TextureType, bool, bool>;
        std::map<TextureGroupKey, u32> texture_pool_sizes;
        for (const auto& desc : descriptors) {
            texture_pool_sizes[{desc.type, desc.is_depth, desc.is_multisample}] += desc.count;
        }

        // Add one SAMPLED_IMAGE array per texture group
        for (const auto& [key, pool_size] : texture_pool_sizes) {
            bindings.push_back({
                .binding = binding,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .descriptorCount = pool_size,
                .stageFlags = stage,
                .pImmutableSamplers = nullptr,
            });
            entries.push_back({
                .dstBinding = binding,
                .dstArrayElement = 0,
                .descriptorCount = pool_size,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .offset = offset,
                .stride = sizeof(DescriptorUpdateEntry),
            });
            ++binding;
            num_descriptors += pool_size;
            // Each texture in the pool needs one DescriptorUpdateEntry
            offset += sizeof(DescriptorUpdateEntry) * pool_size;
        }

        // Collect unique samplers based on TextureSamplerInfo
        std::set<Shader::TextureSamplerInfo> unique_samplers;
        for (const auto& desc : descriptors) {
            Shader::TextureSamplerInfo sampler_info{};
            if (desc.sampler_index < info.sampler_descriptors.size() &&
                info.sampler_descriptors[desc.sampler_index].has_value()) {
                sampler_info = *info.sampler_descriptors[desc.sampler_index];
            }
            unique_samplers.insert(sampler_info);
        }

        // Add SAMPLER descriptors
        for (size_t i = 0; i < unique_samplers.size(); ++i) {
            bindings.push_back({
                .binding = binding,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = stage,
                .pImmutableSamplers = nullptr,
            });
            entries.push_back({
                .dstBinding = binding,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .offset = offset,
                .stride = sizeof(DescriptorUpdateEntry),
            });
            ++binding;
            ++num_descriptors;
            offset += sizeof(DescriptorUpdateEntry);
        }
    }

    template <typename Descriptors>
    void Add(VkDescriptorType type, VkShaderStageFlags stage, const Descriptors& descriptors) {
#ifdef __APPLE__
        if (stage==VK_SHADER_STAGE_GEOMETRY_BIT) {
            return;
        }
#endif
        const size_t num{descriptors.size()};
        for (size_t i = 0; i < num; ++i) {
            bindings.push_back({
                .binding = binding,
                .descriptorType = type,
                .descriptorCount = descriptors[i].count,
                .stageFlags = stage,
                .pImmutableSamplers = nullptr,
            });
            entries.push_back({
                .dstBinding = binding,
                .dstArrayElement = 0,
                .descriptorCount = descriptors[i].count,
                .descriptorType = type,
                .offset = offset,
                .stride = sizeof(DescriptorUpdateEntry),
            });
            ++binding;
            num_descriptors += descriptors[i].count;
            offset += sizeof(DescriptorUpdateEntry);
        }
    }

    const Device* device{};
    bool is_compute{};
    boost::container::small_vector<VkDescriptorSetLayoutBinding, 32> bindings;
    boost::container::small_vector<VkDescriptorUpdateTemplateEntry, 32> entries;
    u32 binding{};
    u32 num_descriptors{};
    size_t offset{};
};

class RescalingPushConstant {
public:
    explicit RescalingPushConstant() noexcept {}

    void PushTexture(bool is_rescaled) noexcept {
        *texture_ptr |= is_rescaled ? texture_bit : 0u;
        texture_bit <<= 1u;
        if (texture_bit == 0u) {
            texture_bit = 1u;
            ++texture_ptr;
        }
    }

    void PushImage(bool is_rescaled) noexcept {
        *image_ptr |= is_rescaled ? image_bit : 0u;
        image_bit <<= 1u;
        if (image_bit == 0u) {
            image_bit = 1u;
            ++image_ptr;
        }
    }

    const std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS>& Data() const noexcept {
        return words;
    }

private:
    std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> words{};
    u32* texture_ptr{words.data()};
    u32* image_ptr{words.data() + Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS};
    u32 texture_bit{1u};
    u32 image_bit{1u};
};

class RenderAreaPushConstant {
public:
    bool uses_render_area{};
    std::array<f32, 4> words{};
};

inline void PushImageDescriptorsPooled(TextureCache& texture_cache,
                                       GuestDescriptorQueue& guest_descriptor_queue,
                                       const Device& device, int stage,
                                       const Shader::Info& info, RescalingPushConstant& rescaling,
                                       const VideoCommon::SamplerId*& samplers,
                                       const VideoCommon::ImageViewInOut*& views) {
    // Group textures by type, depth, and multisample for pool updates
    using TextureGroupKey = std::tuple<Shader::TextureType, bool, bool>;
    std::map<TextureGroupKey, std::vector<VkImageView>> texture_pools;

    // Collect unique samplers and their handles
    // Using std::map ensures keys are sorted, matching the Layout generation order
    std::map<Shader::TextureSamplerInfo, VkSampler> unique_samplers;

    for (const auto& desc : info.texture_descriptors) {
        auto& pool = texture_pools[{desc.type, desc.is_depth, desc.is_multisample}];

        Shader::TextureSamplerInfo sampler_info{};
        if (desc.sampler_index < info.sampler_descriptors.size() &&
            info.sampler_descriptors[desc.sampler_index].has_value()) {
            sampler_info = *info.sampler_descriptors[desc.sampler_index];
        }

        for (u32 index = 0; index < desc.count; ++index) {
            const VideoCommon::ImageViewId image_view_id{(views++)->id};
            const VideoCommon::SamplerId sampler_id{*(samplers++)};
            ImageView& image_view{texture_cache.GetImageView(image_view_id)};
            const VkImageView vk_image_view{image_view.Handle(desc.type)};

            pool.push_back(vk_image_view);
            rescaling.PushTexture(texture_cache.IsRescaling(image_view));

            // Collect sampler handle (only once per key, using the first occurrence)
            if (unique_samplers.find(sampler_info) == unique_samplers.end()) {
                const Sampler& sampler{texture_cache.GetSampler(sampler_id)};
                const bool use_fallback_sampler{sampler.HasAddedAnisotropy() &&
                                                !image_view.SupportsAnisotropy()};
                unique_samplers[sampler_info] = use_fallback_sampler ?
                    sampler.HandleWithDefaultAnisotropy() : sampler.Handle();
            }
        }
    }

    // Calculate binding offset
    u32 binding = 0;
    binding += static_cast<u32>(info.constant_buffer_descriptors.size());
    binding += static_cast<u32>(info.storage_buffers_descriptors.size());
    binding += static_cast<u32>(info.texture_buffer_descriptors.size());
    binding += static_cast<u32>(info.image_buffer_descriptors.size());

    // Add pooled textures (grouped by type, must match AddPooledTextures order)
    for (const auto& [key, pool] : texture_pools) {
        for (const auto& vk_image_view : pool) {
            guest_descriptor_queue.AddImage(stage, binding, vk_image_view);
        }
        binding++;
    }

    // Add unique samplers (must match AddPooledTextures order)
    for (const auto& [key, sampler] : unique_samplers) {
        guest_descriptor_queue.AddSampler(stage, binding++, sampler);
    }

    // Add storage images (must match Add order)
    for (const auto& desc : info.image_descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            ImageView& image_view{texture_cache.GetImageView((views++)->id)};
            if (desc.is_written) {
                texture_cache.MarkModification(image_view.image_id);
            }
            const VkImageView vk_image_view{image_view.StorageView(desc.type, desc.format)};
            guest_descriptor_queue.AddImage(stage, binding, vk_image_view);
            rescaling.PushImage(texture_cache.IsRescaling(image_view));
        }
        binding++;
    }
}

inline void PushImageDescriptors(TextureCache& texture_cache,
                                 GuestDescriptorQueue& guest_descriptor_queue,
                                 const Device& device, int stage,
                                 const Shader::Info& info, RescalingPushConstant& rescaling,
                                 const VideoCommon::SamplerId*& samplers,
                                 const VideoCommon::ImageViewInOut*& views) {
    const u32 num_texture_buffers = Shader::NumDescriptors(info.texture_buffer_descriptors);
    const u32 num_image_buffers = Shader::NumDescriptors(info.image_buffer_descriptors);
    views += num_texture_buffers;
    views += num_image_buffers;

    // Determine mode (must match descriptor layout mode in Add() function)
    const auto setting = Settings::values.texture_pool_mode.GetValue();

    // Check if we should use Pooled mode
    const bool use_pooled = (setting == Settings::TexturePoolMode::Pooled) ||
                           (setting == Settings::TexturePoolMode::Automatic &&
                            device.IsTexturePoolSupported());

    if (use_pooled) {
        // Use dedicated Pooled mode function
        PushImageDescriptorsPooled(texture_cache, guest_descriptor_queue, device, stage, info,
                                  rescaling, samplers, views);
        return;
    }

    // Calculate binding offset
    u32 binding = 0;
    binding += static_cast<u32>(info.constant_buffer_descriptors.size());
    binding += static_cast<u32>(info.storage_buffers_descriptors.size());
    binding += static_cast<u32>(info.texture_buffer_descriptors.size());
    binding += static_cast<u32>(info.image_buffer_descriptors.size());

    const bool use_separated = (setting == Settings::TexturePoolMode::Separated) ||
                              (setting == Settings::TexturePoolMode::Automatic);

    if (use_separated) {
        // Separated mode: Separate textures and samplers to reduce sampler count
        // CRITICAL: Must collect samplers in the same order as AddSeparatedTexturesAndSamplers

        // Build sampler handle map and add textures in one pass
        // Using std::map ensures keys are sorted, matching the Layout generation order
        std::map<Shader::TextureSamplerInfo, VkSampler> unique_samplers;

        for (const auto& desc : info.texture_descriptors) {
            Shader::TextureSamplerInfo sampler_info{};
            if (desc.sampler_index < info.sampler_descriptors.size() &&
                info.sampler_descriptors[desc.sampler_index].has_value()) {
                sampler_info = *info.sampler_descriptors[desc.sampler_index];
            }

            for (u32 index = 0; index < desc.count; ++index) {
                const VideoCommon::ImageViewId image_view_id{(views++)->id};
                const VideoCommon::SamplerId sampler_id{*(samplers++)};

                ImageView& image_view{texture_cache.GetImageView(image_view_id)};
                const VkImageView vk_image_view{image_view.Handle(desc.type)};

                // Add texture (SAMPLED_IMAGE)
                guest_descriptor_queue.AddImage(stage, binding, vk_image_view);
                rescaling.PushTexture(texture_cache.IsRescaling(image_view));

                // Collect sampler handle (only once per key, using the first occurrence)
                if (unique_samplers.find(sampler_info) == unique_samplers.end()) {
                    const Sampler& sampler{texture_cache.GetSampler(sampler_id)};
                    const bool use_fallback_sampler{sampler.HasAddedAnisotropy() &&
                                                    !image_view.SupportsAnisotropy()};
                    unique_samplers[sampler_info] = use_fallback_sampler ?
                        sampler.HandleWithDefaultAnisotropy() : sampler.Handle();
                }
            }
            binding++;
        }

        // Add unique samplers (MUST iterate in same order as AddSeparatedTexturesAndSamplers)
        for (const auto& [key, sampler] : unique_samplers) {
            guest_descriptor_queue.AddSampler(stage, binding++, sampler);
        }
    } else {
        // Combined mode: Use combined image samplers
        for (const auto& desc : info.texture_descriptors) {
            for (u32 index = 0; index < desc.count; ++index) {
                const VideoCommon::ImageViewId image_view_id{(views++)->id};
                const VideoCommon::SamplerId sampler_id{*(samplers++)};
                ImageView& image_view{texture_cache.GetImageView(image_view_id)};
                const VkImageView vk_image_view{image_view.Handle(desc.type)};
                const Sampler& sampler{texture_cache.GetSampler(sampler_id)};
                const bool use_fallback_sampler{sampler.HasAddedAnisotropy() &&
                                                !image_view.SupportsAnisotropy()};
                const VkSampler vk_sampler{use_fallback_sampler ? sampler.HandleWithDefaultAnisotropy()
                                                                : sampler.Handle()};
                guest_descriptor_queue.AddSampledImage(stage, binding, vk_image_view, vk_sampler);
                rescaling.PushTexture(texture_cache.IsRescaling(image_view));
            }
            binding++;
        }
    }

    // Image descriptors remain the same for both modes
    for (const auto& desc : info.image_descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            ImageView& image_view{texture_cache.GetImageView((views++)->id)};
            if (desc.is_written) {
                texture_cache.MarkModification(image_view.image_id);
            }
            const VkImageView vk_image_view{image_view.StorageView(desc.type, desc.format)};
            guest_descriptor_queue.AddImage(stage, binding, vk_image_view);
            rescaling.PushImage(texture_cache.IsRescaling(image_view));
        }
        binding++;
    }
}

} // namespace Vulkan
