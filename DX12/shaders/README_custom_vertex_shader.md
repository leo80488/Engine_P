# Custom Vertex Shaders (Unreal/Unity-style)

Materials can now customize the **vertex** stage in addition to the pixel stage,
the way Unreal's *World Position Offset* node or Unity surface-shader `vertex:`
function does — you write a tiny **hook**, the engine keeps all the
vertex-fetch / tangent-basis / TAA-velocity / billboard machinery.

## Authoring

Create a `*.vs.hlsl` file that includes the engine template and defines exactly
one function — `CustomizeVertex`:

```hlsl
#include "custom_vertex.hlsli"

// Optional tunables — reflected into the Inspector (b8 = per-material custom CBV).
cbuffer MyParams : register(b8, space0)
{
    float WaveStrength;
    float WaveSpeed;
};

void CustomizeVertex(inout VtxContext v)
{
    // World Position Offset
    v.worldPos.y += sin(v.time * WaveSpeed + v.worldPos.x) * WaveStrength;
    // Scrolling UV / vertex-color / normal bend are all writable too.
    v.uv.x += v.time * 0.1;
}
```

Then in the Material Inspector: tick **Use Custom Shader** and drop the file into
**Vertex Shader Path** (independent of **Pixel Shader Path** — set either, both, or
neither). A worked example ships at `asset/Shaders/wind_vertex.vs.hlsl`.

## `VtxContext`

| Writable        | meaning                                  |
|-----------------|------------------------------------------|
| `worldPos`      | world-space position — add WPO offsets   |
| `worldNormal`   | world-space normal                       |
| `worldTangent`  | world-space tangent                      |
| `uv`, `uv1`     | UV sets                                  |
| `color`         | vertex color RGB                         |

| Read-only       | meaning                                  |
|-----------------|------------------------------------------|
| `localPos`      | mesh-local position (pre-transform)      |
| `worldMatrix`   | instance world matrix                    |
| `time`,`deltaTime` | scene time (seconds)                  |
| `cameraPos`     | camera world position                    |
| `vertexID`, `instanceID`, `materialIndex` | identifiers     |

## How it wires (for maintainers)

- `MaterialComponent.customVertexShaderPath` (persisted) + renderer-cached runtime ShaderID
  (runtime). `Renderer::ResolveCustomVSID` registers the file as a dynamic VS in
  **both** the GBuffer and Transparent shader libraries (mirrors `ResolveCustomPSID`),
  and syncs its reflected `b8` params / `space3` textures into the shared custom bag.
- `DrawPacket/IndirectGroup/DrawCandidate.customVSID` flows into
  `GBufferPass::BuildPSODesc` / `TransparentPass::BuildPSODesc`, which set
  `desc.vsID = customVSID ? customVSID : GBuffer_VS`. No new PSO permutations.
- Engine time + camera reach the hook via two `float4`s appended to `PerViewCB`
  (still < 256 B, so the CBV size is unchanged; other PerView consumers are
  oblivious). Root params 35 (`b8`) and 36 (`space3`) were widened to `ALL`
  visibility so the VS can read tunables/textures — `kRootSigVersion` bumped 5→6.

## Shadows

World-Position-Offset **does** reach the shadow maps. `custom_vertex.hlsli` compiles a
depth-only variant (`#if CUSTOM_VS_SHADOW`) that runs the same fetch + `CustomizeVertex`
hook, so the cast shadow tracks the deformed silhouette in **CSM (sun), spot, and point**
shadow passes. `gTimeParams` lives at a fixed CB offset (192) in both the GBuffer
`PerViewCB` and the shadow `ShadowPerViewCB`, so the hook reads identical time in either
pass and the shadow deforms in lock-step with the lit mesh. (The far terrain-only CSM
cascade is skipped — dynamic casters are sub-pixel there.)

Maintainer notes: custom-VS casters are excluded from the shadow passes' `Shadow_VS`
ExecuteIndirect groups and rendered in a dedicated per-draw loop (each needs its own VS
PSO + `b8` params). The custom VS is registered once (`Renderer::RegisterMaterialShader`,
render-arch MT5.0) in the process's dynamic shader registry, which every pass library
resolves its id through — the same id everywhere, with nothing to register per pass.

## Limitations

- Motion vectors reuse the current-frame WPO delta for the previous position — exact
  for static deformation, a close approximation for time-animated WPO (matches
  Unreal's default; no extra per-vertex cost).
- If a material overrides **both** stages and **both** declare `b8` params, they must
  share one cbuffer layout (only one reflection's variables are packed).
