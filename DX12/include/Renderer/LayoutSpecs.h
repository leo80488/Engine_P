#pragma once

// The C++ structs every loaded shader's mirror is checked against
// (Graphics/LayoutCheck.h). The tables live in src/Renderer/LayoutSpecs.cpp,
// drafted by tools/gen_layout_spec.py.

namespace RendererDetail
{
    // Hands the tables to Layout::Register. The Renderer constructor calls it,
    // before any pass loads a shader.
    void RegisterLayoutSpecs();
}
