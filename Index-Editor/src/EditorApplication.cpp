#include <Index.hpp>

#include "Core/Application.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Scene/EntityHelper.hpp"
#include "Scene/SceneDefinition.hpp"
#include "Scene/SceneManager.hpp"
#include <Gui/ImGuiContextLayer.hpp>
#include <Systems/ImGuiEditorLayer.hpp>
#include <Systems/GizmosDebugSystem.hpp>
#include <Project/ProjectManager.hpp>
#include <Project/IndexProject.hpp>
#include <Serialization/SceneSerializer.hpp>
#include <Serialization/File.hpp>
#include <Core/Version.hpp>
#include "Editor/EditorComponentRegistration.hpp"

using namespace Index;

static std::string s_ProjectPath;

class EditorApplication : public Application {
public:
	EditorApplication() {
		// Mark this Application instance as the editor host so engine
		// code (and C# scripts via Application.IsEditor) can branch on
		// runtime context. The flag is the runtime sibling of the
		// compile-time INDEX_EDITOR define already passed to scripts
		// loaded by the editor.
		SetEditorHost(true);
	}

	ApplicationConfig GetConfiguration() const override {
		ApplicationConfig config;
		std::string title = "Index Editor " + std::string(IDX_VERSION);
		if (ProjectManager::HasProject())
			title += " - " + ProjectManager::GetCurrentProject()->Name;
		config.WindowSpecification = WindowSpecification(0, 0, title, true, true, true);
		config.EnableAudio = true;
		config.EnableGizmoRenderer = true;
		// GuiRenderer is the screen-space UI pass for RectTransform2D
		// entities (Image, TextRendererComponent in UI mode, dropdown
		// popups). The editor needs it on so the loaded scene's UI
		// shows up in Editor View / Game View — historical "false" here
		// dates back to when this flag controlled engine-owned ImGui
		// plumbing, which it no longer does.
		config.EnableGuiRenderer = true;
		config.EnablePhysics2D = true;
		config.SetWindowIcon = true;
		SetRunInBackground(true);
		return config;
	}

	void ConfigureScenes() override {
		Application::SetIsPlaying(false);

		SceneDefinition& editorScene = GetSceneManager()->RegisterScene("SampleScene");
		editorScene.OnLoad([](Scene& scene) {
			IndexProject* project = ProjectManager::GetCurrentProject();
			if (project) {
				const std::string scenePath = project->GetSceneFilePath(project->LastOpenedScene);
				if (File::Exists(scenePath)) {
					SceneSerializer::LoadFromFile(scene, scenePath);
					return;
				}
			}

			EntityHelper::CreateCamera2DEntity(scene);
		});
		editorScene.SetAsStartupScene();
	}

	void ConfigureLayers() override {
		// ImGuiContextLayer must be pushed first — its OnPreRender / OnPostRender wrap
		// ImGui::NewFrame and ImGui::Render around the other layers' UI work.
		PushLayer<ImGuiContextLayer>();
		PushLayer<ImGuiEditorLayer>();
		PushOverlay<GizmosDebugSystem>();
	}

	void Start() override {
		RegisterEditorComponentInspectors(*GetSceneManager());
	}
	void Update() override {}
	void FixedUpdate() override {}
	void OnPaused() override {}

	void OnQuit() override {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			return;
		}

		auto* sceneManager = GetSceneManager();
		if (!sceneManager) {
			return;
		}

		Scene* active = sceneManager->GetActiveScene();
		if (active) {
			project->LastOpenedScene = active->GetName();
			project->Save();
		}
	}
};

Index::Application* Index::CreateApplication() {
	// Parse --project="path" from command line
	const Application::CommandLineArgs args = Application::GetCommandLineArgs();

	for (int i = 1; i < args.Count; i++) {
		const char* rawArg = args[i];
		if (!rawArg) {
			continue;
		}

		std::string arg(rawArg);
		if (arg.rfind("--project=", 0) == 0) {
			s_ProjectPath = arg.substr(10);
			// Remove surrounding quotes if present
			if (s_ProjectPath.size() >= 2 && s_ProjectPath.front() == '"' && s_ProjectPath.back() == '"')
				s_ProjectPath = s_ProjectPath.substr(1, s_ProjectPath.size() - 2);
		}
	}

	if (!s_ProjectPath.empty() && IndexProject::Validate(s_ProjectPath)) {
		auto project = std::make_unique<IndexProject>(IndexProject::Load(s_ProjectPath));
		IDX_CORE_INFO_TAG("Editor", "Opening project: {} ({})", project->Name, project->RootDirectory);
		ProjectManager::SetCurrentProject(std::move(project));
	}

	if (!ProjectManager::HasProject()) {
		IDX_CORE_WARN_TAG("Editor", "No project specified. Use: Index-Editor.exe --project=\"path/to/project\"");
		IDX_CORE_WARN_TAG("Editor", "Starting with default fallback paths.");
	}

	return new EditorApplication();
}

#include <EntryPoint.hpp>
