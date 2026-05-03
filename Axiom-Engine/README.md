# Axiom-Engine

The Axiom engine static/shared library. This README documents the public C++ entry path for users who want to ship a game without the editor. For build setup, dependency installation, and submodule init see the top-level [README.md](../README.md).

## Quick start (for game developers)

A minimal Axiom game is one translation unit: a subclass of `Axiom::Application` plus a `CreateApplication()` factory. The engine owns the entry point — you never write `main()`.

```cpp
// MyGame.cpp — the single TU that owns the process entry point.

#include <Axiom/App.hpp>          // Application + standalone-core surface.
#include <EntryPoint.hpp>         // Defines main()/WinMain(). See warning below.

class MyGame : public Axiom::Application {
public:
    Axiom::ApplicationConfig GetConfiguration() const override {
        // Pick a runtime profile; see "Configuring what the engine starts up" below.
        Axiom::ApplicationConfig config = Axiom::ApplicationConfig::Game();
        config.WindowSpecification = Axiom::WindowSpecification(
            1280, 720, "My Game", /*resizable*/ true, /*decorated*/ true, /*fullscreen*/ false);
        return config;
    }

    void ConfigureScenes() override {
        // Register scenes here; the SceneManager instantiates them after Start().
        // GetSceneManager()->RegisterScene("MainMenu").SetAsStartupScene();
    }

    void Start() override      {} // Called once after subsystems are up.
    void Update() override     {} // Per-frame, variable timestep.
    void FixedUpdate() override{} // Fixed-timestep tick (physics step interval).
    void OnPaused() override   {} // Window lost focus / minimized / explicit pause.
    void OnQuit() override     {} // Final teardown hook before subsystems shut down.
};

Axiom::Application* Axiom::CreateApplication() {
    return new MyGame();
}
```

> **Warning (audit E29):** `EntryPoint.hpp` defines `main()` (or `WinMain()` in Dist Windows builds) at file scope. Including it in two translation units triggers a duplicate-symbol linker error — and the header guards against this with a sentinel that produces a `#error` if the header is re-included after the symbol has been emitted. **Include it in exactly one `.cpp`.** Every other TU should use `<Axiom/App.hpp>` (or `<Axiom/Core.hpp>`) only.

The canonical end-to-end example lives in [`Axiom-Runtime/src/RuntimeApplication.cpp`](../Axiom-Runtime/src/RuntimeApplication.cpp): it loads `axiom-project.json` next to the executable, registers project scenes, and falls back to a built-in sample if no project is found.

## Picking an umbrella header

Three umbrella headers ship today. Pick one based on what your TU actually uses:

| Include | Use when |
|---|---|
| `<Axiom/App.hpp>` | Building an app that derives from `Application` (game runtime, editor host, custom tool that drives the engine loop). Pulls Core + `Application.hpp`. Requires `AXIOM_WITH_APPLICATION=1` (set automatically by the `full` module profile or any custom profile that enables every module). |
| `<Axiom/Core.hpp>` | Linking the engine for utility code only — no `Application` loop. Gives you `Collections/` (Vec2/Vec4/Color/Mat2/Viewport), `Core/Time`, `Core/Log`, `Core/Assert`, `Core/Version`, `Core/Base`, `Core/Exceptions`, `Core/Export`, and `ApplicationConfig` (the type — not the runtime). Safe in any module profile, including `core`. |
| `<Axiom.hpp>` | Legacy compatibility header. By itself it is identical to `<Axiom/Core.hpp>`. **Define `AXIOM_ALL_MODULES` before including it** to restore the legacy wide-surface include set (Components, Graphics, Physics, Audio, Scene, Events, Math, Utils). New code should prefer `<Axiom/App.hpp>` for app TUs and per-subsystem includes elsewhere. |

`EntryPoint.hpp` is a fourth header but **not an umbrella**: it exists solely to define `main()`/`WinMain()` and call your `CreateApplication()` factory. Include it in one TU only.

## Configuring what the engine starts up

