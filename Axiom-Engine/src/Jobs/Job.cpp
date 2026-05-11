#include "pch.hpp"

#include "Jobs/Job.hpp"
#include "Jobs/JobSystem.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace Axiom {

	// Internal control block. Lives behind a shared_ptr so it outlives both
	// the worker thread that completes it and the caller that waits on it.
	struct JobControlBlock {
		std::atomic<int>          Pending{0};
		std::atomic<bool>         Done{false};
		std::mutex                Mutex;
		std::condition_variable   Cv;
	};

	namespace JobInternal {

		std::shared_ptr<JobControlBlock> CreateBlock(int pending) {
			auto block = std::make_shared<JobControlBlock>();
			block->Pending.store(pending, std::memory_order_release);
			return block;
		}

		void NotifyOne(const std::shared_ptr<JobControlBlock>& block) {
			if (!block) return;

			// Sequentially-consistent fetch_sub so the "last decrementer
			// wins" pattern is unambiguous across architectures. ABA
			// concerns don't apply (counter only goes down).
			const int remaining = block->Pending.fetch_sub(1, std::memory_order_acq_rel) - 1;
			if (remaining > 0) {
				return;
			}

			// remaining == 0: we are the last completer. Mark Done and
			// wake waiters. We take the mutex even just for the notify
			// because waiters check the Done flag under the same lock.
			{
				std::scoped_lock lock(block->Mutex);
				block->Done.store(true, std::memory_order_release);
			}
			block->Cv.notify_all();
		}

	} // namespace JobInternal

	// ── JobHandle ────────────────────────────────────────────────────

	bool JobHandle::IsValid() const {
		return static_cast<bool>(m_Block);
	}

	bool JobHandle::IsComplete() const {
		// A default-constructed handle counts as complete — "nothing to do"
		// reads naturally as done.
		if (!m_Block) return true;
		return m_Block->Done.load(std::memory_order_acquire);
	}

	// ── Job ──────────────────────────────────────────────────────────

	JobHandle Job::Schedule(std::function<void()> work) {
		auto block = JobInternal::CreateBlock(1);

		JobSystem::Enqueue([block, work = std::move(work)]() mutable {
			if (work) work();
			JobInternal::NotifyOne(block);
		});

		return JobHandle(std::move(block));
	}

	void Job::Wait(const JobHandle& handle) {
		const auto& block = handle.GetBlock();
		if (!block) return;

		// Fast path: already done.
		if (block->Done.load(std::memory_order_acquire)) return;

		// Hybrid work-stealing wait. We don't want to block when there's
		// queued work we could be running — that's the classic fork-join
		// deadlock when a job waits on a sub-job and all workers are also
		// waiting on each other. Drain the queue while we wait.
		using namespace std::chrono_literals;
		for (;;) {
			if (block->Done.load(std::memory_order_acquire)) return;

			if (JobSystem::TryPopAndRun()) {
				continue;
			}

			// Queue is empty but our handle isn't done yet. Brief
			// timed-wait on the completion cv so we don't burn a core,
			// then re-check (in case a job is enqueued during the wait).
			std::unique_lock<std::mutex> lock(block->Mutex);
			block->Cv.wait_for(lock, 1ms, [&] {
				return block->Done.load(std::memory_order_acquire);
			});
			if (block->Done.load(std::memory_order_acquire)) return;
		}
	}

	bool Job::IsComplete(const JobHandle& handle) {
		return handle.IsComplete();
	}

} // namespace Axiom
