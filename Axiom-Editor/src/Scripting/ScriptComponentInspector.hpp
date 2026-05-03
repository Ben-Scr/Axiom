#pragma once
#include "Scene/Entity.hpp"

#include <span>

namespace Axiom {
	void DrawScriptComponentInspector(std::span<const Entity> entities);
}
