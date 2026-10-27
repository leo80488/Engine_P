// Skin.cs.hlsl - GPU skinning compute shader (dual quaternion skinning) with morph targets.
// Shared compute root signature (slots in RHI::Slots::Cmp, Graphics/RootSignatureLayout.h):
// b0 = SkinJobDesc (root CBV), t0-t6 inputs, u0-u2 outputs, all space2.
// [numthreads(64,1,1)], ceil(vertexCount/64) groups, one thread per vertex.
// DQS preserves volume and avoids linear blend skinning's candy-wrapper collapse: each bone's
// rigid transform becomes a dual quaternion, blended in DQ space, then applied to the vertex.

// ENGINE_MAX_MORPH_TARGETS: the morph cap, shared verbatim with C++ (MorphComponent::MAX /
// SkeletonAsset::MAX_MORPH_TARGETS). Do not copy the literal here: a hand-copied cap drifts.
#include "morph_limits.hlsli"

// ==== Per-job descriptor, one per skinned mesh ====
struct SkinJobDesc
{
    uint  poseByteOffset;         // byte offset into g_Pose for this entity's bones
    uint  vertexCount;
    uint  outPosByteOffset;       // byte offset into g_OutPos for this entity's output
    uint  outNrmByteOffset;       // byte offset into g_OutNrm for this entity's output
    uint  morphCount;             // ACTIVE morph targets: (local target, weight) pairs to walk (0 = none)
    uint  morphWeightByteOffset;  // byte offset into g_MorphWeights of those pairs
    uint  _reservedPrevPosSrc;    // reserved: keeps SkinJobDesc at 48 B (no previous-position output)
    uint  _reservedOutPrevPos;
    float charExtent;             // per-skeleton max |restPos|; vertices clamp to ~4x this (TDR guard; 0 = off)
    uint  morphNrmElemOffset;     // g_MorphDeltas element where the NORMAL deltas start (0 = none)
    uint  _reservedPrevMorphWeightByteOffset;
    uint  outTanByteOffset;       // byte offset into g_OutTan (0xFFFFFFFF = mesh has no tangent stream)
};

// Per-job data: the root CBV points at the current job's slice of the upload buffer.
ConstantBuffer<SkinJobDesc> g_Job : register(b0, space2);

// ==== SRV inputs ====
StructuredBuffer<float3>   g_RestPos      : register(t0, space2); // rest-pose positions
StructuredBuffer<float3>   g_RestNrm      : register(t1, space2); // rest-pose normals
ByteAddressBuffer          g_Blend        : register(t2, space2); // BlendVertex (24 B/vertex)
ByteAddressBuffer          g_Pose         : register(t3, space2); // float4x4 bone matrices
StructuredBuffer<float3>   g_MorphDeltas  : register(t4, space2); // morph vertex deltas (then normal deltas)
ByteAddressBuffer          g_MorphWeights : register(t5, space2); // packed per-job { uint local; float w }[morphCount] regions
StructuredBuffer<float4>   g_RestTan      : register(t6, space2); // rest-pose tangents (xyz dir, w handedness)

// ==== UAV outputs ====
RWStructuredBuffer<float3>  g_OutPos : register(u0, space2); // skinned positions
RWStructuredBuffer<float3>  g_OutNrm : register(u1, space2); // skinned normals
// Skinned tangents. Pose the tangent with the normal: left at bind pose, GBuffer.vs's Gram-Schmidt
// keeps T in the right plane but its azimuth inside that plane stays off by the bone rotation,
// rotating the whole tangent frame and corrupting every tangent-space decode (normal maps, and
// anisotropic highlights, which are built from T directly).
RWStructuredBuffer<float4>  g_OutTan : register(u2, space2); // skinned tangents

// Load a row-major float4x4 from g_Pose at byteOff.
float4x4 LoadBoneMat(uint byteOff)
{
    float4 r0 = asfloat(g_Pose.Load4(byteOff +  0));
    float4 r1 = asfloat(g_Pose.Load4(byteOff + 16));
    float4 r2 = asfloat(g_Pose.Load4(byteOff + 32));
    float4 r3 = asfloat(g_Pose.Load4(byteOff + 48));
    return float4x4(r0, r1, r2, r3);
}

