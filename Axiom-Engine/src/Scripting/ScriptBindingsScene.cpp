#include "pch.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Scripting/ScriptBindings.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Core/Application.hpp"
#include "Core/Input.hpp"
#include "Core/Time.hpp"
#include "Core/Window.hpp"
#include "Core/Log.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/SceneDefinition.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/File.hpp"
#include "Project/ProjectManager.hpp"
#include "Project/AxiomProject.hpp"
#include "Scripting/ScriptSystem.hpp"
#include <Systems/ParticleUpdateSystem.hpp>
#include <Systems/AudioUpdateSystem.hpp>
#include "Scene/ComponentRegistry.hpp"
#include "Components/General/UUIDComponent.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Components/Physics/BoxCollider2DComponent.hpp"
#include "Components/Physics/FastBody2DComponent.hpp"
#include "Components/Physics/FastBoxCollider2DComponent.hpp"
#include "Components/Physics/FastCircleCollider2DComponent.hpp"
#include "Components/Audio/AudioSourceComponent.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/Tags.hpp"
#include "Audio/AudioManager.hpp"
#include "Graphics/TextureManager.hpp"
#include "Graphics/Gizmo.hpp"
#include "Graphics/OpenGL.hpp"
#include "Physics/Physics2D.hpp"
#include "Core/Version.hpp"

namespace Axiom {
	EntityHandle ToEntityHandle(uint64_t id);
	uint64_t FromEntityHandle(EntityHandle handle);
	uint64_t GetEntityScriptId(const Scene& scene, EntityHandle handle);
	bool TryResolveEntityByUUID(const Scene& scene, uint64_t entityID, EntityHandle& outHandle);
	bool ResolveEntityReference(uint64_t entityID, Scene*& outScene, EntityHandle& outHandle);
	Scene* GetScene();
	extern thread_local std::string s_StringReturnBuffer;

	// Thread-local buffer for returning strings to managed code
	// ── Application Bindings ────────────────────────────────────────────

	static float Axiom_Application_GetDeltaTime()
	{
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetDeltaTime() : 0.0f;
	}

	static float Axiom_Application_GetTime()
	{
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetElapsedTime() : 0.0f;
	}

	static int Axiom_Application_GetScreenWidth()
	{
		auto* window = Application::GetWindow();
		return window ? window->GetWidth() : 0;
	}

	static int Axiom_Application_GetScreenHeight()
	{
		auto* window = Application::GetWindow();
		return window ? window->GetHeight() : 0;
	}

	static float Axiom_Application_GetTargetFrameRate() { return Application::GetTargetFramerate(); }
	static void  Axiom_Application_SetTargetFrameRate(float fps) { Application::SetTargetFramerate(fps); }

	static void Axiom_Application_Quit() {
		Application* app = Application::GetInstance();
		if (!app) return;
		if (Application::IsEditor()) {
			// In editor: route through the editor's stop-play path
			// instead of closing the editor window. Without this hook,
			// the call was a silent no-op and the user couldn't end a
			// play session from script (had to click Stop manually);
			// with it, scripted Application.Quit() inside the editor
			// behaves like clicking Stop. The editor's pre-render
			// drains the flag via ApplicationEditorAccess.
			Application::RequestEditorStopPlay();
			return;
		}
		Application::Quit();
	}

	// Runtime sibling of the compile-time AXIOM_EDITOR define. True when
	// the host process is the editor binary, false in shipped runtime
	// builds. Useful for runtime branches that can't be #if'd because
	// the same script DLL feeds both editor preview and ship builds.
	static int Axiom_Application_IsEditor() {
		return Application::IsEditor() ? 1 : 0;
	}

	static float Axiom_Application_GetFixedDeltaTime() {
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetFixedDeltaTime() : (1.0f / 50.0f);
	}
	static float Axiom_Application_GetUnscaledDeltaTime() {
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetDeltaTimeUnscaled() : 0.0f;
	}
	static float Axiom_Application_GetFixedUnscaledDeltaTime() {
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetUnscaledFixedDeltaTime() : (1.0f / 50.0f);
	}
	static float Axiom_Application_GetTimeScale() {
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetTimeScale() : 1.0f;
	}
	static void Axiom_Application_SetTimeScale(float scale) {
		auto* app = Application::GetInstance();
		if (app) app->GetTime().SetTimeScale(scale);
	}

