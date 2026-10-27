#include "ECS/FacialAnimationSystem.h"
#include "ECS/AnimationComponents.h"   // SkeletonComponent, kInvalidAnimHandle
#include "Graphics/AnimProfile.h"
#include "Harness/Determinism.h"       // -seed (inert stub in Retail)
#include "System/Log.h"

#include <cmath>
#include <cstring>

static_assert(FacialAnimationComponent::kMaxAuthored == static_cast<int>(kMaxMorphTargets),
              "the Authored list holds at most one entry per morph slot");

// FacialAnimationSystem: the header has the call-order and attachment contracts.
// Everything is component-resident: no per-world map, no allocation on the per-frame path,
// no structural World mutation.

namespace
{
    using FA = FacialAnimationComponent;

    // NaN-safe on purpose: tested as "> 0" first so a NaN (every comparison false) maps to 0.
    // A surviving NaN weight fails both the release test (current <= 1e-4f) and AllocChannel's
    // reclaim test (current <= 1e-3f), so one bad Lua number would burn one of the 32 channel slots
    // forever. Bit-identical to a plain clamp for every other input.
    inline float Sat(float v) { return (v > 0.f) ? ((v < 1.f) ? v : 1.f) : 0.f; }

    // xorshift32. fa.rng is forced non-zero at seed time (0 is a fixed point).
    inline float Rand01(std::uint32_t& s)
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s >> 8) * (1.0f / 16777216.0f);   // [0,1)
    }

    inline bool NameEq(const char* a, const char* b)
    {
        return std::strncmp(a, b, FA::kNameLen) == 0;
    }

    // ---- Diagnostics ----
    // An unresolved morph name is warned once per distinct name, at LOG_WARNING (never LOG_ERROR:
    // negative tests run under -maxerrors=0). A fixed 64-entry hash list keeps it allocation-free;
    // past 64 distinct misses it stops reporting. Single-threaded: RendererAnimationChainSystem is
    // OPAQUE, so nothing runs beside it.
    std::uint32_t g_warned[64]{};
    std::uint32_t g_warnedCount = 0;

    std::uint32_t Fnv1a(const char* s, std::size_t maxLen)
    {
        std::uint32_t h = 2166136261u;
        for (std::size_t i = 0; i < maxLen && s[i]; ++i)
        {
            h ^= static_cast<unsigned char>(s[i]);
            h *= 16777619u;
        }
        return h;
    }

    bool ShouldWarnOnce(std::uint32_t key)
    {
        for (std::uint32_t i = 0; i < g_warnedCount; ++i)
            if (g_warned[i] == key) return false;
        if (g_warnedCount >= 64) return false;
        g_warned[g_warnedCount++] = key;
        return true;
    }

    // ---- resolvedOut emitters ----
    // The Authored list holds up to kMaxAuthored entries, so a slot finds its entry through
    // resolvedIndexOfSlot, not a scan. That index is never cleared: an entry is trusted only while
    // it points below resolvedCount at the same slot. Both vectors keep their capacity, so nothing
    // allocates once warm.
    // This frame's entry for `slot`, or nullptr.
    FA::Resolved* Existing(FA& fa, std::uint16_t slot)
    {
        std::vector<std::int32_t>& at = fa.resolvedIndexOfSlot;
        if (slot >= at.size()) at.resize(static_cast<std::size_t>(slot) + 1, -1);
        const std::int32_t i = at[slot];
        if (i >= 0 && static_cast<std::uint32_t>(i) < fa.resolvedCount && fa.resolvedOut[i].slot == slot)
            return &fa.resolvedOut[i];
        return nullptr;
    }
    void Append(FA& fa, const FA::Resolved& r)
    {
        if (fa.resolvedCount >= fa.resolvedOut.size()) fa.resolvedOut.emplace_back();
        fa.resolvedOut[fa.resolvedCount] = r;
        fa.resolvedIndexOfSlot[r.slot] = static_cast<std::int32_t>(fa.resolvedCount++);
    }
    // The three fold rules. With nothing tracked every entry keeps alpha = 1 / floor = 0, and only
    // Override and Max matter.
    //
    // Override (Authored, Expression, Viseme, Manual): owns the slot and resets alpha and floor, or a
    // hot-key expression over a half-faded track would only half apply.
    void EmitOverride(FA& fa, std::uint16_t slot, float w)
    {
        if (FA::Resolved* r = Existing(fa, slot))
        {
            r->weight  = w;
            r->maxOnly = 0;
            r->alpha   = 1.0f;
            r->floor   = 0.0f;
            return;
        }
        Append(fa, FA::Resolved{ slot, w, 0, 1.0f, 0.0f });
    }
    // Tracked: over an Authored value it blends there (alpha stays 1 - Authored already
    // overrides); alone it carries its alpha to BuildSkinJobs, which blends over the clip.
    void EmitTracked(FA& fa, std::uint16_t slot, float t, float a)
    {
        if (FA::Resolved* r = Existing(fa, slot))
        {
            r->weight = (1.0f - a) * r->weight + a * t;
            return;
        }
        Append(fa, FA::Resolved{ slot, t, 0, a, 0.0f });
    }
    // Blink (Max): raises the weight - or, on a slot whose alpha is below 1, the floor, so
    // a blink returning under a fading track still closes the eye all the way.
    void EmitMax(FA& fa, std::uint16_t slot, float b)
    {
        if (FA::Resolved* r = Existing(fa, slot))
        {
            if (r->alpha >= 1.0f) { if (b > r->weight) r->weight = b; }
            else if (b > r->floor) r->floor = b;
            return;
        }
        Append(fa, FA::Resolved{ slot, b, 1, 1.0f, 0.0f });
    }
}

