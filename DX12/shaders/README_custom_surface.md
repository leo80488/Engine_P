# Custom Material Shaders (single-file template + hook model)

The engine's material system is **template + hook**, the way Unreal (Material
`EvaluateMaterial`) and Unity SRP (surface functions) work. The engine owns the
per-pass shader templates; **one material source file** fills in up to **two small,
optional hooks** — and the engine compiles that same file for both the vertex and
pixel stages:

| Hook | What you fill | Optional? |
|---|---|---|
| `float3 MaterialGetWorldPositionOffset(MaterialVertexParams p)` | World-Position-Offset (wind, animation) | yes — omit for no WPO |
| `void ModifyVertexAttributes(inout MaterialVertexParams p)` | edit normal/uv/vertex-color | yes — omit for no change |
| `void EvaluateMaterial(MaterialPixelParams p, inout SurfaceData s)` | albedo, normal, roughness/metal/AO, emissive, opacity | the pixel half |

A material can override the pixel half only (the common case), the vertex half
only, both, or neither — each missing hook falls back to an engine weak default
(standard PBR pixels, zero WPO). The engine's own `GBuffer.ps` / `Transparent.ps`
are literally just the templates with the default pixel hook.

You include **one header** — `custom_material.hlsli` — and assign the file to the
material's single **"Material Shader"** slot. The two hooks share one set of
interpolators and one material constant buffer, so they can never drift.

> A **pixel-only** material is NOT compiled as a vertex shader at all — it shares
> the engine generic vertex shader + generic depth (and, unless it is Masked, the
> generic shadow). WPO is opt-in: declare `#define MATERIAL_HAS_VERTEX_OFFSET` and
> the engine also compiles the file as a vertex shader so the offset reaches the
> GBuffer, depth, and shadow passes.

---

## Authoring a material (single file)

```hlsl
#define MATERIAL_HAS_SURFACE          // I provide EvaluateMaterial (pixel)
// #define MATERIAL_HAS_VERTEX_OFFSET // uncomment to also provide WPO (vertex)
#include "custom_material.hlsli"

// [optional] World-Position-Offset — only compiled when MATERIAL_HAS_VERTEX_OFFSET.
float3 MaterialGetWorldPositionOffset(MaterialVertexParams p)
{
    return float3(sin(p.time + p.worldPos.z) * 0.1, 0, 0);
}

void EvaluateMaterial(MaterialPixelParams p, inout SurfaceData s)
{
    MaterialGPUData mat = g_Materials[p.materialIndex];

    // baseColor: material tint × base texture (engine helper)
    s.baseColor = mat.baseColor.rgb * MS_SampleBaseColor(mat, p.uv0).rgb;

    // normal map → world space (engine gives you the TBN)
    float3 tsN  = MS_SampleNormalTS(mat, p.uv0, mat.normalStrength);
    s.normalWS  = MS_ApplyNormalMap(tsN, p.tangentToWorld);

    // surface map: R=AO G=roughness B=metalness
    float4 surf = MS_SampleSurface(mat, p.uv0);
    s.ao        = surf.r;
    s.roughness = lerp(mat.roughnessMin, mat.roughnessMax, surf.g);
    s.metallic  = lerp(mat.metalnessMin, mat.metalnessMax, surf.b);

    s.emissive  = MS_SampleEmissive(mat, p.uv0);
    // anything you DON'T set keeps its MS_DefaultSurface() value.
}
```

Then in the Inspector: **Use Custom Shader → Pixel Shader Path → drag this file.**

That's it. The engine `main()` builds the params, calls your hook, runs masked
clip, geometric specular AA, TAA velocity, and the 6-MRT GBuffer encode. The same
hook serves the deferred lighting that follows — you can't desync the GBuffer
layout — and, for a **Masked** material, its shadow: the shadow passes run a
depth-only variant of your pixel shader (your hook, the same masked clip, no
outputs), so the shadow is the cut-out your `s.opacity` draws. WPO reaches depth
and shadow through the vertex side.

