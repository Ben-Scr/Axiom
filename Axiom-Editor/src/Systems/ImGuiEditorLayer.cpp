#include <pch.hpp>
#include "ImGuiEditorLayer.hpp"

#include <imgui.h>
#include <imgui_internal.h>
// glad include retired with the legacy GL backend (2026-05); the editor
// no longer makes raw GL calls, all drawing routes through engine renderers.

#include <cstdio>

#include "Components/Components.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Graphics/OpenGL.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Graphics/GizmoRenderer.hpp"
#include "Graphics/Gizmo.hpp"
#include "Gui/GuiRenderer.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/ComponentRegistry.hpp"
#include <Scene/EntityHelper.hpp>

#include "Graphics/TextureManager.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Serialization/Path.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/File.hpp"
#include "Utils/Process.hpp"
#include "Packages/NuGetSource.hpp"
#include "Packages/GitHubSource.hpp"
#include "Editor/ApplicationEditorAccess.hpp"
#include "Editor/EditorComponentRegistration.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Inspector/ReferencePicker.hpp"
#include "Gui/AddComponentPopup.hpp"
#include "Gui/EditorIcons.hpp"
#include "Gui/EditorTheme.hpp"
#include "Gui/ImGuiContextLayer.hpp"
#include "Gui/HierarchyDragData.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scripting/ScriptSystem.hpp"
#include "Scripting/ScriptDiscovery.hpp"
#include "Scripting/ScriptComponentInspector.hpp"
#include "Systems/TransformHierarchySystem.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_set>

namespace Axiom {
	namespace {
		void StartPlayOnAwakeComponents(Scene& scene)
		{
			auto audioView = scene.GetRegistry().view<AudioSourceComponent>(entt::exclude<DisabledTag>);
			for (auto [ent, audio] : audioView.each()) {
				if (audio.GetPlayOnAwake() && audio.GetAudioHandle().IsValid()) {
					audio.Play();
				}
			}

			auto particleView = scene.GetRegistry().view<ParticleSystem2DComponent>(entt::exclude<DisabledTag>);
			for (auto [ent, particleSystem] : particleView.each()) {
				if (particleSystem.PlayOnAwake) {
					particleSystem.Play();
				}
			}
		}

		bool HasSelectedAncestor(const Scene& scene, EntityHandle entity, const std::unordered_set<uint32_t>& selectedEntities)
		{
			const auto& registry = scene.GetRegistry();
			EntityHandle current = entity;
			while (current != entt::null && registry.valid(current) && registry.all_of<HierarchyComponent>(current)) {
				const EntityHandle parent = registry.get<HierarchyComponent>(current).Parent;
				if (parent == entt::null || !registry.valid(parent)) {
					return false;
				}

				if (selectedEntities.contains(static_cast<uint32_t>(parent))) {
					return true;
				}

				current = parent;
			}

			return false;
		}

		std::vector<EntityHandle> FilterSelectedHierarchyRoots(const Scene& scene, const std::vector<EntityHandle>& selectedEntities)
		{
			std::unordered_set<uint32_t> selectedSet;
			selectedSet.reserve(selectedEntities.size());
			for (EntityHandle entity : selectedEntities) {
				if (entity != entt::null && scene.IsValid(entity)) {
					selectedSet.insert(static_cast<uint32_t>(entity));
				}
			}

			std::vector<EntityHandle> roots;
			roots.reserve(selectedEntities.size());
			for (EntityHandle entity : selectedEntities) {
				if (entity == entt::null || !scene.IsValid(entity)) {
					continue;
				}

				if (!HasSelectedAncestor(scene, entity, selectedSet)) {
					roots.push_back(entity);
				}
			}

			return roots;
		}

		std::string BuildScriptMenuLabel(const EditorScriptDiscovery::ScriptEntry& scriptEntry)
		{
			return scriptEntry.ClassName + "  " + scriptEntry.Extension;
		}

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

		bool IsLeftMouseDragPastClickThreshold()
		{
			const ImGuiIO& io = ImGui::GetIO();
			const ImVec2 delta(io.MousePos.x - io.MouseClickedPos[ImGuiMouseButton_Left].x,
				io.MousePos.y - io.MouseClickedPos[ImGuiMouseButton_Left].y);
			return (delta.x * delta.x + delta.y * delta.y) > (io.MouseDragThreshold * io.MouseDragThreshold);
		}

		struct ComponentClipboardPayload {
			std::string SerializedName;
			Json::Value Data;
		};

		bool TryReadComponentClipboard(const std::string& clipboardJson, ComponentClipboardPayload& outPayload)
		{
			if (clipboardJson.empty()) {
				return false;
			}

			Json::Value root;
			std::string parseError;
			if (!Json::TryParse(clipboardJson, root, &parseError) || !root.IsObject()) {
				return false;
			}

			const Json::Value* componentValue = root.FindMember("component");
			const Json::Value* dataValue = root.FindMember("data");
			if (!componentValue || !componentValue->IsString() || !dataValue || dataValue->IsNull()) {
				return false;
			}

			outPayload.SerializedName = componentValue->AsStringOr();
			outPayload.Data = *dataValue;
			return !outPayload.SerializedName.empty();
		}

		const ComponentInfo* FindComponentInfoBySerializedName(const ComponentRegistry& registry, const std::string& serializedName)
		{
			const ComponentInfo* found = nullptr;
			registry.ForEachComponentInfo([&](const std::type_index&, const ComponentInfo& info) {
				if (!found && info.category == ComponentCategory::Component && info.serializedName == serializedName) {
					found = &info;
				}
			});
			return found;
		}

		float DivideOrKeep(float numerator, float denominator, float fallback)
		{
			constexpr float k_MinScaleAxis = 0.0001f;
			return std::abs(denominator) > k_MinScaleAxis ? numerator / denominator : fallback;
		}

		void SetLocalTransformFromWorld(
			Transform2DComponent& child,
			const Transform2DComponent* parent,
			const Vec2& worldPosition,
			float worldRotation,
			const Vec2& worldScale)
		{
			if (parent) {
				Vec2 parentRelative = worldPosition - parent->Position;
				parentRelative = Rotate(parentRelative, -parent->Rotation);
				child.LocalPosition = {
					DivideOrKeep(parentRelative.x, parent->Scale.x, child.LocalPosition.x),
					DivideOrKeep(parentRelative.y, parent->Scale.y, child.LocalPosition.y)
				};
				child.LocalRotation = worldRotation - parent->Rotation;
				child.LocalScale = {
					DivideOrKeep(worldScale.x, parent->Scale.x, child.LocalScale.x),
					DivideOrKeep(worldScale.y, parent->Scale.y, child.LocalScale.y)
				};
			}
			else {
				child.LocalPosition = worldPosition;
				child.LocalRotation = worldRotation;
				child.LocalScale = worldScale;
			}

			child.Position = worldPosition;
			child.Rotation = worldRotation;
			child.Scale = worldScale;
			child.MarkDirty();
		}
	}



	// ──────────────────────────────────────────────
	//  Lifecycle
	// ──────────────────────────────────────────────

	void ImGuiEditorLayer::OnAttach(Application& app) {
		Application::SetIsPlaying(false);
		ApplicationEditorAccess::SetGameInputEnabled(false);
		Gizmo::SetShowInRuntime(false);
		if (app.GetRenderer2D()) {
			app.GetRenderer2D()->SetSkipBeginFrameRender(true);
		}
		// UI must render *into* the per-panel FBO (Editor / Game View),
		// not the OS window backbuffer that ImGui will paint over. Skip
		// the auto BeginFrame render and drive RenderScene manually from
		// RenderSceneIntoFBO while the panel's FBO is bound.
		if (app.GetGuiRenderer()) {
			app.GetGuiRenderer()->SetSkipBeginFrameRender(true);
		}

		if (m_LogSubscriptionId.value != 0) {
			Log::OnLog.Remove(m_LogSubscriptionId);
		}

		// Profiler panel — initialize once when the editor layer attaches.
		// Lazy first-render still loads project settings, so doing this
		// before a project is loaded is fine.
		m_ProfilerPanel.Initialize();

		ClearLogEntries();
		m_LogDispatchState = std::make_shared<LogDispatchState>();
		std::weak_ptr<LogDispatchState> weakLogDispatchState = m_LogDispatchState;
		m_LogSubscriptionId = Log::OnLog.Add([weakLogDispatchState](const Log::Entry& entry) {
			// Only show user-facing logs in editor panel (Client + EditorConsole)
			// Core/engine logs still go to stdout but not the editor UI
			if (entry.Source == Log::Type::Core) return;

			std::shared_ptr<LogDispatchState> state = weakLogDispatchState.lock();
			if (!state) return;

			// M31: never block worker threads on the editor's log mutex.
			// `try_lock` first; if the main thread is mid-drain, stash the
			// entry in a per-thread buffer and drain it on the next
			// successful lock from the same thread. This means an entry
			// from a thread that never logs again is lost — acceptable for
			// debug log dispatch (the engine logger still wrote it to
			// stdout / spdlog upstream of OnLog).
			thread_local std::vector<LogEntry> tlBuf;

			if (state->Mutex.try_lock()) {
				std::lock_guard<std::mutex> guard(state->Mutex, std::adopt_lock);
				if (!tlBuf.empty()) {
					state->PendingEntries.insert(state->PendingEntries.end(),
						std::make_move_iterator(tlBuf.begin()),
						std::make_move_iterator(tlBuf.end()));
					tlBuf.clear();
				}
				state->PendingEntries.push_back({ entry.Message, entry.Level });
			}
			else {
				tlBuf.push_back({ entry.Message, entry.Level });
			}
			});
	}

	void ImGuiEditorLayer::OnDetach(Application& app) {
		(void)app;
		m_ProfilerPanel.Shutdown();
		ApplicationEditorAccess::SetGameInputEnabled(true);
		Gizmo::SetShowInRuntime(true);
		if (m_LogSubscriptionId.value != 0) {
			Log::OnLog.Remove(m_LogSubscriptionId);
			m_LogSubscriptionId = EventId{};
		}
		m_LogDispatchState.reset();
		// Reset the game-view log overlay BEFORE the rest of teardown so its destructor
		// (which unsubscribes from Log::OnLog) runs while the editor layer is still in
		// a sane state. The Event<>::Remove now blocks until any in-flight Log::Invoke
		// finishes, but resetting early keeps the unsubscribe close to the subscribe
		// site for clarity.
		m_GameViewLogOverlay.reset();
		ClearPreviewTextureCache();

		// Reverse construction order: panels → icons → FBOs.
		m_AssetBrowser.Shutdown();
		m_PackageManagerPanel.Shutdown();
		m_PackageManager.Shutdown();
		// M29: join in-flight ExternalEditor launcher threads so they
		// don't outlive editor shutdown. May briefly block on the
		// Sleep(4000) cold-start VS path — see ExternalEditor.cpp.
		ExternalEditor::JoinPendingLaunchThreads();
		// Picker statics (TU-locals) survive Application::Reload otherwise —
		// reset them here so the next attach re-runs the built-ins scan
		// against whatever AssetRegistry the new project provides.
		// H21/H22: ReferencePicker. M28: ScriptComponentInspector script picker.
		ReferencePicker::Shutdown();
		ScriptComponentInspector::Shutdown();
		EditorIcons::Shutdown();
		// Framebuffers are RAII-managed; explicit destruction here mirrors
		// the historical OnDetach order (drop GPU resources before any
		// later teardown that might re-enter the renderer).
		m_EditorViewFBO.Destroy();
		m_GameViewFBO.Destroy();
	}

