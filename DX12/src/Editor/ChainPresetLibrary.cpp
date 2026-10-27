// ChainPresetLibrary.cpp - the field table, the built-in presets, user preset files
// and the reference-band drawing. See the header for the contract.

#include "Editor/ChainPresetLibrary.h"
#include "Editor/ImGuiFlow.h"   // wrapped read-out, ellipsized names

#include "System/Log.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace Editor::ChainPresets
{
namespace
{
    using CPC = ChainPhysicsComponent;
    using CGD = ChainGroupDef;
    constexpr float CPC::* kNoF = nullptr;
    constexpr int   CPC::* kNoI = nullptr;
    constexpr float CGD::* kNoO = nullptr;

    // ONE table: the per-field clamp (lo/hi), the reference band (typLo/typHi)
    // and the PBD saturation point. Bands are in THIS solver's units; the note
    // says how a KawaiiPhysics number translates. The saturation points are the
    // solver's bucket constants inverted (ChainPhysicsSystem: s = bucket constant x
    // slider stiffness, clamped to 1 BEFORE the substep powf; buckets 1.0 vertical /
    // 0.8 ring / 0.5 shear), so 1.0 / 1.25 / 2.0 are exact, not tuning.
    const std::vector<FieldSpec>& Table()
    {
        static const std::vector<FieldSpec> kFields = {
            // ---- Chain (independent strands: hair, tail, accessories) ----
            { "damping", "Damping (retention)", Target::Chain, 0.f, 1.f, 0.70f, 0.90f, 0.f,
              &CPC::damping, kNoI, &CGD::ovrDamping, "damping", "ovrDamping",
              "Velocity RETAINED per frame. KawaiiPhysics Damping 0.1-0.3 is the LOSS,\n"
              "i.e. 0.70-0.90 here. Lower settles faster." },
            { "gravity", "Gravity", Target::Chain, -100.f, 0.f, -40.f, -10.f, 0.f,
              &CPC::gravity, kNoI, &CGD::ovrGravity, "gravity", "ovrGravity",
              "Y gravity on the strand (world units / s^2 in skeleton space)." },
            { "maxVelocity", "Max velocity", Target::Chain, 0.f, 5.f, 0.2f, 1.0f, 0.f,
              &CPC::maxVelocity, kNoI, &CGD::ovrMaxVelocity, "maxVelocity", "ovrMaxVelocity",
              "Per-frame velocity clamp; 0 = unlimited." },
            { "localStiffness", "Hold shape", Target::Chain, 0.f, 1.f, 0.02f, 0.20f, 0.f,
              &CPC::localStiffness, kNoI, &CGD::ovrLocalStiffness, "localStiffness", "ovrLocalStiffness",
              "Pull toward the animated shape - KawaiiPhysics Stiffness 0.05-0.2." },
            { "tipHold", "Tip hold", Target::Chain, 0.f, 1.f, 0.f, 0.30f, 0.f,
              &CPC::tipHold, kNoI, &CGD::ovrTipHold, "tipHold", "ovrTipHold",
              "Floor of the root->tip hold-shape falloff; 0 = floppy tip." },
            { "limitAngle", "Limit angle", Target::Chain, 0.f, 180.f, 15.f, 60.f, 0.f,
              &CPC::limitAngle, kNoI, &CGD::ovrLimitAngle, "limitAngle", "ovrLimitAngle",
              "KawaiiPhysics LimitAngle (deg); 0 = unlimited. 15-30 bangs, 35-60 long hair." },
            { "limitTipScale", "Limit tip scale", Target::Chain, 0.f, 4.f, 1.5f, 3.0f, 0.f,
              &CPC::limitTipScale, kNoI, &CGD::ovrLimitTipScale, "limitTipScale", "ovrLimitTipScale",
              "Cone widening toward the tip." },
            { "inertiaScale", "Inertia", Target::Chain, 0.f, 1.f, 0.5f, 1.0f, 0.f,
              &CPC::inertiaScale, kNoI, &CGD::ovrInertiaScale, "inertiaScale", "ovrInertiaScale",
              "Fraction of the root's motion the strand feels." },
            // ---- Cloth (ring-tied pieces: skirts, capes authored as cloth) ----
            { "damping", "Damping (retention)", Target::Cloth, 0.f, 1.f, 0.10f, 0.40f, 0.f,
              &CPC::skirtDamping, kNoI, &CGD::ovrDamping, "skirtDamping", "ovrDamping",
              "Cloth settles faster than hair: 0.10-0.40 retention." },
            { "gravity", "Gravity", Target::Cloth, -100.f, 0.f, -15.f, -3.f, 0.f,
              &CPC::skirtGravity, kNoI, &CGD::ovrGravity, "skirtGravity", "ovrGravity",
              "Cloth gravity (softer than hair)." },
            { "maxVelocity", "Max velocity", Target::Cloth, 0.f, 5.f, 0.2f, 0.8f, 0.f,
              &CPC::skirtMaxVelocity, kNoI, &CGD::ovrMaxVelocity, "skirtMaxVelocity", "ovrMaxVelocity",
              "Per-frame velocity clamp; 0 = unlimited." },
            { "localStiffness", "Hold shape", Target::Cloth, 0.f, 1.f, 0.20f, 0.50f, 0.f,
              &CPC::skirtLocalStiffness, kNoI, &CGD::ovrLocalStiffness, "skirtLocalStiffness", "ovrLocalStiffness",
              "Pull toward the animated shape." },
            { "tipHold", "Tip hold", Target::Cloth, 0.f, 1.f, 0.f, 0.30f, 0.f,
              &CPC::skirtTipHold, kNoI, &CGD::ovrTipHold, "skirtTipHold", "ovrTipHold",
              "Floor of the hold-shape falloff." },
            { "limitAngle", "Limit angle", Target::Cloth, 0.f, 180.f, 30.f, 60.f, 0.f,
              &CPC::skirtLimitAngle, kNoI, &CGD::ovrLimitAngle, "skirtLimitAngle", "ovrLimitAngle",
              "35-60 for a skirt (deg); 0 = unlimited." },
            { "limitTipScale", "Limit tip scale", Target::Cloth, 0.f, 4.f, 1.5f, 3.0f, 0.f,
              &CPC::skirtLimitTipScale, kNoI, &CGD::ovrLimitTipScale, "skirtLimitTipScale", "ovrLimitTipScale",
              "Cone widening toward the hem." },
            { "inertiaScale", "Inertia", Target::Cloth, 0.f, 1.f, 0.5f, 1.0f, 0.f,
              &CPC::skirtInertiaScale, kNoI, &CGD::ovrInertiaScale, "skirtInertiaScale", "ovrInertiaScale",
              "Fraction of the root's motion the cloth feels." },
            { "ringStiffness", "Ring stiffness", Target::Cloth, 0.1f, 1.25f, 0.6f, 1.25f, 1.25f,
              &CPC::ringStiffness, kNoI, &CGD::ovrRingStiffness, "ringStiffness", "ovrRingStiffness",
              "Hoop links. PBD SATURATES at 1.25 (bucket constant 0.8): inert above." },
            { "shearStiffness", "Shear stiffness", Target::Cloth, 0.1f, 2.0f, 0.5f, 2.0f, 2.0f,
              &CPC::shearStiffness, kNoI, &CGD::ovrShearStiffness, "shearStiffness", "ovrShearStiffness",
              "Diagonal links. PBD SATURATES at 2.0 (bucket constant 0.5): inert above." },
            { "horizDecay", "Ring+shear decay", Target::Cloth, 0.f, 1.f, 0.3f, 1.0f, 0.f,
              &CPC::skirtHorizDecay, kNoI, &CGD::ovrHorizDecay, "skirtHorizDecay", "ovrHorizDecay",
              "0 = the hem has no hoop hold, 1 = uniform." },
            // ---- both strand types (one component field) ----
            { "structuralStiffness", "Structural stiffness", Target::Both, 0.1f, 1.0f, 0.6f, 1.0f, 1.0f,
              &CPC::structuralStiffness, kNoI, &CGD::ovrStructuralStiffness, "structuralStiffness",
              "ovrStructuralStiffness",
              "Along-chain links (Chain AND Cloth). PBD SATURATES at 1.0: inert above." },
            { "collisionPadding", "Collision thickness", Target::Both, 0.f, 0.3f, 0.01f, 0.05f, 0.f,
              &CPC::collisionPadding, kNoI, &CGD::ovrCollisionPadding, "collisionPadding", "ovrCollisionPadding",
              "Extra capsule radius so cloth rests a thickness off the body." },
            { "contactFriction", "Contact friction", Target::Both, 0.f, 1.f, 0.10f, 0.40f, 0.f,
              &CPC::contactFriction, kNoI, &CGD::ovrContactFriction, "contactFriction", "ovrContactFriction",
              "0 = slides freely, 1 = grips." },
            { "windStrength", "Sway strength", Target::Both, 0.f, 0.5f, 0.f, 0.05f, 0.f,
              &CPC::windStrength, kNoI, &CGD::ovrWindStrength, "windStrength", "ovrWindStrength",
              "Idle drift amplitude at the tip (world units)." },
            { "windFreq", "Sway freq (Hz)", Target::Both, 0.f, 5.f, 0.3f, 1.2f, 0.f,
              &CPC::windFreq, kNoI, &CGD::ovrWindFreq, "windFreq", "ovrWindFreq",
              "Idle drift frequency." },
            // World-motion inertia (the entity's motion reaching the chains).
            // One field for every strand type: the kick is a velocity change,
            // so a heavily damped cloth already absorbs it faster than hair.
            // ENTITY-level only (no group pointer): a group's own value is a
            // "< 0 = follow" field outside the override block, which a preset
            // must not turn into an explicit number.
            { "worldInertiaLocation", "World inertia: location", Target::Both, 0.f, 1.f, 0.3f, 1.0f, 0.f,
              &CPC::worldInertiaLocation, kNoI, kNoO,
              "worldInertiaLocation", "",
              "Share of the ENTITY's translation the chains feel (start / stop kicks).\n"
              "KawaiiPhysics WorldDampingLocation is the complement (1 - this). 0 = rides the entity." },
            { "worldInertiaRotation", "World inertia: rotation", Target::Both, 0.f, 1.f, 0.3f, 1.0f, 0.f,
              &CPC::worldInertiaRotation, kNoI, kNoO,
              "worldInertiaRotation", "",
              "Share of the ENTITY's rotation the chains feel (turn in place).\n"
              "KawaiiPhysics WorldDampingRotation is the complement (1 - this). 0 = rides the entity." },
            // The solver's cost knobs (no per-group override). MAX limits are the
            // REFLECT slider limits (iterations 1-20, substeps 1-8).
            { "iterations", "Iterations", Target::Both, 1.f, 20.f, 4.f, 8.f, 0.f,
              kNoF, &CPC::iterations, kNoO, "iterations", "",
              "Constraint iterations per step; cost is substeps x (1 + iterations)." },
            { "substeps", "Substeps", Target::Both, 1.f, 8.f, 1.f, 2.f, 0.f,
              kNoF, &CPC::substeps, kNoO, "substeps", "",
              "Minimum collision substeps (fast capsules raise it)." },
        };
        return kFields;
    }

    bool Covers(Target preset, Target row)
    {
        return row == Target::Both || row == preset || preset == Target::Both;
    }
    bool GroupMatches(Target preset, const CGD& g)
    {
        if (g.type == ChainGroupType::Spring) return false;
        if (preset == Target::Both) return true;
        return (preset == Target::Cloth) == (g.type == ChainGroupType::Cloth);
    }

    std::string Norm(const std::string& s)
    {
        std::string o;
        for (unsigned char ch : s)
            if (std::isalnum(ch) && ch < 0x80) o += static_cast<char>(std::tolower(ch));
        return o;
    }

    float Get(const CPC& c, const FieldSpec& f)
    {
        return f.gF ? c.*f.gF : static_cast<float>(c.*f.gI);
    }
    void Set(CPC& c, const FieldSpec& f, float v)
    {
        if (f.gF) c.*f.gF = v;
        else      c.*f.gI = static_cast<int>(std::lround(v));
    }

    // Turning on "Override global params" for a group: start from the EFFECTIVE
    // globals of its type (the same seeding the chain parameter tree does in
    // ChainPhysicsEditorWindow; enabling isolation must never make the strand jump),
    // then the preset writes its covered fields on top.
    void SeedOverride(const CPC& c, CGD& g)
    {
        const bool cloth = g.type == ChainGroupType::Cloth;
        g.ovrDamping             = cloth ? c.skirtDamping : c.damping;
        g.ovrGravity             = cloth ? c.skirtGravity : c.gravity;
        g.ovrStructuralStiffness = c.structuralStiffness;
        g.ovrRingStiffness       = c.ringStiffness;
        g.ovrShearStiffness      = c.shearStiffness;
        g.ovrMaxVelocity         = cloth ? c.skirtMaxVelocity : c.maxVelocity;
        g.ovrLocalStiffness      = cloth ? c.skirtLocalStiffness : c.localStiffness;
        g.ovrHorizDecay          = c.skirtHorizDecay;
        g.ovrTipHold             = cloth ? c.skirtTipHold : c.tipHold;
        g.ovrCollisionPadding    = c.collisionPadding;
        g.ovrContactFriction     = c.contactFriction;
        g.ovrLimitAngle          = cloth ? c.skirtLimitAngle : c.limitAngle;
        g.ovrLimitTipScale       = cloth ? c.skirtLimitTipScale : c.limitTipScale;
        g.ovrInertiaScale        = cloth ? c.skirtInertiaScale : c.inertiaScale;
        g.ovrWindStrength        = c.windStrength;
        g.ovrWindFreq            = c.windFreq;
        g.ovrEnabled             = true;
    }

    Preset Make(const char* name, Target t, std::vector<std::pair<std::string, float>> f)
    {
        Preset p;
        p.name = name;
        p.target = t;
        p.builtin = true;
        p.fields = std::move(f);
        return p;
    }
}

const char* TargetName(Target t)
{
    return t == Target::Cloth ? "cloth" : t == Target::Both ? "both" : "chain";
}

bool ParseTarget(const std::string& s, Target& out)
{
    const std::string n = Norm(s);
    if (n == "chain" || n == "hair") { out = Target::Chain; return true; }
    if (n == "cloth" || n == "skirt") { out = Target::Cloth; return true; }
    if (n == "both") { out = Target::Both; return true; }
    return false;
}

const std::vector<FieldSpec>& Fields() { return Table(); }

const FieldSpec* FindField(const std::string& key, Target target)
{
    for (const FieldSpec& f : Table())
        if (key == f.key && Covers(target, f.target) &&
            (f.target == target || f.target == Target::Both || target == Target::Both))
            return &f;
    return nullptr;
}

const FieldSpec* FindByMember(const std::string& member, Target target)
{
    for (const FieldSpec& f : Table())
    {
        const bool ovrHit = f.ovrName[0] && member == f.ovrName &&
                            (f.target == target || f.target == Target::Both);
        if (ovrHit || member == f.globalName) return &f;
    }
    return nullptr;
}

// Seven archetypes ("load a preset first, then tune" - Magica Cloth's advice).
// Starting points in this solver's units; the reference bands are the same
// numbers' neighbourhood.
const std::vector<Preset>& Builtins()
{
    static const std::vector<Preset> s_builtins = {
        Make("Hair\xC2\xB7Long", Target::Chain, {
            { "damping", 0.85f }, { "gravity", -30.f }, { "maxVelocity", 0.6f },
            { "localStiffness", 0.04f }, { "tipHold", 0.f }, { "limitAngle", 50.f },
            { "limitTipScale", 2.f }, { "inertiaScale", 0.9f }, { "structuralStiffness", 1.f } }),
        Make("Hair\xC2\xB7Short", Target::Chain, {
            { "damping", 0.70f }, { "gravity", -15.f }, { "maxVelocity", 0.3f },
            { "localStiffness", 0.20f }, { "tipHold", 0.3f }, { "limitAngle", 25.f },
            { "limitTipScale", 1.5f }, { "inertiaScale", 0.6f }, { "structuralStiffness", 1.f } }),
        Make("Skirt\xC2\xB7Pleated", Target::Cloth, {
            { "damping", 0.25f }, { "gravity", -8.f }, { "maxVelocity", 0.5f },
            { "localStiffness", 0.25f }, { "limitAngle", 45.f }, { "inertiaScale", 0.9f },
            { "ringStiffness", 0.9f }, { "shearStiffness", 1.2f }, { "horizDecay", 0.6f },
            { "structuralStiffness", 1.f } }),
        Make("Skirt\xC2\xB7Tight", Target::Cloth, {
            { "damping", 0.15f }, { "gravity", -4.f }, { "maxVelocity", 0.3f },
            { "localStiffness", 0.5f }, { "limitAngle", 20.f }, { "inertiaScale", 0.6f },
            { "ringStiffness", 1.25f }, { "shearStiffness", 2.f }, { "horizDecay", 1.f },
            { "structuralStiffness", 1.f } }),
        // Capes are authored as independent Chain strands more often than as a
        // ring-tied piece (Rossi's are); a ring-tied cape takes a Skirt preset.
        Make("Cape", Target::Chain, {
            { "damping", 0.90f }, { "gravity", -40.f }, { "maxVelocity", 0.8f },
            { "localStiffness", 0.02f }, { "tipHold", 0.f }, { "limitAngle", 70.f },
            { "limitTipScale", 2.5f }, { "inertiaScale", 1.f } }),
        Make("Tail", Target::Chain, {
            { "damping", 0.75f }, { "gravity", -10.f }, { "maxVelocity", 0.4f },
            { "localStiffness", 0.15f }, { "tipHold", 0.2f }, { "limitAngle", 35.f },
            { "limitTipScale", 2.f }, { "inertiaScale", 0.8f } }),
        Make("Accessory", Target::Chain, {
            { "damping", 0.60f }, { "gravity", -20.f }, { "maxVelocity", 0.2f },
            { "localStiffness", 0.30f }, { "tipHold", 0.4f }, { "limitAngle", 15.f },
            { "limitTipScale", 1.5f }, { "inertiaScale", 0.5f } }),
    };
    return s_builtins;
}

std::string UserPresetDir()
{
    return Sys::WritableDataDir() + "/cache/chain_presets";
}

std::string SanitizeName(const std::string& name)
{
    std::string o;
    for (unsigned char ch : name)
        o += (std::isalnum(ch) && ch < 0x80) || ch == '-' || ch == '_' ? static_cast<char>(ch) : '_';
    while (!o.empty() && o.back() == '_') o.pop_back();
    return o.empty() ? std::string("preset") : o;
}

std::vector<Preset> ListUserPresets()
{
    std::vector<Preset> out;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = Sys::U8Path(UserPresetDir());
    if (!fs::is_directory(dir, ec)) return out;
    for (const fs::directory_entry& de : fs::directory_iterator(dir, ec))
    {
        if (!de.is_regular_file(ec) || de.path().extension() != ".json") continue;
        Preset p;
        if (LoadPresetFile(Sys::PathToUtf8(de.path()), p, nullptr)) out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(),
              [](const Preset& a, const Preset& b) { return a.name < b.name; });
    return out;
}

bool LoadPresetFile(const std::string& path, Preset& out, std::string* err)
{
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    std::ifstream f = Sys::OpenIfstream(path, std::ios::binary);
    if (!f) return fail("cannot open '" + path + "'");
    std::stringstream ss;
    ss << f.rdbuf();
    nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return fail("'" + path + "' is not a JSON object");
    Preset p;
    p.path = path;
    p.builtin = false;
    if (j.contains("name") && j["name"].is_string()) p.name = j["name"].get<std::string>();
    if (p.name.empty())
    {
        const std::filesystem::path fp = Sys::U8Path(path);
        p.name = Sys::PathToUtf8(fp.stem());
    }
    if (!j.contains("type") || !j["type"].is_string() ||
        !ParseTarget(j["type"].get<std::string>(), p.target))
        return fail("'" + path + "': \"type\" must be \"chain\", \"cloth\" or \"both\"");
    if (!j.contains("fields") || !j["fields"].is_object())
        return fail("'" + path + "': no \"fields\" object");
    for (auto it = j["fields"].begin(); it != j["fields"].end(); ++it)
    {
        // A non-number is kept as NaN so Clamp / Apply COUNT it (nonFinite)
        // instead of the file silently covering one field fewer.
        p.fields.emplace_back(it.key(), it.value().is_number()
                                            ? it.value().get<float>()
                                            : std::numeric_limits<float>::quiet_NaN());
    }
    out = std::move(p);
    return true;
}

bool SavePresetFile(const Preset& p, const std::string& path, std::string* err)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(Sys::U8Path(path).parent_path(), ec);
    nlohmann::ordered_json j;
    j["format"] = 1;
    j["name"]   = p.name;
    j["type"]   = TargetName(p.target);
    nlohmann::ordered_json fields = nlohmann::ordered_json::object();
    for (const auto& [k, v] : p.fields)
        if (std::isfinite(v)) fields[k] = v;
    j["fields"] = fields;
    std::ofstream o = Sys::OpenOfstream(path, std::ios::binary | std::ios::trunc);
    if (!o) { if (err) *err = "cannot write '" + path + "'"; return false; }
    o << j.dump(2) << "\n";
    return o.good();
}

