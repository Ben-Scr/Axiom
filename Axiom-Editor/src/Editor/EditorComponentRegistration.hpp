#pragma once

#include "Scene/Entity.hpp"

#include <span>

namespace Axiom {
	class SceneManager;
	struct ComponentInfo;

	void RegisterEditorComponentInspectors(SceneManager& sceneManager);

	// Single dispatch entry-point for an inspector row. Honours a custom
	// drawInspector lambda when set, otherwise falls back to the unified
	// PropertyDrawer driven by ComponentInfo::properties. Inspector loops
	// (ImGuiEditorLayer, PrefabInspector) call this instead of invoking
	// info.drawInspector directly so the auto-drawer fallback is consistent.
	void DispatchComponentInspector(const ComponentInfo& info, std::span<const Entity> entities);
}
