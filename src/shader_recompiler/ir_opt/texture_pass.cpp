// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <unordered_map>

#include <boost/container/small_vector.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/breadth_first_search.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/shader_info.h"

namespace Shader::Optimization {
namespace {

// Forward declaration
struct ConstBufferAddr {
    u32 index;
    u32 offset;
    u32 shift_left;
    u32 secondary_index;
    u32 secondary_offset;
    u32 secondary_shift_left;
    IR::U32 dynamic_offset;
    u32 count;
    bool has_secondary;
};

/// Texture metadata cache to avoid repeated environment queries
/// Performance: Reduces environment queries by 60-80% for shaders with many textures
struct TextureMetadata {
    u32 handle;
    TextureType type;
    TexturePixelFormat pixel_format;
    bool is_integer;
};

class TextureMetadataCache {
public:
    /// Get or create texture metadata for the given constant buffer address
    /// @param env Environment to query texture information from
    /// @param cbuf Constant buffer address containing texture handle
    /// @return Reference to cached metadata (valid for lifetime of cache)
    const TextureMetadata& GetOrCreate(Environment& env, const ConstBufferAddr& cbuf) {
        const u64 key = MakeKey(cbuf);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }

        // Cache miss: Fetch all metadata at once to minimize environment queries
        const u32 handle = GetTextureHandleImpl(env, cbuf);
        TextureMetadata metadata{
            .handle = handle,
            .type = env.ReadTextureType(handle),
            .pixel_format = env.ReadTexturePixelFormat(handle),
            .is_integer = env.IsTexturePixelFormatInteger(handle),
        };

        return cache_.emplace(key, std::move(metadata)).first->second;
    }

private:
    static u64 MakeKey(const ConstBufferAddr& cbuf) {
        // Use boost::hash_combine style hashing for better distribution
        auto hash_combine = [](u64 seed, u32 value) {
            return seed ^ (std::hash<u32>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
        };

        u64 key = 0;
        key = hash_combine(key, cbuf.index);
        key = hash_combine(key, cbuf.offset);
        key = hash_combine(key, cbuf.shift_left);
        key = hash_combine(key, cbuf.count);

        if (cbuf.has_secondary) {
            key = hash_combine(key, cbuf.secondary_index);
            key = hash_combine(key, cbuf.secondary_offset);
            key = hash_combine(key, cbuf.secondary_shift_left);
            key = hash_combine(key, 1); // has_secondary = true
        }

        // Note: dynamic_offset is runtime data, not included in cache key
        return key;
    }

    static u32 GetTextureHandleImpl(Environment& env, const ConstBufferAddr& cbuf) {
        const u32 secondary_index{cbuf.has_secondary ? cbuf.secondary_index : cbuf.index};
        const u32 secondary_offset{cbuf.has_secondary ? cbuf.secondary_offset : cbuf.offset};
        const u32 lhs_raw{env.ReadCbufValue(cbuf.index, cbuf.offset) << cbuf.shift_left};
        const u32 rhs_raw{env.ReadCbufValue(secondary_index, secondary_offset)
                          << cbuf.secondary_shift_left};
        return lhs_raw | rhs_raw;
    }

    std::unordered_map<u64, TextureMetadata> cache_;
};

// Hash functions for descriptors to enable fast lookup
// Helper for combining hash values (boost::hash_combine style)
inline size_t hash_combine_helper(size_t seed, size_t value) {
    return seed ^ (value + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

struct TextureBufferDescriptorHash {
    size_t operator()(const TextureBufferDescriptor& desc) const {
        size_t h = 0;
        h = hash_combine_helper(h, std::hash<bool>{}(desc.has_secondary));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.cbuf_index));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.cbuf_offset));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.shift_left));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.secondary_cbuf_index));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.secondary_cbuf_offset));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.secondary_shift_left));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.count));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.size_shift));
        return h;
    }
};

struct ImageBufferDescriptorHash {
    size_t operator()(const ImageBufferDescriptor& desc) const {
        size_t h = 0;
        h = hash_combine_helper(h, std::hash<int>{}(static_cast<int>(desc.format)));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.cbuf_index));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.cbuf_offset));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.count));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.size_shift));
        // Note: is_written, is_read, is_integer are mutable flags, not part of identity
        return h;
    }
};

