#include "pch.hpp"
#include "Graphics/Framebuffer.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"

#include <webgpu/webgpu_cpp.h>

#include <unordered_map>
#include <utility>

// =============================================================================
// Framebuffer — WebGPU (Dawn) implementation.
// -----------------------------------------------------------------------------
// Layout per FBO:
//   * One colour wgpu::Texture, with usage = RenderAttachment | TextureBinding
//     so the renderer can write to it AND ImGui::Image can sample it for the
//     editor's per-panel previews (Editor View + Game View).
//   * One depth wgpu::Texture (always Depth24PlusStencil8), usage =
//     RenderAttachment only — nothing samples it.
//   * A wgpu::TextureView per attachment, cached so render-pass setup
//     doesn't allocate a fresh view every frame.
//
// IDs:
//   * `m_BackendId`     -> g_Framebuffers map key. A small uint32_t allocated
//     monotonically; resolved by WebGPUBackend::LookupFramebufferByFboId so
//     RenderApi::BindFramebuffer can build the render-pass attachments.
//   * `m_ColorTextureId`-> the raw WGPUTextureView pointer for the colour
//     attachment, cast to uint64_t. ImGui's imgui_impl_wgpu reinterpret_casts
//     ImTextureID directly to WGPUTextureView and dereferences it, so this
//     value MUST be a real Dawn handle (storing a small pool integer here
//     crashes Dawn with 0xC0000005 at addresses like 0x1 + offset). A reverse
//     map g_ColorViewToFbo lets non-ImGui consumers go from the pointer back
//     to the owning Framebuffer.
//   * `m_DepthRenderbuffer` -> kept non-zero so the existing
//     "is-this-slot-active" book-keeping in moves works; legacy field name
//     from the OpenGL era. WebGPU has no separate "renderbuffer" concept;
//     the depth attachment is just another Texture owned by the entry.
//
// `m_BackendId` counter is monotonic with 0 reserved as "unset" — same
// `IsValid() => m_BackendId != 0` contract from the header. `m_ColorTextureId`
// is unique-per-instance by construction (it's a unique pointer).
// =============================================================================

namespace Index {

	namespace {
		struct GpuFramebuffer {
			wgpu::Texture       ColorTexture;
			wgpu::Texture       DepthTexture;
			wgpu::TextureView   ColorView;
			wgpu::TextureView   DepthView;
			wgpu::TextureFormat ColorFormat = wgpu::TextureFormat::Undefined;
			uint32_t            Width  = 0;
			uint32_t            Height = 0;
			uint32_t            ColorTextureId = 0;  // back-pointer for cleanup
		};

		std::unordered_map<uint32_t, GpuFramebuffer> g_Framebuffers;
		// Reverse map: raw WGPUTextureView pointer (cast to uint64_t) -> fboBackendId.
		// `m_ColorTextureId` now stores the raw view pointer directly so the
		// editor can pass it straight to ImGui::Image without a lookup; this
		// map is the inverse so anyone who already has the pointer can find
		// the owning Framebuffer (mostly diagnostic / future use). The pool
		// itself remains keyed by the small `fboBackendId` integer.
		std::unordered_map<uint64_t, uint32_t> g_ColorViewToFbo;
		uint32_t g_NextFbId = 1;

		// Two-frame deferred-destroy queue. When Framebuffer::Destroy runs we
		// move the entry into g_PendingDestroyThisFrame instead of erasing
		// immediately, because ImGui may still hold the color view's raw
		// pointer for the current frame's submitted draws. Each call to
		// ProcessFrameEndDeferredDestroy (from WebGPUApi::Present) advances
		// the queue: this-frame entries roll to last-frame, last-frame
		// entries finally release. So a Destroy() during frame N keeps the
		// Dawn resources alive through the end of frame N+1, which gives
		// any in-flight ImGui submit + Dawn command-buffer execution time
		// to land before the underlying view/texture is dropped.
		std::vector<GpuFramebuffer> g_PendingDestroyThisFrame;
		std::vector<GpuFramebuffer> g_PendingDestroyLastFrame;

		uint32_t AllocateFbId() {
			uint32_t id = g_NextFbId++;
			if (id == 0) id = g_NextFbId++;  // overflow guard, theoretical
			return id;
		}

		GpuFramebuffer* TryLookupFb(uint32_t id) {
			if (id == 0) return nullptr;
			auto it = g_Framebuffers.find(id);
			return (it == g_Framebuffers.end()) ? nullptr : &it->second;
		}

		// Map the engine's backend-neutral TextureFormat enum to a wgpu format.
		// Four supported formats (RGBA8 / R8 / Depth24Stencil8 / RGBA16F);
		// anything else is a hard error rather than a silent substitution.
		wgpu::TextureFormat ToWgpuFormat(TextureFormat f) noexcept {
			switch (f) {
				case TextureFormat::RGBA8:           return wgpu::TextureFormat::RGBA8Unorm;
				case TextureFormat::R8:              return wgpu::TextureFormat::R8Unorm;
				case TextureFormat::Depth24Stencil8: return wgpu::TextureFormat::Depth24PlusStencil8;
				case TextureFormat::RGBA16F:         return wgpu::TextureFormat::RGBA16Float;
			}
			return wgpu::TextureFormat::RGBA8Unorm;
		}
	}

