#pragma once
// Editor::PreviewHarness - the editor host's harness flags: every editor-only -preview_*
// flag, the editor tools' (-mesheditor_*, -skeleton_editor*, -texviewer_*, -mateditor_*,
// -nodeeditor_smoke*) and -chainpreview_* (Editor/ChainPhysicsPreviewHarness.cpp,
// called from ParseBootFlags), the steps those flags schedule inside the frame loop, and
// the files they put back at shutdown.
//
// App owns it and calls each entry point at a fixed point of boot / the frame loop /
// shutdown; that fixes the order of refusals, log lines and frame-timed steps. App hands
// in the EditorLayer, FramePipeline's render-owner flag and its DrainFrameWork.
// WITH_HARNESS only: the definitions compile to nothing in Retail.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class EditorLayer;
class Renderer;
struct GameModeContext;
namespace Diag { enum class DrainCause : uint8_t; }   // Diagnostics/OwnerAccounting.h

namespace Editor
{
    class PreviewHarness
    {
    public:
        PreviewHarness(EditorLayer& editorLayer, const bool& renderOwnerEnabled,
                       std::function<void(Diag::DrainCause)> inDrainFrameWork)
            : m_editorLayer(editorLayer), m_renderOwnerEnabled(renderOwnerEnabled),
              drainFrameWork(std::move(inDrainFrameWork)) {}

        // BOOT, before anything reads an asset: put back every file a run that died left
        // rewritten (the sidecars ArmHarnessFileRestore writes) and every write time it left
        // touched (-preview_touch_at's .touch sidecar).
        static std::size_t RestorePendingRewritesAtBoot();
        // BOOT, editor host, after WireEditor and PrimeViewportLayout.
        void ParseBootFlags();
        // LOOP TOP, beside the asset watcher's poll - the one place that knows the
        // watcher-relative <-> root-relative mapping: touch / appear / rewrite.
        void DeliverDue(const GameModeContext& ctx,
                        const char* const (&kShaderWatchPrefix)[2],
                        const char* const (&kAssetWatchPrefix)[2]);
        // EVERY FRAME, after the editor camera and its ortho override.
        void PinParityCamera(Renderer& renderer) const;
        // LOOP TAIL (ctx.frame >= 1): the one-shot editor actions.
        void AfterFrame();
        // ShutdownAllSystems, while the World is whole: undo every file change.
        void Shutdown();

    private:
        // ---- Files a harness run overwrites, and puts back ----
        struct HarnessFileRestore
        {
            std::string                     path;       // absolute, on disk
            std::vector<std::uint8_t>       bytes;      // the ORIGINAL content
            std::filesystem::file_time_type time{};
            bool                            timeValid = false;
            std::string                     sidecar;    // "" if it could not be written
            std::string                     flag;       // who armed it, for the log
            // The SUBJECT of the restore line, e.g. "asset rewrite" ->
            // "[PREVIEW] asset rewrite: restored the original N bytes of '...'".
            // A field rather than a formatted-in constant because two packs pin
            // that sentence verbatim and a shared cleanup loop must still be able
            // to produce each flag's own wording.
            std::string                     tag;
        };

        // Read @p dstFull's current bytes, write them to a crash-safe sidecar and record the
        // in-memory restore. Call BEFORE the first write to the file. Idempotent per path (a
        // second call is ignored, so the ORIGINAL content is what gets put back). @p flag is the
        // harness flag that armed it, @p tag the subject of its restore line. False only when
        // the file could not be read - the caller then proceeds unprotected or refuses.
        bool ArmHarnessFileRestore(const std::string& dstFull, const char* flag,
                                   const char* tag);

    public:
        // ---- -preview_parity ----
        // Non-zero = the A/B parity run is active; the value is the SQUARE viewport the main
        // pipeline renders into. Forced, not configured: compare_images refuses two sizes, the
        // preview is square (128/256/384/512), and the editor viewport is what the docked
        // panels leave (under -hidden, from a layout primed against a 0x0 display). Cropping
        // would not do: a 16:9 projection puts the sphere's silhouette on other sub-pixel
        // centres with another screen-space radius, charging a rasterisation difference to the
        // shading. Only the same square viewport on both sides gives failures that mean what
        // they say.
        std::uint32_t     m_previewParityPx = 0;
        // -preview_pip_parity. True = m_previewParityPx was set by the SECONDARY-VIEW arm, not
        // -preview_size: the main camera is then pinned to Preview::kLightsParityCamera instead
        // of the studio basis. The PiP's camera entity and this pin read the SAME constant, so
        // the two pipelines share an eye, a forward and a lens.
        bool              m_previewPipParity = false;

