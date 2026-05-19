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

namespace Index::EditorScriptDiscovery {

	struct ScriptEntry {
		std::string ClassName;
		std::filesystem::path Path;
		std::string Extension;
		ScriptType Type = ScriptType::Unknown;
		// `IsManagedComponent` and `IsNativeComponent` are mutually exclusive
		// — a .cs file is either a managed class (`: Component`) hosted by the
		// ScriptComponent's managed slot, OR a native-backed struct
		// (`: IComponent`) that maps onto a C++ ECS pool. The two attach via
		// completely different paths (ScriptComponent::AddManagedComponent vs.
		// ComponentRegistry::AddWithDependencies), so the popup needs to tell
		// them apart.
		bool IsManagedComponent = false;
		bool IsNativeComponent = false;
		// Display name of the paired C++ component. For native-backed structs,
		// this is the value of `[NativeComponent("...")]` when present, or the
		// struct's class name as fallback. The Add Component popup uses this
		// to look up the matching ComponentInfo and to hide the C++ entry from
		// the regular General/Rendering/Physics/Audio categories.
		std::string NativeName;
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

	// Strips characters disallowed in C#/C++ identifiers and prefixes an
	// underscore if the result would start with a digit. Stricter than
	// IsValidRegisteredScriptToken: no ':' (this is for single class names,
	// not fully-qualified namespace tokens). Returns `fallback` when the
	// input is empty or sanitizes down to nothing.
	inline std::string SanitizeIdentifier(const std::string& input, const std::string& fallback)
	{
		std::string out;
		out.reserve(input.size());
		for (char ch : input) {
			const unsigned char uch = static_cast<unsigned char>(ch);
			if (std::isalnum(uch) != 0 || ch == '_') {
				out.push_back(ch);
			}
		}
		if (out.empty()) {
			return fallback;
		}
		if (std::isdigit(static_cast<unsigned char>(out[0])) != 0) {
			out.insert(out.begin(), '_');
		}
		return out;
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
		bool isNativeComponent = false,
		std::string nativeName = {},
		bool isGameSystem = false,
		bool isGlobalSystem = false)
	{
		if (className.empty() || type == ScriptType::Unknown || ContainsScriptEntry(entries, className, type)) {
			return;
		}

		ScriptEntry entry;
		entry.ClassName = className;
		entry.Path = path;
		entry.Extension = extension;
		entry.Type = type;
		entry.IsManagedComponent = isManagedComponent;
		entry.IsNativeComponent = isNativeComponent;
		entry.NativeName = std::move(nativeName);
		entry.IsGameSystem = isGameSystem;
		entry.IsGlobalSystem = isGlobalSystem;
		entries.push_back(std::move(entry));
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
		const std::string qualifiedPattern = ":Index." + std::string(baseClassName);
		const std::string globalPattern = ":global::Index." + std::string(baseClassName);
		return compactSource.find(shortPattern) != std::string::npos
			|| compactSource.find(qualifiedPattern) != std::string::npos
			|| compactSource.find(globalPattern) != std::string::npos;
	}

	inline bool SourceLooksLikeManagedComponent(const std::filesystem::path& filePath)
	{
		const std::string compactSource = ReadSourceWithoutWhitespace(filePath);
		return compactSource.find(":Component") != std::string::npos
			|| compactSource.find(":Index.Component") != std::string::npos
			|| compactSource.find(":global::Index.Component") != std::string::npos
			|| compactSource.find(":IComponent") != std::string::npos
			|| compactSource.find(":Index.Components.IComponent") != std::string::npos
			|| compactSource.find(":global::Index.Components.IComponent") != std::string::npos;
	}

