#pragma once

#include "Memory/Arena.hpp"

#include <cstddef>
#include <new>
#include <type_traits>

namespace Index {

	// STL-compatible adapter that routes container allocations through an
	// Arena. Use it when an existing call site already uses std::vector /
	// std::basic_string / friends and you want the same code to bump-
	// allocate without rewriting it:
	//
	//   Arena scratch(64 * 1024);
	//   std::vector<DrawCommand, ArenaAllocator<DrawCommand>> cmds{
	//       ArenaAllocator<DrawCommand>(scratch)
	//   };
	//   cmds.reserve(256);    // single arena allocation, no later realloc
	//   cmds.push_back(...);  // no allocation, no destructor cost
	//
	// Caveats — read these:
	//   * deallocate() is a no-op. A container that grows by re-allocating
	//     (vector::reserve past capacity, string::append past capacity) will
	//     leave the old block parked inside the arena until Reset(). Pre-
	//     reserve to avoid that.
	//   * The element type T must be trivially-destructible. The arena does
	//     not run destructors on Reset, so the element's destructor would
	//     never run if the container itself was arena-allocated and reset
	//     out from under it. Containers themselves (vector etc.) do call
	//     element destructors on clear/destruct, which is fine.
	//   * The adapter does not own the arena; the arena must outlive every
	//     container that holds an instance of this allocator.
	template <class T>
	class ArenaAllocator {
	public:
		using value_type = T;

		// True so that on container copy/move the allocator can come along
		// pointing at the same arena. (The default value of the
		// propagate_* traits is false; we override both because moving an
		// ArenaAllocator without its arena would be a bug.)
		using propagate_on_container_copy_assignment = std::true_type;
		using propagate_on_container_move_assignment = std::true_type;
		using propagate_on_container_swap            = std::true_type;

		// Two allocators are equal iff they point at the same arena.
		// is_always_equal must be false because we *can* be unequal.
		using is_always_equal = std::false_type;

		ArenaAllocator() noexcept = default;

		explicit ArenaAllocator(Arena& arena) noexcept
			: m_Arena(&arena)
		{
		}

		// Rebind ctor: required so containers can ask the allocator for an
		// allocator-of-a-different-type (e.g. vector's internal node type).
		template <class U>
		ArenaAllocator(const ArenaAllocator<U>& other) noexcept
			: m_Arena(other.GetArena())
		{
		}

		T* allocate(std::size_t n) {
			if (n == 0) {
				return nullptr;
			}
			if (!m_Arena) {
				throw std::bad_alloc();
			}
			void* memory = m_Arena->Allocate(sizeof(T) * n, alignof(T));
			if (!memory) {
				// Bump-allocator exhaustion. Containers expect bad_alloc on
				// failure (they don't check for null returns), so propagate
				// it as an exception. Sized arenas should be tuned so this
				// path stays cold.
				throw std::bad_alloc();
			}
			return static_cast<T*>(memory);
		}

		// No-op: the arena reclaims storage only via Reset(). Containers
		// still call deallocate on destruction; that's fine, it just
		// doesn't shrink the arena.
		void deallocate(T* /*p*/, std::size_t /*n*/) noexcept {
		}

		Arena* GetArena() const noexcept { return m_Arena; }

	private:
		Arena* m_Arena = nullptr;
	};

	template <class T, class U>
	bool operator==(const ArenaAllocator<T>& a, const ArenaAllocator<U>& b) noexcept {
		return a.GetArena() == b.GetArena();
	}

	template <class T, class U>
	bool operator!=(const ArenaAllocator<T>& a, const ArenaAllocator<U>& b) noexcept {
		return !(a == b);
	}

} // namespace Index
