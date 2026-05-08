#include "pch.hpp"
#include "File.hpp"

#ifdef AIM_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#endif

namespace Axiom {
	bool File::Exists(const std::string& path) {
		return std::filesystem::exists(path);
	}

	bool File::WriteAllText(const std::string& path, const std::string& text) {
		// Stage to a sibling .tmp first, then rename. A crash mid-stream then loses only
		// the staging file, leaving the original intact. The previous in-place truncate-
		// then-write produced corrupted scenes / project files on power loss or process
		// kill.
		const std::filesystem::path target(path);
		std::filesystem::path tmp = target;
		tmp += ".tmp";

		{
			std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
			if (!file.is_open()) {
				AIM_CORE_ERROR("File couldn't be opened for writing: {}", tmp.string());
				return false;
			}

			file.write(text.c_str(), text.size());
			if (!file.good()) {
				AIM_CORE_ERROR("Write failed (disk full / IO error): {}", tmp.string());
				file.close();
				std::error_code ec;
				std::filesystem::remove(tmp, ec);
				return false;
			}
			file.close();
			if (file.fail()) {
				AIM_CORE_ERROR("Close failed for: {}", tmp.string());
				std::error_code ec;
				std::filesystem::remove(tmp, ec);
				return false;
			}
		}

#ifdef AIM_PLATFORM_WINDOWS
		// MoveFileExW with REPLACE_EXISTING + WRITE_THROUGH gives us a single atomic
		// swap: no remove-then-rename window where another process could recreate
		// `target` and have its content silently overwritten. WRITE_THROUGH flushes
		// the metadata before returning so a power-loss after success leaves a
		// committed file rather than a directory entry pointing at unflushed data.
		const std::wstring tmpW = tmp.wstring();
		const std::wstring targetW = target.wstring();
		if (!MoveFileExW(tmpW.c_str(), targetW.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			const DWORD err = GetLastError();
			AIM_CORE_ERROR("MoveFileEx failed: {} -> {} (Win32 error {})", tmp.string(), target.string(), static_cast<unsigned long>(err));
			std::error_code cleanup;
			std::filesystem::remove(tmp, cleanup);
			return false;
		}
#else
		// POSIX rename is atomic and replaces the target unconditionally.
		std::error_code ec;
		std::filesystem::rename(tmp, target, ec);
		if (ec) {
			AIM_CORE_ERROR("Rename failed: {} -> {} ({})", tmp.string(), target.string(), ec.message());
			std::error_code cleanup;
			std::filesystem::remove(tmp, cleanup);
			return false;
		}
#endif

		return true;
	}

	std::string File::ReadAllText(const std::string& path) {
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open()) {
			AIM_CORE_ERROR("File couldn't be opened for reading: {}", path);
			return {};
		}

		return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	}

	std::vector<std::uint8_t> File::ReadAllBytes(const std::string& path) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file.is_open()) {
			AIM_CORE_ERROR("File couldn't be opened for reading: {}", path);
			return {};
		}

		std::streamsize size = file.tellg();

		if (size < 0) {
			AIM_CORE_ERROR("tellg() failed while reading file: {}", path);
			return {};
		}

		file.seekg(0, std::ios::beg);

		std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));

		if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
			AIM_CORE_ERROR("File reading failed: {}", path);
			return {};
		}

		file.close();
		return buffer;
	}
}
