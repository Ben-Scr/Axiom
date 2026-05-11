#include "pch.hpp"
#include "Graphics/Texture2D.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/ImageData.hpp"

#include <webgpu/webgpu_cpp.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <unordered_map>
#include <utility>

// =============================================================================
// Texture2D — WebGPU (Dawn) implementation. Stage 2 of the WebGPU port.
// -----------------------------------------------------------------------------
// Sibling to Texture2D.cpp (which is the bgfx implementation). Premake selects
// this file under --rhi=webgpu and excludes Texture2D.cpp; under --rhi=bgfx
// the opposite happens. Texture2D.hpp is shared between the two and stays
// backend-neutral — it only owns an opaque `unsigned m_Tex` ID that each
// backend interprets independently.
//
// On the WebGPU side, `m_Tex` is the raw WGPUTextureView pointer (cast to
// uint64_t) for this texture, and indexes into a TU-local pool of
//   { wgpu::Texture, wgpu::TextureView, wgpu::Sampler, width, height,
//     filter, wrap-u, wrap-v }
// keyed by that same pointer value. 0 stays reserved as the "unset"
// sentinel — the existing `IsValid() => m_Tex != 0` contract from the
// header is preserved. Storing the raw pointer in m_Tex means editor
// code that passes `tex->GetHandle()` as an ImTextureID to ImGui::Image
// hands imgui_impl_wgpu a real Dawn handle, rather than a pool ID that
// imgui_impl_wgpu would reinterpret_cast and dereference as a pointer
// (crashing with 0xC0000005 at addresses like 0x1, 0x2, ...).
//
// What this DOES (Stage 2):
//   * Decodes file via stbi_load (same as bgfx impl), forces RGBA8.
//   * Creates a 2D wgpu::Texture with TextureBinding | CopyDst usage so it
//     can be sampled and uploaded into.
//   * Uploads via wgpu::Queue::WriteTexture — one upload per Load(); no
//     staging buffer is exposed by the wrapper for the simple case.
//   * Creates a default 2D wgpu::TextureView covering the full mip range.
//   * Creates a wgpu::Sampler matching the Filter/Wrap inputs and caches
//     it on the pool entry. SetFilter / SetWrap rebuilds the sampler on
//     change (WebGPU samplers are immutable; you replace them).
//
// What this does NOT do yet:
//   * Mipmap chain. Mipmap-gen on Dawn is "render to each mip level" or
//     "use a compute shader" — neither is in the engine yet. Single-mip
//     uploads are correct for Stage 2's scope; Stage 4 (renderer ports)
//     can revisit when actual scenes start needing minification quality.
//   * sRGB. The bgfx side also defers this; same TODO.
//   * GetImageData. wgpu::Queue::OnSubmittedWorkDone + buffer readback is
//     async; the only inline caller (editor thumbnail preview) is fine
//     without it for now and returns nullptr like the bgfx impl.
// =============================================================================

namespace Axiom {

	namespace {
		// ── Pool ────────────────────────────────────────────────────────────
		struct GpuTexture {
			wgpu::Texture     Texture;
			wgpu::TextureView View;
			wgpu::Sampler     Sampler;
			uint32_t          Width  = 0;
			uint32_t          Height = 0;
			Filter            CachedFilter = Filter::Point;
			Wrap              CachedWrapU  = Wrap::Clamp;
			Wrap              CachedWrapV  = Wrap::Clamp;
		};
		// Pool keyed by the raw WGPUTextureView pointer (cast to uint64_t).
		// Storing the pointer in Texture2D::m_Tex means editor ImGui::Image
		// callers can pass tex->GetHandle() straight to imgui_impl_wgpu
		// (which casts it back to WGPUTextureView and dereferences). The
		// pointer is also unique per GpuTexture entry for the duration of
		// its lifetime, so it works as a stable map key.
		std::unordered_map<uint64_t, GpuTexture> g_Textures;

