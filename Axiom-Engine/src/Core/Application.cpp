#include "pch.hpp"
#include "Application.hpp"
#include "Scene/SceneManager.hpp"
#include "Core/Window.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Graphics/GizmoRenderer.hpp"
#include "Graphics/TextureManager.hpp"
#include "Core/PackageHost.hpp"
#include "Graphics/OpenGL.hpp"
#include "Gui/GuiRenderer.hpp"
#include "Math/Math.hpp"
#include "Core/SingleInstance.hpp"
#include "Audio/AudioManager.hpp"
#include "Events/EventDispatcher.hpp"
#include "Events/WindowEvents.hpp"
#include "Graphics/TextureManager.hpp"
#include "Physics/PhysicsSystem2D.hpp"
#include "Profiling/Profiler.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/ScriptEngine.hpp"

#ifdef AIM_PLATFORM_WINDOWS
// E21: bring in windows.h + winmm for timeBeginPeriod/timeEndPeriod (used to
// raise the system timer resolution to 1ms during the app lifetime so the
// frame-cap sleep_until below stops undershooting). winmm.lib is already
// linked by the project; psapi is only needed by the profiler block.
#  include <windows.h>
#  include <timeapi.h>
#  pragma comment(lib, "winmm.lib")
#  ifdef AXIOM_PROFILER_ENABLED
#    include <psapi.h>
#    pragma comment(lib, "psapi.lib")
#  endif
#endif
#include <Utils/Timer.hpp>
#include <Utils/StringHelper.hpp>

#include "Input.hpp"
#include "Project/ProjectManager.hpp"
#include "Project/AxiomProject.hpp"
#include <GLFW/glfw3.h>

namespace Axiom {
	const float Application::k_PausedTargetFrameRate = 10;

	Application* Application::s_Instance = nullptr;
	Application::CommandLineArgs Application::s_CommandLineArgs{};

	Application::Application()
		: m_SceneManager(std::make_unique<SceneManager>())
		, m_FixedUpdateAccumulator{ 0 }
	{
		Application::s_Instance = this;
	}

	Application::~Application()
	{
		if (s_Instance == this) {
			s_Instance = nullptr;
		}
	}

