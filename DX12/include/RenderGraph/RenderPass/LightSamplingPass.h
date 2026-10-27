#pragma once
// Lights v2 sampling tables: the local-light PDF texture and the presampled RIS
// buffer RTXDI's light sampling reads, built by the engine so both ReSTIR backends
// read one set of tables.
//
// - PDF texture: R32_FLOAT, kPdfSize x kPdfSize (one texel per light of
//   ClusterPass::kMaxLights). Light i sits at its Z-curve (Morton) texel with its
//   power as the value (0 for directional or inactive lights); every coarser mip
//   texel is the SUM of its 2x2 children, so the top 1x1 mip is the total power.
// - RIS buffer: kTileCount tiles x kTileSize entries of uint2(lightIndex,
//   asuint(1/pdf)), each an independent power-proportional draw made by descending
//   the mip chain; regenerated every frame the tables are wanted.
//
// Built while a DI pass draws RTXDI candidates; -light_tables forces them.
// -light_tables_log also histograms each frame's RIS buffer against the PDF and
// logs [LIGHT_TABLES] chi2= (the self-check a pack asserts).

#include "RenderGraph/RenderGraph.h"
#include "Graphics/FrameCB.h"
#include <array>

class LightSamplingPass final : public RG::RenderPass
{
public:
    static constexpr uint32_t kPdfSize = 64, kPdfMips = 7;      // 64 x 64 = 4096 lights
    static constexpr uint32_t kTileSize = 1024, kTileCount = 128; // RTXDI's default presampling shape
    static constexpr uint32_t kEntries = kTileSize * kTileCount;
    static constexpr uint32_t kMaxLights = kPdfSize * kPdfSize;

    struct ServiceRequest { bool forced = false; };

    struct Tables { uint32_t pdfIdx = 0, risIdx = 0, pdfSize = 0, pdfMips = 0, tileSize = 0, tileCount = 0; };

    explicit LightSamplingPass(RG::RenderGraph& graph) : m_graph(graph) {}
    const char* GetName() const override { return "LightTables"; }
    void Setup(RG::RenderGraphBuilder&) override {}
    void Init(IGraphicsDevice& gfx) override;
    void Shutdown(IGraphicsDevice& gfx) override;
    // Renderer::PassSet delivers Shutdown: the main graph never Destroys its
    // passes, so this is their only Shutdown.
    uint32_t HandledEvents() const override { return kShutdown; }
    void ReloadShaders(IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    bool IsActive() const override { return m_active; }

    // Frame boundary only: build (pipelines + tables) or release them.
    void EnsureResources(bool wanted);

    // The service point: wanted while a DI pass that will run draws RTXDI candidates
    // (the board), or forced (-light_tables, the request).
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext& c) override { EnsureResources(m_request.forced || c.board.diLightTables); }
    void SetFrameData(uint32_t lightsIdx, uint32_t lightCount, uint32_t frame, bool active);

    // Bindless indices of this frame's tables (0 when they are not built).
    Tables GetTables() const;
    bool IsWanted() const { return m_wanted; }

private:
    struct alignas(16) Constants
    {
        uint32_t lightsIdx, lightCount, frame, pdfSize;
        uint32_t mipCount, tileSize, tileCount, risUav;
        uint32_t histogramUav, stage, entries, tableFlags;
        uint32_t pdfUav[8];
    };
    struct alignas(256) Stage { Constants values; };
    static constexpr uint32_t kStages = kPdfMips + 2; // BuildPdf, BuildMip 1..6, Presample, Histogram
    struct Block { Stage stages[kStages]; };

    bool BuildPipelines();
    void Release(IGraphicsDevice& gfx);
    void ReadHistogram(uint32_t slot);

    RG::RenderGraph& m_graph;
    IGraphicsDevice* m_gfx = nullptr;
    RHI::PipelineLayout m_layout;   // one root CBV, the rest bindless
    // BuildPdf, BuildMip, Presample, HistogramClear, HistogramCount
    std::array<RHI::PipelineState, 5> m_pso;
    bool m_pipelinesFailed = false;
    FrameCB<Block> m_cb;
    RHI::Texture m_pdf;
    RHI::GPUBuffer m_ris, m_histogram, m_readback[RHI::kFramesInFlight];
    RHI::ResourceState m_pdfState = RHI::ResourceState::UNDEFINED;
    RHI::ResourceState m_risState = RHI::ResourceState::UNDEFINED;
    RHI::ResourceState m_histogramState = RHI::ResourceState::UNDEFINED;
    const uint32_t* m_mapped[RHI::kFramesInFlight]{};
    bool m_primed[RHI::kFramesInFlight]{};
    uint32_t m_readbackFrame[RHI::kFramesInFlight]{};
    uint32_t m_lightsIdx = 0, m_lightCount = 0, m_frame = 0;
    bool m_active = false, m_wanted = false, m_log = false, m_corrupt = false;
    ServiceRequest m_request;   // OnService's
};
