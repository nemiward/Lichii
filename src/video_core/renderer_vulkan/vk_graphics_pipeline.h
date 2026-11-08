// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/pipeline_build.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    FixedPipelineState state;

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    size_t Size() const noexcept {
        return sizeof(unique_hashes) + state.Size();
    }
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::GraphicsPipelineCacheKey> {
    size_t operator()(const Vulkan::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace Vulkan {

class Device;
class PipelineStatistics;
class RenderPassCache;
class RescalingPushConstant;
class RenderAreaPushConstant;
class Scheduler;

/// Mesh emulation on a device without VK_EXT_mesh_shader: the mesh program (stage 0) is built as
/// a compute shader that writes its output to a storage buffer, and this vertex shader draws it
/// (see Shader::Backend::SPIRV::EmitMeshEmulationVertex).
struct MeshEmulationSetup {
    std::vector<u32> vertex_spirv;
    u32 output_binding{};
    u32 block_words{};    ///< output words per mesh workgroup
    u32 max_primitives{}; ///< triangles per mesh workgroup
    // With a task program (stage 0, the mesh program at mesh_stage): its payload buffer and the
    // distribution pass's offsets and arguments (Shader::RuntimeInfo), and that pass's shader.
    bool has_task{};
    u32 mesh_stage{};
    u32 payload_stride{};
    u32 payload_binding{};
    u32 offsets_binding{};
    u32 args_binding{};
    std::vector<u32> distribution_spirv;
    // Output layout, for the readback diagnostics.
    u32 max_vertices{};
    u32 vertex_lines{};
    std::array<u32, 4> position_lines{~0U, ~0U, ~0U, ~0U}; ///< line of PositionX..W
};

class MeshEmulation;

/// A task/mesh pipeline run without VK_EXT_mesh_shader (see MESH_EMULATION.md): the task and mesh
/// stages become compute pipelines and the draw is an indirect draw of `vertex_module`.
struct EmulatedMeshStages {
    MeshEmulation* system{};
    vk::ShaderModule vertex_module; ///< draws what the mesh program wrote to its output slots
    bool has_task{true};            ///< false: a mesh program alone, drawn with its own count
    bool position_only{};           ///< writes only the position and its fragment shader has no textures
    u32 task_stage{};               ///< indices of the task and mesh modules in the stage array
    u32 mesh_stage{};
    u64 task_hash{};
    u64 mesh_hash{};
    u32 payload_words{};
    u32 slot_words{};
    u32 max_primitives{};
    u32 primitive_vertices{3}; ///< 1 points, 2 lines, 3 triangles (MeshInfo::PrimitiveVertices)

    /// The mesh program's primitives as a list, whatever topology the guest set.
    [[nodiscard]] VkPrimitiveTopology Topology() const noexcept {
        return primitive_vertices == 1   ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
               : primitive_vertices == 2 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                         : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
    u32 max_vertices{};
    u32 attribute_base{};
    std::vector<u32> vertex_lines;
    bool task_warp_ops{}; ///< run the compute pipeline at the guest's warp size
    bool mesh_warp_ops{};
    u32 texture_logs{};   ///< diagnostic logs of the fragment shader textures
    u32 draws{};          ///< emulated draws so far (logged at powers of two)
    u32 dumps{};          ///< buffer dumps taken (EDEN_NX2_MESH_EMU_DUMP_DIR)
    std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> push_rescaling{};
    f32 push_scale_down_factor{1.0f};
    bool push_uses_render_area{};
    std::array<f32, 4> push_render_area{};
};

class GraphicsPipeline {
    static constexpr size_t NUM_STAGES = Tegra::Engines::Maxwell3D::Regs::MaxShaderStage;

public:
    explicit GraphicsPipeline(
        Scheduler& scheduler, BufferCache& buffer_cache, TextureCache& texture_cache,
        vk::PipelineCache& pipeline_cache, VideoCore::ShaderNotify* shader_notify,
        const Device& device, DescriptorPool& descriptor_pool,
        GuestDescriptorQueue& guest_descriptor_queue,
        DescriptorBufferRing& descriptor_buffer_ring, Common::ThreadWorker* worker_thread,
        PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
        const GraphicsPipelineCacheKey& key, std::array<vk::ShaderModule, NUM_STAGES> stages,
        const std::array<const Shader::Info*, NUM_STAGES>& infos,
        std::function<std::array<vk::ShaderModule, NUM_STAGES>()> emit_stages = {},
        std::optional<MeshEmulationSetup> mesh_emulation = std::nullopt,
        std::unique_ptr<EmulatedMeshStages> emulated_mesh_stages = {});

    static std::chrono::nanoseconds TakeHoldTime() noexcept;

    bool HasDynamicVertexInput() const noexcept { return key.state.dynamic_vertex_input; }
    const std::array<u64, 6>& ShaderHashes() const noexcept {
        return key.unique_hashes;
    }

    struct SampledImage {
        u32 stage;
        u32 tic;
        VideoCommon::ImageViewId id;
        VideoCommon::SamplerId sampler;
    };

    /// The sampled textures of the last configured draw, kept while a sampled frame dump runs.
    std::span<const SampledImage> SampledImages() const noexcept {
        return sampled_images;
    }

    bool SupportsAlphaToCoverage() const noexcept {
        return fragment_has_color0_output;
    }

    bool SupportsAlphaToOne() const noexcept {
        return fragment_has_color0_output;
    }

    /// Color components of a render target the fragment shader writes, as VkColorComponentFlags
    VkColorComponentFlags ColorOutputMask(size_t render_target) const noexcept {
        return (color_output_mask >> (render_target * 4)) & 0xF;
    }

    /// Color components the fragment shader writes, 4 bits per render target
    u32 ColorOutputMask() const noexcept {
        return color_output_mask;
    }

    bool UsesExtendedDynamicState() const noexcept {
        return key.state.extended_dynamic_state != 0;
    }

    /// Switch 2 task/mesh pipeline: drawn with vkCmdDrawMeshTasksEXT.
    [[nodiscard]] bool IsMeshPipeline() const noexcept {
        return !emulated_mesh && (stage_infos[0].is_task || stage_infos[0].is_mesh);
    }

    /// Mesh pipeline drawn through the compute emulation (MeshEmulationSetup).
    [[nodiscard]] bool IsEmulatedMesh() const noexcept {
        return mesh_emulation.has_value();
    }

    /// Output bytes the emulation needs for one mesh workgroup.
    [[nodiscard]] u64 MeshEmulationBlockBytes() const noexcept {
        return mesh_emulation ? u64{mesh_emulation->block_words} * 4 : 0;
    }

    /// Vertices of the emulation's triangle list for one mesh workgroup.
    [[nodiscard]] u32 MeshEmulationVerticesPerGroup() const noexcept {
        return mesh_emulation ? mesh_emulation->max_primitives * 3 : 0;
    }

    /// The emulation's setup (output layout), or null.
    [[nodiscard]] const MeshEmulationSetup* MeshEmulation() const noexcept {
        return mesh_emulation ? &*mesh_emulation : nullptr;
    }

    /// The emulation's output buffer and workgroup count for the next Configure.
    void SetMeshEmulationOutput(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                                u32 groups) noexcept {
        mesh_output_buffer = buffer;
        mesh_output_offset = offset;
        mesh_output_size = size;
        mesh_groups = groups;
    }

    struct MeshEmulationBuffer {
        VkBuffer buffer{};
        VkDeviceSize offset{};
        VkDeviceSize size{};
    };
    /// With a task program: the task payload, offset and argument buffers, the arena counter,
    /// and the words of the output arena (the draw's group count is the task's).
    void SetMeshEmulationTaskBuffers(const MeshEmulationBuffer& payload,
                                     const MeshEmulationBuffer& offsets,
                                     const MeshEmulationBuffer& args,
                                     const MeshEmulationBuffer& counter,
                                     u32 capacity_words) noexcept {
        mesh_task_buffers = {payload, offsets, args, counter};
        mesh_capacity = capacity_words;
    }

    [[nodiscard]] bool MeshEmulationHasTask() const noexcept {
        return mesh_emulation && mesh_emulation->has_task;
    }

    [[nodiscard]] u32 MeshEmulationPayloadStride() const noexcept {
        return mesh_emulation ? mesh_emulation->payload_stride : 0;
    }

    /// Task/mesh pipeline run as compute passes plus an indirect draw (EmulatedMeshStages; not
    /// the MeshEmulationSetup path above).
    [[nodiscard]] bool IsEmulatedMeshStages() const noexcept {
        return emulated_mesh != nullptr;
    }

    /// Records the emulated task, scan and mesh passes and the draw of what they wrote. Call
    /// after Configure, in place of vkCmdDrawMeshTasksEXT. `task_groups` is the draw's count.
    /// after_renderpass runs once the render pass is open again, before the draw is recorded, to
    /// restore the per-draw state that closing the render pass reset (occlusion queries).
    void DrawEmulatedMesh(u32 task_groups, const std::function<void()>& after_renderpass = {});
    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = delete;
    GraphicsPipeline(GraphicsPipeline&&) noexcept = delete;

    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(const GraphicsPipeline&) = delete;

    void AddTransition(GraphicsPipeline* transition);

    bool Configure(bool is_indexed) {
        return configure_func(this, is_indexed);
    }

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key) {
            return this;
        }
        const auto it{std::find(transition_keys.begin(), transition_keys.end(), current_key)};
        return it != transition_keys.end() ? transitions[std::distance(transition_keys.begin(), it)]
                                           : nullptr;
    }

    /// The frame the pipeline cache last handed this pipeline out (evicts long unused ones).
    void MarkUsed(u64 frame) noexcept {
        last_use_frame = frame;
        used = true;
    }

    [[nodiscard]] u64 LastUse() const noexcept {
        return last_use_frame;
    }

    /// Whether the cache has handed this pipeline out at all (frame 0 included), as opposed to a
    /// pipeline prebuilt from the disk cache that no draw has used yet.
    [[nodiscard]] bool WasUsed() const noexcept {
        return used;
    }

    /// Forgets the transitions, which may point to pipelines the cache evicted.
    void ClearTransitions() noexcept {
        transition_keys.clear();
        transitions.clear();
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return build->IsBuilt();
    }

    /// Waits up to `timeout` for the build; true when built.
    [[nodiscard]] bool WaitBuilt(std::chrono::microseconds timeout) const {
        return build->WaitFor(timeout);
    }

    [[nodiscard]] bool Failed() const noexcept {
        return build->Failed();
    }

    template <typename Spec>
    static auto MakeConfigureSpecFunc() {
        return [](GraphicsPipeline* pl, bool is_indexed) { return pl->ConfigureImpl<Spec>(is_indexed); };
    }

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

private:
    template <typename Spec>
    bool ConfigureImpl(bool is_indexed);

    bool ConfigureDraw(const RescalingPushConstant& rescaling,
                       const RenderAreaPushConstant& render_are);

    void MakePipeline(VkRenderPass render_pass);

    void Validate();

    const GraphicsPipelineCacheKey key;
    Tegra::Engines::Maxwell3D* maxwell3d;
    Tegra::MemoryManager* gpu_memory;
    const Device& device;
    TextureCache& texture_cache;
    BufferCache& buffer_cache;
    vk::PipelineCache& pipeline_cache;
    Scheduler& scheduler;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;

    bool (*configure_func)(GraphicsPipeline*, bool){};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;
    u64 last_use_frame{};
    bool used{};

    std::array<vk::ShaderModule, NUM_STAGES> spv_modules;

    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::array<u32, 5> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    u32 num_descriptor_entries{};
    size_t num_image_elements{};
    std::vector<SampledImage> sampled_images;
    u32 num_textures{};
    bool fragment_has_color0_output{};
    u32 color_output_mask{};
    bool writes_storage{};

    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;
    std::unique_ptr<EmulatedMeshStages> emulated_mesh;
    vk::Pipeline emulated_task_pipeline;
    vk::Pipeline emulated_mesh_pipeline;
    VkDescriptorSet emulated_stage_set{}; ///< set 0 of the last configured draw

    DescriptorBufferLayout descriptor_buffer_layout;
    std::vector<DescriptorUpdateEntry> last_descriptor_payload;
    VkDeviceSize last_descriptor_buffer_offset{};
    u32 last_descriptor_buffer_chunk{};
    u64 last_descriptor_buffer_generation{};

    const bool worker_only_build;
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};

    // Mesh emulation (MeshEmulationSetup).
    std::optional<MeshEmulationSetup> mesh_emulation;
    vk::ShaderModule mesh_vertex_module;
    vk::Pipeline mesh_compute_pipeline;
    VkDescriptorSet mesh_descriptor_set{}; ///< shared by the dispatch and the draw it feeds
    VkBuffer mesh_output_buffer{};
    VkDeviceSize mesh_output_offset{};
    VkDeviceSize mesh_output_size{};
    u32 mesh_groups{};
    // With a task program: its compute pipeline, the distribution pass between it and the mesh
    // dispatch, and the per-draw buffers (payload, offsets, arguments, arena counter).
    vk::Pipeline task_compute_pipeline;
    vk::ShaderModule distribution_module;
    vk::DescriptorSetLayout distribution_set_layout;
    vk::PipelineLayout distribution_layout;
    vk::Pipeline distribution_pipeline;
    DescriptorAllocator distribution_allocator;
    std::array<MeshEmulationBuffer, 4> mesh_task_buffers{};
    u32 mesh_capacity{};

    std::optional<PipelineBuild> build;
};

} // namespace Vulkan