	// Two-call buffer pattern: C# calls once with outBuffer=null/capacity=0 to learn the
	// required size, then again with a sized buffer. Stash into the thread-local so the
	// reported size matches the bytes copied on the second call.
	static int Axiom_Application_GetClipboardStringBuffer(char* outBuffer, int capacity)
	{
		auto* window = Application::GetWindow();
		s_StringReturnBuffer = window ? window->GetClipboardString() : std::string{};

		const int requiredBytes = static_cast<int>(std::min(
			s_StringReturnBuffer.size(),
			static_cast<size_t>(std::numeric_limits<int>::max())));
		if (outBuffer && capacity > 0) {
			const int bytesToCopy = std::min(requiredBytes, capacity - 1);
			if (bytesToCopy > 0) {
				std::memcpy(outBuffer, s_StringReturnBuffer.data(), static_cast<size_t>(bytesToCopy));
			}
			outBuffer[bytesToCopy] = '\0';
		}
		return requiredBytes;
	}

	static void Axiom_Application_SetClipboardString(const char* value)
	{
		auto* window = Application::GetWindow();
		if (!window) return;
		window->SetClipboardString(value ? std::string(value) : std::string{});
	}

	static int Axiom_Application_GetVsyncEnabled()
	{
		return Window::IsVsync() ? 1 : 0;
	}

	static void Axiom_Application_SetVsyncEnabled(int enabled)
	{
		Window::SetVsync(enabled != 0);
	}

	// ── Engine Bindings ─────────────────────────────────────────────────

	// Same two-call buffer pattern as Application_GetClipboardStringBuffer.
	static int CopyToManagedBuffer(std::string_view source, char* outBuffer, int capacity)
	{
		s_StringReturnBuffer.assign(source);
		const int requiredBytes = static_cast<int>(std::min(
			s_StringReturnBuffer.size(),
			static_cast<size_t>(std::numeric_limits<int>::max())));
		if (outBuffer && capacity > 0) {
			const int bytesToCopy = std::min(requiredBytes, capacity - 1);
			if (bytesToCopy > 0) {
				std::memcpy(outBuffer, s_StringReturnBuffer.data(), static_cast<size_t>(bytesToCopy));
			}
			outBuffer[bytesToCopy] = '\0';
		}
		return requiredBytes;
	}

	static int Axiom_Engine_GetVersionBuffer(char* outBuffer, int capacity)
	{
		return CopyToManagedBuffer(AIM_VERSION, outBuffer, capacity);
	}

	static int Axiom_Engine_GetVersionLongBuffer(char* outBuffer, int capacity)
	{
		return CopyToManagedBuffer(AIM_VERSION_LONG, outBuffer, capacity);
	}

	// 0 = Debug (editor preview), 1 = Development, 2 = Release. Editor mode
	// always reports Debug — that mirrors the AXIOM_EDITOR define being the
	// "I am iterating" signal from the C# side. Outside the editor we honor
	// the project's ActiveBuildProfile which drives AXIOM_BUILD_DEVELOPMENT
	// vs AXIOM_BUILD_RELEASE for shipped game binaries.
	static int Axiom_Engine_GetBuildConfiguration()
	{
		if (Application::IsEditor()) return 0;
		auto* project = ProjectManager::GetCurrentProject();
		if (project && project->ActiveBuildProfile == AxiomProject::BuildProfile::Release) return 2;
		return 1;
	}

	static int Axiom_Engine_GetPlatformBuffer(char* outBuffer, int capacity)
	{
#if defined(AIM_PLATFORM_WINDOWS)
		return CopyToManagedBuffer("Windows", outBuffer, capacity);
#elif defined(AIM_PLATFORM_LINUX)
		return CopyToManagedBuffer("Linux", outBuffer, capacity);
#else
		return CopyToManagedBuffer("Unknown", outBuffer, capacity);
#endif
	}

	static int Axiom_Engine_GetGraphicsApiBuffer(char* outBuffer, int capacity)
	{
		// Returns the version string set by the active backend (e.g. "bgfx Vulkan").
		// Empty until the renderer initializes.
		return CopyToManagedBuffer(OpenGL::GetVersionString(), outBuffer, capacity);
	}

	static int Axiom_Engine_GetGpuVendorBuffer(char* outBuffer, int capacity)
	{
		return CopyToManagedBuffer(OpenGL::GetVendorString(), outBuffer, capacity);
	}

