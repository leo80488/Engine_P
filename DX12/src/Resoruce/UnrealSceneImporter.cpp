#include "Resource/UnrealSceneImporter.h"

// Offline cook: UE scene_manifest.json -> .itex / .imat / .meshlib / .iscn.
//
// UE -> engine parameter mapping (an approximation of the UE master materials: their graphs cannot
// be replicated, these cover the exported parameters):
//   vectors  "Color Multiply" | "Base Color" | "Color"     -> baseColor.rgb
//   scalars  "Roughness"                                   -> roughnessMax (min 0)
//   scalars  "Metalness"                                   -> metalnessMax (min 0)
//   scalars  "U Tiling" / "V Tiling"                       -> texMulAdd.xy
//   textures "Base" ("Text" fallback)                      -> BASECOLORMAP
//   textures "Normal"                                      -> NORMALMAP (BC5; UE normals are DX-style like
//                                                             this engine: no flip)
//   textures "RMO" (R=rough,G=metal,B=AO)                  -> SURFACEMAP, channel-swizzled at cook time to
//                                                             the engine ARM order (R=AO,G=rough,B=metal)
//   textures "Emissive" + "Emissive/Light Color/Intensity" -> EMISSIVEMAP + emissiveColor
//   material path contains "Foliage"                       -> DOUBLE_SIDED + Masked, alphaRef 0.5
//
// Placement transforms (validated matrix-exact against an FBX ground-truth import of the same map):
// pos'=(x,z,-y)/100  q'=(qx,qz,-qy,qw)  s'=(sx,sz,sy). Mesh geometry lands in the same space because
// the UE glTF exporter performs the identical basis change and Assimp's
// aiProcess_ConvertToLeftHanded then matches the FBX path (verified per primitive on the cooked
// AABBs).

#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetRefs.h"
#include "Resource/CollisionLibrary.h"   // the collision library's hull blob (shared with the Mesh Editor)
#include "Resource/MaterialSerializer.h"
#include "Resource/MeshLibIdentity.h"    // IDNT - the .meshlib's Mesh/Section keys
#include "Resource/ModelCollision.h"     // the Meshes' asset collision (BO / PR records)
#include "Resource/ModelImportWrite.h"   // AdoptExistingMeshLibKeys - keys survive a re-import
#include "Resource/SceneImporter.h"
#include "Resource/TextureImporter.h"
#include "Scene/SceneInstanceLoader.h"   // StampModelKeys - the level .iscn's model keys
#include "ECS/Components.h"
#include "ECS/PhysicsComponents.h"   // ColliderComponent::Shape values (prim kinds, V records)
#include "System/Log.h"
#include "System/PathUtil.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <assimp/IOSystem.hpp>
#include <assimp/IOStream.hpp>

