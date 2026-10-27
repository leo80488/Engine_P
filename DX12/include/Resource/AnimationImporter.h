#pragma once

// AnimationImporter - Assimp importer for standalone animation clips: FBX / glTF /
// GLB / DAE / BVH -> .ianim. Mesh and skeleton data are ignored; bone channels,
// morph-weight channels and animated cameras are extracted.
//
// .ianim blob: [AssetHeader][AnimationMetadata], then the bone clips, the morph clips
// and the camera clips (each layout is at its writer in AnimationImporter.cpp). With
// root-motion extraction set, SerializeAnimation writes the blob instead.

#include "Resource/IImporter.h"

namespace Resource
{
    class AnimationImporter : public IImporter
    {
    public:
        // sampleRate in Hz (default 30, the editor's import rate); 0 samples at the
        // source tick rate. A rate outside 1..240 Hz falls back to 30 Hz.
        explicit AnimationImporter(float sampleRate = 30.f) : m_sampleRate(sampleRate) {}
        void SetRootMotionExtraction(std::string boneName){m_rootBone=std::move(boneName);}
        // Converts the raw source bytes to a .ianim blob; empty if the source has no
        // animations or does not load.
        std::vector<uint8_t> Import(const std::string& sourcePath,
                                     const std::vector<uint8_t>& sourceData) override;

        std::vector<const char*> GetSourceExtensions() const override
        {
            return { ".fbx", ".gltf", ".glb", ".dae", ".bvh" };
        }

        const char* GetInternalExtension() const override { return ".ianim"; }
    private:
        float m_sampleRate;
        std::string m_rootBone;
    };
}
