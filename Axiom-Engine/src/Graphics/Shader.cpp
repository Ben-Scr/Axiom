#include "pch.hpp"
#include "Graphics/Shader.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Path.hpp"

#include <webgpu/webgpu_cpp.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

// =============================================================================
// Shader — WebGPU (Dawn) implementation. Stage 3 of the WebGPU port.
// -----------------------------------------------------------------------------
// Sibling to Shader.cpp (the bgfx implementation). Premake selects this file
// under --rhi=webgpu and excludes Shader.cpp; the shared Shader.hpp keeps the
// public API stable across both — `unsigned m_Program` is still an opaque ID
// with 0 = invalid, callers just don't reach for a bgfx::ProgramHandle out of it.
//
// Two ways a shader name comes in here:
//   1. Built-in (sprite, gizmo, text, ...). Resolved via the static table
//      below — WGSL source is embedded in this TU so a release build doesn't
//      need .wgsl files on disk to bring up the engine's own pipelines.
//   2. User / project (.wgsl on disk). Path is the vsPath argument's stem —
//      e.g. `AxiomAssets/Shaders/SpriteShader.vs` -> lookup
//      `AxiomAssets/Shaders/webgpu/sprite.wgsl` first under the executable
//      dir, then under the source tree. Same precedence as the bgfx side's
//      .bin lookup, just a different on-disk format.
//
// Why one module per Shader (not vs+fs separately):
// WGSL is multi-entry-point. The sprite module here has both `vs_main` and
// `fs_main`; we don't pre-split it into two modules because Dawn's pipeline
// creation takes (module, entry_point) per stage and can re-use the same
// module across stages. The renderer side (Stage 4) knows the entry-point
// names; this class only owns the module.
//
// The Stage 4 renderer ports (Renderer2D, GuiRenderer, TextRenderer,
// GizmoRenderer) will look up the wgpu::ShaderModule via
// WebGPUBackend::LookupShader(m_Program) and feed it into a
// wgpu::RenderPipeline along with the bind-group layout + target format.
// =============================================================================

namespace Axiom {

	namespace {
		// ── Built-in WGSL registry ──────────────────────────────────────────
		// Sprite shader — used by Renderer2D (world-space sprites) and
		// GuiRenderer (screen-space UI quads). Mirrors the bgfx-side
		// vs_sprite/fs_sprite semantics:
		//   * Vertex inputs: unit-quad position (loc 0) + 3 instance vec4s
		//     (loc 1..3) carrying (Pos.xy, Scale.xy), (Color RGBA), and
		//     (cos, sin, _, _) — packed identically to BgfxSpriteResources's
		//     SpriteInstance / WebGPUSpriteResources's matching layout.
		//   * Bind group 0: { 0: u_ViewProj (mat4 uniform), 1: albedo
		//     texture, 2: albedo sampler }. The matrix uses column-major
		//     storage — matches how the engine's math layer pushes it.
		constexpr const char* k_SpriteWGSL = R"WGSL(
struct Uniforms {
	viewProj: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var t_albedo: texture_2d<f32>;
@group(0) @binding(2) var s_albedo: sampler;

struct VertexInput {
	@location(0) position: vec3<f32>,
	@location(1) i_data0: vec4<f32>,  // Pos.xy, Scale.xy
	@location(2) i_data1: vec4<f32>,  // Color RGBA
	@location(3) i_data2: vec4<f32>,  // cos, sin, _, _
};

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) color: vec4<f32>,
	@location(1) uv: vec2<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
	// Per-instance scale, then rotate by (cos, sin), then translate.
	let scaled = in.position.xy * in.i_data0.zw;
	let c = in.i_data2.x;
	let s = in.i_data2.y;
	let rotated = vec2<f32>(
		scaled.x * c - scaled.y * s,
		scaled.x * s + scaled.y * c
	);
	let world = rotated + in.i_data0.xy;

	var out: VertexOutput;
	out.clip_position = u.viewProj * vec4<f32>(world, 0.0, 1.0);
	out.color = in.i_data1;
	// UV maps [-0.5, 0.5] -> [0, 1] on X, and flips Y so texture top-left
	// shows at the quad's visual top (engine has Y-up world space; textures
	// are top-left origin per stb's load).
	out.uv = vec2<f32>(in.position.x + 0.5, 0.5 - in.position.y);
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let texel = textureSample(t_albedo, s_albedo, in.uv);
	return texel * in.color;
}
)WGSL";

		// Text shader — used by TextRenderer (Stage 6). The atlas is an R8
		// alpha texture baked by stbtt_pack; the fragment shader pulls the
		// red channel as coverage and modulates it into the per-vertex
		// color's alpha. Vertices are per-glyph (6 verts per glyph, not
		// instanced) because each glyph has a unique (UV, position) pair.
		constexpr const char* k_TextWGSL = R"WGSL(
