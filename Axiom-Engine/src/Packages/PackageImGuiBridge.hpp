#pragma once
#include "Core/Export.hpp"

namespace Axiom {

	// Cross-DLL bridge for sharing the editor's ImGui context with package
	// DLLs. ImGui keeps `static ImGuiContext* GImGui` per-DLL, so a package
	// linking ImGui statically gets its own null context and crashes on the
	// first ImGui call. The fix has two halves:
	//
	//   • Editor side: after `ImGui::CreateContext()`, publish the live
	//     context + allocator function pointers here. Both ImGuiContext*
	//     and the allocator funcs are erased to void* so this header — and
	//     the engine DLL — never need to depend on ImGui.
	//
	//   • Package side: before any ImGui call (typically lazily, on the
	//     first inspector dispatch), call SyncImGui() which casts back and
	//     invokes ImGui::SetCurrentContext + ImGui::SetAllocatorFunctions.
	//
	// We deliberately don't wire this through a new package entry point.
	// Packages may be loaded before the editor's ImGui layer attaches, and
	// some packages don't need ImGui at all. Lazy pull-on-first-use keeps
	// the contract opt-in and order-independent.
	class AXIOM_API PackageImGuiBridge {
	public:
		// Editor-side: publish the active ImGui context. Pointers are
		// reinterpret_cast'd from the editor's ImGuiContext*, ImGuiMemAllocFunc,
		// ImGuiMemFreeFunc — packages cast them back. Bumps an internal
		// generation counter so packages that already synced re-sync on
		// the next call.
		static void Publish(void* imguiContext, void* allocFn, void* freeFn, void* userData);

		// Editor-side: call before ImGui::DestroyContext(). Resets the
		// context pointer to nullptr and bumps the generation, so any
		// re-attached editor session can re-publish cleanly.
		static void Clear();

		// Package-side accessors.
		static void* GetContext();
		static void GetAllocators(void*& outAllocFn, void*& outFreeFn, void*& outUserData);
		static bool HasContext();

		// Generation counter — incremented by Publish and Clear. Packages
		// store the last value they synced and re-sync when they observe
		// a change. Lets the bridge survive ImGui re-init (theoretical
		// today; cheap insurance).
		static unsigned long long GetGeneration();
	};

} // namespace Axiom
