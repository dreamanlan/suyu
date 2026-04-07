// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <fmt/format.h>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_vtg_as_compute.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

VtgAsComputeContext::VtgAsComputeContext(const Device& device_,
                                         MemoryAllocator& memory_allocator_,
                                         Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_} {}

VtgAsComputeContext::~VtgAsComputeContext() = default;

void VtgAsComputeContext::ReleaseCompletedBuffers() {
    // Erase buffers whose associated tick has been completed by the GPU.
    std::erase_if(deferred_buffers_, [this](const auto& entry) {
        return scheduler.IsFree(entry.first);
    });
}

void VtgAsComputeContext::EnsureBuffer(vk::Buffer& buffer, VkDeviceSize& current_size,
                                        VkDeviceSize required_size, VkBufferUsageFlags usage) {
    if (current_size >= required_size && buffer) {
        return;
    }
    // Round up to a reasonable alignment.
    required_size = Common::AlignUp(required_size, 256);

    const VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = required_size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    // Defer destruction of the old buffer until the GPU finishes all
    // in-flight commands that may still reference it. We cannot use
    // scheduler.Record() because MoltenVK resolves VkBuffer -> id<MTLBuffer>
    // at vkCmd* time but the command lambda is destroyed before vkQueueSubmit.
    if (buffer) {
        deferred_buffers_.emplace_back(scheduler.CurrentTick(), std::move(buffer));
    }
    buffer = memory_allocator.CreateBuffer(ci, MemoryUsage::DeviceLocal);
    current_size = required_size;
}

