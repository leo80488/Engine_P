#include "Harness/AssetTool.h"

// Self-guard as well as the vcxproj's ExcludedFromBuild: Retail has no tool mode.
#ifdef WITH_HARNESS

#include "Harness/AssetToolUI.h"           // the .iui verbs
#include "Harness/AssetToolShaderCook.h"   // shadercook
#include "Harness/AssetToolMatDecl.h"      // matdecl
#include "Harness/AssetToolUpgrade.h"      // upgrade
#include "UI/UIAsset.h"                    // ... and the catalog's .iui references

#include "ECS/Components.h"                  // matgraph: MaterialComponent
#include "ECS/Guid.h"
#include "MaterialGraph/MaterialGraphCompiler.h"   // matgraph
#include "Resource/AnimationLoader.h"     // compat: the .ianim loader
#include "Resource/MaterialSerializer.h"   // matgraph: LoadMaterial
#include "Resource/AnimationResource.h"   // ... and playback's binding rule (ChannelBinding)
#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetRefs.h"      // the catalog's references: the loader's own parse
#include "Resource/MeshLibIdentity.h"   // meshlib_identity: the .meshlib identity layer
#include "Resource/MeshLibTail.h"       // meshlets: the tail's other chunks, verified untouched
#include "Resource/MeshletBuilder.h"    // meshlets: the MLET chunk builder
#include "Resource/MeshletData.h"
#include "Resource/SceneDocument.h"
#include "Resource/AuthoringSchema.h"
#include "Resource/SceneSerializer.h"    // scene_keys: StampSceneKeys
#include "Resource/AssetGuid.h"          // bake_primitives: .meta sidecars
#include "Resource/SharedContent.h"
#include "Resource/SkeletonInspect.h"          // skeleton_report
#include "Resource/ModelSockets.h"             // skeleton_report: the model's sockets
#include "Resource/ModelVirtualBones.h"        // ... and its virtual bones
#include "Scene/PrimitiveAssets.h"       // bake_primitives
#include "Scene/SceneInstanceLoader.h"   // ... and the .iscn loader's
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Harness
{
namespace
{
    using Json = nlohmann::json;
    using Resource::SceneDocument;

    constexpr int kDone = 0, kRefused = 1, kUsage = 2;

    struct Result
    {
        Json j = Json::object();
        int  code = kDone;
    };

    Result Fail(int code, const char* error, const std::string& message, Json extra = Json::object())
    {
        Result r;
        r.code = code;
        r.j = Json{ { "ok", false }, { "error", error }, { "message", message } };
        for (auto it = extra.begin(); it != extra.end(); ++it) r.j[it.key()] = it.value();
        return r;
    }

    Json FileIdentity(const std::string& path)
    {
        std::uint64_t bytes = 0;
        const std::string sha = Sys::Sha256FileHex(path, &bytes);
        return Json{ { "path", path }, { "sha256", sha }, { "bytes", bytes } };
    }

    Json FindingsJson(const std::vector<SceneDocument::Finding>& fs, bool errors)
    {
        Json a = Json::array();
        for (const auto& f : fs)
            if (f.error == errors)
                a.push_back(Json{ { "code", f.code }, { "line", f.line }, { "message", f.message } });
        return a;
    }

    Json Vec(const float* v, int n)
    {
        Json a = Json::array();
        for (int i = 0; i < n; ++i) a.push_back(v[i]);
        return a;
    }

    Json NodeJson(const SceneDocument::Node& n)
    {
        const auto& f = n.fields;
        return Json{ { "idx", f.idx }, { "parent", f.parent }, { "name", f.name },
                     { "position", Vec(f.t, 3) }, { "rotation", Vec(f.q, 4) }, { "scale", Vec(f.s, 3) },
                     { "line", n.line + 1 }, { "components", n.tags } };
    }

    bool Load(const std::string& path, SceneDocument& doc, Result& r)
    {
        std::string err;
        if (path.empty()) { r = Fail(kUsage, "usage", "-asset=<scene> is required"); return false; }
        if (!doc.LoadFile(path, err)) { r = Fail(kUsage, "load_failed", err); return false; }
        return true;
    }

    int FindByIdx(const SceneDocument& doc, int idx)
    {
        for (std::size_t i = 0; i < doc.Nodes().size(); ++i)
            if (doc.Nodes()[i].fields.idx == idx) return static_cast<int>(i);
        return -1;
    }

    // ==== inspect / validate ====
    Result Inspect(const std::string& path)
    {
        Result r;
        SceneDocument doc;
        if (!Load(path, doc, r)) return r;
        Json nodes = Json::array();
        std::size_t componentLines = 0;
        for (const auto& n : doc.Nodes()) { nodes.push_back(NodeJson(n)); componentLines += n.componentLines.size(); }
        Json world = Json::object();
        for (const auto& [k, v] : doc.World()) world[k] = v;
        const auto& h = doc.Header();
        r.j = Json{
            { "ok", true }, { "file", FileIdentity(path) },
            { "header", { { "magic", "RWLD" }, { "version", h.version }, { "metadataSize", h.metadataSize },
                          { "dataSize", h.dataSize } } },
            { "meta", { { "nodeCount", doc.NodeCount() }, { "textLength", doc.MetaTextLength() } } },
            { "world", world }, { "nodes", nodes },
            { "lines", { { "total", doc.Lines().size() }, { "node", doc.Nodes().size() },
                         { "component", componentLines },
                         { "other", doc.Lines().size() - doc.Nodes().size() - componentLines } } } };
        return r;
    }

    Result Validate(const std::string& path, bool references)
    {
        Result r;
        SceneDocument doc;
        if (!Load(path, doc, r)) return r;
        const auto fs = doc.Validate(references);
        const Json errors = FindingsJson(fs, true);
        r.j = Json{ { "ok", errors.empty() }, { "file", FileIdentity(path) }, { "references", references },
                    { "errors", errors }, { "warnings", FindingsJson(fs, false) } };
        r.code = errors.empty() ? kDone : kRefused;
        return r;
    }

    // ==== patch ====
    struct Intent
    {
        std::string op;
        int         idx = -1;
        bool        hasT = false, hasQ = false, hasS = false;
        float       t[3]{}, q[4]{}, s[3]{};
        std::string name, tag, prefab;
        std::vector<std::pair<std::string, std::string>> values;
    };

    std::string RefText(const Json& v)
    {
        if (v.is_number_integer()) return std::to_string(v.get<long long>());
        if (v.is_string()) return v.get<std::string>();
        return {};
    }

    // Absent: false with @p error empty. Present but malformed: false with @p error.
    bool Floats(const Json& op, const char* key, float* out, std::size_t n, std::string& error)
    {
        if (!op.contains(key)) return false;
        const Json& a = op[key];
        if (!a.is_array() || a.size() != n)
        {
            error = std::string(key) + " wants a list of " + std::to_string(n) + " numbers";
            return false;
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            if (!a[i].is_number() || !std::isfinite(a[i].get<double>()) ||
                std::fabs(a[i].get<double>()) > Resource::Authoring::FloatMax)
            {
                error = std::string(key) + " wants finite numbers";
                return false;
            }
            out[i] = static_cast<float>(a[i].get<double>());
        }
        return true;
    }

    std::string NumberText(double d)
    {
        char b[48];
        if (std::floor(d) == d && std::fabs(d) < 1e15) std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(d));
        else                                          std::snprintf(b, sizeof(b), "%.9g", d);
        return b;
    }

    bool IsNumeric(const std::string& v)
    {
        if (v.empty()) return false;
        const unsigned char c0 = static_cast<unsigned char>(v[0]);
        if (!(std::isdigit(c0) || c0 == '-' || c0 == '+' || c0 == '.')) return false;
        if (v.find_first_of("xXnNiI") != std::string::npos) return false;
        char* end = nullptr;
        std::strtod(v.c_str(), &end);
        return end && *end == '\0';
    }

    std::size_t Arity(const std::string& v)
    {
        if (v.find('_') == std::string::npos) return 0;
        std::size_t n = 0, start = 0;
        for (;;)
        {
            const std::size_t us = v.find('_', start);
            if (!IsNumeric(v.substr(start, us == std::string::npos ? std::string::npos : us - start))) return 0;
            ++n;
            if (us == std::string::npos) return n;
            start = us + 1;
        }
    }

    // A JSON value as line text, shaped like the current text - the same rules as Entity.set:
    // number, "a_b_c" (a list of as many numbers), true/false or 1/0, or text (encoded). GUID keys
    // take their hex as is.
    bool ValueText(const std::string& key, const Json& v, const std::string& current,
                   std::string& out, std::string& error)
    {
        const bool curBool = current == "true" || current == "false";
        const std::size_t arity = Arity(current);
        const bool curNum = IsNumeric(current);
        auto bad = [&](const std::string& want) {
            error = key + " wants " + want + " (it is currently '" + current + "')";
            return false;
        };
        if (key == "hex" || (key.size() >= 4 && key.compare(key.size() - 4, 4, "Guid") == 0))
        {
            if (!v.is_string()) return bad("a hex GUID string");
            out = v.get<std::string>();
            return true;
        }
        if (v.is_boolean())
        {
            if (curBool) { out = v.get<bool>() ? "true" : "false"; return true; }
            if (curNum)  { out = v.get<bool>() ? "1" : "0"; return true; }
            return bad(arity ? "a list of numbers" : "text");
        }
        if (v.is_number())
        {
            const double d = v.get<double>();
            if (!std::isfinite(d)) return bad("a finite number");
            if (curBool) { out = d != 0.0 ? "true" : "false"; return true; }
            if (arity) return bad("a list of " + std::to_string(arity) + " numbers");
            out = NumberText(d);
            return true;
        }
        if (v.is_array())
        {
            if (!arity || v.size() != arity) return bad(arity ? "a list of " + std::to_string(arity) + " numbers"
                                                               : curNum ? "a number" : "text");
            out.clear();
            for (std::size_t i = 0; i < v.size(); ++i)
            {
                if (!v[i].is_number() || !std::isfinite(v[i].get<double>())) return bad("finite numbers");
                if (i) out += '_';
                out += NumberText(v[i].get<double>());
            }
            return true;
        }
        if (v.is_string())
        {
            if (curNum || arity || curBool) return bad(arity ? "a list of numbers" : curBool ? "a boolean" : "a number");
            std::string s = v.get<std::string>();
            for (char c : s)
                if (static_cast<unsigned char>(c) < 0x20) return bad("text without control characters");
            out = Resource::SceneText::EncodeText(s);
            return true;
        }
        return bad("a number, a list, a boolean or a string");
    }

    bool NearlyEqual(float a, float b)
    {
        return std::fabs(a - b) <= 1e-5f + 1e-6f * std::fabs(b);   // the file keeps 6 decimals
    }

    bool IsPrefabFile(const std::string& path)
    {
        std::ifstream f(Sys::U8Path(path), std::ios::binary);
        Resource::AssetHeader h{};
        if (!f.read(reinterpret_cast<char*>(&h), sizeof(h))) return false;
        return h.magic == Resource::MAGIC_PREFAB;
    }

    std::string Key(const SceneDocument::Finding& f) { return f.code + "|" + f.message; }

    Result Patch(const std::string& in, const std::string& opsPath, const std::string& out,
                 bool dryRun, const std::string& expectHash)
    {
        Result r;
        SceneDocument doc;
        if (!Load(in, doc, r)) return r;
        const Json input = FileIdentity(in);
        if (!expectHash.empty() && expectHash != input["sha256"].get<std::string>())
            return Fail(kRefused, "hash_mismatch", "the input changed since it was read: expected " + expectHash +
                                                   ", it is " + input["sha256"].get<std::string>(),
                        Json{ { "input", input } });
        if (!dryRun && out.empty()) return Fail(kUsage, "usage", "-out=<scene> is required (or -dry_run)");

        Json ops;
        {
            std::ifstream f(Sys::U8Path(opsPath), std::ios::binary);
            if (!f) return Fail(kUsage, "usage", "cannot read -ops='" + opsPath + "'");
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            ops = Json::parse(text, nullptr, false);
            if (ops.is_discarded()) return Fail(kUsage, "usage", "-ops is not JSON");
            if (ops.is_object() && ops.contains("ops")) ops = ops["ops"];
            if (!ops.is_array()) return Fail(kUsage, "usage", "-ops wants {\"ops\": [...]} or a list of ops");
        }

        std::set<std::string> before;
        for (const auto& f : doc.Validate(true)) if (f.error) before.insert(Key(f));

        Json changes = Json::array();
        std::vector<Intent> intents;
        for (std::size_t i = 0; i < ops.size(); ++i)
        {
            const Json& op = ops[i];
            const std::string name = op.is_object() ? op.value("op", "") : "";
            std::string err;
            auto refuse = [&](const char* code, const std::string& msg) {
                return Fail(kRefused, code, "op " + std::to_string(i) + " (" + name + "): " + msg, Json{ { "opIndex", i } });
            };
            auto node = [&](const char* key) -> int {
                const std::string ref = op.contains(key) ? RefText(op[key]) : std::string();
                if (ref.empty()) { err = std::string(key) + " names no node (an idx or a unique name)"; return -1; }
                return doc.Find(ref, err);
            };
            Intent it;
            it.op = name;
            if (name == "set_transform")
            {
                const int n = node("node");
                if (n < 0) return refuse("bad_node", err);
                it.hasT = Floats(op, "position", it.t, 3, err); if (!err.empty()) return refuse("bad_value", err);
                it.hasQ = Floats(op, "rotation", it.q, 4, err); if (!err.empty()) return refuse("bad_value", err);
                it.hasS = Floats(op, "scale",    it.s, 3, err); if (!err.empty()) return refuse("bad_value", err);
                if (!it.hasT && !it.hasQ && !it.hasS) return refuse("bad_op", "give position, rotation and / or scale");
                if (it.hasQ)
                {
                    const double l = std::sqrt(double(it.q[0]) * it.q[0] + double(it.q[1]) * it.q[1] + double(it.q[2]) * it.q[2] + double(it.q[3]) * it.q[3]);
                    if (l < 1e-6f) return refuse("bad_value", "rotation is a quaternion xyzw; this one has zero length");
                    for (float& c : it.q) c = float(c / l);
                }
                const Json was = NodeJson(doc.Nodes()[n]);
                if (!doc.SetTransform(n, it.hasT ? it.t : nullptr, it.hasQ ? it.q : nullptr, it.hasS ? it.s : nullptr, err))
                    return refuse("bad_value", err);
                it.idx = doc.Nodes()[n].fields.idx;
                const Json now = NodeJson(doc.Nodes()[n]);
                changes.push_back(Json{ { "op", name }, { "idx", it.idx }, { "name", was["name"] },
                                        { "before", { { "position", was["position"] }, { "rotation", was["rotation"] }, { "scale", was["scale"] } } },
                                        { "after",  { { "position", now["position"] }, { "rotation", now["rotation"] }, { "scale", now["scale"] } } } });
            }
            else if (name == "add_prefab_instance")
            {
                it.prefab = op.value("prefab", "");
                std::replace(it.prefab.begin(), it.prefab.end(), '\\', '/');
                it.name = op.value("name", "");
                if (it.prefab.empty() || it.name.empty()) return refuse("bad_op", "prefab and name are required");
                std::error_code ec;
                if (!std::filesystem::exists(Sys::U8Path(it.prefab), ec)) return refuse("prefab_missing", it.prefab + " does not exist");
                if (!IsPrefabFile(it.prefab)) return refuse("not_a_prefab", it.prefab + " is not a .ipfb (RPFB)");
                float t[3] = { 0, 0, 0 }, q[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 };
                Floats(op, "position", t, 3, err); if (!err.empty()) return refuse("bad_value", err);
                if (Floats(op, "rotation", q, 4, err))
                {
                    const double l = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3]);
                    if (l < 1e-6f) return refuse("bad_value", "rotation has zero length");
                    for (float& c : q) c = static_cast<float>(c / l);
                }
                if (!err.empty()) return refuse("bad_value", err);
                Floats(op, "scale", s, 3, err); if (!err.empty()) return refuse("bad_value", err);
                int parentIdx = -1;
                if (op.contains("parent") && !(op["parent"].is_number_integer() && op["parent"].get<long long>() == -1))
                {
                    const int p = node("parent");
                    if (p < 0) return refuse("bad_node", err);
                    parentIdx = doc.Nodes()[p].fields.idx;
                }
                // The instance's own Guid seeds its members' identities, so they stay the same from load to
                // load (SceneSerializer's PrefabRef re-instantiation).
                const std::vector<std::string> lines = {
                    "  PrefabRef: path=" + Resource::SceneText::EncodeText(it.prefab),
                    "  Guid: hex=" + ECS::Guid::Generate().ToHex(),
                };
                it.idx = doc.AddNode(parentIdx, it.name, t, q, s, lines, err);
                if (it.idx < 0) return refuse("bad_op", err);
                std::copy(t, t + 3, it.t); std::copy(q, q + 4, it.q); std::copy(s, s + 3, it.s);
                it.hasT = it.hasQ = it.hasS = true;
                changes.push_back(Json{ { "op", name }, { "idx", it.idx }, { "name", it.name }, { "prefab", it.prefab },
                                        { "parent", parentIdx }, { "position", Vec(t, 3) }, { "rotation", Vec(q, 4) },
                                        { "scale", Vec(s, 3) } });
            }
            else if (name == "set_component")
            {
                const int n = node("node");
                if (n < 0) return refuse("bad_node", err);
                it.tag = op.value("tag", "");
                if (it.tag.empty() || !op.contains("set") || !op["set"].is_object() || op["set"].empty())
                    return refuse("bad_op", "tag and a non-empty set {key: value} are required");
                const auto& nd = doc.Nodes()[n];
                std::string lineText;
                for (std::size_t k = 0; k < nd.tags.size(); ++k)
                    if (nd.tags[k] == it.tag) { lineText = doc.Lines()[nd.componentLines[k]]; break; }
                if (lineText.empty()) return refuse("no_component", "node '" + nd.fields.name + "' has no " + it.tag + " line");
                std::string tag;
                KVMap kv;
                Resource::SceneText::ParseComponentLine(lineText, tag, kv);
                for (const auto& [key, value] : Resource::Authoring::Defaults(tag)) kv.try_emplace(key, value);
                Json setJson = Json::object();
                for (auto kvIt = op["set"].begin(); kvIt != op["set"].end(); ++kvIt)
                {
                    const auto* field = Resource::Authoring::FindField(it.tag, kvIt.key());
                    if (field && kvIt.value().is_boolean() && std::string_view(field->type) != "boolean")
                        return refuse("bad_value", kvIt.key() + " requires " + field->type);
                    if (field) kv.try_emplace(kvIt.key(), Resource::Authoring::SlotDefault(kvIt.key()));
                    const auto cur = kv.find(kvIt.key());
                    std::string text;
                    const bool textField = field && (std::string_view(field->type) == "string" || std::string_view(field->type) == "asset");
                    if (textField && !kvIt.value().is_string()) return refuse("bad_value", kvIt.key() + " requires text");
                    if (cur != kv.end() && !ValueText(kvIt.key(), kvIt.value(), textField ? "" : cur->second, text, err))
                        return refuse("bad_value", err);
                    if (cur == kv.end()) text = "?";   // SetComponentValues names the missing key
                    it.values.emplace_back(kvIt.key(), text);
                    setJson[kvIt.key()] = text;
                }
                std::string errorCode;
                if (!doc.SetComponentValues(n, it.tag, it.values, err, &errorCode)) return refuse(errorCode.c_str(), err);
                it.idx = doc.Nodes()[n].fields.idx;
                changes.push_back(Json{ { "op", name }, { "idx", it.idx }, { "name", nd.fields.name },
                                        { "tag", it.tag }, { "set", setJson } });
            }
            else if (name == "rename")
            {
                const int n = node("node");
                if (n < 0) return refuse("bad_node", err);
                it.name = op.value("name", "");
                const std::string was = doc.Nodes()[n].fields.name;
                if (!doc.Rename(n, it.name, err)) return refuse("bad_op", err);
                it.idx = doc.Nodes()[n].fields.idx;
                changes.push_back(Json{ { "op", name }, { "idx", it.idx }, { "before", was }, { "after", it.name } });
            }
            else if (name == "remove_node")
            {
                const int n = node("node");
                if (n < 0) return refuse("bad_node", err);
                it.idx = doc.Nodes()[n].fields.idx;
                it.name = doc.Nodes()[n].fields.name;
                if (!doc.RemoveNode(n, err)) return refuse("refused", err);
                changes.push_back(Json{ { "op", name }, { "idx", it.idx }, { "name", it.name } });
            }
            else
                return refuse("unknown_op", "ops are set_transform, add_prefab_instance, set_component, rename, remove_node");
            intents.push_back(std::move(it));
        }

        // Only errors the patch introduced refuse it: a scene that already had a missing reference can
        // still be moved around.
        const auto after = doc.Validate(true);
        Json newErrors = Json::array(), preexisting = Json::array();
        for (const auto& f : after)
            if (f.error)
                (before.count(Key(f)) ? preexisting : newErrors)
                    .push_back(Json{ { "code", f.code }, { "line", f.line }, { "message", f.message } });
        Json validation{ { "errors", newErrors }, { "preexistingErrors", preexisting },
                         { "warnings", FindingsJson(after, false) } };
        if (!newErrors.empty())
            return Fail(kRefused, "invalid_result", "the patched scene would have errors it did not have before",
                        Json{ { "validation", validation }, { "changes", changes } });

        const std::vector<std::uint8_t> bytes = doc.Bytes();
        r.j = Json{ { "ok", true }, { "dryRun", dryRun }, { "input", input }, { "changes", changes },
                    { "validation", validation }, { "bytes", bytes.size() } };
        if (dryRun) return r;

        // Atomic replace: write beside the target, then rename over it.
        const std::string tmp = out + ".tmp" + std::to_string(::GetCurrentProcessId());
        if (!Sys::WriteFileBytes(tmp, bytes.data(), bytes.size()))
            return Fail(kUsage, "write_failed", "cannot write '" + tmp + "'");
        std::error_code ec;
        std::filesystem::rename(Sys::U8Path(tmp), Sys::U8Path(out), ec);
        if (ec)
        {
            std::filesystem::remove(Sys::U8Path(tmp), ec);
            return Fail(kUsage, "write_failed", "cannot replace '" + out + "'");
        }
        r.j["output"] = FileIdentity(out);

        // Read it back: the file says what the ops asked for.
        SceneDocument back;
        std::string err;
        Json problems = Json::array();
        if (!back.LoadFile(out, err)) problems.push_back("the written file does not load: " + err);
        else if (back.Bytes() != bytes) problems.push_back("the written file differs from what was written");
        else
            for (const Intent& it : intents)
            {
                const int n = FindByIdx(back, it.idx);
                if (it.op == "remove_node")
                {
                    if (n >= 0) problems.push_back("node " + std::to_string(it.idx) + " is still there");
                    continue;
                }
                if (n < 0) { problems.push_back("node " + std::to_string(it.idx) + " is missing"); continue; }
                const auto& f = back.Nodes()[n].fields;
                auto check = [&](bool has, const float* want, const float* got, int count, const char* what) {
                    if (!has) return;
                    for (int k = 0; k < count; ++k)
                        if (!NearlyEqual(got[k], want[k]))
                        { problems.push_back(std::string(what) + " of node " + std::to_string(it.idx) + " did not take"); return; }
                };
                check(it.hasT, it.t, f.t, 3, "position");
                check(it.hasQ, it.q, f.q, 4, "rotation");
                check(it.hasS, it.s, f.s, 3, "scale");
                if ((it.op == "rename" || it.op == "add_prefab_instance") && f.name != it.name)
                    problems.push_back("node " + std::to_string(it.idx) + " is named '" + f.name + "'");
                if (it.op == "add_prefab_instance")
                {
                    const auto& tags = back.Nodes()[n].tags;
                    if (std::find(tags.begin(), tags.end(), "PrefabRef") == tags.end())
                        problems.push_back("node " + std::to_string(it.idx) + " has no PrefabRef");
                }
                if (it.op == "set_component")
                {
                    const auto& nd = back.Nodes()[n];
                    for (std::size_t k = 0; k < nd.tags.size(); ++k)
                    {
                        if (nd.tags[k] != it.tag) continue;
                        std::string tag;
                        KVMap kv;
                        Resource::SceneText::ParseComponentLine(back.Lines()[nd.componentLines[k]], tag, kv);
                        for (const auto& [key, text] : it.values)
                            if (kv[key] != text)
                                problems.push_back(it.tag + "." + key + " of node " + std::to_string(it.idx) + " did not take");
                    }
                }
            }
        r.j["verified"] = problems.empty();
        if (!problems.empty())
        {
            r.j["ok"] = false;
            r.j["error"] = "verify_failed";
            r.j["problems"] = problems;
            r.code = kRefused;
        }
        return r;
    }

    // ==== roundtrip ====
    Result Roundtrip(const std::string& path)
    {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        const auto root = Sys::U8Path(path);
        if (std::filesystem::is_directory(root, ec))
        {
            for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec) && it->path().extension() == ".iscene") files.push_back(it->path());
            std::sort(files.begin(), files.end());
        }
        else if (std::filesystem::exists(root, ec)) files.push_back(root);
        else return Fail(kUsage, "usage", "no scene or directory at '" + path + "'");

        Json list = Json::array();
        std::size_t different = 0, failed = 0;
        for (const auto& p : files)
        {
            std::ifstream f(p, std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            SceneDocument doc;
            std::string err;
            Json e{ { "path", Sys::PathToUtf8(p) }, { "bytes", bytes.size() } };
            if (!doc.LoadBytes(bytes, err)) { e["error"] = err; ++failed; }
            else
            {
                const bool same = doc.Bytes() == bytes;
                e["identical"] = same;
                e["nodes"] = doc.Nodes().size();
                if (!same) ++different;
            }
            list.push_back(std::move(e));
        }
        Result r;
        r.j = Json{ { "ok", different == 0 && failed == 0 }, { "files", list }, { "count", files.size() },
                    { "different", different }, { "unreadable", failed } };
        r.code = (different == 0 && failed == 0) ? kDone : kRefused;
        return r;
    }

    // ==== catalog ====
    // Every asset under the roots: its type, guid (its .meta sidecar - the identity authority), size,
    // what its header says, and for the text containers every reference it makes, resolved the way
    // the loader would: the path, else its $g_ guid, else missing - with same-name files elsewhere as
    // candidates. Plus the identity layer's own problems: duplicate guids, .meta files whose asset is
    // gone, assets with no .meta. Read-only. tools/asset_tool.py (find / deps / referrers / check)
    // answers queries from the JSON; nothing here is a second parser - references come from
    // Resource::AssetRefs, the loader's own definition.
    namespace fs = std::filesystem;

    constexpr const char* kCatalogExts[] = {
        ".itex", ".imat", ".imatinst", ".ipfb", ".iscn", ".iscene", ".ianim", ".iskel", ".meshlib", ".imsh",
        ".aclip", ".ivfx", ".iseq", ".inav", ".ppprofile", ".imorph",
        ".imatgraph", ".imatfunc",   // editor-only JSON - the textures and functions a graph names
        ".iui",                      // a UI asset - its images' textures, its texts' fonts / shaders
        ".ifacemap", ".iface",       // face tracking: a mapping (named by FaceTracking profile=), a take
    };

    // A .iui's references through its own codec, keyed "<node id> <field>" (image.texture,
    // text.font, text.customShader).
    std::vector<Resource::AssetRefs::TextRef> UIAssetRefs(const std::string& text)
    {
        std::vector<Resource::AssetRefs::TextRef> out;
        UI::UIAsset a;
        std::vector<UI::UIAssetIssue> issues;
        if (!UI::ParseUIAsset(text, a, issues)) return out;
        for (const UI::UIAssetRef& r : UI::UIAssetRefs(a))
        {
            Resource::AssetRefs::TextRef t;
            t.key  = r.id + " " + r.field;
            t.tag  = "UI";
            t.path = r.path;
            out.push_back(std::move(t));
        }
        return out;
    }

    // A material graph's / function's references - its nodes' string props that name an asset (a
    // texture, a Material Function), keyed "node <id> <prop>".
    std::vector<Resource::AssetRefs::TextRef> GraphRefs(const std::string& text)
    {
        std::vector<Resource::AssetRefs::TextRef> out;
        MaterialGraph::Graph g;
        std::string err;
        if (!MaterialGraph::ParseGraph(text, g, err)) return out;
        for (const MaterialGraph::Node& n : g.nodes)
            for (const auto& [key, v] : n.props)
                if (v.kind == MaterialGraph::Value::Kind::String && Resource::AssetRefs::ValueIsAssetPath(v.str))
                {
                    Resource::AssetRefs::TextRef r;
                    r.key  = "node " + std::to_string(n.id) + " " + key;
                    r.path = v.str;
                    out.push_back(std::move(r));
                }
        return out;
    }

    // A material's `materialGraph = <path>` - not a runtime reference (the runtime
    // never reads a graph), but the catalog's: who uses this graph.
    void AppendMaterialGraphRef(const std::string& text, std::vector<Resource::AssetRefs::TextRef>& refs)
    {
        std::istringstream in(text);
        std::string line;
        unsigned n = 0;
        while (std::getline(in, line))
        {
            ++n;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("materialGraph", 0) != 0) continue;
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string v = line.substr(eq + 1);
            v.erase(0, v.find_first_not_of(" \t"));
            v.erase(v.find_last_not_of(" \t") + 1);
            if (v.empty() || v == "<none>") continue;
            Resource::AssetRefs::TextRef r;
            r.key  = "materialGraph";
            r.path = v;
            r.line = n;
            refs.push_back(std::move(r));
        }
    }

    std::string LowerAscii(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }

    std::string SlashPath(const fs::path& p)
    {
        std::string s = Sys::PathToUtf8(p);
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    }

    // The first `max` bytes (all of them when max == 0).
    std::string ReadBytes(const fs::path& p, std::size_t max = 0)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f) return {};
        if (max == 0) return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string s(max, '\0');
        f.read(s.data(), static_cast<std::streamsize>(max));
        s.resize(static_cast<std::size_t>(f.gcount()));
        return s;
    }

    // An asset's bytes as the engine reads them: the pak, the search roots (a
    // worktree's shared content root too), then the path as given.
    std::string ReadAssetBytes(const std::string& path)
    {
        std::vector<std::uint8_t> b;
        if (!Resource::AssetFS::Get().ReadFile(path, b)) return {};
        return std::string(b.begin(), b.end());
    }

    // The files with extension @p ext under @p dir that only a worktree's
    // shared content root has - the binary payload a sparse worktree leaves to
    // the main checkout (Resource/SharedContent.h). A directory walk of this
    // checkout alone never sees them; a command that reports on a folder must
    // list them, and one that WRITES must refuse them (never the other tree).
    std::vector<std::string> SharedOnlyFiles(const std::string& dir, const char* ext)
    {
        std::vector<std::string> out, all;
        auto& afs = Resource::AssetFS::Get();
        if (afs.SharedTwinDir(dir).empty()) return out;
        afs.EnumerateLoose(dir, all);
        // Spelled as a recursive_directory_iterator over `dir` spells its own
        // files - `dir` as given, the rest in native separators - so a report
        // reads the same in a shared worktree as in a full checkout.
        std::string base = dir;
        for (char& c : base) if (c == '\\') c = '/';
        while (base.rfind("./", 0) == 0) base.erase(0, 2);
        while (!base.empty() && base.back() == '/') base.pop_back();
        for (const std::string& rel : all)
        {
            if (LowerAscii(Sys::PathToUtf8(Sys::U8Path(rel).extension())) != ext || !afs.ResolveLocal(rel).empty())
                continue;
            const bool under = rel.size() > base.size() && LowerAscii(rel.substr(0, base.size())) == LowerAscii(base) &&
                               rel[base.size()] == '/';
            out.push_back(under ? Sys::PathToUtf8(Sys::U8Path(dir) / Sys::U8Path(rel.substr(base.size() + 1)).make_preferred())
                                : rel);
        }
        return out;
    }

    bool OnlySharedHas(const std::string& path)
    {
        auto& afs = Resource::AssetFS::Get();
        return afs.ResolveLocal(path).empty() && afs.IsSharedPath(afs.ResolveOnDisk(path));
    }

    std::string MetaGuid(const fs::path& meta)
    {
        std::istringstream in(ReadBytes(meta));
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq), val = line.substr(eq + 1);
            const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
            key.erase(std::remove_if(key.begin(), key.end(), space), key.end());
            val.erase(std::remove_if(val.begin(), val.end(), space), val.end());
            if (key != "guid") continue;
            val.erase(std::remove(val.begin(), val.end(), '-'), val.end());   // a UUID form is accepted too
            const ECS::Guid g = ECS::Guid::FromHex(val);
            return g.IsValid() ? LowerAscii(g.ToHex()) : std::string{};
        }
        return {};
    }

    template <class T>
    bool ReadMeta(const std::string& blob, T& out)
    {
        if (blob.size() < sizeof(Resource::AssetHeader) + sizeof(T)) return false;
        std::memcpy(&out, blob.data() + sizeof(Resource::AssetHeader), sizeof(T));
        return true;
    }

    // The text payload of a container (.iscene / .ipfb / .imat / .imatinst / .iscn): header,
    // metadata, text, NUL - read as the loaders read it, to the NUL within dataSize
    // (Resource::GetPayloadText). Not dataSize - 1: a PmxImporter .iscn may have no NUL, and that would
    // drop its last byte.
    bool ContainerText(const std::string& blob, std::string& text)
    {
        if (blob.size() < sizeof(Resource::AssetHeader)) return false;
        Resource::AssetHeader h{};
        std::memcpy(&h, blob.data(), sizeof(h));
        const std::size_t off = sizeof(h) + h.metadataSize;
        if (h.dataSize == 0 || off + h.dataSize > blob.size()) return false;
        text = Resource::GetPayloadText(reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size());
        return true;
    }

    // What the header says, per type. Headers only - nothing is decoded.
    Json HeaderInfo(const std::string& ext, const std::string& blob)
    {
        Json m = Json::object();
        if (blob.size() < sizeof(Resource::AssetHeader)) { m["unreadable"] = true; return m; }
        Resource::AssetHeader h{};
        std::memcpy(&h, blob.data(), sizeof(h));
        m["version"] = h.version;
        m["magic"]   = Resource::MagicText(h.magic);
        if (const Resource::AssetFormat* f = Resource::FindAssetFormat(h.magic))
        {
            m["fileFormat"]     = f->kind;
            m["currentVersion"] = f->current;
            m["minimumVersion"] = f->minimum;
            // newer: this engine refuses it; older_than_minimum: it no longer reads it; upgradable: it
            // reads it through the migration chain (asset_tool.py upgrade re-saves it as current).
            m["versionStatus"]  = Resource::VersionStatus(*f, h.version);
        }
        else
            m["fileFormat"] = "unlisted";
        if (ext == ".itex")
        {
            Resource::TextureMetadata t{};
            if (ReadMeta(blob, t))
            {
                static const std::set<unsigned> kSrgb = { 29, 72, 75, 78, 91, 93, 99 };   // *_SRGB DXGI formats
                m["width"] = t.width; m["height"] = t.height; m["depth"] = t.depth;
                m["mips"] = t.mipLevels; m["format"] = t.format; m["srgb"] = kSrgb.count(t.format) != 0;
                m["dimension"] = t.dimension;
            }
        }
        else if (ext == ".meshlib")
        {
            Resource::MeshLibraryMetadata ml{};
            if (ReadMeta(blob, ml))
            { m["meshes"] = ml.meshCount; m["vertices"] = ml.vertexCount; m["indices"] = ml.indexCount; }
        }
        else if (ext == ".ianim")
        {
            Resource::AnimationMetadata a{};
            if (ReadMeta(blob, a))
            {
                m["clips"] = a.clipCount; m["morphClips"] = a.morphClipCount; m["cameraClips"] = a.cameraClipCount;
                const std::size_t off = sizeof(Resource::AssetHeader) + h.metadataSize;
                if (a.clipCount > 0 && blob.size() >= off + 16 + 64)   // the first clip's header
                {
                    std::uint32_t frames = 0; float duration = 0, fps = 0; char name[65] = {};
                    std::memcpy(&frames, blob.data() + off + 4, 4);
                    std::memcpy(&duration, blob.data() + off + 8, 4);
                    std::memcpy(&fps, blob.data() + off + 12, 4);
                    std::memcpy(name, blob.data() + off + 16, 64);
                    m["firstClip"] = Json{ { "name", std::string(name) }, { "frames", frames },
                                           { "duration", duration }, { "fps", fps } };
                }
            }
        }
        else if (ext == ".iskel")
        {
            Resource::SkeletonMetadata s{};
            if (ReadMeta(blob, s)) { m["bones"] = s.boneCount; m["clips"] = s.clipCount; }
        }
        else if (ext == ".aclip")
        {
            Resource::AudioClipMetadata a{};
            if (ReadMeta(blob, a)) { m["duration"] = a.duration; m["sampleRate"] = a.sampleRate; m["channels"] = a.channels; }
        }
        else if (ext == ".ipfb")
        {
            Resource::PrefabMetadata p{};
            if (ReadMeta(blob, p)) m["nodes"] = p.nodeCount;
        }
        else if (ext == ".iscn")
        {
            Resource::SceneMetadata s{};
            if (ReadMeta(blob, s)) { m["nodes"] = s.nodeCount; m["meshFiles"] = s.meshFileCount; }
        }
        return m;
    }

    // A .iscn's F / K / M lines name files RELATIVE to the .iscn's folder, in
    // a positional grammar (a material name may contain spaces): the loader's
    // own parse, SceneInstanceLoader::ListFileReferences.
    std::vector<Resource::AssetRefs::TextRef> IscnRefs(const std::string& text, const fs::path& iscn)
    {
        std::vector<Resource::AssetRefs::TextRef> refs;
        for (const auto& fr : SceneInstanceLoader::ListFileReferences(text, SlashPath(iscn)))
        {
            Resource::AssetRefs::TextRef r;
            r.key  = std::string(1, fr.kind) + ":" + fr.key;
            r.path = fr.path;
            r.line = fr.line;
            refs.push_back(std::move(r));
        }
        return refs;
    }

    // Plain text with k=v or bare tokens (.ivfx, .iseq): any token that has
    // the shape of an asset path.
    std::vector<Resource::AssetRefs::TextRef> PlainTextRefs(const std::string& text)
    {
        std::vector<Resource::AssetRefs::TextRef> refs;
        std::istringstream in(text);
        std::string line;
        unsigned n = 0;
        while (std::getline(in, line))
        {
            ++n;
            std::istringstream toks(line);
            std::string tok;
            while (toks >> tok)
            {
                const auto eq = tok.find('=');
                std::string key = eq == std::string::npos ? std::string() : tok.substr(0, eq);
                std::string val = eq == std::string::npos ? tok : tok.substr(eq + 1);
                if (!Resource::AssetRefs::ValueIsAssetPath(val)) continue;
                Resource::AssetRefs::TextRef r;
                r.key  = key;
                r.path = Resource::AssetRefs::PercentDecode(val);
                r.line = n;
                refs.push_back(std::move(r));
            }
        }
        return refs;
    }

    std::string BaseNameLower(const std::string& path)
    {
        const auto slash = path.find_last_of("/\\");
        return LowerAscii(slash == std::string::npos ? path : path.substr(slash + 1));
    }

    Result MaterialPatch(const std::string& path, const std::string& opsPath, const std::string& out,
                         bool dryRun, const std::string& expected)
    {
        const Json input = FileIdentity(path);
        if (input["sha256"] == "") return Fail(kUsage, "load_failed", path);
        if (!expected.empty() && input["sha256"] != expected) return Fail(kRefused, "hash_mismatch", path);
        const std::string sourceBlob = ReadAssetBytes(path);
        std::string sourceText;
        if (!ContainerText(sourceBlob, sourceText)) return Fail(kRefused, "load_failed", path);
        Resource::AssetHeader header{};
        std::memcpy(&header, sourceBlob.data(), sizeof(header));
        if (header.magic != Resource::MAGIC_MATERIAL ||
            header.version > Resource::CurrentVersion(Resource::MAGIC_MATERIAL) ||
            header.version < Resource::MinimumVersion(Resource::MAGIC_MATERIAL) ||
            header.flags != 0 || header.metadataSize < sizeof(Resource::MaterialMetadata))
            return Fail(kRefused, "unsupported_version", "material container version or flags are not supported for authoring");
        auto trim = [](std::string s) { const auto a = s.find_first_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t\r") - a + 1); };
        {
            std::istringstream lines(sourceText); std::string line;
            while (std::getline(lines, line))
            {
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                const auto key = trim(line.substr(0, eq));
                if (key != "matfmt" && key != "imatfmt") continue;
                const auto value = trim(line.substr(eq + 1)); char* end = nullptr;
                const long version = std::strtol(value.c_str(), &end, 10);
                if (end == value.c_str() || *end || version < 0 || version > (key == "matfmt" ? 3 : 2))
                    return Fail(kRefused, "unsupported_version", key);
            }
        }
        const Json ops = Json::parse(ReadBytes(Sys::U8Path(opsPath)), nullptr, false);
        if (!ops.is_object() || !ops.contains("set") || !ops["set"].is_object() || ops["set"].empty())
            return Fail(kUsage, "bad_op", "set must be a non-empty object");
        MaterialComponent mat;
        Resource::MaterialLoadInfo info;
        if (!Resource::LoadMaterial(path, mat, &info) || !info.parentResolved) return Fail(kRefused, "load_failed", path);
        std::vector<std::pair<std::string, std::string>> values;
        std::string delta, why;
        for (auto it = ops["set"].begin(); it != ops["set"].end(); ++it)
        {
            const auto* field = Resource::Authoring::FindField("Material", it.key());
            // Offline contract is deliberately the discoverable PBR field set.
            if (!field) return Fail(kRefused, "unknown_key", "Material." + it.key());
            std::string value;
            if (field->arity == 1 && it->is_number()) value = NumberText(it->get<double>());
            else if (field->arity > 1 && it->is_array() && it->size() == field->arity)
            {
                for (const auto& v : *it)
                {
                    if (!v.is_number()) return Fail(kRefused, "bad_value", it.key() + " requires numeric channels");
                    if (!value.empty()) value += ' ';
                    value += NumberText(v.get<double>());
                }
            }
            else return Fail(kRefused, "bad_value", it.key() + " has wrong type or arity");
            values.emplace_back(it.key(), value);
            delta += it.key() + " = " + value + "\n";
            info.overrideKeys.insert(it.key());
        }
        if (!Resource::ValidateMaterialEdit(mat, values, why)) return Fail(kRefused, "bad_value", why);
        const std::string before = Resource::BuildMaterialText(mat);
        Resource::ApplyMaterialTextDelta(mat, delta);
        // Patch only the authored PBR lines in the original container. Rebuilding from
        // MaterialComponent would discard unknown fields, comments and instance-only tokens; the native
        // loader remains the value codec.
        std::map<std::string, std::string> replacements(values.begin(), values.end());
        std::set<std::string> seen;
        std::string patchedText;
        for (std::size_t start = 0; start < sourceText.size(); )
        {
            const auto nl = sourceText.find('\n', start);
            const auto end = nl == std::string::npos ? sourceText.size() : nl;
            const auto line = sourceText.substr(start, end - start);
            const auto eq = line.find('=');
            const auto key = eq == std::string::npos ? std::string() : trim(line.substr(0, eq));
            if (const auto it = replacements.find(key); it != replacements.end())
            {
                patchedText += key + " = " + it->second;
                if (!line.empty() && line.back() == '\r') patchedText += '\r';
                seen.insert(key);
            }
            else patchedText += line;
            if (nl != std::string::npos) patchedText += '\n';
            start = nl == std::string::npos ? sourceText.size() : nl + 1;
        }
        for (const auto& [key, value] : replacements) if (!seen.count(key))
        {
            if (!patchedText.empty() && patchedText.back() != '\n') patchedText += '\n';
            patchedText += key + " = " + value + "\n";
        }
        Result result;
        result.j = {{"ok", true}, {"input", input}, {"before", before}, {"after", Resource::BuildMaterialText(mat)},
                    {"dryRun", dryRun}, {"effect", "new material asset; assign/reload separately"}};
        if (dryRun) return result;
        const fs::path target = fs::absolute(Sys::U8Path(out));
        const auto ext = target.extension();
        if (out.empty() || ext != (info.parentPath.empty() ? L".imat" : L".imatinst"))
            return Fail(kUsage, "bad_output", "new output must retain master/instance type");
        if (fs::exists(target)) return Fail(kRefused, "destination_exists", out);
        fs::create_directories(target.parent_path());
        wchar_t temp[MAX_PATH];
        if (!GetTempFileNameW(target.parent_path().c_str(), L"mat", 0, temp)) return Fail(kRefused, "write_failed", out);
        const std::string staged = Sys::PathToUtf8(fs::path(temp));
        std::string blob = sourceBlob.substr(0, sizeof(header) + header.metadataSize);
        header.dataSize = static_cast<std::uint32_t>(patchedText.size() + 1);
        std::memcpy(blob.data(), &header, sizeof(header));
        const auto textLength = static_cast<std::uint32_t>(patchedText.size());
        std::memcpy(blob.data() + sizeof(header), &textLength, sizeof(textLength));
        blob += patchedText; blob += '\0';
        auto file = Sys::OpenOfstream(staged, std::ios::binary);
        file.write(blob.data(), static_cast<std::streamsize>(blob.size())); file.flush();
        const bool saved = file.good(); file.close();
        const bool unchanged = FileIdentity(path)["sha256"] == input["sha256"];
        const bool published = saved && unchanged && CreateHardLinkW(target.c_str(), temp, nullptr);
        std::error_code ec;
        fs::remove(fs::path(temp), ec);
        if (!published) return Fail(kRefused, unchanged ? "publication_failed" : "hash_mismatch", out);
        result.j["output"] = FileIdentity(Sys::PathToUtf8(target));
        return result;
    }

    // Cache only codec output. GUIDs, existence, candidates and repair targets
    // are rebuilt on every invocation, even when every parser entry is reused.
    std::string CatalogDigest(const Json& value)
    {
        try
        {
            const std::string text = value.dump();
            return Sys::Sha256Hex(text.data(), text.size());
        }
        catch (const Json::type_error&)
        {
            // Asset text may contain invalid UTF-8. The public report replaces those bytes, so it cannot
            // round-trip raw parser output losslessly.
            return {}; // Uncacheable: parse again instead of changing semantics.
        }
    }

    Result Catalog(const std::string& rootsArg, const std::string& cachePath, bool incremental)
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::wstring module(32768, L'\0');
        const DWORD moduleSize = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        const std::string producer = incremental && moduleSize && moduleSize < module.size()
            ? Sys::Sha256FileHex(Sys::PathToUtf8(fs::path(module.substr(0, moduleSize)))) : std::string();
        Json previous = Json::object();
        if (!cachePath.empty() && !producer.empty())
        {
            const auto saved = Json::parse(ReadBytes(Sys::U8Path(cachePath)), nullptr, false);
            if (saved.is_object() && saved.contains("parseCache") && saved["parseCache"].is_object())
            {
                const auto& c = saved["parseCache"];
                if (c.contains("version") && c["version"] == 1 && c.contains("producer") &&
                    c["producer"] == producer && c.contains("entries") && c["entries"].is_object())
                    previous = c["entries"];
            }
        }
        Json parsedEntries = Json::object();
        std::size_t parsed = 0, reused = 0;
        std::vector<std::string> roots;
        {
            std::string s = rootsArg.empty() ? std::string("asset;engine") : rootsArg;
            std::size_t start = 0;
            while (start <= s.size())
            {
                const std::size_t semi = s.find(';', start);
                const std::string one = s.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
                if (!one.empty()) roots.push_back(one);
                if (semi == std::string::npos) break;
                start = semi + 1;
            }
        }

        // Pass 1: the files - this checkout's, and in a worktree that shares the
        // main checkout's binary assets (Resource/SharedContent.h) the ones it
        // reads from there: the same tree the engine sees. Paths stay
        // root-relative ("asset/..."); `disk` is where each one is read.
        const auto& afs = Resource::AssetFS::Get();
        std::vector<std::string> files, metas;
        std::error_code ec;
        for (const std::string& root : roots)
        {
            std::vector<std::string> found;
            afs.EnumerateLoose(root, found);
            if (found.empty() && !fs::is_directory(Sys::U8Path(root), ec))
                return Fail(kUsage, "usage", "no directory '" + root + "'");
            for (std::string& rel : found)
            {
                const std::string ext = LowerAscii(Sys::PathToUtf8(Sys::U8Path(rel).extension()));
                if (ext == ".meta") { metas.push_back(std::move(rel)); continue; }
                if (std::find(std::begin(kCatalogExts), std::end(kCatalogExts), ext) != std::end(kCatalogExts))
                    files.push_back(std::move(rel));
            }
        }
        const auto byPath = [](const std::string& a, const std::string& b) { return Sys::U8Path(a) < Sys::U8Path(b); };
        std::sort(files.begin(), files.end(), byPath);
        std::sort(metas.begin(), metas.end(), byPath);
        const auto disk = [&afs](const std::string& rel)
        {
            const std::string d = afs.ResolveOnDisk(rel);
            return Sys::U8Path(d.empty() ? rel : d);
        };

        // The identity layer: guid -> asset(s), asset -> guid, orphans.
        std::map<std::string, std::vector<std::string>> byGuid;
        std::unordered_map<std::string, std::string> guidOf;           // normalized path -> guid
        Json orphanMetas = Json::array();
        for (const std::string& m : metas)
        {
            const std::string assetPath = m.substr(0, m.size() - 5);  // x.itex.meta -> x.itex
            if (!afs.ExistsExact(assetPath)) { orphanMetas.push_back(m); continue; }
            const std::string g = MetaGuid(disk(m));
            if (g.empty()) continue;
            byGuid[g].push_back(assetPath);
            guidOf[Resource::AssetRefs::NormalizeRefPath(assetPath)] = g;
        }

        // Every catalogued path, for resolution and for "a file of this name elsewhere".
        std::unordered_set<std::string> known;
        std::unordered_map<std::string, std::vector<std::string>> byName;
        for (const std::string& p : files)
        {
            known.insert(Resource::AssetRefs::NormalizeRefPath(p));
            byName[BaseNameLower(p)].push_back(p);
        }

        Json assets = Json::array(), broken = Json::array(), repairs = Json::array();
        Json unsupportedVersions = Json::array();
        std::map<std::string, std::map<unsigned, std::size_t>> versionCounts;   // format -> version -> files
        std::map<std::string, std::size_t> counts;
        std::size_t unstamped = 0, refTotal = 0;
        for (const std::string& p : files)
        {
            const fs::path f = disk(p);
            const std::string ext = LowerAscii(Sys::PathToUtf8(Sys::U8Path(p).extension()));
            const bool container = ext == ".iscene" || ext == ".ipfb" || ext == ".imat" || ext == ".imatinst" ||
                                   ext == ".iscn";
            const bool plainText = ext == ".ivfx" || ext == ".iseq";
            const bool graphText = ext == ".imatgraph" || ext == ".imatfunc";
            const bool uiText    = ext == ".iui";
            const std::string blob = ReadBytes(f, (container || plainText || graphText || uiText) ? 0 : 256);
            Json e{ { "path", p }, { "type", ext.substr(1) },
                    { "bytes", static_cast<std::uint64_t>(fs::file_size(f, ec)) } };
            const auto gIt = guidOf.find(Resource::AssetRefs::NormalizeRefPath(p));
            if (gIt != guidOf.end()) e["guid"] = gIt->second; else ++unstamped;
            ++counts[ext.substr(1)];
            std::vector<Resource::AssetRefs::TextRef> refs;
            // Binary formats use only their header; containers/text use all bytes.
            // Include the resolved path because .iscn references are folder-relative.
            const std::string inputHash = incremental ? CatalogDigest(Json::array({
                SlashPath(fs::absolute(f).lexically_normal()), ext, e["bytes"],
                Sys::Sha256Hex(blob.data(), blob.size()) })) : std::string();
            Json raw;
            bool hit = false;
            if (const auto old = previous.find(p); old != previous.end() && old->is_object())
            {
                try
                {
                    const std::string digest = CatalogDigest(old->at("raw"));
                    if (!inputHash.empty() && !digest.empty() && old->at("input") == inputHash && old->at("digest") == digest)
                    {
                        raw = old->at("raw");
                        if (!raw.is_object() || !raw.at("refs").is_array()) throw std::runtime_error("cache shape");
                        for (const auto& r : raw.at("refs"))
                            refs.push_back({ r.at("key").get<std::string>(), r.at("tag").get<std::string>(),
                                r.at("path").get<std::string>(), r.at("guid").get<std::string>(),
                                r.at("line").get<unsigned>(), r.at("inline").get<bool>() });
                        if (raw.contains("meta")) e["meta"] = raw.at("meta");
                        if (raw.contains("unreadable")) e["unreadable"] = raw.at("unreadable").get<bool>();
                        hit = true;
                    }
                }
                catch (const std::exception&) { refs.clear(); e.erase("meta"); e.erase("unreadable"); }
            }
            if (hit) ++reused;
            else
            {
                ++parsed;
                if (!plainText && !graphText && !uiText) e["meta"] = HeaderInfo(ext, blob);
                if (container)
                {
                    std::string text;
                    if (!ContainerText(blob, text)) e["unreadable"] = true;
                    else if (ext == ".imat" || ext == ".imatinst")
                    {
                        refs = Resource::AssetRefs::ListImatTextRefs(text);
                        AppendMaterialGraphRef(text, refs);
                    }
                    else if (ext == ".iscn")                        refs = IscnRefs(text, f);
                    else
                    {
                        refs = Resource::AssetRefs::ListSceneTextRefs(text);
                        // Composition: which components, how many ("  Tag: ..." lines) -
                        // what tells a camera prefab from one that draws a mesh.
                        std::map<std::string, int> tags;
                        std::istringstream in(text);
                        std::string line;
                        while (std::getline(in, line))
                            if (line.size() > 3 && line[0] == ' ' && line[1] == ' ')
                                if (const auto colon = line.find(':', 2); colon != std::string::npos)
                                    ++tags[line.substr(2, colon - 2)];
                        Json comp = Json::object();
                        for (const auto& [t, n] : tags) comp[t] = n;
                        e["meta"]["components"] = comp;
                    }
                }
                else if (plainText)
                    refs = PlainTextRefs(blob);
                else if (graphText)
                    refs = GraphRefs(blob);
                else if (uiText)
                    refs = UIAssetRefs(blob);

                if (incremental)
                {
                    raw = Json{ { "refs", Json::array() } };
                    if (e.contains("meta")) raw["meta"] = e["meta"];
                    if (e.contains("unreadable")) raw["unreadable"] = e["unreadable"];
                    for (const auto& r : refs)
                        raw["refs"].push_back(Json{ { "key", r.key }, { "tag", r.tag }, { "path", r.path },
                            { "guid", r.guidHex }, { "line", r.line }, { "inline", r.inlineMaterial } });
                }
            }
            if (incremental)
                parsedEntries[p] = Json{ { "input", inputHash }, { "raw", raw }, { "digest", CatalogDigest(raw) } };

            // The format and version census: one row per format, and each file this engine will not read.
            if (e.contains("meta") && e["meta"].contains("fileFormat"))
            {
                const Json& meta = e["meta"];
                const std::string status = meta.value("versionStatus", std::string("current"));
                ++versionCounts[meta["fileFormat"].get<std::string>()][meta.value("version", 0u)];
                if (status == "newer" || status == "older_than_minimum")
                    unsupportedVersions.push_back(Json{ { "path", p }, { "format", meta["fileFormat"] },
                        { "version", meta.value("version", 0u) }, { "current", meta.value("currentVersion", 0u) },
                        { "minimum", meta.value("minimumVersion", 0u) }, { "status", status } });
            }

            if (!refs.empty())
            {
                Json ra = Json::array();
                for (const auto& r : refs)
                {
                    ++refTotal;
                    Json rj{ { "key", r.key }, { "path", r.path }, { "line", r.line } };
                    if (!r.tag.empty())      rj["tag"] = r.tag;
                    if (!r.guidHex.empty())  rj["guid"] = r.guidHex;
                    if (r.inlineMaterial)    rj["inline"] = true;
                    const std::string norm = Resource::AssetRefs::NormalizeRefPath(r.path);
                    if (known.count(norm) || afs.ExistsExact(r.path))
                        rj["state"] = "ok";
                    else
                    {
                        const auto g = r.guidHex.empty() ? byGuid.end() : byGuid.find(LowerAscii(r.guidHex));
                        if (g != byGuid.end() && !g->second.empty())
                        {
                            rj["state"]  = "guid_repair";
                            rj["target"] = g->second.front();
                            repairs.push_back(Json{ { "from", p }, { "key", r.key }, { "path", r.path },
                                                    { "target", g->second.front() }, { "line", r.line } });
                        }
                        else
                        {
                            rj["state"] = "missing";
                            Json cands = Json::array();
                            if (const auto nIt = byName.find(BaseNameLower(r.path)); nIt != byName.end())
                                for (std::size_t i = 0; i < nIt->second.size() && i < 5; ++i)
                                    cands.push_back(nIt->second[i]);
                            if (!cands.empty()) rj["candidates"] = cands;
                            broken.push_back(Json{ { "from", p }, { "key", r.key }, { "path", r.path },
                                                   { "line", r.line }, { "candidates", cands } });
                        }
                    }
                    ra.push_back(std::move(rj));
                }
                e["refs"] = std::move(ra);
            }
            assets.push_back(std::move(e));
        }

        Json dups = Json::array();
        for (const auto& [g, paths] : byGuid)
            if (paths.size() > 1) dups.push_back(Json{ { "guid", g }, { "paths", paths } });

        Json countsJson = Json::object();
        for (const auto& [t, n] : counts) countsJson[t] = n;
        Json versionsJson = Json::object();
        for (const auto& [format, byVersion] : versionCounts)
            for (const auto& [version, n] : byVersion) versionsJson[format][std::to_string(version)] = n;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Result r;
        r.j = Json{ { "ok", true }, { "roots", roots }, { "assets", assets }, { "counts", countsJson },
                    { "parseStats", Json{ { "parsed", parsed }, { "reused", reused }, { "total", files.size() } } },
                    { "references", refTotal }, { "unstamped", unstamped }, { "versions", versionsJson },
                    { "problems", Json{ { "missingReferences", broken }, { "guidRepairs", repairs },
                                        { "duplicateGuids", dups }, { "orphanMetas", orphanMetas },
                                        { "unsupportedVersions", unsupportedVersions } } },
                    { "ms", std::round(ms) } };
        if (incremental)
            r.j["parseCache"] = Json{ { "version", 1 }, { "producer", producer }, { "entries", std::move(parsedEntries) } };
        LOG_INFO("[ASSETTOOL] catalog: %zu asset(s), %zu reference(s), %zu missing, %zu guid repair(s), "
                 "%zu duplicate guid(s), %zu orphan .meta, %zu unstamped, %zu unsupported version(s) (%.0f ms)",
                 files.size(), refTotal, broken.size(), repairs.size(), dups.size(), orphanMetas.size(),
                 unstamped, unsupportedVersions.size(), ms);
        return r;
    }

    // ==== compat: does an animation fit a skeleton ====
    // Playback binds a clip's channels to a skeleton's bones by exact name
    // (Resource::AnimClipData::ChannelBinding: FNV-32, case-sensitive) and never says when nothing
    // matched - the clip binds and plays the rest pose. This asks the same question offline, with the
    // same loaders and the same rule:
    //   -skeleton=<.iskel | .iscn (its K line) | .ipfb (its SceneRef's .iscn)>
    //   -anim=<.ianim>   every bone clip in it; omitted with an .ipfb: each of the prefab's
    //                    AnimationSet entries, its clip picked by the entry's selector as playback
    //                    picks it, and every bone clip of any other .ianim it names (usedBy: the
    //                    component and key that name it)
    // A verdict per clip: fits (some bone is driven), rest_pose (none is), clip_not_found /
    // no_bone_clip / load_failed (playback's own failures).
    std::string NameOf(const char (&name)[64]) { return std::string(name, strnlen(name, 64)); }

    bool ContainerTextOf(const std::string& path, std::string& text)
    {
        return ContainerText(ReadAssetBytes(path), text);
    }

    // The .iskel a skeleton source names, and what was looked at on the way.
    bool ResolveSkeleton(const std::string& source, std::string& iskel, std::string& prefabText, Json& chain,
                         std::string& err)
    {
        const std::string ext = LowerAscii(Sys::PathToUtf8(Sys::U8Path(source).extension()));
        std::string iscn;
        if (ext == ".iskel") { iskel = source; return true; }
        if (ext == ".iscn") iscn = source;
        else if (ext == ".ipfb")
        {
            if (!ContainerTextOf(source, prefabText)) { err = "cannot read the prefab " + source; return false; }
            for (const auto& ref : Resource::AssetRefs::ListSceneTextRefs(prefabText))
                if (ref.tag == "SceneRef" && LowerAscii(ref.path).ends_with(".iscn")) { iscn = ref.path; break; }
            if (iscn.empty()) { err = "the prefab names no skinned model (no SceneRef .iscn): " + source; return false; }
        }
        else { err = "-skeleton is an .iskel, an .iscn or an .ipfb: " + source; return false; }
        chain.push_back(iscn);
        std::string text;
        if (!ContainerTextOf(iscn, text)) { err = "cannot read " + iscn; return false; }
        for (const auto& fr : SceneInstanceLoader::ListFileReferences(text, iscn))
            if (fr.kind == 'K') { iskel = fr.path; return true; }
        err = iscn + " has no skeleton (no K line)";
        return false;
    }

    // One clip against the skeleton, by playback's own binding (Resource::FitClip -
    // the Skeleton Editor's animation list asks the same function).
    Json ClipFit(const Resource::AnimClipData& clip, const SkeletonAsset& skel)
    {
        const Resource::ClipFit f = Resource::FitClip(clip, skel);
        Json unmatched = Json::array(), duplicates = Json::array();
        for (const std::string& n : f.unmatched) unmatched.push_back(n);
        for (const std::string& n : f.duplicates) duplicates.push_back(n);
        return Json{ { "clip", f.clip }, { "channels", clip.channels.size() }, { "boundBones", f.boundBones },
                     { "bones", f.bones },
                     { "coverage", f.bones ? static_cast<double>(f.boundBones) / f.bones : 0.0 },
                     { "unmatchedTracks", unmatched }, { "unmatchedCount", f.unmatchedCount },
                     { "duplicateTracks", duplicates }, { "frames", f.frames }, { "duration", f.duration },
                     { "verdict", f.Fits() ? "fits" : "rest_pose" } };
    }

    bool LoadAnimation(const std::string& path, std::unique_ptr<Resource::Resource>& holder,
                       const Resource::AnimationResource*& anim)
    {
        const std::string blob = ReadAssetBytes(path);   // what playback's loader would read
        if (blob.empty()) return false;
        const std::vector<std::uint8_t> bytes(blob.begin(), blob.end());
        Resource::AnimationLoader loader;
        holder = loader.Load(path, bytes).resource;
        anim = dynamic_cast<const Resource::AnimationResource*>(holder.get());
        return anim != nullptr;
    }

    Result Compat(const std::string& skeletonSource, const std::string& animPath)
    {
        if (skeletonSource.empty()) return Fail(kUsage, "usage", "-skeleton=<.iskel|.iscn|.ipfb> is required");
        std::string iskel, prefabText, err;
        Json chain = Json::array();
        if (!ResolveSkeleton(skeletonSource, iskel, prefabText, chain, err)) return Fail(kUsage, "load_failed", err);
        auto skel = std::make_unique<SkeletonAsset>();   // MAX_BONES-sized: never on the stack
        std::vector<ClipAsset> embedded;
        if (!SceneInstanceLoader::LoadSkeletonFile(iskel, *skel, embedded))
            return Fail(kUsage, "load_failed", "the skeleton did not load: " + iskel);

        Json clips = Json::array();
        auto one = [&](const std::string& anim, const std::string& selector, const std::string* alias,
                       const std::string& usedBy = {})
        {
            std::unique_ptr<Resource::Resource> holder;
            const Resource::AnimationResource* res = nullptr;
            Json row;
            if (!LoadAnimation(anim, holder, res))
                row = Json{ { "verdict", "load_failed" } };
            else if (res->clips.empty())
                row = Json{ { "verdict", "no_bone_clip" }, { "morphClips", res->morphClips.size() } };
            else if (alias)
            {
                const int32_t i = Resource::FindSourceClip(res->clips, selector);
                row = i < 0 ? Json{ { "verdict", "clip_not_found" }, { "selector", selector } }
                            : ClipFit(res->clips[static_cast<std::size_t>(i)], *skel);
            }
            if (!alias && res && !res->clips.empty())
            {
                for (const auto& c : res->clips)
                {
                    Json r = ClipFit(c, *skel);
                    r["anim"] = anim;
                    if (!usedBy.empty()) r["usedBy"] = usedBy;
                    clips.push_back(std::move(r));
                }
                return;
            }
            row["anim"] = anim;
            if (alias) row["alias"] = *alias;
            if (!usedBy.empty()) row["usedBy"] = usedBy;
            clips.push_back(std::move(row));
        };

        Json animationSet;   // the prefab's first AnimationSet: {default, autoplay}
        if (!animPath.empty())
            one(animPath, {}, nullptr);
        else
        {
            if (prefabText.empty())
                return Fail(kUsage, "usage", "-anim=<.ianim> is required unless -skeleton is a prefab with an "
                                             "AnimationSet");
            // The prefab's AnimationSet lines: name<i> / path<i> / bone<i>, the
            // values percent-encoded, as ComponentSerializers writes them.
            std::istringstream lines(prefabText);
            std::string line;
            while (std::getline(lines, line))
            {
                const auto colon = line.find(':');
                if (colon == std::string::npos || line.compare(0, colon, "  AnimationSet") != 0) continue;
                std::map<std::string, std::string> kv;
                std::istringstream toks(line.substr(colon + 1));
                std::string tok;
                while (toks >> tok)
                    if (const auto eq = tok.find('='); eq != std::string::npos)
                        kv[tok.substr(0, eq)] = Resource::AssetRefs::PercentDecode(tok.substr(eq + 1));
                if (animationSet.is_null())
                    animationSet = Json{ { "default", kv["default"] }, { "autoplay", kv["autoplay"] == "1" } };
                const int n = std::atoi(kv["n"].c_str());
                for (int i = 0; i < n; ++i)
                {
                    const std::string s = std::to_string(i), path = kv["path" + s], alias = kv["name" + s];
                    if (path.empty()) continue;
                    one(path, kv["bone" + s], &alias, "AnimationSet." + alias);
                }
            }
            // Every other .ianim the prefab names (locomotion blend spaces, ...):
            // which of its clips they play is theirs to say, so each bone clip.
            std::set<std::string> seenPaths;
            for (const auto& ref : Resource::AssetRefs::ListSceneTextRefs(prefabText))
            {
                if (ref.tag == "AnimationSet" || !LowerAscii(ref.path).ends_with(".ianim")) continue;
                if (!seenPaths.insert(LowerAscii(ref.path)).second) continue;
                one(ref.path, {}, nullptr, ref.tag + "." + ref.key);
            }
            if (clips.empty())
                return Fail(kUsage, "usage", "the prefab names no animation: give -anim=<.ianim>");
        }

        std::size_t bad = 0;
        for (const auto& c : clips) bad += c["verdict"] != "fits";
        Result r;
        r.j = Json{ { "ok", true },
                    { "skeleton", { { "source", skeletonSource }, { "via", chain }, { "iskel", iskel },
                                    { "bones", skel->boneCount } } },
                    { "clips", std::move(clips) }, { "compatible", bad == 0 }, { "notFitting", bad } };
        if (!animationSet.is_null()) r.j["animationSet"] = std::move(animationSet);
        r.code = bad == 0 ? kDone : kRefused;
        LOG_INFO("[ASSETTOOL] compat: %s (%u bones) - %zu clip(s), %zu not fitting", iskel.c_str(),
                 skel->boneCount, r.j["clips"].size(), bad);
        return r;
    }

    // ==== skeleton_report: what a skeleton is ====
    //   -skeleton=<.iskel | .iscn (its K line) | .ipfb (its SceneRef's .iscn) | a directory>
    //   -bones=1   one row per bone as well
    // One skeleton: its statistics, its humanoid profile, its issues grouped by kind (count, the first
    // 32 bones, their details). A directory: every .iskel under it (sorted, junctions skipped), one
    // summary each - what the content ratchet compares. Resource::InspectSkeleton is the Skeleton
    // Editor's own report, so this is the oracle for the UI. exitCode 1 on an error-severity issue or
    // a file that does not load.
    struct LoadedSkeleton
    {
        std::unique_ptr<SkeletonAsset>                    skel;   // MAX_BONES-sized: never on the stack
        std::vector<SceneInstanceLoader::SkinBlendStream> blends;
        std::string                                       humanoidSource;   // file | built | none
    };

    bool LoadForReport(const std::string& iskel, LoadedSkeleton& out)
    {
        out.skel = std::make_unique<SkeletonAsset>();
        std::vector<ClipAsset> clips;
        if (!SceneInstanceLoader::LoadSkeletonFile(iskel, *out.skel, clips, &out.blends)) return false;
        // What the runtime does the first time something needs roles.
        out.humanoidSource = Resource::EnsureHumanoidProfile(*out.skel);
        return true;
    }

    std::vector<Resource::SkinWeightStream> WeightStreamsOf(const LoadedSkeleton& l)
    {
        std::vector<Resource::SkinWeightStream> w;
        w.reserve(l.blends.size());
        for (const auto& b : l.blends) w.push_back({ b.weights.data(), b.weights.size() });
        return w;
    }

    std::string SkelBoneName(const SkeletonAsset& s, int32_t b)
    {
        if (b < 0 || b >= static_cast<int32_t>(s.boneCount)) return {};
        return std::string(s.boneNames[b], strnlen(s.boneNames[b], sizeof(s.boneNames[b])));
    }

    Json SkeletonStatsJson(const Resource::SkeletonStats& s)
    {
        return Json{ { "bones", s.bones }, { "roots", s.roots }, { "leaves", s.leaves }, { "maxDepth", s.maxDepth },
                     { "ikChains", s.ikChains }, { "ikLinks", s.ikLinks }, { "grantTargets", s.grantTargets },
                     { "twist", s.twist }, { "weightsKnown", s.weightsKnown }, { "skinStreams", s.skinStreams },
                     { "skinVertices", s.skinVertices }, { "maxInfluences", s.maxInfluences },
                     { "weighted", s.weighted }, { "unweighted", s.unweighted }, { "dummyLeaves", s.dummyLeaves },
                     { "chainName", s.chainName }, { "chainAutoAttach", s.chainAutoAttach },
                     { "humanoidMapped", s.humanoidMapped }, { "humanoidValid", s.humanoidValid },
                     { "modelSize", s.modelSize } };
    }

    Json SkeletonHumanoidJson(const SkeletonAsset& s, const Resource::SkeletonReport& rep,
                              const std::string& source)
    {
        return Json{ { "source", source }, { "convention", s.humanoid.convention },
                     { "valid", s.humanoid.IsValid() }, { "mapped", rep.stats.humanoidMapped },
                     { "userEdited", s.humanoid.userEdited != 0 } };
    }

    // {kind: count}; @p gatedOnly keeps the warning / error kinds - what the content
    // ratchet holds the tree to (infos, e.g. bind_rest_mismatch, move with re-imports).
    Json SkeletonIssueCountsJson(const Resource::SkeletonReport& rep, bool gatedOnly)
    {
        Json counts = Json::object();
        for (const auto& i : rep.issues)
        {
            if (gatedOnly && i.severity == Resource::SkeletonIssueSeverity::Info) continue;
            const char* k = Resource::SkeletonIssueKindName(i.kind);
            counts[k] = counts.value(k, 0) + 1;
        }
        return counts;
    }

    Json SkeletonIssuesJson(const SkeletonAsset& s, const Resource::SkeletonReport& rep)
    {
        using Resource::SkeletonIssueKind;
        Json groups = Json::array();
        for (uint32_t k = 0; k < static_cast<uint32_t>(SkeletonIssueKind::Count); ++k)
        {
            const auto kind = static_cast<SkeletonIssueKind>(k);
            Json bones = Json::array(), details = Json::array();
            std::size_t count = 0;
            for (const auto& i : rep.issues)
            {
                if (i.kind != kind) continue;
                if (count++ >= 32) continue;
                bones.push_back(SkelBoneName(s, i.bone));
                details.push_back(i.detail);
            }
            if (!count) continue;
            groups.push_back(Json{ { "kind", Resource::SkeletonIssueKindName(kind) },
                                   { "severity", Resource::SkeletonIssueSeverityName(
                                                     Resource::SkeletonIssueDefaultSeverity(kind)) },
                                   { "count", count }, { "bones", std::move(bones) },
                                   { "details", std::move(details) } });
        }
        return groups;
    }

    Json SkeletonBonesJson(const SkeletonAsset& s, const Resource::SkeletonReport& rep)
    {
        Json rows = Json::array();
        for (uint32_t b = 0; b < rep.bones.size(); ++b)
        {
            const auto& bi = rep.bones[b];
            Json flags = Json::array();
            for (const char* f : Resource::SkeletonBoneFlagNames(bi.flags)) flags.push_back(f);
            Json row{ { "i", b }, { "name", SkelBoneName(s, static_cast<int32_t>(b)) }, { "parent", bi.parent },
                      { "depth", bi.depth }, { "children", bi.childCount }, { "flags", std::move(flags) },
                      { "weightedVerts", bi.weightedVerts }, { "maxWeight", bi.maxWeight },
                      { "length", bi.length } };
            if (bi.humanoidRole >= 0)
                row["role"] = HumanoidRoleName(static_cast<HumanoidBone>(bi.humanoidRole));
            if (bi.grantSource >= 0)
                row["grant"] = Json{ { "source", SkelBoneName(s, bi.grantSource) }, { "ratio", bi.grantRatio } };
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // The model's socket records (its SO lines) against its skeleton - each one's bone found or not.
    // @p problems: the skeleton sockets whose bone is missing and that are not marked orphan (a spawn
    // warns of each).
    Json ModelSocketsJson(const std::string& model, const SkeletonAsset& s, std::size_t& problems)
    {
        problems = 0;
        std::string text;
        Json rows = Json::array(), warnings = Json::array();
        if (!ContainerTextOf(model, text))
            return Json{ { "model", model }, { "readable", false } };
        Resource::ModelSockets ms;
        std::vector<std::string> warns;
        Resource::ParseModelSockets(text, ms, &warns);
        for (const std::string& w : warns) warnings.push_back(w);
        for (const Resource::ModelSocket& so : ms.sockets)
        {
            Json row{ { "name", so.name }, { "t", Vec(&so.t.x, 3) }, { "q", Vec(&so.q.x, 4) }, { "s", Vec(&so.s.x, 3) },
                      { "source", so.source }, { "orphan", so.orphan } };
            if (so.IsSkeletonSocket())
            {
                int bone = -1;
                for (uint32_t b = 0; b < s.boneCount && bone < 0; ++b)
                    if (SkelBoneName(s, static_cast<int32_t>(b)) == so.bone) bone = static_cast<int>(b);
                row["bone"]      = so.bone;
                row["boneIndex"] = bone;
                if (bone < 0 && !so.orphan) ++problems;
            }
            else
                row["mesh"] = Resource::MeshKeyToHex(so.meshKey);
            rows.push_back(std::move(row));
        }
        return Json{ { "model", model }, { "readable", true }, { "records", std::move(rows) },
                     { "warnings", std::move(warnings) }, { "missingBone", problems } };
    }

    // The model's virtual bones (its VB records) - each one's bones found or not, and what
    // ApplyVirtualBones says of the set (the loader's own function, on a copy of the skeleton).
    Json ModelVirtualBonesJson(const std::string& model, const SkeletonAsset& s)
    {
        std::string text;
        if (!ContainerTextOf(model, text)) return Json{ { "model", model }, { "readable", false } };
        Resource::ModelVirtualBones vbs;
        std::vector<std::string> warns;
        Resource::ParseModelVirtualBones(text, vbs, &warns);
        Json rows = Json::array(), warnings = Json::array();
        for (const std::string& w : warns) warnings.push_back(w);
        for (const Resource::ModelVirtualBone& v : vbs.bones)
            rows.push_back(Json{ { "name", v.name }, { "from", v.from }, { "to", v.to }, { "orphan", v.orphan } });
        uint32_t applied = 0;
        if (!vbs.bones.empty())
        {
            auto copy = std::make_unique<SkeletonAsset>(s);   // ~656 KB: never on the stack
            std::vector<std::string> why;
            applied = Resource::ApplyVirtualBones(*copy, vbs.bones, &why);
            for (const std::string& w : why) warnings.push_back(w);
        }
        return Json{ { "model", model }, { "readable", true }, { "records", std::move(rows) },
                     { "applied", applied }, { "warnings", std::move(warnings) } };
    }

    Result SkeletonReportCmd(const std::string& source, bool withBones)
    {
        namespace fs = std::filesystem;
        using Resource::SkeletonIssueSeverity;
        if (source.empty())
            return Fail(kUsage, "usage", "-skeleton=<.iskel|.iscn|.ipfb|directory> is required");

        std::error_code ec;
        const fs::path root = Sys::U8Path(source);
        const std::vector<std::string> shared = SharedOnlyFiles(source, ".iskel");   // read-only: a report
        if (fs::is_directory(root, ec) || !shared.empty())
        {
            std::vector<fs::path> files;
            for (const std::string& s : shared) files.push_back(Sys::U8Path(s));
            Json skippedReparse = Json::array();
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_directory(ec))
                {
                    const DWORD a = GetFileAttributesW(it->path().c_str());
                    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
                    {
                        skippedReparse.push_back(Sys::PathToUtf8(it->path()));
                        it.disable_recursion_pending();   // another checkout's files: not ours to judge
                    }
                    continue;
                }
                if (it->is_regular_file(ec) && LowerAscii(Sys::PathToUtf8(it->path().extension())) == ".iskel")
                    files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());

            Json rows = Json::array();
            std::size_t withErrors = 0;
            for (const fs::path& p : files)
            {
                std::string ps = Sys::PathToUtf8(p);
                std::replace(ps.begin(), ps.end(), '\\', '/');
                LoadedSkeleton l;
                if (!LoadForReport(ps, l))
                {
                    rows.push_back(Json{ { "iskel", ps }, { "loaded", false } });
                    ++withErrors;
                    continue;
                }
                const Resource::SkeletonReport rep = Resource::InspectSkeleton(*l.skel, WeightStreamsOf(l));
                const uint32_t errors = rep.Count(SkeletonIssueSeverity::Error);
                withErrors += errors ? 1 : 0;
                rows.push_back(Json{ { "iskel", ps }, { "loaded", true }, { "bones", rep.stats.bones },
                                     { "errors", errors }, { "warnings", rep.Count(SkeletonIssueSeverity::Warning) },
                                     { "infos", rep.Count(SkeletonIssueSeverity::Info) },
                                     { "issueCounts", SkeletonIssueCountsJson(rep, false) },
                                     { "gated", SkeletonIssueCountsJson(rep, true) },
                                     { "humanoid", SkeletonHumanoidJson(*l.skel, rep, l.humanoidSource) } });
            }
            Result r;
            r.j = Json{ { "ok", true }, { "root", source }, { "files", std::move(rows) }, { "count", files.size() },
                        { "withErrors", withErrors }, { "skippedReparse", std::move(skippedReparse) } };
            r.code = withErrors ? kRefused : kDone;
            LOG_INFO("[ASSETTOOL] skeleton_report: %zu skeleton(s) under %s, %zu with errors", files.size(),
                     source.c_str(), withErrors);
            return r;
        }

        std::string iskel, prefabText, err;
        Json chain = Json::array();
        if (!ResolveSkeleton(source, iskel, prefabText, chain, err)) return Fail(kUsage, "load_failed", err);
        LoadedSkeleton l;
        if (!LoadForReport(iskel, l)) return Fail(kUsage, "load_failed", "the skeleton did not load: " + iskel);
        const Resource::SkeletonReport rep = Resource::InspectSkeleton(*l.skel, WeightStreamsOf(l));
        const uint32_t errors = rep.Count(SkeletonIssueSeverity::Error);

        Result r;
        r.j = Json{ { "ok", true },
                    { "skeleton", { { "source", source }, { "via", chain }, { "iskel", iskel },
                                    { "bones", l.skel->boneCount } } },
                    { "stats", SkeletonStatsJson(rep.stats) },
                    { "humanoid", SkeletonHumanoidJson(*l.skel, rep, l.humanoidSource) },
                    { "issues", SkeletonIssuesJson(*l.skel, rep) },
                    { "errors", errors }, { "warnings", rep.Count(SkeletonIssueSeverity::Warning) },
                    { "infos", rep.Count(SkeletonIssueSeverity::Info) } };
        if (withBones) r.j["bones"] = SkeletonBonesJson(*l.skel, rep);
        // The model's sockets, when the skeleton was reached through a model.
        if (!chain.empty() && chain[0].is_string())
        {
            std::size_t socketProblems = 0;
            r.j["sockets"] = ModelSocketsJson(chain[0].get<std::string>(), *l.skel, socketProblems);
            r.j["virtualBones"] = ModelVirtualBonesJson(chain[0].get<std::string>(), *l.skel);
        }
        r.code = errors ? kRefused : kDone;
        LOG_INFO("[ASSETTOOL] skeleton_report: %s (%u bones) - %u error(s), %u warning(s), %u info", iskel.c_str(),
                 l.skel->boneCount, errors, rep.Count(SkeletonIssueSeverity::Warning),
                 rep.Count(SkeletonIssueSeverity::Info));
        return r;
    }
}

