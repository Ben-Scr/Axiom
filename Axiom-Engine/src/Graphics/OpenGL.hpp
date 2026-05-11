#pragma once

#include "Collections/Color.hpp"
#include "Core/Export.hpp"
#include "Graphics/GLInitSpecifications.hpp"

// =============================================================================
// DEPRECATED: thin shim over `RenderApi`.
// -----------------------------------------------------------------------------
// `Graphics/RenderApi.hpp` is the backend-neutral interface every renderer
// should call. This file forwards the historical `OpenGL::Foo` static
// methods to the equivalent `RenderApi::Foo` so existing callers
// (Application::Initialize, ScriptBindingsScene's GPU-info accessors,
// Window::IsInitialized check) keep compiling.
//
// New code should use `RenderApi` directly.
//
// Notably, the previous header leaked `<glad/glad.h>` and `GLenum` in its
// public surface (BlendFunc/Enable/Disable). Those are gone — the shim
// only exposes the calls that were actually used outside the renderer
// (init, GPU info, clear color), and the rest moved into `RenderApi`'s
// neutral enum surface (BlendMode / CullMode / etc.).
// =============================================================================

namespace Axiom {

	class AXIOM_API OpenGL {
	public:
		OpenGL() = delete;

		static bool Initialize(const GLInitSpecifications& glInitSpecs);
		static bool IsInitialized();

		static void SetClearColor(const Color& clearColor);
		static Color GetClearColor();

		static const std::string& GetVersionString();
		static const std::string& GetVendorString();
		static const std::string& GetRendererString();
	};

} // namespace Axiom
