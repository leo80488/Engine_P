#include "Editor/AssetReferenceScanner.h"
#include "MaterialGraph/MaterialGraph.h"  // a graph rewritten in its canonical form
#include "UI/UIAsset.h"                  // a .iui rewritten in its canonical form
#include "Scene/SceneInstanceLoader.h"   // .iscn: the loader's own reference parse

#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetGuid.h"   // identities the backfill stamps references with
#include "Resource/AssetRefs.h"   // the definition of "this value names an asset"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace Editor
{
namespace
{
    // Containers the scanner parses and the magic each must carry: .iscene is MAGIC_WORLD 'RWLD', not
    // MAGIC_SCENE, which is the .iscn model format; magic 0 = a JSON asset.
    // textLenOff is the offset of the textLength field inside the metadata block. These formats store
    // the payload length twice (AssetHeader::dataSize and here), and a rewrite that changes the text
    // length must patch both or the loader reads a truncated or over-long payload. WorldMeta and
    // PrefabMetadata lead with nodeCount (+4), MaterialMetadata with textLength (0); SceneMetadata is
    // {nodeCount, meshFileCount, textLength, reserved} (+8) and is read with the loader's own grammar
    // (iscnRules), since its F / K / M paths are relative to the file's folder.
    struct ContainerKind { const char* ext; uint32_t magic; bool imatRules; uint32_t textLenOff; bool iscnRules = false; };
    const ContainerKind kContainers[] = {
        { ".ianimgraph", 0, false, 0 },
        { ".ianimset", 0, false, 0 },
        { ".attack.json", 0, false, 0 },
        { ".attackset.json", 0, false, 0 },
        // Material graphs and functions (JSON): the textures and functions they name.
        { ".imatgraph", 0, false, 0 },
        { ".imatfunc", 0, false, 0 },
        // A UI asset (JSON): its images' textures, its texts' fonts and shaders.
        { ".iui", 0, false, 0 },
        { ".iscene",   Resource::MAGIC_WORLD,    false, 4 },
        { ".ipfb",     Resource::MAGIC_PREFAB,   false, 4 },
        { ".imat",     Resource::MAGIC_MATERIAL, true,  0 },
        { ".imatinst", Resource::MAGIC_MATERIAL, true,  0 },
        { ".iscn",     Resource::MAGIC_SCENE,    false, 8, true },
    };

    // The reference vocabulary lives in Resource::AssetRefs and is shared with
    // the runtime resolver. Two copies would drift, and the failure is silent:
    // the editor would index (or repoint) values the loader never reads, or
    // miss ones it does.
    using Resource::AssetRefs::ToLowerAscii;
    using Resource::AssetRefs::PercentDecode;
    using Resource::AssetRefs::PercentEncode;
    using Resource::AssetRefs::PercentEncodeStrict;
    using Resource::AssetRefs::LooksLikeAssetPath;

    // Container kind of @p lowerPath by extension, or null; pass an already lowercased path.
    const ContainerKind* FindKind(const std::string& lowerPath)
    {
        for (const ContainerKind& k : kContainers)
        {
            const size_t elen = std::strlen(k.ext);
            if (lowerPath.size() >= elen
                && lowerPath.compare(lowerPath.size() - elen, elen, k.ext) == 0)
                return &k;
        }
        return nullptr;
    }

    // ==== Line scanners ====

    // .iscene / .ipfb component-line format: `  Tag: k=v k=v ...`, whitespace-separated tokens,
    // percent-encoded values (space -> %20). Also covers the top-level `W name=... navMesh=...` line.
    void ScanSceneLine(std::string_view line, const std::string& tagLabel,
                       std::vector<std::pair<std::string, std::string>>& out,
                       int inlineDepth);

    // .imat text: `key = value` with spaces around '='. tex_* values are the
    // whole tail; ctex_* values are "<uvset> <path>" (path = second token).
    void ScanImatText(std::string_view text,
                      std::vector<std::pair<std::string, std::string>>& out,
                      const std::string& keyPrefix)
    {
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t eol = text.find('\n', pos);
            if (eol == std::string_view::npos) eol = text.size();
            std::string_view line = text.substr(pos, eol - pos);
            pos = eol + 1;

            const size_t eq = line.find('=');
            if (eq == std::string_view::npos) continue;
            std::string key(line.substr(0, eq));
            std::string val(line.substr(eq + 1));
            const auto trim = [](std::string& s) {
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
            };
            trim(key);
            trim(val);
            if (val.empty()) continue;   // empty tex_ slots are normal

            // ctex_<Name> = <uvset> <path>: the path is the second token.
            if (key.rfind("ctex_", 0) == 0)
            {
                const size_t sp = val.find(' ');
                if (sp != std::string_view::npos) val = val.substr(sp + 1);
            }

            const std::string lower = ToLowerAscii(val);
            if (LooksLikeAssetPath(lower))
                out.emplace_back(AssetReferenceScanner::NormalizePath(val),
                                 keyPrefix + key);
        }
    }

    void ScanSceneLine(std::string_view line, const std::string& tagLabel,
                       std::vector<std::pair<std::string, std::string>>& out,
                       int inlineDepth)
    {
        // Walk whitespace-separated tokens: any k=v with a path-looking v is a reference. MaterialOverride
        // values carry a type prefix; only `t:` is a texture path.
        size_t pos = 0;
        while (pos < line.size())
        {
            while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
            size_t end = pos;
            while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) ++end;
            if (end <= pos) break;
            std::string_view token = line.substr(pos, end - pos);
            pos = end;

            const size_t eq = token.find('=');
            if (eq == std::string_view::npos || eq == 0) continue;
            std::string key(token.substr(0, eq));
            std::string val = PercentDecode(token.substr(eq + 1));
            if (val.empty()) continue;

            // MaterialOverride type prefixes: f:/v:/i:/b: are scalars, t: is
            // a texture path.
            if (val.size() > 2 && val[1] == ':' &&
                (val[0] == 'f' || val[0] == 'v' || val[0] == 'i' || val[0] == 'b'))
                continue;
            if (val.size() > 2 && val[0] == 't' && val[1] == ':')
                val = val.substr(2);

            // MaterialInline embeds a whole percent-encoded .imat text: scan the decoded text with the .imat
            // rules, or its texture / shader references vanish from the index. Depth-guarded (the format nests
            // one level).
            if (key == "data" && tagLabel == "MaterialInline" && inlineDepth == 0)
            {
                ScanImatText(val, out, "MaterialInline/");
                continue;
            }

            const std::string lower = ToLowerAscii(val);
            if (LooksLikeAssetPath(lower))
                out.emplace_back(AssetReferenceScanner::NormalizePath(val),
                                 tagLabel.empty() ? key : tagLabel + "." + key);
        }
    }

    // ==== Rewriters ====
    // Mirror images of the scanners: same tokenisation and special cases, emitting a rewritten copy.
    // Keep them in lockstep: a value the scanner indexes that a rewriter cannot find surfaces as a
    // `failed` row, never as a silent no-op. Unmatched bytes are copied through, so a rewrite that hits
    // nothing changes nothing.

    // normalized old path -> the value to write in its place. A MAP rather
    // than one pair because moving a folder renames every asset inside it at
    // once, and doing that one asset at a time would rebuild the whole
    // reference index once per asset.
    using PathRemap = std::unordered_map<std::string, std::string>;

    // The replacement for `path`, or nullptr when it is not being remapped.
    const std::string* Remapped(const PathRemap& remap, const std::string& path)
    {
        const auto it = remap.find(AssetReferenceScanner::NormalizePath(path));
        return it == remap.end() ? nullptr : &it->second;
    }

    // Replace every value whose target appears in `remap`, inside a .imat TEXT
    // payload (`key = value`; values are NOT percent-encoded here).
    size_t RewriteImatText(std::string& text, const PathRemap& remap)
    {
        std::string out;
        out.reserve(text.size() + 64);
        size_t hits = 0;
        size_t pos  = 0;

        while (pos < text.size())
        {
            const size_t eol     = text.find('\n', pos);
            const size_t lineEnd = (eol == std::string::npos) ? text.size() : eol;
            const std::string_view line(text.data() + pos, lineEnd - pos);

            bool rewritten = false;
            const size_t eq = line.find('=');
            if (eq != std::string_view::npos)
            {
                std::string key(line.substr(0, eq));
                while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) key.erase(key.begin());
                while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))  key.pop_back();

                const std::string_view rawVal = line.substr(eq + 1);
                size_t vb = 0, ve = rawVal.size();
                while (vb < ve && std::isspace(static_cast<unsigned char>(rawVal[vb])))     ++vb;
                while (ve > vb && std::isspace(static_cast<unsigned char>(rawVal[ve - 1]))) --ve;

                // Where the path starts inside rawVal: for ctex_ the second token (`ctex_<Name> = <uvset> <path>`).
                size_t ps = vb;
                if (ve > vb && key.rfind("ctex_", 0) == 0)
                {
                    const size_t sp = rawVal.find(' ', vb);
                    ps = (sp == std::string_view::npos || sp >= ve) ? std::string_view::npos : sp;
                    while (ps != std::string_view::npos && ps < ve && rawVal[ps] == ' ') ++ps;
                }

                if (ve > vb && ps != std::string_view::npos && ps < ve)
                {
                    const std::string pathVal(rawVal.substr(ps, ve - ps));
                    if (const std::string* newValue = Remapped(remap, pathVal))
                    {
                        out.append(line.substr(0, eq + 1));   // "key ="
                        out.append(rawVal.substr(0, ps));      // spacing (+ uvset)
                        out.append(*newValue);
                        out.append(rawVal.substr(ve));         // trailing spacing
                        rewritten = true;
                        ++hits;
                    }
                }
            }
            if (!rewritten) out.append(line);

            if (eol == std::string::npos) break;
            out.push_back('\n');
            pos = eol + 1;
        }

        text.swap(out);
        return hits;
    }

    // Token-level rewrite of one .iscene/.ipfb component-line body. Whitespace
    // and non-matching tokens are copied verbatim, so the only bytes that move
    // are the ones being repointed.
    void RewriteSceneLineBody(std::string_view body, const std::string& tagLabel,
                              const PathRemap& remap,
                              std::string& out, size_t& hits)
    {
        size_t pos = 0;
        while (pos < body.size())
        {
            const size_t wsStart = pos;
            while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
            out.append(body.substr(wsStart, pos - wsStart));
            if (pos >= body.size()) break;

            size_t end = pos;
            while (end < body.size() && !std::isspace(static_cast<unsigned char>(body[end]))) ++end;
            const std::string_view token = body.substr(pos, end - pos);
            pos = end;

            const size_t eq = token.find('=');
            if (eq == std::string_view::npos || eq == 0) { out.append(token); continue; }
            const std::string_view key = token.substr(0, eq);
            std::string val = PercentDecode(token.substr(eq + 1));
            if (val.empty()) { out.append(token); continue; }

            // MaterialInline carries a whole percent-encoded .imat text as one
            // token. Rewrite the DECODED text and re-encode: PercentDecode is
            // lossless, so re-encoding with the strict encoder round-trips to
            // the same decoded bytes even where the original escaped a
            // different character set.
            if (key == "data" && tagLabel == "MaterialInline")
            {
                const size_t sub = RewriteImatText(val, remap);
                if (sub == 0) { out.append(token); continue; }
                out.append(key);
                out.push_back('=');
                out.append(PercentEncodeStrict(val));
                hits += sub;
                continue;
            }

            // MaterialOverride type prefixes: f:/v:/i:/b: are scalars, t: is a
            // texture path.
            if (val.size() > 2 && val[1] == ':' &&
                (val[0] == 'f' || val[0] == 'v' || val[0] == 'i' || val[0] == 'b'))
            { out.append(token); continue; }
            const bool texPrefix = (val.size() > 2 && val[0] == 't' && val[1] == ':');
            const std::string pathVal = texPrefix ? val.substr(2) : val;

            const std::string* newValue = Remapped(remap, pathVal);
            if (!newValue) { out.append(token); continue; }

            out.append(key);
            out.push_back('=');
            if (texPrefix) out.append("t:");
            out.append(PercentEncode(*newValue));
            ++hits;
        }
    }

    bool IsGraphPath(const std::string& path)
    {
        const std::string l = ToLowerAscii(path);
        auto ends = [&](const char* e) { const size_t n = std::strlen(e); return l.size() >= n && l.compare(l.size() - n, n, e) == 0; };
        return ends(".imatgraph") || ends(".imatfunc");
    }

    // A material graph / function: its string props repointed and written back as SerializeGraph
    // writes (canonical; a generic JSON dump would not be).
    size_t RewriteGraphText(std::string& text, const PathRemap& remap)
    {
        MaterialGraph::Graph g;
        std::string err;
        if (!MaterialGraph::ParseGraph(text, g, err)) return 0;
        size_t hits = 0;
        for (MaterialGraph::Node& n : g.nodes)
            for (auto& [key, v] : n.props)
                if (v.kind == MaterialGraph::Value::Kind::String && LooksLikeAssetPath(ToLowerAscii(v.str)))
                    if (const std::string* r = Remapped(remap, v.str))
                    {
                        v.str = *r;
                        ++hits;
                    }
        if (hits) text = MaterialGraph::SerializeGraph(g);
        return hits;
    }

    bool IsUIAssetPath(const std::string& path)
    {
        const std::string l = ToLowerAscii(path);
        return l.size() >= 4 && l.compare(l.size() - 4, 4, ".iui") == 0;
    }

    // A .iui: its texture / font / text-shader paths repointed and written back canonical
    // (UI::WriteUIAsset); a generic JSON dump would reorder every key and reprint every number.
    size_t RewriteUIAssetText(std::string& text, const PathRemap& remap)
    {
        UI::UIAsset a;
        std::vector<UI::UIAssetIssue> issues;
        if (!UI::ParseUIAsset(text, a, issues)) return 0;
        size_t hits = 0;
        std::function<void(UI::UIAssetNode&)> walk = [&](UI::UIAssetNode& n) {
            for (UI::UIAssetBlock& b : n.blocks)
                for (auto& [key, value] : b.kv)
                    if ((b.tag == "CvImage" && key == "tex") ||
                        (b.tag == "CvText" && (key == "font" || key == "customShader")))
                        if (const std::string* r = Remapped(remap, PercentDecode(value)))
                        {
                            value = PercentEncode(*r);
                            ++hits;
                        }
            for (UI::UIAssetNode& c : n.children) walk(c);
        };
        walk(a.root);
        if (hits) text = UI::WriteUIAsset(a);
        return hits;
    }

    size_t RewriteSceneText(std::string& text, const PathRemap& remap)
    {
        if(text.find_first_not_of(" \r\n\t")!=std::string::npos&&text[text.find_first_not_of(" \r\n\t")]=='{') {
            auto doc=nlohmann::json::parse(text,nullptr,false);
            if(doc.is_discarded())return 0;
            size_t hits=0;
            std::function<void(nlohmann::json&)> visit=[&](nlohmann::json& node){
                if(node.is_string()) {
                    auto value=node.get<std::string>();
                    if(LooksLikeAssetPath(ToLowerAscii(value)))if(const auto* replacement=Remapped(remap,value)){node=*replacement;++hits;}
                }else if(node.is_structured())for(auto& child:node)visit(child);
            };
            visit(doc);if(hits)text=doc.dump(2)+'\n';return hits;
        }
        std::string out;
        out.reserve(text.size() + 64);
        size_t hits = 0;
        size_t pos  = 0;

        while (pos < text.size())
        {
            const size_t eol     = text.find('\n', pos);
            const size_t lineEnd = (eol == std::string::npos) ? text.size() : eol;
            const std::string_view line(text.data() + pos, lineEnd - pos);

            // Same shape test as ParseContainer: `  Tag: k=v ...`.
            std::string tag;
            size_t bodyStart = 0;
            bool   skip      = line.empty() || line[0] == '#';
            if (!skip && line.size() > 2 && line[0] == ' ' && line[1] == ' ')
            {
                const size_t colon = line.find(':', 2);
                if (colon != std::string_view::npos)
                {
                    tag       = std::string(line.substr(2, colon - 2));
                    bodyStart = colon + 1;
                    if (tag == "EdFolder") skip = true;   // virtual folders, not assets
                }
            }

            if (skip)
                out.append(line);
            else
            {
                out.append(line.substr(0, bodyStart));
                RewriteSceneLineBody(line.substr(bodyStart), tag, remap, out, hits);
            }

            if (eol == std::string::npos) break;
            out.push_back('\n');
            pos = eol + 1;
        }

        text.swap(out);
        return hits;
    }

    // ==== .iscn models ====
    // @p target (project-relative) as the model at @p containerPath spells it: relative to the model's
    // folder, '/' separated. Both made absolute first, so project-relative and absolute spellings meet.
    std::string RelativeToContainer(const std::string& target, const std::string& containerPath)
    {
        std::error_code ec;
        const fs::path dir = fs::absolute(Sys::U8Path(containerPath), ec).parent_path().lexically_normal();
        const fs::path abs = fs::absolute(Sys::U8Path(target), ec).lexically_normal();
        std::string rel = Sys::PathToUtf8(abs.lexically_relative(dir));
        for (char& c : rel) if (c == '\\') c = '/';
        return rel.empty() ? target : rel;
    }

    // A model's references, read by the loader's own ListFileReferences
    // resolved against @p resolveFrom (where the file was when its paths were
    // written - its OLD path when it just moved) and written back relative to
    // @p writeFor. The grammar is positional: an F line's `file=` runs to
    // ` mesh=`, while `mat=` and a K / M line's `file=` run to the end of the
    // line (a file name may hold spaces; `name=` between them is percent-
    // encoded, so ` mat=` cannot occur inside it). @p rebaseAll rewrites every
    // reference (a moved model), else only the remapped ones. Returns how many
    // values changed.
    size_t RewriteIscnText(std::string& text, const std::string& resolveFrom, const std::string& writeFor,
                           const PathRemap& remap, bool rebaseAll)
    {
        const std::vector<SceneInstanceLoader::FileReference> refs =
            SceneInstanceLoader::ListFileReferences(text, resolveFrom);
        if (refs.empty()) return 0;
        std::vector<std::string> lines;
        for (size_t pos = 0;;)
        {
            const size_t eol = text.find('\n', pos);
            lines.push_back(text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
        size_t hits = 0;
        for (const SceneInstanceLoader::FileReference& fr : refs)
        {
            if (fr.line == 0 || fr.line > lines.size()) continue;
            const std::string* moved = Remapped(remap, fr.path);
            if (!moved && !rebaseAll) continue;
            std::string& line = lines[fr.line - 1];
            const bool cr = !line.empty() && line.back() == '\r';
            if (cr) line.pop_back();
            const std::string marker = " " + fr.key + "=";
            const size_t at = line.find(marker);
            if (at != std::string::npos)
            {
                const size_t vbeg = at + marker.size();
                size_t vend = line.size();
                if (fr.kind == 'F' && fr.key == "file")
                    if (const size_t m = line.find(" mesh=", vbeg); m != std::string::npos) vend = m;
                // A collision library (C / V / PR `lib=`) is one token.
                const bool token = fr.kind == 'C' || fr.kind == 'V' || fr.kind == 'P';
                if (token)
                    if (const size_t sp = line.find(' ', vbeg); sp != std::string::npos) vend = sp;
                std::string value = RelativeToContainer(moved ? *moved : fr.path, writeFor);
                if (fr.kind == 'P') value = Resource::AssetRefs::PercentEncode(value);   // PR values are encoded
                if (line.compare(vbeg, vend - vbeg, value) != 0)
                {
                    line.replace(vbeg, vend - vbeg, value);
                    ++hits;
                }
            }
            if (cr) line.push_back('\r');
        }
        if (!hits) return 0;
        std::string out;
        out.reserve(text.size() + 64);
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (i) out.push_back('\n');
            out.append(lines[i]);
        }
        text.swap(out);
        return hits;
    }

    bool IsIscnPath(const std::string& p)
    {
        const std::string l = ToLowerAscii(p);
        return l.size() >= 5 && l.compare(l.size() - 5, 5, ".iscn") == 0;
    }
} // namespace

