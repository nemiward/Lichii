// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"

namespace Shader::Backend::SPIRV {

struct MeshVertexParams {
    MeshInfo mesh;        ///< the mesh program: vertex lines, sizes, slot layout (emu_* fields)
    u32 slots_set{};      ///< descriptor set and binding of the mesh output slots (read-only)
    u32 slots_binding{};
    bool convert_depth{}; ///< z = (z + w) / 2, as the mesh epilogue does without native NDC
};

/// The vertex shader that draws what an emulated mesh program wrote (RuntimeInfo::emulate_mesh).
/// It draws `mesh groups * max_primitives * 3` vertices with no vertex input: vertex v is corner
/// v % 3 of primitive (v / 3) % max_primitives of the mesh group v / (3 * max_primitives). It
/// reads the group's slot, resolves the corner through the slot's vertex index map and writes
/// the same position and generic outputs the mesh program would have. Primitives past the slot's
/// primitive count collapse to one point, so they rasterize nothing.
[[nodiscard]] std::vector<u32> EmitMeshEmulationVertexShader(const MeshVertexParams& params);

} // namespace Shader::Backend::SPIRV
