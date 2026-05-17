#pragma once

// EntityForEach
// =============
// Ergonomic single- and multi-threaded iteration over entities that have a
// given set of components. Layered on top of `entt::registry::view<...>`
// (for the query) and `Index::ParallelFor` (for the parallel variant) —
// these helpers introduce no new job type and no new scheduling primitive.
//
// The native scripting model in Index relies on closures: any
// `JobSystem::Enqueue` body, any `ParallelFor` chunk, can already do
// whatever ECS work it wants by calling into the registry. These helpers
// just spare the user from writing the boilerplate
// (`view = registry.view<...>()`, `for (auto e : view) view.get<T>(e)`)
// every time — they are the equivalent of Unity DOTS' IJobEntity ergonomic
// without inventing a parallel job-type hierarchy.
//
// Two variants:
//
//   ForEach<Components...>(scene, body)
//       Synchronous, single-threaded. Calls body(component_refs...) for
//       every matching entity in the scene's registry.
//
//   ParallelForEach<Components...>(scene, body)
//       Chunks the entity set and dispatches each chunk through
//       ParallelFor. body runs on a worker thread per chunk.
//
// Body shape:
//
//   ParallelForEach<Transform2DComponent, VelocityComponent>(scene,
//       [&](Transform2DComponent& tr, VelocityComponent& vel) {
//           tr.Position += vel.Velocity * dt;
//       });
//
//   // Optionally take an entity handle as the first arg:
//   ParallelForEach<Transform2DComponent>(scene,
//       [&](EntityHandle e, Transform2DComponent& tr) {
//           // ...
//       });
//
// Thread-safety:
//   ParallelForEach reads/writes the listed components from multiple worker
//   threads. Writes to the SAME component on the SAME entity from multiple
//   workers are UB — chunk boundaries guarantee that each entity is visited
//   by exactly one worker, so this is only a problem if your body crosses
//   into a different entity's storage (e.g. via a global lookup). For
//   structural changes (entity creation, AddComponent on a *different*
//   entity, RemoveComponent), use a ParallelEntityCommandBuffer and play it
//   back after the join.

#include "Core/Export.hpp"
#include "Jobs/ParallelFor.hpp"
#include "Scene/EntityHandle.hpp"
#include "Scene/Scene.hpp"

#include <type_traits>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

namespace Index {

	namespace Detail {

		// Two body shapes are supported. Picks at compile time so the call
		// site doesn't need a separate overload name per shape:
		//   (Components&...)              — components only
		//   (EntityHandle, Components&...) — entity handle + components
		template <typename Body, typename View, typename... Components>
		inline void InvokeForEachBody(Body& body, View& view, EntityHandle entity) {
			if constexpr (std::is_invocable_v<Body&, EntityHandle, Components&...>) {
				body(entity, view.template get<Components>(entity)...);
			}
			else if constexpr (std::is_invocable_v<Body&, Components&...>) {
				body(view.template get<Components>(entity)...);
			}
			else {
				static_assert(std::is_invocable_v<Body&, Components&...>,
					"ForEach body must be invocable with (Components&...) or "
					"(EntityHandle, Components&...).");
			}
		}

	} // namespace Detail

	// Synchronous, single-threaded ForEach over all entities matching
	// Components.... See file header for body shape and usage.
	template <typename... Components, typename Body>
	void ForEach(Scene& scene, Body&& body) {
		auto view = scene.GetRegistry().view<Components...>();
		for (auto entity : view) {
			Detail::InvokeForEachBody<Body, decltype(view), Components...>(
				body, view, entity);
		}
	}

	// Parallel ForEach over all entities matching Components.... Chunks
	// the entity set across JobSystem workers via ParallelFor. Blocks until
	// every chunk finishes; safe to call from inside another ParallelFor
	// because ParallelFor work-steals while waiting (no deadlock on nested
	// dispatches). See file header for the threading contract.
	template <typename... Components, typename Body>
	void ParallelForEach(Scene& scene, Body&& body, std::size_t grainSize = 0) {
		auto view = scene.GetRegistry().view<Components...>();

		// Snapshot entities into a flat vector so each chunk gets a stable
		// random-access range. entt views aren't random-access, and the
		// chunk dispatcher needs O(1) index → entity to slice cleanly.
		// The cost of this snapshot is O(N) once per call — typically far
		// less than the per-entity work that follows. Reserve the
		// underlying contiguous size if the view exposes one (entt views
		// have a fast .size_hint() for single-component views and an exact
		// .size() for ones backed by a group).
		std::vector<EntityHandle> entities;
		entities.reserve(static_cast<std::size_t>(view.size_hint()));
		for (auto entity : view) {
			entities.push_back(entity);
		}

		if (entities.empty()) {
			return;
		}

		// Body is captured by const-ref into the chunk closure — the user's
		// lambda lives in the calling stack frame and stays alive across
		// the ParallelFor (which Wait()s before returning).
		ParallelFor(0, entities.size(),
			[&](std::size_t lo, std::size_t hi) {
				for (std::size_t i = lo; i < hi; ++i) {
					Detail::InvokeForEachBody<Body, decltype(view), Components...>(
						body, view, entities[i]);
				}
			},
			grainSize);
	}

} // namespace Index
