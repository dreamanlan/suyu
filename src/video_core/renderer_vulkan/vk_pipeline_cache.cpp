// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include "common/bit_cast.h"
#include "common/cityhash.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/microprofile.h"
#include "common/thread_worker.h"
#include "core/core.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/pipeline_statistics.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_notify.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "core/memory/debug_script/DbgScpHook.h"

void dbgscpHookEmitSpirv(u64 pipeline_hash, u64 shader_hash, Shader::Stage stage)
{
    DBGSCP_HOOK_VOID("dbgscpHookEmitSpirv", pipeline_hash, shader_hash, stage);
}

namespace Vulkan {
MICROPROFILE_DECLARE(Vulkan_PipelineCache);

namespace {
using Shader::Backend::SPIRV::EmitSPIRV;
using Shader::Maxwell::ConvertLegacyToGeneric;
using Shader::Maxwell::GenerateGeometryPassthrough;
using Shader::Maxwell::MergeDualVertexPrograms;
using Shader::Maxwell::TranslateProgram;
using VideoCommon::ComputeEnvironment;
using VideoCommon::FileEnvironment;
using VideoCommon::GenericEnvironment;
using VideoCommon::GraphicsEnvironment;

constexpr u32 CACHE_VERSION = 12;
constexpr std::array<char, 8> VULKAN_CACHE_MAGIC_NUMBER{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};

template <typename Container>
auto MakeSpan(Container& container) {
    return std::span(container.data(), container.size());
}

Shader::OutputTopology MaxwellToOutputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::OutputTopology::PointList;
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::OutputTopology::LineStrip;
    default:
        return Shader::OutputTopology::TriangleStrip;
    }
}

Shader::CompareFunction MaxwellToCompareFunction(Maxwell::ComparisonOp comparison) {
    switch (comparison) {
    case Maxwell::ComparisonOp::Never_D3D:
    case Maxwell::ComparisonOp::Never_GL:
        return Shader::CompareFunction::Never;
    case Maxwell::ComparisonOp::Less_D3D:
    case Maxwell::ComparisonOp::Less_GL:
        return Shader::CompareFunction::Less;
    case Maxwell::ComparisonOp::Equal_D3D:
    case Maxwell::ComparisonOp::Equal_GL:
        return Shader::CompareFunction::Equal;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return Shader::CompareFunction::LessThanEqual;
    case Maxwell::ComparisonOp::Greater_D3D:
    case Maxwell::ComparisonOp::Greater_GL:
        return Shader::CompareFunction::Greater;
    case Maxwell::ComparisonOp::NotEqual_D3D:
    case Maxwell::ComparisonOp::NotEqual_GL:
        return Shader::CompareFunction::NotEqual;
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return Shader::CompareFunction::GreaterThanEqual;
    case Maxwell::ComparisonOp::Always_D3D:
    case Maxwell::ComparisonOp::Always_GL:
        return Shader::CompareFunction::Always;
    }
    UNIMPLEMENTED_MSG("Unimplemented comparison op={}", comparison);
    return {};
}

Shader::AttributeType CastAttributeType(const FixedPipelineState::VertexAttribute& attr) {
    if (attr.enabled == 0) {
        return Shader::AttributeType::Disabled;
    }
    switch (attr.Type()) {
    case Maxwell::VertexAttribute::Type::UnusedEnumDoNotUseBecauseItWillGoAway:
        ASSERT_MSG(false, "Invalid vertex attribute type!");
        return Shader::AttributeType::Disabled;
    case Maxwell::VertexAttribute::Type::SNorm:
    case Maxwell::VertexAttribute::Type::UNorm:
    case Maxwell::VertexAttribute::Type::Float:
        return Shader::AttributeType::Float;
    case Maxwell::VertexAttribute::Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Maxwell::VertexAttribute::Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Maxwell::VertexAttribute::Type::UScaled:
        return Shader::AttributeType::UnsignedScaled;
    case Maxwell::VertexAttribute::Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    }
    return Shader::AttributeType::Float;
}

Shader::AttributeType AttributeType(const FixedPipelineState& state, size_t index) {
    switch (state.DynamicAttributeType(index)) {
    case 0:
        return Shader::AttributeType::Disabled;
    case 1:
        return Shader::AttributeType::Float;
    case 2:
        return Shader::AttributeType::SignedInt;
    case 3:
        return Shader::AttributeType::UnsignedInt;
    }
    return Shader::AttributeType::Disabled;
}

Shader::RuntimeInfo MakeRuntimeInfo(std::span<const Shader::IR::Program> programs,
                                    const GraphicsPipelineCacheKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program) {
    Shader::RuntimeInfo info;
    if (previous_program) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
        if (previous_program->is_geometry_passthrough) {
            info.previous_stage_stores.mask |= previous_program->info.passthrough.mask;
        }
    } else {
        info.previous_stage_stores.mask.set();
    }
    const Shader::Stage stage{program.stage};
    const bool has_geometry{key.unique_hashes[4] != 0 && !programs[4].is_geometry_passthrough};
    const bool gl_ndc{key.state.ndc_minus_one_to_one != 0};
    const float point_size{Common::BitCast<float>(key.state.point_size)};
    switch (stage) {
    case Shader::Stage::VertexB:
        if (!has_geometry) {
            if (key.state.topology == Maxwell::PrimitiveTopology::Points) {
                info.fixed_state_point_size = point_size;
            }
            if (key.state.xfb_enabled) {
                auto [varyings, count] =
                    VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
                info.xfb_varyings = varyings;
                info.xfb_count = count;
            }
            info.convert_depth_mode = gl_ndc;
        }
        if (key.state.dynamic_vertex_input) {
            for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
                info.generic_input_types[index] = AttributeType(key.state, index);
            }
        } else {
            std::ranges::transform(key.state.attributes, info.generic_input_types.begin(),
                                   &CastAttributeType);
        }
        break;
    case Shader::Stage::TessellationEval:
        info.tess_clockwise = key.state.tessellation_clockwise != 0;
        info.tess_primitive = [&key] {
            const u32 raw{key.state.tessellation_primitive.Value()};
            switch (static_cast<Maxwell::Tessellation::DomainType>(raw)) {
            case Maxwell::Tessellation::DomainType::Isolines:
                return Shader::TessPrimitive::Isolines;
            case Maxwell::Tessellation::DomainType::Triangles:
                return Shader::TessPrimitive::Triangles;
            case Maxwell::Tessellation::DomainType::Quads:
                return Shader::TessPrimitive::Quads;
            }
            ASSERT(false);
            return Shader::TessPrimitive::Triangles;
        }();
        info.tess_spacing = [&] {
            const u32 raw{key.state.tessellation_spacing};
            switch (static_cast<Maxwell::Tessellation::Spacing>(raw)) {
            case Maxwell::Tessellation::Spacing::Integer:
                return Shader::TessSpacing::Equal;
            case Maxwell::Tessellation::Spacing::FractionalOdd:
                return Shader::TessSpacing::FractionalOdd;
            case Maxwell::Tessellation::Spacing::FractionalEven:
                return Shader::TessSpacing::FractionalEven;
            }
            ASSERT(false);
            return Shader::TessSpacing::Equal;
        }();
        break;
    case Shader::Stage::Geometry:
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = point_size;
        }
        if (key.state.xfb_enabled != 0) {
            auto [varyings, count] =
                VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
            info.xfb_varyings = varyings;
            info.xfb_count = count;
        }
        info.convert_depth_mode = gl_ndc;
        break;
    case Shader::Stage::Fragment:
        info.alpha_test_func = MaxwellToCompareFunction(
            key.state.UnpackComparisonOp(key.state.alpha_test_func.Value()));
        info.alpha_test_reference = Common::BitCast<float>(key.state.alpha_test_ref);
        break;
    default:
        break;
    }
    switch (key.state.topology) {
    case Maxwell::PrimitiveTopology::Points:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case Maxwell::PrimitiveTopology::Triangles:
    case Maxwell::PrimitiveTopology::TriangleStrip:
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Quads:
    case Maxwell::PrimitiveTopology::QuadStrip:
    case Maxwell::PrimitiveTopology::Polygon:
    case Maxwell::PrimitiveTopology::Patches:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    }
    info.force_early_z = key.state.early_z != 0;
    info.y_negate = key.state.y_negate != 0;
    return info;
}

