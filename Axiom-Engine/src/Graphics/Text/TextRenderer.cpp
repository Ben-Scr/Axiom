#include "pch.hpp"
#include "Graphics/Text/TextRenderer.hpp"

#include "Components/General/Transform2DComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Tags.hpp"
#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/Shader.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Scene/Scene.hpp"
#include "Serialization/Path.hpp"

#include <webgpu/webgpu_cpp.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <utility>

// =============================================================================
// TextRenderer — WebGPU (Dawn) implementation. Stage 6 of the WebGPU port.
// -----------------------------------------------------------------------------
// Sibling to TextRenderer.cpp (the bgfx implementation). CPU-side glyph
// emission (DecodeUtf8, MeasureLineWidth, MeasureNaturalSize, EmitText with
// word/character wrap) is preserved verbatim — text-layout math doesn't care
// about the GPU backend. Only the submit path is replaced.
//
// Process-shared GPU resources (text shader module, pipeline layout, atlas
// sampler) live in a Globals() singleton with reference counting — same
// pattern as the bgfx side, since multiple TextRenderer instances exist in
// the engine (one per GuiRenderer for the editor's two FBOs, plus
// Renderer2D's owned one for world-space text).
//
// Per-instance state (dynamic vertex buffer, uniform buffer, per-frame
// bind-group cache) is a TU-local side-table keyed by `this`. The buffers
// grow geometrically on demand and are released on Shutdown.
//
// Pipeline cache: built lazily per (color-format, has-depth). The engine
// hits at most ~2 formats per session (swap-chain BGRA8Unorm or FBO RGBA8
// Unorm × {with depth, without}).
//
// Clip handling: TextRenderer_WebGPU groups the supplied commands by
// (atlas, sort key, clip rect) and applies SetScissorRect per group from
// the MVP-projected UI-space clip — so a single RenderInstances call can
// mix unclipped + Mask-clipped text correctly. Different from the bgfx
// side, where GuiRenderer pre-clustered by clip and made multiple
// RenderInstances calls passing a pre-computed scissor index.
// =============================================================================

namespace Axiom {

	namespace {
		constexpr size_t k_VerticesPerGlyph     = 6;
		constexpr size_t k_InitialVertexCapacity = 1024;

		// CPU helpers — identical to TextRenderer.cpp's bgfx version.

		bool DecodeUtf8(std::string_view s, size_t idx, uint32_t& outCp, int& outLen) {
			if (idx >= s.size()) { outCp = 0; outLen = 0; return false; }
			const unsigned char b0 = static_cast<unsigned char>(s[idx]);
			if (b0 < 0x80) { outCp = b0; outLen = 1; return true; }
			int needed = 0;
			uint32_t cp = 0;
			if      ((b0 & 0xE0) == 0xC0) { needed = 2; cp = b0 & 0x1F; }
			else if ((b0 & 0xF0) == 0xE0) { needed = 3; cp = b0 & 0x0F; }
			else if ((b0 & 0xF8) == 0xF0) { needed = 4; cp = b0 & 0x07; }
			else { outCp = b0; outLen = 1; return true; }
			if (idx + needed > s.size()) { outCp = b0; outLen = 1; return true; }
			for (int i = 1; i < needed; ++i) {
				const unsigned char b = static_cast<unsigned char>(s[idx + i]);
				if ((b & 0xC0) != 0x80) { outCp = b0; outLen = 1; return true; }
				cp = (cp << 6) | (b & 0x3F);
			}
			outCp = cp;
			outLen = needed;
			return true;
		}

		float MeasureLineWidth(const Font& font, std::string_view line, float letterSpacing) {
			float width = 0.0f;
			uint32_t prev = 0;
			int glyphCount = 0;
			size_t i = 0;
			while (i < line.size()) {
				uint32_t cp = 0;
				int len = 0;
				if (!DecodeUtf8(line, i, cp, len)) break;
				i += static_cast<size_t>(len);

				const GlyphMetrics* g = font.GetGlyph(cp);
				if (!g) { prev = 0; continue; }
				if (prev != 0) width += font.GetKerning(prev, cp);
				width += g->XAdvance;
				if (glyphCount > 0) width += letterSpacing;
				++glyphCount;
				prev = cp;
			}
			return width;
		}

		// ── Process-shared global state (ref-counted) ───────────────────────
		struct GlobalTextState {
			int                    RefCount = 0;
			std::unique_ptr<Shader> ShaderObj;
			wgpu::ShaderModule     Module;
			wgpu::BindGroupLayout  BindGroupLayout;
			wgpu::PipelineLayout   PipelineLayout;
			wgpu::Sampler          AtlasSampler;
			// Pipeline cache keyed by (colorFormat << 1) | hasDepth, matching
			// the SpriteResources scheme.
			std::unordered_map<uint32_t, wgpu::RenderPipeline> PipelineCache;
		};

