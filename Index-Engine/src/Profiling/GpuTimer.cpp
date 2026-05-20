#include "pch.hpp"
#include "Profiling/GpuTimer.hpp"

// =============================================================================
// GpuTimer — WebGPU (Dawn) implementation.
// -----------------------------------------------------------------------------
// Attaches wgpu::RenderPassTimestampWrites to the renderer's main pass so
// the GPU writes two timestamps per frame (beginning + end of pass) into a
// wgpu::QuerySet. The flow per frame is:
//
//   1. BeginFrameWrites — allocate this frame's slot pair from the ring,
//      hand the QuerySet + indices to the renderer for it to attach to
//      its RenderPassDescriptor.
//   2. (Renderer opens the pass with the timestamp writes attached; the
//      GPU writes the two timestamps as the pass executes.)
//   3. ResolveCurrentFrame — encode ResolveQuerySet into the resolve
//      buffer (one CopyDst-friendly nanosecond u64 per query) then
//      CopyBufferToBuffer into the per-slot readback buffer, then queue
//      MapAsync on the readback so completion drives PollAndPublish.
//   4. PollAndPublish — once per frame, walk slots; any in Mapped state
//      have their two nanosecond timestamps converted to a millisecond
//      delta and pushed into the "GPU" profiler module. Unmap + return
//      the slot to Empty.
//
// Ring depth 3 matches the OpenGL impl's "read frame N during frame N+3"
// pattern — by then the GPU has long since written the timestamps and the
// MapAsync has long since fulfilled, so PollAndPublish is non-blocking.
//
// Falls dormant cleanly when the device wasn't created with TimestampQuery:
// Initialize early-returns without allocating any GPU resources, every
// other method short-circuits on !m_Initialized. The "GPU" profiler module
// stays at its default value (Available=false in the panel → "N/A").
// =============================================================================

#ifdef INDEX_PROFILER_ENABLED

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Profiling/Profiler.hpp"

#include <cstring>
#include <string>

namespace Index {

	GpuTimer::GpuTimer() = default;
	GpuTimer::~GpuTimer() { Shutdown(); }

	void GpuTimer::Initialize() {
		if (m_Initialized) return;
		// Adapter / device must already exist (RenderApi::Init has run) and
		// must have advertised TimestampQuery. Both gates protect against
		// silent failure on adapters where wgpu::QuerySet creation with the
		// Timestamp type validation-errors.
		if (!WebGPUBackend::IsInitialized()) return;
		if (!WebGPUBackend::HasTimestampQuery()) return;

		wgpu::Device device = WebGPUBackend::GetDevice();
		if (!device) return;

		wgpu::QuerySetDescriptor qsDesc{};
		qsDesc.type  = wgpu::QueryType::Timestamp;
		qsDesc.count = static_cast<uint32_t>(k_QueriesPerFrame * k_RingDepth);
		qsDesc.label = "gpu-timer-queryset";
		m_QuerySet = device.CreateQuerySet(&qsDesc);
		if (!m_QuerySet) {
			IDX_CORE_WARN_TAG("GpuTimer",
				"CreateQuerySet(Timestamp, count={}) failed — GPU module stays at N/A",
				qsDesc.count);
			return;
		}

		wgpu::BufferDescriptor resolveDesc{};
		// Size by the aligned slot stride (256), not the useful payload
		// (16) — WebGPU requires ResolveQuerySet's destinationOffset to
		// be a multiple of 256, so we pad each ring entry up to a full
		// 256-byte slot. Wasted ~240 bytes per slot is negligible vs. the
		// alternative of a per-slot resolve buffer + extra Dawn handle.
		resolveDesc.size  = static_cast<uint64_t>(k_ResolveSlotStride * k_RingDepth);
		resolveDesc.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
		resolveDesc.label = "gpu-timer-resolve";
		m_ResolveBuffer = device.CreateBuffer(&resolveDesc);
		if (!m_ResolveBuffer) {
			IDX_CORE_WARN_TAG("GpuTimer", "Failed to create resolve buffer");
			m_QuerySet = nullptr;
			return;
		}

		wgpu::BufferDescriptor rbDesc{};
		rbDesc.size  = static_cast<uint64_t>(k_BytesPerFrame);
		rbDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
		std::string label;
		for (size_t i = 0; i < k_RingDepth; ++i) {
			label = "gpu-timer-readback-" + std::to_string(i);
			rbDesc.label = label.c_str();
			m_Slots[i].Readback = device.CreateBuffer(&rbDesc);
			m_Slots[i].State    = SlotState::Empty;
			if (!m_Slots[i].Readback) {
				IDX_CORE_WARN_TAG("GpuTimer", "Failed to create readback buffer {}", i);
				m_QuerySet      = nullptr;
				m_ResolveBuffer = nullptr;
				for (auto& s : m_Slots) s.Readback = nullptr;
				return;
			}
		}

		m_CurrentSlot       = 0;
		m_FrameWritesIssued = false;
		m_Initialized       = true;
	}

