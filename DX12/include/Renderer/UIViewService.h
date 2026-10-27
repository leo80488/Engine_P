#pragma once

// UIViewService - off-screen pictures of Canvas UI subtrees: the UI Editor's designer
// view and the harness's -ui_render. Owned by the Renderer (Renderer::GetUIViews()),
// EngineCore, usable from either exe; nothing is built until a view is first asked
// for, and Game.exe pays one branch a frame for it.
//
// Shaped like TextureViewService, plus one context it does not have:
//   * MAIN thread (UI handlers, Lua, PPA): Acquire / Request / Release only latch an
//     intent under the mutex. LayoutDesc lays a description out now for a panel's
//     overlay or a layout oracle (design pixels, the same kernel and inputs the paint
//     uses) - static, no view needed.
//   * GAME half, UI paint window (PaintPending, at the end of UIFramePainter::Paint):
//     lays out and paints every live view into its own UIDrawList, after this frame's
//     UIImage SRV re-resolve and before FontLibrary::FlushPendingUploads - glyphs bake
//     here, which is legal only in that window. Never writes UIComputedRect nor runs
//     the interaction pass (both belong to the game screen).
//   * SERIAL window (ServiceFrameBoundary, from Renderer::ServiceEnableGatedResources,
//     render lane parked): applies the latched intents, builds the second UIPass on
//     first use, creates / resizes / destroys RGBA8 targets (frame-stamped
//     DestroyTexture, so a descriptor ImGui captured survives).
//   * RENDER half (Execute, restoreCL beside the texture views): clears and draws at
//     most one view a frame - the second UIPass has one vertex-ring slot and one
//     constant-buffer slot per frame, so a second Execute in the frame would overwrite
//     the first. A fresh target always draws in its first frame (ImGui must never
//     sample an unwritten one); otherwise only a view whose draw list changed (content
//     hash), so an idle designer records nothing.
//
// GPU order: ImGui records into primaryCL, which depends on restoreCL, so the picture
// is complete before the same frame's ImGui::Image samples it.

#include "Renderer/UIView.h"
#include "Graphics/GraphicsStruct.h"   // RHI::Texture, RHI::CommandList
#include "UI/UIDrawList.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class IGraphicsDevice;
class UIPass;
class World;

class UIViewService
{
public:
    static constexpr std::uint32_t kMaxViews  = 16;
    static constexpr std::uint32_t kInvalidId = ~0u;
    static constexpr std::uint32_t kMaxSize   = 4096;   // per target edge

    explicit UIViewService(IGraphicsDevice& gfx);
    ~UIViewService();
    UIViewService(const UIViewService&) = delete;
    UIViewService& operator=(const UIViewService&) = delete;

    // ---- Main thread ----
    std::uint32_t Acquire();                                  // kInvalidId when all are taken
    void          Request(std::uint32_t id, const UIView::Desc& desc);
    void          Release(std::uint32_t id);
    // The target's SRV for ImGui::Image; 0 until it exists. Re-resolve every frame.
    std::uint64_t Srv(std::uint32_t id) const;
    // The target (the harness's CaptureTextureToPNG, serial window); nullptr if none.
    const RHI::Texture* Output(std::uint32_t id) const;
    UIView::Status GetStatus(std::uint32_t id) const;
    std::uint32_t  LiveCount() const;
    std::uint64_t  TargetBytes() const;
    // The screen the GAME's Canvas UI was last laid out for (the main viewport,
    // as UIFramePainter saw it); {0, 0} before the first paint. A designer's
    // "match main viewport" resolution reads it.
    UI::Vec2       MainScreen() const;

    // The subject of @p d laid out at its design size by the game screen's
    // kernel (UI/UICanvasLayout.h): the nodes in DESIGN pixels and the canvas
    // scale. What PaintPending paints, and what an editor's overlay and layout
    // oracle read - no view, no device. False with @p why when the subject cannot
    // be laid out (gone, another world epoch, no UICanvas / UIRect, no size).
    // Resolving parentGuid edges writes UIParent.parent, the value the game
    // screen's own resolve writes, so it is idempotent.
    static bool LayoutDesc(const UIView::Desc& d, World& world, std::vector<UI::LayoutNode>& out,
                           float& canvasScale, std::string& why);

    // ---- Game half: UI paint window ----
    // @p mainScreen: the screen the game's own Canvas paint used this frame.
    void PaintPending(World& world, float realDt, const UI::Vec2& mainScreen);

    // ---- Serial window ----
    void ServiceFrameBoundary();
    void ReloadShaders();
    void Shutdown();

    // ---- Render half ----
    void Execute(RHI::CommandList cl, std::uint64_t frame);

private:
    struct View
    {
        // Main thread, under m_mutex.
        bool          live = false;
        bool          releasePending = false;
        bool          hasRequest = false;
        UIView::Desc  requested;

        // Serial window (the render half reads it, parked meanwhile).
        RHI::Texture  target;
        std::uint64_t targetBytes = 0;
        bool          fresh = false;             // made this boundary: draws this frame

        // Game half (main thread; the render half reads it after SubmitFrame).
        std::vector<UI::LayoutNode> nodes;
        UI::TextWrapCache wrap;
        UI::UIDrawList drawList;
        float          clock = 0.f;               // the view's UI clock (jitter, custom shaders)
        float          canvasScale = 1.f;
        std::uint64_t  paintedHash = 0;           // view pixels + target + clear colour (+ clock)
        bool           needsRecord = false;
        std::uint32_t  clearRgba = 0;
        bool           hasDrawList = false;

        // Render half writes, main thread reads - under m_statusMutex.
        std::uint64_t  recordedHash = 0;
        std::uint64_t  renders = 0;
        std::uint64_t  lastRenderFrame = 0;
        UIView::Status paintStatus;               // the game half's last numbers
    };

    void PaintView(View& v, const UIView::Desc& d, World& world, float realDt);
    bool EnsureTarget(View& v, std::uint32_t w, std::uint32_t h, std::uint32_t id);
    void DestroyTarget(View& v);
    bool EnsureChecker();

    IGraphicsDevice&                 m_gfx;
    std::unique_ptr<UIPass>          m_pass;           // built on the first live view, kept
    RHI::Texture                     m_checker;        // 2x2 backdrop tile
    std::uint64_t                    m_checkerSrv = 0;
    mutable std::mutex               m_mutex;
    mutable std::mutex               m_statusMutex;
    std::vector<View>                m_views;
    bool                             m_anyLive = false; // serial window's view of it
    UI::Vec2                         m_mainScreen{ 0.f, 0.f };   // under m_statusMutex
};
