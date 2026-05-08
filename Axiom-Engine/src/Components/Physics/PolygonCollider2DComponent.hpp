#pragma once
#include "Collider2D.hpp"
#include "Rigidbody2DComponent.hpp"
#include "Physics/PhysicsSystem2D.hpp"

#include <box2d/box2d.h>

#include <vector>

namespace Axiom {
	// Convex polygon collider backed by box2d's b2Polygon shape.
	// Vertex count is bounded by Box2D's B2_MAX_POLYGON_VERTICES (8); points
	// must form a convex hull (we run b2ComputeHull internally to guarantee this).
	class AXIOM_API PolygonCollider2DComponent : public Collider2D {
		friend class PhysicsSystem2D;
	public:
		// Box2D limit — polygons may have at most B2_MAX_POLYGON_VERTICES (8) vertices.
		static constexpr int k_MaxVertices = 8;
		static constexpr int k_MinVertices = 3;
		static constexpr int k_DefaultSides = 5;

		PolygonCollider2DComponent();
		explicit PolygonCollider2DComponent(EntityHandle entity);

		// Replace the polygon's local-space vertex list. Must contain
		// 3..k_MaxVertices points; out-of-range values are clamped and a
		// warning is logged. The points are convex-hulled before upload to
		// Box2D, so winding order doesn't matter.
		void SetPoints(const std::vector<Vec2>& localPoints, const Scene& scene);
		const std::vector<Vec2>& GetLocalPoints() const { return m_LocalPoints; }
		std::vector<Vec2> GetWorldPoints() const;
		int GetVertexCount() const { return static_cast<int>(m_LocalPoints.size()); }

		// Regenerate as a regular n-gon with sides in [k_MinVertices, k_MaxVertices].
		// Resets any custom vertex list back to a uniform polygon.
		void SetSides(int sides, const Scene& scene);
		int GetSides() const { return static_cast<int>(m_LocalPoints.size()); }

		// Per-axis scaling on top of the entity's transform scale, mirroring
		// BoxCollider2D's local size — lets the inspector tweak the polygon
		// without changing transform scale (which would also affect rendering).
		void SetSize(const Vec2& size, const Scene& scene);
		Vec2 GetSize() const { return m_LocalSize; }
		Vec2 GetLocalSize(const Scene& scene) const { (void)scene; return m_LocalSize; }

		void SetCenter(const Vec2& center, const Scene& scene);
		Vec2 GetCenter() const;

		void SetEnabled(bool enabled);
		void SetSensor(bool sensor, Scene& scene);

		void SyncWithTransform(const Scene& scene);
		void UpdatePolygon(const Scene& scene) { SyncWithTransform(scene); }

		void Destroy() override;

		// Build a regular convex polygon with the given side count, centered
		// at origin, fitting in a unit square. Used as the default vertex set.
		static std::vector<Vec2> MakeRegularPolygon(int sides);

	private:
		std::vector<Vec2> m_LocalPoints;
		Vec2 m_LocalSize{ 1.0f, 1.0f };
		Vec2 m_Center{ 0.0f, 0.0f };

		Vec2 m_LastAppliedScale{ 0.0f, 0.0f };
		Vec2 m_LastAppliedLocalSize{ 0.0f, 0.0f };
		Vec2 m_LastAppliedCenter{ 0.0f, 0.0f };
		std::vector<Vec2> m_LastAppliedPoints;

		void RebuildPolygon(const Transform2DComponent& tr);
	};
}
