#include "Graphics/PsoManifest.h"

#include <cstring>
#include <mutex>
#include <unordered_set>

namespace Gfx::PsoManifest
{
namespace
{
    constexpr std::uint32_t kVersion = 1;

    // ==== Little-endian writing and reading ====

    struct Writer
    {
        void U8(std::uint8_t v)   { bytes.push_back(v); }
        void U32(std::uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
        void U64(std::uint64_t v) { U32(static_cast<std::uint32_t>(v)); U32(static_cast<std::uint32_t>(v >> 32)); }
        void I32(std::int32_t v)  { U32(static_cast<std::uint32_t>(v)); }
        void F32(float v)         { std::uint32_t u; std::memcpy(&u, &v, 4); U32(u); }
        void Bytes(const void* p, std::size_t n)
        {
            const auto* b = static_cast<const std::uint8_t*>(p);
            bytes.insert(bytes.end(), b, b + n);
        }

        std::vector<std::uint8_t> bytes;
    };

    struct Reader
    {
        bool Need(std::size_t n) { if (size - at < n) { ok = false; } return ok; }
        std::uint8_t U8() { if (!Need(1)) return 0; return data[at++]; }
        std::uint32_t U32()
        {
            if (!Need(4)) return 0;
            std::uint32_t v = 0;
            for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[at + i]) << (8 * i);
            at += 4;
            return v;
        }
        std::uint64_t U64() { const std::uint64_t lo = U32(); return lo | (static_cast<std::uint64_t>(U32()) << 32); }
        std::int32_t I32()  { return static_cast<std::int32_t>(U32()); }
        float F32()         { const std::uint32_t u = U32(); float v; std::memcpy(&v, &u, 4); return v; }

        const std::uint8_t* data = nullptr;
        std::size_t         size = 0;
        std::size_t         at   = 0;
        bool                ok   = true;
    };

    // ==== The graphics state, field by field ====
    // Never a raw struct: padding is not zero-initialised in descs built on the stack.

    void WriteState(Writer& w, const State& s)
    {
        w.U8(s.blend.AlphaToCoverageEnable ? 1 : 0);
        w.U8(s.blend.IndependentBlendEnable ? 1 : 0);
        for (const D3D12_RENDER_TARGET_BLEND_DESC& rt : s.blend.RenderTarget)
        {
            w.U8(rt.BlendEnable ? 1 : 0);
            w.U8(rt.LogicOpEnable ? 1 : 0);
            w.U32(rt.SrcBlend);
            w.U32(rt.DestBlend);
            w.U32(rt.BlendOp);
            w.U32(rt.SrcBlendAlpha);
            w.U32(rt.DestBlendAlpha);
            w.U32(rt.BlendOpAlpha);
            w.U32(rt.LogicOp);
            w.U8(rt.RenderTargetWriteMask);
        }
        w.U32(s.sampleMask);
        w.U32(s.raster.FillMode);
        w.U32(s.raster.CullMode);
        w.U8(s.raster.FrontCounterClockwise ? 1 : 0);
        w.I32(s.raster.DepthBias);
        w.F32(s.raster.DepthBiasClamp);
        w.F32(s.raster.SlopeScaledDepthBias);
        w.U8(s.raster.DepthClipEnable ? 1 : 0);
        w.U8(s.raster.MultisampleEnable ? 1 : 0);
        w.U8(s.raster.AntialiasedLineEnable ? 1 : 0);
        w.U32(s.raster.ForcedSampleCount);
        w.U32(s.raster.ConservativeRaster);
        w.U8(s.depthStencil.DepthEnable ? 1 : 0);
        w.U32(s.depthStencil.DepthWriteMask);
        w.U32(s.depthStencil.DepthFunc);
        w.U8(s.depthStencil.StencilEnable ? 1 : 0);
        w.U8(s.depthStencil.StencilReadMask);
        w.U8(s.depthStencil.StencilWriteMask);
        for (const D3D12_DEPTH_STENCILOP_DESC* f : { &s.depthStencil.FrontFace, &s.depthStencil.BackFace })
        {
            w.U32(f->StencilFailOp);
            w.U32(f->StencilDepthFailOp);
            w.U32(f->StencilPassOp);
            w.U32(f->StencilFunc);
        }
        w.U32(static_cast<std::uint32_t>(s.inputElements.size()));
        for (const InputElement& e : s.inputElements)
        {
            w.U32(static_cast<std::uint32_t>(e.semanticName.size()));
            w.Bytes(e.semanticName.data(), e.semanticName.size());
            w.U32(e.semanticIndex);
            w.U32(e.format);
            w.U32(e.inputSlot);
            w.U32(e.alignedByteOffset);
            w.U32(e.inputSlotClass);
            w.U32(e.instanceDataStepRate);
        }
        w.U32(s.ibStripCut);
        w.U32(s.topologyType);
        w.U32(s.numRenderTargets);
        for (std::uint32_t f : s.rtvFormats) w.U32(f);
        w.U32(s.dsvFormat);
        w.U32(s.sampleCount);
        w.U32(s.sampleQuality);
        w.U32(s.nodeMask);
        w.U32(s.flags);
    }

