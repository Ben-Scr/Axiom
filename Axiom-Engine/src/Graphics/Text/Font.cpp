#include "pch.hpp"
#include "Graphics/Text/Font.hpp"

#include "Core/Log.hpp"
#include "Serialization/File.hpp"

#include <bgfx/bgfx.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <cstring>
#include <utility>

// =============================================================================
// Font — real bgfx implementation. Stage 2.1 of the bgfx port.
// -----------------------------------------------------------------------------
// Mirrors the OpenGL Font.cpp's stb_truetype baking logic exactly, with the
// atlas upload routed through `bgfx::createTexture2D` (R8 format) instead of
// `glTexImage2D`. The two implementations could share the bake step in a
// later cleanup pass — for Stage 2.1 the duplication is intentional so each
// backend stays a single coherent file.
//
// `m_AtlasTexture` stores (bgfx handle.idx + 1) with 0 = invalid sentinel,
// matching the Texture2D_Bgfx convention. The existing `IsLoaded()` check
// (`m_AtlasTexture != 0`) keeps working for both backends.
// =============================================================================

namespace Axiom {

	namespace {
		struct CodepointRange { uint32_t First; uint32_t Last; };
		constexpr CodepointRange k_BakedRanges[] = {
			{ 32,  126 }, // ASCII printable
			{ 160, 255 }, // Latin-1 supplement
		};
		constexpr int k_CodepointRangeCount =
			static_cast<int>(sizeof(k_BakedRanges) / sizeof(k_BakedRanges[0]));

		constexpr int ComputeBakedCodepointCount() {
			int total = 0;
			for (const auto& r : k_BakedRanges) total += int(r.Last - r.First + 1);
			return total;
		}
		constexpr int k_CodepointCount = ComputeBakedCodepointCount();

		constexpr int k_AtlasInitialSide = 256;
		constexpr int k_AtlasMaxSide = 4096;

		constexpr unsigned EncodeBgfxIdx(uint16_t idx) noexcept {
			return static_cast<unsigned>(idx) + 1u;
		}
		constexpr uint16_t DecodeBgfxIdx(unsigned m) noexcept {
			return static_cast<uint16_t>(m - 1u);
		}
		bgfx::TextureHandle FromAtlas(unsigned m) noexcept {
			if (m == 0) return BGFX_INVALID_HANDLE;
			return bgfx::TextureHandle{ DecodeBgfxIdx(m) };
		}
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
		if (this == &other) return *this;
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
		return *this;
	}

	bool Font::LoadFromFile(const std::string& path, float pixelSize) {
		if (path.empty() || pixelSize <= 0.0f) return false;
		Cleanup();
		if (!File::Exists(path)) {
			AIM_CORE_ERROR_TAG("Font", "TTF not found: {}", path);
			return false;
		}
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
		if (ttfBuffer.empty() || pixelSize <= 0.0f) return false;
		Cleanup();
		m_TtfBuffer = ttfBuffer;
		m_Filepath = sourcePath;
		return BakeAtlas(pixelSize);
	}

	bool Font::BakeAtlas(float pixelSize) {
		if (m_TtfBuffer.empty() || pixelSize <= 0.0f) return false;

		m_StbFontInfoStorage.assign(sizeof(stbtt_fontinfo), 0);
		stbtt_fontinfo* font = reinterpret_cast<stbtt_fontinfo*>(m_StbFontInfoStorage.data());

		if (!stbtt_InitFont(font, m_TtfBuffer.data(),
			stbtt_GetFontOffsetForIndex(m_TtfBuffer.data(), 0)))
		{
			AIM_CORE_ERROR_TAG("Font", "stbtt_InitFont failed: {}", m_Filepath);
			m_TtfBuffer.clear();
			m_StbFontInfoStorage.clear();
			return false;
		}

		const float scale = stbtt_ScaleForPixelHeight(font, pixelSize);
		m_StbScale = scale;
		m_PixelSize = pixelSize;

		int ascent = 0, descent = 0, lineGap = 0;
		stbtt_GetFontVMetrics(font, &ascent, &descent, &lineGap);
		m_Ascent = ascent * scale;
		m_Descent = descent * scale;
		m_LineHeight = (ascent - descent + lineGap) * scale;

		// Mirror the OpenGL impl's two-tier bake: try 2× oversample first
		// for sharper glyphs, fall back to 1× for very large pixel sizes.
		// The Font.cpp fast-path optimization in `Font::BakeAtlas` already
		// skips the 2× pre-pass above 48 px (added in the font-perf fix);
		// duplicate that condition here so the bgfx path matches.
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
				if (!stbtt_PackBegin(&pctx, bitmap.data(), side, side, 0, 1, nullptr)) break;
				stbtt_PackSetOversampling(&pctx, oversample, oversample);

				stbtt_pack_range packRanges[k_CodepointRangeCount]{};
				int cumulative = 0;
				for (int r = 0; r < k_CodepointRangeCount; ++r) {
					const auto& cr = k_BakedRanges[r];
					const int count = int(cr.Last - cr.First + 1);
					packRanges[r].font_size = pixelSize;
					packRanges[r].first_unicode_codepoint_in_range = int(cr.First);
					packRanges[r].array_of_unicode_codepoints = nullptr;
					packRanges[r].num_chars = count;
					packRanges[r].chardata_for_range = packed.data() + cumulative;
					cumulative += count;
				}
				const int packResult = stbtt_PackFontRanges(&pctx, m_TtfBuffer.data(), 0,
					packRanges, k_CodepointRangeCount);
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
			AIM_CORE_ERROR_TAG("Font", "Atlas pack failed (max {}px): {}", k_AtlasMaxSide, m_Filepath);
			m_TtfBuffer.clear();
			m_StbFontInfoStorage.clear();
			return false;
		}
		(void)chosenOversample;

