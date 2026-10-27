#pragma once

// PostProcess::Runtime - process-global home for the post-process volume
// system's per-frame state and the non-spatial gameplay override stack.
//
// A global, not an ECS singleton component: the producer is an ECS system
// (PostProcessResolveSystem) but the CONSUMER is the Renderer, outside the ECS.
// Like the other Get() singletons (AssetFS, GuidRegistry, ProfileSystem) it lets
// the editor, Lua and the Renderer reach the same state.
//
//   PostProcessResolveSystem --writes-->   Runtime::resolved  --Publish--> Published(i) --read--> render half
//   Lua / editor             --push/pop--> Runtime::overrides --read--> resolve
//
// The override stack is a separate, non-spatial layer blended on top of the
// volumes: each entry a profile + time envelope, usually at a higher priority
// than any volume.
//
// `resolved` and the override stack belong to the game half. The render half of frame N
// runs beside the game half of frame N+1, whose resolve rewrites `resolved`, so it reads the
// copy the hand-off published for its frame (Published) and Get() reports a call from the
// Render context.

#include "Graphics/ViewSlot.h"          // RHI::kMaxViews
#include "PostProcess/PostProcessProfile.h"
#include "PostProcess/ResolvedPostProcessSettings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace PostProcess
{

// One transient, event-driven override (damage flash, skill filter, low-HP
// desaturate, cutscene grade ...). Holds an inline profile (Overridable, only the
// touched properties) and a fadeIn -> hold -> fadeOut time envelope.
struct PostProcessOverride
{
    uint64_t           id           = 0;        // assigned by PushOverride
    float              priority     = 1000.0f;  // usually > any volume
    float              masterWeight = 1.0f;     // 0..1
    PostProcessProfile profile;                 // inline per-property overrides

    // Time envelope (seconds). hold < 0 means "stay at full until removed".
    float    elapsed = 0.0f;
    float    fadeIn  = 0.2f;
    float    hold    = 1.0f;
    float    fadeOut = 0.5f;

    uint32_t layerMask = 0xFFFFFFFFu;
    char     label[32] = {};
};

// Evaluate the 0..1 envelope weight for an override at its current elapsed time.
float EvaluateOverrideEnvelope(const PostProcessOverride& o);

// True once an override has fully played out (only when hold >= 0).
bool IsOverrideExpired(const PostProcessOverride& o);

// Per-volume debug snapshot, filled during resolve when debugCapture is on so
// the editor overlay can show "which volumes is the camera in and at what
// weight".
struct VolumeHitDebug
{
    char     label[64] = {};
    float    weight    = 0.0f;
    float    priority  = 0.0f;
    bool     isGlobal  = false;
};

class Runtime
{
public:
    // The game half's instance. Reports (once, [SPLITCHECK][RENDERCTX]) a call from the Render
    // context: the render half reads Published.
    static Runtime& Get();

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    // ---- Override stack ops ----
    // Push a copy; assigns and returns a fresh id (also stamped into the copy).
    uint64_t PushOverride(PostProcessOverride ov);
    void     RemoveOverride(uint64_t id);
    void     ClearOverrides() { overrides.clear(); }

    // ---- The frames on either side of the hand-off ----
    // Copies `resolved` into scene buffer @p bufferIndex (0 or 1), the one the frame in flight
    // owns. Main thread, at the game/render hand-off (Renderer::AdvanceSceneWrite).
    void Publish(uint32_t bufferIndex);
    // Whether the scene's meshes and material textures have all arrived (the renderer's game half sets
    // it before the hand-off; Publish carries it). AutoExposure holds a pending snap until it is true.
    void SetSceneSettled(bool settled) { m_sceneSettled = settled; }
    static bool PublishedSceneSettled(uint32_t bufferIndex);

    // ---- Secondary views ----
    // A secondary view's camera gets its own blend (N2: every view used the main camera's). The game half
    // writes slot @p slot's (1 .. kMaxViews - 1) with PostProcess::ResolveAt at the view's camera, once a
    // frame; Publish carries them. A view with none takes the main view's.
    void SetViewResolved(uint32_t slot, const ResolvedPostProcessSettings& r);
    void ClearViewResolved(uint32_t slot);
    // The settings view @p slot of the frame in scene buffer @p bufferIndex renders with.
    static const ResolvedPostProcessSettings& PublishedView(uint32_t bufferIndex, uint32_t slot);
    // The settings the hand-off published into scene buffer @p bufferIndex: what the render half
    // of that frame reads. The game half no longer writes that buffer while the frame renders.
    static const ResolvedPostProcessSettings& Published(uint32_t bufferIndex);

private:
    Runtime() = default;
    static Runtime& Instance();

public:
    // Latest resolved settings for the main view - written each frame by the
    // resolve system, read by the Renderer when filling PostProcess::Context.
    ResolvedPostProcessSettings resolved;

    // Gameplay override stack (non-spatial). Mutated by Lua / the editor, ticked
    // and applied by the resolve system.
    std::vector<PostProcessOverride> overrides;

    // ---- Debug overlay ----
    bool                        debugCapture = false;  // editor toggles this
    std::vector<VolumeHitDebug> debugHits;             // filled when debugCapture

private:
    uint64_t m_nextId = 1;
    ResolvedPostProcessSettings m_published[2];   // by the Renderer's scene buffer
    bool m_sceneSettled = true;
    bool m_publishedSettled[2] = { true, true };
    ResolvedPostProcessSettings m_viewResolved[RHI::kMaxViews];       // [0] unused
    bool                        m_viewResolvedValid[RHI::kMaxViews] = {};
    ResolvedPostProcessSettings m_viewPublished[2][RHI::kMaxViews];
    bool                        m_viewPublishedValid[2][RHI::kMaxViews] = {};
};

} // namespace PostProcess