size_t GetTotalPipelineWorkers() {
    const size_t max_core_threads =
        std::max<size_t>(static_cast<size_t>(std::thread::hardware_concurrency()), 2ULL) - 1ULL;
#ifdef ANDROID
    // Leave at least a few cores free in android
    constexpr size_t free_cores = 3ULL;
    if (max_core_threads <= free_cores) {
        return 1ULL;
    }
    return max_core_threads - free_cores;
#else
    return max_core_threads;
#endif
}

} // Anonymous namespace

size_t ComputePipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), sizeof *this);
    return static_cast<size_t>(hash);
}

bool ComputePipelineCacheKey::operator==(const ComputePipelineCacheKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, sizeof *this) == 0;
}

size_t GraphicsPipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), Size());
    return static_cast<size_t>(hash);
}

bool GraphicsPipelineCacheKey::operator==(const GraphicsPipelineCacheKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, Size()) == 0;
}

PipelineCache::PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_,
                             const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             GuestDescriptorQueue& guest_descriptor_queue_,
                             RenderPassCache& render_pass_cache_, BufferCache& buffer_cache_,
                             TextureCache& texture_cache_, VideoCore::ShaderNotify& shader_notify_)
    : VideoCommon::ShaderCache{device_memory_}, device{device_}, scheduler{scheduler_},
      descriptor_pool{descriptor_pool_}, guest_descriptor_queue{guest_descriptor_queue_},
      render_pass_cache{render_pass_cache_}, buffer_cache{buffer_cache_},
      texture_cache{texture_cache_}, shader_notify{shader_notify_},
      use_asynchronous_shaders{Settings::values.use_asynchronous_shaders.GetValue()},
      use_vulkan_pipeline_cache{Settings::values.use_vulkan_driver_pipeline_cache.GetValue()},
      workers(device.HasBrokenParallelShaderCompiling() ? 1ULL : GetTotalPipelineWorkers(),
              "VkPipelineBuilder"),
      serialization_thread(1, "VkPipelineSerialization") {
    const auto& float_control{device.FloatControlProperties()};
    const VkDriverId driver_id{device.GetDriverID()};
    profile = Shader::Profile{
        .supported_spirv = device.SupportedSpirvVersion(),
        .unified_descriptor_binding = true,
        .support_descriptor_aliasing = device.IsDescriptorAliasingSupported() && !device.IsMoltenVK(),
        .support_int8 = device.IsInt8Supported(),
        .support_int16 = device.IsShaderInt16Supported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .support_vertex_instance_id = false,
        .support_float_controls = device.IsKhrShaderFloatControlsSupported(),
        .support_separate_denorm_behavior =
            float_control.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_separate_rounding_mode =
            float_control.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_fp16_denorm_preserve = float_control.shaderDenormPreserveFloat16 != VK_FALSE,
        .support_fp32_denorm_preserve = float_control.shaderDenormPreserveFloat32 != VK_FALSE,
        .support_fp16_denorm_flush = float_control.shaderDenormFlushToZeroFloat16 != VK_FALSE,
        .support_fp32_denorm_flush = float_control.shaderDenormFlushToZeroFloat32 != VK_FALSE,
        .support_fp16_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat16 != VK_FALSE,
        .support_fp32_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat32 != VK_FALSE,
        .support_fp64_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat64 != VK_FALSE,
        .support_explicit_workgroup_layout = device.IsKhrWorkgroupMemoryExplicitLayoutSupported(),
        .support_vote = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_VOTE_BIT),
        .support_viewport_index_layer_non_geometry =
            device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_typeless_image_loads = device.IsFormatlessImageLoadSupported(),
        .support_demote_to_helper_invocation =
            device.IsExtShaderDemoteToHelperInvocationSupported(),
        .support_int64_atomics = device.IsExtShaderAtomicInt64Supported(),
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_geometry_shader = device.IsGeometryShaderSupported(),
        .support_native_ndc = device.IsExtDepthClipControlSupported(),
        .support_scaled_attributes = !device.MustEmulateScaledFormats(),
        .support_multi_viewport = device.SupportsMultiViewport(),
        .support_geometry_streams = device.AreTransformFeedbackGeometryStreamsSupported(),
        .support_texture_pool = device.IsTexturePoolSupported(),

        .warp_size_potentially_larger_than_guest = device.IsWarpSizePotentiallyBiggerThanGuest(),

        .lower_left_origin_mode = false,
        .need_declared_frag_colors = false,
        .need_gather_subpixel_offset = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                       driver_id == VK_DRIVER_ID_MESA_RADV ||
                                       driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS ||
                                       driver_id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA,

        .has_broken_spirv_clamp = driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
        .has_broken_spirv_position_input = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_unsigned_image_offsets = false,
        .has_broken_signed_operations = false,
        .has_broken_fp16_float_controls = driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY,
        .ignore_nan_fp_comparisons = false,
        .has_broken_spirv_subgroup_mask_vector_extract_dynamic =
            driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_robust =
            device.IsNvidia() && device.GetNvidiaArch() <= NvidiaArchitecture::Arch_Pascal,
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_user_clip_distances = device.GetMaxUserClipDistances(),
    };

    host_info = Shader::HostTranslateInfo{
        .support_float64 = device.IsFloat64Supported(),
        .support_float16 = device.IsFloat16Supported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .needs_demote_reorder = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                driver_id == VK_DRIVER_ID_SAMSUNG_PROPRIETARY,
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = device.IsExtShaderViewportIndexLayerSupported(),
        .min_ssbo_alignment = static_cast<u32>(device.GetStorageBufferAlignment()),
        .support_geometry_shader = device.IsGeometryShaderSupported(),
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_conditional_barrier = device.SupportsConditionalBarriers(),
    };

    if (device.GetMaxVertexInputAttributes() < Maxwell::NumVertexAttributes) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputAttributes is too low: {} < {}",
                    device.GetMaxVertexInputAttributes(), Maxwell::NumVertexAttributes);
    }
    if (device.GetMaxVertexInputBindings() < Maxwell::NumVertexArrays) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputBindings is too low: {} < {}",
                    device.GetMaxVertexInputBindings(), Maxwell::NumVertexArrays);
    }

    dynamic_features = DynamicFeatures{
        .has_extended_dynamic_state = device.IsExtExtendedDynamicStateSupported(),
        .has_extended_dynamic_state_2 = device.IsExtExtendedDynamicState2Supported(),
        .has_extended_dynamic_state_2_extra = device.IsExtExtendedDynamicState2ExtrasSupported(),
        .has_extended_dynamic_state_3_blend = device.IsExtExtendedDynamicState3BlendingSupported(),
        .has_extended_dynamic_state_3_enables = device.IsExtExtendedDynamicState3EnablesSupported(),
        .has_dynamic_vertex_input = device.IsExtVertexInputDynamicStateSupported(),
    };
}

