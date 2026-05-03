#include "pch.hpp"
#include "Profiling/Profiler.hpp"

#include "Core/Application.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

// The whole TU is no-op when the profiler is stripped — premake also
// removes this file from the build via removefiles, so this guard is
// belt-and-braces.
#ifdef AXIOM_PROFILER_ENABLED

namespace Axiom {

	namespace {
		// Module storage. We keep a stable index map alongside a vector so
		// the panel can iterate in registration order while lookups stay
		// O(1). Modules are never removed; capacity changes (TrackingSpan)
		// just resize the ring buffers in place.
		std::vector<ProfilerModule> g_Modules;
		std::unordered_map<std::string, size_t> g_Index;

		// All public entry points hold this lock briefly. The mutex is
		// uncontested in the normal case (one push per frame from the main
		// thread + one read from the panel render on the same thread). The
		// guard is here because Tracy itself is multi-threaded and we want
		// the option to push samples from worker threads later without a
		// re-architecture. Lock granularity: per-call, never held across
		// chrono::now() or vector growth.
		std::mutex g_Mutex;

		// Gates. All three must be satisfied for samples to land in ring
		// buffers. Tracy zones still fire regardless (they're always cheap
		// and useful for the standalone viewer).
		//
		//   PanelVisible / BackgroundTracking: at least one must be true.
		//     User-facing toggle: "should the engine bother collecting?"
		//   IsPlaying + !IsPaused: profiler only collects in playmode.
		//     Editor-specific UX rule. In the runtime IsPlaying is always
		//     true at startup so this is a no-op there.
		bool g_PanelVisible = false;
		bool g_BackgroundTracking = false;

		int g_SamplingHz = 60;
		int g_TrackingSpan = 200;

		bool IsCollectingNow() {
			if (!g_PanelVisible && !g_BackgroundTracking) return false;

			// Editor-only constraint: only collect during play. The runtime
			// flips IsPlaying=true on launch so this is automatically true
			// there. The launcher disables scripting entirely so it doesn't
			// reach this code path. Pausing the editor (manual pause OR
			// background pause) suspends collection too — your numbers
			// shouldn't drift while the engine is stopped.
			//
			// Tests run without an Application instance (s_Instance == nullptr).
			// In that case skip the play/pause check so the test harness can
			// drive the profiler directly.
			if (Application::GetInstance() != nullptr) {
				if (!Application::GetIsPlaying()) return false;
				if (Application::IsPaused()) return false;
			}

			return true;
		}

		// Per-module sampling-rate gate. Was previously a single shared
		// timestamp, which meant module N+1 always saw "less than 1/Hz
		// seconds since the previous push" and got dropped. Each module now
		// observes its own cadence independently.
		bool ShouldAcceptModuleSample(ProfilerModule& m) {
			if (!IsCollectingNow()) return false;
			if (g_SamplingHz <= 0) return true;

			using namespace std::chrono;
			const auto now = steady_clock::now();
			const auto interval = duration_cast<microseconds>(
				duration<double>(1.0 / static_cast<double>(g_SamplingHz)));
			if (now - m.LastPushTime < interval) return false;
			m.LastPushTime = now;
			return true;
		}

		// Mutates a module's ring buffer with a new value. Maintains:
		//   - CurrentValue (always = newest sample)
		//   - RunningSum (incremental; subtracts the value being overwritten)
		//   - AvgValue (RunningSum / Count)
		//   - MinValue / MaxValue (recomputed only when overwriting an
		//     extremum, so steady-state cost is O(1))
		void RecordSample(ProfilerModule& m, float value) {
			m.CurrentValue = value;

			if (m.Samples.size() != static_cast<size_t>(g_TrackingSpan)) {
				m.Samples.assign(g_TrackingSpan, 0.0f);
				m.Head = 0;
				m.Count = 0;
				m.RunningSum = 0.0;
			}

			const float overwritten = (m.Count == m.Samples.size())
				? m.Samples[m.Head]
				: 0.0f;

			m.Samples[m.Head] = value;
			m.Head = (m.Head + 1) % m.Samples.size();
			if (m.Count < m.Samples.size()) {
				m.Count++;
			}
			m.RunningSum += value - overwritten;
			m.AvgValue = m.Count > 0 ? static_cast<float>(m.RunningSum / m.Count) : 0.0f;

			// Min/Max: cheap path when the new sample doesn't dethrone the
			// current extremum and the overwritten sample wasn't itself
			// the extremum. Otherwise rescan. This keeps steady-state at
			// O(1) and worst-case (replacing the min or max) at O(N).
			const bool needRescan = (m.Count == m.Samples.size()) &&
				(overwritten == m.MinValue || overwritten == m.MaxValue);
			if (m.Count == 1) {
				m.MinValue = value;
				m.MaxValue = value;
			}
			else if (needRescan) {
				auto begin = m.Samples.begin();
				auto end = begin + static_cast<std::ptrdiff_t>(m.Count);
				const auto [mn, mx] = std::minmax_element(begin, end);
				m.MinValue = *mn;
				m.MaxValue = *mx;
			}
			else {
				m.MinValue = std::min(m.MinValue, value);
				m.MaxValue = std::max(m.MaxValue, value);
			}
		}

