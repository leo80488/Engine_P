#include "Sequencer/SequenceTargets.h"

#include "ECS/Components.h"            // LightData, MaterialComponent
#include "ECS/CameraStackComponents.h" // VirtualCameraComponent
#include "ECS/HeightFogComponent.h"
#include "Sequencer/SequenceAsset.h"   // HashTarget
#include "System/Log.h"

#include <algorithm>

namespace Seq
{

namespace
{
    std::vector<SeqTarget> s_targets;
    bool                   s_builtinsRegistered = false;

    // ---- generic accessors ----
    // A macro rather than a template so the table reads as a table: one line per animatable property,
    // with the component type in the row.
#define SEQ_TARGET_F(NAME, COMP, FIELD, TIP)                                    \
    SeqTarget{ NAME, 0, 1, TIP,                                                 \
        [](World& w, Entity e, float* v) -> bool {                              \
            if (auto* c = w.GetComponent<COMP>(e)) { v[0] = c->FIELD; return true; }  \
            return false; },                                                    \
        [](World& w, Entity e, const float* v) -> bool {                        \
            if (auto* c = w.GetComponent<COMP>(e)) { c->FIELD = v[0]; return true; }  \
            return false; } }

#define SEQ_TARGET_F3(NAME, COMP, FIELD, TIP)                                   \
    SeqTarget{ NAME, 0, 3, TIP,                                                 \
        [](World& w, Entity e, float* v) -> bool {                              \
            if (auto* c = w.GetComponent<COMP>(e)) {                            \
                v[0] = c->FIELD.x; v[1] = c->FIELD.y; v[2] = c->FIELD.z; return true; } \
            return false; },                                                    \
        [](World& w, Entity e, const float* v) -> bool {                        \
            if (auto* c = w.GetComponent<COMP>(e)) {                            \
                c->FIELD.x = v[0]; c->FIELD.y = v[1]; c->FIELD.z = v[2]; return true; } \
            return false; } }
}

void RegisterSeqTarget(const SeqTarget& t)
{
    SeqTarget copy = t;
    copy.id = HashTarget(copy.name);
    const auto it = std::find_if(s_targets.begin(), s_targets.end(),
                                 [&](const SeqTarget& e) { return e.id == copy.id; });
    if (it != s_targets.end())
    {
        // A collision here is a silent mis-drive: two names hashing the same
        // would let one Property track write the other's field.
        if (std::string(it->name) != copy.name)
            LOG_ERROR("SeqTarget: hash collision between '%s' and '%s' — rename one",
                      it->name, copy.name);
        *it = copy;
        return;
    }
    s_targets.push_back(copy);
}

const SeqTarget* FindSeqTarget(uint32_t id)
{
    if (id == 0) return nullptr;
    for (const SeqTarget& t : s_targets)
        if (t.id == id) return &t;
    return nullptr;
}

const std::vector<SeqTarget>& AllSeqTargets() { return s_targets; }

void RegisterBuiltinSeqTargets()
{
    if (s_builtinsRegistered) return;
    s_builtinsRegistered = true;

    // ---- Light ----
    RegisterSeqTarget(SEQ_TARGET_F ("light.intensity", LightData, intensity,
                                    "Radiometric intensity of the light"));
    RegisterSeqTarget(SEQ_TARGET_F ("light.radius",    LightData, radius,
                                    "Attenuation range (point / spot)"));
    RegisterSeqTarget(SEQ_TARGET_F ("light.spotAngle", LightData, spotAngle,
                                    "Spot half-angle, RADIANS"));
    RegisterSeqTarget(SEQ_TARGET_F3("light.color",     LightData, color,
                                    "Linear RGB"));

    // ---- Lens ----
    // FOV is stored in radians; the Fov CHANNEL is authored in degrees and converted by the camera
    // track. A Property track on this name is raw radians: one unit convention per address.
    RegisterSeqTarget(SEQ_TARGET_F("lens.fov",   VirtualCameraComponent, fov,
                                   "Vertical FOV, RADIANS"));
    RegisterSeqTarget(SEQ_TARGET_F("lens.nearZ", VirtualCameraComponent, nearZ, ""));
    RegisterSeqTarget(SEQ_TARGET_F("lens.farZ",  VirtualCameraComponent, farZ,  ""));

    // ---- Height fog ----
    RegisterSeqTarget(SEQ_TARGET_F ("fog.density",     HeightFogComponent, fogDensity, ""));
    RegisterSeqTarget(SEQ_TARGET_F ("fog.heightFalloff", HeightFogComponent, fogHeightFalloff, ""));
    RegisterSeqTarget(SEQ_TARGET_F ("fog.height",      HeightFogComponent, fogHeight, ""));
    RegisterSeqTarget(SEQ_TARGET_F ("fog.startDistance", HeightFogComponent, startDistance, ""));
    RegisterSeqTarget(SEQ_TARGET_F ("fog.maxOpacity",  HeightFogComponent, maxOpacity, ""));
    RegisterSeqTarget(SEQ_TARGET_F ("fog.anisotropy",  HeightFogComponent, anisotropy, ""));
    RegisterSeqTarget(SEQ_TARGET_F ("fog.sunInscatter", HeightFogComponent, sunInscatterIntensity, ""));
    RegisterSeqTarget(SEQ_TARGET_F3("fog.color",       HeightFogComponent, fogColor, "Linear RGB"));

    // ---- Material ----
    // Routed through the real setters, each of which calls SetDirty(); writing the fields directly
    // would leave the GPU material cache holding the old values and the change would not appear.
    RegisterSeqTarget(SeqTarget{ "material.emissiveStrength", 0, 1,
        "MaterialComponent::SetEmissiveStrength (emissiveColor.w)",
        [](World& w, Entity e, float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { v[0] = m->emissiveColor.w; return true; }
            return false; },
        [](World& w, Entity e, const float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { m->SetEmissiveStrength(v[0]); return true; }
            return false; } });

    RegisterSeqTarget(SeqTarget{ "material.opacity", 0, 1,
        "MaterialComponent::SetOpacity (baseColor.w)",
        [](World& w, Entity e, float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { v[0] = m->baseColor.w; return true; }
            return false; },
        [](World& w, Entity e, const float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { m->SetOpacity(v[0]); return true; }
            return false; } });

    RegisterSeqTarget(SeqTarget{ "material.saturation", 0, 1, "",
        [](World& w, Entity e, float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { v[0] = m->saturation; return true; }
            return false; },
        [](World& w, Entity e, const float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { m->SetSaturation(v[0]); return true; }
            return false; } });

    RegisterSeqTarget(SeqTarget{ "material.roughnessMin", 0, 1, "",
        [](World& w, Entity e, float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { v[0] = m->roughnessMin; return true; }
            return false; },
        [](World& w, Entity e, const float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { m->SetRoughnessMin(v[0]); return true; }
            return false; } });

    RegisterSeqTarget(SeqTarget{ "material.roughnessMax", 0, 1, "",
        [](World& w, Entity e, float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { v[0] = m->roughnessMax; return true; }
            return false; },
        [](World& w, Entity e, const float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { m->SetRoughnessMax(v[0]); return true; }
            return false; } });

    RegisterSeqTarget(SeqTarget{ "material.metalnessMin", 0, 1, "",
        [](World& w, Entity e, float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { v[0] = m->metalnessMin; return true; }
            return false; },
        [](World& w, Entity e, const float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { m->SetMetalnessMin(v[0]); return true; }
            return false; } });

    RegisterSeqTarget(SeqTarget{ "material.metalnessMax", 0, 1, "",
        [](World& w, Entity e, float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { v[0] = m->metalnessMax; return true; }
            return false; },
        [](World& w, Entity e, const float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) { m->SetMetalnessMax(v[0]); return true; }
            return false; } });

