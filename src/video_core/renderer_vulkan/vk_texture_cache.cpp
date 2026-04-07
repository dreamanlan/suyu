// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <fstream>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/bit_cast.h"
#include "common/bit_util.h"
#include "common/settings.h"
#include "common/stb.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging/log.h"

#include "video_core/renderer_vulkan/vk_texture_cache.h"

#include "video_core/engines/fermi_2d.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/formatter.h"
#include "video_core/texture_cache/samples_helper.h"
#include "video_core/texture_cache/util.h"
#include "video_core/texture_cache/dds_writer.h"
#include "video_core/textures/decoders.h"
#include "video_core/compatible_formats.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "core/memory/debug_script/DbgScpHook.h"

namespace Vulkan {

using Tegra::Engines::Fermi2D;
using Tegra::Texture::SwizzleSource;
using Tegra::Texture::TextureMipmapFilter;
using VideoCommon::BufferImageCopy;
using VideoCommon::ImageFlagBits;
using VideoCommon::ImageInfo;
using VideoCommon::ImageType;
using VideoCommon::SubresourceRange;
using VideoCore::Surface::BytesPerBlock;
using VideoCore::Surface::IsPixelFormatASTC;
using VideoCore::Surface::IsPixelFormatBCn;
using VideoCore::Surface::IsPixelFormatInteger;
using VideoCore::Surface::SurfaceType;

namespace {
constexpr VkBorderColor ConvertBorderColor(const std::array<float, 4>& color) {
    if (color == std::array<float, 4>{0, 0, 0, 0}) {
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    } else if (color == std::array<float, 4>{0, 0, 0, 1}) {
        return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    } else if (color == std::array<float, 4>{1, 1, 1, 1}) {
        return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    if (color[0] + color[1] + color[2] > 1.35f) {
        // If color elements are brighter than roughly 0.5 average, use white border
        return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    } else if (color[3] > 0.5f) {
        return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    } else {
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
}

[[nodiscard]] VkImageType ConvertImageType(const ImageType type) {
    switch (type) {
    case ImageType::e1D:
        return VK_IMAGE_TYPE_1D;
    case ImageType::e2D:
    case ImageType::Linear:
        return VK_IMAGE_TYPE_2D;
    case ImageType::e3D:
        return VK_IMAGE_TYPE_3D;
    case ImageType::Buffer:
        break;
    }
    ASSERT_MSG(false, "Invalid image type={}", type);
    return {};
}

[[nodiscard]] VkSampleCountFlagBits ConvertSampleCount(u32 num_samples) {
    switch (num_samples) {
    case 1:
        return VK_SAMPLE_COUNT_1_BIT;
    case 2:
        return VK_SAMPLE_COUNT_2_BIT;
    case 4:
        return VK_SAMPLE_COUNT_4_BIT;
    case 8:
        return VK_SAMPLE_COUNT_8_BIT;
    case 16:
        return VK_SAMPLE_COUNT_16_BIT;
    default:
        ASSERT_MSG(false, "Invalid number of samples={}", num_samples);
        return VK_SAMPLE_COUNT_1_BIT;
    }
}

[[nodiscard]] bool IsCompressedFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11_SNORM_BLOCK:
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
    case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
    case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
    case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
    case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
    case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
    case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
    case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
    case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
    case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
    case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
    case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
    case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
    case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
    case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
    case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
    case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
    case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
    case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
    case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
    case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
    case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
    case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
    case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
    case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
    case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
    case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] VkImageUsageFlags ImageUsageFlags(const MaxwellToVK::FormatInfo& info,
                                                PixelFormat format) {
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (info.attachable) {
        switch (VideoCore::Surface::GetFormatType(format)) {
        case VideoCore::Surface::SurfaceType::ColorTexture:
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            break;
        case VideoCore::Surface::SurfaceType::Depth:
        case VideoCore::Surface::SurfaceType::Stencil:
        case VideoCore::Surface::SurfaceType::DepthStencil:
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            break;
        default:
            ASSERT_MSG(false, "Invalid surface type");
            break;
        }
    }
    if (info.storage) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    return usage;
}

[[nodiscard]] VkImageCreateInfo MakeImageCreateInfo(const Device& device, const ImageInfo& info) {
    const bool is_2d = (info.type == ImageType::e2D);
    const bool is_3d = (info.type == ImageType::e3D);
    const auto format_info =
        MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, false, info.format);
    VkImageCreateFlags flags{};
    if (is_2d && info.resources.layers >= 6 && info.size.width == info.size.height &&
        !device.HasBrokenCubeImageCompatibility()) {
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

    // fix moltenVK issues with some 3D games
    // credit to Jarrod Norwell from Sudachi https://github.com/jarrodnorwell/Sudachi
    auto usage = ImageUsageFlags(format_info, info.format);
    if (is_3d) {
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        // Force usage to be VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT only on MoltenVK
        if (device.IsMoltenVK()) {
            //usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
    }
    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(info.num_samples);
    return VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = format_info.format,
        .extent{
            .width = info.size.width >> samples_x,
            .height = info.size.height >> samples_y,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = ConvertSampleCount(info.num_samples),
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
}

[[nodiscard]] vk::Image MakeImage(const Device& device, const MemoryAllocator& allocator,
                                  const ImageInfo& info, std::span<const VkFormat> view_formats) {
    if (info.type == ImageType::Buffer) {
        return vk::Image{};
    }
    VkImageCreateInfo image_ci = MakeImageCreateInfo(device, info);
    const bool is_main_compressed = IsCompressedFormat(image_ci.format);
    // Filter view_formats for Vulkan compatibility:
    // - Compressed image: keep all formats, set BLOCK_TEXEL_VIEW_COMPATIBLE_BIT if mixed
    // - Uncompressed image: remove compressed formats (not class-compatible in Vulkan)
    boost::container::small_vector<VkFormat, 16> filtered_formats;
    if (!is_main_compressed) {
        for (const auto fmt : view_formats) {
            if (!IsCompressedFormat(fmt)) {
                filtered_formats.push_back(fmt);
            }
        }
    }
    const auto effective_formats = is_main_compressed
                                       ? view_formats
                                       : std::span<const VkFormat>(filtered_formats);
    const VkImageFormatListCreateInfo image_format_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .pNext = nullptr,
        .viewFormatCount = static_cast<u32>(effective_formats.size()),
        .pViewFormats = effective_formats.data(),
    };
    if (effective_formats.size() > 1) {
        image_ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (device.IsKhrImageFormatListSupported()) {
            image_ci.pNext = &image_format_list;
        }
        if (is_main_compressed) {
            const bool any_uncompressed = std::ranges::any_of(
                effective_formats, [](VkFormat format) { return !IsCompressedFormat(format); });
            if (any_uncompressed) {
                image_ci.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
            }
        }
    }
    return allocator.CreateImage(image_ci);
}

[[nodiscard]] VkFormat ConvertStorageFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
        return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_A8B8G8R8_UINT_PACK32:
        return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_A8B8G8R8_SINT_PACK32:
        return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8_SRGB:
        return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8_SRGB:
        return VK_FORMAT_R8_UNORM;
    default:
        return format;
    }
}

[[nodiscard]] vk::ImageView MakeStorageView(const vk::Device& device, u32 level, VkImage image,
                                            VkFormat format) {
    static constexpr VkImageViewUsageCreateInfo storage_image_view_usage_create_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .pNext = nullptr,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT,
    };
    return device.CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &storage_image_view_usage_create_info,
        .flags = 0,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = format,
        .components{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = level,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    });
}

[[nodiscard]] VkImageAspectFlags ImageAspectMask(PixelFormat format) {
    switch (VideoCore::Surface::GetFormatType(format)) {
    case VideoCore::Surface::SurfaceType::ColorTexture:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    case VideoCore::Surface::SurfaceType::Depth:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VideoCore::Surface::SurfaceType::Stencil:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VideoCore::Surface::SurfaceType::DepthStencil:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        ASSERT_MSG(false, "Invalid surface type");
        return VkImageAspectFlags{};
    }
}

[[nodiscard]] VkImageAspectFlags ImageViewAspectMask(const VideoCommon::ImageViewInfo& info) {
    if (info.IsRenderTarget()) {
        return ImageAspectMask(info.format);
    }
    bool any_r =
        std::ranges::any_of(info.Swizzle(), [](SwizzleSource s) { return s == SwizzleSource::R; });
    switch (info.format) {
    case PixelFormat::D24_UNORM_S8_UINT:
    case PixelFormat::D32_FLOAT_S8_UINT:
        // R = depth, G = stencil
        return any_r ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    case PixelFormat::S8_UINT_D24_UNORM:
        // R = stencil, G = depth
        return any_r ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    case PixelFormat::D16_UNORM:
    case PixelFormat::D32_FLOAT:
    case PixelFormat::X8_D24_UNORM:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case PixelFormat::S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

[[nodiscard]] VkComponentSwizzle ComponentSwizzle(SwizzleSource swizzle) {
    switch (swizzle) {
    case SwizzleSource::Zero:
        return VK_COMPONENT_SWIZZLE_ZERO;
    case SwizzleSource::R:
        return VK_COMPONENT_SWIZZLE_R;
    case SwizzleSource::G:
        return VK_COMPONENT_SWIZZLE_G;
    case SwizzleSource::B:
        return VK_COMPONENT_SWIZZLE_B;
    case SwizzleSource::A:
        return VK_COMPONENT_SWIZZLE_A;
    case SwizzleSource::OneFloat:
    case SwizzleSource::OneInt:
        return VK_COMPONENT_SWIZZLE_ONE;
    }
    ASSERT_MSG(false, "Invalid swizzle={}", swizzle);
    return VK_COMPONENT_SWIZZLE_ZERO;
}

[[nodiscard]] VkImageViewType ImageViewType(Shader::TextureType type) {
    switch (type) {
    case Shader::TextureType::Color1D:
        return VK_IMAGE_VIEW_TYPE_1D;
    case Shader::TextureType::Color2D:
    case Shader::TextureType::Color2DRect:
        return VK_IMAGE_VIEW_TYPE_2D;
    case Shader::TextureType::ColorCube:
        return VK_IMAGE_VIEW_TYPE_CUBE;
    case Shader::TextureType::Color3D:
        return VK_IMAGE_VIEW_TYPE_3D;
    case Shader::TextureType::ColorArray1D:
        return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case Shader::TextureType::ColorArray2D:
        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case Shader::TextureType::ColorArrayCube:
        return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case Shader::TextureType::Buffer:
        ASSERT_MSG(false, "Texture buffers can't be image views");
        return VK_IMAGE_VIEW_TYPE_1D;
    }
    ASSERT_MSG(false, "Invalid image view type={}", type);
    return VK_IMAGE_VIEW_TYPE_2D;
}

[[nodiscard]] VkImageViewType ImageViewType(VideoCommon::ImageViewType type) {
    switch (type) {
    case VideoCommon::ImageViewType::e1D:
        return VK_IMAGE_VIEW_TYPE_1D;
    case VideoCommon::ImageViewType::e2D:
    case VideoCommon::ImageViewType::Rect:
        return VK_IMAGE_VIEW_TYPE_2D;
    case VideoCommon::ImageViewType::Cube:
        return VK_IMAGE_VIEW_TYPE_CUBE;
    case VideoCommon::ImageViewType::e3D:
        return VK_IMAGE_VIEW_TYPE_3D;
    case VideoCommon::ImageViewType::e1DArray:
        return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case VideoCommon::ImageViewType::e2DArray:
        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case VideoCommon::ImageViewType::CubeArray:
        return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case VideoCommon::ImageViewType::Buffer:
        ASSERT_MSG(false, "Texture buffers can't be image views");
        return VK_IMAGE_VIEW_TYPE_1D;
    }
    ASSERT_MSG(false, "Invalid image view type={}", type);
    return VK_IMAGE_VIEW_TYPE_2D;
}

[[nodiscard]] VkImageSubresourceLayers MakeImageSubresourceLayers(
    VideoCommon::SubresourceLayers subresource, VkImageAspectFlags aspect_mask) {
    return VkImageSubresourceLayers{
        .aspectMask = aspect_mask,
        .mipLevel = static_cast<u32>(subresource.base_level),
        .baseArrayLayer = static_cast<u32>(subresource.base_layer),
        .layerCount = static_cast<u32>(subresource.num_layers),
    };
}

[[nodiscard]] VkOffset3D MakeOffset3D(VideoCommon::Offset3D offset3d) {
    return VkOffset3D{
        .x = offset3d.x,
        .y = offset3d.y,
        .z = offset3d.z,
    };
}

[[nodiscard]] VkExtent3D MakeExtent3D(VideoCommon::Extent3D extent3d) {
    return VkExtent3D{
        .width = static_cast<u32>(extent3d.width),
        .height = static_cast<u32>(extent3d.height),
        .depth = static_cast<u32>(extent3d.depth),
    };
}

[[nodiscard]] VkImageCopy MakeImageCopy(const VideoCommon::ImageCopy& copy,
                                        VkImageAspectFlags aspect_mask) noexcept {
    return VkImageCopy{
        .srcSubresource = MakeImageSubresourceLayers(copy.src_subresource, aspect_mask),
        .srcOffset = MakeOffset3D(copy.src_offset),
        .dstSubresource = MakeImageSubresourceLayers(copy.dst_subresource, aspect_mask),
        .dstOffset = MakeOffset3D(copy.dst_offset),
        .extent = MakeExtent3D(copy.extent),
    };
}

[[nodiscard]] VkBufferImageCopy MakeBufferImageCopy(const VideoCommon::ImageCopy& copy, bool is_src,
                                                    VkImageAspectFlags aspect_mask) noexcept {
    return VkBufferImageCopy{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = MakeImageSubresourceLayers(
            is_src ? copy.src_subresource : copy.dst_subresource, aspect_mask),
        .imageOffset = MakeOffset3D(is_src ? copy.src_offset : copy.dst_offset),
        .imageExtent = MakeExtent3D(copy.extent),
    };
}

[[maybe_unused]] [[nodiscard]] boost::container::small_vector<VkBufferCopy, 16>
TransformBufferCopies(std::span<const VideoCommon::BufferCopy> copies, size_t buffer_offset) {
    boost::container::small_vector<VkBufferCopy, 16> result(copies.size());
    std::ranges::transform(
        copies, result.begin(), [buffer_offset](const VideoCommon::BufferCopy& copy) {
            return VkBufferCopy{
                .srcOffset = static_cast<VkDeviceSize>(copy.src_offset + buffer_offset),
                .dstOffset = static_cast<VkDeviceSize>(copy.dst_offset),
                .size = static_cast<VkDeviceSize>(copy.size),
            };
        });
    return result;
}

[[nodiscard]] boost::container::small_vector<VkBufferImageCopy, 16> TransformBufferImageCopies(
    std::span<const BufferImageCopy> copies, size_t buffer_offset, VkImageAspectFlags aspect_mask) {
    struct Maker {
        VkBufferImageCopy operator()(const BufferImageCopy& copy) const {
            return VkBufferImageCopy{
                .bufferOffset = copy.buffer_offset + buffer_offset,
                .bufferRowLength = copy.buffer_row_length,
                .bufferImageHeight = copy.buffer_image_height,
                .imageSubresource =
                    {
                        .aspectMask = aspect_mask,
                        .mipLevel = static_cast<u32>(copy.image_subresource.base_level),
                        .baseArrayLayer = static_cast<u32>(copy.image_subresource.base_layer),
                        .layerCount = static_cast<u32>(copy.image_subresource.num_layers),
                    },
                .imageOffset =
                    {
                        .x = copy.image_offset.x,
                        .y = copy.image_offset.y,
                        .z = copy.image_offset.z,
                    },
                .imageExtent =
                    {
                        .width = copy.image_extent.width,
                        .height = copy.image_extent.height,
                        .depth = copy.image_extent.depth,
                    },
            };
        }
        size_t buffer_offset;
        VkImageAspectFlags aspect_mask;
    };
    if (aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        boost::container::small_vector<VkBufferImageCopy, 16> result(copies.size() * 2);
        std::ranges::transform(copies, result.begin(),
                               Maker{buffer_offset, VK_IMAGE_ASPECT_DEPTH_BIT});
        std::ranges::transform(copies, result.begin() + copies.size(),
                               Maker{buffer_offset, VK_IMAGE_ASPECT_STENCIL_BIT});
        return result;
    } else {
        boost::container::small_vector<VkBufferImageCopy, 16> result(copies.size());
        std::ranges::transform(copies, result.begin(), Maker{buffer_offset, aspect_mask});
        return result;
    }
}

[[nodiscard]] VkImageSubresourceRange MakeSubresourceRange(VkImageAspectFlags aspect_mask,
                                                           const SubresourceRange& range) {
    return VkImageSubresourceRange{
        .aspectMask = aspect_mask,
        .baseMipLevel = static_cast<u32>(range.base.level),
        .levelCount = static_cast<u32>(range.extent.levels),
        .baseArrayLayer = static_cast<u32>(range.base.layer),
        .layerCount = static_cast<u32>(range.extent.layers),
    };
}

[[nodiscard]] VkImageSubresourceRange MakeSubresourceRange(const ImageView* image_view) {
    SubresourceRange range = image_view->range;
    if (True(image_view->flags & VideoCommon::ImageViewFlagBits::Slice)) {
        // Slice image views always affect a single layer, but their subresource range corresponds
        // to the slice. Override the value to affect a single layer.
        range.base.layer = 0;
        range.extent.layers = 1;
    }
    return MakeSubresourceRange(ImageAspectMask(image_view->format), range);
}

[[nodiscard]] VkImageSubresourceLayers MakeSubresourceLayers(const ImageView* image_view) {
    return VkImageSubresourceLayers{
        .aspectMask = ImageAspectMask(image_view->format),
        .mipLevel = static_cast<u32>(image_view->range.base.level),
        .baseArrayLayer = static_cast<u32>(image_view->range.base.layer),
        .layerCount = static_cast<u32>(image_view->range.extent.layers),
    };
}

[[nodiscard]] SwizzleSource ConvertGreenRed(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::G:
        return SwizzleSource::R;
    default:
        return value;
    }
}

[[nodiscard]] SwizzleSource SwapBlueRed(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::R:
        return SwizzleSource::B;
    case SwizzleSource::B:
        return SwizzleSource::R;
    default:
        return value;
    }
}

