#pragma once
#include "Core/Assert.hpp"
#include "Core/Exceptions.hpp"
#include "Core/Log.hpp"
#include "Scene/ComponentInfo.hpp"

#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Axiom {
    class ComponentRegistry {
    public:
        template<typename T>
        void Register(ComponentInfo info) {
            const std::type_index id = typeid(T);
            const auto existing = m_map.find(id);
            if (existing != m_map.end()) {
                if (info.serializedName.empty()) info.serializedName = existing->second.serializedName;
                if (!info.drawInspector) info.drawInspector = existing->second.drawInspector;
                if (info.properties.empty()) info.properties = existing->second.properties;
                if (info.conflictsWith.empty()) info.conflictsWith = existing->second.conflictsWith;
                if (info.dependsOn.empty()) info.dependsOn = existing->second.dependsOn;
                // Preserve serialize/deserialize: AttachInspector re-registers and would otherwise drop them.
                if (!info.serialize) info.serialize = existing->second.serialize;
                if (!info.deserialize) info.deserialize = existing->second.deserialize;
                // Preserve onAdd across re-registration the same way drawInspector
                // does — built-ins set onAdd up-front and AttachInspector should
                // not silently drop it.
                if (!info.onAdd) info.onAdd = existing->second.onAdd;
            }

            info.typeId = id;
            info.has = [](Entity e) { return e.HasComponent<T>(); };
            info.add = [](Entity e) { e.AddComponent<T>(); };
            info.remove = [](Entity e) { e.RemoveComponent<T>(); };

            // Auto-wire raw component pointer access for non-empty types so
            // the ScriptCore ref-API can target every registered component
            // without per-type plumbing. Empty tag types skip this — there's
            // no payload to expose and EnTT's empty-type storage doesn't
            // hand out addressable instances anyway.
            if constexpr (!std::is_empty_v<T>) {
                info.getRaw = [](Entity e) -> void* {
                    if (!e.HasComponent<T>()) return nullptr;
                    return static_cast<void*>(&e.GetComponent<T>());
                };
                info.fillRawPointers = [](entt::registry& registry, void** outPointers, int maxRows) -> int {
                    int count = 0;
                    auto view = registry.view<T>();
                    for (auto&& [entity, component] : view.each()) {
                        (void)entity;
                        if (outPointers && count < maxRows) {
                            outPointers[count] = static_cast<void*>(&component);
                        }
                        ++count;
                    }
                    return count;
                };
                info.rawSize = sizeof(T);
            }

            // If the caller provided a custom copyTo (or a previous registration
            // installed one), keep it. Components that hold scene-bound runtime
            // state — e.g. ParticleSystem2DComponent's m_EmitterScene/Entity —
            // need a copy that re-binds against the destination, because the
            // raw value-copy path bypasses on_construct hooks.
            const bool preserveExistingCopyTo =
                info.copyTo != nullptr ||
                (existing != m_map.end() && existing->second.copyTo != nullptr);
            if (preserveExistingCopyTo && info.copyTo == nullptr) {
                info.copyTo = existing->second.copyTo;
            }

            if (!preserveExistingCopyTo) {
                if constexpr (!std::is_empty_v<T>) {
                    info.copyTo = [](Entity src, Entity dst) {
                        if (!src.HasComponent<T>()) return;
                        if (dst.HasComponent<T>())
                            dst.GetComponent<T>() = src.GetComponent<T>();
                        else
                            dst.AddComponent<T>(src.GetComponent<T>());
                    };
                } else {
                    info.copyTo = [](Entity src, Entity dst) {
                        if (src.HasComponent<T>() && !dst.HasComponent<T>())
                            dst.AddComponent<T>();
                    };
                }
            }

            m_map[id] = std::move(info);
        }

        const auto& All() const { return m_map; }

        /// Resolve ComponentInfo for hybrid inspectors that mix DrawAll with custom widgets.
        template <typename T>
        const ComponentInfo* GetInfo() const {
            const auto it = m_map.find(typeid(T));
            return it != m_map.end() ? &it->second : nullptr;
        }

        template <typename F>
        void ForEachComponentInfo(F&& fn) {
            for (auto& [id, info] : m_map)
                fn(id, info);
        }

        template <typename F>
        void ForEachComponentInfo(F&& fn) const {
            for (const auto& [id, info] : m_map)
                fn(id, info);
        }

        void CopyComponents(Entity src, Entity dst) const {
            for (const auto& [id, info] : m_map) {
                (void)id;
                if (info.copyTo) {
                    info.copyTo(src, dst);
                }
            }
        }

        /// Bidirectional conflict check; either side's declaration counts.
        bool HasConflict(Entity entity, std::type_index proposed) const {
            const auto proposedIt = m_map.find(proposed);
            const ComponentInfo* proposedInfo = (proposedIt != m_map.end()) ? &proposedIt->second : nullptr;

            for (const auto& [existingId, existingInfo] : m_map) {
                if (existingId == proposed) continue;
                if (!existingInfo.has || !existingInfo.has(entity)) continue;

                // proposed → existing
                if (proposedInfo) {
                    for (const std::type_index& conflict : proposedInfo->conflictsWith) {
                        if (conflict == existingId) return true;
                    }
                }
                // existing → proposed
                for (const std::type_index& conflict : existingInfo.conflictsWith) {
                    if (conflict == proposed) return true;
                }
            }
            return false;
        }

        /// Add a component to `entity` plus everything its `dependsOn` chain
        /// pulls in (transitively). Idempotent — already-present components
        /// are skipped. Cycles are guarded against via a visited set, so a
        /// (mistaken) declaration of A depends on B depends on A still
        /// terminates instead of recursing forever.
        ///
        /// A dependency that would violate an existing `conflictsWith` on
        /// the entity is skipped with a warning rather than failing the
        /// parent add — the policy is deliberately "less clicking, never
        /// force." Removal is unrestricted; nothing tracks dependents.
        ///
        /// Returns true if the requested component is on the entity after
        /// the call (added or already present), false if the type isn't
        /// registered or its add would itself conflict.
        bool AddWithDependencies(Entity entity, std::type_index type) const {
            std::unordered_set<std::type_index> visited;
            return AddWithDependenciesImpl(entity, type, visited, /*isRoot=*/true);
        }

        /// Debug-only sweep: every conflict declaration must be symmetric.
        /// `A.conflictsWith → B` requires `B.conflictsWith → A`. The lookup paths
        /// (HasConflict / TypesConflict) accept either side as authoritative, so
        /// asymmetry is currently silent — the validator catches stale registrations
        /// where one side moved or got renamed without updating its mirror.
        ///
        /// Wrapped in AIM_DEBUG so shipping builds skip the O(N*M) walk; call once
        /// after the built-in registration pass completes.
#ifdef AIM_DEBUG
        void ValidateConflictSymmetry() const {
            for (const auto& [aId, aInfo] : m_map) {
                for (const std::type_index& bId : aInfo.conflictsWith) {
                    const auto bIt = m_map.find(bId);
                    bool symmetric = false;
                    if (bIt != m_map.end()) {
                        for (const std::type_index& back : bIt->second.conflictsWith) {
                            if (back == aId) { symmetric = true; break; }
                        }
                    }
                    AIM_CORE_ASSERT(symmetric, AxiomErrorCode::InvalidValue,
                        std::string("Asymmetric conflict declaration: ") + aId.name() + " <-> " + bId.name() +
                        " (declared on first side only - DeclareConflict<A, B>() should add both directions)");
                }
            }
        }
#else
        void ValidateConflictSymmetry() const {}
#endif

        /// Type-pair check for callers that already have type_index values.
        bool TypesConflict(std::type_index a, std::type_index b) const {
            if (a == b) return false;
            const auto ai = m_map.find(a);
            if (ai != m_map.end()) {
                for (const std::type_index& c : ai->second.conflictsWith) {
                    if (c == b) return true;
                }
            }
            const auto bi = m_map.find(b);
            if (bi != m_map.end()) {
                for (const std::type_index& c : bi->second.conflictsWith) {
                    if (c == a) return true;
                }
            }
            return false;
        }

    private:
        bool AddWithDependenciesImpl(Entity entity, std::type_index type,
            std::unordered_set<std::type_index>& visited, bool isRoot) const
        {
            if (!visited.insert(type).second) return true;

            const auto it = m_map.find(type);
            if (it == m_map.end() || !it->second.add || !it->second.has) return false;
            const ComponentInfo& info = it->second;

            if (info.has(entity)) return true;

            if (HasConflict(entity, type)) {
                // Root path: caller is responsible for popup-level filtering, so
                // a conflicting root means something upstream skipped a check —
                // refuse with no warning to avoid log spam from legitimate
                // "user picked a now-conflicting component on a multi-edit"
                // scenarios. Dependency path: warn so silent skips are visible.
                if (!isRoot) {
                    AIM_CORE_WARN_TAG("ComponentRegistry",
                        "Skipping auto-add of dependency '{}' — conflicts with a component already on the entity",
                        info.displayName.empty() ? type.name() : info.displayName);
                }
                return false;
            }

            for (const std::type_index& dep : info.dependsOn) {
                AddWithDependenciesImpl(entity, dep, visited, /*isRoot=*/false);
            }

            info.add(entity);
            // Post-add hook: only fires when the component is added through the
            // user-facing AddWithDependencies path (Inspector "Add Component"
            // popup, dependency-chain pulls). Scripting / scene-load callers
            // that drive `info.add` or `entity.AddComponent<T>()` directly
            // skip this — they write the component's fields explicitly and
            // shouldn't get a "smart" inheritance fighting their values.
            if (info.onAdd) {
                info.onAdd(entity);
            }
            return true;
        }

        std::unordered_map<std::type_index, ComponentInfo> m_map;
    };
}