    State ReadState(Reader& r)
    {
        State s;
        s.blend.AlphaToCoverageEnable  = r.U8() != 0;
        s.blend.IndependentBlendEnable = r.U8() != 0;
        for (D3D12_RENDER_TARGET_BLEND_DESC& rt : s.blend.RenderTarget)
        {
            rt.BlendEnable           = r.U8() != 0;
            rt.LogicOpEnable         = r.U8() != 0;
            rt.SrcBlend              = static_cast<D3D12_BLEND>(r.U32());
            rt.DestBlend             = static_cast<D3D12_BLEND>(r.U32());
            rt.BlendOp               = static_cast<D3D12_BLEND_OP>(r.U32());
            rt.SrcBlendAlpha         = static_cast<D3D12_BLEND>(r.U32());
            rt.DestBlendAlpha        = static_cast<D3D12_BLEND>(r.U32());
            rt.BlendOpAlpha          = static_cast<D3D12_BLEND_OP>(r.U32());
            rt.LogicOp               = static_cast<D3D12_LOGIC_OP>(r.U32());
            rt.RenderTargetWriteMask = r.U8();
        }
        s.sampleMask                   = r.U32();
        s.raster.FillMode              = static_cast<D3D12_FILL_MODE>(r.U32());
        s.raster.CullMode              = static_cast<D3D12_CULL_MODE>(r.U32());
        s.raster.FrontCounterClockwise = r.U8() != 0;
        s.raster.DepthBias             = r.I32();
        s.raster.DepthBiasClamp        = r.F32();
        s.raster.SlopeScaledDepthBias  = r.F32();
        s.raster.DepthClipEnable       = r.U8() != 0;
        s.raster.MultisampleEnable     = r.U8() != 0;
        s.raster.AntialiasedLineEnable = r.U8() != 0;
        s.raster.ForcedSampleCount     = r.U32();
        s.raster.ConservativeRaster    = static_cast<D3D12_CONSERVATIVE_RASTERIZATION_MODE>(r.U32());
        s.depthStencil.DepthEnable      = r.U8() != 0;
        s.depthStencil.DepthWriteMask   = static_cast<D3D12_DEPTH_WRITE_MASK>(r.U32());
        s.depthStencil.DepthFunc        = static_cast<D3D12_COMPARISON_FUNC>(r.U32());
        s.depthStencil.StencilEnable    = r.U8() != 0;
        s.depthStencil.StencilReadMask  = r.U8();
        s.depthStencil.StencilWriteMask = r.U8();
        for (D3D12_DEPTH_STENCILOP_DESC* f : { &s.depthStencil.FrontFace, &s.depthStencil.BackFace })
        {
            f->StencilFailOp      = static_cast<D3D12_STENCIL_OP>(r.U32());
            f->StencilDepthFailOp = static_cast<D3D12_STENCIL_OP>(r.U32());
            f->StencilPassOp      = static_cast<D3D12_STENCIL_OP>(r.U32());
            f->StencilFunc        = static_cast<D3D12_COMPARISON_FUNC>(r.U32());
        }
        const std::uint32_t count = r.U32();
        for (std::uint32_t i = 0; i < count && r.ok; ++i)
        {
            InputElement e;
            const std::uint32_t n = r.U32();
            if (!r.Need(n)) break;
            e.semanticName.assign(reinterpret_cast<const char*>(r.data + r.at), n);
            r.at += n;
            e.semanticIndex        = r.U32();
            e.format               = r.U32();
            e.inputSlot            = r.U32();
            e.alignedByteOffset    = r.U32();
            e.inputSlotClass       = r.U32();
            e.instanceDataStepRate = r.U32();
            s.inputElements.push_back(std::move(e));
        }
        s.ibStripCut       = r.U32();
        s.topologyType     = r.U32();
        s.numRenderTargets = r.U32();
        for (std::uint32_t& f : s.rtvFormats) f = r.U32();
        s.dsvFormat     = r.U32();
        s.sampleCount   = r.U32();
        s.sampleQuality = r.U32();
        s.nodeMask      = r.U32();
        s.flags         = r.U32();
        return s;
    }

