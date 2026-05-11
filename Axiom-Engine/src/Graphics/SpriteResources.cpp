#include "pch.hpp"
#include "Graphics/SpriteResources.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/Shader.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

// =============================================================================
// WebGPUSpriteResources — implementation. See header for design rationale.
// =============================================================================

namespace Axiom::WebGPUSpriteResources {

	namespace {
		// Unit quad in [-0.5, 0.5]². Identical to BgfxSpriteResources's
		// k_QuadVerts so the WGSL sprite shader can use the same
		// `a_position.xy + 0.5` -> UV mapping (with the Y flip baked into
		// the shader, not the geometry).
		struct QuadVertex {
			float X, Y, Z;
		};
		constexpr QuadVertex k_QuadVerts[] = {
			{ -0.5f, -0.5f, 0.0f },
			{  0.5f, -0.5f, 0.0f },
			{  0.5f,  0.5f, 0.0f },
			{ -0.5f,  0.5f, 0.0f },
		};
		constexpr uint16_t k_QuadIndices[] = { 0, 1, 2, 0, 2, 3 };

		// Pipeline cache key: pack (colorFormat << 1) | hasDepth into a
		// single 32-bit value. Format enums are uint32_t in webgpu_cpp.h;
		// the shift keeps headroom and the LSB carries the hasDepth bit.
		uint32_t MakePipelineKey(wgpu::TextureFormat fmt, bool hasDepth) {
			return (static_cast<uint32_t>(fmt) << 1) | (hasDepth ? 1u : 0u);
		}

		struct Resources {
			std::unique_ptr<Shader> SpriteShader;       // owns the wgpu::ShaderModule via Shader::~Shader
			wgpu::ShaderModule      SpriteModule;       // cached fast-path; same handle Shader holds
			wgpu::Buffer            QuadVertexBuffer;
			wgpu::Buffer            QuadIndexBuffer;
			wgpu::BindGroupLayout   BindGroupLayout;
			wgpu::PipelineLayout    PipelineLayout;
			std::unordered_map<uint32_t, wgpu::RenderPipeline> PipelineCache;
		};

		int        g_RefCount = 0;
		bool       g_Ready    = false;
		Resources  g_Res;

		// ── Helpers ─────────────────────────────────────────────────────────

		wgpu::Buffer CreateInitialisedBuffer(wgpu::Device device, wgpu::BufferUsage usage,
			const void* data, size_t byteSize, const char* label)
		{
			wgpu::BufferDescriptor desc{};
			desc.size  = byteSize;
			desc.usage = usage;
			desc.mappedAtCreation = true;
			desc.label = label;
			wgpu::Buffer buf = device.CreateBuffer(&desc);
			if (!buf) return nullptr;
			void* mapped = buf.GetMappedRange();
			if (!mapped) {
				AIM_CORE_ERROR_TAG("WebGPUSpriteResources",
					"GetMappedRange returned null for '{}'", label);
				return nullptr;
			}
			std::memcpy(mapped, data, byteSize);
			buf.Unmap();
			return buf;
		}

		wgpu::BindGroupLayout CreateSpriteBindGroupLayout(wgpu::Device device) {
			wgpu::BindGroupLayoutEntry entries[3] = {};

			// Binding 0: viewProj uniform (mat4x4<f32>).
			entries[0].binding    = 0;
			entries[0].visibility = wgpu::ShaderStage::Vertex;
			entries[0].buffer.type           = wgpu::BufferBindingType::Uniform;
			entries[0].buffer.hasDynamicOffset = false;
			entries[0].buffer.minBindingSize = 64;  // mat4x4<f32> = 64 bytes

			// Binding 1: t_albedo (sampled texture).
			entries[1].binding    = 1;
			entries[1].visibility = wgpu::ShaderStage::Fragment;
			entries[1].texture.sampleType    = wgpu::TextureSampleType::Float;
			entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
			entries[1].texture.multisampled  = false;

			// Binding 2: s_albedo (sampler).
			entries[2].binding    = 2;
			entries[2].visibility = wgpu::ShaderStage::Fragment;
			entries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

			wgpu::BindGroupLayoutDescriptor desc{};
			desc.entryCount = 3;
			desc.entries    = entries;
			desc.label      = "sprite-bindgroup-layout";
			return device.CreateBindGroupLayout(&desc);
		}

		wgpu::PipelineLayout CreateSpritePipelineLayout(wgpu::Device device,
			wgpu::BindGroupLayout bgl)
		{
			wgpu::BindGroupLayout bgls[1] = { bgl };
			wgpu::PipelineLayoutDescriptor desc{};
			desc.bindGroupLayoutCount = 1;
			desc.bindGroupLayouts     = bgls;
			desc.label                = "sprite-pipeline-layout";
			return device.CreatePipelineLayout(&desc);
		}

