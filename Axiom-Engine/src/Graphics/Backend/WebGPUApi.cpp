#include "pch.hpp"
#include "Graphics/RenderApi.hpp"

#include "Core/Application.hpp"
#include "Core/Log.hpp"
#include "Core/Window.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/Framebuffer.hpp"
#include "Graphics/GLInitSpecifications.hpp"

#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <cstring>
#include <string>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#if defined(AIM_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <Windows.h>
#endif

// =============================================================================
// WebGPU backend (Dawn) — Stage 1 of the WebGPU port.
// -----------------------------------------------------------------------------
// Implements the RenderApi static surface against Google's Dawn WebGPU
// implementation. Selected at premake time via `--rhi=webgpu`; in that mode
// this file is compiled and Backend/BgfxApi.cpp is excluded (see
// Axiom-Engine/premake5.lua's RHI swap). Engine code calling into RenderApi
// is unchanged either way.
//
// Stage 1 scope — minimum viable WebGPU integration:
//   * wgpu::Instance / Adapter / Device / Queue creation, with a Win32 HWND
//     surface from the engine's GLFW window. macOS (NSView) + Linux (X11/
//     Wayland) come in Stage 1b alongside platform-data parity with
//     BgfxApi.cpp's PopulatePlatformData.
//   * Per-frame command encoder, surface-texture acquisition, swap-chain
//     present via Present() — invoked from Window::SwapBuffers in place of
//     bgfx::frame(). The encoder is lazy-created on first use this frame so
//     "no-op frames" (engine paused, window minimized) don't pay for an
//     empty submit.
//   * Clear / SetClearColor / SetViewport / SetScissor / OnWindowResize
//     dispatch onto the equivalent WebGPU primitives. Clear opens a load-
//     op-clear render pass on the current target and closes it immediately
//     — same role bgfx::touch plays under the bgfx backend.
//   * Every other state call (blend / cull / polygon / line width / color
//     mask / logic-op clear) is a documented no-op for Stage 1. WebGPU
//     expresses those per-pipeline (not as global state), so they fold
//     into the per-renderer port (Stage 2+) when Renderer2D / GizmoRenderer
//     / TextRenderer learn to build wgpu::RenderPipelines.
//   * BindFramebuffer is a stub that just records the target — Framebuffer
//     itself is still bgfx-flavoured in Stage 1; Stage 3 ports it to a
//     wgpu::Texture-backed render-target wrapper so this routing becomes
//     meaningful.
//
// Frame lifecycle (differs from bgfx by design):
//   - bgfx is submission-driven: per-view state set, draws queued, one
//     bgfx::frame() at end-of-frame flushes everything.
//   - WebGPU is immediate-recording: a wgpu::CommandEncoder records render
//     passes, then wgpu::Queue::Submit() executes them. Each pass is
//     scoped (BeginRenderPass / End) and must be open to record draws.
// We hide this with a small lazy-init helper (EnsureFrameEncoder /
// EnsureSurfaceTexture) so the call-site model in the engine — "set state,
// clear, draw, present" — keeps working without rewriting the renderers
// in Stage 1.
//
// Dawn API version assumption: webgpu_cpp.h as shipped by Dawn ~2024 H2 or
// newer. Specifically expects:
//   * wgpu::SurfaceSourceWindowsHWND chained-struct (older name was
//     SurfaceDescriptorFromWindowsHWND — if your Dawn checkout is from
//     before that rename, the type lives under that older name).
//   * wgpu::SurfaceCapabilities + Surface::GetCapabilities.
//   * RequestAdapter / RequestDevice with the wgpu::CallbackMode +
//     Future / WaitAny pattern (added 2024).
//   * SetUncapturedErrorCallback / SetDeviceLostCallback on
//     wgpu::DeviceDescriptor.
// If the Dawn checkout post-dates further API churn, the symbol names in
// this file are the surface to update — RenderApi.hpp is backend-neutral
// and doesn't change.
// =============================================================================

namespace Axiom {

	namespace {
		// ── Backend lifecycle state (mirror of BgfxApi.cpp's globals) ───────
		bool        g_Initialized = false;
		std::string g_VersionString;
		std::string g_VendorString;
		std::string g_RendererString;

		Color    g_ClearColor{ 0.0f, 0.0f, 0.0f, 1.0f };
		uint32_t g_BackbufferWidth = 0;
		uint32_t g_BackbufferHeight = 0;

		// ── WebGPU objects ──────────────────────────────────────────────────
		wgpu::Instance      g_Instance;
		wgpu::Adapter       g_Adapter;
		wgpu::Device        g_Device;
		wgpu::Queue         g_Queue;
		wgpu::Surface       g_Surface;
		wgpu::TextureFormat g_SurfaceFormat = wgpu::TextureFormat::Undefined;

