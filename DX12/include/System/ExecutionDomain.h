#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <algorithm>

namespace Sys {
// Exclusive publisher issues/retires tokens. A token can be explicitly handed
// to another job, but the execution scope must end before suspension/handoff.
// Ordinary jobs inherit no authority: this TLS is separate from ThreadCtx.
class ExecutionDomain {
    struct Gate { std::atomic<void*> entered{nullptr}; };
    struct State {
        const ExecutionDomain* domain;
        std::shared_ptr<Gate> gate;
        uint64_t generation;
        uint32_t slot;
        uint32_t uploadContext;
        std::atomic<unsigned> status{0}; // available, entered, retired
        State(const ExecutionDomain* d, std::shared_ptr<Gate> gate, uint64_t g, uint32_t s, uint32_t u)
            : domain(d), gate(std::move(gate)), generation(g), slot(s), uploadContext(u) {}
    };
    inline static thread_local State* current = nullptr;
public:
    class Scope;
    class Token {
    public:
        Token() = default;

    private:
        explicit Token(std::shared_ptr<State> s) : state(std::move(s)) {}
        friend class ExecutionDomain;
        friend class Scope;

        std::shared_ptr<State> state;
    };
    class Scope {
    public:
        explicit Scope(const Token& token) : state(token.state), thread(std::this_thread::get_id()) {
            if (current || !state) throw std::logic_error("Invalid or nested execution scope");
            unsigned expected = 0;
            if (!state->status.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
                throw std::logic_error("Execution token is busy or retired");
            void* vacant = nullptr;
            if (!state->gate->entered.compare_exchange_strong(vacant, state.get(), std::memory_order_acq_rel)) {
                state->status.store(0, std::memory_order_release);
                throw std::logic_error("Another frame owns this execution domain");
            }
            current = state.get();
        }
        Scope(const Scope&) = delete;
        ~Scope() {
            if (thread != std::this_thread::get_id() || current != state.get()) std::terminate();
            current = nullptr;
            state->gate->entered.store(nullptr, std::memory_order_release);
            state->status.store(0, std::memory_order_release);
        }
    private:
        std::shared_ptr<State> state;
        std::thread::id thread;
    };
    ExecutionDomain() = default;
    ExecutionDomain(const ExecutionDomain&) = delete;
    ~ExecutionDomain() {
        try {
            Retire();
            for (const auto& frame : frames) if (auto s = frame.lock()) RetireState(s);
        } catch (...) { std::terminate(); }
    }
    Token Issue(uint64_t generation, uint32_t slot, uint32_t uploadContext = 1) {
        if (uploadContext > 1) throw std::logic_error("Invalid upload context lane");
        PruneFrames();
        if (!frames.empty()) throw std::logic_error("Cannot mix replacement and retained frame tokens");
        auto next = std::make_shared<State>(this, gate, generation, slot, uploadContext);
        Retire();
        active = std::move(next);
        enabled.store(true, std::memory_order_release);
        return Token(active);
    }
    // Retained frame authority: issuing F+1 does not invalidate a suspended F.
    // The shared gate still permits exactly one device owner at a time.
    Token IssueFrame(uint64_t generation, uint32_t slot, uint32_t uploadContext = 1) {
        if (uploadContext > 1 || active) throw std::logic_error("Invalid retained frame token issuance");
        PruneFrames();
        for (const auto& weak : frames) if (auto s = weak.lock()) {
            if (s->slot == slot && (s->generation != generation || s->uploadContext == uploadContext))
                throw std::logic_error("Frame upload slot still leased");
        }
        auto state = std::make_shared<State>(this, gate, generation, slot, uploadContext);
        frames.emplace_back(state);
        enabled.store(true, std::memory_order_release);
        return Token(std::move(state));
    }
    void Retire(const Token& token) {
        if (!token.state || token.state->domain != this) throw std::logic_error("Foreign frame token");
        RetireState(token.state);
    }
    void Retire() {
        if (!active) return;
        RetireState(active);
        active.reset();
    }
    bool Enabled() const { return enabled.load(std::memory_order_acquire); }
    static bool HasActiveScope() { return current != nullptr; }
    static uint64_t CurrentGeneration() { return current ? current->generation : 0; }
    bool Owns() const { return current && current->domain == this; }
    uint32_t UploadLane(uint32_t fallback) const { return Owns() ? current->uploadContext : fallback; }
    bool Matches(uint64_t generation, uint32_t slot) const {
        return Owns() && current->generation == generation && current->slot == slot;
    }
private:
    static void RetireState(const std::shared_ptr<State>& state) {
        unsigned expected = 0;
        if (!state->status.compare_exchange_strong(expected, 2, std::memory_order_acq_rel) && expected != 2)
            throw std::logic_error("Cannot retire active execution scope");
    }
    void PruneFrames() {
        std::erase_if(frames, [](const auto& weak) {
            auto state = weak.lock();
            return !state || state->status.load(std::memory_order_acquire) == 2;
        });
    }
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    std::vector<std::weak_ptr<State>> frames; // exclusive publisher; tokens own their leases
    std::shared_ptr<State> active; // exclusive publisher only
    std::atomic<bool> enabled{false};
};
}
