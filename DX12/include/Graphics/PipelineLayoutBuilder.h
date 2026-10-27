#pragma once
// Assembles an RHI::PipelineLayoutDesc by typed root slot. The slot a call names IS
// the root parameter it fills, so the slot constants a pass binds with
// (RHI::Slots::CmpParam / GfxParam) and the layout cannot disagree. Desc() requires
// every index from 0 to the highest slot exactly once; a hole or a repeat logs once
// and yields a desc the device refuses.
//
//     RHI::PipelineLayoutBuilder b("RTShadow");
//     b.RootCBV(kCB, 0).RootSRV(kTLAS, 0)
//      .Table(kSRV_Depth, { RHI::SrvRange(1) })
//      .Sampler(linearWrap, 1)
//      .Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
//     gfx.CreatePipelineLayout(b.Desc(), m_layout);   // the builder outlives the call

#include "Graphics/GraphicsStruct.h"
#include "System/Check.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace RHI
{
    inline DescriptorRange SrvRange(uint32_t baseRegister, uint32_t count = 1, uint32_t space = 0)
    { DescriptorRange r; r.type = DescriptorRange::Type::SRV; r.baseRegister = baseRegister; r.count = count; r.space = space; return r; }
    inline DescriptorRange UavRange(uint32_t baseRegister, uint32_t count = 1, uint32_t space = 0)
    { DescriptorRange r; r.type = DescriptorRange::Type::UAV; r.baseRegister = baseRegister; r.count = count; r.space = space; return r; }
    inline DescriptorRange CbvRange(uint32_t baseRegister, uint32_t count = 1, uint32_t space = 0)
    { DescriptorRange r; r.type = DescriptorRange::Type::CBV; r.baseRegister = baseRegister; r.count = count; r.space = space; return r; }

    class PipelineLayoutBuilder
    {
    public:
        explicit PipelineLayoutBuilder(const char* name) : m_name(name) {}

        template <class Slot>
        PipelineLayoutBuilder& Constants(Slot slot, uint32_t num32BitValues, uint32_t reg, uint32_t space = 0,
                                         ShaderVisibility visibility = ShaderVisibility::ALL)
        {
            LayoutParam p;
            p.kind = LayoutParam::Kind::CONSTANTS; p.reg = reg; p.space = space;
            p.num32BitValues = num32BitValues; p.visibility = visibility;
            return Put(slot.index, p);
        }
        template <class Slot>
        PipelineLayoutBuilder& RootCBV(Slot slot, uint32_t reg, uint32_t space = 0,
                                       ShaderVisibility visibility = ShaderVisibility::ALL)
        { return Root(slot.index, LayoutParam::Kind::CBV, reg, space, visibility); }
        template <class Slot>
        PipelineLayoutBuilder& RootSRV(Slot slot, uint32_t reg, uint32_t space = 0,
                                       ShaderVisibility visibility = ShaderVisibility::ALL)
        { return Root(slot.index, LayoutParam::Kind::SRV, reg, space, visibility); }
        template <class Slot>
        PipelineLayoutBuilder& RootUAV(Slot slot, uint32_t reg, uint32_t space = 0,
                                       ShaderVisibility visibility = ShaderVisibility::ALL)
        { return Root(slot.index, LayoutParam::Kind::UAV, reg, space, visibility); }
        template <class Slot>
        PipelineLayoutBuilder& Table(Slot slot, std::initializer_list<DescriptorRange> ranges,
                                     ShaderVisibility visibility = ShaderVisibility::ALL)
        {
            LayoutParam p;
            p.kind = LayoutParam::Kind::TABLE; p.visibility = visibility;
            p.firstRange = static_cast<uint32_t>(m_ranges.size());
            p.rangeCount = static_cast<uint32_t>(ranges.size());
            m_ranges.insert(m_ranges.end(), ranges);
            return Put(slot.index, p);
        }

        PipelineLayoutBuilder& Sampler(const SamplerDesc& desc, uint32_t reg, uint32_t space = 0,
                                       ShaderVisibility visibility = ShaderVisibility::ALL)
        {
            StaticSampler s;
            s.desc = desc; s.reg = reg; s.space = space; s.visibility = visibility;
            m_samplers.push_back(s);
            return *this;
        }

        PipelineLayoutBuilder& Flags(LayoutFlags flags) { m_flags = flags; return *this; }

        PipelineLayoutDesc Desc() const
        {
            bool complete = true;
            for (size_t i = 0; i < m_given.size(); ++i)
                complete = complete && m_given[i] == 1;
            (void)ENGINE_CHECK_ONCE(complete, "PipelineLayoutBuilder '%s': a root slot was given twice or "
                                    "skipped - the layout is refused", m_name ? m_name : "?");
            PipelineLayoutDesc d;
            d.name         = m_name;
            d.params       = complete && !m_params.empty() ? m_params.data() : nullptr;
            d.paramCount   = static_cast<uint32_t>(m_params.size());
            d.ranges       = m_ranges.empty() ? nullptr : m_ranges.data();
            d.rangeCount   = static_cast<uint32_t>(m_ranges.size());
            d.samplers     = m_samplers.empty() ? nullptr : m_samplers.data();
            d.samplerCount = static_cast<uint32_t>(m_samplers.size());
            d.flags        = m_flags;
            return d;
        }

    private:
        PipelineLayoutBuilder& Root(uint32_t index, LayoutParam::Kind kind, uint32_t reg, uint32_t space,
                                    ShaderVisibility visibility)
        {
            LayoutParam p;
            p.kind = kind; p.reg = reg; p.space = space; p.visibility = visibility;
            return Put(index, p);
        }
        PipelineLayoutBuilder& Put(uint32_t index, const LayoutParam& p)
        {
            if (index >= m_params.size())
            {
                m_params.resize(index + 1);
                m_given.resize(index + 1, 0);
            }
            m_params[index] = p;
            ++m_given[index];
            return *this;
        }

        const char*                  m_name = nullptr;
        std::vector<LayoutParam>     m_params;
        std::vector<uint8_t>         m_given;
        std::vector<DescriptorRange> m_ranges;
        std::vector<StaticSampler>   m_samplers;
        LayoutFlags                  m_flags = LayoutFlags::NONE;
    };
}
