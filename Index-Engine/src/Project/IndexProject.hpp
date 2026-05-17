#pragma once
#include "Core/Export.hpp"
#include "Graphics/Text/FontHandle.hpp"
#include <cstdint>
#include <string>
#include <functional>
#include <string_view>
#include <vector>

namespace Index {

	struct INDEX_API IndexProject {
		using CreateProgressCallback = std::function<void(float progress, std::string_view stage)>;

		struct GlobalSystemRegistration {
			std::string ClassName;
			bool Active = true;
		};

		std::string Name;
		std::string RootDirectory;
		std::string AssetsDirectory;
		std::string ScriptsDirectory;
		std::string ScenesDirectory;
		std::string IndexAssetsDirectory;
		std::string NativeScriptsDir;
		std::string NativeSourceDir;
		std::string PackagesDirectory;
		std::string CsprojPath;
		std::string SlnPath;
		std::string ProjectFilePath;
		std::string EngineVersion;

		// Persistence
		std::string StartupScene = "SampleScene";
		std::string LastOpenedScene = "SampleScene";
		std::string GameViewAspect = "16:9";
		bool GameViewVsync = true;

		// Build settings
		int BuildWidth = 1920;
		int BuildHeight = 1080;
		bool BuildFullscreen = true;
		bool BuildResizable = true;

		// Optional override for the produced .exe filename. Empty = use the
		// project Name (the historical behaviour). Stored without an
		// extension; the build step appends the platform extension. Useful
		// when shipping under a marketing name that differs from the
		// internal project identifier ("MyGame" project -> "Acme Quest.exe").
		std::string ExecutableName;

		// UI scaling — applied by UILayoutSystem to every RectTransform2D
		// so SizeDelta and AnchoredPosition values are interpreted as
		// "reference pixels" at the resolution the UI was authored for.
		// At runtime, the layout multiplies them by `currentSize /
		// referenceSize` so a 100-px button stays visually 100 reference-
		// pixels wide regardless of the actual window size.
		//
		// UIReferenceWidth / Height: the resolution the UI was authored
		// at. Defaults to the build resolution (1280x720) so a freshly-
		// created project's preview matches its shipped size.
		//
		// UIScaleMatch (0..1): controls how the scale factor blends
		// between width-driven and height-driven scaling. Same model as
		// Unity's CanvasScaler "Match Width Or Height":
		//   • 0.0 → only width matters (UI grows with window width).
		//   • 1.0 → only height matters (UI grows with window height).
		//   • 0.5 → balanced (geometric mean of width and height ratios).
		// 0.5 is a sensible default for most layouts; bump toward 1.0 for
		// HUDs anchored to the top/bottom edges, toward 0.0 for side-bar
		// menus.
		int UIReferenceWidth = 1920;
		int UIReferenceHeight = 1080;
		float UIScaleMatch = 0.5f;
		std::string AppIconPath;
		std::vector<std::string> BuildSceneList;
		std::vector<GlobalSystemRegistration> GlobalSystems;

		// Names of packages (engine-shipped or project-local) that this project includes.
		// The premake loader filters manifests by this list when --index-project=<path>
		// is set. Empty vector + missing JSON field == legacy "scan everything" mode.
		std::vector<std::string> Packages;

		// Profiler — per-project preferences. Read at Load(), written at Save().
		// All fields are optional in index-project.json; missing values keep the
		// defaults below. The "ModuleEnabled" map is only populated for modules
		// the user has explicitly toggled (so newly-added engine modules default
		// to enabled without forcing a settings migration).
		struct ProfilerSettings {
			bool EnableInRuntime = false; // Ctrl+F6 panel in shipped runtime; off by default
			bool TrackInBackground = false; // collect even with the panel closed
			int  SamplingHz = 60;
			int  TrackingSpan = 200;
			std::vector<std::pair<std::string, bool>> ModuleEnabled;
		} Profiler;

		// Runtime stats overlay (F6 in built games). When true, the runtime
		// pushes RuntimeStatsLayer at startup so the user can toggle the
		// FPS / tris / memory / audio overlay with F6. When false the layer
		// isn't pushed at all — zero overhead in the shipped exe. Default
		// true so the diagnostic is available out of the box; ship-ready
		// builds can flip it off via Project Settings.
		bool ShowRuntimeStats = true;

		// Runtime log overlay (F7 in built games). Same model as
		// ShowRuntimeStats: the runtime pushes RuntimeLogLayer at startup
		// when this is true. When both overlays are visible, the log
		// window stacks below the stats window so they don't overlap.
		bool ShowRuntimeLogs = true;

