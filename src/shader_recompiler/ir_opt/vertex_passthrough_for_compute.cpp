// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Generates a passthrough Vertex Shader for VTG-as-Compute mode.
// This VS reads vertex data from an SSBO (written by VS-Compute or GS-Compute)
// and outputs it to standard VS outputs for the Fragment Shader.
//
// Modeled after Ryujinx's GenerateVertexPassthroughForCompute().

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/vtg_as_compute.h"

namespace Shader::Optimization {
namespace {

// Helper: generate blocks and post_order_blocks from syntax_list.
// This duplicates the local GenerateBlocks from translate_program.cpp.
IR::BlockList GenerateBlocksFromSyntax(const IR::AbstractSyntaxList& syntax_list) {
    size_t num_syntax_blocks{};
    for (const auto& node : syntax_list) {
        if (node.type == IR::AbstractSyntaxNode::Type::Block) {
            ++num_syntax_blocks;
        }
    }
    IR::BlockList blocks;
    blocks.reserve(num_syntax_blocks);
    for (const auto& node : syntax_list) {
        if (node.type == IR::AbstractSyntaxNode::Type::Block) {
            blocks.push_back(node.data.block);
        }
    }
    return blocks;
}

} // Anonymous namespace

IR::Program GenerateVertexPassthroughForCompute(
    ObjectPool<IR::Inst>& inst_pool, ObjectPool<IR::Block>& block_pool,
    const VtgAsCompute::ResourceReservations& res,
    const VtgAsCompute::IoOffsetMap& output_map,
    const VaryingState& output_state, u32 used_clip_distances, bool uses_layer) {

    IR::Program program{};
    program.stage = Stage::VertexB;
    program.is_geometry_passthrough = false;

    // Create the main block
    IR::Block* main_block = block_pool.Create(inst_pool);
    auto& main_node{program.syntax_list.emplace_back()};
    main_node.type = IR::AbstractSyntaxNode::Type::Block;
    main_node.data.block = main_block;

    IR::IREmitter ir{*main_block};

    // Prologue
    ir.Prologue();

    // Get vertex index (gl_VertexIndex in Vulkan)
    // This is the index into the SSBO data array.
    const IR::U32 vertex_index = ir.GetAttributeU32(IR::Attribute::VertexId);

    // Compute base offset into the SSBO:
    // baseOffset = vertexIndex * outputSizePerInvocation
    const IR::U32 base_offset = ir.IMul(vertex_index,
        ir.Imm32(res.output_size_per_invocation));

    // For each output in the IO offset map, load from SSBO and set as VS output.
    for (const auto& [key, element_offset] : output_map) {
        // Compute byte offset: (baseOffset + elementOffset) * 4
        IR::U32 ssbo_offset = base_offset;
        if (element_offset != 0) {
            ssbo_offset = ir.IAdd(base_offset, ir.Imm32(element_offset));
        }
        const IR::U32 byte_offset = ir.IMul(ssbo_offset, ir.Imm32(4u));

        // Load from the vertex data SSBO
        // Note: binding index 0 refers to the first (and only) SSBO descriptor
        // in this passthrough VS program, not res.vertex_output_ssbo_binding.
        // The actual Vulkan binding is determined by cbuf_offset in the descriptor.
        const IR::U32 u32_value = IR::U32{&*main_block->PrependNewInst(
            main_block->end(), IR::Opcode::LoadStorage32,
            {IR::Value{ir.Imm32(0u)}, IR::Value{byte_offset}})};

        // BitCast to F32 for SetAttribute
        const IR::F32 f32_value = ir.BitCast<IR::F32>(u32_value);

        // Set the VS output attribute
        ir.SetAttribute(key.attribute, f32_value, ir.Imm32(0u));

        // Track stores in shader info
        program.info.stores.Set(key.attribute, true);
    }

    // Create the return block with Epilogue
    IR::Block* return_block = block_pool.Create(inst_pool);
    IR::IREmitter{*return_block}.Epilogue();
    main_block->AddBranch(return_block);

    auto& return_node{program.syntax_list.emplace_back()};
    return_node.type = IR::AbstractSyntaxNode::Type::Block;
    return_node.data.block = return_block;
    program.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;

    // Generate block lists
    program.blocks = GenerateBlocksFromSyntax(program.syntax_list);
    program.post_order_blocks = program.blocks; // Linear program, same order

    // Set up shader info for the SSBO read
    program.info.storage_buffers_descriptors.push_back({
        .cbuf_index = 0,
        .cbuf_offset = res.vertex_output_ssbo_binding * 16,
        .count = 1,
        .is_written = false,
    });
    program.info.used_storage_buffer_types |= IR::Type::U32;

    // Mark loads for VertexId
    program.info.loads.Set(IR::Attribute::VertexId, true);

    // Run SSA rewrite pass to fix up phi nodes
    Optimization::SsaRewritePass(program);

    return program;
}

} // namespace Shader::Optimization