		// ── Per-frame transient state ───────────────────────────────────────
		// The encoder + surface view are lazy-created on first use this frame
		// (Clear / SetClearColor / future draws) and torn down by Present().
		// Tracking HasActivePass lets us coalesce subsequent state ops onto
		// the same pass when possible — Stage 1 only opens clear-only passes
		// so the boolean stays mostly false, but Stage 2+ renderers will keep
		// a pass open across multiple draws.
		struct FrameState {
			wgpu::CommandEncoder    Encoder;
			wgpu::Texture           SurfaceTexture;
			wgpu::TextureView       SurfaceView;
			wgpu::RenderPassEncoder ActivePass;
			bool HasEncoder = false;
			bool HasSurfaceTexture = false;
			bool HasActivePass = false;
			// At least one render pass executed against the swap chain this
			// frame? If false at Present() time, we issue a "touch" clear so
			// the swap-chain frame advances regardless — mirrors bgfx::touch.
			bool PresentedSwapChain = false;
		};
		FrameState g_Frame;

		// Currently-bound render target. Stage 2 added FBO routing via
		// WebGPUBackend::LookupFramebufferByFboId, so non-swap-chain targets
		// now resolve to real wgpu::TextureViews. The DepthView is kept
		// here so Clear can attach + clear depth alongside colour on FBO
		// targets that include a depth attachment.
		struct TargetState {
			wgpu::TextureView ColorView;       // null -> use swap chain's per-frame view
			wgpu::TextureView DepthView;       // null -> no depth attachment
			wgpu::TextureFormat ColorFormat = wgpu::TextureFormat::Undefined;
			bool IsSwapChain = true;
			uint32_t Width  = 0;
			uint32_t Height = 0;
		};
		TargetState g_CurrentTarget;

		// Cached viewport / scissor (in pixels, top-left origin). Applied at
		// render-pass-start time in Stage 2+ when actual draws run; Stage 1
		// just caches the values.
		uint32_t g_ViewportX = 0, g_ViewportY = 0, g_ViewportW = 0, g_ViewportH = 0;
		uint32_t g_ScissorX  = 0, g_ScissorY  = 0, g_ScissorW  = 0, g_ScissorH  = 0;
		bool     g_ScissorActive = false;

		// ── Helpers ─────────────────────────────────────────────────────────

		const char* AdapterTypeName(wgpu::AdapterType t) {
			switch (t) {
				case wgpu::AdapterType::DiscreteGPU:   return "DiscreteGPU";
				case wgpu::AdapterType::IntegratedGPU: return "IntegratedGPU";
				case wgpu::AdapterType::CPU:           return "CPU";
				default:                               return "Unknown";
			}
		}

		const char* BackendTypeName(wgpu::BackendType t) {
			switch (t) {
				case wgpu::BackendType::D3D11:   return "Direct3D11";
				case wgpu::BackendType::D3D12:   return "Direct3D12";
				case wgpu::BackendType::Vulkan:  return "Vulkan";
				case wgpu::BackendType::Metal:   return "Metal";
				case wgpu::BackendType::OpenGL:  return "OpenGL";
				case wgpu::BackendType::OpenGLES:return "OpenGLES";
				case wgpu::BackendType::Null:    return "Null";
				default:                         return "Unknown";
			}
		}

		// Convert wgpu::StringView (Dawn's non-null-terminated string view)
		// into std::string. StringView has .data + .length; copying is the
		// only safe option because the underlying buffer is callee-owned.
		std::string FromStringView(wgpu::StringView sv) {
			if (sv.data == nullptr || sv.length == 0) return {};
			return std::string(sv.data, sv.length);
		}

		// Drain Dawn's internal event loop until predicate returns true.
		// Native WebGPU's adapter / device requests are async by spec; on
		// Dawn we satisfy them synchronously via WaitAny + the WaitAnyOnly
		// callback mode below, but Instance::ProcessEvents is the equivalent
		// for fire-and-forget paths (validation errors arriving out-of-band,
		// for instance).
		void PumpEvents() {
			if (g_Instance) g_Instance.ProcessEvents();
		}

		bool CreateSurface() {
			Window* win = Application::GetWindow();
			if (!win) {
				AIM_CORE_ERROR_TAG("WebGPUApi", "No engine window available for surface creation");
				return false;
			}
			GLFWwindow* w = win->GetGLFWWindow();
			if (!w) {
				AIM_CORE_ERROR_TAG("WebGPUApi", "GLFW window handle is null");
				return false;
			}

#if defined(AIM_PLATFORM_WINDOWS)
			wgpu::SurfaceSourceWindowsHWND hwndSource{};
			hwndSource.hwnd      = glfwGetWin32Window(w);
			hwndSource.hinstance = ::GetModuleHandleW(nullptr);

			wgpu::SurfaceDescriptor surfaceDesc{};
			surfaceDesc.nextInChain = &hwndSource;

			g_Surface = g_Instance.CreateSurface(&surfaceDesc);
#else
			AIM_CORE_ERROR_TAG("WebGPUApi",
				"WebGPU backend platform-data is Windows-only in Stage 1 (macOS NSView + Linux X11/Wayland land in Stage 1b)");
			return false;
#endif
			if (!g_Surface) {
				AIM_CORE_ERROR_TAG("WebGPUApi", "wgpu::Instance::CreateSurface returned null");
				return false;
			}
			return true;
		}

