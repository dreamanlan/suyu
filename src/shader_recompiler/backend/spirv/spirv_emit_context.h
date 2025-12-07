// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <unordered_map>

#include <sirit/sirit.h>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/shader_info.h"

namespace Shader::Backend::SPIRV {

using Sirit::Id;

enum class TextureOrganizationMode {
    Combined,      // Traditional VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
    Separated,     // VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE + VK_DESCRIPTOR_TYPE_SAMPLER
    Pooled,        // Texture pool with descriptor arrays (cross-platform)
};

class VectorTypes {
public:
    void Define(Sirit::Module& sirit_ctx, Id base_type, std::string_view name);

    [[nodiscard]] Id operator[](size_t size) const noexcept {
        return defs[size - 1];
    }

private:
    std::array<Id, 4> defs{};
};

struct TextureDefinition {
    Id id;
    Id sampled_type;
    Id pointer_type;
    Id image_type;
    u32 count;
    bool is_multisample;
    u32 sampler_index{};  // Index into samplers vector (used in Separated and Pooled modes)
};

// Sampler definition (used for both Metal separated samplers and texture pool)
struct SamplerDefinition {
    Id id;  // SPIR-V ID for the sampler
    u32 cbuf_index;
    u32 cbuf_offset;
    u32 secondary_cbuf_index;
    u32 secondary_cbuf_offset;
    bool has_secondary;
};

struct TextureBufferDefinition {
    Id id;
    u32 count;
};

struct ImageBufferDefinition {
    Id id;
    Id image_type;
    u32 count;
    bool is_integer;
};

struct ImageDefinition {
    Id id;
    Id image_type;
    u32 count;
    bool is_integer;
};

struct UniformDefinitions {
    Id U8{};
    Id S8{};
    Id U16{};
    Id S16{};
    Id U32{};
    Id F32{};
    Id U32x2{};
    Id U32x4{};

    constexpr static size_t NumElements(Id UniformDefinitions::*member_ptr) {
        if (member_ptr == &UniformDefinitions::U8) {
            return 1;
        }
        if (member_ptr == &UniformDefinitions::S8) {
            return 1;
        }
        if (member_ptr == &UniformDefinitions::U16) {
            return 1;
        }
        if (member_ptr == &UniformDefinitions::S16) {
            return 1;
        }
        if (member_ptr == &UniformDefinitions::U32) {
            return 1;
        }
        if (member_ptr == &UniformDefinitions::F32) {
            return 1;
        }
        if (member_ptr == &UniformDefinitions::U32x2) {
            return 2;
        }
        if (member_ptr == &UniformDefinitions::U32x4) {
            return 4;
        }
        ASSERT(false);
        return 1;
    }

    constexpr static bool IsFloat(Id UniformDefinitions::*member_ptr) {
        if (member_ptr == &UniformDefinitions::F32) {
            return true;
        }
        return false;
    }
};

struct StorageTypeDefinition {
    Id array{};
    Id element{};
};

struct StorageTypeDefinitions {
    StorageTypeDefinition U8{};
    StorageTypeDefinition S8{};
    StorageTypeDefinition U16{};
    StorageTypeDefinition S16{};
    StorageTypeDefinition U32{};
    StorageTypeDefinition U64{};
    StorageTypeDefinition F32{};
    StorageTypeDefinition U32x2{};
    StorageTypeDefinition U32x4{};
};

struct StorageDefinitions {
    Id U8{};
    Id S8{};
    Id U16{};
    Id S16{};
    Id U32{};
    Id F32{};
    Id U64{};
    Id U32x2{};
    Id U32x4{};
};

enum class InputGenericLoadOp {
    None,
    Bitcast,
    SToF,
    UToF,
};

struct InputGenericInfo {
    Id id;
    Id pointer_type;
    Id component_type;
    InputGenericLoadOp load_op;
};

struct GenericElementInfo {
    Id id{};
    u32 first_element{};
    u32 num_components{};
};

class EmitContext final : public Sirit::Module {
public:
    explicit EmitContext(const Profile& profile, const RuntimeInfo& runtime_info,
                         IR::Program& program, Bindings& binding);
    ~EmitContext();

    [[nodiscard]] Id Def(const IR::Value& value);

    [[nodiscard]] Id BitOffset8(const IR::Value& offset);
    [[nodiscard]] Id BitOffset16(const IR::Value& offset);