#include <DirectXTex.h>
#include <nlohmann/json.hpp>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <tuple>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace Resource
{
namespace
{
    // ==== Assimp IO redirection ====
    // A copy of SceneImporter's file-local SourceDirIOSystem (that one is deliberately TU-private; the
    // anonymous namespace keeps this copy ODR-safe). Each .gltf must find its sibling .bin when fed
    // through ReadFileFromMemory, whatever the CWD and in non-ASCII source folders.
    class UeSourceFileIOStream final : public Assimp::IOStream
    {
    public:
        explicit UeSourceFileIOStream(FILE* f) : m_f(f)
        {
            _fseeki64(m_f, 0, SEEK_END);
            m_size = static_cast<size_t>(_ftelli64(m_f));
            _fseeki64(m_f, 0, SEEK_SET);
        }
        ~UeSourceFileIOStream() override { if (m_f) std::fclose(m_f); }

        size_t Read(void* buf, size_t size, size_t count) override
        {
            return (size == 0) ? 0 : std::fread(buf, size, count, m_f);
        }
        size_t Write(const void*, size_t, size_t) override { return 0; }

        aiReturn Seek(size_t offset, aiOrigin origin) override
        {
            const int whence = (origin == aiOrigin_CUR) ? SEEK_CUR
                             : (origin == aiOrigin_END) ? SEEK_END
                                                        : SEEK_SET;
            const long long delta =
                static_cast<long long>(static_cast<std::ptrdiff_t>(offset));
            return _fseeki64(m_f, delta, whence) == 0 ? AI_SUCCESS : AI_FAILURE;
        }
        size_t Tell()     const override { return static_cast<size_t>(_ftelli64(m_f)); }
        size_t FileSize() const override { return m_size; }
        void   Flush()          override {}

    private:
        FILE*  m_f    = nullptr;
        size_t m_size = 0;
    };

    class UeSourceDirIOSystem final : public Assimp::IOSystem
    {
    public:
        explicit UeSourceDirIOSystem(std::string baseDirUtf8)
            : m_baseDir(std::move(baseDirUtf8)) {}

        bool Exists(const char* file) const override { return !Resolve(file).empty(); }
        char getOsSeparator() const override { return '/'; }

        Assimp::IOStream* Open(const char* file, const char* mode) override
        {
            if (mode && (std::strchr(mode, 'w') || std::strchr(mode, 'a')))
                return nullptr;  // import is read-only

            const std::string path = Resolve(file);
            if (path.empty()) return nullptr;

            FILE* f = nullptr;
            if (_wfopen_s(&f, Sys::Utf8ToWide(path).c_str(), L"rb") != 0 || !f)
                return nullptr;
            return new UeSourceFileIOStream(f);
        }

        void Close(Assimp::IOStream* stream) override { delete stream; }

    private:
        std::string Resolve(const char* file) const
        {
            if (!file || !*file) return {};
            std::string p(file);
            if (Sys::FileExists(p)) return p;
            if (!m_baseDir.empty())
            {
                const std::string rebased =
                    Sys::PathToUtf8(Sys::U8Path(m_baseDir) / Sys::U8Path(p));
                if (Sys::FileExists(rebased)) return rebased;
            }
            // glTF buffer/image references are URI paths (e.g. SM_Road_%2B.bin).
            // Keep literal paths first for FBX and files containing '%' itself;
            // decode once as a fallback, without treating '+' as a form-space.
            const std::string decoded = Resource::AssetRefs::PercentDecode(p);
            if (decoded != p && decoded.find('\0') == std::string::npos)
            {
                if (Sys::FileExists(decoded)) return decoded;
                if (!m_baseDir.empty())
                {
                    const std::string rebased = Sys::PathToUtf8(
                        Sys::U8Path(m_baseDir) / Sys::U8Path(decoded));
                    if (Sys::FileExists(rebased)) return rebased;
                }
            }
            // Not on this checkout's disk: a worktree sharing the main
            // checkout's binary assets reads the source from there
            // (Resource/SharedContent.h). Read-only - Open refuses writes.
            const auto& afs = Resource::AssetFS::Get();
            for (const std::string& cand : { p, decoded })
            {
                if (std::string s = afs.ResolveOnDisk(cand); !s.empty()) return s;
                if (!m_baseDir.empty())
                    if (std::string s = afs.ResolveOnDisk(
                            Sys::PathToUtf8(Sys::U8Path(m_baseDir) / Sys::U8Path(cand))); !s.empty())
                        return s;
            }
            return {};
        }

        std::string m_baseDir;
    };

    // ==== Small helpers ====

    // .iscn is whitespace-tokenized: names must carry no spaces or '='.
    std::string SanitizeToken(const std::string& in)
    {
        std::string out = in;
        for (char& c : out)
            if (static_cast<unsigned char>(c) <= 0x20 || c == '=') c = '_';
        if (out.empty()) out = "Node";
        return out;
    }

    // "/Game/Pack/Sub/MI_X.MI_X" -> "MI_X"
    std::string UELeafName(const std::string& uePath)
    {
        const size_t slash = uePath.find_last_of('/');
        std::string leaf = (slash == std::string::npos) ? uePath : uePath.substr(slash + 1);
        const size_t dot = leaf.find('.');
        if (dot != std::string::npos) leaf = leaf.substr(0, dot);
        return leaf;
    }

    bool ContainsCI(const std::string& hay, const char* needle)
    {
        std::string h = hay, n = needle;
        std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return h.find(n) != std::string::npos;
    }

    struct Warnings
    {
        std::vector<std::string>* out = nullptr;
        int overflow = 0;
        void Add(std::string w)
        {
            LOG_WARNING("UnrealSceneImporter: %s", w.c_str());
            if (out->size() < 80) out->push_back(std::move(w));
            else ++overflow;
        }
        void Flush()
        {
            if (overflow > 0)
                out->push_back("(+" + std::to_string(overflow) + " more warnings, see log)");
        }
    };

    // UE mirrors instances with a negative scale on one axis (490 of ~6000 placements in Tokyo); a
    // negative-determinant world matrix reverses triangle winding. The renderer handles that per draw
    // (front face from sign(det(world))); with Options::bakeMirroredVariants the cook instead bakes the
    // mirror into its own meshlib entry (vertices mirrored, winding reversed) and hands the .iscn the
    // magnitudes only, so every instance transform keeps a positive determinant, at one extra entry per
    // (asset, mirrored-axis) pair.
    //
    // Returns a mask over the engine axes: bit0 = X, bit1 = Y, bit2 = Z. Zero when the determinant is
    // non-negative: two mirrored axes are a 180 deg rotation and render correctly as is.
    uint8_t MirrorMaskFromEngineScale(const float s[3])
    {
        if (s[0] * s[1] * s[2] >= 0.0f) return 0;
        uint8_t m = 0;
        if (s[0] < 0.0f) m |= 0x1u;
        if (s[1] < 0.0f) m |= 0x2u;
        if (s[2] < 0.0f) m |= 0x4u;
        return m;
    }

    // Same question asked of a raw UE scale array (engine scale = (sx, sz, sy)).
    uint8_t MirrorMaskFromUEScale(const json& scArr)
    {
        if (!scArr.is_array() || scArr.size() < 3) return 0;
        const float es[3] = { scArr[0].get<float>(),
                              scArr[2].get<float>(),
                              scArr[1].get<float>() };
        return MirrorMaskFromEngineScale(es);
    }

    std::string VariantKey(const std::string& asset, uint8_t mask)
    {
        return asset + "#" + std::to_string(static_cast<int>(mask));
    }

    // UE placement transform -> engine TRS (the rule in the file header).
    struct EngineTRS { float t[3]; float q[4]; float s[3]; };
    EngineTRS ConvertTRS(const double p[3], const double r[4], const double sc[3])
    {
        EngineTRS e{};
        e.t[0] = float(p[0] / 100.0);
        e.t[1] = float(p[2] / 100.0);
        e.t[2] = float(-p[1] / 100.0);
        e.q[0] = float(r[0]);
        e.q[1] = float(r[2]);
        e.q[2] = float(-r[1]);
        e.q[3] = float(r[3]);
        e.s[0] = float(sc[0]);
        e.s[1] = float(sc[2]);
        e.s[2] = float(sc[1]);
        return e;
    }

    // Collect aiMesh indices in scene-graph traversal order (= glTF primitive order for UE's
    // one-node-per-asset exports: slot i <-> primitive i, which holds for the whole export set,
    // 142/142 prims == slots).
    void CollectMeshIndicesDFS(const aiNode* n, std::vector<unsigned>& out)
    {
        if (!n) return;
        for (unsigned i = 0; i < n->mNumMeshes; ++i) out.push_back(n->mMeshes[i]);
        for (unsigned c = 0; c < n->mNumChildren; ++c) CollectMeshIndicesDFS(n->mChildren[c], out);
    }

    // ==== Manifest model ====
    enum class TexClass : uint8_t { ColorSRGB, Linear, Normal, RmoSwizzle };

    struct TexPlan
    {
        std::string srcFile;      // filename inside <export>/textures/
        TexClass    cls = TexClass::Linear;
        std::string itexRef;      // project-relative ref written into .imat
        std::string itexPath;     // full output path
    };

    struct MatPlan
    {
        std::string imatName;                 // sanitized, deduped stem
        json        src;                      // manifest material object
        std::string uePath;
    };

    // ==== Collision ====
    // Unreal authors collision on the UStaticMesh (BodySetup::AggGeom), not on the placement: the
    // granularity of the model's Mesh, so it is written as the Mesh's asset collision (`BO` / `PR`
    // records, Resource/ModelCollision.h) and PhysicsSystem shares one Jolt shape across every
    // placement. Per-actor volumes (BlockingVolume) are placement-level and get `V ` lines on their own
    // node instead.
    //
    //   UCX convex hull      -> a hull prim: its points, transformed like any
    //                           vertex, in <Scene>_collision.meshlib (by key)
    //   UBX box              -> a box prim: centre, half extents, rotation
    //   USP sphere           -> a sphere prim
    //   UCP capsule (sphyl)  -> a capsule prim
    //   CTF_UseComplexAsSimple -> BO complexity=complex: every section's own
    //                           triangles (UE collides with the whole render mesh)
    // A box / capsule rotation goes through the same basis change ConvertTRS applies to every
    // placement: (x, z, -y) is a proper rotation, so the UE quaternion (x, y, z, w) is (x, z, -y, w) in
    // engine space, with no Euler triple anywhere. A box's half extents swap y and z with the basis; a
    // capsule's axis (UE local Z) lands on engine local Y, Jolt's capsule axis. A volume's box stays its
    // 8 corners as a hull: a V record's rotation is an Euler triple.
    constexpr uint32_t kShapeBox     = static_cast<uint32_t>(ColliderComponent::Shape::Box);
    constexpr uint32_t kShapeSphere  = static_cast<uint32_t>(ColliderComponent::Shape::Sphere);
    constexpr uint32_t kShapeCapsule = static_cast<uint32_t>(ColliderComponent::Shape::Capsule);
    constexpr uint32_t kShapeMesh    = static_cast<uint32_t>(ColliderComponent::Shape::Mesh);
    constexpr uint32_t kShapeHull    = static_cast<uint32_t>(ColliderComponent::Shape::ConvexHull);

    struct UeCollisionPrim
    {
        uint32_t                       shape = kShapeHull;
        std::vector<DirectX::XMFLOAT3> points;                            // hull (engine space)
        DirectX::XMFLOAT3              offset{ 0, 0, 0 };                 // centre: box / sphere / capsule
        DirectX::XMFLOAT3              halfExtents{ 0.5f, 0.5f, 0.5f };   // box
        DirectX::XMFLOAT4              rotation{ 0, 0, 0, 1 };            // box / capsule (engine quaternion)
        float                          radius = 0.5f;                     // sphere / capsule
        float                          halfHeight = 0.5f;                 // capsule: half the cylinder
        bool                           useRenderMesh = false;             // complex-as-simple
    };

    // A UE rotation quaternion (x, y, z, w) in engine space - ConvertTRS's q'.
    DirectX::XMFLOAT4 UeQuatToEngine(const double q[4])
    {
        const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!(n > 1e-9)) return { 0.f, 0.f, 0.f, 1.f };
        return { float(q[0] / n), float(q[2] / n), float(-q[1] / n), float(q[3] / n) };
    }

    // @p p mirrored across the axes of @p mask (bit 0 = x, 1 = y, 2 = z) - the
    // collision of a baked mirrored variant. A reflection M turns a rotation R
    // into M R M, whose quaternion vector is det(M) * M * v.
    UeCollisionPrim MirrorCollisionPrim(const UeCollisionPrim& p, uint8_t mask)
    {
        UeCollisionPrim m = p;
        const float s[3] = { (mask & 0x1u) ? -1.f : 1.f, (mask & 0x2u) ? -1.f : 1.f, (mask & 0x4u) ? -1.f : 1.f };
        const float det = s[0] * s[1] * s[2];
        for (auto& v : m.points) { v.x *= s[0]; v.y *= s[1]; v.z *= s[2]; }
        m.offset   = { p.offset.x * s[0], p.offset.y * s[1], p.offset.z * s[2] };
        m.rotation = { det * s[0] * p.rotation.x, det * s[1] * p.rotation.y, det * s[2] * p.rotation.z, p.rotation.w };
        return m;
    }

    // UE local-space point (cm) -> engine local-space point (m). Same basis change as placements;
    // det = +1, so hull chirality is preserved.
    DirectX::XMFLOAT3 UePointToEngine(double x, double y, double z)
    {
        return { float(x / 100.0), float(z / 100.0), float(-y / 100.0) };
    }

    bool ReadUeVec3(const json& a, double out[3])
    {
        if (a.is_array() && a.size() >= 3)
        {
            for (int i = 0; i < 3; ++i) out[i] = a[i].get<double>();
            return true;
        }
        if (a.is_object())  // {"x":..,"y":..,"z":..}
        {
            out[0] = a.value("x", 0.0); out[1] = a.value("y", 0.0); out[2] = a.value("z", 0.0);
            return true;
        }
        return false;
    }

    // Parses one UE collision block ("convex" / "boxes" / "spheres" / "capsules" + "trace_flag") into
    // engine-space primitives. Returns {} when the block is absent: an export without collision data,
    // or a mesh Unreal gave no collision (posters, text cards: they must not collide).
    // @p boxesAsHulls: a box becomes the hull of its 8 corners (a volume's V record), else a box prim.
    // A capsule is {center, rot, radius, length} (UE KSphylElem; length = the cylinder between the two
    // sphere centres).
    std::vector<UeCollisionPrim> ParseUeCollision(const json& col, Warnings& warn,
                                                  const std::string& label, bool boxesAsHulls)
    {
        std::vector<UeCollisionPrim> out;
        if (!col.is_object()) return out;

        for (const auto& hull : col.value("convex", json::array()))
        {
            UeCollisionPrim p;
            p.shape = kShapeHull;
            for (const auto& v : hull.value("verts", json::array()))
            {
                double q[3];
                if (ReadUeVec3(v, q)) p.points.push_back(UePointToEngine(q[0], q[1], q[2]));
            }
            if (p.points.size() >= 4) out.push_back(std::move(p));
            else warn.Add(label + ": convex hull with < 4 vertices skipped");
        }

        for (const auto& box : col.value("boxes", json::array()))
        {
            double c[3] = { 0, 0, 0 }, e[3] = { 0, 0, 0 }, rq[4] = { 0, 0, 0, 1 };
            ReadUeVec3(box.value("center", json::array()), c);
            ReadUeVec3(box.value("extent", json::array()), e);   // HALF extents, UE cm
            const json& jr = box.value("rot", json::array());
            if (jr.is_array() && jr.size() >= 4)
                for (int i = 0; i < 4; ++i) rq[i] = jr[i].get<double>();

            if (!boxesAsHulls)
            {
                if (!(e[0] > 1e-5 && e[1] > 1e-5 && e[2] > 1e-5))
                {
                    warn.Add(label + ": box collision primitive with a zero extent skipped");
                    continue;
                }
                UeCollisionPrim p;
                p.shape       = kShapeBox;
                p.offset      = UePointToEngine(c[0], c[1], c[2]);
                p.halfExtents = { float(e[0] / 100.0), float(e[2] / 100.0), float(e[1] / 100.0) };
                p.rotation    = UeQuatToEngine(rq);
                out.push_back(std::move(p));
                continue;
            }

            // Rotate + translate the 8 corners in UE space, then convert each like a vertex: no Euler
            // round-trip.
            const double x = rq[0], y = rq[1], z = rq[2], w = rq[3];
            const double m[3][3] = {
                { 1 - 2*(y*y + z*z),     2*(x*y - z*w),     2*(x*z + y*w) },
                {     2*(x*y + z*w), 1 - 2*(x*x + z*z),     2*(y*z - x*w) },
                {     2*(x*z - y*w),     2*(y*z + x*w), 1 - 2*(x*x + y*y) },
            };
            UeCollisionPrim p;
            p.shape = kShapeHull;
            for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2)
            {
                const double l[3] = { sx * e[0], sy * e[1], sz * e[2] };
                const double wpt[3] = {
                    c[0] + m[0][0]*l[0] + m[0][1]*l[1] + m[0][2]*l[2],
                    c[1] + m[1][0]*l[0] + m[1][1]*l[1] + m[1][2]*l[2],
                    c[2] + m[2][0]*l[0] + m[2][1]*l[1] + m[2][2]*l[2],
                };
                p.points.push_back(UePointToEngine(wpt[0], wpt[1], wpt[2]));
            }
            // A zero half-extent collapses the 8 corners onto a plane; Jolt then
            // fails the hull build and MakeShape silently substitutes a 0.5 m
            // box. Same >=4-distinct-point guard the convex branch has.
            if (e[0] > 1e-5 && e[1] > 1e-5 && e[2] > 1e-5)
                out.push_back(std::move(p));
            else
                warn.Add(label + ": box collision primitive with a zero extent skipped");
        }

        for (const auto& sph : col.value("spheres", json::array()))
        {
            double c[3] = { 0, 0, 0 };
            ReadUeVec3(sph.value("center", json::array()), c);
            UeCollisionPrim p;
            p.shape  = kShapeSphere;
            p.offset = UePointToEngine(c[0], c[1], c[2]);
            p.radius = float(sph.value("radius", 0.0) / 100.0);
            if (p.radius > 1e-5f) out.push_back(std::move(p));
        }

        for (const auto& cap : col.value("capsules", json::array()))
        {
            double c[3] = { 0, 0, 0 }, rq[4] = { 0, 0, 0, 1 };
            ReadUeVec3(cap.value("center", json::array()), c);
            const json& jr = cap.value("rot", json::array());
            if (jr.is_array() && jr.size() >= 4)
                for (int i = 0; i < 4; ++i) rq[i] = jr[i].get<double>();
            UeCollisionPrim p;
            p.shape      = kShapeCapsule;
            p.offset     = UePointToEngine(c[0], c[1], c[2]);
            p.rotation   = UeQuatToEngine(rq);
            p.radius     = float(cap.value("radius", 0.0) / 100.0);
            p.halfHeight = float(cap.value("length", 0.0) / 200.0);
            if (!(p.radius > 1e-5f))
            {
                warn.Add(label + ": capsule collision primitive with a zero radius skipped");
                continue;
            }
            if (!(p.halfHeight > 1e-5f)) p.shape = kShapeSphere;   // no cylinder: a sphere
            out.push_back(std::move(p));
        }

        // Complex-as-simple: Unreal collides against the render triangles. UE's Python enum stringifies as
        // "<CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE: 1>", the C++ FName is "CTF_UseComplexAsSimple":
        // dropping separators and case makes either spelling match.
        std::string trace = col.value("trace_flag", "");
        {
            std::string norm;
            for (char ch : trace)
                if (ch != '_' && ch != ' ')
                    norm += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            trace.swap(norm);
        }
        // CTF_UseComplexAsSimple means Unreal builds only the complex (render triangle) shape and answers
        // every query with it; the authored simple primitives in AggGeom are ignored at runtime. So it
        // overrides them rather than filling in when they are missing: hulls kept on the Tokyo pack's curved
        // sidewalks, curbs and cross-beams would make the player walk the convex chord across a curve
        // instead of the curb profile.
        if (trace.find("COMPLEXASSIMPLE") != std::string::npos)
        {
            if (!out.empty())
                warn.Add(label + ": complex-as-simple overrides " +
                         std::to_string(out.size()) + " authored simple primitive(s)");
            out.clear();
            UeCollisionPrim p;
            p.shape         = kShapeMesh;
            p.useRenderMesh = true;
            out.push_back(std::move(p));
        }
        return out;
    }

    // ==== .iscn assembly ====
    struct IscnNode
    {
        int              parent = 0;
        EngineTRS        trs{};
        std::string      name;
        std::vector<int> meshRefs;
        // `V ` collision records owned by this node (blocking volumes).
        std::vector<std::string> volumeLines;
    };

    // @p collisionLines: the Meshes' asset collision (`BO` / `PR` records).
    std::vector<uint8_t> BuildIscnBlobLocal(const std::string&               sceneName,
                                            const std::vector<IscnNode>&     nodes,
                                            const std::vector<std::string>&  fLines,
                                            const std::vector<std::string>&  collisionLines)
    {
        std::ostringstream ss;
        ss << "# ISCN engine scene container (UE manifest import)\n";
        ss << "S nodes=" << (nodes.size() + 1)
           << " meshfiles=" << fLines.size()
           << " source=" << sceneName << "\n";
        ss << "N idx=0 parent=-1 tx=0 ty=0 tz=0 qx=0 qy=0 qz=0 qw=1"
              " sx=1 sy=1 sz=1 mc=0 name=ROOT\n";

        int idx = 1;
        for (const IscnNode& n : nodes)
        {
            ss << "N idx=" << idx++
               << " parent=" << n.parent
               << " tx=" << n.trs.t[0] << " ty=" << n.trs.t[1] << " tz=" << n.trs.t[2]
               << " qx=" << n.trs.q[0] << " qy=" << n.trs.q[1]
               << " qz=" << n.trs.q[2] << " qw=" << n.trs.q[3]
               << " sx=" << n.trs.s[0] << " sy=" << n.trs.s[1] << " sz=" << n.trs.s[2]
               << " mc=" << n.meshRefs.size();
            for (size_t k = 0; k < n.meshRefs.size(); ++k)
                ss << " mr" << k << "=" << n.meshRefs[k];
            ss << " name=" << n.name << "\n";
            // Placement-level collision (blocking volumes) rides with its node;
            // `n=` is filled in here because the index isn't known until now.
            for (const std::string& v : n.volumeLines)
                ss << "V n=" << (idx - 1) << v << "\n";
        }
        for (const std::string& f : fLines)
            ss << f << "\n";
        for (const std::string& c : collisionLines)
            ss << c << "\n";

        const std::string text = ss.str();
        const uint32_t textLen = static_cast<uint32_t>(text.size());

        SceneMetadata meta{};
        meta.nodeCount     = static_cast<uint32_t>(nodes.size() + 1);
        meta.meshFileCount = static_cast<uint32_t>(fLines.size());
        meta.textLength    = textLen;

        AssetHeader header{};
        header.magic        = MAGIC_SCENE;
        header.version      = CurrentVersion(MAGIC_SCENE);
        header.resourceType = static_cast<uint16_t>(ResourceType::Scene);
        header.metadataSize = sizeof(SceneMetadata);
        header.dataSize     = textLen + 1;

        std::vector<uint8_t> blob(sizeof(AssetHeader) + sizeof(SceneMetadata) + textLen + 1, 0);
        uint8_t* dst = blob.data();
        std::memcpy(dst, &header, sizeof(header));  dst += sizeof(header);
        std::memcpy(dst, &meta, sizeof(meta));      dst += sizeof(meta);
        std::memcpy(dst, text.data(), textLen);
        return blob;
    }

    // ==== Texture cooking ====
    // RMO (R=rough, G=metal, B=AO) -> engine ARM (R=AO, G=rough, B=metal), re-wrapped as an
    // uncompressed DDS so TextureImporter builds the mips and compresses it to BC7.
    bool SwizzleRmoToDds(const std::vector<uint8_t>& pngBytes, std::vector<uint8_t>& ddsOut)
    {
        using namespace DirectX;
        TexMetadata meta{};
        ScratchImage loaded;
        if (FAILED(LoadFromWICMemory(pngBytes.data(), pngBytes.size(),
                                     WIC_FLAGS_IGNORE_SRGB, &meta, loaded)))
            return false;

        ScratchImage rgba;
        const ScratchImage* work = &loaded;
        if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            if (FAILED(Convert(loaded.GetImages(), loaded.GetImageCount(), meta,
                               DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT,
                               TEX_THRESHOLD_DEFAULT, rgba)))
                return false;
            work = &rgba;
        }

        for (size_t i = 0; i < work->GetImageCount(); ++i)
        {
            const DirectX::Image& img = work->GetImages()[i];
            for (size_t y = 0; y < img.height; ++y)
            {
                uint8_t* row = img.pixels + y * img.rowPitch;
                for (size_t x = 0; x < img.width; ++x)
                {
                    uint8_t* p = row + x * 4;
                    const uint8_t rough = p[0], metal = p[1], ao = p[2];
                    p[0] = ao;      // R = AO
                    p[1] = rough;   // G = roughness
                    p[2] = metal;   // B = metalness
                    p[3] = 0xFF;
                }
            }
        }

        Blob dds;
        if (FAILED(SaveToDDSMemory(work->GetImages(), work->GetImageCount(),
                                   work->GetMetadata(), DDS_FLAGS_NONE, dds)))
            return false;
        ddsOut.assign(static_cast<const uint8_t*>(dds.GetBufferPointer()),
                      static_cast<const uint8_t*>(dds.GetBufferPointer()) + dds.GetBufferSize());
        return true;
    }

    bool IsUpToDate(const std::string& outPath, const std::string& srcPath)
    {
        std::error_code ec1, ec2;
        const auto dstT = fs::last_write_time(Sys::U8Path(outPath), ec1);
        const auto srcT = fs::last_write_time(Sys::U8Path(srcPath), ec2);
        return !ec1 && !ec2 && dstT >= srcT;
    }

} // anonymous namespace

