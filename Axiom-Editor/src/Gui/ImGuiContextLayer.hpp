#pragma once

#include "Core/Layer.hpp"

#include <string>

namespace Axiom {

	// Owns the ImGui context for an Axiom application. Push this Layer first (before
	// any other ImGui-using Layer) so its OnPreRender / OnPostRender wrap the per-frame
	// NewFrame / Render calls around all other Layers' UI work.
	class ImGuiContextLayer : public Layer {
	public:
		using Layer::Layer;

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnPreRender(Application& app) override;
		void OnPostRender(Application& app) override;

	private:
		static void ApplyAxiomTheme();
		std::string m_IniFilePath;
		bool m_IsInitialized = false;
	};

}
