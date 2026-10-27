#ifndef NOMINMAX
#define NOMINMAX
#endif

// MeshletBuilder.cpp - the contract is in Resource/MeshletBuilder.h.

#include "Resource/MeshletBuilder.h"
#include "Resource/MeshletData.h"
#include "Resource/MeshLibIdentity.h"
#include "Resource/MeshLibTail.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"

#include "meshoptimizer/meshoptimizer.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

// The prebuilt meshoptimizer (external/meshoptimizer/<cfg>); both exes and the
// unit-test drivers carry its library directory.
#ifdef _DEBUG
#pragma comment(lib, "meshoptimizer_debug.lib")
#else
#pragma comment(lib, "meshoptimizer_release.lib")
#endif

namespace Resource
{
namespace
{
    struct Library
    {
        MeshLibraryMetadata meta{};
        const uint8_t*      table = nullptr;   // the entry table
        const uint8_t*      vb    = nullptr;
        const uint8_t*      ib    = nullptr;
    };

    // One entry's clusters, before they are laid out with the others: offsets
    // are LOCAL to this entry's arrays (in elements).
    struct EntryOut
    {
        std::vector<MeshletDesc>  descs;
        std::vector<uint32_t>     vertexRefs;    // absolute library vertex indices
        std::vector<uint32_t>     triangles;     // packed i0 | i1 << 8 | i2 << 16
        std::vector<MeshletLevel> levels;        // firstCluster local
        MeshletEntryBuildInfo     info;
    };

    // The entry's geometry in a compact, cluster-friendly form: its indices
    // rebased onto [lo, hi) - the vertex range they really land in - and the
    // positions of that range. meshoptimizer sizes its scratch by the vertex
    // count it is given, so handing it the whole library per entry would cost
    // O(entries x vertices).
    struct EntryGeometry
    {
        uint32_t              lo = 0;
        std::vector<uint32_t> indices;     // local
        std::vector<float>    positions;   // float3 per local vertex
    };

    template <typename T>
    T ReadPod(const uint8_t* p)
    {
        T v;
        std::memcpy(&v, p, sizeof(T));
        return v;
    }