		uint64_t RegisterTexture(GpuTexture&& tex) {
			const uint64_t key = reinterpret_cast<uint64_t>(tex.View.Get());
			if (key == 0) return 0;  // shouldn't happen; defensive
			g_Textures.emplace(key, std::move(tex));
			return key;
		}

		void FreeTextureSlot(uint64_t id) {
			if (id == 0) return;
			g_Textures.erase(id);
		}

		GpuTexture* TryLookup(uint64_t id) {
			if (id == 0) return nullptr;
			auto it = g_Textures.find(id);
			return (it == g_Textures.end()) ? nullptr : &it->second;
		}

		// ── Enum mapping ────────────────────────────────────────────────────
		using WB = ::Axiom::WebGPUBackend::FilterMode;
		using WW = ::Axiom::WebGPUBackend::WrapMode;

		WB ToBackendFilter(Filter f) noexcept {
			switch (f) {
				case Filter::Point:       return WB::Point;
				case Filter::Bilinear:    return WB::Bilinear;
				case Filter::Trilinear:   return WB::Trilinear;
				case Filter::Anisotropic: return WB::Anisotropic;
			}
			return WB::Point;
		}
		WW ToBackendWrap(Wrap w) noexcept {
			switch (w) {
				case Wrap::Repeat: return WW::Repeat;
				case Wrap::Clamp:  return WW::Clamp;
				case Wrap::Mirror: return WW::Mirror;
				case Wrap::Border: return WW::Border;
			}
			return WW::Clamp;
		}

		wgpu::Sampler MakeSamplerFor(Filter f, Wrap u, Wrap v) {
			return WebGPUBackend::CreateSampler(
				ToBackendFilter(f), ToBackendWrap(u), ToBackendWrap(v));
		}
	}

	// ── WebGPUBackend pool exports + sampler factory ────────────────────────
	// Definitions for the lookup / sampler-construction declarations in
	// Backend/WebGPUBackend.hpp. Living next to the pool that owns the
	// state means callers can't see the pool's storage layout — they get
	// just the wgpu handles + metadata.

	namespace WebGPUBackend {

		TextureLookup LookupTexture2D(uint64_t backendId) {
			GpuTexture* slot = TryLookup(backendId);
			if (!slot) return TextureLookup{};
			TextureLookup out;
			out.View   = slot->View;
			out.Sampler= slot->Sampler;
			out.Width  = slot->Width;
			out.Height = slot->Height;
			out.Valid  = true;
			return out;
		}

		wgpu::Sampler CreateSampler(FilterMode filter, WrapMode wrapU, WrapMode wrapV) {
			wgpu::Device device = GetDevice();
			if (!device) return nullptr;

			auto toAddress = [](WrapMode w) {
				switch (w) {
					case WrapMode::Repeat: return wgpu::AddressMode::Repeat;
					case WrapMode::Clamp:  return wgpu::AddressMode::ClampToEdge;
					case WrapMode::Mirror: return wgpu::AddressMode::MirrorRepeat;
					case WrapMode::Border: return wgpu::AddressMode::ClampToEdge;  // WebGPU has no border-color mode
				}
				return wgpu::AddressMode::ClampToEdge;
			};

			wgpu::SamplerDescriptor desc{};
			desc.addressModeU = toAddress(wrapU);
			desc.addressModeV = toAddress(wrapV);
			desc.addressModeW = wgpu::AddressMode::ClampToEdge;

			switch (filter) {
				case FilterMode::Point:
					desc.magFilter    = wgpu::FilterMode::Nearest;
					desc.minFilter    = wgpu::FilterMode::Nearest;
					desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
					desc.maxAnisotropy = 1;
					break;
				case FilterMode::Bilinear:
					desc.magFilter    = wgpu::FilterMode::Linear;
					desc.minFilter    = wgpu::FilterMode::Linear;
					desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
					desc.maxAnisotropy = 1;
					break;
				case FilterMode::Trilinear:
					desc.magFilter    = wgpu::FilterMode::Linear;
					desc.minFilter    = wgpu::FilterMode::Linear;
					desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
					desc.maxAnisotropy = 1;
					break;
				case FilterMode::Anisotropic:
					// maxAnisotropy > 1 requires linear filtering on all axes
					// per the spec; the renderer's actual aniso level can be
					// project-driven later (Stage 5 settings panel).
					desc.magFilter    = wgpu::FilterMode::Linear;
					desc.minFilter    = wgpu::FilterMode::Linear;
					desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
					desc.maxAnisotropy = 16;
					break;
			}

			return device.CreateSampler(&desc);
		}

	}  // namespace WebGPUBackend

