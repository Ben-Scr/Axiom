#pragma once
#include "Project/IndexBuildProfile.hpp"

#include <string>
#include <vector>

namespace Index {

	// Editor panel for managing `.indexbuild` files under
	// <ProjectRoot>/BuildProfiles/. Modeled on PackageManagerPanel's
	// list-left / inspector-right layout. Synchronous (no worker threads):
	// profile edits are tiny JSON writes so the disk roundtrip stays well
	// inside one frame.
	class BuildProfilesPanel {
	public:
		void Initialize();
		void Shutdown();
		// `pOpen` flips false when the user hits the title bar X.
		void Render(bool* pOpen);

	private:
		void RefreshProfilesIfDirty();
		void RescanFromDisk();
		void RenderProfileList();
		void RenderInspector();

		bool CreateProfile(const std::string& name);
		bool DuplicateSelected();
		bool DeleteSelected();
		bool RenameSelected(const std::string& newName);
		void SaveSelected();

		std::string BuildFilePath(const std::string& profileName) const;
		bool IsValidProfileName(const std::string& name) const;

		std::vector<IndexBuildProfile> m_Profiles;
		int  m_SelectedIndex = -1;
		bool m_Dirty = true;

		// Name-entry buffer for the "New Profile" inline form.
		char m_NewProfileNameBuffer[128]{};
		// Rename buffer — populated from the selected profile each frame
		// the user opens the rename popup.
		char m_RenameBuffer[128]{};
		bool m_OpenRenamePopup = false;

		// Status strip at the bottom of the panel.
		std::string m_StatusMessage;
		bool m_StatusIsError = false;
	};

} // namespace Index