[[nodiscard]] SwizzleSource SwapGreenRed(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::R:
        return SwizzleSource::G;
    case SwizzleSource::G:
        return SwizzleSource::R;
    default:
        return value;
    }
}

[[nodiscard]] SwizzleSource SwapSpecial(SwizzleSource value) {
    switch (value) {
    case SwizzleSource::A:
        return SwizzleSource::R;
    case SwizzleSource::R:
        return SwizzleSource::A;
    case SwizzleSource::G:
        return SwizzleSource::B;
    case SwizzleSource::B:
        return SwizzleSource::G;
    default:
        return value;
    }
}

void CopyBufferToImage(vk::CommandBuffer cmdbuf, VkBuffer src_buffer, VkImage image,
                       VkImageAspectFlags aspect_mask, bool is_initialized,
                       std::span<const VkBufferImageCopy> copies) {
    static constexpr VkAccessFlags WRITE_ACCESS_FLAGS =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    static constexpr VkAccessFlags READ_ACCESS_FLAGS = VK_ACCESS_SHADER_READ_BIT |
                                                       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    const VkImageMemoryBarrier read_barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = WRITE_ACCESS_FLAGS,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = is_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{
            .aspectMask = aspect_mask,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };
    const VkImageMemoryBarrier write_barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = WRITE_ACCESS_FLAGS | READ_ACCESS_FLAGS,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{
            .aspectMask = aspect_mask,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           read_barrier);
    cmdbuf.CopyBufferToImage(src_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies);
    // TODO: Move this to another API
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                           write_barrier);
}

[[nodiscard]] VkImageBlit MakeImageBlit(const Region2D& dst_region, const Region2D& src_region,
                                        const VkImageSubresourceLayers& dst_layers,
                                        const VkImageSubresourceLayers& src_layers) {
    return VkImageBlit{
        .srcSubresource = src_layers,
        .srcOffsets =
            {
                {
                    .x = src_region.start.x,
                    .y = src_region.start.y,
                    .z = 0,
                },
                {
                    .x = src_region.end.x,
                    .y = src_region.end.y,
                    .z = 1,
                },
            },
        .dstSubresource = dst_layers,
        .dstOffsets =
            {
                {
                    .x = dst_region.start.x,
                    .y = dst_region.start.y,
                    .z = 0,
                },
                {
                    .x = dst_region.end.x,
                    .y = dst_region.end.y,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] VkImageResolve MakeImageResolve(const Region2D& dst_region,
                                              const Region2D& src_region,
                                              const VkImageSubresourceLayers& dst_layers,
                                              const VkImageSubresourceLayers& src_layers) {
    return VkImageResolve{
        .srcSubresource = src_layers,
        .srcOffset =
            {
                .x = src_region.start.x,
                .y = src_region.start.y,
                .z = 0,
            },
        .dstSubresource = dst_layers,
        .dstOffset =
            {
                .x = dst_region.start.x,
                .y = dst_region.start.y,
                .z = 0,
            },
        .extent =
            {
                .width = static_cast<u32>(dst_region.end.x - dst_region.start.x),
                .height = static_cast<u32>(dst_region.end.y - dst_region.start.y),
                .depth = 1,
            },
    };
}

void TryTransformSwizzleIfNeeded(PixelFormat format, std::array<SwizzleSource, 4>& swizzle,
                                 bool emulate_bgr565, bool emulate_a4b4g4r4) {
    switch (format) {
    case PixelFormat::A1B5G5R5_UNORM:
        std::ranges::transform(swizzle, swizzle.begin(), SwapBlueRed);
        break;
    case PixelFormat::B5G6R5_UNORM:
        if (emulate_bgr565) {
            std::ranges::transform(swizzle, swizzle.begin(), SwapBlueRed);
        }
        break;
    case PixelFormat::A5B5G5R1_UNORM:
        std::ranges::transform(swizzle, swizzle.begin(), SwapSpecial);
        break;
    case PixelFormat::G4R4_UNORM:
        std::ranges::transform(swizzle, swizzle.begin(), SwapGreenRed);
        break;
    case PixelFormat::A4B4G4R4_UNORM:
        if (emulate_a4b4g4r4) {
            std::ranges::reverse(swizzle);
        }
        break;
    default:
        break;
    }
}

struct RangedBarrierRange {
    u32 min_mip = std::numeric_limits<u32>::max();
    u32 max_mip = std::numeric_limits<u32>::min();
    u32 min_layer = std::numeric_limits<u32>::max();
    u32 max_layer = std::numeric_limits<u32>::min();

    void AddLayers(const VkImageSubresourceLayers& layers) {
        min_mip = std::min(min_mip, layers.mipLevel);
        max_mip = std::max(max_mip, layers.mipLevel + 1);
        min_layer = std::min(min_layer, layers.baseArrayLayer);
        max_layer = std::max(max_layer, layers.baseArrayLayer + layers.layerCount);
    }

    VkImageSubresourceRange SubresourceRange(VkImageAspectFlags aspect_mask) const noexcept {
        return VkImageSubresourceRange{
            .aspectMask = aspect_mask,
            .baseMipLevel = min_mip,
            .levelCount = max_mip - min_mip,
            .baseArrayLayer = min_layer,
            .layerCount = max_layer - min_layer,
        };
    }
};

[[nodiscard]] VkFormat Format(Shader::ImageFormat format) {
    switch (format) {
    case Shader::ImageFormat::Typeless:
        break;
    case Shader::ImageFormat::R8_SINT:
        return VK_FORMAT_R8_SINT;
    case Shader::ImageFormat::R8_UINT:
        return VK_FORMAT_R8_UINT;
    case Shader::ImageFormat::R16_UINT:
        return VK_FORMAT_R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return VK_FORMAT_R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return VK_FORMAT_R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return VK_FORMAT_R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return VK_FORMAT_R32G32B32A32_UINT;
    }
    ASSERT_MSG(false, "Invalid image format={}", format);
    return VK_FORMAT_R32_UINT;
}

void BlitScale(Scheduler& scheduler, VkImage src_image, VkImage dst_image, const ImageInfo& info,
               VkImageAspectFlags aspect_mask, const Settings::ResolutionScalingInfo& resolution,
               bool up_scaling = true) {
    const bool is_2d = (info.type == ImageType::e2D);
    const auto resources = info.resources;
    const VkExtent2D extent{
        .width = info.size.width,
        .height = info.size.height,
    };
    // Depth and integer formats must use NEAREST filter for blits.
    const bool is_color{aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT};
    const bool is_bilinear{is_color && !IsPixelFormatInteger(info.format)};
    const VkFilter vk_filter = is_bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst_image, src_image, extent, resources, aspect_mask, resolution, is_2d,
                      vk_filter, up_scaling](vk::CommandBuffer cmdbuf) {
        const VkOffset2D src_size{
            .x = static_cast<s32>(up_scaling ? extent.width : resolution.ScaleUp(extent.width)),
            .y = static_cast<s32>(is_2d && up_scaling ? extent.height
                                                      : resolution.ScaleUp(extent.height)),
        };
        const VkOffset2D dst_size{
            .x = static_cast<s32>(up_scaling ? resolution.ScaleUp(extent.width) : extent.width),
            .y = static_cast<s32>(is_2d && up_scaling ? resolution.ScaleUp(extent.height)
                                                      : extent.height),
        };
        boost::container::small_vector<VkImageBlit, 4> regions;
        regions.reserve(resources.levels);
        for (s32 level = 0; level < resources.levels; level++) {
            regions.push_back({
                .srcSubresource{
                    .aspectMask = aspect_mask,
                    .mipLevel = static_cast<u32>(level),
                    .baseArrayLayer = 0,
                    .layerCount = static_cast<u32>(resources.layers),
                },
                .srcOffsets{
                    {
                        .x = 0,
                        .y = 0,
                        .z = 0,
                    },
                    {
                        .x = std::max(1, src_size.x >> level),
                        .y = std::max(1, src_size.y >> level),
                        .z = 1,
                    },
                },
                .dstSubresource{
                    .aspectMask = aspect_mask,
                    .mipLevel = static_cast<u32>(level),
                    .baseArrayLayer = 0,
                    .layerCount = static_cast<u32>(resources.layers),
                },
                .dstOffsets{
                    {
                        .x = 0,
                        .y = 0,
                        .z = 0,
                    },
                    {
                        .x = std::max(1, dst_size.x >> level),
                        .y = std::max(1, dst_size.y >> level),
                        .z = 1,
                    },
                },
            });
        }
        const VkImageSubresourceRange subresource_range{
            .aspectMask = aspect_mask,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array read_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = subresource_range,
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, // Discard contents
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = subresource_range,
            },
        };
        const std::array write_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = subresource_range,
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = subresource_range,
            },
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, nullptr, nullptr, read_barriers);
        cmdbuf.BlitImage(src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions, vk_filter);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, nullptr, nullptr, write_barriers);
    });
}
} // Anonymous namespace

