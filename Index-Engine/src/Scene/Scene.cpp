#include "pch.hpp"
#include "Scene/Scene.hpp"
#include "Scene/Entity.hpp"

#include "Components/Audio/AudioSourceComponent.hpp"

#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Components/Physics/BoxCollider2DComponent.hpp"
#include "Components/Physics/CircleCollider2DComponent.hpp"
#include "Components/Physics/PolygonCollider2DComponent.hpp"
#include "Components/Physics/FastBody2DComponent.hpp"
#include "Components/Physics/FastBoxCollider2DComponent.hpp"
#include "Components/Physics/FastCircleCollider2DComponent.hpp"
#include "Physics/PhysicsSystem2D.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Graphics/StaticRenderData.hpp"
#include <Components/Tags.hpp>

#include "Physics/Box2DWorld.hpp"

#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/General/UUIDComponent.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/PrefabInstanceComponent.hpp"
#include "Core/Application.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Profiling/Profiler.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/ComponentInfo.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scripting/ScriptSystem.hpp"
#include "Serialization/SceneSerializer.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <typeinfo>
#include <unordered_set>
#include <utility>

namespace Index {
	namespace {
		class ManagedGameSystem final : public ISystem {
		public:
			explicit ManagedGameSystem(std::string className)
				: m_ClassName(std::move(className)) {}

			const std::string& GetClassName() const { return m_ClassName; }
			uint32_t GetHandle() const { return m_Handle; }
			bool HasEnteredLifecycle() const { return m_Handle != 0 || m_AwakeInvoked || m_StartInvoked; }

			void Start(Scene& scene) override
			{
				if (!EnsureInstance(scene, true)) return;
				if (!IsEnabled()) return;
				InvokeEnableIfNeeded();
				if (!IsEnabled()) return;
				InvokeStartIfNeeded();
			}

			void Update(Scene&) override
			{
				if (!ShouldRunInCurrentMode()) {
					return;
				}
				if (m_Handle != 0) {
					INDEX_PROFILE_SCOPE("ManagedGameSystem");
					ScriptEngine::InvokeGameSystemUpdate(m_Handle);
				}
			}

			// Lazy-creates managed instance and guards against double-fire across reload.
			void Awake(Scene& scene) override
			{
				if (!EnsureInstance(scene, false)) return;
				InvokeAwakeIfNeeded();
			}

			void FixedUpdate(Scene&) override
			{
				if (!ShouldRunInCurrentMode()) {
					return;
				}
				if (m_Handle != 0) {
					ScriptEngine::InvokeGameSystemFixedUpdate(m_Handle);
				}
			}

			void OnEnable(Scene& scene) override
			{
				// If a GameSystem starts disabled, Awake/Start are skipped by the
				// scene lifecycle. Enabling it later from code should still bring
				// the managed instance fully online before the next Update.
				if (!EnsureInstance(scene, true)) return;
				InvokeAwakeIfNeeded();
				if (!IsEnabled()) return;
				InvokeEnableIfNeeded();
				if (!IsEnabled()) return;
				InvokeStartIfNeeded();
			}

			void OnDisable(Scene&) override
			{
				if (m_Handle != 0 && m_EnableInvoked) {
					m_EnableInvoked = false;
					ScriptEngine::InvokeGameSystemDisable(m_Handle);
				}
			}

			void OnDestroy(Scene&) override
			{
				if (m_Handle == 0) {
					return;
				}

				if (m_EnableInvoked) {
					ScriptEngine::InvokeGameSystemDisable(m_Handle);
					m_EnableInvoked = false;
				}
				ScriptEngine::InvokeGameSystemDestroy(m_Handle);
				ScriptEngine::DestroyGameSystemInstance(m_Handle);
				m_Handle = 0;
				m_StartInvoked = false;
				m_AwakeInvoked = false;
			}

		private:
			bool ShouldRunInCurrentMode() const
			{
				return !Application::IsEditor() || Application::GetIsPlaying();
			}

			bool EnsureInstance(Scene& scene, bool warnIfMissing)
			{
				if (!ShouldRunInCurrentMode()) {
					return false;
				}

				if (m_Handle != 0) {
					return true;
				}

				if (!ScriptEngine::GameSystemClassExists(m_ClassName)) {
					if (warnIfMissing) {
						IDX_CORE_WARN_TAG("Scene", "GameSystem '{}' was registered on scene '{}' but no matching class was found", m_ClassName, scene.GetName());
					}
					return false;
				}

				m_Handle = ScriptEngine::CreateGameSystemInstance(m_ClassName, scene.GetName());
				ApplyFieldOverrides(scene);
				return m_Handle != 0;
			}

			void InvokeAwakeIfNeeded()
			{
				if (m_Handle != 0 && !m_AwakeInvoked) {
					ScriptEngine::InvokeGameSystemAwake(m_Handle);
					m_AwakeInvoked = true;
				}
			}

			void InvokeEnableIfNeeded()
			{
				if (m_Handle != 0 && !m_EnableInvoked) {
					m_EnableInvoked = true;
					ScriptEngine::InvokeGameSystemEnable(m_Handle);
				}
			}

			void InvokeStartIfNeeded()
			{
				if (m_Handle != 0 && !m_StartInvoked) {
					ScriptEngine::InvokeGameSystemStart(m_Handle);
					m_StartInvoked = true;
				}
			}

			// Push the scene's stored editor-time field values onto the
			// freshly-created managed instance. Mirrors how ScriptComponent
			// flushes PendingFieldValues for EntityScripts after their
			// CreateScriptInstance call: the instance starts with class
			// defaults, then we overwrite any field the user authored in
			// the inspector before Awake/Start runs.
			void ApplyFieldOverrides(Scene& scene)
			{
				if (m_Handle == 0) return;
				const auto* overrides = scene.GetGameSystemFieldValues(m_ClassName);
				if (!overrides) return;

				for (const auto& [fieldName, value] : *overrides) {
					ScriptEngine::SetGameSystemField(m_Handle, fieldName.c_str(), value.c_str());
				}
			}

			std::string m_ClassName;
			uint32_t m_Handle = 0;
			bool m_EnableInvoked = false;
			bool m_AwakeInvoked = false; // H7
			bool m_StartInvoked = false;
		};

		bool IsManagedGameSystem(const std::unique_ptr<ISystem>& system)
		{
			return dynamic_cast<ManagedGameSystem*>(system.get()) != nullptr;
		}
	}

	Entity Scene::CreateEntity() {
		auto entityHandle = CreateEntityHandle(EntityOrigin::Scene);
		AddComponent<Transform2DComponent>(entityHandle);
		return Entity(entityHandle, *this);
	}
	Entity Scene::CreateEntity(const std::string& name) {
		auto entityHandle = CreateEntityHandle(EntityOrigin::Scene);
		AddComponent<Transform2DComponent>(entityHandle);
		if (!name.empty()) {
			AddComponent<NameComponent>(entityHandle, name);
		}
		return Entity(entityHandle, *this);
	}
	Entity Scene::CreateEmptyEntity() {
		auto entityHandle = CreateEntityHandle(EntityOrigin::Scene);
		return Entity(entityHandle, *this);
	}

	Entity Scene::CreateRuntimeEntity() {
		auto entityHandle = CreateEntityHandle(EntityOrigin::Runtime);
		AddComponent<Transform2DComponent>(entityHandle);
		return Entity(entityHandle, *this);
	}

	Entity Scene::CreateRuntimeEntity(const std::string& name) {
		auto entityHandle = CreateEntityHandle(EntityOrigin::Runtime);
		AddComponent<Transform2DComponent>(entityHandle);
		if (!name.empty()) {
			AddComponent<NameComponent>(entityHandle, name);
		}
		return Entity(entityHandle, *this);
	}

	Entity Scene::CreatePrefabInstance(AssetGUID prefabGuid, const std::string& name) {
		// Instantiate through the serializer so prefab component data, identity, and overrides follow one path.
		const EntityHandle entityHandle = SceneSerializer::InstantiatePrefab(*this, static_cast<uint64_t>(prefabGuid));
		if (entityHandle == entt::null) {
			IDX_CORE_WARN_TAG("Scene", "Failed to instantiate prefab '{}'", static_cast<uint64_t>(prefabGuid));
			return Entity::Null;
		}

		if (!name.empty()) {
			if (HasComponent<NameComponent>(entityHandle)) {
				GetComponent<NameComponent>(entityHandle).Name = name;
			}
			else {
				AddComponent<NameComponent>(entityHandle, name);
			}
		}
		return Entity(entityHandle, *this);
	}

	Scene::LoadGuard::LoadGuard(Scene& scene)
		: m_Scene(scene)
		// Nested-guard collapse: only the outermost guard flips the flag and
		// is responsible for clearing it on destruction. Inner guards
		// observe the flag is already true and become no-ops — this makes
		// it safe for a bulk-loader (e.g. ECB playback) to call into
		// another bulk-loader (e.g. prefab instantiation) without worrying
		// about who owns the suppression scope.
		, m_OwnsSuppression(!scene.m_SuppressConstructHooks)
	{
		if (m_OwnsSuppression) {
			m_Scene.m_SuppressConstructHooks = true;
		}
	}

