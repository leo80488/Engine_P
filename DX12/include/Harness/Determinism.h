#pragma once
// Harness/Determinism.h - the opt-in reproducibility envelope.
//
// Without it two runs of the same scene never agree: dt is wall clock, so a
// run's frame N describes a different simulation state every time, numeric
// assertions need absurd tolerances and golden images are noise.
//
// THERE ARE TWO CLOCKS. App keeps FrameContext::deltaTime (which respects Pause
// / Step through PlayModeController) AND GameModeContext::deltaTime, which
// drives Renderer::BeginFrame, m_globalTimeSec, LightCB.timeParams, particles /
// trails / tracers, grass and water wind, custom-shader time and FSR2. Patching
// PlayModeController alone would leave all of those on wall clock - the
// simulation reproducible and the PICTURE not. So -fixeddt is applied at the
// single source both clocks derive from (App::Run).
//
// EVERYTHING HERE DEFAULTS OFF. With no flags Active() is false and every
// consumer takes its normal path. Keep it that way: a person runs the editor
// interactively, and an envelope that changed their frame pacing, worker count
// or RT denoising would be a regression dressed as a feature.
//
// NOT CLAIMED: byte-exact cross-machine determinism. GPU floating-point results
// differ across driver versions, DLSS output depends on a DL model, and the
// parallel scheduler's completion order is pinned only by -workers=1 (which also
// stops exercising the most bug-prone parallel paths). The goal is
// reproducibility on ONE machine within ONE build - enough for tight numeric
// tolerances, and for golden images on the machine that blessed them.

#include <cstdint>

#ifndef WITH_HARNESS
// ==== Retail: inert stub ====
// Signatures are IDENTICAL to the real class, so every call site compiles
// unchanged and the optimizer folds these away (Engine.Cpp.props: Retail carries
// no harness).
namespace Harness
{
    class Determinism
    {
    public:
        static void Initialize() {}

        // Function-local static, not a static data member: inside the class
        // definition the class is still incomplete, so a member of its own type does
        // not compile. It mirrors the real Determinism::Get() and keeps the return type
        // `const Determinism&` - a prvalue would compile everywhere except a call site
        // that takes the address, and only in Retail.
        static const Determinism& Get()
        {
            static const Determinism s_inert{};
            return s_inert;
        }

        bool          Active()   const { return false; }
        float         FixedDt()  const { return 0.0f; }
        std::uint64_t Seed()     const { return 0; }
        bool          HasSeed()  const { return false; }
        bool          NoPacing() const { return false; }
        int           Workers()  const { return 0; }
    };
}
#else
namespace Harness
{
    class Determinism
    {
    public:
        /// Parse the flags. Called once from main.cpp AFTER Logger::Initialize
        /// (the numeric readers log on a malformed value).
        static void Initialize();
        static const Determinism& Get();

        /// True if ANY determinism flag was given. Consumers that only need
        /// "am I in a reproducible run" test this rather than each field.
        bool Active() const { return m_active; }

        /// -fixeddt=<seconds>. 0 = off (real wall clock).
        /// Applied at the single dt source, so BOTH clocks inherit it.
        float FixedDt() const { return m_fixedDt; }

        /// -seed=<n>; HasSeed() says whether it was given (Seed() is 0 when not, and
        /// -seed=0 is a real seed). Seeds GUID minting, Lua's math.random
        /// (ScriptSystem::Initialize - the stream behaviour-tree leaves draw from) and the
        /// facial-animation RNG. NOT covered: the order pairs() visits string keys, which
        /// Lua hashes with a per-state seed of time() and ASLR addresses.
        std::uint64_t Seed()   const { return m_seed; }
        bool          HasSeed() const { return m_hasSeed; }

        /// -nopacing: skip the frame limiter and force vsync off, so a bounded
        /// run finishes at machine speed instead of at the display's refresh
        /// rate. Purely a wall-clock saving; it does not affect simulation
        /// state once -fixeddt is in play.
        bool NoPacing() const { return m_noPacing; }

        /// -workers=N: TaskSystem worker count. 0 = engine default.
        /// -workers=1 removes the cross-system enqueue race and pins the physics job
        /// graph, but makes runs slower AND stops exercising the parallel code paths
        /// most likely to be buggy. Use it ONLY for golden-eligible packs, never as a
        /// global default.
        int Workers() const { return m_workers; }

    private:
        Determinism() = default;
        static Determinism& Mutable();

        float         m_fixedDt  = 0.0f;
        std::uint64_t m_seed     = 0;
        int           m_workers  = 0;
        bool          m_hasSeed  = false;
        bool          m_noPacing = false;
        bool          m_active   = false;
        bool          m_init     = false;
    };
}
#endif  // WITH_HARNESS
