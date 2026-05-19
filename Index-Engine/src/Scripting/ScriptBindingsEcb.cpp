#include "pch.hpp"
#include "Scripting/EntityCommandBufferWire.hpp"
#include "Scripting/ScriptGlue.hpp"
#include "Scripting/ScriptBindings.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Serialization/PrefabTemplateCache.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Core/Application.hpp"
#include "Core/Log.hpp"

#include <cstring>
#include <span>
#include <vector>

namespace Index {

	// Resolve the active scene for ECB playback. Mirrors the static GetScene()
	// in ScriptBindings.cpp — we duplicate the tiny helper rather than expose
	// it across translation units, since the policy ("prefer the
	// ScriptEngine's scene, fall back to the SceneManager's active scene")
	// is local to the binding layer and a future change to that policy
	// should not silently affect the ECB path.
	static Scene* EcbResolveScene()
	{
		Scene* scene = ScriptEngine::GetScene();
		if (scene && scene->IsLoaded()) {
			return scene;
		}
		auto* app = Application::GetInstance();
		if (app && app->GetSceneManager()) {
			return app->GetSceneManager()->GetActiveScene();
		}
		return nullptr;
	}

	// Two-call buffer convention used by the binding layer: pull little-
	// endian primitives out of the wire buffer via memcpy to dodge unaligned-
	// load UB on platforms that care (the ECB stream is byte-packed and the
	// payload immediately follows a u16, leaving every following field on an
	// odd boundary). The compiler folds memcpy(&dst, src, sizeof(T)) into a
	// single load on x86/x64 so there's no real cost.
	template<typename T>
	static T EcbReadLE(const uint8_t* src) {
		T value;
		std::memcpy(&value, src, sizeof(T));
		return value;
	}

	static uint32_t Index_Component_GetTypeId(const char* componentName)
	{
		if (componentName == nullptr || componentName[0] == '\0') {
			return 0u;
		}
		const auto& registry = SceneManager::Get().GetComponentRegistry();
		// Hash fast path on the exact serializedName — matches the managed
		// caller, which always passes ComponentInfo.serializedName as the
		// canonical key.
		if (const ComponentInfo* hit = registry.FindBySerializedName(componentName)) {
			return hit->typeIdU32;
		}
		// "<Name>Component" fallback for managed scripts that named their
		// mirror type the C# way (e.g. "Transform2DComponent" on the wire
		// when the native serializedName is "Transform2D"). Avoids an
		// allocation when the suffix matches.
		constexpr const char kSuffix[] = "Component";
		constexpr std::size_t kSuffixLen = sizeof(kSuffix) - 1;
		const std::size_t nameLen = std::strlen(componentName);
		if (nameLen > kSuffixLen) {
			if (std::memcmp(componentName + nameLen - kSuffixLen, kSuffix, kSuffixLen) == 0) {
				std::string stem(componentName, nameLen - kSuffixLen);
				if (const ComponentInfo* hit = registry.FindBySerializedName(stem)) {
					return hit->typeIdU32;
				}
			}
		}
		// Slow displayName scan for legacy package types whose C# name
		// doesn't match either path above. ForEachComponentInfo is O(N) in
		// component count but this is a one-shot per type at managed
		// AppDomain load — not on any hot path.
		uint32_t found = 0u;
		registry.ForEachComponentInfo([&](const std::type_index&, const ComponentInfo& info) {
			if (found != 0u) return;
			if (info.displayName == componentName) {
				found = info.typeIdU32;
			}
		});
		return found;
	}

	// Per-binding-call scratch buffer for the bulk handles. Reused across
	// calls so a steady-state spawn loop (e.g. firing 100 bullets per
	// frame for several seconds) doesn't alloc + free a fresh vector each
	// time. Single-threaded by contract — ECB playback is main-thread only,
	// same as every other scripting binding.
	static std::vector<EntityHandle>& EcbHandleScratch()
	{
		static std::vector<EntityHandle> scratch;
		return scratch;
	}

