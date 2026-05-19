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
#include "Core/Log.hpp"
#include "Core/MouseButton.hpp"
#include "Core/Time.hpp"
#include "Core/Window.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/Instance44.hpp"
#include "Graphics/SpriteResources.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Graphics/Text/TextRenderer.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/UIDrawOrder.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Systems/UILayoutSystem.hpp"

#include <webgpu/webgpu_cpp.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

// =============================================================================
// GuiRenderer — WebGPU (Dawn) implementation.
// -----------------------------------------------------------------------------
// CPU-side collection (hierarchy walk, dropdown popups, input-field overlays,
// mask-based clip resolve, sort by Layer/Order/DrawIndex) drives a single
// wgpu::RenderPassEncoder + persistent instance VBO + per-batch
// SetScissorRect.
//
// Scope: image quads (RectTransform2D + ImageComponent), dropdown option
// rows, circular slider geometry, input-field caret/selection overlays.
// Text is NOT rendered yet — the m_TextScratch collection still runs for
// sort ordering, but the TextRenderer::RenderInstances call is a no-op
// stub (the real text path lands later). The visible result: every UI
// widget shows its background + interactive elements, labels are blank.
//
// Per-GuiRenderer GPU state (instance buffer, uniform buffer, per-frame
// bind-group cache) lives in a TU-local side table keyed by `this`. The
// editor instantiates two GuiRenderers (Game View + Editor View FBOs),
// each with its own state entry.
// =============================================================================

namespace Index {

	namespace {
		// ── Input field overlay helpers ─────────────────────────────────────
		// Pure CPU, no backend deps. Byte-walk + MeasureUpToByteUI logic
		// drives the caret/selection rect placement.

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

		// ── WebGPU per-GuiRenderer GPU state ────────────────────────────────
		// Keyed by `this` so the editor's two GuiRenderers (Game View FBO +
		// Editor View FBO) don't trample each other's instance VBO or bind
		// group cache. Cleared on Shutdown.
		struct GuiRendererWebGPUState {
			wgpu::Buffer InstanceBuffer;
			uint32_t     InstanceBufferCapacity = 0;
			wgpu::Buffer UniformBuffer;
			// Keyed by Texture2D::GetHandle() (the raw WGPUTextureView pointer
			// cast to uint64_t under WebGPU). Matches Renderer2D's per-frame
			// cache shape so cache lookups are stable across renderer types.
			// Persisted across multiple RenderScene calls within a single
			// frame so the editor's two FBO renders (which both call
			// RenderScene with the same loaded scenes' textures) don't pay
			// CreateBindGroup twice per texture. LastClearFrameCount tracks
			// when we last invalidated the cache; the per-frame counter
			// flips on Application::GetTime().GetFrameCount() rolling over.
			//
			// CachedBindGroup also stores the sampler/view the bind group
			// was created against so a mid-frame filter change
			// (Texture2D::SetFilter rebuilds the GPU sampler) invalidates
			// the entry instead of replaying a stale bind group.
			struct CachedBindGroup {
				wgpu::BindGroup   Group;
				wgpu::Sampler     Sampler;
				wgpu::TextureView View;
			};
			std::unordered_map<uint64_t, CachedBindGroup> BindGroupsThisFrame;
			uint64_t LastClearFrameCount = static_cast<uint64_t>(-1);
			std::vector<WebGPUSpriteResources::SpriteInstance> EncodedScratch;
		};
		std::unordered_map<const GuiRenderer*, GuiRendererWebGPUState> g_State;

		GuiRendererWebGPUState& GetState(const GuiRenderer* self) {
			return g_State[self];
		}

		bool EnsureUniformBuffer(wgpu::Device device, GuiRendererWebGPUState& state) {
			if (state.UniformBuffer) return true;
			wgpu::BufferDescriptor desc{};
			desc.size  = 64;
			desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
			desc.label = "guirenderer-viewproj-ubo";
			state.UniformBuffer = device.CreateBuffer(&desc);
			return static_cast<bool>(state.UniformBuffer);
		}

