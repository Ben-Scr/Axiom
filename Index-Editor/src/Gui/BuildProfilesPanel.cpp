#include <pch.hpp>
#include "Gui/BuildProfilesPanel.hpp"

#include "Core/Log.hpp"
#include "Project/BuildPlatformSupport.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace Index {

	namespace {
		// Profile filenames double as identifiers (IndexProject::ActiveBuildProfileName).
		// Restrict to characters that survive cleanly on Windows + Linux filesystems
		// and never need escaping in JSON. Same conservative ruleset as
		// IndexProject::IsValidProjectName, slightly relaxed (dot allowed).
		bool IsAcceptableNameChar(char c) {
			if (c >= 'a' && c <= 'z') return true;
			if (c >= 'A' && c <= 'Z') return true;
			if (c >= '0' && c <= '9') return true;
			return c == '_' || c == '-' || c == ' ' || c == '.';
		}

		std::string TrimCopy(std::string value) {
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
			std::size_t first = 0;
			while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
			value.erase(0, first);
			return value;
		}

		const char* RenderBackendLabel(IndexProject::RenderBackend backend) {
			return IndexProject::RenderBackendToString(backend);
		}
	}

	void BuildProfilesPanel::Initialize() {
		m_Dirty = true;
	}

	void BuildProfilesPanel::Shutdown() {
		m_Profiles.clear();
		m_SelectedIndex = -1;
		m_StatusMessage.clear();
		m_StatusIsError = false;
	}

	bool BuildProfilesPanel::IsValidProfileName(const std::string& name) const {
		if (name.empty() || name.size() > 64) return false;
		// No leading/trailing space — would round-trip surprisingly through the
		// file stem. Internal spaces are fine.
		if (std::isspace(static_cast<unsigned char>(name.front()))) return false;
		if (std::isspace(static_cast<unsigned char>(name.back())))  return false;
		for (char c : name) {
			if (!IsAcceptableNameChar(c)) return false;
		}
		return true;
	}

	std::string BuildProfilesPanel::BuildFilePath(const std::string& profileName) const {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) return {};
		const std::string dir = project->GetBuildProfilesDirectory();
		if (dir.empty()) return {};
		return (std::filesystem::path(dir) /
			(profileName + std::string(IndexBuildProfile::FileExtension))).string();
	}

	void BuildProfilesPanel::RefreshProfilesIfDirty() {
		if (!m_Dirty) return;
		RescanFromDisk();
		m_Dirty = false;
	}

	void BuildProfilesPanel::RescanFromDisk() {
		const std::string previouslySelected = (m_SelectedIndex >= 0
			&& m_SelectedIndex < static_cast<int>(m_Profiles.size()))
			? m_Profiles[m_SelectedIndex].Name
			: std::string{};

		m_Profiles.clear();
		m_SelectedIndex = -1;

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) return;
		const std::string dir = project->GetBuildProfilesDirectory();
		if (dir.empty() || !std::filesystem::exists(dir)) return;

		const std::string ext(IndexBuildProfile::FileExtension);
		std::error_code ec;
		for (const auto& entry :
			 std::filesystem::directory_iterator(dir,
				 std::filesystem::directory_options::skip_permission_denied, ec)) {
			if (ec) { ec.clear(); continue; }
			if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }
			const auto path = entry.path();
			if (path.extension().string() != ext) continue;
			m_Profiles.push_back(IndexBuildProfile::Load(path.string()));
		}

		std::sort(m_Profiles.begin(), m_Profiles.end(),
			[](const IndexBuildProfile& a, const IndexBuildProfile& b) { return a.Name < b.Name; });

		if (!previouslySelected.empty()) {
			for (int i = 0; i < static_cast<int>(m_Profiles.size()); ++i) {
				if (m_Profiles[i].Name == previouslySelected) {
					m_SelectedIndex = i;
					break;
				}
			}
		}
		if (m_SelectedIndex < 0 && !m_Profiles.empty()) {
			m_SelectedIndex = 0;
		}
	}

	bool BuildProfilesPanel::CreateProfile(const std::string& rawName) {
		const std::string name = TrimCopy(rawName);
		if (!IsValidProfileName(name)) {
			m_StatusMessage = "Invalid profile name. Use letters, digits, spaces, '_', '-', or '.'.";
			m_StatusIsError = true;
			return false;
		}
		// Reject collision with an existing profile (case-insensitive on
		// Windows to match the filesystem's behaviour).
		for (const auto& existing : m_Profiles) {
			if (existing.Name.size() == name.size() &&
				std::equal(existing.Name.begin(), existing.Name.end(), name.begin(),
					[](char a, char b) {
						return std::tolower(static_cast<unsigned char>(a))
							== std::tolower(static_cast<unsigned char>(b));
					})) {
				m_StatusMessage = "A profile with that name already exists.";
				m_StatusIsError = true;
				return false;
			}
		}

		IndexBuildProfile profile;
		profile.Name = name;
		profile.Platform = BuildPlatform::Windows;
		profile.RenderBackend = IndexProject::RenderBackend::Direct3D12;

		const std::string path = BuildFilePath(name);
		if (path.empty() || !profile.Save(path)) {
			m_StatusMessage = "Failed to write profile to disk.";
			m_StatusIsError = true;
			return false;
		}

		m_StatusMessage = "Created profile '" + name + "'.";
		m_StatusIsError = false;
		m_Dirty = true;
		return true;
	}

	bool BuildProfilesPanel::DuplicateSelected() {
		if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Profiles.size())) return false;
		const IndexBuildProfile& src = m_Profiles[m_SelectedIndex];

		std::string candidate = src.Name + " Copy";
		int suffix = 2;
		while (true) {
			bool collision = false;
			for (const auto& existing : m_Profiles) {
				if (existing.Name == candidate) { collision = true; break; }
			}
			if (!collision) break;
			candidate = src.Name + " Copy " + std::to_string(suffix++);
		}

		IndexBuildProfile clone = src;
		clone.Name = candidate;
		const std::string path = BuildFilePath(candidate);
		if (path.empty() || !clone.Save(path)) {
			m_StatusMessage = "Failed to write duplicated profile.";
			m_StatusIsError = true;
			return false;
		}
		m_StatusMessage = "Duplicated as '" + candidate + "'.";
		m_StatusIsError = false;
		m_Dirty = true;
		return true;
	}

	bool BuildProfilesPanel::DeleteSelected() {
		if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Profiles.size())) return false;
		const std::string name = m_Profiles[m_SelectedIndex].Name;
		const std::string path = BuildFilePath(name);
		std::error_code ec;
		std::filesystem::remove(path, ec);
		if (ec) {
			m_StatusMessage = "Failed to delete profile file: " + ec.message();
			m_StatusIsError = true;
			return false;
		}

		// Clear ActiveBuildProfileName if the deleted profile was the active one.
		if (IndexProject* project = ProjectManager::GetCurrentProject()) {
			if (project->ActiveBuildProfileName == name) {
				project->ActiveBuildProfileName.clear();
				project->Save();
			}
		}

		m_StatusMessage = "Deleted profile '" + name + "'.";
		m_StatusIsError = false;
		m_Dirty = true;
		return true;
	}

	bool BuildProfilesPanel::RenameSelected(const std::string& rawNewName) {
		if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Profiles.size())) return false;
		const std::string newName = TrimCopy(rawNewName);
		IndexBuildProfile& profile = m_Profiles[m_SelectedIndex];
		if (newName == profile.Name) return true; // no-op
		if (!IsValidProfileName(newName)) {
			m_StatusMessage = "Invalid profile name.";
			m_StatusIsError = true;
			return false;
		}
		for (const auto& existing : m_Profiles) {
			if (&existing == &profile) continue;
			if (existing.Name == newName) {
				m_StatusMessage = "A profile with that name already exists.";
				m_StatusIsError = true;
				return false;
			}
		}

		const std::string oldPath = BuildFilePath(profile.Name);
		const std::string newPath = BuildFilePath(newName);
		std::error_code ec;
		std::filesystem::rename(oldPath, newPath, ec);
		if (ec) {
			m_StatusMessage = "Failed to rename profile file: " + ec.message();
			m_StatusIsError = true;
			return false;
		}

		// If the renamed profile was active, keep it active under the new name.
		if (IndexProject* project = ProjectManager::GetCurrentProject()) {
			if (project->ActiveBuildProfileName == profile.Name) {
				project->ActiveBuildProfileName = newName;
				project->Save();
			}
		}

		profile.Name = newName;
		// Persist Name field inside the JSON too (Load uses the stem, but the
		// in-file Name keeps the two consistent for hand-inspection).
		profile.Save(newPath);
		m_StatusMessage = "Renamed to '" + newName + "'.";
		m_StatusIsError = false;
		return true;
	}

	void BuildProfilesPanel::SaveSelected() {
		if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Profiles.size())) return;
		const IndexBuildProfile& profile = m_Profiles[m_SelectedIndex];
		const std::string path = BuildFilePath(profile.Name);
		if (path.empty()) return;
		if (!profile.Save(path)) {
			m_StatusMessage = "Failed to save profile changes.";
			m_StatusIsError = true;
			IDX_WARN_TAG("Build", "Failed to write build profile '{}'.", path);
		}
	}

	void BuildProfilesPanel::RenderProfileList() {
		ImGui::TextUnformatted("Profiles");
		ImGui::Separator();

		const float availY = ImGui::GetContentRegionAvail().y;
		// Leave room for the inline new-profile form + buttons below.
		const float listHeight = std::max(120.0f, availY - 130.0f);

		ImGui::BeginChild("##ProfileList", ImVec2(0, listHeight), true);
		for (int i = 0; i < static_cast<int>(m_Profiles.size()); ++i) {
			const IndexBuildProfile& p = m_Profiles[i];
			const bool isActive = [&]() {
				IndexProject* project = ProjectManager::GetCurrentProject();
				return project && project->ActiveBuildProfileName == p.Name;
			}();
			ImGui::PushID(i);
			std::string label = p.Name;
			if (isActive) label += "  [active]";
			if (ImGui::Selectable(label.c_str(), m_SelectedIndex == i)) {
				m_SelectedIndex = i;
			}
			ImGui::PopID();
		}
		if (m_Profiles.empty()) {
			ImGui::TextDisabled("(no profiles yet — add one below)");
		}
		ImGui::EndChild();

		ImGui::Spacing();

		const bool hasSelection = (m_SelectedIndex >= 0 && m_SelectedIndex < static_cast<int>(m_Profiles.size()));
		if (!hasSelection) ImGui::BeginDisabled();
		if (ImGui::Button("Set Active")) {
			if (IndexProject* project = ProjectManager::GetCurrentProject()) {
				project->ActiveBuildProfileName = m_Profiles[m_SelectedIndex].Name;
				project->Save();
				m_StatusMessage = "Active profile: '" + project->ActiveBuildProfileName + "'.";
				m_StatusIsError = false;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Duplicate")) DuplicateSelected();
		ImGui::SameLine();
		if (ImGui::Button("Rename")) {
			const std::string& current = m_Profiles[m_SelectedIndex].Name;
			std::strncpy(m_RenameBuffer, current.c_str(), sizeof(m_RenameBuffer) - 1);
			m_RenameBuffer[sizeof(m_RenameBuffer) - 1] = '\0';
			m_OpenRenamePopup = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Delete")) ImGui::OpenPopup("##ConfirmDeleteProfile");
		if (!hasSelection) ImGui::EndDisabled();

		if (ImGui::BeginPopupModal("##ConfirmDeleteProfile", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text("Delete profile '%s'?",
				hasSelection ? m_Profiles[m_SelectedIndex].Name.c_str() : "(none)");
			ImGui::Text("This removes the .indexbuild file from disk.");
			ImGui::Spacing();
			if (ImGui::Button("Delete", ImVec2(120, 0))) {
				DeleteSelected();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (m_OpenRenamePopup) {
			ImGui::OpenPopup("##RenameProfile");
			m_OpenRenamePopup = false;
		}
		if (ImGui::BeginPopupModal("##RenameProfile", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text("New name:");
			ImGui::SetNextItemWidth(260.0f);
			const bool entered = ImGui::InputText("##RenameInput",
				m_RenameBuffer, sizeof(m_RenameBuffer),
				ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::Spacing();
			if (ImGui::Button("Rename", ImVec2(120, 0)) || entered) {
				if (RenameSelected(m_RenameBuffer)) ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		ImGui::Spacing();
		ImGui::TextUnformatted("Add new profile:");
		ImGui::SetNextItemWidth(-1);
		const bool addEnter = ImGui::InputTextWithHint("##NewProfileName",
			"Profile name…",
			m_NewProfileNameBuffer, sizeof(m_NewProfileNameBuffer),
			ImGuiInputTextFlags_EnterReturnsTrue);
		if ((ImGui::Button("Add Profile") || addEnter)
			&& m_NewProfileNameBuffer[0] != '\0') {
			if (CreateProfile(m_NewProfileNameBuffer)) {
				m_NewProfileNameBuffer[0] = '\0';
			}
		}
	}

	void BuildProfilesPanel::RenderInspector() {
		ImGui::TextUnformatted("Inspector");
		ImGui::Separator();

		if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Profiles.size())) {
			ImGui::TextDisabled("Select a profile on the left to edit it.");
			return;
		}
		IndexBuildProfile& profile = m_Profiles[m_SelectedIndex];

		ImGui::Text("Name: %s", profile.Name.c_str());
		ImGui::Spacing();

		// Platform combo — drives the available backend list.
		const BuildPlatform platforms[] = { BuildPlatform::Windows, BuildPlatform::Linux };
		int platformIdx = (profile.Platform == BuildPlatform::Linux) ? 1 : 0;
		ImGui::TextUnformatted("Target Platform:");
		ImGui::SetNextItemWidth(-1);
		std::string platformPreview = IndexBuildProfile::PlatformToString(platforms[platformIdx]);
		if (ImGui::BeginCombo("##Platform", platformPreview.c_str())) {
			for (int i = 0; i < IM_ARRAYSIZE(platforms); ++i) {
				const bool selected = (platformIdx == i);
				const char* label = IndexBuildProfile::PlatformToString(platforms[i]);
				std::string itemLabel = label;
				if (!BuildPlatformSupport::IsAvailable(platforms[i])) {
					itemLabel += "  (not installed)";
				}
				if (ImGui::Selectable(itemLabel.c_str(), selected)) {
					if (profile.Platform != platforms[i]) {
						profile.Platform = platforms[i];
						// Coerce backend into the new platform's allowed set.
						if (!IndexBuildProfile::IsBackendAllowed(profile.Platform, profile.RenderBackend)) {
							const auto allowed = IndexBuildProfile::AllowedBackends(profile.Platform);
							if (!allowed.empty()) profile.RenderBackend = allowed.front();
						}
						SaveSelected();
					}
				}
			}
			ImGui::EndCombo();
		}

		ImGui::Spacing();

		// Render backend — filtered by the current platform.
		const auto allowed = IndexBuildProfile::AllowedBackends(profile.Platform);
		ImGui::TextUnformatted("Rendering API:");
		ImGui::SetNextItemWidth(-1);
		if (ImGui::BeginCombo("##RenderBackend", RenderBackendLabel(profile.RenderBackend))) {
			for (IndexProject::RenderBackend backend : allowed) {
				const bool selected = (profile.RenderBackend == backend);
				if (ImGui::Selectable(RenderBackendLabel(backend), selected)) {
					if (profile.RenderBackend != backend) {
						profile.RenderBackend = backend;
						SaveSelected();
					}
				}
			}
			ImGui::EndCombo();
		}

		ImGui::Spacing();

		// Availability warning row — placeholder for the future package check.
		if (!BuildPlatformSupport::IsAvailable(profile.Platform)) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
			ImGui::TextWrapped("[!] %s", BuildPlatformSupport::UnavailableReason(profile.Platform).c_str());
			ImGui::PopStyleColor();
		}
	}

	void BuildProfilesPanel::Render(bool* pOpen) {
		if (!pOpen || !*pOpen) return;

		ImGui::SetNextWindowSize(ImVec2(720, 420), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Build Profiles", pOpen)) {
			ImGui::End();
			return;
		}

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			ImGui::TextDisabled("Open a project to manage build profiles.");
			ImGui::End();
			return;
		}

		RefreshProfilesIfDirty();

		const float listColumnWidth = ImGui::GetContentRegionAvail().x * 0.4f;
		ImGui::BeginChild("##ProfileListCol", ImVec2(listColumnWidth, 0));
		RenderProfileList();
		ImGui::EndChild();

		ImGui::SameLine();

		ImGui::BeginChild("##InspectorCol", ImVec2(0, 0));
		RenderInspector();
		ImGui::EndChild();

		if (!m_StatusMessage.empty()) {
			ImGui::Separator();
			if (m_StatusIsError) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
			ImGui::TextWrapped("%s", m_StatusMessage.c_str());
			if (m_StatusIsError) ImGui::PopStyleColor();
		}

		ImGui::End();
	}

} // namespace Index
