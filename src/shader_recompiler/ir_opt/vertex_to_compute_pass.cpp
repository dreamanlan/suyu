// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Vertex-to-Compute IR transformation pass.
// Converts a Vertex Shader IR program into a Compute Shader that:
//   - Reads vertex attributes from Buffer Textures (instead of VS input attributes)
//   - Writes output vertex data to a VS output SSBO
//   - Replaces VertexId/InstanceId with computed values from VertexInfo CB
//
// This is modeled after Ryujinx's VertexToCompute.cs transform.

#include <algorithm>
#include <unordered_map>

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/vtg_as_compute.h"

namespace Shader::Optimization {
namespace {

// Workgroup size used for VS compute dispatch.
constexpr u32 VsWorkGroupSize = 64;

// Compute GlobalInvocationId for a given component.
// GlobalId = WorkgroupId * WorkGroupSize + LocalInvocationId
IR::U32 GenerateGlobalInvocationId(IR::IREmitter& ir, u32 component, u32 workgroup_size) {
    IR::U32 workgroup_id;
    IR::U32 local_id;
    switch (component) {
    case 0:
        workgroup_id = ir.WorkgroupIdX();
        local_id = ir.LocalInvocationIdX();
        break;
    case 1:
        workgroup_id = ir.WorkgroupIdY();
        local_id = ir.LocalInvocationIdY();
        break;
    case 2:
    default:
        workgroup_id = ir.WorkgroupIdZ();
        local_id = ir.LocalInvocationIdZ();
        break;
    }
    return ir.IAdd(ir.IMul(workgroup_id, ir.Imm32(workgroup_size)), local_id);
}

// Generate the vertex ID for vertex-rate attributes.
// Reads from LocalMemory slot populated by the prologue (after index buffer lookup).
IR::U32 GenerateVertexIdVertexRate(IR::IREmitter& ir,
                                    const VtgAsCompute::ResourceReservations& res) {
    return ir.LoadLocal(ir.Imm32(res.local_slots.vertex_index_vr));
}

// Generate the vertex ID for instance-rate attributes.
// Reads from LocalMemory slot populated by the prologue.
IR::U32 GenerateVertexIdInstanceRate(IR::IREmitter& ir,
                                      const VtgAsCompute::ResourceReservations& res) {
    return ir.LoadLocal(ir.Imm32(res.local_slots.vertex_index_ir));
}

// Generate the vertex buffer element offset for a given attribute location and component.
// offset = (isInstanceRate ? vertexIdIR : vertexIdVR) * stride + attributeOffset + component
IR::U32 GenerateVertexOffset(IR::IREmitter& ir,
                              const VtgAsCompute::ResourceReservations& res,
                              u32 location, u32 component) {
    const u32 cb_binding = res.vertex_info_cb_binding;

    // Load attribute offset from VertexInfo CB
    const u32 offsets_field = static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexOffsets);
    const IR::U32 attr_offset = ir.GetCbuf(
        ir.Imm32(cb_binding),
        ir.Imm32(offsets_field * 16 + location * 16)); // vec4 per location, .x = offset

    // Load divisor from VertexInfo CB (.y = divisor; 0 = vertex rate, >0 = instance rate)
    const IR::U32 divisor = ir.GetCbuf(
        ir.Imm32(cb_binding),
        ir.Imm32(offsets_field * 16 + location * 16 + 4)); // .y = divisor

    // Select vertex ID based on rate, applying instance divisor when > 1.
    const IR::U32 vertex_id_vr = GenerateVertexIdVertexRate(ir, res);
    const IR::U1 is_inst = ir.INotEqual(divisor, ir.Imm32(0u));

    // For instance rate: vertexId = firstInstance + outputInstanceOffset / divisor
    // Load firstInstance and outputInstanceOffset from local memory.
    const IR::U32 vertex_id_ir_base = GenerateVertexIdInstanceRate(ir, res);
    // Compute: firstInstance + (outputInstanceOffset / divisor)
    // vertex_id_ir_base = firstInstance + outputInstanceOffset (computed in prologue)
    // We need: firstInstance + outputInstanceOffset / divisor
    // Rewrite as: firstInstance + (vertex_id_ir_base - firstInstance) / divisor
    const u32 counts_base = static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexCounts) * 16;
    const IR::U32 first_instance = ir.GetCbuf(
        ir.Imm32(cb_binding),
        ir.Imm32(counts_base + 3 * 4)); // VertexCounts.w = firstInstance
    const IR::U32 instance_offset = ir.ISub(vertex_id_ir_base, first_instance);
    // Use max(divisor, 1) to avoid division by zero when divisor=0 (vertex rate).
    // The result is only used in the instance-rate branch, but the division is
    // unconditionally executed in the shader.
    const IR::U32 safe_divisor = ir.UMax(divisor, ir.Imm32(1u));
    const IR::U32 adjusted_offset = ir.IDiv(instance_offset, safe_divisor);
    const IR::U32 vertex_id_ir = ir.IAdd(first_instance, adjusted_offset);

    const IR::U32 vertex_id = IR::U32{ir.Select(is_inst, vertex_id_ir, vertex_id_vr)};