		bool EnsureInstanceBuffer(wgpu::Device device, GuiRendererWebGPUState& state,
			uint32_t neededInstances)
		{
			if (state.InstanceBufferCapacity >= neededInstances && state.InstanceBuffer) return true;
			uint32_t newCapacity = state.InstanceBufferCapacity > 0 ? state.InstanceBufferCapacity : 256;
			while (newCapacity < neededInstances) newCapacity *= 2;

			wgpu::BufferDescriptor desc{};
			desc.size  = static_cast<uint64_t>(newCapacity)
				* sizeof(WebGPUSpriteResources::SpriteInstance);
			desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
			desc.label = "guirenderer-instance-vbo";
			wgpu::Buffer buf = device.CreateBuffer(&desc);
			if (!buf) {
				IDX_CORE_ERROR_TAG("GuiRenderer",
					"Failed to create instance buffer (cap={})", newCapacity);
				return false;
			}
			state.InstanceBuffer         = std::move(buf);
			state.InstanceBufferCapacity = newCapacity;
			return true;
		}

		wgpu::BindGroup ResolveBindGroup(wgpu::Device device, GuiRendererWebGPUState& state,
			TextureHandle handle)
		{
			Texture2D* tex = TextureManager::GetTexture(handle);
			if (!tex || !tex->IsValid()) return nullptr;
			const uint64_t poolId = tex->GetHandle();

			const auto lookup = WebGPUBackend::LookupTexture2D(poolId);
			if (!lookup.Valid || !lookup.View || !lookup.Sampler) return nullptr;

			auto cacheIt = state.BindGroupsThisFrame.find(poolId);
			if (cacheIt != state.BindGroupsThisFrame.end()) {
				const auto& cached = cacheIt->second;
				if (cached.Sampler.Get() == lookup.Sampler.Get()
					&& cached.View.Get() == lookup.View.Get()) {
					return cached.Group;
				}
				state.BindGroupsThisFrame.erase(cacheIt);
			}

			wgpu::BindGroupEntry entries[3] = {};
			entries[0].binding = 0;
			entries[0].buffer  = state.UniformBuffer;
			entries[0].offset  = 0;
			entries[0].size    = 64;
			entries[1].binding     = 1;
			entries[1].textureView = lookup.View;
			entries[2].binding = 2;
			entries[2].sampler = lookup.Sampler;

			wgpu::BindGroupDescriptor desc{};
			desc.layout     = WebGPUSpriteResources::GetBindGroupLayout();
			desc.entryCount = 3;
			desc.entries    = entries;
			desc.label      = "guirenderer-ui-bindgroup";

			wgpu::BindGroup bg = device.CreateBindGroup(&desc);
			if (!bg) return nullptr;
			state.BindGroupsThisFrame.emplace(poolId,
				GuiRendererWebGPUState::CachedBindGroup{ bg, lookup.Sampler, lookup.View });
			return bg;
		}

		bool ClipsEqual(bool aHas, const Vec2& aMin, const Vec2& aMax,
			bool bHas, const Vec2& bMin, const Vec2& bMax)
		{
			if (aHas != bHas) return false;
			if (!aHas) return true;
			return aMin.x == bMin.x && aMin.y == bMin.y
				&& aMax.x == bMax.x && aMax.y == bMax.y;
		}

