#include "pch.hpp"
#include "Memory/FrameArenas.hpp"

namespace Index::FrameArenas {

	namespace {

		// Storage for the two arenas. Function-local statics so we get a
		// well-defined empty-arena state before Initialize and don't depend
		// on TU-static init ordering with the tracked Allocator.
		Arena& FrameStorage() {
			static Arena s_Frame;
			return s_Frame;
		}

		Arena& PersistentStorage() {
			static Arena s_Persistent;
			return s_Persistent;
		}

		bool& InitializedFlag() {
			static bool s_Initialized = false;
			return s_Initialized;
		}

	} // anonymous namespace

	void Initialize(const FrameArenasSpec& spec) {
		// Idempotent. Re-initializing replaces the backing buffers; any
		// pointers handed out by the previous arenas are invalidated. The
		// engine only calls this once per Application lifetime, but tests
		// re-Initialize between cases to control capacity.
		FrameStorage()      = Arena(spec.FrameCapacityBytes);
		PersistentStorage() = Arena(spec.PersistentCapacityBytes);
		InitializedFlag()   = true;
	}

	void Shutdown() {
		// Drop both backing buffers by move-assigning empty arenas. After
		// this, Frame()/Persistent() still return valid references — they
		// just have zero capacity, so Allocate returns nullptr. Avoids
		// the "use-after-shutdown" footgun where a late caller would touch
		// freed memory.
		FrameStorage()      = Arena();
		PersistentStorage() = Arena();
		InitializedFlag()   = false;
	}

	bool IsInitialized() {
		return InitializedFlag();
	}

	Arena& Frame() {
		return FrameStorage();
	}

	Arena& Persistent() {
		return PersistentStorage();
	}

	void OnEndFrame() {
		// Reset is safe on an empty arena (no-op). We don't check
		// IsInitialized first so the call site in Application::EndFrame
		// doesn't have to know about init state.
		FrameStorage().Reset();
	}

	void ResetPersistent() {
		PersistentStorage().Reset();
	}

} // namespace Index::FrameArenas
