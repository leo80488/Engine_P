#pragma once

// SeqTarget - the Property track's address space.
// A hand-written table mapping a stable NAME ("light.intensity") to a getter and a setter on a
// component. Not driven by the reflection system: MaterialComponent has no descriptor (its
// inspector is hand-written), and no reflection field kind expresses a quaternion. Adding a target
// is one table row.
// The name is HASHED once at load (Track::targetId), so per-frame lookup is an integer compare.

#include "ECS/ECS.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Seq
{

// Up to four scalar components, matching Channel roles Scalar0..Scalar3. A float target uses
// one; a colour uses three or four.
struct SeqTarget
{
    const char* name  = "";
    uint32_t    id    = 0;      // HashTarget(name), filled by RegisterSeqTarget
    uint8_t     count = 1;      // how many of v[] are meaningful
    const char* tooltip = "";

    // Both return false when the entity does not carry the component, which is
    // how a mis-bound track becomes inert instead of a crash.
    bool (*Get)(World&, Entity, float* v) = nullptr;
    bool (*Set)(World&, Entity, const float* v) = nullptr;
};

void                          RegisterSeqTarget(const SeqTarget& t);
const SeqTarget*              FindSeqTarget(uint32_t id);
const std::vector<SeqTarget>& AllSeqTargets();

// Populates the built-in table (light, lens, fog, material).
// Idempotent - safe to call from more than one entry point.
void RegisterBuiltinSeqTargets();

} // namespace Seq