    // ==== The recorder ====

    struct Recorder
    {
        std::mutex                      mutex;
        bool                            enabled = false;
        Manifest                        manifest;
        std::unordered_set<std::wstring> names;
    };

    Recorder& R()
    {
        static Recorder s_recorder;
        return s_recorder;
    }
}

std::uint64_t HashBlob(const void* data, std::size_t size)
{
    std::uint64_t h = 14695981039346656037ull;
    const auto* b = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h ^ size;
}

State StateOf(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    State s;
    s.blend        = desc.BlendState;
    s.sampleMask   = desc.SampleMask;
    s.raster       = desc.RasterizerState;
    s.depthStencil = desc.DepthStencilState;
    for (UINT i = 0; i < desc.InputLayout.NumElements; ++i)
    {
        const D3D12_INPUT_ELEMENT_DESC& d = desc.InputLayout.pInputElementDescs[i];
        InputElement e;
        e.semanticName         = d.SemanticName ? d.SemanticName : "";
        e.semanticIndex        = d.SemanticIndex;
        e.format               = d.Format;
        e.inputSlot            = d.InputSlot;
        e.alignedByteOffset    = d.AlignedByteOffset;
        e.inputSlotClass       = d.InputSlotClass;
        e.instanceDataStepRate = d.InstanceDataStepRate;
        s.inputElements.push_back(std::move(e));
    }
    s.ibStripCut       = desc.IBStripCutValue;
    s.topologyType     = desc.PrimitiveTopologyType;
    s.numRenderTargets = desc.NumRenderTargets;
    for (int i = 0; i < 8; ++i) s.rtvFormats[i] = desc.RTVFormats[i];
    s.dsvFormat     = desc.DSVFormat;
    s.sampleCount   = desc.SampleDesc.Count;
    s.sampleQuality = desc.SampleDesc.Quality;
    s.nodeMask      = desc.NodeMask;
    s.flags         = desc.Flags;
    return s;
}

void ApplyState(const State& state, D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                std::vector<D3D12_INPUT_ELEMENT_DESC>& elements)
{
    desc.BlendState        = state.blend;
    desc.SampleMask        = state.sampleMask;
    desc.RasterizerState   = state.raster;
    desc.DepthStencilState = state.depthStencil;
    elements.clear();
    for (const InputElement& e : state.inputElements)
    {
        D3D12_INPUT_ELEMENT_DESC d{};
        d.SemanticName         = e.semanticName.c_str();
        d.SemanticIndex        = e.semanticIndex;
        d.Format               = static_cast<DXGI_FORMAT>(e.format);
        d.InputSlot            = e.inputSlot;
        d.AlignedByteOffset    = e.alignedByteOffset;
        d.InputSlotClass       = static_cast<D3D12_INPUT_CLASSIFICATION>(e.inputSlotClass);
        d.InstanceDataStepRate = e.instanceDataStepRate;
        elements.push_back(d);
    }
    desc.InputLayout           = { elements.empty() ? nullptr : elements.data(), static_cast<UINT>(elements.size()) };
    desc.IBStripCutValue       = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(state.ibStripCut);
    desc.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(state.topologyType);
    desc.NumRenderTargets      = state.numRenderTargets;
    for (int i = 0; i < 8; ++i) desc.RTVFormats[i] = static_cast<DXGI_FORMAT>(state.rtvFormats[i]);
    desc.DSVFormat          = static_cast<DXGI_FORMAT>(state.dsvFormat);
    desc.SampleDesc.Count   = state.sampleCount;
    desc.SampleDesc.Quality = state.sampleQuality;
    desc.NodeMask           = state.nodeMask;
    desc.Flags              = static_cast<D3D12_PIPELINE_STATE_FLAGS>(state.flags);
}

std::vector<std::uint8_t> Serialize(const Manifest& manifest)
{
    Writer w;
    w.Bytes("IPSM", 4);
    w.U32(kVersion);
    w.U32(static_cast<std::uint32_t>(manifest.blobs.size()));
    w.U32(static_cast<std::uint32_t>(manifest.records.size()));
    for (const auto& [hash, bytes] : manifest.blobs)
    {
        w.U64(hash);
        w.U32(static_cast<std::uint32_t>(bytes.size()));
        w.Bytes(bytes.data(), bytes.size());
    }
    for (const Record& rec : manifest.records)
    {
        Writer body;
        body.U8(static_cast<std::uint8_t>(rec.kind));
        body.U8(0); body.U8(0); body.U8(0);
        body.U32(rec.estimatedUs);
        body.U32(static_cast<std::uint32_t>(rec.name.size()));
        for (wchar_t c : rec.name)
        {
            body.U8(static_cast<std::uint8_t>(c));
            body.U8(static_cast<std::uint8_t>(c >> 8));
        }
        body.U64(rec.rootSignature);
        for (std::uint64_t s : rec.stages) body.U64(s);
        if (rec.kind != Kind::Compute) WriteState(body, rec.state);
        w.U32(static_cast<std::uint32_t>(body.bytes.size()));
        w.Bytes(body.bytes.data(), body.bytes.size());
    }
    return std::move(w.bytes);
}

bool Parse(const std::uint8_t* data, std::size_t size, Manifest& out, std::string& error)
{
    out = {};
    Reader r{ data, size };
    if (!r.Need(16) || std::memcmp(data, "IPSM", 4) != 0) { error = "not a PSO manifest (bad magic)"; return false; }
    r.at = 4;
    const std::uint32_t version = r.U32();
    if (version != kVersion) { error = "version " + std::to_string(version) + " (this build reads 1)"; return false; }
    const std::uint32_t blobCount   = r.U32();
    const std::uint32_t recordCount = r.U32();
    for (std::uint32_t i = 0; i < blobCount; ++i)
    {
        const std::uint64_t hash = r.U64();
        const std::uint32_t n    = r.U32();
        if (!r.Need(n)) { error = "blob " + std::to_string(i) + " runs past the end"; return false; }
        out.blobs[hash].assign(data + r.at, data + r.at + n);
        r.at += n;
    }
    for (std::uint32_t i = 0; i < recordCount; ++i)
    {
        const std::uint32_t n = r.U32();
        if (!r.Need(n)) { error = "record " + std::to_string(i) + " runs past the end"; return false; }
        Reader body{ data + r.at, n };
        r.at += n;
        Record rec;
        const std::uint8_t kind = body.U8();
        body.U8(); body.U8(); body.U8();
        rec.estimatedUs = body.U32();
        const std::uint32_t chars = body.U32();
        if (!body.Need(static_cast<std::size_t>(chars) * 2)) { error = "record " + std::to_string(i) + ": its name"; return false; }
        for (std::uint32_t c = 0; c < chars; ++c)
        {
            const std::uint32_t lo = body.U8();
            rec.name.push_back(static_cast<wchar_t>(lo | (static_cast<std::uint32_t>(body.U8()) << 8)));
        }
        rec.rootSignature = body.U64();
        for (std::uint64_t& s : rec.stages) s = body.U64();
        if (kind > static_cast<std::uint8_t>(Kind::Compute)) continue;   // a kind this build does not know
        rec.kind = static_cast<Kind>(kind);
        if (rec.kind != Kind::Compute) rec.state = ReadState(body);
        if (!body.ok) { error = "record " + std::to_string(i) + " is truncated"; return false; }
        out.records.push_back(std::move(rec));
    }
    if (!r.ok) { error = "truncated"; return false; }
    return true;
}

std::uint64_t ContentHash(const Manifest& manifest)
{
    std::uint64_t h = 14695981039346656037ull;
    auto mix = [&h](std::uint64_t v) { for (int i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xFF; h *= 1099511628211ull; } };
    for (const Record& rec : manifest.records)
    {
        mix(HashBlob(rec.name.data(), rec.name.size() * sizeof(wchar_t)));
        mix(rec.rootSignature);
        for (std::uint64_t s : rec.stages) mix(s);
    }
    return h;
}

void EnableRecording()
{
    std::lock_guard<std::mutex> lock(R().mutex);
    R().enabled = true;
}

bool RecordingEnabled()
{
    std::lock_guard<std::mutex> lock(R().mutex);
    return R().enabled;
}

bool IsRecorded(const std::wstring& name)
{
    std::lock_guard<std::mutex> lock(R().mutex);
    return R().names.count(name) != 0;
}

void AddRecord(const Gfx::PsoManifest::Record& record,
               const std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>>& blobs)
{
    Recorder& r = R();
    std::lock_guard<std::mutex> lock(r.mutex);
    if (!r.enabled || !r.names.insert(record.name).second) return;
    for (const auto& [hash, bytes] : blobs)
        if (hash != 0 && !r.manifest.blobs.count(hash)) r.manifest.blobs.emplace(hash, bytes);
    r.manifest.records.push_back(record);
}

Manifest Recorded()
{
    std::lock_guard<std::mutex> lock(R().mutex);
    return R().manifest;
}
}
