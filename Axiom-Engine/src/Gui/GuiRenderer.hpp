#pragma once
#include "Core/Export.hpp"
#include "Graphics/SpriteShaderProgram.hpp"
#include "Graphics/QuadMesh.hpp"
#include "Graphics/Instance44.hpp"
#include "Graphics/Text/TextRenderer.hpp"

#include <memory>
#include <vector>

namespace Axiom {
	class Scene;
	class SceneManager;

	// Screen-space UI renderer. Walks every loaded scene's
	// RectTransform2D entities each frame, rendering:
	//   - Image components (background quads)
	//   - TextRendererComponent on the same entity (widget labels)
	//   - Dropdown popup option lists when DropdownComponent::IsOpen
	//
	// Coordinate system: centered, +Y up, units = framebuffer pixels.
	// The window viewport (NOT the camera viewport) drives layout, so
	// UI doesn't move when the camera scrolls.
	class AXIOM_API GuiRenderer {
	public:
		GuiRenderer() = default;
		void Initialize();
		void Shutdown();

		void BeginFrame(const SceneManager& sceneManager);
		void EndFrame();

		void RenderScene(const Scene& scene);

		// When true, BeginFrame becomes a no-op so an external driver
		// (e.g. the editor's per-FBO render path) can call RenderScene
		// itself with a target framebuffer already bound. Mirrors
		// Renderer2D::SetSkipBeginFrameRender so editor & runtime share
		// the same opt-out mechanism.
		void SetSkipBeginFrameRender(bool skip) { m_SkipBeginFrameRender = skip; }

	private:
		void CollectAndDraw(const Scene& scene, const glm::mat4& mvp,
			float halfW, float halfH);

		SpriteShaderProgram m_SpriteShader;
		QuadMesh m_QuadMesh;

		// Owned text renderer so UI can render labels in screen-space
		// without going through Renderer2D's world-space text pass.
		std::unique_ptr<TextRenderer> m_TextRenderer;

		// Reused across frames so RenderScene doesn't heap-allocate per frame.
		std::vector<Instance44> m_InstancesScratch;
		std::vector<TextDrawCmd> m_TextScratch;

		bool m_IsInitialized = false;
		bool m_SkipBeginFrameRender = false;
	};
}
