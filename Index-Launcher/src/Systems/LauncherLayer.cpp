#include "Systems/LauncherLayer.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/SceneManager.hpp"
#include "Core/Application.hpp"
#include <Core/Version.hpp>
#include <Core/Log.hpp>
#include "Localization/Localization.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/Directory.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Utils/Process.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Utils/Timer.hpp"
#include "Utils/StringHelper.hpp"
#include <imgui.h>
#include <filesystem>
#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <limits>
#include <system_error>

#ifdef IDX_PLATFORM_WINDOWS
#include <shellapi.h>
#include <shobjidl.h>
#endif

namespace Index {

	// ── Relative Time Formatting ────────────────────────────────────

	static std::string FormatRelativeTime(const std::string& iso8601) {
		if (iso8601.empty()) return "";

		std::tm tm{};
		std::istringstream ss(iso8601);
		ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
		if (ss.fail()) return "";

		std::time_t then = std::mktime(&tm);
		std::time_t now = std::time(nullptr);
		double seconds = std::difftime(now, then);

		if (seconds < 0) return "Now";
		if (seconds < 5) return "Now";
		if (seconds < 60) return std::to_string(static_cast<int>(seconds)) + "s ago";

		int minutes = static_cast<int>(seconds / 60);
		if (minutes < 60) return std::to_string(minutes) + "m ago";

		int hours = minutes / 60;
		if (hours < 24) return std::to_string(hours) + "h ago";

		int days = hours / 24;
		if (days < 30) return std::to_string(days) + "d ago";

		int months = days / 30;
		int remainingDays = days % 30;
		if (months < 12) {
			if (remainingDays > 0)
				return std::to_string(months) + "mo " + std::to_string(remainingDays) + "d ago";
			return std::to_string(months) + "mo ago";
		}

		int years = months / 12;
		int remainingMonths = months % 12;
		if (remainingMonths > 0)
			return std::to_string(years) + "y " + std::to_string(remainingMonths) + "mo ago";
		return std::to_string(years) + "y ago";
	}

#ifdef IDX_PLATFORM_WINDOWS
	static std::wstring Utf8ToWide(std::string_view utf8) {
		if (utf8.empty()) {
			return {};
		}

		const int requiredChars = MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS,
			utf8.data(), static_cast<int>(utf8.size()),
			nullptr, 0);
		if (requiredChars <= 0) {
			return {};
		}

		std::wstring wide(static_cast<size_t>(requiredChars), L'\0');
		if (MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS,
			utf8.data(), static_cast<int>(utf8.size()),
			wide.data(), requiredChars) != requiredChars) {
			return {};
		}

		return wide;
	}

	static std::string FormatWindowsError(DWORD errorCode) {
		return std::error_code(static_cast<int>(errorCode), std::system_category()).message();
	}