		bool RequestAdapterSync() {
			wgpu::RequestAdapterOptions opts{};
			opts.compatibleSurface = g_Surface;
			opts.powerPreference   = wgpu::PowerPreference::HighPerformance;

			struct AdapterCtx { wgpu::Adapter Adapter; std::string Error; };
			AdapterCtx ctx;

			wgpu::Future future = g_Instance.RequestAdapter(
				&opts,
				wgpu::CallbackMode::WaitAnyOnly,
				[&ctx](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView msg) {
					if (status == wgpu::RequestAdapterStatus::Success) {
						ctx.Adapter = std::move(adapter);
					} else {
						ctx.Error = FromStringView(msg);
					}
				});

			wgpu::FutureWaitInfo wait{ future };
			g_Instance.WaitAny(1, &wait, /*timeoutNS*/ UINT64_MAX);

			if (!ctx.Adapter) {
				AIM_CORE_ERROR_TAG("WebGPUApi", "RequestAdapter failed: {}",
					ctx.Error.empty() ? "no compatible GPU" : ctx.Error);
				return false;
			}
			g_Adapter = std::move(ctx.Adapter);
			return true;
		}

		bool RequestDeviceSync() {
			wgpu::DeviceDescriptor desc{};
			// Surface-the-uncaptured-error-callback so validation failures
			// land in the engine log instead of vanishing. Dawn calls this
			// for any wgpu validation error that isn't tied to a specific
			// future (e.g. binding a wrong-format texture mid-pass).
			desc.SetUncapturedErrorCallback(
				[](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
					const char* kind = "Unknown";
					switch (type) {
						case wgpu::ErrorType::Validation:  kind = "Validation";  break;
						case wgpu::ErrorType::OutOfMemory: kind = "OutOfMemory"; break;
						case wgpu::ErrorType::Internal:    kind = "Internal";    break;
						default: break;
					}
					AIM_CORE_ERROR_TAG("WebGPUApi", "WebGPU [{}]: {}",
						kind, FromStringView(msg));
				});
			// Device-lost is recoverable in principle (re-request, re-create
			// resources) but Stage 1 just logs and lets the next frame fail.
			// Stage 6 of the port (general robustness) handles re-init.
			desc.SetDeviceLostCallback(
				wgpu::CallbackMode::AllowSpontaneous,
				[](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
					AIM_CORE_ERROR_TAG("WebGPUApi", "Device lost ({}): {}",
						static_cast<int>(reason), FromStringView(msg));
				});

			struct DeviceCtx { wgpu::Device Device; std::string Error; };
			DeviceCtx ctx;

			wgpu::Future future = g_Adapter.RequestDevice(
				&desc,
				wgpu::CallbackMode::WaitAnyOnly,
				[&ctx](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView msg) {
					if (status == wgpu::RequestDeviceStatus::Success) {
						ctx.Device = std::move(device);
					} else {
						ctx.Error = FromStringView(msg);
					}
				});

			wgpu::FutureWaitInfo wait{ future };
			g_Instance.WaitAny(1, &wait, UINT64_MAX);

			if (!ctx.Device) {
				AIM_CORE_ERROR_TAG("WebGPUApi", "RequestDevice failed: {}",
					ctx.Error.empty() ? "unknown" : ctx.Error);
				return false;
			}
			g_Device = std::move(ctx.Device);
			g_Queue  = g_Device.GetQueue();
			return true;
		}

		void ConfigureSurface(uint32_t width, uint32_t height) {
			wgpu::SurfaceCapabilities caps{};
			g_Surface.GetCapabilities(g_Adapter, &caps);

			// First reported format is the adapter's preferred swap-chain
			// format. Dawn typically returns BGRA8Unorm on Windows / Linux
			// and BGRA8Unorm-srgb on macOS — both are RenderAttachment-able.
			g_SurfaceFormat = (caps.formatCount > 0)
				? caps.formats[0]
				: wgpu::TextureFormat::BGRA8Unorm;

			wgpu::SurfaceConfiguration config{};
			config.device      = g_Device;
			config.format      = g_SurfaceFormat;
			config.usage       = wgpu::TextureUsage::RenderAttachment;
			config.width       = width;
			config.height      = height;
			config.presentMode = wgpu::PresentMode::Fifo;       // V-sync; matches bgfx default
			config.alphaMode   = wgpu::CompositeAlphaMode::Opaque;
			config.viewFormatCount = 0;

			g_Surface.Configure(&config);

			g_BackbufferWidth  = width;
			g_BackbufferHeight = height;

			// Initial render target = swap chain.
			g_CurrentTarget = TargetState{};
			g_CurrentTarget.IsSwapChain = true;
			g_CurrentTarget.Width  = width;
			g_CurrentTarget.Height = height;

			// Viewport defaults to full surface so callers don't have to
			// set one before the first Clear.
			g_ViewportX = 0; g_ViewportY = 0;
			g_ViewportW = width; g_ViewportH = height;
		}

