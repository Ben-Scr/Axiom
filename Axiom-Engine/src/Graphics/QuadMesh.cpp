#include "pch.hpp"
#include "Graphics/QuadMesh.hpp"

// QuadMesh stub — the per-shader vertex layout lives in the renderer
// pipeline now; this header-side helper collapses to no-ops.

namespace Axiom {

	void QuadMesh::Initialize() {}
	void QuadMesh::Bind() const {}
	void QuadMesh::Unbind() const {}
	void QuadMesh::Draw() const {}
	void QuadMesh::DrawInstanced(std::size_t /*instanceCount*/) const {}
	void QuadMesh::UploadInstances(std::span<const Instance44> /*instances*/) {}
	void QuadMesh::Shutdown() {}

} // namespace Axiom
