#pragma once

#include "Inspector/PropertyDescriptor.hpp"
#include "Scene/ComponentCategory.hpp"
#include "Scene/Entity.hpp"
#include "Serialization/Json.hpp"

#include <functional>
#include <span>
#include <string>
#include <typeindex>
#include <vector>

namespace Index::Serialization { class IArchive; }

namespace Index {

	class DynamicComponentStorage;

	struct ComponentInfo {
		std::string displayName;
		std::string serializedName;
		std::string subcategory;
		ComponentCategory category;

		ComponentInfo() = default;
		ComponentInfo(const std::string& displayName, ComponentCategory category)
			: displayName(displayName), category(category) {
		}
		ComponentInfo(const std::string& displayName, const std::string& subcategory, ComponentCategory category)
			: displayName(displayName), subcategory(subcategory), category(category) {
		}

		// Set by ComponentRegistry::Register from `typeid(T)`. Lets callers
		// that hold a ComponentInfo* (e.g. clipboard paste, by-name lookups)
		// thread back into typed APIs like AddWithDependencies / TypesConflict
		// without re-iterating the registry.
		std::type_index typeId{ typeid(void) };

		// FNV-1a 64-bit hash of `serializedName`, computed once during
		// ComponentRegistry::Register and used by the hashed name → info
		// lookup map. The hash is stable across builds and platforms so it
		// can also be embedded directly in serialized assets (binary scene
		// v2 component table) — version-tolerant identification without a
		// per-asset string allocation. Zero indicates "not yet hashed"
		// (e.g. registration with an empty serializedName).
		uint64_t serializedNameHash = 0;

		// std::function (not raw function pointer) so dynamic-registration
		// callbacks can capture their backing DynamicComponentStorage. Static
		// registrations from Register<T>() continue to assign non-capturing
		// lambdas — those convert to std::function with no heap allocation
		// (most std libs SBO-store stateless callables inline).
		std::function<bool(Entity)> has;
		std::function<void(Entity)> add;
		std::function<void(Entity)> remove;
		std::function<void(Entity src, Entity dst)> copyTo;

		// Raw pointer to the component instance for this entity (nullptr when missing).
		// Powers the ScriptCore ref-based API: C# casts the void* to a struct mirror of
		// the C++ component and reads/writes fields with no per-property P/Invoke. Auto-
		// wired by ComponentRegistry::Register for any non-empty T; empty tag types skip
		// it because there's no payload to expose. The returned pointer is valid only
		// until the next structural change to the same component pool (EnTT may move
		// the storage on add/remove), so callers must refetch rather than cache across
		// frames.
		std::function<void*(Entity)> getRaw;

		// Fast path for simple ref queries. Fills `outPointers` with pointers to each
		// component instance in the component's own EnTT storage and returns the full
		// row count. If the count exceeds `maxRows`, only the prefix is written and
		// the managed caller retries with a larger buffer.
		std::function<int(entt::registry& registry, void** outPointers, int maxRows, int enableFilter)> fillRawPointers;

		// sizeof(T) for the underlying C++ component, or 0 for empty/tag types.
		// Used by the managed side at script-engine init to detect layout drift between
		// the C++ component and its C# struct mirror — a mismatch hard-fails the user
		// assembly load instead of silently corrupting memory.
		size_t rawSize = 0;

		// Stable u32 component ID assigned monotonically by ComponentRegistry::Register
		// in registration order, valid for the duration of the process. Used by the
		// scripting EntityCommandBuffer wire format so a recorded command can name its
		// target component type in 4 bytes (vs. a UTF-8 string + hash lookup). The ID
		// is NOT persisted to disk — serialized scenes still key on `serializedName`.
		// Zero is the sentinel "unassigned" (e.g. ComponentInfo authored outside the
		// registry, or registry slot 0 reserved as a null guard).
		uint32_t typeIdU32 = 0;

		// EnTT type hash for this component, auto-wired by ComponentRegistry::Register
		// to `entt::type_hash<T>::value()`. Powers the `registry.storage(id)` runtime
		// lookup used by the per-component query fast path — Scene_QueryEntities can
		// iterate the component's own sparse_set instead of walking every entity in
		// the registry. The native side already does this for typed views; this field
		// extends the same O(component count) iteration to the by-name script bindings.
		entt::id_type storageHash = 0;

		// Non-null for components registered via RegisterDynamic (i.e., user-authored
		// C# native components like UserData). Dynamic components live outside EnTT's
		// typed storage — `registry.storage(storageHash)` returns nullptr for them —
		// so the IJobQuery dispatcher in ScriptBindings.cpp uses this raw pointer for
		// O(1) Contains/Get instead of routing through the std::function callbacks,
		// which closes the ~8x perf gap vs. EnTT-backed components observed on a
		// 40k-entity (NativeTransform2D, UserData) IJobQuery. Lifetime is owned by
		// ComponentRegistry::m_dynamicStorages and stays valid until
		// UnregisterAllDynamic on assembly unload (which also drops the owning
		// ComponentInfo, so the pointer can never dangle).
		DynamicComponentStorage* dynamicStorage = nullptr;

