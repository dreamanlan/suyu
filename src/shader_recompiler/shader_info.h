// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bitset>
#include <limits>
#include <map>
#include <tuple>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/type.h"
#include "shader_recompiler/varying_state.h"
#include "video_core/textures/texture.h"

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

namespace Shader {

enum class ReplaceConstant : u32 {
    BaseInstance,
    BaseVertex,
    DrawID,
};

enum class TextureType : u32 {
    Color1D,
    ColorArray1D,
    Color2D,
    ColorArray2D,
    Color3D,
    ColorCube,
    ColorArrayCube,
    Buffer,
    Color2DRect,
};
constexpr u32 NUM_TEXTURE_TYPES = 9;

enum class TexturePixelFormat {
    A8B8G8R8_UNORM,
    A8B8G8R8_SNORM,
    A8B8G8R8_SINT,
    A8B8G8R8_UINT,
    R5G6B5_UNORM,
    B5G6R5_UNORM,
    A1R5G5B5_UNORM,
    A2B10G10R10_UNORM,
    A2B10G10R10_UINT,
    A2R10G10B10_UNORM,
    A1B5G5R5_UNORM,
    A5B5G5R1_UNORM,
    R8_UNORM,
    R8_SNORM,
    R8_SINT,
    R8_UINT,
    R16G16B16A16_FLOAT,
    R16G16B16A16_UNORM,
    R16G16B16A16_SNORM,
    R16G16B16A16_SINT,
    R16G16B16A16_UINT,
    B10G11R11_FLOAT,
    R32G32B32A32_UINT,
    BC1_RGBA_UNORM,
    BC2_UNORM,
    BC3_UNORM,
    BC4_UNORM,
    BC4_SNORM,
    BC5_UNORM,
    BC5_SNORM,
    BC7_UNORM,
    BC6H_UFLOAT,
    BC6H_SFLOAT,
    ASTC_2D_4X4_UNORM,
    B8G8R8A8_UNORM,
    R32G32B32A32_FLOAT,
    R32G32B32A32_SINT,
    R32G32_FLOAT,
    R32G32_SINT,
    R32_FLOAT,
    R16_FLOAT,
    R16_UNORM,
    R16_SNORM,
    R16_UINT,
    R16_SINT,
    R16G16_UNORM,
    R16G16_FLOAT,
    R16G16_UINT,
    R16G16_SINT,
    R16G16_SNORM,
    R32G32B32_FLOAT,
    A8B8G8R8_SRGB,
    R8G8_UNORM,
    R8G8_SNORM,
    R8G8_SINT,
    R8G8_UINT,
    R32G32_UINT,
    R16G16B16X16_FLOAT,
    R32_UINT,
    R32_SINT,
    ASTC_2D_8X8_UNORM,
    ASTC_2D_8X5_UNORM,
    ASTC_2D_5X4_UNORM,
    B8G8R8A8_SRGB,
    BC1_RGBA_SRGB,
    BC2_SRGB,
    BC3_SRGB,
    BC7_SRGB,
    A4B4G4R4_UNORM,
    G4R4_UNORM,
    ASTC_2D_4X4_SRGB,
    ASTC_2D_8X8_SRGB,
    ASTC_2D_8X5_SRGB,
    ASTC_2D_5X4_SRGB,
    ASTC_2D_5X5_UNORM,
    ASTC_2D_5X5_SRGB,
    ASTC_2D_10X8_UNORM,
    ASTC_2D_10X8_SRGB,
    ASTC_2D_6X6_UNORM,
    ASTC_2D_6X6_SRGB,
    ASTC_2D_10X6_UNORM,
    ASTC_2D_10X6_SRGB,
    ASTC_2D_10X5_UNORM,
    ASTC_2D_10X5_SRGB,
    ASTC_2D_10X10_UNORM,
    ASTC_2D_10X10_SRGB,
    ASTC_2D_12X10_UNORM,
    ASTC_2D_12X10_SRGB,
    ASTC_2D_12X12_UNORM,
    ASTC_2D_12X12_SRGB,
    ASTC_2D_8X6_UNORM,
    ASTC_2D_8X6_SRGB,
    ASTC_2D_6X5_UNORM,
    ASTC_2D_6X5_SRGB,
    E5B9G9R9_FLOAT,
    D32_FLOAT,
    D16_UNORM,
    X8_D24_UNORM,
    S8_UINT,
    D24_UNORM_S8_UINT,
    S8_UINT_D24_UNORM,
    D32_FLOAT_S8_UINT,
};

enum class ImageFormat : u32 {
    Typeless,
    R8_UINT,
    R8_SINT,
    R16_UINT,
    R16_SINT,
    R32_UINT,
    R32G32_UINT,
    R32G32B32A32_UINT,
};

enum class Interpolation {
    Smooth,
    Flat,
    NoPerspective,
};

struct ConstantBufferDescriptor {
    u32 index;
    u32 count;

