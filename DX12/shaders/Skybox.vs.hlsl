// Skybox vertex shader: draws the 36-vertex cube from a ByteAddressBuffer at t2
// at the reversed-Z far plane. The local cube position is the cubemap sampling
// direction (world rotation only, no translation).

cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;  // transpose(view * proj), row-vector convention
};

// Cube vertex positions: 36 float3 (12 bytes per vertex)
ByteAddressBuffer gCubeVB : register(t2, space0);

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 dir : TEXCOORD0;  // cubemap sampling direction
};

VSOut main(uint vid : SV_VertexID)
{
    uint byteOffset = vid * 12u;
    float3 localPos;
    localPos.x = asfloat(gCubeVB.Load(byteOffset     ));
    localPos.y = asfloat(gCubeVB.Load(byteOffset + 4u));
    localPos.z = asfloat(gCubeVB.Load(byteOffset + 8u));

    // w = 0 strips the camera translation so the sky stays centred.
    // Reversed-Z: the far plane is NDC z = 0, so force clipPos.z = 0 (z / w stays 0);
    // the GREATER_EQUAL test against the cleared 0 passes only where no geometry wrote.
    float4 clipPos = mul(float4(localPos, 0.0f), viewProj);
    clipPos.z = 0.0f; // force z/w = 0 (far plane under reversed Z)

    VSOut o;
    o.pos = clipPos;
    o.dir = localPos; // local cube position = world direction (no translation, only rotation)
    return o;
}
