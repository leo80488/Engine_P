#pragma once
// The -chainpreview_* harness flags, parsed and handed to
// ChainPhysicsPreviewWindow's Harness* setters at boot.
// WITH_HARNESS only: the definition compiles to nothing in Retail.

namespace Sys { class CommandLine; }

namespace Editor::ChainPreviewHarness
{
    // Called once, on the editor host, from Editor::PreviewHarness::ParseBootFlags -
    // after -preview_asset and before -preview_asset_appear_at, because the order
    // of the refusals and their log lines is part of the packs' contract. A bare
    // value flag refuses the run (exit 12); a malformed one is an ENGINE_CHECK and
    // a documented default.
    void ApplyBootFlags(const Sys::CommandLine& pcl);
}