		// ── Frame lifecycle ─────────────────────────────────────────────────

		void EnsureFrameEncoder() {
			if (g_Frame.HasEncoder) return;
			wgpu::CommandEncoderDescriptor desc{};
			g_Frame.Encoder = g_Device.CreateCommandEncoder(&desc);
			g_Frame.HasEncoder = true;
		}

		bool EnsureSurfaceTexture() {
			if (g_Frame.HasSurfaceTexture) return true;

			wgpu::SurfaceTexture surfaceTex{};
			g_Surface.GetCurrentTexture(&surfaceTex);

			// SuboptimalConfiguration is non-fatal — the surface still has a
			// usable texture, we just want to re-configure on next resize.
			const bool ok =
				surfaceTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
				surfaceTex.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal;
			if (!ok) {
				AIM_CORE_WARN_TAG("WebGPUApi",
					"GetCurrentTexture: status={} — skipping frame",
					static_cast<int>(surfaceTex.status));
				return false;
			}

			g_Frame.SurfaceTexture = wgpu::Texture(surfaceTex.texture);

			wgpu::TextureViewDescriptor viewDesc{};
			viewDesc.format          = g_SurfaceFormat;
			viewDesc.dimension       = wgpu::TextureViewDimension::e2D;
			viewDesc.mipLevelCount   = 1;
			viewDesc.arrayLayerCount = 1;
			viewDesc.aspect          = wgpu::TextureAspect::All;
			g_Frame.SurfaceView = g_Frame.SurfaceTexture.CreateView(&viewDesc);

			g_Frame.HasSurfaceTexture = true;
			return true;
		}

		void EndActivePassIfAny() {
			if (!g_Frame.HasActivePass) return;
			g_Frame.ActivePass.End();
			g_Frame.ActivePass = nullptr;
			g_Frame.HasActivePass = false;
		}
	}  // anonymous namespace

	// ── Backend-internal hooks (consumed by Window::SwapBuffers) ─────────────
	// Mirrors the BgfxBackend:: helper namespace used by the bgfx backend so
	// engine code outside this TU (Window.cpp, the future imgui_impl_wgpu
	// integration) has a typed surface to call into. Symbols are namespaced
	// so they can't collide with the bgfx backend's helpers in mixed-port
	// stages.
	namespace WebGPUBackend {
		// End-of-frame: close any active pass, submit the command buffer,
		// present the surface. Called from Window::SwapBuffers in place of
		// bgfx::touch(0) + bgfx::frame(). Safe to call when nothing was
		// recorded this frame (issues a "touch" clear so the swap-chain
		// frame still advances — same role as bgfx::touch).
		void Present() {
			if (!g_Initialized) return;

			// Touch-fallback: if no render pass ran this frame, do a
			// clear-only pass on the swap-chain so the surface still
			// advances and the visible result matches g_ClearColor.
			// Mirrors bgfx::touch(0) semantics from BgfxApi.cpp Stage 1.
			if (!g_Frame.PresentedSwapChain) {
				EnsureFrameEncoder();
				if (EnsureSurfaceTexture()) {
					wgpu::RenderPassColorAttachment colorAtt{};
					colorAtt.view       = g_Frame.SurfaceView;
					colorAtt.loadOp     = wgpu::LoadOp::Clear;
					colorAtt.storeOp    = wgpu::StoreOp::Store;
					colorAtt.clearValue = wgpu::Color{
						g_ClearColor.r, g_ClearColor.g, g_ClearColor.b, g_ClearColor.a };
					colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

					wgpu::RenderPassDescriptor passDesc{};
					passDesc.colorAttachmentCount = 1;
					passDesc.colorAttachments     = &colorAtt;

					wgpu::RenderPassEncoder pass = g_Frame.Encoder.BeginRenderPass(&passDesc);
					pass.End();
					g_Frame.PresentedSwapChain = true;
				}
			} else {
				EndActivePassIfAny();
			}

			if (g_Frame.HasEncoder) {
				wgpu::CommandBufferDescriptor cbDesc{};
				wgpu::CommandBuffer cmd = g_Frame.Encoder.Finish(&cbDesc);
				g_Queue.Submit(1, &cmd);
			}

			if (g_Frame.HasSurfaceTexture) {
				g_Surface.Present();
			}

			// Reset frame transient state — releases the encoder, the
			// surface view, and the surface texture handle. Next frame
			// re-acquires them lazily on first use.
			g_Frame = FrameState{};

			// Pump Dawn's internal callbacks (validation errors arriving
			// after submit, device-lost reasons, etc.) so they get logged
			// in time for the next frame instead of stacking up.
			PumpEvents();
		}

