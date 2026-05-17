#include "pch.hpp"

#include "Jobs/JobSystem.hpp"

#include "Profiling/Profiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include <blockingconcurrentqueue.h>

#ifdef IDX_PLATFORM_WINDOWS
#include <processthreadsapi.h>
#endif

namespace Index {

	namespace {

		// All pool state is static — JobSystem is a process-wide singleton
		// service. The state lives in this anonymous namespace so it's only
		// visible inside this TU.
		//
		// The queue itself is moodycamel::BlockingConcurrentQueue — an MPMC
		// lock-free queue with a built-in semaphore for blocking waits. This
		// replaces the prior std::mutex + std::deque + std::condition_variable
		// trio, which serialized every Enqueue/Dequeue on a single mutex and
		// thundering-herded workers awake on Shutdown. Enqueue is wait-free
		// in the common path; wait_dequeue_timed lets workers wake on a
		// short timeout so the Running-state poll in WorkerMain still drives
		// shutdown.
		moodycamel::BlockingConcurrentQueue<std::function<void()>> s_Queue;

		// Single source of truth for pool lifecycle. Previously we tracked
		// Initialized + Running as two independent atomics and the Enqueue
		// path loaded them outside the queue-mutex window — Shutdown could
		// flip Running between the load and the actual deque insert, leaving
		// a job stuck behind the drain. Without a queue mutex anymore, the
		// state load is the only ordering primitive: Enqueue checks state,
		// then enqueues; if Shutdown flips state in between, the post-drain
		// loop in WorkerMain catches the late arrival.
		enum class PoolState : uint8_t { Uninit = 0, Running = 1, ShuttingDown = 2 };
		std::atomic<PoolState>           s_State{ PoolState::Uninit };
		std::vector<std::thread>         s_Workers;

		// thread_local so reads from inside Job bodies are a single TLS
		// load. Set on entry to WorkerMain; remains -1 on any thread that
		// isn't a JobSystem worker.
		thread_local int                 t_WorkerIndex = -1;

		int ResolveAutoWorkerCount(int requested) {
			if (requested > 0) {
				// Honor explicit count, but cap to a sane upper bound so an
				// accidental N=1024 doesn't melt the OS.
				return std::min(requested, 32);
			}

			unsigned int hw = std::thread::hardware_concurrency();
			if (hw == 0) hw = 4; // hardware_concurrency may return 0 on unusual platforms.

			int autoCount = static_cast<int>(hw) - 1;
#if INDEX_WITH_SCRIPTING
			// Leave headroom for the .NET ThreadPool that Index.Jobs uses
			// from script code. Two pools on the same machine; capping the
			// native side at cores-2 prevents oversubscription on 4-core
			// hosts (we keep 2 workers for the CLR + 1 for the main thread).
			autoCount = std::min(autoCount, static_cast<int>(hw) - 2);
#endif
			if (autoCount < 2)  autoCount = 2;
			if (autoCount > 16) autoCount = 16;
			return autoCount;
		}

		void SetWorkerThreadName(int workerIndex) {
#ifdef IDX_PLATFORM_WINDOWS
			wchar_t name[32];
			swprintf_s(name, L"Index Worker %d", workerIndex);
			::SetThreadDescription(::GetCurrentThread(), name);
#else
			(void)workerIndex;
#endif
		}

		void ExecuteJob(std::function<void()>&& job) {
			INDEX_PROFILE_SCOPE("JobSystem::Job");
			try {
				if (job) job();
			}
			catch (const std::exception& e) {
				IDX_CORE_ERROR_TAG("JobSystem", "Job failed: {}", e.what());
			}
			catch (...) {
				IDX_CORE_ERROR_TAG("JobSystem", "Job failed: unknown exception");
			}
		}

