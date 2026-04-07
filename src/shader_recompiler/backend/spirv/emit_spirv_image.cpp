// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/static_vector.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/frontend/ir/modifiers.h"

namespace Shader::Backend::SPIRV {
namespace {
class ImageOperands {
public:
    [[maybe_unused]] static constexpr bool ImageSampleOffsetAllowed = false;
    [[maybe_unused]] static constexpr bool ImageGatherOffsetAllowed = true;
    [[maybe_unused]] static constexpr bool ImageFetchOffsetAllowed = false;
    [[maybe_unused]] static constexpr bool ImageGradientOffsetAllowed = false;

    explicit ImageOperands(EmitContext& ctx, bool has_bias, bool has_lod, bool has_lod_clamp,
                           Id lod, const IR::Value& offset) {
        if (has_bias) {
            const Id bias{has_lod_clamp ? ctx.OpCompositeExtract(ctx.F32[1], lod, 0) : lod};
            Add(spv::ImageOperandsMask::Bias, bias);
        }
        if (has_lod) {
            const Id lod_value{has_lod_clamp ? ctx.OpCompositeExtract(ctx.F32[1], lod, 0) : lod};
            Add(spv::ImageOperandsMask::Lod, lod_value);
        }
        AddOffset(ctx, offset, ImageSampleOffsetAllowed);
        if (has_lod_clamp) {
            const Id lod_clamp{has_bias ? ctx.OpCompositeExtract(ctx.F32[1], lod, 1) : lod};
            Add(spv::ImageOperandsMask::MinLod, lod_clamp);
        }
    }

    explicit ImageOperands(EmitContext& ctx, const IR::Value& offset, const IR::Value& offset2) {
        if (offset2.IsEmpty()) {
            if (offset.IsEmpty()) {
                return;
            }
            Add(spv::ImageOperandsMask::Offset, ctx.Def(offset));
            //auto id = ctx.OpSatConvertUToS(ctx.S32[1], ctx.Def(offset));
            //Add(spv::ImageOperandsMask::Offset, id);
            return;
        }
        const std::array values{offset.InstRecursive(), offset2.InstRecursive()};
        if (!values[0]->AreAllArgsImmediates() || !values[1]->AreAllArgsImmediates()) {
            LOG_WARNING(Shader_SPIRV, "Not all arguments in PTP are immediate, ignoring");
            return;
        }
        const IR::Opcode opcode{values[0]->GetOpcode()};
        if (opcode != values[1]->GetOpcode() || opcode != IR::Opcode::CompositeConstructU32x4) {
            throw LogicError("Invalid PTP arguments");
        }
        auto read{[&](unsigned int a, unsigned int b) { return values[a]->Arg(b).U32(); }};

        const Id offsets{ctx.ConstantComposite(
            ctx.TypeArray(ctx.U32[2], ctx.Const(4U)), ctx.Const(read(0, 0), read(0, 1)),
            ctx.Const(read(0, 2), read(0, 3)), ctx.Const(read(1, 0), read(1, 1)),
            ctx.Const(read(1, 2), read(1, 3)))};
        Add(spv::ImageOperandsMask::ConstOffsets, offsets);
    }

    explicit ImageOperands(Id lod, Id ms) {
        if (Sirit::ValidId(lod)) {
            Add(spv::ImageOperandsMask::Lod, lod);
        }
        if (Sirit::ValidId(ms)) {
            Add(spv::ImageOperandsMask::Sample, ms);
        }
    }

    explicit ImageOperands(EmitContext& ctx, bool has_lod_clamp, Id derivatives,
                           u32 num_derivatives, const IR::Value& offset, Id lod_clamp) {
        if (!Sirit::ValidId(derivatives)) {
            throw LogicError("Derivatives must be present");
        }
        boost::container::static_vector<Id, 3> deriv_x_accum;
        boost::container::static_vector<Id, 3> deriv_y_accum;
        for (u32 i = 0; i < num_derivatives; ++i) {
            deriv_x_accum.push_back(ctx.OpCompositeExtract(ctx.F32[1], derivatives, i * 2));
            deriv_y_accum.push_back(ctx.OpCompositeExtract(ctx.F32[1], derivatives, i * 2 + 1));
        }
        const Id derivatives_X{ctx.OpCompositeConstruct(
            ctx.F32[num_derivatives], std::span{deriv_x_accum.data(), deriv_x_accum.size()})};
        const Id derivatives_Y{ctx.OpCompositeConstruct(
            ctx.F32[num_derivatives], std::span{deriv_y_accum.data(), deriv_y_accum.size()})};
        Add(spv::ImageOperandsMask::Grad, derivatives_X, derivatives_Y);
        AddOffset(ctx, offset, ImageGradientOffsetAllowed);
        if (has_lod_clamp) {
            Add(spv::ImageOperandsMask::MinLod, lod_clamp);
        }
    }

