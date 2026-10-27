#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Resource/PmxImporter.h"
#include "Resource/AssetHeader.h"
#include "Resource/MeshLibIdentity.h"   // IDNT - the .meshlib's Mesh/Section keys
#include "Resource/MorphLibrary.h"      // .imorph v2 writer
#include "Resource/ModelShapeKeys.h"    // ShapeKeyPreset (a group morph)
#include "Resource/ModelMaterialImport.h"   // ModelTextureCook (the materials' textures)
#include "Resource/HumanoidProfileBuilder.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/AssetRefs.h"        // PercentEncodeStrict - .iscn F-line name= value
#include "ECS/Components.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <DirectXMath.h>
#include <windows.h>   // WideCharToMultiByte

using namespace DirectX;

namespace Resource
{
    // ==== Internal helpers ====

    // PMX header config parsed from the globals block.
    struct PmxConfig
    {
        uint8_t  encoding;          // 0 = UTF-16LE, 1 = UTF-8
        uint8_t  additionalUVCount; // 0-4
        uint8_t  vertexIndexSize;   // 1, 2, or 4
        uint8_t  textureIndexSize;
        uint8_t  materialIndexSize;
        uint8_t  boneIndexSize;
        uint8_t  morphIndexSize;
        uint8_t  rigidBodyIndexSize;
        float    version;
    };

    // Bounds-checked readers: return false, without advancing, when the bytes run past end.
    template<typename T>
    static bool Read(const uint8_t*& p, const uint8_t* end, T& out)
    {
        if (p + sizeof(T) > end) return false;
        std::memcpy(&out, p, sizeof(T));
        p += sizeof(T);
        return true;
    }

    static bool Skip(const uint8_t*& p, const uint8_t* end, size_t bytes)
    {
        if (p + bytes > end) return false;
        p += bytes;
        return true;
    }

    // Reads a 1/2/4-byte index. 1- and 2-byte indices are unsigned with an all-ones sentinel
    // (0xFF / 0xFFFF); 4-byte ones are signed. Returns -1 for the "no reference" sentinel or a
    // short buffer.
    static int32_t ReadIndex(const uint8_t*& p, const uint8_t* end, uint8_t sz)
    {
        if (p + sz > end) { p = end; return -1; }
        int32_t v = -1;
        switch (sz)
        {
        case 1: { uint8_t  t; std::memcpy(&t, p, 1); v = (t == 0xFF) ? -1 : static_cast<int32_t>(t); } break;
        case 2: { uint16_t t; std::memcpy(&t, p, 2); v = (t == 0xFFFF) ? -1 : static_cast<int32_t>(t); } break;
        case 4: { int32_t  t; std::memcpy(&t, p, 4); v = t; } break; // 4-byte stays signed per PMX spec
        }
        p += sz;
        return v;
    }

    // Reads an unsigned 1/2/4-byte vertex index (0 on a short buffer).
    static uint32_t ReadVertexIndex(const uint8_t*& p, const uint8_t* end, uint8_t sz)
    {
        if (p + sz > end) { p = end; return 0; }
        uint32_t v = 0;
        switch (sz)
        {
        case 1: { uint8_t  t; std::memcpy(&t, p, 1); v = t; } break;
        case 2: { uint16_t t; std::memcpy(&t, p, 2); v = t; } break;
        case 4: { uint32_t t; std::memcpy(&t, p, 4); v = t; } break;
        }
        p += sz;
        return v;
    }

