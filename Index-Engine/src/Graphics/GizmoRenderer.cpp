#include "pch.hpp"
#include "Graphics/GizmoRenderer.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/Shader.hpp"
#include "Memory/FrameArenas.hpp"

#include <webgpu/webgpu_cpp.h>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>

// =============================================================================
// GizmoRenderer2D — WebGPU (Dawn) implementation.
// -----------------------------------------------------------------------------
// CPU side (BuildGeometry — squares→4 edges, circles→N segments, lines
// pass-through) drives a persistent dynamic wgpu::Buffer + one
// wgpu::RenderPipeline (LineList topology) + one render pass with
// LoadOp::Load.
//
// All state is TU-local because GizmoRenderer2D is a fully static class
// (single global instance). No per-`this` map needed.
// =============================================================================

namespace Index {

	// Static class members (declared in GizmoRenderer.hpp) ────────────────────
	bool                           GizmoRenderer2D::m_IsInitialized = false;
	std::unique_ptr<Shader>        GizmoRenderer2D::m_GizmoShader;
	std::vector<uint32_t>          GizmoRenderer2D::m_GizmoIndices;
	std::vector<GizmoUploadVertex> GizmoRenderer2D::s_UploadBuffer;
	uint16_t                       GizmoRenderer2D::m_GizmoViewId = 1;
	unsigned int                   GizmoRenderer2D::m_VAO = 0;
	unsigned int                   GizmoRenderer2D::m_VBO = 0;
	unsigned int                   GizmoRenderer2D::m_EBO = 0;
	std::size_t                    GizmoRenderer2D::m_VBOCapacityBytes = 0;
	std::size_t                    GizmoRenderer2D::m_EBOCapacityBytes = 0;
	int                            GizmoRenderer2D::m_uMVP = -1;

	namespace {
		// ── TU-local GPU state ──────────────────────────────────────────────
		wgpu::ShaderModule    g_Module;
		wgpu::BindGroupLayout g_BindGroupLayout;
		wgpu::PipelineLayout  g_PipelineLayout;
		wgpu::Buffer          g_UniformBuffer;
		wgpu::BindGroup       g_BindGroup;
		wgpu::Buffer          g_VertexBuffer;
		uint32_t              g_VertexBufferCapacityBytes = 0;
		// Pipeline cache keyed by (colorFormat << 1) | hasDepth, same scheme
		// SpriteResources / TextRenderer use.
		std::unordered_map<uint32_t, wgpu::RenderPipeline> g_PipelineCache;

		uint32_t MakePipelineKey(wgpu::TextureFormat fmt, bool hasDepth) {
			return (static_cast<uint32_t>(fmt) << 1) | (hasDepth ? 1u : 0u);
		}

		uint32_t PackRgba(const Color& c) {
			auto u8 = [](float v) {
				v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
				return static_cast<uint32_t>(v * 255.0f + 0.5f);
			};
			// WebGPU's Unorm8x4 vertex format decodes bytes in little-endian
			// order as (r, g, b, a) → vec4.
			return u8(c.r)
				| (u8(c.g) << 8)
				| (u8(c.b) << 16)
				| (u8(c.a) << 24);
		}

		// No bounds check — caller pre-sized `out` to the upper-bound
		// vertex count.
		inline void EmitLine(PosColorVertex* out, std::size_t& cursor,
			float x0, float y0, float x1, float y1, uint32_t rgba)
		{
			out[cursor++] = PosColorVertex{ x0, y0, 0.0f, rgba };
			out[cursor++] = PosColorVertex{ x1, y1, 0.0f, rgba };
		}

		// ── GPU resource helpers ────────────────────────────────────────────

		wgpu::BindGroupLayout BuildBindGroupLayout(wgpu::Device device) {
			wgpu::BindGroupLayoutEntry entry{};
			entry.binding    = 0;
			entry.visibility = wgpu::ShaderStage::Vertex;
			entry.buffer.type           = wgpu::BufferBindingType::Uniform;
			entry.buffer.hasDynamicOffset = false;
			entry.buffer.minBindingSize = 64;

			wgpu::BindGroupLayoutDescriptor desc{};
			desc.entryCount = 1;
			desc.entries    = &entry;
			desc.label      = "gizmo-bindgroup-layout";
			return device.CreateBindGroupLayout(&desc);
		}

		wgpu::PipelineLayout BuildPipelineLayout(wgpu::Device device, wgpu::BindGroupLayout bgl) {
			wgpu::BindGroupLayout bgls[1] = { bgl };
			wgpu::PipelineLayoutDescriptor desc{};
			desc.bindGroupLayoutCount = 1;
			desc.bindGroupLayouts     = bgls;
			desc.label                = "gizmo-pipeline-layout";
			return device.CreatePipelineLayout(&desc);
		}