    explicit ImageOperands(EmitContext& ctx, bool has_lod_clamp, Id derivatives_1, Id derivatives_2,
                           const IR::Value& offset, Id lod_clamp) {
        if (!Sirit::ValidId(derivatives_1) || !Sirit::ValidId(derivatives_2)) {
            throw LogicError("Derivatives must be present");
        }
        boost::container::static_vector<Id, 3> deriv_1_accum{
            ctx.OpCompositeExtract(ctx.F32[1], derivatives_1, 0),
            ctx.OpCompositeExtract(ctx.F32[1], derivatives_1, 2),
            ctx.OpCompositeExtract(ctx.F32[1], derivatives_2, 0),
        };
        boost::container::static_vector<Id, 3> deriv_2_accum{
            ctx.OpCompositeExtract(ctx.F32[1], derivatives_1, 1),
            ctx.OpCompositeExtract(ctx.F32[1], derivatives_1, 3),
            ctx.OpCompositeExtract(ctx.F32[1], derivatives_2, 1),
        };
        const Id derivatives_id1{ctx.OpCompositeConstruct(
            ctx.F32[3], std::span{deriv_1_accum.data(), deriv_1_accum.size()})};
        const Id derivatives_id2{ctx.OpCompositeConstruct(
            ctx.F32[3], std::span{deriv_2_accum.data(), deriv_2_accum.size()})};
        Add(spv::ImageOperandsMask::Grad, derivatives_id1, derivatives_id2);
        AddOffset(ctx, offset, ImageGradientOffsetAllowed);
        if (has_lod_clamp) {
            Add(spv::ImageOperandsMask::MinLod, lod_clamp);
        }
    }

    std::span<const Id> Span() const noexcept {
        return std::span{operands.data(), operands.size()};
    }

    std::optional<spv::ImageOperandsMask> MaskOptional() const noexcept {
        return mask != spv::ImageOperandsMask{} ? std::make_optional(mask) : std::nullopt;
    }

    spv::ImageOperandsMask Mask() const noexcept {
        return mask;
    }

private:
    void AddOffset(EmitContext& ctx, const IR::Value& offset, bool runtime_offset_allowed) {
        if (offset.IsEmpty()) {
            return;
        }
        if (offset.IsImmediate()) {
            Add(spv::ImageOperandsMask::ConstOffset, ctx.SConst(static_cast<s32>(offset.U32())));
            return;
        }
        IR::Inst* const inst{offset.InstRecursive()};
        if (inst->AreAllArgsImmediates()) {
            switch (inst->GetOpcode()) {
            case IR::Opcode::CompositeConstructU32x2:
                Add(spv::ImageOperandsMask::ConstOffset,
                    ctx.SConst(static_cast<s32>(inst->Arg(0).U32()),
                               static_cast<s32>(inst->Arg(1).U32())));
                return;
            case IR::Opcode::CompositeConstructU32x3:
                Add(spv::ImageOperandsMask::ConstOffset,
                    ctx.SConst(static_cast<s32>(inst->Arg(0).U32()),
                               static_cast<s32>(inst->Arg(1).U32()),
                               static_cast<s32>(inst->Arg(2).U32())));
                return;
            case IR::Opcode::CompositeConstructU32x4:
                Add(spv::ImageOperandsMask::ConstOffset,
                    ctx.SConst(static_cast<s32>(inst->Arg(0).U32()),
                               static_cast<s32>(inst->Arg(1).U32()),
                               static_cast<s32>(inst->Arg(2).U32()),
                               static_cast<s32>(inst->Arg(3).U32())));
                return;
            default:
                break;
            }
        }
        if (runtime_offset_allowed) {
            Add(spv::ImageOperandsMask::Offset, ctx.Def(offset));
            //auto id = ctx.OpSatConvertUToS(ctx.S32[1], ctx.Def(offset));
            //Add(spv::ImageOperandsMask::Offset, id);
        }
    }

    void Add(spv::ImageOperandsMask new_mask, Id value) {
        mask = static_cast<spv::ImageOperandsMask>(static_cast<unsigned>(mask) |
                                                   static_cast<unsigned>(new_mask));
        operands.push_back(value);
    }

