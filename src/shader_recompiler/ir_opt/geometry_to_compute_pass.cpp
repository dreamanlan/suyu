// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Geometry-to-Compute IR transformation pass.
// Converts a Geometry Shader IR program into a Compute Shader that:
//   - Reads VS output data from an SSBO (instead of GS input attributes)
//   - Writes output vertex data to a GS vertex output SSBO
//   - Writes output index data to a GS index output SSBO
//   - Replaces EmitVertex with: flush local memory -> SSBO write + index write
//   - Replaces EndPrimitive with: write -1 to index buffer (primitive restart)
//
// This is modeled after Ryujinx's GeometryToCompute.cs transform.

#include <algorithm>

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/vtg_as_compute.h"

namespace Shader::Optimization {
namespace {

// Workgroup size used for GS compute dispatch.
constexpr u32 GsWorkGroupSize = 32;

// Compute GlobalInvocationId for a given component.
// GlobalId = WorkgroupId * WorkGroupSize + LocalInvocationId
IR::U32 GsGenerateGlobalId(IR::IREmitter& ir, u32 component, u32 workgroup_size) {
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

// Generate PrimitiveId = instanceIndex * vertexCount + vertexIndex
// where vertexIndex = GlobalId.x, instanceIndex = GlobalId.y
IR::U32 GeneratePrimitiveId(IR::IREmitter& ir,
                             const VtgAsCompute::ResourceReservations& res) {
    const IR::U32 vertex_count = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexCounts) * 16));
    const IR::U32 vertex_index = GsGenerateGlobalId(ir, 0, GsWorkGroupSize);
    const IR::U32 instance_index = GsGenerateGlobalId(ir, 1, 1);
    return ir.IAdd(ir.IMul(instance_index, vertex_count), vertex_index);
}

// Generate InvocationId = GlobalId.z
IR::U32 GenerateInvocationId(IR::IREmitter& ir) {
    return GsGenerateGlobalId(ir, 2, 1);
}

// Generate the base offset for GS output (vertex or index buffer).
// baseOffset = primitiveId * stride + invocationId * (stride / threadsPerPrim)
IR::U32 GenerateBaseOffset(IR::IREmitter& ir,
                            const VtgAsCompute::ResourceReservations& res,
                            u32 stride, u32 threads_per_prim) {
    const IR::U32 primitive_id = GeneratePrimitiveId(ir, res);
    const IR::U32 base_offset = ir.IMul(primitive_id, ir.Imm32(stride));
    const IR::U32 invocation_id = GenerateInvocationId(ir);
    const IR::U32 invocation_offset = ir.IMul(invocation_id,
        ir.Imm32(stride / threads_per_prim));
    return ir.IAdd(base_offset, invocation_offset);
}

// Replace GetAttribute(input) with a load from the VS output SSBO.
// GS input attributes are read from the VS output buffer at:
//   offset = (instanceIndex * vertexCount + primInputVertex) * inputSizePerInvocation + elementOffset
// where primInputVertex is read from the topology remap LocalMemory.
void ReplaceGsInputLoad(IR::Block& block, IR::Inst& inst,
                         const VtgAsCompute::ResourceReservations& res,
                         const VtgAsCompute::IoOffsetMap& input_map) {
    const IR::Attribute attr = inst.Arg(0).Attribute();
    const IR::Value vertex_arg = inst.Arg(1);

    u32 element_offset{};
    if (!VtgAsCompute::TryGetIoOffset(input_map, attr, element_offset)) {
        return;
    }

    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, it};

    // Load vertex count from VertexInfo CB.
    const IR::U32 vertex_count = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexCounts) * 16));

    // Read the remapped vertex index from topology remap LocalMemory.
    // primVertex is the GS input vertex index (0, 1, 2, ...).
    const IR::U32 prim_vertex = IR::U32{vertex_arg};
    const IR::U32 remap_slot = ir.IAdd(
        ir.Imm32(res.local_slots.topology_remap_base), prim_vertex);
    const IR::U32 prim_input_vertex = ir.LoadLocal(remap_slot);

    // instanceIndex = GlobalId.y
    const IR::U32 instance_index = GsGenerateGlobalId(ir, 1, 1);

    // vertexIndex = instanceIndex * vertexCount + primInputVertex
    const IR::U32 vertex_index = ir.IAdd(
        ir.IMul(instance_index, vertex_count), prim_input_vertex);

    // vertexBaseOffset = vertexIndex * inputSizePerInvocation
    const IR::U32 vertex_base_offset = ir.IMul(vertex_index,
        ir.Imm32(res.input_size_per_invocation));

    // final offset in bytes = (vertexBaseOffset + elementOffset) * 4
    IR::U32 final_offset = vertex_base_offset;
    if (element_offset != 0) {
        final_offset = ir.IAdd(vertex_base_offset, ir.Imm32(element_offset));
    }
    final_offset = ir.IMul(final_offset, ir.Imm32(4u));

    // Load from VS output SSBO (byte-addressed).
    const IR::Value value{&*block.PrependNewInst(it, IR::Opcode::LoadStorage32,
        {ir.Imm32(res.vertex_output_ssbo_binding), final_offset})};

    if (inst.GetOpcode() == IR::Opcode::GetAttribute) {
        const IR::Value float_value{&*block.PrependNewInst(it, IR::Opcode::BitCastF32U32,
            {value})};
        inst.ReplaceUsesWith(float_value);
    } else {
        inst.ReplaceUsesWith(value);
    }
}

