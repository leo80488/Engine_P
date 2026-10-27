#include "Nav/NavMeshSystem.h"

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"  // GlobalTransform
#include "ECS/PhysicsComponents.h"    // ColliderComponent
#include "ECS/CharacterControllerComponent.h"   // the soup filter: a controller's collider makes no body
#include "Physics/PhysicsSystem.h"    // AppendColliderTriangles (all shape kinds)
#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"     // AssetHeader { magic, version, ... }
#include "System/Log.h"
#include "System/PathUtil.h"

// Recast / Detour, vendored under external/recastnavigation. Their include paths must be on the
// EngineCore include list:
//   external/recastnavigation/Recast/Include
//   external/recastnavigation/Detour/Include
// The .lib search paths live on the exe vcxprojs (Editor / Game) beside Jolt and meshoptimizer; the
// #pragma comment(lib) lines below pick the Debug or Release libs.
#include "Recast.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"

#include "RenderGraph/RenderPass/DebugWirePass.h"

#ifdef _DEBUG
#pragma comment(lib, "Recast-d.lib")
#pragma comment(lib, "Detour-d.lib")
#else
#pragma comment(lib, "Recast.lib")
#pragma comment(lib, "Detour.lib")
#endif

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace Nav
{

namespace {

// Recast routes diagnostics through rcContext::doLog. Wire it into the
// engine's async Logger so build issues surface in DX12Log.txt.
class LoggingContext : public rcContext
{
protected:
    void doLog(const rcLogCategory cat, const char* msg, const int /*len*/) override
    {
        switch (cat)
        {
            case RC_LOG_PROGRESS: LOG_INFO   ("[Recast] %s", msg); break;
            case RC_LOG_WARNING:  LOG_WARNING("[Recast] %s", msg); break;
            case RC_LOG_ERROR:    LOG_ERROR  ("[Recast] %s", msg); break;
            default:              LOG_INFO   ("[Recast] %s", msg); break;
        }
    }
};

// Keep only the triangles whose AABB overlaps a tile's XZ bounds (tiles are full-height columns).
// Without it every tile rasterises the whole soup: O(N^2).
void FilterTrianglesInBounds(const float* verts,
                              const uint32_t* tris, int triCount,
                              const float* bmin, const float* bmax,
                              std::vector<int>& outIdx)
{
    outIdx.clear();
    outIdx.reserve(static_cast<size_t>(triCount) * 3);
    for (int i = 0; i < triCount; ++i)
    {
        const uint32_t i0 = tris[i*3+0], i1 = tris[i*3+1], i2 = tris[i*3+2];
        const float* a = &verts[i0*3];
        const float* b = &verts[i1*3];
        const float* c = &verts[i2*3];
        const float xmin = std::min({ a[0], b[0], c[0] });
        const float xmax = std::max({ a[0], b[0], c[0] });
        const float zmin = std::min({ a[2], b[2], c[2] });
        const float zmax = std::max({ a[2], b[2], c[2] });
        if (xmax < bmin[0] || xmin > bmax[0]) continue;
        if (zmax < bmin[2] || zmin > bmax[2]) continue;
        outIdx.push_back(static_cast<int>(i0));
        outIdx.push_back(static_cast<int>(i1));
        outIdx.push_back(static_cast<int>(i2));
    }
}

uint32_t NextPow2(uint32_t v)
{
    if (v <= 1) return 1;
    --v; v|=v>>1; v|=v>>2; v|=v>>4; v|=v>>8; v|=v>>16;
    return v + 1;
}

int Ilog2(uint32_t v)
{
    int r = 0;
    while (v >>= 1) ++r;
    return r;
}

template <typename T>
void AppendLE(std::vector<uint8_t>& dst, const T& x)
{
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    dst.insert(dst.end(), p, p + sizeof(T));
}

} // namespace

NavMeshSystem::NavMeshSystem()  = default;
NavMeshSystem::~NavMeshSystem() { FreeAll(); }

void NavMeshSystem::FreeAll()
{
    if (m_navQuery) { dtFreeNavMeshQuery(m_navQuery); m_navQuery = nullptr; }
    ++m_queryGen;   // retire every per-thread query bound to the old mesh
    if (m_navMesh)  { dtFreeNavMesh(m_navMesh);       m_navMesh  = nullptr; }
    m_sourcePath.clear();
    m_navTileData.clear();
    m_navTileData.shrink_to_fit();
    m_tilesX = m_tilesY = 0;
    m_gridOrigin = { 0, 0, 0 };
    m_tileWidth = m_tileHeight = 0.f;
}

void NavMeshSystem::Clear()
{
    FreeAll();
}

bool NavMeshSystem::InitQuery()
{
    if (!m_navMesh) return false;
    if (m_navQuery) dtFreeNavMeshQuery(m_navQuery);
    ++m_queryGen;   // see FreeAll
    m_navQuery = dtAllocNavMeshQuery();
    if (!m_navQuery)
    {
        LOG_ERROR("NavMeshSystem: dtAllocNavMeshQuery returned null");
        return false;
    }
    const dtStatus s = m_navQuery->init(m_navMesh, 2048);
    if (dtStatusFailed(s))
    {
        LOG_ERROR("NavMeshSystem: dtNavMeshQuery::init failed (status=0x%x)", s);
        dtFreeNavMeshQuery(m_navQuery);
        m_navQuery = nullptr;
        return false;
    }
    return true;
}

BuildStats NavMeshSystem::Build(const TriangleSoup& soup, const BuildParams& bp)
{
    BuildStats stats;
    const auto t0 = std::chrono::steady_clock::now();

    if (soup.positions.empty() || soup.indices.size() < 3 || soup.indices.size() % 3 != 0)
    {
        stats.error = "empty or non-triangle soup";
        LOG_ERROR("NavMeshSystem::Build — %s", stats.error.c_str());
        return stats;
    }

    stats.inputVerts = static_cast<uint32_t>(soup.positions.size());
    stats.inputTris  = static_cast<uint32_t>(soup.indices.size() / 3);

    // Keep the input for the debug overlay: the soup is a stack local at every call site, and without
    // this copy only Recast's OUTPUT can be inspected after a bake, which hides a silently-empty input.
    // Cheap next to the build itself.
    m_debugSoup          = soup;
    m_debugSoupSlopeDeg  = bp.agentMaxSlopeDeg;

    // World bounds drive the tile grid + dtNavMesh origin.
    float worldBmin[3], worldBmax[3];
    rcCalcBounds(reinterpret_cast<const float*>(soup.positions.data()),
                 static_cast<int>(soup.positions.size()),
                 worldBmin, worldBmax);

    // Tile grid: tileSize <= 1 -> one 1x1 tile over the whole bounds (no borderSize); else a grid of
    // (tileSize x cellSize) tiles.
    const bool tiled = bp.tileSize > 1;
    float tileWorldSize;
    int tilesX, tilesY;
    if (tiled)
    {
        tileWorldSize = static_cast<float>(bp.tileSize) * bp.cellSize;
        tilesX = std::max(1, static_cast<int>(std::ceilf((worldBmax[0] - worldBmin[0]) / tileWorldSize)));
        tilesY = std::max(1, static_cast<int>(std::ceilf((worldBmax[2] - worldBmin[2]) / tileWorldSize)));
    }
    else
    {
        tilesX = tilesY = 1;
        tileWorldSize = std::max(worldBmax[0] - worldBmin[0],
                                  worldBmax[2] - worldBmin[2]);
        if (tileWorldSize < 1.f) tileWorldSize = 1.f;
    }
    const int borderSize = tiled
        ? static_cast<int>(std::ceilf(bp.agentRadius / bp.cellSize)) + 3
        : 0;
    stats.tilesTotal = static_cast<uint32_t>(tilesX * tilesY);

    // Detour splits 22 bits between tile and poly ids: the grid gets what it needs (at most 14), the
    // rest is the per-tile polygon limit.
    const int tileBits = std::min(Ilog2(NextPow2(static_cast<uint32_t>(tilesX * tilesY))), 14);
    const int polyBits = 22 - tileBits;

    FreeAll();

    m_navMesh = dtAllocNavMesh();
    if (!m_navMesh) { stats.error = "dtAllocNavMesh failed"; return stats; }
    dtNavMeshParams navParams{};
    navParams.orig[0]    = worldBmin[0];
    navParams.orig[1]    = worldBmin[1];
    navParams.orig[2]    = worldBmin[2];
    navParams.tileWidth  = tileWorldSize;
    navParams.tileHeight = tileWorldSize;
    navParams.maxTiles   = 1 << tileBits;
    navParams.maxPolys   = 1 << polyBits;
    if (dtStatusFailed(m_navMesh->init(&navParams)))
    {
        stats.error = "dtNavMesh::init (multi-tile) failed";
        FreeAll();
        return stats;
    }

    // Build every tile. Cache each tile's bytes (prefixed with tx/ty/size)
    // into m_navTileData for later Save().
    uint32_t builtCount = 0;
    for (int ty = 0; ty < tilesY; ++ty)
    for (int tx = 0; tx < tilesX; ++tx)
    {
        float tileBmin[3], tileBmax[3];
        tileBmin[0] = worldBmin[0] + tx * tileWorldSize;
        tileBmin[1] = worldBmin[1];
        tileBmin[2] = worldBmin[2] + ty * tileWorldSize;
        tileBmax[0] = tileBmin[0] + tileWorldSize;
        tileBmax[1] = worldBmax[1];
        tileBmax[2] = tileBmin[2] + tileWorldSize;
        if (!tiled) { tileBmax[0] = worldBmax[0]; tileBmax[2] = worldBmax[2]; }

        unsigned char* tileData = nullptr;
        int tileDataSize = 0;
        if (!BuildOneTile(soup, bp, tx, ty, tileBmin, tileBmax, borderSize,
                          &tileData, &tileDataSize, stats))
            continue;

        // Cache bytes (record: tx, ty, size, bytes) before Detour eats them.
        AppendLE<int32_t> (m_navTileData, tx);
        AppendLE<int32_t> (m_navTileData, ty);
        AppendLE<uint32_t>(m_navTileData, static_cast<uint32_t>(tileDataSize));
        const size_t old = m_navTileData.size();
        m_navTileData.resize(old + tileDataSize);
        std::memcpy(m_navTileData.data() + old, tileData, tileDataSize);

        const dtStatus s = m_navMesh->addTile(tileData, tileDataSize,
                                                DT_TILE_FREE_DATA, 0, nullptr);
        if (dtStatusFailed(s))
        {
            LOG_WARNING("NavMeshSystem: addTile failed (tx=%d, ty=%d, status=0x%x)",
                        tx, ty, s);
            dtFree(tileData);
            continue;
        }
        ++builtCount;
    }

    if (builtCount == 0)
    {
        stats.error = "no tile produced geometry";
        FreeAll();
        return stats;
    }
    if (!InitQuery())
    {
        stats.error = "InitQuery failed";
        FreeAll();
        return stats;
    }

    m_tilesX     = tilesX;
    m_tilesY     = tilesY;
    m_gridOrigin = { worldBmin[0], worldBmin[1], worldBmin[2] };
    m_tileWidth  = tileWorldSize;
    m_tileHeight = tileWorldSize;
    stats.tilesBuilt = builtCount;

    const auto t1 = std::chrono::steady_clock::now();
    stats.buildMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    stats.success = true;
    LOG_INFO("NavMeshSystem: built nav (%u verts %u tris in → grid %dx%d, "
             "%u/%u tiles built, %u polys total, %.1f ms)",
             stats.inputVerts, stats.inputTris,
             tilesX, tilesY, stats.tilesBuilt, stats.tilesTotal,
             stats.polyCount, stats.buildMs);
    return stats;
}

// BuildOneTile - the Recast pipeline for one tile slot (contract at the declaration).
bool NavMeshSystem::BuildOneTile(const TriangleSoup& soup,
                                  const BuildParams& bp,
                                  int tx, int ty,
                                  const float* tileBmin, const float* tileBmax,
                                  int borderSize,
                                  unsigned char** outNavData, int* outNavDataSize,
                                  BuildStats& running) const
{
    *outNavData     = nullptr;
    *outNavDataSize = 0;

    LoggingContext ctx;

    rcConfig cfg{};
    cfg.cs                     = bp.cellSize;
    cfg.ch                     = bp.cellHeight;
    cfg.walkableSlopeAngle     = bp.agentMaxSlopeDeg;
    cfg.walkableHeight         = static_cast<int>(std::ceilf (bp.agentHeight   / cfg.ch));
    cfg.walkableClimb          = static_cast<int>(std::floorf(bp.agentMaxClimb / cfg.ch));
    cfg.walkableRadius         = static_cast<int>(std::ceilf (bp.agentRadius   / cfg.cs));
    cfg.maxEdgeLen             = static_cast<int>(bp.maxEdgeLen);
    cfg.maxSimplificationError = bp.maxSimplifyError;
    cfg.minRegionArea          = bp.minRegionArea;
    cfg.mergeRegionArea        = bp.mergeRegionArea;
    cfg.maxVertsPerPoly        = bp.maxVertsPerPoly;
    cfg.detailSampleDist       = bp.detailSampleDist < 0.9f ? 0 : cfg.cs * bp.detailSampleDist;
    cfg.detailSampleMaxError   = cfg.ch * bp.detailSampleMaxError;
    cfg.borderSize             = borderSize;

    // Expand the tile bounds outward by borderSize cells so agent-radius
    // erosion has overlap room between neighbouring tiles.
    cfg.bmin[0] = tileBmin[0] - borderSize * cfg.cs;
    cfg.bmin[1] = tileBmin[1];
    cfg.bmin[2] = tileBmin[2] - borderSize * cfg.cs;
    cfg.bmax[0] = tileBmax[0] + borderSize * cfg.cs;
    cfg.bmax[1] = tileBmax[1];
    cfg.bmax[2] = tileBmax[2] + borderSize * cfg.cs;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    const int   nverts    = static_cast<int>(soup.positions.size());
    const float* verts    = reinterpret_cast<const float*>(soup.positions.data());
    const int   nFullTris = static_cast<int>(soup.indices.size() / 3);

    // For tiled builds: narrow the triangle list to those overlapping the
    // expanded tile bounds. Single-tile mode skips the filter.
    std::vector<int> tileTris;
    const int* tris = nullptr;
    int        ntris = 0;
    if (borderSize > 0)
    {
        FilterTrianglesInBounds(verts, soup.indices.data(), nFullTris,
                                 cfg.bmin, cfg.bmax, tileTris);
        if (tileTris.empty()) return false;
        tris  = tileTris.data();
        ntris = static_cast<int>(tileTris.size() / 3);
    }
    else
    {
        tris  = reinterpret_cast<const int*>(soup.indices.data());
        ntris = nFullTris;
    }

    rcHeightfield* hf = rcAllocHeightfield();
    if (!hf || !rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height,
                                     cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
    { rcFreeHeightField(hf); return false; }

    std::vector<unsigned char> triAreas(ntris, 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, nverts,
                            tris, ntris, triAreas.data());
    if (!rcRasterizeTriangles(&ctx, verts, nverts, tris, triAreas.data(),
                              ntris, *hf, cfg.walkableClimb))
    { rcFreeHeightField(hf); return false; }

    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
    rcFilterLedgeSpans                  (&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans      (&ctx, cfg.walkableHeight, *hf);

    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight,
                                            cfg.walkableClimb, *hf, *chf))
    { rcFreeHeightField(hf); rcFreeCompactHeightfield(chf); return false; }
    rcFreeHeightField(hf);

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf) ||
        !rcBuildDistanceField(&ctx, *chf) ||
        !rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
    { rcFreeCompactHeightfield(chf); return false; }

    rcContourSet* cset = rcAllocContourSet();
    if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError,
                                   cfg.maxEdgeLen, *cset))
    { rcFreeContourSet(cset); rcFreeCompactHeightfield(chf); return false; }

    rcPolyMesh* pmesh = rcAllocPolyMesh();
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh) ||
        pmesh->npolys == 0)
    {
        rcFreePolyMesh(pmesh);
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        return false;
    }

    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf,
                                          cfg.detailSampleDist,
                                          cfg.detailSampleMaxError, *dmesh))
    {
        rcFreePolyMeshDetail(dmesh);
        rcFreePolyMesh(pmesh);
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        return false;
    }

    for (int i = 0; i < pmesh->npolys; ++i)
        if (pmesh->areas[i] == RC_WALKABLE_AREA)
            pmesh->flags[i] = 0x01;

    dtNavMeshCreateParams params{};
    params.verts            = pmesh->verts;
    params.vertCount        = pmesh->nverts;
    params.polys            = pmesh->polys;
    params.polyAreas        = pmesh->areas;
    params.polyFlags        = pmesh->flags;
    params.polyCount        = pmesh->npolys;
    params.nvp              = pmesh->nvp;
    params.detailMeshes     = dmesh->meshes;
    params.detailVerts      = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris       = dmesh->tris;
    params.detailTriCount   = dmesh->ntris;
    params.walkableHeight   = bp.agentHeight;
    params.walkableRadius   = bp.agentRadius;
    params.walkableClimb    = bp.agentMaxClimb;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs               = cfg.cs;
    params.ch               = cfg.ch;
    params.buildBvTree      = true;
    params.tileX            = tx;
    params.tileY            = ty;
    params.tileLayer        = 0;

    const bool ok = dtCreateNavMeshData(&params, outNavData, outNavDataSize);
    if (ok)
    {
        running.polyCount += static_cast<uint32_t>(pmesh->npolys);
        running.vertCount += static_cast<uint32_t>(pmesh->nverts);
    }

    rcFreePolyMeshDetail(dmesh);
    rcFreePolyMesh(pmesh);
    rcFreeContourSet(cset);
    rcFreeCompactHeightfield(chf);
    return ok && *outNavData != nullptr && *outNavDataSize > 0;
}