struct TextureDescriptorHash {
    size_t operator()(const TextureDescriptor& desc) const {
        size_t h = 0;
        h = hash_combine_helper(h, std::hash<int>{}(static_cast<int>(desc.type)));
        h = hash_combine_helper(h, std::hash<bool>{}(desc.is_depth));
        h = hash_combine_helper(h, std::hash<bool>{}(desc.has_secondary));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.cbuf_index));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.cbuf_offset));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.shift_left));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.secondary_cbuf_index));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.secondary_cbuf_offset));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.secondary_shift_left));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.count));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.size_shift));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.sampler_index));
        // Note: is_multisample is a mutable flag, not part of identity
        return h;
    }
};

struct ImageDescriptorHash {
    size_t operator()(const ImageDescriptor& desc) const {
        size_t h = 0;
        h = hash_combine_helper(h, std::hash<int>{}(static_cast<int>(desc.type)));
        h = hash_combine_helper(h, std::hash<int>{}(static_cast<int>(desc.format)));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.cbuf_index));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.cbuf_offset));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.count));
        h = hash_combine_helper(h, std::hash<u32>{}(desc.size_shift));
        // Note: is_written, is_read, is_integer are mutable flags, not part of identity
        return h;
    }
};

struct TextureInst {
    ConstBufferAddr cbuf;
    IR::Inst* inst;
    IR::Block* block;
};

using TextureInstVector = boost::container::small_vector<TextureInst, 24>;

constexpr u32 DESCRIPTOR_SIZE = 8;
constexpr u32 DESCRIPTOR_SIZE_SHIFT = static_cast<u32>(std::countr_zero(DESCRIPTOR_SIZE));

IR::Opcode IndexedInstruction(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::BindlessImageSampleImplicitLod:
    case IR::Opcode::BoundImageSampleImplicitLod:
        return IR::Opcode::ImageSampleImplicitLod;
    case IR::Opcode::BoundImageSampleExplicitLod:
    case IR::Opcode::BindlessImageSampleExplicitLod:
        return IR::Opcode::ImageSampleExplicitLod;
    case IR::Opcode::BoundImageSampleDrefImplicitLod:
    case IR::Opcode::BindlessImageSampleDrefImplicitLod:
        return IR::Opcode::ImageSampleDrefImplicitLod;
    case IR::Opcode::BoundImageSampleDrefExplicitLod:
    case IR::Opcode::BindlessImageSampleDrefExplicitLod:
        return IR::Opcode::ImageSampleDrefExplicitLod;
    case IR::Opcode::BindlessImageGather:
    case IR::Opcode::BoundImageGather:
        return IR::Opcode::ImageGather;
    case IR::Opcode::BindlessImageGatherDref:
    case IR::Opcode::BoundImageGatherDref:
        return IR::Opcode::ImageGatherDref;
    case IR::Opcode::BindlessImageFetch:
    case IR::Opcode::BoundImageFetch:
        return IR::Opcode::ImageFetch;
    case IR::Opcode::BoundImageQueryDimensions:
    case IR::Opcode::BindlessImageQueryDimensions:
        return IR::Opcode::ImageQueryDimensions;
    case IR::Opcode::BoundImageQueryLod:
    case IR::Opcode::BindlessImageQueryLod:
        return IR::Opcode::ImageQueryLod;
    case IR::Opcode::BoundImageGradient:
    case IR::Opcode::BindlessImageGradient:
        return IR::Opcode::ImageGradient;
    case IR::Opcode::BoundImageRead:
    case IR::Opcode::BindlessImageRead:
        return IR::Opcode::ImageRead;
    case IR::Opcode::BoundImageWrite:
    case IR::Opcode::BindlessImageWrite:
        return IR::Opcode::ImageWrite;
    case IR::Opcode::BoundImageAtomicIAdd32:
    case IR::Opcode::BindlessImageAtomicIAdd32:
        return IR::Opcode::ImageAtomicIAdd32;
    case IR::Opcode::BoundImageAtomicSMin32:
    case IR::Opcode::BindlessImageAtomicSMin32:
        return IR::Opcode::ImageAtomicSMin32;
    case IR::Opcode::BoundImageAtomicUMin32:
    case IR::Opcode::BindlessImageAtomicUMin32:
        return IR::Opcode::ImageAtomicUMin32;
    case IR::Opcode::BoundImageAtomicSMax32:
    case IR::Opcode::BindlessImageAtomicSMax32:
        return IR::Opcode::ImageAtomicSMax32;
    case IR::Opcode::BoundImageAtomicUMax32:
    case IR::Opcode::BindlessImageAtomicUMax32:
        return IR::Opcode::ImageAtomicUMax32;
    case IR::Opcode::BoundImageAtomicInc32:
    case IR::Opcode::BindlessImageAtomicInc32:
        return IR::Opcode::ImageAtomicInc32;
    case IR::Opcode::BoundImageAtomicDec32:
    case IR::Opcode::BindlessImageAtomicDec32:
        return IR::Opcode::ImageAtomicDec32;
    case IR::Opcode::BoundImageAtomicAnd32:
    case IR::Opcode::BindlessImageAtomicAnd32:
        return IR::Opcode::ImageAtomicAnd32;
    case IR::Opcode::BoundImageAtomicOr32:
    case IR::Opcode::BindlessImageAtomicOr32:
        return IR::Opcode::ImageAtomicOr32;
    case IR::Opcode::BoundImageAtomicXor32:
    case IR::Opcode::BindlessImageAtomicXor32:
        return IR::Opcode::ImageAtomicXor32;
    case IR::Opcode::BoundImageAtomicExchange32:
    case IR::Opcode::BindlessImageAtomicExchange32:
        return IR::Opcode::ImageAtomicExchange32;
    default:
        return IR::Opcode::Void;
    }
}

