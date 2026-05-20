#include "pch.hpp"
#include "Project/IndexProject.hpp"
#include "Project/IndexBuildProfile.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/Directory.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/SceneSerializerShared.hpp"
#include "Utils/Process.hpp"
#include "Core/Log.hpp"
#include "Core/Version.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <random>

namespace Index {
	namespace {
		// Reject scene/icon strings that try to escape the project tree or carry
		// shell/OS-control characters. Defense-in-depth: GetSceneFilePath also
		// guards, but rejecting at load means a tampered index-project.json
		// can't poison BuildSceneList / StartupScene / LastOpenedScene either.
		bool IsValidSceneName(std::string_view value) {
			if (value.empty()) return false;
			if (value.front() == '.') return false;
			if (value.find("..") != std::string_view::npos) return false;
			for (char c : value) {
				if (c == '/' || c == '\\') return false;
				if (c == ':' || c == '\0') return false;
				if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') return false;
			}
			return true;
		}

		uint64_t ParseUInt64String(std::string_view value, uint64_t fallback = 0) {
			if (value.empty()) return fallback;
			try {
				size_t parsed = 0;
				uint64_t result = std::stoull(std::string(value), &parsed, 10);
				return parsed == value.size() ? result : fallback;
			}
			catch (...) {
				return fallback;
			}
		}

		void ReportCreateProgress(const IndexProject::CreateProgressCallback& callback, float progress, std::string_view stage) {
			if (callback) {
				callback(progress, stage);
			}
		}

		bool ToLocalTime(std::time_t value, std::tm& outTime) {
#if defined(_WIN32)
			return localtime_s(&outTime, &value) == 0;
#else
			return localtime_r(&value, &outTime) != nullptr;
#endif
		}

		std::string EscapeXml(const std::string& value) {
			std::string escaped;
			escaped.reserve(value.size());

			for (char c : value) {
				switch (c) {
				case '&':  escaped += "&amp;";  break;
				case '<':  escaped += "&lt;";   break;
				case '>':  escaped += "&gt;";   break;
				case '"':  escaped += "&quot;"; break;
				case '\'': escaped += "&apos;"; break;
				default:   escaped += c;        break;
				}
			}

			return escaped;
		}

		std::string MakeRelativePathOrEmpty(const std::filesystem::path& from, const std::filesystem::path& to) {
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(to, from, ec);
			if (ec) {
				return {};
			}

			return relative.generic_string();
		}

		std::filesystem::path ResolveEngineRoot() {
			const auto exeDir = std::filesystem::path(Path::ExecutableDir());
			// Dev-layout walks come first so that a developer running the
			// launcher out of bin/<cfg>-<plat>/Index-Launcher/ keeps using the
			// real source tree at ../../.. — its EngineSDK sibling is a
			// build-output artifact, not the source of truth, and using it
			// would break the engine-solution regen flow (no premake5.lua).
			// EngineSDK/ is the fallback for a zipped distribution where the
			// source tree is no longer reachable from the exe.
			const std::vector<std::filesystem::path> candidates = {
				exeDir / ".." / ".." / "..",
				exeDir / "..",
				exeDir / "EngineSDK"
			};

			for (const auto& candidate : candidates) {
				std::error_code ec;
				const std::filesystem::path canonicalCandidate = std::filesystem::weakly_canonical(candidate, ec);
				if (ec) {
					continue;
				}

				if (std::filesystem::exists(canonicalCandidate / "Index-Engine" / "src")
					&& std::filesystem::exists(canonicalCandidate / "Index-ScriptCore" / "Index-ScriptCore.csproj")) {
					return canonicalCandidate;
				}
			}

			return {};
		}

		std::string GetNativeLibraryFilename(const std::string& projectName) {
#if defined(_WIN32)
			return projectName + "-NativeScripts.dll";
#elif defined(__APPLE__)
			return "lib" + projectName + "-NativeScripts.dylib";
#else
			return "lib" + projectName + "-NativeScripts.so";
#endif
		}

		std::string GetNativeBuildArchitectureDirectory() {
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
			return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
			return "arm64";
#else
			return {};
#endif
		}

		std::string GetNativeBuildPlatformDirectory() {
			const std::string architectureDirectory = GetNativeBuildArchitectureDirectory();
			if (architectureDirectory.empty()) {
				return {};
			}

#if defined(IDX_PLATFORM_WINDOWS)
			return "windows-" + architectureDirectory;
#elif defined(IDX_PLATFORM_LINUX)
			return "linux-" + architectureDirectory;
#else
			return {};
#endif
		}

		std::vector<std::string> GetPreferredBuildConfigurations() {
			std::vector<std::string> configurations;
			configurations.reserve(3);

			auto appendUnique = [&configurations](std::string value) {
				if (value.empty()) {
					return;
				}

				if (std::find(configurations.begin(), configurations.end(), value) == configurations.end()) {
					configurations.push_back(std::move(value));
				}
			};

			appendUnique(IndexProject::GetActiveBuildConfiguration());
			appendUnique("Release");
			appendUnique("Debug");
			appendUnique("Dist");
			return configurations;
		}

		std::vector<std::filesystem::path> BuildScriptCoreArtifactDirectories(const std::filesystem::path& engineRoot) {
			std::vector<std::filesystem::path> directories;
			if (engineRoot.empty()) {
				return directories;
			}

			auto appendUnique = [&directories](std::filesystem::path path) {
				if (path.empty()) {
					return;
				}

				std::error_code ec;
				const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
				const std::filesystem::path key = ec ? path.lexically_normal() : normalized;
				if (std::find(directories.begin(), directories.end(), key) == directories.end()) {
					directories.push_back(key);
				}
			};

			// Packaged-SDK layout drops Index-ScriptCore.dll flat under
			// EngineSDK/Index-ScriptCore/ (no per-config bin tree). Check it
			// first; FindScriptCoreArtifact falls through to the dev candidates
			// below when the flat copy isn't present.
			appendUnique(engineRoot / "Index-ScriptCore");

			const std::string platformDirectory = GetNativeBuildPlatformDirectory();
			const auto preferredConfigurations = GetPreferredBuildConfigurations();
			for (const std::string& config : preferredConfigurations) {
				if (!platformDirectory.empty()) {
					appendUnique(engineRoot / "bin" / (config + "-" + platformDirectory) / "Index-ScriptCore");
				}
			}

			const std::filesystem::path binDirectory = engineRoot / "bin";
			std::error_code ec;
			if (std::filesystem::exists(binDirectory, ec) && std::filesystem::is_directory(binDirectory, ec)) {
				for (const auto& entry : std::filesystem::directory_iterator(binDirectory, ec)) {
					if (ec || !entry.is_directory()) {
						continue;
					}

					appendUnique(entry.path() / "Index-ScriptCore");
				}
			}

			return directories;
		}

		std::filesystem::path FindScriptCoreArtifact(const std::filesystem::path& engineRoot, const std::string& filename) {
			for (const auto& directory : BuildScriptCoreArtifactDirectories(engineRoot)) {
				const std::filesystem::path candidate = directory / filename;
				if (std::filesystem::exists(candidate)) {
					return candidate;
				}
			}

			return {};
		}

		std::string BuildNativeScriptExportsSource() {
			return R"(#include <Scripting/NativeScript.hpp>

#if defined(_WIN32)
#define INDEX_NATIVE_SCRIPT_EXPORT extern "C" __declspec(dllexport)
#elif defined(__GNUC__)
#define INDEX_NATIVE_SCRIPT_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define INDEX_NATIVE_SCRIPT_EXPORT extern "C"
#endif

INDEX_NATIVE_SCRIPT_EXPORT void IndexInitialize(void* engineAPI) {
	Index::g_EngineAPI = static_cast<Index::NativeEngineAPI*>(engineAPI);
}

INDEX_NATIVE_SCRIPT_EXPORT Index::NativeScript* IndexCreateScript(const char* className) {
	return Index::NativeScriptRegistry::Create(className);
}

INDEX_NATIVE_SCRIPT_EXPORT int IndexHasScript(const char* className) {
	return Index::NativeScriptRegistry::Has(className) ? 1 : 0;
}

INDEX_NATIVE_SCRIPT_EXPORT void IndexDestroyScript(Index::NativeScript* script) {
	delete script;
}
)";
		}

		void EnsureNativeScriptExportsFile(const IndexProject& project) {
			if (project.NativeSourceDir.empty()) {
				return;
			}

			const std::filesystem::path sourceDirectory = project.NativeSourceDir;
			std::error_code ec;
			std::filesystem::create_directories(sourceDirectory, ec);

			const std::filesystem::path exportsPath = sourceDirectory / "NativeScriptExports.cpp";
			if (std::filesystem::exists(exportsPath, ec)) {
				return;
			}

			(void)File::WriteAllText(exportsPath.string(), BuildNativeScriptExportsSource());
		}

		bool IsNativeScriptBootstrapFile(const std::filesystem::path& path) {
			return path.filename().string() == "NativeScriptExports.cpp";
		}

		bool IsNativeScriptSourceExtension(const std::filesystem::path& path) {
			std::string extension = path.extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});

			return extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx";
		}

		bool HasNativeScriptSourceFiles(const std::filesystem::path& sourceDirectory) {
			std::error_code ec;
			if (sourceDirectory.empty() || !std::filesystem::exists(sourceDirectory, ec) || ec) {
				return false;
			}

			for (std::filesystem::recursive_directory_iterator it(sourceDirectory, std::filesystem::directory_options::skip_permission_denied, ec), end;
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

				const std::filesystem::path path = it->path();
				if (IsNativeScriptBootstrapFile(path)) {
					continue;
				}

				if (IsNativeScriptSourceExtension(path)) {
					return true;
				}
			}

			return false;
		}

		std::string BuildNativeScriptCMakeLists(const IndexProject& project) {
			const std::filesystem::path engineRoot = ResolveEngineRoot();
			const std::string nativeIndexRootFallback = engineRoot.empty()
				? std::string()
				: MakeRelativePathOrEmpty(std::filesystem::path(project.NativeScriptsDir), engineRoot);

			// Empty when no custom defines — the placeholder substitution drops the line entirely.
			std::string customDefinesBlock;
			if (!project.CustomDefines.empty()) {
				customDefinesBlock = "target_compile_definitions(${PROJECT_NAME} PRIVATE";
				for (const std::string& d : project.CustomDefines) {
					if (d.empty()) continue;
					customDefinesBlock += " ";
					customDefinesBlock += d;
				}
				customDefinesBlock += ")";
			}

			std::string indexRootBootstrap = R"(set(INDEX_DIR "$ENV{INDEX_DIR}" CACHE PATH "Path to the Index repository root")
)";
			if (!nativeIndexRootFallback.empty()) {
				indexRootBootstrap += "if(NOT INDEX_DIR)\n";
				indexRootBootstrap += "    get_filename_component(INDEX_DIR \"${CMAKE_CURRENT_LIST_DIR}/" + nativeIndexRootFallback + "\" ABSOLUTE)\n";
				indexRootBootstrap += "endif()\n";
			}
			indexRootBootstrap += R"(