bool Resolve(const std::string& nameOrPath, Preset& out, std::string* err)
{
    const std::string want = Norm(nameOrPath);
    for (const Preset& p : Builtins())
        if (Norm(p.name) == want) { out = p; return true; }
    for (Preset& p : ListUserPresets())
        if (Norm(p.name) == want) { out = std::move(p); return true; }
    if (nameOrPath.size() > 5)
    {
        std::string ext = nameOrPath.substr(nameOrPath.size() - 5);
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ext == ".json") return LoadPresetFile(nameOrPath, out, err);
    }
    if (err) *err = "no built-in or user preset named '" + nameOrPath + "'";
    return false;
}

Report Clamp(Preset& p)
{
    Report r;
    std::vector<std::pair<std::string, float>> kept;
    for (auto& [key, v] : p.fields)
    {
        const FieldSpec* f = FindField(key, p.target);
        if (!f) { ++r.unknown; continue; }
        if (!std::isfinite(v)) { ++r.nonFinite; continue; }
        const float c = std::clamp(v, f->lo, f->hi);
        if (c != v)
        {
            ++r.clamped;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s%s %g->%g", r.clampedKeys.empty() ? "" : ",",
                          key.c_str(), static_cast<double>(v), static_cast<double>(c));
            r.clampedKeys += buf;
        }
        kept.emplace_back(key, f->gI ? std::round(c) : c);
        ++r.fields;
    }
    p.fields = std::move(kept);
    return r;
}