// ==== Facial:: helper API ====
namespace Facial
{

std::int16_t FindMorphSlot(const SkeletonAsset& skel, const char* name)
{
    if (!name || !name[0]) return FA::kUnresolved;
    const std::uint32_t n = (skel.morphTargetCount < SkeletonAsset::MAX_MORPH_TARGETS)
                          ? skel.morphTargetCount : SkeletonAsset::MAX_MORPH_TARGETS;
    for (std::uint32_t m = 0; m < n; ++m)
        if (std::strncmp(name, skel.morphTargetNames[m], 64) == 0)
            return static_cast<std::int16_t>(m);
    return FA::kUnresolved;
}

bool IsNameTableTruncated(const SkeletonAsset& skel)
{
    return skel.morphTargetCount >= SkeletonAsset::MAX_MORPH_TARGETS;
}

void CopyName(char* dst64, const char* src)
{
    if (!dst64) return;
    if (!src) { dst64[0] = '\0'; return; }
    std::size_t i = 0;
    for (; i + 1 < static_cast<std::size_t>(FA::kNameLen) && src[i]; ++i)
        dst64[i] = src[i];
    dst64[i] = '\0';
}

namespace
{
    // Channel lookup / allocation, shared by every setter. Identity is (group, name).
    int FindChannel(const FA& fa, FacialGroup group, const char* name)
    {
        const std::uint8_t g = static_cast<std::uint8_t>(group);
        for (int k = 0; k < FA::kMaxChannels; ++k)
            if (fa.channels[k].state != 0 && fa.channels[k].group == g &&
                NameEq(fa.channelName[k], name))
                return k;
        return -1;
    }

    // Free slot first, then a fully-faded releasing channel. Never evicts a live one: the caller
    // reports false and the script keeps its existing face.
    int AllocChannel(FA& fa)
    {
        for (int k = 0; k < FA::kMaxChannels; ++k)
            if (fa.channels[k].state == 0) return k;
        for (int k = 0; k < FA::kMaxChannels; ++k)
            if (fa.channels[k].state == 2 && fa.channels[k].current <= 1e-3f) return k;
        return -1;
    }

