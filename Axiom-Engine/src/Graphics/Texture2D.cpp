#include "pch.hpp"
#include "Graphics/Texture2D.hpp"

#include "Core/Log.hpp"
#include "Graphics/ImageData.hpp"

#include <bgfx/bgfx.h>

// stbi_*: the OpenGL Texture2D.cpp owns the canonical STB_IMAGE_IMPLEMENTATION
// when it's compiled. Under --rhi=bgfx that file is removed from the build,
// so this TU has to bring in the implementation instead. Both backends can't
// be active at once (the premake gate is exclusive), so there's no risk of
// duplicate-symbol issues.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <utility>

// =============================================================================
// Texture2D — real bgfx implementation. Stage 2.1 of the bgfx port.
// -----------------------------------------------------------------------------
// Decodes the file via stbi_load (same as the OpenGL implementation), then
// uploads through bgfx::createTexture2D + bgfx::copy. The class's
// `unsigned m_Tex` field is reused as an opaque token: storage is
// (bgfx::TextureHandle::idx + 1) so the existing `m_Tex != 0` IsValid()
// contract still holds (bgfx idx 0 IS a valid handle, so the +1 sentinel
// avoids aliasing with the "unset" state).
//
// What this DOES today:
//   * Real GPU resource lifetime — PurgeUnreferenced + scene loads/unloads
//     allocate/free actual bgfx textures that show up in bgfx::getStats().
//   * Move semantics / Destroy / Load round-trip identical to the OpenGL impl.
//   * Sampler state (Filter, Wrap) tracked but not yet applied — bgfx applies
//     sampler flags per `bgfx::setTexture` call, not per-resource. The
//     Renderer2D bgfx implementation (also Stage 2) will translate this
//     class's filter/wrap into BGFX_SAMPLER_* flags at submit time.
//
// What this does NOT do yet:
//   * No mipmap chain — single-mip uploads only. Mipmap generation in bgfx
//     uses BGFX_TEXTURE_GENMIPS at create time + the right sampler filter;
//     deferred to a follow-up so the stb_image path stays simple here.
//   * No sRGB sampling — Stage 2 follow-up adds bgfx::TextureFormat::RGBA8S
//     for sRGB textures. Today everything is RGBA8.
//   * GetImageData() returns nullptr — bgfx::readTexture is async (the
//     blit-readback completes on a later frame) and the only caller
//     (editor texture-preview thumbnails) reads inline. Plumbing through
//     a bgfx::FrameNumber-aware readback is its own sub-stage.
// =============================================================================

namespace Axiom {

	namespace {
		// 0 = "no texture"; otherwise the bgfx idx is (m_Tex - 1).
		constexpr unsigned EncodeBgfxIdx(uint16_t idx) noexcept {
			return static_cast<unsigned>(idx) + 1u;
		}
		constexpr uint16_t DecodeBgfxIdx(unsigned m) noexcept {
			return static_cast<uint16_t>(m - 1u);
		}
		bgfx::TextureHandle FromMTex(unsigned m_Tex) noexcept {
			if (m_Tex == 0) return BGFX_INVALID_HANDLE;
			return bgfx::TextureHandle{ DecodeBgfxIdx(m_Tex) };
		}
	}

	Texture2D::Texture2D(const char* path,
		Filter filter, Wrap wrapU, Wrap wrapV,
		bool generateMipmaps, bool srgb, bool flipVertical)
		: m_Filter(filter), m_WrapU(wrapU), m_WrapV(wrapV), m_HasMips(generateMipmaps)
	{
		if (path != nullptr) {
			Load(path, generateMipmaps, srgb, flipVertical);
		}
	}

	Texture2D::~Texture2D() {
		Destroy();
	}