		wgpu::RenderPipeline BuildGizmoPipeline(wgpu::Device device,
			wgpu::ShaderModule module, wgpu::PipelineLayout layout,
			wgpu::TextureFormat colorFormat, bool hasDepth)
		{
			// Vertex layout: 1 buffer, 2 attributes.
			//   loc 0: position (Float32x3, offset 0)
			//   loc 1: color    (Unorm8x4,  offset 12)  ← decoded from the
			//          packed uint32 PosColorVertex::color field.
			wgpu::VertexAttribute attrs[2] = {};
			attrs[0].format         = wgpu::VertexFormat::Float32x3;
			attrs[0].offset         = offsetof(PosColorVertex, x);
			attrs[0].shaderLocation = 0;
			attrs[1].format         = wgpu::VertexFormat::Unorm8x4;
			attrs[1].offset         = offsetof(PosColorVertex, color);
			attrs[1].shaderLocation = 1;

			wgpu::VertexBufferLayout vbl{};
			vbl.arrayStride    = sizeof(PosColorVertex);  // 16
			vbl.stepMode       = wgpu::VertexStepMode::Vertex;
			vbl.attributeCount = 2;
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
			prim.topology         = wgpu::PrimitiveTopology::LineList;
			// LineList topology doesn't need a strip-index-format (only used
			// by *Strip topologies); explicit Undefined matches the spec.
			prim.stripIndexFormat = wgpu::IndexFormat::Undefined;
			prim.frontFace        = wgpu::FrontFace::CCW;
			prim.cullMode         = wgpu::CullMode::None;

			wgpu::DepthStencilState depthState{};
			if (hasDepth) {
				depthState.format            = wgpu::TextureFormat::Depth24PlusStencil8;
				depthState.depthWriteEnabled = false;  // gizmos don't write depth
				depthState.depthCompare      = wgpu::CompareFunction::Always;
			}

			wgpu::RenderPipelineDescriptor desc{};
			desc.label  = "gizmo-pipeline";
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

		wgpu::RenderPipeline GetOrBuildPipeline(wgpu::TextureFormat colorFormat, bool hasDepth) {
			if (!g_Module || !g_PipelineLayout) return nullptr;
			const uint32_t key = MakePipelineKey(colorFormat, hasDepth);
			auto it = g_PipelineCache.find(key);
			if (it != g_PipelineCache.end()) return it->second;
			wgpu::Device device = WebGPUBackend::GetDevice();
			if (!device) return nullptr;
			wgpu::RenderPipeline pipeline = BuildGizmoPipeline(
				device, g_Module, g_PipelineLayout, colorFormat, hasDepth);
			if (!pipeline) return nullptr;
			g_PipelineCache.emplace(key, pipeline);
			return pipeline;
		}

		bool EnsureUniformBuffer(wgpu::Device device) {
			if (g_UniformBuffer) return true;
			wgpu::BufferDescriptor desc{};
			desc.size  = 64;  // mat4x4<f32>
			desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
			desc.label = "gizmo-viewproj-ubo";
			g_UniformBuffer = device.CreateBuffer(&desc);
			return static_cast<bool>(g_UniformBuffer);
		}

		bool EnsureBindGroup(wgpu::Device device) {
			if (g_BindGroup) return true;
			if (!g_BindGroupLayout || !g_UniformBuffer) return false;

			wgpu::BindGroupEntry entry{};
			entry.binding = 0;
			entry.buffer  = g_UniformBuffer;
			entry.offset  = 0;
			entry.size    = 64;

			wgpu::BindGroupDescriptor desc{};
			desc.layout     = g_BindGroupLayout;
			desc.entryCount = 1;
			desc.entries    = &entry;
			desc.label      = "gizmo-bindgroup";
			g_BindGroup = device.CreateBindGroup(&desc);
			return static_cast<bool>(g_BindGroup);
		}

		bool EnsureVertexBuffer(wgpu::Device device, uint32_t neededBytes) {
			if (g_VertexBufferCapacityBytes >= neededBytes && g_VertexBuffer) return true;
			uint32_t cap = g_VertexBufferCapacityBytes > 0
				? g_VertexBufferCapacityBytes
				: 4096u;  // 256 lines × 16 bytes = 4 KB initial
			while (cap < neededBytes) cap *= 2;

			wgpu::BufferDescriptor desc{};
			desc.size  = cap;
			desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
			desc.label = "gizmo-vertex-buffer";
			wgpu::Buffer buf = device.CreateBuffer(&desc);
			if (!buf) return false;
			g_VertexBuffer              = std::move(buf);
			g_VertexBufferCapacityBytes = cap;
			return true;
		}
	}  // anonymous namespace

