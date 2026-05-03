#pragma once

namespace Axiom {
	class Window;
}

namespace Axiom {

	// Process-wide ImGui context owner for the runtime exe. Layers that want
	// to draw ImGui (RuntimeStatsLayer, RuntimeProfilerLayer, future debug
	// overlays) call Acquire() in OnAttach and Release() in OnDetach.
	//
	// First Acquire creates the ImGui context + GLFW/OpenGL backends and
	// raises the refcount; subsequent Acquires just bump the count. Release
	// decrements; the last Release tears the backends + context down. This
	// keeps the runtime free of "two layers fighting over ImGui::CreateContext"
	// and lets layer push order be arbitrary.
	//
	// All methods are main-thread only. Layer attach/detach already runs on
	// the main thread via Application's layer stack.
	class RuntimeImGuiHost {
	public:
		// Returns true if the call succeeded (window/GL handles were valid
		// and either we already had a context or we set one up). False on
		// any setup failure — the caller should bail out cleanly.
		static bool Acquire(Window* window);

		// Decrement refcount; tears down on the last release. Pass the same
		// window for symmetry — currently unused but reserves the slot in
		// case a future tear-down step needs it.
		static void Release();

		// True if a previous Acquire actually completed setup. Layers can
		// gate per-frame work (NewFrame / Render) on this so a failed
		// Acquire doesn't render half-initialised state.
		static bool IsInitialized();

		// Per-frame begin / end. Idempotent against multiple layers; the
		// host tracks whether the current frame has already opened its
		// NewFrame and skips duplicate calls. Each layer should call
		// BeginFrame() in its OnPreRender and EndFrame() in OnPostRender —
		// the host's reference counting ensures only the first BeginFrame
		// of the frame actually opens it and the last EndFrame closes it.
		static void BeginFrame();
		static void EndFrame();
	};

} // namespace Axiom