`Axiom::ApplicationConfig` is a plain struct with one bool per subsystem. Your `Application` subclass returns a populated config from `GetConfiguration()`; the engine reads it once during `Application::Initialize()`.

Three factories cover the common cases:

```cpp
ApplicationConfig::Minimal();  // Window only. No 2D renderer, no GUI/gizmo
                               // renderers, no audio, no physics, no scripting,
                               // no package host. Texture manager stays on so
                               // the window-icon path still works. Use for
                               // headless tools, asset processors, servers.

ApplicationConfig::Game();     // Today's default: full rendering, audio,
                               // physics, scripting, package host. Equivalent
                               // to a default-constructed ApplicationConfig.

ApplicationConfig::Editor();   // Editor-host defaults. Identical to Game()
                               // today; reserved as a future hook.
```

Per-subsystem flags on `ApplicationConfig` (override individually after a factory call):

| Field | Effect when `false` |
|---|---|
| `EnableGuiRenderer` | Skips `GuiRenderer` (ImGui plumbing the engine owns; editor brings its own context). |
| `EnableGizmoRenderer` | Skips `GizmoRenderer2D` construction; gizmo draw calls become no-ops. |
| `EnablePhysics2D` | Skips `PhysicsSystem2D`; Box2D world is never created. |
| `EnableAudio` | Skips miniaudio device init; `AudioSource` components are inert. |
| `EnableScripting` | Skips CoreCLR + ScriptCore + user-assembly load. `ScriptSystem` still attaches but bails in `Awake`. |
| `EnableRenderer2D` | Skips `Renderer2D` construction; `BeginFrame`/`EndFrame` null-check it. |
| `EnableTextureManager` | Skips `TextureManager::Initialize`/`Shutdown`. Window-icon falls back to Win32 resource. |
| `EnablePackageHost` | Skips `PackageHost::LoadAll`. Native packages next to the exe are not discovered. |
| `SetWindowIcon` | Suppresses the icon load entirely. |
| `Vsync` | Disables swap-interval vsync; framerate is governed by `SetTargetFramerate`. |
| `UseTargetFrameRateForMainLoop` | When false, the loop runs uncapped and ignores `m_TargetFramerate`. |

The `WindowSpecification WindowSpecification` field controls the GLFW window — width, height, title, resizable, decorated, fullscreen. See [`Core/WindowSpecification.hpp`](src/Core/WindowSpecification.hpp).

These are runtime feature requests. Enabling a flag does nothing if the corresponding module was stripped at build time (next section).

## Build-time module profiles

The Premake build supports module-level slicing. Pass these to `premake5`:

```bat
vendor\bin\premake5.exe vs2022 --module-profile=full
vendor\bin\premake5.exe vs2022 --module-profile=core
vendor\bin\premake5.exe vs2022 --module-profile=custom --with-render --with-audio
```

Profiles (defined in [`premake5.lua`](../premake5.lua)):

| Profile | Enables | Defines |
|---|---|---|
| `full` *(default for compatibility)* | Render, Audio, Physics, Scripting, Editor | All `AXIOM_WITH_*=1` plus `AXIOM_ALL_MODULES=1` plus `AXIOM_WITH_APPLICATION=1`. Includes `Axiom-Launcher` and `Axiom-Runtime` projects in the solution. |
| `core` | (none) | All `AXIOM_WITH_*=0`, `AXIOM_CORE_ONLY=1`. No third-party module dependencies pulled in. `<Axiom/App.hpp>` will fail to compile in this profile (Application requires the full surface). |
| `custom` | Whatever `--with-*` flags select | Per-module `AXIOM_WITH_*` defines. `AXIOM_WITH_APPLICATION=1` only when render+audio+physics+scripting+editor are all on. |

Per-module flags (only meaningful with `--module-profile=custom`, or implicitly enable `custom` when used without `--module-profile`):