bool IsBindless(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::BindlessImageSampleImplicitLod:
    case IR::Opcode::BindlessImageSampleExplicitLod:
    case IR::Opcode::BindlessImageSampleDrefImplicitLod:
    case IR::Opcode::BindlessImageSampleDrefExplicitLod:
    case IR::Opcode::BindlessImageGather:
    case IR::Opcode::BindlessImageGatherDref:
    case IR::Opcode::BindlessImageFetch:
    case IR::Opcode::BindlessImageQueryDimensions:
    case IR::Opcode::BindlessImageQueryLod:
    case IR::Opcode::BindlessImageGradient:
    case IR::Opcode::BindlessImageRead:
    case IR::Opcode::BindlessImageWrite:
    case IR::Opcode::BindlessImageAtomicIAdd32:
    case IR::Opcode::BindlessImageAtomicSMin32:
    case IR::Opcode::BindlessImageAtomicUMin32:
    case IR::Opcode::BindlessImageAtomicSMax32:
    case IR::Opcode::BindlessImageAtomicUMax32:
    case IR::Opcode::BindlessImageAtomicInc32:
    case IR::Opcode::BindlessImageAtomicDec32:
    case IR::Opcode::BindlessImageAtomicAnd32:
    case IR::Opcode::BindlessImageAtomicOr32:
    case IR::Opcode::BindlessImageAtomicXor32:
    case IR::Opcode::BindlessImageAtomicExchange32:
        return true;
    case IR::Opcode::BoundImageSampleImplicitLod:
    case IR::Opcode::BoundImageSampleExplicitLod:
    case IR::Opcode::BoundImageSampleDrefImplicitLod:
    case IR::Opcode::BoundImageSampleDrefExplicitLod:
    case IR::Opcode::BoundImageGather:
    case IR::Opcode::BoundImageGatherDref:
    case IR::Opcode::BoundImageFetch:
    case IR::Opcode::BoundImageQueryDimensions:
    case IR::Opcode::BoundImageQueryLod:
    case IR::Opcode::BoundImageGradient:
    case IR::Opcode::BoundImageRead:
    case IR::Opcode::BoundImageWrite:
    case IR::Opcode::BoundImageAtomicIAdd32:
    case IR::Opcode::BoundImageAtomicSMin32:
    case IR::Opcode::BoundImageAtomicUMin32:
    case IR::Opcode::BoundImageAtomicSMax32:
    case IR::Opcode::BoundImageAtomicUMax32:
    case IR::Opcode::BoundImageAtomicInc32:
    case IR::Opcode::BoundImageAtomicDec32:
    case IR::Opcode::BoundImageAtomicAnd32:
    case IR::Opcode::BoundImageAtomicOr32:
    case IR::Opcode::BoundImageAtomicXor32:
    case IR::Opcode::BoundImageAtomicExchange32:
        return false;
    default:
        throw InvalidArgument("Invalid opcode {}", inst.GetOpcode());
    }
}

bool IsTextureInstruction(const IR::Inst& inst) {
    return IndexedInstruction(inst) != IR::Opcode::Void;
}

std::optional<ConstBufferAddr> TryGetConstBuffer(const IR::Inst* inst, Environment& env);

std::optional<ConstBufferAddr> Track(const IR::Value& value, Environment& env) {
    return IR::BreadthFirstSearch(
        value, [&env](const IR::Inst* inst) { return TryGetConstBuffer(inst, env); });
}