    RegisterSeqTarget(SeqTarget{ "material.baseColor", 0, 3, "Linear RGB (alpha stays put)",
        [](World& w, Entity e, float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) {
                v[0] = m->baseColor.x; v[1] = m->baseColor.y; v[2] = m->baseColor.z; return true; }
            return false; },
        [](World& w, Entity e, const float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) {
                DirectX::XMFLOAT4 c = m->baseColor;
                c.x = v[0]; c.y = v[1]; c.z = v[2];
                m->SetBaseColor(c); return true; }
            return false; } });

    RegisterSeqTarget(SeqTarget{ "material.emissiveColor", 0, 3, "Linear RGB (strength stays put)",
        [](World& w, Entity e, float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) {
                v[0] = m->emissiveColor.x; v[1] = m->emissiveColor.y; v[2] = m->emissiveColor.z; return true; }
            return false; },
        [](World& w, Entity e, const float* v) -> bool {
            if (auto* m = w.GetComponent<MaterialComponent>(e)) {
                DirectX::XMFLOAT4 c = m->emissiveColor;
                c.x = v[0]; c.y = v[1]; c.z = v[2];
                m->SetEmissiveColor(c); return true; }
            return false; } });

    LOG_INFO("SeqTarget: %zu built-in property targets registered", s_targets.size());
}

#undef SEQ_TARGET_F
#undef SEQ_TARGET_F3

} // namespace Seq
