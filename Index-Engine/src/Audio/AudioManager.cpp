#include "pch.hpp"

#include "Assets/AssetRegistry.hpp"
#include "AudioManager.hpp"
#include "Audio.hpp"
#include  <Math/Common.hpp>

#include "Serialization/Path.hpp"

#include "Audio/IAudioBackend.hpp"
#include "Audio/Backends/MiniaudioBackend.hpp"
#include "Components/Audio/AudioSourceComponent.hpp"
#include "Core/Application.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <filesystem>
#include <unordered_set>

namespace Index {
	namespace {
		// Instance-id encoding: low 8 bits hold (index + 1) so 0 stays "invalid";
		// high 24 bits hold the slot's generation so a recycled slot does not silently
		// alias a stale handle. ~16M generations per slot before wrap (~277x the
		// previous 65k headroom): at MAX_CONCURRENT_SOUNDS = 64 with a worst-case
		// recycle every frame at 60 FPS, that's ~77 hours of continuous churn per
		// slot before a collision is theoretically possible. Index width = 8 bits
		// gives a hard ceiling of 255 concurrent sounds, well above the configurable
		// cap of 128.
		constexpr uint32_t k_AudioInstanceIndexBits = 8u;
		constexpr uint32_t k_AudioInstanceIndexMask = (1u << k_AudioInstanceIndexBits) - 1u;
		constexpr uint32_t k_AudioInstanceGenerationMask = (1u << (32u - k_AudioInstanceIndexBits)) - 1u;
		static_assert(128u <= k_AudioInstanceIndexMask,
			"index field width must hold the maximum configurable MAX_CONCURRENT_SOUNDS cap");

		uint32_t EncodeAudioInstanceId(uint32_t index, uint32_t generation) {
			return ((generation & k_AudioInstanceGenerationMask) << k_AudioInstanceIndexBits)
				| ((index + 1u) & k_AudioInstanceIndexMask);
		}

		uint32_t DecodeAudioInstanceIndex(uint32_t instanceId) {
			return (instanceId & k_AudioInstanceIndexMask) - 1u;
		}

		uint32_t DecodeAudioInstanceGeneration(uint32_t instanceId) {
			return (instanceId >> k_AudioInstanceIndexBits) & k_AudioInstanceGenerationMask;
		}

		bool DecodedAudioInstanceIsValid(uint32_t instanceId) {
			return (instanceId & k_AudioInstanceIndexMask) != 0u;
		}

		std::string NormalizeAudioPath(std::filesystem::path path)
		{
			if (path.empty()) {
				return {};
			}

			std::error_code ec;
			if (std::filesystem::exists(path, ec)) {
				std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, ec);
				if (!ec) {
					return canonicalPath.make_preferred().string();
				}
				ec.clear();
			}

			const std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
			if (ec) {
				return path.lexically_normal().make_preferred().string();
			}

