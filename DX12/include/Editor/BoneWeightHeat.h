#pragma once

// The Skeleton Editor's weight heatmap: the model's skinned parts coloured by how much one bone
// moves each vertex, from dark blue (not at all) through cyan, green and yellow to red (it alone),
// with the Mesh Editor's shape-key ramp (Editor/HeatRamp.h).
// Pairing: a skinned part's weights are the .iskel blend stream of its F-line (stream meshIndex ==
// F index), vertex for vertex against that F-line's library entry - the rule
// SceneInstanceLoader::Load registers a skinned mesh by, including its "VERTEX COUNT MISMATCH"
// check (such a part is not shown). The Model view's parts are
// SceneInstanceLoader::ReadMeshPlacements' placements in order (Preview's ComposeModelAsset);
// MeshPlacement::fileIndex is each one's F index.
// Picture (MeshShapeKeys' recipe, Editor/MeshShapeKeys.h): library slots are shared with every
// placement in the world, so each skinned part gets the panel's own copy - one UPLOAD buffer and
// one mesh-descriptor slot - drawn instead of the library's (Preview::ViewDesc::partMeshes) in a
// white vertex-colour material (vertexColorParts) while the heatmap is on. Only the colours change
// with the bone: three colour regions, one per version, re-pointed with a stable UpdateMesh at most
// once per frame; the geometry is written once. Made lazily on the game half (the heap refuses
// anything else) and forgotten, not freed, after a world clear, which rewinds the heap's
// world-scoped slots under it.
// Editor-only: compiled into EditorCore.

#include "Graphics/GraphicsStruct.h"   // RHI::GPUBuffer, RHI::MeshDescriptor, RHI::kInvalidBufferIndex

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class IGraphicsDevice;
class MeshDescriptorHeap;
struct BlendVertex;
namespace Preview { struct ViewDesc; }

namespace Editor
{
    class BoneWeightHeat
    {
    public:
        // One blend stream of the .iskel (SceneInstanceLoader::SkinBlendStream).
        struct Stream
        {
            std::uint32_t      meshIndex = 0;   // the F index it skins
            const BlendVertex* weights   = nullptr;
            std::size_t        count     = 0;
        };

        struct SectionIn
        {
            std::string   libraryPath;   // the Mesh's .meshlib
            std::uint32_t entry = 0;     // the section's entry in it
            std::uint64_t key   = 0;     // its SectionKey (an F-line's sec=)
        };

        // Pair @p model's parts with @p streams. Call Release first when copies may
        // exist. False + @p why only when the model does not read; a skinned part
        // that cannot be paired is skipped (logged, counted).
        bool Load(const std::string& model, const std::vector<Stream>& streams, std::string& why);

        // The Mesh Editor's picture, where part k is section k of one Mesh (a MeshAsset composition): each
        // section is paired with the stream of the model's first F-line that places its library entry.
        // Same contract as Load (call Release first when copies may exist).
        bool LoadSections(const std::string& model, const std::vector<SectionIn>& sections,
                          const std::vector<Stream>& streams, std::string& why);
        // The log lines' prefix ("[SKELEDITOR]" unless set).
        void SetTag(const char* tag) { m_tag = tag ? tag : "[SKELEDITOR]"; }
        // [bone] = the paired vertices it moves at all (a weight > 0); @p boneCount long.
        std::vector<std::uint32_t> BoneVertexCounts(std::uint32_t boneCount) const;
        // Nothing paired (call Release first when copies may exist).
        void Clear();
        // GAME half: hand the GPU copies back (see MeshShapeKeys::Release).
        void Release(IGraphicsDevice* gfx, MeshDescriptorHeap* heap, std::uint32_t worldEpoch);
        bool HasGpu() const;
        // GAME half, once per frame: make the copies when first needed, rewrite the
        // colours when the bone changed.
        void Update(IGraphicsDevice& gfx, MeshDescriptorHeap& heap, std::uint32_t worldEpoch);
        // The picture's overrides (nothing while off).
        void Apply(Preview::ViewDesc& d) const;

        void SetBone(int bone);   // -1 = off
        int  Bone() const { return m_bone; }
        bool Active() const { return m_bone >= 0 && !m_parts.empty(); }
        // The copies exist and carry the current bone (the picture shows it).
        bool Live() const { return m_gpuReady && !m_dirty && Active(); }
        std::uint64_t Revision() const { return m_revision; }
        std::size_t   Parts() const { return m_parts.size(); }   // skinned parts paired
        std::size_t   Skipped() const { return m_skipped; }      // skinned parts that could not be paired

    private:
        struct Part
        {
            std::uint32_t                              placement = 0;   // the Model view's part index
            std::uint32_t                              n = 0;
            std::vector<float>                         pos, nrm, tan;   // tan empty: none
            std::vector<float>                         uv[4];
            std::vector<std::uint32_t>                 idx;
            std::vector<std::array<std::uint16_t, 8>>  bones;           // BlendVertex's, per vertex
            std::vector<std::array<std::uint8_t, 8>>   weights;
            // GPU copy
            RHI::GPUBuffer buf;
            std::uint8_t*  mapped   = nullptr;
            std::uint32_t  bindless = RHI::kInvalidBufferIndex;
            std::uint32_t  slot     = RHI::kInvalidBufferIndex;
            std::uint64_t  posAt = 0, nrmAt = 0, tanAt = 0, uvAt[4] = {}, idxAt = 0, colAt = 0;
        };

        struct LibCache;

        // One part: @p meshPath's @p entry against @p bs (its vertex count checked);
        // @p libs caches the libraries read. False = skipped (logged, counted).
        bool AddPart(LibCache& libs, const std::string& model, const std::string& meshPath, std::uint32_t entry,
                     std::uint32_t fileIndex, std::uint32_t partIndex, const Stream& bs);
        bool Create(IGraphicsDevice& gfx, MeshDescriptorHeap& heap);
        void Colour(const Part& p, std::uint8_t* dst) const;   // one region's colours
        RHI::MeshDescriptor Describe(const Part& p, std::uint32_t region) const;

        std::vector<Part> m_parts;
        const char*       m_tag      = "[SKELEDITOR]";
        std::size_t       m_skipped  = 0;
        int               m_bone     = -1;
        bool              m_dirty    = true;
        bool              m_gpuReady = false;
        bool              m_failed   = false;
        std::uint64_t     m_revision = 0;
        std::uint32_t     m_epoch    = 0;
    };
}
