#include <pch.hpp>
#include "Gui/AssetBrowser.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Core/Log.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Project/AxiomProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Serialization/Directory.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/SceneSerializer.hpp"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef AIM_PLATFORM_WINDOWS
#include <windows.h>
#include <shellapi.h>
#endif

namespace Axiom {

#ifdef AIM_PLATFORM_WINDOWS
	namespace {
		// M29: tracker for in-flight ShellExecuteW worker threads. Each
		// worker is short-lived (one ShellExecuteW call) but if the user
		// closes the editor mid-launch, detaching would leave the thread
		// running past `main()`. AssetBrowser::Shutdown calls JoinAll on
		// teardown so every worker either finishes naturally or is waited
		// for before global destructors run.
		std::mutex s_ShellLaunchMutex;
		std::vector<std::thread> s_ShellLaunchThreads;

		// Sweep already-finished threads. Called on every track to keep
		// the vector bounded — the mutex is uncontended in normal use.
		void ReapFinishedShellThreads_Locked() {
			s_ShellLaunchThreads.erase(
				std::remove_if(s_ShellLaunchThreads.begin(), s_ShellLaunchThreads.end(),
					[](std::thread& t) {
						if (!t.joinable()) return true;
						return false;
					}),
				s_ShellLaunchThreads.end());
		}
	}

	void TrackShellLaunchThread(std::thread t) {
		std::scoped_lock lock(s_ShellLaunchMutex);
		ReapFinishedShellThreads_Locked();
		s_ShellLaunchThreads.push_back(std::move(t));
	}

	void JoinAllShellLaunchThreads() {
		std::vector<std::thread> drained;
		{
			std::scoped_lock lock(s_ShellLaunchMutex);
			drained.swap(s_ShellLaunchThreads);
		}
		for (auto& t : drained) {
			if (t.joinable()) t.join();
		}
	}
#endif // AIM_PLATFORM_WINDOWS

	namespace {
		bool IsNativeScriptSourceExtension(std::string extension) {
			std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			return extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx"
				|| extension == ".h" || extension == ".hh" || extension == ".hpp" || extension == ".hxx"
				|| extension == ".inl" || extension == ".ipp";
		}

		std::filesystem::path GetNativeSourceDirectory(bool ensureProjectFiles = false) {
			if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
				if (ensureProjectFiles) {
					project->EnsureNativeScriptProjectFiles();
				}
				return project->NativeSourceDir;
			}

			return std::filesystem::path(Path::ExecutableDir()) / ".." / ".." / ".." / "Axiom-NativeScripts" / "Source";
		}

		std::filesystem::path ResolveNativeScriptMirrorPath(const std::string& path, bool ensureProjectFiles = false) {
			const std::filesystem::path sourcePath(path);
			if (!IsNativeScriptSourceExtension(sourcePath.extension().string())) {
				return {};
			}

			std::filesystem::path nativeSourceDir = GetNativeSourceDirectory(ensureProjectFiles);
			if (nativeSourceDir.empty()) {
				return {};
			}

			const std::filesystem::path mirrorPath = nativeSourceDir / sourcePath.filename();
			std::error_code ec;
			if (std::filesystem::equivalent(sourcePath, mirrorPath, ec)) {
				return {};
			}

			return mirrorPath;
		}

