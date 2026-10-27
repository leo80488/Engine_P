#pragma once

// Graphics/PsoManifest.h - the pipelines a game builds, recorded so that a package can build
// every one of them before the title screen.
//
// A record holds everything the driver needs to build one pipeline - its native desc, field by
// field, its root signature's serialized blob and its shaders' DXIL - and the PSO library name the
// device files it under. Replayed in another process on the same content, a record builds the
// pipeline the game's own create would and stores it under the same name, so the game's first use
// of it is a library hit, not a driver compile.
//
// File (little-endian):
//   "IPSM" | u32 version (1) | u32 blob count | u32 record count
//   per blob:   u64 hash | u32 size | bytes                 (DXIL and root signatures, once each)
//   per record: u32 size of the rest | u8 kind | u8 pad[3] | u32 estimated build us
//               | u32 name length (UTF-16 units) | name | u64 root signature blob
//               | u64 blob per stage (StageSlot order; 0 = none) | the kind's state (Graphics, Mesh)
// A record is length-prefixed, so a reader that does not know a kind's state skips it.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d12.h"

namespace Gfx::PsoManifest
{
    enum class Kind : std::uint8_t { Graphics = 0, Mesh = 1, Compute = 2 };
    enum StageSlot : std::uint32_t { kVS, kPS, kGS, kHS, kDS, kAS, kMS, kCS, kStageCount };

    // One input-layout element, its semantic name owned.
    struct InputElement
    {
        std::string   semanticName;
        std::uint32_t semanticIndex = 0;
        std::uint32_t format = 0;
        std::uint32_t inputSlot = 0;
        std::uint32_t alignedByteOffset = 0;
        std::uint32_t inputSlotClass = 0;
        std::uint32_t instanceDataStepRate = 0;
    };

    // The fields of a graphics or mesh pipeline that are not shaders or the root signature: what
    // D3D12_GRAPHICS_PIPELINE_STATE_DESC holds, without its pointers.
    struct State
    {
        D3D12_BLEND_DESC                      blend{};
        std::uint32_t                         sampleMask = 0;
        D3D12_RASTERIZER_DESC                 raster{};
        D3D12_DEPTH_STENCIL_DESC              depthStencil{};
        std::vector<InputElement>             inputElements;
        std::uint32_t                         ibStripCut = 0;
        std::uint32_t                         topologyType = 0;
        std::uint32_t                         numRenderTargets = 0;
        std::uint32_t                         rtvFormats[8] = {};
        std::uint32_t                         dsvFormat = 0;
        std::uint32_t                         sampleCount = 1;
        std::uint32_t                         sampleQuality = 0;
        std::uint32_t                         nodeMask = 0;
        std::uint32_t                         flags = 0;
    };

    struct Record
    {
        Kind          kind = Kind::Graphics;
        std::uint32_t estimatedUs = 0;   // the build's time where it was recorded; orders the replay
        std::wstring  name;              // the PSO library name
        std::uint64_t rootSignature = 0;
        std::uint64_t stages[kStageCount] = {};
        State         state;             // Graphics and Mesh
    };

    struct Manifest
    {
        std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> blobs;
        std::vector<Record>                                          records;
    };

    // The blob table's key: FNV-1a over the bytes.
    std::uint64_t HashBlob(const void* data, std::size_t size);

    // The state of a graphics desc (the input layout's names copied).
    State StateOf(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
    // @p desc's state fields from @p state. The input layout is built in @p elements, whose names
    // point into @p state: both must outlive the desc's use. Shaders and the root signature are the
    // caller's.
    void ApplyState(const State& state, D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                    std::vector<D3D12_INPUT_ELEMENT_DESC>& elements);

    // The file's bytes, and back. Parse: false on a malformed file (@p error says where).
    std::vector<std::uint8_t> Serialize(const Manifest& manifest);
    bool Parse(const std::uint8_t* data, std::size_t size, Manifest& out, std::string& error);
    // A short identity of the content (the records' names and the blobs' hashes), for "is this the
    // manifest the cache was prepared from".
    std::uint64_t ContentHash(const Manifest& manifest);

    // ---- Recording ----
    // Off unless Enable was called (the harness's -pso_variants). Thread-safe: the device records from
    // every thread that builds a pipeline. A name already recorded is skipped.
    void EnableRecording();
    bool RecordingEnabled();
    // @p blobs: (hash, bytes) of the root signature and every stage the record names.
    void AddRecord(const Record& record,
                   const std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>>& blobs);
    // Whether @p name is recorded already: the device skips copying the blobs again.
    bool IsRecorded(const std::wstring& name);
    // What has been recorded so far (a copy).
    Manifest Recorded();
}
