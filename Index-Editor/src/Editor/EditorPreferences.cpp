#include "Editor/EditorPreferences.hpp"

#include "Core/Log.hpp"
#include "Gui/ImGuiContextLayer.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/SpecialFolder.hpp"

#include <imgui.h>

#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#endif

namespace Index {
	namespace {
		// All state lives in the .cpp so the header stays light. The
		// class is a static facade — there's only ever one EditorPreferences
		// per editor process.
		struct State {
			EditorThemeMode Theme = EditorThemeMode::Dark;
			ImVec4 CustomColors[ImGuiCol_COUNT]{};
			bool CustomColorsSeeded = false;

			uint64_t EditorFontAssetId = k_DefaultFontAssetId;

			bool ShowFileExtensions = false;
			bool AutoSaveScenes = false;
			float AutoSaveIntervalSeconds = 120.0f;

			bool Loaded = false;
			bool FreshlyCreated = false;
		};

		State& S() {
			static State s;
			return s;
		}

		std::filesystem::path PrefsPath() {
			try {
				return std::filesystem::path(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData))
					/ "Index" / "Editor" / "EditorPreferences.json";
			} catch (...) {
				return std::filesystem::path("EditorPreferences.json");
			}
		}

		const char* ThemeModeToString(EditorThemeMode mode) {
			switch (mode) {
				case EditorThemeMode::Dark:          return "Dark";
				case EditorThemeMode::Light:         return "Light";
				case EditorThemeMode::SystemDefault: return "SystemDefault";
				case EditorThemeMode::Custom:        return "Custom";
			}
			return "Dark";
		}

		EditorThemeMode ThemeModeFromString(std::string_view value) {
			if (value == "Light")         return EditorThemeMode::Light;
			if (value == "SystemDefault") return EditorThemeMode::SystemDefault;
			if (value == "Custom")        return EditorThemeMode::Custom;
			return EditorThemeMode::Dark;
		}

#ifdef _WIN32
		// Reads HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme.
		// 0 = dark theme, 1 = light theme. Defaults to dark if the key is missing.
		bool IsSystemDarkMode_Win32() {
			DWORD value = 0;
			DWORD size = sizeof(value);
			LSTATUS status = RegGetValueW(
				HKEY_CURRENT_USER,
				L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
				L"AppsUseLightTheme",
				RRF_RT_REG_DWORD,
				nullptr,
				&value,
				&size);
			if (status != ERROR_SUCCESS) {
				return true; // default to dark when registry says nothing
			}
			return value == 0;
		}
#else
		bool IsSystemDarkMode_Win32() { return true; }
#endif

		// Resolve SystemDefault → Dark/Light. ApplyTheme calls this once
		// per invocation; switching the Windows theme mid-session won't
		// auto-update the editor until the user revisits the prefs panel
		// (we could hook WM_SETTINGCHANGE, but the cost/benefit isn't
		// worth it for a setting users rarely flip).
		EditorThemeMode ResolveTheme(EditorThemeMode mode) {
			if (mode == EditorThemeMode::SystemDefault) {
				return IsSystemDarkMode_Win32() ? EditorThemeMode::Dark : EditorThemeMode::Light;
			}
			return mode;
		}

		void SeedCustomColorsFromCurrent() {
			const ImGuiStyle& style = ImGui::GetStyle();
			std::memcpy(S().CustomColors, style.Colors, sizeof(ImVec4) * ImGuiCol_COUNT);
			S().CustomColorsSeeded = true;
		}

		Json::Value Vec4ToJson(const ImVec4& v) {
			Json::Value arr = Json::Value::MakeArray();
			arr.Append(Json::Value(static_cast<double>(v.x)));
			arr.Append(Json::Value(static_cast<double>(v.y)));
			arr.Append(Json::Value(static_cast<double>(v.z)));
			arr.Append(Json::Value(static_cast<double>(v.w)));
			return arr;
		}