// ==== .inav serialisation ====
// Version 2, multi-tile; a single-tile build writes it too (tileCount = 1) so Load has one path:
//   [AssetHeader       (24 B)]
//   [NavMeshMetadataV2 (48 B)]
//   [tileCount x record: int32 tx, int32 ty, uint32 size, byte[size] tileData]
// m_navTileData already holds the record stream; Save writes the header and metadata in front.

struct NavMeshMetadataV2
{
    uint32_t version;        // 2
    uint32_t tileCount;
    int32_t  tilesX;
    int32_t  tilesY;
    float    origin[3];
    float    tileWidth;
    float    tileHeight;
    uint32_t maxTiles;       // dtNavMeshParams.maxTiles
    uint32_t maxPolys;       // dtNavMeshParams.maxPolys
    uint32_t reserved;
};
static_assert(sizeof(NavMeshMetadataV2) == 48, "NavMeshMetadataV2 layout drift");

bool NavMeshSystem::Save(const std::string& path)
{
    if (m_navTileData.empty() || m_tilesX <= 0 || m_tilesY <= 0)
    {
        LOG_ERROR("NavMeshSystem::Save — no tile data cached "
                  "(call Build or Load first)");
        return false;
    }

    // Count tiles by walking m_navTileData's [tx][ty][size][bytes] records.
    uint32_t tileCount = 0;
    {
        size_t cursor = 0;
        while (cursor + 12 <= m_navTileData.size())
        {
            uint32_t sz = 0;
            std::memcpy(&sz, &m_navTileData[cursor + 8], sizeof(uint32_t));
            cursor += 12 + sz;
            ++tileCount;
        }
    }

    // UTF-8-safe parent-dir creation (fs::path(path) would narrow via ACP and
    // mangle non-ASCII save paths).
    Sys::CreateParentDirs(path);

    const int tileBits = std::min(Ilog2(NextPow2(static_cast<uint32_t>(m_tilesX * m_tilesY))), 14);
    const int polyBits = 22 - tileBits;

    NavMeshMetadataV2 meta{};
    meta.version    = 2;
    meta.tileCount  = tileCount;
    meta.tilesX     = m_tilesX;
    meta.tilesY     = m_tilesY;
    meta.origin[0]  = m_gridOrigin.x;
    meta.origin[1]  = m_gridOrigin.y;
    meta.origin[2]  = m_gridOrigin.z;
    meta.tileWidth  = m_tileWidth;
    meta.tileHeight = m_tileHeight;
    meta.maxTiles   = 1u << tileBits;
    meta.maxPolys   = 1u << polyBits;

    Resource::AssetHeader hdr{};
    hdr.magic        = MAGIC_NAVMESH;
    hdr.version      = Resource::CurrentVersion(MAGIC_NAVMESH);
    hdr.resourceType = 0;
    hdr.metadataSize = sizeof(NavMeshMetadataV2);
    hdr.dataSize     = static_cast<uint32_t>(m_navTileData.size());

    std::ofstream f = Sys::OpenOfstream(path, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        LOG_ERROR("NavMeshSystem::Save — cannot open '%s' for writing", path.c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(&hdr),  sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
    f.write(reinterpret_cast<const char*>(m_navTileData.data()),
            static_cast<std::streamsize>(m_navTileData.size()));

    if (f.good()) m_sourcePath = path;
    LOG_INFO("NavMeshSystem: saved '%s' (%u tiles, %zu bytes)",
             path.c_str(), tileCount, m_navTileData.size());
    return f.good();
}

bool NavMeshSystem::Load(const std::string& path)
{
    std::vector<uint8_t> blob;
    if (!Resource::AssetFS::Get().ReadFile(path, blob) || blob.empty())
    {
        LOG_ERROR("NavMeshSystem::Load — cannot read '%s'", path.c_str());
        return false;
    }
    if (!Resource::ValidateHeader(blob.data(), blob.size(), MAGIC_NAVMESH))
    {
        LOG_ERROR("NavMeshSystem::Load — bad header in '%s'", path.c_str());
        return false;
    }

    const Resource::AssetHeader* hdr = Resource::GetHeader(blob.data());
    if (hdr->metadataSize < sizeof(NavMeshMetadataV2))
    {
        LOG_ERROR("NavMeshSystem::Load — file too old (v2 .inav required); re-bake");
        return false;
    }
    const auto* meta = Resource::GetMetadata<NavMeshMetadataV2>(blob.data());
    if (meta->version != 2)
    {
        LOG_ERROR("NavMeshSystem::Load — unsupported .inav version %u", meta->version);
        return false;
    }

    FreeAll();

    m_navMesh = dtAllocNavMesh();
    if (!m_navMesh)
    {
        LOG_ERROR("NavMeshSystem::Load — dtAllocNavMesh failed");
        return false;
    }
    dtNavMeshParams navParams{};
    navParams.orig[0]    = meta->origin[0];
    navParams.orig[1]    = meta->origin[1];
    navParams.orig[2]    = meta->origin[2];
    navParams.tileWidth  = meta->tileWidth;
    navParams.tileHeight = meta->tileHeight;
    navParams.maxTiles   = meta->maxTiles;
    navParams.maxPolys   = meta->maxPolys;
    if (dtStatusFailed(m_navMesh->init(&navParams)))
    {
        LOG_ERROR("NavMeshSystem::Load — dtNavMesh::init failed");
        FreeAll();
        return false;
    }

    // Walk the per-tile records: [int32 tx][int32 ty][uint32 size][bytes].
    const uint8_t* payload     = Resource::GetPayload(blob.data());
    const uint32_t payloadSize = hdr->dataSize;
    uint32_t loaded = 0;
    for (size_t cursor = 0; cursor + 12 <= payloadSize; )
    {
        int32_t  tx = 0, ty = 0;
        uint32_t sz = 0;
        std::memcpy(&tx, payload + cursor + 0, sizeof(int32_t));
        std::memcpy(&ty, payload + cursor + 4, sizeof(int32_t));
        std::memcpy(&sz, payload + cursor + 8, sizeof(uint32_t));
        cursor += 12;
        if (cursor + sz > payloadSize) break;

        unsigned char* tileData =
            static_cast<unsigned char*>(dtAlloc(sz, DT_ALLOC_PERM));
        if (!tileData)
        {
            LOG_ERROR("NavMeshSystem::Load — dtAlloc(%u) failed", sz);
            cursor += sz;
            continue;
        }
        std::memcpy(tileData, payload + cursor, sz);
        cursor += sz;

        if (dtStatusFailed(m_navMesh->addTile(tileData, static_cast<int>(sz),
                                                DT_TILE_FREE_DATA, 0, nullptr)))
        {
            dtFree(tileData);
            continue;
        }
        ++loaded;
        (void)tx; (void)ty;   // Detour reads tile coords from the tile bytes' header
    }

    if (loaded == 0)
    {
        LOG_ERROR("NavMeshSystem::Load — no tiles loaded");
        FreeAll();
        return false;
    }
    if (!InitQuery())
    {
        FreeAll();
        return false;
    }

    m_navTileData.assign(payload, payload + payloadSize);
    m_tilesX     = meta->tilesX;
    m_tilesY     = meta->tilesY;
    m_gridOrigin = { meta->origin[0], meta->origin[1], meta->origin[2] };
    m_tileWidth  = meta->tileWidth;
    m_tileHeight = meta->tileHeight;
    m_sourcePath = path;

    LOG_INFO("NavMeshSystem: loaded '%s' (%u tiles, %dx%d grid, %u bytes)",
             path.c_str(), loaded, m_tilesX, m_tilesY, payloadSize);
    return true;
}


// ==== Queries ====

dtNavMeshQuery* NavMeshSystem::ThreadQuery() const
{
    if (!m_navMesh) return nullptr;

    // One slot per thread. The destructor runs at thread exit and frees only
    // the query's own memory, so it stays valid even if the mesh is long gone.
    struct Slot
    {
        dtNavMeshQuery* q    = nullptr;
        uint32_t        gen  = 0;
        const dtNavMesh* mesh = nullptr;
        ~Slot() { if (q) dtFreeNavMeshQuery(q); }
    };
    thread_local Slot t_slot;

    if (t_slot.q && (t_slot.gen != m_queryGen || t_slot.mesh != m_navMesh))
    {
        dtFreeNavMeshQuery(t_slot.q);
        t_slot.q = nullptr;
    }
    if (!t_slot.q)
    {
        t_slot.q = dtAllocNavMeshQuery();
        if (!t_slot.q) return nullptr;
        // The same node budget (2048) as the shared query in InitQuery: a smaller pool would fail long
        // paths on workers only, the worst kind of difference to debug.
        if (dtStatusFailed(t_slot.q->init(m_navMesh, 2048)))
        {
            dtFreeNavMeshQuery(t_slot.q);
            t_slot.q = nullptr;
            return nullptr;
        }
        t_slot.gen  = m_queryGen;
        t_slot.mesh = m_navMesh;
    }
    return t_slot.q;
}

PathResult NavMeshSystem::FindPath(const DirectX::XMFLOAT3& from,
                                    const DirectX::XMFLOAT3& to,
                                    const DirectX::XMFLOAT3& searchExtents) const
{
    PathResult out;
    // Per-thread query: NavAgentTick fans this call across workers.
    dtNavMeshQuery* q = ThreadQuery();
    if (!m_navMesh || !q) return out;

    const float startPos[3] = { from.x, from.y, from.z };
    const float endPos  [3] = { to.x,   to.y,   to.z   };
    const float halfExt [3] = { searchExtents.x, searchExtents.y, searchExtents.z };

    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);

    dtPolyRef startRef = 0, endRef = 0;
    float startNearest[3] = {0,0,0}, endNearest[3] = {0,0,0};
    q->findNearestPoly(startPos, halfExt, &filter, &startRef, startNearest);
    q->findNearestPoly(endPos,   halfExt, &filter, &endRef,   endNearest);
    if (!startRef || !endRef) return out;

    constexpr int kMaxPolys = 256;
    dtPolyRef polyPath[kMaxPolys];
    int polyCount = 0;
    const dtStatus s = q->findPath(startRef, endRef, startNearest, endNearest,
                                              &filter, polyPath, &polyCount, kMaxPolys);
    if (dtStatusFailed(s) || polyCount == 0) return out;

    constexpr int kMaxStraight = 256;
    float          straight[kMaxStraight * 3];
    unsigned char  straightFlags[kMaxStraight];
    dtPolyRef      straightRefs[kMaxStraight];
    int            straightCount = 0;
    const dtStatus s2 = q->findStraightPath(startNearest, endNearest,
                                                       polyPath, polyCount,
                                                       straight, straightFlags, straightRefs,
                                                       &straightCount, kMaxStraight);
    if (dtStatusFailed(s2)) return out;

    out.waypoints.reserve(straightCount);
    for (int i = 0; i < straightCount; ++i)
    {
        out.waypoints.push_back({ straight[i*3+0], straight[i*3+1], straight[i*3+2] });
    }
    out.partial = (polyPath[polyCount - 1] != endRef);
    out.valid   = true;
    return out;
}

bool NavMeshSystem::Raycast(const DirectX::XMFLOAT3& from,
                             const DirectX::XMFLOAT3& to,
                             DirectX::XMFLOAT3* outHit,
                             const DirectX::XMFLOAT3& searchExtents) const
{
    // Per-thread query (ThreadQuery): Lua and the parallel agent tick call this.
    dtNavMeshQuery* q = ThreadQuery();
    if (!m_navMesh || !q) return false;

    const float startPos[3] = { from.x, from.y, from.z };
    const float halfExt [3] = { searchExtents.x, searchExtents.y, searchExtents.z };
    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);

    dtPolyRef startRef = 0;
    float startNearest[3] = {0,0,0};
    q->findNearestPoly(startPos, halfExt, &filter, &startRef, startNearest);
    if (!startRef) return false;

    const float endPos[3] = { to.x, to.y, to.z };
    float       t = 0;
    float       hitNormal[3] = {0,0,0};
    constexpr int kMaxPolys = 64;
    dtPolyRef   visited[kMaxPolys];
    int         visitedCount = 0;
    const dtStatus s = q->raycast(startRef, startNearest, endPos, &filter,
                                              &t, hitNormal, visited, &visitedCount, kMaxPolys);
    if (dtStatusFailed(s)) return false;

    const bool hit = (t < 1.0f);
    if (outHit)
    {
        if (hit)
        {
            outHit->x = startNearest[0] + (endPos[0] - startNearest[0]) * t;
            outHit->y = startNearest[1] + (endPos[1] - startNearest[1]) * t;
            outHit->z = startNearest[2] + (endPos[2] - startNearest[2]) * t;
        }
        else *outHit = to;
    }
    return !hit;
}