	static int Index_Ecb_Playback(const uint8_t* buffer, int length,
		uint64_t* outRuntimeIds, int maxOut)
	{
		if (buffer == nullptr || length < static_cast<int>(sizeof(EcbHeader))) {
			return kEcbErrorTruncated;
		}
		Scene* scene = EcbResolveScene();
		if (scene == nullptr) {
			return kEcbErrorNoScene;
		}

		EcbHeader header;
		std::memcpy(&header, buffer, sizeof(EcbHeader));

		// Verify the entity table fits before we trust entityCount.
		const std::size_t entityTableBytes =
			static_cast<std::size_t>(header.entityCount) * sizeof(uint32_t);
		if (static_cast<std::size_t>(length) < sizeof(EcbHeader) + entityTableBytes) {
			return kEcbErrorTruncated;
		}
		if (outRuntimeIds == nullptr && header.entityCount > 0) {
			return kEcbErrorOutputTooSmall;
		}
		if (static_cast<uint32_t>(maxOut) < header.entityCount) {
			return kEcbErrorOutputTooSmall;
		}

		const uint8_t* entityTablePtr = buffer + sizeof(EcbHeader);
		const uint8_t* commandStreamPtr = entityTablePtr + entityTableBytes;
		const uint8_t* bufferEnd = buffer + length;

		// ── Pre-scan pass ──────────────────────────────────────────────
		// Two jobs:
		//   1. Bounds-validate every record once so the execute pass can
		//      walk the stream without re-checking.
		//   2. For each Ecb_InstantiatePrefab record, look up (or force-
		//      bake) the cached template so we know how many child
		//      entities the bulk-create needs to cover beyond the
		//      ECB-local root slots. Templates are recorded in the SAME
		//      order they appear in the command stream so the execute
		//      pass can consume them with a monotonically-advancing
		//      index — no per-record map lookup.
		PrefabTemplateCache& prefabCache = PrefabTemplateCache::Get();
		struct PrefabSpawn {
			uint32_t rootEntityIndex;        // ECB-local root slot
			uint64_t prefabGuid;
			uint32_t childBaseIndex;         // filled in after bulk-create sizing
			const PrefabTemplate* templ;
		};
		std::vector<PrefabSpawn> prefabSpawns;
		uint64_t totalExtraChildren = 0;

		{
			const uint8_t* cursor = commandStreamPtr;
			for (uint32_t cmdIdx = 0; cmdIdx < header.commandCount; ++cmdIdx) {
				if (cursor + 11 > bufferEnd) {
					return kEcbErrorTruncated;
				}
				const uint8_t  opcode      = cursor[0];
				const uint32_t entityIndex = EcbReadLE<uint32_t>(cursor + 1);
				const uint16_t payloadSize = EcbReadLE<uint16_t>(cursor + 9);
				const uint8_t* payload     = cursor + 11;
				if (payload + payloadSize > bufferEnd) {
					return kEcbErrorTruncated;
				}

				if (opcode == Ecb_InstantiatePrefab) {
					if (entityIndex >= header.entityCount || payloadSize != 8) {
						IDX_CORE_WARN_TAG("Ecb",
							"Malformed Ecb_InstantiatePrefab at cmd {} (entityIndex={}, payloadSize={})",
							cmdIdx, entityIndex, payloadSize);
						return kEcbErrorTruncated;
					}
					const uint64_t guid = EcbReadLE<uint64_t>(payload);
					const PrefabTemplate* templ = prefabCache.Find(guid);
					if (templ == nullptr) {
						// Bake-on-demand: trigger the slow path once so
						// CaptureFromLive populates the cache, then
						// destroy the throwaway tree before continuing.
						// Cost is paid exactly once per (prefab GUID,
						// process) — subsequent ECB batches with the
						// same prefab take the fast path.
						EntityHandle scratchRoot = SceneSerializer::InstantiatePrefab(*scene, guid);
						if (scratchRoot != entt::null) {
							scene->DestroyEntity(scratchRoot);
						}
						templ = prefabCache.Find(guid);
					}
					if (templ == nullptr || !templ->bakeable) {
						IDX_CORE_WARN_TAG("Ecb",
							"Ecb_InstantiatePrefab rejected prefab {}: {}",
							guid, (templ != nullptr) ? templ->unbakeableReason : std::string("unknown prefab"));
						return kEcbErrorBadPrefab;
					}
					prefabSpawns.push_back({ entityIndex, guid, /*childBaseIndex=*/0u, templ });
					totalExtraChildren += static_cast<uint64_t>(templ->EntityCount() - 1u);
				}

				cursor = payload + payloadSize;
			}
		}

		const uint64_t totalEntityCount64 =
			static_cast<uint64_t>(header.entityCount) + totalExtraChildren;

		// ── Entity-cap check ───────────────────────────────────────────
		// Refuse a batch that would push the live-entity count past EnTT's
		// configured entity_mask. Without this guard, registry.create() at
		// the cap returns a wrapped entity index and the next sparse-set
		// write reaches into garbage memory. The check covers prefab
		// children too (added to totalEntityCount64 above).
		using EntityTraits = entt::entt_traits<EntityHandle>;
		constexpr uint64_t kMaxLiveEntities = static_cast<uint64_t>(EntityTraits::entity_mask);
		const uint64_t currentCount = static_cast<uint64_t>(scene->GetEntityCount());
		if (currentCount + totalEntityCount64 > kMaxLiveEntities) {
			IDX_CORE_ERROR_TAG("Ecb",
				"Ecb_Playback rejected: batch would exceed entity cap "
				"(current={}, requested={}+{} prefab children, cap={}). Raise 'entityBits' in "
				"index-project.json and rebuild (Project Settings > Entity ID bits).",
				currentCount, header.entityCount, totalExtraChildren, kMaxLiveEntities);
			return kEcbErrorEntityCapExceeded;
		}
		const uint32_t totalEntityCount = static_cast<uint32_t>(totalEntityCount64);

		// ── Bulk reserve + bulk create ─────────────────────────────────
		// Root slots occupy [0, header.entityCount); prefab children
		// follow in [header.entityCount, totalEntityCount). The runtime
		// IDs we surface to managed are ONLY the root slots — children
		// are accessible via the root entity's hierarchy after playback.
		scene->ReserveForLoadRuntime(totalEntityCount, {});

		std::vector<EntityHandle>& handles = EcbHandleScratch();
		handles.resize(totalEntityCount);
		scene->CreateEntitiesBulk(totalEntityCount, handles);

		// Assign each prefab spawn its child block now that the layout
		// is known.
		{
			uint32_t base = header.entityCount;
			for (PrefabSpawn& spawn : prefabSpawns) {
				spawn.childBaseIndex = base;
				base += (spawn.templ->EntityCount() - 1u);
			}
		}

		// LoadGuard suppresses Transform/Sprite/StaticTag/ParticleSystem
		// on_construct hooks until the end of this scope. Wraps both the
		// AddComponent loop and every prefab HydrateInto call.
		Scene::LoadGuard guard(*scene);

		// Identity stamping for plain (non-prefab) root slots. Prefab
		// roots are stamped by HydrateInto below with Origin::Prefab +
		// the prefab GUID — re-stamping them here would just overwrite
		// the correct value with Origin::Runtime.
		std::vector<uint8_t> isPrefabRoot(header.entityCount, 0);
		for (const PrefabSpawn& spawn : prefabSpawns) {
			isPrefabRoot[spawn.rootEntityIndex] = 1;
		}
		for (uint32_t i = 0; i < header.entityCount; ++i) {
			if (isPrefabRoot[i] == 0) {
				scene->SetEntityMetaDataNoFlags(handles[i], EntityOrigin::Runtime);
			}
		}

		// ── Execute pass ───────────────────────────────────────────────
		// Walk the command stream. Each record self-describes its size
		// via the payloadSize u16 so an unknown opcode can be safely
		// skipped without losing alignment for the rest of the stream —
		// future-extensible without bumping the wire format major version.
		const ComponentRegistry& componentRegistry = SceneManager::Get().GetComponentRegistry();

		// Scratch buffer for prefab slot mapping (root + children). Reused
		// across every prefab spawn in this batch so we allocate at most
		// once per playback, sized to the largest template seen.
		std::vector<EntityHandle> prefabSlotScratch;
		std::size_t nextPrefabSpawn = 0;

		// Mirror Index_Entity_AddComponent (ScriptBindings.cpp): a dynamic
		// `:IComponent` add must also seed a Managed ScriptComponent.Scripts
		// entry so the inspector's script-field renderer picks it up. Applies
		// to every ECB opcode that introduces a dynamic component on an entity.
		// Idempotent — guards make repeated calls (e.g. SetComponent after
		// AddComponent) no-ops.
		auto seedScriptsEntryForDynamic = [&](const ComponentInfo* info, EntityHandle handle) {
			if (!info->isDynamic) return;
			Entity entity = scene->GetEntity(handle);
			if (!entity.HasComponent<ScriptComponent>()) {
				entity.AddComponent<ScriptComponent>();
			}
			auto& sc = entity.GetComponent<ScriptComponent>();
			if (!sc.HasScript(info->serializedName, ScriptType::Managed)) {
				sc.AddScript(info->serializedName, ScriptType::Managed);
			}
		};

		const uint8_t* cursor = commandStreamPtr;
		for (uint32_t cmdIdx = 0; cmdIdx < header.commandCount; ++cmdIdx) {
			// Pre-scan already bounds-checked every record, but re-check
			// defensively in case the buffer was mutated concurrently
			// (the wire is supposed to be quiescent per the ECB contract,
			// but cheap insurance against UB if the contract is violated).
			if (cursor + 11 > bufferEnd) {
				return kEcbErrorTruncated;
			}
			const uint8_t  opcode       = cursor[0];
			const uint32_t entityIndex  = EcbReadLE<uint32_t>(cursor + 1);
			const uint32_t typeId       = EcbReadLE<uint32_t>(cursor + 5);
			const uint16_t payloadSize  = EcbReadLE<uint16_t>(cursor + 9);
			const uint8_t* payload      = cursor + 11;
			if (payload + payloadSize > bufferEnd) {
				return kEcbErrorTruncated;
			}
			cursor = payload + payloadSize;

			if (entityIndex >= header.entityCount) {
				IDX_CORE_WARN_TAG("Ecb", "Skipping command with out-of-range entityIndex {}", entityIndex);
				continue;
			}

			if (opcode == Ecb_AddComponent || opcode == Ecb_SetComponent) {
				const ComponentInfo* info = componentRegistry.GetByTypeId(typeId);
				if (info == nullptr || info->emplaceFromBytes == nullptr) {
					// Hard error: silently dropping the command was the
					// previous behavior and led to the "Transform appears,
					// SpriteRenderer missing" bug. Surface the typeId in
					// the log AND fail the entire playback so the managed
					// caller's Playback() throws — every future regression
					// of this class is now instantly visible.
					IDX_CORE_WARN_TAG("Ecb",
						"AddComponent for typeId {} cannot proceed: no registered "
						"component or component has no emplaceFromBytes callback "
						"(component holds non-memcpy-safe state and needs a "
						"custom emplacer — see ComponentRegistry contract).",
						typeId);
					return kEcbErrorUnknownComponent;
				}
				info->emplaceFromBytes(scene->GetRegistry(), handles[entityIndex],
					payload, payloadSize);
				seedScriptsEntryForDynamic(info, handles[entityIndex]);
			}
			else if (opcode == Ecb_DefaultConstructComponent) {
				// Payload-free record — typeId selects the component, and the
				// auto-wired `defaultEmplace` runs `registry.emplace<T>(handle)`
				// so the C++ member-initializers fire. Failure here is the
				// same surface as Ecb_AddComponent: unknown typeId OR a
				// component that opted out of default-construction (no
				// auto-wired callback because it isn't default-constructible).
				const ComponentInfo* info = componentRegistry.GetByTypeId(typeId);
				if (info == nullptr || info->defaultEmplace == nullptr) {
					IDX_CORE_WARN_TAG("Ecb",
						"DefaultConstructComponent for typeId {} cannot proceed: no "
						"registered component or component is not default-constructible "
						"(use the value or patch overload of AddComponent instead).",
						typeId);
					return kEcbErrorUnknownComponent;
				}
				info->defaultEmplace(scene->GetRegistry(), handles[entityIndex]);
				seedScriptsEntryForDynamic(info, handles[entityIndex]);
			}
			else if (opcode == Ecb_InstantiatePrefab) {
				// prefabSpawns is built in command-stream order by the
				// pre-scan; the cursor advances in lockstep, so a simple
				// monotone index recovers the matching spawn record.
				if (nextPrefabSpawn >= prefabSpawns.size()) {
					IDX_CORE_ERROR_TAG("Ecb",
						"Ecb_InstantiatePrefab execute-pass iterator out of sync with pre-scan (cmd {})", cmdIdx);
					return kEcbErrorTruncated;
				}
				const PrefabSpawn& spawn = prefabSpawns[nextPrefabSpawn++];
				const PrefabTemplate& templ = *spawn.templ;
				const uint32_t childCount = templ.EntityCount();

				prefabSlotScratch.resize(childCount);
				prefabSlotScratch[0] = handles[spawn.rootEntityIndex];
				for (uint32_t k = 1; k < childCount; ++k) {
					prefabSlotScratch[k] = handles[spawn.childBaseIndex + k - 1u];
				}

				prefabCache.HydrateInto(spawn.prefabGuid, *scene,
					std::span<const EntityHandle>(prefabSlotScratch));
			}
			// Unknown opcodes: skip (forwards-compat). We already advanced
			// `cursor` past the record's payload, so the stream stays
			// aligned for the next iteration.
		}

		// Write the output IDs after the command loop so a mid-stream
		// truncation error rejects the entire playback rather than handing
		// the caller a half-populated buffer.
		for (uint32_t i = 0; i < header.entityCount; ++i) {
			outRuntimeIds[i] = scene->GetEntityPersistentID(handles[i]);
		}

		// LoadGuard destructor fires deferred hooks here, then the
		// single end-of-batch dirty pulse runs (matches the per-entity
		// path's m_Dirty / m_UIDirty bookkeeping at end-of-call).
		// The pulse intentionally lives outside the guard scope so the
		// flush isn't double-counted by a later MarkAllDirtyOnce().
		// (Falling out of the guard's scope happens automatically as
		// we return.)
		scene->MarkAllDirtyOnce();
		return static_cast<int>(header.entityCount);
	}

	void PopulateEcbBindings(NativeBindings& b)
	{
		b.Component_GetTypeId = &Index_Component_GetTypeId;
		b.Ecb_Playback        = &Index_Ecb_Playback;
	}

} // namespace Index