Report Apply(Preset p, ChainPhysicsComponent& c, int onlyGroup, bool clamp)
{
    Report r;
    if (clamp) r = Clamp(p);
    else
    {
        // No clamp: every known, finite field lands exactly as the file wrote it.
        std::vector<std::pair<std::string, float>> kept;
        for (auto& kv : p.fields)
        {
            if (!FindField(kv.first, p.target)) { ++r.unknown; continue; }
            if (!std::isfinite(kv.second)) { ++r.nonFinite; continue; }
            kept.push_back(kv);
            ++r.fields;
        }
        p.fields = std::move(kept);
    }
    // ---- the globals of the preset's type ----
    for (const auto& [key, v] : p.fields)
        if (const FieldSpec* f = FindField(key, p.target)) Set(c, *f, v);

    // ---- the groups that override the globals ----
    // A Cloth group is written as its whole PIECE (every group sharing its
    // ringGroup) - the unit the tree's piece header keeps in sync.
    std::vector<bool> pick(c.groups.size(), false);
    if (onlyGroup >= 0 && onlyGroup < static_cast<int>(c.groups.size()))
    {
        const CGD& og = c.groups[static_cast<std::size_t>(onlyGroup)];
        if (GroupMatches(p.target, og))
            for (std::size_t i = 0; i < c.groups.size(); ++i)
                pick[i] = static_cast<int>(i) == onlyGroup ||
                          (og.type == ChainGroupType::Cloth &&
                           c.groups[i].type == ChainGroupType::Cloth &&
                           c.groups[i].ringGroup == og.ringGroup);
        for (std::size_t i = 0; i < c.groups.size(); ++i)
            if (pick[i] && !c.groups[i].ovrEnabled) SeedOverride(c, c.groups[i]);
    }
    else
    {
        for (std::size_t i = 0; i < c.groups.size(); ++i)
            pick[i] = GroupMatches(p.target, c.groups[i]) && c.groups[i].ovrEnabled;
    }
    for (std::size_t i = 0; i < c.groups.size(); ++i)
    {
        if (!pick[i]) continue;
        CGD& g = c.groups[i];
        const Target gt = g.type == ChainGroupType::Cloth ? Target::Cloth : Target::Chain;
        for (const auto& [key, v] : p.fields)
        {
            const FieldSpec* f = FindField(key, gt);
            if (f && f->ovr && Covers(p.target, f->target)) g.*f->ovr = v;
        }
        ++r.groups;
    }
    return r;
}

