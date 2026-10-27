#include "Anim/Graph/AnimGraphCompiler.h"
#include "ECS/LocomotionComponents.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "Anim/Graph/BlendSpaceGeometry.h"
#include "Anim/Graph/SubGraphExpansion.h"

namespace Anim::Graph {
InstanceLayout BuildInstanceLayout(const Definition& definition){
    InstanceLayout layout;
    layout.stateOffsets.resize(definition.nodes.size());
    layout.blendOffsets.assign(definition.nodes.size(),InstanceLayout::NoOffset);
    auto allocate=[&](size_t bytes,size_t alignment){layout.bytes=(layout.bytes+alignment-1)&~(alignment-1);const size_t offset=layout.bytes;layout.bytes+=bytes;return offset;};
    for(size_t i=0;i<definition.nodes.size();++i)layout.stateOffsets[i]=allocate(sizeof(NodeState),alignof(NodeState));
    for(size_t i=0;i<definition.nodes.size();++i)if(definition.nodes[i].type==NodeType::BlendSpace1D || definition.nodes[i].type==NodeType::BlendSpace2D)
        layout.blendOffsets[i]=allocate(sizeof(LocomotionComponent),alignof(LocomotionComponent));
    return layout;
}

bool IsPose(NodeType type) { return (type == NodeType::AimOffset || type <= NodeType::Inertialization) && type != NodeType::Param && type != NodeType::Constant && type != NodeType::Compare && type != NodeType::Not; }
ParamValue ReadParamValue(const nlohmann::json& j, ParamType type) {
    if(j.is_null())return {};
    auto finite=[](const nlohmann::json& value){if(!value.is_number())throw std::runtime_error("Expected numeric parameter value");const float v=value.get<float>();if(!std::isfinite(v))throw std::runtime_error("Nonfinite parameter value");return v;};
    if(type==ParamType::Float)return ParamValue::FromFloat(finite(j));
    if(type==ParamType::Bool || type==ParamType::Trigger){if(!j.is_boolean())throw std::runtime_error("Expected boolean parameter value");return ParamValue::FromInt(j.get<bool>());}
    if(type==ParamType::Id && j.is_string())return ParamValue::FromInt(static_cast<int32_t>(Hash(j.get<std::string>())));
    if(type==ParamType::Vector3){if(!j.is_array() || j.size()!=3)throw std::runtime_error("Expected vector3 parameter value");ParamValue v;for(int i=0;i<3;++i)v.vector[i]=finite(j[i]);return v;}
    if(!j.is_number_integer())throw std::runtime_error("Expected integer parameter value");
    if(j.is_number_unsigned()){const auto value=j.get<uint64_t>();if(value>(type==ParamType::Id?UINT32_MAX:uint64_t(INT32_MAX)))throw std::runtime_error("Integer parameter value out of range");return ParamValue::FromInt(static_cast<int32_t>(value));}
    const auto value=j.get<int64_t>();if(value<INT32_MIN || value>INT32_MAX)throw std::runtime_error("Integer parameter value out of range");return ParamValue::FromInt(static_cast<int32_t>(value));
}
CompileResult CompileText(std::string_view text, const GraphResolver& resolver) {
    try { return Compile(nlohmann::json::parse(text), resolver); }
    catch (const std::exception& e) { return {nullptr, {{"", e.what(), false}}}; }
}
CompileResult Compile(const nlohmann::json& source, const GraphResolver& resolver) {
    CompileResult result;
    auto def = std::make_shared<Definition>();
    auto error = [&](std::string node, std::string message, bool warning = false) { result.diagnostics.push_back({std::move(node), std::move(message), warning}); };
    try {
        std::vector<std::string> stack;
        auto doc=ExpandSubGraphs(source,resolver,stack,def->dependencies);
        if (doc.value("schema", 0) != 1) { error("", "Unsupported graph schema; expected 1"); return result; }
        def->name = doc.value("name", "Animation Graph");
        std::unordered_set<uint32_t> hashes;
        const std::unordered_map<std::string, ParamType> paramTypes{{"float", ParamType::Float}, {"int", ParamType::Int}, {"bool", ParamType::Bool}, {"trigger", ParamType::Trigger}, {"vec3", ParamType::Vector3}, {"vector3", ParamType::Vector3}, {"id", ParamType::Id}};
        for (const auto& p : doc.value("parameters", nlohmann::json::array())) {
            ParamDecl param; param.name = p.at("name").get<std::string>();
            auto type = paramTypes.find(p.value("type", "float"));
            if (type == paramTypes.end()) { error("", "Unknown parameter type: " + param.name); continue; }
            param.type = type->second;
            if (param.name.empty() || !hashes.insert(Hash(param.name)).second) error("", "Duplicate parameter name/hash: " + param.name);
            param.value = ReadParamValue(p.value("default", nlohmann::json()), param.type);
            param.holdSec = p.value("holdSec", 0.f);
            if (!std::isfinite(param.holdSec) || param.holdSec < 0) error("", "Invalid trigger holdSec: " + param.name);
            def->params.push_back(param);
        }
        if (def->params.size() > 64) error("", "At most 64 parameters are supported");
        for (const auto& s : doc.value("slots", nlohmann::json::array())) def->slots.push_back(s.is_string() ? s.get<std::string>() : s.at("name").get<std::string>());
        def->syncGroups = doc.value("syncGroups", std::vector<std::string>{});
        auto uniqueNames=[&](const std::vector<std::string>& names,const char* category){std::unordered_set<std::string> seen;for(const auto& name:names)if(name.empty() || !seen.insert(name).second)error("",std::string("Duplicate/empty ")+category+": "+name);};
        uniqueNames(def->slots,"slot");uniqueNames(def->syncGroups,"sync group");
        for (const auto& m : doc.value("masks", nlohmann::json::array())) {
            MaskDecl mask; mask.name = m.at("name").get<std::string>(); mask.root = m.value("root", ""); mask.role = m.value("role", "");
            mask.mode = !mask.root.empty() ? 3 : mask.role == "Spine" || mask.role == "UpperBody" ? 1 : mask.role == "LowerBody" ? 2 : 0;
            mask.featherDepth = m.value("featherDepth", 0); def->masks.push_back(mask);
        }
        std::vector<std::string> maskNames;for(const auto& mask:def->masks){maskNames.push_back(mask.name);if(mask.featherDepth<0)error("","Negative mask featherDepth: "+mask.name);}uniqueNames(maskNames,"mask");
        const std::unordered_map<std::string, NodeType> nodeTypes{
            {"Output", NodeType::Output}, {"RefPose", NodeType::RefPose}, {"Clip", NodeType::Clip}, {"Blend2", NodeType::Blend2}, {"BlendByBool", NodeType::BlendByBool},
            {"Param", NodeType::Param}, {"Constant", NodeType::Constant}, {"Compare", NodeType::Compare}, {"Not", NodeType::Not}, {"BlendByInt", NodeType::BlendByInt},
            {"BlendSpace1D", NodeType::BlendSpace1D}, {"BlendSpace2D", NodeType::BlendSpace2D}, {"LayeredBlend", NodeType::LayeredBlend}, {"ApplyAdditive", NodeType::ApplyAdditive},
            {"StateMachine", NodeType::StateMachine}, {"Slot", NodeType::Slot}, {"CachedPose", NodeType::CachedPose}, {"TimeScale", NodeType::TimeScale}, {"Inertialization", NodeType::Inertialization},
            {"Math", NodeType::Math}, {"And", NodeType::And}, {"Or", NodeType::Or}, {"Xor", NodeType::Xor}, {"Select", NodeType::Select}, {"Remap", NodeType::Remap}, {"Damp", NodeType::Damp},
            {"DeltaAngle", NodeType::DeltaAngle}, {"WrapAngle", NodeType::WrapAngle}, {"StateTime", NodeType::StateTime}, {"StateWeight", NodeType::StateWeight}, {"IsInState", NodeType::IsInState},
            {"IsTransitioning", NodeType::IsTransitioning}, {"ClipTimeRemaining", NodeType::ClipTimeRemaining}, {"ClipNormalizedTime", NodeType::ClipNormalizedTime}, {"EventFired", NodeType::EventFired}, {"Random", NodeType::Random}, {"ClipCurve", NodeType::ClipCurve}, {"AimOffset", NodeType::AimOffset}};
        const auto& jsonNodes = doc.at("nodes");
        if (!jsonNodes.is_array() || jsonNodes.empty() || jsonNodes.size() >= kNoNode) { error("", "Graph must have 1..65534 nodes"); return result; }
        std::unordered_map<std::string, NodeIndex> indices;
        for (const auto& j : jsonNodes) {
            Node n; n.id = j.at("id").get<std::string>();
            auto type = nodeTypes.find(j.at("type").get<std::string>());
            if (type == nodeTypes.end()) error(n.id, "Unknown node type"); else n.type = type->second;
            if (n.id.empty() || !indices.emplace(n.id, static_cast<NodeIndex>(def->nodes.size())).second) error(n.id, "Duplicate/empty node id");
            def->nodes.push_back(n);
        }
        auto resolve = [&](const nlohmann::json& id, const std::string& owner) -> NodeIndex {
            if (!id.is_string()) { error(owner, "Pin reference must be a node id"); return kNoNode; }
            auto it = indices.find(id.get<std::string>());
            if (it == indices.end()) { error(owner, "Unknown node: " + id.get<std::string>()); return kNoNode; }
            return it->second;
        };
        auto maskIndex = [&](const std::string& name, const std::string& owner) {
            if (name.empty()) return -1;
            for (size_t i = 0; i < def->masks.size(); ++i) if (def->masks[i].name == name) return static_cast<int>(i);
            error(owner, "Unknown mask: " + name); return -1;
        };
        auto checkSlot = [&](const std::string& slot, const std::string& owner) {
            if (std::find(def->slots.begin(), def->slots.end(), slot) == def->slots.end()) error(owner, "Unknown clip slot: " + slot);
        };
        for (size_t i = 0; i < def->nodes.size(); ++i) {
            auto& n = def->nodes[i]; const auto& j = jsonNodes[i];
            const auto inputs = j.value("in", nlohmann::json::object());
            auto pin = [&](const char* name, int dest) { if (inputs.contains(name)) n.input[dest] = resolve(inputs.at(name), n.id); };
            if (n.type == NodeType::Output || n.type == NodeType::CachedPose || n.type == NodeType::Inertialization) pin("pose", 0);
            else if (n.type == NodeType::Slot) pin("source", 0);
            else if (n.type == NodeType::TimeScale) { pin("pose", 0); pin("rate", 1); }
            else if (n.type == NodeType::LayeredBlend) pin("base", 0);
            else if (n.type == NodeType::AimOffset) {pin("base",0);pin("x",1);pin("y",2);pin("weight",3);}
            else { pin("a", 0); pin("b", 1); pin("alpha", 2); pin("condition", 2); pin("c", 2); pin("d", 3); }
            if (n.type == NodeType::Clip) pin("rate", 0);
            if(n.type==NodeType::Inertialization)pin("request",1);
            if (j.contains("x")) n.input[0] = resolve(j["x"], n.id);
            if (j.contains("y")) n.input[1] = resolve(j["y"], n.id);
            if (inputs.contains("index")) n.input[2] = resolve(inputs.at("index"), n.id);
            for (const auto& branch : j.value("poses", nlohmann::json::array())) n.branches.push_back(resolve(branch, n.id));
            n.alpha = j.value("alpha", n.type==NodeType::AimOffset?1.f:.5f); n.rate = j.value("rate", 1.f); n.startTime = j.value("startTime", 0.f);
            n.rootMotion = j.value("rootMotion", true); n.looping = j.value("loop", true); n.damping = j.value("damping", 0.f); n.blendTime = j.value("blendTime", .15f);
            n.resetOnRelevant = j.value("resetOnRelevant", false); n.maxLod = j.value("maxLod", 255);
            n.inputMin=j.value("inputMin",0.f);n.inputMax=j.value("inputMax",1.f);n.outputMin=j.value("outputMin",0.f);n.outputMax=j.value("outputMax",1.f);n.clampRemap=j.value("clamp",true);
            for(float value:{n.inputMin,n.inputMax,n.outputMin,n.outputMax})if(!std::isfinite(value))error(n.id,"Nonfinite remap range");
            n.deadBlend=j.value("deadBlend",false);n.meshSpace=j.value("space","local")=="mesh";
            n.cartesian=n.type==NodeType::AimOffset || j.value("geometry","polar")=="cartesian";
            n.syncGroup = j.value("syncGroup", ""); n.operation = j.value("op", "greater");
            n.syncRole = j.value("syncRole", "canBeLeader");
            n.eventPolicyAuthored=j.contains("eventPolicy");
            const auto defaultPolicy=!n.syncGroup.empty()?"leader":(n.type==NodeType::BlendSpace1D || n.type==NodeType::BlendSpace2D)?"mostActive":"weightAbove";
            n.eventPolicy = j.value("eventPolicy", defaultPolicy);
            static const std::unordered_map<std::string,std::string> kPolicyAliases={{"LeaderOnly","leader"},{"MostActive","mostActive"},{"All","all"},{"WeightAbove","weightAbove"},{"ActiveBranchOnly","activeState"},{"None","none"}};
            if(auto alias=kPolicyAliases.find(n.eventPolicy);alias!=kPolicyAliases.end())n.eventPolicy=alias->second;
            n.eventThreshold = j.value("eventThreshold", .5f);
            n.slot = j.value("slot", ""); n.mask = maskIndex(j.value("mask", ""), n.id);
            if (j.contains("machine")) n.reference = resolve(j["machine"], n.id);
            if (j.contains("player")) n.reference = resolve(j["player"], n.id);
            n.stateName = j.value("state", "");
            if(n.type==NodeType::EventFired)n.stateName=j.value("event","");
            if(n.type==NodeType::ClipCurve)n.stateName=j.value("curve","");
            if (n.type == NodeType::Clip) checkSlot(n.slot, n.id);
            if (n.type == NodeType::Param) { n.param = def->FindParam(j.value("param", "")); if (n.param < 0) error(n.id, "Unknown parameter"); else n.valueType = def->params[n.param].type; }
            if (n.type == NodeType::Constant) {
                auto type = paramTypes.find(j.value("valueType", "float"));
                if (type == paramTypes.end()) error(n.id, "Unknown constant type"); else n.valueType = type->second;
                n.constant = ReadParamValue(j.value("value", nlohmann::json()), n.valueType);
            }
            auto oneOf=[&](const std::string& value,std::initializer_list<const char*> choices){for(auto choice:choices)if(value==choice)return true;return false;};
            if(n.type==NodeType::Compare && !oneOf(n.operation,{"less","equal","notEqual","lessEqual","greaterEqual","greater"}))error(n.id,"Unknown Compare operation");
            if(n.type==NodeType::Math && !oneOf(n.operation,{"add","subtract","multiply","divide","min","max","abs","clamp","lerp"}))error(n.id,"Unknown Math operation");
            if(!oneOf(n.eventPolicy,{"weightAbove","leader","mostActive","all","activeState","none"}))error(n.id,"Unknown event policy");
            if(!oneOf(n.syncRole,{"canBeLeader","alwaysLeader","alwaysFollower"}))error(n.id,"Unknown sync role");
            if(!n.syncGroup.empty() && std::find(def->syncGroups.begin(),def->syncGroups.end(),n.syncGroup)==def->syncGroups.end())error(n.id,"Unknown sync group: "+n.syncGroup);
            if(n.damping<0 || n.blendTime<0 || n.eventThreshold<0 || n.eventThreshold>1)error(n.id,"Invalid damping, blend duration or event threshold");
            if (n.type == NodeType::Compare || n.type == NodeType::Not || n.type == NodeType::And || n.type == NodeType::Or || n.type == NodeType::Xor || n.type == NodeType::IsInState || n.type == NodeType::IsTransitioning || n.type == NodeType::EventFired) n.valueType = ParamType::Bool;
            for (const auto& sample : j.value("samples", nlohmann::json::array())) {
                Sample s; s.slot = sample.at("slot").get<std::string>(); checkSlot(s.slot, n.id);
                s.speed = sample.value("speed", 0.f); s.angle = sample.value("angle", 0.f); s.phaseOffset = sample.value("phaseOffset", 0.f); s.x=sample.value("x",s.speed);s.y=sample.value("y",s.angle);
                for(float v:{s.speed,s.angle,s.phaseOffset,s.x,s.y})if(!std::isfinite(v))error(n.id,"Nonfinite sample coordinate");
                n.samples.push_back(s);
            }
            if ((n.type == NodeType::BlendSpace1D || n.type == NodeType::BlendSpace2D || n.type == NodeType::AimOffset) && (n.samples.empty() || n.samples.size() > 24)) error(n.id, "Blend space needs 1..24 samples");
            if(n.cartesian){
                for(size_t a=0;a<n.samples.size();++a)for(size_t b=0;b<a;++b)if(n.samples[a].x==n.samples[b].x && n.samples[a].y==n.samples[b].y)error(n.id,"Duplicate cartesian sample coordinate");
                n.triangles=Triangulate(n.samples);
            }
            for (const auto& l : j.value("layers", nlohmann::json::array())) {
                Layer layer; layer.pose = resolve(l.at("pose"), n.id); layer.mask = maskIndex(l.value("mask", ""), n.id);
                if (l.contains("weight") && l["weight"].is_string()) layer.weight = resolve(l["weight"], n.id); else layer.alpha = l.value("weight", 1.f);
                layer.additive = l.value("mode", "override") == "additive"; n.layers.push_back(layer);
            }
            if (n.type == NodeType::StateMachine) {
                std::unordered_map<std::string, int> stateIds;
                for (const auto& state : j.at("states")) {
                    const auto name = state.at("name").get<std::string>();
                    if(name.empty())error(n.id,"Empty state name");
                    if (!stateIds.emplace(name, static_cast<int>(n.states.size())).second) error(n.id, "Duplicate state: " + name);
                    n.states.push_back({name, resolve(state.at("pose"), n.id)});
                }
                auto defaultState = stateIds.find(j.value("default", ""));
                if (defaultState == stateIds.end()) error(n.id, "Missing/unknown default state"); else n.defaultState = defaultState->second;
                for (const auto& t : j.value("transitions", nlohmann::json::array())) {
                    Transition tr; const auto from = t.at("from").get<std::string>(), to = t.at("to").get<std::string>();
                    if (from != "*" && from != "AnyState" && !stateIds.count(from)) error(n.id, "Unknown source state: " + from);
                    else tr.from = from == "*" || from == "AnyState" ? -1 : stateIds.at(from);
                    if (!stateIds.count(to)) { error(n.id, "Unknown target state: " + to); continue; } tr.to = stateIds.at(to);
                    if (t.contains("when")) tr.condition = resolve(t["when"], n.id);
                    tr.duration = t.value("duration", .15f); tr.exitTime = t.value("exitTime", -1.f); tr.priority = t.value("priority", 0);
                    if(!oneOf(t.value("mode","crossfade"),{"crossfade","inertialize"}))error(n.id,"Unknown transition mode");
                    if(!oneOf(t.value("curve","linear"),{"linear","smoothStep","easeIn","easeOut"}))error(n.id,"Unknown transition curve");
                    if(!oneOf(t.value("interrupt","higherPriority"),{"none","higherPriority","any"}))error(n.id,"Unknown transition interrupt policy");
                    if(!std::isfinite(tr.exitTime) || tr.exitTime < -1)error(n.id,"Invalid transition exitTime");
                    tr.inertialize = t.value("mode", "crossfade") == "inertialize"; tr.syncPhase = t.value("syncPhase", false);
                    tr.allowSelf = t.value("allowSelf", false); tr.resetTarget = t.value("resetTarget", true);
                    const auto curve = t.value("curve", "linear"); tr.curve = curve == "smoothStep" ? 1 : curve == "easeIn" ? 2 : curve == "easeOut" ? 3 : 0;
                    const auto interrupt = t.value("interrupt", "higherPriority"); tr.interrupt = interrupt == "none" ? 0 : interrupt == "any" ? 2 : 1;
                    if (tr.duration < 0 || !std::isfinite(tr.duration)) error(n.id, "Transition duration must be finite and nonnegative");
                    if (tr.condition == kNoNode && tr.exitTime < 0) error(n.id, "Unconditional transition without exit time", true);
                    n.transitions.push_back(tr);
                }
                std::stable_sort(n.transitions.begin(), n.transitions.end(), [](const auto& a, const auto& b) { if ((a.from < 0) != (b.from < 0)) return a.from < 0; return a.priority < b.priority; });
            }
            for (float value : {n.alpha, n.rate, n.startTime, n.damping, n.blendTime}) if (!std::isfinite(value)) error(n.id, "Nonfinite node setting");
        }
        def->root = resolve(doc.at("root"), "");
        if (def->root != kNoNode && !IsPose(def->nodes[def->root].type)) error("", "Root must produce a pose");
        std::vector<uint8_t> visit(def->nodes.size(), 0);
        std::function<void(NodeIndex, int)> validate = [&](NodeIndex index, int depth) {
            if (index == kNoNode) return;
            auto& n = def->nodes[index];
            if (visit[index] == 1) { error(n.id, "Cycle in graph"); return; }
            if (visit[index] == 2) return;
            if (depth > 256) { error(n.id, "Graph nesting exceeds 256"); return; }
            visit[index] = 1;
            auto typed = [&](NodeIndex child, bool pose, ParamType value = ParamType::Float) {
                if (child == kNoNode) return;
                validate(child, depth + 1);
                const auto& c = def->nodes[child];
                if (IsPose(c.type) != pose) error(n.id, "Pin type mismatch: " + c.id);
                else if (!pose && c.valueType != value && !(value == ParamType::Bool && c.valueType == ParamType::Trigger) && !(value == ParamType::Float && c.valueType == ParamType::Int)) error(n.id, "Value type mismatch: " + c.id);
            };
            switch (n.type) {
            case NodeType::Output: case NodeType::CachedPose: case NodeType::Slot: typed(n.input[0], true); if (n.input[0] == kNoNode) error(n.id, "Missing pose input"); break;
            case NodeType::Inertialization:typed(n.input[0],true);typed(n.input[1],false,ParamType::Bool);if(n.input[0]==kNoNode)error(n.id,"Missing pose input");break;
            case NodeType::Blend2: case NodeType::ApplyAdditive: case NodeType::BlendByBool:
                typed(n.input[0], true); typed(n.input[1], true); typed(n.input[2], false, n.type == NodeType::BlendByBool ? ParamType::Bool : ParamType::Float); break;
            case NodeType::AimOffset: typed(n.input[0],true);for(int k=1;k<4;++k)typed(n.input[k],false);if(n.input[0]==kNoNode)error(n.id,"Missing AimOffset base");break;
            case NodeType::LayeredBlend: typed(n.input[0], true); for (const auto& l : n.layers) { typed(l.pose, true); typed(l.weight, false); } break;
            case NodeType::BlendByInt:
                if (n.branches.empty()) error(n.id, "BlendByInt needs poses");
                for (auto pose : n.branches) typed(pose, true);
                typed(n.input[2], false, ParamType::Int); break;
            case NodeType::StateMachine: for (const auto& s : n.states) typed(s.pose, true); for (const auto& t : n.transitions) typed(t.condition, false, ParamType::Bool); break;
            case NodeType::TimeScale: typed(n.input[0], true); typed(n.input[1], false); break;
            case NodeType::Compare: {
                for(int k=0;k<2;++k){if(n.input[k]==kNoNode)error(n.id,"Missing Compare input");else{validate(n.input[k],depth+1);if(IsPose(def->nodes[n.input[k]].type))error(n.id,"Pin type mismatch");}}
                if(n.input[0]!=kNoNode && n.input[1]!=kNoNode){auto a=def->nodes[n.input[0]].valueType,b=def->nodes[n.input[1]].valueType;if(a==ParamType::Trigger)a=ParamType::Bool;if(b==ParamType::Trigger)b=ParamType::Bool;
                    const bool numeric=(a==ParamType::Int || a==ParamType::Float) && (b==ParamType::Int || b==ParamType::Float);
                    if(!numeric && (a!=b || (n.operation!="equal" && n.operation!="notEqual")))error(n.id,"Compare value type mismatch or unsupported ordering");}
                break;
            }
            case NodeType::Select: {
                for(int k=0;k<2;++k){if(n.input[k]==kNoNode)error(n.id,"Missing Select input");else{validate(n.input[k],depth+1);if(IsPose(def->nodes[n.input[k]].type))error(n.id,"Pin type mismatch");}}
                typed(n.input[2],false,ParamType::Bool);if(n.input[2]==kNoNode)error(n.id,"Missing Select condition");
                if(n.input[0]!=kNoNode && n.input[1]!=kNoNode){auto a=def->nodes[n.input[0]].valueType,b=def->nodes[n.input[1]].valueType;if(a==ParamType::Trigger)a=ParamType::Bool;if(b==ParamType::Trigger)b=ParamType::Bool;
                    if(a==b)n.valueType=a;else if((a==ParamType::Int && b==ParamType::Float)||(b==ParamType::Int && a==ParamType::Float))n.valueType=ParamType::Float;else error(n.id,"Select branch value type mismatch");}
                break;
            }
            case NodeType::Not: case NodeType::And: case NodeType::Or: case NodeType::Xor: for (auto c : n.input) typed(c, false, ParamType::Bool); break;
            default: for (auto c : n.input) typed(c, false); break;
            }
            if(n.type==NodeType::Blend2 || n.type==NodeType::ApplyAdditive || n.type==NodeType::BlendByBool) {
                if(n.input[0]==kNoNode || n.input[1]==kNoNode)error(n.id,"Missing blend pose input");
                if(n.type==NodeType::BlendByBool && n.input[2]==kNoNode)error(n.id,"Missing blend condition");
            }
            if(n.type==NodeType::ClipNormalizedTime || n.type==NodeType::ClipTimeRemaining || n.type==NodeType::ClipCurve) {
                if(n.reference==kNoNode || def->nodes[n.reference].type!=NodeType::Clip)error(n.id,"Player reference must name a Clip node");
            }
            if(n.type==NodeType::StateTime || n.type==NodeType::StateWeight || n.type==NodeType::IsInState || n.type==NodeType::IsTransitioning) {
                if(n.reference==kNoNode || def->nodes[n.reference].type!=NodeType::StateMachine)error(n.id,"State reference must name a StateMachine node");
                else if(n.type==NodeType::IsInState && std::none_of(def->nodes[n.reference].states.begin(),def->nodes[n.reference].states.end(),[&](const auto& state){return state.name==n.stateName;}))error(n.id,"Unknown queried state: "+n.stateName);
            }
            if(n.type==NodeType::Not || n.type==NodeType::Damp || n.type==NodeType::WrapAngle || n.type==NodeType::Remap || n.type==NodeType::Math || n.type==NodeType::DeltaAngle || n.type==NodeType::And || n.type==NodeType::Or || n.type==NodeType::Xor)
                if(n.input[0]==kNoNode)error(n.id,"Missing value input");
            if(n.type==NodeType::And || n.type==NodeType::Or || n.type==NodeType::Xor || n.type==NodeType::DeltaAngle || (n.type==NodeType::Math && n.operation!="abs"))if(n.input[1]==kNoNode)error(n.id,"Missing second value input");
            if(n.type==NodeType::Math && (n.operation=="clamp" || n.operation=="lerp") && n.input[2]==kNoNode)error(n.id,"Missing third value input");
            visit[index] = 2;
        };
        for (NodeIndex i = 0; i < def->nodes.size(); ++i) validate(i, 0);
        if (std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& d) { return !d.warning; })) return result;
        auto semantic = doc; semantic.erase("editor");
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char c : semantic.dump()) { hash ^= c; hash *= 1099511628211ull; }
        def->instanceLayout=BuildInstanceLayout(*def);
        def->contentHash = hash; result.definition = std::move(def);
    } catch (const std::exception& e) { error("", e.what()); }
    return result;
}
}
