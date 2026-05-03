#pragma once
#include "Graphics/PosColorVertex.hpp"
#include "Graphics/Gizmo.hpp"
#include "Core/Export.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <vector>
#include <cstdint>

namespace Axiom {
    class Camera2DComponent;
    class Shader;

    class AXIOM_API GizmoRenderer2D {
    public:
        static bool Initialize();
        static void Shutdown();
        static void OnResize(int w, int h);
        static void BeginFrame(uint16_t viewId = 1);
        static void Render(GizmoLayerMask layerMask = GizmoLayerMask::Shared);
        static void EndFrame();

        static void RenderWithVP(const glm::mat4& vp, GizmoLayerMask layerMask = GizmoLayerMask::All);

    private:
        static void BuildGeometry(GizmoLayerMask layerMask);
        // E18: single private flush helper takes an optional VP. nullopt means
        // "use Camera2DComponent::Main()'s VP" (legacy FlushGizmos behavior);
        // a value means "use the supplied VP" (legacy FlushGizmosWithVP).
        static void FlushGizmosImpl(const glm::mat4* vpOverride);

        static bool m_IsInitialized;
        static std::unique_ptr<Shader> m_GizmoShader;
        static std::vector<PosColorVertex> m_GizmoVertices;
        static std::vector<uint16_t> m_GizmoIndices;

        static uint16_t m_GizmoViewId;

        static unsigned int m_VAO;
        static unsigned int m_VBO;
        static unsigned int m_EBO;
        // E18: track persistent VBO/EBO capacity (in bytes) so we can grow
        // the buffer once and use glBufferSubData each frame.
        static std::size_t m_VBOCapacityBytes;
        static std::size_t m_EBOCapacityBytes;
        static int m_uMVP;
    };
}
