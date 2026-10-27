#pragma once

#include "Resource/Resource.h"
#include "Graphics/GraphicsStruct.h"

namespace Resource
{
    // GPU shader resource: wraps an RHI::Shader handle and metadata about how it was compiled.
    class ShaderResource : public Resource
    {
    public:
        ShaderResource() = default;

        ShaderResource(RHI::ShaderStage stage,
                       RHI::ShaderFormat format,
                       RHI::ShaderModel model,
                       const RHI::Shader& shader)
            : m_stage(stage)
            , m_format(format)
            , m_model(model)
            , m_shader(shader)
        {
        }

        const RHI::Shader& GetShader() const { return m_shader; }
        RHI::ShaderStage   GetStage() const { return m_stage; }
        RHI::ShaderFormat  GetFormat() const { return m_format; }
        RHI::ShaderModel   GetModel() const { return m_model; }

    private:
        RHI::Shader      m_shader{};
        RHI::ShaderStage m_stage{ RHI::ShaderStage::Count };
        RHI::ShaderFormat m_format{ RHI::ShaderFormat::NONE };
        RHI::ShaderModel  m_model{ RHI::ShaderModel::SM_5_0 };
    };
}