### You never write
- `SV_Position` / `SV_Target` outputs, the GBuffer RT packing, the matIdx encode
- TBN construction, velocity, masked alpha-test, double-sided normal flip
- anything about which pass is running

---

## The hook interface

```hlsl
struct MaterialPixelParams        // what the engine hands you (read-only)
{
    float3   worldPos;
    float3   geoNormalWS;     // interpolated geometry normal, front-face corrected
    float3   tangentWS, bitangentWS;
    float3x3 tangentToWorld;  // mul(tangentSpaceNormal, tangentToWorld) → world
    float2   uv0, uv1;
    float4   vertexColor;     // .rgb vertex color, .a = 1
    float3   viewDirWS;       // normalize(cameraPos - worldPos)
    uint     materialIndex;
    bool     isFrontFace;
};

struct SurfaceData                // what you produce (engine encodes it)
{
    float3 baseColor;     float opacity;
    float3 normalWS;      // WORLD-space shading normal
    float  roughness, metallic, ao, reflectance;
    float3 emissive;      // HDR; added on top of lighting (never goes through BRDF)
    float4 extra;         // RT4 scratch (NPR terminator, face SDF, hair mask …)
    uint   shadingModel;  // informational only — see the note below
    float  directAdditive;// light-independent mask, gated by the sun's visibility

    // --- Anisotropic (only read when the MATERIAL's model is Anisotropic) ---
    float  anisotropy;        // [-1,1]
    float3 anisotropyDirWS;   // seeded with the mesh tangent, already rotated

    // --- Subsurface (only read when the MATERIAL's model is Subsurface) -----
    float  subsurfaceOpacity; // [0,1] 1 = solid, 0 = thin/see-through
    float  subsurfaceMask;    // [0,1] per-pixel scatter amount

    // --- Clear Coat (only read when the MATERIAL's model is ClearCoat) ------
    float  clearCoat;          // [0,1] strength; 0 == Standard, exactly
    float  clearCoatRoughness; // [0,1] the COAT's own, independent of roughness

    // --- Transmission (only read on a Transmissive BLEND MODE material) -----
    // The transmission prepass (TRANSMISSION_PREPASS, TransmissionPrepass) runs the hook too and
    // reads these with opacity (coverage), roughness and normalWS: what crosses the surface, where
    // it goes and how much it blurs. The colour draw then adds the surface's own light.
    float  transmission;       // [0,1] share of the dielectric base that refracts
    float  thickness;          // world units through the volume; 0 = thin wall
};
```

> `s.shadingModel` is **informational**. The lighting model is a *material*
> setting, not a per-pixel one: the deferred pass branches on
> `g_Materials[matIdx].customShadingModel`, which comes from the Inspector.

### Engine helpers (in `material_surface.hlsli`)
`MS_SampleBaseColor`, `MS_SampleSurface`, `MS_SampleNormalTS`, `MS_ApplyNormalMap`,
`MS_SampleEmissive`, `MS_GeometricSpecularAA`, `MS_DefaultSurface`,
`MS_RotateAboutAxis`, `MS_AnisoDirFromTangentSpace`. The sampling
helpers automatically use the bindless table (deferred) or per-draw textures
(forward) as appropriate for the pass.

---

## Shading models: Anisotropic, Subsurface and Clear Coat

Pick one in the Inspector (**Shading Model**). It works on
the engine's own material — *no custom shader required* — because both models
read the standard GBuffer the default hook already produces, and both are
evaluated per light (the sun **and** every point/spot) in `Lighting.ps.hlsl` and
in the forward/translucent path.

| Model | Material knobs (Inspector) | Per-pixel hook fields |
|---|---|---|
| **Anisotropic** | Anisotropy, Rotation | `s.anisotropy`, `s.anisotropyDirWS` |
| **Subsurface** | Subsurface Color, Scatter Strength, Wrap, Opacity | `s.subsurfaceOpacity`, `s.subsurfaceMask` |
| **Clear Coat** | Clear Coat, Coat Roughness | `s.clearCoat`, `s.clearCoatRoughness` |

