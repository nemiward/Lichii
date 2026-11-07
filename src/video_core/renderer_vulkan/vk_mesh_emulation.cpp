// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <span>
#include <string_view>

#include "common/logging.h"
#include "video_core/host_shaders/mesh_emulation_finalize_comp_spv.h"
#include "video_core/host_shaders/mesh_emulation_scan_comp_spv.h"
#include "video_core/renderer_vulkan/vk_mesh_emulation.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
constexpr VkShaderStageFlags EMULATION_STAGES =
    VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;

struct FinalizeParameters {
    u32 slot_words;
    u32 max_primitives;
};

struct ScanParameters {
    u32 task_groups;
    u32 record_words;
    u32 max_mesh_groups;
    u32 vertices_per_group;
};

vk::Buffer MakeStorageBuffer(MemoryAllocator& memory_allocator, VkDeviceSize size) {
    return memory_allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        },
        MemoryUsage::DeviceLocal);
}

void Barrier(vk::CommandBuffer cmdbuf, VkPipelineStageFlags src_stage, VkAccessFlags src_access,
             VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    const VkMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
    };
    cmdbuf.PipelineBarrier(src_stage, dst_stage, 0, barrier);
}
} // Anonymous namespace

bool MeshEmulation::Enabled() {
    static const bool enabled{std::getenv("EDEN_NX2_MESH_EMU") != nullptr};
    return enabled;
}

MeshEmulation::Decision MeshEmulation::Decide(u64 mesh_hash) {
    static const char* const path{std::getenv("EDEN_NX2_MESH_EMU_FILTER")};
    if (path == nullptr) {
        return {};
    }
    static std::mutex mutex;
    static std::set<u64> allowed;
    static bool all{true};
    static bool no_draw{};
    static bool per_meshlet{};
    static bool depth_leq{};
    static bool mask_pos{};
    static bool reassert{true};
    static bool skip_shadow{}, skip_half{}, skip_prepass{};
    static u32 calls{};
    std::scoped_lock lock{mutex};
    if (calls++ % 256 == 0) {
        std::ifstream file{path};
        all = !file.is_open();
        no_draw = false;
        per_meshlet = false;
        depth_leq = false;
        mask_pos = false;
        reassert = true;
        skip_shadow = skip_half = skip_prepass = false;
        allowed.clear();
        std::string token;
        while (file >> token) {
            if (token == "all") {
                all = true;
            } else if (token == "nodraw") {
                no_draw = true;
            } else if (token == "permeshlet") {
                per_meshlet = true;
            } else if (token == "depthleq") {
                depth_leq = true;
            } else if (token == "maskpos") {
                mask_pos = true;
            } else if (token == "noreassert") {
                reassert = false;
            } else if (token == "skipshadow") {
                skip_shadow = true;
            } else if (token == "skiphalf") {
                skip_half = true;
            } else if (token == "skipprepass") {
                skip_prepass = true;
            } else if (token != "none") {
                allowed.insert(std::strtoull(token.c_str(), nullptr, 16));
            }
        }
    }
    return {.run = all || allowed.contains(mesh_hash), .draw = !no_draw,
            .per_meshlet = per_meshlet, .depth_leq = depth_leq,
            .mask_pos = mask_pos, .reassert = reassert,
            .skip_shadow = skip_shadow, .skip_half = skip_half,
            .skip_prepass = skip_prepass};
}

const char* MeshEmulation::DumpDirectory() {
    static const char* const directory{std::getenv("EDEN_NX2_MESH_EMU_DUMP_DIR")};
    return directory;
}

bool MeshEmulation::Forced() {
    static const bool forced{[] {
        const char* const value{std::getenv("EDEN_NX2_MESH_EMU")};
        return value != nullptr && std::string_view{value} == "force";
    }()};
    return forced;
}