TextureCacheRuntime::TextureCacheRuntime(const Device& device_, Scheduler& scheduler_,
                                         MemoryAllocator& memory_allocator_,
                                         StagingBufferPool& staging_buffer_pool_,
                                         BlitImageHelper& blit_image_helper_,
                                         RenderPassCache& render_pass_cache_,
                                         DescriptorPool& descriptor_pool,
                                         ComputePassDescriptorQueue& compute_pass_descriptor_queue)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      staging_buffer_pool{staging_buffer_pool_}, blit_image_helper{blit_image_helper_},
      render_pass_cache{render_pass_cache_}, resolution{Settings::values.resolution_info} {
    if (Settings::values.accelerate_astc.GetValue() == Settings::AstcDecodeMode::Gpu) {
        astc_decoder_pass.emplace(device, scheduler, descriptor_pool, staging_buffer_pool,
                                  compute_pass_descriptor_queue, memory_allocator);
    }
    // Initialize BCn GPU decoder if enabled (Gpu mode)
    const auto bcn_mode = Settings::values.bcn_decode_mode.GetValue();
    if (bcn_mode == Settings::BcnDecodeMode::Gpu) {
        if (!device.IsOptimalBcnSupported()) {
            bcn_decoder_pass.emplace(device, scheduler, descriptor_pool, staging_buffer_pool,
                                     compute_pass_descriptor_queue, memory_allocator);

            // Initialize specialized BC6H and BC7 decoders
            bc6h_decoder_pass.emplace(device, scheduler, descriptor_pool, staging_buffer_pool,
                                      compute_pass_descriptor_queue, memory_allocator);
            bc7_decoder_pass.emplace(device, scheduler, descriptor_pool, staging_buffer_pool,
                                     compute_pass_descriptor_queue, memory_allocator);
        }
    }
    if (device.IsStorageImageMultisampleSupported()) {
        msaa_copy_pass = std::make_unique<MSAACopyPass>(
            device, scheduler, descriptor_pool, staging_buffer_pool, compute_pass_descriptor_queue);
    }
    if (!device.IsKhrImageFormatListSupported()) {
        return;
    }
    for (size_t index_a = 0; index_a < VideoCore::Surface::MaxPixelFormat; index_a++) {
        const auto image_format = static_cast<PixelFormat>(index_a);
        if (IsPixelFormatASTC(image_format) && !device.IsOptimalAstcSupported()) {
            view_formats[index_a].push_back(VK_FORMAT_A8B8G8R8_UNORM_PACK32);
        }
        // Add view format for BCn GPU decoding
        if (IsPixelFormatBCn(image_format) && !device.IsOptimalBcnSupported()) {
            if (bcn_mode == Settings::BcnDecodeMode::Gpu) {
                if (image_format == PixelFormat::BC6H_UFLOAT ||
                    image_format == PixelFormat::BC6H_SFLOAT) {
                    view_formats[index_a].push_back(VK_FORMAT_R16G16B16A16_SFLOAT);
                } else {
                    view_formats[index_a].push_back(VK_FORMAT_A8B8G8R8_UNORM_PACK32);
                }
            }
        }
        for (size_t index_b = 0; index_b < VideoCore::Surface::MaxPixelFormat; index_b++) {
            const auto view_format = static_cast<PixelFormat>(index_b);
            if (VideoCore::Surface::IsViewCompatible(image_format, view_format, false, true)) {
                const auto view_info =
                    MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, true, view_format);
                view_formats[index_a].push_back(view_info.format);
            }
        }
    }
}

bool TextureCacheRuntime::HasBrokenTextureViewFormats() const noexcept {
#if __APPLE__
    if (Settings::values.enable_broken_views)
        return true;
#endif
    return false;
}

void TextureCacheRuntime::Finish() {
    scheduler.Finish();
}

StagingBufferRef TextureCacheRuntime::UploadStagingBuffer(size_t size) {
    return staging_buffer_pool.Request(size, MemoryUsage::Upload);
}

StagingBufferRef TextureCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return staging_buffer_pool.Request(size, MemoryUsage::Download, deferred);
}

void TextureCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) {
    staging_buffer_pool.FreeDeferred(ref);
}

bool TextureCacheRuntime::ShouldReinterpret(Image& dst, Image& src) {
    if (VideoCore::Surface::GetFormatType(dst.info.format) ==
            VideoCore::Surface::SurfaceType::DepthStencil &&
        !device.IsExtShaderStencilExportSupported()) {
        return true;
    }
    if (dst.info.format == PixelFormat::D32_FLOAT_S8_UINT ||
        src.info.format == PixelFormat::D32_FLOAT_S8_UINT) {
        return true;
    }
    return false;
}

VkBuffer TextureCacheRuntime::GetTemporaryBuffer(size_t needed_size) {
    const auto level = (8 * sizeof(size_t)) - std::countl_zero(needed_size - 1ULL);
    if (buffers[level]) {
        return *buffers[level];
    }
    const auto new_size = Common::NextPow2(needed_size);
    static constexpr VkBufferUsageFlags flags =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    const VkBufferCreateInfo temp_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = new_size,
        .usage = flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    buffers[level] = memory_allocator.CreateBuffer(temp_ci, MemoryUsage::DeviceLocal);
    return *buffers[level];
}

void TextureCacheRuntime::BarrierFeedbackLoop() {
    scheduler.RequestOutsideRenderPassOperationContext();
}

void TextureCacheRuntime::ReinterpretImage(Image& dst, Image& src,
                                           std::span<const VideoCommon::ImageCopy> copies) {
    boost::container::small_vector<VkBufferImageCopy, 16> vk_in_copies(copies.size());
    boost::container::small_vector<VkBufferImageCopy, 16> vk_out_copies(copies.size());
    const VkImageAspectFlags src_aspect_mask = src.AspectMask();
    const VkImageAspectFlags dst_aspect_mask = dst.AspectMask();

    const auto bpp_in = BytesPerBlock(src.info.format) / DefaultBlockWidth(src.info.format);
    const auto bpp_out = BytesPerBlock(dst.info.format) / DefaultBlockWidth(dst.info.format);
    std::ranges::transform(copies, vk_in_copies.begin(),
                           [src_aspect_mask, bpp_in, bpp_out](const auto& copy) {
                               auto copy2 = copy;
                               copy2.src_offset.x = (bpp_out * copy.src_offset.x) / bpp_in;
                               copy2.extent.width = (bpp_out * copy.extent.width) / bpp_in;
                               return MakeBufferImageCopy(copy2, true, src_aspect_mask);
                           });
    std::ranges::transform(copies, vk_out_copies.begin(), [dst_aspect_mask](const auto& copy) {
        return MakeBufferImageCopy(copy, false, dst_aspect_mask);
    });
    const u32 img_bpp = BytesPerBlock(dst.info.format);
    size_t total_size = 0;
    for (const auto& copy : copies) {
        total_size += copy.extent.width * copy.extent.height * copy.extent.depth * img_bpp;
    }
    const VkBuffer copy_buffer = GetTemporaryBuffer(total_size);
    const VkImage dst_image = dst.Handle();
    const VkImage src_image = src.Handle();
    const bool src_is_3d = src.info.type == ImageType::e3D;
    const bool dst_is_3d = dst.info.type == ImageType::e3D;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst_image, src_image, copy_buffer, src_aspect_mask, dst_aspect_mask,
                      vk_in_copies, vk_out_copies, src_is_3d, dst_is_3d](vk::CommandBuffer cmdbuf) {
        RangedBarrierRange dst_range;
        RangedBarrierRange src_range;
        for (const VkBufferImageCopy& copy : vk_in_copies) {
            src_range.AddLayers(copy.imageSubresource);
        }
        for (const VkBufferImageCopy& copy : vk_out_copies) {
            dst_range.AddLayers(copy.imageSubresource);
        }

        auto src_subresource_range = src_range.SubresourceRange(src_aspect_mask);
        if (src_is_3d) {
            src_subresource_range.layerCount = VK_REMAINING_ARRAY_LAYERS;
        }
        auto dst_subresource_range = dst_range.SubresourceRange(dst_aspect_mask);
        if (dst_is_3d) {
            dst_subresource_range.layerCount = VK_REMAINING_ARRAY_LAYERS;
        }

        static constexpr VkMemoryBarrier READ_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        };
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        const std::array pre_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_subresource_range,
            },
        };
        const std::array middle_in_barrier{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = 0,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_subresource_range,
            },
        };
        const std::array middle_out_barrier{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_subresource_range,
            },
        };
        const std::array post_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_subresource_range,
            },
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, {}, {}, pre_barriers);

        cmdbuf.CopyImageToBuffer(src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_buffer,
                                 vk_in_copies);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, WRITE_BARRIER, nullptr, middle_in_barrier);

        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, READ_BARRIER, {}, middle_out_barrier);
        cmdbuf.CopyBufferToImage(copy_buffer, dst_image, VK_IMAGE_LAYOUT_GENERAL, vk_out_copies);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, {}, {}, post_barriers);
    });
}

void TextureCacheRuntime::BlitImage(Framebuffer* dst_framebuffer, ImageView& dst, ImageView& src,
                                    const Region2D& dst_region, const Region2D& src_region,
                                    Tegra::Engines::Fermi2D::Filter filter,
                                    Tegra::Engines::Fermi2D::Operation operation) {
    const VkImageAspectFlags aspect_mask = ImageAspectMask(src.format);
    const bool is_dst_msaa = dst.Samples() != VK_SAMPLE_COUNT_1_BIT;
    const bool is_src_msaa = src.Samples() != VK_SAMPLE_COUNT_1_BIT;
    if (aspect_mask != ImageAspectMask(dst.format)) {
        UNIMPLEMENTED_MSG("Incompatible blit from format {} to {}", src.format, dst.format);
        return;
    }
    if (aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT && !is_src_msaa && !is_dst_msaa) {
        blit_image_helper.BlitColor(dst_framebuffer, src.Handle(Shader::TextureType::Color2D),
                                    dst_region, src_region, filter, operation);
        return;
    }
    ASSERT(src.format == dst.format);
    if (aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        const auto format = src.format;
        const auto can_blit_depth_stencil = [this, format] {
            switch (format) {
            case VideoCore::Surface::PixelFormat::D24_UNORM_S8_UINT:
            case VideoCore::Surface::PixelFormat::S8_UINT_D24_UNORM:
                return device.IsBlitDepth24Stencil8Supported();
            case VideoCore::Surface::PixelFormat::D32_FLOAT_S8_UINT:
                return device.IsBlitDepth32Stencil8Supported();
            default:
                UNREACHABLE();
            }
        }();
        if (!can_blit_depth_stencil) {
            UNIMPLEMENTED_IF(is_src_msaa || is_dst_msaa);
            blit_image_helper.BlitDepthStencil(dst_framebuffer, src.DepthView(), src.StencilView(),
                                               dst_region, src_region, filter, operation);
            return;
        }
    }
    ASSERT(!(is_dst_msaa && !is_src_msaa));
    ASSERT(operation == Fermi2D::Operation::SrcCopy);

    const VkImage dst_image = dst.ImageHandle();
    const VkImage src_image = src.ImageHandle();
    const VkImageSubresourceLayers dst_layers = MakeSubresourceLayers(&dst);
    const VkImageSubresourceLayers src_layers = MakeSubresourceLayers(&src);
    const bool is_resolve = is_src_msaa && !is_dst_msaa;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([filter, dst_region, src_region, dst_image, src_image, dst_layers, src_layers,
                      aspect_mask, is_resolve](vk::CommandBuffer cmdbuf) {
        const std::array read_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange{
                    .aspectMask = aspect_mask,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange{
                    .aspectMask = aspect_mask,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };
        VkImageMemoryBarrier write_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst_image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, nullptr, nullptr, read_barriers);
        if (is_resolve) {
            cmdbuf.ResolveImage(src_image, VK_IMAGE_LAYOUT_GENERAL, dst_image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                MakeImageResolve(dst_region, src_region, dst_layers, src_layers));
        } else {
            const bool is_linear = filter == Fermi2D::Filter::Bilinear;
            const VkFilter vk_filter = is_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            cmdbuf.BlitImage(
                src_image, VK_IMAGE_LAYOUT_GENERAL, dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                MakeImageBlit(dst_region, src_region, dst_layers, src_layers), vk_filter);
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, write_barrier);
    });
}

