#pragma once
#include "Core/Export.hpp"
#include "Graphics/Text/FontHandle.hpp"

#include <cstdint>
#include <imgui.h>

namespace Index {

	// User-scoped editor preferences. Lives in
	// %LOCALAPPDATA%\Index\Editor\EditorPreferences.json and is loaded once
	// at editor startup. Anything here is intentionally NOT per-project —
	// it follows the user, not the project file. Project-scoped settings
	// still live on IndexProject.
	enum class EditorThemeMode : uint8_t {
		Dark = 0,
		Light,
		SystemDefault,
		Custom,
	};

	class EditorPreferences {
	public:
		// Read JSON from disk (creates file with defaults if missing).
		// Call once during editor OnAttach BEFORE ApplyTheme(). Idempotent.
		static void Load();
		// True if the most recent Load() call had to create a new prefs
		// file from defaults (i.e. first launch). Used by the editor's
		// legacy-project migration: when a project loaded with pre-2026-05
		// editor-pref fields is opened on a fresh editor install, those
		// values seed EditorPreferences. On subsequent launches the user's
		// already-customised prefs win — legacy project values are ignored.
		static bool WasFreshlyCreated();
		// Persist current state to disk. Cheap synchronous write; called
		// after every mutation by the Set*-helpers below.
		static void Save();

		// Push the active theme into the ImGui style. Re-resolves
		// SystemDefault each call so a Windows theme change picked up
		// between sessions is honored on next Load.
		static void ApplyTheme();

		// ── Appearance ────────────────────────────────────────────
		static EditorThemeMode GetThemeMode();
		// Setting Custom for the first time seeds m_CustomColors from the
		// style currently on screen so the user starts from "what they
		// were just looking at" rather than zeroed-out swatches.
		static void SetThemeMode(EditorThemeMode mode);
		// Mutable accessor — ColorEdit4 writes through this. Caller must
		// pass a valid ImGuiCol_ index (asserted internally).
		static ImVec4& CustomColor(int imGuiColIdx);
		// Reseeds m_CustomColors from the live ImGui style. Useful as a
		// "reset" button after the user has experimented and wants to
		// return to the current base theme's colors.
		static void ResetCustomColorsFromCurrent();

		static uint64_t GetEditorFontAssetId();
		// Persisted immediately; the new font only takes effect on next
		// editor launch (font atlas rebuild is not wired in this change).
		static void SetEditorFontAssetId(uint64_t id);

		// ── Behavior (migrated from IndexProject) ─────────────────
		static bool GetShowFileExtensions();
		static void SetShowFileExtensions(bool value);

		static bool GetAutoSaveScenes();
		static void SetAutoSaveScenes(bool value);
		static float GetAutoSaveIntervalSeconds();
		// Clamped to >= 5.0 to match the lower bound the project-settings
		// UI enforced when these fields lived on IndexProject.
		static void SetAutoSaveIntervalSeconds(float seconds);
	};

}