    void Add(spv::ImageOperandsMask new_mask, Id value_1, Id value_2) {
        mask = static_cast<spv::ImageOperandsMask>(static_cast<unsigned>(mask) |
                                                   static_cast<unsigned>(new_mask));
        operands.push_back(value_1);
        operands.push_back(value_2);
    }

    boost::container::static_vector<Id, 4> operands;
    spv::ImageOperandsMask mask{};
};

Id Texture(EmitContext& ctx, IR::TextureInstInfo info, [[maybe_unused]] const IR::Value& index) {
    using Shader::Backend::SPIRV::TextureOrganizationMode;

    switch (ctx.texture_mode) {
    case TextureOrganizationMode::Pooled: {
        // Pooled mode: Use texture pools with dynamic indexing
        const auto& mapping = ctx.pooled_texture_map.at(info.descriptor_index);
        const auto& pool = ctx.texture_pools.at(mapping.pool_index);
        const auto& sampler_def = ctx.pooled_samplers.at(mapping.sampler_index);

        // Calculate pool index: base_offset + index (if array)
        Id pool_index;
        if (index.IsImmediate()) {
            pool_index = ctx.Const(mapping.pool_offset + index.U32());
        } else {
            const Id base_offset = ctx.Const(mapping.pool_offset);
            pool_index = ctx.OpIAdd(ctx.U32[1], base_offset, ctx.Def(index));
        }

        // Load image from pool
        const Id pointer{ctx.OpAccessChain(pool.pointer_type, pool.id, pool_index)};
        const Id image_id{ctx.OpLoad(pool.image_type, pointer)};

        // Load sampler
        const Id sampler_id{ctx.OpLoad(ctx.TypeSampler(), sampler_def.id)};

        // Combine image and sampler
        const Id sampled_type{ctx.TypeSampledImage(pool.image_type)};
        return ctx.OpSampledImage(sampled_type, image_id, sampler_id);
    }

    case TextureOrganizationMode::Separated: {
        // Separated mode: texture and sampler are separate descriptors
        const TextureDefinition& def{ctx.textures.at(info.descriptor_index)};
        Id image_id;
        if (def.count > 1) {
            const Id pointer{ctx.OpAccessChain(def.pointer_type, def.id, ctx.Def(index))};
            image_id = ctx.OpLoad(def.image_type, pointer);
        } else {
            image_id = ctx.OpLoad(def.image_type, def.id);
        }

        // Load the corresponding sampler
        const SamplerDefinition& sampler_def{ctx.samplers.at(def.sampler_index)};
        const Id sampler_id{ctx.OpLoad(ctx.TypeSampler(), sampler_def.id)};

        // Combine image and sampler to create sampled image
        const Id sampled_type{ctx.TypeSampledImage(def.image_type)};
        return ctx.OpSampledImage(sampled_type, image_id, sampler_id);
    }

    case TextureOrganizationMode::Combined:
    default: {
        // Combined mode: traditional combined image sampler
        const TextureDefinition& def{ctx.textures.at(info.descriptor_index)};
        if (def.count > 1) {
            const Id pointer{ctx.OpAccessChain(def.pointer_type, def.id, ctx.Def(index))};
            return ctx.OpLoad(def.sampled_type, pointer);
        } else {
            return ctx.OpLoad(def.sampled_type, def.id);
        }
    }
    }
}

Id TextureImage(EmitContext& ctx, IR::TextureInstInfo info, const IR::Value& index) {
    using Shader::Backend::SPIRV::TextureOrganizationMode;

    auto array_index = [&]() -> Id {
        return index.IsImmediate() ? ctx.Const(index.U32()) : ctx.Def(index);
    };

    if (info.type == TextureType::Buffer) {
        const TextureBufferDefinition& def{ctx.texture_buffers.at(info.descriptor_index)};
        if (def.count > 1) {
            throw NotImplementedException("Indirect texture sample");
        }
        return ctx.OpLoad(def.image_type, def.id);
    }

    switch (ctx.texture_mode) {
    case TextureOrganizationMode::Pooled: {
        const auto& mapping = ctx.pooled_texture_map.at(info.descriptor_index);
        const auto& pool = ctx.texture_pools.at(mapping.pool_index);

        Id pool_index_id;
        if (index.IsImmediate()) {
            pool_index_id = ctx.Const(mapping.pool_offset + index.U32());
        } else {
            const Id base_offset = ctx.Const(mapping.pool_offset);
            pool_index_id = ctx.OpIAdd(ctx.U32[1], base_offset, ctx.Def(index));
        }

        const Id pointer{ctx.OpAccessChain(pool.pointer_type, pool.id, pool_index_id)};
        return ctx.OpLoad(pool.image_type, pointer);
    }

    case TextureOrganizationMode::Separated: {
        const TextureDefinition& def{ctx.textures.at(info.descriptor_index)};
        if (def.count > 1) {
            const Id pointer{ctx.OpAccessChain(def.pointer_type, def.id, array_index())};
            return ctx.OpLoad(def.image_type, pointer);
        }
        return ctx.OpLoad(def.image_type, def.id);
    }

    case TextureOrganizationMode::Combined:
    default: {
        const TextureDefinition& def{ctx.textures.at(info.descriptor_index)};
#ifdef __APPLE__
        if (def.count > 1) {
            const Id pointer{ctx.OpAccessChain(def.pointer_type, def.id, array_index())};
            return ctx.OpLoad(def.image_type, pointer);
        }
        return ctx.OpLoad(def.image_type, def.id);
#else
        Id sampled_image;
        if (def.count > 1) {
            const Id pointer{ctx.OpAccessChain(def.pointer_type, def.id, array_index())};
            sampled_image = ctx.OpLoad(def.sampled_type, pointer);
        } else {
            sampled_image = ctx.OpLoad(def.sampled_type, def.id);
        }
        return ctx.OpImage(def.image_type, sampled_image);
#endif
    }
    }
}

Id SampledImageToImage(EmitContext& ctx, IR::TextureInstInfo info, Id sampled_image) {
    using Shader::Backend::SPIRV::TextureOrganizationMode;

    if (info.type == TextureType::Buffer) {
        return sampled_image;
    }

    switch (ctx.texture_mode) {
    case TextureOrganizationMode::Pooled: {
        const auto& mapping = ctx.pooled_texture_map.at(info.descriptor_index);
        const auto& pool = ctx.texture_pools.at(mapping.pool_index);
        return ctx.OpImage(pool.image_type, sampled_image);
    }
    case TextureOrganizationMode::Separated:
    case TextureOrganizationMode::Combined:
    default: {
        const TextureDefinition& def{ctx.textures.at(info.descriptor_index)};
        return ctx.OpImage(def.image_type, sampled_image);
    }
    }
}

std::pair<Id, bool> Image(EmitContext& ctx, const IR::Value& index, IR::TextureInstInfo info) {
    auto array_index = [&]() -> Id {
        return index.IsImmediate() ? ctx.Const(index.U32()) : ctx.Def(index);
    };

    if (info.type == TextureType::Buffer) {
        const ImageBufferDefinition def{ctx.image_buffers.at(info.descriptor_index)};
        if (def.count > 1 && !index.IsImmediate()) {
            throw NotImplementedException("Indirect image buffer indexing");
        }
        return {ctx.OpLoad(def.image_type, def.id), def.is_integer};
    }

    const ImageDefinition def{ctx.images.at(info.descriptor_index)};
    if (def.count > 1) {
        const Id pointer{ctx.OpAccessChain(def.pointer_type, def.id, array_index())};
        return {ctx.OpLoad(def.image_type, pointer), def.is_integer};
    }
    return {ctx.OpLoad(def.image_type, def.id), def.is_integer};
}

bool IsTextureMsaa(EmitContext& ctx, const IR::TextureInstInfo& info) {
    using Shader::Backend::SPIRV::TextureOrganizationMode;

    if (info.type == TextureType::Buffer) {
        return false;
    }

    // In Pooled mode, check the texture info from the pooled texture mapping
    if (ctx.texture_mode == TextureOrganizationMode::Pooled) {
        const auto it = ctx.pooled_texture_map.find(info.descriptor_index);
        if (it != ctx.pooled_texture_map.end()) {
            return it->second.is_multisample;
        }
        return false;
    }

    // For Combined and Separated modes, check the texture definition
    return ctx.textures.at(info.descriptor_index).is_multisample;
}

Id Decorate(EmitContext& ctx, IR::Inst* inst, Id sample) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    if (info.relaxed_precision != 0) {
        ctx.Decorate(sample, spv::Decoration::RelaxedPrecision);
    }
    return sample;
}

template <typename MethodPtrType, typename... Args>
Id Emit(MethodPtrType sparse_ptr, MethodPtrType non_sparse_ptr, EmitContext& ctx, IR::Inst* inst,
        Id result_type, Args&&... args) {
    IR::Inst* const sparse{inst->GetAssociatedPseudoOperation(IR::Opcode::GetSparseFromOp)};
    if (!sparse) {
        return Decorate(ctx, inst, (ctx.*non_sparse_ptr)(result_type, std::forward<Args>(args)...));
    }
    const Id struct_type{ctx.TypeStruct(ctx.U32[1], result_type)};
    const Id sample{(ctx.*sparse_ptr)(struct_type, std::forward<Args>(args)...)};
    const Id resident_code{ctx.OpCompositeExtract(ctx.U32[1], sample, 0U)};
    sparse->SetDefinition(ctx.OpImageSparseTexelsResident(ctx.U1, resident_code));
    sparse->Invalidate();
    Decorate(ctx, inst, sample);
    return ctx.OpCompositeExtract(result_type, sample, 1U);
}

Id IsScaled(EmitContext& ctx, const IR::Value& index, Id member_index, u32 base_index) {
    const Id push_constant_u32{ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[1])};
    Id bit{};
    if (index.IsImmediate()) {
        // Use BitwiseAnd instead of BitfieldExtract for better codegen on Nvidia OpenGL.
        // LOP32I.NZ is used to set the predicate rather than BFE+ISETP.
        const u32 index_value{index.U32() + base_index};
        const Id word_index{ctx.Const(index_value / 32)};
        const Id bit_index_mask{ctx.Const(1u << (index_value % 32))};
        const Id pointer{ctx.OpAccessChain(push_constant_u32, ctx.rescaling_push_constants,
                                           member_index, word_index)};
        const Id word{ctx.OpLoad(ctx.U32[1], pointer)};
        bit = ctx.OpBitwiseAnd(ctx.U32[1], word, bit_index_mask);
    } else {
        Id index_value{ctx.Def(index)};
        if (base_index != 0) {
            index_value = ctx.OpIAdd(ctx.U32[1], index_value, ctx.Const(base_index));
        }
        const Id bit_index{ctx.OpBitwiseAnd(ctx.U32[1], index_value, ctx.Const(31u))};
        bit = ctx.OpBitFieldUExtract(ctx.U32[1], index_value, bit_index, ctx.Const(1u));
    }
    return ctx.OpINotEqual(ctx.U1, bit, ctx.u32_zero_value);
}

