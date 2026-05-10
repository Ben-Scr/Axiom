#include "pch.hpp"
#include "Camera2DComponent.hpp"
#include "Collections/Viewport.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace Axiom {
	Camera2DComponent* Camera2DComponent::Main() {
		Application* app = Application::GetInstance();
		if (!app || !app->GetSceneManager()) {
			return nullptr;
		}

		Scene* activeScene = app->GetSceneManager()->GetActiveScene();
		return activeScene ? activeScene->GetMainCamera() : nullptr;
	}

	Transform2DComponent* Camera2DComponent::TryGetTransform() {
		if (!m_OwnerScene || m_OwnerEntity == entt::null) return nullptr;
		if (!m_OwnerScene->IsValid(m_OwnerEntity)) return nullptr;
		if (!m_OwnerScene->HasComponent<Transform2DComponent>(m_OwnerEntity)) return nullptr;
		return &m_OwnerScene->GetComponent<Transform2DComponent>(m_OwnerEntity);
	}

	const Transform2DComponent* Camera2DComponent::TryGetTransform() const {
		if (!m_OwnerScene || m_OwnerEntity == entt::null) return nullptr;
		if (!m_OwnerScene->IsValid(m_OwnerEntity)) return nullptr;
		if (!m_OwnerScene->HasComponent<Transform2DComponent>(m_OwnerEntity)) return nullptr;
		return &m_OwnerScene->GetComponent<Transform2DComponent>(m_OwnerEntity);
	}

	void Camera2DComponent::UpdateViewport() {
		UpdateProj();
		UpdateView();
	}

	namespace {
		// Resolve the parent of the camera entity (if any) into its world
		// Transform2DComponent. Returns nullptr when the entity is a root,
		// has no HierarchyComponent, or the parent is invalid / lacks a
		// transform — in any of those cases SetPosition/SetRotation can
		// write the world value through to LocalPosition/LocalRotation
		// directly because no parent transform composes on top of it.
		const Transform2DComponent* TryGetParentWorldTransform(Scene* scene, EntityHandle entity) {
			if (!scene || entity == entt::null || !scene->IsValid(entity)) return nullptr;
			if (!scene->HasComponent<HierarchyComponent>(entity)) return nullptr;
			const auto& hierarchy = scene->GetComponent<HierarchyComponent>(entity);
			if (hierarchy.Parent == entt::null || !scene->IsValid(hierarchy.Parent)) return nullptr;
			if (!scene->HasComponent<Transform2DComponent>(hierarchy.Parent)) return nullptr;
			return &scene->GetComponent<Transform2DComponent>(hierarchy.Parent);
		}
	}

	void Camera2DComponent::SetPosition(Vec2 p) {
		if (auto* transform = TryGetTransform()) {
			// `p` is a world-space position. If the camera entity is a child,
			// LocalPosition has to be `p` expressed in the parent's local
			// frame — otherwise TransformHierarchySystem will compose
			// (parentWorld * p) next propagation pass and place the camera
			// at twice the intended offset. For root entities Local* and
			// world coincide so the inverse is the identity.
			Vec2 localP = p;
			if (const auto* parentTr = TryGetParentWorldTransform(m_OwnerScene, m_OwnerEntity)) {
				// Inverse of TransformPoint: T^-1 (parent.Position), R^-1
				// (-parent.Rotation), S^-1 (1/parent.Scale). Zero-scale is
				// degenerate; fall through to the rotated value to avoid div-by-zero.
				const Vec2 delta{ p.x - parentTr->Position.x, p.y - parentTr->Position.y };
				const Vec2 unrotated = Rotate(delta, -parentTr->Rotation);
				const float invSx = parentTr->Scale.x != 0.0f ? 1.0f / parentTr->Scale.x : 1.0f;
				const float invSy = parentTr->Scale.y != 0.0f ? 1.0f / parentTr->Scale.y : 1.0f;
				localP = Vec2{ unrotated.x * invSx, unrotated.y * invSy };
			}
			// Route through Transform2DComponent::SetPosition so we update
			// the authored Local* value. Writing transform->Position directly
			// would be silently overwritten by the next TransformHierarchySystem
			// propagation pass (which derives Position from LocalPosition) and
			// the camera would snap back.
			transform->SetPosition(localP);
			// Cache the world value too so UpdateView (and any same-frame
			// reader) sees the new placement before propagation runs.
			transform->Position = p;
			UpdateView();
		}
	}

	void Camera2DComponent::AddPosition(Vec2 p) {
		if (auto* transform = TryGetTransform()) {
			SetPosition(p + transform->Position);
		}
	}

	Vec2 Camera2DComponent::GetPosition() const {
		if (const auto* transform = TryGetTransform()) {
			return transform->Position;
		}
		return Vec2{ 0.0f, 0.0f };
	}

	void Camera2DComponent::SetRotation(float rad) {
		if (auto* transform = TryGetTransform()) {
			// Same logic as SetPosition: `rad` is a world rotation. If we
			// have a parent, LocalRotation must be the offset from the
			// parent's world rotation so propagation reproduces `rad`.
			float localRad = rad;
			if (const auto* parentTr = TryGetParentWorldTransform(m_OwnerScene, m_OwnerEntity)) {
				localRad = rad - parentTr->Rotation;
			}
			transform->SetRotation(localRad);
			transform->Rotation = rad;
			UpdateView();
		}
	}

	float Camera2DComponent::GetRotation() const {
		if (const auto* transform = TryGetTransform()) {
			return transform->Rotation;
		}
		return 0.0f;
	}

	glm::mat4 Camera2DComponent::GetViewProjectionMatrix() const {
		return m_ProjMat * m_ViewMat;
	}

	void Camera2DComponent::UpdateProj() {
		if (!m_Viewport || m_Viewport->GetWidth() == 0 || m_Viewport->GetHeight() == 0) return;
		const float aspect = m_Viewport->GetAspect();
		const float halfH = m_OrthographicSize * m_Zoom;
		const float halfW = halfH * aspect;

		// 2D rendering submits at world z=0. An OpenGL-convention range
		// like (0, 100) maps z=0 to NDC z=-1, which Vulkan/D3D12 clip-space
		// (valid range [0, 1]) culls. A symmetric (-1, 1) range maps z=0
		// to NDC z=0 — visible under every backend bgfx supports without
		// having to branch on caps->homogeneousDepth in the renderer.
		const float zNear = -1.0f;
		const float zFar  =  1.0f;

		m_ProjMat = glm::ortho(-halfW, +halfW, -halfH, +halfH, zNear, zFar);
		if (TryGetTransform()) {
			UpdateViewportAABB();
		}
	}

	void Camera2DComponent::UpdateView() {
		// Closed-form view = Rz(-theta) * T(-p) — replaces per-frame glm::inverse(M).
		const Transform2DComponent* transform = TryGetTransform();
		if (!transform) return;

		const float rotZ = transform->Rotation;
		const float c = std::cos(rotZ);
		const float s = std::sin(rotZ);
		const float px = transform->Position.x;
		const float py = transform->Position.y;

		// Column-major: rotation block is Rz(-theta), translation column is Rz(-theta) * (-p).
		glm::mat4 view(1.0f);
		view[0][0] =  c; view[0][1] = -s; view[0][2] = 0.0f; view[0][3] = 0.0f;
		view[1][0] =  s; view[1][1] =  c; view[1][2] = 0.0f; view[1][3] = 0.0f;
		view[2][0] = 0.0f; view[2][1] = 0.0f; view[2][2] = 1.0f; view[2][3] = 0.0f;
		view[3][0] = -c * px - s * py;
		view[3][1] =  s * px - c * py;
		view[3][2] = 0.0f;
		view[3][3] = 1.0f;
		m_ViewMat = view;

		UpdateViewportAABB();
	}

	void Camera2DComponent::UpdateViewportAABB() {
		if (!m_Viewport) return;

		const Transform2DComponent* transform = TryGetTransform();
		if (!transform) return;
		Vec2 worldViewport = WorldViewPort();
		m_WorldViewportAABB = AABB::Create(transform->Position, worldViewport / 2.f);
	}

	Vec2 Camera2DComponent::WorldViewPort() const {
		if (!m_Viewport) return { 0.0f, 0.0f };

		float aspect = m_Viewport->GetAspect();
		float worldHeight = 2.0f * (m_OrthographicSize * m_Zoom);
		float worldWidth = worldHeight * aspect;
		return { worldWidth, worldHeight };
	}

	Vec2 Camera2DComponent::ScreenToWorld(Vec2 pos) const
	{
		if (!m_Viewport) return { 0.0f, 0.0f };

		// Editor: when the Game View panel is rendering into a sub-rect
		// of the OS window, mouse coords come in OS-window-relative,
		// but the camera renders to the panel's pixel rect. Without
		// remapping here, ScreenToWorld would divide an OS-window
		// coordinate by the OS-window dimensions and miss the panel's
		// offset + size — handing back a world point that drifted by
		// roughly the panel's top-left offset (in world units) and
		// stretched by the OS-window/panel size ratio. Mirrors the
		// UIRegion handling UIEventSystem does for hit-testing.
		//
		// Runtime: UIRegion stays unset (no editor panel publishing it),
		// so we fall back to the OS-window viewport — which IS the
		// camera's render target in shipped builds. Same math as
		// before, no behavioural change.
		const Window::UIRegion uiRegion = Window::GetUIRegion();
		float vpW = 0.0f;
		float vpH = 0.0f;
		Vec2 local = pos;
		if (uiRegion.IsActive()) {
			local.x -= static_cast<float>(uiRegion.OffsetX);
			local.y -= static_cast<float>(uiRegion.OffsetY);
			vpW = static_cast<float>(uiRegion.Width);
			vpH = static_cast<float>(uiRegion.Height);
		}
		else {
			vpW = static_cast<float>(m_Viewport->GetWidth());
			vpH = static_cast<float>(m_Viewport->GetHeight());
		}
		if (vpW <= 0.0f || vpH <= 0.0f) return { 0.0f, 0.0f };

		const float xNdc = (2.0f * local.x / vpW) - 1.0f;
		const float yNdc = 1.0f - (2.0f * local.y / vpH);

		const float zNear = 0.0f, zFar = 100.0f;
		const float zNdc = -(zFar + zNear) / (zFar - zNear);

		const glm::vec4 clip(xNdc, yNdc, zNdc, 1.0f);

		const glm::mat4 vp = m_ProjMat * m_ViewMat;
		const glm::mat4 invVp = glm::inverse(vp);

		glm::vec4 world = invVp * clip;
		if (world.w != 0.0f) world /= world.w;

		return { world.x, world.y };
	}

	void Camera2DComponent::Initialize(Scene& scene, EntityHandle entity) {
		m_Viewport = Window::GetMainViewport();
		m_OwnerScene = &scene;
		m_OwnerEntity = entity;
		m_ViewMat = glm::mat4(1.0f);
		m_ProjMat = glm::mat4(1.0f);
		UpdateProj();
		UpdateView();
	}

	void Camera2DComponent::Destroy() {
		m_OwnerScene = nullptr;
		m_OwnerEntity = entt::null;
		m_Viewport = nullptr;
	}
}
