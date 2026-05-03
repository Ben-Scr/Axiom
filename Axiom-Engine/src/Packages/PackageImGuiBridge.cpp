#include "pch.hpp"
#include "Packages/PackageImGuiBridge.hpp"

#include <atomic>

namespace Axiom {

	namespace {
		std::atomic<void*> s_ImGuiContext{ nullptr };
		std::atomic<void*> s_AllocFn{ nullptr };
		std::atomic<void*> s_FreeFn{ nullptr };
		std::atomic<void*> s_UserData{ nullptr };
		std::atomic<unsigned long long> s_Generation{ 0 };
	}

	void PackageImGuiBridge::Publish(void* imguiContext, void* allocFn, void* freeFn, void* userData) {
		s_AllocFn.store(allocFn, std::memory_order_release);
		s_FreeFn.store(freeFn, std::memory_order_release);
		s_UserData.store(userData, std::memory_order_release);
		// Context store last so a package observing non-null context
		// reads coherent allocator pointers.
		s_ImGuiContext.store(imguiContext, std::memory_order_release);
		s_Generation.fetch_add(1, std::memory_order_acq_rel);
	}

	void PackageImGuiBridge::Clear() {
		s_ImGuiContext.store(nullptr, std::memory_order_release);
		s_AllocFn.store(nullptr, std::memory_order_release);
		s_FreeFn.store(nullptr, std::memory_order_release);
		s_UserData.store(nullptr, std::memory_order_release);
		s_Generation.fetch_add(1, std::memory_order_acq_rel);
	}

	void* PackageImGuiBridge::GetContext() {
		return s_ImGuiContext.load(std::memory_order_acquire);
	}

	void PackageImGuiBridge::GetAllocators(void*& outAllocFn, void*& outFreeFn, void*& outUserData) {
		outAllocFn = s_AllocFn.load(std::memory_order_acquire);
		outFreeFn = s_FreeFn.load(std::memory_order_acquire);
		outUserData = s_UserData.load(std::memory_order_acquire);
	}

	bool PackageImGuiBridge::HasContext() {
		return s_ImGuiContext.load(std::memory_order_acquire) != nullptr;
	}

	unsigned long long PackageImGuiBridge::GetGeneration() {
		return s_Generation.load(std::memory_order_acquire);
	}

} // namespace Axiom