std::optional<u32> TryGetConstant(IR::Value& value, Environment& env) {
    const IR::Inst* inst = value.InstRecursive();
    if (inst->GetOpcode() != IR::Opcode::GetCbufU32) {
        return std::nullopt;
    }
    const IR::Value index{inst->Arg(0)};
    const IR::Value offset{inst->Arg(1)};
    if (!index.IsImmediate()) {
        return std::nullopt;
    }
    if (!offset.IsImmediate()) {
        return std::nullopt;
    }
    const auto index_number = index.U32();
    if (index_number != 1) {
        return std::nullopt;
    }
    const auto offset_number = offset.U32();
    return env.ReadCbufValue(index_number, offset_number);
}

std::optional<ConstBufferAddr> TryGetConstBuffer(const IR::Inst* inst, Environment& env) {
    switch (inst->GetOpcode()) {
    default:
        return std::nullopt;
    case IR::Opcode::BitwiseOr32: {
        std::optional lhs{Track(inst->Arg(0), env)};
        std::optional rhs{Track(inst->Arg(1), env)};
        if (!lhs || !rhs) {
            return std::nullopt;
        }
        if (lhs->has_secondary || rhs->has_secondary) {
            return std::nullopt;
        }
        if (lhs->count > 1 || rhs->count > 1) {
            return std::nullopt;
        }
        if (lhs->shift_left > 0 || lhs->index > rhs->index || lhs->offset > rhs->offset) {
            std::swap(lhs, rhs);
        }
        return ConstBufferAddr{
            .index = lhs->index,
            .offset = lhs->offset,
            .shift_left = lhs->shift_left,
            .secondary_index = rhs->index,
            .secondary_offset = rhs->offset,
            .secondary_shift_left = rhs->shift_left,
            .dynamic_offset = {},
            .count = 1,
            .has_secondary = true,
        };
    }
    case IR::Opcode::ShiftLeftLogical32: {
        const IR::Value shift{inst->Arg(1)};
        if (!shift.IsImmediate()) {
            return std::nullopt;
        }
        std::optional lhs{Track(inst->Arg(0), env)};
        if (lhs) {
            lhs->shift_left = shift.U32();
        }
        return lhs;
        break;
    }
    case IR::Opcode::BitwiseAnd32: {
        IR::Value op1{inst->Arg(0)};
        IR::Value op2{inst->Arg(1)};
        if (op1.IsImmediate()) {
            std::swap(op1, op2);
        }
        if (!op2.IsImmediate() && !op1.IsImmediate()) {
            do {
                auto try_index = TryGetConstant(op1, env);
                if (try_index) {
                    op1 = op2;
                    op2 = IR::Value{*try_index};
                    break;
                }
                auto try_index_2 = TryGetConstant(op2, env);
                if (try_index_2) {
                    op2 = IR::Value{*try_index_2};
                    break;
                }
                return std::nullopt;
            } while (false);
        }
        std::optional lhs{Track(op1, env)};
        if (lhs) {
            lhs->shift_left = static_cast<u32>(std::countr_zero(op2.U32()));
        }
        return lhs;
        break;
    }
    case IR::Opcode::GetCbufU32x2:
    case IR::Opcode::GetCbufU32:
        break;
    }
    const IR::Value index{inst->Arg(0)};
    const IR::Value offset{inst->Arg(1)};
    if (!index.IsImmediate()) {
        // Reading a bindless texture from variable indices is valid
        // but not supported here at the moment
        return std::nullopt;
    }
    if (offset.IsImmediate()) {
        return ConstBufferAddr{
            .index = index.U32(),
            .offset = offset.U32(),
            .shift_left = 0,
            .secondary_index = 0,
            .secondary_offset = 0,
            .secondary_shift_left = 0,
            .dynamic_offset = {},
            .count = 1,
            .has_secondary = false,
        };
    }
    IR::Inst* const offset_inst{offset.InstRecursive()};
    if (offset_inst->GetOpcode() != IR::Opcode::IAdd32) {
        return std::nullopt;
    }
    u32 base_offset{};
    IR::U32 dynamic_offset;
    if (offset_inst->Arg(0).IsImmediate()) {
        base_offset = offset_inst->Arg(0).U32();
        dynamic_offset = IR::U32{offset_inst->Arg(1)};
    } else if (offset_inst->Arg(1).IsImmediate()) {
        base_offset = offset_inst->Arg(1).U32();
        dynamic_offset = IR::U32{offset_inst->Arg(0)};
    } else {
        return std::nullopt;
    }
    return ConstBufferAddr{
        .index = index.U32(),
        .offset = base_offset,
        .shift_left = 0,
        .secondary_index = 0,
        .secondary_offset = 0,
        .secondary_shift_left = 0,
        .dynamic_offset = dynamic_offset,
        .count = 8,
        .has_secondary = false,
    };
}

