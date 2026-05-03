#pragma once

#include "Core/Layer.hpp"
#include "Diagnostics/StatsOverlay.hpp"

namespace Axiom {

	// Runtime-side ImGui layer that hosts the engine's StatsOverlay in built
	// applications. Pushed by RuntimeApplication::Start() when the project's
	// `showRuntimeStats` setting is true (default).
	//
	// Controls
	//   F6 — toggle the overlay on/off (consistent with the existing
	//        Ctrl+F6 binding for the runtime profiler panel).
	//
	// Rendering
	//   The layer shares a single ImGui context with any other runtime layers
	//   via RuntimeImGuiHost; whichever layer attaches first creates it.
	//
	// Cost when disabled
	//   Layer not pushed = zero. Layer pushed but overlay hidden = a single
	//   key-press check per frame and one ImGui frame open/close (the host's
	//   refcounting still has to bracket since other layers might draw).
	class RuntimeStatsLayer : public Layer {
	public:
		using Layer::Layer; // inherit "explicit Layer(const std::string& name)"

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnUpdate(Application& app, float dt) override;
		void OnPreRender(Application& app) override;
		void OnPostRender(Application& app) override;

		bool IsVisible() const { return m_Show; }
		void SetVisible(bool v) { m_Show = v; }

	private:
		bool m_Show = false;
		bool m_ImGuiAcquired = false;
		Diagnostics::StatsOverlay m_Overlay;
	};

} // namespace Axiom
