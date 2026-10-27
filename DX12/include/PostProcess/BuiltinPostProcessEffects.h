#pragma once

// Adapters that wrap the Renderer's compute passes as PostProcess::IEffect so the
// PostChain drives them. Non-owning: the passes stay lifetime-managed by Renderer.
// Each adapter reads the SRV handles it needs from the Context, configures its
// pass, calls pass->Execute(ctx.cl) and writes what it produced back into the
// Context. Stage and Order place them: PostResolveHDR in steps of 100, ToneMap
// alone at the Tonemap stage. The Resolve stage's adapters are in
// PostProcess/ResolveEffects.h.

#include "PostProcess/IPostProcessEffect.h"

class CASPass;
class AutoExposurePass;
class BloomPass;
class LensFlarePass;
class ToneMapPass;
class UnderwaterPass;
class DepthOfFieldPass;
class StylizePass;
class LumaReducePass;

namespace PostProcess
{

// -energy_after_aa's measurement: the harness's LumaReduce on the resolved
// image, before any HDR effect (PostResolveHDR -100). In the chain only when the
// pass exists (-energy); runs only with -energy_after_aa (without it LumaReduce
// measures before the AA, on the pre-compute list). Like every PostResolveHDR ..
// Tonemap effect, its GPU region is the chain's "PostProcess.EnergyAfterAA"; the
// pass's own "Energy.Reduce" nests inside.
class EnergyAfterAAEffect final : public IEffect
{
public:
    explicit EnergyAfterAAEffect(LumaReducePass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::PostResolveHDR; }
    int         Order()    const override { return -100; }
    const char* GetName()  const override { return "EnergyAfterAA"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    LumaReducePass* m_pass = nullptr;
};

class DepthOfFieldEffect final : public IEffect
{
public:
    explicit DepthOfFieldEffect(DepthOfFieldPass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::PostResolveHDR; }
    int         Order()    const override { return 0; }
    const char* GetName()  const override { return "DepthOfField"; }
    bool        ViewSafe() const override { return true; }   // its target is the view's
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    // Accepts a pooled HDR target instead of allocating a private one.
    // Defined out of line: this header only forward-declares the pass types.
    void SetSharedOutput(const RHI::Texture* t) override;
    bool UsesSharedOutput() const override { return true; }
private:
    DepthOfFieldPass* m_pass = nullptr;
};

class CASEffect final : public IEffect
{
public:
    explicit CASEffect(CASPass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::PostResolveHDR; }
    int         Order()    const override { return 200; }
    const char* GetName()  const override { return "CAS"; }
    bool        ViewSafe() const override { return true; }   // its target is the view's
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    // Accepts a pooled HDR target instead of allocating a private one.
    // Defined out of line: this header only forward-declares the pass types.
    void SetSharedOutput(const RHI::Texture* t) override;
    bool UsesSharedOutput() const override { return true; }
private:
    CASPass* m_pass = nullptr;
};

class AutoExposureEffect final : public IEffect
{
public:
    explicit AutoExposureEffect(AutoExposurePass* pass);
    // First in the Resolve stage, ahead of NPRStabilize and the upscalers.
    RG::Stage   GetStage() const override { return m_late ? RG::Stage::PostResolveHDR : RG::Stage::Resolve; }
    int         Order()    const override { return m_late ? 300 : 50; }
    const char* GetName()  const override { return "AutoExposure"; }
    bool        ViewSafe() const override { return true; }   // its exposure history is the view's
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    AutoExposurePass* m_pass = nullptr;
    bool m_late = false;   // harness -ae_late: the old place, after DoF and CAS (a reverse arm)
};

class BloomEffect final : public IEffect
{
public:
    explicit BloomEffect(BloomPass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::PostResolveHDR; }
    int         Order()    const override { return 400; }
    const char* GetName()  const override { return "Bloom"; }
    bool        ViewSafe() const override { return true; }   // its mip chain is the view's
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    BloomPass* m_pass = nullptr;
};

class LensFlareEffect final : public IEffect
{
public:
    explicit LensFlareEffect(LensFlarePass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::PostResolveHDR; }
    int         Order()    const override { return 500; }
    const char* GetName()  const override { return "LensFlare"; }
    bool        ViewSafe() const override { return true; }   // its target and occlusion history are the view's
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    LensFlarePass* m_pass = nullptr;
};

class UnderwaterEffect final : public IEffect
{
public:
    explicit UnderwaterEffect(UnderwaterPass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::PostResolveHDR; }
    int         Order()    const override { return 800; }
    const char* GetName()  const override { return "Underwater"; }
    bool        ViewSafe() const override { return true; }   // its target is the view's; its time advances once a frame
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    // Accepts a pooled HDR target instead of allocating a private one.
    // Defined out of line: this header only forward-declares the pass types.
    void SetSharedOutput(const RHI::Texture* t) override;
    bool UsesSharedOutput() const override { return true; }
private:
    UnderwaterPass* m_pass = nullptr;
};

class StylizeEffect final : public IEffect
{
public:
    explicit StylizeEffect(StylizePass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::PostResolveHDR; }
    int         Order()    const override { return 900; }
    const char* GetName()  const override { return "Stylize"; }
    bool        ViewSafe() const override { return true; }   // its target is the view's
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    // Accepts a pooled HDR target instead of allocating a private one.
    // Defined out of line: this header only forward-declares the pass types.
    void SetSharedOutput(const RHI::Texture* t) override;
    bool UsesSharedOutput() const override { return true; }
private:
    StylizePass* m_pass = nullptr;
};

class ToneMapEffect final : public IEffect
{
public:
    explicit ToneMapEffect(ToneMapPass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::Tonemap; }
    int         Order()    const override { return 0; }
    const char* GetName()  const override { return "Tonemapping"; }
    bool        ViewSafe() const override { return true; }   // its composition buffer is the view's
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
private:
    ToneMapPass* m_pass = nullptr;
};

// The glass shatter's effect (PostTonemap 0) belongs to its render feature:
// Renderer/Features/GlassShatterFeature.cpp.

} // namespace PostProcess
