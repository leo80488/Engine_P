#pragma once
#include "Anim/Graph/AnimGraphDefinition.h"
#include <nlohmann/json.hpp>
#include <functional>

namespace Anim::Graph {
struct Diagnostic { std::string node, message; bool warning = false; };
struct CompileResult {
    std::shared_ptr<const Definition> definition;
    std::vector<Diagnostic> diagnostics;
    bool Ok() const { return definition != nullptr; }
};
using GraphResolver = std::function<nlohmann::json(const std::string&)>;
CompileResult Compile(const nlohmann::json&, const GraphResolver& = {});
CompileResult CompileText(std::string_view, const GraphResolver& = {});
ParamValue ReadParamValue(const nlohmann::json&, ParamType);
bool IsPose(NodeType);
}
