#include "pch.hpp"
#include "Gui/ImGuiImplWebGPU.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Packages/PackageImGuiBridge.hpp"

#include <webgpu/webgpu.h>
#include <webgpu/webgpu_cpp.h>
#include <imgui.h>
#include <backends/imgui_impl_wgpu.h>

// =============================================================================
// imgui_impl_wgpu wrapper — Stage 9 of the WebGPU port.
// -----------------------------------------------------------------------------
// Thin Axiom shim around Dear ImGui's official imgui_impl_wgpu backend (in
// External/imgui/backends/). Compiled only under --rhi=webgpu; the premake
// gate selects between this and ImGuiImplBgfx.cpp via AIM_RHI_WEBGPU.
//
// Why a wrapper rather than calling imgui_impl_wgpu directly from the
// editor / launcher / runtime:
//   1. wgpu::Device + Queue + per-frame command encoder live in engine.dll
//      (WebGPUApi.cpp). The backend has to call into those, so it must run
//      in the same module. The bgfx-side header documents the equivalent
//      constraint for bgfx.
//   2. PackageImGuiBridge synchronises the consumer's ImGuiContext into
//      engine.dll's ImGui copy on every entry. Without this, the engine
//      DLL would see a null GImGui pointer and crash inside ImGui::GetIO.
//   3. The ABI-stable AXIOM_API surface (`Init`, `NewFrame`, `RenderDraw-
//      Data(d, viewId)`, `Shutdown`) is identical to ImGuiImplBgfx so
//      every editor/launcher/runtime call site is backend-neutral.
//
// imgui_impl_wgpu uses Dawn's C API (`WGPUDevice`, `WGPURenderPassEncoder`).
// We pass wgpu::Device through .Get() to drop into the C ABI; equivalent
// for the pass encoder when we hand it to ImGui_ImplWGPU_RenderDrawData.
// =============================================================================

namespace Axiom::ImGuiImplWebGPU {

	namespace {
		bool g_Initialized = false;
		// Bridge generation tracking — same pattern ImGuiImplBgfx uses to
		// resync after the consumer hot-reloads its ImGuiContext.
		unsigned long long g_LastSyncedGen = 0;

