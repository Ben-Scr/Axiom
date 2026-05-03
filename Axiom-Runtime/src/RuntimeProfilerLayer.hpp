#pragma once

#include "Core/Layer.hpp"
#include "Gui/ProfilerPanel.hpp"

namespace Axiom {

	// Runtime-side host for the in-game profiler panel.
	//
	// Activated only when the loaded project's
	// `axiom-project.json:profiler.enableInRuntime` is true. Sets up its
	// own ImGui context (the runtime doesn't otherwise use ImGui) and
	// reuses the editor's ProfilerPanel for the actual UI. Ctrl+F6 toggles
	// visibility — same shortcut as the editor for muscle-memory parity.
	//
	// The whole layer is gated behind AXIOM_PROFILER_ENABLED at compile
	// time. With --no-profiler this file does nothing meaningful (no ImGui
	// init, no panel) — but it stays in the build so RuntimeApplication.cpp
	// doesn't need its own #ifdef around the layer push.
	class RuntimeProfilerLayer : public Layer {
	public:
		using Layer::Layer;

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnPreRender(Application& app) override;
		void OnPostRender(Application& app) override;
		void OnUpdate(Application& app, float dt) override;

	private:
		bool m_ShowPanel = false;
		bool m_ImGuiInitialized = false;
		ProfilerPanel m_Panel;
	};

} // namespace Axiom
