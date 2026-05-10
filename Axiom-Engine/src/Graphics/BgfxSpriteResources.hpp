#pragma once

#include "Core/Export.hpp"
#include "Graphics/Instance44.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <span>

// =============================================================================
// BgfxSpriteResources — shared bgfx state for the sprite + UI submit paths.
// -----------------------------------------------------------------------------
// Both Renderer2D_Bgfx (world-space sprites) and GuiRenderer_Bgfx (screen-space
// UI quads) submit through the same vertex layout (a unit quad in [-0.5, 0.5]²),
// the same instance layout (3 vec4s — Pos+Scale, Color, cos+sin), and the same
// sprite program (vs_sprite + fs_sprite). Owning those once and refcounting
// the init/shutdown calls lets each renderer pull them in independently
// without leaking duplicate VBOs / programs into engine.dll.
//
// Lives in engine.dll alongside the bgfx::init call (BgfxApi.cpp). Acquire is
// idempotent — first caller creates resources, every subsequent caller bumps
// the refcount. Release is the inverse: last decrement destroys the GPU
// buffers + program.
// =============================================================================

namespace Axiom::BgfxSpriteResources {

	// Instance VBO row matching vs_sprite's i_data0 / i_data1 / i_data2.
	// CPU encoders pack a host-side Instance44 into this layout via
	// EncodeInstance44 below — keeps callers from open-coding the rotation
	// → (cos, sin) translation each at their own emit site.
	struct AXIOM_API SpriteInstance {
		float Pos[2];        // i_data0.xy
		float Scale[2];      // i_data0.zw
		float Color[4];      // i_data1
		float Rot[4];        // i_data2 = (cos, sin, 0, 0) — pad to a vec4
	};

	// Reference-counted lifecycle. Call Acquire once before the first submit
	// (typically in Renderer2D::Initialize / GuiRenderer::Initialize), and
	// Release in their Shutdown. Returns false if shader load failed —
	// caller can keep going without the sprite path (the frame just won't
	// render sprites/UI quads).
	AXIOM_API bool Acquire();
	AXIOM_API void Release();
	AXIOM_API bool IsReady();

	// Submit-side accessors. All four are valid only between Acquire / Release.
	AXIOM_API bgfx::ProgramHandle      GetProgram();
	AXIOM_API bgfx::VertexBufferHandle GetQuadVbh();
	AXIOM_API bgfx::IndexBufferHandle  GetQuadIbh();
	AXIOM_API const bgfx::VertexLayout& GetInstanceLayout();
	AXIOM_API bgfx::UniformHandle      GetSamplerAlbedo();

	// Translate a host-side Instance44 (Position, Scale, Rotation, Color)
	// into the GPU instance row. Out-parameter form so the caller can write
	// directly into a transient instance buffer's `data` pointer with a
	// single memcpy-friendly write.
	AXIOM_API void EncodeInstance44(const Instance44& src, SpriteInstance& dst);

} // namespace Axiom::BgfxSpriteResources