// ==== Dual quaternion helpers ====
// DQ = (q_real, q_dual): q_real = unit rotation quaternion, q_dual = 0.5 * translation_quat *
// q_real; stored as two float4 (x, y, z, w).

struct DualQuat
{
    float4 real;  // rotation part (unit quaternion)
    float4 dual;  // translation part
};

// Quaternion multiply a * b (Hamilton product).
float4 QMul(float4 a, float4 b)
{
    return float4(
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    );
}

// Convert a row-major, row-vector 4x4 matrix to a dual quaternion. The matrix must be rigid
// (rotation + translation, no scale); v' = v * M, translation in row 3 (M[3][0..2]).
DualQuat MatToDQ(float4x4 m)
{
    DualQuat dq;

    // Rotation quaternion from the upper-left 3x3; rows are the basis vectors.
    float3x3 rot = float3x3(m[0].xyz, m[1].xyz, m[2].xyz);

    // Shepperd's method for robust quaternion extraction.
    float tr = rot[0][0] + rot[1][1] + rot[2][2];
    float4 q;

    if (tr > 0.0f)
    {
        float s = sqrt(tr + 1.0f) * 2.0f; // s = 4*w
        q.w = 0.25f * s;
        q.x = (rot[1][2] - rot[2][1]) / s;
        q.y = (rot[2][0] - rot[0][2]) / s;
        q.z = (rot[0][1] - rot[1][0]) / s;
    }
    else if (rot[0][0] > rot[1][1] && rot[0][0] > rot[2][2])
    {
        float s = sqrt(1.0f + rot[0][0] - rot[1][1] - rot[2][2]) * 2.0f;
        q.w = (rot[1][2] - rot[2][1]) / s;
        q.x = 0.25f * s;
        q.y = (rot[1][0] + rot[0][1]) / s;
        q.z = (rot[2][0] + rot[0][2]) / s;
    }
    else if (rot[1][1] > rot[2][2])
    {
        float s = sqrt(1.0f + rot[1][1] - rot[0][0] - rot[2][2]) * 2.0f;
        q.w = (rot[2][0] - rot[0][2]) / s;
        q.x = (rot[1][0] + rot[0][1]) / s;
        q.y = 0.25f * s;
        q.z = (rot[2][1] + rot[1][2]) / s;
    }
    else
    {
        float s = sqrt(1.0f + rot[2][2] - rot[0][0] - rot[1][1]) * 2.0f;
        q.w = (rot[0][1] - rot[1][0]) / s;
        q.x = (rot[2][0] + rot[0][2]) / s;
        q.y = (rot[2][1] + rot[1][2]) / s;
        q.z = 0.25f * s;
    }

    q = normalize(q);
    dq.real = q;

    // Dual part: d = 0.5 * t_quat * q_real, t_quat = (tx, ty, tz, 0) from row 3.
    float3 t = m[3].xyz;
    float4 tq = float4(t, 0.0f);
    dq.dual = QMul(tq, q) * 0.5f;

    return dq;
}

// Normalize a blended dual quaternion.
DualQuat NormalizeDQ(DualQuat dq)
{
    float len = length(dq.real);
    if (len < 1e-8f) len = 1.0f;
    float invLen = 1.0f / len;
    dq.real *= invLen;
    dq.dual *= invLen;
    return dq;
}

// Transform a position by a dual quaternion.
// p' = q * p * q* + 2 * d * q*   (simplified form)
float3 DQTransformPoint(DualQuat dq, float3 p)
{
    // Translation: t = 2 * d * q* (vector part).
    float4 q = dq.real;
    float3 t_extract = 2.0f * (q.w * dq.dual.xyz - dq.dual.w * q.xyz
                                + cross(q.xyz, dq.dual.xyz));

    // Rotate position
    float3 rotated = p + 2.0f * cross(q.xyz, cross(q.xyz, p) + q.w * p);

    return rotated + t_extract;
}

// Transform a normal/direction by a dual quaternion (rotation only).
float3 DQTransformNormal(DualQuat dq, float3 n)
{
    float4 q = dq.real;
    return n + 2.0f * cross(q.xyz, cross(q.xyz, n) + q.w * n);
}

