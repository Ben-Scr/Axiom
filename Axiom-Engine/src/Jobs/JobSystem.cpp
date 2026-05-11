#include "pch.hpp"

#include "Jobs/JobSystem.hpp"

#include "Profiling/Profiler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifdef AIM_PLATFORM_WINDOWS
#include <processthreadsapi.h>
#endif

namespace Axiom {

	namespace {

		// All pool state is static — JobSystem is a process-wide singleton
		// service. The state lives in this anonymous namespace so it's only
		// visible inside this TU.
		std::mutex                       s_QueueMutex;
		std::deque<std::function<void()>> s_Queue;
		std::condition_variable          s_QueueCv;

		std::atomic<bool>                s_Running{false};
		std::atomic<bool>                s_Initialized{false};
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
#if AXIOM_WITH_SCRIPTING
			// Leave headroom for the .NET ThreadPool that Axiom.Jobs uses
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
#ifdef AIM_PLATFORM_WINDOWS
			wchar_t name[32];
			swprintf_s(name, L"Axiom Worker %d", workerIndex);
			::SetThreadDescription(::GetCurrentThread(), name);
#else
			(void)workerIndex;
#endif
		}

		void ExecuteJob(std::function<void()>&& job) {
			AXIOM_PROFILE_SCOPE("JobSystem::Job");
			try {
				if (job) job();
			}
			catch (const std::exception& e) {
				AIM_CORE_ERROR_TAG("JobSystem", "Job failed: {}", e.what());
			}
			catch (...) {
				AIM_CORE_ERROR_TAG("JobSystem", "Job failed: unknown exception");
			}
		}

		void WorkerMain(int workerIndex) {
			t_WorkerIndex = workerIndex;
			SetWorkerThreadName(workerIndex);

			for (;;) {
				std::function<void()> job;
				{
					std::unique_lock<std::mutex> lock(s_QueueMutex);
					s_QueueCv.wait(lock, [] {
						return !s_Queue.empty() || !s_Running.load(std::memory_order_acquire);
					});

					// Drain ordering: when shutdown is signalled (s_Running
					// = false) we still process whatever's in the queue.
					// Only exit once the queue is fully empty AND running
					// is false. This lets OnDestroy paths that schedule
					// cleanup work still complete.
					if (s_Queue.empty()) {
						return;
					}

					job = std::move(s_Queue.front());
					s_Queue.pop_front();
				}

				ExecuteJob(std::move(job));
			}
		}

	} // namespace

	void JobSystem::Initialize(const JobSystemSpec& spec) {
		if (s_Initialized.load(std::memory_order_acquire)) {
			AIM_CORE_WARN_TAG("JobSystem", "Initialize called twice — ignoring second call");
			return;
		}

		const int workerCount = ResolveAutoWorkerCount(spec.WorkerCount);

		s_Running.store(true, std::memory_order_release);
		s_Workers.reserve(static_cast<size_t>(workerCount));
		for (int i = 0; i < workerCount; ++i) {
			s_Workers.emplace_back(WorkerMain, i);
		}

		s_Initialized.store(true, std::memory_order_release);
		AIM_CORE_INFO_TAG("JobSystem", "Started with {} worker thread(s)", workerCount);
	}

	void JobSystem::Shutdown() {
		if (!s_Initialized.load(std::memory_order_acquire)) {
			return;
		}

		{
			std::scoped_lock lock(s_QueueMutex);
			s_Running.store(false, std::memory_order_release);
		}
		s_QueueCv.notify_all();

		for (auto& worker : s_Workers) {
			if (worker.joinable()) {
				worker.join();
			}
		}
		s_Workers.clear();

		// Belt-and-braces: discard anything left in the queue. With the
		// drain ordering above this should be empty, but a panicked
		// shutdown (e.g. from a fatal assert) might leave residue.
		{
			std::scoped_lock lock(s_QueueMutex);
			s_Queue.clear();
		}

		s_Initialized.store(false, std::memory_order_release);
		AIM_CORE_INFO_TAG("JobSystem", "Stopped");
	}

	bool JobSystem::IsInitialized() {
		return s_Initialized.load(std::memory_order_acquire);
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
		if (!s_Initialized.load(std::memory_order_acquire)) {
			ExecuteJob(std::move(work));
			return;
		}

		{
			std::scoped_lock lock(s_QueueMutex);
			s_Queue.emplace_back(std::move(work));
		}
		s_QueueCv.notify_one();
	}

	bool JobSystem::TryPopAndRun() {
		std::function<void()> job;
		{
			std::scoped_lock lock(s_QueueMutex);
			if (s_Queue.empty()) {
				return false;
			}
			job = std::move(s_Queue.front());
			s_Queue.pop_front();
		}
		ExecuteJob(std::move(job));
		return true;
	}

} // namespace Axiom
