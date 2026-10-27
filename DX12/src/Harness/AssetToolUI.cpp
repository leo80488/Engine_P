#include "Harness/AssetToolUI.h"

// Self-guard as well as the vcxproj's ExcludedFromBuild: Retail has no tool mode.
#ifdef WITH_HARNESS

#include "ECS/ECS.h"
#include "Renderer/UIView.h"
#include "Renderer/UIViewService.h"     // LayoutDesc: the kernel, without a view
#include "Resource/AssetFS.h"
#include "Resource/AssetRefs.h"         // PercentDecode
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "UI/UIAsset.h"
#include "UI/UICanvasLayout.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <unordered_map>

namespace Harness
{
namespace
{
    using Json = nlohmann::json;
    constexpr int kDone = 0, kRefused = 1, kUsage = 2;

    int Fail(Json& out, int code, const char* error, const std::string& message)
    {
        out["ok"]      = false;
        out["error"]   = error;
        out["message"] = message;
        return code;
    }

    // A file's bytes: the mounted pak, else the disk (a worktree's shared content too).
    bool ReadText(const std::string& path, std::string& text)
    {
        std::vector<std::uint8_t> bytes;
        if (!Resource::AssetFS::Get().ReadFile(Sys::RelativizeAssetPath(path), bytes) && !Sys::ReadFileBytes(path, bytes))
            return false;
        text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

    bool WriteText(const std::string& path, const std::string& text)
    {
        Sys::CreateParentDirs(path);
        return Sys::WriteFileBytesAtomic(path, text.data(), text.size());
    }

    Json IssuesJson(const std::vector<UI::UIAssetIssue>& issues)
    {
        Json a = Json::array();
        for (const UI::UIAssetIssue& i : issues) a.push_back(Json{ { "where", i.where }, { "message", i.message } });
        return a;
    }

    // Read and parse @p path; false = refused (out filled).
    bool Load(const std::string& path, UI::UIAsset& asset, std::string& text, Json& out, int& code)
    {
        if (path.empty()) { code = Fail(out, kUsage, "usage", "-asset=<x.iui> is required"); return false; }
        if (!ReadText(path, text)) { code = Fail(out, kUsage, "io", "cannot read " + path); return false; }
        std::vector<UI::UIAssetIssue> issues;
        if (!UI::ParseUIAsset(text, asset, issues))
        {
            code = Fail(out, kRefused, "invalid", path + ": " + std::to_string(issues.size()) + " issue(s)");
            out["issues"] = IssuesJson(issues);
            return false;
        }
        return true;
    }

    bool Exists(const std::string& path)
    {
        Resource::AssetFS& fs = Resource::AssetFS::Get();
        const std::string canon = Sys::RelativizeAssetPath(path);
        if (fs.HasInPak(canon)) return true;
        std::string onDisk = fs.ResolveOnDisk(canon);
        if (onDisk.empty()) onDisk = canon;
        return Sys::FileExists(onDisk);
    }

    // The canonical text as a JSON tree (what inspect shows and patch edits).
    Json Tree(const UI::UIAsset& a) { return Json::parse(UI::WriteUIAsset(a)); }

    // ==== inspect ====
    int Inspect(const std::string& path, Json& out)
    {
        UI::UIAsset a;
        std::string text;
        int code = kDone;
        if (!Load(path, a, text, out, code)) return code;
        Json rows = Json::array();
        std::function<void(const Json&, int, const std::string&)> walk = [&](const Json& n, int depth, const std::string& parent) {
            Json row = Json::object();
            row["id"]     = n.value("id", "");
            row["name"]   = n.value("name", "");
            row["depth"]  = depth;
            row["parent"] = parent;
            for (auto it = n.begin(); it != n.end(); ++it)
                if (it.key() != "id" && it.key() != "name" && it.key() != "children") row[it.key()] = it.value();
            row["children"] = n.contains("children") ? n["children"].size() : 0;
            rows.push_back(row);
            if (n.contains("children"))
                for (const Json& c : n["children"]) walk(c, depth + 1, n.value("id", ""));
        };
        walk(Tree(a)["root"], 0, "");
        out["ok"]        = true;
        out["path"]      = path;
        out["nodes"]     = rows.size();
        out["canonical"] = UI::WriteUIAsset(a) == text;
        out["tree"]      = rows;
        return kDone;
    }

    // ==== validate ====
    int Validate(const std::string& path, bool refs, Json& out)
    {
        std::string text;
        if (path.empty()) return Fail(out, kUsage, "usage", "-asset=<x.iui> is required");
        if (!ReadText(path, text)) return Fail(out, kUsage, "io", "cannot read " + path);
        UI::UIAsset a;
        std::vector<UI::UIAssetIssue> issues;
        const bool parsed = UI::ParseUIAsset(text, a, issues);
        Json refRows = Json::array();
        if (parsed && refs)
            for (const UI::UIAssetRef& r : UI::UIAssetRefs(a))
            {
                const bool ok = Exists(r.path);
                refRows.push_back(Json{ { "node", r.id }, { "field", r.field }, { "path", r.path }, { "exists", ok } });
                if (!ok) issues.push_back({ r.id + "." + r.field, "'" + r.path + "' does not exist" });
            }
        out["ok"]        = issues.empty();
        out["path"]      = path;
        out["nodes"]     = parsed ? a.NodeCount() : 0;
        out["canonical"] = parsed && UI::WriteUIAsset(a) == text;
        out["issues"]    = IssuesJson(issues);
        out["refs"]      = refRows;
        return issues.empty() ? kDone : kRefused;
    }

    // ==== fmt ====
    int Fmt(const std::string& path, bool check, Json& out)
    {
        UI::UIAsset a;
        std::string text;
        int code = kDone;
        if (!Load(path, a, text, out, code)) return code;
        const std::string canon = UI::WriteUIAsset(a);
        const bool was = canon == text;
        out["path"]      = path;
        out["canonical"] = was;
        out["written"]   = false;
        if (was || check)
        {
            out["ok"] = was;
            if (!was) out["message"] = "not in canonical form (asset_tool.py ui fmt rewrites it)";
            return was ? kDone : kRefused;
        }
        if (!WriteText(path, canon)) return Fail(out, kUsage, "io", "cannot write " + path);
        out["ok"]      = true;
        out["written"] = true;
        return kDone;
    }

    // ==== patch ====
    // The node object with id @p id in the JSON tree (its parent and index too).
    Json* FindNode(Json& n, const std::string& id, Json** parent = nullptr, std::size_t* index = nullptr)
    {
        if (n.value("id", "") == id) return &n;
        if (!n.contains("children")) return nullptr;
        Json& kids = n["children"];
        for (std::size_t i = 0; i < kids.size(); ++i)
        {
            if (kids[i].value("id", "") == id)
            {
                if (parent) *parent = &n;
                if (index) *index = i;
                return &kids[i];
            }
            if (Json* f = FindNode(kids[i], id, parent, index)) return f;
        }
        return nullptr;
    }

    bool Contains(const Json& n, const std::string& id)
    {
        if (n.value("id", "") == id) return true;
        if (n.contains("children"))
            for (const Json& c : n["children"])
                if (Contains(c, id)) return true;
        return false;
    }

    int Patch(const std::string& path, const std::string& opsPath, const std::string& outPath, bool dryRun, Json& out)
    {
        UI::UIAsset a;
        std::string text;
        int code = kDone;
        if (!Load(path, a, text, out, code)) return code;
        std::string opsText;
        if (opsPath.empty() || !ReadText(opsPath, opsText)) return Fail(out, kUsage, "usage", "-ops=<ops.json> is required");
        Json ops;
        try
        {
            ops = Json::parse(opsText);
        }
        catch (const std::exception& e)
        {
            return Fail(out, kUsage, "usage", std::string("the ops file is not JSON: ") + e.what());
        }
        const Json list = ops.is_object() && ops.contains("ops") ? ops["ops"] : ops;
        if (!list.is_array()) return Fail(out, kUsage, "usage", "{\"ops\": [ ... ]}");
        Json doc = Tree(a);
        Json& root = doc["root"];
        int i = 0;
        for (const Json& op : list)
        {
            const std::string kind = op.value("op", "");
            const std::string at   = "ops[" + std::to_string(i++) + "] (" + kind + ")";
            auto refuse = [&](const std::string& m) { return Fail(out, kRefused, "refused", at + ": " + m); };
            if (!op.is_object()) return Fail(out, kRefused, "refused", at + ": an op is an object");
            // "node" names the node for every op but add, whose "node" is the new node itself.
            const std::string id = (op.contains("node") && op["node"].is_string()) ? op["node"].get<std::string>() : "";
            Json* parent = nullptr;
            std::size_t index = 0;
            Json* node = (kind == "add") ? nullptr : FindNode(root, id, &parent, &index);
            if (kind != "add" && !node) return refuse("no node '" + id + "'");
            if (kind == "set")
            {
                const std::string block = op.value("block", "");
                if (block.empty() || !op.contains("values") || !op["values"].is_object())
                    return refuse("{\"block\": ..., \"values\": {...}}");
                // The block exists after a set even with no values (a component added at
                // its defaults) - and the final parse judges whether it may.
                Json& b = (*node)[block];
                if (!b.is_object()) b = Json::object();
                for (auto it = op["values"].begin(); it != op["values"].end(); ++it) b[it.key()] = it.value();
            }
            else if (kind == "unset")
            {
                const std::string block = op.value("block", "");
                if (!node->contains(block)) return refuse("the node has no " + block);
                if (!op.contains("fields") || !op["fields"].is_array()) return refuse("{\"fields\": [...]}");
                for (const Json& f : op["fields"])
                    if (f.is_string()) (*node)[block].erase(f.get<std::string>());
            }
            else if (kind == "remove_block")
            {
                const std::string block = op.value("block", "");
                if (!node->contains(block)) return refuse("the node has no " + block);
                node->erase(block);
            }
            else if (kind == "add")
            {
                Json* into = FindNode(root, op.value("parent", ""));
                if (!into) return refuse("no parent '" + op.value("parent", "") + "'");
                if (!op.contains("node") || !op["node"].is_object()) return refuse("{\"node\": {...}}");
                Json& kids = (*into)["children"];
                if (!kids.is_array()) kids = Json::array();
                const long long at2 = op.value("index", static_cast<long long>(kids.size()));
                const std::size_t pos = static_cast<std::size_t>(std::clamp<long long>(at2, 0, static_cast<long long>(kids.size())));
                kids.insert(kids.begin() + static_cast<std::ptrdiff_t>(pos), op["node"]);
            }
            else if (kind == "remove")
            {
                if (!parent) return refuse("the root is not removed (delete the file)");
                (*parent)["children"].erase(index);
                if ((*parent)["children"].empty()) parent->erase("children");
            }
            else if (kind == "move")
            {
                if (!parent) return refuse("the root does not move");
                const std::string to = op.value("parent", "");
                if (Contains(*node, to)) return refuse("'" + to + "' is the node or under it: a cycle");
                Json moving = *node;
                (*parent)["children"].erase(index);
                if ((*parent)["children"].empty()) parent->erase("children");
                Json* into = FindNode(root, to);
                if (!into) return refuse("no parent '" + to + "'");
                Json& kids = (*into)["children"];
                if (!kids.is_array()) kids = Json::array();
                const long long at2 = op.value("index", static_cast<long long>(kids.size()));
                const std::size_t pos = static_cast<std::size_t>(std::clamp<long long>(at2, 0, static_cast<long long>(kids.size())));
                kids.insert(kids.begin() + static_cast<std::ptrdiff_t>(pos), moving);
            }
            else if (kind == "rename")
            {
                if (!op.contains("name") || !op["name"].is_string()) return refuse("{\"name\": \"...\"}");
                (*node)["name"] = op["name"];
            }
            else if (kind == "order")
            {
                if (op.contains("order") && op["order"].is_number_integer()) (*node)["order"] = op["order"];
                else node->erase("order");
            }
            else
                return refuse("no such op (set, unset, remove_block, add, remove, move, rename, order)");
        }
        // Every op or none: the whole result must parse clean.
        const std::string edited = doc.dump();
        UI::UIAsset b;
        std::vector<UI::UIAssetIssue> issues;
        if (!UI::ParseUIAsset(edited, b, issues))
        {
            Fail(out, kRefused, "invalid_result", "the patched asset has " + std::to_string(issues.size()) + " issue(s)");
            out["issues"] = IssuesJson(issues);
            return kRefused;
        }
        const std::string canon = UI::WriteUIAsset(b);
        const std::string dest  = outPath.empty() ? path : outPath;
        out["ok"]      = true;
        out["applied"] = list.size();
        out["changed"] = canon != UI::WriteUIAsset(a);
        out["path"]    = dest;
        out["written"] = false;
        if (dryRun)
        {
            out["text"] = canon;
            return kDone;
        }
        if (!WriteText(dest, canon)) return Fail(out, kUsage, "io", "cannot write " + dest);
        out["written"] = true;
        return kDone;
    }

    // ==== layout ====
    int Layout(const std::string& path, const std::string& resList, Json& out)
    {
        UI::UIAsset a;
        std::string text;
        int code = kDone;
        if (!Load(path, a, text, out, code)) return code;
        World world;
        UI::UISpawnOptions o;
        o.assetKey = Sys::RelativizeAssetPath(path);
        o.seed     = ECS::Guid::FromHex("0000000000000001000000000000a5e7");
        std::string why;
        std::vector<std::pair<std::string, Entity>> ids;
        const Entity root = UI::SpawnUIAsset(world, a, o, &why, &ids);
        if (root == NullEntity) return Fail(out, kRefused, "spawn_failed", why);
        std::unordered_map<Entity, std::string> idOf;
        for (const auto& [id, e] : ids) idOf[e] = id;

        Json screens = Json::array();
        int problems = 0;
        std::stringstream ss(resList.empty() ? std::string("1280x720,1920x1080,3840x2160") : resList);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            unsigned w = 0, h = 0;
            if (sscanf_s(item.c_str(), "%ux%u", &w, &h) != 2 || w < 16 || h < 16 || w > 16384 || h > 16384)
                return Fail(out, kUsage, "usage", "-ui_res=<w>x<h>[,...]: '" + item + "'");
            UIView::Desc d;
            d.subject    = root;
            d.worldEpoch = world.Epoch();
            d.designSize = UI::Vec2{ static_cast<float>(w), static_cast<float>(h) };
            std::vector<UI::LayoutNode> nodes;
            float scale = 1.f;
            if (!UIViewService::LayoutDesc(d, world, nodes, scale, why)) return Fail(out, kRefused, "layout_failed", why);
            std::unordered_map<Entity, UI::Rect> rectOf;
            for (const UI::LayoutNode& n : nodes) rectOf[n.e] = n.cr.rect;
            Json rows = Json::array(), issues = Json::array();
            for (const UI::LayoutNode& n : nodes)
            {
                const UI::Rect& r = n.cr.rect;
                const std::string& id = idOf[n.e];
                rows.push_back(Json{ { "id", id }, { "x", r.mn.x }, { "y", r.mn.y }, { "w", r.mx.x - r.mn.x },
                                     { "h", r.mx.y - r.mn.y }, { "culled", n.cr.culled }, { "hidden", n.hidden },
                                     { "depth", n.cr.depth } });
                if (n.hidden) continue;
                auto issue = [&](const char* kind, const std::string& msg) {
                    issues.push_back(Json{ { "node", id }, { "kind", kind }, { "message", msg } });
                    ++problems;
                };
                if (r.mx.x - r.mn.x <= 0.f || r.mx.y - r.mn.y <= 0.f) issue("size", "resolves to nothing");
                else if (r.mx.x <= 0.f || r.mx.y <= 0.f || r.mn.x >= static_cast<float>(w) || r.mn.y >= static_cast<float>(h))
                    issue("offscreen", "entirely off the screen");
                if (n.e != root)
                    if (const Entity p = UI::ResolveUIParent(world, n.e); p != NullEntity && rectOf.count(p))
                    {
                        const UI::Rect& pr = rectOf[p];
                        const float eps = 0.5f;
                        if (r.mn.x < pr.mn.x - eps || r.mn.y < pr.mn.y - eps || r.mx.x > pr.mx.x + eps || r.mx.y > pr.mx.y + eps)
                            issues.push_back(Json{ { "node", id }, { "kind", "overflow" },
                                                   { "message", "reaches outside its parent '" + idOf[p] + "'" } });
                    }
            }
            screens.push_back(Json{ { "w", w }, { "h", h }, { "scale", scale }, { "nodes", rows }, { "issues", issues } });
        }
        out["ok"]       = true;
        out["path"]     = path;
        out["screens"]  = screens;
        out["problems"] = problems;   // size / offscreen; overflow is a note, not a problem
        return kDone;
    }

