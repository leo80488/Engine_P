#include "Resource/ShaderLoader.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"

namespace Resource
{
    ShaderLoader::ShaderLoader(IGraphicsDevice* gfx) : m_gfx(gfx) {}

    LoadResult ShaderLoader::Load(const std::string& path, std::span<const std::uint8_t> data)
    {
        if (!m_gfx)
        {
            LOG_ERROR("ShaderLoader: IGraphicsDevice is null (path: '%s')", path.c_str());
            return {};
        }
        if (data.empty())
        {
            LOG_ERROR("ShaderLoader: empty data for '%s'", path.c_str());
            return {};
        }

        LoadResult refused;
        if (!ValidateHeader(data.data(), data.size(), MAGIC_SHADER, refused.failReason))
        {
            LOG_ERROR("ShaderLoader: invalid .ishdr header in '%s': %s", path.c_str(), refused.failReason.c_str());
            return refused;
        }

        const ShaderMetadata* shMeta    = GetMetadata<ShaderMetadata>(data.data());
        const uint8_t*        dxbcData  = GetPayload(data.data());
        const size_t          dxbcSize  = GetHeader(data.data())->dataSize;

        const auto stage = static_cast<RHI::ShaderStage>(shMeta->stage);

        RHI::Shader shader{};
        if (!m_gfx->CreateShader(stage, dxbcData, dxbcSize, shader))
        {
            LOG_ERROR("ShaderLoader: CreateShader failed for '%s'", path.c_str());
            return {};
        }

        auto resource = std::make_unique<ShaderResource>(
            stage,
            RHI::ShaderFormat::HLSL5,
            static_cast<RHI::ShaderModel>(shMeta->shaderModel),
            shader);

        return { std::move(resource), nullptr };
    }
}