Id BitTest(EmitContext& ctx, Id mask, Id bit) {
    const Id shifted{ctx.OpShiftRightLogical(ctx.U32[1], mask, bit)};
    const Id bit_value{ctx.OpBitwiseAnd(ctx.U32[1], shifted, ctx.Const(1u))};
    return ctx.OpINotEqual(ctx.U1, bit_value, ctx.u32_zero_value);
}

Id ImageGatherSubpixelOffset(EmitContext& ctx, const IR::TextureInstInfo& info, Id texture,
                             Id coords) {
    // Apply a subpixel offset of 1/512 the texel size of the texture to ensure same rounding on
    // AMD hardware as on Maxwell or other Nvidia architectures.
    const auto calculate_coords{[&](size_t dim) {
        const Id nudge{ctx.Const(0x1p-9f)};
        const Id image_size{ctx.OpImageQuerySizeLod(ctx.U32[dim], texture, ctx.u32_zero_value)};
        Id offset{dim == 2 ? ctx.ConstantComposite(ctx.F32[dim], nudge, nudge)
                           : ctx.ConstantComposite(ctx.F32[dim], nudge, nudge, ctx.f32_zero_value)};
        offset = ctx.OpFDiv(ctx.F32[dim], offset, ctx.OpConvertUToF(ctx.F32[dim], image_size));
        return ctx.OpFAdd(ctx.F32[dim], coords, offset);
    }};
    switch (info.type) {
    case TextureType::Color2D:
    case TextureType::Color2DRect:
        return calculate_coords(2);
    case TextureType::ColorArray2D:
    case TextureType::ColorCube:
        return calculate_coords(3);
    default:
        return coords;
    }
}