MeshEmulation::MeshEmulation(const Device& device_, MemoryAllocator& memory_allocator)
    : device{device_} {
    const vk::Device& logical{device.GetLogical()};
    task_records = MakeStorageBuffer(
        memory_allocator, VkDeviceSize{MAX_TASK_GROUPS} * MAX_PAYLOAD_WORDS * sizeof(u32));
    mesh_map = MakeStorageBuffer(memory_allocator, VkDeviceSize{MAX_MESH_GROUPS} * 2 * sizeof(u32));
    mesh_slots = MakeStorageBuffer(memory_allocator, SLOT_BUFFER_BYTES);
    arguments = MakeStorageBuffer(memory_allocator, ARGUMENTS_BYTES);
    if (DumpDirectory() != nullptr) {
        readback = memory_allocator.CreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = DUMP_BYTES,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Download);
    }

    const auto storage_bindings = [](VkShaderStageFlags stages) {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (u32 index = 0; index < bindings.size(); ++index) {
            bindings[index] = {
                .binding = index,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = stages,
                .pImmutableSamplers = nullptr,
            };
        }
        return bindings;
    };
    const auto emulation_bindings{storage_bindings(EMULATION_STAGES)};
    set_layout = logical.CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = static_cast<u32>(emulation_bindings.size()),
        .pBindings = emulation_bindings.data(),
    });
    const auto scan_bindings{storage_bindings(VK_SHADER_STAGE_COMPUTE_BIT)};
    scan_set_layout = logical.CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = static_cast<u32>(scan_bindings.size()),
        .pBindings = scan_bindings.data(),
    });

    const std::array<VkDescriptorSetLayoutBinding, 2> finalize_bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    finalize_set_layout = logical.CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = static_cast<u32>(finalize_bindings.size()),
        .pBindings = finalize_bindings.data(),
    });

    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8};
    pool = logical.CreateDescriptorPool({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = 3,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    const std::array<VkDescriptorSetLayout, 3> layouts{*set_layout, *scan_set_layout,
                                                       *finalize_set_layout};
    sets = pool.Allocate({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = *pool,
        .descriptorSetCount = static_cast<u32>(layouts.size()),
        .pSetLayouts = layouts.data(),
    });
    emulation_set = sets[0];
    scan_set = sets[1];
    finalize_set = sets[2];

    // Emulation set: records, map, slots. Scan set: records, map, arguments.
    const std::array<VkDescriptorBufferInfo, 4> infos{{
        {*task_records, 0, VK_WHOLE_SIZE},
        {*mesh_map, 0, VK_WHOLE_SIZE},
        {*mesh_slots, 0, VK_WHOLE_SIZE},
        {*arguments, 0, VK_WHOLE_SIZE},
    }};
    const auto write = [&](VkDescriptorSet set, u32 binding, const VkDescriptorBufferInfo& info) {
        return VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = set,
            .dstBinding = binding,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pImageInfo = nullptr,
            .pBufferInfo = &info,
            .pTexelBufferView = nullptr,
        };
    };
    const std::array<VkWriteDescriptorSet, 8> writes{
        write(emulation_set, 0, infos[0]), write(emulation_set, 1, infos[1]),
        write(emulation_set, 2, infos[2]), write(scan_set, 0, infos[0]),
        write(scan_set, 1, infos[1]),      write(scan_set, 2, infos[3]),
        write(finalize_set, 0, infos[2]),  write(finalize_set, 1, infos[3]),
    };
    logical.UpdateDescriptorSets(writes, {});

    const VkPushConstantRange push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(ScanParameters),
    };
    scan_layout = logical.CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = scan_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });
    const std::span<const u32> code{MESH_EMULATION_SCAN_COMP_SPV};
    scan_module = logical.CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = static_cast<u32>(code.size_bytes()),
        .pCode = code.data(),
    });
    scan_pipeline = logical.CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *scan_module,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        .layout = *scan_layout,
        .basePipelineHandle = 0,
        .basePipelineIndex = 0,
    });
    const VkPushConstantRange finalize_push{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(FinalizeParameters),
    };
    finalize_layout = logical.CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = finalize_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &finalize_push,
    });
    const std::span<const u32> finalize_code{MESH_EMULATION_FINALIZE_COMP_SPV};
    finalize_module = logical.CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = static_cast<u32>(finalize_code.size_bytes()),
        .pCode = finalize_code.data(),
    });
    finalize_pipeline = logical.CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *finalize_module,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        .layout = *finalize_layout,
        .basePipelineHandle = 0,
        .basePipelineIndex = 0,
    });
    LOG_INFO(Render_Vulkan,
             "Mesh emulation ready: up to {} task and {} mesh workgroups per draw, {} MiB of slots",
             MAX_TASK_GROUPS, MAX_MESH_GROUPS, SLOT_BUFFER_BYTES >> 20);
}

MeshEmulation::~MeshEmulation() = default;

u32 MeshEmulation::MaxMeshGroups(u32 slot_words) const noexcept {
    if (slot_words == 0) {
        return 0;
    }
    const VkDeviceSize fit{SLOT_BUFFER_BYTES / (VkDeviceSize{slot_words} * sizeof(u32))};
    return static_cast<u32>(std::min<VkDeviceSize>(fit, MAX_MESH_GROUPS));
}