if(NOT INDEX_DIR OR NOT EXISTS "${INDEX_DIR}/Index-Engine/src")
    message(FATAL_ERROR "Index engine sources not found. Set INDEX_DIR to the Index repository root before configuring native scripts.")
endif()

)";

			std::string output = R"(cmake_minimum_required(VERSION 3.20)
project()" + project.Name + R"(-NativeScripts LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Dist" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_FLAGS_DIST "${CMAKE_CXX_FLAGS_RELEASE}" CACHE STRING "Flags used by the C++ compiler for Dist builds." FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DIST "${CMAKE_SHARED_LINKER_FLAGS_RELEASE}" CACHE STRING "Flags used by the shared linker for Dist builds." FORCE)

function(idx_normalize_native_arch out_var raw_arch)
    string(TOLOWER "${raw_arch}" raw_arch_lower)
    if(raw_arch_lower STREQUAL "amd64" OR raw_arch_lower STREQUAL "x86_64" OR raw_arch_lower STREQUAL "x64")
        set(${out_var} "x86_64" PARENT_SCOPE)
    elseif(raw_arch_lower STREQUAL "arm64" OR raw_arch_lower STREQUAL "aarch64")
        set(${out_var} "arm64" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "Unsupported native script architecture: ${raw_arch}")
    endif()
endfunction()

)" + indexRootBootstrap + R"(
file(GLOB_RECURSE NATIVE_SOURCES CONFIGURE_DEPENDS "Source/*.cpp" "Source/*.h" "Source/*.hpp")
add_library(${PROJECT_NAME} SHARED ${NATIVE_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${INDEX_DIR}/Index-Engine/src"
    "${INDEX_DIR}/External/spdlog/include"
    "${INDEX_DIR}/External/glm"
    "${INDEX_DIR}/External/entt/src"
    "${INDEX_DIR}/External/stb"
    "${INDEX_DIR}/External/magic_enum/include"
    "${INDEX_DIR}/External/cereal/include"
    "${INDEX_DIR}/External/glfw/include"
    "${INDEX_DIR}/External/glad/include"
    "${INDEX_DIR}/External/miniaudio"
    "${INDEX_DIR}/External/box2d/include"
    "${INDEX_DIR}/External/Axiom-Physics/include"
    "${INDEX_DIR}/External/dotnet"
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    INDEX_ALL_MODULES=1
    $<$<CONFIG:Debug>:IDX_DEBUG;_DEBUG>
    $<$<CONFIG:Release>:IDX_RELEASE;NDEBUG>
    $<$<CONFIG:Dist>:IDX_DIST;NDEBUG>
)

# INDEX_BUILD_DEVELOPMENT/RELEASE — mirrors the C# define so native scripts can use #ifdef.
if(NOT DEFINED INDEX_BUILD_PROFILE)
    set(INDEX_BUILD_PROFILE "DEVELOPMENT")
endif()
target_compile_definitions(${PROJECT_NAME} PRIVATE INDEX_BUILD_${INDEX_BUILD_PROFILE})

# Project custom defines — same list as C# <DefineConstants>, baked at save time.
)" + std::string(R"(@INDEX_PROJECT_CUSTOM_DEFINES_BLOCK@)") + R"(

if(WIN32)
    idx_normalize_native_arch(IDX_NATIVE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    set(IDX_NATIVE_PLATFORM "windows-${IDX_NATIVE_ARCH}")
    target_compile_definitions(${PROJECT_NAME} PRIVATE IDX_PLATFORM_WINDOWS)
elseif(UNIX AND NOT APPLE)
    idx_normalize_native_arch(IDX_NATIVE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    set(IDX_NATIVE_PLATFORM "linux-${IDX_NATIVE_ARCH}")
    target_compile_definitions(${PROJECT_NAME} PRIVATE IDX_PLATFORM_LINUX)
else()
    message(FATAL_ERROR "Unsupported platform for native scripts")
endif()

target_link_directories(${PROJECT_NAME} PRIVATE
    "$<$<CONFIG:Debug>:${INDEX_DIR}/bin/Debug-${IDX_NATIVE_PLATFORM}/Index-Engine>"
    "$<$<CONFIG:Release>:${INDEX_DIR}/bin/Release-${IDX_NATIVE_PLATFORM}/Index-Engine>"
    "$<$<CONFIG:Dist>:${INDEX_DIR}/bin/Dist-${IDX_NATIVE_PLATFORM}/Index-Engine>"
)
target_link_libraries(${PROJECT_NAME} PRIVATE Index-Engine)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()

set(NATIVE_OUTPUT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../bin")
foreach(config Debug Release Dist)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${NATIVE_OUTPUT_ROOT}/${config}-${IDX_NATIVE_PLATFORM}/${PROJECT_NAME}"
        LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${NATIVE_OUTPUT_ROOT}/${config}-${IDX_NATIVE_PLATFORM}/${PROJECT_NAME}"
    )
endforeach()
)";

			// Substitute placeholder; strip the line entirely when no defines were configured.
			const std::string placeholder = "@INDEX_PROJECT_CUSTOM_DEFINES_BLOCK@";
			const auto pos = output.find(placeholder);
			if (pos != std::string::npos) {
				if (customDefinesBlock.empty()) {
					std::size_t lineEnd = output.find('\n', pos);
					if (lineEnd == std::string::npos) lineEnd = output.size();
					else lineEnd += 1;
					output.erase(pos, lineEnd - pos);
				} else {
					output.replace(pos, placeholder.size(), customDefinesBlock);
				}
			}
			return output;
		}

		void EnsureNativeScriptCMakeListsFile(const IndexProject& project) {
			if (project.NativeScriptsDir.empty()) {
				return;
			}

			std::error_code ec;
			std::filesystem::create_directories(project.NativeScriptsDir, ec);

			const std::filesystem::path cmakeListsPath = std::filesystem::path(project.NativeScriptsDir) / "CMakeLists.txt";
			if (std::filesystem::exists(cmakeListsPath, ec)) {
				return;
			}

			(void)File::WriteAllText(cmakeListsPath.string(), BuildNativeScriptCMakeLists(project));
		}
	}

	static std::string GenerateGUID() {
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

		auto hex = [&](int bytes) {
			std::stringstream ss;
			ss << std::hex << std::setfill('0');
			for (int i = 0; i < bytes; i++)
				ss << std::setw(2) << (dist(gen) & 0xFF);
			return ss.str();
		};

		return hex(4) + "-" + hex(2) + "-" + hex(2) + "-" + hex(2) + "-" + hex(6);
	}

	static std::string NowISO8601() {
		auto t = std::time(nullptr);
		std::tm tm{};
		if (!ToLocalTime(t, tm)) {
			return {};
		}
		std::stringstream ss;
		ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
		return ss.str();
	}

	static void ResolvePaths(IndexProject& p) {
		p.AssetsDirectory = Path::Combine(p.RootDirectory, "Assets");
		p.ScriptsDirectory = Path::Combine(p.AssetsDirectory, "Scripts");
		p.ScenesDirectory = Path::Combine(p.AssetsDirectory, "Scenes");
		p.IndexAssetsDirectory = Path::Combine(p.RootDirectory, "IndexAssets");
		p.NativeScriptsDir = Path::Combine(p.RootDirectory, "NativeScripts");
		p.NativeSourceDir = Path::Combine(p.NativeScriptsDir, "Source");
		p.PackagesDirectory = Path::Combine(p.RootDirectory, "Packages");
		p.CsprojPath = Path::Combine(p.RootDirectory, p.Name + ".csproj");
		p.SlnPath = Path::Combine(p.RootDirectory, p.Name + ".sln");
		p.ProjectFilePath = Path::Combine(p.RootDirectory, "index-project.json");
	}

	static Json::Value BuildProjectJson(const IndexProject& project) {
		Json::Value root = Json::Value::MakeObject();
		root.AddMember("name", project.Name);
		root.AddMember("engineVersion", project.EngineVersion);
		root.AddMember("startupScene", project.StartupScene);
		root.AddMember("lastOpenedScene", project.LastOpenedScene);
		root.AddMember("gameViewAspect", project.GameViewAspect);
		root.AddMember("gameViewVsync", project.GameViewVsync);
		root.AddMember("buildWidth", project.BuildWidth);
		root.AddMember("buildHeight", project.BuildHeight);
		root.AddMember("buildFullscreen", project.BuildFullscreen);
		root.AddMember("buildResizable", project.BuildResizable);
		root.AddMember("uiReferenceWidth", project.UIReferenceWidth);
		root.AddMember("uiReferenceHeight", project.UIReferenceHeight);
		root.AddMember("uiScaleMatch", project.UIScaleMatch);
		if (!project.AppIconPath.empty()) {
			root.AddMember("appIcon", project.AppIconPath);
		}
		if (!project.ExecutableName.empty()) {
			root.AddMember("executableName", project.ExecutableName);
		}

		{
			Json::Value splash = Json::Value::MakeObject();
			splash.AddMember("enabled", project.SplashScreen.Enabled);
			splash.AddMember("durationSeconds", static_cast<double>(project.SplashScreen.DurationSeconds));
			splash.AddMember("fadeInSeconds", static_cast<double>(project.SplashScreen.FadeInSeconds));
			splash.AddMember("fadeOutSeconds", static_cast<double>(project.SplashScreen.FadeOutSeconds));
			if (!project.SplashScreen.ImagePath.empty()) {
				splash.AddMember("imagePath", project.SplashScreen.ImagePath);
			}
			if (!project.SplashScreen.BackgroundImagePath.empty()) {
				splash.AddMember("backgroundImagePath", project.SplashScreen.BackgroundImagePath);
			}
			if (!project.SplashScreen.CustomText.empty()) {
				splash.AddMember("customText", project.SplashScreen.CustomText);
			}
			splash.AddMember("backgroundR", static_cast<double>(project.SplashScreen.BackgroundR));
			splash.AddMember("backgroundG", static_cast<double>(project.SplashScreen.BackgroundG));
			splash.AddMember("backgroundB", static_cast<double>(project.SplashScreen.BackgroundB));
			splash.AddMember("fontColorR", static_cast<double>(project.SplashScreen.FontColorR));
			splash.AddMember("fontColorG", static_cast<double>(project.SplashScreen.FontColorG));
			splash.AddMember("fontColorB", static_cast<double>(project.SplashScreen.FontColorB));
			splash.AddMember("fontSize", static_cast<double>(project.SplashScreen.FontSize));
			root.AddMember("splashScreen", std::move(splash));
		}

		Json::Value buildScenes = Json::Value::MakeArray();
		for (const std::string& sceneName : project.BuildSceneList) {
			buildScenes.Append(sceneName);
		}
		root.AddMember("buildScenes", std::move(buildScenes));

		Json::Value globalSystems = Json::Value::MakeArray();
		for (const auto& registration : project.GlobalSystems) {
			if (registration.ClassName.empty()) {
				continue;
			}

			Json::Value systemValue = Json::Value::MakeObject();
			systemValue.AddMember("className", registration.ClassName);
			systemValue.AddMember("active", registration.Active);
			globalSystems.Append(std::move(systemValue));
		}
		root.AddMember("globalSystems", std::move(globalSystems));

		// Missing field == legacy "scan everything" default; emit only when allow-list is set.
		if (!project.Packages.empty()) {
			Json::Value packages = Json::Value::MakeArray();
			for (const std::string& name : project.Packages) {
				if (!name.empty()) {
					packages.Append(name);
				}
			}
			root.AddMember("packages", std::move(packages));
		}

		// Only written when non-default to keep virgin project files clean.
		const auto& prof = project.Profiler;
		const bool profilerNonDefault =
			prof.EnableInRuntime ||
			prof.TrackInBackground ||
			prof.SamplingHz != 60 ||
			prof.TrackingSpan != 200 ||
			!prof.ModuleEnabled.empty();
		if (profilerNonDefault) {
			Json::Value profilerJson = Json::Value::MakeObject();
			profilerJson.AddMember("enableInRuntime", prof.EnableInRuntime);
			profilerJson.AddMember("trackInBackground", prof.TrackInBackground);
			profilerJson.AddMember("samplingHz", prof.SamplingHz);
			profilerJson.AddMember("trackingSpan", prof.TrackingSpan);
			if (!prof.ModuleEnabled.empty()) {
				Json::Value modulesJson = Json::Value::MakeObject();
				for (const auto& [name, enabled] : prof.ModuleEnabled) {
					modulesJson.AddMember(name, enabled);
				}
				profilerJson.AddMember("modules", std::move(modulesJson));
			}
			root.AddMember("profiler", std::move(profilerJson));
		}

		if (!project.ShowRuntimeStats) {
			root.AddMember("showRuntimeStats", false);
		}
		if (!project.ShowRuntimeLogs) {
			root.AddMember("showRuntimeLogs", false);
		}
		if (!project.EnablePostProcessing) {
			root.AddMember("enablePostProcessing", false);
		}

		// autoSaveScenes / autoSaveIntervalSeconds / showFileExtensions
		// moved to user-scoped EditorPreferences (2026-05). Save() no
		// longer emits them; legacy values present in pre-migration
		// project files are loaded into LegacyEditorPrefs and dropped
		// from disk on this Save.
		if (!project.AutoRecompileScripts) {
			root.AddMember("autoRecompileScripts", false);
		}
		if (project.RecompileScriptsOnPlay) {
			root.AddMember("recompileScriptsOnPlay", true);
		}
		root.AddMember("assetSerializationFormat",
			std::string(IndexProject::ProjectAssetSerializationFormatToString(project.AssetSerializationFormat)));

		if (!project.EditorEnsureUniqueEntityNames) {
			root.AddMember("editorEnsureUniqueEntityNames", false);
		}
		if (project.EditorEntityNameSuffix != IndexProject::EditorEntityNameSuffixStyle::ParenthesizedNumber) {
			root.AddMember("editorEntityNameSuffix",
				std::string(IndexProject::EditorEntityNameSuffixStyleToString(project.EditorEntityNameSuffix)));
		}
		if (project.EditorAssetDuplicateSuffix != IndexProject::EditorEntityNameSuffixStyle::ParenthesizedNumber) {
			root.AddMember("editorAssetDuplicateSuffix",
				std::string(IndexProject::EditorEntityNameSuffixStyleToString(project.EditorAssetDuplicateSuffix)));
		}

		if (!project.CursorImagePath.empty()) {
			root.AddMember("cursorImagePath", project.CursorImagePath);
		}
		if (!project.UIInteractableCursorImagePath.empty()) {
			root.AddMember("uiInteractableCursorImagePath",
				project.UIInteractableCursorImagePath);
		}

		if (project.ActiveBuildProfile != IndexProject::BuildProfile::Development) {
			root.AddMember("buildProfile",
				std::string(IndexProject::BuildProfileToString(project.ActiveBuildProfile)));
		}
		// Preferred render backend — written only when non-default (Auto)
		// to keep index-project.json minimal for projects that haven't
		// touched the Graphics tab.
		if (project.ActiveRenderBackend != IndexProject::RenderBackend::Auto) {
			root.AddMember("renderBackend",
				std::string(IndexProject::RenderBackendToString(project.ActiveRenderBackend)));
		}
		// Empty == "no build profile selected" (the default). Skip the
		// field entirely in that case so legacy projects round-trip
		// cleanly through the new editor.
		if (!project.ActiveBuildProfileName.empty()) {
			root.AddMember("activeBuildProfile", project.ActiveBuildProfileName);
		}
		if (project.DefaultFontAssetId != k_DefaultFontAssetId) {
			root.AddMember("defaultFontAsset",
				std::to_string(project.DefaultFontAssetId));
		}
		if (!project.CustomDefines.empty()) {
			Json::Value definesJson = Json::Value::MakeArray();
			for (const std::string& d : project.CustomDefines) {
				if (!d.empty()) definesJson.Append(Json::Value(d));
			}
			root.AddMember("customDefines", std::move(definesJson));
		}
		if (project.EntityBits != 20) {
			root.AddMember("entityBits", project.EntityBits);
		}

		return root;
	}

	std::string IndexProject::GetUserAssemblyOutputPath(std::string_view configuration) const {
		const std::string resolvedConfiguration = configuration.empty()
			? GetActiveBuildConfiguration()
			: std::string(configuration);
		return Path::Combine(RootDirectory, "bin", resolvedConfiguration, Name + ".dll");
	}

	std::string IndexProject::GetActiveBuildConfiguration() {
		return IDX_BUILD_CONFIG_NAME;
	}

	std::string IndexProject::GetActiveBuildDefineConstant() {
#if defined(IDX_DEBUG)
		return "IDX_DEBUG";
#elif defined(IDX_RELEASE)
		return "IDX_RELEASE";
#elif defined(IDX_DIST)
		return "IDX_DIST";
#else
		return {};
#endif
	}

	std::string IndexProject::GetManagedPlatformDefine() {
#if defined(IDX_PLATFORM_WINDOWS)
		return "INDEX_WINDOWS";
#elif defined(IDX_PLATFORM_MACOS)
		return "INDEX_MACOS";
#elif defined(IDX_PLATFORM_LINUX)
		return "INDEX_LINUX";
#else
		return {};
#endif
	}

	std::string IndexProject::BuildManagedDefineConstants(std::string_view primarySymbol) {
		std::string defineConstants;
		if (!primarySymbol.empty()) {
			defineConstants.assign(primarySymbol);
		}

		const std::string buildDefine = GetActiveBuildDefineConstant();
		if (!buildDefine.empty()) {
			if (!defineConstants.empty()) {
				defineConstants += "%3B";
			}
			defineConstants += buildDefine;
		}

		// Editor hot-reload always uses Development; shipping defaults shouldn't bleed into editor preview.
		IndexProject* project = ProjectManager::GetCurrentProject();
		const std::string profileDefine = (primarySymbol == "INDEX_EDITOR")
			? std::string("INDEX_BUILD_DEVELOPMENT")
			: (project ? project->GetActiveBuildProfileDefine() : std::string("INDEX_BUILD_DEVELOPMENT"));
		if (!profileDefine.empty()) {
			if (!defineConstants.empty()) defineConstants += "%3B";
			defineConstants += profileDefine;
		}
		const std::string platformDefine = GetManagedPlatformDefine();
		if (!platformDefine.empty()) {
			if (!defineConstants.empty()) defineConstants += "%3B";
			defineConstants += platformDefine;
		}
		if (project) {
			for (const std::string& custom : project->CustomDefines) {
				if (custom.empty()) continue;
				if (!defineConstants.empty()) defineConstants += "%3B";
				defineConstants += custom;
			}
		}

		return defineConstants;
	}

	std::string IndexProject::GetActiveBuildProfileDefine() const {
		switch (ActiveBuildProfile) {
			case BuildProfile::Release:     return "INDEX_BUILD_RELEASE";
			case BuildProfile::Development: return "INDEX_BUILD_DEVELOPMENT";
		}
		return "INDEX_BUILD_DEVELOPMENT";
	}

	const char* IndexProject::BuildProfileToString(BuildProfile profile) {
		switch (profile) {
			case BuildProfile::Release:     return "Release";
			case BuildProfile::Development: return "Development";
		}
		return "Development";
	}

	IndexProject::BuildProfile IndexProject::BuildProfileFromString(std::string_view value) {
		if (value == "Release") return BuildProfile::Release;
		return BuildProfile::Development;
	}

	const char* IndexProject::RenderBackendToString(RenderBackend backend) {
		switch (backend) {
			case RenderBackend::Auto:       return "Auto";
			case RenderBackend::Vulkan:     return "Vulkan";
			case RenderBackend::Direct3D11: return "Direct3D11";
			case RenderBackend::Direct3D12: return "Direct3D12";
			case RenderBackend::OpenGL:     return "OpenGL";
			case RenderBackend::Metal:      return "Metal";
			case RenderBackend::OpenGLES:   return "OpenGLES";
		}
		return "Auto";
	}

	IndexProject::RenderBackend IndexProject::RenderBackendFromString(std::string_view value) {
		// Accept the canonical PascalCase token plus a few common
		// spellings so a hand-edited project file doesn't bite users
		// who type "vulkan" or "d3d11".
		if (value == "Vulkan"     || value == "vulkan"  || value == "VK")    return RenderBackend::Vulkan;
		if (value == "Direct3D11" || value == "DX11"    || value == "d3d11") return RenderBackend::Direct3D11;
		if (value == "Direct3D12" || value == "DX12"    || value == "d3d12") return RenderBackend::Direct3D12;
		if (value == "OpenGL"     || value == "opengl"  || value == "GL")    return RenderBackend::OpenGL;
		if (value == "Metal"      || value == "metal")                        return RenderBackend::Metal;
		if (value == "OpenGLES"   || value == "opengles" || value == "GLES")  return RenderBackend::OpenGLES;
		return RenderBackend::Auto;
	}

	const char* IndexProject::ProjectAssetSerializationFormatToString(ProjectAssetSerializationFormat format) {
		switch (format) {
			case ProjectAssetSerializationFormat::Binary: return "Binary";
			case ProjectAssetSerializationFormat::Json:   return "Json";
		}
		return "Binary";
	}

	IndexProject::ProjectAssetSerializationFormat IndexProject::ProjectAssetSerializationFormatFromString(std::string_view value) {
		if (value == "Binary" || value == "binary" || value == "bin") {
			return ProjectAssetSerializationFormat::Binary;
		}
		if (value == "Json" || value == "json") {
			return ProjectAssetSerializationFormat::Json;
		}
		return ProjectAssetSerializationFormat::Binary;
	}

	const char* IndexProject::EditorEntityNameSuffixStyleToString(EditorEntityNameSuffixStyle style) {
		switch (style) {
			case EditorEntityNameSuffixStyle::SpaceNumber:         return "SpaceNumber";
			case EditorEntityNameSuffixStyle::ParenthesizedNumber: return "ParenthesizedNumber";
			case EditorEntityNameSuffixStyle::HyphenNumber:        return "HyphenNumber";
			case EditorEntityNameSuffixStyle::UnderscoreNumber:    return "UnderscoreNumber";
		}
		return "ParenthesizedNumber";
	}

	IndexProject::EditorEntityNameSuffixStyle IndexProject::EditorEntityNameSuffixStyleFromString(std::string_view value) {
		if (value == "SpaceNumber" || value == "Entity 1" || value == "space") {
			return EditorEntityNameSuffixStyle::SpaceNumber;
		}
		if (value == "HyphenNumber" || value == "Entity-1" || value == "hyphen") {
			return EditorEntityNameSuffixStyle::HyphenNumber;
		}
		if (value == "UnderscoreNumber" || value == "Entity_1" || value == "underscore") {
			return EditorEntityNameSuffixStyle::UnderscoreNumber;
		}
		return EditorEntityNameSuffixStyle::ParenthesizedNumber;
	}

	std::string IndexProject::GetBuildProfilesDirectory() const {
		if (RootDirectory.empty()) {
			return {};
		}
		return Path::Combine(RootDirectory, "BuildProfiles");
	}

	std::string IndexProject::GetNativeDllPath() const {
		const std::string libraryFilename = GetNativeLibraryFilename(Name);
		const std::string buildConfig = IDX_BUILD_CONFIG_NAME;
		const std::string targetName = Name + "-NativeScripts";
		const std::string platformDirectory = GetNativeBuildPlatformDirectory();

		std::vector<std::filesystem::path> candidates;
		if (!platformDirectory.empty()) {
			candidates.emplace_back(std::filesystem::path(RootDirectory) / "bin" / (buildConfig + "-" + platformDirectory) / targetName / libraryFilename);
		}

		candidates.emplace_back(std::filesystem::path(NativeScriptsDir) / "build" / buildConfig / libraryFilename);
		candidates.emplace_back(std::filesystem::path(NativeScriptsDir) / "build" / libraryFilename);
		candidates.emplace_back(std::filesystem::path(NativeScriptsDir) / "build" / "Release" / libraryFilename);
		candidates.emplace_back(std::filesystem::path(NativeScriptsDir) / "build" / "Debug" / libraryFilename);
		candidates.emplace_back(std::filesystem::path(NativeScriptsDir) / "build" / "Dist" / libraryFilename);

		for (const auto& candidate : candidates) {
			if (std::filesystem::exists(candidate)) {
				auto normalizedCandidate = candidate;
				return normalizedCandidate.make_preferred().string();
			}
		}

		auto fallbackCandidate = candidates.front();
		return fallbackCandidate.make_preferred().string();
	}

	std::string IndexProject::GetSceneFilePath(const std::string& sceneName) const {
		// Defense in depth: even though Load() now validates these strings,
		// GetSceneFilePath is also called from runtime code paths that could
		// receive untrusted input. Reject anything that contains traversal
		// characters before we splice it into a filesystem path.
		if (!IsValidSceneName(sceneName)) {
			IDX_CORE_WARN_TAG("IndexProject",
				"Refusing to resolve scene path for unsafe name '{}'", sceneName);
			return {};
		}
		const std::filesystem::path assetsRoot(AssetsDirectory);
		const std::string sceneFileName = sceneName + ".scene";
		std::error_code ec;
		if (std::filesystem::exists(assetsRoot, ec) && !ec) {
			for (std::filesystem::recursive_directory_iterator it(
					 assetsRoot,
					 std::filesystem::directory_options::skip_permission_denied,
					 ec),
				 end;
				 it != end;
				 it.increment(ec)) {
				if (ec) {
					ec.clear();
					continue;
				}

				if (it->is_regular_file(ec) && !ec && it->path().filename().string() == sceneFileName) {
					auto path = it->path();
					return path.make_preferred().string();
				}
			}
		}

		return Path::Combine(ScenesDirectory, sceneFileName);
	}

	void IndexProject::EnsureNativeScriptBootstrapFiles() const {
		EnsureNativeScriptExportsFile(*this);
	}

	void IndexProject::EnsureNativeScriptProjectFiles() const {
		EnsureNativeScriptCMakeListsFile(*this);
		EnsureNativeScriptExportsFile(*this);
	}

	bool IndexProject::HasNativeScriptSources() const {
		return HasNativeScriptSourceFiles(std::filesystem::path(NativeSourceDir));
	}

	bool IndexProject::Save() const {
		const bool ok = File::WriteAllText(ProjectFilePath, Json::Stringify(BuildProjectJson(*this), true));
		// Keep the IntelliSense-visible defines file in sync with the project's
		// CustomDefines + ActiveBuildProfile every time the project is saved.
		// Failures are logged but non-fatal — a stale props file just means VS
		// shows the wrong active branches until the next save; runtime
		// compilation goes through BuildManagedDefineConstants regardless.
		(void)WriteManagedDefinesProps();
		return ok;
	}

	bool IndexProject::WriteManagedDefinesProps(bool* outAddedImport) const {
		if (outAddedImport) *outAddedImport = false;
		if (CsprojPath.empty()) return false;

		// Build the semicolon-separated define list. Mirrors
		// BuildManagedDefineConstants() but uses literal ';' (props files
		// don't need the MSBuild command-line %3B escape) and prepends
		// $(DefineConstants) so the SDK-default DEBUG/TRACE survives.
		//
		// INDEX_EDITOR (NOT INDEX_BUILD) here: this props file drives
		// Visual Studio IntelliSense, which represents the editor's
		// hot-reload compile context — `#if INDEX_EDITOR` blocks show
		// as active in VS and `#if INDEX_BUILD` blocks show as inactive,
		// mirroring what the editor's `dotnet build … -p:DefineConstants=
		// INDEX_EDITOR;…` invocation actually defines. Shipping builds
		// override DefineConstants on the command line (a global
		// property), so the props file is ignored at ship time.
		std::string defines = "$(DefineConstants);INDEX_EDITOR";
		const std::string profile = GetActiveBuildProfileDefine();
		if (!profile.empty()) {
			defines += ';';
			defines += profile;
		}
		const std::string platform = GetManagedPlatformDefine();
		if (!platform.empty()) {
			defines += ';';
			defines += platform;
		}
		for (const std::string& custom : CustomDefines) {
			if (custom.empty()) continue;
			defines += ';';
			defines += custom;
		}

		// Emit alongside Packages/IndexPackages.props so the same .csproj
		// import pattern works. Don't fail the save if the Packages dir
		// doesn't exist yet (project might be mid-create) — try to create it.
		const std::filesystem::path csprojDir =
			std::filesystem::path(CsprojPath).parent_path();
		const std::filesystem::path packagesDir = csprojDir / "Packages";
		std::error_code ec;
		std::filesystem::create_directories(packagesDir, ec);
		const std::filesystem::path propsPath = packagesDir / "IndexDefines.props";

		// Brief, hand-edited header so users who open it understand it
		// regenerates on every save.
		std::string contents =
			"<!-- Auto-generated by the Index editor on every project save. -->\n"
			"<!-- Edit Project Settings → Build Settings → Custom Defines instead. -->\n"
			"<Project>\n"
			"  <PropertyGroup>\n"
			"    <DefineConstants>" + defines + "</DefineConstants>\n"
			"  </PropertyGroup>\n"
			"</Project>\n";

		const bool wrote = File::WriteAllText(propsPath.string(), contents);
		if (!wrote) {
			IDX_CORE_WARN_TAG("IndexProject",
				"Failed to write {} — IntelliSense will not see custom defines.",
				propsPath.string());
			return false;
		}

		// Migration for projects created before this mechanism existed:
		// inject `<Import Project="Packages/IndexDefines.props" ... />` if
		// the .csproj doesn't already have it. Idempotent — looks for the
		// substring before patching.
		if (!File::Exists(CsprojPath)) return true;
		std::string csproj = File::ReadAllText(CsprojPath);
		if (csproj.find("Packages/IndexDefines.props") != std::string::npos
			|| csproj.find("Packages\\IndexDefines.props") != std::string::npos)
		{
			return true;
		}

		// Insert right after the IndexPackages.props import so both
		// Index-managed imports stay grouped. Fall back to inserting
		// before </Project> if the IndexPackages line isn't found.
		const std::string anchor = "Packages/IndexPackages.props";
		const size_t anchorPos = csproj.find(anchor);
		const std::string newImport =
			"\n  <Import Project=\"Packages/IndexDefines.props\" "
			"Condition=\"Exists('Packages/IndexDefines.props')\" />";

		bool patched = false;
		if (anchorPos != std::string::npos) {
			const size_t lineEnd = csproj.find('\n', anchorPos);
			if (lineEnd != std::string::npos) {
				csproj.insert(lineEnd, newImport);
				patched = true;
			}
		}
		if (!patched) {
			const size_t closeTag = csproj.rfind("</Project>");
			if (closeTag != std::string::npos) {
				csproj.insert(closeTag, newImport + "\n");
				patched = true;
			}
		}
		if (patched) {
			if (File::WriteAllText(CsprojPath, csproj)) {
				if (outAddedImport) *outAddedImport = true;
				IDX_CORE_INFO_TAG("IndexProject",
					"Migrated '{}' to import IndexDefines.props.", CsprojPath);
			}
		}
		return true;
	}

	std::string IndexProject::GetDefaultProjectsDir() {
		return Path::Combine(Path::GetSpecialFolderPath(SpecialFolder::User), "Index", "Projects");
	}

	bool IndexProject::IsValidProjectName(const std::string& name) {
		if (name.empty() || name.size() > 64) return false;
		if (name[0] == '.') return false;
		for (char c : name)
			if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
				return false;
		return true;
	}

	bool IndexProject::Validate(const std::string& rootDir) {
		return std::filesystem::exists(Path::Combine(rootDir, "index-project.json"))
			&& std::filesystem::exists(Path::Combine(rootDir, "Assets"));
	}

	IndexProject IndexProject::Load(const std::string& rootDir) {
		IndexProject project;
		project.RootDirectory = rootDir;

		std::string configPath = Path::Combine(rootDir, "index-project.json");
		if (File::Exists(configPath)) {
			std::string jsonText = File::ReadAllText(configPath);
			Json::Value root;
			std::string parseError;
			if (!jsonText.empty() && Json::TryParse(jsonText, root, &parseError) && root.IsObject()) {
				if (const Json::Value* nameValue = root.FindMember("name")) {
					project.Name = nameValue->AsStringOr();
				}
				if (const Json::Value* versionValue = root.FindMember("engineVersion")) {
					project.EngineVersion = versionValue->AsStringOr();
				}
				if (const Json::Value* startupValue = root.FindMember("startupScene")) {
					const std::string startupScene = startupValue->AsStringOr();
					if (!startupScene.empty()) {
						if (IsValidSceneName(startupScene)) {
							project.StartupScene = startupScene;
						}
						else {
							IDX_CORE_WARN_TAG("IndexProject",
								"Project '{}': ignoring invalid startupScene '{}'", configPath, startupScene);
						}
					}
				}
				if (const Json::Value* lastSceneValue = root.FindMember("lastOpenedScene")) {
					const std::string lastScene = lastSceneValue->AsStringOr();
					if (!lastScene.empty()) {
						if (IsValidSceneName(lastScene)) {
							project.LastOpenedScene = lastScene;
						}
						else {
							IDX_CORE_WARN_TAG("IndexProject",
								"Project '{}': ignoring invalid lastOpenedScene '{}'", configPath, lastScene);
						}
					}
				}
				if (const Json::Value* gameViewAspectValue = root.FindMember("gameViewAspect")) {
					const std::string gameViewAspect = gameViewAspectValue->AsStringOr();
					if (!gameViewAspect.empty()) {
						project.GameViewAspect = gameViewAspect;
					}
				}
				if (const Json::Value* gameViewVsyncValue = root.FindMember("gameViewVsync")) {
					project.GameViewVsync = gameViewVsyncValue->AsBoolOr(true);
				}
				if (const Json::Value* buildWidthValue = root.FindMember("buildWidth")) {
					project.BuildWidth = buildWidthValue->AsIntOr(1920);
				}
				if (const Json::Value* buildHeightValue = root.FindMember("buildHeight")) {
					project.BuildHeight = buildHeightValue->AsIntOr(1080);
				}
				if (const Json::Value* fullscreenValue = root.FindMember("buildFullscreen")) {
					project.BuildFullscreen = fullscreenValue->AsBoolOr(true);
				}
				if (const Json::Value* resizableValue = root.FindMember("buildResizable")) {
					project.BuildResizable = resizableValue->AsBoolOr(true);
				}
				if (const Json::Value* uiRefWidthValue = root.FindMember("uiReferenceWidth")) {
					project.UIReferenceWidth = std::max(1, uiRefWidthValue->AsIntOr(project.UIReferenceWidth));
				}
				if (const Json::Value* uiRefHeightValue = root.FindMember("uiReferenceHeight")) {
					project.UIReferenceHeight = std::max(1, uiRefHeightValue->AsIntOr(project.UIReferenceHeight));
				}
				if (const Json::Value* uiMatchValue = root.FindMember("uiScaleMatch")) {
					const float raw = static_cast<float>(uiMatchValue->AsDoubleOr(project.UIScaleMatch));
					project.UIScaleMatch = std::clamp(raw, 0.0f, 1.0f);
				}
				if (const Json::Value* iconValue = root.FindMember("appIcon")) {
					const std::string iconPath = iconValue->AsStringOr();
					if (iconPath.empty() || IsValidSceneName(iconPath)) {
						project.AppIconPath = iconPath;
					}
					else {
						IDX_CORE_WARN_TAG("IndexProject",
							"Project '{}': ignoring invalid appIcon '{}'", configPath, iconPath);
					}
				}
				if (const Json::Value* exeNameValue = root.FindMember("executableName")) {
					project.ExecutableName = exeNameValue->AsStringOr();
				}
				if (const Json::Value* splashValue = root.FindMember("splashScreen")) {
					if (const Json::Value* v = splashValue->FindMember("enabled")) {
						project.SplashScreen.Enabled = v->AsBoolOr(true);
					}
					if (const Json::Value* v = splashValue->FindMember("durationSeconds")) {
						project.SplashScreen.DurationSeconds = static_cast<float>(v->AsDoubleOr(2.5));
					}
					if (const Json::Value* v = splashValue->FindMember("fadeInSeconds")) {
						project.SplashScreen.FadeInSeconds = static_cast<float>(v->AsDoubleOr(0.5));
					}
					if (const Json::Value* v = splashValue->FindMember("fadeOutSeconds")) {
						project.SplashScreen.FadeOutSeconds = static_cast<float>(v->AsDoubleOr(0.5));
					}
					if (const Json::Value* v = splashValue->FindMember("imagePath")) {
						project.SplashScreen.ImagePath = v->AsStringOr();
					}
					if (const Json::Value* v = splashValue->FindMember("backgroundImagePath")) {
						project.SplashScreen.BackgroundImagePath = v->AsStringOr();
					}
					if (const Json::Value* v = splashValue->FindMember("customText")) {
						project.SplashScreen.CustomText = v->AsStringOr();
					}
					if (const Json::Value* v = splashValue->FindMember("backgroundR")) {
						project.SplashScreen.BackgroundR = static_cast<float>(v->AsDoubleOr(0.05));
					}
					if (const Json::Value* v = splashValue->FindMember("backgroundG")) {
						project.SplashScreen.BackgroundG = static_cast<float>(v->AsDoubleOr(0.05));
					}
					if (const Json::Value* v = splashValue->FindMember("backgroundB")) {
						project.SplashScreen.BackgroundB = static_cast<float>(v->AsDoubleOr(0.07));
					}
					if (const Json::Value* v = splashValue->FindMember("fontColorR")) {
						project.SplashScreen.FontColorR = static_cast<float>(v->AsDoubleOr(1.0));
					}
					if (const Json::Value* v = splashValue->FindMember("fontColorG")) {
						project.SplashScreen.FontColorG = static_cast<float>(v->AsDoubleOr(1.0));
					}
					if (const Json::Value* v = splashValue->FindMember("fontColorB")) {
						project.SplashScreen.FontColorB = static_cast<float>(v->AsDoubleOr(1.0));
					}
					if (const Json::Value* v = splashValue->FindMember("fontSize")) {
						project.SplashScreen.FontSize = static_cast<float>(v->AsDoubleOr(16.0));
					}
				}
				if (const Json::Value* buildScenesValue = root.FindMember("buildScenes")) {
					project.BuildSceneList.clear();
					// Mirror the packages-block pattern: degrade gracefully on
					// malformed input instead of asserting inside GetArray().
					if (!buildScenesValue->IsArray()) {
						IDX_CORE_WARN_TAG("IndexProject",
							"Project '{}': 'buildScenes' field is not a JSON array; ignoring.", configPath);
					}
					else for (const Json::Value& sceneValue : buildScenesValue->GetArray()) {
						const std::string sceneName = sceneValue.AsStringOr();
						if (sceneName.empty()) continue;
						if (!IsValidSceneName(sceneName)) {
							IDX_CORE_WARN_TAG("IndexProject",
								"Project '{}': skipping invalid buildScenes entry '{}'", configPath, sceneName);
							continue;
						}
						project.BuildSceneList.push_back(sceneName);
					}
				}
				if (const Json::Value* globalSystemsValue = root.FindMember("globalSystems")) {
					project.GlobalSystems.clear();
					if (!globalSystemsValue->IsArray()) {
						IDX_CORE_WARN_TAG("IndexProject",
							"Project '{}': 'globalSystems' field is not a JSON array; ignoring.", configPath);
					}
					else for (const Json::Value& systemValue : globalSystemsValue->GetArray()) {
						if (systemValue.IsString()) {
							const std::string className = systemValue.AsStringOr();
							if (!className.empty()) {
								project.GlobalSystems.push_back({ className, true });
							}
							continue;
						}

						if (!systemValue.IsObject()) {
							continue;
						}

						const Json::Value* classNameValue = systemValue.FindMember("className");
						const std::string className = classNameValue ? classNameValue->AsStringOr() : "";
						if (className.empty()) {
							continue;
						}

						const Json::Value* activeValue = systemValue.FindMember("active");
						project.GlobalSystems.push_back({ className, activeValue ? activeValue->AsBoolOr(true) : true });
					}
				}
				if (const Json::Value* packagesValue = root.FindMember("packages")) {
					project.Packages.clear();
					// Skip + warn on malformed input rather than asserting in Json::Value::GetArray.
					if (packagesValue->IsArray()) {
						for (const Json::Value& packageValue : packagesValue->GetArray()) {
							const std::string packageName = packageValue.AsStringOr();
							if (!packageName.empty()) {
								project.Packages.push_back(packageName);
							}
						}
					}
					else {
						IDX_CORE_WARN_TAG("IndexProject",
							"Project '{}': 'packages' field is not a JSON array; ignoring.", configPath);
					}
				}
				if (const Json::Value* profilerValue = root.FindMember("profiler")) {
					if (const Json::Value* v = profilerValue->FindMember("enableInRuntime"))
						project.Profiler.EnableInRuntime = v->AsBoolOr(false);
					if (const Json::Value* v = profilerValue->FindMember("trackInBackground"))
						project.Profiler.TrackInBackground = v->AsBoolOr(false);
					if (const Json::Value* v = profilerValue->FindMember("samplingHz"))
						project.Profiler.SamplingHz = static_cast<int>(v->AsInt64Or(60));
					if (const Json::Value* v = profilerValue->FindMember("trackingSpan"))
						project.Profiler.TrackingSpan = static_cast<int>(v->AsInt64Or(200));
					if (const Json::Value* modulesValue = profilerValue->FindMember("modules")) {
						project.Profiler.ModuleEnabled.clear();
						if (!modulesValue->IsObject()) {
							IDX_CORE_WARN_TAG("IndexProject",
								"Project '{}': 'profiler.modules' is not a JSON object; ignoring.", configPath);
						}
						else for (const auto& [name, value] : modulesValue->GetObject()) {
							project.Profiler.ModuleEnabled.emplace_back(
								std::string(name), value.AsBoolOr(true));
						}
					}
				}
				if (const Json::Value* v = root.FindMember("showRuntimeStats"))
					project.ShowRuntimeStats = v->AsBoolOr(true);
				if (const Json::Value* v = root.FindMember("showRuntimeLogs"))
					project.ShowRuntimeLogs = v->AsBoolOr(true);
				if (const Json::Value* v = root.FindMember("enablePostProcessing"))
					project.EnablePostProcessing = v->AsBoolOr(true);
				// autoSaveScenes / autoSaveIntervalSeconds migrated to
				// user-scoped EditorPreferences. We still parse the keys
				// here so legacy projects can hand the values off (via
				// LegacyEditorPrefs) before they drop from the project
				// file on the next Save.
				if (const Json::Value* v = root.FindMember("autoSaveScenes")) {
					project.LegacyEditorPrefs.AutoSaveScenes = v->AsBoolOr(false);
					project.LegacyEditorPrefs.AutoSaveScenesPresent = true;
				}
				if (const Json::Value* v = root.FindMember("autoSaveIntervalSeconds")) {
					project.LegacyEditorPrefs.AutoSaveIntervalSeconds =
						static_cast<float>(v->AsDoubleOr(120.0));
					project.LegacyEditorPrefs.AutoSaveIntervalSecondsPresent = true;
				}
				if (const Json::Value* v = root.FindMember("autoRecompileScripts"))
					project.AutoRecompileScripts = v->AsBoolOr(true);
				if (const Json::Value* v = root.FindMember("recompileScriptsOnPlay"))
					project.RecompileScriptsOnPlay = v->AsBoolOr(false);
				if (const Json::Value* v = root.FindMember("assetSerializationFormat"))
					project.AssetSerializationFormat = IndexProject::ProjectAssetSerializationFormatFromString(
						v->AsStringOr("Binary"));
				if (const Json::Value* v = root.FindMember("showFileExtensions")) {
					project.LegacyEditorPrefs.ShowFileExtensions = v->AsBoolOr(false);
					project.LegacyEditorPrefs.ShowFileExtensionsPresent = true;
				}
				if (const Json::Value* v = root.FindMember("editorEnsureUniqueEntityNames"))
					project.EditorEnsureUniqueEntityNames = v->AsBoolOr(true);
				if (const Json::Value* v = root.FindMember("editorEntityNameSuffix"))
					project.EditorEntityNameSuffix = IndexProject::EditorEntityNameSuffixStyleFromString(
						v->AsStringOr("ParenthesizedNumber"));
				if (const Json::Value* v = root.FindMember("editorAssetDuplicateSuffix"))
					project.EditorAssetDuplicateSuffix = IndexProject::EditorEntityNameSuffixStyleFromString(
						v->AsStringOr("ParenthesizedNumber"));
				if (const Json::Value* v = root.FindMember("cursorImagePath"))
					project.CursorImagePath = v->AsStringOr();
				if (const Json::Value* v = root.FindMember("uiInteractableCursorImagePath"))
					project.UIInteractableCursorImagePath = v->AsStringOr();
				if (const Json::Value* v = root.FindMember("buildProfile"))
					project.ActiveBuildProfile = IndexProject::BuildProfileFromString(v->AsStringOr("Development"));
				if (const Json::Value* v = root.FindMember("renderBackend"))
					project.ActiveRenderBackend = IndexProject::RenderBackendFromString(v->AsStringOr("Auto"));
				if (const Json::Value* v = root.FindMember("activeBuildProfile"))
					project.ActiveBuildProfileName = v->AsStringOr();
				if (const Json::Value* v = root.FindMember("defaultFontAsset")) {
					project.DefaultFontAssetId = v->IsString()
						? ParseUInt64String(v->AsStringOr(), k_DefaultFontAssetId)
						: v->AsUInt64Or(k_DefaultFontAssetId);
					if (project.DefaultFontAssetId == 0) {
						project.DefaultFontAssetId = k_DefaultFontAssetId;
					}
				}
				if (const Json::Value* v = root.FindMember("customDefines"); v && v->IsArray()) {
					project.CustomDefines.clear();
					for (const Json::Value& item : v->GetArray()) {
						if (item.IsString()) {
							std::string s = item.AsStringOr();
							if (!s.empty()) project.CustomDefines.push_back(std::move(s));
						}
					}
				}
				if (const Json::Value* v = root.FindMember("entityBits")) {
					const int bits = v->AsIntOr(20);
					if (bits == 16 || bits == 20 || bits == 22 || bits == 24 || bits == 28) {
						project.EntityBits = bits;
					}
					else {
						IDX_CORE_WARN_TAG("IndexProject",
							"index-project.json: entityBits={} is not one of {{16, 20, 22, 24, 28}}; falling back to 20.",
							bits);
						project.EntityBits = 20;
					}
				}
			}
			else if (!jsonText.empty()) {
				IDX_CORE_WARN_TAG("IndexProject", "Failed to parse '{}': {}", configPath, parseError);
			}
		}

		if (project.Name.empty())
			project.Name = std::filesystem::path(rootDir).filename().string();
		if (project.EngineVersion.empty())
			project.EngineVersion = IDX_VERSION;

		ResolvePaths(project);

		// One-shot migration: projects created before the Assets-wide compile
		// glob shipped have their csproj locked to "Assets\Scripts\**\*.cs",
		// so .cs files placed elsewhere under Assets/ silently fail to compile.
		// Rewrite the narrow glob to the broad one in place. The check is an
		// exact string match so it stays a no-op once migrated and won't
		// disturb users who deliberately scoped their compile glob.
		if (File::Exists(project.CsprojPath)) {
			std::string csproj = File::ReadAllText(project.CsprojPath);
			constexpr std::string_view oldGlob = "<Compile Include=\"Assets\\Scripts\\**\\*.cs\" />";
			constexpr std::string_view newGlob = "<Compile Include=\"Assets\\**\\*.cs\" />";
			const size_t pos = csproj.find(oldGlob);
			if (pos != std::string::npos) {
				csproj.replace(pos, oldGlob.size(), newGlob);
				if (File::WriteAllText(project.CsprojPath, csproj)) {
					IDX_CORE_INFO_TAG("IndexProject", "Migrated csproj compile glob to Assets\\**\\*.cs in '{}'", project.CsprojPath);
				}
			}
		}

		// Refresh IndexDefines.props so the file on disk matches what the
		// project just deserialised. This both writes the props file (if
		// missing) and patches the .csproj to import it (one-shot migration
		// for projects pre-dating the mechanism). Save() also calls this,
		// but doing it here too means a fresh open already shows the right
		// active branches in VS without requiring an explicit save first.
		(void)project.WriteManagedDefinesProps();

		return project;
	}

	void CreateDirectoryTree(const IndexProject& project) {
		Directory::Create(project.RootDirectory);
		Directory::Create(project.AssetsDirectory);
		Directory::Create(project.ScriptsDirectory);
		Directory::Create(project.ScenesDirectory);
		Directory::Create(project.IndexAssetsDirectory);
		Directory::Create(project.NativeScriptsDir);
		Directory::Create(project.NativeSourceDir);
		Directory::Create(project.PackagesDirectory);
		Directory::Create(project.GetBuildProfilesDirectory());
		Directory::Create(Path::Combine(project.RootDirectory, "bin"));
		Directory::Create(Path::Combine(project.RootDirectory, ".vscode"));
	}

	void ConfigurePackages(const IndexProject& project) {

		const std::filesystem::path engineRoot = ResolveEngineRoot();
		const std::filesystem::path scriptCoreOutput = FindScriptCoreArtifact(engineRoot, "Index-ScriptCore.dll");
		const std::filesystem::path scriptCorePdb = FindScriptCoreArtifact(engineRoot, "Index-ScriptCore.pdb");

		std::filesystem::path packageDir = std::filesystem::path(project.PackagesDirectory) / "Index-ScriptCore";
		std::filesystem::create_directories(packageDir);

		// Atomic-ish install: if the PDB copy fails after the DLL copy succeeds we
		// leave a half-installed package on disk that "looks" valid to the next
		// launch but has no symbols. Wrap both copies; on failure, remove the
		// package directory so the next ConfigurePackages call starts clean.
		try {
			if (std::filesystem::exists(scriptCoreOutput))
				std::filesystem::copy_file(scriptCoreOutput, packageDir / "Index-ScriptCore.dll", std::filesystem::copy_options::overwrite_existing);

			if (std::filesystem::exists(scriptCorePdb))
				std::filesystem::copy_file(scriptCorePdb, packageDir / "Index-ScriptCore.pdb", std::filesystem::copy_options::overwrite_existing);
		}
		catch (const std::filesystem::filesystem_error& ex) {
			IDX_CORE_ERROR_TAG("IndexProject",
				"ConfigurePackages: copy failed ({}). Rolling back partial install at '{}'.",
				ex.what(), packageDir.string());
			std::error_code ec;
			std::filesystem::remove_all(packageDir, ec);
			throw;
		}
	}

	IndexProject IndexProject::Create(const std::string& name, const std::string& parentDir,
		const CreateProgressCallback& progressCallback) {
		IndexProject project;
		project.Name = name;
		project.RootDirectory = Path::Combine(parentDir, name);
		project.EngineVersion = IDX_VERSION;
		ResolvePaths(project);

		ReportCreateProgress(progressCallback, 0.08f, "Creating project folders...");
		CreateDirectoryTree(project);
		ReportCreateProgress(progressCallback, 0.18f, "Configuring engine packages...");
		ConfigurePackages(project);
		ReportCreateProgress(progressCallback, 0.20f, "Creating default build profiles...");
		IndexBuildProfile::WriteDefaultProfiles(project.GetBuildProfilesDirectory());

		std::string engineAssets = Path::ResolveIndexAssets("");
		if (!engineAssets.empty() && Directory::Exists(engineAssets)) {
			ReportCreateProgress(progressCallback, 0.34f, "Copying Index assets...");
			try {
				std::filesystem::copy(engineAssets, project.IndexAssetsDirectory,
					std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing);
			}
			catch (const std::exception& ex) {
				IDX_CORE_WARN_TAG("IndexProject", "Failed to copy Index assets from '{}' to '{}': {}",
					engineAssets, project.IndexAssetsDirectory, ex.what());
			}
			catch (...) {
				IDX_CORE_WARN_TAG("IndexProject", "Failed to copy Index assets from '{}' to '{}': unknown error",
					engineAssets, project.IndexAssetsDirectory);
			}
		}

		if (project.BuildSceneList.empty()) {
			project.BuildSceneList.push_back(project.StartupScene);
		}

		// Write index-project.json
		{
			ReportCreateProgress(progressCallback, 0.48f, "Writing project configuration...");
			Json::Value root = BuildProjectJson(project);
			// Note: 'createdAt' is intentionally not written. Load() never reads it back, and
			// BuildProjectJson never re-emits it, so writing it here would be silently dropped
			// on the first Save() and create the illusion of persistent provenance metadata.
			(void)File::WriteAllText(project.ProjectFilePath, Json::Stringify(root, true));
		}

		// Generate starter scene
		{
			ReportCreateProgress(progressCallback, 0.56f, "Generating starter scene...");
			Json::Value sceneRoot = Json::Value::MakeObject();
			sceneRoot.AddMember("version", 1);
			sceneRoot.AddMember("name", project.StartupScene);
			sceneRoot.AddMember("systems", Json::Value::MakeArray());
			sceneRoot.AddMember("entities", Json::Value::MakeArray());
			const SceneSerializationFormat starterFormat =
				project.AssetSerializationFormat == IndexProject::ProjectAssetSerializationFormat::Binary
					? SceneSerializationFormat::Binary
					: SceneSerializationFormat::Json;
			(void)SceneSerializerStorage::WriteRootToFile(
				project.GetSceneFilePath(project.StartupScene),
				sceneRoot,
				starterFormat);
		}

		// Preserve existing NuGet PackageReference entries if .csproj already exists
		std::string existingPackageRefs;
		if (File::Exists(project.CsprojPath)) {
			std::ifstream existing(project.CsprojPath);
			std::string content((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
			existing.close();

			// Extract all ItemGroup blocks that contain PackageReference
			size_t searchPos = 0;
			while (searchPos < content.size()) {
				size_t igStart = content.find("<ItemGroup>", searchPos);
				if (igStart == std::string::npos) break;
				size_t igEnd = content.find("</ItemGroup>", igStart);
				if (igEnd == std::string::npos) break;
				igEnd += 12; // length of "</ItemGroup>"

				std::string block = content.substr(igStart, igEnd - igStart);
				const bool referencesScriptCore = block.find("Index-ScriptCore") != std::string::npos;
				if (block.find("PackageReference") != std::string::npos
					|| (block.find("<Reference ") != std::string::npos && !referencesScriptCore)
					|| (block.find("ProjectReference") != std::string::npos && !referencesScriptCore)) {
					existingPackageRefs += "  " + block + "\n";
				}
				searchPos = igEnd;
			}
		}

		ReportCreateProgress(progressCallback, 0.66f, "Generating C# project files...");
		std::string csproj = R"CS(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Library</OutputType>
    <TargetFramework>net9.0</TargetFramework>
    <Configurations>Debug;Release;Dist</Configurations>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <EnableDynamicLoading>true</EnableDynamicLoading>
    <CopyLocalLockFileAssemblies>true</CopyLocalLockFileAssemblies>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
    <EnableDefaultNoneItems>false</EnableDefaultNoneItems>
    <Nullable>enable</Nullable>
    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>
  </PropertyGroup>

  <PropertyGroup Condition="'$(Configuration)' == 'Debug'">
    <OutputPath>bin\Debug\</OutputPath>
  </PropertyGroup>

  <PropertyGroup Condition="'$(Configuration)' == 'Release'">
    <OutputPath>bin\Release\</OutputPath>
    <Optimize>true</Optimize>
  </PropertyGroup>

  <PropertyGroup Condition="'$(Configuration)' == 'Dist'">
    <OutputPath>bin\Dist\</OutputPath>
    <Optimize>true</Optimize>
  </PropertyGroup>

  <ItemGroup>
    <Compile Include="Assets\**\*.cs" />
  </ItemGroup>

  <ItemGroup>
    <Reference Include="Index-ScriptCore">
      <HintPath>Packages\Index-ScriptCore\Index-ScriptCore.dll</HintPath>
      <Private>true</Private>
    </Reference>
  </ItemGroup>

  <!-- Auto-generated by Index Package Manager every time a package is installed/removed. -->
  <Import Project="Packages/IndexPackages.props" Condition="Exists('Packages/IndexPackages.props')" />
  <!-- Auto-generated on every project save with CustomDefines + active build profile. -->
  <Import Project="Packages/IndexDefines.props" Condition="Exists('Packages/IndexDefines.props')" />
)CS" + existingPackageRefs + R"CS(
</Project>
)CS";
		(void)File::WriteAllText(project.CsprojPath, csproj);

		// Index-ScriptCore is linked via DLL reference in the .csproj — no project entry needed.
		ReportCreateProgress(progressCallback, 0.72f, "Generating solution and starter scripts...");
		std::string projGuid = GenerateGUID();

		std::string sln = R"(
Microsoft Visual Studio Solution File, Format Version 12.00
# Visual Studio Version 17
Project("{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}") = ")" + name + R"(", ")" + name + R"(.csproj", "{)" + projGuid + R"(}"
EndProject
Global
	GlobalSection(SolutionConfigurationPlatforms) = preSolution
		Debug|Any CPU = Debug|Any CPU
		Release|Any CPU = Release|Any CPU
		Dist|Any CPU = Dist|Any CPU
	EndGlobalSection
	GlobalSection(ProjectConfigurationPlatforms) = postSolution
		{)" + projGuid + R"(}.Debug|Any CPU.ActiveCfg = Debug|AnyCPU
		{)" + projGuid + R"(}.Debug|Any CPU.Build.0 = Debug|AnyCPU
		{)" + projGuid + R"(}.Release|Any CPU.ActiveCfg = Release|AnyCPU
		{)" + projGuid + R"(}.Release|Any CPU.Build.0 = Release|AnyCPU
		{)" + projGuid + R"(}.Dist|Any CPU.ActiveCfg = Dist|AnyCPU
		{)" + projGuid + R"(}.Dist|Any CPU.Build.0 = Dist|AnyCPU
	EndGlobalSection
	GlobalSection(SolutionProperties) = preSolution
		HideSolutionNode = FALSE
	EndGlobalSection
EndGlobal
)";
		(void)File::WriteAllText(project.SlnPath, sln);

		// Generate starter script
		std::string starterScript = R"(using Index;

public class GameScript : EntityScript
{
    public override void OnStart()
    {
        Log.Info("Hello from )" + name + R"(!");
    }

    public override void OnUpdate()
    {
    }
}
)";
		(void)File::WriteAllText(Path::Combine(project.ScriptsDirectory, "GameScript.cs"), starterScript);

		// Generate .vscode/settings.json
		std::string vsCodeSettings = R"({
    "omnisharp.projectFilesExcludePattern": [],
    "dotnet.defaultSolution": ")" + name + R"(.sln"
})";
		(void)File::WriteAllText(Path::Combine(project.RootDirectory, ".vscode", "settings.json"), vsCodeSettings);

		project.EnsureNativeScriptProjectFiles();

		// Generate .gitignore
		(void)File::WriteAllText(Path::Combine(project.RootDirectory, ".gitignore"),
			"bin/\nobj/\n.vs/\n*.user\nNativeScripts/build/\nPackages/\n");

		ReportCreateProgress(progressCallback, 0.82f, "Project files ready.");
		IDX_INFO_TAG("IndexProject", "Created project: {} at {}", name, project.RootDirectory);
		return project;
	}

	std::string IndexProject::GetEngineRootDir() {
		const std::filesystem::path root = ResolveEngineRoot();
		return root.empty() ? std::string{} : root.generic_string();
	}

	std::string IndexProject::GetPremakePath() {
		const std::filesystem::path engineRoot = ResolveEngineRoot();
		if (engineRoot.empty()) {
			return {};
		}

#if defined(_WIN32)
		const char* const exeName = "premake5.exe";
#else
		const char* const exeName = "premake5";
#endif

		// Dev layout vendors premake under vendor/bin/. Packaged SDK ships it
		// flat at the SDK root because vendor/ is not redistributed.
		const std::vector<std::filesystem::path> candidates = {
			engineRoot / "vendor" / "bin" / exeName,
			engineRoot / exeName,
		};

		for (const auto& candidate : candidates) {
			std::error_code ec;
			if (std::filesystem::exists(candidate, ec) && !ec) {
				return candidate.generic_string();
			}
		}
		return {};
	}

	IndexProject::RegenerateResult IndexProject::RegenerateSolutionForProject(const std::string& projectRootDir) {
		RegenerateResult result;

		const std::string engineRoot = GetEngineRootDir();
		if (engineRoot.empty()) {
			result.Output = "Could not resolve engine root directory; skipping solution regeneration.";
			IDX_WARN_TAG("IndexProject", "{}", result.Output);
			return result;
		}

		// Packaged-SDK distributions ship headers + Index-Engine.lib but no
		// premake5.lua, because regenerating the engine solution requires the
		// full engine source tree. Treat this as a successful no-op rather
		// than an error — the user's project still builds via its own
		// generated CMakeLists.txt against the bundled SDK.
		std::error_code premakeLuaEc;
		const std::filesystem::path premakeLua = std::filesystem::path(engineRoot) / "premake5.lua";
		if (!std::filesystem::exists(premakeLua, premakeLuaEc) || premakeLuaEc) {
			result.Succeeded = true;
			result.Output = "Engine solution regeneration skipped: running against a packaged SDK (no premake5.lua at engine root).";
			IDX_INFO_TAG("IndexProject", "{}", result.Output);
			return result;
		}

		const std::string premakePath = GetPremakePath();
		if (premakePath.empty()) {
			result.Output = "premake5 binary not found under <engine-root>/vendor/bin/. The engine solution cannot be regenerated automatically; the existing one will be used as-is.";
			IDX_WARN_TAG("IndexProject", "{}", result.Output);
			return result;
		}

		IDX_INFO_TAG("IndexProject", "Regenerating engine solution for project: {}", projectRootDir);

		Process::Result processResult = Process::Run({
			premakePath,
			"vs2022",
			"--index-project=" + projectRootDir,
		}, engineRoot);

		result.ExitCode = processResult.ExitCode;
		result.Succeeded = processResult.Succeeded();
		result.Output = std::move(processResult.Output);

		if (!result.Succeeded) {
			IDX_ERROR_TAG("IndexProject", "premake regeneration failed (exit code {}): {}", result.ExitCode, result.Output);
		}
		return result;
	}

	std::string IndexProject::GetMSBuildPath() {
#if defined(_WIN32)
		const std::vector<std::string> programFilesRoots = {
			"C:\\Program Files\\Microsoft Visual Studio\\2022",
			"C:\\Program Files (x86)\\Microsoft Visual Studio\\2022"
		};
		const std::vector<std::string> editions = { "Community", "Professional", "Enterprise", "BuildTools" };

		for (const auto& root : programFilesRoots) {
			for (const auto& edition : editions) {
				std::filesystem::path candidate(root);
				candidate /= edition;
				candidate /= "MSBuild";
				candidate /= "Current";
				candidate /= "Bin";
				candidate /= "MSBuild.exe";

				std::error_code ec;
				if (std::filesystem::exists(candidate, ec) && !ec) {
					return candidate.generic_string();
				}
			}
		}

		// vswhere fallback for non-default install layouts.
		const std::filesystem::path vswhere = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
		std::error_code vswhereEc;
		if (std::filesystem::exists(vswhere, vswhereEc) && !vswhereEc) {
			Process::Result vswhereResult = Process::Run({
				vswhere.string(),
				"-latest",
				"-requires", "Microsoft.Component.MSBuild",
				"-find", "MSBuild\\**\\Bin\\MSBuild.exe",
				"-nologo"
			});
			if (vswhereResult.Succeeded()) {
				const std::string& out = vswhereResult.Output;
				const auto newline = out.find_first_of("\r\n");
				std::string firstLine = (newline == std::string::npos) ? out : out.substr(0, newline);
				while (!firstLine.empty() && (firstLine.back() == ' ' || firstLine.back() == '\t')) {
					firstLine.pop_back();
				}
				std::error_code existEc;
				if (!firstLine.empty() && std::filesystem::exists(firstLine, existEc) && !existEc) {
					return firstLine;
				}
			}
		}
#endif
		// Last resort: rely on PATH (returns empty string is also valid; caller can decide).
		return "MSBuild.exe";
	}

	IndexProject::BuildResult IndexProject::BuildSolution(const std::string& configuration,
		const std::string& platform) {
		BuildResult result;

		const std::string engineRoot = GetEngineRootDir();
		if (engineRoot.empty()) {
			result.Output = "Could not resolve engine root directory; skipping build.";
			IDX_WARN_TAG("IndexProject", "{}", result.Output);
			return result;
		}

		const std::filesystem::path solutionPath = std::filesystem::path(engineRoot) / "Index.sln";
		std::error_code ec;
		if (!std::filesystem::exists(solutionPath, ec) || ec) {
			result.Output = "Index.sln not found at " + solutionPath.generic_string();
			IDX_WARN_TAG("IndexProject", "{}", result.Output);
			return result;
		}

		const std::string msbuild = GetMSBuildPath();

		IDX_INFO_TAG("IndexProject", "Building Index.sln (Configuration={}, Platform={}) via {}...",
			configuration, platform, msbuild);

		Process::Result processResult = Process::Run({
			msbuild,
			solutionPath.generic_string(),
			"-p:Configuration=" + configuration,
			"-p:Platform=" + platform,
			"-m",
			"-verbosity:minimal",
			"-nologo"
		}, engineRoot);

		result.ExitCode = processResult.ExitCode;
		result.Succeeded = processResult.Succeeded();
		result.Output = std::move(processResult.Output);

		if (!result.Succeeded) {
			IDX_ERROR_TAG("IndexProject", "MSBuild failed (exit code {}): {}", result.ExitCode, result.Output);
		}
		else {
			IDX_INFO_TAG("IndexProject", "MSBuild succeeded.");
		}
		return result;
	}

	IndexProject::BuildResult IndexProject::BuildSolutionTargets(const std::vector<std::string>& targets,
		const std::string& configuration, const std::string& platform) {
		BuildResult result;

		// Empty target list = success no-op. A project with no local packages skips MSBuild entirely.
		if (targets.empty()) {
			result.Succeeded = true;
			result.ExitCode = 0;
			result.Output = "No targets to build.";
			return result;
		}

		const std::string engineRoot = GetEngineRootDir();
		if (engineRoot.empty()) {
			result.Output = "Could not resolve engine root directory; skipping build.";
			IDX_WARN_TAG("IndexProject", "{}", result.Output);
			return result;
		}

		const std::filesystem::path solutionPath = std::filesystem::path(engineRoot) / "Index.sln";
		std::error_code ec;
		if (!std::filesystem::exists(solutionPath, ec) || ec) {
			result.Output = "Index.sln not found at " + solutionPath.generic_string();
			IDX_WARN_TAG("IndexProject", "{}", result.Output);
			return result;
		}

		// MSBuild expects -t:X;Y;Z with semicolons; solution targets are dot-separated (Pkg.Foo.Native).
		std::string targetsArg = "-t:";
		for (size_t i = 0; i < targets.size(); ++i) {
			if (i > 0) targetsArg += ";";
			targetsArg += targets[i];
		}

		const std::string msbuild = GetMSBuildPath();

		IDX_INFO_TAG("IndexProject", "Building {} target(s) in Index.sln (Configuration={}, Platform={}): {}",
			targets.size(), configuration, platform, targetsArg);

		Process::Result processResult = Process::Run({
			msbuild,
			solutionPath.generic_string(),
			"-p:Configuration=" + configuration,
			"-p:Platform=" + platform,
			targetsArg,
			"-m",
			"-restore", // freshly generated csproj fails with NETSDK1004 without restore
			"-verbosity:minimal",
			"-nologo"
		}, engineRoot);

		result.ExitCode = processResult.ExitCode;
		result.Succeeded = processResult.Succeeded();
		result.Output = std::move(processResult.Output);

		if (!result.Succeeded) {
			IDX_ERROR_TAG("IndexProject", "MSBuild failed (exit code {}): {}", result.ExitCode, result.Output);
		}
		else {
			IDX_INFO_TAG("IndexProject", "MSBuild target build succeeded.");
		}
		return result;
	}

	std::vector<std::string> IndexProject::EnumerateProjectLocalPackages(const std::string& projectRoot) {
		// Returns directory names under <projectRoot>/Packages containing an index-package.lua.
		std::vector<std::string> names;
		if (projectRoot.empty()) return names;

		const std::filesystem::path packagesDir = std::filesystem::path(projectRoot) / "Packages";
		std::error_code ec;
		if (!std::filesystem::exists(packagesDir, ec) || ec) return names;
		if (!std::filesystem::is_directory(packagesDir, ec) || ec) return names;

		for (const auto& entry : std::filesystem::directory_iterator(packagesDir, ec)) {
			if (ec) break;
			if (!entry.is_directory(ec) || ec) continue;
			const std::filesystem::path manifest = entry.path() / "index-package.lua";
			if (std::filesystem::exists(manifest, ec) && !ec) {
				names.push_back(entry.path().filename().string());
			}
		}

		return names;
	}

	IndexProject::AutomateResult IndexProject::AutomateForProject(const std::string& projectRootDir,
		const std::string& configuration, const std::string& platform) {
		AutomateResult result;
		result.Regenerate = RegenerateSolutionForProject(projectRootDir);

		// ExitCode == -1 means premake was missing — the existing solution is still valid, proceed.
		const bool premakeRanAndFailed = !result.Regenerate.Succeeded && result.Regenerate.ExitCode != -1;
		if (premakeRanAndFailed) {
			IDX_ERROR_TAG("IndexProject", "Skipping build because premake regeneration failed.");
			return result;
		}

		// Build local packages only — relinking Index-Engine.dll would fail (loaded by caller).
		std::vector<std::string> targets;
		for (const std::string& packageName : EnumerateProjectLocalPackages(projectRootDir)) {
			// Up to two targets per package; MSBuild silently ignores unresolved ones.
			targets.push_back("Pkg." + packageName + ".Native");
			targets.push_back("Pkg." + packageName);
		}

		result.Build = BuildSolutionTargets(targets, configuration, platform);
		result.RanBuild = !targets.empty();
		return result;
	}

} // namespace Index