		GlobalTextState& Globals() {
			static GlobalTextState s;
			return s;
		}

		uint32_t MakePipelineKey(wgpu::TextureFormat fmt, bool hasDepth) {
			return (static_cast<uint32_t>(fmt) << 1) | (hasDepth ? 1u : 0u);
		}

		wgpu::BindGroupLayout BuildBindGroupLayout(wgpu::Device device) {
			wgpu::BindGroupLayoutEntry entries[3] = {};

			entries[0].binding    = 0;
			entries[0].visibility = wgpu::ShaderStage::Vertex;
			entries[0].buffer.type           = wgpu::BufferBindingType::Uniform;
			entries[0].buffer.hasDynamicOffset = false;
			entries[0].buffer.minBindingSize = 64;

			entries[1].binding    = 1;
			entries[1].visibility = wgpu::ShaderStage::Fragment;
			entries[1].texture.sampleType    = wgpu::TextureSampleType::Float;
			entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
			entries[1].texture.multisampled  = false;

			entries[2].binding    = 2;
			entries[2].visibility = wgpu::ShaderStage::Fragment;
			entries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

			wgpu::BindGroupLayoutDescriptor desc{};
			desc.entryCount = 3;
			desc.entries    = entries;
			desc.label      = "text-bindgroup-layout";
			return device.CreateBindGroupLayout(&desc);
		}

		wgpu::PipelineLayout BuildPipelineLayout(wgpu::Device device, wgpu::BindGroupLayout bgl) {
			wgpu::BindGroupLayout bgls[1] = { bgl };
			wgpu::PipelineLayoutDescriptor desc{};
			desc.bindGroupLayoutCount = 1;
			desc.bindGroupLayouts     = bgls;
			desc.label                = "text-pipeline-layout";
			return device.CreatePipelineLayout(&desc);
		}

		wgpu::Sampler BuildAtlasSampler(wgpu::Device device) {
			wgpu::SamplerDescriptor desc{};
			desc.addressModeU  = wgpu::AddressMode::ClampToEdge;
			desc.addressModeV  = wgpu::AddressMode::ClampToEdge;
			desc.addressModeW  = wgpu::AddressMode::ClampToEdge;
			desc.magFilter     = wgpu::FilterMode::Linear;
			desc.minFilter     = wgpu::FilterMode::Linear;
			desc.mipmapFilter  = wgpu::MipmapFilterMode::Nearest;
			desc.maxAnisotropy = 1;
			desc.label         = "text-atlas-sampler";
			return device.CreateSampler(&desc);
		}

		wgpu::RenderPipeline BuildTextPipeline(wgpu::Device device,
			wgpu::ShaderModule module, wgpu::PipelineLayout layout,
			wgpu::TextureFormat colorFormat, bool hasDepth)
		{
			// Vertex layout: 1 buffer × 3 attributes (per-vertex).
			wgpu::VertexAttribute attrs[3] = {};
			attrs[0].format         = wgpu::VertexFormat::Float32x2;
			attrs[0].offset         = offsetof(TextVertex, X);
			attrs[0].shaderLocation = 0;
			attrs[1].format         = wgpu::VertexFormat::Float32x2;
			attrs[1].offset         = offsetof(TextVertex, U);
			attrs[1].shaderLocation = 1;
			attrs[2].format         = wgpu::VertexFormat::Float32x4;
			attrs[2].offset         = offsetof(TextVertex, R);
			attrs[2].shaderLocation = 2;

			wgpu::VertexBufferLayout vbl{};
			vbl.arrayStride    = sizeof(TextVertex);
			vbl.stepMode       = wgpu::VertexStepMode::Vertex;
			vbl.attributeCount = 3;
			vbl.attributes     = attrs;

			wgpu::BlendState blend{};
			blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
			blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
			blend.color.operation = wgpu::BlendOperation::Add;
			blend.alpha.srcFactor = wgpu::BlendFactor::One;
			blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
			blend.alpha.operation = wgpu::BlendOperation::Add;

			wgpu::ColorTargetState colorTarget{};
			colorTarget.format    = colorFormat;
			colorTarget.blend     = &blend;
			colorTarget.writeMask = wgpu::ColorWriteMask::All;

			wgpu::FragmentState fragState{};
			fragState.module      = module;
			fragState.entryPoint  = "fs_main";
			fragState.targetCount = 1;
			fragState.targets     = &colorTarget;

			wgpu::PrimitiveState prim{};
			prim.topology         = wgpu::PrimitiveTopology::TriangleList;
			prim.stripIndexFormat = wgpu::IndexFormat::Undefined;
			prim.frontFace        = wgpu::FrontFace::CCW;
			prim.cullMode         = wgpu::CullMode::None;

			wgpu::DepthStencilState depthState{};
			if (hasDepth) {
				depthState.format            = wgpu::TextureFormat::Depth24PlusStencil8;
				depthState.depthWriteEnabled = false;
				depthState.depthCompare      = wgpu::CompareFunction::Always;
			}

			wgpu::RenderPipelineDescriptor desc{};
			desc.label  = "text-pipeline";
			desc.layout = layout;

			desc.vertex.module      = module;
			desc.vertex.entryPoint  = "vs_main";
			desc.vertex.bufferCount = 1;
			desc.vertex.buffers     = &vbl;

			desc.fragment     = &fragState;
			desc.primitive    = prim;
			desc.depthStencil = hasDepth ? &depthState : nullptr;

			desc.multisample.count = 1;
			desc.multisample.mask  = 0xFFFFFFFF;

			return device.CreateRenderPipeline(&desc);
		}

