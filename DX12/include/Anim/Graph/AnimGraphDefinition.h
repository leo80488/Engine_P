#pragma once
#include "Anim/Graph/AnimGraphTypes.h"
#include "Anim/Core/PoseOps.h"
#include <memory>
#include <vector>

namespace Anim::Graph {
struct Sample { std::string slot; float speed = 0, angle = 0, phaseOffset = 0, x = 0, y = 0; };
struct Layer { NodeIndex pose = kNoNode, weight = kNoNode; int mask = -1; float alpha = 1; bool additive = false; };
struct State { std::string name; NodeIndex pose = kNoNode; };
struct Transition {
    int from = -1, to = -1, priority = 0;
    NodeIndex condition = kNoNode;
    float duration = .15f, exitTime = -1;
    uint8_t curve = 0, interrupt = 1;
    bool inertialize = false, syncPhase = false, allowSelf = false, resetTarget = true;
};
struct Node {
    std::string id;
    NodeType type = NodeType::RefPose;
    ParamType valueType = ParamType::Float;
    std::array<NodeIndex, 4> input{ kNoNode, kNoNode, kNoNode, kNoNode };
    int param = -1, mask = -1, defaultState = 0, maxLod = 255;
    NodeIndex reference = kNoNode;
    std::string stateName;
    std::string slot, syncGroup, operation = "greater", eventPolicy = "weightAbove";
    std::string syncRole = "canBeLeader";
    ParamValue constant;
    float alpha = .5f, rate = 1, startTime = 0, damping = 0, blendTime = .15f, eventThreshold = .5f;
    float inputMin=0,inputMax=1,outputMin=0,outputMax=1;
    bool clampRemap=true;
    bool rootMotion = true, eventPolicyAuthored = false;
    bool looping = true, resetOnRelevant = false, deadBlend = false, cartesian = false, meshSpace = false;
    std::vector<std::array<int,3>> triangles;
    std::vector<Sample> samples;
    std::vector<Layer> layers;
    std::vector<State> states;
    std::vector<Transition> transitions;
    std::vector<NodeIndex> branches;
};
struct MaskDecl { std::string name, root, role; uint8_t mode = 0; int featherDepth = 0; };
struct InstanceLayout {
    static constexpr size_t NoOffset=~size_t(0);
    size_t bytes=0;
    std::vector<size_t> stateOffsets,blendOffsets;
};
struct Definition {
    InstanceLayout instanceLayout;
    uint64_t contentHash = 0;
    std::string name;
    NodeIndex root = kNoNode;
    std::vector<Node> nodes;
    std::vector<ParamDecl> params;
    std::vector<std::string> slots;
    std::vector<MaskDecl> masks;
    std::vector<std::string> syncGroups;
    std::vector<std::string> dependencies;
    bool ResolveParam(ParamHandle& handle)const {
        if(handle.definitionHash==contentHash && handle.index<params.size() && params[handle.index].name==handle.name)return true;
        const int index=FindParam(handle.name);handle.definitionHash=contentHash;handle.index=index<0?kNoNode:static_cast<NodeIndex>(index);return index>=0;
    }
    int FindParam(std::string_view name) const {
        for (size_t i = 0; i < params.size(); ++i) if (params[i].name == name) return static_cast<int>(i);
        return -1;
    }
};
InstanceLayout BuildInstanceLayout(const Definition&);
struct BoundGraph {
    std::vector<LocalPose> rest;
    std::vector<int> parents;
    std::vector<std::string> boneNames;
    std::vector<std::vector<float>> masks;
    static std::shared_ptr<const BoundGraph> Bind(const Definition&, const SkeletonAsset&);
};
}