		// Returns the active wgpu::Device — exposed so per-resource
		// _WebGPU.cpp ports (Texture2D, Framebuffer, future Shader /
		// RenderPipeline, ...) can create GPU objects through the engine's
		// single device without smuggling it via a Singleton or stuffing
		// it onto Application. Same role as BgfxBackend::CurrentViewId
		// in BgfxApi.cpp. Declared in Backend/WebGPUBackend.hpp.
		wgpu::Device GetDevice() { return g_Device; }
		wgpu::Queue  GetQueue()  { return g_Queue; }
		wgpu::TextureFormat GetSurfaceFormat() { return g_SurfaceFormat; }
		// Returns true once Init has completed successfully — resource
		// constructors check this before touching wgpu objects so a
		// pre-engine-init Texture2D / Framebuffer (e.g. one constructed
		// during static initialisation of a global asset registry) bails
		// gracefully instead of crashing inside Dawn.
		bool IsInitialized() { return g_Initialized; }

		// ── Render-pass plumbing for renderer-side BeginRenderPass ──────
		// Declared in Backend/WebGPUBackend.hpp; see the header for the
		// rationale (renderers drive their own passes, backend owns the
		// frame-wide encoder + swap-chain surface acquisition).

		wgpu::CommandEncoder GetFrameEncoder() {
			if (!g_Initialized) return nullptr;
			EnsureFrameEncoder();
			return g_Frame.Encoder;
		}

		CurrentTargetInfo BeginRenderToCurrentTarget() {
			CurrentTargetInfo out;
			if (!g_Initialized) return out;

			EnsureFrameEncoder();

			if (g_CurrentTarget.IsSwapChain) {
				if (!EnsureSurfaceTexture()) return out;
				out.ColorView   = g_Frame.SurfaceView;
				out.DepthView   = nullptr;
				out.ColorFormat = g_SurfaceFormat;
				out.HasDepth    = false;
				out.IsSwapChain = true;
				out.Width       = g_BackbufferWidth;
				out.Height      = g_BackbufferHeight;
			} else {
				if (!g_CurrentTarget.ColorView) return out;
				out.ColorView   = g_CurrentTarget.ColorView;
				out.DepthView   = g_CurrentTarget.DepthView;
				out.ColorFormat = g_CurrentTarget.ColorFormat;
				out.HasDepth    = static_cast<bool>(g_CurrentTarget.DepthView);
				out.IsSwapChain = false;
				out.Width       = g_CurrentTarget.Width;
				out.Height      = g_CurrentTarget.Height;
			}

			// If WebGPUApi.cpp's Clear path left an active clear-only pass
			// open (it doesn't today — Clear opens + closes inline — but
			// renderer-side code should still be defensive against future
			// refactors), end it so the renderer's pass starts on a clean
			// state.
			EndActivePassIfAny();

			out.Valid = true;
			return out;
		}

		void MarkSwapChainRendered() {
			g_Frame.PresentedSwapChain = true;
		}
	}

	// ── RenderApi: Lifecycle ────────────────────────────────────────────────