namespace
{
    // ==== meshlib_identity: the .meshlib identity layer ====
    Json IdentityJson(const Resource::MeshIdentity& id, bool detail)
    {
        Json j{ { "modelKey", Resource::MeshKeyToHex(id.modelKey) }, { "flags", id.flags },
                { "meshCount", id.meshes.size() }, { "sectionCount", id.sections.size() } };
        if (!detail) return j;
        Json meshes = Json::array();
        for (const auto& m : id.meshes)
        {
            Json secs = Json::array();
            for (uint32_t e : m.sections) secs.push_back(e);
            meshes.push_back(Json{ { "key", Resource::MeshKeyToHex(m.key) }, { "name", m.name },
                                   { "sourceKey", m.sourceKey }, { "sections", std::move(secs) },
                                   { "flags", m.flags }, { "aabbMin", Vec(m.aabbMin, 3) },
                                   { "aabbMax", Vec(m.aabbMax, 3) } });
        }
        Json sections = Json::array();
        for (std::size_t e = 0; e < id.sections.size(); ++e)
        {
            const auto& s = id.sections[e];
            sections.push_back(Json{ { "entry", e }, { "key", Resource::MeshKeyToHex(s.key) },
                                     { "slot", s.slotName }, { "sourceKey", s.sourceKey },
                                     { "ownerMesh", s.ownerMesh == UINT32_MAX ? -1 : static_cast<int>(s.ownerMesh) },
                                     { "flags", s.flags } });
        }
        j["meshes"]   = std::move(meshes);
        j["sections"] = std::move(sections);
        return j;
    }

