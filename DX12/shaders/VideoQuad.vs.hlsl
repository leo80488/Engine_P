// World-space quad VS for VideoQuadPass. A procedural unit quad (two triangles from
// kQuad, local XY, Z = 0) is scaled by quadWidth x quadHeight here, then placed by
// worldMatrix and projected by the scene's UNJITTERED viewProj, so the panel does not
// shimmer under TAA. The pass draws with culling off.

// row_major: C++ writes XMFLOAT4X4 row-major, matching the engine's mul(rowVec, mat).
cbuffer VideoQuadCB : register(b2, space0)
{
    row_major float4x4 worldMatrix;     // entity GlobalTransform (quad size not baked in)
    row_major float4x4 viewProjMatrix;  // un-jittered scene viewProj
    float    quadWidth;
    float    quadHeight;
    float    alpha;
    uint     colorSpace;
};

static const float2 kQuad[6] = {
    float2(-0.5, -0.5),
    float2(-0.5,  0.5),
    float2( 0.5, -0.5),
    float2( 0.5, -0.5),
    float2( 0.5,  0.5),
    float2(-0.5,  0.5),
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    float2 local = kQuad[vid];

    // UV 0..1 across the quad, y flipped: local (-0.5, -0.5) -> (0, 1), so the texture's
    // top row is the quad's top edge.
    float2 uv = local + 0.5;
    uv.y = 1.0 - uv.y;

    float3 localPos = float3(local.x * quadWidth, local.y * quadHeight, 0.0);
    float4 worldPos = mul(float4(localPos, 1.0), worldMatrix);
    float4 clipPos  = mul(worldPos, viewProjMatrix);

    VSOut o;
    o.pos = clipPos;
    o.uv  = uv;
    return o;
}