    Id Const(u32 value) {
        return Constant(U32[1], value);
    }

    Id Const(u32 element_1, u32 element_2) {
        return ConstantComposite(U32[2], Const(element_1), Const(element_2));
    }

    Id Const(u32 element_1, u32 element_2, u32 element_3) {
        return ConstantComposite(U32[3], Const(element_1), Const(element_2), Const(element_3));
    }

    Id Const(u32 element_1, u32 element_2, u32 element_3, u32 element_4) {
        return ConstantComposite(U32[4], Const(element_1), Const(element_2), Const(element_3),
                                 Const(element_4));
    }

    Id SConst(s32 value) {
        return Constant(S32[1], value);
    }

    Id SConst(s32 element_1, s32 element_2) {
        return ConstantComposite(S32[2], SConst(element_1), SConst(element_2));
    }

    Id SConst(s32 element_1, s32 element_2, s32 element_3) {
        return ConstantComposite(S32[3], SConst(element_1), SConst(element_2), SConst(element_3));
    }

    Id SConst(s32 element_1, s32 element_2, s32 element_3, s32 element_4) {
        return ConstantComposite(S32[4], SConst(element_1), SConst(element_2), SConst(element_3),
                                 SConst(element_4));
    }

    Id Const(f32 value) {
        return Constant(F32[1], value);
    }

    const Profile& profile;
    const RuntimeInfo& runtime_info;
    Stage stage{};

    Id void_id{};
    Id U1{};
    Id U8{};
    Id S8{};
    Id U16{};
    Id S16{};
    Id U64{};
    VectorTypes F32;
    VectorTypes U32;
    VectorTypes S32;
    VectorTypes F16;
    VectorTypes F64;

    Id true_value{};
    Id false_value{};
    Id u32_zero_value{};
    Id f32_zero_value{};

    UniformDefinitions uniform_types;
    StorageTypeDefinitions storage_types;

    Id private_u32{};

    Id shared_u8{};
    Id shared_u16{};
    Id shared_u32{};
    Id shared_u64{};
    Id shared_u32x2{};
    Id shared_u32x4{};

    Id input_f32{};
    Id input_u32{};
    Id input_s32{};

    Id output_f32{};
    Id output_u32{};

    Id image_buffer_type{};
    Id image_u32{};

    std::array<UniformDefinitions, Info::MAX_CBUFS> cbufs{};
    std::array<StorageDefinitions, Info::MAX_SSBOS> ssbos{};
    std::vector<TextureBufferDefinition> texture_buffers;
    std::vector<ImageBufferDefinition> image_buffers;
    std::vector<TextureDefinition> textures;
    std::vector<ImageDefinition> images;

    // Texture organization mode (cross-platform)
    TextureOrganizationMode texture_mode{TextureOrganizationMode::Combined};

    // Texture pool definitions (for Pooled mode)
    struct TexturePoolInfo {
        Id id;                    // SPIR-V variable ID for the texture array
        Id image_type;            // Base image type
        Id pointer_type;          // Pointer type for array elements
        u32 pool_size;            // Total number of textures in pool
        u32 binding;              // Binding point
        TextureType type;         // Texture type (2D, 3D, Cube, etc.)
    };
    std::vector<TexturePoolInfo> texture_pools;  // One pool per texture type
    std::vector<SamplerDefinition> pooled_samplers;  // Samplers for pooled mode
    u32 pooled_sampler_binding_base{};  // Starting binding for pooled samplers

    // Separated samplers (used in Separated and Pooled modes)
    std::vector<SamplerDefinition> samplers;
    u32 sampler_binding_base{};

    // Mapping from descriptor_index to (pool_index, offset_in_pool, sampler_index)
    struct PooledTextureMapping {
        u32 pool_index;      // Index into texture_pools
        u32 pool_offset;     // Offset within the pool
        u32 sampler_index;   // Index into pooled_samplers
        bool is_multisample; // Whether this texture is multisampled
    };
    std::unordered_map<u32, PooledTextureMapping> pooled_texture_map;

