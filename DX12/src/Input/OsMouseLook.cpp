#include "Input/OsMouseLook.h"

#include "Platform/PointerState.h"

void OsMouseLook::Sample(Plat::PointerState& ms, bool uiCaptures, bool relative, float& mouseLookDX,
                         float& mouseLookDY, bool& mouseLookHeld)
{
    // Consumed every frame, used or not: raw motion accumulates in the pointer
    // until someone takes it.
    const auto raw = ms.ConsumeRawDelta();
    const auto mp  = ms.GetPos();

    if (relative)
    {
        // Captured cursor: look is always on, from unaccelerated raw counts
        // (one count is about one pixel at the default pointer speed, so the
        // camera's sensitivity carries over).
        mouseLookHeld = !uiCaptures;
        mouseLookDX   = mouseLookHeld ? static_cast<float>(raw.first)  : 0.f;
        mouseLookDY   = mouseLookHeld ? static_cast<float>(raw.second) : 0.f;
    }
    else
    {
        const bool rmb = ms.RightIsPressed() && !uiCaptures;
        if (rmb && m_prevMouseValid)
        {
            mouseLookDX = static_cast<float>(mp.first  - m_prevMouseX);
            mouseLookDY = static_cast<float>(mp.second - m_prevMouseY);
        }
        else
        {
            mouseLookDX = 0.f;
            mouseLookDY = 0.f;
        }
        mouseLookHeld = rmb;
    }
    // Tracked in both modes, so leaving captured mode does not jump by the
    // distance the (hidden) pointer moved meanwhile.
    m_prevMouseX     = mp.first;
    m_prevMouseY     = mp.second;
    m_prevMouseValid = true;
}