		void SyncImGuiContextFromBridge() {
			const unsigned long long gen = PackageImGuiBridge::GetGeneration();
			if (gen == g_LastSyncedGen && ImGui::GetCurrentContext() != nullptr) {
				return;
			}
			void* ctx = PackageImGuiBridge::GetContext();
			if (ctx == nullptr) {
				return;
			}
			ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));
			void* allocFn = nullptr;
			void* freeFn  = nullptr;
			void* user    = nullptr;
			PackageImGuiBridge::GetAllocators(allocFn, freeFn, user);
			if (allocFn != nullptr && freeFn != nullptr) {
				ImGui::SetAllocatorFunctions(
					reinterpret_cast<ImGuiMemAllocFunc>(allocFn),
					reinterpret_cast<ImGuiMemFreeFunc>(freeFn),
					user);
			}
			g_LastSyncedGen = gen;
		}
	}

	bool Init() {
		if (g_Initialized) return true;

		SyncImGuiContextFromBridge();
		if (ImGui::GetCurrentContext() == nullptr) {
			// Consumer hasn't published a context yet — try again on the
			// next entry. Matches the ImGuiImplBgfx graceful-defer.
			return false;
		}

		if (!WebGPUBackend::IsInitialized()) {
			AIM_CORE_ERROR_TAG("ImGuiImplWebGPU",
				"Init called before WebGPU backend initialized");
			return false;
		}

		wgpu::Device device = WebGPUBackend::GetDevice();
		if (!device) {
			AIM_CORE_ERROR_TAG("ImGuiImplWebGPU", "No wgpu::Device available");
			return false;
		}

		// Render target format = swap-chain format. The editor draws ImGui
		// directly onto the OS window (the per-FBO scene panels are ImGui
		// images, not ImGui draw lists). One pipeline per session is
		// enough — Dawn caches it internally.
		ImGui_ImplWGPU_InitInfo info{};
		info.Device                          = device.Get();
		info.NumFramesInFlight               = 3;
		info.RenderTargetFormat              = static_cast<WGPUTextureFormat>(
			WebGPUBackend::GetSurfaceFormat());
		info.DepthStencilFormat              = WGPUTextureFormat_Undefined;
		info.PipelineMultisampleState        = {};
		info.PipelineMultisampleState.count  = 1;
		info.PipelineMultisampleState.mask   = 0xFFFFFFFFu;

		if (!ImGui_ImplWGPU_Init(&info)) {
			AIM_CORE_ERROR_TAG("ImGuiImplWebGPU", "ImGui_ImplWGPU_Init failed");
			return false;
		}

		g_Initialized = true;
		AIM_CORE_INFO_TAG("ImGuiImplWebGPU",
			"ImGui WebGPU backend initialized (target format={})",
			static_cast<int>(info.RenderTargetFormat));
		return true;
	}

	void NewFrame() {
		if (!g_Initialized) return;
		// Resync to pick up consumer-side context hot-reloads before any
		// ImGui call. Same defensive pattern the bgfx side uses.
		SyncImGuiContextFromBridge();
		ImGui_ImplWGPU_NewFrame();
	}

	void RenderDrawData(ImDrawData* drawData, unsigned short /*viewId*/) {
		if (!g_Initialized || drawData == nullptr || drawData->CmdListsCount <= 0) {
			return;
		}
		// Resync engine.dll's ImGui context from the consumer's bridge so
		// every helper inside ImGui_ImplWGPU_RenderDrawData (notably
		// ImGui_ImplWGPU_CreateImageBindGroup, which re-fetches the
		// backend data via ImGui::GetIO() each time it's called) sees
		// the right ImGuiContext. Without this, the inner re-fetch can
		// see a stale/null GImGui inside engine.dll and dereference a
		// null `bd` -> 0xC0000005 access violation reading at
		// `bd->wgpuDevice` offset.
		SyncImGuiContextFromBridge();
		if (ImGui::GetCurrentContext() == nullptr) return;
		if (ImGui::GetIO().BackendRendererUserData == nullptr) {
			AIM_CORE_WARN_TAG("ImGuiImplWebGPU",
				"RenderDrawData: BackendRendererUserData is null after sync — "
				"editor's ImGui context likely didn't have ImGui_ImplWGPU_Init "
				"called on it. Skipping draw.");
			return;
		}

		const float fbW = drawData->DisplaySize.x * drawData->FramebufferScale.x;
		const float fbH = drawData->DisplaySize.y * drawData->FramebufferScale.y;
		if (fbW <= 0.0f || fbH <= 0.0f) return;

		// Resolve the current target. Editor's ImGui pass typically runs
		// last in the frame on the swap chain (after Renderer2D / Gui-
		// Renderer / Text / Gizmo passes). Whatever the most recent
		// BindFramebuffer set is what we draw onto.
		auto target = WebGPUBackend::BeginRenderToCurrentTarget();
		if (!target.Valid) return;

		wgpu::CommandEncoder encoder = WebGPUBackend::GetFrameEncoder();
		if (!encoder) return;

		wgpu::RenderPassColorAttachment colorAtt{};
		colorAtt.view       = target.ColorView;
		colorAtt.loadOp     = wgpu::LoadOp::Load;
		colorAtt.storeOp    = wgpu::StoreOp::Store;
		colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

		wgpu::RenderPassDepthStencilAttachment depthAtt{};
		// We initialised ImGui_ImplWGPU with DepthStencilFormat=Undefined,
		// so the pipeline doesn't expect a depth attachment. Always omit
		// it from the pass descriptor even if the target has one — the
		// underlying texture stays untouched.
		(void)depthAtt;

		wgpu::RenderPassDescriptor passDesc{};
		passDesc.label                  = "imgui-pass";
		passDesc.colorAttachmentCount   = 1;
		passDesc.colorAttachments       = &colorAtt;
		passDesc.depthStencilAttachment = nullptr;

		wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);

		// Hand the pass off to ImGui's official backend. It records its
		// SetPipeline / SetVertexBuffer / SetIndexBuffer / SetScissorRect
		// / Draw calls into our encoder.
		ImGui_ImplWGPU_RenderDrawData(drawData, pass.Get());

		pass.End();

		if (target.IsSwapChain) {
			WebGPUBackend::MarkSwapChainRendered();
		}
	}

	void Shutdown() {
		if (!g_Initialized) return;
		ImGui_ImplWGPU_Shutdown();
		g_Initialized = false;
	}

}  // namespace Axiom::ImGuiImplWebGPU
