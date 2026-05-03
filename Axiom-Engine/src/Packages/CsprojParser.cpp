#include "pch.hpp"
#include "Packages/CsprojParser.hpp"

#include <cereal/macros.hpp>
#include <cereal/external/rapidxml/rapidxml.hpp>

#include <fstream>

namespace Axiom {

	namespace {
		namespace rapidxml = cereal::rapidxml;

		struct XmlDocument {
			std::string Buffer;
			rapidxml::xml_document<> Document;
		};

		std::string Trim(std::string_view value) {
			const size_t start = value.find_first_not_of(" \t\r\n");
			if (start == std::string_view::npos) {
				return {};
			}
			const size_t end = value.find_last_not_of(" \t\r\n");
			return std::string(value.substr(start, end - start + 1));
		}

		// IMPORTANT: parse_default null-terminates names/values in place. Do NOT
		// switch back to parse_non_destructive — see PackageManager regression
		// (silently returned 0 installed packages, every comparison strlen-walked
		// past the element name into the rest of the XML buffer).
		bool ParseXmlString(const std::string& xmlText, XmlDocument& document) {
			document.Buffer = xmlText;
			document.Buffer.push_back('\0');
			document.Document.clear();

			try {
				document.Document.parse<rapidxml::parse_default>(&document.Buffer[0]);
				return true;
			}
			catch (const rapidxml::parse_error&) {
				return false;
			}
		}

		bool ParseXmlFile(const std::filesystem::path& path, XmlDocument& document) {
			std::ifstream input(path);
			if (!input.is_open()) {
				return false;
			}
			std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			return ParseXmlString(contents, document);
		}

		std::string GetAttributeValue(const rapidxml::xml_node<>* node, const char* attributeName) {
			if (!node) return {};
			if (const auto* attribute = node->first_attribute(attributeName)) {
				return Trim(attribute->value());
			}
			return {};
		}

		std::string GetFirstChildValue(const rapidxml::xml_node<>* node, const char* childName) {
			if (!node) return {};
			if (const auto* child = node->first_node(childName)) {
				return Trim(child->value());
			}
			return {};
		}

		std::string GetItemIdentity(const rapidxml::xml_node<>* node) {
			std::string identity = GetAttributeValue(node, "Include");
			if (!identity.empty()) {
				return identity;
			}
			return GetAttributeValue(node, "Update");
		}

		std::string GetPackageVersion(const rapidxml::xml_node<>* node) {
			std::string version = GetAttributeValue(node, "Version");
			if (!version.empty()) return version;
			version = GetAttributeValue(node, "VersionOverride");
			if (!version.empty()) return version;
			version = GetFirstChildValue(node, "Version");
			if (!version.empty()) return version;
			return GetFirstChildValue(node, "VersionOverride");
		}

		std::string GetSimpleAssemblyName(std::string identity) {
			const size_t commaPos = identity.find(',');
			if (commaPos != std::string::npos) {
				identity = identity.substr(0, commaPos);
			}
			return Trim(identity);
		}

		void AddOrUpdateInstalledPackage(std::vector<PackageInfo>& installed, PackageInfo info) {
			for (auto& existing : installed) {
				if (existing.Id != info.Id || existing.SourceType != info.SourceType) {
					continue;
				}
				if (existing.Version.empty() && !info.Version.empty()) {
					existing.Version = info.Version;
					existing.InstalledVersion = info.Version;
				}
				return;
			}
			installed.push_back(std::move(info));
		}
	} // namespace

	std::vector<PackageInfo> ParseInstalledPackagesFromXml(
		std::string_view xmlText,
		const std::unordered_map<std::string, std::string>& centralVersions) {

		std::vector<PackageInfo> installed;

		XmlDocument document;
		if (!ParseXmlString(std::string(xmlText), document)) {
			return installed;
		}

		const rapidxml::xml_node<>* root = document.Document.first_node("Project");
		if (!root) {
			return installed;
		}

		for (const auto* itemGroup = root->first_node("ItemGroup"); itemGroup; itemGroup = itemGroup->next_sibling("ItemGroup")) {
			for (const auto* node = itemGroup->first_node(); node; node = node->next_sibling()) {
				const std::string nodeName = node->name();

				if (nodeName == "PackageReference") {
					const std::string packageId = GetItemIdentity(node);
					if (packageId.empty()) {
						continue;
					}

					PackageInfo info;
					info.Id = packageId;
					info.Version = GetPackageVersion(node);
					if (info.Version.empty()) {
						if (const auto it = centralVersions.find(packageId); it != centralVersions.end()) {
							info.Version = it->second;
						}
					}
					info.IsInstalled = true;
					info.InstalledVersion = info.Version;
					info.SourceType = PackageSourceType::NuGet;
					info.SourceName = "NuGet";
					AddOrUpdateInstalledPackage(installed, std::move(info));
					continue;
				}

				if (nodeName != "Reference") {
					continue;
				}

				std::string name = GetSimpleAssemblyName(GetItemIdentity(node));
				if (name.empty()) {
					continue;
				}

				// Skip system references and the engine's own ScriptCore reference —
				// Axiom-ScriptCore is always emitted into every user .csproj by the
				// project generator (it's the engine core, not a user package).
				if (name.find("System") == 0 || name.find("Microsoft") == 0) {
					continue;
				}
				if (name == "Axiom-ScriptCore") {
					continue;
				}

				PackageInfo info;
				info.Id = name;
				info.IsInstalled = true;
				info.SourceType = PackageSourceType::GitHub;
				info.SourceName = "Engine";
				AddOrUpdateInstalledPackage(installed, std::move(info));
			}
		}

		return installed;
	}

	std::unordered_map<std::string, std::string> LoadCentralPackageVersionsFromDisk(
		const std::filesystem::path& csprojPath) {

		std::unordered_map<std::string, std::string> versions;
		std::vector<std::filesystem::path> propsFiles;

		// MSBuild searches upward from the .csproj for Directory.Packages.props,
		// applying every level. Deepest wins, so we apply props in deepest-last order.
		std::filesystem::path currentDirectory = csprojPath.parent_path();
		while (!currentDirectory.empty()) {
			const std::filesystem::path propsPath = currentDirectory / "Directory.Packages.props";
			if (std::filesystem::exists(propsPath)) {
				propsFiles.push_back(propsPath);
			}
			const std::filesystem::path parent = currentDirectory.parent_path();
			if (parent == currentDirectory) {
				break;
			}
			currentDirectory = parent;
		}

		for (auto it = propsFiles.rbegin(); it != propsFiles.rend(); ++it) {
			XmlDocument propsDocument;
			if (!ParseXmlFile(*it, propsDocument)) {
				continue;
			}

			const rapidxml::xml_node<>* root = propsDocument.Document.first_node("Project");
			if (!root) continue;

			for (const auto* itemGroup = root->first_node("ItemGroup"); itemGroup; itemGroup = itemGroup->next_sibling("ItemGroup")) {
				for (const auto* node = itemGroup->first_node("PackageVersion"); node; node = node->next_sibling("PackageVersion")) {
					const std::string packageId = GetItemIdentity(node);
					if (packageId.empty()) continue;
					const std::string version = GetPackageVersion(node);
					if (!version.empty()) {
						versions[packageId] = version;
					}
				}
			}
		}

		return versions;
	}

} // namespace Axiom
