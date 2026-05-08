#include <pch.hpp>
#include <Components/Physics/PolygonCollider2DComponent.hpp>
#include <Components/General/Transform2DComponent.hpp>

#include <Scene/Scene.hpp>

#include <algorithm>
#include <cmath>

namespace Axiom {
	namespace {
		const Transform2DComponent* TryGetTransform(const Scene& scene, EntityHandle entity, const char* context) {
			if (entity == entt::null || !scene.IsValid(entity) || !scene.HasComponent<Transform2DComponent>(entity)) {
				AIM_CORE_WARN_TAG("PolygonCollider2D", "{} skipped because entity {} has no Transform2DComponent", context, static_cast<uint32_t>(entity));
				return nullptr;
			}

			return &scene.GetComponent<Transform2DComponent>(entity);
		}
	}

	std::vector<Vec2> PolygonCollider2DComponent::MakeRegularPolygon(int sides) {
		const int clamped = std::clamp(sides, k_MinVertices, k_MaxVertices);
		constexpr float kPi = 3.14159265358979323846f;

		std::vector<Vec2> points;
		points.reserve(static_cast<size_t>(clamped));
		for (int i = 0; i < clamped; ++i) {
			// Counter-clockwise winding starting at the top of the polygon.
			// b2ComputeHull doesn't care about winding (it sorts internally),
			// but a predictable order keeps inspector edits intuitive.
			const float angle = -static_cast<float>(i) * (2.0f * kPi) / static_cast<float>(clamped) + kPi * 0.5f;
			points.push_back(Vec2{ std::cos(angle) * 0.5f, std::sin(angle) * 0.5f });
		}
		return points;
	}

	PolygonCollider2DComponent::PolygonCollider2DComponent()
		: m_LocalPoints(MakeRegularPolygon(k_DefaultSides)) {}

	PolygonCollider2DComponent::PolygonCollider2DComponent(EntityHandle entity)
		: Collider2D(entity), m_LocalPoints(MakeRegularPolygon(k_DefaultSides)) {}

	void PolygonCollider2DComponent::RebuildPolygon(const Transform2DComponent& tr) {
		if (!b2Shape_IsValid(m_ShapeId)) return;

		const int count = static_cast<int>(m_LocalPoints.size());
		if (count < k_MinVertices || count > k_MaxVertices) {
			AIM_CORE_WARN_TAG("PolygonCollider2D", "Polygon has {} vertices, expected {}..{}; skipping rebuild",
				count, k_MinVertices, k_MaxVertices);
			return;
		}

		b2Vec2 worldPoints[k_MaxVertices];
		for (int i = 0; i < count; ++i) {
			worldPoints[i] = b2Vec2{
				m_LocalPoints[i].x * tr.Scale.x * m_LocalSize.x + m_Center.x,
				m_LocalPoints[i].y * tr.Scale.y * m_LocalSize.y + m_Center.y
			};
		}

		b2Hull hull = b2ComputeHull(worldPoints, count);
		if (hull.count < k_MinVertices) {
			AIM_CORE_WARN_TAG("PolygonCollider2D", "Failed to compute valid convex hull for entity {} (point count: {})",
				static_cast<uint32_t>(m_EntityHandle), count);
			return;
		}

		b2Polygon polygon = b2MakePolygon(&hull, 0.0f);
		b2Shape_SetPolygon(m_ShapeId, &polygon);

		m_LastAppliedScale = tr.Scale;
		m_LastAppliedLocalSize = m_LocalSize;
		m_LastAppliedCenter = m_Center;
		m_LastAppliedPoints = m_LocalPoints;
	}

	void PolygonCollider2DComponent::SetPoints(const std::vector<Vec2>& localPoints, const Scene& scene) {
		const Transform2DComponent* tr = TryGetTransform(scene, m_EntityHandle, "SetPoints");
		if (!tr) return;

		const int count = static_cast<int>(localPoints.size());
		if (count < k_MinVertices || count > k_MaxVertices) {
			AIM_CORE_WARN_TAG("PolygonCollider2D",
				"SetPoints: vertex count {} out of range [{}, {}]; ignoring",
				count, k_MinVertices, k_MaxVertices);
			return;
		}

		m_LocalPoints = localPoints;
		RebuildPolygon(*tr);
	}

