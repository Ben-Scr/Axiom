#pragma once

#include "Core/Export.hpp"

#include <cstddef>
#include <cstdint>

namespace Axiom {

	// ── GPU-side timing for the profiler ──────────────────────────────────
	//
	// CPU instrumentation (AXIOM_PROFILE_SCOPE) only times CPU command
	// submission — the GPU may still be working long after a Renderer2D
	// call returns. To measure actual GPU work we issue OpenGL TIME_ELAPSED
	// queries and read the results back.
	//
	// Polling the result of the query you JUST issued causes the driver
	// to flush + wait for the GPU, which is the exact stall we're trying
	// to measure. Standard fix: keep a small ring of N query objects (3
	// here) and read frame N's result during frame N+3 — by then the
	// query has long since resolved and the read is non-blocking.
	//
	// On GPU memory: NVIDIA exposes GL_NVX_gpu_memory_info; AMD exposes
	// GL_ATI_meminfo; Intel and Mesa generally expose neither. When no
	// extension is available QueryGpuMemoryMb returns -1 and the panel
	// renders "N/A" for that module.
	//
	// Available only when AXIOM_PROFILER_ENABLED. With the profiler
	// stripped, this whole TU is excluded from the build.
	// ──────────────────────────────────────────────────────────────────────

	class AXIOM_API GpuTimer {
	public:
		GpuTimer();
		~GpuTimer();

		// Allocates GL queries. Must be called after the GL context exists.
		// Idempotent — re-calling is a no-op.
		void Initialize();

		// Releases GL queries. Safe to call after context destruction (the
		// query handles are bookkeeping only on the CPU side once context
		// is gone).
		void Shutdown();

		// Wrap a draw region. BeginFrame issues glBeginQuery(TIME_ELAPSED);
		// EndFrame closes it. Call once per frame, around the renderer's
		// draw work. Mismatched begin/end is harmless (the query is just
		// dropped).
		void BeginFrame();
		void EndFrame();

		// Polls the oldest pending query. If its result is ready, computes
		// the elapsed milliseconds and pushes it into the "GPU" profiler
		// module. Call once per frame after EndFrame; non-blocking.
		void PollAndPublish();

		// Returns -1 when no driver extension is available.
		static long long QueryGpuMemoryMb();

	private:
		struct PerFrame;
		PerFrame* m_Frames = nullptr; // owned ring; 3 entries
		size_t m_NextWrite = 0;
		size_t m_NextRead = 0;
		size_t m_PendingCount = 0;
		bool m_Initialized = false;
		bool m_FrameOpen = false;
	};

} // namespace Axiom
