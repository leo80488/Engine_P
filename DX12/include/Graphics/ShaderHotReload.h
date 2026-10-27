#pragma once
// Shader hot reload: App's shader watcher turned into a reload that keeps the old shaders
// on a compile error and says what happened in a receipt (System/HotReloadReceipt.h).
// Per batch of changes (quiet for kDebounceMs, so an editor's truncate-then-write save is
// one batch):
//   1. a receipt opens at the first change;
//   2. ShaderRecipes::RunPreflight compiles every loaded recipe the edit reaches - nothing
//      is cleared yet (no recipe reached: the receipt closes as not_in_use);
//   3. a new compile failure closes the receipt as compile_failed and stops: the old
//      bytecode and every PSO built from it stay in use;
//   4. otherwise the new .ishdr files are committed and the apply callback runs
//      (FlushAndWait + Renderer::ReloadShaders);
//   5. the frame that iteration draws is the first with them (effectiveFrame: its index -
//      frames.jsonl's and the log's `frame`, Engine.Frame() during it); when it is over the
//      receipt closes: ok, restart_required (a holder kept the old version: its pass has no
//      hot-reload support) or errors_after_reload (an ERROR was logged meanwhile, e.g. a PSO
//      that did not rebuild).

#include "Graphics/ShaderRecipes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace HotReload
{
    // A shader source changed where no watcher looks (e.g. the Material Editor's live graph
    // shader of a harness copy outside shaders/ and asset/Shaders/): App folds it into the next
    // frame's batch like a watched change - the same pre-flight, the same receipt.
    // Thread-safe. @p path: project-relative or absolute.
    void RequestShaderReload(const std::string& path);
    std::vector<std::string> TakeRequestedShaderReloads();
    // The roots a shader watcher really watches ("shaders/", "asset/Shaders/"), noted
    // by App when one opens - a project without asset/Shaders at boot has none there,
    // so a file written into it later must be requested. Thread-safe.
    void NoteWatchedShaderRoot(const std::string& rootedPrefix);
    bool IsWatchedShaderPath(const std::string& rootedPath);

    class ShaderReloader
    {
    public:
        static constexpr double kDebounceMs = 120.0;

        // What the watchers reported this frame: root-relative names (what the
        // receipt shows) and the same files as absolute paths (matched against
        // the recipes). The first change of a batch opens its receipt.
        void Note(const std::vector<std::string>& rooted, const std::vector<std::string>& absolute);

        // Once per frame, before the frame's rendering, with the index of the
        // frame this iteration draws (App's ctx.frame = frames presented so far).
        void Tick(std::uint64_t presentedFrames, const std::function<void()>& apply);

    private:
        struct Batch
        {
            std::uint64_t            id = 0;   // receipt; 0 = no batch
            std::vector<std::string> rooted, absolute;
            double                   lastMs = 0;
        };
        void Finish();

        Batch                    m_batch;               // collecting
        bool                     m_inFlight = false;    // applied, waiting for its effective frame
        std::uint64_t            m_flightId = 0;
        std::uint64_t            m_effectiveFrame = 0;
        std::uint64_t            m_generation = 0;
        std::uint64_t            m_startSeq = 0;        // log ring seq before apply
        std::uint32_t            m_written = 0;
        double                   m_applyMs = 0;
        ShaderRecipes::Preflight m_pf;
    };
}
