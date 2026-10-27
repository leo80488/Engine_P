// The present composite's full-screen triangle (SurfaceDX12::EnsurePresentPSO). Loaded
// through ShaderDiskCache::LoadOrCompile: cached, recorded as a variant (a package carries it
// cooked), compiled by the offline gates and hot-reloaded.

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID)
{
    VSOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.uv  = p;
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
