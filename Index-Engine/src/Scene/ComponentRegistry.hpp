#pragma once
#include "Core/Assert.hpp"
#include "Core/Exceptions.hpp"
#include "Core/Log.hpp"
#include "Components/Tags.hpp"
#include "Scene/ComponentInfo.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Index::detail {
    // FNV-1a 64-bit. Stable across builds, platforms, and ABIs — safe to
    // embed in serialized assets. constexpr so callers can hash component
    // names at compile time (e.g. the binding layer's "Transform2D" → hash
    // cache populated from a static initializer).
    constexpr uint64_t FnvHash64(std::string_view str) noexcept {
        constexpr uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
        constexpr uint64_t kPrime = 0x100000001b3ULL;
        uint64_t h = kOffsetBasis;
        for (char c : str) {
            h ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
            h *= kPrime;
        }
        return h;
    }
}

namespace Index {
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
                // Same preservation for the unified-archive callback. The
                // serializationVersion is preserved iff the current
                // registration didn't set it explicitly (i.e. still default
                // 1) — that way bumping the version in a single registration
                // site doesn't get silently overwritten by an
                // AttachInspector pass that re-registers without specifying
                // a version.
                if (!info.serializeArchive) info.serializeArchive = existing->second.serializeArchive;
                if (info.serializationVersion == 1 && existing->second.serializationVersion != 1) {
                    info.serializationVersion = existing->second.serializationVersion;
                }
                // Preserve onAdd across re-registration the same way drawInspector
                // does — built-ins set onAdd up-front and AttachInspector should
                // not silently drop it.
                if (!info.onAdd) info.onAdd = existing->second.onAdd;
                // Preserve a previously-assigned stable typeIdU32 across the
                // AttachInspector re-registration path. The ID is published to
                // the managed side at startup and embedded in EntityCommandBuffer
                // wire payloads — silently reassigning it would invalidate every
                // ECB record made before the re-registration.
                if (info.typeIdU32 == 0) info.typeIdU32 = existing->second.typeIdU32;
                // Likewise preserve a custom emplaceFromBytes from the first
                // registration (e.g. ParticleSystem2DComponent's rebinding
                // emplacer would otherwise be replaced by the auto-wired
                // trivial-memcpy emplacer below).
                if (!info.emplaceFromBytes) info.emplaceFromBytes = existing->second.emplaceFromBytes;
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
                info.fillRawPointers = [](entt::registry& registry, void** outPointers, int maxRows, int enableFilter) -> int {
                    int count = 0;
                    auto view = registry.view<T>();
                    for (auto&& [entity, component] : view.each()) {
                        if (enableFilter == 1 && registry.all_of<DisabledTag>(entity)) continue;
                        if (enableFilter == 2 && !registry.all_of<DisabledTag>(entity)) continue;
                        if (outPointers && count < maxRows) {
                            outPointers[count] = static_cast<void*>(&component);
                        }
                        ++count;
                    }
                    return count;
                };
                info.rawSize = sizeof(T);

                // Auto-wire the ECB memcpy-from-bytes emplacer for EVERY
                // non-empty registered component. The memcpy path is safe
                // whenever the C# mirror's sizeof matches the native struct
                // — and that's exactly what `ComponentTypes<T>` enforces at
                // AppDomain load via `Entity_GetComponentSize`. A size
                // mismatch throws before the user assembly finishes loading,
                // so any payload that reaches this lambda is guaranteed to
                // be the right length for T.
                //
                // The earlier `is_trivially_copyable_v<T>` gate was too
                // strict: it excluded perfectly memcpy-safe components like
                // `SpriteRendererComponent` solely because they hold a
                // `UUID` member, whose user-declared copy constructor flips
                // `is_trivially_copyable` to false even though the
                // underlying payload is just a uint64_t. The bug was a
                // silent "AddComponent dropped" in ECB playback — see the
                // floating-dancing-pretzel plan for the post-mortem.
                //
                // Components whose C++ representation holds runtime state
                // that genuinely cannot survive a byte-level overwrite —
                // owning std::string / std::vector / smart pointers, scene-
                // bound emitter handles, etc. — MUST register a custom
                // `emplaceFromBytes` at registration time (same opt-in
                // pattern `ParticleSystem2DComponent` uses for `copyTo`).
                // The merge-preservation in the re-registration branch above
                // (`if (!info.emplaceFromBytes) info.emplaceFromBytes =
                // existing->second.emplaceFromBytes;`) keeps the custom
                // emplacer alive across `AttachInspector` re-registration.
                if (info.emplaceFromBytes == nullptr) {
                    info.emplaceFromBytes = [](entt::registry& r, EntityHandle e,
                                               const void* bytes, size_t size) {
                        IDX_CORE_ASSERT(size == sizeof(T), IndexErrorCode::InvalidValue,
                            "ComponentRegistry: emplaceFromBytes size mismatch for component");
                        T value;
                        std::memcpy(&value, bytes, sizeof(T));
                        r.emplace_or_replace<T>(e, std::move(value));
                    };
                }
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

