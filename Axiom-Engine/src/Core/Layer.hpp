#pragma once
#include "Core/Export.hpp"
#include "Events/AxiomEvent.hpp"

#include <string>

namespace Axiom {
	class Application;

	class AXIOM_API Layer
	{
	public:
		explicit Layer(const std::string& name = "Layer");
		virtual ~Layer();

		virtual void OnAttach(Application& app) {}
		virtual void OnDetach(Application& app) {}
		virtual void OnUpdate(Application& app, float dt) {}
		virtual void OnFixedUpdate(Application& app, float fixedDt) {}

		// Called once per frame after scene update, before main renderer begin.
		// Layers wrapping an immediate-mode UI library should set up the new UI frame here.
		virtual void OnPreRender(Application& app) {}

		// Called once per frame after main renderer end, before window swap.
		// Layers wrapping an immediate-mode UI library should submit queued draw commands here.
		virtual void OnPostRender(Application& app) {}

		virtual void OnEvent(Application& app, AxiomEvent& event) {}

		inline const std::string& GetName() const { return m_DebugName; }
	protected:
		std::string m_DebugName;
	};
}

