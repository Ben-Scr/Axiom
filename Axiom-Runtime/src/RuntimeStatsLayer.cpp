#include "RuntimeStatsLayer.hpp"
#include "RuntimeImGuiHost.hpp"

#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Collections/Viewport.hpp"

#include <imgui.h>

namespace Axiom {

	// Defined in RuntimeLogLayer.cpp. Publishes our last-rendered window
	// height so the log layer can stack directly below us when both are
	// visible. Resolves to a no-op stub when the log layer isn't pushed
	// because the stub is the same TU; if we ever split it out, fall back
	// to a weak-extern pattern.
	void Internal_SetStatsRenderedHeight(float h);

}

namespace Axiom {

	void RuntimeStatsLayer::OnAttach(Application& app) {
		m_ImGuiAcquired = RuntimeImGuiHost::Acquire(app.GetWindow());
	}

	void RuntimeStatsLayer::OnDetach(Application&) {
		if (m_ImGuiAcquired) {
			RuntimeImGuiHost::Release();
			m_ImGuiAcquired = false;
		}
	}

	void RuntimeStatsLayer::OnUpdate(Application&, float) {
		if (!m_ImGuiAcquired || !RuntimeImGuiHost::IsInitialized()) return;

		// F6: bare key (no modifier) — keeps the binding distinct from
		// Ctrl+F6 used by the runtime profiler panel so power users can
		// have both open simultaneously without clobbering each other.
		ImGuiIO& io = ImGui::GetIO();
		if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
			m_Show = !m_Show;
		}
	}

	void RuntimeStatsLayer::OnPreRender(Application& app) {
		if (!m_ImGuiAcquired || !RuntimeImGuiHost::IsInitialized()) return;

		// The host brackets ImGui::NewFrame on the first BeginFrame call this
		// frame, so it's safe to call from every layer that needs a frame.
		RuntimeImGuiHost::BeginFrame();

		if (!m_Show) {
			// Publish 0 so the log overlay knows we're not rendering and
			// places itself at y = 0 (i.e., where the stats window would be).
			Internal_SetStatsRenderedHeight(0.0f);
			return;
		}

		int width = 0, height = 0;
		if (Window* window = app.GetWindow()) {
			if (Viewport* vp = Window::GetMainViewport()) {
				width = vp->GetWidth();
				height = vp->GetHeight();
			}
			else {
				width = window->GetWidth();
				height = window->GetHeight();
			}
		}
		m_Overlay.RefreshIfDue(width, height);
		const float renderedHeight = m_Overlay.RenderInMainViewport();
		Internal_SetStatsRenderedHeight(renderedHeight);
	}

	void RuntimeStatsLayer::OnPostRender(Application&) {
		if (!m_ImGuiAcquired || !RuntimeImGuiHost::IsInitialized()) return;
		RuntimeImGuiHost::EndFrame();
	}

} // namespace Axiom
