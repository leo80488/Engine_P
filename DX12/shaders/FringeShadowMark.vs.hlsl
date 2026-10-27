// FringeShadowMark.vs.hlsl - bangs -> face SCREEN-SPACE shadow-mark proxy VS.
//
// Port of DanbaidongRP's CharacterFringe helper ("GBufferBase" pass): the bangs
// mesh is drawn a SECOND time, offset in view space so its silhouette lands where
// the fringe's shadow should fall on the face, with its depth pushed slightly AWAY
// from the camera so the reversed-Z GREATER_EQUAL test clips the proxy against the
// rendered face / scene depth. The mark lands only where the pushed proxy still
// floats in front - the face right behind the fringe; it fails on nearer geometry,
// and the far background is stencil-gated out.
//
// Reference math (DanbaidongRP CharacterFringe.shader, view space):
//     positionVS.x -= 0.0045 * lightDirVS.x * _ScreenOffsetScaleX;
//     positionVS.y -= 0.0075 * _ScreenOffsetScaleY * camDirFactor;
//     scale = (positionVS.z + 0.01 * _CamViewDirWorldOffsetScale) / positionVS.z;
//     positionVS.xyz *= scale;              // depth push, screen position kept
// Here the CPU (Renderer::UploadFrameData) precomputes the offsets in view-space
// METRES, applied in world space along the camera's right/up axes; the depth push
// moves the vertex along the camera ray (the same as the view-space uniform scale:
// the screen position is unchanged).
//
// Skinned bangs work for free: FETCH_POS reads the same per-frame skinned vertex
// stream the GBuffer VS reads.

#include "pvf_fetch.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor>  MeshDescriptors : register(t1, space0);

// Per-frame proxy params, uploaded by HairShadowMarkPass (triple-buffered).
cbuffer FringeMarkCB : register(b1, space0)
{
    float4x4 viewProj;      // JITTERED camera VP (matches the GBuffer depth)
    float3   camRight;      // world-space camera right (view-space +X)
    float    offsetX;       // view-space X shift, metres (CPU: -0.0045 x lightDirVS.x x scaleX)
    float3   camUp;         // world-space camera up (view-space +Y)
    float    offsetY;       // view-space Y shift, metres (CPU: -0.0075 x scaleY x camDirFactor)
    float3   camPos;        // world-space camera position
    float    depthPush;     // metres pushed along the camera ray (CPU: 0.01 x pushScale)
    float3   camFwd;        // world-space camera forward
    uint     dissolveNoiseIdx; // dissolve noise bindless index (~0u = none)
    uint     gInstanceRemapIdx; // 128 stable-slot remap heap index (0 = identity)
    uint3    _fmPad;            // 132..143
};

// MUST match FringeShadowMark.ps.hlsl PSIn: uv + instanceSlot feed the
// per-instance dissolve clip (dissolving bangs stop stamping face marks).
struct VSOut
{
    float4 sv : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation uint instanceSlot : INSTSLOT;
};

VSOut main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md = MeshDescriptors[meshDescIdx];
    uint vid = FetchIndex(md, rawID);

    float3   localPos = FETCH_POS(md, vid);
    float2   uv0      = FETCH_UV0(md, vid);
    uint     instSlot = ResolveInstanceSlot(gInstanceRemapIdx, instanceOffset + instID);
    float4x4 world    = InstanceBuffer[instSlot].world;
    float4   wPos     = mul(float4(localPos, 1.0f), world);

    // GPU-TDR safety net (the reason is at GBufferVertex in gbuffer_vertex.hlsli).
    if (!(dot(wPos.xyz, wPos.xyz) < 1e10f))
        wPos = float4(world[3][0], world[3][1], world[3][2], 1.0f);

    // View-space XY shift (constant metres, like the reference).
    wPos.xyz += camRight * offsetX + camUp * offsetY;

    // Depth push along the camera ray: screen position unchanged, depth moved away
    // from the camera by `depthPush` metres (reversed-Z: a smaller value), so the mark
    // cannot land on the fringe's own roots / the surfaces it hugs.
    //
    // Only push vertices safely IN FRONT of the camera. For viewZ <= 0 (bangs beside
    // or behind the camera plane, e.g. a second character next to the camera) the push
    // scale (viewZ + depthPush) / viewZ is hugely NEGATIVE and flings vertices across
    // clip space; the screen-spanning triangles would stamp the hair-shadow flag over
    // every NPR pixel in view and black out whole faces. Un-pushed near-plane vertices
    // just clip.
    float viewZ = dot(wPos.xyz - camPos, camFwd);
    if (viewZ > 0.05f)
    {
        float scale = (viewZ + depthPush) / viewZ;
        wPos.xyz = camPos + (wPos.xyz - camPos) * scale;
    }

    VSOut o;
    o.sv = mul(wPos, viewProj);
    o.uv = uv0;
    o.instanceSlot = instSlot;   // remapped - the PS dissolve clip reads it
    return o;
}
