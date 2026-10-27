#pragma once

// The modules this exe knows (declared), the C++ ones it links, and the ones the project
// enables (game.json "modules"). The per-exe generated list (src/Modules/Modules.generated.cpp,
// tools/gen_modules.py) fills it from main() before anything else runs; App drives the hooks.

#include "Module/EngineModule.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class ComponentSerializerRegistry;
class RenderFeatureRegistry;
namespace sol { class state; }

namespace Module
{
    struct Decl
    {
        std::string              name;
        Kind                     kind = Kind::Cpp;
        std::vector<std::string> dependsOn;
        bool                     enabledByDefault = true;
        std::vector<std::string> components;   // component tags its serializers register
        std::vector<std::string> scripts;      // script manifest paths it owns
    };

    class Registry
    {
    public:
        static Registry& Get();

        // ---- Filled by the generated list ----
        void Declare(Decl decl);
        void Link(std::string_view name, Factory factory);

        // ---- Boot ----
        // Every linked module's OnProcessStart (main(), right after the generated list).
        void OnProcessStart();
        // Every linked module's RegisterTypes, once; GetComponentRegistry calls it when it
        // builds the registry.
        void RegisterTypes(ComponentSerializerRegistry& components);
        // The project's choice: game.json "modules" through AssetFS (content roots mounted),
        // dependencies pulled in. Logs [MODULES]. Without the key, every module's default.
        void Configure();
        // Configure from @p choicesJson (the "modules" object's text, or "" for none): tests.
        void ConfigureFrom(std::string_view choicesJson);

        void Initialize(Context& ctx);
        void RegisterRenderFeatures(RenderFeatureRegistry& features);
        void OnRendererReady(Context& ctx);
        void RegisterLua(Context& ctx);
        void RegisterSystems(Context& ctx);
        void OnDeviceChange();
        void Shutdown();

        // ---- Queries ----
        bool IsDeclared(std::string_view name) const;
        bool IsLinked(std::string_view name) const;
        // Enabled by the project and present: a C++ module must also be linked.
        bool IsEnabled(std::string_view name) const;
        // The declared module whose descriptor lists component @p tag; null = none.
        const std::string* OwnerOfComponentTag(const std::string& tag) const;
        // False when a declared module owns script @p path (its descriptor's "scripts") and is off:
        // the boot loader skips it. A script no module owns is always on.
        bool IsScriptEnabled(std::string_view path) const;
        // One line per declared module: name, kind, linked, enabled.
        std::string Describe() const;

        struct Status
        {
            std::string name;
            Kind        kind;
            bool        linked;
            bool        enabled;
        };
        std::vector<Status> Statuses() const;

    private:
        struct Entry
        {
            Decl                           decl;
            Factory                        factory = nullptr;
            std::unique_ptr<IEngineModule> instance;
            bool                           enabled = false;
        };

        Entry*       Find(std::string_view name);
        const Entry* Find(std::string_view name) const;
        void         Instantiate();
        // Linked and enabled C++ modules, dependencies first.
        std::vector<Entry*> Active();

        std::vector<Entry>                           m_entries;
        std::unordered_map<std::string, std::string> m_componentOwner;   // tag -> module
        bool                                         m_typesRegistered = false;
        bool                                         m_configured      = false;
    };

    // Defined per exe by the generated list (src/Modules/Modules.generated.cpp).
    void RegisterLinkedModules(Registry& registry);

    // Engine.hasModule(name) / Engine.modules().
    void RegisterLuaBindings(sol::state& lua);
}
