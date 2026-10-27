// Opt-in ACEScg scene-linear grading, followed by an authored toe/shoulder film response. Values
// come from a scene profile, never from global engine defaults.
struct UEToneMapData
{
    float4 shadowsSaturation;
    float4 shadowsContrast;
    float4 shadowsGamma;
    float4 shadowsGain;
    float4 shadowsOffset;
    float4 midtonesSaturation;
    float4 midtonesContrast;
    float4 midtonesGamma;
    float4 midtonesGain;
    float4 midtonesOffset;
    float4 highlightsSaturation;
    float4 highlightsContrast;
    float4 highlightsGamma;
    float4 highlightsGain;
    float4 highlightsOffset;
    float4 ranges;
    float4 film;
    float4 controls;
};
static const float3 UE_LUMA = float3(.27222872,.67408177,.05368952);
static const float3x3 UE_SRGB_AP1 = {
    .6130974,.3395231,.0473795,
    .0701937,.9163539,.0134524,
    .0206156,.1095698,.8698146 };
static const float3x3 UE_AP1_SRGB = {
    1.7050515,-.6217907,-.0832584,
    -.1302571,1.1408028,-.0105485,
    -.0240033,-.1289688,1.1529717 };
float3 UEGradeRange(float3 c,float3 sat,float3 contrast,float3 gamma,float3 gain,float3 offset)
{
    c=max(0,lerp(dot(c,UE_LUMA).xxx,c,sat));
    c=pow(max(c/.18,1e-8),max(contrast,1e-4))*.18;
    return pow(max(c,1e-8),rcp(max(gamma,1e-4)))*gain+offset;
}
float3 UEGrade(float3 c,UEToneMapData p)
{
    float l=dot(c,UE_LUMA);
    float s=1-smoothstep(0,max(p.ranges.x,1e-4),l);
    float h=smoothstep(p.ranges.y,max(p.ranges.y+1e-4,p.ranges.z),l);
    return UEGradeRange(c,p.shadowsSaturation.xyz,p.shadowsContrast.xyz,p.shadowsGamma.xyz,p.shadowsGain.xyz,p.shadowsOffset.xyz)*s
        +UEGradeRange(c,p.midtonesSaturation.xyz,p.midtonesContrast.xyz,p.midtonesGamma.xyz,p.midtonesGain.xyz,p.midtonesOffset.xyz)*(1-s-h)
        +UEGradeRange(c,p.highlightsSaturation.xyz,p.highlightsContrast.xyz,p.highlightsGamma.xyz,p.highlightsGain.xyz,p.highlightsOffset.xyz)*h;
}
float3 UEFilmResponse(float3 c,UEToneMapData p)
{
    // The scalar S-curve matches the authored film controls. UE's ACES glow / red-modifier steps and
    // gamut expansion are not part of this approximation.
    c=max(0,c);
    c=lerp(dot(c,UE_LUMA).xxx,c,.96);
    float slope=max(p.film.x,1e-4),toe=p.film.y,shoulder=p.film.z;
    float black=p.film.w,white=p.controls.y;
    float ts=max(1+black-toe,1e-4),ss=max(1+white-shoulder,1e-4);
    float bt=clamp((.18+black)/ts-1,-.9999,.9999);
    float tm=toe>.8 ? (1-toe-.18)/slope+log10(.18)
        : log10(.18)-.5*log((1+bt)/(1-bt))*ts/slope;
    float sm=(1-toe)/slope-tm,hm=shoulder/slope-sm;
    float3 x=log10(max(c,1e-8));
    float3 straight=slope*(x+sm);
    float3 low=-black+2*ts/(1+exp(clamp(-2*slope/ts*(x-tm),-60,60)));
    float3 high=1+white-2*ss/(1+exp(clamp(2*slope/ss*(x-hm),-60,60)));
    low=select(x<tm, low, straight);      // vector conditions need select() (HLSL 2021)
    high=select(x>hm, high, straight);
    float gap=hm-tm;
    float3 t=saturate((x-tm)/(abs(gap)<1e-5 ? 1e-5:gap));
    t=gap<0 ? 1-t:t;
    t=t*t*(3-2*t);
    c=lerp(low,high,t);
    return max(0,lerp(dot(c,UE_LUMA).xxx,c,.93));
}
float3 UEToneMap(float3 sceneLinear,UEToneMapData p)
{
    return max(0,mul(UE_AP1_SRGB,UEFilmResponse(UEGrade(mul(UE_SRGB_AP1,sceneLinear),p),p)));
}
