#pragma once

// IndirectDrawCommand - one ExecuteIndirect command, matching the engine's command signature:
//   [0-3] 4 root constants (root parameter 0, b0 space0): meshDescIdx, instanceOffset,
//         materialIndex, prevPosInfo
//   [4-7] D3D12_DRAW_ARGUMENTS
// 8 x uint32 = 32 bytes per command (the signature's ByteStride).

#include <cstdint>

struct IndirectDrawCommand
{
    // Root constants: must match root parameter 0's layout.
    uint32_t meshDescIdx;
    uint32_t instanceOffset;
    uint32_t materialIndex;
    uint32_t prevPosInfo;

    // D3D12_DRAW_ARGUMENTS
    uint32_t vertexCountPerInstance;
    uint32_t instanceCount;
    uint32_t startVertexLocation;
    uint32_t startInstanceLocation;
};

static_assert(sizeof(IndirectDrawCommand) == 32, "IndirectDrawCommand must be 32 bytes");