Preset Capture(const std::string& name, Target target, const ChainPhysicsComponent& c,
               int sourceGroup, std::string* source)
{
    Preset p;
    p.name = name;
    p.target = target;
    const CGD* src = nullptr;
    if (sourceGroup >= 0 && sourceGroup < static_cast<int>(c.groups.size()) &&
        GroupMatches(target, c.groups[static_cast<std::size_t>(sourceGroup)]) &&
        c.groups[static_cast<std::size_t>(sourceGroup)].ovrEnabled)
        src = &c.groups[static_cast<std::size_t>(sourceGroup)];
    for (std::size_t i = 0; !src && i < c.groups.size(); ++i)
        if (GroupMatches(target, c.groups[i]) && c.groups[i].ovrEnabled) src = &c.groups[i];
    if (source)
        *source = src ? std::string("group '") + src->name + "'" : std::string("globals");
    for (const FieldSpec& f : Table())
    {
        if (f.target != target && f.target != Target::Both) continue;
        const float v = (src && f.ovr) ? src->*f.ovr : Get(c, f);
        p.fields.emplace_back(f.key, v);
    }
    return p;
}

// ==== Reference bands (ImGui) ====
void DrawBand(const char* member, Target target, float value, float lo, float hi, bool covered)
{
    const FieldSpec* f = member ? FindByMember(member, target) : nullptr;
    if (!f || !(hi > lo)) return;
    // The LAST item is the slider: its frame is the item width wide (the label
    // sits to its right). Painted ONTO it - no layout change, so the tree's
    // scroll position and ids stay what they are without the bands.
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx(mn.x + ImGui::CalcItemWidth(), ImGui::GetItemRectMax().y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto X = [&](float v) { return mn.x + (std::clamp(v, lo, hi) - lo) / (hi - lo) * (mx.x - mn.x); };
    // The PBD saturation veil: everything right of the saturation point is inert.
    if (f->sat > 0.f && f->sat < hi)
        dl->AddRectFilled(ImVec2(X(f->sat), mn.y), mx, IM_COL32(150, 150, 150, 70));
    // The typical band: a strip along the bottom edge.
    const float x0 = X(std::min(f->typLo, f->typHi)), x1 = X(std::max(f->typLo, f->typHi));
    dl->AddRectFilled(ImVec2(x0, mx.y - 3.f), ImVec2(std::max(x1, x0 + 2.f), mx.y),
                      IM_COL32(80, 200, 110, 200));
    // Where the value sits relative to both.
    const bool inBand = value >= std::min(f->typLo, f->typHi) && value <= std::max(f->typLo, f->typHi);
    const bool inert  = f->sat > 0.f && value > f->sat;
    const float vx = X(value);
    dl->AddLine(ImVec2(vx, mx.y - 5.f), ImVec2(vx, mx.y),
                inert ? IM_COL32(170, 170, 170, 255)
                      : inBand ? IM_COL32(120, 255, 150, 255) : IM_COL32(255, 190, 80, 255), 2.f);
    if (covered)   // blue chip: the active preset covers this field
        dl->AddRectFilled(ImVec2(mn.x - 4.f, mn.y), ImVec2(mn.x - 1.f, mx.y),
                          IM_COL32(90, 160, 255, 230));
}

void DrawGlobalBands(const ChainPhysicsComponent& c, const std::vector<std::string>* covered)
{
    // Wrapped: the staged tree lives in a resizable column.
    EditorUI::Flow::DisabledWrapped("Reference bands (green = typical, grey = PBD-saturated, "
                                    "blue chip = covered by the active preset)");
    // The field column is as wide as the longest name when the column allows it,
    // else what is left after the value and a minimum band; the name is then
    // ellipsized and the full name stays in the tooltip.
    auto nameOf = [](const FieldSpec& f, bool cov)
    {
        return std::string(cov ? "* " : "") +
               (f.target == Target::Cloth ? "Cloth " : f.target == Target::Chain ? "Chain " : "All ") +
               f.label;
    };
    const ImGuiStyle& st = ImGui::GetStyle();
    float nameW = 0.f;
    for (const FieldSpec& f : Table())
        nameW = std::max(nameW, ImGui::CalcTextSize(nameOf(f, true).c_str()).x);
    const float valueW = ImGui::CalcTextSize("00.000").x;
    const float minBand = 60.f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float fieldW = std::clamp(avail - valueW - minBand - 6.f * st.CellPadding.x, 80.f,
                                    std::max(nameW, 80.f));
    if (!ImGui::BeginTable("##cpv_bands", 3, ImGuiTableFlags_SizingFixedFit |
                                             ImGuiTableFlags_RowBg))
        return;
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthFixed, fieldW);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, valueW);
    ImGui::TableSetupColumn("band",  ImGuiTableColumnFlags_WidthStretch, 1.f);
    for (const FieldSpec& f : Table())
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool cov = covered &&
            std::find(covered->begin(), covered->end(), std::string(f.key)) != covered->end();
        const std::string name = nameOf(f, cov);
        ImGui::TextUnformatted(EditorUI::Flow::Ellipsize(name, ImGui::GetContentRegionAvail().x).c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s\n%s   typical %g .. %g%s", name.c_str(), f.note, f.globalName,
                              static_cast<double>(f.typLo), static_cast<double>(f.typHi),
                              f.sat > 0.f ? "   (inert above the saturation point)" : "");
        ImGui::TableNextColumn();
        const float v = Get(c, f);
        ImGui::Text(f.gI ? "%.0f" : "%.3f", static_cast<double>(v));
        ImGui::TableNextColumn();
        // A bar the column wide, the same painting DrawBand does on a slider.
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(std::max(w, 8.f), ImGui::GetTextLineHeight()));
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(mn, mx, IM_COL32(40, 40, 40, 255));
        auto X = [&](float x) { return mn.x + (std::clamp(x, f.lo, f.hi) - f.lo) / (f.hi - f.lo) * (mx.x - mn.x); };
        dl->AddRectFilled(ImVec2(X(std::min(f.typLo, f.typHi)), mn.y + 2.f),
                          ImVec2(std::max(X(std::max(f.typLo, f.typHi)), X(std::min(f.typLo, f.typHi)) + 2.f),
                                 mx.y - 2.f), IM_COL32(80, 200, 110, 170));
        if (f.sat > 0.f)
        {
            // Grey tick at the saturation point. The authored value may sit ABOVE the
            // clamp range (Rossi authors stiffness 1.5): its marker is pinned at the
            // right edge, grey.
            dl->AddRectFilled(ImVec2(X(f.sat) - 1.f, mn.y), ImVec2(X(f.sat) + 1.f, mx.y),
                              IM_COL32(160, 160, 160, 255));
        }
        const bool inert = f.sat > 0.f && v > f.sat;
        const float vx = X(v);
        dl->AddRectFilled(ImVec2(vx - 1.5f, mn.y), ImVec2(vx + 1.5f, mx.y),
                          inert ? IM_COL32(170, 170, 170, 255) : IM_COL32(255, 220, 90, 255));
        if (cov)
            dl->AddRectFilled(ImVec2(mn.x, mn.y), ImVec2(mn.x + 3.f, mx.y), IM_COL32(90, 160, 255, 230));
    }
    ImGui::EndTable();
}

} // namespace Editor::ChainPresets
