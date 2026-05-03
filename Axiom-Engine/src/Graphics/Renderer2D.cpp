#include "pch.hpp"
#include "Renderer2D.hpp"

#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Scene/SceneManager.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Tags.hpp"
#include <Utils/Timer.hpp>

#include "Profiling/GpuTimer.hpp"
#include "Profiling/Profiler.hpp"
#include "Scene/Scene.hpp"
#include "Graphics/TextureManager.hpp"
#include "Graphics/StaticRenderData.hpp"

#include <glad/glad.h>

namespace Axiom {
	namespace {
		struct ContributorEntry {
			uint32_t Token = 0;
			Renderer2D::InstanceContributor Fn;
		};
		std::vector<ContributorEntry>& GetContributors() {
			static std::vector<ContributorEntry> s_Contributors;
			return s_Contributors;
		}
		uint32_t& NextContributorToken() {
			static uint32_t s_NextToken = 0;
			return s_NextToken;
		}

		Camera2DComponent* ResolveClearCamera() {
			Application* app = Application::GetInstance();
			if (!app || !app->GetSceneManager()) {
				return nullptr;
			}

			Scene* activeScene = app->GetSceneManager()->GetActiveScene();
			return activeScene ? activeScene->GetMainCamera() : nullptr;
		}

		bool TextureHandleLess(const TextureHandle& a, const TextureHandle& b) {
			if (a.index != b.index) {
				return a.index < b.index;
			}

			return a.generation < b.generation;
		}

		TextureHandle ResolveRenderableTextureHandle(TextureHandle requestedTexture, TextureHandle fallbackTexture) {
			if (TextureManager::IsValid(requestedTexture)) {
				return requestedTexture;
			}

			return fallbackTexture;
		}

		Texture2D* ResolveRenderableTexture(TextureHandle requestedTexture, TextureHandle fallbackTexture, TextureHandle& resolvedHandle) {
			resolvedHandle = ResolveRenderableTextureHandle(requestedTexture, fallbackTexture);
			return TextureManager::GetTexture(resolvedHandle);
		}
	}

	Renderer2D::Renderer2D() = default;
	Renderer2D::~Renderer2D() = default;

	uint32_t Renderer2D::RegisterInstanceContributor(InstanceContributor contributor) {
		if (!contributor) return 0;
		auto& contributors = GetContributors();
		auto& nextToken = NextContributorToken();
		const uint32_t token = ++nextToken;
		contributors.push_back({ token, std::move(contributor) });
		return token;
	}

	void Renderer2D::UnregisterInstanceContributor(uint32_t token) {
		if (token == 0) return;
		auto& contributors = GetContributors();
		contributors.erase(
			std::remove_if(contributors.begin(), contributors.end(),
				[token](const ContributorEntry& e) { return e.Token == token; }),
			contributors.end());
	}

	void Renderer2D::Initialize() {
		m_QuadMesh.Initialize();
		m_SpriteShader.Initialize();
		m_Instances.reserve(512);

		if (!m_SpriteShader.IsValid()) {
			AIM_CORE_ERROR_TAG("Renderer2D", "Sprite shader is invalid — rendering disabled");
			return;
		}

#ifdef AXIOM_PROFILER_ENABLED
		// GpuTimer needs a live GL context — Initialize is the right spot
		// since OpenGL::Initialize ran before us in Application::Initialize.
		m_GpuTimer = std::make_unique<GpuTimer>();
		m_GpuTimer->Initialize();
#endif

		m_IsInitialized = true;
	}

	void Renderer2D::BeginFrame() {
		// "Rendering" is the single CPU-Usage bucket for all draw work
		// (covers BeginFrame's RenderScenes/quad submission). EndFrame is
		// trivial; SwapBuffers' vsync wait is captured by the "VSync" bucket.
		AXIOM_PROFILE_SCOPE("Rendering");
#ifdef AXIOM_PROFILER_ENABLED
		if (m_GpuTimer) m_GpuTimer->BeginFrame();
#endif
		Timer timer = Timer();

		if (m_SkipBeginFrameRender) {
			m_DrawCallsCount = 0;
			m_RenderLoopDuration = timer.ElapsedMilliseconds();
			return;
		}

		if (m_OutputFboId != 0) {
			const int savedW = Window::GetMainViewport()->GetWidth();
			const int savedH = Window::GetMainViewport()->GetHeight();

			glBindFramebuffer(GL_FRAMEBUFFER, m_OutputFboId);
			glViewport(0, 0, m_OutputWidth, m_OutputHeight);
			Window::GetMainViewport()->SetSize(m_OutputWidth, m_OutputHeight);

			Camera2DComponent* cam = ResolveClearCamera();
			if (cam) {
				const auto& cc = cam->GetClearColor();
				glClearColor(cc.r, cc.g, cc.b, cc.a);
			}
			glClear(GL_COLOR_BUFFER_BIT);
			if (m_IsInitialized && m_IsEnabled)
				RenderScenes();
			else {
				m_RenderedInstancesCount = 0;
				m_DrawCallsCount = 0;
			}

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(0, 0, savedW, savedH);
			Window::GetMainViewport()->SetSize(savedW, savedH);
			m_OutputFboId = 0;
		}
		else {
			Camera2DComponent* cam = ResolveClearCamera();
			if (cam) {
				const auto& cc = cam->GetClearColor();
				glClearColor(cc.r, cc.g, cc.b, cc.a);
			}
			glClear(GL_COLOR_BUFFER_BIT);
			if (m_IsInitialized && m_IsEnabled)
				RenderScenes();
			else {
				m_RenderedInstancesCount = 0;
				m_DrawCallsCount = 0;
			}
		}

		m_RenderLoopDuration = timer.ElapsedMilliseconds();
	}