	static int Axiom_Engine_GetGpuRendererBuffer(char* outBuffer, int capacity)
	{
		return CopyToManagedBuffer(OpenGL::GetRendererString(), outBuffer, capacity);
	}

	// ── Time Bindings ───────────────────────────────────────────────────

	static int Axiom_Time_GetFrameCount()
	{
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetFrameCount() : 0;
	}

	static float Axiom_Time_GetTimeSinceStartup()
	{
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetTimeSinceStartup() : 0.0f;
	}

	static float Axiom_Time_GetRealtimeSinceStartup()
	{
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetRealtimeSinceStartup() : 0.0f;
	}

	// ── Log Bindings ────────────────────────────────────────────────────

	static void Axiom_Log_Trace(const char* message) { Log::PrintMessageTag(Log::Type::Client, Log::Level::Trace, "Script", message); }
	static void Axiom_Log_Info(const char* message)  { Log::PrintMessageTag(Log::Type::Client, Log::Level::Info, "Script", message); }
	static void Axiom_Log_Warn(const char* message)  { Log::PrintMessageTag(Log::Type::Client, Log::Level::Warn, "Script", message); }
	static void Axiom_Log_Error(const char* message) { Log::PrintMessageTag(Log::Type::Client, Log::Level::Error, "Script", message); }

	// ── Input Bindings ──────────────────────────────────────────────────

	static bool CanReadScriptInput()
	{
		auto* app = Application::GetInstance();
		return app && ScriptBindings::IsScriptInputEnabled();
	}

