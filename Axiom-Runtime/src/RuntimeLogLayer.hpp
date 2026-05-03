#pragma once

#include "Core/Layer.hpp"
#include "Diagnostics/LogOverlay.hpp"

namespace Axiom {

	class RuntimeStatsLayer; // for stacking-offset query

	// Runtime-side ImGui layer that hosts the engine's LogOverlay in built
	// applications. Pushed by RuntimeApplication::Start() when the project's
	// `showRuntimeLogs` setting is true (default).
	//
	// Controls
	//   F7 — toggle the overlay on/off (sibling to F6 stats / Ctrl+F6 profiler).
	//
	// Stacking
	//   When the stats overlay is also visible, the log overlay positions
	//   itself directly below it via the height GetLastRenderedHeight()
	//   exposes on RuntimeStatsLayer. Push order in RuntimeApplication::
	//   Start() ensures stats render first this frame.
	//
	// Cost when disabled
	//   Layer not pushed = zero (subscription to Log::OnLog also not made).
	//   Layer pushed but overlay hidden = a single key-press check + a
	//   tiny mutex-guarded push per AIM_INFO call.
	class RuntimeLogLayer : public Layer {
	public:
		using Layer::Layer; // inherit explicit Layer(name)

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
		// Owned via unique_ptr so the layer's default-constructibility (used by
		// Application::PushLayer<T>'s `new T(name)` path) still works without
		// us reaching for a non-default ctor on LogOverlay (which subscribes
		// to Log::OnLog at construction — we want that delayed until OnAttach
		// so the layer being constructed doesn't subscribe before Log is up).
		std::unique_ptr<Diagnostics::LogOverlay> m_Overlay;
	};

} // namespace Axiom
