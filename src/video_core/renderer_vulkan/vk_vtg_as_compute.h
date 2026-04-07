// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"
#include "shader_recompiler/vtg_as_compute.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class GraphicsPipeline;
class Scheduler;

// VertexInfoBuffer CPU-side data, uploaded to a UBO before each VTG compute dispatch.
// Layout matches Shader::VtgAsCompute::VertexInfoBufferField.
struct VertexInfoBufferData {
    // VertexCounts: uvec4
    std::array<u32, 4> vertex_counts{}; // vertexCount, instanceCount, firstVertex, firstInstance

    // GeometryCounts: uvec4 (matches Ryujinx: .x=primitivesCount, .y=ibElementSize, .z=reserved, .w=ibBaseOffset)
    std::array<u32, 4> geometry_counts{}; // primitivesCount, ibElementSize, 0, ibBaseOffset

    // VertexStrides: uvec4[32]
    struct VertexStrideEntry {
        u32 stride{};           // in component-sized units (stride_bytes / component_size)
        u32 component_count{};  // number of components (1..4), used as mask for CopyMasked
        u32 component_size{};   // bytes per component: 1 (R8), 2 (R16), or 4 (R32/packed)
        u32 numeric_type{};     // 0=Float, 1=Uint, 2=Sint, 3=Unorm, 4=Snorm, 5=Uscaled, 6=Sscaled
    };
    std::array<VertexStrideEntry, 32> vertex_strides{};

    // VertexOffsets: uvec4[32]
    struct VertexOffsetEntry {
        u32 offset{};
        u32 divisor{}; // 0 = vertex rate, >0 = instance rate divisor
        u32 packed_format{}; // 0=normal, 1=A2B10G10R10, 2=B10G11R11
        u32 pad1{};
    };
    std::array<VertexOffsetEntry, 32> vertex_offsets{};
};
static_assert(sizeof(VertexInfoBufferData) == Shader::VtgAsCompute::VertexInfoBufferSize);

// Manages GPU buffers and compute dispatch for VTG-as-Compute mode.
// This is the runtime counterpart of the shader-side VTG-as-Compute passes.
class VtgAsComputeContext {
public:
    explicit VtgAsComputeContext(const Device& device, MemoryAllocator& memory_allocator,
                                 Scheduler& scheduler);
    ~VtgAsComputeContext();

    // Prepare buffers for a draw call.
    // Returns true if buffers were successfully allocated.
    bool PrepareBuffers(u32 vertex_count, u32 instance_count, u32 first_vertex,
                        u32 first_instance, u32 output_size_per_invocation,
                        u32 gs_output_size_per_invocation,
                        u32 gs_max_output_vertices, u32 gs_threads_per_prim,
                        u32 gs_output_topology_vertices,
                        bool has_geometry_shader, bool logVTG);

    // Get the VertexInfoBuffer data for CPU-side updates.
    VertexInfoBufferData& GetVertexInfoData() { return vertex_info_data; }

    // Upload VertexInfoBuffer data to the GPU UBO.
    void UploadVertexInfo();

    // Set the index buffer base offset (stored in GeometryCounts.w).
    void SetIndexBufferOffset(u32 offset) {
        vertex_info_data.geometry_counts[3] = offset;
    }

    // Set the input topology for the current draw.
    void SetTopology(u32 topology) {
        cached_topology = topology;
    }

    // Get Vulkan buffer handles for binding.
    VkBuffer GetVertexInfoBuffer() const { return *vertex_info_buffer; }
    VkBuffer GetVertexOutputBuffer() const { return *vertex_output_buffer; }
    VkBuffer GetGeometryVertexOutputBuffer() const { return *geometry_vertex_output_buffer; }
    VkBuffer GetGeometryIndexOutputBuffer() const { return *geometry_index_output_buffer; }
    VkBuffer GetTopologyRemapBuffer() const { return *topology_remap_buffer; }

    // Get buffer sizes for descriptor binding.
    VkDeviceSize GetVertexInfoBufferSize() const {
        return sizeof(VertexInfoBufferData);
    }
    VkDeviceSize GetVertexOutputBufferSize() const { return vertex_output_buffer_size; }
    VkDeviceSize GetGeometryVertexOutputBufferSize() const {
        return geometry_vertex_output_buffer_size;
    }
    VkDeviceSize GetGeometryIndexOutputBufferSize() const {
        return geometry_index_output_buffer_size;
    }
    VkDeviceSize GetTopologyRemapBufferSize() const {
        return topology_remap_buffer_size;
    }

    // Compute dispatch parameters.
    u32 GetVertexDispatchX() const { return (cached_vertex_count + 63) / 64; }
    u32 GetVertexDispatchY() const { return cached_instance_count; }
    u32 GetGeometryDispatchX() const { return (cached_primitives_count + 31) / 32; }
    u32 GetGeometryDispatchY() const { return cached_instance_count; }
    u32 GetGeometryDispatchZ() const { return cached_gs_threads_per_prim; }

    // Get the index count for the final indexed draw (GS mode).
    u32 GetGeometryIndexCount() const { return geometry_index_count; }

    // Get the cached topology value.
    u32 GetCachedTopology() const { return cached_topology; }

    // Get or create a sequential index buffer (0, 1, 2, ..., count-1).
    // Used for non-indexed draws where the VS prologue still reads from index buffer SSBO.
    VkBuffer GetOrCreateSequentialIndexBuffer(u32 count);
    VkDeviceSize GetSequentialIndexBufferSize() const { return sequential_ib_count * sizeof(u32); }

    // Fill the geometry index buffer with primitive restart markers (0xFFFFFFFF).
    // Must be called before GS compute dispatch to ensure unwritten slots are safe.
    void FillGeometryIndexBufferWithRestart();

