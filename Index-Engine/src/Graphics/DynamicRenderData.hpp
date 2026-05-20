#pragma once
#include "Collections/AABB.hpp"

namespace Index {
	// Per-entity render-time cache for active (non-static, non-disabled)
	// sprite entities. Mirrors StaticRenderData but lives on the dynamic
	// path: lazily populated by Renderer2D on first encounter, refreshed
	// when the transform values differ from the cached snapshot. The cache
	// pays off on rotated sprites that hold still (or change rotation
	// infrequently), where CreateQuadAABB's per-frame sin/cos becomes the
	// dominant per-sprite arithmetic at 100k+ scenes.
	//
	// Separate component (rather than a field on Transform2DComponent) so
	// the cache lives next to other render-only state and doesn't pollute
	// Transform's hot cache line — physics, scripts, and the hierarchy
	// system touch Transform every frame and benefit from it staying small.
	//
	// Cleared via on_destroy<SpriteRendererComponent> in Renderer2D so a
	// reused entity slot doesn't carry stale bounds from the previous
	// occupant.
	struct DynamicRenderData {
		AABB CachedAABB{};
		Vec2 CachedPosition{};
		Vec2 CachedScale{};
		float CachedRotation = 0.0f;
		bool Valid = false;
	};
}
