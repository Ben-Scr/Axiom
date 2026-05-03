#pragma once

#include "Core/Export.hpp"
#include "Core/WindowSpecification.hpp"

#ifndef AXIOM_DEFAULT_ENABLE_GUI_RENDERER
#define AXIOM_DEFAULT_ENABLE_GUI_RENDERER 1
#endif

#ifndef AXIOM_DEFAULT_ENABLE_GIZMO_RENDERER
#define AXIOM_DEFAULT_ENABLE_GIZMO_RENDERER 1
#endif

#ifndef AXIOM_DEFAULT_ENABLE_PHYSICS_2D
#define AXIOM_DEFAULT_ENABLE_PHYSICS_2D 1
#endif

#ifndef AXIOM_DEFAULT_ENABLE_AUDIO
#define AXIOM_DEFAULT_ENABLE_AUDIO 1
#endif

#ifndef AXIOM_DEFAULT_ENABLE_SCRIPTING
#define AXIOM_DEFAULT_ENABLE_SCRIPTING 1
#endif

#ifndef AXIOM_DEFAULT_ENABLE_RENDERER_2D
#define AXIOM_DEFAULT_ENABLE_RENDERER_2D 1
#endif

#ifndef AXIOM_DEFAULT_ENABLE_TEXTURE_MANAGER
#define AXIOM_DEFAULT_ENABLE_TEXTURE_MANAGER 1
#endif

#ifndef AXIOM_DEFAULT_ENABLE_PACKAGE_HOST
#define AXIOM_DEFAULT_ENABLE_PACKAGE_HOST 1
#endif

#ifndef AXIOM_DEFAULT_SET_WINDOW_ICON
#define AXIOM_DEFAULT_SET_WINDOW_ICON 1
#endif

#ifndef AXIOM_DEFAULT_ENABLE_VSYNC
#define AXIOM_DEFAULT_ENABLE_VSYNC 1
#endif

namespace Axiom {

	struct ApplicationConfig {
		WindowSpecification WindowSpecification{ 800, 800, "Axiom Runtime", true, true, false };
		// These are runtime feature requests; build-time module availability is described by the
		// dependency sets in Dependencies.lua and the feature flags in Core/Export.hpp.
		bool EnableGuiRenderer = AXIOM_DEFAULT_ENABLE_GUI_RENDERER != 0;
		bool EnableGizmoRenderer = AXIOM_DEFAULT_ENABLE_GIZMO_RENDERER != 0;
		bool EnablePhysics2D = AXIOM_DEFAULT_ENABLE_PHYSICS_2D != 0;
		bool EnableAudio = AXIOM_DEFAULT_ENABLE_AUDIO != 0;
		// Off skips the entire CoreCLR + ScriptCore + user-assembly setup.
		// The ScriptSystem is still added to scenes (cheap, no side effects),
		// but its Awake bails before loading anything. Use this for processes
		// that don't run game logic — the launcher is the obvious one: it
		// would otherwise hold a file lock on Axiom-ScriptCore.dll for its
		// whole lifetime, blocking any rebuild the editor tries to do.
		bool EnableScripting = AXIOM_DEFAULT_ENABLE_SCRIPTING != 0;
		// Off skips constructing Renderer2D entirely. m_Renderer2D stays null
		// and BeginFrame / EndFrame already null-check it. Use this for
		// processes that don't render 2D content — the launcher draws only
		// ImGui via ImGuiContextLayer and never needs the batch renderer.
		bool EnableRenderer2D = AXIOM_DEFAULT_ENABLE_RENDERER_2D != 0;
		// Off skips TextureManager::Initialize / Shutdown. Safe for the
		// launcher, which never loads sprites or thumbnails. The window-icon
		// path falls back to SetWindowIconFromResource (Win32 resource icon)
		// and only uses TextureManager when a project sets AppIconPath.
		bool EnableTextureManager = AXIOM_DEFAULT_ENABLE_TEXTURE_MANAGER != 0;
		// Off skips PackageHost::LoadAll / UnloadAll. Native packages
		// (Pkg.<Name>.Native.dll next to the executable) are not discovered
		// or loaded. Use this for processes that don't run game packages —
		// the launcher has no project loaded so package OnLoad code would
		// just be dead-weight side effects.
		bool EnablePackageHost = AXIOM_DEFAULT_ENABLE_PACKAGE_HOST != 0;
		bool SetWindowIcon = AXIOM_DEFAULT_SET_WINDOW_ICON != 0;
		bool Vsync = AXIOM_DEFAULT_ENABLE_VSYNC != 0;
		bool UseTargetFrameRateForMainLoop = true;

		// Headless / minimal: window only, no rendering, no audio, no physics, no
		// scripting, no packages. For console tools, asset processors, headless
		// servers. EnableTextureManager stays on so the window icon path still works.
		AXIOM_API static ApplicationConfig Minimal();

		// Game runtime defaults: full rendering, physics, audio, scripting, packages.
		// Equivalent to today's default-constructed ApplicationConfig.
		AXIOM_API static ApplicationConfig Game();

		// Editor-host defaults: same as Game() today; reserved as a future-proofing
		// hook so editor-only feature flags can be flipped here without touching
		// every caller.
		AXIOM_API static ApplicationConfig Editor();
	};

} // namespace Axiom
