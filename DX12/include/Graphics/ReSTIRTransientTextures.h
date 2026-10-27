#pragma once
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/MemoryPolicy.h"
#include "Graphics/TransientAliasArena.h"
#include "System/CommandLine.h"
#include <algorithm>
#include <array>
#include <span>

// Transient ReSTIR DI / GI textures in one alias arena. They have no cross-frame
// reads; DI and GI are serialized direct-queue graph passes, each resolving into a
// separate, non-aliased graph result.
namespace ReSTIRTransient
{
inline constexpr std::array diFormats={RHI::Format::R16G16B16A16_FLOAT,RHI::Format::R16G16B16A16_FLOAT,
    RHI::Format::R10G10B10A2_UNORM,RHI::Format::R32_FLOAT,RHI::Format::R16G16_FLOAT,
    RHI::Format::R16G16B16A16_FLOAT,RHI::Format::R16G16B16A16_FLOAT,RHI::Format::R8_UNORM};
inline constexpr std::array giFormats={RHI::Format::R16G16B16A16_FLOAT,RHI::Format::R10G10B10A2_UNORM,
    RHI::Format::R32_FLOAT,RHI::Format::R16G16_FLOAT,RHI::Format::R16G16B16A16_FLOAT,RHI::Format::R16_FLOAT};
struct Layout { std::array<uint64_t,8> offsets{}; uint64_t bytes=0; };
inline RHI::TextureDesc Describe(uint32_t w,uint32_t h,RHI::Format format)
{
    RHI::TextureDesc d{}; d.width=w; d.height=h; d.depth=1; d.mipLevels=1;
    d.format=format; d.usage=RHI::Usage::DEFAULT;
    d.bindFlags=RHI::BindFlag::SHADER_RESOURCE|RHI::BindFlag::UNORDERED_ACCESS;
    d.layout=RHI::ResourceState::UNORDERED_ACCESS;
    return d;
}
inline Layout Measure(IGraphicsDevice& gfx,uint32_t w,uint32_t h,std::span<const RHI::Format> formats)
{
    Layout layout;
    if(!w || !h || formats.size()>layout.offsets.size()) return layout;
    for(size_t i=0;i<formats.size();++i)
    {
        uint64_t size=0,alignment=0;
        if(!gfx.GetTextureAllocationInfo(Describe(w,h,formats[i]),size,alignment)) return {};
        layout.offsets[i]=TransientAliasArena::AlignUp(layout.bytes,alignment);
        layout.bytes=layout.offsets[i]+size;
    }
    // 64 KiB of mapped slack after the last NRD guide, inside the usable capacity; the
    // arena's kTailGuard stays unoccupied.
    layout.bytes+=64ull<<10;
    return layout;
}
inline void Reserve(IGraphicsDevice& gfx,TransientAliasArena& arena)
{
    // Reserve the full-resolution worst case when the first DI / GI working set is built, so half /
    // full resolution and render-path toggles after it never grow the arena (a growth idles
    // the GPU and re-places every group). Idempotent: a reserved arena answers at once.
    const auto di=Measure(gfx,gfx.GetRenderWidth(),gfx.GetRenderHeight(),diFormats);
    const auto gi=Measure(gfx,gfx.GetRenderWidth(),gfx.GetRenderHeight(),giFormats);
    if(di.bytes && gi.bytes) gfx.EnsureAliasArena(arena,std::max(di.bytes,gi.bytes));
}
// The harness's eager arm (MemoryPolicy::EagerTransients): the reservation made at boot.
inline void ReserveIfEager(IGraphicsDevice& gfx,TransientAliasArena& arena)
{
    if(MemoryPolicy::EagerTransients() && gfx.GetCapabilities().inlineRaytracing
        && !Sys::CommandLine::Get().Has("-restir_no_alias"))
        Reserve(gfx,arena);
}
}
