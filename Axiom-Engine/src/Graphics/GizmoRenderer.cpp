#include "pch.hpp"
#include "Graphics/GizmoRenderer.hpp"

#include "Core/Log.hpp"
#include "Graphics/Shader.hpp"

#include <bgfx/bgfx.h>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace Axiom::BgfxBackend {
	bgfx::ViewId CurrentViewId();
}

// =============================================================================
// GizmoRenderer2D — bgfx implementation. Stage 2.4 of the bgfx port.
// -----------------------------------------------------------------------------
// Emits Gizmo::s_Squares / s_Circles / s_Lines as a single line-list submit
// through the gizmo program (vs_gizmo + fs_gizmo). Each Square fans into 4
// edges, Circles into N segments (caller-supplied), Lines stay as one. The
// CPU build pass writes into a transient vertex buffer; bgfx::submit with
// BGFX_STATE_PT_LINES + BGFX_STATE_BLEND_ALPHA renders the batch as a single
// line draw.
//
// Vertex layout matches PosColorVertex (12 + 4 = 16 bytes) — pos.xyz + rgba8
// — so we can reuse the m_GizmoVertices static member as our staging buffer
// without inventing a new scratch container.
//
// View routing: the caller (editor's RenderSceneIntoFBO) has already set
// `BgfxBackend::CurrentViewId()` to the active panel FBO's view, and we
// inherit the view's setViewTransform that was configured by Renderer2D
// earlier in the same frame. Each submit lands on whatever framebuffer
// that view targets, so editor gizmos compose on top of the world sprites
// the same way the OpenGL impl did.
// =============================================================================

namespace Axiom {

	// Static class members. m_VAO / m_VBO / m_EBO / m_VBOCapacityBytes /
	// m_EBOCapacityBytes / m_uMVP linger from the OpenGL header for ABI
	// compatibility (the header is shared) — they're unused here and
	// zero-initialised so debug-fill doesn't trigger false 'uninitialised
	// member' lint hits.
	bool                                  GizmoRenderer2D::m_IsInitialized = false;
	std::unique_ptr<Shader>               GizmoRenderer2D::m_GizmoShader;
	std::vector<PosColorVertex>           GizmoRenderer2D::m_GizmoVertices;
	std::vector<uint32_t>                 GizmoRenderer2D::m_GizmoIndices;
	std::vector<GizmoUploadVertex>        GizmoRenderer2D::s_UploadBuffer;
	uint16_t                              GizmoRenderer2D::m_GizmoViewId = 1;
	unsigned int                          GizmoRenderer2D::m_VAO = 0;
	unsigned int                          GizmoRenderer2D::m_VBO = 0;
	unsigned int                          GizmoRenderer2D::m_EBO = 0;
	std::size_t                           GizmoRenderer2D::m_VBOCapacityBytes = 0;
	std::size_t                           GizmoRenderer2D::m_EBOCapacityBytes = 0;
	int                                   GizmoRenderer2D::m_uMVP = -1;

	namespace {
		bgfx::ProgramHandle  g_Program     = BGFX_INVALID_HANDLE;
		bgfx::VertexLayout   g_Layout;
		bool                 g_LayoutBuilt = false;

		uint32_t PackRgba(const Color& c) {
			auto u8 = [](float v) {
				v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
				return static_cast<uint32_t>(v * 255.0f + 0.5f);
			};
			// bgfx's rgba8 vertex attribute reads bytes in the order
			// (r, g, b, a) into vec4. Little-endian uint32 packed as
			// `r | (g<<8) | (b<<16) | (a<<24)` produces that byte order.
			return u8(c.r)
				| (u8(c.g) << 8)
				| (u8(c.b) << 16)
				| (u8(c.a) << 24);
		}

		// Emit one line as two vertices into m_GizmoVertices. Both
		// endpoints share the same packed color (gizmo lines are flat —
		// no gradient).
		void EmitLine(std::vector<PosColorVertex>& out,
			float x0, float y0, float x1, float y1, uint32_t rgba)
		{
			out.push_back(PosColorVertex{ x0, y0, 0.0f, rgba });
			out.push_back(PosColorVertex{ x1, y1, 0.0f, rgba });
		}
	}

	bool GizmoRenderer2D::Initialize() {
		if (m_IsInitialized) {
			return true;
		}
		if (!g_LayoutBuilt) {
			g_Layout
				.begin()
				.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
				.add(bgfx::Attrib::Color0,   4, bgfx::AttribType::Uint8, /*normalized=*/true)
				.end();
			g_LayoutBuilt = true;
		}
		m_GizmoShader = std::make_unique<Shader>(
			std::string("AxiomAssets/Shaders/GizmoShader.vs"),
			std::string("AxiomAssets/Shaders/GizmoShader.fs"));
		if (!m_GizmoShader || !m_GizmoShader->IsValid()) {
			AIM_CORE_ERROR_TAG("GizmoRenderer",
				"Gizmo program failed to load (vs_gizmo/fs_gizmo.bin missing?) — gizmos disabled");
			m_GizmoShader.reset();
			return false;
		}
		const uint16_t raw = static_cast<uint16_t>(m_GizmoShader->GetHandle() - 1u);
		g_Program = bgfx::ProgramHandle{ raw };

		m_IsInitialized = true;
		return true;
	}

