#include "pch.hpp"
#include "Graphics/BgfxSpriteResources.hpp"

#include "Core/Log.hpp"
#include "Graphics/Shader.hpp"

#include <cmath>
#include <memory>

// =============================================================================
// BgfxSpriteResources — implementation. See header for the design rationale.
// =============================================================================

namespace Axiom::BgfxSpriteResources {

	namespace {
		struct QuadVertex {
			float X, Y, Z;
		};

		// Unit quad in [-0.5, 0.5]² so the sprite shader can use
		// `a_position.xy * scale + position` and have UV come out as
		// `a_position.xy + 0.5`. Identical to Renderer2D's pre-refactor
		// inline copy.
		constexpr QuadVertex k_QuadVerts[] = {
			{ -0.5f, -0.5f, 0.0f },
			{  0.5f, -0.5f, 0.0f },
			{  0.5f,  0.5f, 0.0f },
			{ -0.5f,  0.5f, 0.0f },
		};
		constexpr uint16_t k_QuadIndices[] = { 0, 1, 2, 0, 2, 3 };

		int                       g_RefCount = 0;
		bool                      g_Ready    = false;
		bgfx::VertexLayout        g_QuadLayout;
		bgfx::VertexLayout        g_InstanceLayout;
		bgfx::VertexBufferHandle  g_QuadVbh   = BGFX_INVALID_HANDLE;
		bgfx::IndexBufferHandle   g_QuadIbh   = BGFX_INVALID_HANDLE;
		bgfx::UniformHandle       g_SamplerAlbedo = BGFX_INVALID_HANDLE;
		std::unique_ptr<Shader>   g_SpriteShader;

		bool BuildResources() {
			g_QuadLayout
				.begin()
				.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
				.end();

			// 3 vec4s per instance; bgfx tags them TEXCOORD7 / 6 / 5 in
			// declaration order. The varying.def maps i_data0 → 7,
			// i_data1 → 6, i_data2 → 5.
			g_InstanceLayout
				.begin()
				.add(bgfx::Attrib::TexCoord7, 4, bgfx::AttribType::Float)
				.add(bgfx::Attrib::TexCoord6, 4, bgfx::AttribType::Float)
				.add(bgfx::Attrib::TexCoord5, 4, bgfx::AttribType::Float)
				.end();

			g_QuadVbh = bgfx::createVertexBuffer(
				bgfx::makeRef(k_QuadVerts, sizeof(k_QuadVerts)), g_QuadLayout);
			g_QuadIbh = bgfx::createIndexBuffer(
				bgfx::makeRef(k_QuadIndices, sizeof(k_QuadIndices)));

			g_SamplerAlbedo = bgfx::createUniform("s_albedo", bgfx::UniformType::Sampler);

			// Same shader path Renderer2D used pre-refactor — Shader_Bgfx
			// strips the legacy ".vs/.fs" naming and resolves to
			// AxiomAssets/Shaders/bgfx/bin/<profile>/{vs,fs}_sprite.bin.
			g_SpriteShader = std::make_unique<Shader>(
				std::string("AxiomAssets/Shaders/SpriteShader.vs"),
				std::string("AxiomAssets/Shaders/SpriteShader.fs"));
			if (!g_SpriteShader || !g_SpriteShader->IsValid()) {
				AIM_CORE_ERROR_TAG("BgfxSpriteResources",
					"Sprite program failed to load — sprite + UI submit paths disabled");
				return false;
			}
			return true;
		}

		void DestroyResources() {
			g_SpriteShader.reset();
			if (bgfx::isValid(g_SamplerAlbedo)) {
				bgfx::destroy(g_SamplerAlbedo);
				g_SamplerAlbedo = BGFX_INVALID_HANDLE;
			}
			if (bgfx::isValid(g_QuadIbh)) {
				bgfx::destroy(g_QuadIbh);
				g_QuadIbh = BGFX_INVALID_HANDLE;
			}
			if (bgfx::isValid(g_QuadVbh)) {
				bgfx::destroy(g_QuadVbh);
				g_QuadVbh = BGFX_INVALID_HANDLE;
			}
		}
	}

	bool Acquire() {
		if (g_RefCount++ == 0) {
			g_Ready = BuildResources();
			if (!g_Ready) {
				DestroyResources();
				--g_RefCount;
			}
		}
		return g_Ready;
	}

	void Release() {
		if (g_RefCount == 0) return;
		if (--g_RefCount == 0) {
			DestroyResources();
			g_Ready = false;
		}
	}

	bool IsReady() {
		return g_Ready;
	}

	bgfx::ProgramHandle GetProgram() {
		if (!g_Ready || !g_SpriteShader) return BGFX_INVALID_HANDLE;
		// Shader_Bgfx encodes its program handle as (idx + 1); decode here
		// so callers can pass the result straight into bgfx::submit.
		const uint16_t raw = static_cast<uint16_t>(g_SpriteShader->GetHandle() - 1u);
		return bgfx::ProgramHandle{ raw };
	}

	bgfx::VertexBufferHandle GetQuadVbh()       { return g_QuadVbh; }
	bgfx::IndexBufferHandle  GetQuadIbh()       { return g_QuadIbh; }
	const bgfx::VertexLayout& GetInstanceLayout() { return g_InstanceLayout; }
	bgfx::UniformHandle      GetSamplerAlbedo() { return g_SamplerAlbedo; }

	void EncodeInstance44(const Instance44& src, SpriteInstance& dst) {
		dst.Pos[0]   = src.Position.x;
		dst.Pos[1]   = src.Position.y;
		dst.Scale[0] = src.Scale.x;
		dst.Scale[1] = src.Scale.y;
		dst.Color[0] = src.Color.r;
		dst.Color[1] = src.Color.g;
		dst.Color[2] = src.Color.b;
		dst.Color[3] = src.Color.a;
		const float c = std::cos(src.Rotation);
		const float s = std::sin(src.Rotation);
		dst.Rot[0] = c;
		dst.Rot[1] = s;
		dst.Rot[2] = 0.0f;
		dst.Rot[3] = 0.0f;
	}

} // namespace Axiom::BgfxSpriteResources
