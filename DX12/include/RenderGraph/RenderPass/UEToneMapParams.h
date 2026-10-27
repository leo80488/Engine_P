#pragma once
#include <DirectXMath.h>

// Optional scene-linear grading in ACEScg (the ACES working primaries). The
// importer folds the global UE controls into each tonal range; W multipliers are
// already applied to RGB. Mirrors UEToneMapData in UEToneMap.hlsli.
struct alignas(16) UEToneMapParams
{
    DirectX::XMFLOAT4 shadowsSaturation = { 1,1,1,0 };
    DirectX::XMFLOAT4 shadowsContrast = { 1,1,1,0 };
    DirectX::XMFLOAT4 shadowsGamma = { 1,1,1,0 };
    DirectX::XMFLOAT4 shadowsGain = { 1,1,1,0 };
    DirectX::XMFLOAT4 shadowsOffset = { 0,0,0,0 };
    DirectX::XMFLOAT4 midtonesSaturation = { 1,1,1,0 };
    DirectX::XMFLOAT4 midtonesContrast = { 1,1,1,0 };
    DirectX::XMFLOAT4 midtonesGamma = { 1,1,1,0 };
    DirectX::XMFLOAT4 midtonesGain = { 1,1,1,0 };
    DirectX::XMFLOAT4 midtonesOffset = { 0,0,0,0 };
    DirectX::XMFLOAT4 highlightsSaturation = { 1,1,1,0 };
    DirectX::XMFLOAT4 highlightsContrast = { 1,1,1,0 };
    DirectX::XMFLOAT4 highlightsGamma = { 1,1,1,0 };
    DirectX::XMFLOAT4 highlightsGain = { 1,1,1,0 };
    DirectX::XMFLOAT4 highlightsOffset = { 0,0,0,0 };
    DirectX::XMFLOAT4 ranges = { .09f,.5f,1.f,0.f };
    DirectX::XMFLOAT4 film = { .88f,.55f,.26f,0.f };
    DirectX::XMFLOAT4 controls = { 0.f,.04f,0.f,0.f }; // UE grading enabled, white clip, vignette strength (works without the UE grading)
};
static_assert(sizeof(UEToneMapParams)==288);