	bool RenderApi::Init(const GLInitSpecifications& spec) {
		if (g_Initialized) return false;

#if defined(AIM_PLATFORM_WINDOWS)
		// Dawn's `DynamicLib::Open` (src/dawn/common/DynamicLib.cpp) calls
		// `LoadLibraryExA(name, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
		// LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`. On Windows 11 24H2 the
		// combination of these flags with a bare-name (no directory) DLL
		// argument can return ERROR_INVALID_PARAMETER (87) even after
		// SetDefaultDllDirectories has enabled safe-search mode -- the
		// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR flag has no well-defined
		// "DLL load dir" when the name has no path prefix.
		//
		// Two-step workaround:
		//   1. SetDefaultDllDirectories enables the safe-search mode so
		//      Dawn's LOAD_LIBRARY_SEARCH_* flags are accepted by the
		//      loader (this is necessary on its own; without it, Dawn's
		//      flag combo always fails).
		//   2. Preload d3dcompiler_47.dll + vulkan-1.dll from System32
		//      via plain LoadLibraryW. Once loaded into the process,
		//      Dawn's subsequent LoadLibraryEx calls for the same name
		//      short-circuit to "already loaded" and return the existing
		//      module handle -- bypassing whatever flag path was failing.
		//      vulkan-1.dll is optional (Vulkan SDK isn't always installed);
		//      d3dcompiler_47.dll ships with Windows so this should always
		//      succeed via the default DLL search.
		const BOOL didSetDirs = ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		const HMODULE d3dCompiler = ::LoadLibraryW(L"d3dcompiler_47.dll");
		const HMODULE vulkan      = ::LoadLibraryW(L"vulkan-1.dll");
		AIM_CORE_INFO_TAG("WebGPUApi",
			"DLL preload: SetDefaultDllDirectories={} (last-error={}), d3dcompiler_47={}, vulkan-1={}",
			didSetDirs ? "ok" : "FAIL",
			didSetDirs ? 0u : static_cast<uint32_t>(::GetLastError()),
			d3dCompiler ? "ok" : "FAIL",
			vulkan ? "ok" : "missing (Vulkan SDK not installed; D3D will be used)");
#endif

		g_ClearColor = spec.ClearColor;

		Window* win = Application::GetWindow();
		if (win) {
			g_BackbufferWidth  = static_cast<uint32_t>(win->GetWidth());
			g_BackbufferHeight = static_cast<uint32_t>(win->GetHeight());
		}
		if (g_BackbufferWidth == 0)  g_BackbufferWidth  = 1280;
		if (g_BackbufferHeight == 0) g_BackbufferHeight = 720;

		// Create the wgpu::Instance. We MUST opt into TimedWaitAny here:
		// RequestAdapterSync / RequestDeviceSync below call
		// `g_Instance.WaitAny(future, UINT64_MAX)` to block until the
		// adapter/device callback fires. Without the TimedWaitAny feature
		// on the instance, any non-zero timeout argument to WaitAny is
		// rejected with "Timeout waits are either not enabled or not
		// supported." and the request silently fails to complete.
		const wgpu::InstanceFeatureName requiredFeatures[] = {
			wgpu::InstanceFeatureName::TimedWaitAny,
		};
		wgpu::InstanceDescriptor instanceDesc{};
		instanceDesc.requiredFeatureCount = 1;
		instanceDesc.requiredFeatures     = requiredFeatures;
		g_Instance = wgpu::CreateInstance(&instanceDesc);
		if (!g_Instance) {
			AIM_CORE_ERROR_TAG("WebGPUApi", "wgpu::CreateInstance failed");
			return false;
		}

		if (!CreateSurface())       { Shutdown(); return false; }
		if (!RequestAdapterSync())  { Shutdown(); return false; }
		if (!RequestDeviceSync())   { Shutdown(); return false; }
		ConfigureSurface(g_BackbufferWidth, g_BackbufferHeight);

		// Adapter info -> About / Stats overlay strings. The native backend
		// (D3D12 / Vulkan / Metal) Dawn picked is informational here — the
		// per-project "preferred backend" knob comes in Stage 5.
		wgpu::AdapterInfo info{};
		g_Adapter.GetInfo(&info);
		g_VendorString   = FromStringView(info.vendor);
		g_RendererString = FromStringView(info.device);
		g_VersionString  = std::string("WebGPU (Dawn) / ") + BackendTypeName(info.backendType)
			+ " / " + AdapterTypeName(info.adapterType);

		AIM_CORE_INFO_TAG("WebGPUApi",
			"WebGPU initialized — adapter='{}' vendor='{}' backend={} type={} backbuffer={}x{} format={}",
			g_RendererString, g_VendorString,
			BackendTypeName(info.backendType), AdapterTypeName(info.adapterType),
			g_BackbufferWidth, g_BackbufferHeight,
			static_cast<int>(g_SurfaceFormat));

		g_Initialized = true;
		return true;
	}

	void RenderApi::Present() {
		// Routes to the backend-internal hook so the implementation can
		// live in the anonymous-namespace state above without leaking
		// FrameState's layout into the header.
		WebGPUBackend::Present();
	}

	void RenderApi::Shutdown() {
		if (!g_Initialized && !g_Instance) return;

		// Drop frame transient state first so no in-flight pass / encoder
		// outlives the device.
		g_Frame = FrameState{};

		// Releasing in reverse-init order keeps Dawn's internal validation
		// happy (queue belongs to device; surface belongs to instance; etc.)
		if (g_Surface) g_Surface.Unconfigure();
		g_CurrentTarget = TargetState{};
		g_Queue   = nullptr;
		g_Device  = nullptr;
		g_Adapter = nullptr;
		g_Surface = nullptr;
		g_Instance = nullptr;

		g_VersionString.clear();
		g_VendorString.clear();
		g_RendererString.clear();
		g_SurfaceFormat = wgpu::TextureFormat::Undefined;

		g_Initialized = false;
	}

	bool RenderApi::IsInitialized() {
		return g_Initialized;
	}

	std::string_view RenderApi::BackendName() {
		return "webgpu";
	}

	const std::string& RenderApi::GetVersionString()  { return g_VersionString; }
	const std::string& RenderApi::GetVendorString()   { return g_VendorString; }
	const std::string& RenderApi::GetRendererString() { return g_RendererString; }

	// ── Per-frame state ─────────────────────────────────────────────────────

