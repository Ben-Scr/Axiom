#include "EditorPreferencesPanel.hpp"

#include "Assets/AssetKind.hpp"
#include "Editor/EditorPreferences.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Editor/ExternalEditorInfo.hpp"
#include "Graphics/Text/FontHandle.hpp"
#include "Gui/ImGuiContextLayer.hpp"
#include "Gui/ImGuiFonts.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Inspector/ReferencePicker.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace Index {
	namespace {
		constexpr const char* k_FontPickerKey = "EditorPrefs.EditorFont";

		uint64_t ParseUInt64String(const std::string& value) {
			if (value.empty()) return 0;
			try {
				size_t parsed = 0;
				uint64_t id = std::stoull(value, &parsed, 10);
				return parsed == value.size() ? id : 0;
			} catch (...) {
				return 0;
			}
		}

		const char* ThemeModeLabel(EditorThemeMode mode) {
			switch (mode) {
				case EditorThemeMode::Dark:          return "Dark";
				case EditorThemeMode::Light:         return "Light";
				case EditorThemeMode::SystemDefault: return "System Default";
				case EditorThemeMode::Custom:        return "Custom";
			}
			return "Dark";
		}
	}

	void EditorPreferencesPanel::Initialize() {
		// Reserved for future per-panel state setup. Detection of script
		// editors is on-demand in RenderScriptingTab so a slow first-time
		// probe doesn't run at editor startup.
	}

	void EditorPreferencesPanel::Shutdown() {
		// No persistent allocations to drop.
	}

	void EditorPreferencesPanel::Render(bool* pOpen) {
		if (!pOpen || !*pOpen) {
			m_WasOpenLastFrame = false;
			return;
		}

		// First-frame open hook: refresh editor detection so the user gets
		// a current list every time they open the panel without having to
		// restart the editor after installing a new IDE.
		if (!m_WasOpenLastFrame) {
			ExternalEditor::DetectEditors();
		}
		m_WasOpenLastFrame = true;

		ImGui::SetNextWindowSize(ImVec2(640.0f, 520.0f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Editor Preferences", pOpen)) {
			ImGui::End();
			return;
		}

		if (ImGui::BeginTabBar("##EditorPrefsTabs", ImGuiTabBarFlags_None)) {
			if (ImGui::BeginTabItem("Appearance")) {
				RenderAppearanceTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Scripting")) {
				RenderScriptingTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Layouts")) {
				RenderLayoutsTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Behavior")) {
				RenderBehaviorTab();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		// Reference picker popup for the editor-font selector. Must run
		// inside the same ImGui::Begin scope as the button that opens it.
		ReferencePicker::RenderPopup();

		// Layout modals are owned by this panel (not the menubar) so the
		// OpenPopup / BeginPopupModal id-stack match. Render them at the
		// panel's Begin/End scope, after the tab bar.
		RenderLayoutModals();

		ImGui::End();
	}

	void EditorPreferencesPanel::RenderAppearanceTab() {
		// ── Theme ───────────────────────────────────────────────────
		ImGui::TextUnformatted("Theme");
		ImGui::Separator();

		EditorThemeMode currentMode = EditorPreferences::GetThemeMode();
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::BeginCombo("##EditorThemeCombo", ThemeModeLabel(currentMode))) {
			constexpr EditorThemeMode kModes[] = {
				EditorThemeMode::Dark,
				EditorThemeMode::Light,
				EditorThemeMode::SystemDefault,
				EditorThemeMode::Custom,
			};
			for (EditorThemeMode mode : kModes) {
				const bool selected = (mode == currentMode);
				if (ImGui::Selectable(ThemeModeLabel(mode), selected)) {
					EditorPreferences::SetThemeMode(mode);
					currentMode = mode;
				}
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Dark: Index's dark palette + sizing.\n"
				"Light: stock ImGui light palette.\n"
				"System Default: follow the Windows app theme.\n"
				"Custom: enable the per-color editors below.");
		}

		ImGui::Spacing();

		// ── Custom Colors ──────────────────────────────────────────
		const bool customActive = (currentMode == EditorThemeMode::Custom);
		ImGui::BeginDisabled(!customActive);

		ImGui::TextUnformatted("Custom Colors");
		ImGui::SameLine();
		if (ImGui::Button("Reset to Current##ResetCustomColors")) {
			EditorPreferences::ResetCustomColorsFromCurrent();
			// Re-apply so the swatches snap back to the just-seeded values.
			EditorPreferences::ApplyTheme();
		}
		if (ImGui::IsItemHovered() && customActive) {
			ImGui::SetTooltip(
				"Reseed the editable swatches from the colors currently\n"
				"on screen. Useful after experimenting with Dark/Light\n"
				"and wanting to start customising from that base.");
		}

		// Scrollable region so the long ImGuiCol_ list doesn't push the
		// rest of the tab off-screen on smaller windows.
		ImGui::BeginChild("##CustomColorsScroll",
			ImVec2(0, 280.0f),
			ImGuiChildFlags_Borders,
			ImGuiWindowFlags_None);

		for (int i = 0; i < ImGuiCol_COUNT; i++) {
			const char* name = ImGui::GetStyleColorName(i);
			if (!name) continue;
			ImVec4& color = EditorPreferences::CustomColor(i);
			ImGui::PushID(i);
			if (ImGui::ColorEdit4(name, &color.x,
					ImGuiColorEditFlags_AlphaBar |
					ImGuiColorEditFlags_NoInputs)) {
				if (customActive) {
					// Push directly into the live style so the change is
					// visible mid-frame, and persist so it survives restart.
					ImGui::GetStyle().Colors[i] = color;
					EditorPreferences::Save();
				}
			}
			ImGui::PopID();
		}

		ImGui::EndChild();
		ImGui::EndDisabled();

		ImGui::Spacing();

		// ── Editor Font ────────────────────────────────────────────
		ImGui::TextUnformatted("Editor Font");
		ImGui::Separator();

		// Consume any selection raised on the previous frame.
		if (auto pending = ReferencePicker::ConsumeSelection(k_FontPickerKey); pending) {
			const uint64_t picked = ParseUInt64String(*pending);
			EditorPreferences::SetEditorFontAssetId(picked != 0 ? picked : k_DefaultFontAssetId);
		}

		bool fontMissing = false;
		std::string fontSecondary;
		const uint64_t fontId = EditorPreferences::GetEditorFontAssetId();
		const std::string fontDisplay = ReferencePicker::ResolveAssetDisplay(
			fontId, AssetKind::Font, fontMissing, &fontSecondary);

		ImGui::PushID("EditorFontPicker");
		if (ImGui::Button(fontDisplay.c_str(), ImVec2(280.0f, 0.0f))) {
			ReferencePicker::OpenForFieldKey(k_FontPickerKey, "Select Editor Font",
				ReferencePicker::CollectAssetsByKind(AssetKind::Font),
				ReferencePicker::Style::Plain);
		}
		if (ImGui::IsItemHovered() && !fontSecondary.empty()) {
			ImGui::SetTooltip("%s", fontSecondary.c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset##EditorFontReset")) {
			EditorPreferences::SetEditorFontAssetId(k_DefaultFontAssetId);
		}
		ImGui::PopID();
		ImGui::TextDisabled("Takes effect after restarting the editor.");

		ImGui::Spacing();

		// Font size — applies live (deferred atlas rebuild on the next
		// OnPreRender). No restart caveat.
		ImGui::PushID("EditorFontSize");
		int fontSize = static_cast<int>(EditorPreferences::GetEditorFontSize() + 0.5f);
		ImGui::SetNextItemWidth(280.0f);
		if (ImGui::SliderInt("Font Size",
				&fontSize,
				static_cast<int>(EditorPreferences::k_MinEditorFontSize),
				static_cast<int>(EditorPreferences::k_MaxEditorFontSize),
				"%d px")) {
			EditorPreferences::SetEditorFontSize(static_cast<float>(fontSize));
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset##EditorFontSizeReset")) {
			EditorPreferences::SetEditorFontSize(k_IndexImGuiFontSize);
		}
		ImGui::PopID();
	}

	void EditorPreferencesPanel::RenderScriptingTab() {
		ImGui::TextUnformatted("External Script Editor");
		ImGui::Separator();

		const auto& editors = ExternalEditor::GetAvailableEditors();
		if (editors.empty()) {
			ImGui::TextDisabled("No supported editors detected on this system.");
			if (ImGui::Button("Re-detect")) {
				ExternalEditor::DetectEditors();
			}
			return;
		}

		const int selected = ExternalEditor::GetSelectedIndex();
		const char* preview = (selected >= 0 && selected < static_cast<int>(editors.size()))
			? editors[selected].DisplayName.c_str()
			: "(none)";

		ImGui::SetNextItemWidth(320.0f);
		if (ImGui::BeginCombo("##ExternalEditorCombo", preview)) {
			for (int i = 0; i < static_cast<int>(editors.size()); i++) {
				const bool isSelected = (i == selected);
				if (ImGui::Selectable(editors[i].DisplayName.c_str(), isSelected)) {
					ExternalEditor::SetSelectedIndex(i);
				}
				if (isSelected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button("Re-detect##ScriptEditorRedetect")) {
			ExternalEditor::DetectEditors();
		}
		ImGui::TextDisabled("The editor used when opening .cs / .cpp / .hpp script files.");
	}

	void EditorPreferencesPanel::RenderLayoutsTab() {
		ImGui::TextUnformatted("Layout Presets");
		ImGui::Separator();

		if (ImGui::Button("Save Current Layout As...")) {
			m_SaveLayoutBuffer[0] = '\0';
			m_OpenSaveLayoutRequest = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset to Default")) {
			ImGuiContextLayer::ResetLayoutToBundledDefault();
		}
		ImGui::TextDisabled("Stored under %%LOCALAPPDATA%%\\Index\\Editor\\Layouts.");

		ImGui::Spacing();

		const std::vector<std::string> presets =
			ImGuiContextLayer::ListLayoutPresets();
		if (presets.empty()) {
			ImGui::TextDisabled("No saved layout presets.");
			return;
		}

		if (ImGui::BeginTable("##LayoutPresetsTable", 3,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##Load", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("##Delete", ImGuiTableColumnFlags_WidthFixed, 80.0f);

			for (const std::string& name : presets) {
				ImGui::PushID(name.c_str());
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(name.c_str());

				ImGui::TableSetColumnIndex(1);
				if (ImGui::Button("Load", ImVec2(-FLT_MIN, 0))) {
					ImGuiContextLayer::LoadLayoutPreset(name);
				}

				ImGui::TableSetColumnIndex(2);
				if (ImGui::Button("Delete", ImVec2(-FLT_MIN, 0))) {
					m_PendingDeleteLayoutName = name;
					m_OpenDeleteLayoutRequest = true;
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	void EditorPreferencesPanel::RenderBehaviorTab() {
		// ── Asset Browser ─────────────────────────────────────────
		ImGui::TextUnformatted("Asset Browser");
		ImGui::Separator();

		bool showExt = EditorPreferences::GetShowFileExtensions();
		if (ImGui::Checkbox("Show file extensions", &showExt)) {
			EditorPreferences::SetShowFileExtensions(showExt);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"When off (default), asset names render without their\n"
				"extension (\"MyScene\" instead of \"MyScene.scene\").\n"
				"Renaming pre-fills only the stem and re-appends the\n"
				"original extension on commit. When on, extensions are\n"
				"shown and editable verbatim.");
		}

		ImGui::Spacing();

		// ── Auto-Save ──────────────────────────────────────────────
		ImGui::TextUnformatted("Auto-Save");
		ImGui::Separator();

		bool autoSave = EditorPreferences::GetAutoSaveScenes();
		if (ImGui::Checkbox("Auto-save scenes", &autoSave)) {
			EditorPreferences::SetAutoSaveScenes(autoSave);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Periodically save the active scene while editing.\n"
				"Skipped during Play mode (Play-mode edits are discarded\n"
				"on Stop, so saving them would clobber the pre-Play snapshot).");
		}

		if (autoSave) {
			float interval = EditorPreferences::GetAutoSaveIntervalSeconds();
			ImGui::SetNextItemWidth(160.0f);
			if (ImGui::InputFloat("Interval (seconds)", &interval, 5.0f, 30.0f, "%.0f")) {
				EditorPreferences::SetAutoSaveIntervalSeconds(interval);
			}
		}

		ImGui::Spacing();

		bool autoSavePrefabs = EditorPreferences::GetAutoSavePrefabs();
		if (ImGui::Checkbox("Auto-save prefabs", &autoSavePrefabs)) {
			EditorPreferences::SetAutoSavePrefabs(autoSavePrefabs);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"In prefab edit mode, save the prefab as soon as you release\n"
				"the active inspector / hierarchy widget — no Ctrl+S needed.\n"
				"Drags and text-input debounce naturally; the save fires once\n"
				"per edit on release. Skipped during Play mode.");
		}
	}

	void EditorPreferencesPanel::RenderLayoutModals() {
		// OpenPopup must run at the same id-stack scope as BeginPopupModal,
		// so emit it here (panel scope) right before the matching Begin.
		if (m_OpenSaveLayoutRequest) {
			ImGui::OpenPopup("Save Layout As");
			m_OpenSaveLayoutRequest = false;
		}
		if (m_OpenDeleteLayoutRequest) {
			ImGui::OpenPopup("Delete Layout");
			m_OpenDeleteLayoutRequest = false;
		}

		ImGuiUtils::CenterNextModal();
		if (ImGui::BeginPopupModal("Save Layout As", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::TextUnformatted("Save current editor layout as preset:");
			ImGui::Spacing();
			ImGui::SetNextItemWidth(320);
			if (ImGui::IsWindowAppearing()) {
				ImGui::SetKeyboardFocusHere();
			}
			const bool enterPressed = ImGui::InputText("##LayoutName",
				m_SaveLayoutBuffer, sizeof(m_SaveLayoutBuffer),
				ImGuiInputTextFlags_EnterReturnsTrue);

			const std::string nameStr(m_SaveLayoutBuffer);
			const bool nameValid = ImGuiContextLayer::IsValidLayoutPresetName(nameStr);
			if (!nameStr.empty() && !nameValid) {
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
					"Invalid name (no / \\ : * ? \" < > |, no trailing dot/space, max 64 chars).");
			} else {
				ImGui::TextDisabled("Stored under %%LOCALAPPDATA%%\\Index\\Editor\\Layouts.");
			}

			ImGui::Spacing();
			ImGui::BeginDisabled(!nameValid);
			const bool saveClicked = ImGui::Button("Save", ImVec2(100, 0));
			ImGui::EndDisabled();
			if ((saveClicked || enterPressed) && nameValid) {
				ImGuiContextLayer::SaveLayoutPreset(nameStr);
				m_SaveLayoutBuffer[0] = '\0';
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0)) ||
				ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				m_SaveLayoutBuffer[0] = '\0';
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		ImGuiUtils::CenterNextModal();
		if (ImGui::BeginPopupModal("Delete Layout", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::Text("Delete layout preset '%s'?", m_PendingDeleteLayoutName.c_str());
			ImGui::TextDisabled("This cannot be undone.");
			ImGui::Spacing();
			if (ImGui::Button("Delete", ImVec2(100, 0))) {
				ImGuiContextLayer::DeleteLayoutPreset(m_PendingDeleteLayoutName);
				m_PendingDeleteLayoutName.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(100, 0)) ||
				ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				m_PendingDeleteLayoutName.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

}