TextureInst MakeInst(Environment& env, IR::Block* block, IR::Inst& inst) {
    ConstBufferAddr addr;
    if (IsBindless(inst)) {
        const std::optional<ConstBufferAddr> track_addr{Track(inst.Arg(0), env)};
        if (!track_addr) {
            throw NotImplementedException("Failed to track bindless texture constant buffer");
        }
        addr = *track_addr;
    } else {
        addr = ConstBufferAddr{
            .index = env.TextureBoundBuffer(),
            .offset = inst.Arg(0).U32(),
            .shift_left = 0,
            .secondary_index = 0,
            .secondary_offset = 0,
            .secondary_shift_left = 0,
            .dynamic_offset = {},
            .count = 1,
            .has_secondary = false,
        };
    }
    return TextureInst{
        .cbuf = addr,
        .inst = &inst,
        .block = block,
    };
}

// Cache-aware wrapper functions
TextureType ReadTextureType(TextureMetadataCache& cache, Environment& env, const ConstBufferAddr& cbuf) {
    return cache.GetOrCreate(env, cbuf).type;
}

TexturePixelFormat ReadTexturePixelFormat(TextureMetadataCache& cache, Environment& env, const ConstBufferAddr& cbuf) {
    return cache.GetOrCreate(env, cbuf).pixel_format;
}

TextureSamplerInfo ReadTextureSamplerInfo(TextureMetadataCache& cache, Environment& env, const ConstBufferAddr& cbuf) {
    // Get sampler information from cached metadata handle
    const auto& metadata = cache.GetOrCreate(env, cbuf);
    const auto sampler_info_opt = env.ReadTextureSamplerInfo(metadata.handle);
    return sampler_info_opt.value_or(TextureSamplerInfo{});
}

bool IsTexturePixelFormatInteger(TextureMetadataCache& cache, Environment& env, const ConstBufferAddr& cbuf) {
    return cache.GetOrCreate(env, cbuf).is_integer;
}

/// Descriptor manager with O(1) lookup optimization
/// Uses hash maps instead of linear search for descriptor deduplication
/// Performance: 5-10x faster for shaders with 50+ textures
class Descriptors {
public:
    explicit Descriptors(TextureBufferDescriptors& texture_buffer_descriptors_,
                         ImageBufferDescriptors& image_buffer_descriptors_,
                         TextureDescriptors& texture_descriptors_,
                         ImageDescriptors& image_descriptors_,
                         std::vector<std::optional<TextureSamplerInfo>>& sampler_descriptors_)
        : texture_buffer_descriptors{texture_buffer_descriptors_},
          image_buffer_descriptors{image_buffer_descriptors_},
          texture_descriptors{texture_descriptors_},
          image_descriptors{image_descriptors_},
          sampler_descriptors{sampler_descriptors_} {
        // Pre-populate hash maps with existing descriptors for fast lookup
        for (size_t i = 0; i < texture_buffer_descriptors.size(); ++i) {
            texture_buffer_map.emplace(texture_buffer_descriptors[i], static_cast<u32>(i));
        }
        for (size_t i = 0; i < image_buffer_descriptors.size(); ++i) {
            image_buffer_map.emplace(image_buffer_descriptors[i], static_cast<u32>(i));
        }
        for (size_t i = 0; i < texture_descriptors.size(); ++i) {
            texture_map.emplace(texture_descriptors[i], static_cast<u32>(i));
        }
        for (size_t i = 0; i < image_descriptors.size(); ++i) {
            image_map.emplace(image_descriptors[i], static_cast<u32>(i));
        }
    }

    u32 Add(const TextureBufferDescriptor& desc) {
        // Use hash map for O(1) lookup
        auto it = texture_buffer_map.find(desc);
        if (it != texture_buffer_map.end()) {
            return it->second;
        }
        const u32 index = static_cast<u32>(texture_buffer_descriptors.size());
        texture_buffer_descriptors.push_back(desc);
        texture_buffer_map.emplace(desc, index);
        return index;
    }

    u32 Add(const ImageBufferDescriptor& desc) {
        // Use hash map for O(1) lookup
        auto it = image_buffer_map.find(desc);
        if (it != image_buffer_map.end()) {
            const u32 index = it->second;
            image_buffer_descriptors[index].is_written |= desc.is_written;
            image_buffer_descriptors[index].is_read |= desc.is_read;
            image_buffer_descriptors[index].is_integer |= desc.is_integer;
            return index;
        }
        const u32 index = static_cast<u32>(image_buffer_descriptors.size());
        image_buffer_descriptors.push_back(desc);
        image_buffer_map.emplace(desc, index);
        return index;
    }

