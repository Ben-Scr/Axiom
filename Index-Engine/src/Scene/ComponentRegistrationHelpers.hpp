#pragma once

#include "Inspector/PropertyDescriptor.hpp"
#include "Scene/ComponentCategory.hpp"
#include "Scene/ComponentInfo.hpp"
#include "Scene/SceneManager.hpp"

#include <string>
#include <typeindex>
#include <utility>
#include <vector>

// Shared registration helpers used by both BuiltInComponentRegistration.cpp
// (hand-written engine components) and the IndexComponentCodegen output in
// `src/Generated/` (user-defined C# components mirrored into C++ at build
// time). Living in `namespace Index::Codegen` keeps these template helpers
// out of the engine's public Index::* surface while still letting both
// registration sites share one definition.
namespace Index::Codegen {

    template <typename T>
    void RegisterComponent(SceneManager& sceneManager,
                           const std::string& displayName,
                           ComponentCategory category = ComponentCategory::Component,
                           const std::string& subcategory = "",
                           const std::string& serializedName = "",
                           std::vector<PropertyDescriptor> properties = {}) {
        ComponentInfo info{ displayName, subcategory, category };
        info.serializedName = serializedName;
        info.properties = std::move(properties);
        sceneManager.RegisterComponentType<T>(info);
    }

    // Symmetric conflict declaration. Adds B to A.conflictsWith AND A to
    // B.conflictsWith — the lookup paths (HasConflict / TypesConflict) accept
    // either side, but ValidateConflictSymmetry asserts both directions exist
    // so a partial declaration would trip a debug build.
    template <typename A, typename B>
    void DeclareConflict(SceneManager& sceneManager) {
        const std::type_index aId(typeid(A));
        const std::type_index bId(typeid(B));
        sceneManager.GetComponentRegistry().ForEachComponentInfo(
            [&](const std::type_index& id, ComponentInfo& info) {
                if (id == aId) {
                    bool present = false;
                    for (const auto& c : info.conflictsWith) if (c == bId) { present = true; break; }
                    if (!present) info.conflictsWith.push_back(bId);
                } else if (id == bId) {
                    bool present = false;
                    for (const auto& c : info.conflictsWith) if (c == aId) { present = true; break; }
                    if (!present) info.conflictsWith.push_back(aId);
                }
            });
    }

    // TDependent.dependsOn += TDependency. Directed (NOT symmetric):
    // the inverse declaration would mean "TDependency pulls in TDependent
    // on add," which is virtually never what we want.
    template <typename TDependent, typename TDependency>
    void DeclareDependency(SceneManager& sceneManager) {
        const std::type_index dependentId(typeid(TDependent));
        const std::type_index dependencyId(typeid(TDependency));
        sceneManager.GetComponentRegistry().ForEachComponentInfo(
            [&](const std::type_index& id, ComponentInfo& info) {
                if (id != dependentId) return;
                for (const auto& c : info.dependsOn) if (c == dependencyId) return;
                info.dependsOn.push_back(dependencyId);
            });
    }

}
