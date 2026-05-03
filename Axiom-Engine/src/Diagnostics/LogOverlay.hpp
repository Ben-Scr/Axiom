#pragma once

#include "Collections/Ids.hpp"
#include "Core/Log.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

// Forward decl so this header doesn't drag ImGui into the engine's public
// surface. Same constraint as StatsOverlay — the .cpp uses ImGui directly
// and is excluded from the engine DLL build (see Axiom-Engine/premake5.lua).
//
// NOT marked AXIOM_API: each consumer (Editor, Runtime) compiles the .cpp
// into its own binary; there is no DLL boundary.
struct ImVec2;

namespace Axiom::Diagnostics {

	// Engine-level runtime log overlay. Mirrors StatsOverlay's positioning /
	// styling so the two stack cleanly when both are visible.
	//
	// Shows the most-recent N client (and editor-console) log entries in a
	// fixed-size, semi-transparent ImGui window pinned to the top-right.
	// Filters out core engine logs by default — the runtime overlay exists
	// for game-developer-authored logs, not engine spam. Color-codes by
	// severity (trace=gray, info=white, warn=yellow, error/critical=red).
	//
	// Subscribes to Log::OnLog in the constructor, unsubscribes in the
	// destructor. The Log event is invoked from whatever thread called
	// AIM_INFO etc., so the entry buffer is mutex-guarded; main-thread
	// rendering also takes the lock.
	class LogOverlay {
	public:
		// Cap on retained entries — older ones are dropped FIFO when the
		// buffer fills. 256 is enough to scroll back through a couple of
		// seconds of busy logging without unbounded memory growth.
		static constexpr std::size_t k_MaxEntries = 256;

		LogOverlay();
		~LogOverlay();

		LogOverlay(const LogOverlay&) = delete;
		LogOverlay& operator=(const LogOverlay&) = delete;
		LogOverlay(LogOverlay&&) = delete;
		LogOverlay& operator=(LogOverlay&&) = delete;

		// Render entry points mirror StatsOverlay. Both return the height
		// the rendered window actually consumed so callers can stack
		// further overlays below if needed.
		float RenderInRect(const ImVec2& imageMin, const ImVec2& imageMax, float yOffset = 0.0f) const;
		float RenderInMainViewport(float yOffset = 0.0f) const;

		// Drop every retained entry. Called by the inspector "Clear"
		// button if/when one ships; useful from scripts too.
		void ClearEntries();

	private:
		void OnLogEntry(const Log::Entry& entry);
		void RenderBody() const;

		struct StoredEntry {
			std::string Message;
			Log::Level Level = Log::Level::Info;
			Log::Type Source = Log::Type::Client;
		};

		mutable std::mutex m_Mutex;
		std::deque<StoredEntry> m_Entries;
		EventId m_SubscriptionId{};
	};

} // namespace Axiom::Diagnostics
