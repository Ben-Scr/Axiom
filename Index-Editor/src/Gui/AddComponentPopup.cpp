#include <pch.hpp>
#include "Gui/AddComponentPopup.hpp"

#include "Gui/ImGuiUtils.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptDiscovery.hpp"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Index {

	namespace {

		std::string BuildScriptMenuLabel(const EditorScriptDiscovery::ScriptEntry& scriptEntry)
		{
			return scriptEntry.ClassName + "  " + scriptEntry.Extension;
		}

		// Same semantics as the ImGuiEditorLayer-internal helpers — duplicated
		// here so this popup can be hosted from both the regular inspector and
		// the asset-side prefab inspector without dragging the editor layer
		// translation unit into the dependency graph.
		bool AttachScriptToEntity(Entity entity, Scene& scene, const EditorScriptDiscovery::ScriptEntry& scriptEntry)
		{
			if (scriptEntry.ClassName.empty() || scriptEntry.Type == ScriptType::Unknown) {
				return false;
			}

			if (!entity.HasComponent<ScriptComponent>()) {
				entity.AddComponent<ScriptComponent>();
			}

			auto& scriptComponent = entity.GetComponent<ScriptComponent>();
			if (scriptComponent.HasScript(scriptEntry.ClassName, scriptEntry.Type)) {
				return false;
			}

			scriptComponent.AddScript(scriptEntry.ClassName, scriptEntry.Type);
			scene.MarkDirty();
			return true;
		}

		bool AttachManagedComponentToEntity(Entity entity, Scene& scene, const EditorScriptDiscovery::ScriptEntry& scriptEntry)
		{
			if (scriptEntry.ClassName.empty() || !scriptEntry.IsManagedComponent) {
				return false;
			}

			if (!entity.HasComponent<ScriptComponent>()) {
				entity.AddComponent<ScriptComponent>();
			}

			auto& scriptComponent = entity.GetComponent<ScriptComponent>();
			if (scriptComponent.HasManagedComponent(scriptEntry.ClassName)) {
				return false;
			}

			scriptComponent.AddManagedComponent(scriptEntry.ClassName);
			scene.MarkDirty();
			return true;
		}

	} // namespace

	void RenderAddComponentPopup(
		const char* popupId,
		Scene& scene,
		std::span<const Entity> entities,
		char* searchBuffer,
		std::size_t searchBufferSize,
		bool* outChanged)
	{
		// Local helper: set the caller's "did we change anything" flag.
		// Cheaper than threading the pointer through every add site.
		auto markChanged = [&]() {
			if (outChanged) *outChanged = true;
		};
		// Constrain the popup so a long component list can't extend past the
		// main viewport. Without this, the popup grows unbounded vertically
		// and the bottom entries scroll off the screen — there's no built-in
		// ImGui clamp for popup height. Min width keeps the search field
		// usable even with a short component name list. Min height keeps
		// the popup from collapsing to ~one tree-node tall when sub-trees
		// are closed (the previous "min 0" let the popup auto-size to a
		// few lines, which is what made it feel "way too small"). Max
		// height = 70% of the main viewport so there's always visible
		// chrome around it.
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const float availableH = viewport ? viewport->WorkSize.y : 800.0f;
		const float maxHeight = availableH * 0.7f;
		const float minHeight = std::min(maxHeight, std::max(360.0f, availableH * 0.45f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, minHeight),
			ImVec2(460.0f, maxHeight));

		if (!ImGui::BeginPopup(popupId)) {
			return;
		}

		// Defensive: an empty selection would make every "missing from any"
		// check vacuously true and offer the entire registry. The caller
		// shouldn't open the popup in that case, but be lenient.
		if (entities.empty()) {
			ImGui::TextDisabled("No entity selected.");
			ImGui::EndPopup();
			return;
		}

		const ComponentRegistry& registry = SceneManager::Get().GetComponentRegistry();

		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##CompSearch", "Search components...",
			searchBuffer, searchBufferSize);
		ImGui::Separator();

		// Wrap the list in a scrolling child so the parent popup respects
		// the size constraint above — without this, the inner contents
		// (TreeNodes, MenuItems) drive the popup taller than its constraint.
		ImGui::BeginChild("##AddComponentScroll", ImVec2(0, 0), false,
			ImGuiWindowFlags_HorizontalScrollbar);

		std::string filter(searchBuffer);
		std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

		const bool hasFilter = !filter.empty();

		auto componentMissingFromAny = [&](const ComponentInfo& info) -> bool {
			for (const Entity& e : entities) {
				if (!info.has(e)) return true;
			}
			return false;
		};

		auto addComponentToAll = [&](const ComponentInfo& info) {
			bool added = false;
			for (const Entity& e : entities) {
				if (!info.has(e)) {
					registry.AddWithDependencies(e, info.typeId);
					added = true;
				}
			}
			if (added) {
				scene.MarkDirty();
				markChanged();
			}
		};

		// Conflict check: any selected entity already holding a component that
		// declares a `conflictsWith` against the proposed type means the
		// proposed type can't be added without violating the invariant.
		// outConflictName receives the display name of the offending existing
		// component so the disabled tooltip can name it.
		auto componentConflictsWithSelection = [&](const std::type_index& proposed,
			std::string* outConflictName) -> bool
		{
			for (const Entity& e : entities) {
				if (registry.HasConflict(e, proposed)) {
					if (outConflictName) {
						registry.ForEachComponentInfo([&](const std::type_index& id, const ComponentInfo& info) {
							if (!outConflictName->empty()) return;
							if (id == proposed || !info.has || !info.has(e)) return;
							if (registry.TypesConflict(id, proposed)) *outConflictName = info.displayName;
						});
					}
					return true;
				}
			}
			return false;
		};

		std::vector<EditorScriptDiscovery::ScriptEntry> scriptEntries;
		EditorScriptDiscovery::CollectProjectScriptEntries(scriptEntries);

		// Index the .cs files that pair with a C++ component by display name
		// so we can:
		//   (a) hide the C++ entry from the regular General/Rendering/Physics/
		//       Audio buckets — it would otherwise be a duplicate of the entry
		//       under Native Components (C#);
		//   (b) render Native Components (C#) using the C++ display name (what
		//       the user sees in the runtime, e.g. "New Native") instead of
		//       the raw .cs file stem.
		// The lookup is keyed on `ComponentInfo::displayName` since that's the
		// string the [NativeComponent("...")] attribute matches against.
		std::unordered_map<std::string, const EditorScriptDiscovery::ScriptEntry*> nativeBackingByDisplayName;
		for (const auto& scriptEntry : scriptEntries) {
			if (!scriptEntry.IsNativeComponent || scriptEntry.NativeName.empty()) continue;
			nativeBackingByDisplayName.emplace(scriptEntry.NativeName, &scriptEntry);
		}
		const auto isNativeBacked = [&](const ComponentInfo& info) {
			return nativeBackingByDisplayName.find(info.displayName) != nativeBackingByDisplayName.end();
		};

		if (hasFilter) {
			// Flat filtered list when searching across both built-in components
			// and project scripts.
			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (info.category != ComponentCategory::Component) return;
				if (info.displayName == "Scripts") return;
				if (!componentMissingFromAny(info)) return;
				// Native-backed C++ entries surface under the Native
				// Components (C#) heading below — skip here to avoid the
				// duplicate ("New Native" appeared both in General and as
				// the .cs entry).
				if (isNativeBacked(info)) return;

				std::string lowerName = info.displayName;
				std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
				if (lowerName.find(filter) == std::string::npos) return;

				std::string conflictName;
				const bool conflicts = componentConflictsWithSelection(typeId, &conflictName);
				const bool enabled = !conflicts;
				if (ImGuiUtils::MenuItemEllipsis(info.displayName, info.displayName.c_str(), nullptr, false, enabled, 260.0f)) {
					if (enabled) {
						addComponentToAll(info);
						ImGui::CloseCurrentPopup();
					}
				}
				if (conflicts && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip("Conflicts with %s", conflictName.c_str());
				}
			});

			// Native Components (C#) — search hits use the C++ display name
			// so "New Native" matches even though the .cs file is named
			// "NewNativeComponent.cs". Click path goes through the C++
			// registry (same as adding the native component from its
			// regular subcategory would).
			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (info.category != ComponentCategory::Component) return;
				if (info.displayName == "Scripts") return;
				if (!componentMissingFromAny(info)) return;
				if (!isNativeBacked(info)) return;

				std::string lowerName = info.displayName;
				std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
				if (lowerName.find(filter) == std::string::npos) return;

				std::string conflictName;
				const bool conflicts = componentConflictsWithSelection(typeId, &conflictName);
				const bool enabled = !conflicts;
				if (ImGuiUtils::MenuItemEllipsis(info.displayName, info.displayName.c_str(), nullptr, false, enabled, 260.0f)) {
					if (enabled) {
						addComponentToAll(info);
						ImGui::CloseCurrentPopup();
					}
				}
				if (conflicts && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip("Conflicts with %s", conflictName.c_str());
				}
			});

			for (const auto& scriptEntry : scriptEntries) {
				if (scriptEntry.IsGameSystem || scriptEntry.IsGlobalSystem) {
					continue;
				}
				// Native-backed .cs files were already surfaced in the
				// previous registry pass under their C++ display name. The
				// raw .cs entry here would duplicate that listing — and
				// clicking the .cs row would route through the managed-
				// component path, which is wrong for native structs.
				if (scriptEntry.IsNativeComponent) {
					continue;
				}
				std::string lowerClassName = EditorScriptDiscovery::ToLowerCopy(scriptEntry.ClassName);
				std::string lowerPath = EditorScriptDiscovery::ToLowerCopy(scriptEntry.Path.string());
				if (lowerClassName.find(filter) == std::string::npos
					&& lowerPath.find(filter) == std::string::npos) {
					continue;
				}

				const std::string label = BuildScriptMenuLabel(scriptEntry);
				const std::string path = scriptEntry.Path.string();
				if (ImGuiUtils::MenuItemEllipsis(label, path.c_str(), nullptr, false, true, 260.0f)) {
					bool added = false;
					for (const Entity& e : entities) {
						if (scriptEntry.IsManagedComponent) {
							added |= AttachManagedComponentToEntity(const_cast<Entity&>(e), scene, scriptEntry);
						}
						else {
							added |= AttachScriptToEntity(const_cast<Entity&>(e), scene, scriptEntry);
						}
					}
					if (added) markChanged();
					ImGui::CloseCurrentPopup();
				}
			}
		}
		else {
			// Categorized tree view. Subcategory order is fixed so users learn
			// muscle memory; unknown subcategories append after the known ones.
			struct CategoryEntry { std::type_index TypeId; const ComponentInfo* Info; };
			std::vector<std::pair<std::string, std::vector<CategoryEntry>>> categories;
			std::unordered_map<std::string, size_t> categoryIndex;

			const std::vector<std::string> subcategoryOrder = {
				"General", "Rendering", "Physics", "Audio"
			};
			for (const auto& sub : subcategoryOrder) {
				categoryIndex[sub] = categories.size();
				categories.emplace_back(sub, std::vector<CategoryEntry>{});
			}

			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (info.category != ComponentCategory::Component) return;
				if (info.displayName == "Scripts") return;
				if (!componentMissingFromAny(info)) return;
				// Routed to the Native Components (C#) tree below — keeps
				// the regular General/Rendering/... categories free of
				// duplicate entries for scripts that own a paired C++ pool.
				if (isNativeBacked(info)) return;

				std::string sub = info.subcategory.empty() ? "General" : info.subcategory;
				auto it = categoryIndex.find(sub);
				if (it == categoryIndex.end()) {
					categoryIndex[sub] = categories.size();
					categories.emplace_back(sub, std::vector<CategoryEntry>{});
					it = categoryIndex.find(sub);
				}
				categories[it->second].second.push_back({ typeId, &info });
			});

			for (const auto& [subcategory, components] : categories) {
				if (components.empty()) continue;

				if (ImGui::TreeNode(subcategory.c_str())) {
					for (const auto& entry : components) {
						const ComponentInfo* info = entry.Info;
						std::string conflictName;
						const bool conflicts = componentConflictsWithSelection(entry.TypeId, &conflictName);
						const bool enabled = !conflicts;
						if (ImGuiUtils::MenuItemEllipsis(info->displayName, info->displayName.c_str(), nullptr, false, enabled, 260.0f)) {
							if (enabled) {
								addComponentToAll(*info);
								ImGui::CloseCurrentPopup();
							}
						}
						if (conflicts && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
							ImGui::SetTooltip("Conflicts with %s", conflictName.c_str());
						}
					}
					ImGui::TreePop();
				}
			}

			// Native Components (C#) — every C# `: IComponent` struct that
			// pairs with a registered C++ component shows up here using the
			// C++ display name. Clicking routes through the registry's
			// AddWithDependencies path (same as the regular subcategory
			// rows would), NOT through ScriptComponent::AddManagedComponent:
			// the data lives in the C++ ECS pool, not in a managed slot.
			struct NativeEntry { std::type_index TypeId; const ComponentInfo* Info; };
			std::vector<NativeEntry> nativeBackedComponents;
			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (info.category != ComponentCategory::Component) return;
				if (info.displayName == "Scripts") return;
				if (!isNativeBacked(info)) return;
				if (!componentMissingFromAny(info)) return;
				nativeBackedComponents.push_back({ typeId, &info });
			});
			if (!nativeBackedComponents.empty()) {
				if (ImGui::TreeNode("Native Components (C#)")) {
					for (const auto& entry : nativeBackedComponents) {
						const ComponentInfo* info = entry.Info;
						std::string conflictName;
						const bool conflicts = componentConflictsWithSelection(entry.TypeId, &conflictName);
						const bool enabled = !conflicts;
						if (ImGuiUtils::MenuItemEllipsis(info->displayName, info->displayName.c_str(), nullptr, false, enabled, 260.0f)) {
							if (enabled) {
								addComponentToAll(*info);
								ImGui::CloseCurrentPopup();
							}
						}
						if (conflicts && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
							ImGui::SetTooltip("Conflicts with %s", conflictName.c_str());
						}
					}
					ImGui::TreePop();
				}
			}

			// Components (C#) — managed `class : Component` types only.
			// IsManagedComponent and IsNativeComponent are mutually
			// exclusive in CollectScriptFile, so the IsManagedComponent
			// filter implicitly excludes native structs.
			bool hasManagedComponents = false;
			for (const auto& scriptEntry : scriptEntries) {
				if (scriptEntry.IsManagedComponent && !scriptEntry.IsGameSystem && !scriptEntry.IsGlobalSystem) {
					hasManagedComponents = true;
					break;
				}
			}
			if (hasManagedComponents) {
				if (ImGui::TreeNode("Components (C#)")) {
					for (const auto& scriptEntry : scriptEntries) {
						if (!scriptEntry.IsManagedComponent || scriptEntry.IsGameSystem || scriptEntry.IsGlobalSystem) {
							continue;
						}
						bool missingFromAny = false;
						for (const Entity& e : entities) {
							if (!e.HasComponent<ScriptComponent>()
								|| !e.GetComponent<ScriptComponent>().HasManagedComponent(scriptEntry.ClassName)) {
								missingFromAny = true;
								break;
							}
						}
						if (!missingFromAny) continue;
						const std::string label = BuildScriptMenuLabel(scriptEntry);
						const std::string path = scriptEntry.Path.string();
						if (ImGuiUtils::MenuItemEllipsis(label, path.c_str(), nullptr, false, true, 260.0f)) {
							bool added = false;
							for (const Entity& e : entities) {
								added |= AttachManagedComponentToEntity(const_cast<Entity&>(e), scene, scriptEntry);
							}
							if (added) markChanged();
							ImGui::CloseCurrentPopup();
						}
					}
					ImGui::TreePop();
				}
			}

			if (!scriptEntries.empty()) {
				if (ImGui::TreeNode("Scripts")) {
					for (const auto& scriptEntry : scriptEntries) {
						if (scriptEntry.IsManagedComponent || scriptEntry.IsNativeComponent
							|| scriptEntry.IsGameSystem || scriptEntry.IsGlobalSystem) {
							continue;
						}
						const std::string label = BuildScriptMenuLabel(scriptEntry);
						const std::string path = scriptEntry.Path.string();
						if (ImGuiUtils::MenuItemEllipsis(label, path.c_str(), nullptr, false, true, 260.0f)) {
							bool added = false;
							for (const Entity& e : entities) {
								added |= AttachScriptToEntity(const_cast<Entity&>(e), scene, scriptEntry);
							}
							if (added) markChanged();
							ImGui::CloseCurrentPopup();
						}
					}
					ImGui::TreePop();
				}
			}
		}

		ImGui::EndChild();
		ImGui::EndPopup();
	}

}