	// ── Texture2D ───────────────────────────────────────────────────────────

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
		m_Tex      = other.m_Tex;
		m_Width    = other.m_Width;
		m_Height   = other.m_Height;
		m_Channels = other.m_Channels;
		m_Filter   = other.m_Filter;
		m_WrapU    = other.m_WrapU;
		m_WrapV    = other.m_WrapV;
		m_HasMips  = other.m_HasMips;
		other.m_Tex = 0;
		other.m_Width = 0;
		other.m_Height = 0;
		other.m_Channels = 0;
		return *this;
	}

	void Texture2D::Destroy() {
		if (m_Tex != 0) {
			FreeTextureSlot(m_Tex);
			m_Tex = 0;
		}
		m_Width = 0;
		m_Height = 0;
		m_Channels = 0;
	}

	bool Texture2D::Load(const char* path, bool generateMipmaps, bool srgb, bool flipVertical) {
		Destroy();
		(void)srgb;  // Stage 2 follow-up — wgpu::TextureFormat::RGBA8UnormSrgb when wired

		if (!WebGPUBackend::IsInitialized()) {
			AIM_CORE_WARN_TAG("Texture2D",
				"Load called before WebGPU backend initialized: {}", path);
			return false;
		}

		stbi_set_flip_vertically_on_load(flipVertical);
		int w = 0, h = 0, n = 0;
		unsigned char* pixels = stbi_load(path, &w, &h, &n, 4);
		stbi_set_flip_vertically_on_load(false);
		if (!pixels) {
			AIM_CORE_WARN_TAG("Texture2D", "Failed to load texture: {}", path);
			return false;
		}

		wgpu::Device device = WebGPUBackend::GetDevice();
		wgpu::Queue  queue  = WebGPUBackend::GetQueue();

		// Create the 2D texture. Usage = TextureBinding (sampled in shaders)
		// + CopyDst (target of queue.WriteTexture). RenderAttachment isn't
		// needed here — this class is for sampled images, not render targets;
		// Framebuffer_WebGPU.cpp owns the render-target side.
		wgpu::TextureDescriptor texDesc{};
		texDesc.dimension       = wgpu::TextureDimension::e2D;
		texDesc.size            = { static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 };
		texDesc.format          = wgpu::TextureFormat::RGBA8Unorm;
		texDesc.usage           = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
		texDesc.mipLevelCount   = 1;  // see "What this does NOT do yet" header note
		texDesc.sampleCount     = 1;
		texDesc.viewFormatCount = 0;

		wgpu::Texture texture = device.CreateTexture(&texDesc);
		if (!texture) {
			stbi_image_free(pixels);
			AIM_CORE_WARN_TAG("Texture2D", "wgpu::Device::CreateTexture failed: {}", path);
			return false;
		}

		// Upload via Queue::WriteTexture. Dawn copies the source bytes into
		// an internal staging buffer, so we can stbi_image_free immediately
		// after the call returns.
		// Dawn renamed these structs mid-2025: ImageCopyTexture ->
		// TexelCopyTextureInfo, TextureDataLayout -> TexelCopyBufferLayout.
		// Layout + semantics are unchanged; only the type names moved.
		wgpu::TexelCopyTextureInfo dst{};
		dst.texture  = texture;
		dst.mipLevel = 0;
		dst.origin   = { 0, 0, 0 };
		dst.aspect   = wgpu::TextureAspect::All;

		wgpu::TexelCopyBufferLayout layout{};
		layout.offset        = 0;
		layout.bytesPerRow   = static_cast<uint32_t>(w) * 4u;
		layout.rowsPerImage  = static_cast<uint32_t>(h);

		wgpu::Extent3D writeSize{ static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 };
		const size_t byteCount = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;

		queue.WriteTexture(&dst, pixels, byteCount, &layout, &writeSize);
		stbi_image_free(pixels);

		// Default 2D view spanning all (one) mip levels.
		wgpu::TextureViewDescriptor viewDesc{};
		viewDesc.format          = wgpu::TextureFormat::RGBA8Unorm;
		viewDesc.dimension       = wgpu::TextureViewDimension::e2D;
		viewDesc.mipLevelCount   = 1;
		viewDesc.arrayLayerCount = 1;
		viewDesc.aspect          = wgpu::TextureAspect::All;
		wgpu::TextureView view = texture.CreateView(&viewDesc);

		GpuTexture slot;
		slot.Texture      = std::move(texture);
		slot.View         = std::move(view);
		slot.Sampler      = MakeSamplerFor(m_Filter, m_WrapU, m_WrapV);
		slot.Width        = static_cast<uint32_t>(w);
		slot.Height       = static_cast<uint32_t>(h);
		slot.CachedFilter = m_Filter;
		slot.CachedWrapU  = m_WrapU;
		slot.CachedWrapV  = m_WrapV;

		m_Tex      = RegisterTexture(std::move(slot));
		m_Width    = w;
		m_Height   = h;
		m_Channels = n;
		m_HasMips  = generateMipmaps;
		return true;
	}

	// Renderer-side submit path is per-pipeline in WebGPU (BindGroup +
	// SetPipeline before the draw); Texture2D::Submit is a no-op on this
	// backend, matching the bgfx impl. The renderer ports (Stage 3+) call
	// `WebGPUBackend::LookupTexture2D(m_Tex)` themselves at submit time
	// to fetch the view + sampler for the active bind group.
	void Texture2D::Submit(uint8_t /*unit*/) const {}

	// Sampler mutations rebuild the wgpu::Sampler on the pool entry — the
	// next bind-group construction picks up the new one. Samplers are
	// immutable in WebGPU, so we can't "update" an existing one; replacing
	// is the spec-correct operation.
	void Texture2D::SetFilter(Filter filter) {
		m_Filter = filter;
		if (GpuTexture* slot = TryLookup(m_Tex)) {
			slot->Sampler = MakeSamplerFor(m_Filter, m_WrapU, m_WrapV);
			slot->CachedFilter = m_Filter;
		}
	}
	void Texture2D::SetWrapU(Wrap u) {
		m_WrapU = u;
		if (GpuTexture* slot = TryLookup(m_Tex)) {
			slot->Sampler = MakeSamplerFor(m_Filter, m_WrapU, m_WrapV);
			slot->CachedWrapU = m_WrapU;
		}
	}
	void Texture2D::SetWrapV(Wrap v) {
		m_WrapV = v;
		if (GpuTexture* slot = TryLookup(m_Tex)) {
			slot->Sampler = MakeSamplerFor(m_Filter, m_WrapU, m_WrapV);
			slot->CachedWrapV = m_WrapV;
		}
	}
	void Texture2D::SetSampler(Filter filter, Wrap u, Wrap v) {
		m_Filter = filter;
		m_WrapU  = u;
		m_WrapV  = v;
		if (GpuTexture* slot = TryLookup(m_Tex)) {
			slot->Sampler = MakeSamplerFor(m_Filter, m_WrapU, m_WrapV);
			slot->CachedFilter = m_Filter;
			slot->CachedWrapU  = m_WrapU;
			slot->CachedWrapV  = m_WrapV;
		}
	}
	void Texture2D::ApplySamplerParams() const {}

	std::unique_ptr<ImageData> Texture2D::GetImageData() const {
		// Async-readback via Queue.OnSubmittedWorkDone + Buffer mapping is
		// multi-frame; the inline editor thumbnail caller is fine without
		// it for Stage 2. Same TODO as the bgfx impl.
		return nullptr;
	}

}  // namespace Axiom
