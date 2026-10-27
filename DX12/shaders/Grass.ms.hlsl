// Grass.ms.hlsl - mesh shader: procedural Bezier grass blades (GoT-style).
//
// One MS group = one slice of one visible patch (the AS computed how many groups
// each patch needs from its LOD + blade budget). Every blade is rebuilt from hashes
// each frame - no vertex / instance buffers:
//
//   blade slot   bit-reversal of the blade index -> stable stratified cell in a
//                16x16 grid + per-blade jitter (positions never swim when density
//                falls with distance - a GoT requirement).
//   root         terrain heightmap sample (the same bicubic + UV math as the
//                terrain MS, so blades sit exactly on the rendered surface).
//   shape        quadratic Bezier ribbon: tilt away from vertical, mid-point slack
//                (bend), tapered width, single tip vertex.
//   clumps       Voronoi-ish cells - blades inherit facing / height / tint from
//                their clump, blended by g_clumpBlend (GoT clump identity).
//   wind         travelling sine front x scrolling value-noise gusts; the tip
//                displacement is re-evaluated at g_prevTime for TAA velocity.
//   culling      per-blade world-Y gates (water line / snow line) + a slope gate
//                from the heightmap gradient; rejected blades emit zero-area
//                triangles (counts are group-uniform).

#include "Grass.hlsli"
#include "Terrain.hlsli"   // TerrainSampleHeightBicubic

cbuffer PerViewCB : register(b1, space0)
{
    float4x4 g_viewProj;             // jittered - rasterization
    float4x4 g_prevViewProj;         // previous frame, unjittered - velocity
    float4x4 g_curViewProjNoJitter;  // current, unjittered - velocity
};