		void ClearRingBuffer(ProfilerModule& m) {
			std::fill(m.Samples.begin(), m.Samples.end(), 0.0f);
			m.Head = 0;
			m.Count = 0;
			m.RunningSum = 0.0;
			m.CurrentValue = 0.0f;
			m.AvgValue = 0.0f;
			m.MinValue = 0.0f;
			m.MaxValue = 0.0f;
			// Reset cadence so the next push lands immediately rather than
			// being gated by a stale timestamp from before the clear.
			m.LastPushTime = std::chrono::steady_clock::time_point{};
		}
	} // namespace

	void Profiler::Initialize() {
		std::scoped_lock lock(g_Mutex);
		g_Modules.clear();
		g_Index.clear();
		// The names below are the contract between instrumentation sites
		// (which push by name) and the ProfilerPanel UI (which renders by
		// name, grouped into four user-facing categories). Order here is
		// the panel's display order within each category.
		//
		//   CPU Usage:   Rendering / Scripts / Physics / VSync / Others
		//   Rendering:   Batches / Triangles / Vertices
		//   Memory:      Total Memory / Texture Memory / Object Count
		//   Audio:       Playing Sources
		//
		// "Frame Time" is registered too — it's not shown in the panel,
		// but the end-of-frame code reads it to derive the "Others" residual
		// (Frame - Render - Scripts - Physics - VSync = unattributed time).
		const char* names[] = {
			// Internal — hidden from panel, used only for Others derivation.
			"Frame Time",

			// CPU Usage category
			"Rendering",
			"Scripts",
			"Physics",
			"VSync",
			"Others",

			// Rendering category
			"Batches",
			"Triangles",
			"Vertices",

			// Memory category
			"Total Memory",
			"Texture Memory",
			"Object Count",

			// Audio category
			"Playing Sources"
		};
		for (const char* name : names) {
			ProfilerModule m;
			m.Name = name;
			m.Samples.assign(g_TrackingSpan, 0.0f);
			g_Index[name] = g_Modules.size();
			g_Modules.push_back(std::move(m));
		}
	}

	void Profiler::Shutdown() {
		std::scoped_lock lock(g_Mutex);
		g_Modules.clear();
		g_Index.clear();
		g_PanelVisible = false;
		g_BackgroundTracking = false;
	}

	ProfilerModule* Profiler::Register(const std::string& name) {
		std::scoped_lock lock(g_Mutex);
		auto it = g_Index.find(name);
		if (it != g_Index.end()) return &g_Modules[it->second];

		ProfilerModule m;
		m.Name = name;
		m.Samples.assign(g_TrackingSpan, 0.0f);
		g_Index[name] = g_Modules.size();
		g_Modules.push_back(std::move(m));
		return &g_Modules.back();
	}

	ProfilerModule* Profiler::Find(const std::string& name) {
		std::scoped_lock lock(g_Mutex);
		auto it = g_Index.find(name);
		if (it == g_Index.end()) return nullptr;
		return &g_Modules[it->second];
	}

	const std::vector<ProfilerModule>& Profiler::AllModules() {
		// No lock here: caller is the panel render on the main thread and
		// reads-only. The vector itself never relocates between Initialize
		// and Shutdown (we don't add modules dynamically post-init in the
		// normal flow). The lock would still be required if a worker thread
		// called Register concurrently — none does today. Documented assumption.
		return g_Modules;
	}

