#pragma once
#include "Scene/EntityHandle.hpp"
#include "Components/ComponentUtils.hpp"
#include "Components/General/EntityMetaDataComponent.hpp"
#include "Core/Export.hpp"
#include <span>
#include <string>
#include <type_traits>

namespace Index {
	class Scene;
	// Forward-declared so the script-branch constraints
	// (`std::is_base_of_v<NativeScript, T>`) parse without dragging
	// NativeScript.hpp / ScriptComponent.hpp / Scene.hpp into the top
	// of this file. The full definition arrives via the
	// `Scripting/EntityScriptOps.hpp` include at the BOTTOM of this
	// file — by then the Entity class is fully defined, so the cycle
	// Entity.hpp -> EntityScriptOps.hpp -> ScriptSystem.hpp ->
	// Scene.hpp -> Entity.hpp (no-op via guard) resolves with every
	// inline reference seeing a complete Entity. The script-branch
	// templates only instantiate at the call site (user TU), where
	// NativeScript IS complete because the same EntityScriptOps.hpp
	// include re-runs through.
	class NativeScript;

	// Helper-template forward declarations so the Entity script-branch
	// definitions further down can reference them at PARSE time, not
	// just at instantiation. The full bodies live in
	// `Scripting/EntityScriptOps.hpp` (included at the bottom of this
	// file). Without these forward decls, including Entity.hpp through
	// ScriptSystem.hpp (which happens during EntityScriptOps.hpp's own
	// processing of #include "Scripting/ScriptSystem.hpp") would
	// produce a parse error: the compiler would see `EntityScriptOps::`
	// before the namespace exists.
	namespace EntityScriptOps {
		template<typename T>
			requires std::is_base_of_v<NativeScript, T>
		T* AddScriptToEntity(Scene* scene, EntityHandle entity);

		template<typename T>
			requires std::is_base_of_v<NativeScript, T>
		T* GetScriptOnEntity(Scene* scene, EntityHandle entity);

		template<typename T>
			requires std::is_base_of_v<NativeScript, T>
		bool HasScriptOnEntity(Scene* scene, EntityHandle entity);

		template<typename T>
			requires std::is_base_of_v<NativeScript, T>
		bool TryGetScriptOnEntity(Scene* scene, EntityHandle entity, T*& out);

		template<typename T>
			requires std::is_base_of_v<NativeScript, T>
		bool RemoveScriptFromEntity(Scene* scene, EntityHandle entity);
	}

	class INDEX_API Entity {
		friend class Scene;
		friend class EntityHelper;

	public:
		static Entity Create();
		// Named-overload: creates a runtime entity in the active scene
		// and attaches a NameComponent. Equivalent to
		// Scene::CreateRuntimeEntity(name). Empty name → unnamed entity
		// (no NameComponent added) — matches the Scene-side behavior.
		static Entity Create(const std::string& name);

		// Runtime clone / prefab spawn. Picks the right path based on
		// the source's origin:
		//   - prefab asset → SceneSerializer::InstantiatePrefab
		//   - runtime / scene entity → Scene::CloneEntity (Component-only)
		// Returns Entity::Null when the source is invalid or no active
		// scene is loaded.
		static Entity Instantiate(Entity source);

		static void Destroy(Entity entity);
		// Schedule the destruction `delay` seconds in the future. The
		// entity stays valid (and addressable) until the next
		// Scene::UpdateSystems tick on which (accumulated dt) >= delay.
		// Non-positive delay collapses to immediate destruction.
		// Forwarded to Scene::DestroyEntity(handle, delay).
		static void Destroy(Entity entity, float delay);

		// Variadic AND-semantics complement to HasAnyComponent<Ts...>:
		// returns true only when EVERY listed component is present.
		// Implementation is a fold over the existing per-T HasComponent.
		// No 8-arity cap — C++ templates accept arbitrary packs.
		template<typename... TComponent>
		bool HasComponents() const {
			if (!IsValid()) return false;
			return (HasComponent<TComponent>() && ...);
		}

		// Convenience: Create(name) + AddComponent<Ti>() per type in the
		// pack, in declaration order. Default-constructs each component
		// — for value-init use Create(name) and per-call AddComponent,
		// or EntityCommandBuffer for batched value attaches.
		template<typename... TComponent>
		static Entity CreateWith(const std::string& name = "") {
			Entity e = name.empty() ? Create() : Create(name);
			if (!e.IsValid()) return e;
			(e.AddComponent<TComponent>(), ...);
			return e;
		}