	static int Axiom_Input_GetKey(int keyCode)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetKey(static_cast<KeyCode>(keyCode)) ? 1 : 0) : 0;
	}

	static int Axiom_Input_GetKeyDown(int keyCode)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetKeyDown(static_cast<KeyCode>(keyCode)) ? 1 : 0) : 0;
	}

	static int Axiom_Input_GetKeyUp(int keyCode)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetKeyUp(static_cast<KeyCode>(keyCode)) ? 1 : 0) : 0;
	}

	static int Axiom_Input_GetMouseButton(int button)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetMouse(static_cast<MouseButton>(button)) ? 1 : 0) : 0;
	}

	static int Axiom_Input_GetMouseButtonDown(int button)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetMouseDown(static_cast<MouseButton>(button)) ? 1 : 0) : 0;
	}

	static int Axiom_Input_GetMouseButtonUp(int button)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetMouseUp(static_cast<MouseButton>(button)) ? 1 : 0) : 0;
	}

	static void Axiom_Input_GetMousePosition(float* outX, float* outY)
	{
		auto* app = Application::GetInstance();
		if (CanReadScriptInput()) { Vec2 pos = app->GetInput().GetMousePosition(); *outX = pos.x; *outY = pos.y; }
		else { *outX = 0.0f; *outY = 0.0f; }
	}

	static void Axiom_Input_GetAxis(float* outX, float* outY) {
		auto* app = Application::GetInstance();
		if (CanReadScriptInput()) { Vec2 axis = app->GetInput().GetAxis(); *outX = axis.x; *outY = axis.y; }
		else { *outX = 0.0f; *outY = 0.0f; }
	}
	static void Axiom_Input_GetMouseDelta(float* outX, float* outY) {
		auto* app = Application::GetInstance();
		if (CanReadScriptInput()) { Vec2 delta = app->GetInput().GetMouseDelta(); *outX = delta.x; *outY = delta.y; }
		else { *outX = 0.0f; *outY = 0.0f; }
	}
	static float Axiom_Input_GetScrollWheelDelta() {
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? app->GetInput().ScrollValue() : 0.0f;
	}

	// ── Entity Bindings ─────────────────────────────────────────────────

	static int Axiom_Entity_IsValid(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		return ResolveEntityReference(entityID, scene, handle) ? 1 : 0;
	}

	static uint64_t Axiom_Entity_FindByName(const char* name)
	{
		Scene* scene = GetScene();
		if (!scene || !name) return 0;
		std::string targetName(name);
		auto& registry = scene->GetRegistry();
		auto view = registry.view<NameComponent>();
		for (auto [entity, nameComp] : view.each())
			if (nameComp.Name == targetName) return GetEntityScriptId(*scene, entity);
		return 0;
	}

	static void Axiom_Entity_Destroy(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return;
		scene->DestroyEntity(handle);
	}

	static uint64_t Axiom_Entity_Create(const char* name)
	{
		Scene* scene = GetScene();
		if (!scene) return 0;
		Entity entity = scene->CreateRuntimeEntity(name ? name : "Entity");
		return GetEntityScriptId(*scene, entity.GetHandle());
	}

	void PopulateNonComponentBindings(NativeBindings& b)
	{
		b.Application_GetDeltaTime = &Axiom_Application_GetDeltaTime;
		b.Application_GetElapsedTime = &Axiom_Application_GetTime;
		b.Application_GetScreenWidth = &Axiom_Application_GetScreenWidth;
		b.Application_GetScreenHeight = &Axiom_Application_GetScreenHeight;
		b.Application_IsEditor = &Axiom_Application_IsEditor;
		b.Application_GetTargetFrameRate = &Axiom_Application_GetTargetFrameRate;
		b.Application_SetTargetFrameRate = &Axiom_Application_SetTargetFrameRate;
		b.Application_Quit = &Axiom_Application_Quit;
		b.Application_GetFixedDeltaTime = &Axiom_Application_GetFixedDeltaTime;
		b.Application_GetUnscaledDeltaTime = &Axiom_Application_GetUnscaledDeltaTime;
		b.Application_GetFixedUnscaledDeltaTime = &Axiom_Application_GetFixedUnscaledDeltaTime;
		b.Application_GetTimeScale = &Axiom_Application_GetTimeScale;
		b.Application_SetTimeScale = &Axiom_Application_SetTimeScale;
		b.Application_GetClipboardStringBuffer = &Axiom_Application_GetClipboardStringBuffer;
		b.Application_SetClipboardString = &Axiom_Application_SetClipboardString;
		b.Application_GetVsyncEnabled = &Axiom_Application_GetVsyncEnabled;
		b.Application_SetVsyncEnabled = &Axiom_Application_SetVsyncEnabled;

		b.Engine_GetVersionBuffer = &Axiom_Engine_GetVersionBuffer;
		b.Engine_GetVersionLongBuffer = &Axiom_Engine_GetVersionLongBuffer;
		b.Engine_GetBuildConfiguration = &Axiom_Engine_GetBuildConfiguration;
		b.Engine_GetPlatformBuffer = &Axiom_Engine_GetPlatformBuffer;
		b.Engine_GetGraphicsApiBuffer = &Axiom_Engine_GetGraphicsApiBuffer;
		b.Engine_GetGpuVendorBuffer = &Axiom_Engine_GetGpuVendorBuffer;
		b.Engine_GetGpuRendererBuffer = &Axiom_Engine_GetGpuRendererBuffer;

		b.Time_GetFrameCount = &Axiom_Time_GetFrameCount;
		b.Time_GetTimeSinceStartup = &Axiom_Time_GetTimeSinceStartup;
		b.Time_GetRealtimeSinceStartup = &Axiom_Time_GetRealtimeSinceStartup;

		b.Log_Trace = &Axiom_Log_Trace;
		b.Log_Info = &Axiom_Log_Info;
		b.Log_Warn = &Axiom_Log_Warn;
		b.Log_Error = &Axiom_Log_Error;

		b.Input_GetKey = &Axiom_Input_GetKey;
		b.Input_GetKeyDown = &Axiom_Input_GetKeyDown;
		b.Input_GetKeyUp = &Axiom_Input_GetKeyUp;
		b.Input_GetMouseButton = &Axiom_Input_GetMouseButton;
		b.Input_GetMouseButtonDown = &Axiom_Input_GetMouseButtonDown;
		b.Input_GetMouseButtonUp = &Axiom_Input_GetMouseButtonUp;
		b.Input_GetMousePosition = &Axiom_Input_GetMousePosition;
		b.Input_GetAxis = &Axiom_Input_GetAxis;
		b.Input_GetMouseDelta = &Axiom_Input_GetMouseDelta;
		b.Input_GetScrollWheelDelta = &Axiom_Input_GetScrollWheelDelta;

		b.Entity_IsValid = &Axiom_Entity_IsValid;
		b.Entity_FindByName = &Axiom_Entity_FindByName;
		b.Entity_Destroy = &Axiom_Entity_Destroy;
		b.Entity_Create = &Axiom_Entity_Create;
	}

} // namespace Axiom
