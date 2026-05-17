#include <doctest/doctest.h>

#include "Index/Core.hpp"

#include <cstdint>

using namespace Index;

namespace {
	// Re-initialize FrameArenas around each test so capacity is controlled
	// and the Persistent arena is reset between cases. Shutdown on scope
	// exit so a test that asserts state-after-Shutdown still gets a clean
	// teardown.
	struct ScopedFrameArenas {
		explicit ScopedFrameArenas(const FrameArenasSpec& spec) {
			FrameArenas::Initialize(spec);
		}

		~ScopedFrameArenas() {
			FrameArenas::Shutdown();
		}
	};
}

TEST_CASE("FrameArenas Initialize/Shutdown toggles IsInitialized") {
	CHECK(FrameArenas::IsInitialized() == false);

	{
		ScopedFrameArenas guard({ 4096, 1024 });
		CHECK(FrameArenas::IsInitialized() == true);
		CHECK(FrameArenas::Frame().Capacity()      == 4096);
		CHECK(FrameArenas::Persistent().Capacity() == 1024);
	}

	CHECK(FrameArenas::IsInitialized() == false);
	CHECK(FrameArenas::Frame().Capacity()      == 0);
	CHECK(FrameArenas::Persistent().Capacity() == 0);
}

TEST_CASE("FrameArenas before Initialize returns empty arenas (no crash, no UB)") {
	// Default state — no Initialize call. Frame()/Persistent() must still
	// return a valid reference whose Allocate fails cleanly.
	CHECK(FrameArenas::Frame().Allocate(8)      == nullptr);
	CHECK(FrameArenas::Persistent().Allocate(8) == nullptr);
	FrameArenas::OnEndFrame();   // no-op on an uninitialized arena
}

TEST_CASE("FrameArenas Frame() is wiped by OnEndFrame, Persistent() is not") {
	ScopedFrameArenas guard({ 1024, 1024 });

	void* frame = FrameArenas::Frame().Allocate(64);
	void* persistent = FrameArenas::Persistent().Allocate(64);
	REQUIRE(frame      != nullptr);
	REQUIRE(persistent != nullptr);

	CHECK(FrameArenas::Frame().Used()      == 64);
	CHECK(FrameArenas::Persistent().Used() == 64);

	FrameArenas::OnEndFrame();

	CHECK(FrameArenas::Frame().Used()      == 0);
	CHECK(FrameArenas::Persistent().Used() == 64); // untouched
}

TEST_CASE("FrameArenas ResetPersistent rewinds the persistent arena") {
	ScopedFrameArenas guard({ 1024, 1024 });

	(void)FrameArenas::Persistent().Allocate(128);
	CHECK(FrameArenas::Persistent().Used() == 128);

	FrameArenas::ResetPersistent();
	CHECK(FrameArenas::Persistent().Used() == 0);
}

TEST_CASE("FrameArenas Initialize is idempotent and re-sizes on second call") {
	FrameArenas::Initialize({ 256, 256 });
	CHECK(FrameArenas::Frame().Capacity() == 256);

	(void)FrameArenas::Frame().Allocate(128);
	CHECK(FrameArenas::Frame().Used() == 128);

	// Second Initialize replaces the backing buffer with a larger one and
	// resets Used to zero. Any pointers from the previous allocation are
	// invalidated — the contract is documented on Initialize.
	FrameArenas::Initialize({ 4096, 4096 });
	CHECK(FrameArenas::Frame().Capacity() == 4096);
	CHECK(FrameArenas::Frame().Used()     == 0);

	FrameArenas::Shutdown();
}

TEST_CASE("FrameArenas allocations survive across the same frame") {
	ScopedFrameArenas guard({ 4096, 1024 });

	int* a = FrameArenas::Frame().Create<int>(7);
	int* b = FrameArenas::Frame().Create<int>(42);

	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	CHECK(*a == 7);
	CHECK(*b == 42);

	// Mid-frame Mark/Reset still works on the frame arena — the per-frame
	// wipe is in addition to user-driven mark/reset, not a replacement.
	std::size_t mark = FrameArenas::Frame().Mark();
	(void)FrameArenas::Frame().Allocate(64);
	FrameArenas::Frame().Reset(mark);
	CHECK(FrameArenas::Frame().Used() == mark);
}

TEST_CASE("FrameArenas zero-capacity spec produces empty arenas") {
	ScopedFrameArenas guard({ 0, 0 });

	CHECK(FrameArenas::IsInitialized());
	CHECK(FrameArenas::Frame().Capacity()      == 0);
	CHECK(FrameArenas::Persistent().Capacity() == 0);
	CHECK(FrameArenas::Frame().Allocate(1)      == nullptr);
	CHECK(FrameArenas::Persistent().Allocate(1) == nullptr);
}
