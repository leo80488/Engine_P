// debug_wire_common.hlsli - the constant layout of the debug-line pass.
//
// DebugWirePass owns this CB (FrameCB<WireCB>); it is not the engine PerView CB even though it
// lands on the same b1 space0 root slot. The pass runs after tonemap/upscale, so it needs the
// un-jittered viewProj plus the two viewport sizes the manual depth test needs - the PerView CB
// carries neither in a usable form at that point in the frame.
//
// Matrix convention: the CPU uploads XMMatrixTranspose(VP) so HLSL's default column-major cbuffer
// packing reconstitutes VP, and the VS uses the engine's row-vector form
// `mul(float4(pos,1), viewProj)`.

#ifndef DEBUG_WIRE_COMMON_HLSLI
#define DEBUG_WIRE_COMMON_HLSLI

cbuffer DebugWireCB : register(b1, space0)
{
    float4x4 g_viewProj;
    // xy = render-target size in pixels (display res after upscale),
    // zw = scene-depth texture size in pixels (render res); zw = 0 -> no depth texture bound this
    // frame, occlusion test disabled.
    float4   g_targetSize;
    // x = alpha multiplier applied to occluded pixels (x-ray),
    // y = relative depth bias, zw = unused.
    float4   g_params;
};

struct DebugWireVSOut
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
};

#endif // DEBUG_WIRE_COMMON_HLSLI