// Replace SetAttribute(output) with a write to local memory.
// The output data is accumulated in local memory and flushed to the GS output SSBO
// on EmitVertex.
void ReplaceGsOutputStore(IR::Block& block, IR::Inst& inst,
                           const VtgAsCompute::IoOffsetMap& output_map) {
    const IR::Attribute attr = inst.Arg(0).Attribute();
    const IR::Value value = inst.Arg(1);

    u32 element_offset{};
    if (!VtgAsCompute::TryGetIoOffset(output_map, attr, element_offset)) {
        // Not a mapped output attribute - invalidate the store
        inst.Invalidate();
        return;
    }

    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, it};

    // Convert F32 value to U32 for local memory storage
    const IR::U32 u32_value = ir.BitCast<IR::U32>(IR::F32{value});

    // Write to local memory at the element offset
    ir.WriteLocal(ir.Imm32(element_offset), u32_value);

    inst.Invalidate();
}

// Replace EmitVertex with:
// 1. Increment local vertex counter
// 2. Compute output vertex offset using GenerateBaseOffset
// 3. Flush all local memory output data to GS vertex output SSBO
// 4. Write vertex index to GS index output SSBO
void ReplaceEmitVertex(IR::Block& block, IR::Inst& inst,
                       const VtgAsCompute::ResourceReservations& res,
                       u32 output_size, u32 max_output_vertices, u32 threads_per_prim,
                       u32 ib_stride_total) {
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, it};

    const u32 vertex_counter_slot = res.local_slots.geometry_output_vertex_count;
    const u32 index_counter_slot = res.local_slots.geometry_output_index_count;

    // OOB check: redirect OOB threads to write into a padding slot.
    const IR::U32 is_oob_flag = ir.LoadLocal(ir.Imm32(res.local_slots.is_oob));
    const IR::U1 is_oob = ir.INotEqual(is_oob_flag, ir.Imm32(0u));

    // Increment vertex counter.
    const IR::U32 old_vertex_count = ir.LoadLocal(ir.Imm32(vertex_counter_slot));
    ir.WriteLocal(ir.Imm32(vertex_counter_slot),
                  ir.IAdd(old_vertex_count, ir.Imm32(1u)));

    // Compute vertex output base offset in bytes.
    const u32 vb_stride = max_output_vertices * threads_per_prim;
    const IR::U32 base_vertex_offset = GenerateBaseOffset(ir, res, vb_stride, threads_per_prim);
    const IR::U32 normal_vertex_global = ir.IAdd(base_vertex_offset, old_vertex_count);

    // Load primitivesCount for padding position calculation.
    const u32 geo_base = static_cast<u32>(VtgAsCompute::VertexInfoBufferField::GeometryCounts) * 16;
    const IR::U32 primitives_count = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(geo_base));
    const IR::U32 instance_count_gs = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexCounts) * 16 + 4));
    const IR::U32 total_prims = ir.IMul(primitives_count, instance_count_gs);
    const IR::U32 vb_padding = ir.IMul(total_prims, ir.Imm32(vb_stride));
    const IR::U32 output_vertex_global = IR::U32{ir.Select(is_oob, vb_padding, normal_vertex_global)};

    // Increment index counter.
    const IR::U32 old_index_count = ir.LoadLocal(ir.Imm32(index_counter_slot));
    ir.WriteLocal(ir.Imm32(index_counter_slot),
                  ir.IAdd(old_index_count, ir.Imm32(1u)));

    // Compute index output base offset.
    const IR::U32 base_index_offset = GenerateBaseOffset(ir, res, ib_stride_total, threads_per_prim);
    const IR::U32 normal_index_global = ir.IAdd(base_index_offset, old_index_count);
    const IR::U32 ib_padding = ir.IMul(total_prims, ir.Imm32(ib_stride_total));
    const IR::U32 output_index_global = IR::U32{ir.Select(is_oob, ib_padding, normal_index_global)};

    // Write vertex index to GS index output SSBO (byte-addressed).
    const IR::U32 index_byte_offset = ir.IMul(output_index_global, ir.Imm32(4u));
    block.PrependNewInst(it, IR::Opcode::WriteStorage32,
        {ir.Imm32(res.geometry_index_output_ssbo_binding), index_byte_offset,
         output_vertex_global});

    // Flush local memory to GS vertex output SSBO (byte-addressed).
    const IR::U32 base_byte_offset = ir.IMul(output_vertex_global, ir.Imm32(output_size * 4u));

    for (u32 offset = 0; offset < output_size; ++offset) {
        IR::U32 vertex_offset = base_byte_offset;
        if (offset > 0) {
            vertex_offset = ir.IAdd(base_byte_offset, ir.Imm32(offset * 4u));
        }

        const IR::U32 local_value = ir.LoadLocal(ir.Imm32(offset));
        block.PrependNewInst(it, IR::Opcode::WriteStorage32,
            {ir.Imm32(res.geometry_vertex_output_ssbo_binding), vertex_offset,
             local_value});
    }

    inst.Invalidate();
}

