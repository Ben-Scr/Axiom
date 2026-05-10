#include <pch.hpp>
#include "Systems/ImGuiEditorLayer.hpp"

#include <imgui.h>

#include <cstdio>

#include "Components/Components.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Graphics/Framebuffer.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Graphics/GizmoRenderer.hpp"
#include "Graphics/Gizmo.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/ComponentRegistry.hpp"
#include <Scene/EntityHelper.hpp>

#include "Graphics/TextureManager.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Serialization/Path.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/File.hpp"
#include "Utils/Process.hpp"
#include "Packages/NuGetSource.hpp"
#include "Packages/GitHubSource.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Gui/EditorIcons.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scripting/ScriptSystem.hpp"
#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace Axiom {
	// Per-panel render target lifecycle. The actual OpenGL handle juggling
	// (glGenFramebuffers, glTexImage2D, depth-stencil renderbuffer attach,
	// completeness check) lives behind `Framebuffer::Recreate` in
	// `Graphics/Framebuffer.cpp` — Stage 0 of the bgfx port carved that
	// out so this file no longer touches GL directly. The two helpers
	// below are kept so existing call sites (RenderSceneIntoFBO, layer
	// teardown) read the same as before; they're just one-line wrappers
	// over the RAII Framebuffer API now.
	void ImGuiEditorLayer::EnsureViewportFramebuffer(int width, int height) {
		m_EditorViewFBO.Recreate(width, height);
	}

	void ImGuiEditorLayer::DestroyViewportFramebuffer() {
		m_EditorViewFBO.Destroy();
	}

	void ImGuiEditorLayer::OnPreRender(Application& app) {
		(void)app;
		Scene* activeScene = SceneManager::Get().GetActiveScene();
		if (!activeScene) {
			return;
		}
		Scene& scene = *activeScene;

		// Build state machine. The 1 → 2 → 3 transition gives the
		// overlay one frame to render before we kick off the worker
		// (state 2 launches m_BuildFuture, state 3 polls it). Without
		// the 1-frame delay the user wouldn't see the overlay before
		// dotnet build's first stage hands the UI back to render.
		if (m_BuildState == 1) {
			m_BuildState = 2;
		} else if (m_BuildState == 2) {
			ExecuteBuildAsync();
			m_BuildState = 3;
		} else if (m_BuildState == 3 && m_BuildFuture.valid()
			&& m_BuildFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			// Worker finished. Drain it (rethrows any exception that
			// escaped onto the worker; we log + recover instead of
			// taking down the editor).
			try {
				m_BuildFuture.get();
			} catch (const std::exception& e) {
				AIM_ERROR_TAG("Build", "Build worker crashed: {}", e.what());
				m_BuildSucceeded.store(false, std::memory_order_relaxed);
			} catch (...) {
				AIM_ERROR_TAG("Build", "Build worker crashed: unknown exception");
				m_BuildSucceeded.store(false, std::memory_order_relaxed);
			}
			m_BuildState = 0;

			// Open Explorer at the finished build directory on the UI
			// thread. ShellExecuteA can pop modal dialogs / interact
			// with the shell on its calling thread, so doing it here
			// instead of inside the worker avoids any UI surprises and
			// matches the pre-async behaviour.
#ifdef AIM_PLATFORM_WINDOWS
			if (m_BuildSucceeded.load(std::memory_order_relaxed) && !m_BuildOutputDir.empty()) {
				ShellExecuteA(nullptr, "open", m_BuildOutputDir.c_str(),
					nullptr, nullptr, SW_SHOWNORMAL);
			}
#endif

			// Launch the built game if "Build and Play" was clicked
			if (m_BuildAndPlay && m_BuildSucceeded.load(std::memory_order_relaxed)) {
				m_BuildAndPlay = false;
				AxiomProject* project = ProjectManager::GetCurrentProject();
				if (project) {
					std::string outputDir = m_BuildOutputDirBuffer;
					if (outputDir.empty())
						outputDir = Path::Combine(project->RootDirectory, "Builds");
#ifdef AIM_PLATFORM_WINDOWS
					// Honour the project's ExecutableName override so a
					// project shipped under a marketing name ("Acme Quest"
					// instead of project ID "515") still launches via
					// Build+Play. ExecuteBuild names the produced .exe by
					// the same rule, so reading both off the project keeps
					// the two paths in lockstep.
					const std::string exeStem = project->ExecutableName.empty()
						? project->Name : project->ExecutableName;
					auto exePath = std::filesystem::path(outputDir) / (exeStem + ".exe");
					if (std::filesystem::exists(exePath)) {
						std::string cmd = "\"" + std::filesystem::canonical(exePath).string() + "\"";
						// Proper UTF-8 → wide conversion. The previous byte-wise iterator
						// construction corrupted any path containing non-ASCII bytes
						// (Unicode user folders, accented project names), causing
						// CreateProcessW to fail silently on those systems.
						std::wstring wcmd;
						{
							const int needed = MultiByteToWideChar(CP_UTF8, 0,
								cmd.c_str(), static_cast<int>(cmd.size()), nullptr, 0);
							if (needed > 0) {
								wcmd.resize(static_cast<size_t>(needed));
								MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(),
									static_cast<int>(cmd.size()), wcmd.data(), needed);
							}
						}
						std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
						buf.push_back(L'\0');
						STARTUPINFOW si{}; si.cb = sizeof(si);
						PROCESS_INFORMATION pi{};
						if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr,
							FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
							const DWORD errorCode = GetLastError();
							AIM_ERROR_TAG("Editor",
								"Failed to launch built game '{}': error {}",
								exePath.string(), errorCode);
						}
						else {
							CloseHandle(pi.hProcess);
							CloseHandle(pi.hThread);
							AIM_INFO_TAG("Build", "Launched: {}", exePath.string());
						}
					} else {
						AIM_CORE_ERROR_TAG("Build", "Built executable not found: {}", exePath.string());
					}
#endif
				}
			}
			// Reset m_BuildAndPlay even on failure so a subsequent
			// "Build" click doesn't unexpectedly auto-launch a stale
			// state from the failed run.
			m_BuildAndPlay = false;
		}

		// Process OS file drops — forward to AssetBrowser
		{
			auto* app = Application::GetInstance();
			if (app) {
				auto drops = app->TakePendingFileDrops();
				if (!drops.empty() && m_AssetBrowserInitialized) {
					m_AssetBrowser.OnExternalFileDrop(drops);
				}
			}
		}

		// Process deferred scene drop from Hierarchy drag-and-drop (blocked during Play Mode)
		if (!m_PendingSceneFileDrop.empty() && !Application::GetIsPlaying()) {
			std::string dropPath = m_PendingSceneFileDrop;
			m_PendingSceneFileDrop.clear();

			std::string sceneName = std::filesystem::path(dropPath).stem().string();
			auto& sm = SceneManager::Get();
			if (!sm.HasSceneDefinition(sceneName))
				sm.RegisterScene(sceneName);
			if (!sm.IsSceneLoaded(sceneName)) {
				auto weakScene = sm.LoadSceneAdditive(sceneName);
				if (auto loaded = weakScene.lock()) {
					SceneSerializer::LoadFromFile(*loaded, dropPath);
					m_EntityOrder.clear(); m_EntityOrderDirty = true;
				}
			}
		}

		// Process deferred scene switch (blocked during Play Mode)
		if (!m_PendingSceneSwitch.empty() && !Application::GetIsPlaying()) {
			std::string switchPath = m_PendingSceneSwitch;
			m_PendingSceneSwitch.clear();

			Scene* active = SceneManager::Get().GetActiveScene();
			if (active) {
				SceneSerializer::LoadFromFile(*active, switchPath);
				m_EntityOrder.clear(); m_EntityOrderDirty = true;
				AxiomProject* project = ProjectManager::GetCurrentProject();
				if (project) {
					project->LastOpenedScene = std::filesystem::path(switchPath).stem().string();
					project->Save();
				}
			}
		}

		// Intercept Asset Browser scene load (blocked during Play Mode)
		if (!Application::GetIsPlaying()) {
			std::string pendingLoad = m_AssetBrowser.TakePendingSceneLoad();
			if (!pendingLoad.empty()) {
				Scene* active = SceneManager::Get().GetActiveScene();
				if (active && active->IsDirty()) {
					m_ConfirmDialogPendingPath = pendingLoad;
					m_ShowSaveConfirmDialog = true;
				} else {
					m_PendingSceneSwitch = pendingLoad;
				}
			}
		}

		// Intercept Asset Browser prefab double-click → enter prefab-edit
		// mode. Blocked during Play Mode (same as scene load) — editing
		// a prefab while a game is running would corrupt running scripts'
		// view of selection.
		if (!Application::GetIsPlaying()) {
			std::string pendingPrefabEdit = m_AssetBrowser.TakePendingPrefabEdit();
			if (!pendingPrefabEdit.empty()) {
				OpenPrefabForEditing(pendingPrefabEdit);
			}
		}

		// Ctrl+S: routes to whatever the editor is currently editing.
		// Priority: full prefab-edit mode > asset-side prefab inspector > active scene.
		// All three paths are blocked during play mode so a stray Ctrl+S doesn't
		// persist transient play-mode state. Pass `repeat=false` so holding the
		// key down doesn't re-fire Save every frame (which would hammer disk +
		// scene serialization at the OS key-repeat rate).
		if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && !Application::GetIsPlaying()) {
			if (IsInPrefabEditMode()) {
				// Full prefab-edit mode: save the detached scene back to its
				// .prefab without exiting edit mode. Without this branch,
				// Ctrl+S would save the (hidden) active scene instead of the
				// thing the user is actually looking at.
				SavePrefabEditChanges();
			}
			else if (m_PrefabInspector.IsOpen() && m_PrefabInspector.HasUnsavedChanges()) {
				m_PrefabInspector.Save();
			}
			else {
				Scene* active = SceneManager::Get().GetActiveScene();
				AxiomProject* project = ProjectManager::GetCurrentProject();
				if (active && project) {
					std::string scenePath = project->GetSceneFilePath(active->GetName());
					SceneSerializer::SaveToFile(*active, scenePath);
					project->LastOpenedScene = active->GetName();
					project->Save();
				}
			}
		}

		// Intercept quit request: exit playmode first, then check for unsaved changes
		if (Application::IsQuitRequested()) {
			// Exit playmode and restore scene first
			if (Application::GetIsPlaying()) {
				RestoreEditorSceneAfterPlaymode();
			}

			// If the user is mid-prefab-edit with unsaved changes, auto-save
			// before exiting. Losing prefab edits to a stray Cmd-W is the same
			// data-loss class as losing scene edits, but layering another
			// modal on top of the scene quit dialog gets noisy fast — auto-
			// save is the safer default here, and matches the
			// switch-prefab-while-editing path above.
			if (m_PrefabEditScene && m_PrefabEditScene->IsDirty()) {
				SavePrefabEditChanges();
			}
			// Always tear down prefab edit mode on quit so its detached
			// scene's destructors run before SceneManager / static state
			// cleanup, regardless of dirty status.
			if (m_PrefabEditScene) {
				ClosePrefabEditing(false);
			}

			Scene* active = SceneManager::Get().GetActiveScene();
			if (active && active->IsDirty()) {
				m_ShowQuitSaveDialog = true;
				Application::CancelQuit();
			}
		}

		RenderDockspaceRoot();
		RenderMainMenu(scene);

		// Save confirmation modal dialog (scene switch)
		if (m_ShowSaveConfirmDialog) {
			ImGui::OpenPopup("Save Changes?");
			m_ShowSaveConfirmDialog = false;
		}
		if (ImGui::BeginPopupModal("Save Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			Scene* active = SceneManager::Get().GetActiveScene();
			std::string activeName = active ? active->GetName() : "Scene";
			ImGui::Text("Save changes to %s before opening?", activeName.c_str());
			ImGui::Spacing();

			if (ImGui::Button("Save", ImVec2(100, 0))) {
				if (active) {
					AxiomProject* project = ProjectManager::GetCurrentProject();
					if (project) {
						std::string savePath = project->GetSceneFilePath(active->GetName());
						SceneSerializer::SaveToFile(*active, savePath);
					}
				}
				m_PendingSceneSwitch = m_ConfirmDialogPendingPath;
				m_ConfirmDialogPendingPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Don't Save", ImVec2(100, 0))) {
				m_PendingSceneSwitch = m_ConfirmDialogPendingPath;
				m_ConfirmDialogPendingPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				m_ConfirmDialogPendingPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// Prefab-EDIT discard modal — fired when the user clicks "< Back"
		// in the hierarchy toolbar while the prefab edit scene has
		// unsaved changes. Distinct from the asset-inspector save prompt
		// below: that one fires on .prefab→.prefab selection change in
		// the asset browser; this one fires on full-edit-mode exit.
		if (m_ShowPrefabEditDiscardPrompt) {
			ImGui::OpenPopup("Discard Prefab Edits?");
			m_ShowPrefabEditDiscardPrompt = false;
		}
		if (ImGui::BeginPopupModal("Discard Prefab Edits?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			std::string editPrefabName = std::filesystem::path(m_PrefabEditPath).filename().string();
			if (editPrefabName.empty()) editPrefabName = "the prefab";
			ImGui::Text("Save changes to %s before closing?", editPrefabName.c_str());
			ImGui::Spacing();

			if (ImGui::Button("Save", ImVec2(100, 0))) {
				ClosePrefabEditing(true);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Don't Save", ImVec2(100, 0))) {
				ClosePrefabEditing(false);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				// Stay in edit mode; modal closes without altering state.
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// Prefab save/discard modal — fired when the user picks a different
		// asset while a dirty prefab is open in the inspector.
		if (m_ShowPrefabSavePrompt) {
			ImGui::OpenPopup("Save Prefab Changes?");
			m_ShowPrefabSavePrompt = false;
		}
		if (ImGui::BeginPopupModal("Save Prefab Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			std::string prefabName = std::filesystem::path(m_PrefabInspector.GetCurrentPath()).filename().string();
			if (prefabName.empty()) prefabName = "the prefab";
			ImGui::Text("Save changes to %s before switching?", prefabName.c_str());
			ImGui::Spacing();

			if (ImGui::Button("Save", ImVec2(100, 0))) {
				m_PrefabInspector.Save();
				if (!m_PendingPrefabSwitchPath.empty()) {
					m_PrefabInspector.Open(m_PendingPrefabSwitchPath);
					m_PrefabInspectorPath = m_PendingPrefabSwitchPath;
					m_PendingPrefabSwitchPath.clear();
				}
				else {
					m_PrefabInspector.Close();
					m_PrefabInspectorPath.clear();
				}
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Don't Save", ImVec2(100, 0))) {
				if (!m_PendingPrefabSwitchPath.empty()) {
					m_PrefabInspector.Open(m_PendingPrefabSwitchPath);
					m_PrefabInspectorPath = m_PendingPrefabSwitchPath;
					m_PendingPrefabSwitchPath.clear();
				}
				else {
					m_PrefabInspector.Close();
					m_PrefabInspectorPath.clear();
				}
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				m_PendingPrefabSwitchPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// Quit confirmation modal dialog
		if (m_ShowQuitSaveDialog) {
			ImGui::OpenPopup("Save Before Quit?");
			m_ShowQuitSaveDialog = false;
		}
		if (ImGui::BeginPopupModal("Save Before Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			Scene* active = SceneManager::Get().GetActiveScene();
			std::string activeName = active ? active->GetName() : "Scene";
			ImGui::Text("Save changes to %s before closing?", activeName.c_str());
			ImGui::Spacing();

			if (ImGui::Button("Save", ImVec2(100, 0))) {
				if (active) {
					AxiomProject* project = ProjectManager::GetCurrentProject();
					if (project) {
						std::string savePath = project->GetSceneFilePath(active->GetName());
						SceneSerializer::SaveToFile(*active, savePath);
						project->LastOpenedScene = active->GetName();
						project->Save();
					}
				}
				ImGui::CloseCurrentPopup();
				Application::ConfirmQuit();
			}
			ImGui::SameLine();
			if (ImGui::Button("Don't Save", ImVec2(100, 0))) {
				ImGui::CloseCurrentPopup();
				Application::ConfirmQuit();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0))) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		// Suppress script recompilation while a script is being created/renamed
		if (scene.HasSystem<ScriptSystem>()) {
			auto* scriptSys = scene.GetSystem<ScriptSystem>();
				if (scriptSys)
					scriptSys->SetRecompileSuppressed(m_AssetBrowser.IsCreatingScript());
		}

		PollPendingPlayModeRequest(scene);

		// Reset every frame; the Game View panel republishes its rect
		// while it's visible. If the user collapsed the Game View tab
		// or closed the dock, we want UI systems to fall back to the
		// OS window viewport instead of using a stale region.
		Window::ClearUIRegion();

		RenderToolbar();
		RenderEntitiesPanel();
		// Inspector + editor view follow the prefab-edit override when
		// active so component edits and viewport rendering both target
		// the detached prefab scene. Game View intentionally stays on
		// the real active scene so the user can still preview play.
		Scene* contextScene = GetContextScene();
		RenderInspectorPanel(contextScene ? *contextScene : scene);
		RenderEditorView(contextScene ? *contextScene : scene);
		RenderGameView(scene);
		RenderProjectPanel();
		RenderLogPanel();
		RenderBuildPanel();
		RenderPlayerSettingsPanel();
		RenderProjectSettingsPanel();
		RenderPackageManagerPanel();

		// Splash preview — replays the runtime's splash timeline as
		// a foreground overlay over the editor when the user clicks
		// "Show Preview" in the Splash Screen settings. Self-completes
		// after FadeIn + Duration + FadeOut seconds; no-op when
		// inactive.
		TickSplashPreview();

		// Profiler panel — Ctrl+F6 toggle. Panel manages its own visibility-gating internally.
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F6)) {
			m_ShowProfiler = !m_ShowProfiler;
		}
		if (m_ShowProfiler) {
			m_ProfilerPanel.Render(&m_ShowProfiler);
		}

		// Build progress overlay. While the worker is running we read
		// the atomic progress + the locked stage label and feed both
		// into a real progress bar. Pre-async this used a fake
		// fmod(elapsed)-driven indeterminate animation that always
		// hovered around 0–1% from the user's perspective; the worker
		// now reports actual stage completion.
		if (m_BuildState > 0) {
			ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f,
				viewport->Pos.y + viewport->Size.y * 0.5f);

			ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(360, 110));
			ImGui::SetNextWindowBgAlpha(0.92f);

			ImGuiWindowFlags overlayFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
				| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
				| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;

			ImGui::Begin("##BuildOverlay", nullptr, overlayFlags);
			ImGui::TextUnformatted("Building Project...");
			ImGui::Spacing();

			std::string stage;
			{
				std::lock_guard<std::mutex> lk(m_BuildProgressMutex);
				stage = m_BuildStage;
			}
			const float progress = m_BuildProgress.load(std::memory_order_relaxed);
			ImGui::ProgressBar(progress, ImVec2(-1, 0), "");
			ImGui::TextDisabled("%s", stage.empty() ? "Working..." : stage.c_str());
			ImGui::End();
		}

		// Script rebuild overlay — rendered editor-side so the engine has no ImGui dep.
		if (scene.HasSystem<ScriptSystem>()) {
			auto* scriptSys = scene.GetSystem<ScriptSystem>();
			if (scriptSys && (scriptSys->IsScriptRebuildRunning() || scriptSys->IsNativeRebuildRunning())) {
				ImGuiViewport* viewport = ImGui::GetMainViewport();
				ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f,
					viewport->Pos.y + viewport->Size.y * 0.5f);

				ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
				ImGui::SetNextWindowSize(ImVec2(320, 80));
				ImGui::SetNextWindowBgAlpha(0.92f);

				ImGuiWindowFlags overlayFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
					| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
					| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;

				ImGui::Begin("##ScriptBuildOverlay", nullptr, overlayFlags);
				ImGui::TextUnformatted(scriptSys->IsNativeRebuildRunning() ? "Compiling Native Scripts..." : "Compiling Scripts...");
				ImGui::Spacing();
				const float elapsed = scriptSys->GetActiveRebuildElapsedSeconds();
				ImGui::ProgressBar(fmodf(elapsed * 0.4f, 1.0f), ImVec2(-1, 0), "");
				ImGui::End();
			}
		}

		ImGui::End(); // Close dockspace opened in RenderDockspaceRoot.
	}
}
