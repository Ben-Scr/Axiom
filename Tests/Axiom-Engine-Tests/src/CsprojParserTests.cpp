// Regression tests for Axiom::ParseInstalledPackagesFromXml.
//
// Each TEST_CASE here pins a specific bug or invariant the package-manager UI
// depends on. When this file grows, prefer adding a new SUBCASE inside an
// existing TEST_CASE if the .csproj fixture is the same; spawn a new TEST_CASE
// when the fixture changes.

#include <doctest/doctest.h>

#include "Packages/CsprojParser.hpp"
#include "Packages/PackageInfo.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

using Axiom::ParseInstalledPackagesFromXml;
using Axiom::PackageInfo;
using Axiom::PackageSourceType;

namespace {
	// Find a package by id (case-sensitive — NuGet IDs as written into the .csproj).
	const PackageInfo* Find(const std::vector<PackageInfo>& v, const std::string& id) {
		for (const auto& p : v) {
			if (p.Id == id) return &p;
		}
		return nullptr;
	}
} // namespace

// ── REGRESSION: parse_non_destructive bug ──────────────────────────────────────
//
// Pre-fix, the parser used rapidxml::parse_non_destructive which left node
// names un-null-terminated. Building std::string from node->name() then read
// past the element name with strlen, comparisons like `s == "PackageReference"`
// always failed, and this function silently returned an empty vector even
// though the .csproj clearly had the entries dotnet had written. The user-
// visible symptom was: install a NuGet package, the row in the package
// manager keeps saying "Install" forever and the package never shows up in
// the In Project tab.
//
// Any future regression that empties the result for a well-formed .csproj
// trips this test.
TEST_CASE("ParseInstalledPackagesFromXml — well-formed PackageReference is detected") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net9.0</TargetFramework>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="RandomCS" Version="1.0.1" />
  </ItemGroup>
</Project>)";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 1);
	CHECK(installed[0].Id == "RandomCS");
	CHECK(installed[0].Version == "1.0.1");
	CHECK(installed[0].InstalledVersion == "1.0.1");
	CHECK(installed[0].IsInstalled);
	CHECK(installed[0].SourceType == PackageSourceType::NuGet);
}

// ── REGRESSION: Axiom-ScriptCore in user packages ──────────────────────────────
//
// The project generator always emits <Reference Include="Axiom-ScriptCore">
// into every user .csproj — it's the engine core, not a user package. Once
// the parser was fixed (above), that reference started leaking into the
// "User Packages" / "NuGet Packages" panel. The filter must drop it.
TEST_CASE("ParseInstalledPackagesFromXml — engine ScriptCore reference is filtered") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <Reference Include="Axiom-ScriptCore">
      <HintPath>Packages\Axiom-ScriptCore\Axiom-ScriptCore.dll</HintPath>
      <Private>true</Private>
    </Reference>
  </ItemGroup>
  <ItemGroup>
    <PackageReference Include="RandomCS" Version="1.0.1" />
    <PackageReference Include="RandomNameGenerator" Version="1.0.4" />
  </ItemGroup>
</Project>)";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 2);
	CHECK(Find(installed, "Axiom-ScriptCore") == nullptr);
	CHECK(Find(installed, "RandomCS") != nullptr);
	CHECK(Find(installed, "RandomNameGenerator") != nullptr);
}

// ── INVARIANT: System.* / Microsoft.* references are filtered ──────────────────
//
// These are always present in real-world .csproj files (sometimes via
// transitive package metadata). They are not "user packages" and must not
// surface as such.
TEST_CASE("ParseInstalledPackagesFromXml — system references are filtered") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <Reference Include="System.Text.Json" />
    <Reference Include="Microsoft.Extensions.Logging" />
    <Reference Include="MyUserDll">
      <HintPath>libs/MyUserDll.dll</HintPath>
    </Reference>
  </ItemGroup>
</Project>)";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 1);
	CHECK(installed[0].Id == "MyUserDll");
	CHECK(installed[0].SourceType == PackageSourceType::GitHub); // <Reference> source bucket
}

// ── INVARIANT: PackageReference Version can come from Directory.Packages.props ─
//
// Central package management omits the Version attribute on PackageReference
// and instead defines it in Directory.Packages.props. The parser must look
// the version up in the centralVersions map when the inline attribute is
// missing.
TEST_CASE("ParseInstalledPackagesFromXml — central package version is resolved") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <PackageReference Include="Newtonsoft.Json" />
  </ItemGroup>
</Project>)";

	const std::unordered_map<std::string, std::string> central = {
		{ "Newtonsoft.Json", "13.0.3" }
	};

	auto installed = ParseInstalledPackagesFromXml(xml, central);

	REQUIRE(installed.size() == 1);
	CHECK(installed[0].Id == "Newtonsoft.Json");
	CHECK(installed[0].Version == "13.0.3");
	CHECK(installed[0].InstalledVersion == "13.0.3");
}

