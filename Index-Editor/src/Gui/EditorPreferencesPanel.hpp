#pragma once
#include "Core/Export.hpp"

#include <string>

namespace Index {

	// Editor-wide preferences window. Opened from the menubar's
	// Edit -> Preferences... item. Backed by EditorPreferences for
	// persistence; this class is purely the ImGui rendering surface.
	//
	// Modeled on PackageManagerPanel / PrefabInspector for parity with
	// the other editor panels: explicit Initialize / Shutdown, single
	// Render call per frame controlled by an open-bool owned by the
	// caller.
	class EditorPreferencesPanel {
	public:
		void Initialize();
		void Shutdown();
		// `pOpen` mirrors the BeginPopupModal / Begin pattern used by the
		// other panels. The close-X button on the window's title bar
		// flips it false; the caller suppresses the call next frame.
		void Render(bool* pOpen);

	private:
		void RenderAppearanceTab();
		void RenderScriptingTab();
		void RenderLayoutsTab();
		void RenderBehaviorTab();

		void RenderLayoutModals();

		// Layout preset modal state. Lives on the panel (not as TU-static)
		// so the modals can OpenPopup at the same id-stack scope as the
		// matching BeginPopupModal — see the matching comment that was
		// in RenderMainMenu before the controls migrated here.
		char m_SaveLayoutBuffer[64]{};
		bool m_OpenSaveLayoutRequest = false;
		std::string m_PendingDeleteLayoutName;
		bool m_OpenDeleteLayoutRequest = false;

		// True the frame the panel becomes visible. Used to refresh
		// detection of external script editors so plugging in a new IDE
		// without restarting the editor still surfaces it.
		bool m_WasOpenLastFrame = false;
	};

}
