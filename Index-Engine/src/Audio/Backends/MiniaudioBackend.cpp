#include "pch.hpp"
#include "Audio/Backends/MiniaudioBackend.hpp"

namespace Index {

	bool MiniaudioBackend::Initialize() {
		if (m_Initialized) {
			return true;
		}

		const ma_result result = ma_engine_init(nullptr, &m_Engine);
		if (result != MA_SUCCESS) {
			IDX_CORE_ERROR("[{}] MiniaudioBackend: ma_engine_init failed. Error: {}",
				ErrorCodeToString(IndexErrorCode::LoadFailed), static_cast<int>(result));
			return false;
		}

		m_Initialized = true;
		return true;
	}

	void MiniaudioBackend::Shutdown() {
		if (!m_Initialized) {
			return;
		}

		ma_engine_uninit(&m_Engine);
		m_Initialized = false;
	}

	void MiniaudioBackend::SetMasterVolume(float volume) {
		if (!m_Initialized) {
			return;
		}

		ma_engine_set_volume(&m_Engine, volume);
	}

}