	void ImGuiEditorLayer::OnUpdate(Application& app, float dt) {
		DrainPendingLogEntries();
		RunAutoSaveTick(app, dt);

		// Drain a script-driven Application.Quit() that fired this frame.
		// In editor it's wired to set m_EditorStopPlayRequested instead
		// of actually quitting; consume the flag and route through the
		// same audio-cleanup + RestoreEditorSceneAfterPlaymode path the
		// Stop button uses, so script and click produce identical state.
		if (Application::GetIsPlaying() && ApplicationEditorAccess::ConsumeQuitStopPlayRequest()) {
			Scene* act = SceneManager::Get().GetActiveScene();
			if (act) {
				for (auto [ent, audio] : act->GetRegistry().view<AudioSourceComponent>().each()) {
					if (audio.IsPlaying() || audio.IsPaused()) audio.Stop();
				}
			}
			RestoreEditorSceneAfterPlaymode();
		}

		Input& input = app.GetInput();
		Scene* activeScene = app.GetSceneManager() ? app.GetSceneManager()->GetActiveScene() : nullptr;
		if (!activeScene) {
			return;
		}

		Scene& scene = *activeScene;

		// Per-frame stale-handle sweep: the inspector / hierarchy hold raw entt
		// handles across frames. EnTT's version field protects most reads (handle
		// recycling bumps the version, IsValid catches it), but a leftover stale
		// selection still confuses anything that doesn't IsValid-gate. Clear here
		// rather than peppering every consumer with checks.
		//
		// CRITICAL: when in prefab-edit mode the selection lives in the
		// detached prefab scene's registry, NOT the active scene's. Validating
		// against the active scene would IsValid()-fail every frame and clear
		// the selection — which is exactly what made prefab children
		// "unselectable" (clicking selected the entity, the next frame's sweep
		// reset it). Route through GetContextScene so the sweep matches the
		// scene the selection is *actually* drawn from.
		Scene* selectionContextScene = GetContextScene();
		if (!selectionContextScene) selectionContextScene = activeScene;
		Scene& selectionScene = *selectionContextScene;

		if (m_SelectedEntity != entt::null && !selectionScene.IsValid(m_SelectedEntity)) {
			m_SelectedEntity = entt::null;
		}
		if (m_PressedEntity != entt::null && !selectionScene.IsValid(m_PressedEntity)) {
			m_PressedEntity = entt::null;
		}
		if (!m_SelectedEntities.empty()) {
			auto isStale = [&selectionScene](EntityHandle h) {
				return h == entt::null || !selectionScene.IsValid(h);
			};
			m_SelectedEntities.erase(
				std::remove_if(m_SelectedEntities.begin(), m_SelectedEntities.end(), isStale),
				m_SelectedEntities.end());
		}
		const bool hasShortcutFocus = HasEntityShortcutFocus();
		// Shortcut handlers (focus / duplicate / rename / etc.) act on the
		// scene the user is editing, which is the prefab edit scene in prefab
		// mode. Hand them the same context-aware reference the sweep used so
		// F-to-focus / Ctrl+D / Delete / F2 work on prefab entities, not just
		// the (hidden) active scene.
		Scene& shortcutScene = selectionScene;
		const bool hasEntitySelection = !GetSelectedEntities(shortcutScene).empty();

		if (input.GetKeyDown(KeyCode::F) && hasShortcutFocus && hasEntitySelection) {
			FocusSelectedEntity(shortcutScene);
		}

		if (!hasShortcutFocus) {
			return;
		}
		// Hierarchy edit shortcuts (Ctrl+C/V/D, Delete, F2) stay live in
		// play mode — RestoreEditorSceneAfterPlaymode reloads the scene
		// from disk on stop, so any duplicate / paste / delete / rename
		// during play is purely transient and matches the user's mental
		// model of iteration. Save (Ctrl+S) is gated separately on the
		// menu, so it can't accidentally persist play-mode state.

		if (m_RenamingEntity != entt::null || ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) {
			return;
		}

		// Use ImGui's KeyCtrl + IsKeyPressed instead of the raw Input wrapper —
		// the engine's KeyCode::LeftControl misses RIGHT Ctrl (and Cmd on macOS),
		// which is why the prior code silently failed for users on RHS keyboards.
		// IsKeyPressed(_, repeat=false) also prevents auto-repeat from firing
		// the action multiple times when the key is held. Same pattern as the
		// Save shortcut wired up in ImGuiEditorLayerChrome.cpp.
		const ImGuiIO& io = ImGui::GetIO();
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && hasEntitySelection) {
			CopySelectedEntities(shortcutScene);
		}
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
			PasteEntities(shortcutScene);
		}
		if (!hasEntitySelection) {
			return;
		}
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
			DuplicateSelectedEntity(shortcutScene);
		}
		if (input.GetKeyDown(KeyCode::Delete) || input.GetKeyDown(KeyCode::KpDecimal)) {
			DeleteSelectedEntity(shortcutScene);
		}
		if (input.GetKeyDown(KeyCode::F2)) {
			BeginRenameSelectedEntity(shortcutScene);
		}
	}

	void ImGuiEditorLayer::DrainPendingLogEntries() {
		if (!m_LogDispatchState) {
			return;
		}

		std::vector<LogEntry> pendingEntries;
		{
			std::scoped_lock lock(m_LogDispatchState->Mutex);
			if (m_LogDispatchState->PendingEntries.empty()) {
				return;
			}

			pendingEntries.swap(m_LogDispatchState->PendingEntries);
		}

		for (LogEntry& entry : pendingEntries) {
			AppendLogEntry(std::move(entry));
		}
	}

	void ImGuiEditorLayer::AppendLogEntry(LogEntry entry) {
		m_LogEntries.push_back(std::move(entry));
		if (m_LogEntries.size() > 2000) {
			m_LogEntries.erase(m_LogEntries.begin(), m_LogEntries.begin() + 500);
		}
	}

	void ImGuiEditorLayer::RunAutoSaveTick(Application& app, float dt) {
		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (!project || !project->AutoSaveScenes) {
			m_AutoSaveAccumulator = 0.0f;
			return;
		}

		// Skip Play mode entirely — entering Play already saves the scene
		// to disk, and Play-mode mutations are deliberately discarded on
		// Stop. Auto-saving over the pre-Play snapshot would persist
		// transient state and surprise the user on Stop.
		if (Application::GetIsPlaying()) {
			m_AutoSaveAccumulator = 0.0f;
			return;
		}

		// Reset the timer when the active scene changes — the new scene
		// hasn't been edited yet, and counting against its lifetime
		// from the moment it loaded would auto-save it the instant the
		// user makes any change instead of after the configured interval.
		Scene* active = app.GetSceneManager() ? app.GetSceneManager()->GetActiveScene() : nullptr;
		if (!active) {
			m_AutoSaveAccumulator = 0.0f;
			m_AutoSaveLastScenePath.clear();
			return;
		}
		const std::string scenePath = project->GetSceneFilePath(active->GetName());
		if (scenePath != m_AutoSaveLastScenePath) {
			m_AutoSaveAccumulator = 0.0f;
			m_AutoSaveLastScenePath = scenePath;
		}

		if (!active->IsDirty()) {
			// Nothing to save; let the accumulator drain back to zero so
			// the next dirty edit gets the full interval before its first
			// auto-save fires.
			m_AutoSaveAccumulator = 0.0f;
			return;
		}

		m_AutoSaveAccumulator += dt;
		const float interval = std::max(5.0f, project->AutoSaveIntervalSeconds);
		if (m_AutoSaveAccumulator < interval) {
			return;
		}
		m_AutoSaveAccumulator = 0.0f;

		if (scenePath.empty()) {
			return;
		}
		if (SceneSerializer::SaveToFile(*active, scenePath)) {
			active->ClearDirty();
			AIM_INFO_TAG("Editor", "Auto-saved scene '{}'.", active->GetName());
		}
	}

	void ImGuiEditorLayer::ClearLogEntries() {
		m_LogEntries.clear();
		if (m_LogDispatchState) {
			std::scoped_lock lock(m_LogDispatchState->Mutex);
			m_LogDispatchState->PendingEntries.clear();
		}
	}

	// ──────────────────────────────────────────────
	//  Dockspace & Menu
	// ──────────────────────────────────────────────

	void ImGuiEditorLayer::RenderDockspaceRoot() {
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->Pos);
		ImGui::SetNextWindowSize(viewport->Size);
		ImGui::SetNextWindowViewport(viewport->ID);

		ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

		ImGui::Begin("EditorDockspace", nullptr, flags);
		ImGui::PopStyleVar(3);

		ImGuiID dockspaceId = ImGui::GetID("AxiomEditorDockspace");
		ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
		// End() is in OnPreRender after RenderMainMenu — must stay open for the menubar.
	}

	void ImGuiEditorLayer::RenderMainMenu(Scene& scene) {
		if (!ImGui::BeginMenuBar()) {
			return;
		}


		if (ImGui::BeginMenu("Application")) {
			if (ImGui::MenuItem("Reload App")) {
				Application::Reload();
			}
			if (ImGui::MenuItem("Reset Layout")) {
				ImGuiContextLayer::ResetLayoutToBundledDefault();
			}
			if (ImGui::MenuItem("Quit")) {
				Application::RequestQuit();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !Application::GetIsPlaying())) {
				AxiomProject* project = ProjectManager::GetCurrentProject();
				if (project) {
					std::string scenePath = project->GetSceneFilePath(scene.GetName());
					SceneSerializer::SaveToFile(scene, scenePath);
					project->LastOpenedScene = scene.GetName();
					project->Save();
				}
			}
			ImGui::Separator();

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Project")) {
			bool hasProject = ProjectManager::HasProject();
			if (ImGui::MenuItem("Build", nullptr, false, hasProject && !Application::GetIsPlaying())) {
				m_ShowBuildPanel = true;
				if (m_BuildOutputDir.empty()) {
					AxiomProject* project = ProjectManager::GetCurrentProject();
					if (project) {
						m_BuildOutputDir = Path::Combine(project->RootDirectory, "Builds", "Windows");
						std::snprintf(m_BuildOutputDirBuffer, sizeof(m_BuildOutputDirBuffer), "%s", m_BuildOutputDir.c_str());
					}
				}
			}
			// Project Settings ⇒ editor-side preferences (Asset Browser
			// pane, Auto-Save cadence, etc.) that govern how the editor
			// behaves in this project, NOT what gets shipped in the
			// build. Runtime / shipped-game settings (resolution, splash,
			// icon, cursors, …) live under Player Settings just below
			// so the two intents don't cross-contaminate one panel.
			if (ImGui::MenuItem("Project Settings", nullptr, false, hasProject)) {
				m_ShowProjectSettings = true;
			}
			if (ImGui::MenuItem("Player Settings", nullptr, false, hasProject)) {
				m_ShowPlayerSettings = true;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Package Manager", nullptr, false, hasProject)) {
				m_ShowPackageManager = true;
			}

			ExternalEditor::DetectEditors();
			const auto& editors = ExternalEditor::GetAvailableEditors();
			if (!editors.empty() && ImGui::BeginMenu("Script Editor")) {
				for (int i = 0; i < static_cast<int>(editors.size()); i++) {
					bool selected = (ExternalEditor::GetSelectedIndex() == i);
					const std::string editorId = std::to_string(i);
					if (ImGuiUtils::MenuItemEllipsis(editors[i].DisplayName, editorId.c_str(), nullptr, selected, true, 220.0f)) {
						ExternalEditor::SetSelectedIndex(i);
					}
				}
				ImGui::EndMenu();
			}

			ImGui::EndMenu();
		}

		// Tools — runtime/diagnostic utilities. Profiler panel is the
		// first inhabitant; later additions (memory tracker, log filter,
		// etc.) belong here too.
		if (ImGui::BeginMenu("Tools")) {
			ImGui::MenuItem("Profiler", "Ctrl+F6", &m_ShowProfiler);
			ImGui::EndMenu();
		}

		ImGui::EndMenuBar();
	}

	static bool IconButton(const char* id, const char* iconName, float iconSize, const ImVec2& btnSize) {
		unsigned int tex = EditorIcons::Get(iconName, (int)iconSize);
		if (tex)
			return ImGui::ImageButton(id, (ImTextureID)(intptr_t)tex, btnSize, ImVec2(0, 1), ImVec2(1, 0));
		return ImGui::Button(id + 2);
	}

	void ImGuiEditorLayer::BeginPlayModeRequest(Scene& scene) {
		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (project) {
			std::string scenePath = project->GetSceneFilePath(scene.GetName());
			if (scene.IsDirty()) {
				SceneSerializer::SaveToFile(scene, scenePath);
				project->LastOpenedScene = scene.GetName();
				project->Save();
			}
			m_PlayModeScenePath = scenePath;
		}

		m_LogEntries.clear();
		ApplicationEditorAccess::SetPlaymodePaused(false);

		ScriptSystem* scriptSys = scene.HasSystem<ScriptSystem>() ? scene.GetSystem<ScriptSystem>() : nullptr;
		if (scriptSys && scriptSys->IsRebuilding()) {
			m_PlayModeRecompilePending = true;
			return;
		}
		if (scriptSys && !scriptSys->DidLastRebuildSucceed()) {
			AIM_ERROR_TAG("PlayMode", "Script compilation failed. Play Mode was not started.");
			return;
		}

		CompletePlayModeEntry(scene);
	}

	void ImGuiEditorLayer::CompletePlayModeEntry(Scene& scene) {
		m_PlayModeRecompilePending = false;
		ApplicationEditorAccess::SetPlaymodePaused(false);
		Application::SetIsPlaying(true);
		// Reset Time.TimeSinceStartup / Time.RealtimeSinceStartup so each
		// play session starts at t=0 from a script's perspective.
		ApplicationEditorAccess::MarkGameStart();
		// Reset Box2D sleep timers so bodies that sat through editor
		// idle don't immediately freeze on the first physics step.
		if (PhysicsSystem2D::IsInitialized()) {
			PhysicsSystem2D::WakeAllBodies();
		}
		StartPlayOnAwakeComponents(scene);
	}

	void ImGuiEditorLayer::PollPendingPlayModeRequest(Scene& scene) {
		if (!m_PlayModeRecompilePending || Application::GetIsPlaying()) {
			return;
		}

		ScriptSystem* scriptSys = scene.HasSystem<ScriptSystem>() ? scene.GetSystem<ScriptSystem>() : nullptr;
		if (!scriptSys || !scriptSys->IsRebuilding()) {
			if (!scriptSys || scriptSys->DidLastRebuildSucceed()) {
				CompletePlayModeEntry(scene);
			}
			else {
				m_PlayModeRecompilePending = false;
				AIM_ERROR_TAG("PlayMode", "Script compilation failed. Play Mode was not started.");
			}
		}
	}

	void ImGuiEditorLayer::RenderToolbar() {
		ImGui::Begin("Toolbar");

		const float iconSize = ImGui::GetTextLineHeight();
		const ImVec2 btnSize(iconSize, iconSize);
		const bool isPlaying = Application::GetIsPlaying();
		const bool isPaused = ApplicationEditorAccess::IsPlaymodePaused();

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));

		if (!isPlaying) {
			// Play mode plays the active scene. Entering it while the editor
			// is showing a detached prefab edit scene leaves the user with a
			// disorienting view (prefab still showing, but game logic running
			// against a scene they can't see). Refuse the action and surface
			// the reason via tooltip — the user must commit / discard prefab
			// edits first.
			const bool blockedByPrefabEdit = IsInPrefabEditMode();
			const bool playButtonDisabled = m_PlayModeRecompilePending || blockedByPrefabEdit;
			if (playButtonDisabled) {
				ImGui::BeginDisabled();
			}
			if (IconButton("##Play", "play", iconSize, btnSize)) {
				Scene* active = SceneManager::Get().GetActiveScene();
				if (active) {
					BeginPlayModeRequest(*active);
				}
			}
			if (playButtonDisabled) {
				ImGui::EndDisabled();
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				if (blockedByPrefabEdit) {
					ImGui::SetTooltip("Exit prefab edit mode before entering play.");
				}
				else {
					ImGui::SetTooltip("Play (Enter playmode)");
				}
			}
		}
		else {
			if (isPaused) {
				if (IconButton("##Continue", "play", iconSize, btnSize)) {
					ApplicationEditorAccess::SetPlaymodePaused(false);
					Scene* active = SceneManager::Get().GetActiveScene();
					if (active) {
						for (auto ent : m_EditorPausedAudioEntities) {
							if (active->IsValid(ent) && active->HasComponent<AudioSourceComponent>(ent)) {
								auto& audio = active->GetComponent<AudioSourceComponent>(ent);
								if (audio.IsPaused()) audio.Resume();
							}
						}
					}
					m_EditorPausedAudioEntities.clear();
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Continue");
			}
			else {
				if (IconButton("##Pause", "pause", iconSize, btnSize)) {
					ApplicationEditorAccess::SetPlaymodePaused(true);
					m_EditorPausedAudioEntities.clear();
					Scene* active = SceneManager::Get().GetActiveScene();
					if (active) {
						for (auto [ent, audio] : active->GetRegistry().view<AudioSourceComponent>().each()) {
							if (audio.IsPlaying()) {
								audio.Pause();
								m_EditorPausedAudioEntities.push_back(ent);
							}
						}
					}
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause");
			}

			ImGui::SameLine();
			if (IconButton("##Step", "step_forward", iconSize, btnSize)) {
				ApplicationEditorAccess::SetPlaymodePaused(false);
				m_StepFrames = 2;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step (advance one frame)");

			ImGui::SameLine();
			if (IconButton("##Stop", "stop", iconSize, btnSize)) {
				{
					Scene* act = SceneManager::Get().GetActiveScene();
					if (act) {
						for (auto [ent, audio] : act->GetRegistry().view<AudioSourceComponent>().each()) {
							if (audio.IsPlaying() || audio.IsPaused()) audio.Stop();
						}
					}
				}

				RestoreEditorSceneAfterPlaymode();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop (exit playmode)");
		}

		ImGui::PopStyleVar();
		ImGui::SameLine();
		const char* statusText = !isPlaying ? "Editor" : (isPaused ? "Paused" : "Playing");
		ImGui::TextUnformatted(statusText);
		if (m_PlayModeRecompilePending) {
			ImGui::SameLine();
			ImGui::TextDisabled("Waiting for script compilation...");
		}

		// (Editor View draw-mode selector lives in RenderEditorView now,
		// alongside the viewport's per-window options — same UX as Game
		// View's aspect / VSync row. Removing it from the toolbar gives
		// the play / pause / step / stop controls breathing room.)

		ImGui::End();

		// Handle step-frame logic: pause again after stepping one frame
		if (isPlaying && m_StepFrames > 0) {
			m_StepFrames--;
			if (m_StepFrames == 0) {
				ApplicationEditorAccess::SetPlaymodePaused(true);
				// SetPlaymodePaused only suspends gameplay update; audio runs on
				// miniaudio's own thread, so we mirror the Pause button's manual
				// capture. List is appended (not cleared) so any entries from a
				// prior explicit Pause survive to be resumed by Continue.
				Scene* active = SceneManager::Get().GetActiveScene();
				if (active) {
					for (auto [ent, audio] : active->GetRegistry().view<AudioSourceComponent>().each()) {
						if (audio.IsPlaying()) {
							audio.Pause();
							m_EditorPausedAudioEntities.push_back(ent);
						}
					}
				}
			}
		}
	}

	void ImGuiEditorLayer::RestoreEditorSceneAfterPlaymode() {
		// Reset pressed-state before scene reload: m_PressedEntity references an
		// EntityHandle from the play-mode scene that is about to be destroyed and
		// repopulated. Without this, the next hierarchy click could re-use the same
		// entt id and dispatch a stale click against an unrelated restored entity.
		m_PressedEntity = entt::null;

		uint64_t selectedUUID = 0;
		Scene* active = SceneManager::Get().GetActiveScene();
		if (active && m_SelectedEntity != entt::null && active->IsValid(m_SelectedEntity)
			&& active->HasComponent<UUIDComponent>(m_SelectedEntity)) {
			selectedUUID = static_cast<uint64_t>(active->GetComponent<UUIDComponent>(m_SelectedEntity).Id);
		}

		m_SelectedEntity = entt::null;
		m_IsSceneNodeSelected = false;
		m_RenamingEntity = entt::null;
		m_EntityOrder.clear(); m_EntityOrderDirty = true;
		m_PlayModeRecompilePending = false;

		ApplicationEditorAccess::SetPlaymodePaused(false);
		Application::SetIsPlaying(false);

		if (!m_PlayModeScenePath.empty()) {
			active = SceneManager::Get().GetActiveScene();
			if (active) {
				SceneSerializer::LoadFromFile(*active, m_PlayModeScenePath);
			}
			m_PlayModeScenePath.clear();
		}

		if (selectedUUID == 0) {
			return;
		}

		active = SceneManager::Get().GetActiveScene();
		if (!active) {
			return;
		}

		auto view = active->GetRegistry().view<UUIDComponent>();
		for (auto [ent, uuid] : view.each()) {
			if (static_cast<uint64_t>(uuid.Id) == selectedUUID) {
				SelectEntity(ent);
				break;
			}
		}
	}

	void ImGuiEditorLayer::SelectSceneNode() {
		m_SelectedEntity = entt::null;
		m_PressedEntity = entt::null;
		m_SelectedEntities.clear();
		m_LastEntitySelectionIndex = -1;
		m_RenamingEntity = entt::null;
		m_EntityRenameFrameCounter = 0;
		m_IsSceneNodeSelected = true;
		m_AssetBrowser.ClearSelection();
	}

	void ImGuiEditorLayer::SelectEntity(EntityHandle entity) {
		m_SelectedEntity = entity;
		m_SelectedEntities.clear();
		if (entity != entt::null) {
			m_SelectedEntities.push_back(entity);
		}
		m_LastEntitySelectionIndex = -1;
		m_IsSceneNodeSelected = false;
		if (entity != entt::null) {
			m_AssetBrowser.ClearSelection();
		}
	}

	void ImGuiEditorLayer::ClearEntitySelection() {
		// Pressed-state must clear together with selection — m_PressedEntity is a
		// transient mouse-down cursor and surviving a selection change would dispatch
		// a click against an entity that is no longer the user's intended target.
		m_SelectedEntity = entt::null;
		m_PressedEntity = entt::null;
		m_SelectedEntities.clear();
		m_LastEntitySelectionIndex = -1;
		m_RenamingEntity = entt::null;
		m_EntityRenameFrameCounter = 0;
		m_IsSceneNodeSelected = false;
	}

	bool ImGuiEditorLayer::IsEntitySelected(EntityHandle entity) const {
		return std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), entity) != m_SelectedEntities.end();
	}

	std::vector<EntityHandle> ImGuiEditorLayer::GetSelectedEntities(const Scene& scene) const {
		std::vector<EntityHandle> entities;
		for (EntityHandle entity : m_SelectedEntities) {
			if (entity != entt::null && scene.IsValid(entity)
				&& std::find(entities.begin(), entities.end(), entity) == entities.end()) {
				entities.push_back(entity);
			}
		}

		if (entities.empty() && m_SelectedEntity != entt::null && scene.IsValid(m_SelectedEntity)) {
			entities.push_back(m_SelectedEntity);
		}

		return entities;
	}

	void ImGuiEditorLayer::SetSingleEntitySelection(EntityHandle entity, int index) {
		m_SelectedEntity = entity;
		m_SelectedEntities.clear();
		if (entity != entt::null) {
			m_SelectedEntities.push_back(entity);
			m_AssetBrowser.ClearSelection();
		}
		m_LastEntitySelectionIndex = index;
		m_IsSceneNodeSelected = false;
	}

	void ImGuiEditorLayer::ToggleEntitySelection(EntityHandle entity, int index) {
		auto it = std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), entity);
		if (it != m_SelectedEntities.end()) {
			m_SelectedEntities.erase(it);
			if (m_SelectedEntity == entity) {
				m_SelectedEntity = m_SelectedEntities.empty() ? entt::null : m_SelectedEntities.back();
			}
		}
		else if (entity != entt::null) {
			m_SelectedEntities.push_back(entity);
			m_SelectedEntity = entity;
			m_AssetBrowser.ClearSelection();
		}

		m_LastEntitySelectionIndex = index;
		m_IsSceneNodeSelected = false;
	}

	void ImGuiEditorLayer::SelectEntityRange(int index) {
		if (m_EntityOrder.empty()) {
			return;
		}

		if (index < 0 || index >= static_cast<int>(m_EntityOrder.size())) {
			return;
		}

		if (m_LastEntitySelectionIndex < 0 || m_LastEntitySelectionIndex >= static_cast<int>(m_EntityOrder.size())) {
			SetSingleEntitySelection(m_EntityOrder[static_cast<std::size_t>(index)], index);
			return;
		}

		const int first = std::min(m_LastEntitySelectionIndex, index);
		const int last = std::max(m_LastEntitySelectionIndex, index);
		m_SelectedEntities.clear();
		for (int i = first; i <= last; ++i) {
			m_SelectedEntities.push_back(m_EntityOrder[static_cast<std::size_t>(i)]);
		}
		m_SelectedEntity = m_EntityOrder[static_cast<std::size_t>(index)];
		m_IsSceneNodeSelected = false;
		m_AssetBrowser.ClearSelection();
	}

	void ImGuiEditorLayer::FocusSelectedEntity(Scene& scene) {
		if (m_SelectedEntity == entt::null || !scene.IsValid(m_SelectedEntity)) {
			return;
		}

		Transform2DComponent* transform = nullptr;
		if (scene.TryGetComponent<Transform2DComponent>(m_SelectedEntity, transform) && transform) {
			m_EditorCamera.SetPosition(transform->Position);
			return;
		}

		RectTransform2DComponent* rectTransform = nullptr;
		if (scene.TryGetComponent<RectTransform2DComponent>(m_SelectedEntity, rectTransform) && rectTransform) {
			m_EditorCamera.SetPosition(rectTransform->GetCenter());
		}
	}

	void ImGuiEditorLayer::DuplicateSelectedEntity(Scene& scene) {
		const std::vector<EntityHandle> selectedEntities = FilterSelectedHierarchyRoots(scene, GetSelectedEntities(scene));
		if (selectedEntities.empty()) {
			return;
		}

		std::vector<EntityHandle> createdEntities;
		for (EntityHandle sourceEntity : selectedEntities) {
			Json::Value entityValue = SceneSerializer::SerializeEntityForClipboard(scene, sourceEntity);
			if (!entityValue.IsObject()) {
				continue;
			}

			EntityHandle clone = SceneSerializer::DeserializeEntityFromValue(scene, entityValue);
			if (clone != entt::null) {
				Entity source = scene.GetEntity(sourceEntity);
				Entity sourceParent = source.GetParent();
				if (sourceParent.IsValid()) {
					Entity cloneEntity = scene.GetEntity(clone);
					cloneEntity.SetParent(sourceParent);
					MoveSiblingNextTo(scene, clone, sourceEntity, true);
					m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(sourceParent.GetHandle()));
				}
				createdEntities.push_back(clone);
			}
		}

		if (!createdEntities.empty()) {
			m_SelectedEntities = createdEntities;
			m_SelectedEntity = createdEntities.back();
			m_LastEntitySelectionIndex = -1;
			m_IsSceneNodeSelected = false;
			scene.MarkDirty();
			m_EntityOrder.clear(); m_EntityOrderDirty = true;
		}
	}

	void ImGuiEditorLayer::DeleteSelectedEntity(Scene& scene) {
		const std::vector<EntityHandle> selectedEntities = GetSelectedEntities(scene);
		if (selectedEntities.empty()) {
			return;
		}

		// Reset pressed-state up-front: m_PressedEntity is a hierarchy-panel cursor
		// from a prior mouse-down and must clear whenever the selection invariants
		// change, otherwise the next click on a re-used entt id would dispatch a
		// click event against a stale (destroyed) entity.
		m_PressedEntity = entt::null;

		for (EntityHandle entity : selectedEntities) {
			if (scene.IsValid(entity)) {
				scene.DestroyEntity(entity);
			}
		}
		ClearEntitySelection();
		m_EntityOrder.clear(); m_EntityOrderDirty = true;
	}

	void ImGuiEditorLayer::BeginRenameSelectedEntity(Scene& scene) {
		if (m_SelectedEntity == entt::null || !scene.IsValid(m_SelectedEntity)) {
			return;
		}

		Entity entity = scene.GetEntity(m_SelectedEntity);
		m_RenamingEntity = m_SelectedEntity;
		m_EntityRenameFrameCounter = 0;
		std::snprintf(m_EntityRenameBuffer, sizeof(m_EntityRenameBuffer), "%s", entity.GetName().c_str());
	}

	void ImGuiEditorLayer::UnpackSelectedPrefabs(Scene& scene) {
		const std::vector<EntityHandle> selectedEntities = GetSelectedEntities(scene);
		if (selectedEntities.empty()) {
			return;
		}

		// Reusing SetEntityMetaData(EntityOrigin::Scene) is the canonical
		// reverse of the create-prefab path: it clears PrefabGUID, drops the
		// PrefabInstanceComponent, allocates a fresh SceneGUID, and marks
		// the scene dirty when not playing — so unpack is the exact inverse
		// of "drag entity to Asset Browser".
		bool anyUnpacked = false;
		for (EntityHandle entity : selectedEntities) {
			if (!scene.IsValid(entity)) continue;
			if (!scene.HasComponent<PrefabInstanceComponent>(entity)) continue;
			scene.SetEntityMetaData(entity, EntityOrigin::Scene);
			anyUnpacked = true;
		}
		if (anyUnpacked) {
			scene.MarkDirty();
		}
	}

	void ImGuiEditorLayer::CopySelectedEntities(Scene& scene) {
		const std::vector<EntityHandle> selectedEntities = FilterSelectedHierarchyRoots(scene, GetSelectedEntities(scene));
		if (selectedEntities.empty()) {
			return;
		}

		Json::Value root = Json::Value::MakeObject();
		root.AddMember("type", Json::Value("EntityClipboard"));

		Json::Value entities = Json::Value::MakeArray();
		for (EntityHandle entity : selectedEntities) {
			Json::Value entityValue = SceneSerializer::SerializeEntityForClipboard(scene, entity);
			if (entityValue.IsObject()) {
				entities.Append(std::move(entityValue));
			}
		}

		if (entities.GetArray().empty()) {
			return;
		}

		root.AddMember("entities", std::move(entities));
		m_EntityClipboardJson = Json::Stringify(root, false);
	}

	void ImGuiEditorLayer::PasteEntities(Scene& scene) {
		if (m_EntityClipboardJson.empty()) {
			return;
		}

		Json::Value root;
		std::string parseError;
		if (!Json::TryParse(m_EntityClipboardJson, root, &parseError) || !root.IsObject()) {
			return;
		}

		const Json::Value* typeValue = root.FindMember("type");
		const Json::Value* entitiesValue = root.FindMember("entities");
		if (!typeValue || typeValue->AsStringOr() != "EntityClipboard" || !entitiesValue || !entitiesValue->IsArray()) {
			return;
		}

		EntityHandle pasteParent = (!m_IsSceneNodeSelected
			&& m_SelectedEntity != entt::null
			&& scene.IsValid(m_SelectedEntity))
			? m_SelectedEntity
			: entt::null;
		// Prefab edit mode: a paste with no selection target would
		// otherwise create a sibling-to-root entity that the prefab
		// serializer drops on save (it walks the root's subtree only).
		// Force-route the paste under the prefab root to keep the
		// pasted entities visible in the saved .prefab.
		if (pasteParent == entt::null
			&& IsInPrefabEditMode()
			&& m_PrefabEditScene
			&& m_PrefabEditRootEntity != entt::null
			&& m_PrefabEditScene->IsValid(m_PrefabEditRootEntity)
			&& &scene == m_PrefabEditScene.get())
		{
			pasteParent = m_PrefabEditRootEntity;
		}

		std::vector<EntityHandle> createdEntities;
		for (const Json::Value& entityValue : entitiesValue->GetArray()) {
			EntityHandle entity = SceneSerializer::DeserializeEntityFromValue(scene, entityValue);
			if (entity != entt::null) {
				if (pasteParent != entt::null) {
					scene.GetEntity(entity).SetParent(scene.GetEntity(pasteParent));
				}
				createdEntities.push_back(entity);
			}
		}

		if (!createdEntities.empty()) {
			m_SelectedEntities = createdEntities;
			m_SelectedEntity = createdEntities.back();
			m_LastEntitySelectionIndex = -1;
			m_IsSceneNodeSelected = false;
			if (pasteParent != entt::null) {
				m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(pasteParent));
			}
			scene.MarkDirty();
			m_EntityOrder.clear(); m_EntityOrderDirty = true;
		}
	}

	EntityHandle ImGuiEditorLayer::RenderCreateEntityMenu(Scene& scene, Entity parent) {
		EntityHandle createdHandle = entt::null;

		// Prefab edit mode constraint: every entity created inside a
		// prefab edit must live UNDER the prefab root (the prefab is a
		// single-rooted asset — siblings to the root would silently get
		// dropped on Save because SceneSerializer::SaveEntityToFile only
		// walks the root's subtree). Force-parent here so a "Create
		// Entity" via the right-click menu without a context entity
		// still ends up nested correctly. Auto-uses the root only for
		// "no explicit parent" calls; an explicit parent (e.g. via
		// "Create Child" on a deeper entity) wins.
		Entity effectiveParent = parent;
		if (!effectiveParent.IsValid()
			&& IsInPrefabEditMode()
			&& m_PrefabEditScene
			&& m_PrefabEditRootEntity != entt::null
			&& m_PrefabEditScene->IsValid(m_PrefabEditRootEntity)
			&& &scene == m_PrefabEditScene.get())
		{
			effectiveParent = m_PrefabEditScene->GetEntity(m_PrefabEditRootEntity);
		}

		auto finishCreated = [&](Entity created) {
			if (!created.IsValid()) {
				return;
			}

			if (effectiveParent.IsValid()) {
				created.SetParent(effectiveParent);
				m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(effectiveParent.GetHandle()));
			}

			createdHandle = created.GetHandle();
			SelectEntity(createdHandle);
		};

		if (ImGui::MenuItem("Create Entity"))
		{
			finishCreated(scene.CreateEntity("Entity " + std::to_string(EntityHelper::EntitiesCount())));
		}

		if (ImGui::BeginMenu("2D Entity"))
		{
			if (ImGui::BeginMenu("Sprite"))
			{
				if (ImGui::MenuItem("Square"))
				{
					Entity created = scene.CreateEntity("Square " + std::to_string(EntityHelper::EntitiesCount()));
					created.AddComponent<SpriteRendererComponent>();
					finishCreated(created);
				}
				if (ImGui::MenuItem("Circle"))
				{
					Entity created = scene.CreateEntity("Circle " + std::to_string(EntityHelper::EntitiesCount()));
					auto& sprite = created.AddComponent<SpriteRendererComponent>();
					sprite.TextureHandle = TextureManager::GetDefaultTexture(DefaultTexture::Circle);
					finishCreated(created);
				}
				if (ImGui::MenuItem("9Sliced"))
				{
					Entity created = scene.CreateEntity("9Sliced " + std::to_string(EntityHelper::EntitiesCount()));
					auto& sprite = created.AddComponent<SpriteRendererComponent>();
					sprite.TextureHandle = TextureManager::GetDefaultTexture(DefaultTexture::_9Sliced);
					finishCreated(created);
				}
				if (ImGui::MenuItem("Pixel"))
				{
					Entity created = scene.CreateEntity("Pixel " + std::to_string(EntityHelper::EntitiesCount()));
					auto& sprite = created.AddComponent<SpriteRendererComponent>();
					sprite.TextureHandle = TextureManager::GetDefaultTexture(DefaultTexture::Pixel);
					finishCreated(created);
				}
				if (ImGui::MenuItem("Logo"))
				{
					Entity created = scene.CreateEntity("Logo " + std::to_string(EntityHelper::EntitiesCount()));
					auto& sprite = created.AddComponent<SpriteRendererComponent>();
					sprite.TextureHandle = TextureManager::LoadTexture("Game/logo.png");
					finishCreated(created);
				}

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Physics")) {
				if (ImGui::MenuItem("Dynamic Body"))
				{
					Entity created = scene.CreateEntity("Dynamic Body " + std::to_string(EntityHelper::EntitiesCount()));
					created.AddComponent<SpriteRendererComponent>();
					created.AddComponent<Rigidbody2DComponent>();
					created.AddComponent<BoxCollider2DComponent>();
					finishCreated(created);
				}
				if (ImGui::MenuItem("Kinematic Body"))
				{
					Entity created = scene.CreateEntity("Kinematic Body " + std::to_string(EntityHelper::EntitiesCount()));
					created.AddComponent<SpriteRendererComponent>();
					created.AddComponent<Rigidbody2DComponent>().SetBodyType(BodyType::Kinematic);
					created.AddComponent<BoxCollider2DComponent>();
					finishCreated(created);
				}
				if (ImGui::MenuItem("Static Body"))
				{
					Entity created = scene.CreateEntity("Static Body " + std::to_string(EntityHelper::EntitiesCount()));
					created.AddComponent<SpriteRendererComponent>();
					created.AddComponent<Rigidbody2DComponent>().SetBodyType(BodyType::Static);
					created.AddComponent<BoxCollider2DComponent>();
					finishCreated(created);
				}

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Effects"))
			{
				if (ImGui::MenuItem("Particle System"))
				{
					Entity created = scene.CreateEntity("Particle System " + std::to_string(EntityHelper::EntitiesCount()));
					created.AddComponent<ParticleSystem2DComponent>();
					finishCreated(created);
				}
				ImGui::EndMenu();
			}

			// Tilemap 2D — surfaced only when the project has the
			// Axiom.Tilemap2D package enabled. The package owns the
			// Tilemap2DComponent definition (it isn't linked into the
			// editor binary), so we add the component via the
			// ComponentRegistry's display-name lookup → info.add() path
			// rather than referencing the type directly. Without the
			// package toggle, the component isn't registered and the
			// menu item is hidden so the user doesn't see a click-then-
			// nothing-happens UX.
			{
				AxiomProject* project = ProjectManager::GetCurrentProject();
				const bool tilemapPackageEnabled = project &&
					std::find(project->Packages.begin(), project->Packages.end(),
						std::string("Axiom.Tilemap2D")) != project->Packages.end();
				if (tilemapPackageEnabled) {
					const ComponentInfo* tilemapInfo = nullptr;
					SceneManager::Get().GetComponentRegistry().ForEachComponentInfo(
						[&](const std::type_index&, const ComponentInfo& info) {
							if (!tilemapInfo
								&& info.category == ComponentCategory::Component
								&& info.displayName == "Tilemap 2D"
								&& info.add)
							{
								tilemapInfo = &info;
							}
						});
					if (tilemapInfo && ImGui::MenuItem("Tilemap 2D")) {
						Entity created = scene.CreateEntity(
							"Tilemap 2D " + std::to_string(EntityHelper::EntitiesCount()));
						tilemapInfo->add(created);
						finishCreated(created);
					}
				}
			}

			ImGui::EndMenu();
		}

		if (ImGui::MenuItem("Camera"))
		{
			Entity created = scene.CreateEntity("Camera " + std::to_string(EntityHelper::EntitiesCount()));
			created.AddComponent<Camera2DComponent>();
			finishCreated(created);
		}

		if (ImGui::MenuItem("Audio Source"))
		{
			Entity created = scene.CreateEntity("Audio Source " + std::to_string(EntityHelper::EntitiesCount()));
			created.AddComponent<AudioSourceComponent>();
			finishCreated(created);
		}

		if (ImGui::BeginMenu("UI"))
		{
			if (ImGui::MenuItem("Panel")) {
				finishCreated(EntityHelper::CreateUIPanel(scene));
			}
			if (ImGui::MenuItem("Image")) {
				Entity created = EntityHelper::CreateImageEntity(scene);
				if (!created.HasComponent<NameComponent>()) {
					created.AddComponent<NameComponent>(NameComponent("Image"));
				}
				finishCreated(created);
			}
			if (ImGui::MenuItem("Text")) {
				// Route through CreateWith (raw CreateEntityHandle) so the
				// entity isn't seeded with a Transform2DComponent — UI text
				// belongs on a RectTransform2D, and Transform2D conflicts
				// with it (declared via DeclareConflict in BuiltInComponent
				// Registration). Mirrors how the other UI presets are built.
				Entity created = EntityHelper::CreateWith<RectTransform2DComponent, TextRendererComponent>(scene);
				created.GetComponent<TextRendererComponent>().HAlign = TextAlignment::Center;
				if (!created.HasComponent<NameComponent>()) {
					created.AddComponent<NameComponent>(NameComponent("Text"));
				}
				finishCreated(created);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Button")) {
				finishCreated(EntityHelper::CreateUIButton(scene));
			}
			if (ImGui::MenuItem("Slider")) {
				finishCreated(EntityHelper::CreateUISlider(scene));
			}
			if (ImGui::MenuItem("Progress Bar")) {
				finishCreated(EntityHelper::CreateUIProgressBar(scene));
			}
			if (ImGui::MenuItem("Circular Slider")) {
				finishCreated(EntityHelper::CreateUICircularSlider(scene));
			}
			if (ImGui::MenuItem("Scrollbar")) {
				finishCreated(EntityHelper::CreateUIScrollbar(scene));
			}
			if (ImGui::MenuItem("Scroll View")) {
				finishCreated(EntityHelper::CreateUIScrollRect(scene));
			}
			if (ImGui::MenuItem("Input Field")) {
				finishCreated(EntityHelper::CreateUIInputField(scene));
			}
			if (ImGui::MenuItem("Dropdown")) {
				finishCreated(EntityHelper::CreateUIDropdown(scene));
			}
			if (ImGui::MenuItem("Toggle")) {
				finishCreated(EntityHelper::CreateUIToggle(scene));
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Horizontal Layout")) {
				finishCreated(EntityHelper::CreateUIHorizontalLayout(scene));
			}
			if (ImGui::MenuItem("Vertical Layout")) {
				finishCreated(EntityHelper::CreateUIVerticalLayout(scene));
			}
			if (ImGui::MenuItem("Grid Layout")) {
				finishCreated(EntityHelper::CreateUIGridLayout(scene));
			}
			ImGui::EndMenu();
		}

		return createdHandle;
	}

	bool ImGuiEditorLayer::SetEntityParentPreservingWorld(Scene& scene, EntityHandle childHandle, Entity parent) {
		if (childHandle == entt::null || !scene.IsValid(childHandle)) {
			return false;
		}
		if (parent.IsValid() && parent.GetScene() != &scene) {
			return false;
		}

		Entity child = scene.GetEntity(childHandle);
		if (parent.IsValid() && (parent.GetHandle() == childHandle || child.IsAncestorOf(parent))) {
			return false;
		}
		if (child.GetParent() == parent) {
			return false;
		}

		TransformHierarchySystem::Propagate(scene);

		Transform2DComponent* childTransform = nullptr;
		const bool hasTransform = scene.TryGetComponent<Transform2DComponent>(childHandle, childTransform) && childTransform;
		Vec2 worldPosition{ 0.0f, 0.0f };
		Vec2 worldScale{ 1.0f, 1.0f };
		float worldRotation = 0.0f;
		if (hasTransform) {
			worldPosition = childTransform->Position;
			worldRotation = childTransform->Rotation;
			worldScale = childTransform->Scale;
		}

		child.SetParent(parent);

		if (hasTransform) {
			Transform2DComponent* parentTransform = nullptr;
			if (parent.IsValid()) {
				scene.TryGetComponent<Transform2DComponent>(parent.GetHandle(), parentTransform);
			}
			SetLocalTransformFromWorld(*childTransform, parentTransform, worldPosition, worldRotation, worldScale);
			TransformHierarchySystem::Propagate(scene);
		}

		return true;
	}

	std::vector<EntityHandle> ImGuiEditorLayer::ResolveDraggedHierarchyEntities(Scene& scene, EntityHandle primary) const {
		std::vector<EntityHandle> result;

		// If the dragged handle is part of the current multi-selection, the
		// gesture means "drag the whole selection." Otherwise (dragging an
		// unselected entity) we keep the single-entity behavior so the
		// existing selection isn't accidentally moved by clicking a
		// non-selected row and dragging it elsewhere.
		const bool draggedIsInSelection =
			std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), primary) != m_SelectedEntities.end();
		if (draggedIsInSelection && m_SelectedEntities.size() > 1) {
			// FilterSelectedHierarchyRoots collapses parent/descendant pairs
			// down to the parent — moving a parent already moves its
			// descendants, so processing both would double-move or fail.
			result = FilterSelectedHierarchyRoots(scene, GetSelectedEntities(scene));
		}

		if (result.empty()) {
			if (primary != entt::null && scene.IsValid(primary)) {
				result.push_back(primary);
			}
		}
		return result;
	}

	bool ImGuiEditorLayer::MoveSiblingNextTo(Scene& scene, EntityHandle dragged, EntityHandle target, bool insertAfter)
	{
		if (dragged == target || !scene.IsValid(dragged) || !scene.IsValid(target)) return false;

		Entity draggedEntity = scene.GetEntity(dragged);
		Entity targetEntity = scene.GetEntity(target);

		// Reparenting `dragged` under one of its own descendants would create
		// a cycle; Entity::SetParent already refuses this silently, but we
		// also guard the sibling reorder path so we don't claim success.
		if (draggedEntity.IsAncestorOf(targetEntity)) return false;

		Entity targetParent = targetEntity.GetParent();
		Entity draggedParent = draggedEntity.GetParent();

		// Cross-branch move: re-attach to target's parent first. SetParent
		// appends to the new parent's Children, so the in-vector reposition
		// below still has work to do.
		if (draggedParent != targetParent) {
			if (!SetEntityParentPreservingWorld(scene, dragged, targetParent)) {
				return false;
			}
		}

		auto repositionVec = [&](std::vector<EntityHandle>& vec) -> bool {
			auto draggedIt = std::find(vec.begin(), vec.end(), dragged);
			if (draggedIt == vec.end()) return false;
			vec.erase(draggedIt);
			auto targetIt = std::find(vec.begin(), vec.end(), target);
			const size_t insertAt = (targetIt == vec.end())
				? vec.size()
				: static_cast<size_t>((targetIt - vec.begin()) + (insertAfter ? 1 : 0));
			vec.insert(vec.begin() + insertAt, dragged);
			return true;
		};

		if (targetParent.IsValid()) {
			auto& childList = scene.GetComponent<HierarchyComponent>(targetParent.GetHandle()).Children;
			repositionVec(childList);
		}
		else {
			// Root-level reorder: m_EntityOrder is the panel's master list and
			// drives the order roots render in. Mutating it here is what makes
			// the drop "stick" between frames — but the engine-side renderer
			// (UIDrawOrder::Build) and the serializer both iterate the entt
			// registry directly, NOT m_EntityOrder, so without the sort below
			// the new order is invisible to render and lost on save+reload.
			repositionVec(m_EntityOrder);
			m_EntityOrderDirty = true; // M30: force re-DFS next render.

			// Mirror m_EntityOrder into entt's entity storage. entt's view
			// iterates dense storage newest-first; UIDrawOrder + the
			// serializer both reverse that into oldest-first. So we want
			// m_EntityOrder.back() to be the "newest" entity in entt (last
			// in m_EntityOrder = drawn on top = highest DrawIndex). Higher
			// m_EntityOrder index → sorts first in entt iteration.
			// Entities not tracked in m_EntityOrder (transient runtime
			// entities created mid-frame, etc.) get a stable tiebreaker
			// from their raw integral value so the comparator stays a
			// strict weak ordering even on the rare missing case.
			std::unordered_map<entt::entity, std::size_t> indexOf;
			indexOf.reserve(m_EntityOrder.size());
			for (std::size_t i = 0; i < m_EntityOrder.size(); ++i) {
				indexOf[m_EntityOrder[i]] = i;
			}
			const std::size_t fallbackBase = m_EntityOrder.size();
			auto& storage = scene.GetRegistry().storage<entt::entity>();
			storage.sort([&](entt::entity a, entt::entity b) {
				auto ita = indexOf.find(a);
				auto itb = indexOf.find(b);
				const std::size_t ai = (ita != indexOf.end())
					? ita->second
					: fallbackBase + static_cast<std::size_t>(entt::to_integral(a));
				const std::size_t bi = (itb != indexOf.end())
					? itb->second
					: fallbackBase + static_cast<std::size_t>(entt::to_integral(b));
				return ai > bi;
			});
		}

		return true;
	}

	bool ImGuiEditorLayer::OpenPrefabForEditing(const std::string& path) {
		if (path.empty() || !File::Exists(path)) {
			AIM_CORE_WARN_TAG("PrefabEdit", "Prefab file not found: {}", path);
			return false;
		}

		// If a prefab is already being edited, auto-save any pending edits
		// before swapping to the new one. The user explicitly asked to open
		// a different prefab, so silent loss-of-work is the wrong default —
		// the alternative (a modal) would require deferring this entire
		// function until the user picks save/discard, which is invasive.
		// Auto-save is safe: SavePrefabEditChanges is a no-op when nothing
		// changed, and identical to clicking the Save button otherwise.
		if (m_PrefabEditScene) {
			if (m_PrefabEditScene->IsDirty()) {
				SavePrefabEditChanges();
			}
			ClosePrefabEditing(false);
		}

		const std::string json = File::ReadAllText(path);
		Json::Value root;
		std::string parseError;
		if (!Json::TryParse(json, root, &parseError) || !root.IsObject()) {
			AIM_CORE_ERROR_TAG("PrefabEdit", "Failed to parse {}: {}", path, parseError);
			return false;
		}

		const Json::Value* entityValue = root.FindMember("Entity");
		if (!entityValue) entityValue = root.FindMember("prefab");
		if (!entityValue || !entityValue->IsObject()) {
			AIM_CORE_WARN_TAG("PrefabEdit", "No Entity/prefab block in {}", path);
			return false;
		}

		// Drop the panel's current view of the active scene before
		// swapping. EntityHandles in m_SelectedEntities / m_EntityOrder
		// belong to the active scene's registry and would alias garbage
		// in the detached prefab registry.
		ClearEntitySelection();
		m_EntityOrder.clear(); m_EntityOrderDirty = true;
		m_CollapsedHierarchyEntities.clear();

		// Snapshot the editor camera so the user returns to their previous
		// framing on Close. We then point the camera at the prefab's root
		// transform — most prefabs are authored at (0, 0) so without this
		// the user lands on whatever the active scene was framing and the
		// prefab is invisible / off-screen ("changes don't show up" report).
		m_PrefabEditSavedCameraPosition = m_EditorCamera.GetPosition();
		m_PrefabEditSavedCameraOrthoSize = m_EditorCamera.GetOrthographicSize();
		m_PrefabEditSavedCameraZoom = m_EditorCamera.GetZoom();
		m_PrefabEditHasSavedCameraState = true;

		auto detached = Scene::CreateDetachedScene("##PrefabEdit");
		const EntityHandle rootEntity = SceneSerializer::DeserializeEntityFromValue(*detached, root);
		// DeserializeEntityFromValue marks the scene dirty as a side
		// effect; clear so our flag only flips on actual user edits.
		detached->ClearDirty();

		// World transforms aren't valid until the hierarchy system runs, but
		// we want to read the root's world position immediately to focus the
		// camera on it. Run a one-shot propagation against the freshly-loaded
		// detached scene so child-of-child positions resolve correctly even
		// though the prefab usually authors at the origin.
		TransformHierarchySystem::Propagate(*detached);

		m_PrefabEditScene = std::move(detached);
		m_PrefabEditPath = path;
		m_PrefabEditRootEntity = rootEntity;
		m_PrefabEditDirty = false;

		// Auto-select + auto-focus on the root. Without selection the user
		// has to click into the hierarchy to find the (potentially deeply
		// nested-named) root before they can edit anything; without focus
		// the camera may be pointed at empty space (see camera-snapshot
		// rationale above). Both fixes target the "I can't see what I'm
		// editing" case the prefab UX was failing on.
		if (rootEntity != entt::null && m_PrefabEditScene->IsValid(rootEntity)) {
			SelectEntity(rootEntity);
			Vec2 focusPos{ 0.0f, 0.0f };
			Transform2DComponent* rootTransform = nullptr;
			if (m_PrefabEditScene->TryGetComponent<Transform2DComponent>(rootEntity, rootTransform) && rootTransform) {
				focusPos = rootTransform->Position;
			}
			m_EditorCamera.SetPosition(focusPos);
			// Reset zoom/ortho size to a sane default so a prior heavily-
			// zoomed-in scene-edit framing doesn't make the prefab fill the
			// viewport (or vice-versa).
			m_EditorCamera.SetZoom(1.0f);
			m_EditorCamera.SetOrthographicSize(5.0f);
		}

		AIM_INFO_TAG("PrefabEdit", "Editing {}", path);
		return true;
	}

	bool ImGuiEditorLayer::SavePrefabEditChanges() {
		if (!m_PrefabEditScene) return false;
		if (m_PrefabEditRootEntity == entt::null
			|| !m_PrefabEditScene->IsValid(m_PrefabEditRootEntity)
			|| m_PrefabEditPath.empty()) {
			return false;
		}

		// Capture the OLD source JSON BEFORE overwriting on disk. Live
		// instance propagation uses this as the baseline for computing each
		// instance's per-field overrides — diffing against the post-save
		// (new) source loses the user's per-instance overrides. Mirrors
		// PrefabInspector::Save().
		Json::Value previousSourceRoot;
		bool havePreviousSource = false;
		if (File::Exists(m_PrefabEditPath)) {
			Json::Value previousRoot;
			std::string parseError;
			if (Json::TryParse(File::ReadAllText(m_PrefabEditPath), previousRoot, &parseError) && previousRoot.IsObject()) {
				previousSourceRoot = previousRoot;
				havePreviousSource = true;
			}
		}

		if (!SceneSerializer::SaveEntityToFile(*m_PrefabEditScene, m_PrefabEditRootEntity, m_PrefabEditPath)) {
			AIM_CORE_ERROR_TAG("PrefabEdit", "Failed to save {}", m_PrefabEditPath);
			return false;
		}

		// Mark the detached scene clean so the toolbar no longer shows the
		// "*" suffix and any auto-save downstream (none today, but mirrors
		// the asset inspector path) doesn't loop on the same edits.
		m_PrefabEditScene->ClearDirty();
		m_PrefabEditDirty = false;

		// Refresh live instances of this prefab in every loaded scene with
		// override-preservation: RefreshPrefabInstance snapshots the
		// instance's overrides relative to the OLD source, re-instantiates
		// against the new source, and re-applies the overrides on top.
		if (havePreviousSource) {
			const uint64_t prefabGuid = AssetRegistry::GetOrCreateAssetUUID(m_PrefabEditPath);
			if (prefabGuid != 0) {
				SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
					if (&s == m_PrefabEditScene.get()) return;
					std::vector<EntityHandle> targets;
					auto view = s.GetRegistry().view<EntityMetaDataComponent>();
					for (entt::entity ent : view) {
						const auto& meta = view.get<EntityMetaDataComponent>(ent).MetaData;
						if (meta.Origin != EntityOrigin::Prefab) continue;
						if (static_cast<uint64_t>(meta.PrefabGUID) != prefabGuid) continue;
						targets.push_back(ent);
					}
					bool anyRefreshed = false;
					for (EntityHandle t : targets) {
						if (!s.IsValid(t)) continue;
						if (SceneSerializer::RefreshPrefabInstance(s, t, previousSourceRoot) != entt::null) {
							anyRefreshed = true;
						}
					}
					if (anyRefreshed) s.MarkDirty();
				});
			}
		}

		return true;
	}

	void ImGuiEditorLayer::ClosePrefabEditing(bool save) {
		if (!m_PrefabEditScene) return;

		if (save) {
			SavePrefabEditChanges();
		}

		ClearEntitySelection();
		m_EntityOrder.clear(); m_EntityOrderDirty = true;
		m_CollapsedHierarchyEntities.clear();
		m_PrefabEditRootEntity = entt::null;
		m_PrefabEditPath.clear();
		m_PrefabEditDirty = false;
		// unique_ptr destruction tears down the registry; destroy hooks
		// gated by Scene::IsDetached() short-circuit so global physics/
		// audio/script subsystems aren't touched.
		m_PrefabEditScene.reset();

		// Restore the editor camera. Order matters: position must come last
		// because SetOrthographicSize / SetZoom recompute projection-only and
		// don't touch the view matrix; setting position last ensures the view
		// matrix is rebuilt from the restored Position field.
		if (m_PrefabEditHasSavedCameraState) {
			m_EditorCamera.SetOrthographicSize(m_PrefabEditSavedCameraOrthoSize);
			m_EditorCamera.SetZoom(m_PrefabEditSavedCameraZoom);
			m_EditorCamera.SetPosition(m_PrefabEditSavedCameraPosition);
			m_PrefabEditHasSavedCameraState = false;
		}
	}

	Scene* ImGuiEditorLayer::GetContextScene() const {
		if (m_PrefabEditScene) return m_PrefabEditScene.get();
		return SceneManager::Get().GetActiveScene();
	}

	bool ImGuiEditorLayer::HasEntityShortcutFocus() const {
		return m_IsEditorViewFocused || m_IsEntitiesPanelFocused || m_IsInspectorPanelFocused;
	}



	// ──────────────────────────────────────────────
	//  Entities Panel (with rename support)
	// ──────────────────────────────────────────────

	void ImGuiEditorLayer::RenderEntitiesPanel() {
		ImGui::Begin("Entities");

		// Prefab-edit-mode toolbar. Drawn above the hierarchy so the
		// user always sees what context they're in and how to leave it.
		// We render this BEFORE the hierarchy so the right-click /
		// drag-drop targets below don't span the toolbar zone.
		if (IsInPrefabEditMode()) {
			// Authoritative dirty flag: the detached scene's IsDirty
			// reflects every component edit, add, remove, parenting
			// change, etc. routed through Scene::MarkDirty. Mirror it
			// onto m_PrefabEditDirty so other code reading the local
			// flag (e.g. the breadcrumb) stays consistent.
			m_PrefabEditDirty = m_PrefabEditScene->IsDirty();

			const std::string prefabName = std::filesystem::path(m_PrefabEditPath).filename().string();
			const std::string breadcrumb = m_PrefabEditDirty
				? std::string("Editing: ") + prefabName + " *"
				: std::string("Editing: ") + prefabName;

			if (ImGui::SmallButton("< Back")) {
				// Dirty-aware back: prompt to save / discard / cancel
				// when the prefab has unsaved changes; close cleanly
				// otherwise. The discard prompt is rendered at the
				// dockspace level (ImGuiEditorLayerChrome) so the modal
				// renders on top of the hierarchy panel rather than
				// trapped inside it.
				if (m_PrefabEditDirty) {
					m_ShowPrefabEditDiscardPrompt = true;
				}
				else {
					ClosePrefabEditing(false);
				}
			}
			ImGui::SameLine();
			// Save = persist to disk + propagate to live instances, BUT
			// stay in prefab-edit mode so the user can keep iterating.
			// Disabled when nothing has changed since the last save —
			// avoids touching the file's mtime on a no-op save (which
			// would otherwise confuse hot-reload watchers).
			ImGui::BeginDisabled(!m_PrefabEditDirty);
			if (ImGui::SmallButton("Save")) {
				SavePrefabEditChanges();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextUnformatted(breadcrumb.c_str());
			ImGui::Separator();
		}

		// In prefab-edit mode, the "current scene" for entity creation,
		// right-click menus, and the hierarchy iteration is the detached
		// prefab scene — not whatever the user had loaded before. The
		// active scene is still alive in SceneManager but hidden from
		// this panel until the user leaves prefab mode.
		Scene* activeScene = GetContextScene();

		// Right-click on empty space: create entities in the active scene
		if (activeScene && ImGui::BeginPopupContextWindow("EntityCreateContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			Scene& scene = *activeScene;
			RenderCreateEntityMenu(scene, Entity::Null);
			ImGui::EndPopup();
		}

		// Build the list of scenes to display. In prefab-edit mode we
		// show only the detached prefab scene; otherwise we mirror the
		// SceneManager's loaded scenes (the existing multi-scene flow).
		std::vector<Scene*> scenesToShow;
		if (IsInPrefabEditMode()) {
			scenesToShow.push_back(m_PrefabEditScene.get());
		}
		else {
			auto loadedScenes = SceneManager::Get().GetLoadedScenes();
			scenesToShow.reserve(loadedScenes.size());
			for (auto& weakScene : loadedScenes) {
				if (auto scenePtr = weakScene.lock()) {
					scenesToShow.push_back(scenePtr.get());
				}
			}
		}
		std::string sceneToRemove;

		for (Scene* scenePtrRaw : scenesToShow) {
			if (!scenePtrRaw) continue;
			Scene& scene = *scenePtrRaw;

			ImGui::PushID(static_cast<int>(static_cast<uint64_t>(scene.GetSceneId())));

			ImGuiTreeNodeFlags sceneFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow
				| ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed;
			const std::string fullSceneLabel = scene.IsDirty() ? scene.GetName() + " *" : scene.GetName();
			bool sceneLabelTruncated = false;
			const std::string sceneLabel = ImGuiUtils::Ellipsize(fullSceneLabel, ImGui::GetContentRegionAvail().x, &sceneLabelTruncated);
			bool sceneOpen = ImGui::TreeNodeEx(sceneLabel.c_str(), sceneFlags);
			if (sceneLabelTruncated && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", fullSceneLabel.c_str());
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
				SelectSceneNode();
			}

			// Drop a dragged entity onto the scene header to unparent it
			// (make it a root again). Multi-selection: every selected
			// entity that has a parent is detached.
			if (ImGui::BeginDragDropTarget()) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
					auto* dragData = static_cast<const HierarchyDragData*>(payload->Data);
					EntityHandle primaryDraggedHandle = static_cast<EntityHandle>(dragData->EntityHandle);
					std::vector<EntityHandle> draggedHandles = ResolveDraggedHierarchyEntities(scene, primaryDraggedHandle);
					bool didMove = false;
					for (EntityHandle h : draggedHandles) {
						Entity draggedEntity = scene.GetEntity(h);
						if (!draggedEntity.HasParent()) continue;
						if (SetEntityParentPreservingWorld(scene, h, Entity::Null)) {
							didMove = true;
						}
					}
					if (didMove) {
						// Force a re-DFS — reparenting changes nesting but
						// not the registry count, so the size-mismatch
						// fallback won't trigger.
						m_EntityOrderDirty = true;
						scene.MarkDirty();
					}
				}
				ImGui::EndDragDropTarget();
			}

			// Right-click context menu on scene tree node. "Remove"
			// only makes sense in scene-edit mode with more than one
			// scene loaded; in prefab-edit mode the single tree-node
			// here IS the prefab and isn't a SceneManager-tracked scene.
			if (ImGui::BeginPopupContextItem()) {
				const bool canRemove = !IsInPrefabEditMode() && scenesToShow.size() > 1;
				if (canRemove) {
					if (ImGui::MenuItem("Remove")) {
						sceneToRemove = scene.GetName();
					}
				} else {
					ImGui::MenuItem("Remove", nullptr, false, false);
				}
				ImGui::EndPopup();
			}

			if (sceneOpen) {
				auto view = scene.GetRegistry().view<entt::entity>();

				// M30: skip the sync diff + DFS subtree-emit pass when nothing
				// has changed. We detect change by:
				//   • m_EntityOrderDirty (flipped at every callsite that used
				//     to clear() the order: entity create / delete / drop /
				//     scene swap / etc.)
				//   • registry size mismatch as a safety net for any caller
				//     that mutates the registry without flipping the flag.
				const auto registryCount = static_cast<std::size_t>(std::distance(view.begin(), view.end()));
				const bool needsRebuild = m_EntityOrderDirty
					|| m_EntityOrder.size() != registryCount;

				if (needsRebuild) {
					// Sync m_EntityOrder with the registry without ever blanket-
					// resetting it: the user's drag-reorder lives inside this
					// vector, so dropping it on size change (e.g. one entity
					// deleted) would silently undo their work. Set-diff keeps
					// the existing order intact and only mutates the deltas.
					//
					// New entities go to the END so freshly-created entities land
					// at the bottom of the hierarchy. entt's `view<entt::entity>`
					// iterates the dense storage in reverse-insertion order
					// (newest first), so we walk it backwards — that way a scene
					// load that creates N entities in sequence yields the same
					// order on screen as the order they were created/serialized.
					{
						std::vector<entt::entity> viewEntities(view.begin(), view.end());

						std::unordered_set<uint32_t> viewSet;
						viewSet.reserve(viewEntities.size());
						for (auto e : viewEntities) viewSet.insert(static_cast<uint32_t>(e));
						m_EntityOrder.erase(
							std::remove_if(m_EntityOrder.begin(), m_EntityOrder.end(),
								[&](entt::entity e) { return viewSet.find(static_cast<uint32_t>(e)) == viewSet.end(); }),
							m_EntityOrder.end());

						std::unordered_set<uint32_t> orderSet;
						orderSet.reserve(m_EntityOrder.size());
						for (auto e : m_EntityOrder) orderSet.insert(static_cast<uint32_t>(e));
						for (auto it = viewEntities.rbegin(); it != viewEntities.rend(); ++it) {
							if (orderSet.find(static_cast<uint32_t>(*it)) == orderSet.end()) {
								m_EntityOrder.push_back(*it);
							}
						}
					}

					// DFS reorder so each child appears immediately after its
					// parent in the flat list — that lets the existing single-
					// loop renderer show parent-child nesting just by tracking
					// depth and calling ImGui::Indent. Roots come first in
					// the user's chosen m_EntityOrder; their subtrees follow
					// in the order each parent's HierarchyComponent::Children
					// lists them.
					m_RenderedEntityDepths.clear();
					m_RenderedEntityDepths.reserve(m_EntityOrder.size());
					{
						auto& reg = scene.GetRegistry();
						std::vector<entt::entity> reordered;
						reordered.reserve(m_EntityOrder.size());
						std::unordered_set<uint32_t> emitted;

						std::function<void(EntityHandle, int)> emitSubtree =
							[&](EntityHandle e, int depth) {
								if (!reg.valid(e)) return;
								const uint32_t key = static_cast<uint32_t>(e);
								if (!emitted.insert(key).second) return; // already in tree
								reordered.push_back(e);
								m_RenderedEntityDepths.push_back(depth);
								if (reg.all_of<HierarchyComponent>(e)) {
									for (EntityHandle child : reg.get<HierarchyComponent>(e).Children) {
										emitSubtree(child, depth + 1);
									}
								}
							};

						// Pass 1: emit each root-of-subtree from the user-ordered
						// list. Skip entities that already appeared as a child.
						for (EntityHandle e : m_EntityOrder) {
							if (!reg.valid(e)) continue;
							EntityHandle parent = entt::null;
							if (reg.all_of<HierarchyComponent>(e)) {
								parent = reg.get<HierarchyComponent>(e).Parent;
							}
							if (parent == entt::null) {
								emitSubtree(e, 0);
							}
						}
						// Pass 2: anything still missing — orphaned children
						// whose parent is not in the registry — render flat.
						for (EntityHandle e : m_EntityOrder) {
							if (!reg.valid(e)) continue;
							const uint32_t key = static_cast<uint32_t>(e);
							if (emitted.contains(key)) continue;
							emitSubtree(e, 0);
						}
						m_EntityOrder = std::move(reordered);
					}

					m_RenderedEntityOrder = m_EntityOrder;
					m_EntityOrderDirty = false;
				}

				// Local alias so the existing render loop reads from the
				// cached depth list whether we just rebuilt or not.
				const std::vector<int>& entityDepths = m_RenderedEntityDepths;

				int currentIndentDepth = 0;
				// When set to >= 0, any subsequent entity with depth greater than
				// this value is hidden because one of its ancestors is folded.
				// Reset whenever we surface back to that depth or shallower.
				int hideUnderDepth = -1;

				// Tighten vertical spacing between rows so the hierarchy reads as
				// a dense list (Unity-style) rather than spaced-out items. Only
				// the y-component of ItemSpacing changes; horizontal spacing for
				// SameLine calls inside the row stays at the default.
				const ImVec2 defaultItemSpacing = ImGui::GetStyle().ItemSpacing;
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(defaultItemSpacing.x, 1.0f));

				for (int entityIdx = 0; entityIdx < static_cast<int>(m_EntityOrder.size()); entityIdx++) {
					const EntityHandle entityHandle = m_EntityOrder[entityIdx];
					if (!scene.IsValid(entityHandle)) continue;
					Entity entity = scene.GetEntity(entityHandle);
					const bool selected = IsEntitySelected(entityHandle);

					const int targetDepth = entityIdx < static_cast<int>(entityDepths.size())
						? entityDepths[entityIdx] : 0;

					// Skip entities whose ancestor (at hideUnderDepth) is folded.
					if (hideUnderDepth >= 0) {
						if (targetDepth > hideUnderDepth) {
							continue;
						}
						hideUnderDepth = -1;
					}

					// Step indent up/down to match this entity's depth.
					while (currentIndentDepth < targetDepth) {
						ImGui::Indent(14.0f);
						++currentIndentDepth;
					}
					while (currentIndentDepth > targetDepth) {
						ImGui::Unindent(14.0f);
						--currentIndentDepth;
					}

					ImGui::PushID(static_cast<int>(static_cast<uint32_t>(entityHandle)));

					// Fold-out toggle. We use an InvisibleButton sized to the
					// text line height (rather than ImGui::ArrowButton which is
					// FrameHeight-tall and inflates each row) and then manually
					// draw the triangle on top — that way every row stays at one
					// line of text, like a tight tree view. Entities without
					// children still consume an equal-width spacer so labels
					// stay vertically aligned across siblings.
					const bool hasChildren = scene.HasComponent<HierarchyComponent>(entityHandle)
						&& !scene.GetComponent<HierarchyComponent>(entityHandle).Children.empty();
					bool isCollapsed = m_CollapsedHierarchyEntities.contains(static_cast<uint32_t>(entityHandle));
					{
						const float arrowSize = ImGui::GetTextLineHeight();
						if (hasChildren) {
							const ImVec2 arrowTopLeft = ImGui::GetCursorScreenPos();
							if (ImGui::InvisibleButton("##fold", ImVec2(arrowSize, arrowSize))) {
								if (isCollapsed) {
									m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(entityHandle));
								}
								else {
									m_CollapsedHierarchyEntities.insert(static_cast<uint32_t>(entityHandle));
								}
								isCollapsed = !isCollapsed;
							}
							const ImU32 arrowColor = ImGui::GetColorU32(
								ImGui::IsItemHovered() ? ImGuiCol_HeaderHovered : ImGuiCol_Text);
							ImGui::RenderArrow(ImGui::GetWindowDrawList(), arrowTopLeft,
								arrowColor,
								isCollapsed ? ImGuiDir_Right : ImGuiDir_Down,
								0.7f);
						}
						else {
							ImGui::Dummy(ImVec2(arrowSize, arrowSize));
						}
						ImGui::SameLine(0.0f, 2.0f);
					}

					bool entityIsDisabled = scene.HasComponent<DisabledTag>(entityHandle);
					if (entityIsDisabled)
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));

					// Prefab-instance tint: amber if the source GUID can't be resolved
					// (orphaned), accent-blue otherwise. Engine is flat today so all
					// prefab-origin entities get the same color — no root vs child
					// distinction is possible until parent/child components land.
					bool entityIsPrefabTinted = false;
					if (!entityIsDisabled && scene.GetEntityOrigin(entityHandle) == EntityOrigin::Prefab) {
						const uint64_t prefabGuid = static_cast<uint64_t>(scene.GetPrefabGUID(entityHandle));
						const bool resolvable = prefabGuid != 0 && !AssetRegistry::ResolvePath(prefabGuid).empty();
						ImGui::PushStyleColor(ImGuiCol_Text,
							resolvable ? EditorTheme::Colors::PrefabInstance : EditorTheme::Colors::PrefabOrphan);
						entityIsPrefabTinted = true;
					}

					if (m_RenamingEntity == entityHandle) {
						m_EntityRenameFrameCounter++;

						ImGui::PushItemWidth(-1);
						if (m_EntityRenameFrameCounter == 1) {
							ImGui::SetKeyboardFocusHere();
						}

						bool committed = ImGui::InputText("##EntityRename", m_EntityRenameBuffer, sizeof(m_EntityRenameBuffer),
							ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

						if (committed) {
							std::string newName(m_EntityRenameBuffer);
							if (!newName.empty() && entity.HasComponent<NameComponent>()) {
								entity.GetComponent<NameComponent>().Name = newName;
								scene.MarkDirty();
							}
							m_RenamingEntity = entt::null;
							m_EntityRenameFrameCounter = 0;
						}
						else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
							m_RenamingEntity = entt::null;
							m_EntityRenameFrameCounter = 0;
						}
						else if (m_EntityRenameFrameCounter > 2 && !ImGui::IsItemActive()) {
							std::string newName(m_EntityRenameBuffer);
							if (!newName.empty() && entity.HasComponent<NameComponent>()) {
								entity.GetComponent<NameComponent>().Name = newName;
								scene.MarkDirty();
							}
							m_RenamingEntity = entt::null;
							m_EntityRenameFrameCounter = 0;
						}

						ImGui::PopItemWidth();
					}
					else {
						bool entityLabelTruncated = false;
						const std::string entityLabel = ImGuiUtils::Ellipsize(entity.GetName(), ImGui::GetContentRegionAvail().x, &entityLabelTruncated);
						ImGui::Selectable(entityLabel.c_str(), selected);
						if (entityLabelTruncated && ImGui::IsItemHovered()) {
							ImGui::SetTooltip("%s", entity.GetName().c_str());
						}
						if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
							m_PressedEntity = entityHandle;
						}
						if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
							if (ImGui::IsItemHovered() && m_PressedEntity == entityHandle && !IsLeftMouseDragPastClickThreshold()) {
								const ImGuiIO& io = ImGui::GetIO();
								if (io.KeyShift) {
									SelectEntityRange(entityIdx);
								}
								else if (io.KeyCtrl) {
									ToggleEntitySelection(entityHandle, entityIdx);
								}
								else {
									SetSingleEntitySelection(entityHandle, entityIdx);
								}
							}
							if (m_PressedEntity == entityHandle) {
								m_PressedEntity = entt::null;
							}
						}

						if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
							m_RenamingEntity = entityHandle;
							m_EntityRenameFrameCounter = 0;
							std::snprintf(m_EntityRenameBuffer, sizeof(m_EntityRenameBuffer), "%s", entity.GetName().c_str());
						}
					}

					// Drag-drop source: drag entity to reorder or to asset browser for prefab.
					// The payload only carries the primary (clicked) entity; the
					// hierarchy drop sites resolve the full multi-selection at
					// drop time via ResolveDraggedHierarchyEntities so external
					// drop targets that expect a single entity (PropertyDrawer,
					// AssetBrowser→prefab) keep working unchanged.
					if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
						HierarchyDragData dragData{ entityIdx, static_cast<uint32_t>(entityHandle) };
						ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &dragData, sizeof(dragData));
						const bool draggingMulti = IsEntitySelected(entityHandle) && m_SelectedEntities.size() > 1;
						if (draggingMulti) {
							ImGui::Text("Move: %zu entities", m_SelectedEntities.size());
						} else {
							ImGui::Text("Move: %s", entity.GetName().c_str());
						}
						ImGui::EndDragDropSource();
					}

					// Capture the row rect before opening the drag-drop target so
					// we can split it into three zones: top quarter inserts as
					// previous sibling, bottom quarter inserts as next sibling,
					// and the middle reparents (the original behavior). This
					// lets the user rearrange siblings without leaving the
					// drag gesture, similar to Unity / Godot.
					const ImVec2 itemRectMin = ImGui::GetItemRectMin();
					const ImVec2 itemRectMax = ImGui::GetItemRectMax();
					const float itemHeight = itemRectMax.y - itemRectMin.y;
					const float zoneHeight = itemHeight * 0.25f;

					if (ImGui::BeginDragDropTarget()) {
						const float mouseY = ImGui::GetMousePos().y;
						const bool inTopZone = mouseY < itemRectMin.y + zoneHeight;
						const bool inBottomZone = mouseY > itemRectMax.y - zoneHeight;

						// Insertion-point indicator. Drawn while a hierarchy drag
						// is hovering the row — a thin colored line at the top
						// or bottom edge tells the user which sibling slot they
						// will land in.
						if (const ImGuiPayload* peek = ImGui::GetDragDropPayload()) {
							if (peek->IsDataType("HIERARCHY_ENTITY") && (inTopZone || inBottomZone)) {
								const float lineY = inTopZone ? itemRectMin.y : itemRectMax.y;
								const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
								ImGui::GetWindowDrawList()->AddLine(
									ImVec2(itemRectMin.x, lineY),
									ImVec2(itemRectMax.x, lineY),
									color, 2.0f);
							}
						}

						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
							auto* dragData = static_cast<const HierarchyDragData*>(payload->Data);
							EntityHandle primaryDraggedHandle = static_cast<EntityHandle>(dragData->EntityHandle);
							std::vector<EntityHandle> draggedHandles = ResolveDraggedHierarchyEntities(scene, primaryDraggedHandle);
							bool didMove = false;
							if (inTopZone || inBottomZone) {
								// Sibling-insert: anchor advances after each move so a
								// multi-drop produces a contiguous block in selection
								// order. Drop-after walks forward (each next entity
								// becomes the next sibling of the previous one);
								// drop-before walks the selection in reverse so the
								// final order at the target's slot is selection order.
								EntityHandle anchor = entityHandle;
								if (inBottomZone) {
									for (EntityHandle h : draggedHandles) {
										if (h == anchor) continue;
										if (MoveSiblingNextTo(scene, h, anchor, /*insertAfter*/true)) {
											didMove = true;
											anchor = h;
										}
									}
								}
								else {
									for (auto it = draggedHandles.rbegin(); it != draggedHandles.rend(); ++it) {
										EntityHandle h = *it;
										if (h == anchor) continue;
										if (MoveSiblingNextTo(scene, h, anchor, /*insertAfter*/false)) {
											didMove = true;
											anchor = h;
										}
									}
								}
							}
							else {
								// Reparent: drop each onto `entity`. Skip drops that
								// SetParent would refuse anyway (cycle: target is a
								// descendant of dragged) so we don't claim a move
								// or unfold the target on a no-op.
								for (EntityHandle h : draggedHandles) {
									if (h == entityHandle) continue;
									Entity draggedEntity = scene.GetEntity(h);
									if (draggedEntity.IsAncestorOf(entity)) continue;
									if (SetEntityParentPreservingWorld(scene, h, entity)) {
										didMove = true;
									}
								}
								if (didMove) {
									// Unfold the target so the dropped children are visible
									// (a no-op when the target was already expanded).
									m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(entityHandle));
									// Force a re-DFS next frame: reparenting doesn't
									// change the registry's entity count, so the size-
									// mismatch heuristic in the renderer won't trip.
									// Without this flag the dragged entity stays
									// visually in its old subtree until the user
									// reloads the scene, even though the parent ref
									// has actually moved.
									m_EntityOrderDirty = true;
								}
							}
							if (didMove) {
								scene.MarkDirty();
							}
						}
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
							std::string droppedPath(static_cast<const char*>(payload->Data));
							std::string ext = std::filesystem::path(droppedPath).extension().string();
							std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
							if (ext == ".prefab") {
								SceneSerializer::LoadEntityFromFile(scene, droppedPath);
								m_EntityOrder.clear(); m_EntityOrderDirty = true;
							}
						}
						ImGui::EndDragDropTarget();
					}

					if (ImGui::BeginPopupContextItem())
					{
						if (!IsEntitySelected(entityHandle)) {
							SetSingleEntitySelection(entityHandle, entityIdx);
						}

						if (ImGui::MenuItem("Delete Entity"))
						{
							DeleteSelectedEntity(scene);
						}

						if (ImGui::MenuItem("Duplicate"))
						{
							DuplicateSelectedEntity(scene);
						}

						if (ImGui::MenuItem("Rename"))
						{
							BeginRenameSelectedEntity(scene);
						}

						// "Unpack Prefab" only shows up when at least one
						// selected entity is currently a prefab instance —
						// no point cluttering the menu otherwise.
						bool anySelectedIsPrefabInstance = false;
						for (EntityHandle h : GetSelectedEntities(scene)) {
							if (scene.HasComponent<PrefabInstanceComponent>(h)) {
								anySelectedIsPrefabInstance = true;
								break;
							}
						}
						if (anySelectedIsPrefabInstance) {
							ImGui::Separator();
							if (ImGui::MenuItem("Unpack Prefab")) {
								UnpackSelectedPrefabs(scene);
							}
						}

						ImGui::Separator();
						RenderCreateEntityMenu(scene, entity);

						ImGui::EndPopup();
					}

					if (entityIsPrefabTinted)
						ImGui::PopStyleColor();
					if (entityIsDisabled)
						ImGui::PopStyleColor();

					// Hide every descendant until we surface back to this depth or shallower.
					if (hasChildren && isCollapsed) {
						hideUnderDepth = targetDepth;
					}

					ImGui::PopID();
				}

				// Pop any remaining indents from nested entities so the
				// next scene tree node starts cleanly at depth 0.
				while (currentIndentDepth > 0) {
					ImGui::Unindent(14.0f);
					--currentIndentDepth;
				}

				ImGui::PopStyleVar(); // ItemSpacing

				ImGui::TreePop();
			}

			ImGui::PopID();
		}

		// Deferred scene removal (after iteration)
		if (!sceneToRemove.empty()) {
			ClearEntitySelection();
			SceneManager::Get().UnloadScene(sceneToRemove);
		}

		// Drag-drop target: only present during an active drag so it doesn't block right-click menus
		if (ImGui::GetDragDropPayload() != nullptr) {
			ImVec2 avail = ImGui::GetContentRegionAvail();
			if (avail.y > 0) {
				ImGui::InvisibleButton("##SceneDropTarget", ImVec2(-1, avail.y));
				if (ImGui::BeginDragDropTarget()) {
					// Dropping an entity into the empty space at the bottom of
					// the panel detaches it (becomes a root again) AND moves
					// it to the end of m_EntityOrder so it shows up at the
					// bottom of the root list — matching the user's gesture.
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
						auto* dragData = static_cast<const HierarchyDragData*>(payload->Data);
						EntityHandle primaryDraggedHandle = static_cast<EntityHandle>(dragData->EntityHandle);
						Scene* dropScene = GetContextScene();
						if (dropScene) {
							std::vector<EntityHandle> draggedHandles = ResolveDraggedHierarchyEntities(*dropScene, primaryDraggedHandle);
							bool didMove = false;
							for (EntityHandle h : draggedHandles) {
								Entity draggedEntity = dropScene->GetEntity(h);
								if (draggedEntity.HasParent()) {
									if (SetEntityParentPreservingWorld(*dropScene, h, Entity::Null)) {
										didMove = true;
									}
								}
								// Push every dragged root to the bottom of m_EntityOrder
								// in selection order — matches the user's "I dropped
								// these at the bottom" gesture even when one was
								// already a root.
								auto orderIt = std::find(m_EntityOrder.begin(), m_EntityOrder.end(), h);
								if (orderIt != m_EntityOrder.end() && std::next(orderIt) != m_EntityOrder.end()) {
									m_EntityOrder.erase(orderIt);
									m_EntityOrder.push_back(h);
									m_EntityOrderDirty = true; // M30: re-DFS next render.
								}
							}
							if (didMove) {
								// Reparent to root happened — force a re-DFS in
								// case the in-vector reposition above didn't run
								// (entity was already at the bottom slot).
								m_EntityOrderDirty = true;
								dropScene->MarkDirty();
							}
						}
					}
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
						std::string droppedPath(static_cast<const char*>(payload->Data));
						std::string ext = std::filesystem::path(droppedPath).extension().string();
						std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
						if (ext == ".scene") {
							// Loading a .scene only makes sense in scene-edit
							// mode — silently ignored while editing a prefab.
							if (!IsInPrefabEditMode()) {
								m_PendingSceneFileDrop = droppedPath;
							}
						}
						else if (ext == ".prefab") {
							Scene* dropScene = GetContextScene();
							if (dropScene) {
								SceneSerializer::LoadEntityFromFile(*dropScene, droppedPath);
								m_EntityOrder.clear(); m_EntityOrderDirty = true;
							}
						}
						else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
							Scene* dropScene = GetContextScene();
							if (dropScene) {
								std::string entityName = std::filesystem::path(droppedPath).stem().string();
								Entity newEntity = dropScene->CreateEntity(entityName);
								auto& sr = newEntity.AddComponent<SpriteRendererComponent>();
								sr.TextureHandle = TextureManager::LoadTexture(droppedPath);

								Texture2D* tex = TextureManager::GetTexture(sr.TextureHandle);
								if (tex && tex->IsValid() && tex->GetHeight() > 0) {
									float aspect = (float)tex->GetWidth() / (float)tex->GetHeight();
									auto& transform = newEntity.GetComponent<Transform2DComponent>();
									transform.Scale = { aspect, 1.0f };
								}

								SelectEntity(newEntity.GetHandle());
								dropScene->MarkDirty();
							}
						}
					}
					ImGui::EndDragDropTarget();
				}
			}
		}

		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)
			&& ImGui::IsMouseReleased(ImGuiMouseButton_Left)
			&& !IsLeftMouseDragPastClickThreshold()
			&& !ImGui::IsAnyItemHovered()) {
			ClearEntitySelection();
		}

		m_IsEntitiesPanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		ImGui::End();
	}

	void ImGuiEditorLayer::RenderSceneSystemsInspector(Scene& scene) {
		ImGui::Text("Scene: %s", scene.GetName().c_str());
		ImGui::SeparatorText("Systems");

		const auto& systems = scene.GetGameSystemClassNames();
		for (size_t i = 0; i < systems.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));
			const std::string& className = systems[i];

			// Reorder/remove buttons share the row with the collapsing header so
			// the layout matches the prior text-only design — header on the left,
			// trailing actions on the right via SameLine. AllowOverlap lets the
			// SmallButton hit-test even though it's drawn over the header.
			bool open = ImGui::CollapsingHeader((className + "##system_header").c_str(),
				ImGuiTreeNodeFlags_AllowOverlap);

			ImGui::SameLine();
			if (ImGui::SmallButton("^") && i > 0) {
				scene.MoveGameSystem(i, i - 1);
			}

			ImGui::SameLine();
			if (ImGui::SmallButton("v") && i + 1 < systems.size()) {
				scene.MoveGameSystem(i, i + 1);
			}

			ImGui::SameLine();
			if (ImGui::SmallButton("Remove")) {
				scene.RemoveGameSystem(i);
				ImGui::PopID();
				break;
			}

			if (open) {
				ImGui::Indent(8.0f);
				DrawGameSystemFields(scene, className);
				ImGui::Unindent(8.0f);
			}

			ImGui::PopID();
		}

		// Pre-collect available systems so the button can be disabled when nothing
		// can be added — avoids the empty-popup sliver that looks broken.
		std::vector<EditorScriptDiscovery::ScriptEntry> scriptEntries;
		EditorScriptDiscovery::CollectProjectScriptEntries(scriptEntries);
		size_t availableSystemCount = 0;
		for (const auto& entry : scriptEntries) {
			if (entry.IsGameSystem && !scene.HasGameSystem(entry.ClassName)) {
				++availableSystemCount;
			}
		}
		const bool hasAvailableSystem = availableSystemCount > 0;

		float buttonWidth = ImGui::GetContentRegionAvail().x;
		if (!hasAvailableSystem) ImGui::BeginDisabled();
		if (ImGui::Button("+ Add System", ImVec2(buttonWidth, 0))) {
			ImGui::OpenPopup("AddSystemPopup");
			m_SystemSearchBuffer[0] = '\0';
		}
		if (!hasAvailableSystem) {
			ImGui::EndDisabled();
			// AllowWhenDisabled so hover still fires after BeginDisabled.
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("No game systems available.\nCreate one via Asset Browser > Create > Scripting > GameSystem (C#).");
			}
		}

		if (ImGui::BeginPopup("AddSystemPopup")) {
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##SystemSearch", "Search systems...",
				m_SystemSearchBuffer, sizeof(m_SystemSearchBuffer));
			ImGui::Separator();

			std::string filter(m_SystemSearchBuffer);
			std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

			for (const auto& scriptEntry : scriptEntries) {
				if (!scriptEntry.IsGameSystem || scene.HasGameSystem(scriptEntry.ClassName)) {
					continue;
				}

				if (!filter.empty()) {
					std::string lowerClassName = EditorScriptDiscovery::ToLowerCopy(scriptEntry.ClassName);
					std::string lowerPath = EditorScriptDiscovery::ToLowerCopy(scriptEntry.Path.string());
					if (lowerClassName.find(filter) == std::string::npos
						&& lowerPath.find(filter) == std::string::npos) {
						continue;
					}
				}

				const std::string label = BuildScriptMenuLabel(scriptEntry);
				const std::string path = scriptEntry.Path.string();
				if (ImGuiUtils::MenuItemEllipsis(label, path.c_str(), nullptr, false, true, 260.0f)) {
					scene.AddGameSystem(scriptEntry.ClassName);
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndPopup();
		}
	}

	void ImGuiEditorLayer::RenderInspectorPanel(Scene& scene) {
		ImGui::Begin("Inspector");

		// Materialize the selection set (filters dropped entities). When N == 0
		// fall through to the asset / scene-systems panels exactly like before.
		std::vector<EntityHandle> selectedHandles = GetSelectedEntities(scene);
		if (selectedHandles.empty()) {
			m_SelectedEntity = entt::null;
			if (m_IsSceneNodeSelected) {
				RenderSceneSystemsInspector(scene);
				// GameSystem fields can include reference pickers (Texture,
				// Audio, Entity). The picker opens here, so it has to render
				// here too — the entity-selection branch's RenderPopup call
				// further down isn't reachable from this early return.
				ReferencePicker::RenderPopup();
			}
			else {
				RenderAssetInspector();
			}
			m_IsInspectorPanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			ImGui::End();
			return;
		}

		// Stable Entity wrappers for the selection — reused for the component
		// intersection and passed to every multi-edit inspector below.
		std::vector<Entity> selectedEntities;
		selectedEntities.reserve(selectedHandles.size());
		for (EntityHandle h : selectedHandles) {
			selectedEntities.push_back(scene.GetEntity(h));
		}
		std::span<const Entity> entitySpan(selectedEntities);
		Entity entity = selectedEntities[0]; // primary, used for the drag-source label and prefab buttons

		// Keep m_SelectedEntity pointed at a valid handle — some callers
		// (prefab Apply / Revert, focus / duplicate / rename helpers) still
		// index it directly.
		if (!scene.IsValid(m_SelectedEntity)) {
			m_SelectedEntity = entity.GetHandle();
		}

		// ── Entity Header: Name + Toggles ──────────────────

		// Editable Name. With multi-select, mixed names display "—" via the
		// hint and edits propagate to all entities (creating NameComponent
		// when missing).
		bool nameUniform = true;
		std::string firstName = entity.HasComponent<NameComponent>()
			? entity.GetComponent<NameComponent>().Name : std::string("Entity");
		for (std::size_t i = 1; i < selectedEntities.size(); ++i) {
			std::string n = selectedEntities[i].HasComponent<NameComponent>()
				? selectedEntities[i].GetComponent<NameComponent>().Name : std::string("Entity");
			if (n != firstName) { nameUniform = false; break; }
		}
		char nameBuf[256]{};
		if (nameUniform) {
			std::snprintf(nameBuf, sizeof(nameBuf), "%s", firstName.c_str());
		}
		ImGui::SetNextItemWidth(-1);
		const bool nameSubmitted = nameUniform
			? ImGui::InputText("##EntityName", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue)
			: ImGui::InputTextWithHint("##EntityName", "-", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
		if (nameSubmitted) {
			std::string newName(nameBuf);
			for (Entity& e : selectedEntities) {
				if (e.HasComponent<NameComponent>()) {
					e.GetComponent<NameComponent>().Name = newName;
				}
				else {
					scene.AddComponent<NameComponent>(e.GetHandle(), newName);
				}
			}
			scene.MarkDirty();
		}

		// Toggles row: Enabled + Static. Tri-state when the underlying tag
		// presence differs across the selection. Editing applies to all.
		{
			constexpr int kMixedValueFlag = 1 << 12;

			auto sampleHas = [&](auto checkFn) -> std::pair<bool, bool> {
				bool first = checkFn(selectedEntities[0]);
				bool uniform = true;
				for (std::size_t i = 1; i < selectedEntities.size(); ++i) {
					if (checkFn(selectedEntities[i]) != first) {
						uniform = false;
						break;
					}
				}
				return { first, uniform };
			};

			// Inspector toggle reflects the AUTHORED enabled state, not
			// the effective in-hierarchy state. A child whose parent is
			// disabled has both DisabledTag and InheritedDisabledTag —
			// inherited-disabled doesn't flip the user's intent, so we
			// keep the checkbox on. The parent re-enable cascade then
			// restores the child's runtime DisabledTag accordingly.
			auto enabledSample = sampleHas([](Entity& e) {
				return !e.HasComponent<DisabledTag>() || e.HasComponent<InheritedDisabledTag>();
			});
			bool isEnabled = enabledSample.first;
			if (!enabledSample.second) ImGui::PushItemFlag(static_cast<ImGuiItemFlags>(kMixedValueFlag), true);
			const bool enabledChanged = ImGui::Checkbox("Enabled", &isEnabled);
			if (!enabledSample.second) ImGui::PopItemFlag();
			if (enabledChanged) {
				for (Entity& e : selectedEntities) {
					e.SetEnabled(isEnabled);
				}
				scene.MarkDirty();
			}

			ImGui::SameLine();

			auto staticSample = sampleHas([](Entity& e) { return e.HasComponent<StaticTag>(); });
			bool isStatic = staticSample.first;
			if (!staticSample.second) ImGui::PushItemFlag(static_cast<ImGuiItemFlags>(kMixedValueFlag), true);
			const bool staticChanged = ImGui::Checkbox("Static", &isStatic);
			if (!staticSample.second) ImGui::PopItemFlag();
			if (staticChanged) {
				for (Entity& e : selectedEntities) {
					if (isStatic && !e.HasComponent<StaticTag>()) e.AddComponent<StaticTag>();
					else if (!isStatic && e.HasComponent<StaticTag>()) e.RemoveComponent<StaticTag>();
				}
				scene.MarkDirty();
			}
		}

		/* INFO(Ben-Scr): MetaData Doesn't need to be displayed
		if (const EntityMetaData* metaData = entity.GetMetaData()) {
			const char* originText = "Runtime";
			if (metaData->Origin == EntityOrigin::Scene) originText = "Scene";
			if (metaData->Origin == EntityOrigin::Prefab) originText = "Prefab";
			ImGui::TextDisabled("Origin: %s", originText);

			const uint64_t runtimeId = metaData->RuntimeID;
			ImGui::TextDisabled("RuntimeID: %llu", runtimeId);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy");
			if (ImGui::IsItemClicked()) ImGui::SetClipboardText(std::to_string(runtimeId).c_str());

			if (metaData->Origin == EntityOrigin::Scene && static_cast<uint64_t>(metaData->SceneGUID) != 0) {
				const uint64_t sceneGuid = static_cast<uint64_t>(metaData->SceneGUID);
				ImGui::TextDisabled("SceneGUID: %llu", sceneGuid);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy");
				if (ImGui::IsItemClicked()) ImGui::SetClipboardText(std::to_string(sceneGuid).c_str());
			}
			if (metaData->Origin == EntityOrigin::Prefab && static_cast<uint64_t>(metaData->PrefabGUID) != 0) {
				const uint64_t prefabGuid = static_cast<uint64_t>(metaData->PrefabGUID);
				ImGui::TextDisabled("PrefabGUID: %llu", prefabGuid);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy");
				if (ImGui::IsItemClicked()) ImGui::SetClipboardText(std::to_string(prefabGuid).c_str());

				if (ImGui::Button("Apply to Prefab")) {
					SceneSerializer::ApplyPrefabInstanceOverrides(scene, m_SelectedEntity);
				}
				ImGui::SameLine();
				if (ImGui::Button("Revert All")) {
					EntityHandle replacement = SceneSerializer::RevertPrefabInstanceOverride(scene, m_SelectedEntity, {});
					if (replacement != entt::null) {
						SelectEntity(replacement);
						m_EntityOrder.clear(); m_EntityOrderDirty = true;
					}
				}
			}
		}
		*/

		m_IsInspectorPanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		ImGui::Separator();

		const auto& registry = SceneManager::Get().GetComponentRegistry();

		std::type_index pendingRemoval = typeid(void);

		// Prefab override state for the inspector header / per-component badges.
		// Computed once per frame for the primary entity when the selection is
		// a single prefab instance. Multi-select + prefab overrides is out of
		// scope for v1 — when more than one entity is selected the UI behaves
		// as if no overrides exist (consistent with mixed-value semantics).
		Json::Value prefabOverrides = Json::Value::MakeObject();
		const bool isSinglePrefabInstance = selectedEntities.size() == 1
			&& scene.GetEntityOrigin(entity.GetHandle()) == EntityOrigin::Prefab
			&& static_cast<uint64_t>(scene.GetPrefabGUID(entity.GetHandle())) != 0;
		bool prefabSourceResolvable = false;
		if (isSinglePrefabInstance) {
			prefabSourceResolvable = SceneSerializer::ComputeInstanceOverrides(
				scene, entity.GetHandle(), prefabOverrides);
		}

		// Top-of-inspector prefab actions (only when the source resolves; orphans
		// can't apply or revert because we have no source to diff against).
		if (isSinglePrefabInstance && prefabSourceResolvable) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.66f, 0.95f, 1.0f));
			ImGui::TextUnformatted("Prefab Instance");
			ImGui::PopStyleColor();
			if (ImGui::SmallButton("Apply All")) {
				// Capture old source BEFORE the apply, then push instance state to disk
				// and propagate to other live instances preserving their overrides.
				const std::string prefabPath = AssetRegistry::ResolvePath(
					static_cast<uint64_t>(scene.GetPrefabGUID(entity.GetHandle())));
				Json::Value previousSourceEntity;
				bool havePrev = false;
				if (!prefabPath.empty() && File::Exists(prefabPath)) {
					Json::Value previousRoot;
					std::string parseError;
					if (Json::TryParse(File::ReadAllText(prefabPath), previousRoot, &parseError) && previousRoot.IsObject()) {
						if (const Json::Value* eb = previousRoot.FindMember("Entity")) { previousSourceEntity = *eb; havePrev = true; }
						else if (const Json::Value* lb = previousRoot.FindMember("prefab")) { previousSourceEntity = *lb; havePrev = true; }
					}
				}
				if (SceneSerializer::ApplyPrefabInstanceOverrides(scene, entity.GetHandle()) && havePrev) {
					const uint64_t prefabGuid = static_cast<uint64_t>(scene.GetPrefabGUID(entity.GetHandle()));
					SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
						std::vector<EntityHandle> targets;
						auto view = s.GetRegistry().view<EntityMetaDataComponent>();
						for (entt::entity e2 : view) {
							const auto& meta = view.get<EntityMetaDataComponent>(e2).MetaData;
							if (meta.Origin != EntityOrigin::Prefab) continue;
							if (static_cast<uint64_t>(meta.PrefabGUID) != prefabGuid) continue;
							if (e2 == entity.GetHandle() && &s == &scene) continue; // skip the just-applied entity
							targets.push_back(e2);
						}
						bool anyRefreshed = false;
						for (EntityHandle t : targets) {
							if (SceneSerializer::RefreshPrefabInstance(s, t, previousSourceEntity) != entt::null) {
								anyRefreshed = true;
							}
						}
						if (anyRefreshed) s.MarkDirty();
					});
				}
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Revert All")) {
				EntityHandle replacement = SceneSerializer::RevertPrefabInstanceOverride(scene, entity.GetHandle(), {});
				if (replacement != entt::null) {
					SelectEntity(replacement);
					m_EntityOrder.clear(); m_EntityOrderDirty = true;
				}
			}
			ImGui::Separator();
		}
		else if (isSinglePrefabInstance) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.70f, 0.30f, 1.0f));
			ImGui::TextUnformatted("Prefab Instance (orphaned — source missing)");
			ImGui::PopStyleColor();
			ImGui::Separator();
		}

		// H24: bucket override paths by component-serialized-name once per
		// frame so the per-component check is O(1). Without this, the
		// `componentHasOverrides` lambda below scanned every override path
		// for every component on every frame the inspector was open
		// (O(components * paths) per render).
		//
		// Override paths follow the convention "<componentSerializedName>"
		// or "<componentSerializedName>.<field…>"; the leading token before
		// the first '.' is the component bucket.
		std::unordered_map<std::string, std::vector<std::string>> overridesByComponent;
		if (prefabSourceResolvable) {
			for (const auto& [path, _] : prefabOverrides.GetObject()) {
				const std::size_t dot = path.find('.');
				const std::string componentKey = (dot == std::string::npos)
					? path
					: path.substr(0, dot);
				overridesByComponent[componentKey].push_back(path);
			}
		}

		// Lambda: do any override paths fall under this component's serializedName?
		auto componentHasOverrides = [&](const std::string& serializedName) -> bool {
			if (!prefabSourceResolvable || serializedName.empty()) return false;
			auto it = overridesByComponent.find(serializedName);
			return it != overridesByComponent.end() && !it->second.empty();
		};

		// Collect components per their presence across the selection. Only
		// components present on EVERY selected entity are shown; components
		// present on some-but-not-all are listed in a muted footer line.
		std::vector<std::string> hiddenPartialComponents;

		registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
			if (info.category != ComponentCategory::Component) return;
			if (info.displayName == "Name") return; // Shown in entity header

			// Set-intersection check: skip when not all entities have it. If
			// some have it and others don't, remember it for the footer.
			std::size_t haveCount = 0;
			for (const Entity& e : selectedEntities) {
				if (info.has(e)) ++haveCount;
			}
			if (haveCount == 0) return;
			if (haveCount < selectedEntities.size()) {
				hiddenPartialComponents.push_back(info.displayName);
				return;
			}

			// Scripts render their own per-script sections — skip the outer wrapper
			if (info.displayName == "Scripts") {
				DispatchComponentInspector(info, entitySpan);
				return;
			}

			bool removeRequested = false;
			bool copyRequested = false;
			bool pasteRequested = false;
			bool resetRequested = false;
			ComponentClipboardPayload clipboardPayload;
			const bool hasComponentClipboard = TryReadComponentClipboard(m_ComponentClipboardJson, clipboardPayload);
			const ComponentInfo* pastedComponentInfo = hasComponentClipboard
				? FindComponentInfoBySerializedName(registry, clipboardPayload.SerializedName)
				: nullptr;
			const bool canPasteComponent = hasComponentClipboard
				&& pastedComponentInfo
				&& pastedComponentInfo->add
				&& pastedComponentInfo->has
				&& clipboardPayload.Data.IsObject();
			const bool canSerializeComponent = !info.serializedName.empty();

			const std::string componentSerializedName = info.serializedName;
			const bool thisComponentOverridden = componentHasOverrides(componentSerializedName);

			bool revertComponentRequested = false;
			bool applyComponentRequested = false;
			std::string revertFieldRequested; // empty = none

			bool open = ImGuiUtils::BeginComponentSection(info.displayName.c_str(), removeRequested, [&]() {
				if (ImGui::MenuItem("Copy Component", nullptr, false, canSerializeComponent)) {
					copyRequested = true;
				}
				if (ImGui::MenuItem("Paste Component", nullptr, false, canPasteComponent)) {
					pasteRequested = true;
				}
				if (ImGui::MenuItem("Reset Component", nullptr, false, canSerializeComponent)) {
					resetRequested = true;
				}
				// Prefab override actions — only meaningful when this entity is a
				// prefab instance whose source resolves AND this component has
				// at least one diff against the source.
				if (isSinglePrefabInstance && prefabSourceResolvable && thisComponentOverridden) {
					ImGui::Separator();
					if (ImGui::BeginMenu("Revert Field")) {
						const std::string prefix = componentSerializedName + ".";
						for (const auto& [path, _] : prefabOverrides.GetObject()) {
							if (path != componentSerializedName && path.compare(0, prefix.size(), prefix) != 0) continue;
							const char* leaf = path.c_str() + (path.size() > prefix.size() ? prefix.size() : 0);
							if (ImGui::MenuItem(leaf)) {
								revertFieldRequested = path;
							}
						}
						ImGui::EndMenu();
					}
					if (ImGui::MenuItem("Revert Component")) {
						revertComponentRequested = true;
					}
					if (ImGui::MenuItem("Apply Component")) {
						applyComponentRequested = true;
					}
				}
			});

			// Override badge — colored dot drawn next to the section header to
			// surface the diff at a glance. Sourced from EditorTheme so future
			// theme changes don't require touching the inspector.
			if (thisComponentOverridden) {
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors::OverrideMarker);
				ImGui::TextUnformatted(" *");
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Component has per-field overrides relative to the source prefab.");
				}
			}

			// Component drag source on the header. References the primary
			// entity — useful when dragging onto a script field, since that
			// field would only be able to hold one component reference anyway.
			//
			// We embed the persistent UUID (not RuntimeID): if the user drops
			// onto a script field the value is saved to disk, and RuntimeIDs
			// are reallocated on every scene reload. The drop side parses
			// this string and writes it directly into the field, so the value
			// must survive save/load.
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
				uint64_t persistentId = 0;
				if (Scene* scene = entity.GetScene()) {
					persistentId = scene->GetEntityPersistentID(entity.GetHandle());
				}
				if (persistentId == 0) {
					persistentId = entity.GetRuntimeID(); // fallback for entities with no UUIDComponent
				}
				std::string refStr = std::to_string(persistentId) + ":" + info.displayName;
				ImGui::SetDragDropPayload("COMPONENT_REF", refStr.c_str(), refStr.size() + 1);
				ImGui::Text("Ref: %s.%s", entity.GetName().c_str(), info.displayName.c_str());
				ImGui::EndDragDropSource();
			}

			if (removeRequested) {
				pendingRemoval = typeId;
			}
			// Copy serializes the primary entity. Paste / Reset apply to ALL
			// selected entities — that matches the rest of the multi-edit
			// behavior (each entity that lacks the component gets it added).
			if (copyRequested && canSerializeComponent) {
				Json::Value componentValue = SceneSerializer::SerializeComponent(scene, entity.GetHandle(), info.serializedName);
				if (!componentValue.IsNull()) {
					Json::Value root = Json::Value::MakeObject();
					root.AddMember("type", Json::Value("ComponentClipboard"));
					root.AddMember("component", Json::Value(info.serializedName));
					root.AddMember("displayName", Json::Value(info.displayName));
					root.AddMember("data", std::move(componentValue));
					m_ComponentClipboardJson = Json::Stringify(root, false);
				}
			}
			if (pasteRequested && canPasteComponent) {
				bool anyApplied = false;
				for (const Entity& e : selectedEntities) {
					if (!pastedComponentInfo->has(e)) {
						registry.AddWithDependencies(e, pastedComponentInfo->typeId);
					}
					if (SceneSerializer::DeserializeComponent(scene, e.GetHandle(),
						clipboardPayload.SerializedName, clipboardPayload.Data)) {
						anyApplied = true;
					}
				}
				if (anyApplied) scene.MarkDirty();
			}
			if (resetRequested && canSerializeComponent) {
				bool anyReset = false;
				for (const Entity& e : selectedEntities) {
					if (SceneSerializer::ResetComponent(scene, e.GetHandle(), info.serializedName)) {
						anyReset = true;
					}
				}
				if (anyReset) scene.MarkDirty();
			}

			// Prefab override operations — single-instance only (multi-select
			// composition is intentionally deferred). Revert paths destroy and
			// re-create the entity, so the selection handle is refreshed below.
			if (!revertFieldRequested.empty()) {
				EntityHandle replacement = SceneSerializer::RevertPrefabInstanceOverride(
					scene, entity.GetHandle(), revertFieldRequested);
				if (replacement != entt::null) {
					SelectEntity(replacement);
					m_EntityOrder.clear(); m_EntityOrderDirty = true;
				}
			}
			if (revertComponentRequested) {
				// Revert every override path under this component by walking the
				// captured overrides set. Each revert call destroys + recreates,
				// so we iterate paths first and only refresh the selection once.
				const std::string prefix = componentSerializedName + ".";
				std::vector<std::string> paths;
				for (const auto& [path, _] : prefabOverrides.GetObject()) {
					if (path == componentSerializedName || path.compare(0, prefix.size(), prefix) == 0) {
						paths.push_back(path);
					}
				}
				EntityHandle current = entity.GetHandle();
				for (const std::string& p : paths) {
					EntityHandle replacement = SceneSerializer::RevertPrefabInstanceOverride(scene, current, p);
					if (replacement != entt::null) current = replacement;
				}
				SelectEntity(current);
				m_EntityOrder.clear(); m_EntityOrderDirty = true;
			}
			if (applyComponentRequested) {
				// "Apply Component" pushes the whole instance state to source
				// (no per-component partial save in v1) then propagates with
				// override-preservation, matching Apply All. The granularity
				// difference vs Apply All is purely UX framing.
				const std::string prefabPath = AssetRegistry::ResolvePath(
					static_cast<uint64_t>(scene.GetPrefabGUID(entity.GetHandle())));
				Json::Value previousSourceEntity;
				bool havePrev = false;
				if (!prefabPath.empty() && File::Exists(prefabPath)) {
					Json::Value previousRoot;
					std::string parseError;
					if (Json::TryParse(File::ReadAllText(prefabPath), previousRoot, &parseError) && previousRoot.IsObject()) {
						if (const Json::Value* eb = previousRoot.FindMember("Entity")) { previousSourceEntity = *eb; havePrev = true; }
						else if (const Json::Value* lb = previousRoot.FindMember("prefab")) { previousSourceEntity = *lb; havePrev = true; }
					}
				}
				if (SceneSerializer::ApplyPrefabInstanceOverrides(scene, entity.GetHandle()) && havePrev) {
					const uint64_t prefabGuid = static_cast<uint64_t>(scene.GetPrefabGUID(entity.GetHandle()));
					SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
						std::vector<EntityHandle> targets;
						auto view = s.GetRegistry().view<EntityMetaDataComponent>();
						for (entt::entity e2 : view) {
							const auto& meta = view.get<EntityMetaDataComponent>(e2).MetaData;
							if (meta.Origin != EntityOrigin::Prefab) continue;
							if (static_cast<uint64_t>(meta.PrefabGUID) != prefabGuid) continue;
							if (e2 == entity.GetHandle() && &s == &scene) continue;
							targets.push_back(e2);
						}
						bool anyRefreshed = false;
						for (EntityHandle t : targets) {
							if (SceneSerializer::RefreshPrefabInstance(s, t, previousSourceEntity) != entt::null) {
								anyRefreshed = true;
							}
						}
						if (anyRefreshed) s.MarkDirty();
					});
				}
			}

			if (open) {
				DispatchComponentInspector(info, entitySpan);
				ImGuiUtils::EndComponentSection();
			}
		});

		if (pendingRemoval != typeid(void)) {
			// Remove from EVERY selected entity that has it.
			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (typeId != pendingRemoval || !info.remove) return;
				for (const Entity& e : selectedEntities) {
					if (info.has(e)) info.remove(e);
				}
			});
			scene.MarkDirty();
		}

		if (!hiddenPartialComponents.empty()) {
			std::string text = "Hidden: ";
			for (std::size_t i = 0; i < hiddenPartialComponents.size(); ++i) {
				if (i > 0) text += ", ";
				text += hiddenPartialComponents[i];
			}
			text += " - not on all selected";
			ImGui::TextDisabled("%s", text.c_str());
		}

		// Render the unified reference-picker popup once per inspector frame,
		// so any native component's PropertyDrawer-driven asset/entity picker
		// surfaces. The script inspector renders it inside its own draw too,
		// for the case where the panel is open without other components.
		ReferencePicker::RenderPopup();

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		float buttonWidth = ImGui::GetContentRegionAvail().x;
		if (ImGui::Button("Add Component", ImVec2(buttonWidth, 0))) {
			ImGui::OpenPopup("AddComponentPopup");
			m_ComponentSearchBuffer[0] = '\0';
		}

		// Categorized + searchable Add Component popup. Shared with the
		// asset-side prefab inspector so the UX matches whether the user
		// is editing a regular entity or a `.prefab` directly. The helper
		// hides components present on every selected entity, applies adds
		// to every entity missing the component, and runs conflict checks
		// against the selection so invariant-violating combinations are
		// disabled with a tooltip explaining why.
		RenderAddComponentPopup("AddComponentPopup", scene, entitySpan,
			m_ComponentSearchBuffer, sizeof(m_ComponentSearchBuffer));

		// Drag-drop target: accept script files dropped onto the Inspector panel
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
				std::string droppedPath(static_cast<const char*>(payload->Data));
				std::vector<EditorScriptDiscovery::ScriptEntry> droppedScripts;
				EditorScriptDiscovery::CollectScriptFile(std::filesystem::path(droppedPath), droppedScripts);
				for (const auto& scriptEntry : droppedScripts) {
					if (scriptEntry.IsGameSystem || scriptEntry.IsGlobalSystem) {
						continue;
					}
					for (const Entity& e : selectedEntities) {
						if (scriptEntry.IsManagedComponent) {
							AttachManagedComponentToEntity(const_cast<Entity&>(e), scene, scriptEntry);
						}
						else {
							AttachScriptToEntity(const_cast<Entity&>(e), scene, scriptEntry);
						}
					}
				}
			}
			ImGui::EndDragDropTarget();
		}

		// Mark the scene dirty only when ImGui reports a real value change on
		// the active widget this frame. IsAnyItemActive() would fire on mere
		// focus/click; ActiveIdHasBeenEditedThisFrame fires only on edits
		// (drag step, keystroke, etc.). Scope to this panel's focus stack.
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
			const ImGuiContext& g = *ImGui::GetCurrentContext();
			if (g.ActiveId != 0 && g.ActiveIdHasBeenEditedThisFrame) {
				scene.MarkDirty();
			}
		}
		m_IsInspectorPanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		ImGui::End();
	}
}
