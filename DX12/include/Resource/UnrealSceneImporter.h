#pragma once

// UnrealSceneImporter - offline cook for an Unreal-exported scene manifest.
//
// Input: a scene_manifest.json produced by the UE-side export script, sitting
// next to  meshes/   (one .gltf + .bin per unique static-mesh asset)
// and       textures/ (the original shared PNGs the UE materials reference).
//
// Output (written under <Options::outputDir>/<sceneName>/, e.g. "asset/Tokyo"):
//   <scene>.meshlib            one combined library: one MeshLibraryEntry per
//                              glTF primitive, deduplicated per asset (not per
//                              placement), so N placements share geometry,
//                              MeshDescriptors and RT BLAS.
//   <scene>.iscn               scene container: one N node per placement /
//                              foliage instance, F records deduped by
//                              (mesh entry, material).
//   Materials/<MI_x>.imat      one per unique UE material instance, textures
//                              bound + UE params mapped (see the .cpp header).
//   Textures/<T_x>.itex        cooked from the original PNGs. Base=BC7_sRGB,
//                              Normal=BC5, RMO is channel-swizzled to the
//                              engine's ARM order (R=AO,G=rough,B=metal)
//                              before BC7 compression.
//
// Transform convention (validated against an FBX ground-truth import of the
// same map, 5981/5982 matrix-exact):
//   pos' = ( x,  z, -y ) / 100      (UE cm Z-up  ->  engine m Y-up)
//   q'   = ( qx, qz, -qy, qw )
//   s'   = ( sx, sz,  sy )
//
// Editor-only (compiled in EditorCore alongside SceneImporter).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Resource
{
    class UnrealSceneImporter
    {
    public:
        struct Options
        {
            std::string manifestPath;   // UTF-8 path to scene_manifest.json
            std::string outputDir;      // asset ROOT; the scene lands in
                                        // <outputDir>/<sceneName>/. Empty = "asset"
            std::string sceneName;      // stem for .iscn/.meshlib; empty = derive
                                        // from manifest meta.source_map
            bool        skipUpToDateTextures = true; // skip .itex newer than source PNG
            // Bakes UE's negative-scale (mirrored) placements into dedicated mesh entries so every emitted
            // instance transform keeps a positive determinant. Redundant: the renderer handles mirrored
            // instances natively (per-draw rs.frontCounterClockwise from sign(det(world)),
            // GBufferPass::BuildPSODesc). On by default so existing cooked scenes keep their shape; turn it
            // off for leaner assets (Tokyo: -46 mesh entries, -82 F records) and to exercise the renderer path.
            bool        bakeMirroredVariants = true;
        };

        // Cross-thread progress mailbox: the import job writes, the UI polls.
        struct Progress
        {
            void        SetCurrent(const std::string& s) { std::lock_guard<std::mutex> l(m); cur = s; }
            std::string GetCurrent() const               { std::lock_guard<std::mutex> l(m); return cur; }
            static const char* StageName(int stage);

            std::atomic<int>  stage { 0 };   // index into StageName()
            std::atomic<int>  done  { 0 };   // items finished in current stage
            std::atomic<int>  total { 0 };   // items in current stage
            std::atomic<bool> cancel{ false };

        private:
            mutable std::mutex m;
            std::string        cur;
        };

        struct Result
        {
            bool ok = false;
            std::string error;                  // set when !ok
            std::vector<std::string> warnings;

            int texturesCooked   = 0;
            int texturesSkipped  = 0;           // up-to-date, not re-cooked
            int assetsCooked     = 0;           // unique glTF assets
            int meshEntries      = 0;           // MeshLibraryEntry count
            int mirrorVariants   = 0;           // extra entries baked for UE mirrors
            int mirroredNodes    = 0;           // placements routed to a mirrored entry
            int collisionHulls   = 0;           // convex hulls in <Scene>_collision.meshlib
            int collisionMeshes  = 0;           // Meshes given asset collision (BO records)
            int collisionPrims   = 0;           // PR records written (hull / box / sphere / capsule)
            int volumes          = 0;           // V records (UE BlockingVolume etc.)
            int materialsWritten = 0;
            // What the re-import kept (through WriteModelImportFiles): .imat files already there, the previous
            // scene's material bindings, an author's collision (and a removed Mesh's, as orphan=1).
            int keptMaterials    = 0;
            int restoredBindings = 0;
            int keptCollision    = 0;
            int orphanCollision  = 0;
            int nodesWritten     = 0;           // N records (excl. ROOT)
            int fileRefs         = 0;           // F records after dedup
            int decalsSkipped    = 0;
        };

        // Runs the whole cook synchronously on the calling thread (dispatch to
        // a worker yourself for UI use). `progress` may be null.
        static Result Run(const Options& options, Progress* progress = nullptr);
    };
}
