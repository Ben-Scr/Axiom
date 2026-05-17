#include "pch.hpp"
#include "Scripting/EntityCommandBufferWire.hpp"
#include "Scripting/ScriptGlue.hpp"
#include "Scripting/ScriptBindings.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Core/Application.hpp"
#include "Core/Log.hpp"

#include <cstring>
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

		// Bulk-reserve + bulk-create. The single CreateEntitiesBulk call
		// collapses N entity-pool allocations into one — primary speedup
		// over the per-entity Entity_Create binding.
		scene->ReserveForLoadRuntime(header.entityCount, {});

		std::vector<EntityHandle>& handles = EcbHandleScratch();
		handles.resize(header.entityCount);
		scene->CreateEntitiesBulk(header.entityCount, handles);

		// LoadGuard suppresses Transform/Sprite/StaticTag/ParticleSystem
		// on_construct hooks until the end of this scope; they fire once
		// per recorded entity in a tight loop instead of being interleaved
		// with the per-entity emplace cost. Physics-related hooks still
		// fire in-line (see Scene.hpp LoadGuard comment for the rationale).
		Scene::LoadGuard guard(*scene);

		// Identity stamping + emplace loop. Each entity gets its metadata
		// component (Runtime origin auto-allocates a fresh RuntimeID), so
		// the managed callers can use the returned IDs with Entity.FindByX
		// without a second resolve pass.
		for (uint32_t i = 0; i < header.entityCount; ++i) {
			scene->SetEntityMetaDataNoFlags(handles[i], EntityOrigin::Runtime);
		}

		// Walk the command stream. Each record self-describes its size via
		// the payloadSize u16 so an unknown opcode can be safely skipped
		// without losing alignment for the rest of the stream — future-
		// extensible without bumping the wire format major version.
		const ComponentRegistry& componentRegistry = SceneManager::Get().GetComponentRegistry();
		const uint8_t* cursor = commandStreamPtr;
		for (uint32_t cmdIdx = 0; cmdIdx < header.commandCount; ++cmdIdx) {
			// Bounds-check the fixed-size record header (1 + 4 + 4 + 2 = 11 bytes)
			// before reading any field.
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
					// Bail before MarkAllDirtyOnce / outRuntimeIds writes so
					// the caller sees a clean "nothing applied" failure rather
					// than a half-populated batch. Matches the truncation
					// error handling above.
					return kEcbErrorUnknownComponent;
				}
				info->emplaceFromBytes(scene->GetRegistry(), handles[entityIndex],
					payload, payloadSize);
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
