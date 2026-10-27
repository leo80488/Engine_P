// particle_vertex.hlsli - the engine-owned particle billboard vertex template + a small
// author hook (CustomizeParticleVertex): the custom_vertex.hlsli counterpart for procedural
// billboard quads (no MeshDescriptor / PVF).
// A custom particle VS is a `.vs.hlsl` that defines PARTICLE_HAS_VERTEX, includes this
// header and implements exactly one function:
//       #define PARTICLE_HAS_VERTEX
//       #include "particle_vertex.hlsli"
//
//       void CustomizeParticleVertex(inout ParticleVtxContext v)
//       {
//           // e.g. butterfly wing flap: fold both wing tips up out of the quad
//           // plane along the normal (pick a Horizontal-orientation emitter so
//           // the plane is the world floor, not the camera).
//           float phase = v.phaseSeed * 6.2831853 + v.time * 9.0;
//           float flap  = sin(phase) * 1.15;
//           float wing  = abs(v.quadOffset.x) * v.size;   // tip folds most
//           v.worldPos += v.normal * (sin(flap) * wing);
//       }
// The engine supplies main(): it builds the billboard, fills ParticleVtxContext, calls the
// hook, then projects to clip space. The output matches the engine default VS, so any
// particle PS consumes it. Assign the file to the emitter's "Vertex Shader" slot in the
// inspector.

#ifndef PARTICLE_VERTEX_HLSLI
#define PARTICLE_VERTEX_HLSLI

#include "particle_common.hlsli"

// Per-frame camera state at b3 space0 (a ROOT_CBV slot of the shared PVF root sig:
// b1 = PerView, b2 = the forward LightCB of lit buckets, b3 -> BindConstantBuffer slot 2).
cbuffer ParticleRenderCB : register(b3, space0)
{
    float4x4 viewProj;     // jittered (for SV_POSITION)
    float3   camRight;     // world-space camera right
    float    particleSize; // global scale multiplier (default 1.0)
    float3   camUp;        // world-space camera up
    float    gTime;        // scene time in seconds (for VS animation)
};

StructuredBuffer<Particle> gPool : register(t4, space0);

// 4 verts per instance, arranged as a triangle strip:
//   (-1,-1) (1,-1) (-1,1) (1,1)
static const float2 kQuadOffsets[4] =
{
    float2(-1.0, -1.0),
    float2( 1.0, -1.0),
    float2(-1.0,  1.0),
    float2( 1.0,  1.0),
};

// ==== ParticleVtxContext - the customization surface ====
// Writable fields feed the final vertex; read-only fields give the hook what it needs
// (quad-local corner to identify wing halves, per-particle phase, time).
struct ParticleVtxContext
{
    // ---- Writable (your hook edits these) ----
    float3 worldPos;     // billboard vertex world position - add offsets here
    float2 uv;           // sprite UV (0..1)
    float4 color;        // RGBA tint (already age-lerped)

    // ---- Read-only context ----
    float2 quadOffset;   // [-1,1] corner; x<0 = left half (e.g. left wing)
    float  lifeT;        // 0..1 age (0 = just spawned)
    float  phaseSeed;    // per-particle random [0,1)
    float  time;         // scene time in seconds (== gTime)
    float3 center;       // particle centre world position
    float3 axisRight;    // quad in-plane right axis (= camera right when billboarding)
    float3 axisUp;       // quad in-plane up axis
    float3 normal;       // quad face normal (out-of-plane; use for 3D folds/flaps).
                         // Billboard / VelocityStretched: the camera FORWARD, i.e.
                         // AWAY from the viewer. Horizontal +Y, Vertical +Z. The
                         // lit shading normal faces the viewer instead (faceSign).
    float  size;         // particle size after life taper (before per-axis scale)
    float3 scale;        // per-axis size multiplier (x -> right, y -> up, z -> normal)
};

// Author-provided hook, declared so main() can call it. The author's file defines it
// (after this include) when PARTICLE_HAS_VERTEX is defined; otherwise the weak default below
// is the identity.
void CustomizeParticleVertex(inout ParticleVtxContext v);

#ifndef PARTICLE_HAS_VERTEX
void CustomizeParticleVertex(inout ParticleVtxContext v) { /* identity */ }
#endif

