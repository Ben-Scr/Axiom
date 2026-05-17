#pragma once

// ParallelEntityCommandBuffer
// ===========================
// Lock-free parallel-record companion to NativeEntityCommandBuffer.
//
// Each worker thread gets its OWN NativeEntityCommandBuffer to record into,
// indexed by `JobSystem::GetWorkerIndex()`. The main thread (and any other
// non-worker) records into a dedicated "spillover" slot. Because every
// concurrently-recording thread writes to a different sub-buffer, no locks
// or atomics live on the hot path.
//
// Typical use from inside a ParallelFor:
//
//   ParallelEntityCommandBuffer pecb;
//   ParallelFor(0, particleCount, [&](size_t lo, size_t hi) {
//       NativeEntityCommandBuffer& ecb = pecb.AsWriter();
//       for (size_t i = lo; i < hi; ++i) {
//           auto e = ecb.CreateEntity();
//           ecb.AddComponent<Transform2DComponent>(e, [&](auto& tr) {
//               tr.Position = SpawnPos(i);
//           });
//           ecb.AddComponent<SpriteRendererComponent>(e); // Color stays white
//       }
//   });
//   pecb.Playback(*GetScene());
//
// On Playback() the buffer (a) sums the total entity count across all sub-
// buffers, (b) does ONE Scene::CreateEntitiesBulk for the whole batch, and
// (c) walks each sub-buffer's commands while remapping their local
// EntityIndex into the merged handle range — the bulk-create-once invariant
// from the managed/native ECB is preserved.
//
// IMPORTANT:
//   - EntityRefs returned by one worker's CreateEntity() are valid ONLY
//     inside that worker's own sub-buffer. Don't cross-use refs between
//     threads.
//   - Recording must be quiescent (no jobs in flight) before Playback().
//     Playback runs on the calling thread and reads every sub-buffer.

#include "Core/Export.hpp"
#include "Scene/EntityHandle.hpp"
#include "Scripting/NativeEntityCommandBuffer.hpp"

#include <vector>

namespace Index {

	class Scene;

	class INDEX_API ParallelEntityCommandBuffer {
	public:
		// Constructs with one sub-buffer per JobSystem worker plus one
		// extra slot for non-worker threads (main thread, foreign threads).
		// The number of slots is fixed for the lifetime of the instance —
		// if the JobSystem isn't initialized yet, we still get a single
		// main-thread slot so the buffer is usable in early-startup code.
		ParallelEntityCommandBuffer();

		ParallelEntityCommandBuffer(const ParallelEntityCommandBuffer&) = delete;
		ParallelEntityCommandBuffer& operator=(const ParallelEntityCommandBuffer&) = delete;
		ParallelEntityCommandBuffer(ParallelEntityCommandBuffer&&) noexcept = default;
		ParallelEntityCommandBuffer& operator=(ParallelEntityCommandBuffer&&) noexcept = default;

		// Returns the sub-buffer for the calling thread. Workers get their
		// own slot (indexed by JobSystem::GetWorkerIndex()); non-worker
		// threads share the spillover slot. No locks. Stable address for
		// the lifetime of *this — safe to take a reference and hold for
		// the duration of a ParallelFor chunk body.
		NativeEntityCommandBuffer& AsWriter();

		// Single-threaded merge + playback. Sums every sub-buffer's entity
		// count, bulk-creates the total in one Scene::CreateEntitiesBulk
		// call, then walks each sub-buffer's commands with a per-writer
		// base offset so local EntityIndex values resolve into the right
		// slot of the merged handle range. Created entity handles are
		// concatenated in writer order (worker 0, worker 1, …, spillover)
		// and addressable via GetCreatedEntity(i).
		int Playback(Scene& scene);

		// Resets every sub-buffer (runs destructors on any non-trivial
		// captures, retains backing storage). Per-frame spawn loops can
		// call this after Playback to reuse the instance allocation-free.
		void Clear();

		// Total entities recorded across every sub-buffer, before playback.
		int EntityCount() const;

		// Total commands recorded across every sub-buffer.
		int CommandCount() const;

		// i-th created entity. Only valid after a successful Playback and
		// until the next Clear(). Index range is [0, total entity count).
		EntityHandle GetCreatedEntity(int i) const;

	private:
		// Stable storage — m_Subs is resized exactly once in the ctor and
		// never grown, so addresses returned by AsWriter() stay valid for
		// the buffer's lifetime even while other threads read different
		// slots concurrently.
		std::vector<NativeEntityCommandBuffer> m_Subs;
		std::vector<EntityHandle>              m_CreatedHandles;
	};

} // namespace Index
