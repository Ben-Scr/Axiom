#include "pch.hpp"
#include "Graphics/OpenGL.hpp"

#include "Graphics/RenderApi.hpp"

// Thin back-compat shim — see OpenGL.hpp for the migration story. Every
// method here forwards directly to `RenderApi`.

namespace Axiom {

	bool OpenGL::Initialize(const GLInitSpecifications& glInitSpecs) {
		return RenderApi::Init(glInitSpecs);
	}

	bool OpenGL::IsInitialized() {
		return RenderApi::IsInitialized();
	}

	void OpenGL::SetClearColor(const Color& clearColor) {
		RenderApi::SetClearColor(clearColor);
	}

	Color OpenGL::GetClearColor() {
		return RenderApi::GetClearColor();
	}

	const std::string& OpenGL::GetVersionString()  { return RenderApi::GetVersionString(); }
	const std::string& OpenGL::GetVendorString()   { return RenderApi::GetVendorString(); }
	const std::string& OpenGL::GetRendererString() { return RenderApi::GetRendererString(); }

} // namespace Axiom