PipelineCache::~PipelineCache() {
    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
    }
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {
    MICROPROFILE_SCOPE(Vulkan_PipelineCache);

    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_pipeline = nullptr;
        return nullptr;
    }
    graphics_key.state.Refresh(*maxwell3d, dynamic_features);

    if (current_pipeline) {
        GraphicsPipeline* const next{current_pipeline->Next(graphics_key)};
        if (next) {
            current_pipeline = next;
            return BuiltPipeline(current_pipeline);
        }
    }
    return CurrentGraphicsPipelineSlowPath();
}

ComputePipeline* PipelineCache::CurrentComputePipeline(ComputePipelineCacheKey& ckey) {
    MICROPROFILE_SCOPE(Vulkan_PipelineCache);

    const ShaderInfo* const shader{ComputeShader()};
    if (!shader) {
        return nullptr;
    }
    const auto& qmd{kepler_compute->launch_description};
    const ComputePipelineCacheKey key{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size{qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
    };
    ckey = key;
    const auto [pair, is_new]{compute_cache.try_emplace(key)};
    auto& pipeline{pair->second};
    if (!is_new) {
        return pipeline.get();
    }
    pipeline = CreateComputePipeline(key, shader);
    return pipeline.get();
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (title_id == 0) {
        return;
    }
    const auto shader_dir{Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir)};
    const auto base_dir{shader_dir / fmt::format("{:016x}", title_id)};
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create pipeline cache directories");
        return;
    }
    pipeline_cache_filename = base_dir / "vulkan.bin";

    if (use_vulkan_pipeline_cache) {
        vulkan_pipeline_cache_filename = base_dir / "vulkan_pipelines.bin";
        vulkan_pipeline_cache =
            LoadVulkanPipelineCache(vulkan_pipeline_cache_filename, CACHE_VERSION);
    }

    struct {
        std::mutex mutex;
        size_t total{};
        size_t built{};
        bool has_loaded{};
        std::unique_ptr<PipelineStatistics> statistics;
    } state;

    if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
        state.statistics = std::make_unique<PipelineStatistics>(device);
    }
    const auto load_compute{[&](std::ifstream& file, FileEnvironment env) {
        ComputePipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        workers.QueueWork([this, key, env_ = std::move(env), &state, &callback]() mutable {
            ShaderPools pools;
            auto pipeline{CreateComputePipeline(pools, key, env_, state.statistics.get(), false)};
            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                compute_cache.emplace(key, std::move(pipeline));
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    const auto load_graphics{[&](std::ifstream& file, std::vector<FileEnvironment> envs) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        if ((key.state.extended_dynamic_state != 0) !=
                dynamic_features.has_extended_dynamic_state ||
            (key.state.extended_dynamic_state_2 != 0) !=
                dynamic_features.has_extended_dynamic_state_2 ||
            (key.state.extended_dynamic_state_2_extra != 0) !=
                dynamic_features.has_extended_dynamic_state_2_extra ||
            (key.state.extended_dynamic_state_3_blend != 0) !=
                dynamic_features.has_extended_dynamic_state_3_blend ||
            (key.state.extended_dynamic_state_3_enables != 0) !=
                dynamic_features.has_extended_dynamic_state_3_enables ||
            (key.state.dynamic_vertex_input != 0) != dynamic_features.has_dynamic_vertex_input) {
            return;
        }
        workers.QueueWork([this, key, envs_ = std::move(envs), &state, &callback]() mutable {
            ShaderPools pools;
            boost::container::static_vector<Shader::Environment*, 5> env_ptrs;
            for (auto& env : envs_) {
                env_ptrs.push_back(&env);
            }
            auto pipeline{CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs),
                                                 state.statistics.get(), false)};

            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                graphics_cache.emplace(key, std::move(pipeline));
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    VideoCommon::LoadPipelines(stop_loading, pipeline_cache_filename, CACHE_VERSION, load_compute,
                               load_graphics);

    LOG_INFO(Render_Vulkan, "Total Pipeline Count: {}", state.total);

    std::unique_lock lock{state.mutex};
    callback(VideoCore::LoadCallbackStage::Build, 0, state.total);
    state.has_loaded = true;
    lock.unlock();

    workers.WaitForRequests(stop_loading);

    if (use_vulkan_pipeline_cache) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
    }

    if (state.statistics) {
        state.statistics->Report();
    }
}

void PipelineCache::DumpInfo(std::ostream& os)const {
    for (auto&& pair : graphics_cache) {
        pair.second->DumpInfo(os, pair.first);
        os << std::endl;
    }
    for (auto&& pair : compute_cache) {
        pair.second->DumpInfo(os, pair.first);
        os << std::endl;
    }
}

int PipelineCache::ReplaceShader(uint64_t hash, Shader::Stage stage, const std::vector<uint32_t>& code) {
    int ct = 0;
    if (stage == Shader::Stage::Compute) {
        for (auto&& pair : compute_cache) {
            auto&& key = pair.first;
            auto&& pipeline = pair.second;
            if (key.unique_hash == hash) {
                pipeline->ReplaceShader(code, key);
                ++ct;
            }
        }
    }
    else {
        for (auto&& pair : graphics_cache) {
            auto&& key = pair.first;
            auto&& pipeline = pair.second;
            if (key.unique_hashes[static_cast<int>(stage) + 1] == hash) {
                pipeline->ReplaceShader(stage, code, key);
                ++ct;
            }
        }
    }
    auto&& it = replace_shaders.find(hash);
    if (it == replace_shaders.end()) {
        std::unordered_map<Shader::Stage, std::vector<uint32_t>> stage_shaders{};
        stage_shaders[stage] = code;
        replace_shaders.insert(std::make_pair(hash, std::move(stage_shaders)));
    }
    else {
        it->second[stage] = code;
    }
    return ct;
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath() {
    const auto [pair, is_new]{graphics_cache.try_emplace(graphics_key)};
    auto& pipeline{pair->second};
    if (is_new) {
        pipeline = CreateGraphicsPipeline();
    }
    if (!pipeline) {
        return nullptr;
    }
    if (current_pipeline) {
        current_pipeline->AddTransition(pipeline.get());
    }
    current_pipeline = pipeline.get();
    return BuiltPipeline(current_pipeline);
}

GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline) const noexcept {
    if (pipeline->IsBuilt()) {
        return pipeline;
    }
    if (!use_asynchronous_shaders) {
        return pipeline;
    }
    // If something is using depth, we can assume that games are not rendering anything which
    // will be used one time.
    if (maxwell3d->regs.zeta_enable) {
        return nullptr;
    }
    // If games are using a small index count, we can assume these are full screen quads.
    // Usually these shaders are only used once for building textures so we can assume they
    // can't be built async
    const auto& draw_state = maxwell3d->draw_manager->GetDrawState();
    if (draw_state.index_buffer.count <= 6 || draw_state.vertex_buffer.count <= 6) {
        return pipeline;
    }
    return nullptr;
}

[[nodiscard]] inline u32 GetHandleSecond(u32 raw, bool via_header_index) {
    if (via_header_index) {
        return raw;
    } else {
        const Tegra::Texture::TextureHandle handle{raw};
        return handle.tsc_id;
    }
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    ShaderPools& pools, const GraphicsPipelineCacheKey& key,
    std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
    bool build_in_parallel) try {
    auto hash = key.Hash();
    LOG_INFO(Render_Vulkan, "0x{:016x}", hash);
    size_t env_index{0};
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};

    // Layer passthrough generation for devices without VK_EXT_shader_viewport_index_layer
    Shader::IR::Program* layer_source_program{};
    const bool gs_supported = device.IsGeometryShaderSupported();

    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && is_emulated_stage) {
            // Skip synthesizing layer emulation GS when device has no GS support.
            // In that case, layer output should be handled by VS via
            // VK_EXT_shader_viewport_index_layer.
            if (!gs_supported) {
                LOG_WARNING(Render_Vulkan,
                    "Skipping layer emulation GS synthesis (no HW GS support)");
                continue;
            }
            auto topology = MaxwellToOutputTopology(key.state.topology);
            programs[index] = GenerateGeometryPassthrough(pools.inst, pools.block, host_info,
                                                          *layer_source_program, topology);
            continue;
        }
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        if (!uses_vertex_a || index != 1) {
            // Normal path
            programs[index] = TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        } else {
            // VertexB path when VertexA is present.
            auto& program_va{programs[0]};
            auto program_vb{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
            programs[index] = MergeDualVertexPrograms(program_va, program_vb, env);
        }

        if (Settings::values.dump_shaders) {
            env.Dump(hash, key.unique_hashes[index]);
        }

        if (programs[index].info.requires_layer_emulation) {
            layer_source_program = &programs[index];
        }
    }
    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};
    std::array<vk::ShaderModule, Maxwell::MaxShaderStage> modules;

    const Shader::IR::Program* previous_stage{};
    Shader::Backend::Bindings binding;
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && !is_emulated_stage) {
            continue;
        }
        UNIMPLEMENTED_IF(index == 0);

        Shader::IR::Program& program{programs[index]};
        const size_t stage_index{index - 1};
        infos[stage_index] = &program.info;

        if (maxwell3d) {
            const auto& regs{maxwell3d->regs};
            const bool via_header_index{regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding};
            const auto& cbufs{maxwell3d->state.shader_stages[stage_index].const_buffers};
            [[maybe_unused]]
            const auto read_handle{[&](const auto& desc, u32 index) {
                ASSERT(cbufs[desc.cbuf_index].enabled);
                const u32 index_offset{index << desc.size_shift};
                const u32 offset{desc.cbuf_offset + index_offset};
                const GPUVAddr addr{cbufs[desc.cbuf_index].address + offset};
                if constexpr (std::is_same_v<decltype(desc), const Shader::TextureDescriptor&> ||
                              std::is_same_v<decltype(desc), const Shader::TextureBufferDescriptor&>) {
                    if (desc.has_secondary) {
                        ASSERT(cbufs[desc.secondary_cbuf_index].enabled);
                        const u32 second_offset{desc.secondary_cbuf_offset + index_offset};
                        const GPUVAddr separate_addr{cbufs[desc.secondary_cbuf_index].address +
                            second_offset};
                        const u32 lhs_raw{gpu_memory->Read<u32>(addr) << desc.shift_left};
                        const u32 rhs_raw{gpu_memory->Read<u32>(separate_addr)
                            << desc.secondary_shift_left};
                        const u32 raw{lhs_raw | rhs_raw};
                        return GetHandleSecond(raw, via_header_index);
                    }
                }
                return GetHandleSecond(gpu_memory->Read<u32>(addr), via_header_index);
            }};
#ifdef __APPLE__
            for (auto& desc : program.info.texture_descriptors) {
                for (u32 tindex = 0; tindex < desc.count; ++tindex) {
                    const auto handle{read_handle(desc, tindex)};
                    VideoCommon::SamplerId sampler{texture_cache.GetGraphicsSamplerId(handle)};
                    (void)sampler;
                }
            }
#endif
        }

        const auto runtime_info{MakeRuntimeInfo(programs, key, program, previous_stage)};
        ConvertLegacyToGeneric(program, runtime_info);
        dbgscpHookEmitSpirv(hash, key.unique_hashes[index], Shader::StageFromIndex(stage_index));
        std::vector<u32> code{EmitSPIRV(profile, runtime_info, program, binding)};
        device.SaveShader(code);
        VideoCommon::DumpSpirvShader(hash, key.unique_hashes[index], Shader::StageFromIndex(stage_index), code);
        // Skip geometry shader on devices without hardware GS support (e.g. MoltenVK).
        // For passthrough GS, this is safe because VS already handles all outputs.
        // For non-passthrough GS, this is a degraded fallback (Phase 3-4 will add
        // compute shader emulation).
        if (!gs_supported && index == static_cast<u32>(Maxwell::ShaderType::Geometry)) {
            const bool is_passthrough = program.is_geometry_passthrough;
            LOG_WARNING(Render_Vulkan,
                "Skipping {} geometry shader (no HW GS support), "
                "pipeline:{:016x} vs:{:016x} gs:{:016x} fs:{:016x}",
                is_passthrough ? "passthrough" : "non-passthrough",
                static_cast<u64>(hash),
                key.unique_hashes[static_cast<int>(Shader::Stage::VertexB) + 1],
                key.unique_hashes[static_cast<int>(Shader::Stage::Geometry) + 1],
                key.unique_hashes[static_cast<int>(Shader::Stage::Fragment) + 1]);
            // Clear infos for this stage so GraphicsPipeline won't create
            // descriptors for a non-existent shader module.
            infos[stage_index] = nullptr;
            continue;
        }
        auto&& it = replace_shaders.find(key.unique_hashes[index]);
        if (it != replace_shaders.end()) {
            auto&& it2 = it->second.find(Shader::StageFromIndex(stage_index));
            if (it2 != it->second.end()) {
                code = it2->second;
            }
        }
        modules[stage_index] = BuildShader(device, code);
        {
            const std::string name{fmt::format("Shader {:016x}", key.unique_hashes[index])};
            modules[stage_index].SetObjectNameEXT(name.c_str());
        }
        previous_stage = &program;
    }
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return std::make_unique<GraphicsPipeline>(
        scheduler, buffer_cache, texture_cache, vulkan_pipeline_cache, &shader_notify, device,
        descriptor_pool, guest_descriptor_queue, thread_worker, statistics, render_pass_cache, key,
        std::move(modules), infos);

} catch (const Shader::Exception& exception) {
    auto hash = key.Hash();
    size_t env_index{0};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        env.Dump(hash, key.unique_hashes[index]);
    }
    std::stringstream ss;
    ss << "exception in graphics pipeline: ";
    for (int ix = 0; ix < static_cast<int>(VideoCommon::NUM_STAGES); ++ix) {
        if (ix > 0)
            ss << "|";
        ss << fmt::format("{:016x}", key.unique_hashes[ix + 1]);
    }
    ss << fmt::format(" {}", exception.what());
    auto&& msg = ss.str();
    LOG_ERROR(Render_Vulkan, "{}", msg);
    Core::g_MainThreadCaller.RequestLogToView(std::move(msg));
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return nullptr;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, graphics_key.unique_hashes);

    main_pools.ReleaseContents();
    auto pipeline{
        CreateGraphicsPipeline(main_pools, graphics_key, environments.Span(), nullptr, true)};
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    serialization_thread.QueueWork([this, key = graphics_key, envs = std::move(environments.envs)] {
        boost::container::static_vector<const GenericEnvironment*, Maxwell::MaxShaderProgram>
            env_ptrs;
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0) {
                env_ptrs.push_back(&envs[index]);
            }
        }
        SerializePipeline(key, env_ptrs, pipeline_cache_filename, CACHE_VERSION);
    });
    return pipeline;
}