    auto operator<=>(const ConstantBufferDescriptor&) const = default;
};

struct StorageBufferDescriptor {
    u32 cbuf_index;
    u32 cbuf_offset;
    u32 count;
    bool is_written;

    auto operator<=>(const StorageBufferDescriptor&) const = default;
};

struct TextureBufferDescriptor {
    ImageFormat format;
    bool is_integer;
    bool is_signed;
    bool has_secondary;
    u32 cbuf_index;
    u32 cbuf_offset;
    u32 shift_left;
    u32 secondary_cbuf_index;
    u32 secondary_cbuf_offset;
    u32 secondary_shift_left;
    u32 count;
    u32 size_shift;

    auto operator<=>(const TextureBufferDescriptor&) const = default;
};
using TextureBufferDescriptors = boost::container::small_vector<TextureBufferDescriptor, 6>;

struct ImageBufferDescriptor {
    ImageFormat format;
    bool is_written;
    bool is_read;
    bool is_integer;
    u32 cbuf_index;
    u32 cbuf_offset;
    u32 count;
    u32 size_shift;

    auto operator<=>(const ImageBufferDescriptor&) const = default;
};
using ImageBufferDescriptors = boost::container::small_vector<ImageBufferDescriptor, 2>;

struct TextureDescriptor {
    TextureType type;
    bool is_depth;
    bool is_multisample;
    bool has_secondary;
    u32 cbuf_index;
    u32 cbuf_offset;
    u32 shift_left;
    u32 secondary_cbuf_index;
    u32 secondary_cbuf_offset;
    u32 secondary_shift_left;
    u32 count;
    u32 size_shift;
    u32 sampler_index{std::numeric_limits<u32>::max()};

    // Custom comparison excluding sampler_index (it's metadata, not part of identity)
    auto operator<=>(const TextureDescriptor& other) const {
        return std::tie(type, is_depth, is_multisample, has_secondary, cbuf_index, cbuf_offset,
                        shift_left, secondary_cbuf_index, secondary_cbuf_offset,
                        secondary_shift_left, count, size_shift) <=>
               std::tie(other.type, other.is_depth, other.is_multisample, other.has_secondary,
                        other.cbuf_index, other.cbuf_offset, other.shift_left,
                        other.secondary_cbuf_index, other.secondary_cbuf_offset,
                        other.secondary_shift_left, other.count, other.size_shift);
    }
    bool operator==(const TextureDescriptor& other) const {
        return std::tie(type, is_depth, is_multisample, has_secondary, cbuf_index, cbuf_offset,
                        shift_left, secondary_cbuf_index, secondary_cbuf_offset,
                        secondary_shift_left, count, size_shift) ==
               std::tie(other.type, other.is_depth, other.is_multisample, other.has_secondary,
                        other.cbuf_index, other.cbuf_offset, other.shift_left,
                        other.secondary_cbuf_index, other.secondary_cbuf_offset,
                        other.secondary_shift_left, other.count, other.size_shift);
    }
};
using TextureDescriptors = boost::container::small_vector<TextureDescriptor, 12>;

struct ImageDescriptor {
    TextureType type;
    ImageFormat format;
    bool is_written;
    bool is_read;
    bool is_integer;
    u32 cbuf_index;
    u32 cbuf_offset;
    u32 count;
    u32 size_shift;

    auto operator<=>(const ImageDescriptor&) const = default;
};
using ImageDescriptors = boost::container::small_vector<ImageDescriptor, 4>;

struct TextureSamplerInfo {
    Tegra::Texture::WrapMode wrap_u;
    Tegra::Texture::WrapMode wrap_v;
    Tegra::Texture::WrapMode wrap_p;
    bool depth_compare_enabled;
    Tegra::Texture::DepthCompareFunc depth_compare_func;
    bool srgb_conversion;
    u32 max_anisotropy;
    Tegra::Texture::TextureFilter mag_filter;
    Tegra::Texture::TextureFilter min_filter;
    Tegra::Texture::TextureMipmapFilter mipmap_filter;
    bool cubemap_anisotropy;
    bool cubemap_interface_filtering;
    Tegra::Texture::SamplerReduction reduction_filter;
    s32 mip_lod_bias;
    bool float_coord_normalization;
    u32 trilin_opt;
    u32 min_lod_clamp;
    u32 max_lod_clamp;
    u32 srgb_border_color_r;
    u32 srgb_border_color_g;
    u32 srgb_border_color_b;
    std::array<f32, 4> border_color;

