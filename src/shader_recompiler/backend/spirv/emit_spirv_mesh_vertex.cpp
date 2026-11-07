// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <map>
#include <span>

#include <sirit/sirit.h>

#include "shader_recompiler/backend/spirv/emit_spirv_mesh_vertex.h"
#include "shader_recompiler/frontend/ir/attribute.h"

namespace Shader::Backend::SPIRV {
namespace {
using Id = Sirit::Id;

constexpr u32 SPIRV_VERSION_1_3 = 0x00010300;

class VertexBuilder : public Sirit::Module {
public:
    explicit VertexBuilder(const MeshVertexParams& params_)
        : Sirit::Module(SPIRV_VERSION_1_3), params{params_}, mesh{params_.mesh} {}

    std::vector<u32> Build() {
        AddCapability(spv::Capability::Shader);
        DefineTypes();
        DefineInterface();
        DefineMain();
        return Assemble();
    }

private:
    Id U(u32 value) {
        return Constant(u32_type, value);
    }

    Id F(float value) {
        return Constant(f32_type, value);
    }

    void DefineTypes() {
        bool_type = TypeBool();
        u32_type = TypeInt(32, false);
        f32_type = TypeFloat(32);
        vec4_type = TypeVector(f32_type, 4);
        const Id words_type{TypeRuntimeArray(u32_type)};
        Decorate(words_type, spv::Decoration::ArrayStride, 4U);
        const Id block_type{TypeStruct(words_type)};
        Decorate(block_type, spv::Decoration::Block);
        MemberDecorate(block_type, 0U, spv::Decoration::Offset, 0U);
        MemberDecorate(block_type, 0U, spv::Decoration::NonWritable);
        slots = AddGlobalVariable(TypePointer(spv::StorageClass::StorageBuffer, block_type),
                                  spv::StorageClass::StorageBuffer);
        Decorate(slots, spv::Decoration::DescriptorSet, params.slots_set);
        Decorate(slots, spv::Decoration::Binding, params.slots_binding);
        Name(slots, "emu_mesh_slots");
        word_pointer = TypePointer(spv::StorageClass::StorageBuffer, u32_type);
    }