		// Project a UI-space clip rect through MVP into NDC, then map NDC to
		// pixel coords (top-left origin to match WebGPU's SetScissorRect
		// convention). Returns the computed [x, y, w, h] in framebuffer pixels.
		// Caller checks `valid`; an invalid rect (NaN, zero-area, fully out
		// of bounds) leaves the pass with no scissor change.
		struct ScissorPx { uint32_t X, Y, W, H; bool Valid; };
		ScissorPx ComputeScissor(const glm::mat4& mvp,
			const Vec2& clipMin, const Vec2& clipMax,
			uint32_t targetW, uint32_t targetH)
		{
			ScissorPx out{};
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

			const float fW = static_cast<float>(targetW);
			const float fH = static_cast<float>(targetH);
			const float xMin = (ndcMinX * 0.5f + 0.5f) * fW;
			const float xMax = (ndcMaxX * 0.5f + 0.5f) * fW;
			// Y flip: NDC +Y up -> screen +Y down for top-left origin.
			const float yMinTop = (1.0f - (ndcMaxY * 0.5f + 0.5f)) * fH;
			const float yMaxTop = (1.0f - (ndcMinY * 0.5f + 0.5f)) * fH;
			const float sx = std::floor(std::min(xMin, xMax));
			const float sy = std::floor(std::min(yMinTop, yMaxTop));
			const float sw = std::ceil(std::abs(xMax - xMin));
			const float sh = std::ceil(std::abs(yMaxTop - yMinTop));
			if (sw <= 0.0f || sh <= 0.0f) return out;

			const uint32_t ix = static_cast<uint32_t>(std::max(0.0f, sx));
			const uint32_t iy = static_cast<uint32_t>(std::max(0.0f, sy));
			uint32_t iw = static_cast<uint32_t>(std::max(0.0f, sw));
			uint32_t ih = static_cast<uint32_t>(std::max(0.0f, sh));
			// Clamp to target bounds so SetScissorRect doesn't trip Dawn's
			// validation (the spec requires x+w <= targetW, y+h <= targetH).
			if (ix >= targetW || iy >= targetH) return out;
			if (ix + iw > targetW) iw = targetW - ix;
			if (iy + ih > targetH) ih = targetH - iy;
			out.X = ix; out.Y = iy; out.W = iw; out.H = ih; out.Valid = true;
			return out;
		}

	}  // anonymous namespace

	void GuiRenderer::Initialize() {
		if (m_IsInitialized) return;

		WebGPUSpriteResources::Acquire();

		m_TextRenderer = std::make_unique<TextRenderer>();
		m_TextRenderer->Initialize();

		(void)GetState(this);  // ensure per-instance state entry exists

		m_IsInitialized = true;
	}

	void GuiRenderer::Shutdown() {
		if (!m_IsInitialized) return;

		g_State.erase(this);

		if (m_TextRenderer) {
			m_TextRenderer->Shutdown();
			m_TextRenderer.reset();
		}
		WebGPUSpriteResources::Release();
		m_IsInitialized = false;
	}

	void GuiRenderer::BeginFrame(const SceneManager& sceneManager) {
		if (!m_IsInitialized || m_SkipBeginFrameRender) return;
		sceneManager.ForeachLoadedScene([&](const Scene& scene) { RenderScene(scene); });
	}
	void GuiRenderer::EndFrame() {
		// No-op — Present() flushes from Window::SwapBuffers.
	}

	float GuiRenderer::ComputeWorldUIPixelScale() {
		int canvasW = 0;
		int canvasH = 0;
		if (!ResolveUICanvasSize(canvasW, canvasH)) return 0.01f;
		if (Camera2DComponent* cam = Camera2DComponent::Main()) {
			const float worldHeight = 2.0f * cam->GetOrthographicSize() * cam->GetZoom();
			if (worldHeight > 0.0f) return worldHeight / static_cast<float>(canvasH);
		}
		return 0.01f;
	}

	void GuiRenderer::RenderScene(const Scene& scene) {
		if (!m_IsInitialized) return;
		ComputeUILayout(const_cast<Scene&>(scene));

		int w = 0, h = 0;
		if (!ResolveUICanvasSize(w, h)) return;

		const float halfW = 0.5f * w;
		const float halfH = 0.5f * h;
		const glm::mat4 mvp = glm::ortho(-halfW, +halfW, -halfH, +halfH, -1.0f, 1.0f);
		CollectAndDraw(scene, mvp, halfW, halfH);
	}