[numthreads(64, 1, 1)]
void SkinCS(uint3 tid : SV_DispatchThreadID)
{
    const uint v = tid.x;
    if (v >= g_Job.vertexCount) return;

    // --- Indices into this entity's output slices ---
    const uint outPosIdx = g_Job.outPosByteOffset / 12u + v;
    const uint outNrmIdx = g_Job.outNrmByteOffset / 12u + v;
    const bool hasTangent = (g_Job.outTanByteOffset != 0xFFFFFFFFu);
    const uint outTanIdx  = hasTangent ? (g_Job.outTanByteOffset / 16u + v) : 0u;

    // --- Blend data, 24 bytes per vertex ---
    // uint16 boneIndices[8] (16 bytes) + uint8 weights[8] (8 bytes), loaded as 6 x uint32.
    const uint baseOff = v * 24u;
    const uint4 raw0 = g_Blend.Load4(baseOff);       // boneIdx[0..7] as 4 uint32s (16 bytes)
    const uint2 raw1 = g_Blend.Load2(baseOff + 16u);  // weights[0..7] as 2 uint32s (8 bytes)

    uint  bIdx[8];
    float bWgt[8];
    // Unpack uint16 bone indices (2 per uint32)
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        uint packed = (k < 2) ? ((k == 0) ? raw0.x : raw0.y) : ((k == 2) ? raw0.z : raw0.w);
        bIdx[k * 2]     = packed & 0xFFFFu;
        bIdx[k * 2 + 1] = (packed >> 16) & 0xFFFFu;
    }
    // Unpack uint8 weights (4 per uint32)
    [unroll] for (uint j = 0; j < 4; ++j)
    {
        bWgt[j]     = float((raw1.x >> (j * 8)) & 0xFFu) * (1.0f / 255.0f);
        bWgt[j + 4] = float((raw1.y >> (j * 8)) & 0xFFu) * (1.0f / 255.0f);
    }

    // --- Load rest-pose data + apply morph deltas ---
    float3 restPos = g_RestPos[v];
    float3 restNrm = g_RestNrm[v];
    float4 restTan = hasTangent ? g_RestTan[v] : float4(1, 0, 0, 1);

    // Morph deltas: restPos += sum over the job's ACTIVE targets of weight[m] * delta[m][v].
    // g_MorphDeltas: [localTarget * vertexCount + vertexIdx]; normal deltas (.imorph v2) repeat the
    // layout from morphNrmElemOffset. g_MorphWeights: this job's region starts at
    // morphWeightByteOffset and is EXACTLY morphCount (uint local target, float weight) pairs
    // (rounded up for alignment) - the weights that move something, chosen per job on the CPU
    // (SkinningPass). Regions are packed back to back, so the next region belongs to another mesh.
    if (g_Job.morphCount > 0)
    {
        const uint vc     = g_Job.vertexCount;
        const uint wOff   = g_Job.morphWeightByteOffset;
        const uint nrmOff = g_Job.morphNrmElemOffset;
        float3     nrmSum = 0;
        // Looping past the region walks into the NEXT job's pairs (or off the upload buffer): garbage
        // that explodes the vertex into a screen-spanning triangle and hangs the GPU (TDR). SkinningPass
        // already writes a morphCount it has room for; this is the backstop, on the SHARED cap.
        const uint mc = min(g_Job.morphCount, ENGINE_MAX_MORPH_TARGETS);
        [loop] for (uint i = 0; i < mc; ++i)
        {
            const uint2 lw = g_MorphWeights.Load2(wOff + i * 8u);
            const uint  m  = lw.x;
            const float w  = asfloat(lw.y);
            restPos += g_MorphDeltas[m * vc + v] * w;
            if (nrmOff != 0u)
                nrmSum += g_MorphDeltas[nrmOff + m * vc + v] * w;
        }

        // The shape's own shading: the rest normal plus the weighted change of the unit normal,
        // renormalised (exact for one target at weight 1). The tangent is re-orthogonalised to it
        // (Gram-Schmidt), handedness kept, so tangent-space normal maps decode against the morphed surface.
        if (any(nrmSum != 0.0f))
        {
            const float3 n  = restNrm + nrmSum;
            const float  n2 = dot(n, n);
            if (n2 > 1e-12f)
            {
                restNrm = n * rsqrt(n2);
                if (hasTangent)
                {
                    const float3 t  = restTan.xyz - restNrm * dot(restNrm, restTan.xyz);
                    const float  t2 = dot(t, t);
                    if (t2 > 1e-12f) restTan.xyz = t * rsqrt(t2);
                }
            }
        }
    }

    // --- Load up to 8 bone matrices, convert to dual quaternions, blend ---
    const uint pBase = g_Job.poseByteOffset;

    // Slot 0 seeds the blend and is the reference hemisphere for the antipodality fix.
    DualQuat dq0 = MatToDQ(LoadBoneMat(pBase + bIdx[0] * 64u));

    DualQuat blended;
    blended.real = dq0.real * bWgt[0];
    blended.dual = dq0.dual * bWgt[0];

    // Accumulate the remaining bones with the antipodality fix.
    [unroll] for (uint i = 1; i < 8; ++i)
    {
        if (bWgt[i] > 0.0f)
        {
            DualQuat dqi = MatToDQ(LoadBoneMat(pBase + bIdx[i] * 64u));
            // Antipodality: flip a quaternion in the opposite hemisphere.
            float sign = (dot(dq0.real, dqi.real) < 0.0f) ? -1.0f : 1.0f;
            blended.real += dqi.real * (bWgt[i] * sign);
            blended.dual += dqi.dual * (bWgt[i] * sign);
        }
    }

    // --- Normalize the blended dual quaternion ---
    blended = NormalizeDQ(blended);

    // --- Apply DQ skinning ---
    float3 skinnedPos = DQTransformPoint(blended, restPos);
    float3 skinnedNrm = normalize(DQTransformNormal(blended, restNrm));

    // The tangent takes the same rigid rotation as the normal (DQS has no shear, so no inverse
    // transpose); handedness .w passes through.
    float3       skinnedTan = hasTangent
                            ? normalize(DQTransformNormal(blended, restTan.xyz))
                            : restTan.xyz;

    // Safety net: a non-finite or wildly out-of-range vertex (degenerate bone matrix, garbage pose or
    // morph data, antipodal DQ blow-up) becomes a screen-spanning triangle whose pixel shader runs for
    // billions of fragments, past Windows' ~2 s GPU timeout -> DXGI_ERROR_DEVICE_HUNG. Collapse it to
    // the rest pose: a glitch, not a hang. !(d < T) is TRUE for NaN, inf AND |pos| > 1e5, and unlike
    // isnan / isinf fast-math cannot strip it.
    if (!(dot(skinnedPos, skinnedPos) < 1e10f))
    {
        skinnedPos = restPos;
        skinnedNrm = float3(0, 1, 0);
        skinnedTan = float3(1, 0, 0);
    }

    // Character-extent clamp, the main guard: a skinned vertex never legitimately sits more than ~4x
    // the character's rest size from the model origin; beyond that the stream is corrupt (e.g. an
    // identical-mesh + recycled-entity-ID reload). Snap it to rest so it cannot span the screen and
    // TDR the GPU. !(d < T) also catches NaN / inf (fast-math-proof).
    const float maxR = g_Job.charExtent * 4.0f;
    if (g_Job.charExtent > 0.0f && !(dot(skinnedPos, skinnedPos) < maxR * maxR))
    {
        skinnedPos = restPos;
        skinnedNrm = restNrm;
        skinnedTan = restTan.xyz;
    }

    // Also clamp DISPLACEMENT from rest: a vertex more than ~2.5x the character's size from its own
    // rest is corrupt even at a moderate absolute magnitude - the screen-spanning triangle the
    // distance-from-origin test misses.
    const float3 disp = skinnedPos - restPos;
    const float  maxD = g_Job.charExtent * 2.5f;
    if (g_Job.charExtent > 0.0f && !(dot(disp, disp) < maxD * maxD))
    {
        skinnedPos = restPos;
        skinnedNrm = restNrm;
        skinnedTan = restTan.xyz;
    }

    // --- Write output ---
    g_OutPos[outPosIdx] = skinnedPos;
    g_OutNrm[outNrmIdx] = skinnedNrm;
    if (hasTangent)
        g_OutTan[outTanIdx] = float4(skinnedTan, restTan.w);

    // No previous-position output: the consumer reads last frame's positions from the previous ring
    // slot (MeshDescriptor::prevPosition).
}
