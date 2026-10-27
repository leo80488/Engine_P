#include "Harness/WorldStateDump.h"

#ifdef WITH_HARNESS

#include "ECS/ECS.h"
#include "Resource/ComponentSerializers.h"
#include "System/PathUtil.h"
#include "System/Log.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace Harness
{
namespace
{
    // The registered serializers that are NOT read-only. Each calls ECS::EnsureGuidOn
    // (src/Resoruce/ComponentSerializers.cpp: FollowSocket's and FollowEntity's target,
    // Script's Entity-typed exposed vars, CvParent's parent), which ADDS a GuidComponent to a
    // live entity that lacks one - observing would change the world, and two dumps of the
    // same world could differ.
    // Remove a tag once its serializer is read-only; the skipped set is written into the
    // dump, so the gap stays visible.
    const char* const kMutatingTags[] = {
        "FollowSocket", "FollowEntity", "Script", "CvParent",
    };

    bool IsMutatingTag(const std::string& tag)
    {
        for (const char* t : kMutatingTags)
            if (tag == t) return true;
        return false;
    }

    std::string JsonEscape(const std::string& in)
    {
        std::string out;
        out.reserve(in.size() + 16);
        for (char c : in)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char esc[8];
                        std::snprintf(esc, sizeof(esc), "\\u%04X",
                                      static_cast<unsigned>(static_cast<unsigned char>(c)));
                        out += esc;
                    }
                    else out += c;
                    break;
            }
        }
        return out;
    }

    // Strip the registry's leading indent and trailing newline so the payload
    // reads as one clean line inside the JSON string.
    std::string Trim(std::string s)
    {
        const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }
}

bool WriteWorldStateDump(World& world, const std::string& utf8Path)
{
    auto& reg = GetComponentRegistry();

    // Entities, ascending (see the determinism note in the header): the backing vector's
    // order reflects destroy / create history, not identity.
    std::vector<Entity> entities;
    entities.reserve(world.GetEntities().size());
    for (Entity e : world.GetEntities())
        if (world.IsAlive(e)) entities.push_back(e);
    std::sort(entities.begin(), entities.end());

    std::string j;
    j.reserve(64 * 1024);
    j += "{\n  \"schema\": 1,\n";
    j += "  \"entityCount\": " + std::to_string(entities.size()) + ",\n";

    j += "  \"skippedTags\": [";
    for (std::size_t i = 0; i < std::size(kMutatingTags); ++i)
    {
        if (i) j += ", ";
        j += "\"";
        j += kMutatingTags[i];
        j += "\"";
    }
    j += "],\n";
    j += "  \"skippedReason\": \"these serializers call ECS::EnsureGuidOn and would "
         "mutate the world they observe\",\n";
    j += "  \"entities\": [\n";

    std::size_t written = 0;
    std::vector<std::pair<std::string, std::string>> comps;   // tag -> payload

    for (std::size_t ei = 0; ei < entities.size(); ++ei)
    {
        const Entity e = entities[ei];

        comps.clear();
        for (const std::type_index& ti : world.GetComponentTypeIndices(e))
        {
            auto it = reg.All().find(ti);
            if (it == reg.All().end())    continue;   // not a serializable type
            const ComponentSerializer& cs = it->second;
            if (IsMutatingTag(cs.tag))    continue;   // see kMutatingTags
            if (!cs.has || !cs.serialize) continue;
            if (!cs.has(world, e))        continue;

            std::ostringstream oss;
            cs.serialize(world, e, oss);
            comps.emplace_back(cs.tag, Trim(oss.str()));
        }

        // Components ascending by tag: the registry is an unordered_map, and the per-entity
        // type list is in unspecified order.
        std::sort(comps.begin(), comps.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        if (written) j += ",\n";
        j += "    {\"id\": " + std::to_string(static_cast<unsigned>(e)) + ", \"components\": {";
        for (std::size_t ci = 0; ci < comps.size(); ++ci)
        {
            if (ci) j += ", ";
            j += "\"" + JsonEscape(comps[ci].first) + "\": \"" +
                 JsonEscape(comps[ci].second) + "\"";
        }
        j += "}}";
        ++written;
    }

    j += "\n  ]\n}\n";

    // WriteFileBytes creates the parent directory and returns false rather than throwing.
    if (!Sys::WriteFileBytes(utf8Path, j.data(), j.size()))
    {
        LOG_ERROR("WorldStateDump: could not write '%s'", utf8Path.c_str());
        return false;
    }

    LOG_INFO("WorldStateDump: %zu entities -> %s", written, utf8Path.c_str());
    return true;
}

}  // namespace Harness

#endif  // WITH_HARNESS