// ==== Debug visualisation ====
void NavMeshSystem::EmitSoupDebugLines(DebugWirePass&           dbg,
                                       const DirectX::XMFLOAT3& viewPos,
                                       float                    maxDistance,
                                       uint32_t                 walkableColor,
                                       uint32_t                 steepColor) const
{
    const std::vector<DirectX::XMFLOAT3>& P = m_debugSoup.positions;
    const std::vector<uint32_t>&          I = m_debugSoup.indices;
    if (P.empty() || I.size() < 3) return;

    // The same test rcMarkWalkableTriangles applies: the RIGHT-handed face normal (p1-p0) x (p2-p0),
    // normalised, is walkable when its Y clears the slope threshold. Match it exactly: the colour then
    // IS Recast's verdict, not an approximation.
    const float walkableThr = std::cos(m_debugSoupSlopeDeg * 3.14159265f / 180.0f);
    const float maxDistSq   = maxDistance > 0.0f ? maxDistance * maxDistance : 0.0f;

    uint32_t drawn = 0, culled = 0, steep = 0;
    for (size_t t = 0; t + 2 < I.size(); t += 3)
    {
        const uint32_t i0 = I[t], i1 = I[t+1], i2 = I[t+2];
        if (i0 >= P.size() || i1 >= P.size() || i2 >= P.size()) continue;
        const DirectX::XMFLOAT3& a = P[i0];
        const DirectX::XMFLOAT3& b = P[i1];
        const DirectX::XMFLOAT3& c = P[i2];

        if (maxDistSq > 0.0f)
        {
            const float cx = (a.x + b.x + c.x) / 3.0f - viewPos.x;
            const float cy = (a.y + b.y + c.y) / 3.0f - viewPos.y;
            const float cz = (a.z + b.z + c.z) / 3.0f - viewPos.z;
            if (cx*cx + cy*cy + cz*cz > maxDistSq) { ++culled; continue; }
        }

        const float e0x = b.x - a.x, e0y = b.y - a.y, e0z = b.z - a.z;
        const float e1x = c.x - a.x, e1y = c.y - a.y, e1z = c.z - a.z;
        const float nx = e0y*e1z - e0z*e1y;
        const float ny = e0z*e1x - e0x*e1z;
        const float nz = e0x*e1y - e0y*e1x;
        const float len = std::sqrt(nx*nx + ny*ny + nz*nz);
        const bool walkable = (len > 1e-12f) && ((ny / len) > walkableThr);
        if (!walkable) ++steep;

        const uint32_t col = walkable ? walkableColor : steepColor;
        dbg.AddLine(a, b, col);
        dbg.AddLine(b, c, col);
        dbg.AddLine(c, a, col);
        ++drawn;
    }

    static uint32_t s_lastDrawn = 0xFFFFFFFFu;
    if (drawn != s_lastDrawn)
    {
        s_lastDrawn = drawn;
        LOG_INFO("Nav: soup overlay — %u tris drawn (%u walkable, %u steep), %u distance-culled",
                 drawn, drawn - steep, steep, culled);
    }
}

