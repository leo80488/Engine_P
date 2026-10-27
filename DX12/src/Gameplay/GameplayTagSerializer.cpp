// GameplayTagSerializer.cpp: the GameplayTags line of a scene, a prefab and a save -
//   GameplayTags: tags=Team.Enemy:2,Rank.Boss:1
// holds the authored half of GameplayTagsComponent (Gameplay/GameplayTagsComponent.h); the tags an
// animation's StateToggle grants are rebuilt every frame and never written.

#include "Gameplay/GameplayTagsComponent.h"
#include "Resource/ComponentKV.h"
#include "Resource/ComponentSerializers.h"
#include "System/Log.h"

#include <sstream>
#include <typeindex>

void RegisterGameplayComponentSerializers(ComponentSerializerRegistry& reg)
{
    ComponentSerializer cs;
    cs.tag = "GameplayTags";
    cs.has = [](World& w, Entity e)
    {
        const auto* c = w.GetComponent<GameplayTagsComponent>(e);
        return c && !c->authored.Empty();
    };
    cs.serialize = [](World& w, Entity e, std::ostringstream& ss)
    {
        if (const auto* c = w.GetComponent<GameplayTagsComponent>(e))
            ss << "  GameplayTags: tags=" << ComponentKV::PercentEncode(c->authored.ToText()) << "\n";
    };
    cs.deserialize = [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*)
    {
        GameplayTagsComponent c;
        const auto it = kv.find("tags");
        std::string error;
        if (it != kv.end() && !c.authored.FromText(ComponentKV::PercentDecode(it->second), &error))
            LOG_ERROR("GameplayTags: %s (the tags before it are kept)", error.c_str());
        w.AddComponent<GameplayTagsComponent>(e, std::move(c));
    };
    reg.Register(std::type_index(typeid(GameplayTagsComponent)), std::move(cs));
    RegisterAttributeSerializer(reg);
}
