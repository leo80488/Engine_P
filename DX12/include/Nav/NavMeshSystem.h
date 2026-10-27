#pragma once

// NavMeshSystem - Recast (build) + Detour (runtime query) wrapper. One system owns the active
// navmesh:
//   Build(soup, params)        bake from a triangle soup (BuildSoupFromWorldColliders, Nav.BuildSurface)
//   Save(path) / Load(path)    the .inav asset: AssetHeader + metadata + per-tile Detour data
//   FindPath / Raycast / ProjectToMesh   queries on a per-thread dtNavMeshQuery (ThreadQuery)
// Coordinates: Y-up, right-handed world space, as Jolt and the rest of the engine.

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;
class DebugWirePass;
class World;   // global ::World
namespace DX12Physics { class PhysicsSystem; }

namespace Nav
{

// File format: .inav
inline constexpr uint32_t MAGIC_NAVMESH = 'RNAV';           // "RNAV" 4CC

struct BuildParams
{
    // Voxelisation cell size. Smaller = more accurate / slower. Default fine
    // for human-scale geometry.
    float cellSize          = 0.30f;
    float cellHeight        = 0.20f;

    // Agent capsule footprint. Anything taller / wider needs a custom build.
    float agentHeight       = 2.00f;
    float agentRadius       = 0.60f;
    float agentMaxClimb     = 0.90f;
    float agentMaxSlopeDeg  = 45.0f;

    // Mesh simplification.
    int   maxEdgeLen        = 12;       // cells
    float maxSimplifyError  = 1.30f;
    int   minRegionArea     = 8 * 8;    // cells^2
    int   mergeRegionArea   = 20 * 20;  // cells^2

    int   maxVertsPerPoly   = 6;
    float detailSampleDist  = 6.0f;     // multiples of cellSize
    float detailSampleMaxError = 1.0f;  // multiples of cellHeight

    // Tile size in cells. 0 or 1 = one tile over the whole world; 32 / 64 / 128 = tiled: Recast slices
    // the world into a grid of (tileSize x cellSize) world units. >= 64 for worlds over 1 km^2; 0 for
    // small levels (faster, less per-tile overhead).
    int   tileSize          = 0;
};

struct BuildStats
{
    bool     success      = false;
    uint32_t inputVerts   = 0;
    uint32_t inputTris    = 0;
    uint32_t polyCount    = 0;   // summed across every successfully-built tile
    uint32_t vertCount    = 0;   // summed across every successfully-built tile
    uint32_t tilesTotal   = 0;   // tilesX x tilesY (whole grid, including empty)
    uint32_t tilesBuilt   = 0;   // tiles that produced non-empty geometry
    float    buildMs      = 0.0f;
    std::string error;
};

// Triangles to feed Recast; the caller assembles them from any source.
struct TriangleSoup
{
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t>          indices;  // length must be a multiple of 3
};

// BuildSoupFromWorldColliders (below): collect a world-space navmesh input soup from the world's
// colliders - what the editor's Build NavMesh feeds NavMeshSystem::Build.
//
// With @p physics every collider (Box, Sphere, Capsule, ConvexHull, Mesh) contributes the exact
// triangles Jolt collides against (offset / local rotation / scale included). Without it only Mesh
// colliders are read off their .meshlib (each file read once), which on an imported level is almost
// nothing: Unreal authors collision as convex hulls, so a nullptr bakes a navmesh that ignores every
// building and wall.
//
// Only what a navmesh stands on goes in: a collider with a RigidBody (a Collider alone makes no
// body; one on a CharacterController makes none either: the controller wins), not a trigger, on a
// Static body (a door or a crate moves - baked in place it would be a wall for good; a Mesh collider
// is always Static), on layer Auto or StaticEnv (a HurtBox or a Character-layer body is not ground).
//
// @p outStats (optional) counts what went in, by ColliderComponent::Shape, the colliders that gave
// nothing, and what the filter left out (Lua Nav.SoupStats).
struct SoupStats
{
    uint32_t colliders   = 0;     // contributed triangles
    uint32_t failed      = 0;     // contributed none
    uint32_t byShape[6]  = {};    // Box, Sphere, Capsule, Mesh, ConvexHull, MeshAsset
    uint32_t noBody      = 0;     // left out: no RigidBody, or a CharacterController's
    uint32_t trigger     = 0;     // left out: a trigger
    uint32_t moving      = 0;     // left out: a Dynamic / Kinematic body
    uint32_t layer       = 0;     // left out: a layer other than Auto / StaticEnv
};
bool BuildSoupFromWorldColliders(::World&                world,
                                 TriangleSoup&           outSoup,
                                 DX12Physics::PhysicsSystem* physics = nullptr,
                                 SoupStats*              outStats = nullptr);

// Path returned by FindPath.
struct PathResult
{
    std::vector<DirectX::XMFLOAT3> waypoints;
    bool partial = false;   // true if endRef wasn't reached (path truncated)
    bool valid   = false;   // false if start or end couldn't be projected onto mesh
};

class NavMeshSystem
{
public:
    NavMeshSystem();
    ~NavMeshSystem();

