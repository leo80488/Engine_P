#include "Renderer/ViewBudget.h"

#include "Graphics/MemoryPolicy.h"
#include "System/Log.h"

#include <algorithm>
#include <atomic>

namespace ViewBudget
{
namespace
{
    // A secondary view costs about 175 bytes a pixel of its own (histories, working sets, pass data); the first
    // one also grows the shared alias heaps, about 95 bytes a pixel more. Measured on a 1080p view of a lit scene.
    constexpr uint64_t kViewBytesPerPixel      = 180;
    constexpr uint64_t kFirstViewExtraPerPixel = 90;

    constexpr double   kAdmitFraction   = 0.90;   // plan up to this much of the budget
    constexpr double   kOverFraction    = 0.95;   // past this the ladder steps down
    constexpr double   kCalmFraction    = 0.70;   // under this it may step back up
    constexpr uint32_t kOverFrames      = 2;      // consecutive samples over before a step
    constexpr uint32_t kCalmFrames      = 300;    // consecutive calm samples before a step back
    constexpr uint32_t kHoldFrames      = 60;     // frames between two steps (a release takes three to show)

    std::atomic<uint8_t>  g_stage{ 0 };
    std::atomic<uint32_t> g_refusals{ 0 };
}

bool Admit(uint32_t width, uint32_t height, const VideoMemory::Info& vm, uint32_t readyViews)
{
    if (MemoryPolicy::IgnoreViewBudget() || vm.budget == 0)
        return true;
    const uint64_t pixels = uint64_t(width ? width : 1920u) * uint64_t(height ? height : 1080u);
    const uint64_t cost   = pixels * (kViewBytesPerPixel + (readyViews == 0 ? kFirstViewExtraPerPixel : 0));
    const uint64_t limit  = static_cast<uint64_t>(double(vm.budget) * kAdmitFraction);
    const bool suspended  = Current() == Stage::Suspended;
    if (!suspended && vm.usage + cost <= limit)
        return true;
    g_refusals.fetch_add(1, std::memory_order_relaxed);
    LOG_WARNING("[BUDGET] a %ux%u view is refused: %llu MB used + about %llu MB for it is over %.0f%% of the "
                "%llu MB video memory budget%s", width ? width : 1920u, height ? height : 1080u,
                (unsigned long long)(vm.usage >> 20), (unsigned long long)(cost >> 20), kAdmitFraction * 100.0,
                (unsigned long long)(vm.budget >> 20), suspended ? " (the ladder has the views suspended)" : "");
    return false;
}

Stage Step(const VideoMemory::Info& vm, uint32_t readyViews)
{
    static uint64_t s_frame = 0, s_lastStep = 0;
    static uint32_t s_overRun = 0, s_calmRun = 0;
    static bool     s_loggedNoViews = false;
    ++s_frame;
    if (MemoryPolicy::IgnoreViewBudget() || vm.budget == 0)
        return Current();

    const bool over = double(vm.usage) > double(vm.budget) * kOverFraction;
    const bool calm = double(vm.usage) < double(vm.budget) * kCalmFraction;
    s_overRun = over ? s_overRun + 1 : 0;
    s_calmRun = calm ? s_calmRun + 1 : 0;

    Stage stage = Current();
    const bool held = s_frame - s_lastStep < kHoldFrames;
    if (s_overRun >= kOverFrames && !held)
    {
        if (readyViews == 0 || stage == Stage::Suspended)
        {
            if (!s_loggedNoViews)
                LOG_WARNING("[BUDGET] %llu MB used is over the %llu MB budget and no secondary view is left to give back",
                            (unsigned long long)(vm.usage >> 20), (unsigned long long)(vm.budget >> 20));
            s_loggedNoViews = true;
        }
        else
        {
            stage = static_cast<Stage>(static_cast<uint8_t>(stage) + 1);
            s_lastStep = s_frame;
            s_overRun = 0;
            LOG_WARNING("[BUDGET] over budget (%llu MB used / %llu MB): secondary views go to stage %u (%s)",
                        (unsigned long long)(vm.usage >> 20), (unsigned long long)(vm.budget >> 20),
                        static_cast<unsigned>(stage), stage == Stage::HalfSize ? "half size" : "suspended");
        }
    }
    else if (s_calmRun >= kCalmFrames && !held && stage != Stage::Normal)
    {
        stage = static_cast<Stage>(static_cast<uint8_t>(stage) - 1);
        s_lastStep = s_frame;
        s_calmRun = 0;
        s_loggedNoViews = false;
        LOG_INFO("[BUDGET] %llu MB used / %llu MB budget: secondary views go back to stage %u",
                 (unsigned long long)(vm.usage >> 20), (unsigned long long)(vm.budget >> 20),
                 static_cast<unsigned>(stage));
    }
    g_stage.store(static_cast<uint8_t>(stage), std::memory_order_relaxed);
    return stage;
}

Stage Current() { return static_cast<Stage>(g_stage.load(std::memory_order_relaxed)); }

uint32_t Scaled(uint32_t pixels)
{
    return Current() == Stage::Normal ? pixels : (std::max)(16u, pixels / 2u);
}

uint32_t Refusals() { return g_refusals.load(std::memory_order_relaxed); }
}
