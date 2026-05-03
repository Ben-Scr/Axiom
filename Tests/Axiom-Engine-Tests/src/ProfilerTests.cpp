// Tests for Axiom::Profiler.
//
// These exercise the parts that don't need a window or a GL context — ring
// buffer + avg/min/max bookkeeping, sampling-rate cadence, gate logic, and
// per-module enable/disable. The Tracy backend is exercised implicitly
// (the macros expand into both Tracy + ring-buffer pushes; here we drive
// the in-engine ring-buffer side directly via Profiler::PushSample).

#include <doctest/doctest.h>

#ifdef AXIOM_PROFILER_ENABLED

#include "Profiling/Profiler.hpp"

#include <chrono>
#include <thread>

using namespace Axiom;

namespace {
	// Each test starts from a clean Profiler state. We can't strictly avoid
	// state bleed without making Profiler a per-instance class (it's a
	// process-global by design), so the convention is: every test calls
	// Initialize() up front, which resets module storage and re-registers
	// the built-ins. SetPanelVisible(true) is what unlocks sample writes.
	void ResetProfiler() {
		Profiler::Shutdown();
		Profiler::Initialize();
		Profiler::SetSamplingHz(0); // 0 = unlimited; lets every push land
		Profiler::SetTrackingSpan(8);
		Profiler::SetPanelVisible(true);
		Profiler::SetBackgroundTracking(false);
	}
} // namespace

TEST_CASE("Profiler — push records value and updates avg/min/max") {
	ResetProfiler();

	Profiler::PushValue("Physics", 10.0f);
	Profiler::PushValue("Physics", 20.0f);
	Profiler::PushValue("Physics", 30.0f);

	const ProfilerModule* m = Profiler::Find("Physics");
	REQUIRE(m != nullptr);
	CHECK(m->Count == 3);
	CHECK(m->CurrentValue == doctest::Approx(30.0f));
	CHECK(m->MinValue == doctest::Approx(10.0f));
	CHECK(m->MaxValue == doctest::Approx(30.0f));
	CHECK(m->AvgValue == doctest::Approx(20.0f));
}

TEST_CASE("Profiler — ring buffer wraps and avg follows the window") {
	ResetProfiler();

	// Span = 8. Push 10 samples; expect the oldest two (1, 2) to fall out
	// and the avg to be the mean of the last 8 (3..10 = 6.5).
	for (int i = 1; i <= 10; ++i) {
		Profiler::PushValue("Physics", float(i));
	}

	const ProfilerModule* m = Profiler::Find("Physics");
	REQUIRE(m != nullptr);
	CHECK(m->Count == 8);
	CHECK(m->CurrentValue == doctest::Approx(10.0f));
	CHECK(m->MinValue == doctest::Approx(3.0f));
	CHECK(m->MaxValue == doctest::Approx(10.0f));
	CHECK(m->AvgValue == doctest::Approx(6.5f));
}

TEST_CASE("Profiler — disabling a module clears its ring buffer") {
	ResetProfiler();

	Profiler::PushValue("Physics", 5.0f);
	Profiler::PushValue("Physics", 10.0f);
	REQUIRE(Profiler::Find("Physics")->Count == 2);

	Profiler::SetModuleEnabled("Physics", false);
	const ProfilerModule* m = Profiler::Find("Physics");
	REQUIRE(m != nullptr);
	CHECK_FALSE(m->Enabled);
	CHECK(m->Count == 0);
	CHECK(m->CurrentValue == doctest::Approx(0.0f));

	// Pushes while disabled are dropped — re-enabling shouldn't suddenly
	// reveal "buffered" data.
	Profiler::PushValue("Physics", 99.0f);
	CHECK(Profiler::Find("Physics")->Count == 0);

	Profiler::SetModuleEnabled("Physics", true);
	Profiler::PushValue("Physics", 7.0f);
	CHECK(Profiler::Find("Physics")->Count == 1);
	CHECK(Profiler::Find("Physics")->CurrentValue == doctest::Approx(7.0f));
}

