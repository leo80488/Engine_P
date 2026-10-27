#pragma once
// Graphics/FrameSnapshot.h - one device frame as plain data.
//
// The render graph is one linear sub-chain of the frame; the frame's real topology
// is the device's: every command list recorded, the dependencies callers declared
// (AddCommandListDependency), the order EndFrame's Kahn sort submitted them in,
// the ExecuteCommandLists batches and the cross-queue fence waits.
// RG::GraphSnapshot sees none of that, nor any barrier recorded outside Phase A.
// This records:
//   - the command-list table in pool order: each list's name (SetCommandListName,
//     kept on the CPU for every list), queue, declared waits, submission position
//     and batch;
//   - the barrier journal: every barrier the frame recorded, through PushBarrier /
//     PushBarriers or straight on the D3D12 list and reported with
//     NoteNativeBarriers, with the recording list, the resource, the states and
//     the subresource (what DX12_DEPTHTRACE in Graphics/DepthTrace.h logs for
//     GBuffer_Depth, for every resource and as data).
// RG::FrameCapture (RenderGraph/FrameCapture.h) joins it with the main graph's
// snapshot of the same frame and derives the observed edges.
//
// On request only: the device journals a frame only when armed before that frame's
// BeginFrame (IGraphicsDevice::ArmFrameJournal); otherwise a barrier costs one
// relaxed atomic load and a command list one pointer store.
//
// States are D3D12_RESOURCE_STATES bits (what the GPU was told, the numbers a
// debug-layer message quotes); subresources are D3D12 subresource indices.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct ID3D12GraphicsCommandList;
struct D3D12_RESOURCE_BARRIER;

namespace GfxDiag
{
    struct FrameSnapshot
    {
        // D3D12_RESOURCE_BARRIER_TYPE, as recorded.
        enum class Kind : uint8_t { Transition, Aliasing, UAV };
        // Device: PushBarrier / PushBarriers. Native: a site's own
        // ResourceBarrier call, reported through NoteNativeBarriers.
        enum class Path : uint8_t { Device, Native };

        static constexpr uint32_t kNone = ~0u;   // no resource; all subresources; not submitted

        struct Resource
        {
            std::string name;          // the D3D12 object's debug name, "" = unnamed (buffers are)
            uint64_t    address = 0;   // the ID3D12Resource* - identity within this frame only
            bool        buffer  = false;
        };
        struct Barrier
        {
            uint32_t cl            = 0;      // pool slot of the list that recorded it
            uint32_t resource      = kNone;  // index into resources; kNone = the global UAV barrier
            uint32_t resourceAfter = kNone;  // Aliasing: the resource that becomes active
            uint32_t before        = 0;      // Transition: D3D12_RESOURCE_STATES
            uint32_t after         = 0;
            uint32_t subresource   = kNone;  // kNone = all subresources
            Kind     kind          = Kind::Transition;
            Path     path          = Path::Device;
            uint8_t  flags         = 0;      // D3D12_RESOURCE_BARRIER_FLAGS (split begin / end)
        };
        // A queue Wait() EndFrame issued before this list's batch went out.
        struct Wait
        {
            uint32_t on    = 0;   // the dependency's pool slot
            uint8_t  queue = 0;   // the queue it ran on (RHI::QueueType)
            uint64_t value = 0;   // the fence value waited for
        };
        struct CommandList
        {
            uint32_t              slot         = 0;      // pool slot: "cmd<slot>"
            std::string           name;                  // SetCommandListName's name, "" = never named
            uint8_t               queue        = 0;      // RHI::QueueType
            std::vector<uint32_t> waitFor;               // DECLARED: slots that must run first
            uint32_t              submitIndex  = kNone;  // position in submitOrder
            uint32_t              batch        = kNone;  // index into batches
            std::vector<Wait>     waits;                 // cross-queue waits issued for it
            uint32_t              firstBarrier = 0;      // its run in journal
            uint32_t              barrierCount = 0;
        };
        // One ExecuteCommandLists call and the fence Signal after it.
        struct Batch
        {
            uint8_t  queue       = 0;
            uint32_t firstSubmit = 0;   // its run in submitOrder
            uint32_t count       = 0;
            uint64_t signalValue = 0;   // the queue fence value it signalled
        };