		// Upload as R8 (single-channel alpha). bgfx::copy duplicates
		// the source bytes so we can free `bitmap` immediately after.
		const uint32_t bytes = static_cast<uint32_t>(m_AtlasWidth)
			* static_cast<uint32_t>(m_AtlasHeight);
		const bgfx::Memory* mem = bgfx::copy(bitmap.data(), bytes);
		bgfx::TextureHandle handle = bgfx::createTexture2D(
			static_cast<uint16_t>(m_AtlasWidth),
			static_cast<uint16_t>(m_AtlasHeight),
			/*hasMips=*/false, /*numLayers=*/1,
			bgfx::TextureFormat::R8,
			BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
			mem);
		if (!bgfx::isValid(handle)) {
			AIM_CORE_ERROR_TAG("Font", "bgfx::createTexture2D failed for atlas: {}", m_Filepath);
			m_TtfBuffer.clear();
			m_StbFontInfoStorage.clear();
			return false;
		}
		m_AtlasTexture = EncodeBgfxIdx(handle.idx);

		// Translate stb's packed table → m_Glyphs map (same as OpenGL impl).
		int packedIndex = 0;
		for (int r = 0; r < k_CodepointRangeCount; ++r) {
			const auto& cr = k_BakedRanges[r];
			const int count = int(cr.Last - cr.First + 1);
			for (int i = 0; i < count; ++i) {
				const stbtt_packedchar& pc = packed[packedIndex++];
				GlyphMetrics m;
				m.U0 = float(pc.x0) / float(m_AtlasWidth);
				m.V0 = float(pc.y0) / float(m_AtlasHeight);
				m.U1 = float(pc.x1) / float(m_AtlasWidth);
				m.V1 = float(pc.y1) / float(m_AtlasHeight);
				m.Width = pc.xoff2 - pc.xoff;
				m.Height = pc.yoff2 - pc.yoff;
				m.XOffset = pc.xoff;
				m.YOffset = pc.yoff;
				m.XAdvance = pc.xadvance;
				m_Glyphs[cr.First + uint32_t(i)] = m;
			}
		}
		return true;
	}

	const GlyphMetrics* Font::GetGlyph(uint32_t codepoint) const {
		auto it = m_Glyphs.find(codepoint);
		return it == m_Glyphs.end() ? nullptr : &it->second;
	}

	float Font::GetKerning(uint32_t a, uint32_t b) const {
		if (m_StbFontInfoStorage.empty() || m_StbScale == 0.0f) return 0.0f;
		const stbtt_fontinfo* font = reinterpret_cast<const stbtt_fontinfo*>(m_StbFontInfoStorage.data());
		int kern = stbtt_GetCodepointKernAdvance(const_cast<stbtt_fontinfo*>(font),
			static_cast<int>(a), static_cast<int>(b));
		return kern * m_StbScale;
	}

	void Font::Cleanup() {
		if (m_AtlasTexture != 0) {
			bgfx::destroy(FromAtlas(m_AtlasTexture));
			m_AtlasTexture = 0;
		}
		m_TtfBuffer.clear();
		m_Glyphs.clear();
		m_AtlasWidth = 0;
		m_AtlasHeight = 0;
		m_PixelSize = 0.0f;
		m_Ascent = 0.0f;
		m_Descent = 0.0f;
		m_LineHeight = 0.0f;
		m_StbScale = 0.0f;
		m_StbFontInfoStorage.clear();
		m_Filepath.clear();
	}

} // namespace Axiom
