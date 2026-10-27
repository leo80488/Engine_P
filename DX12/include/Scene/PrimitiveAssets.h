#pragma once
// PrimitiveAssets: the Create > 3D Object shapes as an engine model.
//
// engine/meshes/Primitives.meshlib + Primitives.iscn hold the five ProceduralMesh shapes, in
// PrimitiveMeshType order, as five Meshes named Cube, Sphere, Cone, Plane and Torus - one
// section each, the engine default material - so a StaticMesh places a cube exactly like any
// other model's Mesh (material slots, asset collision, the Mesh Editor). Everything under
// engine/ ships in a package, so the model always exists at runtime.
//
// The MeshHandle primitive (gpuMeshID = PrimitiveMeshType, drawn from MeshManager's persistent
// slots) stays: scenes hold hundreds of them, and Editor.CreatePrimitive without
// {staticMesh = true} still makes one.
//
// Write() is deterministic (the same bytes every run) and is what `-assettool=bake_primitives`
// runs; the files are committed.

#include <cstdint>
#include <string>

namespace PrimitiveAssets
{
    constexpr const char* kModel   = "engine/meshes/Primitives.iscn";
    constexpr const char* kLibrary = "engine/meshes/Primitives.meshlib";

    // "Cube", "Sphere", "Cone", "Plane", "Torus"; nullptr out of range.
    const char* MeshName(int primitiveType);

    struct Stats
    {
        std::string iscn, meshlib;
        uint32_t    meshes = 0, vertices = 0, indices = 0;
        uint32_t    meshletClusters = 0;   // meshlet clusters in the MLET chunk
    };
    // Build both files into @p dir ("" = engine/meshes), atomically. False
    // with @p err set when a write fails.
    bool Write(const std::string& dir, Stats& out, std::string* err = nullptr);
}