void TextureCacheRuntime::ConvertImage(Framebuffer* dst, ImageView& dst_view, ImageView& src_view) {
    switch (dst_view.format) {
    case PixelFormat::R16_UNORM:
        if (src_view.format == PixelFormat::D16_UNORM) {
            return blit_image_helper.ConvertD16ToR16(dst, src_view);
        }
        break;
    case PixelFormat::A8B8G8R8_SRGB:
        if (src_view.format == PixelFormat::D32_FLOAT) {
            return blit_image_helper.ConvertD32FToABGR8(dst, src_view);
        }
        break;
    case PixelFormat::A8B8G8R8_UNORM:
        if (src_view.format == PixelFormat::S8_UINT_D24_UNORM) {
            return blit_image_helper.ConvertD24S8ToABGR8(dst, src_view);
        }
        if (src_view.format == PixelFormat::D24_UNORM_S8_UINT) {
            return blit_image_helper.ConvertS8D24ToABGR8(dst, src_view);
        }
        if (src_view.format == PixelFormat::D32_FLOAT) {
            return blit_image_helper.ConvertD32FToABGR8(dst, src_view);
        }
        if (src_view.format == PixelFormat::R8_UNORM) {
            const Region2D src_region{
                .start = {0, 0},
                .end = {static_cast<s32>(src_view.size.width), static_cast<s32>(src_view.size.height)},
            };
            const Region2D dst_region{
                .start = {0, 0},
                .end = {static_cast<s32>(dst_view.size.width), static_cast<s32>(dst_view.size.height)},
            };
            return blit_image_helper.BlitColor(dst, src_view.Handle(Shader::TextureType::Color2D),
                                               dst_region, src_region,
                                               Tegra::Engines::Fermi2D::Filter::Point,
                                               Tegra::Engines::Fermi2D::Operation::SrcCopy);
        }
        break;
    case PixelFormat::B8G8R8A8_SRGB:
        if (src_view.format == PixelFormat::D32_FLOAT) {
            return blit_image_helper.ConvertD32FToABGR8(dst, src_view);
        }
        break;
    case PixelFormat::B8G8R8A8_UNORM:
        if (src_view.format == PixelFormat::D32_FLOAT) {
            return blit_image_helper.ConvertD32FToABGR8(dst, src_view);
        }
        break;
    case PixelFormat::R32_FLOAT:
        if (src_view.format == PixelFormat::D32_FLOAT) {
            return blit_image_helper.ConvertD32ToR32(dst, src_view);
        }
        break;
    case PixelFormat::D16_UNORM:
        if (src_view.format == PixelFormat::R16_UNORM) {
            return blit_image_helper.ConvertR16ToD16(dst, src_view);
        }
        break;
    case PixelFormat::S8_UINT_D24_UNORM:
        if (src_view.format == PixelFormat::A8B8G8R8_UNORM ||
            src_view.format == PixelFormat::B8G8R8A8_UNORM) {
            return blit_image_helper.ConvertABGR8ToD24S8(dst, src_view);
        }
        break;
    case PixelFormat::D32_FLOAT:
        if (src_view.format == PixelFormat::A8B8G8R8_UNORM ||
            src_view.format == PixelFormat::B8G8R8A8_UNORM ||
            src_view.format == PixelFormat::A8B8G8R8_SRGB ||
            src_view.format == PixelFormat::B8G8R8A8_SRGB) {
            return blit_image_helper.ConvertABGR8ToD32F(dst, src_view);
        }
        if (src_view.format == PixelFormat::R32_FLOAT) {
            return blit_image_helper.ConvertR32ToD32(dst, src_view);
        }
        break;
    default:
        break;
    }

    if (VideoCore::Surface::GetFormatType(dst_view.format) == VideoCore::Surface::SurfaceType::ColorTexture &&
        VideoCore::Surface::GetFormatType(src_view.format) == VideoCore::Surface::SurfaceType::ColorTexture) {
        const Region2D src_region{
            .start = {0, 0},
            .end = {static_cast<s32>(src_view.size.width), static_cast<s32>(src_view.size.height)},
        };
        const Region2D dst_region{
            .start = {0, 0},
            .end = {static_cast<s32>(dst_view.size.width), static_cast<s32>(dst_view.size.height)},
        };
        return blit_image_helper.BlitColor(dst, src_view.Handle(Shader::TextureType::Color2D),
                                           dst_region, src_region,
                                           Tegra::Engines::Fermi2D::Filter::Point,
                                           Tegra::Engines::Fermi2D::Operation::SrcCopy);
    }

    LOG_WARNING(Render_Vulkan, "Unimplemented format copy from {} to {}", src_view.format, dst_view.format);
}

void TextureCacheRuntime::CopyImage(Image& dst, Image& src,
                                    std::span<const VideoCommon::ImageCopy> copies) {
    boost::container::small_vector<VkImageCopy, 16> vk_copies(copies.size());
    const VkImageAspectFlags aspect_mask = dst.AspectMask();
    ASSERT(aspect_mask == src.AspectMask());

// Check format compatibility for vkCmdCopyImage
    const PixelFormat src_format = src.info.format;
    const PixelFormat dst_format = dst.info.format;
    size_t src_bytes_per_block = BytesPerBlock(src_format);
    size_t dst_bytes_per_block = BytesPerBlock(dst_format);

#ifdef __APPLE__
    // On Apple, BCn formats are software-decoded to smaller actual VkImage formats.
    // Use actual pixel size instead of logical block size for compatibility check.
    if (!device.IsOptimalBcnSupported()) {
        auto actual_bpb = [](PixelFormat fmt, size_t logical) -> size_t {
            if (!VideoCore::Surface::IsPixelFormatBCn(fmt)) return logical;
            if (fmt == PixelFormat::BC6H_SFLOAT || fmt == PixelFormat::BC6H_UFLOAT)
                return 8; // decoded to R16G16B16A16_SFLOAT
            return 4; // decoded to A8B8G8R8_UNORM_PACK32
        };
        src_bytes_per_block = actual_bpb(src_format, src_bytes_per_block);
        dst_bytes_per_block = actual_bpb(dst_format, dst_bytes_per_block);
    }
#endif

    // Vulkan spec requires size-compatible formats for vkCmdCopyImage
    if (src_bytes_per_block != dst_bytes_per_block) {
        const bool is_depth_conversion =
            (src_format == PixelFormat::D24_UNORM_S8_UINT && dst_format == PixelFormat::D32_FLOAT_S8_UINT) ||
            (src_format == PixelFormat::D32_FLOAT_S8_UINT && dst_format == PixelFormat::D24_UNORM_S8_UINT);

        const bool is_color_conversion =
            (src_format == PixelFormat::R8G8_UNORM && dst_format == PixelFormat::A8B8G8R8_UNORM) ||
            (src_format == PixelFormat::R8G8_UNORM && dst_format == PixelFormat::A8B8G8R8_SRGB) ||
            (src_format == PixelFormat::A8B8G8R8_UNORM && dst_format == PixelFormat::R8G8_UNORM) ||
            (src_format == PixelFormat::A8B8G8R8_SRGB && dst_format == PixelFormat::R8G8_UNORM) ||
            (src_format == PixelFormat::A4B4G4R4_UNORM && dst_format == PixelFormat::A8B8G8R8_UNORM) ||
            (src_format == PixelFormat::R5G6B5_UNORM && dst_format == PixelFormat::A8B8G8R8_UNORM) ||
            (src_format == PixelFormat::A1R5G5B5_UNORM && dst_format == PixelFormat::A8B8G8R8_UNORM);

        if (is_depth_conversion || is_color_conversion) {
            for (const auto& copy : copies) {
                VideoCommon::ImageViewInfo src_view_info(VideoCommon::ImageViewType::e2D, src_format);
                src_view_info.range.base.level = copy.src_subresource.base_level;
                src_view_info.range.base.layer = copy.src_subresource.base_layer;
                src_view_info.range.extent.levels = 1;
                src_view_info.range.extent.layers = 1;
                ImageView src_view(*this, src_view_info, ImageId{}, src);

                VideoCommon::ImageViewInfo dst_view_info(VideoCommon::ImageViewType::e2D, dst_format);
                dst_view_info.range.base.level = copy.dst_subresource.base_level;
                dst_view_info.range.base.layer = copy.dst_subresource.base_layer;
                dst_view_info.range.extent.levels = 1;
                dst_view_info.range.extent.layers = 1;
                ImageView dst_view(*this, dst_view_info, ImageId{}, dst);

                const u32 dst_width = std::max(1u, dst.info.size.width >> copy.dst_subresource.base_level);
                const u32 dst_height = std::max(1u, dst.info.size.height >> copy.dst_subresource.base_level);
                Framebuffer dst_fb(*this, &dst_view, nullptr, VkExtent2D{dst_width, dst_height}, false);

                const Region2D src_region{
                    .start = {copy.src_offset.x, copy.src_offset.y},
                    .end = {copy.src_offset.x + static_cast<s32>(copy.extent.width), copy.src_offset.y + static_cast<s32>(copy.extent.height)},
                };
                const Region2D dst_region{
                    .start = {copy.dst_offset.x, copy.dst_offset.y},
                    .end = {copy.dst_offset.x + static_cast<s32>(copy.extent.width), copy.dst_offset.y + static_cast<s32>(copy.extent.height)},
                };

                if (is_depth_conversion) {
                    blit_image_helper.BlitDepthStencil(&dst_fb, src_view.DepthView(),
                                                       src_view.StencilView(),
                                                       dst_region, src_region,
                                                       Tegra::Engines::Fermi2D::Filter::Point,
                                                       Tegra::Engines::Fermi2D::Operation::SrcCopy);
                } else {
                    blit_image_helper.BlitColor(&dst_fb, src_view.Handle(Shader::TextureType::Color2D),
                                                dst_region, src_region,
                                                Tegra::Engines::Fermi2D::Filter::Point,
                                                Tegra::Engines::Fermi2D::Operation::SrcCopy);
                }
            }
            return;
        }

        LOG_WARNING(Render_Vulkan, "Skipping image copy due to format size incompatibility: src format {} ({} bytes), dst format {} ({} bytes)",
                    static_cast<u32>(src_format), src_bytes_per_block, static_cast<u32>(dst_format), dst_bytes_per_block);
        return;
    }

    if (src.info.num_samples != dst.info.num_samples) {
        LOG_WARNING(Render_Vulkan, "Skipping image copy due to sample count mismatch: src samples {}, dst samples {}",
                    src.info.num_samples, dst.info.num_samples);
        return;
    }

    std::ranges::transform(copies, vk_copies.begin(), [aspect_mask](const auto& copy) {
        return MakeImageCopy(copy, aspect_mask);
    });
    const VkImage dst_image = dst.Handle();
    const VkImage src_image = src.Handle();
    const bool src_is_3d = src.info.type == ImageType::e3D;
    const bool dst_is_3d = dst.info.type == ImageType::e3D;

#ifdef __APPLE__
    auto&& src_fmt = src.info.format;
    auto&& src_num_samples = src.info.num_samples;
    auto&& dst_fmt = dst.info.format;
    auto&& dst_num_samples = dst.info.num_samples;

    if (src_fmt != dst_fmt && !VideoCore::Surface::IsViewCompatible(src_fmt, dst_fmt, false, false)) {
        bool skip = false;//Important: skip==true may result in some objects not being rendered.
        DBGSCP_HOOK_VOID("TextureCacheRuntime::CopyImageFailedOnApple", skip, src_fmt, dst_fmt, src_num_samples, dst_num_samples, src_bytes_per_block, dst_bytes_per_block);
        if (skip) {
            return;
        }
        // Fallback to blit for incompatible formats instead of risking vkCmdCopyImage failure
        LOG_WARNING(Render_Vulkan, "Image copy on Apple, format mismatch: {} -> {}, using blit fallback",
                    static_cast<u32>(src_fmt), static_cast<u32>(dst_fmt));
        for (const auto& copy : copies) {
            VideoCommon::ImageViewInfo src_view_info(VideoCommon::ImageViewType::e2D, src_format);
            src_view_info.range.base.level = copy.src_subresource.base_level;
            src_view_info.range.base.layer = copy.src_subresource.base_layer;
            src_view_info.range.extent.levels = 1;
            src_view_info.range.extent.layers = 1;
            ImageView src_view(*this, src_view_info, ImageId{}, src);

            VideoCommon::ImageViewInfo dst_view_info(VideoCommon::ImageViewType::e2D, dst_format);
            dst_view_info.range.base.level = copy.dst_subresource.base_level;
            dst_view_info.range.base.layer = copy.dst_subresource.base_layer;
            dst_view_info.range.extent.levels = 1;
            dst_view_info.range.extent.layers = 1;
            ImageView dst_view(*this, dst_view_info, ImageId{}, dst);

            const u32 dst_width = std::max(1u, dst.info.size.width >> copy.dst_subresource.base_level);
            const u32 dst_height = std::max(1u, dst.info.size.height >> copy.dst_subresource.base_level);
            Framebuffer dst_fb(*this, &dst_view, nullptr, VkExtent2D{dst_width, dst_height}, false);

            const Region2D src_region{
                .start = {copy.src_offset.x, copy.src_offset.y},
                .end = {copy.src_offset.x + static_cast<s32>(copy.extent.width),
                        copy.src_offset.y + static_cast<s32>(copy.extent.height)},
            };
            const Region2D dst_region{
                .start = {copy.dst_offset.x, copy.dst_offset.y},
                .end = {copy.dst_offset.x + static_cast<s32>(copy.extent.width),
                        copy.dst_offset.y + static_cast<s32>(copy.extent.height)},
            };

            blit_image_helper.BlitColor(&dst_fb, src_view.Handle(Shader::TextureType::Color2D),
                                        dst_region, src_region,
                                        Tegra::Engines::Fermi2D::Filter::Point,
                                        Tegra::Engines::Fermi2D::Operation::SrcCopy);
        }
        return;
    }
#endif

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst_image, src_image, aspect_mask, vk_copies, src_is_3d, dst_is_3d](vk::CommandBuffer cmdbuf) {
        RangedBarrierRange dst_range;
        RangedBarrierRange src_range;
        for (const VkImageCopy& copy : vk_copies) {
            dst_range.AddLayers(copy.dstSubresource);
            src_range.AddLayers(copy.srcSubresource);
        }

        auto src_subresource_range = src_range.SubresourceRange(aspect_mask);
        if (src_is_3d) {
            src_subresource_range.layerCount = VK_REMAINING_ARRAY_LAYERS;
        }
        auto dst_subresource_range = dst_range.SubresourceRange(aspect_mask);
        if (dst_is_3d) {
            dst_subresource_range.layerCount = VK_REMAINING_ARRAY_LAYERS;
        }

        const std::array pre_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_subresource_range,
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_subresource_range,
            },
        };
        const std::array post_barriers{
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = 0,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = src_image,
                .subresourceRange = src_subresource_range,
            },
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst_image,
                .subresourceRange = dst_subresource_range,
            },
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, {}, {}, pre_barriers);
        cmdbuf.CopyImage(src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vk_copies);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, {}, {}, post_barriers);
    });
}

