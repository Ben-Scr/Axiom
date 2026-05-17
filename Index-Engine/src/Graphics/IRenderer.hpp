#pragma once

#include "Core/Export.hpp"

namespace Index {

	// IRenderer is the swappable scene-rendering boundary. Application owns one
	// via std::unique_ptr<IRenderer>; the default implementation is Renderer2D.
	// Packages can substitute alternate renderers (null renderer for headless
	// servers, custom 3D backends, layered debug renderers) by installing a
	// different concrete impl before Application::Initialize runs.
	//
	// Current state: this is the first slice. The interface intentionally only
	// captures lifecycle + frame boundary — the methods Application itself
	// calls. Concrete consumers (Scene, Systems, Editor) still talk to
	// Renderer2D* directly via Application::GetRenderer2D(). Future tightening
	// passes will promote enough of Renderer2D's surface onto IRenderer that
	// those concrete dependencies can drop, at which point Application's
	// downcast shim goes away.
	class INDEX_API IRenderer {
	public:
		virtual ~IRenderer() = default;

		virtual void Initialize() = 0;
		virtual void Shutdown() = 0;

		virtual void BeginFrame() = 0;
		virtual void EndFrame() = 0;

		virtual bool IsInitialized() const = 0;
	};

}