		bool EnsureGlobalState() {
			GlobalTextState& g = Globals();
			if (g.RefCount++ > 0) {
				return static_cast<bool>(g.Module);
			}
			if (!WebGPUBackend::IsInitialized()) {
				--g.RefCount;
				return false;
			}
			wgpu::Device device = WebGPUBackend::GetDevice();
			if (!device) { --g.RefCount; return false; }

			// Shader_WebGPU's built-in registry maps "text" -> the embedded WGSL.
			// We pass the same vs/fs paths the bgfx side does so the stem
			// extraction lands on the same name.
			g.ShaderObj = std::make_unique<Shader>(
				std::string("AxiomAssets/Shaders/TextShader.vs"),
				std::string("AxiomAssets/Shaders/TextShader.fs"));
			if (!g.ShaderObj || !g.ShaderObj->IsValid()) {
				AIM_CORE_ERROR_TAG("TextRenderer",
					"Text shader failed to load — text disabled");
				g.ShaderObj.reset();
				--g.RefCount;
				return false;
			}
			const auto lookup = WebGPUBackend::LookupShader(g.ShaderObj->GetHandle());
			if (!lookup.Valid) {
				g.ShaderObj.reset();
				--g.RefCount;
				return false;
			}
			g.Module = lookup.Module;

			g.BindGroupLayout = BuildBindGroupLayout(device);
			g.PipelineLayout  = BuildPipelineLayout(device, g.BindGroupLayout);
			g.AtlasSampler    = BuildAtlasSampler(device);
			if (!g.BindGroupLayout || !g.PipelineLayout || !g.AtlasSampler) {
				AIM_CORE_ERROR_TAG("TextRenderer",
					"Failed to build text bind-group / pipeline layout / sampler");
				g.ShaderObj.reset();
				g.Module          = nullptr;
				g.BindGroupLayout = nullptr;
				g.PipelineLayout  = nullptr;
				g.AtlasSampler    = nullptr;
				--g.RefCount;
				return false;
			}
			return true;
		}

		void ReleaseGlobalState() {
			GlobalTextState& g = Globals();
			if (g.RefCount == 0) return;
			if (--g.RefCount > 0) return;
			g.PipelineCache.clear();
			g.AtlasSampler    = nullptr;
			g.PipelineLayout  = nullptr;
			g.BindGroupLayout = nullptr;
			g.Module          = nullptr;
			g.ShaderObj.reset();
		}

		wgpu::RenderPipeline GetOrBuildPipeline(wgpu::TextureFormat colorFormat, bool hasDepth) {
			GlobalTextState& g = Globals();
			if (!g.Module || !g.PipelineLayout) return nullptr;

			const uint32_t key = MakePipelineKey(colorFormat, hasDepth);
			auto it = g.PipelineCache.find(key);
			if (it != g.PipelineCache.end()) return it->second;

			wgpu::Device device = WebGPUBackend::GetDevice();
			if (!device) return nullptr;
			wgpu::RenderPipeline pipeline = BuildTextPipeline(
				device, g.Module, g.PipelineLayout, colorFormat, hasDepth);
			if (!pipeline) return nullptr;
			g.PipelineCache.emplace(key, pipeline);
			return pipeline;
		}