void TextureCacheRuntime::CopyImageMSAA(Image& dst, Image& src,
                                        std::span<const VideoCommon::ImageCopy> copies) {
    const bool msaa_to_non_msaa = src.info.num_samples > 1 && dst.info.num_samples == 1;
    if (msaa_copy_pass) {
        return msaa_copy_pass->CopyImage(dst, src, copies, msaa_to_non_msaa);
    }
    UNIMPLEMENTED_MSG("Copying images with different samples is not supported.");
}

u64 TextureCacheRuntime::GetDeviceLocalMemory() const {
    return device.GetDeviceLocalMemory();
}

u64 TextureCacheRuntime::GetDeviceMemoryUsage() const {
    return device.GetDeviceMemoryUsage();
}

bool TextureCacheRuntime::CanReportMemoryUsage() const {
    return device.CanReportMemoryUsage();
}

void TextureCacheRuntime::TickFrame() {}

Image::Image(TextureCacheRuntime& runtime_, const ImageInfo& info_, GPUVAddr gpu_addr_,
             VAddr cpu_addr_)
    : VideoCommon::ImageBase(info_, gpu_addr_, cpu_addr_), scheduler{&runtime_.scheduler},
      runtime{&runtime_}, original_image(MakeImage(runtime_.device, runtime_.memory_allocator, info,
                                                   runtime->ViewFormats(info.format))),
      aspect_mask(ImageAspectMask(info.format)) {
    if (IsPixelFormatASTC(info.format) && !runtime->device.IsOptimalAstcSupported()) {
        switch (Settings::values.accelerate_astc.GetValue()) {
        case Settings::AstcDecodeMode::Gpu:
            if (Settings::values.astc_recompression.GetValue() ==
                    Settings::AstcRecompression::Uncompressed &&
                info.size.depth == 1) {
                flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
            }
            break;
        case Settings::AstcDecodeMode::CpuAsynchronous:
            flags |= VideoCommon::ImageFlagBits::AsynchronousDecode;
            break;
        default:
            break;
        }
        flags |= VideoCommon::ImageFlagBits::Converted;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    }
    const auto bcn_mode = Settings::values.bcn_decode_mode.GetValue();
    if (IsPixelFormatBCn(info.format) && !runtime->device.IsOptimalBcnSupported()) {
        if (bcn_mode == Settings::BcnDecodeMode::Gpu && info.size.depth == 1) {
            flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
        }
        flags |= VideoCommon::ImageFlagBits::Converted;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    }
    if (runtime->device.HasDebuggingToolAttached()) {
        original_image.SetObjectNameEXT(VideoCommon::Name(*this).c_str());
    }
    current_image = &Image::original_image;
    storage_image_views.resize(info.resources.levels);
    if (IsPixelFormatASTC(info.format) && !runtime->device.IsOptimalAstcSupported() &&
        Settings::values.astc_recompression.GetValue() ==
            Settings::AstcRecompression::Uncompressed) {
        const auto& device = runtime->device.GetLogical();
        for (s32 level = 0; level < info.resources.levels; ++level) {
            storage_image_views[level] =
                MakeStorageView(device, level, *original_image, VK_FORMAT_A8B8G8R8_UNORM_PACK32);
        }
    }
    // Create storage image views for BCn GPU decoding
    if (IsPixelFormatBCn(info.format) && !runtime->device.IsOptimalBcnSupported()) {
        if (bcn_mode == Settings::BcnDecodeMode::Gpu) {
            const auto& device = runtime->device.GetLogical();
            VkFormat storage_format = VK_FORMAT_A8B8G8R8_UNORM_PACK32;
            if (info.format == PixelFormat::BC6H_UFLOAT ||
                info.format == PixelFormat::BC6H_SFLOAT) {
                storage_format = VK_FORMAT_R16G16B16A16_SFLOAT;
            }
            for (s32 level = 0; level < info.resources.levels; ++level) {
                if (!storage_image_views[level]) {
                    storage_image_views[level] =
                        MakeStorageView(device, level, *original_image, storage_format);
                }
            }
        }
    }
}

Image::Image(const VideoCommon::NullImageParams& params) : VideoCommon::ImageBase{params} {}

Image::~Image() = default;

void Image::UploadMemory(VkBuffer buffer, VkDeviceSize offset,
                         std::span<const VideoCommon::BufferImageCopy> copies) {
    // TODO: Move this to another API
    const bool is_rescaled = True(flags & ImageFlagBits::Rescaled);
    if (is_rescaled) {
        ScaleDown(true);
    }
    scheduler->RequestOutsideRenderPassOperationContext();
    auto vk_copies = TransformBufferImageCopies(copies, offset, aspect_mask);
    const VkBuffer src_buffer = buffer;
    const VkImage vk_image = *original_image;
    const VkImageAspectFlags vk_aspect_mask = aspect_mask;
    const bool is_initialized = std::exchange(initialized, true);
    scheduler->Record([src_buffer, vk_image, vk_aspect_mask, is_initialized,
                       vk_copies](vk::CommandBuffer cmdbuf) {
        CopyBufferToImage(cmdbuf, src_buffer, vk_image, vk_aspect_mask, is_initialized, vk_copies);
    });
    if (is_rescaled) {
        ScaleUp();
    }
}

void Image::UploadMemory(const StagingBufferRef& map, std::span<const BufferImageCopy> copies) {
    UploadMemory(map.buffer, map.offset, copies);
}

void Image::DownloadMemory(VkBuffer buffer, size_t offset,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    std::array buffer_handles{
        buffer,
    };
    std::array buffer_offsets{
        offset,
    };
    DownloadMemory(buffer_handles, buffer_offsets, copies);
}

void Image::DownloadMemory(std::span<VkBuffer> buffers_span, std::span<size_t> offsets_span,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    const bool is_rescaled = True(flags & ImageFlagBits::Rescaled);
    if (is_rescaled) {
        ScaleDown();
    }
    boost::container::small_vector<VkBuffer, 8> buffers_vector{};
    boost::container::small_vector<boost::container::small_vector<VkBufferImageCopy, 16>, 8>
        vk_copies;
    for (size_t index = 0; index < buffers_span.size(); index++) {
        buffers_vector.emplace_back(buffers_span[index]);
        vk_copies.emplace_back(
            TransformBufferImageCopies(copies, offsets_span[index], aspect_mask));
    }
    scheduler->RequestOutsideRenderPassOperationContext();
    scheduler->Record([buffers = std::move(buffers_vector), image = *original_image,
                       aspect_mask_ = aspect_mask, vk_copies](vk::CommandBuffer cmdbuf) {
        const VkImageMemoryBarrier read_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = aspect_mask_,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, read_barrier);

        for (size_t index = 0; index < buffers.size(); index++) {
            cmdbuf.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffers[index],
                                     vk_copies[index]);
        }

        const VkMemoryBarrier memory_write_barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        const VkImageMemoryBarrier image_write_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = aspect_mask_,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, memory_write_barrier, nullptr, image_write_barrier);
    });
    if (is_rescaled) {
        ScaleUp(true);
    }
}

void Image::DownloadMemory(const StagingBufferRef& map, std::span<const BufferImageCopy> copies) {
    std::array buffers{
        map.buffer,
    };
    std::array offsets{
        static_cast<size_t>(map.offset),
    };
    DownloadMemory(buffers, offsets, copies);
}

VkImageView Image::StorageImageView(s32 level) noexcept {
    auto& view = storage_image_views[level];
    if (!view) {
        const auto format_info =
            MaxwellToVK::SurfaceFormat(runtime->device, FormatType::Optimal, true, info.format);
        VkFormat format = ConvertStorageFormat(format_info.format);

        view = MakeStorageView(runtime->device.GetLogical(), level, *(this->*current_image), format);
    }
    return *view;
}

bool Image::IsRescaled() const noexcept {
    return True(flags & ImageFlagBits::Rescaled);
}

bool Image::ScaleUp(bool ignore) {
    const auto& resolution = runtime->resolution;
    if (!resolution.active) {
        return false;
    }
    if (True(flags & ImageFlagBits::Rescaled)) {
        return false;
    }
    ASSERT(info.type != ImageType::Linear);
    flags |= ImageFlagBits::Rescaled;
    has_scaled = true;
    if (!scaled_image) {
        const bool is_2d = (info.type == ImageType::e2D);
        const u32 scaled_width = resolution.ScaleUp(info.size.width);
        const u32 scaled_height = is_2d ? resolution.ScaleUp(info.size.height) : info.size.height;
        auto scaled_info = info;
        scaled_info.size.width = scaled_width;
        scaled_info.size.height = scaled_height;
        scaled_image = MakeImage(runtime->device, runtime->memory_allocator, scaled_info,
                                 runtime->ViewFormats(info.format));
        ignore = false;
    }
    current_image = &Image::scaled_image;
    if (ignore) {
        return true;
    }
    if (aspect_mask == 0) {
        aspect_mask = ImageAspectMask(info.format);
    }
    if (NeedsScaleHelper()) {
        return BlitScaleHelper(true);
    } else {
        BlitScale(*scheduler, *original_image, *scaled_image, info, aspect_mask, resolution);
    }
    return true;
}

bool Image::ScaleDown(bool ignore) {
    const auto& resolution = runtime->resolution;
    if (!resolution.active) {
        return false;
    }
    if (False(flags & ImageFlagBits::Rescaled)) {
        return false;
    }
    ASSERT(info.type != ImageType::Linear);
    flags &= ~ImageFlagBits::Rescaled;
    current_image = &Image::original_image;
    if (ignore) {
        return true;
    }
    if (aspect_mask == 0) {
        aspect_mask = ImageAspectMask(info.format);
    }
    if (NeedsScaleHelper()) {
        return BlitScaleHelper(false);
    } else {
        BlitScale(*scheduler, *scaled_image, *original_image, info, aspect_mask, resolution, false);
    }
    return true;
}

bool Image::BlitScaleHelper(bool scale_up) {
    using namespace VideoCommon;
    static constexpr auto BLIT_OPERATION = Tegra::Engines::Fermi2D::Operation::SrcCopy;
    const bool is_color{aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT};
    const bool is_bilinear{is_color && !IsPixelFormatInteger(info.format)};
    const auto operation = is_bilinear ? Tegra::Engines::Fermi2D::Filter::Bilinear
                                       : Tegra::Engines::Fermi2D::Filter::Point;

    const bool is_2d = (info.type == ImageType::e2D);
    const auto& resolution = runtime->resolution;
    const u32 scaled_width = resolution.ScaleUp(info.size.width);
    const u32 scaled_height = is_2d ? resolution.ScaleUp(info.size.height) : info.size.height;
    std::unique_ptr<ImageView>& blit_view = scale_up ? scale_view : normal_view;
    std::unique_ptr<Framebuffer>& blit_framebuffer =
        scale_up ? scale_framebuffer : normal_framebuffer;
    if (!blit_view) {
        const auto view_info = ImageViewInfo(ImageViewType::e2D, info.format);
        blit_view = std::make_unique<ImageView>(*runtime, view_info, NULL_IMAGE_ID, *this);
    }

    const u32 src_width = scale_up ? info.size.width : scaled_width;
    const u32 src_height = scale_up ? info.size.height : scaled_height;
    const u32 dst_width = scale_up ? scaled_width : info.size.width;
    const u32 dst_height = scale_up ? scaled_height : info.size.height;
    const Region2D src_region{
        .start = {0, 0},
        .end = {static_cast<s32>(src_width), static_cast<s32>(src_height)},
    };
    const Region2D dst_region{
        .start = {0, 0},
        .end = {static_cast<s32>(dst_width), static_cast<s32>(dst_height)},
    };
    const VkExtent2D extent{
        .width = std::max(scaled_width, info.size.width),
        .height = std::max(scaled_height, info.size.height),
    };

    auto* view_ptr = blit_view.get();
    if (aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT) {
        if (!blit_framebuffer) {
            blit_framebuffer =
                std::make_unique<Framebuffer>(*runtime, view_ptr, nullptr, extent, scale_up);
        }
        const auto color_view = blit_view->Handle(Shader::TextureType::Color2D);

        runtime->blit_image_helper.BlitColor(blit_framebuffer.get(), color_view, dst_region,
                                             src_region, operation, BLIT_OPERATION);
    } else if (aspect_mask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if (!blit_framebuffer) {
            blit_framebuffer =
                std::make_unique<Framebuffer>(*runtime, nullptr, view_ptr, extent, scale_up);
        }
        runtime->blit_image_helper.BlitDepthStencil(blit_framebuffer.get(), blit_view->DepthView(),
                                                    blit_view->StencilView(), dst_region,
                                                    src_region, operation, BLIT_OPERATION);
    } else {
        // TODO: Use helper blits where applicable
        flags &= ~ImageFlagBits::Rescaled;
        LOG_ERROR(Render_Vulkan, "Device does not support scaling format {}", info.format);
        return false;
    }
    return true;
}

