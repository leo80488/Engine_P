#pragma once

// MeshShapeKeys - the Mesh Editor's Shape Keys: the Mesh's morph targets, a weight per
// target NAME, and the Mesh drawn deformed in the panel's picture.
//
// Data: the model's M lines name its .imorph; a section's block is found as the loader
// finds it (Resource::MorphLibrary::File::Find - by section key in a keyed file, else
// by entry). A target is a NAME: the union over the Mesh's sections, one weight each,
// applied to every section that carries it.
//
// Picture: the library slots are shared with every placement in the world, so the
// deformed copy is the panel's own - per section with targets one UPLOAD buffer and
// one mesh-descriptor slot, drawn instead of the library's (ViewDesc::partMeshes) while
// a weight is above zero or the heatmap is on. The evaluation is Skin.cs's, on the CPU:
// pos += w x dp, nrm = normalize(n + sum w x dn), tangent re-orthogonalised, weights
// <= 0.001 skipped. The HEATMAP colours each vertex by |dp| of one target
// (ViewDesc::vertexColorParts: a white material times the vertex colour).
//
// GPU rules: the changing streams (position, normal, tangent, colour) live in three
// regions of the buffer, one per version; a version writes region (revision % 3) and
// re-points the slot with a STABLE UpdateMesh, at most one version per frame - so a
// region is rewritten only after the frames that could read it retired. The static
// streams (uv0..3, local indices) are written once. Everything is created lazily (when
// something is first active), on the GAME half only (the heap refuses anything else),
// and forgotten - not freed - after a world clear, which rewound the heap's
// world-scoped slots under it.
//
// Editor-only TU: compiled into EditorCore.

#include "Graphics/GraphicsStruct.h"   // RHI::GPUBuffer, RHI::kInvalidBufferIndex

#include <cstdint>
#include <string>
#include <vector>

class IGraphicsDevice;
class MeshDescriptorHeap;
namespace Preview { struct ViewDesc; }

namespace Editor
{
    class MeshShapeKeys
    {
    public:
        // One section of the Mesh as the panel decoded it (Scene/MeshGeometry.h):
        // local positions / normals / tangents (may be empty) / uv0..3 (each may be
        // empty) / colours (RGBA8, may be empty) / local indices.
        struct SectionIn
        {
            std::uint32_t                     entry = 0;   // library entry == the F-line index of an imported model
            std::uint64_t                     key   = 0;   // its SectionKey
            const std::vector<float>*         pos = nullptr;
            const std::vector<float>*         nrm = nullptr;
            const std::vector<float>*         tan = nullptr;
            const std::vector<float>*         uv[4] = {};
            const std::vector<std::uint32_t>* col = nullptr;
            const std::vector<std::uint32_t>* idx = nullptr;
        };

        struct Snapshot
        {
            std::vector<std::pair<std::string, float>> weights;
            std::string solo, heatmap;
        };

        // Read @p model's targets for these sections (placement k = section k).
        // Call Release first when copies may exist. A Mesh without shape keys
        // loads with Count() == 0; false + @p why only when a named .imorph does
        // not read.
        bool Load(const std::string& model, const std::vector<SectionIn>& sections, std::string& why);
        // GAME half: hand the GPU copies back (the heap slots now, the buffers
        // deferred by the device). After a world clear (@p worldEpoch moved) the
        // slots are forgotten, never freed: the clear rewound them. Null
        // arguments just forget.
        void Release(IGraphicsDevice* gfx, MeshDescriptorHeap* heap, std::uint32_t worldEpoch);
        bool HasGpu() const;
        // GAME half, once per frame: make the copies when first needed (and again
        // after a world clear - @p worldEpoch), and evaluate + upload when a
        // weight, the solo or the heatmap changed.
        void Update(IGraphicsDevice& gfx, MeshDescriptorHeap& heap, std::uint32_t worldEpoch);
        // The picture's overrides (nothing while nothing is active).
        void Apply(Preview::ViewDesc& d) const;

        // ---- The targets ----
        std::size_t        Count() const { return m_names.size(); }
        const std::string& Name(std::size_t i) const { return m_names[i]; }
        int                Find(const std::string& name) const;   // -1 = none
        float              Weight(std::size_t i) const { return m_weights[i]; }
        bool               SetWeight(std::size_t i, float w);     // clamped to [0, 1]
        void               ResetWeights();
        int                Solo() const { return m_solo; }        // -1: every weight counts
        bool               SetSolo(int i);                        // only target i counts
        int                Heatmap() const { return m_heatmap; }  // -1 = off
        bool               SetHeatmap(int i);
        std::uint32_t      SectionsOf(std::size_t i) const;       // sections target i moves
        float              MaxDelta(std::size_t i) const { return m_maxDelta[i]; }   // max |dp|, m
        std::uint32_t      MorphSections() const;                 // sections that carry targets
        bool               HasNormals() const { return m_hasNormals; }
        const std::string& FilePath() const { return m_file; }    // the .imorph read ("" = none)
        std::uint64_t      Revision() const { return m_revision; }

        // The preview state by NAME (weights, solo, heatmap) - what survives a
        // reload of the same Mesh (a Save re-reads the model).
        Snapshot Snap() const;
        void     Restore(const Snapshot& s);
        // Something to draw: a weight that counts, or the heatmap.
        bool               Active() const;
        // The copies exist and carry the current state (the picture uses them).
        bool               Live() const { return m_gpuReady && !m_dirty; }

    private:
        struct Target
        {
            std::uint32_t      name = 0;     // index into m_names
            std::vector<float> pos, nrm;     // xyz per vertex; nrm empty = the file has none
        };
        struct Sec
        {
            std::uint32_t              placement = 0;
            std::uint32_t              n = 0;
            std::vector<float>         basePos, baseNrm, baseTan;
            std::vector<float>         uv[4];
            std::vector<std::uint32_t> baseCol, idx;
            std::vector<Target>        targets;
            // The panel's GPU copy.
            RHI::GPUBuffer buf;
            std::uint8_t*  mapped   = nullptr;
            std::uint32_t  bindless = RHI::kInvalidBufferIndex;
            std::uint32_t  slot     = RHI::kInvalidBufferIndex;
            std::uint64_t  region   = 0;       // bytes of one version: pos, nrm, (tan), colour
            std::uint64_t  uvAt[4]  = {}, idxAt = 0;
        };

        float EffectiveWeight(std::uint32_t name) const;
        bool  Create(IGraphicsDevice& gfx, MeshDescriptorHeap& heap);
        void  Evaluate(Sec& s, std::uint8_t* dst) const;    // one region's streams
        RHI::MeshDescriptor Describe(const Sec& s, std::uint32_t region) const;

        std::vector<Sec>         m_secs;
        std::vector<std::string> m_names;
        std::vector<float>       m_weights, m_maxDelta;
        int                      m_solo = -1, m_heatmap = -1;
        bool                     m_hasNormals = false;
        std::string              m_file;

        bool          m_dirty    = true;    // the copies do not carry the current state
        bool          m_gpuReady = false;
        bool          m_failed   = false;   // the copies could not be made: not retried until Load
        std::uint64_t m_revision = 0;       // bumped per upload: what the picture is told
        std::uint32_t m_epoch    = 0;
    };
}
