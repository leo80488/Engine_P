#pragma once

// An engine module: a feature outside the core that a project enables in game.json
// ("modules"). Its descriptor is src/Modules/<Name>.module.json;
// its .cpp says ENGINE_MODULE(Class), and tools/gen_modules.py writes the per-exe list that
// calls the factory. A module the list does not call is not linked into the exe, so the core
// never names a module's types or functions: it reaches a module only through this interface
// and the extension points the module registers into.

#include <memory>
#include <string>
#include <type_traits>

class World;
class IGraphicsDevice;
class Renderer;
class SystemRegistry;
class ComponentSerializerRegistry;
class RenderFeatureRegistry;
namespace DX12Physics { class PhysicsSystem; }
namespace sol { class state; }
namespace Resource { class ResourceManager; class AssetManager; }

namespace Module
{
    enum class Kind { Cpp, Script, Content };

    // What the boot has built when a hook runs; a hook reads only the members its
    // contract names (null before then).
    struct Context
    {
        World*                      world     = nullptr;
        IGraphicsDevice*            gfx       = nullptr;
        Renderer*                   renderer  = nullptr;   // OnRendererReady and later
        sol::state*                 lua       = nullptr;   // RegisterLua and later
        SystemRegistry*             systems   = nullptr;
        DX12Physics::PhysicsSystem* physics   = nullptr;
        Resource::ResourceManager*  resources = nullptr;
        Resource::AssetManager*     assets    = nullptr;
    };

    class IEngineModule
    {
    public:
        virtual ~IEngineModule() = default;

        // Every linked module, enabled or not, in main() before anything else: no device, no content
        // roots, no log. For an SDK that must hook the device as it is created (Streamline installs its
        // backend here). The project's choice is not known yet, so the editor follows what is linked.
        virtual void OnProcessStart() {}

        // Every linked module, enabled or not, before the first scene or prefab is read: the
        // component serializers and the other type tables, so a project's data round-trips
        // in the editor even when the module is off. Must not create runtime state.
        virtual void RegisterTypes(ComponentSerializerRegistry& components) { (void)components; }

        // The rest run only for an enabled module, in dependency order (Shutdown in reverse).
        // After the core systems, before the renderer exists (world, gfx, resources, assets).
        virtual void Initialize(Context& ctx) { (void)ctx; }
        // The renderer is compiling: the module's render features (Renderer/RenderFeature.h), added
        // after the core's. Not REGISTER_RENDER_FEATURE: that list is compiled into EngineCore.
        virtual void RegisterRenderFeatures(RenderFeatureRegistry& features) { (void)features; }
        // The renderer is compiled (passes and the skinned subsystem exist).
        virtual void OnRendererReady(Context& ctx) { (void)ctx; }
        // The Lua state is up; global tables bind here, before the boot scripts run.
        virtual void RegisterLua(Context& ctx) { (void)ctx; }
        // The tick systems register, before SystemRegistry finalizes the order.
        virtual void RegisterSystems(Context& ctx) { (void)ctx; }
        // A device was plugged in or out (the window's DevicesChanged: a camera, a controller).
        virtual void OnDeviceChange() {}
        // The orderly exit, no frame in flight, device and renderer alive.
        virtual void Shutdown() {}
    };

    using Factory = std::unique_ptr<IEngineModule> (*)();
}

// In a module's .cpp, at namespace scope: the factory the generated list calls.
// @p Type is the class's unqualified name; the descriptor's engineClass names it.
#define ENGINE_MODULE(Type)                                                   \
    static_assert(std::is_base_of_v<Module::IEngineModule, Type>,             \
                  #Type " must derive from Module::IEngineModule");           \
    std::unique_ptr<Module::IEngineModule> CreateEngineModule_##Type()        \
    {                                                                         \
        return std::make_unique<Type>();                                      \
    }