		// ── Per-TextRenderer instance state ─────────────────────────────────
		struct PerInstance {
			wgpu::Buffer VertexBuffer;
			uint32_t     VertexBufferCapacityBytes = 0;
			wgpu::Buffer UniformBuffer;
			// Bind groups for this frame, keyed by font-atlas pool ID. Cleared
			// at the top of every RenderInstances call so an atlas destroyed
			// between frames (Font::~Font -> DeregisterAtlas) can't leak a
			// stale TextureView reference into a cached group.
			std::unordered_map<unsigned, wgpu::BindGroup> BindGroupsThisCall;
		};
		std::unordered_map<const TextRenderer*, PerInstance> g_PerInstance;

		PerInstance& GetPerInstance(const TextRenderer* self) {
			return g_PerInstance[self];
		}

		bool EnsureVertexBuffer(wgpu::Device device, PerInstance& inst, uint32_t neededBytes) {
			if (inst.VertexBufferCapacityBytes >= neededBytes && inst.VertexBuffer) return true;
			uint32_t cap = inst.VertexBufferCapacityBytes > 0
				? inst.VertexBufferCapacityBytes
				: static_cast<uint32_t>(k_InitialVertexCapacity * sizeof(TextVertex));
			while (cap < neededBytes) cap *= 2;

			wgpu::BufferDescriptor desc{};
			desc.size  = cap;
			desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
			desc.label = "text-vertex-buffer";
			wgpu::Buffer buf = device.CreateBuffer(&desc);
			if (!buf) return false;
			inst.VertexBuffer              = std::move(buf);
			inst.VertexBufferCapacityBytes = cap;
			return true;
		}

		bool EnsureUniformBuffer(wgpu::Device device, PerInstance& inst) {
			if (inst.UniformBuffer) return true;
			wgpu::BufferDescriptor desc{};
			desc.size  = 64;
			desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
			desc.label = "text-viewproj-ubo";
			inst.UniformBuffer = device.CreateBuffer(&desc);
			return static_cast<bool>(inst.UniformBuffer);
		}

		wgpu::BindGroup ResolveAtlasBindGroup(wgpu::Device device, PerInstance& inst,
			unsigned atlasId)
		{
			auto it = inst.BindGroupsThisCall.find(atlasId);
			if (it != inst.BindGroupsThisCall.end()) return it->second;

			wgpu::TextureView view = WebGPUBackend::LookupFontAtlas(atlasId);
			if (!view) return nullptr;

			GlobalTextState& g = Globals();
			wgpu::BindGroupEntry entries[3] = {};
			entries[0].binding = 0;
			entries[0].buffer  = inst.UniformBuffer;
			entries[0].offset  = 0;
			entries[0].size    = 64;
			entries[1].binding     = 1;
			entries[1].textureView = view;
			entries[2].binding = 2;
			entries[2].sampler = g.AtlasSampler;

			wgpu::BindGroupDescriptor desc{};
			desc.layout     = g.BindGroupLayout;
			desc.entryCount = 3;
			desc.entries    = entries;
			desc.label      = "text-atlas-bindgroup";
			wgpu::BindGroup bg = device.CreateBindGroup(&desc);
			if (!bg) return nullptr;
			inst.BindGroupsThisCall.emplace(atlasId, bg);
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
			if (ix >= targetW || iy >= targetH) return out;
			if (ix + iw > targetW) iw = targetW - ix;
			if (iy + ih > targetH) ih = targetH - iy;
			out.X = ix; out.Y = iy; out.W = iw; out.H = ih; out.Valid = true;
			return out;
		}
	}  // anonymous namespace

	// ── Static font resolution (backend-neutral logic) ──────────────────────

	Vec2 TextRenderer::MeasureNaturalSize(Font& font, std::string_view text, float letterSpacing) {
		if (text.empty()) return Vec2{ 0.0f, font.GetLineHeight() };

		float maxLineWidth = 0.0f;
		int lineCount = 0;
		size_t lineStart = 0;
		const size_t textSize = text.size();
		while (lineStart <= textSize) {
			size_t lineEnd = text.find('\n', lineStart);
			if (lineEnd == std::string_view::npos) lineEnd = textSize;
			std::string_view line(text.data() + lineStart, lineEnd - lineStart);
			const float w = MeasureLineWidth(font, line, letterSpacing);
			if (w > maxLineWidth) maxLineWidth = w;
			++lineCount;
			if (lineEnd == textSize) break;
			lineStart = lineEnd + 1;
		}
		return Vec2{ maxLineWidth, font.GetLineHeight() * static_cast<float>(lineCount) };
	}