void NavMeshSystem::EmitDebugLines(DebugWirePass& dbg,
                                    uint32_t edgeColor,
                                    uint32_t boundaryColor,
                                    float    yOffset) const
{
    if (!m_navMesh || m_tilesX <= 0 || m_tilesY <= 0) return;

    for (int ty = 0; ty < m_tilesY; ++ty)
    for (int tx = 0; tx < m_tilesX; ++tx)
    {
        const dtMeshTile* tile = m_navMesh->getTileAt(tx, ty, 0);
        if (!tile || !tile->header) continue;

        const dtMeshHeader* h = tile->header;
        for (int i = 0; i < h->polyCount; ++i)
        {
            const dtPoly& p = tile->polys[i];
            if (p.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                continue;

            const int vc = p.vertCount;
            for (int j = 0; j < vc; ++j)
            {
                const int jn = (j + 1) % vc;
                const float* va = &tile->verts[p.verts[j]  * 3];
                const float* vb = &tile->verts[p.verts[jn] * 3];

                // p.neis[j] is the neighbouring poly (interior) or 0 (boundary, no neighbour). An edge linking to
                // another tile has DT_EXT_LINK set: drawn as boundary too.
                const bool isBoundary = (p.neis[j] == 0) || (p.neis[j] & DT_EXT_LINK);
                const uint32_t color  = isBoundary ? boundaryColor : edgeColor;

                dbg.AddLine(
                    DirectX::XMFLOAT3{ va[0], va[1] + yOffset, va[2] },
                    DirectX::XMFLOAT3{ vb[0], vb[1] + yOffset, vb[2] },
                    color);
            }
        }
    }
}

bool NavMeshSystem::ProjectToMesh(const DirectX::XMFLOAT3& worldPos,
                                   DirectX::XMFLOAT3& outOnMesh,
                                   const DirectX::XMFLOAT3& searchExtents) const
{
    // Per-thread query (ThreadQuery): Lua and the parallel agent tick call this.
    dtNavMeshQuery* q = ThreadQuery();
    if (!m_navMesh || !q) return false;

    const float pos[3]     = { worldPos.x, worldPos.y, worldPos.z };
    const float halfExt[3] = { searchExtents.x, searchExtents.y, searchExtents.z };
    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);

    dtPolyRef ref = 0;
    float nearest[3] = {0,0,0};
    q->findNearestPoly(pos, halfExt, &filter, &ref, nearest);
    if (!ref) return false;
    outOnMesh = { nearest[0], nearest[1], nearest[2] };
    return true;
}