    // Load stride from VertexInfo CB
    const u32 strides_field = static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexStrides);
    const IR::U32 stride = ir.GetCbuf(
        ir.Imm32(cb_binding),
        ir.Imm32(strides_field * 16 + location * 16)); // vec4 per location, .x = stride

    // vertexBaseOffset = vertexId * stride
    const IR::U32 vertex_base_offset = ir.IMul(vertex_id, stride);

    // vertexOffset = attributeOffset + vertexBaseOffset
    const IR::U32 vertex_offset = ir.IAdd(attr_offset, vertex_base_offset);

    // Add component offset
    if (component != 0) {
        return ir.IAdd(vertex_offset, ir.Imm32(component));
    }
    return vertex_offset;
}

// Decode a packed A2B10G10R10 format component.
// Layout: [31:30]=A(2bit), [29:20]=B(10bit), [19:10]=G(10bit), [9:0]=R(10bit)
// Returns the decoded f32 value for the given component and numericType.
IR::F32 DecodeA2B10G10R10(IR::IREmitter& ir, const IR::U32& raw_u32,
                           u32 component, const IR::U32& num_type) {
    // Extract the raw bits for this component.
    constexpr u32 shifts[] = {0, 10, 20, 30};
    constexpr u32 widths[] = {10, 10, 10, 2};
    const u32 comp_shift = shifts[component];
    const u32 comp_width = widths[component];

    const IR::U32 raw_unsigned = ir.BitFieldExtract(raw_u32, ir.Imm32(comp_shift),
                                                     ir.Imm32(comp_width), false);
    const IR::U32 raw_signed = ir.BitFieldExtract(raw_u32, ir.Imm32(comp_shift),
                                                   ir.Imm32(comp_width), true);

    // Unorm: unsigned / max_unsigned
    const float max_unsigned = static_cast<float>((1u << comp_width) - 1u);
    const IR::F32 unorm_result = ir.FPMul(
        IR::F32{ir.ConvertUToF(32, 32, IR::Value{raw_unsigned})},
        ir.Imm32(1.0f / max_unsigned));

    // Snorm: sign_extend / max_signed, clamped to -1.0
    const float max_signed = static_cast<float>((1u << (comp_width - 1)) - 1u);
    const IR::F32 snorm_raw = ir.FPMul(
        IR::F32{ir.ConvertSToF(32, 32, IR::Value{raw_signed})},
        ir.Imm32(1.0f / max_signed));
    const IR::F32 snorm_result = ir.FPMax(snorm_raw, ir.Imm32(-1.0f));

    // Uint: raw unsigned as float
    const IR::F32 uint_result = IR::F32{ir.ConvertUToF(32, 32, IR::Value{raw_unsigned})};

    // Sint: raw signed as float
    const IR::F32 sint_result = IR::F32{ir.ConvertSToF(32, 32, IR::Value{raw_signed})};

    // Select based on numericType: 1=Uint, 2=Sint, 3=Unorm, 4=Snorm (default=Unorm)
    const IR::U1 is_uint = ir.IEqual(num_type, ir.Imm32(1u));
    const IR::U1 is_sint = ir.IEqual(num_type, ir.Imm32(2u));
    const IR::U1 is_snorm = ir.IEqual(num_type, ir.Imm32(4u));
    const IR::F32 sel_snorm = IR::F32{ir.Select(is_snorm, IR::Value{snorm_result}, IR::Value{unorm_result})};
    const IR::F32 sel_sint = IR::F32{ir.Select(is_sint, IR::Value{sint_result}, IR::Value{sel_snorm})};
    return IR::F32{ir.Select(is_uint, IR::Value{uint_result}, IR::Value{sel_sint})};
}

// Convert an unsigned N-bit mini-float (no sign bit) to f32 using bit manipulation.
// Handles normal values; denorms are flushed to zero, inf/nan are preserved.
// exp_bits: number of exponent bits (5 for both 10-bit and 11-bit floats)
// man_bits: number of mantissa bits (5 for 10-bit, 6 for 11-bit)
IR::F32 MiniFloatToF32(IR::IREmitter& ir, const IR::U32& raw, u32 exp_bits, u32 man_bits) {
    const u32 exp_mask = (1u << exp_bits) - 1u;
    const u32 man_mask = (1u << man_bits) - 1u;
    // f32 exponent bias adjustment: f32_bias(127) - mini_bias(15) = 112
    constexpr u32 bias_adjust = 112u;

    const IR::U32 mantissa = ir.BitwiseAnd(raw, ir.Imm32(man_mask));
    const IR::U32 exponent = ir.BitwiseAnd(
        ir.ShiftRightLogical(raw, ir.Imm32(man_bits)), ir.Imm32(exp_mask));

    // Normal case: f32_bits = ((exponent + 112) << 23) | (mantissa << (23 - man_bits))
    const IR::U32 f32_exp = ir.ShiftLeftLogical(
        ir.IAdd(exponent, ir.Imm32(bias_adjust)), ir.Imm32(23u));
    const IR::U32 f32_man = ir.ShiftLeftLogical(mantissa, ir.Imm32(23u - man_bits));
    const IR::U32 f32_normal = ir.BitwiseOr(f32_exp, f32_man);

    // Zero case: exponent == 0 && mantissa == 0 -> 0.0
    // Denorm case: exponent == 0 && mantissa != 0 -> flush to 0.0 (simplified)
    const IR::U1 is_zero_exp = ir.IEqual(exponent, ir.Imm32(0u));
    const IR::U32 f32_bits = IR::U32{ir.Select(is_zero_exp, ir.Imm32(0u), f32_normal)};

    return ir.BitCast<IR::F32>(f32_bits);
}

