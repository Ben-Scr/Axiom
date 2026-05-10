#include "pch.hpp"
#include "Gui/GuiRenderer.hpp"

#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Tags.hpp"
#include "Components/UI/CircularSliderComponent.hpp"
#include "Components/UI/DropdownComponent.hpp"
#include "Components/UI/InputFieldComponent.hpp"
#include "Components/UI/MaskComponent.hpp"
#include "Core/Application.hpp"
#include "Core/Input.hpp"
#include "Core/MouseButton.hpp"
#include "Core/Time.hpp"
#include "Core/Window.hpp"
#include "Graphics/BgfxSpriteResources.hpp"
#include "Graphics/Instance44.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Graphics/Text/TextRenderer.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/UIDrawOrder.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Systems/UILayoutSystem.hpp"

#include <bgfx/bgfx.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>

// =============================================================================
// GuiRenderer — bgfx implementation. Stage 2.4 of the bgfx port.
// -----------------------------------------------------------------------------
// Mirrors GuiRenderer.cpp's CollectAndDraw structure verbatim — hierarchy walk,
// image/circle/text/dropdown/input-field collection, sort by (Layer, Order,
// DrawIndex), merge-walk image+text spans — then replaces the GL-state +
// glScissor/glDrawElementsInstanced submission with bgfx per-submit state and
// a transient instance data buffer.
//
// View-id strategy
//   GuiRenderer allocates one bgfx view at Initialize (m_UiViewId). Each frame
//   RenderScene mirrors the currently-bound framebuffer + view rect onto
//   m_UiViewId, sets the UI mvp via setViewTransform, and submits UI draws
//   there. Because m_UiViewId is allocated AFTER bgfx::init's view 0 + any
//   editor-FBO views that exist at construction time, bgfx's numeric view
//   flush order naturally lands UI on top of scene rendering.
//
//   Caveat: editor panels created AFTER GuiRenderer construction get a higher
//   view-id than m_UiViewId, so their scene render would flush AFTER UI on
//   that panel. Practical impact today is nil — the editor allocates Game
//   View / Editor View FBOs at startup before the first GuiRenderer touch.
//   When that becomes a real symptom we'll lift the order via
//   bgfx::setViewOrder().
// =============================================================================

namespace Axiom::BgfxBackend {
	bgfx::ViewId AllocateViewId();
	void FreeViewId(bgfx::ViewId id);
	void CurrentViewRect(uint16_t& outX, uint16_t& outY, uint16_t& outW, uint16_t& outH);
	bgfx::FrameBufferHandle CurrentFramebuffer();
}

namespace Axiom {
	namespace {

		// ── Input field overlay helpers ───────────────────────────
		// Lifted verbatim from GuiRenderer.cpp — pure CPU helpers.

		bool Utf8DecodeAt(std::string_view s, int idx, uint32_t& outCp, int& outLen) {
			if (idx < 0 || idx >= static_cast<int>(s.size())) {
				outCp = 0; outLen = 0; return false;
			}
			const unsigned char b = static_cast<unsigned char>(s[idx]);
			if (b < 0x80) { outCp = b; outLen = 1; return true; }
			int len; uint32_t cp;
			if ((b & 0xE0) == 0xC0)      { len = 2; cp = b & 0x1F; }
			else if ((b & 0xF0) == 0xE0) { len = 3; cp = b & 0x0F; }
			else if ((b & 0xF8) == 0xF0) { len = 4; cp = b & 0x07; }
			else { outCp = b; outLen = 1; return true; }
			if (idx + len > static_cast<int>(s.size())) {
				outCp = b; outLen = 1; return true;
			}
			for (int i = 1; i < len; ++i) {
				cp = (cp << 6) | (static_cast<unsigned char>(s[idx + i]) & 0x3F);
			}
			outCp = cp; outLen = len; return true;
		}

		float MeasureUpToByteUI(const Font& font, std::string_view text, int targetByte,
			float letterSpacing)
		{
			if (targetByte <= 0) return 0.0f;
			const int n = static_cast<int>(text.size());
			if (targetByte > n) targetByte = n;
			float w = 0.0f;
			uint32_t prev = 0;
			int glyphCount = 0;
			int idx = 0;
			while (idx < targetByte) {
				uint32_t cp; int len;
				if (!Utf8DecodeAt(text, idx, cp, len)) break;
				if (idx + len > targetByte) break;
				const GlyphMetrics* g = font.GetGlyph(cp);
				if (g) {
					if (prev != 0) w += font.GetKerning(prev, cp);
					w += g->XAdvance;
					if (glyphCount > 0) w += letterSpacing;
					++glyphCount;
					prev = cp;
				}
				else {
					prev = 0;
				}
				idx += len;
			}
			return w;
		}

		struct InputFieldOverlayLayout {
			bool Valid = false;
			float OriginX = 0.0f;
			float Scale = 1.0f;
			float LetterSpacing = 0.0f;
			Font* FontPtr = nullptr;
			TextAlignment Align = TextAlignment::Left;
			Vec2 BL{};
			Vec2 TR{};
			float FontSize = 16.0f;
		};

