#include "pch.hpp"
#include "GuiRenderer.hpp"

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
#include "Graphics/Shader.hpp"
#include "Graphics/Instance44.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Graphics/Text/TextRenderer.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/UIDrawOrder.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Systems/UILayoutSystem.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace Axiom {
	namespace {

		// ── Input field overlay helpers ───────────────────────────
		// Mirror what UIEventSystem uses to position the caret, so the
		// rendered caret line and the byte offset that hit-testing maps
		// to land on the same X coord. Pulled into anon-namespace
		// helpers here so GuiRenderer doesn't have to depend on internal
		// UIEventSystem code.

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

		// Width in atlas units up to (but not including) `targetByte`.
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

		// Resolve a focused/selecting input field's text-child layout so
		// the caret + selection visuals line up exactly with the rendered
		// glyph baseline. Returns Valid=false when the text child or its
		// font isn't ready to draw — caller skips the overlay in that
		// case (e.g. fonts still loading).
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

			// Match the renderer's text-scale rule: the text rect's world
			// scale grows the rendered font and pixel pads, and the caret /
			// selection geometry has to follow so the bar still hugs the
			// (now larger) glyphs.
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

		// Walk the entity's ancestor chain via HierarchyComponent and
		// intersect every Mask ancestor's resolved AABB into a single
		// clip rect. Returns false (no clip) when no Mask ancestor
		// exists. Rotated mask rects fall back to their resolved AABB
		// — the bounding box of the rotated rect — since glScissor is
		// axis-aligned.
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

	namespace {
		// Resolve the UI canvas in pixel units. Mirrors UILayoutSystem's
		// own pick — UIRegion when an editor host has published one,
		// main viewport otherwise. Returns false when neither is
		// available yet (e.g. first frame before window init).
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
	}

	float GuiRenderer::ComputeWorldUIPixelScale() {
		int canvasW = 0;
		int canvasH = 0;
		if (!ResolveUICanvasSize(canvasW, canvasH)) {
			return 0.01f;
		}
		// Match the runtime camera's pixel-to-world ratio so a UI rect
		// that fills the canvas at runtime also fills the main camera's
		// viewport when viewed in the editor at the same pose. Falls
		// back to 100 px / world unit when no main camera exists yet —
		// keeps a 1080p canvas around 10 world units tall, which is
		// close to the editor camera's default 10-unit viewport.
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
		int w = 0;
		int h = 0;
		if (!ResolveUICanvasSize(w, h)) {
			return;
		}

		const float halfW = 0.5f * w;
		const float halfH = 0.5f * h;
		const float zNear = -1.0f;
		const float zFar = 1.0f;

		const glm::mat4 mvp = glm::ortho(-halfW, +halfW, -halfH, +halfH, zNear, zFar);
		CollectAndDraw(scene, mvp, halfW, halfH);
	}

	void GuiRenderer::RenderScene(const Scene& scene, const glm::mat4& worldVP, float pixelToWorldScale) {
		if (!m_IsInitialized) {
			return;
		}

		AIM_ASSERT(m_SpriteShader.IsValid(), AxiomErrorCode::InvalidHandle, "Invalid Sprite 2D Shader");

		// Same layout pass as the screen-space path — resolves rects in
		// centered-pixel coords; the world-space mapping is just a scale
		// applied below in the projection.
		ComputeUILayout(const_cast<Scene&>(scene));

		int w = 0;
		int h = 0;
		if (!ResolveUICanvasSize(w, h)) {
			return;
		}

		// Scale UI pixel coords into world units, then run them through
		// the caller-supplied world VP. With this composition, a rect
		// resolved to e.g. (-100, -50) — (100, 50) pixels lands at
		// (pixelToWorldScale * those) world units — exactly where the
		// gizmo in DrawEditorComponentGizmos draws when it applies the
		// same scale to the same resolved coords.
		const glm::mat4 uiToWorld = glm::scale(glm::mat4(1.0f),
			glm::vec3(pixelToWorldScale, pixelToWorldScale, 1.0f));
		const glm::mat4 mvp = worldVP * uiToWorld;

		// halfW/halfH are still in canvas pixels — CollectAndDraw uses
		// them for cursor → UI-space conversion in the dropdown popup
		// hover code path. Editor view doesn't have meaningful UI input
		// (UIEventSystem doesn't tick when not playing) so a slightly
		// off mouseUi only affects cosmetic hover on already-open
		// popups, which is harmless.
		const float halfW = 0.5f * w;
		const float halfH = 0.5f * h;
		CollectAndDraw(scene, mvp, halfW, halfH);
	}

	void GuiRenderer::CollectAndDraw(const Scene& scene, const glm::mat4& mvp,
		float halfW, float halfH)
	{
		entt::registry& registry = const_cast<entt::registry&>(scene.GetRegistry());

		// ── 1. Build hierarchy draw order ────────────────────────────
		// Shared with UIEventSystem's hit-test so the rendered z-stack
		// and the picked-front-most-Interactable stack agree. Roots walk
		// oldest-first; children follow HierarchyComponent::Children order
		// (later sibling = on top), so "further down the hierarchy panel
		// = on top of earlier entries" holds at every level.
		// m_DrawOrder is a member so its capacity persists across frames.
		m_DrawOrder.clear();
		UIDrawOrder::Build(registry, m_DrawOrder);

		// Next free DrawIndex slot after the hierarchy walk — dropdown
		// popups below allocate a contiguous block from here so their
		// rows sort above every authored widget without colliding with
		// any walked entity's index.
		int counter = m_DrawOrder.empty()
			? 0
			: m_DrawOrder.back().second + UIDrawOrder::k_HierarchyStep;

		// ── 2. Image instances ───────────────────────────────────────
		// The sprite shader treats `iSpritePos` as the *center* of the
		// quad and rotates around it. We compute the rect's geometric
		// centre, then for rotated rects we rotate that centre around
		// the resolved pivot so rotation pivots correctly even when the
		// pivot is non-centered.
		m_InstancesScratch.clear();
		m_InstancesScratch.reserve(m_DrawOrder.size());

		for (const auto& [entity, drawIndex] : m_DrawOrder) {
			if (!registry.all_of<RectTransform2DComponent, ImageComponent>(entity)) continue;

			// Mask entities with ShowMaskGraphic=false suppress their own
			// image draw — the entity exists purely to clip descendants.
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

			// Honour the component's authored sort fields. The hierarchy
			// walk index (drawIndex) is the explicit tiebreaker so that
			// later siblings draw on top of earlier ones — i.e. a Slider's
			// Handle child (added second) overlays its Fill child (added
			// first) without the author having to set sort fields.
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

		// ── 2b. Circular slider (textured + procedural fill) ────────
		// Background is one textured quad with the engine's circle.png
		// (a solid disc with alpha=0 outside, alpha=1 inside) — replaces
		// the old 64-quad procedural-tangent approach which (a) cost
		// many instances per slider and (b) suffered from sub-pixel
		// seams on tight rings. The fill arc still emits triangle-fan
		// segments because no built-in texture supports a partial
		// sweep, but with a default segment cap of 32 (down from 64)
		// the fill cost is half what it was while remaining visually
		// identical at typical sizes. Hit-test (annulus check) and drag
		// math live in UIEventSystem.
		const TextureHandle defaultWhite  = TextureManager::GetDefaultTexture(DefaultTexture::Square);
		const TextureHandle defaultCircle = TextureManager::GetDefaultTexture(DefaultTexture::Circle);
		const TextureHandle bgTexture     = defaultCircle.IsValid() ? defaultCircle : defaultWhite;

		// Emits a triangle-fan-like sweep made of N narrow quads radiating
		// from `centre`. Each quad spans one angular slice; the inner edge
		// is anchored near the centre and the outer edge sits at
		// `outerRadius`. We render with the default-white texture so the
		// per-instance Color tint gives a flat fill colour. This is the
		// "value indicator" overlay drawn on top of the background disc.
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
			// Quad height = ~outer radius (anchored at centre, extending
			// outward). Width = chord at the outer edge so the slice
			// covers one angular step without overlap. 1.02× pads against
			// sub-pixel seams between adjacent slices.
			const float chord = 2.0f * outerRadius * std::sin(0.5f * std::abs(segAngle));
			const float quadWidth = chord * 1.02f;

			for (int i = 0; i < segments; ++i) {
				const float midAngle = startRad + (static_cast<float>(i) + 0.5f) * segAngle + parentRot;
				// Position the quad's centre halfway between centre and
				// outer edge so its half-height = outerRadius/2 reaches
				// from centre to outerRadius.
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
				// Quad's height axis is the radial direction (centre →
				// outer edge). Tangent (= width axis) is at midAngle + π/2.
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

			// Resolve the disc's centre. Use the rect's geometric centre
			// rather than ResolvedPivot — sliders authored with non-(0.5,
			// 0.5) pivot would otherwise render off-axis. GetCenter()
			// returns the bounds midpoint regardless of pivot.
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

			// Background: one textured quad with the bundled circle.png.
			// The quad covers the full slider rect; the texture's alpha
			// channel masks everything outside the disc so a square
			// rect produces a clean circle without any procedural ring
			// emission. Tinted with BackgroundColor.
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

			// Fill: pie-slice from start angle to (start + sweep * Value).
			// Skipped when Value is at minimum so a fresh slider starts
			// visually empty. Capped at 32 segments (down from 64) — at
			// the typical 200 px slider that's still subpixel-smooth.
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

			// Bake the atlas at the on-screen size: FontSize × the rect's
			// composed world scale. Without this, scaling a parent rect by
			// 2× kept the atlas at the authored FontSize and the renderer
			// upscaled glyphs at draw time → blurry. With it, the cached
			// atlas matches the rendered raster size 1:1 and looks the
			// same as bumping FontSize directly. FontManager quantizes to
			// 1 px so animated scales don't pathologically thrash the
			// cache (multiple sliders with the same effective px share an
			// atlas slot).
			const float effectivePixelSize = text.FontSize * std::max(0.01f, std::abs(rect.Scale.x));
			Font* font = TextRenderer::ResolveFontAtPixelSize(text, effectivePixelSize);
			if (!font || !font->IsLoaded()) continue;
			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const Vec2 size{ tr.x - bl.x, tr.y - bl.y };

			// Anchor the text's baseline near the vertical centre of the
			// rect, biased downward by ~25% of the font height so glyphs
			// with descenders still sit inside the box. For multi-line
			// strings this matches the first line; subsequent lines
			// extend below.
			//
			// rect.Scale is the rect's world scale (parent ⊙ local). Text
			// Scale is a single scalar so we can't honour non-uniform
			// scales without skewing glyphs — use the X component as the
			// uniform factor. Any pixel-domain offset that should grow
			// with the rect (font-height bias, edge padding) gets the
			// same multiplier so the text stays visually centred when
			// the rect scales up.
			const float uniformScale = rect.Scale.x;
			const float bakedSize = font->GetPixelSize() > 0.0f ? font->GetPixelSize() : text.FontSize;
			const float drawScale = (text.FontSize / bakedSize) * uniformScale;

			// Margin insets are authored in pixels; multiply by the rect's
			// uniform scale so a margin grows visually with the rect (same
			// treatment as font-height bias, edge padding).
			const float marginLeftWorld   = text.Margin.x * uniformScale;
			const float marginTopWorld    = text.Margin.y * uniformScale;
			const float marginRightWorld  = text.Margin.z * uniformScale;
			const float marginBottomWorld = text.Margin.w * uniformScale;
			(void)marginBottomWorld; // bottom inset reserved for future v-align

			// Baseline still sits near the rect's vertical centre, but the
			// margin shifts it: positive top inset pushes the baseline DOWN
			// (text starts further below the rect's top edge).
			float baselineY = bl.y + size.y * 0.5f
				- text.FontSize * 0.35f * uniformScale
				- marginTopWorld;

			float originX;
			switch (text.HAlign) {
			case TextAlignment::Center: originX = bl.x + size.x * 0.5f;                      break;
			case TextAlignment::Right:  originX = tr.x - 4.0f * uniformScale - marginRightWorld; break; // tiny right pad + margin
			case TextAlignment::Left:
			default:                    originX = bl.x + 4.0f * uniformScale + marginLeftWorld;  break; // tiny left pad + margin
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
			// UI text inherits the host rect's width as the wrap area.
			// The renderer expects the wrap width in atlas-pixel units
			// (the same domain glyph advances live in), but `size.x` is
			// the rect's world-space width post-`uniformScale`; divide
			// back out so the wrap test happens before `Scale` is
			// reapplied inside EmitText. Trim ~8 px (4 px of left-pad +
			// 4 px of right-pad) matching the alignment biases above
			// so wrapped lines stay flush against the same insets the
			// unwrapped path uses. The margin (left + right) further
			// reduces the wrap area so authored insets actually narrow
			// the line-break test. The historical explicit WrapWidth
			// override was removed — Margin already exposes the same
			// inset and the dual-source wrap area was confusing in the
			// inspector.
			cmd.Wrap = text.WrapMode;
			if (text.WrapMode != TextWrapMode::None) {
				const float padPixels = 8.0f * uniformScale;
				const float marginPixels = marginLeftWorld + marginRightWorld;
				cmd.WrapWidthPixels = uniformScale > 0.0f
					? std::max(0.0f, (size.x - padPixels - marginPixels) / uniformScale)
					: 0.0f;
			}
			// Honour the component's authored sort fields so text and
			// images compete in one z-stack. Default (0,0) ties an
			// Image at (0,0) — the merge-walk's tiebreaker keeps text
			// drawing on top within the same key, preserving the
			// historical "label above its panel" default. The hierarchy
			// index makes sibling text entities respect authored order.
			cmd.SortingOrder = text.SortingOrder;
			cmd.SortingLayer = text.SortingLayer;
			cmd.DrawIndex = static_cast<std::uint32_t>(drawIndex);
			cmd.HasClip = ResolveClipForEntity(registry, entity, cmd.ClipMin, cmd.ClipMax);
			// Honour the rect's composed rotation so a label under a
			// rotated parent rotates with it instead of staying upright.
			// EmitText short-circuits on Rotation==0 so unrotated text
			// (the dominant case) keeps the original hot path.
			cmd.Rotation = rect.Rotation;
			cmd.Pivot = rect.ResolvedPivot;
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
		// Mouse held this frame — pressed-state for popup rows requires
		// both hover AND held. Without this we'd flicker every option
		// to "pressed" on the down edge then back to "hovered" the
		// frame after.
		const bool mouseHeld = app && app->GetInput().GetMouse(MouseButton::Left);

		// Pick the row colour for a popup option using the dropdown's
		// configured per-state palette. Precedence: pressed > hovered >
		// selected > normal. Alpha == 0 in any slot means "no override"
		// — the row falls through to the next-lower precedence, and
		// finally to PopupBackgroundColor when every override is unset.
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
			// Mirror the entity-driven path: the dropdown rect's world
			// scale also drives the popup option label scale, so a
			// scaled-up dropdown grows its option text to match.
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
					static_cast<std::uint8_t>(10), // popup layer
					static_cast<std::uint32_t>(popupDraw + i));

				// Option label.
				TextDrawCmd cmd;
				cmd.FontPtr = dropdownFont;
				cmd.Text = std::string_view(dd.Options[i]);
				cmd.X = rowBL.x + 8.0f * uniformScale; // small left pad, scaled with rect
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
		// Draw a translucent quad behind the selected text and a thin
		// vertical bar at the caret position. We do this here so the
		// overlay sorts into the same z-stack as the rest of the UI:
		//
		//   field-bg (DrawIndex N) → selection-bg (N+1) → text-child
		//     (N+k_HierarchyStep) → caret (text+1)
		//
		// CollectDrawOrder gives every entity a 4-wide DrawIndex slot so
		// we can slip the selection between the field's parent rect and
		// its text child without bumping anyone's authored sort order.
		const float elapsedSeconds = app ? app->GetTime().GetElapsedTime() : 0.0f;

		// Build a quick entity → DrawIndex map for the overlay lookup.
		// m_DrawOrder is in the dozens for typical UIs, so a flat scan is
		// fine, but the map keeps the overlay pass O(N+M). m_DrawIndexByEntity
		// is a member so the bucket array survives between frames.
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

			// If the field sits under a Mask ancestor, the selection +
			// caret quads need the same scissor clipping as the field's
			// own image — otherwise a focused field scrolled out of a
			// ScrollView's viewport would still flash a caret outside
			// the masked area.
			Vec2 fieldClipMin{};
			Vec2 fieldClipMax{};
			const bool fieldHasClip = ResolveClipForEntity(registry, entity, fieldClipMin, fieldClipMax);

			// Sample the underlying TextRenderer for color sourcing /
			// text-string lookup. We measure against field.Text (not
			// tc.Text) because tc.Text might be the placeholder. Secret
			// fields render one '*' per codepoint, so the caret /
			// selection geometry has to walk the masked string instead
			// — otherwise the bar lands at the position of the real
			// glyph but the user sees the mask.
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

			// Real-Text byte → measureText byte. Identity in non-secret
			// mode; in secret mode each codepoint of field.Text becomes
			// one byte of measureText, so it's the codepoint count up
			// to byteInOriginal.
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

			// Caret / selection vertical extent: hug the text's font
			// height around the baseline. Slightly bigger than FontSize
			// so the bar reads cleanly against tall glyphs.
			const float verticalPad = 2.0f;
			const float halfHeight = layout.FontSize * 0.5f + verticalPad;
			const float centerY = layout.BL.y + (layout.TR.y - layout.BL.y) * 0.5f;

			// Width-from-origin to the caret and to the anchor, both in
			// screen pixels (atlas-units * scale). Center / right-aligned
			// text shifts visually so anchorAbsX/caretAbsX bake the
			// alignment offset into the absolute X position.
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
						fieldDI + 1u); // sits between field bg and text child
					selInst.HasClip = fieldHasClip;
					selInst.ClipMin = fieldClipMin;
					selInst.ClipMax = fieldClipMax;
					m_InstancesScratch.push_back(selInst);
				}
			}

			// Caret only when focused — the selection is allowed to
			// outlive focus visually so a programmatic SetSelection
			// call still highlights, but the blinking caret is always
			// scoped to "this field has keyboard focus". Blink rate
			// is per-field: 0 disables blinking entirely (caret stays
			// solid while focused), otherwise the rate sets the cycles-
			// per-second of the on/off square wave.
			//
			// Read-only fields suppress the caret entirely: it's the
			// canonical "you can type here" affordance, and showing it
			// on a field that rejects keystrokes is misleading. The
			// selection highlight still renders so Ctrl+C / Select-All
			// remain visible operations.
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
					textDI + 1u); // sits just after the rendered text
				caretInst.HasClip = fieldHasClip;
				caretInst.ClipMin = fieldClipMin;
				caretInst.ClipMax = fieldClipMax;
				m_InstancesScratch.push_back(caretInst);
			}
			(void)tc;
		}

		// ── 5. Unified UI z-stack ────────────────────────────────────
		// Images and text compete in one sort space keyed by
		// (SortingLayer, SortingOrder, DrawIndex). DrawIndex is the
		// hierarchy walk index — earlier siblings draw first, later
		// siblings on top. By making it an explicit third key (instead
		// of relying on stable_sort's input-order preservation) the
		// "later child renders above earlier child" contract holds even
		// if a future code path inserts instances out of walk order.
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

		// Capture the current GL viewport so we can map our centered
		// screen-space clip rects into window pixels for glScissor.
		// glViewport is whatever the caller set for this frame's
		// target — full window in runtime, sub-FBO in editor — so we
		// don't have to special-case either path.
		GLint glViewportRect[4] = { 0, 0, 0, 0 };
		glGetIntegerv(GL_VIEWPORT, glViewportRect);
		const float vpX = static_cast<float>(glViewportRect[0]);
		const float vpY = static_cast<float>(glViewportRect[1]);
		const float vpW = static_cast<float>(glViewportRect[2]);
		const float vpH = static_cast<float>(glViewportRect[3]);

		// glScissor takes window pixels, so we must project the clip
		// rect through the same MVP the geometry rides through and then
		// map NDC into the viewport. Naively mapping centered-pixel
		// space directly to vp pixels works only for the screen-space
		// RenderScene overload, where MVP is the centered ortho and
		// content sits at canvas pixels. The world-space overload
		// composes the editor camera's view-projection in front, so
		// the same clip pixels render at a different on-screen
		// position when the camera pans or zooms — that's the
		// "mask cuts the wrong region in editor view" bug. Projecting
		// the corners reduces to the old direct mapping when MVP is
		// the centered ortho, so the screen-space path stays exact.
		auto applyScissor = [&](bool hasClip, const Vec2& clipMin, const Vec2& clipMax) {
			if (!hasClip) {
				glDisable(GL_SCISSOR_TEST);
				return;
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
			// Clamp to NDC [-1, 1] before mapping so a clip rect that
			// extends past the on-screen edge can't push glScissor
			// outside the viewport (negative size would silently disable
			// the test on some drivers).
			ndcMinX = std::clamp(ndcMinX, -1.0f, 1.0f);
			ndcMaxX = std::clamp(ndcMaxX, -1.0f, 1.0f);
			ndcMinY = std::clamp(ndcMinY, -1.0f, 1.0f);
			ndcMaxY = std::clamp(ndcMaxY, -1.0f, 1.0f);
			float xMin = vpX + (ndcMinX * 0.5f + 0.5f) * vpW;
			float xMax = vpX + (ndcMaxX * 0.5f + 0.5f) * vpW;
			float yMin = vpY + (ndcMinY * 0.5f + 0.5f) * vpH;
			float yMax = vpY + (ndcMaxY * 0.5f + 0.5f) * vpH;
			if (xMax < xMin) std::swap(xMin, xMax);
			if (yMax < yMin) std::swap(yMin, yMax);
			const GLint sx = static_cast<GLint>(std::floor(xMin));
			const GLint sy = static_cast<GLint>(std::floor(yMin));
			const GLsizei sw = static_cast<GLsizei>(std::max(0.0f, std::ceil(xMax - xMin)));
			const GLsizei sh = static_cast<GLsizei>(std::max(0.0f, std::ceil(yMax - yMin)));
			glEnable(GL_SCISSOR_TEST);
			glScissor(sx, sy, sw, sh);
		};

		// True when two clip-rect descriptors are bit-equal — used by
		// the per-span batching to break runs at clip-state changes so
		// each sub-batch can drive its own glScissor.
		auto clipsEqual = [](bool aHas, const Vec2& aMin, const Vec2& aMax,
			bool bHas, const Vec2& bMin, const Vec2& bMax) -> bool {
			if (aHas != bHas) return false;
			if (!aHas) return true;
			return aMin.x == bMin.x && aMin.y == bMin.y
				&& aMax.x == bMax.x && aMax.y == bMax.y;
		};

		// QuadMesh's per-instance attributes (Position/Scale/Rotation/Color)
		// live in an instanced VBO at locations 2-5 with divisor=1, so we
		// MUST upload + DrawInstanced — a non-instanced Draw would read
		// instance index 0 for every quad and collapse them all to the
		// zero-initialized buffer (in particular Scale=(0,0) → invisible).
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

		// Draw a contiguous slice of pre-sorted images, batching adjacent
		// same-texture instances into one upload+draw. The sprite shader /
		// quad mesh are bound on entry and unbound on exit so the merge
		// walk can interleave image and text segments without leaving
		// stale GL state behind for the next text segment to inherit.
		// Runs also break on clip-rect change so each sub-run draws under
		// its own glScissor — descendants of a Mask entity get their
		// rendering clipped to the mask's resolved AABB.
		auto drawImageSpan = [&](const Instance44* base, size_t count) {
			if (count == 0) return;
			m_SpriteShader.Bind();
			m_SpriteShader.SetMVP(mvp);
			m_QuadMesh.Bind();
			glActiveTexture(GL_TEXTURE0);

			size_t runStart = 0;
			while (runStart < count) {
				TextureHandle runHandle = resolveHandle(base[runStart].TextureHandle);
				const bool runHasClip = base[runStart].HasClip;
				const Vec2 runClipMin = base[runStart].ClipMin;
				const Vec2 runClipMax = base[runStart].ClipMax;

				size_t runEnd = runStart + 1;
				while (runEnd < count) {
					if (!(resolveHandle(base[runEnd].TextureHandle) == runHandle)) break;
					if (!clipsEqual(runHasClip, runClipMin, runClipMax,
						base[runEnd].HasClip, base[runEnd].ClipMin, base[runEnd].ClipMax)) break;
					++runEnd;
				}

				applyScissor(runHasClip, runClipMin, runClipMax);

				Texture2D* texture = TextureManager::GetTexture(runHandle);
				if (texture && texture->IsValid()) {
					texture->Submit(0);
				}
				else {
					glBindTexture(GL_TEXTURE_2D, 0);
				}

				m_QuadMesh.UploadInstances(std::span<const Instance44>(
					base + runStart, runEnd - runStart));
				m_QuadMesh.DrawInstanced(runEnd - runStart);

				runStart = runEnd;
			}

			m_QuadMesh.Unbind();
			m_SpriteShader.Unbind();
		};

		auto drawTextSpan = [&](const TextDrawCmd* base, size_t count) {
			if (count == 0) return;
			if (!m_TextRenderer || !m_TextRenderer->IsInitialized()) return;

			// Sub-batch by clip rect for the same scissor reasoning as
			// drawImageSpan — each homogeneous-clip slice gets its own
			// scissor before the TextRenderer dispatch.
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

				applyScissor(runHasClip, runClipMin, runClipMax);
				m_TextRenderer->RenderInstances(std::span<const TextDrawCmd>(
					base + runStart, runEnd - runStart), mvp);

				runStart = runEnd;
			}
		};

		// Merge-walk both sorted lists by (SortingLayer, SortingOrder, DrawIndex).
		// On a tie of all three, image runs draw first then text — that
		// preserves the "label sits on top of its panel" default for
		// matching keys while still letting authors flip the order by
		// raising image.SortingLayer above the text's. Including
		// DrawIndex here keeps the merge stable when sibling images and
		// text share Layer/Order: they interleave by hierarchy position.
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

		glBlendEquationSeparate(previousBlendEquationRgb, previousBlendEquationAlpha);
		glBlendFuncSeparate(previousBlendSrcRgb, previousBlendDstRgb, previousBlendSrcAlpha, previousBlendDstAlpha);
		wasScissorEnabled ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
		wasDepthEnabled ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
		wasBlendEnabled ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
	}
}