// ── INVARIANT: malformed XML degrades gracefully ───────────────────────────────
//
// The package manager UI calls this on every poll. Throwing or asserting on
// a half-written .csproj (mid-edit by dotnet add, by the user, etc.) would
// crash the editor. Empty result is the contract.
TEST_CASE("ParseInstalledPackagesFromXml — malformed input returns empty without throwing") {
	SUBCASE("not XML at all") {
		auto installed = ParseInstalledPackagesFromXml("this is not xml");
		CHECK(installed.empty());
	}
	SUBCASE("truncated mid-element") {
		auto installed = ParseInstalledPackagesFromXml("<Project><ItemGroup><PackageReference Include=\"Foo");
		CHECK(installed.empty());
	}
	SUBCASE("empty string") {
		auto installed = ParseInstalledPackagesFromXml("");
		CHECK(installed.empty());
	}
	SUBCASE("xml with no Project root") {
		auto installed = ParseInstalledPackagesFromXml("<NotAProject><ItemGroup/></NotAProject>");
		CHECK(installed.empty());
	}
}

// ── INVARIANT: multiple ItemGroups are aggregated ──────────────────────────────
//
// dotnet add typically appends a new <ItemGroup> for each install, rather
// than merging with an existing one. The .csproj typically ends up with
// many ItemGroups. All PackageReferences across all ItemGroups must be
// detected.
TEST_CASE("ParseInstalledPackagesFromXml — packages across multiple ItemGroups are collected") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <Compile Include="Assets\Scripts\**\*.cs" />
  </ItemGroup>
  <ItemGroup>
    <PackageReference Include="A" Version="1.0.0" />
  </ItemGroup>
  <ItemGroup>
    <PackageReference Include="B" Version="2.0.0" />
  </ItemGroup>
  <ItemGroup>
    <PackageReference Include="C" Version="3.0.0" />
  </ItemGroup>
</Project>)";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 3);
	CHECK(Find(installed, "A")->Version == "1.0.0");
	CHECK(Find(installed, "B")->Version == "2.0.0");
	CHECK(Find(installed, "C")->Version == "3.0.0");
}

// ── INVARIANT: child <Version> element is honored ──────────────────────────────
//
// MSBuild allows <PackageReference Include="X"><Version>1.0.0</Version>
// </PackageReference> as an alternative to the Version attribute. Neither
// dotnet add nor central package management produce this style today, but
// hand-edited .csproj files do. Documented as supported.
TEST_CASE("ParseInstalledPackagesFromXml — child <Version> element is honored") {
	const std::string xml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <PackageReference Include="HandWritten">
      <Version>9.9.9</Version>
    </PackageReference>
  </ItemGroup>
</Project>)";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 1);
	CHECK(installed[0].Id == "HandWritten");
	CHECK(installed[0].Version == "9.9.9");
}

// ── INVARIANT: real-world .csproj from the Axiom project generator ─────────────
//
// This is the exact shape AxiomProject.cpp emits today. End-to-end check
// that the parser produces the result the package manager UI expects to
// render: NuGet packages listed, ScriptCore filtered, no spurious entries.
TEST_CASE("ParseInstalledPackagesFromXml — real Axiom-generated .csproj layout") {
	// Custom raw-string delimiter: the .csproj content contains `Exists('...')"`,
	// which embeds the literal sequence `)"` — that would prematurely terminate
	// a default R"(...)" string. Use R"xml(...)xml" so MSBuild-style strings
	// stay intact.
	const std::string xml = R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Library</OutputType>
    <TargetFramework>net9.0</TargetFramework>
    <Configurations>Debug;Release;Dist</Configurations>
  </PropertyGroup>

  <ItemGroup>
    <Compile Include="Assets\Scripts\**\*.cs" />
  </ItemGroup>

  <ItemGroup>
    <Reference Include="Axiom-ScriptCore">
      <HintPath>Packages\Axiom-ScriptCore\Axiom-ScriptCore.dll</HintPath>
      <Private>true</Private>
    </Reference>
  </ItemGroup>

  <ItemGroup>
    <PackageReference Include="RandomCS" Version="1.0.1" />
    <PackageReference Include="RandomNameGenerator" Version="1.0.4" />
  </ItemGroup>

  <!-- Auto-generated by Axiom Package Manager every time a package is installed/removed. -->
  <Import Project="Packages/AxiomPackages.props" Condition="Exists('Packages/AxiomPackages.props')" />

</Project>)xml";

	auto installed = ParseInstalledPackagesFromXml(xml);

	REQUIRE(installed.size() == 2);
	CHECK(Find(installed, "Axiom-ScriptCore") == nullptr);

	const PackageInfo* randomCs = Find(installed, "RandomCS");
	REQUIRE(randomCs != nullptr);
	CHECK(randomCs->Version == "1.0.1");
	CHECK(randomCs->IsInstalled);
	CHECK(randomCs->SourceType == PackageSourceType::NuGet);

	const PackageInfo* rng = Find(installed, "RandomNameGenerator");
	REQUIRE(rng != nullptr);
	CHECK(rng->Version == "1.0.4");
	CHECK(rng->IsInstalled);
}
