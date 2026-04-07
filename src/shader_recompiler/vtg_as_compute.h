// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <unordered_map>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/attribute.h"
#include "shader_recompiler/varying_state.h"

namespace Shader::VtgAsCompute {

// Maximum number of vertex buffer textures (one per vertex attribute location).
constexpr u32 MaxVertexBufferTextures = 32;

// Local memory slot IDs for VTG-as-Compute.
// These are offsets into the local memory array (in u32 units).
// Actual slot positions are relative to the end of the IO data area.
struct LocalMemorySlots {
    // VS: vertex index for vertex-rate attributes (after index buffer lookup).
    u32 vertex_index_vr{};
    // VS: vertex index for instance-rate attributes.
    u32 vertex_index_ir{};
    // GS: topology remap base slot (inputVertices entries follow).
    u32 topology_remap_base{};
    // GS: output vertex counter.
    u32 geometry_output_vertex_count{};
    // GS: output index counter.
    u32 geometry_output_index_count{};
    // OOB flag slot (shared by VS and GS). 1 = out-of-bounds, 0 = valid.
    u32 is_oob{};
};

// VertexInfoBuffer field indices as uvec4 offsets into the UBO.
// Used as: byte_offset = field_value * 16 (+ location * 16 for arrays).
// Layout:
//   [0]     VertexCounts  : uvec4 (vertexCount, instanceCount, firstVertex, firstInstance)
//   [1]     GeometryCounts: uvec4 (primitivesCount, ibElementSize, reserved, ibBaseOffset)
//   [2..33] VertexStrides : uvec4[32] (stride, componentCount, componentSize, numericType)
//   [34..65]VertexOffsets : uvec4[32] (offset, divisor, packedFormat, 0)
enum class VertexInfoBufferField : u32 {
    VertexCounts = 0,
    GeometryCounts = 1,
    VertexStrides = 2,
    VertexOffsets = 2 + MaxVertexBufferTextures, // = 34
};

// Total size of VertexInfoBuffer in bytes:
//   2 * uvec4 (counts) + 32 * uvec4 (strides) + 32 * uvec4 (offsets)
//   = (2 + 32 + 32) * 16 = 1056 bytes
constexpr u32 VertexInfoBufferSize = (2 + MaxVertexBufferTextures * 2) * 16;

// Resource binding reservations for VTG-as-Compute.
// These are the extra bindings needed beyond the shader's own resources.
struct ResourceReservations {
    // Constant buffer binding for VertexInfoBuffer.
    u32 vertex_info_cb_binding{};

    // Storage buffer bindings.
    u32 vertex_output_ssbo_binding{};           // VS output data
    u32 geometry_vertex_output_ssbo_binding{};  // GS vertex output data
    u32 geometry_index_output_ssbo_binding{};   // GS index output data
    u32 index_buffer_ssbo_binding{};            // Index buffer (read-only)
    u32 topology_remap_ssbo_binding{};          // Topology remap buffer (read-only)
    u32 vertex_buffer_ssbo_base_binding{};      // Vertex buffer SSBOs (one per location)

    // Counts of reserved resources.
    u32 reserved_constant_buffers{};
    u32 reserved_storage_buffers{};

    // IO sizes per invocation (in u32 units).
    u32 input_size_per_invocation{};
    u32 output_size_per_invocation{};

    // Local memory slot assignments.
    LocalMemorySlots local_slots{};

    // GS parameters (set during pass setup).
    u32 gs_max_output_vertices{};
    u32 gs_threads_per_prim{1};
    u32 gs_input_vertices{};
    u32 gs_output_topology_vertices{3}; // 1=Points, 2=Lines, 3=Triangles

    u32 GetVertexBufferSsboBinding(u32 location) const {
        return vertex_buffer_ssbo_base_binding + location;
    }

    // Compute the GS index buffer stride per invocation.
    // This is: maxOutputVertices + maxCompleteStrips
    // where maxCompleteStrips depends on the output topology.
    u32 GetGeometryOutputIndexBufferStridePerInstance(u32 output_topology_vertices) const {
        const u32 max_complete_strips = (output_topology_vertices > 0)
            ? (gs_max_output_vertices / output_topology_vertices)
            : gs_max_output_vertices;
        return gs_max_output_vertices + max_complete_strips;
    }