		wgpu::RenderPipeline CreateSpritePipeline(wgpu::Device device,
			wgpu::ShaderModule module, wgpu::PipelineLayout layout,
			wgpu::TextureFormat colorFormat, bool hasDepth)
		{
			// Two vertex buffers:
			//   Buffer 0 (per-vertex):   position vec3<f32>, stride 12
			//   Buffer 1 (per-instance): three vec4<f32>, stride 48
			wgpu::VertexAttribute vertexAttrs[1] = {};
			vertexAttrs[0].format         = wgpu::VertexFormat::Float32x3;
			vertexAttrs[0].offset         = 0;
			vertexAttrs[0].shaderLocation = 0;

			wgpu::VertexAttribute instanceAttrs[3] = {};
			instanceAttrs[0].format         = wgpu::VertexFormat::Float32x4;
			instanceAttrs[0].offset         = 0;
			instanceAttrs[0].shaderLocation = 1;
			instanceAttrs[1].format         = wgpu::VertexFormat::Float32x4;
			instanceAttrs[1].offset         = 16;
			instanceAttrs[1].shaderLocation = 2;
			instanceAttrs[2].format         = wgpu::VertexFormat::Float32x4;
			instanceAttrs[2].offset         = 32;
			instanceAttrs[2].shaderLocation = 3;

			wgpu::VertexBufferLayout buffers[2] = {};
			buffers[0].arrayStride    = sizeof(QuadVertex);
			buffers[0].stepMode       = wgpu::VertexStepMode::Vertex;
			buffers[0].attributeCount = 1;
			buffers[0].attributes     = vertexAttrs;

			buffers[1].arrayStride    = sizeof(SpriteInstance);
			buffers[1].stepMode       = wgpu::VertexStepMode::Instance;
			buffers[1].attributeCount = 3;
			buffers[1].attributes     = instanceAttrs;

			// Alpha-blended colour target — engine default for sprites and UI
			// matches BgfxSpriteResources's BGFX_STATE_BLEND_ALPHA recipe.
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
			fragState.module        = module;
			fragState.entryPoint    = "fs_main";
			fragState.targetCount   = 1;
			fragState.targets       = &colorTarget;

			wgpu::PrimitiveState prim{};
			prim.topology         = wgpu::PrimitiveTopology::TriangleList;
			prim.stripIndexFormat = wgpu::IndexFormat::Undefined;
			prim.frontFace        = wgpu::FrontFace::CCW;
			prim.cullMode         = wgpu::CullMode::None;  // 2D quads, no culling

			wgpu::DepthStencilState depthState{};
			if (hasDepth) {
				depthState.format            = wgpu::TextureFormat::Depth24PlusStencil8;
				depthState.depthWriteEnabled = false;  // sprites don't write depth
				depthState.depthCompare      = wgpu::CompareFunction::Always;
				// Stencil left at defaults (Always / Keep) — no stencil work in Stage 4.
			}

			wgpu::RenderPipelineDescriptor desc{};
			desc.label  = "sprite-pipeline";
			desc.layout = layout;

			desc.vertex.module      = module;
			desc.vertex.entryPoint  = "vs_main";
			desc.vertex.bufferCount = 2;
			desc.vertex.buffers     = buffers;

			desc.fragment = &fragState;
			desc.primitive = prim;
			desc.depthStencil = hasDepth ? &depthState : nullptr;

			desc.multisample.count = 1;
			desc.multisample.mask  = 0xFFFFFFFF;

			return device.CreateRenderPipeline(&desc);
		}

