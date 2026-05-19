#include "pch.hpp"
#include "Serialization/PrefabTemplateCache.hpp"

#include "Core/Log.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/Entity.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Core/Application.hpp"

#if defined(INDEX_EDITOR)
#include "Serialization/FileWatcher.hpp"
#endif

#include <typeindex>
#include <utility>

namespace Index {

	namespace {

		// Stable depth-first walk of the live entity tree, capturing the
		// {handle, parentIndex} pairs that anchor every other lookup in
		// the bake path. Recursion depth is bounded by the prefab's
		// hierarchy depth — the same bound the existing DeserializeEntity
		// path already accepts via parent-link recursion, so no extra
		// stack-safety policy applies here.
		void WalkTreeDFS(Scene& scene, EntityHandle root,
			std::vector<std::pair<EntityHandle, uint32_t>>& outOrdered)
		{
			auto recurse = [&](auto& self, EntityHandle handle, uint32_t parentIdx) -> void {
				const uint32_t myIdx = static_cast<uint32_t>(outOrdered.size());
				outOrdered.emplace_back(handle, parentIdx);
				Entity entity = scene.GetEntity(handle);
				for (EntityHandle child : entity.GetChildren()) {
					self(self, child, myIdx);
				}
			};
			recurse(recurse, root, UINT32_MAX);
		}

		// Snapshot the entity's name out of NameComponent — present on
		// every entity created by either the slow path or the bulk path
		// because both go through the same SetEntityMetaData / name-default
		// path. Defensive empty fallback for the (currently unreachable)
		// case where a hand-rolled entity skipped NameComponent.
		std::string CaptureEntityName(Scene& scene, EntityHandle handle) {
			entt::registry& registry = scene.GetRegistry();
			if (const NameComponent* nc = registry.try_get<NameComponent>(handle)) {
				return nc->Name;
			}
			return {};
		}

		uint64_t CapturePersistentId(Scene& scene, EntityHandle handle) {
			return scene.GetEntityPersistentID(handle);
		}

	} // namespace

	PrefabTemplateCache& PrefabTemplateCache::Get()
	{
		// Function-local static: zero-initialized at thread-safe first use,
		// destroyed in reverse construction order at exit. Matches the
		// singleton pattern used by SceneManager / AssetRegistry siblings.
		static PrefabTemplateCache instance;
		return instance;
	}

	PrefabTemplateCache::~PrefabTemplateCache() {
#if defined(INDEX_EDITOR)
		// FileWatcher destructor joins its worker thread; release it
		// explicitly so the dependency order is obvious even if static
		// destruction runs in an unexpected order.
		m_Watcher.reset();
#endif
	}

	const PrefabTemplate* PrefabTemplateCache::Find(uint64_t prefabGuid) const
	{
		if (prefabGuid == 0) return nullptr;
		std::shared_lock lock(m_Mutex);
		auto it = m_Templates.find(prefabGuid);
		return (it != m_Templates.end()) ? it->second.get() : nullptr;
	}

