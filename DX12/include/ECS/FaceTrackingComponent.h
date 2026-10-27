#pragma once
// FaceTrackingComponent: binds a character to one source of FaceTracking::Service
// through a .ifacemap mapping. It lives on the SkeletonComponent carrier, like the
// FacialAnimationComponent its morphs go through (the Tracked layer of that fold);
// several characters may bind one source.
// Authored fields are serialized ("FaceTracking"). Everything under "runtime" is
// FaceTrackingSystem's: rebuilt from the authored fields, carried across an
// inspector rewrite by ComponentWrite's Capture case, and dropped by Play / Stop (the
// cloner below), so a session never starts from another session's filter state.

#include "ECS/ECS.h"

#include <memory>
#include <string>

namespace FaceTracking { struct BindingRuntime; }

struct FaceTrackingComponent
{
    // ---- Authored ----
    bool        enabled = true;
    std::string source  = "default";  // a FaceTracking::Service source name
    std::string profile;              // a .ifacemap asset; empty = the built-in ARKit 1:1 mapping
    float       weightMorph = 1.0f;   // 0..1 each: how much of the tracked face reaches the morphs,
    float       weightHead  = 1.0f;   // the neck / head chain,
    float       weightEyes  = 1.0f;   // and the eye bones
    bool        mirror      = true;   // a live face mirrors the user (takes store it unmirrored)
    float       holdSec     = 0.25f;  // keep the last face this long after it is lost,
    float       fadeOutSec  = 0.6f;   // then hand back over this long,
    float       fadeInSec   = 0.2f;   // and take over again over this long once it is back
    bool        suppressAutoBlink = true; // pause FacialAnimation's auto-blink while tracking the eyes
    bool        headTrackHandoff  = true; // HeadTrack's weight x (1 - head authority)

    // ---- Runtime (never serialized) ----
    // This frame's head authority x weightHead: HeadTrackSystem scales its own weight
    // by (1 - headAuthority), so a lost face hands the head back to HeadTrack smoothly.
    float headAuthority = 0.0f;
    std::shared_ptr<FaceTracking::BindingRuntime> rt;
};

// Play / Stop and undo copy components by value (ComponentCloner in ECS.h): drop the
// runtime, or a Stop would restore a half-way filter state, a cursor into a source
// that moved on and a mapping compiled against a skeleton that may have changed.
// Constants only, so Clone(Clone(x)) == Clone(x). Never kPlainCopy; defined in this
// header for the ODR.
template <>
struct ComponentCloner<FaceTrackingComponent>
{
    static FaceTrackingComponent Clone(const FaceTrackingComponent& src)
    {
        FaceTrackingComponent d = src;
        d.headAuthority = 0.0f;
        d.rt.reset();
        return d;
    }
};
