#include "pch.hpp"
#include "Application.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Scene/SceneManager.hpp"
#include "Core/Window.hpp"
#include "Graphics/GLInitSpecifications.hpp"
#include "Graphics/RenderApi.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Graphics/GizmoRenderer.hpp"
#include "Graphics/ShaderManager.hpp"
#include "Graphics/TextureManager.hpp"
#include "Core/PackageHost.hpp"
#include "Gui/GuiRenderer.hpp"
#include "Math/Math.hpp"
#include "Core/SingleInstance.hpp"
#include "Audio/AudioManager.hpp"
#include "Events/EventDispatcher.hpp"
#include "Events/WindowEvents.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Graphics/TextureManager.hpp"
#include "Jobs/JobSystem.hpp"
#include "Memory/FrameArenas.hpp"
#include "Physics/PhysicsSystem2D.hpp"
#include "Profiling/Profiler.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Serialization/Path.hpp"

#ifdef IDX_PLATFORM_WINDOWS
// winmm: timeBeginPeriod for 1ms timer resolution (frame-cap accuracy).
#  include <windows.h>
#  include <timeapi.h>
#  pragma comment(lib, "winmm.lib")
#  ifdef INDEX_PROFILER_ENABLED
#    include <psapi.h>
#    pragma comment(lib, "psapi.lib")
#  endif
#endif
#include <Utils/Timer.hpp>
#include <Utils/StringHelper.hpp>

#include "Input.hpp"
#include "Project/ProjectManager.hpp"
#include "Project/IndexProject.hpp"
#include <GLFW/glfw3.h>

namespace Index {
	const float Application::k_PausedTargetFrameRate = 10;

	Application* Application::s_Instance = nullptr;
	Application::CommandLineArgs Application::s_CommandLineArgs{};

	Application::Application()
		: m_SceneManager(std::make_unique<SceneManager>())
		, m_FixedUpdateAccumulator{ 0 }
	{
		// Process-wide singleton. Constructing a second Application while the
		// first is still alive would silently rebind s_Instance and leave the
		// original instance reachable only through whatever pointers callers
		// happen to hold — every subsystem keyed off Application::Get()
		// (input, scripting, scene manager) would start dispatching to the
		// new instance mid-frame. IDX_VERIFY is on in all build configs so
		// this catches the misuse in release too.
		IDX_VERIFY(s_Instance == nullptr,
			"Application: a second instance was constructed while one is still alive — "
			"only one Application is permitted per process.");
		m_MainThreadId = std::this_thread::get_id();
		Application::s_Instance = this;
	}

	Renderer2D* Application::GetRenderer2D() {
		// Today Renderer2D is the only IRenderer impl installed during
		// Initialize. When alternate backends (NullRenderer, custom 3D) become
		// installable, this static_cast is the documented coupling point —
		// callers that bypass IRenderer to reach Renderer2D-specific features
		// must tolerate a nullptr return.
		return static_cast<Renderer2D*>(m_Renderer2D.get());
	}

	Application::~Application()
	{
		// Defensive teardown for paths that destroyed the Application without
		// ever calling Run() (e.g. an Initialize-time exception in a non-Run
		// host or a unit test). Run()'s normal exit already calls Shutdown,
		// which sets m_IsShuttingDown back to false at the end — so we use
		// (window-still-alive AND not-currently-shutting-down) as the signal
		// that no Shutdown has happened yet.
		if (!m_IsShuttingDown && m_Window) {
			Shutdown(false);
		}

		if (s_Instance == this) {
			s_Instance = nullptr;
		}
	}

