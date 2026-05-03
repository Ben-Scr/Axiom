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

#include "Scene/ComponentInfo.hpp"
#include "Scene/Entity.hpp"
#include "Scene/SceneManager.hpp"

#include <initializer_list>
#include <span>
#include <string>
#include <typeindex>
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
