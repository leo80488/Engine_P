#ifndef LOCAL_FOG_VOLUMES_HLSLI
#define LOCAL_FOG_VOLUMES_HLSLI

// Bounded fog pockets (LocalFogVolumeComponent). The CPU (Renderer, game half)
// culls the scene's volumes against the froxel view distance, keeps the nearest
// VolumetricFogPass::kMaxLocalFogVolumes, sorts the emissive ones to the front and
// uploads this array. The froxel density pass walks all of them
// (FroxelParams.localVolumeCount); the light injection pass walks only the
// emissive prefix (localEmissiveCount).
//
// The consumer picks the register before including:
//   #define LOCAL_FOG_VOLUMES_REGISTER register(t5, space2)
//
// Mirror: VolumetricFogPass::LocalFogVolumeGPU (96 B).
struct LocalFogVolumeGPU
{
    // World -> unit-volume space. The volume is [-1,1]^3 (Box) or the unit ball
    // (Sphere) in this space: local.i = dot(float4(worldPos, 1), row_i).
    float4 row0;
    float4 row1;
    float4 row2;
    float3 albedo;     float density;       // sigma_t at the core (1/m)
    float3 emissive;   float edgeFade;      // emissive premultiplied by intensity
    uint   shape;      float heightFalloff; float halfHeight; float _pad;
};

#ifndef LOCAL_FOG_VOLUMES_REGISTER
#error "Define LOCAL_FOG_VOLUMES_REGISTER before including LocalFogVolumes.hlsli"
#endif
StructuredBuffer<LocalFogVolumeGPU> LocalFogVolumes : LOCAL_FOG_VOLUMES_REGISTER;

#define LOCAL_FOG_SHAPE_BOX    0u
#define LOCAL_FOG_SHAPE_SPHERE 1u

// Density weight of one volume at a world position, in [0, 1]:
//   0 outside the bounds, ramping to 1 over `edgeFade` (a fraction of the
//   half-extent) inside the boundary, times an optional exponential falloff
//   with height above the volume's bottom face.
float LocalFogWeight(LocalFogVolumeGPU v, float3 worldPos)
{
    const float4 wp = float4(worldPos, 1.0);
    const float3 l  = float3(dot(wp, v.row0), dot(wp, v.row1), dot(wp, v.row2));
    const float  invFade = 1.0 / max(v.edgeFade, 1e-4);

    float w;
    if (v.shape == LOCAL_FOG_SHAPE_SPHERE)
    {
        const float r = length(l);
        if (r >= 1.0) return 0.0;
        w = saturate((1.0 - r) * invFade);
    }
    else
    {
        const float3 a = abs(l);
        if (any(a >= 1.0)) return 0.0;
        // Per-axis product: soft corners, and a face never fades faster than
        // the axis it belongs to.
        const float3 f = saturate((1.0 - a) * invFade);
        w = f.x * f.y * f.z;
    }

    if (v.heightFalloff > 0.0)
        w *= exp(-v.heightFalloff * (l.y + 1.0) * v.halfHeight);
    return w;
}

#endif // LOCAL_FOG_VOLUMES_HLSLI
