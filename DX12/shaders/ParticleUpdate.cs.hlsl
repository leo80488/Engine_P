// Advances every particle in the global pool.
//
// One thread per pool slot. Dead particles (lifetime <= 0) are skipped; the render VS also
// drops them as degenerate quads. A single global gravity lives in ParticleSystemCB (taken
// from the first emitter on the CPU side): per-emitter gravity directions are not supported.
//
// Root signature (space2):
//   b0 -> ParticleSystemParams (dt, particleCount, frameIndex, gravity)
//   u0 -> DESC_TABLE -> gPool UAV

#include "Particle.hlsli"

cbuffer SystemCB : register(b0, space2)
{
    ParticleSystemParams gParams;
};

RWStructuredBuffer<Particle> gPool : register(u0, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint idx = id.x;
    if (idx >= gParams.particleCount) return;

    // Cheap field-level early-outs before loading the whole 128 B record: most pool slots are
    // dead, so the full-pool dispatch reads ~4 B per dead slot instead of 128 B.
    if (gPool[idx].lifetime <= 0.0) return;
    // Boids are simulated by ParticleBoidSteer (it already integrated position / velocity and
    // re-armed lifetime this frame); skip them so gravity / drag does not fight the steering.
    if ((gPool[idx].flags & PARTICLE_FLAG_FLOCK) != 0u) return;

    Particle p = gPool[idx];

    // Ambient: camera-follow box + toroidal wrap. World-anchored particles are wrapped into a
    // camera-centred AABB by whole box periods only (bit-exact identity inside the box), so the
    // field hangs in world space and only the sampling window follows the camera: no swimming,
    // no per-frame FP drift.
    if ((p.flags & PARTICLE_FLAG_AMBIENT) != 0u)
    {
        const uint vi = (p.flags >> PARTICLE_AMBIENT_VOL_SHIFT) & PARTICLE_AMBIENT_VOL_MASK;
        if ((gParams.ambientActiveMask & (1u << vi)) != 0u)
        {
            const float4 A = gParams.ambientVolA[vi];   // xyz = H,      w = edgeFade
            const float4 B = gParams.ambientVolB[vi];   // xyz = center, w = population count
            const float4 C = gParams.ambientVolC[vi];   // xyz = wind,   w = nearFade0
            const float4 D = gParams.ambientVolD[vi];   // x = nearFade1, y = expectedBucket (float value), z = intensity ramp

            // Kill orphans stranded in a stale slice (bucket ids are re-discovered every frame and an
            // immortal population must never live in the wrong slice), and slots past the population
            // after a count shrink (as ParticleBoidSteer kills slots >= flockCount). D.y is the expected
            // bucket as a float value, not a bit-cast: bucket ids 4..7 as int bit patterns are denormals
            // a float CB load may flush to zero, which would kill every ambient particle.
            const uint sliceLen = gParams.particleCount / PARTICLE_BUCKET_COUNT;
            const uint bucketId = idx / sliceLen;
            const uint localIdx = idx - bucketId * sliceLen;
            if (bucketId != (uint)(D.y + 0.5) || localIdx >= (uint)B.w)
            {
                p.lifetime = 0.0; p.flags = 0u; p.fade = 0.0;
                gPool[idx] = p;
                return;
            }

            // Refresh appearance from the live CB rows so inspector edits to the sibling
            // ParticleRenderComponent apply in real time (the one-shot Box fill only sets the initial
            // look). Position / velocity / lifetime / flags / phaseSeed are left untouched.
            const float4 E  = gParams.ambientVolE[vi];   // startColor
            const float4 F  = gParams.ambientVolF[vi];   // endColor
            const float4 G  = gParams.ambientVolG[vi];   // xyz = startScale, w = startSize
            const float4 Hp = gParams.ambientVolH[vi];   // x=intensity, y=emissiveFade, z=stretch, w=packed(vm+or*4)
            p.startColor   = E;
            p.endColor     = F;
            p.startScale   = G.xyz;
            p.startSize    = G.w;
            p.intensity    = Hp.x;
            p.emissiveFade = Hp.y;
            p.stretch      = Hp.z;
            const uint apk = (uint)(Hp.w + 0.5);          // packed value, exact (<= 15)
            p.visualMode   = apk & 3u;
            p.orientation  = (apk >> 2) & 3u;
            p.textureBindlessIdx = (D.w < 0.0) ? 0xFFFFFFFFu : (uint)(D.w + 0.5);

            // Clamp dt so a frame hitch can't teleport a particle past the fade
            // band (which would ghost under TAA). Worst case the field lags a
            // hitch by one frame instead of streaking.
            const float dt = min(gParams.deltaTime, 0.05);

            // Integrate without globalGravity (it comes from the first, unrelated emitter); ambient
            // fall / drift is authored velocity + wind.
            p.velocity *= saturate(1.0 - p.drag * dt);
            p.position += (p.velocity + C.xyz) * dt;

            // Toroidal wrap: subtract whole periods only (k == 0 -> bit-exact identity). Use floor():
            // HLSL fmod truncates toward zero and would never wrap a particle past the -face.
            const float3 H      = A.xyz;
            const float3 period = 2.0 * H;
            const float3 rel    = p.position - B.xyz;
            const float3 k      = floor((rel + H) / period);
            p.position         -= k * period;

            // Edge fade (0 at every face) x camera-approach fade x intensity ramp. A particle can only
            // wrap where fade == 0, so the teleport carries no radiance and is invisible to TAA.
            const float3 dEdge = H - abs(p.position - B.xyz);           // >= 0 post-wrap
            const float3 t     = saturate(dEdge / max(A.w, 1e-3));
            float fade         = t.x * t.y * t.z;
            fade               = fade * fade * (3.0 - 2.0 * fade);      // smoothstep the product (C1)
            fade              *= smoothstep(C.w, D.x, distance(p.position, gParams.cameraPos));
            fade              *= D.z;                                    // intensity ramp
            p.fade             = saturate(fade);

            // Lifetime (position is never re-randomized, so the uniform fill stays uniform).
            // maxLifetime < 0 = infinite: skip aging so the one-shot fill does not pulse in sync (the
            // ambient default). A finite maxLifetime cycles by re-arming so fireflies blink through the
            // start -> end colour cycle.
            if (p.maxLifetime > 0.0)
            {
                p.lifetime -= dt;
                if (p.lifetime <= 0.0) p.lifetime += max(p.maxLifetime, 0.01);
            }

            gPool[idx] = p;
            return;
        }
        // Volume inactive (emitter disabled / destroyed / world cleared): fall through to the
        // standard path so the field drifts and decays over its remaining lifetime instead of
        // despawning at once. An infinite field (lifetime 1, maxLifetime < 0) ends together ~1 s later.
    }

    p.velocity += gParams.globalGravity * gParams.deltaTime;
    // Per-particle air drag: damping so sparks decelerate fast, then settle into the gravity
    // arc. p.drag == 0 -> coast.
    p.velocity *= saturate(1.0 - p.drag * gParams.deltaTime);
    p.position += p.velocity            * gParams.deltaTime;
    p.lifetime -= gParams.deltaTime;

    gPool[idx] = p;
}