void MeshEmulation::RecordPasses(vk::CommandBuffer cmdbuf, const Draw& draw,
                                 bool per_meshlet) const {
    const u32 task_groups{std::min(draw.task_groups, MAX_TASK_GROUPS)};
    const ScanParameters scan_parameters{
        .task_groups = task_groups,
        // 0 tells the scan there is no task program: task_groups already counts mesh workgroups.
        .record_words = draw.task != VkPipeline{} ? draw.payload_words : 0,
        .max_mesh_groups = MaxMeshGroups(draw.slot_words),
        .vertices_per_group = draw.max_primitives * 3,
    };
    const std::array<VkDescriptorSet, 2> sets{draw.stage_set, emulation_set};

    // Everything recorded before: the guest's uploads and writes the programs read, and the
    // previous emulated draw's reads of the buffers this one overwrites.
    Barrier(cmdbuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

    if (draw.task != VkPipeline{}) {
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, draw.task);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, draw.layout, 0, sets, {});
        cmdbuf.Dispatch(task_groups, 1, 1);
    } else {
        // No task program: the mesh programs read an empty payload, the one record the scan maps
        // every mesh workgroup to.
        cmdbuf.FillBuffer(*task_records, 0, VkDeviceSize{draw.payload_words} * sizeof(u32), 0);
    }
    Barrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *scan_pipeline);
    cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *scan_layout, 0, scan_set, {});
    cmdbuf.PushConstants(*scan_layout, VK_SHADER_STAGE_COMPUTE_BIT, scan_parameters);
    cmdbuf.Dispatch(1, 1, 1);
    Barrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);

    // The scan's layout replaced the sets bound for the task.
    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, draw.mesh);
    cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, draw.layout, 0, sets, {});
    cmdbuf.DispatchIndirect(*arguments, 0);
    if (per_meshlet) {
        // One draw command per mesh workgroup, from the primitive counts the mesh programs wrote.
        Barrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        const FinalizeParameters finalize_parameters{
            .slot_words = draw.slot_words,
            .max_primitives = draw.max_primitives,
        };
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *finalize_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *finalize_layout, 0,
                                  finalize_set, {});
        cmdbuf.PushConstants(*finalize_layout, VK_SHADER_STAGE_COMPUTE_BIT, finalize_parameters);
        cmdbuf.Dispatch(1, 1, 1);
    }
    Barrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
}

void MeshEmulation::RecordDump(vk::CommandBuffer cmdbuf, u32 payload_words, u32 slot_words) const {
    if (!readback) {
        return;
    }
    Barrier(cmdbuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const auto capped = [](VkDeviceSize words, VkDeviceSize cap) {
        return std::min<VkDeviceSize>(words * sizeof(u32), cap);
    };
    cmdbuf.CopyBuffer(*arguments, *readback, VkBufferCopy{0, 0, 8 * sizeof(u32)});
    cmdbuf.CopyBuffer(*mesh_map, *readback, VkBufferCopy{0, DUMP_MAP_OFFSET, DUMP_MAP_BYTES});
    cmdbuf.CopyBuffer(*mesh_slots, *readback,
                      VkBufferCopy{0, DUMP_SLOT_OFFSET,
                                   capped(VkDeviceSize{DUMP_SLOTS} * slot_words, DUMP_SLOT_BYTES)});
    cmdbuf.CopyBuffer(*task_records, *readback,
                      VkBufferCopy{0, DUMP_TASK_OFFSET,
                                   capped(VkDeviceSize{16} * payload_words, DUMP_TASK_BYTES)});
    Barrier(cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
}

void MeshEmulation::WriteDump(const DumpInfo& info) {
    const char* const directory{DumpDirectory()};
    if (!readback || directory == nullptr) {
        return;
    }
    readback.Invalidate();
    const std::span<u8> data{readback.Mapped()};
    const auto base{std::filesystem::path{directory} /
                    fmt::format("emu-{:016x}-{}", info.mesh_hash, dumps_taken++)};
    {
        std::ofstream file{std::filesystem::path{base}.concat(".bin"), std::ios::binary};
        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(std::min<size_t>(data.size(), DUMP_BYTES)));
    }
    std::string lines;
    for (const u32 line : info.vertex_lines) {
        lines += fmt::format("{}{}", lines.empty() ? "" : ",", line);
    }
    std::ofstream json{std::filesystem::path{base}.concat(".json")};
    json << fmt::format(
        "{{\"task_hash\":\"{:016x}\",\"mesh_hash\":\"{:016x}\",\"task_groups\":{},"
        "\"payload_words\":{},\"slot_words\":{},\"max_vertices\":{},\"max_primitives\":{},"
        "\"attribute_base\":{},\"vertex_lines\":[{}],\"dump_slots\":{},"
        "\"map_offset\":{},\"map_bytes\":{},\"slot_offset\":{},\"slot_bytes\":{},"
        "\"task_offset\":{},\"task_bytes\":{}}}\n",
        info.task_hash, info.mesh_hash, info.task_groups, info.payload_words, info.slot_words,
        info.max_vertices, info.max_primitives, info.attribute_base, lines, DUMP_SLOTS,
        DUMP_MAP_OFFSET, DUMP_MAP_BYTES, DUMP_SLOT_OFFSET, DUMP_SLOT_BYTES, DUMP_TASK_OFFSET,
        DUMP_TASK_BYTES);
    LOG_INFO(Render_Vulkan, "Mesh emulation dump written: {}", base.string());
}

} // namespace Vulkan
