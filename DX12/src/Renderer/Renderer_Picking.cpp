// Renderer_Picking.cpp - GPU picking: the one-pixel pick (editor, harness)
// and the whole object-ID image.

#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/PickingPass.h"
#include "Graphics/IGraphicsDevice.h"   // FlushAndWait before a readback

bool Renderer::RequestPick(float pixelX, float pixelY, bool forHarness, uint32_t viewSlot)
{
    if (!m_pickingPass) return false;
    if (viewSlot != 0)
    {
        // A secondary view renders at its own size, no upscaler: its pixels are the id buffer's.
        if (!IsViewPickable(viewSlot)) return false;
        m_pickForHarness = forHarness;
        m_pickingPass->RequestPick(static_cast<int>(pixelX), static_cast<int>(pixelY), viewSlot);
        return true;
    }
    m_pickForHarness = forHarness;
    // Pick coords arrive in DISPLAY/viewport pixels, but the ID buffer is
    // rendered at the actual internal render size (FSR and renderScale already
    // applied). Scale so the sampled texel matches the display-space click.
    const float sx = (FrameViewWrite().vpWidth  > 0) ? static_cast<float>(FrameViewWrite().renderWidth)  / static_cast<float>(FrameViewWrite().vpWidth)  : 1.0f;
    const float sy = (FrameViewWrite().vpHeight > 0) ? static_cast<float>(FrameViewWrite().renderHeight) / static_cast<float>(FrameViewWrite().vpHeight) : 1.0f;
    m_pickingPass->RequestPick(static_cast<int>(pixelX * sx),
                               static_cast<int>(pixelY * sy));
    return true;
}

bool Renderer::IsViewPickable(uint32_t viewSlot) const
{
    if (viewSlot >= RHI::kMaxViews) return false;
    const SecondaryView& v = m_secondary[viewSlot];
    return v.live && v.ready && v.renders.load(std::memory_order_relaxed) > 0;
}

bool Renderer::ResolvePick(Entity& outEntity)
{
    if (!m_pickingPass || !m_pickingPass->IsPickPending()) return false;
    m_gfx.FlushAndWait();
    const uint32_t slotValue = m_pickingPass->ReadPickResult();
    m_pickingPass->ClearPickPending();
    m_pickForHarness = false;
    if (slotValue == 0)
    {
        outEntity = NullEntity;
        return true;
    }
    const uint32_t slot = slotValue - 1;
    outEntity = (slot < m_maxInstances) ? m_instanceSlotToEntity[slot] : NullEntity;
    return true;
}

bool Renderer::IsPickBusy() const
{
    return m_pickingPass && (m_pickingPass->IsPickRequested() || m_pickingPass->IsPickPending());
}

bool Renderer::IsPickReadable() const
{
    return m_pickingPass && m_pickingPass->IsPickPending();
}

void Renderer::CancelHarnessPick()
{
    if (!m_pickForHarness) return;
    if (m_pickingPass) m_pickingPass->CancelPick();
    m_pickForHarness = false;
}

bool Renderer::RequestPickImage(uint32_t viewSlot)
{
    if (viewSlot != 0 && !IsViewPickable(viewSlot)) return false;
    return m_pickingPass && m_pickingPass->RequestImage(viewSlot);
}

bool Renderer::IsPickImageBusy() const
{
    return m_pickingPass && (m_pickingPass->IsImageRequested() || m_pickingPass->IsImagePending());
}

bool Renderer::IsPickImageReadable() const
{
    return m_pickingPass && m_pickingPass->IsImagePending();
}

bool Renderer::ResolvePickImage(std::vector<std::uint32_t>& outSlots, std::uint32_t& outW, std::uint32_t& outH)
{
    if (!m_pickingPass || !m_pickingPass->IsImagePending()) return false;
    m_gfx.FlushAndWait();
    return m_pickingPass->ReadImage(outSlots, outW, outH);
}

Entity Renderer::PickSlotEntity(std::uint32_t slotPlusOne) const
{
    if (slotPlusOne == 0) return NullEntity;
    const std::uint32_t slot = slotPlusOne - 1;
    return (slot < m_maxInstances) ? m_instanceSlotToEntity[slot] : NullEntity;
}

void Renderer::CancelPickImage()
{
    if (m_pickingPass) m_pickingPass->CancelImage();
}
