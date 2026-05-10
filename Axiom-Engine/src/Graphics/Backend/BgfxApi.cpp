#include "pch.hpp"
#include "Graphics/RenderApi.hpp"

#include "Core/Application.hpp"
#include "Core/Log.hpp"
#include "Core/Window.hpp"
#include "Graphics/Framebuffer.hpp"
#include "Graphics/GLInitSpecifications.hpp"
#include "Project/AxiomProject.hpp"
#include "Project/ProjectManager.hpp"

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// =============================================================================
// bgfx backend — Stage 1 of the bgfx port.
// -----------------------------------------------------------------------------
// Implements the same RenderApi static surface as Backend/OpenGLApi.cpp using
// bgfx primitives. Only one of the two .cpp files is compiled per-build; the
// `--rhi=bgfx` premake option swaps which one this is. Engine code calling
// into RenderApi is unchanged either way.
//
// Stage 1 scope — minimum viable bgfx integration:
//   * `bgfx::init` succeeds with auto-detected renderer (D3D11 on Windows,
//     Metal on macOS, Vulkan on Linux).
//   * Per-frame `bgfx::touch` + `bgfx::frame` keeps the device alive.
//   * `Clear` / `SetClearColor` / `SetViewport` / `SetScissor` mapped onto
//     the equivalent `bgfx::setViewClear` / `setViewRect` / `setViewScissor`
//     calls.
//   * Every other state call (blend / cull / polygon / line width / color
//     mask / logic-op clear) is a documented no-op for now — bgfx exposes
//     them per-submission via `bgfx::setState`, not as global state, so they
//     fold into the per-resource port (Stage 2/3) when the renderers learn
//     to submit `bgfx::ProgramHandle`s.
//   * Framebuffer bind is a no-op too: `Graphics/Framebuffer.cpp` is the
//     OpenGL-flavoured class right now, and the bgfx equivalent
//     (`bgfx::FrameBufferHandle`) lands in Stage 3 alongside the FBO porting
//     of GuiRenderer / TextRenderer.
//
// All TODOs below are tracked against later sub-stages. Stage 1 is "the
// engine boots, the window opens, bgfx is alive, frame() runs."
// =============================================================================

namespace Axiom {

	namespace {
		bool        g_Initialized = false;
		std::string g_VersionString;
		std::string g_VendorString;
		std::string g_RendererString;

		Color    g_ClearColor{ 0.0f, 0.0f, 0.0f, 1.0f };
		uint32_t g_ClearColorRgba = 0x000000ffu;
		uint16_t g_BackbufferWidth = 0;
		uint16_t g_BackbufferHeight = 0;

		// View 0 is bgfx's default "swap-chain" view. Engine renderers that
		// later switch to bgfx will allocate their own view IDs (1..n) for
		// per-FBO targets.
		constexpr bgfx::ViewId k_DefaultViewId = 0;

		// All immediate state ops (Clear / SetViewport / SetScissor) target
		// `g_CurrentViewId`. Default is 0 (the swap-chain view); BindFramebuffer
		// retargets it to a per-FBO view, BindDefaultFramebuffer resets it.
		bgfx::ViewId g_CurrentViewId = k_DefaultViewId;

		// Per-FBO view-id pool. Each unique Framebuffer backend-id gets its
		// own view; freed view-ids go back into g_FreeViewIds for reuse.
		// The first BindFramebuffer for an FBO allocates + records the
		// mapping; subsequent binds look it up.
		std::unordered_map<uint32_t, bgfx::ViewId> g_FboToViewId;
		std::vector<bgfx::ViewId> g_FreeViewIds;
		bgfx::ViewId g_NextViewId = 1; // 0 reserved for default view

		// Currently-bound framebuffer handle (BGFX_INVALID_HANDLE = swap chain).
		// Tracked so renderers that allocate their own overlay views (e.g.
		// GuiRenderer's per-panel UI view) can mirror the same FBO without
		// reverse-mapping through g_FboToViewId.
		bgfx::FrameBufferHandle g_CurrentFramebuffer = BGFX_INVALID_HANDLE;

		// Per-view (x, y, w, h) cached so renderers can derive bgfx::setScissor
		// rects without round-tripping through the GPU. SetViewport keeps
		// this fresh for the currently-bound view; SetupFramebufferView seeds
		// it for FBO views at creation time. Indexed by ViewId; the array is
		// sparse for the rare case of a freed-and-reused id (zeroed there).
		// 256 = bgfx's default BGFX_CONFIG_MAX_VIEWS — the macro lives in
		// bgfx/src/config.h (private) so we hardcode the public default. If
		// the engine ever overrides the bgfx config, bump this in lockstep.
		struct ViewRect { uint16_t X, Y, W, H; };
		constexpr int k_MaxViewRects = 256;
		ViewRect g_ViewRects[k_MaxViewRects]{};

