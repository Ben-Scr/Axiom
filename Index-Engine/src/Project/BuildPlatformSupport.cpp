#include "pch.hpp"
#include "Project/BuildPlatformSupport.hpp"

namespace Index {

	bool BuildPlatformSupport::IsAvailable(BuildPlatform platform) {
		switch (platform) {
			case BuildPlatform::Windows: return true;
			// Linux runtime/toolchain support is gated behind a future
			// editor platform-package install. Until that package system
			// exists, Linux profiles are surfaced in the UI for planning
			// but the Build button stays disabled.
			case BuildPlatform::Linux:   return false;
		}
		return false;
	}

	std::string BuildPlatformSupport::UnavailableReason(BuildPlatform platform) {
		switch (platform) {
			case BuildPlatform::Windows:
				return {};
			case BuildPlatform::Linux:
				return "Linux platform support is not installed. "
				       "Install it via the Package Manager once Linux platform packages are available.";
		}
		return "Unknown platform.";
	}

} // namespace Index
