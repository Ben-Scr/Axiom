#include <pch.hpp>
#include "Localization/LanguageDownloader.hpp"

#include "Core/Log.hpp"
#include "Serialization/Path.hpp"
#include "Utils/Process.hpp"

#include <array>
#include <cctype>
#include <fstream>
#include <system_error>
#include <vector>

#ifdef IDX_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace Index::Localization {

	namespace {

		std::string GetPackageToolExecutableName() {
#if defined(IDX_PLATFORM_WINDOWS)
			return "Index-PackageTool.exe";
#else
			return "Index-PackageTool";
#endif
		}

		// Mirrors PackageManager's resolution logic. Looks alongside the running
		// exe first (the postbuild copy lands the tool next to consumer binaries),
		// then falls back to the Index-PackageTool build output a few levels up
		// from the launcher's dev-layout exeDir. Returns empty if nothing found.
		std::filesystem::path ResolvePackageToolPath() {
			const std::filesystem::path exeDir(Path::ExecutableDir());

			std::vector<std::filesystem::path> candidates;
			candidates.push_back(exeDir / GetPackageToolExecutableName());
			candidates.push_back(exeDir / "Index-PackageTool.dll");

			const std::filesystem::path projectDir = exeDir / ".." / ".." / ".." / "Index-PackageTool";
			for (const char* configuration : { "Debug", "Release", "Dist" }) {
				const std::filesystem::path outputDirectory = projectDir / "bin" / configuration / "net9.0";
				candidates.push_back(outputDirectory / GetPackageToolExecutableName());
				candidates.push_back(outputDirectory / "Index-PackageTool.dll");
			}

			for (const auto& candidate : candidates) {
				std::error_code ec;
				if (std::filesystem::exists(candidate, ec) && !ec) {
					return std::filesystem::canonical(candidate, ec);
				}
			}
			return {};
		}

		std::string ToLowerHex(std::string_view s) {
			std::string out;
			out.reserve(s.size());
			for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			return out;
		}

#ifdef IDX_PLATFORM_WINDOWS
		bool ComputeSha256Win32(const std::filesystem::path& filePath, std::string& outHexLower, std::string& outError) {
			BCRYPT_ALG_HANDLE algHandle = nullptr;
			NTSTATUS status = BCryptOpenAlgorithmProvider(&algHandle, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
			if (status != 0) { outError = "BCryptOpenAlgorithmProvider failed"; return false; }

			struct AlgGuard { BCRYPT_ALG_HANDLE h; ~AlgGuard() { if (h) BCryptCloseAlgorithmProvider(h, 0); } } algGuard{ algHandle };

			DWORD hashObjLen = 0; DWORD cbResult = 0;
			status = BCryptGetProperty(algHandle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjLen), sizeof(hashObjLen), &cbResult, 0);
			if (status != 0) { outError = "BCryptGetProperty(OBJECT_LENGTH) failed"; return false; }

			std::vector<UCHAR> hashObj(hashObjLen);
			BCRYPT_HASH_HANDLE hashHandle = nullptr;
			status = BCryptCreateHash(algHandle, &hashHandle, hashObj.data(), hashObjLen, nullptr, 0, 0);
			if (status != 0) { outError = "BCryptCreateHash failed"; return false; }

			struct HashGuard { BCRYPT_HASH_HANDLE h; ~HashGuard() { if (h) BCryptDestroyHash(h); } } hashGuard{ hashHandle };

			std::ifstream in(filePath, std::ios::binary);
			if (!in.is_open()) { outError = "open failed"; return false; }

			std::array<char, 64 * 1024> buf{};
			while (in.good()) {
				in.read(buf.data(), buf.size());
				const std::streamsize n = in.gcount();
				if (n <= 0) break;
				status = BCryptHashData(hashHandle, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0);
				if (status != 0) { outError = "BCryptHashData failed"; return false; }
			}

			std::array<UCHAR, 32> digest{};
			status = BCryptFinishHash(hashHandle, digest.data(), static_cast<ULONG>(digest.size()), 0);
			if (status != 0) { outError = "BCryptFinishHash failed"; return false; }

			std::string hex;
			hex.reserve(digest.size() * 2);
			static const char* k_Hex = "0123456789abcdef";
			for (UCHAR b : digest) {
				hex.push_back(k_Hex[(b >> 4) & 0xF]);
				hex.push_back(k_Hex[b & 0xF]);
			}
			outHexLower = std::move(hex);
			return true;
		}
#else
		bool ComputeSha256Win32(const std::filesystem::path&, std::string& outHexLower, std::string& outError) {
			outHexLower.clear();
			outError = "SHA-256 verification not implemented on this platform";
			return false;
		}
#endif

	}

	LanguageDownloader::~LanguageDownloader() {
		if (m_Worker.joinable()) m_Worker.join();
	}

	bool LanguageDownloader::IsRunning() const {
		std::scoped_lock lock(m_Mutex);
		return m_Running;
	}

	DownloadStatusSnapshot LanguageDownloader::Status() const {
		std::scoped_lock lock(m_Mutex);
		DownloadStatusSnapshot s;
		s.Code = m_Code;
		s.Stage = m_Stage;
		s.Progress = m_Progress;
		s.Running = m_Running;
		s.Finished = m_Finished;
		s.Success = m_Success;
		s.RestartRequired = m_RestartRequired;
		s.Error = m_Error;
		return s;
	}

	void LanguageDownloader::Start(std::string code, std::vector<DownloadItem> items, bool restartRequiredOnSuccess) {
		{
			std::scoped_lock lock(m_Mutex);
			if (m_Running) return;
		}

		if (m_Worker.joinable()) m_Worker.join();

		{
			std::scoped_lock lock(m_Mutex);
			m_Code = code;
			m_Stage.clear();
			m_Progress = 0.0f;
			m_Running = true;
			m_Finished = false;
			m_Success = false;
			m_RestartRequired = false;
			m_Error.clear();
		}

		m_Worker = std::thread(
			[this, code = std::move(code), items = std::move(items), restartRequiredOnSuccess]() mutable {
				RunWorker(std::move(code), std::move(items), restartRequiredOnSuccess);
			});
	}

	void LanguageDownloader::RunWorker(std::string code, std::vector<DownloadItem> items, bool restartRequiredOnSuccess) {
		bool success = true;
		std::string finalError;

		const std::size_t total = items.empty() ? 1 : items.size();
		for (std::size_t i = 0; i < items.size(); ++i) {
			const DownloadItem& item = items[i];
			{
				std::scoped_lock lock(m_Mutex);
				m_Stage = "Downloading " + item.TargetPath.filename().string();
				m_Progress = static_cast<float>(i) / static_cast<float>(total);
			}

			std::string err;
			if (!DownloadOne(item, err)) {
				success = false;
				finalError = err;
				break;
			}
		}

		{
			std::scoped_lock lock(m_Mutex);
			m_Stage = success ? "Done" : "Failed";
			m_Progress = success ? 1.0f : m_Progress;
			m_Running = false;
			m_Finished = true;
			m_Success = success;
			m_RestartRequired = success && restartRequiredOnSuccess;
			m_Error = finalError;
		}
	}

	bool LanguageDownloader::DownloadOne(const DownloadItem& item, std::string& outError) {
		const std::filesystem::path toolPath = ResolvePackageToolPath();
		if (toolPath.empty()) {
			outError = "Index-PackageTool not found";
			return false;
		}

		std::error_code ec;
		std::filesystem::create_directories(item.TargetPath.parent_path(), ec);

		const std::filesystem::path tempPath = item.TargetPath.string() + ".dl.tmp";
		std::filesystem::remove(tempPath, ec);

		std::vector<std::string> command;
		if (toolPath.extension() == ".dll") {
			command.push_back("dotnet");
			command.push_back(toolPath.string());
		}
		else {
			command.push_back(toolPath.string());
		}
		command.push_back("github-download");
		command.push_back(item.Url);
		command.push_back(tempPath.string());

		const Process::Result result = Process::Run(command, {}, std::chrono::milliseconds(0));
		if (!result.Succeeded()) {
			std::filesystem::remove(tempPath, ec);
			outError = "PackageTool exited with code " + std::to_string(result.ExitCode);
			IDX_CORE_WARN_TAG("Localization", "Download failed for {}: {}", item.Url, result.Output);
			return false;
		}

		if (item.ExpectedSizeBytes > 0) {
			const std::uintmax_t actual = std::filesystem::file_size(tempPath, ec);
			if (ec || actual != item.ExpectedSizeBytes) {
				std::filesystem::remove(tempPath, ec);
				outError = "Size mismatch (expected " + std::to_string(item.ExpectedSizeBytes)
					+ ", got " + (ec ? std::string("unreadable") : std::to_string(actual)) + ")";
				return false;
			}
		}

		if (!item.ExpectedSha256.empty()) {
			std::string verifyError;
			if (!VerifySha256(tempPath, item.ExpectedSha256, verifyError)) {
				std::filesystem::remove(tempPath, ec);
				outError = verifyError;
				return false;
			}
		}

		std::filesystem::remove(item.TargetPath, ec);
		std::filesystem::rename(tempPath, item.TargetPath, ec);
		if (ec) {
			outError = "Rename to target failed: " + ec.message();
			return false;
		}

		IDX_CORE_INFO_TAG("Localization", "Downloaded '{}' ({})",
			item.TargetPath.string(), item.Url);
		return true;
	}

	bool LanguageDownloader::VerifySha256(const std::filesystem::path& filePath,
		std::string_view expectedHex, std::string& outError) const {
		std::string actual;
		if (!ComputeSha256Win32(filePath, actual, outError)) return false;

		const std::string expected = ToLowerHex(expectedHex);
		if (actual != expected) {
			outError = "SHA-256 mismatch (expected " + expected + ", got " + actual + ")";
			return false;
		}
		return true;
	}

}
