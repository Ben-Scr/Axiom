#pragma once

#include "Core/Export.hpp"

#include <string>
#include <vector>

namespace Axiom {

	struct LoadedPackage {
		std::string Name;          // Without the "Pkg." prefix or ".Native" suffix, e.g. "Axiom.Hello".
		std::string ModulePath;    // Absolute path to the loaded shared library.
		void*       ModuleHandle = nullptr; // HMODULE on Windows; void* dlopen handle on POSIX.
	};

	// Runtime host for Axiom packages.
	//
	// Discovers Pkg.<Name>.Native shared libraries adjacent to the running executable,
	// loads each one, and (if the package exports it) calls `AxiomPackage_OnLoad()`
	// to give the package a chance to self-register components, systems, etc.
	//
	// Symmetrically, `UnloadAll()` calls `AxiomPackage_OnUnload()` if exported and
	// frees the modules in reverse load order.
	class AXIOM_API PackageHost {
	public:
		// Scan and load all packages reachable from the current executable.
		// Idempotent — calling twice is a no-op.
		static void LoadAll();

		// Re-scan for package DLLs and load any that the active project's
		// `packages` allow-list now permits but that aren't already loaded.
		// Safe to call after a package-install build completes: the editor's
		// PackageManagerPanel uses this so newly-built component types appear
		// in the Add Component popup without an editor restart. Returns the
		// number of new packages loaded by this call.
		static size_t LoadInstalled();

		// Unload all packages in reverse order. Safe to call even if LoadAll() never ran.
		static void UnloadAll();

		static const std::vector<LoadedPackage>& GetLoaded();
	};

}
