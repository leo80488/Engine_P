#pragma once

// Renderer/FrameView.h - the frame's view state: the camera, the view, the
// sizes and the frame's intent, as the render half reads it
// (Renderer::FrameViewRead). The Renderer double-buffers it; the comment at
// its m_frameViewBuf says how, and which game-half readers want the previous
// frame's. A header of its own so a render feature (Renderer/RenderFeature.h)
// can read it without Renderer.h.

#include "Graphics/RenderTypes.h"   // RenderCamera, RenderView, FrameIndex, RHI::kInvalidBufferIndex
#include "Graphics/ViewSlot.h"      // RHI::kMaxViews
#include "Renderer/FramePlan.h"     // FramePlan::Intent

#include <cstdint>

// A secondary view of the frame (Renderer::SetSecondaryView, game half): the same scene through
// its own camera, at its own size, into its own ViewState - recorded by the same passes after the
// main view.
struct SecondaryFrameView
{
    uint32_t     slot     = 0;   // its ViewState's slot, 1 .. RHI::kMaxViews - 1; 0 = no view
    RenderCamera camera;
    uint32_t     vpWidth  = 0;   // its display size (= render size: no upscaler in a secondary view)
    uint32_t     vpHeight = 0;
    uint32_t     show     = 0;   // the RG::ShowFlag bits it shows
};

struct FrameView
{
    RenderCamera camera;
    RenderView   view;
    uint32_t     vpWidth      = 0;
    uint32_t     vpHeight     = 0;
    uint32_t     renderWidth  = 0;
    uint32_t     renderHeight = 0;
    FrameIndex   currentFrame = 0;
    float        deltaTime    = 0.0f;
    float        globalTimeSec = 0.0f;   // accumulated; tracer noise scroll
    // The world clock's step this frame (Renderer::BeginWorldClock): deltaTime, 0 while the path
    // tracer's time freeze holds, or one step of it. Everything that animates the world reads it -
    // globalTimeSec, time of day, clouds, water, particles, tracers, the shaders' timeParams.y;
    // deltaTime stays the frame's wall time (exposure adaptation, the post chain, overlays).
    float        worldDeltaTime = 0.0f;
    // A frozen world took one step this frame (Lua Render.ptStep): the path tracer restarts.
    bool         worldStepped   = false;
    float        hdrClearColor[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
    // The main view shows the editor's overlays (RG::ShowFlag::EditorOverlays): false while it is the
    // Game View (Renderer::SetMainViewEditorOverlays).
    bool         editorOverlays = true;
    // Prepared by the parked owner; render consumers never register slots.
    uint32_t reflectionMaterialSlot = RHI::kInvalidBufferIndex;
    uint32_t reflectionExposureSlot = RHI::kInvalidBufferIndex;
    // Written by CaptureFrameIntent; read by the render half's plan and by the
    // game half's own decisions.
    FramePlan::Intent planIntent;
    // The debug target producers this frame runs (DebugTargetViewPass::Produced
    // bits), armed at the service point.
    uint32_t debugProduce = 0;
    // This frame's material PSOs take the DEBUG_SHADE permutation (a producer
    // reads what they count) - set at the service point, read by
    // Render_BindFrameResources.
    bool debugShade = false;
    // Why the main view's history breaks at this frame - the HistoryReset bits
    // (RenderGraph/ViewState.h) raised on the game side while this frame was built
    // (Renderer::RaiseViewBreak, any thread; the hand-off, AdvanceSceneWrite, moves
    // them in) and the ones raised loudly (logged; the Sequencer's seeks are
    // quiet). The render half applies them at its start
    // (Render_BindFrameResources); the seed of the next frame's view does not
    // carry them.
    uint32_t viewBreaks     = 0;
    uint32_t viewBreaksLoud = 0;
    // This frame's DLSS constants carried a reset (UploadFrameData); the
    // evaluate clears only a reset its constants carried.
    bool     dlssResetSent  = false;
    // The frame's secondary views, by slot - 1 (an entry with slot 0 is unused). They carry
    // over to the next frame's view until their owner clears them.
    SecondaryFrameView secondary[RHI::kMaxViews - 1];
};
