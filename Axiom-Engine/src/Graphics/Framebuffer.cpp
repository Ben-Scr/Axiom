#include "pch.hpp"
#include "Graphics/Framebuffer.hpp"

#include "Core/Log.hpp"

#include <bgfx/bgfx.h>

#include <utility>

namespace Axiom::BgfxBackend {
	// Defined in Backend/BgfxApi.cpp — exposed here so Destroy() can
	// release the per-FBO view-id back to the pool when the FBO is torn
	// down. Otherwise long-running editor sessions that resize / recreate
	// FBOs would leak view-ids until the pool is exhausted.
	void FreeViewIdForFbo(uint32_t encodedBackendId);
}

// =============================================================================
// Framebuffer — real bgfx implementation. Stage 2.1 of the bgfx port.
// -----------------------------------------------------------------------------
// Allocates a real bgfx::FrameBufferHandle backed by a colour attachment and
// a paired depth/stencil attachment, mirroring the OpenGL impl's layout.
// `m_BackendId` stores (frameBuffer.idx + 1) and `m_ColorTextureId` stores
// (colorTexture.idx + 1), preserving the existing "0 = unset" sentinel that
// the editor already treats as IsValid()==false.
//
// `GetColorTextureBackendId()` returns the bgfx idx (un-shifted, raw) so
// callers passing this value into `ImGui::Image` get the right token for
// imgui_impl_bgfx to sample. Stage 3's imgui-backend swap is what wires
// that consumer up — for now the FBO is correctly created and destroyed,
// the editor just doesn't render anything into it yet (the renderer-side
// stubs don't submit draws).
// =============================================================================

namespace Axiom {

	namespace {
		constexpr uint32_t EncodeIdx(uint16_t idx) noexcept {
			return static_cast<uint32_t>(idx) + 1u;
		}
		constexpr uint16_t DecodeIdx(uint32_t m) noexcept {
			return static_cast<uint16_t>(m - 1u);
		}
		bgfx::FrameBufferHandle FromFbo(uint32_t m) noexcept {
			if (m == 0) return BGFX_INVALID_HANDLE;
			return bgfx::FrameBufferHandle{ DecodeIdx(m) };
		}
		bgfx::TextureHandle FromTex(uint32_t m) noexcept {
			if (m == 0) return BGFX_INVALID_HANDLE;
			return bgfx::TextureHandle{ DecodeIdx(m) };
		}

		bgfx::TextureFormat::Enum ToBgfx(TextureFormat f) noexcept {
			switch (f) {
			case TextureFormat::RGBA8:           return bgfx::TextureFormat::RGBA8;
			case TextureFormat::R8:              return bgfx::TextureFormat::R8;
			case TextureFormat::Depth24Stencil8: return bgfx::TextureFormat::D24S8;
			}
			return bgfx::TextureFormat::RGBA8;
		}

		uint64_t SamplerFlags(TextureFilter filter) noexcept {
			// Match the OpenGL impl's defaults — clamp + chosen min/mag.
			const uint64_t base =
				BGFX_SAMPLER_U_CLAMP |
				BGFX_SAMPLER_V_CLAMP;
			if (filter == TextureFilter::Nearest) {
				return base | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
			}
			return base; // bgfx default min/mag are linear
		}
	}

	Framebuffer::Framebuffer(int width, int height,
		TextureFormat colorFormat, TextureFilter filter)
	{
		Recreate(width, height, colorFormat, filter);
	}

	Framebuffer::Framebuffer(Framebuffer&& other) noexcept
		: m_BackendId(other.m_BackendId)
		, m_ColorTextureId(other.m_ColorTextureId)
		, m_DepthRenderbuffer(other.m_DepthRenderbuffer)
		, m_Viewport(other.m_Viewport)
	{
		other.m_BackendId = 0;
		other.m_ColorTextureId = 0;
		other.m_DepthRenderbuffer = 0;
		other.m_Viewport = Viewport{ 0, 0 };
	}