		// memcpy-from-bytes emplacer. ECB playback writes a raw component payload into
		// the entity's storage in one call — no per-property P/Invoke, no JSON, no
		// generic field walker. Auto-wired by ComponentRegistry::Register for any
		// non-empty, trivially-copyable T as a simple `emplace_or_replace<T>(memcpy)`.
		// Components with scene-bound runtime state (e.g. ParticleSystem2DComponent's
		// emitter handle) must override this at registration time to rebind correctly,
		// the same way they currently override `copyTo`.
		std::function<void(entt::registry& registry, EntityHandle entity,
			const void* bytes, size_t size)> emplaceFromBytes;

		// Default-construct emplacer. Calls `registry.emplace<T>(entity)`, firing the
		// C++ default-member-initializers (e.g. Transform2DComponent::Scale{1,1},
		// SpriteRendererComponent::Color{1,1,1,1}). Auto-wired by
		// ComponentRegistry::Register for any non-empty, default-constructible T.
		// Used by the ECB's Ecb_DefaultConstructComponent opcode so
		// `CreateEntityWith<T...>` attaches components with engine defaults — the
		// managed-side `default(T)` is all-zero bytes and would silently clobber
		// those defaults if routed through emplaceFromBytes instead.
		std::function<void(entt::registry& registry, EntityHandle entity)> defaultEmplace;

		// Symmetric counterpart to emplaceFromBytes — serializes the live
		// component instance to a contiguous byte buffer that emplaceFromBytes
		// can later consume. Used by the PrefabTemplateCache to bake a prefab
		// into a memcpy-ready blob after the first slow-path hydrate so every
		// subsequent spawn skips JSON entirely.
		//
		// Contract: appends exactly `rawSize` bytes to `out` when the entity
		// has the component and returns true; returns false (and leaves `out`
		// untouched) when the component is missing. Auto-wired by
		// ComponentRegistry::Register for any non-empty T as a raw memcpy of
		// the EnTT storage. Components whose representation holds scene-bound
		// pointers / handles (ParticleSystem2DComponent's m_EmitterScene,
		// m_EmitterEntity) are still safe under the auto-wired path *iff*
		// they own an `on_construct` hook that rebinds — emplaceFromBytes
		// fires on_construct as part of `emplace_or_replace`. Components that
		// own non-trivial heap state (std::vector, std::string with non-SSO
		// payload) MUST override this with a body that projects a
		// serializable view, or the bytes will alias freed memory after the
		// source entity is destroyed.
		bool (*writeBytes)(const entt::registry& registry, EntityHandle entity,
			std::vector<uint8_t>& out) = nullptr;

		// Optional post-add hook fired by ComponentRegistry::AddWithDependencies
		// AFTER the default `add` runs. Used by UI widgets (Button, Slider,
		// Toggle, Dropdown, InputField, Scrollbar) to seed their NormalColor
		// from the entity's existing ImageComponent::Color so that adding the
		// component to a styled image preserves the user's color instead of
		// stamping over it with the component's default white. Not invoked
		// from raw `info.add(entity)` calls or from scripting / scene-load
		// paths — those write through deserialize / explicit field assignment
		// and don't want a "smart" default fighting their values.
		void (*onAdd)(Entity) = nullptr;
		// Inspector draw. Receives the full set of currently-selected entities
		// that have this component. For a single-entity selection the span has
		// size 1 — single-entity edits use the same code path as multi-entity.
		//
		// Two ways to populate this:
		//   1. Hand-written lambda (legacy / custom UX). Set drawInspector
		//      directly to a function pointer.
		//   2. Declarative properties (preferred for new components). Push
		//      PropertyDescriptors into `properties`; the editor auto-generates
		//      a drawInspector that walks the list and dispatches to
		//      PropertyDrawer::Draw(entities, descriptor) for each one.
		//
		// If both are set, drawInspector wins. The auto-generated drawer is
		// only installed when drawInspector is null AND properties is non-empty.
		void (*drawInspector)(std::span<const Entity>) = nullptr;