		void Destroy();
		// Sentinel "no entity" value. Const so callers can't accidentally
		// reseat the global to a real entity handle and corrupt every
		// downstream null-check. Defined in Entity.cpp.
		static const Entity Null;

		// Scene-bound placeholder: an Entity with no underlying handle but
		// a live Scene pointer, for code paths that need to dispatch
		// scene-scoped logic (e.g. PropertyDrawer's MarkSceneDirty hook,
		// which only reads Scene* via Entity::GetScene()) through an API
		// that takes an Entity. The returned wrapper reports IsValid() ==
		// false and asserts on any component operation — it exists purely
		// to carry the scene reference, not to stand in for a real entity.
		//
		// Public so editor / inspector translation units can call it without
		// being added to the Entity friend list; the private constructors
		// remain off-limits to general callers.
		static Entity MakeScenePlaceholder(Scene& scene);


		// ── Component branch (ECS data — Transform2D, SpriteRenderer, …) ───
		// Constrained out of the NativeScript-derived case so script
		// types fall through to the script-branch overloads below.
		// CS0111-style overload ambiguity doesn't apply in C++; concept
		// disambiguation picks the unique candidate at the call site.
		template<typename TComponent, typename... Args>
			requires (!std::is_empty_v<TComponent> && !std::is_base_of_v<NativeScript, TComponent>)
		TComponent& AddComponent(Args&&... args) {
			EnsureValid("Cannot add component to invalid entity");
			return  ComponentUtils::AddComponent<TComponent>(*m_Registry, m_EntityHandle, std::forward<Args>(args)...);
		}

		template<typename TTag>
			requires (std::is_empty_v<TTag> && !std::is_base_of_v<NativeScript, TTag>)
		void AddComponent() {
			EnsureValid("Cannot add tag component to invalid entity");
			ComponentUtils::AddComponent<TTag>(*m_Registry, m_EntityHandle);
		}

		template<typename TComponent>
			requires (!std::is_base_of_v<NativeScript, TComponent>)
		bool HasComponent() const {
			if (!IsValid()) {
				return false;
			}
			return  ComponentUtils::HasComponent<TComponent>(*m_Registry, m_EntityHandle);
		}

		template<typename... TComponent>
		bool HasAnyComponent() const {
			if (!IsValid()) {
				return false;
			}
			return  ComponentUtils::HasAnyComponent<TComponent...>(*m_Registry, m_EntityHandle);
		}

		template<typename TComponent>
			requires (!std::is_base_of_v<NativeScript, TComponent>)
		TComponent& GetComponent() {
			EnsureValid("Cannot get component from invalid entity");
			return  ComponentUtils::GetComponent<TComponent>(*m_Registry, m_EntityHandle);
		}

		template<typename TComponent>
			requires (!std::is_base_of_v<NativeScript, TComponent>)
		const TComponent& GetComponent() const {
			EnsureValid("Cannot get component from invalid entity");
			return  ComponentUtils::GetComponent<TComponent>(*m_Registry, m_EntityHandle);
		}

		template<typename TComponent>
			requires (!std::is_base_of_v<NativeScript, TComponent>)
		bool TryGetComponent(TComponent*& out) {
			if (!IsValid()) {
				out = nullptr;
				return false;
			}
			out = ComponentUtils::TryGetComponent<TComponent>(*m_Registry, m_EntityHandle);
			return out != nullptr;
		}

		template<typename TComponent>
			requires (!std::is_base_of_v<NativeScript, TComponent>)
		void RemoveComponent() {
			if (!IsValid()) {
				return;
			}
			ComponentUtils::RemoveComponent<TComponent>(*m_Registry, m_EntityHandle);
		}

		// ── Script branch (NativeScript-derived) ────────────────────────
		// Selected by concept disambiguation when T derives from
		// NativeScript. Returns T* (nullable) rather than T& — a script
		// MAY be absent, and the caller pattern
		// `if (auto* s = e.GetComponent<MyScript>()) ...` matches Unity
		// /Unreal idioms. Definitions live in `EntityScriptOps.hpp` (see
		// include below the class) so the heavy ScriptComponent /
		// NativeScriptHost machinery isn't pulled into every Entity.hpp
		// consumer.
		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		TScript* AddComponent();

		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		bool HasComponent() const;

		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		TScript* GetComponent();

		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		const TScript* GetComponent() const;

		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		bool TryGetComponent(TScript*& out);

		template<typename TScript>
			requires std::is_base_of_v<NativeScript, TScript>
		void RemoveComponent();

