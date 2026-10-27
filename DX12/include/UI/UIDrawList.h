#pragma once

// UIDrawList - CPU-side draw list (Dear ImGui style). Canvas UI emits primitives into it
// during paint; UIPass uploads the vertex/index/command buffers once a frame and turns each
// command into a draw call.
//   Vertex: 20 bytes (pos.xy, uv.xy, RGBA8 colour packed into a uint32).
//   Indices: uint16 (source vertex indices 0..65535).
//   Commands: { indexOffset, indexCount, clipRect, texture, materialID, effectIndex,
//               samplerId, customShaderSlot }.

#include <cstdint>
#include <string>
#include <vector>

namespace UI
{
    // Packed 0xAABBGGRR (matches Dear ImGui IM_COL32 byte order so debugging
    // tools that already understand IM_COL32 can be reused unchanged).
    struct Color32
    {
        uint32_t rgba = 0xFFFFFFFFu;

        constexpr Color32() = default;
        constexpr Color32(uint32_t v) : rgba(v) {}
        constexpr Color32(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
            : rgba(uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24)) {}

        static constexpr Color32 White() { return Color32(255, 255, 255, 255); }
        static constexpr Color32 Black() { return Color32(0,   0,   0,   255); }
        static constexpr Color32 Transparent() { return Color32(0, 0, 0, 0); }
    };

    struct Vec2
    {
        float x = 0.f, y = 0.f;
        constexpr Vec2() = default;
        constexpr Vec2(float xx, float yy) : x(xx), y(yy) {}
    };

    // Fields are `mn` / `mx`, not `min` / `max`, so the type compiles in TUs that pulled in
    // <Windows.h>'s min/max macros (NOMINMAX is not enforced project-wide).
    struct Rect
    {
        Vec2 mn{}, mx{};
        constexpr Rect() = default;
        constexpr Rect(const Vec2& a, const Vec2& b) : mn(a), mx(b) {}
        float Width()  const { return mx.x - mn.x; }
        float Height() const { return mx.y - mn.y; }
        bool  Contains(const Vec2& p) const
        { return p.x >= mn.x && p.y >= mn.y && p.x < mx.x && p.y < mx.y; }
        bool  Intersects(const Rect& o) const
        { return !(o.mx.x <= mn.x || o.mn.x >= mx.x ||
                   o.mx.y <= mn.y || o.mn.y >= mx.y); }
    };

    struct UIVertex
    {
        float    pos[2];   // screen-space pixels (UIPass shader applies ortho)
        float    uv[2];    // 0..1 if texture, otherwise unused
        uint32_t col;      // RGBA8 packed (see Color32::rgba layout)
    };
    static_assert(sizeof(UIVertex) == 20, "UIVertex must be 20 bytes");

    // ---- Text effects (SDF text only) ----
    // Fully resolved style handed to Font::DrawText. Every effect is an independent toggle.
    // Widths/offsets are kTextRefPx-relative pixels (they scale with the text); colours RGBA8.
    struct TextEffect
    {
        bool    outline      = false;
        Color32 outlineColor = Color32::Black();
        float   outlineWidthPx = 2.f;

        bool    glow      = false;
        Color32 glowColor = Color32::White();
        float   glowWidthPx = 4.f;

        bool    shadow       = false;
        Color32 shadowColor  = Color32(0, 0, 0, 160);
        Vec2    shadowOffsetPx{ 2.f, 2.f };

        bool    jitter       = false;
        float   jitterAmpPx  = 2.f;
        float   jitterFreq   = 12.f;

        // Optional custom pixel shader (RegisterTextShader slot; 0 = the built-in SDF shader). It
        // replaces UI.ps.hlsl's SDF branch for this text's glyph quads; customParams reach it
        // through the effects table entry (customA/customB in TextFX.hlsli).
        uint32_t customShaderSlot = 0;
        float    customParams[8]  = {};
    };

    // GPU mirror of one SDF effect entry: must match UITextEffect in UI.ps.hlsl and
    // shaders/TextFX.hlsli (16-byte aligned, 96 bytes). Widths are normalised (pixels /
    // FontMetrics::sdfPixelRange) so the shader is scale-agnostic. customA/customB carry
    // TextEffect::customParams. runRect is filled by Font::DrawText for custom-shader runs so an
    // effect can span the whole text: {originX, originY, 1/width, 1/height} in canvas pixels,
    // which TextRunT() in TextFX.hlsli turns into a 0..1 position across the run.
    struct GpuTextEffect
    {
        float outlineColor[4] = { 0.f, 0.f, 0.f, 1.f };
        float glowColor[4]    = { 1.f, 1.f, 1.f, 1.f };
        float outlineWidthN   = 0.f;
        float glowWidthN      = 0.f;
        float softness        = 0.f;
        float _pad            = 0.f;
        float customA[4]      = { 0.f, 0.f, 0.f, 0.f };
        float customB[4]      = { 0.f, 0.f, 0.f, 0.f };
        float runRect[4]      = { 0.f, 0.f, 0.f, 0.f };
    };
    static_assert(sizeof(GpuTextEffect) == 96, "mirror UI.ps.hlsl + TextFX.hlsli");