// Decode a packed B10G11R11 Float format component.
// Layout: [31:22]=B(10bit float, 5e5m), [21:11]=G(11bit float, 5e6m), [10:0]=R(11bit float, 5e6m)
IR::F32 DecodeB10G11R11Float(IR::IREmitter& ir, const IR::U32& raw_u32, u32 component) {
    constexpr u32 shifts[] = {0, 11, 22};
    constexpr u32 widths[] = {11, 11, 10};
    constexpr u32 man_bits[] = {6, 6, 5};

    const IR::U32 raw_comp = ir.BitFieldExtract(raw_u32, ir.Imm32(shifts[component]),
                                                 ir.Imm32(widths[component]), false);
    return MiniFloatToF32(ir, raw_comp, 5, man_bits[component]);
}

// Replace GetAttribute(input, UserDefined) with a buffer SSBO load + format decode.
// Handles R8/R16/R32 formats with Float/Uint/Sint/Unorm/Snorm numeric types,
// and packed formats: A2B10G10R10 (Unorm/Snorm/Uint/Sint), B10G11R11 (Float).
// Numeric type encoding: 0=Float, 1=Uint, 2=Sint, 3=Unorm, 4=Snorm, 5=Uscaled, 6=Sscaled
// Packed format encoding: 0=normal, 1=A2B10G10R10, 2=B10G11R11
// vb_ssbo_index_map: maps attribute location -> actual ctx.ssbos[] array index.
void ReplaceVsInputLoad(IR::Block& block, IR::Inst& inst,
                         const VtgAsCompute::ResourceReservations& res,
                         const std::unordered_map<u32, u32>& vb_ssbo_index_map) {
    const IR::Attribute attr = inst.Arg(0).Attribute();

    // Only handle generic (user-defined) attributes
    if (!IR::IsGeneric(attr)) {
        return;
    }

    const u32 location = IR::GenericAttributeIndex(attr);
    const u32 component = IR::GenericAttributeElement(attr);

    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, it};

    // Look up the actual ctx.ssbos[] array index for this vertex buffer location.
    const auto vb_it = vb_ssbo_index_map.find(location);
    if (vb_it == vb_ssbo_index_map.end()) {
        return; // This location has no SSBO descriptor; skip.
    }
    const u32 ssbo_binding = vb_it->second;
    const u32 cb_binding = res.vertex_info_cb_binding;
    const u32 strides_field = static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexStrides);
    const u32 offsets_field = static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexOffsets);

    // Load componentSize from VertexStrides[loc].z (1, 2, or 4 bytes).
    const IR::U32 comp_size = ir.GetCbuf(
        ir.Imm32(cb_binding),
        ir.Imm32(strides_field * 16 + location * 16 + 8)); // .z = componentSize

    // Load numericType from VertexStrides[loc].w.
    const IR::U32 num_type = ir.GetCbuf(
        ir.Imm32(cb_binding),
        ir.Imm32(strides_field * 16 + location * 16 + 12)); // .w = numericType

    // Load packedFormat from VertexOffsets[loc].z (0=normal, 1=A2B10G10R10, 2=B10G11R11).
    const IR::U32 packed_format = ir.GetCbuf(
        ir.Imm32(cb_binding),
        ir.Imm32(offsets_field * 16 + location * 16 + 8)); // .z = packedFormat

    // For packed formats, all components are in the same u32, so use component 0 offset.
    // For normal formats, use the per-component offset.
    const IR::U32 vertex_elem_offset_normal = GenerateVertexOffset(ir, res, location, component);
    const IR::U32 vertex_elem_offset_packed = GenerateVertexOffset(ir, res, location, 0);
    const IR::U1 is_packed = ir.INotEqual(packed_format, ir.Imm32(0u));
    const IR::U32 vertex_elem_offset = IR::U32{ir.Select(is_packed,
        IR::Value{vertex_elem_offset_packed}, IR::Value{vertex_elem_offset_normal})};

    // Step 1: Compute byte offset and load raw u32 from SSBO.
    // byteOffset = vertexElemOffset * componentSize
    const IR::U32 byte_offset = ir.IMul(vertex_elem_offset, comp_size);
    // Align to 4-byte boundary for LoadStorage32.
    const IR::U32 aligned_offset = ir.BitwiseAnd(byte_offset, ir.Imm32(~3u));
    const IR::U32 raw_u32 = IR::U32{&*block.PrependNewInst(it, IR::Opcode::LoadStorage32,
        {IR::Value{ir.Imm32(ssbo_binding)}, IR::Value{aligned_offset}})};

    // === Packed format decode path ===
    // A2B10G10R10: [31:30]=A(2), [29:20]=B(10), [19:10]=G(10), [9:0]=R(10)
    const IR::F32 a2b10_result = DecodeA2B10G10R10(ir, raw_u32, component, num_type);
    // B10G11R11 Float: [31:22]=B(10-bit float), [21:11]=G(11-bit float), [10:0]=R(11-bit float)
    const IR::F32 b10g11_result = (component < 3)
        ? DecodeB10G11R11Float(ir, raw_u32, component)
        : ir.Imm32(1.0f); // B10G11R11 has no alpha; default to 1.0
    const IR::U1 is_a2b10 = ir.IEqual(packed_format, ir.Imm32(1u));
    const IR::F32 packed_result = IR::F32{ir.Select(is_a2b10,
        IR::Value{a2b10_result}, IR::Value{b10g11_result})};

    // === Normal format decode path ===
    // Step 2: Extract the sub-u32 component using bit operations.
    const IR::U32 sub_byte = ir.BitwiseAnd(byte_offset, ir.Imm32(3u));
    const IR::U32 shift = ir.IMul(sub_byte, ir.Imm32(8u));
    const IR::U32 bit_width = ir.IMul(comp_size, ir.Imm32(8u));
    const IR::U32 raw_comp = ir.BitFieldExtract(raw_u32, shift, bit_width, false);

    // Step 3: Decode based on numericType.
    // --- Float path (numericType == 0) ---
    const IR::F32 float_r32 = ir.BitCast<IR::F32>(raw_comp);
    const IR::Value half_vec = ir.UnpackHalf2x16(raw_comp);
    const IR::F32 float_r16 = IR::F32{ir.CompositeExtract(half_vec, 0)};
    const IR::U1 is_cs4 = ir.IEqual(comp_size, ir.Imm32(4u));
    const IR::F32 float_result = IR::F32{ir.Select(is_cs4, IR::Value{float_r32}, IR::Value{float_r16})};

    // --- Unorm path (numericType == 3) ---
    const IR::F32 raw_float_u = IR::F32{ir.ConvertUToF(32, 32, IR::Value{raw_comp})};
    const IR::F32 inv_255 = ir.Imm32(1.0f / 255.0f);
    const IR::F32 inv_65535 = ir.Imm32(1.0f / 65535.0f);
    const IR::U1 is_cs1 = ir.IEqual(comp_size, ir.Imm32(1u));
    const IR::F32 unorm_scale = IR::F32{ir.Select(is_cs1, IR::Value{inv_255}, IR::Value{inv_65535})};
    const IR::F32 unorm_result = ir.FPMul(raw_float_u, unorm_scale);

    // --- Snorm path (numericType == 4) ---
    const IR::U32 raw_signed = ir.BitFieldExtract(raw_u32, shift, bit_width, true);
    const IR::F32 raw_float_s = IR::F32{ir.ConvertSToF(32, 32, IR::Value{raw_signed})};
    const IR::F32 inv_127 = ir.Imm32(1.0f / 127.0f);
    const IR::F32 inv_32767 = ir.Imm32(1.0f / 32767.0f);
    const IR::F32 snorm_scale = IR::F32{ir.Select(is_cs1, IR::Value{inv_127}, IR::Value{inv_32767})};
    const IR::F32 snorm_raw = ir.FPMul(raw_float_s, snorm_scale);
    const IR::F32 snorm_result = ir.FPMax(snorm_raw, ir.Imm32(-1.0f));

    // --- Uint path (numericType == 1) / Uscaled path (numericType == 5) ---
    const IR::F32 uint_result = raw_float_u;

    // --- Sint path (numericType == 2) / Sscaled path (numericType == 6) ---
    const IR::F32 sint_result = raw_float_s;

    // Step 4: Select final result based on numericType using a Select chain.
    const IR::U1 is_float = ir.IEqual(num_type, ir.Imm32(0u));
    const IR::U1 is_uint = ir.IEqual(num_type, ir.Imm32(1u));
    const IR::U1 is_sint = ir.IEqual(num_type, ir.Imm32(2u));
    const IR::U1 is_unorm = ir.IEqual(num_type, ir.Imm32(3u));
    const IR::U1 is_snorm = ir.IEqual(num_type, ir.Imm32(4u));
    const IR::U1 is_uscaled = ir.IEqual(num_type, ir.Imm32(5u));

    const IR::F32 sel_uscaled = IR::F32{ir.Select(is_uscaled, IR::Value{uint_result}, IR::Value{sint_result})};
    const IR::F32 sel_snorm = IR::F32{ir.Select(is_snorm, IR::Value{snorm_result}, IR::Value{sel_uscaled})};
    const IR::F32 sel_unorm = IR::F32{ir.Select(is_unorm, IR::Value{unorm_result}, IR::Value{sel_snorm})};
    const IR::F32 sel_sint = IR::F32{ir.Select(is_sint, IR::Value{sint_result}, IR::Value{sel_unorm})};
    const IR::F32 sel_uint = IR::F32{ir.Select(is_uint, IR::Value{uint_result}, IR::Value{sel_sint})};
    const IR::F32 normal_result = IR::F32{ir.Select(is_float, IR::Value{float_result}, IR::Value{sel_uint})};

    // Step 5: Select between packed and normal decode paths.
    const IR::F32 decoded_result = IR::F32{ir.Select(is_packed,
        IR::Value{packed_result}, IR::Value{normal_result})};

    // Step 6: Apply component mask (CopyMasked).
    // If the component index >= componentCount, return default (0.0 for xyz, 1.0 for w).
    const IR::U32 comp_count = ir.GetCbuf(
        ir.Imm32(cb_binding),
        ir.Imm32(strides_field * 16 + location * 16 + 4)); // .y = componentCount
    const IR::U1 comp_exists = ir.IGreaterThan(comp_count, ir.Imm32(component), false);
    const IR::F32 default_val = ir.Imm32(component == 3 ? 1.0f : 0.0f);
    const IR::F32 masked_result = IR::F32{ir.Select(comp_exists, IR::Value{decoded_result}, IR::Value{default_val})};

    // Replace the original instruction.
    if (inst.GetOpcode() == IR::Opcode::GetAttribute) {
        inst.ReplaceUsesWith(IR::Value{masked_result});
    } else {
        // GetAttributeU32: return the raw unsigned component value.
        // For packed formats, extract the raw bits for this component.
        // A2B10G10R10: shifts={0,10,20,30}, widths={10,10,10,2}
        constexpr u32 a2b10_shifts[] = {0, 10, 20, 30};
        constexpr u32 a2b10_widths[] = {10, 10, 10, 2};
        const IR::U32 a2b10_raw = ir.BitFieldExtract(raw_u32,
            ir.Imm32(a2b10_shifts[component]), ir.Imm32(a2b10_widths[component]), false);
        // B10G11R11: shifts={0,11,22}, widths={11,11,10}
        constexpr u32 b10g11_shifts[] = {0, 11, 22, 0};
        constexpr u32 b10g11_widths[] = {11, 11, 10, 0};
        const IR::U32 b10g11_raw = (component < 3)
            ? ir.BitFieldExtract(raw_u32,
                  ir.Imm32(b10g11_shifts[component]), ir.Imm32(b10g11_widths[component]), false)
            : ir.Imm32(1u); // B10G11R11 has no alpha; default to 1
        const IR::U32 packed_raw = IR::U32{ir.Select(is_a2b10,
            IR::Value{a2b10_raw}, IR::Value{b10g11_raw})};
        const IR::U32 raw_result = IR::U32{ir.Select(is_packed,
            IR::Value{packed_raw}, IR::Value{raw_comp})};
        inst.ReplaceUsesWith(IR::Value{raw_result});
    }
}