    void FreeChannel(FA& fa, int k)
    {
        fa.channels[k]       = FA::Channel{};
        fa.channelName[k][0] = '\0';
    }
}

bool SetChannel(FA& fa, FacialGroup group, const char* name, float weight, float blendSeconds)
{
    if (!name || !name[0]) return false;
    const float w = Sat(weight);

    int k = FindChannel(fa, group, name);
    if (k < 0)
    {
        k = AllocChannel(fa);
        if (k < 0) return false;                 // table full - never evicts
        FreeChannel(fa, k);
        CopyName(fa.channelName[k], name);
        fa.channels[k].slot    = FA::kUnresolved;
        fa.channels[k].current = 0.0f;
        fa.channels[k].group   = static_cast<std::uint8_t>(group);
    }

    FA::Channel& c = fa.channels[k];
    c.state    = 1;                              // revive a releasing channel
    c.target   = w;
    c.blendSec = (blendSeconds > 0.f) ? blendSeconds : 0.f;
    // Instant writes land now so a same-frame GetChannelWeight is exact: the system runs a phase
    // later (Animation), so a script could not otherwise read back what it just wrote.
    if (c.blendSec <= 0.f) c.current = w;
    return true;
}

bool ClearChannel(FA& fa, FacialGroup group, const char* name, float blendSeconds)
{
    if (!name || !name[0]) return false;
    const int k = FindChannel(fa, group, name);
    if (k < 0) return false;
    if (blendSeconds > 0.f)
    {
        fa.channels[k].state    = 2;             // releasing; freed at current == 0
        fa.channels[k].target   = 0.0f;
        fa.channels[k].blendSec = blendSeconds;
    }
    else
    {
        FreeChannel(fa, k);
    }
    return true;
}

void ClearGroup(FA& fa, FacialGroup group, float blendSeconds)
{
    const std::uint8_t g = static_cast<std::uint8_t>(group);
    for (int k = 0; k < FA::kMaxChannels; ++k)
    {
        if (fa.channels[k].state == 0 || fa.channels[k].group != g) continue;
        if (blendSeconds > 0.f)
        {
            fa.channels[k].state    = 2;
            fa.channels[k].target   = 0.0f;
            fa.channels[k].blendSec = blendSeconds;
        }
        else
        {
            FreeChannel(fa, k);
        }
    }
}

void ClearScripted(FA& fa, float blendSeconds)
{
    // Authored rows are the designer's persistent base and auto-blink is a machine, not a channel:
    // Face.ClearAll owns neither.
    ClearGroup(fa, FacialGroup::Manual,     blendSeconds);
    ClearGroup(fa, FacialGroup::Expression, blendSeconds);
    ClearGroup(fa, FacialGroup::Viseme,     blendSeconds);
}

void BeginExpression(FA& fa)
{
    // Mark every live Expression channel "releasing". SetChannel revives the ones
    // the new table names; EndExpression finishes off the rest. No allocation, no
    // second name list.
    const std::uint8_t g = static_cast<std::uint8_t>(FacialGroup::Expression);
    for (int k = 0; k < FA::kMaxChannels; ++k)
        if (fa.channels[k].state == 1 && fa.channels[k].group == g)
        {
            fa.channels[k].state  = 2;
            fa.channels[k].target = 0.0f;
        }
}

void EndExpression(FA& fa, float blendSeconds)
{
    const std::uint8_t g = static_cast<std::uint8_t>(FacialGroup::Expression);
    for (int k = 0; k < FA::kMaxChannels; ++k)
    {
        if (fa.channels[k].state != 2 || fa.channels[k].group != g) continue;
        if (blendSeconds > 0.f) fa.channels[k].blendSec = blendSeconds;
        else                    FreeChannel(fa, k);
    }
}

bool GetChannelWeight(const FA& fa, FacialGroup group, const char* name, float* outWeight)
{
    if (!name || !name[0]) return false;
    const int k = FindChannel(fa, group, name);
    if (k < 0) return false;
    if (outWeight) *outWeight = fa.channels[k].current;
    return true;
}

bool GetResolvedWeight(const FA& fa, std::uint16_t slot, float* outWeight)
{
    for (std::uint32_t i = 0; i < fa.resolvedCount; ++i)
        if (fa.resolvedOut[i].slot == slot)
        {
            if (outWeight) *outWeight = fa.resolvedOut[i].weight;
            return true;
        }
    return false;
}

int VowelIndex(const char* key)
{
    if (!key || !key[0] || key[1]) return -1;    // exactly one ASCII char
    switch (key[0])
    {
        case 'a': case 'A': return 0;
        case 'i': case 'I': return 1;
        case 'u': case 'U': return 2;
        case 'e': case 'E': return 3;
        case 'o': case 'O': return 4;
        default:            return -1;
    }
}

bool SetVowel(FA& fa, int vowelIdx, float amount, float blendSeconds)
{
    if (vowelIdx < 0 || vowelIdx >= FA::kVowelCount) return false;
    bool ok = true;
    for (int v = 0; v < FA::kVowelCount; ++v)
    {
        const char* n = fa.visemeMorph[v];
        if (!n[0]) continue;
        if (v == vowelIdx) ok = SetChannel(fa, FacialGroup::Viseme, n, amount, blendSeconds) && ok;
        else               ClearChannel(fa, FacialGroup::Viseme, n, blendSeconds);
    }
    return ok;
}

void ForceBlinkWeight(FA& fa, float weight)
{
    fa.blinkWeightCur = Sat(weight);
}

void InvalidateResolution(FA& fa)
{
    fa.resolvedAssetIndex   = 0xFFFFFFFFu;
    fa.blinkSlot            = FA::kUnresolved;
    fa.blinkResolvedName[0] = '\0';
    for (int k = 0; k < FA::kMaxChannels; ++k)
        fa.channels[k].slot = FA::kUnresolved;
    for (FA::AuthoredEntry& a : fa.authored)
        a.slot = FA::kUnresolved;
}

bool SetAuthored(FA& fa, const char* name, float weight)
{
    if (!name || !name[0]) return false;
    // The same truncation a 64-byte skeleton name has: a longer one can never match.
    const std::string key(name, ::strnlen(name, static_cast<std::size_t>(FA::kNameLen) - 1));
    const float w = Sat(weight);
    for (std::size_t i = 0; i < fa.authored.size(); ++i)
        if (fa.authored[i].name == key)
        {
            if (w <= 0.f) fa.authored.erase(fa.authored.begin() + static_cast<std::ptrdiff_t>(i));
            else          fa.authored[i].weight = w;
            return true;
        }
    if (w <= 0.f) return true;   // nothing to remove
    if (fa.authored.size() >= static_cast<std::size_t>(FA::kMaxAuthored)) return false;
    FA::AuthoredEntry e;
    e.name   = key;
    e.weight = w;
    fa.authored.push_back(std::move(e));
    return true;
}

float GetAuthored(const FA& fa, const char* name)
{
    if (!name || !name[0]) return -1.f;
    for (const FA::AuthoredEntry& a : fa.authored)
        if (std::strncmp(a.name.c_str(), name, FA::kNameLen) == 0) return a.weight;
    return -1.f;
}

} // namespace Facial

