#pragma once

// TrailComponent: a ribbon trail attached to an entity's world position.
//
// Each frame the Renderer snapshots the entity's GlobalTransform translation and appends a segment
// to a GPU ring buffer (at most kMaxSegments per trail, owned by TrailSystem); a compute shader
// writes the sample into its slot and advances the ring cursor. The render pass extrudes each
// consecutive pair of segments into a camera-facing ribbon quad whose width and colour fade with
// the segment's age.
//
// Slots are assigned lazily: the first frame that sees an entity allocates an unused slot in
// TrailSystem's pool; it is leased for the component's lifetime and returned when the entity is
// destroyed.

#include <cstdint>
#include <map>
#include <string>
#include <DirectXMath.h>

// Trail render-flag bits (mirrors TrailHeaderGPU.renderFlags).
enum TrailRenderFlag : uint32_t
{
    TrailRenderFlag_ArcLengthUV    = 1u << 0,
    TrailRenderFlag_Additive       = 1u << 1,
    TrailRenderFlag_TAAExclude     = 1u << 2,
    // With ArcLengthUV: U = absolute arc length from the trail's (re)start instead of distance
    // behind the head. Assigned when a sample is laid down and never changed -> the pattern is painted
    // into the world and dissolves in place (the classic weapon-ribbon look). Head-anchored U slides
    // through the points as the trail advances.
    TrailRenderFlag_UVWorldAnchor  = 1u << 3,
};

struct TrailComponent
{
    struct ShaderParamMeta
    {
        DirectX::XMFLOAT4 defaultValue{ 0, 0, 0, 0 };
        DirectX::XMFLOAT4 minValue{ 0, 0, 0, 0 };
        DirectX::XMFLOAT4 maxValue{ 1, 1, 1, 1 };
        bool              hasRange = false;
        // b8 params named "Tex_*" are TEXTURE SLOTS: the inspector shows a
        // drag-drop field and the packed value is the texture's bindless
        // index (-1 = unbound) instead of a user float.
        bool              isTextureSlot = false;
        uint8_t           numComponents = 4;   // 1..4, from the reflected var type
        std::string       label;
    };

    // ---- Visual parameters ----
    float             width      = 0.15f;  // ribbon half-width at spawn, world units
    float             maxAge     = 1.5f;   // seconds until a segment is fully faded
    DirectX::XMFLOAT4 startColor = { 1.0f, 0.9f, 0.4f, 1.0f }; // head (newest)
    DirectX::XMFLOAT4 endColor   = { 1.0f, 0.3f, 0.1f, 0.0f }; // tail (oldest)

    // ---- Sampling behaviour ----
    // Spawn a new segment only once the entity has moved more than this since the last sample, so a
    // stationary entity does not fill the ring with duplicate points. Multi-point trails gate on the
    // tip point (the fastest mover).
    float             minSampleDistance = 0.02f;

    // ---- Emitter control ----
    // Disabling stops sampling but does not clear the ribbon: TrailUpdate.cs.hlsl ages every live record
    // every dispatch regardless of requests, so an effect switched off mid-swing fades out over maxAge
    // instead of popping.
    bool              enabled    = true;

    // Set by gameplay (AnimFXGateSystem) when a ribbon is re-armed after being off. Without it the
    // first sample of the new swing chains onto whatever survived the gap within maxAge and draws a
    // straight bar between the two poses. CPU-only; consumed and cleared by CollectTrails.
    bool              restartRequested = false;

    // ---- Multi-point ribbon (weapon trail) ----
    // 1 = camera-facing single point; 2 = weapon base + tip; up to 4. localPoints are entity-local
    // offsets, [0] = base .. [pointCount-1] = tip (+Y default: PMX bones run +Y along their length).
    // At pointCount == 1 with localPoints[0] == (0,0,0) the sample is the entity's own position.
    uint32_t          pointCount     = 1;
    DirectX::XMFLOAT3 localPoints[4] = { {0,0,0}, {0,1.0f,0}, {0,0,0}, {0,0,0} };

