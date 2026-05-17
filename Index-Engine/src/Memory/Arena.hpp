#pragma once

#include "Core/Export.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace Index {

	// Linear (bump) allocator. Allocations are O(1) pointer-bumps; there is
	// no per-object free. The whole arena is rewound in O(1) via Reset() or
	// to a saved Mark().
	//
	// Non-growing by design: when the arena runs out, Allocate() returns
	// nullptr. Callers in hot paths must not pay for a hidden re-allocation,
	// so the caller picks the capacity up front. If you genuinely need
	// growth, hold an Arena per use-site and reconstruct it with a larger
	// capacity, or use the general Allocator instead.
	//
	// Not thread-safe. The engine pairs one arena with one owning thread
	// via FrameArenas (added in a later step); user code that wants a
	// shared arena must serialize externally.
	class INDEX_CORE_API Arena {
	public:
		// Empty arena. Allocate() always returns nullptr until ownership is
		// moved in from a constructed arena. Useful as a default-initialized
		// member that's filled in later.
		Arena() noexcept = default;

		// Allocate a fixed backing buffer of `capacity` bytes. `capacity == 0`
		// is valid and produces an empty arena (every Allocate returns nullptr).
		explicit Arena(std::size_t capacity);

		~Arena();

		Arena(const Arena&) = delete;
		Arena& operator=(const Arena&) = delete;

		Arena(Arena&& other) noexcept;
		Arena& operator=(Arena&& other) noexcept;

		// Raw aligned allocation. Returns nullptr when the arena cannot fit
		// the request (or when `size == 0`). Alignment must be a power of two.
		void* Allocate(std::size_t size,
			std::size_t alignment = alignof(std::max_align_t));

		// Construct a single T in place. Only trivially-destructible types
		// are allowed: the arena never runs destructors on Reset, so a type
		// with a non-trivial destructor would silently leak its post-
		// construction resources every frame. If T's destructor matters,
		// either hold an Index::Ref<T> outside the arena, or destruct
		// manually before resetting.
		//
		// Returns nullptr if the arena is exhausted.
		template <class T, class... Args>
		T* Create(Args&&... args) {
			static_assert(std::is_trivially_destructible_v<T>,
				"Arena::Create<T> requires a trivially-destructible T. "
				"Non-trivial destructors will not run on Reset.");

			void* memory = Allocate(sizeof(T), alignof(T));
			if (!memory) {
				return nullptr;
			}
			return new (memory) T(std::forward<Args>(args)...);
		}

		// Uninitialized typed array. The returned span's storage is
		// uninitialized (placement-new not invoked); callers fill it. T is
		// restricted to trivially-destructible for the same reason as
		// Create<T>.
		//
		// Returns an empty span if the arena is exhausted.
		template <class T>
		std::span<T> CreateArray(std::size_t count) {
			static_assert(std::is_trivially_destructible_v<T>,
				"Arena::CreateArray<T> requires a trivially-destructible T.");

			if (count == 0) {
				return {};
			}
			void* memory = Allocate(sizeof(T) * count, alignof(T));
			if (!memory) {
				return {};
			}
			return std::span<T>(static_cast<T*>(memory), count);
		}

		// Returns the current bump offset. Pair with Reset(mark) to rewind
		// to this point — the classic scratch-and-restore pattern. See also
		// ScopedArena for an RAII wrapper.
		std::size_t Mark() const noexcept { return m_Offset; }

		// Rewind to `mark` (0 = full rewind). Bytes past `mark` become
		// available for reuse. No destructors run; that's the trade.
		void Reset(std::size_t mark = 0) noexcept;

		std::size_t Capacity()  const noexcept { return m_Capacity; }
		std::size_t Used()      const noexcept { return m_Offset; }
		std::size_t Remaining() const noexcept { return m_Capacity - m_Offset; }

		// Backing buffer base. Mostly for diagnostics / debugger inspection;
		// don't poke at the bytes directly in normal code.
		const std::byte* Data() const noexcept { return m_Base; }

	private:
		std::byte*  m_Base     = nullptr;
		std::size_t m_Capacity = 0;
		std::size_t m_Offset   = 0;
	};

} // namespace Index
