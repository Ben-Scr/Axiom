#pragma once
#include "Scene/Entity.hpp"
#include "Scene/ISystem.hpp"
#include "Collections/Ids.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Core/Export.hpp"
#include "Core/UUID.hpp"
#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Index {
	class SceneDefinition;
	class Camera2DComponent;
	class Renderer2D;
	class Transform2DComponent;
	class TransformHierarchySystem;
	class INDEX_API Scene {
		friend class SceneManager;
		friend class SceneDefinition;
		friend class Application;
		friend class Renderer2D;
		friend class Entity;
		friend class Transform2DComponent;
		friend class TransformHierarchySystem;

	public:
		Scene(const Scene&) = delete;

		// ─── Detached-scene surface ──────────────────────────────────────
		// A detached scene has no SceneDefinition, is not registered with
		// SceneManager, will not tick, and component-construction hooks
		// that touch global subsystems (physics worlds, audio, scripts)
		// are gated off — see IsDetached().
		//
		// The editor uses these for prefab/inspector preview machinery,
		// but the engine itself has no opinion about *who* uses detached
		// scenes — the flag means "this scene is isolated from world
		// hooks", not "this scene runs inside the editor".
		static std::unique_ptr<Scene> CreateDetachedScene(const std::string& name);
		bool IsDetached() const { return m_IsDetached; }
		// ─── End detached-scene surface ──────────────────────────────────

		// Info: Creates an entity with a Transform2D component
		Entity CreateEntity();
		// Info: Creates an entity with a Transform2D and Name component
		Entity CreateEntity(const std::string& name);
		Entity CreateEmptyEntity();
		Entity CreateRuntimeEntity();
		Entity CreateRuntimeEntity(const std::string& name);
		Entity CreatePrefabInstance(AssetGUID prefabGuid, const std::string& name = "Entity");
		// Info: Creates an entity handle and applies scene invariants such as UUID + dirty tracking
		EntityHandle CreateEntityHandle(
			EntityOrigin origin = EntityOrigin::Scene,
			AssetGUID prefabGuid = AssetGUID(0),
			AssetGUID sceneGuid = AssetGUID(0),
			EntityID runtimeId = 0);

		Entity GetEntity(EntityHandle nativeEntity) {
			// Assert loud on stale/invalid handles instead of silently returning an
			// Entity wrapper that will surprise the caller later (component lookups
			// against an invalid handle become undefined inside EnTT).
			IDX_CORE_ASSERT(IsValid(nativeEntity), IndexErrorCode::InvalidValue,
				"Scene::GetEntity called with an invalid/destroyed EntityHandle");
			return Entity(nativeEntity, *this);
		}
		// CAVEAT: returns an `Entity` that can mutate this Scene even though the
		// caller holds a `const Scene&`. The const_cast is intentional to keep the
		// API symmetrical, but it is a known footgun: a caller treating a const
		// Scene reference as immutable will silently corrupt state if they mutate
		// through the returned Entity. Prefer `GetComponent<T>(handle) const` on
		// the Scene directly when you actually need read-only access. New code
		// should treat this overload as deprecated.
		[[deprecated("Returns a writable Entity from a const Scene; prefer Scene::GetComponent<T>(handle) const for read-only access")]]
		Entity GetEntity(EntityHandle nativeEntity) const {
			IDX_CORE_ASSERT(IsValid(nativeEntity), IndexErrorCode::InvalidValue,
				"Scene::GetEntity called with an invalid/destroyed EntityHandle");
			return Entity(nativeEntity, const_cast<Scene&>(*this));
		}

		void DestroyEntity(Entity entity);
		void DestroyEntity(EntityHandle nativeEntity);
		void ClearEntities();

		// ─── Bulk load helpers ─────────────────────────────────────────
		// Pre-size every EnTT storage that will be touched by a bulk-load
		// (scene deserialize, prefab instantiation batch, ECB playback) so
		// that the per-entity emplace path doesn't trigger N sparse-set
		// page reallocations. Safe to call before the entities exist —
		// reserve() just bumps the underlying vector's capacity.
		//
		// The template form reserves the entity pool plus a typed storage
		// for each TComponent. A runtime-typed overload taking a span of
		// type_index will land alongside the v2 binary loader (PR 3) which
		// discovers types from the on-disk component table rather than
		// knowing them statically.
		template <typename... TComponent>
		void ReserveForLoad(std::size_t entityCount) {
			// Reserve the entity pool itself; this is what EnTT's range-create
			// reads from when handing out fresh handles.
			m_Registry.storage<EntityHandle>().reserve(entityCount);
			// Reserve each requested component storage. The fold expression
			// expands to one call per type; folding over a no-op '0' silences
			// the unused-value warning on empty packs.
			((m_Registry.storage<TComponent>().reserve(entityCount)), ..., (void)0);
			// Identity tables grow alongside entities in the loader, so pre-
			// reserve them too. Default load factor is 1.0; reserving the
			// final count plus headroom keeps rehashes off the hot path
			// for the entire load.
			m_RuntimeIdToEntity.reserve(entityCount);
			m_SceneGuidToEntity.reserve(entityCount);
		}

		// Allocate `n` fresh entity handles in one call. Writes the handles
		// into `out` (which must have size() >= n). Uses EnTT's range-
		// create overload so the entity pool grows once instead of N times,
		// and skips the per-entity `m_Dirty`/`m_UIDirty` bookkeeping that
		// the single-shot CreateEntityHandle path performs — the caller is
		// expected to set those flags once at the end of the batch.
		//
		// IMPORTANT: this does NOT populate identity components
		// (EntityMetaDataComponent, UUIDComponent, PrefabInstanceComponent).
		// The caller must walk the returned handles and call
		// SetEntityMetaData on each, or emplace the components directly in
		// a tight loop. The bulk path exists precisely to let the loader
		// emplace components in column form rather than going through the
		// generic per-entity setup.
		void CreateEntitiesBulk(std::size_t n, std::span<EntityHandle> out);

		// Runtime-typed sibling of ReserveForLoad<TComponent...>. Accepts a
		// list of (componentTypeIdU32, count) pairs so the bulk loader can
		// pre-reserve every per-type EnTT storage it's about to touch
		// without knowing the types at compile time — matters for
		// EntityCommandBuffer playback, where the command stream is
		// type-erased and the type set isn't known until playback time.
		// Reserves the entity pool and identity maps for `entityCount` up
		// front (same as the templated overload).
		void ReserveForLoadRuntime(std::size_t entityCount,
			std::span<const std::pair<uint32_t, std::size_t>> perTypeCounts);

		// Identity-component variant of SetEntityMetaData that does NOT
		// pulse m_Dirty / m_UIDirty. Used by EntityCommandBuffer playback,
		// which pulses them exactly once at the end of the batch via
		// MarkAllDirtyOnce(). Avoids N redundant flag-flips during a big
		// runtime spawn.
		void SetEntityMetaDataNoFlags(
			EntityHandle entity,
			EntityOrigin origin,
			AssetGUID prefabGuid = AssetGUID(0),
			AssetGUID sceneGuid = AssetGUID(0),
			EntityID runtimeId = 0);

		// Equivalent of two MarkDirty() calls without re-running any
		// per-entity logic. Intended as the single flag-pulse at the tail
		// of a bulk-load / ECB playback so the editor (m_Dirty) and UI
		// rebuild (m_UIDirty) both schedule their refreshes once.
		void MarkAllDirtyOnce();

		// RAII guard that defers a SUBSET of on_construct hooks for the
		// lifetime of the guard. Bulk loaders (scene deserialize, prefab
		// instantiation, ECB playback) construct the guard, emplace
		// components freely, and let the destructor re-invoke each
		// deferred hook in emplace order once at the end — collapsing
		// per-entity dirty-mark thrash and per-emplace render-data version
		// bumps into a single sweep.
		//
		// What defers vs. what fires immediately:
		//   Defers:    Transform2D, SpriteRenderer, StaticTag,
		//              ParticleSystem2D. These are idempotent (only touch
		//              own component fields and global dirty/version state)
		//              and produce identical results whether fired in-line
		//              with emplace or in a post-batch sweep.
		//   Immediate: Rigidbody2D, BoxCollider2D, CircleCollider2D,
		//              PolygonCollider2D, Camera2D, DisabledTag, all Fast*
		//              variants. These read sibling components or cascade
		//              into the hierarchy / camera cache, so deferring
		//              them while preserving emplace order would change
		//              observable behavior (e.g. a deferred Rigidbody2D
		//              hook would see a Collider component that exists
		//              but whose hook hasn't run yet, adopting a still-
		//              zero body handle). A future PR can topologically
		//              order the flush to safely defer the physics hooks.
		//
		// Nested guards collapse: only the outermost flips the flag and
		// flushes on destruction. Inner guards are no-ops, which makes it
		// safe to compose (e.g. ECB playback that internally calls a
		// prefab instantiation).
		//
		// Guard ONLY affects on_construct. on_destroy hooks always run —
		// nothing in the engine ever wants to bulk-destroy without proper
		// teardown (physics body cleanup, rigidbody mirror clearing, etc).
		class INDEX_API LoadGuard {
		public:
			explicit LoadGuard(Scene& scene);
			~LoadGuard();
			LoadGuard(const LoadGuard&) = delete;
			LoadGuard& operator=(const LoadGuard&) = delete;
			LoadGuard(LoadGuard&&) = delete;
			LoadGuard& operator=(LoadGuard&&) = delete;
		private:
			Scene& m_Scene;
			bool m_OwnsSuppression;
		};

		bool IsConstructHooksSuppressed() const { return m_SuppressConstructHooks; }

		bool IsValid(EntityHandle nativeEntity) const {
			return m_Registry.valid(nativeEntity);
		}
		template<typename TComponent, typename... Args>
			requires (!std::is_empty_v<TComponent>)
		TComponent& AddComponent(EntityHandle entity, Args&&... args) {
			return ComponentUtils::AddComponent<TComponent>(m_Registry, entity, std::forward<Args>(args)...);
		}

		template<typename TTag>
			requires std::is_empty_v<TTag>
		void AddComponent(EntityHandle entity) {
			ComponentUtils::AddComponent<TTag>(m_Registry, entity);
		}

		template<typename TComponent>
		bool HasComponent(EntityHandle entity) const {
			return ComponentUtils::HasComponent<TComponent>(m_Registry, entity);
		}

		template<typename... TComponent>
		bool HasAnyComponent(EntityHandle entity) const {
			return ComponentUtils::HasAnyComponent<TComponent...>(m_Registry, entity);
		}

		template<typename... TComponent, typename... TEntity>
		bool AnyHasComponent(TEntity... handles) const {
			return ComponentUtils::AnyHasComponent<TComponent...>(m_Registry, handles...);
		}

		template<typename TComponent>
		TComponent& GetComponent(EntityHandle entity) {
			return ComponentUtils::GetComponent<TComponent>(m_Registry, entity);
		}

		template<typename TComponent>
		const TComponent& GetComponent(EntityHandle entity) const {
			return ComponentUtils::GetComponent<TComponent>(m_Registry, entity);
		}

		template<typename TComponent>
		bool TryGetComponent(EntityHandle entity, TComponent*& out) {
			out = ComponentUtils::TryGetComponent<TComponent>(m_Registry, entity);
			return out != nullptr;
		}

		template<typename TComponent>
		void RemoveComponent(EntityHandle entity) {
			ComponentUtils::RemoveComponent<TComponent>(m_Registry, entity);
		}

		template<typename TComponent>
		TComponent* GetSingletonComponent() {
			auto view = m_Registry.view<TComponent>();
			const auto count = view.size();
			if (count == 0) {
				IDX_CORE_ERROR_TAG("Scene", "Singleton component '{}' not found", typeid(TComponent).name());
				return nullptr;
			}
			if (count > 1) {
				IDX_CORE_WARN_TAG("Scene", "Multiple ({}) instances of singleton component '{}', returning first", count, typeid(TComponent).name());
			}

			auto entity = *view.begin();
			return &view.get<TComponent>(entity);
		}

		template<typename TComponent>
		entt::entity GetSingletonEntity() {
			auto view = m_Registry.view<TComponent>();

			const auto count = view.size();
			if (count == 0) {
				IDX_CORE_ERROR_TAG("Scene", "Singleton entity with component '{}' not found", typeid(TComponent).name());
				return entt::null;
			}
			// In non-shipping builds, multiple "singletons" indicate a setup bug:
			// either the user added the same component to two entities, or a
			// system that should have de-duplicated didn't. Failing fast in dev
			// catches it; the fall-through warn-and-return-first preserves behavior
			// in shipping where we don't want to crash on a recoverable mistake.
			// IDX_VERIFY (not IDX_ASSERT): a duplicate-singleton state is a caller
			// bug we want surfaced at runtime, but we don't want to crash a shipping
			// build over it. The fall-through warn-and-return-first below preserves
			// observable behavior; the verify just signals louder in dev builds.
			IDX_VERIFY(count == 1, IndexErrorCode::InvalidValue,
				"Multiple instances of singleton component '" + std::string(typeid(TComponent).name()) +
				"' (count=" + std::to_string(count) + "). Caller expects exactly one.");
			if (count > 1) {
				IDX_CORE_WARN_TAG("Scene", "Multiple ({}) instances of singleton component '{}', returning first", count, typeid(TComponent).name());
			}

			return *view.begin();
		}

		template<typename T>
		T* GetSystem() {
			static_assert(std::is_base_of<ISystem, T>::value, "T must derive from ISystem");

			for (auto& sysPtr : m_Systems) {
				if (auto ptr = dynamic_cast<T*>(sysPtr.get())) {
					return ptr;
				}
			}

			// Demoted from ERROR to TRACE: GetSystem<T>() is a soft probe and was
			// spamming the log when callers legitimately treat a missing system as
			// "not present, skip this work". Callers that REQUIRE the system to be
			// present should IDX_CORE_ASSERT on the result themselves.
			IDX_CORE_TRACE_TAG("Scene", "System not found: {}", typeid(T).name());
			return nullptr;
		}

		template<typename T>
		bool HasSystem() const {
			static_assert(std::is_base_of<ISystem, T>::value, "T must derive from ISystem");

			for (const auto& sysPtr : m_Systems) {
				if (dynamic_cast<T*>(sysPtr.get())) {
					return true;
				}
			}
			return false;
		}

		template<typename T>
		void DisableSystem() {
			static_assert(std::is_base_of<ISystem, T>::value, "T must derive from ISystem");

			for (auto& sysPtr : m_Systems) {
				if (auto ptr = dynamic_cast<T*>(sysPtr.get())) {
					if (ptr->IsEnabled()) {
						ptr->SetEnabled(false, *this);
					}
				}
			}
		}

		template<typename T>
		void EnableSystem() {
			static_assert(std::is_base_of<ISystem, T>::value, "T must derive from ISystem");

			for (auto& sysPtr : m_Systems) {
				if (auto ptr = dynamic_cast<T*>(sysPtr.get())) {
					ptr->SetEnabled(true, *this);
				}
			}
		}

		entt::registry& GetRegistry() { return m_Registry; }
		const entt::registry& GetRegistry() const { return m_Registry; }

		// Cached count of live entities. Maintained incrementally in the
		// Create/Destroy paths so callers (the profiler, the editor's scene
		// stats panel) don't have to walk entt's entity storage every frame
		// to compute it. Single-frame freshness is preserved as long as
		// every entity birth/death routes through CreateEntityHandle,
		// CreateEntitiesBulk or DestroyEntityInternal — those are the only
		// public spawn/destroy paths in the engine.
		std::size_t GetEntityCount() const { return m_EntityCount; }

		const std::string& GetName() const { return m_Name; }
		void SetName(const std::string& name) { m_Name = name; }
		bool IsLoaded() const { return m_IsLoaded; }
		bool IsPersistent() const { return m_Persistent; }
		const SceneDefinition* GetDefinition() const { return m_Definition; }
		Camera2DComponent* GetMainCamera();
		const Camera2DComponent* GetMainCamera() const;
		// Returns the EntityHandle backing the main camera (or entt::null when
		// none exists). Calls GetMainCamera() first so the cached
		// `m_MainCameraEntity` is refreshed against the current registry state.
		EntityHandle GetMainCameraEntity();
		bool SetMainCamera(EntityHandle entity);

		bool IsDirty() const { return m_Dirty; }
		void MarkDirty();
		void ClearDirty() { m_Dirty = false; }

		// UI rebuild signal: editor mutations (inspector edits, entity
		// create/destroy/reparent) bump this so UIEventSystem::OnPreRender
		// can refresh derived widget visuals (slider fill/handle, toggle
		// checkmark, dropdown label, input-field text) once per change
		// instead of every frame. Default true so the first frame after
		// load runs an initial rebuild. Cleared by the system after it
		// processes the rebuild. MarkDirty() also flags it, so any code
		// path that already calls MarkDirty automatically gets a UI
		// refresh — no extra wiring at every edit site.
		bool IsUIDirty() const { return m_UIDirty; }
		void MarkUIDirty() { m_UIDirty = true; }
		void ClearUIDirty() { m_UIDirty = false; }

		void MarkTransformDirty(EntityHandle entity);
		std::vector<EntityHandle> ConsumeDirtyTransformEntities();
		bool HasDirtyTransforms() const { return m_TransformHierarchyDirty; }
		void MarkStaticRenderDataDirty();
		uint64_t GetStaticRenderDataVersion() const { return m_StaticRenderDataVersion; }

		const std::vector<std::string>& GetGameSystemClassNames() const { return m_GameSystemClassNames; }
		bool HasGameSystem(const std::string& className) const;
		bool AddGameSystem(const std::string& className);
		bool RemoveGameSystem(size_t index);
		bool MoveGameSystem(size_t fromIndex, size_t toIndex);
		void ClearGameSystems();
		bool IsGameSystemEnabled(const std::string& className) const;
		bool SetGameSystemEnabled(const std::string& className, bool enabled);
		void StartManagedGameSystemsForPlayMode();

		// ── GameSystem inspector fields ────────────────────────────────────
		// Returns the live ScriptEngine handle for the named GameSystem, or 0
		// when the scene hasn't entered Awake/Start yet. The editor uses this
		// to decide whether to read fields from a live managed instance
		// (via ScriptEngine::GetGameSystemFields) or fall back to class
		// defaults (GetClassFieldDefs) patched with stored overrides.
		uint32_t GetGameSystemHandle(const std::string& className) const;
		// Pending field-value overrides keyed by GameSystem class name and
		// field name. Persisted to the scene file and re-applied to the
		// managed instance on creation, mirroring ScriptComponent.PendingFieldValues
		// but at scene scope instead of per-entity.
		void SetGameSystemFieldValue(const std::string& className, const std::string& fieldName, const std::string& value);
		const std::unordered_map<std::string, std::string>* GetGameSystemFieldValues(const std::string& className) const;
		const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& GetAllGameSystemFieldOverrides() const { return m_GameSystemFieldOverrides; }
		void ClearGameSystemFieldOverrides(const std::string& className);

		void SetEntityMetaData(
			EntityHandle entity,
			EntityOrigin origin,
			AssetGUID prefabGuid = AssetGUID(0),
			AssetGUID sceneGuid = AssetGUID(0),
			EntityID runtimeId = 0);
		EntityMetaData* GetEntityMetaData(EntityHandle entity);
		const EntityMetaData* GetEntityMetaData(EntityHandle entity) const;
		EntityOrigin GetEntityOrigin(EntityHandle entity) const;
		EntityID GetRuntimeID(EntityHandle entity) const;
		AssetGUID GetSceneEntityGUID(EntityHandle entity) const;
		AssetGUID GetPrefabGUID(EntityHandle entity) const;
		bool TryResolveRuntimeID(EntityID runtimeId, EntityHandle& outHandle) const;
		bool TryResolveSceneGUID(AssetGUID sceneGuid, EntityHandle& outHandle) const;

		// Persistent identifier for an entity. For Scene-origin entities this is
		// the SceneGUID (saved to disk); for Prefab/Runtime origin it equals the
		// current RuntimeID (volatile across scene reload, but stable within the
		// session). Returned 0 when the entity has no UUIDComponent.
		// The picker, drag-source, and any code that persists an entity reference
		// MUST use this — RuntimeID alone is reallocated on reload and produces
		// "(Missing)" references.
		uint64_t GetEntityPersistentID(EntityHandle entity) const;

		// Tries to resolve an entity reference produced by GetEntityPersistentID
		// (or by the legacy RuntimeID-based pickers). Checks RuntimeID first
		// (cheap O(1)), then falls back to a UUIDComponent scan. Mirrors the
		// runtime resolver in ScriptBindings::TryResolveEntityByUUID so the
		// editor display and the script runtime agree on what's "missing".
		bool TryResolveEntityRef(uint64_t entityId, EntityHandle& outHandle) const;

		// Defer a closure that resolves a persistent entity reference
		// to its runtime EntityHandle. Used by component deserialize
		// lambdas when the referenced entity hasn't been created yet
		// (forward reference within the same scene file). The closure
		// is run after every entity in the load batch has its
		// UUIDComponent in place — RunPendingEntityRefFixups drains
		// the queue and clears it. Capturing pointers into component
		// fields is safe for the duration of one load: components
		// don't move once their entity exists, and SceneSerializer
		// drains the queue before returning.
		void DeferEntityRefFixup(std::function<void()> fixup);
		void RunPendingEntityRefFixups();

		UUID GetSceneId() const { return m_SceneId; }
		void SetSceneId(UUID id) { m_SceneId = id; }

	private:
		Scene(const std::string& name, const SceneDefinition* definition, bool IsPersistent);

		void OnTransform2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnTransform2DComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnSpriteRendererComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnSpriteRendererComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnRigidBody2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnRigidBody2DComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnBoxCollider2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnBoxCollider2DComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnCircleCollider2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnCircleCollider2DComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnPolygonCollider2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnPolygonCollider2DComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnAudioSourceComponentDestroy(entt::registry& registry, EntityHandle entity);
		void OnScriptComponentDestroy(entt::registry& registry, EntityHandle entity);

		void OnCamera2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnCamera2DComponentDestruct(entt::registry& registry, EntityHandle entity);
		void OnDisabledTagConstruct(entt::registry& registry, EntityHandle entity);
		void OnDisabledTagDestroy(entt::registry& registry, EntityHandle entity);
		void OnStaticTagConstruct(entt::registry& registry, EntityHandle entity);
		void OnStaticTagDestroy(entt::registry& registry, EntityHandle entity);

		void OnParticleSystem2DComponentConstruct(entt::registry& registry, EntityHandle entity);
		void OnParticleSystem2DComponentDestruct(entt::registry& registry, EntityHandle entity);

		void OnFastBody2DConstruct(entt::registry& registry, EntityHandle entity);
		void OnFastBody2DDestroy(entt::registry& registry, EntityHandle entity);
		void OnFastBoxCollider2DConstruct(entt::registry& registry, EntityHandle entity);
		void OnFastBoxCollider2DDestroy(entt::registry& registry, EntityHandle entity);
		void OnFastCircleCollider2DConstruct(entt::registry& registry, EntityHandle entity);
		void OnFastCircleCollider2DDestroy(entt::registry& registry, EntityHandle entity);
		void DestroyEntityInternal(EntityHandle nativeEntity, bool markDirty);
		EntityID AllocateRuntimeEntityID();
		void RegisterEntityIdentity(EntityHandle entity);
		void UnregisterEntityIdentity(EntityHandle entity);
		void TrackEntityDestruction(EntityHandle entity);
		void UntrackEntityDestruction(EntityHandle entity);
		bool IsEntityBeingDestroyed(EntityHandle entity) const;
		void ApplyEntityEnabledState(entt::registry& registry, EntityHandle entity, bool enabled);
		void RefreshMainCameraSelection(entt::registry& registry, EntityHandle preferred = entt::null, EntityHandle excluded = entt::null);

		void RunLifecycleSystems(const std::function<void(ISystem&)>& func, size_t* enteredSystemCount = nullptr);
		void AwakeSystems(size_t* enteredSystemCount = nullptr);
		void StartSystems(size_t* enteredSystemCount = nullptr);
		void UpdateSystems();
		void FixedUpdateSystems();
		void OnPreRenderSystems();
		void DestroyScene();
		void DestroyScene(size_t enabledSystemCount);
		void ForeachEnabledSystem(std::string_view phase, const std::function<void(ISystem&)>& func);

		entt::registry m_Registry;
		std::vector<std::unique_ptr<ISystem>> m_Systems;
		std::vector<std::string> m_GameSystemClassNames;
		// className -> (fieldName -> string-encoded value). Mirrors
		// ScriptComponent::PendingFieldValues but lives on the Scene because
		// GameSystems are scene-scoped, not entity-scoped. Applied to the
		// managed instance whenever ManagedGameSystem creates one.
		std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_GameSystemFieldOverrides;
		// Snapshot of systems whose Awake() returned successfully, in awakening
		// order. DestroyScene(enabledSystemCount) walks this list in reverse so
		// rollback only tears down systems that actually entered the lifecycle —
		// even if a system disabled itself or another system between Awake and
		// the failure point, we still call OnDestroy on the right set. Cleared
		// when DestroyScene runs.
		std::vector<ISystem*> m_AwakenedSystems;


		std::string m_Name;
		const SceneDefinition* m_Definition;
		UUID m_SceneId;
		// Cached main camera. Invalidated by RefreshMainCameraSelection when
		// a Camera2DComponent is destroyed or disabled, and re-pointed when
		// a new camera entity is selected. Callers MUST NOT dereference this
		// directly — go through GetMainCamera() / GetMainCameraEntity() so
		// the cache is validated against the current registry first.
		//
		// Cache-invalidation contract:
		//   - Read access: ONLY through GetMainCamera() / GetMainCameraEntity().
		//     Both call sites validate the cached handle against the live
		//     registry and re-resolve via RefreshMainCameraSelection if stale.
		//   - Write access: any code that destroys, disables, enables, or
		//     replaces a Camera2DComponent MUST call RefreshMainCameraSelection
		//     (or rely on the on_destroy / DisabledTag hooks that already do).
		//   - Teardown windows: while m_TearingDown is true (ClearEntities loop)
		//     the cache is intentionally cleared and refresh is suppressed —
		//     readers during this window see entt::null and must defend
		//     accordingly. Same applies to detached/preview scenes which never
		//     register cameras.
		//
		// `mutable`: GetMainCamera() lazily refreshes the cache when the
		// previously-cached entity is gone or disabled. The const overload
		// shares this code path; without `mutable` it would need to
		// const_cast away constness — UB if any caller ever holds an
		// actually-const Scene&.
		mutable EntityHandle m_MainCameraEntity = entt::null;

		bool m_IsLoaded = false;
		bool m_Persistent = false;
		bool m_Dirty = false;

		// Live-entity counter. Incremented in CreateEntityHandle and the
		// bulk-create path; decremented in DestroyEntityInternal. Exposed
		// via GetEntityCount() so the profiler reads a scalar instead of
		// scanning entt's entity storage every frame.
		std::size_t m_EntityCount = 0;
		// Set true while ClearEntities() is destroying every entity at once.
		// Each Camera2DComponent destruction would otherwise re-enter
		// RefreshMainCameraSelection, which scans the still-mutating registry
		// and can pick a doomed entity (or repeatedly null/refresh). Hooks
		// observe this flag and skip work that's about to be invalidated.
		bool m_TearingDown = false;
		bool m_TransformHierarchyDirty = true;
		std::vector<EntityHandle> m_DirtyTransformEntities;
		std::unordered_set<uint32_t> m_DirtyTransformEntitySet;
		uint64_t m_StaticRenderDataVersion = 1;

		// Closures pushed by DeferEntityRefFixup, drained by
		// RunPendingEntityRefFixups. Lives on the scene so that the
		// load batch (multiple DeserializeEntity calls) shares one
		// queue and the final drain happens at the end of the load
		// — same single-pass model the parentUuid resolver uses.
		std::vector<std::function<void()>> m_PendingEntityRefFixups;
		// Default true so the first OnPreRender after scene load runs an
		// initial visual rebuild (matches play-mode where Update runs the
		// same logic on the first tick). Cleared by UIEventSystem.
		bool m_UIDirty = true;
		bool m_IsDetached = false;
		EntityID m_NextRuntimeEntityID = 1;
		std::unordered_map<EntityID, EntityHandle> m_RuntimeIdToEntity;
		std::unordered_map<uint64_t, EntityHandle> m_SceneGuidToEntity;
		std::unordered_set<uint32_t> m_EntitiesBeingDestroyed;

		// LoadGuard-controlled flag. When true, the per-component
		// on_construct handlers in this Scene short-circuit and record
		// the entity into m_DeferredConstructHooks. The outermost
		// LoadGuard's destructor clears the flag and re-invokes each
		// recorded hook in order — at which point the same member
		// function runs its normal body, since the flag is now false.
		bool m_SuppressConstructHooks = false;

		// Recorded (memberFunction, entity) pairs for hooks that fired
		// while m_SuppressConstructHooks was true. Flushed in registration
		// order by ~LoadGuard. Cleared after the flush completes. The
		// member-pointer sentinel pattern keeps each record at 16 bytes
		// (member-fn ptr + EntityHandle) with no per-record heap
		// allocations and no std::function overhead — matters because a
		// 10 000-entity prefab spawn produces ~30 000 deferred records.
		struct DeferredConstructHook {
			void (Scene::*fn)(entt::registry&, EntityHandle);
			EntityHandle entity;
		};
		std::vector<DeferredConstructHook> m_DeferredConstructHooks;
		// Re-entrance latch: keep the deferral path off while the outermost
		// LoadGuard is replaying records, so a hook can't recursively
		// re-enqueue itself if a downstream construct fires synchronously
		// from inside another hook's body.
		bool m_FlushingDeferredHooks = false;
	};
}
