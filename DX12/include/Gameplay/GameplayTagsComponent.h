#pragma once

// The components of the gameplay tags (Gameplay/GameplayTags.h) on an entity.

#include "Gameplay/GameplayTags.h"

#include <vector>

// GameplayTagsComponent - the tags an entity carries. `authored` is what scripts (Tags.Add) and the
// scene or a prefab (the GameplayTags line) put on it: it is saved. `derived` is rebuilt every frame
// by StateToggleSystem from the animation's open StateToggle intervals and timed toggles: it is never
// saved, so a clip cancelled mid-interval leaves nothing behind. Queries ask both.
struct GameplayTagsComponent
{
    Gameplay::TagCounts authored;
    Gameplay::TagCounts derived;

    bool Matches(Gameplay::Tag tag) const { return authored.Matches(tag) || derived.Matches(tag); }
    int  Count(Gameplay::Tag tag) const { return authored.Count(tag) + derived.Count(tag); }
};

// ActiveStateToggles - the StateToggle state of one animated entity, runtime only. TimelineSystem
// rewrites `open` every frame (the intervals the clip's current time lies in, as for ActiveFXGates);
// StateToggleSystem keeps `held` (point toggles) and turns both into the owner's derived tags.
struct ActiveStateToggles
{
    struct Held
    {
        Gameplay::Tag tag;
        bool          timed = false;      // false: held until a toggle switches it off
        float         remaining = 0.f;    // game seconds left when timed
    };

    std::vector<Gameplay::Tag> open;
    std::vector<Held>          held;
};
