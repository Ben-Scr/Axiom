#pragma once
#include "Scene/Entity.hpp"
#include <cstddef>
#include <span>

namespace Index {

	class Scene;

	// Categorized + searchable Add Component popup body. Shared between the
	// entity inspector (RenderInspectorPanel) and the asset-side prefab
	// inspector (PrefabInspector) so the user gets the same UX whether they
	// click "Add Component" on a regular entity or on a `.prefab` asset.
	//
	// The caller is responsible for placing the trigger button and calling
	// ImGui::OpenPopup(popupId). This function only renders the body inside
	// BeginPopup/EndPopup.
	//
	// `searchBuffer`/`searchBufferSize` is per-popup persistent state for the
	// search box; the caller owns it across frames so the input survives a
	// rebuild of the popup contents on each frame.
	//
	// Multi-entity selections: components present on EVERY entity are hidden;
	// adding writes to every selected entity that's currently missing the
	// component (matches the rest of the multi-edit inspector behavior).
	// Conflict checks run against the union of components on the selection,
	// so an entry is disabled when adding it to ANY selected entity would
	// violate a `conflictsWith` declaration.
	//
	// `outChanged` (optional): set to true when the popup actually adds a
	// component (or game system) this frame. Callers that cache derived
	// per-selection state — currently the entity inspector's component-
	// intersection cache — use this to invalidate exactly when needed
	// rather than every frame the popup is open.
	void RenderAddComponentPopup(
		const char* popupId,
		Scene& scene,
		std::span<const Entity> entities,
		char* searchBuffer,
		std::size_t searchBufferSize,
		bool* outChanged = nullptr);

}
