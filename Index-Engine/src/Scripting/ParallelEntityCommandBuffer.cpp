#include "pch.hpp"

#include "Scripting/ParallelEntityCommandBuffer.hpp"

#include "Components/General/EntityMetaDataComponent.hpp"
#include "Jobs/JobSystem.hpp"
#include "Scene/Scene.hpp"

#include <cstddef>

namespace Index {

	ParallelEntityCommandBuffer::ParallelEntityCommandBuffer() {
		// One slot per worker plus one for non-workers / main thread. We
		// resize ONCE and never touch the vector again — the addresses
		// AsWriter() returns must stay valid across the rest of the
		// recording window even while other threads are calling AsWriter()
		// for different slots.
		int workerCount = JobSystem::GetWorkerCount();
		if (workerCount < 0) workerCount = 0;
		const std::size_t slotCount = static_cast<std::size_t>(workerCount) + 1u;
		m_Subs.resize(slotCount);
	}

	NativeEntityCommandBuffer& ParallelEntityCommandBuffer::AsWriter() {
		// Worker threads land in their dedicated slot. Non-workers (main
		// thread or third-party threads) share the spillover slot, which
		// is always the last one. The spillover slot is single-threaded
		// from each non-worker's perspective IFF the user only calls
		// AsWriter() from one non-worker thread at a time — which is the
		// contract (parallel record is for jobs; the main thread records
		// linearly).
		const int workerIdx = JobSystem::GetWorkerIndex();
		const std::size_t spilloverSlot = m_Subs.size() - 1u;

		std::size_t slot = spilloverSlot;
		if (workerIdx >= 0 && static_cast<std::size_t>(workerIdx) < spilloverSlot) {
			slot = static_cast<std::size_t>(workerIdx);
		}
		return m_Subs[slot];
	}

	int ParallelEntityCommandBuffer::EntityCount() const {
		std::size_t total = 0;
		for (const auto& sub : m_Subs) {
			total += static_cast<std::size_t>(sub.EntityCount());
		}
		return static_cast<int>(total);
	}

	int ParallelEntityCommandBuffer::CommandCount() const {
		std::size_t total = 0;
		for (const auto& sub : m_Subs) {
			total += static_cast<std::size_t>(sub.CommandCount());
		}
		return static_cast<int>(total);
	}

	int ParallelEntityCommandBuffer::Playback(Scene& scene) {
		// Sum the total entity count up front. This drives the single
		// bulk-create call below — the whole point of the parallel ECB
		// is collapsing all sub-buffers' creations into one.
		uint32_t total = 0;
		for (const auto& sub : m_Subs) {
			total += sub.m_EntityCount;
		}
		if (total == 0) {
			m_CreatedHandles.clear();
			return 0;
		}

		// Reserve + bulk-create the merged batch. Same fast path as the
		// single-threaded NativeEntityCommandBuffer; we just feed it the
		// sum of every sub-buffer's count.
		scene.ReserveForLoadRuntime(total, {});
		m_CreatedHandles.resize(total);
		scene.CreateEntitiesBulk(total, m_CreatedHandles);

		Scene::LoadGuard guard(scene);

		// Stamp metadata over the whole merged range — one tight loop
		// without per-entity flag pulses (MarkAllDirtyOnce at the tail).
		for (uint32_t i = 0; i < total; ++i) {
			scene.SetEntityMetaDataNoFlags(m_CreatedHandles[i], EntityOrigin::Runtime);
		}

		// Walk each sub-buffer in order, remapping each command's local
		// EntityIndex by the per-writer base offset. Sub-buffers contribute
		// contiguous ranges in worker order so GetCreatedEntity(i) returns
		// the i-th entity as recorded across the whole parallel batch.
		//
		// Apply takes Scene& and EntityHandle& now — same signature
		// change documented in NativeEntityCommandBuffer.hpp. Lets
		// Instantiate thunks rewrite the merged-handle slot in place
		// when a parallel writer records a prefab spawn.
		uint32_t base = 0;
		for (auto& sub : m_Subs) {
			const uint32_t subCount = sub.m_EntityCount;
			for (auto& cmd : sub.m_Commands) {
				if (cmd.EntityIndex < subCount) {
					cmd.Apply(cmd.Storage, scene, m_CreatedHandles[base + cmd.EntityIndex]);
				}
			}
			base += subCount;
		}

		// End-of-batch dirty pulse after the guard releases (same ordering
		// as NativeEntityCommandBuffer::Playback / managed ECB).
		scene.MarkAllDirtyOnce();

		return static_cast<int>(total);
	}

	void ParallelEntityCommandBuffer::Clear() {
		for (auto& sub : m_Subs) {
			sub.Clear();
		}
		// m_CreatedHandles capacity retained for the next Playback at the
		// same scale.
	}

	EntityHandle ParallelEntityCommandBuffer::GetCreatedEntity(int i) const {
		if (i < 0 || static_cast<std::size_t>(i) >= m_CreatedHandles.size()) {
			return entt::null;
		}
		return m_CreatedHandles[static_cast<std::size_t>(i)];
	}

} // namespace Index