		// Editor-only viewport gizmo for the selected entity. The editor walks
		// every registered component on the selected entity and invokes this
		// callback on each one that sets it — so a component (built-in or
		// package) can paint its own selection helper (a particle-system shape,
		// a tilemap grid, a camera frustum, etc.) without touching the editor's
		// hardcoded gizmo list. Called inside the Editor View's gizmo pass with
		// `Gizmo::SetLayer(GizmoLayer::EditorOnly)` already set; the callback is
		// free to override layer/color/line-width — the editor saves and
		// restores those around the dispatch loop. No-op when null. Must be
		// safe to call when the entity has only some of its sibling components
		// (e.g. the tilemap gizmo bails if Transform2D isn't present).
		void (*drawEditorGizmo)(Entity) = nullptr;

		// Declarative property list. Populated by Properties::Add / MakeFlagEnum
		// / MakeTextureRef etc. Iterated by the editor's auto-drawer and (later)
		// by the generic JSON serializer to round-trip values without a custom
		// serialize/deserialize callback.
		std::vector<PropertyDescriptor> properties;

		// Components that cannot coexist on the same entity. The editor's
		// "Add Component" popup hides entries that conflict with anything
		// already on the selected entity. The relationship is implicitly
		// symmetric — declaring "A conflicts with B" on either side is
		// enough; ComponentRegistry::HasConflict checks both directions
		// at lookup time.
		//
		// Common pattern: visual-output renderers (sprite, image, tilemap,
		// particle system) tend to conflict with each other so an entity
		// has exactly one "what shows up at this transform" component.
		std::vector<std::type_index> conflictsWith;

		// Components that should be auto-added alongside this one when the
		// user adds it through the inspector / paste / scripting AddComponent
		// paths. Directed (NOT symmetric) and walked transitively by
		// ComponentRegistry::AddWithDependencies — pull in `Button` and you
		// also get its `dependsOn` (Interactable, RectTransform2D), and
		// anything those depend on, etc. A dep that would violate an
		// existing conflict on the entity is skipped with a warning rather
		// than failing the parent add — the policy is "less clicking, never
		// force" (so removal is unrestricted; nothing tracks dependents).
		// For a stricter "must have these or refuse to run" relationship —
		// e.g. user-script preconditions — see the future `requires`
		// system, which is intentionally separate.
		std::vector<std::type_index> dependsOn;

		// ── Serialization callbacks (legacy JSON-tree path) ──────────────
		// Optional. When set, SceneSerializer routes this component through
		// the generic registry-driven path (SerializeEntity walks the
		// registry after the hardcoded built-ins and calls `serialize` on
		// any component with a non-null callback that the entity has;
		// DeserializeFullEntity / DeserializeComponent do the symmetric
		// lookup by `serializedName`). Built-in components leave both null
		// today — they're hardcoded in SceneSerializerDeserialize.cpp until
		// the H1 reflection refactor migrates them to this path. Package
		// components MUST set both to round-trip in .scene / .prefab files.
		//
		// These two are SCHEDULED FOR REMOVAL once every component that
		// currently sets them has been migrated to `serializeArchive` below.
		// New components should NOT set these — write a single
		// `serializeArchive` body instead; SceneSerializer's JSON path will
		// route the call through JsonArchive and produce the same on-disk
		// shape automatically.
		Json::Value (*serialize)(Entity) = nullptr;
		void (*deserialize)(Entity, const Json::Value&) = nullptr;

		// ── Serialization callbacks (true-binary / unified path) ──────────
		// Optional. When set, SceneSerializer uses this single bidirectional
		// method for BOTH the binary AND JSON file formats by passing the
		// appropriate concrete archive (Index::Serialization::BinaryArchive
		// or JsonArchive). The component's body only calls Field / Object /
		// Array on the archive — it doesn't know or care which format is on
		// disk.
		//
		// When BOTH `serializeArchive` and the legacy `serialize` /
		// `deserialize` are set on the same component, the new path wins.
		// During the migration window the registry preserves whichever set
		// of callbacks is non-null across `AttachInspector` re-registration
		// so half-migrated components remain functional.
		//
		// `serializationVersion` is the per-component schema version, written
		// alongside each component frame in the binary format. Components
		// that add or rename fields in a non-backward-compatible way must
		// bump this and branch on `ar.ComponentVersion()` to support old
		// saves. Additive changes (a new Field call appended to the end of
		// Serialize) do NOT require a version bump — reading an older save
		// simply leaves the new field at its default.
		void (*serializeArchive)(Entity, Index::Serialization::IArchive&) = nullptr;
		std::uint16_t serializationVersion = 1;

		// True when the component was registered via ComponentRegistry::RegisterDynamic
		// (i.e., it was authored in the user's C# project, not as a hand-written C++
		// struct). UnregisterAllDynamic walks the registry on assembly unload and
		// drops every entry with this flag set, freeing the backing
		// DynamicComponentStorage and reclaiming its typeIdU32 slot.
		bool isDynamic = false;
	};

} // namespace Index
