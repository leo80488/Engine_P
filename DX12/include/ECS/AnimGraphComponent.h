#pragma once
#include "Anim/Graph/AnimGraphTypes.h"
#include <vector>
#include "ECS/ECS.h"
#include "Anim/Core/ClipTracks.h"

struct AnimGraphComponent {
    std::string graphPath, animSetPath;
    bool enabled = true;
    bool motorRootMotion = false;
    int lod = 0, updateInterval = 1;
    Anim::RootMotionDelta rootMotion;
    uint32_t rootFrame=0,consumedRootFrame=0;
    std::vector<std::pair<std::string, Anim::Graph::ParamValue>> defaults;
    uint64_t definitionVersion = 0;
    uint32_t instanceHandle = 0;
    float previewDt = -1; // runtime-only, owned by an isolated editor stage
};
struct AnimGraphParamsComponent {
    struct Request {
        Anim::Graph::SlotRequestDesc desc;
        Anim::Graph::SlotHandle handle;
        bool stop = false;
    };

    std::array<Anim::Graph::ParamValue, 64> values{};
    uint64_t pendingTriggers = 0, definitionHash = 0;
    uint32_t count = 0, nextRequest = 1;

    std::vector<Request> slotRequests;
};
template<> struct ComponentCloner<AnimGraphComponent> {
    static AnimGraphComponent Clone(const AnimGraphComponent& src) {
        auto copy = src; copy.definitionVersion = 0; copy.instanceHandle = 0; copy.previewDt = -1;copy.rootMotion={};copy.rootFrame=copy.consumedRootFrame=0; return copy;
    }
};