	void GizmoRenderer2D::Shutdown() {
		if (!m_IsInitialized) {
			return;
		}
		m_GizmoShader.reset();
		g_Program = BGFX_INVALID_HANDLE;
		m_GizmoVertices.clear();
		m_GizmoVertices.shrink_to_fit();
		m_GizmoIndices.clear();
		m_GizmoIndices.shrink_to_fit();
		s_UploadBuffer.clear();
		s_UploadBuffer.shrink_to_fit();
		m_IsInitialized = false;
	}

	void GizmoRenderer2D::OnResize(int /*w*/, int /*h*/) {}

	void GizmoRenderer2D::BeginFrame(uint16_t viewId) {
		m_GizmoViewId = viewId;
	}

	void GizmoRenderer2D::EndFrame() {}

	void GizmoRenderer2D::RenderWithVP(const glm::mat4& vp, GizmoLayerMask layerMask) {
		if (!m_IsInitialized || !bgfx::isValid(g_Program)) return;
		BuildGeometry(layerMask);
		FlushGizmosImpl(vp);
		Gizmo::Clear();
	}

	void GizmoRenderer2D::BuildGeometry(GizmoLayerMask layerMask) {
		// Translate Gizmo's accumulated primitives into a flat list of
		// line-list vertices. Capacity persists between frames (we only
		// clear the size).
		m_GizmoVertices.clear();

		// Squares — 4 edges around the rotated rect. HalfExtents is
		// already half-the-rect-size from Gizmo::DrawSquare.
		for (const Square& sq : Gizmo::s_Squares) {
			if (!HasAnyLayer(sq.Layer, layerMask)) continue;
			const uint32_t rgba = PackRgba(sq.Color);
			const float cx = sq.Center.x;
			const float cy = sq.Center.y;
			const float hx = sq.HalfExtents.x;
			const float hy = sq.HalfExtents.y;
			const float c  = std::cos(sq.Radiant);
			const float s  = std::sin(sq.Radiant);
			auto corner = [&](float lx, float ly) {
				return std::pair<float, float>{
					cx + lx * c - ly * s,
					cy + lx * s + ly * c
				};
			};
			const auto [x0, y0] = corner(-hx, -hy);
			const auto [x1, y1] = corner(+hx, -hy);
			const auto [x2, y2] = corner(+hx, +hy);
			const auto [x3, y3] = corner(-hx, +hy);
			EmitLine(m_GizmoVertices, x0, y0, x1, y1, rgba);
			EmitLine(m_GizmoVertices, x1, y1, x2, y2, rgba);
			EmitLine(m_GizmoVertices, x2, y2, x3, y3, rgba);
			EmitLine(m_GizmoVertices, x3, y3, x0, y0, rgba);
		}

		// Lines — pass-through, one segment each.
		for (const Line& ln : Gizmo::s_Lines) {
			if (!HasAnyLayer(ln.Layer, layerMask)) continue;
			EmitLine(m_GizmoVertices, ln.Start.x, ln.Start.y, ln.End.x, ln.End.y, PackRgba(ln.Color));
		}

		// Circles — N segments around the center, looped back to start.
		for (const Circle& ci : Gizmo::s_Circles) {
			if (!HasAnyLayer(ci.Layer, layerMask)) continue;
			if (ci.Segments < 3) continue;
			const uint32_t rgba = PackRgba(ci.Color);
			const float step = 6.28318530717958647692f / static_cast<float>(ci.Segments);
			float prevX = ci.Center.x + ci.Radius;
			float prevY = ci.Center.y;
			for (int i = 1; i <= ci.Segments; ++i) {
				const float a = static_cast<float>(i) * step;
				const float nx = ci.Center.x + std::cos(a) * ci.Radius;
				const float ny = ci.Center.y + std::sin(a) * ci.Radius;
				EmitLine(m_GizmoVertices, prevX, prevY, nx, ny, rgba);
				prevX = nx;
				prevY = ny;
			}
		}
	}

	void GizmoRenderer2D::FlushGizmosImpl(const glm::mat4& vp) {
		if (m_GizmoVertices.empty()) return;

		const uint32_t numVerts = static_cast<uint32_t>(m_GizmoVertices.size());
		if (bgfx::getAvailTransientVertexBuffer(numVerts, g_Layout) < numVerts) {
			AIM_CORE_WARN_TAG("GizmoRenderer",
				"Transient vertex buffer exhausted; dropped {} gizmo verts", numVerts);
			return;
		}
		bgfx::TransientVertexBuffer tvb{};
		bgfx::allocTransientVertexBuffer(&tvb, numVerts, g_Layout);
		std::memcpy(tvb.data, m_GizmoVertices.data(), numVerts * sizeof(PosColorVertex));

		// Submit through the currently-bound view (set by RenderSceneIntoFBO
		// before this call). Renderer2D already configured that view's
		// transform with the same vp, so we'd be re-setting it to the same
		// value — but do it explicitly anyway so callers that drive
		// gizmos without a prior Renderer2D pass also get the right
		// projection.
		const bgfx::ViewId vid = BgfxBackend::CurrentViewId();
		bgfx::setViewTransform(vid, nullptr, glm::value_ptr(vp));

		bgfx::setVertexBuffer(0, &tvb, 0, numVerts);
		bgfx::setState(0
			| BGFX_STATE_WRITE_RGB
			| BGFX_STATE_WRITE_A
			| BGFX_STATE_PT_LINES
			| BGFX_STATE_BLEND_ALPHA
			| BGFX_STATE_MSAA);
		bgfx::submit(vid, g_Program);
	}

} // namespace Axiom