		// ── Build / destroy --------------------------------------------------
		bool BuildResources() {
			if (!WebGPUBackend::IsInitialized()) {
				AIM_CORE_ERROR_TAG("WebGPUSpriteResources",
					"Acquire called before WebGPU backend initialized");
				return false;
			}
			wgpu::Device device = WebGPUBackend::GetDevice();
			if (!device) return false;

			// Shader — same name BgfxSpriteResources passes
			// ("SpriteShader.vs" -> stem "sprite"), now resolved by
			// Shader_WebGPU.cpp via its built-in WGSL registry.
			g_Res.SpriteShader = std::make_unique<Shader>(
				std::string("AxiomAssets/Shaders/SpriteShader.vs"),
				std::string("AxiomAssets/Shaders/SpriteShader.fs"));
			if (!g_Res.SpriteShader || !g_Res.SpriteShader->IsValid()) {
				AIM_CORE_ERROR_TAG("WebGPUSpriteResources",
					"Sprite shader failed to load — sprite + UI submit paths disabled");
				return false;
			}
			const auto lookup = WebGPUBackend::LookupShader(g_Res.SpriteShader->GetHandle());
			if (!lookup.Valid) {
				AIM_CORE_ERROR_TAG("WebGPUSpriteResources",
					"Sprite shader has no module in WebGPUBackend pool");
				return false;
			}
			g_Res.SpriteModule = lookup.Module;

			g_Res.QuadVertexBuffer = CreateInitialisedBuffer(
				device, wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
				k_QuadVerts, sizeof(k_QuadVerts), "sprite-quad-vbo");
			if (!g_Res.QuadVertexBuffer) {
				AIM_CORE_ERROR_TAG("WebGPUSpriteResources", "Failed to create quad VBO");
				return false;
			}

			g_Res.QuadIndexBuffer = CreateInitialisedBuffer(
				device, wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst,
				k_QuadIndices, sizeof(k_QuadIndices), "sprite-quad-ibo");
			if (!g_Res.QuadIndexBuffer) {
				AIM_CORE_ERROR_TAG("WebGPUSpriteResources", "Failed to create quad IBO");
				return false;
			}

			g_Res.BindGroupLayout = CreateSpriteBindGroupLayout(device);
			if (!g_Res.BindGroupLayout) {
				AIM_CORE_ERROR_TAG("WebGPUSpriteResources", "Failed to create bind group layout");
				return false;
			}
			g_Res.PipelineLayout = CreateSpritePipelineLayout(device, g_Res.BindGroupLayout);
			if (!g_Res.PipelineLayout) {
				AIM_CORE_ERROR_TAG("WebGPUSpriteResources", "Failed to create pipeline layout");
				return false;
			}

			return true;
		}

		void DestroyResources() {
			g_Res.PipelineCache.clear();
			g_Res.PipelineLayout    = nullptr;
			g_Res.BindGroupLayout   = nullptr;
			g_Res.QuadIndexBuffer   = nullptr;
			g_Res.QuadVertexBuffer  = nullptr;
			g_Res.SpriteModule      = nullptr;
			g_Res.SpriteShader.reset();
		}
	}

	// ── Public API ──────────────────────────────────────────────────────────

	bool Acquire() {
		if (g_RefCount++ == 0) {
			g_Ready = BuildResources();
			if (!g_Ready) {
				DestroyResources();
				--g_RefCount;
			}
		}
		return g_Ready;
	}

	void Release() {
		if (g_RefCount == 0) return;
		if (--g_RefCount == 0) {
			DestroyResources();
			g_Ready = false;
		}
	}

	bool IsReady() { return g_Ready; }

	wgpu::ShaderModule    GetSpriteModule()    { return g_Res.SpriteModule; }
	wgpu::Buffer          GetQuadVertexBuffer(){ return g_Res.QuadVertexBuffer; }
	wgpu::Buffer          GetQuadIndexBuffer() { return g_Res.QuadIndexBuffer; }
	wgpu::BindGroupLayout GetBindGroupLayout() { return g_Res.BindGroupLayout; }
	wgpu::PipelineLayout  GetPipelineLayout()  { return g_Res.PipelineLayout; }

	wgpu::RenderPipeline GetSpritePipeline(wgpu::TextureFormat colorFormat, bool hasDepth) {
		if (!g_Ready) return nullptr;

		const uint32_t key = MakePipelineKey(colorFormat, hasDepth);
		auto it = g_Res.PipelineCache.find(key);
		if (it != g_Res.PipelineCache.end()) return it->second;

		wgpu::Device device = WebGPUBackend::GetDevice();
		if (!device) return nullptr;

		wgpu::RenderPipeline pipeline = CreateSpritePipeline(
			device, g_Res.SpriteModule, g_Res.PipelineLayout, colorFormat, hasDepth);
		if (!pipeline) {
			AIM_CORE_ERROR_TAG("WebGPUSpriteResources",
				"Failed to create pipeline for color format {} (hasDepth={})",
				static_cast<int>(colorFormat), hasDepth);
			return nullptr;
		}
		g_Res.PipelineCache.emplace(key, pipeline);
		return pipeline;
	}

	void EncodeInstance44(const Instance44& src, SpriteInstance& dst) {
		dst.Pos[0]   = src.Position.x;
		dst.Pos[1]   = src.Position.y;
		dst.Scale[0] = src.Scale.x;
		dst.Scale[1] = src.Scale.y;
		dst.Color[0] = src.Color.r;
		dst.Color[1] = src.Color.g;
		dst.Color[2] = src.Color.b;
		dst.Color[3] = src.Color.a;
		const float c = std::cos(src.Rotation);
		const float s = std::sin(src.Rotation);
		dst.Rot[0] = c;
		dst.Rot[1] = s;
		dst.Rot[2] = 0.0f;
		dst.Rot[3] = 0.0f;
	}

}  // namespace Axiom::WebGPUSpriteResources