	Texture2D::Texture2D(Texture2D&& other) noexcept
		: m_Tex(other.m_Tex)
		, m_Width(other.m_Width)
		, m_Height(other.m_Height)
		, m_Channels(other.m_Channels)
		, m_Filter(other.m_Filter)
		, m_WrapU(other.m_WrapU)
		, m_WrapV(other.m_WrapV)
		, m_HasMips(other.m_HasMips)
	{
		other.m_Tex = 0;
		other.m_Width = 0;
		other.m_Height = 0;
		other.m_Channels = 0;
	}

	Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
		if (this == &other) return *this;
		Destroy();
		m_Tex = other.m_Tex;
		m_Width = other.m_Width;
		m_Height = other.m_Height;
		m_Channels = other.m_Channels;
		m_Filter = other.m_Filter;
		m_WrapU = other.m_WrapU;
		m_WrapV = other.m_WrapV;
		m_HasMips = other.m_HasMips;
		other.m_Tex = 0;
		other.m_Width = 0;
		other.m_Height = 0;
		other.m_Channels = 0;
		return *this;
	}

	void Texture2D::Destroy() {
		if (m_Tex != 0) {
			bgfx::destroy(FromMTex(m_Tex));
			m_Tex = 0;
		}
		m_Width = 0;
		m_Height = 0;
		m_Channels = 0;
	}

	bool Texture2D::Load(const char* path, bool generateMipmaps, bool srgb, bool flipVertical) {
		Destroy();
		(void)srgb; // see header comment — sRGB sampling is a follow-up.

		stbi_set_flip_vertically_on_load(flipVertical);
		int w = 0, h = 0, n = 0;
		// Force RGBA so bgfx::TextureFormat::RGBA8 matches without a
		// per-channel-count switch.
		unsigned char* pixels = stbi_load(path, &w, &h, &n, 4);
		stbi_set_flip_vertically_on_load(false);
		if (!pixels) {
			AIM_CORE_WARN_TAG("Texture2D", "Failed to load texture: {}", path);
			return false;
		}

		// bgfx::copy duplicates the source bytes into a bgfx-managed
		// memory block (released after upload). We can immediately
		// stbi_image_free without waiting on the GPU.
		const uint32_t byteCount = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4u;
		const bgfx::Memory* mem = bgfx::copy(pixels, byteCount);

		bgfx::TextureHandle handle = bgfx::createTexture2D(
			static_cast<uint16_t>(w),
			static_cast<uint16_t>(h),
			/*hasMips=*/false,
			/*numLayers=*/1,
			bgfx::TextureFormat::RGBA8,
			BGFX_TEXTURE_NONE,
			mem);

		stbi_image_free(pixels);

		if (!bgfx::isValid(handle)) {
			AIM_CORE_WARN_TAG("Texture2D", "bgfx::createTexture2D failed: {}", path);
			return false;
		}

		m_Tex = EncodeBgfxIdx(handle.idx);
		m_Width = w;
		m_Height = h;
		m_Channels = n;
		m_HasMips = generateMipmaps;
		return true;
	}

	// Sampler state tracked here is plumbed into bgfx at submit time
	// (BGFX_SAMPLER_* flags via `bgfx::setTexture(..., flags)`), not as
	// a per-resource property. So setters just update the cached values
	// — the next setTexture call reads them.
	void Texture2D::Submit(uint8_t /*unit*/) const {
		// Renderer2D / TextRenderer / GuiRenderer's bgfx submit paths
		// call bgfx::setTexture themselves with the desired sampler
		// flags. Texture2D::Submit existed for the OpenGL `glActiveTexture
		// + glBindTexture` model; under bgfx it's a no-op.
	}

	void Texture2D::SetFilter(Filter filter) { m_Filter = filter; }
	void Texture2D::SetWrapU(Wrap u) { m_WrapU = u; }
	void Texture2D::SetWrapV(Wrap v) { m_WrapV = v; }
	void Texture2D::SetSampler(Filter filter, Wrap u, Wrap v) {
		m_Filter = filter;
		m_WrapU = u;
		m_WrapV = v;
	}
	void Texture2D::ApplySamplerParams() const {}

	std::unique_ptr<ImageData> Texture2D::GetImageData() const {
		// Async-readback in bgfx is multi-frame (bgfx::readTexture +
		// frame deferral). The single inline caller (the editor's
		// preview thumbnail) is fine without it for now — the preview
		// renders empty until that follow-up sub-stage lands.
		return nullptr;
	}

} // namespace Axiom
