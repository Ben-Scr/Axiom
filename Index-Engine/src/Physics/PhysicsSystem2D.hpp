#pragma once
#include <optional>
#include "Core/Export.hpp"
#include "Physics/Box2DWorld.hpp"
#include "Physics/IndexPhysicsWorld2D.hpp"
#include "Physics/IPhysicsEngine.hpp"

namespace Index {

	class INDEX_API PhysicsSystem2D : public IPhysicsEngine {
	public:
		~PhysicsSystem2D() override = default;

		void FixedUpdate(float dt) override;
		// Editor-mode + play-mode sync. No simulation step.
		void Update() override;

		// Reset Box2D's sleep timer on every Rigidbody2D body across all
		// loaded scenes. Call when entering play mode so bodies that sat
		// in editor mode aren't immediately considered idle.
		static void WakeAllBodies();

		void Initialize() override;
		void Shutdown() override;

		static Box2DWorld& GetMainPhysicsWorld() { return s_MainWorld.value(); }
		static IndexPhysicsWorld2D& GetIndexPhysicsWorld() { return s_IndexWorld.value(); }
		static bool IsInitialized() { return s_MainWorld.has_value() && s_IndexWorld.has_value(); }
		static bool IsEnabled() { return s_IsEnabled; };
		static void SetEnabled(bool enabled) { s_IsEnabled = enabled; }
	private:
		void SyncTransformsToPhysics();

		static std::optional<Box2DWorld> s_MainWorld;
		static std::optional<IndexPhysicsWorld2D> s_IndexWorld;
		static bool s_IsEnabled;
	};
}
