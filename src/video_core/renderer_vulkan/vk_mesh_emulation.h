// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

/// Task and mesh programs of Switch 2 titles on devices without VK_EXT_mesh_shader (see
/// MESH_EMULATION.md). They run as compute shaders that exchange data through the buffers this
/// class owns; the draw is then an indirect draw with a generated vertex shader. All emulated
/// draws share the buffers, so each one starts with a barrier against the previous.
class MeshEmulation {
public:
    static constexpr u32 MAX_TASK_GROUPS = 4096;
    static constexpr u32 MAX_PAYLOAD_WORDS = 800; ///< largest task record seen in Ocarina of Time
    static constexpr u32 MAX_MESH_GROUPS = 4096;
    static constexpr VkDeviceSize SLOT_BUFFER_BYTES = VkDeviceSize{96} << 20;
    /// vkCmdDrawIndirect arguments inside the arguments buffer (the dispatch ones are at 0).
    static constexpr VkDeviceSize DRAW_ARGUMENTS_OFFSET = 16;
    /// The draw count of the one-draw-per-meshlet path, and its commands (16 bytes each).
    static constexpr VkDeviceSize DRAW_COUNT_OFFSET = 32;
    static constexpr VkDeviceSize MESHLET_COMMANDS_OFFSET = 64;
    static constexpr VkDeviceSize ARGUMENTS_BYTES = MESHLET_COMMANDS_OFFSET + VkDeviceSize{MAX_MESH_GROUPS} * 16;

    /// EDEN_NX2_MESH_EMU is set (read once).
    [[nodiscard]] static bool Enabled();
    /// EDEN_NX2_MESH_EMU=force: emulate even where VK_EXT_mesh_shader exists (to compare).
    [[nodiscard]] static bool Forced();

    struct Decision {
        bool run{true};          ///< run the task, scan and mesh passes of this pipeline
        bool draw{true};         ///< and draw what they wrote
        bool per_meshlet{false}; ///< one draw command per mesh workgroup, so gl_PrimitiveID
                                 ///< restarts in each, as with mesh shaders
        bool no_depth{false};    ///< depth compare ALWAYS for draws that write colour (diagnostic)
        bool depth_leq{false};   ///< test depth with LESS_OR_EQUAL where the guest uses EQUAL
        bool mask_pos{false};    ///< no color writes in position-only pipelines (a depth prepass)
        bool reassert{true};     ///< record the guest's color write state again before the draw
        bool skip_shadow{false}; ///< skip draws into a shadow map (no color target, small viewport)
        bool skip_half{false};   ///< skip draws into the half resolution target
        bool skip_prepass{false}; ///< skip the main view's depth prepass draws
    };
    /// What to do with the emulated draws of the mesh program `mesh_hash`. With
    /// EDEN_NX2_MESH_EMU_FILTER=<file>, the file lists the hex hashes that run ("none" or an empty
    /// list: none, "all" or no file: all) and may contain "nodraw" to run the passes without
    /// drawing, "permeshlet" to draw one command per mesh workgroup and "depthleq" to relax an EQUAL
    /// depth test (a prepass whose depth the shaded pass does not reproduce bit for bit); it is
    /// "maskpos" to turn color writes off in position-only pipelines (the guest masks them) and
    /// "noreassert" to not record the guest's color write state again before each draw;
    /// "skipshadow", "skiphalf" and "skipprepass" to leave a class of passes out and see what each
    /// one does to the image;
    /// read again every few hundred draws, so a running game can be bisected.
    [[nodiscard]] static Decision Decide(u64 mesh_hash);

    /// EDEN_NX2_MESH_EMU_DUMP_DIR: the first draws of each pipeline write their buffers there.
    [[nodiscard]] static const char* DumpDirectory();

    explicit MeshEmulation(const Device& device, MemoryAllocator& memory_allocator);
    ~MeshEmulation();

    MeshEmulation(const MeshEmulation&) = delete;
    MeshEmulation& operator=(const MeshEmulation&) = delete;