std::string AssetReferenceScanner::NormalizePath(const std::string& raw)
{
    return Resource::AssetRefs::NormalizeRefPath(raw);
}

void AssetReferenceScanner::ParseContainer(const std::string& relPath, FileEntry& entry)
{
    entry.refs.clear();

    const ContainerKind* kind = FindKind(ToLowerAscii(relPath));
    if (!kind) return;

    std::vector<uint8_t> blob;
    // AssetFS, not ifstream: same read path the loaders use (pak-aware).
    if (!Resource::AssetFS::Get().ReadFile(relPath, blob)) return;
    if(kind->magic==0) {
        auto doc=nlohmann::json::parse(blob.begin(),blob.end(),nullptr,false);
        if(doc.is_discarded())return;
        std::function<void(const nlohmann::json&,const std::string&)> visit;
        visit=[&](const nlohmann::json& node,const std::string& key){
            if(node.is_string()) {
                auto value=node.get<std::string>();
                if(LooksLikeAssetPath(ToLowerAscii(value)))entry.refs.emplace_back(NormalizePath(value),key);
            }else if(node.is_structured())for(auto it=node.begin();it!=node.end();++it)visit(it.value(),key+"/"+(node.is_object()?it.key():std::to_string(it-node.begin())));
        };
        visit(doc,"");return;
    }
    if (blob.size() < sizeof(Resource::AssetHeader)) return;

    const Resource::AssetHeader* h = Resource::GetHeader(blob.data());
    if (h->magic != kind->magic) return;   // lenient like LoadScene: magic only
    const size_t payloadOff = sizeof(Resource::AssetHeader) + h->metadataSize;
    if (payloadOff >= blob.size()) return;
    size_t len = std::min<size_t>(h->dataSize, blob.size() - payloadOff);
    const char* text = reinterpret_cast<const char*>(blob.data() + payloadOff);
    while (len > 0 && text[len - 1] == '\0') --len;   // dataSize includes the NUL

    const std::string_view payload(text, len);

    if (kind->imatRules)
    {
        ScanImatText(payload, entry.refs, "");
        return;
    }
    if (kind->iscnRules)
    {
        // The loader's own parse, resolved the way it resolves them. Keyed
        // "<line kind>.<key>": F.file (a .meshlib), F.mat, K.file, M.file.
        for (const SceneInstanceLoader::FileReference& fr :
                 SceneInstanceLoader::ListFileReferences(std::string(payload), relPath))
            entry.refs.emplace_back(NormalizePath(fr.path), std::string(1, fr.kind) + "." + fr.key);
        return;
    }

    // Line-oriented .iscene/.ipfb: derive the tag from `  Tag: ...` lines so
    // referrer rows say WHICH component held the reference.
    size_t pos = 0;
    while (pos < payload.size())
    {
        size_t eol = payload.find('\n', pos);
        if (eol == std::string_view::npos) eol = payload.size();
        std::string_view line = payload.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line[0] == '#') continue;

        std::string tag;
        std::string_view body = line;
        // `  Tag: k=v ...`: a two-space indent marks a component line.
        if (line.size() > 2 && line[0] == ' ' && line[1] == ' ')
        {
            const size_t colon = line.find(':', 2);
            if (colon != std::string_view::npos)
            {
                tag  = std::string(line.substr(2, colon - 2));
                body = line.substr(colon + 1);
                if (tag == "EdFolder") continue;   // virtual folders, not assets
            }
        }
        ScanSceneLine(body, tag, entry.refs, 0);
    }
}

