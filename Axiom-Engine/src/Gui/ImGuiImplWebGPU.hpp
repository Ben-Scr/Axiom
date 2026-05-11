#pragma once

#include "Core/Export.hpp"

// Stage 9 of the WebGPU port: ImGui backend that submits draw lists through
// the Dawn-backed WebGPU device instead of bgfx. Wraps Dear ImGui's official
// `imgui_impl_wgpu` backend (vendored under External/imgui/backends/) with
// the same `Init / NewFrame / RenderDrawData / Shutdown` surface ImGuiImplBgfx
// exposes, so editor / launcher / runtime call sites stay backend-neutral.
//
// Built only under --rhi=webgpu. The premake gate swaps this in for
// ImGuiImplBgfx.cpp the same way every other Stage 2+ resource port did.
//
// IMPORTANT: this code lives in Axiom-Engine.dll. The bgfx-side header
// documented why (bgfx's renderer state is process-DLL-static, so a
// consumer-side bgfx::init would see Noop). WebGPU has the same constraint:
// wgpu::Device is owned by engine.dll's WebGPUApi.cpp, so the backend that
// consumes it has to live in the same module. PackageImGuiBridge syncs the
// consumer's ImGuiContext into engine.dll's ImGui copy on every entry point.

struct ImDrawData;

namespace Axiom::ImGuiImplWebGPU {

	// Initialise the Dear ImGui WebGPU backend. Runs after ImGui::Create-
	// Context and imgui_impl_glfw::Init. Picks up the active wgpu::Device
	// + surface format from WebGPUApi.cpp's internal state. Idempotent;
	// returns true once successful and on subsequent calls. Returns false
	// if the backend isn't initialised yet (device not ready).
	AXIOM_API bool Init();

	// Per-frame setup. Forwards to ImGui_ImplWGPU_NewFrame, which inspects
	// the font atlas for changes and re-uploads if needed. Cheap when
	// nothing changed.
	AXIOM_API void NewFrame();

	// Submit ImDrawData. The `viewId` parameter is preserved for ABI
	// parity with the bgfx-side signature so editor / launcher / runtime
	// call sites compile unchanged; under WebGPU it is ignored — the
	// active render target is determined by the most recent
	// RenderApi::BindFramebuffer (typically the swap chain for the editor's
	// ImGui pass).
	//
	// Internally: opens a wgpu::RenderPassEncoder with LoadOp::Load on the
	// current target, dispatches to ImGui_ImplWGPU_RenderDrawData with the
	// pass encoder, then ends the pass. Marks the swap chain as rendered
	// so Present()'s touch-fallback skips its safety clear.
	AXIOM_API void RenderDrawData(ImDrawData* drawData, unsigned short viewId);

	// Releases ImGui's wgpu resources (pipeline, bind groups, font atlas
	// texture). Runs before ImGui::DestroyContext.
	AXIOM_API void Shutdown();

}  // namespace Axiom::ImGuiImplWebGPU
