#pragma once

// NotifyIO - the one converter between NotifyTrack data and its JSON text form, used by
// ComponentSerializers (TimelineComponent in .iscn) and AnimationSerializer (the .ianim notify
// section: notifies that live on the clip itself). One schema keeps the two interchangeable, so
// the editor moves notify data between an entity timeline and a clip asset without re-mapping.
// Do not change the shape: existing scene files load with it.
//   {
//     "dur": <float clipDuration>,
//     "nid": <uint nextNotifyId>,
//     "trk": [ { "nm", "cat", "mu", "n":[ <notify> ], "s":[ <state> ] } ]
//   }
//   notify = { "id","cat","t","nm","col","pb": { key: {"t","v"} } }
//   state  = { "id","cat","t0","t1","nm","col","pb": { ... } }
//   pb value: { "t": "i"|"f"|"s"|"v3"|"v4", "v": <payload> }

#include "ECS/NotifyTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace NotifyIO
{
    // Serialize notify tracks, the clip duration and the next-id allocator to a compact JSON
    // string. Never throws.
    std::string TracksToJsonString(const std::vector<NotifyTrack>& tracks,
                                   float                           clipDuration,
                                   uint32_t                        nextNotifyId);

    // Parse a string produced by TracksToJsonString. @p outClipDuration / @p outNextNotifyId may
    // be null. @p outTracks is cleared first; a parse error returns false with the two optional
    // outputs untouched. An empty string yields no tracks and returns true.
    bool TracksFromJsonString(const std::string&        json,
                              std::vector<NotifyTrack>& outTracks,
                              float*                    outClipDuration = nullptr,
                              uint32_t*                 outNextNotifyId = nullptr);
}