    // The first directory at or above @p p, up to (not including) @p stop, that
    // is a reparse point - a junction or a symlink. Writing through one would
    // modify ANOTHER checkout's files (this repo's worktrees reach the main
    // checkout's asset\Bistro and the SDKs through junctions).
    std::string ReparseAncestor(std::filesystem::path p, const std::filesystem::path& stop)
    {
        for (; !p.empty(); p = p.parent_path())
        {
            if (!stop.empty() && p == stop) break;
            const DWORD a = GetFileAttributesW(p.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
                return Sys::PathToUtf8(p);
            if (p == p.root_path()) break;
        }
        return {};
    }

    Result MeshLibIdentityCmd(const std::string& path, bool apply, bool force, const std::string& iscn)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = Sys::U8Path(path);
        const fs::path cwd  = fs::current_path(ec);
        std::vector<fs::path> files;
        Json skippedReparse = Json::array();
        bool single = false;
        if (path.empty()) return Fail(kUsage, "usage", "-asset=<.meshlib or directory> is required");
        const std::vector<std::string> shared = SharedOnlyFiles(path, ".meshlib");
        if (fs::is_directory(root, ec) || !shared.empty())
        {
            for (const std::string& s : shared) files.push_back(Sys::U8Path(s));
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_directory(ec))
                {
                    const DWORD a = GetFileAttributesW(it->path().c_str());
                    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
                    {
                        skippedReparse.push_back(Sys::PathToUtf8(it->path()));
                        it.disable_recursion_pending();   // another checkout's files: not ours to scan
                    }
                    continue;
                }
                if (it->is_regular_file(ec) && LowerAscii(Sys::PathToUtf8(it->path().extension())) == ".meshlib")
                    files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
        }
        else if (fs::exists(root, ec) || Resource::AssetFS::Get().ExistsExact(path)) { files.push_back(root); single = true; }
        else return Fail(kUsage, "usage", "no .meshlib or directory at '" + path + "'");
        if (!iscn.empty() && !single)
            return Fail(kUsage, "usage", "-iscn= names ONE library's model; give -asset=<one .meshlib>");

