#pragma once

// An engine module's editor half: its inspector sections, its Window-menu rows, its rows
// in the core menus and its panels. The .cpp says EDITOR_MODULE(Class); the
// descriptor's editorClass names it and the per-exe list (Modules.generated.cpp, under
// WITH_EDITOR) links it. Inspectors register for every linked module, so a project's data stays
// editable; menu rows and panels only exist while the project enables the module.

#include "ECS/ECS.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <vector>

class EditorLayer;
class IGraphicsDevice;
class Renderer;
namespace DX12Physics { class PhysicsSystem; }

namespace Editor
{
    // What the editor hands a module's hooks; read only during the call.
    struct ModuleHost
    {
        EditorLayer*                     layer    = nullptr;
        IGraphicsDevice*                 gfx      = nullptr;
        World*                           world    = nullptr;
        Renderer*                        renderer = nullptr;
        DX12Physics::PhysicsSystem*      physics  = nullptr;
        Entity                           selected = NullEntity;
        // A -ui_open_menu harness hold on the named window.
        std::function<bool(const char*)> menuHeld;
        // The Inspector's "Add Component" semantics: applied at the frame boundary, one undo step.
        std::function<void(Entity, std::type_index, std::function<void(World&, Entity)>)> addComponentWithUndo;
    };

    // One Window-menu row: the label is the window's visible title, windowId its Begin() name.
    struct ModulePanel
    {
        const char*               section;
        const char*               label;
        const char*               windowId;
        std::function<bool()>     isOpen;
        std::function<void(bool)> setOpen;
        const char*               tooltip = nullptr;
    };

    // One row of a core menu: @c menu names it ("Tools/Bake"), @c label is the row and
    // @c paletteLabel the command palette's Tools entry.
    struct ModuleMenuItem
    {
        const char*           menu;
        const char*           label;
        const char*           paletteLabel;
        std::function<void()> onClick;
    };

    class IEditorModule
    {
    public:
        virtual ~IEditorModule() = default;

        // The inspector sections, Add Component categories and fingerprints of its components
        // (EditorLayer::RegisterDefaultEditors), for every linked module.
        virtual void RegisterComponentEditors(const ModuleHost& host) { (void)host; }
        // Its Window-menu rows, placed after its section's last core row.
        virtual void AddPanels(std::vector<ModulePanel>& out) { (void)out; }
        // Its rows in the core menus (Tools > Bake) and the command palette.
        virtual void AddMenuItems(std::vector<ModuleMenuItem>& out) { (void)out; }
        // Every frame, open or not, beside the core panels.
        virtual void DrawPanels(const ModuleHost& host) { (void)host; }
    };

    using EditorModuleFactory = std::unique_ptr<IEditorModule> (*)();

    class ModuleRegistry
    {
    public:
        static ModuleRegistry& Get();

        void Link(std::string_view name, EditorModuleFactory factory);

        void RegisterComponentEditors(const ModuleHost& host);
        void AddPanels(std::vector<ModulePanel>& out);
        void AddMenuItems(std::vector<ModuleMenuItem>& out);
        void DrawPanels(const ModuleHost& host);
        // Releases every module's instance (the editor's shutdown); a later call makes them again.
        void Shutdown();

    private:
        struct Entry
        {
            std::string                    name;
            EditorModuleFactory            factory = nullptr;
            std::unique_ptr<IEditorModule> instance;
        };

        IEditorModule* Instance(Entry& e);

        std::vector<Entry> m_entries;
    };

    // Defined in Editor.exe by the generated list (src/Modules/Modules.generated.cpp).
    void RegisterLinkedEditorModules(ModuleRegistry& registry);
}

// In a module's editor .cpp, at namespace scope: the factory the generated list calls.
#define EDITOR_MODULE(Type)                                                   \
    static_assert(std::is_base_of_v<Editor::IEditorModule, Type>,             \
                  #Type " must derive from Editor::IEditorModule");           \
    std::unique_ptr<Editor::IEditorModule> CreateEditorModule_##Type()        \
    {                                                                         \
        return std::make_unique<Type>();                                      \
    }
