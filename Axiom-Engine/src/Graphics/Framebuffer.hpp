#pragma once

#include "Collections/Viewport.hpp"
#include "Core/Export.hpp"
#include "Graphics/RenderApiTypes.hpp"

#include <cstdint>

// =============================================================================
// Framebuffer — RAII wrapper for an offscreen render target.
// -----------------------------------------------------------------------------
// Used by the editor's per-panel viewports (Editor View + Game View) to
// render into a texture that ImGui then samples for its docked panels.
// Created with (width, height, format), can be resized in place via
// Recreate, and cleans up its backend handles on destruction. Under
// WebGPU a colour wgpu::Texture + depth/stencil wgpu::Texture are
// created with matching attachments.
//
// `GetColorTextureBackendId()` exposes the raw backend handle for the
// colour attachment so ImGui::Image can sample it. This is the one place
// backend integers leak through the abstraction — required by ImGui's
// API contract.
// =============================================================================

namespace Axiom {

	class AXIOM_API Framebuffer {
	public:
		Framebuffer() = default;
		Framebuffer(int width, int height,
			TextureFormat colorFormat = TextureFormat::RGBA8,
			TextureFilter filter = TextureFilter::Linear);

		// Move-only — backend handles are unique resources; copying would
		// double-free on destruction.
		Framebuffer(const Framebuffer&) = delete;
		Framebuffer& operator=(const Framebuffer&) = delete;
		Framebuffer(Framebuffer&& other) noexcept;
		Framebuffer& operator=(Framebuffer&& other) noexcept;

		~Framebuffer();

		// Tear down + recreate at a new size. No-op when the size matches
		// the existing attachments (the editor calls this every frame
		// against the panel's content region, so the cheap-path matters).
		// Returns true on success.
		bool Recreate(int width, int height,
			TextureFormat colorFormat = TextureFormat::RGBA8,
			TextureFilter filter = TextureFilter::Linear);

		// Release backend handles. Idempotent — safe to call on an
		// already-empty framebuffer.
		void Destroy();

		bool IsValid() const { return m_BackendId != 0; }

		// Backend identifier for the framebuffer object itself. Required by
		// `RenderApi::BindFramebuffer`; callers shouldn't interpret the
		// integer otherwise.
		uint32_t GetBackendId() const { return m_BackendId; }

		// Backend identifier for the colour attachment. The editor passes
		// this into `ImGui::Image` so the panel samples this FBO's texture.
		// Returns a 64-bit value because under the WebGPU backend it's the
		// raw WGPUTextureView pointer (must fit 8 bytes on x64), while
		// under earlier backends it stored a small integer handle. ImGui's
		// ImTextureID is `ImU64` in current Dear ImGui, so a uint64_t fits
		// the consumer's cast chain naturally.
		uint64_t GetColorTextureBackendId() const { return m_ColorTextureId; }

		int GetWidth() const { return m_Viewport.GetWidth(); }
		int GetHeight() const { return m_Viewport.GetHeight(); }
		const Viewport& GetViewport() const { return m_Viewport; }

	private:
		// Backend handles. 0 = unset. Under WebGPU `m_ColorTextureId`
		// stores the raw WGPUTextureView pointer cast to uint64_t so it
		// can be passed straight through to ImGui's imgui_impl_wgpu as a
		// real WGPUTextureView (imgui_impl_wgpu reinterpret_casts the
		// supplied ImTextureID to WGPUTextureView and dereferences it).
		// `m_BackendId` stays a 32-bit pool key indexing into
		// Framebuffer_WebGPU.cpp's GPU resource map.
		uint32_t m_BackendId       = 0;
		uint64_t m_ColorTextureId  = 0;
		uint32_t m_DepthRenderbuffer = 0;

		Viewport m_Viewport{ 0, 0 };
	};

} // namespace Axiom