		// AutoSaveScenes / AutoSaveIntervalSeconds moved to user-scoped
		// EditorPreferences (2026-05). Legacy index-project.json values
		// are migrated to EditorPreferences on Load and dropped on next
		// Save — see IndexProject::Load.
		bool AutoRecompileScripts = true;
		bool RecompileScriptsOnPlay = false;

		enum class ProjectAssetSerializationFormat : uint8_t {
			Json = 0,
			Binary = 1,
		};
		ProjectAssetSerializationFormat AssetSerializationFormat = ProjectAssetSerializationFormat::Binary;

		enum class EditorEntityNameSuffixStyle : uint8_t {
			SpaceNumber = 0,       // Entity 1
			ParenthesizedNumber,   // Entity (1)
			HyphenNumber,          // Entity-1
			UnderscoreNumber,      // Entity_1
		};

		bool EditorEnsureUniqueEntityNames = true;
		EditorEntityNameSuffixStyle EditorEntityNameSuffix = EditorEntityNameSuffixStyle::ParenthesizedNumber;
		EditorEntityNameSuffixStyle EditorAssetDuplicateSuffix = EditorEntityNameSuffixStyle::ParenthesizedNumber;

		// ShowFileExtensions moved to user-scoped EditorPreferences
		// (2026-05). Legacy index-project.json values migrate on Load
		// and drop from the project file on next Save — see
		// LegacyEditorPrefs below.

		// Pre-2026-05 projects stored ShowFileExtensions / AutoSaveScenes
		// / AutoSaveIntervalSeconds inline. Load() parses them into this
		// transient struct (one `*Present` flag per value); the editor
		// migrates them into EditorPreferences exactly once on first
		// launch (gated by EditorPreferences::WasFreshlyCreated). Save()
		// never re-emits these fields, so the next Save drops them from
		// the project file entirely. Members default to "not present" so
		// new projects skip the migration path automatically.
		struct LegacyEditorPrefsMigration {
			bool ShowFileExtensions = false;
			bool ShowFileExtensionsPresent = false;
			bool AutoSaveScenes = false;
			bool AutoSaveScenesPresent = false;
			float AutoSaveIntervalSeconds = 120.0f;
			bool AutoSaveIntervalSecondsPresent = false;
		} LegacyEditorPrefs;

		// Custom cursor images. Both are project-relative paths (same
		// convention as AppIconPath), loaded via TextureManager into a
		// `Window` cursor at init. Empty = use the OS default.
		//   • CursorImagePath — applied as the default cursor (always-on).
		//   • UIInteractableCursorImagePath — swapped in while the cursor
		//     hovers an Index UI Interactable element (Buttons, Sliders,
		//     etc.). When empty, the default cursor stays active over UI
		//     too. UIEventSystem flips between the two each frame.
		std::string CursorImagePath;
		std::string UIInteractableCursorImagePath;

		uint64_t DefaultFontAssetId = k_DefaultFontAssetId;

		// Build profile — drives compile-time defines emitted to user
		// scripts at build time:
		//   • Development → INDEX_BUILD_DEVELOPMENT (and toggle defaults
		//     for ShowRuntimeStats/ShowRuntimeLogs are ON)
		//   • Release     → INDEX_BUILD_RELEASE (defaults OFF)
		// Independent of MSBuild's Debug/Release/Dist (which controls
		// ENGINE compilation). Set in the Build panel; default is
		// Development so iteration-day defaults are sensible.
		enum class BuildProfile : uint8_t {
			Development = 0,
			Release     = 1,
		};
		BuildProfile ActiveBuildProfile = BuildProfile::Development;

		// Preferred native backend WebGPU (Dawn) routes through at
		// adapter-request time. Auto lets Dawn pick the most capable
		// backend for the host platform (D3D12 on Windows, Metal on
		// macOS, Vulkan on Linux). Selecting an unsupported backend
		// (e.g. D3D12 on Linux) falls back to Auto with a warning so
		// users can carry a project across machines without it failing
		// to boot.
		enum class RenderBackend : uint8_t {
			Auto       = 0,
			Vulkan     = 1,
			Direct3D11 = 2,
			Direct3D12 = 3,
			OpenGL     = 4,
			Metal      = 5,
			OpenGLES   = 6,
		};
		RenderBackend ActiveRenderBackend = RenderBackend::Auto;

