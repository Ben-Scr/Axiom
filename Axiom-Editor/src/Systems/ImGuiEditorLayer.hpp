#pragma once
#include "Core/Layer.hpp"
#include "Core/Export.hpp"
#include "Collections/Color.hpp"
#include "Diagnostics/LogOverlay.hpp"
#include "Diagnostics/StatsOverlay.hpp"
#include "Scene/Entity.hpp"
#include "Scene/Scene.hpp"
#include "Collections/Ids.hpp"
#include "Collections/Viewport.hpp"
#include "Core/Log.hpp"
#include "Gui/AssetBrowser.hpp"
#include "Gui/PackageManagerPanel.hpp"
#include "Gui/PrefabInspector.hpp"
#include "Gui/ProfilerPanel.hpp"
#include "Packages/PackageManager.hpp"
#include "Editor/EditorCamera.hpp"
#include "Graphics/Texture2D.hpp"


#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

namespace Axiom {
	class ImGuiEditorLayer : public Layer {
	public:
		using Layer::Layer;

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnPreRender(Application& app) override;
		void OnUpdate(Application& app, float dt) override;
	private:
		struct LogEntry {
			std::string Message;
			Log::Level Level;
		};

		struct LogDispatchState {
			std::mutex Mutex;
			std::vector<LogEntry> PendingEntries;
		};

		struct PreviewTextureEntry {
			std::string CanonicalPath;
			std::unique_ptr<Texture2D> Texture;
			std::uint64_t LastTouchTick = 0;
		};

		struct ViewportFBO {
			unsigned int FramebufferId = 0;
			unsigned int ColorTextureId = 0;
			unsigned int DepthRenderbufferId = 0;
			Viewport ViewportSize{ 1, 1 };
		};

		void EnsureFBO(ViewportFBO& fbo, int width, int height);
		void DestroyFBO(ViewportFBO& fbo);

		void EnsureViewportFramebuffer(int width, int height);
		void DestroyViewportFramebuffer();

		void RenderDockspaceRoot();
		void RenderMainMenu(Scene& scene);
		void RenderToolbar();
		void RenderEntitiesPanel();
		void RenderInspectorPanel(Scene& scene);
		void RenderEditorView(Scene& scene);
		void RenderGameView(Scene& scene);
		void RenderLogPanel();
		void RenderProjectPanel();
		void RenderBuildPanel();
		void RenderPlayerSettingsPanel();
		void RenderSceneSystemsInspector(Scene& scene);
		void ExecuteBuild();
		void RenderPackageManagerPanel();
		void RenderAssetInspector();
		void BeginPlayModeRequest(Scene& scene);
		void CompletePlayModeEntry(Scene& scene);
		void PollPendingPlayModeRequest(Scene& scene);
		void RestoreEditorSceneAfterPlaymode();
		void SelectSceneNode();
		void SelectEntity(EntityHandle entity);
		void ClearEntitySelection();
		bool IsEntitySelected(EntityHandle entity) const;
		std::vector<EntityHandle> GetSelectedEntities(const Scene& scene) const;
		void SetSingleEntitySelection(EntityHandle entity, int index);
		void ToggleEntitySelection(EntityHandle entity, int index);
		void SelectEntityRange(int index);
		void DrainPendingLogEntries();
		void AppendLogEntry(LogEntry entry);
		void ClearLogEntries();
		void FocusSelectedEntity(Scene& scene);
		void DuplicateSelectedEntity(Scene& scene);
		void DeleteSelectedEntity(Scene& scene);
		void BeginRenameSelectedEntity(Scene& scene);
		// Convert every selected entity that's a prefab instance back into a
		// regular scene entity (drops PrefabInstanceComponent + clears the
		// prefab GUID via SetEntityMetaData).
		void UnpackSelectedPrefabs(Scene& scene);
		void CopySelectedEntities(Scene& scene);
		void PasteEntities(Scene& scene);
		bool HasEntityShortcutFocus() const;
		void DrawEditorComponentGizmos(Scene& scene);
		const Texture2D* GetPreviewTexture(const std::filesystem::path& path);
		void TrimPreviewTextureCache();
		void ClearPreviewTextureCache();

		void RenderSceneIntoFBO(ViewportFBO& fbo, Scene& scene,
			const glm::mat4& vp, const AABB& viewportAABB,
			bool withGizmos, bool sharedGizmosOnly = false,
			const Color& clearColor = Color::Background());

		EntityHandle m_SelectedEntity = entt::null;
		EntityHandle m_PressedEntity = entt::null;
		std::vector<EntityHandle> m_SelectedEntities;
		int m_LastEntitySelectionIndex = -1;
		bool m_IsSceneNodeSelected = false;
		EventId m_LogSubscriptionId{};
		std::vector<LogEntry> m_LogEntries;
		std::shared_ptr<LogDispatchState> m_LogDispatchState;
		bool m_ShowLogInfo = true;
		bool m_ShowLogWarn = true;
		bool m_ShowLogError = true;
		std::vector<PreviewTextureEntry> m_PreviewTextureCache;
		std::unordered_map<std::string, size_t> m_PreviewTextureLookup;
		std::uint64_t m_PreviewTextureTick = 0;
		static constexpr size_t kMaxPreviewTextures = 16;

