#include "pch.hpp"
#include "Graphics/Text/TextRenderer.hpp"

#include "Components/General/Transform2DComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Tags.hpp"
#include "Graphics/Shader.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Scene/Scene.hpp"
#include "Serialization/Path.hpp"

#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstddef>

namespace Axiom {
    namespace {
        constexpr size_t k_VerticesPerGlyph = 6;
        constexpr size_t k_InitialVertexCapacity = 1024;

        // Decode the next UTF-8 codepoint starting at byte `idx` in `s`.
        // Writes the decoded codepoint to `outCp` and the consumed byte
        // count (1-4) to `outLen`. On a malformed lead/continuation byte
        // we yield the raw byte as a Latin-1 codepoint with len=1 — that
        // keeps callers making forward progress and ensures a stray
        // 0xC3 (the German "ä" lead byte without its continuation) at
        // least renders something instead of stalling the loop.
        // Returns false past the end of the string.
        bool DecodeUtf8(std::string_view s, size_t idx, uint32_t& outCp, int& outLen) {
            if (idx >= s.size()) { outCp = 0; outLen = 0; return false; }
            const unsigned char b0 = static_cast<unsigned char>(s[idx]);
            if (b0 < 0x80) { outCp = b0; outLen = 1; return true; }
            int needed = 0;
            uint32_t cp = 0;
            if      ((b0 & 0xE0) == 0xC0) { needed = 2; cp = b0 & 0x1F; }
            else if ((b0 & 0xF0) == 0xE0) { needed = 3; cp = b0 & 0x0F; }
            else if ((b0 & 0xF8) == 0xF0) { needed = 4; cp = b0 & 0x07; }
            else { outCp = b0; outLen = 1; return true; } // bad lead byte
            if (idx + needed > s.size()) { outCp = b0; outLen = 1; return true; }
            for (int i = 1; i < needed; ++i) {
                const unsigned char b = static_cast<unsigned char>(s[idx + i]);
                if ((b & 0xC0) != 0x80) { outCp = b0; outLen = 1; return true; }
                cp = (cp << 6) | (b & 0x3F);
            }
            outCp = cp;
            outLen = needed;
            return true;
        }

        // Pixel-space width of a single line at the font's baked size.
        // Used for left/center/right alignment offset. `letterSpacing` is
        // the same screen-pixel-domain value we add per advance step in
        // EmitText so the alignment math stays consistent with what's
        // actually drawn.
        float MeasureLineWidth(const Font& font, std::string_view line, float letterSpacing) {
            float width = 0.0f;
            uint32_t prev = 0;
            int glyphCount = 0;
            size_t i = 0;
            while (i < line.size()) {
                uint32_t cp = 0;
                int len = 0;
                if (!DecodeUtf8(line, i, cp, len)) break;
                i += static_cast<size_t>(len);

                const GlyphMetrics* g = font.GetGlyph(cp);
                if (!g) {
                    prev = 0;
                    continue;
                }
                if (prev != 0) {
                    width += font.GetKerning(prev, cp);
                }
                width += g->XAdvance;
                if (glyphCount > 0) {
                    width += letterSpacing;
                }
                ++glyphCount;
                prev = cp;
            }
            return width;
        }
    }

    Vec2 TextRenderer::MeasureNaturalSize(Font& font, std::string_view text, float letterSpacing) {
        if (text.empty()) {
            return Vec2{ 0.0f, font.GetLineHeight() };
        }

        float maxLineWidth = 0.0f;
        int lineCount = 0;
        size_t lineStart = 0;
        const size_t textSize = text.size();
        while (lineStart <= textSize) {
            size_t lineEnd = text.find('\n', lineStart);
            if (lineEnd == std::string_view::npos) {
                lineEnd = textSize;
            }
            std::string_view line(text.data() + lineStart, lineEnd - lineStart);
            const float w = MeasureLineWidth(font, line, letterSpacing);
            if (w > maxLineWidth) maxLineWidth = w;
            ++lineCount;
            if (lineEnd == textSize) break;
            lineStart = lineEnd + 1;
        }

        return Vec2{ maxLineWidth, font.GetLineHeight() * static_cast<float>(lineCount) };
    }