        uint64_t                 frame = 0;      // the device's render frame
        std::vector<CommandList> commandLists;   // pool order: commandLists[s].slot == s
        std::vector<uint32_t>    submitOrder;    // pool slots in the order EndFrame submitted them
        std::vector<Batch>       batches;        // in submission order
        // Grouped by command list in SUBMISSION order, recording order within
        // a list - the order the GPU meets them in.
        std::vector<Barrier>     journal;
        std::vector<Resource>    resources;

        uint32_t NativeCount() const
        {
            uint32_t n = 0;
            for (const Barrier& b : journal) n += b.path == Path::Native ? 1u : 0u;
            return n;
        }

        // The names the -rgdump JSON, the `rg` verb and the editor's Render Graph panel
        // print.
        static const char* QueueName(uint8_t q)   // RHI::QueueType
        {
            switch (q)
            {
            case 0: return "graphics";
            case 1: return "compute";
            case 2: return "copy";
            }
            return "?";
        }
        static const char* KindName(Kind k)
        {
            switch (k)
            {
            case Kind::Transition: return "transition";
            case Kind::Aliasing:   return "aliasing";
            case Kind::UAV:        return "uav";
            }
            return "?";
        }
        // D3D12_RESOURCE_STATES as the debug layer spells them, without the
        // D3D12_RESOURCE_STATE_ prefix, OR-ed parts joined by '|'. 0 is COMMON
        // (which is also PRESENT). Unknown bits print as hex.
        static std::string D3DStateName(uint32_t s)
        {
            if (s == 0) return "COMMON";
            static const struct { uint32_t bit; const char* name; } kBits[] = {
                { 0x1,       "VERTEX_AND_CONSTANT_BUFFER" },
                { 0x2,       "INDEX_BUFFER" },
                { 0x4,       "RENDER_TARGET" },
                { 0x8,       "UNORDERED_ACCESS" },
                { 0x10,      "DEPTH_WRITE" },
                { 0x20,      "DEPTH_READ" },
                { 0x40,      "NON_PIXEL_SHADER_RESOURCE" },
                { 0x80,      "PIXEL_SHADER_RESOURCE" },
                { 0x100,     "STREAM_OUT" },
                { 0x200,     "INDIRECT_ARGUMENT" },
                { 0x400,     "COPY_DEST" },
                { 0x800,     "COPY_SOURCE" },
                { 0x1000,    "RESOLVE_DEST" },
                { 0x2000,    "RESOLVE_SOURCE" },
                { 0x10000,   "VIDEO_DECODE_READ" },
                { 0x20000,   "VIDEO_DECODE_WRITE" },
                { 0x40000,   "VIDEO_PROCESS_READ" },
                { 0x80000,   "VIDEO_PROCESS_WRITE" },
                { 0x200000,  "VIDEO_ENCODE_READ" },
                { 0x400000,  "RAYTRACING_ACCELERATION_STRUCTURE" },
                { 0x800000,  "VIDEO_ENCODE_WRITE" },
                { 0x1000000, "SHADING_RATE_SOURCE" },
            };
            std::string out;
            uint32_t rest = s;
            for (const auto& b : kBits)
            {
                if ((s & b.bit) == 0) continue;
                if (!out.empty()) out += '|';
                out += b.name;
                rest &= ~b.bit;
            }
            if (rest)
            {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "0x%X", rest);
                if (!out.empty()) out += '|';
                out += buf;
            }
            return out;
        }
    };

    // Report barriers a site issued straight on a D3D12 command list, not
    // through PushBarrier / PushBarriers, so a journaled frame sees them too.
    // Call it right after the list->ResourceBarrier(...) it describes, on the
    // thread recording @p list. While no frame is journaled this is one relaxed
    // atomic load. A list that is not one of the device's pooled command lists
    // (an upload context, the video decoder's) is ignored.
    void NoteNativeBarriers(ID3D12GraphicsCommandList* list,
                            const D3D12_RESOURCE_BARRIER* barriers, uint32_t count);
}
