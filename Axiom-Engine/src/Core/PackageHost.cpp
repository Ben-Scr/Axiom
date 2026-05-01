#include "pch.hpp"
#include "Core/PackageHost.hpp"

#include "Core/Log.hpp"
#include "Serialization/Path.hpp"

#include <filesystem>

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
					if (name.rfind("Pkg.", 0) == 0) {
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

	void PackageHost::LoadAll() {
		if (s_LoadAllRan) {
			return;
		}
		s_LoadAllRan = true;

		std::vector<std::filesystem::path> candidates;

		const std::filesystem::path exeDir(Path::ExecutableDir());
		// Dev / build layout: bin/<config>/<Consumer>/<exe>; package DLLs are siblings in
		// bin/<config>/Pkg.<Name>.Native/. Scan one level up from the exe directory.
		std::filesystem::path searchRoot = exeDir.parent_path();
		DiscoverIn(searchRoot, candidates);

		// Distribution layout (future): packages alongside the exe in a Packages/ folder.
		DiscoverIn(exeDir / "Packages", candidates);

		for (const auto& candidate : candidates) {
			const std::string pathStr = candidate.string();
			const std::string fileName = candidate.filename().string();

			void* module = PlatformLoad(pathStr);
			if (!module) {
				AIM_CORE_WARN_TAG("PackageHost", "Failed to load package: {}", pathStr);
				continue;
			}

			LoadedPackage loaded;
			loaded.Name = PackageNameFromFilename(fileName);
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
		}

		AIM_CORE_INFO_TAG("PackageHost", "Loaded {} package(s).", s_LoadedPackages.size());
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
