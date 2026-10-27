// light_cb_fields.hlsli - LightCB's member list, and the only copy of it in HLSL.
//
// Included inside a declaration, by light_cb.hlsli only: the `cbuffer LightCB`
// every graphics consumer binds, or the `struct LightCB` a shader opens from the
// heap under LIGHT_CB_AS_STRUCT. Hence no include guard and no declaration of its
// own: include light_cb.hlsli, never this file.
//
// Mirrors: RendererDetail::LightCB (include/Renderer/Renderer.h), the
// kLightCB_* offsets (RootSignatureLayout.h), and LayoutSpecs.cpp's kLightCB
// spec, which checks every member against each graphics shader's reflection.
// Add or move a member in all of them together.

    // ---- Direct directional light + camera + IBL params ----
    float3   lightDir;          float _p0;
    float3   lightColor;        float _p1;
    float3   cameraPos;         float _p3;
    float4x4 invViewProj;
    uint     iblRadianceMips;
    float    iblStrength;
    uint     iblUseSH;          // 0 = sample gIrradiance cubemap, 1 = evaluate gSkySH
    float    aerialMaxDistKm;   // Aerial Perspective LUT far slice (0 = disabled)

    // ---- CSM shadow data - 4 cascades ----
    // Cascades 0..2 are the standard near cascades; cascade 3 is a dedicated ultra-far
    // cascade for terrain self-shadow (mountain silhouettes onto the plain at
    // sunrise/sunset).
    float4x4 shadowMatrix[4];          // per-cascade light VP (row-major, transposed)
    float4   cascadeSplits;            // view-Z far boundary of each cascade
    float4   cascadeTexelWorldSize;    // world m/texel per cascade (receiver bias)
    float    shadowBias;               // constant receiver compare bias (NDC, shadow.hlsli)
    float    shadowStrength;           // 0 = disabled, 1 = full shadows
    float    shadowMapTexelSize;       // 1.0 / kShadowMapSize, precomputed
    float    shadowBlendRange;         // cascade blend zone width in view-Z (world units)
    uint     shadowFrameIndex;         // per-frame PCF dither rotation key (TAA smoothing)
    float    shadowNormalOffset;       // receiver normal-offset strength (texels), 0 disables
    // RT sun-shadow mask gate. Non-zero -> shadow.hlsli min()s the RT sun-shadow mask
    // (ResourceDescriptorHeap[rtShadowMaskIdx]) into the CSM result. Renderer sets it
    // only on frames RTShadowPass produced a mask (rtShadowActive), so a stale texture
    // is never sampled; the no-mask fallback is 1x1, so this flag is also what keeps a
    // full-screen Load() in bounds.
    float    rtShadowEnabled;
    // ---- SSGI fold ----
    // ssgiResultIdx: heap index of SSGIPass's RGBA16F output. rgb = irradiance over the
    // solid angle the screen trace resolved; a = that resolved fraction in [0,1].
    // 0 = no SSGI this frame (kill switch; the composition is bit-identical).
    // Do not treat `a` as a confidence: it is an integral over many rays, smooth and
    // surviving denoising. Do not use it to lerp between two already-denoised
    // estimates; that shape produces artifacts on the reflection side.
    uint     ssgiResultIdx;
    float3   cameraForward;            // unit camera forward (view-Z cascade selection)
    float    ssgiIntensity;

    // ---- Clustered lighting params ----
    float4x4 viewMatrix;        // world -> view, used to pick clusters by view-Z
    float    clusterNearZ;
    float    clusterFarZ;
    uint     clusterLightCount;
    float    nprMinBrightness;  // NPR minimum brightness floor (0 = unused for PBR)

    // ---- Reflection-probe pool ----
    // Number of valid entries in the probe buffer (reflProbeBufIdx) - 0 -> skip the
    // probe loop and fall back to gRadiance only. Also the gate the probe grid, index
    // list and buffer opens live inside.
    uint     reflectionProbeCount;
    // Scene-wide absolute HDR ceiling for NPR pixels (0 = none). min()s with the
    // per-material MaterialGPUData.nprMaxBrightness - the tighter wins, as
    // nprMinBrightness max()es with the per-material floor. Protects bloom /
    // AutoExposure from a single blown-out NPR pixel, a scene-level concern the
    // per-material field cannot cover. Source:
    // IndirectLightingSettingsComponent::nprMaxBrightness.
    float    nprMaxBrightness;
    float2   _reflectionPad;

    // ---- DDGI integration ----
    // Number of bound DDGI volumes (0..DDGI_MAX_VOLUMES). 0 -> skip the DDGI sampling
    // block in Lighting.ps and use Sky IBL diffuse only.
    uint     ddgiVolumeCount;
    // Master DDGI on/off + global scales - mirrors IndirectLightingSettings.
    uint     ddgiEnabled;          // 0 / 1
    float    ddgiDiffuseScale;
    // Unused slot kept so the offsets after it stay put (the sky light has one
    // intensity, iblStrength).
    float    _padSkyDiffuse;

    // Near-field AO strength on the DDGI diffuse path. DDGI already integrates
    // probe-distance visibility (mid-scale occlusion), so full screen-space AO on top
    // double-darkens. Attenuates AO on the DDGI portion only; the Sky-IBL fallback
    // portion still gets full AO.
    //   0.0 = no AO on DDGI (trust probes)
    //   1.0 = full AO on DDGI (double-occludes)
    //   ~0.4 = recommended balance (keeps near-field contact darkening)
    float    ddgiAONearFieldStrength;
    // Global view-mode switch (Lit/Unlit/Wireframe). VIEW_MODE_* enum in
    // view_mode_common.hlsli; C++ mirror Renderer::ViewMode.
    uint     viewMode;
    // Shader-visible-heap index of the AutoExposure StructuredBuffer<float>
    // (1 element, the current scene exposure). 0 = unavailable - heap index 0 is a real
    // descriptor, so a consumer gates on != 0 before reading it. Opened bindlessly like
    // reflProbeBufIdx:
    //     StructuredBuffer<float> b = BindlessSBuf<float>(exposureBufIdx);
    // The graphics root signature sets CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED, so this costs
    // no root parameter (reflection_probe.hlsli's dual-path note says which signatures
    // may do this).
    // The forward unlit branch in custom_surface.hlsli counter-scales against this live
    // GPU value: forward draws composite after NPRLitSnapshot, so NPRStabilize never
    // reaches them and the protection has to be applied in the pixel shader.
    uint     exposureBufIdx;
    float    ambientIntensity;   // WorldEnvironment flat-ambient scale (None/FlatColor)

    // ---- Per-object (player) high-res directional shadow ----
    // Sampled by shadow.hlsli::SamplePerObjectShadow only when the consumer
    // #defines PER_OBJECT_SHADOW and perObjShadowParams.x != 0 (player present).
    float4x4 perObjShadowMatrix;       // light VP (row-major, transposed) fitted to player AABB
    float4   perObjShadowParams;       // x=enabled y=texelWorld z=receiverBias w=normalOffset
    float    perObjShadowTexelSize;    // 1.0 / kPerObjShadowSize (PCF tap step)
    float3   ambientColor;             // WorldEnvironment flat ambient (None/FlatColor)

    // ---- Hair (bangs -> face) directional shadow ----
    // Small map fitted to the tagged bangs subtree (HairShadowCasterComponent); sampled
    // by shadow.hlsli::SampleHairShadow only when the consumer #defines HAIR_SHADOW and
    // hairShadowParams.x != 0. Lighting.ps calls it only on NPR face pixels (extra.a
    // marker), where it min()s into the SDF face terminator.
    float4x4 hairShadowMatrix;         // light VP (row-major, transposed) fitted to bangs AABB
    float4   hairShadowParams;         // x=enabled y=texelWorld z=receiverBias w=normalOffset
    float    hairShadowTexelSize;      // 1.0 / kHairShadowMapSize (PCF tap step)
    float    hairShadowStrength;       // artistic strength of the bangs shadow on the face

    // ---- Descriptor-heap indices ----
    // The graphics root signature must stay within the D3D12 limit of 64 DWORDs; a
    // shader-visible heap index here read through ResourceDescriptorHeap[idx] costs no
    // root DWORD.
    // Requires SM6.6, Resource Binding Tier 3 and the root signature's
    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED flag; device init hard-fails without the first
    // two (GraphicsDX12.cpp).
    //
    // These come from a constant buffer, so they are wave-uniform: do not wrap them in
    // NonUniformResourceIndex(), that only buys a scalarization loop. (Indices derived
    // from them by adding a per-pixel loop variable - the DDGI block bases - do need
    // it.)
    //
    // The declared type at each use site must match the SRV the C++ side created
    // exactly: under Tier 3 a mismatch produces no D3D12 validation message, just
    // garbage or a TDR. Each field below names its type.
    //
    // Offsets 776..812 are pinned in Renderer.h's LightCB by static_assert against
    // RHI::Slots::BindlessIdx. DDGIVolumeGPU (DDGICommon.hlsli) is a second carrier,
    // pinned the same way against RHI::Slots::BindlessIdx::kDDGIVolumeDesc_*.

    // RTShadowPass sun-visibility mask (1 = lit), Texture2D<float>. Read by
    // shadow.hlsli under RT_SHADOW_MASK and only when rtShadowEnabled != 0: the
    // fallback texture is 1x1, so a full-screen Load() against it would be out of
    // bounds for every pixel but (0,0).
    uint     rtShadowMaskIdx;
    // RTReflectionPass denoised output (rgb radiance, a rtConf), Texture2D<float4>.
    // Read unconditionally by Lighting.ps; the fallback is 1x1 black, so rtConf == 0
    // and the specular composite keeps its non-traced tiers.
    uint     rtReflectionIdx;

    // ---- Shadow maps ----
    // PointShadowPass omnidirectional cube atlas, TextureCubeArray<float>,
    // SampleCmpLevelZero against gShadowSampler (s2, root-bound: ResourceDescriptorHeap
    // covers CBV/SRV/UAV only). Opened by Lighting.ps inside SamplePointShadow, which
    // the clustered loop calls only for `type == 1 && shadowSliceIdx != ~0u`. The
    // fallback is a real 1x1 12-slice cube array, so it samples in bounds regardless.
    // Not in g_AllTextures[]: that array is Texture2D and cannot hold a cube-array SRV.
    uint     pointShadowIdx;
    // PerObjectShadowPass high-res player map, Texture2D<float>, SampleCmpLevelZero.
    // Opened by shadow.hlsli::SamplePerObjectShadow, which ComputeShadowFactor calls
    // only when perObjShadowParams.x != 0.
    uint     perObjShadowIdx;
    // Hair (bangs -> face) map, Texture2D<float>, SampleCmpLevelZero. Opened by
    // shadow.hlsli::SampleHairShadow, which Lighting.ps calls only on NPR face pixels
    // and only when hairShadowParams.x != 0. Shares the 1x1 depth-0.0 fallback with
    // perObjShadowIdx (reversed-Z: 0 = no occluder = lit, the no-op value under a
    // GREATER_EQUAL comparison sampler).
    uint     hairShadowIdx;

    // ---- DDGI volumes ----
    // The engine-wide StructuredBuffer<DDGIVolumeGPU>, opened by Lighting.ps inside
    // the `ddgiEnabled != 0 && ddgiVolumeCount > 0` gate.
    // The entry point to DDGI: the per-volume probe SH, depth atlas and probe data get
    // no field here; each block's base heap index rides in this buffer's elements
    // (DDGIVolumeGPU::irradianceBaseIdx / depthBaseIdx / probeDataBaseIdx,
    // DDGICommon.hlsli). The shader opens this buffer, loads the volume, then opens
    // `base + volumeSlot` for its three resources - and those do need
    // NonUniformResourceIndex(), because the volume slot is a per-pixel loop variable.
    // This field, from a CB, does not.
    // The buffer is triple-buffered on the CPU side; the index is re-resolved every
    // frame from the current ring slot's SRV. With DDGI off Renderer writes 0 (a real
    // descriptor the gate keeps the shader from reading).
    uint     ddgiVolumeBufIdx;

    // ---- Per-cluster reflection-probe list ----
    // ClusterPass probe grid: StructuredBuffer<ProbeGridEntry>, ProbeGridEntry =
    // { uint offset; uint count; } - StructureByteStride 8, matching the buffer
    // ClusterPass creates and the null SRV LightingPass builds for the absent case.
    uint     reflProbeGridIdx;
    // The flat probe index list the grid points into: StructuredBuffer<uint>,
    // StructureByteStride 4.
    // Both are opened by Lighting.ps inside the `reflectionProbeCount > 0` gate; both
    // come from this CB, so no NonUniformResourceIndex().
    // When ClusterPass has not produced the buffers, Renderer writes the index of a
    // type-correct null StructuredBuffer SRV, never of some convenient texture: the
    // read is `StructuredBuffer<T> b = ResourceDescriptorHeap[i]`, and a descriptor of
    // the wrong dimension there is UB with no debug-layer message under Tier 3.
    uint     reflProbeIndexIdx;

    // ---- SSR result ----
    // SSRUpsamplePass color output, Texture2D<float4>: .rgb = resolved screen-space
    // reflection, .a = confidence. Opened only by Water.ps.hlsl, inside its
    // `!underwater && prevClip.w > 1e-3` block, to dampen the sky reflection by
    // (1 - conf); from this CB, so no NonUniformResourceIndex(). Lighting.ps does not
    // read it: its whole traced signal arrives same-frame through rtReflectionIdx.
    // The texture is recreated on a render-resolution change (SSRSubsystem::OnResize),
    // so Renderer re-derives this index every frame in its resolve() block, after
    // OnResize and before graph.Execute. With SSR off Renderer writes the index of
    // WaterPass's 1x1 black texture: alpha 0, so the dampening is a no-op.
    uint     ssrResultIdx;

    // ---- Reflection-probe buffer ----
    // ReflectionProbeManager's StructuredBuffer<ReflectionProbe>, StructureByteStride
    // 192 (12 x float4), matching `struct ReflectionProbe` byte for byte.
    // Opened by both graphics consumers, each inside its own gate:
    //   Lighting.ps          - `if (reflectionProbeCount > 0)`, the same block that
    //                          opens reflProbeGridIdx / reflProbeIndexIdx.
    //   custom_surface.hlsli - the MS_IS_FORWARD IBL block
    //                          `if (iblRadianceMips > 0 || reflectionProbeCount > 0)`.
    // From this CB, so no NonUniformResourceIndex(); only `pi`, fed to the probe's own
    // cubemapSrvIndex, needs it, and SampleProbe applies it.
    // Compute consumers (ClusterCullProbes.cs, HybridReflIntersectHW/SW.cs) get the
    // buffer's index through their own constants, not from LightCB.
    // The buffer is a 3-slot upload ring (one per frame in flight); all three SRVs are
    // created in ReflectionProbeManager::Init and picked per frame by
    // GetBufferSrv(gfx) -> gfx.GetFrameIndex(). So Renderer re-resolves this index
    // every frame and writes it unconditionally: LightCB is itself a ring, and a
    // skipped write leaves the index kFrameSlots frames stale, naming the wrong slot.
    // "Absent" (the manager failed to create the buffer) is a type-correct null
    // StructuredBuffer SRV of stride 192 built in LightingPass::Init. Never substitute
    // a texture: opening a StructuredBuffer through a TextureCubeArray descriptor is UB
    // that Resource Binding Tier 3 never reports.
    uint     reflProbeBufIdx;

    // ---- Water domain (LightCB 816 -> 1104) ----
    // Mirrors: RendererDetail::LightCB (Renderer.h) and
    // RHI::Slots::BindlessIdx::kLightCB_* (RootSignatureLayout.h); move them together.
    //
    // .x = refraction source bindless index (lit opaque scene), 0 = unbound
    // .y = scene depth snapshot bindless index,                 0 = unbound
    // .z = number of valid entries in gWaterBodies (0..WATER_MAX_RIPPLE_BODIES)
    // .w = unused (the render-target size is gWaterRTSize)
    uint4    gWaterGlobals;
    // Render-target size the frame was rasterised at, in its own row so the water
    // helpers never guess the size under FSR2 (render res != display res).
    float4   gWaterRTSize;        // .xy = width,height  .zw = reserved

    // Ripple bodies, 2 rows each (see WaterRippleSlope in water_common.hlsli):
    //   [2i+0] .xyz = body world origin/level, .w = body world size
    //   [2i+1] .xy  = ripple window world origin, .z = window world size,
    //           .w  = asfloat(ripple field bindless index), 0 = no field
    // Sized for WATER_MAX_RIPPLE_BODIES (8), WaterRippleSystem's slot pool, which
    // caps how many bodies can have a height field.
    float4   gWaterBodies[16];

    // ---- Volumetric cloud shadow (LightCB 1104 -> 1152) ----
    // Mirrors: RendererDetail::LightCB (Renderer.h) and kLightCB_CloudShadow* /
    // kLightCB_Size (RootSignatureLayout.h); move them together.
    // Baked by CloudShadowBake.cs.hlsl, read through shaders/cloud_shadow.hlsli (the
    // projection math and the binding-model rules live there). Both basis rows are
    // pre-divided by the projection extent with the centre folded into .w, so a
    // receiver's UV costs two dot products.
    float4   gCloudShadowRight;   // xyz = right / (2*extent), w = 0.5 - dot(centre, xyz)
    float4   gCloudShadowUp;      // xyz = up    / (2*extent), w = 0.5 - dot(centre, xyz)
    // Bindless SRV index of the R16_FLOAT shadow map. 0 = unbound (clouds off,
    // bake did not run, or strength 0) -> SampleCloudShadow returns full sun.
    uint     gCloudShadowIdx;
    // PT-lit toon receivers: PathTracePass's denoised NprAmbient output
    // (Texture2D<float4>: rgb sphere-mean radiance, a sun visibility, a < 0 = not a
    // receiver), read by Lighting.ps's NPR variant on the path-trace path. 0 = none.
    // Written unconditionally every frame.
    uint     gNprPtAmbientIdx;
    // Face frame records: this frame's StructuredBuffer<float4> by material slot -
    // (HeadForward.xz, HeadRight.xz) of a PER_FRAME_PARAMS face - read by Lighting.ps's
    // NPR variant where the material's MAT_FLAG_FRAME_RECORD is set. 0 = none.
    uint     gFaceFrameIdx;
    // 1 when TransmissionApply wrote the refracted, tinted background under this frame's
    // Transmissive surfaces: their colour pass then adds only their own light. 0 = it did
    // not run (a preview, the pass off): they blend over the plain background by coverage.
    // Written unconditionally every frame.
    uint     gTransmissionApplied;

    // ---- Forward translucent traced-reflection fold (LightCB 1152 -> 1168) ----
    // Mirrors: RendererDetail::LightCB (Renderer.h) and kLightCB_FwdRefl* /
    // kLightCB_Size (RootSignatureLayout.h); move them together.
    //
    // Consumed only by custom_surface.hlsli's translucent main
    // (MS_FetchTracedReflection), which folds the same-frame RTReflectionPass output
    // (rtReflectionIdx) into a transparent surface's indirect specular where the opaque
    // surface behind the pixel is near-coplanar and close - the traced ray belonged to
    // that surface, and those two tests make reusing it defensible. Both textures are
    // the deferred GBuffer's, opened bindless:
    //   gFwdReflNormalIdx  Texture2D<float4>  GBuffer1 normal (N*0.5+0.5 in .xyz)
    //   gFwdReflDepthIdx   Texture2D<float>   SSRTraceDepth snapshot (reverse-Z
    //                                          NDC, grass-free, sky exactly 0.0)
    // 0 = unbound (no TransparentPass, or the feature is off) and the fold is
    // skipped - heap index 0 is a real descriptor, so consumers test `!= 0`.
    // gFwdReflStrength is features.transparentReflections as a scalar weight:
    // 0 disables the fold bit-exactly. CB scalars, wave-uniform: no
    // NonUniformResourceIndex().
    uint     gFwdReflNormalIdx;
    uint     gFwdReflDepthIdx;
    float    gFwdReflStrength;
    // Previous-frame SceneColorPyramid (SSRSubsystem; mipped RGBA16F, the
    // reflection-free lit opaque scene - no sky, no transparents). 0 when SSR is off or
    // the pyramid is not built yet; the translucent surface's own screen-space trace
    // (MS_ScreenTraceReflection) is then skipped and only the coplanar reuse remains.
    uint     gFwdReflPrevHdrIdx;

    // ---- Forward translucent screen trace (LightCB 1168 -> 1200) ----
    // The translucent surface's own screen-space reflection march (custom_surface.hlsli
    // MS_ScreenTraceReflection): the pyramid's top mip, the camera planes the reverse-Z
    // linearisation needs, the roughness above which no ray is traced (Renderer's
    // m_rtrMaxRoughness - engine.json `raytracing.reflectionsMaxRoughness`, a dev
    // setting, not a player one), and the world-space ray range
    // (features.rtReflectionsRayTMax).
    float    gFwdReflPrevHdrMaxMip;
    float    gFwdReflNearZ;
    float    gFwdReflFarZ;
    float    gFwdReflRoughMax;

    float    gFwdReflRayMax;
    // Last frame's TAA/upscaler jitter in UV units (jitterPx / renderRes). The march
    // reprojects a hit through PerViewCB.prevViewProj, the unjittered previous matrix,
    // but the pyramid it samples was rasterised with the jittered one - add this back
    // to land on the pyramid's pixel grid. 0 when jitter is off.
    float2   gFwdReflPrevJitterUV;
    // The front-layer trace: RTReflectionPass's second instance, traced from the
    // nearest translucent surface (TranslucentFrontLayerPass stamp).
    // Texture2D<float4>, rgb = reflected radiance, a = conf. 0 = no front-layer
    // instance ran this frame (RT tier off, no eligible translucent draw).
    uint     gFwdReflFrontRtrIdx;

    // ---- Forward translucent front layer (LightCB 1200 -> 1216) ----
    // The front-layer depth target (Texture2D<float>, reverse-Z NDC, sky = 0): a
    // fragment is the front layer when its own SV_Position.z matches it
    // (MS_FetchFrontLayerReflection); deeper translucent layers keep the screen tiers.
    // 0 = unbound. Written unconditionally every frame.
    uint     gFwdReflFrontDepthIdx;
    // The screen AO texture LightingPass samples at t18 this frame (RTAO
    // output or XeGTAO aoFinal, render res, Texture2D<float>); 0 = none. The
    // forward BRDF folds it by opaque-surface proximity (MS_ForwardScreenAO).
    uint     gFwdAoIdx;
    uint     gRestirDiIdx;
    uint     gPtResultIdx;

    // ---- CSM cascade-selection camera (LightCB 1216 -> 1248) ----
    // ComputeCascadeShadowTerm (shadow.hlsli) picks a cascade from
    // dot(worldPos - gCsmCamPos, gCsmCamFwd): the camera the cascade matrices and
    // splits were fitted to, which is not always the shading camera - a secondary
    // preview view (PiP / camera preview) borrows the main camera's cascades while
    // overriding cameraPos / cameraForward for its own specular V. The main path
    // writes these equal to cameraPos / cameraForward.
    float3   gCsmCamPos;
    uint     gRenderPathMode;
    float3   gCsmCamFwd;
    uint     gRestirGiIdx;