    // ---- Look fields (migration storage) ----
    // Shading lives on the entity's MaterialComponent: texture = Base Color slot, blend =
    // userBlendMode (Opaque = unset), custom shader + params / Tex_ textures = the material's
    // custom-shader block. The fields below remain only as (a) what older scenes / .ivfx files store -
    // TrailSystem::CollectTrails synthesizes a MaterialComponent from them on first sight and clears
    // them - and (b) the fallback for mesh-bearing entities whose material belongs to the mesh. The
    // Inspector does not show them.
    std::string       texturePath;             // empty = untextured (vertex color only)
    // Custom pixel shader; empty = engine default (Trail.ps.hlsl). A bare filename resolves under
    // shaders/ (built-ins: "TrailFX_Fire.ps.hlsl", "TrailFX_RainbowNoise.ps.hlsl"); a dir-qualified
    // path is used as-is. The PS must #include "TrailFX.hlsli" and return via TrailOutput()
    // (premultiplied: the blend state is ONE / INV_SRC_ALPHA). With a custom shader startColor /
    // endColor are ignored - colour belongs to the shader, which exposes its own params via a
    // b8-space0 cbuffer with //@default/@min/@max/@label annotations (as material custom shaders do).
    std::string       customShaderPath;

    // ---- Custom shader parameters (customShaderPath != "" only) ----
    // User overrides for the shader's b8-cbuffer params (name -> value). Only entries that differ from
    // the shader's //@default are persisted.
    std::map<std::string, DirectX::XMFLOAT4> customParams;

    // Runtime-only reflection meta (defaults/ranges/labels), seeded by
    // TrailSystem once the shader compiles; drives the inspector UI.
    std::map<std::string, ShaderParamMeta> customParamMeta;   // NOT serialized
    // Extra texture slots ("Tex_*" b8 params): slot name -> asset path.
    std::map<std::string, std::string> customTextures;        // serialized (ctex_)
    // Resolved bindless indices per slot (-1 = unresolved / none). Runtime-only.
    std::map<std::string, int32_t>     customTexIndices;      // NOT serialized
    std::string       lastSyncedShaderPath;                   // NOT serialized (sync latch)

    uint32_t          uvMode         = 1;      // 0 = age U, 1 = arc U head-anchored, 2 = arc U world-anchored
    float             uvTileLength   = 0.0f;   // arc mode: metres per U repeat; 0 = stretch once
    uint32_t          blendMode      = 0;      // 0 = Alpha, 1 = Additive
    float             hdrIntensity   = 1.0f;   // rgb multiplier (bloom feed)
    bool              excludeFromTAA = false;  // stamp stencil 0x40 (kParticleTAAExcludeBit)

    // ---- Smoothing (pointCount >= 2 only) ----
    uint32_t          subdivisions   = 4;      // Catmull-Rom sub-segments per pair, 1..4
    float             subSampleAngleDeg = 15.0f; // CPU slerp substep trigger; 0 = off

    // ---- CPU-only per-frame state (not serialized) ----
    // Slot index assigned by TrailSystem (0..kMaxTrails-1); 0xFFFFFFFF = unassigned, allocated by
    // TrailSystem::CollectTrails on first sight.
    uint32_t          trailSlot        = 0xFFFFFFFFu;
    // Runtime-only clock for custom-shader Engine_TrailAge. Reset on re-arm;
    // unlike segment age, this is shared by every vertex of this emitter.
    float             emitterAge       = 0.0f;
    // Elapsed time since the last accepted pose, including gated frames.
    float             sampleElapsed    = 0.0f;
    // Last spawn position (tip point), used to gate minSampleDistance.
    DirectX::XMFLOAT3 lastSamplePos    = { 0.0f, 0.0f, 0.0f };
    bool              hasLastSample    = false;
    // Resolved bindless texture index (-1 = unresolved/none).
    int32_t           textureBindlessIdx = -1;
    // pointCount edit detection -> forced ResetHeader (ring relayout).
    uint32_t          lastPointCount     = 0;
    // Visual-param edit detection while idle -> RefreshParams request.
    uint32_t          lastParamHash      = 0;
    // Previous emitted sample's world pose (slerp substep reference).
    DirectX::XMFLOAT4 lastXformRot   = { 0, 0, 0, 1 };
    DirectX::XMFLOAT3 lastXformPos   = { 0, 0, 0 };
    DirectX::XMFLOAT3 lastXformScale = { 1, 1, 1 };
    bool              hasLastXform   = false;
};