bool Image::NeedsScaleHelper() const {
    const auto& device = runtime->device;
    const bool needs_msaa_helper = info.num_samples > 1 && device.CantBlitMSAA();
    if (needs_msaa_helper) {
        return true;
    }
    static constexpr auto OPTIMAL_FORMAT = FormatType::Optimal;
    const auto vk_format =
        MaxwellToVK::SurfaceFormat(device, OPTIMAL_FORMAT, false, info.format).format;
    const auto blit_usage = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const bool needs_blit_helper = !device.IsFormatSupported(vk_format, blit_usage, OPTIMAL_FORMAT);
    return needs_blit_helper;
}

ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info,
                     ImageId image_id_, Image& image)
    : VideoCommon::ImageViewBase{info, image.info, image_id_, image.gpu_addr},
      device{&runtime.device}, image_handle{image.Handle()},
      samples(ConvertSampleCount(image.info.num_samples)) {
    using Shader::TextureType;
    const VkImageAspectFlags aspect_mask = ImageViewAspectMask(info);
    std::array<SwizzleSource, 4> swizzle{
        SwizzleSource::R,
        SwizzleSource::G,
        SwizzleSource::B,
        SwizzleSource::A,
    };
    if (!info.IsRenderTarget()) {
        swizzle = info.Swizzle();
        TryTransformSwizzleIfNeeded(format, swizzle, device->MustEmulateBGR565(),
                                    !device->IsExt4444FormatsSupported());
        if ((aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
            std::ranges::transform(swizzle, swizzle.begin(), ConvertGreenRed);
        }
    }
    const auto format_info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
    VkImageUsageFlags usage = image.UsageFlags();
    if (!format_info.storage) {
        usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
    }
    const VkImageViewUsageCreateInfo image_view_usage{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .pNext = nullptr,
        .usage = usage,
    };
    const VkImageViewCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &image_view_usage,
        .flags = 0,
        .image = image.Handle(),
        .viewType = VkImageViewType{},
        .format = format_info.format,
        .components{
            .r = ComponentSwizzle(swizzle[0]),
            .g = ComponentSwizzle(swizzle[1]),
            .b = ComponentSwizzle(swizzle[2]),
            .a = ComponentSwizzle(swizzle[3]),
        },
        .subresourceRange = MakeSubresourceRange(aspect_mask, info.range),
    };
    const auto create = [&](TextureType tex_type, std::optional<u32> num_layers) {
        VkImageViewCreateInfo ci{create_info};
        ci.viewType = ImageViewType(tex_type);
        if (num_layers) {
            ci.subresourceRange.layerCount = *num_layers;
        }
        vk::ImageView handle = device->GetLogical().CreateImageView(ci);
        if (device->HasDebuggingToolAttached()) {
            handle.SetObjectNameEXT(VideoCommon::Name(*this, gpu_addr).c_str());
        }
        image_views[static_cast<size_t>(tex_type)] = std::move(handle);
    };
    switch (info.type) {
    case VideoCommon::ImageViewType::e1D:
    case VideoCommon::ImageViewType::e1DArray:
        create(TextureType::Color1D, 1);
        create(TextureType::ColorArray1D, std::nullopt);
        render_target = Handle(TextureType::ColorArray1D);
        break;
    case VideoCommon::ImageViewType::e2D:
    case VideoCommon::ImageViewType::e2DArray:
    case VideoCommon::ImageViewType::Rect:
        create(TextureType::Color2D, 1);
        create(TextureType::ColorArray2D, std::nullopt);
        render_target = Handle(Shader::TextureType::ColorArray2D);
        break;
    case VideoCommon::ImageViewType::e3D:
        create(TextureType::Color3D, std::nullopt);
        render_target = Handle(Shader::TextureType::Color3D);
        break;
    case VideoCommon::ImageViewType::Cube:
    case VideoCommon::ImageViewType::CubeArray:
        create(TextureType::ColorCube, 6);
        create(TextureType::ColorArrayCube, std::nullopt);
        break;
    case VideoCommon::ImageViewType::Buffer:
        ASSERT(false);
        break;
    }
}

ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info,
                     ImageId image_id_, Image& image, const SlotVector<Image>& slot_imgs)
    : ImageView{runtime, info, image_id_, image} {
    slot_images = &slot_imgs;
}

ImageView::ImageView(TextureCacheRuntime&, const VideoCommon::ImageInfo& info,
                     const VideoCommon::ImageViewInfo& view_info, GPUVAddr gpu_addr_)
    : VideoCommon::ImageViewBase{info, view_info, gpu_addr_},
      buffer_size{VideoCommon::CalculateGuestSizeInBytes(info)} {}

ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::NullImageViewParams& params)
    : VideoCommon::ImageViewBase{params}, device{&runtime.device} {
    if (device->HasNullDescriptor()) {
        return;
    }

    // Handle fallback for devices without nullDescriptor
    ImageInfo info{};
    info.format = PixelFormat::A8B8G8R8_UNORM;

    null_image = MakeImage(*device, runtime.memory_allocator, info, {});
    image_handle = *null_image;
    for (u32 i = 0; i < Shader::NUM_TEXTURE_TYPES; i++) {
        image_views[i] = MakeView(VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_IMAGE_ASPECT_COLOR_BIT);
    }
}

ImageView::~ImageView() = default;

VkImageView ImageView::DepthView() {
    if (!image_handle) {
        return VK_NULL_HANDLE;
    }
    if (depth_view) {
        return *depth_view;
    }
    const auto& info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
    depth_view = MakeView(info.format, VK_IMAGE_ASPECT_DEPTH_BIT);
    return *depth_view;
}

VkImageView ImageView::StencilView() {
    if (!image_handle) {
        return VK_NULL_HANDLE;
    }
    if (stencil_view) {
        return *stencil_view;
    }
    const auto& info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
    stencil_view = MakeView(info.format, VK_IMAGE_ASPECT_STENCIL_BIT);
    return *stencil_view;
}

VkImageView ImageView::ColorView() {
    if (!image_handle) {
        return VK_NULL_HANDLE;
    }
    if (color_view) {
        return *color_view;
    }
    color_view = MakeView(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    return *color_view;
}

VkImageView ImageView::StorageView(Shader::TextureType texture_type,
                                   Shader::ImageFormat image_format) {
    if (!image_handle) {
        return VK_NULL_HANDLE;
    }
    if (image_format == Shader::ImageFormat::Typeless) {
        if (!storage_views) {
            storage_views = std::make_unique<StorageViews>();
        }
        auto& view = storage_views->identity[static_cast<size_t>(texture_type)];
        if (view) {
            return *view;
        }
        const auto format_info = MaxwellToVK::SurfaceFormat(*device, FormatType::Optimal, true, format);
        view = MakeView(format_info.format, VK_IMAGE_ASPECT_COLOR_BIT);
        return *view;
    }
    const bool is_signed{image_format == Shader::ImageFormat::R8_SINT ||
                         image_format == Shader::ImageFormat::R16_SINT};
    if (!storage_views) {
        storage_views = std::make_unique<StorageViews>();
    }
    auto& views{is_signed ? storage_views->signeds : storage_views->unsigneds};
    auto& view{views[static_cast<size_t>(texture_type)]};
    if (view) {
        return *view;
    }
    view = MakeView(Format(image_format), VK_IMAGE_ASPECT_COLOR_BIT);
    return *view;
}

bool ImageView::IsRescaled() const noexcept {
    if (!slot_images) {
        return false;
    }
    const auto& slots = *slot_images;
    const auto& src_image = slots[image_id];
    return src_image.IsRescaled();
}

vk::ImageView ImageView::MakeView(VkFormat vk_format, VkImageAspectFlags aspect_mask) {
    return device->GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image_handle,
        .viewType = ImageViewType(type),
        .format = vk_format,
        .components{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = MakeSubresourceRange(aspect_mask, range),
    });
}

Sampler::Sampler(TextureCacheRuntime& runtime, const Tegra::Texture::TSCEntry& tsc) {
    const auto& device = runtime.device;
    const bool arbitrary_borders = runtime.device.IsExtCustomBorderColorSupported();
    const auto color = tsc.BorderColor();

// Check if the border color can be represented by standard border colors
    const bool can_use_standard_border = ConvertBorderColor(color) != VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    const bool should_use_custom_border = arbitrary_borders && !can_use_standard_border;

    VkSamplerCustomBorderColorCreateInfoEXT border_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
        .pNext = nullptr,
        // TODO: Make use of std::bit_cast once libc++ supports it.
        .customBorderColor = Common::BitCast<VkClearColorValue>(color),
        .format = VK_FORMAT_UNDEFINED,
    };

    VkSamplerReductionModeCreateInfoEXT reduction_ci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .reductionMode = MaxwellToVK::SamplerReduction(tsc.reduction_filter),
    };

    void* pnext = nullptr;
    if (runtime.device.IsExtSamplerFilterMinmaxSupported()) {
        reduction_ci.pNext = pnext;
        pnext = &reduction_ci;
    }
    if (should_use_custom_border) {
        border_ci.pNext = pnext;
        pnext = &border_ci;
    } else if (reduction_ci.reductionMode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT &&
               !runtime.device.IsExtSamplerFilterMinmaxSupported()) {
        LOG_WARNING(Render_Vulkan, "VK_EXT_sampler_filter_minmax is required");
    }
    // Some games have samplers with garbage. Sanitize them here.
    const f32 max_anisotropy = std::clamp(tsc.MaxAnisotropy(), 1.0f, 16.0f);

    const auto create_sampler = [&](const f32 anisotropy) {
        return device.GetLogical().CreateSampler(VkSamplerCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = pnext,
            .flags = 0,
            .magFilter = MaxwellToVK::Sampler::Filter(tsc.mag_filter),
            .minFilter = MaxwellToVK::Sampler::Filter(tsc.min_filter),
            .mipmapMode = MaxwellToVK::Sampler::MipmapMode(tsc.mipmap_filter),
            .addressModeU = MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_u, tsc.mag_filter),
            .addressModeV = MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_v, tsc.mag_filter),
            .addressModeW = MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_p, tsc.mag_filter),
            .mipLodBias = tsc.LodBias(),
            .anisotropyEnable = static_cast<VkBool32>(anisotropy > 1.0f ? VK_TRUE : VK_FALSE),
            .maxAnisotropy = anisotropy,
            .compareEnable = tsc.depth_compare_enabled,
            .compareOp = MaxwellToVK::Sampler::DepthCompareFunction(tsc.depth_compare_func),
            .minLod = tsc.mipmap_filter == TextureMipmapFilter::None ? 0.0f : tsc.MinLod(),
            .maxLod = tsc.mipmap_filter == TextureMipmapFilter::None ? 0.25f : tsc.MaxLod(),
            .borderColor = should_use_custom_border ? VK_BORDER_COLOR_FLOAT_CUSTOM_EXT
                                                    : ConvertBorderColor(color),
            .unnormalizedCoordinates = VK_FALSE,
        });
    };

    sampler = create_sampler(max_anisotropy);

    const f32 max_anisotropy_default = static_cast<f32>(1U << tsc.max_anisotropy);
    if (max_anisotropy > max_anisotropy_default) {
        sampler_default_anisotropy = create_sampler(max_anisotropy_default);
    }
}

Framebuffer::Framebuffer(TextureCacheRuntime& runtime, std::span<ImageView*, NUM_RT> color_buffers,
                         ImageView* depth_buffer, const VideoCommon::RenderTargets& key)
    : render_area{VkExtent2D{
          .width = key.size.width,
          .height = key.size.height,
      }} {
    CreateFramebuffer(runtime, color_buffers, depth_buffer, key.is_rescaled);
    if (runtime.device.HasDebuggingToolAttached()) {
        framebuffer.SetObjectNameEXT(VideoCommon::Name(key).c_str());
    }
}

Framebuffer::Framebuffer(TextureCacheRuntime& runtime, ImageView* color_buffer,
                         ImageView* depth_buffer, VkExtent2D extent, bool is_rescaled_)
    : render_area{extent} {
    std::array<ImageView*, NUM_RT> color_buffers{color_buffer};
    CreateFramebuffer(runtime, color_buffers, depth_buffer, is_rescaled_);
}

