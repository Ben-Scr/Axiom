#include "pch.hpp"
#include "Camera2DComponent.hpp"
#include "Collections/Viewport.hpp"
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

	void Camera2DComponent::UpdateViewport() {
		UpdateProj();
		UpdateView();
	}

	glm::mat4 Camera2DComponent::GetViewProjectionMatrix() const {
		return m_ProjMat * m_ViewMat;
	}

	void Camera2DComponent::UpdateProj() {
		if (m_Viewport->GetWidth() == 0 || m_Viewport->GetHeight() == 0) return;
		const float aspect = m_Viewport->GetAspect();
		const float halfH = m_OrthographicSize * m_Zoom;
		const float halfW = halfH * aspect;

		const float zNear = 0.0f;
		const float zFar = 100.0f;

		m_ProjMat = glm::ortho(-halfW, +halfW, -halfH, +halfH, zNear, zFar);
		if (m_Transform) {
			UpdateViewportAABB();
		}
	}

	void Camera2DComponent::UpdateView() {
		// E19: closed-form 2D inverse view. The camera model is
		//   M = T(px,py) * Rz(theta)
		// Its inverse is
		//   M^-1 = Rz(-theta) * T(-px,-py)
		// which expanded into a 4x4 column-major glm::mat4 is the matrix below.
		// Replaces the previous glm::inverse(M) call (per-frame general 4x4
		// inverse via cofactor expansion).
		const float rotZ = m_Transform->Rotation;
		const float c = std::cos(rotZ);
		const float s = std::sin(rotZ);
		const float px = m_Transform->Position.x;
		const float py = m_Transform->Position.y;

		// Column-major. Rotation block is Rz(-theta) (transpose of Rz);
		// translation column is Rz(-theta) * (-p).
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
		Vec2 worldViewport = WorldViewPort();
		m_WorldViewportAABB = AABB::Create(m_Transform->Position, worldViewport / 2.f);
	}

	Vec2 Camera2DComponent::WorldViewPort() const {
		float aspect = m_Viewport->GetAspect();
		float worldHeight = 2.0f * (m_OrthographicSize * m_Zoom);
		float worldWidth = worldHeight * aspect;
		return { worldWidth, worldHeight };
	}

	Vec2 Camera2DComponent::ScreenToWorld(Vec2 pos) const
	{
		if (m_Viewport->GetWidth() == 0 || m_Viewport->GetHeight() == 0) return { 0.0f, 0.0f };
		const float xNdc = (2.0f * pos.x / float(m_Viewport->GetWidth())) - 1.0f;
		const float yNdc = 1.0f - (2.0f * pos.y / float(m_Viewport->GetHeight()));

		const float zNear = 0.0f, zFar = 100.0f;
		const float zNdc = -(zFar + zNear) / (zFar - zNear);

		const glm::vec4 clip(xNdc, yNdc, zNdc, 1.0f);

		const glm::mat4 vp = m_ProjMat * m_ViewMat;
		const glm::mat4 invVp = glm::inverse(vp);

		glm::vec4 world = invVp * clip;
		if (world.w != 0.0f) world /= world.w;

		return { world.x, world.y };
	}

	void Camera2DComponent::Initialize(Transform2DComponent& transform) {
		m_Viewport = Window::GetMainViewport();
		m_Transform = &transform;
		m_ViewMat = glm::mat4(1.0f);
		m_ProjMat = glm::mat4(1.0f);
		UpdateProj();
		UpdateView();
	}

	void Camera2DComponent::Destroy() {
		m_Transform = nullptr;
		m_Viewport = nullptr;
	}
}