	void Application::Run()
	{
		if (m_ForceSingleInstance) {
			static SingleInstance instance(m_Name);
			AIM_ASSERT(!instance.IsAlreadyRunning(), AxiomErrorCode::Undefined, "An Instance of this app is already running!");
		}

		for (;;) {
			AIM_INFO_TAG("Application", "Initializing...");
			Timer timer = Timer();
			try {
				Initialize();
			}
			catch (const std::exception& e) {
				AIM_ERROR_TAG("Application", std::string("Initialization failed: ") + e.what());
				Shutdown(false);
				return;
			}
			catch (...) {
				AIM_ERROR_TAG("Application", "Initialization failed with unknown exception");
				Shutdown(false);
				return;
			}
			AIM_INFO_TAG("Application", "Full Initialization took " + StringHelper::ToString(timer));

			try {
				Start();
			}
			catch (const std::exception& e) {
				AIM_ERROR_TAG("Application", std::string("Start failed: ") + e.what());
				Shutdown();
				return;
			}
			catch (...) {
				AIM_ERROR_TAG("Application", "Start failed with unknown exception");
				Shutdown();
				return;
			}
			m_LastFrameTime = Clock::now();

			while (m_Window && !m_Window->ShouldClose() && !m_ShouldQuit) {
				const float targetFps = Max(GetTargetFramerate(), 0.0f);
				DurationChrono targetFrameTime{};
				if (targetFps > 0.0f) {
					targetFrameTime = std::chrono::duration_cast<DurationChrono>(std::chrono::duration<double>(1.0 / targetFps));
				}
				auto now = Clock::now();

				// "VSync" bucket — accumulates time spent waiting between frames.
				// Two contributors: the soft frame-cap idle below (when v-sync is
				// off) and the hard SwapBuffers wait (when v-sync is on). Since
				// SwapBuffers happens inside RenderPipelineOnly which we don't
				// instrument, we approximate by measuring the gap between when
				// last frame's render completed and when we resume here. That
				// gap is what the user perceives as "the frame is paced".
				const auto vsyncStart = m_LastFrameEndTime != Clock::time_point{}
					? m_LastFrameEndTime
					: now;

				// CPU idling for runtime fps caps. The editor renders Game View pacing separately.
				// E21: replaced the previous "sleep_until(target - 10ms) + busy yield"
				// pattern with a single sleep_until(target - 1ms). On Windows the
				// system timer resolution is raised to 1ms via timeBeginPeriod(1)
				// in Initialize() (released in Shutdown()), so sleep_until's
				// undershoot collapses to ~1ms — well within tolerance for a
				// frame cap and far cheaper than a per-iteration yield syscall.
				if (m_Configuration.UseTargetFrameRateForMainLoop && targetFps > 0.0f && (!m_Window->IsVsync() || IsEnginePaused()))
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
					// One zone per whole frame. The macro expands to a Tracy
					// ZoneScopedN("Frame") AND an in-engine ring-buffer push
					// (the panel reads from the latter). Both are no-ops
					// when the profiler is stripped.
					AXIOM_PROFILE_SCOPE("Frame");

					// FPS + Frame Time both derived from this single dt so
					// they can never drift apart — no separate measurement
					// for either.
					Profiler::PushFrameDelta(deltaTime);

					m_Time.Update(deltaTime);

					m_FixedUpdateAccumulator += m_Time.GetDeltaTime();
					while (m_FixedUpdateAccumulator >= m_Time.GetUnscaledFixedDeltaTime()) {
						if (!IsEnginePaused() && !m_IsPlaymodePaused) {
							BeginFixedFrame();
							for (const auto& layer : m_LayerStack) {
								layer->OnFixedUpdate(*this, m_Time.GetFixedDeltaTime());
							}
							EndFixedFrame();
						}

						m_FixedUpdateAccumulator -= m_Time.GetUnscaledFixedDeltaTime();
					}

					BeginFrame();
					EndFrame();
					TryCompleteQuitRequest();

					glfwPollEvents();
					TryCompleteQuitRequest();

					m_LastFrameTime = frameStart;
					m_Time.AdvanceFrameCount();

#ifdef AXIOM_PROFILER_ENABLED
					// VSync (between-frame idle) — measured at top of next frame
					// against m_LastFrameEndTime. Small enough that we don't want
					// to gate it; the profiler's per-module cadence handles that.
					AXIOM_PROFILE_VALUE("VSync", vsyncMs);

					// Memory category. Total Memory = process working set;
					// Texture Memory = sum of loaded texture pixels (RGBA8);
					// Object Count = live entity count across all loaded scenes.
#  ifdef AIM_PLATFORM_WINDOWS
					{
						PROCESS_MEMORY_COUNTERS pmc{};
						if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
							AXIOM_PROFILE_VALUE("Total Memory", float(pmc.WorkingSetSize / (1024 * 1024)));
						}
					}
#  endif
					{
						const std::size_t textureBytes = TextureManager::GetTotalTextureMemoryBytes();
						AXIOM_PROFILE_VALUE("Texture Memory", float(textureBytes / (1024 * 1024)));
					}

					// Object Count: sum of live entities across loaded scenes.
					{
						std::size_t totalEntities = 0;
						if (m_SceneManager) {
							m_SceneManager->ForeachLoadedScene([&totalEntities](Scene& scene) {
								auto& entityStorage = scene.GetRegistry().storage<entt::entity>();
								totalEntities += entityStorage.free_list();
							});
						}
						AXIOM_PROFILE_VALUE("Object Count", float(totalEntities));
					}

					// Audio category.
					AXIOM_PROFILE_VALUE("Playing Sources", float(AudioManager::GetActiveSoundCount()));

					// "Others" residual: total Frame Time minus the four named
					// CPU buckets we already attributed. Surfaces work that's
					// not in Render/Scripts/Physics/VSync — engine overhead,
					// glfwPollEvents, Time::Update, layer dispatching, etc.
					{
						const float frameMs    = Profiler::GetCurrentValue("Frame Time");
						const float renderMs   = Profiler::GetCurrentValue("Rendering");
						const float scriptsMs  = Profiler::GetCurrentValue("Scripts");
						const float physicsMs  = Profiler::GetCurrentValue("Physics");
						const float others = frameMs - renderMs - scriptsMs - physicsMs - vsyncMs;
						AXIOM_PROFILE_VALUE("Others", std::max(0.0f, others));
					}
#endif

					// End-of-frame Tracy mark + sampling-cadence advance.
					AXIOM_PROFILE_FRAME("Frame");

					// Stamp end-of-frame for next iteration's VSync measurement.
					// Has to be the very last thing in the try block — anything
					// after it eats into the bucket.
					m_LastFrameEndTime = Clock::now();
				}
				catch (const std::exception& e) {
					m_IsRenderingFrame = false;
					AIM_ERROR_TAG("Application", "Unhandled frame exception: {}", e.what());
					m_ShouldQuit = true;
				}
				catch (...) {
					m_IsRenderingFrame = false;
					AIM_ERROR_TAG("Application", "Unhandled frame exception: unknown exception");
					m_ShouldQuit = true;
				}
			}

