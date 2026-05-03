#include "RuntimeProfilerLayer.hpp"
#include "RuntimeImGuiHost.hpp"

#include "Core/Application.hpp"
#include "Core/Window.hpp"

#ifdef AXIOM_PROFILER_ENABLED
#include <imgui.h>
#endif

namespace Axiom {

#ifdef AXIOM_PROFILER_ENABLED

	void RuntimeProfilerLayer::OnAttach(Application& app) {
		// Acquire the shared runtime ImGui context (created lazily on the
		// first acquirer; subsequent layers like RuntimeStatsLayer share it).
		m_ImGuiInitialized = RuntimeImGuiHost::Acquire(app.GetWindow());
		if (m_ImGuiInitialized) {
			m_Panel.Initialize();
		}
	}

	void RuntimeProfilerLayer::OnDetach(Application&) {
		if (!m_ImGuiInitialized) return;
		m_Panel.Shutdown();
		RuntimeImGuiHost::Release();
		m_ImGuiInitialized = false;
	}

	void RuntimeProfilerLayer::OnUpdate(Application&, float) {
		if (!m_ImGuiInitialized || !RuntimeImGuiHost::IsInitialized()) return;
		// Ctrl+F6: same shortcut as the editor. We sample at OnUpdate
		// time so input is on the polled-this-frame state, not from the
		// previous frame.
		ImGuiIO& io = ImGui::GetIO();
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
			m_ShowPanel = !m_ShowPanel;
		}
	}

	void RuntimeProfilerLayer::OnPreRender(Application&) {
		if (!m_ImGuiInitialized || !RuntimeImGuiHost::IsInitialized()) return;
		// Host's BeginFrame is refcounted — first layer this frame opens the
		// ImGui frame, others share it. Each layer calls EndFrame in
		// OnPostRender; the last one closes + draws.
		RuntimeImGuiHost::BeginFrame();

		// Render the panel on every frame the user has it open. The panel
		// itself manages collection-gating via SetPanelVisible.
		if (m_ShowPanel) {
			m_Panel.Render(&m_ShowPanel);
		}
	}

	void RuntimeProfilerLayer::OnPostRender(Application&) {
		if (!m_ImGuiInitialized || !RuntimeImGuiHost::IsInitialized()) return;
		RuntimeImGuiHost::EndFrame();
	}

#else // !AXIOM_PROFILER_ENABLED — empty stubs so the layer-push site stays clean

	void RuntimeProfilerLayer::OnAttach(Application&)         {}
	void RuntimeProfilerLayer::OnDetach(Application&)         {}
	void RuntimeProfilerLayer::OnUpdate(Application&, float)  {}
	void RuntimeProfilerLayer::OnPreRender(Application&)      {}
	void RuntimeProfilerLayer::OnPostRender(Application&)     {}

#endif

} // namespace Axiom
