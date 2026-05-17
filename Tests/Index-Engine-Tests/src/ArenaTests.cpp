#include <doctest/doctest.h>

#include "Index/Core.hpp"

#include <cstdint>
#include <vector>

using namespace Index;

namespace {
	// Helper: confirm that `ptr` is aligned to `alignment`.
	bool IsAligned(const void* ptr, std::size_t alignment) {
		return (reinterpret_cast<std::uintptr_t>(ptr) % alignment) == 0;
	}
}

TEST_CASE("Arena default-constructs empty and returns nullptr from Allocate") {
	Arena arena;

	CHECK(arena.Capacity()  == 0);
	CHECK(arena.Used()      == 0);
	CHECK(arena.Remaining() == 0);
	CHECK(arena.Allocate(16) == nullptr);
}

TEST_CASE("Arena allocates aligned bytes within capacity") {
	Arena arena(1024);

	void* a = arena.Allocate(8, 8);
	void* b = arena.Allocate(8, 8);

	CHECK(a != nullptr);
	CHECK(b != nullptr);
	CHECK(a != b);
	CHECK(IsAligned(a, 8));
	CHECK(IsAligned(b, 8));
	CHECK(arena.Used() == 16);
}

TEST_CASE("Arena honors arbitrary power-of-two alignment requests") {
	Arena arena(1024);

	// Burn a single byte so the next request actually has to align up.
	void* prefix = arena.Allocate(1, 1);
	CHECK(prefix != nullptr);

	for (std::size_t alignment : { std::size_t{ 2 }, std::size_t{ 16 }, std::size_t{ 64 }, std::size_t{ 128 } }) {
		void* p = arena.Allocate(4, alignment);
		CHECK(p != nullptr);
		CHECK(IsAligned(p, alignment));
	}
}

TEST_CASE("Arena returns nullptr when exhausted without growing") {
	Arena arena(64);

	void* a = arena.Allocate(48);
	CHECK(a != nullptr);

	// 48 used, 64 capacity — a 32B request must not fit, even though the
	// arena is far from empty. Non-growing is the contract.
	void* b = arena.Allocate(32);
	CHECK(b == nullptr);
	CHECK(arena.Used() == 48);
}

TEST_CASE("Arena Mark and Reset round-trip") {
	Arena arena(256);

	std::size_t before = arena.Mark();
	CHECK(before == 0);

	(void)arena.Allocate(32);
	std::size_t mid = arena.Mark();
	CHECK(mid == 32);

	(void)arena.Allocate(64);
	CHECK(arena.Used() == 96);

	arena.Reset(mid);
	CHECK(arena.Used() == mid);

	// Storage past `mid` is now reusable.
	void* reused = arena.Allocate(64);
	CHECK(reused != nullptr);
	CHECK(arena.Used() == mid + 64);

	arena.Reset();
	CHECK(arena.Used() == 0);
}

TEST_CASE("Arena::Create constructs trivially-destructible types") {
	struct Pod {
		int   x;
		float y;
	};
	static_assert(std::is_trivially_destructible_v<Pod>);

	Arena arena(256);
	Pod* p = arena.Create<Pod>(Pod{ 7, 2.5f });

	REQUIRE(p != nullptr);
	CHECK(p->x == 7);
	CHECK(p->y == doctest::Approx(2.5f));
	CHECK(IsAligned(p, alignof(Pod)));
}

TEST_CASE("Arena::CreateArray returns an empty span when exhausted") {
	Arena arena(16);

	auto first = arena.CreateArray<int>(2);   // 8 bytes
	CHECK(first.size() == 2);

	auto second = arena.CreateArray<int>(64); // would need 256 bytes
	CHECK(second.empty());
}

TEST_CASE("Arena is move-constructible and move-assignable") {
	Arena a(128);
	void* p = a.Allocate(32);
	CHECK(p != nullptr);
	CHECK(a.Used() == 32);

	Arena b(std::move(a));
	CHECK(b.Capacity() == 128);
	CHECK(b.Used()     == 32);
	CHECK(a.Capacity() == 0);
	CHECK(a.Used()     == 0);

	Arena c;
	c = std::move(b);
	CHECK(c.Capacity() == 128);
	CHECK(c.Used()     == 32);
	CHECK(b.Capacity() == 0);
}

TEST_CASE("ScopedArena restores the arena offset on scope exit") {
	Arena arena(256);
	(void)arena.Allocate(16);
	std::size_t outerMark = arena.Used();

	{
		ScopedArena scope(arena);
		(void)arena.CreateArray<int>(8);
		CHECK(arena.Used() > outerMark);
	}

	CHECK(arena.Used() == outerMark);
}

TEST_CASE("ScopedArena nests correctly") {
	Arena arena(512);
	(void)arena.Allocate(8);
	std::size_t a = arena.Used();

	{
		ScopedArena outer(arena);
		(void)arena.Allocate(16);
		std::size_t b = arena.Used();
		CHECK(b > a);

		{
			ScopedArena inner(arena);
			(void)arena.Allocate(32);
			CHECK(arena.Used() > b);
		}

		// Inner restored, but outer is still in effect.
		CHECK(arena.Used() == b);
	}

	CHECK(arena.Used() == a);
}

TEST_CASE("ArenaAllocator drives std::vector with a pre-reserved arena") {
	Arena arena(4096);
	using AllocT = ArenaAllocator<int>;
	std::vector<int, AllocT> v(AllocT{ arena });
	v.reserve(64);   // single arena allocation

	for (int i = 0; i < 64; i++) {
		v.push_back(i);
	}

	REQUIRE(v.size() == 64);
	for (int i = 0; i < 64; i++) {
		CHECK(v[i] == i);
	}
}

TEST_CASE("ArenaAllocator equality reflects shared arena identity") {
	Arena arenaA(256);
	Arena arenaB(256);

	ArenaAllocator<int> a1(arenaA);
	ArenaAllocator<int> a2(arenaA);
	ArenaAllocator<int> b1(arenaB);

	CHECK(a1 == a2);
	CHECK(a1 != b1);
}