			Shutdown();

			if (!m_CanReload) break;
			m_CanReload = false;
		}
	}

	void Application::Initialize() {
#ifdef AIM_PLATFORM_WINDOWS
		// E21: raise the Windows system timer resolution to 1ms once at app
		// init so sleep_until in the frame-cap path is accurate. Paired with
		// timeEndPeriod(1) in Shutdown(). Do NOT call this per frame.
		timeBeginPeriod(1);
#endif
		m_Configuration = GetConfiguration();
		SetName(m_Configuration.WindowSpecification.Title);

		// Profiler comes up first — it has no GL/window dependencies and
		// every later subsystem can already emit AXIOM_PROFILE_SCOPE marks
		// during its own init. Stripped builds skip this entire block.
#ifdef AXIOM_PROFILER_ENABLED
		Profiler::Initialize();
#endif

		Timer timer = Timer();
		Window::Initialize();
		m_Window = std::make_unique<Window>(m_Configuration.WindowSpecification);
		m_Window->SetVsync(m_Configuration.Vsync);
		m_Window->SetEventCallback([this](AxiomEvent& e) { DispatchEvent(e); });
		AIM_INFO_TAG("Window", "Initialization took " + StringHelper::ToString(timer));

		timer.Reset();
		OpenGL::Initialize(GLInitSpecifications(Color::Background(), GLCullingMode::GLBack));
		AIM_INFO_TAG("OpenGL", "Initialization took " + StringHelper::ToString(timer));

		if (m_Configuration.EnableRenderer2D) {
			timer.Reset();
			m_Renderer2D = std::make_unique<Renderer2D>();
			m_Renderer2D->Initialize();
			m_Renderer2D->SetSceneProvider([this](const std::function<void(const Scene&)>& fn) {
				if (m_SceneManager) {
					m_SceneManager->ForeachLoadedScene(fn);
				}
				});
			AIM_INFO_TAG("Renderer2D", "Initialization took " + StringHelper::ToString(timer));
		}

		if (m_Configuration.EnableGizmoRenderer) {
			timer.Reset();
			m_GizmoRenderer2D = std::make_unique<GizmoRenderer2D>();
			m_GizmoRenderer2D->Initialize();
			AIM_INFO_TAG("GizmoRenderer", "Initialization took " + StringHelper::ToString(timer));
		}

		if (m_Configuration.EnableGuiRenderer) {
			timer.Reset();
			m_GuiRenderer = std::make_unique<GuiRenderer>();
			m_GuiRenderer->Initialize();
			AIM_INFO_TAG("GuiRenderer", "Initialization took " + StringHelper::ToString(timer));
		}

		if (m_Configuration.EnablePhysics2D) {
			timer.Reset();
			m_PhysicsSystem2D = std::make_unique<PhysicsSystem2D>();
			m_PhysicsSystem2D->Initialize();
			AIM_INFO_TAG("PhysicsSystem", "Initialization took " + StringHelper::ToString(timer));
		}

		if (m_Configuration.EnableTextureManager) {
			timer.Reset();
			TextureManager::Initialize();
			AIM_INFO_TAG("TextureManager", "Initialization took " + StringHelper::ToString(timer));
		}

		if (m_Configuration.EnableAudio) {
			timer.Reset();
			if (AudioManager::Initialize()) {
				AIM_INFO_TAG("AudioManager", "Initialization took " + StringHelper::ToString(timer));
			}
			else {
				AIM_ERROR_TAG("AudioManager", "Initialization failed. Continuing without audio.");
				m_Configuration.EnableAudio = false;
			}
		}

		timer.Reset();

		ConfigureScenes();
		m_SceneManager->Initialize();
		AIM_INFO_TAG("SceneManager", "Initialization took " + StringHelper::ToString(timer));
		ConfigureLayers();

		if (m_Configuration.SetWindowIcon) {
			m_Window->SetWindowIconFromResource();

			AxiomProject* project = ProjectManager::GetCurrentProject();
			if (project && !project->AppIconPath.empty()) {
				TextureHandle h = TextureManager::LoadTexture(project->AppIconPath);
				Texture2D* tex = TextureManager::GetTexture(h);
				if (tex && tex->IsValid()) {
					m_Window->SetWindowIcon(tex);
				}
			}
		}

		if (m_Configuration.EnablePackageHost) {
			// Load Axiom packages discovered next to the running executable. Each package's
			// AxiomPackage_OnLoad runs once here so packages can register with engine
			// subsystems (script bindings, component types, etc.) before Start().
			// MUST run before InitializeStartupScenes so package-registered
			// component types are present when scene deserialization runs.
			PackageHost::LoadAll();
		}

		// Load startup scenes AFTER package registration so package components
		// (e.g. Tilemap2DComponent) round-trip through the deserialize sweep
		// in SceneSerializerDeserialize.cpp.
		m_SceneManager->InitializeStartupScenes();

		ScriptEngine::RaiseApplicationStart();
	}

	void Application::BeginFrame() {
		AXIOM_PROFILE_SCOPE("Application::BeginFrame");
		m_IsRenderingFrame = true;

		CoreInput();

		const bool enginePaused = IsEnginePaused();
		if (enginePaused && !m_WasEnginePaused) {
			ScriptEngine::RaiseApplicationPaused();
		}
		m_WasEnginePaused = enginePaused;

		if (!enginePaused) {
			bool gameplayActive = m_IsPlaying && !m_IsPlaymodePaused;

			if (gameplayActive && m_Configuration.EnableAudio) 
				AudioManager::Update();

			if (gameplayActive)
				ScriptEngine::UpdateGlobalSystems();

			Update();

			for (const auto& layer : m_LayerStack) {
				layer->OnUpdate(*this, m_Time.GetDeltaTime());
			}

			if (gameplayActive && m_SceneManager) m_SceneManager->UpdateScenes();

			if (m_SceneManager) m_SceneManager->OnPreRenderScenes();
			for (const auto& layer : m_LayerStack) {
				AXIOM_TRY_CATCH_LOG(layer->OnPreRender(*this));
			}

			if (m_Renderer2D)
				AXIOM_TRY_CATCH_LOG(m_Renderer2D->BeginFrame());

			if (m_GuiRenderer)
				AXIOM_TRY_CATCH_LOG(m_GuiRenderer->BeginFrame(*m_SceneManager));

			if (m_GizmoRenderer2D)
				AXIOM_TRY_CATCH_LOG(m_GizmoRenderer2D->BeginFrame());
		}
		else {
			OnPaused();
		}
	}

	void Application::EndFrame() {
		AXIOM_PROFILE_SCOPE("Application::EndFrame");
		if (!IsEnginePaused()) {
			RenderPipelineOnly();
		}

		m_Input.Update();
		m_IsRenderingFrame = false;
	}

	void Application::DispatchEvent(AxiomEvent& event) {
		EventDispatcher dispatcher(event);

		dispatcher.Dispatch<WindowCloseEvent>([this](WindowCloseEvent&) {
			RequestQuit();
			glfwSetWindowShouldClose(m_Window->GetGLFWWindow(), GLFW_FALSE);
			return false;
			});

		dispatcher.Dispatch<WindowResizeEvent>([this](WindowResizeEvent& e) {
			m_IsMinimized = e.GetWidth() == 0 || e.GetHeight() == 0;
			return false;
			});

		dispatcher.Dispatch<FileDropEvent>([this](FileDropEvent& e) {
			m_PendingFileDrops = e.GetPaths();
			return false;
			});

		for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it) {
			if (event.Handled) {
				break;
			}
			(*it)->OnEvent(*this, event);
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
		if (m_PhysicsSystem2D) m_PhysicsSystem2D->FixedUpdate(m_Time.GetFixedDeltaTime());
		// H7: GlobalSystem.OnFixedUpdate dispatch — paired with the per-scene
		// FixedUpdate above. Runs after physics so global systems observe
		// transforms already synced from the physics step.
		ScriptEngine::FixedUpdateGlobalSystems();
	}

	void Application::EndFixedFrame() { }

	void Application::RenderPipelineOnly() {
		if (m_Renderer2D)
			AXIOM_TRY_CATCH_LOG(m_Renderer2D->EndFrame());

		for (const auto& layer : m_LayerStack) {
			AXIOM_TRY_CATCH_LOG(layer->OnPostRender(*this));
		}

		if (m_GuiRenderer)
			AXIOM_TRY_CATCH_LOG(m_GuiRenderer->EndFrame());
		if (m_GizmoRenderer2D)
			AXIOM_TRY_CATCH_LOG(m_GizmoRenderer2D->EndFrame());

		if (m_Window) m_Window->SwapBuffers();
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

		if (m_SceneManager) AXIOM_TRY_CATCH_LOG(m_SceneManager->OnPreRenderScenes());
		for (const auto& layer : m_LayerStack) {
			AXIOM_TRY_CATCH_LOG(layer->OnPreRender(*this));
		}

		if (m_Renderer2D) {
			AXIOM_TRY_CATCH_LOG(m_Renderer2D->BeginFrame());
			AXIOM_TRY_CATCH_LOG(m_Renderer2D->EndFrame());
		}

		for (const auto& layer : m_LayerStack) {
			AXIOM_TRY_CATCH_LOG(layer->OnPostRender(*this));
		}

		if (m_GuiRenderer && m_SceneManager) {
			AXIOM_TRY_CATCH_LOG(m_GuiRenderer->BeginFrame(*m_SceneManager));
			AXIOM_TRY_CATCH_LOG(m_GuiRenderer->EndFrame());
		}

		if (m_GizmoRenderer2D) {
			AXIOM_TRY_CATCH_LOG(m_GizmoRenderer2D->BeginFrame());
			AXIOM_TRY_CATCH_LOG(m_GizmoRenderer2D->EndFrame());
		}

		if (m_Window) m_Window->SwapBuffers();
	}


	void Application::CoreInput() {
		if (m_Input.GetKey(KeyCode::LeftControl) && m_Input.GetKeyDown(KeyCode::I))
		{
			AIM_INFO_TAG("Debug", "From: " + m_Name);
			AIM_INFO_TAG("Debug", "Current FPS: " + StringHelper::ToString(m_Time.GetFrameRate()));
			AIM_INFO_TAG("Debug", "Time Elapsed Since Start: " + StringHelper::ToString(m_Time.GetElapsedTime(), " s"));
		}

		if (m_Input.GetKeyDown(KeyCode::Esc)) {
			if (m_Window) m_Window->MinimizeWindow();
		}
		if (m_Input.GetKeyDown(KeyCode::F11)) {
			if (m_Window) m_Window->SetFullScreen(!m_Window->IsFullScreen());
		}
	}

	void Application::ResetTimePoints() {
		m_LastFrameTime = Clock::now();
		m_FixedUpdateAccumulator = 0;
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
				AIM_ERROR_TAG("Application", std::string("OnQuit failed: ") + e.what());
			}
			catch (...) {
				AIM_ERROR_TAG("Application", "OnQuit failed with unknown exception");
			}
		}

		for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it) {
			(*it)->OnDetach(*this);
		}
		m_EventBus.Clear();
		m_LayerStack.Clear();

		// H10: PackageHost::UnloadAll() must run AFTER every subsystem that
		// could call into package code. Packages may register components,
		// script bindings, or other callbacks; FreeLibrary'ing them while
		// any subsystem still holds those callbacks is a recipe for use-
		// after-unmap. Order here: scenes/scripts down first (they invoke
		// on_destroy hooks that may live in package code), then engine
		// resource managers, then PackageHost::UnloadAll() *last*.
		if (m_SceneManager) m_SceneManager->Shutdown();
		if (ScriptEngine::IsInitialized()) ScriptEngine::Shutdown();
		if (m_Configuration.EnableTextureManager) TextureManager::Shutdown();

		if (m_PhysicsSystem2D) m_PhysicsSystem2D->Shutdown();
		if (m_GuiRenderer) m_GuiRenderer->Shutdown();
		if (m_GizmoRenderer2D) m_GizmoRenderer2D->Shutdown();
		if (m_Renderer2D) m_Renderer2D->Shutdown();

		if (AudioManager::IsInitialized())
			AudioManager::Shutdown();

		// H10: PackageHost teardown is the last subsystem call. By this
		// point no engine callback table or registry holds package code.
		if (m_Configuration.EnablePackageHost) PackageHost::UnloadAll();

		if (m_Window) {
			m_Window->SetEventCallback({});
			m_Window->Destroy();
		}
		if (Window::IsInitialized()) {
			Window::Shutdown();
		}

		m_GuiRenderer.reset();
		m_GizmoRenderer2D.reset();
		m_Renderer2D.reset();
		m_PhysicsSystem2D.reset();
		m_Window.reset();

#ifdef AXIOM_PROFILER_ENABLED
		// Profiler comes down last so anything destructed above could still
		// emit AXIOM_PROFILE_SCOPE marks during teardown without segfaulting
		// on the static module table.
		Profiler::Shutdown();
#endif

#ifdef AIM_PLATFORM_WINDOWS
		// E21: pair to the timeBeginPeriod(1) in Initialize().
		timeEndPeriod(1);
#endif

		m_ShouldQuit = false;
		m_QuitRequested = false;
		m_QuitRequestFrame = -1;
		m_IsPaused = false;
		m_IsBackgroundPaused = false;
		m_IsMinimized = false;
		m_WindowHasFocus = true;
		m_IsPlaymodePaused = false;
		m_IsGameInputEnabled = true;
		m_IsShuttingDown = false;
		m_WasEnginePaused = false;
		m_FixedUpdateAccumulator = 0.0;
		m_PendingFileDrops.clear();
	}
}