	Scene::LoadGuard::~LoadGuard() {
		if (!m_OwnsSuppression) {
			return;
		}
		// Clear the flag FIRST so the recorded hooks run their normal
		// body when we re-invoke them below (each hook short-circuits
		// when m_SuppressConstructHooks is true).
		m_Scene.m_SuppressConstructHooks = false;

		if (m_Scene.m_DeferredConstructHooks.empty()) {
			return;
		}

		// Move-out so a hook body that itself triggers a new construct
		// (during the flush) is appended to a fresh vector instead of
		// silently mutating the one we're iterating. The flushing latch
		// also keeps any such nested constructs on the "run immediately"
		// path even if downstream code spins up a new LoadGuard
		// transiently.
		auto hooks = std::move(m_Scene.m_DeferredConstructHooks);
		m_Scene.m_FlushingDeferredHooks = true;
		for (auto& h : hooks) {
			(m_Scene.*h.fn)(m_Scene.m_Registry, h.entity);
		}
		m_Scene.m_FlushingDeferredHooks = false;
	}

	void Scene::ReserveForLoadRuntime(std::size_t entityCount,
		std::span<const std::pair<uint32_t, std::size_t>> /*perTypeCounts*/)
	{
		// Entity pool + identity maps — the dominant reservation savings.
		// EnTT grows the entity pool by powers of two and rehashes the
		// underlying vector on every threshold, so reserving up front
		// collapses ~log2(N) pool reallocations into one.
		m_Registry.storage<EntityHandle>().reserve(entityCount);
		m_RuntimeIdToEntity.reserve(m_RuntimeIdToEntity.size() + entityCount);
		m_SceneGuidToEntity.reserve(m_SceneGuidToEntity.size() + entityCount);
		// NOTE: per-component-storage reservation is intentionally omitted
		// here. EnTT exposes typed storages via its compile-time type_hash
		// table, but our stable u32 component ID is registry-private and
		// doesn't map cleanly back to entt::id_type. The component storages
		// still grow on demand during emplace — O(log N) reallocations
		// versus the entity pool's single growth, which is negligible
		// compared to the eliminated P/Invoke and string-marshal overhead.
		// The templated Scene::ReserveForLoad<T...> overload remains the
		// statically-typed fast path for the scene loader.
	}

	void Scene::SetEntityMetaDataNoFlags(
		EntityHandle entity,
		EntityOrigin origin,
		AssetGUID prefabGuid,
		AssetGUID sceneGuid,
		EntityID runtimeId)
	{
		// SetEntityMetaData only pulses m_Dirty for non-Runtime origin
		// while NOT playing — i.e. the editor case. For ECB playback at
		// runtime (the dominant case), origin == Runtime and IsPlaying()
		// is true, so the regular function never touches the flags.
		// The "NoFlags" suffix is a contract guarantee for callers — we
		// snapshot the flags around the inner call and restore them, so
		// the function is a no-op on m_Dirty / m_UIDirty regardless of
		// origin or play state. End-of-batch MarkAllDirtyOnce() is the
		// authoritative pulse.
		const bool savedDirty = m_Dirty;
		const bool savedUIDirty = m_UIDirty;
		SetEntityMetaData(entity, origin, prefabGuid, sceneGuid, runtimeId);
		m_Dirty = savedDirty;
		m_UIDirty = savedUIDirty;
	}

	void Scene::MarkAllDirtyOnce()
	{
		// Single end-of-batch pulse for editor save-state and UI rebuild.
		// Mirrors the two-flag pattern used by CreateEntityHandle, but
		// without the per-entity edit-mode guard — the bulk caller (ECB
		// playback, scene deserialize) has already decided to pulse once
		// for the whole batch.
		m_Dirty = true;
		m_UIDirty = true;
	}

	void Scene::CreateEntitiesBulk(std::size_t n, std::span<EntityHandle> out) {
		// Caller-allocates so we don't smuggle a heap allocation into the
		// loader's hot path — the loader typically reuses one buffer across
		// the entire load. Short-circuit the zero case so EnTT's
		// range-create isn't asked to operate on an empty iterator pair.
		if (n == 0) {
			return;
		}
		IDX_CORE_ASSERT(out.size() >= n, IndexErrorCode::InvalidValue,
			"Scene::CreateEntitiesBulk: output span is smaller than the requested count");
		// Refuse a batch that would push the live-entity count past EnTT's
		// configured entity_mask. Beyond the cap, registry.create() returns
		// a wrapped entity index and the next sparse-set write reaches
		// garbage memory (Windows surfaces it as 0xC0000005 mid-batch). The
		// compare is O(1) per call so it's free even at six-figure batches.
		// The cap is set by the project-level entityBits field — see
		// External/entt/src/entt/entity/entity.hpp for the bit-split table.
		using EntityTraits = entt::entt_traits<EntityHandle>;
		constexpr std::size_t kMaxLiveEntities = static_cast<std::size_t>(EntityTraits::entity_mask);
		IDX_CORE_ASSERT(m_EntityCount + n <= kMaxLiveEntities, IndexErrorCode::InvalidValue,
			"Scene::CreateEntitiesBulk: batch would exceed the EnTT entity cap. "
			"Raise 'entityBits' in index-project.json and rebuild.");
		// EnTT's range-create writes one entity per iterator slot. Reserving
		// up front bounds the entity pool's growth to a single allocation
		// even when this is the first batch on a fresh scene.
		m_Registry.storage<EntityHandle>().reserve(
			m_Registry.storage<EntityHandle>().size() + n);
		m_Registry.create(out.begin(), out.begin() + n);
		m_EntityCount += n;
	}

	EntityHandle Scene::CreateEntityHandle(EntityOrigin origin, AssetGUID prefabGuid, AssetGUID sceneGuid, EntityID runtimeId) {
		EntityHandle entityHandle = m_Registry.create();
		++m_EntityCount;
		SetEntityMetaData(entityHandle, origin, prefabGuid, sceneGuid, runtimeId);
		if (origin != EntityOrigin::Runtime && !Application::GetIsPlaying()) {
			m_Dirty = true;
		}
		// Pulse the UI rebuild gate so freshly-created widgets get their
		// derived visuals (slider fill stretch, scrollbar handle size,
		// dropdown label text, etc.) refreshed on the next OnPreRender pass.
		// Without this, a slider created from EntityHelper::CreateUISlider
		// or via the editor's "Create UI > Slider" menu has its fill
		// invisible until the user drags the value off its initial setting,
		// because UIEventSystem's preview path early-outs on !IsUIDirty().
		// Runtime-spawned widgets (game code instantiating a Slider prefab
		// while playing) need the same pulse — m_Dirty's edit-mode guard
		// would otherwise skip the runtime case entirely.
		m_UIDirty = true;
		return entityHandle;
	}

	EntityID Scene::AllocateRuntimeEntityID() {
		while (m_NextRuntimeEntityID == 0 || m_RuntimeIdToEntity.contains(m_NextRuntimeEntityID)) {
			++m_NextRuntimeEntityID;
		}

		return m_NextRuntimeEntityID++;
	}

	void Scene::RegisterEntityIdentity(EntityHandle entity) {
		if (!m_Registry.valid(entity) || !m_Registry.all_of<EntityMetaDataComponent>(entity)) {
			return;
		}

		const EntityMetaData& metaData = m_Registry.get<EntityMetaDataComponent>(entity).MetaData;
		if (metaData.RuntimeID != 0) {
			m_RuntimeIdToEntity[metaData.RuntimeID] = entity;
		}
		if (metaData.Origin == EntityOrigin::Scene && static_cast<uint64_t>(metaData.SceneGUID) != 0) {
			m_SceneGuidToEntity[static_cast<uint64_t>(metaData.SceneGUID)] = entity;
		}
	}

	void Scene::UnregisterEntityIdentity(EntityHandle entity) {
		if (!m_Registry.valid(entity) || !m_Registry.all_of<EntityMetaDataComponent>(entity)) {
			return;
		}

		const EntityMetaData& metaData = m_Registry.get<EntityMetaDataComponent>(entity).MetaData;
		if (metaData.RuntimeID != 0) {
			auto runtimeIt = m_RuntimeIdToEntity.find(metaData.RuntimeID);
			if (runtimeIt != m_RuntimeIdToEntity.end() && runtimeIt->second == entity) {
				m_RuntimeIdToEntity.erase(runtimeIt);
			}
		}
		if (static_cast<uint64_t>(metaData.SceneGUID) != 0) {
			auto sceneIt = m_SceneGuidToEntity.find(static_cast<uint64_t>(metaData.SceneGUID));
			if (sceneIt != m_SceneGuidToEntity.end() && sceneIt->second == entity) {
				m_SceneGuidToEntity.erase(sceneIt);
			}
		}
	}

	void Scene::SetEntityMetaData(
		EntityHandle entity,
		EntityOrigin origin,
		AssetGUID prefabGuid,
		AssetGUID sceneGuid,
		EntityID runtimeId) {
		if (entity == entt::null || !m_Registry.valid(entity)) {
			return;
		}

		UnregisterEntityIdentity(entity);

		EntityMetaData metaData;
		if (m_Registry.all_of<EntityMetaDataComponent>(entity)) {
			metaData = m_Registry.get<EntityMetaDataComponent>(entity).MetaData;
		}

		metaData.RuntimeID = runtimeId != 0 ? runtimeId : (metaData.RuntimeID != 0 ? metaData.RuntimeID : AllocateRuntimeEntityID());
		metaData.Origin = origin;
		metaData.PrefabGUID = origin == EntityOrigin::Prefab ? prefabGuid : AssetGUID(0);
		metaData.SceneGUID = origin == EntityOrigin::Scene
			? (static_cast<uint64_t>(sceneGuid) != 0 ? sceneGuid : AssetGUID(UUID()))
			: AssetGUID(0);

		m_Registry.emplace_or_replace<EntityMetaDataComponent>(entity, EntityMetaDataComponent{ metaData });

		UUID legacyId = UUID(metaData.RuntimeID);
		if (metaData.Origin == EntityOrigin::Scene && static_cast<uint64_t>(metaData.SceneGUID) != 0) {
			legacyId = metaData.SceneGUID;
		}
		m_Registry.emplace_or_replace<UUIDComponent>(entity, legacyId);

		if (metaData.Origin == EntityOrigin::Prefab && static_cast<uint64_t>(metaData.PrefabGUID) != 0) {
			m_Registry.emplace_or_replace<PrefabInstanceComponent>(entity, PrefabInstanceComponent{ metaData.PrefabGUID });
		}
		else if (m_Registry.all_of<PrefabInstanceComponent>(entity)) {
			m_Registry.remove<PrefabInstanceComponent>(entity);
		}

		RegisterEntityIdentity(entity);
		if (origin != EntityOrigin::Runtime && !Application::GetIsPlaying()) {
			m_Dirty = true;
		}
	}

