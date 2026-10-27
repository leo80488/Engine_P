#pragma once

// The Face Tracking inspector's field descriptor (the FaceTracking module's editor half;
// editor-only, like Reflection/ComponentReflection.h).

#include "Reflection/Reflect.h"
#include "Reflection/ReflectionEditor.h"

#include "ECS/FaceTrackingComponent.h"

// The run-time half (rt, headAuthority) is not reflected: FaceTrackingSystem owns it, and the
// editor fingerprints this component by its serializer.

REFLECT_BEGIN(FaceTrackingComponent)
    REFLECT_BOOL       (enabled,           "Enabled")
    REFLECT_INFO       ("Must sit on the entity that carries SkeletonComponent; the morphs go through its Facial Animation.")
    REFLECT_STRING     (source,            "Source")
    REFLECT_STRING_DROP(profile,           "Mapping (.ifacemap)", "IFACEMAP_PATH")
    REFLECT_INFO       ("Empty = the built-in mapping that fits the model's morph names (arkit, vroid or mmd_jp).")
    REFLECT_HEADER     ("Weights")
    REFLECT_DRAG       (weightMorph,       "Morphs", 0.f, 1.f)
    REFLECT_DRAG       (weightHead,        "Head",   0.f, 1.f)
    REFLECT_DRAG       (weightEyes,        "Eyes",   0.f, 1.f)
    REFLECT_BOOL       (mirror,            "Mirror")
    REFLECT_INFO       ("Mirror: the character moves like a mirror image of the performer (live default).")
    REFLECT_HEADER     ("Tracking Loss")
    REFLECT_DRAG       (holdSec,           "Hold (s)",     0.f, 5.f)
    REFLECT_DRAG       (fadeOutSec,        "Fade Out (s)", 0.f, 5.f)
    REFLECT_DRAG       (fadeInSec,         "Fade In (s)",  0.f, 5.f)
    REFLECT_BOOL       (suppressAutoBlink, "Pause Auto Blink")
    REFLECT_BOOL       (headTrackHandoff,  "Hand Off Head Track")
    REFLECT_INFO       ("While a face is tracked, Head Track gives way by the face's authority.")
REFLECT_END()
