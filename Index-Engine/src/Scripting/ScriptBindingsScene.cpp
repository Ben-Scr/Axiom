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
#include "Project/IndexProject.hpp"
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
#include "Graphics/RenderApi.hpp"
#include "Physics/Physics2D.hpp"
#include "Core/Version.hpp"

namespace Index {
	EntityHandle ToEntityHandle(uint64_t id);
	uint64_t FromEntityHandle(EntityHandle handle);
	uint64_t GetEntityScriptId(const Scene& scene, EntityHandle handle);
	bool TryResolveEntityByUUID(const Scene& scene, uint64_t entityID, EntityHandle& outHandle);
	bool ResolveEntityReference(uint64_t entityID, Scene*& outScene, EntityHandle& outHandle);
	Scene* GetScene();
	extern thread_local std::string s_StringReturnBuffer;

	// Thread-local buffer for returning strings to managed code
	// ── Application Bindings ────────────────────────────────────────────

	static float Index_Application_GetDeltaTime()
	{
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetDeltaTime() : 0.0f;
	}

	static float Index_Application_GetTime()
	{
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetElapsedTime() : 0.0f;
	}

	static int Index_Application_GetScreenWidth()
	{
		auto* window = Application::GetWindow();
		return window ? window->GetWidth() : 0;
	}

	static int Index_Application_GetScreenHeight()
	{
		auto* window = Application::GetWindow();
		return window ? window->GetHeight() : 0;
	}

	static float Index_Application_GetTargetFrameRate() { return Application::GetTargetFramerate(); }
	static void  Index_Application_SetTargetFrameRate(float fps) { Application::SetTargetFramerate(fps); }

