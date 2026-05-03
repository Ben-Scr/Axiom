#pragma once

#include "Scene/ComponentCategory.hpp"
#include "Scene/Entity.hpp"
#include "Serialization/Json.hpp"

#include <span>
#include <string>

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
		void (*drawInspector)(std::span<const Entity>) = nullptr;

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
