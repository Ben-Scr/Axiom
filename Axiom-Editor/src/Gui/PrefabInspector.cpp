#include <pch.hpp>
#include "Gui/PrefabInspector.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Core/Log.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializer.hpp"

#include <imgui.h>

#include <filesystem>
#include <span>
#include <vector>

namespace Axiom {

	PrefabInspector::PrefabInspector() = default;
	PrefabInspector::~PrefabInspector() = default;

	void PrefabInspector::Open(const std::string& prefabPath) {
		Close();

		m_PrefabScene = Scene::CreateDetachedEditorScene("##PrefabInspector");
		m_PrefabPath = prefabPath;

		if (!File::Exists(prefabPath)) {
			AIM_CORE_WARN_TAG("PrefabInspector", "Prefab file not found: {}", prefabPath);
			return;
		}

		const std::string json = File::ReadAllText(prefabPath);
		Json::Value root;
		std::string parseError;
		if (!Json::TryParse(json, root, &parseError) || !root.IsObject()) {
			AIM_CORE_ERROR_TAG("PrefabInspector", "Failed to parse {}: {}", prefabPath, parseError);
			return;
		}

		// The .prefab envelope keeps both "Entity" (preferred) and a legacy
		// "prefab" key with the same payload. Prefer the new key, fall back.
		const Json::Value* entityValue = root.FindMember("Entity");
		if (!entityValue) entityValue = root.FindMember("prefab");
		if (!entityValue || !entityValue->IsObject()) {
			AIM_CORE_WARN_TAG("PrefabInspector", "No Entity/prefab block in {}", prefabPath);
			return;
		}

		// Deserialize as a Scene-origin entity (not a Prefab instance) so the
		// inspector edits a self-contained tree that round-trips back to the
		// .prefab file via SaveEntityToFile without leaking instance metadata.
		m_RootEntity = SceneSerializer::DeserializeEntityFromValue(*m_PrefabScene, *entityValue);
		// DeserializeEntityFromValue has already marked the detached scene
		// dirty as a side effect of creating the entity. Reset so we only
		// flag dirty on actual user edits.
		m_PrefabScene->ClearDirty();
	}

	void PrefabInspector::Close() {
		// unique_ptr destruction tears down the scene's registry, which fires
		// destroy hooks. The hooks gated by Scene::IsEditorPreview() will skip,
		// so we don't leak into the global physics/audio/script subsystems.
		m_RootEntity = entt::null;
		m_PrefabScene.reset();
		m_PrefabPath.clear();
	}

	bool PrefabInspector::HasUnsavedChanges() const {
		return m_PrefabScene && m_PrefabScene->IsDirty();
	}

