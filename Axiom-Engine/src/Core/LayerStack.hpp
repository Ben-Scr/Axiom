#pragma once

#include "Core/Export.hpp"
#include "Core/Layer.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace Axiom {

	// Implementation detail of Application — intentionally NOT AXIOM_API.
	// Exporting a class with a `vector<unique_ptr<...>>` member triggers MSVC's
	// well-known dllexport-instantiates-deleted-copy-op error. Consumers reach
	// LayerStack only via Application's public methods, so direct DLL export
	// isn't needed.
	class LayerStack {
	public:
		using Storage = std::vector<std::unique_ptr<Layer>>;
		using iterator = Storage::iterator;
		using const_iterator = Storage::const_iterator;
		using reverse_iterator = Storage::reverse_iterator;
		using const_reverse_iterator = Storage::const_reverse_iterator;

		void PushLayer(std::unique_ptr<Layer> layer) {
			auto insertIt = m_Layers.begin() + static_cast<std::ptrdiff_t>(m_InsertIndex);
			m_Layers.insert(insertIt, std::move(layer));
			++m_InsertIndex;
		}

		void PushOverlay(std::unique_ptr<Layer> overlay) {
			m_Layers.push_back(std::move(overlay));
		}

		// Pop semantics: if a dispatch is currently iterating a snapshot of the
		// stack (BeginDispatch/EndDispatch around it), the layer is *flagged* for
		// removal and the actual erase + destructor runs in EndDispatch. Without
		// a dispatch in flight, removal is immediate.
		//
		// Why: snapshots hand out raw `Layer*`. If a callback's pop destroyed a
		// sibling unique_ptr, later iterations would dispatch to freed memory.
		// Deferred-pop keeps the layer object alive until iteration finishes.
		bool PopLayer(Layer* layer) {
			auto begin = m_Layers.begin();
			auto end = begin + static_cast<std::ptrdiff_t>(m_InsertIndex);
			auto it = std::find_if(begin, end, [layer](const std::unique_ptr<Layer>& entry) {
				return entry.get() == layer;
			});
			if (it == end) {
				return false;
			}

			if (m_DispatchDepth > 0) {
				m_PendingPop.push_back(layer);
				return true;
			}

			m_Layers.erase(it);
			--m_InsertIndex;
			return true;
		}

		bool PopOverlay(Layer* layer) {
			auto begin = m_Layers.begin() + static_cast<std::ptrdiff_t>(m_InsertIndex);
			auto it = std::find_if(begin, m_Layers.end(), [layer](const std::unique_ptr<Layer>& entry) {
				return entry.get() == layer;
			});
			if (it == m_Layers.end()) {
				return false;
			}

			if (m_DispatchDepth > 0) {
				m_PendingPop.push_back(layer);
				return true;
			}

			m_Layers.erase(it);
			return true;
		}

		void Clear() {
			m_Layers.clear();
			m_PendingPop.clear();
			m_InsertIndex = 0;
		}

		bool Empty() const { return m_Layers.empty(); }
		std::size_t Size() const { return m_Layers.size(); }

		// Open a dispatch window. PopLayer/PopOverlay calls inside the window
		// defer their erase until the matching EndDispatch. Re-entrant: nested
		// dispatches share the same pending-pop list and only the outermost
		// EndDispatch flushes.
		void BeginDispatch() {
			++m_DispatchDepth;
		}

		void EndDispatch() {
			if (m_DispatchDepth == 0) {
				return;
			}
			--m_DispatchDepth;
			if (m_DispatchDepth > 0 || m_PendingPop.empty()) {
				return;
			}

			for (Layer* layer : m_PendingPop) {
				auto begin = m_Layers.begin();
				auto insertEnd = begin + static_cast<std::ptrdiff_t>(m_InsertIndex);
				auto it = std::find_if(begin, insertEnd, [layer](const std::unique_ptr<Layer>& entry) {
					return entry.get() == layer;
				});
				if (it != insertEnd) {
					m_Layers.erase(it);
					--m_InsertIndex;
					continue;
				}

				auto overlayBegin = m_Layers.begin() + static_cast<std::ptrdiff_t>(m_InsertIndex);
				auto overlayIt = std::find_if(overlayBegin, m_Layers.end(), [layer](const std::unique_ptr<Layer>& entry) {
					return entry.get() == layer;
				});
				if (overlayIt != m_Layers.end()) {
					m_Layers.erase(overlayIt);
				}
			}
			m_PendingPop.clear();
		}

		// True iff `layer` is staged for removal. Snapshot iteration uses this
		// to skip layers a sibling has already popped during the current
		// dispatch — visiting them would still be safe (they exist) but is
		// surprising and burns work.
		bool IsPendingPop(Layer* layer) const {
			if (m_PendingPop.empty()) {
				return false;
			}
			return std::find(m_PendingPop.begin(), m_PendingPop.end(), layer) != m_PendingPop.end();
		}

		// Index-based access. Returns nullptr on out-of-range. Callers that
		// dispatch into Layer callbacks should iterate with a fresh
		// `Size()` check each step rather than range-for, since user code
		// inside the callback may PushLayer/PopLayer and invalidate
		// iterators (the underlying vector can reallocate).
		Layer* At(std::size_t index) {
			return index < m_Layers.size() ? m_Layers[index].get() : nullptr;
		}
		const Layer* At(std::size_t index) const {
			return index < m_Layers.size() ? m_Layers[index].get() : nullptr;
		}

		iterator begin() { return m_Layers.begin(); }
		iterator end() { return m_Layers.end(); }
		const_iterator begin() const { return m_Layers.begin(); }
		const_iterator end() const { return m_Layers.end(); }
		const_iterator cbegin() const { return m_Layers.cbegin(); }
		const_iterator cend() const { return m_Layers.cend(); }

		reverse_iterator rbegin() { return m_Layers.rbegin(); }
		reverse_iterator rend() { return m_Layers.rend(); }
		const_reverse_iterator rbegin() const { return m_Layers.rbegin(); }
		const_reverse_iterator rend() const { return m_Layers.rend(); }
		const_reverse_iterator crbegin() const { return m_Layers.crbegin(); }
		const_reverse_iterator crend() const { return m_Layers.crend(); }

	private:
		Storage m_Layers;
		std::size_t m_InsertIndex = 0;
		std::size_t m_DispatchDepth = 0;
		std::vector<Layer*> m_PendingPop;
	};

} // namespace Axiom
