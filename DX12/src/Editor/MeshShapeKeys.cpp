#include "Editor/MeshShapeKeys.h"

#include "Editor/HeatRamp.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/MeshDescriptorHeap.h"
#include "Renderer/Preview/PreviewTypes.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "Resource/MorphLibrary.h"
#include "Scene/SceneInstanceLoader.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Editor
{
namespace
{
    // The model's .imorph files, from its M lines (one combined file in practice).
    std::vector<std::string> ModelMorphFiles(const std::string& model)
    {
        std::vector<std::string> out;
        std::vector<uint8_t>     blob;
        if (!Resource::AssetFS::Get().ReadFile(model, blob) ||
            blob.size() < sizeof(Resource::AssetHeader) + sizeof(Resource::SceneMetadata))
            return out;
        Resource::AssetHeader h{};
        std::memcpy(&h, blob.data(), sizeof(h));
        if (h.magic != Resource::MAGIC_SCENE || h.metadataSize < sizeof(Resource::SceneMetadata)) return out;
        Resource::SceneMetadata m{};
        std::memcpy(&m, blob.data() + sizeof(Resource::AssetHeader), sizeof(m));
        const std::size_t at = sizeof(Resource::AssetHeader) + h.metadataSize;
        if (at > blob.size()) return out;
        const std::string text(reinterpret_cast<const char*>(blob.data() + at),
                               std::min<std::size_t>(m.textLength, blob.size() - at));
        for (const auto& r : SceneInstanceLoader::ListFileReferences(text, model))
            if (r.kind == 'M' && std::find(out.begin(), out.end(), r.path) == out.end()) out.push_back(r.path);
        return out;
    }

    // The heatmap's colour is Editor/HeatRamp.h (dark blue where nothing moves -> red at
    // the target's largest move), the ramp the Skeleton Editor's weight heatmap uses too.

    constexpr std::uint32_t kRegions = 3;   // versions in flight: see the header
}

bool MeshShapeKeys::Load(const std::string& model, const std::vector<SectionIn>& sections, std::string& why)
{
    m_secs.clear();
    m_names.clear();
    m_weights.clear();
    m_maxDelta.clear();
    m_solo = m_heatmap = -1;
    m_hasNormals = false;
    m_file.clear();
    m_dirty    = true;
    m_gpuReady = false;
    m_failed   = false;

    const std::vector<std::string> files = ModelMorphFiles(model);
    if (files.empty()) return true;

    std::vector<std::vector<uint8_t>>          blobs(files.size());
    std::vector<Resource::MorphLibrary::File>  parsed(files.size());
    for (std::size_t f = 0; f < files.size(); ++f)
    {
        std::string w;
        if (!Resource::AssetFS::Get().ReadFile(files[f], blobs[f]))
        {
            why = "'" + files[f] + "' does not read";
            return false;
        }
        if (!Resource::MorphLibrary::Read(blobs[f].data(), blobs[f].size(), parsed[f], &w))
        {
            why = "'" + files[f] + "': " + w;
            return false;
        }
    }
    m_file = files.front();

    bool everyNormal = true;
    for (std::size_t k = 0; k < sections.size(); ++k)
    {
        const SectionIn& in = sections[k];
        if (!in.pos || in.pos->empty()) continue;
        // The loader's rule: a keyed file by the section's key, else by entry.
        const Resource::MorphLibrary::Block* blk = nullptr;
        for (const Resource::MorphLibrary::File& pf : parsed)
            if ((blk = pf.Find(in.entry, in.key)) != nullptr) break;
        if (!blk || blk->count == 0) continue;
        const std::uint32_t n = static_cast<std::uint32_t>(in.pos->size() / 3);
        if (blk->vertexCount != n)
        {
            LOG_WARNING("[MESHEDITOR] '%s' section %zu: its shape-key block has %u vertices, the geometry %u - "
                        "not shown (a stale .imorph)", model.c_str(), k, blk->vertexCount, n);
            continue;
        }

        Sec s;
        s.placement = static_cast<std::uint32_t>(k);
        s.n         = n;
        s.basePos   = *in.pos;
        if (in.nrm && in.nrm->size() == std::size_t(n) * 3) s.baseNrm = *in.nrm;
        else                                                  s.baseNrm.assign(std::size_t(n) * 3, 0.f);
        if (in.tan && in.tan->size() == std::size_t(n) * 4) s.baseTan = *in.tan;
        for (int c = 0; c < 4; ++c)
            if (in.uv[c] && in.uv[c]->size() == std::size_t(n) * 2) s.uv[c] = *in.uv[c];
        if (in.col && in.col->size() == n) s.baseCol = *in.col;
        if (in.idx) s.idx = *in.idx;

        for (std::uint32_t t = 0; t < blk->count; ++t)
        {
            Target tg;
            const std::string name = blk->Name(t);
            auto it = std::find(m_names.begin(), m_names.end(), name);
            if (it == m_names.end())
            {
                m_names.push_back(name);
                m_maxDelta.push_back(0.f);
                it = m_names.end() - 1;
            }
            tg.name = static_cast<std::uint32_t>(it - m_names.begin());
            tg.pos.resize(std::size_t(n) * 3);
            blk->CopyPositions(t, reinterpret_cast<DirectX::XMFLOAT3*>(tg.pos.data()));
            if (blk->hasNormals)
            {
                tg.nrm.resize(std::size_t(n) * 3);
                blk->CopyNormals(t, reinterpret_cast<DirectX::XMFLOAT3*>(tg.nrm.data()));
            }
            else
                everyNormal = false;
            float mx = 0.f;
            for (std::uint32_t v = 0; v < n; ++v)
            {
                const float* d = &tg.pos[std::size_t(v) * 3];
                mx = std::max(mx, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
            }
            m_maxDelta[tg.name] = std::max(m_maxDelta[tg.name], mx);
            s.targets.push_back(std::move(tg));
        }
        m_secs.push_back(std::move(s));
    }
    m_hasNormals = !m_secs.empty() && everyNormal;
    m_weights.assign(m_names.size(), 0.f);
    LOG_INFO("[MESHEDITOR] shape keys: '%s' - %zu target(s) on %zu section(s)%s", m_file.c_str(), m_names.size(),
             m_secs.size(), m_hasNormals ? ", with normal deltas" : "");
    return true;
}

void MeshShapeKeys::Release(IGraphicsDevice* gfx, MeshDescriptorHeap* heap, std::uint32_t worldEpoch)
{
    // After a world clear the heap's world slots were rewound under us: they are
    // someone else's now, so they are forgotten, never freed. The buffers are ours.
    const bool slotsOurs = m_gpuReady && worldEpoch == m_epoch;
    for (Sec& s : m_secs)
    {
        if (heap && slotsOurs)
        {
            if (s.slot != RHI::kInvalidBufferIndex) heap->FreeMesh(s.slot);
            if (s.bindless != RHI::kInvalidBufferIndex && s.buf.IsValid()) heap->FreeBuffer(s.buf);
        }
        if (gfx && s.buf.IsValid()) gfx->DestroyBuffer(s.buf);   // deferred by the device
        s.buf      = {};
        s.mapped   = nullptr;
        s.bindless = RHI::kInvalidBufferIndex;
        s.slot     = RHI::kInvalidBufferIndex;
    }
    m_gpuReady = false;
    m_dirty    = true;
}

bool MeshShapeKeys::HasGpu() const
{
    for (const Sec& s : m_secs)
        if (s.buf.IsValid()) return true;
    return false;
}

void MeshShapeKeys::Update(IGraphicsDevice& gfx, MeshDescriptorHeap& heap, std::uint32_t worldEpoch)
{
    if (HasGpu() && worldEpoch != m_epoch) Release(&gfx, &heap, worldEpoch);   // forgets the slots
    m_epoch = worldEpoch;
    if (m_secs.empty() || !m_dirty || m_failed) return;

    if (!m_gpuReady)
    {
        if (!Active()) return;             // nothing to draw yet: nothing allocated
        if (!Create(gfx, heap))
        {
            Release(&gfx, &heap, worldEpoch);
            m_failed = true;               // until the next Load, not every frame
            LOG_WARNING("[MESHEDITOR] shape keys: the preview copies could not be made - the picture stays undeformed");
            return;
        }
    }
    else
    {
        ++m_revision;
        const std::uint32_t region = static_cast<std::uint32_t>(m_revision % kRegions);
        for (Sec& s : m_secs)
        {
            Evaluate(s, s.mapped + region * s.region);
            heap.UpdateMesh(s.slot, Describe(s, region));
        }
    }
    m_dirty = false;
}

bool MeshShapeKeys::Create(IGraphicsDevice& gfx, MeshDescriptorHeap& heap)
{
    ++m_revision;
    const std::uint32_t region = static_cast<std::uint32_t>(m_revision % kRegions);
    for (Sec& s : m_secs)
    {
        const bool tan = s.baseTan.size() == std::size_t(s.n) * 4;
        s.region = std::uint64_t(s.n) * (12u + 12u + (tan ? 16u : 0u) + 4u);
        std::uint64_t at = kRegions * s.region;
        for (int c = 0; c < 4; ++c)
        {
            s.uvAt[c] = at;
            at += s.uv[c].size() * sizeof(float);
        }
        s.idxAt = at;
        at += s.idx.size() * sizeof(std::uint32_t);

        RHI::GPUBufferDesc bd{};
        bd.size       = (at + 15u) & ~std::uint64_t(15u);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (!gfx.CreateBuffer(bd, s.buf)) return false;
        s.mapped = static_cast<std::uint8_t*>(gfx.MapBuffer(s.buf));
        if (!s.mapped) return false;
        for (int c = 0; c < 4; ++c)
            if (!s.uv[c].empty()) std::memcpy(s.mapped + s.uvAt[c], s.uv[c].data(), s.uv[c].size() * sizeof(float));
        if (!s.idx.empty()) std::memcpy(s.mapped + s.idxAt, s.idx.data(), s.idx.size() * sizeof(std::uint32_t));
        s.bindless = heap.RegisterBuffer(s.buf);
        if (s.bindless == RHI::kInvalidBufferIndex) return false;
        Evaluate(s, s.mapped + region * s.region);
        s.slot = heap.RegisterMesh(Describe(s, region));
        if (s.slot == RHI::kInvalidBufferIndex) return false;
    }
    m_gpuReady = true;
    return true;
}

float MeshShapeKeys::EffectiveWeight(std::uint32_t name) const
{
    if (name >= m_weights.size()) return 0.f;
    if (m_solo >= 0 && static_cast<int>(name) != m_solo) return 0.f;
    return m_weights[name];
}

void MeshShapeKeys::Evaluate(Sec& s, std::uint8_t* dst) const
{
    // Built in scratch and copied once: the destination is write-combined upload
    // memory, which must never be read back.
    const std::size_t n3  = std::size_t(s.n) * 3;
    const bool        tan = s.baseTan.size() == std::size_t(s.n) * 4;
    std::vector<float> pos(s.basePos), nsum;
    for (const Target& t : s.targets)
    {
        const float w = EffectiveWeight(t.name);
        if (!(w > 0.001f)) continue;   // Skin.cs's bar (and never a NaN)
        for (std::size_t i = 0; i < n3; ++i) pos[i] += w * t.pos[i];
        if (!t.nrm.empty())
        {
            if (nsum.empty()) nsum.assign(n3, 0.f);
            for (std::size_t i = 0; i < n3; ++i) nsum[i] += w * t.nrm[i];
        }
    }
    std::vector<float> nrm(s.baseNrm), tng(s.baseTan);
    if (!nsum.empty())
        for (std::uint32_t v = 0; v < s.n; ++v)
        {
            float* nn = &nrm[std::size_t(v) * 3];
            const float x = nn[0] + nsum[std::size_t(v) * 3], y = nn[1] + nsum[std::size_t(v) * 3 + 1],
                        z = nn[2] + nsum[std::size_t(v) * 3 + 2];
            const float l2 = x * x + y * y + z * z;
            if (!(l2 > 1e-12f)) continue;   // collapsed: keep the rest normal
            const float inv = 1.f / std::sqrt(l2);
            nn[0] = x * inv; nn[1] = y * inv; nn[2] = z * inv;
            if (tan)
            {
                // Gram-Schmidt against the morphed normal; the handedness stays.
                float* tt = &tng[std::size_t(v) * 4];
                const float d  = tt[0] * nn[0] + tt[1] * nn[1] + tt[2] * nn[2];
                const float tx = tt[0] - nn[0] * d, ty = tt[1] - nn[1] * d, tz = tt[2] - nn[2] * d;
                const float t2 = tx * tx + ty * ty + tz * tz;
                if (t2 > 1e-12f)
                {
                    const float ti = 1.f / std::sqrt(t2);
                    tt[0] = tx * ti; tt[1] = ty * ti; tt[2] = tz * ti;
                }
            }
        }

    std::vector<std::uint32_t> col(s.n, 0xFFFFFFFFu);
    if (m_heatmap >= 0)
    {
        const Target* ht = nullptr;
        for (const Target& t : s.targets)
            if (static_cast<int>(t.name) == m_heatmap) { ht = &t; break; }
        const float mx = m_maxDelta[static_cast<std::size_t>(m_heatmap)];
        for (std::uint32_t v = 0; v < s.n; ++v)
        {
            float d = 0.f;
            if (ht && mx > 0.f)
            {
                const float* p = &ht->pos[std::size_t(v) * 3];
                d = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) / mx;
            }
            col[v] = HeatColour(d);
        }
    }
    else if (s.baseCol.size() == s.n)
        col = s.baseCol;

    std::uint8_t* p = dst;
    std::memcpy(p, pos.data(), n3 * sizeof(float));  p += n3 * sizeof(float);
    std::memcpy(p, nrm.data(), n3 * sizeof(float));  p += n3 * sizeof(float);
    if (tan) { std::memcpy(p, tng.data(), tng.size() * sizeof(float)); p += tng.size() * sizeof(float); }
    std::memcpy(p, col.data(), col.size() * sizeof(std::uint32_t));
}

RHI::MeshDescriptor MeshShapeKeys::Describe(const Sec& s, std::uint32_t region) const
{
    RHI::MeshDescriptor d{};
    auto stream = [&](RHI::StreamDescriptor& sd, std::uint64_t off, std::uint32_t stride, RHI::VertexFormat f)
    {
        sd.bufferIndex = s.bindless;
        sd.byteOffset  = static_cast<std::uint32_t>(off);
        sd.byteStride  = stride;
        sd.format      = static_cast<std::uint32_t>(f);
    };
    const bool          tan  = s.baseTan.size() == std::size_t(s.n) * 4;
    const std::uint64_t base = std::uint64_t(region) * s.region;
    const std::uint64_t n    = s.n;
    stream(d.position, base, 12, RHI::VertexFormat::Float3);
    stream(d.normal, base + n * 12, 12, RHI::VertexFormat::Float3);
    if (tan) stream(d.tangent, base + n * 24, 16, RHI::VertexFormat::Float4);
    stream(d.color, base + n * (24 + (tan ? 16 : 0)), 4, RHI::VertexFormat::R8G8B8A8);
    RHI::StreamDescriptor* uvs[4] = { &d.uv0, &d.uv1, &d.uv2, &d.uv3 };
    for (int c = 0; c < 4; ++c)
        if (!s.uv[c].empty()) stream(*uvs[c], s.uvAt[c], 8, RHI::VertexFormat::Float2);
    d.indexBufferIndex = s.bindless;
    d.indexByteOffset  = static_cast<std::uint32_t>(s.idxAt);
    d.indexFormat      = 1;   // uint32
    d.vertexCount      = s.n;
    return d;
}

void MeshShapeKeys::Apply(Preview::ViewDesc& d) const
{
    if (!m_gpuReady || !Active()) return;
    for (const Sec& s : m_secs)
    {
        if (s.slot == RHI::kInvalidBufferIndex) continue;
        d.partMeshes.push_back({ s.placement, s.slot, static_cast<std::uint32_t>(s.idx.size()) });
        if (m_heatmap >= 0 && s.placement < 64) d.vertexColorParts |= 1ull << s.placement;
    }
    d.partMeshRevision = m_revision;
}

int MeshShapeKeys::Find(const std::string& name) const
{
    const auto it = std::find(m_names.begin(), m_names.end(), name);
    return it == m_names.end() ? -1 : static_cast<int>(it - m_names.begin());
}

bool MeshShapeKeys::SetWeight(std::size_t i, float w)
{
    if (i >= m_weights.size() || !std::isfinite(w)) return false;
    w = std::clamp(w, 0.f, 1.f);
    if (m_weights[i] != w) { m_weights[i] = w; m_dirty = true; }
    return true;
}

void MeshShapeKeys::ResetWeights()
{
    for (float& w : m_weights)
        if (w != 0.f) { w = 0.f; m_dirty = true; }
    if (m_solo != -1) { m_solo = -1; m_dirty = true; }
}

bool MeshShapeKeys::SetSolo(int i)
{
    if (i < -1 || i >= static_cast<int>(m_names.size())) return false;
    if (m_solo != i) { m_solo = i; m_dirty = true; }
    return true;
}

bool MeshShapeKeys::SetHeatmap(int i)
{
    if (i < -1 || i >= static_cast<int>(m_names.size())) return false;
    if (m_heatmap != i) { m_heatmap = i; m_dirty = true; }
    return true;
}

std::uint32_t MeshShapeKeys::SectionsOf(std::size_t i) const
{
    std::uint32_t k = 0;
    for (const Sec& s : m_secs)
        for (const Target& t : s.targets)
            if (t.name == i) { ++k; break; }
    return k;
}

std::uint32_t MeshShapeKeys::MorphSections() const
{
    return static_cast<std::uint32_t>(m_secs.size());
}

MeshShapeKeys::Snapshot MeshShapeKeys::Snap() const
{
    Snapshot s;
    for (std::size_t i = 0; i < m_names.size(); ++i)
        if (m_weights[i] != 0.f) s.weights.emplace_back(m_names[i], m_weights[i]);
    if (m_solo >= 0)    s.solo    = m_names[static_cast<std::size_t>(m_solo)];
    if (m_heatmap >= 0) s.heatmap = m_names[static_cast<std::size_t>(m_heatmap)];
    return s;
}

void MeshShapeKeys::Restore(const Snapshot& s)
{
    for (const auto& [name, w] : s.weights)
        if (const int i = Find(name); i >= 0) SetWeight(static_cast<std::size_t>(i), w);
    if (!s.solo.empty())    SetSolo(Find(s.solo));
    if (!s.heatmap.empty()) SetHeatmap(Find(s.heatmap));
}

bool MeshShapeKeys::Active() const
{
    if (m_secs.empty()) return false;
    if (m_heatmap >= 0) return true;
    for (std::size_t i = 0; i < m_weights.size(); ++i)
        if (EffectiveWeight(static_cast<std::uint32_t>(i)) > 0.001f) return true;
    return false;
}
}
