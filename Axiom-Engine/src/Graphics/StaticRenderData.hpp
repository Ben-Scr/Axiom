#pragma once
#include "Collections/AABB.hpp"

namespace Axiom {
	// Per-entity render-time cache for entities tagged StaticTag. Lazily
	// populated by Renderer2D on first encounter and refreshed when the
	// Transform2DComponent dirty flag flips true. Scene removes this via the
	// on_destroy<StaticTag> observer so that toggling the tag off and back on
	// starts with a clean cache instead of stale bounds.
	struct StaticRenderData {
		AABB CachedAABB{};
		bool Valid = false;
	};
}
