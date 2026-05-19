#pragma once

#include "Scene/EntityHandle.hpp"

#include <cstdint>
#include <cstring>
#include <unordered_map>
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
	public:
		explicit DynamicComponentStorage(uint32_t elementSize, uint32_t alignment)
			: m_ElementSize(elementSize), m_Alignment(alignment) {}

		bool Contains(EntityHandle e) const {
			return m_Indices.find(e) != m_Indices.end();
		}

		void* Get(EntityHandle e) {
			auto it = m_Indices.find(e);
			if (it == m_Indices.end()) return nullptr;
			return &m_Data[static_cast<size_t>(it->second) * m_ElementSize];
		}

		const void* Get(EntityHandle e) const {
			auto it = m_Indices.find(e);
			if (it == m_Indices.end()) return nullptr;
			return &m_Data[static_cast<size_t>(it->second) * m_ElementSize];
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
			auto it = m_Indices.find(e);
			if (it == m_Indices.end()) {
				AddInternal(e, bytes);
				return;
			}
			std::memcpy(&m_Data[static_cast<size_t>(it->second) * m_ElementSize],
				bytes, m_ElementSize);
		}

		void Remove(EntityHandle e) {
			auto it = m_Indices.find(e);
			if (it == m_Indices.end()) return;

			const uint32_t idx = it->second;
			const uint32_t lastIdx = static_cast<uint32_t>(m_Entities.size() - 1);
			if (idx != lastIdx) {
				const EntityHandle lastEntity = m_Entities[lastIdx];
				std::memcpy(&m_Data[static_cast<size_t>(idx) * m_ElementSize],
					&m_Data[static_cast<size_t>(lastIdx) * m_ElementSize],
					m_ElementSize);
				m_Entities[idx] = lastEntity;
				m_Indices[lastEntity] = idx;
			}
			m_Entities.pop_back();
			m_Data.resize(m_Data.size() - m_ElementSize);
			m_Indices.erase(it);
		}

		void Clear() {
			m_Indices.clear();
			m_Entities.clear();
			m_Data.clear();
		}

		size_t Size() const { return m_Entities.size(); }
		uint32_t ElementSize() const { return m_ElementSize; }
		uint32_t Alignment() const { return m_Alignment; }
		const std::vector<EntityHandle>& Entities() const { return m_Entities; }

	private:
		void AddInternal(EntityHandle e, const void* bytes) {
			if (Contains(e)) return;
			const uint32_t idx = static_cast<uint32_t>(m_Entities.size());
			m_Entities.push_back(e);
			m_Indices[e] = idx;
			m_Data.resize(m_Data.size() + m_ElementSize);
			std::byte* slot = reinterpret_cast<std::byte*>(
				&m_Data[static_cast<size_t>(idx) * m_ElementSize]);
			if (bytes) {
				std::memcpy(slot, bytes, m_ElementSize);
			}
			else {
				std::memset(slot, 0, m_ElementSize);
			}
		}

		uint32_t m_ElementSize;
		uint32_t m_Alignment;
		std::vector<EntityHandle> m_Entities;
		std::vector<uint8_t> m_Data;
		// EntityHandle is an enum class wrapping uint32_t. std::hash works
		// because EnTT defines std::hash<entt::entity> via its config.
		std::unordered_map<EntityHandle, uint32_t> m_Indices;
	};

}