		std::string GetName() const;
		const EntityMetaData* GetMetaData() const;
		EntityOrigin GetOrigin() const;
		EntityID GetRuntimeID() const;
		AssetGUID GetSceneGUID() const;
		AssetGUID GetPrefabGUID() const;
		bool IsSceneEntity() const;
		bool IsPrefabInstance() const;
		bool IsRuntime() const;

		EntityHandle GetHandle() const;
		Scene* GetScene() { return m_Scene; }
		const Scene* GetScene() const { return m_Scene; }
		bool IsValid() const { return m_Registry && m_EntityHandle != entt::null && m_Registry->valid(m_EntityHandle); }

		void SetStatic(bool isStatic);
		void SetEnabled(bool enabled);

		// ── Parent-child hierarchy ────────────────────────────────────
		// Reparent this entity. Pass Entity::Null to detach (make this a
		// root). Detaches from the previous parent's child list first.
		// Refuses cycles (passing a descendant of `this`) silently — the
		// call becomes a no-op so a buggy editor drag-drop can't corrupt
		// the scene graph.
		void SetParent(Entity parent);

		// Returns Entity::Null when this is a root.
		Entity GetParent() const;

		// Direct children only. Returns a non-mutable view into the underlying
		// HierarchyComponent::Children — structural edits must go through
		// SetParent so the parent's child list and each child's parent pointer
		// stay in sync. The previous overload returned `const vector&` which
		// only documented immutability; a const_cast away from that reference
		// would silently corrupt hierarchy invariants.
		std::span<const EntityHandle> GetChildren() const;

		bool HasParent() const;
		bool IsAncestorOf(Entity other) const;

	private:
		void EnsureValid(const char* message) const;

		explicit Entity(EntityHandle e, Scene& scene);
		explicit Entity(EntityHandle e, Scene* scene);
		EntityHandle    m_EntityHandle;
		entt::registry* m_Registry;
		Scene*          m_Scene;
	};

	inline bool operator==(const Entity& a, const Entity& b) { return a.GetHandle() == b.GetHandle(); }
	inline bool operator!=(const Entity& a, const Entity& b) { return !(a == b); }
}

// Pulls in NativeScript / Scene / ScriptSystem / ScriptComponent and
// defines Index::EntityScriptOps helpers. Placed AFTER `class Entity`
// is fully defined so the chain
//   EntityScriptOps.hpp -> ScriptSystem.hpp -> Scene.hpp -> Entity.hpp
// hits the Entity.hpp guard as a no-op when Entity is already complete.
#include "Scripting/EntityScriptOps.hpp"

namespace Index {

	// ── Script-branch template definitions (out-of-class) ───────────────
	// Lifted out of the class body so the call to
	// `EntityScriptOps::AddScriptToEntity` etc. — defined in the
	// EntityScriptOps.hpp include just above — is fully visible at the
	// point of definition. Two-phase lookup would handle this even with
	// inline-in-class bodies, but keeping them out-of-class makes the
	// dependency between Entity templates and the helpers explicit and
	// matches the include ordering described at the top of this file.

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	TScript* Entity::AddComponent() {
		if (!IsValid()) return nullptr;
		return EntityScriptOps::AddScriptToEntity<TScript>(m_Scene, m_EntityHandle);
	}

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	bool Entity::HasComponent() const {
		if (!IsValid()) return false;
		return EntityScriptOps::HasScriptOnEntity<TScript>(m_Scene, m_EntityHandle);
	}

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	TScript* Entity::GetComponent() {
		if (!IsValid()) return nullptr;
		return EntityScriptOps::GetScriptOnEntity<TScript>(m_Scene, m_EntityHandle);
	}

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	const TScript* Entity::GetComponent() const {
		if (!IsValid()) return nullptr;
		// const_cast for the helper call only — script storage IS
		// mutable per-frame (Update mutates fields, scene drives
		// lifecycle); the const overload exists for callers that
		// don't intend to mutate the returned pointer, but the
		// pointer itself targets non-const script storage by design.
		return EntityScriptOps::GetScriptOnEntity<TScript>(m_Scene, m_EntityHandle);
	}

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	bool Entity::TryGetComponent(TScript*& out) {
		if (!IsValid()) { out = nullptr; return false; }
		return EntityScriptOps::TryGetScriptOnEntity<TScript>(m_Scene, m_EntityHandle, out);
	}

	template<typename TScript>
		requires std::is_base_of_v<NativeScript, TScript>
	void Entity::RemoveComponent() {
		if (!IsValid()) return;
		(void)EntityScriptOps::RemoveScriptFromEntity<TScript>(m_Scene, m_EntityHandle);
	}

}