    template <typename T>
    void AppendPod(std::vector<uint8_t>& out, const T& v)
    {
        const auto* b = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), b, b + sizeof(T));
    }

    bool ReadLibrary(const uint8_t* blob, size_t size, Library& outLibrary, std::string* err)
    {
        auto fail = [&](const char* why) { if (err) *err = why; return false; };
        if (!ValidateHeader(blob, size, MAGIC_MESHLIB)) return fail("not a .meshlib (header)");
        const AssetHeader h = ReadPod<AssetHeader>(blob);
        if (h.metadataSize < sizeof(MeshLibraryMetadata)) return fail("metadata smaller than MeshLibraryMetadata");
        outLibrary.meta = ReadPod<MeshLibraryMetadata>(blob + sizeof(AssetHeader));
        if (outLibrary.meta.indexStride != 4) return fail("indexStride is not 4");
        if (outLibrary.meta.vertexStride != ComputeMeshLibVertexLayout(outLibrary.meta.flags).stride)
            return fail("vertexStride disagrees with the layout flags");
        const size_t tableOff = sizeof(AssetHeader) + h.metadataSize;
        const size_t vbOff    = tableOff + size_t(outLibrary.meta.meshCount) * sizeof(MeshLibraryEntry);
        const size_t ibOff    = vbOff + size_t(outLibrary.meta.vertexCount) * outLibrary.meta.vertexStride;
        if (ibOff + size_t(outLibrary.meta.indexCount) * 4 > size) return fail("truncated entry table / vertex / index data");
        outLibrary.table = blob + tableOff;
        outLibrary.vb    = blob + vbOff;
        outLibrary.ib    = blob + ibOff;
        return true;
    }

    bool GatherEntry(const Library& library, const MeshLibraryEntry& en, EntryGeometry& g, std::string& why)
    {
        if (en.indexCount < 3) { why = "no triangles"; return false; }
        if (uint64_t(en.indexStart) + en.indexCount > library.meta.indexCount) { why = "indices past the index buffer"; return false; }
        const uint32_t triCount = en.indexCount / 3;
        uint32_t lo = UINT32_MAX, hi = 0;
        for (uint32_t i = 0; i < triCount * 3; ++i)
        {
            const uint32_t v = ReadPod<uint32_t>(library.ib + (size_t(en.indexStart) + i) * 4);
            if (v >= library.meta.vertexCount) { why = "an index past the vertex buffer"; return false; }
            lo = std::min(lo, v);
            hi = std::max(hi, v + 1);
        }
        g.lo = lo;
        g.indices.resize(size_t(triCount) * 3);
        for (uint32_t i = 0; i < triCount * 3; ++i)
            g.indices[i] = ReadPod<uint32_t>(library.ib + (size_t(en.indexStart) + i) * 4) - lo;
        const uint32_t n      = hi - lo;
        const uint32_t stride = library.meta.vertexStride;
        g.positions.resize(size_t(n) * 3);
        for (uint32_t v = 0; v < n; ++v)
            std::memcpy(&g.positions[size_t(v) * 3], library.vb + size_t(lo + v) * stride, 12);   // position at offset 0
        return true;
    }

    uint32_t PackCone(const meshopt_Bounds& b)
    {
        return uint32_t(uint8_t(b.cone_axis_s8[0])) | (uint32_t(uint8_t(b.cone_axis_s8[1])) << 8) |
               (uint32_t(uint8_t(b.cone_axis_s8[2])) << 16) | (uint32_t(uint8_t(b.cone_cutoff_s8)) << 24);
    }

    // Cluster one level: @p indices (local) -> clusters appended to @p out, all
    // tagged @p level. Returns the number of clusters made.
    uint32_t BuildLevel(const EntryGeometry& g, const std::vector<uint32_t>& indices, uint32_t level, EntryOut& out)
    {
        const size_t nv = g.positions.size() / 3;
        const size_t maxMeshlets = meshopt_buildMeshletsBound(indices.size(), kMeshletMaxVerts, kMeshletMaxTris);
        std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
        std::vector<unsigned int>    mverts(indices.size());
        std::vector<unsigned char>   mtris(indices.size());
        const size_t count = meshopt_buildMeshlets(meshlets.data(), mverts.data(), mtris.data(), indices.data(),
                                                   indices.size(), g.positions.data(), nv, sizeof(float) * 3,
                                                   kMeshletMaxVerts, kMeshletMaxTris, kMeshletConeWeight);
        uint32_t made = 0;
        for (size_t m = 0; m < count; ++m)
        {
            const meshopt_Meshlet& ml = meshlets[m];
            if (ml.triangle_count == 0 || ml.vertex_count == 0) continue;
            meshopt_optimizeMeshlet(&mverts[ml.vertex_offset], &mtris[ml.triangle_offset], ml.triangle_count,
                                    ml.vertex_count);
            const meshopt_Bounds b = meshopt_computeMeshletBounds(&mverts[ml.vertex_offset], &mtris[ml.triangle_offset],
                                                                  ml.triangle_count, g.positions.data(), nv,
                                                                  sizeof(float) * 3);
            MeshletDesc d{};
            d.cullCenter[0] = b.center[0];
            d.cullCenter[1] = b.center[1];
            d.cullCenter[2] = b.center[2];
            d.cullRadius    = b.radius;
            d.coneAxisCutoff = PackCone(b);
            d.vertexRefOffset = static_cast<uint32_t>(out.vertexRefs.size());
            d.triangleOffset  = static_cast<uint32_t>(out.triangles.size());
            const uint32_t flags = b.cone_cutoff_s8 >= 127 ? MESHLET_FLAG_NO_CONE : 0u;
            d.counts = PackMeshletCounts(ml.vertex_count, ml.triangle_count, level, flags);
            for (unsigned v = 0; v < ml.vertex_count; ++v) out.vertexRefs.push_back(mverts[ml.vertex_offset + v] + g.lo);
            for (unsigned t = 0; t < ml.triangle_count; ++t)
            {
                const unsigned char* tri = &mtris[ml.triangle_offset + size_t(t) * 3];
                out.triangles.push_back(uint32_t(tri[0]) | (uint32_t(tri[1]) << 8) | (uint32_t(tri[2]) << 16));
            }
            out.descs.push_back(d);
            ++made;
        }
        return made;
    }

    // The entry's bounding sphere over the vertices its indices reference.
    meshopt_Bounds EntrySphere(const EntryGeometry& g)
    {
        std::vector<uint8_t> used(g.positions.size() / 3, 0);
        for (uint32_t i : g.indices) used[i] = 1;
        std::vector<float> pts;
        pts.reserve(g.positions.size());
        for (size_t v = 0; v < used.size(); ++v)
            if (used[v]) pts.insert(pts.end(), &g.positions[v * 3], &g.positions[v * 3] + 3);
        return meshopt_computeSphereBounds(pts.data(), pts.size() / 3, sizeof(float) * 3, nullptr, 0);
    }

    // ==== The discrete LOD chain ====
    // Each level is the ORIGINAL index buffer simplified to 1/2^k of its triangles, never the previous
    // level simplified again, so a level's error is against the source - the error the runtime
    // compares with tau.
    // The attributes are the normal (3) and uv0 (2): every .meshlib vertex opens with pos(12)
    // normal(12) uv0(8) (ComputeMeshLibVertexLayout), so they are the 5 floats at byte 12.
    // meshoptimizer folds the weighted attribute error into the error it returns (made absolute by
    // ErrorAbsolute), so texture swimming or a shading change counts like a geometric one, and a flat
    // wall whose UVs a collapse would drag does not get an error of 0.
    constexpr float    kNormalWeight   = 0.5f;
    constexpr float    kUvWeight       = 1.0f;
    constexpr double   kMinReduction   = 0.85;   // a level must cut at least 15%
    constexpr uint32_t kMaxLodLevels   = 16;     // 2^-15 of the source; counts.level holds 8 bits

    void BuildLodLevels(const Library& library, const EntryGeometry& g, EntryOut& out)
    {
        const size_t nv = g.positions.size() / 3;
        std::vector<float> attrs(nv * 5);
        for (size_t v = 0; v < nv; ++v)
            std::memcpy(&attrs[v * 5], library.vb + (size_t(g.lo) + v) * library.meta.vertexStride + 12, 20);
        static const float kWeights[5] = { kNormalWeight, kNormalWeight, kNormalWeight, kUvWeight, kUvWeight };
        const float scale = meshopt_simplifyScale(g.positions.data(), nv, sizeof(float) * 3);

        // The shading check (MeshletData.h): a level's damage is measured
        // against level 0's, the source's own.
        auto shadingDamage = [&](const std::vector<uint32_t>& idx)
        {
            return MeshletShadingDamage(reinterpret_cast<const uint8_t*>(g.positions.data()), sizeof(float) * 3,
                                        reinterpret_cast<const uint8_t*>(attrs.data()), sizeof(float) * 5,
                                        idx.data(), idx.size());
        };
        const float baseDamage = shadingDamage(g.indices);

        std::vector<uint32_t> lod(g.indices.size());
        size_t prevCount = g.indices.size();
        float  prevError = 0.f;
        for (uint32_t k = 1; k < kMaxLodLevels; ++k)
        {
            if (prevCount / 3 <= kMeshletMaxTris) { out.info.stop = "fits one cluster"; break; }
            size_t target = ((g.indices.size() >> k) / 3) * 3;
            if (target < 3) target = 3;
            float  err = 0.f;
            size_t n   = meshopt_simplifyWithAttributes(lod.data(), g.indices.data(), g.indices.size(),
                                                        g.positions.data(), nv, sizeof(float) * 3,
                                                        attrs.data(), sizeof(float) * 5, kWeights, 5, nullptr,
                                                        target, FLT_MAX, meshopt_SimplifyErrorAbsolute, &err);
            bool sloppy = false;
            if (double(n) > double(prevCount) * kMinReduction)
            {
                // Topology-preserving simplification is stuck (open borders,
                // UV seams everywhere - foliage cards, thin sheets). A far
                // level does not need the topology: try the sloppy one.
                float  rel = 0.f;
                size_t ns  = meshopt_simplifySloppy(lod.data(), g.indices.data(), g.indices.size(),
                                                    g.positions.data(), nv, sizeof(float) * 3, nullptr,
                                                    target, FLT_MAX, &rel);
                if (ns >= 3 && double(ns) <= double(prevCount) * kMinReduction)
                {
                    n      = ns;
                    err    = rel * scale;
                    sloppy = true;
                }
            }
            if (n < 3)                                          { out.info.stop = "simplified to nothing"; break; }
            if (double(n) > double(prevCount) * kMinReduction)   { out.info.stop = "cannot cut 15%"; break; }
            if (!std::isfinite(err) || err < 0.f) err = prevError;
            err = std::max(err, prevError);                      // monotonic: e_k = max(e_k, e_k-1)

            // A collapse can leave a triangle whose three corners are collinear:
            // it rasterizes nothing, and its normal is floating-point noise that
            // would widen its cluster's cone (or point it anywhere). Level 0 keeps
            // the source's triangles exactly; a simplified level drops these.
            std::vector<uint32_t> level;
            level.reserve(n);
            for (size_t t = 0; t + 2 < n; t += 3)
            {
                const float* a = &g.positions[size_t(lod[t]) * 3];
                const float* b = &g.positions[size_t(lod[t + 1]) * 3];
                const float* c = &g.positions[size_t(lod[t + 2]) * 3];
                const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
                const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                const float cx = e1[1] * e2[2] - e1[2] * e2[1];
                const float cy = e1[2] * e2[0] - e1[0] * e2[2];
                const float cz = e1[0] * e2[1] - e1[1] * e2[0];
                // Collinear: the sine of the corner angle below 1e-5 (relative to
                // the edges, so it holds at any scale; float noise on a truly
                // collinear triple is ~1e-7).
                const float l1 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
                const float l2 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
                if (cx * cx + cy * cy + cz * cz <= 1e-10f * l1 * l2) continue;
                level.insert(level.end(), { lod[t], lod[t + 1], lod[t + 2] });
            }
            if (level.size() < 3) { out.info.stop = "simplified to degenerate triangles"; break; }
            n = level.size();
            const uint32_t first = static_cast<uint32_t>(out.descs.size());
            const uint32_t made  = BuildLevel(g, level, k, out);
            if (made == 0) { out.info.stop = "a level made no clusters"; break; }
            out.levels.push_back(MeshletLevel{ first, made, err, kMeshletRootError });
            out.info.levelTriangles.push_back(static_cast<uint32_t>(n / 3));
            out.info.levelErrors.push_back(err);
            out.info.levelShadingDamage.push_back(shadingDamage(level) - baseDamage);
            if (sloppy) ++out.info.sloppyLevels;
            prevCount = n;
            prevError = err;
        }
        if (out.info.stop.empty()) out.info.stop = "level cap";
    }

    void BuildEntry(const Library& library, uint32_t e, const MeshletBuildOptions& opt, EntryOut& out)
    {
        out = EntryOut{};
        const MeshLibraryEntry en = ReadPod<MeshLibraryEntry>(library.table + size_t(e) * sizeof(MeshLibraryEntry));
        EntryGeometry g;
        std::string   why;
        if (!GatherEntry(library, en, g, why))
        {
            out.info.stop = why;
            return;
        }
        const meshopt_Bounds sphere = EntrySphere(g);
        out.info.triangles = static_cast<uint32_t>(g.indices.size() / 3);
        out.info.radius    = sphere.radius;

        // Level 0.
        const uint32_t n0 = BuildLevel(g, g.indices, 0, out);
        if (n0 == 0)
        {
            out = EntryOut{};
            out.info.stop = "no clusters";
            return;
        }
        out.levels.push_back(MeshletLevel{ 0, n0, 0.f, kMeshletRootError });
        out.info.levelTriangles.push_back(out.info.triangles);
        out.info.levelErrors.push_back(0.f);
        out.info.levelShadingDamage.push_back(0.f);

        // Levels 1..
        if (opt.lod) BuildLodLevels(library, g, out);

        // LOD data: the entry is one group - both spheres are the entry's, level k's self error is e_k,
        // its parent error e_k+1 (last level: kMeshletRootError).
        for (size_t l = 0; l < out.levels.size(); ++l)
        {
            MeshletLevel& lv = out.levels[l];
            const float self   = out.info.levelErrors[l];
            const float parent = l + 1 < out.levels.size() ? out.info.levelErrors[l + 1] : kMeshletRootError;
            lv.minSelfError   = self;
            lv.maxParentError = parent;
            for (uint32_t k = lv.firstCluster; k < lv.firstCluster + lv.clusterCount; ++k)
            {
                MeshletDesc& d = out.descs[k];
                for (int a = 0; a < 3; ++a) d.lodSelfCenter[a] = d.lodParentCenter[a] = sphere.center[a];
                d.lodSelfRadius = d.lodParentRadius = sphere.radius;
                d.lodSelfError   = self;
                d.lodParentError = parent;
            }
        }
        out.info.built = true;
    }
}