    bool operator<(const TextureSamplerInfo& other) const {
        return std::tie(wrap_u, wrap_v, wrap_p, depth_compare_enabled, depth_compare_func,
                        srgb_conversion, max_anisotropy, mag_filter, min_filter, mipmap_filter,
                        cubemap_anisotropy, cubemap_interface_filtering, reduction_filter,
                        mip_lod_bias, float_coord_normalization, trilin_opt, min_lod_clamp,
                        max_lod_clamp, srgb_border_color_r, srgb_border_color_g,
                        srgb_border_color_b, border_color) <
               std::tie(other.wrap_u, other.wrap_v, other.wrap_p, other.depth_compare_enabled,
                        other.depth_compare_func, other.srgb_conversion, other.max_anisotropy,
                        other.mag_filter, other.min_filter, other.mipmap_filter,
                        other.cubemap_anisotropy, other.cubemap_interface_filtering,
                        other.reduction_filter, other.mip_lod_bias, other.float_coord_normalization,
                        other.trilin_opt, other.min_lod_clamp, other.max_lod_clamp,
                        other.srgb_border_color_r, other.srgb_border_color_g,
                        other.srgb_border_color_b, other.border_color);
    }
};

struct Info {
    static constexpr size_t MAX_INDIRECT_CBUFS{14};
    static constexpr size_t MAX_CBUFS{18};
    static constexpr size_t MAX_SSBOS{32};

    bool uses_workgroup_id{};
    bool uses_local_invocation_id{};
    bool uses_invocation_id{};
    bool uses_invocation_info{};
    bool uses_sample_id{};
    bool uses_is_helper_invocation{};
    bool uses_subgroup_invocation_id{};
    bool uses_subgroup_shuffles{};
    std::array<bool, 30> uses_patches{};

    std::array<Interpolation, 32> interpolation{};
    VaryingState loads;
    VaryingState stores;
    VaryingState passthrough;

    std::map<IR::Attribute, IR::Attribute> legacy_stores_mapping;

    bool loads_indexed_attributes{};

    std::array<bool, 8> stores_frag_color{};
    bool stores_sample_mask{};
    bool stores_frag_depth{};

    bool stores_tess_level_outer{};
    bool stores_tess_level_inner{};

    bool stores_indexed_attributes{};

    bool stores_global_memory{};
    bool uses_local_memory{};

    bool uses_fp16{};
    bool uses_fp64{};
    bool uses_fp16_denorms_flush{};
    bool uses_fp16_denorms_preserve{};
    bool uses_fp32_denorms_flush{};
    bool uses_fp32_denorms_preserve{};
    bool uses_int8{};
    bool uses_int16{};
    bool uses_int64{};
    bool uses_image_1d{};
    bool uses_sampled_1d{};
    bool uses_sparse_residency{};
    bool uses_demote_to_helper_invocation{};
    bool uses_subgroup_vote{};
    bool uses_subgroup_mask{};
    bool uses_fswzadd{};
    bool uses_derivatives{};
    bool uses_typeless_image_reads{};
    bool uses_typeless_image_writes{};
    bool uses_image_buffers{};
    bool uses_shared_increment{};
    bool uses_shared_decrement{};
    bool uses_global_increment{};
    bool uses_global_decrement{};
    bool uses_atomic_f32_add{};
    bool uses_atomic_f16x2_add{};
    bool uses_atomic_f16x2_min{};
    bool uses_atomic_f16x2_max{};
    bool uses_atomic_f32x2_add{};
    bool uses_atomic_f32x2_min{};
    bool uses_atomic_f32x2_max{};
    bool uses_atomic_s32_min{};
    bool uses_atomic_s32_max{};
    bool uses_int64_bit_atomics{};
    bool uses_global_memory{};
    bool uses_atomic_image_u32{};
    bool uses_shadow_lod{};
    bool uses_rescaling_uniform{};
    bool uses_cbuf_indirect{};
    bool uses_render_area{};

    IR::Type used_constant_buffer_types{};
    IR::Type used_storage_buffer_types{};
    IR::Type used_indirect_cbuf_types{};

    u32 constant_buffer_mask{};
    std::array<u32, MAX_CBUFS> constant_buffer_used_sizes{};
    u32 nvn_buffer_base{};
    std::bitset<16> nvn_buffer_used{};

    bool requires_layer_emulation{};
    IR::Attribute emulated_layer{};

    u32 used_clip_distances{};

    boost::container::static_vector<ConstantBufferDescriptor, MAX_CBUFS>
        constant_buffer_descriptors;
    boost::container::static_vector<StorageBufferDescriptor, MAX_SSBOS> storage_buffers_descriptors;
    TextureBufferDescriptors texture_buffer_descriptors;
    ImageBufferDescriptors image_buffer_descriptors;
    TextureDescriptors texture_descriptors;
    ImageDescriptors image_descriptors;
    std::vector<std::optional<TextureSamplerInfo>> sampler_descriptors; // Array of actual sampler configuration data
};

template <typename Descriptors>
u32 NumDescriptors(const Descriptors& descriptors) {
    u32 num{};
    for (const auto& desc : descriptors) {
        num += desc.count;
    }
    return num;
}

} // namespace Shader
