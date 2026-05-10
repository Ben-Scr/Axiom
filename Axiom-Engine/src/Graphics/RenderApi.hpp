#pragma once

#include "Collections/Color.hpp"
#include "Core/Export.hpp"
#include "Graphics/RenderApiTypes.hpp"

#include <string>
#include <string_view>

// =============================================================================
// RenderApi — backend-neutral static API for the engine's renderer.
// -----------------------------------------------------------------------------
// Every immediate-mode GPU operation the engine needs (clear / viewport /
// scissor / blend / cull / polygon mode / line width / generic state) is
// dispatched through this interface so the renderer code itself doesn't
// include any backend headers. The OpenGL implementation lives in
// `Backend/OpenGLApi.cpp` — it is the only file outside this header allowed
// to include `<glad/glad.h>` going forward (existing resource wrappers like
// Texture2D / Shader / QuadMesh / Font / GpuTimer / GuiRenderer's
// stream-buffer code are documented exceptions, to be migrated in later
// stages of the bgfx port).
//
// Stage 0 scope (this file): immediate state + framebuffer binding +
// debugging info. Resource creation (buffers, textures, shaders) keeps its
// existing class wrappers; those are migrated in later stages so the diff
// stays bounded.
// =============================================================================

namespace Axiom {

	struct GLInitSpecifications;
	class Framebuffer;

	class AXIOM_API RenderApi {
	public:
		RenderApi() = delete;

		// Bring the backend up. Implementation reads the current GL context,
		// loads function pointers (in the GL backend that means glad), caches
		// vendor / renderer / version strings, and applies the initial
		// blend / cull / clear-colour state from `spec`. Idempotent — second
		// call is a no-op. Returns true on success.
		static bool Init(const GLInitSpecifications& spec);

		// Tear down any backend-owned global state. Resource wrappers handle
		// their own cleanup; this handles backend-init mirror image.
		static void Shutdown();

		static bool IsInitialized();

		// Identifier of the active backend ("OpenGL", "bgfx-d3d11", etc.) —
		// shown in the editor's About / Stats overlay. Backend-neutral so the
		// caller doesn't have to special-case GL strings.
		static std::string_view BackendName();

		// Cached GPU info (filled in by Init). Empty strings if Init never
		// succeeded. The build / about UI surfaces these.
		static const std::string& GetVersionString();
		static const std::string& GetVendorString();
		static const std::string& GetRendererString();

		// ── Per-frame / per-pass immediate state ─────────────────────
		// Standard "set state then draw" model. Wireframe, color-mask, and
		// the logic-op trick the editor uses for the wireframe overlay all
		// live here so callers stop poking GL directly.

		static void Clear(ClearFlags flags);
		static void SetClearColor(const Color& color);
		static Color GetClearColor();

		static void SetViewport(int x, int y, int width, int height);
		static void SetScissor(int x, int y, int width, int height);

		// Window resize → swap-chain resize hook. Called from Window's
		// resize callback so the bgfx swap-chain follows the GLFW window
		// (otherwise the swap-chain stays at bgfx::init's resolution and
		// rendering only fills the top-left of the resized window). Pulled
		// out of SetViewport because SetViewport is ALSO called when an
		// FBO of a different size is bound — only `OnWindowResize` should
		// trigger a swap-chain reset.
		static void OnWindowResize(int width, int height);
		static void EnableScissorTest();
		static void DisableScissorTest();

		static void EnableDepthTest();
		static void DisableDepthTest();

		static void SetCullMode(CullMode mode);

		// Predefined blend recipe + a generic RGBA toggle. SetBlendMode
		// covers ~95% of call sites; SetBlendingEnabled is for the rare
		// case (e.g. text renderer) that already configured a custom
		// glBlendFuncSeparate before this abstraction landed.
		static void SetBlendMode(BlendMode mode);
		static void SetBlendingEnabled(bool enabled);

		static void SetPolygonMode(PolygonMode mode);
		static void SetLineWidth(float width);

		static void SetColorMask(bool r, bool g, bool b, bool a);

		// ── Color-logic-op overlay scope (editor wireframe pass) ─────
		// The editor's "Triangle / Mixed" draw modes use a logic-op
		// trick (GL_CLEAR) to force every wireframe-overlaid pixel to
		// solid black regardless of the entity's shader output. Wrap
		// the begin/end so callers don't have to know the recipe.
		static void BeginColorLogicOpClear();
		static void EndColorLogicOpClear();

		// ── Framebuffer binding ──────────────────────────────────────
		// Bind one of the engine's Framebuffer wrappers as the current
		// render target, or restore the window's default framebuffer.
		// Implementation translates to glBindFramebuffer(GL_FRAMEBUFFER, id)
		// today; on bgfx this becomes a `bgfx::setViewFrameBuffer` call.
		static void BindFramebuffer(const Framebuffer& fbo);
		static void BindDefaultFramebuffer();
	};

} // namespace Axiom
