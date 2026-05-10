#include "pch.hpp"
#include "Graphics/Text/Font.hpp"
#include "Serialization/File.hpp"

#include <glad/glad.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <cstring>

namespace Axiom {
    namespace {
        // Codepoint ranges baked into the atlas. ASCII printable
        // (32-126, "space" through "tilde") plus the Latin-1
        // Supplement (160-255, NBSP through "ÿ"). The C1 gap
        // (127-159) is skipped — those are control characters with
        // no printable glyphs, baking them would waste atlas space.
        //
        // This covers most Western-European text (German umlauts,
        // French accents, Spanish ñ/¿/¡, Nordic vowels, etc.) and
        // common typographic punctuation that fits in Latin-1 (©,
        // ®, ±, °, ²/³, ¼/½/¾, etc.). Codepoints above U+00FF still
        // render as the missing-glyph fallback — once SDF / on-demand
        // glyph baking lands, this whole block becomes dynamic.
        struct CodepointRange { uint32_t First; uint32_t Last; };
        constexpr CodepointRange k_BakedRanges[] = {
            { 32,  126 },   // Basic Latin (ASCII printable)
            { 160, 255 },   // Latin-1 Supplement
        };
        constexpr int k_CodepointRangeCount =
            static_cast<int>(sizeof(k_BakedRanges) / sizeof(k_BakedRanges[0]));

        constexpr int ComputeBakedCodepointCount() {
            int total = 0;
            for (const auto& r : k_BakedRanges) {
                total += static_cast<int>(r.Last - r.First + 1);
            }
            return total;
        }
        constexpr int k_CodepointCount = ComputeBakedCodepointCount();

        // Atlas size: square power-of-two large enough for ASCII at
        // typical UI sizes (32-64 px). Larger fonts get a bigger atlas
        // automatically; 4096² is the cap. Beyond that we fall back to
        // 1× oversampling (see the bake loop) before giving up — the
        // alternative is silently substituting the default font, which
        // surprises users who picked a custom font and a large size.
        constexpr int k_AtlasInitialSide = 256;
        constexpr int k_AtlasMaxSide = 4096;
    }

    Font::~Font() {
        Cleanup();
    }

    Font::Font(Font&& other) noexcept
        : m_TtfBuffer(std::move(other.m_TtfBuffer))
        , m_Glyphs(std::move(other.m_Glyphs))
        , m_AtlasTexture(other.m_AtlasTexture)
        , m_AtlasWidth(other.m_AtlasWidth)
        , m_AtlasHeight(other.m_AtlasHeight)
        , m_PixelSize(other.m_PixelSize)
        , m_Ascent(other.m_Ascent)
        , m_Descent(other.m_Descent)
        , m_LineHeight(other.m_LineHeight)
        , m_StbScale(other.m_StbScale)
        , m_StbFontInfoStorage(std::move(other.m_StbFontInfoStorage))
        , m_Filepath(std::move(other.m_Filepath))
    {
        other.m_AtlasTexture = 0;
        other.m_AtlasWidth = 0;
        other.m_AtlasHeight = 0;
        other.m_PixelSize = 0.0f;
        other.m_Ascent = 0.0f;
        other.m_Descent = 0.0f;
        other.m_LineHeight = 0.0f;
        other.m_StbScale = 0.0f;
    }

    Font& Font::operator=(Font&& other) noexcept {
        if (this != &other) {
            Cleanup();

            m_TtfBuffer = std::move(other.m_TtfBuffer);
            m_Glyphs = std::move(other.m_Glyphs);
            m_AtlasTexture = other.m_AtlasTexture;
            m_AtlasWidth = other.m_AtlasWidth;
            m_AtlasHeight = other.m_AtlasHeight;
            m_PixelSize = other.m_PixelSize;
            m_Ascent = other.m_Ascent;
            m_Descent = other.m_Descent;
            m_LineHeight = other.m_LineHeight;
            m_StbScale = other.m_StbScale;
            m_StbFontInfoStorage = std::move(other.m_StbFontInfoStorage);
            m_Filepath = std::move(other.m_Filepath);

            other.m_AtlasTexture = 0;
            other.m_AtlasWidth = 0;
            other.m_AtlasHeight = 0;
            other.m_PixelSize = 0.0f;
            other.m_Ascent = 0.0f;
            other.m_Descent = 0.0f;
            other.m_LineHeight = 0.0f;
            other.m_StbScale = 0.0f;
        }
        return *this;
    }

    bool Font::LoadFromFile(const std::string& path, float pixelSize) {
        if (path.empty() || pixelSize <= 0.0f) {
            return false;
        }

        Cleanup();

        if (!File::Exists(path)) {
            AIM_CORE_ERROR_TAG("Font", "TTF not found: {}", path);
            return false;
        }

        // Read the whole .ttf into memory — stb_truetype keeps a pointer
        // into this buffer for the lifetime of the fontinfo, so we keep
        // it alive as a Font member.
        m_TtfBuffer = File::ReadAllBytes(path);
        if (m_TtfBuffer.empty()) {
            AIM_CORE_ERROR_TAG("Font", "TTF empty / unreadable: {}", path);
            return false;
        }
        m_Filepath = path;
        return BakeAtlas(pixelSize);
    }

