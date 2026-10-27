// Procedural water grid, no vertex buffer: SV_VertexID -> one quad of a
// gridQuads x gridQuads grid spanning the water tile. The surface is flat (waves live in
// the PS flow normals); the grid lets vertex displacement be added here alone.

cbuffer PerViewCB : register(b1, space0)
{
    float4x4 g_viewProj;             // jittered (TAA)
    float4x4 g_prevViewProj;
    float4x4 g_curViewProjNoJitter;
};

cbuffer WaterCB : register(b2, space0)
{
    float2 g_waterOrigin;        // bottom-left XZ
    float  g_waterSize;
    float  g_waterHeight;

    float4 g_deepColor;          // rgb
    float4 g_shallowColor;       // rgb

    float2 g_flowDir;            // normalized XZ
    float  g_flowSpeed;          // m/s
    float  g_normalTiling;       // normal-map repeats per metre

    float  g_normalStrength;
    float  g_absorbDist;         // metres to full deep colour
    float  g_shoreFade;          // metres of shoreline alpha fade
    float  g_fresnelF0;

    float  g_specPower;
    float  g_reflStrength;
    float  g_waterTime;
    uint   g_gridQuads;

    float2 g_terrainOrigin;
    float  g_terrainSize;
    float  g_terrainBaseY;

    float  g_terrainHeightScale;
    uint   g_hasTerrain;
    float  g_hmTexel;
    float  g_waterRoughness;   // PS-only - laid out for parity

    float2 g_hmUVOffset;
    float2 g_hmUVScale;

    // row 9: interactive ripple field (PS-only, laid out for parity)
    float  g_rippleNormalStrength;
    float  g_rippleTexel;
    uint   g_hasRipple;
    float  g_ripplePad;

    // row 10: ripple sim window footprint (PS-only)
    float2 g_rippleWinOrigin;
    float  g_rippleWinSize;
    float  g_ripplePad2;
};

struct VSOut
{
    float4 sv       : SV_Position;
    float3 worldPos : POSITIONWS;
    float4 curClip  : TEXCOORD0;   // unjittered - velocity numerator
    float4 prevClip : TEXCOORD1;   // previous frame, unjittered
};

static const float2 kCorner[6] =
{
    float2(0, 0), float2(1, 0), float2(0, 1),
    float2(1, 0), float2(1, 1), float2(0, 1),
};

VSOut main(uint vid : SV_VertexID)
{
    const uint quads   = max(g_gridQuads, 1u);
    const uint quadIdx = vid / 6u;
    const uint corner  = vid % 6u;
    const uint qx      = quadIdx % quads;
    const uint qz      = quadIdx / quads;

    float2 uv = (float2(qx, qz) + kCorner[corner]) / float(quads);
    float3 wp = float3(g_waterOrigin.x + uv.x * g_waterSize,
                       g_waterHeight,
                       g_waterOrigin.y + uv.y * g_waterSize);

    VSOut o;
    o.sv       = mul(float4(wp, 1.0), g_viewProj);
    o.worldPos = wp;
    // Velocity inputs: the plane is static, so motion is the camera reprojection delta
    // (unjittered matrices, as GBuffer / Terrain / Grass).
    o.curClip  = mul(float4(wp, 1.0), g_curViewProjNoJitter);
    o.prevClip = mul(float4(wp, 1.0), g_prevViewProj);
    return o;
}
