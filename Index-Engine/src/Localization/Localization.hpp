#pragma once
#include "Core/Export.hpp"

#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Index {

	// User-scoped UI language. Strings live in
	// IndexAssets/Localization/<code>.json (bundled) or
	// %LOCALAPPDATA%/Index/Localization/<code>.json (downloaded), and the
	// active code is persisted to %LOCALAPPDATA%/Index/locale.json. Both the
	// Launcher and the Editor link the engine DLL, so the same facade serves
	// both binaries without coupling their per-app settings classes.
	namespace Localization {

		enum class LanguageStatus : std::uint8_t {
			Installed,        // file is locally available (bundled or user dir)
			Available,        // listed in manifest but not yet downloaded
			Downloading,
			DownloadFailed,
		};

		struct LanguageInfo {
			std::string Code;
			std::string DisplayName;
			LanguageStatus Status = LanguageStatus::Installed;
			bool RequiresCjkFont = false;
		};

		// Idempotent. Scans bundled IndexAssets/Localization, then
		// %LOCALAPPDATA%/Index/Localization, then loads manifest.json so the
		// dropdown can offer not-yet-downloaded languages too. Safe to call
		// before any UI layer attaches — both binaries invoke this from
		// EntryPoint.hpp::Main right after InitializeCore.
		INDEX_API void Initialize();

		// Reference stays valid for the lifetime of the active language (i.e.
		// until SetLanguage swaps the table). Missing-key path:
		//   1. fall back to English
		//   2. otherwise log once and return a reference into a side map keyed
		//      by `key` so the raw identifier shows in the UI — visible to
		//      developers, harmless to end users.
		INDEX_API const std::string& Get(std::string_view key);

		INDEX_API const std::vector<LanguageInfo>& GetAvailableLanguages();
		INDEX_API const std::string& GetCurrentLanguage();

		// If the language is `Installed`, swaps the active table immediately.
		// If `Available`, queues a download and applies the switch once the
		// download completes (callers see the dropdown update next frame as
		// status flips through Downloading → Installed).
		// Invalid codes are ignored with a warning (no assert — locale.json is
		// user-editable).
		INDEX_API void SetLanguage(std::string_view code);

		// Kick off an async download for an `Available` language (and the
		// CJK font, if required and not already installed). No-op if a
		// download is already running or the language is not `Available`.
		INDEX_API void RequestLanguageDownload(std::string_view code);

		struct DownloadProgress {
			std::string Code;
			std::string Stage;
			float Progress = 0.0f;
			bool Failed = false;
			bool RestartRequired = false;
			std::string Error;
		};

		// Snapshot of any in-flight or recently-finished download. UI polls
		// this each frame for progress display. Returns nullopt only when no
		// download has been initiated since Initialize().
		INDEX_API std::optional<DownloadProgress> GetActiveDownload();

		// Called once per frame from the main loop to settle a finished
		// download — applies any queued language switch, clears transient
		// state, etc. Without this the worker's results would never affect
		// the visible UI. Cheap on the common path (no work if no download).
		INDEX_API void Poll();

		using ChangeCallbackHandle = std::uint32_t;
		INDEX_API ChangeCallbackHandle RegisterChangeCallback(std::function<void()> cb);
		INDEX_API void UnregisterChangeCallback(ChangeCallbackHandle handle);

		// std::vformat-based positional substitution. Translation values use
		// {0}, {1} placeholders; missing-key fallback still applies because
		// the format string comes from Get(key).
		template <typename... Args>
		std::string Format(std::string_view key, Args&&... args) {
			const std::string& tmpl = Get(key);
			return std::vformat(tmpl, std::make_format_args(args...));
		}

	}

}

#define IDX_TR(key) ::Index::Localization::Get(key)