		void WorkerMain(int workerIndex) {
			t_WorkerIndex = workerIndex;
			SetWorkerThreadName(workerIndex);

			std::function<void()> job;
			while (s_State.load(std::memory_order_acquire) == PoolState::Running) {
				// 50 ms timeout balances responsiveness on shutdown against
				// CPU spin: workers wake at most 20 times/sec on an idle pool
				// to re-check the running flag, but a freshly enqueued job
				// is dispatched immediately via the queue's semaphore.
				if (s_Queue.wait_dequeue_timed(job, std::chrono::milliseconds(50))) {
					ExecuteJob(std::move(job));
				}
			}

			// Drain ordering: when shutdown is signalled we still process
			// whatever remains in the queue so OnDestroy paths that
			// scheduled cleanup work still complete. Race-free because the
			// state flip in Shutdown happens-before this drain, and any
			// concurrent Enqueue that observed Running is allowed to land
			// — its work will be picked up here.
			while (s_Queue.try_dequeue(job)) {
				ExecuteJob(std::move(job));
			}
		}

	} // namespace

	void JobSystem::Initialize(const JobSystemSpec& spec) {
		if (s_State.load(std::memory_order_acquire) != PoolState::Uninit) {
			IDX_CORE_WARN_TAG("JobSystem", "Initialize called twice — ignoring second call");
			return;
		}

		const int workerCount = ResolveAutoWorkerCount(spec.WorkerCount);

		s_Workers.reserve(static_cast<size_t>(workerCount));
		// Flip state to Running BEFORE spawning workers so the first thing
		// each WorkerMain sees is the live state, not Uninit.
		s_State.store(PoolState::Running, std::memory_order_release);
		for (int i = 0; i < workerCount; ++i) {
			s_Workers.emplace_back(WorkerMain, i);
		}

		IDX_CORE_INFO_TAG("JobSystem", "Started with {} worker thread(s)", workerCount);
	}

	void JobSystem::Shutdown() {
		if (s_State.load(std::memory_order_acquire) != PoolState::Running) {
			return;
		}

		s_State.store(PoolState::ShuttingDown, std::memory_order_release);

		// Wake every blocked worker so the state check in WorkerMain runs
		// promptly. Enqueueing N empty jobs is the simplest poison-pill —
		// workers pop them, see empty std::function (ExecuteJob skips it),
		// then loop back to the state check and exit. The post-drain in
		// WorkerMain still empties any genuine work that slipped in.
		const size_t workerCount = s_Workers.size();
		for (size_t i = 0; i < workerCount; ++i) {
			s_Queue.enqueue(std::function<void()>{});
		}

		for (auto& worker : s_Workers) {
			if (worker.joinable()) {
				worker.join();
			}
		}
		s_Workers.clear();

		// Belt-and-braces: discard anything left in the queue. With the
		// drain ordering above this should be empty, but a panicked
		// shutdown (e.g. from a fatal assert) might leave residue.
		std::function<void()> sink;
		while (s_Queue.try_dequeue(sink)) { /* discard */ }

		s_State.store(PoolState::Uninit, std::memory_order_release);
		IDX_CORE_INFO_TAG("JobSystem", "Stopped");
	}

	bool JobSystem::IsInitialized() {
		return s_State.load(std::memory_order_acquire) == PoolState::Running;
	}

	int JobSystem::GetWorkerCount() {
		return static_cast<int>(s_Workers.size());
	}

	bool JobSystem::IsCallerWorker() {
		return t_WorkerIndex >= 0;
	}

	int JobSystem::GetWorkerIndex() {
		return t_WorkerIndex;
	}

	void JobSystem::Enqueue(std::function<void()> work) {
		if (!work) return;

		// Fallback: if the pool isn't up yet (or is already torn down),
		// run inline on the calling thread. Keeps callers safe across
		// init/shutdown boundaries — the worst case is a synchronous
		// fan-in, not a crash.
		//
		// There is a benign race: Shutdown may flip state between this
		// load and the enqueue below. That late arrival still lands in
		// the queue and is drained by WorkerMain's post-loop try_dequeue
		// pass, so no work is lost — just executed on a worker rather
		// than inline. The opposite race (Initialize racing with
		// Enqueue) is impossible: Initialize stores Running before any
		// worker spawns, and pre-init Enqueue here runs inline.
		if (s_State.load(std::memory_order_acquire) != PoolState::Running) {
			ExecuteJob(std::move(work));
			return;
		}
		s_Queue.enqueue(std::move(work));
	}

	bool JobSystem::TryPopAndRun() {
		std::function<void()> job;
		if (!s_Queue.try_dequeue(job)) {
			return false;
		}
		ExecuteJob(std::move(job));
		return true;
	}

} // namespace Index
