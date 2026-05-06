#pragma once
#include "Collections/Vec2.hpp"
#include "Scene/EntityHandle.hpp"

namespace Axiom {
	class Scene;

	struct Collision2D {
		EntityHandle entityA;
		EntityHandle entityB;
		Scene* sceneA = nullptr;
		Scene* sceneB = nullptr;
		Vec2 contactPoint{ 0.0f, 0.0f };
	};
}
