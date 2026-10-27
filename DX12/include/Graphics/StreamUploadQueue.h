#pragma once

// The device-independent half of the streaming texture upload: which frame a request lands in,
// where in that frame's slot ring its bytes go, what merges, what overflows, and what survives a
// frame the render half never submitted. No D3D12 here - GraphicsDX12_StreamUpload.cpp owns the
// UPLOAD buffers, writes the bytes at the offsets this hands out, records the copies and checks
// the textures' identity - so tests/unit/stream_upload_queue_test.cpp drives every rule below
// without a GPU.
//
// THE WINDOW. Two frame numbers, both moved under the owner's mutex: the OPEN frame
// (AcquireUploadSlot(F) opens F on its slot) and the CONSUMED frame (the render half's
// RecordPendingStreamUploads for F takes F's list). A request is recorded in the open frame while
// that frame has not been taken; after that it is CARRIED OVER - kept with its bytes - into the
// next frame the next acquire opens. Frame overlap is why both are needed: the game half can be
// in F+1's early phases (no acquire yet, so F is still "open") after the render half took F, and
// IsUploadSlotWritable() still says yes there.
//
// THE RING. One bump allocator per upload slot, reset when the slot is acquired again - the
// caller guarantees the slot's previous GPU use retired.
//
// ORDER. Every request gets a sequence number when it is made, and a taken list is recorded in
// that order - so a carried-over request, or one queued again after a lost frame, runs where its
// request was made, never on top of a newer one. A LatestWins merge moves to its latest
// request's place.
//
// GPU COPIES. An Item with gpuSource is a GPU-to-GPU copy whose source the payload holds (a
// decoder's frame): no bytes, no ring space, so it carries over and survives a lost frame without
// a CPU copy. It merges only with copies.
//
// MERGING. LatestWins requests merge by Key (texture, generation, subresource, rect): the same
// footprint size overwrites the earlier bytes in place, another size takes new space (the old is
// reclaimed at the reset). MustDeliver requests never merge. A request that does not fit:
// LatestWins is dropped, MustDeliver goes to a one-off staging buffer the device creates
// (Fallback) - and in both cases the counters say so.
//
// CPU COPIES. An item keeps its bytes while it has no place on the GPU (carried over) or must
// survive a lost frame (MustDeliver until submitted). Their total is counted and capped (Init's
// @p retainCap): past the cap a carry-over cannot be kept and is dropped, and a MustDeliver in
// the open frame is still delivered but without its lost-frame copy - both counted as capped.
//
// LOST FRAMES. Recorded items stay with their slot until the frame's EndFrame reports them
// submitted. The next Take finds every older frame that will never reach the GPU - recorded but
// not submitted (the render half threw after taking it), or never taken (it skipped the frame) -
// and moves their MustDeliver items, from their own copy of the bytes, into the list it hands
// out, in request order; their LatestWins items are dropped (a newer request has the data, or
// nothing needs it). A slot that comes round again with no Take in between does the same at its
// Open.
//
// Every method assumes the caller holds the one mutex that guards a Queue.