    // Total GS index buffer stride = per-instance stride * threadsPerPrim.
    u32 GetGeometryOutputIndexBufferStride(u32 output_topology_vertices) const {
        return GetGeometryOutputIndexBufferStridePerInstance(output_topology_vertices) * gs_threads_per_prim;
    }
};

// Compute resource reservations for VTG-as-Compute mode.
// This sets up binding indices for the extra resources needed.
inline ResourceReservations MakeResourceReservations(u32 output_size = 0,
                                                      u32 input_size = 0,
                                                      u32 gs_input_vertices = 0) {
    ResourceReservations res{};

    // Reserve one constant buffer for VertexInfoBuffer (after the support buffer at 0).
    res.reserved_constant_buffers = 2; // 0=support, 1=vertex_info
    res.vertex_info_cb_binding = 1;

    // Reserve storage buffers:
    //   0 = VS output, 1 = GS vertex output, 2 = GS index output,
    //   3 = index buffer, 4 = topology remap, 5..36 = vertex buffers
    res.vertex_output_ssbo_binding = 0;
    res.geometry_vertex_output_ssbo_binding = 1;
    res.geometry_index_output_ssbo_binding = 2;
    res.index_buffer_ssbo_binding = 3;
    res.topology_remap_ssbo_binding = 4;
    res.vertex_buffer_ssbo_base_binding = 5;
    res.reserved_storage_buffers = 5 + MaxVertexBufferTextures;

    res.output_size_per_invocation = output_size;
    res.input_size_per_invocation = input_size;
    res.gs_input_vertices = gs_input_vertices;

    // Assign local memory slots.
    // For VS: slots are after the output data area.
    res.local_slots.vertex_index_vr = output_size;
    res.local_slots.vertex_index_ir = output_size + 1;
    res.local_slots.is_oob = output_size + 2;

    // For GS: topology remap slots start after the output data area.
    // Then vertex/index counters and OOB flag follow.
    res.local_slots.topology_remap_base = output_size;
    res.local_slots.geometry_output_vertex_count = output_size + gs_input_vertices;
    res.local_slots.geometry_output_index_count = output_size + gs_input_vertices + 1;
    // Note: is_oob slot is shared; for GS it overlaps with VS slot assignment
    // but that's fine since VS and GS are separate programs.
    if (gs_input_vertices > 0) {
        res.local_slots.is_oob = output_size + gs_input_vertices + 2;
    }

    return res;
}

// Key for IO variable offset mapping.
struct IoKey {
    IR::Attribute attribute{};
    u32 component{};

    bool operator==(const IoKey&) const = default;
};

struct IoKeyHash {
    size_t operator()(const IoKey& key) const {
        return std::hash<u64>{}(static_cast<u64>(key.attribute) << 32 | key.component);
    }
};

using IoOffsetMap = std::unordered_map<IoKey, u32, IoKeyHash>;

// Build an IO offset map from the varying state.
// This assigns a sequential u32 offset to each active output/input component.
// The order matches Ryujinx's FillIoOffsetMap:
//   Position (4 components) -> PointSize (1) -> ClipDistances -> Layer -> UserDefined
inline u32 BuildIoOffsetMap(const VaryingState& state, u32 used_clip_distances,
                            bool uses_layer, IoOffsetMap& out_map) {
    u32 offset = 0;

    // Position (4 components)
    if (state.AnyComponent(IR::Attribute::PositionX)) {
        for (u32 c = 0; c < 4; ++c) {
            out_map[IoKey{IR::Attribute::PositionX + c, 0}] = offset++;
        }
    }

    // PointSize
    if (state[IR::Attribute::PointSize]) {
        out_map[IoKey{IR::Attribute::PointSize, 0}] = offset++;
    }

    // ClipDistances
    for (u32 i = 0; i < 8; ++i) {
        if (used_clip_distances & (1u << i)) {
            out_map[IoKey{IR::Attribute::ClipDistance0 + i, 0}] = offset++;
        }
    }

    // Layer
    if (uses_layer && state[IR::Attribute::Layer]) {
        out_map[IoKey{IR::Attribute::Layer, 0}] = offset++;
    }

    // ViewportIndex
    if (state[IR::Attribute::ViewportIndex]) {
        out_map[IoKey{IR::Attribute::ViewportIndex, 0}] = offset++;
    }

    // Generic attributes (up to 32 locations, 4 components each)
    for (u32 loc = 0; loc < IR::NUM_GENERICS; ++loc) {
        const IR::Attribute base = IR::Attribute::Generic0X + loc * 4;
        if (state.AnyComponent(base)) {
            for (u32 c = 0; c < 4; ++c) {
                out_map[IoKey{base + c, 0}] = offset++;
            }
        }
    }

    return offset;
}

// Try to get the offset for a given attribute from the IO offset map.
inline bool TryGetIoOffset(const IoOffsetMap& map, IR::Attribute attr, u32& out_offset) {
    auto it = map.find(IoKey{attr, 0});
    if (it != map.end()) {
        out_offset = it->second;
        return true;
    }
    return false;
}

} // namespace Shader::VtgAsCompute