// ==== FacialAnimationSystem ====

void FacialAnimationSystem::TriggerBlink(FacialAnimationComponent& fa,
                                         float closeSec, float holdSec,
                                         float openSec, float peak)
{
    fa.blinkCloseCur = (closeSec > 0.f) ? closeSec : ((fa.blinkCloseSec > 0.f) ? fa.blinkCloseSec : 0.f);
    fa.blinkHoldCur  = (holdSec  > 0.f) ? holdSec  : ((fa.blinkHoldSec  > 0.f) ? fa.blinkHoldSec  : 0.f);
    fa.blinkOpenCur  = (openSec  > 0.f) ? openSec  : ((fa.blinkOpenSec  > 0.f) ? fa.blinkOpenSec  : 0.f);
    fa.blinkPeakCur  = (peak     > 0.f) ? Sat(peak) : Sat(fa.blinkMaxWeight);
    fa.blinkPhase    = 1;   // closing
    fa.blinkT        = 0.0f;
    fa.blinkIdleT    = 0.0f;
}

void FacialAnimationSystem::Update(World& world, float dt,
                                   const std::unordered_set<Entity>* activeSet)
{
    auto* pFA = world.GetPool<FacialAnimationComponent>();

    // The A/B toggle must neutralise the layer, not freeze it: resolvedOut lives on the component
    // and BuildSkinJobs folds it without consulting this system, so a bare return would leave the
    // last facial layer stamped over the clip every frame. Zero it, then leave.
    if (!m_enabled)
    {
        Anim::AnimProfile::Get().facialEntities = 0;
        if (pFA)
        {
            auto& data = pFA->Data();
            for (std::size_t i = 0; i < data.size(); ++i) data[i].resolvedCount = 0;
        }
        return;
    }

    // Published before the early returns: nothing resets AnimProfile per frame, so a return past it
    // would keep reporting the previous world's count, and AnimStats.facialEntities is asserted
    // from Lua.
    Anim::AnimProfile::Get().facialEntities = 0;

    // No pool -> no component -> nothing to fold or clear. Same for the SkeletonComponent pool:
    // only World::Clear drops pools, and it drops both, so no facial component with a stale
    // resolvedOut outlives the skeleton pool.
    if (!pFA) return;
    auto* pSkel = world.GetPool<SkeletonComponent>();
    if (!pSkel) return;

    const auto& ents = pFA->Entities();
    auto&       data = pFA->Data();
    const std::size_t n = data.size();

    std::uint32_t facialEntities = 0;

    for (std::size_t i = 0; i < n; ++i)
    {
        const Entity e = ents[i];
        // First statement, before any component read, as in HeadTrackSystem. A culled character keeps
        // its previous resolvedOut (BuildSkinJobs ignores activeSet): a frozen face, deliberately.
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;

        FacialAnimationComponent& fa = data[i];

        // ---- Owner invariant ----
        // The component only works on the SkeletonComponent carrier: that is the only entity
        // BuildSkinJobs looks it up on.
        const SkeletonComponent* skc = pSkel->Get(e);
        if (!skc || skc->assetIndex == kInvalidAnimHandle ||
            skc->assetIndex >= m_skeletons.Count())
        {
            fa.resolvedCount = 0;
            static bool s_warnedOwner = false;
            if (!s_warnedOwner)
            {
                s_warnedOwner = true;
                LOG_WARNING("FacialAnimationComponent on entity %u has no usable "
                            "SkeletonComponent - it will never reach the GPU. Attach it "
                            "to the skeleton entity (the SkeletonComponent carrier).",
                            static_cast<unsigned>(e));
            }
            continue;
        }
        // Bounds-checked above on purpose: SkeletonRegistry::Get indexes m_skeletons unchecked.
        const SkeletonAsset& skel = m_skeletons.Get(skc->assetIndex);

        // ---- Runtime re-seed (fresh add / deserialize / Play-Stop round trip) ----
        if (!fa.runtimeSeeded)
        {
            // Knuth mix of the entity id, not random (as in AISystem): a fixed-seed benchmark has to replay
            // identically. XOR the harness seed so -seed shifts the whole crowd coherently; forced non-zero
            // because 0 is an xorshift32 fixed point (and NullEntity is 0).
            std::uint32_t h = static_cast<std::uint32_t>(e) * 2654435761u;
            h ^= h >> 15;
            h ^= static_cast<std::uint32_t>(Harness::Determinism::Get().Seed());
            fa.rng = (h == 0u) ? 1u : h;

            // The blink machine is deliberately not reset here, like channels[] below: Face.Blink and the
            // editor's "Blink now" run before this phase, so a reset would erase a blink requested in the
            // frame the component was created, after the Lua call returned true. Fresh, deserialized and
            // Play/Stop-restored (ComponentCloner) components already carry
            // blinkPhase/blinkT/blinkIdleT/blinkWeightCur == 0.
            //
            // The first countdown draws from [0, max) rather than [min, max) so a crowd spawned in one frame
            // scatters instead of blinking in unison.
            fa.blinkNextAt = Rand01(fa.rng) *
                             ((fa.blinkIntervalMaxSec > 0.f) ? fa.blinkIntervalMaxSec : 0.f);

            // Drop resolved slots only - do not wipe the channels: Face.* runs in GameplayLogic, before this
            // phase, so a script's EnsureComp-then-SetWeight would otherwise be lost on the first Update.
            // ComponentCloner scrubs channel state on the Play/Stop round trip; a deserialized component
            // has none.
            for (int k = 0; k < FA::kMaxChannels; ++k)
                fa.channels[k].slot = FA::kUnresolved;
            for (FA::AuthoredEntry& a : fa.authored)
                a.slot = FA::kUnresolved;
            fa.resolvedAssetIndex = 0xFFFFFFFFu;
            fa.blinkSlot          = FA::kUnresolved;
            fa.blinkResolvedName[0] = '\0';
            fa.runtimeSeeded    = true;
        }

        // ---- Resolution cache invalidation ----
        // SkeletonRegistry is append-only, so an index never renames an asset in-process, but a world
        // reload re-registers the same .iskel at a new index. The per-channel guard below adds the
        // self-healing half (as in HeadTrackSystem).
        const bool assetChanged = (fa.resolvedAssetIndex != skc->assetIndex);
        if (assetChanged) fa.resolvedAssetIndex = skc->assetIndex;

        if (!fa.enabled)
        {
            fa.resolvedCount = 0;
            continue;
        }

        // ---- Blink state machine (dt > 0 only) ----
        // Frozen at dt <= 0 so Edit mode shows a still, editable face and the editor's "Blink now" stamp
        // (Facial::ForceBlinkWeight) survives.
        if (dt > 0.0f && fa.trackingHoldsBlink)
        {
            // Face tracking owns the eyelids: no blink starts, and one in flight fades out in a few frames.
            // The idle timer restarts, so the first automatic blink after tracking lets go is a full
            // interval away.
            fa.blinkPhase  = 0;
            fa.blinkT      = 0.0f;
            fa.blinkIdleT  = 0.0f;
            fa.blinkWeightCur = (fa.blinkWeightCur > 1e-3f) ? fa.blinkWeightCur * std::exp(-dt / 0.06f) : 0.0f;
        }
        else if (dt > 0.0f)
        {
            if (fa.autoBlinkEnabled && fa.blinkPhase == 0)
            {
                fa.blinkIdleT += dt;
                if (fa.blinkIdleT >= fa.blinkNextAt)
                    TriggerBlink(fa, fa.blinkCloseSec, fa.blinkHoldSec,
                                     fa.blinkOpenSec, fa.blinkMaxWeight);
            }

            float w = 0.0f;
            if (fa.blinkPhase != 0)
            {
                fa.blinkT += dt;
                // Phase-advance loop, not a switch: with sub-frame durations (0.03 s holds at a 16.7 ms step)
                // one dt can cross two phase boundaries, and a switch would strand the machine a frame per
                // boundary. The phase advances monotonically 1->2->3->0, so the loop terminates.
                for (;;)
                {
                    if (fa.blinkPhase == 1)
                    {
                        if (fa.blinkT >= fa.blinkCloseCur)
                        { fa.blinkT -= fa.blinkCloseCur; fa.blinkPhase = 2; continue; }
                        w = fa.blinkPeakCur * Sat(fa.blinkT / ((fa.blinkCloseCur > 1e-4f) ? fa.blinkCloseCur : 1e-4f));
                        break;
                    }
                    if (fa.blinkPhase == 2)
                    {
                        if (fa.blinkT >= fa.blinkHoldCur)
                        { fa.blinkT -= fa.blinkHoldCur; fa.blinkPhase = 3; continue; }
                        w = fa.blinkPeakCur;
                        break;
                    }
                    // phase 3 - opening
                    if (fa.blinkT >= fa.blinkOpenCur)
                    {
                        fa.blinkPhase = 0;
                        fa.blinkT     = 0.0f;
                        fa.blinkIdleT = 0.0f;
                        const float lo = (fa.blinkIntervalMinSec > 0.f) ? fa.blinkIntervalMinSec : 0.f;
                        const float hi = (fa.blinkIntervalMaxSec > lo)  ? fa.blinkIntervalMaxSec : lo;
                        fa.blinkNextAt = lo + Rand01(fa.rng) * (hi - lo);
                        w = 0.0f;
                        break;
                    }
                    w = fa.blinkPeakCur *
                        (1.0f - Sat(fa.blinkT / ((fa.blinkOpenCur > 1e-4f) ? fa.blinkOpenCur : 1e-4f)));
                    break;
                }
            }
            fa.blinkWeightCur = w;
        }

        // ---- Authored: resolve ----
        // The channels' rule: re-resolve on an asset change, a never-looked-up entry, or a slot outside
        // the asset; a miss is cached and warned once.
        for (FA::AuthoredEntry& a : fa.authored)
        {
            if (!(assetChanged || a.slot == FA::kUnresolved ||
                  (a.slot >= 0 && static_cast<std::uint32_t>(a.slot) >= skel.morphTargetCount)))
                continue;
            a.slot = Facial::FindMorphSlot(skel, a.name.c_str());
            if (a.slot >= 0) continue;
            a.slot = FA::kResolvedMissing;
            const std::uint32_t key = Fnv1a(a.name.c_str(), FA::kNameLen) ^ 0x27D4EB2Fu;
            if (ShouldWarnOnce(key))
                LOG_WARNING("Facial: authored morph '%s' is not in this skeleton (%u targets).",
                            a.name.c_str(), skel.morphTargetCount);
        }

        // ---- Per-channel: resolve + ease ----
        for (int k = 0; k < FA::kMaxChannels; ++k)
        {
            FA::Channel& c = fa.channels[k];
            if (c.state == 0) continue;

            // Name -> mesh slot. HeadTrack's self-healing guard rather than assetIndex equality alone: a
            // channel whose cached slot fell outside the current asset re-resolves too.
            //
            // kResolvedMissing is not retried, or a name this rig lacks would re-run the linear name scan
            // every frame (the default PMX blink morph on any non-PMX rig does exactly that). assetChanged
            // and Facial::InvalidateResolution still force a retry, so a later rig swap can resolve it.
            if (assetChanged || c.slot == FA::kUnresolved ||
                (c.slot >= 0 &&
                 static_cast<std::uint32_t>(c.slot) >= skel.morphTargetCount))
            {
                c.slot = Facial::FindMorphSlot(skel, fa.channelName[k]);
                if (c.slot < 0)
                {
                    c.slot = FA::kResolvedMissing;   // cache the MISS
                    // Three outcomes, not two: a rig whose name table is truncated at MAX_MORPH_TARGETS has morphs
                    // past the cap that no name can reach, and that must not be debugged as a typo.
                    const std::uint32_t key = Fnv1a(fa.channelName[k], FA::kNameLen) ^ 0x9E3779B9u;
                    if (ShouldWarnOnce(key))
                    {
                        if (Facial::IsNameTableTruncated(skel))
                            LOG_WARNING("Facial: morph '%s' did not resolve, and this "
                                        "skeleton's name table is TRUNCATED at %u - morphs "
                                        "past that index cannot be addressed by name.",
                                        fa.channelName[k], SkeletonAsset::MAX_MORPH_TARGETS);
                        else
                            LOG_WARNING("Facial: morph '%s' is not in this skeleton (%u targets).",
                                        fa.channelName[k], skel.morphTargetCount);
                    }
                    // Keep the channel alive; a later rig swap may resolve it.
                }
            }

            // Ease. An instant channel (blendSec == 0) lands on target at once, which makes an inspector edit
            // show at dt == 0: Edit-mode writers call Facial::SetChannel with blendSeconds <= 0, which sets
            // current at call time. A blending channel is frozen at dt <= 0 instead of snapped: animDt is 0
            // while the transport is paused, and snapping would complete every in-flight blend and fade-out
            // the moment the user pauses - pause must not change the picture. Unlike HeadTrackSystem, whose
            // snap is invisible because its target comes from frozen inputs, here the target is the end of
            // a blend.
            if (c.blendSec <= 0.0f)
            {
                c.current = c.target;
            }
            else if (dt > 0.0f)
            {
                c.current += (c.target - c.current) * (1.0f - std::exp(-dt / c.blendSec));
                if (std::fabs(c.target - c.current) < 1e-4f) c.current = c.target;
            }

            if (c.state == 2 && c.current <= 1e-4f)
            {
                c       = FA::Channel{};
                fa.channelName[k][0] = '\0';
            }
        }

        // ---- Blink slot resolution ----
        // The machine's output is not a channel, so it has its own one-entry cache. The name compare makes
        // an inspector edit of blinkMorph self-healing without the editor invalidating anything. A miss is
        // cached as kResolvedMissing so an unmatched blink name does not re-scan the table every frame.
        if (assetChanged || fa.blinkSlot == FA::kUnresolved ||
            (fa.blinkSlot >= 0 &&
             static_cast<std::uint32_t>(fa.blinkSlot) >= skel.morphTargetCount) ||
            !NameEq(fa.blinkResolvedName, fa.blinkMorph))
        {
            fa.blinkSlot = Facial::FindMorphSlot(skel, fa.blinkMorph);
            Facial::CopyName(fa.blinkResolvedName, fa.blinkMorph);
            if (fa.blinkSlot < 0) fa.blinkSlot = FA::kResolvedMissing;   // cache the MISS
            if (fa.blinkSlot < 0 && fa.blinkMorph[0])
            {
                const std::uint32_t key = Fnv1a(fa.blinkMorph, FA::kNameLen) ^ 0x85EBCA6Bu;
                if (ShouldWarnOnce(key))
                    LOG_WARNING("Facial: blink morph '%s' is not in this skeleton "
                                "(%u targets) - auto-blink is inert.",
                                fa.blinkMorph, skel.morphTargetCount);
            }
        }

        // ---- Group fold -> resolvedOut (rebuilt every call, dt == 0 included) ----
        fa.resolvedCount = 0;
        // Authored first (Override) - straight from the list.
        for (const FA::AuthoredEntry& a : fa.authored)
            if (a.slot >= 0) EmitOverride(fa, static_cast<std::uint16_t>(a.slot), a.weight);
        // Tracked: after Authored, before the groups a script drives. FaceTrackingSystem::Resolve
        // resolved these slots against this skeleton this frame; entries at alpha <= 1e-4 never reach
        // the list.
        for (const FA::Tracked& t : fa.tracked)
            if (t.slot < skel.morphTargetCount) EmitTracked(fa, t.slot, Sat(t.weight), Sat(t.alpha));
        static constexpr FacialGroup kOverrideOrder[] = {
            FacialGroup::Expression, FacialGroup::Viseme, FacialGroup::Manual
        };
        for (FacialGroup g : kOverrideOrder)
        {
            const std::uint8_t gi = static_cast<std::uint8_t>(g);
            for (int k = 0; k < FA::kMaxChannels; ++k)
            {
                const FA::Channel& c = fa.channels[k];
                if (c.state == 0 || c.group != gi || c.slot < 0) continue;
                EmitOverride(fa, static_cast<std::uint16_t>(c.slot), c.current);
            }
        }
        // Blink is Max, and last, so a blink still closes fully over a partial squint set by SetEyeClose.
        // Emitted only when it raises something: an unconditional 0 would force the morph off,
        // clobbering a morph clip that drives the same target.
        {
            const std::uint8_t gi = static_cast<std::uint8_t>(FacialGroup::Blink);
            for (int k = 0; k < FA::kMaxChannels; ++k)
            {
                const FA::Channel& c = fa.channels[k];
                if (c.state == 0 || c.group != gi || c.slot < 0 || c.current <= 0.0f) continue;
                EmitMax(fa, static_cast<std::uint16_t>(c.slot), c.current);
            }
            if (fa.blinkWeightCur > 0.0f && fa.blinkSlot >= 0)
                EmitMax(fa, static_cast<std::uint16_t>(fa.blinkSlot), fa.blinkWeightCur);
        }

        // The CPU is the only clamp in the pipeline: Skin.cs.hlsl has no upper bound and only tests
        // w > 0.001f.
        for (std::uint32_t r = 0; r < fa.resolvedCount; ++r)
        {
            fa.resolvedOut[r].weight = Sat(fa.resolvedOut[r].weight);
            fa.resolvedOut[r].alpha  = Sat(fa.resolvedOut[r].alpha);
            fa.resolvedOut[r].floor  = Sat(fa.resolvedOut[r].floor);
        }
        // Consumed: FaceTrackingSystem::Resolve writes both afresh every frame, so a binding
        // that went away (its component removed) leaves nothing behind after one frame.
        fa.tracked.clear();
        fa.trackingHoldsBlink = false;

        ++facialEntities;
    }

    Anim::AnimProfile::Get().facialEntities = facialEntities;
    // facialSlots is accumulated by BuildSkinJobs: what actually reached the pool.
}