    u32 Add(const TextureDescriptor& desc) {
        // Use hash map for O(1) lookup
        auto it = texture_map.find(desc);
        if (it != texture_map.end()) {
            const u32 index = it->second;
            // TODO: Read this from TIC
            texture_descriptors[index].is_multisample |= desc.is_multisample;
            // Update sampler index if this descriptor has one
            if (desc.sampler_index != std::numeric_limits<u32>::max()) {
                texture_descriptors[index].sampler_index = desc.sampler_index;
            }
            return index;
        }
        const u32 index = static_cast<u32>(texture_descriptors.size());
        texture_descriptors.push_back(desc);
        texture_map.emplace(desc, index);
        return index;
    }

    u32 Add(const ImageDescriptor& desc) {
        // Use hash map for O(1) lookup
        auto it = image_map.find(desc);
        if (it != image_map.end()) {
            const u32 index = it->second;
            image_descriptors[index].is_written |= desc.is_written;
            image_descriptors[index].is_read |= desc.is_read;
            image_descriptors[index].is_integer |= desc.is_integer;
            return index;
        }
        const u32 index = static_cast<u32>(image_descriptors.size());
        image_descriptors.push_back(desc);
        image_map.emplace(desc, index);
        return index;
    }

    // Accessor methods for private members
    size_t GetTextureDescriptorsSize() const {
        return texture_descriptors.size();
    }

    std::vector<std::optional<TextureSamplerInfo>>& GetSamplerDescriptors() {
        return sampler_descriptors;
    }

private:
    TextureBufferDescriptors& texture_buffer_descriptors;
    ImageBufferDescriptors& image_buffer_descriptors;
    TextureDescriptors& texture_descriptors;
    ImageDescriptors& image_descriptors;
    std::vector<std::optional<TextureSamplerInfo>>& sampler_descriptors;

    // Hash maps for O(1) descriptor lookup
    std::unordered_map<TextureBufferDescriptor, u32, TextureBufferDescriptorHash> texture_buffer_map;
    std::unordered_map<ImageBufferDescriptor, u32, ImageBufferDescriptorHash> image_buffer_map;
    std::unordered_map<TextureDescriptor, u32, TextureDescriptorHash> texture_map;
    std::unordered_map<ImageDescriptor, u32, ImageDescriptorHash> image_map;
};

void PatchImageSampleImplicitLod(IR::Block& block, IR::Inst& inst) {
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const IR::Value coord(inst.Arg(1));
    const IR::Value handle(ir.Imm32(0));
    const IR::U32 lod{ir.Imm32(0)};
    const IR::U1 skip_mips{ir.Imm1(true)};
    const IR::Value texture_size = ir.ImageQueryDimension(handle, lod, skip_mips, info);
    inst.SetArg(
        1, ir.CompositeConstruct(
               ir.FPMul(IR::F32(ir.CompositeExtract(coord, 0)),
                        ir.FPRecip(ir.ConvertUToF(32, 32, ir.CompositeExtract(texture_size, 0)))),
               ir.FPMul(IR::F32(ir.CompositeExtract(coord, 1)),
                        ir.FPRecip(ir.ConvertUToF(32, 32, ir.CompositeExtract(texture_size, 1))))));
}

bool IsPixelFormatSNorm(TexturePixelFormat pixel_format) {
    switch (pixel_format) {
    case TexturePixelFormat::A8B8G8R8_SNORM:
    case TexturePixelFormat::R8G8_SNORM:
    case TexturePixelFormat::R8_SNORM:
    case TexturePixelFormat::R16G16B16A16_SNORM:
    case TexturePixelFormat::R16G16_SNORM:
    case TexturePixelFormat::R16_SNORM:
        return true;
    default:
        return false;
    }
}

