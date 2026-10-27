#pragma once

// The streaming texture upload's public vocabulary: what IGraphicsDevice::QueueTextureUpload
// takes and answers, and what GetStreamUploadStats reports. Small on purpose -
// IGraphicsDevice.h includes it; the queue's rules are in Graphics/StreamUploadQueue.h.

#include <cstdint>

namespace RHI { struct SubresourceData; }

namespace StreamUpload
{
    // LatestWins: only the newest data for a (texture, subresource, rect)
    // matters - a camera preview, a video frame. Requests merge, and one that
    // does not fit is dropped.
    // MustDeliver: every request must land - a font page's new glyphs. Never
    // merged; one that does not fit goes through a one-off staging buffer, and
    // its bytes are kept until its frame is submitted (within a cap - past it
    // the request still lands, without the copy that survives a lost frame).
    // Copies run in the order they were requested; a LatestWins merge at its
    // latest request's place.
    enum class Policy : uint8_t { LatestWins, MustDeliver };

    enum class Result : uint8_t
    {
        Queued,       // in the open frame's ring: copied before this frame's first reader
        Replaced,     // merged into an earlier LatestWins request for the same key
        CarriedOver,  // the frame was already taken by the render half: the next one copies it
        Fallback,     // MustDeliver that did not fit the ring: a one-off staging buffer
        Dropped,      // LatestWins that did not fit; a carry-over the CPU-copy cap had no room for
        Rejected,     // refused: the render half's thread, bad arguments, an unsupported rect
    };

    inline const char* ResultName(Result r)
    {
        switch (r)
        {
        case Result::Queued:      return "Queued";
        case Result::Replaced:    return "Replaced";
        case Result::CarriedOver: return "CarriedOver";
        case Result::Fallback:    return "Fallback";
        case Result::Dropped:     return "Dropped";
        case Result::Rejected:    return "Rejected";
        }
        return "?";
    }

    // A sub-rectangle of one subresource, in texels; width == 0 = all of it.
    struct Rect { uint32_t x = 0, y = 0, width = 0, height = 0; };

    struct Desc
    {
        // One SubresourceData per plane (NV12 / P010: two), each with its own
        // row pitch. Read during the call only - the data is copied.
        const RHI::SubresourceData* planes = nullptr;
        uint32_t planeCount = 1;
        uint32_t mip = 0, arraySlice = 0;
        Rect     rect{};                // single-plane, uncompressed formats only
        Policy   policy = Policy::LatestWins;
    };

    // Running totals since boot (they never reset), and the ring's shape.
    struct Stats
    {
        bool     active = false;          // a device with a stream uploader
        uint64_t ringBytesPerSlot = 0;
        uint32_t slots = 0;
        uint64_t requests = 0, bytes = 0;
        uint64_t queued = 0, replaced = 0, carriedOver = 0, fallback = 0, dropped = 0, rejected = 0;
        uint64_t stale = 0;               // skipped at record: the texture was destroyed / its id reused
        uint64_t requeued = 0;            // MustDeliver queued again after a frame that never reached the GPU
        uint64_t lostDropped = 0;         // lost with such a frame (LatestWins, or no CPU copy to requeue)
        uint64_t recordedLists = 0;       // StreamUpload command lists recorded
        uint64_t recordedCopies = 0;      // copies recorded into them
        uint64_t ringHighWater = 0;       // the most bytes one slot's ring has held
        // The CPU copies (carried-over requests, MustDeliver until submitted):
        // held now, their peak, the cap, and the requests the cap refused one.
        uint64_t retainedBytes = 0, retainedHighWater = 0, retainCap = 0, capped = 0;
    };
}
