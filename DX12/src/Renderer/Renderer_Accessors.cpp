#include "Renderer/Renderer.h"

#include "RenderGraph/RenderPass/ToneMapPass.h"

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace) so the
// triple-buffered FrameCB<T> members can be instantiated in the class layout.
// Layouts there MUST stay in sync with the matching HLSL.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;

// Renderer final-output and glass-shatter accessors (one TU per Renderer subsystem;
// the class is declared in Renderer/Renderer.h).

uint64_t Renderer::GetFinalOutputSrvHandle() const
{
    if (m_toneMapPass) return m_toneMapPass->GetFinalOutputSrvHandle();
    return 0;
}

void Renderer::TriggerGlassShatter(float impactU, float impactV)
{
    // The feature owns the pass (Renderer/Features/GlassShatterFeature.cpp).
    m_features.ForEach<IGlassShatter>([&](IGlassShatter& g) { g.Trigger(impactU, impactV); });
}