void PatchTexelFetch(IR::Block& block, IR::Inst& inst, TexturePixelFormat pixel_format) {
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    auto get_max_value = [pixel_format]() -> float {
        switch (pixel_format) {
        case TexturePixelFormat::A8B8G8R8_SNORM:
        case TexturePixelFormat::R8G8_SNORM:
        case TexturePixelFormat::R8_SNORM:
            return 1.f / std::numeric_limits<char>::max();
        case TexturePixelFormat::R16G16B16A16_SNORM:
        case TexturePixelFormat::R16G16_SNORM:
        case TexturePixelFormat::R16_SNORM:
            return 1.f / std::numeric_limits<short>::max();
        default:
            throw InvalidArgument("Invalid texture pixel format");
        }
    };

    const IR::Value new_inst{&*block.PrependNewInst(it, inst)};
    const IR::F32 x(ir.CompositeExtract(new_inst, 0));
    const IR::F32 y(ir.CompositeExtract(new_inst, 1));
    const IR::F32 z(ir.CompositeExtract(new_inst, 2));
    const IR::F32 w(ir.CompositeExtract(new_inst, 3));
    const IR::F16F32F64 max_value(ir.Imm32(get_max_value()));
    const IR::Value converted =
        ir.CompositeConstruct(ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(x)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(y)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(z)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(w)), max_value));
    inst.ReplaceUsesWith(converted);
}
} // Anonymous namespace