// ==== World -> soup ====

namespace {

// Decode positions + indices for one entry in a .meshlib blob. Indices come
// back re-based into [0, vertexCount) so callers can offset them when
// concatenating into a bigger soup.
bool DecodeMeshLibSlice(const std::vector<uint8_t>& blob, uint32_t meshId,
                        std::vector<DirectX::XMFLOAT3>& outPositions,
                        std::vector<uint32_t>&          outIndices)
{
    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESHLIB))
        return false;
    const auto* meta = Resource::GetMetadata<Resource::MeshLibraryMetadata>(blob.data());
    if (meshId >= meta->meshCount) return false;
    const uint8_t* payload = Resource::GetPayload(blob.data());
    const auto*    entries = reinterpret_cast<const Resource::MeshLibraryEntry*>(payload);
    const auto&    entry   = entries[meshId];
    if (entry.vertexCount == 0 || entry.indexCount == 0) return false;
    const uint32_t vStride = meta->vertexStride;
    const uint8_t* vbBase  = payload + size_t(meta->meshCount) * sizeof(Resource::MeshLibraryEntry);
    const uint8_t* ibBase  = vbBase + size_t(meta->vertexCount) * vStride;
    const uint8_t* vsBase  = vbBase + size_t(entry.vertexStart) * vStride;
    const auto*    idxData = reinterpret_cast<const uint32_t*>(ibBase) + entry.indexStart;
    outPositions.resize(entry.vertexCount);
    for (uint32_t i = 0; i < entry.vertexCount; ++i)
        std::memcpy(&outPositions[i], vsBase + size_t(i) * vStride, sizeof(DirectX::XMFLOAT3));
    outIndices.resize(entry.indexCount);
    for (uint32_t i = 0; i < entry.indexCount; ++i)
        outIndices[i] = idxData[i] - entry.vertexStart;
    return true;
}

} // namespace

