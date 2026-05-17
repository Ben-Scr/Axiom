#pragma once

// NativeEntityCommandBuffer
// =========================
// Batch-record entity creations and component additions from native (C++)
// code, then play the whole batch back to a Scene in a single pass. This is
// the native-side counterpart to the managed `EntityCommandBuffer` (see
// Index-ScriptCore/Source/Index/Scene/EntityCommandBuffer.cs) and reuses the
// same engine fast-path primitives (`Scene::ReserveForLoadRuntime`,
// `Scene::CreateEntitiesBulk`, `Scene::LoadGuard`, `Scene::MarkAllDirtyOnce`).
//
// Why it exists:
//   - Spawning N entities one-by-one through `Scene::CreateEntity` +
//     `AddComponent<T>` pays per-entity pool growth, identity-map rehash and
//     on_construct hook costs N times. The ECB collapses all of those into
//     one sweep — typically 20-50x faster on tight spawn loops.
//   - On the **managed** ECB path, component payloads travel as a byte blob
//     (memcpy from C# into native storage). That blob is whatever the C# side
//     produced, so a `default(NativeTransform2D)` ends up with Scale = (0,0)
//     in the EnTT pool — defaults are silently lost. The native ECB
//     SIDESTEPS this entirely by storing typed commands and either default-
//     constructing in-place (C++ member initializers fire) or applying a
//     user-supplied patch on top of a default-constructed component. As a
//     result, `ecb.AddComponent<Transform2DComponent>(e)` always produces a
//     Transform with Scale = (1,1), regardless of how the caller built the
//     buffer.
//
// Thread-safety:
//   A single NativeEntityCommandBuffer is NOT safe to record from multiple
//   threads concurrently. For parallel-record patterns use
//   `ParallelEntityCommandBuffer`, which fans out into per-worker
//   NativeEntityCommandBuffers and merges them at playback.

#include "Core/Export.hpp"
#include "Scene/EntityHandle.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

namespace Index {

	class Scene;

	class INDEX_API NativeEntityCommandBuffer {
	public:
		// Opaque handle to an entity recorded by this buffer. The wrapped
		// index is local to the buffer — it cannot be cross-used between
		// instances and is invalidated by Clear()/Playback().
		struct EntityRef {
			uint32_t Index;
		};

		NativeEntityCommandBuffer() = default;
		~NativeEntityCommandBuffer();

		NativeEntityCommandBuffer(const NativeEntityCommandBuffer&) = delete;
		NativeEntityCommandBuffer& operator=(const NativeEntityCommandBuffer&) = delete;
		NativeEntityCommandBuffer(NativeEntityCommandBuffer&&) noexcept = default;
		NativeEntityCommandBuffer& operator=(NativeEntityCommandBuffer&&) noexcept = default;

		// Records the creation of a fresh runtime-origin entity and returns
		// a handle that subsequent AddComponent calls reference. The handle
		// is stable until Clear() or destruction.
		EntityRef CreateEntity();

		// Add a component using its C++ default constructor. For
		// Transform2DComponent this gives Scale = (1,1); for
		// SpriteRendererComponent this gives Color = white. Use this when
		// you want the engine's defaults to apply.
		template <typename T>
		std::enable_if_t<!std::is_empty_v<T>> AddComponent(EntityRef e);

		// Add a component with a fully-built value. The value is moved into
		// the buffer's inline storage and emplaced verbatim at playback.
		template <typename T>
		std::enable_if_t<!std::is_empty_v<T>> AddComponent(EntityRef e, T value);

		// Add a component, then run `configure(component)` on the default-
		// constructed instance to apply only the fields you care about. The
		// engine defaults stick for every field you don't touch:
		//
		//   ecb.AddComponent<Transform2DComponent>(e, [&](auto& tr) {
		//       tr.Position = spawnPos;
		//   });
		//   // Scale is (1,1), Rotation is 0 — exactly the C++ defaults.
		//
		// The Configure callable is captured by value into the buffer's
		// small-buffer-optimized storage (32 bytes). Captures larger than
		// that fail to compile with a clear message — split the body into
		// referenced state instead of inlining heavy captures.
		template <typename T, typename Configure>
		std::enable_if_t<!std::is_empty_v<T>>
		AddComponent(EntityRef e, Configure&& configure);

		// Tag (empty-type) variant. No payload, no patch.
		template <typename T>
		std::enable_if_t<std::is_empty_v<T>> AddComponent(EntityRef e);

		// Ships the recorded batch to `scene`. Mirrors the managed ECB's
		// fast path: ReserveForLoadRuntime -> CreateEntitiesBulk -> LoadGuard
		// scope -> stamp metadata -> walk commands -> MarkAllDirtyOnce. After
		// return the created entity handles are addressable via
		// GetCreatedEntity(i). Returns the number of entities created.
		int Playback(Scene& scene);

		// Discards every recorded command without releasing the backing
		// buffer. Runs each command's per-state destructor first (no-op for
		// trivially-destructible captures, so per-frame spawn loops with
		// trivial components stay realloc- AND destruct-free).
		void Clear();

		// Number of entities queued so far (before Playback). After
		// Playback, this is the number of entities that were created.
		int EntityCount() const { return static_cast<int>(m_EntityCount); }

		// Number of recorded AddComponent commands so far.
		int CommandCount() const { return static_cast<int>(m_Commands.size()); }

		// Handle of the i-th created entity. Only valid after a successful
		// Playback and until the next Clear(). Index matches EntityRef::Index.
		EntityHandle GetCreatedEntity(int i) const;