		// File-stem of the currently-selected `.indexbuild` profile under
		// <RootDirectory>/BuildProfiles/. Empty when no profile is selected
		// (legacy projects, or freshly-created ones before the user opens
		// the Build Profiles panel). The Build panel reads this to drive
		// the Build button's target platform + render backend, and falls
		// back to ActiveRenderBackend / Windows when empty.
		std::string ActiveBuildProfileName;

		// `<RootDirectory>/BuildProfiles/` — directory the `.indexbuild`
		// files live in. Created lazily on first profile save; safe to call
		// before the directory exists. Empty string if RootDirectory itself
		// is empty.
		std::string GetBuildProfilesDirectory() const;

		// Splash screen — shown briefly before the first scene loads in
		// shipped builds. Disabled = no splash, scene loads immediately.
		// Enabled with no custom values produces the engine default
		// (Index version + platform + build profile, faded in/out).
		// SplashImagePath is an optional foreground image (relative
		// project path, same convention as AppIconPath); when empty the
		// default Index logo from IndexAssets/Textures/Index64.png is
		// used. SplashCustomText, when non-empty, replaces the engine-
		// generated subtitle line.
		struct SplashScreenSettings {
			bool Enabled = true;
			float DurationSeconds = 2.5f;
			float FadeInSeconds = 0.5f;
			float FadeOutSeconds = 0.5f;
			std::string ImagePath;
			// Optional background image painted full-canvas behind the
			// foreground logo. Same path convention as ImagePath /
			// AppIconPath (relative to the project root, "Assets/foo.png"
			// style). Empty = solid Background{R,G,B} fill (the original
			// behaviour). The image is drawn first with the splash's fade
			// alpha, then the logo + subtitle paint on top so the colour
			// fields still act as a fallback if the image fails to load.
			std::string BackgroundImagePath;
			std::string CustomText;
			float BackgroundR = 0.05f;
			float BackgroundG = 0.05f;
			float BackgroundB = 0.07f;
			// Subtitle text styling. FontSize is in pixels — ImGui draw-list
			// AddText accepts a custom size on top of the current font. Zero
			// or negative values fall back to the active ImGui font's
			// default size.
			float FontColorR = 1.0f;
			float FontColorG = 1.0f;
			float FontColorB = 1.0f;
			float FontSize = 16.0f;
		} SplashScreen;

		// User-defined preprocessor symbols. Get baked into BOTH the C#
		// .csproj's <DefineConstants> AND the native scripts' CMakeLists
		// target_compile_definitions on every script compile. Useful for
		// per-team / per-build flags (STEAM_BUILD, DEMO_MODE, etc.).
		// Names only — no `=value` form (matches Unity's scripting-define-
		// symbols convention; keeps the UI a flat list and the symbols
		// uniform across both compilers).
		std::vector<std::string> CustomDefines;

		std::string GetUserAssemblyOutputPath(std::string_view configuration = {}) const;

		// Returns "INDEX_BUILD_DEVELOPMENT" or "INDEX_BUILD_RELEASE" — the
		// compile-time symbol matching ActiveBuildProfile. Used by the C#
		// build pipeline (BuildManagedDefineConstants) and the native
		// CMakeLists generator.
		std::string GetActiveBuildProfileDefine() const;
		static const char* BuildProfileToString(BuildProfile profile);
		static BuildProfile BuildProfileFromString(std::string_view value);

		// String <-> enum conversion for the preferred render-backend
		// choice. Used by serialization + the Player Settings UI dropdown.
		// The "FromString" form accepts a few common spellings ("vulkan",
		// "VK", etc.) so manually-edited project files are forgiving.
		static const char* RenderBackendToString(RenderBackend backend);
		static RenderBackend RenderBackendFromString(std::string_view value);
		static const char* ProjectAssetSerializationFormatToString(ProjectAssetSerializationFormat format);
		static ProjectAssetSerializationFormat ProjectAssetSerializationFormatFromString(std::string_view value);
		static const char* EditorEntityNameSuffixStyleToString(EditorEntityNameSuffixStyle style);
		static EditorEntityNameSuffixStyle EditorEntityNameSuffixStyleFromString(std::string_view value);

		std::string GetNativeDllPath() const;
		std::string GetSceneFilePath(const std::string& sceneName) const;
		void EnsureNativeScriptBootstrapFiles() const;
		void EnsureNativeScriptProjectFiles() const;
		bool HasNativeScriptSources() const;

		// Returns true on a confirmed atomic write to disk. Callers that depend on the
		// project file having reached the disk (e.g. before clearing dirty state, before
		// triggering project regen) MUST check the return value.
		bool Save() const;