		InputFieldOverlayLayout ResolveInputFieldOverlay(entt::registry& registry,
			const InputFieldComponent& field)
		{
			InputFieldOverlayLayout out{};
			if (field.TextEntity == entt::null || !registry.valid(field.TextEntity)) return out;
			if (!registry.all_of<RectTransform2DComponent, TextRendererComponent>(field.TextEntity)) return out;

			auto& rect = registry.get<RectTransform2DComponent>(field.TextEntity);
			auto& tc = registry.get<TextRendererComponent>(field.TextEntity);
			Font* font = TextRenderer::ResolveFont(tc);
			if (!font || !font->IsLoaded()) return out;

			const float uniformScale = rect.Scale.x;
			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const float w = tr.x - bl.x;
			float originX;
			switch (tc.HAlign) {
			case TextAlignment::Center: originX = bl.x + w * 0.5f;         break;
			case TextAlignment::Right:  originX = tr.x - 4.0f * uniformScale; break;
			case TextAlignment::Left:
			default:                    originX = bl.x + 4.0f * uniformScale; break;
			}

			const float bakedSize = font->GetPixelSize() > 0.0f ? font->GetPixelSize() : tc.FontSize;
			out.Valid = true;
			out.OriginX = originX;
			out.Scale = (tc.FontSize / bakedSize) * uniformScale;
			out.LetterSpacing = tc.LetterSpacing;
			out.FontPtr = font;
			out.Align = tc.HAlign;
			out.BL = bl;
			out.TR = tr;
			out.FontSize = tc.FontSize * uniformScale;
			return out;
		}

		bool ResolveClipForEntity(entt::registry& registry, EntityHandle entity,
			Vec2& outMin, Vec2& outMax)
		{
			bool found = false;
			Vec2 clipMin{ 0.0f, 0.0f };
			Vec2 clipMax{ 0.0f, 0.0f };

			EntityHandle current = entity;
			while (true) {
				const HierarchyComponent* h = registry.try_get<HierarchyComponent>(current);
				if (!h || h->Parent == entt::null) break;
				current = h->Parent;
				if (!registry.valid(current)) break;
				if (!registry.all_of<MaskComponent, RectTransform2DComponent>(current)) continue;

				const auto& rect = registry.get<RectTransform2DComponent>(current);
				const Vec2 bl = rect.GetBottomLeft();
				const Vec2 tr = rect.GetTopRight();

				if (!found) {
					clipMin = bl;
					clipMax = tr;
					found = true;
				}
				else {
					clipMin.x = std::max(clipMin.x, bl.x);
					clipMin.y = std::max(clipMin.y, bl.y);
					clipMax.x = std::min(clipMax.x, tr.x);
					clipMax.y = std::min(clipMax.y, tr.y);
				}
			}

			if (!found) return false;
			outMin = clipMin;
			outMax = clipMax;
			return true;
		}

		bool ResolveUICanvasSize(int& outW, int& outH) {
			const Window::UIRegion uiRegion = Window::GetUIRegion();
			if (uiRegion.IsActive()) {
				outW = uiRegion.Width;
				outH = uiRegion.Height;
				return outW > 0 && outH > 0;
			}
			Viewport* vp = Window::GetMainViewport();
			if (!vp || vp->GetWidth() <= 0 || vp->GetHeight() <= 0) {
				return false;
			}
			outW = vp->GetWidth();
			outH = vp->GetHeight();
			return true;
		}

		// Per-(GuiRenderer instance, framebuffer) UI view-id pool.
		//
		// The editor renders two FBOs per frame (Game View + Editor View),
		// each calling GuiRenderer::RenderScene against its own bgfx
		// framebuffer. A single UI view-id can't service both — the second
		// RenderScene's setViewFrameBuffer would point the view at the
		// Editor View FBO and bgfx would flush BOTH passes' UI submits
		// onto that one target, leaving Game View's UI overlay missing.
		//
		// Keying the view-id by (this, framebuffer-idx) gives every FBO its
		// own dedicated UI overlay view. View-ids stay allocated until the
		// GuiRenderer is destroyed (FBOs come and go but their handle idx
		// reuse cycles through us via Shutdown). 0xFFFF stands in for
		// "swap chain / default framebuffer".
		struct GuiRendererBgfxState {
			std::unordered_map<uint16_t, bgfx::ViewId> UiViewByFboIdx;
		};
		std::unordered_map<const GuiRenderer*, GuiRendererBgfxState> g_BgfxState;

		GuiRendererBgfxState& GetBgfxState(const GuiRenderer* self) {
			return g_BgfxState[self];
		}

		bgfx::ViewId AcquireUiViewForFbo(GuiRendererBgfxState& s, bgfx::FrameBufferHandle fbo) {
			const uint16_t key = bgfx::isValid(fbo) ? fbo.idx : uint16_t(0xFFFFu);
			auto it = s.UiViewByFboIdx.find(key);
			if (it != s.UiViewByFboIdx.end()) {
				return it->second;
			}
			const bgfx::ViewId id = BgfxBackend::AllocateViewId();
			s.UiViewByFboIdx.emplace(key, id);
			return id;
		}

	} // namespace

	void GuiRenderer::Initialize() {
		if (m_IsInitialized) {
			return;
		}

		// Bumping the shared sprite resources is idempotent — Renderer2D
		// already grabbed them at engine start, but the refcount keeps
		// them alive even if Renderer2D shuts down first.
		BgfxSpriteResources::Acquire();

		// Owned text renderer for screen-space widget labels. The bgfx
		// variant manages its own program + transient vertex buffer; same
		// public surface as the GL version.
		m_TextRenderer = std::make_unique<TextRenderer>();
		m_TextRenderer->Initialize();

		// UI view-ids are allocated lazily per framebuffer in
		// AcquireUiViewForFbo — there's nothing to do at Initialize.
		(void)GetBgfxState(this); // ensure the side-table entry exists

		m_IsInitialized = true;
	}

	void GuiRenderer::Shutdown() {
		if (!m_IsInitialized) {
			return;
		}

		auto it = g_BgfxState.find(this);
		if (it != g_BgfxState.end()) {
			for (auto& [_, vid] : it->second.UiViewByFboIdx) {
				BgfxBackend::FreeViewId(vid);
			}
			g_BgfxState.erase(it);
		}

		if (m_TextRenderer) {
			m_TextRenderer->Shutdown();
			m_TextRenderer.reset();
		}
		BgfxSpriteResources::Release();
		m_IsInitialized = false;
	}