Texture2D<float> g_HeightMap   : register(t2, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_LinearClamp SmpFixed(SMP_LINEAR_CLAMP)

#define THREAD_COUNT 128
#define MAX_VERTS    256
#define MAX_PRIMS    256

struct VertexOut
{
    float4 sv       : SV_Position;
    float3 wn       : NORMAL;
    float4 col      : COLOR;       // rgb = clump/noise tint, a = root AO
    float2 misc     : TEXCOORD0;   // x = t along blade, y = unused
    float4 curClip  : TEXCOORD1;
    float4 prevClip : TEXCOORD2;
};

struct BladeData
{
    float3 root;       // world-space root position
    float  height;     // 0 = culled (degenerate)
    float2 facing;     // XZ unit
    float  width;      // half-width at the root
    float  tilt;       // radians from vertical
    float2 windCur;    // tip XZ displacement at g_time
    float2 windPrev;   // tip XZ displacement at g_prevTime
    float3 tint;
};

groupshared BladeData s_blades[48];   // kGrassBPG[2] = 48 max

float SampleTerrainY(float2 hmUV)
{
    float h = TerrainSampleHeightBicubic(g_HeightMap, g_LinearClamp, hmUV, g_hmTexel);
    return g_baseY + h * g_heightScale;
}

[outputtopology("triangle")]
[numthreads(THREAD_COUNT, 1, 1)]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    in   payload GrassPayload payload,
    out  vertices VertexOut verts[MAX_VERTS],
    out  indices  uint3     tris [MAX_PRIMS])
{
    // ---- Locate this group's patch in the per-lane payload ----
    uint patchIdx   = 0;
    uint bladeCount = 0;
    uint lod        = 0;
    uint subGroup   = 0;
    [unroll(GRASS_AS_GROUP_SIZE)]
    for (uint i = 0; i < GRASS_AS_GROUP_SIZE; ++i)
    {
        uint cnt = payload.bladeCount[i];
        if (cnt == 0) continue;            // culled patch -> empty group range
        uint base   = payload.groupBase[i];
        uint l      = payload.lodLevel[i];
        uint groups = (cnt + kGrassBPG[l] - 1u) / kGrassBPG[l];
        if (gid >= base && gid < base + groups)
        {
            patchIdx   = payload.patchIdx[i];
            bladeCount = cnt;
            lod        = l;
            subGroup   = gid - base;
        }
    }

    const uint seg  = kGrassSegs[lod];
    const uint bpg  = kGrassBPG[lod];
    const uint vpb  = 2 * seg + 1;     // verts per blade (pairs + tip)
    const uint tpb  = 2 * seg - 1;     // tris per blade

    const uint firstBlade = subGroup * bpg;
    const uint blades     = (bladeCount > firstBlade)
                          ? min(bpg, bladeCount - firstBlade) : 0u;

    SetMeshOutputCounts(blades * vpb, blades * tpb);
    if (blades == 0) return;

    // ---- Patch geometry ----
    const uint  N          = max(g_patchesPerSide, 1u);
    const uint  px         = patchIdx % N;
    const uint  pz         = patchIdx / N;
    const float patchSize  = g_grassSize / float(N);
    const float2 patchOrig = g_grassOrigin + float2(px, pz) * patchSize;

    // ---- Phase 1: one thread per blade -> groupshared blade data ----
    if (gtid < blades)
    {
        const uint b           = gtid;
        const uint bladeGlobal = firstBlade + b;

        BladeData bd;
        bd.height = 0.0;
        bd.root   = float3(patchOrig.x, g_baseY, patchOrig.y);
        bd.facing = float2(1, 0);
        bd.width  = g_bladeWidth;
        bd.tilt   = 0.0;
        bd.windCur = bd.windPrev = float2(0, 0);
        bd.tint   = float3(1, 1, 1);

        // Stable stratified placement: a bit-reversed slot in a 16x16 cell grid.
        uint  slot   = GrassBitRev8(bladeGlobal & 255u);
        float2 cell  = float2(slot & 15u, slot >> 4u);
        float2 jit   = float2(GrassHash(patchIdx, bladeGlobal, 1u),
                              GrassHash(patchIdx, bladeGlobal, 2u));
        float2 xz    = patchOrig + (cell + jit) * (patchSize / 16.0);

        // Terrain anchor + slope gate.
        float rootY   = g_baseY;
        float slopeUp = 1.0;
        if (g_hasHeightmap != 0)
        {
            float2 hmUV;
            float inTile = GrassTerrainUVValid(xz, hmUV);
            if (inTile > 0.0)
            {
                rootY = SampleTerrainY(hmUV);

                // Heightmap-gradient slope (bilinear taps are plenty for a gate).
                float texel = g_hmTexel;
                float hL = g_HeightMap.SampleLevel(g_LinearClamp, hmUV - float2(texel, 0), 0);
                float hR = g_HeightMap.SampleLevel(g_LinearClamp, hmUV + float2(texel, 0), 0);
                float hD = g_HeightMap.SampleLevel(g_LinearClamp, hmUV - float2(0, texel), 0);
                float hU = g_HeightMap.SampleLevel(g_LinearClamp, hmUV + float2(0, texel), 0);
                float worldTexel = g_hmTexel * g_terrainSize / max(g_hmUVScale.x, 1e-4);
                float3 nrm = normalize(float3((hL - hR) * g_heightScale,
                                              2.0 * worldTexel,
                                              (hD - hU) * g_heightScale));
                slopeUp = nrm.y;
            }
            else
            {
                slopeUp = -1.0;   // outside the terrain tile -> no grass
            }
        }

        bool alive = (slopeUp >= g_maxSlopeCos)
                  && (rootY >= g_minWorldY) && (rootY <= g_maxWorldY);
        if (alive)
        {
            bd.root = float3(xz.x, rootY, xz.y);

            // Clump identity (Voronoi-lite: one hash per clump cell).
            float2 clumpCell = floor(xz / max(g_clumpCellSize, 0.05));
            float2 ch  = HashSin22(clumpCell);
            float2 ch2 = HashSin22(clumpCell + 17.31);
            float  clumpAngle  = ch.x * 6.2831853;
            float2 clumpFacing = float2(cos(clumpAngle), sin(clumpAngle));
            float  clumpHMul   = lerp(0.70, 1.35, ch.y);
            float3 clumpTint   = float3(1.0, 1.0, 1.0)
                                + (float3(ch2.x, ch2.y, ch.x) - 0.5) * 0.25;

            // Per-blade identity, blended toward the clump.
            float hH = GrassHash(patchIdx, bladeGlobal, 3u);
            float hT = GrassHash(patchIdx, bladeGlobal, 4u);
            float hA = GrassHash(patchIdx, bladeGlobal, 5u);
            float hW = GrassHash(patchIdx, bladeGlobal, 6u);

            float  bladeAngle  = hA * 6.2831853;
            float2 ownFacing   = float2(cos(bladeAngle), sin(bladeAngle));
            bd.facing = normalize(lerp(ownFacing, clumpFacing, g_clumpBlend));

            float hMul = lerp(1.0, clumpHMul, g_clumpBlend);
            bd.height  = g_bladeHeight * (1.0 + (hH * 2.0 - 1.0) * g_bladeHeightVar) * hMul;
            bd.tilt    = g_tiltMax * (0.25 + 0.75 * hT);

            // Distance-based width: far blades widen to keep coverage (GoT).
            float dist = length(g_cameraPos.xz - xz);
            float wide = lerp(1.0, g_farWidthMul,
                              smoothstep(g_lod1Dist, g_cullDist, dist));
            bd.width = g_bladeWidth * (0.80 + 0.40 * hW) * wide;

            bd.tint = lerp(float3(1, 1, 1), clumpTint, g_clumpBlend);
            float cn = GrassValueNoise(xz * g_colorNoiseScale);
            bd.tint *= 1.0 + (cn - 0.5) * 2.0 * g_colorNoiseAmount;

            // Wind: amplitude field sampled at the root, applied at the tip.
            float phase = GrassHash(patchIdx, bladeGlobal, 8u) * 1.7;
            float ampC  = GrassWindAmp(xz, g_time + phase)     * g_windStrength
                        * (0.65 + 0.70 * hT);
            float ampP  = GrassWindAmp(xz, g_prevTime + phase) * g_windStrength
                        * (0.65 + 0.70 * hT);
            float2 perp = float2(-g_windDir.y, g_windDir.x);
            // Wobble evaluated at BOTH times, so the TAA velocity (cur - prev) captures the
            // lateral motion too, not just the gust amplitude. Two octaves at different rates,
            // for the same reason as the gust in GrassWindAmp: single-octave value noise stalls
            // on lattice crossings.
            float  wobC = (GrassValueNoise(xz * g_windScale * 2.3
                                           + g_time * 0.31) * 0.6
                         + GrassValueNoise(xz * g_windScale * 4.9
                                           - g_time * 0.53 + 7.3) * 0.4
                         - 0.5) * 0.45;
            float  wobP = (GrassValueNoise(xz * g_windScale * 2.3
                                           + g_prevTime * 0.31) * 0.6
                         + GrassValueNoise(xz * g_windScale * 4.9
                                           - g_prevTime * 0.53 + 7.3) * 0.4
                         - 0.5) * 0.45;
            bd.windCur  = g_windDir * ampC + perp * wobC * ampC;
            bd.windPrev = g_windDir * ampP + perp * wobP * ampP;
        }

        s_blades[b] = bd;
    }

    GroupMemoryBarrierWithGroupSync();

    // ---- Phase 2: vertices ----
    const uint totalVerts = blades * vpb;
    for (uint v = gtid; v < totalVerts; v += THREAD_COUNT)
    {
        const uint  b    = v / vpb;
        const uint  lv   = v % vpb;
        BladeData   bd   = s_blades[b];

        VertexOut o;
        if (bd.height <= 0.0)
        {
            // Culled blade - collapse to the root (zero-area triangles).
            float4 clip = mul(float4(bd.root, 1.0), g_viewProj);
            o.sv = clip;
            o.wn = float3(0, 1, 0);
            o.col = float4(0, 0, 0, 1);
            o.misc = float2(0, 0);
            o.curClip  = mul(float4(bd.root, 1.0), g_curViewProjNoJitter);
            o.prevClip = mul(float4(bd.root, 1.0), g_prevViewProj);
            verts[v] = o;
            continue;
        }

        const bool  isTip = (lv == 2 * seg);
        const uint  row   = isTip ? seg : (lv / 2);
        const float sideS = isTip ? 0.0 : ((lv & 1) ? 1.0 : -1.0);
        const float t     = float(row) / float(seg);

        const float3 up   = float3(0, 1, 0);
        const float3 dir3 = float3(bd.facing.x, 0, bd.facing.y);
        const float3 wC   = float3(bd.windCur.x,
                                   -0.35 * length(bd.windCur),
                                   bd.windCur.y);

        // Quadratic Bezier: P0 root, P1 mid (stiff lower half + bend slack),
        // P2 tip (static tilt + wind).
        const float sinT = sin(bd.tilt), cosT = cos(bd.tilt);
        const float3 P0 = bd.root;
        const float3 P2 = P0 + dir3 * (sinT * bd.height)
                             + up   * (cosT * bd.height) + wC;
        const float3 P1 = P0 + up * (bd.height * (0.62 - 0.22 * g_bendAmount))
                             + dir3 * (sinT * bd.height * 0.28)
                             + wC * 0.45;

        const float omt = 1.0 - t;
        float3 pos = omt * omt * P0 + 2.0 * t * omt * P1 + t * t * P2;
        float3 tan3 = normalize(2.0 * omt * (P1 - P0) + 2.0 * t * (P2 - P1));

        // Ribbon side + view-dependent thickening (edge-on blades widen so they never
        // alias away - a GoT trick).
        float2 s2    = float2(-bd.facing.y, bd.facing.x);
        float3 side3 = float3(s2.x, 0, s2.y);
        float2 toCam = g_cameraPos.xz - bd.root.xz;
        float  lenTC = max(length(toCam), 1e-3);
        float  edgeOn = abs(dot(toCam / lenTC, s2));
        float  wMul  = 1.0 + g_viewThicken * pow(edgeOn, 4.0);

        float halfW = bd.width * (1.0 - 0.85 * t) * wMul;
        pos += side3 * (halfW * sideS);

        // Normal: ribbon face normal oriented toward the camera, then blended
        // toward up so deferred lighting reads grass like a soft ground layer.
        float3 nrm = normalize(cross(side3, tan3));
        if (dot(nrm, g_cameraPos - pos) < 0.0) nrm = -nrm;
        nrm = normalize(lerp(nrm, up, g_normalBlend));

        // Previous-frame position: wind enters the Bezier through P1 (x0.45) and P2 (x1),
        // so dB/dwind ~ t. Cheap, and exact enough for TAA velocity.
        const float3 wP = float3(bd.windPrev.x,
                                 -0.35 * length(bd.windPrev),
                                 bd.windPrev.y);
        float3 prevPos = pos + (wP - wC) * t;

        o.sv       = mul(float4(pos, 1.0), g_viewProj);
        o.wn       = nrm;
        o.col      = float4(bd.tint, lerp(g_rootAO, 1.0, t));
        o.misc     = float2(t, 0.0);
        o.curClip  = mul(float4(pos, 1.0),     g_curViewProjNoJitter);
        o.prevClip = mul(float4(prevPos, 1.0), g_prevViewProj);
        verts[v] = o;
    }

    // ---- Phase 3: indices ----
    const uint totalPrims = blades * tpb;
    for (uint p = gtid; p < totalPrims; p += THREAD_COUNT)
    {
        const uint b    = p / tpb;
        const uint lp   = p % tpb;
        const uint base = b * vpb;

        uint3 tri;
        if (lp == tpb - 1)
        {
            // Tip triangle: last pair + tip vertex.
            tri = uint3(base + 2 * seg - 2, base + 2 * seg, base + 2 * seg - 1);
        }
        else
        {
            const uint r = lp / 2;
            tri = (lp & 1)
                ? uint3(base + 2 * r + 1, base + 2 * r + 2, base + 2 * r + 3)
                : uint3(base + 2 * r,     base + 2 * r + 2, base + 2 * r + 1);
        }
        tris[p] = tri;
    }
}