	void PrefabTemplateCache::CaptureFromLive(uint64_t prefabGuid, Scene& scene,
		EntityHandle root, std::size_t fixupsAddedDuringDeserialize)
	{
		if (prefabGuid == 0 || root == entt::null) return;

		auto tmpl = std::make_unique<PrefabTemplate>();

		// Pre-flight: any fixup added by DeserializeEntityTree means an
		// internal entity reference fired its deferred-resolve, which v1
		// has no template-local representation for. Cache the bare
		// "unbakeable" marker so subsequent spawns short-circuit straight
		// back to the slow path without rewalking the tree.
		if (fixupsAddedDuringDeserialize > 0) {
			tmpl->bakeable = false;
			tmpl->unbakeableReason = "prefab contains internal entity references; "
				"baking requires template-local fixup capture (v2 follow-up)";
			std::unique_lock lock(m_Mutex);
			m_Templates[prefabGuid] = std::move(tmpl);
			return;
		}

		// 1) Depth-first walk: anchor parent indices and per-slot handles.
		std::vector<std::pair<EntityHandle, uint32_t>> ordered;
		WalkTreeDFS(scene, root, ordered);
		const uint32_t entityCount = static_cast<uint32_t>(ordered.size());
		tmpl->entities.resize(entityCount);

		// 2) Per-entity capture: name, source id, component byte slices.
		const ComponentRegistry& componentRegistry =
			SceneManager::Get().GetComponentRegistry();
		const entt::registry& registry = scene.GetRegistry();
		const std::type_index nameComponentTypeIndex = typeid(NameComponent);
		const std::type_index metaComponentTypeIndex = typeid(EntityMetaDataComponent);

		for (uint32_t i = 0; i < entityCount; ++i) {
			if (!tmpl->bakeable) break;  // see post-loop reset; no point walking further

			EntityHandle h = ordered[i].first;
			PrefabTemplate::EntitySlot& slot = tmpl->entities[i];
			slot.parentIndex = ordered[i].second;
			slot.name = CaptureEntityName(scene, h);
			slot.sourceUuid = CapturePersistentId(scene, h);
			slot.componentRecordBegin = static_cast<uint32_t>(tmpl->components.size());

			// Walk every registered component on the entity. NameComponent
			// (captured as a std::string in the slot above) and
			// EntityMetaDataComponent (re-stamped per-hydrate with a fresh
			// RuntimeID) are excluded — the cache owns their values
			// directly so the byte path stays memcpy-safe and the metadata
			// stays consistent across spawns.
			componentRegistry.ForEachComponentInfo(
				[&](const std::type_index& typeId, const ComponentInfo& info) {
					if (!tmpl->bakeable) return;
					if (typeId == nameComponentTypeIndex) return;
					if (typeId == metaComponentTypeIndex) return;
					if (info.has == nullptr || !info.has(scene.GetEntity(h))) return;

					if (info.writeBytes == nullptr || info.typeIdU32 == 0) {
						tmpl->bakeable = false;
						tmpl->unbakeableReason = "component '" +
							(info.serializedName.empty() ? info.displayName : info.serializedName) +
							"' has no writeBytes callback or stable type id; cannot bake "
							"(component is not trivially-destructible — register a custom "
							"writeBytes/emplaceFromBytes that handles its heap-owned state, "
							"or accept the slow-path prefab spawn for this prefab)";
						return;
					}

					const uint32_t byteOffset =
						static_cast<uint32_t>(tmpl->payloadBlob.size());
					if (!info.writeBytes(registry, h, tmpl->payloadBlob)) {
						// has() returned true but writeBytes returned false:
						// indicates a registry / EnTT-storage drift. Skip
						// silently rather than crash the spawn loop; the
						// missing component will simply be absent at hydrate.
						return;
					}
					const uint32_t byteSize =
						static_cast<uint32_t>(tmpl->payloadBlob.size() - byteOffset);
					tmpl->components.push_back(
						PrefabTemplate::ComponentRecord{ info.typeIdU32, byteOffset, byteSize });
				});

			slot.componentRecordEnd = static_cast<uint32_t>(tmpl->components.size());
		}

		if (!tmpl->bakeable) {
			// Drop the partial bake — Hydrate gates on `bakeable` so these
			// bytes would never be replayed, and they pin the heap for the
			// lifetime of the cache entry. Reset to the minimal "unbakeable
			// marker" form the contract describes.
			tmpl->entities.clear();
			tmpl->components.clear();
			tmpl->payloadBlob.clear();
			IDX_CORE_WARN_TAG("PrefabTemplateCache",
				"Marking prefab {} as unbakeable: {}", prefabGuid, tmpl->unbakeableReason);
		}

		std::unique_lock lock(m_Mutex);
		m_Templates[prefabGuid] = std::move(tmpl);
	}

	EntityHandle PrefabTemplateCache::Hydrate(uint64_t prefabGuid, Scene& scene)
	{
		// Shared lookup; we hold the read lock only across the pointer
		// snapshot, not the entire hydrate. Invalidate calls a unique
		// lock that won't conflict because nothing here mutates the map.
		const PrefabTemplate* tmpl = nullptr;
		{
			std::shared_lock lock(m_Mutex);
			auto it = m_Templates.find(prefabGuid);
			if (it == m_Templates.end()) return entt::null;
			tmpl = it->second.get();
		}
		if (tmpl == nullptr || !tmpl->bakeable) return entt::null;

		const uint32_t entityCount = tmpl->EntityCount();
		if (entityCount == 0) return entt::null;

		// Self-contained variant: bulk-allocate, wrap in LoadGuard, then
		// delegate the per-component work to HydrateInto.
		scene.ReserveForLoadRuntime(entityCount, {});
		std::vector<EntityHandle> handles(entityCount);
		scene.CreateEntitiesBulk(entityCount, std::span<EntityHandle>(handles));

		Scene::LoadGuard guard(scene);
		EntityHandle root = HydrateInto(prefabGuid, scene,
			std::span<const EntityHandle>(handles));
		scene.MarkAllDirtyOnce();
		return root;
	}