	void RenderApi::Clear(ClearFlags /*flags*/) {
		// A Clear runs a clear-only render pass on the current target. The
		// target's colour view is either the per-frame swap-chain surface
		// view (lazy-acquired) or an FBO's persistent colour view (looked
		// up by BindFramebuffer). If the target has a depth attachment
		// (FBOs always do), it's cleared alongside — matches the bgfx side
		// where every FBO ships with a D24S8 attachment and BGFX_CLEAR_DEPTH
		// is part of the view's clear flags.
		EnsureFrameEncoder();

		wgpu::TextureView targetColorView;
		if (g_CurrentTarget.IsSwapChain) {
			if (!EnsureSurfaceTexture()) return;
			targetColorView = g_Frame.SurfaceView;
		} else {
			targetColorView = g_CurrentTarget.ColorView;
			if (!targetColorView) {
				AIM_CORE_WARN_TAG("WebGPUApi", "Clear on FBO target with no colour view bound");
				return;
			}
		}

		EndActivePassIfAny();

		wgpu::RenderPassColorAttachment colorAtt{};
		colorAtt.view       = targetColorView;
		colorAtt.loadOp     = wgpu::LoadOp::Clear;
		colorAtt.storeOp    = wgpu::StoreOp::Store;
		colorAtt.clearValue = wgpu::Color{
			g_ClearColor.r, g_ClearColor.g, g_ClearColor.b, g_ClearColor.a };
		colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

		wgpu::RenderPassDescriptor passDesc{};
		passDesc.colorAttachmentCount = 1;
		passDesc.colorAttachments     = &colorAtt;

		// Depth/stencil attachment — present only when the target is an FBO
		// with a depth view. Swap-chain passes don't include depth here
		// because Stage 1's swap-chain configuration doesn't attach one;
		// when 3D scene rendering lands (Stage 5+) the swap chain gets a
		// matching depth target.
		wgpu::RenderPassDepthStencilAttachment depthAtt{};
		if (!g_CurrentTarget.IsSwapChain && g_CurrentTarget.DepthView) {
			depthAtt.view              = g_CurrentTarget.DepthView;
			depthAtt.depthLoadOp       = wgpu::LoadOp::Clear;
			depthAtt.depthStoreOp      = wgpu::StoreOp::Store;
			depthAtt.depthClearValue   = 1.0f;
			depthAtt.stencilLoadOp     = wgpu::LoadOp::Clear;
			depthAtt.stencilStoreOp    = wgpu::StoreOp::Store;
			depthAtt.stencilClearValue = 0;
			passDesc.depthStencilAttachment = &depthAtt;
		}

		wgpu::RenderPassEncoder pass = g_Frame.Encoder.BeginRenderPass(&passDesc);
		pass.End();

		if (g_CurrentTarget.IsSwapChain) {
			g_Frame.PresentedSwapChain = true;
		}
	}

	void RenderApi::SetClearColor(const Color& color) {
		g_ClearColor = color;
		// No GPU work — value picked up on next Clear / next render pass.
	}

	Color RenderApi::GetClearColor() {
		return g_ClearColor;
	}

	void RenderApi::SetViewport(int x, int y, int width, int height) {
		g_ViewportX = static_cast<uint32_t>(x < 0 ? 0 : x);
		g_ViewportY = static_cast<uint32_t>(y < 0 ? 0 : y);
		g_ViewportW = static_cast<uint32_t>(width  > 0 ? width  : 1);
		g_ViewportH = static_cast<uint32_t>(height > 0 ? height : 1);
		// SetViewport in WebGPU is a per-pass op (RenderPassEncoder::SetViewport).
		// We just cache the rect here — Stage 2+ renderers apply it on their
		// SetPipeline + SetViewport pair before each batched draw. Apply-now
		// is impossible: there's no implicit pass to set it on.
	}

	void RenderApi::OnWindowResize(int width, int height) {
		if (!g_Initialized) return;
		if (width <= 0 || height <= 0) return;

		const uint32_t uw = static_cast<uint32_t>(width);
		const uint32_t uh = static_cast<uint32_t>(height);
		if (uw == g_BackbufferWidth && uh == g_BackbufferHeight) return;

		AIM_CORE_INFO_TAG("WebGPUApi",
			"OnWindowResize: {}x{} -> {}x{} (surface.Configure)",
			g_BackbufferWidth, g_BackbufferHeight, uw, uh);

		// Any in-flight surface texture is now invalid — Dawn detects the
		// stale view at submit time but explicit cleanup avoids the
		// validation chatter.
		g_Frame = FrameState{};

		ConfigureSurface(uw, uh);
	}

	void RenderApi::SetScissor(int x, int y, int width, int height) {
		g_ScissorX = static_cast<uint32_t>(x < 0 ? 0 : x);
		g_ScissorY = static_cast<uint32_t>(y < 0 ? 0 : y);
		g_ScissorW = static_cast<uint32_t>(width  > 0 ? width  : 0);
		g_ScissorH = static_cast<uint32_t>(height > 0 ? height : 0);
		g_ScissorActive = (g_ScissorW > 0 && g_ScissorH > 0);
		// Per-pass via RenderPassEncoder::SetScissorRect — cached here for
		// Stage 2+. WebGPU has no global "scissor disabled" state; passes
		// default to the full attachment size which matches the engine's
		// expectation when scissor isn't explicitly enabled.
	}