    /// Descriptor set 1 of every emulated pipeline layout: the task records, the mesh group map
    /// and the mesh output slots.
    [[nodiscard]] VkDescriptorSetLayout SetLayout() const noexcept {
        return *set_layout;
    }
    [[nodiscard]] VkDescriptorSet Set() const noexcept {
        return emulation_set;
    }
    [[nodiscard]] VkBuffer Arguments() const noexcept {
        return *arguments;
    }

    /// How many mesh workgroups the slot buffer holds for a program with `slot_words` words per
    /// slot.
    [[nodiscard]] u32 MaxMeshGroups(u32 slot_words) const noexcept;

    struct Draw {
        VkPipelineLayout layout;  ///< layout of the emulated pipeline (set 0 + MeshEmulation::Set)
        VkPipeline task;
        VkPipeline mesh;
        VkDescriptorSet stage_set; ///< set 0, with the guest's descriptors
        u32 task_groups;
        u32 payload_words;
        u32 slot_words;
        u32 max_primitives;
        u32 primitive_vertices; ///< 1 points, 2 lines, 3 triangles
    };

    /// Records, outside a render pass: task workgroups, the scan that numbers the mesh
    /// workgroups, and the indirect mesh dispatch. Leaves the buffers readable by the vertex
    /// shader and by vkCmdDrawIndirect.
    void RecordPasses(vk::CommandBuffer cmdbuf, const Draw& draw, bool per_meshlet) const;

    // --- debugging dump of the buffers of one draw (EDEN_NX2_MESH_EMU_DUMP_DIR) ---
    static constexpr u32 DUMP_SLOTS = 16;
    static constexpr VkDeviceSize DUMP_MAP_OFFSET = 64;
    static constexpr VkDeviceSize DUMP_MAP_BYTES = 4096;
    static constexpr VkDeviceSize DUMP_SLOT_OFFSET = 8192;
    static constexpr VkDeviceSize DUMP_SLOT_BYTES = 320 * 1024;
    static constexpr VkDeviceSize DUMP_TASK_OFFSET = DUMP_SLOT_OFFSET + DUMP_SLOT_BYTES;
    static constexpr VkDeviceSize DUMP_TASK_BYTES = 64 * 1024;
    static constexpr VkDeviceSize DUMP_BYTES = DUMP_TASK_OFFSET + DUMP_TASK_BYTES;

    [[nodiscard]] bool DumpEnabled() const noexcept {
        return static_cast<bool>(readback);
    }
    /// Copies the arguments, the mesh map, the first slots and the first task records to the
    /// readback buffer, after RecordPasses.
    void RecordDump(vk::CommandBuffer cmdbuf, u32 payload_words, u32 slot_words) const;

    struct DumpInfo {
        u64 task_hash;
        u64 mesh_hash;
        u32 task_groups;
        u32 payload_words;
        u32 slot_words;
        u32 max_vertices;
        u32 max_primitives;
        u32 attribute_base;
        std::span<const u32> vertex_lines;
    };
    /// Writes the readback buffer and its description. Call after the work was finished.
    void WriteDump(const DumpInfo& info);

    u32 dumps_taken{};

private:
    const Device& device;
    vk::Buffer task_records;
    vk::Buffer mesh_map;
    vk::Buffer mesh_slots;
    vk::Buffer arguments;
    vk::Buffer readback; ///< only with EDEN_NX2_MESH_EMU_DUMP_DIR
    vk::DescriptorSetLayout set_layout;
    vk::DescriptorPool pool;
    VkDescriptorSet emulation_set{};

    vk::DescriptorSetLayout scan_set_layout;
    vk::PipelineLayout scan_layout;
    vk::ShaderModule scan_module;
    vk::Pipeline scan_pipeline;
    VkDescriptorSet scan_set{};
    vk::DescriptorSetLayout finalize_set_layout;
    vk::PipelineLayout finalize_layout;
    vk::ShaderModule finalize_module;
    vk::Pipeline finalize_pipeline;
    VkDescriptorSet finalize_set{};
    vk::DescriptorSets sets;
};

} // namespace Vulkan