    // The position and generic outputs the mesh program writes, one per distinct generic index.
    void DefineInterface() {
        vertex_index = AddGlobalVariable(TypePointer(spv::StorageClass::Input, u32_type),
                                         spv::StorageClass::Input);
        Decorate(vertex_index, spv::Decoration::BuiltIn, spv::BuiltIn::VertexIndex);
        interfaces.push_back(vertex_index);

        position = AddGlobalVariable(TypePointer(spv::StorageClass::Output, vec4_type),
                                     spv::StorageClass::Output);
        Decorate(position, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
        interfaces.push_back(position);

        for (const u32 line : mesh.vertex_lines) {
            const auto attribute{static_cast<IR::Attribute>(line)};
            if (attribute == IR::Attribute::PointSize && !Sirit::ValidId(point_size)) {
                point_size = AddGlobalVariable(TypePointer(spv::StorageClass::Output, f32_type),
                                               spv::StorageClass::Output);
                Decorate(point_size, spv::Decoration::BuiltIn, spv::BuiltIn::PointSize);
                interfaces.push_back(point_size);
            } else if (IR::IsGeneric(attribute)) {
                const u32 index{IR::GenericAttributeIndex(attribute)};
                if (!generics.contains(index)) {
                    const Id variable{
                        AddGlobalVariable(TypePointer(spv::StorageClass::Output, vec4_type),
                                          spv::StorageClass::Output)};
                    Decorate(variable, spv::Decoration::Location, index);
                    interfaces.push_back(variable);
                    generics.emplace(index, variable);
                }
            }
        }
        // Per-primitive attributes: flat outputs, the same value at the three corners.
        for (const u32 line : mesh.primitive_lines) {
            const auto attribute{static_cast<IR::Attribute>(line)};
            if (!IR::IsGeneric(attribute)) {
                continue;
            }
            const u32 index{IR::GenericAttributeIndex(attribute)};
            if (!primitive_generics.contains(index) && !generics.contains(index)) {
                const Id variable{AddGlobalVariable(TypePointer(spv::StorageClass::Output, vec4_type),
                                                    spv::StorageClass::Output)};
                Decorate(variable, spv::Decoration::Location, index);
                Decorate(variable, spv::Decoration::Flat);
                interfaces.push_back(variable);
                primitive_generics.emplace(index, variable);
            }
        }
    }

    // The value of one vertex line (a float) for the vertex whose shared-memory index is `vertex`.
    Id LineValue(Id vertex_group_base, Id vertex_slot, u32 line_position) {
        const Id word{IAdd(IAdd(vertex_group_base, vertex_slot), U(line_position * 32))};
        const Id value{OpLoad(u32_type, OpAccessChain(word_pointer, slots, U(0), word))};
        return OpBitcast(f32_type, value);
    }

    Id IAdd(Id a, Id b) {
        return OpIAdd(u32_type, a, b);
    }

    Id IMul(Id a, Id b) {
        return OpIMul(u32_type, a, b);
    }

    void DefineMain() {
        const u32 num_lines{static_cast<u32>(mesh.vertex_lines.size())};
        const Id void_type{TypeVoid()};
        const Id main{OpFunction(void_type, spv::FunctionControlMask::MaskNone,
                                 TypeFunction(void_type))};
        AddLabel();

        const Id vertex_id{OpLoad(u32_type, vertex_index)};
        const Id triangle{OpUDiv(u32_type, vertex_id, U(3))};
        const Id corner{OpUMod(u32_type, vertex_id, U(3))};
        const Id group{OpUDiv(u32_type, triangle, U(mesh.max_primitives))};
        const Id primitive{OpUMod(u32_type, triangle, U(mesh.max_primitives))};
        const Id slot_base{IMul(group, U(mesh.emu_slot_words))};
        const auto load_word = [&](Id index) {
            return OpLoad(u32_type, OpAccessChain(word_pointer, slots, U(0), index));
        };
        const Id primitive_count{load_word(slot_base)};
        const Id valid{OpULessThan(bool_type, primitive, primitive_count)};

        // The map is one word per byte, after the primitive count: indices of the primitive's
        // corners start at byte 4 + 3 * primitive.
        const Id map_word{IAdd(slot_base, IAdd(U(1 + 4), IAdd(IMul(primitive, U(3)), corner)))};
        // Corner indices come from the guest: keep a bad one inside the vertices the mesh declared.
        const Id vertex{OpUMin(u32_type, OpSelect(u32_type, valid, load_word(map_word), U(0)),
                               U(mesh.max_vertices - 1))};

        // Attribute lines of 32 vertices form one group; a group holds num_lines lines.
        const Id attributes{IAdd(slot_base, U(mesh.emu_attribute_base))};
        const Id group_base{
            IAdd(attributes, IMul(OpShiftRightLogical(u32_type, vertex, U(5)), U(num_lines * 32)))};
        const Id vertex_slot{OpBitwiseAnd(u32_type, vertex, U(31))};

        std::array<Id, 4> components{F(0.0f), F(0.0f), F(0.0f), F(1.0f)};
        std::map<u32, std::array<Id, 4>> generic_values;
        for (u32 position_in_slot = 0; position_in_slot < num_lines; ++position_in_slot) {
            const auto attribute{static_cast<IR::Attribute>(mesh.vertex_lines[position_in_slot])};
            if (attribute >= IR::Attribute::PositionX && attribute <= IR::Attribute::PositionW) {
                components[static_cast<u32>(attribute) -
                           static_cast<u32>(IR::Attribute::PositionX)] =
                    LineValue(group_base, vertex_slot, position_in_slot);
            } else if (attribute == IR::Attribute::PointSize && Sirit::ValidId(point_size)) {
                OpStore(point_size, LineValue(group_base, vertex_slot, position_in_slot));
            } else if (IR::IsGeneric(attribute)) {
                auto& values{generic_values.try_emplace(IR::GenericAttributeIndex(attribute),
                                                        std::array<Id, 4>{F(0.0f), F(0.0f),
                                                                          F(0.0f), F(1.0f)})
                                 .first->second};
                values[IR::GenericAttributeElement(attribute)] =
                    LineValue(group_base, vertex_slot, position_in_slot);
            }
        }
        for (const auto& [index, values] : generic_values) {
            OpStore(generics.at(index), OpCompositeConstruct(vec4_type, std::span{values}));
        }

        // Per-primitive lines come after the vertex lines in the attribute region, in groups of 32
        // primitives, one 32-word line per attribute.
        const u32 num_primitive_lines{static_cast<u32>(mesh.primitive_lines.size())};
        if (num_primitive_lines != 0) {
            const MeshSlotLayout layout{MakeMeshSlotLayout(mesh)};
            const Id primitive_base{IAdd(attributes, U(layout.primitive_base))};
            const Id primitive_group_base{IAdd(
                primitive_base,
                IMul(OpShiftRightLogical(u32_type, primitive, U(5)), U(num_primitive_lines * 32)))};
            const Id primitive_lane{OpBitwiseAnd(u32_type, primitive, U(31))};
            std::map<u32, std::array<Id, 4>> primitive_values;
            for (u32 position_in_slot = 0; position_in_slot < num_primitive_lines;
                 ++position_in_slot) {
                const auto attribute{
                    static_cast<IR::Attribute>(mesh.primitive_lines[position_in_slot])};
                if (!IR::IsGeneric(attribute) ||
                    !primitive_generics.contains(IR::GenericAttributeIndex(attribute))) {
                    continue;
                }
                auto& values{primitive_values
                                 .try_emplace(IR::GenericAttributeIndex(attribute),
                                              std::array<Id, 4>{F(0.0f), F(0.0f), F(0.0f), F(1.0f)})
                                 .first->second};
                values[IR::GenericAttributeElement(attribute)] =
                    LineValue(primitive_group_base, primitive_lane, position_in_slot);
            }
            for (const auto& [index, values] : primitive_values) {
                OpStore(primitive_generics.at(index),
                        OpCompositeConstruct(vec4_type, std::span{values}));
            }
        }

        if (params.convert_depth) {
            // OpenGL-style clip space to Vulkan's: z = (z + w) / 2.
            components[2] = OpFMul(f32_type, OpFAdd(f32_type, components[2], components[3]), F(0.5f));
        }
        const Id live{OpCompositeConstruct(vec4_type, std::span{components})};
        // Collapsed primitives: every corner at the same point, so nothing is rasterized.
        const Id collapsed{OpCompositeConstruct(vec4_type, F(0.0f), F(0.0f), F(0.0f), F(1.0f))};
        const Id splat{OpCompositeConstruct(TypeVector(bool_type, 4), valid, valid, valid, valid)};
        OpStore(position, OpSelect(vec4_type, splat, live, collapsed));
        OpReturn();
        OpFunctionEnd();
        AddEntryPoint(spv::ExecutionModel::Vertex, main, "main", std::span<const Id>{interfaces});
    }

    const MeshVertexParams& params;
    const MeshInfo& mesh;
    Id bool_type{};
    Id u32_type{};
    Id f32_type{};
    Id vec4_type{};
    Id slots{};
    Id word_pointer{};
    Id vertex_index{};
    Id position{};
    Id point_size{};
    std::map<u32, Id> generics;
    std::map<u32, Id> primitive_generics;
    std::vector<Id> interfaces;
};
} // Anonymous namespace

std::vector<u32> EmitMeshEmulationVertexShader(const MeshVertexParams& params) {
    return VertexBuilder{params}.Build();
}

} // namespace Shader::Backend::SPIRV