struct Uniforms {
	viewProj: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var t_atlas: texture_2d<f32>;
@group(0) @binding(2) var s_atlas: sampler;

struct VertexInput {
	@location(0) position: vec2<f32>,
	@location(1) uv:       vec2<f32>,
	@location(2) color:    vec4<f32>,
};

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) color: vec4<f32>,
	@location(1) uv:    vec2<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
	var out: VertexOutput;
	out.clip_position = u.viewProj * vec4<f32>(in.position, 0.0, 1.0);
	out.color = in.color;
	out.uv = in.uv;
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let alpha = textureSample(t_atlas, s_atlas, in.uv).r;
	return vec4<f32>(in.color.rgb, in.color.a * alpha);
}
)WGSL";

		// Gizmo shader — used by GizmoRenderer2D for debug line drawing
		// (squares, circles, raw lines). Vertex layout: 1 buffer with
		// position vec3<f32> + color packed as Unorm8x4 (the engine's
		// PosColorVertex stores 4 bytes RGBA as a uint32 — Dawn decodes
		// it through Unorm8x4 at the vertex-fetch stage). No texture
		// sampling, no per-vertex UV — just pass color through to the
		// fragment shader. Pipeline uses LineList primitive topology.
		constexpr const char* k_GizmoWGSL = R"WGSL(