	// Pulls the string argument out of `[NativeComponent("…")]` in the *raw*
	// source. Cannot use the whitespace-stripped buffer here — that path
	// collapses interior spaces inside string literals (so "New Native" would
	// become "NewNative" and never match the registered C++ display name).
	inline std::string ExtractNativeComponentAttributeName(const std::filesystem::path& filePath)
	{
		std::ifstream input(filePath, std::ios::binary);
		if (!input) {
			return {};
		}

		const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		constexpr std::string_view marker = "NativeComponent";
		std::size_t pos = 0;
		while ((pos = source.find(marker, pos)) != std::string::npos) {
			// Look back for the `[` that opens the attribute — skip
			// whitespace so `[ NativeComponent(...)]` is accepted. Bail if
			// the prior non-whitespace char isn't `[`; that filters out the
			// `using Index.Components` / `: IComponent` / interface-name
			// occurrences of the token "NativeComponent".
			std::size_t bracket = pos;
			while (bracket > 0 && std::isspace(static_cast<unsigned char>(source[bracket - 1]))) {
				--bracket;
			}
			if (bracket == 0 || source[bracket - 1] != '[') {
				pos += marker.size();
				continue;
			}

			std::size_t open = source.find('(', pos + marker.size());
			if (open == std::string::npos) return {};
			std::size_t quoteOpen = source.find('"', open);
			if (quoteOpen == std::string::npos) return {};
			std::size_t quoteClose = source.find('"', quoteOpen + 1);
			if (quoteClose == std::string::npos) return {};
			return TrimWhitespace(source.substr(quoteOpen + 1, quoteClose - quoteOpen - 1));
		}
		return {};
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
			// `: IComponent` (or qualified variants) means this .cs is a
			// native-backed struct paired with a C++ ECS pool. `: Component`
			// alone is a regular managed class hosted by the ScriptComponent.
			// The two are mutually exclusive — IComponent wins because the
			// substring check for ":Component" would otherwise also fire for
			// classes that happen to inherit from a base named Component.
			const bool isNativeComponent = SourceHasBaseClass(compactSource, "IComponent")
				|| SourceHasBaseClass(compactSource, "Components.IComponent");
			const bool isManagedComponent = !isNativeComponent
				&& (SourceHasBaseClass(compactSource, "Component")
					|| SourceHasBaseClass(compactSource, "Index.Component"));
			std::string nativeName;
			if (isNativeComponent) {
				nativeName = ExtractNativeComponentAttributeName(filePath);
				if (nativeName.empty()) {
					// Fallback to the struct name when no attribute hint is
					// provided. ComponentTypes<T>.ResolveNativeName tries the
					// same candidate, so this keeps the popup and the runtime
					// lookup in sync for the no-attribute case.
					nativeName = filePath.stem().string();
				}
			}
			const bool isGameSystem = SourceHasBaseClass(compactSource, "GameSystem");
			const bool isGlobalSystem = SourceHasBaseClass(compactSource, "GlobalSystem");
			AppendScriptEntry(entries, filePath.stem().string(), filePath, extension, ScriptType::Managed,
				isManagedComponent, isNativeComponent, std::move(nativeName),
				isGameSystem, isGlobalSystem);
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
		if (IndexProject* project = ProjectManager::GetCurrentProject()) {
			// Walk the whole Assets tree so .cs files placed outside the
			// historical Assets/Scripts/ folder still appear in the
			// "Add Script" picker. Matches the csproj's `Assets\**\*.cs`
			// compile glob — discovery and compilation share one root.
			CollectScriptFiles(std::filesystem::path(project->AssetsDirectory), rebuilt);
			CollectScriptFiles(std::filesystem::path(project->NativeSourceDir), rebuilt);
		}
		else {
			const std::filesystem::path base = std::filesystem::path(Path::ExecutableDir());
			CollectScriptFiles(base / ".." / ".." / ".." / "Index-Sandbox" / "Source", rebuilt);
			CollectScriptFiles(base / ".." / ".." / ".." / "Index-NativeScripts" / "Source", rebuilt);
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

} // namespace Index::EditorScriptDiscovery
