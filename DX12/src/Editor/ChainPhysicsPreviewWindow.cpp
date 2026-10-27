// ChainPhysicsPreviewWindow.cpp - the Chain Physics Preview panel: StageClock,
// the harness setters, Draw (UI half) and ProcessPendingActions (game half: the
// stage, the view, the clock). See the header for the contract, the pollution
// paths and the side-effect discipline. One StageClock drives both the clip and
// the chain solver's preview lane (the stage simulates in Edit mode); step-less
// frames and Pause hold the simulated pose.

#include "Editor/ChainPhysicsPreviewWindow.h"

#include "ECS/AnimationComponents.h"        // SkeletonComponent / MeshSkinnedComponent / Animator*
#include "ECS/AnimEntityResolve.h"          // Anim::ResolveAnimEntityOrSelf (the Animator owner)
#include "Resource/AnimationClipSystem.h"   // alias registration + clip read-out
#include "Resource/AssetFS.h"               // EnumerateLoose: the clip scan over every search root
#include "Resource/AssetGuid.h"             // AssetGuidIndex::ContentRoots (clip scan)
#include "ECS/CameraStackComponents.h"      // VirtualCameraComponent
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"        // tags, transforms, Visibility, WorldAabb, MeshLibRef
#include "ECS/HierarchyOps.h"               // Hierarchy::DestroySubtree (World::DestroyEntity is FLAT)
// ---- the strip list's component types ----
#include "AI/AIComponents.h"                // AIComponent
#include "Audio/AudioComponents.h"          // Audio::AudioSourceComponent
#include "ECS/CharacterControllerComponent.h"
#include "ECS/CharacterIntentComponent.h"   // BrainComponent
#include "ECS/HairShadowCasterComponent.h"
#include "ECS/LocomotionComponents.h"       // LocomotionComponent
#include "ECS/PerceptionComponent.h"        // PerceptionComponent / PerceptibleComponent
#include "ECS/PerObjectShadowComponent.h"
#include "ECS/PhysicsComponents.h"          // ColliderComponent / RigidBodyComponent
#include "ECS/PlayerComponent.h"
#include "Nav/NavComponents.h"              // NavAgentComponent
#include "Scripting/ScriptComponent.h"
// animation writers and VFX the preview must not run
#include "ECS/CharacterAnimDriver.h"        // CharacterAnimDriverComponent
#include "ECS/HeadTrackComponent.h"
#include "ECS/ParticleComponent.h"          // ParticleEmitterComponent / ParticleRenderComponent
#include "ECS/TrailComponent.h"
// ----
#include "Editor/Undo/ComponentBlob.h"      // SerializeComponentBlob (staged-dirty test)
#include "Reflection/ComponentReflection.h" // -chainpreview_param field lookup
#include "Resource/ComponentSerializers.h"  // GetComponentRegistry
#include "Graphics/AnimProfile.h"           // the cost sentinel (chainCpuMs)
#include "Graphics/GraphicsDX12.h"          // QueryVramUsage (harness census only)
#include "Graphics/SkinningBuffers.h"       // PoseRingBuffer / SkinnedVertexRing caps
#include "Harness/ExitCodes.h"              // Harness::PreconditionFail
#include "Physics/ChainPhysicsSystem.h"
#include "Physics/PhysicsSystem.h"          // GetBodyCount (harness census only)
#include "RenderGraph/RenderPass/SkinningPass.h"   // SkinningPass::kMaxSkinJobs
#include "Renderer/Preview/PreviewStudioRig.h"     // Preview::StudioCameraBasis / kStudioCamera
#include "Renderer/Preview/PreviewTypes.h"
#include "Renderer/Renderer.h"
#include "Resource/PrefabSerializer.h"      // LoadPrefab / SavePrefab
#include "Resource/ProceduralMesh.h"        // PrimitiveMeshType::Plane
#include "Scene/MeshSpawner.h"              // the floor plane
#include "Resource/SkeletonAsset.h"
#include "System/Check.h"
#include "System/CommandLine.h"             // -panelvisibleverify
#include "System/Log.h"
#include "System/PathUtil.h"                // Sys::WritableDataDir (in-place clone file)

#include "Editor/ImGuiFlow.h"               // flowing rows / wrapped text
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"           // settings handler, overflow read-out

#include <DirectXMath.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace Editor
{
namespace
{
    // A WorldCull lane recompile destroys and recreates seven physical textures,
    // so render sizes move in 16 px steps and the commit is debounced
    // (CameraPreviewWindow's numbers, so dragging either panel behaves the same).
    constexpr double        kResizeDebounceSeconds = 0.2;
    constexpr std::uint32_t kMinW = 192, kMaxW = 960;
    // Editor frames after the stage spawns before the "open" census is taken:
    // long enough for BuildRenderScene to rebuild the per-object shadow set
    // and for the stage's skinned registration to settle.
    constexpr std::uint64_t kCensusOpenDelay = 30;
    // Editor frames after the close before the "post" census: the view's
    // release, the WorldCull GraphSet teardown and kFrameCount-deferred buffer
    // releases all have to have happened.
    constexpr std::uint64_t kCensusPostDelay = 60;
    // Panel frames a harness-requested subject may take to resolve before the
    // run is refused (AssetPreviewWindow's rule).
    constexpr std::uint32_t kHarnessDeadline = 120;

    // Clip-editor frames after a clip is assigned before a HARNESS clip that
    // never became the stage's current clip is a precondition failure (the
    // panel would show the rest pose under the pack's name).
    constexpr std::uint64_t kClipDeadline = 240;

    // QuantiseWidth / HeightFor (width only, 16:9) serve only
    // -chainpreview_layout_fixed; the normal layout sizes from both axes
    // (DrawViewArea).
    std::uint32_t QuantiseWidth(float avail)
    {
        std::uint32_t w = static_cast<std::uint32_t>(std::max(avail, 0.f));
        w = (w / 16u) * 16u;
        return std::clamp(w, kMinW, kMaxW);
    }
    std::uint32_t HeightFor(std::uint32_t w)
    {
        const std::uint32_t h = (w * 9u) / 16u;
        return (h < 2u) ? 2u : (h & ~1u);
    }

    template <class T>
    std::uint32_t PoolSize(World& w)
    {
        auto* p = w.GetPool<T>();
        return p ? static_cast<std::uint32_t>(p->Size()) : 0u;
    }

    std::string Stem(const std::string& path)
    {
        const std::size_t slash = path.find_last_of("/\\");
        std::string s = (slash == std::string::npos) ? path : path.substr(slash + 1);
        const std::size_t dot = s.find_last_of('.');
        return (dot == std::string::npos) ? s : s.substr(0, dot);
    }

    bool EndsWithIpfb(const std::string& p)
    {
        if (p.size() < 5) return false;
        std::string e = p.substr(p.size() - 5);
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".ipfb";
    }

    bool HarnessVerifyOn()
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        return Sys::CommandLine::Get().Has("-panelvisibleverify");
#else
        return false;
#endif
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool EndsWithIanim(const std::string& p)
    {
        return p.size() >= 6 && Lower(p.substr(p.size() - 6)) == ".ianim";
    }

    // AnimClipData::BindToSkeleton's own hash (AnimationResource.h): the
    // "matched X / Y channels" read-out has to count exactly what the bind
    // will match, not an approximation of it.
    std::uint32_t Fnv32(const char* s)
    {
        std::uint32_t h = 2166136261u;
        for (; *s; ++s) h = (h ^ static_cast<std::uint8_t>(*s)) * 16777619u;
        return h;
    }

    // AnimationClipSystem's sub-clip selector rules (ResolveSourceClipIndex):
    // empty = the first clip, "#N" = index N, otherwise the clip's name.
    int SourceClipIndex(const std::vector<Resource::AnimClipData>& clips,
                        const std::string& selector)
    {
        if (clips.empty()) return -1;
        if (selector.empty()) return 0;
        if (selector.size() > 1 && selector[0] == '#')
        {
            char* end = nullptr;
            const unsigned long n = std::strtoul(selector.c_str() + 1, &end, 10);
            if (end && *end == '\0' && n < clips.size()) return static_cast<int>(n);
        }
        for (std::size_t i = 0; i < clips.size(); ++i)
            if (selector == clips[i].name) return static_cast<int>(i);
        return -1;
    }

    // ---- -chainpreview_param ----
    // kind: 'f' float, 'i' int, 'b' bool, '3' float3 (value "x:y:z"), 's' a char
    // array (rootBone / name - moving a group's root is the one structural edit
    // that is a string). ChainGroupDef has no REFLECT descriptor, so its settable
    // members are listed here; `structural` = the change re-initialises the chains
    // (it must Invalidate).
    struct GroupField { const char* name; std::size_t off; char kind; bool structural;
                        std::size_t cap = 0; };
#define CPV_GF(m, k, s) { #m, offsetof(ChainGroupDef, m), k, s }
#define CPV_GS(m, s) { #m, offsetof(ChainGroupDef, m), 's', s, sizeof(ChainGroupDef::m) }
    const GroupField kGroupFields[] = {
        CPV_GS(rootBone, true),            CPV_GS(name, false),
        CPV_GF(enabled, 'b', true),        CPV_GF(excludeSubtree, 'b', true),
        CPV_GF(ringGroup, 'i', true),      CPV_GF(ringIndex, 'i', true),
        CPV_GF(pairGroupId, 'i', true),    CPV_GF(ovrEnabled, 'b', false),
        CPV_GF(ovrDamping, 'f', false),    CPV_GF(ovrGravity, 'f', false),
        CPV_GF(ovrStructuralStiffness, 'f', false),
        CPV_GF(ovrRingStiffness, 'f', false), CPV_GF(ovrShearStiffness, 'f', false),
        CPV_GF(ovrMaxVelocity, 'f', false),   CPV_GF(ovrLocalStiffness, 'f', false),
        CPV_GF(ovrHorizDecay, 'f', false),    CPV_GF(ovrTipHold, 'f', false),
        CPV_GF(ovrCollisionPadding, 'f', false), CPV_GF(ovrContactFriction, 'f', false),
        CPV_GF(ovrLimitAngle, 'f', false),    CPV_GF(ovrLimitTipScale, 'f', false),
        CPV_GF(ovrInertiaScale, 'f', false),  CPV_GF(ovrWindStrength, 'f', false),
        CPV_GF(ovrWindFreq, 'f', false),      CPV_GF(collide, 'b', false),
        CPV_GF(ovrSpringStiffness, 'f', false), CPV_GF(ovrSpringDamping, 'f', false),
        CPV_GF(ovrSpringMass, 'f', false),    CPV_GF(ovrSpringGravity, 'f', false),
        CPV_GF(ovrSpringMaxDisp, 'f', false), CPV_GF(ovrSpringChildStiffness, 'f', false),
        CPV_GF(ovrSpringChildDamping, 'f', false), CPV_GF(ovrSpringChildMass, 'f', false),
        CPV_GF(ovrSpringChildGravity, 'f', false), CPV_GF(ovrSpringChildMaxDisp, 'f', false),
        CPV_GF(ovrWorldInertiaLocation, 'f', false), CPV_GF(ovrWorldInertiaRotation, 'f', false),
    };
#undef CPV_GF
#undef CPV_GS

    // "g12_ovrDamping" -> (12, "ovrDamping"); false for a top-level key.
    bool SplitGroupKey(const std::string& key, int& idx, std::string& member)
    {
        if (key.size() < 4 || key[0] != 'g' ||
            !std::isdigit(static_cast<unsigned char>(key[1]))) return false;
        std::size_t i = 1;
        while (i < key.size() && std::isdigit(static_cast<unsigned char>(key[i]))) ++i;
        if (i >= key.size() || key[i] != '_' || i - 1 > 3) return false;
        idx    = std::atoi(key.substr(1, i - 1).c_str());
        member = key.substr(i + 1);
        return !member.empty();
    }

    const GroupField* FindGroupField(const std::string& member)
    {
        for (const GroupField& g : kGroupFields)
            if (member == g.name) return &g;
        return nullptr;
    }

    // A top-level ChainPhysicsComponent field by its REFLECT name, with the
    // storage kind the harness can write.
    const Reflect::FieldDescriptor* FindTopField(const std::string& key, char& kind)
    {
        for (const Reflect::FieldDescriptor& f : Reflect::Describe<ChainPhysicsComponent>().fields)
        {
            if (key != f.name) continue;
            switch (f.kind)
            {
            case Reflect::FieldKind::F32:   kind = 'f'; return &f;
            case Reflect::FieldKind::I32:   kind = 'i'; return &f;
            case Reflect::FieldKind::Bool:  kind = 'b'; return &f;
            case Reflect::FieldKind::F32x3: kind = '3'; return &f;
            default: return nullptr;
            }
        }
        return nullptr;
    }

    // The two rest-pose scalars are not REFLECT fields (the tree draws them in
    // its own section), but a staged rest-pose edit is what the
    // Apply-to-instances gate needs.
    bool RestPoseKey(const std::string& key, char& kind)
    {
        if (key == "restPoseWeight")  { kind = 'f'; return true; }
        if (key == "restPoseEnabled") { kind = 'b'; return true; }
        return false;
    }

    bool ParseParamValue(char kind, const std::string& s, float out[3])
    {
        auto num = [](const std::string& t, float& o)
        {
            if (t.empty()) return false;
            char* end = nullptr;
            const double d = std::strtod(t.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(d)) return false;
            o = static_cast<float>(d);
            return true;
        };
        switch (kind)
        {
        case 'b':
            if (s == "1" || s == "true")  { out[0] = 1.f; return true; }
            if (s == "0" || s == "false") { out[0] = 0.f; return true; }
            return false;
        case 'i':
        {
            char* end = nullptr;
            const long v = std::strtol(s.c_str(), &end, 10);
            if (s.empty() || !end || *end != '\0') return false;
            out[0] = static_cast<float>(v);
            return true;
        }
        case 'f': return num(s, out[0]);
        case 's': return !s.empty();   // stored by the caller (StoreParam has no string)
        case '3':
        {
            const std::size_t a = s.find(':'), b = (a == std::string::npos)
                                                   ? std::string::npos : s.find(':', a + 1);
            if (a == std::string::npos || b == std::string::npos) return false;
            return num(s.substr(0, a), out[0]) && num(s.substr(a + 1, b - a - 1), out[1]) &&
                   num(s.substr(b + 1), out[2]);
        }
        default: return false;
        }
    }

    void StoreParam(char kind, void* p, const float v[3])
    {
        switch (kind)
        {
        case 'b': *static_cast<bool*>(p)  = v[0] != 0.f; break;
        case 'i': *static_cast<int*>(p)   = static_cast<int>(v[0]); break;
        case 'f': *static_cast<float*>(p) = v[0]; break;
        case '3': *static_cast<DirectX::XMFLOAT3*>(p) = { v[0], v[1], v[2] }; break;
        default: break;
        }
    }
}

// ==== StageClock - fixed step + bounded catch-up ====
std::uint32_t StageClock::Advance(float realDt, float* outStepDt)
{
    ++frames;
    // Pause, a pinned count and a Step all run steps of the FIXED size; "follow
    // editor" has none, so those fall back to the default 1/60.
    const float fixedStep = fixedDt > 0.f ? fixedDt : ChainPreview::kFixedDt;
    *outStepDt = fixedStep;
    // Step wins over everything (it is how a PAUSED stage advances one step).
    if (stepOnce) { stepOnce = false; return 1; }
    if (paused)   { ++pausedFrames; return 0; }
    // Harness pin: the exact count, cycled; realDt and timeScale ignored.
    if (!pinSteps.empty())
    {
        const std::uint32_t n = pinSteps[pinCursor % pinSteps.size()];
        ++pinCursor;
        return std::min(n, ChainPreview::kMaxCatchupSteps);
    }
    float frameDt = realDt;
    if (!feedDt.empty())   // harness pacing: a synthetic REAL frame time
    {
        frameDt = feedDt[feedCursor % feedDt.size()];
        ++feedCursor;
    }
    const float dt = (std::isfinite(frameDt) && frameDt > 0.f) ? frameDt : 0.f;
    const float ts = (std::isfinite(timeScale) && timeScale > 0.f) ? timeScale : 0.f;
    if (fixedDt <= 0.f)
    {
        // "Follow editor": one variable step. The solver clamps dt to 1/30
        // (Simulate and PrepareSubsteps), so the clock clamps it first - the clip
        // then advances by exactly what the chains integrate, and the frame counts
        // as dropped time.
        float step = dt * ts;
        if (step > 1.f / 30.f) { step = 1.f / 30.f; ++droppedSteps; }
        *outStepDt = step;
        return step > 0.f ? 1u : 0u;
    }
    accumulator += dt * ts;
    std::uint32_t n = static_cast<std::uint32_t>(accumulator / fixedDt);
    if (n > ChainPreview::kMaxCatchupSteps)
    {
        // THE EXCESS IS DROPPED, not banked: a hitch must not turn into a
        // burst of fast-forward on the next frames.
        droppedSteps += n - ChainPreview::kMaxCatchupSteps;
        n = ChainPreview::kMaxCatchupSteps;
        accumulator = 0.f;
    }
    else
        accumulator -= static_cast<float>(n) * fixedDt;
    return n;
}

void StageClock::Commit(std::uint32_t n, float stepDt)
{
    totalSteps += n;
    if (n == 0) ++zeroStepFrames;
    else if (paused) stepsWhilePaused += n;
    lastSteps = n;
    maxStepsPerFrame = std::max(maxStepsPerFrame, n);
    stageTime += static_cast<double>(n) * static_cast<double>(stepDt);
}

ChainPhysicsPreviewWindow& ChainPhysicsPreviewWindow::Get()
{
    static ChainPhysicsPreviewWindow s_instance;
    return s_instance;
}

// ==== Harness setters ====
bool ChainPhysicsPreviewWindow::HarnessSetSubject(const std::string& path)
{
    if (path.empty()) return false;
    m_harnessSubject     = path;
    m_pendingSubject     = path;
    m_pendingSubjectInPlace = false;
    m_subjectFromHarness = true;
    // -chainpreview_open_at defers the OPEN (so the isolation census's "pre"
    // reading is taken on a settled scene); without it the panel opens now.
    m_open               = (m_harnessOpenAt == 0);
    // A harness run's stage clock is pinned to one fixed step per editor frame
    // unless -chainpreview_steps / -chainpreview_realdt said otherwise (App sets
    // those first): the packs' frame numbers depend on it (a capture's clip time
    // and step count are a function of its frame number, not of wall time).
    if (!m_harnessPacingSet) m_clock.pinSteps = { 1u };
    // The stats box carries live timings: a harness subject shows it only when
    // -chainpreview_stats asked (App parses that first).
    if (!m_harnessStatsSet) m_showStats = false;
    // "selected:<name>[#k]" = Use Selected on that scene entity (an in-place
    // subject), resolved on the first frame the panel runs with a World.
    static const std::string kSel = "selected:";
    if (path.rfind(kSel, 0) == 0)
    {
        if (path.size() == kSel.size()) return false;
        m_harnessUseSelected = path.substr(kSel.size());
        m_pendingSubject.clear();
        m_subjectFromHarness = false;
    }
    std::snprintf(m_subjectEdit, sizeof(m_subjectEdit), "%s", path.c_str());
    LOG_INFO("[CHAINPREVIEW] harness: subject '%s' (panel opened by "
             "-chainpreview_subject)", path.c_str());
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetSize(std::uint32_t w, std::uint32_t h)
{
    if (w < 16 || h < 16 || w > 4096 || h > 4096) return false;
    m_harnessW = w; m_harnessH = h;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetFrames(std::uint32_t n)
{
    if (n == 0) return false;
    m_harnessFrames = n;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetOpenAt(std::uint64_t editorFrame)
{
    if (editorFrame == 0) return false;
    m_harnessOpenAt = editorFrame;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetCloseAt(std::uint64_t editorFrame)
{
    if (editorFrame == 0) return false;
    m_harnessCloseAt = editorFrame;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetClip(const std::string& nameOrPath)
{
    if (nameOrPath.empty()) return false;
    m_harnessClip = nameOrPath;
    m_harnessClipResolved = false;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetTime(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 36000.0) return false;
    m_harnessTime    = seconds;
    m_harnessTimeSet = true;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetDt(float seconds)
{
    // 0 is a VALUE (the lane never runs - the reverse arm), not an error. Above
    // 1/30 the solver would clamp it silently (Simulate and PrepareSubsteps
    // both take min(dt, 1/30)), so it is refused rather than half-honoured.
    if (!std::isfinite(seconds) || seconds < 0.f || seconds > (1.f / 30.f) + 1e-6f)
        return false;
    // A positive value is the stage clock's fixed step (the dt dropdown); 0 keeps
    // the clip clock running and never runs the lane.
    if (seconds > 0.f) m_clock.fixedDt = seconds;
    else               m_harnessLaneOff = true;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetTimeScale(float scale)
{
    if (!std::isfinite(scale) || scale < 0.1f - 1e-6f || scale > 2.f + 1e-6f) return false;
    m_clock.timeScale = std::clamp(scale, 0.1f, 2.f);
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetSteps(const std::vector<std::uint32_t>& steps)
{
    if (steps.empty()) return false;
    for (std::uint32_t n : steps)
        if (n > ChainPreview::kMaxCatchupSteps) return false;
    m_clock.pinSteps = steps;
    m_clock.feedDt.clear();
    m_harnessPacingSet = true;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetRealDt(const std::vector<float>& realDt)
{
    if (realDt.empty()) return false;
    for (float s : realDt)
        if (!std::isfinite(s) || s <= 0.f || s > 1.f) return false;
    m_clock.feedDt = realDt;
    m_clock.pinSteps.clear();
    m_harnessPacingSet = true;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetPauseAt(std::uint64_t editorFrame)
{
    if (editorFrame == 0) return false;
    m_harnessPauseAt = editorFrame;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetStepAt(std::uint64_t editorFrame)
{
    if (editorFrame == 0) return false;
    m_harnessStepAt = editorFrame;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetAb(double seconds,
                                             const std::vector<AbPacing>& extraWindows)
{
    if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > 60.0) return false;
    if (extraWindows.size() > 3) return false;
    for (const AbPacing& p : extraWindows)
    {
        if (!p.steps.empty() && !p.realDt.empty()) return false;
        for (std::uint32_t n : p.steps)
            if (n > ChainPreview::kMaxCatchupSteps) return false;
        for (float s : p.realDt)
            if (!std::isfinite(s) || s <= 0.f || s > 1.f) return false;
    }
    m_abSeconds = seconds;
    // [0] = window a (filled when it starts); none given = one window b that
    // repeats a's pacing (the determinism case).
    m_abPacing.assign(1, AbPacing{});
    if (extraWindows.empty()) m_abPacing.push_back(AbPacing{});
    else m_abPacing.insert(m_abPacing.end(), extraWindows.begin(), extraWindows.end());
    m_abWin.assign(m_abPacing.size(), AbWindow{});
    m_abPhase = AbPhase::Wait;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetResetAt(std::uint64_t editorFrame)
{
    if (editorFrame == 0) return false;
    m_harnessResetAt = editorFrame;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetDump(const std::string& path)
{
    if (path.empty()) return false;
    m_harnessDump = path;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddParam(const std::string& key,
                                                const std::string& value)
{
    return HarnessAddParamAt(key, value, 0);
}

bool ChainPhysicsPreviewWindow::HarnessAddParamAt(const std::string& key,
                                                  const std::string& value,
                                                  std::uint64_t editorFrame)
{
    // Syntax + key here (App turns false into ENGINE_CHECK + "ignored"); the
    // group INDEX can only be checked against the stage, when it is applied.
    float v[3] = {};
    char kind = 0;
    int gi = -1;
    std::string member;
    if (SplitGroupKey(key, gi, member))
    {
        const GroupField* gf = FindGroupField(member);
        if (!gf || !ParseParamValue(gf->kind, value, v)) return false;
    }
    else if (RestPoseKey(key, kind))
    {
        if (!ParseParamValue(kind, value, v)) return false;
    }
    else if (!FindTopField(key, kind) || !ParseParamValue(kind, value, v))
        return false;
    HarnessParam hp;
    hp.key   = key;
    hp.value = value;
    hp.at    = editorFrame;
    m_harnessParams.push_back(std::move(hp));
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetUndoAt(const std::vector<std::uint64_t>& frames)
{
    for (std::uint64_t f : frames) if (f == 0) return false;
    m_harnessUndoAt = frames;
    std::sort(m_harnessUndoAt.begin(), m_harnessUndoAt.end());
    m_harnessUndoNext = 0;
    return !m_harnessUndoAt.empty();
}

bool ChainPhysicsPreviewWindow::HarnessSetRedoAt(const std::vector<std::uint64_t>& frames)
{
    for (std::uint64_t f : frames) if (f == 0) return false;
    m_harnessRedoAt = frames;
    std::sort(m_harnessRedoAt.begin(), m_harnessRedoAt.end());
    m_harnessRedoNext = 0;
    return !m_harnessRedoAt.empty();
}

bool ChainPhysicsPreviewWindow::HarnessSetFullResetAt(const std::vector<std::uint64_t>& frames)
{
    for (std::uint64_t f : frames) if (f == 0) return false;
    m_harnessFullResetAt = frames;
    std::sort(m_harnessFullResetAt.begin(), m_harnessFullResetAt.end());
    m_harnessFullResetNext = 0;
    return !m_harnessFullResetAt.empty();
}

bool ChainPhysicsPreviewWindow::HarnessSetOverlay(
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>& schedule)
{
    if (schedule.empty()) return false;
    for (const auto& e : schedule) if (e.first & ~ChainOverlay::kAll) return false;
    m_harnessOverlay = schedule;
    std::stable_sort(m_harnessOverlay.begin(), m_harnessOverlay.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });
    m_harnessOverlayNext = 0;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddOverlayShot(const std::string& path,
                                                      std::uint64_t editorFrame)
{
    if (path.empty() || editorFrame == 0) return false;
    OverlayShot s;
    s.path = path;
    s.at   = editorFrame;
    m_harnessOverlayShots.push_back(std::move(s));
    return true;
}

// ---- preview scene: scale, floor, gravity, wind, drive, follow ----
bool ChainPhysicsPreviewWindow::HarnessSetScale(float scale)
{
    if (!std::isfinite(scale) || scale > 100.f) return false;
    m_harnessScale = scale > 0.f ? scale : -1.f;   // <= 0 = the default rule
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetFloor(int mode, float gap)
{
    if (mode < 0 || mode > 2 || !std::isfinite(gap) || gap < 0.f || gap > 100.f) return false;
    m_floorMode    = mode == 0 ? 0 : 1;
    m_floorCollide = mode == 2;
    m_floorGap     = gap;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddGravity(const float down[3], float scale,
                                                  std::uint64_t at)
{
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(down[k]) || std::fabs(down[k]) > 10.f) return false;
    if (!std::isfinite(scale) || scale < 0.f || scale > 10.f) return false;
    SceneEvent e;
    e.kind = SceneEvent::Gravity;
    e.at   = at;
    e.v[0] = down[0]; e.v[1] = down[1]; e.v[2] = down[2]; e.v[3] = scale;
    m_harnessScene.push_back(e);
    return true;
}

void ChainPhysicsPreviewWindow::HarnessSetNoGravityAt(std::uint64_t at)
{
    SceneEvent e;
    e.kind = SceneEvent::NoGravity;
    e.at   = at;
    m_harnessScene.push_back(e);
}

bool ChainPhysicsPreviewWindow::HarnessAddWind(const float dir[3], float strength,
                                               float freq, float turb, std::uint64_t at)
{
    for (int k = 0; k < 3; ++k) if (!std::isfinite(dir[k])) return false;
    if (!std::isfinite(strength) || strength < 0.f || strength > 1000.f) return false;
    if (!std::isfinite(freq) || !std::isfinite(turb)) return false;
    SceneEvent e;
    e.kind = SceneEvent::Wind;
    e.at   = at;
    e.v[0] = dir[0]; e.v[1] = dir[1]; e.v[2] = dir[2];
    e.v[3] = strength; e.v[4] = freq; e.v[5] = turb;
    m_harnessScene.push_back(e);
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddDrive(float move, float rotDegPerSec, std::uint64_t at)
{
    if (!std::isfinite(move) || !std::isfinite(rotDegPerSec) ||
        std::fabs(move) > 1000.f || std::fabs(rotDegPerSec) > 3600.f) return false;
    SceneEvent e;
    e.kind = SceneEvent::Drive;
    e.at   = at;
    e.v[0] = move; e.v[1] = rotDegPerSec;
    m_harnessScene.push_back(e);
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddTeleportAt(std::uint64_t at, float dist, float angleDeg,
                                                     bool reset)
{
    if (at == 0 || !std::isfinite(dist) || !std::isfinite(angleDeg) ||
        dist > 1000.f || std::fabs(angleDeg) > 360.f) return false;
    SceneEvent e;
    e.kind = SceneEvent::Teleport;
    e.at   = at;
    e.v[0] = dist; e.v[1] = angleDeg; e.v[2] = reset ? 1.f : 0.f;
    m_harnessScene.push_back(e);
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddScaleAt(std::uint64_t at, float scale)
{
    if (at == 0 || !std::isfinite(scale) || scale <= 0.f || scale > 100.f) return false;
    SceneEvent e;
    e.kind = SceneEvent::Scale;
    e.at   = at;
    e.v[0] = scale;
    m_harnessScene.push_back(e);
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetStress(const std::string& which)
{
    if (which != "yawsweep" && which != "teleport" && which != "idle" && which != "walkrun")
        return false;
    m_harnessStress = which;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddShot(const std::string& path, std::uint64_t at)
{
    if (path.empty() || at == 0) return false;
    Shot s;
    s.path = path;
    s.at   = at;
    m_harnessShots.push_back(std::move(s));
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetFollow(const std::string& entityName)
{
    if (entityName.empty()) return false;
    m_harnessFollow = entityName;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetFollowKillAt(std::uint64_t at)
{
    if (at == 0) return false;
    SceneEvent e;
    e.kind = SceneEvent::FollowKill;
    e.at   = at;
    m_harnessScene.push_back(e);
    return true;
}

// ==== The staged parameters (UI half) ====
ChainPhysicsComponent* ChainPhysicsPreviewWindow::EditStagedFor(Entity e)
{
    if (e == NullEntity || e != m_stageChain || m_stageRoot == NullEntity || !m_stagedValid)
        return nullptr;
    return &m_staged;
}

ChainParamHooks ChainPhysicsPreviewWindow::StagedHooks()
{
    ChainParamHooks h;
    h.entity = m_stageChain;
    // Raise a flag, never Invalidate from the UI half: the one writer
    // (SyncStagedToLive, in ProcessPendingActions) Invalidates in the serial
    // window after copying the new value into the live component, so the re-init
    // always reads the edited topology.
    h.invalidate = [this]() { m_stagedStructural = true; };
    h.markStructural = true;
    // Reference bands + the active preset's coverage on the staged tree.
    h.referenceBands = true;
    h.coveredFields  = &m_presetCovered;
    // The stage's own rest-pose tool state - never EditorLayer's main-viewport
    // one (a bone index picked on a scene character must not index the stage
    // skeleton) - and never the main overlay's target.
    h.pose              = &m_stagePose;
    h.mainOverlayTarget = false;
    return h;
}

bool ChainPhysicsPreviewWindow::OwnsUndoShortcut() const
{
    if (!m_open || m_stageRoot == NullEntity || !m_stagedValid) return false;
    // Called at the top of the next UI frame: "focused" means during the
    // previous frame's Draw.
    const int now = ImGui::GetFrameCount();
    return (now - m_uiFocusFrame) <= 1;
}

// The Inspector's "Edit Chains...". Panel state only: in Edit mode the next
// ProcessPendingActions resolves it (TickEditRequest); in Play the character
// becomes the LIVE section's target.
void ChainPhysicsPreviewWindow::RequestEditEntity(EntityHandle carrier, bool playing)
{
    m_open = true;
    m_pendingFocus = true;
    if (playing)
    {
        m_liveRequestH = carrier;
        LOG_INFO("[CHAINPREVIEW] inspector edit (play): entity=%u -> LIVE target",
                 static_cast<unsigned>(carrier.entity));
        return;
    }
    m_pendingEdit  = true;
    m_pendingEditH = carrier;
}

// "Keep these values" - UI half, panel state only (EditorLayer's LIVE functor
// reads the World and hands the value over).
void ChainPhysicsPreviewWindow::KeepLiveValues(const ChainPhysicsComponent& value, bool materialOnly,
                                               EntityHandle root, EntityHandle carrier,
                                               const std::string& name, const std::string& prefabPath)
{
    m_playKeep = PlayKeep{};
    value.CopyAuthoredInto(m_playKeep.value);
    m_playKeep.valid        = true;
    m_playKeep.materialOnly = materialOnly;
    m_playKeep.root         = root;
    m_playKeep.carrier      = carrier;
    m_playKeep.name         = name;
    m_playKeep.prefabPath   = prefabPath;
    m_playKeep.takenAt      = m_tick;
    ++m_playKeeps;
    LOG_INFO("[CHAINPREVIEW] play keep taken: target='%s' root=%u carrier=%u materialOnly=%d "
             "damping=%.4f keeps=%u frame=%llu", name.c_str(), static_cast<unsigned>(root.entity),
             static_cast<unsigned>(carrier.entity), materialOnly ? 1 : 0, value.damping,
             m_playKeeps, static_cast<unsigned long long>(m_tick));
}

void ChainPhysicsPreviewWindow::ForgetPlayKeep()
{
    if (!m_playKeep.valid) return;
    LOG_INFO("[CHAINPREVIEW] play keep forgotten: target='%s'", m_playKeep.name.c_str());
    m_playKeep = PlayKeep{};
}

// The panel-tree probe's body: the same call DrawStagedParams makes and the
// same focus bookkeeping Draw does.
bool ChainPhysicsPreviewWindow::HarnessDrawStagedTree(Renderer* renderer,
                                                      const ChainParamTreeFn& drawParamTree,
                                                      bool focused)
{
    if (focused) m_uiFocusFrame = ImGui::GetFrameCount();
    if (!m_stagedValid || m_stageRoot == NullEntity) return false;
    return DrawStagedTree(renderer, drawParamTree);
}

// ==== Draw - UI half, panel-owned state only ====
namespace
{
    // A view box bigger than this is rendered at this many pixels (aspect kept)
    // and displayed fit-scaled up: a WorldCull resize is one ~3-8 ms recompile of
    // the physical textures, and a 4K panel does not need a 4K chain preview.
    constexpr float         kMaxRenderArea = 1280.f * 720.f;
    constexpr std::uint32_t kMinRenderH = 112, kMaxRenderAxis = 2048;

    // The fidelity notice: always on screen, so it wraps (~1500 px as one line)
    // and the body is sized around it.
    constexpr const char* kFidelityNotice =
        "Lit by YOUR scene (no studio rig). No punctual shadows / SSR / AO / DDGI / "
        "TAA / post grade; directional shadows are borrowed from the main camera. "
        "Do not tune shadow-sensitive looks here.";
    const ImVec4 kNoticeCol(0.9f, 0.75f, 0.4f, 1.f);

    std::uint32_t Quantise16(float v, std::uint32_t lo, std::uint32_t hi)
    {
        std::uint32_t q = static_cast<std::uint32_t>(std::max(v, 0.f));
        q = (q / 16u) * 16u;
        return std::clamp(q, lo, hi);
    }

    // Overflow of the CURRENT window's content past its VISIBLE right edge, in
    // px (<= 0 = everything fits). CursorMaxPos and ContentRegionRect both move
    // with the scroll, so the difference is scroll-invariant.
    float OverflowX()
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        return w->DC.CursorMaxPos.x - w->ContentRegionRect.Max.x;
    }
}

// Helpers shared by the Show section, the picture's badge and PPA's
// TickDisplay.
namespace
{
    // Editor frames between a display switch and its "settled" line: PPA writes
    // the bit, the next BuildScene diffs it (viewMask is part of the retained
    // key), the render half's WorldCull drops the packets, and the draw count is
    // read back a frame later - ~3 frames; 30 is slack.
    constexpr std::uint64_t kDisplaySettle = 30;

    ChainPhysicsPreviewWindow::DisplayMode NextDisplay(ChainPhysicsPreviewWindow::DisplayMode m)
    {
        using DM = ChainPhysicsPreviewWindow::DisplayMode;
        return static_cast<DM>((static_cast<int>(m) + 1) % 5);
    }

    const char* DisplayLabel(ChainPhysicsPreviewWindow::DisplayMode m)
    {
        using DM = ChainPhysicsPreviewWindow::DisplayMode;
        return m == DM::OverlayOnly ? "Overlay only" : m == DM::MeshOnly ? "Mesh only" :
               m == DM::ChainOnly ? "Chain only" : m == DM::CollidersOnly ? "Colliders only" : "Both";
    }

    constexpr const char* kDisplayTip =
        "What the picture shows (V over the picture cycles it; the badge in the picture's\n"
        "top-right corner does too):\n"
        "  Both          the mesh AND the overlay (the default).\n"
        "  Chain only    chain roots, bones and links; no mesh or colliders.\n"
        "  Colliders only  capsule colliders; no mesh or chains.\n"
        "  Overlay only  the selected overlays: every stage mesh (the character and the floor\n"
        "                plane) is hidden from THIS picture - animation, skinning and the\n"
        "                simulation keep running, the sky stays, the main viewport is not\n"
        "                touched.\n"
        "  Mesh only     the character alone: no overlay primitive (bones, links,\n"
        "                capsules, cones, the A/B ghost, the pose tool's bone dots). The\n"
        "                checkboxes below keep their state and come back with Both.\n"
        "The stats box keeps its own toggle, and with 'Pose bones in preview' on the\n"
        "picked bone's rotation ring stays except in Colliders only;\n"
        "a click still picks the nearest chain bone in Mesh only.";
}

// The layout mode, both split ratios and the display mode persist in
// imgui.ini.
void ChainPhysicsPreviewWindow::RegisterSettingsHandler()
{
    if (!ImGui::GetCurrentContext()) return;
    static const char* kType = "ChainPhysicsPreview";
    if (ImGui::FindSettingsHandler(kType)) return;
    ImGuiSettingsHandler h;
    h.TypeName   = kType;
    h.TypeHash   = ImHashStr(kType);
    h.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void*
    {
        return std::strcmp(name, "Layout") == 0 ? static_cast<void*>(&Get()) : nullptr;
    };
    h.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
    {
        auto* p = static_cast<ChainPhysicsPreviewWindow*>(entry);
        auto value = [line](const char* key, float& out)
        {
            const std::size_t n = std::strlen(key);
            if (std::strncmp(line, key, n) != 0) return false;
            char* end = nullptr;
            const float v = std::strtof(line + n, &end);
            if (!end || end == line + n || !std::isfinite(v)) return false;
            out = v;
            return true;
        };
        float v = 0.f;
        if (value("Mode=", v) && v >= 0.f && v <= 2.f)
            p->m_layout = static_cast<Layout>(static_cast<int>(v));
        else if (value("SplitSide=", v) && v > 0.05f && v < 0.95f)
            p->m_splitSide = v;
        else if (value("SplitStack=", v) && v > 0.05f && v < 0.95f)
            p->m_splitStack = v;
        // The display mode (0..4, DisplayMode).
        else if (value("Display=", v) && v >= 0.f && v <= 4.f)
            p->m_display = static_cast<DisplayMode>(static_cast<int>(v));
    };
    h.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
    {
        const ChainPhysicsPreviewWindow& p = Get();
        buf->appendf("[%s][Layout]\n", handler->TypeName);
        buf->appendf("Mode=%d\nSplitSide=%.3f\nSplitStack=%.3f\nDisplay=%d\n\n",
                     static_cast<int>(p.m_layout), p.m_splitSide, p.m_splitStack,
                     static_cast<int>(p.m_display));
    };
    ImGui::AddSettingsHandler(&h);
}

bool ChainPhysicsPreviewWindow::HarnessAddSections(std::uint32_t mask, std::uint64_t at)
{
    if (mask == 0 || (mask & ~static_cast<std::uint32_t>(kSecAll)) != 0) return false;
    m_harnessSections.emplace_back(mask, at);
    std::stable_sort(m_harnessSections.begin(), m_harnessSections.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddParamsScroll(float frac, std::uint64_t at)
{
    if (!(frac >= 0.f && frac <= 1.f)) return false;
    m_harnessParamsScroll.emplace_back(frac, at);
    std::stable_sort(m_harnessParamsScroll.begin(), m_harnessParamsScroll.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddSplit(float ratio, std::uint64_t at)
{
    if (!(ratio == -1.f || (ratio >= 0.f && ratio <= 1.f)) || at == 0) return false;
    m_harnessSplits.push_back(SplitStep{ ratio, at, false });
    std::stable_sort(m_harnessSplits.begin(), m_harnessSplits.end(),
                     [](const SplitStep& a, const SplitStep& b) { return a.at < b.at; });
    return true;
}

std::uint32_t ChainPhysicsPreviewWindow::HarnessSectionMask() const
{
    std::uint32_t m = ~0u;
    for (const auto& s : m_harnessSections)
        if (m_tick >= s.second) m = s.first;
    return m;
}

bool ChainPhysicsPreviewWindow::HarnessAddWindowStep(std::uint32_t w, std::uint32_t h,
                                                     std::uint64_t at)
{
    if (w < 200 || h < 150 || w > 8192 || h > 8192) return false;
    WindowStep s;
    s.w = w; s.h = h; s.at = at;
    m_harnessWindows.push_back(s);
    std::stable_sort(m_harnessWindows.begin(), m_harnessWindows.end(),
                     [](const WindowStep& a, const WindowStep& b) { return a.at < b.at; });
    return true;
}

// A window step with an explicit top-left.
bool ChainPhysicsPreviewWindow::HarnessAddWindowStepAt(std::uint32_t w, std::uint32_t h,
                                                       float x, float y, std::uint64_t at)
{
    if (!(x >= 0.f && x <= 8192.f && y >= 0.f && y <= 8192.f)) return false;
    if (!HarnessAddWindowStep(w, h, at)) return false;
    // stable_sort kept equal frames in insertion order: the new step is the
    // LAST one with this frame.
    for (auto it = m_harnessWindows.rbegin(); it != m_harnessWindows.rend(); ++it)
        if (it->at == at && it->w == w && it->h == h) { it->x = x; it->y = y; break; }
    return true;
}

float ChainPhysicsPreviewWindow::TreeLabelColumn() const
{
    return m_harnessLayoutFixed ? 0.f : EditorUI::Flow::LabelColumn();
}

void ChainPhysicsPreviewWindow::Draw(Renderer* renderer, World* world,
                                     Entity editorSelection,
                                     const ChainParamTreeFn& drawParamTree,
                                     const ChainLiveEditFn& drawLive, bool playing)
{
    // First-use size 1280 x 760, so the resizable parameter column starts wide
    // enough; clamped to 90 % of the editor's work area so a small main window
    // does not start the transport and the notice off-screen.
    {
        // A degenerate work area (a -hidden run minimises the window) keeps
        // the plain default: nothing on a hidden run may move.
        const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
        const bool sane = work.x >= 320.f && work.y >= 240.f;
        ImGui::SetNextWindowSize(ImVec2(sane ? std::min(1280.f, 0.9f * work.x) : 1280.f,
                                        sane ? std::min(760.f, 0.9f * work.y) : 760.f),
                                 ImGuiCond_FirstUseEver);
    }
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -chainpreview_window forces the size (and a fixed spot inside the main
    // viewport) from its editor frame on.
    {
        const WindowStep* cur = nullptr;
        for (const WindowStep& s : m_harnessWindows)
            if (m_tick >= s.at) cur = &s;
        if (cur)
        {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + cur->x, vp->WorkPos.y + cur->y), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(cur->w), static_cast<float>(cur->h)),
                                     ImGuiCond_Always);
        }
    }
#endif
    // "Edit Chains..." brings the panel to the front.
    if (m_pendingFocus)
    {
        m_pendingFocus = false;
        ImGui::SetNextWindowFocus();
        // A collapsed floating panel is not visible, so the request would wait
        // silently until expanded.
        ImGui::SetNextWindowCollapsed(false);
    }
    const bool wasOpen = m_open;
    const bool began = ImGui::Begin("Chain Physics Preview", &m_open);
    // The title-bar X with unapplied staged edits does not close - it asks
    // (DrawM5Modals), through the same guarded request the Window menu, Load,
    // drop, Use Selected and Reload take (RequestGuarded; a clean panel closes at
    // once). A harness close (-chainpreview_close_at / _frames) never comes
    // through here, and a collapsed tab is not a close (its teardown carries the
    // edits to the next spawn).
    if (wasOpen && !m_open && !m_pendingDiscard)
    {
        m_open = true;
        RequestGuarded(GuardedRequest{ GuardKind::Close, std::string(), NullEntity });
    }
    if (!began)
    {
        // COLLAPSED (or a degenerate dock) IS NOT VISIBLE: m_visibleThisFrame
        // stays false and the next ProcessPendingActions takes the one close
        // path - stage, camera, view and globals all handed back.
        ++m_beginRefusedFrames;
        ImGui::End();
        return;
    }
    if (!renderer || !world)
    {
        ImGui::TextDisabled("No renderer / world attached.");
        ImGui::End();
        return;
    }
    if (!renderer->IsPreviewEnabled())
    {
        // The kill switch, reported rather than shown as an empty box. Not visible
        // => nothing is spawned or held.
        EditorUI::Flow::DisabledWrapped("Preview disabled (DX12_PREVIEW=0 or "
                                        "editor_prefs preview.enabled=0)");
        ImGui::End();
        return;
    }
    m_visibleThisFrame = true;
    m_uiSelection = editorSelection;   // "Follow selected"
    // The panel-undo scope (EditorLayer's shortcut handler reads it at the top of
    // the next frame - OwnsUndoShortcut).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        m_uiFocusFrame = ImGui::GetFrameCount();

    m_layoutRep = LayoutReport{};
    m_layoutRep.panelW    = ImGui::GetWindowWidth();
    m_layoutRep.panelH    = ImGui::GetWindowHeight();
    m_layoutRep.fixed     = m_harnessLayoutFixed;
    m_layoutRep.drawFrame = ImGui::GetFrameCount();

    DrawToolbar(editorSelection, playing);
    m_layoutRep.toolbarOverflow = OverflowX();
    DrawBody(renderer, drawParamTree, drawLive, playing);

    // The fidelity notice: always on screen, wrapped (DrawBody reserved its
    // height).
    EditorUI::Flow::ColoredWrapped(kNoticeCol, "%s", kFidelityNotice);
    DrawM5Modals();   // rig-mismatch + close confirmations
    m_layoutRep.valid = true;
    ImGui::End();
}

// ---- toolbar: the subject picker (three entrances) + Apply ----
// The row flows: the subject field takes what the buttons leave (160..480 px)
// and every later group wraps to a new line rather than running off the right
// edge.
void ChainPhysicsPreviewWindow::DrawToolbar(Entity editorSelection, bool playing)
{
    namespace Flow = EditorUI::Flow;
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float buttonsW = Flow::ButtonW("Load") + Flow::ButtonW("Use Selected") +
                           Flow::ButtonW("Reload") + 3.f * sp;
    const float subjectW = m_harnessLayoutFixed
        ? 360.f
        : std::clamp(ImGui::GetContentRegionAvail().x - buttonsW, 160.f, 480.f);
    ImGui::SetNextItemWidth(subjectW);
    ImGui::InputTextWithHint("##cpv_subject", "asset/.../Character.ipfb",
                             m_subjectEdit, sizeof(m_subjectEdit));
    // No subject change while Play has the stage suspended: the unapplied edits
    // live only in the carry then, and the dirty confirmation can only be
    // answered after Stop (harness requests are queued the same way, see
    // RequestGuarded).
    const bool subjectLocked = m_suspendedForPlay || playing;
    // Entrance 2 of 3: drop a prefab from the Asset Browser onto the field.
    if (!subjectLocked && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IPFB_PATH"))
        {
            std::snprintf(m_subjectEdit, sizeof(m_subjectEdit), "%s",
                          static_cast<const char*>(p->Data));
            // Through the dirty confirmation (CommitGuarded clears the scale / in-place /
            // force state).
            RequestGuarded(GuardedRequest{ GuardKind::Drop, std::string(m_subjectEdit), NullEntity });
        }
        ImGui::EndDragDropTarget();
    }
    Flow::SameLineIfFits(Flow::ButtonW("Load"));
    ImGui::BeginDisabled(subjectLocked);
    if (ImGui::Button("Load"))
        RequestGuarded(GuardedRequest{ GuardKind::Load, std::string(m_subjectEdit), NullEntity });
    ImGui::EndDisabled();
    if (subjectLocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Suspended during Play - change the subject after Stop.");
    Flow::SameLineIfFits(Flow::ButtonW("Use Selected"));
    ImGui::BeginDisabled(editorSelection == NullEntity || subjectLocked);
    if (ImGui::Button("Use Selected"))
        RequestGuarded(GuardedRequest{ GuardKind::UseSelected, std::string(), editorSelection });
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Preview the selected character. A prefab instance is "
                          "previewed from its .ipfb; an in-place character is "
                          "cloned into the stage.");
    Flow::SameLineIfFits(Flow::ButtonW("Reload"));
    ImGui::BeginDisabled(m_subjectPath.empty() || subjectLocked);
    if (ImGui::Button("Reload"))
        RequestGuarded(GuardedRequest{ GuardKind::Reload, std::string(), NullEntity });
    ImGui::EndDisabled();
    // Apply v / Revert / the dirty dot. The Apply group (dot + "Apply (...)" +
    // arrow) moves as one piece.
    {
        const float dotW = m_uiDirty ? ImGui::GetTextLineHeight() * 0.6f + 2.f + sp : 0.f;
        const float applyW = dotW + Flow::ButtonW(ApplyButtonLabel().c_str()) + 2.f +
                             ImGui::GetFrameHeight();
        Flow::SameLineIfFits(applyW);
    }
    DrawApplyUi();
    DrawShowUi();
    // Auto | Side | Stack.
    if (!m_harnessLayoutFixed) DrawLayoutSelector();

    if (!m_status.empty())
        Flow::ColoredWrapped(m_spawnFailed ? ImVec4(1.f, 0.45f, 0.35f, 1.f)
                                           : ImVec4(0.75f, 0.75f, 0.75f, 1.f),
                             "%s", m_status.c_str());
    // Staged edits a world load interrupted, offered back when their subject is
    // picked again.
    if (m_reapplyOffer)
    {
        Flow::ColoredWrapped(ImVec4(1.f, 0.8f, 0.35f, 1.f),
                             "Unapplied staged edits from before the world reload exist for this "
                             "subject (%s).", m_reapplyLabel.c_str());
        if (ImGui::SmallButton("Re-apply staged params")) m_pendingReapply = 1;
        Flow::SameLineIfFits(Flow::ButtonW("Forget them"));
        if (ImGui::SmallButton("Forget them")) m_pendingReapply = 0;
    }
}

void ChainPhysicsPreviewWindow::DrawLayoutSelector()
{
    namespace Flow = EditorUI::Flow;
    static const char* kNames[3] = { "Auto", "Side", "Stack" };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Layout");
    ImGui::SameLine();
    ImGui::PushID("cpv_layout");
    for (int i = 0; i < 3; ++i)
    {
        if (i > 0) ImGui::SameLine(0.f, 2.f);
        const bool on = static_cast<int>(m_layout) == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        if (ImGui::SmallButton(kNames[i]) && !on)
        {
            m_layout = static_cast<Layout>(i);
            ImGui::MarkIniSettingsDirty();
        }
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(i == 0 ? "Auto: parameters beside the picture, or below it when the\n"
                                       "panel is taller than wide (or too narrow for both)."
                            : i == 1 ? "Parameters beside the picture (drag the bar between them)."
                                     : "Parameters below the picture (drag the bar between them).");
    }
    ImGui::PopID();
}

// ---- the body: view | splitter | parameters, or stacked ----
void ChainPhysicsPreviewWindow::DrawBody(Renderer* renderer, const ChainParamTreeFn& drawParamTree,
                                         const ChainLiveEditFn& drawLive, bool playing)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // The notice under the body is sized with the wrap it will get.
    const float noticeH = ImGui::CalcTextSize(kFidelityNotice, nullptr, false,
                                              std::max(avail.x, 1.f)).y + st.ItemSpacing.y;
    const ImVec2 body(std::max(avail.x, 1.f), std::max(avail.y - noticeH, 64.f));
    const ImVec2 bodyPos = ImGui::GetCursorScreenPos();

    auto reportView = [this]()
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        m_layoutRep.viewX = w->Pos.x - ImGui::GetWindowViewport()->Pos.x;
        m_layoutRep.viewY = w->Pos.y - ImGui::GetWindowViewport()->Pos.y;
        m_layoutRep.viewW = w->Size.x;
        m_layoutRep.viewH = w->Size.y;
        m_layoutRep.viewOverflow = std::max(OverflowX(),
                                            w->DC.CursorMaxPos.y - w->ContentRegionRect.Max.y);
    };
    auto reportParams = [this]()
    {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        m_layoutRep.paramsW = w->Size.x;
        m_layoutRep.paramsH = w->Size.y;
        m_layoutRep.paramsOverflow = OverflowX();
        m_layoutRep.paramsHScroll  = w->ScrollMax.x > 0.f;
    };

    if (m_harnessLayoutFixed)
    {
        // The -chainpreview_layout_fixed reverse arm: a fixed 300 px column, and the
        // picture sized from the width alone (DrawViewArea).
        const float rightW = 300.f;
        const float leftW  = std::max(body.x - rightW - 8.f, 64.f);
        ImGui::BeginChild("##cpv_view", ImVec2(leftW, body.y), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        DrawViewArea(renderer);
        reportView();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##cpv_params", ImVec2(rightW, body.y), ImGuiChildFlags_Borders);
        DrawParamsBody(renderer, drawParamTree, drawLive, playing);
        reportParams();
        ImGui::EndChild();
        m_layoutRep.stacked = false;
        m_layoutRep.split   = leftW / body.x;
        return;
    }

    // In Play the stage is suspended and the picture would be an empty box: the
    // view collapses to one line and the LIVE parameters take the body.
    if (playing)
    {
        EditorUI::Flow::DisabledWrapped("Preview suspended during Play - the stage is rebuilt "
                                        "when Play stops. The LIVE parameters below edit the "
                                        "running character.");
        const float h = std::max(ImGui::GetContentRegionAvail().y - noticeH, 64.f);
        ImGui::BeginChild("##cpv_params", ImVec2(0.f, h), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        DrawParamsBody(renderer, drawParamTree, drawLive, playing);
        reportParams();
        ImGui::EndChild();
        m_layoutRep.stacked = true;
        m_layoutRep.split   = 0.f;
        return;
    }

    // ---- which layout: forced, or Auto with hysteresis around aspect 1 ----
    bool stacked = m_layout == Layout::Stacked;
    if (m_layout == Layout::Auto)
    {
        const float a = body.x / std::max(body.y, 1.f);
        stacked = m_stacked ? (a < 1.25f) : (a < 1.0f);
        if (body.x < kMinViewW + kSplitBar + kMinParamW) stacked = true;
    }
    m_stacked = stacked;
    m_layoutRep.stacked = stacked;

    // -chainpreview_split: the splitter set exactly as the drag (or the
    // double-click) sets it, with the same clamp.
    auto harnessSplit = [&](float& ratio, float extent, float minV, float maxV, float def,
                            const char* axis)
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        for (SplitStep& s : m_harnessSplits)
        {
            if (s.done || m_tick < s.at) continue;
            s.done = true;
            ++m_harnessSplitsApplied;
            ratio = s.ratio < 0.f ? def : std::clamp(s.ratio * extent, minV, maxV) / extent;
            LOG_INFO("[CHAINPREVIEW] harness: split %u axis=%s asked=%.3f ratio=%.4f extent=%.0f "
                     "min=%.0f max=%.0f reset=%d frame=%llu", m_harnessSplitsApplied, axis,
                     s.ratio, ratio, extent, minV, maxV, s.ratio < 0.f ? 1 : 0,
                     static_cast<unsigned long long>(m_tick));
        }
#else
        (void)ratio; (void)extent; (void)minV; (void)maxV; (void)def; (void)axis;
#endif
    };

    const ImGuiWindowFlags viewFlags = ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;   // the wheel is zoom
    const ImU32 barCol    = ImGui::GetColorU32(ImGuiCol_Separator);
    const ImU32 barColHot = ImGui::GetColorU32(ImGuiCol_SeparatorActive);
    if (!stacked)
    {
        const float maxVw = std::max(kMinViewW, body.x - kSplitBar - kMinParamW);
        harnessSplit(m_splitSide, body.x, kMinViewW, maxVw, kDefSplitSide, "x");
        const float vw    = std::clamp(std::floor(body.x * m_splitSide), kMinViewW, maxVw);
        ImGui::BeginChild("##cpv_view", ImVec2(vw, body.y), ImGuiChildFlags_None, viewFlags);
        DrawViewArea(renderer);
        reportView();
        ImGui::EndChild();
        // ---- the splitter: drag = the ratio, double-click = the default ----
        ImGui::SameLine(0.f, 0.f);
        ImGui::InvisibleButton("##cpv_split", ImVec2(kSplitBar, body.y));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f))
        {
            const float x = ImGui::GetIO().MousePos.x - bodyPos.x - 0.5f * kSplitBar;
            m_splitSide = std::clamp(x, kMinViewW, maxVw) / body.x;
            ImGui::MarkIniSettingsDirty();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            m_splitSide = kDefSplitSide;
            ImGui::MarkIniSettingsDirty();
        }
        if (hot)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            ImGui::SetTooltip("Drag to resize; double-click resets.");
        }
        {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const float cx = 0.5f * (a.x + b.x);
            ImGui::GetWindowDrawList()->AddLine(ImVec2(cx, a.y + 4.f), ImVec2(cx, b.y - 4.f),
                                                hot ? barColHot : barCol, hot ? 3.f : 1.f);
        }
        ImGui::SameLine(0.f, 0.f);
        ImGui::BeginChild("##cpv_params", ImVec2(0.f, body.y), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        m_layoutRep.split = vw / body.x;
    }
    else
    {
        const float maxVh = std::max(kMinViewH, body.y - kSplitBar - kMinParamH);
        harnessSplit(m_splitStack, body.y, kMinViewH, maxVh, kDefSplitStack, "y");
        const float vh    = std::clamp(std::floor(body.y * m_splitStack), kMinViewH, maxVh);
        ImGui::BeginChild("##cpv_view", ImVec2(0.f, vh), ImGuiChildFlags_None, viewFlags);
        DrawViewArea(renderer);
        reportView();
        ImGui::EndChild();
        // stacked children: the cursor is already on the next line; remove the
        // item spacing so the bar sits flush between them.
        ImGui::SetCursorScreenPos(ImVec2(bodyPos.x, bodyPos.y + vh));
        ImGui::InvisibleButton("##cpv_split", ImVec2(body.x, kSplitBar));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f))
        {
            const float y = ImGui::GetIO().MousePos.y - bodyPos.y - 0.5f * kSplitBar;
            m_splitStack = std::clamp(y, kMinViewH, maxVh) / body.y;
            ImGui::MarkIniSettingsDirty();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            m_splitStack = kDefSplitStack;
            ImGui::MarkIniSettingsDirty();
        }
        if (hot)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            ImGui::SetTooltip("Drag to resize; double-click resets.");
        }
        {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const float cy = 0.5f * (a.y + b.y);
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + 4.f, cy), ImVec2(b.x - 4.f, cy),
                                                hot ? barColHot : barCol, hot ? 3.f : 1.f);
        }
        ImGui::SetCursorScreenPos(ImVec2(bodyPos.x, bodyPos.y + vh + kSplitBar));
        ImGui::BeginChild("##cpv_params", ImVec2(0.f, std::max(body.y - vh - kSplitBar, 32.f)),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        m_layoutRep.split = vh / body.y;
    }
    DrawParamsBody(renderer, drawParamTree, drawLive, playing);
    reportParams();
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -chainpreview_params_scroll: the column at a fraction of its range.
    {
        float frac = -1.f;
        for (const auto& s : m_harnessParamsScroll)
            if (m_tick >= s.second) frac = s.first;
        if (frac >= 0.f) ImGui::SetScrollY(ImGui::GetScrollMaxY() * frac);
    }
#endif
    ImGui::EndChild();
}

// ---- the picture + transport ----
// The render size comes from both axes of the box the view child leaves above
// the transport; the picture is displayed fit-scaled and centred (letterbox,
// never stretched), which also covers a shared-lane / Match-size picture whose
// aspect is not the box's. Under the harness pin (-chainpreview_size=WxH) the
// display scale stays 1 and the picture sits at the child's top-left: the
// overlay statistics are taken at the display size, and the pinned packs and
// the chain_preview_overlay golden must not move.
void ChainPhysicsPreviewWindow::DrawViewArea(Renderer* renderer)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 cur0  = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const bool pinned  = m_harnessW != 0 && m_harnessH != 0;
    const bool fixed   = m_harnessLayoutFixed;
    // The block under the picture (FROZEN / sharing rows + the transport) as
    // MEASURED last frame; before any, the style's estimate: the buttons row,
    // the read-out, the 40 px ruler, their spacing.
    const float belowEstimate = 4.f + ImGui::GetFrameHeightWithSpacing() +
                                ImGui::GetTextLineHeightWithSpacing() + 40.f + st.ItemSpacing.y;
    const float belowH = m_belowH > 0.f ? m_belowH : belowEstimate;
    const ImVec2 box(std::max(avail.x, 1.f), std::max(avail.y - belowH, 1.f));

    // ---- size: the harness pin, the fixed arm, or both axes of the box ----
    if (pinned)
    {
        m_sizeW = m_pendW = m_harnessW;
        m_sizeH = m_pendH = m_harnessH;
        m_sizeCapped = false;
    }
    else
    {
        std::uint32_t wantW = 0, wantH = 0;
        if (fixed)
        {
            // fixed arm: the width alone, 16:9, 192..960
            const float a = avail.x;
            wantW = QuantiseWidth(a > 0.f ? a : 640.f);
            wantH = HeightFor(wantW);
            m_sizeCapped = false;
        }
        else
        {
            // Hidden (region <= 0): the documented 640x360.
            float bw = avail.x > 0.f ? box.x : 640.f;
            float bh = avail.x > 0.f && avail.y > 0.f ? box.y : 360.f;
            const float area = bw * bh;
            m_sizeCapped = area > kMaxRenderArea;
            if (m_sizeCapped)
            {
                const float k = std::sqrt(kMaxRenderArea / area);
                bw *= k; bh *= k;
            }
            wantW = Quantise16(bw, kMinW, kMaxRenderAxis);
            wantH = Quantise16(bh, kMinRenderH, kMaxRenderAxis);
        }
        // Debounced on (W, H) - SplitViewportPanel's rule. -chainpreview_window
        // bypasses it: a harness commit frame must not depend on wall time.
        const double now = ImGui::GetTime();
        const bool   immediate = !m_harnessWindows.empty();
        if (wantW != m_measuredW || wantH != m_measuredH)
        {
            m_measuredW = wantW; m_measuredH = wantH;
            m_pendW = wantW; m_pendH = wantH; m_pendAt = now;
        }
        if ((m_pendW != m_sizeW || m_pendH != m_sizeH) &&
            (immediate || (now - m_pendAt) >= kResizeDebounceSeconds))
        { m_sizeW = m_pendW; m_sizeH = m_pendH; }
    }

    // THE SRV IS RE-RESOLVED HERE, ONCE, EVERY FRAME, NEVER CACHED.
    const std::uint64_t srv =
        (m_viewId != ~0u) ? renderer->GetPreviewSrv(m_viewId) : 0ull;
    const bool frozen = srv && m_viewId != ~0u && renderer->IsPreviewViewSizeRefused(m_viewId);

    // ---- display: fit-scaled + centred (scale 1 at the top-left when pinned)
    const std::uint32_t showW = m_renderW ? m_renderW : m_sizeW;
    const std::uint32_t showH = m_renderH ? m_renderH : m_sizeH;
    float scale = 1.f;
    if (!pinned && !fixed && showW && showH)
    {
        const float fit = std::min(box.x / static_cast<float>(showW),
                                   box.y / static_cast<float>(showH));
        // a picture rendered FOR this box is shown 1:1 (never blurred up by a
        // sub-16 px remainder); a capped / borrowed / frozen one fills the box
        scale = (m_sizeCapped || m_sharingLane || frozen) ? fit : std::min(fit, 1.f);
        if (!(scale > 0.f)) scale = 1.f;
    }
    const ImVec2 imgSize(std::max(std::floor(static_cast<float>(showW) * scale), 1.f),
                         std::max(std::floor(static_cast<float>(showH) * scale), 1.f));
    ImVec2 origin = cur0;
    if (!pinned && !fixed)
        origin = ImVec2(cur0.x + std::floor(std::max(box.x - imgSize.x, 0.f) * 0.5f),
                        cur0.y + std::floor(std::max(box.y - imgSize.y, 0.f) * 0.5f));
    m_layoutRep.imgW = imgSize.x;  m_layoutRep.imgH = imgSize.y;
    m_layoutRep.scale = scale;
    m_layoutRep.renderW = showW;   m_layoutRep.renderH = showH;

    ImGui::SetCursorScreenPos(origin);
    if (m_suspendedForPlay || m_viewUnavailable)
    {
        ImGui::SetCursorScreenPos(cur0);
        if (m_suspendedForPlay)
            EditorUI::Flow::DisabledWrapped("Suspended during Play - the stage is rebuilt "
                                            "when Play stops.");
        else
            EditorUI::Flow::DisabledWrapped("No free preview view (at most %u live). Close "
                                            "another preview panel and reopen this one.",
                                            Preview::kMaxPreviews);
    }
    else if (srv)
    {
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(srv)), imgSize,
                     ImVec2(0, 0), ImVec2(1, 1));
        // The overlay, over the picture, from the snapshot PPA took (Draw reads no
        // World and no solver). A FROZEN picture is an old one: no overlay on it.
        if (!frozen) DrawOverlay(renderer, origin, imgSize);
    }
    else
    {
        ImGui::Dummy(imgSize);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.f, origin.y + 8.f));
        ImGui::TextDisabled(m_subjectPath.empty()
                            ? "Pick a subject (.ipfb) above, or drop one here."
                            : "Rendering...");
    }
    // The mode badge in the picture's top-right corner (a click cycles the
    // display modes; V over the picture does the same) is submitted before the
    // pose ring and ##cpv_orbit: over the badge it owns the hover, so neither the
    // ring nor an orbit can start there. The in-picture rest-pose gizmo (pickable
    // bone markers + the rotate ring on the picked one) goes over the displayed
    // rect, also before ##cpv_orbit (ImGuizmo's patched CanActivate needs
    // HoveredId == 0 && ActiveId == 0 on the click frame); not on a FROZEN (old)
    // picture, nor without one.
    if (srv && !frozen && !m_suspendedForPlay && !m_viewUnavailable)
    {
        const char*  lbl = DisplayLabel(m_display);
        const ImVec2 ts  = ImGui::CalcTextSize(lbl);
        const ImVec2 bsz(ts.x + 2.f * st.FramePadding.x, ts.y);
        if (imgSize.x > bsz.x + 16.f && imgSize.y > bsz.y + 16.f)
        {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + imgSize.x - bsz.x - 6.f, origin.y + 6.f));
            ImGui::PushID("cpv_display_badge");
            if (ImGui::SmallButton(lbl)) RequestDisplay(NextDisplay(m_display), "picture");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Display: %s - click (or V over the picture) to cycle "
                                  "Both -> Overlay only -> Mesh only -> Chain only -> Colliders only.\n\n%s",
                                  DisplayLabel(m_display), kDisplayTip);
            ImGui::PopID();
        }
    }
    if (srv && !frozen && !m_suspendedForPlay && !m_viewUnavailable)
        DrawPoseGizmo(origin, imgSize);
    else
        DrawPoseGizmo(origin, ImVec2(0.f, 0.f));   // closes a stranded drag
    // Orbit / pan stand aside while the ring is hovered or held (the orbit
    // button is DISABLED then, so the click that grabs the ring cannot also
    // start an orbit); a hover while an orbit is already running does not
    // interrupt it.
    const bool gizmoBusy = m_poseGestureUi || (m_poseGizmoOver && !m_orbitActive);

    // ---- orbit: the panel's OWN yaw/pitch/distance (never m_vpMouseDX) ----
    // Everything below maps the mouse through the DISPLAYED rect (origin,
    // imgSize) - the one the picture and the overlay were drawn in.
    ImGui::SetCursorScreenPos(origin);
    if (gizmoBusy) ImGui::BeginDisabled();
    ImGui::InvisibleButton("##cpv_orbit", imgSize);
    if (gizmoBusy) ImGui::EndDisabled();
    m_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    m_orbitActive = ImGui::IsItemActive();
    // A click (no drag) in pose mode picks the nearest pickable chain bone under
    // the cursor (the button was enabled, so the press did not land on the ring).
    if (m_stagePose.poseMode && EffectiveDisplay() != DisplayMode::CollidersOnly && ImGui::IsItemDeactivated() &&
        ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16.f)
    {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        PickPoseBone(ImVec2(mp.x - origin.x, mp.y - origin.y), imgSize);
    }
    if (m_orbitActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        m_yaw   += d.x * 0.01f;
        m_pitch -= d.y * 0.01f;
        m_pitch  = std::clamp(m_pitch, -1.4f, 1.4f);
    }
    // Pan (middle drag) in the camera's own right / up plane, scaled so the
    // subject tracks the mouse; F = Focus the chains. Panel state only: PPA
    // writes the camera entity.
    if (m_hovered && !gizmoBusy && ImGui::IsMouseDragging(ImGuiMouseButton_Middle) && m_overlayCam.valid)
    {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        const DirectX::XMVECTOR fw = DirectX::XMLoadFloat3(&m_overlayCam.forward);
        const DirectX::XMVECTOR up = DirectX::XMLoadFloat3(&m_overlayCam.up);
        const DirectX::XMVECTOR rt = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(up, fw));
        DirectX::XMFLOAT3 r3, u3;
        DirectX::XMStoreFloat3(&r3, rt);
        DirectX::XMStoreFloat3(&u3, up);
        // world units per DISPLAYED pixel at the focus distance
        const float dist = 2.f * (m_camRadius > 0.f ? m_camRadius : m_radius) * m_dist *
                           Preview::kStudioCamera.distance;
        const float perPx = dist * 2.f * std::tan(0.5f * Preview::kStudioCamera.fovY) /
                            std::max(imgSize.y, 1.f);
        for (int k = 0; k < 3; ++k)
            m_pan[k] += (-d.x * (&r3.x)[k] + d.y * (&u3.x)[k]) * perPx;
    }
    // Poke: Ctrl + right-drag (PhAT's gesture). Panel state only: the press point
    // and the drag, normalised over the displayed picture; PPA casts the ray,
    // picks the particle and applies the impulse.
    {
        const ImGuiIO& pio = ImGui::GetIO();
        const ImVec2 mp = pio.MousePos;
        if (m_hovered && pio.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_pokeDragging = true;
            m_pokeStart[0] = mp.x - origin.x;
            m_pokeStart[1] = mp.y - origin.y;
        }
        if (m_pokeDragging)
        {
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            const ImVec2 a(origin.x + m_pokeStart[0], origin.y + m_pokeStart[1]);
            pdl->AddLine(a, mp, IM_COL32(255, 120, 40, 220), 2.f);
            pdl->AddCircle(a, 5.f, IM_COL32(255, 120, 40, 220), 12, 1.5f);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                m_pokeDragging = false;
                if (imgSize.x > 1.f && imgSize.y > 1.f)
                {
                    m_pendingPoke = PendingPoke{};
                    m_pendingPoke.valid = true;
                    m_pendingPoke.u  = m_pokeStart[0] / imgSize.x;
                    m_pendingPoke.v  = m_pokeStart[1] / imgSize.y;
                    m_pendingPoke.du = (mp.x - a.x) / imgSize.x;
                    m_pendingPoke.dv = (mp.y - a.y) / imgSize.y;
                }
            }
        }
    }
    if (m_hovered)
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
            m_dist = std::clamp(m_dist * (1.f - wheel * 0.1f), 0.2f, 6.f);
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) m_pendingFocusChains = true;
        // V cycles the display modes.
        if (!ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_V, false))
            RequestDisplay(NextDisplay(m_display), "key");
        // No tooltip over the ring it would cover.
        if (!gizmoBusy)
            ImGui::SetTooltip(m_stagePose.poseMode
                ? "Pose mode: click a chain bone (dots) to pick it, then drag its rotation "
                  "ring - edits the STAGED authored rest pose (one undo step per drag; the "
                  "simulation pauses while you drag). Drag elsewhere to orbit, middle-drag "
                  "to pan, wheel to zoom."
                : "Drag to orbit, middle-drag to pan, wheel to zoom, F to "
                  "focus the chains, V to cycle the display modes, Ctrl + right-drag to poke a chain. "
                  "Drop a .ipfb here to preview it.");
    }
    // Entrance 2 again, on the picture itself.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IPFB_PATH"))
        {
            std::snprintf(m_subjectEdit, sizeof(m_subjectEdit), "%s",
                          static_cast<const char*>(p->Data));
            // The dirty confirmation first.
            RequestGuarded(GuardedRequest{ GuardKind::Drop, std::string(m_subjectEdit), NullEntity });
        }
        ImGui::EndDragDropTarget();
    }

    // ---- under the picture: FROZEN / sharing rows, then the transport ----
    // Laid out from the cursor after the picture box, so the transport never
    // overlaps the FROZEN text and the Match size button.
    const float belowY = std::max(cur0.y + box.y, origin.y + imgSize.y) + 4.f;
    ImGui::SetCursorScreenPos(ImVec2(cur0.x, belowY));
    if (frozen)
    {
        std::uint32_t lw = 0, lh = 0;
        renderer->GetPreviewWorldCullSize(lw, lh);
        ImGui::TextDisabled("FROZEN - camera views share one %u x %u lane.", lw, lh);
        if (lw != 0 && lh != 0)
        {
            EditorUI::Flow::SameLineIfFits(EditorUI::Flow::ButtonW("Match size"));
            if (ImGui::SmallButton("Match size"))
            {
                m_sizeW = m_pendW = lw;
                m_sizeH = m_pendH = lh;
                if (m_harnessW != 0) { m_harnessW = lw; m_harnessH = lh; }
            }
        }
    }
    if (m_sharingLane)
        ImGui::TextDisabled("sharing lane size %u x %u", m_renderW, m_renderH);
    DrawTransport();
    const float endY = ImGui::GetCursorScreenPos().y - st.ItemSpacing.y;
    m_layoutRep.transportDrawn   = true;
    m_layoutRep.transportVisible = endY <= ImGui::GetCurrentWindow()->InnerRect.Max.y + 0.5f;
    // what the NEXT frame reserves under the picture (+1: rounding)
    m_belowH = std::max(endY - belowY, 0.f) + 4.f + 1.f;
}

// ---- the parameter column's sections ----
void ChainPhysicsPreviewWindow::DrawParamsBody(Renderer* renderer,
                                               const ChainParamTreeFn& drawParamTree,
                                               const ChainLiveEditFn& drawLive, bool playing)
{
    namespace Flow = EditorUI::Flow;
    // the running overflow after each section - the first one that jumps is
    // the culprit (CursorMaxPos only grows), reported on the layout line
    int sec = 0;
    auto mark = [&]() { if (sec < 6) m_layoutRep.secOverflow[sec++] = OverflowX(); };
    // -chainpreview_sections: the harness forces EXACTLY this set open (the
    // layout pack's expanded steps); ~0u = the user's own state.
    const std::uint32_t forced = HarnessSectionMask();
    std::uint32_t secBit = 1u;
    auto open = [forced, &secBit]()
    {
        if (forced != ~0u) ImGui::SetNextItemOpen((forced & secBit) != 0, ImGuiCond_Always);
        secBit <<= 1;
    };
    open();
    if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawControllerUi();
        if (m_stageRoot != NullEntity)
            Flow::Wrapped("stage: %u entities, %u part(s), %u skinned",
                          static_cast<unsigned>(m_stageEnts.size()),
                          m_budgetInstances, m_budgetSkinJobs);
        Flow::DisabledWrapped("bones %u  verts %u  stripped %u",
                              m_budgetBones, m_budgetVerts, m_stripped);
    }
    mark();
    open();
    if (ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawSimulationUi();   // the simulation transport
        DrawSoloUi();         // per-group solo
    }
    mark();
    open();
    if (ImGui::CollapsingHeader("Scene"))
        DrawSceneUi();
    mark();
    if (Sys::CommandLine::Get().Has("-editor_declutter_verify"))
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (drawSceneColliders && ImGui::CollapsingHeader("Capsule Colliders"))
        drawSceneColliders();
    open();
    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // In Play the stage is suspended and the section edits a scene character's
        // live component instead (EditorLayer's functor - transient, reverted on
        // Stop).
        if (playing) DrawLiveParams(drawLive);
        else         DrawStagedParams(renderer, drawParamTree);
    }
    mark();
    open();
    if (ImGui::CollapsingHeader("A/B compare"))
        DrawAbUi(renderer, drawParamTree);
    mark();
    // The Show section lives in the toolbar; this mark fills the layout report's
    // sixth slot (ovShow).
    mark();
}

// One "[CHAINPREVIEW] layout:" line per -chainpreview_window step,
// kLayoutSettle editor frames after it (the size commit is immediate under
// the steps; the lane move + first picture take a few frames). PPA runs
// before this frame's Draw, so the line is what the last Draw laid out.
void ChainPhysicsPreviewWindow::TickLayoutHarness()
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    for (std::size_t i = 0; i < m_harnessWindows.size(); ++i)
    {
        WindowStep& s = m_harnessWindows[i];
        if (s.reported || m_tick < s.at + kLayoutSettle) continue;
        // a later step already due would overwrite this one's layout
        if (i + 1 < m_harnessWindows.size() && m_tick >= m_harnessWindows[i + 1].at)
        {
            s.reported = true;
            LOG_WARNING("[CHAINPREVIEW] layout step %zu (%ux%u@%llu) superseded before it "
                        "settled - no report", i + 1, s.w, s.h,
                        static_cast<unsigned long long>(s.at));
            continue;
        }
        s.reported = true;
        char when[32];
        std::snprintf(when, sizeof(when), "step=%zu", i + 1);
        LogLayout(when);
    }
#endif
}

void ChainPhysicsPreviewWindow::LogLayout(const char* when) const
{
    const LayoutReport& r = m_layoutRep;
    const bool fits = r.imgW <= r.viewW + 0.5f && r.imgH <= r.viewH + 0.5f;
    // The whole clip is on the ruler (its pixel length <= the lanes' width
    // + 1 px).
    const bool rulerFits = r.rulerDrawn && r.rulerLanesW > 1.f &&
                           r.rulerFill * r.rulerLanesW <= r.rulerLanesW + 1.f;
    // name=number pairs (logLineFields); the requested composite fields
    // (view=x,y,w,h img=w,h render=WxH) are followed by their numeric twins.
    LOG_INFO("[CHAINPREVIEW] layout: %s panel=%.0fx%.0f mode=%s split=%.3f "
             "view=%.0f,%.0f,%.0f,%.0f img=%.0f,%.0f render=%ux%u scale=%.3f params=%.0f,%.0f "
             "paramsOverflowPx=%.1f viewOverflowPx=%.1f transportVisible=%d "
             "panelW=%.0f panelH=%.0f stacked=%d viewW=%.0f viewH=%.0f imgW=%.0f imgH=%.0f "
             "renderW=%u renderH=%u paramsW=%.0f paramsH=%.0f imgFitsView=%d paramsHScroll=%d "
             "toolbarOverflowPx=%.1f fixed=%d valid=%d ovPreview=%.1f ovSimulation=%.1f "
             "ovScene=%.1f ovParameters=%.1f ovAb=%.1f ovShow=%.1f "
             "rulerDrawn=%d rulerFits=%d rulerFill=%.3f rulerLanesW=%.0f frame=%llu",
             when ? when : "", r.panelW, r.panelH, r.stacked ? "stacked" : "side", r.split,
             r.viewX, r.viewY, r.viewW, r.viewH, r.imgW, r.imgH, r.renderW, r.renderH, r.scale,
             r.paramsW, r.paramsH, r.paramsOverflow, r.viewOverflow,
             (r.transportDrawn && r.transportVisible) ? 1 : 0,
             r.panelW, r.panelH, r.stacked ? 1 : 0, r.viewW, r.viewH, r.imgW, r.imgH,
             r.renderW, r.renderH, r.paramsW, r.paramsH, fits ? 1 : 0, r.paramsHScroll ? 1 : 0,
             r.toolbarOverflow, r.fixed ? 1 : 0, r.valid ? 1 : 0,
             r.secOverflow[0], r.secOverflow[1], r.secOverflow[2], r.secOverflow[3],
             r.secOverflow[4], r.secOverflow[5],
             r.rulerDrawn ? 1 : 0, rulerFits ? 1 : 0, r.rulerFill, r.rulerLanesW,
             static_cast<unsigned long long>(m_tick));
}

// ==== ProcessPendingActions - game half, serial window: every side effect ====
void ChainPhysicsPreviewWindow::ProcessPendingActions(const ChainPreviewContext& ctx)
{
    ++m_tick;
    // "Did the panel draw LAST frame" - this runs before this frame's Draw.
    const bool drewLast = m_visibleThisFrame;
    m_visibleThisFrame  = false;
    if (drewLast)                                   ++m_drewFrames;
    else if (m_open && !m_harnessSubject.empty())   ++m_hiddenFrames;
    // A panel that did not draw cannot be holding its ring: the gesture closes
    // (TickPoseGizmo commits it this frame).
    if (!drewLast) ReleasePoseGesture();   // also releases ImGuizmo's latch
    // -chainpreview_window's self-reports (what the last Draw laid out).
    TickLayoutHarness();

    Renderer* renderer = ctx.renderer;
    World*    world    = ctx.world;
    if (!renderer || !world) return;
    m_scenePathNow = ctx.scenePath;   // keys an in-place stash (RecordSceneTarget)

    // ---- World::Epoch(): a clear / AdoptState recycled every entity id ----
    // The stage's handles now name nothing (Clear) or someone else's entities
    // (a restore reusing the ids), so they are DROPPED - never destroyed, which
    // would delete whatever holds those ids now - and the stage is respawned
    // rather than re-found by name.
    const std::uint32_t epoch = world->Epoch();
    // A DIFFERENT World object is the same event: two worlds' epochs can be
    // equal, and the ids would still name nothing of ours.
    if (epoch != m_worldEpoch || world != m_lastWorld)
    {
        m_lastWorld = world;
        const bool hadStage = m_stageRoot != NullEntity || m_camEnt != NullEntity;
        if (m_worldEpochSeen && hadStage)
            LOG_INFO("[CHAINPREVIEW] world epoch %u -> %u: dropping the stage "
                     "handles (ids recycle across a reload); the stage is %s",
                     m_worldEpoch, epoch,
                     (m_subjectFromHarness || m_suspendedForPlay)
                        ? "respawned from its subject" : "cleared with its subject");
        m_worldEpoch     = epoch;
        m_worldEpochSeen = true;
        // The floor's id names nothing of ours any more, and a FollowSceneEntity
        // source is gone with the old World: fall back to Clip and say so - never
        // follow a recycled id.
        m_floorEnt = NullEntity;
        m_floorH   = NullEntityHandle;
        // AdoptState (Play -> Stop) is an epoch change too, and it restores ids and
        // generations: the Scene Apply target is kept when its handle still names an
        // entity with the recorded name (and Guid, when it has one); otherwise it is
        // re-resolved through the Guid; otherwise it is LOST and Apply refuses -
        // never the live selection.
        m_pendingSceneTargetH = NullEntityHandle;
        KeepSceneTargetAcrossEpoch(*world);
        // A world load takes the stage with it; unapplied staged edits are kept,
        // keyed by subject, and offered back ("Re-apply staged params") when that
        // subject is picked again. (Play -> Stop never gets here with a stage:
        // TearDownForPlay captured it first.)
        if (hadStage && m_stagedValid && !m_harnessConfirmOff && IsStagedDirty())
        {
            StagedStash st;
            m_staged.CopyAuthoredInto(st.value);
            st.why = "world load";
            // Keyed by the session's identity: an in-place subject's is its scene
            // target, not the shared clone path.
            st.label = SessionStashLabel();
            const std::string key = SessionStashKey();
            m_stagedStash[key] = std::move(st);
            ++m_stashes;
            LOG_INFO("[CHAINPREVIEW] staged stashed: subject='%s' damping=%.4f dirty=1 "
                     "stashes=%u why=world load key='%s'", m_subjectPath.c_str(),
                     m_staged.damping, m_stashes, key.c_str());
        }
        if (m_followSrc != NullEntity || m_controller == Controller::FollowSceneEntity)
            FollowFallback("world clear");
        if (hadStage)
        {
            m_stageRoot = m_stageChain = m_camEnt = NullEntity;
            m_stageRootH = m_stageChainH = m_camEntH = NullEntityHandle;
            m_stageHandles.clear();
            m_stageEnts.clear();
            m_stageSet.clear();
            // The respawned stage's first picture must wait for its framing again (the
            // sky-only spawn-frame picture, and the renderer's one-shot "world cull kept
            // 0" warning).
            m_requesting = false;
            // The pinned ids now name someone else's skeletons (or nothing): left
            // pinned, a scene character would animate off-screen forever. The alias
            // lived on the dropped stage; the clip is re-registered on the respawned one.
            m_stagePin.clear();
            // The same for the preview lane: recycled ids would be simulated on the
            // panel's clock and excluded from the scene's lane. Emptied here; the pointer
            // stays until a close path hands it back (an empty set makes both lanes
            // ignore it).
            m_previewSet.clear();
            // The A/B shadow's id went with the World too (dropped, never destroyed - it
            // names somebody else now), and the solo must be handed to the respawned
            // carrier again.
            m_abEnt  = NullEntity;
            m_abEntH = NullEntityHandle;
            m_abDbgValid = false;
            m_soloAppliedTo = NullEntity;
            m_animEnt   = NullEntity;
            m_clipAlias.clear();
            m_clipBound = false;
            m_clipApplyPending = !m_clipPath.empty();
            m_focusValid = false;
            // A user-picked subject goes back to "unselected"; a harness subject is kept
            // so a pack survives its own scene load.
            if (!m_subjectFromHarness && !m_suspendedForPlay)
            {
                m_subjectPath.clear();
                m_status = "World reloaded - pick the subject again.";
            }
        }
    }

    // ---- stage entities destroyed outside the panel ----
    ValidateStageHandles(ctx);
    // ---- -chainpreview_shot (the previous frame's picture) ----
    TickShots(ctx);
    // ---- the harness Reset windows' A/B (before anything this frame can reset
    // the chains)
    TickResetWindows(ctx);

    // ---- harness open / close hooks ----
    if (m_harnessOpenAt != 0 && !m_harnessOpened && m_tick >= m_harnessOpenAt)
    {
        m_harnessOpened = true;
        m_open = true;
        LOG_INFO("[CHAINPREVIEW] panel opened by the harness at editor frame %llu "
                 "(-chainpreview_open_at)", static_cast<unsigned long long>(m_tick));
    }
    if (!m_harnessClosed && m_open &&
        ((m_harnessCloseAt != 0 && m_tick >= m_harnessCloseAt) ||
         (m_harnessFrames != 0 && m_spawnTick != 0 &&
          m_tick >= m_spawnTick + m_harnessFrames)))
    {
        m_harnessClosed = true;
        m_open = false;
        LOG_INFO("[CHAINPREVIEW] panel closed by the harness at editor frame "
                 "%llu (%s)", static_cast<unsigned long long>(m_tick),
                 m_harnessCloseAt != 0 ? "-chainpreview_close_at"
                                       : "-chainpreview_frames");
    }

    // ---- harness requests, through the UI's own guarded paths. Here, before
    // the visibility gate, because "open" must be served while the panel is
    // closed.
    TickHarnessRequests(ctx);

    // ---- the census the isolation pack reads ----
    if (!m_harnessSubject.empty())
    {
        if (m_censusOpenAt != 0 && m_tick >= m_censusOpenAt && m_stageRoot != NullEntity)
        {
            m_censusOpenAt = 0;
            m_censusOpen = TakeCensus(ctx);
            LogCensus("open", m_censusOpen);
        }
        if (m_censusPostAt != 0 && m_tick >= m_censusPostAt)
        {
            m_censusPostAt = 0;
            m_censusPost = TakeCensus(ctx);
            LogCensus("post", m_censusPost);
            if (m_censusPre.valid && m_censusOpen.valid && !m_isolationLogged)
            {
                m_isolationLogged = true;
                const Census& a = m_censusPre;
                const Census& o = m_censusOpen;
                const Census& p = m_censusPost;
                // TWO lines because the logger formats into 1024 bytes. The
                // pack asserts pre == open for what the STRIP must hold still,
                // and pre == post for what the close must give back.
                LOG_INFO("[CHAINPREVIEW] isolation components: preBodies=%u "
                         "openBodies=%u postBodies=%u preJolt=%u openJolt=%u "
                         "postJolt=%u preAi=%u openAi=%u postAi=%u preAiComps=%u "
                         "openAiComps=%u postAiComps=%u preShadowSet=%u "
                         "openShadowSet=%u postShadowSet=%u preShadowRoots=%u "
                         "openShadowRoots=%u postShadowRoots=%u preLoco=%u "
                         "openLoco=%u postLoco=%u preScripts=%u openScripts=%u "
                         "postScripts=%u",
                         a.joltBodies, o.joltBodies, p.joltBodies,
                         a.joltComps, o.joltComps, p.joltComps,
                         a.aiAgents, o.aiAgents, p.aiAgents,
                         a.aiComps, o.aiComps, p.aiComps,
                         a.shadowSet, o.shadowSet, p.shadowSet,
                         a.shadowRoots, o.shadowRoots, p.shadowRoots,
                         a.locomotion, o.locomotion, p.locomotion,
                         a.scripts, o.scripts, p.scripts);
                LOG_INFO("[CHAINPREVIEW] isolation resources: preOwned=%u "
                         "openOwned=%u postOwned=%u preOrphans=%u openOrphans=%u "
                         "postOrphans=%u preShared=%u postShared=%u preDraws=%u postDraws=%u preLeaves=%u "
                         "postLeaves=%u preSkeletons=%u openSkeletons=%u "
                         "postSkeletons=%u preEntities=%u openEntities=%u "
                         "postEntities=%u preVramMB=%.1f postVramMB=%.1f "
                         "vramDeltaMB=%.1f released=%llu residualShared=%u "
                         "stageSources=%u",
                         a.skinnedOwned, o.skinnedOwned, p.skinnedOwned,
                         a.skinnedOrphans, o.skinnedOrphans, p.skinnedOrphans,
                         a.sharedSources, p.sharedSources,
                         a.drawPackets, p.drawPackets, a.bvhLeaves, p.bvhLeaves,
                         a.skeletons, o.skeletons, p.skeletons,
                         a.entities, o.entities, p.entities,
                         a.vramMB, p.vramMB, p.vramMB - a.vramMB,
                         static_cast<unsigned long long>(
                             renderer->GetSkinnedUnsnapshottedReleased()),
                         // The shared skinned-source entries the stage added and left
                         // behind (the cache is world-scoped, freed only at the next
                         // world clear - see chain_preview_isolation_base), beside the
                         // number of distinct sources the stage's parts used.
                         p.sharedSources > a.sharedSources
                             ? p.sharedSources - a.sharedSources : 0u,
                         m_stageSources);
            }
        }
    }

    const bool active = m_stageRoot != NullEntity || m_camEnt != NullEntity ||
                        m_viewId != ~0u || m_globalsOverridden;
    const bool previewOn = renderer->IsPreviewEnabled();
    const bool visible   = m_open && drewLast;

    // ---- AUTO-RELEASE / CLOSE: one path for every way the panel goes away ----
    if (!visible || !ctx.editing || !previewOn)
    {
        if (active)
        {
            const char* reason = !ctx.editing ? "play"
                               : m_harnessClosed && !m_open ? "harness close"
                               : !m_open ? "closed" : !previewOn ? "kill switch"
                               : "not drawn";
            if (ctx.editing && !visible && !(m_harnessClosed && !m_open))
            {
                ++m_autoReleases;
#if defined(WITH_HARNESS) && WITH_HARNESS
                // THE ORACLE: a panel the harness asked for that stops drawing
                // turns every downstream assertion red at once without saying
                // why. Opt-in, like the camera panel's, because an intended
                // close also ends here.
                if (HarnessVerifyOn() && !m_harnessSubject.empty() && !m_harnessClosed)
                    ENGINE_CHECK_ONCE(false,
                        "-chainpreview_subject='%s' was asked for, but the Chain "
                        "Physics Preview panel stopped drawing after %llu frame(s) "
                        "(beginRefused=%llu) and its stage is being torn down. "
                        "Nothing downstream of this can produce a picture.",
                        m_harnessSubject.c_str(),
                        static_cast<unsigned long long>(m_drewFrames),
                        static_cast<unsigned long long>(m_beginRefusedFrames));
#endif
            }
            TearDown(renderer, world, reason, /*destroyEntities=*/true);
            if (!ctx.editing) m_suspendedForPlay = true;
            // The close confirmation's "Discard": TearDown just captured the
            // edits for the next spawn; a discarded session must not bring
            // them back.
            if (m_pendingDiscard)
            {
                m_pendingDiscard = false;
                m_stagedCarry = false;
                m_stagedDirty = false;
                if (m_loadedValid) m_loadedValue.CopyAuthoredInto(m_staged);
                // The discarded states must not come back through Ctrl+Z /
                // Ctrl+Y on a reopen of the same subject: the panel-undo ring
                // goes with them.
                m_undo.clear();
                m_redo.clear();
                m_undoSessionReset = true;
                m_stagedStash.erase(SessionStashKey());   // nor via the re-apply offer
                LOG_INFO("[CHAINPREVIEW] close: unapplied staged edits DISCARDED (%s) "
                         "undoDepth=%u redoDepth=%u", reason, static_cast<unsigned>(m_undo.size()),
                         static_cast<unsigned>(m_redo.size()));
            }
        }
        return;
    }
    m_suspendedForPlay = false;

    // ---- the dirty confirmation's answer, before the subject requests it may
    // post.
    TickGuard(ctx);
    // ---- a Play-time Inspector request named the LIVE target of that Play
    // session only.
    if (m_liveRequestH.entity != NullEntity)
    {
        m_liveRequestH = NullEntityHandle;
        LOG_INFO("[CHAINPREVIEW] inspector edit (play): LIVE target request cleared at Stop");
    }
    // ---- the Inspector's "Edit Chains..." ----
    TickEditRequest(ctx);
    // ---- a "Keep these values" slot whose character is not the subject posts
    // its guarded Use Selected here, before the subject requests below, so the
    // stage respawns once - as that character - rather than first as the old
    // subject (called again after the spawn to stage the values).
    TickPlayKeep(ctx);

    // ---- subject requests from Draw ----
    if (m_pendingUseSelected)
    {
        m_pendingUseSelected = false;
        Entity e = m_pendingSelected;
        m_pendingSelected = NullEntity;
        if (e == NullEntity || !world->IsAlive(e))
            m_status = "Use Selected: nothing selected.";
        else if (world->HasComponent<PreviewStageTag>(e))
            m_status = "Use Selected: that entity IS the preview stage.";
        else
        {
            // The instance ROOT: PrefabInstanceMember names it for every entity
            // LoadPrefab created; otherwise walk Parent to the top.
            Entity root = e;
            if (const PrefabInstanceMember* pim = world->GetComponent<PrefabInstanceMember>(e))
                if (pim->root != NullEntity && world->IsAlive(pim->root)) root = pim->root;
            for (int guard = 0; guard < 256; ++guard)
            {
                const Parent* par = world->GetComponent<Parent>(root);
                if (!par || par->entity == NullEntity || !world->IsAlive(par->entity)) break;
                root = par->entity;
            }
            // The selected character is Apply -> Scene's default target - for an
            // in-place subject the original (the stage is spawned from a clone of
            // it).
            m_pendingSceneTargetH = world->MakeHandle(root);
            // The entity the user pointed at, when it is a chain carrier (the
            // Inspector's "Edit Chains..." always names one), is the carrier
            // Apply writes - not the first carrier of the root's DFS.
            m_pendingSceneCarrierH =
                (!EditNaiveArm() && world->HasComponent<ChainPhysicsComponent>(e) &&
                 world->HasComponent<SkeletonComponent>(e))
                    ? world->MakeHandle(e) : NullEntityHandle;
            // ...and the stage is spawned with its counterpart as the carrier
            // (SpawnStage).
            m_pendingCarrierPref = CarrierPref{};
            if (world->IsHandleValid(m_pendingSceneCarrierH))
            {
                std::vector<Entity> cs;
                ChainCarriersUnder(*world, root, false, cs);
                for (std::size_t i = 0; i < cs.size(); ++i)
                    if (cs[i] == e) m_pendingCarrierPref.ordinal = static_cast<int>(i);
                if (m_pendingCarrierPref.ordinal >= 0)
                    m_pendingCarrierPref.path = CarrierNamePath(*world, root, e);
            }
            const PrefabSourcePath* psp = world->GetComponent<PrefabSourcePath>(root);
            if (psp && !psp->path.empty())
            {
                m_pendingSubject        = psp->path;
                m_pendingSubjectInPlace = false;
                // The instance's scale travels with it: scenes place prefab
                // characters scaled (AIBench: 0.1) and the stage should be the
                // size the scene shows. The chain solver simulates in skeleton
                // space, so the entity scale does not change the simulation
                // (bit-identical particles at 0.1 and 1.0); it sets the stage's
                // world size: floor, drive distances, camera.
                if (const LocalTransform* rlt = world->GetComponent<LocalTransform>(root))
                {
                    m_pendingScale[0] = rlt->scale.x;
                    m_pendingScale[1] = rlt->scale.y;
                    m_pendingScale[2] = rlt->scale.z;
                    m_pendingScaleValid = true;
                }
            }
            else
            {
                // In-place: clone the authored character through the prefab
                // pipeline (the clipboard's route - it remaps ids and reseeds
                // GUIDs) and spawn the stage from the clone. Apply targets the
                // original entity.
                namespace fs = std::filesystem;
                std::error_code ec;
                const fs::path dir = Sys::U8Path(Sys::WritableDataDir()) / "cache";
                fs::create_directories(dir, ec);
                const std::string clone = Sys::PathToUtf8(dir / "chain_preview_subject.ipfb");
                if (Resource::SavePrefab(root, *world, clone))
                {
                    m_pendingSubject        = clone;
                    m_pendingSubjectInPlace = true;
                }
                else
                    m_status = "Use Selected: could not clone the selection.";
            }
            if (!m_pendingSubject.empty())
            {
                std::snprintf(m_subjectEdit, sizeof(m_subjectEdit), "%s",
                              m_pendingSubject.c_str());
                // Use Selected always respawns: the in-place clone is rewritten
                // under the same file name for every character, and a prefab
                // instance of the current subject may carry a different scale.
                // The path alone cannot tell.
                m_pendingForceRespawn = true;
            }
            else
                m_pendingSceneTargetH = NullEntityHandle;   // no subject, no target
        }
    }
    if (!m_pendingSubject.empty())
    {
        std::string next = std::move(m_pendingSubject);
        m_pendingSubject.clear();
        if (next != m_subjectPath || m_spawnFailed || m_stageRoot == NullEntity ||
            m_pendingForceRespawn)
        {
            if (m_stageRoot != NullEntity) DestroyStage(renderer, *world);
            m_subjectPath    = next;
            m_subjectInPlace = m_pendingSubjectInPlace;
            m_stageScaleValid = m_pendingScaleValid;
            if (m_pendingScaleValid)
                for (int k = 0; k < 3; ++k) m_stageScale[k] = m_pendingScale[k];
            if (m_subjectPath != m_harnessSubject) m_subjectFromHarness = false;
            m_spawnFailed    = false;
            m_status.clear();
            // An explicit subject request starts a new subject session: the
            // previous stage's captured parameters belong to it, not to this one
            // (the in-place clone path is the same file for every character, so
            // the path cannot decide).
            m_stagedCarry = false;
            m_undoSessionReset = true;   // and its undo history
            m_userScale = false;         // the default scale rule again
            // A new subject session: its Scene target (Use Selected's source, or
            // none for a Load / drop), the default Apply destination and the
            // load-time baseline start over.
            m_sceneTargetH  = m_pendingSceneTargetH;
            m_sceneCarrierH = m_pendingSceneCarrierH;
            m_carrierPref   = m_pendingCarrierPref;
            m_applyDestUser = false;
            m_loadedMoved   = false;
            // A session with a Use Selected target never falls back to the live
            // selection; its identity (name + Guid) is what an epoch change is
            // checked against.
            m_sceneTargetSet  = world->IsHandleValid(m_sceneTargetH);
            m_sceneTargetLost = false;
            if (m_sceneTargetSet) RecordSceneTarget(*world, m_sceneTargetH.entity);
            m_reapplyOffer = false;       // an offer belongs to its spawn
        }
        // Consumed whichever branch ran: a skipped request must not leave its
        // scale / in-place / force state for the next Load.
        m_pendingScaleValid     = false;
        m_pendingSubjectInPlace = false;
        m_pendingForceRespawn   = false;
        m_pendingSceneTargetH   = NullEntityHandle;
        m_pendingSceneCarrierH  = NullEntityHandle;
        m_pendingCarrierPref    = CarrierPref{};
    }
    if (m_pendingReload)
    {
        m_pendingReload = false;
        if (m_stageRoot != NullEntity) DestroyStage(renderer, *world);
        m_spawnFailed = false;
        // Reload means "from disk": the captured edits are dropped.
        m_stagedCarry = false;
        m_undoSessionReset = true;   // and the panel-undo history
    }

    // ---- the stage, with the process-global overrides ----
    if (m_stageRoot == NullEntity && !m_subjectPath.empty() && !m_spawnFailed)
    {
        if (!m_harnessSubject.empty() && m_spawnCount == 0 && !m_censusPre.valid)
        {
            m_censusPre = TakeCensus(ctx);
            LogCensus("pre", m_censusPre);
        }
        if (!m_globalsOverridden)
        {
            m_globalsOverridden = true;
            // The anim pinned set: the stage sits outside the main frustum and
            // every shadow cascade, so PrepareAnimationVisibility would classify
            // it kCulled and it would draw its bind pose; pinning its skeletons
            // admits them alone and leaves the scene's culling as the user set
            // it. SpawnStage fills the set and only this serial window rewrites
            // it; every close path clears the pointer (RestoreGlobals).
            if (!m_harnessPinOff)
            {
                // One owner among several: the Skeleton Editor's stage pins its
                // own set beside this one.
                renderer->AddAnimPinOwner(this, &m_stagePin);
                m_pinPushed = true;
            }
            if (ChainPhysicsSystem* cps = renderer->GetChainPhysicsSystem())
            {
                // Chain LOD measures from the main camera, ~4 km from the stage:
                // it would drop the subject straight to tier 2 (no collision).
                m_savedLodEnabled = cps->lodEnabled;
                // -chainpreview_lod_keep (reverse-arm evidence only) leaves it as
                // it was, so the tier / iterations fields of the lane line and the
                // forbidden "ChainPhysics LOD: camera=" line can be shown to catch
                // a missing override.
                if (!m_harnessLodKeep) cps->lodEnabled = false;
            }
        }
        if (!SpawnStage(ctx)) RestoreGlobals(renderer, "subject refused");
    }

    // ---- no stage (no subject yet, or it was refused): hold nothing ----
    // A view, a camera or the global overrides left over from a previous
    // subject would otherwise sit there for as long as the panel stays open.
    // The harness deadline: a harness subject that has not made a stage in
    // kHarnessDeadline editor frames is a precondition failure, not a run that
    // measures an empty panel under the pack's name.
    if (m_stageRoot == NullEntity)
    {
        if (m_viewId != ~0u || m_camEnt != NullEntity || m_globalsOverridden)
            TearDown(renderer, world,
                     m_subjectPath.empty() ? "no subject" : "subject refused",
                     /*destroyEntities=*/true);
#if defined(WITH_HARNESS) && WITH_HARNESS
        if (!m_harnessSubject.empty() && !m_harnessDeadlineFired && m_spawnCount == 0 &&
            ++m_harnessTries >= kHarnessDeadline)
        {
            m_harnessDeadlineFired = true;
            LOG_ERROR("[CHAINPREVIEW] -chainpreview_subject='%s' never produced a "
                      "stage in %u editor frames (%s). The panel is showing nothing "
                      "and this run would measure the no-panel case. Refusing to run.",
                      m_harnessSubject.c_str(), kHarnessDeadline,
                      m_status.empty() ? "no reason recorded" : m_status.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
#endif
        return;
    }

    // ---- the view: only while there is a stage to look at ----
    if (m_stageRoot != NullEntity && m_viewId == ~0u &&
        (!m_viewUnavailable || (m_tick % 60u) == 0u))
    {
        m_viewId = renderer->AcquirePreviewView(/*worldCull=*/true);
        m_viewUnavailable = (m_viewId == ~0u);
        m_laneAskedW = m_laneAskedH = 0;
        m_sharingLane = false;
        m_requesting  = false;
    }

    if (m_pendingAutoDetect)
    {
        m_pendingAutoDetect = false;
        if (m_stageChain != NullEntity && m_noChain &&
            !world->HasComponent<ChainPhysicsComponent>(m_stageChain))
        {
            const SkeletonComponent* skc = world->GetComponent<SkeletonComponent>(m_stageChain);
            SkeletonRegistry& reg = renderer->GetSkeletonRegistry();
            if (skc && skc->assetIndex < reg.Count())
            {
                ChainPhysicsComponent cfg{};
                ChainPhysicsSystem::BuildGroupsFromNames(reg.Get(skc->assetIndex), cfg);
                world->AddComponent<ChainPhysicsComponent>(m_stageChain, cfg);
                m_staged = cfg;
                m_stagedValid = true;
                m_noChain = false;
                // Nothing was loaded: the new component is unapplied work
                // (dirty) and Revert has no target.
                m_loadedValid = false;
                // A new component: the sync baseline is the value just added,
                // and there is no history before it.
                ResetStagedTracking(/*clearHistory=*/true);
                LOG_INFO("[CHAINPREVIEW] auto-detected %u chain group(s) on the "
                         "stage from bone names", static_cast<unsigned>(cfg.groups.size()));
            }
        }
    }

    // ---- -chainpreview_param (each at its editor frame, into m_staged) ----
    ApplyHarnessParams(ctx);
    // ---- presets + Revert, into m_staged (each one panel-undo step; synced by
    // TickStagedParams below in this same window).
    TickPresets(ctx);
    // ---- the "Re-apply staged params" answer, into m_staged (one panel-undo
    // step, synced below).
    TickReapply(ctx);
    // ---- "Keep these values" from Play, into m_staged once its character is
    // the subject (one panel-undo step).
    TickPlayKeep(ctx);
    // ---- harness scene events (a wind event writes m_staged, synced below in
    // this same window; gravity / drive / teleport latch state the clock-driven
    // ticks consume).
    TickSceneHarness(ctx);
    // ---- the in-picture rest-pose gizmo's gesture (auto-pause edges) and its
    // harness driver - before TickStagedParams, whose commit and sync it holds
    // while a drag is open.
    TickPoseGizmo(ctx);
    // ---- the one writer of the stage's live component ----
    // Commit a finished edit to the panel-undo ring, apply Ctrl+Z / Ctrl+Y, then
    // sync m_staged into the live ChainPhysicsComponent field by field (and
    // Invalidate on a structural change). Before the clock and this frame's chain
    // Update, so a re-init happens in this frame.
    TickStagedParams(ctx);
    // ---- A/B (B's entity, the buttons, B's one writer) and the per-group solo -
    // both before the clock and this frame's chain Update (a solo change / a new B
    // re-initialises the chains in this frame).
    TickAb(ctx);
    TickSolo(ctx);
    // ---- Apply (the live component already equals m_staged)
    TickApply(ctx);
    // ---- -chainpreview_scene_save (after the applies, so the written file is
    // what a Ctrl+S would save now)
    TickSceneSave(ctx);
    // ---- Draw's scene requests + the floor entity ----
    TickScene(ctx);
    // ---- the display mode - after the floor exists, before this frame's
    // BuildScene reads the stage's visibility.
    TickDisplay(ctx);

    // ---- the simulation transport, then the clock ----
    // TickSimTransport applies Draw's Simulate / Pause / Step / Reset / time
    // scale / dt requests and the harness hooks; TickStageClock turns them into
    // this frame's one step count, which UpdateAnimation (the clip) and
    // UpdateSimulation (the chain lane) both consume below.
    TickSimTransport(ctx);
    TickStageClock(ctx);
    // ---- the root-motion driver (the one root LocalTransform writer) on this
    // frame's steps, then FollowSceneEntity's clip + time - both before this
    // frame's TransformSystem / AnimationSystem / chains.
    TickDriver(ctx);
    TickFollow(ctx);

    // ---- controller, clip, transport and the per-frame clock write ----
    // Before the view gate below: the stage animates (and the read-out counts)
    // whether or not a preview view could be acquired this frame.
    UpdateAnimation(ctx);
    // ---- the chain solver's preview lane (reset requests + the push) ----
    // After UpdateAnimation, whose transport latches the resets; before the
    // view gate, for the same reason: the stage simulates whether or not a
    // view could be acquired this frame.
    UpdateSimulation(ctx);
    // ---- the sync-probe read-out (after every panel write this frame, before
    // this frame's ApplyRestPose) and the UI's Apply target cache.
    TickSyncProbe(ctx);
    RefreshApplyTargets(ctx);
#if defined(WITH_HARNESS) && WITH_HARNESS
    // The periodic clock line (harness runs only; every close prints one).
    if (!m_harnessSubject.empty() && m_stageRoot != NullEntity && (m_tick % 60u) == 0u)
    {
        LogClock("periodic");
        LogScene("periodic");
        LogDrive("periodic");
        LogSyncProbe("periodic");
    }
#endif
    // ---- the solver snapshot + the stats overlay (serial; Draw reads only
    // m_statsBox / the overlay snapshot)
    TickStats(ctx);
    // ---- B's snapshot + the A/B divergence (reads TickStats' A snapshot), then
    // the poke (the picked particle comes from that snapshot; this frame's
    // preview passes consume the impulse).
    TickAbReadout(ctx);
    TickPoke(ctx);

    // (The harness deadline - never retry in silence - lives in the no-stage
    // branch above: it is the only state in which it can be due.)

    // ---- camera + request ----
    if (m_stageRoot == NullEntity || m_viewId == ~0u) return;
    EnsureCamera(*world);
    UpdateCamera(*world, *renderer);
    // ---- the overlay snapshot (serial; Draw reads only it) ----
    TickOverlay(ctx);
    // ---- what the in-picture gizmo draws / edits
    TickPoseSnapshot(ctx);
    // THE FIRST REQUEST WAITS FOR THE STAGE TO BE IN PLACE. On the spawn frame
    // and the one after it the parts still sit at the prefab's authored
    // position (TransformSystem has not propagated the y=-4096 root yet) and
    // are not skinned yet, so a picture taken then is sky only - and the
    // renderer's one-shot "world cull kept 0" WARNING spends itself on it.
    // Bounded: a subject whose framing never resolves is requested anyway
    // after 120 frames rather than never.
    if (!m_requesting && !m_focusValid && m_focusTries < 120) return;
    m_requesting = true;
    // PUSHED EVERY FRAME: ExtractPreviewCameras reads it on the game half, and
    // Renderer zeroes camera identities on a world clear.
    renderer->SetPreviewCameraEntity(m_viewId, m_camEnt);
    if (m_hovered) renderer->SetPreviewPriorityView(m_viewId);
    NegotiateLaneSize(*renderer);

    Preview::ViewDesc desc{};
    desc.w      = m_renderW;
    desc.h      = m_renderH;
    desc.source = Preview::Source::WorldCull;
    desc.tickIntervalFrames = 1;
    // The scene's own sky: a WorldCull view is "a second camera under your
    // scene's lighting" - there is no studio backdrop to pick.
    desc.features   = Preview::Feature::kSkybox;
    desc.background = Preview::Background::Skybox;
    // Requested every frame from then on, paused or not: a view that stops
    // requesting is idle-torn-down after kIdleTeardownFrames and rebuilt with a
    // hitch.
    renderer->RequestPreview(m_viewId, desc, MaterialComponent{});
}

// ==== Lane size: the panel yields ====
// It moves the WorldCull lane only when it holds every live WorldCull view;
// otherwise it adopts the lane's size and says so. It never suspends another
// panel's view.
void ChainPhysicsPreviewWindow::NegotiateLaneSize(Renderer& renderer)
{
    const std::uint32_t wantW = m_sizeW, wantH = m_sizeH;
    if (wantW != m_laneAskedW || wantH != m_laneAskedH || m_sharingLane)
    {
        const std::uint32_t one = m_viewId;
        if (renderer.RequestPreviewWorldCullLaneSize(wantW, wantH, &one, 1u))
        {
            if (m_sharingLane)
                LOG_INFO("[CHAINPREVIEW] lane is ours again - moving to %ux%u",
                         wantW, wantH);
            m_laneAskedW = wantW; m_laneAskedH = wantH;
            m_renderW = wantW;    m_renderH = wantH;
            m_sharingLane = false;
        }
        else
        {
            std::uint32_t lw = 0, lh = 0;
            renderer.GetPreviewWorldCullSize(lw, lh);
            if (lw != 0 && lh != 0)
            {
                if (!m_sharingLane || m_renderW != lw || m_renderH != lh)
                    LOG_INFO("[CHAINPREVIEW] sharing lane size %ux%u (wanted %ux%u; "
                             "another camera view holds the WorldCull lane)",
                             lw, lh, wantW, wantH);
                m_renderW = lw; m_renderH = lh;
                m_sharingLane = true;
            }
            else
            {
                m_renderW = wantW; m_renderH = wantH;
            }
            m_laneAskedW = wantW; m_laneAskedH = wantH;
        }
    }
    if (m_renderW == 0 || m_renderH == 0) { m_renderW = wantW; m_renderH = wantH; }
}

// ==== Spawn ====
bool ChainPhysicsPreviewWindow::SpawnStage(const ChainPreviewContext& ctx)
{
    Renderer& renderer = *ctx.renderer;
    World&    world    = *ctx.world;
    auto fail = [&](const std::string& why)
    {
        m_spawnFailed = true;
        m_status      = why;
        LOG_WARNING("[CHAINPREVIEW] subject '%s' refused: %s",
                    m_subjectPath.c_str(), why.c_str());
        return false;
    };
    if (!ctx.assetMgr) return fail("no AssetManager is wired to the editor.");
    if (!EndsWithIpfb(m_subjectPath))
        return fail("only .ipfb prefabs can be previewed (drop one from the Asset Browser).");
    if (!Resource::AssetFS::Get().Exists(m_subjectPath))   // where LoadPrefab will look
        return fail("the file does not exist.");

    // ---- LoadPrefab, and the exact set of entities it created ----
    // (Editor/PreviewStage: a diff of the alive set - SavePrefab writes
    // FollowSocket / FollowEntity followers as top-level nodes - and never
    // RecordSpawn, so there is no structural undo of the stage.)
    StageSpawn spawned = LoadStageSubject(m_subjectPath, world, *ctx.assetMgr, &renderer, ctx.animClipSys);
    if (spawned.root == NullEntity) return fail("LoadPrefab failed (see the log).");
    const Entity root = spawned.root;
    m_stageEnts = std::move(spawned.ents);
    m_stageSet  = std::move(spawned.set);
    m_stageRoot = root;

    // ---- strip (the list and its reasons: PreviewStage.cpp), in the same
    // serial window as the spawn, so no system ever sees them.
    m_stripped = StripStage(world, m_stageSet, "[CHAINPREVIEW]", m_harnessStripOff, "-chainpreview_strip_off");

    // ---- tags on every stage entity: not saved, not in the Outliner, not
    // pickable, kept out of Save as Prefab / Copy
    TagStage(world, m_stageEnts, PreviewStageTag::kChainPreview);
    world.SetName(root, "[ChainPreview] " + Stem(m_subjectPath));
    // Generation-stamped copies of every stage id, taken while they are provably
    // ours (this serial window created them).
    m_stageHandles.clear();
    m_stageHandles.reserve(m_stageEnts.size());
    for (Entity e : m_stageEnts) m_stageHandles.push_back(world.MakeHandle(e));
    m_stageRootH = world.MakeHandle(root);

    // ---- the stage origin + the stage scale ----
    // The stage takes the scale the scene uses this prefab at (ResolveStageScale)
    // - the stage's world size (the chains simulate in skeleton space, so it is
    // not their input). From here on TickDriver is the one writer of the root's
    // LocalTransform (origin + drive offset, the authored rotation + drive yaw,
    // the stage scale).
    ResolveStageScale(world, root);
    if (LocalTransform* lt = world.GetComponent<LocalTransform>(root))
    {
        m_rootBaseRot[0] = lt->rotation.x; m_rootBaseRot[1] = lt->rotation.y;
        m_rootBaseRot[2] = lt->rotation.z; m_rootBaseRot[3] = lt->rotation.w;
        lt->translation = { 0.f, kStageOriginY, 0.f };
        lt->scale = { m_stageScale[0], m_stageScale[1], m_stageScale[2] };
    }
    // A new stage starts at the driver's origin; the user's speeds survive.
    m_driveOffset[0] = m_driveOffset[1] = m_driveOffset[2] = 0.f;
    m_driveYawDeg = 0.f;
    m_driveRotTotal = 0.0;
    m_driveProgT = 0.0;
    m_driveDir = 1.f;
    m_floorRefused = false;   // the context-mesh budget is re-checked for this stage

    // ---- visibility: out of the main viewport and the shadows (the policy and
    // its reasons: PreviewStage.cpp)
    ApplyStageViewPolicy(world, m_stageEnts, m_stageSet);

    // ---- the chain carrier (not necessarily the prefab root) ----
    // The skeleton root owns SkeletonComponent; mesh children only carry a
    // SkeletonRef to it (ResolveChainTarget's hop, taken from the pool side).
    m_stageChain = NullEntity;
    m_noChain = false;
    Entity firstSkel = NullEntity;
    std::uint32_t skeletons = 0;
    m_budgetBones = 0;
    if (auto* sp = world.GetPool<SkeletonComponent>())
    {
        auto& ents = sp->Entities();
        auto& data = sp->Data();
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            if (!m_stageSet.count(ents[i])) continue;
            ++skeletons;
            m_budgetBones += data[i].boneCount;
            if (firstSkel == NullEntity) firstSkel = ents[i];
            if (m_stageChain == NullEntity &&
                world.HasComponent<ChainPhysicsComponent>(ents[i]))
                m_stageChain = ents[i];
        }
    }
    if (firstSkel == NullEntity)
    {
        DestroyStage(&renderer, world);
        return fail("This asset has no skeleton; chain physics needs one.");
    }
    // The carrier the user clicked (Use Selected / "Edit Chains..." on a carrier)
    // is the stage's carrier - the staged values are read from it and Apply
    // writes its counterpart. Matched by the name path below the root, else by the
    // DFS ordinal; with no preference, the first carrier of the root's DFS
    // (CarrierOf's rule - pool order could disagree with it on a root with two
    // carriers).
    m_stageCarrierPath.clear();
    m_stageCarrierOrdinal = -1;
    m_stageCarrierCount   = 0;
    int carrierMatch = 0;   // 2 = by name path, 1 = by ordinal, 0 = default
    {
        std::vector<Entity> cs;
        ChainCarriersUnder(world, m_stageRoot, true, cs);
        std::erase_if(cs, [this](Entity e) { return !m_stageSet.count(e); });
        m_stageCarrierCount = static_cast<std::uint32_t>(cs.size());
        std::size_t pick = cs.size();
        if (!cs.empty() && m_carrierPref.ordinal >= 0)
        {
            if (!m_carrierPref.path.empty())
                for (std::size_t i = 0; i < cs.size() && pick == cs.size(); ++i)
                    if (CarrierNamePath(world, m_stageRoot, cs[i]) == m_carrierPref.path)
                    { pick = i; carrierMatch = 2; }
            if (pick == cs.size() && static_cast<std::size_t>(m_carrierPref.ordinal) < cs.size())
            { pick = static_cast<std::size_t>(m_carrierPref.ordinal); carrierMatch = 1; }
        }
        if (pick == cs.size() && !cs.empty()) pick = 0;
        if (pick < cs.size())
        {
            m_stageChain          = cs[pick];
            m_stageCarrierOrdinal = static_cast<int>(pick);
            m_stageCarrierPath    = CarrierNamePath(world, m_stageRoot, m_stageChain);
        }
    }
    if (m_stageChain == NullEntity) { m_stageChain = firstSkel; m_noChain = true; }
    m_stageChainH = world.MakeHandle(m_stageChain);
    {
        const ChainPhysicsComponent* cc = world.GetComponent<ChainPhysicsComponent>(m_stageChain);
        LOG_INFO("[CHAINPREVIEW] stage carrier: ordinal=%d carriers=%u preferred=%d prefOrdinal=%d "
                 "matched=%d damping=%.4f path='%s' prefPath='%s'", m_stageCarrierOrdinal,
                 m_stageCarrierCount, m_carrierPref.ordinal >= 0 ? 1 : 0, m_carrierPref.ordinal,
                 carrierMatch, cc ? cc->damping : -1.f, m_stageCarrierPath.c_str(),
                 m_carrierPref.path.c_str());
    }

    // ---- the anim pinned set = every stage skeleton ----
    // Pool-driven, like the carrier scan above. Rewritten here, in the serial
    // window, never while PrepareAnimationVisibility reads it.
    m_stagePin.clear();
    if (auto* sp = world.GetPool<SkeletonComponent>())
        for (Entity e : sp->Entities())
            if (m_stageSet.count(e)) m_stagePin.insert(e);
    m_pinLogged = false;
    // ---- the preview lane's set = the whole stage (a superset of the carrier:
    // it also covers a second chain carrier and a ChainPhysicsComponent
    // auto-attached after this frame). UpdateSimulation pushes it every frame;
    // RestoreGlobals hands it back.
    m_previewSet = m_stageSet;
    // The Animator owner the clip aliases are registered on (the entity that
    // carries SkeletonComponent + AnimationComponent - QueueAnimatorPlay
    // resolves the same way).
    m_animEnt = Anim::ResolveAnimEntityOrSelf(world, m_stageChain);
    m_authoredClips.clear();
    if (const AnimationSetComponent* set = world.GetComponent<AnimationSetComponent>(m_animEnt))
        for (const AnimationClipReference& ref : set->clips)
            if (!ref.name.empty() && !ref.path.empty() && ref.name.rfind("__cpv_", 0) != 0)
                m_authoredClips.push_back({ ref.name, ref.path, ref.sourceClip });
    m_clipAlias.clear();
    m_clipBound = false;
    m_clipApplyPending = !m_clipPath.empty();
    // The selected clip carries over to a new subject, but "matched X / Y"
    // is a property of the (clip, skeleton) pair: recount it for this stage.
    m_clipInfoValid   = false;
    m_clipBoundLogged = false;

    // ---- budgets: the four real budgets ----
    // Against the live headroom (the scene's side of the same four numbers:
    // PreviewStage.cpp, MeasureStageBudget). m_budgetBones is the carrier scan's
    // sum above - the same stage skeletons MeasureStageBudget adds up.
    StageBudget bud;
    const char* over = MeasureStageBudget(world, renderer, m_stageSet, bud);
    m_budgetInstances = bud.instances;
    m_budgetSkinJobs  = bud.skinJobs;
    m_budgetVerts     = bud.verts;
    m_stageSources    = bud.sources;
    LOG_INFO("[CHAINPREVIEW] budget: instances=%u skinJobs=%u bones=%u verts=%u "
             "sceneInstances=%u sceneSkinJobs=%u sceneBones=%u sceneVerts=%u "
             "capInstances=%u capSkinJobs=%u capBones=%u capVerts=%u sources=%u",
             m_budgetInstances, m_budgetSkinJobs, m_budgetBones, m_budgetVerts,
             bud.sceneInstances, bud.sceneSkinJobs, bud.sceneBones, bud.sceneVerts,
             bud.capInstances, bud.capSkinJobs, bud.capBones, bud.capVerts, m_stageSources);
    // REFUSED AND NAMED, never truncated: a subject over a per-frame cap would
    // be parked at its rest pose at the SOURCE (kMaxSkinJobs) or overrun a ring.
    if (over)
    {
        DestroyStage(&renderer, world);
        return fail(std::string("the scene plus the subject exceed the per-frame "
                                "budget for ") + over);
    }

    // ---- m_staged ----
    // m_staged is the authority and SyncStagedToLive (PPA) is the one writer of
    // the live component. When this spawn continues the same subject session
    // after a teardown captured the live parameters (Play -> Stop, close ->
    // reopen, a collapsed tab), they are re-applied field by field
    // (CopyAuthoredInto - never a whole-struct assignment, which would clobber
    // BoneRestPose's runtime cache) and the chain is Invalidated, so the edits
    // survive the boundary.
    bool carried = false;
    if (ChainPhysicsComponent* cp = world.GetComponent<ChainPhysicsComponent>(m_stageChain))
    {
        // The load-time value (Revert's target, the dirty baseline) is the
        // component as loaded - taken before a carry below re-applies captured edits
        // - unless an Apply this session persisted a newer value to the subject's
        // own source (m_loadedMoved), which a same-session respawn (Play -> Stop)
        // must not forget. An in-place subject respawns from its (stale) clone, so
        // the moved value wins.
        const bool keepLoaded = m_loadedValid && m_loadedMoved && m_stagedCarry &&
                                m_stagedSubject == m_subjectPath;
        if (!keepLoaded)
        {
            m_loadedBlob.clear();
            cp->CopyAuthoredInto(m_loadedValue);
            m_loadedValid = true;
            m_loadedMoved = false;
            m_loadedBlob = Editor::Undo::SerializeComponentBlob(
                GetComponentRegistry(), world, m_stageChain,
                std::type_index(typeid(ChainPhysicsComponent)));
        }
        // Only when the captured value differs from the file (m_stagedDirty) - an
        // untouched subject respawns exactly as loaded, with no Invalidate - or when
        // an Apply moved the load-time value (the respawn reads an older file /
        // clone, and the captured value is the current one).
        if (m_stagedCarry && (m_stagedDirty || keepLoaded) && m_stagedValid &&
            m_stagedSubject == m_subjectPath && !m_harnessCarryOff)
        {
            m_staged.CopyAuthoredInto(*cp);
            if (ChainPhysicsSystem* cps = renderer.GetChainPhysicsSystem())
                cps->Invalidate(m_stageChain);
            carried = true;
            ++m_carryApplied;
        }
        else
            m_staged = *cp;
        m_stagedValid   = true;
        m_stagedSubject = m_subjectPath;
    }
    else
    {
        m_stagedValid = false;
        m_loadedValid = false;
        m_loadedBlob.clear();
    }
    m_stagedCarry = false;
    // The tree needs the carrier's skeleton asset (Draw reads the registry by
    // this index; it never reads the World), and the one writer's baselines start
    // at what the live component holds now (the loaded value, or the carried one
    // CopyAuthoredInto just wrote). The panel-undo ring survives a respawn of the
    // same subject session (Play -> Stop, a collapsed tab); a new subject / Reload
    // empties it.
    m_stageSkelAsset = ~0u;
    if (const SkeletonComponent* skc = world.GetComponent<SkeletonComponent>(m_stageChain))
        m_stageSkelAsset = skc->assetIndex;
    {
        const bool newSession = m_undoSessionReset || m_undoSubject != m_subjectPath;
        m_undoSessionReset = false;
        m_undoSubject      = m_subjectPath;
        ResetStagedTracking(newSession);
    }
    // Staged edits a world load interrupted for this subject are offered back -
    // never re-applied behind the user's back (the file may have moved on since).
    m_reapplyOffer = false;
    if (m_stagedValid && !m_harnessConfirmOff)
        if (auto it = m_stagedStash.find(SessionStashKey()); it != m_stagedStash.end())
        {
            // Only the same session identity: an in-place character's scene
            // target, never the shared clone path.
            m_reapplyOffer = true;
            m_reapplyKey   = it->first;
            m_reapplyLabel = it->second.label;
            ++m_reapplyOffers;
            LOG_INFO("[CHAINPREVIEW] re-apply offered: subject='%s' stashedDamping=%.4f "
                     "loadedDamping=%.4f spawn=%u offers=%u key='%s' source=%s",
                     m_subjectPath.c_str(), it->second.value.damping, m_staged.damping,
                     m_spawnCount + 1, m_reapplyOffers, it->first.c_str(),
                     it->second.label.c_str());
        }

    // The stage tree's rest-pose tool state belongs to this carrier.
    m_stagePose.Reset(m_stageChain);

    m_focusValid = false;
    m_focusTries = 0;
    m_focusHoldUntil = 0;
    m_focus[0] = 0.f; m_focus[1] = kStageOriginY + 0.9f; m_focus[2] = 0.f;
    m_radius   = 0.9f;
    m_subjectHeight = 0.f;
    m_pan[0] = m_pan[1] = m_pan[2] = 0.f;
    ++m_spawnCount;
    m_spawnTick = m_tick;
    if (!m_harnessSubject.empty() && m_spawnCount == 1)
        m_censusOpenAt = m_tick + kCensusOpenDelay;
    // A new stage is a new clock session: the totals the "[CHAINPREVIEW] clock:"
    // line reports start here, nothing is banked, and the chains have no state
    // yet. The user's settings (Simulate, Pause, time scale, dt) survive a
    // respawn (Play -> Stop, Reload).
    m_clock.Rewind();
    m_clock.ResetTotals();
    m_laneHasState = m_simStale = false;
    m_lastPush = m_prevPush = PushRecord{};
    m_prevPoseValid = false;
    m_holdChecks = m_holdMismatches = 0;
    m_statsMaxTier2 = m_statsMaxInterpen = 0;   // per stage session
    m_sceneCpuMsAtOpen = Anim::AnimProfile::Get().chainCpuMs;
    if (m_abPhase != AbPhase::Off && m_abPhase != AbPhase::Done)
    {
        m_abPhase = AbPhase::Wait;   // a respawn mid-A/B starts it over
        m_abWait  = 0;
    }

    if (m_harnessPinOff)
        LOG_WARNING("[CHAINPREVIEW] anim pinned set SKIPPED (-chainpreview_pin_off): "
                    "the stage's %u skeleton(s) are left to the main camera's "
                    "anim cull and draw their bind pose",
                    static_cast<unsigned>(m_stagePin.size()));
    else
        LOG_INFO("[CHAINPREVIEW] anim pinned set size=%u (the stage's skeletons; "
                 "anim culling stays %s for the scene)",
                 static_cast<unsigned>(m_stagePin.size()),
                 renderer.IsAnimCullingEnabled() ? "ON" : "OFF");
    m_pinLogged = true;
    LOG_INFO("[CHAINPREVIEW] stage spawned root=%u entities=%u origin=(0,%.0f,0) "
             "stripped=%u spawns=%u subject='%s'%s",
             static_cast<unsigned>(root), static_cast<unsigned>(m_stageEnts.size()),
             kStageOriginY, m_stripped, m_spawnCount, m_subjectPath.c_str(),
             m_subjectInPlace ? " (in-place clone)" : "");
    LOG_INFO("[CHAINPREVIEW] subject resolved parts=%u skinned=%u skeletons=%u "
             "chainEntity=%u chainGroups=%u restPoseBones=%u noChain=%d",
             m_budgetInstances, m_budgetSkinJobs, skeletons,
             static_cast<unsigned>(m_stageChain),
             m_stagedValid ? static_cast<unsigned>(m_staged.groups.size()) : 0u,
             m_stagedValid ? static_cast<unsigned>(m_staged.restPose.size()) : 0u,
             m_noChain ? 1 : 0);
    // What the live component holds after this spawn - the read-back
    // chain_preview_carry asserts across Play -> Stop.
    if (const ChainPhysicsComponent* cp = world.GetComponent<ChainPhysicsComponent>(m_stageChain))
        LOG_INFO("[CHAINPREVIEW] stage params: spawn=%u carried=%d carryApplied=%u "
                 "damping=%.4f gravity=%.4f iterations=%d substeps=%d groups=%u "
                 "g0ovrDamping=%.4f restPoseBones=%u carryOff=%d stageTagged=%u",
                 m_spawnCount, carried ? 1 : 0, m_carryApplied, cp->damping,
                 cp->gravity, cp->iterations, cp->substeps,
                 static_cast<unsigned>(cp->groups.size()),
                 cp->groups.empty() ? 0.f : cp->groups[0].ovrDamping,
                 static_cast<unsigned>(cp->restPose.size()), m_harnessCarryOff ? 1 : 0,
                 // Every PreviewStageTag entity in the World right after
                 // this spawn: the new stage alone, unless an older stage
                 // survived somewhere (e.g. baked into a Play snapshot).
                 StageTagCount(world, PreviewStageTag::kChainPreview));
    LOG_INFO("[CHAINPREVIEW] scale: value=%.4f scaleX=%.4f scaleY=%.4f scaleZ=%.4f "
             "source=%s instance=%u spawn=%u", m_stageScale[0], m_stageScale[0],
             m_stageScale[1], m_stageScale[2], m_scaleSource,
             static_cast<unsigned>(m_scaleInstance), m_spawnCount);
    m_status = "Subject: " + m_subjectPath;
    return true;
}

// ==== The panel's camera entity: "__ChainPreviewCam" ====
// It carries a VirtualCameraComponent and no CameraComponent. Do not give it a
// CameraComponent: ExtractPreviewCameras accepts either lens, but
// App::RefreshMainCamera falls back to "the first entity with a
// CameraComponent" whenever a scene has no ActiveCameraTag, so the preview
// camera could become the scene's main camera (and Lua's CameraEntity).
// Without VCamPriority / VCamBlend it never joins the VCam stack either.
void ChainPhysicsPreviewWindow::EnsureCamera(World& world)
{
    // The handle decides: a recycled id is somebody else's entity.
    if (m_camEnt != NullEntity && world.IsHandleValid(m_camEntH)) return;
    m_camEnt  = CreateStageCamera(world, "__ChainPreviewCam", PreviewStageTag::kChainPreview);
    m_camEntH = world.MakeHandle(m_camEnt);
}

void ChainPhysicsPreviewWindow::UpdateCamera(World& world, Renderer& renderer)
{
    using namespace DirectX;
    // ---- focus: the stage skeleton's bind-pose joints, once in place ----
    // Not the skinned parts' WorldAabb, and not MeshSkinnedComponent's box: both
    // are deliberately conservative cull bounds (e.g. 12.9 x 20 x 10.5 m per part
    // around a 1.6 m character) that would frame the subject as a speck. The
    // bind-pose joints, taken through the part's GlobalTransform, are tight and
    // are what a chain preview frames anyway. Accepted only once the transform
    // has moved to the stage (centre well below y = 0) - the frame after the
    // spawn it still reads the prefab's authored position. A subject whose
    // skeleton cannot be read falls back to the WorldAabb union. After a Scale
    // change the framing waits until the new transforms have propagated
    // (m_focusHoldUntil), or it frames the old size.
    if (!m_focusValid && m_focusTries < 600 && m_tick >= m_focusHoldUntil)
    {
        ++m_focusTries;
        XMFLOAT3 mn{ 1e30f, 1e30f, 1e30f }, mx{ -1e30f, -1e30f, -1e30f };
        bool any = false, moved = true, fromJoints = false;
        auto grow = [&](const XMFLOAT3& lo, const XMFLOAT3& hi)
        {
            mn.x = std::min(mn.x, lo.x); mn.y = std::min(mn.y, lo.y); mn.z = std::min(mn.z, lo.z);
            mx.x = std::max(mx.x, hi.x); mx.y = std::max(mx.y, hi.y); mx.z = std::max(mx.z, hi.z);
            any = true;
        };
        // Pool-driven: which skeleton asset each stage skinned part deforms
        // with (its own SkeletonComponent, or the one its SkeletonRef names),
        // then the parts' transforms from the GlobalTransform pool - no
        // per-entity Get loop. The part's transform, not the skeleton
        // entity's: a skinned vertex lands at partWorld x (bone x invBind x
        // rest), so a scale authored on the mesh child (a skeleton root can
        // read a 23 m joint box for a ~1.7 m drawn character) is only seen
        // from the part.
        SkeletonRegistry& reg = renderer.GetSkeletonRegistry();
        std::unordered_map<Entity, std::uint32_t> skelOfEntity;
        if (auto* sp = world.GetPool<SkeletonComponent>())
        {
            auto& ents = sp->Entities();
            auto& data = sp->Data();
            for (std::size_t i = 0; i < ents.size(); ++i)
                if (m_stageSet.count(ents[i]) && data[i].assetIndex < reg.Count())
                    skelOfEntity.emplace(ents[i], data[i].assetIndex);
        }
        std::unordered_map<Entity, std::uint32_t> skelAsset;   // skinned part -> asset
        if (auto* kp = world.GetPool<MeshSkinnedComponent>())
            for (Entity e : kp->Entities())
                if (m_stageSet.count(e))
                {
                    const auto own = skelOfEntity.find(e);
                    if (own != skelOfEntity.end()) skelAsset.emplace(e, own->second);
                }
        if (auto* rp = world.GetPool<SkeletonRef>())
        {
            auto& ents = rp->Entities();
            auto& data = rp->Data();
            for (std::size_t i = 0; i < ents.size(); ++i)
            {
                if (!m_stageSet.count(ents[i])) continue;
                const auto it = skelOfEntity.find(data[i].entity);
                if (it != skelOfEntity.end()) skelAsset.emplace(ents[i], it->second);
            }
        }
        if (!skelAsset.empty())
            if (auto* gp = world.GetPool<GlobalTransform>())
            {
                auto& ents = gp->Entities();
                auto& data = gp->Data();
                for (std::size_t i = 0; i < ents.size(); ++i)
                {
                    const auto it = skelAsset.find(ents[i]);
                    if (it == skelAsset.end()) continue;
                    const SkeletonAsset& sa = reg.Get(it->second);
                    const XMMATRIX M = XMLoadFloat4x4(&data[i].matrix);
                    XMVECTOR lo = XMVectorReplicate(1e30f), hi = XMVectorReplicate(-1e30f);
                    const std::uint32_t n = std::min<std::uint32_t>(sa.boneCount,
                                                                    SkeletonAsset::MAX_BONES);
                    for (std::uint32_t k = 0; k < n; ++k)
                    {
                        const XMFLOAT4X4& bp = sa.bindPose[k];
                        const XMVECTOR p = XMVector3TransformCoord(
                            XMVectorSet(bp._41, bp._42, bp._43, 1.f), M);
                        lo = XMVectorMin(lo, p);
                        hi = XMVectorMax(hi, p);
                    }
                    if (n == 0) continue;
                    XMFLOAT3 l, h;
                    XMStoreFloat3(&l, lo); XMStoreFloat3(&h, hi);
                    if (!(0.5f * (l.y + h.y) < kStageOriginY * 0.5f)) { moved = false; continue; }
                    grow(l, h);
                    fromJoints = true;
                }
            }
        if (!fromJoints && moved)
            if (auto* ap = world.GetPool<WorldAabb>())
            {
                auto& ents = ap->Entities();
                auto& data = ap->Data();
                for (std::size_t i = 0; i < ents.size(); ++i)
                {
                    if (!m_stageSet.count(ents[i])) continue;
                    const WorldAabb& wb = data[i];
                    const float cy = 0.5f * (wb.min.y + wb.max.y);
                    if (!(cy < kStageOriginY * 0.5f)) { moved = false; continue; }
                    grow(wb.min, wb.max);
                }
            }
        if (any && moved)
        {
            const float ex = mx.x - mn.x, ey = mx.y - mn.y, ez = mx.z - mn.z;
            // Framed by height, not by the box diagonal: a chain preview looks
            // at a standing character, and the diagonal of a T-pose's joint box
            // (arm span, a tail, a held weapon) would leave the subject at half
            // the frame height. The horizontal extent still bounds it from below
            // so a wide subject is not cropped at the sides of a 16:9 view.
            // Joints sit inside the skin (the head joint is below the crown,
            // the ankle above the sole), hence the margin.
            const float r = std::max(0.5f * ey, 0.35f * std::max(ex, ez)) *
                            (fromJoints ? 1.12f : 1.f);
            if (std::isfinite(r) && r > 0.05f && r < 100.f)
            {
                m_focus[0] = 0.5f * (mn.x + mx.x);
                m_focus[1] = 0.5f * (mn.y + mx.y);
                m_focus[2] = 0.5f * (mn.z + mx.z);
                m_radius   = r;
                // The horizontal extent, for the aspect-aware distance below
                // (m_radius itself stays the 16:9 rule - the logged framing and
                // the auto sizes use it).
                m_frameWide = std::max(ex, ez) * (fromJoints ? 1.12f : 1.f);
                m_focusValid = true;
                // The driver's auto sizes (floor, span, teleport) scale with the
                // framed height; the camera follows the drive offset relative to
                // where it was when framed.
                m_subjectHeight = ey;
                for (int k = 0; k < 3; ++k) m_focusDriveBase[k] = m_driveOffset[k];
                LOG_INFO("[CHAINPREVIEW] framing: focus=(%.2f,%.2f,%.2f) radius=%.2f "
                         "source=%s", m_focus[0], m_focus[1], m_focus[2], m_radius,
                         fromJoints ? "bindPoseJoints" : "worldAabb");
            }
        }
    }

    // ---- the orbit: Preview::StudioCameraBasis, scaled to the subject ----
    // The studio rig frames a unit-diameter subject; distanceScale is the
    // subject's radius over 0.5 times the user's zoom. The yaw adds the turntable
    // angle (advanced on the stage clock by TickDriver), panOffset is the user's
    // pan, and the framed point follows the root-motion driver's translation so a
    // driven subject stays in frame while the floor slides under it.
    float eye[3], fwd[3];
    // The render follows the view box's aspect, so a portrait (stacked, narrow)
    // view must not crop a T-pose at the sides: the framed radius grows to
    // 0.5 x width / aspect when that is the larger. At 16:9 (every pinned pack)
    // 0.5 / 1.78 < 0.35, i.e. unchanged.
    float frameR = m_radius;
    if (m_renderW != 0 && m_renderH != 0 && m_frameWide > 0.f)
    {
        const float aspect = static_cast<float>(m_renderW) / static_cast<float>(m_renderH);
        frameR = std::max(frameR, 0.5f * m_frameWide / std::max(aspect, 0.1f));
    }
    m_camRadius = frameR;   // the pan's world-units-per-pixel reads the same distance
    Preview::StudioCameraBasis(m_yaw + m_turntableYaw, m_pitch, (frameR / 0.5f) * m_dist,
                               eye, fwd, m_pan);
    const float follow[3] = { m_driveOffset[0] - m_focusDriveBase[0],
                              m_driveOffset[1] - m_focusDriveBase[1],
                              m_driveOffset[2] - m_focusDriveBase[2] };
    const XMVECTOR f   = XMVector3Normalize(XMVectorSet(fwd[0], fwd[1], fwd[2], 0.f));
    const XMVECTOR upW = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    const XMVECTOR r   = XMVector3Normalize(XMVector3Cross(upW, f));   // LH: up x fwd
    const XMVECTOR u   = XMVector3Cross(f, r);
    const XMVECTOR pos = XMVectorSet(m_focus[0] + follow[0] + eye[0],
                                     m_focus[1] + follow[1] + eye[1],
                                     m_focus[2] + follow[2] + eye[2], 1.f);
    XMMATRIX m;
    m.r[0] = r; m.r[1] = u; m.r[2] = f; m.r[3] = pos;
    // ExtractPreviewCameras reads position = row 3, forward = row 2, up = row 1.
    if (GlobalTransform* gt = world.GetComponent<GlobalTransform>(m_camEnt))
        XMStoreFloat4x4(&gt->matrix, m);
    // The overlay projects with exactly what this camera was just given (the
    // panel is its author), lens included.
    XMStoreFloat3(&m_overlayCam.eye, pos);
    XMStoreFloat3(&m_overlayCam.forward, f);
    XMStoreFloat3(&m_overlayCam.up, u);
    if (const VirtualCameraComponent* vc = world.GetComponent<VirtualCameraComponent>(m_camEnt))
    {
        m_overlayCam.fovY  = vc->fov;
        m_overlayCam.nearZ = vc->nearZ;
        m_overlayCam.farZ  = vc->farZ;
    }
    m_overlayCam.valid = true;
    if (LocalTransform* lt = world.GetComponent<LocalTransform>(m_camEnt))
    {
        XMStoreFloat3(&lt->translation, pos);
        XMStoreFloat4(&lt->rotation, XMQuaternionNormalize(XMQuaternionRotationMatrix(m)));
        lt->scale = { 1.f, 1.f, 1.f };
    }
}

// ==== Teardown ====
// The one path every close takes (Close button, collapse, auto-release, kill
// switch, Play, harness close).
void ChainPhysicsPreviewWindow::DestroyStage(Renderer* renderer, World& world)
{
    // The A/B shadow goes with the stage it is fed from (it owns no GPU buffer -
    // a bare ChainPhysicsComponent entity, never one of the stage handles).
    // m_abOn is a setting: the next stage of the session gets a new B.
    DestroyAbStage(renderer, &world, "stage destroyed");
    // The solver's solo entry dies with the carrier (OnEntityDestroyed); the
    // next stage - whose carrier may even reuse the id - is handed it afresh.
    m_soloAppliedTo = NullEntity;

    // Editor/PreviewStage: only still-valid handles are ours; their skinned
    // buffers are released (safe: the stage is never in a Play snapshot -
    // TearDownForPlay runs before CloneState - and never undo-parked), then
    // DestroySubtree of each whose parent is not in the stage.
    const std::uint32_t destroyed = DestroyStageEntities(world, renderer, m_stageHandles, !m_harnessReleaseOff);
    LOG_INFO("[CHAINPREVIEW] stage destroyed: %u entit%s (root %u)", destroyed,
             destroyed == 1 ? "y" : "ies", static_cast<unsigned>(m_stageRoot));
    m_stageEnts.clear();
    m_stageHandles.clear();
    m_stageSet.clear();
    // The ids are free now and will be recycled: a stale pin would keep
    // animating whatever claims them next. (The alias went with the stage's
    // AnimationSetComponent; AnimationClipSystem drops the binding on its own
    // stale-handle sweep.)
    m_stagePin.clear();
    m_previewSet.clear();   // same reason as the pin
    m_animEnt   = NullEntity;
    m_clipAlias.clear();
    m_clipBound = false;
    m_clipApplyPending = !m_clipPath.empty();
    m_authoredClips.clear();
    m_stageRoot = m_stageChain = NullEntity;
    m_stageRootH = m_stageChainH = NullEntityHandle;
    m_stagePose.Reset(NullEntity);
    // The snapshot names the old skeleton's bones, and a gesture cannot outlive
    // its stage (the auto-pause resumes).
    m_poseSnap            = PoseSnap{};
    ReleasePoseGesture();
    m_poseGestureHarness  = false;
    m_poseGizmoBone       = ~0u;
    // The floor goes with the stage (through its handle).
    DestroyFloor(world);
    // m_staged stays valid: a teardown that captured it (CaptureStaged) hands it
    // to the next spawn of the subject.
    m_noChain = false;
    m_focusValid = false;
    // The next stage's first picture waits for its framing again.
    m_requesting = false;
}

// The Play-boundary save. Called from TearDown while the stage is still
// alive: the live component (which equals m_staged after the one writer's
// sync, unless a harness probe wrote it) is copied into m_staged field by
// field and flagged for the next spawn of the same subject to re-apply.
void ChainPhysicsPreviewWindow::CaptureStaged(World& world, const char* why)
{
    if (m_stageChain == NullEntity || !world.IsHandleValid(m_stageChainH)) return;
    const ChainPhysicsComponent* live = world.GetComponent<ChainPhysicsComponent>(m_stageChain);
    if (!live) return;
    live->CopyAuthoredInto(m_staged);
    m_stagedValid   = true;
    m_stagedCarry   = true;
    m_stagedSubject = m_subjectPath;
    const std::string now = Editor::Undo::SerializeComponentBlob(
        GetComponentRegistry(), world, m_stageChain,
        std::type_index(typeid(ChainPhysicsComponent)));
    m_stagedDirty = !m_loadedBlob.empty() && now != m_loadedBlob;
    LOG_INFO("[CHAINPREVIEW] staged captured: dirty=%d damping=%.4f groups=%u "
             "restPoseBones=%u why=%s", m_stagedDirty ? 1 : 0, m_staged.damping,
             static_cast<unsigned>(m_staged.groups.size()),
             static_cast<unsigned>(m_staged.restPose.size()), why ? why : "");
}

void ChainPhysicsPreviewWindow::ApplyHarnessParams(const ChainPreviewContext& ctx)
{
    (void)ctx;
    if (m_harnessParams.empty()) return;
    if (m_stageRoot == NullEntity || m_stageChain == NullEntity) return;
    // A harness edit is a tree edit: it lands in m_staged (never the live
    // component), becomes one panel-undo step and reaches the live component
    // through the one writer (TickStagedParams -> SyncStagedToLive) in this same
    // serial window. `at` 0 = the first frame with a stage; each entry is applied
    // once per run (a respawn re-applies nothing - the carry does that).
    ChainPhysicsComponent* c = m_stagedValid ? &m_staged : nullptr;
    for (HarnessParam& hp : m_harnessParams)
    {
        if (hp.applied || (hp.at != 0 && m_tick < hp.at)) continue;
        hp.applied = true;
        bool live = false;
        float v[3] = {};
        int gi = -1;
        std::string member;
        char kind = 0;
        ChainPhysicsComponent before;
        if (c) c->CopyAuthoredInto(before);
        if (c && SplitGroupKey(hp.key, gi, member))
        {
            const GroupField* gf = FindGroupField(member);
            if (gf && gi >= 0 && gi < static_cast<int>(c->groups.size()) &&
                ParseParamValue(gf->kind, hp.value, v))
            {
                char* dst = reinterpret_cast<char*>(&c->groups[static_cast<std::size_t>(gi)]) + gf->off;
                if (gf->kind == 's')
                    std::snprintf(dst, gf->cap, "%s", hp.value.c_str());   // char array
                else
                    StoreParam(gf->kind, dst, v);
                live = true;
            }
        }
        else if (c && RestPoseKey(hp.key, kind))
        {
            if (ParseParamValue(kind, hp.value, v))
            {
                if (kind == 'f') c->restPoseWeight  = std::clamp(v[0], 0.f, 1.f);
                else             c->restPoseEnabled = v[0] != 0.f;
                live = true;
            }
        }
        else if (c)
        {
            if (const Reflect::FieldDescriptor* f = FindTopField(hp.key, kind))
                if (ParseParamValue(kind, hp.value, v))
                {
                    StoreParam(kind, reinterpret_cast<char*>(c) + f->offset, v);
                    live = true;
                }
        }
        ENGINE_CHECK(live, "-chainpreview_param %s=%s could not be applied to the "
                     "stage (%s); the field keeps its authored value.",
                     hp.key.c_str(), hp.value.c_str(),
                     c ? "group index past the stage's groups" : "the stage has no "
                         "ChainPhysicsComponent");
        // rebuild = the solver's own structural rule (NeedsReinit), not the key
        // table: what the sync will actually do with this edit.
        const bool rebuild = live && ChainPhysicsSystem::NeedsReinit(before, *c);
        LOG_INFO("[CHAINPREVIEW] param apply: key=%s value=%s live=%d rebuild=%d at=%llu "
                 "frame=%llu", hp.key.c_str(), hp.value.c_str(), live ? 1 : 0,
                 rebuild ? 1 : 0, static_cast<unsigned long long>(hp.at),
                 static_cast<unsigned long long>(m_tick));
    }
}

// The -chainpreview_param grammar as two reusable pieces: the A/B's
// -chainpreview_abparam edits B's staged copy with exactly the keys (and the
// same field tables) the harness uses for A.
bool ChainPhysicsPreviewWindow::ParamKeyValid(const std::string& key, const std::string& value)
{
    float v[3] = {};
    char kind = 0;
    int gi = -1;
    std::string member;
    if (SplitGroupKey(key, gi, member))
    {
        const GroupField* gf = FindGroupField(member);
        return gf && ParseParamValue(gf->kind, value, v);
    }
    if (RestPoseKey(key, kind)) return ParseParamValue(kind, value, v);
    return FindTopField(key, kind) && ParseParamValue(kind, value, v);
}

bool ChainPhysicsPreviewWindow::ApplyParamKey(ChainPhysicsComponent& c, const std::string& key,
                                              const std::string& value)
{
    float v[3] = {};
    int gi = -1;
    std::string member;
    char kind = 0;
    if (SplitGroupKey(key, gi, member))
    {
        const GroupField* gf = FindGroupField(member);
        if (!gf || gi < 0 || gi >= static_cast<int>(c.groups.size()) ||
            !ParseParamValue(gf->kind, value, v))
            return false;
        char* dst = reinterpret_cast<char*>(&c.groups[static_cast<std::size_t>(gi)]) + gf->off;
        if (gf->kind == 's') std::snprintf(dst, gf->cap, "%s", value.c_str());
        else                 StoreParam(gf->kind, dst, v);
        return true;
    }
    if (RestPoseKey(key, kind))
    {
        if (!ParseParamValue(kind, value, v)) return false;
        if (kind == 'f') c.restPoseWeight  = std::clamp(v[0], 0.f, 1.f);
        else             c.restPoseEnabled = v[0] != 0.f;
        return true;
    }
    const Reflect::FieldDescriptor* f = FindTopField(key, kind);
    if (!f || !ParseParamValue(kind, value, v)) return false;
    StoreParam(kind, reinterpret_cast<char*>(&c) + f->offset, v);
    return true;
}

// ==== The one writer and the panel-undo ring ====
void ChainPhysicsPreviewWindow::ResetStagedTracking(bool clearHistory)
{
    if (!m_stagedValid) return;
    m_staged.CopyAuthoredInto(m_stagedSynced);
    m_staged.CopyAuthoredInto(m_undoBase);
    m_undoBaseValid    = true;
    m_stagedStructural = false;
    if (clearHistory)
    {
        m_undo.clear();
        m_redo.clear();
    }
}

void ChainPhysicsPreviewWindow::SyncStagedToLive(const ChainPreviewContext& ctx, const char* why)
{
    if (m_stageChain == NullEntity || !m_stagedValid || !ctx.world) return;
    World& world = *ctx.world;
    if (!world.IsHandleValid(m_stageChainH)) return;
    ChainPhysicsComponent* live = world.GetComponent<ChainPhysicsComponent>(m_stageChain);
    if (!live) return;
#if defined(WITH_HARNESS) && WITH_HARNESS
    // Reverse arm (-chainpreview_sync_wholesale): a per-frame whole-struct copy.
    // It clobbers BoneRestPose's runtime name cache every frame, which the
    // sync-probe read-out (TickSyncProbe's boneNameResolves) and
    // chain_preview_apply exist to catch.
    if (m_harnessSyncWholesale)
    {
        *live = m_staged;
        m_staged.CopyAuthoredInto(m_stagedSynced);
        m_stagedStructural = false;
        return;
    }
#endif
    // Only when the staged value MOVED since the last sync - never every frame,
    // and never by comparing against the live component (a harness probe that
    // wrote the live one directly is not silently reverted).
    if (m_staged.AuthoredEquals(m_stagedSynced)) { m_stagedStructural = false; return; }
    const bool structural = m_stagedStructural ||
                            ChainPhysicsSystem::NeedsReinit(m_stagedSynced, m_staged);
    // Field by field: `*live = m_staged` would wipe BoneRestPose's runtime cache
    // (re-resolve every name every frame, a LOG_WARNING per missing bone per
    // frame, appliedThisFrame lost between ApplyRestPose and resetRot).
    m_staged.CopyAuthoredInto(*live);
    m_staged.CopyAuthoredInto(m_stagedSynced);
    m_stagedStructural = false;
    ++m_syncs;
    if (!structural) return;
    if (m_harnessReinitOff)
    {
        LOG_WARNING("[CHAINPREVIEW] structural change synced WITHOUT re-initialising the "
                    "chains (-chainpreview_reinit_off, why=%s frame=%llu)", why,
                    static_cast<unsigned long long>(m_tick));
        return;
    }
    // A structural change re-initialises the chains: InitEntity must rebuild
    // the topology from the new groups / rest pose. Serial window, before this
    // frame's chain Update - which re-inits. ResetPreviewEntity, not a bare
    // Invalidate: on a no-clip rig the LocalPose scratch still holds the last
    // simulated rotations and InitEntity (before resetRot) would bake the new
    // rest data from that swing (a lasting ~0.01-unit particle offset vs a clean
    // reset). While A/B is on, B restarts with A: its fed input is A's pose, and
    // a half reset is a divergence nobody's parameter caused.
    if (ChainPhysicsSystem* cps = ctx.renderer->GetChainPhysicsSystem())
    {
        if (m_harnessReinitBare)
            cps->Invalidate(m_stageChain);   // reverse arm: a bare Invalidate
        else
        {
            cps->ResetPreviewEntity(m_stageChain);
            if (m_abEnt != NullEntity && world.IsHandleValid(m_abEntH))
                cps->ResetPreviewEntity(m_abEnt);
        }
    }
    ++m_rebuilds;
    ++m_resetSerial;              // the hold check must not compare across it
    m_laneHasState = false;
    m_lastRebuildWhy = why;
    m_toastUntil = ImGui::GetTime() + 2.5;
    LOG_INFO("[CHAINPREVIEW] chains re-initialised: why=%s rebuilds=%llu syncs=%llu "
             "groups=%u restPoseBones=%u frame=%llu", why,
             static_cast<unsigned long long>(m_rebuilds),
             static_cast<unsigned long long>(m_syncs),
             static_cast<unsigned>(m_staged.groups.size()),
             static_cast<unsigned>(m_staged.restPose.size()),
             static_cast<unsigned long long>(m_tick));
}

void ChainPhysicsPreviewWindow::TickStagedParams(const ChainPreviewContext& ctx)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -chainpreview_undo_at / -chainpreview_redo_at: the panel's Ctrl+Z / Ctrl+Y.
    while (m_harnessUndoNext < m_harnessUndoAt.size() &&
           m_tick >= m_harnessUndoAt[m_harnessUndoNext] && m_stageRoot != NullEntity)
    {
        ++m_harnessUndoNext;
        ++m_pendingUndo;
        LOG_INFO("[CHAINPREVIEW] -chainpreview_undo_at: Ctrl+Z at editor frame %llu",
                 static_cast<unsigned long long>(m_tick));
    }
    while (m_harnessRedoNext < m_harnessRedoAt.size() &&
           m_tick >= m_harnessRedoAt[m_harnessRedoNext] && m_stageRoot != NullEntity)
    {
        ++m_harnessRedoNext;
        ++m_pendingRedo;
        LOG_INFO("[CHAINPREVIEW] -chainpreview_redo_at: Ctrl+Y at editor frame %llu",
                 static_cast<unsigned long long>(m_tick));
    }
#endif
    if (m_stageRoot == NullEntity || !m_stagedValid)
    {
        m_pendingUndo = m_pendingRedo = 0;
        return;
    }

    // ---- 1. commit: a FINISHED edit becomes one undo step ----
    // While a slider is held (any ImGui item active) the value streams to the
    // live component every frame (the sync below) but is not recorded - a drag
    // is ONE step, whose "before" is the value it started from.
    if (!m_undoBaseValid)
    {
        m_staged.CopyAuthoredInto(m_undoBase);
        m_undoBaseValid = true;
    }
    // ImGuizmo never sets an ImGui ActiveId, so a ring drag is invisible to
    // uiEditActive - its gesture flag holds the commit.
    else if (!ctx.uiEditActive && !PoseGestureHeld() && !m_staged.AuthoredEquals(m_undoBase))
    {
        m_undo.push_back({ m_undoBase });
        if (m_undo.size() > kUndoDepth) m_undo.pop_front();
        m_redo.clear();
        m_staged.CopyAuthoredInto(m_undoBase);
        ++m_commits;
    }

    // ---- 2. Ctrl+Z / Ctrl+Y (the panel's history, never the global one) ----
    auto logOp = [&](const char* op, std::uint64_t n, bool applied, bool structural)
    {
        const ChainPhysicsComponent* live = ctx.world
            ? ctx.world->GetComponent<ChainPhysicsComponent>(m_stageChain) : nullptr;
        LOG_INFO("[CHAINPREVIEW] %s %llu: applied=%d depth=%u redo=%u structural=%d "
                 "rebuilds=%llu damping=%.4f liveDamping=%.4f gravity=%.4f "
                 "liveGravity=%.4f g0enabled=%d groups=%u frame=%llu", op,
                 static_cast<unsigned long long>(n), applied ? 1 : 0,
                 static_cast<unsigned>(m_undo.size()), static_cast<unsigned>(m_redo.size()),
                 structural ? 1 : 0, static_cast<unsigned long long>(m_rebuilds),
                 m_staged.damping, live ? live->damping : -1.f, m_staged.gravity,
                 live ? live->gravity : 0.f,
                 m_staged.groups.empty() ? -1 : (m_staged.groups[0].enabled ? 1 : 0),
                 static_cast<unsigned>(m_staged.groups.size()),
                 static_cast<unsigned long long>(m_tick));
    };
    auto step = [&](bool undo)
    {
        bool applied = false, structural = false;
        ChainPhysicsComponent before;
        m_staged.CopyAuthoredInto(before);
        if (undo && !m_undo.empty())
        {
            m_redo.push_back({ m_staged });
            m_undo.back().value.CopyAuthoredInto(m_staged);
            m_undo.pop_back();
            applied = true;
        }
        else if (!undo && !m_redo.empty())
        {
            m_undo.push_back({ m_staged });
            if (m_undo.size() > kUndoDepth) m_undo.pop_front();
            m_redo.back().value.CopyAuthoredInto(m_staged);
            m_redo.pop_back();
            applied = true;
        }
        if (applied)
        {
            m_staged.CopyAuthoredInto(m_undoBase);
            structural = ChainPhysicsSystem::NeedsReinit(before, m_staged);
        }
        const std::uint64_t n = undo ? ++m_undoOps : ++m_redoOps;
        // The same sync as an edit: a structural undo REPLAYS Invalidate there.
        SyncStagedToLive(ctx, undo ? "undo" : "redo");
        logOp(undo ? "undo" : "redo", n, applied, structural);
    };
    for (; m_pendingUndo > 0; --m_pendingUndo) step(true);
    for (; m_pendingRedo > 0; --m_pendingRedo) step(false);

    // ---- 3. the per-frame sync (a no-op unless m_staged moved) ----
    // Held while a ring drag is open: a rest-pose rotation is structural, and
    // syncing every drag frame would re-initialise the chains every drag frame.
    // The release frame syncs once (one re-init).
    if (!PoseGestureHeld()) SyncStagedToLive(ctx, "edit");
}

void ChainPhysicsPreviewWindow::LogStagedSummary(World* world, const char* when) const
{
    const ChainPhysicsComponent* live =
        (world && m_stageChain != NullEntity && world->IsHandleValid(m_stageChainH))
            ? world->GetComponent<ChainPhysicsComponent>(m_stageChain) : nullptr;
    // No World (shutdown): the value the one writer last synced IS the live one.
    const ChainPhysicsComponent& l = live ? *live : m_stagedSynced;
    LOG_INFO("[CHAINPREVIEW] staged: valid=%d commits=%llu undoDepth=%u redoDepth=%u "
             "undos=%llu redos=%llu syncs=%llu rebuilds=%llu damping=%.4f "
             "liveDamping=%.4f gravity=%.4f liveGravity=%.4f g0enabled=%d "
             "liveG0enabled=%d groups=%u dirty=%d applies=%llu applyRefusals=%llu "
             "reverts=%llu presetLoads=%llu when=%s",
             m_stagedValid ? 1 : 0, static_cast<unsigned long long>(m_commits),
             static_cast<unsigned>(m_undo.size()), static_cast<unsigned>(m_redo.size()),
             static_cast<unsigned long long>(m_undoOps),
             static_cast<unsigned long long>(m_redoOps),
             static_cast<unsigned long long>(m_syncs),
             static_cast<unsigned long long>(m_rebuilds),
             m_staged.damping, l.damping, m_staged.gravity, l.gravity,
             m_staged.groups.empty() ? -1 : (m_staged.groups[0].enabled ? 1 : 0),
             l.groups.empty() ? -1 : (l.groups[0].enabled ? 1 : 0),
             static_cast<unsigned>(m_staged.groups.size()), IsStagedDirty() ? 1 : 0,
             static_cast<unsigned long long>(m_applies),
             static_cast<unsigned long long>(m_applyRefusals),
             static_cast<unsigned long long>(m_reverts),
             static_cast<unsigned long long>(m_presetLoads), when ? when : "");
}

void ChainPhysicsPreviewWindow::ValidateStageHandles(const ChainPreviewContext& ctx)
{
    if (m_stageRoot == NullEntity || !ctx.world) return;
    World& world = *ctx.world;
    // O(stage) - ~20 handle compares a frame.
    bool anyDead = false;
    for (const EntityHandle& h : m_stageHandles)
        if (!world.IsHandleValid(h)) { anyDead = true; break; }
    if (!anyDead) return;
    const bool rootDead = !world.IsHandleValid(m_stageRootH);
    std::uint32_t dead = 0;
    std::vector<Entity>       keptEnts;
    std::vector<EntityHandle> keptHandles;
    for (std::size_t i = 0; i < m_stageHandles.size(); ++i)
    {
        if (world.IsHandleValid(m_stageHandles[i]))
        {
            keptEnts.push_back(m_stageEnts[i]);
            keptHandles.push_back(m_stageHandles[i]);
            continue;
        }
        // The id may already be somebody else's: out of every set NOW, before
        // the lane / the pin can read it this frame.
        const Entity e = m_stageEnts[i];
        m_stageSet.erase(e);
        m_previewSet.erase(e);
        m_stagePin.erase(e);
        ++dead;
    }
    m_stageEnts.swap(keptEnts);
    m_stageHandles.swap(keptHandles);
    LOG_WARNING("[CHAINPREVIEW] %u stage entit%s destroyed outside the panel%s", dead,
                dead == 1 ? "y was" : "ies were",
                rootDead ? " - including the root; tearing the stage down "
                           "(only still-valid handles are destroyed)" : "");
    if (!rootDead && world.IsHandleValid(m_stageChainH)) return;
    // The root (or the chain carrier) is gone: the stage is not a character
    // any more. The one teardown path, which destroys only handle-valid ids.
    // The subject is dropped rather than respawned in a loop against whatever
    // destroyed it.
    TearDown(ctx.renderer, ctx.world, "stage destroyed outside", /*destroyEntities=*/true);
    m_subjectPath.clear();
    m_status = "The preview stage was deleted outside the panel - pick the "
               "subject again.";
}

void ChainPhysicsPreviewWindow::RestoreGlobals(Renderer* renderer, const char* reason)
{
    if (!m_globalsOverridden) return;
    m_globalsOverridden = false;
    bool lodNow = m_savedLodEnabled;
    bool pinCleared = !m_pinPushed;
    if (renderer)
    {
        // Hand back the pin. Only this panel's owner entry goes - the
        // Skeleton Editor's stage stays pinned.
        if (m_pinPushed) renderer->RemoveAnimPinOwner(this);
        pinCleared = !renderer->HasAnimPinOwner(this);
        if (ChainPhysicsSystem* cps = renderer->GetChainPhysicsSystem())
        {
            cps->lodEnabled = m_savedLodEnabled;
            lodNow = cps->lodEnabled;
            // Hand the preview lane back (only if it is still ours - a
            // world clear may already have dropped it). From here the main
            // lane excludes nothing and no second pass runs.
            if (m_lanePushed && cps->GetPreviewLaneSet() == &m_previewSet)
            {
                renderer->SetChainPhysicsPreviewLane(nullptr, 0.f);
                cps->SetPreviewLaneLogEvery(0);
            }
        }
    }
    m_pinPushed  = false;
    m_lanePushed = false;
    m_lastSimDt  = 0.f;
    m_lastLaneSteps = 0;
    m_lastPush = m_prevPush = PushRecord{};
    m_prevPoseValid = false;
    m_stagePin.clear();
    m_previewSet.clear();
    // The restore line prints animCulling (packs assert it): the value the panel
    // leaves alone.
    LOG_INFO("[CHAINPREVIEW] restore: animCulling=%d lodEnabled=%d reason=%s "
             "pinCleared=%d",
             renderer ? (renderer->IsAnimCullingEnabled() ? 1 : 0) : 1,
             lodNow ? 1 : 0, reason, pinCleared ? 1 : 0);
}

void ChainPhysicsPreviewWindow::TearDown(Renderer* renderer, World* world,
                                         const char* reason, bool destroyEntities)
{
    const bool hadStage = m_stageRoot != NullEntity;
    // The read-out's closing line (the last "anim:" line in the log is the one a
    // pack reads), then the panel's own clip reference. The clip selection
    // survives - a stage rebuilt after Play / a reload plays it again - only the
    // handle is given back.
    if (!m_clipPath.empty() && hadStage) LogAnimSummary(reason);
    // The preview lane's closing summary (the last "[CHAINPHYS] preview lane:"
    // line is the one a pack reads) and the -chainpreview_dump, before the stage
    // is destroyed - both read it.
    if (renderer && m_lanePushed)
        if (ChainPhysicsSystem* cps = renderer->GetChainPhysicsSystem())
            if (cps->GetPreviewLaneSet() == &m_previewSet) cps->LogPreviewLaneStats(reason);
    // The stage clock's closing line (the last "[CHAINPREVIEW] clock:" line is
    // the session's), and a harness A/B that never finished says so - its packs
    // read the compare line.
    if (hadStage) LogClock(reason);
    if (hadStage && (m_abPhase != AbPhase::Off && m_abPhase != AbPhase::Done))
        LOG_WARNING("[CHAINPREVIEW] ab: -chainpreview_ab=%.3f did not finish before "
                    "the stage went (%s) - phase %d, window %c of %u, %llu steps into it",
                    m_abSeconds, reason, static_cast<int>(m_abPhase),
                    static_cast<char>('a' + m_abIndex), static_cast<unsigned>(m_abWin.size()),
                    static_cast<unsigned long long>(
                        m_abIndex < m_abWin.size() ? m_abWin[m_abIndex].steps : 0u));
    if (renderer && world && hadStage) WriteDump(*renderer, *world, reason);
    // Capture the live parameters before the stage goes, on every teardown that
    // has a World (Play, close, collapse, not drawn, ...).
    if (world && hadStage) CaptureStaged(*world, reason);
    // The closing overlay / staged-parameter lines (the last one in a log is the
    // session's). World-less shutdown: nothing to read.
    if (hadStage)
    {
        LogOverlay(reason);
        LogDisplay(renderer, world, reason, "display:");
        LogStagedSummary(world, reason);
        LogSyncProbe(reason);   // the session's sync-probe read-out
        LogStats(reason);       // the session's closing stats line
        LogScene(reason);   // the session's closing scene / driver lines
        LogDrive(reason);
        // The session's closing A/B and solo lines (only when the session used them
        // - the last such line in a log is the one a pack reads).
        if (m_abCreates != 0) LogAb(reason);
        if (m_soloSets != 0)  LogSolo(reason);
    }
    m_overlaySnap = ChainOverlay::Snapshot{};
    ReleaseClipHandle(m_clipHandleOwner);
    if (destroyEntities && world)
    {
        if (m_stageRoot != NullEntity) DestroyStage(renderer, *world);
        // Through the handle: a recycled id is not our camera.
        if (m_camEnt != NullEntity && world->IsHandleValid(m_camEntH))
            Hierarchy::DestroySubtree(*world, m_camEnt);
    }
    m_camEnt = NullEntity;
    m_camEntH = NullEntityHandle;
    if (m_viewId != ~0u && renderer)
    {
        renderer->SetPreviewCameraEntity(m_viewId, NullEntity);
        renderer->ReleasePreviewView(m_viewId);
        LOG_INFO("[CHAINPREVIEW] released view %u (%s)", m_viewId, reason);
    }
    m_viewId = ~0u;
    m_laneAskedW = m_laneAskedH = 0;
    m_renderW = m_renderH = 0;
    m_sharingLane = false;
    m_requesting  = false;
    RestoreGlobals(renderer, reason);
    if (hadStage && !m_harnessSubject.empty() && m_censusPre.valid &&
        m_censusPostAt == 0 && !m_isolationLogged)
        m_censusPostAt = m_tick + kCensusPostDelay;
}

void ChainPhysicsPreviewWindow::TearDownForPlay(Renderer* renderer, World* world)
{
    const bool active = m_stageRoot != NullEntity || m_camEnt != NullEntity ||
                        m_viewId != ~0u || m_globalsOverridden;
    if (!active) return;
    if (m_harnessPlayTeardownOff)
    {
        LOG_WARNING("[CHAINPREVIEW] enter play: teardown SKIPPED "
                    "(-chainpreview_play_teardown_off) - the Play snapshot will "
                    "contain the stage");
        return;
    }
    // Before CloneState. CloneState has no opt-out, so a stage alive here would
    // be baked into the Play snapshot and AdoptState would bring it back at Stop
    // owned by nothing.
    TearDown(renderer, world, "enter play", /*destroyEntities=*/true);
    m_suspendedForPlay = true;
}

void ChainPhysicsPreviewWindow::ReleaseViewsAtShutdown(Renderer* renderer, World* world)
{
    // The World is still whole here (EditorLayer's ReleasePreviewViews runs
    // before the harness fixture cleanup); the dump reads it. TearDown below is
    // called without it, so it destroys nothing.
    if (renderer && world && m_stageRoot != NullEntity)
        WriteDump(*renderer, *world, "shutdown");
#if defined(WITH_HARNESS) && WITH_HARNESS
    // [PANELVIS] chainpreview: emitted whenever the flag is on, healthy case
    // included - a number that only appears when something is wrong is a
    // number nobody has seen the good value of.
    if (HarnessVerifyOn())
        LOG_INFO("[PANELVIS] chainpreview asked=%u drew=%llu hidden=%llu "
                 "beginRefused=%llu autoReleases=%u spawns=%u",
                 m_harnessSubject.empty() ? 0u : 1u,
                 static_cast<unsigned long long>(m_drewFrames),
                 static_cast<unsigned long long>(m_hiddenFrames),
                 static_cast<unsigned long long>(m_beginRefusedFrames),
                 m_autoReleases, m_spawnCount);
#endif
    // Entities are left to the World's own teardown (see the header).
    TearDown(renderer, nullptr, "shutdown", /*destroyEntities=*/false);
    m_abEnt  = NullEntity;         // left to the World's teardown too
    m_abEntH = NullEntityHandle;
    m_clipAlias.clear();
    m_animEnt = NullEntity;
    m_stageRoot = m_stageChain = NullEntity;
    m_stageRootH = m_stageChainH = NullEntityHandle;
    m_stageHandles.clear();
    m_stageEnts.clear();
    m_stageSet.clear();
    m_visibleThisFrame = false;
}

// ==== The Preview Controller, the clip picker and the transport (UI half) ====
// Draw only edits the panel's own state and posts requests;
// ProcessPendingActions applies them.
void ChainPhysicsPreviewWindow::DrawControllerUi()
{
    // Every row below flows within the column.
    namespace Flow = EditorUI::Flow;
    int mode = static_cast<int>(m_pendingControllerSet ? m_pendingController : m_controller);
    ImGui::TextUnformatted("Controller");
    Flow::SameLineIfFits(Flow::CheckboxW("Rest pose"));
    if (ImGui::RadioButton("Rest pose", mode == static_cast<int>(Controller::RestPose)))
    { m_pendingController = Controller::RestPose; m_pendingControllerSet = true; }
    Flow::SameLineIfFits(Flow::CheckboxW("Clip"));
    if (ImGui::RadioButton("Clip", mode == static_cast<int>(Controller::Clip)))
    { m_pendingController = Controller::Clip; m_pendingControllerSet = true; }
    Flow::SameLineIfFits(Flow::CheckboxW("Follow entity"));
    // The third controller state. Picking it needs a source - the editor
    // selection - so the radio is the pick.
    ImGui::BeginDisabled(m_uiSelection == NullEntity &&
                         mode != static_cast<int>(Controller::FollowSceneEntity));
    if (ImGui::RadioButton("Follow entity", mode == static_cast<int>(Controller::FollowSceneEntity)) &&
        m_uiSelection != NullEntity)
    {
        m_pendingFollowSel = true;
        m_pendingFollowEnt = m_uiSelection;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Play the clip the SELECTED scene entity plays, at its time,\n"
                          "every frame. The source is NOT pinned: if it is anim-culled the\n"
                          "stage mirrors its last known state. The source keeps its OWN\n"
                          "chain simulation, so the two chains differ - that is the point\n"
                          "(compare). A destroyed source / a world reload falls back to Clip.");
    if (mode == static_cast<int>(Controller::FollowSceneEntity))
    {
        Flow::ColoredWrapped(m_followCulled ? ImVec4(1.f, 0.8f, 0.3f, 1.f)
                                            : ImVec4(0.6f, 0.9f, 0.6f, 1.f),
                             "%s", m_followStatus.empty() ? "following..." : m_followStatus.c_str());
        ImGui::BeginDisabled(m_uiSelection == NullEntity);
        if (ImGui::SmallButton("Follow selection instead"))
        {
            m_pendingFollowSel = true;
            m_pendingFollowEnt = m_uiSelection;
        }
        ImGui::EndDisabled();
        return;
    }

    if (mode != static_cast<int>(Controller::Clip)) return;

    // The .ianim walk (~3000 files, ~230 ms) is paid only once the picker is on
    // screen; ProcessPendingActions runs it.
    if (!m_clipScanDone) m_pendingRescan = true;

    // ---- the picker: the subject's authored aliases first, then every .ianim
    // Leave exactly the Rescan button's width + the item spacing.
    ImGui::SetNextItemWidth(-(Flow::ButtonW("Rescan") + ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##cpv_clipfilter", "filter .ianim", m_clipFilter,
                             sizeof(m_clipFilter));
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) m_pendingRescan = true;
    const std::string filter = Lower(m_clipFilter);
    ImGui::BeginChild("##cpv_cliplist", ImVec2(0.f, 150.f), ImGuiChildFlags_Borders);
    // The four stress motions, always at the top. idle / walk-run are existing
    // clips (resolved in PPA, labelled with what they resolved to); yaw sweep +
    // hard stop and teleport hop are deterministic root-motion driver programs,
    // not clips - they run on top of whatever the controller plays.
    {
        const ImVec4 kStress(1.f, 0.72f, 0.45f, 1.f);
        const char* labels[4] = { "Stress: idle", "Stress: walk-run",
                                  "Stress: yaw sweep + hard stop", "Stress: teleport hop" };
        for (int i = 0; i < 4; ++i)
        {
            const bool sel = (i == 2 && m_driveProgram == DriveProgram::YawSweep) ||
                             (i == 3 && m_driveProgram == DriveProgram::TeleportHop) ||
                             (i == 0 && !m_stressIdle.empty() && m_clipPath == m_stressIdle) ||
                             (i == 1 && !m_stressWalkRun.empty() && m_clipPath == m_stressWalkRun);
            ImGui::PushStyleColor(ImGuiCol_Text, kStress);
            if (ImGui::Selectable(labels[i], sel)) m_pendingStress = i;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", i == 0 ? (m_stressIdle.empty() ? "an idle clip (resolved on pick)" : m_stressIdle.c_str())
                                  : i == 1 ? (m_stressWalkRun.empty() ? "a run / walk clip (resolved on pick)" : m_stressWalkRun.c_str())
                                  : i == 2 ? "Root-motion driver program: yaw +270 deg/s for 1 s, HARD\n"
                                             "stop 1.5 s, back -270 deg/s for 1 s, stop 1.5 s (5 s loop)."
                                           : "Root-motion driver program: a teleport of the Teleport\n"
                                             "distance / angle every 2 s, alternating direction.");
        }
        ImGui::Separator();
    }
    for (const AuthoredClip& a : m_authoredClips)
    {
        if (!filter.empty() && Lower(a.name).find(filter) == std::string::npos &&
            Lower(a.path).find(filter) == std::string::npos) continue;
        const bool sel = a.path == m_clipPath && a.source == m_clipSource;
        const std::string label = "[" + a.name + "]  " + Stem(a.path);
        if (ImGui::Selectable(label.c_str(), sel))
        {
            m_pendingClipPath = a.path; m_pendingClipSource = a.source;
            m_pendingClipSet  = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.path.c_str());
    }
    std::vector<int> shown;
    shown.reserve(m_clipList.size());
    for (int i = 0; i < static_cast<int>(m_clipList.size()); ++i)
        if (filter.empty() || Lower(m_clipList[i]).find(filter) != std::string::npos)
            shown.push_back(i);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(shown.size()));
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r)
        {
            const std::string& p = m_clipList[shown[r]];
            ImGui::PushID(shown[r]);
            if (ImGui::Selectable(Stem(p).c_str(), p == m_clipPath))
            {
                m_pendingClipPath = p; m_pendingClipSource.clear();
                m_pendingClipSet  = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.c_str());
            ImGui::PopID();
        }
    if (!m_clipScanDone) ImGui::TextDisabled("scanning...");
    ImGui::EndChild();
    Flow::DisabledWrapped("%u clip file(s) - compatibility is not pre-checked; "
                          "unmatched bones hold their rest pose.",
                          static_cast<unsigned>(m_clipList.size()));

    // ---- sub-clip of a multi-clip file + the channel read-out ----
    if (m_boneClipNames.size() > 1)
    {
        const char* cur = m_clipSource.empty() ? m_boneClipNames[0].c_str()
                                               : m_clipSource.c_str();
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::BeginCombo("##cpv_subclip", cur))
        {
            for (const std::string& n : m_boneClipNames)
                if (ImGui::Selectable(n.c_str(), n == cur))
                {
                    m_pendingClipPath = m_clipPath; m_pendingClipSource = n;
                    m_pendingClipSet  = true;
                }
            ImGui::EndCombo();
        }
    }
    if (!m_clipPath.empty())
    {
        if (!m_clipInfoValid)
            Flow::DisabledWrapped("%s: loading...", Stem(m_clipPath).c_str());
        else
        {
            const ImVec4 col = m_clipMatched == 0 ? ImVec4(1.f, 0.45f, 0.35f, 1.f)
                             : m_clipMatched < m_clipChannels ? ImVec4(1.f, 0.8f, 0.3f, 1.f)
                             : ImVec4(0.6f, 0.9f, 0.6f, 1.f);
            Flow::ColoredWrapped(col, "matched %u / %u channels  (skeleton %u bones)",
                                 m_clipMatched, m_clipChannels, m_clipBones);
            if (!m_morphClipNames.empty())
                ImGui::TextDisabled("%u morph clip(s)",
                                    static_cast<unsigned>(m_morphClipNames.size()));
        }
    }
}

void ChainPhysicsPreviewWindow::DrawTransport()
{
    // FollowSceneEntity shows the read-out and the ruler, but its playhead
    // belongs to the source: the transport buttons are greyed.
    const bool followMode = m_controller == Controller::FollowSceneEntity;
    const bool clipMode = (m_controller == Controller::Clip || followMode) && !m_clipPath.empty();
    const bool known    = clipMode && m_clipInfoValid && m_clipDuration > 0.f;
    const float fps     = m_clipFps > 0.f ? m_clipFps : 30.f;

    // The buttons and the speed slider flow (the view can be 240 px wide);
    // DrawViewArea measures the height this takes.
    namespace Flow = EditorUI::Flow;
    ImGui::BeginDisabled(!known || followMode);
    if (ImGui::SmallButton("|<")) m_pendingJump = -1;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to the first frame");
    Flow::SameLineIfFits(Flow::ButtonW("<"));
    if (ImGui::SmallButton("<")) --m_pendingStep;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step one clip frame back (pauses)");
    Flow::SameLineIfFits(Flow::ButtonW(" || "));
    if (ImGui::SmallButton(m_playing ? " || " : " > ")) m_pendingPlayToggle = true;
    Flow::SameLineIfFits(Flow::ButtonW(">"));
    if (ImGui::SmallButton(">")) ++m_pendingStep;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step one clip frame forward (pauses)");
    Flow::SameLineIfFits(Flow::ButtonW(">|"));
    if (ImGui::SmallButton(">|")) m_pendingJump = +1;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to the last frame");
    Flow::SameLineIfFits(Flow::ButtonW("Loop"));
    // Loop turns red when on (Persona's visual vocabulary).
    if (m_looping) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.2f, 0.2f, 1.f));
    if (ImGui::SmallButton("Loop")) m_looping = !m_looping;   // panel-only state
    if (m_looping) ImGui::PopStyleColor();
    Flow::SameLineIfFits(std::min(110.f, ImGui::GetContentRegionAvail().x));
    ImGui::SetNextItemWidth(std::max(40.f, std::min(110.f, ImGui::GetContentRegionAvail().x)));
    ImGui::SliderFloat("##cpv_speed", &m_speed, -2.f, 2.f, "speed %.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Playback rate; negative plays in REVERSE. Reverse "
                          "playback HOLDS the chain simulation (the solver "
                          "refuses a negative dt): the chains follow the clip.");
    ImGui::EndDisabled();

    // ---- read-out: clip  %  sec / total  frame ----
    if (clipMode)
    {
        const float  dur  = m_clipInfoValid ? m_clipDuration : 0.f;
        const float  t    = static_cast<float>(m_clipT);
        const int    f    = static_cast<int>(std::floor(t * fps + 1e-4f));
        const int    ftot = static_cast<int>(m_clipFrames);
        Flow::Wrapped("%s  %5.1f%%  %.2f / %.2f s  f%d / %d%s",
                      Stem(m_clipPath).c_str(), dur > 0.f ? 100.f * t / dur : 0.f,
                      t, dur, f, ftot, m_clipBound ? "" : "  (binding...)");
        if (m_speed < 0.f)
        {
            Flow::SameLineIfFits(Flow::TextW("REVERSE"));
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "REVERSE");
        }
    }
    else
        Flow::DisabledWrapped("Rest pose - pick Clip in the Preview section to play one.");

    // ---- the scrub ruler: Editor::TimelineCanvas ----
    if (!known) { m_scrubbing = false; return; }
    m_timeline.headerWidth = 0.f;
    m_timeline.rulerHeight = 22.f;
    const std::int32_t frames = std::max<std::int32_t>(1, static_cast<std::int32_t>(m_clipFrames));
    if (m_timeline.Begin("##cpv_timeline", 40.f, frames, fps, 0.f))
    {
        // The fit follows the ruler's width: a fit taken once per clip would
        // run the clip's tail and the playhead off the edge of a narrower panel
        // / splitter / stacked mode (and show empty frames past the end of a
        // wider one). A user zoom / pan (wheel, middle drag) latches the view
        // until right-click or the next clip. The -chainpreview_layout_fixed
        // arm keeps the fit-once rule.
        const float lanesW = m_timeline.lanesW;
        if (lanesW > 1.f)
        {
            const bool refit = !m_timelineFramed ||
                               (!m_harnessLayoutFixed && !m_timelineUserZoom &&
                                std::fabs(lanesW - m_timelineFramedW) > 1.f);
            if (refit)
            {
                m_timeline.FrameAll();
                m_timelineFramed  = true;
                m_timelineFramedW = lanesW;
            }
        }
        if (m_timeline.lanesHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_timelineUserZoom = false;
            m_timeline.FrameAll();
            m_timelineFramed  = lanesW > 1.f;
            m_timelineFramedW = lanesW;
        }
        const float pxBefore = m_timeline.pxPerFrame, scrollBefore = m_timeline.scrollFrames;
        m_timeline.HandleZoomPan();
        if (m_timeline.pxPerFrame != pxBefore || m_timeline.scrollFrames != scrollBefore)
            m_timelineUserZoom = true;
        m_layoutRep.rulerDrawn  = true;
        m_layoutRep.rulerLanesW = lanesW;
        m_layoutRep.rulerFill   = lanesW > 1.f
            ? static_cast<float>(frames) * m_timeline.pxPerFrame / lanesW : 0.f;
        m_timeline.DrawRangeShading(0, frames);
        m_timeline.DrawRuler();
        m_timeline.DrawPlayhead(static_cast<std::int32_t>(std::floor(m_clipT * fps + 1e-4)));
        // Drag anywhere on the ruler / lane = scrub. ProcessPendingActions
        // applies the request (and resets the simulation there). A followed
        // source owns the playhead: no scrub.
        const bool drag = !followMode && m_timeline.lanesActive &&
                          ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (drag)
        {
            const float fr = m_timeline.XToFrame(ImGui::GetIO().MousePos.x);
            m_pendingScrubT = std::clamp(static_cast<double>(fr) / fps, 0.0,
                                         static_cast<double>(m_clipDuration));
        }
        m_scrubbing = drag;
    }
    m_timeline.End();
}

// ==== The Preview Controller and clips (game half) ====
void ChainPhysicsPreviewWindow::ScanClips()
{
    // No "clips compatible with this skeleton" query exists, so the list is
    // every .ianim under the content roots and the read-out says how well the
    // picked one binds. Once per panel session (+ Rescan): the walk is a
    // directory scan, not a load.
    namespace fs = std::filesystem;
    const auto t0 = std::chrono::steady_clock::now();
    m_pendingRescan = false;
    m_clipScanDone  = true;
    m_clipList.clear();
    std::vector<std::string> roots = Resource::AssetGuidIndex::Get().ContentRoots();
    if (roots.empty()) roots.push_back("asset");
    for (const std::string& root : roots)
    {
        // Every search root's files under it - a worktree reading its binary
        // assets from the main checkout holds none of its .ianim itself.
        std::vector<std::string> files;
        Resource::AssetFS::Get().EnumerateLoose(root, files);
        for (const std::string& p : files)
            if (EndsWithIanim(p)) m_clipList.push_back(Sys::RelativizeAssetPath(p));
    }
    std::sort(m_clipList.begin(), m_clipList.end());
    m_clipList.erase(std::unique(m_clipList.begin(), m_clipList.end()), m_clipList.end());
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("[CHAINPREVIEW] clip scan: %u .ianim file(s) under %u root(s) in %.1f ms",
             static_cast<unsigned>(m_clipList.size()),
             static_cast<unsigned>(roots.size()), ms);
}

void ChainPhysicsPreviewWindow::ReleaseClipHandle(Resource::AnimationClipSystem* acs)
{
    if (m_clipHandle.IsValid() && acs) acs->ReleaseClip(m_clipHandle);
    m_clipHandle = Resource::AnimHandle{};
    m_clipHandleOwner = nullptr;
}

void ChainPhysicsPreviewWindow::RemovePreviewAlias(const ChainPreviewContext& ctx)
{
    if (m_clipAlias.empty()) return;
    if (ctx.animClipSys && ctx.world && m_animEnt != NullEntity && ctx.world->IsAlive(m_animEnt))
        ctx.animClipSys->RemoveAnimatorClip(*ctx.world, m_animEnt, m_clipAlias);
    m_clipAlias.clear();
    m_clipBound = false;
}

void ChainPhysicsPreviewWindow::OnPlayheadDiscontinuity(const char* why)
{
    // A playhead that jumps (a clip change, a scrub, a step / jump) feeds the
    // solver a teleport, and integrating across it whips every chain. The reset
    // is only latched here - UpdateSimulation applies it later in this same
    // ProcessPendingActions call. `why` is always a string literal, so the
    // pointer outlives the latch.
    ++m_discontinuities;
    if (std::strcmp(why, "scrub") == 0 && !m_resetOnScrub) return;
    m_pendingSimReset = why;
}

void ChainPhysicsPreviewWindow::SelectClip(const ChainPreviewContext& ctx,
                                           const std::string& path,
                                           const std::string& sourceClip,
                                           const char* why)
{
    const std::string rel = Sys::RelativizeAssetPath(path);
    if (rel.empty()) return;
    ReleaseClipHandle(ctx.animClipSys);
    m_clipPath         = rel;
    m_clipSource       = sourceClip;
    m_controller       = Controller::Clip;
    m_clipApplyPending = true;
    m_clipInfoValid    = false;
    m_clipDuration     = 0.f;
    m_clipFrames       = 0;
    m_clipMatched = m_clipChannels = m_clipBones = 0;
    m_boneClipNames.clear();
    m_morphClipNames.clear();
    m_clipBound        = false;
    m_clipBoundLogged  = false;
    m_clipDeadlineFired = false;
    m_timelineFramed   = false;
    m_timelineUserZoom = false;
    m_clipSelectTick   = m_tick;
    m_clipT            = m_harnessTimeSet ? m_harnessTime : 0.0;
    m_playing          = true;
    m_clock.accumulator = 0.f;
    m_animFrames = m_tposeFrames = 0;
    if (ctx.animClipSys)
    {
        // The panel's OWN reference: GetBoneClipNames / GetResource answer only
        // for a loaded path, and the read-out must not depend on the alias's
        // binding having been reconciled yet.
        m_clipHandle      = ctx.animClipSys->AcquireClip(m_clipPath);
        m_clipHandleOwner = ctx.animClipSys;
    }
    LOG_INFO("[CHAINPREVIEW] clip selected: '%s' source='%s' t0=%.3f (%s)",
             m_clipPath.c_str(), m_clipSource.c_str(), m_clipT, why);
    OnPlayheadDiscontinuity("clip change");
}

bool ChainPhysicsPreviewWindow::ResolveHarnessClip(const ChainPreviewContext& ctx)
{
    const std::string& v = m_harnessClip;
    const bool isPath = v.find('/') != std::string::npos ||
                        v.find('\\') != std::string::npos || EndsWithIanim(v);
    if (isPath)
    {
        // Where the clip loader will look (a worktree's shared content root too).
        if (!Resource::AssetFS::Get().Exists(v))
        {
            LOG_ERROR("[CHAINPREVIEW] -chainpreview_clip='%s' does not exist. "
                      "Refusing to run - the stage would stay in its rest pose "
                      "under the pack's name.", v.c_str());
            return false;
        }
        SelectClip(ctx, v, {}, "-chainpreview_clip path");
        return true;
    }
    // 1. an alias the SUBJECT authored (its AnimationSetComponent)
    for (const AuthoredClip& a : m_authoredClips)
        if (a.name == v)
        {
            LOG_INFO("[CHAINPREVIEW] clip resolved: '%s' -> '%s' (authored alias)",
                     v.c_str(), a.path.c_str());
            SelectClip(ctx, a.path, a.source, "-chainpreview_clip alias");
            return true;
        }
    // 2. a scanned stem: exact (case-insensitive), else a substring. The
    //    SHORTEST candidate wins (then the shorter path, then the name), and
    //    the choice is logged with the candidate count - a name is a
    //    convenience, a pack that must be exact passes a path.
    if (!m_clipScanDone) ScanClips();
    const std::string lv = Lower(v);
    auto pick = [&](bool exact) -> const std::string*
    {
        const std::string* best = nullptr;
        std::uint32_t n = 0;
        for (const std::string& p : m_clipList)
        {
            const std::string s = Lower(Stem(p));
            if (exact ? (s != lv) : (s.find(lv) == std::string::npos)) continue;
            ++n;
            if (!best) { best = &p; continue; }
            const std::size_t bs = Stem(*best).size(), cs = Stem(p).size();
            if (cs < bs || (cs == bs && (p.size() < best->size() ||
                                         (p.size() == best->size() && p < *best))))
                best = &p;
        }
        if (best)
            LOG_INFO("[CHAINPREVIEW] clip resolved: '%s' -> '%s' (%s stem, %u "
                     "candidate(s))", v.c_str(), best->c_str(),
                     exact ? "exact" : "substring", n);
        return best;
    };
    const std::string* hit = pick(true);
    if (!hit) hit = pick(false);
    if (!hit)
    {
        LOG_ERROR("[CHAINPREVIEW] -chainpreview_clip='%s' matches no authored "
                  "alias and no .ianim stem among %u scanned file(s). Refusing "
                  "to run.", v.c_str(), static_cast<unsigned>(m_clipList.size()));
        return false;
    }
    SelectClip(ctx, *hit, {}, "-chainpreview_clip name");
    return true;
}

void ChainPhysicsPreviewWindow::RefreshClipInfo(const ChainPreviewContext& ctx)
{
    Resource::AnimationClipSystem* acs = ctx.animClipSys;
    if (!acs || m_clipPath.empty()) return;
    if (!m_clipHandle.IsValid())
    {
        // Given back by a teardown (Play, a close) while the selection
        // survived: re-take it for the respawned stage.
        m_clipHandle      = acs->AcquireClip(m_clipPath);
        m_clipHandleOwner = acs;
    }
    if (m_clipInfoValid || !acs->IsReady(m_clipHandle)) return;

    m_boneClipNames  = acs->GetBoneClipNames(m_clipPath);
    m_morphClipNames = acs->GetMorphClipNames(m_clipPath);
    m_clipInfoValid  = true;
    const Resource::AnimationResource* res = acs->GetResource(m_clipHandle);
    const int idx = res ? SourceClipIndex(res->clips, m_clipSource) : -1;
    if (idx < 0)
    {
        m_clipDuration = 0.f; m_clipFrames = 0; m_clipMatched = m_clipChannels = 0;
        m_status = "Clip '" + m_clipPath + "' has no bone clip" +
                   (m_clipSource.empty() ? std::string() : " named '" + m_clipSource + "'") + ".";
        LOG_WARNING("[CHAINPREVIEW] %s", m_status.c_str());
        return;
    }
    const Resource::AnimClipData& c = res->clips[static_cast<std::size_t>(idx)];
    m_clipDuration = c.duration;
    m_clipFps      = c.frameRate > 0.f ? c.frameRate : 30.f;
    m_clipFrames   = c.frameCount;
    m_clipChannels = static_cast<std::uint32_t>(c.channels.size());
    m_clipMatched  = 0;
    m_clipBones    = 0;
    // matched = clip channels whose bone-name hash names a bone of the stage
    // skeleton - exactly BindToSkeleton's test.
    World& world = *ctx.world;
    const Entity skelEnt = world.HasComponent<SkeletonComponent>(m_animEnt) ? m_animEnt : m_stageChain;
    if (const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(skelEnt))
    {
        SkeletonRegistry& reg = ctx.renderer->GetSkeletonRegistry();
        if (sk->assetIndex < reg.Count())
        {
            const SkeletonAsset& sa = reg.Get(sk->assetIndex);
            m_clipBones = sa.boneCount;
            std::unordered_set<std::uint32_t> boneHashes;
            const std::uint32_t nb = std::min<std::uint32_t>(sa.boneCount, SkeletonAsset::MAX_BONES);
            boneHashes.reserve(nb);
            for (std::uint32_t b = 0; b < nb; ++b) boneHashes.insert(Fnv32(sa.boneNames[b]));
            for (const Resource::AnimClipChannel& ch : c.channels)
                m_clipMatched += boneHashes.count(Fnv32(ch.name)) ? 1u : 0u;
        }
    }
}

void ChainPhysicsPreviewWindow::LogAnimSummary(const char* when) const
{
    const float fps = m_clipFps > 0.f ? m_clipFps : 30.f;
    LOG_INFO("[CHAINPREVIEW] anim: clip=%d name='%s' matchedChannels=%u channels=%u "
             "bones=%u tpose=%d animatedFrames=%llu tposeFrames=%llu t=%.3f "
             "duration=%.3f frame=%d frames=%u fps=%.2f discontinuities=%llu when=%s",
             m_clipBound ? 1 : 0, Stem(m_clipPath).c_str(), m_clipMatched,
             m_clipChannels, m_clipBones, m_lastTpose ? 1 : 0,
             static_cast<unsigned long long>(m_animFrames),
             static_cast<unsigned long long>(m_tposeFrames), m_clipT, m_clipDuration,
             static_cast<int>(std::floor(m_clipT * fps + 1e-4)), m_clipFrames, fps,
             static_cast<unsigned long long>(m_discontinuities), when);
}

void ChainPhysicsPreviewWindow::UpdateAnimation(const ChainPreviewContext& ctx)
{
    World&    world = *ctx.world;
    Resource::AnimationClipSystem* acs = ctx.animClipSys;

    // Lazy: only when Draw asked (the picker is on screen, or Rescan) - a harness
    // clip name scans inside ResolveHarnessClip; a path and a RestPose session
    // never pay the ~3000-file walk on the spawn frame.
    if (m_pendingRescan) ScanClips();

    // ---- harness clip (once, after the stage exists: aliases are read off it)
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (!m_harnessClip.empty() && !m_harnessClipResolved)
    {
        m_harnessClipResolved = true;
        if (!ResolveHarnessClip(ctx))
        {
            PostQuitMessage(Harness::PreconditionFail);
            return;
        }
    }
#endif

    // ---- requests from Draw ----
    if (m_pendingClipSet)
    {
        m_pendingClipSet = false;
        if (m_pendingClipPath != m_clipPath || m_pendingClipSource != m_clipSource ||
            m_controller != Controller::Clip)
            SelectClip(ctx, m_pendingClipPath, m_pendingClipSource, "picker");
        m_pendingControllerSet = false;
    }
    if (m_pendingControllerSet)
    {
        m_pendingControllerSet = false;
        if (m_pendingController == Controller::Clip && m_clipPath.empty())
            m_controller = Controller::Clip;       // waits for a pick
        else if (m_pendingController != m_controller)
        {
            // Leaving FollowSceneEntity drops its source.
            if (m_controller == Controller::FollowSceneEntity)
            {
                m_followSrc = NullEntity;
                m_followSrcH = NullEntityHandle;
                m_followStatus.clear();
            }
            m_controller = m_pendingController;
            if (m_controller == Controller::Clip) m_clipApplyPending = true;
            // Every controller change teleports the pose (Clip -> RestPose
            // snaps to the reference pose just as hard as the other way), so
            // both directions reset the chains.
            OnPlayheadDiscontinuity("controller change");
        }
    }

    if (!acs || m_animEnt == NullEntity || !world.IsAlive(m_animEnt)) return;

    AnimationComponent* anim = world.GetComponent<AnimationComponent>(m_animEnt);

    // FollowSceneEntity plays through the Clip machinery (the alias, the bind,
    // the per-frame clock write) - only the playhead comes from the source
    // (TickFollow) instead of the transport.
    const bool follow = m_controller == Controller::FollowSceneEntity;
    if ((m_controller != Controller::Clip && !follow) || m_clipPath.empty())
    {
        // REST POSE: no clip on the stage. The authored default clip (if the
        // subject plays one on load) is stopped too - AnimationSystem then
        // writes the skeleton's rest pose, which is what this mode promises.
        RemovePreviewAlias(ctx);
        if (anim && anim->primaryClip != kInvalidAnimHandle)
            acs->StopAnimator(world, m_animEnt);
        m_clipBound = false;
        return;
    }

    // ---- (re)register the alias: two steps ----
    // QueueAnimatorPlay resolves a clip name through the entity's
    // AnimationSetComponent alias table and returns false for anything not in
    // it, so a picked path needs its alias written first. The old preview alias
    // is removed before the new one is queued: removing the current clip stops
    // the animator, which would also drop a queued request.
    if (m_clipApplyPending)
    {
        m_clipApplyPending = false;
        RemovePreviewAlias(ctx);
        const std::string alias = "__cpv_" + Stem(m_clipPath);
        const bool registered = acs->SetAnimatorClip(world, m_animEnt, alias, m_clipPath,
                                                     /*looping=*/true, /*speed=*/1.f,
                                                     m_clipSource);
        // blendDuration 0 is deliberate: with animDt = 0 a cross-fade can never
        // start and is hard-cut anyway (blendCannotStart); a preview wants the
        // hard cut.
        const bool queued = registered &&
            acs->QueueAnimatorPlay(world, m_animEnt, alias, /*blend=*/0.f, /*restart=*/false);
        if (registered) m_clipAlias = alias;
        m_clipSelectTick = m_tick;
        LOG_INFO("[CHAINPREVIEW] clip: alias='%s' path='%s' source='%s' "
                 "aliasRegistered=%d queued=%d entity=%u",
                 alias.c_str(), m_clipPath.c_str(), m_clipSource.c_str(),
                 registered ? 1 : 0, queued ? 1 : 0, static_cast<unsigned>(m_animEnt));
    }
    RefreshClipInfo(ctx);

    // ---- the transport: arithmetic on m_clipT only ----
    const double dur = (m_clipInfoValid && m_clipDuration > 0.f) ? m_clipDuration : 0.0;
    const double fps = m_clipFps > 0.f ? m_clipFps : 30.0;
    if (m_pendingJump != 0)
    {
        m_clipT = (m_pendingJump < 0) ? 0.0 : dur;
        m_pendingJump = 0;
        OnPlayheadDiscontinuity("jump");
    }
    if (m_pendingStep != 0)
    {
        m_playing = false;
        m_clipT  += static_cast<double>(m_pendingStep) / fps;
        m_pendingStep = 0;
        OnPlayheadDiscontinuity("step");
    }
    if (m_pendingScrubT >= 0.0)
    {
        m_clipT = m_pendingScrubT;
        m_pendingScrubT = -1.0;
        OnPlayheadDiscontinuity("scrub");
    }
    if (m_pendingPlayToggle)
    {
        m_pendingPlayToggle = false;
        m_playing = !m_playing;
        // Play at the END of a non-looping clip starts it over (reverse: from
        // the end), rather than doing nothing.
        if (m_playing && !m_looping && dur > 0.0)
        {
            if (m_speed >= 0.f && m_clipT >= dur) m_clipT = 0.0;
            if (m_speed <  0.f && m_clipT <= 0.0) m_clipT = dur;
        }
    }
    // This frame's step count (TickStageClock): the chain lane runs exactly these
    // steps too, so clip time and chain time are one clock. (A harness run pins
    // it to one fixed step per editor frame unless -chainpreview_steps /
    // -chainpreview_realdt, see HarnessSetSubject.)
    const float         stepDt = m_frameStepDt;
    const std::uint32_t steps  = m_frameSteps;
    // Time runs even before the clip has loaded (dur == 0), so the harness
    // clock stays a function of the frame number; it is wrapped once the
    // duration is known.
    if (m_playing && !m_scrubbing && !follow)
        for (std::uint32_t i = 0; i < steps; ++i)
            m_clipT += static_cast<double>(stepDt) * static_cast<double>(m_speed);
    if (dur > 0.0)
    {
        if (m_looping)
        {
            m_clipT = std::fmod(m_clipT, dur);
            if (m_clipT < 0.0) m_clipT += dur;
        }
        else
        {
            if (m_clipT >= dur) { m_clipT = dur; if (m_speed > 0.f) m_playing = false; }
            if (m_clipT <= 0.0) { m_clipT = 0.0; if (m_speed < 0.f) m_playing = false; }
        }
    }
    else if (!std::isfinite(m_clipT) || m_clipT < 0.0)
        m_clipT = 0.0;

    // ---- the per-frame clock write (the Sequencer's shape) ----
    // AnimationSystem samples at primaryTime unconditionally; only the advance
    // is gated (externalTime / Edit mode's animDt = 0), so the panel owns the
    // clock outright. prevNotifyTime = -1 re-arms the notify cursor the way
    // Animator.SetTime does - scrubbing must not replay every notify it crosses.
    if (anim)
    {
        anim->externalTime   = true;
        anim->paused         = false;
        anim->primaryTime    = static_cast<float>(m_clipT);
        anim->prevNotifyTime = -1.f;
    }

    // ---- read-out: is our clip current, and did the stage get a pose? ----
    // poseByteOffset == ~0u is LocalToWorldSystem's "no pose this frame"
    // sentinel - exactly what an anim-culled skeleton gets, and what
    // -chainpreview_pin_off produces.
    const AnimatorTransitionComponent* tr = world.GetComponent<AnimatorTransitionComponent>(m_animEnt);
    const bool wasBound = m_clipBound;
    m_clipBound = anim && tr && !m_clipAlias.empty() &&
                  tr->currentClipName == m_clipAlias &&
                  anim->primaryClip != kInvalidAnimHandle;
    // The clip lands frames after it was picked (async load +
    // QueueAnimatorPlay), and the pose jumps from the previous one to the clip's
    // on that frame - the teleport a clip change must not feed the solver. So the
    // reset rides the bind, not only the pick.
    if (m_clipBound && !wasBound) OnPlayheadDiscontinuity("clip change");
    const Entity skelEnt = world.HasComponent<SkeletonComponent>(m_animEnt) ? m_animEnt : m_stageChain;
    const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(skelEnt);
    m_lastTpose = !sk || sk->poseByteOffset == ~0u;
    if (m_clipBound)
    {
        if (m_lastTpose) ++m_tposeFrames; else ++m_animFrames;
        if (!m_clipBoundLogged && m_clipInfoValid)
        {
            m_clipBoundLogged = true;
            LogAnimSummary("bound");
        }
    }
#if defined(WITH_HARNESS) && WITH_HARNESS
    // The clip deadline: a harness clip that never became the stage's current
    // clip leaves a rest-pose picture under the pack's name.
    else if (!m_harnessClip.empty() && !m_clipDeadlineFired &&
             m_tick >= m_clipSelectTick + kClipDeadline)
    {
        m_clipDeadlineFired = true;
        LOG_ERROR("[CHAINPREVIEW] -chainpreview_clip='%s' ('%s') never became the "
                  "stage's current clip in %llu editor frames (alias='%s' "
                  "loaded=%d). Refusing to run.", m_harnessClip.c_str(),
                  m_clipPath.c_str(), static_cast<unsigned long long>(kClipDeadline),
                  m_clipAlias.c_str(), m_clipInfoValid ? 1 : 0);
        PostQuitMessage(Harness::PreconditionFail);
    }
#endif
}

// ==== The chain solver's preview lane ====
void ChainPhysicsPreviewWindow::UpdateSimulation(const ChainPreviewContext& ctx)
{
    Renderer& renderer = *ctx.renderer;
    World&    world    = *ctx.world;
    ChainPhysicsSystem* cps = renderer.GetChainPhysicsSystem();
    if (!cps) return;

#if defined(WITH_HARNESS) && WITH_HARNESS
    if (m_harnessResetAt != 0 && !m_harnessResetDone && m_tick >= m_harnessResetAt)
    {
        m_harnessResetDone = true;
        LOG_INFO("[CHAINPREVIEW] -chainpreview_reset_at: resetting the stage's "
                 "simulation at editor frame %llu",
                 static_cast<unsigned long long>(m_tick));
        m_pendingSimReset = "-chainpreview_reset_at";
    }
#endif
    // ---- is the lane live this frame? ----
    // Reverse playback turns it off: the solver returns on a non-positive dt,
    // so a "negative step" cannot be expressed at all - the chains follow the
    // clip rigidly (Edit-mode rest pose) until playback runs forward again.
    // -chainpreview_dt=0 (a reverse arm) never lets it run.
    const bool reverse  = m_controller == Controller::Clip && !m_clipPath.empty() &&
                          m_playing && m_speed < 0.f;
    const bool live     = m_simulate && !reverse && !m_harnessLaneOff;
    // Particles integrated against an earlier pose (before Simulate went off /
    // reverse started) would whip across the gap: going live again restarts the
    // chains from the current pose instead.
    if (live && m_simStale && !m_pendingSimReset) m_pendingSimReset = "simulate resumed";

    if (m_pendingSimReset)
    {
        ResetSimulation(renderer, world, m_pendingSimReset);
        m_pendingSimReset = nullptr;
    }

    // ---- the per-frame push: the clock's N ----
    // TickStageClock produced this frame's N fixed steps of m_frameStepDt and
    // UpdateAnimation has already advanced the clip by the same N, so the lane
    // runs N passes. The clip is sampled once per editor frame (at its end
    // time); the solver hands pass k the pose interpolated k/N of the way from
    // the previous frame's input and pass N the sample itself - the documented
    // approximation, exact for a clip that is linear over the frame (see
    // ChainPhysicsSystem::SetPreviewLane). N = 0 with the lane live is a hold:
    // the solver re-writes the last simulated pose from the particles instead of
    // letting the clip's raw pose through (which flickers at editor rates above
    // 60 Hz); it is also what makes Pause freeze the swing. Not live = dt 0 = the
    // Edit-mode rest pose.
    float               laneDt    = live ? m_frameStepDt : 0.f;
    const std::uint32_t laneSteps = live ? m_frameSteps : 0u;
    // -chainpreview_hold_off (the pause pack's reverse arm): a step-less frame
    // pushes dt 0, i.e. falls back to the raw clip pose.
    if (live && laneSteps == 0u && m_harnessHoldOff) laneDt = 0.f;
    // The only writer of the lane, in the serial window. TickAnimationChain reads
    // the set inside ChainPhysicsSystem::Update, which never overlaps this call;
    // m_previewSet itself is only rewritten in this same window (spawn, teardown,
    // RestoreGlobals, the epoch drop, ValidateStageHandles, the A/B shadow).
    renderer.SetChainPhysicsPreviewLane(&m_previewSet, laneDt, laneSteps);
    m_lanePushed    = true;
    // The preview scene (gravity override + the floor proxy capsule), pushed
    // with the lane every frame from this same window - the solver reads both
    // only for Lane::Preview jobs, and the lane hand-back (RestoreGlobals ->
    // SetPreviewLane(nullptr)) drops them.
    PushPreviewScene(*cps);
    m_lastSimDt     = laneDt;
    m_lastLaneSteps = laneSteps;
    if (live && laneSteps > 0u) m_laneHasState = true;
    if (!live && m_laneHasState) m_simStale = true;
    // The hold check's record of what THIS frame asked the lane to do.
    m_prevPush = m_lastPush;
    m_lastPush.mode = laneDt <= 0.f ? PushMode::Off
                    : laneSteps > 0u ? PushMode::Step : PushMode::Hold;
    m_lastPush.clipT       = m_clipT;
    m_lastPush.resetSerial = m_resetSerial;
    // The periodic "[CHAINPHYS] preview lane:" line is for a -chainpreview_*
    // harness run only; an interactive session gets the closing summary alone.
#if defined(WITH_HARNESS) && WITH_HARNESS
    cps->SetPreviewLaneLogEvery(m_harnessSubject.empty() ? 0u : 60u);
#else
    cps->SetPreviewLaneLogEvery(0u);
#endif

    // Draw never reads the solver: the read-out is copied here.
    m_uiLaneStats = cps->GetPreviewLaneStats();
    m_uiSwayTime  = cps->GetPreviewSwayTime();
}

void ChainPhysicsPreviewWindow::ResetSimulation(Renderer& renderer, World& world,
                                                const char* why)
{
    ChainPhysicsSystem* cps = renderer.GetChainPhysicsSystem();
    if (!cps) return;
    // Pool-driven: every stage entity that carries a chain. Single-entity
    // resets only - Renderer::ResetChainPhysicsSimulation would clear the
    // Verlet history of every character in the user's scene.
    std::uint32_t n = 0;
    if (auto* cp = world.GetPool<ChainPhysicsComponent>())
        for (Entity e : cp->Entities())
            if (m_previewSet.count(e)) { cps->ResetPreviewEntity(e); ++n; }
    ++m_simResets;
    // Nothing stale any more, and the hold check must not compare a pose across
    // this reset.
    ++m_resetSerial;
    m_laneHasState = false;
    m_simStale     = false;
    // A drag on the scrub ruler resets every frame; one line per drag frame
    // would bury the log, so scrubs are counted (resets=) but not printed.
    if (std::strcmp(why, "scrub") != 0)
        LOG_INFO("[CHAINPREVIEW] sim reset: entities=%u resets=%llu why=%s", n,
                 static_cast<unsigned long long>(m_simResets), why);
}

// ==== The stage's one clock and the simulation transport ====
// Everything here runs in ProcessPendingActions (the game half's serial
// window); Draw only posts m_pending* requests.
void ChainPhysicsPreviewWindow::LogTransport(const char* what) const
{
    LOG_INFO("[CHAINPREVIEW] sim transport: %s totalSteps=%llu clipT=%.4f paused=%d "
             "simulate=%d fixedDt=%.5f timeScale=%.3f", what,
             static_cast<unsigned long long>(m_clock.totalSteps), m_clipT,
             m_clock.paused ? 1 : 0, m_simulate ? 1 : 0, m_clock.fixedDt,
             m_clock.timeScale);
}

void ChainPhysicsPreviewWindow::LogClock(const char* when) const
{
    // One line, name=number pairs (logLineFields reads it; the LAST one in a
    // log is the session's closing line). source / mode are words and are
    // deliberately not name=number.
    const char* source = !m_clock.pinSteps.empty() ? "pin"
                       : !m_clock.feedDt.empty()   ? "realdt" : "realtime";
    LOG_INFO("[CHAINPREVIEW] clock: totalSteps=%llu frames=%llu zeroStepFrames=%llu "
             "maxStepsPerFrame=%u lastSteps=%u dropped=%llu fixedDt=%.5f followEditor=%d "
             "timeScale=%.3f paused=%d pausedFrames=%llu stepsWhilePaused=%llu "
             "stageTime=%.4f clipT=%.4f simulate=%d laneOff=%d holdChecks=%llu "
             "holdMismatches=%llu sceneCpuMsOpen=%.3f sceneCpuMs=%.3f source=%s when=%s",
             static_cast<unsigned long long>(m_clock.totalSteps),
             static_cast<unsigned long long>(m_clock.frames),
             static_cast<unsigned long long>(m_clock.zeroStepFrames),
             m_clock.maxStepsPerFrame, m_clock.lastSteps,
             static_cast<unsigned long long>(m_clock.droppedSteps),
             m_clock.fixedDt, m_clock.fixedDt <= 0.f ? 1 : 0, m_clock.timeScale,
             m_clock.paused ? 1 : 0,
             static_cast<unsigned long long>(m_clock.pausedFrames),
             static_cast<unsigned long long>(m_clock.stepsWhilePaused),
             m_clock.stageTime, m_clipT, m_simulate ? 1 : 0, m_harnessLaneOff ? 1 : 0,
             static_cast<unsigned long long>(m_holdChecks),
             static_cast<unsigned long long>(m_holdMismatches),
             m_sceneCpuMsAtOpen, Anim::AnimProfile::Get().chainCpuMs,
             source, when ? when : "");
}

std::uint32_t ChainPhysicsPreviewWindow::HashStagePose(Renderer& renderer, World& world) const
{
    // The post-chain LocalPose of every preview chain carrier (pool order) -
    // what the LAST frame's chain pass left, which is what skinning drew.
    AnimationSystem* anim = renderer.GetAnimationSystem();
    std::uint32_t h = 2166136261u;
    if (!anim) return h;
    auto fnv = [](std::uint32_t x, const void* p, std::size_t n)
    {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) x = (x ^ b[i]) * 16777619u;
        return x;
    };
    if (auto* cp = world.GetPool<ChainPhysicsComponent>())
        for (Entity e : cp->Entities())
        {
            if (!m_previewSet.count(e)) continue;
            const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(e);
            const AnimationSystem::LocalPose* lp = anim->GetLocalPose(e);
            if (!sk || !lp) continue;
            h = fnv(h, lp, static_cast<std::size_t>(sk->boneCount) * sizeof(lp[0]));
        }
    return h;
}

void ChainPhysicsPreviewWindow::FullReset(const ChainPreviewContext& ctx, const char* why)
{
    // "Reset Sim": the chains drop their state (ResetPreviewEntity per carrier -
    // never the scene-wide ResetChainPhysicsSimulation), the playhead goes back
    // to its start (the harness's -chainpreview_time, else 0), the preview lane's
    // sway phase restarts and the clock banks nothing. No frame of dt 0 is
    // needed: this runs in PPA before the frame's AnimationSystem, which re-seeds
    // the reset carriers' LocalPose, so the first pass of this very frame
    // InitEntity's from the fresh pose.
    m_clipT = m_harnessTimeSet ? m_harnessTime : 0.0;
    m_clock.Rewind();
    ResetSimulation(*ctx.renderer, *ctx.world, why);
    if (ChainPhysicsSystem* cps = ctx.renderer->GetChainPhysicsSystem())
        cps->ResetPreviewSwayTime();
    m_pendingSimReset = nullptr;   // this reset covers any latched one
    // The driven root goes back to where it started too (the speeds and the
    // program selection are settings and stay), so two Resets replay
    // bit-identical windows with a driver running.
    m_driveOffset[0] = m_driveOffset[1] = m_driveOffset[2] = 0.f;
    m_driveYawDeg = 0.f;
    m_driveProgT  = 0.0;
    m_driveDir    = 1.f;
    m_turntableYaw = 0.f;
}

void ChainPhysicsPreviewWindow::TickSimTransport(const ChainPreviewContext& ctx)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    // ---- the hold check: did a step-less frame reproduce the pose? ----
    // Harness diagnostics (a pose hash a frame): when LAST frame HELD and the
    // frame before it stepped or held at the SAME playhead with no reset in
    // between, the pose skinning drew must be bit-identical to the one before.
    if (!m_harnessSubject.empty() && m_stageRoot != NullEntity && m_lanePushed)
    {
        const std::uint32_t h = HashStagePose(*ctx.renderer, *ctx.world);
        const bool comparable =
            m_prevPoseValid && m_lastPush.mode == PushMode::Hold &&
            (m_prevPush.mode == PushMode::Step || m_prevPush.mode == PushMode::Hold) &&
            m_lastPush.clipT == m_prevPush.clipT &&
            m_lastPush.resetSerial == m_prevPush.resetSerial &&
            // A reset this frame, before this check (a structural sync in
            // TickStagedParams - e.g. the rest-pose gizmo's release while the
            // clock is still held) legitimately changes the pose it hashes.
            m_resetSerial == m_lastPush.resetSerial;
        if (comparable)
        {
            ++m_holdChecks;
            if (h != m_prevPoseHash && ++m_holdMismatches <= 3)
                LOG_WARNING("[CHAINPREVIEW] hold check: the held pose differs from "
                            "the previous frame's (hash %u vs %u, editor frame %llu) "
                            "- a step-less frame did not reproduce the simulated pose",
                            h, m_prevPoseHash, static_cast<unsigned long long>(m_tick));
        }
        m_prevPoseHash  = h;
        m_prevPoseValid = true;
    }
    else
        m_prevPoseValid = false;

    // ---- harness transport hooks ----
    if (m_harnessPauseAt != 0 && !m_harnessPauseDone && m_tick >= m_harnessPauseAt &&
        m_stageRoot != NullEntity)
    {
        m_harnessPauseDone = true;
        LOG_INFO("[CHAINPREVIEW] -chainpreview_pause_at: pausing the stage clock at "
                 "editor frame %llu", static_cast<unsigned long long>(m_tick));
        if (!m_clock.paused) m_pendingPauseToggle = true;
    }
    if (m_harnessStepAt != 0 && !m_harnessStepDone && m_tick >= m_harnessStepAt &&
        m_stageRoot != NullEntity)
    {
        m_harnessStepDone = true;
        LOG_INFO("[CHAINPREVIEW] -chainpreview_step_at: one step at editor frame %llu",
                 static_cast<unsigned long long>(m_tick));
        m_pendingSimStep = true;
    }
    // -chainpreview_fullreset_at: the Reset button.
    while (m_harnessFullResetNext < m_harnessFullResetAt.size() &&
           m_tick >= m_harnessFullResetAt[m_harnessFullResetNext] && m_stageRoot != NullEntity)
    {
        ++m_harnessFullResetNext;
        LOG_INFO("[CHAINPREVIEW] -chainpreview_fullreset_at: Reset at editor frame %llu",
                 static_cast<unsigned long long>(m_tick));
        m_pendingFullReset = true;
    }
#endif

    // ---- Draw's requests ----
    if (m_pendingSimulate >= 0)
    {
        const bool on = m_pendingSimulate == 1;
        m_pendingSimulate = -1;
        if (on != m_simulate)
        {
            m_simulate = on;
            // Pause / Step are greyed with Simulate off - never leave the
            // stage clock frozen behind a disabled button.
            if (!on) { m_clock.paused = false; m_clock.stepOnce = false; }
            LogTransport(on ? "simulate on" : "simulate off");
        }
    }
    if (m_pendingPauseToggle)
    {
        m_pendingPauseToggle = false;
        m_clock.paused = !m_clock.paused;
        LogTransport(m_clock.paused ? "pause" : "resume");
    }
    if (m_pendingSimStep)
    {
        m_pendingSimStep = false;
        m_clock.stepOnce = true;
        LogTransport("step");
    }
    if (m_pendingTimeScale >= 0.f)
    {
        m_clock.timeScale = std::clamp(m_pendingTimeScale, 0.1f, 2.f);
        m_pendingTimeScale = -1.f;
        LogTransport("time scale");
    }
    if (m_pendingFixedDt >= 0.f)
    {
        // A new step size: whatever was banked in the old one is dropped. The
        // chains need no reset - PrepareSubsteps re-scales the Verlet history
        // across a step-size change.
        m_clock.fixedDt = m_pendingFixedDt;
        m_clock.accumulator = 0.f;
        m_pendingFixedDt = -1.f;
        LogTransport(m_clock.fixedDt > 0.f ? "dt fixed" : "dt follow editor");
    }
    if (m_pendingFullReset)
    {
        m_pendingFullReset = false;
        if (m_stageRoot != NullEntity)
        {
            FullReset(ctx, "reset");
            LogTransport("reset");
        }
    }

    // ---- the harness A/B windows (-chainpreview_ab) ----
    // Window boundaries live HERE, before the clock: a window's end is captured
    // at the START of the frame after its last step (that frame's passes have
    // run by now), and the next window's Reset lands before this frame's clock
    // produces the first step of it.
    switch (m_abPhase)
    {
    case AbPhase::Wait:
    {
        const bool ready = m_stageRoot != NullEntity && m_lanePushed &&
                           (m_controller != Controller::Clip ||
                            (m_clipBound && m_clipInfoValid));
        if (!ready) { m_abWait = 0; break; }
        if (++m_abWait < 10u) break;   // a few frames past the clip's bind reset
        StartAbWindow(ctx, 0);
        break;
    }
    case AbPhase::Capture:
        CaptureAbWindow(ctx, m_abIndex);
        if (m_abIndex + 1 < m_abWin.size())
            StartAbWindow(ctx, m_abIndex + 1);
        else
        {
            // back to the run's own pacing
            m_clock.pinSteps = m_abPacing[0].steps;
            m_clock.feedDt   = m_abPacing[0].realDt;
            m_clock.Rewind();
            for (std::size_t w = 1; w < m_abWin.size(); ++w) LogAbCompare(w);
            m_abPhase = AbPhase::Done;
        }
        break;
    default:
        break;
    }
}

void ChainPhysicsPreviewWindow::TickStageClock(const ChainPreviewContext& ctx)
{
    // The one step count of this editor frame. Interactive: StageClock::Advance
    // on the editor's real delta (fixed step + catch-up, at most
    // kMaxCatchupSteps, the excess dropped). Harness: pinned (default one step
    // per frame) or fed synthetic frame times - see StageClock.
    float stepDt = ChainPreview::kFixedDt;
    std::uint32_t n = m_clock.Advance(ctx.realDt, &stepDt);

    // A harness A/B window TRUNCATES its last frame to exactly its length in
    // fixed steps, so both windows end on the same simulated instant however
    // their frames grouped the steps. (Follow-editor steps vary in size; that
    // window ends on the first frame that reaches its stage time.)
    if (m_abPhase == AbPhase::Run && m_abIndex < m_abWin.size())
    {
        AbWindow& w = m_abWin[m_abIndex];
        if (m_abTarget != 0 && w.steps + n > m_abTarget)
            n = static_cast<std::uint32_t>(m_abTarget - w.steps);
        w.steps     += n;
        w.frames    += 1;
        w.zeroFrames += (n == 0) ? 1u : 0u;
        w.maxSteps   = std::max(w.maxSteps, n);
        w.stageTime += static_cast<double>(n) * static_cast<double>(stepDt);
        const bool done = m_abTarget != 0 ? (w.steps >= m_abTarget)
                                          : (w.stageTime >= m_abSeconds - 1e-6);
        if (done)
        {
            w.dropped = m_clock.droppedSteps - w.dropped0;
            m_abPhase = AbPhase::Capture;
        }
    }
    m_clock.Commit(n, stepDt);
    m_frameSteps  = n;
    m_frameStepDt = stepDt;
}

void ChainPhysicsPreviewWindow::StartAbWindow(const ChainPreviewContext& ctx, std::size_t which)
{
    if (which >= m_abWin.size() || which >= m_abPacing.size()) { m_abPhase = AbPhase::Done; return; }
    if (which == 0 && !m_abSavedA)
    {
        // Window a runs the run's own pacing: saved once (it is re-applied on
        // a restart after a respawn mid-A/B, and after the last window).
        m_abPacing[0].steps  = m_clock.pinSteps;
        m_abPacing[0].realDt = m_clock.feedDt;
        m_abSavedA = true;
    }
    const AbPacing& p = (m_abPacing[which].steps.empty() && m_abPacing[which].realDt.empty())
                        ? m_abPacing[0] : m_abPacing[which];
    m_clock.pinSteps = p.steps;
    m_clock.feedDt   = p.realDt;
    m_abWin[which] = AbWindow{};
    m_abWin[which].dropped0 = m_clock.droppedSteps;
    // Fixed mode: the window is an exact step count. Follow editor: stage time.
    m_abTarget = m_clock.fixedDt > 0.f
        ? static_cast<std::uint64_t>(std::llround(m_abSeconds / static_cast<double>(m_clock.fixedDt)))
        : 0u;
    static const char* kWhy[] = { "ab window a", "ab window b", "ab window c", "ab window d" };
    FullReset(ctx, kWhy[std::min<std::size_t>(which, 3)]);
    m_abIndex = which;
    m_abPhase = AbPhase::Run;
    LOG_INFO("[CHAINPREVIEW] ab: window %c starts at editor frame %llu (seconds=%.3f "
             "targetSteps=%llu fixedDt=%.5f pacing %s)", static_cast<char>('a' + which),
             static_cast<unsigned long long>(m_tick), m_abSeconds,
             static_cast<unsigned long long>(m_abTarget), m_clock.fixedDt,
             !m_clock.pinSteps.empty() ? "pinned steps" :
             !m_clock.feedDt.empty() ? "fed frame times" : "real time");
}

void ChainPhysicsPreviewWindow::CaptureAbWindow(const ChainPreviewContext& ctx, std::size_t which)
{
    if (which >= m_abWin.size()) return;
    AbWindow& w = m_abWin[which];
    w.clipT = m_clipT;   // not advanced yet this frame: the window's end
    ChainPhysicsSystem* cps  = ctx.renderer->GetChainPhysicsSystem();
    AnimationSystem*    anim = ctx.renderer->GetAnimationSystem();
    World& world = *ctx.world;
    auto fnv = [](std::uint32_t x, const void* p, std::size_t n)
    {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) x = (x ^ b[i]) * 16777619u;
        return x;
    };
    w.particleHash = 2166136261u;
    w.poseHash     = 2166136261u;
    w.particles.clear();
    w.maxOffset = 0.f;
    std::vector<DirectX::XMFLOAT3> pos, rest;
    if (auto* cp = world.GetPool<ChainPhysicsComponent>())
        for (Entity e : cp->Entities())
        {
            if (!m_previewSet.count(e)) continue;
            if (cps && cps->CopyParticleState(e, &pos, &rest))
            {
                w.particles.insert(w.particles.end(), pos.begin(), pos.end());
                for (std::size_t i = 0; i < pos.size() && i < rest.size(); ++i)
                {
                    const float dx = pos[i].x - rest[i].x, dy = pos[i].y - rest[i].y,
                                dz = pos[i].z - rest[i].z;
                    w.maxOffset = std::max(w.maxOffset, std::sqrt(dx * dx + dy * dy + dz * dz));
                }
            }
            if (anim)
                if (const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(e))
                    if (const AnimationSystem::LocalPose* lp = anim->GetLocalPose(e))
                        w.poseHash = fnv(w.poseHash, lp,
                                         static_cast<std::size_t>(sk->boneCount) * sizeof(lp[0]));
        }
    if (!w.particles.empty())
        w.particleHash = fnv(w.particleHash, w.particles.data(),
                             w.particles.size() * sizeof(w.particles[0]));
    LOG_INFO("[CHAINPREVIEW] ab window %c: steps=%llu frames=%llu zeroStepFrames=%llu "
             "maxStepsPerFrame=%u dropped=%llu stageTime=%.4f clipT=%.4f particles=%u "
             "particleHash=%u poseHash=%u maxOffset=%.4f",
             static_cast<char>('a' + which),
             static_cast<unsigned long long>(w.steps),
             static_cast<unsigned long long>(w.frames),
             static_cast<unsigned long long>(w.zeroFrames), w.maxSteps,
             static_cast<unsigned long long>(w.dropped), w.stageTime, w.clipT,
             static_cast<unsigned>(w.particles.size()), w.particleHash, w.poseHash,
             w.maxOffset);
}

// Window `which` against window a. Field names say A (window a) and B (the
// compared window); the prefix names which one ("ab compare b:" / "c:").
void ChainPhysicsPreviewWindow::LogAbCompare(std::size_t which) const
{
    if (which == 0 || which >= m_abWin.size()) return;
    const AbWindow& a = m_abWin[0];
    const AbWindow& b = m_abWin[which];
    const bool sameCount = a.particles.size() == b.particles.size() && !a.particles.empty();
    double maxDelta = sameCount ? 0.0 : 1e9, sum = 0.0;
    // The distribution too: a chain in CONTACT is chaotic (a tiny target
    // difference flips a capsule contact and the tip goes elsewhere), so the
    // max is one particle's story and the median / p95 are the population's.
    std::vector<double> ds;
    if (sameCount)
    {
        ds.reserve(a.particles.size());
        for (std::size_t i = 0; i < a.particles.size(); ++i)
        {
            const double dx = static_cast<double>(a.particles[i].x) - b.particles[i].x;
            const double dy = static_cast<double>(a.particles[i].y) - b.particles[i].y;
            const double dz = static_cast<double>(a.particles[i].z) - b.particles[i].z;
            const double d  = std::sqrt(dx * dx + dy * dy + dz * dz);
            maxDelta = std::max(maxDelta, d);
            sum += d;
            ds.push_back(d);
        }
        std::sort(ds.begin(), ds.end());
    }
    auto pct = [&ds](double p)   // nearest rank, the runner's definition
    {
        if (ds.empty()) return 1e9;
        const std::size_t k = static_cast<std::size_t>(std::ceil(p * static_cast<double>(ds.size())));
        return ds[std::min(ds.size() - 1, k == 0 ? 0 : k - 1)];
    };
    const double meanDelta = sameCount ? sum / static_cast<double>(a.particles.size()) : 1e9;
    const double p50Delta  = pct(0.50), p95Delta = pct(0.95);
    const bool equal = sameCount &&
        std::memcmp(a.particles.data(), b.particles.data(),
                    a.particles.size() * sizeof(a.particles[0])) == 0 &&
        a.poseHash == b.poseHash;
    LOG_INFO("[CHAINPREVIEW] ab compare %c: window=%u seconds=%.3f stepsA=%llu stepsB=%llu "
             "framesA=%llu framesB=%llu zeroStepFramesA=%llu zeroStepFramesB=%llu "
             "maxStepsPerFrameB=%u droppedB=%llu "
             "particles=%u particlesB=%u maxDelta=%.6f meanDelta=%.6f p50Delta=%.6f "
             "p95Delta=%.6f particleHashA=%u particleHashB=%u poseHashA=%u poseHashB=%u "
             "clipTA=%.4f clipTB=%.4f maxOffsetA=%.4f maxOffsetB=%.4f equal=%d",
             static_cast<char>('a' + which), static_cast<unsigned>(which), m_abSeconds,
             static_cast<unsigned long long>(a.steps), static_cast<unsigned long long>(b.steps),
             static_cast<unsigned long long>(a.frames), static_cast<unsigned long long>(b.frames),
             static_cast<unsigned long long>(a.zeroFrames),
             static_cast<unsigned long long>(b.zeroFrames), b.maxSteps,
             static_cast<unsigned long long>(b.dropped),
             static_cast<unsigned>(a.particles.size()), static_cast<unsigned>(b.particles.size()),
             maxDelta, meanDelta, p50Delta, p95Delta, a.particleHash, b.particleHash,
             a.poseHash, b.poseHash, a.clipT, b.clipT, a.maxOffset, b.maxOffset, equal ? 1 : 0);
}

// ---- the Simulation section, UI half ----
void ChainPhysicsPreviewWindow::DrawSimulationUi()
{
    // Draw never reads the solver or the World: the read-outs are what PPA
    // copied (m_uiLaneStats, m_lastSimDt, the clock's totals), and every
    // button only posts a request TickSimTransport applies.
    if (m_stageRoot == NullEntity) { ImGui::TextDisabled("No stage."); return; }
    namespace Flow = EditorUI::Flow;
    const ImVec4 kWarn(1.f, 0.8f, 0.3f, 1.f);
    const bool reverse = m_controller == Controller::Clip && !m_clipPath.empty() &&
                         m_playing && m_speed < 0.f;
    const bool simOn   = (m_pendingSimulate >= 0) ? (m_pendingSimulate == 1) : m_simulate;
    const bool paused  = m_clock.paused != m_pendingPauseToggle;   // what it is about to be
    const float stepLen = m_clock.fixedDt > 0.f ? m_clock.fixedDt : ChainPreview::kFixedDt;

    // ---- Simulate / Pause / Step / Reset ----
    ImGui::BeginDisabled(reverse);
    bool sim = simOn;
    if (ImGui::Checkbox("Simulate", &sim)) m_pendingSimulate = sim ? 1 : 0;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(reverse
            ? "Disabled during REVERSE playback: the solver refuses a negative dt\n"
              "(it returns on !(dt > 0)), so the chains follow the clip rigidly.\n"
              "Play forward again and the simulation restarts from that pose."
            : "Run the chain solver on the stage's own clock. Off: the chains follow\n"
              "the clip rigidly (plus the authored rest pose) - the 'is it the clip\n"
              "or the chain?' check. Turning it back on restarts the chains from the\n"
              "current pose.");
    Flow::SameLineIfFits(Flow::ButtonW("Resume"));
    ImGui::BeginDisabled(!simOn || reverse);
    if (ImGui::Button(paused ? "Resume" : "Pause")) m_pendingPauseToggle = !m_pendingPauseToggle;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(!simOn ? "Nothing to pause - Simulate is off."
                          : reverse ? "Nothing to pause - reverse playback holds the simulation."
                          : "Stops the STAGE clock: clip and chains freeze together and the\n"
                            "chains HOLD their simulated pose. (The clip's own play / pause\n"
                            "below stops only the clip - the chains keep settling.)");
    Flow::SameLineIfFits(Flow::ButtonW("Step"));
    if (ImGui::Button("Step")) m_pendingSimStep = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(!simOn ? "Nothing to step - Simulate is off."
                          : "Advance the stage by exactly one fixed step (%.4f s):\n"
                            "the clip and the chains together, also while paused.", stepLen);
    ImGui::EndDisabled();
    Flow::SameLineIfFits(Flow::ButtonW("Reset"));
    if (ImGui::Button("Reset")) m_pendingFullReset = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reset Sim: the chains drop their state and re-initialise from\n"
                          "the current pose, the playhead goes to 0 and the sway / wind\n"
                          "phase restarts. The parameters and the subject are untouched\n"
                          "(Revert and Reload subject are separate).");

    // ---- time scale and step size (timeScale is not substeps)
    ImGui::BeginDisabled(!simOn);
    float ts = m_pendingTimeScale >= 0.f ? m_pendingTimeScale : m_clock.timeScale;
    ImGui::SetNextItemWidth(Flow::FitWidth(150.f, "Time scale"));
    if (ImGui::SliderFloat("Time scale", &ts, 0.1f, 2.f, "x%.2f"))
        m_pendingTimeScale = ts;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(!simOn
            ? "Simulate is off - the stage clock still runs at x%.2f; turn Simulate\n"
              "on to change it (the clip's own speed is in the transport below)."
            : "How many fixed steps a frame runs - clip and chains slow down\n"
              "together. It NEVER changes the size of a step, so the tuned spring\n"
              "stays the same spring. Not the same knob as Substeps (integration\n"
              "subdivision inside one step).", m_clock.timeScale);
    static const char* kDtItems[] = { "fixed 120 Hz", "fixed 60 Hz", "fixed 30 Hz",
                                      "follow editor" };
    static const float kDtValues[] = { 1.f / 120.f, 1.f / 60.f, 1.f / 30.f, 0.f };
    const float dtNow = m_pendingFixedDt >= 0.f ? m_pendingFixedDt : m_clock.fixedDt;
    int dtIdx = -1;
    for (int i = 0; i < 4; ++i)
        if (std::fabs(dtNow - kDtValues[i]) < 1e-6f) dtIdx = i;
    char dtLabel[48];
    if (dtIdx >= 0) std::snprintf(dtLabel, sizeof(dtLabel), "%s", kDtItems[dtIdx]);
    else            std::snprintf(dtLabel, sizeof(dtLabel), "fixed %.4f s", dtNow);
    ImGui::SetNextItemWidth(Flow::FitWidth(150.f, "dt"));
    if (ImGui::BeginCombo("dt", dtLabel))
    {
        for (int i = 0; i < 4; ++i)
            if (ImGui::Selectable(kDtItems[i], i == dtIdx)) m_pendingFixedDt = kDtValues[i];
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The stage clock's step. FIXED is the default (plan Q2): a spring\n"
                          "tuned at an unsteady editor rate is the same spring at 60 Hz.\n"
                          "A slow frame catches up at most %u steps; the rest is DROPPED.\n"
                          "'follow editor' steps once per frame by the real frame time -\n"
                          "anything above 1/30 s is clamped by the solver (Simulate and\n"
                          "PrepareSubsteps both take min(dt, 1/30)), and the clip is\n"
                          "clamped with it so both stay on one clock.",
                          ChainPreview::kMaxCatchupSteps);
    Flow::Checkbox("Reset sim on scrub", &m_resetOnScrub);   // panel-only state
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Dragging the playhead / stepping clip frames / jumping resets\n"
                          "the chains (integrating across a jump whips them). Untick to\n"
                          "let the chains ride the jump - a legitimate diagnostic.");

    // ---- status ----
    if (m_lastSimDt > 0.f && m_lastLaneSteps == 0u)
        Flow::ColoredWrapped(ImVec4(0.55f, 0.8f, 1.f, 1.f), "%s  dt %.4f s",
                             m_clock.paused ? "PAUSED - holding the simulated pose"
                                            : "HOLDING (no step this frame)",
                             m_lastSimDt);
    else if (m_lastSimDt > 0.f)
        Flow::ColoredWrapped(ImVec4(0.6f, 0.9f, 0.6f, 1.f), "SIMULATING  %u x %.4f s%s",
                             m_lastLaneSteps, m_lastSimDt,
                             m_clock.fixedDt > 0.f ? "" : " (follow editor)");
    else
        Flow::ColoredWrapped(kWarn, "%s",
                             !m_simulate ? "HELD (simulation off - the chains follow the clip)"
                             : reverse ? "HELD (reverse playback - the solver refuses a negative dt)"
                             : m_harnessLaneOff ? "HELD (-chainpreview_dt=0)"
                             : "HELD (dt 0)");
    Flow::DisabledWrapped("clock: %u step(s) this frame, %llu total, %llu dropped",
                          m_clock.lastSteps,
                          static_cast<unsigned long long>(m_clock.totalSteps),
                          static_cast<unsigned long long>(m_clock.droppedSteps));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("One clock drives the clip AND the chains: N fixed steps per\n"
                          "editor frame, 0..%u. A frame that owes more than %u steps\n"
                          "(a hitch) runs %u and DROPS the rest - clip and chains skip the\n"
                          "same time; it is never banked into a burst of fast-forward.\n"
                          "A frame with 0 steps HOLDS the simulated pose.",
                          ChainPreview::kMaxCatchupSteps, ChainPreview::kMaxCatchupSteps,
                          ChainPreview::kMaxCatchupSteps);
    Flow::DisabledWrapped("entities %u  substeps %u  passes %llu",
                          m_uiLaneStats.entities, m_uiLaneStats.substeps,
                          static_cast<unsigned long long>(m_uiLaneStats.passes));
    Flow::DisabledWrapped("solve %.3f ms (cpu %.3f)  sway t %.2f s  resets %llu",
                          m_uiLaneStats.serialMs + m_uiLaneStats.parallelMs,
                          m_uiLaneStats.cpuMs, m_uiSwayTime,
                          static_cast<unsigned long long>(m_simResets));
    Flow::ColoredWrapped(kWarn, "LOD: forced OFF while this panel is open");
    // "Capture current pose" lives in the rest-pose section of Parameters
    // (greyed while the stage simulates - ChainPhysicsSystem::IsSimulating
    // counts a holding lane).
}

// ==== The Parameters section (the shared tree on m_staged) and the overlay ====
void ChainPhysicsPreviewWindow::DrawStagedParams(Renderer* renderer,
                                                 const ChainParamTreeFn& drawParamTree)
{
    namespace Flow = EditorUI::Flow;
    if (m_noChain && m_stageRoot != NullEntity)
    {
        Flow::Wrapped("The subject has a skeleton but no ChainPhysicsComponent.");
        if (ImGui::Button("Auto-detect from bone names"))
            m_pendingAutoDetect = true;
        return;
    }
    if (!m_stagedValid || m_stageRoot == NullEntity)
    {
        ImGui::TextDisabled("(no subject)");
        return;
    }
    // ---- Apply, prominent and naming its target. There is no continuous
    // auto-apply: a scene entity would get two writers (m_staged and the global
    // undo / Inspector / gizmo) and every change would be recorded twice (the
    // panel ring and a global macro).
    DrawParamsApplyRow();
    // ---- panel undo: the staged history, not the scene's ----
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::SmallButton("Undo")) ++m_pendingUndo;
    ImGui::EndDisabled();
    Flow::SameLineIfFits(Flow::ButtonW("Redo"));
    ImGui::BeginDisabled(m_redo.empty());
    if (ImGui::SmallButton("Redo")) ++m_pendingRedo;
    ImGui::EndDisabled();
    {
        char hist[64];
        std::snprintf(hist, sizeof(hist), "history %u/%u  redo %u",
                      static_cast<unsigned>(m_undo.size()), static_cast<unsigned>(kUndoDepth),
                      static_cast<unsigned>(m_redo.size()));
        Flow::SameLineIfFits(Flow::TextW(hist));
        ImGui::TextDisabled("%s", hist);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ctrl+Z / Ctrl+Y while this panel is focused step THESE staged\n"
                          "values - never the scene's undo stack. A structural step\n"
                          "re-initialises the chains. Apply -> Scene entity is what pushes\n"
                          "one scene undo entry.");
    if (ImGui::GetTime() < m_toastUntil)
        Flow::ColoredWrapped(ImVec4(1.f, 0.75f, 0.3f, 1.f), "chains re-initialised (%s)",
                             m_lastRebuildWhy);
    // Why the last ring drag did not write (full / mirrored parent frame);
    // cleared by the next accepted one.
    if (!m_poseStatus.empty())
        Flow::ColoredWrapped(ImVec4(1.f, 0.55f, 0.35f, 1.f), "Pose gizmo: %s", m_poseStatus.c_str());
    Flow::DisabledWrapped("Apply target: %s   rebuilds %llu", ApplyDestName(m_applyDest),
                          static_cast<unsigned long long>(m_rebuilds));
    // Preset [v] [Load] [Save as...] [Delete], above the parameter tree.
    DrawPresetUi();
    ImGui::Separator();
    if (!DrawStagedTree(renderer, drawParamTree))
        ImGui::TextDisabled("(parameter tree unavailable)");
}

// The chain parameter tree (EditorLayer::DrawChainParamTree) on PANEL state:
// the live component is written only by SyncStagedToLive.
bool ChainPhysicsPreviewWindow::DrawStagedTree(Renderer* renderer,
                                               const ChainParamTreeFn& drawParamTree)
{
    if (!drawParamTree || !renderer ||
        m_stageSkelAsset >= renderer->GetSkeletonRegistry().Count())
        return false;
    ChainParamHooks hooks = StagedHooks();
    // The tree fills the resizable column.
    hooks.labelColumn = TreeLabelColumn();
    {
        const std::uint32_t forced = HarnessSectionMask();
        hooks.expandAll = forced != ~0u && (forced & kSecTree) != 0;
    }
    drawParamTree(m_staged, renderer->GetSkeletonRegistry().Get(m_stageSkelAsset), hooks);
    return true;
}

// The Play-mode Parameters body. The stage is suspended; EditorLayer's
// functor edits a scene character's live component (transient - Stop's
// AdoptState reverts it) and offers "Keep these values".
void ChainPhysicsPreviewWindow::DrawLiveParams(const ChainLiveEditFn& drawLive)
{
    if (!drawLive)
    {
        ImGui::TextDisabled("(live editing unavailable)");
        return;
    }
    drawLive();
}

// ==== The display mode ====
// What the picture shows: Both (the default), Overlay only / Chain only /
// Colliders only (every stage mesh hidden, the sky stays; Chain and Colliders
// also narrow the overlay to the chain or the capsules), Mesh only (no overlay
// primitive). The UI half only records the request (m_display); PPA's
// TickDisplay is the one writer of the stage's visibility.

const char* ChainPhysicsPreviewWindow::DisplayName(DisplayMode m)
{
    return m == DisplayMode::OverlayOnly ? "overlay" : m == DisplayMode::MeshOnly ? "mesh" :
           m == DisplayMode::ChainOnly ? "chain" : m == DisplayMode::CollidersOnly ? "colliders" : "both";
}

bool ChainPhysicsPreviewWindow::HarnessAddDisplay(DisplayMode m, std::uint64_t at)
{
    if (static_cast<int>(m) < 0 || static_cast<int>(m) > 4) return false;
    m_harnessDisplay.emplace_back(m, at);
    std::stable_sort(m_harnessDisplay.begin(), m_harnessDisplay.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });
    return true;
}

void ChainPhysicsPreviewWindow::RequestDisplay(DisplayMode m, const char* via)
{
    if (m == m_display) return;
    m_display    = m;
    m_displayVia = via ? via : "";
    // imgui.ini (inert under -unattended: io.IniFilename is null there)
    if (ImGui::GetCurrentContext()) ImGui::MarkIniSettingsDirty();
}

void ChainPhysicsPreviewWindow::DrawDisplayOptions()
{
    ImGui::SeparatorText("Display");
    static const DisplayMode kModes[] = { DisplayMode::Both, DisplayMode::OverlayOnly,
        DisplayMode::MeshOnly, DisplayMode::ChainOnly, DisplayMode::CollidersOnly };
    for (DisplayMode mode : kModes)
    {
        bool selected = m_display == mode;
        if (ImGui::Checkbox(DisplayLabel(mode), &selected) && selected)
            RequestDisplay(mode, "ui");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kDisplayTip);
    if (m_display == DisplayMode::ChainOnly || m_display == DisplayMode::CollidersOnly)
        ImGui::TextWrapped("This view isolates its named overlay. Both and Overlay only use the selections below.");
}
std::uint32_t ChainPhysicsPreviewWindow::CountStageHidden(const World& world,
                                                          std::uint32_t* ents) const
{
    std::uint32_t hidden = 0, n = 0;
    auto one = [&](Entity e)
    {
        const VisibilityComponent* v = world.GetComponent<VisibilityComponent>(e);
        if (!v) return;
        ++n;
        if ((v->viewMask & ViewBit::SecondaryView) == 0) ++hidden;
    };
    for (const EntityHandle& h : m_stageHandles)
        if (world.IsHandleValid(h)) one(h.entity);
    if (m_floorEnt != NullEntity && world.IsHandleValid(m_floorH)) one(m_floorEnt);
    if (ents) *ents = n;
    return hidden;
}

void ChainPhysicsPreviewWindow::TickDisplay(const ChainPreviewContext& ctx)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -chainpreview_display: the scheduled switches, through the UI's own request.
    while (m_harnessDisplayNext < m_harnessDisplay.size() &&
           m_tick >= m_harnessDisplay[m_harnessDisplayNext].second)
    {
        RequestDisplay(m_harnessDisplay[m_harnessDisplayNext].first, "harness");
        ++m_harnessDisplayNext;
    }
#endif
    World& world = *ctx.world;
    // ---- the switch edge (whoever asked: the Show section, the corner badge,
    // V, the harness). The FIRST switch records what "Both" drew before it.
    if (!m_displayLoggedValid)
    {
        m_displayLogged      = m_display;
        m_displayLoggedValid = true;
    }
    if (m_display != m_displayLogged)
    {
        if (!m_displayFirstValid)
        {
            m_displayFirstValid = true;
            m_displayFirstDraws = m_viewId != ~0u ? ctx.renderer->GetPreviewWorldCullDraws(m_viewId) : 0u;
            m_displayFirstEdit  = m_showEdit;
            m_displayFirstSim   = m_showSim;
        }
        ++m_displaySwitches;
        LOG_INFO("[CHAINPREVIEW] display switch %u: from=%s to=%s via=%s applied=%d frame=%llu",
                 m_displaySwitches, DisplayName(m_displayLogged), DisplayName(m_display),
                 m_displayVia, m_harnessDisplayOff ? 0 : 1,
                 static_cast<unsigned long long>(m_tick));
        m_displayLogged   = m_display;
        m_displaySettleAt = m_tick + kDisplaySettle;
    }
    // ---- the stage's secondary-view bit: idempotent, every frame, O(stage) ----
    // Every frame rather than on the edge, so a respawn (a subject change, a
    // world load, Play -> Stop), the floor plane appearing later (TickScene)
    // and a stage entity whose VisibilityComponent was rewritten all honour
    // the current mode with no extra bookkeeping. Only ViewBit::SecondaryView
    // moves; MainCamera / shadow / probe bits are the spawn policy's and are
    // never touched here (the main viewport cannot see the stage either way).
    // A write that does not change the value is skipped, so Both leaves the
    // spawn policy's bits exactly as they are. Teardown needs no restore: the
    // stage's entities are destroyed with it, and the next spawn starts from the
    // spawn policy (bit set) before this runs in the same PPA.
    const DisplayMode display = EffectiveDisplay();
    const bool hide = display == DisplayMode::OverlayOnly || display == DisplayMode::ChainOnly ||
                      display == DisplayMode::CollidersOnly;
    auto apply = [hide, &world](Entity e)
    {
        VisibilityComponent* v = world.GetComponent<VisibilityComponent>(e);
        if (!v) return;
        const std::uint32_t want = hide ? (v->viewMask & ~ViewBit::SecondaryView)
                                        : (v->viewMask | ViewBit::SecondaryView);
        if (want != v->viewMask) v->viewMask = want;
    };
    for (const EntityHandle& h : m_stageHandles)
        if (world.IsHandleValid(h)) apply(h.entity);
    if (m_floorEnt != NullEntity && world.IsHandleValid(m_floorH)) apply(m_floorEnt);

    // A RESPAWN (a subject reload, a world load, Play -> Stop) must honour
    // the mode: the loop above already re-applied it this frame (SpawnStage
    // ran earlier in this PPA); the settled line is the evidence.
    if (m_spawnCount != m_displaySpawnSeen)
    {
        if (m_displaySpawnSeen != 0)
        {
            ++m_displayRespawns;
            m_displayRespawnSettleAt = m_tick + kDisplaySettle;
        }
        m_displaySpawnSeen = m_spawnCount;
    }
    char head[48];
    if (m_displaySettleAt != 0 && m_tick >= m_displaySettleAt)
    {
        m_displaySettleAt = 0;
        std::snprintf(head, sizeof(head), "display %u:", m_displaySwitches);
        LogDisplay(ctx.renderer, &world, "settled", head);
    }
    if (m_displayRespawnSettleAt != 0 && m_tick >= m_displayRespawnSettleAt)
    {
        m_displayRespawnSettleAt = 0;
        std::snprintf(head, sizeof(head), "display respawn %u:", m_displayRespawns);
        LogDisplay(ctx.renderer, &world, "respawn", head);
    }
}

void ChainPhysicsPreviewWindow::LogDisplay(Renderer* renderer, const World* world,
                                           const char* when, const char* head) const
{
    std::uint32_t ents = 0;
    const std::uint32_t hidden = world ? CountStageHidden(*world, &ents) : 0u;
    const std::uint32_t draws  = (renderer && m_viewId != ~0u)
                               ? renderer->GetPreviewWorldCullDraws(m_viewId) : m_displayLastDraws;
    const std::uint32_t cands  = (renderer && m_viewId != ~0u)
                               ? renderer->GetPreviewWorldCullCandidates(m_viewId) : 0u;
    m_displayLastDraws = draws;
    // name=number pairs (logLineFields); mode= is the name, modeId= its number.
    LOG_INFO("[CHAINPREVIEW] %s mode=%s modeId=%d effectiveId=%d stageHidden=%u stageEnts=%u "
             "floor=%d worldDraws=%u worldCandidates=%u firstDraws=%u overlayMask=%u "
             "editMask=%u simMask=%u firstEditMask=%u firstSimMask=%u overlayBones=%u "
             "overlayLinks=%u overlayCapsules=%u overlayPrims=%u ghostParticles=%u "
             "poseDots=%u statsLines=%u displayOff=%d switches=%u frame=%llu when=%s",
             head ? head : "display:", DisplayName(m_display), static_cast<int>(m_display),
             static_cast<int>(EffectiveDisplay()), hidden, ents,
             m_floorEnt != NullEntity ? 1 : 0, draws, cands, m_displayFirstDraws,
             ActiveOverlayMask(), m_showEdit, m_showSim, m_displayFirstEdit, m_displayFirstSim,
             m_overlayStats.bones, m_overlayStats.links, m_overlayStats.capsules,
             m_overlayStats.prims, m_overlayStats.ghostParticles, m_poseDotsDrawn,
             m_overlayStats.statsLines, m_harnessDisplayOff ? 1 : 0, m_displaySwitches,
             static_cast<unsigned long long>(m_tick), when ? when : "");
}

void ChainPhysicsPreviewWindow::DrawShowUi()
{
    namespace Flow = EditorUI::Flow;
    // A bounded popup scrolls instead of expanding the parameter column.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Show");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - 48.f, 100.f, 260.f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(280.f, 0.f), ImVec2(420.f, 440.f));
#if defined(WITH_HARNESS) && WITH_HARNESS
    // Open the real dropdown for a UI capture without OS input.
    static const int s_showAt = Sys::CommandLine::Get().GetInt("-chainpreview_show_menu_at", 0);
    if (s_showAt > 0 && m_tick == static_cast<std::uint64_t>(s_showAt))
    {
        const ImGuiID comboId = ImGui::GetID("##cpv_show");
        ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, comboId), ImGuiPopupFlags_None);
    }
#endif
    if (!ImGui::BeginCombo("##cpv_show", DisplayLabel(m_display))) return;
    DrawDisplayOptions();
    auto maskRow = [](const char* label, std::uint32_t& mask)
    {
        ImGui::SeparatorText(label);
        ImGui::PushID(label);
        for (std::uint32_t i = 0; i < ChainOverlay::kBitCount; ++i)
        {
            bool on = (mask & (1u << i)) != 0;
            if (ImGui::Checkbox(ChainOverlay::BitName(i), &on))
                mask = on ? (mask | (1u << i)) : (mask & ~(1u << i));
        }
        ImGui::PopID();
    };
    maskRow("While editing (simulation off):", m_showEdit);
    maskRow("While simulating:", m_showSim);
    ImGui::SeparatorText("Overlay options");
    ImGui::Checkbox("Statistics", &m_showStats);
    ImGui::Checkbox("Depth fade", &m_depthFade);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The overlay has no depth test (a preview wire pass is M9):\n"
                          "primitives are drawn far to near and fade with distance.");
    // the limit-cone group (-1 = the first group that has a limit)
    std::string preview = "auto";
    if (m_overlayFocusGroup >= 0 && m_stagedValid &&
        m_overlayFocusGroup < static_cast<int>(m_staged.groups.size()))
        preview = m_staged.groups[static_cast<std::size_t>(m_overlayFocusGroup)].name[0]
                ? m_staged.groups[static_cast<std::size_t>(m_overlayFocusGroup)].name : "(chain)";
    ImGui::SetNextItemWidth(Flow::FitWidth(160.f, "Cone group"));
    if (ImGui::BeginCombo("Cone group", preview.c_str()))
    {
        if (ImGui::Selectable("auto (first limited group)", m_overlayFocusGroup < 0))
            m_overlayFocusGroup = -1;
        if (m_stagedValid)
            for (int gi = 0; gi < static_cast<int>(m_staged.groups.size()); ++gi)
            {
                char lbl[96];
                std::snprintf(lbl, sizeof(lbl), "%s##cg%d",
                              m_staged.groups[static_cast<std::size_t>(gi)].name[0]
                                  ? m_staged.groups[static_cast<std::size_t>(gi)].name : "(chain)",
                              gi);
                if (ImGui::Selectable(lbl, m_overlayFocusGroup == gi)) m_overlayFocusGroup = gi;
            }
        ImGui::EndCombo();
    }
    Flow::DisabledWrapped("bones %u  links %u  caps %u  cone %u  off-screen %u",
                          m_overlayStats.bones, m_overlayStats.links, m_overlayStats.capsules,
                          m_overlayStats.coneSegs, m_overlayStats.offscreenBones);
    ImGui::EndCombo();
}

void ChainPhysicsPreviewWindow::DrawOverlay(Renderer* renderer, const ImVec2& origin,
                                            const ImVec2& size)
{
    (void)renderer;
    // The stats box needs no snapshot: a stage whose overlay is off still shows
    // its solve cost.
    const ChainOverlay::StatsBox* statsBox = m_showStats ? &m_statsBox : nullptr;
    if (!m_overlaySnap.valid || !m_overlayCam.valid)
    {
        m_overlayStats = {};
        if (statsBox)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
            m_overlayStats = ChainOverlay::Draw(dl, origin, size, DirectX::XMMatrixIdentity(), 1.f,
                                                ChainOverlay::Snapshot{}, 0u, false, statsBox);
            dl->PopClipRect();
        }
        return;
    }
    // NDC -> pixels over the output the preview produced, which is also the
    // size the image is drawn at; the aspect comes from it.
    const float ow = m_overlayOutW ? static_cast<float>(m_overlayOutW) : size.x;
    const float oh = m_overlayOutH ? static_cast<float>(m_overlayOutH) : size.y;
    const DirectX::XMMATRIX vp = ChainOverlay::ViewProj(m_overlayCam, oh > 0.f ? ow / oh : 1.f,
                                                        m_harnessOverlayBadProj);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    m_overlayStats = ChainOverlay::Draw(dl, origin, size, vp, m_overlayCam.fovY, m_overlaySnap,
                                        ActiveOverlayMask(), m_depthFade, statsBox);
    dl->PopClipRect();
    ++m_overlayDrawn;

#if defined(WITH_HARNESS) && WITH_HARNESS
    // -chainpreview_overlay_shot: the overlay LAYER, rasterized from a private
    // draw list at the output size (see ChainOverlay::WriteLayerPng).
    for (OverlayShot& s : m_harnessOverlayShots)
    {
        if (s.done || m_tick < s.at) continue;
        s.done = true;
        std::string path = s.path;
        const bool absolute = path.size() > 1 &&
                              (path[1] == ':' || path[0] == '/' || path[0] == '\\');
        if (!absolute) path = Sys::DiagnosticsDir() + "/" + path;
        const std::uint32_t w = m_overlayOutW ? m_overlayOutW : static_cast<std::uint32_t>(size.x);
        const std::uint32_t h = m_overlayOutH ? m_overlayOutH : static_cast<std::uint32_t>(size.y);
        ChainOverlay::Stats st{};
        std::uint32_t tris = 0;
        std::string err;
        const long long lit = ChainOverlay::WriteLayerPng(path, w, h, vp, m_overlayCam.fovY,
                                                          m_overlaySnap, ActiveOverlayMask(),
                                                          m_depthFade, &st, &tris, &err, statsBox);
        ENGINE_CHECK(lit >= 0, "-chainpreview_overlay_shot '%s' failed: %s", path.c_str(),
                     err.c_str());
        // The shot line also carries the solver-state counts, the stats box's line
        // count (statsLines: 0 = the box was not drawn into the layer) and the
        // ghost's and the solo's counts.
        LOG_INFO("[CHAINPREVIEW] overlay shot: file='%s' w=%u h=%u mask=%u bones=%u links=%u "
                 "capsules=%u coneSegs=%u restPosed=%u offscreenBones=%u tris=%u litPixels=%lld "
                 "written=%d particles=%u constraints=%u limitMarks=%u ownerLines=%u "
                 "lodMarks=%u interCaps=%u statsLines=%u ghostParticles=%u ghostLinks=%u "
                 "frozenMarks=%u frame=%llu", path.c_str(), w, h,
                 ActiveOverlayMask(), st.bones,
                 st.links, st.capsules, st.coneSegs, st.restPosed, st.offscreenBones, tris,
                 lit < 0 ? 0ll : lit, lit >= 0 ? 1 : 0, st.particles, st.constraints,
                 st.limitMarks, st.ownerLines, st.lodMarks, st.interCaps, st.statsLines,
                 st.ghostParticles, st.ghostLinks, st.frozenMarks,
                 static_cast<unsigned long long>(m_tick));
        // With the A/B ghost in the layer, the same frame is also written
        // without it (<stem>_noghost.png) - the imagePair that proves the
        // ghost puts pixels on screen at all.
        if (st.ghostParticles != 0 && (ActiveOverlayMask() & ChainOverlay::kGhost))
        {
            std::string ng = path;
            const std::size_t dot = ng.rfind('.');
            ng = (dot == std::string::npos ? ng : ng.substr(0, dot)) + "_noghost.png";
            ChainOverlay::Stats st2{};
            std::uint32_t tris2 = 0;
            std::string err2;
            const long long lit2 = ChainOverlay::WriteLayerPng(
                ng, w, h, vp, m_overlayCam.fovY, m_overlaySnap,
                ActiveOverlayMask() & ~ChainOverlay::kGhost, m_depthFade, &st2, &tris2, &err2,
                statsBox);
            ENGINE_CHECK(lit2 >= 0, "-chainpreview_overlay_shot '%s' failed: %s", ng.c_str(),
                         err2.c_str());
            LOG_INFO("[CHAINPREVIEW] overlay shot noghost: file='%s' litPixels=%lld "
                     "ghostParticles=%u written=%d frame=%llu", ng.c_str(),
                     lit2 < 0 ? 0ll : lit2, st2.ghostParticles, lit2 >= 0 ? 1 : 0,
                     static_cast<unsigned long long>(m_tick));
        }
    }
#endif
}

void ChainPhysicsPreviewWindow::TickOverlay(const ChainPreviewContext& ctx)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -chainpreview_overlay: the scheduled show masks (edit AND sim).
    while (m_harnessOverlayNext < m_harnessOverlay.size() &&
           m_tick >= m_harnessOverlay[m_harnessOverlayNext].second)
    {
        const std::uint32_t mask = m_harnessOverlay[m_harnessOverlayNext].first;
        ++m_harnessOverlayNext;
        m_showEdit = m_showSim = mask;
        LOG_INFO("[CHAINPREVIEW] overlay mask=%u from editor frame %llu (-chainpreview_overlay)",
                 mask, static_cast<unsigned long long>(m_tick));
    }
#endif
    if (m_viewId != ~0u)
    {
        std::uint32_t w = 0, h = 0;
        if (ctx.renderer->GetPreviewOutputSize(m_viewId, w, h) && w && h)
        { m_overlayOutW = w; m_overlayOutH = h; }
        else
        { m_overlayOutW = m_renderW; m_overlayOutH = m_renderH; }
    }
    bool shotPending = false;
    for (const OverlayShot& s : m_harnessOverlayShots) shotPending = shotPending || !s.done;
    // The snapshot is taken only when something will draw it (the overlay is
    // on, or a harness shot is due) - the panel costs nothing extra otherwise.
    if ((ActiveOverlayMask() != 0 || shotPending) && m_stagedValid && ctx.buildBones)
        ChainOverlay::BuildSnapshot(*ctx.renderer, *ctx.world, m_stageChain, m_staged,
                                    m_stageSet, m_overlayFocusGroup, ctx.buildBones,
                                    m_overlaySnap,
                                    // the solver's own state (TickStats)
                                    m_dbgValid ? &m_dbgSnap : nullptr,
                                    // the A/B ghost (TickAbReadout)
                                    (m_abDbgValid && m_abEnt != NullEntity) ? &m_abDbg : nullptr);
    else
        m_overlaySnap.valid = false;
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (!m_harnessSubject.empty() && (m_tick % 60u) == 0u) LogOverlay("periodic");
#endif
}

void ChainPhysicsPreviewWindow::LogOverlay(const char* when) const
{
    // name=number pairs only (logLineFields); the mask in DECIMAL for that reason.
    LOG_INFO("[CHAINPREVIEW] overlay: mask=%u editMask=%u simMask=%u bones=%u links=%u "
             "capsules=%u excludes=%u restPosed=%u coneSegs=%u offscreenBones=%u prims=%u "
             "snapBones=%u snapCapsules=%u focusGroup=%d outW=%u outH=%u drawn=%llu "
             "depthFade=%d badProj=%d when=%s",
             ActiveOverlayMask(), m_showEdit, m_showSim, m_overlayStats.bones,
             m_overlayStats.links, m_overlayStats.capsules, m_overlayStats.excludes,
             m_overlayStats.restPosed, m_overlayStats.coneSegs, m_overlayStats.offscreenBones,
             m_overlayStats.prims, static_cast<unsigned>(m_overlaySnap.bone.size()),
             static_cast<unsigned>(m_overlaySnap.capsules.size()), m_overlaySnap.focusGroup,
             m_overlayOutW, m_overlayOutH, static_cast<unsigned long long>(m_overlayDrawn),
             m_depthFade ? 1 : 0, m_harnessOverlayBadProj ? 1 : 0, when ? when : "");
}

// -chainpreview_dump=<file.json>: the stage's particles and two pose hashes,
// once, when the panel closes (or at shutdown). Everything is READ: the
// solver's particle copy (CopyParticleState), the AnimationSystem's LocalPose.
void ChainPhysicsPreviewWindow::WriteDump(Renderer& renderer, World& world, const char* when)
{
    if (m_harnessDump.empty() || m_dumpWritten) return;
    ChainPhysicsSystem* cps  = renderer.GetChainPhysicsSystem();
    AnimationSystem*    anim = renderer.GetAnimationSystem();
    if (!cps) return;
    m_dumpWritten = true;

    // A bare / relative name lands in the run's artifact directory - the
    // HarnessCaptureSystem rule for -screenshot / -dumpstate.
    std::string path = m_harnessDump;
    const bool absolute = path.size() > 1 &&
                          (path[1] == ':' || path[0] == '/' || path[0] == '\\');
    if (!absolute) path = Sys::DiagnosticsDir() + "/" + path;

    auto fnv = [](std::uint32_t h, const void* p, std::size_t n)
    {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
        return h;
    };
    std::uint32_t particleHash = 2166136261u, poseHash = 2166136261u;
    std::uint32_t entities = 0, particles = 0;
    std::uint32_t snapshots = 0, snapLinks = 0, snapIslands = 0, snapInterpen = 0;
    float maxOffset = 0.f;
    std::string json = "{\n  \"when\": \"" + std::string(when ? when : "") +
                       "\",\n  \"subject\": \"" + m_subjectPath + "\",\n";
    char buf[256];
    std::snprintf(buf, sizeof(buf), "  \"tick\": %llu,\n  \"passes\": %llu,\n"
                  "  \"dt\": %.6f,\n  \"entities\": [",
                  static_cast<unsigned long long>(m_tick),
                  static_cast<unsigned long long>(cps->GetPreviewLaneStats().passes),
                  m_lastSimDt);
    json += buf;
    std::vector<DirectX::XMFLOAT3> pos, rest;
    if (auto* cp = world.GetPool<ChainPhysicsComponent>())
        for (Entity e : cp->Entities())
        {
            if (!m_previewSet.count(e)) continue;
            const bool have = cps->CopyParticleState(e, &pos, &rest);
            std::uint32_t ph = 2166136261u;
            if (have && !pos.empty()) ph = fnv(ph, pos.data(), pos.size() * sizeof(pos[0]));
            std::uint32_t lh = 2166136261u;
            std::uint32_t bones = 0;
            if (anim)
                if (const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(e))
                    if (const AnimationSystem::LocalPose* lp = anim->GetLocalPose(e))
                    {
                        bones = sk->boneCount;
                        lh = fnv(lh, lp, static_cast<std::size_t>(bones) * sizeof(lp[0]));
                    }
            float off = 0.f;
            for (std::size_t i = 0; have && i < pos.size() && i < rest.size(); ++i)
            {
                const float dx = pos[i].x - rest[i].x, dy = pos[i].y - rest[i].y,
                            dz = pos[i].z - rest[i].z;
                off = std::max(off, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            particleHash = fnv(particleHash, &ph, sizeof(ph));
            poseHash     = fnv(poseHash, &lh, sizeof(lh));
            maxOffset    = std::max(maxOffset, off);
            particles   += have ? static_cast<std::uint32_t>(pos.size()) : 0u;
            std::snprintf(buf, sizeof(buf),
                          "%s\n    { \"entity\": %u, \"initialized\": %d, \"bones\": %u, "
                          "\"particleHash\": %u, \"poseHash\": %u, \"maxOffset\": %.6f, "
                          "\"positions\": [",
                          entities ? "," : "", static_cast<unsigned>(e), have ? 1 : 0,
                          bones, ph, lh, off);
            json += buf;
            for (std::size_t i = 0; have && i < pos.size(); ++i)
            {
                std::snprintf(buf, sizeof(buf), "%s[%.6f,%.6f,%.6f]", i ? "," : "",
                              pos[i].x, pos[i].y, pos[i].z);
                json += buf;
            }
            json += "]";
            // The full solver snapshot (FillDebugSnapshot - skeleton space), for a
            // same-build regression sentinel: positions compare with a tolerance,
            // the topology (links / buckets / islands / ownership) exactly.
            ChainPhysicsSystem::DebugSnapshot ds;
            if (cps->FillDebugSnapshot(e, ds, world.GetComponent<ChainPhysicsComponent>(e)))
            {
                auto u32List = [&](const char* key, const auto& v)
                {
                    json += ",\n      \"";
                    json += key;
                    json += "\": [";
                    for (std::size_t i = 0; i < v.size(); ++i)
                    {
                        std::snprintf(buf, sizeof(buf), "%s%u", i ? "," : "",
                                      static_cast<unsigned>(v[i]));
                        json += buf;
                    }
                    json += "]";
                };
                std::snprintf(buf, sizeof(buf),
                              ",\n      \"snapshot\": { \"strands\": %u, \"islands\": %u, "
                              "\"springBones\": %u, \"groupsSimulated\": %u, \"lodTier\": %d, "
                              "\"ownCapsules\": %u, \"interpenetratingCapsules\": %u, "
                              "\"interpenetratingPairs\": %u, \"skippedCollideGroups\": %u, "
                              "\"limitOnCone\": %u, \"limitBeyond\": %u",
                              ds.strands, ds.islands, ds.springBones, ds.groupsSimulated,
                              ds.lodTier, ds.ownCapsules, ds.interpenetratingCapsules,
                              ds.interpenetratingPairs, ds.skippedCollideGroups,
                              ds.limitOnCone, ds.limitBeyond);
                json += buf;
                json += ",\n      \"particleRadius\": [";
                for (std::size_t i = 0; i < ds.particleRadius.size(); ++i)
                {
                    std::snprintf(buf, sizeof(buf), "%s%.6f", i ? "," : "", ds.particleRadius[i]);
                    json += buf;
                }
                json += "]";
                u32List("particleBone", ds.particleBone);
                u32List("particleWritesBone", ds.particleWritesBone);
                u32List("particleIsland", ds.particleIsland);
                u32List("particleGroup", ds.particleGroup);
                u32List("particlePinned", ds.particlePinned);
                u32List("particleLimitViolated", ds.particleLimitViolated);
                json += ",\n      \"links\": [";
                for (std::size_t i = 0; i < ds.links.size(); ++i)
                {
                    std::snprintf(buf, sizeof(buf), "%s[%u,%u]", i ? "," : "",
                                  ds.links[i].first, ds.links[i].second);
                    json += buf;
                }
                json += "]";
                u32List("linkBucket", ds.linkBucket);
                json += ",\n      \"capsules\": [";
                for (std::size_t i = 0; i < ds.capsules.size(); ++i)
                {
                    const auto& c = ds.capsules[i];
                    std::snprintf(buf, sizeof(buf),
                                  "%s{\"a\":[%.6f,%.6f,%.6f],\"b\":[%.6f,%.6f,%.6f],\"r\":%.6f,"
                                  "\"scene\":%u,\"interpenetrating\":%u}", i ? "," : "",
                                  c.a.x, c.a.y, c.a.z, c.b.x, c.b.y, c.b.z, c.radius,
                                  static_cast<unsigned>(c.scene),
                                  static_cast<unsigned>(c.interpenetrating));
                    json += buf;
                }
                json += "] }";
                snapLinks += static_cast<std::uint32_t>(ds.links.size());
                snapIslands += ds.islands;
                snapInterpen += ds.interpenetratingCapsules;
                ++snapshots;
            }
            json += " }";
            ++entities;
        }
    json += "\n  ]\n}\n";

    std::ofstream o(Sys::U8Path(path), std::ios::binary | std::ios::trunc);
    const bool ok = static_cast<bool>(o) && static_cast<bool>(o.write(json.data(),
                                               static_cast<std::streamsize>(json.size())));
    ENGINE_CHECK(ok, "-chainpreview_dump: cannot write '%s'", path.c_str());
    // The hashes are 32-bit and printed in DECIMAL so the harness's
    // logLineFields (name=<number>) can read and compare them.
    LOG_INFO("[CHAINPREVIEW] dump: file='%s' entities=%u particles=%u particleHash=%u "
             "poseHash=%u maxOffset=%.4f passes=%llu written=%d snapshots=%u links=%u "
             "islands=%u interpenetrating=%u when=%s",
             path.c_str(), entities, particles, particleHash, poseHash, maxOffset,
             static_cast<unsigned long long>(cps->GetPreviewLaneStats().passes),
             ok ? 1 : 0, snapshots, snapLinks, snapIslands, snapInterpen, when ? when : "");
}

// ==== The preview scene, the root-motion driver, the stress motions, FollowSceneEntity ====
// Everything here runs in ProcessPendingActions (the game half's serial
// window) except DrawSceneUi, which only edits panel state (m_staged included
// - the tree does the same).
namespace
{
    // One spelling for comparing prefab paths: project-relative, forward
    // slashes, lower case (PrefabSourcePath is written by several loaders).
    std::string NormAssetPath(const std::string& p)
    {
        std::string r = Sys::RelativizeAssetPath(p);
        if (r.empty()) r = p;
        for (char& c : r)
        {
            if (c == '\\') c = '/';
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return r;
    }

    constexpr float         kDegToRad          = 0.017453292519943295f;
    constexpr float         kFloorProxyHeights = 50.f;   // proxy radius, in subject heights
    // The yaw-sweep program (synthesized): +270 deg/s for 1 s, hard stop
    // 1.5 s, -270 deg/s for 1 s, stop 1.5 s. The teleport hop: one hop every 2 s.
    constexpr double        kSweepPeriod       = 5.0;
    constexpr float         kSweepRate         = 270.f;
    constexpr double        kHopPeriod         = 2.0;
}

float ChainPhysicsPreviewWindow::FloorSize() const
{
    // Large enough for the character, the drive's span and a margin, so the
    // floor reads as a floor from the default framing.
    const float h = SubjectHeight();
    return std::max(6.f * h, DriveSpan() + 4.f * h);
}

void ChainPhysicsPreviewWindow::TickShots(const ChainPreviewContext& ctx)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -chainpreview_shot: the panel's OWN view output, at any number of editor
    // frames - the scene pack needs four moments of one process and
    // -preview_screenshot has two slots. Captured HERE, at the frame boundary
    // (EditorLayer::CapturePreviewPNG's point and reasoning: the render half is
    // parked, the device frame is not open, and the last ToneMapPass#2 left
    // the output in SHADER_RESOURCE) - so frame f's shot is the picture frame
    // f-1 rendered.
    if (m_harnessShots.empty() || !ctx.gfx || !ctx.renderer) return;
    for (Shot& s : m_harnessShots)
    {
        if (s.done || m_tick < s.at) continue;
        const RHI::Texture* tex = (m_viewId != ~0u)
            ? ctx.renderer->GetPreviewOutputTexture(m_viewId) : nullptr;
        if (!tex)
        {
            // No picture yet (the view is still coming up): retry for a while,
            // then say so - the pack's artifactsExist turns it red.
            if (m_tick > s.at + 60)
            {
                s.done = true;
                LOG_ERROR("[CHAINPREVIEW] shot '%s' at editor frame %llu: the panel's "
                          "view has no output to capture", s.path.c_str(),
                          static_cast<unsigned long long>(s.at));
            }
            continue;
        }
        s.done = true;
        std::string path = s.path;
        const bool absolute = path.size() > 1 &&
                              (path[1] == ':' || path[0] == '/' || path[0] == '\\');
        if (!absolute) path = Sys::DiagnosticsDir() + "/" + path;
        const bool ok = ctx.gfx->CaptureTextureToPNG(*tex, RHI::ResourceState::SHADER_RESOURCE,
                                                     path.c_str());
        ENGINE_CHECK(ok, "-chainpreview_shot '%s' failed to write", path.c_str());
        LOG_INFO("[CHAINPREVIEW] shot: file='%s' written=%d frame=%llu clipT=%.4f "
                 "yawDeg=%.3f", path.c_str(), ok ? 1 : 0,
                 static_cast<unsigned long long>(m_tick), m_clipT, m_driveYawDeg);
    }
#else
    (void)ctx;
#endif
}

void ChainPhysicsPreviewWindow::TickResetWindows(const ChainPreviewContext& ctx)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -chainpreview_fullreset_at windows as an in-process A/B. When a harness
    // Reset is due this frame, the state the window leaves (the last frame's
    // solve: particles + post-chain pose) is captured before anything this frame
    // can reset it (a scale change, a teleport reset, the Reset itself), and
    // every window after the first is compared with the first: "[CHAINPREVIEW]
    // window <k>: ... equal=" - the replay of the same steps with one lever
    // changed. (-chainpreview_ab cannot place a lever at a known step; fullreset
    // windows sit on fixed editor frames.)
    if (m_harnessFullResetNext >= m_harnessFullResetAt.size() ||
        m_tick < m_harnessFullResetAt[m_harnessFullResetNext] ||
        m_stageRoot == NullEntity || !m_lanePushed || !ctx.renderer || !ctx.world)
        return;
    AbWindow w;
    ChainPhysicsSystem* cps  = ctx.renderer->GetChainPhysicsSystem();
    AnimationSystem*    anim = ctx.renderer->GetAnimationSystem();
    World& world = *ctx.world;
    auto fnv = [](std::uint32_t x, const void* p, std::size_t n)
    {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) x = (x ^ b[i]) * 16777619u;
        return x;
    };
    w.particleHash = w.poseHash = 2166136261u;
    std::vector<DirectX::XMFLOAT3> pos;
    if (auto* cp = world.GetPool<ChainPhysicsComponent>())
        for (Entity e : cp->Entities())
        {
            if (!m_previewSet.count(e)) continue;
            if (cps && cps->CopyParticleState(e, &pos, nullptr))
                w.particles.insert(w.particles.end(), pos.begin(), pos.end());
            if (anim)
                if (const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(e))
                    if (const AnimationSystem::LocalPose* lp = anim->GetLocalPose(e))
                        w.poseHash = fnv(w.poseHash, lp,
                                         static_cast<std::size_t>(sk->boneCount) * sizeof(lp[0]));
        }
    if (!w.particles.empty())
        w.particleHash = fnv(w.particleHash, w.particles.data(),
                             w.particles.size() * sizeof(w.particles[0]));
    // Capture 0 closes the segment from the spawn to the FIRST Reset - not a
    // replay window (it contains the spawn and the clip bind). Window 1 (first
    // Reset -> second) is the reference; windows 2.. are compared with it.
    const std::uint32_t k = m_resetWindows++;
    if (k == 0) return;
    if (k == 1)
    {
        m_resetWindowRef = std::move(w);
        LOG_INFO("[CHAINPREVIEW] window 1: frame=%llu particles=%u particleHash=%u poseHash=%u "
                 "(the reference)", static_cast<unsigned long long>(m_tick),
                 static_cast<unsigned>(m_resetWindowRef.particles.size()),
                 m_resetWindowRef.particleHash, m_resetWindowRef.poseHash);
        return;
    }
    const AbWindow& a = m_resetWindowRef;
    const bool same = a.particles.size() == w.particles.size() && !a.particles.empty();
    double maxD = same ? 0.0 : 1e9, sum = 0.0;
    for (std::size_t i = 0; same && i < a.particles.size(); ++i)
    {
        const double dx = static_cast<double>(a.particles[i].x) - w.particles[i].x;
        const double dy = static_cast<double>(a.particles[i].y) - w.particles[i].y;
        const double dz = static_cast<double>(a.particles[i].z) - w.particles[i].z;
        const double d  = std::sqrt(dx * dx + dy * dy + dz * dz);
        maxD = std::max(maxD, d);
        sum += d;
    }
    const bool equal = same && a.poseHash == w.poseHash &&
        std::memcmp(a.particles.data(), w.particles.data(),
                    a.particles.size() * sizeof(a.particles[0])) == 0;
    LOG_INFO("[CHAINPREVIEW] window %u: frame=%llu particles=%u particleHash=%u poseHash=%u "
             "refParticleHash=%u refPoseHash=%u maxDelta=%.6f meanDelta=%.6f equal=%d",
             k, static_cast<unsigned long long>(m_tick),
             static_cast<unsigned>(w.particles.size()), w.particleHash, w.poseHash,
             a.particleHash, a.poseHash, maxD,
             same ? sum / static_cast<double>(a.particles.size()) : 1e9, equal ? 1 : 0);
#else
    (void)ctx;
#endif
}

void ChainPhysicsPreviewWindow::TickSceneHarness(const ChainPreviewContext& ctx)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (m_stageRoot == NullEntity || !ctx.world) return;
    World& world = *ctx.world;
    if (!m_harnessStress.empty() && !m_harnessStressDone)
    {
        m_harnessStressDone = true;
        m_pendingStress = m_harnessStress == "idle" ? 0 : m_harnessStress == "walkrun" ? 1
                        : m_harnessStress == "yawsweep" ? 2 : 3;
    }
    if (!m_harnessFollow.empty() && !m_harnessFollowDone)
    {
        m_harnessFollowDone = true;
        // <name>[|<clip.ianim>]: the optional clip is a FIXTURE - in Edit mode
        // no scene character plays anything, so the harness gives the source a
        // clip (its own alias, queued the way the Animator does) before the
        // panel starts mirroring it.
        std::string name = m_harnessFollow, fixtureClip;
        const std::size_t bar = name.find('|');
        if (bar != std::string::npos) { fixtureClip = name.substr(bar + 1); name = name.substr(0, bar); }
        Entity hit = NullEntity;
        for (Entity e : world.GetEntities())
            if (world.IsAlive(e) && !m_stageSet.count(e) && world.GetName(e) == name)
            { hit = e; break; }
        if (hit != NullEntity && !fixtureClip.empty() && ctx.animClipSys)
        {
            const Entity a = Anim::ResolveAnimEntityOrSelf(world, hit);
            const bool reg = a != NullEntity &&
                ctx.animClipSys->SetAnimatorClip(world, a, "__cpv_follow_fixture", fixtureClip);
            const bool q = reg && ctx.animClipSys->QueueAnimatorPlay(world, a, "__cpv_follow_fixture",
                                                                     0.f, false);
            LOG_INFO("[CHAINPREVIEW] -chainpreview_follow fixture: source %u plays '%s' "
                     "(registered=%d queued=%d)", static_cast<unsigned>(a), fixtureClip.c_str(),
                     reg ? 1 : 0, q ? 1 : 0);
        }
        if (hit == NullEntity)
        {
            LOG_ERROR("[CHAINPREVIEW] -chainpreview_follow='%s' names no scene entity. "
                      "Refusing to run - the controller would stay on Rest pose under "
                      "the pack's name.", m_harnessFollow.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
        else
        {
            m_pendingFollowSel = true;
            m_pendingFollowEnt = hit;
        }
    }
    for (SceneEvent& ev : m_harnessScene)
    {
        if (ev.done || (ev.at != 0 && m_tick < ev.at)) continue;
        ev.done = true;
        switch (ev.kind)
        {
        case SceneEvent::Gravity:
            m_gravDown[0] = ev.v[0]; m_gravDown[1] = ev.v[1]; m_gravDown[2] = ev.v[2];
            m_gravScale = ev.v[3];
            m_noGravity = false;
            LOG_INFO("[CHAINPREVIEW] gravity: downX=%.3f downY=%.3f downZ=%.3f scale=%.3f "
                     "noGravity=0 frame=%llu (-chainpreview_gravity)", ev.v[0], ev.v[1], ev.v[2],
                     ev.v[3], static_cast<unsigned long long>(m_tick));
            break;
        case SceneEvent::NoGravity:
            m_noGravity = true;
            LOG_INFO("[CHAINPREVIEW] gravity: noGravity=1 frame=%llu (-chainpreview_nogravity)",
                     static_cast<unsigned long long>(m_tick));
            break;
        case SceneEvent::Wind:
        {
            const float dir[3] = { ev.v[0], ev.v[1], ev.v[2] };
            const std::uint32_t groups = ApplyWindToStaged(dir, ev.v[3], ev.v[4], ev.v[5],
                                                           m_windAlsoGroups);
            LOG_INFO("[CHAINPREVIEW] wind: dirX=%.3f dirY=%.3f dirZ=%.3f strength=%.4f "
                     "freq=%.3f turbulence=%.3f groupsSet=%u frame=%llu (-chainpreview_wind)",
                     dir[0], dir[1], dir[2], ev.v[3],
                     m_stagedValid ? m_staged.windFreq : 0.f,
                     m_stagedValid ? m_staged.windTurbulence : 0.f, groups,
                     static_cast<unsigned long long>(m_tick));
            break;
        }
        case SceneEvent::Drive:
            m_driveMove = ev.v[0];
            m_driveRot  = ev.v[1];
            LOG_INFO("[CHAINPREVIEW] drive set: move=%.4f rot=%.3f frame=%llu (-chainpreview_drive)",
                     m_driveMove, m_driveRot, static_cast<unsigned long long>(m_tick));
            break;
        case SceneEvent::Teleport:
            m_teleportDist  = ev.v[0] > 0.f ? ev.v[0] : 0.f;   // <= 0 = the auto distance
            m_teleportAngle = ev.v[1];
            m_teleportResetOnce = ev.v[2] > 0.5f;              // ":reset" on this one
            m_pendingTeleport = true;
            break;
        case SceneEvent::Scale:
            // the Scene section's Scale control, pressed by the harness
            m_pendingStageScale = ev.v[0];
            LOG_INFO("[CHAINPREVIEW] -chainpreview_scale_at: scale %.4f at editor frame %llu",
                     ev.v[0], static_cast<unsigned long long>(m_tick));
            break;
        case SceneEvent::FollowKill:
            // Harness only: the fallback's trigger. The source is a scene
            // entity and this destroys it - acceptable only because a harness
            // run never saves its scene.
            if (m_followSrc != NullEntity && world.IsHandleValid(m_followSrcH))
            {
                LOG_WARNING("[CHAINPREVIEW] -chainpreview_follow_kill_at: destroying the "
                            "follow source %u ('%s') at editor frame %llu",
                            static_cast<unsigned>(m_followSrc), m_followName.c_str(),
                            static_cast<unsigned long long>(m_tick));
                Hierarchy::DestroySubtree(world, m_followSrc);
            }
            break;
        }
    }
#else
    (void)ctx;
#endif
}

void ChainPhysicsPreviewWindow::ResolveStageScale(World& world, Entity root)
{
    // A raw .ipfb spawns at its authored scale (e.g. 1.0 for a ~19-unit
    // character) while scenes place the same prefab scaled (AIBench: 0.1). The
    // default is the scale the scene uses this prefab at - its first instance
    // (PrefabSourcePath, pool order) - and only then the authored one, so the
    // stage is the size the scene shows. The chain solver is invariant to it: it
    // simulates in the skeleton's own space (FK from the LocalPose, no entity
    // transform), and 0.1 vs 1.0 replay bit-identical particles
    // (chain_preview_scale). A harness value, the user's Scale control and a Use
    // Selected instance each win over that rule, in that order.
    m_scaleInstance = NullEntity;
    float authored[3] = { 1.f, 1.f, 1.f };
    if (const LocalTransform* lt = world.GetComponent<LocalTransform>(root))
    { authored[0] = lt->scale.x; authored[1] = lt->scale.y; authored[2] = lt->scale.z; }
    if (m_harnessScale > 0.f)
    {
        m_stageScale[0] = m_stageScale[1] = m_stageScale[2] = m_harnessScale;
        m_scaleSource = "harness";
    }
    else if (m_userScale)
        m_scaleSource = "user";                     // m_stageScale already holds it
    else if (m_stageScaleValid)
        m_scaleSource = "selected";                 // Use Selected's instance scale
    else
    {
        const std::string want = NormAssetPath(m_subjectPath);
        if (auto* pp = world.GetPool<PrefabSourcePath>())
        {
            auto& ents = pp->Entities();
            auto& data = pp->Data();
            for (std::size_t i = 0; i < ents.size(); ++i)
            {
                if (m_stageSet.count(ents[i]) || data[i].path.empty()) continue;
                if (NormAssetPath(data[i].path) != want) continue;
                const LocalTransform* lt = world.GetComponent<LocalTransform>(ents[i]);
                if (!lt) continue;
                m_stageScale[0] = lt->scale.x;
                m_stageScale[1] = lt->scale.y;
                m_stageScale[2] = lt->scale.z;
                m_scaleInstance = ents[i];
                break;
            }
        }
        if (m_scaleInstance != NullEntity)
            m_scaleSource = "scene instance";
        else
        {
            for (int k = 0; k < 3; ++k) m_stageScale[k] = authored[k];
            m_scaleSource = "authored";
        }
    }
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(m_stageScale[k]) || m_stageScale[k] <= 0.f) m_stageScale[k] = 1.f;
}

std::uint32_t ChainPhysicsPreviewWindow::ApplyWindToStaged(const float dir[3], float strength,
                                                           float freq, float turb, bool alsoGroups)
{
    // Wind is authored (it serializes and Apply writes it), so it goes to
    // m_staged and reaches the stage through the one writer (TickStagedParams).
    // A group with ovrEnabled replaces the global strength / freq with its own
    // ovrWindStrength / ovrWindFreq (authored characters' groups usually do, at
    // 0) - the global fields alone would move nothing there, hence alsoGroups.
    if (!m_stagedValid) return 0;
    m_staged.windDir      = { dir[0], dir[1], dir[2] };
    m_staged.windStrength = std::max(strength, 0.f);
    if (freq >= 0.f) m_staged.windFreq = freq;
    if (turb >= 0.f) m_staged.windTurbulence = turb;
    std::uint32_t n = 0;
    if (alsoGroups)
        for (ChainGroupDef& g : m_staged.groups)
        {
            if (!g.ovrEnabled) continue;
            g.ovrWindStrength = m_staged.windStrength;
            if (freq >= 0.f) g.ovrWindFreq = freq;
            ++n;
        }
    m_windGroupsSet = n;
    ++m_windApplies;
    return n;
}

void ChainPhysicsPreviewWindow::DestroyFloor(World& world)
{
    if (m_floorEnt != NullEntity && world.IsHandleValid(m_floorH))
        Hierarchy::DestroySubtree(world, m_floorEnt);
    m_floorEnt = NullEntity;
    m_floorH   = NullEntityHandle;
    m_floorProxyValid = false;
}

void ChainPhysicsPreviewWindow::EnsureFloor(const ChainPreviewContext& ctx)
{
    World& world = *ctx.world;
    // Default on for a user, off for a harness subject unless the pack asks
    // (-chainpreview_floor): a pack that does not ask keeps its floorless pictures.
    const bool want = m_stageRoot != NullEntity &&
                      (m_floorMode == 1 || (m_floorMode < 0 && m_harnessSubject.empty()));
    if (!want)
    {
        if (m_floorEnt != NullEntity) DestroyFloor(world);
        m_floorProxyValid = false;
        return;
    }
    if (m_floorEnt != NullEntity && !world.IsHandleValid(m_floorH))
    {
        // Deleted outside the panel: never touch the (recycled) id again.
        m_floorEnt = NullEntity;
        m_floorH   = NullEntityHandle;
    }
    const float y = kStageOriginY - m_floorGap;
    if (m_floorEnt == NullEntity)
    {
        if (m_floorRefused) return;
        // ---- the context-mesh budget ----
        // The floor is one static instance row and no skin job / bone / vertex
        // of the per-frame rings; refused and named if the scene + the stage +
        // it would pass the instance cap - never truncated.
        const Renderer::FrameStats& fs = ctx.renderer->GetFrameStats();
        const std::uint32_t sceneInst = fs.staticInstSlots + fs.dynamicInstances;
        const std::uint32_t capInst   = ctx.renderer->GetInstanceCapacity();
        if (static_cast<std::uint64_t>(sceneInst) + m_budgetInstances + 1u > capInst)
        {
            m_floorRefused = true;
            m_floorRefusedWhy = "instances (Renderer::m_maxInstances)";
            LOG_WARNING("[CHAINPREVIEW] context mesh: floor REFUSED - the scene (%u) + the "
                        "stage (%u) + the floor (1) exceed the per-frame budget for %s (%u)",
                        sceneInst, m_budgetInstances, m_floorRefusedWhy.c_str(), capInst);
            return;
        }
        m_floorEnt = MeshSpawner::Spawn(static_cast<int>(PrimitiveMeshType::Plane), world,
                                        DirectX::XMFLOAT3{ 0.f, y, 0.f });
        if (m_floorEnt == NullEntity) { m_floorRefused = true; m_floorRefusedWhy = "spawn failed"; return; }
        m_floorH = world.MakeHandle(m_floorEnt);
        world.SetName(m_floorEnt, "__ChainPreviewFloor");
        // The stage's pollution policy: never saved, never in the Outliner /
        // picking, never in the main view, the CSM or a probe.
        world.AddComponent<SceneTransientTag>(m_floorEnt, SceneTransientTag{});
        world.AddComponent<PreviewStageTag>(m_floorEnt, PreviewStageTag{ PreviewStageTag::kChainPreview });
        if (VisibilityComponent* v = world.GetComponent<VisibilityComponent>(m_floorEnt))
        {
            v->viewMask &= ~(ViewBit::MainCamera | ViewBit::ShadowAny | ViewBit::ReflectionProbe);
            v->viewMask |= ViewBit::SecondaryView;
            v->SetCastShadow(false);
            v->SetRenderInMainPass(true);   // filterList tests it BEFORE the view bit
        }
        ++m_floorSpawns;
        LOG_INFO("[CHAINPREVIEW] context mesh: floor=1 entity=%u instances=1 skinJobs=0 bones=0 "
                 "verts=0 sceneInstances=%u stageInstances=%u capInstances=%u spawns=%u",
                 static_cast<unsigned>(m_floorEnt), sceneInst, m_budgetInstances, capInst,
                 m_floorSpawns);
    }
    // Per frame, idempotent (TransformSystem skips unchanged bytes): the floor
    // is WORLD-fixed at the stage origin - the driven root moves over it.
    const float S = FloorSize();
    if (LocalTransform* lt = world.GetComponent<LocalTransform>(m_floorEnt))
    {
        lt->translation = { 0.f, y, 0.f };
        lt->rotation    = { 0.f, 0.f, 0.f, 1.f };
        lt->scale       = { S, 1.f, S };
    }
    // The solver collides with CAPSULES only, so "collide with the floor" is a
    // proxy: one very large capsule lying along the drive axis (Z) under the
    // plane, its radius pre-divided by the staged contactSkin (the solver's
    // contact radius is (radius + particle radius) x skin) so its top is AT
    // the floor; flat to within height/100 over one character height sideways.
    // Only once the stage has SETTLED (the framing accepted its joints at the
    // stage origin, and fixed the height the proxy is sized from): on the
    // spawn frames the carrier's transform still sits at the prefab's
    // authored place, and a proxy pushed then jumps ~4 km in skeleton space
    // and changes size when the height becomes known.
    m_floorProxyValid = m_floorCollide && m_focusValid && m_subjectHeight > 0.f;
    if (m_floorProxyValid)
    {
        const float reff = kFloorProxyHeights * SubjectHeight();
        const float skin = std::max(m_stagedValid ? m_staged.contactSkin : 1.015f, 1.f);
        m_floorProxyA[0] = 0.f; m_floorProxyA[1] = y - reff; m_floorProxyA[2] = -0.5f * S;
        m_floorProxyB[0] = 0.f; m_floorProxyB[1] = y - reff; m_floorProxyB[2] =  0.5f * S;
        m_floorProxyR    = reff / skin;
    }
}

void ChainPhysicsPreviewWindow::PushPreviewScene(ChainPhysicsSystem& cps)
{
    // Active only when it differs from the authored path, so the default scene
    // is the solver's authored scalar gravity, bit for bit.
    const bool gravOn = m_noGravity || m_gravScale != 1.f ||
                        m_gravDown[0] != 0.f || m_gravDown[1] != -1.f || m_gravDown[2] != 0.f;
    const DirectX::XMFLOAT3 down = m_noGravity ? DirectX::XMFLOAT3{ 0.f, 0.f, 0.f }
        : DirectX::XMFLOAT3{ m_gravDown[0] * m_gravScale, m_gravDown[1] * m_gravScale,
                             m_gravDown[2] * m_gravScale };
    cps.SetPreviewGravity(gravOn, down);
    if (m_floorProxyValid && m_floorEnt != NullEntity)
    {
        const DirectX::XMFLOAT3 a{ m_floorProxyA[0], m_floorProxyA[1], m_floorProxyA[2] };
        const DirectX::XMFLOAT3 b{ m_floorProxyB[0], m_floorProxyB[1], m_floorProxyB[2] };
        cps.SetPreviewSceneCapsules(&a, &b, &m_floorProxyR, 1u);
    }
    else
        cps.SetPreviewSceneCapsules(nullptr, nullptr, nullptr, 0u);
}

std::string ChainPhysicsPreviewWindow::ResolveStressClip(bool idle)
{
    // idle and walk-run are existing clips: 1) an alias the subject authored
    // whose name says so; 2) a scanned .ianim whose stem says so, preferring the
    // subject's own family (its stem's first token appears in the clip's path -
    // "Rossi" for Rossi_Base), a "loop" for walk-run, then the shortest stem.
    const std::vector<std::string> keys = idle ? std::vector<std::string>{ "idle" }
                                               : std::vector<std::string>{ "run", "walk" };
    for (const std::string& k : keys)
        for (const AuthoredClip& a : m_authoredClips)
            if (Lower(a.name).find(k) != std::string::npos) return a.path;
    if (!m_clipScanDone) ScanClips();
    std::string family = Lower(Stem(m_subjectPath));
    const std::size_t us = family.find('_');
    if (us != std::string::npos) family = family.substr(0, us);
    for (const std::string& k : keys)
    {
        const std::string* best = nullptr;
        int bestScore = -1;
        for (const std::string& p : m_clipList)
        {
            const std::string st = Lower(Stem(p));
            if (st.find(k) == std::string::npos) continue;
            const int score = (!family.empty() && Lower(p).find("/" + family + "/") != std::string::npos ? 2 : 0) +
                              (!idle && st.find("loop") != std::string::npos ? 1 : 0);
            if (!best || score > bestScore ||
                (score == bestScore && (Stem(p).size() < Stem(*best).size() ||
                                        (Stem(p).size() == Stem(*best).size() && p < *best))))
            { best = &p; bestScore = score; }
        }
        if (best) return *best;
    }
    return {};
}

void ChainPhysicsPreviewWindow::Teleport(float dist, float angleDeg, const char* why)
{
    // A one-shot relocation of the stage root: `dist` along the root's current
    // facing (alternating sign for the hop program) and `angleDeg` of yaw, in one
    // frame, written to the root's LocalTransform exactly as a game moves a
    // character. The chain solver simulates in the skeleton's own space (FK from
    // the LocalPose, no entity transform); an entity teleport - like every entity
    // translation / rotation the driver makes - reaches the chains only through
    // the rig's authored world-motion inertia, and a hop beyond its teleport
    // distance / angle is ignored there (bit-identical to no hop,
    // chain_preview_world_teleport). With inertia 0 it reaches them not at all
    // (chain_preview_teleport). "Reset chains on teleport" re-inits them from the
    // new pose instead.
    const float d = dist != 0.f ? dist : 0.5f * SubjectHeight();   // 0 = auto, < 0 = backwards
    const float yaw = m_driveYawDeg * kDegToRad;
    m_driveOffset[0] += d * std::sin(yaw);
    m_driveOffset[2] += d * std::cos(yaw);
    m_driveYawDeg = std::fmod(m_driveYawDeg + angleDeg, 360.f);
    ++m_teleports;
    const bool reset = m_teleportReset || m_teleportResetOnce;
    m_teleportResetOnce = false;
    if (reset) m_pendingSimReset = "teleport";
    LOG_INFO("[CHAINPREVIEW] teleport: n=%llu dist=%.4f angleDeg=%.2f offsetX=%.4f "
             "offsetZ=%.4f yawDeg=%.3f reset=%d frame=%llu why=%s",
             static_cast<unsigned long long>(m_teleports), d, angleDeg, m_driveOffset[0],
             m_driveOffset[2], m_driveYawDeg, reset ? 1 : 0,
             static_cast<unsigned long long>(m_tick), why);
}

void ChainPhysicsPreviewWindow::TickDriver(const ChainPreviewContext& ctx)
{
    if (m_stageRoot == NullEntity || !ctx.world) return;
    World& world = *ctx.world;
    const std::uint32_t n  = m_frameSteps;
    const float         dt = m_frameStepDt;

    // ---- the turntable: CAMERA yaw on the stage clock (pauses with it) ----
    if (m_turntable)
    {
        m_turntableYaw += static_cast<float>(n) * dt * m_turntableSpeed * kDegToRad;
        m_turntableYaw = std::fmod(m_turntableYaw, 6.2831853f);
    }

    // ---- the driver: the SAME n steps of dt as the clip and the chains ----
    if (!m_harnessDriveOff)
    {
        const float span = DriveSpan();
        for (std::uint32_t i = 0; i < n; ++i)
        {
            float rot = m_driveRot;
            if (m_driveProgram == DriveProgram::YawSweep)
            {
                const double t = std::fmod(m_driveProgT, kSweepPeriod);
                rot = t < 1.0 ? kSweepRate : t < 2.5 ? 0.f : t < 3.5 ? -kSweepRate : 0.f;
            }
            else if (m_driveProgram == DriveProgram::TeleportHop)
            {
                // a hop each time the program clock crosses a period boundary
                const double t0 = m_driveProgT, t1 = m_driveProgT + dt;
                if (std::floor(t1 / kHopPeriod) > std::floor(t0 / kHopPeriod))
                {
                    // alternating hops (out, back, out, ...) so the program
                    // stays near the origin for as long as it runs
                    const bool back = (static_cast<long long>(std::floor(t1 / kHopPeriod)) & 1) == 0;
                    const float d = m_teleportDist > 0.f ? m_teleportDist : 0.5f * SubjectHeight();
                    Teleport(back ? -d : d, back ? -m_teleportAngle : m_teleportAngle,
                             "teleport hop");
                }
            }
            m_driveYawDeg   = std::fmod(m_driveYawDeg + rot * dt, 360.f);
            m_driveRotTotal += static_cast<double>(rot) * static_cast<double>(dt);
            if (m_driveMove != 0.f && span > 0.f)
            {
                // ping-pong along world Z inside +-span/2 (a straight line
                // forever would carry the stage off its float precision)
                float z = m_driveOffset[2] + m_driveDir * m_driveMove * dt;
                const float h = 0.5f * span;
                if (z > h)  { z = 2.f * h - z;  m_driveDir = -m_driveDir; }
                if (z < -h) { z = -2.f * h - z; m_driveDir = -m_driveDir; }
                m_driveOffset[2] = std::clamp(z, -h, h);
            }
            m_driveProgT += dt;
            ++m_driveSteps;
        }
        if (m_pendingTeleport)
        {
            m_pendingTeleport = false;
            Teleport(m_teleportDist, m_teleportAngle, "teleport");
        }
    }
    else
        m_pendingTeleport = false;

    // ---- the write of the stage root's LocalTransform ----
    // Every frame, idempotent (TransformSystem's memcmp skip): the stage origin +
    // the drive offset, the prefab's authored rotation then the drive's world
    // yaw, the stage scale. Before this frame's TransformSystem, so the chains'
    // rest targets see the move this frame.
    if (!world.IsHandleValid(m_stageRootH)) return;
    if (LocalTransform* lt = world.GetComponent<LocalTransform>(m_stageRoot))
    {
        using namespace DirectX;
        lt->translation = { m_driveOffset[0], kStageOriginY + m_driveOffset[1], m_driveOffset[2] };
        const XMVECTOR base = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(m_rootBaseRot));
        const XMVECTOR yawQ = XMQuaternionRotationAxis(XMVectorSet(0.f, 1.f, 0.f, 0.f),
                                                       m_driveYawDeg * kDegToRad);
        XMFLOAT4 q;
        XMStoreFloat4(&q, XMQuaternionNormalize(XMQuaternionMultiply(base, yawQ)));
        // Bit-stable when nothing drives: the authored rotation, untouched.
        if (m_driveYawDeg == 0.f)
            q = { m_rootBaseRot[0], m_rootBaseRot[1], m_rootBaseRot[2], m_rootBaseRot[3] };
        lt->rotation = q;
        lt->scale    = { m_stageScale[0], m_stageScale[1], m_stageScale[2] };
    }
}

void ChainPhysicsPreviewWindow::FollowFallback(const char* why)
{
    const bool wasFollowing = m_controller == Controller::FollowSceneEntity;
    if (wasFollowing) m_controller = Controller::Clip;
    if (wasFollowing || m_followSrc != NullEntity)
    {
        ++m_followFallbacks;
        LOG_INFO("[CHAINPREVIEW] follow: source %u ('%s') %s - the controller falls back "
                 "to Clip (clip='%s' t=%.3f fallbacks=%llu)",
                 static_cast<unsigned>(m_followSrc), m_followName.c_str(), why,
                 Stem(m_clipPath).c_str(), m_clipT,
                 static_cast<unsigned long long>(m_followFallbacks));
    }
    m_followSrc    = NullEntity;
    m_followSrcH   = NullEntityHandle;
    m_followCulled = false;
    m_followStatus.clear();
    m_followLastT  = -1.0;
}

void ChainPhysicsPreviewWindow::TickFollow(const ChainPreviewContext& ctx)
{
    if (m_controller != Controller::FollowSceneEntity || !ctx.world) return;
    World& world = *ctx.world;
    // A destroyed source (or one a world clear took - the epoch block handles
    // that before this) falls back to Clip, loudly.
    if (m_followSrc == NullEntity || !world.IsHandleValid(m_followSrcH))
    {
        FollowFallback("was destroyed");
        return;
    }
    ++m_followFrames;
    // The source is not pinned: the panel never changes its behaviour. An
    // anim-culled source has no pose this frame (poseByteOffset == ~0u,
    // LocalToWorldSystem's sentinel): mirror the last known clip + time instead
    // of pinning it.
    const Entity skelEnt = world.HasComponent<SkeletonComponent>(m_followSrc)
                         ? m_followSrc : Anim::ResolveAnimEntityOrSelf(world, m_followSrc);
    const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(skelEnt);
    m_followCulled = !sk || sk->poseByteOffset == ~0u;
    char buf[256];
    if (m_followCulled)
    {
        ++m_followCulledFrames;
        std::snprintf(buf, sizeof(buf), "source culled - mirroring last known state ('%s' t=%.2f)",
                      Stem(m_clipPath).c_str(), m_clipT);
        m_followStatus = buf;
        return;
    }
    const AnimatorTransitionComponent* tr = world.GetComponent<AnimatorTransitionComponent>(m_followSrc);
    const AnimationSetComponent*       set = world.GetComponent<AnimationSetComponent>(m_followSrc);
    const AnimationComponent*          an  = world.GetComponent<AnimationComponent>(m_followSrc);
    const AnimationClipReference* ref = (tr && set && !tr->currentClipName.empty())
                                      ? set->Find(tr->currentClipName) : nullptr;
    if (!ref || ref->path.empty() || !an)
    {
        std::snprintf(buf, sizeof(buf), "'%s' plays no clip - holding the last one",
                      m_followName.c_str());
        m_followStatus = buf;
        return;
    }
    if (Sys::RelativizeAssetPath(ref->path) != m_clipPath || ref->sourceClip != m_clipSource)
    {
        SelectClip(ctx, ref->path, ref->sourceClip, "follow");
        m_controller = Controller::FollowSceneEntity;   // SelectClip picks Clip
        ++m_followClipChanges;
        m_followLastT = -1.0;
    }
    const double t = static_cast<double>(an->primaryTime);
    // A JUMP of the source's playhead (a scrub, a restart that is not a loop
    // wrap) is the same teleport a scrub is - reset under the same checkbox.
    if (m_followLastT >= 0.0 && std::fabs(t - m_followLastT) > 0.25 &&
        !(m_clipDuration > 0.f && t < m_followLastT &&
          m_followLastT > 0.8 * m_clipDuration && t < 0.2 * m_clipDuration))
        OnPlayheadDiscontinuity("scrub");
    m_clipT = t;
    m_followLastT = t;
    // The source keeps its own (main-lane) chains - say so.
    std::snprintf(buf, sizeof(buf), "following '%s': %s t=%.2f (its own chains keep simulating)",
                  m_followName.c_str(), Stem(m_clipPath).c_str(), t);
    m_followStatus = buf;
}

void ChainPhysicsPreviewWindow::FocusChains(const ChainPreviewContext& ctx)
{
    // Frame the chain bones (every enabled group's root + simulated bones), not
    // the character - the overlay snapshot's world positions, which come from
    // the one FK path.
    if (m_stageChain == NullEntity || !m_stagedValid || !ctx.buildBones) return;
    ChainOverlay::Snapshot snap;
    if (!ChainOverlay::BuildSnapshot(*ctx.renderer, *ctx.world, m_stageChain, m_staged,
                                     m_stageSet, -1, ctx.buildBones, snap) || !snap.valid)
        return;
    DirectX::XMFLOAT3 mn{ 1e30f, 1e30f, 1e30f }, mx{ -1e30f, -1e30f, -1e30f };
    std::uint32_t bones = 0;
    for (std::size_t i = 0; i < snap.bone.size() && i < snap.kind.size(); ++i)
    {
        if (snap.kind[i] != 1 && snap.kind[i] != 2) continue;
        const DirectX::XMFLOAT3& p = snap.bone[i];
        mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
        mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
        ++bones;
    }
    if (bones == 0) return;
    const float ex = mx.x - mn.x, ey = mx.y - mn.y, ez = mx.z - mn.z;
    const float r = std::max(0.6f * std::max(ey, 0.6f * std::max(ex, ez)), 0.02f);
    m_focus[0] = 0.5f * (mn.x + mx.x);
    m_focus[1] = 0.5f * (mn.y + mx.y);
    m_focus[2] = 0.5f * (mn.z + mx.z);
    m_radius = r;
    m_dist   = 1.f;
    m_pan[0] = m_pan[1] = m_pan[2] = 0.f;
    m_focusValid = true;
    for (int k = 0; k < 3; ++k) m_focusDriveBase[k] = m_driveOffset[k];
    ++m_focusChainsCount;
    LOG_INFO("[CHAINPREVIEW] focus: chains bones=%u focusX=%.3f focusY=%.3f focusZ=%.3f "
             "radius=%.4f n=%llu", bones, m_focus[0], m_focus[1], m_focus[2], m_radius,
             static_cast<unsigned long long>(m_focusChainsCount));
}

void ChainPhysicsPreviewWindow::TickScene(const ChainPreviewContext& ctx)
{
    if (m_stageRoot == NullEntity || !ctx.world) return;
    World& world = *ctx.world;

    // ---- stress pick (the picker's top rows, -chainpreview_stress) ----
    if (m_pendingStress >= 0)
    {
        const int k = m_pendingStress;
        m_pendingStress = -1;
        if (k <= 1)
        {
            const std::string path = ResolveStressClip(k == 0);
            (k == 0 ? m_stressIdle : m_stressWalkRun) = path;
            if (path.empty())
                m_status = k == 0 ? "Stress: no idle clip found." : "Stress: no run / walk clip found.";
            else
            {
                m_pendingClipPath = path;
                m_pendingClipSource.clear();
                m_pendingClipSet  = true;
                m_driveProgram    = DriveProgram::None;
            }
            LOG_INFO("[CHAINPREVIEW] stress: %s -> '%s'", k == 0 ? "idle" : "walk-run",
                     path.c_str());
        }
        else
        {
            m_driveProgram = k == 2 ? DriveProgram::YawSweep : DriveProgram::TeleportHop;
            m_driveProgT   = 0.0;
            LOG_INFO("[CHAINPREVIEW] stress: %s (root-motion driver program, synthesized)",
                     k == 2 ? "yaw sweep + hard stop" : "teleport hop");
        }
    }

    // ---- FollowSceneEntity pick ----
    if (m_pendingFollowSel)
    {
        m_pendingFollowSel = false;
        const Entity e = m_pendingFollowEnt;
        m_pendingFollowEnt = NullEntity;
        if (e == NullEntity || !world.IsAlive(e))
            m_status = "Follow: nothing selected.";
        else if (world.HasComponent<PreviewStageTag>(e))
            m_status = "Follow: that entity IS the preview stage.";
        else
        {
            const Entity a = Anim::ResolveAnimEntityOrSelf(world, e);
            if (a == NullEntity || !world.HasComponent<AnimationComponent>(a))
                m_status = "Follow: the selection has no animation to follow.";
            else
            {
                m_followSrc    = a;
                m_followSrcH   = world.MakeHandle(a);
                m_followName   = world.GetName(a).empty() ? world.GetName(e) : world.GetName(a);
                m_followLastT  = -1.0;
                m_controller   = Controller::FollowSceneEntity;
                m_pendingControllerSet = false;
                LOG_INFO("[CHAINPREVIEW] follow: source=%u name='%s' pinned=0 (it keeps its "
                         "own main-lane chains and its own anim cull)",
                         static_cast<unsigned>(a), m_followName.c_str());
            }
        }
    }

    // ---- Scale (the Scene section; resets the chains) ----
    if (m_pendingStageScale > 0.f)
    {
        const float s = std::clamp(m_pendingStageScale, 0.001f, 100.f);
        m_pendingStageScale = -1.f;
        m_stageScale[0] = m_stageScale[1] = m_stageScale[2] = s;
        m_userScale   = true;
        m_scaleSource = "user";
        m_scaleInstance = NullEntity;
        // A different size is a different character to the solver: restart the
        // chains from the new pose, and reframe once the transforms moved.
        ResetSimulation(*ctx.renderer, world, "scale");
        m_focusValid = false;
        m_focusTries = 0;
        m_focusHoldUntil = m_tick + 3;
        LOG_INFO("[CHAINPREVIEW] scale: value=%.4f scaleX=%.4f scaleY=%.4f scaleZ=%.4f "
                 "source=%s instance=0 spawn=%u", s, s, s, s, m_scaleSource, m_spawnCount);
    }

    // ---- camera: Focus (chains) / Frame all ----
    if (m_pendingFocusChains)
    {
        m_pendingFocusChains = false;
        FocusChains(ctx);
    }
    if (m_pendingFrameAll)
    {
        m_pendingFrameAll = false;
        m_pan[0] = m_pan[1] = m_pan[2] = 0.f;
        m_dist = 1.f;
        m_focusValid = false;
        m_focusTries = 0;
    }

    // ---- the floor (a context mesh) ----
    EnsureFloor(ctx);
}

void ChainPhysicsPreviewWindow::LogScene(const char* when) const
{
    LOG_INFO("[CHAINPREVIEW] scene: floor=%d floorCollide=%d floorGap=%.4f floorSize=%.3f "
             "floorRefused=%d floorSpawns=%u proxyRadius=%.3f sceneCapsules=%u gravX=%.3f "
             "gravY=%.3f gravZ=%.3f gravScale=%.3f noGravity=%d gravityOverride=%u "
             "windStrength=%.4f windFreq=%.3f windTurbulence=%.3f windGroupsSet=%u "
             "windApplies=%llu turntable=%d turntableYawDeg=%.2f panX=%.4f panY=%.4f "
             "panZ=%.4f scale=%.4f subjectHeight=%.4f focusChains=%llu controller=%d "
             "followFrames=%llu followCulledFrames=%llu followClipChanges=%llu "
             "followFallbacks=%llu when=%s",
             m_floorEnt != NullEntity ? 1 : 0, m_floorCollide ? 1 : 0, m_floorGap, FloorSize(),
             m_floorRefused ? 1 : 0, m_floorSpawns, m_floorProxyValid ? m_floorProxyR : 0.f,
             m_uiLaneStats.sceneCapsules, m_gravDown[0], m_gravDown[1], m_gravDown[2],
             m_gravScale, m_noGravity ? 1 : 0, m_uiLaneStats.gravityOverride,
             m_stagedValid ? m_staged.windStrength : 0.f,
             m_stagedValid ? m_staged.windFreq : 0.f,
             m_stagedValid ? m_staged.windTurbulence : 0.f, m_windGroupsSet,
             static_cast<unsigned long long>(m_windApplies), m_turntable ? 1 : 0,
             m_turntableYaw / kDegToRad, m_pan[0], m_pan[1], m_pan[2], m_stageScale[0],
             SubjectHeight(), static_cast<unsigned long long>(m_focusChainsCount),
             static_cast<int>(m_controller),
             static_cast<unsigned long long>(m_followFrames),
             static_cast<unsigned long long>(m_followCulledFrames),
             static_cast<unsigned long long>(m_followClipChanges),
             static_cast<unsigned long long>(m_followFallbacks), when ? when : "");
}

void ChainPhysicsPreviewWindow::LogDrive(const char* when) const
{
    LOG_INFO("[CHAINPREVIEW] drive: move=%.4f rot=%.3f span=%.4f programId=%d progT=%.3f "
             "rotDeg=%.3f yawDeg=%.3f offsetX=%.4f offsetZ=%.4f steps=%llu teleports=%llu "
             "teleportReset=%d driveOff=%d when=%s",
             m_driveMove, m_driveRot, DriveSpan(), static_cast<int>(m_driveProgram),
             m_driveProgT, m_driveRotTotal, m_driveYawDeg, m_driveOffset[0], m_driveOffset[2],
             static_cast<unsigned long long>(m_driveSteps),
             static_cast<unsigned long long>(m_teleports), m_teleportReset ? 1 : 0,
             m_harnessDriveOff ? 1 : 0, when ? when : "");
}

// ---- the Scene section, UI half ----
void ChainPhysicsPreviewWindow::DrawSceneUi()
{
    if (m_stageRoot == NullEntity) { ImGui::TextDisabled("No stage."); return; }
    // Every row flows; fixed widths shrink to fit.
    namespace Flow = EditorUI::Flow;
    const ImVec4 kNote(0.75f, 0.75f, 0.75f, 1.f);

    // ---- scale ----
    float sc = m_pendingStageScale > 0.f ? m_pendingStageScale : m_stageScale[0];
    ImGui::SetNextItemWidth(Flow::FitWidth(120.f, "Scale"));
    if (ImGui::DragFloat("Scale", &sc, 0.005f, 0.001f, 100.f, "%.3f") && sc > 0.f)
        m_pendingStageScale = sc;
    {
        char src[64];
        std::snprintf(src, sizeof(src), "(%s)", m_scaleSource);
        Flow::SameLineIfFits(Flow::TextW(src));
        ImGui::TextDisabled("%s", src);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The stage's WORLD size. Default: the scale your SCENE uses this\n"
                          "prefab at (its first instance), else the prefab's authored scale.\n"
                          "The chain solver simulates in the skeleton's own space, so the\n"
                          "scale does not change the chains (measured: bit-identical at 0.1\n"
                          "and 1.0); it sizes the floor, the drive distances and the camera.\n"
                          "Changing it resets the simulation.");
    if (m_stageScale[0] != m_stageScale[1] || m_stageScale[0] != m_stageScale[2])
        Flow::ColoredWrapped(ImVec4(1.f, 0.8f, 0.3f, 1.f), "non-uniform: %.3f %.3f %.3f",
                             m_stageScale[0], m_stageScale[1], m_stageScale[2]);

    // ---- floor + gap ----
    ImGui::SeparatorText("Floor");
    bool floorOn = m_floorMode == 1 || (m_floorMode < 0 && m_harnessSubject.empty());
    if (ImGui::Checkbox("Floor", &floorOn)) { m_floorMode = floorOn ? 1 : 0; m_floorRefused = false; }
    Flow::SameLineIfFits(Flow::LabeledW(90.f, "gap"));
    ImGui::SetNextItemWidth(Flow::FitWidth(90.f, "gap"));
    ImGui::DragFloat("gap", &m_floorGap, 0.005f * std::max(SubjectHeight(), 0.01f), 0.f,
                     10.f * std::max(SubjectHeight(), 0.1f), "%.3f");
    Flow::SameLineIfFits(Flow::CheckboxW("Collide"));
    ImGui::Checkbox("Collide", &m_floorCollide);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The chain solver collides with CAPSULES only - the floor plane is a\n"
                          "visual reference. 'Collide' adds a capsule PROXY: one very large\n"
                          "capsule (radius %.0f x the subject's height) lying under the floor,\n"
                          "flat to ~1%% over a character's width. Your authored capsules are\n"
                          "unchanged; the proxy exists only in this preview.",
                          static_cast<double>(kFloorProxyHeights));
    if (m_floorRefused)
        Flow::ColoredWrapped(ImVec4(1.f, 0.45f, 0.35f, 1.f), "floor refused: %s",
                             m_floorRefusedWhy.c_str());
    Flow::DisabledWrapped("context meshes: floor %s (1 instance of %u + the stage's %u)",
                          m_floorEnt != NullEntity ? "on" : "off", m_budgetInstances + 1u,
                          m_budgetInstances);

    // ---- camera: turntable, focus ----
    ImGui::SeparatorText("Camera");
    ImGui::Checkbox("Turntable", &m_turntable);
    Flow::SameLineIfFits(Flow::LabeledW(90.f, "deg/s##tt"));
    ImGui::SetNextItemWidth(Flow::FitWidth(90.f, "deg/s##tt"));
    ImGui::DragFloat("deg/s##tt", &m_turntableSpeed, 1.f, -360.f, 360.f, "%.0f");
    Flow::SameLineIfFits(Flow::ButtonW("Stop##tt"));
    if (ImGui::SmallButton("Stop##tt")) { m_turntable = false; m_turntableYaw = 0.f; }
    if (ImGui::SmallButton("Focus chains (F)")) m_pendingFocusChains = true;
    Flow::SameLineIfFits(Flow::ButtonW("Frame all"));
    if (ImGui::SmallButton("Frame all")) m_pendingFrameAll = true;
    Flow::SameLineIfFits(Flow::TextW("pan: middle drag"));
    ImGui::TextDisabled("pan: middle drag");

    // ---- gravity ----
    ImGui::SeparatorText("Gravity");
    ImGui::SetNextItemWidth(Flow::FitWidth(180.f, "down"));
    ImGui::DragFloat3("down", m_gravDown, 0.01f, -2.f, 2.f, "%.2f");
    ImGui::SetNextItemWidth(Flow::FitWidth(120.f, "scale##grav"));
    ImGui::SliderFloat("scale##grav", &m_gravScale, 0.f, 3.f, "x%.2f");
    Flow::SameLineIfFits(Flow::CheckboxW("No Gravity"));
    ImGui::Checkbox("No Gravity", &m_noGravity);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("DIAGNOSTIC: every strand's gravity is zero - chain, cloth, per-group\n"
                          "overrides and spring bones. What is left is the rest pull, the\n"
                          "limits and the collisions: does the chain hold its shape by itself?");
    if (m_stagedValid)
        Flow::DisabledWrapped("chain gravity now: (%.2f, %.2f, %.2f) skel. units",
                              m_noGravity ? 0.f : m_gravDown[0] * m_gravScale * -m_staged.gravity,
                              m_noGravity ? 0.f : m_gravDown[1] * m_gravScale * -m_staged.gravity,
                              m_noGravity ? 0.f : m_gravDown[2] * m_gravScale * -m_staged.gravity);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A PREVIEW-SCENE setting, not a parameter: it scales / turns every\n"
                          "strand's AUTHORED gravity (down x scale x |authored|) and is never\n"
                          "Applied. (0,-1,0) x 1 is exactly the authored gravity.");
    if (ImGui::SmallButton("Reset gravity"))
    { m_gravDown[0] = 0.f; m_gravDown[1] = -1.f; m_gravDown[2] = 0.f; m_gravScale = 1.f; m_noGravity = false; }

    // ---- wind (AUTHORED - edits the staged parameters) ----
    ImGui::SeparatorText("Wind");
    if (m_stagedValid)
    {
        float dir[3] = { m_staged.windDir.x, m_staged.windDir.y, m_staged.windDir.z };
        float str = m_staged.windStrength, fr = m_staged.windFreq, tb = m_staged.windTurbulence;
        bool changed = false;
        ImGui::SetNextItemWidth(Flow::FitWidth(180.f, "dir"));
        changed |= ImGui::DragFloat3("dir", dir, 0.01f, -1.f, 1.f, "%.2f");
        ImGui::SetNextItemWidth(Flow::FitWidth(120.f, "strength (skel.)"));
        changed |= ImGui::DragFloat("strength (skel.)", &str, 0.005f * std::max(SubjectHeight(), 0.01f),
                                    0.f, 10.f * std::max(SubjectHeight(), 0.1f), "%.3f");
        ImGui::SetNextItemWidth(Flow::FitWidth(120.f, "freq (Hz)"));
        changed |= ImGui::DragFloat("freq (Hz)", &fr, 0.01f, 0.f, 10.f, "%.2f");
        ImGui::SetNextItemWidth(Flow::FitWidth(120.f, "turbulence"));
        changed |= ImGui::DragFloat("turbulence", &tb, 0.01f, 0.f, 4.f, "%.2f");
        if (changed) ApplyWindToStaged(dir, str, fr, tb, m_windAlsoGroups);
        Flow::Checkbox("also groups that override wind", &m_windAlsoGroups);
        std::uint32_t ovr = 0, stiff0 = 0;
        for (const ChainGroupDef& g : m_staged.groups)
        {
            ovr += g.ovrEnabled ? 1u : 0u;
            const float ls = g.ovrEnabled ? g.ovrLocalStiffness
                           : (g.type == ChainGroupType::Cloth ? m_staged.skirtLocalStiffness
                                                              : m_staged.localStiffness);
            stiff0 += ls <= 0.f ? 1u : 0u;
        }
        Flow::ColoredWrapped(kNote, "%u / %u group(s) override wind; %u have localStiffness 0",
                             ovr, static_cast<unsigned>(m_staged.groups.size()), stiff0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Wind is a REST-TARGET offset, not a force: it moves the target the\n"
                              "local-stiffness spring pulls toward, so a group needs\n"
                              "localStiffness > 0 to show any wind. A group with an override uses\n"
                              "its own wind strength / freq (the global ones are ignored there).\n"
                              "The sway phase is the preview lane's clock (sway t in Simulation) -\n"
                              "it must advance, or wind is a static lean. Wind is AUTHORED: it is a\n"
                              "panel-undo step and Apply writes it.");
    }
    else
        ImGui::TextDisabled("(no chain parameters)");

    // ---- the root-motion driver ----
    ImGui::SeparatorText("Drive (root motion)");
    {
        // World-motion inertia (Parameters > World Motion): whether the driver's
        // motion reaches the chains at all is an AUTHORED property of the rig.
        const bool anyInertia = m_stagedValid && ChainPhysicsSystem::WantsWorldInertia(m_staged);
        if (!anyInertia)
            Flow::ColoredWrapped(ImVec4(1.f, 0.8f, 0.3f, 1.f),
                                 "world inertia 0: entity motion does not reach the chains");
        else
            Flow::ColoredWrapped(kNote, "world inertia loc %.2f rot %.2f - kicks %llu, teleports "
                                 "ignored %llu (> %.3f u / %.1f deg a step)",
                                 m_staged.worldInertiaLocation, m_staged.worldInertiaRotation,
                                 static_cast<unsigned long long>(m_uiLaneStats.worldKicks),
                                 static_cast<unsigned long long>(m_uiLaneStats.worldTeleports),
                                 m_staged.teleportDistance, m_staged.teleportAngle);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The chain solver simulates in the SKELETON's own space (FK from the\n"
                              "pose, no entity transform). The driver moves the stage root the way a\n"
                              "game moves a character; that motion reaches the chains only through\n"
                              "Parameters > World Motion: 'Inertia' = the share of the entity's\n"
                              "translation / rotation the chains feel (0 = none, the legacy solve;\n"
                              "only CHANGES of motion kick - start, stop, turn, the ping-pong's\n"
                              "reversals - not a steady walk). A step moving further than the\n"
                              "'Teleport' distance / angle is ignored: no whip. Per group: its\n"
                              "'Own world inertia' (otherwise it follows the entity's).");
    }
    ImGui::SetNextItemWidth(Flow::FitWidth(100.f, "move"));
    ImGui::DragFloat("move", &m_driveMove, 0.01f * std::max(SubjectHeight(), 0.01f),
                     -20.f * std::max(SubjectHeight(), 0.1f), 20.f * std::max(SubjectHeight(), 0.1f),
                     "%.3f u/s");
    Flow::SameLineIfFits(Flow::LabeledW(100.f, "span"));
    ImGui::SetNextItemWidth(Flow::FitWidth(100.f, "span"));
    ImGui::DragFloat("span", &m_driveSpan, 0.01f * std::max(SubjectHeight(), 0.01f), 0.f,
                     50.f * std::max(SubjectHeight(), 0.1f), m_driveSpan > 0.f ? "%.3f" : "auto");
    ImGui::SetNextItemWidth(Flow::FitWidth(100.f, "rotate"));
    ImGui::DragFloat("rotate", &m_driveRot, 1.f, -720.f, 720.f, "%.0f deg/s");
    Flow::SameLineIfFits(Flow::ButtonW("Stop drive"));
    if (ImGui::SmallButton("Stop drive"))
    {
        m_driveMove = m_driveRot = 0.f;
        m_driveProgram = DriveProgram::None;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stops every speed and the stress program; the root stays where it is\n"
                          "(Reset in Simulation puts it back at the origin).");
    ImGui::SetNextItemWidth(Flow::FitWidth(100.f, "dist"));
    ImGui::DragFloat("dist", &m_teleportDist, 0.01f * std::max(SubjectHeight(), 0.01f), 0.f,
                     20.f * std::max(SubjectHeight(), 0.1f), m_teleportDist > 0.f ? "%.3f" : "auto");
    Flow::SameLineIfFits(Flow::LabeledW(80.f, "angle"));
    ImGui::SetNextItemWidth(Flow::FitWidth(80.f, "angle"));
    ImGui::DragFloat("angle", &m_teleportAngle, 1.f, -180.f, 180.f, "%.0f deg");
    Flow::SameLineIfFits(Flow::ButtonW("Teleport"));
    if (ImGui::Button("Teleport")) m_pendingTeleport = true;
    Flow::Checkbox("Reset chains on teleport", &m_teleportReset);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off (default): the chains integrate across the hop. With World Motion\n"
                          "inertia 0 the hop never reaches them; with inertia > 0 a hop beyond\n"
                          "the Teleport distance / angle is ignored (no whip) and a smaller one\n"
                          "is felt as a kick. On: the chains restart from the new pose (what a\n"
                          "game's teleport path may do instead).");
    const char* prog = m_driveProgram == DriveProgram::YawSweep ? "yaw sweep + hard stop"
                     : m_driveProgram == DriveProgram::TeleportHop ? "teleport hop" : "none";
    Flow::ColoredWrapped(kNote, "program: %s   yaw %.1f deg   offset %.3f   teleports %llu",
                         prog, m_driveYawDeg, m_driveOffset[2],
                         static_cast<unsigned long long>(m_teleports));
    if (ImGui::SmallButton("Yaw sweep")) m_pendingStress = 2;
    Flow::SameLineIfFits(Flow::ButtonW("Hop"));
    if (ImGui::SmallButton("Hop")) m_pendingStress = 3;
    Flow::DisabledWrapped("The driver runs on the stage clock: Pause stops it too.");
}

// ==== Harness census - O(1) per counter (pool sizes, renderer tallies) ====
ChainPhysicsPreviewWindow::Census
ChainPhysicsPreviewWindow::TakeCensus(const ChainPreviewContext& ctx) const
{
    Census c{};
    if (!ctx.renderer || !ctx.world) return c;
    World&    w = *ctx.world;
    Renderer& r = *ctx.renderer;
    c.joltBodies  = ctx.physics ? ctx.physics->GetBodyCount() : 0u;
    c.joltComps   = PoolSize<ColliderComponent>(w) + PoolSize<RigidBodyComponent>(w) +
                    PoolSize<CharacterControllerComponent>(w);
    c.aiAgents    = PoolSize<AIComponent>(w);
    c.aiComps     = PoolSize<BrainComponent>(w) + PoolSize<NavAgentComponent>(w) +
                    PoolSize<PerceptionComponent>(w) + PoolSize<PerceptibleComponent>(w);
    c.shadowSet   = r.GetPerObjectShadowSetSize();
    c.shadowRoots = PoolSize<PerObjectShadowComponent>(w) + PoolSize<PlayerComponent>(w);
    c.locomotion  = PoolSize<LocomotionComponent>(w);
    c.scripts     = PoolSize<ScriptComponent>(w);
    c.skinnedOwned   = r.GetSkinnedOwnedBufferEntityCount();
    c.skinnedOrphans = r.GetSkinnedOrphanedBufferCount();
    c.sharedSources  = r.GetSkinnedSharedSourceCount();
    c.drawPackets = r.GetFrameStats().drawPackets;
    c.bvhLeaves   = r.GetSceneBVHLeafCount();
    c.skeletons   = PoolSize<SkeletonComponent>(w);
    c.entities    = static_cast<std::uint32_t>(w.GetEntities().size());
    c.cameras     = PoolSize<VirtualCameraComponent>(w);
    if (ctx.gfx)
    {
        std::uint64_t used = 0, budget = 0;
        if (static_cast<GraphicsDX12*>(ctx.gfx)->QueryVramUsage(used, budget))
            c.vramMB = static_cast<double>(used) / (1024.0 * 1024.0);
    }
    c.valid = true;
    return c;
}

void ChainPhysicsPreviewWindow::LogCensus(const char* phase, const Census& c) const
{
    LOG_INFO("[CHAINPREVIEW] census phase=%s bodies=%u jolt=%u ai=%u aiComps=%u "
             "shadowSet=%u shadowRoots=%u loco=%u scripts=%u owned=%u orphans=%u "
             "shared=%u draws=%u leaves=%u skeletons=%u entities=%u vcams=%u vramMB=%.1f",
             phase, c.joltBodies, c.joltComps, c.aiAgents, c.aiComps, c.shadowSet,
             c.shadowRoots, c.locomotion, c.scripts, c.skinnedOwned,
             c.skinnedOrphans, c.sharedSources, c.drawPackets, c.bvhLeaves, c.skeletons,
             c.entities, c.cameras, c.vramMB);
}

} // namespace Editor