	static void Index_Application_Quit() {
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

	// Runtime sibling of the compile-time INDEX_EDITOR define. True when
	// the host process is the editor binary, false in shipped runtime
	// builds. Useful for runtime branches that can't be #if'd because
	// the same script DLL feeds both editor preview and ship builds.
	static int Index_Application_IsEditor() {
		return Application::IsEditor() ? 1 : 0;
	}

	// Logical-core count on the host machine. Returns 4 as a floor when
	// the OS can't report a value (matches the JobSystem auto path's
	// fallback so script-driven thread budgets stay consistent with
	// engine-internal sizing).
	static int Index_Application_GetProcessorCount() {
		unsigned int hw = std::thread::hardware_concurrency();
		return hw > 0 ? static_cast<int>(hw) : 4;
	}

	static float Index_Application_GetFixedDeltaTime() {
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetFixedDeltaTime() : (1.0f / 50.0f);
	}
	static float Index_Application_GetUnscaledDeltaTime() {
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetDeltaTimeUnscaled() : 0.0f;
	}
	static float Index_Application_GetFixedUnscaledDeltaTime() {
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetUnscaledFixedDeltaTime() : (1.0f / 50.0f);
	}
	static float Index_Application_GetTimeScale() {
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetTimeScale() : 1.0f;
	}
	static void Index_Application_SetTimeScale(float scale) {
		auto* app = Application::GetInstance();
		if (app) app->GetTime().SetTimeScale(scale);
	}

	// Two-call buffer pattern: C# calls once with outBuffer=null/capacity=0 to learn the
	// required size, then again with a sized buffer. Stash into the thread-local so the
	// reported size matches the bytes copied on the second call.
	static int Index_Application_GetClipboardStringBuffer(char* outBuffer, int capacity)
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

	static void Index_Application_SetClipboardString(const char* value)
	{
		auto* window = Application::GetWindow();
		if (!window) return;
		window->SetClipboardString(value ? std::string(value) : std::string{});
	}

	static int Index_Application_GetVsyncEnabled()
	{
		return Window::IsVsync() ? 1 : 0;
	}

	static void Index_Application_SetVsyncEnabled(int enabled)
	{
		Window::SetVsync(enabled != 0);
	}

	static int Index_Application_GetRunInBackground()
	{
		return Application::GetRunInBackground() ? 1 : 0;
	}

	static void Index_Application_SetRunInBackground(int enabled)
	{
		Application::SetRunInBackground(enabled != 0);
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

	// ── Window Bindings ─────────────────────────────────────────────────
	// All Window_* thunks share the same null-window safety: if the
	// engine hasn't created a window yet (early init / headless test
	// host), getters return zero / empty and setters silently no-op.

	static int Index_Window_GetWidth()
	{
		auto* window = Application::GetWindow();
		return window ? window->GetWidth() : 0;
	}

	static int Index_Window_GetHeight()
	{
		auto* window = Application::GetWindow();
		return window ? window->GetHeight() : 0;
	}

	// Two-call buffer pattern — see Index_Application_GetClipboardStringBuffer
	// for the protocol. The thread-local s_StringReturnBuffer stages the
	// value between the size-probe call and the copy call so the reported
	// byte count matches what the second call writes.
	static int Index_Window_GetTitleBuffer(char* outBuffer, int capacity)
	{
		auto* window = Application::GetWindow();
		const std::string title = window ? window->GetTitle() : std::string{};
		return CopyToManagedBuffer(title, outBuffer, capacity);
	}

	static void Index_Window_SetTitle(const char* title)
	{
		auto* window = Application::GetWindow();
		if (!window) return;
		window->SetTitle(title ? std::string(title) : std::string{});
	}

	static void Index_Window_Minimize()
	{
		auto* window = Application::GetWindow();
		if (window) window->MinimizeWindow();
	}

	static void Index_Window_Maximize()
	{
		auto* window = Application::GetWindow();
		if (window) window->MaximizeWindow();
	}

	static void Index_Window_Restore()
	{
		auto* window = Application::GetWindow();
		if (window) window->RestoreWindow();
	}

	static int Index_Window_IsMaximized()
	{
		auto* window = Application::GetWindow();
		return (window && window->IsMaximized()) ? 1 : 0;
	}

	static int Index_Window_IsFullScreen()
	{
		auto* window = Application::GetWindow();
		return (window && window->IsFullScreen()) ? 1 : 0;
	}

	static void Index_Window_SetFullScreen(int enabled)
	{
		auto* window = Application::GetWindow();
		if (window) window->SetFullScreen(enabled != 0);
	}

	static void Index_Window_GetPosition(int* outX, int* outY)
	{
		auto* window = Application::GetWindow();
		const Vec2Int pos = window ? window->GetPosition() : Vec2Int{ 0, 0 };
		if (outX) *outX = pos.x;
		if (outY) *outY = pos.y;
	}

	static void Index_Window_SetPosition(int x, int y)
	{
		auto* window = Application::GetWindow();
		if (window) window->SetPosition(Vec2Int{ x, y });
	}

	static void Index_Window_Focus()
	{
		auto* window = Application::GetWindow();
		if (window) window->Focus();
	}

	// Primary monitor's video-mode dimensions — Window.ScreenCenter
	// derives its value from this (mirroring Window::GetScreenCenter on
	// the engine side, which reads the same k_Videomode field).
	static void Index_Window_GetScreenSize(int* outWidth, int* outHeight)
	{
		auto* window = Application::GetWindow();
		int w = 0, h = 0;
		if (window) {
			if (const GLFWvidmode* mode = window->GetVideomode()) {
				w = mode->width;
				h = mode->height;
			}
		}
		if (outWidth)  *outWidth  = w;
		if (outHeight) *outHeight = h;
	}

	static int Index_Cursor_GetMode()
	{
		auto* window = Application::GetWindow();
		return window ? window->GetCursorMode() : 0;
	}

	static void Index_Cursor_SetMode(int mode)
	{
		auto* window = Application::GetWindow();
		if (window) window->SetCursorMode(mode);
	}

	static uint64_t Index_Cursor_GetTexture()
	{
		auto* window = Application::GetWindow();
		return window ? window->GetCursorTextureAsset() : 0;
	}

	static void Index_Cursor_SetTexture(uint64_t assetId)
	{
		auto* window = Application::GetWindow();
		if (!window) return;

		if (assetId == 0) {
			window->SetCursorImage(nullptr);
			window->SetCursorTextureAsset(0);
			return;
		}

		TextureHandle handle = TextureManager::LoadTextureByUUID(assetId);
		Texture2D* texture = TextureManager::GetTexture(handle);
		if (!texture || !texture->IsValid()) {
			IDX_WARN_TAG("Script", "Cursor texture asset {} could not be loaded", assetId);
			return;
		}

		window->SetCursorImage(texture);
		window->SetCursorTextureAsset(assetId);
	}

	static int Index_Engine_GetVersionBuffer(char* outBuffer, int capacity)
	{
		return CopyToManagedBuffer(IDX_VERSION, outBuffer, capacity);
	}

	static int Index_Engine_GetVersionLongBuffer(char* outBuffer, int capacity)
	{
		return CopyToManagedBuffer(IDX_VERSION_LONG, outBuffer, capacity);
	}

	// 0 = Debug (editor preview), 1 = Development, 2 = Release. Editor mode
	// always reports Debug — that mirrors the INDEX_EDITOR define being the
	// "I am iterating" signal from the C# side. Outside the editor we honor
	// the project's ActiveBuildProfile which drives INDEX_BUILD_DEVELOPMENT
	// vs INDEX_BUILD_RELEASE for shipped game binaries.
	static int Index_Engine_GetBuildConfiguration()
	{
		if (Application::IsEditor()) return 0;
		auto* project = ProjectManager::GetCurrentProject();
		if (project && project->ActiveBuildProfile == IndexProject::BuildProfile::Release) return 2;
		return 1;
	}

	static int Index_Engine_GetPlatformBuffer(char* outBuffer, int capacity)
	{
#if defined(IDX_PLATFORM_WINDOWS)
		return CopyToManagedBuffer("Windows", outBuffer, capacity);
#elif defined(IDX_PLATFORM_LINUX)
		return CopyToManagedBuffer("Linux", outBuffer, capacity);
#else
		return CopyToManagedBuffer("Unknown", outBuffer, capacity);
#endif
	}

	static int Index_Engine_GetGraphicsApiBuffer(char* outBuffer, int capacity)
	{
		// Returns the version string set by the active backend.
		// Empty until the renderer initializes.
		return CopyToManagedBuffer(RenderApi::GetVersionString(), outBuffer, capacity);
	}

	static int Index_Engine_GetGpuVendorBuffer(char* outBuffer, int capacity)
	{
		return CopyToManagedBuffer(RenderApi::GetVendorString(), outBuffer, capacity);
	}

	static int Index_Engine_GetGpuRendererBuffer(char* outBuffer, int capacity)
	{
		return CopyToManagedBuffer(RenderApi::GetRendererString(), outBuffer, capacity);
	}

	// ── Time Bindings ───────────────────────────────────────────────────

	static int Index_Time_GetFrameCount()
	{
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetFrameCount() : 0;
	}

	static float Index_Time_GetTimeSinceStartup()
	{
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetTimeSinceStartup() : 0.0f;
	}

	static float Index_Time_GetRealtimeSinceStartup()
	{
		auto* app = Application::GetInstance();
		return app ? app->GetTime().GetRealtimeSinceStartup() : 0.0f;
	}

	// ── Log Bindings ────────────────────────────────────────────────────

	static void Index_Log_Trace(const char* message) { Log::PrintMessageTag(Log::Type::Client, Log::Level::Trace, "Script", message); }
	static void Index_Log_Info(const char* message)  { Log::PrintMessageTag(Log::Type::Client, Log::Level::Info, "Script", message); }
	static void Index_Log_Warn(const char* message)  { Log::PrintMessageTag(Log::Type::Client, Log::Level::Warn, "Script", message); }
	static void Index_Log_Error(const char* message) { Log::PrintMessageTag(Log::Type::Client, Log::Level::Error, "Script", message); }

	// ── Input Bindings ──────────────────────────────────────────────────

	static bool CanReadScriptInput()
	{
		auto* app = Application::GetInstance();
		return app && ScriptBindings::IsScriptInputEnabled();
	}

	static int Index_Input_GetKey(int keyCode)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetKey(static_cast<KeyCode>(keyCode)) ? 1 : 0) : 0;
	}

