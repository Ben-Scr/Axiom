#pragma once

// Registration helpers for `AxiomPackage_OnLoad` bodies.
//
// Without these, every package re-implements the same boilerplate to wire a
// component into the engine's ComponentRegistry — populate a ComponentInfo,
// optionally adapt an inspector function pointer, call SceneManager::Get()
// .RegisterComponentType<T>(info). The Tilemap2D package was the prototype
// for the pattern; these helpers extract it.
//
// All helpers run on the main thread inside the host's `AxiomPackage_OnLoad`
// dispatch — no synchronization is needed at registration time.

#include "Core/Assert.hpp"
#include "Scene/ComponentInfo.hpp"
#include "Scene/Entity.hpp"
#include "Scene/SceneManager.hpp"

#include <initializer_list>
#include <functional>
#include <span>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace Axiom::Package {

    // Registers a component type with the engine-wide ComponentRegistry.
    // The registry auto-fills has/add/remove/copyTo via the underlying
    // template — callers only supply the editor-visible fields.
    //
    //   displayName       Shown in the Add Component popup and inspector header.
    //   subcategory       Optional grouping ("Rendering", "Physics", ...). May be empty.
    //   serializedName    Stable JSON key used by the scene serializer. If empty,
    //                     the engine derives a name from the C++ type — pass an
    //                     explicit value if you ever rename the type.
    //   drawInspector     Optional inspector pointer. Receives the multi-select
    //                     entity span; size==1 for a single selection.
    //   category          Defaults to ComponentCategory::Component. Pass Tag for
    //                     empty-tag types.
    template <typename TComponent>
    void RegisterComponent(
        const char* displayName,
        const char* subcategory                                = "",
        const char* serializedName                             = "",
        void (*drawInspector)(std::span<const Entity>)         = nullptr,
        ComponentCategory category = ComponentCategory::Component) {

        ComponentInfo info(displayName, subcategory ? subcategory : "", category);
        if (serializedName && *serializedName) {
            info.serializedName = serializedName;
        }
        info.drawInspector = drawInspector;
        SceneManager::Get().RegisterComponentType<TComponent>(info);
    }

    // Adapter shims: erase TComponent so ComponentInfo can hold a flat pair
    // of function pointers without templating. C++ rejects static data
    // members in local classes, so this lives at namespace scope. One pair
    // of static trampolines per TComponent instantiation; the templated
    // statics are inline so each TU sees the same instance.
    template <typename TComponent>
    struct ComponentSerializerTrampoline {
        static inline Json::Value (*s_Serialize)(const TComponent&) = nullptr;
        static inline void (*s_Deserialize)(TComponent&, const Json::Value&) = nullptr;
        static Json::Value SerializeAdapter(Entity entity) {
            return s_Serialize(entity.GetComponent<TComponent>());
        }
        static void DeserializeAdapter(Entity entity, const Json::Value& value) {
            s_Deserialize(entity.GetComponent<TComponent>(), value);
        }
    };

    // Same as RegisterComponent but additionally wires the component into the
    // generic scene-serializer path. After this call the component round-trips
    // in .scene and .prefab files like every built-in. `serialize` and
    // `deserialize` operate on the live `TComponent&` — the helper handles
    // the registry lookup so package authors don't touch EnTT directly.
    //
    //   serialize     Called when the entity is being written to JSON. Return
    //                 the JSON value for THIS component's slot — the engine
    //                 stores it under `serializedName`.
    //   deserialize   Called when the entity is being read from JSON. The
    //                 component is already added to the entity by the engine
    //                 before this fires; just populate the fields from `value`.
    template <typename TComponent>
    void RegisterSerializableComponent(
        const char* displayName,
        const char* subcategory,
        const char* serializedName,
        void (*drawInspector)(std::span<const Entity>),
        Json::Value (*serialize)(const TComponent&),
        void (*deserialize)(TComponent&, const Json::Value&),
        ComponentCategory category = ComponentCategory::Component,
        std::vector<PropertyDescriptor> properties = {}) {

        ComponentInfo info(displayName, subcategory ? subcategory : "", category);
        if (serializedName && *serializedName) {
            info.serializedName = serializedName;
        }
        info.drawInspector = drawInspector;
        info.properties = std::move(properties);

        using Trampoline = ComponentSerializerTrampoline<TComponent>;
        Trampoline::s_Serialize = serialize;
        Trampoline::s_Deserialize = deserialize;
        info.serialize = serialize ? &Trampoline::SerializeAdapter : nullptr;
        info.deserialize = deserialize ? &Trampoline::DeserializeAdapter : nullptr;

        SceneManager::Get().RegisterComponentType<TComponent>(info);
    }

    // Install a viewport gizmo callback for an already-registered component.
    // The editor's selection pass walks every registered component on the
    // selected entity and invokes this callback for each one whose info has
    // it set — see ComponentInfo::drawEditorGizmo for the calling discipline.
    //
    // Call AFTER RegisterComponent / RegisterSerializableComponent for the
    // same TComponent: the helper looks up the existing ComponentInfo by
    // type id and patches the callback in place. A second call replaces the
    // previous one (last-call-wins) so a package can rewire its gizmo at
    // runtime without re-registering the whole component.
    //
    // Asserts at registration time if TComponent isn't registered yet — this
    // is almost always a missing/misordered RegisterComponent call, so failing
    // loudly beats silently dropping the gizmo and confusing the package author.
    template <typename TComponent>
    void SetEditorGizmo(void (*drawEditorGizmo)(Entity)) {
        const std::type_index typeId(typeid(TComponent));
        bool patched = false;
        SceneManager::Get().GetComponentRegistry().ForEachComponentInfo(
            [&](const std::type_index& id, ComponentInfo& info) {
                if (id == typeId) {
                    info.drawEditorGizmo = drawEditorGizmo;
                    patched = true;
                }
            });
        AIM_CORE_ASSERT(patched, AxiomErrorCode::InvalidValue,
            "SetEditorGizmo<T>: component type is not registered yet — "
            "call RegisterComponent / RegisterSerializableComponent first.");
    }

    // Registers a package-owned entity preset for the editor's Create menu.
    // `menuPath` is the tab/menu the preset appears under, e.g. "2D Entity";
    // `label` is the clickable item text; `defaultName` is the base entity
    // name before the editor's project-specific unique-name pass runs.
    inline void RegisterEntityPreset(
        const char* menuPath,
        const char* label,
        const char* defaultName,
        std::function<Entity(Scene&)> create) {

        SceneManager::Get().RegisterEntityPreset({
            menuPath ? menuPath : "",
            label ? label : "",
            defaultName ? defaultName : (label ? label : "Entity"),
            std::move(create)
        });
    }

    // Convenience for the common "new entity plus one component" preset.
    // Packages such as Tilemap2D can surface themselves under "2D Entity"
    // without the editor hardcoding package names or component display names.
    template <typename TComponent>
    void RegisterComponentEntityPreset(
        const char* menuPath,
        const char* label,
        const char* defaultName = nullptr) {

        std::string baseName = defaultName ? defaultName : (label ? label : "Entity");
        RegisterEntityPreset(menuPath, label, baseName.c_str(),
            [baseName](Scene& scene) {
                Entity entity = scene.CreateEntity(baseName);
                SceneManager::Get().GetComponentRegistry().AddWithDependencies(entity, typeid(TComponent));
                return entity;
            });
    }

    // Declare a symmetric conflict between two already-registered components.
    // Either side may declare it — the registry's HasConflict lookup walks
    // both directions. Packages call this for each rendering-style component
    // they introduce against the engine's built-ins (SpriteRenderer / Image /
    // ParticleSystem2D) so the editor's Add Component popup hides the
    // conflicting entry on selected entities.
    template <typename A, typename B>
    void DeclareComponentConflict() {
        const std::type_index aId(typeid(A));
        const std::type_index bId(typeid(B));
        SceneManager::Get().GetComponentRegistry().ForEachComponentInfo(
            [&](const std::type_index& id, ComponentInfo& info) {
                if (id == aId) {
                    bool present = false;
                    for (const auto& c : info.conflictsWith) if (c == bId) { present = true; break; }
                    if (!present) info.conflictsWith.push_back(bId);
                }
                else if (id == bId) {
                    bool present = false;
                    for (const auto& c : info.conflictsWith) if (c == aId) { present = true; break; }
                    if (!present) info.conflictsWith.push_back(aId);
                }
            });
    }

} // namespace Axiom::Package