// ==== Main (engine-owned) ====
ParticleVaryings main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    ParticleVaryings o = (ParticleVaryings)0;

    Particle p = gPool[instanceID + g_bucketBase];   // rebase into this bucket's slice

    // Dead particle -> degenerate quad at origin (the rasteriser discards it).
    // maxLifetime == 0 is an uninitialised/dead slot; maxLifetime < 0 is an infinite ambient
    // particle (kept alive, no aging), so it must not be culled.
    if (p.lifetime <= 0.0 || p.maxLifetime == 0.0)
    {
        o.pos     = float4(0, 0, 0, 1);
        o.texIdx  = 0xFFFFFFFFu;
        return o;
    }

    // 0..1 normalized life (0 = just spawned, 1 = about to die). Infinite particles
    // (maxLifetime < 0) never age -> lifeT = 0 (constant spawned look).
    float lifeT = (p.maxLifetime > 0.0) ? saturate(1.0 - p.lifetime / p.maxLifetime) : 0.0;

    // Size taper: shrink to 70% by end of life so particles don't pop out.
    float size = p.startSize * particleSize * (1.0 - 0.3 * lifeT);

    // Per-axis scale: x stretches along the quad right axis, y along up. z scales the
    // out-of-plane normal axis - inert for a flat billboard, but exposed to custom vertex hooks
    // for 3D folds/flaps.
    float3 scale = p.startScale;

    // Select quad axes by orientation. Billboard faces the camera; the fixed
    // modes keep a stable world plane; VelocityStretched aligns the quad to the
    // particle's velocity and elongates it with speed (sparks / streaks).
    float3 axisR, axisU, axisN;
    bool   velMode = (p.orientation == PARTICLE_ORIENT_VELOCITY);
    float  stretchLen = 0.0;   // extra length along axisU (velocity mode only)
    // faceSign turns axisN into the face term of the lit shading normal, which must point at
    // the viewer. The camera-facing modes set axisN to cross(camRight, camUp) = the camera
    // forward (the view basis is left-handed), i.e. away from the viewer, so they set
    // faceSign = -1. Do not bulge around +axisN there: that shades the far hemisphere (a sun
    // behind the camera leaves a lit emitter black). Only the shading normal flips: axisN
    // reaches custom hooks as v.normal and they displace along it (butterfly_flap.vs.hlsl folds
    // its wings that way), so its direction is part of the hook contract. Horizontal / Vertical
    // shade with their fixed world normal (+Y / +Z) seen from either side.
    float  faceSign = 1.0;
    if (p.orientation == PARTICLE_ORIENT_HORIZONTAL)
    {
        axisR = float3(1, 0, 0); axisU = float3(0, 0, 1); axisN = float3(0, 1, 0);
    }
    else if (p.orientation == PARTICLE_ORIENT_VERTICAL)
    {
        axisR = float3(1, 0, 0); axisU = float3(0, 1, 0); axisN = float3(0, 0, 1);
    }
    else if (velMode)
    {
        // Stretch axis = velocity; width axis = perpendicular to velocity and the view direction
        // (cross(camRight,camUp) is the camera forward, so no camera position is needed) -> the
        // streak always faces the camera.
        float  speed = length(p.velocity);
        float3 dir   = speed > 1e-4 ? p.velocity / speed : camUp;
        float3 viewN = normalize(cross(camRight, camUp));
        // When the streak flies straight at/away from the camera, dir is parallel to viewN, cross()
        // collapses to ~0 and normalize() would yield NaN: fall back to camRight for the width axis
        // in that (visually degenerate) case.
        float3 sideR = cross(dir, viewN);
        float  sideL = length(sideR);
        axisR = sideL > 1e-4 ? sideR / sideL : camRight;
        axisU = dir;
        axisN = viewN;
        faceSign = -1.0;
        stretchLen = speed * p.stretch;     // motion-blur-like elongation
    }
    else // PARTICLE_ORIENT_BILLBOARD
    {
        axisR = camRight; axisU = camUp; axisN = normalize(cross(camRight, camUp));
        faceSign = -1.0;
    }

    float2 q = kQuadOffsets[vertexID];
    float3 worldPos;
    if (velMode)
    {
        // Anchor the head at the particle position and trail the quad BACKWARD
        // along -velocity, so the streak reads like motion blur of its path.
        float halfW    = size * scale.x;                    // streak half-width
        float totalLen = size * scale.y * 2.0 + stretchLen; // head -> tail length
        float along    = q.y * 0.5 + 0.5;                   // 1 = head, 0 = tail
        worldPos = p.position
                 + axisR * (q.x * halfW)
                 - axisU * ((1.0 - along) * totalLen);
    }
    else
    {
        worldPos = p.position
                 + axisR * (q.x * size * scale.x)
                 + axisU * (q.y * size * scale.y);
    }

    // Fill the customization context and let the author hook displace it.
    ParticleVtxContext v;
    v.worldPos   = worldPos;
    v.uv         = q * 0.5 + 0.5;
    v.color      = lerp(p.startColor, p.endColor, lifeT);
    v.quadOffset = q;
    v.lifeT      = lifeT;
    v.phaseSeed  = p.phaseSeed;
    v.time       = gTime;
    v.center     = p.position;
    v.axisRight  = axisR;
    v.axisUp     = axisU;
    v.normal     = axisN;
    v.size       = size;
    v.scale      = scale;
    CustomizeParticleVertex(v);

    // Emissive fade over life: taper the brightness as the particle ages so sparks cool toward
    // death (emissiveFade == 0 -> constant). Folded into the intensity varying.
    float emissive = p.intensity;
    if (p.emissiveFade > 0.0)
        emissive *= pow(saturate(1.0 - lifeT), p.emissiveFade);

    o.pos       = mul(float4(v.worldPos, 1.0), viewProj);
    o.uv        = v.uv;
    o.color     = v.color;
    o.texIdx    = p.textureBindlessIdx;
    o.visMode   = p.visualMode;
    o.lifeT     = lifeT;
    o.intensity = emissive;
    o.worldPos  = v.worldPos;
    // Spherical billboard normal: bulge the quad corners out from the face
    // normal so lit particles read as round soft volumes (not flat-shaded
    // cards) - the hemisphere that faces the viewer, through faceSign (see
    // the orientation block). Written unconditionally so the VS signature
    // matches both the lit and unlit PS permutations.
    o.normalWS  = normalize(v.axisRight * v.quadOffset.x
                          + v.axisUp    * v.quadOffset.y
                          + v.normal    * faceSign);

    // Ambient wrap-volume fade, applied after the author hook so a custom VS cannot erase it
    // (without it a wrap teleport pops and smears in TAA). Dims alpha and intensity so every
    // blend mode darkens: alpha covers the SRC_ALPHA blends (Alpha, Additive), intensity the ones
    // whose source factor ignores alpha (Premultiplied ONE, Multiply DEST_COLOR). fade == 0 at
    // the faces -> wraps are radiometrically invisible.
    if ((p.flags & PARTICLE_FLAG_AMBIENT) != 0u)
    {
        o.color.a   *= p.fade;
        o.intensity *= p.fade;
    }
    return o;
}

#endif // PARTICLE_VERTEX_HLSLI
