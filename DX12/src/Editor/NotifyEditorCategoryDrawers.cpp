// NotifyEditorCategoryDrawers - the per-NotifyCategory property panels registered into
// NotifyEditorRegistry. NotifyTrackEditor stays unaware of per-category fields; this TU
// tells the registry what to draw for each category. When a subsystem (Hitbox / Audio /
// Camera) grows editor-only details, move its drawer into that subsystem's own .cpp.

#include "Editor/NotifyEditorRegistry.h"
#include "Editor/NotifyEditorCategoryDrawers.h"
#include "ECS/StateToggle.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstring>

namespace
{
    // Inline char-buf editor backed by a std::string-typed PropertyBag entry.
    void StringField(PropertyBag& bag, const char* key, const char* label,
                     const char* defaultVal = "", const char* dropPayload = nullptr)
    {
        auto cur = bag.GetString(key, defaultVal);
        char buf[256];
        const size_t n = std::min(cur.size(), sizeof(buf) - 1);
        std::memcpy(buf, cur.data(), n);
        buf[n] = '\0';
        if (ImGui::InputText(label, buf, sizeof(buf)))
            bag.SetString(key, buf);
        if (dropPayload && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(dropPayload))
                bag.SetString(key, std::string(static_cast<const char*>(p->Data),
                                               p->DataSize > 0 ? p->DataSize - 1 : 0));
            ImGui::EndDragDropTarget();
        }
    }
    void IntField(PropertyBag& bag, const char* key, const char* label,
                  int defaultVal = 0, int lo = INT_MIN, int hi = INT_MAX)
    {
        int v = bag.GetInt(key, defaultVal);
        if (ImGui::DragInt(label, &v, 1.f, lo, hi)) bag.SetInt(key, v);
    }
    void FloatField(PropertyBag& bag, const char* key, const char* label,
                    float defaultVal = 0.f, float lo = -1e6f, float hi = 1e6f,
                    const char* fmt = "%.3f", float step = 0.01f)
    {
        float v = bag.GetFloat(key, defaultVal);
        if (ImGui::DragFloat(label, &v, step, lo, hi, fmt)) bag.SetFloat(key, v);
    }
    void Vec3Field(PropertyBag& bag, const char* key, const char* label,
                   DirectX::XMFLOAT3 defaultVal = {0,0,0})
    {
        DirectX::XMFLOAT3 v = bag.GetVec3(key, defaultVal);
        if (ImGui::DragFloat3(label, &v.x, 0.01f)) bag.SetVec3(key, v);
    }
    void EnumField(PropertyBag& bag, const char* key, const char* label,
                   const char* const* names, int count, int defaultVal = 0)
    {
        int v = bag.GetInt(key, defaultVal);
        v = std::clamp(v, 0, count - 1);
        if (ImGui::Combo(label, &v, names, count)) bag.SetInt(key, v);
    }
}