	void GpuTimer::Shutdown() {
		m_QuerySet      = nullptr;
		m_ResolveBuffer = nullptr;
		for (auto& s : m_Slots) {
			// Defensive unmap — a slot still in AwaitingMap at shutdown
			// would otherwise leak the Dawn-internal map state. Unmap is
			// a no-op on an unmapped buffer.
			if (s.Readback) {
				s.Readback.Unmap();
			}
			s.Readback = nullptr;
			s.State    = SlotState::Empty;
		}
		m_CurrentSlot       = 0;
		m_FrameWritesIssued = false;
		m_Initialized       = false;
	}

	GpuTimer::FrameSlots GpuTimer::BeginFrameWrites() {
		FrameSlots out;
		if (!m_Initialized) return out;
		// Ring full — every slot is mid-pipeline (encoded-not-submitted or
		// waiting-for-map). Editor scenes that drive two render passes per
		// Application frame burn through slots fast; with a 3-deep ring
		// you'll typically see one render per frame get timestamps and
		// the other skipped. Acceptable degradation — bump k_RingDepth if
		// you want full coverage.
		if (m_Slots[m_CurrentSlot].State != SlotState::Empty) {
			return out;
		}
		out.QuerySet                  = m_QuerySet;
		out.BeginningOfPassWriteIndex = static_cast<uint32_t>(m_CurrentSlot * k_QueriesPerFrame);
		out.EndOfPassWriteIndex       = static_cast<uint32_t>(m_CurrentSlot * k_QueriesPerFrame + 1);
		out.Valid                     = true;
		m_FrameWritesIssued           = true;
		return out;
	}

	void GpuTimer::ResolveCurrentFrame(wgpu::CommandEncoder encoder) {
		if (!m_Initialized || !m_FrameWritesIssued || !encoder) return;

		const uint32_t firstQuery    = static_cast<uint32_t>(m_CurrentSlot * k_QueriesPerFrame);
		// Slot offset uses the 256-aligned stride (WebGPU spec requirement
		// for ResolveQuerySet's destinationOffset), not the useful payload
		// size — see k_ResolveSlotStride for the rationale. Earlier
		// versions of this code used slot * 16 directly and Dawn
		// validated "destination buffer offset (16) is not a multiple of
		// 256" once the second ring slot tried to resolve, invalidating
		// the whole frame's command buffer and dropping every draw on
		// the floor.
		const uint64_t resolveOffset = static_cast<uint64_t>(m_CurrentSlot * k_ResolveSlotStride);
		encoder.ResolveQuerySet(m_QuerySet,
			firstQuery, static_cast<uint32_t>(k_QueriesPerFrame),
			m_ResolveBuffer, resolveOffset);
		// CopyBufferToBuffer copies only the 16 useful bytes (two u64
		// timestamps). The destination readback buffer is a separate per-
		// slot wgpu::Buffer with its own dst offset 0, so no 256-alignment
		// constraint here — CopyBufferToBuffer only requires multiple-of-4
		// alignment for src offset / dst offset / size.
		encoder.CopyBufferToBuffer(m_ResolveBuffer, resolveOffset,
			m_Slots[m_CurrentSlot].Readback, 0,
			static_cast<uint64_t>(k_BytesPerFrame));

		// MapAsync is INTENTIONALLY DEFERRED to OnFrameStart() in the next
		// frame. Calling it here would immediately transition the readback
		// to Pending state, and the subsequent Queue::Submit (which still
		// holds the CopyBufferToBuffer encoded above) would validate-fail
		// with "used in submit while mapped", invalidating the entire
		// frame's command buffer. By transitioning to AwaitingSubmit and
		// deferring the map, we keep the readback in Unmapped state
		// through Submit; the map is then safely issued next frame after
		// Submit has completed.
		m_Slots[m_CurrentSlot].State = SlotState::AwaitingSubmit;

		m_FrameWritesIssued = false;
		m_CurrentSlot       = (m_CurrentSlot + 1) % k_RingDepth;
	}