	// ── WebGPUBackend pool exports ──────────────────────────────────────────
	// Implementations of the LookupFramebuffer* declarations in
	// Backend/WebGPUBackend.hpp. WebGPUApi.cpp::BindFramebuffer + the future
	// imgui_impl_wgpu image-binding path call these to resolve a Framebuffer's
	// opaque IDs into wgpu views.

	namespace WebGPUBackend {

		FramebufferLookup LookupFramebufferByFboId(uint32_t fboBackendId) {
			GpuFramebuffer* fb = TryLookupFb(fboBackendId);
			if (!fb) return FramebufferLookup{};

			FramebufferLookup out;
			out.ColorView   = fb->ColorView;
			out.DepthView   = fb->DepthView;
			out.ColorFormat = fb->ColorFormat;
			out.Width       = fb->Width;
			out.Height      = fb->Height;
			out.Valid       = true;
			return out;
		}

		wgpu::TextureView LookupFramebufferColorViewByTextureId(uint64_t colorTextureViewPtr) {
			// `m_ColorTextureId` now IS the raw WGPUTextureView pointer
			// cast to uint64_t. No lookup needed at the engine level:
			// imgui_impl_wgpu reinterpret_casts ImTextureID -> WGPUTextureView
			// and dereferences it directly. This function stays so call
			// sites that want the wgpu::TextureView wrapper (for example,
			// future Framebuffer-as-sampler-source code paths outside
			// ImGui) can resolve it back through our pool.
			if (colorTextureViewPtr == 0) return nullptr;
			auto mapIt = g_ColorViewToFbo.find(colorTextureViewPtr);
			if (mapIt == g_ColorViewToFbo.end()) return nullptr;
			GpuFramebuffer* fb = TryLookupFb(mapIt->second);
			return fb ? fb->ColorView : nullptr;
		}

	}  // namespace WebGPUBackend