void RegisterDefaultNotifyEditors()
{
    // ---- Hitbox ----
    // `cmdType` only matters for point Notifies: for NotifyState intervals TimelineSystem
    // injects Begin / End itself. The combo stays so a point notify can author a one-shot End.
    NotifyEditorRegistry::Register(NotifyCategory::Hitbox, [](Notify& n) {
        static const char* kCmdNames[] = { "Begin", "End", "ChangeShape", "ChangeDamage" };
        EnumField(n.params, "cmdType",  "Cmd",          kCmdNames, IM_ARRAYSIZE(kCmdNames), 0);
        IntField (n.params, "slot",     "Slot",         0, 0, 3);
        IntField (n.params, "hitGroup", "Hit Group",    0, 0, 1024);

        // WHERE the volume lives. Empty = the entity's own transform (a body-check); a socket
        // name for a weapon or a fist - a socket on the hand bone or on the blade, no dummy
        // entity either way.
        StringField(n.params, "socket", "Socket", "");
        Vec3Field (n.params, "offset",  "Offset (socket space)");

        static const char* kShapeNames[] = { "Sphere", "Capsule" };
        EnumField(n.params, "shape",    "Shape",        kShapeNames, IM_ARRAYSIZE(kShapeNames), 0);
        FloatField(n.params, "radius",  "Radius (m)",   0.5f, 0.05f, 10.f, "%.2f", 0.01f);
        // Capsule only. Half the central SEGMENT, caps excluded, along the socket's local Y -
        // a blade is a long thin capsule, a fist a sphere.
        FloatField(n.params, "halfHeight", "Half Height (m)", 0.f, 0.f, 10.f, "%.2f", 0.01f);

        FloatField(n.params, "damage",  "Damage",      10.f,  0.f,   9999.f, "%.1f", 0.5f);

        // WHOSE side, as opposed to what KIND - see HitboxParams. Empty = hit anything the
        // layer mask found. The first two are TagComponent strings; the lists are gameplay tags
        // (comma separated dotted names, a tag matching the tags below it).
        StringField(n.params, "requireTag", "Require Tag", "");
        StringField(n.params, "ignoreTag",  "Ignore Tag",  "");
        StringField(n.params, "requireTags", "Require Gameplay Tags", "");
        StringField(n.params, "ignoreTags",  "Ignore Gameplay Tags",  "");
    });

    // ---- VFX ----
    NotifyEditorRegistry::Register(NotifyCategory::VFX, [](Notify& n) {
        StringField(n.params, "asset",   "Asset Path", "", "IVFX_PATH");
        static const char* kAttach[] = { "Detached", "AttachToBone", "FollowEntity" };
        EnumField (n.params, "attach",  "Attach Mode", kAttach, IM_ARRAYSIZE(kAttach), 0);
        IntField  (n.params, "bone",    "Bone Index", -1, -1, 256);
        Vec3Field (n.params, "offset",  "Local Offset");
        // 0 = inherit the .ivfx's own DUR/DURATION (recommended). A value > 0
        // OVERRIDES the asset's lifetime for every emitter in the prefab.
        FloatField(n.params, "duration","Duration (s, 0=from .ivfx)", 0.f, 0.f, 60.f, "%.2f", 0.05f);
    });

    // ---- Camera ----
    NotifyEditorRegistry::Register(NotifyCategory::Camera, [](Notify& n) {
        static const char* kKind[] = { "Shake", "Zoom", "FOVPunch", "HitStop" };
        EnumField (n.params, "kind",     "Effect",       kKind, IM_ARRAYSIZE(kKind), 0);

        const int kind = n.params.GetInt("kind", 0);
        if (kind == 0) {  // Shake
            // "amp" is camera TRAUMA (0..1); CameraShakeTickSystem uses trauma^2 for the noise
            // strength, so values below ~0.4 are barely visible. 0.6 gives a clearly felt shake.
            FloatField(n.params, "amp",   "Trauma (0..1)", 0.6f, 0.f, 1.f, "%.2f", 0.01f);
            FloatField(n.params, "freq",  "Frequency", 20.f, 0.f, 100.f);
            FloatField(n.params, "duration", "Duration (s)", 0.2f, 0.f, 5.f);
        } else if (kind == 1 || kind == 2) {  // Zoom / FOVPunch
            FloatField(n.params, "fovDelta", "FOV Delta (deg)", -5.f, -45.f, 45.f);
            FloatField(n.params, "duration", "Duration (s)", 0.2f, 0.f, 5.f);
        } else if (kind == 3) {  // HitStop
            FloatField(n.params, "duration", "Duration (s)", 0.05f, 0.f, 1.f);
            FloatField(n.params, "scale",    "Time Scale",   0.05f, 0.f, 1.f);
        }
    });

    // ---- Audio ----
    NotifyEditorRegistry::Register(NotifyCategory::Audio, [](Notify& n) {
        StringField(n.params, "clip",   "Clip Path");
        static const char* kBus[] = { "SFX", "Voice", "Foley", "Music" };
        EnumField (n.params, "cat",    "Category", kBus, IM_ARRAYSIZE(kBus), 0);
        FloatField(n.params, "volume", "Volume",   1.f, 0.f, 4.f, "%.2f", 0.02f);
        FloatField(n.params, "pitch",  "Pitch",    1.f, 0.25f, 4.f, "%.2f", 0.01f);
        IntField  (n.params, "bone",   "Attach Bone (-1 = 2D)", -1, -1, 256);
    });

    // ---- StateToggle ----
    // The tag is a dotted gameplay name (Gameplay/GameplayTags.h); a notify that names none is an
    // authoring error, shown here in red and reported when the clip is loaded.
    NotifyEditorRegistry::Register(NotifyCategory::StateToggle, [](Notify& n) {
        static const char* kKnown[] = {
            "State.IFrame", "State.SuperArmor", "State.InputBufferOpen", "State.RootMotionEnabled",
            "State.Invincible", "State.Vulnerable", "State.GuardBreakable", "State.HyperArmor",
        };
        StringField(n.params, "tagName", "Tag (dotted name)");
        if (ImGui::BeginCombo("Known tags", "pick one..."))
        {
            for (const char* known : kKnown)
                if (ImGui::Selectable(known)) n.params.SetString("tagName", known);
            ImGui::EndCombo();
        }
        Gameplay::Tag tag;
        std::string error;
        if (!StateToggle::ReadTag(n.params, tag, error))
            ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "Authoring error: this notify %s", error.c_str());
        IntField  (n.params, "enable",   "Enable (0/1)", 1, 0, 1);
        FloatField(n.params, "duration", "Auto-clear (s, 0=persist)", 0.f, 0.f, 30.f, "%.2f", 0.05f);
        ImGui::TextDisabled("As a NotifyState the tag is held while the clip is inside the interval;\nEnable and Auto-clear are for a point notify.");
    });

    NotifyEditorRegistry::Register(NotifyCategory::IKGate, [](Notify& n) {
        int mask=n.params.GetInt("goalMask",3);
        ImGui::InputInt("Goal mask (feet = 3)",&mask);
        n.params.SetInt("goalMask",mask&255);
        float weight=n.params.GetFloat("weight",0.f);
        ImGui::SliderFloat("IK weight",&weight,0.f,1.f);
        n.params.SetFloat("weight",weight);
        bool lock=n.params.GetInt("forceLock",0)!=0;
        ImGui::Checkbox("Force planted feet",&lock);
        n.params.SetInt("forceLock",lock?1:0);
        ImGui::TextDisabled("Use a NotifyState interval. End/cancel restores normal IK.");
    });
    // ---- FXGate ----
    // Arms resident weapon FX (trail / swing sparks) for the length of the interval.
    // Author it as a NotifyState: a POINT notify of this category does nothing (an instant
    // has no duration to hold the gate open).
    NotifyEditorRegistry::Register(NotifyCategory::FXGate, [](Notify& n) {
        StringField(n.params, "group", "Group (blank = any)");
        ImGui::TextDisabled("Must match AnimFXGateComponent::group on the\n"
                            "weapon prefab. Blank on either side = match all.");
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f),
                           "Use a NotifyState (interval), not a point notify.");
    });

    // ---- Custom (Tier 3) ----
    // No drawer: the generic PropertyBag fallback lets designers author arbitrary key/value
    // pairs that Lua picks up via the `CustomNotifyEvent` broadcast.
}
