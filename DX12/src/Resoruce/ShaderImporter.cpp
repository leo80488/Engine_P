#include "Resource/ShaderImporter.h"
#include "Resource/AssetHeader.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/DxcCompiler.h"
#include "System/Log.h"
#include <cstring>
#include <cctype>

namespace
{
    RHI::ShaderStage InferStageFromPath(const std::string& path)
    {
        std::string lower = path;
        for (char& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

        auto ends = [&lower](const char* suffix) {
            const size_t len = std::strlen(suffix);
            return lower.size() >= len &&
                   lower.compare(lower.size() - len, len, suffix) == 0;
        };

        if (ends(".vs.hlsl")) return RHI::ShaderStage::VS;
        if (ends(".ps.hlsl")) return RHI::ShaderStage::PS;
        if (ends(".cs.hlsl")) return RHI::ShaderStage::CS;
        if (ends(".gs.hlsl")) return RHI::ShaderStage::GS;
        if (ends(".hs.hlsl")) return RHI::ShaderStage::HS;
        if (ends(".ds.hlsl")) return RHI::ShaderStage::DS;
        return RHI::ShaderStage::PS; // fallback
    }
}

namespace Resource
{
    std::vector<uint8_t> ShaderImporter::Import(const std::string& sourcePath,
                                                  const std::vector<uint8_t>& sourceData)
    {
        if (sourceData.empty())
        {
            LOG_ERROR("ShaderImporter: empty source for '%s'", sourcePath.c_str());
            return {};
        }

        const RHI::ShaderStage stage = InferStageFromPath(sourcePath);

        DxcCompiler::CompileOptions opts;
        opts.sourceName = sourcePath;
        opts.entry      = "main";
        opts.stage      = stage;
#ifdef _DEBUG
        opts.debug      = true;
#else
        opts.debug      = false;
#endif

        const DxcCompiler::CompileResult cr =
            DxcCompiler::Compile(sourceData.data(), sourceData.size(), opts);
        if (!cr.ok)
        {
            LOG_ERROR("ShaderImporter: DXC compile failed for '%s'\n%s",
                      sourcePath.c_str(), cr.errorMsg.c_str());
            return {};
        }

        // .ishdr blob: [AssetHeader][ShaderMetadata][DXIL container]
        ShaderMetadata shMeta{};
        shMeta.bytecodeSize = static_cast<uint32_t>(cr.dxil.size());
        shMeta.stage        = static_cast<uint8_t>(stage);
        shMeta.shaderModel  = static_cast<uint8_t>(RHI::ShaderModel::SM_6_6);
        strncpy_s(shMeta.entryPoint, sizeof(shMeta.entryPoint), "main", 4);

        AssetHeader header{};
        header.magic        = MAGIC_SHADER;
        header.version      = CurrentVersion(MAGIC_SHADER);
        header.resourceType = static_cast<uint16_t>(ResourceType::Shader);
        header.metadataSize = sizeof(ShaderMetadata);
        header.dataSize     = shMeta.bytecodeSize;
        header.flags        = 0;
        header.reserved     = 0;

        const size_t totalSize = sizeof(AssetHeader) + sizeof(ShaderMetadata) + cr.dxil.size();
        std::vector<uint8_t> blob(totalSize);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header, sizeof(AssetHeader));    dst += sizeof(AssetHeader);
        std::memcpy(dst, &shMeta, sizeof(ShaderMetadata)); dst += sizeof(ShaderMetadata);
        std::memcpy(dst, cr.dxil.data(), cr.dxil.size());

        LOG_INFO("ShaderImporter: compiled '%s' → .ishdr (stage=%s, %zu bytes)",
                 sourcePath.c_str(), DxcCompiler::ProfileForStage(stage), totalSize);
        return blob;
    }
}
