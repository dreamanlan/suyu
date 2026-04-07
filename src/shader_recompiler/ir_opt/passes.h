// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/vtg_as_compute.h"

namespace Shader {
struct HostTranslateInfo;
}

namespace Shader::Optimization {

void CollectShaderInfoPass(Environment& env, IR::Program& program);
void ConditionalBarrierPass(IR::Program& program);
void ConstantPropagationPass(Environment& env, IR::Program& program);
void DeadCodeEliminationPass(IR::Program& program);
void GlobalMemoryToStorageBufferPass(IR::Program& program, const HostTranslateInfo& host_info);
void IdentityRemovalPass(IR::Program& program);
void LowerFp64ToFp32(IR::Program& program);
void LowerFp16ToFp32(IR::Program& program);
void LowerInt64ToInt32(IR::Program& program);
void RescalingPass(IR::Program& program);
void SsaRewritePass(IR::Program& program);
void PositionPass(Environment& env, IR::Program& program);
void TexturePass(Environment& env, IR::Program& program, const HostTranslateInfo& host_info);
void LayerPass(IR::Program& program, const HostTranslateInfo& host_info);
void VendorWorkaroundPass(IR::Program& program);
void VerificationPass(const IR::Program& program);

// Dual Vertex
void VertexATransformPass(IR::Program& program);
void VertexBTransformPass(IR::Program& program);
void JoinTextureInfo(Info& base, Info& source);
void JoinStorageInfo(Info& base, Info& source);

// VTG-as-Compute (Vertex/Geometry shader emulation via Compute)
void VertexToComputePass(IR::Program& program,
                          const VtgAsCompute::ResourceReservations& res,
                          const VtgAsCompute::IoOffsetMap& output_map);
void GeometryToComputePass(IR::Program& program,
                            const VtgAsCompute::ResourceReservations& res,
                            const VtgAsCompute::IoOffsetMap& input_map,
                            const VtgAsCompute::IoOffsetMap& output_map);
IR::Program GenerateVertexPassthroughForCompute(
    ObjectPool<IR::Inst>& inst_pool, ObjectPool<IR::Block>& block_pool,
    const VtgAsCompute::ResourceReservations& res,
    const VtgAsCompute::IoOffsetMap& output_map,
    const VaryingState& output_state, u32 used_clip_distances, bool uses_layer);

} // namespace Shader::Optimization
