// material_types.hlsli - the material hook interface structs, shared by the vertex and
// pixel sides so a unified material source (custom_material.hlsli) compiles for both stages
// (each stage's main calls only its hook, but the unused hook still has to parse).
// Included by material_surface.hlsli (pixel) and custom_material.hlsli (both). Pure struct
// declarations - no bindings, no main, no helpers - so either stage can include it without
// register conflicts.

#ifndef MATERIAL_TYPES_HLSLI
#define MATERIAL_TYPES_HLSLI

#include "material.hlsli"   // MAT_* / SHADING_MODEL_* constants + MaterialGPUData

// ==== Vertex hook params (context handed to the vertex hooks) ====
// `MaterialGetWorldPositionOffset(MaterialVertexParams)` returns a world-space offset;
// `ModifyVertexAttributes(inout MaterialVertexParams)` may edit the writable surface
// attributes (normal/uv/color) in place.
struct MaterialVertexParams
{
    // Writable by ModifyVertexAttributes (the engine reads them back).
    float3 worldPos;       // world-space position (pre-WPO)
    float3 normalWS;       // world-space normal
    float3 tangentWS;      // world-space tangent
    float2 uv0;            // primary UV
    float2 uv1;            // second UV
    float2 uv2;            // additional UV
    float2 uv3;            // additional UV
    float3 vertexColor;    // per-vertex color rgb
    // Read-only context.
    float3   localPos;     // mesh-local position
    float4x4 worldMatrix;  // per-instance world matrix (row-vector; [3].xyz = instance origin)
    float    time;         // scene time (seconds)
    float    deltaTime;    // seconds since previous frame
    float3   cameraPos;    // camera world position
    uint     vertexID;     // resolved vertex index
    uint     instanceID;   // SV_InstanceID
    uint     materialIndex;// index into g_Materials / customParams
};

// ==== Pixel hook input (read-only) ====
struct MaterialPixelParams
{
    float3   worldPos;
    float3   geoNormalWS;     // interpolated geometry normal (front-face corrected, normalized)
    float3   tangentWS;
    float3   bitangentWS;
    float3x3 tangentToWorld;  // rows = (T, BT, N); world = mul(tangentSpace, tangentToWorld)
    float2   uv0;
    float2   uv1;
    float2   uv2;
    float2   uv3;
    float4   vertexColor;     // .rgb per-vertex color (white when absent), .a = 1
    float3   viewDirWS;       // normalize(cameraPos - worldPos)
    float    time;            // scene time in seconds (animated pixel effects)
    // The instance's local axes in world space, rows = (X, Y, Z), normalized, so a material
    // never hardcodes a world-axis convention (`float3(0,1,0)` for "up"): `p.objectAxes[1]` is
    // what this engine and this entity's transform call up. Also the port target for DCC graphs
    // that reference an object axis: Blender's radial Tangent node is about the object's Z, which
    // is row 1 after the Z-up -> Y-up import. Built from the same `mul(v, world)` the vertex
    // stage uses, so matrix majorness/packing cannot differ. Dead-code-eliminated when unread.
    float3x3 objectAxes;
    // The instance's origin in world space (the world matrix's translation).
    // `worldPos - objectOrigin` is the object-relative position that DCC nodes building a
    // positional field need (Blender's radial Tangent is a function of the generated/bbox
    // coordinate, not of the normal). Same provenance as objectAxes; DCE'd when unread.
    float3   objectOrigin;
    // Per-axis scale baked into the instance world matrix. objectAxes is normalized, so
    // dot(worldPos - objectOrigin, objectAxes[i]) is in world units; divide by this to get the
    // mesh's own object units, the space a DCC "generated"/ORCO coordinate is defined in.
    float3   objectScale;
    uint     materialIndex;
    bool     isFrontFace;
    // Raw SV_Position: .xy = pixel coordinate in the current render target, .z = device depth
    // (reverse-Z, so sky is exactly 0), .w = 1/clipW. DCE'd when unread.
    // A hook addresses screen-space engine resources (scene depth, scene colour) with it, e.g.
    // a water material's depth and refraction. Do not hand-roll a screen UV from worldPos: the
    // water_common.hlsli helpers apply the render-target size the engine rendered at, which is
    // not the display size when FSR2 upscaling is on.
    float4   svPosition;
};

