#pragma once

// FaceMappingWindow - Window > Animation > Face Mapping: the face mapping of the
// selected character.
//   - Auto map: the built-in mapping for its kind as the base, the model's spellings of the
//     base's morphs, ARKit channels matched to morphs by name (FaceTracking::AutoMapMorphs), and
//     the coverage report - the channels nothing reads, the morphs nothing writes. Save writes
//     the .ifacemap; Use sets it as the FaceTracking component's profile (hot reload recompiles
//     the binding whenever the file is saved again).
//   - The compiled rules with this frame's values, the head chain and the binding's warnings.
//   - The virtual tracker: sliders for every ARKit channel, the head and the eyes, pushed to a
//     synthetic source ("virtual") each frame they are shown and on - a face without a camera.
//
// THE HARNESS. -ui_open_menu=Face Mapping@<frame> opens it; it shows the selected entity, or
// with none the first character that has a FaceTracking component.
//
// Editor-only TU: compiled into EditorCore, never into EngineCore.

#include "ECS/ECS.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

class SkeletonRegistry;

namespace Editor
{
    class FaceMappingWindow
    {
    public:
        struct Context
        {
            std::function<bool(const char*)> held;   // the harness's -ui_open_menu hold
            World*                           world = nullptr;
            SkeletonRegistry*                skeletons = nullptr;
            Entity                           selected = NullEntity;
        };

        static FaceMappingWindow& Get();

        bool IsOpen() const { return m_open; }
        void SetOpen(bool open) { m_open = open; }
        // Every frame, open or not (a harness hold opens it; the virtual tracker pushes while shown).
        void Draw(const Context& ctx);

    private:
        void DrawAutoMap(const Context& ctx, Entity carrier);
        void DrawBinding(const Context& ctx, Entity carrier);
        void DrawVirtualTracker(Entity carrier, const Context& ctx);

        bool m_open = false;
        bool m_heldBefore = false;
        // the last automatic mapping
        Entity m_mappedFor = NullEntity;
        bool m_haveMap = false;
        std::string m_base;
        std::string m_mapJson;
        size_t m_rules = 0;
        std::vector<std::pair<std::string, std::string>> m_matches, m_renamed;
        std::vector<std::string> m_unmapped, m_undriven;
        char m_savePath[260] = "";
        std::string m_note;
        // the virtual tracker
        bool m_virtualOn = false;
        float m_channels[52] = {};
        float m_headYaw = 0.0f, m_headPitch = 0.0f, m_headRoll = 0.0f;
        float m_eyeYaw = 0.0f, m_eyePitch = 0.0f;
    };
}