// Replace SetAttribute(output) with a write to local memory.
void ReplaceVsOutputStore(IR::Block& block, IR::Inst& inst,
                           const VtgAsCompute::IoOffsetMap& output_map) {
    const IR::Attribute attr = inst.Arg(0).Attribute();
    const IR::Value value = inst.Arg(1);

    u32 element_offset{};
    if (!VtgAsCompute::TryGetIoOffset(output_map, attr, element_offset)) {
        inst.Invalidate();
        return;
    }

    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, it};

    // Convert F32 value to U32 for local memory storage
    const IR::U32 u32_value = ir.BitCast<IR::U32>(IR::F32{value});
    ir.WriteLocal(ir.Imm32(element_offset), u32_value);

    inst.Invalidate();
}

// Replace VertexId/VertexIndex loads with computed values.
void ReplaceVsVertexIdLoad(IR::Block& block, IR::Inst& inst,
                            const VtgAsCompute::ResourceReservations& res) {
    const IR::Attribute attr = inst.Arg(0).Attribute();
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, it};

    switch (attr) {
    case IR::Attribute::VertexId: {
        // VertexId = vertexIndexVR (from LocalMemory, set in prologue via index buffer lookup)
        const IR::U32 vertex_id = GenerateVertexIdVertexRate(ir, res);
        if (inst.GetOpcode() == IR::Opcode::GetAttribute) {
            const IR::Value fval{&*block.PrependNewInst(it, IR::Opcode::BitCastF32U32,
                {IR::Value{vertex_id}})};
            inst.ReplaceUsesWith(fval);
        } else {
            inst.ReplaceUsesWith(IR::Value{vertex_id});
        }
        break;
    }
    case IR::Attribute::InstanceId: {
        // InstanceId = GlobalInvocationId.y
        const IR::U32 instance_id = GenerateGlobalInvocationId(ir, 1, 1);
        if (inst.GetOpcode() == IR::Opcode::GetAttribute) {
            const IR::Value fval{&*block.PrependNewInst(it, IR::Opcode::BitCastF32U32,
                {IR::Value{instance_id}})};
            inst.ReplaceUsesWith(fval);
        } else {
            inst.ReplaceUsesWith(IR::Value{instance_id});
        }
        break;
    }
    case IR::Attribute::BaseVertex: {
        // BaseVertex from VertexInfo CB
        const IR::U32 base_vertex = ir.GetCbuf(
            ir.Imm32(res.vertex_info_cb_binding),
            ir.Imm32(static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexCounts) * 16
                     + 2 * 4)); // VertexCounts.z = firstVertex
        if (inst.GetOpcode() == IR::Opcode::GetAttribute) {
            const IR::Value fval{&*block.PrependNewInst(it, IR::Opcode::BitCastF32U32,
                {IR::Value{base_vertex}})};
            inst.ReplaceUsesWith(fval);
        } else {
            inst.ReplaceUsesWith(IR::Value{base_vertex});
        }
        break;
    }
    case IR::Attribute::BaseInstance: {
        // BaseInstance from VertexInfo CB
        const IR::U32 base_instance = ir.GetCbuf(
            ir.Imm32(res.vertex_info_cb_binding),
            ir.Imm32(static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexCounts) * 16
                     + 3 * 4)); // VertexCounts.w = firstInstance
        if (inst.GetOpcode() == IR::Opcode::GetAttribute) {
            const IR::Value fval{&*block.PrependNewInst(it, IR::Opcode::BitCastF32U32,
                {IR::Value{base_instance}})};
            inst.ReplaceUsesWith(fval);
        } else {
            inst.ReplaceUsesWith(IR::Value{base_instance});
        }
        break;
    }
    default:
        break;
    }
}