    // UV address mode for UI images. Clamp = default (atlas / sprite-sheet
    // sub-rects); Wrap / Mirror tile when uv goes outside [0,1].
    enum class UIWrapMode : uint32_t { Clamp = 0, Wrap = 1, Mirror = 2 };

    // Sampler slots UIPass creates, indexed by UISamplerId(wrap, point):
    //   [0..2] linear: Clamp / Wrap / Mirror     [3..5] point: Clamp / Wrap / Mirror
    static constexpr uint32_t kUISamplerCount = 6;
    inline uint32_t UISamplerId(UIWrapMode wrap, bool pointFilter)
    {
        return (pointFilter ? 3u : 0u) + static_cast<uint32_t>(wrap);
    }

    // Texture binding identity for a draw command. srvGpuHandle == 0 means the engine's
    // default 1x1 white texture, so AddRectFilled needs no atlas.
    struct UITextureRef
    {
        uint64_t srvGpuHandle = 0;
    };

    struct UIDrawCmd
    {
        uint32_t     indexOffset = 0;
        uint32_t     indexCount  = 0;
        Rect         clipRect;       // axis-aligned scissor (pixels)
        UITextureRef texture;
        uint32_t     materialID  = 0; // 0=plain (image/fill), 1=SDF text (live)
        uint32_t     effectIndex = 0; // index into UIDrawList::Effects() (0 = none)
        uint32_t     samplerId   = 0; // index into UIPass samplers (0 = clamp-linear)
        // Custom text pixel shader slot (RegisterTextShader; 0 = built-in
        // UI.ps.hlsl). UIPass resolves the slot to a dynamic PSO per command.
        uint32_t     customShaderSlot = 0;
    };

    // ---- Custom text shader registry (global, main thread) ----
    // Maps a custom text pixel-shader path to a stable slot id (>= 1; 0 = the built-in
    // shader). Backslashes become '/', and a directory-qualified path that does not start with
    // shaders/ or asset/ (text_fx/Foo.ps.hlsl) gets shaders/ prepended; a bare filename is left
    // for ShaderLibrary to prefix. UIPass turns slots into dynamic ShaderIDs on its own
    // ShaderLibrary at draw time.
    uint32_t RegisterTextShader(const std::string& hlslPath);
    const std::vector<std::string>& TextShaderPaths(); // [slot-1] = path

    class UIDrawList
    {
    public:
        // ---- capacity ----
        // Indices are uint16, so a source vertex index above 65535 cannot be represented: past the
        // cap an emitter's static_cast<uint16_t> of the vertex base wraps and the primitive
        // silently indexes unrelated geometry. The emitters test these caps and drop the primitive
        // instead. The index cap matches UIPass::kMaxIndices: the pass stops expanding there, so
        // nothing past it reaches the GPU anyway.
        static constexpr size_t kMaxSourceVertices = 65536; // indices 0..65535
        static constexpr size_t kMaxSourceIndices  = 65535;

        // ---- lifecycle ----
        void Clear();
        bool IsEmpty() const { return m_indices.empty(); }

        // ---- capacity ----
        // Ask before building a primitive that emits in several steps (the debug HUD, the canvas
        // wireframe) so it degrades as a whole rather than being cut in half.
        bool HasRoomFor(size_t verts, size_t indices) const
        {
            return m_verts.size()   + verts   <= kMaxSourceVertices
                && m_indices.size() + indices <= kMaxSourceIndices;
        }

        // ---- clipping ----
        // Push an axis-aligned scissor rect. New commands inherit the current
        // clip until the matching Pop. Stacked scissors intersect (top wins).
        void PushClipRect(const Rect& r);
        void PopClipRect();
        Rect CurrentClipRect() const;

        // ---- texture ----
        // Bind a texture for subsequent primitives. Pass {} to revert to the
        // default white texture (solid-colour fills).
        void PushTexture(const UITextureRef& tex);
        void PopTexture();

        // ---- transform (pivot-anchored scale) ----
        // Pushes (pivot, scale): every later primitive vertex P is written as
        // `(P - pivot) * scale + pivot`. World-space UI uses it to scale whole UI roots about their
        // projected pivot. Stacked transforms apply most-recent push first (OpenGL pushMatrix
        // semantics). Hit testing is NOT transformed: widgets that need click-accurate scaling
        // clamp the visual scale to 1 or disable hit testing.
        void PushTransform(const Vec2& pivot, const Vec2& scale);
        void PopTransform();