#include "Graphics/StreamUploadTypes.h"   // Policy, Result

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace StreamUpload
{
    // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT: where a copy's footprint may start.
    constexpr uint64_t kPlacementAlign = 512;

    // One slot's bump allocator.
    class Ring
    {
    public:
        void     Init(uint64_t capacity) { m_capacity = capacity; m_cursor = 0; m_highWater = 0; }
        void     Reset() { m_cursor = 0; }
        uint64_t Capacity() const { return m_capacity; }
        uint64_t Used() const { return m_cursor; }
        uint64_t HighWater() const { return m_highWater; }

        // The offset of @p size bytes aligned to @p align (a power of two), or
        // nothing when they do not fit. A zero size never fits.
        std::optional<uint64_t> Allocate(uint64_t size, uint64_t align = kPlacementAlign)
        {
            if (size == 0 || align == 0 || (align & (align - 1)) != 0) return std::nullopt;
            const uint64_t at = (m_cursor + align - 1) & ~(align - 1);
            if (at > m_capacity || size > m_capacity - at) return std::nullopt;
            m_cursor    = at + size;
            m_highWater = (std::max)(m_highWater, m_cursor);
            return at;
        }

    private:
        uint64_t m_capacity  = 0;
        uint64_t m_cursor    = 0;
        uint64_t m_highWater = 0;
    };

    // What a LatestWins request merges on. All 32-bit, so no padding to compare.
    struct Key
    {
        uint32_t texture     = 0;   // the texture's handle id
        uint32_t generation  = 0;   // its creation stamp: an id reused by another texture differs here
        uint32_t subresource = 0;
        uint32_t x = 0, y = 0, w = 0, h = 0;   // w == 0: the whole subresource

        bool operator==(const Key& o) const
        {
            return texture == o.texture && generation == o.generation && subresource == o.subresource &&
                   x == o.x && y == o.y && w == o.w && h == o.h;
        }
    };

    // One queued copy. @p Payload is the device's: the destination texture, the
    // copy's footprint, a staging buffer.
    template <class Payload>
    struct Item
    {
        Key      key;
        Policy   policy = Policy::LatestWins;
        uint64_t seq    = 0;       // when it was requested (Admit): the order its list records it in
        uint64_t frame  = 0;       // the frame whose list records it (set when it reaches a slot)
        // A GPU-to-GPU copy (QueueNativeTextureCopy): its source is a resource the payload holds, so it
        // has no bytes and takes no ring space - it can be carried over and recorded again after a lost
        // frame without a CPU copy.
        bool     gpuSource = false;
        bool     staged = false;   // in a one-off staging buffer (Payload owns it), not the ring
        uint64_t offset = 0;       // in the slot's ring, when !staged
        uint64_t size   = 0;       // bytes, laid out exactly as the copy reads them
        // The bytes themselves, in the copy's layout, while the item has no
        // place on the GPU yet (carried over) or must survive a lost frame
        // (MustDeliver until its frame is submitted). Empty otherwise.
        std::vector<uint8_t> bytes;
        Payload  payload{};
    };

    struct Counters
    {
        uint64_t requests = 0, bytes = 0;
        uint64_t queued = 0, replaced = 0, carriedOver = 0, fallback = 0, dropped = 0, rejected = 0;
        uint64_t requeued = 0;      // MustDeliver queued again after a lost frame
        uint64_t lostDropped = 0;   // lost with a frame that never reached the GPU (LatestWins, or no CPU copy)
        uint64_t forgotten = 0;     // dropped because their texture was destroyed first
        uint64_t capped = 0;        // a CPU copy the cap refused (a carry-over dropped, a MustDeliver without one)
    };

    // Where Admit put a request: the caller writes the bytes there (into the
    // slot's ring at @c offset, or into its new staging buffer), or keeps them
    // in the item (carried over).
    struct Placement
    {
        Result   result = Result::Rejected;
        uint32_t slot   = 0;
        uint64_t offset = 0;
        bool     needsStaging = false;   // Fallback: create a staging buffer of the item's size
        bool     copy   = false;         // a GPU-to-GPU copy (Item::gpuSource): no bytes to write
        // The item's index in Pending(slot); for a CarriedOver result in CarryOver(), for Take's
        // reclaimed placements in the list Take returned.
        size_t   index  = 0;
    };

    template <class Payload>
    class Queue
    {
    public:
        using ItemT = Item<Payload>;

        void Init(uint32_t slots, uint64_t ringBytes,
                  uint64_t retainCap = (std::numeric_limits<uint64_t>::max)())
        {
            m_rings.assign(slots, Ring{});
            for (Ring& r : m_rings) r.Init(ringBytes);
            m_pending.assign(slots, {});
            m_recorded.assign(slots, {});
            m_submitted.assign(slots, true);
            m_carry.clear();
            m_open = false;
            m_consumed = false;
            m_retained = 0;
            m_retainedHigh = 0;
            m_retainCap = retainCap;
        }

        uint32_t Slots() const { return static_cast<uint32_t>(m_rings.size()); }
        const Ring& RingOf(uint32_t slot) const { return m_rings[slot]; }
        bool     IsOpen() const { return m_open; }
        uint64_t OpenFrame() const { return m_openFrame; }
        uint32_t OpenSlot() const { return m_openSlot; }
        bool     HasConsumed() const { return m_consumed; }
        uint64_t ConsumedFrame() const { return m_consumedFrame; }
        const Counters& Stats() const { return m_stats; }
        uint64_t RingHighWater() const
        {
            uint64_t h = 0;
            for (const Ring& r : m_rings) h = (std::max)(h, r.HighWater());
            return h;
        }

        // The CPU copies (Item::bytes) held right now, their peak, and whether
        // @p n more would stay within the cap.
        uint64_t RetainedBytes() const { return m_retained; }
        uint64_t RetainedHighWater() const { return m_retainedHigh; }
        uint64_t RetainCap() const { return m_retainCap; }
        bool     CanRetain(uint64_t n) const { return n <= m_retainCap && m_retained <= m_retainCap - n; }

        // True while a request would be recorded in the open frame.
        bool AcceptsOpenFrame() const
        {
            return m_open && (!m_consumed || m_consumedFrame < m_openFrame);
        }

        std::vector<ItemT>&       Pending(uint32_t slot)       { return m_pending[slot]; }
        const std::vector<ItemT>& Pending(uint32_t slot) const { return m_pending[slot]; }
        std::vector<ItemT>&       CarryOver()                  { return m_carry; }

        // A request. @p item carries its key, policy and size, and its bytes when
        // the caller kept them: a carry-over needs them (one without is dropped -
        // the caller found no room under the cap), a MustDeliver keeps them until
        // submitted, a LatestWins in a slot drops them.
        Placement Admit(ItemT item)
        {
            ++m_stats.requests;
            m_stats.bytes += item.size;
            item.seq = ++m_seq;
            if (item.gpuSource) return AdmitCopy(std::move(item));
            Placement p;
            if (item.size == 0) { ++m_stats.rejected; p.result = Result::Rejected; return p; }

            if (!AcceptsOpenFrame())
            {
                if (item.bytes.empty()) { ++m_stats.dropped; p.result = Result::Dropped; return p; }
                // Merge a LatestWins into the carried request for the same key.
                if (item.policy == Policy::LatestWins)
                    for (size_t i = 0; i < m_carry.size(); ++i)
                        if (!m_carry[i].gpuSource && m_carry[i].policy == Policy::LatestWins &&
                            m_carry[i].key == item.key)
                        {
                            Release(m_carry[i].bytes.size());
                            Retain(item.bytes.size());
                            m_carry[i] = std::move(item);
                            ++m_stats.carriedOver;
                            p.result = Result::CarriedOver;
                            p.index  = i;
                            return p;
                        }
                Retain(item.bytes.size());
                m_carry.push_back(std::move(item));
                ++m_stats.carriedOver;
                p.result = Result::CarriedOver;
                p.index  = m_carry.size() - 1;
                return p;
            }

            const uint32_t s = m_openSlot;
            p.slot = s;
            item.frame = m_openFrame;
            std::vector<ItemT>& pend = m_pending[s];

            if (item.policy == Policy::LatestWins)
            {
                for (size_t i = 0; i < pend.size(); ++i)
                {
                    ItemT& old = pend[i];
                    if (old.gpuSource || old.policy != Policy::LatestWins || !(old.key == item.key)) continue;
                    if (!old.staged && old.size == item.size)
                    {
                        // In place: the same footprint over the same bytes,
                        // recorded where this request was made.
                        ++m_stats.replaced;
                        p.result = Result::Replaced;
                        p.offset = old.offset;
                        p.index  = i;
                        old.seq     = item.seq;
                        old.payload = std::move(item.payload);
                        return p;
                    }
                    if (std::optional<uint64_t> at = m_rings[s].Allocate(item.size))
                    {
                        Release(old.bytes.size());
                        item.offset = *at;
                        item.staged = false;
                        item.bytes.clear();
                        old = std::move(item);
                        ++m_stats.replaced;
                        p.result = Result::Replaced;
                        p.offset = old.offset;
                        p.index  = i;
                        return p;
                    }
                    // No room for the new size: the latest is lost, the earlier stays.
                    ++m_stats.dropped;
                    p.result = Result::Dropped;
                    return p;
                }
            }

            if (std::optional<uint64_t> at = m_rings[s].Allocate(item.size))
            {
                item.offset = *at;
                item.staged = false;
                if (item.policy == Policy::LatestWins) item.bytes.clear();
                Retain(item.bytes.size());
                pend.push_back(std::move(item));
                ++m_stats.queued;
                p.result = Result::Queued;
                p.offset = pend.back().offset;
                p.index  = pend.size() - 1;
                return p;
            }
            if (item.policy == Policy::LatestWins)
            {
                ++m_stats.dropped;
                p.result = Result::Dropped;
                return p;
            }
            item.staged = true;
            item.offset = 0;
            Retain(item.bytes.size());
            pend.push_back(std::move(item));
            ++m_stats.fallback;
            p.result       = Result::Fallback;
            p.needsStaging = true;
            p.index        = pend.size() - 1;
            return p;
        }

        // AcquireUploadSlot(@p frame) on @p slot, after the slot's previous GPU
        // use is known retired. Resets the slot's ring, opens the frame, and
        // returns what the caller must now place: the MustDeliver items of a
        // frame this slot never submitted (from their bytes), then the
        // carried-over ones - each with its new placement already decided (the
        // caller copies item.bytes to the slot's ring at the offset, or creates
        // staging for a Fallback, then calls DropBytes on a LatestWins). The
        // returned items are also in Pending(slot) at the returned indices.
        //
        // @p released receives the previous use's items (their staging buffers
        // can go now): the recorded ones, and anything left unrecorded.
        std::vector<Placement> Open(uint64_t frame, uint32_t slot, std::vector<ItemT>& released)
        {
            std::vector<ItemT> requeue;
            // A frame recorded on this slot but never submitted (and no Take
            // since to find it): its copies never ran.
            if (!m_submitted[slot]) Salvage(m_recorded[slot], requeue);
            // Never recorded at all (the render half skipped the frame): same rule.
            Salvage(m_pending[slot], requeue);
            ReleaseAll(m_recorded[slot], released);
            ReleaseAll(m_pending[slot], released);
            m_submitted[slot] = true;
            m_rings[slot].Reset();

            m_open      = true;
            m_openFrame = frame;
            m_openSlot  = slot;

            for (ItemT& it : m_carry) { Release(it.bytes.size()); requeue.push_back(std::move(it)); }
            m_carry.clear();

            std::vector<Placement> placed;
            placed.reserve(requeue.size());
            for (ItemT& it : requeue)
            {
                Placement p = Place(std::move(it), slot);
                if (p.result != Result::Dropped) placed.push_back(p);
            }
            return placed;
        }

        // The render half, frame @p frame on @p slot: takes the slot's list
        // (the caller records it) and marks the frame consumed. The items stay
        // with the slot as "recorded" until Submitted(slot) - or the next Open
        // of the slot, which treats them as lost.
        //
        // First, every older frame that will never reach the GPU gives its
        // MustDeliver items to this list (LOST FRAMES above): @p reclaimed
        // receives their placements - indices into the returned list - and the
        // caller writes their bytes as after Open; @p released receives what
        // the lost lists held (their staging can go).
        std::vector<ItemT>* Take(uint64_t frame, uint32_t slot, std::vector<Placement>& reclaimed,
                                 std::vector<ItemT>& released)
        {
            reclaimed.clear();
            std::vector<ItemT> lost;
            for (uint32_t s = 0; s < Slots(); ++s)
            {
                // A list taken earlier and never submitted (this slot's too: no
                // Open came between).
                if (!m_submitted[s])
                {
                    Salvage(m_recorded[s], lost);
                    ReleaseAll(m_recorded[s], released);
                    m_submitted[s] = true;
                }
                if (s == slot) continue;
                // A frame before this one that was never taken. On overlap the
                // NEXT frame may already be open on another slot: its items stay.
                std::vector<ItemT> older;
                std::vector<ItemT>& pend = m_pending[s];
                for (size_t i = 0; i < pend.size();)
                {
                    if (pend[i].frame < frame)
                    {
                        older.push_back(std::move(pend[i]));
                        pend.erase(pend.begin() + static_cast<std::ptrdiff_t>(i));
                    }
                    else ++i;
                }
                Salvage(older, lost);
                ReleaseAll(older, released);
            }

            m_consumed      = true;
            m_consumedFrame = frame;
            ReleaseAll(m_recorded[slot], released);   // a submitted list: its GPU use is the caller's to know
            m_recorded[slot].swap(m_pending[slot]);
            std::vector<ItemT>& out = m_recorded[slot];
            std::vector<uint64_t> lostSeqs;
            lostSeqs.reserve(lost.size());
            for (ItemT& it : lost)
            {
                it.frame = frame;
                if (it.gpuSource)
                {
                    it.offset = 0;
                    it.staged = false;
                }
                else if (std::optional<uint64_t> at = m_rings[slot].Allocate(it.size))
                {
                    it.offset = *at;
                    it.staged = false;
                }
                else
                {
                    it.offset = 0;
                    it.staged = true;
                    ++m_stats.fallback;
                }
                Retain(it.bytes.size());
                lostSeqs.push_back(it.seq);
                out.push_back(std::move(it));
            }
            // Request order (ORDER above): the salvaged requests were made
            // before this frame's, a carried one before its frame's own.
            std::stable_sort(out.begin(), out.end(), [](const ItemT& a, const ItemT& b) { return a.seq < b.seq; });
            std::sort(lostSeqs.begin(), lostSeqs.end());
            for (size_t i = 0; i < out.size() && !lostSeqs.empty(); ++i)
            {
                if (!std::binary_search(lostSeqs.begin(), lostSeqs.end(), out[i].seq)) continue;
                Placement p;
                p.result       = out[i].staged ? Result::Fallback : Result::Queued;
                p.slot         = slot;
                p.offset       = out[i].offset;
                p.needsStaging = out[i].staged;
                p.copy         = out[i].gpuSource;
                p.index        = i;
                reclaimed.push_back(p);
            }
            m_submitted[slot] = out.empty();
            return &out;
        }

        // EndFrame submitted the frame recorded on @p slot: the copies will run,
        // so MustDeliver no longer needs its bytes.
        void Submitted(uint32_t slot)
        {
            m_submitted[slot] = true;
            for (ItemT& it : m_recorded[slot]) DropBytes(it);
        }

        const std::vector<ItemT>& Recorded(uint32_t slot) const { return m_recorded[slot]; }

        // The caller is done with @p it's CPU copy (a LatestWins it placed).
        void DropBytes(ItemT& it)
        {
            Release(it.bytes.size());
            it.bytes.clear();
            it.bytes.shrink_to_fit();
        }

        void CountRejected() { ++m_stats.requests; ++m_stats.rejected; }
        // The cap refused a request's CPU copy (CanRetain): Admit then drops a
        // carry-over, or delivers a MustDeliver without it.
        void CountCapped() { ++m_stats.capped; }

        // Every CPU copy summed from the items - RetainedBytes() must equal it.
        uint64_t RecountRetained() const
        {
            uint64_t n = 0;
            for (const ItemT& it : m_carry) n += it.bytes.size();
            for (const auto& v : m_pending)  for (const ItemT& it : v) n += it.bytes.size();
            for (const auto& v : m_recorded) for (const ItemT& it : v) n += it.bytes.size();
            return n;
        }

        // The texture (@p texture, @p generation) is being destroyed: its
        // requests that no Take has handed out yet are dropped - every slot's
        // pending list and the carry-over. Recorded lists are left alone: one
        // may be recording right now, and its copies only write into a
        // resource the device keeps alive through the release delay.
        // @p released receives the dropped items (their staging can go).
        size_t Forget(uint32_t texture, uint32_t generation, std::vector<ItemT>& released)
        {
            auto drop = [&](std::vector<ItemT>& v)
            {
                size_t n = 0;
                for (size_t i = 0; i < v.size();)
                {
                    if (v[i].key.texture == texture && v[i].key.generation == generation)
                    {
                        Release(v[i].bytes.size());
                        released.push_back(std::move(v[i]));
                        v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
                        ++n;
                    }
                    else ++i;
                }
                return n;
            };
            size_t n = drop(m_carry);
            for (std::vector<ItemT>& p : m_pending) n += drop(p);
            m_stats.forgotten += n;
            return n;
        }

    private:
        // A GPU-to-GPU copy (Item::gpuSource): the window's rules without bytes
        // or ring space. A LatestWins copy replaces the earlier copy for its key
        // in the same list - never a byte upload, whose place it does not share.
        Placement AdmitCopy(ItemT item)
        {
            Placement p;
            item.staged = false;
            item.offset = 0;
            item.bytes.clear();
            const bool carry = !AcceptsOpenFrame();
            std::vector<ItemT>* list = &m_carry;
            if (!carry)
            {
                item.frame = m_openFrame;
                p.slot = m_openSlot;
                list = &m_pending[m_openSlot];
            }
            if (item.policy == Policy::LatestWins)
                for (size_t i = 0; i < list->size(); ++i)
                {
                    ItemT& old = (*list)[i];
                    if (!old.gpuSource || old.policy != Policy::LatestWins || !(old.key == item.key)) continue;
                    old = std::move(item);   // its new seq: recorded where this request was made
                    if (carry) { ++m_stats.carriedOver; p.result = Result::CarriedOver; }
                    else       { ++m_stats.replaced;    p.result = Result::Replaced; }
                    p.index = i;
                    return p;
                }
            list->push_back(std::move(item));
            if (carry) { ++m_stats.carriedOver; p.result = Result::CarriedOver; }
            else       { ++m_stats.queued;      p.result = Result::Queued; }
            p.index = list->size() - 1;
            return p;
        }

        void Retain(uint64_t n)
        {
            m_retained += n;
            m_retainedHigh = (std::max)(m_retainedHigh, m_retained);
        }
        void Release(uint64_t n) { m_retained -= (std::min)(n, m_retained); }

        // The items of a frame that will never reach the GPU: MustDeliver with
        // its CPU copy - or a GPU copy, whose source its payload still holds -
        // goes again (a COPY into @p again, its seq kept; the original stays in
        // @p from for ReleaseAll), the rest is counted lost.
        void Salvage(const std::vector<ItemT>& from, std::vector<ItemT>& again)
        {
            for (const ItemT& it : from)
            {
                if (it.policy == Policy::MustDeliver && (it.gpuSource || !it.bytes.empty()))
                {
                    again.push_back(it);
                    ++m_stats.requeued;
                }
                else ++m_stats.lostDropped;
            }
        }

        // Every item of @p v to the caller's @p released (its staging goes
        // there, after the lock), its CPU copy uncounted; @p v ends empty.
        void ReleaseAll(std::vector<ItemT>& v, std::vector<ItemT>& released)
        {
            for (ItemT& it : v)
            {
                Release(it.bytes.size());
                released.push_back(std::move(it));
            }
            v.clear();
        }

        // A carried or requeued item into the just-opened slot.
        Placement Place(ItemT item, uint32_t slot)
        {
            Placement p;
            p.slot = slot;
            item.frame = m_openFrame;
            std::vector<ItemT>& pend = m_pending[slot];
            if (item.gpuSource)
            {
                // Nothing to place: the caller skips it (needsStaging false, no bytes).
                pend.push_back(std::move(item));
                p.result = Result::Queued;
                p.index  = pend.size() - 1;
                p.copy   = true;
                return p;
            }
            if (std::optional<uint64_t> at = m_rings[slot].Allocate(item.size))
            {
                item.offset = *at;
                item.staged = false;
                Retain(item.bytes.size());
                pend.push_back(std::move(item));
                p.result = Result::Queued;
                p.offset = pend.back().offset;
                p.index  = pend.size() - 1;
                return p;
            }
            if (item.policy == Policy::LatestWins)
            {
                ++m_stats.dropped;
                p.result = Result::Dropped;
                return p;
            }
            item.staged = true;
            item.offset = 0;
            Retain(item.bytes.size());
            pend.push_back(std::move(item));
            ++m_stats.fallback;
            p.result       = Result::Fallback;
            p.needsStaging = true;
            p.index        = pend.size() - 1;
            return p;
        }

        std::vector<Ring>               m_rings;
        std::vector<std::vector<ItemT>> m_pending;    // per slot: what its next Take records
        std::vector<std::vector<ItemT>> m_recorded;   // per slot: what its last Take recorded
        std::vector<bool>               m_submitted;  // per slot: m_recorded's frame was submitted
        std::vector<ItemT>              m_carry;
        bool     m_open = false, m_consumed = false;
        uint64_t m_openFrame = 0, m_consumedFrame = 0;
        uint32_t m_openSlot = 0;
        uint64_t m_seq = 0;
        uint64_t m_retained = 0, m_retainedHigh = 0;
        uint64_t m_retainCap = (std::numeric_limits<uint64_t>::max)();
        Counters m_stats;
    };
}
