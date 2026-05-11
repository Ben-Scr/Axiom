#include "pch.hpp"
#include "Profiling/GpuTimer.hpp"

// =============================================================================
// GpuTimer — WebGPU stub.
// -----------------------------------------------------------------------------
// WebGPU has no built-in per-frame GPU timer; getting per-frame GPU times
// requires:
//
//   1. Request the `wgpu::FeatureName::TimestampQuery` feature when the
//      device is created (in WebGPUApi.cpp::RequestDeviceSync). Not all
//      adapter+OS combinations advertise it (Apple Silicon / iOS Safari /
//      some Vulkan drivers don't), so the path has to gracefully degrade.
//   2. Create a wgpu::QuerySet with the timestamp type (sized for N frames
//      of ring buffering — same lag pattern the OpenGL impl's per-frame
//      query ring used).
//   3. Plumb `wgpu::RenderPassTimestampWrites` into EVERY render pass
//      descriptor across the engine (Renderer2D / GuiRenderer / Text /
//      Gizmo, plus WebGPUApi.cpp's Clear path). The `beginningOfPass-
//      WriteIndex` / `endOfPassWriteIndex` flags tell WebGPU to write a
//      timestamp at the GPU start/end of the pass; we'd allocate two
//      slots per pass per frame.
//   4. Encode `encoder.ResolveQuerySet` into a CopyDst-usage wgpu::Buffer
//      at end-of-frame, then map+read it on the next frame (async — same
//      "read frame N's result during frame N+3" pattern the header
//      documents).
//
// For now this is a stub: GpuTimer compiles cleanly so the engine links,
// but it pushes no samples into the profiler's GPU module — the panel
// renders "N/A", matching the documented behaviour when no GPU timer is
// available.
//
// To wire the real implementation later:
//   * Add TimestampQuery to the requiredFeatures array in RequestDeviceSync,
//     wrapped in an availability check via adapter.HasFeature.
//   * Add `WebGPUBackend::WriteFrameStartTimestamp` and `…EndTimestamp`
//     helpers callable from BeginFrame / EndFrame here.
//   * Add the timestamp-writes struct to each renderer's RenderPassDescriptor.
//   * Implement PollAndPublish to consume the previous-frame buffer
//     readback and push the ms delta into Profiler::PushSample("GPU", ms).
// =============================================================================

#ifdef AXIOM_PROFILER_ENABLED
#include "Profiling/Profiler.hpp"
#endif

namespace Axiom {

	GpuTimer::GpuTimer() = default;
	GpuTimer::~GpuTimer() = default;

	void GpuTimer::Initialize() { m_Initialized = true; }
	void GpuTimer::Shutdown()   { m_Initialized = false; m_FrameOpen = false; }

	void GpuTimer::BeginFrame() { m_FrameOpen = true; }
	void GpuTimer::EndFrame()   { m_FrameOpen = false; }

	void GpuTimer::PollAndPublish() {
#ifdef AXIOM_PROFILER_ENABLED
		// No-op until wgpu::QuerySet timestamp plumbing lands (see file
		// header). Profiler's GPU panel renders "N/A".
#endif
	}

	long long GpuTimer::QueryGpuMemoryMb() {
		// WebGPU has no standard surface for reporting GPU memory usage.
		// Dawn exposes some Vulkan/D3D12 stats via internal toggles but
		// nothing portable; returning -1 surfaces as "N/A" in the
		// editor's stats overlay.
		return -1;
	}

}  // namespace Axiom
