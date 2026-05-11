#pragma once

#include "Core/Export.hpp"

#include <string>

namespace Axiom {
	struct AxiomProject;

	// Splash-screen and app-icon asset paths can come from three places:
	//   1. The project's own Assets/ tree            → stored project-relative ("Assets/foo.png")
	//   2. The engine-shipped AxiomAssets/ tree      → stored as "axiom:<subpath>"
	//      where <subpath> is the path under AxiomAssets/ (e.g. "axiom:Textures/icon.png")
	//   3. An absolute filesystem path              → stored verbatim (not portable across machines)
	//
	// SplashAssetResolve centralises that storage convention and the inverse
	// resolution. Both the runtime splash layer (Axiom-Runtime) and the editor's
	// splash preview consume it so the editor's "Show Preview" matches exactly
	// what the shipped game renders, including the engine-shipped defaults.
	namespace SplashAssetResolve {
		// Resolves a stored path (any of the three forms above) to an absolute
		// filesystem path that exists, or an empty string if it can't be
		// located. Pass the active project so project-relative paths can be
		// anchored at RootDirectory / AssetsDirectory.
		AXIOM_API std::string Resolve(const std::string& stored, const AxiomProject* project);

		// Turns a picker's absolute result back into the portable stored form.
		// Strategy: prefer "axiom:<sub>" when the path is inside AxiomAssets,
		// otherwise project-relative when inside the project's assets tree,
		// falling back to the bare filename for anything else. Empty input
		// passes through.
		AXIOM_API std::string NormalizeForStorage(const std::string& pickedAbsolute, const AxiomProject* project);

		// Engine-shipped default logo — AxiomAssets/Textures/icon.png with a
		// historical Axiom64.png fallback for older installs. Returns empty
		// when neither exists (running outside a packaged or dev layout).
		AXIOM_API std::string DefaultLogoPath();

		// "Axiom <version>  ·  <platform>  ·  <profile>" — the engine-generated
		// subtitle the splash uses when SplashScreen.CustomText is empty.
		// Each translation unit that includes this header compiles the
		// platform / profile branch matching its own build, so the editor
		// preview shows the editor's compile-time values and the runtime
		// shows its own.
		AXIOM_API std::string DefaultSubtitleLine();
	}
}
