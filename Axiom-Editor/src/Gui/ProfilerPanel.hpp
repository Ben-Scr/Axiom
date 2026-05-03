#pragma once

#include <string>
#include <vector>

namespace Axiom {

	// In-engine ImGui dashboard for the Axiom profiler.
	//
	// Layout:
	//   [Track in Background]  [Sampling Hz: __]  [Tracking Span: __]
	//   ─────────────────────────────────────────────────────────────
	//   [✓] FPS          Current 142.0  Avg 140.3  Min 60  Max 200  [graph]
	//   [✓] Frame        Current  6.9ms Avg  7.1   Min 4   Max 16   [graph]
	//   [✓] Physics      ...
	//   ...
	//
	// Reads from Axiom::Profiler::AllModules() each frame; the panel itself
	// holds zero collection state. When the window is collapsed or closed
	// it sets Profiler::SetPanelVisible(false), which (combined with the
	// Track-in-Background toggle) gates whether modules continue to push
	// samples.
	//
	// Always compilable (the .cpp is gated internally on AXIOM_PROFILER_ENABLED
	// — when stripped it renders a single "Profiler disabled" line so the
	// menu item still works).
	class ProfilerPanel {
	public:
		void Initialize();
		void Render(bool* pOpen);
		void Shutdown();

	private:
		// Float-side mirrors of editor/project settings; written into them
		// only when the user changes a value so we don't spam the
		// Profiler/AxiomProject getters with no-op writes per frame.
		int m_SamplingHz = 60;
		int m_TrackingSpan = 200;
		bool m_TrackInBackground = false;

		// True after the first Render(): used to lazily restore project
		// settings + per-module enabled flags exactly once when the panel
		// first opens against a loaded project.
		bool m_SettingsLoaded = false;

		void RenderModuleRow(const std::string& moduleName);
		// Implementation helper. Takes the registered module name (used to
		// look up the live ProfilerModule in the registry), the human-
		// friendly label shown in the panel, and the unit ("ms", "MB", or
		// empty for counts).
		void RenderModuleRowImpl(const char* registeredName,
			const char* displayLabel,
			const char* unit);
		void LoadSettingsFromProject();
		void SaveSettingsToProject();
	};

} // namespace Axiom