		// Size of the inline storage available to a per-command capture.
		// Exposed as a constant so users can static_assert at the call site
		// if they want a friendlier diagnostic than the template's own
		// internal assert.
		static constexpr std::size_t kInlineStorageBytes = 32;

	private:
		// One per recorded AddComponent. Fits in 56 bytes on x64 — well
		// inside a single cache line.
		struct Command {
			uint32_t EntityIndex;
			// Type-erased applier. `state` points at Storage.
			void (*Apply)(void* state, entt::registry& registry, EntityHandle handle);
			// Type-erased destructor for the captured state. nullptr for
			// trivially-destructible captures so Clear() can skip the call.
			void (*Destroy)(void* state);
			alignas(std::max_align_t) std::byte Storage[kInlineStorageBytes];
		};
		static_assert(sizeof(Command) <= 64, "Command must fit in one cache line");

		// Reuses storage across Clear() cycles — per-frame spawn loops stay
		// allocation-free once the buffer reaches steady-state size.
		std::vector<Command>      m_Commands;
		std::vector<EntityHandle> m_CreatedHandles;
		uint32_t                  m_EntityCount = 0;

		// Internal helper used by every AddComponent overload. Reserves a
		// fresh Command slot in m_Commands and returns a pointer to its
		// state storage so the caller can placement-new the capture there.
		Command& AppendCommand(uint32_t entityIndex,
			void (*apply)(void*, entt::registry&, EntityHandle),
			void (*destroy)(void*));

		// Lets ParallelEntityCommandBuffer reach into the merge details
		// (entity count and command list) without exposing them publicly.
		friend class ParallelEntityCommandBuffer;
	};

	// ───────────────────────────── inline template definitions ───────────

	template <typename T>
	std::enable_if_t<!std::is_empty_v<T>>
	NativeEntityCommandBuffer::AddComponent(EntityRef e) {
		// Default-construct only — captures nothing, so Destroy is null
		// and Apply is a stateless thunk that compiles to one call.
		AppendCommand(e.Index,
			[](void*, entt::registry& registry, EntityHandle handle) {
				if constexpr (std::is_default_constructible_v<T>) {
					registry.emplace<T>(handle);
				}
				else {
					static_assert(std::is_default_constructible_v<T>,
						"AddComponent<T>(e) requires T to be default-constructible. "
						"Use the value or patch overload for components without a "
						"default constructor.");
				}
			},
			nullptr);
	}

	template <typename T>
	std::enable_if_t<!std::is_empty_v<T>>
	NativeEntityCommandBuffer::AddComponent(EntityRef e, T value) {
		// Move the value into the command's inline storage. emplace<T> uses
		// T(T&&) at playback so callers control their own field values
		// without the default-construct + assign round-trip.
		static_assert(sizeof(T) <= kInlineStorageBytes,
			"Component value is larger than the ECB inline storage (32 bytes). "
			"Use the patch overload (lambda) and refer to heap state by "
			"reference, or split this component.");
		static_assert(alignof(T) <= alignof(std::max_align_t),
			"Component alignment exceeds NativeEntityCommandBuffer storage alignment.");

		Command& cmd = AppendCommand(e.Index,
			[](void* state, entt::registry& registry, EntityHandle handle) {
				T* held = std::launder(reinterpret_cast<T*>(state));
				registry.emplace<T>(handle, std::move(*held));
			},
			std::is_trivially_destructible_v<T>
				? nullptr
				: +[](void* state) {
					std::launder(reinterpret_cast<T*>(state))->~T();
				});
		::new (cmd.Storage) T(std::move(value));
	}

	template <typename T, typename Configure>
	std::enable_if_t<!std::is_empty_v<T>>
	NativeEntityCommandBuffer::AddComponent(EntityRef e, Configure&& configure) {
		using Patch = std::decay_t<Configure>;
		static_assert(sizeof(Patch) <= kInlineStorageBytes,
			"Captured patch lambda is larger than the ECB inline storage "
			"(32 bytes). Capture less by-value, or close over a pointer to "
			"the heap state instead.");
		static_assert(alignof(Patch) <= alignof(std::max_align_t),
			"Patch alignment exceeds NativeEntityCommandBuffer storage alignment.");
		static_assert(std::is_default_constructible_v<T>,
			"AddComponent<T>(e, configure) default-constructs T before applying "
			"the patch. T must be default-constructible — use the value overload "
			"for components without a default constructor.");
		static_assert(std::is_invocable_v<Patch&, T&>,
			"Patch callable must be invocable as (T&). Example shape: "
			"[](Transform2DComponent& tr) { tr.Position = ...; }");

		Command& cmd = AppendCommand(e.Index,
			[](void* state, entt::registry& registry, EntityHandle handle) {
				Patch* patch = std::launder(reinterpret_cast<Patch*>(state));
				T& component = registry.emplace<T>(handle);
				(*patch)(component);
			},
			std::is_trivially_destructible_v<Patch>
				? nullptr
				: +[](void* state) {
					std::launder(reinterpret_cast<Patch*>(state))->~Patch();
				});
		::new (cmd.Storage) Patch(std::forward<Configure>(configure));
	}

	template <typename T>
	std::enable_if_t<std::is_empty_v<T>>
	NativeEntityCommandBuffer::AddComponent(EntityRef e) {
		AppendCommand(e.Index,
			[](void*, entt::registry& registry, EntityHandle handle) {
				registry.emplace<T>(handle);
			},
			nullptr);
	}

} // namespace Index
