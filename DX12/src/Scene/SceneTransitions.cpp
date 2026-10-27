#include "Scene/ISceneTransition.h"
#include "UI/UIDrawList.h"

#include <unordered_map>
#include <cstdint>

// Built-in scene-transition effects and the name -> factory registry.
// FadeTransition is the reference effect and the fallback. Adding an effect = one more
// class (here or in game code) + a RegisterTransition call; nothing else changes.

namespace Scene
{
namespace
{
    inline float   Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    inline uint8_t ToByte(float a)  { return static_cast<uint8_t>(Clamp01(a) * 255.f + 0.5f); }

    // ---- FadeTransition: fade to a solid colour (default black) ----
    class FadeTransition final : public ISceneTransition
    {
    public:
        const char* Name() const override { return "fade"; }

        void OnBegin(const TransitionParams& p) override { m_r = p.r; m_g = p.g; m_b = p.b; }

        void Render(UI::UIDrawList& dl, const TransitionFrame& f) override
        {
            const float w = static_cast<float>(f.viewportW);
            const float h = static_cast<float>(f.viewportH);
            dl.AddRectFilled(UI::Vec2{ 0.f, 0.f }, UI::Vec2{ w, h },
                             UI::Color32(m_r, m_g, m_b, ToByte(f.cover)));
        }

    private:
        uint8_t m_r = 0, m_g = 0, m_b = 0;
    };

    // ---- WipeTransition: a solid bar sweeping left -> right ----
    // Cover: the bar grows in from the left edge. Reveal: it exits off the right edge, so the
    // new scene is uncovered left to right. Uses only TransitionFrame, no extra state.
    class WipeTransition final : public ISceneTransition
    {
    public:
        const char* Name() const override { return "wipe"; }

        void OnBegin(const TransitionParams& p) override { m_r = p.r; m_g = p.g; m_b = p.b; }

        void Render(UI::UIDrawList& dl, const TransitionFrame& f) override
        {
            const float w = static_cast<float>(f.viewportW);
            const float h = static_cast<float>(f.viewportH);
            const UI::Color32 col(m_r, m_g, m_b, 255);

            float x0, x1;
            if (f.phase == TransitionPhase::Reveal)
            {
                // cover 1 -> 0: the bar holds the right portion and recedes right, revealing the new
                // scene from the left.
                x0 = (1.f - Clamp01(f.cover)) * w;
                x1 = w;
            }
            else
            {
                // Cover/Loading: black grows from the left edge to full width.
                x0 = 0.f;
                x1 = Clamp01(f.cover) * w;
            }
            if (x1 > x0)
                dl.AddRectFilled(UI::Vec2{ x0, 0.f }, UI::Vec2{ x1, h }, col);
        }

    private:
        uint8_t m_r = 0, m_g = 0, m_b = 0;
    };

    std::unordered_map<std::string, TransitionFactory>& Registry()
    {
        static std::unordered_map<std::string, TransitionFactory> s_reg;
        return s_reg;
    }

    void EnsureBuiltins()
    {
        static bool s_done = false;
        if (s_done) return;
        // Built-ins register on the first CreateTransition, so they overwrite a game factory
        // registered earlier under "fade" or "wipe".
        s_done = true;
        Registry()["fade"] = [] { return std::unique_ptr<ISceneTransition>(std::make_unique<FadeTransition>()); };
        Registry()["wipe"] = [] { return std::unique_ptr<ISceneTransition>(std::make_unique<WipeTransition>()); };
    }
}

void RegisterTransition(const std::string& name, TransitionFactory factory)
{
    if (!name.empty() && factory)
        Registry()[name] = std::move(factory);
}

std::unique_ptr<ISceneTransition> CreateTransition(const TransitionParams& params)
{
    EnsureBuiltins();
    auto& reg = Registry();

    if (!params.type.empty())
    {
        auto it = reg.find(params.type);
        if (it != reg.end() && it->second)
            return it->second();
    }
    // Unknown / empty type -> fade fallback.
    auto it = reg.find("fade");
    if (it != reg.end() && it->second)
        return it->second();
    return std::make_unique<FadeTransition>();
}

} // namespace Scene