bool VtgAsComputeContext::PrepareBuffers(u32 vertex_count, u32 instance_count, u32 first_vertex,
                                          u32 first_instance, u32 output_size_per_invocation,
                                          u32 gs_output_size_per_invocation,
                                          u32 gs_max_output_vertices, u32 gs_threads_per_prim,
                                          u32 gs_output_topology_vertices,
                                          bool has_geometry_shader, bool logVTG) {
    // Release deferred buffers whose GPU tick has completed.
    ReleaseCompletedBuffers();

    cached_vertex_count = vertex_count;
    cached_instance_count = instance_count;
    cached_gs_threads_per_prim = std::max(gs_threads_per_prim, 1u);

    // Update VertexInfoBuffer data.
    vertex_info_data.vertex_counts = {vertex_count, instance_count, first_vertex, first_instance};

    // Ensure VertexInfoBuffer UBO.
    EnsureBuffer(vertex_info_buffer, vertex_info_buffer_size, sizeof(VertexInfoBufferData),
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // VS output SSBO: (vertex_count * instance_count + 1) * output_size_per_invocation * 4 bytes.
    // The +1 provides a padding slot for OOB threads to write into safely.
    const VkDeviceSize vs_output_size =
        static_cast<VkDeviceSize>(vertex_count * instance_count + 1) * output_size_per_invocation * 4;
    EnsureBuffer(vertex_output_buffer, vertex_output_buffer_size, std::max(vs_output_size, VkDeviceSize{256}),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    if (has_geometry_shader) {
        // Compute primitives count based on topology (per-instance, used for GS dispatch).
        cached_primitives_count = GetPrimitivesCount(vertex_count, cached_topology);

        // Total primitives for buffer allocation must match GeneratePrimitiveId's formula:
        //   primitiveId = instanceIndex * vertexCount + primitiveIndex
        // Max primitiveId = (instanceCount-1)*vertexCount + (primitivesCount-1)
        // This equals GetPrimitivesCount(vertexCount * instanceCount) for strip topologies.
        // Matches Ryujinx: totalPrimitivesCount = GetPrimitivesCount(topology, count * instanceCount)
        const u32 total_primitives =
            GetPrimitivesCount(vertex_count * instance_count, cached_topology);
        const u32 total_vertices =
            total_primitives * gs_max_output_vertices * cached_gs_threads_per_prim;

        // GS vertex output SSBO.
        // Use gs_output_size_per_invocation (GS output stride, may differ from VS output stride).
        // +1 padding slot for OOB thread writes.
        const VkDeviceSize gs_vb_size =
            static_cast<VkDeviceSize>(total_vertices + 1) * gs_output_size_per_invocation * 4;
        EnsureBuffer(geometry_vertex_output_buffer, geometry_vertex_output_buffer_size,
                     std::max(gs_vb_size, VkDeviceSize{256}),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

        // GS index output SSBO.
        // Index buffer stride per instance = maxOutputVertices + maxCompleteStrips.
        // maxCompleteStrips = maxOutputVertices / outputTopologyVertices.
        const u32 otv = std::max(gs_output_topology_vertices, 1u);
        const u32 max_complete_strips = gs_max_output_vertices / otv;
        const u32 ib_stride_per_instance = gs_max_output_vertices + max_complete_strips;
        const u32 ib_stride_total = ib_stride_per_instance * cached_gs_threads_per_prim;
        geometry_index_count = total_primitives * ib_stride_total;
        // +1 padding slot for OOB thread writes.
        const VkDeviceSize gs_ib_size = static_cast<VkDeviceSize>(geometry_index_count + 1) * 4;
        EnsureBuffer(geometry_index_output_buffer, geometry_index_output_buffer_size,
                     std::max(gs_ib_size, VkDeviceSize{256}),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

        // GeometryCounts layout matches Ryujinx:
        //   .x = primitivesCount, .y = 0, .z = 0, .w = ibBaseOffset
        vertex_info_data.geometry_counts = {cached_primitives_count, 0, 0, 0};

        // Diagnostic logging for PrepareBuffers (sampled: every 10 calls, max 10 times).
        {
            static u32 pb_log_call_count = 0;
            static u32 pb_log_output_count = 0;
            if (logVTG) {
                const u32 call_idx = pb_log_call_count++;
                if (pb_log_output_count < 10 && (call_idx % 10) == 0) {
                    ++pb_log_output_count;
                    LOG_DBGSCP(Render_Vulkan,
                        "VTG PrepareBuffers #{}: primsPerInst={} totalPrims={} "
                        "totalVerts={} gsMaxOutVerts={} threadsPerPrim={} "
                        "ibStride={} ibStrideTotal={} geoIdxCount={}",
                        call_idx, cached_primitives_count, total_primitives,
                        total_vertices, gs_max_output_vertices, cached_gs_threads_per_prim,
                        ib_stride_per_instance, ib_stride_total, geometry_index_count);
                    LOG_DBGSCP(Render_Vulkan,
                        "VTG PrepareBuffers #{}: vsOutBufSize={} gsVbSize={} gsIbSize={} "
                        "vertCounts=({},{},{},{}) geoCounts=({},{},{},{})",
                        call_idx,
                        vertex_output_buffer_size,
                        geometry_vertex_output_buffer_size,
                        geometry_index_output_buffer_size,
                        vertex_info_data.vertex_counts[0], vertex_info_data.vertex_counts[1],
                        vertex_info_data.vertex_counts[2], vertex_info_data.vertex_counts[3],
                        vertex_info_data.geometry_counts[0], vertex_info_data.geometry_counts[1],
                        vertex_info_data.geometry_counts[2], vertex_info_data.geometry_counts[3]);
                }
            } else {
                pb_log_call_count = 0;
                pb_log_output_count = 0;
            }
        }
    }

    return true;
}

void VtgAsComputeContext::UploadVertexInfo() {
    if (!vertex_info_buffer) {
        return;
    }
    // Use the scheduler to upload the data via vkCmdUpdateBuffer (works for small buffers <= 65536).
    const auto data = vertex_info_data;
    const VkBuffer buffer = *vertex_info_buffer;
    scheduler.Record([data, buffer](vk::CommandBuffer cmdbuf) {
        cmdbuf.UpdateBuffer(buffer, 0, sizeof(VertexInfoBufferData), &data);

        // Memory barrier to ensure the UBO data is visible to compute shaders.
        const VkBufferMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer,
            .offset = 0,
            .size = sizeof(VertexInfoBufferData),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
    });
}

void VtgAsComputeContext::FillGeometryIndexBufferWithRestart() {
    if (!geometry_index_output_buffer || geometry_index_output_buffer_size == 0) {
        return;
    }
    const VkBuffer buffer = *geometry_index_output_buffer;
    const VkDeviceSize size = geometry_index_output_buffer_size;
    scheduler.Record([buffer, size](vk::CommandBuffer cmdbuf) {
        // Fill the entire index buffer with 0xFFFFFFFF (primitive restart marker).
        // This ensures unwritten slots are skipped during DrawIndexed.
        cmdbuf.FillBuffer(buffer, 0, size, 0xFFFFFFFF);

        // Barrier: transfer write -> compute shader write.
        const VkBufferMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer,
            .offset = 0,
            .size = size,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
    });
}

void VtgAsComputeContext::InsertComputeBarrier() {
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        const VkMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
    });
}

void VtgAsComputeContext::InsertComputeToGraphicsBarrier() {
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        const VkMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                             VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                   VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                               0, barrier);
    });
}