	std::vector<Vec2> PolygonCollider2DComponent::GetWorldPoints() const {
		std::vector<Vec2> result;
		if (!b2Shape_IsValid(m_ShapeId)) return result;

		b2Polygon polygon = b2Shape_GetPolygon(m_ShapeId);
		result.reserve(static_cast<size_t>(polygon.count));
		for (int i = 0; i < polygon.count; ++i) {
			result.push_back(Vec2{ polygon.vertices[i].x, polygon.vertices[i].y });
		}
		return result;
	}

	void PolygonCollider2DComponent::SetSides(int sides, const Scene& scene) {
		const Transform2DComponent* tr = TryGetTransform(scene, m_EntityHandle, "SetSides");
		if (!tr) return;

		m_LocalPoints = MakeRegularPolygon(sides);
		RebuildPolygon(*tr);
	}

	void PolygonCollider2DComponent::SetSize(const Vec2& size, const Scene& scene) {
		const Transform2DComponent* tr = TryGetTransform(scene, m_EntityHandle, "SetSize");
		if (!tr) return;

		m_LocalSize = size;
		RebuildPolygon(*tr);
	}

	void PolygonCollider2DComponent::SetCenter(const Vec2& center, const Scene& scene) {
		const Transform2DComponent* tr = TryGetTransform(scene, m_EntityHandle, "SetCenter");
		if (!tr) return;

		m_Center = center;
		RebuildPolygon(*tr);
	}

	Vec2 PolygonCollider2DComponent::GetCenter() const {
		// Return the centroid of the current b2Polygon — a runtime mutation
		// (e.g. SetPoints with an off-center hull) shifts this away from
		// m_Center, and reads should reflect the actual shape state.
		if (!b2Shape_IsValid(m_ShapeId)) return m_Center;
		b2Polygon polygon = b2Shape_GetPolygon(m_ShapeId);
		return Vec2{ polygon.centroid.x, polygon.centroid.y };
	}

	void PolygonCollider2DComponent::SetEnabled(bool enabled) {
		if (!b2Body_IsValid(m_BodyId)) return;
		if (enabled)
			b2Body_Enable(m_BodyId);
		else
			b2Body_Disable(m_BodyId);
	}

	void PolygonCollider2DComponent::SetSensor(bool sensor, Scene& scene) {
		if (!IsValid() || IsSensor() == sensor) {
			return;
		}

		const Vec2 size = GetSize();
		const Vec2 center = GetCenter();
		const std::vector<Vec2> points = m_LocalPoints;
		const float friction = GetFriction();
		const float bounciness = GetBounciness();
		const uint64_t layer = GetLayer();
		const bool enabled = IsEnabled();
		const bool registerContacts = CanRegisterContacts();

		// See BoxCollider2DComponent::SetSensor for callback-snapshot rationale —
		// recreating the b2Shape mints a fresh id, so dispatcher entries keyed
		// on the old id must be migrated explicitly or they silently disappear.
		auto& dispatcher = PhysicsSystem2D::GetMainPhysicsWorld().GetDispatcher();
		auto savedCallbacks = dispatcher.SnapshotCallbacks(m_ShapeId);
		DestroyShape(false);
		m_ShapeId = PhysicsSystem2D::GetMainPhysicsWorld().CreateShape(m_EntityHandle, scene, m_BodyId, ShapeType::Polygon, sensor);
		dispatcher.RestoreCallbacks(m_ShapeId, std::move(savedCallbacks));
		// Restore custom vertices first so the next size/center rebuild uses them.
		m_LocalPoints = points;
		SetCenter(center, scene);
		SetSize(size, scene);
		SetFriction(friction);
		SetBounciness(bounciness);
		SetLayer(layer);
		SetRegisterContacts(registerContacts);
		SetEnabled(enabled);
	}

	void PolygonCollider2DComponent::SyncWithTransform(const Scene& scene) {
		const Transform2DComponent* tr = TryGetTransform(scene, m_EntityHandle, "SyncWithTransform");
		if (!tr) return;
		if (tr->Scale == m_LastAppliedScale
			&& m_LocalSize == m_LastAppliedLocalSize
			&& m_Center == m_LastAppliedCenter
			&& m_LocalPoints == m_LastAppliedPoints) {
			return;
		}
		if (!b2Shape_IsValid(m_ShapeId)) return;

		RebuildPolygon(*tr);
	}

	// Idempotent: destroy hooks fire from both Rigidbody2D and this component.
	void PolygonCollider2DComponent::Destroy() {
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
