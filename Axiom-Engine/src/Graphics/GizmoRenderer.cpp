#include "pch.hpp"
#include "GizmoRenderer.hpp"
#include "Shader.hpp"
#include "Gizmo.hpp"
#include "Serialization/Path.hpp"

#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

namespace Axiom {
	namespace {
		constexpr GLsizei GizmoVertexStride = static_cast<GLsizei>(sizeof(GizmoUploadVertex));

		static void ConvertVertices(const std::vector<PosColorVertex>& src, std::vector<GizmoUploadVertex>& dst) {
			dst.clear();
			dst.reserve(src.size());
			for (const auto& v : src) {
				Color c = Color::FromABGR32(v.color);
				dst.push_back({ v.x, v.y, v.z, c.r, c.g, c.b, c.a });
			}
		}

		static Vec2 RotateGizmoPoint(const Vec2& v, float radians) {
			// Standard CCW rotation matrix: [c,-s; s, c]. The previous
			// formulation `(v.x*c + v.y*s, -v.x*s + v.y*c)` was the
			// transpose / CW rotation, so the editor's selection box,
			// camera frustum, and collider outlines all rotated in the
			// OPPOSITE direction from the entity itself — most visible
			// when dragging the rotation handle and watching the gizmo
			// counter-rotate against the sprite.
			const float c = Cos(radians);
			const float s = Sin(radians);
			return Vec2(v.x * c - v.y * s, v.x * s + v.y * c);
		}
	}

	bool GizmoRenderer2D::m_IsInitialized = false;
	std::unique_ptr<Shader> GizmoRenderer2D::m_GizmoShader;
	std::vector<PosColorVertex> GizmoRenderer2D::m_GizmoVertices;
	std::vector<uint32_t> GizmoRenderer2D::m_GizmoIndices;
	std::vector<GizmoUploadVertex> GizmoRenderer2D::s_UploadBuffer;
	uint16_t GizmoRenderer2D::m_GizmoViewId = 1;
	unsigned int GizmoRenderer2D::m_VAO = 0;
	unsigned int GizmoRenderer2D::m_VBO = 0;
	unsigned int GizmoRenderer2D::m_EBO = 0;
	// Grow-only via glBufferData; per-frame uploads use glBufferSubData (no realloc on steady state).
	std::size_t GizmoRenderer2D::m_VBOCapacityBytes = 0;
	std::size_t GizmoRenderer2D::m_EBOCapacityBytes = 0;
	int GizmoRenderer2D::m_uMVP = -1;

	namespace {
		// Power-of-two grow helper; mirrors QuadMesh::NextInstanceCapacity.
		std::size_t NextBufferCapacityBytes(std::size_t requiredBytes, std::size_t initialBytes) {
			std::size_t capacity = initialBytes;
			while (capacity < requiredBytes) {
				capacity *= 2;
			}
			return capacity;
		}
	}