	Framebuffer& Framebuffer::operator=(Framebuffer&& other) noexcept {
		if (this == &other) return *this;
		Destroy();
		m_BackendId = other.m_BackendId;
		m_ColorTextureId = other.m_ColorTextureId;
		m_DepthRenderbuffer = other.m_DepthRenderbuffer;
		m_Viewport = other.m_Viewport;
		other.m_BackendId = 0;
		other.m_ColorTextureId = 0;
		other.m_DepthRenderbuffer = 0;
		other.m_Viewport = Viewport{ 0, 0 };
		return *this;
	}

	Framebuffer::~Framebuffer() {
		Destroy();
	}

	bool Framebuffer::Recreate(int width, int height,
		TextureFormat colorFormat, TextureFilter filter)
	{
		if (width <= 0 || height <= 0) return false;

		const bool sizeMatches = m_Viewport.GetWidth() == width
			&& m_Viewport.GetHeight() == height;
		if (m_BackendId != 0 && sizeMatches) {
			return true;
		}

		Destroy();
		m_Viewport.SetSize(width, height);

		const uint64_t texFlags = BGFX_TEXTURE_RT | SamplerFlags(filter);
		bgfx::TextureHandle color = bgfx::createTexture2D(
			static_cast<uint16_t>(width),
			static_cast<uint16_t>(height),
			/*hasMips=*/false, /*numLayers=*/1,
			ToBgfx(colorFormat),
			texFlags);
		if (!bgfx::isValid(color)) {
			AIM_CORE_ERROR_TAG("Framebuffer", "bgfx::createTexture2D (color) failed");
			return false;
		}

		bgfx::TextureHandle depth = bgfx::createTexture2D(
			static_cast<uint16_t>(width),
			static_cast<uint16_t>(height),
			/*hasMips=*/false, /*numLayers=*/1,
			bgfx::TextureFormat::D24S8,
			BGFX_TEXTURE_RT_WRITE_ONLY);
		if (!bgfx::isValid(depth)) {
			bgfx::destroy(color);
			AIM_CORE_ERROR_TAG("Framebuffer", "bgfx::createTexture2D (depth) failed");
			return false;
		}

		bgfx::TextureHandle attachments[2] = { color, depth };
		// destroyTextures=true so the bgfx::destroy(fbo) call in Destroy()
		// also frees the colour + depth textures (matches the OpenGL impl's
		// "delete attachments alongside the FBO" semantics).
		bgfx::FrameBufferHandle fbo =
			bgfx::createFrameBuffer(2, attachments, /*destroyTextures=*/true);
		if (!bgfx::isValid(fbo)) {
			bgfx::destroy(color);
			bgfx::destroy(depth);
			AIM_CORE_ERROR_TAG("Framebuffer", "bgfx::createFrameBuffer failed");
			return false;
		}

		m_BackendId = EncodeIdx(fbo.idx);
		m_ColorTextureId = EncodeIdx(color.idx);
		// bgfx tracks the depth attachment's lifetime through the FBO's
		// destroyTextures flag; we don't need a separate handle slot for
		// it, but keep m_DepthRenderbuffer non-zero so the existing
		// `m_DepthRenderbuffer != 0` book-keeping in moves still works.
		m_DepthRenderbuffer = EncodeIdx(depth.idx);
		return true;
	}

	void Framebuffer::Destroy() {
		if (m_BackendId != 0) {
			// Release the view-id this FBO was paired with (no-op for
			// FBOs that never got bound). Then destroy the bgfx FBO —
			// destroyTextures=true at create time also frees the
			// colour + depth attachments.
			BgfxBackend::FreeViewIdForFbo(m_BackendId);
			bgfx::destroy(FromFbo(m_BackendId));
			m_BackendId = 0;
			m_ColorTextureId = 0;
			m_DepthRenderbuffer = 0;
		}
		m_Viewport = Viewport{ 0, 0 };
	}

} // namespace Axiom
