#pragma once
#include "Collections/AABB.hpp"
#include "Collections/Color.hpp"
#include "Collections/Vec2.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Core/Export.hpp"
#include "Graphics/Text/FontHandle.hpp"

#include <cstdint>
#include <glm/mat4x4.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Axiom {

    class Scene;
    class Shader;
    class Font;
    struct TextRendererComponent;

    // Per-vertex layout pushed to the GPU. Six of these per visible glyph,
    // built fresh every frame — keeps the implementation simple at a
    // small memory cost (~32 bytes × 6 verts × visible glyphs). When that
    // becomes a hot-path concern we'll move to instanced quads with a
    // per-instance UV-rect attribute.
    struct TextVertex {
        float X = 0.0f;
        float Y = 0.0f;
        float U = 0.0f;
        float V = 0.0f;
        float R = 1.0f;
        float G = 1.0f;
        float B = 1.0f;
        float A = 1.0f;
    };

    // Generic text-draw command for `RenderInstances`. Used by both the
    // entity-driven world-space pass and the UI renderer (which builds a
    // command list of widget labels, dropdown popup options, etc).
    //
    //   FontPtr — already-resolved Font (call ResolveFont on a
    //     TextRendererComponent or FontManager directly).
    //   Text — non-owning view into the source string. Caller must keep
    //     it alive for the duration of the RenderInstances call.
    //   X / Y — baseline origin in the same world units `mvp` projects.
    //     (For screen-space UI text, the same centered-screen-space
    //     coords RectTransform2D resolves to.)
    //   Scale — multiplier from atlas pixels to world units. Built from
    //     (FontSize / atlasBakedSize) / k_TextPixelsPerWorldUnit for
    //     entity-driven text; for raw screen-space text use
    //     (desiredPixelHeight / atlasBakedSize).
    struct AXIOM_API TextDrawCmd {
        Font* FontPtr = nullptr;
        std::string_view Text;
        float X = 0.0f;
        float Y = 0.0f;
        float Scale = 1.0f;
        float LetterSpacing = 0.0f;
        Color Tint{};
        TextAlignment Align = TextAlignment::Left;
        // Wrap strategy + width in atlas-pixel units (same domain as
        // glyph advances — no Scale baked in). When Wrap == None the
        // renderer keeps its legacy single-line-per-`\n` behaviour;
        // otherwise lines exceeding WrapWidthPixels are broken at word
        // boundaries (Word) or arbitrary glyphs (Character). 0 / negative
        // WrapWidthPixels also short-circuits to no-wrap so callers can
        // safely default it.
        TextWrapMode Wrap = TextWrapMode::None;
        float WrapWidthPixels = 0.0f;
        int16_t SortingOrder = 0;
        uint8_t SortingLayer = 0;
        // Hierarchy walk index used by GuiRenderer as a tiebreaker
        // after (SortingLayer, SortingOrder). Lower values draw first
        // (further back). Mirrors Instance44::DrawIndex so the merged
        // image+text sort space stays coherent.
        std::uint32_t DrawIndex = 0;

        // Optional clip rect (centered-screen-space pixels). Mirrors
        // Instance44::HasClip / ClipMin / ClipMax — GuiRenderer
        // applies glScissor for text under a UI Mask ancestor.
        bool HasClip = false;
        Vec2 ClipMin{};
        Vec2 ClipMax{};

        // Optional rotation around Pivot (radians). Default 0 = identity,
        // and EmitText short-circuits the rotation step entirely so the
        // overwhelmingly common axis-aligned text path stays a hot loop
        // of plain quad emits. Set on UI text whose RectTransform has a
        // non-zero composed rotation; the world-space TextRenderer path
        // (Renderer2D-driven) doesn't use this and leaves it at zero.
        float Rotation = 0.0f;
        Vec2 Pivot{};
    };

    class AXIOM_API TextRenderer {
    public:
        TextRenderer();
        ~TextRenderer();

        TextRenderer(const TextRenderer&) = delete;
        TextRenderer& operator=(const TextRenderer&) = delete;

        // GL setup. Safe to call once a GL context is alive.
        void Initialize();

        // Render every TextRendererComponent in the scene whose AABB intersects
        // the supplied view frustum, transformed by `vp`. Called by
        // Renderer2D as part of its scene render pass — sits *after*
        // sprite submission so text always layers on top of sprites by
        // default. Within text, sorting follows (SortingLayer,
        // SortingOrder, font atlas) for batching efficiency.
        void RenderScene(Scene& scene, const glm::mat4& vp, const AABB& viewportAABB);

        // Generic batched text draw. Sorts the supplied commands by
        // (layer, order, atlas) and emits them with the same shader /
        // VAO pipeline as the entity-driven path. UIRenderer calls this
        // for screen-space widget labels + dropdown popups.
        //
        // `viewId` is forwarded to the render backend so a caller
        // hosting its own UI view (GuiRenderer's per-instance UI view)
        // can route text submits onto the same view it draws sprites
        // with — otherwise text would land on whatever framebuffer was
        // last bound, which is rarely what the caller wants. The default
        // 0xFFFF means "fall back to the current view" so legacy callers
        // (Renderer2D's text-on-Transform2D path) keep working unchanged.
        //
        // `scissorCache` is a scissor-cache index re-applied before every
        // submit so multi-atlas runs inside one Mask-clipped UI span all
        // stay clipped. 0xFFFF means "no scissor" (default).
        void RenderInstances(std::span<const TextDrawCmd> commands, const glm::mat4& mvp,
            unsigned short viewId = 0xFFFFu,
            unsigned short scissorCache = 0xFFFFu);

        // Resolve a TextRendererComponent's runtime FontHandle through
        // FontManager, caching into ResolvedFont so subsequent frames
        // skip the lookup. Falls back to the engine default font when
        // the asset is missing — same UX as world-space text.
        static Font* ResolveFont(TextRendererComponent& text);

        // Same as ResolveFont but bakes the atlas at an explicit pixel
        // size — used by GuiRenderer to bake at the on-screen size when
        // the text rides a scaled RectTransform, so a 2× scaled label
        // renders from a 2× atlas (sharp) instead of upscaling a 1×
        // atlas (blurry).
        static Font* ResolveFontAtPixelSize(TextRendererComponent& text, float pixelSize);

        // Measure the text's natural width and height in atlas-pixel units
        // (the same domain as glyph XAdvance). Width = max line width
        // across `\n`-separated lines. Height = font.GetLineHeight() ×
        // line count. Used by UILayoutSystem to auto-size a host
        // RectTransform2D when the TextRendererComponent is set to
        // wrap mode "None" — the rect should hug the text rather than
        // require manual width/height authoring.
        //
        // Caller is responsible for converting the result to screen
        // pixels (multiply by `text.FontSize / font.GetPixelSize()` —
        // same `drawScale` math the renderer uses).
        static Vec2 MeasureNaturalSize(Font& font, std::string_view text, float letterSpacing);

        // Drop GL state. Must run while the GL context is still alive.
        void Shutdown();

        bool IsInitialized() const { return m_IsInitialized; }
        size_t GetGlyphsRenderedLastFrame() const { return m_LastFrameGlyphCount; }
        size_t GetDrawCallsLastFrame() const { return m_LastFrameDrawCalls; }

    private:
        struct GlyphBatchKey {
            unsigned AtlasTexture = 0;
            int16_t SortingOrder = 0;
            uint8_t SortingLayer = 0;
        };

        struct GlyphRun {
            GlyphBatchKey Key{};
            size_t VertexStart = 0;
            size_t VertexCount = 0;
        };

        void EnsureGpuCapacity(size_t requiredBytes);
        void EmitText(Font& font, std::string_view text,
            float worldX, float worldY,
            float scale, const Color& color,
            TextAlignment alignment, float letterSpacing,
            TextWrapMode wrapMode = TextWrapMode::None,
            float wrapWidthPixels = 0.0f,
            float rotation = 0.0f, Vec2 pivot = Vec2{ 0.0f, 0.0f });

        bool m_IsInitialized = false;

        unsigned m_VAO = 0;
        unsigned m_VBO = 0;
        size_t m_VBOCapacity = 0;

        // Reused across frames so we don't allocate in the hot path.
        std::vector<TextVertex> m_Vertices;
        std::vector<GlyphRun> m_Runs;
        // Visual-line ranges produced by EmitText's wrap pass. (begin,
        // end) byte-offsets into the source string; persists across
        // calls so the wrap loop doesn't churn the heap.
        std::vector<std::pair<size_t, size_t>> m_WrapScratch;
        // m_PendingDrawCmds is the per-frame TextDrawCmd buffer used by
        // RenderScene before dispatching through RenderInstances. Held as
        // a member so its capacity persists across frames (no per-frame
        // heap churn). m_Order is the sort-permutation scratch used by
        // RenderInstances; same reuse contract.
        std::vector<TextDrawCmd> m_PendingDrawCmds;
        std::vector<size_t> m_Order;

        std::unique_ptr<Shader> m_Shader;
        int m_uMVP = -1;
        int m_uAtlas = -1;

        size_t m_LastFrameGlyphCount = 0;
        size_t m_LastFrameDrawCalls = 0;
    };

}
