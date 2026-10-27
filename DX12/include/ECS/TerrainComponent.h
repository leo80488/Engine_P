#pragma once

// TerrainComponent: a heightmap-displaced mesh-shader terrain tile.
//
// One TerrainComponent per entity = one square tile; the MS samples the heightmap per vertex (flat
// when unset). PBR layers (bindless Texture2Ds sampled by world XZ x tilingScale) are weighted by
// the splatmap's RGBA or by each layer's height / slope auto-blend.
// Renderer::BuildScene_SyncTerrain syncs every texture path through TextureSystem (acquire /
// release on path change) and uploads the per-frame TerrainCB before the graph executes.

#include "ECS/ECS.h"
#include "Resource/SystemHandles.h"   // TextureHandle / kInvalidTextureHandle
#include "Resource/HeightField.h"     // CPU-side height grid for collision

#include <array>
#include <memory>
#include <string>
#include <vector>
#include <DirectXMath.h>

// One PBR layer in a terrain splat blend:
//   albedoPath - diffuse / albedo (RGB)
//   normalPath - tangent-space normal (DX convention: Y-up)
//   armPath    - packed AO / Roughness / Metalness (R / G / B)
//   dispPath   - single-channel height; biases the splat blend so layer transitions follow the
//                micro-relief. Read only when TerrainComponent heightBlendEnabled is set; missing
//                -> that layer adds 0 bias.
// With a splatmap the first 4 layers take its RGBA channels (layer 0 = R ... 3 = A); the
// auto-blend path supports any count.
struct TerrainLayer
{
    std::string albedoPath;
    std::string normalPath;
    std::string armPath;
    std::string dispPath;

    // World-space tiling: UV repeats per world metre. 0.05 = one texture repeat every 20 m, a sensible
    // default for ground textures.
    float       tilingScale = 0.05f;

    // Auto-blend (no splatmap): each layer is gated by a world-space height range and a slope range,
    // both in authorable units ("grass between 0 m and 200 m, on slopes 0-35 deg"). The two gates
    // multiply, so a layer shows only where both hold.
    //   Height: weight 1 inside [minHeight, maxHeight], 0 outside [minHeight - fadeHeight,
    //           maxHeight + fadeHeight], smoothstep falloff in the fade band.
    //   Slope:  weight 1 inside [minSlopeDeg, maxSlopeDeg], 0 outside [minSlopeDeg - fadeSlopeDeg,
    //           maxSlopeDeg + fadeSlopeDeg]; slope = angle between surface normal and world up
    //           (0 deg = flat ground, 90 deg = vertical cliff).
    // Defaults are wide open, so a fresh layer is visible everywhere: narrow min/maxHeight and
    // min/maxSlopeDeg in the Inspector to gate it to a band. Disjoint gates give clean per-zone looks;
    // overlapping gates cross-fade. Ignored when a splatmap is set.
    float       minHeight    = -1e6f;    // world m - layer fully visible above this height
    float       maxHeight    =  1e6f;    // world m - layer fully visible below this height
    float       fadeHeight   =  10.0f;   // world m - soft falloff width on both ends

    float       minSlopeDeg  =   0.0f;   // 0 deg = flat horizontal
    float       maxSlopeDeg  =  90.0f;   // 90 deg = vertical cliff
    float       fadeSlopeDeg =   5.0f;   // soft falloff width on both ends

    // Renderer-internal, filled by SyncTerrain each frame. bindlessIdx == -1 means the texture is not
    // GPU-resident yet; the PS skips that map (per-channel fallback) until promotion.
    mutable Resource::TextureHandle albedoHandle      = Resource::kInvalidTextureHandle;
    mutable Resource::TextureHandle normalHandle      = Resource::kInvalidTextureHandle;
    mutable Resource::TextureHandle armHandle         = Resource::kInvalidTextureHandle;
    mutable Resource::TextureHandle dispHandle        = Resource::kInvalidTextureHandle;
    mutable int32_t                 albedoBindlessIdx = -1;
    mutable int32_t                 normalBindlessIdx = -1;
    mutable int32_t                 armBindlessIdx    = -1;
    mutable int32_t                 dispBindlessIdx   = -1;
};

struct TerrainComponent
{
    // Heightmap asset path (.itex, .dds, .png - anything TextureSystem resolves). Empty -> the tile
    // renders flat.
    std::string heightmapPath;

    // World-space tile placement:
    //   X in [worldCenter.x - worldSize*0.5, worldCenter.x + worldSize*0.5]
    //   Z in [worldCenter.z - worldSize*0.5, worldCenter.z + worldSize*0.5]
    //   Y in [worldCenter.y, worldCenter.y + heightScale]
    // worldCenter.y is the base of the terrain and heightScale the total relief (valley floor -> highest
    // peak). The Renderer re-anchors the heightmap's actual data range onto that span (lowest sample ->
    // worldCenter.y, highest -> worldCenter.y + heightScale), so raising heightScale only grows the
    // peaks: the floor stays pinned even when the heightmap does not use the full [0,1] range (a raw
    // mapping would shift the tile by dataMin x heightScale on every edit). Until the CPU HeightField is
    // decoded (R16_UNORM only) the raw mapping is the fallback. Changing worldSize keeps the tile
    // centred on (worldCenter.x, worldCenter.z).
    DirectX::XMFLOAT3 worldCenter   { 0.0f, 0.0f, 0.0f };
    float             worldSize     = 1024.0f;
    float             heightScale   = 100.0f;

