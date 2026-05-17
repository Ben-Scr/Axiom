#pragma once

#include "Core/Export.hpp"
#include "Core/WindowSpecification.hpp"

#include <cstddef>

#ifndef INDEX_DEFAULT_ENABLE_GUI_RENDERER
#define INDEX_DEFAULT_ENABLE_GUI_RENDERER 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_GIZMO_RENDERER
#define INDEX_DEFAULT_ENABLE_GIZMO_RENDERER 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_PHYSICS_2D
#define INDEX_DEFAULT_ENABLE_PHYSICS_2D 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_AUDIO
#define INDEX_DEFAULT_ENABLE_AUDIO 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_SCRIPTING
#define INDEX_DEFAULT_ENABLE_SCRIPTING 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_RENDERER_2D
#define INDEX_DEFAULT_ENABLE_RENDERER_2D 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_TEXTURE_MANAGER
#define INDEX_DEFAULT_ENABLE_TEXTURE_MANAGER 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_SHADER_MANAGER
#define INDEX_DEFAULT_ENABLE_SHADER_MANAGER 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_PACKAGE_HOST
#define INDEX_DEFAULT_ENABLE_PACKAGE_HOST 1
#endif

#ifndef INDEX_DEFAULT_SET_WINDOW_ICON
#define INDEX_DEFAULT_SET_WINDOW_ICON 1
#endif

#ifndef INDEX_DEFAULT_ENABLE_VSYNC
#define INDEX_DEFAULT_ENABLE_VSYNC 1
#endif

#ifndef INDEX_DEFAULT_FRAME_ARENA_CAPACITY_BYTES
#define INDEX_DEFAULT_FRAME_ARENA_CAPACITY_BYTES (1024 * 1024) // 1 MiB
#endif

#ifndef INDEX_DEFAULT_PERSISTENT_ARENA_CAPACITY_BYTES
#define INDEX_DEFAULT_PERSISTENT_ARENA_CAPACITY_BYTES (64 * 1024) // 64 KiB
#endif

namespace Index {

	struct ApplicationConfig {
		WindowSpecification WindowSpecification{ 800, 800, "Index Runtime", true, true, false };
		// These are runtime feature requests; build-time module availability is described by the
		// dependency sets in Dependencies.lua and the feature flags in Core/Export.hpp.
		bool EnableGuiRenderer = INDEX_DEFAULT_ENABLE_GUI_RENDERER != 0;
		bool EnableGizmoRenderer = INDEX_DEFAULT_ENABLE_GIZMO_RENDERER != 0;
		bool EnablePhysics2D = INDEX_DEFAULT_ENABLE_PHYSICS_2D != 0;
		bool EnableAudio = INDEX_DEFAULT_ENABLE_AUDIO != 0;
		// Off skips the entire CoreCLR + ScriptCore + user-assembly setup.
		// The ScriptSystem is still added to scenes (cheap, no side effects),
		// but its Awake bails before loading anything. Use this for processes
		// that don't run game logic — the launcher is the obvious one: it
		// would otherwise hold a file lock on Index-ScriptCore.dll for its
		// whole lifetime, blocking any rebuild the editor tries to do.
		bool EnableScripting = INDEX_DEFAULT_ENABLE_SCRIPTING != 0;
		// Off skips constructing Renderer2D entirely. m_Renderer2D stays null
		// and BeginFrame / EndFrame already null-check it. Use this for
		// processes that don't render 2D content — the launcher draws only
		// ImGui via ImGuiContextLayer and never needs the batch renderer.
		bool EnableRenderer2D = INDEX_DEFAULT_ENABLE_RENDERER_2D != 0;
		// Off skips TextureManager::Initialize / Shutdown. Safe for the
		// launcher, which never loads sprites or thumbnails. The window-icon
		// path falls back to SetWindowIconFromResource (Win32 resource icon)
		// and only uses TextureManager when a project sets AppIconPath.
		bool EnableTextureManager = INDEX_DEFAULT_ENABLE_TEXTURE_MANAGER != 0;
		// Off skips ShaderManager::Initialize / Shutdown. Safe for any
		// process that doesn't load .wgsl shaders through the cache; the
		// renderers ship embedded WGSL via the WebGPU backend directly.
		bool EnableShaderManager = INDEX_DEFAULT_ENABLE_SHADER_MANAGER != 0;
		// Off skips PackageHost::LoadAll / UnloadAll. Native packages
		// (Pkg.<Name>.Native.dll next to the executable) are not discovered
		// or loaded. Use this for processes that don't run game packages —
		// the launcher has no project loaded so package OnLoad code would
		// just be dead-weight side effects.
		bool EnablePackageHost = INDEX_DEFAULT_ENABLE_PACKAGE_HOST != 0;
		bool SetWindowIcon = INDEX_DEFAULT_SET_WINDOW_ICON != 0;
		bool Vsync = INDEX_DEFAULT_ENABLE_VSYNC != 0;
		bool UseTargetFrameRateForMainLoop = true;

		// FrameArenas backing-buffer sizes. Frame() is reset every
		// EndFrame; Persistent() is never auto-reset. Set to 0 to opt
		// out — Allocate on a zero-capacity arena returns nullptr, so
		// callers that gracefully handle exhaustion still work.
		std::size_t FrameArenaCapacityBytes      = INDEX_DEFAULT_FRAME_ARENA_CAPACITY_BYTES;
		std::size_t PersistentArenaCapacityBytes = INDEX_DEFAULT_PERSISTENT_ARENA_CAPACITY_BYTES;

		// Headless / minimal: window only, no rendering, no audio, no physics, no
		// scripting, no packages. For console tools, asset processors, headless
		// servers. EnableTextureManager stays on so the window icon path still works.
		INDEX_API static ApplicationConfig Minimal();

		// Game runtime defaults: full rendering, physics, audio, scripting, packages.
		// Equivalent to today's default-constructed ApplicationConfig.
		INDEX_API static ApplicationConfig Game();

		// Editor-host defaults: same as Game() today; reserved as a future-proofing
		// hook so editor-only feature flags can be flipped here without touching
		// every caller.
		INDEX_API static ApplicationConfig Editor();
	};

} // namespace Index