		AxiomProject::EditorEntityNameSuffixStyle GetAssetDuplicateSuffixStyle()
		{
			if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
				return project->EditorAssetDuplicateSuffix;
			}
			return AxiomProject::EditorEntityNameSuffixStyle::ParenthesizedNumber;
		}

		std::string FormatDuplicateAssetName(const std::string& baseName, int index)
		{
			switch (GetAssetDuplicateSuffixStyle()) {
			case AxiomProject::EditorEntityNameSuffixStyle::SpaceNumber:
				return baseName + " " + std::to_string(index);
			case AxiomProject::EditorEntityNameSuffixStyle::HyphenNumber:
				return baseName + "-" + std::to_string(index);
			case AxiomProject::EditorEntityNameSuffixStyle::UnderscoreNumber:
				return baseName + "_" + std::to_string(index);
			case AxiomProject::EditorEntityNameSuffixStyle::ParenthesizedNumber:
			default:
				return baseName + " (" + std::to_string(index) + ")";
			}
		}

		// .scene files embed a "name" field in their JSON which Scene::SetName
		// reads at load time. If the file is renamed but the embedded "name"
		// keeps the old stem, reopening "Bar.scene" still shows "Foo" in the
		// hierarchy — the bug case from the editor session. Rewrite the field
		// in place so the on-disk file matches its filename. We preserve the
		// pretty-printing convention SceneSerializer::SaveToFile uses.
		void SyncSceneEmbeddedNameToFilename(const std::string& scenePath, const std::string& newStem) {
			const std::string content = File::ReadAllText(scenePath);
			if (content.empty()) {
				return;
			}

			Json::Value root;
			std::string parseError;
			if (!Json::TryParse(content, root, &parseError) || !root.IsObject()) {
				AIM_WARN_TAG("AssetBrowser",
					"Renamed scene '{}' has unreadable JSON ({}); the embedded name was not updated.",
					scenePath, parseError);
				return;
			}

			if (Json::Value* nameNode = root.FindMember("name")) {
				if (nameNode->AsStringOr() == newStem) {
					return;
				}
				*nameNode = Json::Value(newStem);
			}
			else {
				root.AddMember("name", Json::Value(newStem));
			}

			if (!File::WriteAllText(scenePath, Json::Stringify(root, true))) {
				AIM_WARN_TAG("AssetBrowser",
					"Failed to write renamed scene '{}' back to disk; embedded name remains stale.",
					scenePath);
			}
		}

		// If the renamed scene happens to be the one currently loaded, the
		// hierarchy panel reads its name from Scene::GetName() — bring that
		// in sync with the new filename so the user sees the change without
		// reloading. Also nudge the project's LastOpenedScene pointer so
		// the next launcher run loads the renamed file by its new stem.
		void UpdateLoadedSceneNameAfterRename(const std::string& oldStem, const std::string& newStem) {
			Scene* active = SceneManager::Get().GetActiveScene();
			if (!active || active->GetName() != oldStem) {
				return;
			}
			active->SetName(newStem);

			if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
				if (project->LastOpenedScene == oldStem) {
					project->LastOpenedScene = newStem;
					project->Save();
				}
			}
		}

		void RegisterImportedAssetPath(const std::filesystem::path& path) {
			std::error_code ec;
			if (std::filesystem::is_regular_file(path, ec) && !ec) {
				if (!AssetRegistry::IsMetaFilePath(path.string())) {
					AssetRegistry::GetOrCreateAssetUUID(path.string());
				}
				return;
			}

			ec.clear();
			if (!std::filesystem::is_directory(path, ec) || ec) {
				return;
			}

			for (std::filesystem::recursive_directory_iterator it(
				 path,
				 std::filesystem::directory_options::skip_permission_denied,
				 ec), end;
				 it != end;
				 it.increment(ec)) {
				if (ec) {
					ec.clear();
					continue;
				}

				std::error_code fileEc;
				if (!it->is_regular_file(fileEc) || fileEc) {
					continue;
				}

				const std::string assetPath = it->path().string();
				if (!AssetRegistry::IsMetaFilePath(assetPath)) {
					AssetRegistry::GetOrCreateAssetUUID(assetPath);
				}
			}
		}
	}

	void AssetBrowser::BeginRename(const std::string& path, const std::string& currentName) {
		m_IsRenaming = true;
		m_RenamePath = path;
		m_PressedPath.clear();
		m_RenameFrameCounter = 0;

		// Pre-fill with the stem only when ShowFileExtensions is off:
		// authoring "MyScene" instead of "MyScene.scene" matches the way
		// the asset browser displays the entry, and the user doesn't
		// have to delete a redundant ".scene" before typing. CommitRename
		// re-appends the original extension on commit. Folders never
		// have an extension to strip — fall back to currentName for them.
		AxiomProject* project = ProjectManager::GetCurrentProject();
		const bool showExt = project ? project->ShowFileExtensions : false;
		std::string display = currentName;
		if (!showExt) {
			std::filesystem::path p(currentName);
			std::string stem = p.stem().string();
			std::string ext = p.extension().string();
			// stem.empty() catches dotfiles like ".env" — leave them
			// alone so we don't render an empty rename buffer.
			if (!ext.empty() && !stem.empty()) {
				display = stem;
			}
		}
		std::snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", display.c_str());
	}

	void AssetBrowser::CommitRename() {
		std::string newName(m_RenameBuffer);
		std::string oldName = std::filesystem::path(m_RenamePath).filename().string();

		if (m_PendingScriptType == PendingScriptType::EntityPrefab) {
			// Strip a trailing ".prefab" the user typed so we don't end up with "Foo.prefab.prefab".
			std::string baseName = newName;
			if (baseName.size() > 7) {
				std::string lower = baseName;
				std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
				if (lower.substr(lower.size() - 7) == ".prefab") {
					baseName = baseName.substr(0, baseName.size() - 7);
				}
			}
			if (baseName.empty()) {
				baseName = "NewPrefab";
			}

			// Suffix " (N)" to dodge collisions with anything already in the folder.
			std::filesystem::path targetPath = std::filesystem::path(m_PendingScriptDir) / (baseName + ".prefab");
			std::error_code existsEc;
			for (int n = 1; targetPath.string() != m_RenamePath
				&& std::filesystem::exists(targetPath, existsEc) && n < 10000; ++n) {
				targetPath = std::filesystem::path(m_PendingScriptDir) / (FormatDuplicateAssetName(baseName, n) + ".prefab");
				existsEc.clear();
			}

			const std::string finalPath = targetPath.string();
			bool renameSucceeded = true;
			if (finalPath != m_RenamePath) {
				std::error_code renameEc;
				std::filesystem::rename(m_RenamePath, finalPath, renameEc);
				if (renameEc) {
					renameSucceeded = false;
					AIM_WARN_TAG("AssetBrowser",
						"Failed to rename '{}' to '{}': {}",
						m_RenamePath, finalPath, renameEc.message());
				}
			}

			if (renameSucceeded) {
				m_SelectedPath = finalPath;
			}
			m_PendingScriptType = PendingScriptType::None;
			m_PendingScriptDir.clear();
			m_PendingPrefabSourceEntity = entt::null;
			m_NeedsRefresh = true;
			CancelRename();
			return;
		}

		if (m_PendingScriptType != PendingScriptType::None) {
			std::string className = newName;
			const bool isCSharp = m_PendingScriptType == PendingScriptType::CSharp
				|| m_PendingScriptType == PendingScriptType::CSharpComponent
				|| m_PendingScriptType == PendingScriptType::CSharpNativeComponent
				|| m_PendingScriptType == PendingScriptType::CSharpGameSystem
				|| m_PendingScriptType == PendingScriptType::CSharpGlobalSystem;

			const bool isComponent = m_PendingScriptType == PendingScriptType::CSharpComponent;
			const bool isNativeComponent = m_PendingScriptType == PendingScriptType::CSharpNativeComponent;
			const bool isGameSystem = m_PendingScriptType == PendingScriptType::CSharpGameSystem;
			const bool isGlobalSystem = m_PendingScriptType == PendingScriptType::CSharpGlobalSystem;
			std::string ext = isCSharp ? ".cs" : (isComponent ? ".hpp" : ".cpp");

			if (!className.empty() && className.find('.') == std::string::npos) {
			}
			else if (className.size() > ext.size()) {
				std::string lowerName = className;
				std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
				if (lowerName.substr(lowerName.size() - ext.size()) == ext)
					className = className.substr(0, className.size() - ext.size());
			}

			if (className.empty()) {
				className = isComponent ? "NewComponent" : (isGameSystem ? "NewGameSystem" : (isGlobalSystem ? "NewGlobalSystem" : "NewScript"));
			}

			std::string finalFileName = className + ext;
			std::string finalPath = (std::filesystem::path(m_PendingScriptDir) / finalFileName).string();

			if (std::filesystem::exists(m_RenamePath)) {
				std::error_code ec;
				std::filesystem::remove(m_RenamePath, ec);
			}

			if (isCSharp) {
				std::string boilerplate;
				if (isComponent) {
					boilerplate =
						"using Axiom;\n"
						"\n"
						"public class " + className + " : Component\n"
						"{\n"
						"    public float Value = 0.0f;\n"
						"}\n";
				}
				else if (isNativeComponent) {
					boilerplate =
						"using Axiom;\n"
						"using Axiom.Components;\n"
						"\n"
						"public struct " + className + " : IComponent\n"
						"{\n"
						"    public float Value = 0.0f;\n"
						"\n"
						"    public " + className + "()\n"
						"{\n"
						"}\n"
						"\n"
						"}\n";
				}
				else if (isGameSystem) {
					boilerplate =
						"using Axiom;\n"
						"\n"
						"public class " + className + " : GameSystem\n"
						"{\n"
						"    public override void OnStart()\n"
						"    {\n"
						"    }\n"
						"\n"
						"    public override void OnUpdate()\n"
						"    {\n"
						"    }\n"
						"\n"
						"    public override void OnDestroy()\n"
						"    {\n"
						"    }\n"
						"}\n";
				}
				else if (isGlobalSystem) {
					boilerplate =
						"using Axiom;\n"
						"\n"
						"public class " + className + " : GlobalSystem\n"
						"{\n"
						"    public static " + className + " Instance { get; private set; } = null!;\n"
						"\n"
						"    public override void OnInitialize()\n"
						"    {\n"
						"        Instance = this;\n"
						"    }\n"
						"\n"
						"    public override void OnUpdate()\n"
						"    {\n"
						"    }\n"
						"}\n";
				}
				else {
					boilerplate =
						"using Axiom;\n"
						"\n"
						"public class " + className + " : EntityScript\n"
						"{\n"
						"    public override void OnStart()\n"
						"    {\n"
						"    }\n"
						"\n"
						"    public override void OnUpdate()\n"
						"    {\n"
						"    }\n"
						"}\n";
				}

				std::ofstream file(finalPath);
				if (file.is_open()) { file << boilerplate; file.close(); }

				AxiomProject* project = ProjectManager::GetCurrentProject();
				if (!project) {
					auto sandboxDir = std::filesystem::path(Path::ExecutableDir()) / ".." / ".." / ".." / "Axiom-Sandbox" / "Source";
					if (std::filesystem::exists(sandboxDir)) {
						auto dst = sandboxDir / finalFileName;
						if (!std::filesystem::exists(dst)) {
							std::ofstream f(dst); if (f.is_open()) { f << boilerplate; f.close(); }
						}
					}
				}
			}
			else {
				std::string boilerplate = isComponent
					? "#pragma once\n"
					  "\n"
					  "struct " + className + "\n"
					  "{\n"
					  "    float Value = 0.0f;\n"
					  "};\n"
					: "#include <Scripting/NativeScript.hpp>\n"
					  "\n"
					  "class " + className + " : public Axiom::NativeScript {\n"
					  "public:\n"
					  "    void Start() override\n"
					  "    {\n"
					  "    }\n"
					  "\n"
					  "    void Update(float dt) override\n"
					  "    {\n"
					  "    }\n"
					  "\n"
					  "    void OnDestroy() override\n"
					  "    {\n"
					  "    }\n"
					  "};\n"
					  "REGISTER_SCRIPT(" + className + ")\n";

				std::ofstream file(finalPath);
				if (file.is_open()) { file << boilerplate; file.close(); }

				std::filesystem::path nativeDir = GetNativeSourceDirectory(true);
				if (!nativeDir.empty()) {
					std::error_code ec;
					std::filesystem::create_directories(nativeDir, ec);
					auto dst = nativeDir / finalFileName;
					if (!std::filesystem::exists(dst)) {
						std::ofstream f(dst); if (f.is_open()) { f << boilerplate; f.close(); }
					}
				}
			}

			m_SelectedPath = finalPath;
			m_PendingScriptType = PendingScriptType::None;
			m_PendingScriptDir.clear();
			m_NeedsRefresh = true;
			CancelRename();
			return;
		}

		// Re-append the original extension when the user renamed without
		// typing one — necessary because BeginRename stripped it for the
		// "ShowFileExtensions=false" UX. Skip when the entry IS a folder
		// (no extension to preserve) or when the user typed an explicit
		// extension already (don't double-stack ".scene.scene"). Done in
		// the regular-rename branch only; the script/prefab pending
		// branch above already builds its own filename from scratch.
		if (!newName.empty() && newName.find('.') == std::string::npos) {
			std::string oldExt = std::filesystem::path(m_RenamePath).extension().string();
			if (!oldExt.empty()) {
				newName += oldExt;
			}
		}

		if (!newName.empty() && newName != oldName) {
			RenameEntry(m_RenamePath, newName);
		}
		CancelRename();
	}

	void AssetBrowser::CancelRename() {
		if (m_PendingScriptType != PendingScriptType::None) {
			if (!m_RenamePath.empty() && std::filesystem::exists(m_RenamePath)) {
				std::error_code ec;
				std::filesystem::remove(m_RenamePath, ec);
			}
			m_PendingScriptType = PendingScriptType::None;
			m_PendingScriptDir.clear();
			m_PendingPrefabSourceEntity = entt::null;
			m_NeedsRefresh = true;
		}

		m_IsRenaming = false;
		m_RenamePath.clear();
		m_RenameFrameCounter = 0;
	}

	bool AssetBrowser::IsRenamingEntry(const std::string& path) const {
		return m_IsRenaming && m_RenamePath == path;
	}

	bool AssetBrowser::BeginRenameSelected() {
		if (m_SelectedPath.empty() || m_IsRenaming) {
			return false;
		}

		std::error_code ec;
		if (!std::filesystem::exists(m_SelectedPath, ec) || ec) {
			return false;
		}

		BeginRename(m_SelectedPath, std::filesystem::path(m_SelectedPath).filename().string());
		return true;
	}

	void AssetBrowser::DeleteEntry(const std::string& path) {
		m_Thumbnails.Invalidate(path);
		const std::filesystem::path nativeMirrorPath = ResolveNativeScriptMirrorPath(path);

		if (Directory::Delete(path)) {
			if (!nativeMirrorPath.empty()) {
				std::error_code ec;
				std::filesystem::remove(nativeMirrorPath, ec);
			}

			if (m_SelectedPath == path) {
				m_SelectedPath.clear();
			}
			m_SelectedPaths.erase(
				std::remove(m_SelectedPaths.begin(), m_SelectedPaths.end(), path),
				m_SelectedPaths.end());
			if (m_PressedPath == path) {
				m_PressedPath.clear();
			}
			CancelRename();
			m_NeedsRefresh = true;
		}
	}

	void AssetBrowser::RenameEntry(const std::string& path, const std::string& newName) {
		m_Thumbnails.Invalidate(path);

		std::string oldExt = std::filesystem::path(path).extension().string();
		std::string oldStem = std::filesystem::path(path).stem().string();

		if (Directory::Rename(path, newName)) {
			std::filesystem::path p(path);
			std::string newPath = (p.parent_path() / newName).string();
			if (m_SelectedPath == path) {
				m_SelectedPath = newPath;
			}
			for (std::string& selectedPath : m_SelectedPaths) {
				if (selectedPath == path) {
					selectedPath = newPath;
				}
			}

			std::string ext = std::filesystem::path(newName).extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			std::string newStem = std::filesystem::path(newName).stem().string();
			const bool oldIsNativeScript = IsNativeScriptSourceExtension(oldExt);
			const bool newIsNativeScript = IsNativeScriptSourceExtension(ext);

			std::filesystem::path projectSourceDir;
			AxiomProject* project = ProjectManager::GetCurrentProject();
			if (oldIsNativeScript || newIsNativeScript)
			{
				projectSourceDir = GetNativeSourceDirectory(newIsNativeScript);
			}
			else if (ext == ".cs")
			{
				if (project)
					projectSourceDir = project->ScriptsDirectory;
				else
					projectSourceDir = std::filesystem::path(Path::ExecutableDir()) / ".." / ".." / ".." / "Axiom-Sandbox" / "Source";
			}

			if (!projectSourceDir.empty() && (std::filesystem::exists(projectSourceDir) || newIsNativeScript))
			{
				if (newIsNativeScript) {
					std::error_code ec;
					std::filesystem::create_directories(projectSourceDir, ec);
				}

				auto oldProjectFile = projectSourceDir / (oldStem + oldExt);
				auto newProjectFile = projectSourceDir / newName;

				if (oldIsNativeScript && !newIsNativeScript)
				{
					std::error_code ec;
					std::filesystem::remove(oldProjectFile, ec);
				}
				else if (newIsNativeScript)
				{
					if (std::filesystem::exists(oldProjectFile))
					{
						std::error_code ec;
						std::filesystem::rename(oldProjectFile, newProjectFile, ec);
					}
					else if (std::filesystem::exists(newPath) && !std::filesystem::exists(newProjectFile))
					{
						std::error_code ec;
						std::filesystem::copy_file(newPath, newProjectFile, std::filesystem::copy_options::none, ec);
					}

					if (std::filesystem::exists(newProjectFile))
					{
						std::ifstream in(newProjectFile);
						std::string content((std::istreambuf_iterator<char>(in)),
							std::istreambuf_iterator<char>());
						in.close();

						size_t pos = 0;
						while ((pos = content.find(oldStem, pos)) != std::string::npos)
						{
							content.replace(pos, oldStem.size(), newStem);
							pos += newStem.size();
						}

						std::ofstream out(newProjectFile);
						out << content;
					}
				}
			}

			// Scene files carry their displayed name inside the JSON. Without
			// this sync, renaming Foo.scene -> Bar.scene leaves the embedded
			// name as "Foo", and the hierarchy still shows "Foo" the next time
			// the file is opened.
			if (ext == ".scene")
			{
				SyncSceneEmbeddedNameToFilename(newPath, newStem);
				UpdateLoadedSceneNameAfterRename(oldStem, newStem);
			}

			m_NeedsRefresh = true;
		}
	}

	void AssetBrowser::CopyPathToClipboard(const std::string& path) {
		ImGui::SetClipboardText(path.c_str());
	}

	void AssetBrowser::CreateFolder(const std::string& parentDir) {
		std::string baseName = "New Folder";
		std::string folderPath = (std::filesystem::path(parentDir) / baseName).string();
		int counter = 1;
		while (Directory::Exists(folderPath)) {
			folderPath = (std::filesystem::path(parentDir) / (baseName + " " + std::to_string(counter))).string();
			counter++;
		}

		Directory::Create(folderPath, false);
		m_NeedsRefresh = true;

		Refresh();

		m_SelectedPath = folderPath;
		std::string name = std::filesystem::path(folderPath).filename().string();
		BeginRename(folderPath, name);
	}

	void AssetBrowser::CreateScript(const std::string& parentDir) {
		std::string baseName = "NewEntityScript";
		std::string ext = ".cs";
		std::string scriptPath = (std::filesystem::path(parentDir) / (baseName + ext)).string();
		int counter = 1;
		while (std::filesystem::exists(scriptPath)) {
			scriptPath = (std::filesystem::path(parentDir) / (baseName + std::to_string(counter) + ext)).string();
			counter++;
		}

		m_PendingScriptType = PendingScriptType::CSharp;
		m_PendingScriptDir = parentDir;

		m_NeedsRefresh = true;
		Refresh();

		m_SelectedPath = scriptPath;
		std::string name = std::filesystem::path(scriptPath).stem().string();
		BeginRename(scriptPath, name);
	}
	
	void AssetBrowser::CreateManagedCSharpComponent(const std::string& parentDir) {
		std::string baseName = "NewComponent";
		std::string ext = ".cs";
		std::string componentPath = (std::filesystem::path(parentDir) / (baseName + ext)).string();
		int counter = 1;
		while (std::filesystem::exists(componentPath)) {
			componentPath = (std::filesystem::path(parentDir) / (baseName + std::to_string(counter) + ext)).string();
			counter++;
		}

		m_PendingScriptType = PendingScriptType::CSharpComponent;
		m_PendingScriptDir = parentDir;

		m_NeedsRefresh = true;
		Refresh();

		m_SelectedPath = componentPath;
		std::string name = std::filesystem::path(componentPath).stem().string();
		BeginRename(componentPath, name);
	}

	void AssetBrowser::CreateNativeCSharpComponent(const std::string& parentDir) {
		std::string baseName = "NewNativeComponent";
		std::string ext = ".cs";
		std::string componentPath = (std::filesystem::path(parentDir) / (baseName + ext)).string();
		int counter = 1;
		while (std::filesystem::exists(componentPath)) {
			componentPath = (std::filesystem::path(parentDir) / (baseName + std::to_string(counter) + ext)).string();
			counter++;
		}

		m_PendingScriptType = PendingScriptType::CSharpNativeComponent;
		m_PendingScriptDir = parentDir;

		m_NeedsRefresh = true;
		Refresh();

		m_SelectedPath = componentPath;
		std::string name = std::filesystem::path(componentPath).stem().string();
		BeginRename(componentPath, name);
	}


	void AssetBrowser::CreateGameSystem(const std::string& parentDir) {
		std::string baseName = "NewGameSystem";
		std::string ext = ".cs";
		std::string systemPath = (std::filesystem::path(parentDir) / (baseName + ext)).string();
		int counter = 1;
		while (std::filesystem::exists(systemPath)) {
			systemPath = (std::filesystem::path(parentDir) / (baseName + std::to_string(counter) + ext)).string();
			counter++;
		}

		m_PendingScriptType = PendingScriptType::CSharpGameSystem;
		m_PendingScriptDir = parentDir;

		m_NeedsRefresh = true;
		Refresh();

		m_SelectedPath = systemPath;
		std::string name = std::filesystem::path(systemPath).stem().string();
		BeginRename(systemPath, name);
	}

	void AssetBrowser::CreateGlobalSystem(const std::string& parentDir) {
		std::string baseName = "NewGlobalSystem";
		std::string ext = ".cs";
		std::string systemPath = (std::filesystem::path(parentDir) / (baseName + ext)).string();
		int counter = 1;
		while (std::filesystem::exists(systemPath)) {
			systemPath = (std::filesystem::path(parentDir) / (baseName + std::to_string(counter) + ext)).string();
			counter++;
		}

		m_PendingScriptType = PendingScriptType::CSharpGlobalSystem;
		m_PendingScriptDir = parentDir;

		m_NeedsRefresh = true;
		Refresh();

		m_SelectedPath = systemPath;
		std::string name = std::filesystem::path(systemPath).stem().string();
		BeginRename(systemPath, name);
	}

	
	void AssetBrowser::CreateDefaultTexture(const std::string& parentDir,
		const std::string& sourceFile, const std::string& displayName)
	{
		const std::string sourceDir = Path::ResolveAxiomAssets("Textures/Default");
		if (sourceDir.empty()) {
			AIM_WARN_TAG("AssetBrowser",
				"Default textures directory not found (AxiomAssets/Textures/Default). "
				"Cannot create '{}'.", displayName);
			return;
		}

		const std::filesystem::path source =
			std::filesystem::path(sourceDir) / sourceFile;
		if (!std::filesystem::exists(source)) {
			AIM_WARN_TAG("AssetBrowser",
				"Default texture '{}' not found at {}.",
				sourceFile, source.string());
			return;
		}

		const std::string ext = source.extension().string();
		std::filesystem::path destPath =
			std::filesystem::path(parentDir) / (displayName + ext);
		std::error_code existsEc;
		for (int n = 1; std::filesystem::exists(destPath, existsEc) && n < 10000; ++n) {
			destPath = std::filesystem::path(parentDir) /
				(FormatDuplicateAssetName(displayName, n) + ext);
			existsEc.clear();
		}

		try {
			std::filesystem::copy_file(source, destPath,
				std::filesystem::copy_options::overwrite_existing);
		}
		catch (const std::exception& e) {
			AIM_ERROR_TAG("AssetBrowser",
				"Failed to copy default texture '{}' to '{}': {}",
				source.string(), destPath.string(), e.what());
			return;
		}

		const std::string finalPath = destPath.string();
		m_NeedsRefresh = true;
		Refresh();

		m_SelectedPath = finalPath;
		BeginRename(finalPath, std::filesystem::path(finalPath).stem().string());
	}

	void AssetBrowser::CreateScene(const std::string& parentDir) {
		std::string baseName = "NewScene";
		std::string ext = ".scene";

		auto sceneNameTaken = [&](const std::string& name) -> bool {
			try {
				for (auto& entry : std::filesystem::recursive_directory_iterator(
					m_RootDirectory, std::filesystem::directory_options::skip_permission_denied)) {
					if (entry.is_regular_file() && entry.path().extension() == ext
						&& entry.path().stem().string() == name)
						return true;
				}
			}
			catch (...) {}
			return false;
		};

		std::string sceneName = baseName;
		std::string scenePath = (std::filesystem::path(parentDir) / (sceneName + ext)).string();
		int counter = 1;
		while (sceneNameTaken(sceneName)) {
			sceneName = baseName + std::to_string(counter++);
			scenePath = (std::filesystem::path(parentDir) / (sceneName + ext)).string();
		}

		std::string content =
			"{\n"
			"  \"name\": \"" + sceneName + "\",\n"
			"  \"systems\": [],\n"
			"  \"entities\": [\n"
			"    {\n"
			"      \"name\": \"Camera\",\n"
			"      \"Transform2D\": { \"posX\": 0, \"posY\": 0, \"rotation\": 0, \"scaleX\": 1, \"scaleY\": 1 },\n"
			"      \"Camera2D\": { \"orthoSize\": 5, \"zoom\": 1 }\n"
			"    }\n"
			"  ]\n"
			"}\n";

		std::ofstream file(scenePath);
		if (file.is_open()) {
			file << content;
			file.close();
		}

		m_NeedsRefresh = true;
		Refresh();

		m_SelectedPath = scenePath;
		std::string name = std::filesystem::path(scenePath).filename().string();
		BeginRename(scenePath, name);
	}

	void AssetBrowser::CreateFile(const std::string& parentDir, const std::string& baseName,
		const std::string& extension, const std::string& defaultContent) {
		// Normalize: extension may or may not include the leading dot.
		const std::string ext = (extension.empty() || extension[0] == '.') ? extension : ("." + extension);

		std::filesystem::path filePath = std::filesystem::path(parentDir) / (baseName + ext);
		std::error_code existsEc;
		for (int n = 1; std::filesystem::exists(filePath, existsEc) && n < 10000; ++n) {
			filePath = std::filesystem::path(parentDir) / (FormatDuplicateAssetName(baseName, n) + ext);
			existsEc.clear();
		}

		const std::string finalPath = filePath.string();
		std::ofstream file(finalPath, std::ios::binary);
		if (file.is_open()) {
			if (!defaultContent.empty()) {
				file.write(defaultContent.data(), static_cast<std::streamsize>(defaultContent.size()));
			}
			file.close();
		}

		m_NeedsRefresh = true;
		Refresh();

		m_SelectedPath = finalPath;
		BeginRename(finalPath, std::filesystem::path(finalPath).filename().string());
	}

	void AssetBrowser::CreateEntityPrefab(const std::string& parentDir, EntityHandle sourceEntity) {
		const std::string baseName = "NewPrefab";
		std::filesystem::path prefabPath = std::filesystem::path(parentDir) / (baseName + ".prefab");
		std::error_code existsEc;
		for (int n = 1; std::filesystem::exists(prefabPath, existsEc) && n < 10000; ++n) {
			prefabPath = std::filesystem::path(parentDir) / (FormatDuplicateAssetName(baseName, n) + ".prefab");
			existsEc.clear();
		}

		const std::string finalPath = prefabPath.string();

		if (sourceEntity != entt::null) {
			Scene* activeScene = SceneManager::Get().GetActiveScene();
			if (activeScene && activeScene->IsValid(sourceEntity)) {
				SceneSerializer::SaveEntityToFile(*activeScene, sourceEntity, finalPath);
			}
		}
		else {
			// Minimal Transform2D-only prefab — matches what Scene::CreateEntity produces, so
			// instantiating from this file gives a usable entity instead of a component-less one.
			const std::string content =
				"{\n"
				"  \"version\": 1,\n"
				"  \"type\": \"Prefab\",\n"
				"  \"Entity\": {\n"
				"    \"Transform2D\": { \"posX\": 0, \"posY\": 0, \"rotation\": 0, \"scaleX\": 1, \"scaleY\": 1 }\n"
				"  },\n"
				"  \"prefab\": {\n"
				"    \"Transform2D\": { \"posX\": 0, \"posY\": 0, \"rotation\": 0, \"scaleX\": 1, \"scaleY\": 1 }\n"
				"  }\n"
				"}\n";
			std::ofstream file(finalPath);
			if (file.is_open()) {
				file << content;
				file.close();
			}
		}

		m_PendingScriptType = PendingScriptType::EntityPrefab;
		m_PendingScriptDir = parentDir;
		m_PendingPrefabSourceEntity = sourceEntity;
		m_NeedsRefresh = true;
		Refresh();

		m_SelectedPath = finalPath;
		BeginRename(finalPath, std::filesystem::path(finalPath).filename().string());
	}

	void AssetBrowser::OpenAssetExternal(const DirectoryEntry& entry) {
		try
		{
			std::string ext = std::filesystem::path(entry.Path).extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

			if (ext == ".scene")
			{
				m_PendingSceneLoad = entry.Path;
				return;
			}

			if (ext == ".prefab")
			{
				// Defer: ImGuiEditorLayer drains this in OnPreRender and
				// swaps into prefab-edit mode (detached scene + blue
				// background). Routing through here keeps double-click
				// behaviour for prefabs in the same place as scenes.
				m_PendingPrefabEdit = entry.Path;
				return;
			}

			if (ExternalEditor::IsScriptExtension(ext))
			{
				std::filesystem::path scriptPath = entry.Path;
				const std::filesystem::path nativeMirrorPath = ResolveNativeScriptMirrorPath(entry.Path);
				if (!nativeMirrorPath.empty() && std::filesystem::exists(nativeMirrorPath)) {
					scriptPath = nativeMirrorPath;
				}
				ExternalEditor::OpenFile(scriptPath.string());
				return;
			}

#ifdef AIM_PLATFORM_WINDOWS
			// M29: shell-launch threads are tracked instead of detached so
			// AssetBrowser::Shutdown can join them on editor close.
			// Process::Run / LaunchDetached use CreateProcessW directly and
			// don't resolve OS file-association handlers, so for arbitrary
			// asset types (.png, .pdf, ...) we still need ShellExecuteW —
			// the lifecycle just gets a tracker.
			//
			// The cap on concurrent in-flight opens stays in place so a
			// stuck shell handler can't be amplified by a user
			// double-clicking many assets in rapid succession.
			static constexpr int k_MaxConcurrentOpens = 8;
			static std::atomic<int> s_ActiveOpens{ 0 };
			if (s_ActiveOpens.load(std::memory_order_acquire) >= k_MaxConcurrentOpens) {
				AIM_WARN_TAG("AssetBrowser",
					"Skipping external open for '{}' — too many concurrent shell launches.",
					entry.Path);
				return;
			}
			s_ActiveOpens.fetch_add(1, std::memory_order_acq_rel);

			std::wstring wpath = std::filesystem::absolute(entry.Path).wstring();
			std::thread worker([wpath]() {
				CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				CoUninitialize();
				s_ActiveOpens.fetch_sub(1, std::memory_order_acq_rel);
			});
			TrackShellLaunchThread(std::move(worker));
#endif
		}
		catch (...) {}
	}

	void AssetBrowser::OnExternalFileDrop(const std::vector<std::string>& paths) {
		if (m_CurrentDirectory.empty()) return;

		int imported = 0;
		for (const auto& sourcePath : paths) {
			try {
				std::filesystem::path src(sourcePath);
				if (!std::filesystem::exists(src)) continue;

				std::filesystem::path destDir(m_CurrentDirectory);
				std::filesystem::path importedPath;

				if (std::filesystem::is_directory(src)) {
					std::filesystem::path dest = destDir / src.filename();
					std::filesystem::copy(src, dest,
						std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing);
					importedPath = dest;
					imported++;
				}
				else {
					std::filesystem::path dest = destDir / src.filename();

					if (std::filesystem::exists(dest)) {
						std::string stem = dest.stem().string();
						std::string ext = dest.extension().string();
						int counter = 1;
						while (std::filesystem::exists(dest)) {
							dest = destDir / (stem + " (" + std::to_string(counter) + ")" + ext);
							counter++;
						}
					}

					std::filesystem::copy_file(src, dest);
					importedPath = dest;
					imported++;
				}

				if (!importedPath.empty()) {
					RegisterImportedAssetPath(importedPath);
					m_Thumbnails.Invalidate(importedPath.string());
				}
			}
			catch (const std::exception& e) {
				AIM_CORE_WARN_TAG("AssetBrowser", "Failed to import '{}': {}", sourcePath, e.what());
			}
		}

		if (imported > 0) {
			AssetRegistry::MarkDirty();
			m_NeedsRefresh = true;
			AIM_CORE_INFO_TAG("AssetBrowser", "Imported {} file(s) into {}", imported, m_CurrentDirectory);
		}
	}

}
