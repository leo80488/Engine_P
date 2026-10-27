#include "Harness/Determinism.h"

#ifdef WITH_HARNESS
#include "System/CommandLine.h"
#include "System/Log.h"

namespace Harness
{

Determinism& Determinism::Mutable()
{
    static Determinism s_det;
    return s_det;
}

const Determinism& Determinism::Get() { return Mutable(); }

void Determinism::Initialize()
{
    Determinism& d = Mutable();
    if (d.m_init) return;
    d.m_init = true;

    const auto& cl = Sys::CommandLine::Get();

    d.m_fixedDt  = cl.Has("-fixeddt") ? cl.GetFloat("-fixeddt", 0.0f) : 0.0f;
    d.m_noPacing = cl.Has("-nopacing");
    d.m_workers  = cl.Has("-workers") ? cl.GetInt("-workers", 0) : 0;

    if (cl.Has("-seed"))
    {
        // GetInt, then widen: the seed is a convenience for reproducing a run,
        // not a cryptographic input, and a 32-bit space is plenty.
        const int s = cl.GetInt("-seed", 0);
        d.m_seed    = static_cast<std::uint64_t>(static_cast<std::uint32_t>(s));
        d.m_hasSeed = true;
    }

    // A negative or absurd fixed dt would silently produce a frozen or
    // exploding simulation, which reads as "the engine is broken" rather than
    // "the flag was wrong". Refuse it and stay on wall clock.
    if (d.m_fixedDt < 0.0f || d.m_fixedDt > 1.0f)
    {
        LOG_WARNING("-fixeddt=%.6f is outside (0, 1] seconds; ignoring it and "
                    "staying on wall-clock dt.", d.m_fixedDt);
        d.m_fixedDt = 0.0f;
    }

    d.m_active = (d.m_fixedDt > 0.0f) || d.m_hasSeed || d.m_noPacing || (d.m_workers > 0);

    if (!d.m_active) return;

    LOG_INFO("Determinism envelope ACTIVE: fixeddt=%.6f seed=%s%llu pacing=%s workers=%s",
             d.m_fixedDt,
             d.m_hasSeed ? "" : "(none) ",
             static_cast<unsigned long long>(d.m_seed),
             d.m_noPacing ? "OFF" : "on",
             d.m_workers > 0 ? "pinned" : "default");

    if (d.m_workers == 1)
        LOG_WARNING("-workers=1: runs are slower AND the parallel code paths — the "
                    "ones most likely to be buggy — are no longer exercised. This "
                    "is intended only for golden-image packs.");
}

}  // namespace Harness

#endif  // WITH_HARNESS