    bool Font::LoadFromBuffer(const std::string& sourcePath,
        const std::vector<uint8_t>& ttfBuffer, float pixelSize)
    {
        if (ttfBuffer.empty() || pixelSize <= 0.0f) {
            return false;
        }

        Cleanup();
        // stb_truetype keeps a raw pointer into our buffer for the
        // lifetime of the fontinfo — copy the bytes so the caller's
        // buffer can outlive (or precede) our Font without aliasing
        // worries.
        m_TtfBuffer = ttfBuffer;
        m_Filepath = sourcePath;
        return BakeAtlas(pixelSize);
    }

    bool Font::BakeAtlas(float pixelSize) {
        if (m_TtfBuffer.empty() || pixelSize <= 0.0f) {
            return false;
        }

        // Reserve fontinfo storage. stbtt_fontinfo size is fixed at compile
        // time; reserve the storage as raw bytes to avoid pulling stb's
        // header into Font.hpp.
        m_StbFontInfoStorage.assign(sizeof(stbtt_fontinfo), 0);
        stbtt_fontinfo* font = reinterpret_cast<stbtt_fontinfo*>(m_StbFontInfoStorage.data());

        if (!stbtt_InitFont(font, m_TtfBuffer.data(), stbtt_GetFontOffsetForIndex(m_TtfBuffer.data(), 0))) {
            AIM_CORE_ERROR_TAG("Font", "stbtt_InitFont failed for: {}", m_Filepath);
            m_TtfBuffer.clear();
            m_StbFontInfoStorage.clear();
            return false;
        }

        const float scale = stbtt_ScaleForPixelHeight(font, pixelSize);
        m_StbScale = scale;
        m_PixelSize = pixelSize;

        int ascent = 0;
        int descent = 0;
        int lineGap = 0;
        stbtt_GetFontVMetrics(font, &ascent, &descent, &lineGap);
        m_Ascent = ascent * scale;
        m_Descent = descent * scale;
        m_LineHeight = (ascent - descent + lineGap) * scale;

        // Bake atlas. Two-tier: first try with 2× oversampling (sharp
        // glyphs), then fall back to 1× (4× more glyphs per atlas px²)
        // so a user who picks a large pixel size still gets THEIR font
        // rather than DefaultSans. Each tier walks the atlas-size ladder
        // independently — atlas grows from 256² to k_AtlasMaxSide.
        // packed[] is a flat buffer indexed range-by-range. Per-range
        // slices are passed to stbtt_PackFontRanges via the stbtt_pack_range
        // table below; the post-pack loop walks the same slices to translate
        // results back into m_Glyphs.
        //
        // Above ~48 px the 2× pre-pass essentially always fails: at 2×
        // every glyph rasters at ~96 px, and ~256 ASCII+Latin-1 glyphs at
        // that size don't fit until 4096² atlas (≈ 16 MB R8 upload). The
        // current loop dutifully walks the entire 256→512→1024→2048→4096
        // ladder, fails it, then runs the same ladder again at 1×. Skip
        // the wasted 2× walk when the requested size is large enough that
        // it's known to fail. The `chosenOversample==1` info-log below
        // already advises the user to pick a smaller FontSize for sharper
        // glyphs at these sizes, so the visual outcome is unchanged —
        // we're just not paying for a doomed pre-pass on every drag tick.
        std::vector<stbtt_packedchar> packed(k_CodepointCount);
        std::vector<uint8_t> bitmap;
        int chosenOversample = 0;
        bool ok = false;

        constexpr float k_OversampleSkipThresholdPx = 48.0f;
        const std::initializer_list<int> oversampleTiers = (pixelSize > k_OversampleSkipThresholdPx)
            ? std::initializer_list<int>{ 1 }
            : std::initializer_list<int>{ 2, 1 };
        for (int oversample : oversampleTiers) {
            int side = k_AtlasInitialSide;
            while (side <= k_AtlasMaxSide) {
                bitmap.assign(static_cast<size_t>(side) * static_cast<size_t>(side), 0);

                stbtt_pack_context pctx{};
                if (!stbtt_PackBegin(&pctx, bitmap.data(), side, side, 0, 1, nullptr)) {
                    AIM_CORE_ERROR_TAG("Font", "stbtt_PackBegin failed at side={}", side);
                    break;
                }
                stbtt_PackSetOversampling(&pctx, oversample, oversample);

                // One stbtt_pack_range per logical block (ASCII, Latin-1).
                // chardata_for_range slots are wired into our flat `packed`
                // buffer at the right offset so the post-pack loop can
                // resolve glyph→codepoint without an additional table.
                stbtt_pack_range packRanges[k_CodepointRangeCount]{};
                int cumulative = 0;
                for (int r = 0; r < k_CodepointRangeCount; ++r) {
                    const auto& cr = k_BakedRanges[r];
                    const int count = static_cast<int>(cr.Last - cr.First + 1);
                    packRanges[r].font_size = pixelSize;
                    packRanges[r].first_unicode_codepoint_in_range = static_cast<int>(cr.First);
                    packRanges[r].array_of_unicode_codepoints = nullptr;
                    packRanges[r].num_chars = count;
                    packRanges[r].chardata_for_range = packed.data() + cumulative;
                    cumulative += count;
                }
                const int packResult = stbtt_PackFontRanges(
                    &pctx,
                    m_TtfBuffer.data(),
                    0,
                    packRanges,
                    k_CodepointRangeCount);
                stbtt_PackEnd(&pctx);

                if (packResult) {
                    m_AtlasWidth = side;
                    m_AtlasHeight = side;
                    chosenOversample = oversample;
                    ok = true;
                    break;
                }
                side *= 2;
            }
            if (ok) break;
        }

        if (!ok) {
            AIM_CORE_ERROR_TAG("Font", "Atlas packing failed (font too large for max atlas {}px even at 1× oversample): {}", k_AtlasMaxSide, m_Filepath);
            m_TtfBuffer.clear();
            m_StbFontInfoStorage.clear();
            return false;
        }
        if (chosenOversample == 1) {
            AIM_CORE_INFO_TAG("Font", "{} baked at {} px with 1× oversample (atlas {}²) — pick a smaller FontSize for sharper glyphs",
                m_Filepath, static_cast<int>(pixelSize), m_AtlasWidth);
        }

        // Upload as GL_RED (single-channel). Linear filter keeps glyphs
        // smooth at non-baked sizes; clamp-to-edge avoids bleeding when
        // a glyph sits at the atlas border.
        GLint savedUnpackAlign = 4;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlign);
        glGenTextures(1, &m_AtlasTexture);
        glBindTexture(GL_TEXTURE_2D, m_AtlasTexture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED,
            m_AtlasWidth, m_AtlasHeight, 0,
            GL_RED, GL_UNSIGNED_BYTE, bitmap.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
        glBindTexture(GL_TEXTURE_2D, 0);

        // Translate stb's packed table into our codepoint→GlyphMetrics map.
        // Note on the oversampling-vs-domain mismatch:
        //   pc.x0/x1/y0/y1 are *atlas-pixel* coordinates, which include the
        //     2× oversample factor applied above (so a 32-px glyph occupies
        //     ~64 atlas pixels of width).
        //   pc.xoff/xoff2/yoff/yoff2 and pc.xadvance are in *screen-pixel*
        //     domain (oversample already divided out by stb).
        // We must use xoff2-xoff / yoff2-yoff for the rendered quad width
        // and height — using x1-x0 directly would render glyphs at 2× the
        // intended size and they'd overlap horizontally.
        //
        // Walk the same range table that drove the pack so the flat
        // packed[] indices map back to the right codepoints — without
        // this, Latin-1 characters would alias into ASCII slots.
        int packedIndex = 0;
        for (int r = 0; r < k_CodepointRangeCount; ++r) {
            const auto& cr = k_BakedRanges[r];
            const int count = static_cast<int>(cr.Last - cr.First + 1);
            for (int i = 0; i < count; ++i) {
                const stbtt_packedchar& pc = packed[packedIndex++];
                GlyphMetrics m;
                m.U0 = static_cast<float>(pc.x0) / static_cast<float>(m_AtlasWidth);
                m.V0 = static_cast<float>(pc.y0) / static_cast<float>(m_AtlasHeight);
                m.U1 = static_cast<float>(pc.x1) / static_cast<float>(m_AtlasWidth);
                m.V1 = static_cast<float>(pc.y1) / static_cast<float>(m_AtlasHeight);
                m.Width = pc.xoff2 - pc.xoff;
                m.Height = pc.yoff2 - pc.yoff;
                m.XOffset = pc.xoff;
                m.YOffset = pc.yoff;
                m.XAdvance = pc.xadvance;
                m_Glyphs[cr.First + static_cast<uint32_t>(i)] = m;
            }
        }

        return true;
    }

    const GlyphMetrics* Font::GetGlyph(uint32_t codepoint) const {
        const auto it = m_Glyphs.find(codepoint);
        return it != m_Glyphs.end() ? &it->second : nullptr;
    }

    float Font::GetKerning(uint32_t a, uint32_t b) const {
        if (m_StbFontInfoStorage.empty()) {
            return 0.0f;
        }
        const stbtt_fontinfo* font = reinterpret_cast<const stbtt_fontinfo*>(m_StbFontInfoStorage.data());
        const int kern = stbtt_GetCodepointKernAdvance(font, static_cast<int>(a), static_cast<int>(b));
        return kern * m_StbScale;
    }

    void Font::Cleanup() {
        if (m_AtlasTexture) {
            glDeleteTextures(1, &m_AtlasTexture);
            m_AtlasTexture = 0;
        }
        m_AtlasWidth = 0;
        m_AtlasHeight = 0;
        m_Glyphs.clear();
        m_TtfBuffer.clear();
        m_StbFontInfoStorage.clear();
        m_PixelSize = 0.0f;
        m_Ascent = 0.0f;
        m_Descent = 0.0f;
        m_LineHeight = 0.0f;
        m_StbScale = 0.0f;
        m_Filepath.clear();
    }

}