VkBuffer VtgAsComputeContext::GetOrCreateSequentialIndexBuffer(u32 count) {
    if (sequential_ib_count >= count && sequential_index_buffer) {
        return *sequential_index_buffer;
    }

    const VkDeviceSize buffer_size = static_cast<VkDeviceSize>(count) * sizeof(u32);
    const VkDeviceSize required_size = Common::AlignUp(std::max(buffer_size, VkDeviceSize{256}), 256);

    const VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = required_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    // Defer destruction until the GPU finishes in-flight commands.
    if (sequential_index_buffer) {
        deferred_buffers_.emplace_back(scheduler.CurrentTick(), std::move(sequential_index_buffer));
    }
    sequential_index_buffer = memory_allocator.CreateBuffer(ci, MemoryUsage::Upload);
    sequential_ib_count = count;

    // Fill with sequential indices: 0, 1, 2, ..., count-1
    auto* data = reinterpret_cast<u32*>(sequential_index_buffer.Mapped().data());
    for (u32 i = 0; i < count; ++i) {
        data[i] = i;
    }

    return *sequential_index_buffer;
}

void VtgAsComputeContext::UpdateTopologyRemapBuffer(u32 topology, u32 vertex_count) {
    const u32 primitives_count = GetPrimitivesCount(vertex_count, topology);
    const u32 vertices_per_prim = GetVerticesPerPrimitive(topology);
    const u32 data_count = std::max(primitives_count * vertices_per_prim, 1u);

    std::vector<u32> data(data_count, 0);

    switch (topology) {
    case 0:  // Points
    case 1:  // Lines
    case 4:  // Triangles
    case 10: // LinesAdjacency
    case 12: // TrianglesAdjacency
    case 14: // Patches
        for (u32 i = 0; i < data_count; ++i) {
            data[i] = i;
        }
        break;
    case 2: { // LineLoop
        if (data_count >= 2) {
            data[data_count - 1] = 0;
        }
        for (u32 i = 0; i < ((data_count - 1) & ~1u); i += 2) {
            data[i] = i >> 1;
            data[i + 1] = (i >> 1) + 1;
        }
        break;
    }
    case 3: { // LineStrip
        for (u32 i = 0; i < ((data_count - 1) & ~1u); i += 2) {
            data[i] = i >> 1;
            data[i + 1] = (i >> 1) + 1;
        }
        break;
    }
    case 5: { // TriangleStrip
        const u32 tri_count = data_count / 3;
        if (tri_count > 0) {
            data[0] = 0;
            data[1] = 1;
            data[2] = 2;
        }
        u32 out_index = 3;
        for (u32 tri = 1; tri < tri_count; ++tri) {
            const u32 base = tri * 3;
            if ((tri & 1) != 0) {
                data[base] = out_index - 1;
                data[base + 1] = out_index - 2;
                data[base + 2] = out_index++;
            } else {
                data[base] = out_index - 2;
                data[base + 1] = out_index - 1;
                data[base + 2] = out_index++;
            }
        }
        break;
    }
    case 6:  // TriangleFan
    case 9: { // Polygon
        const u32 tri_count = data_count / 3;
        u32 out_index = 1;
        for (u32 i = 0; i < tri_count * 3; i += 3) {
            data[i] = 0;
            data[i + 1] = out_index;
            data[i + 2] = ++out_index;
        }
        break;
    }
    case 7: { // Quads
        const u32 quad_count = data_count / 6;
        for (u32 q = 0; q < quad_count; ++q) {
            const u32 idx = q * 6;
            const u32 qi = q * 4;
            data[idx] = qi;
            data[idx + 1] = qi + 1;
            data[idx + 2] = qi + 2;
            data[idx + 3] = qi;
            data[idx + 4] = qi + 2;
            data[idx + 5] = qi + 3;
        }
        break;
    }
    case 8: { // QuadStrip
        const u32 quad_count = data_count / 6;
        if (quad_count > 0) {
            data[0] = 0;
            data[1] = 1;
            data[2] = 2;
            data[3] = 0;
            data[4] = 2;
            data[5] = 3;
        }
        for (u32 q = 1; q < quad_count; ++q) {
            const u32 idx = q * 6;
            const u32 qi = q * 2;
            data[idx] = qi + 1;
            data[idx + 1] = qi;
            data[idx + 2] = qi + 2;
            data[idx + 3] = qi + 1;
            data[idx + 4] = qi + 2;
            data[idx + 5] = qi + 3;
        }
        break;
    }
    case 11: { // LineStripAdjacency
        for (u32 i = 0; i < ((data_count - 3) & ~3u); i += 4) {
            const u32 li = i >> 2;
            data[i] = li;
            data[i + 1] = li + 1;
            data[i + 2] = li + 2;
            data[i + 3] = li + 3;
        }
        break;
    }
    case 13: { // TriangleStripAdjacency
        const u32 tri_count = data_count / 6;
        if (tri_count > 0) {
            data[0] = 0;
            data[1] = 1;
            data[2] = 2;
            data[3] = 3;
            data[4] = 4;
            data[5] = 5;
        }
        u32 out_index = 6;
        for (u32 tri = 1; tri < tri_count; ++tri) {
            const u32 base = tri * 6;
            if ((tri & 1) != 0) {
                data[base] = out_index - 2;
                data[base + 1] = out_index - 1;
                data[base + 2] = out_index - 4;
                data[base + 3] = out_index - 3;
                data[base + 4] = out_index++;
                data[base + 5] = out_index++;
            } else {
                data[base] = out_index - 4;
                data[base + 1] = out_index - 3;
                data[base + 2] = out_index - 2;
                data[base + 3] = out_index - 1;
                data[base + 4] = out_index++;
                data[base + 5] = out_index++;
            }
        }
        break;
    }
    default:
        for (u32 i = 0; i < data_count; ++i) {
            data[i] = i;
        }
        break;
    }

    const VkDeviceSize buffer_size = static_cast<VkDeviceSize>(data_count) * sizeof(u32);

    // Topology remap buffer is read-only from compute shaders.
    // Use Upload memory (host-visible) so we can directly write data without staging.
    const VkDeviceSize required_size = Common::AlignUp(std::max(buffer_size, VkDeviceSize{256}), 256);
    if (topology_remap_buffer_size < required_size || !topology_remap_buffer) {
        const VkBufferCreateInfo ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = required_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        // Defer destruction until the GPU finishes in-flight commands.
        if (topology_remap_buffer) {
            deferred_buffers_.emplace_back(scheduler.CurrentTick(), std::move(topology_remap_buffer));
        }
        topology_remap_buffer = memory_allocator.CreateBuffer(ci, MemoryUsage::Upload);
        topology_remap_buffer_size = required_size;
    }

    // Direct host write into the buffer.
    std::memcpy(topology_remap_buffer.Mapped().data(), data.data(), buffer_size);
}