void AssetReferenceScanner::Rebuild(const std::string& assetRoot)
{
    const auto t0 = std::chrono::steady_clock::now();

    // Pass 1: enumerate. Walked here because ScanAssets is single-directory and skips .iscene.
    std::unordered_map<std::string, std::pair<int64_t, uint64_t>> onDisk;
    std::error_code ec;
    const std::string root = assetRoot.empty() ? std::string("asset") : assetRoot;
    m_assetRoot = root;

    // Every directory this rebuild indexes: the root plus the SetExtraRoots engine content.
    std::vector<std::string> roots{ root };
    for (const std::string& extra : m_extraRoots)
        if (!extra.empty()
            && std::find(roots.begin(), roots.end(), extra) == roots.end())
            roots.push_back(extra);
    for (const std::string& scanRoot : roots)
    for (fs::recursive_directory_iterator
             it(Sys::U8Path(scanRoot), fs::directory_options::skip_permission_denied, ec), endIt;
         it != endIt; it.increment(ec))
    {
        if (ec) { ec.clear(); break; }
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = Sys::PathToUtf8(it->path());
        if (!FindKind(ToLowerAscii(rel))) continue;

        const auto     mt   = fs::last_write_time(it->path(), ec);
        const int64_t  mtT  = ec ? 0 : mt.time_since_epoch().count();
        const uint64_t size = it->file_size(ec);
        const std::string norm = NormalizePath(rel);
        onDisk.emplace(norm, std::make_pair(mtT, size));

        // The cache key is normalized (lowercased); keep the enumerated spelling too, since the rewrites
        // have to open these files and the key is only good for lookups.
        FileEntry& fe = m_files[norm];
        fe.diskPath = rel;
        if (fe.mtime != mtT || fe.size != size)
        {
            fe.mtime = mtT;
            fe.size  = size;
            ParseContainer(rel, fe);
        }
    }

    // Drop rows for deleted files.
    for (auto it = m_files.begin(); it != m_files.end(); )
    {
        if (onDisk.find(it->first) == onDisk.end()) it = m_files.erase(it);
        else                                        ++it;
    }

    // Pass 2: invert.
    m_index.clear();
    m_edges = 0;
    for (const auto& [file, fe] : m_files)
        for (const auto& [refPath, key] : fe.refs)
        {
            m_index[refPath].push_back(Referrer{ file, key });
            ++m_edges;
        }

    m_built = true;
    m_lastScanMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("AssetReferenceScanner: %zu containers, %zu reference edges, "
             "%zu distinct targets (%.0f ms)",
             m_files.size(), m_edges, m_index.size(), m_lastScanMs);
}