	static int Index_Input_GetKeyDown(int keyCode)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetKeyDown(static_cast<KeyCode>(keyCode)) ? 1 : 0) : 0;
	}

	static int Index_Input_GetKeyUp(int keyCode)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetKeyUp(static_cast<KeyCode>(keyCode)) ? 1 : 0) : 0;
	}

	static int Index_Input_GetAnyKey()
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetAnyKey() ? 1 : 0) : 0;
	}

	static int Index_Input_GetMouseButton(int button)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetMouse(static_cast<MouseButton>(button)) ? 1 : 0) : 0;
	}

	static int Index_Input_GetMouseButtonDown(int button)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetMouseDown(static_cast<MouseButton>(button)) ? 1 : 0) : 0;
	}

	static int Index_Input_GetMouseButtonUp(int button)
	{
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? (app->GetInput().GetMouseUp(static_cast<MouseButton>(button)) ? 1 : 0) : 0;
	}

	static void Index_Input_GetMousePosition(float* outX, float* outY)
	{
		auto* app = Application::GetInstance();
		if (CanReadScriptInput()) { Vec2 pos = app->GetInput().GetMousePosition(); *outX = pos.x; *outY = pos.y; }
		else { *outX = 0.0f; *outY = 0.0f; }
	}

	static void Index_Input_GetAxis(float* outX, float* outY) {
		auto* app = Application::GetInstance();
		if (CanReadScriptInput()) { Vec2 axis = app->GetInput().GetAxis(); *outX = axis.x; *outY = axis.y; }
		else { *outX = 0.0f; *outY = 0.0f; }
	}
	static void Index_Input_GetMouseDelta(float* outX, float* outY) {
		auto* app = Application::GetInstance();
		if (CanReadScriptInput()) { Vec2 delta = app->GetInput().GetMouseDelta(); *outX = delta.x; *outY = delta.y; }
		else { *outX = 0.0f; *outY = 0.0f; }
	}
	static float Index_Input_GetScrollWheelDelta() {
		auto* app = Application::GetInstance();
		return CanReadScriptInput() ? app->GetInput().ScrollValue() : 0.0f;
	}

	// ── Entity Bindings ─────────────────────────────────────────────────

	static int Index_Entity_IsValid(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		return ResolveEntityReference(entityID, scene, handle) ? 1 : 0;
	}

	static uint64_t Index_Entity_FindByName(const char* name)
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

	static void Index_Entity_Destroy(uint64_t entityID)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return;
		scene->DestroyEntity(handle);
	}

	static void Index_Entity_DestroyDelayed(uint64_t entityID, float delay)
	{
		Scene* scene = nullptr;
		EntityHandle handle = entt::null;
		if (!ResolveEntityReference(entityID, scene, handle)) return;
		scene->DestroyEntity(handle, delay);
	}

	static uint64_t Index_Entity_Create(const char* name)
	{
		Scene* scene = GetScene();
		if (!scene) return 0;
		Entity entity = scene->CreateRuntimeEntity(name ? name : "");
		return GetEntityScriptId(*scene, entity.GetHandle());
	}

	void PopulateNonComponentBindings(NativeBindings& b)
	{
		b.Application_GetDeltaTime = &Index_Application_GetDeltaTime;
		b.Application_GetElapsedTime = &Index_Application_GetTime;
		b.Application_GetScreenWidth = &Index_Application_GetScreenWidth;
		b.Application_GetScreenHeight = &Index_Application_GetScreenHeight;
		b.Application_IsEditor = &Index_Application_IsEditor;
		b.Application_GetProcessorCount = &Index_Application_GetProcessorCount;
		b.Application_GetTargetFrameRate = &Index_Application_GetTargetFrameRate;
		b.Application_SetTargetFrameRate = &Index_Application_SetTargetFrameRate;
		b.Application_Quit = &Index_Application_Quit;
		b.Application_GetFixedDeltaTime = &Index_Application_GetFixedDeltaTime;
		b.Application_GetUnscaledDeltaTime = &Index_Application_GetUnscaledDeltaTime;
		b.Application_GetFixedUnscaledDeltaTime = &Index_Application_GetFixedUnscaledDeltaTime;
		b.Application_GetTimeScale = &Index_Application_GetTimeScale;
		b.Application_SetTimeScale = &Index_Application_SetTimeScale;
		b.Application_GetClipboardStringBuffer = &Index_Application_GetClipboardStringBuffer;
		b.Application_SetClipboardString = &Index_Application_SetClipboardString;
		b.Application_GetVsyncEnabled = &Index_Application_GetVsyncEnabled;
		b.Application_SetVsyncEnabled = &Index_Application_SetVsyncEnabled;
		b.Application_GetRunInBackground = &Index_Application_GetRunInBackground;
		b.Application_SetRunInBackground = &Index_Application_SetRunInBackground;

		b.Window_GetWidth = &Index_Window_GetWidth;
		b.Window_GetHeight = &Index_Window_GetHeight;
		b.Window_GetTitleBuffer = &Index_Window_GetTitleBuffer;
		b.Window_SetTitle = &Index_Window_SetTitle;
		b.Window_Minimize = &Index_Window_Minimize;
		b.Window_Maximize = &Index_Window_Maximize;
		b.Window_Restore = &Index_Window_Restore;
		b.Window_IsMaximized = &Index_Window_IsMaximized;
		b.Window_IsFullScreen = &Index_Window_IsFullScreen;
		b.Window_SetFullScreen = &Index_Window_SetFullScreen;
		b.Window_GetPosition = &Index_Window_GetPosition;
		b.Window_SetPosition = &Index_Window_SetPosition;
		b.Window_Focus = &Index_Window_Focus;
		b.Window_GetScreenSize = &Index_Window_GetScreenSize;
		b.Cursor_GetMode = &Index_Cursor_GetMode;
		b.Cursor_SetMode = &Index_Cursor_SetMode;
		b.Cursor_GetTexture = &Index_Cursor_GetTexture;
		b.Cursor_SetTexture = &Index_Cursor_SetTexture;

		b.Engine_GetVersionBuffer = &Index_Engine_GetVersionBuffer;
		b.Engine_GetVersionLongBuffer = &Index_Engine_GetVersionLongBuffer;
		b.Engine_GetBuildConfiguration = &Index_Engine_GetBuildConfiguration;
		b.Engine_GetPlatformBuffer = &Index_Engine_GetPlatformBuffer;
		b.Engine_GetGraphicsApiBuffer = &Index_Engine_GetGraphicsApiBuffer;
		b.Engine_GetGpuVendorBuffer = &Index_Engine_GetGpuVendorBuffer;
		b.Engine_GetGpuRendererBuffer = &Index_Engine_GetGpuRendererBuffer;

		b.Time_GetFrameCount = &Index_Time_GetFrameCount;
		b.Time_GetTimeSinceStartup = &Index_Time_GetTimeSinceStartup;
		b.Time_GetRealtimeSinceStartup = &Index_Time_GetRealtimeSinceStartup;

		b.Log_Trace = &Index_Log_Trace;
		b.Log_Info = &Index_Log_Info;
		b.Log_Warn = &Index_Log_Warn;
		b.Log_Error = &Index_Log_Error;

		b.Input_GetKey = &Index_Input_GetKey;
		b.Input_GetKeyDown = &Index_Input_GetKeyDown;
		b.Input_GetKeyUp = &Index_Input_GetKeyUp;
		b.Input_GetAnyKey = &Index_Input_GetAnyKey;
		b.Input_GetMouseButton = &Index_Input_GetMouseButton;
		b.Input_GetMouseButtonDown = &Index_Input_GetMouseButtonDown;
		b.Input_GetMouseButtonUp = &Index_Input_GetMouseButtonUp;
		b.Input_GetMousePosition = &Index_Input_GetMousePosition;
		b.Input_GetAxis = &Index_Input_GetAxis;
		b.Input_GetMouseDelta = &Index_Input_GetMouseDelta;
		b.Input_GetScrollWheelDelta = &Index_Input_GetScrollWheelDelta;

		b.Entity_IsValid = &Index_Entity_IsValid;
		b.Entity_FindByName = &Index_Entity_FindByName;
		b.Entity_Destroy = &Index_Entity_Destroy;
		b.Entity_DestroyDelayed = &Index_Entity_DestroyDelayed;
		b.Entity_Create = &Index_Entity_Create;
	}

} // namespace Index