u32 VtgAsComputeContext::GetPrimitivesCount(u32 vertex_count, u32 topology) {
    // Topology values match Maxwell3D PrimitiveTopology enum.
    // 0 = Points, 1 = Lines, 2 = LineLoop, 3 = LineStrip,
    // 4 = Triangles, 5 = TriangleStrip, 6 = TriangleFan,
    // 7 = Quads, 8 = QuadStrip, 9 = Polygon,
    // 10 = LinesAdjacency, 11 = LineStripAdjacency,
    // 12 = TrianglesAdjacency, 13 = TriangleStripAdjacency, 14 = Patches
    switch (topology) {
    case 1: // Lines
        return vertex_count / 2;
    case 2: // LineLoop
        return vertex_count > 1 ? vertex_count : 0;
    case 3: // LineStrip
        return vertex_count > 1 ? vertex_count - 1 : 0;
    case 4: // Triangles
        return vertex_count / 3;
    case 5: // TriangleStrip
    case 6: // TriangleFan
    case 9: // Polygon
        return vertex_count > 2 ? vertex_count - 2 : 0;
    case 7: // Quads (converted to 2 triangles each)
        return (vertex_count / 4) * 2;
    case 8: // QuadStrip
        return vertex_count > 3 ? ((vertex_count - 2) / 2) * 2 : 0;
    case 10: // LinesAdjacency
        return vertex_count / 4;
    case 11: // LineStripAdjacency
        return vertex_count > 3 ? vertex_count - 3 : 0;
    case 12: // TrianglesAdjacency
        return vertex_count / 6;
    case 13: // TriangleStripAdjacency
        return vertex_count > 2 ? (vertex_count - 2) / 2 : 0;
    default: // Points, Patches, etc.
        return vertex_count;
    }
}

