#pragma once

// Settings/SettingsSnapshot.h - the per-frame settings snapshot: every row's effective
// value from the settings store, flat and indexed like the store's rows, plus the store
// revision those values are from.
//
// Filled on main at the game -> render hand-off, into the buffer the frame in flight
// owns (Renderer::AdvanceSceneWrite), so every decision one frame makes can read the
// same values. A buffer is refilled only when the store has moved on since it was last
// filled; otherwise the hand-off costs a compare.
//
// Only the harness reads it (Renderer::SettingsPublished -> frames.jsonl);
// Renderer::SettingsRead has no caller.

#include <cstdint>
#include <vector>

namespace Resource { struct EngineSettings; }

namespace Settings
{
    template <class S> class BasicStore;

    // One row, 4 bytes: Bool / Int / Enum / Choice in i (a Bool is 0 / 1),
    // Float in f. A String row holds its label index when it has a legal set
    // (antialiasing.dlssRRModel), else -1 - strings are not render inputs.
    union SnapValue
    {
        int32_t i;
        float   f;
    };

    struct Snapshot
    {
        static constexpr uint64_t kNever = ~0ull;
        uint64_t               revision = kNever;   // the store revision the values are from
        uint64_t               fills    = 0;        // times this buffer was refilled
        std::vector<SnapValue> values;
    };

    // Refill @p out from the player store when it is behind; true when it copied.
    bool FillSnapshot(const BasicStore<Resource::EngineSettings>& store, Snapshot& out);
}
