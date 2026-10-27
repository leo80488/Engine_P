#pragma once
// Platform/Subscription.h - a handle to one entry in one of Plat::Window's hook
// lists (a message filter, a capture predicate, a close handler, an event
// handler). Destroying it, or Reset(), removes the entry.
//
// Every installer gets its own entry (no single-slot hook a second installer
// overwrites), and a subscription lives in its owner, so teardown order is member
// order. A subscription must not outlive the Window it came from (App declares
// the window first, so every member's subscriptions go before it).
// Small on purpose: EditorLayer.h holds subscriptions and must not pull in the
// window.

#include <cstdint>
#include <utility>

namespace Plat
{
    class Window;

    class Subscription
    {
    public:
        Subscription() = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& o) noexcept
            : m_window(std::exchange(o.m_window, nullptr)), m_id(std::exchange(o.m_id, 0u)) {}
        Subscription& operator=(Subscription&& o) noexcept
        {
            if (this != &o)
            {
                Reset();
                m_window = std::exchange(o.m_window, nullptr);
                m_id     = std::exchange(o.m_id, 0u);
            }
            return *this;
        }
        ~Subscription() { Reset(); }

        // Remove the entry now (idempotent). Defined in Window.cpp, which has the
        // Window definition this header must not include.
        void Reset() noexcept;

        explicit operator bool() const noexcept { return m_window != nullptr; }

    private:
        friend class Window;
        Subscription(Window* window, std::uint32_t id) noexcept : m_window(window), m_id(id) {}

        Window*       m_window = nullptr;
        std::uint32_t m_id     = 0;
    };
}
