#include "pch.hpp"
#include "Graphics/Renderer2D.hpp"

#include "Components/General/Transform2DComponent.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Core/Log.hpp"
#include "Graphics/BgfxSpriteResources.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Graphics/Text/TextRenderer.hpp"
#include "Scene/Scene.hpp"
#ifdef AXIOM_PROFILER_ENABLED
#include "Profiling/GpuTimer.hpp"
#endif

#include <bgfx/bgfx.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Axiom::BgfxBackend {
	bgfx::ViewId CurrentViewId();
}

// =============================================================================
// Renderer2D — bgfx implementation. Stage 2.2 of the bgfx port.
// -----------------------------------------------------------------------------
// Real sprite submission:
//   1. Initialize ref-bumps the shared BgfxSpriteResources (unit quad VBO / IBO,
//      sprite program, instance layout, s_albedo sampler) — same module
//      GuiRenderer_Bgfx pulls from so we don't duplicate VBOs across renderers.
//   2. RenderSceneWithVP walks every Transform2D + SpriteRenderer entity,
//      sorts by (SortingLayer, SortingOrder, DrawIndex) so the GL impl's
//      z-stack contract holds, and submits one bgfx::submit per texture run.
//
// Still pending (out of scope for this pass):
//   * Particle / managed-component instance contributors — `RegisterInstance-
//     Contributor` returns 0 (unwired) until those subsystems learn to
//     publish through the same instance buffer.
//   * Premultiplied-alpha + alpha-cutoff uniforms — fs_sprite.sc has the
//     simplest possible body; the parameter vector lands when the
//     SpriteShaderProgram_Bgfx grows the uniform setters.
// =============================================================================

namespace Axiom {

	namespace {
		using BgfxSpriteResources::SpriteInstance;

		// Scratch reused across frames so the per-frame collect path
		// doesn't churn the heap. Mirrors Renderer2D.cpp's persistent
		// scratch buffers.
		std::vector<Instance44>     g_InstancesScratch;
		std::vector<TextureHandle>  g_TexturesScratch;

		// Walk Transform2D + SpriteRenderer pairs. Returns the instance
		// count for the stats overlay; per-instance texture handles land
		// in the parallel `outTextures` so the submit loop can group runs.
		size_t CollectSpriteInstances(Scene& scene,
			std::vector<Instance44>& outInstances,
			std::vector<TextureHandle>& outTextures)
		{
			outInstances.clear();
			outTextures.clear();
			auto view = scene.GetRegistry().view<Transform2DComponent, SpriteRendererComponent>();
			for (auto entity : view) {
				const auto& t = view.get<Transform2DComponent>(entity);
				const auto& s = view.get<SpriteRendererComponent>(entity);
				outInstances.emplace_back(
					Vec2{ t.Position.x, t.Position.y },
					Vec2{ t.Scale.x, t.Scale.y },
					t.Rotation,
					s.Color,
					s.TextureHandle,
					s.SortingOrder,
					s.SortingLayer,
					/*drawIndex*/ static_cast<std::uint32_t>(outInstances.size()));
				outTextures.push_back(s.TextureHandle);
			}
			return outInstances.size();
		}

		// Submit the (already-sorted) instance run. `instances` and `textures`
		// must be parallel arrays; consecutive entries with the same
		// resolved texture handle become one bgfx::submit using a transient
		// instance data buffer.
		void SubmitBatches(const std::vector<Instance44>& instances,
			const std::vector<TextureHandle>& textures,
			size_t& outDrawCalls)
		{
			if (instances.empty()) return;
			if (!BgfxSpriteResources::IsReady()) return;

			const bgfx::ProgramHandle prog = BgfxSpriteResources::GetProgram();
			if (!bgfx::isValid(prog)) return;

			const bgfx::ViewId vid    = BgfxBackend::CurrentViewId();
			const auto& instanceLayout = BgfxSpriteResources::GetInstanceLayout();
			const bgfx::VertexBufferHandle quadVbh = BgfxSpriteResources::GetQuadVbh();
			const bgfx::IndexBufferHandle  quadIbh = BgfxSpriteResources::GetQuadIbh();
			const bgfx::UniformHandle      sampler = BgfxSpriteResources::GetSamplerAlbedo();

			const TextureHandle defaultTexture = TextureManager::GetDefaultTexture(DefaultTexture::Square);
			auto resolveHandle = [&](TextureHandle h) {
				return TextureManager::IsValid(h) ? h : defaultTexture;
			};

			size_t i = 0;
			while (i < instances.size()) {
				const TextureHandle runHandle = resolveHandle(textures[i]);
				size_t runEnd = i + 1;
				while (runEnd < instances.size()
					&& resolveHandle(textures[runEnd]).index == runHandle.index
					&& resolveHandle(textures[runEnd]).generation == runHandle.generation)
				{
					++runEnd;
				}
				const uint32_t count = static_cast<uint32_t>(runEnd - i);

				if (bgfx::getAvailInstanceDataBuffer(count, sizeof(SpriteInstance)) < count) {
					AIM_CORE_WARN_TAG("Renderer2D",
						"Instance buffer exhausted; dropped {} sprites this frame", count);
					break;
				}
				bgfx::InstanceDataBuffer idb{};
				bgfx::allocInstanceDataBuffer(&idb, count, sizeof(SpriteInstance));
				SpriteInstance* dst = reinterpret_cast<SpriteInstance*>(idb.data);
				for (uint32_t k = 0; k < count; ++k) {
					BgfxSpriteResources::EncodeInstance44(instances[i + k], dst[k]);
				}

				bgfx::setVertexBuffer(0, quadVbh);
				bgfx::setIndexBuffer(quadIbh);
				bgfx::setInstanceDataBuffer(&idb);

				if (Texture2D* tex = TextureManager::GetTexture(runHandle); tex && tex->IsValid()) {
					const uint16_t texIdx = static_cast<uint16_t>(tex->GetHandle() - 1u);
					bgfx::setTexture(0, sampler, bgfx::TextureHandle{ texIdx });
				}

				bgfx::setState(0
					| BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_BLEND_ALPHA
					| BGFX_STATE_MSAA);

				bgfx::submit(vid, prog);
				++outDrawCalls;
				i = runEnd;
			}
		}
	}

