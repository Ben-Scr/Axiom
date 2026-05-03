#pragma once
#include <imgui.h>

namespace Axiom::EditorTheme {

	// Editor-only color palette. Sourced once here so per-row tints in the
	// hierarchy and per-field tints in the inspector pull from a single
	// place instead of inlining ImVec4 literals.
	namespace Colors {

		// Prefab instance whose source GUID resolves in the AssetRegistry.
		// Drawn in the same accent-blue family as ImGuiCol_TabSelectedOverline.
		inline constexpr ImVec4 PrefabInstance = ImVec4(0.42f, 0.66f, 0.95f, 1.00f);

		// Reserved for "child of an instance, part of the source prefab".
		// Engine is currently flat (no parent/child) so this isn't used yet —
		// kept as a forward-compatible placeholder so the hierarchy code can
		// stop hardcoding when entity hierarchy lands.
		inline constexpr ImVec4 PrefabChild = ImVec4(0.55f, 0.70f, 0.85f, 0.85f);

		// Prefab instance whose source GUID can't be resolved (deleted file,
		// missing metadata). Warning amber — the entity still works locally
		// but apply/revert can't run.
		inline constexpr ImVec4 PrefabOrphan = ImVec4(0.95f, 0.70f, 0.30f, 1.00f);

		// Override marker dot drawn next to a component header when any
		// field of that component differs from the source prefab.
		inline constexpr ImVec4 OverrideMarker = ImVec4(0.42f, 0.66f, 0.95f, 1.00f);
	}
}