u32 VtgAsComputeContext::GetVerticesPerPrimitive(u32 topology) {
    switch (topology) {
    case 1: // Lines
    case 2: // LineLoop
    case 3: // LineStrip
        return 2;
    case 4: // Triangles
    case 5: // TriangleStrip
    case 6: // TriangleFan
    case 9: // Polygon
        return 3;
    case 7: // Quads
    case 8: // QuadStrip
        return 3; // Converted to triangles.
    case 10: // LinesAdjacency
    case 11: // LineStripAdjacency
        return 4;
    case 12: // TrianglesAdjacency
    case 13: // TriangleStripAdjacency
        return 6;
    default: // Points, Patches
        return 1;
    }
}

// --- VtgComputePipeline ---

VtgComputePipeline::VtgComputePipeline(const Device& device_, DescriptorPool& descriptor_pool,
                                        const Shader::Info& info_, vk::ShaderModule spv_module_,
                                        vk::PipelineCache& pipeline_cache,
                                        std::string_view debug_name)
    : device{device_}, info{info_}, spv_module{std::move(spv_module_)} {
    // Build descriptor set layout from shader info.
    // VTG compute shaders use: UBOs (vertex info + original CBs), SSBOs, textures.
    boost::container::small_vector<VkDescriptorSetLayoutBinding, 16> bindings;
    u32 binding_index = 0;

    // Constant buffers
    for (u32 i = 0; i < info.constant_buffer_descriptors.size(); ++i) {
        bindings.push_back({
            .binding = binding_index++,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        });
    }

    // Storage buffers
    for (u32 i = 0; i < info.storage_buffers_descriptors.size(); ++i) {
        bindings.push_back({
            .binding = binding_index++,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        });
    }

    const VkDescriptorSetLayoutCreateInfo desc_layout_ci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    descriptor_set_layout = device.GetLogical().CreateDescriptorSetLayout(desc_layout_ci);
    descriptor_allocator = descriptor_pool.Allocator(*descriptor_set_layout, info);

    const VkPipelineLayoutCreateInfo pipeline_layout_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_set_layout.address(),
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = nullptr,
    };
    pipeline_layout = device.GetLogical().CreatePipelineLayout(pipeline_layout_ci);

    const VkComputePipelineCreateInfo pipeline_ci{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *spv_module,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        .layout = *pipeline_layout,
        .basePipelineHandle = nullptr,
        .basePipelineIndex = 0,
    };
    pipeline = device.GetLogical().CreateComputePipeline(pipeline_ci, *pipeline_cache);

    if (device.HasDebuggingToolAttached() && !debug_name.empty()) {
        const auto pl_name = fmt::format("{} Pipeline", debug_name);
        pipeline.SetObjectNameEXT(pl_name.c_str());
        const auto layout_name = fmt::format("{} PipelineLayout", debug_name);
        pipeline_layout.SetObjectNameEXT(layout_name.c_str());
    }
}

VtgComputePipeline::~VtgComputePipeline() = default;