- `--with-render` — GLFW, Glad, OpenGL, `Graphics/`, `Gui/GuiRenderer`, particle/gizmo systems.
- `--with-audio` — miniaudio, `Audio/`, `Components/Audio/`, `AudioUpdateSystem`.
- `--with-physics` — Box2D, Axiom-Physics, `Physics/`, `Components/Physics/`.
- `--with-scripting` — .NET host, `Scripting/`, `Axiom-ScriptCore` and `Axiom-Sandbox` projects.
- `--with-editor` — ImGui static lib. Implicitly turns on render, audio, physics, and scripting (the editor depends on all of them).

Other build-shaping flags:

- `--no-profiler` — Strip Tracy and the `Profiling/` folder entirely. `AXIOM_PROFILER_ENABLED` is undefined; the in-engine ProfilerPanel is excluded.
- `--with-imgui-demo` — Compile `imgui_demo.cpp` into the ImGui static lib (editor only).
- `--axiom-project=PATH` — Add `<PATH>/Packages/<Name>/axiom-package.lua` manifests to the package loader so project-local packages get built into the solution.

The exact wiring lives in [`premake5.lua`](../premake5.lua) (see `ResolveAxiomModules`, `GetAxiomModuleDefines`) and per-module dependency sets in [`Dependencies.lua`](../Dependencies.lua).

## Distribution checklist

A shipped game must include the following next to its executable. The runtime's [`premake5.lua`](../Axiom-Runtime/premake5.lua) post-build steps do this for `Axiom-Runtime.exe` — your own game project should do the same.

Always required:

- `<YourGame>.exe` — your game binary.
- `Axiom-Engine.dll` — the engine shared lib. Built as a `SharedLib` from `Axiom-Engine/premake5.lua`. Consumers define `AIM_IMPORT_DLL`; the engine builds with `AIM_BUILD_DLL`.
- `GLFW.dll` — windowing/input. Shipped as a `SharedLib` so engine+exe share its global state.
- `Glad.dll` — OpenGL loader. Same shared-state reason as GLFW.
- `AxiomAssets/` directory — engine default assets (`Audio/`, `Fonts/`, `Shader/`, `Textures/`). Source of truth is [`Axiom-Runtime/AxiomAssets/`](../Axiom-Runtime/AxiomAssets); the post-build copies it next to the exe.

Conditional:

- `nethost.dll` — required when scripting is enabled. The runtime's post-build copies it from `External/dotnet/lib/nethost.dll`.
- `Tracy.dll` — required when the profiler is enabled (i.e. when `--no-profiler` was *not* passed). Stripped builds skip this.
- `axiom-project.json` — your project manifest. The runtime auto-detects it via `AxiomProject::Validate(exeDir)` and loads it through `ProjectManager`. **If absent, the runtime logs a warning and falls back to a built-in `SampleScene`** — that warning is the only signal a packaging mistake gives you (audit E30). Always ship the project file.
- Your project's scene files, package DLLs (`Pkg.<Name>.Native.dll`), C# script assemblies, and any user assets referenced by `axiom-project.json`.

## See also

- [Top-level README](../README.md) — prerequisites, clean checkout, Premake/MSBuild commands, Linux setup.
- `~/.claude/skills/axiom-task-workflow` — engine-internal contributor workflow (when to regen Premake, what counts as "done", multi-site registrations).
- [`Axiom-Runtime/src/RuntimeApplication.cpp`](../Axiom-Runtime/src/RuntimeApplication.cpp) — canonical `Application` example with project loading.
- [`Core/Application.hpp`](src/Core/Application.hpp) — full virtual surface and lifecycle.
- [`Core/ApplicationConfig.hpp`](src/Core/ApplicationConfig.hpp) — every per-subsystem flag and its meaning.
- [`Core/Export.hpp`](src/Core/Export.hpp) — `AXIOM_API` / `AXIOM_WITH_*` / `AXIOM_CORE_ONLY` macro contract.
- Audit references: **H13** (umbrella consolidation — three headers documented here), **E29** (`EntryPoint.hpp` single-TU guard), **E30** (`axiom-project.json` fallback warning), **E6** (`ApplicationConfig` factories).