        Json list = Json::array();
        std::size_t nIdentity = 0, nNone = 0, nMalformed = 0, nWritten = 0, nRefused = 0, nOther = 0;
        for (const fs::path& p : files)
        {
            const std::string ps = Sys::PathToUtf8(p);
            Json row{ { "path", ps } };
            std::vector<uint8_t> bytes;
            if (!Resource::AssetFS::Get().ReadFile(ps, bytes) ||   // a shared worktree: the main checkout's too
                !Resource::ValidateHeader(bytes.data(), bytes.size(), Resource::MAGIC_MESHLIB) ||
                Resource::MeshLibCoreEnd(bytes.data(), bytes.size()) == 0)
            {
                row["state"] = "unreadable";
                ++nOther;
                list.push_back(std::move(row));
                continue;
            }
            Resource::MeshLibraryMetadata meta{};
            std::memcpy(&meta, bytes.data() + sizeof(Resource::AssetHeader), sizeof(meta));
            row["entries"] = meta.meshCount;
            row["bytes"]   = bytes.size();

            Resource::MeshIdentity  have;
            std::string             why;
            const Resource::MeshIdentityParse st = Resource::ParseMeshLibIdentity(bytes.data(), bytes.size(), have, &why);
            if (st == Resource::MeshIdentityParse::Ok)
            {
                row["state"]    = "identity";
                row["identity"] = IdentityJson(have, single);
                ++nIdentity;
                list.push_back(std::move(row));
                continue;
            }
            const bool malformed = (st == Resource::MeshIdentityParse::Malformed);
            row["state"] = malformed ? "malformed" : "none";
            if (malformed) { row["error"] = why; ++nMalformed; } else ++nNone;

            // The file as a pre-identity writer made it (a damaged tail dropped).
            std::vector<uint8_t> base = bytes;
            if (malformed && !Resource::StripMeshLibTail(base, &why))
            { row["refused"] = "cannot strip the damaged tail: " + why; ++nRefused; list.push_back(std::move(row)); continue; }
            Resource::MeshIdentity synth =
                SceneInstanceLoader::SynthesizeMeshLibIdentity(ps, base.data(), base.size(), iscn);
            row["wouldWrite"] = IdentityJson(synth, single);

            if (apply)
            {
                const std::string junction = ReparseAncestor(fs::absolute(p, ec), cwd);
                if (malformed && !force)
                    row["refused"] = "the tail is damaged; -force replaces it (keys it held are lost)";
                else if (!junction.empty())
                    row["refused"] = "reached through the reparse point '" + junction + "' (another checkout's file)";
                else if (OnlySharedHas(ps))
                    row["refused"] = "only the shared content root has it (the main checkout's file) - make a local "
                                     "copy first: python tools/worktree_content.py materialize " + ps;
                else
                {
                    synth.flags = (synth.flags & ~Resource::IDNT_FLAG_SYNTHESIZED) | Resource::IDNT_FLAG_BACKFILLED;
                    std::vector<uint8_t> out = base;
                    std::vector<uint8_t> back;
                    Resource::MeshIdentity readBack;
                    const size_t coreEnd = Resource::MeshLibCoreEnd(base.data(), base.size());
                    if (!Resource::WriteMeshLibIdentity(out, synth, &why))
                        row["refused"] = "identity does not fit the file: " + why;
                    else if (!Sys::WriteFileBytesAtomic(ps, out.data(), out.size()))
                        row["refused"] = "write failed";
                    else if (!Sys::ReadFileBytes(ps, back) ||
                             Resource::ParseMeshLibIdentity(back.data(), back.size(), readBack, &why) != Resource::MeshIdentityParse::Ok)
                        row["refused"] = "verify: the written identity does not parse back (" + why + ")";
                    else
                    {
                        // Keys equal to the synthesized ones, geometry byte-identical.
                        bool same = readBack.sections.size() == synth.sections.size() &&
                                    readBack.meshes.size() == synth.meshes.size();
                        for (std::size_t e = 0; same && e < synth.sections.size(); ++e)
                            same = readBack.sections[e].key == synth.sections[e].key;
                        for (std::size_t m = 0; same && m < synth.meshes.size(); ++m)
                            same = readBack.meshes[m].key == synth.meshes[m].key;
                        const size_t payload = sizeof(Resource::AssetHeader) + Resource::GetHeader(base.data())->metadataSize;
                        same = same && back.size() >= coreEnd &&
                               std::memcmp(back.data() + payload, base.data() + payload, coreEnd - payload) == 0;
                        if (!same) row["refused"] = "verify: keys or geometry differ after the write";
                        else       row["written"] = true;
                    }
                }
                if (row.contains("refused")) ++nRefused; else ++nWritten;
            }
            list.push_back(std::move(row));
        }