Framebuffer::~Framebuffer() = default;

void Framebuffer::CreateFramebuffer(TextureCacheRuntime& runtime,
                                    std::span<ImageView*, NUM_RT> color_buffers,
                                    ImageView* depth_buffer, bool is_rescaled_) {
    boost::container::small_vector<VkImageView, NUM_RT + 1> attachments;
    RenderPassKey renderpass_key{};
    s32 num_layers = 1;

    is_rescaled = is_rescaled_;
    const auto& resolution = runtime.resolution;

    u32 width = std::numeric_limits<u32>::max();
    u32 height = std::numeric_limits<u32>::max();
    for (size_t index = 0; index < NUM_RT; ++index) {
        const ImageView* const color_buffer = color_buffers[index];
        if (!color_buffer) {
            renderpass_key.color_formats[index] = PixelFormat::Invalid;
            continue;
        }
        width = std::min(width, is_rescaled ? resolution.ScaleUp(color_buffer->size.width)
                                            : color_buffer->size.width);
        height = std::min(height, is_rescaled ? resolution.ScaleUp(color_buffer->size.height)
                                              : color_buffer->size.height);
        attachments.push_back(color_buffer->RenderTarget());
        renderpass_key.color_formats[index] = color_buffer->format;
        num_layers = std::max(num_layers, color_buffer->range.extent.layers);
        images[num_images] = color_buffer->ImageHandle();
        image_ranges[num_images] = MakeSubresourceRange(color_buffer);
        rt_map[index] = num_images;
        samples = color_buffer->Samples();
        ++num_images;
    }
    const size_t num_colors = attachments.size();
    if (depth_buffer) {
        width = std::min(width, is_rescaled ? resolution.ScaleUp(depth_buffer->size.width)
                                            : depth_buffer->size.width);
        height = std::min(height, is_rescaled ? resolution.ScaleUp(depth_buffer->size.height)
                                              : depth_buffer->size.height);
        attachments.push_back(depth_buffer->RenderTarget());
        renderpass_key.depth_format = depth_buffer->format;
        num_layers = std::max(num_layers, depth_buffer->range.extent.layers);
        images[num_images] = depth_buffer->ImageHandle();
        const VkImageSubresourceRange subresource_range = MakeSubresourceRange(depth_buffer);
        image_ranges[num_images] = subresource_range;
        samples = depth_buffer->Samples();
        ++num_images;
        has_depth = (subresource_range.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        has_stencil = (subresource_range.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    } else {
        renderpass_key.depth_format = PixelFormat::Invalid;
    }
    renderpass_key.samples = samples;

    renderpass = runtime.render_pass_cache.Get(renderpass_key);
    render_area.width = std::min(render_area.width, width);
    render_area.height = std::min(render_area.height, height);

    num_color_buffers = static_cast<u32>(num_colors);
    framebuffer = runtime.device.GetLogical().CreateFramebuffer({
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = renderpass,
        .attachmentCount = static_cast<u32>(attachments.size()),
        .pAttachments = attachments.data(),
        .width = render_area.width,
        .height = render_area.height,
        .layers = static_cast<u32>(std::max(num_layers, 1)),
    });
}

// DDS file format structures for BCn texture export
namespace {

// GOB (Group of Bytes) constants from Tegra X1 TRM
constexpr u32 GOB_SIZE_X_SHIFT = 6u;                                // 64 bytes
//constexpr u32 GOB_SIZE_Y_SHIFT = 3u;                                // 8 rows
//constexpr u32 GOB_SIZE_SHIFT = GOB_SIZE_X_SHIFT + GOB_SIZE_Y_SHIFT; // 9 => 512 bytes per GOB

/**
 * Deswizzle a single mip level of BCn texture using yuzu's official implementation
 *
 * For BCn textures, we treat each 4x4 compressed block as a single "pixel"
 * with bytes_per_pixel = bytes_per_block (8 for BC1/BC4, 16 for others)
 */
BufferImageCopy DeswizzleBCnMipLevel(std::span<u8> output, std::span<const u8> input,
                                     const VideoCommon::SwizzleParameters& swizzle,
                                     u32 mip_width, u32 mip_height, u32 num_layers,
                                     u32 bytes_per_block) {
    constexpr u32 TILE_WIDTH = 4;
    constexpr u32 TILE_HEIGHT = 4;

    // Validate bytes_per_block
    if (bytes_per_block == 0 || (bytes_per_block & (bytes_per_block - 1)) != 0) {
        LOG_ERROR(Render_Vulkan, "DeswizzleBCn: Invalid bytes_per_block={}", bytes_per_block);
        return BufferImageCopy{};
    }

    // Calculate mip level dimensions in blocks
    const u32 mip_width_in_blocks = Common::DivCeil(mip_width, TILE_WIDTH);
    const u32 mip_height_in_blocks = Common::DivCeil(mip_height, TILE_HEIGHT);

    // Validate dimensions
    if (mip_width == 0 || mip_height == 0) {
        LOG_ERROR(Render_Vulkan, "DeswizzleBCn: Invalid dimensions {}x{}", mip_width, mip_height);
        return BufferImageCopy{};
    }

    // block.height and block.depth are log2 values
    const u32 block_height_log2 = swizzle.block.height;
    const u32 block_depth_log2 = swizzle.block.depth;

    // Validate block_depth for 2D textures
    if (block_depth_log2 != 0) {
        LOG_WARNING(Render_Vulkan,
                    "DeswizzleBCn: Unexpected block_depth_log2={} for 2D texture, expected 0",
                    block_depth_log2);
    }

    // Calculate output size
    const u32 blocks_per_layer = mip_width_in_blocks * mip_height_in_blocks;
    const u32 bytes_per_layer = blocks_per_layer * bytes_per_block;

    LOG_INFO(Render_Vulkan,
             "DeswizzleBCn: {}x{} pixels ({}x{} blocks), {} layers, "
             "bytes_per_block={}, block_height_log2={}, block_depth_log2={}",
             mip_width, mip_height, mip_width_in_blocks, mip_height_in_blocks, num_layers,
             bytes_per_block, block_height_log2, block_depth_log2);

    // Use yuzu's UnswizzleTexture
    // For BCn: treat blocks as "pixels", so width/height are in blocks, not pixels
    Tegra::Texture::UnswizzleTexture(
        output,                  // output buffer
        input,                   // input buffer (swizzled)
        bytes_per_block,         // bytes per "pixel" (actually bytes per block)
        mip_width_in_blocks,     // width in "pixels" (actually blocks)
        mip_height_in_blocks,    // height in "pixels" (actually blocks)
        num_layers,              // depth (number of layers)
        block_height_log2,       // block_height (log2)
        block_depth_log2,        // block_depth (log2)
        GOB_SIZE_X_SHIFT         // stride_alignment
    );

    // Return BufferImageCopy info
    return BufferImageCopy{
        .buffer_offset = 0,
        .buffer_size = static_cast<size_t>(bytes_per_layer) * num_layers,
        .buffer_row_length = Common::AlignUp(mip_width, TILE_WIDTH),
        .buffer_image_height = Common::AlignUp(mip_height, TILE_HEIGHT),
        .image_subresource =
            {
                .base_level = 0,
                .base_layer = 0,
                .num_layers = static_cast<s32>(num_layers),
            },
        .image_offset = {0, 0, 0},
        .image_extent = {mip_width, mip_height, 1},
    };
}

/**
 * Deswizzle all mip levels of BCn texture
 * Each mip level uses its own swizzle parameters
 * BCn formats only support 2D textures and 2D texture arrays
 *
 * @param output Output buffer for all mip levels
 * @param input Input buffer with block-linear swizzled data
 * @param swizzles Array of swizzle parameters, one per mip level
 * @param base_width Base mip level width in pixels
 * @param base_height Base mip level height in pixels
 * @param num_layers Number of array layers (1 for non-array textures)
 * @param bytes_per_block Bytes per compressed block (8 for BC1/BC4, 16 for others)
 * @return Vector of BufferImageCopy structures for each mip level
 */
boost::container::small_vector<BufferImageCopy, 16> DeswizzleBCnTexture(
    std::span<u8> output, std::span<const u8> input,
    std::span<const VideoCommon::SwizzleParameters> swizzles, u32 base_width, u32 base_height,
    u32 num_layers, u32 bytes_per_block) {
    // Validate parameters
    if (swizzles.empty()) {
        LOG_ERROR(Render_Vulkan, "DeswizzleBCn: swizzles is empty");
        return {};
    }
    if (base_width == 0 || base_height == 0 || num_layers == 0) {
        LOG_ERROR(Render_Vulkan, "DeswizzleBCn: Invalid parameters {}x{}, {} layers", base_width,
                  base_height, num_layers);
        return {};
    }

    const u32 num_levels = static_cast<u32>(swizzles.size());
    boost::container::small_vector<BufferImageCopy, 16> copies(num_levels);

    size_t output_offset = 0;

    // Process each mip level with its own swizzle parameters
    for (u32 level = 0; level < num_levels; ++level) {
        // Calculate mip level dimensions
        const u32 mip_width = std::max(1u, base_width >> level);
        const u32 mip_height = std::max(1u, base_height >> level);

        // Verify swizzle level matches
        if (swizzles[level].level != static_cast<s32>(level)) {
            LOG_WARNING(Render_Vulkan,
                        "DeswizzleBCn: Swizzle level mismatch at index {}: "
                        "expected level={}, got level={}",
                        level, level, swizzles[level].level);
        }

        // Get output span for this mip level
        auto mip_output = output.subspan(output_offset);

        // Validate buffer offset before creating subspan
        if (swizzles[level].buffer_offset >= input.size()) {
            LOG_ERROR(Render_Vulkan,
                      "DeswizzleBCn: Invalid buffer_offset={:#x} for level {}, input size={:#x}",
                      swizzles[level].buffer_offset, level, input.size());
            return {};
        }

        // Get input span starting at this mip level's offset
        auto mip_input = input.subspan(swizzles[level].buffer_offset);

        // Deswizzle this mip level using yuzu's implementation
        copies[level] = DeswizzleBCnMipLevel(mip_output, mip_input, swizzles[level], mip_width,
                                             mip_height, num_layers, bytes_per_block);

        // Validate deswizzle result
        if (copies[level].buffer_size == 0) {
            LOG_ERROR(Render_Vulkan, "DeswizzleBCn: Failed to deswizzle level {}", level);
            return {};
        }

        // Update buffer offset and level info
        copies[level].buffer_offset = output_offset;
        copies[level].image_subresource.base_level = level;

        // Move to next mip level in output buffer
        output_offset += copies[level].buffer_size;
    }

    return copies;
}

} // namespace

void DbgScpExportBCnTexture(Image& image, const StagingBufferRef& map,
                            std::span<const VideoCommon::SwizzleParameters> swizzles,
                            const char* tag, Scheduler& scheduler,
                            MemoryAllocator& memory_allocator) {
    bool needExport = false;
    auto&& imgInfo = image.info;
    DBGSCP_HOOK_VOID("DbgScpExportBCnTexture", needExport, image, map, swizzles, imgInfo);
    if (!needExport) {
        return;
    }

    if (swizzles.empty()) {
        LOG_ERROR(Render_Vulkan, "DbgScpExportBCnTexture: swizzles is empty");
        return;
    }

    const u32 width = image.info.size.width;
    const u32 height = image.info.size.height;
    const u32 num_layers = image.info.resources.layers;
    const u32 num_levels = static_cast<u32>(swizzles.size());

    // BCn textures should always be 2D
    if (image.info.size.depth != 1) {
        LOG_WARNING(Render_Vulkan, "DbgScpExportBCnTexture: BCn format with depth={}, expected 1",
                    image.info.size.depth);
    }

    // Get DDS format information
    DDSWriter::DDSFormatInfo formatInfo = DDSWriter::GetDDSFormatInfo(image.info.format);
    const u32 bytes_per_block = formatInfo.bytesPerBlock;

    // Log swizzle parameters for debugging
    LOG_INFO(Render_Vulkan, "Deswizzling BCn texture: {}x{}, {} layers, {} mip levels, format={}",
             width, height, num_layers, num_levels, static_cast<u32>(image.info.format));

    for (u32 level = 0; level < std::min(num_levels, 5u); ++level) {
        const auto& swizzle = swizzles[level];
        const u32 mip_width = std::max(1u, width >> level);
        const u32 mip_height = std::max(1u, height >> level);

        LOG_INFO(Render_Vulkan, "  Mip {}: {}x{}, offset={:#x}, block={}x{}x{}, num_tiles={}x{}x{}",
                 level, mip_width, mip_height, swizzle.buffer_offset, swizzle.block.width,
                 swizzle.block.height, swizzle.block.depth, swizzle.num_tiles.width,
                 swizzle.num_tiles.height, swizzle.num_tiles.depth);
    }
    if (num_levels > 5) {
        LOG_INFO(Render_Vulkan, "  ... and {} more mip levels", num_levels - 5);
    }

    // Calculate total output size for all mip levels
    size_t total_output_size = 0;
    for (u32 level = 0; level < num_levels; ++level) {
        const u32 mip_width = std::max(1u, width >> level);
        const u32 mip_height = std::max(1u, height >> level);
        const u32 mip_width_in_blocks = Common::DivCeil(mip_width, 4u);
        const u32 mip_height_in_blocks = Common::DivCeil(mip_height, 4u);

        total_output_size += static_cast<size_t>(mip_width_in_blocks) * mip_height_in_blocks *
                             bytes_per_block * num_layers;
    }

    LOG_INFO(Render_Vulkan, "Total output size: {} bytes ({} KB)", total_output_size,
             total_output_size / 1024);

    // Allocate output buffer
    std::vector<u8> linear_data(total_output_size);

    // Get input data
    const u8* compressed_data = reinterpret_cast<const u8*>(map.mapped_span.data());
    const size_t source_buffer_size = map.mapped_span.size();

    LOG_INFO(Render_Vulkan, "Input buffer size: {} bytes ({} KB)", source_buffer_size,
             source_buffer_size / 1024);

    // Deswizzle all mip levels
    auto copies =
        DeswizzleBCnTexture(linear_data, std::span<const u8>(compressed_data, source_buffer_size),
                            swizzles, width, height, num_layers, bytes_per_block);

    // Validate deswizzle result
    if (copies.empty()) {
        LOG_ERROR(Render_Vulkan, "DeswizzleBCnTexture failed");
        return;
    }

    // Create dump directory
    const auto dump_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::DumpDir);
    const auto base_dir = dump_dir / "textures";
    if (!Common::FS::CreateDir(dump_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Render_Vulkan, "Failed to create texture dump directories");
        return;
    }

    // Generate filename
    const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    std::string filename =
        fmt::format("{}_{}x{}_L{}_M{}_fmt{}_{:016X}.dds", tag, width, height, num_layers,
                    num_levels, static_cast<u32>(image.info.format), timestamp);

    DDSWriter::WriteDDSLinearData(
        (base_dir / filename).string(), linear_data.data(), linear_data.size(), width, height,
        static_cast<u32>(copies[0].buffer_size / num_layers), num_layers, num_levels, formatInfo);

    LOG_INFO(Render_Vulkan,
             "Exported BCn texture: {} ({} bytes, {}x{} pixels, {} layers, {} mip levels)",
             filename, linear_data.size(), width, height, num_layers, num_levels);

    // Add this flag at the top of your function or as a parameter
    constexpr bool SAVE_AS_DDS_WITH_MIPMAPS = false; // Set to true when you need mipmaps

    if (SAVE_AS_DDS_WITH_MIPMAPS) {
        // Save as DDS with all mipmaps and layers
        std::string savefilename =
            fmt::format("{}_{}x{}_L{}_M{}_fmt{}_{:016X}_decoded.dds", tag, width, height,
                        num_layers, num_levels, static_cast<u32>(image.info.format), timestamp);

        // Collect all mipmap data (organized as: Mip0-Layer0, Mip0-Layer1, ..., Mip1-Layer0, Mip1-Layer1, ...)
        std::vector<DDSWriter::MipmapData> mipmaps;
        std::vector<std::vector<u8>> mipmap_buffers; // Keep buffers alive

        // BC6H decoded textures are in RGBA16F format (8 bytes per pixel), others are RGBA8 (4 bytes per pixel)
        const bool is_bc6h = (image.info.format == VideoCore::Surface::PixelFormat::BC6H_UFLOAT ||
                               image.info.format == VideoCore::Surface::PixelFormat::BC6H_SFLOAT);
        const u32 bytes_per_pixel = is_bc6h ? 8 : 4;

        // Iterate through all layers and mip levels
        // DDS format requires: Layer0[Mip0, Mip1, ...], Layer1[Mip0, Mip1, ...], ...
        for (u32 layer = 0; layer < num_layers; ++layer) {
            for (u32 mip_level = 0; mip_level < num_levels; ++mip_level) {
                const u32 mip_width = std::max(1u, width >> mip_level);
                const u32 mip_height = std::max(1u, height >> mip_level);
                // Create readback buffer for this mip level and layer
                const size_t rgba_size = mip_width * mip_height * bytes_per_pixel;
                const VkBufferCreateInfo buffer_ci = {
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .size = rgba_size,
                    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices = nullptr,
                };
                auto readback_buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::Download);

                // Copy decoded image data from GPU to readback buffer
                scheduler.RequestOutsideRenderPassOperationContext();
                scheduler.Record([&image, &readback_buffer, mip_width, mip_height,
                                  mip_level, layer](vk::CommandBuffer cmdbuf) {
                    // Transition image to TRANSFER_SRC_OPTIMAL
                    const VkImageMemoryBarrier read_barrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = image.Handle(),
                        .subresourceRange =
                            {
                                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .baseMipLevel = mip_level,
                                .levelCount = 1,
                                .baseArrayLayer = layer,
                                .layerCount = 1,
                            },
                    };
                    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, read_barrier);

                    // Copy image to buffer
                    const VkBufferImageCopy copy{
                        .bufferOffset = 0,
                        .bufferRowLength = 0,
                        .bufferImageHeight = 0,
                        .imageSubresource =
                            {
                                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .mipLevel = mip_level,
                                .baseArrayLayer = layer,
                                .layerCount = 1,
                            },
                        .imageOffset = {0, 0, 0},
                        .imageExtent = {mip_width, mip_height, 1},
                    };
                    cmdbuf.CopyImageToBuffer(image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             *readback_buffer, copy);

                    // Transition image back to GENERAL
                    const VkImageMemoryBarrier restore_barrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = image.Handle(),
                        .subresourceRange =
                            {
                                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .baseMipLevel = mip_level,
                                .levelCount = 1,
                                .baseArrayLayer = layer,
                                .layerCount = 1,
                            },
                    };
                    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, restore_barrier);
                });
                scheduler.Finish();

                // Copy data to persistent buffer
                const u8* rgba_data = reinterpret_cast<const u8*>(readback_buffer.Mapped().data());
                std::vector<u8> mip_buffer(rgba_data, rgba_data + rgba_size);

                DDSWriter::MipmapData mip;
                mip.width = mip_width;
                mip.height = mip_height;
                mip.data = mip_buffer.data();
                mip.size = rgba_size;

                mipmap_buffers.push_back(std::move(mip_buffer));
                mipmaps.push_back(mip);
            }
        }

        // Update pointers after all buffers are created
        for (size_t i = 0; i < mipmaps.size(); ++i) {
            mipmaps[i].data = mipmap_buffers[i].data();
        }

        // Write DDS file
        const std::string full_path = (base_dir / savefilename).string();

        // BC6H decoded textures are in RGBA16F format, others are RGBA8
        const bool use_rgba16f = (image.info.format == VideoCore::Surface::PixelFormat::BC6H_UFLOAT ||
                                   image.info.format == VideoCore::Surface::PixelFormat::BC6H_SFLOAT);

        if (!DDSWriter::WriteDDS(full_path, mipmaps, true, use_rgba16f, num_layers)) {
            LOG_ERROR(Render_Vulkan, "Failed to write DDS file: {}", savefilename);
            return;
        }

        LOG_INFO(Render_Vulkan,
                 "Exported decoded BCn texture to DDS: {} ({} mip levels, {} layers, {}x{} base resolution, format={})",
                 savefilename, num_levels, num_layers, width, height, use_rgba16f ? "RGBA16F" : "RGBA8");

    } else {
        // Generate filename with timestamp
        std::string savefilename =
            fmt::format("{}_{}x{}_L{}_M{}_fmt{}_{:016X}_decoded.png", tag, width, height,
                        num_layers, num_levels, static_cast<u32>(image.info.format), timestamp);

        // BC6H decoded textures are in RGBA16F format (8 bytes per pixel), others are RGBA8 (4 bytes per pixel)
        const bool is_bc6h = (image.info.format == VideoCore::Surface::PixelFormat::BC6H_UFLOAT ||
                               image.info.format == VideoCore::Surface::PixelFormat::BC6H_SFLOAT);
        const u32 bytes_per_pixel = is_bc6h ? 8 : 4;

        // Create readback buffer to download decoded data from GPU
        const size_t rgba_size = width * height * bytes_per_pixel;
        const VkBufferCreateInfo buffer_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = rgba_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        auto readback_buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::Download);

        // Copy decoded image data from GPU to readback buffer
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([&image, &readback_buffer, width, height](vk::CommandBuffer cmdbuf) {
            // Transition image to TRANSFER_SRC_OPTIMAL
            const VkImageMemoryBarrier read_barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image.Handle(),
                .subresourceRange =
                    {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, read_barrier);

            // Copy image to buffer
            const VkBufferImageCopy copy{
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource =
                    {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .imageOffset = {0, 0, 0},
                .imageExtent = {width, height, 1},
            };
            cmdbuf.CopyImageToBuffer(image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     *readback_buffer, copy);

            // Transition image back to GENERAL
            const VkImageMemoryBarrier restore_barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image.Handle(),
                .subresourceRange =
                    {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, restore_barrier);
        });
        scheduler.Finish();

        // Map readback buffer and read data
        const u8* rgba_data = reinterpret_cast<const u8*>(readback_buffer.Mapped().data());

        // For BC6H (RGBA16F), we need to convert to RGBA8 for PNG
        std::vector<u8> rgba8_data;
        const u8* png_data = rgba_data;

        if (is_bc6h) {
            // Convert RGBA16F to RGBA8
            rgba8_data.resize(width * height * 4);
            const u16* rgba16f = reinterpret_cast<const u16*>(rgba_data);

            for (size_t i = 0; i < width * height * 4; ++i) {
                // Convert half-float to float, then to 8-bit
                // Simple conversion: clamp to [0, 1] and scale to [0, 255]
                const u16 half = rgba16f[i];

                // Extract sign, exponent, mantissa from half-float (IEEE 754 binary16)
                const u32 sign = (half >> 15) & 0x1;
                const u32 exponent = (half >> 10) & 0x1F;
                const u32 mantissa = half & 0x3FF;

                float value = 0.0f;

                if (exponent == 0) {
                    // Subnormal or zero
                    if (mantissa == 0) {
                        value = sign ? -0.0f : 0.0f;
                    } else {
                        // Subnormal: value = (-1)^sign * 2^-14 * (mantissa / 1024)
                        value = (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa) / 1024.0f, -14);
                    }
                } else if (exponent == 31) {
                    // Infinity or NaN
                    value = (mantissa == 0) ? (sign ? -INFINITY : INFINITY) : NAN;
                } else {
                    // Normal: value = (-1)^sign * 2^(exponent-15) * (1 + mantissa/1024)
                    value = (sign ? -1.0f : 1.0f) * std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, static_cast<int>(exponent) - 15);
                }

                // Clamp to [0, 1] and convert to 8-bit
                // For HDR content, we apply a simple tone mapping
                if (std::isnan(value) || std::isinf(value)) {
                    rgba8_data[i] = 255; // Clamp invalid values to white
                } else {
                    // Simple Reinhard tone mapping for HDR: x / (1 + x)
                    value = std::max(0.0f, value); // Remove negative values
                    value = value / (1.0f + value); // Tone map
                    rgba8_data[i] = static_cast<u8>(std::clamp(value * 255.0f, 0.0f, 255.0f));
                }
            }

            png_data = rgba8_data.data();
        }

        // Write PNG file using stb_image_write
        const std::string full_path = (base_dir / savefilename).string();
        const int stride_in_bytes = width * 4; // RGBA8

        if (!stbi_write_png(full_path.c_str(), width, height, 4, png_data, stride_in_bytes)) {
            LOG_ERROR(Render_Vulkan, "Failed to write PNG file: {}", savefilename);
            return;
        }

        LOG_INFO(
            Render_Vulkan,
            "Exported decoded BCn texture to PNG: {} ({} bytes, {}x{} pixels, {} layers, {} mip "
            "levels, format={})",
            savefilename, linear_data.size(), width, height, num_layers, num_levels,
            is_bc6h ? "RGBA16F->RGBA8" : "RGBA8");
    }
}

