#include <Axiom.hpp>

#include "Core/Application.hpp"
#include "Scene/SceneDefinition.hpp"
#include "Scene/SceneManager.hpp"
#include <Gui/ImGuiContextLayer.hpp>
#include <Systems/LauncherLayer.hpp>
#include <Core/Version.hpp>

using namespace Axiom;

class LauncherApplication : public Application {
public:
	ApplicationConfig GetConfiguration() const override {
		ApplicationConfig config;
		config.WindowSpecification = WindowSpecification(900, 600, "Axiom Launcher " + std::string(AIM_VERSION), true, true, false);
		config.EnableAudio = false;
		config.EnableGuiRenderer = false;
		config.EnableGizmoRenderer = false;
		config.EnablePhysics2D = false;
		// Launcher is purely a project picker — it never runs user game code,
		// so spinning up CoreCLR and loading Axiom-ScriptCore.dll just to do
		// nothing with them is wasted work. Worse, holding the DLL would
		// block the editor (separate process) from rebuilding ScriptCore
		// while we're alive — exactly the lock-contention bug seen on
		// project-close.
		config.EnableScripting = false;
		// Launcher draws only ImGui (via ImGuiContextLayer); no sprites, no
		// scenes, no native packages. Skipping these avoids unnecessary GL
		// state setup and, for PackageHost, prevents loading and executing
		// every Pkg.*.Native.dll's OnLoad on a UI that never uses them.
		config.EnableRenderer2D = false;
		config.EnableTextureManager = false;
		config.EnablePackageHost = false;
		config.SetWindowIcon = true;
		config.Vsync = false;

		SetTargetFramerate(144.f);

		return config;
	}

	void ConfigureScenes() override {
		SceneDefinition& launcherScene = GetSceneManager()->RegisterScene("Launcher");
		launcherScene.SetAsStartupScene();
	}

	void ConfigureLayers() override {
		// ImGuiContextLayer must be pushed first — its OnPreRender / OnPostRender wrap
		// ImGui::NewFrame and ImGui::Render around the launcher's UI work.
		PushLayer<ImGuiContextLayer>();
		PushLayer<LauncherLayer>();
	}

	void Start() override {}
	void Update() override {}
	void FixedUpdate() override {}
	void OnPaused() override {}
	void OnQuit() override {}
};

Axiom::Application* Axiom::CreateApplication() {
	return new LauncherApplication();
}

#include <EntryPoint.hpp>
