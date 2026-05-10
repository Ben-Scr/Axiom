#include "pch.hpp"
#include "Graphics/Text/TextRenderer.hpp"

#include "Components/General/Transform2DComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Tags.hpp"
#include "Core/Log.hpp"
#include "Graphics/Shader.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Scene/Scene.hpp"
#include "Serialization/Path.hpp"

#include <bgfx/bgfx.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>

// =============================================================================
// TextRenderer — bgfx implementation. Stage 2.4 of the bgfx port.
// -----------------------------------------------------------------------------
// Mirrors TextRenderer.cpp's CPU-side glyph emission verbatim — DecodeUtf8,
// MeasureLineWidth, MeasureNaturalSize, ResolveFontAtPixelSize, EmitText with
// word/character wrapping. The GPU bits change shape:
//
//   * VAO + per-frame glBufferSubData → bgfx::TransientVertexBuffer alloc.
//     Per-frame allocation is fine; bgfx releases it at frame end.
//   * GL_TRIANGLES draw → bgfx::submit per glyph run, one submit per atlas.
//   * GL_BLEND state ops → BGFX_STATE_BLEND_ALPHA + WRITE_RGB|A in setState.
//   * uMVP uniform → bgfx::setViewTransform on the caller's view (sets it
//     once per RenderInstances call). The text view-id matches whatever
//     view is currently bound — TextRenderer is invoked by GuiRenderer
//     which has already configured its UI view-id with the right transform.
//
// Atlas format is R8 (Font_Bgfx::BakeAtlas). The fragment shader pulls the
// red channel as alpha coverage and blends with v_color0.rgb.
// =============================================================================

namespace Axiom::BgfxBackend {
	bgfx::ViewId CurrentViewId();
}

namespace Axiom {
	namespace {
		constexpr size_t k_VerticesPerGlyph = 6;
		constexpr size_t k_InitialVertexCapacity = 1024;

		bool DecodeUtf8(std::string_view s, size_t idx, uint32_t& outCp, int& outLen) {
			if (idx >= s.size()) { outCp = 0; outLen = 0; return false; }
			const unsigned char b0 = static_cast<unsigned char>(s[idx]);
			if (b0 < 0x80) { outCp = b0; outLen = 1; return true; }
			int needed = 0;
			uint32_t cp = 0;
			if      ((b0 & 0xE0) == 0xC0) { needed = 2; cp = b0 & 0x1F; }
			else if ((b0 & 0xF0) == 0xE0) { needed = 3; cp = b0 & 0x0F; }
			else if ((b0 & 0xF8) == 0xF0) { needed = 4; cp = b0 & 0x07; }
			else { outCp = b0; outLen = 1; return true; }
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

		// Per-process bgfx state shared by every TextRenderer instance —
		// ref-counted so the editor's owned TextRenderer + Renderer2D's
		// internal text path don't double-create the program. Allocated
		// on the first Initialize, freed when the last Shutdown lands.
		struct GlobalTextState {
			int                  RefCount  = 0;
			bgfx::ProgramHandle  Program   = BGFX_INVALID_HANDLE;
			bgfx::UniformHandle  Sampler   = BGFX_INVALID_HANDLE;
			bgfx::VertexLayout   VertexLayout;
			bool                 LayoutBuilt = false;
			std::unique_ptr<Shader> ShaderObj;
		};
		GlobalTextState& Globals() {
			static GlobalTextState s;
			return s;
		}

		bool EnsureGlobalState() {
			GlobalTextState& g = Globals();
			if (g.RefCount++ > 0) {
				return bgfx::isValid(g.Program);
			}

			if (!g.LayoutBuilt) {
				g.VertexLayout
					.begin()
					.add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
					.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
					.add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Float)
					.end();
				g.LayoutBuilt = true;
			}

			// Same Shader_Bgfx loader pattern as Renderer2D — the legacy
			// `.vs/.fs` argument names are stripped and the loader resolves
			// to AxiomAssets/Shaders/bgfx/bin/<profile>/{vs,fs}_text.bin.
			g.ShaderObj = std::make_unique<Shader>(
				std::string("AxiomAssets/Shaders/TextShader.vs"),
				std::string("AxiomAssets/Shaders/TextShader.fs"));
			if (!g.ShaderObj || !g.ShaderObj->IsValid()) {
				AIM_CORE_ERROR_TAG("TextRenderer",
					"Text program failed to load (vs_text/fs_text.bin missing?) — text disabled");
				g.ShaderObj.reset();
				return false;
			}
			const uint16_t raw = static_cast<uint16_t>(g.ShaderObj->GetHandle() - 1u);
			g.Program = bgfx::ProgramHandle{ raw };
			g.Sampler = bgfx::createUniform("s_atlas", bgfx::UniformType::Sampler);
			return bgfx::isValid(g.Program);
		}