std::vector<AssetReferenceScanner::Referrer>
AssetReferenceScanner::FindReferrers(const std::string& assetPath) const
{
    const auto it = m_index.find(NormalizePath(assetPath));
    return it != m_index.end() ? it->second : std::vector<Referrer>{};
}

bool AssetReferenceScanner::TransformContainer(
        const std::string& diskPath,
        const std::function<size_t(std::string&, const TextKind&)>& transform,
        size_t& outHits)
{
    outHits = 0;

    const ContainerKind* kind = FindKind(ToLowerAscii(diskPath));
    if (!kind) return false;

    // Sys::ReadFileBytes, not AssetFS: a pak-only entry has no writable home, so this must fail loudly
    // instead of "succeeding" against a copy the next cook overwrites.
    std::vector<uint8_t> blob;
    if (!Sys::ReadFileBytes(diskPath, blob))            return false;
    if(kind->magic==0) {
        std::string text(blob.begin(),blob.end());
        size_t hits=transform(text,TextKind{false,false,diskPath});if(!hits)return true;
        if(nlohmann::json::parse(text,nullptr,false).is_discarded())return false;
        const std::string tmp=diskPath+".tmp";
        std::vector<uint8_t> data(text.begin(),text.end());
        if(!Sys::WriteFileBytes(tmp,data))return false;
        if(!MoveFileExW(Sys::U8Path(tmp).c_str(),Sys::U8Path(diskPath).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return false;
        outHits=hits;return true;
    }
    if (blob.size() < sizeof(Resource::AssetHeader))    return false;

    Resource::AssetHeader hdr;
    std::memcpy(&hdr, blob.data(), sizeof(hdr));
    if (hdr.magic != kind->magic)                       return false;

    const size_t payloadOff = sizeof(Resource::AssetHeader) + hdr.metadataSize;
    if (payloadOff >= blob.size())                      return false;

    const size_t payloadLen = std::min<size_t>(hdr.dataSize, blob.size() - payloadOff);
    const char*  payload    = reinterpret_cast<const char*>(blob.data() + payloadOff);

    // dataSize counts the NUL terminator; preserve however many the writer put
    // there so the rewritten file keeps the same shape, not just the same text.
    size_t textLen = payloadLen;
    while (textLen > 0 && payload[textLen - 1] == '\0') --textLen;
    const size_t nulCount = payloadLen - textLen;

    std::string  text(payload, textLen);
    const size_t hits = transform(text, TextKind{ kind->imatRules, kind->iscnRules, diskPath });
    if (hits == 0) return true;   // read and parsed fine, nothing to change

    // Re-emit the whole container; both length fields move with the text.
    hdr.dataSize = static_cast<uint32_t>(text.size() + nulCount);

    std::vector<uint8_t> outBlob;
    outBlob.reserve(payloadOff + text.size() + nulCount + 8);
    const uint8_t* hdrBytes = reinterpret_cast<const uint8_t*>(&hdr);
    outBlob.insert(outBlob.end(), hdrBytes, hdrBytes + sizeof(hdr));
    outBlob.insert(outBlob.end(),
                   blob.begin() + static_cast<std::ptrdiff_t>(sizeof(Resource::AssetHeader)),
                   blob.begin() + static_cast<std::ptrdiff_t>(payloadOff));
    if (kind->textLenOff + sizeof(uint32_t) <= hdr.metadataSize)
    {
        const uint32_t tl = static_cast<uint32_t>(text.size());
        std::memcpy(outBlob.data() + sizeof(Resource::AssetHeader) + kind->textLenOff,
                    &tl, sizeof(tl));
    }
    outBlob.insert(outBlob.end(), text.begin(), text.end());
    outBlob.insert(outBlob.end(), nulCount, uint8_t{ 0 });
    // Bytes past the declared payload (no current format has any) are carried over, not dropped.
    if (blob.size() > payloadOff + payloadLen)
        outBlob.insert(outBlob.end(),
                       blob.begin() + static_cast<std::ptrdiff_t>(payloadOff + payloadLen),
                       blob.end());

    // .tmp + rename: an interrupted write must not truncate a live asset.
    const std::string tmp = diskPath + ".tmp";
    if (!Sys::WriteFileBytes(tmp, outBlob)) return false;
    std::error_code ec;
    fs::rename(Sys::U8Path(tmp), Sys::U8Path(diskPath), ec);
    if (ec)
    {
        std::error_code rmEc;
        fs::remove(Sys::U8Path(tmp), rmEc);
        return false;
    }

    outHits = hits;
    return true;
}

bool AssetReferenceScanner::RewriteContainer(const std::string& diskPath,
                                             const std::string& oldNorm,
                                             const std::string& newValue,
                                             size_t& outHits)
{
    if (IsUIAssetPath(diskPath))   // canonical, not a generic JSON dump
    {
        PathRemap one;
        one.emplace(oldNorm, newValue);
        return TransformContainer(diskPath, [&](std::string& text, const TextKind&) {
            return RewriteUIAssetText(text, one);
        }, outHits);
    }
    if (const auto* kind = FindKind(ToLowerAscii(diskPath)); kind && kind->magic == 0) {
        return TransformContainer(diskPath, [&](std::string& text, const TextKind&) {
            auto document = nlohmann::json::parse(text);
            size_t hits = 0;
            std::function<void(nlohmann::json&)> rewrite;
            rewrite = [&](nlohmann::json& j) {
                if (j.is_string()) { if (NormalizePath(j.get<std::string>()) == oldNorm) { j = newValue; ++hits; } }
                else if (j.is_structured()) for (auto& value : j) rewrite(value);
            };
            rewrite(document);
            if (hits) text = document.dump(2) + "\n";
            return hits;
        }, outHits);
    }
    PathRemap one;
    one.emplace(oldNorm, newValue);
    return TransformContainer(diskPath,
        [&](std::string& text, const TextKind& k) {
            return k.iscnRules ? RewriteIscnText(text, k.diskPath, k.diskPath, one, false)
                 : k.imatRules ? RewriteImatText (text, one)
                 : IsGraphPath(k.diskPath) ? RewriteGraphText(text, one)
                               : RewriteSceneText(text, one);
        }, outHits);
}

AssetReferenceScanner::BackfillReport
AssetReferenceScanner::BackfillGuids(const std::string& assetRoot)
{
    BackfillReport rep;
    Rebuild(assetRoot);   // fresh list of containers, and their on-disk spellings

    // Stamping identities is a prerequisite: Augment*Text asks the index for
    // each target's guid, and a target without one contributes no token.
    Resource::AssetGuidIndex::Get().Rebuild(assetRoot, /*stampMissing=*/true);

    const auto countTokens = [](const std::string& s) {
        size_t n = 0, at = 0;
        while ((at = s.find(Resource::AssetRefs::kGuidPrefix, at)) != std::string::npos)
        { ++n; at += 3; }
        return n;
    };

    // Snapshot: the loop below rewrites files, and Rebuild's cache is keyed on
    // mtime+size, so hold the work list rather than iterating m_files live.
    std::vector<std::string> targets;
    targets.reserve(m_files.size());
    for (const auto& [norm, fe] : m_files)
        targets.push_back(fe.diskPath.empty() ? norm : fe.diskPath);

    for (const std::string& disk : targets)
    {
        if (const auto* kind = FindKind(ToLowerAscii(disk)); kind && kind->magic == 0) continue;
        ++rep.containersScanned;
        size_t added = 0;
        const bool ok = TransformContainer(disk,
            [&](std::string& text, const TextKind& k) -> size_t
            {
                // A model's references carry no guid token (its grammar is
                // positional; the loader would read one as part of the path).
                if (k.iscnRules) return 0;
                const std::string before = text;
                std::string after = k.imatRules
                    ? Resource::AssetRefs::AugmentImatText(before)
                    : Resource::AssetRefs::AugmentSceneText(before);
                if (after == before) return 0;          // already current: no write
                const size_t was = countTokens(before);
                const size_t now = countTokens(after);
                text = std::move(after);
                return now > was ? now - was : 1;       // 1 = changed but not by adding
            }, added);

        if (!ok)                { rep.failed.push_back(disk); continue; }
        if (added == 0)         continue;               // nothing to add
        ++rep.containersChanged;
        rep.referencesStamped += added;
    }

    m_built = false;   // every rewritten container's mtime moved
    LOG_SUCCESS("AssetReferenceScanner: guid backfill — %zu reference(s) stamped "
                "across %zu of %zu container(s), %zu failed",
                rep.referencesStamped, rep.containersChanged,
                rep.containersScanned, rep.failed.size());
    for (const std::string& f : rep.failed)
        LOG_WARNING("AssetReferenceScanner: backfill could not rewrite '%s'", f.c_str());
    return rep;
}

AssetReferenceScanner::RewriteReport
AssetReferenceScanner::RewriteRawPrefix(const std::string& assetRoot,
                                        const std::string& oldPrefix,
                                        const std::string& newPrefix)
{
    RewriteReport rep;
    if (oldPrefix.empty() || oldPrefix == newPrefix) return rep;

    Rebuild(assetRoot);   // fresh list of containers and their disk spellings

    // Snapshot: TransformContainer moves mtime+size, so iterating m_files live
    // would walk a map being invalidated underneath.
    std::vector<std::string> targets;
    targets.reserve(m_files.size());
    for (const auto& [norm, fe] : m_files)
        targets.push_back(fe.diskPath.empty() ? norm : fe.diskPath);

    for (const std::string& disk : targets)
    {
        size_t hits = 0;
        const bool ok = TransformContainer(disk,
            [&](std::string& text, const TextKind& k) -> size_t
            {
                // A model's paths are relative to its folder: a project-path
                // prefix never begins one, and substituting inside one would
                // corrupt it rather than repoint it.
                if (k.iscnRules) return 0;
                // Byte-level and deliberately encoding-agnostic: everything after the prefix is carried across
                // untouched, which is the point - a percent-encoded, backslash-separated tail is what the
                // normalizing path cannot handle.
                size_t n = 0, at = 0;
                std::string out;
                out.reserve(text.size());
                for (;;)
                {
                    const size_t f = text.find(oldPrefix, at);
                    if (f == std::string::npos) { out.append(text, at, std::string::npos); break; }
                    out.append(text, at, f - at);
                    out += newPrefix;
                    at = f + oldPrefix.size();
                    ++n;
                }
                if (n) text = std::move(out);
                return n;
            }, hits);

        if (!ok)      { rep.failed.push_back(disk); continue; }
        if (hits == 0) continue;
        ++rep.containersChanged;
        rep.referencesRewritten += hits;
    }

    m_built = false;   // every rewritten container's mtime moved
    LOG_SUCCESS("AssetReferenceScanner: raw prefix '%s' -> '%s' — %zu "
                "occurrence(s) in %zu of %zu container(s), %zu failed",
                oldPrefix.c_str(), newPrefix.c_str(), rep.referencesRewritten,
                rep.containersChanged, targets.size(), rep.failed.size());
    for (const std::string& f : rep.failed)
        LOG_WARNING("AssetReferenceScanner: prefix rewrite could not touch '%s'", f.c_str());
    return rep;
}

AssetReferenceScanner::RewriteReport
AssetReferenceScanner::RewriteReferences(const std::string& oldPath,
                                         const std::string& newPath)
{
    RewriteReport rep;
    if (!m_built) return rep;

    const std::string oldNorm = NormalizePath(oldPath);
    const std::string newNorm = NormalizePath(newPath);
    // Equal-after-normalization covers a pure CASE change, which needs no
    // rewrite: every referrer still resolves, and the index is case-folded
    // anyway so there is nothing to repoint.
    if (oldNorm.empty() || newNorm.empty() || oldNorm == newNorm) return rep;

    const auto it = m_index.find(oldNorm);
    if (it == m_index.end()) return rep;

    // The value written keeps the caller's case and is project-relative with forward slashes, the
    // shape AssetFS and a packed game.ipak look up. NormalizePath lowercases for comparison only.
    std::string newValue = Sys::RelativizeAssetPath(newPath);
    for (char& c : newValue) if (c == '\\') c = '/';

    // One index row per (file,key) edge; a container holding three references
    // to the same asset must still be opened exactly once.
    std::vector<std::string> targets;
    for (const Referrer& r : it->second)
        if (std::find(targets.begin(), targets.end(), r.file) == targets.end())
            targets.push_back(r.file);

    for (const std::string& normFile : targets)
    {
        const auto fit = m_files.find(normFile);
        const std::string disk = (fit != m_files.end() && !fit->second.diskPath.empty())
                                 ? fit->second.diskPath : normFile;
        size_t hits = 0;
        const bool ok = RewriteContainer(disk, oldNorm, newValue, hits);
        if (ok && hits > 0)
        {
            ++rep.containersChanged;
            rep.referencesRewritten += hits;
            LOG_INFO("AssetReferenceScanner: repointed %zu reference(s) in '%s'",
                     hits, disk.c_str());
        }
        else
        {
            rep.failed.push_back(disk);
            LOG_WARNING("AssetReferenceScanner: could NOT repoint '%s' in '%s' (%s)",
                        oldNorm.c_str(), disk.c_str(),
                        ok ? "indexed value no longer present"
                           : "read / parse / write failed");
        }
    }

    LOG_INFO("AssetReferenceScanner: '%s' -> '%s' — %zu reference(s) in %zu "
             "container(s), %zu failed",
             oldNorm.c_str(), newValue.c_str(), rep.referencesRewritten,
             rep.containersChanged, rep.failed.size());

    // Cached rows and mtimes are now out of step with disk: force the caller through Rebuild() before
    // the next query.
    m_built = false;
    return rep;
}

AssetReferenceScanner::RewriteReport
AssetReferenceScanner::RewriteReferencesBatch(
    const std::vector<std::pair<std::string, std::string>>& moves)
{
    RewriteReport rep;
    if (moves.empty()) return rep;

    // One rebuild for the whole batch. Looping RewriteReferences instead would
    // work exactly once: it marks the index stale on the way out, so every
    // later call returns having done nothing.
    Rebuild(m_assetRoot.empty() ? std::string("asset") : m_assetRoot);

    PathRemap remap;
    std::vector<std::string> targets;   // containers to rewrite, deduped
    for (const auto& [oldPath, newPath] : moves)
    {
        const std::string oldNorm = NormalizePath(oldPath);
        const std::string newNorm = NormalizePath(newPath);
        if (oldNorm.empty() || newNorm.empty() || oldNorm == newNorm) continue;

        std::string value = Sys::RelativizeAssetPath(newPath);
        for (char& c : value) if (c == '\\') c = '/';
        remap[oldNorm] = std::move(value);

        const auto it = m_index.find(oldNorm);
        if (it == m_index.end()) continue;
        for (const Referrer& r : it->second)
            if (std::find(targets.begin(), targets.end(), r.file) == targets.end())
                targets.push_back(r.file);
    }
    // A MODEL (.iscn) that moved in this batch names its files relative to the
    // folder it LEFT: Rebuild parsed it at the new one, so what the index says
    // it references is wrong until it is re-based below - skip it here.
    std::unordered_set<std::string> movedModels;
    for (const auto& [oldPath, newPath] : moves)
        if (IsIscnPath(newPath)) movedModels.insert(NormalizePath(newPath));
    if (remap.empty() || (targets.empty() && movedModels.empty())) { m_built = false; return rep; }

    for (const std::string& normFile : targets)
    {
        if (movedModels.count(normFile)) continue;
        const auto fit = m_files.find(normFile);
        const std::string disk = (fit != m_files.end() && !fit->second.diskPath.empty())
                                 ? fit->second.diskPath : normFile;
        std::size_t hits = 0;
        const bool ok = TransformContainer(disk,
            [&](std::string& text, const TextKind& k) {
                return k.iscnRules ? RewriteIscnText(text, k.diskPath, k.diskPath, remap, false)
                     : k.imatRules ? RewriteImatText (text, remap)
                     : IsGraphPath(k.diskPath) ? RewriteGraphText(text, remap)
                     : IsUIAssetPath(k.diskPath) ? RewriteUIAssetText(text, remap)
                                   : RewriteSceneText(text, remap);
            }, hits);

        if (ok && hits > 0)
        {
            ++rep.containersChanged;
            rep.referencesRewritten += hits;
        }
        else
        {
            rep.failed.push_back(disk);
        }
    }

    // Each moved model: every reference re-read against where it WAS, a target
    // that moved in the same batch taken to its new path, and all of them
    // written relative to where it IS. A model moved together with its files
    // (a folder move) comes out unchanged - zero hits is success here.
    for (const auto& [oldPath, newPath] : moves)
    {
        if (!IsIscnPath(newPath)) continue;
        std::size_t hits = 0;
        const std::string from = oldPath, to = newPath;
        const bool ok = TransformContainer(newPath,
            [&](std::string& text, const TextKind&) { return RewriteIscnText(text, from, to, remap, true); }, hits);
        if (!ok) rep.failed.push_back(newPath);
        else if (hits > 0)
        {
            ++rep.containersChanged;
            rep.referencesRewritten += hits;
            LOG_INFO("AssetReferenceScanner: re-based %zu reference(s) of the moved model '%s'",
                     hits, newPath.c_str());
        }
    }

    LOG_INFO("AssetReferenceScanner: batch repoint of %zu asset(s) — %zu "
             "reference(s) in %zu container(s), %zu failed",
             moves.size(), rep.referencesRewritten, rep.containersChanged,
             rep.failed.size());
    m_built = false;   // rewritten containers moved their mtimes
    return rep;
}

} // namespace Editor