	EntityMetaData* Scene::GetEntityMetaData(EntityHandle entity) {
		if (entity == entt::null || !m_Registry.valid(entity) || !m_Registry.all_of<EntityMetaDataComponent>(entity)) {
			return nullptr;
		}

		return &m_Registry.get<EntityMetaDataComponent>(entity).MetaData;
	}

	const EntityMetaData* Scene::GetEntityMetaData(EntityHandle entity) const {
		if (entity == entt::null || !m_Registry.valid(entity) || !m_Registry.all_of<EntityMetaDataComponent>(entity)) {
			return nullptr;
		}

		return &m_Registry.get<EntityMetaDataComponent>(entity).MetaData;
	}

	EntityOrigin Scene::GetEntityOrigin(EntityHandle entity) const {
		const EntityMetaData* metaData = GetEntityMetaData(entity);
		return metaData ? metaData->Origin : EntityOrigin::Runtime;
	}

	EntityID Scene::GetRuntimeID(EntityHandle entity) const {
		const EntityMetaData* metaData = GetEntityMetaData(entity);
		return metaData ? metaData->RuntimeID : 0;
	}

	AssetGUID Scene::GetSceneEntityGUID(EntityHandle entity) const {
		const EntityMetaData* metaData = GetEntityMetaData(entity);
		return metaData ? metaData->SceneGUID : AssetGUID(0);
	}

	AssetGUID Scene::GetPrefabGUID(EntityHandle entity) const {
		const EntityMetaData* metaData = GetEntityMetaData(entity);
		return metaData ? metaData->PrefabGUID : AssetGUID(0);
	}

	bool Scene::TryResolveRuntimeID(EntityID runtimeId, EntityHandle& outHandle) const {
		outHandle = entt::null;
		if (runtimeId == 0) {
			return false;
		}

		const auto it = m_RuntimeIdToEntity.find(runtimeId);
		if (it == m_RuntimeIdToEntity.end() || !IsValid(it->second)) {
			return false;
		}

		outHandle = it->second;
		return true;
	}

	uint64_t Scene::GetEntityPersistentID(EntityHandle entity) const {
		if (entity == entt::null || !m_Registry.valid(entity) || !m_Registry.all_of<UUIDComponent>(entity)) {
			return 0;
		}
		return static_cast<uint64_t>(m_Registry.get<UUIDComponent>(entity).Id);
	}

	bool Scene::TryResolveEntityRef(uint64_t entityId, EntityHandle& outHandle) const {
		outHandle = entt::null;
		if (entityId == 0) {
			return false;
		}

		if (TryResolveRuntimeID(entityId, outHandle)) {
			return true;
		}

		// Fall back to a UUIDComponent scan. Saved scene files persist the
		// SceneGUID (= UUIDComponent.Id for Scene-origin entities); after
		// reload that ID won't appear in m_RuntimeIdToEntity but UUIDComponent
		// preserves it. Linear scan is fine: scenes hold O(1k) entities and
		// this only runs in the editor display + reference-picker path.
		auto view = m_Registry.view<UUIDComponent>();
		for (EntityHandle handle : view) {
			if (static_cast<uint64_t>(view.get<UUIDComponent>(handle).Id) == entityId) {
				outHandle = handle;
				return true;
			}
		}
		return false;
	}

	void Scene::DeferEntityRefFixup(std::function<void()> fixup) {
		if (fixup) {
			m_PendingEntityRefFixups.emplace_back(std::move(fixup));
		}
	}

	void Scene::RunPendingEntityRefFixups() {
		// Fixups can in principle queue more fixups (a closure that
		// re-resolves and finds another forward-only ref), so swap-and-
		// drain in a loop until the queue is stable. In practice one
		// pass is enough — every fixup just looks up an existing UUID —
		// but the loop guards against future schemes that build the
		// scene incrementally.
		while (!m_PendingEntityRefFixups.empty()) {
			std::vector<std::function<void()>> batch;
			batch.swap(m_PendingEntityRefFixups);
			for (auto& fn : batch) {
				if (fn) fn();
			}
		}
	}

	bool Scene::TryResolveSceneGUID(AssetGUID sceneGuid, EntityHandle& outHandle) const {
		outHandle = entt::null;
		const uint64_t guidValue = static_cast<uint64_t>(sceneGuid);
		if (guidValue == 0) {
			return false;
		}

		const auto it = m_SceneGuidToEntity.find(guidValue);
		if (it == m_SceneGuidToEntity.end() || !IsValid(it->second)) {
			return false;
		}

		outHandle = it->second;
		return true;
	}

	void Scene::DestroyEntity(Entity entity) { DestroyEntity(entity.GetHandle()); }
	void Scene::DestroyEntity(EntityHandle nativeEntity) { DestroyEntityInternal(nativeEntity, true); }

	void Scene::DestroyEntity(EntityHandle nativeEntity, float delay) {
		// Non-positive delay collapses to immediate destruction — keeps
		// Entity::Destroy(e, 0.0f) semantically identical to
		// Entity::Destroy(e), no surprise one-frame skew when callers
		// pass a computed delay that happens to be zero.
		if (delay <= 0.0f) {
			DestroyEntityInternal(nativeEntity, true);
			return;
		}
		if (!m_Registry.valid(nativeEntity)) return;
		// Already queued? Keep the SHORTER remaining time so the first
		// scheduled destruction wins (caller wants the entity gone by
		// then; later schedules are best treated as redundant).
		for (auto& [h, remaining] : m_PendingDestroys) {
			if (h == nativeEntity) {
				if (delay < remaining) remaining = delay;
				return;
			}
		}
		m_PendingDestroys.emplace_back(nativeEntity, delay);
	}

	Entity Scene::CloneEntity(Entity source) {
		if (!source.IsValid()) return Entity::Null;
		const std::string name = source.GetName();
		Entity clone = CreateRuntimeEntity(name + " (Clone)");
		const auto& registry = SceneManager::Get().GetComponentRegistry();
		registry.ForEachComponentInfo([&](const std::type_index&, const ComponentInfo& info) {
			if (info.category != ComponentCategory::Component) return;
			if (!info.has || !info.has(source)) return;
			if (info.copyTo) {
				info.copyTo(source, clone);
			}
		});
		return clone;
	}

	void Scene::ClearEntities() {
		// Invalidate the main-camera cache up front and gate the destroy hooks
		// so each Camera2DComponent destruction doesn't re-enter
		// RefreshMainCameraSelection mid-iteration (which scans the still-
		// mutating registry).
		m_MainCameraEntity = entt::null;
		m_TearingDown = true;

		auto view = m_Registry.view<entt::entity>();
		std::vector<EntityHandle> entities;
		entities.reserve(view.size());

		for (EntityHandle entity : view) {
			entities.push_back(entity);
		}

		for (EntityHandle entity : entities) {
			if (m_Registry.valid(entity)) {
				DestroyEntityInternal(entity, false);
			}
		}

		m_TearingDown = false;
		m_TransformHierarchyDirty = true;
		m_DirtyTransformEntities.clear();
		m_DirtyTransformEntitySet.clear();
		MarkStaticRenderDataDirty();
		Renderer2D::ClearSceneCache(this);
	}

	void Scene::MarkDirty() {
		if (!Application::GetIsPlaying()) m_Dirty = true;
		// Pulse the UI rebuild signal in both modes: edit-mode edits drive
		// the OnPreRender refresh path, and runtime calls (e.g. game code
		// that toggles a slider entity in/out of the scene) still want the
		// rebuild gate to fire at least once on the next frame.
		m_UIDirty = true;
		MarkStaticRenderDataDirty();
	}

	bool Scene::HasGameSystem(const std::string& className) const
	{
		return std::find(m_GameSystemClassNames.begin(), m_GameSystemClassNames.end(), className) != m_GameSystemClassNames.end();
	}

	bool Scene::AddGameSystem(const std::string& className)
	{
		if (className.empty() || HasGameSystem(className)) {
			return false;
		}

		auto managedSystem = std::make_unique<ManagedGameSystem>(className);
		ISystem* system = managedSystem.get();
		m_GameSystemClassNames.push_back(className);
		m_Systems.push_back(std::move(managedSystem));

		if (m_IsLoaded && (!Application::IsEditor() || Application::GetIsPlaying())) {
			system->Awake(*this);
			system->Start(*this);
			if (auto* gameSystem = dynamic_cast<ManagedGameSystem*>(system);
				gameSystem && gameSystem->HasEnteredLifecycle()
				&& std::find(m_AwakenedSystems.begin(), m_AwakenedSystems.end(), system) == m_AwakenedSystems.end()) {
				m_AwakenedSystems.push_back(system);
			}
		}

		MarkDirty();
		return true;
	}

