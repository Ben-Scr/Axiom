#pragma once

#include "Core/Export.hpp"
#include "Scene/EntityHandle.hpp"

#if defined(INDEX_WITH_EDITOR)
// Full include (not forward decl): the std::unique_ptr<FileWatcher> member
// below requires a complete type at every TU that instantiates the cache's
// implicit destructor.
#include "Serialization/FileWatcher.hpp"
#endif

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Index {

	class Scene;

	// One-time-baked prefab representation. The first slow-path hydrate of
	// a prefab populates this struct by walking the live entity tree and
	// serializing each component via the registry's writeBytes callback;
	// every subsequent spawn replays from `payloadBlob` through the same
	// emplaceFromBytes path the EntityCommandBuffer uses, skipping the
	// disk read, the JSON parse, and the per-property setter loop entirely.
	//
	// Layout decisions:
	//   * Entities live in `entities` in a stable depth-first order
	//     starting from the prefab root. parentIndex == UINT32_MAX means
	//     "root"; child entities point at their parent's slot index.
	//   * Names live separately in EntitySlot::name rather than in the
	//     byte blob because NameComponent owns a std::string whose raw
	//     bytes would alias source-entity heap memory after a memcpy.
	//   * Component bytes pack contiguously into `payloadBlob` so a single
	//     allocation drops the cache in cache-friendly stride and every
	//     hydrate copies from one source buffer.
	struct PrefabTemplate {
		struct EntitySlot {
			uint32_t parentIndex = UINT32_MAX;   // UINT32_MAX = root
			std::string name;                     // captured separately from byte blob
			uint64_t sourceUuid = 0;              // diagnostics only
			uint32_t componentRecordBegin = 0;    // index into `components`
			uint32_t componentRecordEnd = 0;      // exclusive
		};
		std::vector<EntitySlot> entities;

		struct ComponentRecord {
			uint32_t typeIdU32 = 0;
			uint32_t byteOffset = 0;              // into payloadBlob
			uint32_t byteSize = 0;
		};
		std::vector<ComponentRecord> components;
		std::vector<uint8_t> payloadBlob;

		// V1 limitation flag. False means the prefab contains internal
		// entity refs (or a component whose writeBytes is not wired);
		// callers must fall back to the slow path until a follow-up adds
		// template-local ref fixup capture.
		bool bakeable = true;
		std::string unbakeableReason;

		// Total entity count == entities.size(). Exposed as a u32 to
		// match the EnTityCommandBuffer wire format that consumes this
		// cache during ECB.InstantiatePrefab playback.
		uint32_t EntityCount() const { return static_cast<uint32_t>(entities.size()); }
	};

	// Process-wide singleton that owns every baked PrefabTemplate. The
	// cache is populated lazily: SceneSerializer::InstantiatePrefab runs
	// the existing slow path on first request and immediately calls
	// CaptureFromLive on the resulting root so the template is ready for
	// every subsequent spawn. Invalidated wholesale on script hot-reload
	// (component layouts can shift) and, in editor builds only, on prefab
	// asset edits detected by an embedded FileWatcher.
	class INDEX_API PrefabTemplateCache {
	public:
		static PrefabTemplateCache& Get();

		// Returns the cached template, or nullptr if none exists yet.
		// Caller responsibility: on null, run the slow path and call
		// CaptureFromLive afterwards. Thread-safe (shared lock).
		const PrefabTemplate* Find(uint64_t prefabGuid) const;

		// Walks the live entity tree rooted at `root` and bakes a fresh
		// template, replacing any prior entry for `prefabGuid`. The
		// `fixupsAddedDuringDeserialize` argument is the queue delta
		// observed by the caller across DeserializeEntityTree —
		// non-zero means the prefab has internal entity refs and the
		// template is marked unbakeable for v1. Idempotent on a
		// concurrent re-capture from a different thread (last writer
		// wins; the previous template's bytes are discarded).
		void CaptureFromLive(uint64_t prefabGuid, Scene& scene,
			EntityHandle root,
			std::size_t fixupsAddedDuringDeserialize);

		// Hydrate `prefabGuid` into `scene` from the cached template.
		// Returns entt::null when the template doesn't exist or is
		// marked unbakeable so the caller can fall back. The cache
		// allocates its own bulk-create slot, wraps the work in a
		// Scene::LoadGuard, and issues the end-of-batch dirty pulse.
		EntityHandle Hydrate(uint64_t prefabGuid, Scene& scene);

		// Hydrate into a caller-provided handle span. Used by the ECB
		// playback path so a batch that spans many prefab spawns can
		// do ONE bulk-allocate up front (root slots + every prefab's
		// children) and route each prefab's template into its assigned
		// slice. `preAllocated.size()` must equal the template's
		// EntityCount(); preAllocated[0] is the root, the remainder
		// are the prefab's children in template DFS order. The caller
		// owns LoadGuard / dirty-pulse policy — HydrateInto does NOT
		// bracket either, since the ECB playback already does.
		EntityHandle HydrateInto(uint64_t prefabGuid, Scene& scene,
			std::span<const EntityHandle> preAllocated);

		void Invalidate(uint64_t prefabGuid);
		void InvalidateAll();

		// Diagnostics: how many templates currently live in the cache.
		// Used by the upcoming editor-side "prefab spawn perf" view.
		std::size_t Size() const;

#if defined(INDEX_WITH_EDITOR)
		// Wires a FileWatcher rooted at `assetsRoot` matching `*.prefab`
		// and pointing every hit at InvalidateAll(). Safe to call
		// multiple times — subsequent calls replace the previous watcher.
		// Editor only; shipped builds keep templates forever.
		void InitializeForProject(const std::string& assetsRoot);
		void Shutdown();
#endif

	private:
		PrefabTemplateCache() = default;
		~PrefabTemplateCache();

		PrefabTemplateCache(const PrefabTemplateCache&) = delete;
		PrefabTemplateCache& operator=(const PrefabTemplateCache&) = delete;

		mutable std::shared_mutex m_Mutex;
		std::unordered_map<uint64_t, std::unique_ptr<PrefabTemplate>> m_Templates;

#if defined(INDEX_WITH_EDITOR)
		std::unique_ptr<FileWatcher> m_Watcher;
#endif
	};

} // namespace Index