// Emit the VS prologue: bounds check + index buffer lookup + store vertex indices to LocalMemory.
// ib_ssbo_index: the actual ctx.ssbos[] array index for the index buffer SSBO.
void EmitVsPrologue(IR::Block& block, IR::Inst& inst,
                    const VtgAsCompute::ResourceReservations& res,
                    u32 ib_ssbo_index) {
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, it};

    // outputVertexOffset = GlobalInvocationId.x
    const IR::U32 output_vertex_offset = GenerateGlobalInvocationId(ir, 0, VsWorkGroupSize);
    // outputInstanceOffset = GlobalInvocationId.y
    const IR::U32 output_instance_offset = GenerateGlobalInvocationId(ir, 1, 1);

    // Load firstVertex and firstInstance from VertexInfo CB.
    const u32 counts_base = static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexCounts) * 16;
    const IR::U32 first_vertex = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(counts_base + 2 * 4)); // VertexCounts.z = firstVertex
    const IR::U32 first_instance = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(counts_base + 3 * 4)); // VertexCounts.w = firstInstance

    // Load ibBaseOffset from GeometryCounts.w (matches Ryujinx SetIndexBufferOffset).
    const u32 geo_base = static_cast<u32>(VtgAsCompute::VertexInfoBufferField::GeometryCounts) * 16;
    const IR::U32 ib_base_offset = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(geo_base + 3 * 4)); // GeometryCounts.w = ibBaseOffset

    // Load ibElementSize from GeometryCounts.y (1, 2, or 4 bytes per index element).
    const IR::U32 ib_elem_size = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(geo_base + 1 * 4)); // GeometryCounts.y = ibElementSize

    // Fetch vertex index from the index buffer SSBO, handling 8/16/32-bit index formats.
    // byteOffset = (ibBaseOffset + outputVertexOffset) * ibElemSize
    const IR::U32 ib_elem = ir.IAdd(ib_base_offset, output_vertex_offset);
    const IR::U32 ib_byte_offset = ir.IMul(ib_elem, ib_elem_size);
    // Align to 4-byte boundary for LoadStorage32.
    const IR::U32 aligned_byte_offset = ir.BitwiseAnd(ib_byte_offset, ir.Imm32(~3u));
    const IR::U32 raw_index = IR::U32{&*block.PrependNewInst(
        IR::Block::InstructionList::s_iterator_to(inst), IR::Opcode::LoadStorage32,
        {IR::Value{ir.Imm32(ib_ssbo_index)}, IR::Value{aligned_byte_offset}})};

    // Extract the sub-u32 index value using bit operations.
    // shift = (byteOffset & 3) * 8
    const IR::U32 sub_byte_offset = ir.BitwiseAnd(ib_byte_offset, ir.Imm32(3u));
    const IR::U32 shift = ir.IMul(sub_byte_offset, ir.Imm32(8u));
    const IR::U32 shifted = ir.ShiftRightLogical(raw_index, shift);
    // mask = (1 << (ibElemSize * 8)) - 1, but for ibElemSize=4 this overflows.
    // Use: mask = ibElemSize == 4 ? 0xFFFFFFFF : (1 << (ibElemSize * 8)) - 1
    const IR::U32 bit_width = ir.IMul(ib_elem_size, ir.Imm32(8u));
    const IR::U32 mask_raw = ir.ISub(ir.ShiftLeftLogical(ir.Imm32(1u), bit_width), ir.Imm32(1u));
    const IR::U1 is_full_u32 = ir.IEqual(ib_elem_size, ir.Imm32(4u));
    const IR::U32 mask = IR::U32{ir.Select(is_full_u32, ir.Imm32(0xFFFFFFFFu), mask_raw)};
    const IR::U32 vertex_index_vr = ir.BitwiseAnd(shifted, mask);

    // vertexIdVR = firstVertex + vertexIndexVr
    const IR::U32 vertex_id_vr = ir.IAdd(first_vertex, vertex_index_vr);
    // vertexIdIR = firstInstance + outputInstanceOffset
    const IR::U32 vertex_id_ir = ir.IAdd(first_instance, output_instance_offset);

    // OOB check: if outputVertexOffset >= vertexCount, this thread is out-of-bounds.
    const IR::U32 vertex_count = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(counts_base)); // VertexCounts.x = vertexCount
    const IR::U32 instance_count = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(counts_base + 1 * 4)); // VertexCounts.y = instanceCount
    const IR::U1 oob_vertex = ir.IGreaterThanEqual(output_vertex_offset, vertex_count, false);
    const IR::U1 oob_instance = ir.IGreaterThanEqual(output_instance_offset, instance_count, false);
    const IR::U1 is_oob = ir.LogicalOr(oob_vertex, oob_instance);
    ir.WriteLocal(ir.Imm32(res.local_slots.is_oob),
                  IR::U32{ir.Select(is_oob, ir.Imm32(1u), ir.Imm32(0u))});

    // For OOB threads, use safe default vertex IDs (0) to avoid index buffer OOB reads.
    const IR::U32 safe_vertex_id_vr = IR::U32{ir.Select(is_oob, first_vertex, vertex_id_vr)};
    const IR::U32 safe_vertex_id_ir = IR::U32{ir.Select(is_oob, first_instance, vertex_id_ir)};

    // Store to LocalMemory for later use by attribute loads.
    ir.WriteLocal(ir.Imm32(res.local_slots.vertex_index_vr), safe_vertex_id_vr);
    ir.WriteLocal(ir.Imm32(res.local_slots.vertex_index_ir), safe_vertex_id_ir);
}

