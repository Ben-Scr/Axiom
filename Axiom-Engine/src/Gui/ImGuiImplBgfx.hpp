#pragma once

#include "Core/Export.hpp"

// Stage 3 of the bgfx port: ImGui backend that submits draw lists through
// bgfx instead of imgui_impl_opengl3. Stage 3 does not vendor a third-party
// imgui_impl_bgfx; instead this minimal backend follows the standard ImGui
// backend contract (Init / NewFrame / RenderDrawData / Shutdown) using
// bgfx primitives directly.
//
// Built only under --rhi=bgfx. The editor / launcher / runtime switch
// between this backend and imgui_impl_opengl3 via the AIM_RHI_BGFX define.
//
// IMPORTANT: this code now lives in Axiom-Engine.dll (was previously
// duplicated into each consumer's binary). bgfx maintains its renderer
// state in process-global statics; static-linking bgfx into multiple
// binaries inside one process means each binary gets its own
// uninitialised copy of those statics, so a consumer .exe calling
// `bgfx::getRendererType()` would see `Noop` even though engine.dll's
// `bgfx::init` had already brought up D3D11. Centralising the imgui
// backend inside engine.dll — same TU as the bgfx::init call —
// guarantees both run against the same bgfx state.

struct ImDrawData;

namespace Axiom::ImGuiImplBgfx {

	// Allocate the imgui shader program, sampler uniform, ortho-uniform,
	// and font-atlas texture. Run after `ImGui::CreateContext` and
	// `imgui_impl_glfw::Init`. Returns false if the shader binaries are
	// missing or the shader program failed to link.
	AXIOM_API bool Init();

	// Per-frame setup. Mirrors imgui_impl_opengl3::NewFrame — picks up
	// any font-atlas changes (re-uploads the texture) and validates that
	// Init has been called. Cheap when nothing changed.
	AXIOM_API void NewFrame();

	// Submit the assembled ImDrawData through bgfx::submit. Allocates
	// transient vertex / index buffers per draw list, then issues one
	// bgfx::submit per ImDrawCmd into the supplied bgfx view-id (the
	// editor passes its dedicated UI view). Honours scissor + texture
	// switches just like the GL backend.
	//
	// `viewId` is a bgfx::ViewId — declared as uint16_t here so this
	// header doesn't have to include bgfx/bgfx.h (consumers can stay
	// backend-neutral).
	AXIOM_API void RenderDrawData(ImDrawData* drawData, unsigned short viewId);

	// Release all bgfx resources (program, sampler uniform, ortho
	// uniform, font atlas). Run before `ImGui::DestroyContext`.
	AXIOM_API void Shutdown();

} // namespace Axiom::ImGuiImplBgfx