        // ---- primitives (filled) ----
        void AddRectFilled(const Vec2& min, const Vec2& max, Color32 col);
        // Rounded rect: r=0 is a sharp rect, r>0 carves the four corners.
        // segmentsPerCorner caps tessellation; design defaults are fine.
        void AddRectFilledRounded(const Vec2& min, const Vec2& max,
                                  Color32 col, float radius,
                                  int segmentsPerCorner = 6);
        void AddTriangleFilled(const Vec2& a, const Vec2& b, const Vec2& c, Color32 col);
        void AddCircleFilled(const Vec2& center, float radius,
                             Color32 col, int segments = 24);

        // ---- primitives (outline) ----
        void AddRect(const Vec2& min, const Vec2& max, Color32 col, float thickness = 1.f);
        void AddLine(const Vec2& a, const Vec2& b, Color32 col, float thickness = 1.f);

        // ---- textured ----
        // Sample @p tex over the rect. uv0/uv1 are texture-space coordinates
        // [0,1]. Tint is multiplied with the sampled colour.
        void AddImage(const UITextureRef& tex,
                      const Vec2& min, const Vec2& max,
                      const Vec2& uv0 = Vec2(0, 0), const Vec2& uv1 = Vec2(1, 1),
                      Color32 tint = Color32::White(),
                      uint32_t samplerId = 0);

        // AddImage that also tags the command with a material id (1 = SDF text) and an
        // effect-table index, so glyph quads take UI.ps.hlsl's SDF branch while plain images stay
        // id 0. customShaderSlot (RegisterTextShader) routes the command to a custom text pixel
        // shader; 0 = built-in.
        void AddImageMat(const UITextureRef& tex,
                         const Vec2& min, const Vec2& max,
                         const Vec2& uv0, const Vec2& uv1, Color32 tint,
                         uint32_t materialID, uint32_t effectIndex,
                         uint32_t samplerId = 0, uint32_t customShaderSlot = 0);

        // Append a resolved SDF effect to this frame's effect palette; returns
        // its index (>= 1). Index 0 is always the no-op effect. UIPass uploads
        // Effects() to the b2 effects constant buffer each frame.
        uint32_t AddTextEffect(const GpuTextEffect& fx);
        const std::vector<GpuTextEffect>& Effects() const { return m_effects; }

        // ---- raw access (consumed by UIPass) ----
        // Text quads come from UI::Font::DrawText (UI/Font.h); the draw list has no font binding
        // (fonts resolve per text element through UI::FontLibrary).
        const std::vector<UIVertex>&  Vertices()  const { return m_verts; }
        const std::vector<uint16_t>&  Indices()   const { return m_indices; }
        const std::vector<UIDrawCmd>& Commands()  const { return m_cmds; }

        // FNV-1a 64 over everything UIPass consumes: vertices, indices, every command field and
        // the effects table. Equal hashes draw the same pixels (textures compare by descriptor
        // handle, so only within one process). The UI Editor's view service skips re-recording a
        // picture whose hash did not move, and its parity pack compares two paints with it.
        uint64_t ContentHash() const;

        // ---- view transform (UI Editor designer) ----
        // Map everything already emitted from design to view pixels: p' = p * zoom + pan for every
        // vertex and clip rect (the clip then intersected with @p bounds, the view's rect), and for
        // each custom-shader run rect in the effects table (origin moves, the reciprocal extent is
        // divided by zoom) - TextRunT compares SV_Position against it, so an untransformed run rect
        // would misplace a zoomed gradient. SDF-normalised outline / glow widths need nothing.
        // Apply once, after painting, before upload. @p transformRunRects false leaves the run
        // rects alone (a test's reverse arm).
        void ApplyViewTransform(float zoom, const Vec2& pan, const Rect& bounds, bool transformRunRects = true);

    private:
        struct XForm { Vec2 pivot; Vec2 scale; };

        static constexpr uint32_t kNoOverflowScratch = 0xFFFFFFFFu;

        Vec2 TransformPoint(const Vec2& p) const;

        // Start a new command unless the last one has the same texture / clip / material /
        // effect / sampler / custom shader and no transform is active.
        void EnsureCmd(const UITextureRef& tex, uint32_t materialID,
                       uint32_t effectIndex, uint32_t samplerId,
                       uint32_t customShaderSlot = 0);

        uint32_t ReserveQuad(); // returns first vertex index (4 verts + 6 indices)

        std::vector<UIVertex>     m_verts;
        std::vector<uint16_t>     m_indices;
        std::vector<UIDrawCmd>    m_cmds;
        std::vector<GpuTextEffect> m_effects; // [0] = no-op; appended per styled run

        std::vector<Rect>         m_clipStack;
        std::vector<UITextureRef> m_texStack;

        std::vector<XForm>        m_xformStack;

        // Shared 4-vertex parking spot handed to every quad requested once the list is full, so
        // ReserveQuad keeps its "the caller may write four vertices at the returned base" contract
        // without the array growing. Nothing indexes it. Reset by Clear().
        uint32_t m_overflowScratchBase = kNoOverflowScratch;
    };

} // namespace UI