	bool Scene::RemoveGameSystem(size_t index)
	{
		if (index >= m_GameSystemClassNames.size()) {
			return false;
		}

		const std::string className = m_GameSystemClassNames[index];
		for (auto it = m_Systems.begin(); it != m_Systems.end(); ++it) {
			if (auto* managedSystem = dynamic_cast<ManagedGameSystem*>(it->get());
				managedSystem && managedSystem->GetClassName() == className) {
				if (m_IsLoaded) {
					// Symmetric teardown: disable before destroy, matching the
					// SetEnabled / OnEnable / OnDisable contract on ISystem.
					managedSystem->OnDisable(*this);
					managedSystem->OnDestroy(*this);
				}
				m_AwakenedSystems.erase(std::remove(m_AwakenedSystems.begin(), m_AwakenedSystems.end(), managedSystem), m_AwakenedSystems.end());
				m_Systems.erase(it);
				break;
			}
		}

		m_GameSystemClassNames.erase(m_GameSystemClassNames.begin() + static_cast<std::ptrdiff_t>(index));
		m_GameSystemFieldOverrides.erase(className);
		MarkDirty();
		return true;
	}

	bool Scene::MoveGameSystem(size_t fromIndex, size_t toIndex)
	{
		if (fromIndex >= m_GameSystemClassNames.size() || toIndex >= m_GameSystemClassNames.size() || fromIndex == toIndex) {
			return false;
		}

		// Reorder the names list.
		const std::string movedClassName = m_GameSystemClassNames[fromIndex];
		m_GameSystemClassNames.erase(m_GameSystemClassNames.begin() + static_cast<std::ptrdiff_t>(fromIndex));
		m_GameSystemClassNames.insert(m_GameSystemClassNames.begin() + static_cast<std::ptrdiff_t>(toIndex), movedClassName);

		// Reorder the m_Systems entries that correspond to managed systems
		// to match the new name order, WITHOUT calling OnDisable/OnDestroy
		// on the way out and OnAwake/OnEnable on the way back in. Native
		// (non-managed) systems keep their slot positions so a simple
		// reorder doesn't disturb the rest of the system list. Reordering
		// is a UX-level concern; user OnDestroy/OnAwake callbacks should
		// not see a phantom destroy+recreate just because someone dragged
		// a system in the inspector.
		std::vector<std::unique_ptr<ISystem>> managedExtracted;
		std::vector<size_t> managedSlots;
		managedExtracted.reserve(m_GameSystemClassNames.size());
		managedSlots.reserve(m_GameSystemClassNames.size());

		for (size_t i = 0; i < m_Systems.size(); ++i) {
			if (IsManagedGameSystem(m_Systems[i])) {
				managedExtracted.push_back(std::move(m_Systems[i]));
				managedSlots.push_back(i);
			}
		}

		// Build the reordered managed list following the names order. A name
		// without a matching extracted system (shouldn't happen in practice,
		// since AddGameSystem is the only path that grows the list) gets a
		// fresh ManagedGameSystem so the lists stay in lock-step (L5).
		std::vector<std::unique_ptr<ISystem>> managedReordered;
		managedReordered.reserve(m_GameSystemClassNames.size());
		for (const std::string& className : m_GameSystemClassNames) {
			bool placed = false;
			for (auto it = managedExtracted.begin(); it != managedExtracted.end(); ++it) {
				if (!*it) continue;
				auto* mg = static_cast<ManagedGameSystem*>(it->get());
				if (mg->GetClassName() == className) {
					managedReordered.push_back(std::move(*it));
					placed = true;
					break;
				}
			}
			if (!placed) {
				managedReordered.push_back(std::make_unique<ManagedGameSystem>(className));
			}
		}

		// Slot reordered managed systems back into the original managed
		// positions. Surplus extracted systems (a name was dropped) are
		// discarded — that path also calls OnDisable/OnDestroy so the
		// user observes a clean shutdown for the dropped system.
		for (size_t i = 0; i < managedSlots.size() && i < managedReordered.size(); ++i) {
			m_Systems[managedSlots[i]] = std::move(managedReordered[i]);
		}
		// If managedReordered grew beyond the original slot count (a new
		// system was injected), append the extras at the back.
		for (size_t i = managedSlots.size(); i < managedReordered.size(); ++i) {
			m_Systems.push_back(std::move(managedReordered[i]));
		}

		// Any unique_ptr left behind in managedExtracted (no matching name in the
		// reorder) is about to be destroyed when this scope exits. m_AwakenedSystems
		// stores raw ISystem* — purge any pointers that match the soon-to-be-deleted
		// systems before they dangle. (Without this, OnDestroy on an unrelated scene
		// teardown would dereference freed memory.)
		std::erase_if(m_AwakenedSystems, [&](ISystem* awakened) {
			for (const auto& leftover : managedExtracted) {
				if (leftover && leftover.get() == awakened) {
					return true;
				}
			}
			return false;
		});

		MarkDirty();
		return true;
	}

	void Scene::ClearGameSystems()
	{
		for (auto it = m_Systems.begin(); it != m_Systems.end(); ) {
			if (IsManagedGameSystem(*it)) {
				ISystem* system = it->get();
				if (m_IsLoaded) {
					system->OnDisable(*this);
					system->OnDestroy(*this);
				}
				m_AwakenedSystems.erase(std::remove(m_AwakenedSystems.begin(), m_AwakenedSystems.end(), system), m_AwakenedSystems.end());
				it = m_Systems.erase(it);
			}
			else {
				++it;
			}
		}
		m_GameSystemClassNames.clear();
		m_GameSystemFieldOverrides.clear();
	}

	uint32_t Scene::GetGameSystemHandle(const std::string& className) const
	{
		for (const auto& sysPtr : m_Systems) {
			const auto* managedSystem = dynamic_cast<const ManagedGameSystem*>(sysPtr.get());
			if (managedSystem && managedSystem->GetClassName() == className) {
				return managedSystem->GetHandle();
			}
		}
		return 0;
	}

	void Scene::SetGameSystemFieldValue(const std::string& className, const std::string& fieldName, const std::string& value)
	{
		m_GameSystemFieldOverrides[className][fieldName] = value;
	}

	const std::unordered_map<std::string, std::string>* Scene::GetGameSystemFieldValues(const std::string& className) const
	{
		auto it = m_GameSystemFieldOverrides.find(className);
		if (it == m_GameSystemFieldOverrides.end()) return nullptr;
		return &it->second;
	}

	void Scene::ClearGameSystemFieldOverrides(const std::string& className)
	{
		m_GameSystemFieldOverrides.erase(className);
	}

	bool Scene::IsGameSystemEnabled(const std::string& className) const
	{
		if (className.empty()) {
			return false;
		}

		for (const auto& system : m_Systems) {
			const auto* managedSystem = dynamic_cast<const ManagedGameSystem*>(system.get());
			if (managedSystem && managedSystem->GetClassName() == className) {
				return managedSystem->IsEnabled();
			}
		}

		return false;
	}

	bool Scene::SetGameSystemEnabled(const std::string& className, bool enabled)
	{
		if (className.empty()) {
			return false;
		}

		for (auto& system : m_Systems) {
			auto* managedSystem = dynamic_cast<ManagedGameSystem*>(system.get());
			if (!managedSystem || managedSystem->GetClassName() != className) {
				continue;
			}

			if (managedSystem->IsEnabled() == enabled) {
				return true;
			}

			managedSystem->SetEnabled(enabled, *this);
			ISystem* systemBase = managedSystem;
			if (enabled && managedSystem->HasEnteredLifecycle()
				&& std::find(m_AwakenedSystems.begin(), m_AwakenedSystems.end(), systemBase) == m_AwakenedSystems.end()) {
				m_AwakenedSystems.push_back(systemBase);
			}

			MarkDirty();
			return true;
		}

		return false;
	}

	void Scene::StartManagedGameSystemsForPlayMode()
	{
		for (const auto& systemPointer : m_Systems) {
			if (!systemPointer || !systemPointer->IsEnabled() || !IsManagedGameSystem(systemPointer)) {
				continue;
			}

			ISystem* system = systemPointer.get();
			system->Awake(*this);
			system->Start(*this);
			if (std::find(m_AwakenedSystems.begin(), m_AwakenedSystems.end(), system) == m_AwakenedSystems.end()) {
				m_AwakenedSystems.push_back(system);
			}
		}
	}

	Camera2DComponent* Scene::GetMainCamera() {
		const auto isUsableCamera = [this](EntityHandle entity) {
			return entity != entt::null
				&& m_Registry.valid(entity)
				&& m_Registry.all_of<Camera2DComponent>(entity)
				&& !m_Registry.all_of<DisabledTag>(entity);
		};

		if (isUsableCamera(m_MainCameraEntity)) {
			return &m_Registry.get<Camera2DComponent>(m_MainCameraEntity);
		}

		auto view = m_Registry.view<Camera2DComponent>(entt::exclude<DisabledTag>);
		for (EntityHandle entity : view) {
			m_MainCameraEntity = entity;
			return &view.get<Camera2DComponent>(entity);
		}

		m_MainCameraEntity = entt::null;
		return nullptr;
	}

	const Camera2DComponent* Scene::GetMainCamera() const {
		// m_MainCameraEntity is `mutable` so this const overload can do
		// the same lazy-cache update without const_casting the registry.
		const auto isUsableCamera = [this](EntityHandle entity) {
			return entity != entt::null
				&& m_Registry.valid(entity)
				&& m_Registry.all_of<Camera2DComponent>(entity)
				&& !m_Registry.all_of<DisabledTag>(entity);
		};

		if (isUsableCamera(m_MainCameraEntity)) {
			return &m_Registry.get<Camera2DComponent>(m_MainCameraEntity);
		}

		auto view = m_Registry.view<Camera2DComponent>(entt::exclude<DisabledTag>);
		for (EntityHandle entity : view) {
			m_MainCameraEntity = entity;
			return &view.get<Camera2DComponent>(entity);
		}

		m_MainCameraEntity = entt::null;
		return nullptr;
	}

