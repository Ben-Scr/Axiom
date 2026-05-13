#pragma once

#include "Inspector/PropertyDescriptor.hpp"
#include "Scene/ComponentCategory.hpp"
#include "Scene/Entity.hpp"
#include "Serialization/Json.hpp"

#include <span>
#include <string>
#include <typeindex>
#include <vector>

namespace Axiom {

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

		bool (*has)(Entity) = nullptr;
		void (*add)(Entity) = nullptr;
		void (*remove)(Entity) = nullptr;
		void (*copyTo)(Entity src, Entity dst) = nullptr;

		// Raw pointer to the component instance for this entity (nullptr when missing).
		// Powers the ScriptCore ref-based API: C# casts the void* to a struct mirror of
		// the C++ component and reads/writes fields with no per-property P/Invoke. Auto-
		// wired by ComponentRegistry::Register for any non-empty T; empty tag types skip
		// it because there's no payload to expose. The returned pointer is valid only
		// until the next structural change to the same component pool (EnTT may move
		// the storage on add/remove), so callers must refetch rather than cache across
		// frames.
		void* (*getRaw)(Entity) = nullptr;

		// Fast path for simple ref queries. Fills `outPointers` with pointers to each
		// component instance in the component's own EnTT storage and returns the full
		// row count. If the count exceeds `maxRows`, only the prefix is written and
		// the managed caller retries with a larger buffer.
		int (*fillRawPointers)(entt::registry& registry, void** outPointers, int maxRows, int enableFilter) = nullptr;

		// sizeof(T) for the underlying C++ component, or 0 for empty/tag types.
		// Used by the managed side at script-engine init to detect layout drift between
		// the C++ component and its C# struct mirror — a mismatch hard-fails the user
		// assembly load instead of silently corrupting memory.
		size_t rawSize = 0;

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

		// ── Serialization callbacks ─────────────────────────────────────
		// Optional. When set, SceneSerializer routes this component through
		// the generic registry-driven path (SerializeEntity walks the
		// registry after the hardcoded built-ins and calls `serialize` on
		// any component with a non-null callback that the entity has;
		// DeserializeFullEntity / DeserializeComponent do the symmetric
		// lookup by `serializedName`). Built-in components leave both null
		// today — they're hardcoded in SceneSerializerDeserialize.cpp until
		// the H1 reflection refactor migrates them to this path. Package
		// components MUST set both to round-trip in .scene / .prefab files.
		Json::Value (*serialize)(Entity) = nullptr;
		void (*deserialize)(Entity, const Json::Value&) = nullptr;
	};

} // namespace Axiom