bool BuildMeshletChunk(const uint8_t* blob, size_t size, const MeshletBuildOptions& options,
                       std::vector<uint8_t>& chunk, MeshletBuildStats* stats, std::string* err)
{
    const auto t0 = std::chrono::steady_clock::now();
    chunk.clear();
    Library library;
    if (!ReadLibrary(blob, size, library, err)) return false;
    const uint32_t E = library.meta.meshCount;

    std::vector<EntryOut> outs(E);
    std::vector<uint32_t> todo;
    for (uint32_t e = 0; e < E; ++e)
        if (options.entryMask.empty() || (e < options.entryMask.size() && options.entryMask[e])) todo.push_back(e);

    uint32_t threads = options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    threads = std::min<uint32_t>(threads, static_cast<uint32_t>(todo.size()));
    if (threads <= 1)
        for (uint32_t e : todo) BuildEntry(library, e, options, outs[e]);
    else
    {
        // Dynamic hand-out; every entry writes only its own slot, so the result
        // is the serial one whatever the schedule.
        std::atomic<size_t> next{ 0 };
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (uint32_t t = 0; t < threads; ++t)
            pool.emplace_back([&]
            {
                for (size_t i = next.fetch_add(1); i < todo.size(); i = next.fetch_add(1))
                    BuildEntry(library, todo[i], options, outs[todo[i]]);
            });
        for (std::thread& t : pool) t.join();
    }

    // ---- Lay the entries out ----
    MeshletBuildStats st;
    st.entries = E;
    st.perEntry.resize(E);
    uint32_t clusterCount = 0, refCount = 0, triCount = 0, levelCount = 0;
    for (uint32_t e = 0; e < E; ++e)
    {
        const EntryOut& o = outs[e];
        clusterCount += static_cast<uint32_t>(o.descs.size());
        refCount     += static_cast<uint32_t>(o.vertexRefs.size());
        triCount     += static_cast<uint32_t>(o.triangles.size());
        levelCount   += static_cast<uint32_t>(o.levels.size());
    }
    if (clusterCount == 0)
    {
        for (uint32_t e = 0; e < E; ++e) st.perEntry[e] = std::move(outs[e].info);
        st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (stats) *stats = std::move(st);
        return true;   // nothing to build: an empty chunk
    }

    const uint32_t refBase = clusterCount * static_cast<uint32_t>(sizeof(MeshletDesc) / 4);   // DWORDs
    const uint32_t triBase = refBase + refCount;
    std::vector<MeshletEntryRange> ranges(E);
    std::vector<MeshletLevel>      levels;
    std::vector<MeshletDesc>       descs;
    std::vector<uint32_t>          refs, tris;
    levels.reserve(levelCount);
    descs.reserve(clusterCount);
    refs.reserve(refCount);
    tris.reserve(triCount);
    for (uint32_t e = 0; e < E; ++e)
    {
        EntryOut& o = outs[e];
        MeshletEntryRange& r = ranges[e];
        r = MeshletEntryRange{ static_cast<uint32_t>(descs.size()), static_cast<uint32_t>(o.descs.size()),
                               static_cast<uint32_t>(levels.size()), static_cast<uint32_t>(o.levels.size()) };
        if (o.descs.empty()) r = MeshletEntryRange{ static_cast<uint32_t>(descs.size()), 0, static_cast<uint32_t>(levels.size()), 0 };
        for (MeshletLevel lv : o.levels)
        {
            lv.firstCluster += r.firstCluster;
            levels.push_back(lv);
        }
        const uint32_t refAt = static_cast<uint32_t>(refs.size()), triAt = static_cast<uint32_t>(tris.size());
        for (MeshletDesc d : o.descs)
        {
            d.vertexRefOffset += refBase + refAt;
            d.triangleOffset  += triBase + triAt;
            descs.push_back(d);
        }
        refs.insert(refs.end(), o.vertexRefs.begin(), o.vertexRefs.end());
        tris.insert(tris.end(), o.triangles.begin(), o.triangles.end());

        if (!o.descs.empty())
        {
            ++st.entriesBuilt;
            st.level0Clusters  += o.levels.empty() ? 0 : o.levels[0].clusterCount;
            st.level0Triangles += o.info.triangles;
            st.maxLevels        = std::max<uint32_t>(st.maxLevels, static_cast<uint32_t>(o.levels.size()));
            st.sloppyLevels    += o.info.sloppyLevels;
            for (uint32_t t : o.info.levelTriangles) st.triangles += t;
        }
        st.perEntry[e] = std::move(o.info);
    }

    MeshletChunkHeader h{};
    h.magic           = MESHLET_CHUNK_MAGIC;
    h.entryCount      = E;
    h.vertexCount     = library.meta.vertexCount;
    h.indexCount      = library.meta.indexCount;
    h.coreFingerprint = MeshLibCoreFingerprint(blob, size);
    h.clusterCount    = clusterCount;
    h.vertexRefCount  = refCount;
    h.triangleCount   = triCount;
    h.levelCount      = static_cast<uint32_t>(levels.size());
    const size_t cpuEnd = sizeof(MeshletChunkHeader) + ranges.size() * sizeof(MeshletEntryRange) +
                          levels.size() * sizeof(MeshletLevel);
    h.gpuByteOffset   = static_cast<uint32_t>((cpuEnd + 15) & ~size_t(15));
    h.gpuByteSize     = static_cast<uint32_t>(descs.size() * sizeof(MeshletDesc) + 4 * (refs.size() + tris.size()));

    chunk.reserve(size_t(h.gpuByteOffset) + h.gpuByteSize);
    AppendPod(chunk, h);
    for (const MeshletEntryRange& r : ranges) AppendPod(chunk, r);
    for (const MeshletLevel& lv : levels)     AppendPod(chunk, lv);
    chunk.resize(h.gpuByteOffset, 0);
    for (const MeshletDesc& d : descs) AppendPod(chunk, d);
    for (uint32_t v : refs)            AppendPod(chunk, v);
    for (uint32_t t : tris)            AppendPod(chunk, t);

    st.clusters     = clusterCount;
    st.vertexRefs   = refCount;
    st.levelRecords = static_cast<uint32_t>(levels.size());
    st.chunkBytes   = chunk.size();
    st.gpuBytes     = h.gpuByteSize;
    st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (stats) *stats = std::move(st);
    return true;
}

