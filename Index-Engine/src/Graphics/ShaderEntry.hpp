#pragma once

#include "Graphics/Shader.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Index {

    // Slot record for ShaderManager's slot table. Held by pointer so that
    // s_Shaders.push_back never moves a live Shader (which owns GPU state
    // and would have to walk move ctors on every vector growth). The cost
    // is one indirection on GetShader, which is once-per-bind.
    struct ShaderEntry {
        ShaderEntry() = default;
        ShaderEntry(ShaderEntry&&) noexcept = default;
        ShaderEntry& operator=(ShaderEntry&&) noexcept = default;
        ShaderEntry(const ShaderEntry&) = delete;
        ShaderEntry& operator=(const ShaderEntry&) = delete;

        std::unique_ptr<Shader> Shader;
        uint16_t Generation = 0;
        std::string VsPath;
        std::string FsPath;
        bool IsValid = false;
    };

} // namespace Index