            // Compute the serialized-name hash exactly once at registration
            // time. AttachInspector re-registers existing types — we recompute
            // here so a late-changing serializedName is honored. Empty names
            // hash to 0 (sentinel for "no lookup key").
            info.serializedNameHash = info.serializedName.empty()
                ? 0u
                : detail::FnvHash64(info.serializedName);

            // Maintain the hash → ComponentInfo* index in lockstep with m_map.
            // If a previous registration installed a different hash for this
            // typeId, drop the old entry first to avoid a dangling reference
            // pointing at the soon-to-be-replaced m_map slot.
            if (existing != m_map.end()) {
                const uint64_t oldHash = existing->second.serializedNameHash;
                if (oldHash != 0 && oldHash != info.serializedNameHash) {
                    const auto oldIt = m_hashIndex.find(oldHash);
                    if (oldIt != m_hashIndex.end() && oldIt->second == &existing->second) {
                        m_hashIndex.erase(oldIt);
                    }
                }
            }

            // Assign a stable u32 type ID on first registration. The vector
            // is 1-indexed so that 0 remains a sentinel for "unregistered"
            // in the wire format and in ComponentTypes<T>.NativeId on the
            // managed side. Re-registration paths preserve the ID via the
            // merge above, so this branch fires exactly once per type.
            if (info.typeIdU32 == 0) {
                if (m_byTypeId.empty()) {
                    m_byTypeId.push_back(nullptr); // reserve slot 0 as null
                }
                info.typeIdU32 = static_cast<uint32_t>(m_byTypeId.size());
                m_byTypeId.push_back(nullptr); // placeholder, fixed up below
            }

            auto inserted = m_map.insert_or_assign(id, std::move(info));
            ComponentInfo& stored = inserted.first->second;
            if (stored.serializedNameHash != 0) {
                m_hashIndex[stored.serializedNameHash] = &stored;
            }
            if (stored.typeIdU32 != 0 && stored.typeIdU32 < m_byTypeId.size()) {
                // unordered_map nodes are stable across rehash, so the
                // pointer stays valid even when later registrations grow
                // the underlying map.
                m_byTypeId[stored.typeIdU32] = &stored;
            }
        }

        /// O(1) lookup by stable u32 type ID. Returns nullptr for id 0 (the
        /// reserved "unregistered" sentinel) or for an id beyond the highest
        /// one assigned by Register. Used by EntityCommandBuffer playback to
        /// dispatch a recorded command to its component's emplacer in one
        /// vector indirection — no string compare, no hash, no map lookup.
        const ComponentInfo* GetByTypeId(uint32_t typeIdU32) const {
            if (typeIdU32 == 0 || typeIdU32 >= m_byTypeId.size()) return nullptr;
            return m_byTypeId[typeIdU32];
        }

        /// Number of stable IDs currently assigned (== the highest valid id).
        /// Useful for the managed-side resolver loop that walks every
        /// component name and looks up its id at AppDomain load.
        uint32_t GetTypeIdCount() const {
            // Slot 0 is the null sentinel — subtract it from the reported count.
            return m_byTypeId.empty() ? 0u : static_cast<uint32_t>(m_byTypeId.size() - 1);
        }

        const auto& All() const { return m_map; }

        /// O(1) lookup by FNV-1a hash of the component's `serializedName`.
        /// Used by the binary scene loader (v2 component table stores hashes,
        /// not strings) and by the script binding layer's AddComponentByHash
        /// fast path — both want to avoid the linear scan in FindByName.
        /// Returns nullptr if no component is registered with this hash, or
        /// if the only matching registration had an empty serializedName.
        const ComponentInfo* FindByHash(uint64_t serializedNameHash) const {
            if (serializedNameHash == 0) return nullptr;
            const auto it = m_hashIndex.find(serializedNameHash);
            return it != m_hashIndex.end() ? it->second : nullptr;
        }

        /// String-keyed wrapper. Hashes the name (no allocation, no std::string
        /// copy) and dispatches through FindByHash. Callers that already have
        /// the hash should prefer FindByHash directly.
        const ComponentInfo* FindBySerializedName(std::string_view name) const {
            return FindByHash(detail::FnvHash64(name));
        }

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
        /// Wrapped in IDX_DEBUG so shipping builds skip the O(N*M) walk; call once
        /// after the built-in registration pass completes.
#ifdef IDX_DEBUG
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
                    IDX_CORE_ASSERT(symmetric, IndexErrorCode::InvalidValue,
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
                    IDX_CORE_WARN_TAG("ComponentRegistry",
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
        // Hash → ComponentInfo* (pointers into m_map). Maintained in lockstep
        // with m_map by Register(). Pointers stay stable across re-registration
        // because `insert_or_assign` reuses the existing node's storage when
        // the key already exists.
        std::unordered_map<uint64_t, const ComponentInfo*> m_hashIndex;
        // typeIdU32 → ComponentInfo* (pointers into m_map). 1-indexed; slot 0
        // is reserved as a null sentinel matching the "unregistered" meaning
        // in the EntityCommandBuffer wire format. Grows monotonically — IDs
        // are never reused, even if a registration were ever removed, so the
        // managed-side cache and any persisted-by-id artifact stays valid.
        std::vector<const ComponentInfo*> m_byTypeId;
    };
}
