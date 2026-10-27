#include "ECS/StateToggle.h"

#include <iterator>

namespace StateToggle
{
namespace
{
    constexpr const char* kLegacyNames[] = {
        "State.IFrame", "State.SuperArmor", "State.InputBufferOpen", "State.RootMotionEnabled",
        "State.Invincible", "State.Vulnerable", "State.GuardBreakable", "State.HyperArmor",
    };

    std::string Describe(const NotifyTrack& track, const std::string& displayName, uint32_t id)
    {
        return "track '" + track.name + "', StateToggle '" + (displayName.empty() ? "#" + std::to_string(id) : displayName) + "'";
    }
}

bool ReadTag(const PropertyBag& params, Gameplay::Tag& out, std::string& error)
{
    const std::string name = params.GetString("tagName");
    if (!name.empty())
    {
        out = Gameplay::TagTable::Get().Intern(name, &error);
        return out.IsValid();
    }
    const int legacy = params.GetInt("tag", 0);
    if (legacy >= 1 && legacy <= static_cast<int>(std::size(kLegacyNames)))
    {
        out = Gameplay::TagTable::Get().Intern(kLegacyNames[legacy - 1], &error);
        return out.IsValid();
    }
    error = "names no tag: set tagName to a dotted name such as State.IFrame";
    return false;
}

std::vector<std::string> Problems(const std::vector<NotifyTrack>& tracks)
{
    std::vector<std::string> out;
    for (const NotifyTrack& track : tracks)
    {
        Gameplay::Tag tag;
        std::string error;
        for (const Notify& n : track.notifies)
            if (n.category == NotifyCategory::StateToggle && !ReadTag(n.params, tag, error))
                out.push_back(Describe(track, n.displayName, n.id) + " " + error);
        for (const NotifyState& s : track.states)
            if (s.category == NotifyCategory::StateToggle && !ReadTag(s.params, tag, error))
                out.push_back(Describe(track, s.displayName, s.id) + " " + error);
    }
    return out;
}
}