	Font* TextRenderer::ResolveFont(TextRendererComponent& text) {
		return ResolveFontAtPixelSize(text, text.FontSize);
	}

	Font* TextRenderer::ResolveFontAtPixelSize(TextRendererComponent& text, float pixelSize) {
		constexpr float k_MaxBakedPixelSize = 192.0f;
		const float requested = pixelSize > 0.0f ? pixelSize : text.FontSize;
		const float bakeRequest = std::min(requested, k_MaxBakedPixelSize);

		auto quantizeBucket = [](float p) -> int {
			int v = std::max(1, static_cast<int>(std::lround(p)));
			auto snap = [](int value, int step) {
				return ((value + step / 2) / step) * step;
			};
			if (v <= 16)  return v;
			if (v <= 32)  return snap(v, 2);
			if (v <= 64)  return snap(v, 4);
			if (v <= 128) return snap(v, 8);
			return snap(v, 16);
		};

		if (FontManager::IsValid(text.ResolvedFont)) {
			if (Font* font = FontManager::GetFont(text.ResolvedFont)) {
				if (quantizeBucket(font->GetPixelSize()) == quantizeBucket(bakeRequest)) {
					return font;
				}
			}
		}

		const uint64_t uuid = static_cast<uint64_t>(text.FontAssetId);
		if (uuid != 0) {
			text.ResolvedFont = FontManager::LoadFontByUUID(uuid, bakeRequest);
			if (Font* f = FontManager::GetFont(text.ResolvedFont)) return f;
		}

		text.ResolvedFont = FontManager::GetDefaultFont();
		Font* fallback = FontManager::GetFont(text.ResolvedFont);
		if (!fallback) {
			static bool s_LoggedMissingFont = false;
			if (!s_LoggedMissingFont) {
				s_LoggedMissingFont = true;
				AIM_CORE_WARN_TAG("TextRenderer",
					"No font available — assign one in the inspector or ensure AxiomAssets/Fonts/DefaultSans-Regular.ttf is shipped next to the executable.");
			}
		}
		return fallback;
	}

	// ── Lifecycle ───────────────────────────────────────────────────────────

	TextRenderer::TextRenderer() = default;
	TextRenderer::~TextRenderer() {
		Shutdown();
	}

	void TextRenderer::Initialize() {
		if (m_IsInitialized) return;
		if (!EnsureGlobalState()) return;
		m_Vertices.reserve(k_InitialVertexCapacity);
		m_Runs.reserve(16);
		(void)GetPerInstance(this);
		m_IsInitialized = true;
		AIM_CORE_INFO_TAG("TextRenderer", "Text renderer initialized (WebGPU)");
	}

	void TextRenderer::Shutdown() {
		if (!m_IsInitialized) return;
		m_Vertices.clear();
		m_Vertices.shrink_to_fit();
		m_Runs.clear();
		m_Runs.shrink_to_fit();
		g_PerInstance.erase(this);
		ReleaseGlobalState();
		m_IsInitialized = false;
	}

	void TextRenderer::EnsureGpuCapacity(size_t /*requiredBytes*/) {
		// Per-frame vertex buffer growth is handled inside RenderInstances —
		// this hook is the OpenGL holdover. Empty in both bgfx and WebGPU.
	}

	// ── Glyph emission (CPU side, identical to bgfx + GL) ────────────────────