	void GuiRenderer::RenderScene(const Scene& scene,
		const glm::mat4& worldVP, float pixelToWorldScale)
	{
		if (!m_IsInitialized) return;
		ComputeUILayout(const_cast<Scene&>(scene));

		int w = 0, h = 0;
		if (!ResolveUICanvasSize(w, h)) return;

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
					0, 0,
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

		// ── 3. UI text instances (collection runs; submission is a Stage-6 no-op) ──
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
			// Stage 5: Font_WebGPU stub always reports IsLoaded()=false so we
			// never reach the cmd-emit path. Stage 6 swaps Font + TextRenderer
			// to the real WebGPU impls and the rest of this block becomes
			// live without further GuiRenderer changes.
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
			// Stage 5: dropdownFont is nullptr (stub FontManager). Option-row
			// quads still emit (they're textureless); labels skipped.
			const float bakedSize = (dropdownFont && dropdownFont->GetPixelSize() > 0.0f)
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

				if (dropdownFont && dropdownFont->IsLoaded()) {
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
			if (!layout.Valid) continue;  // stub Font path returns Valid=false

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
						selCenter, selSize, 0.0f,
						field.SelectionColor,
						TextureHandle{},
						0, 0,
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
					caretCenter, caretSize, 0.0f,
					field.CaretColor,
					TextureHandle{},
					0, 0,
					textDI + 1u);
				caretInst.HasClip = fieldHasClip;
				caretInst.ClipMin = fieldClipMin;
				caretInst.ClipMax = fieldClipMax;
				m_InstancesScratch.push_back(caretInst);
			}
			(void)tc;
		}

		// ── 5. Sort image and text instances by unified key ──────────
		// (SortingLayer, SortingOrder, DrawIndex). DrawIndex comes from
		// the hierarchy walk above, so equal (layer, order) pairs fall
		// back to hierarchy order — earlier-in-hierarchy renders behind
		// later. We need BOTH lists sorted by the same key before the
		// merge walk below interleaves their phases.
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

		if (m_InstancesScratch.empty() && m_TextScratch.empty()) {
			return;
		}

		// ── WebGPU submit setup ──────────────────────────────────────
		auto& state = GetState(this);
		// Clear the bind-group cache only when we cross a frame boundary,
		// not on every RenderScene call. The editor invokes RenderScene
		// twice per frame (once per FBO) plus once per loaded scene, so
		// per-call clearing forced CreateBindGroup to fire for each
		// texture on every call — a ~Nx multiplier for texture-heavy UI.
		// Per-frame clearing still releases the cache so a texture
		// destroyed between frames can't be sampled via a zombie bind
		// group: a Texture2D::Destroy mid-frame is caught by the
		// IsValid() guard inside ResolveBindGroup, and any stale entry
		// is dropped by the next frame's clear.
		const uint64_t currentFrame = app ? app->GetTime().GetFrameCount() : state.LastClearFrameCount;
		if (state.LastClearFrameCount != currentFrame) {
			state.BindGroupsThisFrame.clear();
			state.LastClearFrameCount = currentFrame;
		}

		auto target = WebGPUBackend::BeginRenderToCurrentTarget();
		if (!target.Valid) return;
		if (!WebGPUSpriteResources::IsReady()) return;

		wgpu::Device device = WebGPUBackend::GetDevice();
		wgpu::Queue  queue  = WebGPUBackend::GetQueue();
		if (!device || !queue) return;

		const size_t n = m_InstancesScratch.size();
		wgpu::RenderPipeline pipeline;
		if (n > 0) {
			pipeline = WebGPUSpriteResources::GetSpritePipeline(
				target.ColorFormat, target.HasDepth);
			if (!pipeline) {
				IDX_CORE_WARN_TAG("GuiRenderer",
					"No pipeline for color-format {} hasDepth={} — image quads skipped",
					static_cast<int>(target.ColorFormat), target.HasDepth);
				// Don't return — text might still want to render below.
			} else {
				if (!EnsureUniformBuffer(device, state)) return;
				queue.WriteBuffer(state.UniformBuffer, 0, glm::value_ptr(mvp), 64);

				if (!EnsureInstanceBuffer(device, state, static_cast<uint32_t>(n))) return;
				state.EncodedScratch.resize(n);
				for (size_t k = 0; k < n; ++k) {
					WebGPUSpriteResources::EncodeInstance44(m_InstancesScratch[k], state.EncodedScratch[k]);
				}
				queue.WriteBuffer(state.InstanceBuffer, 0,
					state.EncodedScratch.data(),
					n * sizeof(WebGPUSpriteResources::SpriteInstance));
			}
		}

		const TextureHandle defaultTexture = TextureManager::GetDefaultTexture(DefaultTexture::Square);
		auto resolveHandle = [&](TextureHandle h) {
			return TextureManager::IsValid(h) ? h : defaultTexture;
		};

		// ── Helper: submit one image-quad phase covering [start, end) ─
		// Opens a fresh render pass — multiple phases per frame are how
		// the merge walk interleaves image runs with text runs while
		// still letting each phase batch by texture run-length.
		auto submitImagePhase = [&](size_t start, size_t end) {
			if (!pipeline || start >= end) return;

			wgpu::CommandEncoder enc = WebGPUBackend::GetFrameEncoder();
			if (!enc) return;

			wgpu::RenderPassColorAttachment colorAtt{};
			colorAtt.view       = target.ColorView;
			colorAtt.loadOp     = wgpu::LoadOp::Load;
			colorAtt.storeOp    = wgpu::StoreOp::Store;
			colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

			wgpu::RenderPassDepthStencilAttachment depthAtt{};
			if (target.HasDepth) {
				depthAtt.view              = target.DepthView;
				depthAtt.depthLoadOp       = wgpu::LoadOp::Load;
				depthAtt.depthStoreOp      = wgpu::StoreOp::Store;
				depthAtt.stencilLoadOp     = wgpu::LoadOp::Load;
				depthAtt.stencilStoreOp    = wgpu::StoreOp::Store;
			}

			wgpu::RenderPassDescriptor passDesc{};
			passDesc.label                  = "guirenderer-ui";
			passDesc.colorAttachmentCount   = 1;
			passDesc.colorAttachments       = &colorAtt;
			passDesc.depthStencilAttachment = target.HasDepth ? &depthAtt : nullptr;

			wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&passDesc);
			pass.SetPipeline(pipeline);
			pass.SetVertexBuffer(0, WebGPUSpriteResources::GetQuadVertexBuffer());
			pass.SetVertexBuffer(1, state.InstanceBuffer);
			pass.SetIndexBuffer(WebGPUSpriteResources::GetQuadIndexBuffer(),
				wgpu::IndexFormat::Uint16);
			pass.SetScissorRect(0, 0, target.Width, target.Height);

			bool scissorIsFull = true;
			size_t i = start;
			while (i < end) {
				const TextureHandle runHandle = resolveHandle(m_InstancesScratch[i].TextureHandle);
				const bool runHasClip = m_InstancesScratch[i].HasClip;
				const Vec2 runClipMin = m_InstancesScratch[i].ClipMin;
				const Vec2 runClipMax = m_InstancesScratch[i].ClipMax;

				size_t runEnd = i + 1;
				while (runEnd < end) {
					const TextureHandle h = resolveHandle(m_InstancesScratch[runEnd].TextureHandle);
					if (!(h.index == runHandle.index && h.generation == runHandle.generation)) break;
					if (!ClipsEqual(runHasClip, runClipMin, runClipMax,
						m_InstancesScratch[runEnd].HasClip,
						m_InstancesScratch[runEnd].ClipMin,
						m_InstancesScratch[runEnd].ClipMax)) break;
					++runEnd;
				}
				const uint32_t count = static_cast<uint32_t>(runEnd - i);

				wgpu::BindGroup bg = ResolveBindGroup(device, state, runHandle);
				if (!bg) {
					i = runEnd;
					continue;
				}

				if (runHasClip) {
					const ScissorPx sc = ComputeScissor(mvp, runClipMin, runClipMax,
						target.Width, target.Height);
					if (sc.Valid) {
						pass.SetScissorRect(sc.X, sc.Y, sc.W, sc.H);
						scissorIsFull = false;
					} else {
						i = runEnd;
						continue;
					}
				} else if (!scissorIsFull) {
					pass.SetScissorRect(0, 0, target.Width, target.Height);
					scissorIsFull = true;
				}

				pass.SetBindGroup(0, bg);
				pass.DrawIndexed(/*indexCount=*/6,
					/*instanceCount=*/count,
					/*firstIndex=*/0,
					/*baseVertex=*/0,
					/*firstInstance=*/static_cast<uint32_t>(i));

				i = runEnd;
			}

			pass.End();

			if (target.IsSwapChain) {
				WebGPUBackend::MarkSwapChainRendered();
			}
		};

		// ── Helper: submit one text phase covering [start, end) ──────
		auto submitTextPhase = [&](size_t start, size_t end) {
			if (!m_TextRenderer || !m_TextRenderer->IsInitialized() || start >= end) return;
			std::span<const TextDrawCmd> sub(m_TextScratch.data() + start, end - start);
			m_TextRenderer->RenderInstances(sub, mvp, /*viewId*/ 0xFFFFu, /*scissorCache*/ 0xFFFFu);
		};

		// ── Merge walk: interleave image / text phases ───────────────
		// The hierarchy panel shows entities top-to-bottom in DrawIndex
		// order, with later siblings drawn ON TOP. Splitting into
		// "all images, then all text" passes broke that: text always
		// painted last regardless of where the TextRenderer entity sat
		// in the hierarchy. Walking the two pre-sorted lists in tandem
		// produces a sequence of contiguous-same-type phases that
		// preserves hierarchy order. Equal sort keys prefer images
		// (`<=`) so a text + image at identical DrawIndex still has
		// text behind, matching the design intent that text labels
		// underneath an image overlay get covered.
		auto imgKey = [&](size_t k) {
			const auto& inst = m_InstancesScratch[k];
			return std::make_tuple(inst.SortingLayer, inst.SortingOrder, inst.DrawIndex);
		};
		auto txtKey = [&](size_t k) {
			const auto& cmd = m_TextScratch[k];
			return std::make_tuple(cmd.SortingLayer, cmd.SortingOrder, cmd.DrawIndex);
		};

		const size_t nTxt = m_TextScratch.size();
		size_t imgPos = 0;
		size_t txtPos = 0;
		bool   anyTextPhaseSubmitted = false;

		while (imgPos < n || txtPos < nTxt) {
			bool drawImage;
			if (imgPos >= n)        drawImage = false;
			else if (txtPos >= nTxt) drawImage = true;
			else drawImage = imgKey(imgPos) <= txtKey(txtPos);

			if (drawImage) {
				size_t imgEnd = imgPos + 1;
				while (imgEnd < n
					&& (txtPos >= nTxt || imgKey(imgEnd) <= txtKey(txtPos)))
				{
					++imgEnd;
				}
				submitImagePhase(imgPos, imgEnd);
				imgPos = imgEnd;
			} else {
				size_t txtEnd = txtPos + 1;
				while (txtEnd < nTxt
					&& (imgPos >= n || txtKey(txtEnd) < imgKey(imgPos)))
				{
					++txtEnd;
				}
				// Second-or-later text phase: flush so the earlier text
				// pass's vertex buffer write isn't clobbered by this
				// one's. TextRenderer reuses one vertex buffer per
				// PerInstance and rewrites it via queue.WriteBuffer on
				// every RenderInstances call — Dawn's uploader flushes
				// all pending copies before user submits, so two
				// recorded text passes without a submit between them
				// both end up reading the second write's vertices.
				if (anyTextPhaseSubmitted) {
					WebGPUBackend::FlushCommands();
				}
				submitTextPhase(txtPos, txtEnd);
				anyTextPhaseSubmitted = true;
				txtPos = txtEnd;
			}
		}
	}

}  // namespace Index
