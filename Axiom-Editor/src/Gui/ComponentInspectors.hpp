#pragma once
#include "Scene/Entity.hpp"

#include <span>

namespace Axiom {

	void DrawNameComponentInspector(std::span<const Entity> entities);
	void DrawTransform2DInspector(std::span<const Entity> entities);
	void DrawRigidbody2DInspector(std::span<const Entity> entities);
	void DrawSpriteRendererInspector(std::span<const Entity> entities);
	void DrawCamera2DInspector(std::span<const Entity> entities);
	void DrawParticleSystem2DInspector(std::span<const Entity> entities);
	void DrawBoxCollider2DInspector(std::span<const Entity> entities);
	void DrawAudioSourceInspector(std::span<const Entity> entities);

	// Axiom-Physics inspectors
	void DrawFastBody2DInspector(std::span<const Entity> entities);
	void DrawFastBoxCollider2DInspector(std::span<const Entity> entities);
	void DrawFastCircleCollider2DInspector(std::span<const Entity> entities);

} // namespace Axiom
