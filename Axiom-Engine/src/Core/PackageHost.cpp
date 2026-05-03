#include "pch.hpp"
#include "Core/PackageHost.hpp"

#include "Core/Log.hpp"
#include "Project/AxiomProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/Path.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <unordered_set>

#ifdef AIM_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Axiom {

	namespace {
		using OnLoadFn   = int (*)();
		using OnUnloadFn = void (*)();

		std::vector<LoadedPackage> s_LoadedPackages;
		bool s_LoadAllRan = false;

		void* PlatformLoad(const std::string& path) {
#ifdef AIM_PLATFORM_WINDOWS
			return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
			return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
		}

		void* PlatformResolve(void* module, const char* symbol) {
#ifdef AIM_PLATFORM_WINDOWS
			return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(module), symbol));
#else
			return dlsym(module, symbol);
#endif
		}

		void PlatformUnload(void* module) {
#ifdef AIM_PLATFORM_WINDOWS
			FreeLibrary(static_cast<HMODULE>(module));
#else
			dlclose(module);
#endif
		}

		std::string PackageNameFromFilename(const std::string& filename) {
			// "Pkg.Axiom.Hello.Native.dll"  ->  "Axiom.Hello"
			std::string stem = filename;
			const auto dot = stem.find_last_of('.');
			if (dot != std::string::npos) {
				stem = stem.substr(0, dot);
			}
			constexpr std::string_view k_Prefix = "Pkg.";
			constexpr std::string_view k_Suffix = ".Native";
			if (stem.rfind(k_Prefix, 0) == 0) {
				stem = stem.substr(k_Prefix.size());
			}
			if (stem.size() > k_Suffix.size() &&
				stem.compare(stem.size() - k_Suffix.size(), k_Suffix.size(), k_Suffix) == 0) {
				stem = stem.substr(0, stem.size() - k_Suffix.size());
			}
			return stem;
		}

		// Folders we care about end in `.Native` — that's where the
		// unmanaged C++ DLL for an engine-core / standalone-cpp package
		// lands. Plain `Pkg.<Name>/` folders hold the *managed* C# assembly,
		// which Win32 LoadLibrary will happily map (a .NET assembly is a
		// valid PE file) but pollutes the unmanaged loader, pins the file
		// against editor C# hot-reload, and produces the misleading
		// "no AxiomPackage_OnLoad export" log lines users have seen.
		bool LooksLikeNativePackageFolder(const std::string& folderName) {
			if (folderName.rfind("Pkg.", 0) != 0) return false;
			constexpr std::string_view k_NativeSuffix = ".Native";
			return folderName.size() > k_NativeSuffix.size()
				&& folderName.compare(folderName.size() - k_NativeSuffix.size(),
					k_NativeSuffix.size(), k_NativeSuffix) == 0;
		}

		void DiscoverIn(const std::filesystem::path& root, std::vector<std::filesystem::path>& outCandidates) {
			std::error_code ec;
			if (!std::filesystem::is_directory(root, ec) || ec) {
				return;
			}

			for (const auto& entry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
				if (ec) break;

				const auto& entryPath = entry.path();
				const std::string name = entryPath.filename().string();

				if (entry.is_directory(ec) && !ec) {
					if (LooksLikeNativePackageFolder(name)) {
#ifdef AIM_PLATFORM_WINDOWS
						const std::filesystem::path candidate = entryPath / (name + ".dll");
#else
						const std::filesystem::path candidate = entryPath / ("lib" + name + ".so");
#endif
						if (std::filesystem::exists(candidate, ec) && !ec) {
							outCandidates.push_back(candidate);
						}
					}
				}
			}
		}
	}

	namespace {
		// Shared discovery + filter + load worker for both LoadAll() and
		// LoadInstalled(). Returns the count of newly loaded packages.
		// Skips any candidate whose name OR file path is already present in
		// s_LoadedPackages — calling twice is safe and produces no double-
		// register from OnLoad. Logging is suppressed for "already loaded"
		// packages on the second pass so the post-install path doesn't
		// noisy-log every existing package.
		size_t LoadInternal(bool isInitialLoadAll) {
			std::vector<std::filesystem::path> candidates;

			const std::filesystem::path exeDir(Path::ExecutableDir());
			// Dev / build layout: bin/<config>/<Consumer>/<exe>; package DLLs are siblings in
			// bin/<config>/Pkg.<Name>.Native/. Scan one level up from the exe directory.
			std::filesystem::path searchRoot = exeDir.parent_path();
			DiscoverIn(searchRoot, candidates);

			// Distribution layout (future): packages alongside the exe in a Packages/ folder.
			DiscoverIn(exeDir / "Packages", candidates);

			// Modular package policy: only packages the active project has
			// explicitly *installed* (listed in axiom-project.json's `packages`
			// array) are LoadLibrary'd. The engine itself stays free of types
			// that no project asked for — the same install-or-not contract the
			// user-facing package menu implies.
			//
			// Decision matrix:
			//   project loaded + packages array present  ->  load only listed
			//   project loaded + packages array empty    ->  load nothing
			//   no project loaded                        ->  load nothing
			//   AXIOM_LOAD_ALL_PACKAGES=1                ->  load every found DLL
			//
			// AXIOM_LOAD_ALL_PACKAGES is the engine-developer escape hatch — set
			// it when running the engine repo's own editor build with no project
			// just to smoke-test that every shipped package still loads. Off by
			// default so end-user editors / runtimes never silently load a
			// package that wasn't installed.
			const AxiomProject* activeProject = ProjectManager::GetCurrentProject();
			std::unordered_set<std::string> allowList;
			if (activeProject) {
				allowList.insert(activeProject->Packages.begin(), activeProject->Packages.end());
			}

			bool loadEverything = false;
			if (const char* env = std::getenv("AXIOM_LOAD_ALL_PACKAGES")) {
				if (env[0] == '1' || env[0] == 't' || env[0] == 'T') {
					loadEverything = true;
					if (isInitialLoadAll) {
						AIM_CORE_INFO_TAG("PackageHost",
							"AXIOM_LOAD_ALL_PACKAGES is set — overriding the project's `packages` allow-list and loading every discovered package.");
					}
				}
			}

			if (!loadEverything && allowList.empty()) {
				if (isInitialLoadAll) {
					if (activeProject) {
						AIM_CORE_INFO_TAG("PackageHost",
							"No packages installed for project '{}' — skipping all {} discovered package DLL(s). Add entries to axiom-project.json's `packages` array to install them.",
							activeProject->Name, candidates.size());
					}
					else {
						AIM_CORE_INFO_TAG("PackageHost",
							"No project loaded — skipping all {} discovered package DLL(s). Pass --project=<path> or set AXIOM_LOAD_ALL_PACKAGES=1 to load packages.",
							candidates.size());
					}
				}
				return 0;
			}

			// Build a fast lookup of what's already loaded so we don't double-load.
			// Match by package name AND by module path: name catches the common
			// "DLL was rebuilt with the same name" case; path catches the rare
			// "two DLLs with the same package name in different folders" case.
			std::unordered_set<std::string> alreadyLoadedByName;
			std::unordered_set<std::string> alreadyLoadedByPath;
			for (const LoadedPackage& existing : s_LoadedPackages) {
				alreadyLoadedByName.insert(existing.Name);
				alreadyLoadedByPath.insert(existing.ModulePath);
			}

			size_t newlyLoaded = 0;
			for (const auto& candidate : candidates) {
				const std::string pathStr = candidate.string();
				const std::string fileName = candidate.filename().string();
				const std::string packageName = PackageNameFromFilename(fileName);

				if (!loadEverything && allowList.find(packageName) == allowList.end()) {
					if (isInitialLoadAll) {
						AIM_CORE_INFO_TAG("PackageHost",
							"Skipping package '{}' — not in project's `packages` allow-list.", packageName);
					}
					continue;
				}

				if (alreadyLoadedByName.count(packageName) || alreadyLoadedByPath.count(pathStr)) {
					// Quietly skip — common during LoadInstalled() rescans.
					continue;
				}

				void* module = PlatformLoad(pathStr);
				if (!module) {
					AIM_CORE_WARN_TAG("PackageHost", "Failed to load package: {}", pathStr);
					continue;
				}

				LoadedPackage loaded;
				loaded.Name = packageName;
				loaded.ModulePath = pathStr;
				loaded.ModuleHandle = module;

				if (auto* onLoad = reinterpret_cast<OnLoadFn>(PlatformResolve(module, "AxiomPackage_OnLoad"))) {
					const int result = onLoad();
					if (result != 0) {
						AIM_CORE_WARN_TAG("PackageHost",
							"Package '{}' AxiomPackage_OnLoad returned {} (non-zero); keeping module loaded.",
							loaded.Name, result);
					}
				}
				else {
					AIM_CORE_INFO_TAG("PackageHost", "Loaded package '{}' (no AxiomPackage_OnLoad export).", loaded.Name);
				}

				s_LoadedPackages.push_back(std::move(loaded));
				++newlyLoaded;
			}

			return newlyLoaded;
		}
	} // namespace

	void PackageHost::LoadAll() {
		if (s_LoadAllRan) {
			return;
		}

		const size_t loaded = LoadInternal(/*isInitialLoadAll=*/true);
		AIM_CORE_INFO_TAG("PackageHost", "Loaded {} package(s).", loaded);

		// Set the guard AFTER the load loop so a partial init (exception
		// thrown out of an OnLoad, etc.) leaves LoadAll callable again.
		s_LoadAllRan = true;
	}

	size_t PackageHost::LoadInstalled() {
		// No s_LoadAllRan check — this is the path that exists *because*
		// LoadAll already ran but didn't see the just-built DLL.
		const size_t loaded = LoadInternal(/*isInitialLoadAll=*/false);
		if (loaded > 0) {
			AIM_CORE_INFO_TAG("PackageHost",
				"LoadInstalled: loaded {} new package(s) (total now {}).",
				loaded, s_LoadedPackages.size());
		}
		return loaded;
	}

	void PackageHost::UnloadAll() {
		for (auto it = s_LoadedPackages.rbegin(); it != s_LoadedPackages.rend(); ++it) {
			if (auto* onUnload = reinterpret_cast<OnUnloadFn>(PlatformResolve(it->ModuleHandle, "AxiomPackage_OnUnload"))) {
				onUnload();
			}
			PlatformUnload(it->ModuleHandle);
		}
		s_LoadedPackages.clear();
		s_LoadAllRan = false;
	}

	const std::vector<LoadedPackage>& PackageHost::GetLoaded() {
		return s_LoadedPackages;
	}

}
