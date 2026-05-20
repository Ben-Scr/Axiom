#pragma once

#include "Scene/EntityHandle.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace Index {

	// Type-erased component storage for runtime-registered components
	// (structs authored in the user's C# project and registered via
	// DynamicComponentRegistrar at assembly-load time). Holds raw bytes
	// per entity, sized to the C# struct's Marshal.SizeOf at registration.
	//
	// Owned by ComponentRegistry — one instance per dynamic component
	// type. Lives entirely outside EnTT's typed-storage system because
	// EnTT's templates need a compile-time component type, which dynamic
	// registration by definition does not have.
	//
	// Lookup strategy: paged sparse array, layout-compatible with
	// entt::basic_sparse_set. Each sparse slot stores
	// `traits::combine(dense_index, original_entity)` — the dense index
	// in the entity-mask bits, the entity's version in the version-mask
	// bits. Contains() then becomes the same versioned XOR check EnTT
	// uses internally (sparse_set.hpp:691), letting IJobQuery's hot path
	// touch only sparse memory + one packed-array load per match. The
	// prior implementation used std::unordered_map for this lookup,
	// which cost ~8x in the (NativeTransform2D, UserData) IJobQuery case
	// due to hashed bucket walks per probed entity.
	//
	// Tradeoffs vs. native EnTT storage:
	//   - No on_construct / on_destroy signals fire for dynamic components.
	//   - EnTT view<T> / group<T> can't include dynamic components — they're
	//     reached via the registry's name-keyed bridge instead. Scripts go
	//     through Entity_GetComponentPtr which already speaks names.
	//   - Entity destruction is handled via ComponentRegistry::ScrubEntity,
	//     called from Scene::DestroyEntityInternal right before the EnTT
	//     destroy. The hook walks every dynamic storage and Removes the
	//     entity from each; cheap when an entity has no dynamic components.
	class DynamicComponentStorage {
		using traits_type = entt::entt_traits<EntityHandle>;
		static constexpr std::size_t kPageSize = traits_type::page_size;

	public:
		explicit DynamicComponentStorage(uint32_t elementSize, uint32_t alignment)
			: m_ElementSize(elementSize), m_Alignment(alignment) {}

		bool Contains(EntityHandle e) const noexcept {
			const EntityHandle* slot = SparsePtr(e);
			// Same XOR-version check EnTT uses in basic_sparse_set::contains
			// (sparse_set.hpp:691-697). `cap` = entity_mask, `mask` = the
			// version bits. The XOR zeros out the version bits when they
			// match and leaves the dense index in the low bits, so a result
			// < cap proves both that the slot is occupied AND that the
			// entity's version matches the stored version (i.e., the slot
			// hasn't been recycled by a newer entity at the same identifier).
			constexpr auto cap = traits_type::entity_mask;
			constexpr auto mask = traits_type::to_integral(entt::null) & ~cap;
			return slot && (((mask & traits_type::to_integral(e)) ^ traits_type::to_integral(*slot)) < cap);
		}

		void* Get(EntityHandle e) noexcept {
			const EntityHandle* slot = SparsePtr(e);
			constexpr auto cap = traits_type::entity_mask;
			constexpr auto mask = traits_type::to_integral(entt::null) & ~cap;
			if (!slot) return nullptr;
			const auto xored = (mask & traits_type::to_integral(e)) ^ traits_type::to_integral(*slot);
			if (xored >= cap) return nullptr;
			const auto denseIdx = static_cast<std::size_t>(traits_type::to_entity(*slot));
			return &m_Data[denseIdx * m_ElementSize];
		}

		const void* Get(EntityHandle e) const noexcept {
			const EntityHandle* slot = SparsePtr(e);
			constexpr auto cap = traits_type::entity_mask;
			constexpr auto mask = traits_type::to_integral(entt::null) & ~cap;
			if (!slot) return nullptr;
			const auto xored = (mask & traits_type::to_integral(e)) ^ traits_type::to_integral(*slot);
			if (xored >= cap) return nullptr;
			const auto denseIdx = static_cast<std::size_t>(traits_type::to_entity(*slot));
			return &m_Data[denseIdx * m_ElementSize];
		}

		// Zero-initialized add. Idempotent — already-present entity is a no-op.
		void Add(EntityHandle e) {
			AddInternal(e, nullptr);
		}

		// Add with payload (memcpy from `bytes`, which must point to at least
		// ElementSize bytes). Idempotent on the entity-already-present path.
		void AddWithBytes(EntityHandle e, const void* bytes) {
			AddInternal(e, bytes);
		}

		// Replace-or-insert. Used by the ECB emplaceFromBytes callback.
		void EmplaceOrReplace(EntityHandle e, const void* bytes) {
			if (void* existing = Get(e)) {
				std::memcpy(existing, bytes, m_ElementSize);
				return;
			}
			AddInternal(e, bytes);
		}

		void Remove(EntityHandle e) {
			if (!Contains(e)) return;

			EntityHandle& slot = AssureSlot(e);
			const auto denseIdx = static_cast<uint32_t>(traits_type::to_entity(slot));
			const auto lastIdx = static_cast<uint32_t>(m_Entities.size() - 1);
			if (denseIdx != lastIdx) {
				const EntityHandle lastEntity = m_Entities[lastIdx];
				std::memcpy(&m_Data[static_cast<std::size_t>(denseIdx) * m_ElementSize],
					&m_Data[static_cast<std::size_t>(lastIdx) * m_ElementSize],
					m_ElementSize);
				m_Entities[denseIdx] = lastEntity;
				// Re-point the moved entity's sparse slot at its new dense index,
				// preserving its version (mirror swap_and_pop in EnTT's sparse_set
				// at sparse_set.hpp:244-256).
				AssureSlot(lastEntity) = traits_type::combine(
					static_cast<traits_type::entity_type>(denseIdx),
					traits_type::to_integral(lastEntity));
			}
			m_Entities.pop_back();
			m_Data.resize(m_Data.size() - m_ElementSize);
			slot = entt::null;
		}

		void Clear() {
			for (auto& page : m_Sparse) {
				if (!page.empty()) {
					std::fill(page.begin(), page.end(), entt::null);
				}
			}
			m_Entities.clear();
			m_Data.clear();
		}

		std::size_t Size() const noexcept { return m_Entities.size(); }
		uint32_t ElementSize() const noexcept { return m_ElementSize; }
		uint32_t Alignment() const noexcept { return m_Alignment; }
		const std::vector<EntityHandle>& Entities() const noexcept { return m_Entities; }

	private:
		// Returns a pointer to the sparse slot for `e`, or nullptr if the
		// page hasn't been allocated yet. Read-only — does not allocate.
		const EntityHandle* SparsePtr(EntityHandle e) const noexcept {
			const auto pos = static_cast<std::size_t>(traits_type::to_entity(e));
			const auto page = pos / kPageSize;
			if (page >= m_Sparse.size() || m_Sparse[page].empty()) return nullptr;
			return &m_Sparse[page][pos % kPageSize];
		}

		// Returns a reference to the sparse slot for `e`, allocating the
		// page (initialized to entt::null) if it doesn't exist.
		EntityHandle& AssureSlot(EntityHandle e) {
			const auto pos = static_cast<std::size_t>(traits_type::to_entity(e));
			const auto page = pos / kPageSize;
			if (page >= m_Sparse.size()) {
				m_Sparse.resize(page + 1);
			}
			if (m_Sparse[page].empty()) {
				m_Sparse[page].assign(kPageSize, entt::null);
			}
			return m_Sparse[page][pos % kPageSize];
		}

		void AddInternal(EntityHandle e, const void* bytes) {
			if (Contains(e)) return;
			const uint32_t denseIdx = static_cast<uint32_t>(m_Entities.size());
			m_Entities.push_back(e);
			m_Data.resize(m_Data.size() + m_ElementSize);
			std::byte* slot = reinterpret_cast<std::byte*>(
				&m_Data[static_cast<std::size_t>(denseIdx) * m_ElementSize]);
			if (bytes) {
				std::memcpy(slot, bytes, m_ElementSize);
			}
			else {
				std::memset(slot, 0, m_ElementSize);
			}
			// Encode dense index in the entity-mask bits, e's version in the
			// version-mask bits. Contains() then verifies version match.
			AssureSlot(e) = traits_type::combine(
				static_cast<traits_type::entity_type>(denseIdx),
				traits_type::to_integral(e));
		}

		uint32_t m_ElementSize;
		uint32_t m_Alignment;
		std::vector<EntityHandle> m_Entities;
		std::vector<uint8_t> m_Data;
		// Paged sparse array. Outer vector indexed by page = (entity_id /
		// page_size); each inner vector is either empty (page never
		// allocated) or sized to page_size with entt::null defaults.
		std::vector<std::vector<EntityHandle>> m_Sparse;
	};

}