// Emit the epilogue: flush all local memory output data to the VS output SSBO.
// vs_output_ssbo_index: the actual ctx.ssbos[] array index for the VS output SSBO.
void EmitVsOutputFlush(IR::Block& block, IR::Inst& inst,
                        const VtgAsCompute::ResourceReservations& res,
                        u32 vs_output_ssbo_index) {
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, it};

    // Compute output offset using GlobalInvocationId.
    const IR::U32 vertex_index = GenerateGlobalInvocationId(ir, 0, VsWorkGroupSize);
    const IR::U32 instance_index = GenerateGlobalInvocationId(ir, 1, 1);

    // Load vertex count from VertexInfo CB.
    const IR::U32 vertex_count = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexCounts) * 16));

    // OOB check: redirect OOB threads to write into a padding slot at the end of the buffer.
    const IR::U32 is_oob_flag = ir.LoadLocal(ir.Imm32(res.local_slots.is_oob));
    const IR::U1 is_oob = ir.INotEqual(is_oob_flag, ir.Imm32(0u));

    // globalVertexIndex = instanceIndex * vertexCount + vertexIndex
    const IR::U32 normal_global = ir.IAdd(ir.IMul(instance_index, vertex_count), vertex_index);
    // OOB threads write to padding position: vertexCount * instanceCount
    const IR::U32 instance_count = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexCounts) * 16 + 4));
    const IR::U32 padding_pos = ir.IMul(vertex_count, instance_count);
    const IR::U32 global_vertex = IR::U32{ir.Select(is_oob, padding_pos, normal_global)};

    const IR::U32 base_byte_offset = ir.IMul(
        ir.IMul(global_vertex, ir.Imm32(res.output_size_per_invocation)),
        ir.Imm32(4u));

    for (u32 offset = 0; offset < res.output_size_per_invocation; ++offset) {
        IR::U32 byte_offset = base_byte_offset;
        if (offset > 0) {
            byte_offset = ir.IAdd(base_byte_offset, ir.Imm32(offset * 4u));
        }

        const IR::U32 local_value = ir.LoadLocal(ir.Imm32(offset));
        block.PrependNewInst(it, IR::Opcode::WriteStorage32,
            {ir.Imm32(vs_output_ssbo_index), byte_offset, local_value});
    }
}

} // Anonymous namespace

