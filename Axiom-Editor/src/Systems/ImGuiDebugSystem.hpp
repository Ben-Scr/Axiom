#pragma once
#include "Core/Layer.hpp"
#include "Core/Export.hpp"

namespace Axiom {
	class ImGuiDebugSystem : public Layer {
	public:
		using Layer::Layer;

		void OnPreRender(Application& app) override;
	};
}