	void Application::Run()
	{
		m_MainThreadId = std::this_thread::get_id();

		if (m_ForceSingleInstance) {
			static SingleInstance instance(m_Name);
			IDX_ASSERT(!instance.IsAlreadyRunning(), IndexErrorCode::Undefined, "An Instance of this app is already running!");
		}

		for (;;) {
			IDX_INFO_TAG("Application", "Initializing...");
			Timer timer = Timer();
			try {
				Initialize();
			}
			catch (const std::exception& e) {
				IDX_ERROR_TAG("Application", std::string("Initialization failed: ") + e.what());
				Shutdown(false);
				return;
			}
			catch (...) {
				IDX_ERROR_TAG("Application", "Initialization failed with unknown exception");
				Shutdown(false);
				return;
			}
			IDX_INFO_TAG("Application", "Full Initialization took " + StringHelper::ToString(timer));

			try {
				Start();
			}
			catch (const std::exception& e) {
				IDX_ERROR_TAG("Application", std::string("Start failed: ") + e.what());
				Shutdown();
				return;
			}
			catch (...) {
				IDX_ERROR_TAG("Application", "Start failed with unknown exception");
				Shutdown();
				return;
			}
			m_LastFrameTime = Clock::now();

			// Headless (no m_Window): the loop exits on RequestQuit / Quit alone,
			// since there's no WindowClose event to listen for. The window-close
			// check is short-circuited when m_Window is null so the same loop
			// drives both modes.
			while (!m_ShouldQuit && !(m_Window && m_Window->ShouldClose())) {
				const float targetFps = Max(GetTargetFramerate(), 0.0f);
				DurationChrono targetFrameTime{};
				if (targetFps > 0.0f) {
					targetFrameTime = std::chrono::duration_cast<DurationChrono>(std::chrono::duration<double>(1.0 / targetFps));
				}
				auto now = Clock::now();

				// VSync bucket: gap between last frame's render end and now (covers SwapBuffers + frame-cap idle).
				const auto vsyncStart = m_LastFrameEndTime != Clock::time_point{}
					? m_LastFrameEndTime
					: now;

				// CPU idle for runtime fps cap. Relies on 1ms timer res from timeBeginPeriod in Initialize().
				// The "no vsync OR paused" predicate becomes "always (when capping)" in headless mode
				// because there's no swap-chain vsync to defer to — the explicit null guard keeps the
				// short-circuit safe for callers that flipped EnableWindow=false without using the
				// Headless() factory (which also clears UseTargetFrameRateForMainLoop).
				if (m_Configuration.UseTargetFrameRateForMainLoop && targetFps > 0.0f && (!m_Window || !m_Window->IsVsync() || IsEnginePaused()))
				{
					auto const nextFrameTime = m_LastFrameTime + targetFrameTime;

					if (now + std::chrono::milliseconds(1) < nextFrameTime) {
						std::this_thread::sleep_until(nextFrameTime - std::chrono::milliseconds(1));
					}
				}


				auto frameStart = Clock::now();
				const float vsyncMs = std::chrono::duration<float, std::milli>(frameStart - vsyncStart).count();
				float deltaTime = std::chrono::duration<float>(frameStart - m_LastFrameTime).count();

				if (deltaTime >= 0.25f) {
					ResetTimePoints();
					deltaTime = 0.0f;
				}

				try {
					INDEX_PROFILE_SCOPE("Frame");

					Profiler::PushFrameDelta(deltaTime);

					m_Time.Update(deltaTime);

					// Skip the entire input tick in headless mode: PollGamepads
					// (called from Input::Update) hits glfwGetGamepadState, which
					// crashes when GLFW wasn't initialised. Key / mouse / axis
					// state stays at its zero-initialised defaults, so any script
					// that reads Input.GetKey(...) just sees "nothing pressed" —
					// the right answer for a headless run.
					if (m_Window) {
						// Snapshot prev-state BEFORE polling so GetKeyDown/Up reflect events from this frame.
						m_Input.Update();
						glfwPollEvents();
						// PostPoll AFTER glfwPollEvents so derived state (axis) sees this
						// frame's keys, not last frame's.
						m_Input.PostPoll();
					}
					// Do NOT TryCompleteQuitRequest here — it must run AFTER
					// BeginFrame's OnPreRender pass so layers (e.g. the
					// editor's dirty-scene save-before-quit dialog) get to
					// see Application::IsQuitRequested() and call CancelQuit
					// before the request auto-completes. RequestQuit calls
					// originating from a layer's previous-frame OnPreRender
					// (Application > Quit menu, custom hotkeys, etc.) would
					// otherwise be completed at the very top of the next
					// frame — before the intercept has a chance to fire —
					// because m_QuitRequestFrame < current frame count by
					// then. The end-of-frame call at the bottom of the loop
					// is sufficient.

					// Scale at the accumulator input so TimeScale controls step *frequency*.
					// The step quantum stays unscaled — Box2D's solver needs a constant dt
					// for stability, and per-call dt staying fixed prevents script integration
					// (vel * Time.FixedDeltaTime) from compounding with the changed call rate.
					m_FixedUpdateAccumulator += m_Time.GetDeltaTime();
					const double fixedDt = m_Time.GetUnscaledFixedDeltaTime();
					if (fixedDt <= 0.0) {
						// Misconfigured fixed-step: a zero/negative step would make the
						// accumulator loop infinite. Drop accumulated time and skip the
						// FixedUpdate dispatch this frame.
						m_FixedUpdateAccumulator = 0.0;
					} else {
						while (m_FixedUpdateAccumulator >= fixedDt) {
							if (!IsEnginePaused() && !m_IsGameplayPaused) {
								BeginFixedFrame();
								// Dispatch guard defers any PopLayer/PopOverlay to the end of
								// the snapshot so a popped sibling's memory stays alive while
								// we're still iterating raw pointers into it.
								LayerDispatchGuard dispatchGuard(m_LayerStack);
								for (Layer* layer : SnapshotLayerOrder()) {
									layer->OnFixedUpdate(*this, m_Time.GetUnscaledFixedDeltaTime());
								}
								EndFixedFrame();
							}

							m_FixedUpdateAccumulator -= fixedDt;
						}
					}

					BeginFrame();
					EndFrame();
					TryCompleteQuitRequest();

					m_LastFrameTime = frameStart;
					m_Time.AdvanceFrameCount();

#ifdef INDEX_PROFILER_ENABLED
					INDEX_PROFILE_VALUE("VSync", vsyncMs);

#  ifdef IDX_PLATFORM_WINDOWS
					{
						PROCESS_MEMORY_COUNTERS pmc{};
						if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
							INDEX_PROFILE_VALUE("Total Memory", float(pmc.WorkingSetSize / (1024 * 1024)));
						}
					}
#  endif
					{
						const std::size_t textureBytes = TextureManager::GetTotalTextureMemoryBytes();
						INDEX_PROFILE_VALUE("Texture Memory", float(textureBytes / (1024 * 1024)));
					}

					{
						// Read Scene's cached live-entity counter instead of
						// iterating entt's entity storage every frame. The
						// previous scan was O(all entities) inside the
						// profiler block; on a 10k-entity scene it dominated
						// the profile sample it was meant to report on.
						std::size_t totalEntities = 0;
						if (m_SceneManager) {
							m_SceneManager->ForeachLoadedScene([&totalEntities](Scene& scene) {
								totalEntities += scene.GetEntityCount();
							});
						}
						INDEX_PROFILE_VALUE("Entity Count", float(totalEntities));
					}

					INDEX_PROFILE_VALUE("Playing Sources", float(AudioManager::GetActiveSoundCount()));

					// "Others" residual: Frame Time minus the four named CPU buckets.
					{
						const float frameMs    = Profiler::GetCurrentValue("Frame Time");
						const float renderMs   = Profiler::GetCurrentValue("Rendering");
						const float scriptsMs  = Profiler::GetCurrentValue("Scripts");
						const float physicsMs  = Profiler::GetCurrentValue("Physics");
						const float others = frameMs - renderMs - scriptsMs - physicsMs - vsyncMs;
						INDEX_PROFILE_VALUE("Others", std::max(0.0f, others));
					}
#endif

					INDEX_PROFILE_FRAME("Frame");

					// MUST be last in try-block — anything after eats into next frame's VSync bucket.
					m_LastFrameEndTime = Clock::now();
				}
				catch (const std::exception& e) {
					m_IsRenderingFrame = false;
					IDX_ERROR_TAG("Application", "Unhandled frame exception: {}", e.what());
					m_ShouldQuit = true;
				}
				catch (...) {
					m_IsRenderingFrame = false;
					IDX_ERROR_TAG("Application", "Unhandled frame exception: unknown exception");
					m_ShouldQuit = true;
				}
			}

			Shutdown();

			if (!m_CanReload) break;
			m_CanReload = false;

			// Rebuild SceneManager so reload doesn't reuse a shut-down instance.
			m_SceneManager.reset();
			m_SceneManager = std::make_unique<SceneManager>();
		}
	}

	void Application::Initialize() {
#ifdef IDX_PLATFORM_WINDOWS
		// 1ms timer res for accurate sleep_until in the frame-cap path. Paired with timeEndPeriod in Shutdown.
		timeBeginPeriod(1);
#endif
		// Report the configured entity cap once at startup. The bit-split is
		// chosen at compile time via index-project.json's `entityBits` field
		// (premake bakes it into the engine TUs as -DINDEX_ENTITY_BITS=N).
		// Surfacing the cap here lets users sanity-check the build matches
		// the setting they expected, especially after editing the project
		// JSON manually.
		{
			using EntityTraits = entt::entt_traits<EntityHandle>;
			constexpr uint64_t kEntityCap = static_cast<uint64_t>(EntityTraits::entity_mask);
			constexpr uint64_t kVersionCount = static_cast<uint64_t>(EntityTraits::version_mask) + 1u;
			IDX_INFO_TAG("Scene",
				"Entity cap: {} live entities, {} versions per slot (entityBits in index-project.json controls this).",
				kEntityCap, kVersionCount);
		}

		m_Configuration = GetConfiguration();
		SetName(m_Configuration.WindowSpecification.Title);

		// Profiler first — no GL/window deps, lets later subsystems emit profile marks during init.
#ifdef INDEX_PROFILER_ENABLED
		Profiler::Initialize();
#endif

		Timer timer = Timer();
		// JobSystem before any other subsystem so init-time fan-out
		// (e.g. parallel asset scans) is available. The pool itself is
		// cheap to spin up — milliseconds — and other shutdown paths
		// rely on it still being alive when they tear down.
		// Worker count comes from ApplicationConfig (default -1 = auto,
		// matching the previous hardcoded behavior). Game code can
		// override via IDX_GAME's config; scripts can override at boot
		// via JobSystem.Configure on the C# side.
		JobSystem::Initialize(JobSystemSpec{ m_Configuration.JobSystemWorkerCount });
		IDX_INFO_TAG("JobSystem", "Initialization took " + StringHelper::ToString(timer));

		timer.Reset();
		// FrameArenas backs per-frame scratch (Renderer2D, Gizmo, etc.) and
		// a persistent buffer for cross-frame engine scratch. Initialize
		// before any subsystem that might use them; both capacities come
		// from ApplicationConfig and either can be 0 to opt out.
		FrameArenas::Initialize(FrameArenasSpec{
			m_Configuration.FrameArenaCapacityBytes,
			m_Configuration.PersistentArenaCapacityBytes,
		});
		IDX_INFO_TAG("FrameArenas", "Initialization took " + StringHelper::ToString(timer));

		if (m_Configuration.EnableWindow) {
			timer.Reset();
			Window::Initialize();
			m_Window = std::make_unique<Window>(m_Configuration.WindowSpecification);
			m_Window->SetVsync(m_Configuration.Vsync);
			m_Window->SetEventCallback([this](IndexEvent& e) { DispatchEvent(e); });
			IDX_INFO_TAG("Window", "Initialization took " + StringHelper::ToString(timer));

			timer.Reset();
			RenderApi::Init(GLInitSpecifications(Color::Background(), GLCullingMode::Back));
			IDX_INFO_TAG("RenderApi", "Initialization took " + StringHelper::ToString(timer));
		}
		else {
			// Headless mode: force off every subsystem that needs a window
			// or a GL context, even if the user explicitly enabled them.
			// Each override logs a warning so the silent demotion is visible
			// in the log. ApplicationConfig::Headless() already clears these,
			// but a caller who flipped EnableWindow=false on a default-
			// constructed config still gets a working build.
			auto disable = [](bool& flag, const char* name) {
				if (flag) {
					IDX_WARN_TAG("Application", "{} disabled because EnableWindow=false (headless mode).", name);
					flag = false;
				}
			};
			disable(m_Configuration.EnableRenderer2D,     "EnableRenderer2D");
			disable(m_Configuration.EnableGuiRenderer,    "EnableGuiRenderer");
			disable(m_Configuration.EnableGizmoRenderer,  "EnableGizmoRenderer");
			disable(m_Configuration.EnableTextureManager, "EnableTextureManager");
			disable(m_Configuration.EnableShaderManager,  "EnableShaderManager");
			disable(m_Configuration.SetWindowIcon,        "SetWindowIcon");
			IDX_INFO_TAG("Application", "Running headless (EnableWindow=false): no window, no graphics backend.");
		}

		// TextureManager must come before Renderer2D: Renderer2D::Initialize
		// hard-asserts that the default Square texture is registered (it's
		// the unresolved-sprite fallback), and that registration happens
		// inside TextureManager::Initialize -> LoadDefaultTextures.
		if (m_Configuration.EnableTextureManager) {
			timer.Reset();
			TextureManager::Initialize();
			IDX_INFO_TAG("TextureManager", "Initialization took " + StringHelper::ToString(timer));
		}

		if (m_Configuration.EnableShaderManager) {
			timer.Reset();
			ShaderManager::Initialize();
			IDX_INFO_TAG("ShaderManager", "Initialization took " + StringHelper::ToString(timer));
		}

		if (m_Configuration.EnableRenderer2D) {
			timer.Reset();
			// Construct the concrete default renderer to do impl-specific
			// configuration, then move into the IRenderer-typed member. Packages
			// that want to substitute a different IRenderer can do so by
			// installing it BEFORE Application reaches this point (future hook).
			auto renderer = std::make_unique<Renderer2D>();
			renderer->Initialize();
			renderer->SetSceneProvider([this](const std::function<void(Scene&)>& fn) {
				if (m_SceneManager) {
					m_SceneManager->ForeachLoadedScene(fn);
				}
				});
			m_Renderer2D = std::move(renderer);
			IDX_INFO_TAG("Renderer2D", "Initialization took " + StringHelper::ToString(timer));
		}

		if (m_Configuration.EnableGizmoRenderer) {
			timer.Reset();
			m_GizmoRenderer2D = std::make_unique<GizmoRenderer2D>();
			m_GizmoRenderer2D->Initialize();
			IDX_INFO_TAG("GizmoRenderer", "Initialization took " + StringHelper::ToString(timer));
		}

		if (m_Configuration.EnableGuiRenderer) {
			timer.Reset();
			m_GuiRenderer = std::make_unique<GuiRenderer>();
			m_GuiRenderer->Initialize();
			IDX_INFO_TAG("GuiRenderer", "Initialization took " + StringHelper::ToString(timer));
		}

		if (m_Configuration.EnablePhysics2D) {
			timer.Reset();
			// Construct the concrete default impl, then move into the
			// IPhysicsEngine-typed member — mirrors the renderer wiring.
			auto physics = std::make_unique<PhysicsSystem2D>();
			physics->Initialize();
			m_PhysicsSystem2D = std::move(physics);
			IDX_INFO_TAG("PhysicsSystem", "Initialization took " + StringHelper::ToString(timer));
		}

		// FontManager piggy-backs on Renderer2D's GL context but lives on
		// the same enable flag as TextureManager (both are GPU asset
		// caches). No extra config knob — text without a renderer is
		// pointless anyway.
		if (m_Configuration.EnableTextureManager && m_Configuration.EnableRenderer2D) {
			timer.Reset();
			FontManager::Initialize();
			IDX_INFO_TAG("FontManager", "Initialization took " + StringHelper::ToString(timer));
		}

		// Register the rest of IndexAssets/ as built-in assets so the
		// inspector's reference picker can offer engine-shipped icons,
		// audio, etc. via its eye toggle. Runs AFTER FontManager so the
		// default font's hand-picked GUID wins over the auto-scan.
		{
			timer.Reset();
			const std::string indexAssetsRoot = Path::ResolveIndexAssets("");
			IDX_INFO_TAG("AssetRegistry", "IndexAssets root resolved to: '{}'", indexAssetsRoot);
			if (!indexAssetsRoot.empty()) {
				const size_t before = AssetRegistry::GetBuiltInCount();
				AssetRegistry::RegisterBuiltInDirectory(indexAssetsRoot);
				const size_t after = AssetRegistry::GetBuiltInCount();
				IDX_INFO_TAG("AssetRegistry", "Built-in scan registered {} files in {}", after - before, StringHelper::ToString(timer));
			} else {
				IDX_WARN_TAG("AssetRegistry", "IndexAssets root not found - built-in assets won't appear in picker");
			}
		}

		if (m_Configuration.EnableAudio) {
			timer.Reset();
			if (AudioManager::Initialize()) {
				IDX_INFO_TAG("AudioManager", "Initialization took " + StringHelper::ToString(timer));
			}
			else {
				IDX_ERROR_TAG("AudioManager", "Initialization failed. Continuing without audio.");
				m_Configuration.EnableAudio = false;
			}
		}

		timer.Reset();

		ConfigureScenes();
		m_SceneManager->Initialize();
		IDX_INFO_TAG("SceneManager", "Initialization took " + StringHelper::ToString(timer));
		ConfigureLayers();

		// Preload-frame: render whatever layers ConfigureLayers pushed
		// (typically a splash overlay in shipped builds) BEFORE the
		// potentially seconds-long blocking InitializeStartupScenes call
		// below. Without this, the user stares at a black window for the
		// duration of every Awake/Start in the startup scene; the splash
		// only appeared after scenes had finished loading. Render twice:
		// the first frame initializes ImGui's context-bound resources and
		// often draws nothing visible, the second actually paints.
		RenderOnceForRefresh();
		RenderOnceForRefresh();

		// Apply the project's custom cursor images (always-on default
		// + UI hover variant). UIEventSystem flips between the two each
		// frame via Window::SetCursorOverUI; loading them here once
		// avoids re-decoding the texture per swap. Cursor support runs
		// in both editor and shipped runtime so authored projects get
		// the same look in either host. Skipped entirely when headless
		// (no window to apply the cursor to, no TextureManager to load
		// the image through).
		if (m_Window) {
			IndexProject* project = ProjectManager::GetCurrentProject();
			auto applyCursor = [&](const std::string& projectPath, void (Window::*setter)(const Texture2D*))
			{
				if (!project || projectPath.empty()) return;
				std::filesystem::path absPath(projectPath);
				if (!absPath.is_absolute()) {
					absPath = std::filesystem::path(project->RootDirectory) / projectPath;
				}
				if (!std::filesystem::exists(absPath)) {
					IDX_WARN_TAG("Application", "Cursor image not found: {}", absPath.string());
					return;
				}
				TextureHandle h = TextureManager::LoadTexture(absPath.string());
				if (Texture2D* tex = TextureManager::GetTexture(h); tex && tex->IsValid()) {
					(m_Window.get()->*setter)(tex);
				}
			};
			if (project) {
				applyCursor(project->CursorImagePath, &Window::SetCursorImage);
				applyCursor(project->UIInteractableCursorImagePath, &Window::SetUICursorImage);
			}
		}

		if (m_Configuration.SetWindowIcon && m_Window) {
			m_Window->SetWindowIconFromResource();

			// AppIconPath is the icon for the SHIPPED game's window (and
			// what the editor's build pipeline embeds into the runtime
			// .exe). Applying it here in the editor would replace the
			// editor's own window icon with the user's game icon, which
			// isn't what the project setting promises — the editor should
			// keep its embedded RT_ICON regardless of which project is
			// open. Restrict the load to the runtime host.
			if (!IsEditor()) {
				IndexProject* project = ProjectManager::GetCurrentProject();
				if (project && !project->AppIconPath.empty()) {
					// AppIconPath is stored project-relative (e.g.
					// "Assets/icon.png"). TextureManager::ResolveTexturePath
					// only checks CWD / engine IndexAssets / <exe>/Assets/Textures,
					// none of which equal "<project>/Assets/icon.png" once
					// the runtime is launched from anywhere other than the
					// project dir. Prepend the project root explicitly so
					// the load works regardless of the working directory.
					std::filesystem::path iconPath(project->AppIconPath);
					if (!iconPath.is_absolute()) {
						iconPath = std::filesystem::path(project->RootDirectory) / project->AppIconPath;
					}
					TextureHandle h = TextureManager::LoadTexture(iconPath.string());
					Texture2D* tex = TextureManager::GetTexture(h);
					if (tex && tex->IsValid()) {
						m_Window->SetWindowIcon(tex);
					}
				}
			}
		}

		if (m_Configuration.EnablePackageHost) {
			// MUST run before InitializeStartupScenes so package-registered components exist for deserialization.
			PackageHost::LoadAll();
		}

		// Splash-gated startup: when the splash overlay is currently
		// attached (runtime hosts opt in via project.SplashScreen.Enabled
		// and push the layer in ConfigureLayers), skip the heavy startup
		// trio here and let TickDeferredStartupScenes drain it once the
		// splash pops itself at the end of its timeline. The main loop
		// will tick the splash's OnUpdate / OnPreRender each frame in
		// the meantime, so the user sees the splash play through cleanly
		// before any scene-load stutter. Hosts without a splash (editor,
		// or runtime with SplashScreen disabled) load synchronously here
		// — the m_DeferredStartupScenes flag stays false and the gate
		// no-ops every frame.
		if (m_SplashLayerActive) {
			m_DeferredStartupScenes = true;
			IDX_INFO_TAG("Application", "Startup scene load deferred until splash completes.");
		}
		else {
			m_SceneManager->InitializeStartupScenes();
			ScriptEngine::RaiseApplicationStart();
			// Baseline for Time.TimeSinceStartup / Time.RealtimeSinceStartup —
			// excludes window/GL/scene/package init from the "since game start" clock.
			// In editor preview the editor calls MarkGameStart() again on play-mode
			// entry so each play session starts at zero.
			m_Time.MarkGameStart();
		}
	}

	void Application::TickDeferredStartupScenes() {
		// Splash hasn't fired yet — wait. The flag flips false in
		// SignalSplashDetached, which the splash layer calls from its
		// OnDetach (after RuntimeImGuiHost::Release).
		if (!m_DeferredStartupScenes || m_SplashLayerActive) return;

		m_DeferredStartupScenes = false;

		if (m_SceneManager) {
			IDX_INFO_TAG("Application", "Splash complete — loading startup scenes...");
			m_SceneManager->InitializeStartupScenes();
		}
		ScriptEngine::RaiseApplicationStart();
		// Reset t=0 here rather than at the bottom of Initialize: with
		// the splash sitting in front of the user for several seconds,
		// counting that time in Time.timeSinceStartup would make script
		// timers (cooldowns, fade-ins) start mid-elapsed. Marking the
		// start now means t=0 == "first frame the player actually sees".
		m_Time.MarkGameStart();
		// Drop the leftover dt-jump from the splash period: without this,
		// the next BeginFrame would feed several seconds' worth of dt into
		// every system at once, breaking Box2D stability and any unscaled-
		// dt animation that tweened against actual elapsed time.
		ResetTimePoints();
	}

	void Application::BeginFrame() {
		INDEX_PROFILE_SCOPE("Application::BeginFrame");
		m_IsRenderingFrame = true;

		CoreInput();

		const bool enginePaused = IsEnginePaused();
		if (enginePaused && !m_WasEnginePaused) {
			ScriptEngine::RaiseApplicationPaused();
		}
		m_WasEnginePaused = enginePaused;

		if (!enginePaused) {
			bool gameplayActive = m_IsPlaying && !m_IsGameplayPaused;

			if (gameplayActive && m_Configuration.EnableAudio) 
				AudioManager::Update();

			if (gameplayActive)
				ScriptEngine::UpdateGlobalSystems();

			Update();

			{
				INDEX_PROFILE_SCOPE("Layer.OnUpdate");
				// Dispatch guard defers PopLayer/PopOverlay to scope exit so popped
				// siblings stay live for the rest of this iteration.
				LayerDispatchGuard updateGuard(m_LayerStack);
				for (Layer* layer : SnapshotLayerOrder()) {
					layer->OnUpdate(*this, m_Time.GetDeltaTime());
				}
			}

			// Splash gate: when the splash layer just popped itself in
			// the OnUpdate dispatch above, this drains the deferred
			// startup-scene load so the very next render pass sees a
			// loaded scene. Runs BEFORE UpdateScenes / OnPreRenderScenes
			// because both depend on the active scene actually existing.
			TickDeferredStartupScenes();

			if (gameplayActive && m_SceneManager) {
				INDEX_PROFILE_SCOPE("UpdateScenes");
				m_SceneManager->UpdateScenes();
			}

			// Sync Transform2D into physics every frame (editor + play).
			if (m_PhysicsSystem2D) m_PhysicsSystem2D->Update();

			if (m_SceneManager) {
				INDEX_PROFILE_SCOPE("OnPreRenderScenes");
				m_SceneManager->OnPreRenderScenes();
			}
			{
				INDEX_PROFILE_SCOPE("Layer.OnPreRender");
				LayerDispatchGuard preRenderGuard(m_LayerStack);
				for (Layer* layer : SnapshotLayerOrder()) {
					INDEX_TRY_CATCH_LOG(layer->OnPreRender(*this));
				}
			}

			if (m_Renderer2D) {
				INDEX_PROFILE_SCOPE("Renderer2D.Begin");
				INDEX_TRY_CATCH_LOG(m_Renderer2D->BeginFrame());
			}

			if (m_GuiRenderer) {
				INDEX_PROFILE_SCOPE("GuiRenderer.Begin");
				INDEX_TRY_CATCH_LOG(m_GuiRenderer->BeginFrame(*m_SceneManager));
			}

			if (m_GizmoRenderer2D) {
				INDEX_PROFILE_SCOPE("GizmoRenderer.Begin");
				INDEX_TRY_CATCH_LOG(m_GizmoRenderer2D->BeginFrame());
			}
		}
		else {
			OnPaused();
		}
	}

	void Application::EndFrame() {
		INDEX_PROFILE_SCOPE("Application::EndFrame");
		if (!IsEnginePaused()) {
			RenderPipelineOnly();
		}

		m_IsRenderingFrame = false;

		// Wipe per-frame scratch after rendering completes. O(1) bump-
		// pointer reset; pointers handed out during this frame become
		// dangling here, which is the contract callers opted into. Runs
		// every frame including the paused branch — paused frames may
		// still have allocated scratch via OnPaused / UI layers.
		FrameArenas::OnEndFrame();
	}

	void Application::DispatchEvent(IndexEvent& event) {
		EventDispatcher dispatcher(event);

		dispatcher.Dispatch<WindowCloseEvent>([this](WindowCloseEvent&) {
			RequestQuit();
			glfwSetWindowShouldClose(m_Window->GetGLFWWindow(), GLFW_FALSE);
			return false;
			});

		dispatcher.Dispatch<WindowResizeEvent>([this](WindowResizeEvent& e) {
			m_IsMinimized = e.GetWidth() == 0 || e.GetHeight() == 0;
			// Forward to managed scripts so `Index.Window.OnResize`
			// subscribers fire the same frame GLFW delivered the new
			// framebuffer size. Skipped when minimised (zero-area
			// frame) so scripts don't see a transient 0x0 just because
			// the user dragged the window down to the taskbar.
			if (!m_IsMinimized) ScriptEngine::RaiseWindowResize();
			return false;
			});

		dispatcher.Dispatch<FileDropEvent>([this](FileDropEvent& e) {
			// Append rather than replace: GLFW can fire multiple drop events in
			// the same frame (drag of N files split across batches), and the
			// previous assignment dropped earlier batches before TakePendingFileDrops ran.
			const auto& paths = e.GetPaths();
			m_PendingFileDrops.insert(m_PendingFileDrops.end(), paths.begin(), paths.end());
			return false;
			});

		// Dispatch guard so OnEvent that pushes/pops a sibling can't shift indices
		// or invalidate a sibling pointer mid-iteration.
		LayerDispatchGuard eventGuard(m_LayerStack);
		for (Layer* layer : SnapshotLayerOrderReversed()) {
			if (event.Handled) {
				break;
			}
			layer->OnEvent(*this, event);
		}

		if (!event.Handled) {
			m_EventBus.Publish(event);
		}
	}

	void Application::Quit() {
		if (!s_Instance) {
			return;
		}

		s_Instance->m_QuitRequested = false;
		s_Instance->m_QuitRequestFrame = -1;
		s_Instance->m_ShouldQuit = true;
	}
	void Application::RequestQuit() {
		if (!s_Instance) {
			return;
		}

		s_Instance->m_QuitRequested = true;
		s_Instance->m_QuitRequestFrame = s_Instance->m_Time.GetFrameCount();
	}
	bool Application::IsQuitRequested() {
		return s_Instance ? s_Instance->m_QuitRequested : false;
	}
	void Application::CancelQuit() {
		if (!s_Instance) {
			return;
		}

		s_Instance->m_QuitRequested = false;
		s_Instance->m_QuitRequestFrame = -1;
	}
	void Application::ConfirmQuit() {
		Quit();
	}
	void Application::TryCompleteQuitRequest() {
		if (!m_QuitRequested || m_ShouldQuit) {
			return;
		}

		if (m_Time.GetFrameCount() <= m_QuitRequestFrame) {
			return;
		}

		Quit();
	}

	void Application::BeginFixedFrame() {
		FixedUpdate();

		if (!m_IsPlaying) return;

		if (m_SceneManager) m_SceneManager->FixedUpdateScenes();
		// Step quantum is unscaled and constant. TimeScale already affected how many
		// times this fires per real second via the accumulator above.
		if (m_PhysicsSystem2D) m_PhysicsSystem2D->FixedUpdate(m_Time.GetUnscaledFixedDeltaTime());
		// Runs after physics so global systems observe transforms synced from the physics step.
		ScriptEngine::FixedUpdateGlobalSystems();
	}

	void Application::EndFixedFrame() { }

	void Application::RenderPipelineOnly() {
		if (m_Renderer2D) {
			INDEX_PROFILE_SCOPE("Renderer2D.End");
			INDEX_TRY_CATCH_LOG(m_Renderer2D->EndFrame());
		}

		{
			INDEX_PROFILE_SCOPE("Layer.OnPostRender");
			LayerDispatchGuard postRenderGuard(m_LayerStack);
			for (Layer* layer : SnapshotLayerOrder()) {
				INDEX_TRY_CATCH_LOG(layer->OnPostRender(*this));
			}
		}

		if (m_GuiRenderer) {
			INDEX_PROFILE_SCOPE("GuiRenderer.End");
			INDEX_TRY_CATCH_LOG(m_GuiRenderer->EndFrame());
		}
		if (m_GizmoRenderer2D) {
			INDEX_PROFILE_SCOPE("GizmoRenderer.End");
			// Explicit gizmo pass — used to be a hidden draw inside EndFrame.
			// Now declared as its own step in the render pipeline.
			if (Gizmo::GetShowInRuntime()) {
				// Resolve VP from the runtime main camera. The renderer no
				// longer reaches into Camera2DComponent::Main() implicitly
				// (per audit H3) — callers thread the VP they want gizmos
				// projected against. No camera = no draw.
				if (Camera2DComponent* cam = Camera2DComponent::Main()) {
					INDEX_TRY_CATCH_LOG(m_GizmoRenderer2D->RenderWithVP(cam->GetViewProjectionMatrix(), GizmoLayerMask::Shared));
				}
			}
			INDEX_TRY_CATCH_LOG(m_GizmoRenderer2D->EndFrame());
		}

		if (m_Window) {
			INDEX_PROFILE_SCOPE("SwapBuffers");
			m_Window->SwapBuffers();
		}

		// Post-Submit hook. SwapBuffers triggers WebGPUBackend::Present,
		// which runs Queue::Submit. By the time control returns here, the
		// command buffer for this frame has been consumed by Dawn and the
		// GPU work is in flight. This is the safe place to issue any
		// MapAsync calls that would otherwise have raced against Submit
		// (e.g. GpuTimer's deferred readback maps).
		if (Renderer2D* r2d = GetRenderer2D()) {
			INDEX_TRY_CATCH_LOG(r2d->OnAfterPresent());
		}
	}

	void Application::RenderOnceForRefresh() {
		if (m_IsRenderingFrame || m_IsRenderingRefresh || !m_Window || m_ShouldQuit) {
			return;
		}

		struct RefreshRenderGuard {
			explicit RefreshRenderGuard(Application& application)
				: App(application)
			{
				App.m_IsRenderingRefresh = true;
			}

			~RefreshRenderGuard()
			{
				App.m_IsRenderingRefresh = false;
			}

			Application& App;
		};

		RefreshRenderGuard guard(*this);
		LayerDispatchGuard refreshGuard(m_LayerStack);

		if (m_SceneManager) INDEX_TRY_CATCH_LOG(m_SceneManager->OnPreRenderScenes());
		for (Layer* layer : SnapshotLayerOrder()) {
			INDEX_TRY_CATCH_LOG(layer->OnPreRender(*this));
		}

		if (m_Renderer2D) {
			INDEX_TRY_CATCH_LOG(m_Renderer2D->BeginFrame());
			INDEX_TRY_CATCH_LOG(m_Renderer2D->EndFrame());
		}

		for (Layer* layer : SnapshotLayerOrder()) {
			INDEX_TRY_CATCH_LOG(layer->OnPostRender(*this));
		}

		if (m_GuiRenderer && m_SceneManager) {
			INDEX_TRY_CATCH_LOG(m_GuiRenderer->BeginFrame(*m_SceneManager));
			INDEX_TRY_CATCH_LOG(m_GuiRenderer->EndFrame());
		}

		if (m_GizmoRenderer2D) {
			INDEX_TRY_CATCH_LOG(m_GizmoRenderer2D->BeginFrame());
			if (Gizmo::GetShowInRuntime()) {
				if (Camera2DComponent* cam = Camera2DComponent::Main()) {
					INDEX_TRY_CATCH_LOG(m_GizmoRenderer2D->RenderWithVP(cam->GetViewProjectionMatrix(), GizmoLayerMask::Shared));
				}
			}
			INDEX_TRY_CATCH_LOG(m_GizmoRenderer2D->EndFrame());
		}

		if (m_Window) m_Window->SwapBuffers();
	}


	void Application::CoreInput() {
		if (m_Input.GetKey(KeyCode::LeftControl) && m_Input.GetKeyDown(KeyCode::M)) {
			if (m_Window) m_Window->MinimizeWindow();
		}
		if (m_Input.GetKeyDown(KeyCode::F11)) {
			if (m_Window) m_Window->SetFullScreen(!m_Window->IsFullScreen());
		}
	}

	void Application::ResetTimePoints() {
		m_LastFrameTime = Clock::now();
		m_FixedUpdateAccumulator = 0;
		// Reset m_LastFrameEndTime too: leaving it at the prior frame's stamp meant
		// the next frame's vsync-bucket calculation (frameStart - m_LastFrameEndTime)
		// treated the entire delta-spike interval as "vsync wait time", emitting a
		// misleading multi-second VSync profile mark and dragging the residual
		// 'Others' bucket into negative-clamped-to-0 territory.
		m_LastFrameEndTime = Clock::time_point{};
	}

	void Application::RefreshBackgroundPauseState() {
		m_IsBackgroundPaused = !m_RunInBackground && !m_WindowHasFocus;
	}

	void Application::Shutdown(bool invokeOnQuit) {
		m_IsShuttingDown = true;

		if (invokeOnQuit) {
			try {
				ScriptEngine::RaiseApplicationQuit();
				OnQuit();
			}
			catch (const std::exception& e) {
				IDX_ERROR_TAG("Application", std::string("OnQuit failed: ") + e.what());
			}
			catch (...) {
				IDX_ERROR_TAG("Application", "OnQuit failed with unknown exception");
			}
		}

		// Snapshot raw pointers — layer OnDetach may pop other layers
		// (legitimate during shutdown), and we need to detach each one
		// exactly once regardless of mutations. The dispatch guard makes
		// any pop deferred for the duration of the loop, so popped layer
		// memory stays live until we're done iterating.
		{
			LayerDispatchGuard detachGuard(m_LayerStack);
			std::vector<Layer*> detachOrder;
			detachOrder.reserve(m_LayerStack.Size());
			for (size_t i = m_LayerStack.Size(); i > 0; --i) {
				Layer* layer = m_LayerStack.At(i - 1);
				if (layer) detachOrder.push_back(layer);
			}
			for (Layer* layer : detachOrder) {
				if (m_LayerStack.IsPendingPop(layer)) {
					continue;
				}
				layer->OnDetach(*this);
			}
		}
		m_EventBus.Clear();
		m_LayerStack.Clear();

		// Each subsystem shutdown is guarded so that a throw doesn't skip
		// the rest. Critically, every owning unique_ptr is .reset() in the
		// same step as its Shutdown — otherwise an exception would leave
		// the pointer non-null and a re-entrant render path (e.g. a frame
		// triggered by a hook firing during teardown) could call into a
		// half-shutdown object.
		auto guardedShutdownStatic = [](const char* name, auto&& fn) {
			try { fn(); }
			catch (const std::exception& e) {
				IDX_CORE_ERROR_TAG("Application::Shutdown", "{} threw: {}", name, e.what());
			}
			catch (...) {
				IDX_CORE_ERROR_TAG("Application::Shutdown", "{} threw unknown exception", name);
			}
		};
		auto guardedShutdownOwned = [&guardedShutdownStatic](const char* name, auto& ptr) {
			if (!ptr) return;
			guardedShutdownStatic(name, [&] { ptr->Shutdown(); });
			ptr.reset();  // null even on throw so render path can't reach a half-torn object
		};

		// Order matters: subsystems that hold package callbacks tear down BEFORE PackageHost::UnloadAll.
		if (m_SceneManager) guardedShutdownStatic("SceneManager", [&]{ m_SceneManager->Shutdown(); });
		if (ScriptEngine::IsInitialized()) guardedShutdownStatic("ScriptEngine", []{ ScriptEngine::Shutdown(); });
		if (FontManager::IsInitialized()) guardedShutdownStatic("FontManager", []{ FontManager::Shutdown(); });
		if (m_Configuration.EnableTextureManager) guardedShutdownStatic("TextureManager", []{ TextureManager::Shutdown(); });
		if (ShaderManager::IsInitialized()) guardedShutdownStatic("ShaderManager", []{ ShaderManager::Shutdown(); });

		guardedShutdownOwned("PhysicsSystem2D", m_PhysicsSystem2D);
		guardedShutdownOwned("GuiRenderer", m_GuiRenderer);
		guardedShutdownOwned("GizmoRenderer2D", m_GizmoRenderer2D);
		guardedShutdownOwned("Renderer2D", m_Renderer2D);
		if (RenderApi::IsInitialized()) guardedShutdownStatic("RenderApi", []{ RenderApi::Shutdown(); });

		if (AudioManager::IsInitialized()) guardedShutdownStatic("AudioManager", []{ AudioManager::Shutdown(); });

		if (m_Configuration.EnablePackageHost) guardedShutdownStatic("PackageHost", []{ PackageHost::UnloadAll(); });

		if (m_Window) {
			guardedShutdownStatic("Window::Destroy", [&]{
				m_Window->SetEventCallback({});
				m_Window->Destroy();
			});
		}
		if (Window::IsInitialized()) guardedShutdownStatic("Window::Shutdown", []{ Window::Shutdown(); });

		m_Window.reset();

		// Release FrameArenas backing buffers. Pairs with the Initialize
		// call. After this, Frame()/Persistent() still return valid
		// references but with zero capacity, so any late callers fail
		// gracefully (Allocate returns nullptr) instead of touching freed
		// memory.
		FrameArenas::Shutdown();

		if (JobSystem::IsInitialized()) {
			JobSystem::Shutdown();
		}

#ifdef INDEX_PROFILER_ENABLED
		// Profiler last so subsystems above can still emit profile marks during teardown.
		Profiler::Shutdown();
#endif

#ifdef IDX_PLATFORM_WINDOWS
		timeEndPeriod(1);
#endif

		m_ShouldQuit = false;
		m_QuitRequested = false;
		m_QuitRequestFrame = -1;
		m_IsPaused = false;
		m_IsBackgroundPaused = false;
		m_IsMinimized = false;
		m_WindowHasFocus = true;
		m_IsGameplayPaused = false;
		m_IsScriptInputEnabled = true;
		m_IsShuttingDown = false;
		m_WasEnginePaused = false;
		m_FixedUpdateAccumulator = 0.0;
		m_PendingFileDrops.clear();
	}
}
