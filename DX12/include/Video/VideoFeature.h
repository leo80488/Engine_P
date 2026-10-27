#pragma once

// The Video module's render feature (src/Video/VideoFeature.cpp): the screen overlay and the
// world-space quads. The module adds it from IEngineModule::RegisterRenderFeatures.

#include <memory>

class IRenderFeature;

std::unique_ptr<IRenderFeature> CreateVideoFeature();