	// ── Framebuffer ─────────────────────────────────────────────────────────

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
		other.m_BackendId         = 0;
		other.m_ColorTextureId    = 0;
		other.m_DepthRenderbuffer = 0;
		other.m_Viewport          = Viewport{ 0, 0 };
	}

	Framebuffer& Framebuffer::operator=(Framebuffer&& other) noexcept {
		if (this == &other) return *this;
		Destroy();
		m_BackendId         = other.m_BackendId;
		m_ColorTextureId    = other.m_ColorTextureId;
		m_DepthRenderbuffer = other.m_DepthRenderbuffer;
		m_Viewport          = other.m_Viewport;
		other.m_BackendId         = 0;
		other.m_ColorTextureId    = 0;
		other.m_DepthRenderbuffer = 0;
		other.m_Viewport          = Viewport{ 0, 0 };
		return *this;
	}

	Framebuffer::~Framebuffer() {
		Destroy();
	}

	bool Framebuffer::Recreate(int width, int height,
		TextureFormat colorFormat, TextureFilter /*filter*/)
	{
		if (width <= 0 || height <= 0) return false;

		const bool sizeMatches = m_Viewport.GetWidth() == width
			&& m_Viewport.GetHeight() == height;
		if (m_BackendId != 0 && sizeMatches) {
			return true;
		}

		Destroy();
		m_Viewport.SetSize(width, height);

		if (!WebGPUBackend::IsInitialized()) {
			IDX_CORE_ERROR_TAG("Framebuffer",
				"Recreate called before WebGPU backend initialized");
			return false;
		}

		wgpu::Device device = WebGPUBackend::GetDevice();
		const wgpu::TextureFormat colorWgpuFormat = ToWgpuFormat(colorFormat);

		// Colour attachment. RenderAttachment so we can write to it as a
		// pass target; TextureBinding so the editor's ImGui::Image preview
		// can sample it. CopySrc isn't included — readback isn't supported
		// from the editor yet (same scope as the Stage 1 GetImageData TODO
		// in Texture2D_WebGPU.cpp).
		wgpu::TextureDescriptor colorDesc{};
		colorDesc.dimension       = wgpu::TextureDimension::e2D;
		colorDesc.size            = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
		colorDesc.format          = colorWgpuFormat;
		colorDesc.usage           = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
		colorDesc.mipLevelCount   = 1;
		colorDesc.sampleCount     = 1;
		colorDesc.viewFormatCount = 0;

		wgpu::Texture color = device.CreateTexture(&colorDesc);
		if (!color) {
			IDX_CORE_ERROR_TAG("Framebuffer", "wgpu::Device::CreateTexture (color) failed");
			return false;
		}

		// Depth/stencil attachment — always D24S8. Project settings can
		// add an HDR / no-stencil variant later for renderers that don't
		// need stencil.
		wgpu::TextureDescriptor depthDesc{};
		depthDesc.dimension       = wgpu::TextureDimension::e2D;
		depthDesc.size            = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
		depthDesc.format          = wgpu::TextureFormat::Depth24PlusStencil8;
		depthDesc.usage           = wgpu::TextureUsage::RenderAttachment;
		depthDesc.mipLevelCount   = 1;
		depthDesc.sampleCount     = 1;
		depthDesc.viewFormatCount = 0;

		wgpu::Texture depth = device.CreateTexture(&depthDesc);
		if (!depth) {
			IDX_CORE_ERROR_TAG("Framebuffer", "wgpu::Device::CreateTexture (depth) failed");
			return false;
		}

		// Views. The colour view is the same one ImGui::Image samples; the
		// depth view is only consumed by the render-pass descriptor's
		// depthStencilAttachment.
		wgpu::TextureViewDescriptor colorViewDesc{};
		colorViewDesc.format          = colorWgpuFormat;
		colorViewDesc.dimension       = wgpu::TextureViewDimension::e2D;
		colorViewDesc.mipLevelCount   = 1;
		colorViewDesc.arrayLayerCount = 1;
		colorViewDesc.aspect          = wgpu::TextureAspect::All;
		wgpu::TextureView colorView = color.CreateView(&colorViewDesc);

		wgpu::TextureViewDescriptor depthViewDesc{};
		depthViewDesc.format          = wgpu::TextureFormat::Depth24PlusStencil8;
		depthViewDesc.dimension       = wgpu::TextureViewDimension::e2D;
		depthViewDesc.mipLevelCount   = 1;
		depthViewDesc.arrayLayerCount = 1;
		depthViewDesc.aspect          = wgpu::TextureAspect::All;
		wgpu::TextureView depthView = depth.CreateView(&depthViewDesc);

		const uint32_t fbId = AllocateFbId();
		// Store the raw WGPUTextureView pointer (cast to uint64_t) as the
		// public "color texture id". ImGui's imgui_impl_wgpu reinterpret-
		// casts ImTextureID -> WGPUTextureView and dereferences it, so the
		// value MUST be a real Dawn handle, not a small integer pool key.
		// .Get() returns the underlying C-ABI handle (a pointer) without
		// affecting the C++ wrapper's refcount.
		const uint64_t colorViewPtr =
			reinterpret_cast<uint64_t>(colorView.Get());

		GpuFramebuffer entry;
		entry.ColorTexture   = std::move(color);
		entry.DepthTexture   = std::move(depth);
		entry.ColorView      = std::move(colorView);
		entry.DepthView      = std::move(depthView);
		entry.ColorFormat    = colorWgpuFormat;
		entry.Width          = static_cast<uint32_t>(width);
		entry.Height         = static_cast<uint32_t>(height);
		entry.ColorTextureId = 0;  // legacy field, no longer used as a pool key

		g_Framebuffers.emplace(fbId, std::move(entry));
		g_ColorViewToFbo[colorViewPtr] = fbId;

		m_BackendId         = fbId;
		m_ColorTextureId    = colorViewPtr;
		// Keep m_DepthRenderbuffer non-zero so move ops + IsValid bookkeeping
		// stays consistent. The depth attachment is owned by the framebuffer
		// entry, so the value here is informational only.
		m_DepthRenderbuffer = fbId;
		return true;
	}

	void Framebuffer::Destroy() {
		if (m_BackendId != 0) {
			// Clear the color-view reverse-map entry before moving the
			// main entry into the deferred-destroy queue: any future
			// LookupFramebufferColorViewByTextureId call will fail fast.
			// The view + texture handles themselves stay alive in the
			// pending bucket for one extra frame so an in-flight ImGui
			// frame holding the raw view pointer doesn't dereference a
			// freed Dawn resource.
			if (m_ColorTextureId != 0) {
				g_ColorViewToFbo.erase(m_ColorTextureId);
			}
			auto it = g_Framebuffers.find(m_BackendId);
			if (it != g_Framebuffers.end()) {
				g_PendingDestroyThisFrame.push_back(std::move(it->second));
				g_Framebuffers.erase(it);
			}

			m_BackendId         = 0;
			m_ColorTextureId    = 0;
			m_DepthRenderbuffer = 0;
		}
		m_Viewport = Viewport{ 0, 0 };
	}

	void Framebuffer::ProcessFrameEndDeferredDestroy() {
		// Two-frame bucket roll. Entries enqueued during frame N stay alive
		// through the end of frame N+1: the call at the end of frame N moves
		// them from ThisFrame -> LastFrame, and the call at the end of
		// frame N+1 finally releases LastFrame. This window is wide enough
		// that any ImGui draw issued before Destroy() reaches Dawn's GPU
		// before the underlying view goes away.
		g_PendingDestroyLastFrame.clear();
		g_PendingDestroyLastFrame.swap(g_PendingDestroyThisFrame);
	}

}  // namespace Index
