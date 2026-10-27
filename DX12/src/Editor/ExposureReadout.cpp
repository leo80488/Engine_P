#include "Editor/ExposureReadout.h"

#include "RenderGraph/RenderPass/AutoExposurePass.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>

void EditorUI::DrawExposureReadout(AutoExposurePass& ae, bool autoExposureOn)
{
    ae.SetDebugReadbackEnabled(true);
    const float e = ae.GetLastExposure();
    if (!(e > 0.0f))
    {
        ImGui::TextDisabled("  Exposure -  (waiting for readback)");
        return;
    }
    ImGui::Text("  Exposure %.3fx (%+.2f EV)%s%s", e, ae.GetLastExposureEV(),
                autoExposureOn ? "" : "  [manual]", ae.IsHolding() ? "  [held]" : "");

    std::array<uint32_t, AutoExposurePass::kHistogramBins> bins{};
    float minLog = 0.0f, maxLog = 0.0f;
    if (!ae.GetLastHistogram(bins, minLog, maxLog)) return;

    std::array<float, AutoExposurePass::kHistogramBins> values{};
    uint32_t peak = 1;
    for (uint32_t b : bins) peak = std::max(peak, b);
    for (size_t i = 0; i < bins.size(); ++i) values[i] = static_cast<float>(bins[i]) / static_cast<float>(peak);
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "log2 luma %.1f .. %.1f", minLog, maxLog);
    ImGui::PlotHistogram("##exposureHistogram", values.data(), static_cast<int>(values.size()), 0, overlay,
                         0.0f, 1.0f, ImVec2(0.0f, 48.0f));
}
