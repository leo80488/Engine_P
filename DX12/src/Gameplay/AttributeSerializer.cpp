// AttributeSerializer.cpp: the Attributes line of a scene, a prefab and a save -
//   Attributes: profile=asset/RossiDuel/Attributes.dtable#boss set=Health:650:650,MaxHealth:650:650
// `set` is every attribute as name:base:current (Gameplay/AttributeSet.h); `profile` is the table row the
// entity's kind was filled from. A line with a profile and no `set` is filled from the row when it loads,
// which is how an author puts a kind of entity in a scene by hand.

#include "Gameplay/AttributeSet.h"
#include "Resource/ComponentKV.h"
#include "Resource/ComponentSerializers.h"
#include "System/Log.h"

#include <sstream>
#include <typeindex>

void RegisterAttributeSerializer(ComponentSerializerRegistry& reg)
{
    ComponentSerializer cs;
    cs.tag = "Attributes";
    cs.has = [](World& w, Entity e) { return w.GetComponent<AttributeSetComponent>(e) != nullptr; };
    cs.serialize = [](World& w, Entity e, std::ostringstream& ss)
    {
        const AttributeSetComponent* c = w.GetComponent<AttributeSetComponent>(e);
        if (!c) return;
        ss << "  Attributes:";
        if (!c->profile.empty()) ss << " profile=" << ComponentKV::PercentEncode(c->profile);
        ss << " set=" << ComponentKV::PercentEncode(Gameplay::Attr::ToText(*c)) << "\n";
    };
    cs.deserialize = [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*)
    {
        AttributeSetComponent c;
        const auto profile = kv.find("profile");
        const auto set     = kv.find("set");
        std::string error;
        if (profile != kv.end()) c.profile = ComponentKV::PercentDecode(profile->second);
        if (set != kv.end())
        {
            if (!Gameplay::Attr::FromText(c, ComponentKV::PercentDecode(set->second), &error))
                LOG_ERROR("Attributes: %s (the attributes before it are kept)", error.c_str());
        }
        else if (!c.profile.empty())
        {
            const std::string ref = c.profile;
            if (!Gameplay::Attr::ApplyProfile(c, ref, error))
            {
                LOG_ERROR("Attributes: %s", error.c_str());
                c.profile = ref;
            }
        }
        w.AddComponent<AttributeSetComponent>(e, std::move(c));
    };
    reg.Register(std::type_index(typeid(AttributeSetComponent)), std::move(cs));
}