struct Uniforms {
	viewProj: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;

struct VertexInput {
	@location(0) position: vec3<f32>,
	@location(1) color:    vec4<f32>,
};

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) color: vec4<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
	var out: VertexOutput;
	out.clip_position = u.viewProj * vec4<f32>(in.position, 1.0);
	out.color = in.color;
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	return in.color;
}
)WGSL";

		struct BuiltIn {
			std::string_view Name;
			const char*      WGSL;
		};
		// Each entry maps a "name" (the stem of vsPath after stripping the
		// "Shader" suffix and lowercasing — matches the bgfx ExtractName
		// convention) to its embedded WGSL source. Stage 3 = sprite,
		// Stage 6 = text, Stage 7 = gizmo. Future renderer ports register
		// here as they land.
		constexpr BuiltIn k_BuiltIns[] = {
			{ "sprite", k_SpriteWGSL },
			{ "text",   k_TextWGSL   },
			{ "gizmo",  k_GizmoWGSL  },
		};

		// ── Pool ────────────────────────────────────────────────────────────
		struct GpuShader {
			wgpu::ShaderModule Module;
			std::string        Name;
		};
		std::unordered_map<unsigned, GpuShader> g_Shaders;
		unsigned g_NextShaderId = 1;  // 0 reserved as "invalid"

		unsigned AllocateShaderSlot(GpuShader&& s) {
			unsigned id = g_NextShaderId++;
			if (id == 0) id = g_NextShaderId++;
			g_Shaders.emplace(id, std::move(s));
			return id;
		}

		void FreeShaderSlot(unsigned id) {
			if (id == 0) return;
			g_Shaders.erase(id);
		}

		// Stem extraction — matches Shader.cpp::ExtractName's behaviour so
		// the same callers (BgfxSpriteResources / WebGPUSpriteResources /
		// GizmoRenderer / TextRenderer) hit the same name regardless of
		// backend. Strips path + extension, drops "Shader" suffix, lowercases.
		std::string ExtractName(const std::string& path) {
			std::filesystem::path p(path);
			std::string stem = p.stem().string();
			static const std::string suffix = "Shader";
			if (stem.size() > suffix.size()
				&& stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0)
			{
				stem.erase(stem.size() - suffix.size());
			}
			for (char& c : stem) {
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			}
			return stem;
		}

		const char* FindBuiltinWGSL(std::string_view name) {
			for (const BuiltIn& b : k_BuiltIns) {
				if (b.Name == name) return b.WGSL;
			}
			return nullptr;
		}

		std::string ResolveOnDiskWGSL(const std::string& name) {
			const std::string rel = std::string("AxiomAssets/Shaders/webgpu/") + name + ".wgsl";
			const std::string exeRel = Path::Combine(Path::ExecutableDir(), rel);
			if (std::filesystem::exists(exeRel)) return exeRel;
			if (std::filesystem::exists(rel)) return rel;
			return {};
		}

		// Build a wgpu::ShaderModule from WGSL text. Returns null module on
		// failure. The compilation-info callback fires asynchronously on
		// Dawn — we surface errors via the device's uncaptured-error hook
		// set up in WebGPUApi.cpp::RequestDeviceSync.
		wgpu::ShaderModule CompileWGSL(const std::string& name, std::string_view wgsl) {
			wgpu::Device device = WebGPUBackend::GetDevice();
			if (!device) {
				AIM_CORE_ERROR_TAG("Shader",
					"CompileWGSL '{}' called before WebGPU device exists", name);
				return nullptr;
			}

			wgpu::ShaderSourceWGSL src{};
			src.code = wgsl.data();

			wgpu::ShaderModuleDescriptor desc{};
			desc.nextInChain = &src;
			desc.label = name.c_str();

			wgpu::ShaderModule module = device.CreateShaderModule(&desc);
			if (!module) {
				AIM_CORE_ERROR_TAG("Shader",
					"wgpu::Device::CreateShaderModule returned null for '{}'", name);
			}
			return module;
		}
	}

	// ── WebGPUBackend::LookupShader (declared in WebGPUBackend.hpp) ─────────
	namespace WebGPUBackend {
		ShaderLookup LookupShader(unsigned shaderHandleId) {
			if (shaderHandleId == 0) return ShaderLookup{};
			auto it = g_Shaders.find(shaderHandleId);
			if (it == g_Shaders.end()) return ShaderLookup{};
			ShaderLookup out;
			out.Module = it->second.Module;
			out.Valid  = static_cast<bool>(out.Module);
			return out;
		}
	}

	// ── Shader ──────────────────────────────────────────────────────────────

	Shader::Shader(const std::string& vsPath, const std::string& /*fsPath*/) {
		// vsPath / fsPath were the two-source-files convention from the
		// OpenGL era and carried through to the bgfx side (which also
		// extracts a stem and ignores fsPath). The WebGPU side does the
		// same — one module covers both stages.
		const std::string name = ExtractName(vsPath);

		std::string wgslHolder;          // owns the on-disk source if loaded
		std::string_view wgslSource;     // points into the holder OR a built-in

		if (const char* builtin = FindBuiltinWGSL(name)) {
			wgslSource = builtin;
		} else {
			const std::string path = ResolveOnDiskWGSL(name);
			if (path.empty()) {
				AIM_CORE_WARN_TAG("Shader",
					"No WGSL source for '{}' (no built-in, no AxiomAssets/Shaders/webgpu/{}.wgsl) — Shader will be invalid",
					name, name);
				return;
			}
			std::vector<uint8_t> bytes = File::ReadAllBytes(path);
			if (bytes.empty()) {
				AIM_CORE_WARN_TAG("Shader", "WGSL file empty: {}", path);
				return;
			}
			wgslHolder.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			wgslSource = wgslHolder;
		}

		wgpu::ShaderModule module = CompileWGSL(name, wgslSource);
		if (!module) return;

		GpuShader slot;
		slot.Module = std::move(module);
		slot.Name   = name;

		m_Program = AllocateShaderSlot(std::move(slot));
		m_IsValid = (m_Program != 0);
	}

	Shader::Shader(GLuint /*program*/) {}

	Shader::~Shader() {
		if (m_Program != 0) {
			FreeShaderSlot(m_Program);
			m_Program = 0;
			m_IsValid = false;
		}
	}

	Shader::Shader(Shader&& other) noexcept
		: m_Program(other.m_Program)
		, m_IsValid(other.m_IsValid)
	{
		other.m_Program = 0;
		other.m_IsValid = false;
	}

	Shader& Shader::operator=(Shader&& other) noexcept {
		if (this != &other) {
			if (m_Program != 0) FreeShaderSlot(m_Program);
			m_Program = other.m_Program;
			m_IsValid = other.m_IsValid;
			other.m_Program = 0;
			other.m_IsValid = false;
		}
		return *this;
	}

	Shader Shader::FromBinary(const std::string& binaryPath) {
		// WebGPU has no pre-compiled binary format equivalent to bgfx's
		// .bin (.wgsl is the canonical authoring format; SPIR-V via Tint
		// is an option but the engine doesn't ship a SPIR-V build step).
		// Treat the input as a stem and re-route through the regular
		// Shader(vsPath, fsPath) constructor — matches the bgfx behaviour
		// where FromBinary also re-derives the lookup from the stem.
		return Shader{ binaryPath, binaryPath };
	}

	bool Shader::ExportBinary(const std::string& /*outputPath*/) const {
		// No native binary representation worth exporting — WGSL text is
		// already the source-of-truth.
		return false;
	}

	Shader Shader::LoadWithBinaryCache(const std::string& /*binaryPath*/,
		const std::string& vsPath, const std::string& fsPath)
	{
		return Shader{ vsPath, fsPath };
	}

	void Shader::Submit() const {
		// WebGPU binds shader modules via wgpu::RenderPipeline at
		// pipeline-creation time, not per-draw. Renderers do that in
		// Stage 4; Submit stays a no-op like the bgfx side.
	}

	GLuint Shader::LoadAndCompile(GLenum /*type*/, const std::string& /*path*/) {
		// Legacy single-stage helper from the OpenGL era — unused under
		// both bgfx and WebGPU. Stub mirrors the bgfx impl.
		return 0;
	}

}  // namespace Axiom
