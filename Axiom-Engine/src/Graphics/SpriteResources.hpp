#pragma once

#include "Core/Export.hpp"
#include "Graphics/Instance44.hpp"

#include <webgpu/webgpu_cpp.h>

#include <cstdint>

// =============================================================================
// WebGPUSpriteResources — shared WebGPU state for the sprite + UI submit paths.
// -----------------------------------------------------------------------------
// Sibling to BgfxSpriteResources (which is the --rhi=bgfx implementation).
// Both Renderer2D (world-space sprites) and GuiRenderer (screen-space UI quads)
// pull their core GPU state from here so engine.dll owns one copy of:
//   * The sprite WGSL ShaderModule (vs_main / fs_main entry points)
//   * The unit-quad vertex + index buffers (4 verts, 6 indices)
//   * The BindGroupLayout + PipelineLayout for { u_ViewProj, t_albedo, s_albedo }
//   * A per-color-format wgpu::RenderPipeline cache (one pipeline per swap-chain
//     format / FBO format the engine actually renders to in a session, lazily
//     built on first request)
//
// Lifecycle is reference-counted, matching BgfxSpriteResources: Acquire() in
// each renderer's Initialize, Release() in each renderer's Shutdown. The
// counter exists so engine.dll holds the resources exactly once even when
// multiple renderers init independently in different orders.
//
// Why this isn't in WebGPUBackend.hpp: that header is for cross-cutting
// resource-pool access (texture lookup, FBO lookup) used by many TUs. The
// sprite pipeline is specific to the sprite/UI submit path, so its scope
// matches Renderer2D + GuiRenderer rather than every WebGPU consumer.
// =============================================================================

namespace Axiom::WebGPUSpriteResources {

	// Per-instance layout written into the instance VBO. Identical CPU layout
	// to BgfxSpriteResources::SpriteInstance — pack/encode is the same math.
	// Kept duplicate (not shared via a backend-neutral header) because each
	// backend's emitter may diverge slightly: WebGPU's vertex layout has
	// different padding requirements than bgfx in future revisions, so a
	// shared struct would force cross-backend lockstep on changes.
	struct AXIOM_API SpriteInstance {
		float Pos[2];        // matches i_data0.xy in vs_main
		float Scale[2];      // matches i_data0.zw
		float Color[4];      // matches i_data1
		float Rot[4];        // matches i_data2 = (cos, sin, 0, 0)
	};

	// Reference-counted lifecycle. Returns false on shader-load failure —
	// callers can keep going without the sprite path (renderer falls back
	// to issuing no draws this frame; the engine doesn't crash). IsReady()
	// is the runtime "should I emit sprite draws this frame" check.
	AXIOM_API bool Acquire();
	AXIOM_API void Release();
	AXIOM_API bool IsReady();

	// Submit-side accessors. All valid only between Acquire / Release.

	AXIOM_API wgpu::ShaderModule GetSpriteModule();

	// Quad geometry — 4 vertices at [-0.5, 0.5]² (Z=0), 6 indices forming
	// two CCW triangles. Vertex stride = 12 bytes (3 floats).
	AXIOM_API wgpu::Buffer GetQuadVertexBuffer();
	AXIOM_API wgpu::Buffer GetQuadIndexBuffer();

	// Bind group / pipeline layout.
	//   group 0:
	//     binding 0: uniform buffer (mat4 viewProj)
	//     binding 1: 2D sampled texture
	//     binding 2: sampler
	AXIOM_API wgpu::BindGroupLayout GetBindGroupLayout();
	AXIOM_API wgpu::PipelineLayout  GetPipelineLayout();

	// Per-target-format render pipeline. Cached internally: a session
	// typically only renders to ~2 formats (swap-chain BGRA8Unorm or
	// BGRA8UnormSrgb, plus FBO RGBA8Unorm for the editor's panels), so
	// the cache stays small. `hasDepth` selects between the
	// no-depth-attachment variant (swap-chain Stage 1) and the
	// D24S8-depth variant (every editor FBO).
	AXIOM_API wgpu::RenderPipeline GetSpritePipeline(
		wgpu::TextureFormat colorFormat,
		bool                hasDepth);

	// CPU-side encode helper — same math as the bgfx side. Out-parameter
	// so callers can write directly into a transient instance buffer's
	// mapped range with a single memcpy-free assignment.
	AXIOM_API void EncodeInstance44(const Instance44& src, SpriteInstance& dst);

}  // namespace Axiom::WebGPUSpriteResources