void TextureCacheRuntime::AccelerateImageUpload(
    Image & image, const StagingBufferRef& map,
    std::span<const VideoCommon::SwizzleParameters> swizzles) {
        if (IsPixelFormatASTC(image.info.format)) {
            return astc_decoder_pass->Assemble(image, map, swizzles);
        }
    if (IsPixelFormatBCn(image.info.format) && bcn_decoder_pass) {
        // Use specialized decoder for BC6H
        if ((image.info.format == PixelFormat::BC6H_UFLOAT ||
             image.info.format == PixelFormat::BC6H_SFLOAT) &&
            bc6h_decoder_pass) {
            return bc6h_decoder_pass->Decode(image, map, swizzles);
        }

        // Use specialized decoder for BC7
        if ((image.info.format == PixelFormat::BC7_UNORM ||
             image.info.format == PixelFormat::BC7_SRGB) &&
            bc7_decoder_pass) {
            return bc7_decoder_pass->Decode(image, map, swizzles);
        }

        // Fall back to unified BCn decoder
        return bcn_decoder_pass->Decode(image, map, swizzles);
    }
    ASSERT(false);
}

void TextureCacheRuntime::TransitionImageLayout(Image& image) {
    if (!image.ExchangeInitialization()) {
        VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_NONE,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.Handle(),
            .subresourceRange{
                .aspectMask = image.AspectMask(),
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([barrier](vk::CommandBuffer cmdbuf) {
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, barrier);
        });
    }
}

} // namespace Vulkan
