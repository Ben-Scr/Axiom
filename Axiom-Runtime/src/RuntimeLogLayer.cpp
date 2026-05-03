#include "RuntimeLogLayer.hpp"
#include "RuntimeImGuiHost.hpp"
#include "RuntimeStatsLayer.hpp"

#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Collections/Viewport.hpp"

#include <imgui.h>

namespace Axiom {

	// Tracks the last-rendered height of the runtime stats overlay so the
	// log overlay can stack directly below it. Updated each frame the stats
	// layer renders; reset to 0 in OnDetach. Lives here (not on
	// RuntimeStatsLayer) so the engine's editor/runtime can both use a
	// uniform "ask the stats layer how tall it was" pattern via the
	// accessor exposed by the stats layer below.
	namespace {
		float s_LastStatsRenderedHeight = 0.0f;
	}

	// Defined here so RuntimeStatsLayer doesn't need to know about the log
	// layer; we just publish the value in RuntimeStatsLayer::OnPreRender via
	// an extern hook (see the small change in RuntimeStatsLayer.cpp).
	void Internal_SetStatsRenderedHeight(float h) { s_LastStatsRenderedHeight = h; }

	void RuntimeLogLayer::OnAttach(Application& app) {
		m_ImGuiAcquired = RuntimeImGuiHost::Acquire(app.GetWindow());
		if (m_ImGuiAcquired) {
			m_Overlay = std::make_unique<Diagnostics::LogOverlay>();
		}
	}

	void RuntimeLogLayer::OnDetach(Application&) {
		m_Overlay.reset();
		if (m_ImGuiAcquired) {
			RuntimeImGuiHost::Release();
			m_ImGuiAcquired = false;
		}
	}

	void RuntimeLogLayer::OnUpdate(Application&, float) {
		if (!m_ImGuiAcquired || !RuntimeImGuiHost::IsInitialized()) return;
		// F7: bare key (no modifier) — sibling to F6 stats. Both can be on
		// at the same time; the log overlay stacks below the stats overlay.
		ImGuiIO& io = ImGui::GetIO();
		if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F7, false)) {
			m_Show = !m_Show;
		}
	}

	void RuntimeLogLayer::OnPreRender(Application&) {
		if (!m_ImGuiAcquired || !RuntimeImGuiHost::IsInitialized()) return;
		RuntimeImGuiHost::BeginFrame();

		if (!m_Show || !m_Overlay) return;

		// Stack below stats overlay if it rendered this frame. The stats
		// layer (pushed first in RuntimeApplication::Start) writes its
		// rendered height into s_LastStatsRenderedHeight via the helper
		// in RuntimeStatsLayer.cpp. Add a small gap so the two windows
		// don't sit edge-to-edge.
		float yOffset = 0.0f;
		if (s_LastStatsRenderedHeight > 0.0f) {
			yOffset = s_LastStatsRenderedHeight + 8.0f;
		}

		m_Overlay->RenderInMainViewport(yOffset);
	}

	void RuntimeLogLayer::OnPostRender(Application&) {
		if (!m_ImGuiAcquired || !RuntimeImGuiHost::IsInitialized()) return;
		RuntimeImGuiHost::EndFrame();
	}

} // namespace Axiom