```hlsl
void EvaluateMaterial(MaterialPixelParams p, inout SurfaceData s)
{
    MaterialGPUData mat = g_Materials[p.materialIndex];
    // … the usual baseColor / normal / roughness work …

    // Brushed disc: the grain runs around the object's own axis, not the UVs.
    s.anisotropy      = -0.8;                       // negative = streak ACROSS the axis
    s.anisotropyDirWS = normalize(cross(p.objectAxes[1], p.worldPos - p.objectOrigin));

    // Skin: thin at the ears and the nose, solid everywhere else.
    s.subsurfaceOpacity = MA_Tex(_ThicknessMap, p.uv0, 1.0).r;

    // Car paint: a mirror film over a rough flake base, worn thin on the edges.
    s.clearCoat          = wearMask;
    s.clearCoatRoughness = 0.05;
}
```

**Clear coat** is a second, always-dielectric (F0 = 0.04, IOR 1.5) specular layer
with its OWN roughness — that independence is the whole point, because a mirror
film over a rough base is what no single GGX lobe can express. The base, diffuse
and specular, direct and indirect, is attenuated by the coat's Fresnel. Strength
0 makes the lobe and the attenuation vanish *exactly*, so a ClearCoat material
that never authored a strength renders bit-identically to Standard.

The coat shares `normalWS` with the base — RT4 has no room for a second normal —
so over a strongly normal-mapped base the coat's reflection follows the bumps.

**Sign convention (matches Unreal):** positive anisotropy widens the lobe
*along* the tangent, so the highlight stretches along it. Hair and brushed metal,
whose tangent runs along the strand, want a **negative** value — or `Rotation =
0.25`, which is the same thing.

**Things that will bite you**

* Never hand-write `s.extra.r/.g` for these models. The engine packs them into
  GBuffer RT4 with the codec the deferred pass decodes (`shading_models.hlsli`);
  a hand-rolled packing has no way to stay in sync. `s.extra.b` (hair mask) and
  `s.extra.a` (face marker / direct-additive) are untouched and stay yours.
* A **face** material (RT4.a marker ≥ 1) keeps all four RT4 channels and the
  per-pixel half of these models is skipped — the material-level values still
  apply. There is no fifth channel; this is deliberate, not a bug.
* Anisotropy needs a real **tangent stream**. A mesh imported without one gets a
  synthesised basis with a hard axis swap near `|N.y| = 0.99` (PMX/MMD meshes are
  all in this category), so the grain direction there is arbitrary. Re-import
  through the Assimp path if you need authored anisotropy on such a mesh.
* Anisotropy at 0 is *not* bit-identical to Standard: the anisotropic lobe uses
  the height-correlated Smith visibility that pairs with its NDF, while Standard
  uses UE4's Schlick-k approximation. A few percent at grazing angles, and it is
  a per-material choice, so nothing can pop.
* **Toon/NPR materials discard transmission.** The NPR composite rebuilds its
  colour from its own accumulators, so Subsurface's (and Two-Sided Foliage's)
  scatter term is computed and thrown away there. Anisotropic *does* work under
  Toon — it replaces the GGX lobe the toon spec ramp stylizes.

---

## Tunable parameters & custom textures

Exactly like a custom vertex shader: declare a `cbuffer` at `b8 space0` and/or
unbound `Texture2D`s at `space3`. The engine reads those declarations from your
source (render-arch MT5.2) and renders every member and texture as an editable
slider / drag-drop texture slot in the Inspector, whether a compiled variant reads
it or not — one none reads is marked **(not read)**. Values persist in the `.imat`.
(`b8` is shared with the custom VS — if a material overrides both stages and both
declare `b8`, they must share one layout.)

```hlsl
cbuffer MyParams : register(b8, space0) { float4 TintColor; float Brightness; };
Texture2D _DetailMap : register(t0, space3);   // slots bind by register: gaps are fine
```

