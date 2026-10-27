#pragma once

// Compute root-signature slot names for the SSR passes.
// The SSR sub-passes bind into the SHARED compute root signature (space2), so these
// are aliases of RHI::Slots::Cmp in Graphics/RootSignatureLayout.h, not a separate
// layout: a slot renumber cannot leave SSR behind.

#include <cstdint>

#include "Graphics/RootSignatureLayout.h"

namespace SSR { namespace RootSig {

inline constexpr auto kCB      = RHI::Slots::Cmp::CB;
inline constexpr auto kSRV_T0  = RHI::Slots::Cmp::SRV_t0;
inline constexpr auto kSRV_T1  = RHI::Slots::Cmp::SRV_t1;
inline constexpr auto kSRV_T2  = RHI::Slots::Cmp::SRV_t2;
inline constexpr auto kSRV_T3  = RHI::Slots::Cmp::SRV_t3;
inline constexpr auto kSRV_T4  = RHI::Slots::Cmp::SRV_t4;
inline constexpr auto kSRV_T5  = RHI::Slots::Cmp::SRV_t5;
inline constexpr auto kSRV_T6  = RHI::Slots::Cmp::SRV_t6;
inline constexpr auto kSRV_T7  = RHI::Slots::Cmp::SRV_t7;
inline constexpr auto kUAV_U0  = RHI::Slots::Cmp::UAV_u0;
inline constexpr auto kUAV_U1  = RHI::Slots::Cmp::UAV_u1;
inline constexpr auto kUAV_U2  = RHI::Slots::Cmp::UAV_u2;
inline constexpr auto kUAV_U3  = RHI::Slots::Cmp::UAV_u3;

}} // namespace SSR::RootSig
