#pragma once

// MaterialPropertyPanel - every control of one MaterialComponent: the entity
// header (source, Save / Save as New / Save as Override / Save as Instance /
// Load .imat), the shading model, the custom-shader block (path, compile
// errors, the reflected Custom Textures and Custom Parameters, Prune), Blend
// Mode, Domain, Shadow Cull, the flags, the schema's Material Parameters and
// the engine Texture Slots - with the instance override ticks. The Inspector
// and the Material Editor window draw this same panel.
//
// Editor-only TU: compiled into EditorCore.

#include "ECS/Components.h"          // MaterialComponent
#include "ECS/ECS.h"                 // Entity, NullEntity
#include "Resource/MaterialSerializer.h"   // Resource::MaterialLoadInfo

#include <cstdint>
#include <functional>
#include <string>

class Renderer;
class World;

namespace Editor
{
    // What the panel edits against. Entity mode: the selected entity (its
    // MaterialSourcePath, MaterialInstanceInfo, MaterialOverride). Asset mode:
    // no entity - the open file's instance info and its unsaved flag.
    struct MaterialPanelContext
    {
        Renderer*                   renderer   = nullptr;
        World*                      world      = nullptr;
        Entity                      entity     = NullEntity;
        bool                        assetMode  = false;
        Resource::MaterialLoadInfo* assetInfo  = nullptr;   // asset mode: parent, override / unset keys
        bool*                       assetDirty = nullptr;   // asset mode: set when an edit is made
        // An edit of a control with no override tick, by its .imat key. When set it
        // replaces the panel's own "an instance's edit is an override" rule: the
        // Material Editor's session applies that rule for the panel and its scripted
        // edits alike (MaterialEditSession::MarkEdited).
        std::function<void(const char* key)> onEdit;
    };

    class MaterialPropertyPanel
    {
    public:
        void Draw(MaterialComponent& mat, const MaterialPanelContext& ctx);

    private:
        // The instance's resolved parent, cached by path: loading it per frame
        // would mean an AssetFS read plus a full chain resolve every frame.
        std::string       m_parentSnapPath;
        MaterialComponent m_parentSnap;
        // Also keyed by Resource::MaterialContentGeneration: a save of the parent (in
        // another Material Editor tab, say) re-reads it, or unticking an override
        // would restore the parent's old value.
        std::uint64_t     m_parentSnapGen = 0;
    };
}
