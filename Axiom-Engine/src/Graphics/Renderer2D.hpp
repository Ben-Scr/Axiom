#pragma once
#include "QuadMesh.hpp"
#include "SpriteShaderProgram.hpp"
#include "TextureHandle.hpp"
#include "Instance44.hpp"
#include "Collections/AABB.hpp"
#include "Core/Export.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <functional>

namespace Axiom {
	class Scene;
#ifdef AXIOM_PROFILER_ENABLED
	class GpuTimer;
#endif

	class AXIOM_API Renderer2D {
	public:
		// unique_ptr<GpuTimer> with a forward-declared GpuTimer requires the
		// destructor to be defined where GpuTimer is complete. Out-of-line
		// in Renderer2D.cpp does that without forcing every Renderer2D.hpp
		// consumer to also include GpuTimer.hpp.
		Renderer2D();
		~Renderer2D();
		Renderer2D(const Renderer2D&) = delete;
		Renderer2D& operator=(const Renderer2D&) = delete;

		void Initialize();
		void BeginFrame();
		void EndFrame();
		void Shutdown();
		void SetEnabled(bool enabled) { m_IsEnabled = enabled; }
		bool IsEnabled() const { return m_IsEnabled; }
		bool IsInitialized() const { return m_IsInitialized; }

		void SetOutputTarget(unsigned int fboId, int width, int height) {
			m_OutputFboId = fboId;
			m_OutputWidth = width;
			m_OutputHeight = height;
		}

		void RenderScene(const Scene& scene);
		void RenderSceneWithVP(const Scene& scene, const glm::mat4& vp, const AABB& viewportAABB);

		void SetSkipBeginFrameRender(bool skip) { m_SkipBeginFrameRender = skip; }

		size_t GetRenderedInstancesCount() const { return m_RenderedInstancesCount; }
		size_t GetDrawCallsCount() const { return m_DrawCallsCount; }
		float GetRenderLoopDuration() const { return m_RenderLoopDuration; }

		using SceneProvider = std::function<void(const std::function<void(const Scene&)>&)>;
		void SetSceneProvider(SceneProvider provider) { m_SceneProvider = std::move(provider); }

		// External instance contribution. Each contributor is invoked once per
		// scene per frame DURING the renderer's collect phase — after built-in
		// particles and sprites are appended, before the sort. Lets packages
		// (e.g. Tilemap2D) push instances into the same per-frame batch so
		// they participate in sort order, frustum culling, and texture
		// batching like any built-in renderable. The contributor receives:
		//   • the scene currently being rendered
		//   • the camera viewport AABB (for cheap culling)
		//   • a vector to emplace_back into
		// Multiple contributors are called in registration order; nothing
		// stops one from being registered twice (each registration gets its
		// own token).
		using InstanceContributor = std::function<void(const Scene& scene, const AABB& viewportAABB, std::vector<Instance44>& outInstances)>;
		// Returns a token for later removal. Tokens are non-zero on success.
		static uint32_t RegisterInstanceContributor(InstanceContributor contributor);
		// No-op on an unknown / already-removed token.
		static void UnregisterInstanceContributor(uint32_t token);

	private:
		void RenderScenes();
		void CollectAndRenderInstances(const Scene& scene, const glm::mat4& vp, const AABB& viewportAABB);

		size_t m_RenderedInstancesCount = 0;
		size_t m_DrawCallsCount = 0;
		float m_RenderLoopDuration = 0.0f;
		bool m_IsInitialized = false;
		bool m_IsEnabled = true;
		bool m_SkipBeginFrameRender = false;

		std::vector<Instance44> m_Instances;

		unsigned int m_OutputFboId = 0;
		int m_OutputWidth = 0;
		int m_OutputHeight = 0;

		QuadMesh m_QuadMesh;
		SpriteShaderProgram m_SpriteShader;

		SceneProvider m_SceneProvider;

#ifdef AXIOM_PROFILER_ENABLED
		// GPU-side frame timer. Owned by unique_ptr so the .hpp doesn't
		// need to pull in GpuTimer.hpp's GLuint dependency. nullptr when
		// the profiler is stripped (the whole field is removed via #ifdef).
		std::unique_ptr<GpuTimer> m_GpuTimer;
#endif
	};
}