        Result r;
        r.j = Json{ { "ok", nRefused == 0 }, { "apply", apply }, { "files", std::move(list) },
                    { "summary", { { "count", files.size() }, { "identity", nIdentity }, { "none", nNone },
                                   { "malformed", nMalformed }, { "unreadable", nOther },
                                   { "written", nWritten }, { "refused", nRefused } } } };
        if (!skippedReparse.empty()) r.j["skippedReparse"] = std::move(skippedReparse);
        r.code = nRefused == 0 ? kDone : kRefused;
        LOG_INFO("[ASSETTOOL] meshlib_identity: %zu file(s) - %zu with identity, %zu without, %zu malformed, "
                 "%zu written, %zu refused", files.size(), nIdentity, nNone, nMalformed, nWritten, nRefused);
        return r;
    }

    // ==== meshlets: the MLET chunk ====
    // Every .meshlib under -asset (or the one named): does it carry meshlets, are they valid for its
    // geometry, how many clusters / levels / bytes. -apply builds them where missing or stale
    // (-rebuild: everywhere) for the library's static sections (its identity, or the one the loader
    // synthesizes from the model's .iscn - a model naming a skeleton is then skinned throughout),
    // verified on read-back with every byte before the tail and every other chunk untouched.
    // Never: a collision library, a file reached through a junction, a file only the shared content
    // root has.
    Json MeshletChunkJson(const Resource::MeshletChunkView& v, bool perEntry)
    {
        uint32_t built = 0, maxLevels = 0;
        uint64_t lvl0Tris = 0, allTris = 0;
        std::vector<uint64_t> trisPerLevel;
        Json entries = Json::array();
        for (uint32_t e = 0; e < v.header.entryCount; ++e)
        {
            const Resource::MeshletEntryRange r = v.Entry(e);
            if (r.levelCount == 0) continue;
            ++built;
            maxLevels = std::max(maxLevels, r.levelCount);
            Json levels = Json::array();
            for (uint32_t l = 0; l < r.levelCount; ++l)
            {
                const Resource::MeshletLevel lv = v.Level(r.firstLevel + l);
                uint64_t tris = 0;
                for (uint32_t k = lv.firstCluster; k < lv.firstCluster + lv.clusterCount; ++k)
                    tris += Resource::MeshletTriangleCount(v.Cluster(k));
                if (trisPerLevel.size() <= l) trisPerLevel.resize(l + 1, 0);
                trisPerLevel[l] += tris;
                allTris += tris;
                if (l == 0) lvl0Tris += tris;
                if (perEntry)
                    levels.push_back(Json{ { "clusters", lv.clusterCount }, { "triangles", tris },
                                           { "selfError", lv.minSelfError },
                                           { "parentError", lv.maxParentError >= Resource::kMeshletRootError
                                                                ? Json(nullptr) : Json(lv.maxParentError) } });
            }
            if (perEntry)
                entries.push_back(Json{ { "entry", e }, { "clusters", r.clusterCount }, { "levels", std::move(levels) } });
        }
        Json j{ { "clusters", v.header.clusterCount }, { "levelRecords", v.header.levelCount },
                { "entriesWithMeshlets", built }, { "maxLevels", maxLevels }, { "level0Triangles", lvl0Tris },
                { "triangles", allTris }, { "trianglesPerLevel", trisPerLevel }, { "chunkBytes", v.chunkSize },
                { "gpuBytes", v.header.gpuByteSize } };
        if (perEntry) j["entries"] = std::move(entries);
        return j;
    }

    Result MeshletsCmd(const std::string& path, bool apply, bool rebuild, bool lod, const std::string& iscn)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = Sys::U8Path(path);
        const fs::path cwd  = fs::current_path(ec);
        std::vector<fs::path> files;
        Json skippedReparse = Json::array();
        bool single = false;
        if (path.empty()) return Fail(kUsage, "usage", "-asset=<.meshlib or directory> is required");
        const std::vector<std::string> shared = SharedOnlyFiles(path, ".meshlib");
        if (fs::is_directory(root, ec) || !shared.empty())
        {
            for (const std::string& s : shared) files.push_back(Sys::U8Path(s));
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_directory(ec))
                {
                    const DWORD a = GetFileAttributesW(it->path().c_str());
                    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
                    {
                        skippedReparse.push_back(Sys::PathToUtf8(it->path()));
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (it->is_regular_file(ec) && LowerAscii(Sys::PathToUtf8(it->path().extension())) == ".meshlib")
                    files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
        }
        else if (fs::exists(root, ec) || Resource::AssetFS::Get().ExistsExact(path)) { files.push_back(root); single = true; }
        else return Fail(kUsage, "usage", "no .meshlib or directory at '" + path + "'");
        if (!iscn.empty() && !single)
            return Fail(kUsage, "usage", "-iscn= names ONE library's model; give -asset=<one .meshlib>");

        Json list = Json::array();
        std::map<std::string, std::size_t> counts{ { "meshlets", 0 }, { "none", 0 }, { "stale", 0 },
                                                   { "skinned", 0 }, { "collision", 0 }, { "unreadable", 0 } };
        std::size_t nWritten = 0, nRefused = 0;
        uint64_t    bytesBefore = 0, bytesAfter = 0;
        for (const fs::path& p : files)
        {
            const std::string ps = Sys::PathToUtf8(p);
            Json row{ { "path", ps } };
            std::vector<uint8_t> bytes;
            if (!Resource::AssetFS::Get().ReadFile(ps, bytes) ||
                !Resource::ValidateHeader(bytes.data(), bytes.size(), Resource::MAGIC_MESHLIB) ||
                Resource::MeshLibCoreEnd(bytes.data(), bytes.size()) == 0)
            {
                row["state"] = "unreadable";
                ++counts["unreadable"];
                list.push_back(std::move(row));
                continue;
            }
            Resource::MeshLibraryMetadata meta{};
            std::memcpy(&meta, bytes.data() + sizeof(Resource::AssetHeader), sizeof(meta));
            row["entries"] = meta.meshCount;
            row["bytes"]   = bytes.size();

            // Collision libraries (<stem>_collision.meshlib, baked_collision.meshlib) never draw.
            const std::string lowerPath = LowerAscii(ps);
            static constexpr std::string_view kCollision = "_collision.meshlib";
            if (lowerPath.size() >= kCollision.size() &&
                lowerPath.compare(lowerPath.size() - kCollision.size(), kCollision.size(), kCollision) == 0)
            {
                row["state"] = "collision";
                ++counts["collision"];
                list.push_back(std::move(row));
                continue;
            }

            // The static sections: the file's identity, or the loader's synthesis.
            fs::path model = p;
            model.replace_extension(".iscn");
            const std::string modelPath = iscn.empty() ? Sys::PathToUtf8(model) : iscn;
            Resource::MeshIdentity id;
            const bool own = Resource::ParseMeshLibIdentity(bytes.data(), bytes.size(), id, nullptr) ==
                             Resource::MeshIdentityParse::Ok;
            if (!own) id = SceneInstanceLoader::SynthesizeMeshLibIdentity(ps, bytes.data(), bytes.size(), iscn);
            const bool namesSkeleton = Resource::AssetFS::Get().Exists(modelPath) &&
                                       SceneInstanceLoader::NamesSkeleton(modelPath);
            const std::vector<uint8_t> mask = Resource::StaticEntryMask(id, meta.meshCount, namesSkeleton);
            const std::size_t staticEntries = static_cast<std::size_t>(std::count(mask.begin(), mask.end(), 1));
            row["staticEntries"] = staticEntries;

            Resource::MeshletChunkView view;
            std::string                why;
            const Resource::MeshletChunkState ms = Resource::ReadMeshletChunk(bytes.data(), bytes.size(), view, &why);
            std::string state;
            if (ms == Resource::MeshletChunkState::Ok)             state = "meshlets";
            else if (ms == Resource::MeshletChunkState::Malformed) { state = "stale"; row["error"] = why; }
            else if (staticEntries == 0)                           state = "skinned";
            else                                                   state = "none";
            row["state"] = state;
            ++counts[state];
            if (ms == Resource::MeshletChunkState::Ok) row["meshlets"] = MeshletChunkJson(view, single);

            const bool want = state == "none" || state == "stale" || (rebuild && state == "meshlets");
            if (apply && want && staticEntries > 0)
            {
                const std::string junction = ReparseAncestor(fs::absolute(p, ec), cwd);
                if (!junction.empty())
                    row["refused"] = "reached through the reparse point '" + junction + "' (another checkout's file)";
                else if (OnlySharedHas(ps))
                    row["refused"] = "only the shared content root has it (the main checkout's file) - make a local "
                                     "copy first: python tools/worktree_content.py materialize " + ps;
                else
                {
                    Resource::MeshletBuildOptions mo;
                    mo.lod       = lod;
                    mo.entryMask = mask;
                    Resource::MeshletBuildStats st;
                    std::vector<uint8_t> out = bytes;
                    std::vector<uint8_t> back;
                    const size_t coreEnd = Resource::MeshLibCoreEnd(bytes.data(), bytes.size());
                    Resource::MeshLibTailDirectory dirBefore, dirAfter;
                    Resource::ReadMeshLibTailDirectory(bytes.data(), bytes.size(), dirBefore, nullptr);
                    Resource::MeshletChunkView readBack;
                    if (!Resource::AddMeshletsToMeshLib(out, mo, &st, &why))
                        row["refused"] = "the build failed: " + why;
                    else if (!Sys::WriteFileBytesAtomic(ps, out.data(), out.size()))
                        row["refused"] = "write failed";
                    else if (!Sys::ReadFileBytes(ps, back) ||
                             Resource::ReadMeshletChunk(back.data(), back.size(), readBack, &why) !=
                                 Resource::MeshletChunkState::Ok)
                        row["refused"] = "verify: the written MLET does not read back (" + why + ")";
                    else
                    {
                        // Every byte of the geometry, and every other chunk, where it was.
                        const size_t payload = sizeof(Resource::AssetHeader) +
                                               Resource::GetHeader(bytes.data())->metadataSize;
                        bool same = back.size() >= coreEnd &&
                                    std::memcmp(back.data() + payload, bytes.data() + payload, coreEnd - payload) == 0;
                        Resource::ReadMeshLibTailDirectory(back.data(), back.size(), dirAfter, nullptr);
                        for (const Resource::MeshLibChunkRef& c : dirBefore.chunks)
                        {
                            if (!same || c.fourcc == Resource::MESHLIB_CHUNK_MLET) continue;
                            const Resource::MeshLibChunkRef* n = dirAfter.Find(c.fourcc);
                            same = n && n->size == c.size && n->version == c.version &&
                                   std::memcmp(back.data() + n->offset, bytes.data() + c.offset, c.size) == 0;
                        }
                        if (!same) row["refused"] = "verify: geometry or another chunk changed after the write";
                        else
                        {
                            row["written"]  = true;
                            row["meshlets"] = MeshletChunkJson(readBack, single);
                            row["buildMs"]  = st.ms;
                            row["bytesAfter"] = back.size();
                            bytesBefore += bytes.size();
                            bytesAfter  += back.size();
                            Resource::LogMeshletBuild("assettool", ps, st, true);
                        }
                    }
                }
                if (row.contains("refused")) ++nRefused; else ++nWritten;
            }
            list.push_back(std::move(row));
        }

        Result r;
        r.j = Json{ { "ok", nRefused == 0 }, { "apply", apply }, { "rebuild", rebuild }, { "lod", lod },
                    { "files", std::move(list) },
                    { "summary", { { "count", files.size() }, { "meshlets", counts["meshlets"] },
                                   { "none", counts["none"] }, { "stale", counts["stale"] },
                                   { "skinned", counts["skinned"] }, { "collision", counts["collision"] },
                                   { "unreadable", counts["unreadable"] }, { "written", nWritten },
                                   { "refused", nRefused }, { "bytesBefore", bytesBefore },
                                   { "bytesAfter", bytesAfter } } } };
        if (!skippedReparse.empty()) r.j["skippedReparse"] = std::move(skippedReparse);
        r.code = nRefused == 0 ? kDone : kRefused;
        LOG_INFO("[ASSETTOOL] meshlets: %zu file(s) - %zu with meshlets, %zu without, %zu stale, %zu skinned, "
                 "%zu collision, %zu written, %zu refused", files.size(), counts["meshlets"], counts["none"],
                 counts["stale"], counts["skinned"], counts["collision"], nWritten, nRefused);
        return r;
    }
}

