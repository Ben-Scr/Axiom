#pragma once

namespace Axiom {

	// Init options for JobSystem. Forward-extensible — adding fields here
	// does not break callers, since the struct is default-constructible
	// and Initialize takes it by const-ref.
	struct JobSystemSpec {
		// -1 selects an automatic count: hardware_concurrency() - 1,
		// clamped to [2, 16]. When AXIOM_WITH_SCRIPTING is on, the auto
		// path further caps at max(2, cores - 2) so the .NET ThreadPool
		// (used by Axiom.Jobs from script code) has headroom on low-
		// core machines.
		int WorkerCount = -1;
	};

}
