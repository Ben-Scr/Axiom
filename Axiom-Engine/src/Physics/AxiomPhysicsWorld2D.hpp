#pragma once
#include "Collections/Vec2.hpp"
#include "Core/Export.hpp"
#include "Physics/AxiomContact2D.hpp"
#include "Physics/AxiomPhysicsInterop.hpp"
#include "Scene/EntityHandle.hpp"

#include <PhysicsWorld.hpp>
#include <WorldSettings.hpp>
#include <Contact.hpp>
#include <BoxCollider.hpp>
#include <CircleCollider.hpp>

#include <unordered_map>
#include <functional>
#include <vector>

namespace Axiom {
	using AxiomContactCallback = std::function<void(const AxiomContact2D&)>;

	/// Engine-level wrapper around the Axiom-Physics PhysicsWorld.
	/// Manages Body/Collider ownership and provides per-entity contact callbacks.
	// Owns `unordered_map<uint32_t, unique_ptr<Body|Collider>>` members; not AXIOM_API
	// for the same reason as LayerStack — would trigger MSVC's dllexport copy-op
	// instantiation error. Consumers access it via PhysicsSystem2D's API.
	class AxiomPhysicsWorld2D {
	public:
		AxiomPhysicsWorld2D();
		explicit AxiomPhysicsWorld2D(const AxiomPhys::WorldSettings& settings);

		void Step(float dt);
		void Destroy();

		AxiomPhys::PhysicsWorld& GetWorld() { return m_World; }
		const AxiomPhys::PhysicsWorld& GetWorld() const { return m_World; }

		void SetSettings(const AxiomPhys::WorldSettings& settings) { m_World.SetSettings(settings); }
		const AxiomPhys::WorldSettings& GetSettings() const { return m_World.GetSettings(); }

		// Body registration tied to an entity
		AxiomPhys::Body* CreateBody(EntityHandle entity, AxiomPhys::BodyType type);
		void DestroyBody(EntityHandle entity);
		AxiomPhys::Body* GetBody(EntityHandle entity);

		// Collider attachment. Two collider kinds can coexist on a single
		// entity at the storage level: the inspector's component-conflict
		// system blocks user-driven dual-collider setups, but programmatic
		// adds (scripts, deserialised legacy scenes) used to overwrite the
		// existing entry and leave the user-component's pointer dangling.
		// Keying by (entity, kind) keeps both alive and removable
		// independently.
		enum class FastColliderKind : uint8_t {
			Box,
			Circle,
		};

		AxiomPhys::BoxCollider* CreateBoxCollider(EntityHandle entity, const Vec2& halfExtents);
		AxiomPhys::CircleCollider* CreateCircleCollider(EntityHandle entity, float radius);
		// Destroy a specific collider kind on the entity. Called from the
		// matching FastBoxCollider2D / FastCircleCollider2D destroy hook so
		// removing one component never invalidates the other.
		void DestroyCollider(EntityHandle entity, FastColliderKind kind);
		// Destroy every collider on this entity. Used by DestroyBody (which
		// must clean up all attachments before the body itself goes away).
		void DestroyAllCollidersOnEntity(EntityHandle entity);

		// Contact callbacks per entity
		void RegisterContactCallback(EntityHandle entity, AxiomContactCallback callback);
		void UnregisterContactCallback(EntityHandle entity);

		size_t GetBodyCount() const { return m_World.GetBodyCount(); }

	private:
		void DispatchContacts();

		AxiomPhys::PhysicsWorld m_World;

		// Composite (entity, kind) key for the collider map. Packed into a
		// uint64 for cheap hashing.
		struct ColliderKey {
			uint32_t entity;
			FastColliderKind kind;
			bool operator==(const ColliderKey& o) const noexcept {
				return entity == o.entity && kind == o.kind;
			}
		};
		struct ColliderKeyHash {
			size_t operator()(const ColliderKey& k) const noexcept {
				const uint64_t packed =
					(static_cast<uint64_t>(k.entity) << 8) |
					static_cast<uint64_t>(static_cast<uint8_t>(k.kind));
				return std::hash<uint64_t>{}(packed);
			}
		};

		// Ownership: bodies keyed by entity, colliders keyed by (entity, kind)
		std::unordered_map<uint32_t, std::unique_ptr<AxiomPhys::Body>> m_Bodies;
		std::unordered_map<ColliderKey, std::unique_ptr<AxiomPhys::Collider>, ColliderKeyHash> m_Colliders;

		// Entity lookup from Body pointer (reverse map)
		std::unordered_map<AxiomPhys::Body*, EntityHandle> m_BodyToEntity;

		// Contact callbacks per entity
		std::unordered_map<uint32_t, AxiomContactCallback> m_ContactCallbacks;
	};

}
