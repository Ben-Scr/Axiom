#include "pch.hpp"

#include <Components/Physics/Rigidbody2DComponent.hpp>
#include <Components/Physics/FastBody2DComponent.hpp>
#include <Components/General/Transform2DComponent.hpp>
#include <Components/Tags.hpp>

#include "Physics/PhysicsSystem2D.hpp"
#include "Physics/Box2DWorld.hpp"
#include "Physics/Collision2D.hpp"

#include "Profiling/Profiler.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/ScriptSystem.hpp"


namespace Axiom {
	bool PhysicsSystem2D::s_IsEnabled = true;
	std::optional<Box2DWorld> PhysicsSystem2D::s_MainWorld;
	std::optional<AxiomPhysicsWorld2D> PhysicsSystem2D::s_AxiomWorld;

	void PhysicsSystem2D::Initialize() {
		s_MainWorld.emplace();
		s_AxiomWorld.emplace();
		AIM_CORE_INFO_TAG("PhysicsSystem", "Box2D + Axiom-Physics initialized");
	}

	void PhysicsSystem2D::FixedUpdate(float dt) {
		AXIOM_PROFILE_SCOPE("Physics");
		if (!s_IsEnabled) return;

		// E16: Pre-step sync — single ForeachLoadedScene walk handling both
		// Rigidbody2D (Box2D) and FastBody2D (Axiom-Physics) write-back to the
		// physics worlds.
		SceneManager::Get().ForeachLoadedScene([](Scene& scene) {
			auto& registry = scene.GetRegistry();

			for (auto [ent, rb, tf] : registry.view<Rigidbody2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (tf.IsDirty() && rb.IsValid()) {
					rb.SetTransform(tf);
					tf.ClearDirty();
				}
			}

			for (auto [ent, body, tf] : registry.view<FastBody2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (tf.IsDirty() && body.m_Body) {
					body.SetPosition(tf.Position);
					tf.ClearDirty();
				}
			}
		});

		// Box2D simulation. We deliberately defer collision-callback dispatch
		// until AFTER the post-step transform sync below (see H4). Box2D's own
		// dispatcher collects events into a list during Step; running user
		// scripts here would (a) hand them stale Transform2D values that
		// haven't been synced from Box2D yet, and (b) leave any user-side
		// `entity.Destroy()` straddling the Box2D step / Axiom-Physics step
		// boundary. Both steps run first, then transforms are synced, then
		// callbacks fire on a fully-settled scene.
		s_MainWorld->Step(dt);

		// Axiom-Physics simulation (Box2D step already completed above; Axiom
		// step has no dependency on Box2D state, so both syncs fuse into one
		// ForeachLoadedScene walk below).
		s_AxiomWorld->Step(dt);

		// E16: Fused post-step transform sync — one ForeachLoadedScene walk
		// for both Rigidbody2D (Box2D) and FastBody2D (Axiom-Physics) instead
		// of two. Replaces the previously separate Box2D-sync + Axiom-sync
		// walks. Ordering preserved: both steps have already run.
		SceneManager::Get().ForeachLoadedScene([](Scene& scene) {
			auto& registry = scene.GetRegistry();

			for (auto [ent, rb, tf] : registry.view<Rigidbody2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				tf.Position = rb.GetPosition();
				tf.Rotation = rb.GetRotation();
				tf.ClearDirty();
			}

			for (auto [ent, body, tf] : registry.view<FastBody2DComponent, Transform2DComponent>(entt::exclude<DisabledTag>).each()) {
				if (body.m_Body) {
					auto pos = body.m_Body->GetPosition();
					tf.Position = { pos.x, pos.y };
					tf.ClearDirty();
				}
			}
		});

		// H4: Collision callback dispatch happens AFTER all physics steps and
		// transform syncs are complete. User scripts may call
		// `entity.Destroy()` inside these callbacks; the IsValid guard
		// inside each lambda short-circuits subsequent events targeting an
		// already-destroyed entity in the same dispatch batch.
		s_MainWorld->GetDispatcher().Process(
			s_MainWorld->GetWorldID(),
			[](const Collision2D& collision) {
				SceneManager::Get().ForeachLoadedScene([&](Scene& scene) {
					if (scene.IsValid(collision.entityA) && scene.IsValid(collision.entityB)) {
						ScriptSystem::DispatchCollisionEnter2D(scene, collision);
					}
				});
			},
			[](const Collision2D& collision) {
				SceneManager::Get().ForeachLoadedScene([&](Scene& scene) {
					if (scene.IsValid(collision.entityA) && scene.IsValid(collision.entityB)) {
						ScriptSystem::DispatchCollisionExit2D(scene, collision);
					}
				});
			},
			[](const Collision2D& collision) {
				SceneManager::Get().ForeachLoadedScene([&](Scene& scene) {
					if (scene.IsValid(collision.entityA) && scene.IsValid(collision.entityB)) {
						ScriptSystem::DispatchCollisionStay2D(scene, collision);
					}
				});
			});
	}

	void PhysicsSystem2D::Shutdown() {
		if (s_AxiomWorld) {
			s_AxiomWorld->Destroy();
			s_AxiomWorld.reset();
		}
		s_MainWorld.reset();
	}
}