	float Profiler::GetCurrentValue(const std::string& name) {
		std::scoped_lock lock(g_Mutex);
		auto it = g_Index.find(name);
		if (it == g_Index.end()) return 0.0f;
		return g_Modules[it->second].CurrentValue;
	}

	void Profiler::PushValue(const std::string& name, float value) {
		std::scoped_lock lock(g_Mutex);
		auto it = g_Index.find(name);
		if (it == g_Index.end()) return;
		ProfilerModule& m = g_Modules[it->second];
		if (!m.Enabled) return;
		if (!ShouldAcceptModuleSample(m)) return;
		RecordSample(m, value);
	}

	void Profiler::PushSample(const std::string& name, float milliseconds) {
		// Same path as PushValue — separate function only so call sites
		// document intent (duration vs. arbitrary numeric).
		PushValue(name, milliseconds);
	}

	void Profiler::OnFrameMark(const std::string& /*name*/) {
		// Reserved hook — no per-frame work needed today, but having the
		// macro call this gives us a single chokepoint to add things like
		// per-frame stats reset, GPU timer poll, etc., without touching
		// every instrumentation site.
	}

	void Profiler::PushFrameDelta(float deltaSeconds) {
		// Frame Time is a hidden internal module — it's not rendered in the
		// panel directly, but the "Others" residual computation reads it to
		// derive `Frame - Render - Scripts - Physics - VSync`.
		const float frameMs = deltaSeconds * 1000.0f;

		std::scoped_lock lock(g_Mutex);
		if (auto it = g_Index.find("Frame Time"); it != g_Index.end()) {
			ProfilerModule& m = g_Modules[it->second];
			if (m.Enabled && ShouldAcceptModuleSample(m)) RecordSample(m, frameMs);
		}
	}

	void Profiler::SetPanelVisible(bool visible) {
		std::scoped_lock lock(g_Mutex);
		g_PanelVisible = visible;
	}

	void Profiler::SetBackgroundTracking(bool enabled) {
		std::scoped_lock lock(g_Mutex);
		g_BackgroundTracking = enabled;
	}

	bool Profiler::IsCollecting() {
		std::scoped_lock lock(g_Mutex);
		// Mirror IsCollectingNow() so the panel can show "(paused)" or
		// "(not playing)" hints if it wants. Currently informational only.
		if (!g_PanelVisible && !g_BackgroundTracking) return false;
		if (Application::GetInstance() != nullptr) {
			if (!Application::GetIsPlaying()) return false;
			if (Application::IsPaused()) return false;
		}
		return true;
	}

	void Profiler::SetSamplingHz(int hz) {
		std::scoped_lock lock(g_Mutex);
		g_SamplingHz = std::max(0, hz);
	}

	void Profiler::SetTrackingSpan(int span) {
		std::scoped_lock lock(g_Mutex);
		g_TrackingSpan = std::max(1, span);
		// Resize all existing module ring buffers so the next sample lands
		// in the new shape. RecordSample's "size mismatch -> reset" path
		// would also catch this, but doing it eagerly keeps the panel's
		// graph from showing a partial buffer at the wrong scale.
		for (auto& m : g_Modules) {
			ClearRingBuffer(m);
			m.Samples.assign(g_TrackingSpan, 0.0f);
		}
	}

	int Profiler::GetSamplingHz() {
		std::scoped_lock lock(g_Mutex);
		return g_SamplingHz;
	}

	int Profiler::GetTrackingSpan() {
		std::scoped_lock lock(g_Mutex);
		return g_TrackingSpan;
	}

	void Profiler::SetModuleEnabled(const std::string& name, bool enabled) {
		std::scoped_lock lock(g_Mutex);
		auto it = g_Index.find(name);
		if (it == g_Index.end()) return;
		ProfilerModule& m = g_Modules[it->second];
		if (m.Enabled == enabled) return;
		m.Enabled = enabled;
		if (!enabled) {
			ClearRingBuffer(m);
		}
	}

} // namespace Axiom

#endif // AXIOM_PROFILER_ENABLED