VtgPipelineSet* PipelineCache::GetVtgPipelineSet(const GraphicsPipelineCacheKey& key) {
    // Check if already cached.
    const auto it = vtg_compute_cache.find(key);
    if (it != vtg_compute_cache.end()) {
        return it->second.get();
    }

    // Only create VTG pipeline set for pipelines with non-passthrough GS
    // on devices without HW GS support.
    if (device.IsGeometryShaderSupported()) {
        return nullptr;
    }

    const u32 gs_index = static_cast<u32>(Maxwell::ShaderType::Geometry);
    if (key.unique_hashes[gs_index] == 0) {
        return nullptr;
    }

    // Re-translate VS and GS programs for VTG-as-Compute transformation.
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, key.unique_hashes);

    ShaderPools pools;
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;

    size_t env_index = 0;
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*environments.Span()[env_index]};
        ++env_index;

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        programs[index] = Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
    }

    // Check if GS is passthrough - if so, no VTG compute needed.
    auto& gs_program = programs[gs_index];
    if (gs_program.is_geometry_passthrough) {
        vtg_compute_cache.emplace(key, nullptr);
        return nullptr;
    }

    LOG_INFO(Render_Vulkan,
        "Creating VTG-as-Compute pipeline set for pipeline:{:016x} vs:{:016x} gs:{:016x}",
        static_cast<u64>(key.Hash()),
        key.unique_hashes[static_cast<int>(Shader::Stage::VertexB) + 1],
        key.unique_hashes[static_cast<int>(Shader::Stage::Geometry) + 1]);

    auto vtg_set = std::make_unique<VtgPipelineSet>();
    vtg_set->has_geometry_shader = true;
    vtg_set->gs_max_output_vertices = gs_program.output_vertices;
    vtg_set->gs_threads_per_prim = std::max(gs_program.invocations, 1u);

    // Determine GS output topology vertices from the GS program header.
    switch (gs_program.output_topology) {
    case Shader::OutputTopology::PointList:
        vtg_set->gs_output_topology_vertices = 1;
        break;
    case Shader::OutputTopology::LineStrip:
        vtg_set->gs_output_topology_vertices = 2;
        break;
    default: // TriangleStrip
        vtg_set->gs_output_topology_vertices = 3;
        break;
    }

    // Determine GS input vertices from the topology.
    Shader::InputTopology input_topo = Shader::InputTopology::Triangles;
    switch (key.state.topology) {
    case Maxwell::PrimitiveTopology::Points:
        input_topo = Shader::InputTopology::Points;
        break;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        input_topo = Shader::InputTopology::Lines;
        break;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        input_topo = Shader::InputTopology::LinesAdjacency;
        break;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        input_topo = Shader::InputTopology::TrianglesAdjacency;
        break;
    default:
        input_topo = Shader::InputTopology::Triangles;
        break;
    }
    const u32 gs_input_vertices = Shader::InputTopologyVertices::vertices(input_topo);

    // Build IO offset maps from VS output state.
    const u32 vs_index = static_cast<u32>(Maxwell::ShaderType::VertexB);
    auto& vs_program = programs[vs_index];

    // Convert legacy attributes to generic before building IO offset maps.
    // Without this, legacy attributes (e.g. ColorFrontDiffuse) would be missing
    // from the IO maps, causing GS output data loss and FS varying mismatch.
    {
        const Shader::IR::Program* prev = nullptr;
        const auto vs_runtime = MakeRuntimeInfo(programs, key, vs_program, prev);
        Shader::Maxwell::ConvertLegacyToGeneric(vs_program, vs_runtime);
        prev = &vs_program;
        const auto gs_runtime = MakeRuntimeInfo(programs, key, gs_program, prev);
        Shader::Maxwell::ConvertLegacyToGeneric(gs_program, gs_runtime);
    }

    Shader::VtgAsCompute::IoOffsetMap vs_output_map;
    const u32 vs_output_size = Shader::VtgAsCompute::BuildIoOffsetMap(
        vs_program.info.stores, vs_program.info.used_clip_distances,
        vs_program.info.stores[Shader::IR::Attribute::Layer], vs_output_map);

    // Build GS output map.
    Shader::VtgAsCompute::IoOffsetMap gs_output_map;
    const u32 gs_output_size = Shader::VtgAsCompute::BuildIoOffsetMap(
        gs_program.info.stores, gs_program.info.used_clip_distances,
        gs_program.info.stores[Shader::IR::Attribute::Layer], gs_output_map);

    // Build resource reservations with proper sizes.
    // VS reservations use vs_output_size for output stride.
    auto res = Shader::VtgAsCompute::MakeResourceReservations(
        vs_output_size, vs_output_size, gs_input_vertices);
    res.gs_max_output_vertices = gs_program.output_vertices;
    res.gs_threads_per_prim = vtg_set->gs_threads_per_prim;
    res.gs_output_topology_vertices = vtg_set->gs_output_topology_vertices;

    // GS reservations use gs_output_size for output stride.
    // GS writes to its own vertex output SSBO with a different stride than VS.
    auto gs_res = Shader::VtgAsCompute::MakeResourceReservations(
        gs_output_size, vs_output_size, gs_input_vertices);
    gs_res.gs_max_output_vertices = gs_program.output_vertices;
    gs_res.gs_threads_per_prim = vtg_set->gs_threads_per_prim;
    gs_res.gs_output_topology_vertices = vtg_set->gs_output_topology_vertices;

    vtg_set->reservations = res;
    vtg_set->gs_reservations = gs_res;
    vtg_set->vs_output_map = vs_output_map;
    vtg_set->gs_output_map = gs_output_map;

    // Transform VS to compute.
    Shader::Optimization::VertexToComputePass(vs_program, res, vs_output_map);

    // Compile VS compute shader.
    std::vector<u32> vs_code{Shader::Backend::SPIRV::EmitSPIRV(profile, vs_program)};
    device.SaveShader(vs_code);
    VideoCommon::DumpSpirvShader(key.Hash(),
        key.unique_hashes[static_cast<int>(Shader::Stage::VertexB) + 1],
        Shader::Stage::Compute, vs_code);
    vk::ShaderModule vs_spv = BuildShader(device, vs_code);
    {
        const auto name{fmt::format("VTG-VS-Compute {:016x}",
            key.unique_hashes[static_cast<int>(Shader::Stage::VertexB) + 1])};
        vs_spv.SetObjectNameEXT(name.c_str());
    }

    try {
        const auto vs_pipeline_name{fmt::format("VTG-VS-Compute {:016x}",
            key.unique_hashes[static_cast<int>(Shader::Stage::VertexB) + 1])};
        vtg_set->vs_compute = std::make_unique<VtgComputePipeline>(
            device, descriptor_pool, vs_program.info, std::move(vs_spv), vulkan_pipeline_cache,
            vs_pipeline_name);
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Failed to create VS compute pipeline: {}", e.what());
        vtg_compute_cache.emplace(key, nullptr);
        return nullptr;
    }

    // Transform GS to compute.
    auto gs_program_copy = programs[gs_index]; // Work on a copy
    Shader::Optimization::GeometryToComputePass(gs_program_copy, gs_res, vs_output_map, gs_output_map);

    // Compile GS compute shader.
    std::vector<u32> gs_code{Shader::Backend::SPIRV::EmitSPIRV(profile, gs_program_copy)};
    device.SaveShader(gs_code);
    VideoCommon::DumpSpirvShader(key.Hash(),
        key.unique_hashes[static_cast<int>(Shader::Stage::Geometry) + 1],
        Shader::Stage::Compute, gs_code);
    vk::ShaderModule gs_spv = BuildShader(device, gs_code);
    {
        const auto name{fmt::format("VTG-GS-Compute {:016x}",
            key.unique_hashes[static_cast<int>(Shader::Stage::Geometry) + 1])};
        gs_spv.SetObjectNameEXT(name.c_str());
    }

    try {
        const auto gs_pipeline_name{fmt::format("VTG-GS-Compute {:016x}",
            key.unique_hashes[static_cast<int>(Shader::Stage::Geometry) + 1])};
        vtg_set->gs_compute = std::make_unique<VtgComputePipeline>(
            device, descriptor_pool, gs_program_copy.info, std::move(gs_spv),
            vulkan_pipeline_cache, gs_pipeline_name);
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Failed to create GS compute pipeline: {}", e.what());
        vtg_compute_cache.emplace(key, nullptr);
        return nullptr;
    }

    LOG_INFO(Render_Vulkan,
        "VTG-as-Compute pipeline set created successfully "
        "(vs_output_size={}, gs_output_size={}, max_output_verts={}, threads_per_prim={})",
        vs_output_size, gs_output_size,
        vtg_set->gs_max_output_vertices, vtg_set->gs_threads_per_prim);

    // Generate passthrough VS for the final draw.
    // This VS reads from the GS output SSBO and writes to standard VS outputs.
    try {
        // Use GS output map for the passthrough VS (it reads GS output data).
        const auto& passthrough_output_map = vtg_set->has_geometry_shader
            ? gs_output_map : vs_output_map;
        const auto& passthrough_res = vtg_set->has_geometry_shader
            ? [&]() -> const Shader::VtgAsCompute::ResourceReservations& {
                // For GS mode, the passthrough VS reads from geometry_vertex_output SSBO.
                // Use gs_res which already has output_size_per_invocation = gs_output_size.
                static thread_local Shader::VtgAsCompute::ResourceReservations pt_res;
                pt_res = gs_res;
                pt_res.vertex_output_ssbo_binding = gs_res.geometry_vertex_output_ssbo_binding;
                return pt_res;
            }()
            : res;

        Shader::IR::Program passthrough_program =
            Shader::Optimization::GenerateVertexPassthroughForCompute(
                pools.inst, pools.block, passthrough_res, passthrough_output_map,
                gs_program.info.stores, gs_program.info.used_clip_distances,
                gs_program.info.stores[Shader::IR::Attribute::Layer]);

        // Propagate legacy_stores_mapping from GS to passthrough VS.
        // This ensures FS ConvertLegacyToGeneric uses matching generic slots
        // when mapping legacy loads (e.g. ColorFrontDiffuse -> Generic5X).
        passthrough_program.info.legacy_stores_mapping = gs_program.info.legacy_stores_mapping;

        // Compile passthrough VS.
        // Use a persistent Bindings object so FS compilation starts after VS bindings.
        Shader::Backend::Bindings pt_binding{};
        std::vector<u32> pt_code{Shader::Backend::SPIRV::EmitSPIRV(profile, {}, passthrough_program, pt_binding)};
        device.SaveShader(pt_code);
        VideoCommon::DumpSpirvShader(key.Hash(),
            key.unique_hashes[static_cast<int>(Shader::Stage::VertexB) + 1],
            Shader::Stage::VertexB, pt_code, "PT_VB");
        vk::ShaderModule pt_spv = BuildShader(device, pt_code);
        {
            const auto name{fmt::format("VTG-Passthrough-VS {:016x}",
                key.unique_hashes[static_cast<int>(Shader::Stage::VertexB) + 1])};
            pt_spv.SetObjectNameEXT(name.c_str());
        }

        // Create a modified graphics pipeline key for the passthrough pipeline.
        // Copy the original key but clear the GS hash.
        GraphicsPipelineCacheKey pt_key = key;
        pt_key.unique_hashes[gs_index] = 0;

        // Override topology to match GS output topology (not the original draw topology).
        // GS output index buffer uses strip + primitive restart format.
        if (vtg_set->has_geometry_shader) {
            switch (vtg_set->gs_output_topology_vertices) {
            case 1:
                pt_key.state.topology.Assign(Maxwell::PrimitiveTopology::Points);
                break;
            case 2:
                pt_key.state.topology.Assign(Maxwell::PrimitiveTopology::LineStrip);
                break;
            default: // 3 = TriangleStrip
                pt_key.state.topology.Assign(Maxwell::PrimitiveTopology::TriangleStrip);
                break;
            }
            pt_key.state.dynamic_state.primitive_restart_enable.Assign(1);
        }

        // Build modules and infos arrays for the passthrough pipeline.
        // Only VS (passthrough) and FS are needed.
        std::array<vk::ShaderModule, Maxwell::MaxShaderStage> pt_modules{};
        std::array<const Shader::Info*, Maxwell::MaxShaderStage> pt_infos{};

        // VS at stage index 0 (VertexB - 1 = 0)
        pt_modules[0] = std::move(pt_spv);
        pt_infos[0] = &passthrough_program.info;

        // Re-translate FS for the passthrough pipeline.
        const u32 fs_index = static_cast<u32>(Maxwell::ShaderType::Pixel);
        if (key.unique_hashes[fs_index] != 0) {
            // FS was already translated in programs[fs_index].
            auto& fs_program = programs[fs_index];
            const Shader::IR::Program* prev_stage = &passthrough_program;
            const auto runtime_info = MakeRuntimeInfo(programs, pt_key, fs_program, prev_stage);
            Shader::Maxwell::ConvertLegacyToGeneric(fs_program, runtime_info);
            // Use pt_binding which was advanced past VS bindings by EmitSPIRV.
            std::vector<u32> fs_code{
                Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, fs_program, pt_binding)};
            device.SaveShader(fs_code);
            VideoCommon::DumpSpirvShader(key.Hash(),
                key.unique_hashes[fs_index], Shader::Stage::Fragment, fs_code, "PT_FS");
            const u32 fs_stage_index = fs_index - 1; // Fragment stage index = 4
            pt_modules[fs_stage_index] = BuildShader(device, fs_code);
            {
                const auto name{fmt::format("VTG-Passthrough-FS {:016x}",
                    key.unique_hashes[fs_index])};
                pt_modules[fs_stage_index].SetObjectNameEXT(name.c_str());
            }
            pt_infos[fs_stage_index] = &fs_program.info;
        }

        auto passthrough_pipeline = std::make_unique<GraphicsPipeline>(
            scheduler, buffer_cache, texture_cache, vulkan_pipeline_cache, &shader_notify,
            device, descriptor_pool, guest_descriptor_queue, nullptr, nullptr,
            render_pass_cache, pt_key, std::move(pt_modules), pt_infos);

        vtg_set->passthrough_pipeline = passthrough_pipeline.get();

        // Store the passthrough pipeline in the graphics cache so it stays alive.
        graphics_cache.emplace(pt_key, std::move(passthrough_pipeline));

        LOG_INFO(Render_Vulkan, "Passthrough pipeline created for VTG-as-Compute");
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Failed to create passthrough pipeline: {}", e.what());
        // Continue without passthrough pipeline - will fall back to degraded draw.
    }

    auto* result = vtg_set.get();
    vtg_compute_cache.emplace(key, std::move(vtg_set));
    return result;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    const ComputePipelineCacheKey& key, const ShaderInfo* shader) {
    const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
    const auto& qmd{kepler_compute->launch_description};
    ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start};
    env.SetCachedSize(shader->size_bytes);

    main_pools.ReleaseContents();
    auto pipeline{CreateComputePipeline(main_pools, key, env, nullptr, true)};
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    serialization_thread.QueueWork([this, key, env_ = std::move(env)] {
        SerializePipeline(key, std::array<const GenericEnvironment*, 1>{&env_},
                          pipeline_cache_filename, CACHE_VERSION);
    });
    return pipeline;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
    PipelineStatistics* statistics, bool build_in_parallel) try {
    auto hash = key.Hash();
    if (device.HasBrokenCompute()) {
        LOG_ERROR(Render_Vulkan, "Skipping 0x{:016x}", hash);
        return nullptr;
    }

    LOG_INFO(Render_Vulkan, "0x{:016x}", hash);

    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};

    // Dump it before error.
    if (Settings::values.dump_shaders) {
        env.Dump(hash, key.unique_hash);
    }

    auto program{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};

    if (kepler_compute) {
        const auto& qmd{kepler_compute->launch_description};
        const auto& cbufs{qmd.const_buffer_config};
        const bool via_header_index{qmd.linked_tsc != 0};
        [[maybe_unused]]
        const auto read_handle{[&](const auto& desc, u32 index) {
            ASSERT(((qmd.const_buffer_enable_mask >> desc.cbuf_index) & 1) != 0);
            const u32 index_offset{index << desc.size_shift};
            const u32 offset{desc.cbuf_offset + index_offset};
            const GPUVAddr addr{cbufs[desc.cbuf_index].Address() + offset};
            if constexpr (std::is_same_v<decltype(desc), const Shader::TextureDescriptor&> ||
                          std::is_same_v<decltype(desc), const Shader::TextureBufferDescriptor&>) {
                if (desc.has_secondary) {
                    ASSERT(((qmd.const_buffer_enable_mask >> desc.secondary_cbuf_index) & 1) != 0);
                    const u32 secondary_offset{desc.secondary_cbuf_offset + index_offset};
                    const GPUVAddr separate_addr{cbufs[desc.secondary_cbuf_index].Address() +
                        secondary_offset};
                    const u32 lhs_raw{gpu_memory->Read<u32>(addr) << desc.shift_left};
                    const u32 rhs_raw{gpu_memory->Read<u32>(separate_addr) << desc.secondary_shift_left};
                    return GetHandleSecond(lhs_raw | rhs_raw, via_header_index);
                }
            }
            return GetHandleSecond(gpu_memory->Read<u32>(addr), via_header_index);
        }};
#ifdef __APPLE__
        for (const auto& desc : program.info.texture_descriptors) {
            for (u32 index = 0; index < desc.count; ++index) {
                const auto handle{read_handle(desc, index)};

                VideoCommon::SamplerId sampler = texture_cache.GetComputeSamplerId(handle);
                (void)sampler;
            }
        }
#endif
    }

    dbgscpHookEmitSpirv(hash, key.unique_hash, Shader::Stage::Compute);
    std::vector<u32> code{EmitSPIRV(profile, program)};
    device.SaveShader(code);
    VideoCommon::DumpSpirvShader(hash, key.unique_hash, Shader::Stage::Compute, code);
    auto&& it = replace_shaders.find(key.unique_hash);
    if (it != replace_shaders.end()) {
        auto&& it2 = it->second.find(Shader::Stage::Compute);
        if (it2 != it->second.end()) {
            code = it2->second;
        }
    }
    vk::ShaderModule spv_module{BuildShader(device, code)};
    {
        const auto name{fmt::format("Shader {:016x}", key.unique_hash)};
        spv_module.SetObjectNameEXT(name.c_str());
    }
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return std::make_unique<ComputePipeline>(device, vulkan_pipeline_cache, descriptor_pool,
                                             guest_descriptor_queue, thread_worker, statistics,
                                             &shader_notify, key, program.info, std::move(spv_module));

} catch (const Shader::Exception& exception) {
    std::stringstream ss;
    ss << "exception in compute pipeline: ";
    ss << fmt::format("{:016x} {}", key.unique_hash, exception.what());
    auto&& msg = ss.str();
    LOG_ERROR(Render_Vulkan, "{}", msg);
    Core::g_MainThreadCaller.RequestLogToView(std::move(msg));
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return nullptr;
}