			return absolutePath.lexically_normal().make_preferred().string();
		}
	}

	std::unique_ptr<IAudioBackend> AudioManager::s_Backend;
	bool AudioManager::s_IsInitialized = false;

	void AudioManager::SetBackend(std::unique_ptr<IAudioBackend> backend) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined,
			"AudioManager::SetBackend must be called on the main thread");
		IDX_CORE_ASSERT(!s_IsInitialized, IndexErrorCode::Undefined,
			"AudioManager::SetBackend must be called before Initialize");
		s_Backend = std::move(backend);
	}

	// Internal accessor: backends that don't speak miniaudio return nullptr. The
	// remaining ma_* call sites in this file fall back to a quiet no-op when the
	// active backend doesn't provide an engine pointer.
	static ma_engine* GetActiveMiniaudioEngine() {
		return AudioManager::GetBackend() ? AudioManager::GetBackend()->GetMiniaudioEngine() : nullptr;
	}
	std::unordered_map<AudioHandle::HandleType, std::unique_ptr<Audio>> AudioManager::s_audioMap;
	std::unordered_map<std::string, AudioHandle::HandleType> AudioManager::s_audioPathToHandle;
	std::unordered_map<std::string, AudioHandle::HandleType> AudioManager::s_audioRawPathToHandle;
	AudioHandle::HandleType AudioManager::s_nextHandle = 1;
	std::vector<std::unique_ptr<AudioManager::SoundInstance>> AudioManager::s_soundInstances;
	std::vector<uint32_t> AudioManager::s_freeInstanceIndices;
	float AudioManager::s_masterVolume = 1.0f;
	std::string AudioManager::s_RootPath = Path::Combine("IndexAssets", "Audio");

	uint32_t AudioManager::s_maxConcurrentSounds = MAX_CONCURRENT_SOUNDS;
	uint32_t AudioManager::s_maxSoundsPerFrame = MAX_SOUNDS_PER_FRAME;
	uint32_t AudioManager::s_soundsPlayedThisFrame = 0;
	uint32_t AudioManager::s_activeSoundCount = 0;
	std::priority_queue<AudioManager::SoundRequest> AudioManager::s_soundQueue;
	std::unordered_map<AudioHandle::HandleType, AudioManager::SoundLimitData> AudioManager::s_soundLimits;


	bool AudioManager::Initialize() {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::Initialize must be called on the main thread");
		if (s_IsInitialized) {
			IDX_CORE_WARN("AudioManager already initialized");
			return true;
		}

		std::string audioDir = Path::ResolveIndexAssets("Audio");
		if (audioDir.empty()) {
			IDX_CORE_WARN("IndexAssets/Audio not found");
			audioDir = Path::Combine(Path::ExecutableDir(), "IndexAssets", "Audio");
		}
		s_RootPath = audioDir;

		// Install the default backend if no package overrode it via SetBackend().
		if (!s_Backend) {
			s_Backend = std::make_unique<MiniaudioBackend>();
		}

		if (!s_Backend->Initialize()) {
			IDX_CORE_ERROR("[{}] AudioManager: backend Initialize failed",
				ErrorCodeToString(IndexErrorCode::LoadFailed));
			s_Backend.reset();
			return false;
		}

		s_soundInstances.reserve(256);
		s_freeInstanceIndices.reserve(256);

		UpdateListener();

		s_IsInitialized = true;
		return true;
	}

	void AudioManager::Shutdown() {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::Shutdown must be called on the main thread");
		if (!s_IsInitialized) {
			IDX_CORE_WARN("AudioManager isn't initialized");
			return;
		}

		UnloadAllAudio();

		s_soundInstances.clear();
		s_freeInstanceIndices.clear();
		s_soundLimits.clear();
		s_activeSoundCount = 0;
		s_soundsPlayedThisFrame = 0;
		s_soundQueue = {};
		s_audioPathToHandle.clear();
		s_audioRawPathToHandle.clear();

		if (s_Backend) {
			s_Backend->Shutdown();
			s_Backend.reset();
		}
		s_IsInitialized = false;
	}

	void AudioManager::Update() {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::Update must be called on the main thread");
		if (!s_IsInitialized) {
			return;
		}

		s_soundsPlayedThisFrame = 0;
		CleanupFinishedSounds();
		// One recalc — ProcessSoundQueue and the cleanup pass keep s_activeSoundCount
		// accurate via direct increments, so a second post-pass scan would just clobber
		// any in-flight increments and waste an O(N) walk.
		RecalculateActiveSoundCount();
		ProcessSoundQueue();
		UpdateListener();
		UpdateSoundInstances();
	}

	bool AudioManager::CanPlaySound(const AudioHandle& audioHandle, float priority) {
		if (priority >= 2.0f) {
			return true;
		}


		if (s_activeSoundCount >= s_maxConcurrentSounds) {
			return priority > 1.5f;
		}

		if (s_soundsPlayedThisFrame >= s_maxSoundsPerFrame) {
			return priority > 1.8f;
		}

		return true;
	}

	void AudioManager::ProcessSoundQueue() {
		// Bound the work this frame: at most maxStartsPerFrame *successful* starts.
		// Stale (age > 200ms) and throttled requests are skipped without consuming
		// the start-budget so they don't starve legitimate sounds — only successful
		// starts (or hard rejections like StartOneShotInstance failure) count.
		const uint32_t maxStartsPerFrame = 4;
		uint32_t startsThisCall = 0;
		uint32_t requeueGuard = 0;
		const uint32_t requeueGuardLimit = static_cast<uint32_t>(s_soundQueue.size()) + 16;

		// Local in-flight counter — added to s_activeSoundCount *only for the loop
		// bound check*. We deliberately do NOT mutate s_activeSoundCount here:
		// RecalculateActiveSoundCount (called once per Update tick) is the single
		// source of truth, since incrementing here would drift under any unmodelled
		// lifecycle event (failed start that half-allocated a slot, mid-frame
		// stop race with the audio thread, RecycleSoundInstance that bypassed
		// CleanupFinishedSounds). The recalc is O(MAX_CONCURRENT_SOUNDS)=64 —
		// cheap enough to stay authoritative, and we just avoid burning it
		// per-iteration of the queue drain.
		uint32_t inFlightStarts = 0;

		while (!s_soundQueue.empty() && startsThisCall < maxStartsPerFrame &&
			s_soundsPlayedThisFrame < s_maxSoundsPerFrame &&
			(s_activeSoundCount + inFlightStarts) < s_maxConcurrentSounds) {

			if (++requeueGuard > requeueGuardLimit) break;

			SoundRequest request = s_soundQueue.top();
			s_soundQueue.pop();

			auto now = std::chrono::steady_clock::now();
			auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - request.RequestTime);
			if (age.count() > 200) {
				continue; // dropped — too stale to bother
			}

			if (IsThrottled(request.Handle)) {
				continue; // skip but don't burn the start-budget
			}

			if (StartOneShotInstance(request.Handle, request.Volume)) {
				s_soundsPlayedThisFrame++;
				ThrottleSound(request.Handle);
				inFlightStarts++;
				startsThisCall++;
			}
		}
	}

	void AudioManager::ThrottleSound(const AudioHandle& audioHandle) {
		auto& limitData = s_soundLimits[audioHandle.GetHandle()];
		limitData.LastPlayTime = std::chrono::steady_clock::now();
		limitData.FramePlayCount++;
	}

	bool AudioManager::IsThrottled(const AudioHandle& audioHandle) {
		auto it = s_soundLimits.find(audioHandle.GetHandle());
		if (it == s_soundLimits.end()) {
			return false;
		}

		auto now = std::chrono::steady_clock::now();
		auto timeSinceLastPlay = std::chrono::duration_cast<std::chrono::milliseconds>(
			now - it->second.LastPlayTime);

		return timeSinceLastPlay.count() < (MIN_SOUND_INTERVAL * 1000);
	}

	void AudioManager::SetMaxConcurrentSounds(uint32_t maxSounds) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::SetMaxConcurrentSounds must be called on the main thread");
		s_maxConcurrentSounds = Min(maxSounds, 128u);
	}

	void AudioManager::SetMaxSoundsPerFrame(uint32_t maxPerFrame) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::SetMaxSoundsPerFrame must be called on the main thread");
		s_maxSoundsPerFrame = Min(maxPerFrame, 16u);
	}

	uint32_t AudioManager::GetActiveSoundCount() {
		return s_activeSoundCount;
	}

	AudioHandle AudioManager::LoadAudio(const std::string_view& path) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::LoadAudio must be called on the main thread");
		if (!s_IsInitialized) {
			IDX_CORE_ERROR("[{}] AudioManager not initialized", ErrorCodeToString(IndexErrorCode::NotInitialized));
			return AudioHandle();
		}

		const std::string requestedPath(path);

		// Raw-string fast path: hot scripts re-issue LoadAudio for the same
		// literal path many times. Probe the raw cache before touching the
		// filesystem so cache-hit cost is one hash lookup, not three syscalls.
		if (const AudioHandle existing = FindAudioByRawPath(requestedPath); existing.IsValid()) {
			return existing;
		}

		const std::string fullpath = NormalizeAudioPath(std::filesystem::path(requestedPath));
		if (const AudioHandle existing = FindAudioByPath(fullpath); existing.IsValid()) {
			// Promote into the raw-cache so the next LoadAudio for this exact
			// input string skips normalization too.
			s_audioRawPathToHandle[requestedPath] = existing.GetHandle();
			return existing;
		}

		const std::string rootPath = NormalizeAudioPath(std::filesystem::path(Path::Combine(s_RootPath, requestedPath)));
		if (!rootPath.empty() && rootPath != fullpath) {
			if (const AudioHandle existing = FindAudioByPath(rootPath); existing.IsValid()) {
				s_audioRawPathToHandle[requestedPath] = existing.GetHandle();
				return existing;
			}
		}

		std::string resolvedPath = fullpath;
		auto audio = std::make_unique<Audio>();
		if (!audio->LoadFromFile(resolvedPath)) {
			resolvedPath = rootPath;
			audio = std::make_unique<Audio>();
			if (resolvedPath.empty() || !audio->LoadFromFile(resolvedPath)) {
				IDX_CORE_ERROR("[{}] AudioManager: Failed to load audio: {}", ErrorCodeToString(IndexErrorCode::LoadFailed), requestedPath);
				return AudioHandle();
			}
		}

		AudioHandle::HandleType id = GenerateHandle();
		if (!RegisterAudioData(*audio)) {
			IDX_CORE_WARN_TAG("AudioManager", "Falling back to on-demand audio loading for '{}'", resolvedPath);
		}
		s_audioMap[id] = std::move(audio);
		s_audioPathToHandle[resolvedPath] = id;
		s_audioRawPathToHandle[requestedPath] = id;
		return AudioHandle(id);
	}

	AudioHandle AudioManager::LoadAudioByUUID(uint64_t assetId) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::LoadAudioByUUID must be called on the main thread");
		if (assetId == 0) {
			return AudioHandle();
		}

		// Optimistic path: probe both predicates first, only burn a full
		// AssetRegistry Sync (filesystem scan) if either misses. Collapses
		// two MarkDirty + Sync cycles into at most one, so a successful
		// lookup costs zero filesystem hits.
		bool isAudio = AssetRegistry::IsAudio(assetId);
		std::string path = isAudio ? AssetRegistry::ResolvePath(assetId) : std::string{};

		if (!isAudio || path.empty()) {
			AssetRegistry::MarkDirty();
			AssetRegistry::Sync();
			isAudio = AssetRegistry::IsAudio(assetId);
			if (!isAudio) {
				return AudioHandle();
			}
			path = AssetRegistry::ResolvePath(assetId);
			if (path.empty()) {
				return AudioHandle();
			}
		}

		return LoadAudio(path);
	}

	void AudioManager::UnloadAudio(const AudioHandle& audioHandle) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::UnloadAudio must be called on the main thread");
		if (!audioHandle.IsValid()) {
			return;
		}

		auto it = s_audioMap.find(audioHandle.GetHandle());
		if (it != s_audioMap.end()) {
			if (it->second) {
				s_audioPathToHandle.erase(it->second->GetFilepath());
			}

			// Lifetime ordering — DO NOT REORDER:
			//   1. Recycle every SoundInstance referencing this audio. Each
			//      calls ma_sound_uninit + ma_resource_manager_data_source_uninit,
			//      so miniaudio's audio thread releases its hold on the buffer.
			//   2. Unregister the resource manager entry. miniaudio drops its
			//      pointer to audio->GetData() / GetFilepath().
			//   3. Erase from s_audioMap. Audio destructor runs HERE, freeing
			//      the PCM buffer and the filepath string. By this point no
			//      miniaudio code path can reach the buffer — without the
			//      ordering, the audio thread could read a freed buffer.
			for (size_t i = 0; i < s_soundInstances.size(); ++i) {
				auto& slot = s_soundInstances[i];
				if (slot && slot->IsValid && slot->AudioHandle == audioHandle) {
					RecycleSoundInstance(static_cast<uint32_t>(i));
				}
			}

			UnregisterAudioData(*it->second);
			s_audioMap.erase(it);
			s_soundLimits.erase(audioHandle.GetHandle());
		}
	}

	void AudioManager::UnloadAllAudio() {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::UnloadAllAudio must be called on the main thread");

		for (size_t i = 0; i < s_soundInstances.size(); ++i) {
			auto& slot = s_soundInstances[i];
			if (slot && slot->IsValid) {
				RecycleSoundInstance(static_cast<uint32_t>(i));
			}
		}

		for (const auto& [id, audio] : s_audioMap) {
			if (audio) {
				UnregisterAudioData(*audio);
			}
		}

		s_audioMap.clear();
		s_audioPathToHandle.clear();
		s_audioRawPathToHandle.clear();
		s_nextHandle = 1;
		s_soundLimits.clear();
		s_soundQueue = {};
		s_activeSoundCount = 0;
	}

	size_t AudioManager::PurgeUnreferenced() {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::PurgeUnreferenced must be called on the main thread");
		if (!s_IsInitialized) {
			IDX_CORE_WARN("AudioManager isn't initialized");
			return 0;
		}

		// AudioSourceComponent is the only component holding an AudioHandle today.
		std::unordered_set<AudioHandle::HandleType> referenced;
		referenced.reserve(s_audioMap.size());

		SceneManager::Get().ForeachLoadedScene([&referenced](Scene& scene) {
			entt::registry& registry = scene.GetRegistry();

			auto sources = registry.view<AudioSourceComponent>();
			for (auto entity : sources) {
				const auto& source = sources.get<AudioSourceComponent>(entity);
				const AudioHandle& handle = source.GetAudioHandle();
				if (handle.IsValid()) {
					referenced.insert(handle.GetHandle());
				}
			}
		});

		// Collect doomed handles before freeing — can't mutate s_audioMap mid-iteration.
		std::vector<AudioHandle> toFree;
		toFree.reserve(s_audioMap.size());
		for (const auto& [id, audio] : s_audioMap) {
			if (referenced.find(id) == referenced.end()) {
				toFree.emplace_back(id);
			}
		}

		for (const AudioHandle& handle : toFree) {
			UnloadAudio(handle);
		}

		const size_t freedCount = toFree.size();
		IDX_CORE_INFO_TAG("AudioManager", "Purged {} unreferenced audio entries", freedCount);
		return freedCount;
	}

	void AudioManager::PlayAudioSource(AudioSourceComponent& source) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::PlayAudioSource must be called on the main thread");
		if (!s_IsInitialized) {
			IDX_CORE_WARN("AudioManager not initialized");
			return;
		}
		if (!source.GetAudioHandle().IsValid()) {
			IDX_CORE_WARN("[{}] Invalid AudioHandle", ErrorCodeToString(IndexErrorCode::InvalidHandle));
			return;
		}

		if (source.GetInstanceId() != 0) {
			StopAudioSource(source);
		}

		uint32_t instanceId = CreateSoundInstance(source.GetAudioHandle());
		if (instanceId == 0) {
			IDX_CORE_ERROR("[{}] Failed to create sound instance", ErrorCodeToString(IndexErrorCode::LoadFailed));
			return;
		}

		source.SetInstanceId(instanceId);
		SoundInstance* instance = GetSoundInstance(instanceId);

		if (instance) {
			ma_sound_set_volume(&instance->Sound, source.GetVolume());
			ma_sound_set_pitch(&instance->Sound, source.GetPitch());
			ma_sound_set_looping(&instance->Sound, source.IsLooping());
			ma_sound_set_positioning(&instance->Sound, ma_positioning_relative);


			ma_result result = ma_sound_start(&instance->Sound);
			if (result != MA_SUCCESS) {
				IDX_CORE_ERROR("[{}] Failed to start sound playback. Error: {}", ErrorCodeToString(IndexErrorCode::LoadFailed), static_cast<int>(result));
				source.SetInstanceId(0);
				DestroySoundInstance(instanceId);
			}
		}
		else {
			IDX_CORE_ERROR("[{}] Failed to retrieve sound instance after creation", ErrorCodeToString(IndexErrorCode::NullReference));
			source.SetInstanceId(0);
			DestroySoundInstance(instanceId);
		}
	}

	void AudioManager::PauseAudioSource(AudioSourceComponent& source) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::PauseAudioSource must be called on the main thread");
		if (!s_IsInitialized || source.GetInstanceId() == 0) {
			return;
		}

		SoundInstance* instance = GetSoundInstance(source.GetInstanceId());
		if (instance && instance->IsValid) {
			ma_sound_stop(&instance->Sound);
		}
	}

	void AudioManager::StopAudioSource(AudioSourceComponent& source) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::StopAudioSource must be called on the main thread");
		if (!s_IsInitialized || source.GetInstanceId() == 0) {
			return;
		}

		DestroySoundInstance(source.GetInstanceId());
		source.SetInstanceId(0);
	}

	void AudioManager::ResumeAudioSource(AudioSourceComponent& source) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::ResumeAudioSource must be called on the main thread");
		if (!s_IsInitialized || source.GetInstanceId() == 0) {
			return;
		}

		SoundInstance* instance = GetSoundInstance(source.GetInstanceId());
		if (instance && instance->IsValid) {
			ma_sound_start(&instance->Sound);
		}
	}

	void AudioManager::SetMasterVolume(float volume) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::SetMasterVolume must be called on the main thread");
		s_masterVolume = Max(0.0f, volume);

		if (s_IsInitialized && s_Backend) {
			s_Backend->SetMasterVolume(s_masterVolume);
		}
	}

	void AudioManager::PlayOneShot(const AudioHandle& audioHandle, float volume) {
		IDX_CORE_ASSERT(Application::IsMainThread(), IndexErrorCode::Undefined, "AudioManager::PlayOneShot must be called on the main thread");
		if (!s_IsInitialized || !audioHandle.IsValid()) {
			return;
		}

		SoundRequest request{};
		request.Handle = audioHandle;
		request.Volume = volume;
		request.Priority = 1.0f;
		request.RequestTime = std::chrono::steady_clock::now();

		if (IsThrottled(audioHandle) || !CanPlaySound(audioHandle, request.Priority)) {
			s_soundQueue.push(request);
			return;
		}

		if (StartOneShotInstance(audioHandle, volume)) {
			s_soundsPlayedThisFrame++;
			// Don't recalc here. The next Update tick's
			// CleanupFinishedSounds + RecalculateActiveSoundCount pair is
			// the single source of truth (see ProcessSoundQueue). A local
			// increment would risk drift, and the once-per-frame recalc
			// is cheap enough to handle this slot count too — keeping it
			// out of the hot PlayOneShot path matters when scripts fire
			// many one-shots in a single frame.
			ThrottleSound(audioHandle);
		}
	}

	bool AudioManager::RegisterAudioData(const Audio& audio) {
		if (!s_IsInitialized || !audio.IsLoaded() || audio.GetData() == nullptr || audio.GetFrameCount() == 0) {
			return false;
		}

		ma_engine* engine = GetActiveMiniaudioEngine();
		if (!engine) {
			return false;
		}
		ma_resource_manager* resourceManager = ma_engine_get_resource_manager(engine);
		if (!resourceManager) {
			return false;
		}

		const ma_result result = ma_resource_manager_register_decoded_data(
			resourceManager,
			audio.GetFilepath().c_str(),
			audio.GetData(),
			audio.GetFrameCount(),
			audio.GetFormat(),
			audio.GetChannels(),
			audio.GetSampleRate());
		if (result != MA_SUCCESS) {
			IDX_CORE_WARN_TAG("AudioManager", "Failed to register decoded audio data for '{}': {}", audio.GetFilepath(), static_cast<int>(result));
			return false;
		}

		return true;
	}

	void AudioManager::UnregisterAudioData(const Audio& audio) {
		if (!s_IsInitialized || audio.GetFilepath().empty()) {
			return;
		}

		if (ma_engine* engine = GetActiveMiniaudioEngine()) {
			if (ma_resource_manager* resourceManager = ma_engine_get_resource_manager(engine)) {
				ma_resource_manager_unregister_data(resourceManager, audio.GetFilepath().c_str());
			}
		}
	}

	bool AudioManager::IsAudioLoaded(const AudioHandle& audioHandle) {
		if (!audioHandle.IsValid()) {
			return false;
		}

		return s_audioMap.find(audioHandle.GetHandle()) != s_audioMap.end();
	}

	const Audio* AudioManager::GetAudio(const AudioHandle& audioHandle) {
		if (!audioHandle.IsValid()) {
			return nullptr;
		}

		auto it = s_audioMap.find(audioHandle.GetHandle());
		return (it != s_audioMap.end()) ? it->second.get() : nullptr;
	}

	std::string AudioManager::GetAudioName(const AudioHandle& audioHandle) {
		const Audio* audio = GetAudio(audioHandle);
		if (!audio) return "";
		return audio->GetFilepath();
	}

	uint64_t AudioManager::GetAudioAssetUUID(const AudioHandle& audioHandle) {
		const Audio* audio = GetAudio(audioHandle);
		if (!audio) {
			return 0;
		}

		return AssetRegistry::GetOrCreateAssetUUID(audio->GetFilepath());
	}

	AudioHandle::HandleType AudioManager::GenerateHandle() {
		return s_nextHandle++;
	}

	AudioHandle AudioManager::FindAudioByPath(const std::string& path) {
		auto pathIt = s_audioPathToHandle.find(path);
		if (pathIt == s_audioPathToHandle.end()) {
			return AudioHandle();
		}

		auto audioIt = s_audioMap.find(pathIt->second);
		if (audioIt != s_audioMap.end() && audioIt->second && audioIt->second->GetFilepath() == path) {
			return AudioHandle(pathIt->second);
		}

		s_audioPathToHandle.erase(pathIt);
		return AudioHandle();
	}

	AudioHandle AudioManager::FindAudioByRawPath(const std::string& rawPath) {
		// Fast-path cache probe keyed on whatever the caller handed in.
		// Skips NormalizeAudioPath (3–4 syscalls) on repeated loads of the
		// same string. The raw key is not authoritative — if the entry is
		// stale (audio was unloaded), erase it and let the caller fall
		// through to the normalized lookup.
		auto it = s_audioRawPathToHandle.find(rawPath);
		if (it == s_audioRawPathToHandle.end()) {
			return AudioHandle();
		}

		auto audioIt = s_audioMap.find(it->second);
		if (audioIt != s_audioMap.end() && audioIt->second) {
			return AudioHandle(it->second);
		}

		s_audioRawPathToHandle.erase(it);
		return AudioHandle();
	}

	uint32_t AudioManager::CreateSoundInstance(const AudioHandle& audioHandle) {
		if (!audioHandle.IsValid()) {
			return 0;
		}

		const Audio* audio = GetAudio(audioHandle);
		if (!audio || !audio->IsLoaded()) {
			return 0;
		}

		ma_engine* engine = GetActiveMiniaudioEngine();
		if (!engine) {
			return 0;
		}

		uint32_t index;
		uint32_t reuseGeneration = 0;

		if (!s_freeInstanceIndices.empty()) {
			index = s_freeInstanceIndices.back();
			s_freeInstanceIndices.pop_back();
			// Carry the slot's prior generation across the recycle so a stale handle
			// minted before this point fails GetSoundInstance.
			if (index < s_soundInstances.size() && s_soundInstances[index]) {
				reuseGeneration = s_soundInstances[index]->Generation;
			}
			s_soundInstances[index] = std::make_unique<SoundInstance>();
			s_soundInstances[index]->Generation = reuseGeneration;
		}
		else {
			index = static_cast<uint32_t>(s_soundInstances.size());
			s_soundInstances.emplace_back(std::make_unique<SoundInstance>());
		}

		SoundInstance& instance = *s_soundInstances[index];
		const ma_uint32 dataSourceFlags = MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE;
		ma_result result = ma_resource_manager_data_source_init(
			ma_engine_get_resource_manager(engine),
			audio->GetFilepath().c_str(),
			dataSourceFlags,
			nullptr,
			&instance.DataSource);
		if (result != MA_SUCCESS) {
			IDX_CORE_WARN("[{}] AudioManager: Failed to create sound data source. Error: {}", ErrorCodeToString(IndexErrorCode::LoadFailed), static_cast<int>(result));
			if (index == s_soundInstances.size() - 1) {
				s_soundInstances.pop_back();
			}
			else {
				// Capture the bumped generation BEFORE resetting the slot. Mirroring
				// RecycleSoundInstance: write to a brand-new sentinel SoundInstance so
				// the slot is non-null when CreateSoundInstance reads
				// s_soundInstances[index]->Generation on the next reuse. The previous
				// order wrote Generation into the about-to-be-destroyed instance, then
				// reset() to nullptr, then re-queued an index whose slot lookup yielded
				// reuseGeneration=0 — silently aliasing stale handles.
				const uint32_t nextGeneration = reuseGeneration + 1u;
				s_soundInstances[index].reset();
				s_soundInstances[index] = std::make_unique<SoundInstance>();
				s_soundInstances[index]->Generation = nextGeneration;
				s_soundInstances[index]->IsValid = false;
				s_freeInstanceIndices.push_back(index);
			}
			return 0;
		}

		instance.HasDataSource = true;
		result = ma_sound_init_from_data_source(engine, &instance.DataSource, 0, nullptr, &instance.Sound);
		if (result != MA_SUCCESS) {
			ma_resource_manager_data_source_uninit(&instance.DataSource);
			instance.HasDataSource = false;
			IDX_CORE_WARN("[{}] AudioManager: Failed to create sound instance. Error: {}", ErrorCodeToString(IndexErrorCode::LoadFailed), static_cast<int>(result));
			if (index == s_soundInstances.size() - 1) {
				s_soundInstances.pop_back();
			}
			else {
				// Same capture-then-rebuild pattern as the data-source-init failure
				// branch above — see comment there.
				const uint32_t nextGeneration = reuseGeneration + 1u;
				s_soundInstances[index].reset();
				s_soundInstances[index] = std::make_unique<SoundInstance>();
				s_soundInstances[index]->Generation = nextGeneration;
				s_soundInstances[index]->IsValid = false;
				s_freeInstanceIndices.push_back(index);
			}
			return 0;
		}

		instance.AudioHandle = audioHandle;
		instance.IsValid = true;

		return EncodeAudioInstanceId(index, instance.Generation);
	}

	void AudioManager::DestroySoundInstance(uint32_t instanceId) {
		if (!DecodedAudioInstanceIsValid(instanceId)) {
			return;
		}

		const uint32_t index = DecodeAudioInstanceIndex(instanceId);
		if (index >= s_soundInstances.size()) {
			return;
		}

		// Only recycle when the generation matches; a stale handle pointing at a
		// recycled slot must be a no-op rather than freeing the live sound.
		// Compare the masked low 24 bits of slot->Generation — encoded IDs
		// only carry that many bits, so the runtime counter is the source
		// of truth but matching uses the truncated form.
		auto& slot = s_soundInstances[index];
		if (!slot || (slot->Generation & k_AudioInstanceGenerationMask)
				!= DecodeAudioInstanceGeneration(instanceId)) {
			return;
		}

		RecycleSoundInstance(index);
	}

	AudioManager::SoundInstance* AudioManager::GetSoundInstance(uint32_t instanceId) {
		if (!DecodedAudioInstanceIsValid(instanceId)) {
			return nullptr;
		}

		const uint32_t index = DecodeAudioInstanceIndex(instanceId);
		if (index >= s_soundInstances.size()) {
			return nullptr;
		}

		auto& slot = s_soundInstances[index];
		if (!slot || !slot->IsValid) {
			return nullptr;
		}
		// Generation mismatch = the caller's id was minted before this slot was recycled.
		// Returning nullptr is the whole point of the generation field. Mask the
		// stored counter to the encoded width so a slot whose runtime counter
		// has grown past 2^24 still matches the encoded id's low 24 bits.
		if ((slot->Generation & k_AudioInstanceGenerationMask) != DecodeAudioInstanceGeneration(instanceId)) {
			return nullptr;
		}

		return slot.get();
	}

	void AudioManager::RecycleSoundInstance(uint32_t index) {
		if (index >= s_soundInstances.size()) {
			return;
		}

		auto& slot = s_soundInstances[index];
		if (!slot || !slot->IsValid) {
			return;
		}

		SoundInstance& instance = *slot;
		ma_sound_stop(&instance.Sound);
		ma_sound_uninit(&instance.Sound);
		if (instance.HasDataSource) {
			ma_resource_manager_data_source_uninit(&instance.DataSource);
			instance.HasDataSource = false;
		}

		// Bump generation BEFORE tearing down the unique_ptr, so any handle we just
		// invalidated is recorded against the stale generation rather than the (zero)
		// default that a freshly-allocated SoundInstance would have.
		const uint32_t nextGeneration = instance.Generation + 1u;

		// Reset the unique_ptr so the SoundInstance (and its embedded ma_sound, which
		// the audio thread was reading) is fully torn down before the slot is reused.
		slot.reset();

		// Re-allocate an empty sentinel that just carries the bumped generation forward,
		// so CreateSoundInstance's reuseGeneration read sees the correct value next time.
		slot = std::make_unique<SoundInstance>();
		slot->Generation = nextGeneration;
		// Mark as not-IsValid so anyone holding the old id who slips past the generation
		// check (they shouldn't) still sees an inert slot.
		slot->IsValid = false;
		s_freeInstanceIndices.push_back(index);
	}

	void AudioManager::CleanupFinishedSounds() {
		for (size_t i = 0; i < s_soundInstances.size(); ++i) {
			auto& slot = s_soundInstances[i];
			if (!slot || !slot->IsValid) continue;
			SoundInstance& instance = *slot;

			// "not playing && not looping" alone matches *paused* sounds too — recycling
			// them would silently invalidate AudioSourceComponent::m_InstanceId and break
			// any later Resume(). Require ma_sound_at_end to distinguish real finishers
			// from mid-stream pauses (same predicate AudioSourceComponent::IsPaused uses).
			if (!ma_sound_is_playing(&instance.Sound)
				&& !ma_sound_is_looping(&instance.Sound)
				&& ma_sound_at_end(&instance.Sound) == MA_TRUE) {
				RecycleSoundInstance(static_cast<uint32_t>(i));
			}
		}
	}

	void AudioManager::UpdateListener() {
		if (!s_IsInitialized) {
			return;
		}
	}

	void AudioManager::UpdateSoundInstances() {
		if (!s_IsInitialized) {
			return;
		}
	}

	void AudioManager::RecalculateActiveSoundCount() {
		s_activeSoundCount = 0;
		for (const auto& slot : s_soundInstances) {
			if (slot && slot->IsValid && ma_sound_is_playing(&slot->Sound)) {
				s_activeSoundCount++;
			}
		}
	}

	bool AudioManager::StartOneShotInstance(const AudioHandle& audioHandle, float volume) {
		const uint32_t instanceId = CreateSoundInstance(audioHandle);
		if (instanceId == 0) {
			IDX_CORE_WARN("[{}] Failed to create one-shot sound instance", ErrorCodeToString(IndexErrorCode::LoadFailed));
			return false;
		}

		SoundInstance* instance = GetSoundInstance(instanceId);
		if (!instance) {
			IDX_CORE_WARN("[{}] Failed to retrieve one-shot sound instance", ErrorCodeToString(IndexErrorCode::NullReference));
			DestroySoundInstance(instanceId);
			return false;
		}

		ma_sound_set_volume(&instance->Sound, volume);
		const ma_result result = ma_sound_start(&instance->Sound);
		if (result != MA_SUCCESS) {
			IDX_CORE_WARN("[{}] Failed to start one-shot sound. Error: {}", ErrorCodeToString(IndexErrorCode::LoadFailed), static_cast<int>(result));
			DestroySoundInstance(instanceId);
			return false;
		}

		return true;
	}

}