	bool PrefabInspector::Render() {
		if (!m_PrefabScene || m_RootEntity == entt::null || !m_PrefabScene->IsValid(m_RootEntity)) {
			ImGui::TextDisabled("No prefab loaded.");
			return false;
		}

		const auto& registry = SceneManager::Get().GetComponentRegistry();
		Entity rootEntity = m_PrefabScene->GetEntity(m_RootEntity);
		const std::span<const Entity> entitySpan(&rootEntity, 1);

		// Header — file path + auto-save status. No Save button: edits are
		// flushed to disk automatically at the bottom of this function as
		// soon as the user releases the active widget.
		const std::string filename = std::filesystem::path(m_PrefabPath).filename().string();
		ImGui::TextDisabled("Prefab:");
		ImGui::SameLine();
		ImGui::TextUnformatted(filename.c_str());
		ImGui::SameLine();
		const bool dirty = m_PrefabScene->IsDirty();
		ImGui::TextDisabled(dirty ? "(saving)" : "(auto-saved)");
		ImGui::Separator();

		// Component dispatch — same pattern as the entity inspector but without
		// clipboard / copy / paste / reset (those can come in v2).
		std::type_index pendingRemoval = typeid(void);

		registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
			if (info.category != ComponentCategory::Component) return;
			if (info.displayName == "Name") return; // Edited via the entity name field, if shown.
			if (!info.has || !info.has(rootEntity)) return;

			if (info.displayName == "Scripts") {
				if (info.drawInspector) info.drawInspector(entitySpan);
				return;
			}

			bool removeRequested = false;
			bool open = ImGuiUtils::BeginComponentSection(info.displayName.c_str(), removeRequested,
				[]() {});
			if (removeRequested) {
				pendingRemoval = typeId;
			}

			if (open) {
				if (info.drawInspector) {
					info.drawInspector(entitySpan);
				}
				ImGuiUtils::EndComponentSection();
			}
		});

		if (pendingRemoval != typeid(void)) {
			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (typeId == pendingRemoval && info.remove) {
					info.remove(rootEntity);
				}
			});
			m_PrefabScene->MarkDirty();
		}

		// Add Component popup. Bare list of registered components missing from
		// the entity. No script-discovery search in v1 — only built-in components.
		ImGui::Separator();
		const float buttonWidth = ImGui::GetContentRegionAvail().x;
		if (ImGui::Button("Add Component", ImVec2(buttonWidth, 0))) {
			ImGui::OpenPopup("AddPrefabComponentPopup");
		}
		if (ImGui::BeginPopup("AddPrefabComponentPopup")) {
			registry.ForEachComponentInfo([&](const std::type_index&, const ComponentInfo& info) {
				if (info.category != ComponentCategory::Component) return;
				if (!info.has || info.has(rootEntity)) return;
				if (!info.add) return;
				if (info.displayName == "Name") return;
				if (ImGui::MenuItem(info.displayName.c_str())) {
					info.add(rootEntity);
					m_PrefabScene->MarkDirty();
				}
			});
			ImGui::EndPopup();
		}

		// Heuristic dirty signal: if any item is currently being interacted with,
		// mark the detached scene dirty. Component-add / -remove above have
		// already handled their own dirtying — this catches every drag/edit on
		// the field widgets without requiring per-drawer instrumentation.
		const bool itemActive = ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (itemActive) {
			m_PrefabScene->MarkDirty();
		}

		// Auto-save: flush as soon as the prefab is dirty AND no widget is
		// currently being interacted with. During a drag the slider stays
		// "active" every frame, so this naturally debounces — Save() fires
		// once on release. One-shot edits (Add Component, Remove Component)
		// have no held item, so they save on the same frame they happen.
		// A failing Save() leaves the dirty flag set; we'll retry next frame.
		const bool dirtyForSave = m_PrefabScene->IsDirty();
		if (dirtyForSave && !itemActive) {
			Save();
		}

		return dirtyForSave;
	}

	bool PrefabInspector::Save() {
		if (!m_PrefabScene || m_RootEntity == entt::null) return false;

		// Capture the OLD source JSON before we overwrite it on disk. Live
		// instance propagation uses this as the baseline for computing each
		// instance's per-field overrides — diffing against the new source
		// after save would lose the user's overrides.
		Json::Value previousSourceEntity;
		bool havePreviousSource = false;
		if (File::Exists(m_PrefabPath)) {
			Json::Value previousRoot;
			std::string parseError;
			if (Json::TryParse(File::ReadAllText(m_PrefabPath), previousRoot, &parseError) && previousRoot.IsObject()) {
				if (const Json::Value* entity = previousRoot.FindMember("Entity")) {
					previousSourceEntity = *entity;
					havePreviousSource = true;
				}
				else if (const Json::Value* legacy = previousRoot.FindMember("prefab")) {
					previousSourceEntity = *legacy;
					havePreviousSource = true;
				}
			}
		}

		if (!SceneSerializer::SaveEntityToFile(*m_PrefabScene, m_RootEntity, m_PrefabPath)) {
			return false;
		}
		m_PrefabScene->ClearDirty();

		const uint64_t prefabGuid = AssetRegistry::GetOrCreateAssetUUID(m_PrefabPath);
		if (prefabGuid != 0 && havePreviousSource) {
			PropagateToLiveInstances(prefabGuid, previousSourceEntity);
		}
		return true;
	}

	void PrefabInspector::PropagateToLiveInstances(uint64_t prefabGuid,
		const Json::Value& previousSourceEntity) {
		// For every loaded scene (excluding our own detached preview), refresh
		// every instance of this prefab. RefreshPrefabInstance snapshots the
		// instance's overrides relative to the OLD source, re-instantiates
		// against the NEW source, and re-applies the overrides on top — so
		// other instances' overrides survive an Apply from any one instance
		// (the spec's "overrides win over apply" policy).
		SceneManager::Get().ForeachLoadedScene([&](Scene& scene) {
			if (&scene == m_PrefabScene.get()) return;

			std::vector<EntityHandle> instancesToRefresh;
			auto view = scene.GetRegistry().view<EntityMetaDataComponent>();
			for (entt::entity ent : view) {
				const auto& meta = view.get<EntityMetaDataComponent>(ent).MetaData;
				if (meta.Origin != EntityOrigin::Prefab) continue;
				if (static_cast<uint64_t>(meta.PrefabGUID) != prefabGuid) continue;
				instancesToRefresh.push_back(ent);
			}

			bool anyRefreshed = false;
			for (EntityHandle instance : instancesToRefresh) {
				if (!scene.IsValid(instance)) continue;
				EntityHandle replacement = SceneSerializer::RefreshPrefabInstance(scene, instance, previousSourceEntity);
				if (replacement != entt::null) {
					anyRefreshed = true;
				}
			}

			if (anyRefreshed) {
				scene.MarkDirty();
			}
		});
	}

}
