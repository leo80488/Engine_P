#pragma once

#include "ECS/ECS.h"

#include <cstdint>
#include <string_view>

// Non-owning, GPU-ready view of one authored material texture reference.
// TextureSystem ownership stays in Renderer's shared RuntimeMaterialResource;
// ECS/authored material data never stores these runtime descriptor values.
struct MaterialRuntimeTextureView
{
    uint64_t gpuHandle        = 0;
    uint64_t previewGpuHandle = 0;
    int32_t  bindlessIndex    = -1;

    constexpr bool IsReady() const { return bindlessIndex >= 0; }
};

// Narrow interface used by renderer-adjacent systems (DDGI/RTR and editor)
// without exposing RuntimeMaterialResource ownership or DX12 texture objects.
class IMaterialRuntimeLookup
{
public:
    virtual ~IMaterialRuntimeLookup() = default;

    virtual MaterialRuntimeTextureView GetMaterialTextureRuntime(
        Entity entity, uint32_t slot) const = 0;
    virtual MaterialRuntimeTextureView GetMaterialCustomTextureRuntime(
        Entity entity, std::string_view bindingName) const = 0;
};
