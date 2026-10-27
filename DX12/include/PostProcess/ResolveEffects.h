#pragma once

// The Resolve stage's effects: NPRStabilize, RRColorAlign, the upscalers'
// reactive mask, then ONE of FSR 2 / DLSS / TAA, then FXAA. Adapters around passes
// the Renderer owns, like BuiltinPostProcessEffects.h's; Order() fixes their
// sequence, and each opens its own GPU region (OwnsRegions).
//
// While the stage runs, Context::hdrSrv / hdrTex is the AA input: the HDR scene,
// or NPRStabilize's output once it ran. The resolve effects read it and leave it
// alone; after the stage the chain hands the frame's resolved image over
// (IEffect::ClaimsResolve):
//   - FSR 2 when the frame plan resolves through it, else DLSS when the plan
//     resolves through DLSS - even in a frame the upscaler could not run (its
//     output keeps last frame's pixels at the size the pool was made for);
//   - otherwise TAA's output when TAA is on, FXAA's over it when FXAA is on (each
//     only with a live handle);
//   - otherwise the AA input itself.

#include "PostProcess/IPostProcessEffect.h"

class NPRStabilizePass;
class RRColorSnapshot;
class UpscalerReactiveMaskPass;
class FSR2Pass;
class DLSSPass;
class TAAPass;
class FXAAPass;

namespace PostProcess
{

// Scales exposure-protected NPR pixels by rcp(last frame's exposure) before any
// temporal history sees them; its output becomes the AA input.
class NPRStabilizeEffect final : public IEffect
{
public:
    explicit NPRStabilizeEffect(NPRStabilizePass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::Resolve; }
    int         Order()    const override { return 100; }
    const char* GetName()  const override { return "NPRStabilize"; }
    bool        ViewSafe() const override { return true; }   // its output and lit snapshot are the view's
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
private:
    NPRStabilizePass* m_pass = nullptr;
};

// DLSS-RR's colour before transparency, given NPRStabilize's correction
// (RRColorAlign.cs.hlsl) - only in a frame NPRStabilize ran and the snapshot was
// written.
class RRColorAlignEffect final : public IEffect
{
public:
    explicit RRColorAlignEffect(RRColorSnapshot* snapshot) : m_snapshot(snapshot) {}
    RG::Stage   GetStage() const override { return RG::Stage::Resolve; }
    int         Order()    const override { return 150; }
    const char* GetName()  const override { return "RRColorAlign"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
private:
    RRColorSnapshot* m_snapshot = nullptr;
};

// The reactive mask the upscaler reads (the stencil bits TAA floors its history
// weight on, with TAA's weights) - only while FSR 2 or DLSS is selected. Leaves
// it in Context::resources.reactiveMask.
class ReactiveMaskEffect final : public IEffect
{
public:
    ReactiveMaskEffect(UpscalerReactiveMaskPass* pass, TAAPass* taa) : m_pass(pass), m_taa(taa) {}
    RG::Stage   GetStage() const override { return RG::Stage::Resolve; }
    int         Order()    const override { return 200; }
    const char* GetName()  const override { return "ReactiveMask"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
private:
    UpscalerReactiveMaskPass* m_pass = nullptr;
    TAAPass*                  m_taa  = nullptr;
};

// FSR 2: anti-aliasing and the upscale to the display size (AA mode FSR).
class FSR2Effect final : public IEffect
{
public:
    explicit FSR2Effect(FSR2Pass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::Resolve; }
    int         Order()    const override { return 300; }
    const char* GetName()  const override { return "FSR2"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
    bool        ClaimsResolve(const Context& ctx, uint64_t& srv) const override;
private:
    FSR2Pass* m_pass = nullptr;
};

// DLSS Super Resolution / DLAA, and ray reconstruction (DLSS-RR). Structurally
// FSR 2's arm; Streamline takes the camera through the frame constants. The
// one effect that needs the graphics queue: Streamline's own barriers target
// pixel-shader states, illegal on a compute list (RequiresGraphicsQueue).
class DLSSEffect final : public IEffect
{
public:
    explicit DLSSEffect(DLSSPass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::Resolve; }
    int         Order()    const override { return 310; }
    const char* GetName()  const override { return "DLSS"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
    bool        ClaimsResolve(const Context& ctx, uint64_t& srv) const override;
    bool        RequiresGraphicsQueue(const Context& ctx) const override;
private:
    DLSSPass* m_pass = nullptr;
};

// The engine's TAA: the resolve when no upscaler runs, and the fallback of one
// that could not (it does not ask whether an upscaler is selected).
class TAAEffect final : public IEffect
{
public:
    explicit TAAEffect(TAAPass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::Resolve; }
    int         Order()    const override { return 320; }
    const char* GetName()  const override { return "TAA"; }
    bool        ViewSafe() const override { return true; }   // its history is the view's
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
    bool        ClaimsResolve(const Context& ctx, uint64_t& srv) const override;
private:
    TAAPass* m_pass = nullptr;
};

// FXAA: spatial AA, never beside an upscaler (FSR 2 / DLSS anti-alias as they
// upscale). Reads TAA's output when TAA is on (AA mode FXAA+TAA), the AA input
// otherwise.
class FXAAEffect final : public IEffect
{
public:
    FXAAEffect(FXAAPass* pass, TAAPass* taa) : m_pass(pass), m_taa(taa) {}
    RG::Stage   GetStage() const override { return RG::Stage::Resolve; }
    int         Order()    const override { return 400; }
    const char* GetName()  const override { return "FXAA"; }
    bool        ViewSafe() const override { return true; }   // its output is the view's
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
    bool        ClaimsResolve(const Context& ctx, uint64_t& srv) const override;
private:
    FXAAPass* m_pass = nullptr;
    TAAPass*  m_taa  = nullptr;
};

} // namespace PostProcess
