#pragma once

#include "Project/ProjectManager.hpp"
#include "Scripting/ScriptType.hpp"
#include "Serialization/Path.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Axiom::EditorScriptDiscovery {

	struct ScriptEntry {
		std::string ClassName;
		std::filesystem::path Path;
		std::string Extension;
		ScriptType Type = ScriptType::Unknown;
		bool IsManagedComponent = false;
		bool IsGameSystem = false;
		bool IsGlobalSystem = false;
	};

	inline std::string ToLowerCopy(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		return value;
	}

	inline std::string TrimWhitespace(std::string value)
	{
		const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
			return std::isspace(ch) != 0;
		});
		const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
			return std::isspace(ch) != 0;
		}).base();

		if (first >= last) {
			return {};
		}

		return std::string(first, last);
	}

	inline bool IsNativeScriptBootstrapFile(const std::filesystem::path& path)
	{
		return path.filename().string() == "NativeScriptExports.cpp";
	}

	inline bool IsCSharpScriptExtension(const std::string& extension)
	{
		return ToLowerCopy(extension) == ".cs";
	}

	inline bool IsNativeScriptSourceExtension(const std::string& extension)
	{
		const std::string normalized = ToLowerCopy(extension);
		return normalized == ".cpp" || normalized == ".cc" || normalized == ".cxx";
	}

	inline bool IsValidRegisteredScriptToken(const std::string& token)
	{
		if (token.empty()) {
			return false;
		}

		for (char ch : token) {
			const unsigned char uch = static_cast<unsigned char>(ch);
			if (std::isalnum(uch) != 0 || ch == '_' || ch == ':') {
				continue;
			}
			return false;
		}

		return true;
	}

	inline bool ContainsScriptEntry(
		const std::vector<ScriptEntry>& entries,
		const std::string& className,
		ScriptType type)
	{
		return std::any_of(entries.begin(), entries.end(), [&](const ScriptEntry& entry) {
			return entry.ClassName == className && entry.Type == type;
		});
	}

	inline void AppendScriptEntry(
		std::vector<ScriptEntry>& entries,
		const std::string& className,
		const std::filesystem::path& path,
		const std::string& extension,
		ScriptType type,
		bool isManagedComponent = false,
		bool isGameSystem = false,
		bool isGlobalSystem = false)
	{
		if (className.empty() || type == ScriptType::Unknown || ContainsScriptEntry(entries, className, type)) {
			return;
		}

		entries.push_back({ className, path, extension, type, isManagedComponent, isGameSystem, isGlobalSystem });
	}

	inline std::string ReadSourceWithoutWhitespace(const std::filesystem::path& filePath)
	{
		std::ifstream input(filePath, std::ios::binary);
		if (!input) {
			return {};
		}

		const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		std::string compactSource;
		compactSource.reserve(source.size());
		for (char ch : source) {
			if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
				compactSource.push_back(ch);
			}
		}

		return compactSource;
	}

	inline bool SourceHasBaseClass(const std::string& compactSource, std::string_view baseClassName)
	{
		const std::string shortPattern = ":" + std::string(baseClassName);
		const std::string qualifiedPattern = ":Axiom." + std::string(baseClassName);
		const std::string globalPattern = ":global::Axiom." + std::string(baseClassName);
		return compactSource.find(shortPattern) != std::string::npos
			|| compactSource.find(qualifiedPattern) != std::string::npos
			|| compactSource.find(globalPattern) != std::string::npos;
	}

	inline bool SourceLooksLikeManagedComponent(const std::filesystem::path& filePath)
	{
		const std::string compactSource = ReadSourceWithoutWhitespace(filePath);
		return compactSource.find(":Component") != std::string::npos
			|| compactSource.find(":Axiom.Component") != std::string::npos
			|| compactSource.find(":global::Axiom.Component") != std::string::npos
			|| compactSource.find(":IComponent") != std::string::npos
			|| compactSource.find(":Axiom.Components.IComponent") != std::string::npos
			|| compactSource.find(":global::Axiom.Components.IComponent") != std::string::npos;
	}

	inline std::vector<std::string> ExtractRegisteredNativeScriptClasses(const std::filesystem::path& filePath)
	{
		std::ifstream input(filePath, std::ios::binary);
		if (!input) {
			return {};
		}

		const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		constexpr std::string_view token = "REGISTER_SCRIPT";

		std::vector<std::string> classes;
		std::size_t searchOffset = 0;
		while (true) {
			const std::size_t tokenPosition = source.find(token.data(), searchOffset, token.size());
			if (tokenPosition == std::string::npos) {
				break;
			}

			const std::size_t openParen = source.find('(', tokenPosition + token.size());
			if (openParen == std::string::npos) {
				break;
			}

			const std::size_t closeParen = source.find(')', openParen + 1);
			if (closeParen == std::string::npos) {
				break;
			}

			std::string className = TrimWhitespace(source.substr(openParen + 1, closeParen - openParen - 1));
			if (IsValidRegisteredScriptToken(className)
				&& std::find(classes.begin(), classes.end(), className) == classes.end()) {
				classes.push_back(std::move(className));
			}

			searchOffset = closeParen + 1;
		}

		return classes;
	}

	inline void CollectScriptFile(const std::filesystem::path& filePath, std::vector<ScriptEntry>& entries)
	{
		std::string extension = ToLowerCopy(filePath.extension().string());
		if (IsCSharpScriptExtension(extension)) {
			const std::string compactSource = ReadSourceWithoutWhitespace(filePath);
			const bool isManagedComponent = SourceHasBaseClass(compactSource, "Component")
				|| SourceHasBaseClass(compactSource, "IComponent")
				|| SourceHasBaseClass(compactSource, "Components.IComponent");
			const bool isGameSystem = SourceHasBaseClass(compactSource, "GameSystem");
			const bool isGlobalSystem = SourceHasBaseClass(compactSource, "GlobalSystem");
			AppendScriptEntry(entries, filePath.stem().string(), filePath, extension, ScriptType::Managed, isManagedComponent, isGameSystem, isGlobalSystem);
			return;
		}

		if (!IsNativeScriptSourceExtension(extension) || IsNativeScriptBootstrapFile(filePath)) {
			return;
		}

		for (const std::string& className : ExtractRegisteredNativeScriptClasses(filePath)) {
			AppendScriptEntry(entries, className, filePath, extension, ScriptType::Native);
		}
	}

	inline void CollectScriptFiles(const std::filesystem::path& directory, std::vector<ScriptEntry>& entries)
	{
		std::error_code ec;
		if (directory.empty() || !std::filesystem::exists(directory, ec) || ec) {
			return;
		}

		for (std::filesystem::recursive_directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec), end;
			 it != end;
			 it.increment(ec)) {
			if (ec) {
				ec.clear();
				continue;
			}

			if (!it->is_regular_file(ec) || ec) {
				ec.clear();
				continue;
			}

			CollectScriptFile(it->path(), entries);
		}
	}

	inline void SortScriptEntries(std::vector<ScriptEntry>& entries)
	{
		std::sort(entries.begin(), entries.end(), [](const ScriptEntry& a, const ScriptEntry& b) {
			if (a.ClassName != b.ClassName) {
				return a.ClassName < b.ClassName;
			}
			if (a.Type != b.Type) {
				return a.Type < b.Type;
			}
			return a.Path.string() < b.Path.string();
		});
	}

	// H23: per-frame cache. CollectProjectScriptEntries used to do a
	// full recursive directory walk on EVERY call — and several call
	// sites (ImGuiEditorLayer panels, AssetBrowser, ScriptComponentInspector)
	// invoke it once per frame. With more than a handful of scripts the
	// cost is visible.
	//
	// Cache strategy: a 1-second debounce with explicit MarkDirty(). The
	// debounce alone is sufficient to fold per-frame thrash within a tick;
	// MarkDirty is provided so file-mutation paths (script create / rename /
	// delete) can force an immediate rebuild. Atomic dirty flag + scoped
	// mutex keeps the cache coherent if a future caller invokes from a
	// worker thread.
	//
	// `inline` statics are guaranteed unique across translation units
	// (one symbol per program), so the cache is shared by every TU that
	// includes this header.
	namespace detail {
		inline std::vector<ScriptEntry> s_Cache;
		inline std::atomic<bool> s_Dirty{ true };
		inline std::chrono::steady_clock::time_point s_LastScan{};
		inline std::mutex s_CacheMutex;
	}

	inline void MarkDirty()
	{
		detail::s_Dirty.store(true, std::memory_order_release);
	}

	inline void CollectProjectScriptEntries(std::vector<ScriptEntry>& entries)
	{
		using namespace std::chrono;
		const auto now = steady_clock::now();

		{
			std::scoped_lock lock(detail::s_CacheMutex);
			const bool dirty = detail::s_Dirty.load(std::memory_order_acquire);
			const auto sinceLast = duration_cast<milliseconds>(now - detail::s_LastScan);
			if (!dirty && sinceLast < milliseconds(1000)) {
				entries.insert(entries.end(), detail::s_Cache.begin(), detail::s_Cache.end());
				return;
			}
		}

		// Rebuild outside the lock — directory iteration is the slow part
		// and we don't want to block other readers while it runs. The
		// cache write is short and re-takes the lock.
		std::vector<ScriptEntry> rebuilt;
		if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
			// Walk the whole Assets tree so .cs files placed outside the
			// historical Assets/Scripts/ folder still appear in the
			// "Add Script" picker. Matches the csproj's `Assets\**\*.cs`
			// compile glob — discovery and compilation share one root.
			CollectScriptFiles(std::filesystem::path(project->AssetsDirectory), rebuilt);
			CollectScriptFiles(std::filesystem::path(project->NativeSourceDir), rebuilt);
		}
		else {
			const std::filesystem::path base = std::filesystem::path(Path::ExecutableDir());
			CollectScriptFiles(base / ".." / ".." / ".." / "Axiom-Sandbox" / "Source", rebuilt);
			CollectScriptFiles(base / ".." / ".." / ".." / "Axiom-NativeScripts" / "Source", rebuilt);
		}
		SortScriptEntries(rebuilt);

		{
			std::scoped_lock lock(detail::s_CacheMutex);
			detail::s_Cache = rebuilt;
			detail::s_LastScan = now;
			detail::s_Dirty.store(false, std::memory_order_release);
		}

		entries.insert(entries.end(), rebuilt.begin(), rebuilt.end());
	}

} // namespace Axiom::EditorScriptDiscovery