	void Renderer2D::EndFrame() {
		if (!m_IsEnabled) return;
#ifdef AXIOM_PROFILER_ENABLED
		if (m_GpuTimer) {
			m_GpuTimer->EndFrame();
			m_GpuTimer->PollAndPublish();
		}
		// Per-frame Rendering-category counters. With the engine's instanced
		// quad pipeline: each rendered instance is one quad = 4 vertices /
		// 2 triangles. DrawCalls already represents the number of GL draw
		// invocations submitted (one per state-change "batch").
		AXIOM_PROFILE_VALUE("Batches", float(m_DrawCallsCount));
		AXIOM_PROFILE_VALUE("Triangles", float(m_RenderedInstancesCount * 2));
		AXIOM_PROFILE_VALUE("Vertices", float(m_RenderedInstancesCount * 4));
#endif
	}

	void Renderer2D::RenderScenes() {
		if (m_SceneProvider) {
			m_SceneProvider([this](const Scene& scene) { RenderScene(scene); });
		}
		else {
			SceneManager::Get().ForeachLoadedScene([this](const Scene& scene) {
				RenderScene(scene);
			});
		}
	}

	void Renderer2D::RenderScene(const Scene& scene) {
		Camera2DComponent* camera2D = const_cast<Camera2DComponent*>(scene.GetMainCamera());
		if (!camera2D) {
			AIM_WARN_TAG("Renderer2D", "No main camera found in the scene. Nothing will be rendered.");
			return;
		}

		camera2D->UpdateViewport();
		CollectAndRenderInstances(scene, camera2D->GetViewProjectionMatrix(), camera2D->GetViewportAABB());
	}

	void Renderer2D::RenderSceneWithVP(const Scene& scene, const glm::mat4& vp, const AABB& viewportAABB) {
		if (!m_IsInitialized || !m_IsEnabled) return;
		CollectAndRenderInstances(scene, vp, viewportAABB);
	}