namespace
{
    // ==== iscn_keys: the keys in the model files ====
    // Every .iscn under -asset (or the one named) gets SceneInstanceLoader::StampModelKeys: F `sec=`,
    // N `mkey=`, ME records; stale mesh= hints refreshed from existing keys. Dry run unless -apply;
    // idempotent; a file reached through a reparse point (another checkout's, e.g. asset\Bistro) is
    // never written.
    Result IscnKeysCmd(const std::string& path, bool apply)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (path.empty()) return Fail(kUsage, "usage", "-asset=<.iscn or directory> is required");
        const fs::path root = Sys::U8Path(path);
        const fs::path cwd  = fs::current_path(ec);
        std::vector<fs::path> files;
        Json skippedReparse = Json::array();
        if (fs::is_directory(root, ec))
        {
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_directory(ec))
                {
                    const DWORD a = GetFileAttributesW(it->path().c_str());
                    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
                    {
                        skippedReparse.push_back(Sys::PathToUtf8(it->path()));
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (it->is_regular_file(ec) && LowerAscii(Sys::PathToUtf8(it->path().extension())) == ".iscn")
                    files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
        }
        else if (fs::exists(root, ec)) files.push_back(root);
        else return Fail(kUsage, "usage", "no .iscn or directory at '" + path + "'");

        SceneInstanceLoader::LibraryIdentityCache libs;
        Json list = Json::array();
        SceneInstanceLoader::ModelKeyReport tot;
        std::size_t nChanged = 0, nWritten = 0, nRefused = 0, nFailed = 0;
        for (const fs::path& p : files)
        {
            // Project-relative, the spelling AssetFS and the loader use.
            const std::string ps = Sys::RelativizeAssetPath(Sys::PathToUtf8(p));
            Json row{ { "path", ps } };
            const std::string junction = ReparseAncestor(fs::absolute(p, ec), cwd);
            const bool write = apply && junction.empty();
            SceneInstanceLoader::ModelKeyReport rep;
            std::string err;
            if (!SceneInstanceLoader::StampModelKeys(ps, write, rep, libs, &err))
            {
                row["error"] = err;
                ++nFailed;
                list.push_back(std::move(row));
                continue;
            }
            row["f"] = Json{ { "lines", rep.fLines }, { "added", rep.fAdded }, { "kept", rep.fKept },
                             { "hintFixed", rep.fHintFixed }, { "dangling", rep.fDangling },
                             { "unresolved", rep.fUnresolved }, { "legacy", rep.fLegacy } };
            row["n"] = Json{ { "lines", rep.nLines }, { "added", rep.nAdded }, { "kept", rep.nKept },
                             { "changed", rep.nChanged }, { "mixed", rep.nMixed }, { "legacy", rep.nLegacy } };
            row["me"]      = rep.meLines;
            row["changed"] = rep.changed;
            row["written"] = rep.written;
            if (apply && rep.changed && !junction.empty())
            {
                row["refused"] = "reached through the reparse point '" + junction + "' (another checkout's file)";
                ++nRefused;
            }
            if (rep.changed) ++nChanged;
            if (rep.written) ++nWritten;
            tot.fLines += rep.fLines;   tot.fAdded += rep.fAdded;         tot.fKept += rep.fKept;
            tot.fHintFixed += rep.fHintFixed; tot.fDangling += rep.fDangling; tot.fUnresolved += rep.fUnresolved;
            tot.fLegacy += rep.fLegacy;
            tot.nLines += rep.nLines;   tot.nAdded += rep.nAdded;         tot.nKept += rep.nKept;
            tot.nChanged += rep.nChanged; tot.nMixed += rep.nMixed;       tot.meLines += rep.meLines;
            tot.nLegacy += rep.nLegacy;
            list.push_back(std::move(row));
        }

        Result r;
        r.j = Json{ { "ok", nRefused == 0 && nFailed == 0 }, { "apply", apply }, { "files", std::move(list) },
                    { "summary", { { "count", files.size() }, { "changed", nChanged }, { "written", nWritten },
                                   { "refused", nRefused }, { "failed", nFailed },
                                   { "fLines", tot.fLines }, { "fAdded", tot.fAdded }, { "fKept", tot.fKept },
                                   { "fHintFixed", tot.fHintFixed }, { "fDangling", tot.fDangling },
                                   { "fUnresolved", tot.fUnresolved }, { "fLegacy", tot.fLegacy },
                                   { "nLines", tot.nLines }, { "nAdded", tot.nAdded }, { "nKept", tot.nKept },
                                   { "nChanged", tot.nChanged }, { "nMixed", tot.nMixed },
                                   { "nLegacy", tot.nLegacy }, { "meLines", tot.meLines } } } };
        if (!skippedReparse.empty()) r.j["skippedReparse"] = std::move(skippedReparse);
        r.code = (nRefused == 0 && nFailed == 0) ? kDone : kRefused;
        LOG_INFO("[ASSETTOOL] iscn_keys: %zu file(s) - %zu changed, %zu written, %zu refused, %zu failed; "
                 "F %u (added %u, kept %u, hint fixed %u, dangling %u, unresolved %u; %u per-mesh file(s) not "
                 "keyed); N %u (added %u, kept %u, changed %u, mixed %u, per-mesh %u); ME %u", files.size(), nChanged,
                 nWritten, nRefused, nFailed, tot.fLines, tot.fAdded, tot.fKept, tot.fHintFixed, tot.fDangling,
                 tot.fUnresolved, tot.fLegacy, tot.nLines, tot.nAdded, tot.nKept, tot.nChanged, tot.nMixed,
                 tot.nLegacy, tot.meLines);
        return r;
    }

