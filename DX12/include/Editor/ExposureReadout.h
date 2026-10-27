#pragma once

// The editor's exposure readout: the live exposure number and the metering histogram.

class AutoExposurePass;

namespace EditorUI
{
    // Turns the pass's debug readback on and draws, in the current ImGui window, the last exposure (the
    // multiplier, its stops, whether it is manual or held) and a plot of the last metered frame's histogram
    // over log2 luma. Call it every frame the panel is visible: the readback decays when not renewed.
    void DrawExposureReadout(AutoExposurePass& ae, bool autoExposureOn);
}