// Replace EndPrimitive with writing -1 (primitive restart) to the GS index output SSBO.
void ReplaceEndPrimitive(IR::Block& block, IR::Inst& inst,
                         const VtgAsCompute::ResourceReservations& res,
                         u32 threads_per_prim, u32 ib_stride_total) {
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, it};

    const u32 index_counter_slot = res.local_slots.geometry_output_index_count;

    // OOB check: redirect OOB threads to write into a padding slot.
    const IR::U32 is_oob_flag = ir.LoadLocal(ir.Imm32(res.local_slots.is_oob));
    const IR::U1 is_oob = ir.INotEqual(is_oob_flag, ir.Imm32(0u));

    // Increment index counter.
    const IR::U32 old_index_count = ir.LoadLocal(ir.Imm32(index_counter_slot));
    ir.WriteLocal(ir.Imm32(index_counter_slot),
                  ir.IAdd(old_index_count, ir.Imm32(1u)));

    // Compute index buffer offset in bytes.
    const IR::U32 base_index_offset = GenerateBaseOffset(ir, res, ib_stride_total, threads_per_prim);
    const IR::U32 normal_index_global = ir.IAdd(base_index_offset, old_index_count);

    // OOB threads write to padding position.
    const u32 geo_base = static_cast<u32>(VtgAsCompute::VertexInfoBufferField::GeometryCounts) * 16;
    const IR::U32 primitives_count = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(geo_base));
    const IR::U32 instance_count_gs = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(static_cast<u32>(VtgAsCompute::VertexInfoBufferField::VertexCounts) * 16 + 4));
    const IR::U32 total_prims = ir.IMul(primitives_count, instance_count_gs);
    const IR::U32 ib_padding = ir.IMul(total_prims, ir.Imm32(ib_stride_total));
    const IR::U32 output_index_global = IR::U32{ir.Select(is_oob, ib_padding, normal_index_global)};
    const IR::U32 index_byte_offset = ir.IMul(output_index_global, ir.Imm32(4u));

    // Write -1 (primitive restart marker).
    block.PrependNewInst(it, IR::Opcode::WriteStorage32,
        {ir.Imm32(res.geometry_index_output_ssbo_binding), index_byte_offset,
         ir.Imm32(static_cast<u32>(-1))});

    inst.Invalidate();
}

