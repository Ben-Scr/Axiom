#include "pch.hpp"
#include "Scene/Entity.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/Scene.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/Transform2DComponent.hpp"
#include "Components/Tags.hpp"
#include "Core/Exceptions.hpp"
#include "Core/Log.hpp"


namespace Index {
	const Entity Entity::Null{ entt::null, static_cast<Scene*>(nullptr) };

	EntityHandle Entity::GetHandle() const {
		return m_EntityHandle;
	}

	Entity::Entity(EntityHandle e, Scene& scene)
		: Entity(e, &scene) {}

	Entity::Entity(EntityHandle e, Scene* scene)
		: m_EntityHandle(e), m_Registry(scene ? &scene->GetRegistry() : nullptr), m_Scene(scene) {}

	Entity Entity::MakeScenePlaceholder(Scene& scene) {
		// entt::null + a real scene pointer — IsValid() returns false so
		// any accidental component access fails fast, while GetScene()
		// hands back the live scene so dispatchers like PropertyDrawer's
		// MarkSceneDirty hook can do their work.
		return Entity(EntityHandle{ entt::null }, scene);
	}

	void Entity::EnsureValid(const char* message) const {
		if (!IsValid()) {
			IDX_THROW(IndexErrorCode::InvalidHandle, message);
		}
	}

	Entity Entity::Create() {
		Scene* activeScene = SceneManager::Get().GetActiveScene();
		if (!activeScene || !activeScene->IsLoaded()) {
			IDX_ERROR_TAG("Entity", "Cannot create entity: no active scene loaded");
			return Entity::Null;
		}

		return activeScene->CreateRuntimeEntity();
	}

	void Entity::Destroy(Entity entity) {
		entity.Destroy();
	}

	void Entity::Destroy() {
		if (!m_Registry || !m_Scene || m_EntityHandle == entt::null) {
			return;
		}

		const EntityHandle entityHandle = m_EntityHandle;
		Scene* owningScene = m_Scene;

		m_EntityHandle = entt::null;
		m_Registry = nullptr;
		m_Scene = nullptr;

		if (owningScene->IsValid(entityHandle)) {
			owningScene->DestroyEntity(entityHandle);
		}
	}

	std::string Entity::GetName() const {
		if (HasComponent<NameComponent>()) {
			return GetComponent<NameComponent>().Name;
		}
		else {
			return "Unnamed Entity (" + std::to_string(static_cast<std::uint32_t>(m_EntityHandle)) + ")";
		}
	}

	const EntityMetaData* Entity::GetMetaData() const {
		return m_Scene ? m_Scene->GetEntityMetaData(m_EntityHandle) : nullptr;
	}

	EntityOrigin Entity::GetOrigin() const {
		return m_Scene ? m_Scene->GetEntityOrigin(m_EntityHandle) : EntityOrigin::Runtime;
	}

	EntityID Entity::GetRuntimeID() const {
		return m_Scene ? m_Scene->GetRuntimeID(m_EntityHandle) : 0;
	}

	AssetGUID Entity::GetSceneGUID() const {
		return m_Scene ? m_Scene->GetSceneEntityGUID(m_EntityHandle) : AssetGUID(0);
	}

	AssetGUID Entity::GetPrefabGUID() const {
		return m_Scene ? m_Scene->GetPrefabGUID(m_EntityHandle) : AssetGUID(0);
	}

	bool Entity::IsSceneEntity() const {
		return GetOrigin() == EntityOrigin::Scene;
	}

	bool Entity::IsPrefabInstance() const {
		return GetOrigin() == EntityOrigin::Prefab;
	}

	bool Entity::IsRuntime() const {
		return GetOrigin() == EntityOrigin::Runtime;
	}

	void Entity::SetStatic(bool isStatic) {
		if (isStatic) { if (!HasComponent<StaticTag>()) AddComponent<StaticTag>(); }
		else { if (HasComponent<StaticTag>()) RemoveComponent<StaticTag>(); }
	}

	void Entity::SetEnabled(bool enabled) {
		if (!m_Registry || m_EntityHandle == entt::null) return;

		const bool parentDisabled = HasParent() && GetParent().HasComponent<DisabledTag>();
		if (enabled) {
			if (parentDisabled) {
				// User wants this entity enabled, but the parent is disabled. Keep it
				// disabled by inheritance — but preserve any authored-disabled state.
				// If the child already has DisabledTag without InheritedDisabledTag,
				// the user authored it disabled; converting that to inherited would
				// erase the authored intent on the next parent re-enable cascade.
				if (!HasComponent<DisabledTag>()) {
					AddComponent<InheritedDisabledTag>();
					AddComponent<DisabledTag>();
				}
				return;
			}

			if (HasComponent<InheritedDisabledTag>()) RemoveComponent<InheritedDisabledTag>();
			if (HasComponent<DisabledTag>()) RemoveComponent<DisabledTag>();
		}
		else {
			if (HasComponent<InheritedDisabledTag>()) RemoveComponent<InheritedDisabledTag>();
			if (!HasComponent<DisabledTag>()) AddComponent<DisabledTag>();
		}
	}

	// ── Parent-child hierarchy ──────────────────────────────────────────

