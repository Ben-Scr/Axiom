#pragma once

#include "Core/Export.hpp"
#include "Packages/PackageInfo.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Axiom {

	// Pure-string parse of a .csproj's installed-package set. Returns one
	// PackageInfo per <PackageReference> and per non-system <Reference>.
	//
	// The engine's ScriptCore reference is intentionally excluded — it's emitted
	// into every user .csproj by the project generator (it's the engine core,
	// not a user-installable package), so it must never surface in a "what's
	// installed" view.
	//
	// `centralVersions` carries the merged contents of all `Directory.Packages.props`
	// files that apply to the project (loaded by LoadCentralPackageVersionsFromDisk
	// when reading from disk; tests pass it in directly). When a PackageReference
	// has no Version attribute, the parser looks the version up here.
	//
	// On parse failure the returned vector is empty — the function never throws
	// or asserts. This is intentional so the package manager UI degrades gracefully
	// when the .csproj is mid-edit.
	AXIOM_API std::vector<PackageInfo> ParseInstalledPackagesFromXml(
		std::string_view xmlText,
		const std::unordered_map<std::string, std::string>& centralVersions = {});

	// Walks the .csproj's directory upward, merging every Directory.Packages.props
	// it finds. The deepest props file wins (matches MSBuild's evaluation order).
	AXIOM_API std::unordered_map<std::string, std::string> LoadCentralPackageVersionsFromDisk(
		const std::filesystem::path& csprojPath);

} // namespace Axiom
