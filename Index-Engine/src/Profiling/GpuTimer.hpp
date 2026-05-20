#pragma once

#include "Core/Export.hpp"

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Index {

	// ── GPU-side timing for the profiler ──────────────────────────────────
	//
	// CPU instrumentation (INDEX_PROFILE_SCOPE) only times CPU command
	// submission — the GPU may still be working long after a Renderer2D
	// call returns. To measure actual GPU work this timer attaches a
	// wgpu::RenderPassTimestampWrites onto the renderer's main render
	// pass: two timestamps (beginning-of-pass + end-of-pass) per frame
	// written into a wgpu::QuerySet of type Timestamp.
	//
	// After the pass ends the timer encodes ResolveQuerySet into a
	// QueryResolve|CopySrc buffer, then CopyBufferToBuffer into a small
	// CopyDst|MapRead readback buffer, then queues an asynchronous map.
	// Polling the result of the query you JUST issued causes the driver
	// to flush + wait for the GPU, which is the exact stall we're trying
	// to measure. Standard fix: keep a small ring of N readback buffers
	// (3 here) and read frame N's result during frame N+3 — by then the
	// MapAsync has long since fulfilled and the read is non-blocking.
	//
	// On adapters without TimestampQuery (some Metal / older Vulkan
	// drivers — see WebGPUBackend::HasTimestampQuery), Initialize is a
	// no-op, BeginFrameWrites returns Valid=false, and the "GPU" profiler
	// module stays at "N/A". The renderer attaches no timestampWrites
	// in that case — the pass runs unchanged.
	//
	// Available only when INDEX_PROFILER_ENABLED. With the profiler
	// stripped this whole TU is excluded from the build by premake.
	// ──────────────────────────────────────────────────────────────────────

	class INDEX_API GpuTimer {
	public:
		GpuTimer();
		~GpuTimer();

		// Allocates the QuerySet + resolve + readback buffers when the
		// WebGPU device was created with TimestampQuery enabled. No-op
		// otherwise. Must run after the WebGPU device exists.
		void Initialize();

		// Releases all GPU resources. Safe to call before any Initialize.
		void Shutdown();

		// Slot info handed to the renderer so it can attach a
		// wgpu::RenderPassTimestampWrites to its pass descriptor. The
		// QuerySet field stays non-null only when Valid is true.
		struct FrameSlots {
			wgpu::QuerySet QuerySet;
			uint32_t       BeginningOfPassWriteIndex = 0;
			uint32_t       EndOfPassWriteIndex       = 0;
			bool           Valid = false;
		};

		// Allocate this frame's slot pair from the ring. Returns Valid=false
		// when GPU timing isn't available, when the ring is full (a prior
		// frame's MapAsync never completed — should be rare; the ring is
		// 3-deep and Dawn typically completes the map within one or two
		// frames), or when BeginFrameWrites was already called this frame
		// without an intervening Resolve.
		FrameSlots BeginFrameWrites();

		// After the pass ends, encode the resolve + copy-to-readback on the
		// supplied encoder. MapAsync is intentionally NOT issued here:
		// MapAsync immediately transitions the buffer into Pending state,
		// and the subsequent Queue::Submit (which still references that
		// buffer via the CopyBufferToBuffer encoded above) validation-fails
		// with "used in submit while mapped" — invalidating the whole
		// command buffer and dropping every draw on the floor. MapAsync
		// is deferred to OnFrameStart() which runs at the start of the
		// next frame, after the prior Submit has completed.
		void ResolveCurrentFrame(wgpu::CommandEncoder encoder);

		// Called once per frame at the very start, before any new render
		// passes are encoded. Issues MapAsync for any readback whose Copy
		// was encoded last frame (AwaitingSubmit → AwaitingMap). Safe
		// because at this point the previous frame's Queue::Submit has
		// already run.
		void OnFrameStart();

		// Polls any pending readbacks and publishes completed deltas into
		// the "GPU" profiler module (milliseconds). Non-blocking. Call once
		// per frame from outside any render pass.
		void PollAndPublish();

		// Returns -1 when no driver extension is available. WebGPU has no
		// portable surface for GPU memory queries today.
		static long long QueryGpuMemoryMb();

	private:
		// 3-deep ring matches the historical OpenGL impl's design rationale
		// — by frame N+3 the GPU has finished frame N and MapAsync's
		// completion callback has fired, so the read is guaranteed
		// non-blocking. Larger rings burn buffer memory; smaller rings risk
		// stalling on a slow GPU.
		static constexpr size_t k_RingDepth        = 3;
		static constexpr size_t k_QueriesPerFrame  = 2;          // begin + end
		static constexpr size_t k_BytesPerQuery    = 8;          // u64 timestamp
		static constexpr size_t k_BytesPerFrame    = k_QueriesPerFrame * k_BytesPerQuery; // 16
		// WebGPU spec requires the destination offset of ResolveQuerySet to
		// be a multiple of 256. We only WRITE 16 bytes per frame (two u64
		// timestamps), but we have to PLACE each frame's slice at a
		// 256-aligned offset within the resolve buffer — the remaining 240
		// bytes per slice stay untouched padding. The readback buffer
		// (one per ring slot, see m_Slots[].Readback) gets just the 16
		// useful bytes via CopyBufferToBuffer.
		static constexpr size_t k_ResolveSlotStride = 256;

		enum class SlotState : uint8_t {
			Empty,           // Available for the next frame's resolve
			AwaitingSubmit,  // Copy encoded into this frame's cmd buffer; MapAsync deferred
			AwaitingMap,     // MapAsync issued; PollAndPublish drains on completion
		};

		struct Slot {
			wgpu::Buffer Readback;
			SlotState    State = SlotState::Empty;
		};

		wgpu::QuerySet                m_QuerySet;
		wgpu::Buffer                  m_ResolveBuffer;  // size = k_BytesPerFrame * k_RingDepth
		std::array<Slot, k_RingDepth> m_Slots{};
		size_t                        m_CurrentSlot = 0;
		bool                          m_Initialized = false;
		bool                          m_FrameWritesIssued = false;
	};

} // namespace Index