void AddOffsetToCoordinates(EmitContext& ctx, const IR::TextureInstInfo& info, Id& coords,
                            Id offset) {
    if (!Sirit::ValidId(offset)) {
        return;
    }

    Id result_type{};
    switch (info.type) {
    case TextureType::Buffer:
    case TextureType::Color1D: {
        result_type = ctx.U32[1];
        break;
    }
    case TextureType::ColorArray1D:
        offset = ctx.OpCompositeConstruct(ctx.U32[2], offset, ctx.u32_zero_value);
        [[fallthrough]];
    case TextureType::Color2D:
    case TextureType::Color2DRect: {
        result_type = ctx.U32[2];
        break;
    }
    case TextureType::ColorArray2D:
        offset = ctx.OpCompositeConstruct(ctx.U32[3], ctx.OpCompositeExtract(ctx.U32[1], offset, 0),
                                          ctx.OpCompositeExtract(ctx.U32[1], offset, 1),
                                          ctx.u32_zero_value);
        [[fallthrough]];
    case TextureType::Color3D: {
        result_type = ctx.U32[3];
        break;
    }
    case TextureType::ColorCube:
    case TextureType::ColorArrayCube:
        return;
    }
    coords = ctx.OpIAdd(result_type, coords, offset);
}
} // Anonymous namespace

Id EmitBindlessImageSampleImplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBindlessImageSampleExplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBindlessImageSampleDrefImplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBindlessImageSampleDrefExplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBindlessImageGather(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBindlessImageGatherDref(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBindlessImageFetch(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBindlessImageQueryDimensions(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBindlessImageQueryLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBindlessImageGradient(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBindlessImageRead(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBindlessImageWrite(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageSampleImplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageSampleExplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageSampleDrefImplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageSampleDrefExplicitLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageGather(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageGatherDref(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageFetch(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageQueryDimensions(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageQueryLod(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageGradient(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageRead(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitBoundImageWrite(EmitContext&) {
    throw LogicError("Unreachable instruction");
}

Id EmitImageSampleImplicitLod(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                              Id bias_lc, const IR::Value& offset) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    if (ctx.stage == Stage::Fragment) {
        const ImageOperands operands(ctx, info.has_bias != 0, false, info.has_lod_clamp != 0,
                                     bias_lc, offset);
        return Emit(&EmitContext::OpImageSparseSampleImplicitLod,
                    &EmitContext::OpImageSampleImplicitLod, ctx, inst, ctx.F32[4],
                    Texture(ctx, info, index), coords, operands.MaskOptional(), operands.Span());
    } else {
        // We can't use implicit lods on non-fragment stages on SPIR-V. Maxwell hardware behaves as
        // if the lod was explicitly zero.  This may change on Turing with implicit compute
        // derivatives
        const Id lod{ctx.Const(0.0f)};
        const ImageOperands operands(ctx, false, true, info.has_lod_clamp != 0, lod, offset);
        return Emit(&EmitContext::OpImageSparseSampleExplicitLod,
                    &EmitContext::OpImageSampleExplicitLod, ctx, inst, ctx.F32[4],
                    Texture(ctx, info, index), coords, operands.Mask(), operands.Span());
    }
}

Id EmitImageSampleExplicitLod(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                              Id lod, const IR::Value& offset) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    const ImageOperands operands(ctx, false, true, false, lod, offset);
    return Emit(&EmitContext::OpImageSparseSampleExplicitLod,
                &EmitContext::OpImageSampleExplicitLod, ctx, inst, ctx.F32[4],
                Texture(ctx, info, index), coords, operands.Mask(), operands.Span());
}

Id EmitImageSampleDrefImplicitLod(EmitContext& ctx, IR::Inst* inst, const IR::Value& index,
                                  Id coords, Id dref, Id bias_lc, const IR::Value& offset) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    if (ctx.stage == Stage::Fragment) {
        const ImageOperands operands(ctx, info.has_bias != 0, false, info.has_lod_clamp != 0,
                                     bias_lc, offset);
        return Emit(&EmitContext::OpImageSparseSampleDrefImplicitLod,
                    &EmitContext::OpImageSampleDrefImplicitLod, ctx, inst, ctx.F32[1],
                    Texture(ctx, info, index), coords, dref, operands.MaskOptional(),
                    operands.Span());
    } else {
        // Implicit lods in compute behave on hardware as if sampling from LOD 0.
        // This check is to ensure all drivers behave this way.
        const Id lod{ctx.Const(0.0f)};
        const ImageOperands operands(ctx, false, true, false, lod, offset);
        return Emit(&EmitContext::OpImageSparseSampleDrefExplicitLod,
                    &EmitContext::OpImageSampleDrefExplicitLod, ctx, inst, ctx.F32[1],
                    Texture(ctx, info, index), coords, dref, operands.Mask(), operands.Span());
    }
}

Id EmitImageSampleDrefExplicitLod(EmitContext& ctx, IR::Inst* inst, const IR::Value& index,
                                  Id coords, Id dref, Id lod, const IR::Value& offset) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    const ImageOperands operands(ctx, false, true, false, lod, offset);
    return Emit(&EmitContext::OpImageSparseSampleDrefExplicitLod,
                &EmitContext::OpImageSampleDrefExplicitLod, ctx, inst, ctx.F32[1],
                Texture(ctx, info, index), coords, dref, operands.Mask(), operands.Span());
}

Id EmitImageGather(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                   const IR::Value& offset, const IR::Value& offset2) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    const ImageOperands operands(ctx, offset, offset2);
    const Id sampled_texture{Texture(ctx, info, index)};
    if (ctx.profile.need_gather_subpixel_offset) {
        const Id image_view{SampledImageToImage(ctx, info, sampled_texture)};
        coords = ImageGatherSubpixelOffset(ctx, info, image_view, coords);
    }
    return Emit(&EmitContext::OpImageSparseGather, &EmitContext::OpImageGather, ctx, inst,
                ctx.F32[4], sampled_texture, coords, ctx.Const(info.gather_component),
                operands.MaskOptional(), operands.Span());
}

Id EmitImageGatherDref(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                       const IR::Value& offset, const IR::Value& offset2, Id dref) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    const ImageOperands operands(ctx, offset, offset2);
    const Id sampled_texture{Texture(ctx, info, index)};
    if (ctx.profile.need_gather_subpixel_offset) {
        const Id image_view{SampledImageToImage(ctx, info, sampled_texture)};
        coords = ImageGatherSubpixelOffset(ctx, info, image_view, coords);
    }
    return Emit(&EmitContext::OpImageSparseDrefGather, &EmitContext::OpImageDrefGather, ctx, inst,
                ctx.F32[4], sampled_texture, coords, dref, operands.MaskOptional(),
                operands.Span());
}

Id EmitImageFetch(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords, Id offset,
                  Id lod, Id ms) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    AddOffsetToCoordinates(ctx, info, coords, offset);
    if (info.type == TextureType::Buffer) {
        lod = Id{};
    }
    if (Sirit::ValidId(ms)) {
        // This image is multisampled, lod must be implicit
        lod = Id{};
    }

    // Determine the correct result type based on texture format
    Id result_type = ctx.F32[4];
    bool needs_bitcast = false;
    if (info.type == TextureType::Buffer) {
        const TextureBufferDefinition& def{ctx.texture_buffers.at(info.descriptor_index)};
        if (def.is_integer) {
            result_type = def.is_signed ? ctx.S32[4] : ctx.U32[4];
            needs_bitcast = true;
        }
    }

    const ImageOperands operands(lod, ms);
    Id result = Emit(&EmitContext::OpImageSparseFetch, &EmitContext::OpImageFetch, ctx, inst, result_type,
                     TextureImage(ctx, info, index), coords, operands.MaskOptional(), operands.Span());

    // Convert integer texture result to float to match IR expectations
    if (needs_bitcast) {
        result = ctx.OpBitcast(ctx.F32[4], result);
    }

    return result;
}

Id EmitImageQueryDimensions(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id lod,
                            const IR::Value& skip_mips_val) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    const Id image{TextureImage(ctx, info, index)};
    const Id zero{ctx.u32_zero_value};
    const bool skip_mips{skip_mips_val.U1()};
    const auto mips{[&] { return skip_mips ? zero : ctx.OpImageQueryLevels(ctx.U32[1], image); }};
    const bool is_msaa{IsTextureMsaa(ctx, info)};
    const bool uses_lod{!is_msaa && info.type != TextureType::Buffer};
    const auto query{[&](Id type) {
        return uses_lod ? ctx.OpImageQuerySizeLod(type, image, lod)
                        : ctx.OpImageQuerySize(type, image);
    }};
    switch (info.type) {
    case TextureType::Color1D:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[1]), zero, zero, mips());
    case TextureType::ColorArray1D:
    case TextureType::Color2D:
    case TextureType::ColorCube:
    case TextureType::Color2DRect:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[2]), zero, mips());
    case TextureType::ColorArray2D:
    case TextureType::Color3D:
    case TextureType::ColorArrayCube:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[3]), mips());
    case TextureType::Buffer:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[1]), zero, zero, mips());
    }
    throw LogicError("Unspecified image type {}", info.type.Value());
}