#endif

	static void AddSaturating(std::uintmax_t& total, std::uintmax_t value) {
		const std::uintmax_t maxValue = std::numeric_limits<std::uintmax_t>::max();
		total = value > maxValue - total ? maxValue : total + value;
	}

	static std::uintmax_t CalculateProjectDirectorySize(const std::filesystem::path& root, std::string& errorMessage) {
		std::error_code ec;
		if (!std::filesystem::exists(root, ec) || ec) {
			errorMessage = ec ? ec.message() : "Project folder not found.";
			return 0;
		}

		if (!std::filesystem::is_directory(root, ec) || ec) {
			errorMessage = ec ? ec.message() : "Project path is not a folder.";
			return 0;
		}

		std::uintmax_t total = 0;
		std::vector<std::filesystem::path> pendingDirectories;
		pendingDirectories.push_back(root);

		while (!pendingDirectories.empty()) {
			std::filesystem::path current = std::move(pendingDirectories.back());
			pendingDirectories.pop_back();

			ec.clear();
			std::filesystem::directory_iterator it(
				current,
				std::filesystem::directory_options::skip_permission_denied,
				ec);
			if (ec) {
				if (current == root) {
					errorMessage = ec.message();
					return 0;
				}
				continue;
			}

			const std::filesystem::directory_iterator end;
			while (it != end) {
				const std::filesystem::directory_entry& entry = *it;

				std::error_code entryError;
				if (entry.is_symlink(entryError) && !entryError) {
					// Avoid counting linked directories outside the project or following cycles.
				}
				else {
					entryError.clear();
					if (entry.is_directory(entryError) && !entryError) {
						pendingDirectories.push_back(entry.path());
					}
					else {
						entryError.clear();
						if (entry.is_regular_file(entryError) && !entryError) {
							entryError.clear();
							const std::uintmax_t fileSize = entry.file_size(entryError);
							if (!entryError) {
								AddSaturating(total, fileSize);
							}
						}
					}
				}

				ec.clear();
				it.increment(ec);
				if (ec) {
					break;
				}
			}
		}

		return total;
	}

	static bool IsSafeProjectDeleteRoot(const std::filesystem::path& path) {
		if (path.empty()) {
			return false;
		}

		std::error_code ec;
		std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
		if (ec) {
			return false;
		}

		absolutePath = absolutePath.lexically_normal();
		if (!absolutePath.has_filename() || absolutePath == absolutePath.root_path()) {
			return false;
		}

		// Require at least 2 path components past the root. This blocks
		// disasters like `C:\Windows` or `/usr` while still allowing any
		// legitimate `<drive>/parent/project` layout. `relative()` against
		// the root_path drops the drive/leading-slash so its components are
		// strictly post-root.
		std::filesystem::path postRoot = absolutePath.lexically_relative(absolutePath.root_path());
		std::size_t componentCount = 0;
		for (const auto& part : postRoot) {
			if (!part.empty() && part != ".") {
				++componentCount;
			}
		}
		if (componentCount < 2) {
			return false;
		}

		// Reject paths whose leading component matches a well-known system
		// folder name. Defense-in-depth — the depth check above already
		// rules out most danger, but a corrupted registry pointing at
		// `C:\Windows\Temp\SomethingProjecty` shouldn't reach remove_all
		// just because something forged an index-project.json there.
		static constexpr const char* k_SystemFolderNames[] = {
			"Windows", "Program Files", "Program Files (x86)", "ProgramData",
			"System Volume Information", "$Recycle.Bin",
			"usr", "etc", "bin", "sbin", "boot", "lib", "lib64", "dev",
			"proc", "sys", "var"
		};
		const std::string firstComponent = postRoot.begin()->string();
		for (const char* banned : k_SystemFolderNames) {
			if (firstComponent == banned) {
				return false;
			}
		}

		return true;
	}

	// ── Lifecycle ───────────────────────────────────────────────────

	void LauncherLayer::OnAttach(Application& app) {
		(void)app;
		Application::SetIsPlaying(false);

		// Keep the launcher's main loop ticking even when the OS window
		// loses focus. Without this, opening a project while the launcher
		// is in the background made the progress overlay freeze (worker
		// kept running, but the polling that surfaces stage/progress was
		// gated on the render loop). With it, the launcher renders in the
		// background and the open pipeline reports progress regardless of
		// focus, plus the editor process spawns at the moment the worker
		// actually finishes — not whenever the user clicks back.
		Application::SetRunInBackground(true);

		m_Registry.Load();
		m_Registry.ValidateAll();
		m_Registry.Save();

		LoadLauncherSettings();

		// Default projects location falls back to the engine's default if
		// the settings file hasn't been written yet — matches the prior
		// hard-coded behaviour without requiring a settings round-trip on
		// the first launch.
		const std::string defaultLocation = m_DefaultProjectsLocation.empty()
			? IndexProject::GetDefaultProjectsDir()
			: m_DefaultProjectsLocation;
		std::snprintf(m_NewProjectLocation, sizeof(m_NewProjectLocation), "%s",
			defaultLocation.c_str());

		IDX_INFO_TAG("Launcher", "Index Launcher opened ({} project(s))", m_Registry.GetProjects().size());
	}

	void LauncherLayer::OnPreRender(Application& app) {
		(void)app;
		Localization::Poll();
		PollCreateProjectTask();
		RenderLauncherPanel();
	}

	void LauncherLayer::OnDetach(Application& app) {
		(void)app;
		ResetCreateProjectTask();
		if (m_OpenTask.Worker.joinable()) {
			m_OpenTask.Worker.join();
		}
		// Signal stop to all size-calc workers before clearing. Each task's jthread
		// destructor will block until the worker observes the stop or finishes, so
		// asking them all to stop first lets the joins overlap rather than serializing.
		for (auto& [_, task] : m_ProjectSizeTasks) {
			if (task) {
				task->Worker.request_stop();
			}
		}
		m_ProjectSizeTasks.clear();
	}

	void LauncherLayer::ResetCreateProjectTask(bool clearWorker) {
		if (clearWorker && m_CreateTask.Worker.joinable()) {
			m_CreateTask.Worker.join();
		}

		std::scoped_lock lock(m_CreateTask.Mutex);
		m_CreateTask.Worker = std::thread();
		m_CreateTask.CreatedProject.reset();
		m_CreateTask.Error.clear();
		m_CreateTask.Stage = "Idle";
		m_CreateTask.Progress = 0.0f;
		m_CreateTask.BuildExitCode = 0;
		m_CreateTask.Running = false;
		m_CreateTask.Finished = false;
		m_CreateTask.Success = false;
		m_CreateTask.InitialBuildSucceeded = true;
	}

	void LauncherLayer::StartCreateProjectAsync(const std::string& name, const std::string& location) {
		ResetCreateProjectTask();

		m_CreateError.clear();
		m_CloseCreatePopup = false;
		m_IsCreating = true;

		{
			std::scoped_lock lock(m_CreateTask.Mutex);
			m_CreateTask.Stage = "Preparing project...";
			m_CreateTask.Progress = 0.02f;
			m_CreateTask.Running = true;
		}

		m_CreateTask.Worker = std::thread([this, name, location]() {
			auto updateProgress = [this](float progress, std::string_view stage) {
				std::scoped_lock lock(m_CreateTask.Mutex);
				m_CreateTask.Progress = progress;
				m_CreateTask.Stage = std::string(stage);
			};

			try {
				const std::string fullPath = Path::Combine(location, name);
				if (std::filesystem::exists(fullPath)) {
					std::scoped_lock lock(m_CreateTask.Mutex);
					m_CreateTask.Error = "Directory already exists: " + fullPath;
					m_CreateTask.Stage = "Project creation failed";
					m_CreateTask.Finished = true;
					m_CreateTask.Running = false;
					m_CreateTask.Success = false;
					return;
				}

				updateProgress(0.05f, "Creating project...");
				IndexProject project = IndexProject::Create(name, location, updateProgress);

				// Project-local packages affect the engine solution, so we regenerate. If
				// the project has no packages, regen is still cheap and keeps the solution
				// in sync with the latest engine package set — no rebuild follows.
				updateProgress(0.55f, "Regenerating engine solution...");
				IndexProject::RegenerateResult regen = IndexProject::RegenerateSolutionForProject(project.RootDirectory);
				if (!regen.Succeeded && regen.ExitCode != -1) {
					IDX_WARN_TAG("Launcher", "Solution regeneration returned non-zero ({}); continuing anyway.", regen.ExitCode);
				}

				// CRITICAL: do NOT call BuildSolution() here — that would try to relink
				// Index-Engine.dll while the launcher process holds it loaded, failing with
				// LNK1104 on the locked .ilk. Engine binaries are already present from
				// Setup.bat / Visual Studio. Build only the project-local package projects
				// (if any) — those are new artifacts the engine doesn't have yet.
				const std::vector<std::string> packageNames =
					IndexProject::EnumerateProjectLocalPackages(project.RootDirectory);
				if (!packageNames.empty()) {
					updateProgress(0.65f, "Building project-local packages...");
					std::vector<std::string> targets;
					targets.reserve(packageNames.size() * 2);
					for (const std::string& pkg : packageNames) {
						targets.push_back("Pkg." + pkg + ".Native");
						targets.push_back("Pkg." + pkg);
					}
					IndexProject::BuildResult pkgBuild = IndexProject::BuildSolutionTargets(targets);
					if (!pkgBuild.Succeeded) {
						IDX_WARN_TAG("Launcher", "Project-local package build failed (exit code {}); continuing anyway.", pkgBuild.ExitCode);
					}
				}

				updateProgress(0.90f, "Compiling starter scripts...");
				const std::string buildConfiguration = IndexProject::GetActiveBuildConfiguration();
				Process::Result buildResult = Process::Run({
					"dotnet",
					"build",
					project.CsprojPath,
					"-c", buildConfiguration,
					"--nologo",
					"-v", "q",
					"-nowarn:CS8632",
					"-p:DefineConstants=" + IndexProject::BuildManagedDefineConstants("INDEX_EDITOR")
					});

				std::scoped_lock lock(m_CreateTask.Mutex);
				m_CreateTask.CreatedProject = project;
				m_CreateTask.BuildExitCode = buildResult.ExitCode;
				m_CreateTask.InitialBuildSucceeded = buildResult.Succeeded();
				m_CreateTask.Progress = 1.0f;
				m_CreateTask.Stage = buildResult.Succeeded()
					? "Project ready."
					: "Project ready. Initial script build failed.";
				m_CreateTask.Finished = true;
				m_CreateTask.Running = false;
				m_CreateTask.Success = true;
			}
			catch (const std::exception& e) {
				std::scoped_lock lock(m_CreateTask.Mutex);
				m_CreateTask.Error = e.what();
				m_CreateTask.Stage = "Project creation failed";
				m_CreateTask.Finished = true;
				m_CreateTask.Running = false;
				m_CreateTask.Success = false;
			}
		});
	}

	void LauncherLayer::PollCreateProjectTask() {
		bool finished = false;
		bool success = false;
		bool initialBuildSucceeded = true;
		int buildExitCode = 0;
		std::string error;
		std::optional<IndexProject> createdProject;

		{
			std::scoped_lock lock(m_CreateTask.Mutex);
			finished = m_CreateTask.Finished;
			if (!finished) {
				return;
			}

			success = m_CreateTask.Success;
			initialBuildSucceeded = m_CreateTask.InitialBuildSucceeded;
			buildExitCode = m_CreateTask.BuildExitCode;
			error = m_CreateTask.Error;
			createdProject = m_CreateTask.CreatedProject;
		}

		if (m_CreateTask.Worker.joinable()) {
			m_CreateTask.Worker.join();
		}

		m_IsCreating = false;

		if (!success) {
			m_CreateError = error;
			ResetCreateProjectTask(false);
			return;
		}

		if (!createdProject.has_value()) {
			m_CreateError = "Project creation finished without a project result.";
			ResetCreateProjectTask(false);
			return;
		}

		m_Registry.AddProject(createdProject->Name, createdProject->RootDirectory);
		m_Registry.Save();

		if (!initialBuildSucceeded) {
			IDX_WARN_TAG("Launcher", "Project created, but the initial script build failed (exit code {}).", buildExitCode);
		}

		m_CloseCreatePopup = true;
		m_PendingCreatedProjectOpen = LauncherProjectEntry{ createdProject->Name, createdProject->RootDirectory };
		ResetCreateProjectTask(false);
	}

	// ── Main Panel ──────────────────────────────────────────────────

	void LauncherLayer::RenderLauncherPanel() {
		// Pull async open-task results before we draw anything.
		PollOpenProjectTask();

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->Pos);
		ImGui::SetNextWindowSize(viewport->Size);

		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;

		ImGui::Begin("##Launcher", nullptr, flags);

		ImGui::TextUnformatted(IDX_TR("launcher.title").c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("%s", IDX_VERSION);
		ImGui::Separator();
		ImGui::Spacing();

		// Snapshot async open-task state for the floating progress strip.
		// Reading under lock so the worker thread's writes are observed
		// coherently.
		bool taskRunning = false;
		std::string taskStage;
		float taskProgress = 0.0f;
		{
			std::scoped_lock lock(m_OpenTask.Mutex);
			taskRunning = m_OpenTask.Running;
			taskStage = m_OpenTask.Stage;
			taskProgress = m_OpenTask.Progress;
		}

		// Non-blocking progress card. The launcher used to draw a
		// fullscreen overlay that blocked every other interaction for the
		// duration of the open pipeline (potentially many seconds while
		// MSBuild ran). Now the open task gets its own auto-sized window
		// — the user can keep browsing projects, edit settings, or start
		// a second open (refused upstream) while the previous one is
		// still spinning up.
		if (m_IsOpening || taskRunning) {
			// Dismiss the post-launch grace period 1.5 s after the worker
			// reports success. While the worker is still running we never
			// dismiss — the user can't cancel a running build, but they
			// CAN keep using the rest of the launcher in the meantime.
			if (!taskRunning) {
				float elapsed = std::chrono::duration<float>(
					std::chrono::steady_clock::now() - m_OpenStartTime).count();
				if (elapsed >= 1.5f) {
					m_IsOpening = false;
					if (!m_DeferredUpdatePath.empty()) {
						m_Registry.UpdateLastOpened(m_DeferredUpdatePath);
						m_Registry.Save();
						m_DeferredUpdatePath.clear();
					}
				}
			}

			// Pin the progress card to the bottom-right of the launcher
			// viewport so it never sits over a project row the user might
			// want to click. Auto-size to its contents.
			const ImVec2 pad{ 16.0f, 16.0f };
			const ImVec2 anchor{
				viewport->Pos.x + viewport->Size.x - pad.x,
				viewport->Pos.y + viewport->Size.y - pad.y,
			};
			ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
			ImGui::SetNextWindowBgAlpha(0.92f);
			ImGuiWindowFlags overlayFlags = ImGuiWindowFlags_AlwaysAutoResize
				| ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
				| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
				| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;

			if (ImGui::Begin("##LauncherOpenStatus", nullptr, overlayFlags)) {
				ImGui::TextUnformatted(IDX_TR("launcher.progress.opening_project").c_str());
				ImGui::TextDisabled("%s", m_OpeningProjectName.c_str());
				ImGui::Separator();
				if (taskRunning) {
					ImGui::TextUnformatted(taskStage.empty()
						? IDX_TR("launcher.progress.working").c_str()
						: taskStage.c_str());
					ImGui::ProgressBar(taskProgress, ImVec2(260, 0), "");
				}
				else {
					ImGui::TextUnformatted(IDX_TR("launcher.progress.launching_editor").c_str());
					float elapsed = std::chrono::duration<float>(
						std::chrono::steady_clock::now() - m_OpenStartTime).count();
					ImGui::ProgressBar(fmodf(elapsed * 0.3f, 1.0f), ImVec2(260, 0), "");
				}
			}
			ImGui::End();

			// IMPORTANT: don't return — the rest of the launcher UI (project
			// list, create/add/settings buttons, popups) still wants to draw
			// underneath. The status strip floats over the bottom-right
			// corner without stealing focus.
		}

		float panelWidth = ImGui::GetContentRegionAvail().x;
		float rightColWidth = 200.0f;

		// Sort + refresh strip above the project list. The combo controls
		// the sort axis (Last Opened, Name, Created), the arrow button
		// flips the direction, and the refresh icon re-loads launcher.json
		// + clears the size cache so projects added or modified outside
		// the launcher show up without restarting the app.
		const float strip_h = ImGui::GetFrameHeight();
		const float refreshW = strip_h;
		const float reverseW = strip_h;
		const float listColW = panelWidth - rightColWidth - 10.0f;

		ImGui::BeginGroup();
		const std::string sortLabelText = IDX_TR("launcher.sort.label");
		const float comboW = listColW - refreshW - reverseW
			- ImGui::GetStyle().ItemSpacing.x * 2.0f
			- ImGui::CalcTextSize(sortLabelText.c_str()).x - ImGui::GetStyle().ItemSpacing.x;
		ImGui::TextUnformatted(sortLabelText.c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(comboW);
		const std::string sortLastOpened = IDX_TR("launcher.sort.mode.last_opened");
		const std::string sortName = IDX_TR("launcher.sort.mode.name");
		const std::string sortCreated = IDX_TR("launcher.sort.mode.created");
		const char* k_SortLabels[] = { sortLastOpened.c_str(), sortName.c_str(), sortCreated.c_str() };
		int sortMode = static_cast<int>(m_SortMode);
		if (ImGui::Combo("##LauncherSort", &sortMode, k_SortLabels, IM_ARRAYSIZE(k_SortLabels))) {
			m_SortMode = static_cast<SortMode>(sortMode);
			SaveLauncherSettings();
		}
		ImGui::SameLine();
		const char* arrow = m_SortReverse ? "^" : "v";
		if (ImGui::Button(arrow, ImVec2(reverseW, strip_h))) {
			m_SortReverse = !m_SortReverse;
			SaveLauncherSettings();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", m_SortReverse
				? IDX_TR("launcher.sort.tooltip.ascending").c_str()
				: IDX_TR("launcher.sort.tooltip.descending").c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button("R", ImVec2(refreshW, strip_h))) {
			RefreshProjectsList();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", IDX_TR("launcher.sort.tooltip.refresh").c_str());
		}
		ImGui::EndGroup();

		// Left column: project list
		ImGui::BeginChild("##ProjectList", ImVec2(listColW, 0), true);
		RenderProjectList();
		ImGui::EndChild();

		ImGui::SameLine();

		// Right column: actions (L4: removed selection-dependent buttons)
		ImGui::BeginChild("##Actions", ImVec2(rightColWidth, 0));

		if (ImGui::Button(IDX_TR("launcher.action.create_new_project").c_str(), ImVec2(-1, 0))) {
			m_OpenCreatePopup = true;
			m_CreateError.clear();
			m_IsCreating = false;
			m_NewProjectName[0] = '\0';
			// Re-snap the location to whatever the user has set in
			// settings — they may have changed it since OnAttach.
			const std::string defaultLocation = m_DefaultProjectsLocation.empty()
				? IndexProject::GetDefaultProjectsDir()
				: m_DefaultProjectsLocation;
			std::snprintf(m_NewProjectLocation, sizeof(m_NewProjectLocation), "%s",
				defaultLocation.c_str());
		}

		ImGui::Spacing();

		if (ImGui::Button(IDX_TR("launcher.action.add_existing_project").c_str(), ImVec2(-1, 0))) {
			BrowseForExistingProject();
		}

		ImGui::Spacing();

		if (ImGui::Button(IDX_TR("launcher.action.settings").c_str(), ImVec2(-1, 0))) {
			m_OpenSettingsPopup = true;
		}

		if (!m_BrowseError.empty()) {
			ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", m_BrowseError.c_str());
		}

		// L3: Show open error if any
		if (!m_OpenError.empty()) {
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", m_OpenError.c_str());
		}

		if (!m_ProjectActionError.empty()) {
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", m_ProjectActionError.c_str());
		}

		ImGui::EndChild();

		// L6: Render create popup modal inside the launcher window context
		RenderCreateProjectPopup();
		RenderDeleteProjectPopups();
		RenderSettingsPopup();
		RenderProjectInfoPopup();

		ImGui::End();
	}

	// ── Project List ────────────────────────────────────────────────

	void LauncherLayer::RenderProjectList() {
		const auto sortedProjects = GetSortedProjectsView();

		if (sortedProjects.empty()) {
			ImGui::TextDisabled("%s", IDX_TR("launcher.list.empty.title").c_str());
			ImGui::TextDisabled("%s", IDX_TR("launcher.list.empty.subtitle").c_str());
			return;
		}

		std::string removePath;

		for (int i = 0; i < static_cast<int>(sortedProjects.size()); i++) {
			const auto& entry = *sortedProjects[i];

			ImGui::PushID(i);

			std::string timeStr = FormatRelativeTime(entry.LastOpened);
			std::string sizeStr = GetProjectSizeDisplayText(entry);

			ImVec2 cursorPos = ImGui::GetCursorScreenPos();
			float rowWidth = ImGui::GetContentRegionAvail().x;
			float buttonWidth = 60.0f;
			float rowHeight = 68.0f;

			// Draw an invisible item spanning the row for layout + right-click context
			ImGui::InvisibleButton("##Row", ImVec2(rowWidth - buttonWidth - 8, rowHeight));

			// Right-click context menu per item
			if (ImGui::BeginPopupContextItem("##RowCtx")) {
				if (ImGui::MenuItem(IDX_TR("launcher.context.info").c_str())) {
					m_PendingInfoProject = entry;
					m_OpenInfoPopup = true;
				}

				ImGui::Separator();

				if (ImGui::MenuItem(IDX_TR("launcher.context.open_in_explorer").c_str())) {
					OpenProjectInExplorer(entry);
				}

				if (ImGui::MenuItem(IDX_TR("launcher.context.remove_from_list").c_str())) {
					removePath = entry.Path;
				}

				ImGui::Separator();
				if (ImGui::MenuItem(IDX_TR("launcher.context.delete_project").c_str())) {
					RequestProjectDelete(entry);
				}
				ImGui::EndPopup();
			}

			// Draw hover/active highlight
			bool hovered = ImGui::IsItemHovered();
			if (hovered) {
				ImGui::GetWindowDrawList()->AddRectFilled(
					cursorPos,
					ImVec2(cursorPos.x + rowWidth, cursorPos.y + rowHeight),
					ImGui::GetColorU32(ImGuiCol_HeaderHovered));
			}

			// Draw name
			ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + 8, cursorPos.y + 4));
			ImGui::TextUnformatted(entry.Name.c_str());

			// Draw path
			ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + 8, cursorPos.y + 24));
			ImGui::TextDisabled("%s", entry.Path.c_str());

			// Draw size with a placeholder while the background scan is running.
			ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + 8, cursorPos.y + 44));
			const std::string sizeRowText = Localization::Format("launcher.list.size_prefix", sizeStr);
			ImGui::TextDisabled("%s", sizeRowText.c_str());

			// Draw relative time in upper-right (offset left to leave room for button)
			if (!timeStr.empty()) {
				float timeWidth = ImGui::CalcTextSize(timeStr.c_str()).x;
				ImGui::SetCursorScreenPos(ImVec2(
					cursorPos.x + rowWidth - buttonWidth - timeWidth - 20,
					cursorPos.y + 4));
				ImGui::TextDisabled("%s", timeStr.c_str());
			}

			// Per-item Open button on the right side
			ImGui::SetCursorScreenPos(ImVec2(
				cursorPos.x + rowWidth - buttonWidth - 4,
				cursorPos.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f));
			if (ImGui::Button(IDX_TR("launcher.list.open").c_str(), ImVec2(buttonWidth, 0))) {
				Timer timer;
				OpenProject(entry);
				IDX_INFO_TAG("Project", "Opening Took: " + StringHelper::ToString(timer));
			}

			// Advance cursor past the row
			ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + rowHeight));

			ImGui::PopID();
		}

		// Deferred removal to avoid modifying the list during iteration.
		// Keying by path (not index) because the displayed order is a
		// sorted *view* over the registry — the index in `sortedProjects`
		// doesn't map back to the registry's underlying storage.
		if (!removePath.empty()) {
			m_Registry.RemoveProject(removePath);
			m_Registry.Save();
			m_ProjectSizeTasks.erase(removePath);
			m_CreatedAtCache.erase(removePath);
			m_EngineVersionCache.erase(removePath);
		}
	}

	std::shared_ptr<LauncherLayer::ProjectSizeTaskState> LauncherLayer::GetOrStartProjectSizeTask(const LauncherProjectEntry& entry) {
		auto existing = m_ProjectSizeTasks.find(entry.Path);
		if (existing != m_ProjectSizeTasks.end()) {
			return existing->second;
		}

		auto task = std::make_shared<ProjectSizeTaskState>();
		m_ProjectSizeTasks.emplace(entry.Path, task);

		std::weak_ptr<ProjectSizeTaskState> weakTask = task;
		task->Worker = std::jthread([weakTask = std::move(weakTask), path = entry.Path](std::stop_token stop) {
			std::string error;
			const std::uintmax_t bytes = CalculateProjectDirectorySize(std::filesystem::path(path), error);
			if (stop.stop_requested()) {
				return;
			}
			auto t = weakTask.lock();
			if (!t) {
				return;
			}
			std::scoped_lock lock(t->Mutex);
			t->Bytes = bytes;
			t->Error = std::move(error);
			t->Failed = !t->Error.empty();
			t->Finished = true;
		});

		return task;
	}

	std::string LauncherLayer::GetProjectSizeDisplayText(const LauncherProjectEntry& entry) {
		std::shared_ptr<ProjectSizeTaskState> task = GetOrStartProjectSizeTask(entry);

		std::scoped_lock lock(task->Mutex);
		if (!task->Finished) {
			return "calculating...";
		}
		if (task->Failed) {
			return "unavailable";
		}

		const std::uintmax_t maxSize = static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max());
		const std::size_t formattedSize = static_cast<std::size_t>(std::min(task->Bytes, maxSize));
		return StringHelper::ToIEC(formattedSize);
	}

	void LauncherLayer::RequestProjectDelete(const LauncherProjectEntry& entry) {
		m_PendingDeleteProject = entry;
		m_DeleteError.clear();
		m_ProjectActionError.clear();
		m_OpenDeleteConfirmPopup = true;
		m_OpenDeleteFinalConfirmPopup = false;
	}

	void LauncherLayer::RenderDeleteProjectPopups() {
		// Stable IDs (everything after `###`) so the translated title can
		// vary without losing the popup's identity across frames.
		constexpr const char* k_DeleteId = "###LauncherDeleteProject";
		constexpr const char* k_DeleteFinalId = "###LauncherDeleteProjectFinal";

		if (m_OpenDeleteConfirmPopup) {
			ImGui::OpenPopup(k_DeleteId);
			m_OpenDeleteConfirmPopup = false;
		}
		if (m_OpenDeleteFinalConfirmPopup) {
			ImGui::OpenPopup(k_DeleteFinalId);
			m_OpenDeleteFinalConfirmPopup = false;
		}

		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);

		const std::string deleteTitle = IDX_TR("launcher.delete.title") + k_DeleteId;
		if (ImGui::BeginPopupModal(deleteTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			if (!m_PendingDeleteProject.has_value()) {
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}

			const LauncherProjectEntry& entry = *m_PendingDeleteProject;
			const std::string msg = Localization::Format("launcher.delete.message", entry.Name);
			ImGui::TextWrapped("%s", msg.c_str());
			ImGui::Spacing();
			ImGui::TextWrapped("%s", IDX_TR("launcher.delete.body").c_str());
			ImGui::Spacing();
			const std::string folder = Localization::Format("launcher.delete.folder_label", entry.Path);
			ImGui::TextWrapped("%s", folder.c_str());
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			if (ImGui::Button(IDX_TR("launcher.delete.continue").c_str(), ImVec2(140, 0))) {
				m_OpenDeleteFinalConfirmPopup = true;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();
			if (ImGui::Button(IDX_TR("launcher.delete.cancel").c_str(), ImVec2(120, 0))) {
				m_PendingDeleteProject.reset();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);

		const std::string deleteFinalTitle = IDX_TR("launcher.delete_final.title") + k_DeleteFinalId;
		if (ImGui::BeginPopupModal(deleteFinalTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			if (!m_PendingDeleteProject.has_value()) {
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}

			const LauncherProjectEntry& entry = *m_PendingDeleteProject;
			const std::string msg = Localization::Format("launcher.delete_final.message", entry.Name);
			ImGui::TextWrapped("%s", msg.c_str());
			ImGui::Spacing();
			ImGui::TextWrapped("%s", IDX_TR("launcher.delete_final.body").c_str());
			ImGui::Spacing();
			const std::string folder = Localization::Format("launcher.delete.folder_label", entry.Path);
			ImGui::TextWrapped("%s", folder.c_str());

			if (!m_DeleteError.empty()) {
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", m_DeleteError.c_str());
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.12f, 0.12f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.18f, 0.18f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.08f, 0.08f, 1.0f));
			const bool deleteClicked = ImGui::Button(IDX_TR("launcher.delete_final.confirm").c_str(), ImVec2(180, 0));
			ImGui::PopStyleColor(3);

			if (deleteClicked && DeleteProjectFromDisk(entry)) {
				m_PendingDeleteProject.reset();
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();
			if (ImGui::Button(IDX_TR("launcher.delete.cancel").c_str(), ImVec2(120, 0))) {
				m_PendingDeleteProject.reset();
				m_DeleteError.clear();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	// ── Create Project Popup ────────────────────────────────────────

	void LauncherLayer::RenderCreateProjectPopup() {
		constexpr const char* k_CreateId = "###LauncherCreateProject";

		if (m_OpenCreatePopup) {
			ImGui::OpenPopup(k_CreateId);
			m_OpenCreatePopup = false;
		}

		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_Appearing);

		const std::string createTitle = IDX_TR("launcher.create.title") + k_CreateId;
		if (ImGui::BeginPopupModal(createTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			float createProgress = 0.0f;
			std::string createStage;
			bool createTaskRunning = false;
			{
				std::scoped_lock lock(m_CreateTask.Mutex);
				createProgress = m_CreateTask.Progress;
				createStage = m_CreateTask.Stage;
				createTaskRunning = m_CreateTask.Running;
			}

			if (m_CloseCreatePopup) {
				m_CloseCreatePopup = false;
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();

				if (m_PendingCreatedProjectOpen.has_value()) {
					OpenProject(*m_PendingCreatedProjectOpen);
					m_PendingCreatedProjectOpen.reset();
				}
				return;
			}

			ImGui::TextUnformatted(IDX_TR("launcher.create.project_name").c_str());
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText("##ProjName", m_NewProjectName, sizeof(m_NewProjectName));

			ImGui::Spacing();
			ImGui::TextUnformatted(IDX_TR("launcher.create.location").c_str());
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText("##ProjLocation", m_NewProjectLocation, sizeof(m_NewProjectLocation));

			ImGui::Spacing();
			std::string preview = Path::Combine(m_NewProjectLocation, m_NewProjectName);
			const std::string previewText = Localization::Format("launcher.create.path_preview", preview);
			ImGui::TextDisabled("%s", previewText.c_str());

			if (!m_CreateError.empty()) {
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", m_CreateError.c_str());
			}

			if (m_IsCreating || createTaskRunning) {
				ImGui::Spacing();
				ImGui::TextDisabled("%s", createStage.empty()
					? IDX_TR("launcher.create.default_stage").c_str()
					: createStage.c_str());
				ImGui::ProgressBar(createProgress, ImVec2(-1, 0));
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			bool canCreate = IndexProject::IsValidProjectName(m_NewProjectName) && !m_IsCreating;
			if (!canCreate) ImGui::BeginDisabled();

			if (ImGui::Button(IDX_TR("launcher.create.create_button").c_str(), ImVec2(120, 0))) {
				Timer timer;
				StartCreateProjectAsync(std::string(m_NewProjectName), std::string(m_NewProjectLocation));
				IDX_INFO_TAG("Project", "Queued async project creation in {}", StringHelper::ToString(timer));
			}

			if (!canCreate) ImGui::EndDisabled();

			ImGui::SameLine();
			if (m_IsCreating) {
				ImGui::BeginDisabled();
			}
			if (ImGui::Button(m_IsCreating
				? IDX_TR("launcher.create.working").c_str()
				: IDX_TR("launcher.create.cancel").c_str(), ImVec2(120, 0))) {
				ImGui::CloseCurrentPopup();
			}
			if (m_IsCreating) {
				ImGui::EndDisabled();
			}

			ImGui::EndPopup();
		}
	}

	// ── Open Project ────────────────────────────────────────────────
	// Launches the editor process but keeps the launcher open.

	void LauncherLayer::OpenProject(const LauncherProjectEntry& entry) {
		if (m_IsOpening) return; // Already in post-launch grace overlay
		{
			std::scoped_lock lock(m_OpenTask.Mutex);
			if (m_OpenTask.Running) return; // Worker already busy
		}

		m_OpenError.clear();

#ifdef IDX_PLATFORM_WINDOWS
		// Refuse to spawn a duplicate editor for the same project.
		{
			auto it = m_RunningProjects.find(entry.Path);
			if (it != m_RunningProjects.end()) {
				HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, it->second);
				if (hProc) {
					DWORD exitCode = 0;
					if (GetExitCodeProcess(hProc, &exitCode) && exitCode == STILL_ACTIVE) {
						CloseHandle(hProc);
						m_OpenError = IDX_TR("launcher.error.project_already_open");
						return;
					}
					CloseHandle(hProc);
				}
				m_RunningProjects.erase(it);
			}
		}
#endif

		// Initialize task state and spawn the worker. The render thread shows progress
		// from the task state each frame; PollOpenProjectTask collects results.
		{
			std::scoped_lock lock(m_OpenTask.Mutex);
			m_OpenTask.Entry = entry;
			m_OpenTask.Stage = "Preparing...";
			m_OpenTask.Progress = 0.02f;
			m_OpenTask.Error.clear();
			m_OpenTask.Running = true;
			m_OpenTask.Finished = false;
			m_OpenTask.Success = false;
#ifdef IDX_PLATFORM_WINDOWS
			m_OpenTask.SpawnedProcessId = 0;
#endif
		}

		if (m_OpenTask.Worker.joinable()) {
			m_OpenTask.Worker.join();
		}

		// Show the in-progress overlay immediately by setting m_IsOpening — the overlay
		// reads stage/progress from m_OpenTask and dismisses 1.5 s after the worker
		// reports success. m_OpeningProjectName is what the overlay shows underneath.
		m_IsOpening = true;
		m_OpeningProjectName = entry.Name;

		m_OpenTask.Worker = std::thread([this, entry]() {
			OpenProjectWorkerBody(entry);
		});
	}

	void LauncherLayer::OpenProjectWorkerBody(const LauncherProjectEntry& entry) {
		auto setStage = [this](const std::string& stage, float progress) {
			std::scoped_lock lock(m_OpenTask.Mutex);
			m_OpenTask.Stage = stage;
			m_OpenTask.Progress = progress;
		};

		auto fail = [this](const std::string& error) {
			std::scoped_lock lock(m_OpenTask.Mutex);
			m_OpenTask.Stage = "Failed";
			m_OpenTask.Progress = 1.0f;
			m_OpenTask.Error = error;
			m_OpenTask.Finished = true;
			m_OpenTask.Running = false;
			m_OpenTask.Success = false;
		};

		setStage("Regenerating engine solution...", 0.10f);
		IndexProject::RegenerateResult regen = IndexProject::RegenerateSolutionForProject(entry.Path);
		if (!regen.Succeeded && regen.ExitCode != -1) {
			fail("Solution regen failed (premake exit code " + std::to_string(regen.ExitCode) + ").");
			return;
		}

		// Build ONLY the project-local packages — never the engine itself. The launcher
		// has Index-Engine.dll loaded; rebuilding it from this same process would fail
		// with LNK1104 on the locked .ilk file. Engine binaries are already in place
		// from Setup.bat. Projects without packages need zero MSBuild work.
		const std::vector<std::string> packageNames =
			IndexProject::EnumerateProjectLocalPackages(entry.Path);
		if (!packageNames.empty()) {
			setStage("Building project-local packages...", 0.40f);
			std::vector<std::string> targets;
			targets.reserve(packageNames.size() * 2);
			for (const std::string& pkg : packageNames) {
				targets.push_back("Pkg." + pkg + ".Native");
				targets.push_back("Pkg." + pkg);
			}
			IndexProject::BuildResult build = IndexProject::BuildSolutionTargets(targets);
			if (!build.Succeeded) {
				fail("Project package build failed (MSBuild exit code " + std::to_string(build.ExitCode) + ").");
				return;
			}
		}

		setStage("Launching editor...", 0.90f);

#ifdef IDX_PLATFORM_WINDOWS
		auto exeDir = std::filesystem::path(Path::ExecutableDir());
		auto editorExe = exeDir / "Index-Editor.exe";
		if (!std::filesystem::exists(editorExe)) {
			editorExe = exeDir / ".." / "Index-Editor" / "Index-Editor.exe";
		}
		if (!std::filesystem::exists(editorExe)) {
			fail("Index-Editor.exe not found.");
			return;
		}

		std::error_code canonicalError;
		std::filesystem::path resolvedEditorExe = std::filesystem::weakly_canonical(editorExe, canonicalError);
		if (canonicalError) {
			resolvedEditorExe = editorExe.lexically_normal();
		}

		const std::wstring projectPath = Utf8ToWide(entry.Path);
		if (projectPath.empty() && !entry.Path.empty()) {
			fail("Project path is not valid UTF-8.");
			return;
		}

		const std::wstring commandLine =
			L"\"" + resolvedEditorExe.native() + L"\" --project=\"" + projectPath + L"\"";
		std::vector<wchar_t> buf(commandLine.begin(), commandLine.end());
		buf.push_back(L'\0');

		const std::wstring workingDirectory = resolvedEditorExe.parent_path().native();

		STARTUPINFOW si{};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi{};

		if (!CreateProcessW(resolvedEditorExe.c_str(), buf.data(), nullptr, nullptr,
			FALSE, CREATE_NEW_PROCESS_GROUP, nullptr,
			workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &si, &pi))
		{
			const DWORD errorCode = GetLastError();
			fail("Failed to launch editor: " + FormatWindowsError(errorCode));
			return;
		}

		const DWORD spawnedPid = pi.dwProcessId;
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);

		IDX_INFO_TAG("Launcher", "Opened project: {} at {}", entry.Name, entry.Path);

		std::scoped_lock lock(m_OpenTask.Mutex);
		m_OpenTask.Stage = "Editor launched";
		m_OpenTask.Progress = 1.0f;
		m_OpenTask.SpawnedProcessId = spawnedPid;
		m_OpenTask.Success = true;
		m_OpenTask.Finished = true;
		m_OpenTask.Running = false;
#else
		fail("Project open is only implemented on Windows.");
#endif
	}

	void LauncherLayer::PollOpenProjectTask() {
		bool finished = false;
		bool success = false;
		std::string error;
		LauncherProjectEntry entry;
#ifdef IDX_PLATFORM_WINDOWS
		DWORD pid = 0;
#endif

		{
			std::scoped_lock lock(m_OpenTask.Mutex);
			if (!m_OpenTask.Finished) return;
			finished = true;
			success = m_OpenTask.Success;
			error = m_OpenTask.Error;
			entry = m_OpenTask.Entry;
#ifdef IDX_PLATFORM_WINDOWS
			pid = m_OpenTask.SpawnedProcessId;
#endif
			m_OpenTask.Finished = false;
		}

		if (m_OpenTask.Worker.joinable()) {
			m_OpenTask.Worker.join();
		}

		if (success) {
			// Editor process spawned; transition to the post-launch grace overlay so
			// the user sees the launch confirmation for a beat before the launcher
			// becomes interactive again.
			m_OpenStartTime = std::chrono::steady_clock::now();
			m_OpeningProjectName = entry.Name;
			m_DeferredUpdatePath = entry.Path;
#ifdef IDX_PLATFORM_WINDOWS
			m_RunningProjects[entry.Path] = pid;
#endif
		}
		else {
			m_OpenError = error.empty() ? "Failed to open project." : error;
			m_IsOpening = false;
			IDX_ERROR_TAG("Launcher", "Open project failed: {}", m_OpenError);
		}
	}

	void LauncherLayer::OpenProjectInExplorer(const LauncherProjectEntry& entry) {
		m_ProjectActionError.clear();

		std::error_code ec;
		if (!std::filesystem::exists(entry.Path, ec) || ec) {
			m_ProjectActionError = ec ? "Project folder could not be checked: " + ec.message() : "Project folder not found";
			return;
		}

#ifdef IDX_PLATFORM_WINDOWS
		const std::wstring projectPath = Utf8ToWide(entry.Path);
		if (projectPath.empty() && !entry.Path.empty()) {
			m_ProjectActionError = "Project path is not valid UTF-8";
			return;
		}

		const HINSTANCE result = ShellExecuteW(nullptr, L"open", projectPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		const INT_PTR resultCode = reinterpret_cast<INT_PTR>(result);
		if (resultCode <= 32) {
			m_ProjectActionError = "Failed to open project in Explorer (code " + std::to_string(resultCode) + ")";
		}
#elif defined(IDX_PLATFORM_LINUX)
		if (!Process::LaunchDetached({ "xdg-open", entry.Path })) {
			m_ProjectActionError = "Failed to open project in file manager";
		}
#else
		m_ProjectActionError = "Open in Explorer is not supported on this platform";
#endif
	}

	bool LauncherLayer::DeleteProjectFromDisk(const LauncherProjectEntry& entry) {
		auto fail = [this](std::string message) -> bool {
			m_DeleteError = std::move(message);
			m_ProjectActionError = m_DeleteError;
			return false;
		};

		m_DeleteError.clear();
		m_ProjectActionError.clear();

		std::error_code ec;
		std::filesystem::path projectPath = std::filesystem::weakly_canonical(entry.Path, ec);
		if (ec) {
			ec.clear();
			projectPath = std::filesystem::absolute(entry.Path, ec);
			if (ec) {
				return fail("Project folder could not be resolved: " + ec.message());
			}
		}
		projectPath = projectPath.lexically_normal();

		if (!IsSafeProjectDeleteRoot(projectPath)) {
			return fail("Refusing to delete an unsafe project folder path.");
		}

		const bool exists = std::filesystem::exists(projectPath, ec);
		if (ec) {
			return fail("Project folder could not be checked: " + ec.message());
		}
		if (!exists) {
			m_Registry.RemoveProject(entry.Path);
			m_Registry.Save();
			m_ProjectSizeTasks.erase(entry.Path);
			return true;
		}

		const bool isDirectory = std::filesystem::is_directory(projectPath, ec);
		if (ec) {
			return fail("Project folder could not be checked: " + ec.message());
		}
		if (!isDirectory) {
			return fail("Project path is not a folder. Use Remove from List instead.");
		}

		if (!IndexProject::Validate(projectPath.string())) {
			return fail("Folder no longer looks like a Index project. Use Remove from List instead.");
		}

		std::filesystem::remove_all(projectPath, ec);
		if (ec) {
			return fail("Failed to delete project folder: " + ec.message());
		}

		m_Registry.RemoveProject(entry.Path);
		m_Registry.Save();
		m_ProjectSizeTasks.erase(entry.Path);

		IDX_INFO_TAG("Launcher", "Deleted project '{}' at {}", entry.Name, projectPath.string());
		return true;
	}

	// ── Browse for Existing Project ─────────────────────────────────

	void LauncherLayer::BrowseForExistingProject() {
		m_BrowseError.clear();

#ifdef IDX_PLATFORM_WINDOWS
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

		IFileOpenDialog* dialog = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
			IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog));

		if (SUCCEEDED(hr)) {
			DWORD options;
			dialog->GetOptions(&options);
			dialog->SetOptions(options | FOS_PICKFOLDERS);
			dialog->SetTitle(L"Select Index Project Folder");

			hr = dialog->Show(nullptr);
			if (SUCCEEDED(hr)) {
				IShellItem* result = nullptr;
				dialog->GetResult(&result);
				if (result) {
					PWSTR widePath = nullptr;
					result->GetDisplayName(SIGDN_FILESYSPATH, &widePath);
					if (widePath) {
						int len = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
						std::string path(len - 1, '\0');
						WideCharToMultiByte(CP_UTF8, 0, widePath, -1, path.data(), len, nullptr, nullptr);
						CoTaskMemFree(widePath);

						if (IndexProject::Validate(path)) {
							auto project = IndexProject::Load(path);
							m_Registry.AddProject(project.Name, project.RootDirectory);
							m_Registry.Save();
						}
						else {
							m_BrowseError = "Not a valid Index project (missing index-project.json or Assets/)";
						}
					}
					result->Release();
				}
			}
			dialog->Release();
		}

		CoUninitialize();
#endif
	}

	// ── Sort / Refresh / Settings ───────────────────────────────────

	std::vector<const LauncherProjectEntry*> LauncherLayer::GetSortedProjectsView() const {
		const auto& projects = m_Registry.GetProjects();
		std::vector<const LauncherProjectEntry*> view;
		view.reserve(projects.size());
		for (const auto& entry : projects) view.push_back(&entry);

		auto getCreatedAt = [&](const LauncherProjectEntry* e) -> std::int64_t {
			auto it = m_CreatedAtCache.find(e->Path);
			if (it != m_CreatedAtCache.end()) return it->second;
			std::error_code ec;
			std::int64_t ts = 0;
			std::filesystem::path p(e->Path);
			auto ftime = std::filesystem::last_write_time(p, ec);
			if (!ec) {
				ts = std::chrono::duration_cast<std::chrono::seconds>(
					ftime.time_since_epoch()).count();
			}
			m_CreatedAtCache.emplace(e->Path, ts);
			return ts;
		};

		switch (m_SortMode) {
			case SortMode::LastOpened:
				std::sort(view.begin(), view.end(),
					[](const auto* a, const auto* b) { return a->LastOpened > b->LastOpened; });
				break;
			case SortMode::Name:
				std::sort(view.begin(), view.end(),
					[](const auto* a, const auto* b) {
						return std::lexicographical_compare(
							a->Name.begin(), a->Name.end(),
							b->Name.begin(), b->Name.end(),
							[](unsigned char x, unsigned char y) {
								return std::tolower(x) < std::tolower(y);
							});
					});
				break;
			case SortMode::CreatedAt:
				std::sort(view.begin(), view.end(),
					[&](const auto* a, const auto* b) { return getCreatedAt(a) > getCreatedAt(b); });
				break;
		}

		if (m_SortReverse) {
			std::reverse(view.begin(), view.end());
		}
		return view;
	}

	void LauncherLayer::RefreshProjectsList() {
		// Stop any running size workers so they don't write into the cache
		// after Load() pulls in a different project list (path-keyed map
		// makes this strictly additive, but stopping eliminates wasted IO).
		for (auto& [_, task] : m_ProjectSizeTasks) {
			if (task) task->Worker.request_stop();
		}
		m_ProjectSizeTasks.clear();
		m_CreatedAtCache.clear();
		m_EngineVersionCache.clear();

		m_Registry.Load();
		m_Registry.ValidateAll();
		m_Registry.Save();
	}

	std::string LauncherLayer::GetSettingsPath() {
		return Path::Combine(
			Path::GetSpecialFolderPath(SpecialFolder::LocalAppData),
			"Index", "launcher_settings.json");
	}

	void LauncherLayer::LoadLauncherSettings() {
		const std::string path = GetSettingsPath();
		if (!File::Exists(path)) return;

		Json::Value root;
		std::string parseError;
		const std::string text = File::ReadAllText(path);
		if (!Json::TryParse(text, root, &parseError) || !root.IsObject()) {
			IDX_CORE_WARN_TAG("Launcher", "Failed to parse '{}': {}", path, parseError);
			return;
		}

		if (const Json::Value* v = root.FindMember("defaultProjectsLocation")) {
			m_DefaultProjectsLocation = v->AsStringOr();
		}
		if (const Json::Value* v = root.FindMember("sortMode")) {
			int mode = v->AsIntOr(static_cast<int>(SortMode::LastOpened));
			if (mode >= 0 && mode <= static_cast<int>(SortMode::CreatedAt)) {
				m_SortMode = static_cast<SortMode>(mode);
			}
		}
		if (const Json::Value* v = root.FindMember("sortReverse")) {
			m_SortReverse = v->AsBoolOr(false);
		}
	}

	void LauncherLayer::SaveLauncherSettings() const {
		const std::string path = GetSettingsPath();

		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

		Json::Value root = Json::Value::MakeObject();
		root.AddMember("defaultProjectsLocation", m_DefaultProjectsLocation);
		root.AddMember("sortMode", Json::Value(static_cast<int>(m_SortMode)));
		root.AddMember("sortReverse", Json::Value(m_SortReverse));
		if (!File::WriteAllText(path, Json::Stringify(root, true))) {
			IDX_CORE_WARN_TAG("Launcher", "Failed to write launcher settings to '{}'", path);
		}
	}

	void LauncherLayer::RenderSettingsPopup() {
		constexpr const char* k_SettingsId = "###LauncherSettings";

		if (m_OpenSettingsPopup) {
			ImGui::OpenPopup(k_SettingsId);
			m_OpenSettingsPopup = false;
		}

		{
			const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
			ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		}
		ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
		const std::string settingsTitle = IDX_TR("launcher.settings.title") + k_SettingsId;
		if (!ImGui::BeginPopupModal(settingsTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			return;
		}

		// The "effective" path — what `Create New Project` would use right
		// now if the user clicked it. Falls back to the engine's bundled
		// default when the user hasn't authored an override. Always
		// non-empty (modulo a missing engine install), so the input below
		// always shows something the user can edit instead of looking
		// blank-by-default the first time the popup is opened.
		const std::string effectiveDefault = m_DefaultProjectsLocation.empty()
			? IndexProject::GetDefaultProjectsDir()
			: m_DefaultProjectsLocation;

		ImGui::TextUnformatted(IDX_TR("launcher.settings.default_location").c_str());
		ImGui::TextDisabled("%s", IDX_TR("launcher.settings.default_location_subtitle").c_str());

		// Surface the current value as a small read-only line so the user
		// can compare what's saved vs. what they're typing — particularly
		// useful when the input is the engine fallback (no explicit
		// override) so it's clear they haven't customised anything yet.
		const std::string currentLine = Localization::Format("launcher.settings.current", effectiveDefault)
			+ (m_DefaultProjectsLocation.empty() ? IDX_TR("launcher.settings.engine_default_suffix") : std::string{});
		ImGui::TextDisabled("%s", currentLine.c_str());
		ImGui::Spacing();

		// Editable buffer kept on the popup itself — sized for typical
		// long Windows paths plus a margin. Pre-fill with the effective
		// path (override or engine default) so the user immediately sees
		// what's in use rather than an empty field.
		static char s_LocationBuffer[1024]{};
		if (ImGui::IsWindowAppearing()) {
			std::snprintf(s_LocationBuffer, sizeof(s_LocationBuffer), "%s",
				effectiveDefault.c_str());
		}
		ImGui::SetNextItemWidth(-110.0f);
		ImGui::InputText("##DefaultLocation", s_LocationBuffer, sizeof(s_LocationBuffer));
		ImGui::SameLine();
		if (ImGui::Button(IDX_TR("launcher.settings.browse").c_str(), ImVec2(100, 0))) {
			BrowseForDefaultProjectsLocation();
			std::snprintf(s_LocationBuffer, sizeof(s_LocationBuffer), "%s",
				m_DefaultProjectsLocation.c_str());
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// Language selector. Persists immediately on selection — separate
		// from the Save/Cancel buttons below which only commit the
		// default-projects-location override. Hot reload is supported by
		// the Localization service: switching here re-translates every
		// IDX_TR() lookup next frame, including this popup's own labels.
		// Entries that aren't installed yet are decorated with a status
		// suffix; picking one kicks off an async download whose progress
		// shows inline below the combo (see Localization::Poll()).
		ImGui::TextUnformatted(IDX_TR("launcher.settings.language").c_str());
		const auto& languages = Localization::GetAvailableLanguages();
		const std::string& currentCode = Localization::GetCurrentLanguage();
		const std::string notInstalledSuffix = IDX_TR("launcher.settings.language.not_installed_suffix");
		int activeIdx = 0;
		for (size_t i = 0; i < languages.size(); ++i) {
			if (languages[i].Code == currentCode) {
				activeIdx = static_cast<int>(i);
				break;
			}
		}
		const char* previewName = languages.empty() ? "" : languages[activeIdx].DisplayName.c_str();
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo("##LauncherLanguageCombo", previewName)) {
			for (size_t i = 0; i < languages.size(); ++i) {
				const bool selected = (static_cast<int>(i) == activeIdx);
				std::string label = languages[i].DisplayName;
				if (languages[i].Status != Localization::LanguageStatus::Installed) {
					label += notInstalledSuffix;
				}
				if (ImGui::Selectable(label.c_str(), selected)) {
					Localization::SetLanguage(languages[i].Code);
				}
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		if (const auto download = Localization::GetActiveDownload()) {
			ImGui::Spacing();
			if (download->Failed) {
				const std::string msg = Localization::Format("launcher.settings.language.download_failed", download->Error);
				ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", msg.c_str());
			}
			else if (download->RestartRequired) {
				ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "%s",
					IDX_TR("launcher.settings.language.restart_required").c_str());
			}
			else {
				const std::string msg = Localization::Format("launcher.settings.language.downloading", download->Code);
				ImGui::TextDisabled("%s", msg.c_str());
				ImGui::ProgressBar(download->Progress, ImVec2(-1.0f, 0));
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		if (ImGui::Button(IDX_TR("launcher.settings.save").c_str(), ImVec2(100, 0))) {
			// If the user left the field at the engine default verbatim,
			// don't persist it as an override — the empty-string sentinel
			// keeps the "follows engine default" behaviour even if the
			// engine's default path changes in a future version.
			const std::string typed(s_LocationBuffer);
			if (typed == IndexProject::GetDefaultProjectsDir()) {
				m_DefaultProjectsLocation.clear();
			}
			else {
				m_DefaultProjectsLocation = typed;
			}
			SaveLauncherSettings();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(IDX_TR("launcher.settings.cancel").c_str(), ImVec2(100, 0))) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	// Formats a filesystem timestamp (seconds since epoch in system_clock terms)
	// as "YYYY-MM-DD HH:MM" in local time. Returns "unknown" for zero/invalid
	// timestamps. Local time matches the user's expectation when the dialog
	// reads "Created on …".
	static std::string FormatLocalDateTime(std::int64_t secondsSinceEpoch) {
		if (secondsSinceEpoch <= 0) return "unknown";
		std::time_t t = static_cast<std::time_t>(secondsSinceEpoch);
		std::tm tm{};
#ifdef IDX_PLATFORM_WINDOWS
		if (localtime_s(&tm, &t) != 0) return "unknown";
#else
		if (!localtime_r(&t, &tm)) return "unknown";
#endif
		std::ostringstream ss;
		ss << std::put_time(&tm, "%Y-%m-%d %H:%M");
		return ss.str();
	}

	void LauncherLayer::RenderProjectInfoPopup() {
		constexpr const char* k_InfoId = "###LauncherProjectInfo";

		if (m_OpenInfoPopup) {
			ImGui::OpenPopup(k_InfoId);
			m_OpenInfoPopup = false;
		}

		{
			const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
			ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		}
		ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
		const std::string infoTitle = IDX_TR("launcher.info.title") + k_InfoId;
		if (!ImGui::BeginPopupModal(infoTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			return;
		}

		if (!m_PendingInfoProject.has_value()) {
			// Nothing to show — close defensively so the modal can't get stuck.
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		const LauncherProjectEntry& entry = *m_PendingInfoProject;

		// Lazily look up the engine version from index-project.json. Cached per
		// path so re-renders don't re-parse the file every frame. IndexProject::Load
		// is only called on paths that pass Validate(), guarding against partially-
		// deleted projects whose entry still lingers in the registry.
		auto versionIt = m_EngineVersionCache.find(entry.Path);
		if (versionIt == m_EngineVersionCache.end()) {
			std::string version;
			if (IndexProject::Validate(entry.Path)) {
				auto project = IndexProject::Load(entry.Path);
				version = project.EngineVersion;
			}
			// Cache an empty sentinel rather than the translated "unknown"
			// string — that way hot-reloading the language picks up the new
			// translation on next frame without invalidating the cache.
			versionIt = m_EngineVersionCache.emplace(entry.Path, std::move(version)).first;
		}
		const std::string& engineVersionCached = versionIt->second;
		const std::string engineVersion = engineVersionCached.empty()
			? IDX_TR("launcher.info.unknown")
			: engineVersionCached;

		// Lazily look up the creation/modification timestamp of the project
		// directory. Reuses m_CreatedAtCache so this shares state with the
		// "Created" sort axis and stays consistent across the UI. Note that on
		// Windows std::filesystem::last_write_time is mtime, not true ctime —
		// matching what the existing "Created" sort axis surfaces.
		auto createdIt = m_CreatedAtCache.find(entry.Path);
		if (createdIt == m_CreatedAtCache.end()) {
			std::error_code ec;
			std::int64_t ts = 0;
			std::filesystem::path p(entry.Path);
			auto ftime = std::filesystem::last_write_time(p, ec);
			if (!ec) {
				ts = std::chrono::duration_cast<std::chrono::seconds>(
					ftime.time_since_epoch()).count();
			}
			createdIt = m_CreatedAtCache.emplace(entry.Path, ts).first;
		}
		const std::string createdStr = FormatLocalDateTime(createdIt->second);

		const std::string sizeStr = GetProjectSizeDisplayText(entry);
		const std::string relativeLastOpened = FormatRelativeTime(entry.LastOpened);

		// Header line: project name as the modal's title-ish content.
		ImGui::TextUnformatted(entry.Name.c_str());
		ImGui::Separator();
		ImGui::Spacing();

		// Two-column table: left column auto-sized to the longest label, right
		// column stretches to fill. Horizontal inner borders give visual
		// separation between rows without making it look like a heavy form.
		if (ImGui::BeginTable("##ProjectInfoTable", 2,
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH)) {
			ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

			auto labelValueRow = [](const char* label, const char* value) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(label);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextWrapped("%s", value && *value ? value : "—");
			};

			// Path row gets a special right side: read-only input for selecting
			// + copying, plus an Open button that hands off to the existing
			// "Open in Explorer" path. Sized so the button always fits.
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(IDX_TR("launcher.info.path").c_str());
			ImGui::TableSetColumnIndex(1);
			{
				static char s_PathBuffer[1024];
				std::snprintf(s_PathBuffer, sizeof(s_PathBuffer), "%s", entry.Path.c_str());
				const float buttonWidth = 70.0f;
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - 6.0f);
				ImGui::InputText("##InfoPath", s_PathBuffer, sizeof(s_PathBuffer),
					ImGuiInputTextFlags_ReadOnly);
				ImGui::SameLine();
				if (ImGui::Button(IDX_TR("launcher.info.open").c_str(), ImVec2(buttonWidth, 0))) {
					OpenProjectInExplorer(entry);
				}
			}

			labelValueRow(IDX_TR("launcher.info.created").c_str(), createdStr.c_str());

			// Last opened: show the relative form ("3h ago") plus the raw ISO
			// timestamp in parens so power users can see the exact moment.
			std::string lastOpenedCombined;
			if (!entry.LastOpened.empty()) {
				if (!relativeLastOpened.empty()) {
					lastOpenedCombined = relativeLastOpened + "  (" + entry.LastOpened + ")";
				} else {
					lastOpenedCombined = entry.LastOpened;
				}
			}
			labelValueRow(IDX_TR("launcher.info.last_opened").c_str(),
				lastOpenedCombined.empty() ? IDX_TR("launcher.info.never").c_str() : lastOpenedCombined.c_str());

			labelValueRow(IDX_TR("launcher.info.size").c_str(), sizeStr.c_str());
			labelValueRow(IDX_TR("launcher.info.engine_version").c_str(), engineVersion.c_str());

			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		if (ImGui::Button(IDX_TR("launcher.info.close").c_str(), ImVec2(100, 0))) {
			m_PendingInfoProject.reset();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void LauncherLayer::BrowseForDefaultProjectsLocation() {
#ifdef IDX_PLATFORM_WINDOWS
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

		IFileOpenDialog* dialog = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
			IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog));

		if (SUCCEEDED(hr)) {
			DWORD options;
			dialog->GetOptions(&options);
			dialog->SetOptions(options | FOS_PICKFOLDERS);
			dialog->SetTitle(L"Default projects location");

			if (SUCCEEDED(dialog->Show(nullptr))) {
				IShellItem* result = nullptr;
				dialog->GetResult(&result);
				if (result) {
					PWSTR widePath = nullptr;
					result->GetDisplayName(SIGDN_FILESYSPATH, &widePath);
					if (widePath) {
						const int len = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
						if (len > 1) {
							std::string path(static_cast<size_t>(len - 1), '\0');
							WideCharToMultiByte(CP_UTF8, 0, widePath, -1, path.data(), len, nullptr, nullptr);
							m_DefaultProjectsLocation = std::move(path);
						}
						CoTaskMemFree(widePath);
					}
					result->Release();
				}
			}
			dialog->Release();
		}

		CoUninitialize();
#endif
	}

}
