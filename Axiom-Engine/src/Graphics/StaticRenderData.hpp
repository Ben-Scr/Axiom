#pragma once
#include "Collections/AABB.hpp"

namespace Axiom {
	// Per-entity render-time cache for entities tagged StaticTag. Lazily
	// populated by Renderer2D on first encounter and refreshed when the
	// transform values differ from the cached snapshot. Scene removes this via the
	// on_destroy<StaticTag> observer so that toggling the tag off and back on
	// starts with a clean cache instead of stale bounds.
	struct StaticRenderData {
		AABB CachedAABB{};
		Vec2 CachedPosition{};
		Vec2 CachedScale{};
		float CachedRotation = 0.0f;
		bool Valid = false;
	};
}