	void GuiRenderer::BeginFrame(const SceneManager& sceneManager) {
		if (!m_IsInitialized || m_SkipBeginFrameRender) {
			return;
		}

		sceneManager.ForeachLoadedScene([&](const Scene& scene) { RenderScene(scene); });
	}
	void GuiRenderer::EndFrame() {
		// No-op — submission landed inside RenderScene; bgfx::frame()
		// flushes everything from Window::SwapBuffers.
	}

	float GuiRenderer::ComputeWorldUIPixelScale() {
		int canvasW = 0;
		int canvasH = 0;
		if (!ResolveUICanvasSize(canvasW, canvasH)) {
			return 0.01f;
		}
		if (Camera2DComponent* cam = Camera2DComponent::Main()) {
			const float worldHeight = 2.0f * cam->GetOrthographicSize() * cam->GetZoom();
			if (worldHeight > 0.0f) {
				return worldHeight / static_cast<float>(canvasH);
			}
		}
		return 0.01f;
	}

	void GuiRenderer::RenderScene(const Scene& scene) {
		if (!m_IsInitialized) {
			return;
		}

		// Compute layout for this frame so renderer reads fresh rects
		// (mirrors the OpenGL impl's pre-render layout pass).
		ComputeUILayout(const_cast<Scene&>(scene));

		int w = 0;
		int h = 0;
		if (!ResolveUICanvasSize(w, h)) {
			return;
		}

		const float halfW = 0.5f * w;
		const float halfH = 0.5f * h;
		const float zNear = -1.0f;
		const float zFar  = 1.0f;

		// Standard +Y-up ortho — matches the camera VP convention used
		// for sprite rendering, so screen-space UI and world-space
		// sprites share an orientation. Editor FBO display + standalone
		// runtime both consume the result without an extra Y flip
		// (ImGui::Image uses default UV(0,0)–(1,1), runtime draws
		// straight to the swap chain). An earlier port of this path
		// Y-flipped the ortho to compensate for an ImGui::Image UV flip
		// that has since been removed; flipping both broke the runtime
		// (no FBO to undo the flip) and left UI mirrored vs sprites
		// inside the editor's Editor View.
		const glm::mat4 mvp = glm::ortho(-halfW, +halfW, -halfH, +halfH, zNear, zFar);
		CollectAndDraw(scene, mvp, halfW, halfH);
	}

	void GuiRenderer::RenderScene(const Scene& scene, const glm::mat4& worldVP, float pixelToWorldScale) {
		if (!m_IsInitialized) {
			return;
		}

		ComputeUILayout(const_cast<Scene&>(scene));

		int w = 0;
		int h = 0;
		if (!ResolveUICanvasSize(w, h)) {
			return;
		}

		const glm::mat4 uiToWorld = glm::scale(glm::mat4(1.0f),
			glm::vec3(pixelToWorldScale, pixelToWorldScale, 1.0f));
		const glm::mat4 mvp = worldVP * uiToWorld;

		const float halfW = 0.5f * w;
		const float halfH = 0.5f * h;
		CollectAndDraw(scene, mvp, halfW, halfH);
	}

