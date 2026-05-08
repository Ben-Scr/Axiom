#include <pch.hpp>
#include <Components/Physics/CircleCollider2DComponent.hpp>
#include <Components/General/Transform2DComponent.hpp>

#include <Scene/Scene.hpp>

namespace Axiom {
	namespace {
		const Transform2DComponent* TryGetTransform(const Scene& scene, EntityHandle entity, const char* context) {
			if (entity == entt::null || !scene.IsValid(entity) || !scene.HasComponent<Transform2DComponent>(entity)) {
				AIM_CORE_WARN_TAG("CircleCollider2D", "{} skipped because entity {} has no Transform2DComponent", context, static_cast<uint32_t>(entity));
				return nullptr;
			}

			return &scene.GetComponent<Transform2DComponent>(entity);
		}

		// A circle has a single radius, but the entity transform is anisotropic.
		// Pick the larger axis so non-uniform scaling still grows the circle —
		// Unity's CircleCollider2D uses the same heuristic.
		float ComputeWorldRadius(const Transform2DComponent& tr, float localRadius) {
			return localRadius * std::max(std::abs(tr.Scale.x), std::abs(tr.Scale.y));
		}
	}

	void CircleCollider2DComponent::SetRadius(float radius, const Scene& scene) {
		const Transform2DComponent* tr = TryGetTransform(scene, m_EntityHandle, "SetRadius");
		if (!tr) return;

		m_LocalRadius = radius;

		if (!b2Shape_IsValid(m_ShapeId)) return;

		const Vec2 center = this->GetCenter();
		b2Circle circle{ b2Vec2{ center.x, center.y }, ComputeWorldRadius(*tr, radius) };
		b2Shape_SetCircle(m_ShapeId, &circle);
		m_LastAppliedScale = tr->Scale;
		m_LastAppliedLocalRadius = m_LocalRadius;
	}

	void CircleCollider2DComponent::SetEnabled(bool enabled) {
		if (!b2Body_IsValid(m_BodyId)) return;
		if (enabled)
			b2Body_Enable(m_BodyId);
		else
			b2Body_Disable(m_BodyId);
	}

	void CircleCollider2DComponent::SetSensor(bool sensor, Scene& scene) {
		if (!IsValid() || IsSensor() == sensor) {
			return;
		}

		const float localRadius = GetLocalRadius(scene);
		const Vec2 center = GetCenter();
		const float friction = GetFriction();
		const float bounciness = GetBounciness();
		const uint64_t layer = GetLayer();
		const bool enabled = IsEnabled();
		const bool registerContacts = CanRegisterContacts();

		// Snapshot collision callbacks BEFORE DestroyShape so the recreated b2Shape
		// (with a fresh id) inherits the user's OnCollisionEnter/Exit/Hit handlers.
		// See BoxCollider2DComponent::SetSensor for the rationale — every Box2D-backed
		// collider on the same dispatcher needs this guard or callbacks silently
		// disappear when IsTrigger is toggled.
		auto& dispatcher = PhysicsSystem2D::GetMainPhysicsWorld().GetDispatcher();
		auto savedCallbacks = dispatcher.SnapshotCallbacks(m_ShapeId);
		DestroyShape(false);
		m_ShapeId = PhysicsSystem2D::GetMainPhysicsWorld().CreateShape(m_EntityHandle, scene, m_BodyId, ShapeType::Circle, sensor);
		dispatcher.RestoreCallbacks(m_ShapeId, std::move(savedCallbacks));
		SetCenter(center, scene);
		SetRadius(localRadius, scene);
		SetFriction(friction);
		SetBounciness(bounciness);
		SetLayer(layer);
		SetRegisterContacts(registerContacts);
		SetEnabled(enabled);
	}

	float CircleCollider2DComponent::GetRadius() const {
		if (!b2Shape_IsValid(m_ShapeId)) return 0.0f;
		b2ShapeType shapeType = b2Shape_GetType(m_ShapeId);

		AIM_ASSERT(shapeType == b2_circleShape, AxiomErrorCode::Undefined, "This b2shape type isn't type of b2_circleShape");

		b2Circle circle = b2Shape_GetCircle(m_ShapeId);
		return circle.radius;
	}

	float CircleCollider2DComponent::GetLocalRadius(const Scene& scene) const {
		(void)scene;
		return m_LocalRadius;
	}

	void CircleCollider2DComponent::SyncWithTransform(const Scene& scene) {
		const Transform2DComponent* tr = TryGetTransform(scene, m_EntityHandle, "SyncWithTransform");
		if (!tr) return;
		if (tr->Scale == m_LastAppliedScale && m_LocalRadius == m_LastAppliedLocalRadius) return;
		if (!b2Shape_IsValid(m_ShapeId)) return;

		const Vec2 center = this->GetCenter();
		b2Circle circle{ b2Vec2{ center.x, center.y }, ComputeWorldRadius(*tr, m_LocalRadius) };
		b2Shape_SetCircle(m_ShapeId, &circle);
		m_LastAppliedScale = tr->Scale;
		m_LastAppliedLocalRadius = m_LocalRadius;
	}

	void CircleCollider2DComponent::SetCenter(const Vec2& center, const Scene& scene) {
		const Transform2DComponent* tr = TryGetTransform(scene, m_EntityHandle, "SetCenter");
		if (!tr) return;
		if (!b2Shape_IsValid(m_ShapeId)) return;

		b2Circle circle{ b2Vec2{ center.x, center.y }, ComputeWorldRadius(*tr, m_LocalRadius) };
		b2Shape_SetCircle(m_ShapeId, &circle);
		m_LastAppliedScale = tr->Scale;
		m_LastAppliedLocalRadius = m_LocalRadius;
	}

	Vec2 CircleCollider2DComponent::GetCenter() const {
		if (!b2Shape_IsValid(m_ShapeId)) return Vec2{ 0.0f, 0.0f };
		b2Circle circle = b2Shape_GetCircle(m_ShapeId);
		return Vec2(circle.center.x, circle.center.y);
	}

	// Idempotent: destroy hooks fire from both Rigidbody2D and this component.
	void CircleCollider2DComponent::Destroy() {
		if (b2Shape_IsValid(m_ShapeId)) {
			DestroyShape(true);
		}
		if (b2Body_IsValid(m_BodyId)) {
			if (PhysicsSystem2D::IsInitialized()) {
				PhysicsSystem2D::GetMainPhysicsWorld().UnregisterBodyBinding(m_BodyId);
			}
			b2DestroyBody(m_BodyId);
		}
		m_ShapeId = b2_nullShapeId;
		m_BodyId = b2_nullBodyId;
	}
}