	bool GizmoRenderer2D::Initialize() {
		if (m_IsInitialized)
			return true;

		std::string shaderDir = Path::ResolveAxiomAssets("Shader");
		if (shaderDir.empty()) {
			AIM_CORE_ERROR("AxiomAssets/Shader not found");
			shaderDir = Path::Combine(Path::ExecutableDir(), "AxiomAssets", "Shader");
		}
		m_GizmoShader = std::make_unique<Shader>(Path::Combine(shaderDir, "gizmo.vert.glsl"), Path::Combine(shaderDir, "gizmo.frag.glsl"));
		AIM_ASSERT(m_GizmoShader && m_GizmoShader->IsValid(), AxiomErrorCode::Undefined, "Failed to load gizmo shader");

		GLuint program = m_GizmoShader->GetHandle();
		m_uMVP = glGetUniformLocation(program, "uMVP");

		glGenVertexArrays(1, &m_VAO);
		glGenBuffers(1, &m_VBO);
		glGenBuffers(1, &m_EBO);

		glBindVertexArray(m_VAO);
		glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
		// Initial capacity sized so first frame's glBufferSubData runs without a realloc.
		m_VBOCapacityBytes = 4096 * sizeof(GizmoUploadVertex);
		glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_VBOCapacityBytes), nullptr, GL_DYNAMIC_DRAW);

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, GizmoVertexStride, reinterpret_cast<void*>(0));

		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, GizmoVertexStride, reinterpret_cast<void*>(sizeof(float) * 3));

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
		m_EBOCapacityBytes = 8192 * sizeof(uint32_t);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_EBOCapacityBytes), nullptr, GL_DYNAMIC_DRAW);

		glBindVertexArray(0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

		m_GizmoVertices.reserve(4096);
		m_GizmoIndices.reserve(8192);

		m_IsInitialized = true;
		return true;
	}

	void GizmoRenderer2D::Shutdown() {
		if (!m_IsInitialized)
			return;

		if (m_EBO) {
			glDeleteBuffers(1, &m_EBO);
			m_EBO = 0;
		}
		if (m_VBO) {
			glDeleteBuffers(1, &m_VBO);
			m_VBO = 0;
		}
		if (m_VAO) {
			glDeleteVertexArrays(1, &m_VAO);
			m_VAO = 0;
		}
		// Reset so a future Initialize re-seeds correctly.
		m_VBOCapacityBytes = 0;
		m_EBOCapacityBytes = 0;

		m_GizmoShader.reset();
		m_GizmoVertices.clear();
		m_GizmoIndices.clear();
		s_UploadBuffer.clear();

		m_IsInitialized = false;
	}

	void GizmoRenderer2D::OnResize(int /*w*/, int /*h*/) {

	}

	void GizmoRenderer2D::BeginFrame(uint16_t viewId) {
		if (!m_IsInitialized)
			return;

		m_GizmoViewId = viewId;
		m_GizmoVertices.clear();
		m_GizmoIndices.clear();
	}

	void GizmoRenderer2D::BuildGeometry(GizmoLayerMask layerMask) {
		m_GizmoVertices.clear();
		m_GizmoIndices.clear();

		for (const auto& square : Gizmo::s_Squares) {
			if (!HasAnyLayer(square.Layer, layerMask)) {
				continue;
			}

			uint32_t color = square.Color.ABGR32();
			uint32_t baseIndex = static_cast<uint32_t>(m_GizmoVertices.size());

			Vec2 corners[4] = {
					Vec2(-square.HalfExtents.x, -square.HalfExtents.y),
					Vec2(square.HalfExtents.x, -square.HalfExtents.y),
					Vec2(square.HalfExtents.x,  square.HalfExtents.y),
					Vec2(-square.HalfExtents.x,  square.HalfExtents.y)
			};

			for (int i = 0; i < 4; ++i) {
				Vec2 rotated = RotateGizmoPoint(corners[i], square.Radiant);
				Vec2 final = square.Center + rotated;
				m_GizmoVertices.push_back({ final.x, final.y, 0.0f, color });
			}

			m_GizmoIndices.push_back(baseIndex + 0);
			m_GizmoIndices.push_back(baseIndex + 1);
			m_GizmoIndices.push_back(baseIndex + 1);
			m_GizmoIndices.push_back(baseIndex + 2);
			m_GizmoIndices.push_back(baseIndex + 2);
			m_GizmoIndices.push_back(baseIndex + 3);
			m_GizmoIndices.push_back(baseIndex + 3);
			m_GizmoIndices.push_back(baseIndex + 0);
		}

		for (const auto& line : Gizmo::s_Lines) {
			if (!HasAnyLayer(line.Layer, layerMask)) {
				continue;
			}

			uint32_t color = line.Color.ABGR32();
			uint32_t baseIndex = static_cast<uint32_t>(m_GizmoVertices.size());

			m_GizmoVertices.push_back({ line.Start.x, line.Start.y, 0.0f, color });
			m_GizmoVertices.push_back({ line.End.x, line.End.y, 0.0f, color });

			m_GizmoIndices.push_back(baseIndex);
			m_GizmoIndices.push_back(baseIndex + 1);
		}

		for (const auto& circle : Gizmo::s_Circles) {
			if (!HasAnyLayer(circle.Layer, layerMask) || circle.Segments <= 0) {
				continue;
			}

			uint32_t color = circle.Color.ABGR32();
			uint32_t baseIndex = static_cast<uint32_t>(m_GizmoVertices.size());

			float angleStep = TwoPi<float>() / static_cast<float>(circle.Segments);
			for (int i = 0; i < circle.Segments; ++i) {
				float angle = i * angleStep;
				float x = circle.Center.x + circle.Radius * Cos(angle);
				float y = circle.Center.y + circle.Radius * Sin(angle);
				m_GizmoVertices.push_back({ x, y, 0.0f, color });
			}

			for (int i = 0; i < circle.Segments; ++i) {
				m_GizmoIndices.push_back(baseIndex + static_cast<uint32_t>(i));
				m_GizmoIndices.push_back(baseIndex + static_cast<uint32_t>((i + 1) % circle.Segments));
			}
		}
	}

	void GizmoRenderer2D::RenderWithVP(const glm::mat4& vp, GizmoLayerMask layerMask) {
		if (!m_IsInitialized || !Gizmo::s_IsEnabled)
			return;

		BuildGeometry(layerMask);
		FlushGizmosImpl(vp);
	}

	// glBufferSubData against grow-only persistent buffer; previously orphan-realloced every frame.
	void GizmoRenderer2D::FlushGizmosImpl(const glm::mat4& vp) {
		if (!m_IsInitialized || m_GizmoVertices.empty() || !m_GizmoShader || !m_GizmoShader->IsValid())
			return;

		// Save GL state we mutate so the caller's pipeline is preserved
		// across the gizmo draw. Mirrors TextRenderer::RenderInstances'
		// save/restore discipline — without these, the next pass inherits
		// whatever blend / depth / scissor / line-width we last set.
		GLfloat savedLineWidth = 1.0f;
		glGetFloatv(GL_LINE_WIDTH, &savedLineWidth);

		const GLboolean savedBlend = glIsEnabled(GL_BLEND);
		GLint savedBlendSrcRgb = GL_ONE;
		GLint savedBlendDstRgb = GL_ZERO;
		GLint savedBlendSrcAlpha = GL_ONE;
		GLint savedBlendDstAlpha = GL_ZERO;
		GLint savedBlendEquationRgb = GL_FUNC_ADD;
		GLint savedBlendEquationAlpha = GL_FUNC_ADD;
		glGetIntegerv(GL_BLEND_SRC_RGB, &savedBlendSrcRgb);
		glGetIntegerv(GL_BLEND_DST_RGB, &savedBlendDstRgb);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &savedBlendSrcAlpha);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &savedBlendDstAlpha);
		glGetIntegerv(GL_BLEND_EQUATION_RGB, &savedBlendEquationRgb);
		glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &savedBlendEquationAlpha);

		const GLboolean savedDepth = glIsEnabled(GL_DEPTH_TEST);
		const GLboolean savedScissor = glIsEnabled(GL_SCISSOR_TEST);

		ConvertVertices(m_GizmoVertices, s_UploadBuffer);

		const std::size_t vboBytes = s_UploadBuffer.size() * sizeof(GizmoUploadVertex);
		const std::size_t eboBytes = m_GizmoIndices.size() * sizeof(uint32_t);

		glBindVertexArray(m_VAO);

		glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
		if (vboBytes > m_VBOCapacityBytes) {
			m_VBOCapacityBytes = NextBufferCapacityBytes(vboBytes, m_VBOCapacityBytes ? m_VBOCapacityBytes : 4096 * sizeof(GizmoUploadVertex));
			glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_VBOCapacityBytes), nullptr, GL_DYNAMIC_DRAW);
		}
		glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vboBytes), s_UploadBuffer.data());

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
		if (eboBytes > m_EBOCapacityBytes) {
			m_EBOCapacityBytes = NextBufferCapacityBytes(eboBytes, m_EBOCapacityBytes ? m_EBOCapacityBytes : 8192 * sizeof(uint32_t));
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_EBOCapacityBytes), nullptr, GL_DYNAMIC_DRAW);
		}
		glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(eboBytes), m_GizmoIndices.data());

		m_GizmoShader->Submit();

		if (m_uMVP >= 0) {
			glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, glm::value_ptr(vp));
		}

		glLineWidth(Gizmo::s_LineWidth);
		glDrawElements(GL_LINES, static_cast<GLsizei>(m_GizmoIndices.size()), GL_UNSIGNED_INT, nullptr);

		glBindVertexArray(0);
		glUseProgram(0);

		// Restore everything we touched. Order matches TextRenderer's
		// pattern — equation before func, then enable/disable bits.
		glLineWidth(savedLineWidth);
		glBlendEquationSeparate(savedBlendEquationRgb, savedBlendEquationAlpha);
		glBlendFuncSeparate(savedBlendSrcRgb, savedBlendDstRgb, savedBlendSrcAlpha, savedBlendDstAlpha);
		if (savedBlend)   glEnable(GL_BLEND);   else glDisable(GL_BLEND);
		if (savedDepth)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
		if (savedScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	}

	void GizmoRenderer2D::EndFrame() {
		// Explicit-pass invariant: GizmoRenderer2D::EndFrame is cleanup only.
		// Drawing is done by an explicit caller via Render() / RenderWithVP() —
		// per Axiom's render-graph design, every draw must be declared in a named
		// pass, not slipped in from a lifecycle hook. The runtime "always-show
		// shared gizmos" behavior moves to Application::RenderPipelineOnly which
		// owns the pass list. Apps that want runtime gizmos call Render() there;
		// EndFrame just clears the per-frame buffers.
		Gizmo::Clear();
	}
}
