#include "pch.hpp"
#include <Components/Physics/Collider2D.hpp>
#include <Components/General/Transform2DComponent.hpp>

#include <Math/Trigonometry.hpp>


namespace Axiom {
	// Shape-validity guards on every b2Shape_* accessor: a collider can outlive
	// its underlying b2Shape (SetSensor rebuilds the shape, scene unload destroys
	// it, etc.) while still holding the old m_ShapeId. Calling b2Shape_* with a
	// stale id is undefined behavior — Box2D may mutate an unrelated shape that
	// happened to land in the same recycled slot.
	#define AIM_COLLIDER_SHAPE_GUARD() do { if (!b2Shape_IsValid(m_ShapeId)) return; } while (0)
	#define AIM_COLLIDER_BODY_GUARD()  do { if (!b2Body_IsValid(m_BodyId))   return; } while (0)

	bool Collider2D::IsValid() const { return b2Body_IsValid(m_BodyId) && b2Shape_IsValid(m_ShapeId); }

	void Collider2D::SetFriction(float friction) {
		AIM_COLLIDER_SHAPE_GUARD();
		b2Shape_SetFriction(m_ShapeId, friction);
	}
	float Collider2D::GetFriction() const {
		if (!b2Shape_IsValid(m_ShapeId)) return 0.0f;
		return b2Shape_GetFriction(m_ShapeId);
	}
	void Collider2D::SetBounciness(float bounciness) {
		AIM_COLLIDER_SHAPE_GUARD();
		b2Shape_SetRestitution(m_ShapeId, bounciness);
	}
	float Collider2D::GetBounciness() const {
		if (!b2Shape_IsValid(m_ShapeId)) return 0.0f;
		return b2Shape_GetRestitution(m_ShapeId);
	}
	void Collider2D::SetLayer(uint64_t layer) {
		AIM_COLLIDER_SHAPE_GUARD();
		b2Filter filter = b2Shape_GetFilter(m_ShapeId);
		filter.categoryBits = layer;
		b2Shape_SetFilter(m_ShapeId, filter);
	}
	uint64_t Collider2D::GetLayer() const {
		if (!b2Shape_IsValid(m_ShapeId)) return 0;
		return b2Shape_GetFilter(m_ShapeId).categoryBits;
	}
	bool Collider2D::IsEnabled() const {
		if (!b2Body_IsValid(m_BodyId)) return false;
		return b2Body_IsEnabled(m_BodyId);
	}
	bool Collider2D::IsSensor() const {
		if (!b2Shape_IsValid(m_ShapeId)) return false;
		return b2Shape_IsSensor(m_ShapeId);
	}

	Vec2 Collider2D::GetBodyPosition() const {
		if (!b2Body_IsValid(m_BodyId)) return Vec2{ 0.0f, 0.0f };
		b2Vec2 position = b2Body_GetPosition(m_BodyId);
		return { position.x, position.y };
	}
	float Collider2D::GetRotationDegrees() const {
		if (!b2Body_IsValid(m_BodyId)) return 0.0f;
		// Negate to match Rigidbody2DComponent::GetRotation and the engine's b2Rot(cos,-sin)
		// convention. Without negation, get(set(angle)) would not be the identity.
		return Degrees<float>(-b2Rot_GetAngle(b2Body_GetRotation(m_BodyId)));
	}
	float Collider2D::GetRotationRadiant() const {
		if (!b2Body_IsValid(m_BodyId)) return 0.0f;
		return -b2Rot_GetAngle(b2Body_GetRotation(m_BodyId));
	}

	void Collider2D::SetRegisterContacts(bool enabled) {
		AIM_COLLIDER_SHAPE_GUARD();
		b2Shape_EnableContactEvents(m_ShapeId, enabled);
	}
	bool Collider2D::CanRegisterContacts() const {
		if (!b2Shape_IsValid(m_ShapeId)) return false;
		return b2Shape_AreContactEventsEnabled(m_ShapeId);
	}


	void Collider2D::EnableRotation(bool enabled) {
		AIM_COLLIDER_SHAPE_GUARD();
		b2Shape_SetDensity(m_ShapeId, enabled ? 1.f : 0.f, false);
	}


	void Collider2D::DestroyShape(bool updateBodyMass) {
		const bool hasShapeHandle = b2StoreShapeId(m_ShapeId) != b2StoreShapeId(b2_nullShapeId);
		if (hasShapeHandle) {
			PhysicsSystem2D::GetMainPhysicsWorld().GetDispatcher().UnregisterShape(m_ShapeId);
		}

		if (b2Shape_IsValid(m_ShapeId)) {
			b2DestroyShape(m_ShapeId, updateBodyMass);
		}

		m_ShapeId = b2_nullShapeId;
	}

	void Collider2D::Destroy() {
		DestroyShape(true);
		if (b2Body_IsValid(m_BodyId)) {
			if (PhysicsSystem2D::IsInitialized()) {
				PhysicsSystem2D::GetMainPhysicsWorld().UnregisterBodyBinding(m_BodyId);
			}
			b2DestroyBody(m_BodyId);
		}

		m_BodyId = b2_nullBodyId;
	}

	void Collider2D::SetRotation(float radiant) {
		AIM_COLLIDER_BODY_GUARD();
		// Sign convention matches Rigidbody2DComponent::SetRotation and the canonical
		// Transform2DComponent::GetB2Rotation used everywhere else in the engine
		// (b2Rot(cos, -sin)). The original positive-sin version produced a conjugate
		// b2Rot for the same input, so reading the rotation off the collider,
		// transforming it, and writing it back to the rigidbody flipped the sign.
		b2Body_SetTransform(m_BodyId, b2Body_GetPosition(m_BodyId), b2Rot(std::cos(radiant), -std::sin(radiant)));
	}
	void Collider2D::SetPositionRotation(const Vec2& position, float radiant) {
		AIM_COLLIDER_BODY_GUARD();
		b2Body_SetTransform(m_BodyId, b2Vec2(position.x, position.y), b2Rot(std::cos(radiant), -std::sin(radiant)));
	}
	void Collider2D::SetPosition(const Vec2& position) {
		AIM_COLLIDER_BODY_GUARD();
		b2Body_SetTransform(m_BodyId, b2Vec2(position.x, position.y), b2Body_GetRotation(m_BodyId));
	}
	void Collider2D::SetTransform(const Transform2DComponent& tr) {
		AIM_COLLIDER_BODY_GUARD();
		b2Body_SetTransform(m_BodyId, b2Vec2(tr.Position.x, tr.Position.y), tr.GetB2Rotation());
	}
}