		// Write `Packages/IndexDefines.props` next to the .csproj. This file is
		// imported by the .csproj and bakes in CustomDefines + the active build
		// profile so Visual Studio's IntelliSense sees them as active (without
		// it, command-line `-p:DefineConstants=...` only affects MSBuild and
		// `#if XD` blocks render as inactive in the editor even though they
		// compile correctly). Idempotent — safe to call every Save.
		// `outAddedImport` is set to true if the .csproj was patched to add
		// the missing Import line (older projects pre-date this mechanism).
		bool WriteManagedDefinesProps(bool* outAddedImport = nullptr) const;

		static std::string GetActiveBuildConfiguration();
		static std::string GetActiveBuildDefineConstant();
		static std::string GetManagedPlatformDefine();
		static std::string BuildManagedDefineConstants(std::string_view primarySymbol);

		static IndexProject Create(const std::string& name, const std::string& parentDir,
			const CreateProgressCallback& progressCallback = {});
		static IndexProject Load(const std::string& rootDir);
		static bool Validate(const std::string& rootDir);
		static bool IsValidProjectName(const std::string& name);
		static std::string GetDefaultProjectsDir();

		// Locate the engine repository root from the running executable (walks up the bin/ tree
		// until it finds Index-Engine/src + Index-ScriptCore.csproj). Empty string on failure.
		static std::string GetEngineRootDir();

		// Path to the bundled premake5 binary (vendor/bin/premake5.exe). Empty if not found.
		static std::string GetPremakePath();

		// Re-runs the engine's premake5 with --index-project=<projectRootDir> to regenerate
		// the engine solution so it includes that project's package list. Returns the
		// process result (non-zero exit code on failure). Safe no-op if premake isn't found
		// (returns a synthetic result with ExitCode != 0 and an error message in Output).
		struct RegenerateResult {
			bool   Succeeded = false;
			int    ExitCode = -1;
			std::string Output;
		};
		static RegenerateResult RegenerateSolutionForProject(const std::string& projectRootDir);

		// Locate MSBuild.exe for VS 2022 (Community/Professional/Enterprise/BuildTools),
		// falling back to vswhere.exe and finally to PATH lookup. Empty string on failure.
		static std::string GetMSBuildPath();

		// Run MSBuild on the engine's Index.sln. Returns process result. Caller should
		// call RegenerateSolutionForProject first if the project's package list changed.
		//
		// IMPORTANT: by default this builds the WHOLE solution — including Index-Engine,
		// Index-Editor, Index-Launcher, Index-Runtime. If the *running process* has any
		// of those DLLs loaded (it always does — the launcher has Index-Engine.dll
		// loaded for its own runtime), MSBuild's link step will fail with LNK1104 on
		// the locked .ilk/.dll. For the common project-create / project-open path,
		// prefer the overload taking explicit targets and pass only the project-local
		// package projects (`Pkg.<Name>.Native`, `Pkg.<Name>`).
		struct BuildResult {
			bool   Succeeded = false;
			int    ExitCode = -1;
			std::string Output;
		};
		static BuildResult BuildSolution(const std::string& configuration = "Debug",
			const std::string& platform = "x64");

		// Selective build: passes -t:<target1>;<target2>;... to MSBuild so only the
		// listed projects get built. Empty target list is a no-op (returns Success
		// with no MSBuild invocation). Use this for project-local-package builds
		// where the engine itself is already up to date and shouldn't be relinked.
		static BuildResult BuildSolutionTargets(const std::vector<std::string>& targets,
			const std::string& configuration = "Debug",
			const std::string& platform = "x64");

		// Enumerate names of index-package.lua manifests under <projectRoot>/Packages/.
		// Returns the directory names (which is the package name by convention).
		// Empty vector when the project has no Packages/ subdir or no manifests in it.
		static std::vector<std::string> EnumerateProjectLocalPackages(const std::string& projectRoot);

		// Convenience: regenerate + build for a project in one call. Stops early on
		// regen failure; build runs only if regen succeeds (or if premake was missing,
		// which is a benign warning).
		struct AutomateResult {
			RegenerateResult Regenerate;
			BuildResult      Build;
			bool             RanBuild = false; // false if regen errored or no MSBuild
		};
		static AutomateResult AutomateForProject(const std::string& projectRootDir,
			const std::string& configuration = "Debug",
			const std::string& platform = "x64");
	};

} // namespace Index