TEST_CASE("Profiler — gates: panel hidden + background off => no collection") {
	ResetProfiler();
	Profiler::SetPanelVisible(false);
	Profiler::SetBackgroundTracking(false);
	REQUIRE_FALSE(Profiler::IsCollecting());

	Profiler::PushValue("Physics", 42.0f);
	CHECK(Profiler::Find("Physics")->Count == 0);

	SUBCASE("background tracking unlocks collection") {
		Profiler::SetBackgroundTracking(true);
		REQUIRE(Profiler::IsCollecting());
		Profiler::PushValue("Physics", 17.0f);
		CHECK(Profiler::Find("Physics")->Count == 1);
		CHECK(Profiler::Find("Physics")->CurrentValue == doctest::Approx(17.0f));
	}

	SUBCASE("panel-visible unlocks collection") {
		Profiler::SetPanelVisible(true);
		REQUIRE(Profiler::IsCollecting());
		Profiler::PushValue("Physics", 23.0f);
		CHECK(Profiler::Find("Physics")->Count == 1);
		CHECK(Profiler::Find("Physics")->CurrentValue == doctest::Approx(23.0f));
	}
}

TEST_CASE("Profiler — sampling rate gate drops pushes below cadence") {
	ResetProfiler();

	// 10 Hz = at most one sample every ~100ms. Push 100k times in a tight
	// loop and verify only a handful land — orders of magnitude fewer than
	// the push count. The exact number depends on how long the loop took
	// to run (one extra sample per ~100ms wall-clock), so we bound below
	// (>= 1) and above (<= a generous ceiling that catches the gate being
	// effectively disabled). Avoids tying the test to scheduler precision.
	Profiler::SetSamplingHz(10);

	const auto loopStart = std::chrono::steady_clock::now();
	for (int i = 0; i < 100000; ++i) {
		Profiler::PushValue("Physics", 1.0f);
	}
	const auto loopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - loopStart).count();

	const size_t countAfterBurst = Profiler::Find("Physics")->Count;
	CHECK(countAfterBurst >= 1);            // first push always lands (cold gate)
	CHECK(countAfterBurst <= size_t(loopMs / 100 + 2)); // upper bound: one per ~100ms + slack

	// Sleep past the interval and push exactly one more — that one MUST
	// land regardless of what the burst did.
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	Profiler::PushValue("Physics", 42.0f);
	const ProfilerModule* m = Profiler::Find("Physics");
	CHECK(m->Count == countAfterBurst + 1);
	CHECK(m->CurrentValue == doctest::Approx(42.0f));
}

TEST_CASE("Profiler — PushFrameDelta populates Frame Time") {
	ResetProfiler();

	// 1/60 sec = ~16.6667 ms. PushFrameDelta records the dt in milliseconds
	// into the internal "Frame Time" module. (FPS as a separate module was
	// removed in the four-category restructure — the panel derives an FPS
	// readout from Frame Time at render time.)
	Profiler::PushFrameDelta(1.0f / 60.0f);

	const ProfilerModule* frame = Profiler::Find("Frame Time");
	REQUIRE(frame != nullptr);
	CHECK(frame->CurrentValue == doctest::Approx(16.6667f).epsilon(0.001));
}

TEST_CASE("Profiler — unregistered module names are ignored, not asserted") {
	ResetProfiler();

	// Calling PushValue with an unknown name must not crash, must not
	// silently auto-register (we want the registered-module set to be the
	// stable contract for the panel).
	Profiler::PushValue("NotARealModule", 99.0f);
	CHECK(Profiler::Find("NotARealModule") == nullptr);
}

TEST_CASE("Profiler — SetTrackingSpan reshapes ring buffer and clears state") {
	ResetProfiler();

	for (int i = 0; i < 5; ++i) Profiler::PushValue("Physics", float(i));
	REQUIRE(Profiler::Find("Physics")->Count == 5);

	Profiler::SetTrackingSpan(16);
	const ProfilerModule* m = Profiler::Find("Physics");
	REQUIRE(m != nullptr);
	CHECK(m->Samples.size() == 16);
	CHECK(m->Count == 0); // resize discards prior samples — graph would otherwise show at wrong scale
}

#endif // AXIOM_PROFILER_ENABLED