const char* UnrealSceneImporter::Progress::StageName(int stage)
{
    switch (stage)
    {
    case 0: return "Parsing manifest";
    case 1: return "Cooking textures";
    case 2: return "Cooking meshes";
    case 3: return "Writing materials";
    case 4: return "Writing scene";
    default: return "Done";
    }
}

UnrealSceneImporter::Result UnrealSceneImporter::Run(const Options& opt, Progress* prog)
{
    Result R;
    Warnings warn; warn.out = &R.warnings;

    Progress localProg;
    if (!prog) prog = &localProg;

    const auto Cancelled = [&]() { return prog->cancel.load(); };
    const auto Fail = [&](std::string err) { R.ok = false; R.error = std::move(err);
                                             LOG_ERROR("UnrealSceneImporter: %s", R.error.c_str());
                                             return R; };

    // This may run on a TaskSystem worker: an escaped exception (json .at(), bad file data) would
    // terminate the process, so it degrades to a failed Result.
    try
    {

    // WIC (PNG decode in DirectXTex) needs COM on this thread; the import may
    // run on a TaskSystem worker that never initialized it.
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInit = SUCCEEDED(coHr);  // RPC_E_CHANGED_MODE -> already STA, usable as is
    struct CoGuard { bool init; ~CoGuard() { if (init) CoUninitialize(); } } coGuard{ coInit };

    // ---- Stage 0: parse ----
    prog->stage = 0; prog->done = 0; prog->total = 1;
    prog->SetCurrent(opt.manifestPath);

    std::vector<uint8_t> manifestBytes;
    if (!Sys::ReadFileBytes(opt.manifestPath, manifestBytes) || manifestBytes.empty())
        return Fail("cannot read manifest: " + opt.manifestPath);

    const json j = json::parse(manifestBytes.begin(), manifestBytes.end(),
                               nullptr, /*allow_exceptions*/false, /*ignore_comments*/true);
    if (j.is_discarded())
        return Fail("manifest is not valid JSON: " + opt.manifestPath);

    const fs::path exportDir = Sys::U8Path(opt.manifestPath).parent_path();
    const fs::path texSrcDir  = exportDir / "textures";
    const fs::path meshSrcDir = exportDir / "meshes";

    std::string sceneName = opt.sceneName;
    if (sceneName.empty())
    {
        const std::string srcMap = j.contains("meta") ? j["meta"].value("source_map", "") : "";
        sceneName = srcMap.empty() ? "UnrealScene" : UELeafName(srcMap);
    }
    sceneName = SanitizeToken(sceneName);

    // Output roots. Texture refs inside .imat are resolved against the
    // engine CWD (project dir), so prefer a project-relative form.
    const fs::path outDir =
        Sys::U8Path(opt.outputDir.empty() ? std::string("asset") : opt.outputDir)
        / Sys::U8Path(sceneName);
    std::string outRefRoot;  // forward-slash, project-relative if possible
    {
        fs::path refPath = outDir;
        if (outDir.is_absolute())
        {
            std::error_code ec;
            const fs::path rel = fs::relative(outDir, fs::current_path(), ec);
            if (!ec && !rel.empty() && rel.native().rfind(L"..", 0) != 0)
                refPath = rel;
        }
        outRefRoot = Sys::PathToUtf8(refPath);
        std::replace(outRefRoot.begin(), outRefRoot.end(), '\\', '/');
        while (!outRefRoot.empty() && outRefRoot.back() == '/') outRefRoot.pop_back();
    }

    if (!j.contains("placements") || !j["placements"].is_array())
        return Fail("manifest has no placements array");
    const json& placements = j["placements"];
    const json  meshAssets = j.value("mesh_assets", json::object());
    const json  materials  = j.value("materials",  json::object());
    const json  textures   = j.value("textures",   json::object());
    const json  instanced  = j.value("instanced",  json::array());

    R.decalsSkipped = j.contains("decals") && j["decals"].is_array()
                    ? static_cast<int>(j["decals"].size()) : 0;

    // ---- Collect the used assets / materials (deterministic first-use order) ----
    std::vector<std::string> assetOrder;
    std::unordered_set<std::string> assetSeen;
    std::vector<std::string> matOrder;
    std::unordered_set<std::string> matSeen;

    const auto noteAsset = [&](const std::string& a) {
        if (!a.empty() && assetSeen.insert(a).second) assetOrder.push_back(a);
    };
    const auto noteMats = [&](const json& arr) {
        if (!arr.is_array()) return;
        for (const auto& m : arr)
        {
            if (!m.is_string()) continue;
            const std::string s = m.get<std::string>();
            if (!s.empty() && s != "None" && matSeen.insert(s).second) matOrder.push_back(s);
        }
    };
    for (const auto& p : placements)
    {
        noteAsset(p.value("asset", ""));
        noteMats(p.value("materials", json::array()));
    }
    for (const auto& g : instanced)
    {
        noteAsset(g.value("asset", ""));
        noteMats(g.value("materials", json::array()));
    }

    // ---- Material plans (dedup names) ----
    std::unordered_map<std::string, MatPlan> matPlans;   // uePath -> plan
    {
        std::unordered_map<std::string, int> nameUse;
        for (const std::string& mp : matOrder)
        {
            MatPlan plan;
            plan.uePath = mp;
            if (materials.contains(mp)) plan.src = materials[mp];
            else warn.Add("material not in manifest table, using defaults: " + mp);

            std::string base = SanitizeToken(UELeafName(mp));
            const int use = ++nameUse[base];
            plan.imatName = (use == 1) ? base : base + "_" + std::to_string(use);
            matPlans.emplace(mp, std::move(plan));
        }
    }

    // ---- Texture plans ----
    std::unordered_map<std::string, TexPlan> texPlans;   // ue texture path -> plan
    {
        const auto planTexture = [&](const std::string& uePath, const std::string& slotKey)
        {
            if (uePath.empty()) return;
            TexClass cls;
            std::string comp, file;
            bool srgb = false;
            if (textures.contains(uePath))
            {
                const json& t = textures[uePath];
                file = t.value("file", "");
                srgb = t.value("srgb", false);
                comp = t.value("compression_settings", "");
            }
            if (file.empty())
            {
                warn.Add("texture not in manifest table: " + uePath + " (slot " + slotKey + ")");
                return;
            }
            if (slotKey == "Normal" || comp.find("TC_NORMALMAP") != std::string::npos)
                cls = TexClass::Normal;
            else if (slotKey == "RMO")
                cls = TexClass::RmoSwizzle;
            else
                cls = srgb ? TexClass::ColorSRGB : TexClass::Linear;

            auto it = texPlans.find(uePath);
            if (it != texPlans.end())
            {
                if (it->second.cls != cls)
                    warn.Add("texture used with conflicting roles, keeping first: " + uePath);
                return;
            }
            TexPlan tp;
            tp.srcFile = file;
            tp.cls = cls;
            const std::string stem = Sys::PathToUtf8(Sys::U8Path(file).stem());
            tp.itexRef  = outRefRoot + "/Textures/" + stem + ".itex";
            tp.itexPath = Sys::PathToUtf8(outDir / "Textures" / (stem + ".itex"));
            texPlans.emplace(uePath, std::move(tp));
        };

        for (const std::string& mp : matOrder)
        {
            const json& src = matPlans[mp].src;
            if (!src.is_object() || !src.contains("textures")) continue;
            for (auto it = src["textures"].begin(); it != src["textures"].end(); ++it)
                if (it.value().is_string())
                    planTexture(it.value().get<std::string>(), it.key());
        }
    }

    LOG_INFO("UnrealSceneImporter: %s — %d placements, %d assets, %d materials, %d textures",
             sceneName.c_str(), (int)placements.size(), (int)assetOrder.size(),
             (int)matOrder.size(), (int)texPlans.size());

    // ---- Stage 1: cook textures ----
    prog->stage = 1; prog->done = 0; prog->total = (int)texPlans.size();
    {
        TextureImporter texImporter;
        for (auto& [uePath, tp] : texPlans)
        {
            if (Cancelled()) return Fail("cancelled");
            prog->SetCurrent(tp.srcFile);

            const std::string srcPath = Sys::PathToUtf8(texSrcDir / Sys::U8Path(tp.srcFile));
            if (opt.skipUpToDateTextures && IsUpToDate(tp.itexPath, srcPath))
            {
                ++R.texturesSkipped; ++prog->done;
                continue;
            }

            std::vector<uint8_t> srcBytes;
            if (!Sys::ReadFileBytes(srcPath, srcBytes) || srcBytes.empty())
            {
                warn.Add("cannot read texture: " + srcPath);
                ++prog->done;
                continue;
            }

            TextureImportSettings settings;
            std::vector<uint8_t> blob;
            switch (tp.cls)
            {
            case TexClass::Normal:
                settings.format = TextureImportFormat::BC5_Linear;
                blob = texImporter.ImportWithSettings(srcPath, srcBytes, settings);
                break;
            case TexClass::RmoSwizzle:
            {
                std::vector<uint8_t> dds;
                if (!SwizzleRmoToDds(srcBytes, dds))
                {
                    warn.Add("RMO swizzle failed: " + srcPath);
                    break;
                }
                settings.format = TextureImportFormat::BC7_Linear;
                const std::string fakeDds =
                    Sys::PathToUtf8(Sys::U8Path(tp.srcFile).stem()) + ".dds";
                blob = texImporter.ImportWithSettings(fakeDds, dds, settings);
                break;
            }
            case TexClass::ColorSRGB:
                settings.format = TextureImportFormat::BC7_SRGB;
                blob = texImporter.ImportWithSettings(srcPath, srcBytes, settings);
                break;
            case TexClass::Linear:
            default:
                settings.format = TextureImportFormat::BC7_Linear;
                blob = texImporter.ImportWithSettings(srcPath, srcBytes, settings);
                break;
            }

            if (blob.empty())
                warn.Add("texture cook failed: " + srcPath);
            else if (!Sys::WriteFileBytes(tp.itexPath, blob))
                warn.Add("cannot write: " + tp.itexPath);
            else
                ++R.texturesCooked;
            ++prog->done;
        }
    }

    // What WriteModelImportFiles writes at the end: the level library and the materials (and the
    // scene).
    std::vector<uint8_t>                                      levelLibBlob;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> matBlobs;

    // ---- Stage 2: cook meshes into one meshlib ----
    prog->stage = 2; prog->done = 0; prog->total = (int)assetOrder.size();

    // Importers stay alive until BuildMeshLibBlob has packed every aiMesh.
    std::vector<std::unique_ptr<Assimp::Importer>> keepAlive;
    keepAlive.reserve(assetOrder.size());
    std::vector<const aiMesh*> allMeshes;
    std::vector<uint32_t>      allMatIdx;
    std::vector<uint8_t>       allMirror;   // per-source axis mirror mask
    // asset -> meshlib entry per material slot (UINT32_MAX = missing/empty)
    std::unordered_map<std::string, std::vector<uint32_t>> assetSlotEntries;
    // asset -> the aiMesh behind each slot, so mirrored variants can be packed from the same source
    // (pointers stay valid: keepAlive owns the importers)
    std::unordered_map<std::string, std::vector<const aiMesh*>> assetSlotMeshes;
    // "asset#mask" -> entry per slot, for the mirrored variants
    std::map<std::string, std::vector<uint32_t>> variantSlotEntries;

    constexpr unsigned kAiFlags = aiProcess_Triangulate
                                | aiProcess_ConvertToLeftHanded
                                | aiProcess_GenSmoothNormals
                                | aiProcess_CalcTangentSpace
                                | aiProcess_JoinIdenticalVertices
                                | aiProcess_SortByPType;

    for (const std::string& assetPath : assetOrder)
    {
        if (Cancelled()) return Fail("cancelled");

        std::string file;
        size_t slotCount = 0;
        if (meshAssets.contains(assetPath))
        {
            file = meshAssets[assetPath].value("file", "");
            slotCount = meshAssets[assetPath].value("material_slots", json::array()).size();
        }
        prog->SetCurrent(file.empty() ? assetPath : file);
        if (file.empty())
        {
            warn.Add("asset not in mesh_assets table: " + assetPath);
            assetSlotEntries[assetPath] = {};
            ++prog->done;
            continue;
        }

        const std::string srcPath = Sys::PathToUtf8(meshSrcDir / Sys::U8Path(file));
        std::vector<uint8_t> srcBytes;
        if (!Sys::ReadFileBytes(srcPath, srcBytes) || srcBytes.empty())
        {
            warn.Add("cannot read mesh: " + srcPath);
            assetSlotEntries[assetPath] = {};
            ++prog->done;
            continue;
        }

        auto importer = std::make_unique<Assimp::Importer>();
        importer->SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        importer->SetIOHandler(new UeSourceDirIOSystem(
            Sys::PathToUtf8(Sys::U8Path(srcPath).parent_path())));

        const std::string extHint =
            Sys::PathToUtf8(Sys::U8Path(file).extension());
        const aiScene* scene = importer->ReadFileFromMemory(
            srcBytes.data(), srcBytes.size(), kAiFlags,
            extHint.empty() ? "" : extHint.c_str() + 1);

        if (!scene || !scene->mRootNode)
        {
            warn.Add(std::string("Assimp failed on ") + file + ": " +
                     importer->GetErrorString());
            assetSlotEntries[assetPath] = {};
            ++prog->done;
            continue;
        }

        std::vector<unsigned> meshIdxOrder;
        CollectMeshIndicesDFS(scene->mRootNode, meshIdxOrder);

        if (slotCount != 0 && meshIdxOrder.size() != slotCount)
            warn.Add(file + ": " + std::to_string(meshIdxOrder.size()) +
                     " primitives vs " + std::to_string(slotCount) +
                     " material slots — slot mapping clamped");

        std::vector<uint32_t>      entries;
        std::vector<const aiMesh*> slotMeshes;
        entries.reserve(meshIdxOrder.size());
        slotMeshes.reserve(meshIdxOrder.size());
        for (unsigned mi : meshIdxOrder)
        {
            const aiMesh* am = (mi < scene->mNumMeshes) ? scene->mMeshes[mi] : nullptr;
            if (!am || am->mNumVertices == 0 || am->mNumFaces == 0)
            {
                entries.push_back(UINT32_MAX);
                slotMeshes.push_back(nullptr);
                continue;
            }
            entries.push_back(static_cast<uint32_t>(allMeshes.size()));
            slotMeshes.push_back(am);
            allMeshes.push_back(am);
            allMatIdx.push_back(0);
            allMirror.push_back(0);
        }
        assetSlotEntries[assetPath] = std::move(entries);
        assetSlotMeshes[assetPath]  = std::move(slotMeshes);

        keepAlive.push_back(std::move(importer));
        ++R.assetsCooked;
        ++prog->done;
    }

    if (allMeshes.empty())
        return Fail("no meshes were cooked — nothing to import");

    // ---- Mirrored variants (see MirrorMaskFromEngineScale) ----
    // Collect every (asset, mirror mask) a negative-scale placement needs, then pack one extra entry per
    // slot with the mirror baked into the geometry.
    if (opt.bakeMirroredVariants)
    {
        std::map<std::string, std::pair<std::string, uint8_t>> needed;  // key -> (asset, mask)
        const auto note = [&](const std::string& asset, const json& scArr)
        {
            if (asset.empty()) return;
            const uint8_t mask = MirrorMaskFromUEScale(scArr);
            if (mask == 0) return;
            needed.emplace(VariantKey(asset, mask), std::make_pair(asset, mask));
        };
        for (const auto& p : placements)
            note(p.value("asset", ""), p.value("scale", json::array()));
        for (const auto& g : instanced)
        {
            const std::string a = g.value("asset", "");
            for (const auto& inst : g.value("instances", json::array()))
                note(a, inst.value("scale", json::array()));
        }

        for (const auto& [key, am] : needed)
        {
            const auto sm = assetSlotMeshes.find(am.first);
            if (sm == assetSlotMeshes.end()) continue;   // asset failed to cook

            std::vector<uint32_t> entries;
            entries.reserve(sm->second.size());
            for (const aiMesh* src : sm->second)
            {
                if (!src) { entries.push_back(UINT32_MAX); continue; }
                entries.push_back(static_cast<uint32_t>(allMeshes.size()));
                allMeshes.push_back(src);
                allMatIdx.push_back(0);
                allMirror.push_back(am.second);
            }
            variantSlotEntries[key] = std::move(entries);
        }
        R.mirrorVariants = (int)variantSlotEntries.size();
        if (R.mirrorVariants > 0)
            LOG_INFO("UnrealSceneImporter: baked %d mirrored mesh variant(s) for "
                     "negative-scale placements", R.mirrorVariants);
    }
    else
    {
        LOG_INFO("UnrealSceneImporter: mirror baking OFF — negative-scale "
                 "placements keep their signed scale (renderer flips the "
                 "front-face convention per draw)");
    }

    const std::string meshlibName = sceneName + ".meshlib";
    // The level library's identity as written (a re-import adopts the previous file's keys): the keys
    // the Meshes' BO / PR records name. Empty when the identity could not be written: no asset
    // collision can be keyed then.
    MeshIdentity renderId;
    {
        std::vector<uint8_t> blob =
            SceneImporter::BuildMeshLibBlob(allMeshes, allMatIdx, false, &allMirror);
        if (blob.empty())
            return Fail("BuildMeshLibBlob failed");

        // ---- Identity: one Mesh per UE static-mesh asset ----
        // Keyed by the asset path ("ue:/Game/..."), which is what a re-export of the level keeps; a section
        // by its UE slot index. A mirrored variant is an importer artefact (the author placed the asset with
        // a negative scale), so it is its own Mesh but flagged and hidden from placement.
        MeshIdentitySpec spec;
        spec.modelSource = "ue|" + sceneName;
        spec.sections.resize(allMeshes.size());
        const auto addMesh = [&](const std::string& assetPath, const std::vector<uint32_t>& slotEntries,
                                 uint8_t mask)
        {
            MeshIdentitySpec::Mesh m;
            m.name      = UELeafName(assetPath);
            m.sourceKey = "ue:" + assetPath;
            if (mask)
            {
                m.name      += " (mirrored)";
                m.sourceKey += "#mirror=" + std::to_string(static_cast<int>(mask));
                m.flags      = MESH_IDENT_MIRROR_VARIANT | MESH_IDENT_HIDDEN;
            }
            const auto ai    = meshAssets.find(assetPath);
            const json slots = (ai != meshAssets.end()) ? ai->value("material_slots", json::array())
                                                        : json::array();
            for (size_t k = 0; k < slotEntries.size(); ++k)
            {
                const uint32_t e = slotEntries[k];
                if (e == UINT32_MAX || e >= spec.sections.size()) continue;
                MeshIdentitySpec::Section& sec = spec.sections[e];
                sec.slotName  = (k < slots.size() && slots[k].is_string())
                              ? UELeafName(slots[k].get<std::string>()) : "slot" + std::to_string(k);
                sec.sourceKey = m.sourceKey + "/slot" + std::to_string(k);
                m.sections.push_back(e);
            }
            if (!m.sections.empty()) spec.meshes.push_back(std::move(m));
        };
        for (const std::string& assetPath : assetOrder)
        {
            const auto it = assetSlotEntries.find(assetPath);
            if (it != assetSlotEntries.end()) addMesh(assetPath, it->second, 0);
        }
        for (const auto& [key, entries] : variantSlotEntries)   // VariantKey: "<asset>#<mask>"
        {
            const size_t hash = key.rfind('#');
            if (hash == std::string::npos) continue;
            addMesh(key.substr(0, hash), entries,
                    static_cast<uint8_t>(std::atoi(key.c_str() + hash + 1)));
        }
        {
            MeshIdentity id;
            std::string  why;
            if (!BuildMeshIdentity(spec, blob.data(), blob.size(), id, &why) ||
                !WriteMeshLibIdentity(blob, id, &why))
                warn.Add("identity layer not written for " + meshlibName + " (" + why + ")");
            else
            {
                AdoptExistingMeshLibKeys(Sys::PathToUtf8(outDir / meshlibName), blob);
                if (ParseMeshLibIdentity(blob.data(), blob.size(), renderId, &why) != MeshIdentityParse::Ok)
                    renderId = MeshIdentity{};
            }
        }

        // Written by WriteModelImportFiles with the scene (below). The keys were adopted above already,
        // because the collision records name them; the writer's adoption finds them kept.
        levelLibBlob  = std::move(blob);
        R.meshEntries = (int)allMeshes.size();
    }
    keepAlive.clear();  // aiMesh data packed; release Assimp scenes

    // ---- Stage 3: materials ----
    prog->stage = 3; prog->done = 0; prog->total = (int)matOrder.size();

    for (const std::string& mp : matOrder)
    {
        if (Cancelled()) return Fail("cancelled");
        MatPlan& plan = matPlans[mp];
        prog->SetCurrent(plan.imatName);

        MaterialComponent mc{};
        const json& src = plan.src;

        const auto scalar = [&](const char* key, float defVal) -> float {
            if (src.is_object() && src.contains("scalars") && src["scalars"].contains(key))
                return src["scalars"][key].get<float>();
            return defVal;
        };
        const auto vec = [&](const char* key, float* out3) -> bool {
            if (src.is_object() && src.contains("vectors") && src["vectors"].contains(key)
                && src["vectors"][key].is_array() && src["vectors"][key].size() >= 3)
            {
                for (int i = 0; i < 3; ++i)
                    out3[i] = (std::max)(0.0f, src["vectors"][key][i].get<float>());
                return true;
            }
            return false;
        };
        const auto texRef = [&](const char* key) -> const TexPlan* {
            if (src.is_object() && src.contains("textures") && src["textures"].contains(key)
                && src["textures"][key].is_string())
            {
                const auto it = texPlans.find(src["textures"][key].get<std::string>());
                if (it != texPlans.end()) return &it->second;
            }
            return nullptr;
        };

        // base color tint
        float rgb[3];
        if (vec("Color Multiply", rgb) || vec("Base Color", rgb) || vec("Color", rgb))
            mc.baseColor = { rgb[0], rgb[1], rgb[2], 1.0f };

        // roughness / metalness remap ceilings
        mc.roughnessMin = 0.0f;
        mc.roughnessMax = std::clamp(scalar("Roughness", 1.0f), 0.0f, 1.0f);
        mc.metalnessMin = 0.0f;
        mc.metalnessMax = std::clamp(scalar("Metalness", 1.0f), 0.0f, 1.0f);

        // tiling
        const float uT = scalar("U Tiling", 0.0f), vT = scalar("V Tiling", 0.0f);
        if (uT > 0.0f) mc.texMulAdd.x = uT;
        if (vT > 0.0f) mc.texMulAdd.y = vT;

        // textures
        if (const TexPlan* t = texRef("Base"))
            mc.textures[MaterialComponent::BASECOLORMAP].name = t->itexRef;
        else if (const TexPlan* t2 = texRef("Text"))
        {
            mc.textures[MaterialComponent::BASECOLORMAP].name = t2->itexRef;
            warn.Add(plan.imatName + ": using 'Text' texture as base color");
        }
        if (const TexPlan* t = texRef("Normal"))
            mc.textures[MaterialComponent::NORMALMAP].name = t->itexRef;
        if (const TexPlan* t = texRef("RMO"))
            mc.textures[MaterialComponent::SURFACEMAP].name = t->itexRef;

        // emissive
        const TexPlan* emTex = texRef("Emissive");
        float tint[3] = { 1, 1, 1 };
        const bool hasTint = vec("Emissive Color", tint) || vec("Light Color", tint);
        float strength = scalar("Emissive Intensity",
                         scalar("Light Intensity", emTex || hasTint ? 1.0f : 0.0f));
        strength = std::clamp(strength, 0.0f, 200.0f);
        if (emTex)
            mc.textures[MaterialComponent::EMISSIVEMAP].name = emTex->itexRef;
        if ((emTex || hasTint) && strength > 0.0f)
            mc.emissiveColor = { tint[0], tint[1], tint[2], strength };

        // Foliage: two-sided + Masked (UE's two-sided flag is not exported; the pack's foliage lives under
        // /Foliage_* folders). BlendMode::Masked = UE BLEND_Masked semantics: unconditional clip at
        // alphaRef.
        if (ContainsCI(plan.uePath, "foliage"))
        {
            mc.flags |= MaterialComponent::DOUBLE_SIDED;
            mc.alphaRef = 0.5f;
            mc.userBlendMode = BlendMode::Masked;
        }

        // To the writer, which keeps an .imat that is already there (an author's edit survives a
        // re-import, as a model's does).
        std::vector<uint8_t> blob = BuildMaterialBlob(mc);
        if (blob.empty())
            warn.Add("cannot build material: " + plan.imatName);
        else
            matBlobs.emplace_back(sceneName + "/Materials/" + plan.imatName + ".imat", std::move(blob));
        ++prog->done;
    }

    // ---- Stage 4: scene container ----
    prog->stage = 4; prog->done = 0;
    prog->total = (int)placements.size() + 1;

    // ---- Collision: per-asset primitives + the hull library ----
    // Parsed once per asset (Unreal authors collision on the UStaticMesh), then written once per Mesh
    // that a placement uses (its BO / PR records). Absent from an export without collision data:
    // everything below then no-ops.
    std::map<std::string, std::vector<UeCollisionPrim>> assetCollision;
    std::vector<std::vector<DirectX::XMFLOAT3>>          hullPointSets;
    // (asset, primitive index, mirror mask) -> hull entry id in the collision lib
    std::map<std::tuple<std::string, size_t, uint8_t>, uint32_t> hullEntryOf;
    // A volume's hulls: (volume index, label, first entry, count) - the library's
    // identity names them too.
    struct VolumeHulls { size_t volume; std::string label; uint32_t first; uint32_t count; };
    std::vector<VolumeHulls> volumeHulls;
    const std::string collLibName = sceneName + "_collision.meshlib";

    // With mirror baking on, the render geometry is pre-mirrored and the node scale positive, so the
    // hull must be mirrored the same way or collision will not match the visual. With baking off the
    // node keeps its negative scale and Jolt's ScaledShape mirrors the hull: no variants needed.
    std::map<std::string, std::set<uint8_t>> assetMirrorMasks;
    if (opt.bakeMirroredVariants)
    {
        const auto noteMask = [&](const std::string& a, const json& scArr) {
            if (a.empty()) return;
            const uint8_t m = MirrorMaskFromUEScale(scArr);
            if (m) assetMirrorMasks[a].insert(m);
        };
        for (const auto& p : placements)
            noteMask(p.value("asset", ""), p.value("scale", json::array()));
        for (const auto& g : instanced)
        {
            const std::string a = g.value("asset", "");
            for (const auto& inst : g.value("instances", json::array()))
                noteMask(a, inst.value("scale", json::array()));
        }
    }

    for (const std::string& assetPath : assetOrder)
    {
        if (!meshAssets.contains(assetPath)) continue;
        const json& info = meshAssets[assetPath];
        if (!info.contains("collision")) continue;

        auto prims = ParseUeCollision(info["collision"], warn, UELeafName(assetPath), /*boxesAsHulls*/ false);
        for (size_t i = 0; i < prims.size(); ++i)
        {
            if (prims[i].shape != kShapeHull) continue;
            hullEntryOf[{ assetPath, i, uint8_t(0) }] =
                static_cast<uint32_t>(hullPointSets.size());
            hullPointSets.push_back(prims[i].points);

            const auto mIt = assetMirrorMasks.find(assetPath);
            if (mIt == assetMirrorMasks.end()) continue;
            for (uint8_t mask : mIt->second)
            {
                hullEntryOf[{ assetPath, i, mask }] =
                    static_cast<uint32_t>(hullPointSets.size());
                hullPointSets.push_back(MirrorCollisionPrim(prims[i], mask).points);
            }
        }
        if (!prims.empty()) assetCollision[assetPath] = std::move(prims);
    }

    std::vector<IscnNode>    nodes;
    std::vector<std::string> fLines;
    std::map<std::string, int> fDedup;   // "entry|mat" -> F index
    // The Meshes placed with collision, (asset, mirror mask) in first-use order:
    // each gets its BO / PR records once, after the collision library is written.
    std::vector<std::pair<std::string, uint8_t>> collisionMeshes;
    std::set<std::pair<std::string, uint8_t>>    collisionMeshSeen;

    const auto fIndexFor = [&](uint32_t entry, const std::string& matPath) -> int {
        const std::string key = std::to_string(entry) + "|" + matPath;
        const auto it = fDedup.find(key);
        if (it != fDedup.end()) return it->second;
        const int idx = (int)fLines.size();
        std::ostringstream fs2;
        fs2 << "F idx=" << idx << " file=" << meshlibName << " mesh=" << entry;
        if (!matPath.empty()) fs2 << " mat=" << matPath;
        fLines.push_back(fs2.str());
        fDedup.emplace(key, idx);
        return idx;
    };

    const auto emitNode = [&](const std::string& name, const std::string& assetPath,
                              const json& mats, const double p[3], const double r[4],
                              const double sc[3])
    {
        IscnNode n;
        n.trs  = ConvertTRS(p, r, sc);
        n.name = SanitizeToken(name);

        // Mirrored placement: point at the pre-mirrored variant and strip the
        // sign off the node so the instance determinant stays positive.
        const std::vector<uint32_t>* slots = nullptr;
        uint8_t mirrorMask = 0;   // non-zero only when a baked variant is used
        if (const uint8_t mask = MirrorMaskFromEngineScale(n.trs.s))
        {
            const auto vIt = variantSlotEntries.find(VariantKey(assetPath, mask));
            if (vIt != variantSlotEntries.end())
            {
                slots = &vIt->second;
                mirrorMask = mask;
                for (int k = 0; k < 3; ++k) n.trs.s[k] = std::fabs(n.trs.s[k]);
                ++R.mirroredNodes;
            }
            // else: the variant is missing (the asset failed to cook); keep the signed scale so the geometry
            // at least lands in the right place.
        }
        if (!slots)
        {
            const auto se = assetSlotEntries.find(assetPath);
            if (se != assetSlotEntries.end()) slots = &se->second;
        }

        if (slots)
        {
            const std::vector<uint32_t>& entries = *slots;
            for (size_t slot = 0; slot < entries.size(); ++slot)
            {
                if (entries[slot] == UINT32_MAX) continue;
                std::string matPath;
                if (mats.is_array() && slot < mats.size() && mats[slot].is_string())
                {
                    const auto mIt = matPlans.find(mats[slot].get<std::string>());
                    if (mIt != matPlans.end())
                        matPath = "Materials/" + mIt->second.imatName + ".imat";
                }
                n.meshRefs.push_back(fIndexFor(entries[slot], matPath));
            }
        }

        // ---- Collision: the placed Mesh's asset collision ----
        // Unreal authors collision per static mesh = per Mesh of the level library (a mirrored variant is a
        // Mesh of its own), so it is written once for it, not per placement or per F record: its body rides
        // on the Mesh's first part of every node that places it.
        if (assetCollision.count(assetPath) && !n.meshRefs.empty() &&
            collisionMeshSeen.insert({ assetPath, mirrorMask }).second)
            collisionMeshes.emplace_back(assetPath, mirrorMask);

        nodes.push_back(std::move(n));
    };

    const auto readV3 = [](const json& a, double* out) {
        for (int i = 0; i < 3; ++i) out[i] = a.at(i).get<double>();
    };
    const auto readV4 = [](const json& a, double* out) {
        for (int i = 0; i < 4; ++i) out[i] = a.at(i).get<double>();
    };

    for (const auto& p : placements)
    {
        if (Cancelled()) return Fail("cancelled");
        double pos[3], rot[4], sc[3];
        readV3(p.at("pos"), pos); readV4(p.at("rot"), rot); readV3(p.at("scale"), sc);
        emitNode(p.value("actor", "Actor"), p.value("asset", ""),
                 p.value("materials", json::array()), pos, rot, sc);
        ++prog->done;
    }

    for (const auto& g : instanced)
    {
        const std::string asset = g.value("asset", "");
        const std::string actor = g.value("actor", "Instanced");
        const json mats = g.value("materials", json::array());
        int i = 0;
        for (const auto& inst : g.value("instances", json::array()))
        {
            double pos[3], rot[4], sc[3];
            readV3(inst.at("pos"), pos); readV4(inst.at("rot"), rot); readV3(inst.at("scale"), sc);
            emitNode(actor + "_" + std::to_string(i++), asset, mats, pos, rot, sc);
        }
    }

    // ---- Geometry-free volumes (UE BlockingVolume / trigger brushes) ----
    // Placement-level, not per asset: each becomes an .iscn node carrying `V ` records instead of mesh
    // refs. No MeshLibRef = nothing to render, so the volume is invisible as in Unreal.
    for (const auto& v : j.value("volumes", json::array()))
    {
        if (Cancelled()) return Fail("cancelled");
        double pos[3] = { 0,0,0 }, rot[4] = { 0,0,0,1 }, sc[3] = { 1,1,1 };
        if (v.contains("pos"))   readV3(v["pos"], pos);
        if (v.contains("rot"))   readV4(v["rot"], rot);
        if (v.contains("scale")) readV3(v["scale"], sc);

        const std::string label = v.value("actor", "Volume");
        auto prims = ParseUeCollision(v, warn, label, /*boxesAsHulls*/ true);
        if (prims.empty())
        {
            warn.Add("volume '" + label + "' has no usable collision primitive — skipped");
            continue;
        }

        IscnNode n;
        n.trs  = ConvertTRS(pos, rot, sc);
        n.name = SanitizeToken(label);
        const bool isTrigger = v.value("trigger", false);

        VolumeHulls vh{ static_cast<size_t>(R.volumes), label, static_cast<uint32_t>(hullPointSets.size()), 0 };
        for (size_t i = 0; i < prims.size(); ++i)
        {
            const UeCollisionPrim& prim = prims[i];
            std::ostringstream vs;
            vs << " shape=" << prim.shape;
            if (prim.shape == kShapeHull)
            {
                // Volume hulls are unique to their placement, so they append to
                // the same collision library rather than being asset-keyed.
                vs << " lib=" << collLibName << " mesh=" << hullPointSets.size();
                hullPointSets.push_back(prim.points);
                ++vh.count;
            }
            else if (prim.shape == kShapeSphere)
            {
                vs << " r=" << prim.radius
                   << " off=" << prim.offset.x << "_" << prim.offset.y << "_" << prim.offset.z;
            }
            else
            {
                // A capsule's rotation would need a V record's Euler triple; a
                // volume has no render mesh to fall back on.
                warn.Add("volume '" + label + "': a " + (prim.shape == kShapeCapsule ? "capsule" : "mesh") +
                         " primitive cannot be a volume - skipped");
                continue;
            }
            if (isTrigger) vs << " trig=1";
            n.volumeLines.push_back(vs.str());
        }
        if (n.volumeLines.empty()) continue;
        if (vh.count) volumeHulls.push_back(std::move(vh));
        nodes.push_back(std::move(n));
        ++R.volumes;
    }

    // ---- The collision library: every hull, written once, with its identity ----
    // Written after the volumes, whose hulls append to it, so the one write carries the identity (a hull
    // prim names its hull by section key).
    MeshIdentity collId;   // as written (a re-import adopts the previous keys)
    if (!hullPointSets.empty())
    {
        std::vector<uint8_t> hullBlob = CollisionLibrary::BuildHullBlob(hullPointSets);
        if (!hullBlob.empty())
        {
            // Identity: one hidden Mesh per (asset, mirror mask) - "the collision
            // of SM_X" - whose sections are its hulls in primitive order, and one
            // per volume.
            MeshIdentitySpec cs;
            cs.modelSource = "ue|" + sceneName + "#collision";
            cs.sections.resize(hullPointSets.size());
            std::map<std::pair<std::string, uint8_t>, size_t> meshOfAsset;
            for (const auto& [k, e] : hullEntryOf)   // ordered by (asset, primitive, mask)
            {
                const auto& [asset, prim, mask] = k;
                const auto [it, added] = meshOfAsset.emplace(std::make_pair(asset, mask), cs.meshes.size());
                if (added)
                {
                    MeshIdentitySpec::Mesh m;
                    m.name      = UELeafName(asset) + " collision" + (mask ? " (mirrored)" : "");
                    m.sourceKey = "ue:" + asset + "#collision" +
                                  (mask ? "#mirror=" + std::to_string(static_cast<int>(mask)) : std::string());
                    m.flags     = MESH_IDENT_HIDDEN | (mask ? MESH_IDENT_MIRROR_VARIANT : 0u);
                    cs.meshes.push_back(std::move(m));
                }
                if (e >= cs.sections.size()) continue;
                MeshIdentitySpec::Mesh& m = cs.meshes[it->second];
                cs.sections[e].slotName  = "hull" + std::to_string(prim);
                cs.sections[e].sourceKey = m.sourceKey + "/hull" + std::to_string(prim);
                m.sections.push_back(e);
            }
            for (const VolumeHulls& vh : volumeHulls)
            {
                MeshIdentitySpec::Mesh m;
                m.name      = vh.label + " volume";
                m.sourceKey = "ue-volume:" + vh.label + "#" + std::to_string(vh.volume);
                m.flags     = MESH_IDENT_HIDDEN;
                for (uint32_t k = 0; k < vh.count; ++k)
                {
                    const uint32_t e = vh.first + k;
                    if (e >= cs.sections.size()) continue;
                    cs.sections[e].slotName  = "hull" + std::to_string(k);
                    cs.sections[e].sourceKey = m.sourceKey + "/hull" + std::to_string(k);
                    m.sections.push_back(e);
                }
                cs.meshes.push_back(std::move(m));
            }
            MeshIdentity id;
            std::string  why;
            if (!BuildMeshIdentity(cs, hullBlob.data(), hullBlob.size(), id, &why) ||
                !WriteMeshLibIdentity(hullBlob, id, &why))
                warn.Add("identity layer not written for " + collLibName + " (" + why + ")");
            else
            {
                AdoptExistingMeshLibKeys(Sys::PathToUtf8(outDir / collLibName), hullBlob);
                if (ParseMeshLibIdentity(hullBlob.data(), hullBlob.size(), collId, &why) != MeshIdentityParse::Ok)
                    collId = MeshIdentity{};
            }
        }
        if (hullBlob.empty() ||
            !Sys::WriteFileBytes(Sys::PathToUtf8(outDir / collLibName), hullBlob))
            warn.Add("cannot write collision library " + collLibName);
        else
            R.collisionHulls = (int)hullPointSets.size();
    }

    // ---- The Meshes' asset collision: BO / PR records ----
    std::vector<std::string> collisionLines;
    {
        std::unordered_map<std::string, uint64_t> meshKeyOf;   // source key -> MeshKey
        for (const auto& m : renderId.meshes) meshKeyOf.emplace(m.sourceKey, m.key);
        uint32_t unkeyed = 0;
        for (const auto& [asset, mask] : collisionMeshes)
        {
            const std::string src = "ue:" + asset +
                                    (mask ? "#mirror=" + std::to_string(static_cast<int>(mask)) : std::string());
            const auto mk = meshKeyOf.find(src);
            if (mk == meshKeyOf.end()) { ++unkeyed; continue; }
            const std::vector<UeCollisionPrim>& prims = assetCollision[asset];
            MeshBodySetup setup;
            setup.meshKey = mk->second;
            for (size_t i = 0; i < prims.size(); ++i)
            {
                if (prims[i].useRenderMesh)
                {
                    setup.complexity = MeshBodySetup::Complexity::Complex;   // every section's triangles
                    setup.prims.clear();
                    break;
                }
                const UeCollisionPrim p = mask ? MirrorCollisionPrim(prims[i], mask) : prims[i];
                CollisionPrim out;
                out.center = p.offset;   // a hull's points are in the Mesh's own space: 0
                if (p.shape == kShapeHull)
                {
                    const auto h = hullEntryOf.find({ asset, i, mask });
                    if (h == hullEntryOf.end() || h->second >= collId.sections.size())
                    {
                        warn.Add(UELeafName(asset) + ": hull " + std::to_string(i) +
                                 " has no key in " + collLibName + " - left out");
                        continue;
                    }
                    out.type       = CollisionPrim::Type::Hull;
                    out.lib        = collLibName;
                    out.sectionKey = collId.sections[h->second].key;
                }
                else if (p.shape == kShapeBox)
                {
                    out.type        = CollisionPrim::Type::Box;
                    out.halfExtents = p.halfExtents;
                    out.rotation    = p.rotation;
                }
                else if (p.shape == kShapeSphere)
                {
                    out.type   = CollisionPrim::Type::Sphere;
                    out.radius = p.radius;
                }
                else if (p.shape == kShapeCapsule)
                {
                    out.type       = CollisionPrim::Type::Capsule;
                    out.radius     = p.radius;
                    out.halfHeight = p.halfHeight;
                    out.rotation   = p.rotation;
                }
                else continue;
                setup.prims.push_back(std::move(out));
            }
            if (setup.complexity != MeshBodySetup::Complexity::Complex && setup.prims.empty()) continue;
            // The source's records: a re-import remakes them; an author's (no src=) on a Mesh Unreal gives
            // none survives it (WriteModelImportFiles).
            setup.source = "names";
            const std::string text = FormatBodySetup(setup);
            for (size_t b = 0; b < text.size();)
            {
                const size_t e = text.find('\n', b);
                collisionLines.push_back(text.substr(b, (e == std::string::npos ? text.size() : e) - b));
                if (e == std::string::npos) break;
                b = e + 1;
            }
            ++R.collisionMeshes;
            R.collisionPrims += static_cast<int>(setup.prims.size());
        }
        if (unkeyed)
            warn.Add(std::to_string(unkeyed) + " Mesh(es) with collision have no key in " + meshlibName +
                     " (its identity was not written) - their collision is left out");
    }

    R.nodesWritten = (int)nodes.size();
    R.fileRefs     = (int)fLines.size();

    {
        // The level library, the materials and the scene go through WriteModelImportFiles, a model's one
        // writer: an existing .imat is kept, the previous scene's material bindings are re-applied, an
        // author's collision on a Mesh Unreal gives none is kept (a removed Mesh's as orphan=1), the keys
        // are stamped (F-lines by their entry's SectionKey, N-lines by their Mesh), and the SR record names
        // the manifest. The collision library is written above on its own: its volume hulls are named by
        // position (V records).
        const std::vector<uint8_t> iscnBlob = BuildIscnBlobLocal(sceneName, nodes, fLines, collisionLines);
        std::vector<ImportFileOut> files;
        files.push_back({ sceneName + "/" + meshlibName, &levelLibBlob });
        for (const auto& [matRel, matBlob] : matBlobs) files.push_back({ matRel, &matBlob });
        files.push_back({ sceneName + "/" + sceneName + ".iscn", &iscnBlob });

        ModelSourceRecord source;
        source.path = Sys::RelativizeAssetPath(opt.manifestPath);
        std::replace(source.path.begin(), source.path.end(), '\\', '/');
        source.hash     = MeshIdentityHash(std::string_view(reinterpret_cast<const char*>(manifestBytes.data()),
                                                            manifestBytes.size()));
        source.importer = "ue";
        ModelWriteOptions wo;
        wo.source = &source;
        const ModelImportStats st = WriteModelImportFiles(
            Sys::PathToUtf8(outDir.parent_path()), files, "UE", nullptr, nullptr, &wo);
        if (st.failed)
            return Fail(std::to_string(st.failed) + " of the scene's files could not be written");
        R.materialsWritten = st.written - 2;   // less the library and the scene
        R.keptMaterials    = st.keptMaterials;
        R.restoredBindings = st.restoredBindings;
        R.keptCollision    = st.keptCollision;
        R.orphanCollision  = st.orphanCollision;
    }
    ++prog->done;

    if (R.decalsSkipped > 0)
        warn.Add(std::to_string(R.decalsSkipped) +
                 " UE decals skipped (no DecalComponent serializer yet)");
    if (j.contains("skipped_actors") && j["skipped_actors"].is_object())
        for (auto it = j["skipped_actors"].begin(); it != j["skipped_actors"].end(); ++it)
            warn.Add("UE export skipped: " + it.key());

    warn.Flush();
    prog->stage = 5;
    R.ok = true;

    LOG_SUCCESS("UnrealSceneImporter: %s done — %d tex cooked (%d up-to-date), "
                "%d assets → %d mesh entries (%d mirrored variants for %d nodes), "
                "%d materials, %d nodes, %d refs, "
                "collision: %d Mesh(es) / %d prims / %d hulls / %d volumes, %d warnings",
                sceneName.c_str(), R.texturesCooked, R.texturesSkipped, R.assetsCooked,
                R.meshEntries, R.mirrorVariants, R.mirroredNodes,
                R.materialsWritten, R.nodesWritten, R.fileRefs,
                R.collisionMeshes, R.collisionPrims, R.collisionHulls, R.volumes,
                (int)R.warnings.size());
    if (R.collisionMeshes == 0 && R.volumes == 0)
        LOG_INFO("UnrealSceneImporter: manifest carried no collision data — re-run the "
                 "UE export script with collision emission to import it");
    return R;
    }
    catch (const std::exception& e)
    {
        return Fail(std::string("exception: ") + e.what());
    }
}

} // namespace Resource