    Font* TextRenderer::ResolveFont(TextRendererComponent& text) {
        return ResolveFontAtPixelSize(text, text.FontSize);
    }

    Font* TextRenderer::ResolveFontAtPixelSize(TextRendererComponent& text, float pixelSize) {
        // The cached ResolvedFont handle is keyed on a single pixel size,
        // but UI text drawn under a scaled RectTransform wants the atlas
        // baked at the on-screen size (FontSize × parent scale) for sharp
        // glyphs — same final raster size as bumping FontSize directly.
        // FontManager::LoadFontByUUID caches per (assetId, quantized px),
        // so a scaled-up button shares an atlas with the equivalently-
        // sized non-scaled label. We refresh ResolvedFont when the
        // requested pixelSize doesn't match what's currently cached so
        // an animated scale doesn't keep re-rendering through a stale
        // smaller atlas.
        //
        // Bake size is capped — even with the 4096² atlas + 1× oversample
        // fallback, a 95-glyph ASCII range stops fitting somewhere around
        // 256–300 px. Above the cap we bake at the cap and let the
        // renderer's `drawScale = requested / baked` upscale via the
        // GL_LINEAR filter. The alternative is silently substituting
        // DefaultSans for the user's chosen font, which is the bug this
        // cap exists to prevent.
        constexpr float k_MaxBakedPixelSize = 192.0f;
        const float requested = pixelSize > 0.0f ? pixelSize : text.FontSize;
        const float bakeRequest = std::min(requested, k_MaxBakedPixelSize);

        // Cache match uses the same quantize ladder FontManager keys on
        // (1 / 2 / 4 / 8 / 16 px steps as size grows). Without this, a
        // rect animating between scale 1.0 and 1.04 — or two text rows
        // with FontSize 14 and 15 — would each see lround mismatch and
        // force a fresh LoadFontByUUID call every frame, even though
        // FontManager's quantize bucket is identical and the lookup is
        // returning the SAME atlas. Bucket-equality skips the lookup.
        auto quantizeBucket = [](float p) -> int {
            int v = std::max(1, static_cast<int>(std::lround(p)));
            auto snap = [](int value, int step) {
                return ((value + step / 2) / step) * step;
            };
            if (v <= 16)  return v;
            if (v <= 32)  return snap(v, 2);
            if (v <= 64)  return snap(v, 4);
            if (v <= 128) return snap(v, 8);
            return snap(v, 16);
        };

        if (FontManager::IsValid(text.ResolvedFont)) {
            if (Font* font = FontManager::GetFont(text.ResolvedFont)) {
                if (quantizeBucket(font->GetPixelSize()) == quantizeBucket(bakeRequest)) {
                    return font;
                }
            }
        }

        const uint64_t uuid = static_cast<uint64_t>(text.FontAssetId);
        if (uuid != 0) {
            text.ResolvedFont = FontManager::LoadFontByUUID(uuid, bakeRequest);
            if (Font* f = FontManager::GetFont(text.ResolvedFont)) {
                return f;
            }
        }

        text.ResolvedFont = FontManager::GetDefaultFont();
        Font* fallback = FontManager::GetFont(text.ResolvedFont);
        if (!fallback) {
            // One-shot: surface the failure in the log instead of silently
            // rendering blank text. Users who see no text would otherwise
            // have no idea whether the renderer is broken or just couldn't
            // find a font asset to draw with.
            static bool s_LoggedMissingFont = false;
            if (!s_LoggedMissingFont) {
                s_LoggedMissingFont = true;
                AIM_CORE_WARN_TAG("TextRenderer",
                    "No font available - assign one in the inspector or ensure AxiomAssets/Fonts/DefaultSans-Regular.ttf is shipped next to the executable.");
            }
        }
        return fallback;
    }

    TextRenderer::TextRenderer() = default;
    TextRenderer::~TextRenderer() {
        Shutdown();
    }

