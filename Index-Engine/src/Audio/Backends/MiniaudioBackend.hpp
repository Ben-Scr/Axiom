#pragma once

#include "Audio/IAudioBackend.hpp"

#include <miniaudio.h>

namespace Index {

	// Default IAudioBackend implementation. Wraps a single ma_engine instance and
	// owns its lifetime. Installed by AudioManager::Initialize() when no other
	// backend was registered first.
	class INDEX_API MiniaudioBackend : public IAudioBackend {
	public:
		MiniaudioBackend() = default;
		~MiniaudioBackend() override = default;

		MiniaudioBackend(const MiniaudioBackend&) = delete;
		MiniaudioBackend& operator=(const MiniaudioBackend&) = delete;

		bool Initialize() override;
		void Shutdown() override;
		void SetMasterVolume(float volume) override;
		ma_engine* GetMiniaudioEngine() override { return m_Initialized ? &m_Engine : nullptr; }

	private:
		ma_engine m_Engine{};
		bool m_Initialized = false;
	};

}
