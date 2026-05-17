#pragma once
#include "Core/Export.hpp"
#include "Project/IndexProject.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Index {

	// Target platform a build profile produces a runtime for. Kept separate
	// from IndexProject::RenderBackend (which is the platform's graphics
	// backend) so future platforms drop in by adding one enum value plus a
	// row in the BuildPlatformSupport table.
	enum class BuildPlatform : uint8_t {
		Windows = 0,
		Linux   = 1,
	};

	// A named bundle of (platform, render backend) the user can pick from
	// the Build panel. Persisted as a single-file `.indexbuild` JSON under
	// `<ProjectRoot>/BuildProfiles/`. The on-disk file stem is the profile's
	// identity (used by IndexProject::ActiveBuildProfileName); the `Name`
	// field is just a display label and can be renamed by re-saving under a
	// different filename.
	struct INDEX_API IndexBuildProfile {
		std::string Name;
		BuildPlatform Platform = BuildPlatform::Windows;
		IndexProject::RenderBackend RenderBackend = IndexProject::RenderBackend::Auto;

		static const char* PlatformToString(BuildPlatform platform);
		static BuildPlatform PlatformFromString(std::string_view value);

		// Render backends the editor exposes as valid for a given platform.
		// Profiles loaded with a backend outside this list get coerced to
		// the first allowed entry on next save (and a warning is logged).
		static std::vector<IndexProject::RenderBackend> AllowedBackends(BuildPlatform platform);
		static bool IsBackendAllowed(BuildPlatform platform, IndexProject::RenderBackend backend);

		bool Save(const std::string& filePath) const;
		static IndexBuildProfile Load(const std::string& filePath);

		// Writes the engine's default profiles (Windows + Linux) into `directory`.
		// Files that already exist are left untouched so user edits are never
		// clobbered. The directory is created if it doesn't already exist.
		// Returns the number of profile files actually written.
		static int WriteDefaultProfiles(const std::string& directory);

		// File extension recognised by the editor (".indexbuild"). Centralised
		// so the panel and the Build flow use the same literal.
		static constexpr std::string_view FileExtension = ".indexbuild";
	};

} // namespace Index