    // Insert a compute-to-compute barrier (SSBO write -> SSBO read).
    void InsertComputeBarrier();

    // Insert a compute-to-graphics barrier (SSBO write -> vertex/index read).
    void InsertComputeToGraphicsBarrier();

    // Create or update the topology remap buffer for the given input topology.
    // This buffer maps sequential primitive vertex indices to the actual vertex indices
    // for strip/fan topologies (e.g., triangle strip -> individual triangles).
    void UpdateTopologyRemapBuffer(u32 topology, u32 vertex_count);

private:
    void EnsureBuffer(vk::Buffer& buffer, VkDeviceSize& current_size, VkDeviceSize required_size,
                      VkBufferUsageFlags usage);

    // Compute the number of primitives from vertex count and topology.
    static u32 GetPrimitivesCount(u32 vertex_count, u32 topology);

    // Get the number of input vertices per primitive for a given topology.
    static u32 GetVerticesPerPrimitive(u32 topology);

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;

    // CPU-side vertex info data.
    VertexInfoBufferData vertex_info_data{};

    // GPU buffers.
    vk::Buffer vertex_info_buffer{};
    vk::Buffer vertex_output_buffer{};
    vk::Buffer geometry_vertex_output_buffer{};
    vk::Buffer geometry_index_output_buffer{};
    vk::Buffer topology_remap_buffer{};

    // Current buffer sizes.
    VkDeviceSize vertex_info_buffer_size{};
    VkDeviceSize vertex_output_buffer_size{};
    VkDeviceSize geometry_vertex_output_buffer_size{};
    VkDeviceSize geometry_index_output_buffer_size{};
    VkDeviceSize topology_remap_buffer_size{};

    // Cached dispatch parameters.
    u32 cached_vertex_count{};
    u32 cached_instance_count{};
    u32 cached_primitives_count{};
    u32 cached_gs_threads_per_prim{1};
    u32 geometry_index_count{};
    // Topology for the current draw.
    u32 cached_topology{};

    // Release deferred buffers whose GPU tick has completed.
    void ReleaseCompletedBuffers();

    // Sequential index buffer for non-indexed draws.
    vk::Buffer sequential_index_buffer{};
    u32 sequential_ib_count{};

    // Buffers pending deferred destruction, paired with the scheduler tick
    // at which they were retired. MoltenVK resolves VkBuffer -> id<MTLBuffer>
    // at vkCmd* recording time but only retains it at vkQueueSubmit encode
    // time, so we must keep the buffer alive until the submit completes.
    std::vector<std::pair<u64, vk::Buffer>> deferred_buffers_;
};

// Lightweight compute pipeline wrapper for VTG-as-Compute shaders.
// Unlike ComputePipeline, this does not depend on KeplerCompute engine.
// Resources are bound directly from VtgAsComputeContext buffers.
class VtgComputePipeline {
public:
    explicit VtgComputePipeline(const Device& device, DescriptorPool& descriptor_pool,
                                const Shader::Info& info, vk::ShaderModule spv_module,
                                vk::PipelineCache& pipeline_cache,
                                std::string_view debug_name = {});
    ~VtgComputePipeline();

    VtgComputePipeline(const VtgComputePipeline&) = delete;
    VtgComputePipeline& operator=(const VtgComputePipeline&) = delete;

    // Extra SSBO binding descriptor: buffer handle, byte offset, byte size.
    struct SsboBinding {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceSize offset{};
        VkDeviceSize size{4};
    };

    // Extra UBO binding descriptor for original shader constant buffers.
    struct UboBinding {
        u32 cbuf_index{};       // Maxwell cbuf index (matches ConstantBufferDescriptor.index)
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceSize offset{};
        VkDeviceSize size{4};
    };

    // Bind descriptor set and dispatch compute.
    // extra_ssbos: additional SSBOs to bind (e.g., index buffer, vertex buffers)
    // extra_ubos: original shader constant buffers to bind
    void Dispatch(Scheduler& scheduler, VtgAsComputeContext& ctx,
                  u32 group_count_x, u32 group_count_y, u32 group_count_z,
                  bool has_geometry_shader,
                  std::span<const SsboBinding> extra_ssbos = {},
                  std::span<const UboBinding> extra_ubos = {});

    // Get shader info (for querying constant_buffer_descriptors, etc.)
    const Shader::Info& GetInfo() const { return info; }

private:
    const Device& device;
    Shader::Info info;

    vk::ShaderModule spv_module;
    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::Pipeline pipeline;
};

// A set of VTG compute pipelines for a single graphics pipeline that needs
// VTG-as-Compute emulation.
struct VtgPipelineSet {
    // VS converted to compute shader.
    std::unique_ptr<VtgComputePipeline> vs_compute;

    // GS converted to compute shader (may be null if no GS).
    std::unique_ptr<VtgComputePipeline> gs_compute;

    // The passthrough graphics pipeline (passthrough VS + original FS).
    // This is a regular GraphicsPipeline that reads from the VTG output SSBOs.
    GraphicsPipeline* passthrough_pipeline{};

    // Resource reservations used during shader compilation.
    Shader::VtgAsCompute::ResourceReservations reservations{};
    // GS-specific reservations (output_size_per_invocation = gs_output_size).
    Shader::VtgAsCompute::ResourceReservations gs_reservations{};

    // IO offset maps.
    Shader::VtgAsCompute::IoOffsetMap vs_output_map;
    Shader::VtgAsCompute::IoOffsetMap gs_output_map;

    // GS parameters.
    u32 gs_max_output_vertices{};
    u32 gs_threads_per_prim{};
    u32 gs_output_topology_vertices{3};
    bool has_geometry_shader{};
};

} // namespace Vulkan