bool AddMeshletsToMeshLib(std::vector<uint8_t>& blob, const MeshletBuildOptions& options,
                          MeshletBuildStats* stats, std::string* err)
{
    std::vector<uint8_t> chunk;
    if (!BuildMeshletChunk(blob.data(), blob.size(), options, chunk, stats, err)) return false;
    if (chunk.empty()) return RemoveMeshLibChunk(blob, MESHLIB_CHUNK_MLET, err);
    return WriteMeshLibChunk(blob, MESHLIB_CHUNK_MLET, MESHLIB_MLET_VERSION, chunk.data(), chunk.size(), err);
}

std::vector<uint8_t> StaticEntryMask(const MeshIdentity& id, uint32_t entryCount, bool modelNamesSkeleton)
{
    std::vector<uint8_t> listed(entryCount, 0), mask(entryCount, 0);
    for (const MeshIdentity::Mesh& m : id.meshes)
    {
        const bool skinned = MeshCountsAsSkinned(m.flags, id.flags, modelNamesSkeleton);
        for (uint32_t e : m.sections)
            if (e < entryCount)
            {
                listed[e] = 1;
                if (!skinned) mask[e] = 1;
            }
    }
    for (uint32_t e = 0; e < entryCount; ++e)
        if (!listed[e]) mask[e] = 1;
    return mask;
}