	void Renderer2D::CollectAndRenderInstances(const Scene& scene, const glm::mat4& vp, const AABB& viewportAABB) {
		if (!m_SpriteShader.IsValid()) {
			AIM_CORE_ERROR_TAG("Renderer2D", "Sprite shader is invalid — cannot render");
			return;
		}
		m_SpriteShader.Bind();
		m_SpriteShader.SetMVP(vp);

		m_Instances.clear();

		// const_cast: we lazily attach a per-entity render-time cache
		// (StaticRenderData) for entities tagged Static, and clear the
		// Transform2D dirty flag once the cache is refreshed. The registry is
		// conceptually mutable here — same shape as the camera const_cast in
		// ResolveClearCamera above.
		entt::registry& registry = const_cast<entt::registry&>(scene.GetRegistry());

		auto ptsView = registry.view<ParticleSystem2DComponent>(entt::exclude<DisabledTag>);
		auto srView = registry.view<Transform2DComponent, SpriteRendererComponent>(entt::exclude<DisabledTag>);

		m_Instances.reserve(ptsView.size_hint() + srView.size_hint());

		for (const auto& [ent, particleSystem] : ptsView.each()) {
			for (const auto& particle : particleSystem.GetParticles()) {
				if (!AABB::Intersects(viewportAABB, AABB::FromTransform(particle.Transform)))
					continue;

				m_Instances.emplace_back(
					particle.Transform.Position,
					particle.Transform.Scale,
					particle.Transform.Rotation,
					particle.Color,
					particleSystem.m_TextureHandle,
					particleSystem.RenderingSettings.SortingOrder,
					particleSystem.RenderingSettings.SortingLayer
				);
			}
		}

		for (auto&& [ent, tr, spriteRenderer] : srView.each()) {
			AABB entityAABB;
			if (registry.all_of<StaticTag>(ent)) {
				StaticRenderData& cache = registry.get_or_emplace<StaticRenderData>(ent);
				if (!cache.Valid || tr.IsDirty()) {
					cache.CachedAABB = AABB::FromTransform(tr);
					cache.Valid = true;
					tr.ClearDirty();
				}
				entityAABB = cache.CachedAABB;
			}
			else {
				entityAABB = AABB::FromTransform(tr);
			}

			if (!AABB::Intersects(viewportAABB, entityAABB))
				continue;

			m_Instances.emplace_back(
				tr.Position,
				tr.Scale,
				tr.Rotation,
				spriteRenderer.Color,
				spriteRenderer.TextureHandle,
				spriteRenderer.SortingOrder,
				spriteRenderer.SortingLayer
			);
		}

		// Package instance contributors. Tilemap2D and any future renderable
		// package pushes instances here so they participate in the same
		// sort + texture-batch path as built-in sprites/particles. Wrapped
		// per-contributor so a single misbehaving package can't crash the
		// whole render frame.
		for (const auto& entry : GetContributors()) {
			if (!entry.Fn) continue;
			try {
				entry.Fn(scene, viewportAABB, m_Instances);
			} catch (const std::exception& e) {
				AIM_CORE_ERROR_TAG("Renderer2D",
					"Instance contributor (token {}) threw: {}", entry.Token, e.what());
			} catch (...) {
				AIM_CORE_ERROR_TAG("Renderer2D",
					"Instance contributor (token {}) threw an unknown exception", entry.Token);
			}
		}

		std::sort(m_Instances.begin(), m_Instances.end(),
			[](const Instance44& a, const Instance44& b) {
				if (a.SortingLayer != b.SortingLayer)
					return a.SortingLayer < b.SortingLayer;
				if (a.SortingOrder != b.SortingOrder)
					return a.SortingOrder < b.SortingOrder;
				if (a.TextureHandle != b.TextureHandle) {
					return TextureHandleLess(a.TextureHandle, b.TextureHandle);
				}

				return false;
			});

		m_QuadMesh.Bind();
		glActiveTexture(GL_TEXTURE0);

		const TextureHandle fallbackTexture = TextureManager::GetDefaultTexture(DefaultTexture::Square);
		m_DrawCallsCount = 0;

		// E17: Carry the resolved handle for the next batch forward instead of
		// re-resolving the boundary instance with ResolveRenderableTextureHandle.
		// The inner loop now compares cheap raw handle equality and only
		// re-evaluates IsValid (via ResolveRenderableTextureHandle) when an
		// instance's stored handle differs from the current batch's stored
		// handle. Roughly halves the IsValid calls in long single-texture runs.
		for (size_t batchStart = 0; batchStart < m_Instances.size(); ) {
			TextureHandle batchTextureHandle{};
			Texture2D* batchTexture = ResolveRenderableTexture(
				m_Instances[batchStart].TextureHandle,
				fallbackTexture,
				batchTextureHandle);

			const TextureHandle batchSourceHandle = m_Instances[batchStart].TextureHandle;
			size_t batchEnd = batchStart + 1;
			for (; batchEnd < m_Instances.size(); ++batchEnd) {
				const TextureHandle& nextSourceHandle = m_Instances[batchEnd].TextureHandle;
				// Same source handle as the batch -> guaranteed to resolve to
				// the same handle, no IsValid call needed.
				if (nextSourceHandle == batchSourceHandle) {
					continue;
				}

				TextureHandle nextTextureHandle = ResolveRenderableTextureHandle(
					nextSourceHandle,
					fallbackTexture);
				if (!(nextTextureHandle == batchTextureHandle)) {
					break;
				}
			}

			if (batchTexture && batchTexture->IsValid()) {
				batchTexture->Submit(0);
			}
			else {
				glBindTexture(GL_TEXTURE_2D, 0);
			}

			m_QuadMesh.UploadInstances(std::span<const Instance44>(m_Instances.data() + batchStart, batchEnd - batchStart));
			m_QuadMesh.DrawInstanced(batchEnd - batchStart);
			++m_DrawCallsCount;
			batchStart = batchEnd;
		}

		m_QuadMesh.Unbind();
		m_SpriteShader.Unbind();
		m_RenderedInstancesCount = m_Instances.size();
	}

	void Renderer2D::Shutdown() {
#ifdef AXIOM_PROFILER_ENABLED
		// Tear down the GPU timer while we still have a live GL context.
		if (m_GpuTimer) {
			m_GpuTimer->Shutdown();
			m_GpuTimer.reset();
		}
#endif
		m_QuadMesh.Shutdown();
		m_SpriteShader.Shutdown();
		m_Instances.clear();
		m_Instances.shrink_to_fit();
		m_DrawCallsCount = 0;
		// Defensive: clear instance contributors here so the dispatch list
		// can never outlive the renderer. Today PackageHost::UnloadAll runs
		// last in Application::Shutdown (per audit H10) and each package's
		// AxiomPackage_OnUnload calls UnregisterInstanceContributor — but a
		// hard process exit, abort path, or future reorder could skip those.
		// A zero-sized contributor list during render is the only safe
		// post-Shutdown state for the renderer, so we enforce it here.
		GetContributors().clear();
	}
}