	bool GizmoRenderer2D::Initialize() {
		if (m_IsInitialized) return true;
		if (!WebGPUBackend::IsInitialized()) {
			IDX_CORE_ERROR_TAG("GizmoRenderer",
				"Initialize called before WebGPU backend initialized");
			return false;
		}

		// Shader_WebGPU's built-in registry resolves "gizmo" to the embedded
		// WGSL. Same vs/fs path pattern Renderer2D + TextRenderer use.
		m_GizmoShader = std::make_unique<Shader>(
			std::string("IndexAssets/Shaders/GizmoShader.vs"),
			std::string("IndexAssets/Shaders/GizmoShader.fs"));
		if (!m_GizmoShader || !m_GizmoShader->IsValid()) {
			IDX_CORE_ERROR_TAG("GizmoRenderer",
				"Gizmo shader failed to load — gizmos disabled");
			m_GizmoShader.reset();
			return false;
		}
		const auto lookup = WebGPUBackend::LookupShader(m_GizmoShader->GetHandle());
		if (!lookup.Valid) {
			IDX_CORE_ERROR_TAG("GizmoRenderer",
				"Gizmo shader has no module in WebGPUBackend pool");
			m_GizmoShader.reset();
			return false;
		}
		g_Module = lookup.Module;

		wgpu::Device device = WebGPUBackend::GetDevice();
		g_BindGroupLayout = BuildBindGroupLayout(device);
		g_PipelineLayout  = BuildPipelineLayout(device, g_BindGroupLayout);
		if (!g_BindGroupLayout || !g_PipelineLayout) {
			IDX_CORE_ERROR_TAG("GizmoRenderer",
				"Failed to build bindgroup / pipeline layout");
			g_Module          = nullptr;
			g_BindGroupLayout = nullptr;
			g_PipelineLayout  = nullptr;
			m_GizmoShader.reset();
			return false;
		}

		m_IsInitialized = true;
		return true;
	}

	void GizmoRenderer2D::Shutdown() {
		if (!m_IsInitialized) return;
		g_PipelineCache.clear();
		g_VertexBuffer              = nullptr;
		g_VertexBufferCapacityBytes = 0;
		g_BindGroup                 = nullptr;
		g_UniformBuffer             = nullptr;
		g_PipelineLayout            = nullptr;
		g_BindGroupLayout           = nullptr;
		g_Module                    = nullptr;
		m_GizmoShader.reset();

		m_GizmoIndices.clear();
		m_GizmoIndices.shrink_to_fit();
		s_UploadBuffer.clear();
		s_UploadBuffer.shrink_to_fit();
		m_IsInitialized = false;
	}

	void GizmoRenderer2D::OnResize(int /*w*/, int /*h*/) {}

	void GizmoRenderer2D::BeginFrame(uint16_t viewId) {
		// viewId is unused under WebGPU — the active framebuffer is set
		// by RenderApi::BindFramebuffer before this call. Stored for ABI
		// but not consulted at submit time.
		m_GizmoViewId = viewId;
	}

	void GizmoRenderer2D::EndFrame() {}

	void GizmoRenderer2D::RenderWithVP(const glm::mat4& vp, GizmoLayerMask layerMask) {
		if (!m_IsInitialized) return;
		std::span<const PosColorVertex> verts = BuildGeometry(layerMask);
		FlushGizmosImpl(vp, verts);
		Gizmo::Clear();
	}