void LogMeshletBuild(const char* who, const std::string& path, const MeshletBuildStats& st, bool perEntry)
{
    LOG_INFO("[MESHLET] build who=%s path='%s' entries=%u built=%u clusters=%u lvl0Clusters=%u tris=%llu "
             "lvl0Tris=%llu levels=%u maxLevels=%u sloppy=%u bytes=%zu gpuBytes=%zu ms=%.1f",
             who, path.c_str(), st.entries, st.entriesBuilt, st.clusters, st.level0Clusters,
             static_cast<unsigned long long>(st.triangles), static_cast<unsigned long long>(st.level0Triangles),
             st.levelRecords, st.maxLevels, st.sloppyLevels, st.chunkBytes, st.gpuBytes, st.ms);
    if (!perEntry) return;
    for (size_t e = 0; e < st.perEntry.size(); ++e)
    {
        const MeshletEntryBuildInfo& i = st.perEntry[e];
        if (!i.built) continue;
        std::string lv, dmg;
        size_t shaded = 0;
        for (size_t l = 0; l < i.levelTriangles.size(); ++l)
        {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%s%u@%.4g", l ? "," : "", i.levelTriangles[l],
                          l < i.levelErrors.size() ? i.levelErrors[l] : 0.f);
            lv += buf;
            const float d = l < i.levelShadingDamage.size() ? i.levelShadingDamage[l] : 0.f;
            std::snprintf(buf, sizeof(buf), "%s%.1f", l ? "," : "", 100.f * d);
            dmg += buf;
            if (shaded == l && d <= kMeshletMaxShadingDamage) shaded = l + 1;
        }
        // shading = each level's shading-check damage in % of its area;
        // shaded = the levels the shaded view draws (the loader's cutoff).
        LOG_INFO("[MESHLET] build.entry path='%s' entry=%zu tris=%u radius=%.4g levels=%zu lvl=%s sloppy=%u stop='%s' "
                 "shading=%s shaded=%zu",
                 path.c_str(), e, i.triangles, i.radius, i.levelTriangles.size(), lv.c_str(), i.sloppyLevels,
                 i.stop.c_str(), dmg.c_str(), shaded);
    }
}
}
