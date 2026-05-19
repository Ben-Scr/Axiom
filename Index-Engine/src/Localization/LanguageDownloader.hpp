#pragma once

#include "Core/Export.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Index::Localization {

	// One file the downloader should fetch. Verified after fetch by length and
	// (if set) SHA-256. The downloader stages every item to a `.tmp` sibling
	// and only renames-into-place after every check passes — a failed item
	// never replaces the existing user-writable file.
	struct DownloadItem {
		std::string Url;
		std::filesystem::path TargetPath;
		std::string ExpectedSha256;       // hex, lowercase. Empty = skip verify.
		std::uint64_t ExpectedSizeBytes = 0;
	};

	struct DownloadStatusSnapshot {
		std::string Code;          // language code this download belongs to
		std::string Stage;         // human-readable, surfaced to UI
		float Progress = 0.0f;     // 0..1; coarse (per-item) — see notes in .cpp
		bool Running = false;
		bool Finished = false;
		bool Success = false;
		bool RestartRequired = false;  // CJK font installed; needs restart
		std::string Error;
	};

	// Owns a single worker thread that processes a queue of items serially.
	// Only one job runs at a time; a second Start() while running is a no-op.
	// Lives inside Localization.cpp's anonymous-namespace state.
	class LanguageDownloader {
	public:
		LanguageDownloader() = default;
		~LanguageDownloader();

		LanguageDownloader(const LanguageDownloader&) = delete;
		LanguageDownloader& operator=(const LanguageDownloader&) = delete;

		void Start(std::string code, std::vector<DownloadItem> items, bool restartRequiredOnSuccess);

		DownloadStatusSnapshot Status() const;
		bool IsRunning() const;

	private:
		void RunWorker(std::string code, std::vector<DownloadItem> items, bool restartRequiredOnSuccess);
		bool DownloadOne(const DownloadItem& item, std::string& outError);
		bool VerifySha256(const std::filesystem::path& filePath,
			std::string_view expectedHex, std::string& outError) const;

		mutable std::mutex m_Mutex;
		std::thread m_Worker;

		std::string m_Code;
		std::string m_Stage;
		float m_Progress = 0.0f;
		bool m_Running = false;
		bool m_Finished = false;
		bool m_Success = false;
		bool m_RestartRequired = false;
		std::string m_Error;
	};

}