    void TextRenderer::Initialize() {
        if (m_IsInitialized) {
            return;
        }

        std::string shaderDir = Path::ResolveAxiomAssets("Shader");
        if (shaderDir.empty()) {
            shaderDir = Path::Combine(Path::ExecutableDir(), "AxiomAssets", "Shader");
        }

        m_Shader = std::make_unique<Shader>(
            Path::Combine(shaderDir, "2D/text.vert.glsl"),
            Path::Combine(shaderDir, "2D/text.frag.glsl"));
        if (!m_Shader->IsValid()) {
            AIM_CORE_ERROR_TAG("TextRenderer", "Text shader failed to compile — text rendering disabled");
            m_Shader.reset();
            return;
        }

        m_Shader->Submit();
        const GLuint program = m_Shader->GetHandle();
        m_uMVP = glGetUniformLocation(program, "uMVP");
        m_uAtlas = glGetUniformLocation(program, "uAtlas");
        if (m_uAtlas >= 0) {
            glUniform1i(m_uAtlas, 0);
        }
        glUseProgram(0);

        glGenVertexArrays(1, &m_VAO);
        glGenBuffers(1, &m_VBO);

        glBindVertexArray(m_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
        m_VBOCapacity = sizeof(TextVertex) * k_InitialVertexCapacity;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_VBOCapacity), nullptr, GL_DYNAMIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(TextVertex), reinterpret_cast<void*>(offsetof(TextVertex, X)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(TextVertex), reinterpret_cast<void*>(offsetof(TextVertex, U)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(TextVertex), reinterpret_cast<void*>(offsetof(TextVertex, R)));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        m_Vertices.reserve(k_InitialVertexCapacity);
        m_Runs.reserve(16);

        m_IsInitialized = true;
        AIM_CORE_INFO_TAG("TextRenderer", "Text renderer initialized");
    }

    void TextRenderer::Shutdown() {
        if (m_VBO) {
            glDeleteBuffers(1, &m_VBO);
            m_VBO = 0;
        }
        if (m_VAO) {
            glDeleteVertexArrays(1, &m_VAO);
            m_VAO = 0;
        }
        m_VBOCapacity = 0;
        m_Vertices.clear();
        m_Vertices.shrink_to_fit();
        m_Runs.clear();
        m_Runs.shrink_to_fit();
        m_Shader.reset();
        m_uMVP = -1;
        m_uAtlas = -1;
        m_IsInitialized = false;
    }

    void TextRenderer::EnsureGpuCapacity(size_t requiredBytes) {
        if (requiredBytes <= m_VBOCapacity) {
            return;
        }
        size_t newCapacity = m_VBOCapacity == 0 ? sizeof(TextVertex) * k_InitialVertexCapacity : m_VBOCapacity;
        while (newCapacity < requiredBytes) {
            newCapacity *= 2;
        }
        glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(newCapacity), nullptr, GL_DYNAMIC_DRAW);
        m_VBOCapacity = newCapacity;
    }