	namespace {
		// Walk up the parent chain. Returns true if `ancestor` appears as
		// a (possibly transitive) parent of `descendant`. Used to refuse
		// cycles in SetParent.
		bool HierarchyContains(entt::registry& registry, EntityHandle ancestor, EntityHandle descendant) {
			EntityHandle cursor = descendant;
			// Hard cap on depth so a corrupted scene can't infinite-loop here.
			for (int i = 0; i < 10000 && cursor != entt::null; ++i) {
				if (cursor == ancestor) return true;
				if (!registry.valid(cursor) || !registry.all_of<HierarchyComponent>(cursor)) {
					return false;
				}
				cursor = registry.get<HierarchyComponent>(cursor).Parent;
			}
			return false;
		}

		// Detach `child` from its current parent's child list, leaving the
		// child's own HierarchyComponent in place (caller decides what to
		// set Parent to next).
		void DetachFromCurrentParent(entt::registry& registry, EntityHandle child) {
			if (!registry.valid(child) || !registry.all_of<HierarchyComponent>(child)) return;
			HierarchyComponent& hc = registry.get<HierarchyComponent>(child);
			const EntityHandle currentParent = hc.Parent;
			if (currentParent == entt::null) return;

			if (registry.valid(currentParent) && registry.all_of<HierarchyComponent>(currentParent)) {
				auto& parentHc = registry.get<HierarchyComponent>(currentParent);
				parentHc.Children.erase(
					std::remove(parentHc.Children.begin(), parentHc.Children.end(), child),
					parentHc.Children.end());
			}
			hc.Parent = entt::null;
		}

		void ClearInheritedDisabled(entt::registry& registry, EntityHandle entity) {
			if (!registry.valid(entity) || !registry.all_of<InheritedDisabledTag>(entity)) return;
			registry.remove<InheritedDisabledTag>(entity);
			if (registry.all_of<DisabledTag>(entity)) {
				registry.remove<DisabledTag>(entity);
			}
		}

		void ApplyInheritedDisabled(entt::registry& registry, EntityHandle entity) {
			if (!registry.valid(entity) || registry.all_of<DisabledTag>(entity)) return;
			registry.emplace<InheritedDisabledTag>(entity);
			registry.emplace<DisabledTag>(entity);
		}
	}

	void Entity::SetParent(Entity parent) {
		if (!m_Registry || m_EntityHandle == entt::null) return;

		const EntityHandle parentHandle = parent.m_EntityHandle;
		if (parentHandle == m_EntityHandle) return; // can't parent to self

		// Cycle check: refuse if `parent` is a descendant of `this`.
		if (parentHandle != entt::null && HierarchyContains(*m_Registry, m_EntityHandle, parentHandle)) {
			IDX_WARN_TAG("Entity", "SetParent refused: would create a cycle");
			return;
		}

		// Ensure both ends have a HierarchyComponent.
		if (!HasComponent<HierarchyComponent>()) {
			AddComponent<HierarchyComponent>();
		}
		DetachFromCurrentParent(*m_Registry, m_EntityHandle);

		HierarchyComponent& myHc = GetComponent<HierarchyComponent>();
		if (parentHandle == entt::null) {
			myHc.Parent = entt::null;
			ClearInheritedDisabled(*m_Registry, m_EntityHandle);
			if (m_Scene) {
				m_Scene->MarkTransformDirty(m_EntityHandle);
			}
			return;
		}

		if (!m_Registry->all_of<HierarchyComponent>(parentHandle)) {
			m_Registry->emplace<HierarchyComponent>(parentHandle);
		}
		auto& parentHc = m_Registry->get<HierarchyComponent>(parentHandle);
		parentHc.Children.push_back(m_EntityHandle);
		myHc.Parent = parentHandle;

		// Keep inherited disables tied to the current parent. Authored disables
		// have plain DisabledTag only, so moving between parents won't clear them.
		if (m_Registry->all_of<DisabledTag>(parentHandle)) {
			ApplyInheritedDisabled(*m_Registry, m_EntityHandle);
		}
		else if (HasComponent<InheritedDisabledTag>()) {
			ClearInheritedDisabled(*m_Registry, m_EntityHandle);
		}

		if (m_Scene) {
			m_Scene->MarkTransformDirty(m_EntityHandle);
		}
	}

	Entity Entity::GetParent() const {
		if (!m_Registry || m_EntityHandle == entt::null) return Entity::Null;
		if (!HasComponent<HierarchyComponent>()) return Entity::Null;
		const EntityHandle p = GetComponent<HierarchyComponent>().Parent;
		if (p == entt::null || !m_Registry->valid(p)) return Entity::Null;
		return Entity(p, m_Scene);
	}

	std::span<const EntityHandle> Entity::GetChildren() const {
		if (!m_Registry || m_EntityHandle == entt::null) return {};
		if (!HasComponent<HierarchyComponent>()) return {};
		const std::vector<EntityHandle>& children = GetComponent<HierarchyComponent>().Children;
		return std::span<const EntityHandle>(children);
	}

	bool Entity::HasParent() const {
		if (!HasComponent<HierarchyComponent>()) return false;
		return GetComponent<HierarchyComponent>().Parent != entt::null;
	}

	bool Entity::IsAncestorOf(Entity other) const {
		if (!m_Registry || m_EntityHandle == entt::null) return false;
		return HierarchyContains(*m_Registry, m_EntityHandle, other.m_EntityHandle);
	}
}