	EntityHandle Scene::GetMainCameraEntity() {
		// Side effect: refreshes m_MainCameraEntity if cached camera is stale.
		(void)GetMainCamera();
		return m_MainCameraEntity;
	}

	bool Scene::SetMainCamera(EntityHandle entity) {
		if (entity == entt::null
			|| !m_Registry.valid(entity)
			|| !m_Registry.all_of<Camera2DComponent>(entity)
			|| m_Registry.all_of<DisabledTag>(entity)) {
			return false;
		}

		m_MainCameraEntity = entity;
		return true;
	}

	void Scene::DestroyEntityInternal(EntityHandle nativeEntity, bool markDirty) {
		if (nativeEntity == entt::null || !m_Registry.valid(nativeEntity)) {
			return;
		}

		if (markDirty && !Application::GetIsPlaying()) {
			m_Dirty = true;
		}
		// Symmetric with CreateEntityHandle: destroying an entity (or one
		// of its children) can change which Fill/Handle/Checkmark/Label a
		// widget auto-resolves to, or take a tinted Image out of view —
		// pulse the UI rebuild gate so the next OnPreRender refreshes the
		// remaining widgets. `markDirty=false` means we're in the middle
		// of a cascade destroy — the outermost call already pulsed the
		// flag, so the inner recursion doesn't need to.
		if (markDirty) {
			m_UIDirty = true;
		}

		// Cascade-destroy any children, and unhook from the parent's child list, before
		// destroying this entity. Snapshot Children + Parent into locals first: EnTT's default
		// pool is swap-and-pop, so a recursive child-destroy can relocate this entity's own
		// HierarchyComponent (or the parent's). Holding a HierarchyComponent& across the
		// recursion would dangle.
		if (m_Registry.all_of<HierarchyComponent>(nativeEntity)) {
			std::vector<EntityHandle> childrenSnapshot;
			EntityHandle parent = entt::null;
			{
				const HierarchyComponent& hc = m_Registry.get<HierarchyComponent>(nativeEntity);
				childrenSnapshot = hc.Children;
				parent = hc.Parent;
			}

			for (EntityHandle child : childrenSnapshot) {
				DestroyEntityInternal(child, false);
			}

			// Re-fetch the parent's HierarchyComponent after the recursive destroys; any
			// reference taken before the loop may now be moved-from.
			if (parent != entt::null
				&& m_Registry.valid(parent)
				&& m_Registry.all_of<HierarchyComponent>(parent))
			{
				auto& parentHc = m_Registry.get<HierarchyComponent>(parent);
				parentHc.Children.erase(
					std::remove(parentHc.Children.begin(), parentHc.Children.end(), nativeEntity),
					parentHc.Children.end());
			}
		}

		UnregisterEntityIdentity(nativeEntity);
		// Dynamic components live outside EnTT and don't get on_destroy
		// hooks, so without this they leak bytes until scene unload /
		// assembly reload. Cheap no-op when no dynamics are attached.
		SceneManager::Get().GetComponentRegistry().ScrubEntity(nativeEntity);
		TrackEntityDestruction(nativeEntity);
		m_Registry.destroy(nativeEntity);
		UntrackEntityDestruction(nativeEntity);
		if (m_EntityCount > 0) {
			--m_EntityCount;
		}
	}

	void Scene::TrackEntityDestruction(EntityHandle entity) {
		m_EntitiesBeingDestroyed.insert(static_cast<uint32_t>(entity));
	}

	void Scene::UntrackEntityDestruction(EntityHandle entity) {
		m_EntitiesBeingDestroyed.erase(static_cast<uint32_t>(entity));
	}

	bool Scene::IsEntityBeingDestroyed(EntityHandle entity) const {
		return m_EntitiesBeingDestroyed.contains(static_cast<uint32_t>(entity));
	}

	void Scene::RunLifecycleSystems(const std::function<void(ISystem&)>& func, size_t* enteredSystemCount) {
		for (const auto& systemPointer : m_Systems) {
			ISystem& system = *systemPointer;
			if (!system.IsEnabled()) {
				continue;
			}

			if (enteredSystemCount) {
				++(*enteredSystemCount);
			}

			func(system);
		}
	}

	void Scene::AwakeSystems(size_t* enteredSystemCount) {
		m_AwakenedSystems.clear();
		for (const auto& systemPointer : m_Systems) {
			ISystem& system = *systemPointer;
			if (!system.IsEnabled()) {
				continue;
			}

			system.Awake(*this);
			m_AwakenedSystems.push_back(&system);
			if (enteredSystemCount) {
				++(*enteredSystemCount);
			}
		}
	}

	void Scene::StartSystems(size_t* enteredSystemCount) {
		RunLifecycleSystems([this](ISystem& s) { s.Start(*this); }, enteredSystemCount);
	}

	void Scene::UpdateSystems() {
		// Drain delayed-destroy queue first so any entity whose timer
		// elapsed this frame disappears BEFORE systems iterate views over
		// it. Decrement-then-test runs the destruction on the same tick
		// the timer hits zero (i.e. delay 0.5s at 60fps fires at tick 30).
		// Swap-pop preserves O(1) removal; entries the user added during
		// system Update will be processed next frame, not this one.
		if (!m_PendingDestroys.empty()) {
			const float dt = Application::GetInstance()
				? static_cast<float>(Application::GetInstance()->GetTime().GetDeltaTime())
				: 0.0f;
			for (size_t i = 0; i < m_PendingDestroys.size();) {
				auto& [h, remaining] = m_PendingDestroys[i];
				remaining -= dt;
				if (remaining <= 0.0f) {
					if (m_Registry.valid(h)) {
						DestroyEntityInternal(h, true);
					}
					// swap-pop; do NOT increment i — re-test slot i which
					// now holds the previously-last element.
					m_PendingDestroys[i] = m_PendingDestroys.back();
					m_PendingDestroys.pop_back();
				}
				else {
					++i;
				}
			}
		}

		ForeachEnabledSystem("Update", [this](ISystem& s) { s.Update(*this); });
	}

	void Scene::FixedUpdateSystems() {
		ForeachEnabledSystem("FixedUpdate", [this](ISystem& s) { s.FixedUpdate(*this); });
	}

	void Scene::OnPreRenderSystems() {
		ForeachEnabledSystem("OnPreRender", [this](ISystem& s) { s.OnPreRender(*this); });
	}

	void Scene::ForeachEnabledSystem(std::string_view phase, const std::function<void(ISystem&)>& func) {
		for (size_t i = 0; i < m_Systems.size(); ++i) {
			ISystem& system = *m_Systems[i];
			if (!system.IsEnabled()) {
				continue;
			}

			try {
				func(system);
			}
			catch (const std::exception& e) {
				IDX_CORE_ERROR_TAG("Scene", "{} failed in '{}': {}", typeid(system).name(), phase, e.what());
				throw;
			}
			catch (...) {
				IDX_CORE_ERROR_TAG("Scene", "{} failed in '{}' with an unknown exception", typeid(system).name(), phase);
				throw;
			}
		}
	}

	void Scene::DestroyScene() {
		DestroyScene(std::numeric_limits<size_t>::max());
	}

	void Scene::DestroyScene(size_t enabledSystemCount) {
		std::vector<ISystem*> systemsToDestroy;
		systemsToDestroy.reserve(m_Systems.size());

		// Walk m_Systems as the superset and dedup against m_AwakenedSystems by raw
		// pointer. Systems added after AwakeSystems (e.g. via AddGameSystem mid-play)
		// are appended to m_Systems but not to m_AwakenedSystems, so a m_AwakenedSystems-
		// only walk would silently skip OnDestroy on them and leak their resources.
		std::unordered_set<ISystem*> alreadyQueued;
		alreadyQueued.reserve(m_AwakenedSystems.size());

		if (!m_AwakenedSystems.empty()) {
			const size_t count = std::min(enabledSystemCount, m_AwakenedSystems.size());
			for (size_t i = 0; i < count; ++i) {
				ISystem* sys = m_AwakenedSystems[i];
				systemsToDestroy.push_back(sys);
				alreadyQueued.insert(sys);
			}
		}

		// Second pass: every enabled, not-yet-queued system in m_Systems gets
		// OnDestroy. The cap (enabledSystemCount) constrains the FIRST pass —
		// it represents how many m_AwakenedSystems entries the rollback should
		// cover when a partial Awake failed. Capping the second pass too would
		// silently skip OnDestroy on systems registered AFTER AwakeSystems
		// (e.g. AddGameSystem mid-play), leaking their resources. We always
		// want to tear those down regardless of enabledSystemCount.
		for (const auto& systemPointer : m_Systems) {
			ISystem* system = systemPointer.get();
			if (alreadyQueued.contains(system)) {
				continue;
			}
			const auto* managedSystem = dynamic_cast<const ManagedGameSystem*>(system);
			if (!system->IsEnabled() && (!managedSystem || managedSystem->GetHandle() == 0)) {
				continue;
			}

			systemsToDestroy.push_back(system);
		}

		std::exception_ptr firstFailure;
		for (auto it = systemsToDestroy.rbegin(); it != systemsToDestroy.rend(); ++it) {
			try {
				(*it)->OnDestroy(*this);
			}
			catch (const std::exception& e) {
				IDX_CORE_ERROR_TAG("Scene", "System error during destroy: {}", e.what());
				if (!firstFailure) {
					firstFailure = std::current_exception();
				}
			}
			catch (...) {
				IDX_CORE_ERROR_TAG("Scene", "Unknown system error during destroy");
				if (!firstFailure) {
					firstFailure = std::current_exception();
				}
			}
		}

		m_AwakenedSystems.clear();

		if (firstFailure) {
			std::rethrow_exception(firstFailure);
		}
	}

