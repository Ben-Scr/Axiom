#include "pch.hpp"
#include "Graphics/Shader.hpp"

#include "Core/Log.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Path.hpp"

#include <bgfx/bgfx.h>

#include <filesystem>
#include <utility>

// =============================================================================
// Shader / Shader-binary loading — bgfx (Stage 2.2 of the bgfx port).
// -----------------------------------------------------------------------------
// Real `bgfx::createProgram` from compiled shader binaries on disk. The
// loader maps `<vsPath, fsPath>` (the legacy Shader.hpp ctor's interface
// designed around `vs.glsl` / `fs.glsl` source files for the OpenGL backend)
// to a per-renderer-profile lookup under
// `AxiomAssets/Shaders/bgfx/bin/<profile>/<vs|fs>_<name>.bin` — the suffix
// of `vsPath` is stripped, only the *name* matters.
//
// `m_Program` stores (bgfx::ProgramHandle::idx + 1) with 0 = invalid, same
// `IsValid()` contract the OpenGL impl had. SpriteShaderProgram_Bgfx +
// Renderer2D_Bgfx use this to bind the sprite shader and submit instanced
// quads.
// =============================================================================

namespace Axiom {

	namespace {
		constexpr unsigned EncodeProgram(uint16_t idx) noexcept {
			return static_cast<unsigned>(idx) + 1u;
		}
		constexpr uint16_t DecodeProgram(unsigned m) noexcept {
			return static_cast<uint16_t>(m - 1u);
		}
		bgfx::ProgramHandle FromMProgram(unsigned m) noexcept {
			if (m == 0) return BGFX_INVALID_HANDLE;
			return bgfx::ProgramHandle{ DecodeProgram(m) };
		}

		// Map bgfx's runtime renderer to the on-disk shader-binary subdir.
		// Layout matches the per-platform compile in compile.bat /
		// AxiomAssets/Shaders/bgfx/bin/{dx11,glsl,spirv,...}.
		const char* ProfileDir() {
			switch (bgfx::getRendererType()) {
			case bgfx::RendererType::Direct3D11: return "dx11";
			case bgfx::RendererType::Direct3D12: return "dx11"; // d3d12 reads the same DXBC blob
			case bgfx::RendererType::OpenGL:     return "glsl";
			case bgfx::RendererType::OpenGLES:   return "glsl";
			case bgfx::RendererType::Vulkan:     return "spirv";
			case bgfx::RendererType::Metal:      return "metal";
			case bgfx::RendererType::Noop:       return nullptr;
			default:                              return nullptr;
			}
		}

		// Strip directory + ext, return the bare shader name. The legacy
		// Shader.hpp ctor takes `(vsPath, fsPath)`; under bgfx the meaningful
		// part is the shared base name (e.g. "sprite" from
		// "AxiomAssets/Shaders/SpriteShader.vs").
		std::string ExtractName(const std::string& path) {
			std::filesystem::path p(path);
			std::string stem = p.stem().string();
			// Drop the SpriteShader / TextShader / etc. "Shader" suffix
			// where present so the on-disk name is just the pipeline.
			static const std::string suffix = "Shader";
			if (stem.size() > suffix.size()
				&& stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0)
			{
				stem.erase(stem.size() - suffix.size());
			}
			// Lowercase to match the on-disk convention (vs_sprite.bin not vs_Sprite.bin).
			for (char& c : stem) {
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			}
			return stem;
		}

		std::string ResolveBinPath(const std::string& stage, const std::string& name) {
			const char* profile = ProfileDir();
			if (!profile) return std::string();
			// Look in the project's runtime asset dir first (works at
			// runtime when the editor copies AxiomAssets next to the exe),
			// fall back to the source tree's AxiomAssets for editor use.
			const std::string rel = std::string("AxiomAssets/Shaders/bgfx/bin/")
				+ profile + "/" + stage + "_" + name + ".bin";
			std::string exeRel = Path::Combine(Path::ExecutableDir(), rel);
			if (std::filesystem::exists(exeRel)) return exeRel;
			return rel;
		}