    Id workgroup_id{};
    Id local_invocation_id{};
    Id invocation_id{};
    Id patch_vertices_in{};
    Id sample_id{};
    Id is_helper_invocation{};
    Id subgroup_local_invocation_id{};
    Id subgroup_mask_eq{};
    Id subgroup_mask_lt{};
    Id subgroup_mask_le{};
    Id subgroup_mask_gt{};
    Id subgroup_mask_ge{};
    Id instance_id{};
    Id instance_index{};
    Id base_instance{};
    Id vertex_id{};
    Id vertex_index{};
    Id draw_index{};
    Id base_vertex{};
    Id front_face{};
    Id point_coord{};
    Id tess_coord{};
    Id clip_distances{};
    Id layer{};
    Id viewport_index{};
    Id viewport_mask{};
    Id primitive_id{};

    Id fswzadd_lut_a{};
    Id fswzadd_lut_b{};

    Id indexed_load_func{};
    Id indexed_store_func{};

    Id rescaling_uniform_constant{};
    Id rescaling_push_constants{};
    Id rescaling_textures_type{};
    Id rescaling_images_type{};
    u32 rescaling_textures_member_index{};
    u32 rescaling_images_member_index{};
    u32 rescaling_downfactor_member_index{};
    u32 texture_rescaling_index{};
    u32 image_rescaling_index{};

    Id render_area_push_constant{};
    u32 render_are_member_index{};

    Id local_memory{};

    Id shared_memory_u8{};
    Id shared_memory_u16{};
    Id shared_memory_u32{};
    Id shared_memory_u64{};
    Id shared_memory_u32x2{};
    Id shared_memory_u32x4{};

    Id shared_memory_u32_type{};

    Id shared_store_u8_func{};
    Id shared_store_u16_func{};
    Id increment_cas_shared{};
    Id increment_cas_ssbo{};
    Id decrement_cas_shared{};
    Id decrement_cas_ssbo{};
    Id f32_add_cas{};
    Id f16x2_add_cas{};
    Id f16x2_min_cas{};
    Id f16x2_max_cas{};
    Id f32x2_add_cas{};
    Id f32x2_min_cas{};
    Id f32x2_max_cas{};

    Id write_storage_cas_loop_func{};

    Id load_global_func_u32{};
    Id load_global_func_u32x2{};
    Id load_global_func_u32x4{};
    Id write_global_func_u32{};
    Id write_global_func_u32x2{};
    Id write_global_func_u32x4{};

    bool need_input_position_indirect{};
    Id input_position{};
    std::array<InputGenericInfo, 32> input_generics{};

    Id output_point_size{};
    Id output_position{};
    std::array<std::array<GenericElementInfo, 4>, 32> output_generics{};

    Id output_tess_level_outer{};
    Id output_tess_level_inner{};
    std::array<Id, 30> patches{};

    std::array<Id, 8> frag_color{};
    Id sample_mask{};
    Id frag_depth{};

    std::vector<Id> interfaces;

    Id load_const_func_u8{};
    Id load_const_func_u16{};
    Id load_const_func_u32{};
    Id load_const_func_f32{};
    Id load_const_func_u32x2{};
    Id load_const_func_u32x4{};

private:
    void DefineCommonTypes(const Info& info);
    void DefineCommonConstants();
    void DefineInterfaces(const IR::Program& program);
    void DefineLocalMemory(const IR::Program& program);
    void DefineSharedMemory(const IR::Program& program);
    void DefineSharedMemoryFunctions(const IR::Program& program);
    void DefineConstantBuffers(const Info& info, u32& binding);
    void DefineConstantBufferIndirectFunctions(const Info& info);
    void DefineStorageBuffers(const Info& info, u32& binding);
    void DefineTextureBuffers(const Info& info, u32& binding);
    void DefineImageBuffers(const Info& info, u32& binding);
    void DefineTextures(const Info& info, u32& binding, u32& scaling_index);
    void DefineTexturesCombined(const Info& info, u32& binding, u32& scaling_index);
    void DefineTexturesSeparated(const Info& info, u32& binding, u32& scaling_index);
    void DefineTexturesPooled(const Info& info, u32& binding, u32& scaling_index);
    void DefineImages(const Info& info, u32& binding, u32& scaling_index);
    void DefineAttributeMemAccess(const Info& info);
    void DefineWriteStorageCasLoopFunction(const Info& info);
    void DefineGlobalMemoryFunctions(const Info& info);
    void DefineRescalingInput(const Info& info);
    void DefineRescalingInputPushConstant();
    void DefineRescalingInputUniformConstant();
    void DefineRenderArea(const Info& info);

    void DefineInputs(const IR::Program& program);
    void DefineOutputs(const IR::Program& program);
};

} // namespace Shader::Backend::SPIRV
