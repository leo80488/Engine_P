// Generated from CartoonStreet PPVM_OutLine and PPVM_HightLight connected graphs.
// Source UV derivatives use pixel-sized offsets; source depth is in centimeters.
struct StreetPostParams { float2 uv0; };
float StreetSceneDepth(float2 uv) {
    uint w,h; gStreetDepth.GetDimensions(w,h);
    int2 p=clamp(int2(uv*float2(w,h)),int2(0,0),int2(w-1,h-1));
    float z=gStreetDepth.Load(int3(p,0));
    return z>0 ? 100*streetNear*streetFar/(streetNear+z*(streetFar-streetNear)) : 1e8;
}
float3 StreetWorldNormal(float2 uv) {
    uint w,h; gStreetNormal.GetDimensions(w,h);
    int2 p=clamp(int2(uv*float2(w,h)),int2(0,0),int2(w-1,h-1));
    float3 n=normalize(gStreetNormal.Load(int3(p,0)).xyz*2-1);
    return float3(n.x,-n.z,n.y);
}
// Source CustomDepth is used only to suppress outlines on visible tree surfaces.
// The imported per-instance material flag represents that visible mask; it is
// not a general replacement for a separate occluded-geometry depth buffer.
float StreetOutlineDepth(float2 uv) {
    uint w,h; gStreetNormal.GetDimensions(w,h);
    int2 p=clamp(int2(uv*float2(w,h)),int2(0,0),int2(w-1,h-1));
    uint material=(uint)(gStreetNormal.Load(int3(p,0)).a*65535.0+0.5)&0x7FFFu;
    if (material>=streetMaterialCount) return 1e8;
    return (gStreetMaterials[material].materialFlags&MAT_FLAG_SOURCE_OUTLINE_EXEMPT)!=0
        ? StreetSceneDepth(uv) : 1e8;
}
float3 SourceOutline(float2 uv,float3 inputColor) {
    StreetPostParams p; p.uv0=uv;
    float4 v0 = float4(inputColor,1);
    float3 v1 = (v0).rgb;
    float3 v2 = (float4(streetOutlineColor,1)).rgb;
    float4 v3 = StreetSceneDepth(p.uv0).xxxx;
    float v4 = (v3).r;
    float v5 = streetDepthMax;
    float v6 = (v4-v5);
    float v7 = (v6*0.000500000024f);
    float4 v8 = StreetSceneDepth(p.uv0).xxxx;
    float4 v9 = float4(p.uv0,0,1);
    float v10 = streetDepthWidth;
    float2 v11 = float2(1.0/width,1.0/height);
    float2 v12 = ((v10).xx*v11);
    float v13 = (float4(v12,0,0)).r;
    float v14 = 0.0f;
    float2 v15 = float2(v13,v14);
    float4 v16 = (v9+float4(v15,0,0));
    float4 v17 = StreetSceneDepth((v16).xy).xxxx;
    float4 v18 = (v8-v17);
    float2 v19 = (v15*(-1.0f).xx);
    float4 v20 = (v9+float4(v19,0,0));
    float4 v21 = StreetSceneDepth((v20).xy).xxxx;
    float4 v22 = (v8-v21);
    float4 v23 = (v18+v22);
    float v24 = (float4(v12,0,0)).g;
    float2 v25 = float2(v14,v24);
    float4 v26 = (v9+float4(v25,0,0));
    float4 v27 = StreetSceneDepth((v26).xy).xxxx;
    float4 v28 = (v8-v27);
    float2 v29 = (v25*(-1.0f).xx);
    float4 v30 = (v9+float4(v29,0,0));
    float4 v31 = StreetSceneDepth((v30).xy).xxxx;
    float4 v32 = (v8-v31);
    float4 v33 = (v28+v32);
    float4 v34 = (v23+v33);
    float v35 = streetDepthExponent;
    float4 v36 = pow(max(abs(v34),1e-8),(v35).xxxx);
    float4 v37 = saturate(v36);
    float4 v38 = 1-(v37);
    float v39 = (v38).r;
    float v40 = saturate(v39);
    float v41 = max(v7,v40);
    float v42 = saturate(v41);
    float3 v43 = lerp(v2,v1,(v42).xxx);
    float4 v44 = StreetOutlineDepth(p.uv0).xxxx;
    float4 v45 = StreetSceneDepth(p.uv0).xxxx;
    float4 v46 = (v44-v45);
    float v47 = (v46).r;
    float v48 = saturate(v47);
    float3 v49 = lerp(v1,v43,(v48).xxx);
    return v49;
}
float3 SourceHighlight(float2 uv,float3 inputColor) {
    StreetPostParams p; p.uv0=uv;
    float4 v0 = float4(inputColor,1);
    float3 v1 = (v0).rgb;
    float3 v2 = (float4(streetHighlightColor,1)).rgb;
    float3 v3 = (v1*v2);
    float2 v4 = (p.uv0*float2(1,1));
    float2 v5 = float2(1.0/width,1.0/height);
    float v6 = streetHighlightU;
    float v7 = streetHighlightV;
    float2 v8 = float2(v6,v7);
    float2 v9 = (v5*v8);
    float2 v10 = (v4+v9);
    float4 v11 = StreetSceneDepth(v10).xxxx;
    float v12 = (v11).r;
    float4 v13 = StreetSceneDepth(p.uv0).xxxx;
    float v14 = (v13).r;
    float v15 = (v14*1.10000002f);
    float v16 = (v12-v15);
    float v17 = saturate(v16);
    float3 v18 = lerp(v1,v3,(v17).xxx);
    float3 v19 = v1;
    float4 v20 = StreetSceneDepth(p.uv0).xxxx;
    float v21 = (v20).r;
    float v22 = (v21/100000.0f);
    float v23 = saturate(v22);
    float3 v24 = lerp(v18,v19,(v23).xxx);
    return v24;
}
