#pragma once
#include "Collections/Color.hpp"
#include "Collections/Vec4.hpp"
#include "Core/Export.hpp"
#include "Core/UUID.hpp"
#include "Graphics/Text/FontHandle.hpp"

#include <cstdint>
#include <string>

namespace Axiom {

    enum class TextAlignment : uint8_t {
        Left = 0,
        Center,
        Right
    };

    // Line-breaking strategy applied on top of explicit `\n` breaks.
    // None preserves the historical "single line + explicit \n" layout.
    // Word breaks at the last whitespace before the wrap-width is
    // exceeded; Character breaks at the next glyph that would overflow
    // (used for languages or content where word boundaries don't help).
    enum class TextWrapMode : uint8_t {
        None = 0,
        Word,
        Character
    };

    // Pixels-per-world-unit for text rendering. Glyph metrics from
    // stb_truetype are pixel-domain; world-space is unit-domain, so we
    // divide. 100 mirrors Unity's default and means a 32 px font on a
    // default 1×1 sprite-sized entity stays visually balanced rather
    // than dwarfing the rest of the scene.
    inline constexpr float k_TextPixelsPerWorldUnit = 100.0f;

    // 2D text-rendering component. Mirrors the SpriteRendererComponent
    // shape: anchored at the entity's Transform2D, sortable via
    // Layer/Order, and drawn by the engine's TextRenderer in a pass
    // layered on top of sprites.
    //
    // FontSize is in pixel units (matching how designers think of font
    // sizes); the renderer divides by k_TextPixelsPerWorldUnit on the
    // way to world-space. Use Transform2D.Scale for per-entity scaling
    // on top.
    //
    // Layout for the MVP is single-line plus explicit `\n` line breaks
    // with horizontal alignment; auto-wrap and vertical alignment land
    // alongside the SDF atlas refactor.
    struct AXIOM_API TextRendererComponent {
        std::string Text = "Hello, World!";

        // Font reference is the .ttf's stable asset GUID. The runtime
        // FontHandle is resolved by TextRenderer at draw time so scene
        // serialization stays asset-stable across atlas re-bakes.
        //
        // Defaults to the engine-shipped DefaultSans-Regular.ttf's stable
        // GUID rather than UUID()'s random default — without this, a
        // freshly-added TextRenderer rolls a random 64-bit number that
        // resolves to nothing, and the inspector flags it as "(Missing
        // Asset)" even though the renderer is happily drawing the default
        // fallback font.
        UUID FontAssetId{ k_DefaultFontAssetId };

        // Cached resolved handle — populated by the renderer so the
        // FontManager lookup happens at most once per FontAssetId per
        // frame. Not serialized.
        FontHandle ResolvedFont{};

        // Pixel height the atlas is baked at; also drives display size
        // via the px→world conversion above. Reasonable values: 16-128.
        float FontSize = 32.0f;

        Color Color{ 1.0f, 1.0f, 1.0f, 1.0f };

        // Extra horizontal pixels added between consecutive glyphs (in
        // the same screen-pixel domain as FontSize). Positive values
        // loosen tracking, negative values tighten it. 0 = use the
        // font's natural advances + kerning.
        float LetterSpacing = 0.0f;

        TextAlignment HAlign = TextAlignment::Left;

        // Line-wrap strategy. None == legacy single-line + explicit `\n`
        // behaviour; Word / Character break overlong runs at the rect's
        // wrap width. UI text on a RectTransform2D wraps inside the
        // rect's width minus the Margin (so authored insets actually
        // narrow the wrap area). World-space text (no ambient rect)
        // doesn't wrap — explicit `\n` is the only line-break source
        // there. The historical `WrapWidth` override field was removed
        // because Margin already provides the inset and the dual-path
        // (explicit width vs rect-derived) made the wrap area
        // non-obvious in the inspector.
        TextWrapMode WrapMode = TextWrapMode::None;

        // Insets the rendered text inside the host RectTransform2D. Each
        // component is in screen-pixel units (same domain as FontSize):
        //   .x = left   inset (text origin shifts right)
        //   .y = top    inset (text baseline shifts down)
        //   .z = right  inset (wrap width / right-align edge shifts in)
        //   .w = bottom inset (reserved for future vertical-align work)
        //
        // The wrap width — derived from the rect's width — is reduced
        // by `Margin.x + Margin.z` so wrapped lines stop short of the
        // right margin. Visualised in the Editor View as an inner
        // rectangle with four edge handles the user can drag.
        //
        // World-space text (no host RectTransform2D) only honours the
        // left + top components today; right / bottom rely on the rect's
        // edges which world-space text doesn't have.
        Vec4 Margin{ 0.0f, 0.0f, 0.0f, 0.0f };

        // Sort batch — same semantics as SpriteRendererComponent. Text
        // batches independently from sprites today; the sort key only
        // matters within the text batch.
        int16_t SortingOrder = 0;
        uint8_t SortingLayer = 0;
    };

}
