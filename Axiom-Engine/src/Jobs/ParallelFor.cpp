#include "pch.hpp"

#include "Jobs/ParallelFor.hpp"

namespace Axiom {

	size_t ComputeAutoGrainSize(size_t rangeLength, int workerCount) {
		if (workerCount < 1) {
			workerCount = 1;
		}

		// Target ~4 chunks per worker — keeps imbalanced workloads
		// rebalanceable across the pool while keeping per-chunk overhead
		// negligible for typical sizes.
		const size_t targetChunks = static_cast<size_t>(workerCount) * 4;
		if (targetChunks == 0) {
			return rangeLength > 0 ? rangeLength : 1;
		}

		const size_t grain = rangeLength / targetChunks;
		return grain > 0 ? grain : 1;
	}

} // namespace Axiom