    // Reads a PMX text field (int32 byte length + bytes) as UTF-8; empty on a bad length.
    static std::string ReadText(const uint8_t*& p, const uint8_t* end, uint8_t encoding)
    {
        int32_t byteLen = 0;
        if (!Read(p, end, byteLen) || byteLen <= 0)
            return {};
        if (p + byteLen > end) { p = end; return {}; }

        std::string result;
        if (encoding == 1) // UTF-8
        {
            result.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(byteLen));
        }
        else // UTF-16LE -> UTF-8
        {
            const int wLen = byteLen / 2;
            const wchar_t* wStr = reinterpret_cast<const wchar_t*>(p);
            int uLen = WideCharToMultiByte(CP_UTF8, 0, wStr, wLen, nullptr, 0, nullptr, nullptr);
            if (uLen > 0)
            {
                result.resize(static_cast<size_t>(uLen));
                WideCharToMultiByte(CP_UTF8, 0, wStr, wLen, result.data(), uLen, nullptr, nullptr);
            }
        }
        p += byteLen;
        return result;
    }

    // No axis flip: MMD and the engine are both left-handed (X right, Y up). Only the facing along Z
    // differs (an MMD model faces the viewer); camera placement handles that, not mirroring the model.
    static XMFLOAT3 ConvertPos3(float x, float y, float z) { return { x, y, z }; }

    // No quaternion conversion needed (same handedness).
    static XMFLOAT4 ConvertQuat(float qx, float qy, float qz, float qw)
    {
        return { qx, qy, qz, qw };
    }

    // FNV-1a 32-bit hash (SkeletonAsset::nameToIndex keys).
    static uint32_t Fnv32(const char* s)
    {
        uint32_t h = 2166136261u;
        while (*s) { h ^= static_cast<uint8_t>(*s++); h *= 16777619u; }
        return h;
    }

    // ==== PmxImporter::Import ====
    PmxImportResult PmxImporter::Import(const uint8_t* data, size_t size)
    {
        PmxImportResult result;
        const uint8_t* p   = data;
        const uint8_t* end = data + size;

        // ---- Header ----
        // Magic "PMX " (4 bytes)
        if (size < 4 || std::memcmp(p, "PMX ", 4) != 0)
        {
            LOG_ERROR("PmxImporter: invalid magic");
            return result;
        }
        p += 4;

        PmxConfig cfg{};
        if (!Read(p, end, cfg.version))                    return result;
        if (cfg.version < 2.0f || cfg.version > 2.2f)
        {
            LOG_ERROR("PmxImporter: unsupported version %.1f", cfg.version);
            return result;
        }

        uint8_t globalsCount = 0;
        if (!Read(p, end, globalsCount))                   return result;
        if (globalsCount < 8 || p + globalsCount > end)
        {
            LOG_ERROR("PmxImporter: bad globals count %u", globalsCount);
            return result;
        }
        cfg.encoding          = p[0];
        cfg.additionalUVCount = p[1];
        cfg.vertexIndexSize   = p[2];
        cfg.textureIndexSize  = p[3];
        cfg.materialIndexSize = p[4];
        cfg.boneIndexSize     = p[5];
        cfg.morphIndexSize    = p[6];
        cfg.rigidBodyIndexSize= p[7];
        p += globalsCount;

        LOG_INFO("PmxImporter: version=%.1f encoding=%u addlUV=%u "
                 "vertIdxSz=%u texIdxSz=%u matIdxSz=%u boneIdxSz=%u morphIdxSz=%u rbIdxSz=%u",
                 cfg.version, cfg.encoding, cfg.additionalUVCount,
                 cfg.vertexIndexSize, cfg.textureIndexSize, cfg.materialIndexSize,
                 cfg.boneIndexSize, cfg.morphIndexSize, cfg.rigidBodyIndexSize);

        // Model name / comments (skip)
        ReadText(p, end, cfg.encoding); // name JP
        ReadText(p, end, cfg.encoding); // name EN
        ReadText(p, end, cfg.encoding); // comment JP
        ReadText(p, end, cfg.encoding); // comment EN

        // ---- Vertices ----
        int32_t vertexCount = 0;
        if (!Read(p, end, vertexCount) || vertexCount <= 0)
        {
            LOG_ERROR("PmxImporter: bad vertex count");
            return result;
        }

        result.positions.resize(static_cast<size_t>(vertexCount));
        result.normals.resize(static_cast<size_t>(vertexCount));
        result.uvs.resize(static_cast<size_t>(vertexCount));
        result.blendData.resize(static_cast<size_t>(vertexCount));
        if (cfg.additionalUVCount > 0)
            result.uv1.resize(static_cast<size_t>(vertexCount));

        if (cfg.additionalUVCount > 1) result.uv2.resize(static_cast<size_t>(vertexCount));
        if (cfg.additionalUVCount > 2) result.uv3.resize(static_cast<size_t>(vertexCount));

        for (int32_t vi = 0; vi < vertexCount; ++vi)
        {
            float px, py, pz, nx, ny, nz, u, v;
            if (!Read(p, end, px)) return result;
            if (!Read(p, end, py)) return result;
            if (!Read(p, end, pz)) return result;
            if (!Read(p, end, nx)) return result;
            if (!Read(p, end, ny)) return result;
            if (!Read(p, end, nz)) return result;
            if (!Read(p, end, u))  return result;
            if (!Read(p, end, v))  return result;

            result.positions[vi] = ConvertPos3(px, py, pz);
            result.normals[vi]   = ConvertPos3(nx, ny, nz);
            result.uvs[vi]       = { u, v };

            // PMX additional UVs are float4; expose the first three sets' xy.
            for (uint8_t channel = 0; channel < cfg.additionalUVCount; ++channel)
            {
                float x, y, z, w;
                if (!Read(p, end, x) || !Read(p, end, y) ||
                    !Read(p, end, z) || !Read(p, end, w)) return result;
                if (channel == 0) result.uv1[vi] = { x, y };
                if (channel == 1) result.uv2[vi] = { x, y };
                if (channel == 2) result.uv3[vi] = { x, y };
            }

            uint8_t weightType = 0;
            if (!Read(p, end, weightType)) return result;

            BlendVertex& bv = result.blendData[vi];
            std::memset(&bv, 0, sizeof(bv));

            switch (weightType)
            {
            case 0: // BDEF1
            {
                int32_t b0 = ReadIndex(p, end, cfg.boneIndexSize);
                bv.boneIndices[0] = static_cast<uint16_t>(std::clamp(b0, 0, 65535));
                bv.boneWeights[0] = 255;
                break;
            }
            case 1: // BDEF2
            {
                int32_t b0 = ReadIndex(p, end, cfg.boneIndexSize);
                int32_t b1 = ReadIndex(p, end, cfg.boneIndexSize);
                float w0; Read(p, end, w0);
                bv.boneIndices[0] = static_cast<uint16_t>(std::clamp(b0, 0, 65535));
                bv.boneIndices[1] = static_cast<uint16_t>(std::clamp(b1, 0, 65535));
                bv.boneWeights[0] = static_cast<uint8_t>(std::clamp(w0 * 255.f + 0.5f, 0.f, 255.f));
                bv.boneWeights[1] = static_cast<uint8_t>(255 - bv.boneWeights[0]);
                break;
            }
            case 2: // BDEF4
            {
                int32_t bi[4]; float bw[4];
                for (int k = 0; k < 4; ++k) bi[k] = ReadIndex(p, end, cfg.boneIndexSize);
                for (int k = 0; k < 4; ++k) Read(p, end, bw[k]);
                float sum = bw[0] + bw[1] + bw[2] + bw[3];
                if (sum < 1e-6f) sum = 1.f;
                for (int k = 0; k < 4; ++k)
                {
                    bv.boneIndices[k] = static_cast<uint16_t>(std::clamp(bi[k], 0, 65535));
                    bv.boneWeights[k] = static_cast<uint8_t>(std::clamp(bw[k] / sum * 255.f + 0.5f, 0.f, 255.f));
                }
                break;
            }
            case 3: // SDEF - treat as BDEF2
            {
                int32_t b0 = ReadIndex(p, end, cfg.boneIndexSize);
                int32_t b1 = ReadIndex(p, end, cfg.boneIndexSize);
                float w0; Read(p, end, w0);
                Skip(p, end, 36); // C(f3) + R0(f3) + R1(f3)
                bv.boneIndices[0] = static_cast<uint16_t>(std::clamp(b0, 0, 65535));
                bv.boneIndices[1] = static_cast<uint16_t>(std::clamp(b1, 0, 65535));
                bv.boneWeights[0] = static_cast<uint8_t>(std::clamp(w0 * 255.f + 0.5f, 0.f, 255.f));
                bv.boneWeights[1] = static_cast<uint8_t>(255 - bv.boneWeights[0]);
                break;
            }
            case 4: // QDEF - treat as BDEF4
            {
                int32_t bi[4]; float bw[4];
                for (int k = 0; k < 4; ++k) bi[k] = ReadIndex(p, end, cfg.boneIndexSize);
                for (int k = 0; k < 4; ++k) Read(p, end, bw[k]);
                float sum = bw[0] + bw[1] + bw[2] + bw[3];
                if (sum < 1e-6f) sum = 1.f;
                for (int k = 0; k < 4; ++k)
                {
                    bv.boneIndices[k] = static_cast<uint16_t>(std::clamp(bi[k], 0, 65535));
                    bv.boneWeights[k] = static_cast<uint8_t>(std::clamp(bw[k] / sum * 255.f + 0.5f, 0.f, 255.f));
                }
                break;
            }
            default:
                LOG_ERROR("PmxImporter: unknown weight type %u at vertex %d", weightType, vi);
                return result;
            }

            // Edge scale (skip)
            float edgeScale; Read(p, end, edgeScale);
        }

        // Log the highest bone index and the zero-weight vertices.
        {
            uint32_t maxBoneIdx = 0;
            uint32_t zeroWeightCount = 0;
            uint32_t highBoneCount = 0;  // bone index >= 128
            for (int32_t vi = 0; vi < vertexCount; ++vi)
            {
                const BlendVertex& bv = result.blendData[vi];
                uint32_t wsum = 0;
                for (int k = 0; k < 4; ++k)
                {
                    if (bv.boneWeights[k] > 0)
                    {
                        if (bv.boneIndices[k] > maxBoneIdx) maxBoneIdx = bv.boneIndices[k];
                        if (bv.boneIndices[k] >= 128) ++highBoneCount;
                    }
                    wsum += bv.boneWeights[k];
                }
                if (wsum == 0) ++zeroWeightCount;
            }
            LOG_INFO("PmxImporter: %d vertices — maxBoneIdx=%u  highBone(>=128)=%u  zeroWeight=%u",
                     vertexCount, maxBoneIdx, highBoneCount, zeroWeightCount);
        }

        // ---- Faces ----
        int32_t indexCount = 0;
        if (!Read(p, end, indexCount) || indexCount <= 0)
        {
            LOG_ERROR("PmxImporter: bad index count");
            return result;
        }
        result.indices.resize(static_cast<size_t>(indexCount));
        for (int32_t i = 0; i < indexCount; ++i)
            result.indices[i] = ReadVertexIndex(p, end, cfg.vertexIndexSize);

        // No winding reversal: MMD and DX12 share the same left-handed convention.

        LOG_INFO("PmxImporter: %d indices (%d triangles)", indexCount, indexCount / 3);

        // ---- Textures ----
        int32_t textureCount = 0;
        if (!Read(p, end, textureCount)) return result;
        result.textureFiles.resize(static_cast<size_t>(std::max(textureCount, 0)));
        for (int32_t i = 0; i < textureCount; ++i)
            result.textureFiles[i] = ReadText(p, end, cfg.encoding);

        LOG_INFO("PmxImporter: %d textures", textureCount);

        // ---- Materials ----
        int32_t materialCount = 0;
        if (!Read(p, end, materialCount)) return result;

        uint32_t indexOffset = 0;
        result.materials.resize(static_cast<size_t>(std::max(materialCount, 0)));
        for (int32_t mi = 0; mi < materialCount; ++mi)
        {
            PmxMaterial& mat = result.materials[mi];
            mat.nameJP = ReadText(p, end, cfg.encoding);
            mat.nameEN = ReadText(p, end, cfg.encoding);

            Read(p, end, mat.diffuse);
            Read(p, end, mat.specular);
            Read(p, end, mat.shininess);
            Read(p, end, mat.ambient);

            uint8_t drawFlags = 0; Read(p, end, drawFlags);
            mat.drawFlags = drawFlags;
            Skip(p, end, 16); // edge color (f4)
            float edgeSize; Read(p, end, edgeSize);

            mat.textureIndex       = ReadIndex(p, end, cfg.textureIndexSize);
            mat.sphereTextureIndex = ReadIndex(p, end, cfg.textureIndexSize);
            Read(p, end, mat.sphereMode);

            uint8_t toonFlag; Read(p, end, toonFlag);
            if (toonFlag == 0)
                mat.toonTextureIndex = ReadIndex(p, end, cfg.textureIndexSize);
            else
            {
                uint8_t builtinIdx; Read(p, end, builtinIdx);
                mat.toonTextureIndex = -1; // built-in toon
            }

            ReadText(p, end, cfg.encoding); // memo

            int32_t faceIndexCount = 0;
            Read(p, end, faceIndexCount);
            mat.indexStart = indexOffset;
            mat.indexCount = static_cast<uint32_t>(std::max(faceIndexCount, 0));
            indexOffset += mat.indexCount;
        }

        // Log each material's index range and highest bone index.
        for (int32_t mi = 0; mi < materialCount; ++mi)
        {
            const PmxMaterial& mat = result.materials[mi];
            uint32_t maxBI = 0;
            for (uint32_t ii = 0; ii < mat.indexCount; ++ii)
            {
                uint32_t vi = result.indices[mat.indexStart + ii];
                if (vi < static_cast<uint32_t>(vertexCount))
                {
                    const BlendVertex& bv = result.blendData[vi];
                    for (int k = 0; k < 4; ++k)
                        if (bv.boneWeights[k] > 0 && bv.boneIndices[k] > maxBI)
                            maxBI = bv.boneIndices[k];
                }
            }
            LOG_INFO("PmxImporter:   mat[%d] idxStart=%u idxCount=%u maxBoneIdx=%u tex=%d",
                     mi, mat.indexStart, mat.indexCount, maxBI, mat.textureIndex);
        }
        LOG_INFO("PmxImporter: %d materials", materialCount);

        // ---- Bones ----
        int32_t boneCount = 0;
        if (!Read(p, end, boneCount) || boneCount <= 0)
        {
            LOG_ERROR("PmxImporter: bad bone count");
            return result;
        }
        if (static_cast<uint32_t>(boneCount) > SkeletonAsset::MAX_BONES)
        {
            LOG_ERROR("PmxImporter: bone count %d exceeds MAX_BONES=%u", boneCount, SkeletonAsset::MAX_BONES);
            return result;
        }

        SkeletonAsset& skel = result.skeleton;
        skel.boneCount = static_cast<uint32_t>(boneCount);

        // Default: no grant (append) source.
        for (uint32_t i = 0; i < SkeletonAsset::MAX_BONES; ++i)
        {
            skel.grantSource[i] = -1;
            skel.grantRatio[i]  = 0.f;
        }

        // Model-space rest positions, turned into matrices after the bone loop.
        std::vector<XMFLOAT3> boneWorldPos(static_cast<size_t>(boneCount));

        for (int32_t bi = 0; bi < boneCount; ++bi)
        {
            std::string nameJP = ReadText(p, end, cfg.encoding);
            ReadText(p, end, cfg.encoding); // name EN

            strncpy_s(skel.boneNames[bi], sizeof(skel.boneNames[0]),
                      nameJP.c_str(), _TRUNCATE);
            skel.boneNames[bi][63] = '\0';

            // Rest position, model space (no axis conversion).
            float bx, by, bz;
            Read(p, end, bx); Read(p, end, by); Read(p, end, bz);
            boneWorldPos[bi] = ConvertPos3(bx, by, bz);

            skel.parentIndex[bi] = ReadIndex(p, end, cfg.boneIndexSize);

            int32_t transOrder = 0;
            Read(p, end, transOrder);
            skel.transformOrder[bi] = transOrder;

            uint16_t flags = 0;
            Read(p, end, flags);

            // Tail: a bone index or an offset; neither is used.
            if (flags & 0x0001) // tail is bone index
                ReadIndex(p, end, cfg.boneIndexSize); // skip
            else
                Skip(p, end, 12); // tail offset (f3)

            // Grant (MMD's append / fuyo): bit 8 inherits rotation, bit 9 translation; both read one
            // source bone and ratio.
            if (flags & 0x0100 || flags & 0x0200)
            {
                int32_t grantParent = ReadIndex(p, end, cfg.boneIndexSize);
                float   grantRatio  = 0.f;
                Read(p, end, grantRatio);
                skel.grantSource[bi] = grantParent;
                skel.grantRatio[bi]  = grantRatio;
            }

            // Fixed axis
            if (flags & 0x0400)
                Skip(p, end, 12); // axis vector (f3)

            // Local axis
            if (flags & 0x0800)
                Skip(p, end, 24); // localX(f3) + localZ(f3)

            // External parent deform
            if (flags & 0x2000)
            {
                int32_t extKey; Read(p, end, extKey);
            }

            // IK
            if (flags & 0x0020)
            {
                SkeletonAsset::IKChain chain;
                chain.ikBoneIndex = static_cast<uint32_t>(bi);

                int32_t targetIdx = ReadIndex(p, end, cfg.boneIndexSize);
                chain.targetBoneIndex = static_cast<uint32_t>(std::max(targetIdx, 0));

                int32_t loopCount = 0;
                Read(p, end, loopCount);
                chain.loopCount = static_cast<uint32_t>(std::max(loopCount, 0));

                Read(p, end, chain.angleLimit);

                int32_t linkCount = 0;
                Read(p, end, linkCount);

                chain.links.resize(static_cast<size_t>(std::max(linkCount, 0)));
                for (int32_t li = 0; li < linkCount; ++li)
                {
                    auto& link = chain.links[li];
                    int32_t linkBone = ReadIndex(p, end, cfg.boneIndexSize);
                    link.boneIndex = static_cast<uint32_t>(std::max(linkBone, 0));

                    uint8_t hasLimit = 0;
                    Read(p, end, hasLimit);
                    link.hasAngleLimit = (hasLimit != 0);

                    if (link.hasAngleLimit)
                    {
                        float minX, minY, minZ, maxX, maxY, maxZ;
                        Read(p, end, minX); Read(p, end, minY); Read(p, end, minZ);
                        Read(p, end, maxX); Read(p, end, maxY); Read(p, end, maxZ);
                        // No axis conversion: the angle limits are used as PMX stores them.
                        link.minAngle = { minX, minY, minZ };
                        link.maxAngle = { maxX, maxY, maxZ };
                    }
                }

                skel.ikChains.push_back(std::move(chain));
            }
        }

        LOG_INFO("PmxImporter: %d bones, %zu IK chains", boneCount,
                 skel.ikChains.size());

        // Log how many bones have a grant source.
        {
            uint32_t gc = 0;
            for (int32_t bi = 0; bi < boneCount; ++bi)
                if (skel.grantSource[bi] >= 0) ++gc;
            if (gc > 0)
                LOG_INFO("PmxImporter: %u grant relationships", gc);
        }

        // ---- Parent order ----
        // PMX convention: parentIndex[i] < i; a violation is only logged.
        for (int32_t bi = 1; bi < boneCount; ++bi)
        {
            if (skel.parentIndex[bi] >= bi && skel.parentIndex[bi] >= 0)
            {
                LOG_WARNING("PmxImporter: bone %d (%s) has parent %d >= self — topological violation",
                            bi, skel.boneNames[bi], skel.parentIndex[bi]);
            }
        }

        // ---- Bind pose, inverse bind pose, local rest pose ----
        // PMX bones store a model-space rest position and no rest rotation.
        for (int32_t bi = 0; bi < boneCount; ++bi)
        {
            const XMFLOAT3& wp = boneWorldPos[bi];

            XMStoreFloat4x4(&skel.bindPose[bi],
                XMMatrixTranslation(wp.x, wp.y, wp.z));

            XMStoreFloat4x4(&skel.inverseBindPose[bi],
                XMMatrixTranslation(-wp.x, -wp.y, -wp.z));

            // restPoseLocal: the offset from the parent's rest position.
            const int32_t parent = skel.parentIndex[bi];
            if (parent < 0 || parent >= boneCount)
            {
                // Root bone: local = world
                skel.restPoseLocal[bi] = skel.bindPose[bi];
            }
            else
            {
                const XMFLOAT3& pp = boneWorldPos[parent];
                float dx = wp.x - pp.x;
                float dy = wp.y - pp.y;
                float dz = wp.z - pp.z;
                XMStoreFloat4x4(&skel.restPoseLocal[bi],
                    XMMatrixTranslation(dx, dy, dz));
            }
        }

        // ---- nameToIndex (Fnv32 of each bone name) ----
        for (uint32_t bi = 0; bi < skel.boneCount; ++bi)
            skel.nameToIndex[Fnv32(skel.boneNames[bi])] = bi;

        // ---- Morphs ----
        int32_t morphCount = 0;
        if (!Read(p, end, morphCount)) morphCount = 0;

        uint32_t vertexMorphCount = 0;
        for (int32_t mi = 0; mi < morphCount; ++mi)
        {
            std::string morphNameJP = ReadText(p, end, cfg.encoding);
            ReadText(p, end, cfg.encoding); // name EN

            uint8_t panel = 0, morphType = 0;
            Read(p, end, panel);
            Read(p, end, morphType);

            int32_t offsetCount = 0;
            Read(p, end, offsetCount);

            // For each morph, in file order: its type, and its index in morphTargets (vertex) or groupMorphs
            // (group), else -1. Group morphs name their members by this file index.
            result.morphTypes.push_back(morphType);
            result.morphSlots.push_back(morphType == 1 ? static_cast<int32_t>(result.morphTargets.size())
                                      : morphType == 0 ? static_cast<int32_t>(result.groupMorphs.size()) : -1);

            if (morphType == 1) // Vertex morph
            {
                PmxMorphTarget target;
                strncpy_s(target.name, sizeof(target.name), morphNameJP.c_str(), _TRUNCATE);
                target.vertexIndices.reserve(static_cast<size_t>(offsetCount));
                target.positionDeltas.reserve(static_cast<size_t>(offsetCount));

                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    uint32_t vi = ReadVertexIndex(p, end, cfg.vertexIndexSize);
                    float dx, dy, dz;
                    Read(p, end, dx); Read(p, end, dy); Read(p, end, dz);
                    target.vertexIndices.push_back(vi);
                    target.positionDeltas.push_back(ConvertPos3(dx, dy, dz));
                }

                result.morphTargets.push_back(std::move(target));
                ++vertexMorphCount;
            }
            else if (morphType == 0) // Group morph: becomes a preset
            {
                PmxGroupMorph g;
                g.name = morphNameJP;
                g.offsets.reserve(static_cast<size_t>((std::max)(offsetCount, 0)));
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    const int32_t mi = ReadIndex(p, end, cfg.morphIndexSize);
                    float ratio = 0.f; Read(p, end, ratio);
                    g.offsets.emplace_back(mi, ratio);
                }
                result.groupMorphs.push_back(std::move(g));
            }
            else if (morphType == 2) // Bone morph
            {
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    ReadIndex(p, end, cfg.boneIndexSize);
                    Skip(p, end, 12 + 16); // position(f3) + quaternion(f4)
                }
            }
            else if (morphType >= 3 && morphType <= 7) // UV morph (3=UV, 4-7=addlUV)
            {
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    ReadVertexIndex(p, end, cfg.vertexIndexSize);
                    Skip(p, end, 16); // float4 UV delta
                }
            }
            else if (morphType == 8) // Material morph
            {
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    ReadIndex(p, end, cfg.materialIndexSize);
                    Skip(p, end, 1 + 16 + 12 + 4 + 12 + 16 + 4 + 16 + 16 + 16);
                    // op(1) + diffuse(f4) + specular(f3) + shininess(f) + ambient(f3)
                    // + edgeColor(f4) + edgeSize(f) + texTint(f4) + sphTint(f4) + toonTint(f4)
                }
            }
            else if (morphType == 9) // Flip morph
            {
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    ReadIndex(p, end, cfg.morphIndexSize);
                    float ratio; Read(p, end, ratio);
                }
            }
            else if (morphType == 10) // Impulse morph
            {
                for (int32_t oi = 0; oi < offsetCount; ++oi)
                {
                    ReadIndex(p, end, cfg.rigidBodyIndexSize);
                    Skip(p, end, 1 + 12 + 12); // local(1) + velocity(f3) + torque(f3)
                }
            }
            else
            {
                LOG_WARNING("PmxImporter: unknown morph type %u at morph %d, skipping %d offsets",
                            morphType, mi, offsetCount);
                // An unknown type's offset size is unknown: stop reading morphs.
                break;
            }
        }

        LOG_INFO("PmxImporter: %d morphs total, %u vertex morphs extracted, %zu group morph(s)",
                 morphCount, vertexMorphCount, result.groupMorphs.size());
        // The import report of what the model does not carry: a vertex morph becomes a shape key,
        // a group morph a preset; the other types drop.
        {
            uint32_t bone = 0, uv = 0, material = 0, flip = 0, impulse = 0;
            for (const uint8_t t : result.morphTypes)
            {
                if (t == 2) ++bone;
                else if (t >= 3 && t <= 7) ++uv;
                else if (t == 8) ++material;
                else if (t == 9) ++flip;
                else if (t == 10) ++impulse;
            }
            if (bone + uv + material + flip + impulse)
                LOG_WARNING("PmxImporter: not imported - %u bone, %u UV, %u material, %u flip, %u impulse "
                            "morph(s) (a vertex morph becomes a shape key and a group morph a preset; these "
                            "have no counterpart yet)", bone, uv, material, flip, impulse);
        }

        // Store morph target names in skeleton for runtime name-matching
        skel.morphTargetCount = std::min(static_cast<uint32_t>(result.morphTargets.size()),
                                          SkeletonAsset::MAX_MORPH_TARGETS);
        for (uint32_t mi = 0; mi < skel.morphTargetCount; ++mi)
            std::memcpy(skel.morphTargetNames[mi], result.morphTargets[mi].name, 64);

        // Display frames, rigid bodies and joints follow the morphs and are not read.

        // Build the canonical humanoid role map for retargeting. PMX bones carry
        // Japanese semi-standard names (nameJP), so the convention is PmxJp.
        Resource::HumanoidProfileBuilder::Build(skel, HumanoidConvention::PmxJp);
        LOG_INFO("PmxImporter: humanoid profile — valid=%u (all 15 required mapped=%s)",
                 skel.humanoid.valid, skel.humanoid.valid ? "yes" : "no");

        result.success = true;

        LOG_SUCCESS("PmxImporter: import complete — %d verts, %d tris, %d bones, %zu materials, %u morphs",
                    vertexCount, indexCount / 3, boneCount, result.materials.size(), vertexMorphCount);
        return result;
    }

    // ==== ImportFromFile ====
    PmxImportResult PmxImporter::ImportFromFile(const std::string& path)
    {
        std::vector<uint8_t> data;
        if (!Sys::ReadFileBytes(path, data) || data.empty())
        {
            LOG_ERROR("PmxImporter: cannot open '%s'", path.c_str());
            return {};
        }
        return Import(data.data(), data.size());
    }

    // ==== BuildBlobs ====
    // Writes the .meshlib, .iskel, .imorph, .imat and .iscn blobs (plus the cooked textures) in
    // SceneImporter's formats, so SceneInstanceLoader loads them unchanged.

    // A 32-byte pos/normal/uv vertex (no tangent); BuildBlobs lays the .meshlib out with
    // ComputeMeshLibVertexLayout instead. Keep it in the anonymous namespace: SceneImporter.cpp and
    // SceneInstanceLoader.cpp define PackedVertex structs of their own (48 / 32 bytes), and with
    // external linkage ICF folding merges vector<PackedVertex> onto one sizeof and overruns the
    // buffer.
    namespace {
        struct PackedVertex
        {
            float px, py, pz;
            float nx, ny, nz;
            float u, v;
        };
        static_assert(sizeof(PackedVertex) == 32, "PackedVertex must be 32 bytes");
    } // anonymous

    void PmxImporter::DropMorphs(PmxImportResult& pmx)
    {
        const size_t dropped = pmx.morphTargets.size() + pmx.groupMorphs.size();
        pmx.morphTargets.clear();
        pmx.groupMorphs.clear();
        pmx.morphTypes.clear();
        pmx.morphSlots.clear();
        pmx.skeleton.morphTargetCount = 0;
        if (dropped)
            LOG_INFO("PmxImporter: morphs = 0 - %zu vertex / group morph(s) not imported", dropped);
    }

    std::vector<PmxExportedFile> PmxImporter::BuildBlobs(
        const PmxImportResult&          pmx,
        const std::string&              stem,
        std::vector<ImportedShapeKeys>* outShapeKeys,
        const std::string*              sourcePath,
        const std::string*              materialRoot)
    {
        std::vector<PmxExportedFile> files;
        if (!pmx.success) return files;

        const SkeletonAsset& sk = pmx.skeleton;
        const uint32_t boneCount = sk.boneCount;

        // ---- One .meshlib for the model, plus per-material blend data ----
        // Each PMX material is a contiguous triangle range over a shared vertex buffer; it becomes one
        // MeshLibraryEntry of one shared VB/IB, as SceneImporter writes it.
        // Collect the blend data in the same pass and vertex order as the packed VB, or the .iskel's
        // per-mesh blend streams do not line up with the entries' vertex ranges.
        const std::string meshLibName = stem + ".meshlib";   // single file
        uint32_t meshEntryCount = 0;                          // final Entry[] size

        // Keep the xy of up to three additional UV sets; PMX has no tangent or colour stream. The
        // layout comes from the channel flags via ComputeMeshLibVertexLayout, so the .meshlib
        // round-trips through MeshLibrary::Load and MeshManager.
        const bool pmxHasUV1 = !pmx.uv1.empty();
        const bool pmxHasUV2 = !pmx.uv2.empty();
        const bool pmxHasUV3 = !pmx.uv3.empty();
        const uint32_t libFlags = (pmxHasUV1 ? MESHLIB_FLAG_HAS_UV1 : 0u)
            | (pmxHasUV2 ? MESHLIB_FLAG_HAS_UV2 : 0u)
            | (pmxHasUV3 ? MESHLIB_FLAG_HAS_UV3 : 0u);
        const MeshLibVertexLayout L = ComputeMeshLibVertexLayout(libFlags);

        std::vector<uint8_t>          libVertBytes;   // shared interleaved VB
        std::vector<uint32_t>         libIndices;     // shared IB
        std::vector<MeshLibraryEntry> libEntries;     // one per non-empty PMX material
        libEntries.reserve(pmx.materials.size());

        // Append one interleaved vertex (pos/normal/uv0 [+uv1/uv2/uv3]) to the shared VB.
        auto appendVert = [&](const XMFLOAT3& pos, const XMFLOAT3& nrm,
                              const XMFLOAT2& uv0, const XMFLOAT2* uv1ptr,
                              const XMFLOAT2* uv2ptr, const XMFLOAT2* uv3ptr)
        {
            const size_t base = libVertBytes.size();
            libVertBytes.resize(base + L.stride, 0u);
            uint8_t* vp = libVertBytes.data() + base;
            auto wF = [](uint8_t* p, float f) { std::memcpy(p, &f, sizeof(float)); };
            wF(vp + L.posOffset + 0, pos.x); wF(vp + L.posOffset + 4, pos.y); wF(vp + L.posOffset + 8, pos.z);
            wF(vp + L.normalOffset + 0, nrm.x); wF(vp + L.normalOffset + 4, nrm.y); wF(vp + L.normalOffset + 8, nrm.z);
            wF(vp + L.uv0Offset + 0, uv0.x); wF(vp + L.uv0Offset + 4, uv0.y);
            if (L.uv1Offset != 0xFFFFFFFFu)
            {
                const XMFLOAT2 u1 = uv1ptr ? *uv1ptr : uv0;
                wF(vp + L.uv1Offset + 0, u1.x); wF(vp + L.uv1Offset + 4, u1.y);
            }
            if (L.uv2Offset != 0xFFFFFFFFu)
            {
                const XMFLOAT2 u1 = uv2ptr ? *uv2ptr : uv0;
                wF(vp + L.uv2Offset + 0, u1.x); wF(vp + L.uv2Offset + 4, u1.y);
            }
            if (L.uv3Offset != 0xFFFFFFFFu)
            {
                const XMFLOAT2 u1 = uv3ptr ? *uv3ptr : uv0;
                wF(vp + L.uv3Offset + 0, u1.x); wF(vp + L.uv3Offset + 4, u1.y);
            }
        };

        struct MeshBlendEntry { uint32_t meshIdx; std::vector<BlendVertex> data; };
        std::vector<MeshBlendEntry> meshBlends;
        std::vector<std::vector<uint32_t>> perMeshGlobalVerts; // global vertex indices per submesh
        std::vector<std::vector<uint32_t>> perMeshLocalIndices; // its triangles, local indices (morph normals)

        uint32_t vOffset = 0;   // running base vertex in the shared VB

        for (size_t mi = 0; mi < pmx.materials.size(); ++mi)
        {
            const PmxMaterial& mat = pmx.materials[mi];
            if (mat.indexCount == 0) continue;

            // This material's unique vertices in first-use order (a vector, not a map iteration), so the
            // VB slice and the .iskel blend data share one order.
            std::vector<uint32_t> uniqueGlobalVerts; // ordered list of unique global vertex indices
            std::unordered_map<uint32_t, uint32_t> globalToLocal;
            std::vector<uint32_t> localIndices;
            localIndices.reserve(mat.indexCount);

            for (uint32_t ii = 0; ii < mat.indexCount; ++ii)
            {
                uint32_t gi = pmx.indices[mat.indexStart + ii];
                auto it = globalToLocal.find(gi);
                if (it == globalToLocal.end())
                {
                    uint32_t li = static_cast<uint32_t>(uniqueGlobalVerts.size());
                    globalToLocal[gi] = li;
                    uniqueGlobalVerts.push_back(gi);
                    localIndices.push_back(li);
                }
                else
                    localIndices.push_back(it->second);
            }

            const uint32_t nv      = static_cast<uint32_t>(uniqueGlobalVerts.size());
            const uint32_t iStart  = static_cast<uint32_t>(libIndices.size());
            const uint32_t vStart  = vOffset;

            // Pack vertices, blend data and the entry AABB in one pass, in that order.
            float bmin[3] = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
            float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            std::vector<BlendVertex> blends(nv);

            for (uint32_t li = 0; li < nv; ++li)
            {
                uint32_t gi = uniqueGlobalVerts[li];
                const auto& pos = pmx.positions[gi];
                const auto& nrm = pmx.normals[gi];
                const auto& uv  = pmx.uvs[gi];
                const XMFLOAT2* uv1p = pmxHasUV1 ? &pmx.uv1[gi] : nullptr;
                const XMFLOAT2* uv2p = pmxHasUV2 ? &pmx.uv2[gi] : nullptr;
                const XMFLOAT2* uv3p = pmxHasUV3 ? &pmx.uv3[gi] : nullptr;
                appendVert(pos, nrm, uv, uv1p, uv2p, uv3p);
                blends[li] = pmx.blendData[gi];

                if (pos.x < bmin[0]) bmin[0] = pos.x;
                if (pos.y < bmin[1]) bmin[1] = pos.y;
                if (pos.z < bmin[2]) bmin[2] = pos.z;
                if (pos.x > bmax[0]) bmax[0] = pos.x;
                if (pos.y > bmax[1]) bmax[1] = pos.y;
                if (pos.z > bmax[2]) bmax[2] = pos.z;
            }

            // Re-base local indices into the shared VB pool.
            for (uint32_t li : localIndices)
                libIndices.push_back(li + vOffset);

            MeshLibraryEntry entry{};
            entry.vertexStart        = vStart;
            entry.vertexCount        = nv;
            entry.indexStart         = iStart;
            entry.indexCount         = static_cast<uint32_t>(localIndices.size());
            entry.aabbMin[0]         = bmin[0]; entry.aabbMin[1] = bmin[1]; entry.aabbMin[2] = bmin[2];
            entry.aabbMax[0]         = bmax[0]; entry.aabbMax[1] = bmax[1]; entry.aabbMax[2] = bmax[2];
            entry.defaultMaterialIdx = 0xFFFFFFFFu;  // resolved via F-line `mat=` in .iscn
            entry.flags              = 0;
            libEntries.push_back(entry);

            // Save blend data keyed by mesh entry index (== libEntries.size()-1).
            meshBlends.push_back({ meshEntryCount, std::move(blends) });

            // Save vertex remap + triangles for morph delta generation
            perMeshGlobalVerts.push_back(std::move(uniqueGlobalVerts));
            perMeshLocalIndices.push_back(std::move(localIndices));

            vOffset += nv;
            ++meshEntryCount;
        }

        // Emit the single .meshlib blob.
        if (!libEntries.empty())
        {
            const uint32_t entryBytes = static_cast<uint32_t>(libEntries.size() * sizeof(MeshLibraryEntry));
            const uint32_t vbBytes    = static_cast<uint32_t>(libVertBytes.size());
            const uint32_t ibBytes    = static_cast<uint32_t>(libIndices.size() * sizeof(uint32_t));

            MeshLibraryMetadata libMeta{};
            libMeta.meshCount    = static_cast<uint32_t>(libEntries.size());
            libMeta.vertexCount  = static_cast<uint32_t>(libVertBytes.size() / L.stride);
            libMeta.indexCount   = static_cast<uint32_t>(libIndices.size());
            libMeta.vertexStride = static_cast<uint16_t>(L.stride);
            libMeta.indexStride  = 4;
            libMeta.flags        = libFlags;
            libMeta.reserved     = 0;

            AssetHeader libHdr{};
            libHdr.magic        = MAGIC_MESHLIB;
            libHdr.version      = CurrentVersion(MAGIC_MESHLIB);
            libHdr.resourceType = static_cast<uint16_t>(ResourceType::MeshLibrary);
            libHdr.metadataSize = sizeof(MeshLibraryMetadata);
            libHdr.dataSize     = entryBytes + vbBytes + ibBytes;

            const size_t total = sizeof(AssetHeader) + sizeof(MeshLibraryMetadata)
                               + entryBytes + vbBytes + ibBytes;
            std::vector<uint8_t> blob(total);
            uint8_t* dst = blob.data();
            std::memcpy(dst, &libHdr,          sizeof(AssetHeader));         dst += sizeof(AssetHeader);
            std::memcpy(dst, &libMeta,         sizeof(MeshLibraryMetadata)); dst += sizeof(MeshLibraryMetadata);
            std::memcpy(dst, libEntries.data(),  entryBytes);                 dst += entryBytes;
            std::memcpy(dst, libVertBytes.data(), vbBytes);                   dst += vbBytes;
            std::memcpy(dst, libIndices.data(),  ibBytes);

            files.push_back({ stem + "/" + meshLibName, std::move(blob) });
        }

        // ---- Per-bone rest AABBs (bind-model space) ----
        // The union of the rest positions each bone influences, from the same model-space vertices the
        // .meshlib packs: the space the skin matrices (inverseBindPose x localToModel) map from, which
        // the renderer's bone-AABB merge transforms these boxes with. sk.boneRestAABBs holds only the
        // +-1e30 seeds and `sk` is const, so the boxes go to a local table and
        // ISKEL_FLAG_BONE_AABB_MODEL marks them valid. Do not write the seeds: the merge skips every
        // bone and collapses the culling bounds to the model origin.
        std::vector<SkeletonAsset::BoneAABB> boneAabbs(boneCount);
        uint32_t validBoneAabbs = 0;
        {
            const size_t vtxCount = (std::min)(pmx.positions.size(), pmx.blendData.size());
            for (size_t vi = 0; vi < vtxCount; ++vi)
            {
                const XMFLOAT3&    pos = pmx.positions[vi];
                const BlendVertex& bv  = pmx.blendData[vi];
                for (uint32_t w = 0; w < MAX_BONES_PER_VERTEX; ++w)
                {
                    if (bv.boneWeights[w] == 0) continue;
                    const uint32_t bi = bv.boneIndices[w];
                    if (bi >= boneCount) continue;
                    auto& ab = boneAabbs[bi];
                    ab.localMin.x = (std::min)(ab.localMin.x, pos.x);
                    ab.localMin.y = (std::min)(ab.localMin.y, pos.y);
                    ab.localMin.z = (std::min)(ab.localMin.z, pos.z);
                    ab.localMax.x = (std::max)(ab.localMax.x, pos.x);
                    ab.localMax.y = (std::max)(ab.localMax.y, pos.y);
                    ab.localMax.z = (std::max)(ab.localMax.z, pos.z);
                }
            }
            // A bone with no vertices stays degenerate on purpose (the merge skips it); only boxes with
            // skin get the kPad margin.
            const float kPad = 0.02f;
            for (uint32_t bi = 0; bi < boneCount; ++bi)
            {
                auto& ab = boneAabbs[bi];
                if (ab.localMin.x > ab.localMax.x) continue;
                ab.localMin.x -= kPad; ab.localMin.y -= kPad; ab.localMin.z -= kPad;
                ab.localMax.x += kPad; ab.localMax.y += kPad; ab.localMax.z += kPad;
                ++validBoneAabbs;
            }
            LOG_INFO("PmxImporter: computed %u/%u bone rest AABBs (bind-model space)",
                     validBoneAabbs, boneCount);
        }

        // ---- Build .iskel blob (with extended PMX data) ----
        {
            uint32_t meshBlendCount = meshEntryCount;   // one blend stream per MeshLibraryEntry

            // --- Compute payload size ---
            size_t payloadSz = sizeof(uint32_t); // meshBlendCount
            payloadSz += boneCount * sizeof(int32_t);          // parentIndex
            payloadSz += boneCount * 16 * sizeof(float) * 3;   // invBind + bind + restLocal
            payloadSz += boneCount * 64;                        // boneNames

            // clips (none for PMX)

            // per-mesh blend data
            for (const auto& mb : meshBlends)
                payloadSz += 2 * sizeof(uint32_t) + mb.data.size() * sizeof(BlendVertex);

            // Extended PMX data (ISKEL_FLAG_EXTENDED):
            //   grantSource[boneCount] (int32)
            //   grantRatio[boneCount]  (float)
            //   transformOrder[boneCount] (int32)
            //   ikChainCount (uint32)
            //   per chain: ikBoneIndex(u32), targetBoneIndex(u32), loopCount(u32),
            //              angleLimit(f32), linkCount(u32)
            //   per link:  boneIndex(u32), hasAngleLimit(u8),
            //              [if hasLimit: minAngle(f3) + maxAngle(f3)]
            payloadSz += boneCount * sizeof(int32_t);   // grantSource
            payloadSz += boneCount * sizeof(float);     // grantRatio
            payloadSz += boneCount * sizeof(int32_t);   // transformOrder
            payloadSz += sizeof(uint32_t);              // ikChainCount
            for (const auto& chain : sk.ikChains)
            {
                payloadSz += 5 * sizeof(uint32_t);     // ik/target/loop/angleLimit/linkCount (20B)
                for (const auto& link : chain.links)
                {
                    payloadSz += sizeof(uint32_t) + 1;  // boneIndex + hasAngleLimit
                    if (link.hasAngleLimit)
                        payloadSz += 6 * sizeof(float); // minAngle(f3) + maxAngle(f3)
                }
            }

            // boneRestAABBs
            payloadSz += boneCount * 6 * sizeof(float); // min3 + max3 per bone

            // Morph target names
            payloadSz += sizeof(uint32_t); // morphTargetCount
            payloadSz += sk.morphTargetCount * 64; // morphTargetNames

            // Humanoid profile (retarget role map), appended at the tail.
            const bool writeHumanoid =
                (sk.humanoid.convention != static_cast<uint8_t>(HumanoidConvention::Unknown));
            if (writeHumanoid)
                payloadSz += sizeof(HumanoidProfile);

            // --- Build header + metadata ---
            SkeletonMetadata smeta{};
            smeta.boneCount   = boneCount;
            smeta.clipCount   = 0;
            smeta.reserved[0] = meshBlendCount;
            smeta.reserved[1] = ISKEL_FLAG_EXTENDED
                              | (writeHumanoid ? ISKEL_FLAG_HUMANOID : 0)
                              | (validBoneAabbs > 0 ? ISKEL_FLAG_BONE_AABB_MODEL : 0);

            AssetHeader shdr{};
            shdr.magic        = MAGIC_SKELETON;
            shdr.version      = CurrentVersion(MAGIC_SKELETON);
            shdr.resourceType = static_cast<uint16_t>(ResourceType::Skeleton);
            shdr.metadataSize = sizeof(SkeletonMetadata);
            shdr.dataSize     = static_cast<uint32_t>(payloadSz);

            std::vector<uint8_t> blob(sizeof(AssetHeader) + sizeof(SkeletonMetadata) + payloadSz, 0);
            uint8_t* dst = blob.data();

            std::memcpy(dst, &shdr,  sizeof(AssetHeader));      dst += sizeof(AssetHeader);
            std::memcpy(dst, &smeta, sizeof(SkeletonMetadata)); dst += sizeof(SkeletonMetadata);

            // --- Standard payload ---
            std::memcpy(dst, &meshBlendCount, sizeof(uint32_t)); dst += sizeof(uint32_t);

            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.parentIndex[i], sizeof(int32_t)); dst += sizeof(int32_t); }
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.inverseBindPose[i], 64); dst += 64; }
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.bindPose[i], 64); dst += 64; }
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.restPoseLocal[i], 64); dst += 64; }
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, sk.boneNames[i], 64); dst += 64; }

            // boneRestAABBs: the boxes computed above, not the seeds in sk.boneRestAABBs.
            for (uint32_t i = 0; i < boneCount; ++i)
            {
                std::memcpy(dst, &boneAabbs[i].localMin, 3 * sizeof(float)); dst += 3 * sizeof(float);
                std::memcpy(dst, &boneAabbs[i].localMax, 3 * sizeof(float)); dst += 3 * sizeof(float);
            }

            // clips (none)

            // per-mesh blend data
            for (const auto& mb : meshBlends)
            {
                uint32_t idx = mb.meshIdx;
                uint32_t vc  = static_cast<uint32_t>(mb.data.size());
                std::memcpy(dst, &idx, sizeof(uint32_t)); dst += sizeof(uint32_t);
                std::memcpy(dst, &vc,  sizeof(uint32_t)); dst += sizeof(uint32_t);
                std::memcpy(dst, mb.data.data(), vc * sizeof(BlendVertex));
                dst += vc * sizeof(BlendVertex);
            }

            // --- Extended PMX data ---
            // grantSource
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.grantSource[i], sizeof(int32_t)); dst += sizeof(int32_t); }
            // grantRatio
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.grantRatio[i], sizeof(float)); dst += sizeof(float); }
            // transformOrder
            for (uint32_t i = 0; i < boneCount; ++i)
            { std::memcpy(dst, &sk.transformOrder[i], sizeof(int32_t)); dst += sizeof(int32_t); }

            // IK chains
            uint32_t ikCount = static_cast<uint32_t>(sk.ikChains.size());
            std::memcpy(dst, &ikCount, sizeof(uint32_t)); dst += sizeof(uint32_t);

            for (const auto& chain : sk.ikChains)
            {
                std::memcpy(dst, &chain.ikBoneIndex,     sizeof(uint32_t)); dst += sizeof(uint32_t);
                std::memcpy(dst, &chain.targetBoneIndex, sizeof(uint32_t)); dst += sizeof(uint32_t);
                std::memcpy(dst, &chain.loopCount,       sizeof(uint32_t)); dst += sizeof(uint32_t);
                std::memcpy(dst, &chain.angleLimit,      sizeof(float));    dst += sizeof(float);

                uint32_t linkCount = static_cast<uint32_t>(chain.links.size());
                std::memcpy(dst, &linkCount, sizeof(uint32_t)); dst += sizeof(uint32_t);

                for (const auto& link : chain.links)
                {
                    std::memcpy(dst, &link.boneIndex, sizeof(uint32_t)); dst += sizeof(uint32_t);
                    uint8_t hasLim = link.hasAngleLimit ? 1 : 0;
                    *dst++ = hasLim;
                    if (link.hasAngleLimit)
                    {
                        std::memcpy(dst, &link.minAngle, sizeof(XMFLOAT3)); dst += sizeof(XMFLOAT3);
                        std::memcpy(dst, &link.maxAngle, sizeof(XMFLOAT3)); dst += sizeof(XMFLOAT3);
                    }
                }
            }

            // Morph target names (for runtime name-matching against MorphClipAsset)
            std::memcpy(dst, &sk.morphTargetCount, sizeof(uint32_t)); dst += sizeof(uint32_t);
            for (uint32_t i = 0; i < sk.morphTargetCount; ++i)
            { std::memcpy(dst, sk.morphTargetNames[i], 64); dst += 64; }

            // Humanoid profile (tail, gated by ISKEL_FLAG_HUMANOID)
            if (writeHumanoid)
            {
                std::memcpy(dst, &sk.humanoid, sizeof(HumanoidProfile)); dst += sizeof(HumanoidProfile);
            }

            std::string skelFileName = stem + ".iskel";
            files.push_back({ stem + "/" + skelFileName, std::move(blob) });
        }

        // ---- .imorph: per-submesh morph target deltas ----
        std::vector<std::string> morphFileNames; // per submesh .imorph filename (empty = no morphs)
        if (!pmx.morphTargets.empty())
        {
            // One combined .imorph holds every submesh's block (one read at load); each block names its
            // submesh via MorphTargetMetadata.meshFileIndex.
            // Blocks are sparse (MORPHLIB_FLAG_SPARSE_SLOTS): a block carries only the morphs whose
            // vertexIndices land in its submesh, with a strictly increasing skeleton-slot table that
            // BuildSkinJobs scatters weights through.
            // MorphLibrary::Build writes the v2 file: section-keyed (WriteModelImportFiles stamps the
            // library's final keys), with a normal delta per target computed here over the submesh's own
            // triangles (MorphLibrary::ComputeNormalDeltas), since PMX authors none.
            // SceneImporter::BuildMorphLibBlob uses the same writer: keep the two in step.
            const std::string                   combinedMorphFile = stem + ".imorph";
            std::vector<MorphLibrary::BlockOut> blockOuts;

            // Capped like the skeleton's name list (Import clamps it to SkeletonAsset::MAX_MORPH_TARGETS):
            // a slot past that list would name a shape Face.* can never resolve.
            const uint32_t morphCount =
                (std::min)(static_cast<uint32_t>(pmx.morphTargets.size()),
                           static_cast<uint32_t>(SkeletonAsset::MAX_MORPH_TARGETS));

            for (size_t si = 0; si < perMeshGlobalVerts.size(); ++si)
            {
                const auto& globalVerts = perMeshGlobalVerts[si];
                const uint32_t nv = static_cast<uint32_t>(globalVerts.size());
                if (nv == 0) continue;

                // Reverse map: global vertex index -> local index in this submesh.
                std::unordered_map<uint32_t, uint32_t> globalToLocal;
                for (uint32_t li = 0; li < nv; ++li)
                    globalToLocal[globalVerts[li]] = li;

                // Which morphs touch this submesh at all? Ascending by
                // construction (the loop walks the skeleton order), which is the
                // strictly-increasing slot table the format requires.
                std::vector<uint32_t> localSlots;
                for (uint32_t mi = 0; mi < morphCount; ++mi)
                {
                    const auto& target = pmx.morphTargets[mi];
                    for (uint32_t gi : target.vertexIndices)
                        if (globalToLocal.count(gi)) { localSlots.push_back(mi); break; }
                }

                if (localSlots.empty())
                {
                    morphFileNames.push_back(""); // no morphs for this submesh
                    continue;
                }

                // The submesh as the .meshlib packs it: what the normal delta is
                // taken over, and the authored normals that orient it.
                std::vector<XMFLOAT3> basePos(nv), baseNrm(nv);
                for (uint32_t li = 0; li < nv; ++li)
                {
                    basePos[li] = pmx.positions[globalVerts[li]];
                    baseNrm[li] = pmx.normals[globalVerts[li]];
                }
                const std::vector<uint32_t>& tris = perMeshLocalIndices[si];

                MorphLibrary::BlockOut bo;
                bo.meshFileIndex = static_cast<uint32_t>(si);
                bo.vertexCount   = nv;
                bo.slots         = localSlots;
                bo.targets.reserve(localSlots.size());
                for (const uint32_t slot : localSlots)
                {
                    const auto& target = pmx.morphTargets[slot];
                    MorphLibrary::TargetOut t;
                    t.name.assign(target.name, strnlen(target.name, sizeof(target.name) - 1));
                    t.pos.assign(nv, XMFLOAT3{ 0.f, 0.f, 0.f });
                    for (size_t di = 0; di < target.vertexIndices.size(); ++di)
                    {
                        auto it = globalToLocal.find(target.vertexIndices[di]);
                        if (it != globalToLocal.end()) t.pos[it->second] = target.positionDeltas[di];
                    }
                    t.nrm.resize(nv);
                    MorphLibrary::ComputeNormalDeltas(basePos.data(), nv, tris.data(), tris.size(), t.pos.data(),
                                                      t.nrm.data(), baseNrm.data());
                    bo.targets.push_back(std::move(t));
                }
                blockOuts.push_back(std::move(bo));
                morphFileNames.push_back(combinedMorphFile);  // every submesh with morphs -> the one combined file
            }

            if (!blockOuts.empty())
                files.push_back({ stem + "/" + combinedMorphFile, MorphLibrary::Build(blockOuts) });

            LOG_INFO("PmxImporter: built combined .imorph '%s' — %zu submesh blocks (%zu morph targets)",
                     combinedMorphFile.c_str(), blockOuts.size(), pmx.morphTargets.size());

            // ---- Group morphs -> presets of the model's one Mesh ----
            // A group morph is other morphs at ratios; MMD adds them, so a vertex morph named twice (or
            // through a nested group, whose ratios multiply) sums, then clamps into a preset's 0..1. Only
            // vertex morphs the .imorph carries can be named; bone / UV / material / flip / impulse
            // members are counted in a warning and skipped.
            if (outShapeKeys && !pmx.groupMorphs.empty())
            {
                std::unordered_set<std::string> carried;
                for (const MorphLibrary::BlockOut& bo : blockOuts)
                    for (const MorphLibrary::TargetOut& t : bo.targets) carried.insert(t.name);

                ImportedShapeKeys is;
                is.meshSourceKey = "pmx:" + stem;   // the identity's one Mesh (below)
                is.meshName      = stem;
                uint32_t other = 0, clamped = 0, uncarried = 0, empty = 0, repeated = 0;
                for (size_t gi = 0; gi < pmx.groupMorphs.size(); ++gi)
                {
                    const PmxGroupMorph& g = pmx.groupMorphs[gi];
                    ShapeKeyPreset preset;
                    preset.name   = g.name;
                    preset.source = "pmx";
                    if (preset.name.empty()) continue;
                    if (is.setup.FindPreset(preset.name)) { ++repeated; continue; }
                    std::unordered_map<std::string, size_t> at;
                    struct Item { size_t group; float scale; int depth; };
                    std::vector<Item> stack{ { gi, 1.f, 0 } };
                    while (!stack.empty())
                    {
                        const Item it = stack.back();
                        stack.pop_back();
                        for (const auto& [mi, ratio] : pmx.groupMorphs[it.group].offsets)
                        {
                            if (mi < 0 || static_cast<size_t>(mi) >= pmx.morphTypes.size()) { ++other; continue; }
                            const uint8_t type = pmx.morphTypes[static_cast<size_t>(mi)];
                            const int32_t slot = pmx.morphSlots[static_cast<size_t>(mi)];
                            const float   r    = ratio * it.scale;
                            if (type == 1 && slot >= 0 && static_cast<size_t>(slot) < pmx.morphTargets.size())
                            {
                                const PmxMorphTarget& t = pmx.morphTargets[static_cast<size_t>(slot)];
                                const std::string name(t.name, strnlen(t.name, sizeof(t.name) - 1));
                                if (!carried.count(name)) { ++uncarried; continue; }
                                const auto [w, added] = at.emplace(name, preset.weights.size());
                                if (added) preset.weights.emplace_back(name, r);
                                else       preset.weights[w->second].second += r;
                            }
                            else if (type == 0 && slot >= 0 && it.depth < 4)   // nested (depth-bounded: no cycle)
                                stack.push_back({ static_cast<size_t>(slot), r, it.depth + 1 });
                            else
                                ++other;
                        }
                    }
                    std::vector<std::pair<std::string, float>> kept;
                    for (auto& [n, w] : preset.weights)
                    {
                        const float c = std::clamp(w, 0.f, 1.f);
                        if (c != w) ++clamped;
                        if (c > 0.f) kept.emplace_back(n, c);
                    }
                    preset.weights.swap(kept);
                    if (preset.weights.empty()) { ++empty; continue; }
                    is.setup.presets.push_back(std::move(preset));
                }
                LOG_INFO("PmxImporter: %zu of %zu group morph(s) become presets", is.setup.presets.size(),
                         pmx.groupMorphs.size());
                if (other)
                    LOG_WARNING("PmxImporter:   %u group member(s) are bone / UV / material / flip / impulse morphs "
                                "(or out of range) - a preset carries shape-key weights only", other);
                if (uncarried)
                    LOG_WARNING("PmxImporter:   %u group member(s) name a vertex morph the model does not carry "
                                "(past the morph cap, or touching no vertex) - dropped", uncarried);
                if (clamped)
                    LOG_INFO("PmxImporter:   %u group weight(s) outside 0..1 (negative or summed past 1) clamped",
                             clamped);
                if (empty)    LOG_INFO("PmxImporter:   %u group morph(s) move no shape key - no preset", empty);
                if (repeated) LOG_WARNING("PmxImporter:   %u group morph(s) repeat an earlier name - the first kept",
                                          repeated);
                if (!is.setup.presets.empty()) outShapeKeys->push_back(std::move(is));
            }
        }

        // ---- One .imat per material, named after the PMX material ----
        // Two names come out of this loop, deliberately different:
        //   matFileNames[i]  the .imat filename - sanitised and deduplicated
        //   meshNames[i]     the mesh entity name - the author's string verbatim
        // PMX binds one material to each contiguous face range, so the material name is the only label
        // a submesh has; without it every part lands in the Outliner as "<model>_mesh".
        std::vector<std::string> matFileNames;  // parallel to the F-line index
        std::vector<std::string> meshNames;     // parallel to the F-line index
        // Cooks the textures the materials name beside the model.
        ModelTextureCook cook((sourcePath && materialRoot) ? *materialRoot : std::string(), stem,
                              sourcePath ? Sys::PathToUtf8(Sys::U8Path(*sourcePath).parent_path()) : std::string());
        uint32_t texturedMats = 0, missingTextures = 0, sphereOrToon = 0;
        {
            // Count the materials claiming each name. Authors copy a material and leave nameEN stale (a
            // teeth material named "HairA" like the real hair), and MMD keys materials by nameJP. So prefer
            // nameEN, but when it is shared and the nameJP is unique, use the nameJP.
            std::unordered_map<std::string, int> enCount, jpCount;
            for (const PmxMaterial& pm : pmx.materials)
            {
                if (pm.indexCount == 0) continue;
                if (!pm.nameEN.empty()) ++enCount[pm.nameEN];
                if (!pm.nameJP.empty()) ++jpCount[pm.nameJP];
            }

            std::unordered_set<std::string> usedNames;
            for (size_t mi = 0; mi < pmx.materials.size(); ++mi)
            {
                const PmxMaterial& pmat = pmx.materials[mi];
                if (pmat.indexCount == 0) continue;

                // Prefer nameEN (nameJP when nameEN is shared), then nameJP, then material_<i>.
                std::string rawName = pmat.nameEN.empty() ? pmat.nameJP : pmat.nameEN;
                if (!pmat.nameEN.empty() && !pmat.nameJP.empty() &&
                    enCount[pmat.nameEN] > 1 && jpCount[pmat.nameJP] == 1)
                {
                    LOG_INFO("PmxImporter: material %zu nameEN='%s' is shared by %d materials — "
                             "using nameJP='%s' instead",
                             mi, pmat.nameEN.c_str(), enCount[pmat.nameEN], pmat.nameJP.c_str());
                    rawName = pmat.nameJP;
                }
                if (rawName.empty()) rawName = "material_" + std::to_string(mi);

                // Sys::SanitizeFileName replaces only what Win32 and the engine's text containers reject, so
                // UTF-8 names and punctuation such as '+' or '[' survive. Do not filter with `c > 0`: MSVC's
                // char is signed, so every UTF-8 continuation byte would be dropped.
                std::string safeName = Sys::SanitizeFileName(rawName);
                if (safeName.empty()) safeName = "mat_" + std::to_string(mi);

                // Deduplicate filenames only: two materials may share a name, and the second .imat must not
                // overwrite the first. Compare case-insensitively, as the Windows filesystem does: "Eye" and
                // "eye" are one file.
                auto lowerAscii = [](std::string t)
                {
                    for (char& lc : t)
                        if (lc >= 'A' && lc <= 'Z') lc = static_cast<char>(lc - 'A' + 'a');
                    return t;
                };
                std::string uniqueName = safeName;
                int suffix = 1;
                while (usedNames.count(lowerAscii(uniqueName)))
                    uniqueName = safeName + "_" + std::to_string(suffix++);
                usedNames.insert(lowerAscii(uniqueName));

                // The PMX material: diffuse colour (MMD colours are sRGB, as the engine stores them) and
                // alpha, its texture, two-sidedness. Alpha below 1 blends; an opaque colour over a texture
                // with cut-out texels clips (Masked) - hair, lashes, lace.
                MaterialComponent mc{};
                mc.baseColor = { std::clamp(pmat.diffuse.x, 0.f, 1.f), std::clamp(pmat.diffuse.y, 0.f, 1.f),
                                 std::clamp(pmat.diffuse.z, 0.f, 1.f), std::clamp(pmat.diffuse.w, 0.f, 1.f) };
                if (pmat.drawFlags & 0x01) mc.SetDoubleSided(true);
                bool texAlpha = false;
                if (cook.Enabled() && pmat.textureIndex >= 0 &&
                    static_cast<size_t>(pmat.textureIndex) < pmx.textureFiles.size())
                {
                    const ModelTextureCook::Image img = cook.FromFile(pmx.textureFiles[pmat.textureIndex]);
                    if (!img.Valid())
                    {
                        ++missingTextures;
                        LOG_WARNING("PmxImporter: material '%s' names texture '%s' - not found beside the .pmx",
                                    rawName.c_str(), pmx.textureFiles[pmat.textureIndex].c_str());
                    }
                    else if (const auto r = cook.Cook(img, ModelTextureCook::Kind::Color, false, /*probeAlpha*/ true);
                             !r.path.empty())
                    {
                        mc.textures[MaterialComponent::BASECOLORMAP].name = r.path;
                        texAlpha = r.hasAlpha;
                        ++texturedMats;
                    }
                }
                if (pmat.diffuse.w < 0.999f)
                    mc.userBlendMode = BlendMode::Alpha;
                else if (texAlpha)
                {
                    mc.userBlendMode = BlendMode::Masked;
                    mc.alphaRef      = 0.5f;
                }
                if (pmat.sphereTextureIndex >= 0 || pmat.toonTextureIndex >= 0) ++sphereOrToon;

                // Save as .imat blob
                std::string matFileName = uniqueName + ".imat";
                std::string matRelPath = stem + "/Materials/" + matFileName;

                // BuildMaterialBlob builds the .imat in memory; the caller writes the file list.
                files.push_back({ matRelPath, Resource::BuildMaterialBlob(mc) });

                matFileNames.push_back("Materials/" + matFileName);
                // The raw name, not the sanitised one: an entity name is not a path, so "Eyes+" reads in the
                // Outliner as the author typed it.
                meshNames.push_back(rawName);
            }

            LOG_INFO("PmxImporter: generated %zu .imat files", matFileNames.size());
            if (cook.Enabled())
                LOG_INFO("PmxImporter: materials from the source - %u textured, %u texture(s) cooked into "
                         "'%s/Textures', %u failed, %u not found; %u material(s) name a sphere / toon map "
                         "(not imported)", texturedMats, cook.Cooked(), stem.c_str(), cook.Failed(),
                         missingTextures, sphereOrToon);
            for (auto& [rel, blob] : cook.Files()) files.push_back({ rel, std::move(blob) });
        }

        // ---- Identity layer: the whole model is ONE Mesh ----
        // A PMX is one skinned object whose materials are its sections, so its library holds one Mesh
        // listing every entry. Section names are the F-lines' `name=` (meshNames), so an identity
        // synthesized from the written .iscn reads the same names back.
        for (PmxExportedFile& f : files)
        {
            if (f.relativePath != stem + "/" + meshLibName) continue;
            MeshIdentitySpec spec;
            spec.modelSource = "pmx|" + stem;
            MeshIdentitySpec::Mesh m;
            m.name      = stem;
            m.sourceKey = "pmx:" + stem;
            m.flags     = MESH_IDENT_SKINNED;
            std::unordered_map<std::string, uint32_t> occurrence;
            spec.sections.resize(meshEntryCount);
            for (uint32_t i = 0; i < meshEntryCount; ++i)
            {
                MeshIdentitySpec::Section& sec = spec.sections[i];
                sec.slotName  = (i < meshNames.size()) ? meshNames[i] : std::string();
                const std::string base = m.sourceKey + "/" + sec.slotName;
                sec.sourceKey = base + "#" + std::to_string(occurrence[base]++);
                m.sections.push_back(i);
                if (i < morphFileNames.size() && !morphFileNames[i].empty())
                    m.flags |= MESH_IDENT_HAS_MORPH;
            }
            spec.meshes.push_back(std::move(m));

            MeshIdentity id;
            std::string  why;
            if (!BuildMeshIdentity(spec, f.blob.data(), f.blob.size(), id, &why) ||
                !WriteMeshLibIdentity(f.blob, id, &why))
                LOG_WARNING("PmxImporter: identity layer not written (%s) - the model loads, "
                            "but its meshes have no stable keys", why.c_str());
            break;
        }

        // ---- .iscn ----
        // Every mesh F-line references the one shared .meshlib; `mesh=K` picks the MeshLibraryEntry.
        {
            std::ostringstream ss;
            ss << "# ISCN engine scene container (PMX)\n";
            ss << "S nodes=1 meshfiles=" << meshEntryCount
               << " source=" << stem << "\n";

            ss << "N idx=0 parent=-1"
               << " tx=0 ty=0 tz=0"
               << " qx=0 qy=0 qz=0 qw=1"
               << " sx=1 sy=1 sz=1"
               << " mc=" << meshEntryCount;
            for (uint32_t k = 0; k < meshEntryCount; ++k)
                ss << " mr" << k << "=" << k;
            ss << " name=" << stem << "\n";

            for (uint32_t i = 0; i < meshEntryCount; ++i)
            {
                ss << "F idx=" << i << " file=" << meshLibName
                   << " mesh=" << i;

                // `name=` must sit between `mesh=` and `mat=`: ParseFLine reads the material path to the end
                // of the line (filenames may contain spaces), so anything after `mat=` lands in that path and
                // breaks every material. Percent-encoded: the value ends at the next space, must not contain
                // '=', and a PMX material name is arbitrary UTF-8 author text.
                if (i < meshNames.size() && !meshNames[i].empty())
                    ss << " name=" << Resource::AssetRefs::PercentEncodeStrict(meshNames[i]);

                if (i < matFileNames.size())
                    ss << " mat=" << matFileNames[i];
                ss << "\n";
            }

            ss << "K file=" << stem << ".iskel\n";

            // M-lines: per-mesh morph target files
            for (size_t i = 0; i < morphFileNames.size(); ++i)
            {
                if (!morphFileNames[i].empty())
                    ss << "M idx=" << i << " file=" << morphFileNames[i] << "\n";
            }

            std::string text = ss.str();

            // dataSize = textLength + 1 with one trailing NUL: the shape SceneImporter and
            // UnrealSceneImporter write and the write-back path normalises to. Do not drop the NUL: a reader
            // that takes the payload as a C string would run off the end of the blob.
            SceneMetadata scMeta{};
            scMeta.nodeCount     = 1;
            scMeta.meshFileCount = meshEntryCount;
            scMeta.textLength    = static_cast<uint32_t>(text.size());
            scMeta.reserved      = 0;

            AssetHeader scHdr{};
            scHdr.magic        = MAGIC_SCENE;
            scHdr.version      = CurrentVersion(MAGIC_SCENE);
            scHdr.resourceType = static_cast<uint16_t>(ResourceType::Scene);
            scHdr.metadataSize = sizeof(SceneMetadata);
            scHdr.dataSize     = static_cast<uint32_t>(text.size()) + 1;

            std::vector<uint8_t> blob(sizeof(AssetHeader) + sizeof(SceneMetadata) + text.size() + 1, 0);
            uint8_t* dst = blob.data();
            std::memcpy(dst, &scHdr,  sizeof(AssetHeader));     dst += sizeof(AssetHeader);
            std::memcpy(dst, &scMeta, sizeof(SceneMetadata));  dst += sizeof(SceneMetadata);
            std::memcpy(dst, text.data(), text.size());        // terminator already zeroed

            files.push_back({ stem + "/" + stem + ".iscn", std::move(blob) });
        }

        LOG_SUCCESS("PmxImporter::BuildBlobs: %zu files for '%s'", files.size(), stem.c_str());
        return files;
    }

}