	void GpuTimer::OnFrameStart() {
		if (!m_Initialized) return;
		// Promote any slots whose CopyBufferToBuffer was encoded last frame
		// (and is now safely past Submit) into MapAsync-pending. State
		// transition: AwaitingSubmit → AwaitingMap. Empty / AwaitingMap
		// slots stay put. Safe to call every frame — idempotent on no-
		// AwaitingSubmit-slots and cheap (a small fixed loop).
		for (auto& s : m_Slots) {
			if (s.State != SlotState::AwaitingSubmit) continue;
			if (!s.Readback) continue;
			s.Readback.MapAsync(
				wgpu::MapMode::Read, 0,
				static_cast<size_t>(k_BytesPerFrame),
				wgpu::CallbackMode::AllowSpontaneous,
				[](wgpu::MapAsyncStatus /*status*/, wgpu::StringView /*msg*/) {});
			s.State = SlotState::AwaitingMap;
		}
	}

	void GpuTimer::PollAndPublish() {
		if (!m_Initialized) return;

		// Drain every slot whose MapAsync has completed. Doing this on
		// every call (rather than just the oldest) is cheap (≤ k_RingDepth
		// state checks) and prevents lag if Dawn batches multiple
		// completions onto one ProcessEvents pump.
		for (auto& s : m_Slots) {
			if (s.State != SlotState::AwaitingMap) continue;
			if (!s.Readback) continue;
			if (s.Readback.GetMapState() != wgpu::BufferMapState::Mapped) continue;

			const void* mapped = s.Readback.GetConstMappedRange(0, k_BytesPerFrame);
			if (mapped) {
				uint64_t ts[2] = { 0, 0 };
				std::memcpy(ts, mapped, sizeof(ts));
				// WebGPU timestamps are nanoseconds. End-before-start is
				// only valid if the GPU clock wrapped or the pass didn't
				// actually execute — guard with the max(0) we get
				// implicitly by clamping the unsigned subtract.
				const uint64_t deltaNs = ts[1] > ts[0] ? ts[1] - ts[0] : 0;
				const float    deltaMs = static_cast<float>(deltaNs) * 1.0e-6f;
				Profiler::PushSample("GPU", deltaMs);
			}
			s.Readback.Unmap();
			s.State = SlotState::Empty;
		}
	}

	long long GpuTimer::QueryGpuMemoryMb() {
		// WebGPU has no portable surface for GPU memory queries today.
		// Dawn exposes Vulkan-internal stats via toggles, but nothing
		// portable. Returning -1 surfaces as "N/A" in the editor stats
		// overlay.
		return -1;
	}

}  // namespace Index

#else  // !INDEX_PROFILER_ENABLED

// When the profiler is stripped, premake removes Profiling/**.cpp from the
// build. The class declaration in the header still exists but nothing
// references it (Renderer2D guards `m_GpuTimer` behind the same #ifdef),
// so the undefined symbols never get pulled in by the linker.

#endif  // INDEX_PROFILER_ENABLED
