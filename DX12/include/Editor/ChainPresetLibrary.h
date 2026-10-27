#pragma once

// ChainPresetLibrary - the Chain Physics Preview panel's preset archetypes,
// user presets, per-field clamps and the slider reference bands.
//
// A preset is a named set of material values for one strand type (Chain =
// hair / tail / accessory strands, Cloth = ring-tied skirt pieces). It records
// exactly which fields it covers; loading it writes those fields and nothing
// else (the panel shades the covered fields in the reference-band table). Keys
// are type-neutral material names ("damping", "limitAngle", ...); the preset's
// type picks the component field they land in: the type-global field (Chain:
// damping, Cloth: skirtDamping, ...) and every group of that type that
// overrides the globals (ovrDamping, ...) - authored characters override per
// group, so writing only the globals would change nothing on screen. A Cloth
// preset writes whole pieces (every group sharing a ringGroup).
//
// Every load is clamped field by field: a PBD stiffness above its saturation
// point (structural 1.0, ring 1.25, shear 2.0 - from the solver's bucket
// constants 1.0 / 0.8 / 0.5) is inert at best, the int fields are held to the
// solver's limits, and a non-finite value is dropped, so a preset cannot poison
// an island even without the solver's own [0,1] clamp.
//
// Built-ins: seven archetypes (Builtins). User presets are JSON files under
// <WritableDataDir>/cache/chain_presets/<name>.json (inside the existing
// runtime cache/ folder: no new top-level folder). No asset type, no
// serializer field.
//
// Reference bands: a typical range per field in this solver's units
// (KawaiiPhysics guidance translated - KP Damping 0.1-0.3 is a loss, this
// solver's damping is a retention, so the band reads 0.70-0.90) and, for the
// three PBD stiffness buckets, the saturation point above which the slider is
// inert. DrawBand paints both onto the slider just drawn (no layout change);
// DrawGlobalBands draws a table for the reflected globals.
//
// Editor-only TU (EditorCore). ImGui is used only by the two Draw* helpers.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Physics/ChainPhysicsSystem.h"   // ChainPhysicsComponent / ChainGroupDef (member pointers)

namespace Editor::ChainPresets
{
    enum class Target : std::uint8_t { Chain, Cloth, Both };
    const char* TargetName(Target t);
    bool        ParseTarget(const std::string& s, Target& out);

    // One authored material field. `key` is the type-neutral preset key; the
    // table holds one row per (key, target) - "damping" has a Chain row
    // (damping / ovrDamping) and a Cloth row (skirtDamping / ovrDamping).
    struct FieldSpec
    {
        const char* key;
        const char* label;
        Target      target;
        float       lo, hi;          // the per-field clamp on load (hi = saturation for PBD)
        float       typLo, typHi;    // the reference band (typical range)
        float       sat;             // PBD saturation point (0 = none)
        // The type-global member (exactly one of gF / gI is set) and the group
        // override member (nullptr = the field has no per-group override).
        float ChainPhysicsComponent::* gF;
        int   ChainPhysicsComponent::* gI;
        float ChainGroupDef::*         ovr;
        const char* globalName;      // REFLECT / C++ name of the global
        const char* ovrName;         // C++ name of the override ("" = none)
        const char* note;            // tooltip (the KawaiiPhysics translation)
    };
    const std::vector<FieldSpec>& Fields();
    // The row for (key, target); a Both row answers either target.
    const FieldSpec* FindField(const std::string& key, Target target);
    // The row a slider for GROUP override `ovrMember` / a global REFLECT member
    // uses (the tree calls DrawBand with these names).
    const FieldSpec* FindByMember(const std::string& memberName, Target target);

    struct Preset
    {
        std::string name;
        Target      target = Target::Chain;
        bool        builtin = false;
        std::string path;                                   // user presets: the file
        std::vector<std::pair<std::string, float>> fields;  // the fields it COVERS
    };
    const std::vector<Preset>& Builtins();

    // <WritableDataDir>/cache/chain_presets (created on the first save).
    std::string UserPresetDir();
    std::vector<Preset> ListUserPresets();
    // A built-in by name (case-insensitive, '.' / '-' / '_' / ' ' / U+00B7
    // ignored, so "hair_long" finds "Hair<U+00B7>Long"), else a user preset by
    // name, else - when `nameOrPath` ends in .json - that file.
    bool Resolve(const std::string& nameOrPath, Preset& out, std::string* err);
    bool LoadPresetFile(const std::string& path, Preset& out, std::string* err);
    bool SavePresetFile(const Preset& p, const std::string& path, std::string* err);
    std::string SanitizeName(const std::string& name);

    struct Report
    {
        std::uint32_t fields = 0;    // covered fields written
        std::uint32_t clamped = 0;   // ... of which were out of range (held to lo/hi)
        std::uint32_t unknown = 0;   // keys no row answers (ignored)
        std::uint32_t nonFinite = 0; // NaN / inf values (dropped)
        std::uint32_t groups = 0;    // groups whose override fields were written
        std::string   clampedKeys;   // "structuralStiffness 5->1, ..."
    };
    // Clamp every covered field in place.
    Report Clamp(Preset& p);
    // Write the preset into `c`: the type globals + the override fields of every
    // group of the preset's type that overrides (onlyGroup >= 0: that group only -
    // a Cloth group means its whole piece; a non-overriding group is switched to
    // override, seeded from the effective globals first, as the tree's "Override
    // global params" checkbox does). `clamp` false skips the clamp
    // (-chainpreview_preset_clamp_off).
    Report Apply(Preset p, ChainPhysicsComponent& c, int onlyGroup, bool clamp);
    // A preset holding every field of `target` from `c` (the first overriding
    // group of that type when there is one - its values are what runs - else
    // the globals). `sourceGroup` >= 0 picks the group.
    Preset Capture(const std::string& name, Target target,
                   const ChainPhysicsComponent& c, int sourceGroup, std::string* source);

    // ---- reference bands (ImGui; call right after the slider) ----
    // Paints the typical band (green strip along the bottom edge) and the
    // saturated part (grey veil) onto the last item, a slider/drag of range
    // [lo, hi]; `covered` marks the field as covered by the active preset.
    void DrawBand(const char* member, Target target, float value, float lo, float hi,
                  bool covered);
    // A compact table of the reflected globals with their bands and the active
    // preset's coverage (keys in `covered`).
    void DrawGlobalBands(const ChainPhysicsComponent& c,
                         const std::vector<std::string>* covered);
}