		// Entity ordering for hierarchy drag-reorder
		std::vector<entt::entity> m_EntityOrder;

		EntityHandle m_RenamingEntity = entt::null;
		char m_EntityRenameBuffer[256]{};
		int m_EntityRenameFrameCounter = 0;

		ViewportFBO m_EditorViewFBO;
		EditorCamera m_EditorCamera;
		bool m_IsEditorViewHovered = false;
		bool m_IsEditorViewFocused = false;

		bool m_IsGameViewActive = false;
		bool m_IsEditorViewActive = false;
		bool m_IsEntitiesPanelFocused = false;
		bool m_IsInspectorPanelFocused = false;

		ViewportFBO m_GameViewFBO;
		bool m_IsGameViewHovered = false;
		bool m_IsGameViewFocused = false;
		int m_GameViewAspectPresetIndex = 0;
		bool m_GameViewAspectLoaded = false;
		bool m_GameViewVsync = true;
		bool m_GameViewVsyncLoaded = false;
		bool m_GameViewHasRendered = false;
		int m_LastGameViewFbW = 0;
		int m_LastGameViewFbH = 0;
		std::chrono::steady_clock::time_point m_LastGameViewRenderTime{};

		// Game View overlays. The Stats / Logs buttons next to VSync toggle
		// these flags; when on, the engine-level Diagnostics overlays draw
		// pinned to the top-right of the rendered FBO area. Both share the
		// same implementation as the runtime F6/F7 overlays. When both are
		// visible the log window stacks below the stats window.
		bool m_ShowGameViewStats = false;
		bool m_ShowGameViewLogs  = false;
		Axiom::Diagnostics::StatsOverlay m_GameViewStatsOverlay;
		// unique_ptr so LogOverlay's constructor (which subscribes to
		// Log::OnLog) only fires once Log is up. The editor's ImGuiEditorLayer
		// constructor runs before Application::Initialize on some paths;
		// we lazily new the overlay on first use to avoid that ordering risk.
		std::unique_ptr<Axiom::Diagnostics::LogOverlay> m_GameViewLogOverlay;

		Viewport m_EditorViewport{ 1, 1 };
		bool m_IsViewportHovered = false;
		bool m_IsViewportFocused = false;
		bool m_IsPlaying = false;

		AssetBrowser m_AssetBrowser;
		bool m_AssetBrowserInitialized = false;

		// Editor-side inspector for `.prefab` assets. Owns a detached editor-preview
		// scene (Scene::CreateDetachedEditorScene) where the prefab is unpacked
		// for editing. RenderAssetInspector dispatches to it when the selected
		// asset's extension is `.prefab`.
		PrefabInspector m_PrefabInspector;
		// Tracks the path the prefab inspector has currently loaded; used to
		// detect selection changes and drive the dirty-prompt dialog.
		std::string m_PrefabInspectorPath;
		// Save/discard prompt state for switching away from a dirty prefab.
		bool m_ShowPrefabSavePrompt = false;
		std::string m_PendingPrefabSwitchPath;

		std::string m_PendingSceneFileDrop;
		std::string m_PendingSceneSwitch;
		std::string m_ConfirmDialogPendingPath;
		bool m_ShowSaveConfirmDialog = false;
		char m_ComponentSearchBuffer[128]{};
		char m_SystemSearchBuffer[128]{};
		char m_GlobalSystemSearchBuffer[128]{};
		std::string m_SelectedAssetPath;
		std::string m_ComponentClipboardJson;
		std::string m_EntityClipboardJson;

		std::string m_PlayModeScenePath;
		bool m_PlayModeRecompilePending = false;
		int m_StepFrames = 0;

		bool m_ShowQuitSaveDialog = false;
		bool m_ShowBuildPanel = false;
		bool m_ShowPlayerSettings = false;
		bool m_ShowPackageManager = false;
		bool m_ShowProfiler = false;
		ProfilerPanel m_ProfilerPanel;

		// Scene list for build
		std::vector<std::string> m_BuildSceneList;
		bool m_BuildSceneListInitialized = false;
		int m_DraggedSceneIndex = -1;
		bool m_PackageManagerInitialized = false;
		PackageManager m_PackageManager;
		PackageManagerPanel m_PackageManagerPanel;
		std::string m_BuildOutputDir;
		char m_BuildOutputDirBuffer[512]{};
		char m_CustomDefineEntryBuffer[128]{}; // Build panel custom-define text input
		int m_BuildState = 0; // 0=idle, 1=pending (render overlay), 2=execute
		bool m_BuildAndPlay = false;
		std::vector<entt::entity> m_EditorPausedAudioEntities; // AudioSources paused by editor, not by gameplay
		std::chrono::steady_clock::time_point m_BuildStartTime;
	};
}