	void RenderApi::EnableScissorTest()  { g_ScissorActive = (g_ScissorW > 0 && g_ScissorH > 0); }
	void RenderApi::DisableScissorTest() { g_ScissorActive = false; }

	// ── Per-draw / per-pipeline state (Stage 2+ folds into pipelines) ───────
	// WebGPU expresses depth, cull, blend, polygon mode, line width, color
	// mask, and logic-op as part of the wgpu::RenderPipeline (BlendState,
	// PrimitiveState, ColorTargetState). They aren't global state at all
	// — there's no equivalent of glEnable / bgfx::setState here. The
	// renderer ports (Renderer2D / GizmoRenderer / TextRenderer) will bake
	// these into their pipelines in Stage 2; until then these are documented
	// no-ops on the WebGPU backend, matching BgfxApi.cpp's Stage 1 treatment
	// of the same ops.

	void RenderApi::EnableDepthTest()                            { /* per-pipeline via DepthStencilState; Stage 2 */ }
	void RenderApi::DisableDepthTest()                           { /* per-pipeline via DepthStencilState; Stage 2 */ }
	void RenderApi::SetCullMode(CullMode /*mode*/)               { /* per-pipeline via PrimitiveState::cullMode; Stage 2 */ }
	void RenderApi::SetBlendMode(BlendMode /*mode*/)             { /* per-pipeline via BlendState; Stage 2 */ }
	void RenderApi::SetBlendingEnabled(bool /*enabled*/)         { /* per-pipeline via ColorTargetState::blend; Stage 2 */ }
	void RenderApi::SetPolygonMode(PolygonMode /*mode*/)         { /* per-pipeline via PrimitiveState::topology; line topology covers wireframe; Stage 2 */ }
	void RenderApi::SetLineWidth(float /*width*/)                { /* WebGPU has no wide-lines feature — gpu-side wide lines or fat-line geometry shader in Stage 2 */ }
	void RenderApi::SetColorMask(bool /*r*/, bool /*g*/, bool /*b*/, bool /*a*/) {
		// Per-pipeline via ColorTargetState::writeMask; Stage 2.
	}

	// Editor wireframe-overlay (color-logic-op CLEAR trick). WebGPU has no
	// logic-op blend state at all; the equivalent in Stage 3+ is a custom
	// shader pass that writes solid black for wireframe-overlaid pixels.
	// Stage 1 gracefully degrades to a normal pass.
	void RenderApi::BeginColorLogicOpClear() { /* see EndColorLogicOpClear */ }
	void RenderApi::EndColorLogicOpClear()   { /* see Stage 3 fat-line / overlay-shader plan */ }

	// ── Framebuffer binding ────────────────────────────────────────────────
	// Resolves the FBO's opaque backend ID into the wgpu::TextureView pair
	// (colour + depth) registered by Framebuffer_WebGPU.cpp's pool. The
	// resolved views become g_CurrentTarget — Clear opens render passes
	// against them, and Stage 3+ renderers will too. An FBO with an
	// unresolved ID (no matching pool entry — shouldn't happen in normal
	// operation; would indicate use-after-destroy) falls back to the swap
	// chain with a warning, matching the BgfxApi side's graceful-degradation
	// behaviour.
	void RenderApi::BindFramebuffer(const Framebuffer& fbo) {
		const uint32_t backendId = fbo.GetBackendId();
		if (backendId == 0) {
			BindDefaultFramebuffer();
			return;
		}

		const auto lookup = WebGPUBackend::LookupFramebufferByFboId(backendId);
		if (!lookup.Valid) {
			AIM_CORE_WARN_TAG("WebGPUApi",
				"BindFramebuffer: no GPU resources for fboId={} — falling back to swap chain",
				backendId);
			BindDefaultFramebuffer();
			return;
		}

		// Close any in-flight pass first — its render-target binding is
		// about to become stale.
		EndActivePassIfAny();

		g_CurrentTarget = TargetState{};
		g_CurrentTarget.ColorView   = lookup.ColorView;
		g_CurrentTarget.DepthView   = lookup.DepthView;
		g_CurrentTarget.ColorFormat = lookup.ColorFormat;
		g_CurrentTarget.IsSwapChain = false;
		g_CurrentTarget.Width       = lookup.Width;
		g_CurrentTarget.Height      = lookup.Height;
	}

	void RenderApi::BindDefaultFramebuffer() {
		// Close any in-flight FBO pass before switching to the swap-chain.
		EndActivePassIfAny();

		g_CurrentTarget = TargetState{};
		g_CurrentTarget.IsSwapChain = true;
		g_CurrentTarget.ColorFormat = g_SurfaceFormat;
		g_CurrentTarget.Width       = g_BackbufferWidth;
		g_CurrentTarget.Height      = g_BackbufferHeight;
	}

}  // namespace Axiom