	void GuiRenderer::CollectAndDraw(const Scene& scene, const glm::mat4& mvp,
		float halfW, float halfH)
	{
		entt::registry& registry = const_cast<entt::registry&>(scene.GetRegistry());

		// ── 1. Build hierarchy draw order ────────────────────────────
		m_DrawOrder.clear();
		UIDrawOrder::Build(registry, m_DrawOrder);

		int counter = m_DrawOrder.empty()
			? 0
			: m_DrawOrder.back().second + UIDrawOrder::k_HierarchyStep;

		// ── 2. Image instances ───────────────────────────────────────
		m_InstancesScratch.clear();
		m_InstancesScratch.reserve(m_DrawOrder.size());

		for (const auto& [entity, drawIndex] : m_DrawOrder) {
			if (!registry.all_of<RectTransform2DComponent, ImageComponent>(entity)) continue;
			if (const auto* mask = registry.try_get<MaskComponent>(entity)) {
				if (!mask->ShowMaskGraphic) continue;
			}

			const auto& rect = registry.get<RectTransform2DComponent>(entity);
			const auto& image = registry.get<ImageComponent>(entity);

			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const Vec2 size{ tr.x - bl.x, tr.y - bl.y };
			Vec2 spritePos{ bl.x + size.x * 0.5f, bl.y + size.y * 0.5f };

			if (rect.Rotation != 0.0f && rect.ResolvedValid) {
				const Vec2 fromPivot{
					spritePos.x - rect.ResolvedPivot.x,
					spritePos.y - rect.ResolvedPivot.y
				};
				const float c = std::cos(rect.Rotation);
				const float s = std::sin(rect.Rotation);
				spritePos = {
					rect.ResolvedPivot.x + c * fromPivot.x - s * fromPivot.y,
					rect.ResolvedPivot.y + s * fromPivot.x + c * fromPivot.y
				};
			}

			Instance44 inst(
				spritePos,
				size,
				rect.Rotation,
				image.Color,
				image.TextureHandle,
				image.SortingOrder,
				image.SortingLayer,
				static_cast<std::uint32_t>(drawIndex));
			inst.HasClip = ResolveClipForEntity(registry, entity, inst.ClipMin, inst.ClipMax);
			m_InstancesScratch.push_back(inst);
		}

		// ── 2b. Circular slider background + fill ───────────────────
		const TextureHandle defaultWhite  = TextureManager::GetDefaultTexture(DefaultTexture::Square);
		const TextureHandle defaultCircle = TextureManager::GetDefaultTexture(DefaultTexture::Circle);
		const TextureHandle bgTexture     = defaultCircle.IsValid() ? defaultCircle : defaultWhite;

		auto emitFillSlice = [&](const Vec2& centre, float parentRot,
			const Color& color, float startRad, float sweepRad,
			float outerRadius, int segments,
			std::uint32_t drawIndex, std::int16_t sortOrder, std::uint8_t sortLayer,
			bool hasClip, const Vec2& clipMin, const Vec2& clipMax)
		{
			if (segments < 2) segments = 2;
			if (outerRadius <= 0.0f) return;
			if (std::abs(sweepRad) < 1e-4f) return;

			const float segAngle = sweepRad / static_cast<float>(segments);
			const float chord = 2.0f * outerRadius * std::sin(0.5f * std::abs(segAngle));
			const float quadWidth = chord * 1.02f;

			for (int i = 0; i < segments; ++i) {
				const float midAngle = startRad + (static_cast<float>(i) + 0.5f) * segAngle + parentRot;
				const float midRadius = outerRadius * 0.5f;
				const float dx = std::cos(midAngle) * midRadius;
				const float dy = std::sin(midAngle) * midRadius;
				Instance44 inst(
					Vec2{ centre.x + dx, centre.y + dy },
					Vec2{ quadWidth, outerRadius },
					0.0f,
					color,
					defaultWhite,
					sortOrder,
					sortLayer,
					drawIndex);
				inst.Rotation = midAngle + 1.5707963267948966f;
				inst.HasClip = hasClip;
				inst.ClipMin = clipMin;
				inst.ClipMax = clipMax;
				m_InstancesScratch.push_back(inst);
			}
		};

		for (const auto& [entity, drawIndex] : m_DrawOrder) {
			if (!registry.all_of<RectTransform2DComponent, CircularSliderComponent>(entity)) continue;
			const auto& rect = registry.get<RectTransform2DComponent>(entity);
			const auto& cs   = registry.get<CircularSliderComponent>(entity);

			const Vec2 size = rect.GetSize();
			const float outerRadius = std::min(size.x, size.y) * 0.5f;
			if (outerRadius <= 0.0f) continue;

			const Vec2 centre = rect.GetCenter();
			const float parentRot = rect.Rotation;

			constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
			const float startRad = cs.StartAngleDegrees * kDeg2Rad;
			const float sweepRad = cs.SweepDegrees * kDeg2Rad
				* (cs.Clockwise ? -1.0f : 1.0f);

			const float range = cs.MaxValue - cs.MinValue;
			const float t = (range != 0.0f)
				? std::clamp((cs.Value - cs.MinValue) / range, 0.0f, 1.0f)
				: 0.0f;

			Vec2 clipMin{}, clipMax{};
			const bool hasClip = ResolveClipForEntity(registry, entity, clipMin, clipMax);

			{
				Instance44 bg(
					centre,
					Vec2{ outerRadius * 2.0f, outerRadius * 2.0f },
					parentRot,
					cs.BackgroundColor,
					bgTexture,
					/*sortOrder*/ 0,
					/*sortLayer*/ 0,
					static_cast<std::uint32_t>(drawIndex));
				bg.HasClip = hasClip;
				bg.ClipMin = clipMin;
				bg.ClipMax = clipMax;
				m_InstancesScratch.push_back(bg);
			}

			if (t > 0.0f) {
				const int totalSegments = std::max(8, std::min(cs.RingSegments, 32));
				const int fillSegments = std::max(1,
					static_cast<int>(std::round(static_cast<float>(totalSegments) * t)));
				emitFillSlice(centre, parentRot, cs.FillColor,
					startRad, sweepRad * t, outerRadius, fillSegments,
					static_cast<std::uint32_t>(drawIndex) + 1u,
					/*sortOrder*/ 1, /*sortLayer*/ 0,
					hasClip, clipMin, clipMax);
			}
		}

		// ── 3. UI text instances (entity-driven) ─────────────────────
		m_TextScratch.clear();
		m_TextScratch.reserve(m_DrawOrder.size());

		for (const auto& [entity, drawIndex] : m_DrawOrder) {
			if (!registry.all_of<RectTransform2DComponent, TextRendererComponent>(entity)) continue;
			auto& text = registry.get<TextRendererComponent>(entity);
			if (text.Text.empty()) continue;

			const auto& rect = registry.get<RectTransform2DComponent>(entity);

			const float effectivePixelSize = text.FontSize * std::max(0.01f, std::abs(rect.Scale.x));
			Font* font = TextRenderer::ResolveFontAtPixelSize(text, effectivePixelSize);
			if (!font || !font->IsLoaded()) continue;
			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const Vec2 size{ tr.x - bl.x, tr.y - bl.y };

			const float uniformScale = rect.Scale.x;
			const float bakedSize = font->GetPixelSize() > 0.0f ? font->GetPixelSize() : text.FontSize;
			const float drawScale = (text.FontSize / bakedSize) * uniformScale;

			const float marginLeftWorld   = text.Margin.x * uniformScale;
			const float marginTopWorld    = text.Margin.y * uniformScale;
			const float marginRightWorld  = text.Margin.z * uniformScale;
			const float marginBottomWorld = text.Margin.w * uniformScale;
			(void)marginBottomWorld;

			float baselineY = bl.y + size.y * 0.5f
				- text.FontSize * 0.35f * uniformScale
				- marginTopWorld;

			float originX;
			switch (text.HAlign) {
			case TextAlignment::Center: originX = bl.x + size.x * 0.5f;                      break;
			case TextAlignment::Right:  originX = tr.x - 4.0f * uniformScale - marginRightWorld; break;
			case TextAlignment::Left:
			default:                    originX = bl.x + 4.0f * uniformScale + marginLeftWorld;  break;
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
			cmd.Wrap = text.WrapMode;
			if (text.WrapMode != TextWrapMode::None) {
				const float padPixels = 8.0f * uniformScale;
				const float marginPixels = marginLeftWorld + marginRightWorld;
				cmd.WrapWidthPixels = uniformScale > 0.0f
					? std::max(0.0f, (size.x - padPixels - marginPixels) / uniformScale)
					: 0.0f;
			}
			cmd.SortingOrder = text.SortingOrder;
			cmd.SortingLayer = text.SortingLayer;
			cmd.DrawIndex = static_cast<std::uint32_t>(drawIndex);
			cmd.HasClip = ResolveClipForEntity(registry, entity, cmd.ClipMin, cmd.ClipMax);
			cmd.Rotation = rect.Rotation;
			cmd.Pivot = rect.ResolvedPivot;
			m_TextScratch.push_back(cmd);
		}

		// ── 4. Dropdown popups ───────────────────────────────────────
		Application* app = Application::GetInstance();
		const Vec2 mouseRaw = app ? app->GetInput().GetMousePosition() : Vec2{ 0, 0 };
		const Vec2 mouseUi{ mouseRaw.x - halfW, halfH - mouseRaw.y };
		const bool mouseHeld = app && app->GetInput().GetMouse(MouseButton::Left);

		auto resolveOptionColor = [](const DropdownComponent& dd, bool hovered, bool pressed, bool selected) -> Color {
			if (pressed && dd.OptionPressedColor.a > 0.f)  return dd.OptionPressedColor;
			if (hovered && dd.OptionHoverColor.a > 0.f)    return dd.OptionHoverColor;
			if (selected && dd.OptionSelectedColor.a > 0.f) return dd.OptionSelectedColor;
			if (dd.OptionNormalColor.a > 0.f)              return dd.OptionNormalColor;
			return dd.PopupBackgroundColor;
		};

		auto popupView = registry.view<RectTransform2DComponent, DropdownComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, rect, dd] : popupView.each()) {
			if (!dd.IsOpen || dd.Options.empty()) continue;

			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const float width = tr.x - bl.x;
			const float topOfPopup = bl.y;

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
			const float uniformScale = rect.Scale.x;
			const float textScale = (fontPx / bakedSize) * uniformScale;

			const int popupDraw = ++counter;
			for (int i = 0; i < static_cast<int>(dd.Options.size()); ++i) {
				const float rowTop = topOfPopup - dd.OptionRowHeight * static_cast<float>(i);
				const float rowBottom = rowTop - dd.OptionRowHeight;
				const Vec2 rowBL{ bl.x, rowBottom };
				const Vec2 rowSize{ width, dd.OptionRowHeight };

				const bool hovered = mouseUi.x >= rowBL.x && mouseUi.x <= rowBL.x + rowSize.x
					&& mouseUi.y >= rowBL.y && mouseUi.y <= rowBL.y + rowSize.y;
				const bool pressed = hovered && mouseHeld;
				const bool selected = (i == dd.SelectedIndex);

				const Color rowColor = resolveOptionColor(dd, hovered, pressed, selected);

				const Vec2 rowCenter{ rowBL.x + rowSize.x * 0.5f, rowBL.y + rowSize.y * 0.5f };
				m_InstancesScratch.emplace_back(
					rowCenter,
					rowSize,
					0.0f,
					rowColor,
					TextureHandle{},
					static_cast<short>((popupDraw + i) & 0x7fff),
					static_cast<std::uint8_t>(10),
					static_cast<std::uint32_t>(popupDraw + i));

				TextDrawCmd cmd;
				cmd.FontPtr = dropdownFont;
				cmd.Text = std::string_view(dd.Options[i]);
				cmd.X = rowBL.x + 8.0f * uniformScale;
				cmd.Y = rowBL.y + rowSize.y * 0.5f - fontPx * 0.35f * uniformScale;
				cmd.Scale = textScale;
				cmd.LetterSpacing = 0.0f;
				cmd.Tint = dd.OptionTextColor;
				cmd.Align = TextAlignment::Left;
				cmd.SortingOrder = static_cast<int16_t>((popupDraw + i) & 0x7fff);
				cmd.SortingLayer = 11;
				cmd.DrawIndex = static_cast<std::uint32_t>(popupDraw + i);
				m_TextScratch.push_back(cmd);
			}
			counter += static_cast<int>(dd.Options.size());
		}

