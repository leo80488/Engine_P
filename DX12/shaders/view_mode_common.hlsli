#ifndef VIEW_MODE_COMMON_HLSLI
#define VIEW_MODE_COMMON_HLSLI

// Constants of the global view-mode switch (the editor's Lit / Unlit / Wireframe and
// debug-view dropdown). The active mode is LightCB.viewMode (light_cb.hlsli), so any
// shader that includes light_cb.hlsli can branch on it. C++ mirror: Renderer::ViewMode
// (include/Renderer/Renderer.h).
//
//   Lit       - normal shading (default; no override).
//   Unlit     - base color only, no BRDF.
//   Wireframe - geometry PSOs rasterize edges only (FILL_MODE_WIREFRAME) and the shaded
//               edge pixels get one flat colour, so the wireframe reads as uniform lines
//               on a dark background.

#define VIEW_MODE_LIT       0u
#define VIEW_MODE_UNLIT     1u
#define VIEW_MODE_WIREFRAME 2u

// ==== Lighting-taxonomy debug views (3..11) ====
// Each isolates one bucket of AggregateLighting (lighting_types.hlsli) or one of the two
// visibility quantities: sun-coloured energy surviving in a fully shadowed NPR pixel is
// invisible in Lit and obvious in DirectDiffuse alone.
//
// They reuse spare values of the existing LightCB.viewMode field on purpose: a new
// LightCB field costs a fresh 16 B row plus synchronised bumps across
// RootSignatureLayout.h (kLightCB_Size) / Renderer.h / light_cb.hlsli, which a debug
// feature does not get to spend.
//
// Bucket views:
#define VIEW_MODE_DIRECT_DIFFUSE      3u
#define VIEW_MODE_DIRECT_SPECULAR     4u
#define VIEW_MODE_DIRECT_TRANSMISSION 5u
#define VIEW_MODE_INDIRECT_DIFFUSE    6u
#define VIEW_MODE_INDIRECT_SPECULAR   7u
#define VIEW_MODE_DIRECT_ONLY         8u
#define VIEW_MODE_INDIRECT_ONLY       9u
// The taxonomy itself, side by side. PHYSICAL is what an occlusion query returned; FORM
// is authored surface shading. Seeing the two apart is the point: confusing them is what
// leaves sun-coloured energy in a fully shadowed NPR pixel.
#define VIEW_MODE_PHYSICAL_VISIBILITY 10u
#define VIEW_MODE_FORM_SHADOW         11u

// Is this mode one of the lighting-bucket isolations? Lighting.ps early-outs on it, and
// the C++ side suppresses the environment passes and TAA so the isolated bucket is
// readable. The sky views (20, 21) are defined below; a macro expands where it is used,
// so naming them here is fine.
#define VIEW_MODE_IS_LIGHTING_DEBUG(m) (((m) >= VIEW_MODE_DIRECT_DIFFUSE && (m) <= VIEW_MODE_FORM_SHADOW) \
                                        || (m) == VIEW_MODE_SKY_VISIBILITY || (m) == VIEW_MODE_IBL_ONLY)

// ==== GBuffer channel visualisation (12..17) ====
// Raw GBuffer contents, bypassing the BRDF: the first look at "why is this pixel the wrong
// colour" without RenderDoc. Same LightCB.viewMode reuse as the lighting-debug block. No
// velocity view on purpose: LightingPass does not bind the velocity target, registers
// t23..t45 are reserved (RootSignatureLayout.h), and a debug view does not get to spend a
// root-signature change.
#define VIEW_MODE_BASECOLOR    12u
#define VIEW_MODE_WORLDNORMAL  13u
#define VIEW_MODE_ROUGHNESS    14u
#define VIEW_MODE_METALLIC     15u
#define VIEW_MODE_AO           16u
#define VIEW_MODE_DEPTH        17u

#define VIEW_MODE_IS_GBUFFER_DEBUG(m) ((m) >= VIEW_MODE_BASECOLOR && (m) <= VIEW_MODE_DEPTH)

// Meshlet views (18..19). Listed for the mirror only: the CPU uploads VIEW_MODE_BASECOLOR
// for both (GBufferPass draws every opaque packet with MeshletDebug.ps, whose albedo is the
// debug colour), so no shader ever sees these two values.
#define VIEW_MODE_MESHLETS     18u
#define VIEW_MODE_MESHLET_LOD  19u

// The sky's share of the indirect light (20..21). Lighting-debug views like 3..11
// (VIEW_MODE_IS_LIGHTING_DEBUG), numbered after the meshlet pair only because the values
// are append-only:
//   SKY_VISIBILITY  what the sky-IBL fallback is multiplied by, knobs excluded:
//                   R = diffuse  (1 - DDGI coverage) * AO * DDGI's sky visibility
//                       about N (1 outside volumes) * (1 - SSGI / ReSTIR GI coverage)
//                   G = specular sky visibility (DDGI's, along R) * horizon
//                       * specular occlusion
//                   B = the share of the diffuse hemisphere a GI technique owns
//                   Without GI, R and G are near 1 indoors: nothing occludes the sky.
//   IBL_ONLY        the radiance the sky-IBL fallback terms deliver (sky diffuse + sky
//                   specular, coat included) - not DDGI / SSGI / ReSTIR GI, probes,
//                   traced reflections or the flat ambient, even where their light came
//                   from the sky.
#define VIEW_MODE_SKY_VISIBILITY 20u
#define VIEW_MODE_IBL_ONLY       21u

// Flat colour of every wireframe edge pixel. Linear, through the same tonemap as the
// scene, bright enough to stay readable after exposure / tonemapping.
#define WIREFRAME_COLOR float3(0.10, 0.90, 0.75)

#endif // VIEW_MODE_COMMON_HLSLI