    // Sub-tile grid count per side. One mesh-shader group emits each of the tilesPerSide x tilesPerSide
    // sub-tiles as a 12x12 vertex patch (11x11 quads), the most one MS group can produce within DX12's
    // 256-vertex / 256-primitive budget; finer means more groups, not bigger ones.
    // Per-quad world size = worldSize / (tilesPerSide * 11); at worldSize 4096:
    //   32 -> ~11.6 m, 64 -> ~5.8 m, 128 -> ~2.9 m (~1M triangles), 192 -> ~1.9 m, 256 -> ~1.5 m (~4M).
    // The PS derives the normal per pixel from heightmap derivatives, so this only controls geometric
    // silhouette fidelity (skylines, ridges). Cost grows as N^2; up to ~256 is still cheap.
    uint32_t tilesPerSide = 128;

    // Heightmap UV remap. Defaults to identity (the whole texture covers the
    // whole tile). Use these when one heightmap drives many tiles.
    DirectX::XMFLOAT2 heightmapUVOffset { 0.0f, 0.0f };
    DirectX::XMFLOAT2 heightmapUVScale  { 1.0f, 1.0f };

    // Splatmap (Texture2D, RGBA): each channel weights one of the first 4 layers. Unset -> the layers'
    // height / slope auto-blend; with neither a splatmap nor a layer the PS draws a slope-based debug
    // palette.
    std::string splatmapPath;

    // PBR layers blended by the splatmap's RGBA (first 4) or by the per-layer height / slope
    // auto-blend (any count). The GPU loops over layers.size() via a StructuredBuffer, so adding or
    // removing a layer is a pure data edit (no shader / CB / struct change). Defaults to 4 entries.
    std::vector<TerrainLayer> layers = std::vector<TerrainLayer>(4);

    // ---- Height-correlated blend (per-layer displacement) ----
    // When enabled, each layer's blend weight is biased by its disp map so transitions follow the
    // micro-relief ("grass pokes through where the gravel dips"). Off -> the plain linear splat /
    // auto blend.
    //   strength - how strongly disp biases the base weight; small (0.1-0.2) keeps the splat / gate
    //              dominant, large lets disp override.
    //   range    - soft cutoff: a layer keeps weight while its biased value is within `range` of the
    //              local maximum. Wider = smoother multi-layer blends, narrower = sharper interlock.
    bool  heightBlendEnabled  = false;
    float heightBlendStrength = 0.15f;
    float heightBlendRange    = 0.10f;

    // Renderer-internal, filled by SyncTerrain each frame.
    mutable Resource::TextureHandle heightmapHandle = Resource::kInvalidTextureHandle;
    mutable uint64_t                heightmapSRV    = 0;
    mutable Resource::TextureHandle splatmapHandle  = Resource::kInvalidTextureHandle;
    mutable uint64_t                splatmapSRV     = 0;

    // Renderer-internal: the re-anchored world-Y mapping fed to the GPU (TerrainParams) and the CPU
    // (HeightField) this frame:
    //   worldY = effBaseY + rawSample01 * effHeightScale
    // Equal to (worldCenter.y, heightScale) until the heightmap's data range is known, then adjusted
    // so the data minimum lands on worldCenter.y. Grass / Water sync read these so every height
    // consumer agrees.
    mutable float effBaseY        = 0.0f;
    mutable float effHeightScale  = 0.0f;

    // CPU-side height grid for collision / queries. The Renderer decodes it from the loaded R16_UNORM
    // heightmap the first frame the texture is ready, and refreshes it whenever the heightmap path or
    // worldCenter.y / heightScale change. A shared_ptr so several tiles can share one heightmap
    // without a copy each. Null until ready.
    mutable std::shared_ptr<Resource::HeightField> heightField;
};

// ==== SampleTerrainHeightAt ====
// Collision-side height query: the world Y of @p tc's surface at (@p worldX, @p worldZ). Repeats
// the GPU math of Terrain.ms.hlsl / Terrain.ps.hlsl, so a physics entity on the rendered surface
// sits at exactly the rendered Y:
//   1. world XZ -> tile-local [0,1]^2 (worldOrigin = worldCenter - worldSize/2);
//   2. apply heightmapUVOffset / heightmapUVScale (the GPU's UV remap);
//   3. bilinear-sample the HeightField -> world Y in [baseY, baseY + heightScale].
// @p outInBounds (optional) is set false when the HeightField is not decoded yet (the result is
// then worldCenter.y) or XZ is outside the tile; check it before using the result for collision
// response.
inline float SampleTerrainHeightAt(const TerrainComponent& tc,
                                   float worldX, float worldZ,
                                   bool* outInBounds = nullptr)
{
    const auto& hf = tc.heightField;
    if (!hf || !hf->IsValid())
    {
        if (outInBounds) *outInBounds = false;
        return tc.worldCenter.y;
    }

    const float halfSize = tc.worldSize * 0.5f;
    const float originX  = tc.worldCenter.x - halfSize;
    const float originZ  = tc.worldCenter.z - halfSize;

    const float u = (worldX - originX) / tc.worldSize;
    const float v = (worldZ - originZ) / tc.worldSize;

    const bool inBounds = (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f);
    if (outInBounds) *outInBounds = inBounds;

    // Apply the same UV remap the GPU uses, so a heightmap tiled over
    // many entities samples consistently.
    const float hu = tc.heightmapUVOffset.x + u * tc.heightmapUVScale.x;
    const float hv = tc.heightmapUVOffset.y + v * tc.heightmapUVScale.y;

    return hf->SampleHeightWorld(hu, hv);
}
