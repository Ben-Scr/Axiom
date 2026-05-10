#pragma once

#include <cstdint>

// =============================================================================
// Backend-neutral rendering enums.
// -----------------------------------------------------------------------------
// These types mediate between the engine and a concrete graphics backend
// (OpenGL today, bgfx tomorrow). They MUST NOT include any backend headers
// (glad, bgfx, vulkan, d3d) so callers in renderer / UI / editor code can be
// compiled against the abstract `RenderApi` without leaking GL types into
// their headers. Translation to backend constants happens inside the backend
// .cpp file (see Backend/OpenGLApi.cpp).
// =============================================================================

namespace Axiom {

	// Polygon rasterization mode. The Editor View's "Triangle / Mixed" draw
	// modes flip into Wireframe to render every primitive's edges; default is
	// Filled. Backends without per-face polygon mode (Metal, WebGPU) emulate
	// Wireframe by repacking the index buffer at submission time — that work
	// stays inside the backend, callers just request the mode.
	enum class PolygonMode : uint8_t {
		Filled = 0,
		Wireframe,
	};

	// Cull-face mode. Mirrors GL's GL_NONE / GL_FRONT / GL_BACK / GL_FRONT_AND_BACK
	// without leaking the GLenum. The default for 2D rendering is `None` because
	// quads are submitted in mixed winding orders depending on flip flags.
	enum class CullMode : uint8_t {
		None = 0,
		Back,
		Front,
		FrontAndBack,
	};

	// Predefined blend recipes. Most engine call sites need exactly one of
	// these (alpha-blended sprite quads, premultiplied UI text). A custom
	// `(srcRGB, dstRGB, srcAlpha, dstAlpha)` overload lives on RenderApi for
	// the rare exception. The intent: `RenderApi::SetBlendMode(BlendMode::Alpha)`
	// is the readable, common-case form.
	enum class BlendMode : uint8_t {
		Disabled = 0,
		Alpha,           // src = SRC_ALPHA, dst = ONE_MINUS_SRC_ALPHA
		Premultiplied,   // src = ONE,       dst = ONE_MINUS_SRC_ALPHA
		Additive,        // src = SRC_ALPHA, dst = ONE
		Opaque,          // src = ONE,       dst = ZERO
	};

	// Channels to clear in a `RenderApi::Clear` call. Bitmask so callers can
	// `Color | Depth` in one call. The historical engine-init clear path
	// always cleared both, so `Color | Depth` is the typical default.
	enum class ClearFlags : uint32_t {
		None  = 0,
		Color = 1u << 0,
		Depth = 1u << 1,
		Stencil = 1u << 2,
	};

	constexpr ClearFlags operator|(ClearFlags a, ClearFlags b) noexcept {
		return static_cast<ClearFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}
	constexpr ClearFlags operator&(ClearFlags a, ClearFlags b) noexcept {
		return static_cast<ClearFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
	}
	constexpr bool HasFlag(ClearFlags value, ClearFlags flag) noexcept {
		return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
	}

	// Pixel format for offscreen render targets. The editor's per-viewport
	// FBOs use RGBA8 + Depth24Stencil8 today; the enum exists so a future
	// HDR pipeline (RGBA16F) doesn't have to invent a parallel type.
	enum class TextureFormat : uint8_t {
		RGBA8 = 0,
		R8,
		Depth24Stencil8,
	};

	// Filtering for offscreen-render-target attachments (and the editor's
	// ImGui::Image preview, which samples the same texture). The renderer's
	// own assets use the existing Texture2D class which carries its own
	// Filter/Wrap enums — those don't change.
	enum class TextureFilter : uint8_t {
		Nearest = 0,
		Linear,
	};

} // namespace Axiom