void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache,
                                                 u32 cache_version) try {
    std::ofstream file(filename, std::ios::binary);
    file.exceptions(std::ifstream::failbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    file.write(VULKAN_CACHE_MAGIC_NUMBER.data(), VULKAN_CACHE_MAGIC_NUMBER.size())
        .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));

    size_t cache_size = 0;
    std::vector<char> cache_data;
    if (pipeline_cache) {
        pipeline_cache.Read(&cache_size, nullptr);
        cache_data.resize(cache_size);
        pipeline_cache.Read(&cache_size, cache_data.data());
    }
    file.write(cache_data.data(), cache_size);

    LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
             Common::FS::PathToUTF8String(filename));

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

vk::PipelineCache PipelineCache::LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                                         u32 expected_cache_version) {
    const auto create_pipeline_cache = [this](size_t data_size, const void* data) {
        VkPipelineCacheCreateInfo pipeline_cache_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .initialDataSize = data_size,
            .pInitialData = data};
        return device.GetLogical().CreatePipelineCache(pipeline_cache_ci);
    };
    try {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return create_pipeline_cache(0, nullptr);
        }
        file.exceptions(std::ifstream::failbit);
        const auto end{file.tellg()};
        file.seekg(0, std::ios::beg);

        std::array<char, 8> magic_number;
        u32 cache_version;
        file.read(magic_number.data(), magic_number.size())
            .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
        if (magic_number != VULKAN_CACHE_MAGIC_NUMBER || cache_version != expected_cache_version) {
            file.close();
            if (Common::FS::RemoveFile(filename)) {
                if (magic_number != VULKAN_CACHE_MAGIC_NUMBER) {
                    LOG_ERROR(Common_Filesystem, "Invalid Vulkan driver pipeline cache file");
                }
                if (cache_version != expected_cache_version) {
                    LOG_INFO(Common_Filesystem, "Deleting old Vulkan driver pipeline cache");
                }
            } else {
                LOG_ERROR(Common_Filesystem,
                          "Invalid Vulkan pipeline cache file and failed to delete it in \"{}\"",
                          Common::FS::PathToUTF8String(filename));
            }
            return create_pipeline_cache(0, nullptr);
        }

        static constexpr size_t header_size = magic_number.size() + sizeof(cache_version);
        const size_t cache_size = static_cast<size_t>(end) - header_size;
        std::vector<char> cache_data(cache_size);
        file.read(cache_data.data(), cache_size);

        LOG_INFO(Render_Vulkan,
                 "Loaded Vulkan driver pipeline cache: ", Common::FS::PathToUTF8String(filename));

        return create_pipeline_cache(cache_size, cache_data.data());

    } catch (const std::ios_base::failure& e) {
        LOG_ERROR(Common_Filesystem, "{}", e.what());
        if (!Common::FS::RemoveFile(filename)) {
            LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                      Common::FS::PathToUTF8String(filename));
        }

        return create_pipeline_cache(0, nullptr);
    }
}

} // namespace Vulkan
