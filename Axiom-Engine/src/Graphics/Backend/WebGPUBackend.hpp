#pragma once

// =============================================================================
// WebGPUBackend — private engine-internal interface to the WebGPU backend.
// -----------------------------------------------------------------------------
// Mirror of the BgfxBackend:: helpers defined inside Backend/BgfxApi.cpp: a
// small namespace that lets resource files (Texture2D_WebGPU.cpp,
// Framebuffer_WebGPU.cpp, future Shader_WebGPU.cpp / Renderer2D_WebGPU.cpp /
// etc.) talk to the active wgpu::Device + Queue + surface without depending on
// the singleton-style globals living in WebGPUApi.cpp's anonymous namespace.
//
// This header is ONLY safe to include from TUs compiled under --rhi=webgpu —
// it includes <webgpu/webgpu_cpp.h>, which is only on the include path when
// Dawn is wired in. Files needing backend-neutral types should use
// Graphics/RenderApi.hpp instead.
//
// Pool model: each resource type that wraps a wgpu:: handle (Texture2D,
// Framebuffer, future Shader / RenderPipeline, ...) keeps a TU-local pool of
// its GPU objects, indexed by the same opaque uint32_t IDs the resource's
// header already exposes via `GetBackendId()` / `GetHandle()`. The header
// declarations here are the LOOKUP surface — the storage stays inside each
// resource's .cpp TU so the pools can specialise (samplers per Texture2D,
// depth attachments per Framebuffer, etc.) without forcing a shared
// godclass. Callers from outside that TU (chiefly WebGPUApi.cpp's
// BindFramebuffer routing draws to a target view, and the imgui backend's
// ImGui::Image consuming a sampled texture) ask the resource's TU for a
// view + sampler.
//
// Lifetime: the wgpu objects in each pool are RAII wrappers; clearing a pool
// entry releases the underlying GPU resource immediately. The pools are
// cleared on `Texture2D::Destroy` / `Framebuffer::Destroy` (and in their
// destructors), so resource teardown is deterministic. There's no GC.
// =============================================================================

#include <webgpu/webgpu_cpp.h>

#include <cstdint>

namespace Axiom::WebGPUBackend {

	// ── Frame lifecycle (defined in Backend/WebGPUApi.cpp) ───────────────────

	// End-of-frame: close any active pass, submit the command buffer, present
	// the surface. Routed to from RenderApi::Present(). Safe to call when
	// nothing was rendered this frame — issues a touch-clear so the swap
	// chain still advances (mirror of bgfx::touch).
	void Present();

	// Accessors for resource TUs that need to create GPU objects on the same
	// device the engine initialised.
	wgpu::Device        GetDevice();
	wgpu::Queue         GetQueue();
	wgpu::TextureFormat GetSurfaceFormat();
	bool                IsInitialized();

	// ── Texture2D pool (defined in Graphics/Texture2D_WebGPU.cpp) ────────────

	// Each loaded Texture2D registers its wgpu::Texture + a sampled
	// TextureView + a Sampler matching its Filter/Wrap state. The
	// `uint64_t m_Tex` field on Texture2D is the lookup key -- it holds
	// the raw WGPUTextureView pointer so the same value can flow straight
	// through ImGui::Image into imgui_impl_wgpu as an ImTextureID without
	// needing an engine-side lookup.
	//
	// Returns null wgpu handles for unknown IDs. The renderer backends look
	// these up at submit time to bind into bind groups; ImGui's image
	// preview hands the ID to the imgui_impl_wgpu integration which
	// resolves it the same way.
	struct TextureLookup {
		wgpu::TextureView View;
		wgpu::Sampler     Sampler;
		uint32_t          Width  = 0;
		uint32_t          Height = 0;
		bool              Valid  = false;
	};
	TextureLookup LookupTexture2D(uint64_t backendId);

	// ── Framebuffer pool (defined in Graphics/Framebuffer_WebGPU.cpp) ────────

	// Each Framebuffer registers a colour attachment (RGBA8 / R8 by
	// TextureFormat enum) and a depth attachment (always D24S8) plus
	// their views. The opaque uint32_t IDs from Framebuffer's header
	// are the lookup keys.
	struct FramebufferLookup {
		wgpu::TextureView ColorView;
		wgpu::TextureView DepthView;
		wgpu::TextureFormat ColorFormat = wgpu::TextureFormat::Undefined;
		uint32_t          Width  = 0;
		uint32_t          Height = 0;
		bool              Valid  = false;
	};
	FramebufferLookup LookupFramebufferByFboId(uint32_t fboBackendId);

