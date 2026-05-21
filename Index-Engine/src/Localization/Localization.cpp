#include <pch.hpp>
#include "Localization/Localization.hpp"

#include "Core/Log.hpp"
#include "Localization/LanguageDownloader.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/SpecialFolder.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Index::Localization {

	namespace {

		constexpr const char* k_FallbackLanguage = "en";
		constexpr int k_LocaleSchemaVersion = 1;
		constexpr const char* k_CjkFontSubpath = "Fonts/NotoSansCJK/NotoSansCJK-Regular.ttc";

		struct ManifestLanguage {
			std::string Code;
			std::string DisplayName;
			std::string Url;
			std::string Sha256;
			std::uint64_t SizeBytes = 0;
			bool RequiresCjkFont = false;
		};

		struct ManifestFont {
			std::string Url;
			std::string Sha256;
			std::uint64_t SizeBytes = 0;
			std::string TargetSubpath;
		};

		struct State {
			bool Initialized = false;
			std::string CurrentLanguage = k_FallbackLanguage;

			std::vector<LanguageInfo> AvailableLanguages;

			// Local files on disk that we can load right now. Code -> path.
			// User-dir entries override bundled-dir entries on collision.
			std::unordered_map<std::string, std::filesystem::path> InstalledFiles;

			// Manifest entries — drives the "Available" rows and downloads.
			std::unordered_map<std::string, ManifestLanguage> Manifest;
			std::optional<ManifestFont> CjkFontEntry;

			std::unordered_map<std::string, std::string> Active;
			std::unordered_map<std::string, std::string> Fallback;

			// Insert-only. Doubles as missing-key dedupe: presence means
			// "we already warned about this key this session".
			std::unordered_map<std::string, std::string> MissingKeys;

			std::unordered_map<ChangeCallbackHandle, std::function<void()>> ChangeCallbacks;
			ChangeCallbackHandle NextCallbackHandle = 1;

			LanguageDownloader Downloader;
			std::string PendingLanguageSwitch;  // applied on Poll() when download finishes
		};

		State& S() {
			static State s;
			return s;
		}

		std::filesystem::path LocalePrefsPath() {
			try {
				return std::filesystem::path(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData))
					/ "Index" / "locale.json";
			}
			catch (...) {
				return std::filesystem::path("locale.json");
			}
		}

		std::filesystem::path UserLocalizationDir() {
			try {
				return std::filesystem::path(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData))
					/ "Index" / "Localization";
			}
			catch (...) {
				return std::filesystem::path("Localization");
			}
		}

		std::filesystem::path UserCjkFontPath() {
			try {
				return std::filesystem::path(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData))
					/ "Index" / "Fonts" / "NotoSansCJK" / "NotoSansCJK-Regular.ttc";
			}
			catch (...) {
				return std::filesystem::path("NotoSansCJK-Regular.ttc");
			}
		}

		std::filesystem::path BundledLocalizationDir() {
			const std::string resolved = Path::ResolveIndexAssets("Localization");
			if (resolved.empty()) return {};
			return std::filesystem::path(resolved);
		}

		std::string ReadLanguagePreference() {
			const std::filesystem::path path = LocalePrefsPath();
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec)) {
				return {};
			}

			const std::string text = File::ReadAllText(path.string());
			if (text.empty()) return {};

			std::string parseError;
			Json::Value root = Json::Parse(text, &parseError);
			if (!root.IsObject()) {
				IDX_CORE_WARN_TAG("Localization",
					"Failed to parse locale.json: {}", parseError);
				return {};
			}

			if (const Json::Value* v = root.FindMember("language")) {
				return v->AsStringOr({});
			}
			return {};
		}

		void WriteLanguagePreference(std::string_view code) {
			const std::filesystem::path path = LocalePrefsPath();
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);

			Json::Value root = Json::Value::MakeObject();
			root.AddMember("language", Json::Value(std::string(code)));
			root.AddMember("schemaVersion", Json::Value(static_cast<int64_t>(k_LocaleSchemaVersion)));

			const std::string text = Json::Stringify(root, /*pretty*/ true);
			if (!File::WriteAllText(path.string(), text)) {
				IDX_CORE_WARN_TAG("Localization",
					"Failed to write locale.json to '{}'", path.string());
			}
		}

		// Parses just the meta block — used by the directory scan so the
		// dropdown can populate without loading every translation table.
		bool ParseMeta(const std::filesystem::path& filePath, std::string& outCode, std::string& outDisplayName) {
			const std::string text = File::ReadAllText(filePath.string());
			if (text.empty()) return false;

			std::string parseError;
			Json::Value root = Json::Parse(text, &parseError);
			if (!root.IsObject()) {
				IDX_CORE_WARN_TAG("Localization",
					"Failed to parse '{}': {}", filePath.string(), parseError);
				return false;
			}

			const Json::Value* meta = root.FindMember("meta");
			if (!meta || !meta->IsObject()) return false;

			const Json::Value* code = meta->FindMember("code");
			const Json::Value* displayName = meta->FindMember("displayName");
			if (!code || !displayName) return false;

			outCode = code->AsStringOr({});
			outDisplayName = displayName->AsStringOr({});
			return !outCode.empty() && !outDisplayName.empty();
		}

		bool LoadStrings(const std::filesystem::path& filePath, std::unordered_map<std::string, std::string>& outTable) {
			const std::string text = File::ReadAllText(filePath.string());
			if (text.empty()) return false;

			std::string parseError;
			Json::Value root = Json::Parse(text, &parseError);
			if (!root.IsObject()) {
				IDX_CORE_WARN_TAG("Localization",
					"Failed to parse '{}': {}", filePath.string(), parseError);
				return false;
			}

			const Json::Value* strings = root.FindMember("strings");
			if (!strings || !strings->IsObject()) {
				IDX_CORE_WARN_TAG("Localization",
					"'{}' has no 'strings' object", filePath.string());
				return false;
			}

			outTable.clear();
			for (const auto& [key, value] : strings->GetObject()) {
				if (value.IsString()) {
					outTable.emplace(key, value.AsStringOr({}));
				}
			}
			return true;
		}

		// Walks one directory and records every <code>.json's path + metadata.
		// `displayNamesOut` carries the displayName for codes whose meta block
		// we managed to parse — used when no manifest entry overrides it.
		void ScanFilesIn(const std::filesystem::path& dir,
			std::unordered_map<std::string, std::filesystem::path>& files,
			std::unordered_map<std::string, std::string>& displayNamesOut) {
			if (dir.empty()) return;

			std::error_code ec;
			if (!std::filesystem::is_directory(dir, ec)) return;

			for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
				if (ec) break;
				if (!entry.is_regular_file()) continue;
				if (entry.path().extension() != ".json") continue;
				if (entry.path().filename() == "manifest.json") continue;

				std::string code, displayName;
				if (!ParseMeta(entry.path(), code, displayName)) continue;

				files[code] = entry.path();
				displayNamesOut.emplace(code, displayName);
			}
		}

		void LoadManifest() {
			State& s = S();
			s.Manifest.clear();
			s.CjkFontEntry.reset();

			const std::filesystem::path manifestPath = BundledLocalizationDir() / "manifest.json";
			std::error_code ec;
			if (!std::filesystem::is_regular_file(manifestPath, ec)) {
				return;
			}

			const std::string text = File::ReadAllText(manifestPath.string());
			if (text.empty()) return;

			std::string parseError;
			Json::Value root = Json::Parse(text, &parseError);
			if (!root.IsObject()) {
				IDX_CORE_WARN_TAG("Localization",
					"Failed to parse manifest.json: {}", parseError);
				return;
			}

			if (const Json::Value* languages = root.FindMember("languages"); languages && languages->IsArray()) {
				for (const Json::Value& entry : languages->GetArray()) {
					if (!entry.IsObject()) continue;
					ManifestLanguage lang;
					if (const Json::Value* v = entry.FindMember("code")) lang.Code = v->AsStringOr({});
					if (const Json::Value* v = entry.FindMember("displayName")) lang.DisplayName = v->AsStringOr({});
					if (const Json::Value* v = entry.FindMember("url")) lang.Url = v->AsStringOr({});
					if (const Json::Value* v = entry.FindMember("sha256")) lang.Sha256 = v->AsStringOr({});
					if (const Json::Value* v = entry.FindMember("sizeBytes")) lang.SizeBytes = static_cast<std::uint64_t>(v->AsUInt64Or(0));
					if (const Json::Value* v = entry.FindMember("requiresCjkFont")) lang.RequiresCjkFont = v->AsBoolOr(false);
					if (!lang.Code.empty()) s.Manifest.emplace(lang.Code, std::move(lang));
				}
			}

			if (const Json::Value* fonts = root.FindMember("fonts"); fonts && fonts->IsObject()) {
				if (const Json::Value* cjk = fonts->FindMember("cjk"); cjk && cjk->IsObject()) {
					ManifestFont f;
					if (const Json::Value* v = cjk->FindMember("url")) f.Url = v->AsStringOr({});
					if (const Json::Value* v = cjk->FindMember("sha256")) f.Sha256 = v->AsStringOr({});
					if (const Json::Value* v = cjk->FindMember("sizeBytes")) f.SizeBytes = static_cast<std::uint64_t>(v->AsUInt64Or(0));
					if (const Json::Value* v = cjk->FindMember("targetSubpath")) f.TargetSubpath = v->AsStringOr({});
					if (!f.Url.empty()) s.CjkFontEntry = std::move(f);
				}
			}
		}

		// Builds the AvailableLanguages list from the union of {installed
		// files} ∪ {manifest entries}, with each language tagged Installed /
		// Available based on whether a local file was found. Stable English-
		// first ordering keeps the dropdown predictable.
		void RebuildLanguageList() {
			State& s = S();
			s.AvailableLanguages.clear();

			std::unordered_set<std::string> seen;

			auto addEntry = [&](LanguageInfo info) {
				if (seen.insert(info.Code).second) {
					s.AvailableLanguages.push_back(std::move(info));
				}
			};

			// Installed files first — their displayName comes from the file's
			// meta block (preferred over manifest for community translations
			// dropped into the user dir).
			std::unordered_map<std::string, std::string> displayNames;
			std::unordered_map<std::string, std::filesystem::path> files;
			ScanFilesIn(BundledLocalizationDir(), files, displayNames);
			ScanFilesIn(UserLocalizationDir(), files, displayNames);  // user wins on collision

			for (const auto& [code, path] : files) {
				LanguageInfo info;
				info.Code = code;
				info.DisplayName = displayNames[code];
				info.Status = LanguageStatus::Installed;
				if (auto it = s.Manifest.find(code); it != s.Manifest.end()) {
					info.RequiresCjkFont = it->second.RequiresCjkFont;
				}
				addEntry(std::move(info));
			}
			s.InstalledFiles = std::move(files);

			// Then manifest entries we don't have locally.
			for (const auto& [code, entry] : s.Manifest) {
				if (seen.count(code)) continue;
				LanguageInfo info;
				info.Code = code;
				info.DisplayName = entry.DisplayName;
				info.Status = LanguageStatus::Available;
				info.RequiresCjkFont = entry.RequiresCjkFont;
				addEntry(std::move(info));
			}

			std::sort(s.AvailableLanguages.begin(), s.AvailableLanguages.end(),
				[](const LanguageInfo& a, const LanguageInfo& b) {
					if (a.Code == k_FallbackLanguage) return true;
					if (b.Code == k_FallbackLanguage) return false;
					return a.DisplayName < b.DisplayName;
				});
		}

		bool LoadLanguageTable(const std::string& code, std::unordered_map<std::string, std::string>& outTable) {
			State& s = S();
			auto it = s.InstalledFiles.find(code);
			if (it == s.InstalledFiles.end()) return false;
			return LoadStrings(it->second, outTable);
		}

		void FireChangeCallbacks() {
			for (const auto& [handle, cb] : S().ChangeCallbacks) {
				if (cb) cb();
			}
		}

		void SwapActiveTable(const std::string& code) {
			State& s = S();
			if (code == s.CurrentLanguage) return;

			std::unordered_map<std::string, std::string> newTable;
			if (code == k_FallbackLanguage) {
				newTable = s.Fallback;
			}
			else if (!LoadLanguageTable(code, newTable)) {
				IDX_CORE_WARN_TAG("Localization",
					"SwapActiveTable('{}') failed: file could not be loaded.", code);
				return;
			}

			s.Active = std::move(newTable);
			s.CurrentLanguage = code;
			s.MissingKeys.clear();
			FireChangeCallbacks();
			IDX_CORE_INFO_TAG("Localization", "Language switched to '{}'.", code);
		}

		// Kicks off a font-only download when the chosen language is already
		// installed locally (e.g. its .json ships in the bundled IndexAssets)
		// but the merged CJK font isn't on disk yet. Without this, picking a
		// CJK language whose JSON is bundled would never trigger the font
		// fetch — the download path in RequestLanguageDownload() only runs
		// for languages with Status == Available. No-op when there is no
		// CJK requirement, the font already exists at the user-dir path,
		// the manifest has no CJK entry, or another download is in flight.
		// Sets restartRequired so the UI knows the ImGui atlas has already
		// been baked without the glyph ranges and needs a relaunch.
		void EnsureCjkFontForInstalledLanguage(const std::string& code) {
			State& s = S();

			auto it = s.Manifest.find(code);
			if (it == s.Manifest.end()) return;
			if (!it->second.RequiresCjkFont) return;
			if (!s.CjkFontEntry) return;
			if (s.Downloader.IsRunning()) return;

			const std::filesystem::path fontTarget = UserCjkFontPath();
			std::error_code ec;
			if (std::filesystem::exists(fontTarget, ec)) return;

			std::vector<DownloadItem> items;
			items.push_back(DownloadItem{
				s.CjkFontEntry->Url,
				fontTarget,
				s.CjkFontEntry->Sha256,
				s.CjkFontEntry->SizeBytes,
			});

			s.Downloader.Start(code, std::move(items), /*restartRequired*/ true);
			IDX_CORE_INFO_TAG("Localization",
				"Started CJK font download for '{}' (language already installed).", code);
		}

		LanguageInfo* FindLanguage(const std::string& code) {
			State& s = S();
			for (auto& info : s.AvailableLanguages) {
				if (info.Code == code) return &info;
			}
			return nullptr;
		}

	}

	void Initialize() {
		State& s = S();
		if (s.Initialized) return;
		s.Initialized = true;

		LoadManifest();
		RebuildLanguageList();

		const bool fallbackLoaded = LoadLanguageTable(k_FallbackLanguage, s.Fallback);
		if (!fallbackLoaded) {
			IDX_CORE_WARN_TAG("Localization",
				"English fallback table not loaded — keys will surface as raw identifiers.");
		}

		std::string requested = ReadLanguagePreference();
		bool prefFileExists = !requested.empty();

		if (requested.empty()) {
			requested = k_FallbackLanguage;
		}

		LanguageInfo* info = FindLanguage(requested);
		if (!info || info->Status != LanguageStatus::Installed) {
			if (!info) {
				IDX_CORE_WARN_TAG("Localization",
					"Requested language '{}' is not available; falling back to '{}'.",
					requested, k_FallbackLanguage);
			}
			else {
				IDX_CORE_INFO_TAG("Localization",
					"Language '{}' not installed yet; falling back to '{}' for this session.",
					requested, k_FallbackLanguage);
			}
			requested = k_FallbackLanguage;
			prefFileExists = false;
		}

		s.CurrentLanguage = requested;
		if (requested == k_FallbackLanguage) {
			s.Active = s.Fallback;
		}
		else if (!LoadLanguageTable(requested, s.Active)) {
			IDX_CORE_WARN_TAG("Localization",
				"Failed to load language '{}'; falling back to '{}'.",
				requested, k_FallbackLanguage);
			s.CurrentLanguage = k_FallbackLanguage;
			s.Active = s.Fallback;
			prefFileExists = false;
		}

		if (!prefFileExists) {
			WriteLanguagePreference(s.CurrentLanguage);
		}

		EnsureCjkFontForInstalledLanguage(s.CurrentLanguage);

		IDX_CORE_INFO_TAG("Localization",
			"Initialized with language '{}' ({} available, {} in manifest).",
			s.CurrentLanguage, s.AvailableLanguages.size(), s.Manifest.size());
	}

	const std::string& Get(std::string_view key) {
		State& s = S();

		const std::string keyStr(key);
		if (auto it = s.Active.find(keyStr); it != s.Active.end()) {
			return it->second;
		}
		if (auto it = s.Fallback.find(keyStr); it != s.Fallback.end()) {
			return it->second;
		}

		auto [it, inserted] = s.MissingKeys.emplace(keyStr, keyStr);
		if (inserted) {
			IDX_CORE_WARN_TAG("Localization", "Missing key: '{}'", keyStr);
		}
		return it->second;
	}

	const std::vector<LanguageInfo>& GetAvailableLanguages() {
		return S().AvailableLanguages;
	}

	const std::string& GetCurrentLanguage() {
		return S().CurrentLanguage;
	}

	void SetLanguage(std::string_view code) {
		State& s = S();
		const std::string codeStr(code);

		if (codeStr == s.CurrentLanguage) return;

		LanguageInfo* info = FindLanguage(codeStr);
		if (!info) {
			IDX_CORE_WARN_TAG("Localization",
				"SetLanguage('{}') ignored: language not available.", codeStr);
			return;
		}

		if (info->Status == LanguageStatus::Installed) {
			SwapActiveTable(codeStr);
			WriteLanguagePreference(codeStr);
			EnsureCjkFontForInstalledLanguage(codeStr);
			return;
		}

		if (info->Status == LanguageStatus::Available) {
			s.PendingLanguageSwitch = codeStr;
			RequestLanguageDownload(codeStr);
			return;
		}

		// Downloading / DownloadFailed: ignore — UI surfaces the active
		// download separately, no second concurrent download to spawn.
	}

	void RequestLanguageDownload(std::string_view code) {
		State& s = S();
		const std::string codeStr(code);

		auto it = s.Manifest.find(codeStr);
		if (it == s.Manifest.end()) {
			IDX_CORE_WARN_TAG("Localization",
				"RequestLanguageDownload('{}') ignored: not in manifest.", codeStr);
			return;
		}
		const ManifestLanguage& entry = it->second;

		if (s.Downloader.IsRunning()) return;

		std::vector<DownloadItem> items;

		const std::filesystem::path jsonTarget = UserLocalizationDir() / (codeStr + ".json");
		items.push_back(DownloadItem{
			entry.Url,
			jsonTarget,
			entry.Sha256,
			entry.SizeBytes,
		});

		bool restartRequired = false;
		if (entry.RequiresCjkFont && s.CjkFontEntry) {
			const std::filesystem::path fontTarget = UserCjkFontPath();
			std::error_code ec;
			if (!std::filesystem::exists(fontTarget, ec)) {
				items.push_back(DownloadItem{
					s.CjkFontEntry->Url,
					fontTarget,
					s.CjkFontEntry->Sha256,
					s.CjkFontEntry->SizeBytes,
				});
				restartRequired = true;
			}
		}

		if (LanguageInfo* info = FindLanguage(codeStr)) {
			info->Status = LanguageStatus::Downloading;
		}

		s.Downloader.Start(codeStr, std::move(items), restartRequired);
		IDX_CORE_INFO_TAG("Localization", "Started download for '{}'.", codeStr);
	}

	std::optional<DownloadProgress> GetActiveDownload() {
		const DownloadStatusSnapshot snap = S().Downloader.Status();
		if (snap.Code.empty()) return std::nullopt;

		DownloadProgress out;
		out.Code = snap.Code;
		out.Stage = snap.Stage;
		out.Progress = snap.Progress;
		out.Failed = snap.Finished && !snap.Success;
		out.RestartRequired = snap.RestartRequired;
		out.Error = snap.Error;
		return out;
	}

	void Poll() {
		State& s = S();
		const DownloadStatusSnapshot snap = s.Downloader.Status();
		if (snap.Code.empty() || !snap.Finished) return;

		// The downloader has finished a job. Settle its results into the
		// service's state, then clear the pending switch so future polls
		// don't re-fire — but leave the snapshot in place so the UI can
		// keep showing "Done"/"Restart required" until the next download
		// starts.
		const std::string code = snap.Code;
		const bool success = snap.Success;

		if (success) {
			RebuildLanguageList();

			if (LanguageInfo* info = FindLanguage(code)) {
				info->Status = LanguageStatus::Installed;
			}

			if (!s.PendingLanguageSwitch.empty() && s.PendingLanguageSwitch == code) {
				SwapActiveTable(code);
				WriteLanguagePreference(code);
			}
		}
		else {
			if (LanguageInfo* info = FindLanguage(code)) {
				info->Status = LanguageStatus::DownloadFailed;
			}
		}

		s.PendingLanguageSwitch.clear();
	}

	ChangeCallbackHandle RegisterChangeCallback(std::function<void()> cb) {
		State& s = S();
		const ChangeCallbackHandle handle = s.NextCallbackHandle++;
		s.ChangeCallbacks.emplace(handle, std::move(cb));
		return handle;
	}

	void UnregisterChangeCallback(ChangeCallbackHandle handle) {
		S().ChangeCallbacks.erase(handle);
	}

}
