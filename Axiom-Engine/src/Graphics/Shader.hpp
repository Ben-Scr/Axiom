#pragma once
#include <string>
#include <cstdint>

// Shader was historically a thin wrapper around an OpenGL program handle
// (typed GLuint). With the bgfx-only cleanup on 2026-05 the GL backend
// was retired, but the public interface still exposes the handle as a
// 32-bit unsigned — Shader_Bgfx stores `(bgfx::ProgramHandle::idx + 1)`
// so consumers reading `GetHandle()` can keep treating 0 as "invalid"
// without learning bgfx types. The GLuint/GLenum aliases remain for
// header back-compat; they're plain integer typedefs now.
using GLuint = std::uint32_t;
using GLenum = std::uint32_t;

namespace Axiom {
    class Shader {
    public:
        Shader(const std::string& vsPath, const std::string& fsPath);
        ~Shader();

        static Shader FromBinary(const std::string& binaryPath);
        bool ExportBinary(const std::string& outputPath) const;
        static Shader LoadWithBinaryCache(
            const std::string& binaryPath,
            const std::string& vsPath,
            const std::string& fsPath);

        void Submit() const;

        GLuint GetHandle() const { return m_Program; }
        bool IsValid() const { return m_IsValid && m_Program != 0; }

        Shader(const Shader&) = delete;
        Shader& operator=(const Shader&) = delete;
        Shader(Shader&&) noexcept;
        Shader& operator=(Shader&&) noexcept;

    private:
        explicit Shader(GLuint program);
        static GLuint LoadAndCompile(GLenum type, const std::string& path);
        GLuint m_Program{ 0 };
        bool m_IsValid{ false };
    };
}