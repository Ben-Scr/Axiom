#pragma once

#include "Core/Layer.hpp"

#include <chrono>
#include <string>

namespace Axiom {

	class AxiomEvent;

	// Owns the ImGui context for an Axiom application. Push this Layer first (before
	// any other ImGui-using Layer) so its OnPreRender / OnPostRender wrap the per-frame
	// NewFrame / Render calls around all other Layers' UI work.
	class ImGuiContextLayer : public Layer {
	public:
		using Layer::Layer;

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnEvent(Application& app, AxiomEvent& event) override;
		void OnPreRender(Application& app) override;
		void OnPostRender(Application& app) override;

	private:
		static void ApplyAxiomTheme();
		// Belt-and-suspenders save. ImGui auto-saves internally on its
		// own dirty-timer schedule, but specific change types (mid-
		// drag dock split ratios, certain window-close paths, late-
		// frame layout mutations) sometimes don't flip the dirty flag
		// and can be lost on a hard quit. We force a write whenever
		// `WantSaveIniSettings` flips OR every k_PeriodicSaveSeconds
		// regardless. The redundant writes are cheap (small ini, same
		// bytes when unchanged).
		void FlushSettingsIfDirtyOrPeriodic();

		std::string m_IniFilePath;
		bool m_IsInitialized = false;
		std::chrono::steady_clock::time_point m_LastSaveTime{};
	};

}
