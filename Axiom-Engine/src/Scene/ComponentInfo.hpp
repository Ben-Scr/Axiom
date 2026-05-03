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

		bool (*has)(Entity) = nullptr;
		void (*add)(Entity) = nullptr;
		void (*remove)(Entity) = nullptr;
		void (*copyTo)(Entity src, Entity dst) = nullptr;
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
