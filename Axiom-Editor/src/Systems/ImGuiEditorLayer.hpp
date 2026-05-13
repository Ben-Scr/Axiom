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
#include "Graphics/Framebuffer.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureHandle.hpp"


#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>

namespace Axiom {

	// Editor-view debug draw mode (toolbar dropdown). Default renders
	// the scene normally; Triangle switches glPolygonMode to GL_LINE so
	// every quad is drawn as its two constituent triangles' edges only;
	// Mixed runs the full scene render twice — once normally, once in
	// wireframe — so the user can see the geometry on top of the
	// shaded result. Affects only the Editor View FBO; the Game View
	// always renders Default.
	enum class EditorViewDrawMode : uint8_t {
		Default = 0,
		Triangle,
		Mixed,
	};

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

		// Per-panel render targets are RAII-managed Framebuffers — see
		// `Graphics/Framebuffer.hpp`. Resize is `m_*FBO.Recreate(w, h)`,
		// destruction is automatic on layer teardown.
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
		void RenderProjectSettingsPanel();
		// Splash preview overlay. Drawn on top of the dockspace with an
		// ImGui foreground draw list so the editor stays interactive
		// underneath; the preview self-completes after FadeIn +
		// Duration + FadeOut seconds.
		void TickSplashPreview();
		void RenderSceneSystemsInspector(Scene& scene);
		void ExecuteBuild();
		// Async wrapper. Captures the project + output dir on the UI
		// thread (file access is fine here), then dispatches the bulk
		// of the build (process spawn, file copies, icon embedding) to
		// a worker. The returned future completes when the worker has
		// finished — RenderBuildPanel polls it once per frame.
		void ExecuteBuildAsync();
		// Public-to-private helper: post a stage / progress update
		// from the build worker. Thread-safe; the UI reads the values
		// each frame.
		void ReportBuildProgress(float progress, std::string_view stage);
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
		void RunAutoSaveTick(Application& app, float dt);
		void AppendLogEntry(LogEntry entry);
		void ClearLogEntries();
		void ResetEditorFocusCycle();
		void UpdateEditorCameraFocus(float dt);
		bool TryBuildEntityAABB(Scene& scene, EntityHandle entity, bool includeChildren, AABB& outAABB);
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
		EntityHandle RenderCreateEntityMenu(Scene& scene, Entity parent);
		std::string MakeEditorUniqueEntityName(Scene& scene, std::string_view baseName, EntityHandle ignoreEntity = entt::null) const;
		void EnsureEditorUniqueEntityName(Scene& scene, EntityHandle entity);
		void EnsureEditorUniqueEntityNames(Scene& scene, const std::vector<EntityHandle>& roots);
		bool SetEntityParentPreservingWorld(Scene& scene, EntityHandle child, Entity parent);
		// Reorders `dragged` so it becomes the immediate previous (insertAfter=false)
		// or next (insertAfter=true) sibling of `target`. Reparents across
		// branches when needed. Mutates HierarchyComponent::Children for
		// non-root targets and m_EntityOrder for root-level targets so the
		// hierarchy panel reflects the new order. Returns true on a real
		// reorder (caller should mark scene dirty).
		bool MoveSiblingNextTo(Scene& scene, EntityHandle dragged, EntityHandle target, bool insertAfter);

		// Resolves a hierarchy drag-drop primary handle into the full set of
		// entities the gesture should affect. When `primary` is part of the
		// current multi-selection we return the entire (hierarchy-root-
		// filtered) selection in selection order; otherwise we return just
		// `primary`. Filters stale handles. The drag payload itself only
		// carries the primary entity, so the drop-side resolves the full
		// set against the editor's selection state at drop time.
		std::vector<EntityHandle> ResolveDraggedHierarchyEntities(Scene& scene, EntityHandle primary) const;

		// ── Prefab edit mode ──────────────────────────────────────────
		// Loads `path` into a detached scene and switches the editor's
		// hierarchy panel + viewport to that scene. The main scene stays
		// loaded but is hidden until ClosePrefabEditing runs. Returns
		// false when the file can't be read or parsed; the editor stays
		// in scene-edit mode in that case.
		bool OpenPrefabForEditing(const std::string& path);
		// `save=true` writes the detached scene back to its source path
		// before tearing the prefab scene down. Either way, returns the
		// editor to scene-edit mode and clears the prefab selection.
		void ClosePrefabEditing(bool save);
		// Persist the in-flight prefab edits to disk and propagate to
		// live instances WITHOUT exiting prefab edit mode. Used by the
		// "Save" button in the hierarchy toolbar so the user can keep
		// iterating on the prefab between explicit save points.
		bool SavePrefabEditChanges();
		// True while a detached prefab scene is the editor's focus. The
		// hierarchy / viewport / inspector all consult this to decide
		// whether to operate on the detached scene or the active scene.
		bool IsInPrefabEditMode() const { return m_PrefabEditScene != nullptr; }
		// Returns the scene the editor is currently driving — the prefab
		// edit scene when in prefab mode, otherwise the active scene.
		Scene* GetContextScene() const;

