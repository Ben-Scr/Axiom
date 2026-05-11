#pragma once

#include "Jobs/JobSystemSpec.hpp"

#include <functional>

namespace Axiom {

	// JobSystem owns a small fixed-size worker pool and a single MPMC FIFO
	// queue of callables. Use for short-lived fork/join parallel work —
	// physics transform sync, particle update, ParallelFor, etc. For
	// long-lived background tasks (file watching, async loaders), reach
	// for OwnedTask instead.
	//
	// Lifecycle: Initialize() must be called before any Job::Schedule /
	// ParallelFor call. Shutdown() drains pending work and joins all
	// workers. Calling Schedule from an uninitialized pool falls back to
	// running the work inline on the calling thread (defensive, so
	// shutdown ordering bugs don't crash — they merely serialize work).
	//
	// Thread-safety: every public method may be called from any thread.
	class AXIOM_API JobSystem {
	public:
		static void Initialize(const JobSystemSpec& spec = {});
		static void Shutdown();
		static bool IsInitialized();

		static int  GetWorkerCount();
		static bool IsCallerWorker();
		// 0..GetWorkerCount()-1 when called from a worker, -1 elsewhere.
		// Cheap (thread_local read) — fine to call in hot paths.
		static int  GetWorkerIndex();

		// Internal — used by Job::Schedule, ParallelFor, and Job::Wait
		// (work-stealing). Public so templates can call without friend
		// gymnastics. Not part of the stable API; callers should prefer
		// Job::Schedule / ParallelFor / ParallelForAsync.
		static void Enqueue(std::function<void()> work);

		// Try to pop one job from the queue and run it on the calling
		// thread. Returns true if a job ran. Used by Job::Wait to drain
		// the queue while waiting on a handle, preventing deadlock when
		// a job spawns sub-jobs and waits on them.
		static bool TryPopAndRun();

	private:
		JobSystem() = delete;
	};

}