		void SetCurrentViewRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
			g_ViewRects[g_CurrentViewId] = ViewRect{ x, y, w, h };
		}
		void SetViewRectFor(bgfx::ViewId vid, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
			g_ViewRects[vid] = ViewRect{ x, y, w, h };
		}
	}

	// Allocate a fresh bgfx::ViewId — public to BgfxApi.cpp's translation
	// unit only, called by the BindFramebuffer path. Reuses a freed id if
	// available, otherwise hands out the next monotonic one. bgfx supports
	// up to BGFX_CONFIG_MAX_VIEWS (default 256), well above what the
	// editor needs (~2 FBOs in active use).
	namespace BgfxBackend {
		bgfx::ViewId AllocateViewId() {
			if (!g_FreeViewIds.empty()) {
				bgfx::ViewId id = g_FreeViewIds.back();
				g_FreeViewIds.pop_back();
				return id;
			}
			return g_NextViewId++;
		}
		void FreeViewId(bgfx::ViewId id) {
			if (id == k_DefaultViewId) return;
			g_FreeViewIds.push_back(id);
		}
		// Used by Framebuffer_Bgfx so per-Framebuffer state (clear color,
		// view rect, framebuffer binding) is set up immediately at create
		// time rather than on the first bind.
		void SetupFramebufferView(bgfx::ViewId id, bgfx::FrameBufferHandle fbo,
			uint16_t width, uint16_t height)
		{
			bgfx::setViewClear(id,
				BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
				g_ClearColorRgba, 1.0f, 0);
			bgfx::setViewRect(id, 0, 0, width, height);
			bgfx::setViewFrameBuffer(id, fbo);
			SetViewRectFor(id, 0, 0, width, height);
		}
		// Returns the currently-bound view so renderer stubs can issue
		// `bgfx::touch` against the right view-id without each one
		// having to reach into BgfxApi internals.
		bgfx::ViewId CurrentViewId() {
			return g_CurrentViewId;
		}
		// Returns the cached pixel rect (in bgfx top-left coords) of the
		// currently-bound view. Renderers project their NDC clip rects
		// through this rect to derive a per-submit bgfx::setScissor.
		void CurrentViewRect(uint16_t& outX, uint16_t& outY, uint16_t& outW, uint16_t& outH) {
			const auto& r = g_ViewRects[g_CurrentViewId];
			outX = r.X;
			outY = r.Y;
			outW = r.W;
			outH = r.H;
		}
		// Returns the framebuffer the current view targets — BGFX_INVALID_HANDLE
		// is the swap chain. Used by overlay views (UI / ImGui) that need to
		// render onto the same target without going through BindFramebuffer's
		// view-id mapping.
		bgfx::FrameBufferHandle CurrentFramebuffer() {
			return g_CurrentFramebuffer;
		}
		// Release the view-id paired with `encodedBackendId` (Framebuffer's
		// stored bgfx-handle-idx-plus-one). Called from Framebuffer::Destroy
		// so per-panel FBO recreation cycles don't leak view-ids.
		void FreeViewIdForFbo(uint32_t encodedBackendId) {
			auto it = g_FboToViewId.find(encodedBackendId);
			if (it == g_FboToViewId.end()) return;
			const bgfx::ViewId vid = it->second;
			g_FboToViewId.erase(it);
			FreeViewId(vid);
			if (g_CurrentViewId == vid) {
				g_CurrentViewId = k_DefaultViewId;
			}
		}
	}

	namespace {

		uint32_t PackRgba(const Color& c) {
			auto toByte = [](float v) -> uint32_t {
				if (v < 0.0f) v = 0.0f;
				if (v > 1.0f) v = 1.0f;
				return static_cast<uint32_t>(v * 255.0f + 0.5f);
			};
			return (toByte(c.r) << 24) | (toByte(c.g) << 16) | (toByte(c.b) << 8) | toByte(c.a);
		}

		// Push the engine's GLFW window handle into bgfx's PlatformData
		// before init. Platform-specific because bgfx wants the native HWND
		// / NSView / Display+Window — not the GLFW abstraction.
		bool PopulatePlatformData(bgfx::PlatformData& pd) {
			GLFWwindow* w = nullptr;
			if (Window* win = Application::GetWindow()) {
				w = win->GetGLFWWindow();
			}
			if (!w) {
				AIM_CORE_ERROR_TAG("BgfxApi", "No GLFW window available for bgfx::init");
				return false;
			}
#if defined(AIM_PLATFORM_WINDOWS)
			pd.nwh = glfwGetWin32Window(w);
#else
			(void)w;
			AIM_CORE_ERROR_TAG("BgfxApi", "BgfxApi platform-data is Windows-only in Stage 1");
			return false;
#endif
			pd.ndt = nullptr;
			pd.context = nullptr;
			pd.backBuffer = nullptr;
			pd.backBufferDS = nullptr;
			return true;
		}

		const char* RendererTypeName(bgfx::RendererType::Enum type) {
			switch (type) {
			case bgfx::RendererType::Direct3D11: return "Direct3D11";
			case bgfx::RendererType::Direct3D12: return "Direct3D12";
			case bgfx::RendererType::OpenGL:     return "OpenGL";
			case bgfx::RendererType::OpenGLES:   return "OpenGLES";
			case bgfx::RendererType::Vulkan:     return "Vulkan";
			case bgfx::RendererType::Metal:      return "Metal";
			case bgfx::RendererType::Noop:       return "Noop";
			default:                             return "Unknown";
			}
		}
	}

	// ── Lifecycle ──────────────────────────────────────────────────

	bool RenderApi::Init(const GLInitSpecifications& spec) {
		if (g_Initialized) return false;

		g_ClearColor = spec.ClearColor;
		g_ClearColorRgba = PackRgba(g_ClearColor);

		Window* win = Application::GetWindow();
		if (win) {
			g_BackbufferWidth  = static_cast<uint16_t>(win->GetWidth());
			g_BackbufferHeight = static_cast<uint16_t>(win->GetHeight());
		}

		bgfx::Init init{};
		// Backend selection priority:
		//   1. The current project's `ActiveBgfxBackend` setting (Player
		//      Settings → Rendering dropdown).
		//   2. `RendererType::Count` — auto-pick the most capable backend
		//      the platform supports (D3D11/Metal/Vulkan/GL depending on
		//      OS), used when the project picks "Auto" or no project is
		//      loaded yet (e.g. the launcher process before a project is
		//      opened).
		//
		// We deliberately don't fall back if the user picked an
		// unavailable backend (e.g. D3D11 on Linux). bgfx::init returns
		// false and we surface the failure in the log so the user can
		// fix their selection rather than silently dropping to a backend
		// they didn't choose.
		init.type = bgfx::RendererType::Count;
		if (const AxiomProject* project = ProjectManager::GetCurrentProject()) {
			switch (project->ActiveBgfxBackend) {
				case AxiomProject::BgfxBackend::Vulkan:
					init.type = bgfx::RendererType::Vulkan; break;
				case AxiomProject::BgfxBackend::Direct3D11:
					init.type = bgfx::RendererType::Direct3D11; break;
				case AxiomProject::BgfxBackend::Direct3D12:
					init.type = bgfx::RendererType::Direct3D12; break;
				case AxiomProject::BgfxBackend::OpenGL:
					init.type = bgfx::RendererType::OpenGL; break;
				case AxiomProject::BgfxBackend::Auto:
				default:
					init.type = bgfx::RendererType::Count; break;
			}
			AIM_CORE_INFO_TAG("BgfxApi",
				"Project requested bgfx backend: {} ({})",
				AxiomProject::BgfxBackendToString(project->ActiveBgfxBackend),
				init.type == bgfx::RendererType::Count ? "auto" : "explicit");
		}
		init.vendorId = BGFX_PCI_ID_NONE;
		init.resolution.width  = g_BackbufferWidth  > 0 ? g_BackbufferWidth  : 1280;
		init.resolution.height = g_BackbufferHeight > 0 ? g_BackbufferHeight : 720;
		init.resolution.reset  = BGFX_RESET_VSYNC;

		if (!PopulatePlatformData(init.platformData)) {
			return false;
		}

		if (!bgfx::init(init)) {
			AIM_CORE_ERROR_TAG("BgfxApi", "bgfx::init failed");
			return false;
		}

		const bgfx::RendererType::Enum rt = bgfx::getRendererType();
		const bgfx::Caps* caps = bgfx::getCaps();

		g_RendererString = RendererTypeName(rt);
		g_VendorString   = (caps && caps->vendorId == BGFX_PCI_ID_AMD)    ? "AMD"
		                 : (caps && caps->vendorId == BGFX_PCI_ID_INTEL)  ? "Intel"
		                 : (caps && caps->vendorId == BGFX_PCI_ID_NVIDIA) ? "NVIDIA"
		                 : "Unknown";
		g_VersionString  = std::string("bgfx ") + g_RendererString;

		bgfx::setViewClear(k_DefaultViewId,
			BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
			g_ClearColorRgba, 1.0f, 0);
		bgfx::setViewRect(k_DefaultViewId, 0, 0,
			init.resolution.width, init.resolution.height);
		g_CurrentViewId = k_DefaultViewId;
		SetViewRectFor(k_DefaultViewId, 0, 0,
			init.resolution.width, init.resolution.height);

		AIM_CORE_INFO_TAG("BgfxApi", "bgfx initialized — renderer={} ({}), backbuffer={}x{}",
			g_RendererString, g_VendorString,
			static_cast<int>(init.resolution.width),
			static_cast<int>(init.resolution.height));

		g_Initialized = true;
		return true;
	}

	void RenderApi::Shutdown() {
		if (!g_Initialized) return;
		bgfx::shutdown();
		g_Initialized = false;
		g_VersionString.clear();
		g_VendorString.clear();
		g_RendererString.clear();
	}

	bool RenderApi::IsInitialized() {
		return g_Initialized;
	}

	std::string_view RenderApi::BackendName() {
		return "bgfx";
	}

	const std::string& RenderApi::GetVersionString()  { return g_VersionString; }
	const std::string& RenderApi::GetVendorString()   { return g_VendorString; }
	const std::string& RenderApi::GetRendererString() { return g_RendererString; }

	// ── Per-frame state ────────────────────────────────────────────
	// bgfx is submission-driven: state isn't a global side-effect, it's part
	// of the per-draw call set via `bgfx::setState` / `bgfx::setStencil` /
	// per-view setters. Stage 1 implements just enough state to make the
	// view's clear render correctly. Per-draw state moves into the
	// renderer-side port (Stage 2/3) when Renderer2D / GuiRenderer /
	// TextRenderer / GizmoRenderer learn to submit `bgfx::ProgramHandle`s
	// instead of raw GL programs.

	void RenderApi::Clear(ClearFlags /*flags*/) {
		// `bgfx::touch` ensures the view actually executes even if no
		// draw calls were submitted — required for the clear to land
		// when a panel renders nothing this frame (e.g. before the
		// first scene loads or during a paused state). Targets the
		// currently-bound view so per-FBO panels clear independently.
		bgfx::touch(g_CurrentViewId);
	}

	void RenderApi::SetClearColor(const Color& color) {
		g_ClearColor = color;
		g_ClearColorRgba = PackRgba(color);
		bgfx::setViewClear(g_CurrentViewId,
			BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
			g_ClearColorRgba, 1.0f, 0);
	}

	Color RenderApi::GetClearColor() {
		return g_ClearColor;
	}

	void RenderApi::SetViewport(int x, int y, int width, int height) {
		const uint16_t ux = static_cast<uint16_t>(x);
		const uint16_t uy = static_cast<uint16_t>(y);
		const uint16_t uw = static_cast<uint16_t>(width > 0 ? width : 1);
		const uint16_t uh = static_cast<uint16_t>(height > 0 ? height : 1);
		bgfx::setViewRect(g_CurrentViewId, ux, uy, uw, uh);
		SetCurrentViewRect(ux, uy, uw, uh);
		// Swap-chain reset moved to OnWindowResize — relying on
		// "SetViewport targets view 0" was fragile because the editor
		// keeps an FBO bound between scene renders and ImGui-end-of-
		// frame, so a window-resize that landed mid-frame missed the
		// reset and left the swap chain at its bgfx::init resolution
		// (visible as "editor only renders into the top-left corner").
	}

	void RenderApi::OnWindowResize(int width, int height) {
		if (!g_Initialized) return;
		if (width <= 0 || height <= 0) return;
		const uint16_t uw = static_cast<uint16_t>(width);
		const uint16_t uh = static_cast<uint16_t>(height);
		if (uw == g_BackbufferWidth && uh == g_BackbufferHeight) return;

		AIM_CORE_INFO_TAG("BgfxApi",
			"OnWindowResize: {}x{} -> {}x{} (bgfx::reset)",
			g_BackbufferWidth, g_BackbufferHeight, uw, uh);

		g_BackbufferWidth  = uw;
		g_BackbufferHeight = uh;
		bgfx::reset(uw, uh, BGFX_RESET_VSYNC);
		// View 0 always covers the full swap chain — keep its rect in
		// sync so anything rendering through the default view (e.g.
		// the launcher's background-clear-only frame) doesn't keep
		// using the pre-resize rect.
		bgfx::setViewRect(k_DefaultViewId, 0, 0, uw, uh);
		SetViewRectFor(k_DefaultViewId, 0, 0, uw, uh);
	}

	void RenderApi::SetScissor(int x, int y, int width, int height) {
		bgfx::setViewScissor(g_CurrentViewId,
			static_cast<uint16_t>(x), static_cast<uint16_t>(y),
			static_cast<uint16_t>(width > 0 ? width : 0),
			static_cast<uint16_t>(height > 0 ? height : 0));
	}

	void RenderApi::EnableScissorTest() {
		// bgfx applies scissor only when setViewScissor's width/height are
		// non-zero. Toggling globally is a no-op — caller's scissor rect
		// already controls when it activates.
	}

	void RenderApi::DisableScissorTest() {
		bgfx::setViewScissor(g_CurrentViewId, 0, 0, 0, 0);
	}

	void RenderApi::EnableDepthTest()  { /* per-draw via bgfx::setState; Stage 2/3 */ }
	void RenderApi::DisableDepthTest() { /* per-draw via bgfx::setState; Stage 2/3 */ }
	void RenderApi::SetCullMode(CullMode /*mode*/)        { /* per-draw via bgfx::setState; Stage 2/3 */ }
	void RenderApi::SetBlendMode(BlendMode /*mode*/)      { /* per-draw via bgfx::setState; Stage 2/3 */ }
	void RenderApi::SetBlendingEnabled(bool /*enabled*/)  { /* per-draw via bgfx::setState; Stage 2/3 */ }
	void RenderApi::SetPolygonMode(PolygonMode /*mode*/)  { /* per-draw via BGFX_STATE_PT_LINES; Stage 2/3 */ }
	void RenderApi::SetLineWidth(float /*width*/)         { /* bgfx has no analogue; geometry-shader lines or gpu-side wide lines in Stage 2/3 */ }
	void RenderApi::SetColorMask(bool /*r*/, bool /*g*/, bool /*b*/, bool /*a*/) {
		// Per-draw via BGFX_STATE_WRITE_RGB / BGFX_STATE_WRITE_A; folded
		// into the renderers' setState calls in Stage 2/3.
	}

	void RenderApi::BeginColorLogicOpClear() {
		// Editor wireframe-overlay trick. bgfx exposes logic op via
		// BGFX_STATE_BLEND_FACTOR with a custom blend equation, but the
		// "wireframe-on-top-of-shaded" pass is itself rebuilt in Stage 3
		// when the editor's RenderSceneIntoFBO submits through bgfx
		// pipelines. For Stage 1 the wireframe pass gracefully degrades
		// to a normal pass.
	}
	void RenderApi::EndColorLogicOpClear() { /* see BeginColorLogicOpClear */ }

	// ── Framebuffer binding ────────────────────────────────────────
	// Each unique Framebuffer backend-id is paired with a bgfx::ViewId on
	// first bind. Subsequent state ops (Clear / SetViewport / SetScissor)
	// route to that view via g_CurrentViewId, so the editor's per-panel
	// FBOs render into independent bgfx views and present their own
	// clear color + (eventually) their own draw submissions. Reverting
	// to the default framebuffer points g_CurrentViewId back at view 0
	// (the swap-chain).
	//
	// Stage 2.1 only routes clears + view rect — actual draws (sprites,
	// text, gizmos) need shaders compiled by shaderc, which is the next
	// sub-stage.
	void RenderApi::BindFramebuffer(const Framebuffer& fbo) {
		const uint32_t backendId = fbo.GetBackendId();
		if (backendId == 0) {
			BindDefaultFramebuffer();
			return;
		}
		bgfx::FrameBufferHandle h{ static_cast<uint16_t>(backendId - 1u) };
		auto it = g_FboToViewId.find(backendId);
		if (it == g_FboToViewId.end()) {
			const bgfx::ViewId vid = BgfxBackend::AllocateViewId();
			g_FboToViewId.emplace(backendId, vid);
			// Set up the view's framebuffer mapping + initial clear/rect.
			// `backendId` stores (handle.idx + 1) per Framebuffer_Bgfx's
			// encoding — decode it back for setViewFrameBuffer.
			BgfxBackend::SetupFramebufferView(vid, h,
				static_cast<uint16_t>(fbo.GetWidth()),
				static_cast<uint16_t>(fbo.GetHeight()));
			g_CurrentViewId = vid;
			g_CurrentFramebuffer = h;
			return;
		}
		g_CurrentViewId = it->second;
		g_CurrentFramebuffer = h;
	}

	void RenderApi::BindDefaultFramebuffer() {
		g_CurrentViewId = k_DefaultViewId;
		g_CurrentFramebuffer = BGFX_INVALID_HANDLE;
	}

} // namespace Axiom