		bool HasEntityShortcutFocus() const;
		void DrawEditorComponentGizmos(Scene& scene);
		const Texture2D* GetPreviewTexture(const std::filesystem::path& path);
		void TrimPreviewTextureCache();
		void ClearPreviewTextureCache();

		// `onlyPassedScene` skips the SceneManager loop and renders only
		// the explicit `scene` argument. Used by prefab-edit mode so the
		// detached prefab scene doesn't get its visuals composited with
		// the still-loaded "real" scene underneath.
		//
		// `uiInWorldSpace` switches the GuiRenderer pass from its default
		// screen-space ortho to a world-space projection through `vp`.
		// Used by the Editor View so UI rects pan/zoom with the editor
		// camera (and selection gizmos line up); the Game View leaves
		// it false because runtime UI is screen-locked by design.
		void RenderSceneIntoFBO(Framebuffer& fbo, Scene& scene,
			const glm::mat4& vp, const AABB& viewportAABB,
			bool withGizmos, bool sharedGizmosOnly = false,
			const Color& clearColor = Color::Background(),
			bool onlyPassedScene = false,
			bool uiInWorldSpace = false,
			EditorViewDrawMode drawMode = EditorViewDrawMode::Default);

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

		// M30: hierarchy panel was rebuilding the registry-sync diff and
		// the DFS subtree-emit pass on EVERY frame. The user-visible order
		// usually doesn't change between frames, so cache the most-recently-
		// rendered flat list + per-row depths, and only rebuild when an
		// upstream signal flips m_EntityOrderDirty. New entities pushed onto
		// m_EntityOrder, scene swap, or any other structural change all
		// flip this flag; per-frame ImGui interaction (selection, expand/
		// collapse) does not.
		bool m_EntityOrderDirty = true;
		std::vector<entt::entity> m_RenderedEntityOrder;
		std::vector<int> m_RenderedEntityDepths;
		std::vector<entt::entity> m_VisibleEntityOrder;

		// ── Prefab edit mode ──────────────────────────────────────────
		// Owned detached scene that contains the entity tree of the
		// .prefab being edited. `nullptr` when the editor is in normal
		// scene-edit mode. Built via Scene::CreateDetachedScene so the
		// physics/audio/script subsystems aren't touched by it.
		std::unique_ptr<Scene> m_PrefabEditScene;
		std::string m_PrefabEditPath;
		EntityHandle m_PrefabEditRootEntity = entt::null;
		bool m_PrefabEditDirty = false;
		// Snapshot of the scene-edit editor camera taken at OpenPrefabForEditing
		// time. Restored on ClosePrefabEditing so the user returns to exactly
		// the view they had before opening the prefab — the prefab's authored
		// origin is usually nowhere near the active scene's framing, and forcing
		// the user to manually pan back to where they were is jarring. The
		// in-between camera mutations (the auto-focus on the prefab root, and
		// any user pan/zoom while editing the prefab) write through to the
		// shared m_EditorCamera and are discarded by this restore path.
		Vec2 m_PrefabEditSavedCameraPosition{ 0.0f, 0.0f };
		float m_PrefabEditSavedCameraOrthoSize = 5.0f;
		float m_PrefabEditSavedCameraZoom = 1.0f;
		bool m_PrefabEditHasSavedCameraState = false;
		// Discard-confirmation modal state. Fired when the user clicks
		// "< Back" (or otherwise tries to leave) while the prefab edit
		// scene has unsaved changes. The user can Save+Close, Discard
		// (close without save), or Cancel (stay in edit mode).
		bool m_ShowPrefabEditDiscardPrompt = false;
		// Override clear color for the editor viewport while in prefab-
		// edit mode. The blue tint matches the user-supplied palette and
		// clearly differentiates prefab editing from scene editing.
		static constexpr float k_PrefabEditClearR = 0.13f;
		static constexpr float k_PrefabEditClearG = 0.21f;
		static constexpr float k_PrefabEditClearB = 0.32f;