// Handle special GS input loads (PrimitiveId, InvocationId)
void ReplaceGsSpecialInput(IR::Block& block, IR::Inst& inst,
                            const VtgAsCompute::ResourceReservations& res) {
    const IR::Attribute attr = inst.Arg(0).Attribute();
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, it};

    switch (attr) {
    case IR::Attribute::PrimitiveId: {
        const IR::U32 prim_id = GeneratePrimitiveId(ir, res);
        if (inst.GetOpcode() == IR::Opcode::GetAttribute) {
            const IR::Value float_val{&*block.PrependNewInst(it, IR::Opcode::BitCastF32U32,
                {IR::Value{prim_id}})};
            inst.ReplaceUsesWith(float_val);
        } else {
            inst.ReplaceUsesWith(IR::Value{prim_id});
        }
        break;
    }
    default:
        break;
    }
}

// Emit the GS prologue: topology remap initialization + counter initialization.
// topo_remap_ssbo_index: the actual ctx.ssbos[] array index for the topology remap SSBO.
void EmitGsPrologue(IR::Block& block, IR::Inst& inst,
                    const VtgAsCompute::ResourceReservations& res,
                    u32 topo_remap_ssbo_index) {
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, it};

    // outputVertexOffset = GlobalId.x (primitive offset)
    const IR::U32 output_vertex_offset = GsGenerateGlobalId(ir, 0, GsWorkGroupSize);

    // OOB check: if outputVertexOffset >= primitivesCount, this thread is out-of-bounds.
    const u32 geo_base = static_cast<u32>(VtgAsCompute::VertexInfoBufferField::GeometryCounts) * 16;
    const IR::U32 primitives_count = ir.GetCbuf(
        ir.Imm32(res.vertex_info_cb_binding),
        ir.Imm32(geo_base)); // GeometryCounts.x = primitivesCount
    const IR::U1 is_oob = ir.IGreaterThanEqual(output_vertex_offset, primitives_count, false);
    ir.WriteLocal(ir.Imm32(res.local_slots.is_oob),
                  IR::U32{ir.Select(is_oob, ir.Imm32(1u), ir.Imm32(0u))});

    // Load topology remap data from the topology remap buffer texture.
    // For OOB threads, use 0 as safe default to avoid topology remap SSBO OOB reads.
    // baseVertex = outputVertexOffset * inputVertices
    const IR::U32 safe_offset = IR::U32{ir.Select(is_oob, ir.Imm32(0u), output_vertex_offset)};
    const IR::U32 base_vertex = ir.IMul(safe_offset,
        ir.Imm32(res.gs_input_vertices));

    // For each input vertex, fetch the remapped index from the topology remap SSBO
    // and store in LocalMemory.
    for (u32 i = 0; i < res.gs_input_vertices; ++i) {
        const IR::U32 coord = ir.IAdd(base_vertex, ir.Imm32(i));
        // Convert to byte offset for LoadStorage32.
        const IR::U32 byte_coord = ir.IMul(coord, ir.Imm32(4u));
        const IR::U32 vertex_index = IR::U32{&*block.PrependNewInst(
            IR::Block::InstructionList::s_iterator_to(inst), IR::Opcode::LoadStorage32,
            {IR::Value{ir.Imm32(topo_remap_ssbo_index)}, IR::Value{byte_coord}})};
        ir.WriteLocal(ir.Imm32(res.local_slots.topology_remap_base + i), vertex_index);
    }

    // Initialize vertex and index counters to 0.
    ir.WriteLocal(ir.Imm32(res.local_slots.geometry_output_vertex_count), ir.Imm32(0u));
    ir.WriteLocal(ir.Imm32(res.local_slots.geometry_output_index_count), ir.Imm32(0u));
}

} // Anonymous namespace

