#include "pch.hpp"
#include "Graphics/SpriteShaderProgram.hpp"

// SpriteShaderProgram stub — sprite parameter binding lives on the
// renderer's uniform / instance buffer now; this header-side helper
// collapses to no-ops.

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
