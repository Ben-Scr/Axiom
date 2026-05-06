#pragma once
#include "Core/Export.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Axiom {

    // Per-glyph metrics + UV rect into the font's atlas texture.
    // All measurements are in pixels at the font's baked size — callers
    // scale them via TextRendererComponent::FontSize / Font::GetPixelSize().
    struct GlyphMetrics {
        // UV rect inside the atlas (normalized 0..1).
        float U0 = 0.0f;
        float V0 = 0.0f;
        float U1 = 0.0f;
        float V1 = 0.0f;

        // Pixel-space size and offset of the glyph quad relative to the
        // pen position (cursor advancing along the baseline). Y is in
        // top-down screen orientation: yOffset is positive for descent
        // below the baseline.
        float Width = 0.0f;
        float Height = 0.0f;
        float XOffset = 0.0f;
        float YOffset = 0.0f;

        // Horizontal advance to apply to the pen after this glyph.
        float XAdvance = 0.0f;
    };

    // Font owns a TTF blob, an atlas texture (R8 alpha), and a glyph
    // table. Atlas baking happens at LoadFromFile time for the given
    // pixel size — different pixel sizes need different Font instances
    // (or, future: a multi-size SDF atlas).
    class AXIOM_API Font {
    public:
        Font() = default;
        ~Font();

        Font(const Font&) = delete;
        Font& operator=(const Font&) = delete;
        Font(Font&&) noexcept;
        Font& operator=(Font&&) noexcept;

        // Load the .ttf at `path`, bake an ASCII (32..126) atlas at the
        // requested pixel height, upload to GPU. Returns false on any
        // failure; on failure no GL state is left allocated.
        bool LoadFromFile(const std::string& path, float pixelSize);

        bool IsLoaded() const { return m_AtlasTexture != 0; }

        // Returns nullptr if the codepoint isn't in the baked range.
        const GlyphMetrics* GetGlyph(uint32_t codepoint) const;

        // Vertical metrics in pixels, scaled to the baked size.
        float GetAscent() const { return m_Ascent; }
        float GetDescent() const { return m_Descent; }
        float GetLineHeight() const { return m_LineHeight; }
        float GetPixelSize() const { return m_PixelSize; }

        // GL handles / dimensions.
        unsigned GetAtlasTexture() const { return m_AtlasTexture; }
        int GetAtlasWidth() const { return m_AtlasWidth; }
        int GetAtlasHeight() const { return m_AtlasHeight; }

        const std::string& GetFilepath() const { return m_Filepath; }

        // Optional kerning between two adjacent codepoints, in pixels.
        // Returns 0 when no kerning data is available for the pair.
        float GetKerning(uint32_t a, uint32_t b) const;

    private:
        void Cleanup();

        std::vector<uint8_t> m_TtfBuffer;
        std::unordered_map<uint32_t, GlyphMetrics> m_Glyphs;

        unsigned m_AtlasTexture = 0;
        int m_AtlasWidth = 0;
        int m_AtlasHeight = 0;

        float m_PixelSize = 0.0f;
        float m_Ascent = 0.0f;
        float m_Descent = 0.0f;
        float m_LineHeight = 0.0f;

        // Cached scale factor and font info bytes for kerning lookups.
        // Stored as a void* / float pair so stb_truetype's `stbtt_fontinfo`
        // doesn't leak into Font.hpp's public surface.
        float m_StbScale = 0.0f;
        std::vector<unsigned char> m_StbFontInfoStorage;

        std::string m_Filepath;
    };

}