Id EmitImageQueryLod(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    const Id zero{ctx.f32_zero_value};
    const Id sampler{Texture(ctx, info, index)};
    return ctx.OpCompositeConstruct(ctx.F32[4], ctx.OpImageQueryLod(ctx.F32[2], sampler, coords),
                                    zero, zero);
}

Id EmitImageGradient(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords,
                     Id derivatives, const IR::Value& offset, Id lod_clamp) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    const bool has_lod_clamp{info.has_lod_clamp != 0};

    const IR::Value& derivatives_val = inst->Arg(2);
    IR::Type derivatives_type = derivatives_val.Type();
    if (derivatives_type == IR::Type::Opaque) {
        derivatives_type = derivatives_val.InstRecursive()->Type();
    }
    if (derivatives_type == IR::Type::U32x2) {
        derivatives = ctx.OpBitcast(ctx.F32[2], derivatives);
    } else if (derivatives_type == IR::Type::U32x4) {
        derivatives = ctx.OpBitcast(ctx.F32[4], derivatives);
    }

    if (info.num_derivatives == 3) {
        Id derivatives_2{ctx.Def(offset)};
        IR::Type offset_type{offset.Type()};
        if (offset_type == IR::Type::Opaque) {
            offset_type = offset.InstRecursive()->Type();
        }
        if (offset_type == IR::Type::U32x2) {
            derivatives_2 = ctx.OpBitcast(ctx.F32[2], derivatives_2);
        }
        const ImageOperands operands(ctx, has_lod_clamp, derivatives, derivatives_2, {}, lod_clamp);
        return Emit(&EmitContext::OpImageSparseSampleExplicitLod,
                    &EmitContext::OpImageSampleExplicitLod, ctx, inst, ctx.F32[4],
                    Texture(ctx, info, index), coords, operands.Mask(), operands.Span());
    }
    const auto operands = ImageOperands(ctx, has_lod_clamp, derivatives, info.num_derivatives,
                                        offset, lod_clamp);
    return Emit(&EmitContext::OpImageSparseSampleExplicitLod,
                &EmitContext::OpImageSampleExplicitLod, ctx, inst, ctx.F32[4],
                Texture(ctx, info, index), coords, operands.Mask(), operands.Span());
}

