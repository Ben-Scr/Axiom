#pragma once
#include "Collider2D.hpp"
#include "Rigidbody2DComponent.hpp"
#include "Physics/PhysicsSystem2D.hpp"

#include <box2d/box2d.h>

namespace Axiom {
	class AXIOM_API CircleCollider2DComponent : public Collider2D {
		friend class PhysicsSystem2D;
	public:
		CircleCollider2DComponent() = default;
		explicit CircleCollider2DComponent(EntityHandle entity)
			: Collider2D(entity) {}

		void SetRadius(float radius, const Scene& scene);
		void SetEnabled(bool enabled);
		void SetSensor(bool sensor, Scene& scene);
		float GetRadius() const;
		float GetLocalRadius(const Scene& scene) const;

		void SyncWithTransform(const Scene& scene);
		void UpdateRadius(const Scene& scene) { SyncWithTransform(scene); }

		void SetCenter(const Vec2& center, const Scene& scene);
		Vec2 GetCenter() const;

		void Destroy() override;
	private:
		float m_LocalRadius{ 0.5f };
		Vec2 m_LastAppliedScale{ 0.0f, 0.0f };
		// Track the local radius used during the last circle rebuild so
		// SyncWithTransform doesn't short-circuit when the inspector edits
		// m_LocalRadius without changing the transform.
		float m_LastAppliedLocalRadius{ 0.0f };
	};
}