    NavMeshSystem(const NavMeshSystem&)            = delete;
    NavMeshSystem& operator=(const NavMeshSystem&) = delete;

    // Build a navmesh from @p soup: one tile, or a grid when params.tileSize > 1. Replaces the active
    // mesh; a build that fails after validating the soup leaves none. Synchronous: tens of ms to a few
    // seconds for room- to building-scale input.
    BuildStats Build(const TriangleSoup& soup, const BuildParams& params);

    // Serialise the active navmesh to @p path. On success SourcePath() becomes @p path, so SaveScene
    // can persist the .inav reference in the .iscene's W line.
    bool Save(const std::string& path);

    // Replace the active navmesh from <path>. Returns false if the file is
    // missing / malformed.
    bool Load(const std::string& path);

    void Clear();

    bool IsReady() const { return m_navMesh && m_navQuery; }

    // Path query. Both `from` and `to` are projected onto the nearest mesh
    // polygon within `searchExtents` (per-axis half-extents). Returns the
    // straight-path waypoints; first one is `from` projected, last is `to`
    // projected (or wherever the path truncates if unreachable).
    PathResult FindPath(const DirectX::XMFLOAT3& from,
                        const DirectX::XMFLOAT3& to,
                        const DirectX::XMFLOAT3& searchExtents = { 2.0f, 4.0f, 2.0f }) const;

    // A Detour query object for the CALLING thread. dtNavMeshQuery is NOT thread-safe: it carries a
    // node pool and an open list as mutable state, so two threads sharing one corrupt each other's
    // search; the dtNavMesh itself is safe for concurrent reads. Created lazily per thread and
    // invalidated by m_queryGen / the mesh pointer, so a navmesh reload cannot leave a worker holding a
    // query bound to freed memory. nullptr when there is no mesh or allocation fails (callers treat it
    // as "no path").
    dtNavMeshQuery* ThreadQuery() const;

    // Is `to` reachable in a straight walk from `from` (dtNavMeshQuery::raycast)? true when nothing
    // was hit; @p outHit gets the hit point, or `to` when clear.
    bool Raycast(const DirectX::XMFLOAT3& from,
                 const DirectX::XMFLOAT3& to,
                 DirectX::XMFLOAT3* outHit = nullptr,
                 const DirectX::XMFLOAT3& searchExtents = { 2.0f, 4.0f, 2.0f }) const;

    // Project a world-space point onto the navmesh; true if a polygon was found within
    // `searchExtents` (Lua Nav.Project).
    bool ProjectToMesh(const DirectX::XMFLOAT3& worldPos,
                       DirectX::XMFLOAT3& outOnMesh,
                       const DirectX::XMFLOAT3& searchExtents = { 2.0f, 4.0f, 2.0f }) const;

    // Source-file path the last Load/Save used. Empty if the mesh was Built
    // and never saved.
    const std::string& SourcePath() const { return m_sourcePath; }

    // The Detour navmesh, for advanced callers. Owned by the system; do not delete.
    const dtNavMesh* GetNavMesh() const { return m_navMesh; }

