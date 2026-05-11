#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace Axiom {

	// Forward-declare the internal control block; its definition lives in
	// Job.cpp so callers don't see the synchronization plumbing. shared_ptr
	// to an incomplete type is fine here — the deleter is stamped in
	// Job.cpp where the type is complete.
	struct JobControlBlock;

	// Lightweight handle to a scheduled job (or a parallel-for fan-out).
	// Trivially copyable / movable. A default-constructed handle is invalid
	// and IsComplete() returns true for it (so "no job" is treated as
	// "already done", which keeps caller-side null checks short).
	class AXIOM_API JobHandle {
	public:
		JobHandle() = default;

		bool IsValid()    const;
		bool IsComplete() const;

		// Internal — used by Job::Schedule / ParallelFor implementation.
		// Public to avoid friend-template gymnastics; not part of the
		// stable surface.
		explicit JobHandle(std::shared_ptr<JobControlBlock> block) noexcept
			: m_Block(std::move(block)) {
		}

		const std::shared_ptr<JobControlBlock>& GetBlock() const noexcept { return m_Block; }

	private:
		std::shared_ptr<JobControlBlock> m_Block;
	};

	// Free-standing job API. Backed by JobSystem::Enqueue and a refcounted
	// control block. For a parallel fan-out, prefer ParallelFor /
	// ParallelForAsync rather than scheduling each chunk by hand.
	class AXIOM_API Job {
	public:
		// Schedule a single piece of work. Returns a handle that completes
		// when the callable finishes (or throws — exceptions are caught
		// and logged via AIM_CORE_ERROR_TAG, then the handle still
		// completes).
		static JobHandle Schedule(std::function<void()> work);

		// Block the calling thread until the handle completes. While
		// waiting, drains the JobSystem queue (work-stealing) so a job
		// that spawns sub-jobs and waits on them cannot deadlock.
		//
		// Safe to call from main thread or from inside another job.
		// No-op on a default-constructed (invalid) handle.
		static void Wait(const JobHandle& handle);

		// Non-blocking completion check. Cheap atomic load.
		static bool IsComplete(const JobHandle& handle);

	private:
		Job() = delete;
	};

	// Internal helpers used by ParallelFor and Job::Schedule. Exposed in
	// the header so the templated ParallelFor (which lives in
	// ParallelFor.hpp) can call them without friend gymnastics.
	namespace JobInternal {

		AXIOM_API std::shared_ptr<JobControlBlock> CreateBlock(int pending);

		// Decrement the pending counter on a control block; if it reaches
		// zero, mark the block complete and notify any waiters.
		AXIOM_API void NotifyOne(const std::shared_ptr<JobControlBlock>& block);

	}

} // namespace Axiom