	// Resolve a Framebuffer's colour attachment view by the raw
	// WGPUTextureView pointer (cast to uint64_t). This is what
	// `Framebuffer::GetColorTextureBackendId()` returns under WebGPU --
	// the value IS the WGPUTextureView pointer so that ImGui's
	// imgui_impl_wgpu can `reinterpret_cast` ImTextureID -> WGPUTextureView
	// without going through an engine lookup. This function exists for
	// non-ImGui consumers that want the wgpu::TextureView wrapper back.
	wgpu::TextureView LookupFramebufferColorViewByTextureId(uint64_t colorTextureViewPtr);

	// Sampler mapping from the engine's Filter / Wrap enums to a freshly
	// built wgpu::Sampler. Exposed here because both Texture2D and the
	// future Renderer2D need to derive samplers from the same enum values,
	// and we'd rather not duplicate the switch in both call sites.
	// Implementation lives in Texture2D_WebGPU.cpp alongside the Texture2D
	// pool — same logical concern.
	enum class FilterMode : uint8_t { Point, Bilinear, Trilinear, Anisotropic };
	enum class WrapMode   : uint8_t { Repeat, Clamp, Mirror, Border };
	wgpu::Sampler CreateSampler(FilterMode filter, WrapMode wrapU, WrapMode wrapV);

	// ── Shader pool (defined in Graphics/Shader_WebGPU.cpp) ──────────────────

	// WebGPU collapses bgfx's vs+fs pair into a single wgpu::ShaderModule
	// with named entry points (vs_main / fs_main). Each Shader instance
	// holds one module; the engine's `unsigned m_Program` field is its
	// lookup key here. The renderers (Stage 4+) request the module
	// alongside the entry-point names when building a wgpu::RenderPipeline.
	struct ShaderLookup {
		wgpu::ShaderModule Module;
		bool               Valid = false;
	};
	ShaderLookup LookupShader(unsigned shaderHandleId);

	// ── Font atlas pool (defined in Graphics/Text/Font_WebGPU.cpp) ───────────

	// Each Font registers an R8Unorm atlas texture (built from stbtt_pack)
	// here. The opaque `unsigned m_AtlasTexture` field on Font is the
	// lookup key. TextRenderer_WebGPU's submit path resolves these into
	// the wgpu::TextureView it puts in the text pipeline's bind group.
	wgpu::TextureView LookupFontAtlas(unsigned atlasId);

	// ── Render-pass plumbing (defined in Backend/WebGPUApi.cpp) ──────────────
	//
	// Renderers (Renderer2D / GuiRenderer / TextRenderer / GizmoRenderer)
	// open their own wgpu::RenderPassEncoder per submit batch — bgfx's
	// submission-driven model gets translated into WebGPU's immediate-
	// recording model by having each renderer drive its own pass with
	// LoadOp::Load semantics (preserves whatever a prior RenderApi::Clear
	// painted). The backend owns the frame-wide CommandEncoder + the
	// surface-texture acquisition; the renderer asks for both via
	// `BeginRenderToCurrentTarget`.

	struct CurrentTargetInfo {
		// Color attachment view to render into. For the swap chain this is
		// the surface-texture view acquired for THIS frame; for an FBO it's
		// the persistent view registered by Framebuffer_WebGPU.cpp.
		wgpu::TextureView   ColorView;
		// Depth/stencil attachment, when the current target has one. FBOs
		// always do (D24S8); the swap chain currently does not (Stage 1's
		// surface config doesn't allocate one — Stage 5+ may revisit).
		wgpu::TextureView   DepthView;
		wgpu::TextureFormat ColorFormat = wgpu::TextureFormat::Undefined;
		uint32_t            Width      = 0;
		uint32_t            Height     = 0;
		bool                IsSwapChain= true;
		bool                HasDepth   = false;
		// Valid=false means the backend can't render this frame (not
		// initialised, surface-texture acquisition failed, etc.) and the
		// renderer should skip its submit phase entirely.
		bool                Valid      = false;
	};

	// Resolves the current bound target (swap chain or FBO) into renderable
	// info. For the swap chain, ensures the per-frame surface texture is
	// acquired; subsequent calls in the same frame reuse the cached one.
	// Returns Valid=false on any failure — caller should bail without
	// drawing rather than open a pass on a null view.
	CurrentTargetInfo BeginRenderToCurrentTarget();

	// Returns the per-frame wgpu::CommandEncoder. Lazy-created on first
	// access; renderers use it to BeginRenderPass + queue their draws.
	// Returns null when uninitialised.
	wgpu::CommandEncoder GetFrameEncoder();

	// Notify the backend that at least one render pass executed against the
	// swap chain this frame. Without this flag, `Present()`'s touch-fallback
	// would issue an extra clear-only pass on the swap chain, double-clearing
	// (and corrupting) whatever the renderer just drew. Renderers call this
	// when their pass targets `IsSwapChain == true`.
	void MarkSwapChainRendered();

}  // namespace Axiom::WebGPUBackend