    // Tile grid dimensions: 1x1 for a single-tile build.
    int  TilesX() const { return m_tilesX; }
    int  TilesY() const { return m_tilesY; }

    // Draw the TRIANGLE SOUP last fed to Recast - the build input, which neither the navmesh overlay
    // (Detour output) nor the collider overlay (hull AABBs) shows: it answers "is there a walkable floor
    // here at all" and exposes a silently empty soup. Colour is Recast's walkability verdict
    // (rcMarkWalkableTriangles: face-normal Y vs cos(agentMaxSlopeDeg)): green = walkable, red = too
    // steep / facing down. maxDistance <= 0 = unlimited; lower it when the wire buffer fills (a full
    // Tokyo soup is ~185k triangles = 1.1M of the 2.1M vertex budget). Call it every frame: DebugWirePass
    // clears its lines each frame.
    void EmitSoupDebugLines(DebugWirePass&           dbg,
                            const DirectX::XMFLOAT3& viewPos,
                            float                    maxDistance   = 0.0f,
                            uint32_t                 walkableColor = 0xff40ff40u,
                            uint32_t                 steepColor    = 0xff4040ffu) const;

    // True once a bake has captured a soup (Build() stores it).
    bool HasDebugSoup() const { return !m_debugSoup.positions.empty(); }

    // Push every tile's polygon edges into @p dbg: boundary edges (no neighbour, or a link to another
    // tile) in @p boundaryColor, interior ones in @p edgeColor, to tell reachable regions from holes.
    // @p yOffset lifts the wire so it does not z-fight. Call it every frame, as EmitSoupDebugLines.
    void EmitDebugLines(DebugWirePass& dbg,
                        uint32_t edgeColor = 0xff20c0ffu,
                        uint32_t boundaryColor = 0xff0080ffu,
                        float    yOffset = 0.05f) const;

private:
    bool InitQuery();
    void FreeAll();

    // Build one tile of Detour data: tx = ty = 0 and borderSize = 0 for a single-tile build; a tiled
    // build grows the tile bounds by borderSize cells so agent-radius erosion overlaps its neighbours.
    // true when a non-empty tile was produced (*outNavData set), else *outNavData stays null. The caller
    // hands *outNavData to Detour with DT_TILE_FREE_DATA.
    bool BuildOneTile(const TriangleSoup& soup,
                       const BuildParams& bp,
                       int tx, int ty,
                       const float* tileBmin, const float* tileBmax,
                       int borderSize,
                       unsigned char** outNavData, int* outNavDataSize,
                       BuildStats& running) const;

    // Copy of the last soup handed to Build(), kept only so the overlay can show what Recast received.
    // ~4 MB for a Tokyo-sized level.
    TriangleSoup m_debugSoup;
    float        m_debugSoupSlopeDeg = 45.0f;   // params used for that bake

    // Owned, raw pointers because Detour uses its own allocator (dtAlloc/Free).
    dtNavMesh*       m_navMesh  = nullptr;
    dtNavMeshQuery*  m_navQuery = nullptr;
    // Bumped on every navmesh (re)build and destroy. Per-thread queries stamp
    // themselves with it so a reload cannot leave a worker holding a query
    // bound to a freed dtNavMesh.
    uint32_t         m_queryGen = 0;
    std::string      m_sourcePath;

    // Detour owns the tile bytes (DT_TILE_FREE_DATA) and cannot hand them back (getTile is private),
    // so Build / Load keep a copy for Save(): the .inav payload, a stream of
    // [int32 tx][int32 ty][uint32 size][bytes] tile records.
    std::vector<uint8_t> m_navTileData;

    // Tile grid metadata, set by Build / Load; 1x1 for a single-tile build.
    int               m_tilesX     = 0;
    int               m_tilesY     = 0;
    DirectX::XMFLOAT3 m_gridOrigin = { 0, 0, 0 };
    float             m_tileWidth  = 0.f;
    float             m_tileHeight = 0.f;
};

// The Nav module's mesh while the module is enabled; null otherwise (the editor half reaches it
// here).
NavMeshSystem* ActiveNavMesh();

} // namespace Nav
