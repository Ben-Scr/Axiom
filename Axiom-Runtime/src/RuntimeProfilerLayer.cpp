#include "RuntimeProfilerLayer.hpp"

#include "Core/Application.hpp"
#include "Core/Window.hpp"

#ifdef AXIOM_PROFILER_ENABLED
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#endif

namespace Axiom {

#ifdef AXIOM_PROFILER_ENABLED

	void RuntimeProfilerLayer::OnAttach(Application& app) {
		Window* window = app.GetWindow();
		if (!window) return;
		GLFWwindow* glfwWindow = window->GetGLFWWindow();
		if (!glfwWindow) return;

		// Minimal duplicate of the editor's ImGuiContextLayer init. Kept
		// local rather than promoted to the engine because the editor
		// version is theme-laden and editor-specific; runtime just needs
		// a panel and the default theme is fine for a debug overlay.
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

		if (!ImGui_ImplGlfw_InitForOpenGL(glfwWindow, true)) return;
		if (!ImGui_ImplOpenGL3_Init("#version 330 core")) return;

		m_ImGuiInitialized = true;
		m_Panel.Initialize();
	}

	void RuntimeProfilerLayer::OnDetach(Application&) {
		if (!m_ImGuiInitialized) return;
		m_Panel.Shutdown();
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		m_ImGuiInitialized = false;
	}

	void RuntimeProfilerLayer::OnUpdate(Application&, float) {
		if (!m_ImGuiInitialized) return;
		// Ctrl+F6: same shortcut as the editor. We sample at OnUpdate
		// time so input is on the polled-this-frame state, not from the
		// previous frame.
		ImGuiIO& io = ImGui::GetIO();
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
			m_ShowPanel = !m_ShowPanel;
		}
	}

	void RuntimeProfilerLayer::OnPreRender(Application&) {
		if (!m_ImGuiInitialized) return;
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		// Render the panel on every frame the user has it open. The panel
		// itself manages collection-gating via SetPanelVisible.
		if (m_ShowPanel) {
			m_Panel.Render(&m_ShowPanel);
		}
	}

	void RuntimeProfilerLayer::OnPostRender(Application&) {
		if (!m_ImGuiInitialized) return;
		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	}

#else // !AXIOM_PROFILER_ENABLED — empty stubs so the layer-push site stays clean

	void RuntimeProfilerLayer::OnAttach(Application&)         {}
	void RuntimeProfilerLayer::OnDetach(Application&)         {}
	void RuntimeProfilerLayer::OnUpdate(Application&, float)  {}
	void RuntimeProfilerLayer::OnPreRender(Application&)      {}
	void RuntimeProfilerLayer::OnPostRender(Application&)     {}

#endif

} // namespace Axiom