		void ReleaseGlobalState() {
			GlobalTextState& g = Globals();
			if (g.RefCount == 0) return;
			if (--g.RefCount > 0) return;
			if (bgfx::isValid(g.Sampler)) {
				bgfx::destroy(g.Sampler);
				g.Sampler = BGFX_INVALID_HANDLE;
			}
			g.ShaderObj.reset();
			g.Program = BGFX_INVALID_HANDLE;
		}

		// Decode the (encoded idx + 1) the Font_Bgfx atlas stores back into
		// a bgfx::TextureHandle for setTexture. Mirrors EncodeBgfxIdx /
		// DecodeBgfxIdx in Font_Bgfx.cpp.
		bgfx::TextureHandle AtlasToHandle(unsigned m) {
			if (m == 0) return BGFX_INVALID_HANDLE;
			return bgfx::TextureHandle{ static_cast<uint16_t>(m - 1u) };
		}
	} // namespace

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
		constexpr float k_MaxBakedPixelSize = 192.0f;
		const float requested = pixelSize > 0.0f ? pixelSize : text.FontSize;
		const float bakeRequest = std::min(requested, k_MaxBakedPixelSize);

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
		if (!EnsureGlobalState()) {
			return;
		}
		m_Vertices.reserve(k_InitialVertexCapacity);
		m_Runs.reserve(16);
		m_IsInitialized = true;
		AIM_CORE_INFO_TAG("TextRenderer", "Text renderer initialized (bgfx)");
	}

	void TextRenderer::Shutdown() {
		if (!m_IsInitialized) {
			return;
		}
		m_Vertices.clear();
		m_Vertices.shrink_to_fit();
		m_Runs.clear();
		m_Runs.shrink_to_fit();
		ReleaseGlobalState();
		m_IsInitialized = false;
	}

	void TextRenderer::EnsureGpuCapacity(size_t /*requiredBytes*/) {
		// bgfx transient vertex buffers are allocated per-submit so there
		// isn't a long-lived VBO to grow. The OpenGL impl needed this
		// because its VBO was retained between frames.
	}

	void TextRenderer::EmitText(Font& font, std::string_view text,
		float worldX, float worldY,
		float scale, const Color& color,
		TextAlignment alignment, float letterSpacing,
		TextWrapMode wrapMode, float wrapWidthPixels,
		float rotation, Vec2 pivot) {

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

		const float lineHeight = font.GetLineHeight() * scale;
		const bool autoWrap = wrapMode != TextWrapMode::None && wrapWidthPixels > 0.0f;

		m_WrapScratch.clear();

		auto emitVisualLine = [&](size_t s, size_t e) {
			m_WrapScratch.push_back({ s, e });
		};

		auto wrapSegment = [&](size_t segStart, size_t segEnd) {
			if (!autoWrap || segStart >= segEnd) {
				emitVisualLine(segStart, segEnd);
				return;
			}

			size_t lineStartIdx = segStart;
			size_t lastBreakIdx = std::string_view::npos;
			float widthSinceLineStart = 0.0f;
			uint32_t prev = 0;
			int glyphsOnLine = 0;

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
						emitVisualLine(lineStartIdx, lastBreakIdx);
						lineStartIdx = lastBreakIdx + 1;
						i = lineStartIdx;
						if (i >= segEnd) { lineStartIdx = i; break; }
						widthSinceLineStart = 0.0f;
						lastBreakIdx = std::string_view::npos;
						prev = 0;
						glyphsOnLine = 0;
						continue;
					}
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
				if (glyphsOnLine > 0) {
					penX += letterSpacing * scale;
				}

				if (g->Width > 0.0f && g->Height > 0.0f) {
					const float x0 = penX + g->XOffset * scale;
					const float y0 = baselineY - g->YOffset * scale;
					const float x1 = x0 + g->Width * scale;
					const float y1 = y0 - g->Height * scale;

					auto [tlX, tlY] = rot(x0, y0);
					auto [trX, trY] = rot(x1, y0);
					auto [brX, brY] = rot(x1, y1);
					auto [blX, blY] = rot(x0, y1);

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

	void TextRenderer::RenderInstances(std::span<const TextDrawCmd> commands, const glm::mat4& mvp,
		unsigned short viewId, unsigned short scissorCache) {
		m_LastFrameGlyphCount = 0;
		m_LastFrameDrawCalls = 0;
		if (!m_IsInitialized || commands.empty()) {
			return;
		}
		GlobalTextState& g = Globals();
		if (!bgfx::isValid(g.Program)) {
			return;
		}

		m_Vertices.clear();
		m_Runs.clear();

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

		// Submit one transient vertex buffer per atlas-batched run. We
		// don't use one shared buffer because bgfx::setVertexBuffer takes
		// (buffer, startVertex, numVertices) and the same buffer can in
		// principle drive multiple draws — but allocating per run keeps
		// the lifetime obvious (released at frame end either way) and
		// avoids the cross-run startVertex/numVertices bookkeeping.
		// Caller-supplied view-id wins; sentinel 0xFFFF falls back to the
		// currently bound view (legacy callers that don't yet route through
		// a dedicated UI view).
		const bgfx::ViewId vid = (viewId == 0xFFFFu)
			? BgfxBackend::CurrentViewId()
			: static_cast<bgfx::ViewId>(viewId);
		(void)mvp; // The caller's view transform already routes through u_viewProj.

		for (const GlyphRun& run : m_Runs) {
			const bgfx::TextureHandle atlas = AtlasToHandle(run.Key.AtlasTexture);
			if (!bgfx::isValid(atlas)) continue;

			const uint32_t numVerts = static_cast<uint32_t>(run.VertexCount);
			if (bgfx::getAvailTransientVertexBuffer(numVerts, g.VertexLayout) < numVerts) {
				AIM_CORE_WARN_TAG("TextRenderer",
					"Transient vertex buffer exhausted; dropping {} verts", numVerts);
				continue;
			}
			bgfx::TransientVertexBuffer tvb{};
			bgfx::allocTransientVertexBuffer(&tvb, numVerts, g.VertexLayout);
			std::memcpy(tvb.data,
				m_Vertices.data() + run.VertexStart,
				numVerts * sizeof(TextVertex));

			// Re-apply the cached scissor before each submit; bgfx
			// resets scissor state after submit, so a multi-atlas run
			// inside a single Mask-clipped span would otherwise leak
			// past the clip from the second submit onward. UINT16_MAX
			// disables scissor (default for callers that don't clip).
			bgfx::setScissor(scissorCache);

			bgfx::setVertexBuffer(0, &tvb, 0, numVerts);
			bgfx::setTexture(0, g.Sampler, atlas);
			bgfx::setState(0
				| BGFX_STATE_WRITE_RGB
				| BGFX_STATE_WRITE_A
				| BGFX_STATE_BLEND_ALPHA
				| BGFX_STATE_MSAA);
			bgfx::submit(vid, g.Program);
			++m_LastFrameDrawCalls;
		}

		m_LastFrameGlyphCount = m_Vertices.size() / k_VerticesPerGlyph;
	}

	void TextRenderer::RenderScene(Scene& scene, const glm::mat4& vp, const AABB& viewportAABB) {
		m_LastFrameGlyphCount = 0;
		m_LastFrameDrawCalls = 0;
		if (!m_IsInitialized) {
			return;
		}
		GlobalTextState& g = Globals();
		if (!bgfx::isValid(g.Program)) {
			return;
		}

		m_PendingDrawCmds.clear();

		entt::registry& registry = scene.GetRegistry();
		auto view = registry.view<TextRendererComponent, Transform2DComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, text, tr] : view.each()) {
			if (text.Text.empty()) continue;

			Font* font = ResolveFont(text);
			if (!font || !font->IsLoaded()) continue;

			const AABB approx = AABB::FromTransform(tr);
			if (!AABB::Intersects(viewportAABB, approx)) {
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
			cmd.X = tr.Position.x + text.Margin.x * drawScale;
			cmd.Y = tr.Position.y - text.Margin.y * drawScale;
			cmd.Scale = drawScale;
			cmd.LetterSpacing = text.LetterSpacing;
			cmd.Tint = text.Color;
			cmd.Align = text.HAlign;
			cmd.Wrap = text.WrapMode;
			cmd.WrapWidthPixels = 0.0f;
			cmd.SortingOrder = text.SortingOrder;
			cmd.SortingLayer = text.SortingLayer;
			m_PendingDrawCmds.push_back(cmd);
		}

		RenderInstances(m_PendingDrawCmds, vp);
	}

} // namespace Axiom