	std::span<const PosColorVertex>
	GizmoRenderer2D::BuildGeometry(GizmoLayerMask layerMask) {
		// CPU geometry build — squares fan into 4 edges; circles into N
		// segments; lines pass-through. Storage comes from the per-frame
		// arena: one bump-alloc up front, no per-shape push_back, the whole
		// buffer is reclaimed in O(1) by Application::EndFrame.
		//
		// Upper-bound vertex count, ignoring the layer-mask filter — the
		// cursor below tracks the actual count and the arena slack is
		// reclaimed at EndFrame regardless, so over-allocation is free.
		std::size_t cap = Gizmo::s_Squares.size() * 8   // 4 edges × 2 verts
		                + Gizmo::s_Lines.size()   * 2;
		for (const Circle& ci : Gizmo::s_Circles) {
			if (ci.Segments >= 3) {
				cap += static_cast<std::size_t>(ci.Segments) * 2;
			}
		}
		if (cap == 0) return {};

		std::span<PosColorVertex> buf =
			FrameArenas::Frame().CreateArray<PosColorVertex>(cap);
		if (buf.empty()) {
			// Arena exhausted this frame. Log once per session — repeated
			// warnings would spam the console every frame the scene stays
			// over budget. The gizmo pass is silently skipped this frame;
			// the arena resets at EndFrame and the next frame retries.
			static bool s_OverflowLogged = false;
			if (!s_OverflowLogged) {
				IDX_CORE_WARN_TAG("GizmoRenderer",
					"Frame arena exhausted: {} verts requested ({} bytes). "
					"Increase ApplicationConfig::FrameArenaCapacityBytes.",
					cap, cap * sizeof(PosColorVertex));
				s_OverflowLogged = true;
			}
			return {};
		}

		PosColorVertex* out = buf.data();
		std::size_t cursor = 0;

		for (const Square& sq : Gizmo::s_Squares) {
			if (!HasAnyLayer(sq.Layer, layerMask)) continue;
			const uint32_t rgba = PackRgba(sq.Color);
			const float cx = sq.Center.x;
			const float cy = sq.Center.y;
			const float hx = sq.HalfExtents.x;
			const float hy = sq.HalfExtents.y;
			const float c  = std::cos(sq.Radiant);
			const float s  = std::sin(sq.Radiant);
			auto corner = [&](float lx, float ly) {
				return std::pair<float, float>{
					cx + lx * c - ly * s,
					cy + lx * s + ly * c
				};
			};
			const auto [x0, y0] = corner(-hx, -hy);
			const auto [x1, y1] = corner(+hx, -hy);
			const auto [x2, y2] = corner(+hx, +hy);
			const auto [x3, y3] = corner(-hx, +hy);
			EmitLine(out, cursor, x0, y0, x1, y1, rgba);
			EmitLine(out, cursor, x1, y1, x2, y2, rgba);
			EmitLine(out, cursor, x2, y2, x3, y3, rgba);
			EmitLine(out, cursor, x3, y3, x0, y0, rgba);
		}

		for (const Line& ln : Gizmo::s_Lines) {
			if (!HasAnyLayer(ln.Layer, layerMask)) continue;
			EmitLine(out, cursor, ln.Start.x, ln.Start.y, ln.End.x, ln.End.y, PackRgba(ln.Color));
		}

		for (const Circle& ci : Gizmo::s_Circles) {
			if (!HasAnyLayer(ci.Layer, layerMask)) continue;
			if (ci.Segments < 3) continue;
			const uint32_t rgba = PackRgba(ci.Color);
			const float step = 6.28318530717958647692f / static_cast<float>(ci.Segments);
			float prevX = ci.Center.x + ci.Radius;
			float prevY = ci.Center.y;
			for (int i = 1; i <= ci.Segments; ++i) {
				const float a = static_cast<float>(i) * step;
				const float nx = ci.Center.x + std::cos(a) * ci.Radius;
				const float ny = ci.Center.y + std::sin(a) * ci.Radius;
				EmitLine(out, cursor, prevX, prevY, nx, ny, rgba);
				prevX = nx;
				prevY = ny;
			}
		}

		return buf.first(cursor);
	}

	void GizmoRenderer2D::FlushGizmosImpl(const glm::mat4& vp,
		std::span<const PosColorVertex> verts)
	{
		if (verts.empty()) return;

		auto target = WebGPUBackend::BeginRenderToCurrentTarget();
		if (!target.Valid) return;

		wgpu::Device device = WebGPUBackend::GetDevice();
		wgpu::Queue  queue  = WebGPUBackend::GetQueue();
		if (!device || !queue) return;

		wgpu::RenderPipeline pipeline = GetOrBuildPipeline(target.ColorFormat, target.HasDepth);
		if (!pipeline) {
			IDX_CORE_WARN_TAG("GizmoRenderer",
				"No pipeline for color-format {} hasDepth={} — gizmos not submitted",
				static_cast<int>(target.ColorFormat), target.HasDepth);
			return;
		}

		if (!EnsureUniformBuffer(device)) return;
		if (!EnsureBindGroup(device))    return;
		queue.WriteBuffer(g_UniformBuffer, 0, glm::value_ptr(vp), 64);

		const uint32_t numVerts = static_cast<uint32_t>(verts.size());
		const uint32_t totalBytes = numVerts * static_cast<uint32_t>(sizeof(PosColorVertex));
		if (!EnsureVertexBuffer(device, totalBytes)) return;
		queue.WriteBuffer(g_VertexBuffer, 0, verts.data(), totalBytes);

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
		passDesc.label                  = "gizmo-pass";
		passDesc.colorAttachmentCount   = 1;
		passDesc.colorAttachments       = &colorAtt;
		passDesc.depthStencilAttachment = target.HasDepth ? &depthAtt : nullptr;

		wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
		pass.SetPipeline(pipeline);
		pass.SetBindGroup(0, g_BindGroup);
		pass.SetVertexBuffer(0, g_VertexBuffer);
		pass.Draw(numVerts, /*instanceCount=*/1, /*firstVertex=*/0, /*firstInstance=*/0);
		pass.End();

		if (target.IsSwapChain) {
			WebGPUBackend::MarkSwapChainRendered();
		}
	}

}  // namespace Index