		bool JsonToVec4(const Json::Value& v, ImVec4& out) {
			if (!v.IsArray()) return false;
			const Json::Value::Array& arr = v.GetArray();
			if (arr.size() != 4) return false;
			out.x = static_cast<float>(arr[0].AsDoubleOr(0.0));
			out.y = static_cast<float>(arr[1].AsDoubleOr(0.0));
			out.z = static_cast<float>(arr[2].AsDoubleOr(0.0));
			out.w = static_cast<float>(arr[3].AsDoubleOr(1.0));
			return true;
		}
	}

	void EditorPreferences::Load() {
		State& s = S();
		if (s.Loaded) return;
		s.Loaded = true;

		const std::filesystem::path path = PrefsPath();
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) {
			// First launch — defaults already set, write the file so the
			// user can see it exists. WasFreshlyCreated() flips true so
			// the editor knows it can seed values from a legacy project
			// without clobbering an existing pref set.
			s.FreshlyCreated = true;
			Save();
			return;
		}

		const std::string text = File::ReadAllText(path.string());
		if (text.empty()) {
			IDX_CORE_WARN_TAG("EditorPrefs", "EditorPreferences.json was empty; using defaults.");
			return;
		}

		std::string parseError;
		Json::Value root = Json::Parse(text, &parseError);
		if (!root.IsObject()) {
			IDX_CORE_WARN_TAG("EditorPrefs",
				"Failed to parse EditorPreferences.json: {}", parseError);
			return;
		}

		if (const Json::Value* v = root.FindMember("Theme")) {
			s.Theme = ThemeModeFromString(v->AsStringOr("Dark"));
		}
		if (const Json::Value* v = root.FindMember("EditorFontAssetId")) {
			// Stored as a decimal string so 64-bit ids round-trip cleanly.
			// Same convention as IndexProject::DefaultFontAssetId.
			s.EditorFontAssetId = v->IsString()
				? static_cast<uint64_t>(std::stoull(v->AsStringOr("0")))
				: v->AsUInt64Or(k_DefaultFontAssetId);
			if (s.EditorFontAssetId == 0) {
				s.EditorFontAssetId = k_DefaultFontAssetId;
			}
		}
		if (const Json::Value* v = root.FindMember("ShowFileExtensions")) {
			s.ShowFileExtensions = v->AsBoolOr(false);
		}
		if (const Json::Value* v = root.FindMember("AutoSaveScenes")) {
			s.AutoSaveScenes = v->AsBoolOr(false);
		}
		if (const Json::Value* v = root.FindMember("AutoSaveIntervalSeconds")) {
			s.AutoSaveIntervalSeconds = static_cast<float>(v->AsDoubleOr(120.0));
			if (s.AutoSaveIntervalSeconds < 5.0f) s.AutoSaveIntervalSeconds = 5.0f;
		}

		if (const Json::Value* v = root.FindMember("CustomColors"); v && v->IsObject()) {
			// Seed from compiled-in dark defaults first so any color not
			// present in the file falls back to a sensible value rather
			// than zero-alpha black.
			ImGuiStyle tempStyle;
			ImGui::StyleColorsDark(&tempStyle);
			std::memcpy(s.CustomColors, tempStyle.Colors, sizeof(ImVec4) * ImGuiCol_COUNT);
			for (int i = 0; i < ImGuiCol_COUNT; i++) {
				const char* name = ImGui::GetStyleColorName(i);
				if (!name) continue;
				if (const Json::Value* entry = v->FindMember(name)) {
					ImVec4 parsed;
					if (JsonToVec4(*entry, parsed)) {
						s.CustomColors[i] = parsed;
					}
				}
			}
			s.CustomColorsSeeded = true;
		}
	}

	bool EditorPreferences::WasFreshlyCreated() {
		return S().FreshlyCreated;
	}

	void EditorPreferences::Save() {
		const State& s = S();
		const std::filesystem::path path = PrefsPath();
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);

		Json::Value root = Json::Value::MakeObject();
		root.AddMember("Theme", Json::Value(std::string(ThemeModeToString(s.Theme))));
		root.AddMember("EditorFontAssetId", Json::Value(std::to_string(s.EditorFontAssetId)));
		root.AddMember("ShowFileExtensions", Json::Value(s.ShowFileExtensions));
		root.AddMember("AutoSaveScenes", Json::Value(s.AutoSaveScenes));
		root.AddMember("AutoSaveIntervalSeconds", Json::Value(static_cast<double>(s.AutoSaveIntervalSeconds)));

		// Always serialize CustomColors once seeded — the user can return
		// to the saved set after experimenting with Dark/Light/System.
		if (s.CustomColorsSeeded) {
			Json::Value colors = Json::Value::MakeObject();
			for (int i = 0; i < ImGuiCol_COUNT; i++) {
				const char* name = ImGui::GetStyleColorName(i);
				if (!name) continue;
				colors.AddMember(std::string(name), Vec4ToJson(s.CustomColors[i]));
			}
			root.AddMember("CustomColors", std::move(colors));
		}

		const std::string text = Json::Stringify(root, /*pretty*/ true);
		if (!File::WriteAllText(path.string(), text)) {
			IDX_CORE_WARN_TAG("EditorPrefs",
				"Failed to write EditorPreferences.json to '{}'.", path.string());
		}
	}

	void EditorPreferences::ApplyTheme() {
		const EditorThemeMode resolved = ResolveTheme(S().Theme);

		switch (resolved) {
			case EditorThemeMode::Light:
				// Plain ImGui Light. We don't carry an Index-branded light
				// variant; the user opting in to Light is explicitly asking
				// for the stock palette.
				ImGui::StyleColorsLight();
				return;
			case EditorThemeMode::Dark:
			case EditorThemeMode::SystemDefault: // already resolved above
				// Index dark palette only — sizing/rounding from the
				// startup ApplyIndexTheme (post ScaleAllSizes) is preserved.
				// Light → Dark thus keeps the DPI-scaled metrics.
				ImGuiContextLayer::ApplyIndexThemeColors();
				return;
			case EditorThemeMode::Custom: {
				if (!S().CustomColorsSeeded) {
					SeedCustomColorsFromCurrent();
				}
				ImGuiStyle& style = ImGui::GetStyle();
				std::memcpy(style.Colors, S().CustomColors, sizeof(ImVec4) * ImGuiCol_COUNT);
				return;
			}
		}
	}

	EditorThemeMode EditorPreferences::GetThemeMode() {
		return S().Theme;
	}

	void EditorPreferences::SetThemeMode(EditorThemeMode mode) {
		if (S().Theme == mode) return;

		// Seed Custom from the *current* style at the moment of switch so
		// the editable swatches start from "what the user was just
		// looking at", not all-zeroes.
		if (mode == EditorThemeMode::Custom && !S().CustomColorsSeeded) {
			SeedCustomColorsFromCurrent();
		}

		S().Theme = mode;
		ApplyTheme();
		Save();
	}

	ImVec4& EditorPreferences::CustomColor(int imGuiColIdx) {
		assert(imGuiColIdx >= 0 && imGuiColIdx < ImGuiCol_COUNT);
		if (!S().CustomColorsSeeded) {
			SeedCustomColorsFromCurrent();
		}
		return S().CustomColors[imGuiColIdx];
	}

	void EditorPreferences::ResetCustomColorsFromCurrent() {
		SeedCustomColorsFromCurrent();
		Save();
	}

	uint64_t EditorPreferences::GetEditorFontAssetId() {
		return S().EditorFontAssetId;
	}

	void EditorPreferences::SetEditorFontAssetId(uint64_t id) {
		const uint64_t normalized = (id == 0) ? k_DefaultFontAssetId : id;
		if (S().EditorFontAssetId == normalized) return;
		S().EditorFontAssetId = normalized;
		Save();
	}

	bool EditorPreferences::GetShowFileExtensions() {
		return S().ShowFileExtensions;
	}

	void EditorPreferences::SetShowFileExtensions(bool value) {
		if (S().ShowFileExtensions == value) return;
		S().ShowFileExtensions = value;
		Save();
	}

	bool EditorPreferences::GetAutoSaveScenes() {
		return S().AutoSaveScenes;
	}

	void EditorPreferences::SetAutoSaveScenes(bool value) {
		if (S().AutoSaveScenes == value) return;
		S().AutoSaveScenes = value;
		Save();
	}

	float EditorPreferences::GetAutoSaveIntervalSeconds() {
		return S().AutoSaveIntervalSeconds;
	}

	void EditorPreferences::SetAutoSaveIntervalSeconds(float seconds) {
		const float clamped = (seconds < 5.0f) ? 5.0f : seconds;
		if (S().AutoSaveIntervalSeconds == clamped) return;
		S().AutoSaveIntervalSeconds = clamped;
		Save();
	}

}
