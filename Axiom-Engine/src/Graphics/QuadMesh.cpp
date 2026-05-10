#include "pch.hpp"
#include "Graphics/QuadMesh.hpp"

// QuadMesh bgfx stub. See Shader_Bgfx.cpp for the Stage 2 rationale.

namespace Axiom {

	void QuadMesh::Initialize() {}
	void QuadMesh::Bind() const {}
	void QuadMesh::Unbind() const {}
	void QuadMesh::Draw() const {}
	void QuadMesh::DrawInstanced(std::size_t /*instanceCount*/) const {}
	void QuadMesh::UploadInstances(std::span<const Instance44> /*instances*/) {}
	void QuadMesh::Shutdown() {}

} // namespace Axiom