		bgfx::ShaderHandle LoadStage(const std::string& path) {
			if (!std::filesystem::exists(path)) {
				AIM_CORE_WARN_TAG("Shader", "shader binary not found: {}", path);
				return BGFX_INVALID_HANDLE;
			}
			std::vector<uint8_t> bytes = File::ReadAllBytes(path);
			if (bytes.empty()) {
				AIM_CORE_WARN_TAG("Shader", "shader binary empty: {}", path);
				return BGFX_INVALID_HANDLE;
			}
			const bgfx::Memory* mem = bgfx::copy(bytes.data(),
				static_cast<uint32_t>(bytes.size()));
			bgfx::ShaderHandle h = bgfx::createShader(mem);
			if (!bgfx::isValid(h)) {
				AIM_CORE_ERROR_TAG("Shader", "bgfx::createShader failed: {}", path);
			}
			return h;
		}

		bgfx::ProgramHandle LoadProgram(const std::string& vsPath, const std::string& fsPath) {
			const std::string name = ExtractName(vsPath);
			const std::string vsBin = ResolveBinPath("vs", name);
			const std::string fsBin = ResolveBinPath("fs", name);

			bgfx::ShaderHandle vs = LoadStage(vsBin);
			if (!bgfx::isValid(vs)) return BGFX_INVALID_HANDLE;
			bgfx::ShaderHandle fs = LoadStage(fsBin);
			if (!bgfx::isValid(fs)) {
				bgfx::destroy(vs);
				return BGFX_INVALID_HANDLE;
			}
			// destroyShaders=true so destroy(program) takes the shader
			// handles with it — matches the OpenGL impl's lifetime.
			return bgfx::createProgram(vs, fs, /*destroyShaders=*/true);
		}
	}

	Shader::Shader(const std::string& vsPath, const std::string& fsPath) {
		bgfx::ProgramHandle h = LoadProgram(vsPath, fsPath);
		if (bgfx::isValid(h)) {
			m_Program = EncodeProgram(h.idx);
			m_IsValid = true;
		}
		(void)fsPath; // captured by LoadProgram via vsPath stem.
	}

	Shader::Shader(GLuint /*program*/) {}

	Shader::~Shader() {
		if (m_Program != 0) {
			bgfx::destroy(FromMProgram(m_Program));
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
			if (m_Program != 0) bgfx::destroy(FromMProgram(m_Program));
			m_Program = other.m_Program;
			m_IsValid = other.m_IsValid;
			other.m_Program = 0;
			other.m_IsValid = false;
		}
		return *this;
	}

	Shader Shader::FromBinary(const std::string& binaryPath) {
		// Treat `binaryPath` as the vsPath stem (caller convention is
		// "<name>.bin", we re-derive vs/fs from the stem).
		Shader s{ 0u };
		bgfx::ProgramHandle h = LoadProgram(binaryPath, binaryPath);
		if (bgfx::isValid(h)) {
			s.m_Program = EncodeProgram(h.idx);
			s.m_IsValid = true;
		}
		return s;
	}

	bool Shader::ExportBinary(const std::string& /*outputPath*/) const {
		// bgfx programs are immutable handles — there's no readback path.
		// Shader binaries are produced by the offline shaderc step in
		// AxiomAssets/Shaders/bgfx/compile.bat.
		return false;
	}

	Shader Shader::LoadWithBinaryCache(const std::string& binaryPath,
		const std::string& vsPath, const std::string& fsPath)
	{
		// Cache step is a no-op under bgfx — the .bin files ARE the cache.
		(void)binaryPath;
		return Shader{ vsPath, fsPath };
	}

	void Shader::Submit() const {
		// bgfx submits programs per-draw via bgfx::submit(view, program).
		// The actual submit happens in Renderer2D_Bgfx, not here.
	}

	GLuint Shader::LoadAndCompile(GLenum /*type*/, const std::string& /*path*/) {
		return 0;
	}

} // namespace Axiom