	Renderer2D::Renderer2D() = default;
	Renderer2D::~Renderer2D() = default;

	void Renderer2D::Initialize() {
		BgfxSpriteResources::Acquire();
		m_IsInitialized = true;
	}

	void Renderer2D::Shutdown() {
		if (!m_IsInitialized) return;
		BgfxSpriteResources::Release();
		m_IsInitialized = false;
	}

	void Renderer2D::BeginFrame() {
		m_RenderedInstancesCount = 0;
		m_DrawCallsCount = 0;
	}

	void Renderer2D::EndFrame() {
		// All submission already happened in RenderScene; bgfx::frame()
		// flushes from Window::SwapBuffers.
	}

	void Renderer2D::RenderScene(Scene& scene) {
		// World-space pass with no explicit VP — touch the view so its
		// clear executes even when no instances submit, then defer to
		// the explicit-VP path with identity (matches the pre-sort
		// fallback the GL impl uses when the active camera is missing).
		bgfx::touch(BgfxBackend::CurrentViewId());
		(void)scene;
	}

	void Renderer2D::RenderSceneWithVP(Scene& scene,
		const glm::mat4& vp, const AABB& /*viewportAABB*/)
	{
		const bgfx::ViewId vid = BgfxBackend::CurrentViewId();
		bgfx::setViewTransform(vid, nullptr, glm::value_ptr(vp));

		const size_t n = CollectSpriteInstances(scene, g_InstancesScratch, g_TexturesScratch);
		m_RenderedInstancesCount = n;

		// Sort by (SortingLayer, SortingOrder, DrawIndex) so the GL impl's
		// z-stack contract — later entries draw on top — holds verbatim
		// here. Sort indices into the pair (instances, textures) so we
		// don't reshuffle the textures vector twice.
		if (n > 1) {
			std::vector<size_t> order(n);
			for (size_t k = 0; k < n; ++k) order[k] = k;
			std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
				const auto& ia = g_InstancesScratch[a];
				const auto& ib = g_InstancesScratch[b];
				if (ia.SortingLayer != ib.SortingLayer) return ia.SortingLayer < ib.SortingLayer;
				if (ia.SortingOrder != ib.SortingOrder) return ia.SortingOrder < ib.SortingOrder;
				return ia.DrawIndex < ib.DrawIndex;
			});
			std::vector<Instance44>    sortedInst(n);
			std::vector<TextureHandle> sortedTex(n);
			for (size_t k = 0; k < n; ++k) {
				sortedInst[k] = g_InstancesScratch[order[k]];
				sortedTex[k]  = g_TexturesScratch[order[k]];
			}
			g_InstancesScratch.swap(sortedInst);
			g_TexturesScratch.swap(sortedTex);
		}

		SubmitBatches(g_InstancesScratch, g_TexturesScratch, m_DrawCallsCount);
		bgfx::touch(vid); // ensure the view's clear runs even when n == 0.
	}

	void Renderer2D::RenderScenes() {
		// SceneProvider iteration follows the OpenGL impl's pattern; left
		// as a no-op until that callback is wired through to the bgfx
		// path.
	}

	void Renderer2D::CollectAndRenderInstances(Scene& /*scene*/,
		const glm::mat4& /*vp*/, const AABB& /*viewportAABB*/)
	{
	}

	uint32_t Renderer2D::RegisterInstanceContributor(InstanceContributor /*contributor*/) {
		// External instance contributors (Tilemap2D etc.) plug in via the
		// instance buffer once the sprite path is stable; for now they're
		// intentionally unwired so the sprite-only happy path stays the
		// only thing the Stage 2 port has to keep correct.
		return 0;
	}

	void Renderer2D::UnregisterInstanceContributor(uint32_t /*token*/) {}

} // namespace Axiom
