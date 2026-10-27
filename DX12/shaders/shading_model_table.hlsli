// One row per ShadingModel: the decisions other code makes about a model.
//
// What belongs here: questions a caller asks about a model it does not implement -
// which deferred lighting variant draws it (the GBuffer stencil code), whether ReSTIR DI
// lights it, whether ReSTIR GI receives on it, whether the RT proxy needs its material
// slot, which material-graph output pins it reads, which editor menu offers it and what
// selecting it also sets.
//
// What does not: the code that is a model - Lighting.ps's Unlit early-out, the
// anisotropic BRDF, the RT4 codec (shading_models.hlsli), the toon ramp - keeps its own
// `model == SHADING_MODEL_X` compares: that code defines X, and a table lookup there
// would only add an indirection.
//
// One file, both languages. C++ includes it as "../../shaders/shading_model_table.hlsli"
// (include/Graphics/ShadingModelTable.h builds the constexpr table and static_asserts
// every row against the ShadingModel enum); HLSL includes it by name and gets the masks
// and helpers at the bottom. It lives under shaders/ so a packaged build's runtime DXC
// can resolve it (game.ipak carries that tree; see morph_limits.hlsli).
//
// Rules for editing:
//   * Rows in enum-value order, dense, one per ShadingModel (C++ asserts it). The value
//     is serialized: a new model is appended, never inserted.
//   * Keep this file pure ASCII: the offline SDK dxc reads it in the system code page.
//   * No token pasting (the double-hash operator) in this file's code:
//     gen_perm_relevance.py treats it anywhere in a shader's include closure as "every
//     permutation bit may matter" and widens that shader's row (more PSOs). The C++ side
//     stringizes in its own header instead.
//   * `display` must not change spelling: generated material HLSL records it
//     ("// Shading model Clear Coat, ...") and ParseGeneratedHeader matches it.

#ifndef SHADING_MODEL_TABLE_HLSLI
#define SHADING_MODEL_TABLE_HLSLI

// route: the deferred lighting variant = the GBuffer stencil code
// (ShadingStencil::kPBR / kNPR / kUnlit; LightingPass tests it EQUAL).
#define SMT_ROUTE_PBR   1
#define SMT_ROUTE_NPR   2
#define SMT_ROUTE_UNLIT 3

// di: does ReSTIR DI light it (RTDI.cs's receiver, Lighting.ps's fold).
#define SMT_DI_NEVER     0
#define SMT_DI_ALWAYS    1
#define SMT_DI_ISOTROPIC 2   // only where the pixel is isotropic (each shader decides that)

// pins: material-graph output pins the model reads, by group. BaseColor,
// Opacity and OpacityMask depend on the blend mode and stay rules in
// MaterialGraph::OutputPinActive; EmissiveColor, Normal, AmbientOcclusion and
// WorldPositionOffset are read by every model.
#define SMT_PIN_METALLIC_SPECULAR 1    // Metallic, Specular
#define SMT_PIN_ROUGHNESS         2    // Roughness
#define SMT_PIN_ANISOTROPY        4    // Anisotropy, Tangent
#define SMT_PIN_SUBSURFACE        8    // SubsurfaceOpacity, SubsurfaceMask
#define SMT_PIN_CLEARCOAT         16   // ClearCoat, ClearCoatRoughness
#define SMT_PIN_DIRECT_ADDITIVE   32   // DirectAdditive
#define SMT_PINS_PBR (SMT_PIN_METALLIC_SPECULAR | SMT_PIN_ROUGHNESS)

// menus: the Inspector's Shading Model combos that offer it.
#define SMT_MENU_ENGINE 1    // a material without a custom shader
#define SMT_MENU_CUSTOM 2    // a material with one
#define SMT_MENU_BOTH   (SMT_MENU_ENGINE | SMT_MENU_CUSTOM)

// onSelect: what picking it in a combo also sets, because the model is inert
// without it (the user can change each afterwards).
#define SMT_SELECT_DOUBLE_SIDED 1    // the far side must not be culled
#define SMT_SELECT_TRANSLUCENT  2    // an Opaque/Masked/Dithered blend becomes Alpha
#define SMT_SELECT_NO_SHADOW    4    // castShadow off

// Path support - declared, read by no decision: the Material Editor's support list.
// Every APPROX cell has a one-line note in C++ (ShadingModels::kPathNotes,
// ShadingModelTable.h) saying what the path draws instead and where; a NONE cell may
// have one.
//   deferred   the opaque route (GBuffer + Lighting.ps)
//   forward    the translucent route (custom_surface.hlsli's forward main)
//   rtHit      what a ray sees on it: RT reflections, RTGI and RTXDI bounces
//   pathTrace  the path tracer, primary and bounces
//   ddgi       the DDGI probe trace
#define SMT_PATH_NONE   0    // the path does not draw it
#define SMT_PATH_FULL   1
#define SMT_PATH_APPROX 2

