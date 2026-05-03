#include "pch.hpp"
#include "Core/ApplicationConfig.hpp"

namespace Axiom {

	ApplicationConfig ApplicationConfig::Minimal() {
		ApplicationConfig config;
		config.EnableGuiRenderer = false;
		config.EnableGizmoRenderer = false;
		config.EnablePhysics2D = false;
		config.EnableAudio = false;
		config.EnableScripting = false;
		config.EnableRenderer2D = false;
		// EnableTextureManager stays on so the window-icon path can still load
		// a project icon (a one-shot upload, not a per-frame cost).
		config.EnableTextureManager = true;
		config.EnablePackageHost = false;
		// Window-icon and vsync are not subsystem toggles — leave them alone.
		return config;
	}

	ApplicationConfig ApplicationConfig::Game() {
		// Today's default-constructed config — full game runtime: rendering,
		// physics, audio, scripting, packages. Lives here so callers can spell
		// the intent (`ApplicationConfig::Game()`) instead of relying on the
		// implicit default.
		return ApplicationConfig{};
	}

	ApplicationConfig ApplicationConfig::Editor() {
		// Editor host has no extra flags today, but the factory exists so that
		// flipping editor-only defaults later is a one-line change.
		return Game();
	}

} // namespace Axiom