void VertexToComputePass(IR::Program& program,
                          const VtgAsCompute::ResourceReservations& res,
                          const VtgAsCompute::IoOffsetMap& output_map) {
    if (program.stage != Stage::VertexB && program.stage != Stage::VertexA) {
        return;
    }

    // Pre-compute actual ctx.ssbos[] array indices for each SSBO.
    // The SSBO descriptors are added at the end of this function in this order:
    //   [0] = VS output, [1] = index buffer, [2..N] = vertex buffers (per used location)
    // These indices must match the order of push_back calls below.
    constexpr u32 vs_output_ssbo_index = 0;
    constexpr u32 ib_ssbo_index = 1;
    u32 next_ssbo_index = 2;
    std::unordered_map<u32, u32> vb_ssbo_index_map;
    for (u32 loc = 0; loc < IR::NUM_GENERICS; ++loc) {
        const IR::Attribute base = IR::Attribute::Generic0X + loc * 4;
        if (program.info.loads.AnyComponent(base)) {
            vb_ssbo_index_map[loc] = next_ssbo_index++;
        }
    }

    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::Prologue: {
                // After the prologue, emit VS initialization (index buffer lookup, etc.)
                EmitVsPrologue(*block, inst, res, ib_ssbo_index);
                // Initialize output local memory slots to default values.
                // GPU hardware uses default (0,0,0,1) for unwritten Position components,
                // but local memory is undefined. We must explicitly initialize Position.w
                // to 1.0 to avoid broken perspective divide when the shader omits it.
                {
                    const auto after_prologue{IR::Block::InstructionList::s_iterator_to(inst)};
                    IR::IREmitter ir2{*block, after_prologue};
                    for (u32 s = 0; s < res.output_size_per_invocation; ++s) {
                        ir2.WriteLocal(ir2.Imm32(s), ir2.Imm32(0u));
                    }
                    u32 pos_w_offset{};
                    if (VtgAsCompute::TryGetIoOffset(output_map, IR::Attribute::PositionW, pos_w_offset)) {
                        ir2.WriteLocal(ir2.Imm32(pos_w_offset), ir2.Imm32(0x3f800000u));
                    }
                }
                break;
            }
            case IR::Opcode::GetAttribute:
            case IR::Opcode::GetAttributeU32: {
                const IR::Attribute attr = inst.Arg(0).Attribute();
                if (IR::IsGeneric(attr)) {
                    ReplaceVsInputLoad(*block, inst, res, vb_ssbo_index_map);
                } else if (attr == IR::Attribute::VertexId ||
                           attr == IR::Attribute::InstanceId ||
                           attr == IR::Attribute::BaseVertex ||
                           attr == IR::Attribute::BaseInstance) {
                    ReplaceVsVertexIdLoad(*block, inst, res);
                }
                break;
            }
            case IR::Opcode::SetAttribute: {
                ReplaceVsOutputStore(*block, inst, output_map);
                break;
            }
            case IR::Opcode::Epilogue: {
                // Before the epilogue, flush all output data to the VS output SSBO.
                EmitVsOutputFlush(*block, inst, res, vs_output_ssbo_index);
                break;
            }
            default:
                break;
            }
        }
    }

    // Convert the program from Vertex to Compute stage.
    program.stage = Stage::Compute;
    program.workgroup_size = {VsWorkGroupSize, 1, 1}; // One thread per vertex
    program.info.uses_workgroup_id = true;
    program.info.uses_local_invocation_id = true;

    // Reserve local memory for output data + extra slots (vertexIndexVR, vertexIndexIR, isOob).
    // Use is_oob + 1 to ensure all slots fit, since is_oob position depends on gs_input_vertices.
    const u32 local_mem_needed = (res.local_slots.is_oob + 1) * sizeof(u32);
    program.local_memory_size = std::max(program.local_memory_size, local_mem_needed);

    // Add VertexInfo constant buffer descriptor.
    // The VTG pass inserts GetCbufU32 instructions referencing this cbuf,
    // but CollectShaderInfoPass ran before the transform, so we must register it manually.
    {
        const u32 vi_cb = res.vertex_info_cb_binding;
        program.info.constant_buffer_mask |= 1u << vi_cb;
        auto& descs = program.info.constant_buffer_descriptors;
        const auto it = std::lower_bound(descs.begin(), descs.end(), vi_cb,
            [](const ConstantBufferDescriptor& d, u32 idx) { return d.index < idx; });
        if (it == descs.end() || it->index != vi_cb) {
            descs.insert(it, ConstantBufferDescriptor{.index = vi_cb, .count = 1});
        }
        program.info.constant_buffer_used_sizes[vi_cb] = VtgAsCompute::VertexInfoBufferSize;
        program.info.used_constant_buffer_types |= IR::Type::U32;
    }

    // Clear output-related stores flags.
    // The VTG pass has already redirected all VS outputs to SSBO writes,
    // so the compute shader must not declare any Output variables.
    program.info.stores.mask.reset();
    program.info.stores_indexed_attributes = false;

    // Add SSBO descriptor for VS output (write)
    auto& ssbos = program.info.storage_buffers_descriptors;
    ssbos.push_back({
        .cbuf_index = 0,
        .cbuf_offset = res.vertex_output_ssbo_binding * 16,
        .count = 1,
        .is_written = true,
    });

    // Add SSBO descriptor for index buffer (read-only)
    ssbos.push_back({
        .cbuf_index = 0,
        .cbuf_offset = res.index_buffer_ssbo_binding * 16,
        .count = 1,
        .is_written = false,
    });

    // Add SSBO descriptors for vertex buffers (read-only, one per used location)
    for (u32 loc = 0; loc < IR::NUM_GENERICS; ++loc) {
        const IR::Attribute base = IR::Attribute::Generic0X + loc * 4;
        if (program.info.loads.AnyComponent(base)) {
            ssbos.push_back({
                .cbuf_index = 0,
                .cbuf_offset = res.GetVertexBufferSsboBinding(loc) * 16,
                .count = 1,
                .is_written = false,
            });
        }
    }
}

} // namespace Shader::Optimization
