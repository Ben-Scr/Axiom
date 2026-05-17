#pragma once

#include "Memory/Arena.hpp"

#include <cstddef>

namespace Index {

	// RAII mark/restore for an Arena. On construction, captures the arena's
	// current bump offset; on destruction, rewinds the arena to that offset.
	//
	// Nests cleanly: inner scopes are restored before outer ones, so
	// allocations inside an inner scope cannot survive past it.
	//
	//   Arena& a = FrameArenas::Frame();
	//   {
	//       ScopedArena scope(a);
	//       auto buf = a.CreateArray<Vertex>(1024);  // scratch
	//       Submit(buf);
	//   } // <- arena rewound here, buf is dangling
	//
	// Non-copyable, non-movable: the lifetime model only makes sense as a
	// strict stack scope.
	class ScopedArena {
	public:
		explicit ScopedArena(Arena& arena) noexcept
			: m_Arena(arena)
			, m_Mark(arena.Mark())
		{
		}

		~ScopedArena() {
			m_Arena.Reset(m_Mark);
		}

		ScopedArena(const ScopedArena&) = delete;
		ScopedArena& operator=(const ScopedArena&) = delete;
		ScopedArena(ScopedArena&&) = delete;
		ScopedArena& operator=(ScopedArena&&) = delete;

		// The mark this scope will restore on destruction. Useful for
		// asserting in tests / debug builds that nested arena use behaves.
		std::size_t Mark() const noexcept { return m_Mark; }

	private:
		Arena&      m_Arena;
		std::size_t m_Mark;
	};

} // namespace Index