    // ==== scene_keys: the keys in scenes and prefabs ====
    // Every .iscene / .ipfb under -asset (or the one named) gets Resource::StampSceneKeys: srcSec= on
    // override lines, meshSec= on collider meshes, stale hints refreshed. Same rules as iscn_keys: dry
    // run unless -apply, idempotent, a file reached through a reparse point is never written.
    Result SceneKeysCmd(const std::string& path, bool apply)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (path.empty()) return Fail(kUsage, "usage", "-asset=<.iscene / .ipfb or directory> is required");
        const fs::path root = Sys::U8Path(path);
        const fs::path cwd  = fs::current_path(ec);
        auto wanted = [](const fs::path& p)
        {
            const std::string ext = LowerAscii(Sys::PathToUtf8(p.extension()));
            return ext == ".iscene" || ext == ".ipfb";
        };
        std::vector<fs::path> files;
        Json skippedReparse = Json::array();
        if (fs::is_directory(root, ec))
        {
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_directory(ec))
                {
                    const DWORD a = GetFileAttributesW(it->path().c_str());
                    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
                    {
                        skippedReparse.push_back(Sys::PathToUtf8(it->path()));
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (it->is_regular_file(ec) && wanted(it->path())) files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
        }
        else if (fs::exists(root, ec)) files.push_back(root);
        else return Fail(kUsage, "usage", "no .iscene / .ipfb or directory at '" + path + "'");

        using Counts = Resource::SceneKeyReport::Counts;
        auto countsJson = [](const Counts& c)
        {
            return Json{ { "lines", c.lines }, { "added", c.added }, { "kept", c.kept },
                         { "hintFixed", c.hintFixed }, { "dangling", c.dangling }, { "unresolved", c.unresolved } };
        };
        auto accumulate = [](Counts& t, const Counts& c)
        {
            t.lines += c.lines; t.added += c.added; t.kept += c.kept;
            t.hintFixed += c.hintFixed; t.dangling += c.dangling; t.unresolved += c.unresolved;
        };
        Json list = Json::array();
        Counts part, collider, prefabPart;
        std::size_t nChanged = 0, nWritten = 0, nRefused = 0, nFailed = 0;
        for (const fs::path& p : files)
        {
            const std::string ps = Sys::RelativizeAssetPath(Sys::PathToUtf8(p));
            Json row{ { "path", ps } };
            const std::string junction = ReparseAncestor(fs::absolute(p, ec), cwd);
            Resource::SceneKeyReport rep;
            std::string err;
            if (!Resource::StampSceneKeys(ps, apply && junction.empty(), rep, &err))
            {
                row["error"] = err;
                ++nFailed;
                list.push_back(std::move(row));
                continue;
            }
            row["part"]       = countsJson(rep.part);
            row["collider"]   = countsJson(rep.collider);
            row["prefabPart"] = countsJson(rep.prefabPart);
            row["changed"]  = rep.changed;
            row["written"]  = rep.written;
            if (apply && rep.changed && !junction.empty())
            {
                row["refused"] = "reached through the reparse point '" + junction + "' (another checkout's file)";
                ++nRefused;
            }
            if (rep.changed) ++nChanged;
            if (rep.written) ++nWritten;
            accumulate(part, rep.part);
            accumulate(collider, rep.collider);
            accumulate(prefabPart, rep.prefabPart);
            // Only files that name a part or a collider mesh are listed (a
            // directory of scenes is mostly neither).
            if (rep.part.lines || rep.collider.lines || rep.prefabPart.lines || rep.changed)
                list.push_back(std::move(row));
        }

        Result r;
        r.j = Json{ { "ok", nRefused == 0 && nFailed == 0 }, { "apply", apply }, { "files", std::move(list) },
                    { "summary", { { "count", files.size() }, { "changed", nChanged }, { "written", nWritten },
                                   { "refused", nRefused }, { "failed", nFailed },
                                   { "part", countsJson(part) }, { "collider", countsJson(collider) },
                                   { "prefabPart", countsJson(prefabPart) } } } };
        if (!skippedReparse.empty()) r.j["skippedReparse"] = std::move(skippedReparse);
        r.code = (nRefused == 0 && nFailed == 0) ? kDone : kRefused;
        LOG_INFO("[ASSETTOOL] scene_keys: %zu file(s) - %zu changed, %zu written, %zu refused, %zu failed; "
                 "srcSec %u (added %u, kept %u, hint fixed %u, dangling %u, unresolved %u); meshSec %u (added %u, "
                 "kept %u, hint fixed %u, dangling %u, unresolved %u); prefab msec %u (added %u, kept %u, "
                 "differs %u)", files.size(), nChanged, nWritten, nRefused,
                 nFailed, part.lines, part.added, part.kept, part.hintFixed, part.dangling, part.unresolved,
                 collider.lines, collider.added, collider.kept, collider.hintFixed, collider.dangling,
                 collider.unresolved, prefabPart.lines, prefabPart.added, prefabPart.kept, prefabPart.dangling);
        return r;
    }

    // The files under @p root (or @p root itself) whose extension is one of @p exts,
    // sorted; a directory reached through a reparse point (another checkout's) is
    // listed in @p skippedReparse and not entered. False when @p root does not exist.
    bool CollectToolFiles(const std::string& root, std::initializer_list<const char*> exts,
                          std::vector<std::filesystem::path>& files, Json& skippedReparse)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path r = Sys::U8Path(root);
        auto wanted = [&](const fs::path& p)
        {
            const std::string ext = LowerAscii(Sys::PathToUtf8(p.extension()));
            for (const char* x : exts) if (ext == x) return true;
            return false;
        };
        if (fs::is_directory(r, ec))
        {
            for (auto it = fs::recursive_directory_iterator(r, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (it->is_directory(ec))
                {
                    const DWORD a = GetFileAttributesW(it->path().c_str());
                    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
                    {
                        skippedReparse.push_back(Sys::PathToUtf8(it->path()));
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (it->is_regular_file(ec) && wanted(it->path())) files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
            return true;
        }
        if (!fs::exists(r, ec)) return false;
        files.push_back(r);
        return true;
    }

    // ==== collision_migrate: C records -> the model's BO / PR ====
    // Every .iscn under -asset (or the one named) gets SceneInstanceLoader::MigrateModelCollision. Dry
    // run unless -apply; idempotent; a file reached through a reparse point is never written. Run
    // mc_echoes on the scenes that place a model first - their echo lines are judged against the C
    // records this removes.
    Result CollisionMigrateCmd(const std::string& path, bool apply)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (path.empty()) return Fail(kUsage, "usage", "-asset=<.iscn or directory> is required");
        std::vector<fs::path> files;
        Json skippedReparse = Json::array();
        if (!CollectToolFiles(path, { ".iscn" }, files, skippedReparse))
            return Fail(kUsage, "usage", "no .iscn or directory at '" + path + "'");
        const fs::path cwd = fs::current_path(ec);

        SceneInstanceLoader::LibraryIdentityCache libs;
        Json list = Json::array();
        SceneInstanceLoader::CollisionMigrationReport tot;
        std::size_t nChanged = 0, nWritten = 0, nRefused = 0, nFailed = 0, nMeshesRefused = 0;
        for (const fs::path& p : files)
        {
            const std::string ps = Sys::RelativizeAssetPath(Sys::PathToUtf8(p));
            Json row{ { "path", ps } };
            const std::string junction = ReparseAncestor(fs::absolute(p, ec), cwd);
            SceneInstanceLoader::CollisionMigrationReport rep;
            std::string err;
            if (!SceneInstanceLoader::MigrateModelCollision(ps, apply && junction.empty(), rep, libs, &err))
            {
                row["error"] = err;
                ++nFailed;
                list.push_back(std::move(row));
                continue;
            }
            if (rep.cLines == 0 && !rep.changed) continue;   // a model with no C records
            Json refused = Json::array();
            for (const auto& [key, why] : rep.refused)
                refused.push_back(Json{ { "mesh", Resource::MeshKeyToHex(key) }, { "why", why } });
            row["cLines"]        = rep.cLines;
            row["migrated"]      = rep.migrated;
            row["meshes"]        = rep.meshes;
            row["complexMeshes"] = rep.complexMeshes;
            row["bodyPrims"]     = rep.bodyPrims;
            row["sectionPrims"]  = rep.sectionPrims;
            row["compounds"]     = rep.compounds;
            row["colEntities"]   = rep.colEntities;
            row["boLines"]       = rep.boLines;
            row["refused"]       = std::move(refused);
            row["changed"]       = rep.changed;
            row["written"]       = rep.written;
            if (apply && rep.changed && !junction.empty())
            {
                row["refusedWrite"] = "reached through the reparse point '" + junction + "' (another checkout's file)";
                ++nRefused;
            }
            if (rep.changed) ++nChanged;
            if (rep.written) ++nWritten;
            nMeshesRefused    += rep.refused.size();
            tot.cLines        += rep.cLines;       tot.migrated     += rep.migrated;
            tot.meshes        += rep.meshes;       tot.complexMeshes += rep.complexMeshes;
            tot.bodyPrims     += rep.bodyPrims;    tot.sectionPrims += rep.sectionPrims;
            tot.compounds     += rep.compounds;    tot.colEntities  += rep.colEntities;
            list.push_back(std::move(row));
        }

        Result r;
        r.j = Json{ { "ok", nRefused == 0 && nFailed == 0 }, { "apply", apply }, { "files", std::move(list) },
                    { "summary", { { "count", files.size() }, { "changed", nChanged }, { "written", nWritten },
                                   { "refusedWrite", nRefused }, { "failed", nFailed },
                                   { "cLines", tot.cLines }, { "migrated", tot.migrated }, { "meshes", tot.meshes },
                                   { "complexMeshes", tot.complexMeshes }, { "bodyPrims", tot.bodyPrims },
                                   { "sectionPrims", tot.sectionPrims }, { "compounds", tot.compounds },
                                   { "colEntities", tot.colEntities }, { "meshesRefused", nMeshesRefused } } } };
        if (!skippedReparse.empty()) r.j["skippedReparse"] = std::move(skippedReparse);
        r.code = (nRefused == 0 && nFailed == 0) ? kDone : kRefused;
        LOG_INFO("[ASSETTOOL] collision_migrate: %zu file(s) - %zu changed, %zu written, %zu refused, %zu failed; "
                 "C %u (migrated %u); %u Mesh(es) (complex %u, compound %u), PR %u body + %u section, %u _colN "
                 "entit(ies) fewer, %zu Mesh(es) kept on C", files.size(), nChanged, nWritten, nRefused, nFailed,
                 tot.cLines, tot.migrated, tot.meshes, tot.complexMeshes, tot.compounds, tot.bodyPrims,
                 tot.sectionPrims, tot.colEntities, nMeshesRefused);
        return r;
    }

    // ==== mc_echoes: drop the MC lines that restate a model's C records ====
    // Every .iscene / .ipfb under -asset (or the one named) gets Resource::StripColliderEchoes. Same
    // rules as scene_keys: dry run unless -apply, idempotent, a file reached through a reparse point is
    // never written.
    Result McEchoesCmd(const std::string& path, bool apply)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (path.empty()) return Fail(kUsage, "usage", "-asset=<.iscene / .ipfb or directory> is required");
        std::vector<fs::path> files;
        Json skippedReparse = Json::array();
        if (!CollectToolFiles(path, { ".iscene", ".ipfb" }, files, skippedReparse))
            return Fail(kUsage, "usage", "no .iscene / .ipfb or directory at '" + path + "'");
        const fs::path cwd = fs::current_path(ec);

        Json list = Json::array();
        Resource::ColliderEchoReport tot;
        std::size_t nChanged = 0, nWritten = 0, nRefused = 0, nFailed = 0;
        for (const fs::path& p : files)
        {
            const std::string ps = Sys::RelativizeAssetPath(Sys::PathToUtf8(p));
            Json row{ { "path", ps } };
            const std::string junction = ReparseAncestor(fs::absolute(p, ec), cwd);
            Resource::ColliderEchoReport rep;
            std::string err;
            if (!Resource::StripColliderEchoes(ps, apply && junction.empty(), rep, &err))
            {
                row["error"] = err;
                ++nFailed;
                list.push_back(std::move(row));
                continue;
            }
            if (rep.mcLines == 0) continue;   // only files that carry MC lines are listed
            row["mcLines"]    = rep.mcLines;
            row["echoes"]     = rep.echoes;
            row["authored"]   = rep.authored;
            row["differs"]    = rep.differs;
            row["unplaced"]   = rep.unplaced;
            row["staticMesh"] = rep.staticMesh;
            row["unparsed"]   = rep.unparsed;
            row["models"]     = rep.models;
            row["changed"]    = rep.changed;
            row["written"]    = rep.written;
            if (apply && rep.changed && !junction.empty())
            {
                row["refusedWrite"] = "reached through the reparse point '" + junction + "' (another checkout's file)";
                ++nRefused;
            }
            if (rep.changed) ++nChanged;
            if (rep.written) ++nWritten;
            tot.mcLines += rep.mcLines;   tot.echoes += rep.echoes;     tot.authored += rep.authored;
            tot.differs += rep.differs;   tot.unplaced += rep.unplaced; tot.staticMesh += rep.staticMesh;
            tot.unparsed += rep.unparsed;
            list.push_back(std::move(row));
        }

        Result r;
        r.j = Json{ { "ok", nRefused == 0 && nFailed == 0 }, { "apply", apply }, { "files", std::move(list) },
                    { "summary", { { "count", files.size() }, { "changed", nChanged }, { "written", nWritten },
                                   { "refusedWrite", nRefused }, { "failed", nFailed },
                                   { "mcLines", tot.mcLines }, { "echoes", tot.echoes }, { "authored", tot.authored },
                                   { "differs", tot.differs }, { "unplaced", tot.unplaced },
                                   { "staticMesh", tot.staticMesh }, { "unparsed", tot.unparsed } } } };
        if (!skippedReparse.empty()) r.j["skippedReparse"] = std::move(skippedReparse);
        r.code = (nRefused == 0 && nFailed == 0) ? kDone : kRefused;
        LOG_INFO("[ASSETTOOL] mc_echoes: %zu file(s) - %zu changed, %zu written, %zu refused, %zu failed; MC %u: "
                 "%u echo(es) dropped, kept %u authored / %u differ / %u unplaced / %u static-mesh / %u unparsed",
                 files.size(), nChanged, nWritten, nRefused, nFailed, tot.mcLines, tot.echoes, tot.authored,
                 tot.differs, tot.unplaced, tot.staticMesh, tot.unparsed);
        return r;
    }

    // ==== bake_primitives: the engine's Primitives model ====
    // engine/meshes/Primitives.meshlib + .iscn (Scene/PrimitiveAssets), deterministic; a .meta sidecar
    // stamped for a file that has none (a re-bake keeps the identity).
    Result BakePrimitivesCmd(const std::string& outDir)
    {
        PrimitiveAssets::Stats st;
        std::string            err;
        if (!PrimitiveAssets::Write(outDir, st, &err)) return Fail(kUsage, "write_failed", err);
        Json stamped = Json::array();
        for (const std::string& f : { st.meshlib, st.iscn })
        {
            const std::string meta = Resource::MetaPathFor(f);
            if (Sys::FileExists(meta)) continue;
            const std::string text = Resource::BuildMetaText(ECS::Guid::Generate());
            if (!Sys::WriteFileBytes(meta, text.data(), text.size()))
                return Fail(kUsage, "write_failed", "cannot write '" + meta + "'");
            stamped.push_back(meta);
        }
        Result r;
        r.j = Json{ { "ok", true }, { "iscn", st.iscn }, { "meshlib", st.meshlib }, { "meshes", st.meshes },
                    { "vertices", st.vertices }, { "indices", st.indices },
                    { "meshletClusters", st.meshletClusters }, { "stamped", stamped },
                    { "iscnFile", FileIdentity(st.iscn) }, { "meshlibFile", FileIdentity(st.meshlib) } };
        return r;
    }

    // ==== matgraph: material graph -> its generated HLSL ====
    // -asset=<x.imatgraph>: compile it (-matgraph_shading / -matgraph_blend, default Standard /
    // Opaque) and report; -out=<file> writes the HLSL.
    // -asset=<x.imat | folder>: every material with a `materialGraph` key - compile the graph with the
    // material's shading model and blend mode and compare with the file its customShaderPath names.
    // Dry run lists what is stale (exit 1); -apply writes it. The compiler's own report
    // (MaterialGraph::CompileReportJson) is the shape Lua Editor.MaterialGraphCompile answers too.
    Json MatGraphCompileJson(const MaterialGraph::CompileResult& c)
    {
        return Json::parse(MaterialGraph::CompileReportJson(c, false));
    }

    bool EndsWithNoCase(const std::string& s, const char* suffix)
    {
        const std::size_t n = std::strlen(suffix);
        if (s.size() < n) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != std::tolower(static_cast<unsigned char>(suffix[i])))
                return false;
        return true;
    }