    void TextRenderer::EmitText(Font& font, std::string_view text,
        float worldX, float worldY,
        float scale, const Color& color,
        TextAlignment alignment, float letterSpacing,
        TextWrapMode wrapMode, float wrapWidthPixels,
        float rotation, Vec2 pivot) {

        // Optional rotation around `pivot` (world coords). cos/sin are
        // computed once per call; the per-vertex branch checks
        // applyRotation so the unrotated path stays a plain copy. Used
        // by UI text under a rotated RectTransform — without it,
        // labels stayed visually upright while their parent rect
        // rotated, producing the "100% sits horizontal under a tilted
        // progress bar" symptom.
        const bool applyRotation = rotation != 0.0f;
        const float rotC = applyRotation ? std::cos(rotation) : 1.0f;
        const float rotS = applyRotation ? std::sin(rotation) : 0.0f;
        auto rot = [&](float x, float y) -> std::pair<float, float> {
            if (!applyRotation) return { x, y };
            const float dx = x - pivot.x;
            const float dy = y - pivot.y;
            return { pivot.x + rotC * dx - rotS * dy,
                     pivot.y + rotS * dx + rotC * dy };
        };

        // Iterate per line so explicit `\n` produces wrapping. Auto-wrap
        // sub-divides each `\n` segment into one or more "visual lines"
        // before alignment/emit, so all the existing alignment math
        // continues to work line-by-line.
        const float lineHeight = font.GetLineHeight() * scale;

        // Wrap-width is supplied in screen-pixel units (the same domain
        // as font advances); divide by `scale` once so the wrap check
        // stays in pixel-domain when measuring against MeasureLineWidth.
        const bool autoWrap =
            wrapMode != TextWrapMode::None && wrapWidthPixels > 0.0f;

        // Stash visual lines as (start, end) offsets into `text` so we
        // can run the existing per-line alignment + emit loop unchanged.
        // m_WrapScratch is a member to avoid per-call heap churn — it's
        // a small std::vector that stays warm across frames.
        m_WrapScratch.clear();

        auto emitVisualLine = [&](size_t s, size_t e) {
            m_WrapScratch.push_back({ s, e });
        };

        // Wrap a single hard-break segment into one or more visual lines.
        // Word mode breaks at the last whitespace before the wrap-width;
        // Character mode breaks at the next overflow glyph. If a single
        // word doesn't fit, both modes fall back to character splits so
        // unbreakable runs still render (they just overflow visually).
        auto wrapSegment = [&](size_t segStart, size_t segEnd) {
            if (!autoWrap || segStart >= segEnd) {
                emitVisualLine(segStart, segEnd);
                return;
            }

            size_t lineStartIdx = segStart;
            size_t lastBreakIdx = std::string_view::npos; // last whitespace candidate
            float widthSinceLineStart = 0.0f;
            uint32_t prev = 0;
            int glyphsOnLine = 0;

            // Manual byte index because UTF-8 codepoints are 1-4 bytes;
            // a for-loop with ++i would step into the middle of a
            // multi-byte sequence. `glyphStart` captures where the
            // current codepoint begins so wraps split on glyph
            // boundaries, never inside a sequence.
            size_t i = segStart;
            while (i < segEnd) {
                const size_t glyphStart = i;
                uint32_t cp = 0;
                int len = 0;
                if (!DecodeUtf8(text, i, cp, len)) break;
                const size_t nextI = i + static_cast<size_t>(len);
                i = nextI;

                const GlyphMetrics* g = font.GetGlyph(cp);
                if (!g) { prev = 0; continue; }

                float advance = g->XAdvance;
                if (prev != 0) advance += font.GetKerning(prev, cp);
                if (glyphsOnLine > 0) advance += letterSpacing;

                const float candidate = widthSinceLineStart + advance;

                if (candidate > wrapWidthPixels && glyphsOnLine > 0) {
                    if (wrapMode == TextWrapMode::Word
                        && lastBreakIdx != std::string_view::npos
                        && lastBreakIdx > lineStartIdx)
                    {
                        // Break at the last whitespace; consume the
                        // whitespace itself (skip it on the next line).
                        emitVisualLine(lineStartIdx, lastBreakIdx);
                        lineStartIdx = lastBreakIdx + 1;
                        // Re-measure leftover after the break — could
                        // be empty, could still overflow if the next
                        // word alone is too wide. Restart at lineStart
                        // and re-decode from there.
                        i = lineStartIdx;
                        if (i >= segEnd) { lineStartIdx = i; break; }
                        widthSinceLineStart = 0.0f;
                        lastBreakIdx = std::string_view::npos;
                        prev = 0;
                        glyphsOnLine = 0;
                        continue;
                    }
                    // Character wrap, or word-wrap with no prior break:
                    // break right before this glyph. Rewind to
                    // glyphStart so the new line re-decodes it.
                    emitVisualLine(lineStartIdx, glyphStart);
                    lineStartIdx = glyphStart;
                    i = glyphStart;
                    widthSinceLineStart = 0.0f;
                    lastBreakIdx = std::string_view::npos;
                    prev = 0;
                    glyphsOnLine = 0;
                    continue;
                }

                widthSinceLineStart = candidate;
                if (cp == ' ' || cp == '\t') {
                    lastBreakIdx = glyphStart;
                }
                prev = cp;
                ++glyphsOnLine;
            }

            if (lineStartIdx < segEnd) {
                emitVisualLine(lineStartIdx, segEnd);
            }
        };

        size_t segStart = 0;
        const size_t textSize = text.size();
        while (segStart <= textSize) {
            size_t segEnd = text.find('\n', segStart);
            if (segEnd == std::string_view::npos) {
                segEnd = textSize;
            }
            wrapSegment(segStart, segEnd);
            if (segEnd == textSize) break;
            segStart = segEnd + 1;
        }

        for (size_t lineIndex = 0; lineIndex < m_WrapScratch.size(); ++lineIndex) {
            const auto [lineBegin, lineEnd] = m_WrapScratch[lineIndex];
            std::string_view line(text.data() + lineBegin, lineEnd - lineBegin);
            const float lineWidth = MeasureLineWidth(font, line, letterSpacing) * scale;

            float alignOffset = 0.0f;
            switch (alignment) {
            case TextAlignment::Center: alignOffset = -lineWidth * 0.5f; break;
            case TextAlignment::Right:  alignOffset = -lineWidth; break;
            case TextAlignment::Left:
            default:                    alignOffset = 0.0f; break;
            }

            // Pen starts at the entity's transform with the requested
            // alignment offset on X. Y advances downward by line index;
            // baseline math is handled inside the per-glyph emit.
            float penX = worldX + alignOffset;
            const float baselineY = worldY - static_cast<float>(lineIndex) * lineHeight;

            uint32_t prev = 0;
            int glyphsOnLine = 0;
            size_t i = 0;
            while (i < line.size()) {
                uint32_t cp = 0;
                int len = 0;
                if (!DecodeUtf8(line, i, cp, len)) break;
                i += static_cast<size_t>(len);

                const GlyphMetrics* g = font.GetGlyph(cp);
                if (!g) {
                    prev = 0;
                    continue;
                }
                if (prev != 0) {
                    penX += font.GetKerning(prev, cp) * scale;
                }
                // Letter spacing applies *between* glyphs only (after the
                // first), so leading/trailing whitespace stays intact and
                // alignment math (which uses the same rule) lines up.
                if (glyphsOnLine > 0) {
                    penX += letterSpacing * scale;
                }

                if (g->Width > 0.0f && g->Height > 0.0f) {
                    // Glyph quad in world space. stb's yoff is positive
                    // *downward* from the baseline (screen-Y-down); we flip
                    // by subtracting so y0 = top of glyph (higher world Y)
                    // and y1 = bottom (lower world Y).
                    const float x0 = penX + g->XOffset * scale;
                    const float y0 = baselineY - g->YOffset * scale;
                    const float x1 = x0 + g->Width * scale;
                    const float y1 = y0 - g->Height * scale;

                    auto [tlX, tlY] = rot(x0, y0);
                    auto [trX, trY] = rot(x1, y0);
                    auto [brX, brY] = rot(x1, y1);
                    auto [blX, blY] = rot(x0, y1);

                    // Two triangles, wound CCW from the camera's view so
                    // the engine-wide GL_BACK cull doesn't drop them.
                    // Sprites use the same BL → BR → TR convention.
                    TextVertex vTL{ tlX, tlY, g->U0, g->V0, color.r, color.g, color.b, color.a };
                    TextVertex vTR{ trX, trY, g->U1, g->V0, color.r, color.g, color.b, color.a };
                    TextVertex vBR{ brX, brY, g->U1, g->V1, color.r, color.g, color.b, color.a };
                    TextVertex vBL{ blX, blY, g->U0, g->V1, color.r, color.g, color.b, color.a };

                    m_Vertices.push_back(vBL);
                    m_Vertices.push_back(vBR);
                    m_Vertices.push_back(vTR);
                    m_Vertices.push_back(vBL);
                    m_Vertices.push_back(vTR);
                    m_Vertices.push_back(vTL);
                }

                penX += g->XAdvance * scale;
                ++glyphsOnLine;
                prev = cp;
            }
        }
    }

