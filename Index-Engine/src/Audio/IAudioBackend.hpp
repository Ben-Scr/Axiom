#pragma once

#include "Core/Export.hpp"

// Forward-declare miniaudio's engine type so this header is cheap to include.
// The full miniaudio.h is heavy (single-file library) and we don't want every
// backend consumer to pay for it.
struct ma_engine;

namespace Index {

	// IAudioBackend is the swappable surface that owns the platform audio device.
	// The static AudioManager facade dispatches to whichever backend was installed
	// at boot time (default: MiniaudioBackend).
	//
	// Goal: keep AudioManager's public static API stable while allowing packages
	// to substitute a different audio implementation (null backend for headless
	// servers, FMOD, Wwise, custom platform backends).
	//
	// Current state: this is the first slice. The interface deliberately exposes
	// a `GetMiniaudioEngine()` escape hatch because AudioManager's sound-instance
	// machinery still talks to miniaudio directly. The escape hatch lets a
	// MiniaudioBackend ship today; a future tightening pass will fold every
	// remaining `ma_*` call into virtual methods so a Null/FMOD backend can be
	// written without returning a fake `ma_engine*`.
	class INDEX_API IAudioBackend {
	public:
		virtual ~IAudioBackend() = default;

		// Lifecycle. Initialize creates the platform audio device; Shutdown tears
		// it down. AudioManager::Initialize / Shutdown invoke these.
		virtual bool Initialize() = 0;
		virtual void Shutdown() = 0;

		// Master volume in [0, 1]. AudioManager clamps before dispatch.
		virtual void SetMasterVolume(float volume) = 0;

		// Escape hatch. Backends that don't speak miniaudio return nullptr; sound-
		// instance code in AudioManager treats nullptr as "no-op this operation".
		// Will be removed once every ma_* call in AudioManager is folded into a
		// dedicated virtual method on this interface.
		virtual ma_engine* GetMiniaudioEngine() { return nullptr; }
	};

}
