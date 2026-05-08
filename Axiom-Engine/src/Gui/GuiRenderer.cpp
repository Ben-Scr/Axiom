#include "pch.hpp"
#include "GuiRenderer.hpp"

#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Tags.hpp"
#include "Components/UI/DropdownComponent.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Graphics/Shader.hpp"
#include "Graphics/Instance44.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Graphics/TextureManager.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Systems/UILayoutSystem.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace Axiom {
	namespace {

		// Walk the rect-transform hierarchy and assign each entity a
		// monotonically-increasing draw index so a parent draws before its
		// children, and earlier siblings draw before later ones. The
		// renderer sorts by this index so anything painted later overlays
		// anything painted earlier — the standard UI z-rule.
		void CollectDrawOrder(entt::registry& registry, EntityHandle entity,
			std::vector<std::pair<EntityHandle, int>>& outOrder, int& counter)
		{
			if (registry.all_of<DisabledTag>(entity)) return;
			if (registry.all_of<RectTransform2DComponent>(entity)) {
				outOrder.emplace_back(entity, counter++);
			}
			if (auto* hierarchy = registry.try_get<HierarchyComponent>(entity)) {
				for (EntityHandle child : hierarchy->Children) {
					if (registry.valid(child)) {
						CollectDrawOrder(registry, child, outOrder, counter);
					}
				}
			}
		}

	} // namespace

	void GuiRenderer::Initialize() {
		if (m_IsInitialized) {
			return;
		}

		m_SpriteShader.Initialize();
		m_QuadMesh.Initialize();

		// Owned text renderer for screen-space widget labels. Re-uses
		// the same text shader path as world-space text but rides our
		// own GL state so the two passes don't stomp on each other.
		m_TextRenderer = std::make_unique<TextRenderer>();
		m_TextRenderer->Initialize();

		m_IsInitialized = true;
	}
	void GuiRenderer::Shutdown() {
		if (!m_IsInitialized) {
			return;
		}

		if (m_TextRenderer) {
			m_TextRenderer->Shutdown();
			m_TextRenderer.reset();
		}
		m_QuadMesh.Shutdown();
		m_SpriteShader.Shutdown();
		m_IsInitialized = false;
	}

	void GuiRenderer::BeginFrame(const SceneManager& sceneManager) {
		if (!m_IsInitialized || m_SkipBeginFrameRender) {
			return;
		}

		sceneManager.ForeachLoadedScene([&](const Scene& scene) { RenderScene(scene); });
	}
	void GuiRenderer::EndFrame() {
		if (!m_IsInitialized) {
			return;
		}

	}

	void GuiRenderer::RenderScene(const Scene& scene) {
		if (!m_IsInitialized) {
			return;
		}

		AIM_ASSERT(m_SpriteShader.IsValid(), AxiomErrorCode::InvalidHandle, "Invalid Sprite 2D Shader");

		// Compute the UI layout right here so:
		//   1. Editor mode (which doesn't tick scene systems while not
		//      playing) still gets correct stretch / hierarchy layout.
		//   2. Post-event mutations from UIEventSystem (slider handle
		//      moves, fill stretches, dropdown label updates) land in
		//      the renderer's frame instead of one frame later.
		// In play mode this duplicates UILayoutSystem's earlier pass —
		// cheap (a vector walk + a few floats per rect), and the
		// guarantee of "renderer always reads fresh layout" is worth it.
		ComputeUILayout(const_cast<Scene&>(scene));

		// UI is screen-space and must NOT scroll with the camera. In
		// editor mode the renderer paints into a sub-panel FBO whose
		// pixel size differs from the OS window — the editor publishes
		// that region via Window::SetUIRegion so we project against
		// the same dimensions UILayoutSystem and UIEventSystem used.
		// In standalone runtime builds the region stays unset and the
		// main viewport (full OS window) wins.
		const Window::UIRegion uiRegion = Window::GetUIRegion();
		int w = 0;
		int h = 0;
		if (uiRegion.IsActive()) {
			w = uiRegion.Width;
			h = uiRegion.Height;
		}
		else {
			Viewport* vp = Window::GetMainViewport();
			if (!vp || vp->GetWidth() <= 0 || vp->GetHeight() <= 0) {
				return;
			}
			w = vp->GetWidth();
			h = vp->GetHeight();
		}

		const float halfW = 0.5f * w;
		const float halfH = 0.5f * h;
		const float zNear = -1.0f;
		const float zFar = 1.0f;

		const glm::mat4 mvp = glm::ortho(-halfW, +halfW, -halfH, +halfH, zNear, zFar);
		CollectAndDraw(scene, mvp, halfW, halfH);
	}

	void GuiRenderer::CollectAndDraw(const Scene& scene, const glm::mat4& mvp,
		float halfW, float halfH)
	{
		entt::registry& registry = const_cast<entt::registry&>(scene.GetRegistry());

		// ── 1. Build hierarchy draw order ────────────────────────────
		std::vector<std::pair<EntityHandle, int>> drawOrder;
		drawOrder.reserve(64);
		int counter = 0;
		auto allView = registry.view<entt::entity>(entt::exclude<DisabledTag>);
		for (auto entity : allView) {
			const HierarchyComponent* hc = registry.try_get<HierarchyComponent>(entity);
			const bool isRoot = !hc || hc->Parent == entt::null;
			if (!isRoot) continue;
			CollectDrawOrder(registry, entity, drawOrder, counter);
		}

		// ── 2. Image instances ───────────────────────────────────────
		// The sprite shader treats `iSpritePos` as the *center* of the
		// quad and rotates around it. We compute the rect's geometric
		// centre, then for rotated rects we rotate that centre around
		// the resolved pivot so rotation pivots correctly even when the
		// pivot is non-centered.
		m_InstancesScratch.clear();
		m_InstancesScratch.reserve(drawOrder.size());

		for (const auto& [entity, drawIndex] : drawOrder) {
			if (!registry.all_of<RectTransform2DComponent, ImageComponent>(entity)) continue;
			const auto& rect = registry.get<RectTransform2DComponent>(entity);
			const auto& image = registry.get<ImageComponent>(entity);

			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const Vec2 size{ tr.x - bl.x, tr.y - bl.y };
			Vec2 spritePos{ bl.x + size.x * 0.5f, bl.y + size.y * 0.5f };

			if (rect.ResolvedRotation != 0.0f && rect.ResolvedValid) {
				const Vec2 fromPivot{
					spritePos.x - rect.ResolvedPivot.x,
					spritePos.y - rect.ResolvedPivot.y
				};
				const float c = std::cos(rect.ResolvedRotation);
				const float s = std::sin(rect.ResolvedRotation);
				spritePos = {
					rect.ResolvedPivot.x + c * fromPivot.x - s * fromPivot.y,
					rect.ResolvedPivot.y + s * fromPivot.x + c * fromPivot.y
				};
			}

			m_InstancesScratch.emplace_back(
				spritePos,
				size,
				rect.ResolvedRotation,
				image.Color,
				image.TextureHandle,
				static_cast<short>(drawIndex & 0x7fff),
				static_cast<std::uint8_t>(0));
		}

		// ── 3. UI text instances (entity-driven) ─────────────────────
		m_TextScratch.clear();
		m_TextScratch.reserve(drawOrder.size());

		for (const auto& [entity, drawIndex] : drawOrder) {
			if (!registry.all_of<RectTransform2DComponent, TextRendererComponent>(entity)) continue;
			auto& text = registry.get<TextRendererComponent>(entity);
			if (text.Text.empty()) continue;

			Font* font = TextRenderer::ResolveFont(text);
			if (!font || !font->IsLoaded()) continue;

			const auto& rect = registry.get<RectTransform2DComponent>(entity);
			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const Vec2 size{ tr.x - bl.x, tr.y - bl.y };

			// Anchor the text's baseline near the vertical centre of the
			// rect, biased downward by ~25% of the font height so glyphs
			// with descenders still sit inside the box. For multi-line
			// strings this matches the first line; subsequent lines
			// extend below.
			const float bakedSize = font->GetPixelSize() > 0.0f ? font->GetPixelSize() : text.FontSize;
			const float drawScale = (text.FontSize / bakedSize); // pixels — UI is 1 unit per pixel

			float baselineY = bl.y + size.y * 0.5f - text.FontSize * 0.35f;

			float originX;
			switch (text.HAlign) {
			case TextAlignment::Center: originX = bl.x + size.x * 0.5f; break;
			case TextAlignment::Right:  originX = tr.x - 4.0f;            break; // tiny right pad
			case TextAlignment::Left:
			default:                    originX = bl.x + 4.0f;            break; // tiny left pad
			}

			TextDrawCmd cmd;
			cmd.FontPtr = font;
			cmd.Text = std::string_view(text.Text);
			cmd.X = originX;
			cmd.Y = baselineY;
			cmd.Scale = drawScale;
			cmd.LetterSpacing = text.LetterSpacing;
			cmd.Tint = text.Color;
			cmd.Align = text.HAlign;
			cmd.SortingOrder = static_cast<int16_t>(drawIndex & 0x7fff);
			cmd.SortingLayer = 1; // text always above the entity's image
			m_TextScratch.push_back(cmd);
		}

		// ── 4. Dropdown popups ───────────────────────────────────────
		// Drawn last (top of z-stack). Each open dropdown contributes a
		// background quad per option row plus an option-label text draw.
		// Hover highlight is computed on-the-fly from the cursor: the
		// renderer doesn't own input state but the visual feedback is
		// trivial enough to mirror the event-system's hit logic here.
		Application* app = Application::GetInstance();
		const Vec2 mouseRaw = app ? app->GetInput().GetMousePosition() : Vec2{ 0, 0 };
		const Vec2 mouseUi{ mouseRaw.x - halfW, halfH - mouseRaw.y };

		auto popupView = registry.view<RectTransform2DComponent, DropdownComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, rect, dd] : popupView.each()) {
			if (!dd.IsOpen || dd.Options.empty()) continue;

			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const float width = tr.x - bl.x;
			const float topOfPopup = bl.y; // popup hangs below the button

			// Resolve the dropdown's font from a label child if any —
			// otherwise default font. Dropdowns inherit the look of
			// their label entity for visual consistency.
			Font* dropdownFont = nullptr;
			float fontPx = 16.0f;
			if (dd.LabelEntity != entt::null && registry.valid(dd.LabelEntity)
				&& registry.all_of<TextRendererComponent>(dd.LabelEntity))
			{
				auto& labelText = registry.get<TextRendererComponent>(dd.LabelEntity);
				dropdownFont = TextRenderer::ResolveFont(labelText);
				fontPx = labelText.FontSize;
			}
			else {
				FontHandle dh = FontManager::GetDefaultFont();
				dropdownFont = FontManager::GetFont(dh);
			}
			if (!dropdownFont || !dropdownFont->IsLoaded()) continue;

			const float bakedSize = dropdownFont->GetPixelSize() > 0.0f
				? dropdownFont->GetPixelSize() : fontPx;
			const float textScale = (fontPx / bakedSize);

			const int popupDraw = ++counter;
			for (int i = 0; i < static_cast<int>(dd.Options.size()); ++i) {
				const float rowTop = topOfPopup - dd.OptionRowHeight * static_cast<float>(i);
				const float rowBottom = rowTop - dd.OptionRowHeight;
				const Vec2 rowBL{ bl.x, rowBottom };
				const Vec2 rowSize{ width, dd.OptionRowHeight };

				const bool hovered = mouseUi.x >= rowBL.x && mouseUi.x <= rowBL.x + rowSize.x
					&& mouseUi.y >= rowBL.y && mouseUi.y <= rowBL.y + rowSize.y;

				const Color rowColor = hovered ? dd.OptionHoverColor : dd.PopupBackgroundColor;

				const Vec2 rowCenter{ rowBL.x + rowSize.x * 0.5f, rowBL.y + rowSize.y * 0.5f };
				m_InstancesScratch.emplace_back(
					rowCenter,
					rowSize,
					0.0f,
					rowColor,
					TextureHandle{},
					static_cast<short>((popupDraw + i) & 0x7fff),
					static_cast<std::uint8_t>(10)); // popup layer

				// Option label.
				TextDrawCmd cmd;
				cmd.FontPtr = dropdownFont;
				cmd.Text = std::string_view(dd.Options[i]);
				cmd.X = rowBL.x + 8.0f; // small left pad
				cmd.Y = rowBL.y + rowSize.y * 0.5f - fontPx * 0.35f;
				cmd.Scale = textScale;
				cmd.LetterSpacing = 0.0f;
				cmd.Tint = dd.OptionTextColor;
				cmd.Align = TextAlignment::Left;
				cmd.SortingOrder = static_cast<int16_t>((popupDraw + i) & 0x7fff);
				cmd.SortingLayer = 11;
				m_TextScratch.push_back(cmd);
			}
			counter += static_cast<int>(dd.Options.size());
		}

		// ── 5. Sort and draw all images ──────────────────────────────
		std::sort(m_InstancesScratch.begin(), m_InstancesScratch.end(),
			[](const Instance44& a, const Instance44& b) {
				if (a.SortingLayer != b.SortingLayer)
					return a.SortingLayer < b.SortingLayer;
				return a.SortingOrder < b.SortingOrder;
			});

		const GLboolean wasScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
		const GLboolean wasDepthEnabled = glIsEnabled(GL_DEPTH_TEST);
		const GLboolean wasBlendEnabled = glIsEnabled(GL_BLEND);
		GLint previousBlendSrcRgb = GL_ONE;
		GLint previousBlendDstRgb = GL_ZERO;
		GLint previousBlendSrcAlpha = GL_ONE;
		GLint previousBlendDstAlpha = GL_ZERO;
		GLint previousBlendEquationRgb = GL_FUNC_ADD;
		GLint previousBlendEquationAlpha = GL_FUNC_ADD;
		glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSrcRgb);
		glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDstRgb);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSrcAlpha);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDstAlpha);
		glGetIntegerv(GL_BLEND_EQUATION_RGB, &previousBlendEquationRgb);
		glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &previousBlendEquationAlpha);

		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_DEPTH_TEST);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		m_SpriteShader.Bind();
		m_SpriteShader.SetMVP(mvp);
		m_QuadMesh.Bind();
		glActiveTexture(GL_TEXTURE0);

		// QuadMesh's per-instance attributes (Position/Scale/Rotation/Color)
		// live in an instanced VBO at locations 2-5 with divisor=1, so we
		// MUST upload + DrawInstanced — a non-instanced Draw would read
		// instance index 0 for every quad and collapse them all to the
		// zero-initialized buffer (in particular Scale=(0,0) → invisible).
		// Sort already preserved layer/order, so contiguous runs of the
		// same texture can share an upload + draw call without disturbing
		// the z-rule.
		//
		// Mirror Renderer2D's ResolveRenderableTextureHandle: a Handle
		// can satisfy `IsValid()` (index != sentinel) yet still refer to
		// a freed slot (mismatched generation) or an entry whose GPU
		// upload hasn't completed. TextureManager::IsValid covers both,
		// and lets a stale handle fall back to the engine default white
		// square instead of binding texture 0 (which samples black on
		// most drivers, making the UI quad invisible against a dark
		// background).
		const TextureHandle defaultTexture = TextureManager::GetDefaultTexture(DefaultTexture::Square);
		auto resolveHandle = [&](TextureHandle h) {
			return TextureManager::IsValid(h) ? h : defaultTexture;
		};

		size_t runStart = 0;
		while (runStart < m_InstancesScratch.size()) {
			TextureHandle runHandle = resolveHandle(m_InstancesScratch[runStart].TextureHandle);

			size_t runEnd = runStart + 1;
			while (runEnd < m_InstancesScratch.size()) {
				if (!(resolveHandle(m_InstancesScratch[runEnd].TextureHandle) == runHandle)) break;
				++runEnd;
			}

			Texture2D* texture = TextureManager::GetTexture(runHandle);
			if (texture && texture->IsValid()) {
				texture->Submit(0);
			}
			else {
				glBindTexture(GL_TEXTURE_2D, 0);
			}

			m_QuadMesh.UploadInstances(std::span<const Instance44>(
				m_InstancesScratch.data() + runStart, runEnd - runStart));
			m_QuadMesh.DrawInstanced(runEnd - runStart);

			runStart = runEnd;
		}

		m_QuadMesh.Unbind();
		m_SpriteShader.Unbind();

		// ── 6. Draw text on top ──────────────────────────────────────
		if (m_TextRenderer && m_TextRenderer->IsInitialized()) {
			m_TextRenderer->RenderInstances(m_TextScratch, mvp);
		}

		glBlendEquationSeparate(previousBlendEquationRgb, previousBlendEquationAlpha);
		glBlendFuncSeparate(previousBlendSrcRgb, previousBlendDstRgb, previousBlendSrcAlpha, previousBlendDstAlpha);
		wasScissorEnabled ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
		wasDepthEnabled ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
		wasBlendEnabled ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
	}
}
