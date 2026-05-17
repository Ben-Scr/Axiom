#pragma once

#include "Core/Export.hpp"
#include "Memory/Arena.hpp"

#include <cstddef>

namespace Index {

	// Spec passed to FrameArenas::Initialize. Capacities of 0 produce empty
	// arenas — every Allocate returns nullptr — which is the right behavior
	// for processes that explicitly opt out (e.g. tooling builds that don't
	// run a frame loop).
	struct FrameArenasSpec {
		std::size_t FrameCapacityBytes      = 1024 * 1024;   // 1 MiB
		std::size_t PersistentCapacityBytes = 64   * 1024;   // 64 KiB
	};

	// Engine-managed scratch arenas. Two named pools, both main-thread only
	// for now:
	//
	//   Frame()      — auto-reset by Application::EndFrame. Use for transient
	//                  per-frame buffers: draw-call scratch, vertex pack
	//                  storage, sort keys, anything whose lifetime is exactly
	//                  one frame. Cheap to reset (O(1)), no destructor passes.
	//
	//   Persistent() — never auto-reset. Use for cross-frame scratch the
	//                  engine owns: the C# marshalling buffer for in-flight
	//                  P/Invoke calls (added in a later step), one-time
	//                  startup bumps, etc. Caller resets manually via
	//                  ResetPersistent() if/when it's safe to do so.
	//
	// Lifetime: Initialize() must run before the first Frame()/Persistent()
	// call. The Application wires this up; standalone consumers (tests,
	// tooling) call Initialize/Shutdown themselves. Both calls are
	// idempotent — re-Initializing tears down the previous arenas and
	// allocates fresh ones with the new spec.
	//
	// Thread-safety: NOT thread-safe. Both arenas are main-thread only.
	// Worker threads that need scratch storage should hold a local Arena.
	// A future step will add per-worker arenas with explicit reset broadcast
	// if a real consumer needs them.
	namespace FrameArenas {

		// Allocates backing buffers per the spec. Calling Initialize again
		// frees the existing buffers and allocates new ones.
		INDEX_CORE_API void Initialize(const FrameArenasSpec& spec = FrameArenasSpec{});

		// Frees backing buffers. Safe to call when never Initialize'd.
		INDEX_CORE_API void Shutdown();

		// True between Initialize and Shutdown. Mostly for assertions.
		INDEX_CORE_API bool IsInitialized();

		// Per-frame scratch arena. Reset at end-of-frame via OnEndFrame.
		// Calling before Initialize returns a reference to an empty arena
		// (Allocate returns nullptr); use IsInitialized() to gate code that
		// must allocate.
		INDEX_CORE_API Arena& Frame();

		// Persistent scratch arena. Never auto-reset.
		INDEX_CORE_API Arena& Persistent();

		// Hook called from Application::EndFrame. Wipes Frame() in O(1).
		// Persistent() is untouched. Safe to call when never Initialize'd.
		INDEX_CORE_API void OnEndFrame();

		// Explicit reset for Persistent(). The engine never calls this on
		// its own; the owner of the persistent scratch decides when its
		// contents are safe to drop.
		INDEX_CORE_API void ResetPersistent();

	} // namespace FrameArenas

} // namespace Index