	void TextRenderer::EmitText(Font& font, std::string_view text,
		float worldX, float worldY,
		float scale, const Color& color,
		TextAlignment alignment, float letterSpacing,
		TextWrapMode wrapMode, float wrapWidthPixels,
		float rotation, Vec2 pivot)
	{
		const bool applyRotation = rotation != 0.0f;
		const float rotC = applyRotation ? std::cos(rotation) : 1.0f;
		const float rotS = applyRotation ? std::sin(rotation) : 0.0f;
		auto rot = [&](float x, float y) -> std::pair<float, float> {
			if (!applyRotation) return { x, y };
			const float dx = x - pivot.x;
			const float dy = y - pivot.y;
			return { pivot.x + rotC * dx - rotS * dy,
			         pivot.y + rotS * dx + rotC * dy };
		};

		const float lineHeight = font.GetLineHeight() * scale;
		const bool autoWrap = wrapMode != TextWrapMode::None && wrapWidthPixels > 0.0f;

		m_WrapScratch.clear();

		auto emitVisualLine = [&](size_t s, size_t e) {
			m_WrapScratch.push_back({ s, e });
		};

		auto wrapSegment = [&](size_t segStart, size_t segEnd) {
			if (!autoWrap || segStart >= segEnd) {
				emitVisualLine(segStart, segEnd);
				return;
			}
			size_t lineStartIdx = segStart;
			size_t lastBreakIdx = std::string_view::npos;
			float widthSinceLineStart = 0.0f;
			uint32_t prev = 0;
			int glyphsOnLine = 0;

			size_t i = segStart;
			while (i < segEnd) {
				const size_t glyphStart = i;
				uint32_t cp = 0;
				int len = 0;
				if (!DecodeUtf8(text, i, cp, len)) break;
				const size_t nextI = i + static_cast<size_t>(len);
				i = nextI;

				const GlyphMetrics* g = font.GetGlyph(cp);
				if (!g) { prev = 0; continue; }

				float advance = g->XAdvance;
				if (prev != 0) advance += font.GetKerning(prev, cp);
				if (glyphsOnLine > 0) advance += letterSpacing;

				const float candidate = widthSinceLineStart + advance;

				if (candidate > wrapWidthPixels && glyphsOnLine > 0) {
					if (wrapMode == TextWrapMode::Word
						&& lastBreakIdx != std::string_view::npos
						&& lastBreakIdx > lineStartIdx)
					{
						emitVisualLine(lineStartIdx, lastBreakIdx);
						lineStartIdx = lastBreakIdx + 1;
						i = lineStartIdx;
						if (i >= segEnd) { lineStartIdx = i; break; }
						widthSinceLineStart = 0.0f;
						lastBreakIdx = std::string_view::npos;
						prev = 0;
						glyphsOnLine = 0;
						continue;
					}
					emitVisualLine(lineStartIdx, glyphStart);
					lineStartIdx = glyphStart;
					i = glyphStart;
					widthSinceLineStart = 0.0f;
					lastBreakIdx = std::string_view::npos;
					prev = 0;
					glyphsOnLine = 0;
					continue;
				}

				widthSinceLineStart = candidate;
				if (cp == ' ' || cp == '\t') lastBreakIdx = glyphStart;
				prev = cp;
				++glyphsOnLine;
			}
			if (lineStartIdx < segEnd) emitVisualLine(lineStartIdx, segEnd);
		};

		size_t segStart = 0;
		const size_t textSize = text.size();
		while (segStart <= textSize) {
			size_t segEnd = text.find('\n', segStart);
			if (segEnd == std::string_view::npos) segEnd = textSize;
			wrapSegment(segStart, segEnd);
			if (segEnd == textSize) break;
			segStart = segEnd + 1;
		}

		for (size_t lineIndex = 0; lineIndex < m_WrapScratch.size(); ++lineIndex) {
			const auto [lineBegin, lineEnd] = m_WrapScratch[lineIndex];
			std::string_view line(text.data() + lineBegin, lineEnd - lineBegin);
			const float lineWidth = MeasureLineWidth(font, line, letterSpacing) * scale;

			float alignOffset = 0.0f;
			switch (alignment) {
			case TextAlignment::Center: alignOffset = -lineWidth * 0.5f; break;
			case TextAlignment::Right:  alignOffset = -lineWidth; break;
			case TextAlignment::Left:
			default:                    alignOffset = 0.0f; break;
			}

			float penX = worldX + alignOffset;
			const float baselineY = worldY - static_cast<float>(lineIndex) * lineHeight;

			uint32_t prev = 0;
			int glyphsOnLine = 0;
			size_t i = 0;
			while (i < line.size()) {
				uint32_t cp = 0;
				int len = 0;
				if (!DecodeUtf8(line, i, cp, len)) break;
				i += static_cast<size_t>(len);

				const GlyphMetrics* g = font.GetGlyph(cp);
				if (!g) { prev = 0; continue; }
				if (prev != 0) penX += font.GetKerning(prev, cp) * scale;
				if (glyphsOnLine > 0) penX += letterSpacing * scale;

				if (g->Width > 0.0f && g->Height > 0.0f) {
					const float x0 = penX + g->XOffset * scale;
					const float y0 = baselineY - g->YOffset * scale;
					const float x1 = x0 + g->Width * scale;
					const float y1 = y0 - g->Height * scale;

					auto [tlX, tlY] = rot(x0, y0);
					auto [trX, trY] = rot(x1, y0);
					auto [brX, brY] = rot(x1, y1);
					auto [blX, blY] = rot(x0, y1);

					TextVertex vTL{ tlX, tlY, g->U0, g->V0, color.r, color.g, color.b, color.a };
					TextVertex vTR{ trX, trY, g->U1, g->V0, color.r, color.g, color.b, color.a };
					TextVertex vBR{ brX, brY, g->U1, g->V1, color.r, color.g, color.b, color.a };
					TextVertex vBL{ blX, blY, g->U0, g->V1, color.r, color.g, color.b, color.a };

					m_Vertices.push_back(vBL);
					m_Vertices.push_back(vBR);
					m_Vertices.push_back(vTR);
					m_Vertices.push_back(vBL);
					m_Vertices.push_back(vTR);
					m_Vertices.push_back(vTL);
				}
				penX += g->XAdvance * scale;
				++glyphsOnLine;
				prev = cp;
			}
		}
	}