    // ==== convert ====
    // A UI .ipfb as a .iui: the prefab's text block (after its binary header) read node by node; the
    // hierarchy from CvParent's GUID (else the N line's parent index), sibling order from CvRect's
    // sibling; every Cv* line becomes its block as it is.
    int Convert(const std::string& path, const std::string& outPath, Json& out)
    {
        std::string bytes;
        if (path.empty()) return Fail(out, kUsage, "usage", "-asset=<x.ipfb> is required");
        if (!ReadText(path, bytes)) return Fail(out, kUsage, "io", "cannot read " + path);
        const std::size_t start = bytes.find("\nN idx=");
        if (start == std::string::npos) return Fail(out, kRefused, "not_a_prefab", path + " has no node text");
        std::string textBlock = bytes.substr(start + 1);
        if (const std::size_t nul = textBlock.find('\0'); nul != std::string::npos) textBlock.resize(nul);

        struct PNode
        {
            int idx = -1, parentIdx = -1;
            std::string name, guid, parentGuid;
            int sibling = 0;
            std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> lines;   // tag, kv
        };
        std::vector<PNode> nodes;
        std::istringstream in(textBlock);
        std::string line;
        auto pairsOf = [](const std::string& s, std::size_t from) {
            std::vector<std::pair<std::string, std::string>> kv;
            std::istringstream tok(s.substr(from));
            std::string t;
            while (tok >> t)
                if (const auto eq = t.find('='); eq != std::string::npos) kv.emplace_back(t.substr(0, eq), t.substr(eq + 1));
            return kv;
        };
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("N ", 0) == 0)
            {
                PNode n;
                for (const auto& [k, v] : pairsOf(line, 2))
                {
                    if (k == "idx") n.idx = std::atoi(v.c_str());
                    else if (k == "parent") n.parentIdx = std::atoi(v.c_str());
                    else if (k == "name") n.name = Resource::AssetRefs::PercentDecode(v);
                }
                nodes.push_back(std::move(n));
            }
            else if (line.rfind("  ", 0) == 0 && !nodes.empty())
            {
                const std::size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                const std::string tag = line.substr(2, colon - 2);
                auto kv = pairsOf(line, colon + 1);
                PNode& n = nodes.back();
                if (tag == "Guid")
                {
                    for (const auto& [k, v] : kv) if (k == "hex") n.guid = v;
                }
                else if (tag == "CvParent")
                {
                    for (const auto& [k, v] : kv) if (k == "parentGuid") n.parentGuid = v;
                }
                else if (tag.rfind("Cv", 0) == 0)
                {
                    if (tag == "CvRect")
                        for (const auto& [k, v] : kv) if (k == "sibling") n.sibling = std::atoi(v.c_str());
                    n.lines.emplace_back(tag, std::move(kv));
                }
            }
        }
        if (nodes.empty()) return Fail(out, kRefused, "not_a_prefab", path + " has no nodes");
        bool uiRoot = false;
        for (const auto& [tag, kv] : nodes[0].lines) uiRoot |= tag == "CvRect" || tag == "CvCanvas";
        if (!uiRoot) return Fail(out, kRefused, "not_ui", path + ": its root has no CvRect or CvCanvas - not a UI prefab");