> **Keep the declarations plain.** A declaration the parser cannot lay out —
> inside `#if`, a register or size that is not a literal, a global outside any
> cbuffer, a struct / `half` / sized-integer member, `packoffset` — makes the
> source *inexact*: its interface falls back to DXC's reflection (an unread member
> then gets no row, and a texture DXC strips loses its slot) and the log says why
> once (`[MATDECL] '<file>': N declaration(s) the parser cannot lay out ...`). A
> stripped texture never shifts the others: slots bind by register. The full list
> and the offline check (`-assettool=matdecl`): the `custom-shader-authoring`
> skill, §4a.

## Permutation defines and the domain

The engine compiles your hook with every permutation define (`ALPHA_TEST`,
`DOUBLE_SIDED`, `UNLIT`, … — `Graphics/PermutationKey.h`) passed as `0` or `1`.
Test them with `#if X`, never `#ifdef X`: `#ifdef` is always true, `#ifndef` always
false, `defined(X)` alone always true — the parser warns once per line
(`[MATDECL] <file>:<line>: #ifdef ALPHA_TEST is always true ...`). A variant is
cached under the defines your own files and the engine headers they include name
(render-arch MT5.3), so a define nothing you compile mentions shares one blob.

A water surface says so with `#define MATERIAL_DOMAIN_WATER` beside
`MATERIAL_HAS_SURFACE` (`asset/Shaders/water_surface.hlsl` does; the material
graph writes it for the Water shading model). The domain is recorded and shown —
`Render.explain(id).material.customPS.declaredDomain`, a note in the Inspector that
flags a Shading Model that disagrees — not enforced.

---

## Opaque vs translucent

The **same** `EvaluateMaterial` serves both. The material's **Blend Mode** picks
the pass:

- **Opaque** → deferred GBuffer (6 MRTs). Deferred lighting (PBR or NPR) runs after.
- **Alpha / Additive / Premultiplied / Multiply** → forward shading. The template
  does the BRDF + IBL + reflection probes from your `SurfaceData` and returns a
  single HDR color, honouring the blend mode.

The depth prepass never runs a custom pixel shader (a Masked material is not in it;
its depth comes from the GBuffer), and an opaque material's shadow is depth-only —
only a custom *vertex* shader reaches it (so World-Position-Offset deforms shadows).
A **Masked** material's shadow runs your pixel shader in the `SHADOW_HOOK` variant
(render-arch MT3): `ALPHA_TEST` + `CUSTOM_VS_SHADOW` + `SHADOW_HOOK`, fed by your
custom VS or the engine's `ShadowHook.vs`, clipping with the GBuffer's rule. In that
variant b1 is the shadow view's constant buffer (time and camera at the GBuffer
offsets, `p.viewDirWS` from the MAIN camera, no `gFrameIndex` — a Dithered shadow
dithers statically); a hook that does not compile there falls back to the engine's
`Shadow.ps` (the engine slots' alpha) with one WARNING. Ray-traced effects (RT
shadows, RT reflections, DDGI) still alpha-test with the engine slots
(BASECOLORMAP.a / OPACITYMAP.r) — mirror your mask into OPACITYMAP for them.

---

## NPR / deferred-stylized materials

For a toon/NPR look, write an **unlit material GBuffer** (stylized albedo + real
normal + rough/metal/AO + emissive), set **Shading Model = Toon / NPR** (the
deferred NPR route, stencil 2 - "Shader Type" is gone; a material left on Standard
is lit as PBR), and let the deferred `Lighting.ps` NPR pass light + stylize it.
Light-dependent NPR features that must be computed in the geometry pass (face-SDF
terminator, hair anisotropy) can be stashed in `SurfaceData.extra` (RT4) for the
lighting pass to read. Custom GBuffer hooks get `LightCB` at `b2` (sun direction,
camera, view matrix) — see `asset/EngineResource/amaeth/char/npr_character.hlsli`
for the Aimisi character reference.

---

## Power-user escape hatch

You can still write a whole monolithic `.ps.hlsl` that owns `main()` and the
GBuffer encode itself (the pre-refactor style) — it compiles and binds exactly the
same way. Use it only for effects that need full control; you lose the automatic
multi-pass consistency the hook model guarantees. Prefer the hook.