		// Hierarchy panel: entities whose subtrees are folded shut. Stored as
		// raw uint32 keys so destroyed entities don't dangle as enttity handles;
		// stale entries are harmless (they never match a live id) and only
		// risk a freshly-recycled id starting collapsed.
		std::unordered_set<uint32_t> m_CollapsedHierarchyEntities;

		EntityHandle m_RenamingEntity = entt::null;
		char m_EntityRenameBuffer[256]{};
		int m_EntityRenameFrameCounter = 0;

		Framebuffer m_EditorViewFBO;
		EditorCamera m_EditorCamera;
		bool m_IsEditorViewHovered = false;
		bool m_IsEditorViewFocused = false;
		EditorViewDrawMode m_EditorViewDrawMode = EditorViewDrawMode::Default;
		bool m_EditorCameraFocusActive = false;
		Vec2 m_EditorCameraFocusTarget{ 0.0f, 0.0f };
		float m_EditorCameraFocusOrthoSize = 5.0f;
		EntityHandle m_FocusLastEntity = entt::null;
		bool m_FocusNextPressTight = false;

		bool m_IsGameViewActive = false;
		bool m_IsEditorViewActive = false;
		bool m_IsEntitiesPanelFocused = false;
		bool m_IsInspectorPanelFocused = false;

		Framebuffer m_GameViewFBO;
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

		// Editor-side inspector for `.prefab` assets. Owns a detached preview
		// scene (Scene::CreateDetachedScene) where the prefab is unpacked
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

		// Auto-save: accumulates real time since the last successful save
		// of a dirty active scene. Reset on save (manual or auto), or when
		// the active scene swaps. Driven by Project.AutoSaveScenes /
		// AutoSaveIntervalSeconds — see RunAutoSaveTick.
		float m_AutoSaveAccumulator = 0.0f;
		std::string m_AutoSaveLastScenePath;

		bool m_ShowQuitSaveDialog = false;
		bool m_ShowBuildPanel = false;
		bool m_ShowPlayerSettings = false;
		bool m_ShowPackageManager = false;
		bool m_ShowProfiler = false;
		ProfilerPanel m_ProfilerPanel;

		// Scene list for build. Re-synced with disk every frame in
		// RenderBuildPanel so newly-imported scenes auto-appear; manual
		// drag-drop reordering is preserved across syncs.
		std::vector<std::string> m_BuildSceneList;
		int m_DraggedSceneIndex = -1;
		bool m_ShowProjectSettings = false;
		bool m_PackageManagerInitialized = false;
		// Splash preview state. Set by the Show Preview button in the
		// Player Settings panel; consumed by the editor's chrome update
		// each frame to advance + render the preview overlay. Mirrors
		// the runtime's RuntimeSplashLayer timeline (fade in → hold →
		// fade out) but draws on top of the editor's main viewport
		// instead of locking the application.
		bool m_SplashPreviewRequest = false;
		bool m_SplashPreviewActive = false;
		float m_SplashPreviewElapsed = 0.0f;
		TextureHandle m_SplashPreviewLogo;
		TextureHandle m_SplashPreviewBackground;
		std::uint32_t m_SplashPreviewTexRefToken = 0;
		PackageManager m_PackageManager;
		PackageManagerPanel m_PackageManagerPanel;
		std::string m_BuildOutputDir;
		char m_BuildOutputDirBuffer[512]{};
		char m_CustomDefineEntryBuffer[128]{}; // Build panel custom-define text input
		// Build state machine:
		//   0 = idle
		//   1 = pending (render overlay one frame so the user sees it)
		//   2 = launch worker (transition only — kicks off m_BuildFuture, becomes 3)
		//   3 = running (worker thread alive, polled by RenderBuildPanel)
		int m_BuildState = 0;
		bool m_BuildAndPlay = false;
		// Cross-thread build progress. ExecuteBuildAsync runs on
		// m_BuildThread / m_BuildFuture and writes to these atomics +
		// the message string under m_BuildProgressMutex; the UI thread
		// reads them every frame to drive the progress bar / status
		// label, then polls m_BuildFuture::wait_for(0) to detect
		// completion. Atomics keep the per-frame UI read lock-free.
		std::future<void> m_BuildFuture;
		std::atomic<float> m_BuildProgress{ 0.0f };
		std::atomic<bool>  m_BuildSucceeded{ true };
		std::mutex m_BuildProgressMutex;
		std::string m_BuildStage;     // protected by m_BuildProgressMutex
		std::vector<entt::entity> m_EditorPausedAudioEntities; // AudioSources paused by editor, not by gameplay
		std::chrono::steady_clock::time_point m_BuildStartTime;
	};
}
