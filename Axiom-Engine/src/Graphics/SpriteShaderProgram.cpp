#include "pch.hpp"
#include "Graphics/SpriteShaderProgram.hpp"

// SpriteShaderProgram bgfx stub. See Shader_Bgfx.cpp's header comment for
// the Stage 2 rationale — all three header-side helpers (Shader,
// SpriteShaderProgram, QuadMesh) collapse to no-ops under the bgfx port
// until the real bgfx pipeline lands in a follow-up sub-stage.

namespace Axiom {

	void SpriteShaderProgram::Initialize() {}
	bool SpriteShaderProgram::IsValid() const { return false; }
	void SpriteShaderProgram::Bind() const {}
	void SpriteShaderProgram::Unbind() const {}
	void SpriteShaderProgram::Shutdown() {}

	void SpriteShaderProgram::SetMVP(const glm::mat4& /*mvp*/) const {}
	void SpriteShaderProgram::SetSpritePosition(const Vec2& /*position*/) const {}
	void SpriteShaderProgram::SetScale(const Vec2& /*scale*/) const {}
	void SpriteShaderProgram::SetRotation(float /*rotationRadians*/) const {}
	void SpriteShaderProgram::SetUV(const glm::vec2& /*offset*/, const glm::vec2& /*scale*/) const {}
	void SpriteShaderProgram::SetPremultipliedAlpha(bool /*enabled*/) const {}
	void SpriteShaderProgram::SetAlphaCutoff(float /*cutoff*/) const {}
	void SpriteShaderProgram::SetVertexColor(const Color& /*color*/) const {}
	void SpriteShaderProgram::ApplyDefaults() const {}

} // namespace Axiom