    std::string ForwardSlashes(std::string p)
    {
        for (char& c : p) if (c == '\\') c = '/';
        return p;
    }

    std::string WithoutCR(std::string s)
    {
        s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
        return s;
    }

    // One material: "ok" | "stale" | "missing" | "written" | "error".
    Json MatGraphCheckMaterial(const std::string& path, bool apply)
    {
        Json row{ { "material", path } };
        auto error = [&](const std::string& why) { row["status"] = "error"; row["message"] = why; return row; };
        MaterialComponent mat;
        if (!Resource::LoadMaterial(path, mat)) return error("cannot load the material");
        row["graph"]  = mat.materialGraph;
        row["shader"] = mat.customShaderPath;
        row["shading"] = ShadingModelName(mat.customShadingModel);
        row["blend"]   = MaterialGraph::BlendModeLabel(mat.userBlendMode);
        if (mat.materialGraph.empty()) return error("the material has no materialGraph key");

        MaterialGraph::Graph g;
        std::string err;
        if (!MaterialGraph::LoadGraph(mat.materialGraph, g, err)) return error(err);
        MaterialGraph::CompileOptions o;
        o.shadingModel = mat.customShadingModel;
        o.blendMode    = mat.userBlendMode;
        o.sourceLabel  = mat.materialGraph;
        for (const auto& [name, on] : mat.staticSwitches) o.switches[name] = on;   // its static-switch combination
        const MaterialGraph::CompileResult c = MaterialGraph::Compile(g, o);
        const Json cj = MatGraphCompileJson(c);
        for (auto it = cj.begin(); it != cj.end(); ++it) row[it.key()] = it.value();
        if (!c.ok) return error("the graph does not compile");
        if (!mat.useCustomShader || mat.customShaderPath.empty())
            return error("useCustomShader is off or customShaderPath is empty: nothing points at the generated HLSL");
        if (mat.customShaderPath.find('/') == std::string::npos)
            return error("customShaderPath must be a project-relative path (asset/Shaders/Generated/...)");

        std::string current;
        const bool exists = Sys::ReadFileText(mat.customShaderPath, current);
        // CRLF vs LF is the same shader (a checkout's line-ending policy, not an edit).
        const bool same = exists && WithoutCR(current) == c.hlsl;
        row["status"] = same ? "ok" : exists ? "stale" : "missing";
        if (!same && apply)
        {
            Sys::CreateParentDirs(mat.customShaderPath);
            bool locked = false;
            if (!Sys::WriteFileBytesAtomic(mat.customShaderPath, c.hlsl.data(), c.hlsl.size(), &locked))
                return error(locked ? "the shader file stayed locked by a reader" : "cannot write the shader file");
            row["status"] = "written";
        }
        return row;
    }

    // -matgraph_switches=Name=1,Other=0 (1/0, true/false, on/off).
    bool ParseSwitches(const std::string& text, std::map<std::string, bool>& out, std::string& why)
    {
        std::size_t b = 0;
        while (b < text.size())
        {
            std::size_t e = text.find(',', b);
            if (e == std::string::npos) e = text.size();
            const std::string item = text.substr(b, e - b);
            b = e + 1;
            if (item.empty()) continue;
            const std::size_t eq = item.find('=');
            const std::string v = eq == std::string::npos ? std::string() : item.substr(eq + 1);
            if (eq == 0 || eq == std::string::npos || (v != "1" && v != "0" && v != "true" && v != "false" && v != "on" && v != "off"))
            {
                why = "'" + item + "' is not <name>=1|0";
                return false;
            }
            out[item.substr(0, eq)] = v == "1" || v == "true" || v == "on";
        }
        return true;
    }

    Result MatGraphCmd(const std::string& asset, bool apply, const std::string& shading, const std::string& blend,
                       const std::string& out, const std::string& switches)
    {
        if (asset.empty()) return Fail(kUsage, "usage", "-asset=<.imatgraph | .imatfunc | .imat | folder> is required");
        if (EndsWithNoCase(asset, ".imatgraph") || EndsWithNoCase(asset, ".imatfunc"))
        {
            MaterialGraph::Graph g;
            std::string err, text;
            if (!Sys::ReadFileText(asset, text)) return Fail(kUsage, "load_failed", "cannot read '" + asset + "'");
            if (!MaterialGraph::ParseGraph(text, g, err)) return Fail(kUsage, "load_failed", asset + ": " + err);
            // Is the file what SerializeGraph writes? -apply rewrites it so (after a tool or a hand edit that
            // wrote other JSON), as an editor save would. Line endings do not count (.gitattributes pins LF;
            // a CRLF checkout is the same graph).
            const bool canonical = MaterialGraph::SerializeGraph(g) == WithoutCR(text);
            bool rewritten = false;
            if (!canonical && apply)
            {
                if (!MaterialGraph::SaveGraph(asset, g, err)) return Fail(kUsage, "write_failed", err);
                rewritten = true;
            }
            MaterialGraph::CompileOptions o;
            o.sourceLabel = ForwardSlashes(asset);
            if (!shading.empty() && !MaterialGraph::ParseShadingModel(shading, o.shadingModel))
                return Fail(kUsage, "usage", "-matgraph_shading='" + shading + "' is not a shading model");
            if (!blend.empty() && !MaterialGraph::ParseBlendMode(blend, o.blendMode))
                return Fail(kUsage, "usage", "-matgraph_blend='" + blend + "' is not a blend mode");
            if (std::string why; !ParseSwitches(switches, o.switches, why))
                return Fail(kUsage, "usage", "-matgraph_switches: " + why);
            const MaterialGraph::CompileResult c = MaterialGraph::Compile(g, o);
            Result r;
            r.j = MatGraphCompileJson(c);
            r.j["ok"]        = c.ok;
            r.j["canonical"] = canonical;
            r.j["rewritten"] = rewritten;
            r.j["graph"]     = o.sourceLabel;
            r.j["shading"] = ShadingModelName(o.shadingModel);
            r.j["blend"]   = MaterialGraph::BlendModeLabel(o.blendMode);
            if (c.ok && !out.empty())
            {
                Sys::CreateParentDirs(out);
                if (!Sys::WriteFileBytesAtomic(out, c.hlsl.data(), c.hlsl.size()))
                    return Fail(kUsage, "write_failed", "cannot write -out='" + out + "'");
                r.j["out"] = out;
            }
            r.code = c.ok ? kDone : kRefused;
            LOG_INFO("[ASSETTOOL] matgraph %s: %s, %zu diagnostic(s), %d bytes", o.sourceLabel.c_str(),
                     c.ok ? "compiled" : "FAILED", c.diagnostics.size(), c.stats.hlslBytes);
            return r;
        }

        std::vector<std::string> materials;
        std::error_code ec;
        if (EndsWithNoCase(asset, ".imat")) materials.push_back(ForwardSlashes(asset));
        else if (fs::is_directory(Sys::U8Path(asset), ec))
        {
            for (fs::recursive_directory_iterator it(Sys::U8Path(asset), fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                if (!it->is_regular_file(ec)) continue;
                const std::string p = ForwardSlashes(Sys::PathToUtf8(it->path()));
                if (!EndsWithNoCase(p, ".imat")) continue;
                std::string bytes;
                // The text payload is stored verbatim: a byte search skips the load
                // of every material that has no graph.
                if (Sys::ReadFileText(p, bytes) && bytes.find("materialGraph") != std::string::npos) materials.push_back(p);
            }
            std::sort(materials.begin(), materials.end());
        }
        else return Fail(kUsage, "usage", "no .imatgraph, .imat or directory at '" + asset + "'");

        Json rows = Json::array();
        std::map<std::string, int> counts{ { "ok", 0 }, { "stale", 0 }, { "missing", 0 }, { "written", 0 }, { "error", 0 } };
        for (const std::string& m : materials)
        {
            Json row = MatGraphCheckMaterial(m, apply);
            ++counts[row.value("status", std::string("error"))];
            rows.push_back(std::move(row));
        }
        const bool clean = counts["error"] == 0 && (apply || (counts["stale"] == 0 && counts["missing"] == 0));
        Result r;
        r.j = Json{ { "ok", clean }, { "apply", apply }, { "materials", std::move(rows) },
                    { "summary", { { "count", materials.size() }, { "ok", counts["ok"] }, { "stale", counts["stale"] },
                                   { "missing", counts["missing"] }, { "written", counts["written"] },
                                   { "error", counts["error"] } } } };
        r.code = clean ? kDone : kRefused;
        LOG_INFO("[ASSETTOOL] matgraph %s: %zu material(s) - %d ok, %d stale, %d missing, %d written, %d error",
                 asset.c_str(), materials.size(), counts["ok"], counts["stale"], counts["missing"], counts["written"],
                 counts["error"]);
        return r;
    }
}

int RunAssetToolIfRequested()
{
    auto& cl = Sys::CommandLine::Get();
    if (!cl.Has("-assettool")) return -1;
    const std::string command = cl.Get("-assettool");
    const std::string outJson = cl.Get("-assettool_out");
    const std::string asset   = cl.Get("-asset");
    // A worktree sharing the main checkout's binary assets sees the whole
    // tree the engine does (Resource/SharedContent.h).
    Resource::SharedContent::Configure();

    Result r;
    try
    {
        if      (command == "inspect")   r = Inspect(asset);
        else if (command == "validate")  r = Validate(asset, cl.Get("-assettool_refs", "1") != "0");
        else if (command == "patch")     r = Patch(asset, cl.Get("-ops"), cl.Get("-out"), cl.Has("-dry_run"),
                                                   cl.Get("-expect_hash"));
        else if (command == "roundtrip") r = Roundtrip(asset);
        else if (command == "material_patch") r = MaterialPatch(asset, cl.Get("-ops"), cl.Get("-out"),
            cl.Has("-dry_run"), cl.Get("-expect_hash"));
        else if (command == "catalog")   r = Catalog(cl.Get("-assettool_roots"), cl.Get("-assettool_catalog_cache"),
            cl.Has("-assettool_catalog_incremental") || cl.Has("-assettool_catalog_cache"));
        else if (command == "compat")    r = Compat(cl.Get("-skeleton"), cl.Get("-anim"));
        else if (command == "skeleton_report")
            r = SkeletonReportCmd(cl.Get("-skeleton"), cl.Get("-bones", "0") == "1");
        else if (command == "meshlib_identity")
            r = MeshLibIdentityCmd(asset, cl.Has("-apply"), cl.Has("-force"), cl.Get("-iscn"));
        else if (command == "bake_primitives")
            r = BakePrimitivesCmd(cl.Get("-out"));
        else if (command == "meshlets")
            r = MeshletsCmd(asset, cl.Has("-apply"), cl.Has("-rebuild"), cl.Get("-meshlet_lod", "1") != "0",
                            cl.Get("-iscn"));
        else if (command == "iscn_keys")
            r = IscnKeysCmd(asset, cl.Has("-apply"));
        else if (command == "scene_keys")
            r = SceneKeysCmd(asset, cl.Has("-apply"));
        else if (command == "collision_migrate")
            r = CollisionMigrateCmd(asset, cl.Has("-apply"));
        else if (command == "mc_echoes")
            r = McEchoesCmd(asset, cl.Has("-apply"));
        else if (command == "matgraph")
            r = MatGraphCmd(asset, cl.Has("-apply"), cl.Get("-matgraph_shading"), cl.Get("-matgraph_blend"), cl.Get("-out"),
                            cl.Get("-matgraph_switches"));
        else if (command.rfind("ui_", 0) == 0)   // the .iui verbs (Harness/AssetToolUI.h)
            r.code = RunUIAssetTool(command, r.j);
        else if (command == "shadercook")        // Harness/AssetToolShaderCook.h
            r.code = RunShaderCook(r.j);
        else if (command == "matdecl")           // Harness/AssetToolMatDecl.h
            r.code = RunMatDeclCheck(r.j);
        else if (command == "upgrade")           // Harness/AssetToolUpgrade.h
            r.code = RunUpgrade(r.j);
        else r = Fail(kUsage, "usage",
                      "-assettool=inspect|validate|patch|roundtrip|catalog|compat|skeleton_report|meshlib_identity|bake_primitives|"
                      "meshlets|iscn_keys|scene_keys|collision_migrate|mc_echoes|matgraph|ui_inspect|ui_validate|ui_fmt|"
                      "ui_patch|ui_layout|ui_convert|shadercook|matdecl|upgrade");
    }
    catch (const std::exception& e)
    {
        r = Fail(kUsage, "exception", e.what());
    }
    r.j["command"]  = command;
    r.j["exitCode"] = r.code;
    const std::string text = r.j.dump(1, ' ', false, Json::error_handler_t::replace);
    if (!outJson.empty())
    {
        if (!Sys::WriteFileBytes(outJson, text.data(), text.size()))
            LOG_ERROR("[ASSETTOOL] cannot write -assettool_out='%s'", outJson.c_str());
    }
    else
        std::fwrite(text.data(), 1, text.size(), stdout);
    LOG_INFO("[ASSETTOOL] %s -> exit %d", command.c_str(), r.code);
    return r.code;
}
} // namespace Harness

#endif // WITH_HARNESS
