#include "pch.hpp"
#include "Profiling/GpuTimer.hpp"

#ifdef AXIOM_PROFILER_ENABLED
#include "Profiling/Profiler.hpp"
#include <bgfx/bgfx.h>
#endif

// =============================================================================
// GpuTimer — real bgfx implementation. Stage 2.1 of the bgfx port.
// -----------------------------------------------------------------------------
// bgfx already runs a per-frame GPU timer internally and exposes it through
// `bgfx::getStats()`:
//   * `gpuTimeBegin` / `gpuTimeEnd` — device-clock ticks bracketing the
//     frame's GPU work.
//   * `gpuTimerFreq` — ticks per second.
// We just convert (end - begin) / freq → milliseconds and push it into the
// "GPU" profiler module each frame. No per-frame ring of GL queries needed
// — bgfx's read is already non-blocking + lag-correct.
//
// PerFrame (declared in the header) stays an opaque pointer for ABI
// compatibility with the OpenGL impl; we don't allocate it under bgfx.
// =============================================================================

namespace Axiom {

	GpuTimer::GpuTimer() = default;
	GpuTimer::~GpuTimer() = default;

	void GpuTimer::Initialize() { m_Initialized = true; }
	void GpuTimer::Shutdown()   { m_Initialized = false; m_FrameOpen = false; }

	// bgfx tracks the GPU timer for us — Begin/End are markers only.
	void GpuTimer::BeginFrame() { m_FrameOpen = true; }
	void GpuTimer::EndFrame()   { m_FrameOpen = false; }

	void GpuTimer::PollAndPublish() {
#ifdef AXIOM_PROFILER_ENABLED
		if (!m_Initialized) return;
		const bgfx::Stats* stats = bgfx::getStats();
		if (!stats) return;
		// gpuTimerFreq == 0 means the backend doesn't expose a GPU
		// timer (bgfx noop / some software backends). Skip silently.
		if (stats->gpuTimerFreq == 0) return;
		const int64_t deltaTicks = stats->gpuTimeEnd - stats->gpuTimeBegin;
		if (deltaTicks <= 0) return;
		const double ms = (double(deltaTicks) * 1000.0) / double(stats->gpuTimerFreq);
		Profiler::PushSample("GPU", static_cast<float>(ms));
#endif
	}

	long long GpuTimer::QueryGpuMemoryMb() {
#ifdef AXIOM_PROFILER_ENABLED
		const bgfx::Stats* stats = bgfx::getStats();
		if (!stats) return -1;
		// gpuMemoryUsed is bytes, -1 when the backend can't report.
		if (stats->gpuMemoryUsed < 0) return -1;
		return stats->gpuMemoryUsed / (1024 * 1024);
#else
		return -1;
#endif
	}

} // namespace Axiom