	std::unique_ptr<Scene> Scene::CreateDetachedScene(const std::string& name) {
		auto scene = std::unique_ptr<Scene>(new Scene(name, nullptr, false));
		scene->m_IsDetached = true;
		return scene;
	}

	Scene::Scene(const std::string& name, const SceneDefinition* definition, bool IsPersistent)
		: m_Name(name)
		, m_Definition(definition)
		, m_Persistent(IsPersistent)
		, m_IsLoaded(false) {

		m_Registry.on_construct<Transform2DComponent>().connect<&Scene::OnTransform2DComponentConstruct>(this);
		m_Registry.on_destroy<Transform2DComponent>().connect<&Scene::OnTransform2DComponentDestroy>(this);
		m_Registry.on_construct<SpriteRendererComponent>().connect<&Scene::OnSpriteRendererComponentConstruct>(this);
		m_Registry.on_destroy<SpriteRendererComponent>().connect<&Scene::OnSpriteRendererComponentDestroy>(this);
		m_Registry.on_construct<Rigidbody2DComponent>().connect<&Scene::OnRigidBody2DComponentConstruct>(this);
		m_Registry.on_construct<BoxCollider2DComponent>().connect<&Scene::OnBoxCollider2DComponentConstruct>(this);
		m_Registry.on_construct<CircleCollider2DComponent>().connect<&Scene::OnCircleCollider2DComponentConstruct>(this);
		m_Registry.on_construct<PolygonCollider2DComponent>().connect<&Scene::OnPolygonCollider2DComponentConstruct>(this);
		m_Registry.on_construct<Camera2DComponent>().connect<&Scene::OnCamera2DComponentConstruct>(this);
		m_Registry.on_construct<ParticleSystem2DComponent>().connect<&Scene::OnParticleSystem2DComponentConstruct>(this);
		m_Registry.on_construct<DisabledTag>().connect<&Scene::OnDisabledTagConstruct>(this);
		m_Registry.on_construct<StaticTag>().connect<&Scene::OnStaticTagConstruct>(this);

		m_Registry.on_destroy<Rigidbody2DComponent>().connect<&Scene::OnRigidBody2DComponentDestroy>(this);
		m_Registry.on_destroy<BoxCollider2DComponent>().connect<&Scene::OnBoxCollider2DComponentDestroy>(this);
		m_Registry.on_destroy<CircleCollider2DComponent>().connect<&Scene::OnCircleCollider2DComponentDestroy>(this);
		m_Registry.on_destroy<PolygonCollider2DComponent>().connect<&Scene::OnPolygonCollider2DComponentDestroy>(this);
		m_Registry.on_destroy<AudioSourceComponent>().connect<&Scene::OnAudioSourceComponentDestroy>(this);
		m_Registry.on_destroy<ScriptComponent>().connect<&Scene::OnScriptComponentDestroy>(this);
		m_Registry.on_destroy<Camera2DComponent>().connect<&Scene::OnCamera2DComponentDestruct>(this);
		m_Registry.on_destroy<ParticleSystem2DComponent>().connect<&Scene::OnParticleSystem2DComponentDestruct>(this);
		m_Registry.on_destroy<DisabledTag>().connect<&Scene::OnDisabledTagDestroy>(this);
		m_Registry.on_destroy<StaticTag>().connect<&Scene::OnStaticTagDestroy>(this);

		// Axiom-Physics component hooks
		m_Registry.on_construct<FastBody2DComponent>().connect<&Scene::OnFastBody2DConstruct>(this);
		m_Registry.on_destroy<FastBody2DComponent>().connect<&Scene::OnFastBody2DDestroy>(this);
		m_Registry.on_construct<FastBoxCollider2DComponent>().connect<&Scene::OnFastBoxCollider2DConstruct>(this);
		m_Registry.on_destroy<FastBoxCollider2DComponent>().connect<&Scene::OnFastBoxCollider2DDestroy>(this);
		m_Registry.on_construct<FastCircleCollider2DComponent>().connect<&Scene::OnFastCircleCollider2DConstruct>(this);
		m_Registry.on_destroy<FastCircleCollider2DComponent>().connect<&Scene::OnFastCircleCollider2DDestroy>(this);
	}

	void Scene::MarkTransformDirty(EntityHandle entity)
	{
		m_TransformHierarchyDirty = true;

		if (entity == entt::null || !m_Registry.valid(entity)) {
			return;
		}

		if (m_DirtyTransformEntitySet.insert(entity).second) {
			m_DirtyTransformEntities.push_back(entity);
		}

		if (m_Registry.all_of<StaticTag>(entity)) {
			MarkStaticRenderDataDirty();
		}
	}

	std::vector<EntityHandle> Scene::ConsumeDirtyTransformEntities()
	{
		m_TransformHierarchyDirty = false;
		m_DirtyTransformEntitySet.clear();

		std::vector<EntityHandle> dirty;
		dirty.swap(m_DirtyTransformEntities);
		return dirty;
	}

	void Scene::MarkStaticRenderDataDirty()
	{
		++m_StaticRenderDataVersion;
		if (m_StaticRenderDataVersion == 0) {
			m_StaticRenderDataVersion = 1;
		}
	}

	void Scene::OnTransform2DComponentConstruct(entt::registry& registry, EntityHandle entity)
	{
		// LoadGuard suppression: idempotent hook (binds owner + flips dirty
		// flags, doesn't read siblings), so deferring it during ECB playback
		// is safe — at flush time the same body runs with no observable
		// difference. The suppression check itself is the entire deferral
		// surface for this hook; the flush loop in ~LoadGuard re-invokes
		// this same member function with the flag cleared, which then runs
		// the body below normally.
		if (m_SuppressConstructHooks) {
			m_DeferredConstructHooks.push_back({ &Scene::OnTransform2DComponentConstruct, entity });
			return;
		}
		auto& transform = registry.get<Transform2DComponent>(entity);
		transform.BindOwner(this, entity);
		transform.MarkDirty();
		MarkStaticRenderDataDirty();
	}

	void Scene::OnTransform2DComponentDestroy(entt::registry&, EntityHandle)
	{
		m_TransformHierarchyDirty = true;
		MarkStaticRenderDataDirty();
	}

	void Scene::OnSpriteRendererComponentConstruct(entt::registry&, EntityHandle entity)
	{
		// LoadGuard suppression: stateless hook (only bumps a version
		// counter), safe to defer to the end of an ECB / load batch so a
		// 1 000-entity spawn pulses the version once instead of 1 000 times.
		if (m_SuppressConstructHooks) {
			m_DeferredConstructHooks.push_back({ &Scene::OnSpriteRendererComponentConstruct, entity });
			return;
		}
		MarkStaticRenderDataDirty();
	}

	void Scene::OnSpriteRendererComponentDestroy(entt::registry&, EntityHandle)
	{
		MarkStaticRenderDataDirty();
	}

	void Scene::OnRigidBody2DComponentConstruct(entt::registry& registry, EntityHandle entity)
	{
		// Body adoption invariant: when a Rigidbody2D is added to an entity that already
		// holds a Box/Circle/Polygon collider, the rigidbody adopts the collider's body
		// and BOTH components mirror the same b2BodyId. On destroy, whichever path
		// actually calls b2DestroyBody MUST also zero the sibling component's m_BodyId.
		// If we don't, Box2D body-id recycling (long sessions / generation wrap) can
		// alias the stale id to a fresh, unrelated body — silent corruption with no log.
		//
		// Skip Box2D body creation for editor-preview scenes (e.g. prefab inspector
		// thumbnails). Without this gate, every preview entity would push a real body
		// into the singleton physics world and corrupt simulation state on the live scene.
		// Drawers must defend against rb2D.IsValid() == false rather than assume.
		if (m_IsDetached) return;

		if (!registry.all_of<Transform2DComponent>(entity)) {
			IDX_CORE_WARN_TAG("Scene", "Adding missing Transform2DComponent before creating Rigidbody2D for entity {}", static_cast<uint32_t>(entity));
			registry.emplace<Transform2DComponent>(entity);
		}

		bool isEnabled = !registry.all_of<DisabledTag>(entity);
		Rigidbody2DComponent& rb2D = registry.get<Rigidbody2DComponent>(entity);

		// All three Box2D-backed colliders (Box / Circle / Polygon) own a b2Body
		// when added before the rigidbody, so we adopt whichever one is already
		// on the entity. The mutual-exclusion conflict declarations in
		// BuiltInComponentRegistration ensure at most one is present.
		if (HasAnyComponent<BoxCollider2DComponent>(entity)) {
			rb2D.m_BodyId = GetComponent<BoxCollider2DComponent>(entity).m_BodyId;
			rb2D.SetBodyType(BodyType::Dynamic);
		}
		else if (HasAnyComponent<CircleCollider2DComponent>(entity)) {
			rb2D.m_BodyId = GetComponent<CircleCollider2DComponent>(entity).m_BodyId;
			rb2D.SetBodyType(BodyType::Dynamic);
		}
		else if (HasAnyComponent<PolygonCollider2DComponent>(entity)) {
			rb2D.m_BodyId = GetComponent<PolygonCollider2DComponent>(entity).m_BodyId;
			rb2D.SetBodyType(BodyType::Dynamic);
		}
		else {
			rb2D.m_BodyId = PhysicsSystem2D::GetMainPhysicsWorld().CreateBody(entity, *this, BodyType::Dynamic);
		}

		rb2D.SetEnabled(isEnabled);
	}