bool BuildSoupFromWorldColliders(::World& world, TriangleSoup& outSoup,
                                 DX12Physics::PhysicsSystem* physics, SoupStats* outStats)
{
    outSoup.positions.clear();
    outSoup.indices.clear();
    SoupStats localStats;
    SoupStats& st = outStats ? *outStats : localStats;
    st = SoupStats{};

    // Only a static, solid body of the static world goes in (see SoupStats in the header). The layer is
    // read the way PhysicsSystem resolves it: Auto on a static, non-trigger body IS StaticEnv. A Mesh
    // collider's body is always Static (PhysicsSystem coerces it and writes Static back).
    auto isGround = [&](Entity e, const ColliderComponent& c) -> bool
    {
        const RigidBodyComponent* rb = world.GetComponent<RigidBodyComponent>(e);
        if (!rb || world.HasComponent<CharacterControllerComponent>(e)) { ++st.noBody; return false; }
        if (c.isTrigger) { ++st.trigger; return false; }
        if (rb->motion != RigidBodyComponent::Motion::Static && c.shape != ColliderComponent::Shape::Mesh)
        {
            ++st.moving;
            return false;
        }
        if (c.layer != ColliderComponent::CollisionLayer::Auto &&
            c.layer != ColliderComponent::CollisionLayer::StaticEnv) { ++st.layer; return false; }
        return true;
    };

    // ---- Preferred path: ask physics for the real collision triangles ----
    // Covers every shape kind and honours the collider's offset / local rotation / entity scale, because
    // it reuses the exact shape the body is built from. It is the only way to get a ConvexHull's
    // SURFACE: the indices beside the hull points in a .meshlib are a synthetic fan (so the entry stays
    // decodable), not the hull.
    if (physics)
    {
        uint32_t emittedP = 0, failedP = 0;
        uint32_t (&byShape)[6] = st.byShape;
        world.ForEach<ColliderComponent>([&](Entity e, ColliderComponent& c)
        {
            if (!isGround(e, c)) return;
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            const DirectX::XMMATRIX M =
                gt ? DirectX::XMLoadFloat4x4(&gt->matrix) : DirectX::XMMatrixIdentity();
            if (physics->AppendColliderTriangles(c, M, outSoup.positions, outSoup.indices))
            {
                ++emittedP;
                const int s = static_cast<int>(c.shape);
                if (s >= 0 && s < 6) ++byShape[s];
            }
            else ++failedP;
        });
        st.colliders = emittedP;
        st.failed    = failedP;
        LOG_INFO("Nav: BuildSoupFromWorldColliders (physics shapes) — %u colliders "
                 "(box %u, sphere %u, capsule %u, mesh %u, hull %u, asset %u), %u produced NO "
                 "geometry → %zu verts, %zu tris; left out: %u trigger, %u moving, %u other layer, "
                 "%u without a body",
                 emittedP, byShape[0], byShape[1], byShape[2], byShape[3], byShape[4], byShape[5],
                 failedP, outSoup.positions.size(), outSoup.indices.size() / 3,
                 st.trigger, st.moving, st.layer, st.noBody);
        // Loud, not just a stat: a collider that silently contributes nothing leaves a hole in the navmesh.
        if (failedP > 0)
            LOG_WARNING("Nav: %u collider(s) contributed no triangles to the soup — "
                        "the navmesh will have holes where they are", failedP);
        return !outSoup.positions.empty() && !outSoup.indices.empty();
    }

    LOG_WARNING("Nav: BuildSoupFromWorldColliders called without a PhysicsSystem — "
                "only Mesh colliders will contribute (ConvexHull/Box/Sphere/Capsule "
                "colliders are invisible on this path)");

    // Cache .meshlib bytes per path: many colliders share one file (Bistro: 1591 colliders, one file).
    std::unordered_map<std::string, std::vector<uint8_t>> blobs;
    auto getBlob = [&](const std::string& path) -> const std::vector<uint8_t>*
    {
        auto it = blobs.find(path);
        if (it != blobs.end()) return &it->second;
        std::vector<uint8_t> fresh;
        if (!Resource::AssetFS::Get().ReadFile(path, fresh) || fresh.empty()) return nullptr;
        it = blobs.emplace(path, std::move(fresh)).first;
        return &it->second;
    };

    uint32_t emitted = 0;
    world.ForEach<ColliderComponent>([&](Entity e, ColliderComponent& c)
    {
        if (c.shape != ColliderComponent::Shape::Mesh) return;
        if (c.meshLibPath.empty()) return;
        if (!isGround(e, c)) return;
        const auto* blob = getBlob(c.meshLibPath);
        if (!blob) return;

        std::vector<DirectX::XMFLOAT3> localPositions;
        std::vector<uint32_t>          localIndices;
        if (!DecodeMeshLibSlice(*blob, c.meshLibMeshId, localPositions, localIndices))
            return;

        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
        using namespace DirectX;
        const XMMATRIX M = gt ? XMLoadFloat4x4(&gt->matrix) : XMMatrixIdentity();

        const uint32_t vBase = static_cast<uint32_t>(outSoup.positions.size());
        outSoup.positions.reserve(outSoup.positions.size() + localPositions.size());
        for (const auto& p : localPositions)
        {
            XMVECTOR v = XMVectorSet(p.x, p.y, p.z, 1.f);
            v = XMVector3Transform(v, M);
            XMFLOAT3 wp;
            XMStoreFloat3(&wp, v);
            outSoup.positions.push_back(wp);
        }
        outSoup.indices.reserve(outSoup.indices.size() + localIndices.size());
        for (uint32_t idx : localIndices)
            outSoup.indices.push_back(idx + vBase);
        ++emitted;
        ++st.byShape[static_cast<int>(ColliderComponent::Shape::Mesh)];
    });
    st.colliders = emitted;

    LOG_INFO("Nav: BuildSoupFromWorldColliders — %u entities → %zu verts, %zu tris",
             emitted, outSoup.positions.size(), outSoup.indices.size() / 3);
    return !outSoup.positions.empty() && !outSoup.indices.empty();
}

} // namespace Nav
