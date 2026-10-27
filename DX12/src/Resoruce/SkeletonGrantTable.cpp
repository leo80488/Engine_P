// SkeletonImporter::Fnv32 and BuildGrantTable, apart from the Assimp half of SkeletonImporter (EditorCore),
// because SceneInstanceLoader calls BuildGrantTable in Game.exe too, which links no Assimp. EngineCore
// compiles this file once for both exes; SkeletonImporter.cpp (EditorCore) uses them and does not define them.

#include "Resource/SkeletonImporter.h"
#include "System/Log.h"

#include <cstdint>
#include <string>

namespace Resource
{
    uint32_t SkeletonImporter::Fnv32(const char* s)
    {
        uint32_t hash = 2166136261u;
        while (*s)
        {
            hash ^= static_cast<uint8_t>(*s++);
            hash *= 16777619u;
        }
        return hash;
    }

    void SkeletonImporter::BuildGrantTable(SkeletonAsset& skel)
    {
        for (uint32_t i = 0; i < SkeletonAsset::MAX_BONES; ++i)
        {
            skel.grantSource[i] = -1;
            skel.grantRatio[i]  = 0.f;
        }

        uint32_t grantCount = 0;

        for (uint32_t i = 0; i < skel.boneCount; ++i)
        {
            const std::string name(skel.boneNames[i]);
            if (name.empty()) continue;

            // MMD D-bone: a Japanese name + a trailing ASCII 'D' copies the rotation of the same bone
            // without the 'D' (a non-ASCII byte before the 'D' tells it from a name that just ends in D).
            if (name.size() >= 2 && name.back() == 'D')
            {
                uint8_t prevByte = static_cast<uint8_t>(name[name.size() - 2]);
                if (prevByte >= 0x80)  // part of a multibyte UTF-8 character
                {
                    std::string baseName = name.substr(0, name.size() - 1);
                    uint32_t baseHash = Fnv32(baseName.c_str());
                    auto it = skel.nameToIndex.find(baseHash);
                    if (it != skel.nameToIndex.end())
                    {
                        skel.grantSource[i] = static_cast<int32_t>(it->second);
                        skel.grantRatio[i]  = 1.0f;
                        ++grantCount;
                    }
                }
            }
        }

        if (grantCount > 0)
            LOG_INFO("SkeletonImporter: detected %u D-bone grant relationships", grantCount);
    }
}