// X(name, value, token, display, route, di, gi, rtMatSlot, pins, menus, onSelect,
//   deferred, forward, rtHit, pathTrace, ddgi)
//
//   gi        ReSTIR GI receives on it (RTGI.cs's receiver)
//   rtMatSlot the RT/DDGI proxy carries its raster material slot
//             (DDGISceneAS: Toon's NPR knobs, Unlit's exposure compensation)
#define SHADING_MODEL_TABLE(X) \
    X(Standard,        0, "standard",        "Standard",          SMT_ROUTE_PBR,   SMT_DI_ALWAYS,    1, 0, \
      SMT_PINS_PBR,                                 SMT_MENU_BOTH,   0, \
      SMT_PATH_FULL, SMT_PATH_FULL, SMT_PATH_FULL, SMT_PATH_FULL, SMT_PATH_FULL) \
    X(Unlit,           1, "unlit",           "Unlit",             SMT_ROUTE_UNLIT, SMT_DI_NEVER,     0, 1, \
      0,                                            SMT_MENU_BOTH,   0, \
      SMT_PATH_FULL, SMT_PATH_APPROX, SMT_PATH_FULL, SMT_PATH_FULL, SMT_PATH_APPROX) \
    X(ClearCoat,       2, "clearcoat",       "Clear Coat",        SMT_ROUTE_PBR,   SMT_DI_NEVER,     1, 0, \
      SMT_PINS_PBR | SMT_PIN_CLEARCOAT,             SMT_MENU_BOTH,   0, \
      SMT_PATH_FULL, SMT_PATH_FULL, SMT_PATH_APPROX, SMT_PATH_APPROX, SMT_PATH_APPROX) \
    X(Subsurface,      3, "subsurface",      "Subsurface",        SMT_ROUTE_PBR,   SMT_DI_NEVER,     1, 0, \
      SMT_PINS_PBR | SMT_PIN_SUBSURFACE,            SMT_MENU_BOTH,   0, \
      SMT_PATH_FULL, SMT_PATH_FULL, SMT_PATH_APPROX, SMT_PATH_APPROX, SMT_PATH_APPROX) \
    X(Anisotropic,     4, "anisotropic",     "Anisotropic",       SMT_ROUTE_PBR,   SMT_DI_ISOTROPIC, 1, 0, \
      SMT_PINS_PBR | SMT_PIN_ANISOTROPY,            SMT_MENU_BOTH,   0, \
      SMT_PATH_FULL, SMT_PATH_FULL, SMT_PATH_APPROX, SMT_PATH_APPROX, SMT_PATH_FULL) \
    X(Water,           5, "water",           "Water",             SMT_ROUTE_PBR,   SMT_DI_NEVER,     1, 0, \
      SMT_PINS_PBR,                                 SMT_MENU_CUSTOM, \
      SMT_SELECT_TRANSLUCENT | SMT_SELECT_DOUBLE_SIDED | SMT_SELECT_NO_SHADOW, \
      SMT_PATH_NONE, SMT_PATH_FULL, SMT_PATH_NONE, SMT_PATH_NONE, SMT_PATH_NONE) \
    X(TwoSidedFoliage, 6, "twosidedfoliage", "Two Sided Foliage", SMT_ROUTE_PBR,   SMT_DI_NEVER,     1, 0, \
      SMT_PINS_PBR | SMT_PIN_SUBSURFACE,            SMT_MENU_BOTH,   SMT_SELECT_DOUBLE_SIDED, \
      SMT_PATH_FULL, SMT_PATH_FULL, SMT_PATH_APPROX, SMT_PATH_APPROX, SMT_PATH_APPROX) \
    X(Toon,            7, "toon",            "Toon / NPR",        SMT_ROUTE_NPR,   SMT_DI_NEVER,     0, 1, \
      SMT_PIN_ROUGHNESS | SMT_PIN_DIRECT_ADDITIVE,  SMT_MENU_BOTH,   0, \
      SMT_PATH_FULL, SMT_PATH_APPROX, SMT_PATH_APPROX, SMT_PATH_APPROX, SMT_PATH_APPROX)

#ifndef __cplusplus
// ==== HLSL: the decisions shaders make ====
// Each helper is a chain the preprocessor writes with one term per row; the terms of
// rows the decision does not name are constant false (or true) and fold away, so what
// compiles is the plain compare a hand-written shader would have:
// `model == 0 || (model == 4 && isotropic)`. A value outside the enum is never a DI
// receiver and is a GI receiver.

#define SMT_DI_TERM(name, value, token, display, route, di, gi, slot, pins, menus, sel, d0, d1, d2, d3, d4) \
    || ((di) == SMT_DI_ALWAYS && model == (value)) \
    || ((di) == SMT_DI_ISOTROPIC && model == (value) && isotropic)
#define SMT_GI_TERM(name, value, token, display, route, di, gi, slot, pins, menus, sel, d0, d1, d2, d3, d4) \
    && ((gi) != 0 || model != (value))

// ReSTIR DI lights a pixel of this model. `isotropic` is the caller's own test
// (Lighting.ps: the decoded anisotropy is 0; RTDI.cs: a face pixel or RT4.g == 0)
// and only matters for an SMT_DI_ISOTROPIC model.
bool ShadingModelDiReceives(uint model, bool isotropic)
{
    return false SHADING_MODEL_TABLE(SMT_DI_TERM);
}

// ReSTIR GI receives on a pixel of this model.
bool ShadingModelGiReceives(uint model)
{
    return true SHADING_MODEL_TABLE(SMT_GI_TERM);
}
#endif // !__cplusplus

#endif // SHADING_MODEL_TABLE_HLSLI