	void Scene::OnRigidBody2DComponentDestroy(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		auto& rb2D = GetComponent<Rigidbody2DComponent>(entity);

		if (!rb2D.IsValid()) return;

		// Match the BoxCollider2D path for circle/polygon: when the entity
		// itself is being destroyed, route through the collider's Destroy so
		// it tears down the shape AND the shared body in one step. Otherwise
		// the rigidbody is being removed in isolation, so demote the body to
		// static (the collider keeps using it) or fully destroy the body when
		// no collider remains.
		const bool hasAnyCollider =
			registry.all_of<BoxCollider2DComponent>(entity) ||
			registry.all_of<CircleCollider2DComponent>(entity) ||
			registry.all_of<PolygonCollider2DComponent>(entity);

		if (IsEntityBeingDestroyed(entity)) {
			// Body ownership invariant: collider's Destroy() will b2DestroyBody the
			// shared body, so we MUST zero our mirrored m_BodyId here — otherwise
			// rb2D.m_BodyId outlives the underlying body and a recycled b2BodyId
			// can later alias an unrelated entity's body.
			if (registry.all_of<BoxCollider2DComponent>(entity)) {
				registry.get<BoxCollider2DComponent>(entity).Destroy();
				rb2D.m_BodyId = b2_nullBodyId;
				return;
			}
			if (registry.all_of<CircleCollider2DComponent>(entity)) {
				registry.get<CircleCollider2DComponent>(entity).Destroy();
				rb2D.m_BodyId = b2_nullBodyId;
				return;
			}
			if (registry.all_of<PolygonCollider2DComponent>(entity)) {
				registry.get<PolygonCollider2DComponent>(entity).Destroy();
				rb2D.m_BodyId = b2_nullBodyId;
				return;
			}
		}

		if (hasAnyCollider) {
			rb2D.SetBodyType(BodyType::Static);
		}
		else {
			rb2D.Destroy();
			// Defensive: rb2D.Destroy() destroys the body. Make sure any mirror is
			// cleared even if the collider branch above didn't run.
			rb2D.m_BodyId = b2_nullBodyId;
		}
	}

	void Scene::OnBoxCollider2DComponentConstruct(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		if (!registry.all_of<Transform2DComponent>(entity)) {
			IDX_CORE_WARN_TAG("Scene", "Adding missing Transform2DComponent before creating BoxCollider2D for entity {}", static_cast<uint32_t>(entity));
			registry.emplace<Transform2DComponent>(entity);
		}

		bool isEnabled = !registry.all_of<DisabledTag>(entity);
		BoxCollider2DComponent& boxCollider = GetComponent<BoxCollider2DComponent>(entity);
		boxCollider.m_EntityHandle = entity;

		// Reuse existing rigidbody if present, else spawn a static body.
		if (HasComponent<Rigidbody2DComponent>(entity)) {
			auto& rb = GetComponent<Rigidbody2DComponent>(entity);
			boxCollider.m_BodyId = rb.GetBodyHandle();
		}
		else {
			boxCollider.m_BodyId = PhysicsSystem2D::GetMainPhysicsWorld().CreateBody(entity, *this, BodyType::Static);
		}

		boxCollider.m_ShapeId = PhysicsSystem2D::GetMainPhysicsWorld().CreateShape(entity, *this, boxCollider.m_BodyId, ShapeType::Square);
		boxCollider.SetEnabled(isEnabled);
	}

	void Scene::OnBoxCollider2DComponentDestroy(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		auto& boxCollider2D = GetComponent<BoxCollider2DComponent>(entity);
		// Body ownership invariant: if a Rigidbody2D is present, IT owns the body's
		// lifetime — the collider's Destroy must NOT call b2DestroyBody. Otherwise
		// both hooks fire b2DestroyBody on the same b2BodyId during entity teardown.
		// b2Body_IsValid catches the second call most of the time via generation
		// bumps, but if Box2D recycles the slot (long-running session, generation
		// wrap, or MaxBodyId churn), the second destroy hits an unrelated body —
		// silent corruption with no log.
		if (registry.all_of<Rigidbody2DComponent>(entity)) {
			boxCollider2D.DestroyShape(false);
		}
		else {
			boxCollider2D.Destroy();
		}
		// Body ownership invariant (mirror): if a Rigidbody2D survives the
		// collider's destroy hook (it was the body owner above, or — for the
		// belt-and-braces case below — the registry transiently holds both
		// components while teardown is in flight), make sure no stale b2BodyId
		// is left pointing at a slot Box2D may recycle. Doing this AFTER
		// boxCollider2D.Destroy()/DestroyShape() means we zero the mirror
		// regardless of which branch ran. Previously this clear lived inside
		// the `else` and tested all_of<Rigidbody2DComponent> a second time —
		// the inner predicate was always false in that branch, so the guard
		// the author documented was statically dead.
		if (registry.all_of<Rigidbody2DComponent>(entity)) {
			auto& rb = registry.get<Rigidbody2DComponent>(entity);
			if (!b2Body_IsValid(rb.m_BodyId)) {
				rb.m_BodyId = b2_nullBodyId;
			}
		}
	}

	void Scene::OnCircleCollider2DComponentConstruct(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		if (!registry.all_of<Transform2DComponent>(entity)) {
			IDX_CORE_WARN_TAG("Scene", "Adding missing Transform2DComponent before creating CircleCollider2D for entity {}", static_cast<uint32_t>(entity));
			registry.emplace<Transform2DComponent>(entity);
		}

		bool isEnabled = !registry.all_of<DisabledTag>(entity);
		CircleCollider2DComponent& circleCollider = GetComponent<CircleCollider2DComponent>(entity);
		circleCollider.m_EntityHandle = entity;

		// Reuse existing rigidbody body if present, else spawn a static body —
		// mirrors BoxCollider2D's body-sharing rules.
		if (HasComponent<Rigidbody2DComponent>(entity)) {
			auto& rb = GetComponent<Rigidbody2DComponent>(entity);
			circleCollider.m_BodyId = rb.GetBodyHandle();
		}
		else {
			circleCollider.m_BodyId = PhysicsSystem2D::GetMainPhysicsWorld().CreateBody(entity, *this, BodyType::Static);
		}

		circleCollider.m_ShapeId = PhysicsSystem2D::GetMainPhysicsWorld().CreateShape(entity, *this, circleCollider.m_BodyId, ShapeType::Circle);
		circleCollider.SetEnabled(isEnabled);
	}

	void Scene::OnCircleCollider2DComponentDestroy(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		auto& circleCollider = GetComponent<CircleCollider2DComponent>(entity);
		// See OnBoxCollider2DComponentDestroy: collider does NOT own the body when
		// a Rigidbody2D is present.
		if (registry.all_of<Rigidbody2DComponent>(entity)) {
			circleCollider.DestroyShape(false);
		}
		else {
			circleCollider.Destroy();
			// Body ownership invariant (mirror): zero a stale rigidbody mirror.
			if (registry.all_of<Rigidbody2DComponent>(entity)) {
				registry.get<Rigidbody2DComponent>(entity).m_BodyId = b2_nullBodyId;
			}
		}
	}

	void Scene::OnPolygonCollider2DComponentConstruct(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		if (!registry.all_of<Transform2DComponent>(entity)) {
			IDX_CORE_WARN_TAG("Scene", "Adding missing Transform2DComponent before creating PolygonCollider2D for entity {}", static_cast<uint32_t>(entity));
			registry.emplace<Transform2DComponent>(entity);
		}

		bool isEnabled = !registry.all_of<DisabledTag>(entity);
		PolygonCollider2DComponent& polygonCollider = GetComponent<PolygonCollider2DComponent>(entity);
		polygonCollider.m_EntityHandle = entity;

		if (HasComponent<Rigidbody2DComponent>(entity)) {
			auto& rb = GetComponent<Rigidbody2DComponent>(entity);
			polygonCollider.m_BodyId = rb.GetBodyHandle();
		}
		else {
			polygonCollider.m_BodyId = PhysicsSystem2D::GetMainPhysicsWorld().CreateBody(entity, *this, BodyType::Static);
		}

		polygonCollider.m_ShapeId = PhysicsSystem2D::GetMainPhysicsWorld().CreateShape(entity, *this, polygonCollider.m_BodyId, ShapeType::Polygon);
		polygonCollider.SetEnabled(isEnabled);
	}

	void Scene::OnPolygonCollider2DComponentDestroy(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		auto& polygonCollider = GetComponent<PolygonCollider2DComponent>(entity);
		// See OnBoxCollider2DComponentDestroy: collider does NOT own the body when
		// a Rigidbody2D is present.
		if (registry.all_of<Rigidbody2DComponent>(entity)) {
			polygonCollider.DestroyShape(false);
		}
		else {
			polygonCollider.Destroy();
			// Body ownership invariant (mirror): zero a stale rigidbody mirror.
			if (registry.all_of<Rigidbody2DComponent>(entity)) {
				registry.get<Rigidbody2DComponent>(entity).m_BodyId = b2_nullBodyId;
			}
		}
	}

	void Scene::OnAudioSourceComponentDestroy(entt::registry& registry, EntityHandle entity)
	{
		if (m_IsDetached) return;
		registry.get<AudioSourceComponent>(entity).Destroy();
	}

	void Scene::OnScriptComponentDestroy(entt::registry& registry, EntityHandle entity)
	{
		if (m_IsDetached) return; // No script lifecycle ran in this scene.
		(void)registry;
		ScriptSystem::RemoveAllScripts(Entity(entity, *this));
	}