	EntityHandle PrefabTemplateCache::HydrateInto(uint64_t prefabGuid, Scene& scene,
		std::span<const EntityHandle> preAllocated)
	{
		const PrefabTemplate* tmpl = nullptr;
		{
			std::shared_lock lock(m_Mutex);
			auto it = m_Templates.find(prefabGuid);
			if (it == m_Templates.end()) return entt::null;
			tmpl = it->second.get();
		}
		if (tmpl == nullptr || !tmpl->bakeable) return entt::null;

		const uint32_t entityCount = tmpl->EntityCount();
		if (entityCount == 0) return entt::null;
		if (preAllocated.size() != entityCount) {
			IDX_CORE_WARN_TAG("PrefabTemplateCache",
				"HydrateInto called with {} slots but template has {}; refusing to hydrate prefab {}",
				preAllocated.size(), entityCount, prefabGuid);
			return entt::null;
		}

		entt::registry& registry = scene.GetRegistry();
		const ComponentRegistry& componentRegistry =
			SceneManager::Get().GetComponentRegistry();

		// Identity stamping + name. Origin::Prefab + the prefab's GUID so
		// downstream queries (Entity.IsPrefabInstance, etc.) return the
		// same shape they would on the slow path.
		const AssetGUID prefabAssetGuid{ prefabGuid };
		for (uint32_t i = 0; i < entityCount; ++i) {
			scene.SetEntityMetaDataNoFlags(preAllocated[i], EntityOrigin::Prefab, prefabAssetGuid);
			registry.emplace_or_replace<NameComponent>(preAllocated[i], tmpl->entities[i].name);
		}

		// Component replay. One emplaceFromBytes per recorded slice — the
		// registry-side lambda does the appropriate memcpy +
		// emplace_or_replace which fires the component's on_construct
		// hook so scene-bound state (e.g. ParticleSystem2DComponent's
		// emitter rebind) is reinitialized correctly even though the
		// captured bytes carried the source entity's pointers.
		for (uint32_t i = 0; i < entityCount; ++i) {
			const PrefabTemplate::EntitySlot& slot = tmpl->entities[i];
			for (uint32_t r = slot.componentRecordBegin; r < slot.componentRecordEnd; ++r) {
				const PrefabTemplate::ComponentRecord& rec = tmpl->components[r];
				const ComponentInfo* info = componentRegistry.GetByTypeId(rec.typeIdU32);
				if (info == nullptr || info->emplaceFromBytes == nullptr) {
					IDX_CORE_WARN_TAG("PrefabTemplateCache",
						"Skipping component record typeId={} during hydrate of prefab {}: "
						"no emplaceFromBytes registered",
						rec.typeIdU32, prefabGuid);
					continue;
				}
				info->emplaceFromBytes(registry, preAllocated[i],
					tmpl->payloadBlob.data() + rec.byteOffset, rec.byteSize);
			}
		}

		// Parent linkage in a second pass — every entity must exist
		// before SetParent runs so cycle guards and child-list updates
		// can resolve consistently.
		for (uint32_t i = 0; i < entityCount; ++i) {
			const uint32_t parentIdx = tmpl->entities[i].parentIndex;
			if (parentIdx == UINT32_MAX) continue;
			Entity child  = scene.GetEntity(preAllocated[i]);
			Entity parent = scene.GetEntity(preAllocated[parentIdx]);
			child.SetParent(parent);
		}

		return preAllocated[0];
	}

	void PrefabTemplateCache::Invalidate(uint64_t prefabGuid)
	{
		std::unique_lock lock(m_Mutex);
		m_Templates.erase(prefabGuid);
	}

	void PrefabTemplateCache::InvalidateAll()
	{
		std::unique_lock lock(m_Mutex);
		m_Templates.clear();
	}

	std::size_t PrefabTemplateCache::Size() const
	{
		std::shared_lock lock(m_Mutex);
		return m_Templates.size();
	}

#if defined(INDEX_EDITOR)
	void PrefabTemplateCache::InitializeForProject(const std::string& assetsRoot)
	{
		if (assetsRoot.empty()) {
			IDX_CORE_WARN_TAG("PrefabTemplateCache",
				"InitializeForProject called with empty assetsRoot; cache will not auto-invalidate on prefab edits");
			return;
		}

		// Replace any prior watcher so re-opening a project doesn't pile
		// up watchers on stale roots. The destructor in unique_ptr's
		// reset() joins the previous worker thread cleanly.
		m_Watcher = std::make_unique<FileWatcher>();
		m_Watcher->Watch(
			assetsRoot,
			".prefab",
			[]() {
				// Coarse invalidation: drop the entire map on any
				// .prefab edit. A finer-grained "invalidate just the
				// changed GUID" would require the watcher to surface
				// the path it tripped on, which the current callback
				// API doesn't carry. The cost is negligible — the
				// next spawn of any prefab will repopulate its slot
				// via the existing slow path.
				PrefabTemplateCache::Get().InvalidateAll();
			},
			/*recursive=*/true);
	}

	void PrefabTemplateCache::Shutdown()
	{
		m_Watcher.reset();
		InvalidateAll();
	}
#endif

} // namespace Index
