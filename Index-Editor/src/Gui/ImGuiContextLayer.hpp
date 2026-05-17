#pragma once

#include "Core/Layer.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace Index {

	class IndexEvent;

	// Owns the ImGui context for an Index application. Push this Layer first (before
	// any other ImGui-using Layer) so its OnPreRender / OnPostRender wrap the per-frame
	// NewFrame / Render calls around all other Layers' UI work.
	class ImGuiContextLayer : public Layer {
	public:
		using Layer::Layer;

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnEvent(Application& app, IndexEvent& event) override;
		void OnPreRender(Application& app) override;
		void OnPostRender(Application& app) override;

		// Re-loads the bundled (checked-in) imgui.ini default into the
		// active context and persists the result to the user's
		// %LOCALAPPDATA% file, so the next launch starts from the same
		// reset state rather than re-saving the pre-reset layout. Called
		// from the editor's Application → Reset Layout menu item; safe to
		// call from any frame inside the editor's render scope.
		static void ResetLayoutToBundledDefault();

		// Layout preset API — user-named .ini snapshots stored under
		// %LOCALAPPDATA%\Index\Editor\Layouts\<name>.ini. Each preset
		// is a stand-alone imgui.ini file produced by ImGui's normal
		// save path, so swapping presets is just a Clear + Load + Save-
		// to-user-ini sequence (same shape as ResetLayoutToBundledDefault).
		//
		// All four are safe to call from any frame inside the editor's
		// render scope. Errors are logged via IDX_CORE_*_TAG("ImGui",
		// ...) and surfaced through the bool return.
		static std::vector<std::string> ListLayoutPresets();
		static bool SaveLayoutPreset(const std::string& name);
		static bool LoadLayoutPreset(const std::string& name);
		static bool DeleteLayoutPreset(const std::string& name);
		// True iff `name` is a non-empty, filesystem-safe preset name
		// (no path separators, no Windows-reserved chars, not "." / "..",
		// length ≤ 64). Used by the Save-As popup to gate the OK button.
		static bool IsValidLayoutPresetName(const std::string& name);

		// Index brand palette + sizing/rounding. Public so the editor
		// preferences theme switcher can re-apply it after picking a base
		// (Dark) palette, instead of every theme path having to know the
		// Index colour table. Idempotent.
		static void ApplyIndexTheme();
		// Just the Index colour palette (no sizing/rounding writes).
		// EditorPreferences::ApplyTheme uses this for Dark so a runtime
		// theme switch doesn't overwrite the DPI-scaled sizing values
		// that ImGuiContextLayer::OnAttach finalised post-ScaleAllSizes.
		static void ApplyIndexThemeColors();

	private:
		// Belt-and-suspenders save. ImGui auto-saves internally on its
		// own dirty-timer schedule, but specific change types (mid-
		// drag dock split ratios, certain window-close paths, late-
		// frame layout mutations) sometimes don't flip the dirty flag
		// and can be lost on a hard quit. We force a write whenever
		// `WantSaveIniSettings` flips OR every k_PeriodicSaveSeconds
		// regardless. The redundant writes are cheap (small ini, same
		// bytes when unchanged).
		void FlushSettingsIfDirtyOrPeriodic();

		std::string m_IniFilePath;
		bool m_IsInitialized = false;
		std::chrono::steady_clock::time_point m_LastSaveTime{};
	};

}
