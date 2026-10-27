#pragma once

// MemoryPolicy - the harness arms that put the engine's lazy GPU memory back to its eager shape, so a pack
// can prove the lazy path is what saves the bytes (scenario_run --reverse). Each reads its flag once; all are
// false in Retail.
namespace MemoryPolicy
{
    // -mem_eager_transients: ReSTIR's transient arena is reserved at boot and the translucent front-layer
    // targets are backed with the graph, as if the features were on.
    bool EagerTransients();

    // -mem_view_committed: a secondary view's pass transients (RT reflections, AO) are committed per view, not
    // placed in the views' shared scratch heap.
    bool ViewTransientsCommitted();

    // -mem_keep_view_heaps: the views' alias heaps stay once made, also when no secondary view is left.
    bool KeepViewHeaps();

    // -mem_no_view_budget: ViewBudget admits every secondary view and never steps its ladder.
    bool IgnoreViewBudget();

    // -mem_no_residency_priority: render targets and depth keep the default residency priority.
    bool NoResidencyPriority();
}