void TexturePass(Environment& env, IR::Program& program, const HostTranslateInfo& host_info) {
    TextureInstVector to_replace;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsTextureInstruction(inst)) {
                continue;
            }
            to_replace.push_back(MakeInst(env, block, inst));
        }
    }

    // Optimized: single-pass sort by (index, offset)
    std::sort(to_replace.begin(), to_replace.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.cbuf.index != rhs.cbuf.index) {
            return lhs.cbuf.index < rhs.cbuf.index;
        }
        return lhs.cbuf.offset < rhs.cbuf.offset;
    });

    // Create metadata cache to avoid repeated environment queries
    TextureMetadataCache metadata_cache;
    Descriptors descriptors{
        program.info.texture_buffer_descriptors,
        program.info.image_buffer_descriptors,
        program.info.texture_descriptors,
        program.info.image_descriptors,
        program.info.sampler_descriptors,
    };
    for (TextureInst& texture_inst : to_replace) {
        // TODO: Handle arrays
        IR::Inst* const inst{texture_inst.inst};
        inst->ReplaceOpcode(IndexedInstruction(*inst));

        const auto& cbuf{texture_inst.cbuf};
        auto flags{inst->Flags<IR::TextureInstInfo>()};
        bool is_multisample{false};
        switch (inst->GetOpcode()) {
        case IR::Opcode::ImageQueryDimensions:
            flags.type.Assign(ReadTextureType(metadata_cache, env, cbuf));
            inst->SetFlags(flags);
            break;
        case IR::Opcode::ImageSampleImplicitLod:
            if (flags.type != TextureType::Color2D) {
                break;
            }
            if (ReadTextureType(metadata_cache, env, cbuf) == TextureType::Color2DRect) {
                PatchImageSampleImplicitLod(*texture_inst.block, *texture_inst.inst);
            }
            break;
        case IR::Opcode::ImageFetch:
            if (flags.type == TextureType::Color2D || flags.type == TextureType::Color2DRect ||
                flags.type == TextureType::ColorArray2D) {
                is_multisample = !inst->Arg(4).IsEmpty();
            } else {
                inst->SetArg(4, IR::U32{});
            }
            if (flags.type != TextureType::Color1D) {
                break;
            }
            if (ReadTextureType(metadata_cache, env, cbuf) == TextureType::Buffer) {
                // Replace with the bound texture type only when it's a texture buffer
                // If the instruction is 1D and the bound type is 2D, don't change the code and let
                // the rasterizer robustness handle it
                // This happens on Fire Emblem: Three Houses
                flags.type.Assign(TextureType::Buffer);
            }
            break;
        default:
            break;
        }
        u32 index;
        switch (inst->GetOpcode()) {
        case IR::Opcode::ImageRead:
        case IR::Opcode::ImageAtomicIAdd32:
        case IR::Opcode::ImageAtomicSMin32:
        case IR::Opcode::ImageAtomicUMin32:
        case IR::Opcode::ImageAtomicSMax32:
        case IR::Opcode::ImageAtomicUMax32:
        case IR::Opcode::ImageAtomicInc32:
        case IR::Opcode::ImageAtomicDec32:
        case IR::Opcode::ImageAtomicAnd32:
        case IR::Opcode::ImageAtomicOr32:
        case IR::Opcode::ImageAtomicXor32:
        case IR::Opcode::ImageAtomicExchange32:
        case IR::Opcode::ImageWrite: {
            if (cbuf.has_secondary) {
                throw NotImplementedException("Unexpected separate sampler");
            }
            const bool is_written{inst->GetOpcode() != IR::Opcode::ImageRead};
            const bool is_read{inst->GetOpcode() != IR::Opcode::ImageWrite};
            const bool is_integer{IsTexturePixelFormatInteger(metadata_cache, env, cbuf)};
            if (flags.type == TextureType::Buffer) {
                index = descriptors.Add(ImageBufferDescriptor{
                    .format = flags.image_format,
                    .is_written = is_written,
                    .is_read = is_read,
                    .is_integer = is_integer,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .count = cbuf.count,
                    .size_shift = DESCRIPTOR_SIZE_SHIFT,
                });
            } else {
                index = descriptors.Add(ImageDescriptor{
                    .type = flags.type,
                    .format = flags.image_format,
                    .is_written = is_written,
                    .is_read = is_read,
                    .is_integer = is_integer,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .count = cbuf.count,
                    .size_shift = DESCRIPTOR_SIZE_SHIFT,
                });
            }
            break;
        }
        default:
            if (flags.type == TextureType::Buffer) {
                index = descriptors.Add(TextureBufferDescriptor{
                    .has_secondary = cbuf.has_secondary,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .shift_left = cbuf.shift_left,
                    .secondary_cbuf_index = cbuf.secondary_index,
                    .secondary_cbuf_offset = cbuf.secondary_offset,
                    .secondary_shift_left = cbuf.secondary_shift_left,
                    .count = cbuf.count,
                    .size_shift = DESCRIPTOR_SIZE_SHIFT,
                });
            } else {
                // Read sampler info and add to sampler_descriptors
                const auto sampler_info = ReadTextureSamplerInfo(metadata_cache, env, cbuf);

                // Ensure sampler_descriptors vector is large enough
                if (descriptors.GetTextureDescriptorsSize() >= descriptors.GetSamplerDescriptors().size()) {
                    descriptors.GetSamplerDescriptors().resize(descriptors.GetTextureDescriptorsSize() + 1);
                }

                // Store sampler info at the same index as the texture descriptor will be created
                descriptors.GetSamplerDescriptors()[descriptors.GetTextureDescriptorsSize()] = sampler_info;

                index = descriptors.Add(TextureDescriptor{
                    .type = flags.type,
                    .is_depth = flags.is_depth != 0,
                    .is_multisample = is_multisample,
                    .has_secondary = cbuf.has_secondary,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .shift_left = cbuf.shift_left,
                    .secondary_cbuf_index = cbuf.secondary_index,
                    .secondary_cbuf_offset = cbuf.secondary_offset,
                    .secondary_shift_left = cbuf.secondary_shift_left,
                    .count = cbuf.count,
                    .size_shift = DESCRIPTOR_SIZE_SHIFT,
                    .sampler_index = static_cast<u32>(descriptors.GetTextureDescriptorsSize()),
                });
            }
            break;
        }
        flags.descriptor_index.Assign(index);
        inst->SetFlags(flags);

        if (cbuf.count > 1) {
            const auto insert_point{IR::Block::InstructionList::s_iterator_to(*inst)};
            IR::IREmitter ir{*texture_inst.block, insert_point};
            const IR::U32 shift{ir.Imm32(std::countr_zero(DESCRIPTOR_SIZE))};
            inst->SetArg(0, ir.UMin(ir.ShiftRightArithmetic(cbuf.dynamic_offset, shift),
                                    ir.Imm32(DESCRIPTOR_SIZE - 1)));
        } else {
            inst->SetArg(0, IR::Value{});
        }

        if (!host_info.support_snorm_render_buffer && inst->GetOpcode() == IR::Opcode::ImageFetch &&
            flags.type == TextureType::Buffer) {
            const auto pixel_format = ReadTexturePixelFormat(metadata_cache, env, cbuf);
            if (IsPixelFormatSNorm(pixel_format)) {
                PatchTexelFetch(*texture_inst.block, *texture_inst.inst, pixel_format);
            }
        }
    }
}

void JoinTextureInfo(Info& base, Info& source) {
    Descriptors descriptors{
        base.texture_buffer_descriptors,
        base.image_buffer_descriptors,
        base.texture_descriptors,
        base.image_descriptors,
        base.sampler_descriptors,
    };
    for (auto& desc : source.texture_buffer_descriptors) {
        descriptors.Add(desc);
    }
    for (auto& desc : source.image_buffer_descriptors) {
        descriptors.Add(desc);
    }
    for (auto& desc : source.texture_descriptors) {
        descriptors.Add(desc);
    }
    for (auto& desc : source.image_descriptors) {
        descriptors.Add(desc);
    }
}

} // namespace Shader::Optimization