	// ── Submit path ─────────────────────────────────────────────────────────

	void TextRenderer::RenderInstances(std::span<const TextDrawCmd> commands, const glm::mat4& mvp,
		unsigned short /*viewId*/, unsigned short /*scissorCache*/)
	{
		m_LastFrameGlyphCount = 0;
		m_LastFrameDrawCalls = 0;
		if (!m_IsInitialized || commands.empty()) return;

		GlobalTextState& g = Globals();
		if (!g.Module || !g.PipelineLayout) return;

		auto target = WebGPUBackend::BeginRenderToCurrentTarget();
		if (!target.Valid) return;

		wgpu::Device device = WebGPUBackend::GetDevice();
		wgpu::Queue  queue  = WebGPUBackend::GetQueue();
		if (!device || !queue) return;

		wgpu::RenderPipeline pipeline = GetOrBuildPipeline(target.ColorFormat, target.HasDepth);
		if (!pipeline) return;

		PerInstance& inst = GetPerInstance(this);
		inst.BindGroupsThisCall.clear();

		if (!EnsureUniformBuffer(device, inst)) return;
		queue.WriteBuffer(inst.UniformBuffer, 0, glm::value_ptr(mvp), 64);

		// Sort the supplied commands so glyph emission groups by
		// (SortingLayer, SortingOrder, atlas) — adjacent atlas runs share a
		// bind group + draw call.
		m_Order.clear();
		m_Order.reserve(commands.size());
		for (size_t i = 0; i < commands.size(); ++i) {
			if (commands[i].FontPtr && !commands[i].Text.empty()) {
				m_Order.push_back(i);
			}
		}
		std::sort(m_Order.begin(), m_Order.end(), [&](size_t a, size_t b) {
			const auto& ca = commands[a];
			const auto& cb = commands[b];
			if (ca.SortingLayer != cb.SortingLayer) return ca.SortingLayer < cb.SortingLayer;
			if (ca.SortingOrder != cb.SortingOrder) return ca.SortingOrder < cb.SortingOrder;
			return ca.FontPtr < cb.FontPtr;
		});

		// Emit all glyph vertices first, recording one GlyphRun per
		// homogeneous (atlas, sort key, clip) span. The clip is the
		// per-pass scissor target; including it in the run key keeps
		// SetScissorRect changes minimal.
		m_Vertices.clear();
		m_Runs.clear();

		struct Run {
			unsigned AtlasId = 0;
			int      SortingLayer = 0;
			int16_t  SortingOrder = 0;
			bool     HasClip = false;
			Vec2     ClipMin{};
			Vec2     ClipMax{};
			size_t   VertexStart = 0;
			size_t   VertexCount = 0;
		};
		std::vector<Run> runs;
		runs.reserve(16);

		for (size_t i = 0; i < m_Order.size(); ) {
			const TextDrawCmd& head = commands[m_Order[i]];
			Run r;
			r.AtlasId      = head.FontPtr->GetAtlasTexture();
			r.SortingLayer = head.SortingLayer;
			r.SortingOrder = head.SortingOrder;
			r.HasClip      = head.HasClip;
			r.ClipMin      = head.ClipMin;
			r.ClipMax      = head.ClipMax;
			r.VertexStart  = m_Vertices.size();

			size_t j = i;
			while (j < m_Order.size()) {
				const TextDrawCmd& cmd = commands[m_Order[j]];
				if (cmd.FontPtr->GetAtlasTexture() != r.AtlasId
					|| cmd.SortingLayer != r.SortingLayer
					|| cmd.SortingOrder != r.SortingOrder)
					break;
				if (!ClipsEqual(r.HasClip, r.ClipMin, r.ClipMax,
					cmd.HasClip, cmd.ClipMin, cmd.ClipMax))
					break;

				EmitText(*cmd.FontPtr, cmd.Text, cmd.X, cmd.Y, cmd.Scale,
					cmd.Tint, cmd.Align, cmd.LetterSpacing,
					cmd.Wrap, cmd.WrapWidthPixels,
					cmd.Rotation, cmd.Pivot);
				++j;
			}
			r.VertexCount = m_Vertices.size() - r.VertexStart;
			if (r.VertexCount > 0) runs.push_back(r);
			i = j;
		}

		if (m_Vertices.empty() || runs.empty()) return;

		const uint32_t totalBytes = static_cast<uint32_t>(m_Vertices.size() * sizeof(TextVertex));
		if (!EnsureVertexBuffer(device, inst, totalBytes)) return;
		queue.WriteBuffer(inst.VertexBuffer, 0, m_Vertices.data(), totalBytes);

		wgpu::CommandEncoder encoder = WebGPUBackend::GetFrameEncoder();
		if (!encoder) return;

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
		passDesc.label                  = "text-pass";
		passDesc.colorAttachmentCount   = 1;
		passDesc.colorAttachments       = &colorAtt;
		passDesc.depthStencilAttachment = target.HasDepth ? &depthAtt : nullptr;

		wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
		pass.SetPipeline(pipeline);
		pass.SetVertexBuffer(0, inst.VertexBuffer);
		pass.SetScissorRect(0, 0, target.Width, target.Height);

		bool scissorIsFull = true;
		for (const Run& r : runs) {
			wgpu::BindGroup bg = ResolveAtlasBindGroup(device, inst, r.AtlasId);
			if (!bg) continue;  // atlas missing (Font destroyed mid-frame)

			if (r.HasClip) {
				const ScissorPx sc = ComputeScissor(mvp, r.ClipMin, r.ClipMax,
					target.Width, target.Height);
				if (!sc.Valid) continue;  // fully outside target — skip
				pass.SetScissorRect(sc.X, sc.Y, sc.W, sc.H);
				scissorIsFull = false;
			} else if (!scissorIsFull) {
				pass.SetScissorRect(0, 0, target.Width, target.Height);
				scissorIsFull = true;
			}

			pass.SetBindGroup(0, bg);
			pass.Draw(static_cast<uint32_t>(r.VertexCount),
				/*instanceCount=*/1,
				/*firstVertex=*/static_cast<uint32_t>(r.VertexStart),
				/*firstInstance=*/0);
			++m_LastFrameDrawCalls;
		}

		pass.End();

		if (target.IsSwapChain) {
			WebGPUBackend::MarkSwapChainRendered();
		}

		m_LastFrameGlyphCount = m_Vertices.size() / k_VerticesPerGlyph;
	}

