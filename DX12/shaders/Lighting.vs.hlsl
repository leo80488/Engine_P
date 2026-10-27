struct VSOut 
    { 
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
    };
VSOut main(uint id : SV_VertexID) {
    float2 pos[3] = { float2(-1,1), float2(3,1), float2(-1,-3) };
    float2 uv[3]  = { float2(0,0),  float2(2,0), float2(0, 2)  };
    VSOut o; o.pos = float4(pos[id],0,1); o.uv = uv[id]; return o;
}