void GeometryToComputePass(IR::Program& program,
                            const VtgAsCompute::ResourceReservations& res,
                            const VtgAsCompute::IoOffsetMap& input_map,
                            const VtgAsCompute::IoOffsetMap& output_map) {
    if (program.stage != Stage::Geometry) {
        return;
    }

    const u32 output_size = res.output_size_per_invocation;
    const u32 max_output_vertices = program.output_vertices;
    const u32 threads_per_prim = std::max(program.invocations, 1u);

    // Determine output topology vertices for index buffer stride calculation.
    const u32 output_topology_vertices = res.gs_output_topology_vertices;
    const u32 ib_stride_total = res.GetGeometryOutputIndexBufferStride(output_topology_vertices);

    // Pre-compute actual ctx.ssbos[] array indices for GS SSBOs.
    // The SSBO descriptors are added at the end of this function in this order:
    //   [0] = VS output, [1] = GS vertex output, [2] = GS index output, [3] = topology remap
    // These indices must match the order of push_back calls below.
    constexpr u32 topo_remap_ssbo_index = 3;

    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::Prologue: {
                // After the prologue, emit GS initialization.
                EmitGsPrologue(*block, inst, res, topo_remap_ssbo_index);
                // Initialize output local memory slots to default values.
                // GPU hardware uses default (0,0,0,1) for unwritten Position components,
                // but local memory is undefined. We must explicitly initialize Position.w
                // to 1.0 to avoid broken perspective divide when the shader omits it.
                {
                    const auto after_prologue{IR::Block::InstructionList::s_iterator_to(inst)};
                    IR::IREmitter ir2{*block, after_prologue};
                    for (u32 s = 0; s < output_size; ++s) {
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
                if (attr == IR::Attribute::PrimitiveId) {
                    ReplaceGsSpecialInput(*block, inst, res);
                } else {
                    ReplaceGsInputLoad(*block, inst, res, input_map);
                }
                break;
            }
            case IR::Opcode::InvocationId: {
                // Replace InvocationId opcode with GlobalId.z.
                const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
                IR::IREmitter ir{*block, it};
                const IR::U32 invocation_id = GenerateInvocationId(ir);
                inst.ReplaceUsesWith(IR::Value{invocation_id});
                break;
            }
            case IR::Opcode::SetAttribute: {
                ReplaceGsOutputStore(*block, inst, output_map);
                break;
            }
            case IR::Opcode::EmitVertex: {
                ReplaceEmitVertex(*block, inst, res, output_size,
                                  max_output_vertices, threads_per_prim,
                                  ib_stride_total);
                break;
            }
            case IR::Opcode::EndPrimitive: {
                ReplaceEndPrimitive(*block, inst, res,
                                    threads_per_prim, ib_stride_total);
                break;
            }
            default:
                break;
            }
        }
    }

    // Convert the program from Geometry to Compute stage.
    program.stage = Stage::Compute;
    program.workgroup_size = {GsWorkGroupSize, 1, 1}; // One thread per primitive
    program.info.uses_workgroup_id = true;
    program.info.uses_local_invocation_id = true;

    // Reserve local memory: output data + topology remap slots + 2 counters + 1 OOB flag.
    const u32 local_mem_needed =
        (output_size + res.gs_input_vertices + 3) * sizeof(u32);
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
    // The VTG pass has already redirected all GS outputs to SSBO writes,
    // so the compute shader must not declare any Output variables.
    program.info.stores.mask.reset();
    program.info.stores_indexed_attributes = false;

    // Add SSBO descriptors for VS output, GS vertex output, GS index output
    auto& ssbos = program.info.storage_buffers_descriptors;

    // VS output SSBO (read-only from GS perspective)
    ssbos.push_back({
        .cbuf_index = 0,
        .cbuf_offset = res.vertex_output_ssbo_binding * 16,
        .count = 1,
        .is_written = false,
    });

    // GS vertex output SSBO
    ssbos.push_back({
        .cbuf_index = 0,
        .cbuf_offset = res.geometry_vertex_output_ssbo_binding * 16,
        .count = 1,
        .is_written = true,
    });

    // GS index output SSBO
    ssbos.push_back({
        .cbuf_index = 0,
        .cbuf_offset = res.geometry_index_output_ssbo_binding * 16,
        .count = 1,
        .is_written = true,
    });

    // Topology remap SSBO (read-only)
    ssbos.push_back({
        .cbuf_index = 0,
        .cbuf_offset = res.topology_remap_ssbo_binding * 16,
        .count = 1,
        .is_written = false,
    });
}

} // namespace Shader::Optimization
