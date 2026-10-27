#pragma once

// MaterialWidgets - the material panel's two reusable widgets: a texture slot
// (thumbnail, path field, drag-drop of ITEX_PATH, the Texture Viewer on a
// right-click) and one reflected b8 custom parameter.
//
// Editor-only TU: compiled into EditorCore.

#include "ECS/Components.h"              // MaterialComponent::TextureMap
#include "Graphics/MaterialRuntime.h"    // MaterialRuntimeTextureView
#include "Graphics/ShaderReflection.h"   // ShaderReflect::VarType

namespace Editor
{
    // Shared TextureMap slot UI (preview + drag-drop + path edit). Returns true if
    // edited; the caller calls SetEditDirty.
    bool DrawTextureSlotWidget(
        const char* label,
        MaterialComponent::TextureMap& slot,
        const MaterialRuntimeTextureView& runtime = {});

    // ---- Shared custom-param widget (material + trail inspectors) ----
    // Draws one reflected b8 param. The //@label text is the visible widget name;
    // a '|'-separated label on a multi-component float param splits the row into
    // per-component sliders, each honoring its own @min/@max. "color"-named
    // params keep the combined HDR colour widget. Right-click a row -> reset to
    // the shader default. @p val is float[4]; returns true when it changed.
    struct CustomParamDrawDesc
    {
        const char*            name          = nullptr;  // cbuffer var name (stable ID)
        int                    numComponents = 4;        // 1..4
        ShaderReflect::VarType type          = ShaderReflect::VarType::Float4;
        const float*           defaultValue  = nullptr;  // float[4]; null = no reset menu
        const float*           minValue      = nullptr;  // float[4], used when hasRange
        const float*           maxValue      = nullptr;
        bool                   hasRange      = false;
        const char*            label         = nullptr;  // may be null/empty
    };

    bool DrawCustomParamWidget(const CustomParamDrawDesc& d, float* val);
}