	void TextRenderer::RenderScene(Scene& scene, const glm::mat4& vp, const AABB& viewportAABB) {
		m_LastFrameGlyphCount = 0;
		m_LastFrameDrawCalls = 0;
		if (!m_IsInitialized) return;

		m_PendingDrawCmds.clear();

		entt::registry& registry = scene.GetRegistry();
		auto view = registry.view<TextRendererComponent, Transform2DComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, text, tr] : view.each()) {
			if (text.Text.empty()) continue;

			Font* font = ResolveFont(text);
			if (!font || !font->IsLoaded()) continue;

			const AABB approx = AABB::FromTransform(tr);
			if (!AABB::Intersects(viewportAABB, approx)) {
				const float radius = text.FontSize * static_cast<float>(text.Text.size()) * 0.5f;
				AABB textBounds{
					{ tr.Position.x - radius, tr.Position.y - radius },
					{ tr.Position.x + radius, tr.Position.y + radius }
				};
				if (!AABB::Intersects(viewportAABB, textBounds)) continue;
			}

			const float bakedSize = font->GetPixelSize() > 0.0f ? font->GetPixelSize() : text.FontSize;
			const float drawScale = (text.FontSize / bakedSize)
				* tr.Scale.x
				/ k_TextPixelsPerWorldUnit;

			TextDrawCmd cmd;
			cmd.FontPtr = font;
			cmd.Text = std::string_view(text.Text);
			cmd.X = tr.Position.x + text.Margin.x * drawScale;
			cmd.Y = tr.Position.y - text.Margin.y * drawScale;
			cmd.Scale = drawScale;
			cmd.LetterSpacing = text.LetterSpacing;
			cmd.Tint = text.Color;
			cmd.Align = text.HAlign;
			cmd.Wrap = text.WrapMode;
			cmd.WrapWidthPixels = 0.0f;
			cmd.SortingOrder = text.SortingOrder;
			cmd.SortingLayer = text.SortingLayer;
			m_PendingDrawCmds.push_back(cmd);
		}

		RenderInstances(m_PendingDrawCmds, vp);
	}

}  // namespace Axiom
