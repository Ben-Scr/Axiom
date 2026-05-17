#pragma once
#include "Core/Export.hpp"
#include "Project/IndexBuildProfile.hpp"

#include <string>

namespace Index {

	// Thin abstraction over "is the toolchain / runtime artifacts for this
	// platform installed in the editor?". Today the answer is hard-coded
	// (Windows yes, Linux no) because the cross-platform package system
	// for the editor doesn't exist yet — only the scripting-side engine
	// package system does. The Build panel reads this to decide whether
	// to disable the Build button + show a warning row pointing the user
	// at the Package Manager.
	//
	// TODO(platform-packages): Replace these stubs with a query against
	// the editor's package registry once `Pkg.Index.Platform.<Name>`
	// packages exist and the PackageHost knows how to enumerate them.
	struct INDEX_API BuildPlatformSupport {
		static bool IsAvailable(BuildPlatform platform);

		// Human-readable explanation for the warning row. Empty when the
		// platform is available.
		static std::string UnavailableReason(BuildPlatform platform);
	};

} // namespace Index