    void TextRenderer::RenderInstances(std::span<const TextDrawCmd> commands, const glm::mat4& mvp) {
        m_LastFrameGlyphCount = 0;
        m_LastFrameDrawCalls = 0;
        if (!m_IsInitialized || !m_Shader || !m_Shader->IsValid() || commands.empty()) {
            return;
        }

        m_Vertices.clear();
        m_Runs.clear();

        // Sort indices into commands rather than the input span (caller
        // owns it and may need it intact for later use). m_Order is a
        // member so its capacity persists across frames — clear() keeps it.
        m_Order.clear();
        m_Order.reserve(commands.size());
        for (size_t i = 0; i < commands.size(); ++i) {
            if (commands[i].FontPtr && !commands[i].Text.empty()) {
                m_Order.push_back(i);
            }
        }
        std::sort(m_Order.begin(), m_Order.end(), [&](size_t a, size_t b) {
            const auto& ca = commands[a];
            const auto& cb = commands[b];
            if (ca.SortingLayer != cb.SortingLayer) return ca.SortingLayer < cb.SortingLayer;
            if (ca.SortingOrder != cb.SortingOrder) return ca.SortingOrder < cb.SortingOrder;
            return ca.FontPtr < cb.FontPtr;
        });

        for (size_t i = 0; i < m_Order.size(); ) {
            const TextDrawCmd& head = commands[m_Order[i]];
            GlyphRun run;
            run.Key.AtlasTexture = head.FontPtr->GetAtlasTexture();
            run.Key.SortingOrder = head.SortingOrder;
            run.Key.SortingLayer = head.SortingLayer;
            run.VertexStart = m_Vertices.size();

            size_t j = i;
            while (j < m_Order.size()) {
                const TextDrawCmd& cmd = commands[m_Order[j]];
                if (cmd.FontPtr->GetAtlasTexture() != run.Key.AtlasTexture
                    || cmd.SortingOrder != run.Key.SortingOrder
                    || cmd.SortingLayer != run.Key.SortingLayer)
                {
                    break;
                }
                // EmitText takes string_view directly — no per-glyph-run
                // std::string allocation.
                EmitText(*cmd.FontPtr, cmd.Text, cmd.X, cmd.Y, cmd.Scale,
                    cmd.Tint, cmd.Align, cmd.LetterSpacing,
                    cmd.Wrap, cmd.WrapWidthPixels,
                    cmd.Rotation, cmd.Pivot);
                ++j;
            }

            run.VertexCount = m_Vertices.size() - run.VertexStart;
            if (run.VertexCount > 0) {
                m_Runs.push_back(run);
            }
            i = j;
        }

        if (m_Vertices.empty()) {
            return;
        }

        m_Shader->Submit();
        if (m_uMVP >= 0) {
            glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, glm::value_ptr(mvp));
        }

