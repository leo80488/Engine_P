#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Resource/VrmImporter.h"
#include "Resource/SceneImporter.h"
#include "Resource/HumanoidNameTable.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace Resource
{
    namespace
    {
        // GLB container layout (glTF 2.0): a 12-byte header "glTF" (u32 LE) | version=2 (u32) |
        // length (u32), then chunks of chunkLength (u32) | chunkType (u32) | data. Chunk 0 must be
        // JSON (0x4E4F534A); chunk 1, optional, is BIN ("BIN\0" = 0x004E4942). Only the JSON chunk is
        // read here; Assimp consumes BIN.
        constexpr uint32_t kGlbMagic   = 0x46546C67; // "glTF"
        constexpr uint32_t kChunkJson  = 0x4E4F534A; // "JSON"

        // Returns a view into @p data of the JSON chunk; ptr == nullptr on failure, logged unless the
        // input is under 20 bytes.
        struct JsonSpan { const char* ptr; size_t len; };
        static JsonSpan ExtractGlbJsonChunk(const uint8_t* data, size_t size)
        {
            if (!data || size < 12 + 8) return { nullptr, 0 };

            uint32_t magic, version, length;
            std::memcpy(&magic,   data + 0, 4);
            std::memcpy(&version, data + 4, 4);
            std::memcpy(&length,  data + 8, 4);

            if (magic != kGlbMagic)
            {
                LOG_ERROR("VrmImporter: not a GLB file (magic mismatch)");
                return { nullptr, 0 };
            }
            if (version != 2)
            {
                LOG_ERROR("VrmImporter: unsupported GLB version %u (expected 2)", version);
                return { nullptr, 0 };
            }
            if (length > size) length = static_cast<uint32_t>(size); // be lenient

            // First chunk header at offset 12
            uint32_t chunkLen, chunkType;
            std::memcpy(&chunkLen,  data + 12, 4);
            std::memcpy(&chunkType, data + 16, 4);

            if (chunkType != kChunkJson)
            {
                LOG_ERROR("VrmImporter: first GLB chunk is not JSON (type=0x%08X)", chunkType);
                return { nullptr, 0 };
            }
            if (20ull + chunkLen > size)
            {
                LOG_ERROR("VrmImporter: GLB JSON chunk overruns file (len=%u, file=%zu)",
                          chunkLen, size);
                return { nullptr, 0 };
            }

            return { reinterpret_cast<const char*>(data + 20), chunkLen };
        }

        // Safe optional-string accessor.
        static std::string JStr(const nlohmann::json& obj, const char* key)
        {
            auto it = obj.find(key);
            if (it == obj.end() || !it->is_string()) return {};
            return it->get<std::string>();
        }

        // glTF node index -> node name (empty if out of range or unnamed).
        static std::string ResolveNodeName(const nlohmann::json& gltf, int32_t nodeIdx)
        {
            if (nodeIdx < 0) return {};
            auto nodes = gltf.find("nodes");
            if (nodes == gltf.end() || !nodes->is_array()) return {};
            if (static_cast<size_t>(nodeIdx) >= nodes->size()) return {};
            return JStr((*nodes)[nodeIdx], "name");
        }

        // The name glTF mesh @p meshIdx gives its morph target @p morphIdx - the
        // name the model's .imorph carries (assimp takes aiAnimMesh::mName from the
        // mesh's extras.targetNames; SceneImporter cuts it to 63 bytes). UniVRM
        // also writes the list per primitive: the fallback. "" = unnamed.
        static std::string TargetName(const nlohmann::json& gltf, int32_t meshIdx, int32_t morphIdx)
        {
            if (meshIdx < 0 || morphIdx < 0) return {};
            const auto meshes = gltf.find("meshes");
            if (meshes == gltf.end() || !meshes->is_array() || static_cast<size_t>(meshIdx) >= meshes->size())
                return {};
            const nlohmann::json& mesh = (*meshes)[static_cast<size_t>(meshIdx)];
            auto fromExtras = [morphIdx](const nlohmann::json& owner) -> std::string
            {
                const auto ex = owner.find("extras");
                if (ex == owner.end() || !ex->is_object()) return {};
                const auto names = ex->find("targetNames");
                if (names == ex->end() || !names->is_array() || static_cast<size_t>(morphIdx) >= names->size())
                    return {};
                const nlohmann::json& n = (*names)[static_cast<size_t>(morphIdx)];
                return n.is_string() ? n.get<std::string>() : std::string();
            };
            std::string n = fromExtras(mesh);
            if (n.empty())
                if (const auto prims = mesh.find("primitives"); prims != mesh.end() && prims->is_array() && !prims->empty())
                    n = fromExtras((*prims)[0]);
            if (n.size() > 63) n.resize(63);   // .imorph / .iskel name field is char[64]
            return n;
        }

        // Parses the VRM 1.0 ("VRMC_vrm") extension @p vrmc into @p out.
        static void ParseVrm1(const nlohmann::json& gltf,
                              const nlohmann::json& vrmc,
                              VrmImportResult&      out)
        {
            out.specVersion = 1;

            if (auto m = vrmc.find("meta"); m != vrmc.end() && m->is_object())
            {
                out.meta.title              = JStr(*m, "name");
                out.meta.version            = JStr(*m, "version");
                out.meta.licenseName        = JStr(*m, "licenseUrl");
                out.meta.contactInformation = JStr(*m, "contactInformation");
                // `references` is an array in VRM 1.0 and JStr keeps only a plain string, so this is usually
                // empty.
                out.meta.reference          = JStr(*m, "references");
                out.meta.licenseTextUrl     = JStr(*m, "thirdPartyLicenses");

                // authors is an array in 1.0; the first is kept.
                if (auto a = m->find("authors"); a != m->end() && a->is_array() && !a->empty())
                {
                    if (a->front().is_string())
                        out.meta.author = a->front().get<std::string>();
                }

                if (auto t = m->find("thumbnailImage"); t != m->end() && t->is_number_integer())
                {
                    // thumbnailImage is a glTF image index; resolve to image name if present.
                    int idx = t->get<int>();
                    auto images = gltf.find("images");
                    if (images != gltf.end() && images->is_array() &&
                        static_cast<size_t>(idx) < images->size())
                        out.meta.thumbnailNodeName = JStr((*images)[idx], "name");
                }
            }

            // humanoid.humanBones is an object keyed by bone name.
            if (auto h = vrmc.find("humanoid"); h != vrmc.end() && h->is_object())
            {
                if (auto hb = h->find("humanBones"); hb != h->end() && hb->is_object())
                {
                    out.humanoidBones.reserve(hb->size());
                    for (auto it = hb->begin(); it != hb->end(); ++it)
                    {
                        if (!it.value().is_object()) continue;
                        auto nodeIt = it.value().find("node");
                        if (nodeIt == it.value().end() || !nodeIt->is_number_integer()) continue;

                        VrmHumanoidBone b;
                        b.humanoidName = it.key();
                        b.nodeIndex    = nodeIt->get<int32_t>();
                        b.nodeName     = ResolveNodeName(gltf, b.nodeIndex);
                        out.humanoidBones.push_back(std::move(b));
                    }
                }
            }

            // expressions.preset / expressions.custom: objects keyed by expression name.
            auto parseExpressionMap = [&](const nlohmann::json& mapNode, bool preset)
            {
                if (!mapNode.is_object()) return;
                for (auto it = mapNode.begin(); it != mapNode.end(); ++it)
                {
                    if (!it.value().is_object()) continue;
                    VrmExpression e;
                    e.name     = it.key();
                    e.isPreset = preset;
                    e.isBinary = false;
                    if (auto isb = it.value().find("isBinary");
                        isb != it.value().end() && isb->is_boolean())
                        e.isBinary = isb->get<bool>();

                    if (auto mb = it.value().find("morphTargetBinds");
                        mb != it.value().end() && mb->is_array())
                    {
                        e.morphBindings.reserve(mb->size());
                        for (const auto& b : *mb)
                        {
                            if (!b.is_object()) continue;
                            VrmExpressionBinding bb;
                            if (auto v = b.find("node");   v != b.end() && v->is_number_integer())
                                bb.nodeIndex  = v->get<int32_t>();   // VRM 1.0 binds a NODE
                            if (auto v = b.find("index");  v != b.end() && v->is_number_integer())
                                bb.morphIndex = v->get<int32_t>();
                            if (auto v = b.find("weight"); v != b.end() && v->is_number())
                                bb.weight     = v->get<float>();
                            // The bound node's mesh holds the target.
                            if (const auto nodes = gltf.find("nodes");
                                bb.nodeIndex >= 0 && nodes != gltf.end() && nodes->is_array() &&
                                static_cast<size_t>(bb.nodeIndex) < nodes->size())
                                if (const auto m = (*nodes)[static_cast<size_t>(bb.nodeIndex)].find("mesh");
                                    m != (*nodes)[static_cast<size_t>(bb.nodeIndex)].end() && m->is_number_integer())
                                    bb.meshIndex = m->get<int32_t>();
                            bb.morphName = TargetName(gltf, bb.meshIndex, bb.morphIndex);
                            e.morphBindings.push_back(bb);
                        }
                    }
                    for (const char* k : { "materialColorBinds", "textureTransformBinds" })
                        if (auto mb = it.value().find(k); mb != it.value().end() && mb->is_array())
                            e.materialBinds += static_cast<uint32_t>(mb->size());
                    out.expressions.push_back(std::move(e));
                }
            };

            if (auto exprs = vrmc.find("expressions"); exprs != vrmc.end() && exprs->is_object())
            {
                if (auto p = exprs->find("preset"); p != exprs->end()) parseExpressionMap(*p, true);
                if (auto c = exprs->find("custom"); c != exprs->end()) parseExpressionMap(*c, false);
            }
        }

        // Parses the VRM 0.x ("VRM") extension @p vrm0 into @p out.
        static void ParseVrm0(const nlohmann::json& gltf,
                              const nlohmann::json& vrm0,
                              VrmImportResult&      out)
        {
            out.specVersion = 0;

            // 0.x meta is a flat object (author is one string).
            if (auto m = vrm0.find("meta"); m != vrm0.end() && m->is_object())
            {
                out.meta.title              = JStr(*m, "title");
                out.meta.version            = JStr(*m, "version");
                out.meta.author             = JStr(*m, "author");
                out.meta.contactInformation = JStr(*m, "contactInformation");
                out.meta.reference          = JStr(*m, "reference");
                out.meta.licenseName        = JStr(*m, "licenseName");
                out.meta.allowedUserName    = JStr(*m, "allowedUserName");
                out.meta.commercialUseName  = JStr(*m, "commercialUssageName"); // VRM 0.x typo'd key
                if (out.meta.commercialUseName.empty())
                    out.meta.commercialUseName = JStr(*m, "commercialUseName");
                out.meta.licenseTextUrl     = JStr(*m, "otherLicenseUrl");

                if (auto t = m->find("texture"); t != m->end() && t->is_number_integer())
                {
                    int idx = t->get<int>();
                    auto textures = gltf.find("textures");
                    if (textures != gltf.end() && textures->is_array() &&
                        static_cast<size_t>(idx) < textures->size())
                        out.meta.thumbnailNodeName = JStr((*textures)[idx], "name");
                }
            }

            // humanoid.humanBones is an array of {bone, node, ...}.
            if (auto h = vrm0.find("humanoid"); h != vrm0.end() && h->is_object())
            {
                if (auto hb = h->find("humanBones"); hb != h->end() && hb->is_array())
                {
                    out.humanoidBones.reserve(hb->size());
                    for (const auto& entry : *hb)
                    {
                        if (!entry.is_object()) continue;
                        auto boneIt = entry.find("bone");
                        auto nodeIt = entry.find("node");
                        if (boneIt == entry.end() || !boneIt->is_string()) continue;
                        if (nodeIt == entry.end() || !nodeIt->is_number_integer()) continue;

                        VrmHumanoidBone b;
                        b.humanoidName = boneIt->get<std::string>();
                        b.nodeIndex    = nodeIt->get<int32_t>();
                        b.nodeName     = ResolveNodeName(gltf, b.nodeIndex);
                        out.humanoidBones.push_back(std::move(b));
                    }
                }
            }

            // blendShapeMaster.blendShapeGroups is an array.
            if (auto bsm = vrm0.find("blendShapeMaster"); bsm != vrm0.end() && bsm->is_object())
            {
                if (auto groups = bsm->find("blendShapeGroups");
                    groups != bsm->end() && groups->is_array())
                {
                    out.expressions.reserve(groups->size());
                    for (const auto& g : *groups)
                    {
                        if (!g.is_object()) continue;
                        VrmExpression e;
                        // 0.x uses presetName (e.g. "joy") + name (display name)
                        e.name     = JStr(g, "presetName");
                        if (e.name.empty() || e.name == "unknown")
                            e.name = JStr(g, "name");
                        e.isPreset = (e.name != JStr(g, "name") || JStr(g, "presetName").size() > 0);
                        if (auto isb = g.find("isBinary"); isb != g.end() && isb->is_boolean())
                            e.isBinary = isb->get<bool>();

                        if (auto binds = g.find("binds"); binds != g.end() && binds->is_array())
                        {
                            e.morphBindings.reserve(binds->size());
                            for (const auto& b : *binds)
                            {
                                if (!b.is_object()) continue;
                                VrmExpressionBinding bb;
                                if (auto v = b.find("mesh");   v != b.end() && v->is_number_integer())
                                    bb.meshIndex  = v->get<int32_t>();
                                if (auto v = b.find("index");  v != b.end() && v->is_number_integer())
                                    bb.morphIndex = v->get<int32_t>();
                                // VRM 0.x weight is 0..100, normalize to 0..1
                                if (auto v = b.find("weight"); v != b.end() && v->is_number())
                                    bb.weight = v->get<float>() / 100.f;
                                bb.morphName = TargetName(gltf, bb.meshIndex, bb.morphIndex);
                                e.morphBindings.push_back(bb);
                            }
                        }
                        if (auto mv = g.find("materialValues"); mv != g.end() && mv->is_array())
                            e.materialBinds = static_cast<uint32_t>(mv->size());
                        out.expressions.push_back(std::move(e));
                    }
                }
            }
        }

        // The expressions as SceneImporter presets: by target name, weights 0..1; a name bound twice
        // keeps its largest weight (two meshes may share a target name, and the runtime drives names).
        // A repeated expression name keeps the first. What cannot be carried is logged once: unnamed
        // targets, material binds, isBinary.
        static std::vector<SceneImporter::PresetHint> PresetsOf(const VrmImportResult& vrm)
        {
            std::vector<SceneImporter::PresetHint> out;
            uint32_t unnamed = 0, materialOnly = 0, binary = 0, repeated = 0;
            for (const VrmExpression& e : vrm.expressions)
            {
                if (e.name.empty()) continue;
                if (std::any_of(out.begin(), out.end(), [&](const SceneImporter::PresetHint& h) { return h.name == e.name; }))
                { ++repeated; continue; }
                SceneImporter::PresetHint h;
                h.name   = e.name;
                h.source = "vrm";
                std::unordered_map<std::string, size_t> at;
                for (const VrmExpressionBinding& b : e.morphBindings)
                {
                    if (b.morphName.empty()) { ++unnamed; continue; }
                    const float w = std::clamp(b.weight, 0.f, 1.f);
                    const auto [it, added] = at.emplace(b.morphName, h.weights.size());
                    if (added) h.weights.emplace_back(b.morphName, w);
                    else       h.weights[it->second].second = (std::max)(h.weights[it->second].second, w);
                }
                if (e.materialBinds > 0 && h.weights.empty()) ++materialOnly;
                if (e.isBinary) ++binary;
                if (!h.weights.empty()) out.push_back(std::move(h));
            }
            size_t materialBinds = 0;
            for (const VrmExpression& e : vrm.expressions) materialBinds += e.materialBinds;
            LOG_INFO("VrmImporter: %zu of %zu expression(s) become presets", out.size(), vrm.expressions.size());
            if (unnamed)
                LOG_WARNING("VrmImporter:   %u bind(s) name a morph target the source leaves unnamed "
                            "(no extras.targetNames) - dropped", unnamed);
            if (materialBinds)
                LOG_WARNING("VrmImporter:   %zu material bind(s) not imported (a preset carries shape-key weights "
                            "only); %u expression(s) are material-only", materialBinds, materialOnly);
            if (binary)
                LOG_INFO("VrmImporter:   %u expression(s) are isBinary - imported as plain presets (a script "
                         "snaps them itself)", binary);
            if (repeated)
                LOG_WARNING("VrmImporter:   %u expression(s) repeat an earlier name - the first kept", repeated);
            return out;
        }
    } // anonymous namespace

    VrmImportResult VrmImporter::Import(const uint8_t* data, size_t size)
    {
        VrmImportResult result;
        if (!data || size == 0)
        {
            LOG_ERROR("VrmImporter: empty input");
            return result;
        }

        const JsonSpan js = ExtractGlbJsonChunk(data, size);
        if (!js.ptr)
        {
            // ExtractGlbJsonChunk logged why, unless the input is under 20 bytes.
            return result;
        }

        nlohmann::json gltf;
        try
        {
            gltf = nlohmann::json::parse(js.ptr, js.ptr + js.len);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("VrmImporter: JSON parse failed: %s", e.what());
            return result;
        }

        auto extensions = gltf.find("extensions");
        if (extensions == gltf.end() || !extensions->is_object())
        {
            LOG_ERROR("VrmImporter: glTF has no 'extensions' block — not a VRM file");
            return result;
        }

        if (auto v1 = extensions->find("VRMC_vrm"); v1 != extensions->end() && v1->is_object())
        {
            ParseVrm1(gltf, *v1, result);
        }
        else if (auto v0 = extensions->find("VRM"); v0 != extensions->end() && v0->is_object())
        {
            ParseVrm0(gltf, *v0, result);
        }
        else
        {
            LOG_ERROR("VrmImporter: neither VRMC_vrm (1.0) nor VRM (0.x) extension found");
            return result;
        }

        result.success = true;
        LOG_INFO("VrmImporter: parsed VRM %d.x — %zu humanoid bone(s), %zu expression(s)",
                 result.specVersion,
                 result.humanoidBones.size(),
                 result.expressions.size());
        return result;
    }

    std::vector<VrmExportedFile> VrmImporter::BuildBlobs(
        const std::string&              sourcePath,
        const std::vector<uint8_t>&     sourceData,
        const VrmImportResult&          vrm,
        std::vector<ImportedShapeKeys>* outShapeKeys,
        std::vector<ImportedCollision>* outCollision,
        const ModelImportContext*       context)
    {
        std::vector<VrmExportedFile> files;

        // 1) SceneImporter (Assimp) extracts mesh / skeleton / materials / animations. The VRM humanoid
        //    map goes in as a hint so the .iskel carries a HumanoidProfile (role -> bone) without name
        //    heuristics; the expressions go in as presets, which SceneImporter cuts to the Meshes whose
        //    targets they move.
        SceneImporter::HumanoidHint hint;
        HumanoidConvention conv = HumanoidConvention::Unknown;
        if (vrm.success && !vrm.humanoidBones.empty())
        {
            conv = (vrm.specVersion == 0) ? HumanoidConvention::Vrm0 : HumanoidConvention::Vrm1;
            hint.reserve(vrm.humanoidBones.size());
            for (const auto& hb : vrm.humanoidBones)
            {
                HumanoidBone role = MapBoneNameToRole(conv, hb.humanoidName.c_str());
                if (role != HumanoidBone::Count && !hb.nodeName.empty())
                    hint.emplace_back(role, hb.nodeName);
            }
        }
        const std::vector<SceneImporter::PresetHint> presets = vrm.success ? PresetsOf(vrm)
                                                                           : std::vector<SceneImporter::PresetHint>{};
        SceneImporter::ImportResult scene = SceneImporter::Import(
            sourcePath, sourceData, hint.empty() ? nullptr : &hint, conv, presets.empty() ? nullptr : &presets,
            context);
        if (!scene.success || scene.files.empty())
        {
            LOG_ERROR("VrmImporter: SceneImporter failed for '%s'", sourcePath.c_str());
            return files;
        }

        files.reserve(scene.files.size());
        for (auto& f : scene.files)
            files.push_back({ std::move(f.relativePath), std::move(f.blob) });
        if (outShapeKeys) *outShapeKeys = std::move(scene.shapeKeys);
        if (outCollision) *outCollision = std::move(scene.collision);

        // 2) The meta goes to the log.
        if (vrm.success)
            LOG_INFO("VrmImporter: VRM %d.x '%s' by '%s' - license '%s'%s%s", vrm.specVersion,
                     vrm.meta.title.c_str(), vrm.meta.author.c_str(), vrm.meta.licenseName.c_str(),
                     vrm.meta.licenseTextUrl.empty() ? "" : " ", vrm.meta.licenseTextUrl.c_str());
        else
            LOG_WARNING("VrmImporter: VRM extension data missing/invalid — emitting glTF data only");

        return files;
    }
} // namespace Resource