    private:
        EditorLayer& m_editorLayer;
        // FramePipeline's render-owner flag (RenderOwnerEnabledFlag): while set, park the lane
        // with drainFrameWork before a step that changes state the render half reads.
        const bool&  m_renderOwnerEnabled;
        // FramePipeline::DrainFrameWork, bound by App.
        std::function<void(Diag::DrainCause)> drainFrameWork;

        // ---- Run-loop latches: set by ParseBootFlags, drained by AfterFrame ----
        // Material-preview harness flags: each is a one-shot editor action that must land at a
        // frame boundary (like -play_frames), not mid-frame. WITH_EDITOR builds only (Game.exe
        // has no EditorLayer, and these flags drive editor panels), and App parses them only when
        // Sys::IsEditorHost(), so a host that is not the authoring tool never acts on them.
        std::string previewMaterial;
        int         previewStartFrame = 0;
        bool        previewOpenSent   = false;
        // -preview_parity=<fixture>. Applied at the same post-boot point as
        // -preview_material, and for the same reason: it clears and rebuilds the
        // World, which must not race the asset scan that resolves the boot scene.
        std::string previewParityFixture;
        bool        previewParitySent = false;
        // -preview_lights_parity=<baseline|lights>, applied at the same post-boot point for the
        // same reason: it clears and rebuilds the World, which must not race the asset scan
        // that resolves the boot scene.
        std::string previewLightsParityArm;
        bool        previewLightsParitySent = false;

        // ---- Files a harness run touches or creates, and puts back ----
        // -preview_touch_at BUMPS A REAL FILE'S mtime (a simulated event alone
        // cannot prove the content-addressed tile filenames: the dependency has to
        // genuinely change or the re-render hashes to the same name). A scenario
        // run must not leave the working tree modified, so the original write time
        // is put back in ShutdownAllSystems. Empty = nothing was touched.
        std::string                        m_previewTouchRestorePath;
        std::filesystem::file_time_type    m_previewTouchRestoreTime{};
        bool                               m_previewTouchRestoreValid = false;
        // The crash-safe copy of that time (a .touch sidecar beside the rewrite
        // ones): a run that dies before Shutdown() leaves it, and the next boot's
        // RestorePendingRewritesAtBoot puts the time back. "" = none written.
        std::string                        m_previewTouchSidecar;

        // -preview_asset_appear_at CREATED A FILE: the touch's discipline one step further -
        // the file did not exist before the run, so "restore" means DELETE. Empty = nothing was
        // created; only a path this process actually copied is put here, so the removal cannot
        // reach a tracked asset.
        std::string                        m_previewAppearRemovePath;

        // ---- Files a harness run overwrites, and puts back ----
        // -preview_asset_rewrite_at overwrites an existing file; -preview_asset_edit overwrites
        // an asset-tree .imat TWICE per run (a dull "before" at the first harness tick, a
        // bright "after" at -preview_edit_at). The file must be byte-identical after the run,
        // so the restore writes back the bytes read before the first overwrite, plus the
        // original mtime (as the touch restores one).
        //
        // In memory AND in a sidecar written BEFORE the overwrite: a -maxseconds watchdog exit,
        // a crash or a kill would otherwise leave the TRACKED fixture holding other bytes, the
        // working tree dirty, and every later run silently measuring the rewritten case.
        // RestorePendingRewritesAtBoot replays any sidecar at the NEXT boot. Sidecars live
        // under the writable data root in a directory this feature owns, never in the asset
        // tree, so one left by a dead run cannot pass for a fixture. Both flags arm this one
        // mechanism through ArmHarnessFileRestore.
        std::vector<HarnessFileRestore>    m_harnessFileRestores;
    };
}