        glBindVertexArray(m_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_VBO);

        const size_t requiredBytes = m_Vertices.size() * sizeof(TextVertex);
        EnsureGpuCapacity(requiredBytes);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(requiredBytes), m_Vertices.data());

        GLboolean prevBlend = glIsEnabled(GL_BLEND);
        GLint prevBlendSrcRgb = GL_ONE;
        GLint prevBlendDstRgb = GL_ZERO;
        GLint prevBlendSrcAlpha = GL_ONE;
        GLint prevBlendDstAlpha = GL_ZERO;
        GLint prevBlendEquationRgb = GL_FUNC_ADD;
        GLint prevBlendEquationAlpha = GL_FUNC_ADD;
        glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevBlendEquationRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevBlendEquationAlpha);
        if (!prevBlend) glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Text always rides on top of opaque sprites; disable depth so
        // multi-line text doesn't z-fight with itself when the projection
        // squashes glyphs onto the same plane.
        const GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
        if (prevDepth) glDisable(GL_DEPTH_TEST);

        glActiveTexture(GL_TEXTURE0);
        for (const GlyphRun& run : m_Runs) {
            glBindTexture(GL_TEXTURE_2D, run.Key.AtlasTexture);
            glDrawArrays(GL_TRIANGLES,
                static_cast<GLint>(run.VertexStart),
                static_cast<GLsizei>(run.VertexCount));
            ++m_LastFrameDrawCalls;
        }

        glBlendEquationSeparate(prevBlendEquationRgb, prevBlendEquationAlpha);
        glBlendFuncSeparate(prevBlendSrcRgb, prevBlendDstRgb, prevBlendSrcAlpha, prevBlendDstAlpha);
        if (!prevBlend) glDisable(GL_BLEND);
        if (prevDepth) glEnable(GL_DEPTH_TEST);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindVertexArray(0);
        glUseProgram(0);

        m_LastFrameGlyphCount = m_Vertices.size() / k_VerticesPerGlyph;
    }

    void TextRenderer::RenderScene(Scene& scene, const glm::mat4& vp, const AABB& viewportAABB) {
        m_LastFrameGlyphCount = 0;
        m_LastFrameDrawCalls = 0;
        if (!m_IsInitialized || !m_Shader || !m_Shader->IsValid()) {
            return;
        }

        // Reuse m_PendingDrawCmds's capacity across frames — the per-frame
        // text-list size is roughly stable, and clear() keeps capacity intact
        // so we avoid the per-frame heap churn of a fresh local vector. The
        // entries are populated directly as TextDrawCmd so we can hand the
        // member to RenderInstances without an intermediate copy.
        m_PendingDrawCmds.clear();

        // Walk all visible TextRendererComponents on entities with a
        // world-space Transform2D (separate from UI text, which lives on
        // RectTransform2D and is rendered by UIRenderer).
        entt::registry& registry = scene.GetRegistry();

        auto view = registry.view<TextRendererComponent, Transform2DComponent>(entt::exclude<DisabledTag>);
        for (auto&& [entity, text, tr] : view.each()) {
            if (text.Text.empty()) continue;

            Font* font = ResolveFont(text);
            if (!font || !font->IsLoaded()) continue;

            // Cull cheaply via a generous AABB around the transform.
            // Tighter bounds (per-line measure) is a future optimization;
            // for typical text sizes this is plenty.
            const AABB approx = AABB::FromTransform(tr);
            if (!AABB::Intersects(viewportAABB, approx)) {
                // Fall back to bigger probe — text may extend beyond
                // the transform's scale-derived AABB.
                const float radius = text.FontSize * static_cast<float>(text.Text.size()) * 0.5f;
                AABB textBounds{
                    { tr.Position.x - radius, tr.Position.y - radius },
                    { tr.Position.x + radius, tr.Position.y + radius }
                };
                if (!AABB::Intersects(viewportAABB, textBounds)) continue;
            }

            const float bakedSize = font->GetPixelSize() > 0.0f ? font->GetPixelSize() : text.FontSize;
            const float drawScale = (text.FontSize / bakedSize)
                * tr.Scale.x
                / k_TextPixelsPerWorldUnit;

            TextDrawCmd cmd;
            cmd.FontPtr = font;
            cmd.Text = std::string_view(text.Text);
            // World-space text has no host rect, so right / bottom margins
            // would have nothing to inset against. Left / top still apply —
            // they shift the text origin by the authored amount (in pixel
            // units, scaled by drawScale to land in world units).
            cmd.X = tr.Position.x + text.Margin.x * drawScale;
            cmd.Y = tr.Position.y - text.Margin.y * drawScale;
            cmd.Scale = drawScale;
            cmd.LetterSpacing = text.LetterSpacing;
            cmd.Tint = text.Color;
            cmd.Align = text.HAlign;
            cmd.Wrap = text.WrapMode;
            // World-space text has no ambient rect, so wrapping is
            // disabled here (no source for the wrap width). Use UI
            // text on a RectTransform2D for wrapped layouts; world-
            // space text honours explicit `\n` line breaks but won't
            // auto-wrap. The historical explicit WrapWidth override
            // was removed alongside the rest of the field.
            cmd.WrapWidthPixels = 0.0f;
            cmd.SortingOrder = text.SortingOrder;
            cmd.SortingLayer = text.SortingLayer;
            m_PendingDrawCmds.push_back(cmd);
        }

        RenderInstances(m_PendingDrawCmds, vp);
    }

}
