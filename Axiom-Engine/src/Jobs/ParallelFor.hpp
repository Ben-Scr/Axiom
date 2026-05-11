#pragma once

#include "Jobs/Job.hpp"
#include "Jobs/JobSystem.hpp"

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace Axiom {

	// Compute a sensible chunk size when the caller passes grainSize == 0.
	// Aims for roughly (workerCount * 4) chunks so each worker can pick up
	// 4 chunks on average — small enough that imbalanced workloads
	// redistribute, large enough that per-chunk overhead stays trivial.
	AXIOM_API size_t ComputeAutoGrainSize(size_t rangeLength, int workerCount);

	namespace Detail {

		// Dispatches a chunk to the user's callable. Supports two body
		// forms — pick whichever reads better at the call site:
		//   fn(size_t i)              -> per-index
		//   fn(size_t lo, size_t hi)  -> per-chunk (for per-chunk setup
		//                                amortization, e.g. allocating a
		//                                local scratch buffer once)
		// If both work (e.g. a generic-lambda taking auto...), per-chunk
		// wins because it's the cheaper dispatch.
		template <typename F>
		inline void InvokeParallelChunk(F& fn, size_t lo, size_t hi) {
			if constexpr (std::is_invocable_v<F&, size_t, size_t>) {
				fn(lo, hi);
			}
			else if constexpr (std::is_invocable_v<F&, size_t>) {
				for (size_t i = lo; i < hi; ++i) {
					fn(i);
				}
			}
			else {
				static_assert(sizeof(F) == 0,
					"ParallelFor body must be callable with (size_t) or (size_t, size_t)");
			}
		}

	} // namespace Detail

	// Fire-and-forget parallel-for. Splits [begin, end) into chunks,
	// schedules each chunk as a job, and returns a handle that completes
	// when every chunk has finished. The callable is copied/moved into a
	// shared owner so it can safely outlive the calling function.
	template <typename F>
	JobHandle ParallelForAsync(size_t begin, size_t end, F&& fn, size_t grainSize = 0) {
		if (begin >= end) {
			return JobHandle{};
		}

		const int workers = JobSystem::GetWorkerCount();
		const size_t rangeLength = end - begin;

		if (grainSize == 0) {
			grainSize = ComputeAutoGrainSize(rangeLength, workers > 0 ? workers : 1);
		}
		if (grainSize == 0) {
			grainSize = 1;
		}

		const size_t numChunks = (rangeLength + grainSize - 1) / grainSize;

		// Degenerate cases (single chunk, or no pool available): run
		// synchronously on the calling thread. Still return a completed
		// handle so callers don't need to special-case the return value.
		if (numChunks <= 1 || workers <= 0) {
			Detail::InvokeParallelChunk(fn, begin, end);
			auto block = JobInternal::CreateBlock(1);
			JobInternal::NotifyOne(block);
			return JobHandle(std::move(block));
		}

		auto fnPtr = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
		auto block = JobInternal::CreateBlock(static_cast<int>(numChunks));

		for (size_t c = 0; c < numChunks; ++c) {
			const size_t lo = begin + c * grainSize;
			const size_t hi = (lo + grainSize < end) ? (lo + grainSize) : end;

			JobSystem::Enqueue([block, fnPtr, lo, hi]() {
				Detail::InvokeParallelChunk(*fnPtr, lo, hi);
				JobInternal::NotifyOne(block);
			});
		}

		return JobHandle(std::move(block));
	}

	// Synchronous parallel-for. Blocks until every chunk completes,
	// work-stealing while waiting so a nested ParallelFor from inside a
	// job body cannot deadlock.
	template <typename F>
	void ParallelFor(size_t begin, size_t end, F&& fn, size_t grainSize = 0) {
		JobHandle handle = ParallelForAsync(begin, end, std::forward<F>(fn), grainSize);
		Job::Wait(handle);
	}

} // namespace Axiom