        // Parent of each node: by GUID, else by index.
        std::map<std::string, int> byGuid;
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (!nodes[i].guid.empty()) byGuid[nodes[i].guid] = static_cast<int>(i);
        std::vector<std::vector<int>> kids(nodes.size());
        for (std::size_t i = 1; i < nodes.size(); ++i)
        {
            int p = -1;
            if (!nodes[i].parentGuid.empty())
                if (const auto it = byGuid.find(nodes[i].parentGuid); it != byGuid.end()) p = it->second;
            if (p < 0)
                for (std::size_t j = 0; j < nodes.size(); ++j)
                    if (nodes[j].idx == nodes[i].parentIdx) p = static_cast<int>(j);
            if (p < 0 || p == static_cast<int>(i))
                return Fail(out, kRefused, "orphan", "node '" + nodes[i].name + "' names no parent in the prefab");
            kids[static_cast<std::size_t>(p)].push_back(static_cast<int>(i));
        }
        std::unordered_map<std::string, int> used;
        std::function<void(int, UI::UIAssetNode&, int, bool)> build = [&](int i, UI::UIAssetNode& n, int index, bool isRoot) {
            const PNode& p = nodes[static_cast<std::size_t>(i)];
            const std::string base = UI::UIAssetIdFromName(p.name);
            const int k = used[base]++;
            n.id   = k == 0 ? base : base + "_" + std::to_string(k + 1);
            n.name = p.name;
            for (const auto& [tag, kv] : p.lines)
            {
                UI::UIAssetBlock b;
                b.tag = tag;
                for (const auto& [key, v] : kv)
                    if (!(tag == "CvRect" && key == "sibling")) b.kv.emplace_back(key, v);
                n.blocks.push_back(std::move(b));
            }
            if (!isRoot && p.sibling != index)
            {
                n.hasOrder = true;
                n.order    = p.sibling;
            }
            std::vector<int> order = kids[static_cast<std::size_t>(i)];
            std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
                return nodes[static_cast<std::size_t>(x)].sibling < nodes[static_cast<std::size_t>(y)].sibling;
            });
            for (std::size_t c = 0; c < order.size(); ++c)
            {
                n.children.emplace_back();
                build(order[c], n.children.back(), static_cast<int>(c), false);
            }
        };
        UI::UIAsset a;
        build(0, a.root, 0, true);
        // Canonical block order, and the codec's own judgement of the result.
        std::string canon = UI::WriteUIAsset(a);
        UI::UIAsset check;
        std::vector<UI::UIAssetIssue> issues;
        if (!UI::ParseUIAsset(canon, check, issues))
        {
            Fail(out, kRefused, "invalid_result", "the converted asset has " + std::to_string(issues.size()) + " issue(s)");
            out["issues"] = IssuesJson(issues);
            return kRefused;
        }
        canon = UI::WriteUIAsset(check);
        std::string dest = outPath;
        if (dest.empty())
        {
            dest = path;
            if (const std::size_t dot = dest.find_last_of('.'); dot != std::string::npos) dest.resize(dot);
            dest += ".iui";
        }
        if (!WriteText(dest, canon)) return Fail(out, kUsage, "io", "cannot write " + dest);
        out["ok"]    = true;
        out["from"]  = path;
        out["path"]  = dest;
        out["nodes"] = check.NodeCount();
        return kDone;
    }
}

int RunUIAssetTool(const std::string& command, Json& out)
{
    const auto& cl = Sys::CommandLine::Get();
    const std::string asset = cl.Get("-asset");
    out = Json::object();
    int code = kUsage;
    if      (command == "ui_inspect")  code = Inspect(asset, out);
    else if (command == "ui_validate") code = Validate(asset, cl.Get("-assettool_refs", "1") != "0", out);
    else if (command == "ui_fmt")      code = Fmt(asset, cl.Has("-ui_check"), out);
    else if (command == "ui_patch")    code = Patch(asset, cl.Get("-ops"), cl.Get("-out"), cl.Has("-dry_run"), out);
    else if (command == "ui_layout")   code = Layout(asset, cl.Get("-ui_res"), out);
    else if (command == "ui_convert")  code = Convert(asset, cl.Get("-out"), out);
    else code = Fail(out, kUsage, "usage", "-assettool=ui_inspect|ui_validate|ui_fmt|ui_patch|ui_layout|ui_convert");
    LOG_INFO("[ASSETTOOL] %s %s -> %s", command.c_str(), asset.c_str(), out.value("ok", false) ? "ok" : "refused");
    return code;
}
} // namespace Harness

#endif // WITH_HARNESS