void VtgComputePipeline::Dispatch(Scheduler& scheduler, VtgAsComputeContext& ctx,
                                   u32 group_count_x, u32 group_count_y, u32 group_count_z,
                                   bool has_geometry_shader,
                                   std::span<const VtgComputePipeline::SsboBinding> extra_ssbos,
                                   std::span<const VtgComputePipeline::UboBinding> extra_ubos) {
    // Allocate a descriptor set.
    const VkDescriptorSet descriptor_set = descriptor_allocator.Commit();

    // Build descriptor writes for the VTG buffers.
    boost::container::small_vector<VkWriteDescriptorSet, 32> writes;
    boost::container::small_vector<VkDescriptorBufferInfo, 40> buffer_infos;

    // Pre-reserve capacity to prevent reallocation (which would invalidate pBufferInfo pointers).
    const u32 total_descriptors = static_cast<u32>(
        info.constant_buffer_descriptors.size() + info.storage_buffers_descriptors.size());
    buffer_infos.reserve(total_descriptors);
    writes.reserve(total_descriptors);

    u32 binding = 0;

    // Constant buffers: vertex info UBO (cbuf index 1) + original shader cbufs.
    for (const auto& cbuf_desc : info.constant_buffer_descriptors) {
        if (cbuf_desc.index == 1) {
            // VertexInfoBuffer (vertex_info_cb_binding = 1)
            buffer_infos.push_back({
                .buffer = ctx.GetVertexInfoBuffer(),
                .offset = 0,
                .range = ctx.GetVertexInfoBufferSize(),
            });
        } else {
            // Original shader constant buffer: find matching UBO from extra_ubos.
            VkBuffer ubo_buffer = VK_NULL_HANDLE;
            VkDeviceSize ubo_offset = 0;
            VkDeviceSize ubo_range = 4;
            for (const auto& ubo : extra_ubos) {
                if (ubo.cbuf_index == cbuf_desc.index) {
                    ubo_buffer = ubo.buffer;
                    ubo_offset = ubo.offset;
                    ubo_range = std::max(ubo.size, VkDeviceSize{4});
                    break;
                }
            }
            // Fallback to vertex info buffer if not found.
            if (ubo_buffer == VK_NULL_HANDLE) {
                ubo_buffer = ctx.GetVertexInfoBuffer();
                ubo_offset = 0;
                ubo_range = 4;
            }
            buffer_infos.push_back({
                .buffer = ubo_buffer,
                .offset = ubo_offset,
                .range = ubo_range,
            });
        }
        writes.push_back({
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = descriptor_set,
            .dstBinding = binding++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pImageInfo = nullptr,
            .pBufferInfo = &buffer_infos.back(),
            .pTexelBufferView = nullptr,
        });
    }

    // Storage buffers: resolve each SSBO descriptor by its cbuf_offset.
    // cbuf_offset / 16 maps to the ResourceReservations binding index:
    //   0 = VS output, 1 = GS vertex output, 2 = GS index output,
    //   3 = index buffer, 4 = topology remap, 5..36 = vertex buffers
    // The extra_ssbos array is ordered as:
    //   [0] = index buffer, [1] = topology remap, [2..33] = vertex buffers
    for (const auto& ssbo_desc : info.storage_buffers_descriptors) {
        const u32 res_binding = ssbo_desc.cbuf_offset / 16;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize range = 4;

        switch (res_binding) {
        case 0: // VS output
            buffer = ctx.GetVertexOutputBuffer();
            range = std::max(ctx.GetVertexOutputBufferSize(), VkDeviceSize{4});
            break;
        case 1: // GS vertex output
            buffer = ctx.GetGeometryVertexOutputBuffer();
            range = std::max(ctx.GetGeometryVertexOutputBufferSize(), VkDeviceSize{4});
            break;
        case 2: // GS index output
            buffer = ctx.GetGeometryIndexOutputBuffer();
            range = std::max(ctx.GetGeometryIndexOutputBufferSize(), VkDeviceSize{4});
            break;
        default: {
            // Extra SSBOs: index buffer (3), topology remap (4), vertex buffers (5+)
            const u32 extra_index = res_binding - 3;
            if (extra_index < extra_ssbos.size()) {
                buffer = extra_ssbos[extra_index].buffer;
                offset = extra_ssbos[extra_index].offset;
                range = std::max(extra_ssbos[extra_index].size, VkDeviceSize{4});
            }
            break;
        }
        }

        // Use vertex info buffer as fallback for null handles.
        if (buffer == VK_NULL_HANDLE) {
            buffer = ctx.GetVertexInfoBuffer();
            offset = 0;
            range = 4;
        }

        buffer_infos.push_back({
            .buffer = buffer,
            .offset = offset,
            .range = range,
        });
        writes.push_back({
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = descriptor_set,
            .dstBinding = binding++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pImageInfo = nullptr,
            .pBufferInfo = &buffer_infos.back(),
            .pTexelBufferView = nullptr,
        });
    }

    if (!writes.empty()) {
        device.GetLogical().UpdateDescriptorSets(writes, {});
    }

    const VkPipeline vk_pipeline = *pipeline;
    const VkPipelineLayout vk_layout = *pipeline_layout;
    scheduler.Record([vk_pipeline, vk_layout, descriptor_set,
                      group_count_x, group_count_y, group_count_z](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, vk_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, vk_layout, 0,
                                  descriptor_set, {});
        cmdbuf.Dispatch(group_count_x, group_count_y, group_count_z);
    });
}

} // namespace Vulkan
