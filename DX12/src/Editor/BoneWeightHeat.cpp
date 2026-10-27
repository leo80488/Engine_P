#include "Editor/BoneWeightHeat.h"

#include "Editor/HeatRamp.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/MeshDescriptorHeap.h"
#include "Renderer/Preview/PreviewTypes.h"
#include "Resource/MeshLibIdentity.h"    // MeshLibCanonicalPath (LoadSections)
#include "Resource/SkeletonAsset.h"      // BlendVertex
#include "Scene/MeshGeometry.h"
#include "Scene/SceneInstanceLoader.h"   // ReadMeshPlacements
#include "System/Log.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace Editor
{
namespace
{
    constexpr std::uint32_t kRegions = 3;   // colour versions in flight: see the header
}

void BoneWeightHeat::Clear()
{
    m_parts.clear();
    m_skipped  = 0;
    m_bone     = -1;
    m_dirty    = true;
    m_gpuReady = false;
    m_failed   = false;
}

struct BoneWeightHeat::LibCache
{
    std::unordered_map<std::string, std::unique_ptr<MeshGeometry::Library>> libs;
};

bool BoneWeightHeat::AddPart(LibCache& cache, const std::string& model, const std::string& meshPath,
                             std::uint32_t entry, std::uint32_t fileIndex, std::uint32_t partIndex, const Stream& bs)
{
    auto& lib = cache.libs[meshPath];
    std::string w;
    if (!lib)
    {
        lib = std::make_unique<MeshGeometry::Library>();
        if (!MeshGeometry::ReadLibrary(meshPath, *lib, w))
        {
            LOG_WARNING("%s weights: '%s' does not read (%s) - its parts are not shown", m_tag, meshPath.c_str(),
                        w.c_str());
            lib->blob.clear();
        }
    }
    if (lib->blob.empty()) { ++m_skipped; return false; }
    MeshGeometry::Section sec;
    if (entry >= lib->meta.meshCount || !MeshGeometry::DecodeSection(*lib, entry, sec, w))
    {
        LOG_WARNING("%s weights: '%s' entry %u does not decode (%s) - not shown", m_tag, meshPath.c_str(), entry,
                    w.c_str());
        ++m_skipped;
        return false;
    }
    if (sec.positions.size() != bs.count)
    {
        // SceneInstanceLoader::Load's VERTEX COUNT MISMATCH: skinning would be wrong too.
        LOG_WARNING("%s weights: '%s' F %u has %zu vertices, its blend stream %zu - not shown", m_tag, model.c_str(),
                    fileIndex, sec.positions.size(), bs.count);
        ++m_skipped;
        return false;
    }
    Part p;
    p.placement = partIndex;
    p.n         = static_cast<std::uint32_t>(sec.positions.size());
    p.pos.resize(std::size_t(p.n) * 3);
    std::memcpy(p.pos.data(), sec.positions.data(), p.pos.size() * sizeof(float));
    p.nrm.assign(std::size_t(p.n) * 3, 0.f);
    if (sec.normals.size() == p.n) std::memcpy(p.nrm.data(), sec.normals.data(), p.nrm.size() * sizeof(float));
    if (sec.tangents.size() == p.n)
    {
        p.tan.resize(std::size_t(p.n) * 4);
        std::memcpy(p.tan.data(), sec.tangents.data(), p.tan.size() * sizeof(float));
    }
    for (int c = 0; c < 4; ++c)
        if (sec.uv[c].size() == p.n)
        {
            p.uv[c].resize(std::size_t(p.n) * 2);
            std::memcpy(p.uv[c].data(), sec.uv[c].data(), p.uv[c].size() * sizeof(float));
        }
    p.idx = std::move(sec.indices);
    p.bones.resize(p.n);
    p.weights.resize(p.n);
    for (std::uint32_t v = 0; v < p.n; ++v)
    {
        const BlendVertex& b = bs.weights[v];
        for (int i = 0; i < 8; ++i)
        {
            p.bones[v][static_cast<std::size_t>(i)]   = b.boneIndices[i];
            p.weights[v][static_cast<std::size_t>(i)] = b.boneWeights[i];
        }
    }
    m_parts.push_back(std::move(p));
    return true;
}

bool BoneWeightHeat::Load(const std::string& model, const std::vector<Stream>& streams, std::string& why)
{
    Clear();

    std::vector<SceneInstanceLoader::MeshPlacement> places;
    if (!SceneInstanceLoader::ReadMeshPlacements(model, places))
    {
        why = "'" + model + "' does not read as a model";
        return false;
    }
    LibCache cache;
    for (std::size_t k = 0; k < places.size(); ++k)
    {
        const SceneInstanceLoader::MeshPlacement& mp = places[k];
        const Stream* bs = nullptr;
        for (const Stream& s : streams)
            if (s.meshIndex == mp.fileIndex) { bs = &s; break; }
        if (!bs || !bs->weights || bs->count == 0) continue;   // not a skinned part
        // The entry: by the F-line's section key first, as the loader resolves it, else the meshId hint.
        std::uint32_t entry = mp.meshId;
        if (mp.sectionKey)
        {
            auto& lib = cache.libs[mp.meshPath];
            if (!lib)
            {
                lib = std::make_unique<MeshGeometry::Library>();
                std::string w;
                if (!MeshGeometry::ReadLibrary(mp.meshPath, *lib, w))
                {
                    LOG_WARNING("%s weights: '%s' does not read (%s) - its parts are not shown", m_tag,
                                mp.meshPath.c_str(), w.c_str());
                    lib->blob.clear();
                }
            }
            for (std::size_t e = 0; e < lib->id.sections.size(); ++e)
                if (lib->id.sections[e].key == mp.sectionKey) { entry = static_cast<std::uint32_t>(e); break; }
        }
        AddPart(cache, model, mp.meshPath, entry, mp.fileIndex, static_cast<std::uint32_t>(k), *bs);
    }
    LOG_INFO("%s weights: '%s' - %zu skinned part(s) paired with their blend streams, %zu skipped", m_tag,
             model.c_str(), m_parts.size(), m_skipped);
    return true;
}

bool BoneWeightHeat::LoadSections(const std::string& model, const std::vector<SectionIn>& sections,
                                  const std::vector<Stream>& streams, std::string& why)
{
    Clear();

    std::vector<SceneInstanceLoader::MeshPlacement> places;
    if (!SceneInstanceLoader::ReadMeshPlacements(model, places))
    {
        why = "'" + model + "' does not read as a model";
        return false;
    }
    LibCache cache;
    for (std::size_t k = 0; k < sections.size(); ++k)
    {
        const SectionIn& sec = sections[k];
        const std::string lib = Resource::MeshLibCanonicalPath(sec.libraryPath);
        // The first F-line placing this entry (by its sec= key when it has one).
        const SceneInstanceLoader::MeshPlacement* mp = nullptr;
        for (const SceneInstanceLoader::MeshPlacement& pl : places)
        {
            if (Resource::MeshLibCanonicalPath(pl.meshPath) != lib) continue;
            if (pl.sectionKey ? pl.sectionKey == sec.key : pl.meshId == sec.entry) { mp = &pl; break; }
        }
        if (!mp) continue;   // the model does not place it: no F-line, no blend stream
        const Stream* bs = nullptr;
        for (const Stream& s : streams)
            if (s.meshIndex == mp->fileIndex) { bs = &s; break; }
        if (!bs || !bs->weights || bs->count == 0) continue;   // not skinned
        AddPart(cache, model, sec.libraryPath, sec.entry, mp->fileIndex, static_cast<std::uint32_t>(k), *bs);
    }
    LOG_INFO("%s weights: '%s' - %zu of %zu section(s) paired with their blend streams, %zu skipped", m_tag,
             model.c_str(), m_parts.size(), sections.size(), m_skipped);
    return true;
}

std::vector<std::uint32_t> BoneWeightHeat::BoneVertexCounts(std::uint32_t boneCount) const
{
    std::vector<std::uint32_t> out(boneCount, 0u);
    std::vector<std::uint32_t> seen(boneCount, ~0u);   // the last vertex counted for a bone
    std::uint32_t vid = 0;
    for (const Part& p : m_parts)
        for (std::uint32_t v = 0; v < p.n; ++v, ++vid)
            for (int i = 0; i < 8; ++i)
            {
                const std::uint32_t b = p.bones[v][static_cast<std::size_t>(i)];
                if (p.weights[v][static_cast<std::size_t>(i)] == 0 || b >= boneCount || seen[b] == vid) continue;
                seen[b] = vid;
                ++out[b];
            }
    return out;
}

void BoneWeightHeat::Release(IGraphicsDevice* gfx, MeshDescriptorHeap* heap, std::uint32_t worldEpoch)
{
    // After a world clear the heap's world slots were rewound under us and belong to someone else:
    // forget them, never free them. The buffers are ours.
    const bool slotsOurs = m_gpuReady && worldEpoch == m_epoch;
    for (Part& p : m_parts)
    {
        if (heap && slotsOurs)
        {
            if (p.slot != RHI::kInvalidBufferIndex) heap->FreeMesh(p.slot);
            if (p.bindless != RHI::kInvalidBufferIndex && p.buf.IsValid()) heap->FreeBuffer(p.buf);
        }
        if (gfx && p.buf.IsValid()) gfx->DestroyBuffer(p.buf);   // deferred by the device
        p.buf      = {};
        p.mapped   = nullptr;
        p.bindless = RHI::kInvalidBufferIndex;
        p.slot     = RHI::kInvalidBufferIndex;
    }
    m_gpuReady = false;
    m_dirty    = true;
}

bool BoneWeightHeat::HasGpu() const
{
    for (const Part& p : m_parts)
        if (p.buf.IsValid()) return true;
    return false;
}

void BoneWeightHeat::SetBone(int bone)
{
    if (bone == m_bone) return;
    m_bone  = bone;
    m_dirty = true;
}

void BoneWeightHeat::Update(IGraphicsDevice& gfx, MeshDescriptorHeap& heap, std::uint32_t worldEpoch)
{
    if (HasGpu() && worldEpoch != m_epoch) Release(&gfx, &heap, worldEpoch);   // forgets the slots
    m_epoch = worldEpoch;
    if (m_parts.empty() || !m_dirty || m_failed || !Active()) return;
    if (!m_gpuReady)
    {
        if (!Create(gfx, heap))
        {
            Release(&gfx, &heap, worldEpoch);
            m_failed = true;   // until the next Load, not every frame
            LOG_WARNING("%s weights: the heatmap copies could not be made - the picture stays as it is", m_tag);
            return;
        }
    }
    else
    {
        ++m_revision;
        const std::uint32_t region = static_cast<std::uint32_t>(m_revision % kRegions);
        for (Part& p : m_parts)
        {
            Colour(p, p.mapped + p.colAt + std::uint64_t(region) * p.n * 4u);
            heap.UpdateMesh(p.slot, Describe(p, region));
        }
    }
    m_dirty = false;
}

bool BoneWeightHeat::Create(IGraphicsDevice& gfx, MeshDescriptorHeap& heap)
{
    ++m_revision;
    const std::uint32_t region = static_cast<std::uint32_t>(m_revision % kRegions);
    for (Part& p : m_parts)
    {
        // The static streams once, then the colour regions.
        std::uint64_t at = 0;
        p.posAt = at; at += p.pos.size() * sizeof(float);
        p.nrmAt = at; at += p.nrm.size() * sizeof(float);
        p.tanAt = at; at += p.tan.size() * sizeof(float);
        for (int c = 0; c < 4; ++c) { p.uvAt[c] = at; at += p.uv[c].size() * sizeof(float); }
        p.idxAt = at; at += p.idx.size() * sizeof(std::uint32_t);
        at = (at + 3u) & ~std::uint64_t(3u);
        p.colAt = at; at += std::uint64_t(kRegions) * p.n * 4u;

        RHI::GPUBufferDesc bd{};
        bd.size       = (at + 15u) & ~std::uint64_t(15u);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (!gfx.CreateBuffer(bd, p.buf)) return false;
        p.mapped = static_cast<std::uint8_t*>(gfx.MapBuffer(p.buf));
        if (!p.mapped) return false;
        std::memcpy(p.mapped + p.posAt, p.pos.data(), p.pos.size() * sizeof(float));
        std::memcpy(p.mapped + p.nrmAt, p.nrm.data(), p.nrm.size() * sizeof(float));
        if (!p.tan.empty()) std::memcpy(p.mapped + p.tanAt, p.tan.data(), p.tan.size() * sizeof(float));
        for (int c = 0; c < 4; ++c)
            if (!p.uv[c].empty()) std::memcpy(p.mapped + p.uvAt[c], p.uv[c].data(), p.uv[c].size() * sizeof(float));
        if (!p.idx.empty()) std::memcpy(p.mapped + p.idxAt, p.idx.data(), p.idx.size() * sizeof(std::uint32_t));
        p.bindless = heap.RegisterBuffer(p.buf);
        if (p.bindless == RHI::kInvalidBufferIndex) return false;
        Colour(p, p.mapped + p.colAt + std::uint64_t(region) * p.n * 4u);
        p.slot = heap.RegisterMesh(Describe(p, region));
        if (p.slot == RHI::kInvalidBufferIndex) return false;
    }
    m_gpuReady = true;
    return true;
}

void BoneWeightHeat::Colour(const Part& p, std::uint8_t* dst) const
{
    // Built in scratch and copied once: the destination is write-combined upload
    // memory, which must never be read back.
    std::vector<std::uint32_t> col(p.n);
    for (std::uint32_t v = 0; v < p.n; ++v)
    {
        std::uint32_t w = 0;
        for (std::size_t i = 0; i < 8; ++i)
            if (p.weights[v][i] != 0 && static_cast<int>(p.bones[v][i]) == m_bone) w += p.weights[v][i];
        col[v] = HeatColour(std::min(1.f, static_cast<float>(w) / 255.f));
    }
    std::memcpy(dst, col.data(), col.size() * sizeof(std::uint32_t));
}

RHI::MeshDescriptor BoneWeightHeat::Describe(const Part& p, std::uint32_t region) const
{
    RHI::MeshDescriptor d{};
    auto stream = [&](RHI::StreamDescriptor& sd, std::uint64_t off, std::uint32_t stride, RHI::VertexFormat f)
    {
        sd.bufferIndex = p.bindless;
        sd.byteOffset  = static_cast<std::uint32_t>(off);
        sd.byteStride  = stride;
        sd.format      = static_cast<std::uint32_t>(f);
    };
    stream(d.position, p.posAt, 12, RHI::VertexFormat::Float3);
    stream(d.normal, p.nrmAt, 12, RHI::VertexFormat::Float3);
    if (!p.tan.empty()) stream(d.tangent, p.tanAt, 16, RHI::VertexFormat::Float4);
    stream(d.color, p.colAt + std::uint64_t(region) * p.n * 4u, 4, RHI::VertexFormat::R8G8B8A8);
    RHI::StreamDescriptor* uvs[4] = { &d.uv0, &d.uv1, &d.uv2, &d.uv3 };
    for (int c = 0; c < 4; ++c)
        if (!p.uv[c].empty()) stream(*uvs[c], p.uvAt[c], 8, RHI::VertexFormat::Float2);
    d.indexBufferIndex = p.bindless;
    d.indexByteOffset  = static_cast<std::uint32_t>(p.idxAt);
    d.indexFormat      = 1;   // uint32
    d.vertexCount      = p.n;
    return d;
}

void BoneWeightHeat::Apply(Preview::ViewDesc& d) const
{
    if (!m_gpuReady || !Active()) return;
    for (const Part& p : m_parts)
    {
        if (p.slot == RHI::kInvalidBufferIndex) continue;
        d.partMeshes.push_back({ p.placement, p.slot, static_cast<std::uint32_t>(p.idx.size()) });
        if (p.placement < 64) d.vertexColorParts |= 1ull << p.placement;
    }
    d.partMeshRevision = m_revision;
}
}
