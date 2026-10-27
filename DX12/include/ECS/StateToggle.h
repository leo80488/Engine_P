#pragma once

// StateToggle notifies: what the tag a clip's StateToggle names is, read the same way by the
// timeline at run time, the load-time check and the editor's property panel.

#include "ECS/NotifyTypes.h"
#include "Gameplay/GameplayTags.h"

#include <string>
#include <vector>

namespace StateToggle
{
    // The tag @p params names: "tagName", a dotted name ("State.IFrame"); else "tag", the index the
    // first version of the notify wrote (1 IFrame, 2 SuperArmor, 3 InputBufferOpen, 4 RootMotionEnabled,
    // 5 Invincible, 6 Vulnerable, 7 GuardBreakable, 8 HyperArmor - each under "State."). False, with
    // the reason in @p error, when it names none or an illegal one.
    bool ReadTag(const PropertyBag& params, Gameplay::Tag& out, std::string& error);

    // The problems of every StateToggle notify and interval in @p tracks, one line each, naming the
    // track and the notify. Empty when all of them name a tag.
    std::vector<std::string> Problems(const std::vector<NotifyTrack>& tracks);
}