		// ── 4.5 Input field overlays (selection + caret) ────────────
		const float elapsedSeconds = app ? app->GetTime().GetElapsedTime() : 0.0f;

		m_DrawIndexByEntity.clear();
		m_DrawIndexByEntity.reserve(m_DrawOrder.size());
		for (const auto& [entity, di] : m_DrawOrder) {
			m_DrawIndexByEntity.emplace(entity, static_cast<std::uint32_t>(di));
		}

		auto inputView = registry.view<RectTransform2DComponent, InputFieldComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, fieldRect, field] : inputView.each()) {
			const bool hasSelection = field.SelectionAnchorBytePos != field.CaretBytePos;
			if (!field.IsFocused && !hasSelection) continue;

			InputFieldOverlayLayout layout = ResolveInputFieldOverlay(registry, field);
			if (!layout.Valid) continue;

			auto fieldIt = m_DrawIndexByEntity.find(entity);
			auto textIt = m_DrawIndexByEntity.find(field.TextEntity);
			if (fieldIt == m_DrawIndexByEntity.end() || textIt == m_DrawIndexByEntity.end()) continue;

			const std::uint32_t fieldDI = fieldIt->second;
			const std::uint32_t textDI = textIt->second;

			Vec2 fieldClipMin{};
			Vec2 fieldClipMax{};
			const bool fieldHasClip = ResolveClipForEntity(registry, entity, fieldClipMin, fieldClipMax);

			const auto& tc = registry.get<TextRendererComponent>(field.TextEntity);
			std::string secretMaskBuffer;
			if (field.IsSecret && !field.Text.empty()) {
				secretMaskBuffer.reserve(field.Text.size());
				int mIdx = 0;
				const int mN = static_cast<int>(field.Text.size());
				while (mIdx < mN) {
					std::uint32_t cp; int len;
					if (!Utf8DecodeAt(field.Text, mIdx, cp, len)) break;
					secretMaskBuffer.push_back('*');
					mIdx += len;
				}
			}
			const std::string& measureText = field.IsSecret ? secretMaskBuffer : field.Text;

			auto convertByte = [&](int byteInOriginal) -> int {
				if (!field.IsSecret) return byteInOriginal;
				int idx = 0;
				int count = 0;
				const int n = static_cast<int>(field.Text.size());
				if (byteInOriginal > n) byteInOriginal = n;
				while (idx < byteInOriginal) {
					std::uint32_t cp; int len;
					if (!Utf8DecodeAt(field.Text, idx, cp, len)) break;
					idx += len;
					++count;
				}
				return count;
			};

			const float verticalPad = 2.0f;
			const float halfHeight = layout.FontSize * 0.5f + verticalPad;
			const float centerY = layout.BL.y + (layout.TR.y - layout.BL.y) * 0.5f;

			const float fullLineW = MeasureUpToByteUI(*layout.FontPtr, measureText,
				static_cast<int>(measureText.size()), layout.LetterSpacing) * layout.Scale;
			float alignShift = 0.0f;
			if (layout.Align == TextAlignment::Center) alignShift = -fullLineW * 0.5f;
			else if (layout.Align == TextAlignment::Right) alignShift = -fullLineW;

			auto byteToAbsX = [&](int byte) {
				const float w = MeasureUpToByteUI(*layout.FontPtr, measureText,
					convertByte(byte), layout.LetterSpacing) * layout.Scale;
				return layout.OriginX + alignShift + w;
			};

			if (hasSelection) {
				const int lo = std::min(field.SelectionAnchorBytePos, field.CaretBytePos);
				const int hi = std::max(field.SelectionAnchorBytePos, field.CaretBytePos);
				const float xLo = byteToAbsX(lo);
				const float xHi = byteToAbsX(hi);
				const float selW = std::max(0.0f, xHi - xLo);
				if (selW > 0.0f) {
					const Vec2 selCenter{ xLo + selW * 0.5f, centerY };
					const Vec2 selSize{ selW, halfHeight * 2.0f };
					Instance44 selInst(
						selCenter,
						selSize,
						0.0f,
						field.SelectionColor,
						TextureHandle{},
						static_cast<short>(0),
						static_cast<std::uint8_t>(0),
						fieldDI + 1u);
					selInst.HasClip = fieldHasClip;
					selInst.ClipMin = fieldClipMin;
					selInst.ClipMax = fieldClipMax;
					m_InstancesScratch.push_back(selInst);
				}
			}

			const bool caretBlinkOn = (field.CaretBlinkRate <= 0.0f)
				? true
				: (std::fmod(elapsedSeconds * field.CaretBlinkRate, 1.0f) < 0.5f);
			if (field.IsFocused && caretBlinkOn && !field.IsReadOnly) {
				const float caretX = byteToAbsX(field.CaretBytePos);
				const float caretWidth = std::max(1.0f, field.CaretWidth);
				const Vec2 caretCenter{ caretX + caretWidth * 0.5f, centerY };
				const Vec2 caretSize{ caretWidth, halfHeight * 2.0f };
				Instance44 caretInst(
					caretCenter,
					caretSize,
					0.0f,
					field.CaretColor,
					TextureHandle{},
					static_cast<short>(0),
					static_cast<std::uint8_t>(0),
					textDI + 1u);
				caretInst.HasClip = fieldHasClip;
				caretInst.ClipMin = fieldClipMin;
				caretInst.ClipMax = fieldClipMax;
				m_InstancesScratch.push_back(caretInst);
			}
			(void)tc;
		}

		// ── 5. Unified UI z-stack ────────────────────────────────────
		std::sort(m_InstancesScratch.begin(), m_InstancesScratch.end(),
			[](const Instance44& a, const Instance44& b) {
				if (a.SortingLayer != b.SortingLayer)
					return a.SortingLayer < b.SortingLayer;
				if (a.SortingOrder != b.SortingOrder)
					return a.SortingOrder < b.SortingOrder;
				return a.DrawIndex < b.DrawIndex;
			});
		std::sort(m_TextScratch.begin(), m_TextScratch.end(),
			[](const TextDrawCmd& a, const TextDrawCmd& b) {
				if (a.SortingLayer != b.SortingLayer)
					return a.SortingLayer < b.SortingLayer;
				if (a.SortingOrder != b.SortingOrder)
					return a.SortingOrder < b.SortingOrder;
				return a.DrawIndex < b.DrawIndex;
			});

		// ── bgfx submit setup ────────────────────────────────────────
		// Acquire (or reuse) a UI view-id keyed on the currently-bound
		// framebuffer. The editor renders two FBOs per frame (Game View
		// + Editor View) — each gets its own UI overlay view so neither
		// overwrites the other's setViewFrameBuffer when GuiRenderer
		// runs again later in the frame.
		auto& bgfxState = GetBgfxState(this);
		const bgfx::FrameBufferHandle currentFbo = BgfxBackend::CurrentFramebuffer();
		const bgfx::ViewId uiView = AcquireUiViewForFbo(bgfxState, currentFbo);

		uint16_t vpX = 0, vpY = 0, vpW = 0, vpH = 0;
		BgfxBackend::CurrentViewRect(vpX, vpY, vpW, vpH);
		if (vpW == 0 || vpH == 0) return;

		bgfx::setViewName(uiView, "GuiRendererUi");
		bgfx::setViewMode(uiView, bgfx::ViewMode::Sequential);
		bgfx::setViewClear(uiView, BGFX_CLEAR_NONE);
		bgfx::setViewRect(uiView, vpX, vpY, vpW, vpH);
		bgfx::setViewFrameBuffer(uiView, currentFbo);
		bgfx::setViewTransform(uiView, nullptr, glm::value_ptr(mvp));

		if (!BgfxSpriteResources::IsReady()) return;
		const bgfx::ProgramHandle prog = BgfxSpriteResources::GetProgram();
		if (!bgfx::isValid(prog)) return;

		const auto& instanceLayout = BgfxSpriteResources::GetInstanceLayout();
		const bgfx::VertexBufferHandle quadVbh = BgfxSpriteResources::GetQuadVbh();
		const bgfx::IndexBufferHandle  quadIbh = BgfxSpriteResources::GetQuadIbh();
		const bgfx::UniformHandle      sampler = BgfxSpriteResources::GetSamplerAlbedo();
		(void)instanceLayout;

		const TextureHandle defaultTexture = TextureManager::GetDefaultTexture(DefaultTexture::Square);
		auto resolveHandle = [&](TextureHandle h) {
			return TextureManager::IsValid(h) ? h : defaultTexture;
		};

		auto clipsEqual = [](bool aHas, const Vec2& aMin, const Vec2& aMax,
			bool bHas, const Vec2& bMin, const Vec2& bMax) -> bool {
			if (aHas != bHas) return false;
			if (!aHas) return true;
			return aMin.x == bMin.x && aMin.y == bMin.y
				&& aMax.x == bMax.x && aMax.y == bMax.y;
		};

		// Project a UI-space clip rect through MVP into NDC, then map NDC
		// to pixel coords via the cached UI view rect. bgfx::setScissor
		// uses top-left coords in framebuffer pixels — Y is flipped from
		// the GL impl's bottom-left mapping.
		//
		// Returns a bgfx scissor-cache index so TextRenderer can re-set
		// the same scissor before each of its multi-atlas submits.
		// 0xFFFF = no scissor (bgfx sentinel that disables clipping).
		auto applyScissor = [&](bool hasClip, const Vec2& clipMin, const Vec2& clipMax) -> uint16_t {
			if (!hasClip) {
				bgfx::setScissor(0xFFFFu);
				return 0xFFFFu;
			}
			const glm::vec4 corners[4] = {
				mvp * glm::vec4(clipMin.x, clipMin.y, 0.0f, 1.0f),
				mvp * glm::vec4(clipMax.x, clipMin.y, 0.0f, 1.0f),
				mvp * glm::vec4(clipMin.x, clipMax.y, 0.0f, 1.0f),
				mvp * glm::vec4(clipMax.x, clipMax.y, 0.0f, 1.0f),
			};
			const float invW0 = corners[0].w != 0.0f ? 1.0f / corners[0].w : 1.0f;
			float ndcMinX = corners[0].x * invW0;
			float ndcMaxX = ndcMinX;
			float ndcMinY = corners[0].y * invW0;
			float ndcMaxY = ndcMinY;
			for (int i = 1; i < 4; ++i) {
				const float invW = corners[i].w != 0.0f ? 1.0f / corners[i].w : 1.0f;
				const float x = corners[i].x * invW;
				const float y = corners[i].y * invW;
				ndcMinX = std::min(ndcMinX, x);
				ndcMaxX = std::max(ndcMaxX, x);
				ndcMinY = std::min(ndcMinY, y);
				ndcMaxY = std::max(ndcMaxY, y);
			}
			ndcMinX = std::clamp(ndcMinX, -1.0f, 1.0f);
			ndcMaxX = std::clamp(ndcMaxX, -1.0f, 1.0f);
			ndcMinY = std::clamp(ndcMinY, -1.0f, 1.0f);
			ndcMaxY = std::clamp(ndcMaxY, -1.0f, 1.0f);

			const float fVpW = static_cast<float>(vpW);
			const float fVpH = static_cast<float>(vpH);
			const float xMin = vpX + (ndcMinX * 0.5f + 0.5f) * fVpW;
			const float xMax = vpX + (ndcMaxX * 0.5f + 0.5f) * fVpW;
			const float yMinTop = vpY + (1.0f - (ndcMaxY * 0.5f + 0.5f)) * fVpH;
			const float yMaxTop = vpY + (1.0f - (ndcMinY * 0.5f + 0.5f)) * fVpH;
			const float sx = std::floor(std::min(xMin, xMax));
			const float sy = std::floor(std::min(yMinTop, yMaxTop));
			const float sw = std::ceil(std::abs(xMax - xMin));
			const float sh = std::ceil(std::abs(yMaxTop - yMinTop));
			return bgfx::setScissor(
				static_cast<uint16_t>(std::max(0.0f, sx)),
				static_cast<uint16_t>(std::max(0.0f, sy)),
				static_cast<uint16_t>(std::max(0.0f, sw)),
				static_cast<uint16_t>(std::max(0.0f, sh)));
		};

		// Submit a contiguous span of pre-sorted images, batching adjacent
		// same-texture / same-clip instances into one bgfx::submit through
		// a transient instance data buffer.
		auto drawImageSpan = [&](const Instance44* base, size_t count) {
			if (count == 0) return;

			size_t runStart = 0;
			while (runStart < count) {
				const TextureHandle runHandle = resolveHandle(base[runStart].TextureHandle);
				const bool runHasClip = base[runStart].HasClip;
				const Vec2 runClipMin = base[runStart].ClipMin;
				const Vec2 runClipMax = base[runStart].ClipMax;

				size_t runEnd = runStart + 1;
				while (runEnd < count) {
					const TextureHandle h = resolveHandle(base[runEnd].TextureHandle);
					if (!(h.index == runHandle.index && h.generation == runHandle.generation)) break;
					if (!clipsEqual(runHasClip, runClipMin, runClipMax,
						base[runEnd].HasClip, base[runEnd].ClipMin, base[runEnd].ClipMax)) break;
					++runEnd;
				}
				const uint32_t runCount = static_cast<uint32_t>(runEnd - runStart);

				if (bgfx::getAvailInstanceDataBuffer(runCount,
					sizeof(BgfxSpriteResources::SpriteInstance)) < runCount)
				{
					AIM_CORE_WARN_TAG("GuiRenderer",
						"Instance buffer exhausted; dropped {} UI quads this frame", runCount);
					break;
				}
				bgfx::InstanceDataBuffer idb{};
				bgfx::allocInstanceDataBuffer(&idb, runCount,
					sizeof(BgfxSpriteResources::SpriteInstance));
				BgfxSpriteResources::SpriteInstance* dst =
					reinterpret_cast<BgfxSpriteResources::SpriteInstance*>(idb.data);
				for (uint32_t k = 0; k < runCount; ++k) {
					BgfxSpriteResources::EncodeInstance44(base[runStart + k], dst[k]);
				}

				(void)applyScissor(runHasClip, runClipMin, runClipMax);

				if (Texture2D* tex = TextureManager::GetTexture(runHandle); tex && tex->IsValid()) {
					const uint16_t texIdx = static_cast<uint16_t>(tex->GetHandle() - 1u);
					bgfx::setTexture(0, sampler, bgfx::TextureHandle{ texIdx });
				}

				bgfx::setVertexBuffer(0, quadVbh);
				bgfx::setIndexBuffer(quadIbh);
				bgfx::setInstanceDataBuffer(&idb);
				bgfx::setState(0
					| BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_BLEND_ALPHA
					| BGFX_STATE_MSAA);
				bgfx::submit(uiView, prog);

				runStart = runEnd;
			}
		};

		auto drawTextSpan = [&](const TextDrawCmd* base, size_t count) {
			if (count == 0) return;
			if (!m_TextRenderer || !m_TextRenderer->IsInitialized()) return;

			size_t runStart = 0;
			while (runStart < count) {
				const bool runHasClip = base[runStart].HasClip;
				const Vec2 runClipMin = base[runStart].ClipMin;
				const Vec2 runClipMax = base[runStart].ClipMax;

				size_t runEnd = runStart + 1;
				while (runEnd < count) {
					if (!clipsEqual(runHasClip, runClipMin, runClipMax,
						base[runEnd].HasClip, base[runEnd].ClipMin, base[runEnd].ClipMax)) break;
					++runEnd;
				}

				const uint16_t scissorCache = applyScissor(runHasClip, runClipMin, runClipMax);
				m_TextRenderer->RenderInstances(std::span<const TextDrawCmd>(
					base + runStart, runEnd - runStart), mvp,
					static_cast<unsigned short>(uiView),
					scissorCache);

				runStart = runEnd;
			}
		};

		// Merge-walk sorted images + text by (Layer, Order, DrawIndex). Same
		// stable interleave as the GL impl so a label authored below its
		// background image still renders on top.
		const auto imageKey = [](const Instance44& v) {
			return std::tuple<int, int, std::uint32_t>{
				static_cast<int>(v.SortingLayer),
				static_cast<int>(v.SortingOrder),
				v.DrawIndex };
		};
		const auto textKey = [](const TextDrawCmd& v) {
			return std::tuple<int, int, std::uint32_t>{
				static_cast<int>(v.SortingLayer),
				static_cast<int>(v.SortingOrder),
				v.DrawIndex };
		};

		size_t ii = 0;
		size_t ti = 0;
		const size_t iCount = m_InstancesScratch.size();
		const size_t tCount = m_TextScratch.size();

		while (ii < iCount && ti < tCount) {
			const auto ik = imageKey(m_InstancesScratch[ii]);
			const auto tk = textKey(m_TextScratch[ti]);

			if (ik <= tk) {
				size_t end = ii + 1;
				while (end < iCount && imageKey(m_InstancesScratch[end]) == ik) ++end;
				drawImageSpan(m_InstancesScratch.data() + ii, end - ii);
				ii = end;
			}
			else {
				size_t end = ti + 1;
				while (end < tCount && textKey(m_TextScratch[end]) == tk) ++end;
				drawTextSpan(m_TextScratch.data() + ti, end - ti);
				ti = end;
			}
		}
		if (ii < iCount) {
			drawImageSpan(m_InstancesScratch.data() + ii, iCount - ii);
		}
		if (ti < tCount) {
			drawTextSpan(m_TextScratch.data() + ti, tCount - ti);
		}

		// Make sure the view executes even if no spans submitted (e.g.
		// empty UI scene) — bgfx::touch keeps the clear/setup live.
		bgfx::touch(uiView);
	}

} // namespace Axiom