// ==== Pixel hook output (the engine encodes it into the GBuffer) ====
struct SurfaceData
{
    float3 baseColor;     // albedo (linear); RT0.rgb
    float  opacity;       // RT0.a + masked alpha-test source
    float3 normalWS;      // WORLD-space shading normal (already TBN-transformed)
    float  roughness;     // [0,1]
    float  metallic;      // [0,1]
    float  ao;            // [0,1]
    float  reflectance;   // dielectric F0 = 0.16 * reflectance^2
    float3 emissive;      // linear HDR; RT5 additive seed (never passes through BRDF)
    float4 extra;         // RT4 shading-model scratch (NPR terminator/marker, SSS, ...)
    uint   shadingModel;  // informational: the GBuffer encode and deferred lighting follow the
                          // MATERIAL's customShadingModel (its stencil route, ShadingStencil::RefFor)

    // ---- Stylized direct-light additive ----
    // A light-independent scalar mask for a term that must be multiplied by the sun's radiance
    // and gated by the sun's visibility. A GBuffer pixel shader runs before any shadow exists,
    // so anything it multiplies by lightColor (and routes into s.emissive, which never passes
    // through the BRDF) shows sun-coloured energy at full brightness in a cast shadow.
    // Contract: write the mask without lightColor. The deferred pass supplies
    // `lightColor * directVisibility` (RAD-GATED in Lighting.ps.hlsl), so the term is
    // shadow-gated for free.
    // Non-face materials only. It rides RT4.a, which on a non-face pixel is otherwise 0 and is
    // tested only as `> 0.5` for the face marker, so values in [0, 0.5) still read as "not a
    // face". On a face pixel RT4 is full (r/b = SDF halves, g = hair screen mark, a = marker
    // version) and RGBA16F has no fifth channel, so a face's nose/lip spec cannot be gated
    // without a new render target or a format change; see the interim mitigation in Face.ps.hlsl.
    float  directAdditive;   // [0,1] light-independent mask; 0 = none

    // ---- ShadingModel::Anisotropic ----
    // Read only when the material's shading model is Anisotropic (the Inspector dropdown), so on
    // any other material they cost and do nothing. The engine packs them into RT4.r/.g in
    // MS_EncodeGBuffer: never write RT4 yourself for this, or the deferred decode reads garbage.
    // anisotropyDirWS is seeded with the mesh tangent rotated by the material's "Anisotropy
    // Rotation" knob, which is all most materials need. Overwrite it only for a per-pixel
    // direction (a flow map, a hair-card direction, a radial brushed disc). It need not be
    // perpendicular to normalWS or unit-length; the encoder projects and normalises.
    float  anisotropy;        // [-1,1]. 0 = isotropic. Positive widens the lobe
                              //   ALONG the tangent (Unreal's sign convention),
                              //   so strands/brush grooves usually want NEGATIVE.
    float3 anisotropyDirWS;   // world-space anisotropy axis

    // ---- ShadingModel::Subsurface ----
    // Same rule: read only on a Subsurface material, packed by the engine into RT4.r/.g. The tint
    // and overall strength are per-material (no GBuffer room for a per-pixel colour); these two
    // are the per-pixel half.
    float  subsurfaceOpacity; // [0,1]. 1 = solid/thick (tight, normal-driven
                              //   scatter); 0 = thin and see-through (broad wrap,
                              //   strong glow when backlit). Seeded from the
                              //   material; drive it from a thickness map.
    float  subsurfaceMask;    // [0,1] per-pixel scatter amount. Seeded to 1.

    // ---- ShadingModel::ClearCoat ----
    // A second, thin, always-dielectric specular layer over the base. Same rule: read only on a
    // ClearCoat material, packed by the engine into RT4.r/.g, seeded from the material so a hook
    // that ignores them still gets a correct result.
    // The coat shares normalWS with the base (no room on RT4 for a second normal): on a strongly
    // normal-mapped base the coat's reflection follows the bumps; author the coat over a smooth
    // normal if that shows.
    float  clearCoat;          // [0,1] strength. 0 = the model degrades to
                               //   Standard, exactly (lobe and attenuation both
                               //   vanish), so it is safe to leave at 0.
    float  clearCoatRoughness; // [0,1] the COAT's own roughness, independent of
                               //   `roughness` - a mirror coat over a rough base
                               //   is the reason the model exists.

    // ---- BlendMode::Transmissive ----
    // Read only on a Transmissive material (a forward draw: no GBuffer channel), seeded from
    // the material. `opacity` stays coverage; these say what crosses a covered pixel.
    float  transmission;       // [0,1] share of the dielectric base that refracts; drive it
                               //   with a mask to keep frosted labels on a clear bottle
    float  thickness;          // world units through the volume; 0 = a thin wall
};

// RT4.a encoding for the direct-additive channel. 0.49 keeps the whole range strictly below
// the 0.5 face-marker threshold, with headroom for the FP16 rounding a linear Sample
// introduces at a face/non-face silhouette edge.
#define DIRECT_ADDITIVE_ENCODE_SCALE 0.49

#endif // MATERIAL_TYPES_HLSLI
