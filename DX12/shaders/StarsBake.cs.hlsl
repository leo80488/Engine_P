// Bakes the static starfield cube that Skybox.ps.hlsl samples. SkyboxPass runs it
// on the first Execute after it creates the cube (the first night frame) and again
// after a shader reload; the runtime PS then does one cube sample plus the twinkle
// and the horizon / day-night fade, with no per-pixel hash or cell traversal.
// Output: TextureCube (6 slices, RGBA16F, alpha = twinkle phase), written as a
// 2D-array UAV with z = face index.
// Star generation:
//   1. Ray direction -> 3D cell index (Worley-like cell grid).
//   2. Per-cell hash: presence (~3% of cells), position in the cell, colour, size.
//   3. Angular distance from the texel direction to the star -> smoothstep disk.
//   4. Pastel palette, 5 stops red -> orange -> white -> cyan -> blue.

cbuffer StarsBakeCB : register(b0, space2)
{
    uint   CubeSize;            // texels per face (SkyboxPass uses 1024)
    float  StarDensity;         // grid resolution; ~250 -> a few thousand stars
    float  StarBrightnessBake;  // not applied: the bake stays full HDR, the runtime applies StarBrightness
    uint   _pad0;
};

RWTexture2DArray<float4> gOutput : register(u0, space2);

// 3-channel hash from a 3D cell key. Cheap and well-distributed.
float3 HashCell3(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}

// Pastel star palette; t in [0, 1] runs from red to blue.
float3 StarColor(float t)
{
    float3 cRed    = float3(1.00, 0.78, 0.72);
    float3 cOrange = float3(1.00, 0.90, 0.78);
    float3 cWhite  = float3(1.00, 0.98, 0.95);
    float3 cCyan   = float3(0.86, 0.93, 1.00);
    float3 cBlue   = float3(0.78, 0.86, 1.00);
    if (t < 0.25) return lerp(cRed,    cOrange, t * 4.0);
    if (t < 0.50) return lerp(cOrange, cWhite,  (t - 0.25) * 4.0);
    if (t < 0.75) return lerp(cWhite,  cCyan,   (t - 0.50) * 4.0);
                  return lerp(cCyan,   cBlue,   (t - 0.75) * 4.0);
}

// Cube-face UV -> world ray direction. D3D cubemap face order (slice 0..5):
//   0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z.
// uv in [0,1]^2 covers one face and is mapped to [-1,+1]^2.
float3 CubeFaceToDir(uint face, float2 uv)
{
    float u = uv.x * 2.0 - 1.0;
    float v = uv.y * 2.0 - 1.0;
    float3 dir;
    if      (face == 0) dir = float3( 1.0, -v,  -u);  // +X
    else if (face == 1) dir = float3(-1.0, -v,   u);  // -X
    else if (face == 2) dir = float3( u,    1.0, v);  // +Y
    else if (face == 3) dir = float3( u,   -1.0,-v);  // -Y
    else if (face == 4) dir = float3( u,   -v,   1.0);// +Z
    else                dir = float3(-u,   -v,  -1.0);// -Z
    return normalize(dir);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= CubeSize || dtid.y >= CubeSize) return;
    if (dtid.z >= 6) return;

    uint   face = dtid.z;
    float2 uv   = (float2(dtid.xy) + 0.5) / float(CubeSize);
    float3 rd   = CubeFaceToDir(face, uv);

    // Star math; the twinkle and the horizon fade are applied at runtime.
    float3 scaled  = rd * StarDensity;
    float3 cellId  = floor(scaled);

    float3 h1 = HashCell3(cellId);
    float3 h2 = HashCell3(cellId + 71.23);

    const float kPresenceThreshold = 0.03;
    float3 colorOut = float3(0, 0, 0);

    float twinklePhase = 0.0;  // 0..1, the runtime scales it by 2pi; 0 = no star here
    if (h1.x <= kPresenceThreshold)
    {
        // Star centre in the cell -> unit direction.
        float3 starWorld = cellId + h1;
        float3 starDir   = normalize(starWorld);
        float  cosD      = dot(rd, starDir);

        // Per-star size: small enough to read as stars, wide enough that bilinear
        // sampling is stable and every screen pixel inside a star hits a non-empty texel
        // (keeps the alpha phase valid). At 1024^2 per face a texel is ~0.088 deg; ~85%
        // of stars get a core radius of ~0.06 deg (cos 0.9999995), the rest grow up to
        // ~0.11 deg (cos 0.9999981).
        float sizeT   = saturate((h2.z - 0.85) / 0.15);
        float coreCos = lerp(0.9999995, 0.9999981, sizeT);

        if (cosD >= coreCos)
        {
            float disk    = smoothstep(coreCos, 1.0, cosD);
            float baseMag = lerp(0.40, 1.40, h1.z);
            float3 col    = StarColor(h1.y);
            // Full-HDR bake: StarBrightnessBake is not applied, the runtime applies
            // StarBrightness once. Baked values stay near [0.4, 1.4], in the linear part of
            // the tonemap where the twinkle swing stays visible; attenuating twice pushes
            // them into the toe and hides it.
            colorOut = col * disk * baseMag;

            // Per-star twinkle phase in alpha. h2.x depends only on cellId, so every texel
            // of one star stores the same phase, bilinear sampling keeps it flat across the
            // disk, and the whole star brightens and dims in lockstep instead of shimmering
            // per pixel.
            // Offset by 0.01 so a phase of 0 still marks a star: alpha 0 is empty sky, which
            // the runtime tests.
            twinklePhase = 0.01 + 0.99 * h2.x;
        }
    }

    gOutput[uint3(dtid.xy, face)] = float4(colorOut, twinklePhase);
}
