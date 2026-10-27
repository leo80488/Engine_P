#pragma once

// The Overlay stage's effects: what the Renderer draws over the finished picture
// on the restore list (graphics queue) - the debug wires, the world-space UI, the
// editor's icons, Frame Generation's two inputs, the screen-space HUD. Adapters
// around passes the Renderer owns, like BuiltinPostProcessEffects.h's; Order()
// fixes their sequence. All own their GPU regions (OwnsRegions), named after the
// pass; the HUD-less capture opens none.
//
// Those that draw into Tonemapping's output, the composition buffer (its state
// is ToneMapPass's), read the state on entry and leave it SHADER_RESOURCE; the
// HUD-less capture puts it back as it found it. What is not an effect stays the
// Renderer's, around the stage: the composition buffer and HDR transitions and
// the Texture / UI / debug target views before it; depth back to DEPTHSTENCIL
// and the HUD draw list's clear after it.

#include "PostProcess/IPostProcessEffect.h"

class ToneMapPass;
class DebugWirePass;
class WorldUIBillboardPass;
class DebugIconPass;
class UIPass;

namespace PostProcess
{

// Debug wires, post-tonemap: no temporal pass drags them by the velocity
// buffer, no exposure or bloom touches them. Borrows depth as a pixel-shader
// SRV for its occlusion test (Context::graph tracks the state).
class DebugWireEffect final : public IEffect
{
public:
    DebugWireEffect(DebugWirePass* pass, ToneMapPass* toneMap) : m_pass(pass), m_toneMap(toneMap) {}
    RG::Stage   GetStage() const override { return RG::Stage::Overlay; }
    int         Order()    const override { return 100; }
    const char* GetName()  const override { return "DebugWire"; }
    bool        ViewSafe() const override { return true; }   // its lines are the scene's, its camera the view's
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
private:
    DebugWirePass* m_pass    = nullptr;
    ToneMapPass*   m_toneMap = nullptr;
};

// World-space UI billboards (built on the game side by ExtractWorldUI).
class WorldUIEffect final : public IEffect
{
public:
    WorldUIEffect(WorldUIBillboardPass* pass, ToneMapPass* toneMap) : m_pass(pass), m_toneMap(toneMap) {}
    RG::Stage   GetStage() const override { return RG::Stage::Overlay; }
    int         Order()    const override { return 200; }
    const char* GetName()  const override { return "WorldUI"; }
    bool        ViewSafe() const override { return true; }   // each view's lane, from its camera
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
private:
    WorldUIBillboardPass* m_pass    = nullptr;
    ToneMapPass*          m_toneMap = nullptr;
};

// The editor's billboard icons (light / camera gizmos); empty in the game.
class DebugIconsEffect final : public IEffect
{
public:
    DebugIconsEffect(DebugIconPass* pass, ToneMapPass* toneMap) : m_pass(pass), m_toneMap(toneMap) {}
    RG::Stage   GetStage() const override { return RG::Stage::Overlay; }
    int         Order()    const override { return 300; }
    const char* GetName()  const override { return "DebugIcons"; }
    bool        ViewSafe() const override { return true; }   // its quads face the view's camera
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
private:
    DebugIconPass* m_pass    = nullptr;
    ToneMapPass*   m_toneMap = nullptr;
};

// Frame Generation's HUD-less colour: the picture after the world-space
// overlays, before the screen-space HUD (world UI moves with the world, so
// warping it is right; the HUD is not). Only while FG runs - and then every
// frame, even one whose HUD is empty. No GPU region.
class FrameGenHudLessEffect final : public IEffect
{
public:
    explicit FrameGenHudLessEffect(ToneMapPass* toneMap) : m_toneMap(toneMap) {}
    RG::Stage   GetStage() const override { return RG::Stage::Overlay; }
    int         Order()    const override { return 400; }
    const char* GetName()  const override { return "FrameGenHudLess"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
private:
    ToneMapPass* m_toneMap = nullptr;
};

// Frame Generation's UI colour + alpha: the HUD drawn a second time, into the
// surface's transparent target, purely as a DLSS-G input - before the HUD's
// own draw, so that stays the last writer of the presented picture.
class FrameGenUILayerEffect final : public IEffect
{
public:
    explicit FrameGenUILayerEffect(UIPass* ui) : m_ui(ui) {}
    RG::Stage   GetStage() const override { return RG::Stage::Overlay; }
    int         Order()    const override { return 500; }
    const char* GetName()  const override { return "FG_UILayer"; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
private:
    UIPass* m_ui = nullptr;
};

// The screen-space HUD into the composition buffer: the main view's list, or a secondary view's
// (laid out at its size) when it shows the HUD (RG::ShowFlag::Hud).
class UIEffect final : public IEffect
{
public:
    UIEffect(UIPass* ui, ToneMapPass* toneMap) : m_ui(ui), m_toneMap(toneMap) {}
    RG::Stage   GetStage() const override { return RG::Stage::Overlay; }
    int         Order()    const override { return 600; }
    const char* GetName()  const override { return "UI"; }
    bool        ViewSafe() const override { return true; }
    bool        IsEnabled(const Context& ctx) const override;
    void        Execute  (Context& ctx) override;
    bool        OwnsRegions() const override { return true; }
private:
    UIPass*      m_ui      = nullptr;
    ToneMapPass* m_toneMap = nullptr;
};

} // namespace PostProcess