Id EmitImageRead(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    if (info.image_format == ImageFormat::Typeless && !ctx.profile.support_typeless_image_loads) {
        LOG_WARNING(Shader_SPIRV, "Typeless image read not supported by host");
        return ctx.ConstantNull(ctx.U32[4]);
    }
    const auto [image, is_integer] = Image(ctx, index, info);
    const Id result_type{is_integer ? ctx.U32[4] : ctx.F32[4]};
    Id color{Emit(&EmitContext::OpImageSparseRead, &EmitContext::OpImageRead, ctx, inst,
                  result_type, image, coords, std::nullopt, std::span<const Id>{})};
    if (!is_integer) {
        color = ctx.OpBitcast(ctx.U32[4], color);
    }
    return color;
}

void EmitImageWrite(EmitContext& ctx, IR::Inst* inst, const IR::Value& index, Id coords, Id color) {
    const auto info{inst->Flags<IR::TextureInstInfo>()};
    const auto [image, is_integer] = Image(ctx, index, info);
    if (!is_integer) {
        color = ctx.OpBitcast(ctx.F32[4], color);
    }
    ctx.OpImageWrite(image, coords, color);
}

Id EmitIsTextureScaled(EmitContext& ctx, const IR::Value& index) {
    if (ctx.profile.unified_descriptor_binding) {
        const Id member_index{ctx.Const(ctx.rescaling_textures_member_index)};
        return IsScaled(ctx, index, member_index, ctx.texture_rescaling_index);
    } else {
        const Id composite{ctx.OpLoad(ctx.F32[4], ctx.rescaling_uniform_constant)};
        const Id mask_f32{ctx.OpCompositeExtract(ctx.F32[1], composite, 0u)};
        const Id mask{ctx.OpBitcast(ctx.U32[1], mask_f32)};
        return BitTest(ctx, mask, ctx.Def(index));
    }
}

Id EmitIsImageScaled(EmitContext& ctx, const IR::Value& index) {
    if (ctx.profile.unified_descriptor_binding) {
        const Id member_index{ctx.Const(ctx.rescaling_images_member_index)};
        return IsScaled(ctx, index, member_index, ctx.image_rescaling_index);
    } else {
        const Id composite{ctx.OpLoad(ctx.F32[4], ctx.rescaling_uniform_constant)};
        const Id mask_f32{ctx.OpCompositeExtract(ctx.F32[1], composite, 1u)};
        const Id mask{ctx.OpBitcast(ctx.U32[1], mask_f32)};
        return BitTest(ctx, mask, ctx.Def(index));
    }
}

} // namespace Shader::Backend::SPIRV
