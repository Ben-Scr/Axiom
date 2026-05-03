#pragma once
#include "pch.hpp"
#include "ParticleUpdateSystem.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/Scene.hpp"

#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/Tags.hpp"
#include "Core/Application.hpp"

#include <entt/entt.hpp>

namespace Axiom {
	void ParticleUpdateSystem::Awake(Scene& scene) {
		if (!Application::GetIsPlaying()) {
			return;
		}

		for (const auto& [ent, particleSystem] : scene.GetRegistry().view<ParticleSystem2DComponent>(entt::exclude<DisabledTag>).each())
			if (particleSystem.PlayOnAwake)
				particleSystem.Play();
	}

	void ParticleUpdateSystem::Update(Scene& scene) {
		// E20: fetch dt once per system Update and forward it to each component.
		// Previously each ParticleSystem2DComponent::Update() reached back to
		// Application::GetInstance()->GetTime().GetDeltaTime() per call.
		const float dt = Application::GetInstance()->GetTime().GetDeltaTime();
		for (const auto& [ent, particleSystem] : scene.GetRegistry().view<ParticleSystem2DComponent>(entt::exclude<DisabledTag>).each())
			particleSystem.Update(dt);
	}
}