	void Scene::OnCamera2DComponentConstruct(entt::registry& registry, EntityHandle entity)
	{
		// Match every other heavy on_construct hook in this file: detached editor
		// scenes are scratch copies (e.g. play-mode preview rollback) and must not
		// allocate FBOs / register the entity as a render camera. Doing so leaks
		// GL resources and confuses RefreshMainCameraSelection across two scenes.
		if (m_IsDetached) return;

		if (!registry.all_of<Transform2DComponent>(entity)) {
			IDX_CORE_WARN_TAG("Scene", "Adding missing Transform2DComponent before creating Camera2D for entity {}", static_cast<uint32_t>(entity));
			registry.emplace<Transform2DComponent>(entity);
		}

		Camera2DComponent& camera2D = GetComponent<Camera2DComponent>(entity);
		camera2D.Initialize(*this, entity);
		camera2D.UpdateViewport();

		if (!registry.all_of<DisabledTag>(entity)
			&& (m_MainCameraEntity == entt::null
				|| !registry.valid(m_MainCameraEntity)
				|| registry.all_of<DisabledTag>(m_MainCameraEntity))) {
			RefreshMainCameraSelection(registry, entity);
		}
	}

	void Scene::OnCamera2DComponentDestruct(entt::registry& registry, EntityHandle entity)
	{
		Camera2DComponent& camera2D = GetComponent<Camera2DComponent>(entity);
		camera2D.Destroy();

		// During ClearEntities() every entity is being destroyed; refreshing
		// the cache mid-loop would just pick another doomed entity and waste
		// a registry scan per camera. ClearEntities clears the cache up front.
		if (m_TearingDown) {
			return;
		}

		if (m_MainCameraEntity == entity) {
			RefreshMainCameraSelection(registry, entt::null, entity);
		}
	}

	void Scene::OnDisabledTagConstruct(entt::registry& registry, EntityHandle entity)
	{
		if (IsEntityBeingDestroyed(entity)) {
			return;
		}

		MarkStaticRenderDataDirty();
		ApplyEntityEnabledState(registry, entity, false);

		// Propagate to direct children — each child's own on_construct hook
		// recurses further into the subtree, so we only emplace one level deep
		// here. Snapshot the children list before iterating because emplace
		// may reorder pool storage internally.
		// InheritedDisabledTag lets the re-enable cascade remove only the
		// DisabledTag values that this parent cascade created.
		if (registry.all_of<HierarchyComponent>(entity)) {
			std::vector<EntityHandle> children = registry.get<HierarchyComponent>(entity).Children;
			for (EntityHandle child : children) {
				if (registry.valid(child) && !registry.all_of<DisabledTag>(child)) {
					registry.emplace<InheritedDisabledTag>(child);
					registry.emplace<DisabledTag>(child);
				}
			}
		}
	}

	void Scene::OnDisabledTagDestroy(entt::registry& registry, EntityHandle entity)
	{
		if (IsEntityBeingDestroyed(entity)) {
			return;
		}

		MarkStaticRenderDataDirty();
		if (registry.all_of<InheritedDisabledTag>(entity)) {
			registry.remove<InheritedDisabledTag>(entity);
		}

		ApplyEntityEnabledState(registry, entity, true);

		// Mirror the construct-time cascade: each child's own on_destroy hook
		// continues the descent.
		if (registry.all_of<HierarchyComponent>(entity)) {
			std::vector<EntityHandle> children = registry.get<HierarchyComponent>(entity).Children;
			for (EntityHandle child : children) {
				if (registry.valid(child)
					&& registry.all_of<DisabledTag>(child)
					&& registry.all_of<InheritedDisabledTag>(child)) {
					registry.remove<InheritedDisabledTag>(child);
					registry.remove<DisabledTag>(child);
				}
			}
		}
	}

	void Scene::OnStaticTagConstruct(entt::registry&, EntityHandle entity)
	{
		// LoadGuard suppression: stateless hook — defer for the same reason
		// as the sprite renderer hook above.
		if (m_SuppressConstructHooks) {
			m_DeferredConstructHooks.push_back({ &Scene::OnStaticTagConstruct, entity });
			return;
		}
		MarkStaticRenderDataDirty();
	}

	void Scene::OnStaticTagDestroy(entt::registry& registry, EntityHandle entity)
	{
		MarkStaticRenderDataDirty();
		if (registry.all_of<StaticRenderData>(entity)) {
			registry.remove<StaticRenderData>(entity);
		}
	}

	void Scene::OnParticleSystem2DComponentConstruct(entt::registry& registry, EntityHandle entity) {
		// LoadGuard suppression: only touches own component fields, no
		// sibling reads — safe to defer.
		if (m_SuppressConstructHooks) {
			m_DeferredConstructHooks.push_back({ &Scene::OnParticleSystem2DComponentConstruct, entity });
			return;
		}
		auto& ps = registry.get<ParticleSystem2DComponent>(entity);
		ps.m_EmitterScene = this;
		ps.m_EmitterEntity = entity;
	}

	void Scene::OnParticleSystem2DComponentDestruct(entt::registry& registry, EntityHandle entity) {
		auto& ps = registry.get<ParticleSystem2DComponent>(entity);
		ps.m_EmitterScene = nullptr;
		ps.m_EmitterEntity = entt::null;
	}

	// ── Axiom-Physics component hooks ────────────────────────────────
	// All Fast* hooks gate on m_IsDetached the same way the Box2D
	// hooks above do — without it, prefab-inspector preview entities
	// would push real bodies/colliders into the singleton IndexPhysicsWorld
	// and corrupt simulation state on the live scene.

	void Scene::OnFastBody2DConstruct(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		auto& comp = registry.get<FastBody2DComponent>(entity);
		auto& indexWorld = PhysicsSystem2D::GetIndexPhysicsWorld();

		comp.m_Body = indexWorld.CreateBody(entity, comp.Type);
		if (comp.m_Body) {
			comp.m_Body->SetMass(comp.Mass);
			comp.m_Body->SetGravityEnabled(comp.UseGravity);
			comp.m_Body->SetBoundaryCheckEnabled(comp.BoundaryCheck);

			// Sync initial position from Transform
			if (HasComponent<Transform2DComponent>(entity)) {
				auto& tf = GetComponent<Transform2DComponent>(entity);
				comp.m_Body->SetPosition({ tf.Position.x, tf.Position.y });
			}
		}
	}

	void Scene::OnFastBody2DDestroy(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		PhysicsSystem2D::GetIndexPhysicsWorld().DestroyBody(entity);
	}

	void Scene::OnFastBoxCollider2DConstruct(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		auto& comp = registry.get<FastBoxCollider2DComponent>(entity);
		auto& indexWorld = PhysicsSystem2D::GetIndexPhysicsWorld();
		comp.m_Collider = indexWorld.CreateBoxCollider(entity, comp.HalfExtents);
	}

	void Scene::OnFastBoxCollider2DDestroy(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		PhysicsSystem2D::GetIndexPhysicsWorld().DestroyCollider(entity, IndexPhysicsWorld2D::FastColliderKind::Box);
	}

	void Scene::OnFastCircleCollider2DConstruct(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		auto& comp = registry.get<FastCircleCollider2DComponent>(entity);
		auto& indexWorld = PhysicsSystem2D::GetIndexPhysicsWorld();
		comp.m_Collider = indexWorld.CreateCircleCollider(entity, comp.Radius);
	}

	void Scene::OnFastCircleCollider2DDestroy(entt::registry& registry, EntityHandle entity) {
		if (m_IsDetached) return;
		PhysicsSystem2D::GetIndexPhysicsWorld().DestroyCollider(entity, IndexPhysicsWorld2D::FastColliderKind::Circle);
	}

	void Scene::ApplyEntityEnabledState(entt::registry& registry, EntityHandle entity, bool enabled)
	{
		if (!registry.valid(entity)) {
			return;
		}

		if (auto* rigidbody = registry.try_get<Rigidbody2DComponent>(entity); rigidbody && rigidbody->IsValid()) {
			rigidbody->SetEnabled(enabled);
		}

		if (auto* collider = registry.try_get<BoxCollider2DComponent>(entity); collider && collider->IsValid()) {
			collider->SetEnabled(enabled);
		}

		if (auto* collider = registry.try_get<CircleCollider2DComponent>(entity); collider && collider->IsValid()) {
			collider->SetEnabled(enabled);
		}

		if (auto* collider = registry.try_get<PolygonCollider2DComponent>(entity); collider && collider->IsValid()) {
			collider->SetEnabled(enabled);
		}

		if (registry.all_of<ScriptComponent>(entity)) {
			ScriptSystem::SetScriptsEnabled(Entity(entity, *this), enabled);
		}

		if (!registry.all_of<Camera2DComponent>(entity)) {
			return;
		}

		if (!enabled && m_MainCameraEntity == entity) {
			RefreshMainCameraSelection(registry, entt::null, entity);
		}
		else if (enabled
			&& (m_MainCameraEntity == entt::null
				|| !registry.valid(m_MainCameraEntity)
				|| registry.all_of<DisabledTag>(m_MainCameraEntity))) {
			RefreshMainCameraSelection(registry, entity);
		}
	}

	void Scene::RefreshMainCameraSelection(entt::registry& registry, EntityHandle preferred, EntityHandle excluded)
	{
		const auto isUsableCamera = [&](EntityHandle entity) {
			return entity != entt::null
				&& entity != excluded
				&& registry.valid(entity)
				&& registry.all_of<Camera2DComponent>(entity)
				&& !registry.all_of<DisabledTag>(entity);
		};

		if (isUsableCamera(preferred)) {
			m_MainCameraEntity = preferred;
			return;
		}

		auto view = registry.view<Camera2DComponent>(entt::exclude<DisabledTag>);
		for (EntityHandle candidate : view) {
			if (candidate != excluded) {
				m_MainCameraEntity = candidate;
				return;
			}
		}

		m_MainCameraEntity = entt::null;
	}
}
