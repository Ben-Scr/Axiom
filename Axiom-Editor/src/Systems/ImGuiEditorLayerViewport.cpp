#include <pch.hpp>
#include "Systems/ImGuiEditorLayer.hpp"

#include <imgui.h>

#include "Components/Components.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Diagnostics/StatsOverlay.hpp"
#include "Editor/ApplicationEditorAccess.hpp"
#include "Graphics/Framebuffer.hpp"
#include "Graphics/GizmoRenderer.hpp"
#include "Graphics/Gizmo.hpp"
#include "Graphics/RenderApi.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/GuiRenderer.hpp"
#include "Gui/EditorIcons.hpp"
#include "Math/Trigonometry.hpp"
#include "Math/VectorMath.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/ComponentInfo.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Systems/TransformHierarchySystem.hpp"
#include "Systems/UILayoutSystem.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace Axiom {
	namespace {
		struct GameViewAspectPreset {
			const char* Label;
			float Aspect = 0.0f;
		};

		constexpr std::array<GameViewAspectPreset, 6> k_GameViewAspectPresets = {{
			{ "Free Aspect", 0.0f },
			{ "16:9", 16.0f / 9.0f },
			{ "21:9", 21.0f / 9.0f },
			{ "4:3", 4.0f / 3.0f },
			{ "3:2", 3.0f / 2.0f },
			{ "9:16", 9.0f / 16.0f }
		}};
	}

	void ImGuiEditorLayer::RenderSceneIntoFBO(Framebuffer& fbo, Scene& scene,
		const glm::mat4& vp, const AABB& viewportAABB,
		bool withGizmos, bool sharedGizmosOnly, const Color& clearColor,
		bool onlyPassedScene, bool uiInWorldSpace,
		EditorViewDrawMode drawMode)
	{
		auto* app = Application::GetInstance();
		if (!app) return;
		auto* renderer = app->GetRenderer2D();
		if (!renderer) return;

		const int w = fbo.GetWidth();
		const int h = fbo.GetHeight();

		// All immediate-mode state goes through RenderApi — no glXxx calls
		// in editor code. Backend translation lives in
		// `Graphics/Backend/WebGPUApi.cpp`.
		RenderApi::BindFramebuffer(fbo);
		RenderApi::SetViewport(0, 0, w, h);
		RenderApi::SetClearColor(clearColor);
		RenderApi::Clear(ClearFlags::Color | ClearFlags::Depth);

		// Sprites + UI + (optional) gizmos extracted into a callable so
		// the Mixed draw mode can run the scene twice — once filled and
		// once in wireframe — into the same FBO without duplicating the
		// surrounding setup. Triangle mode flips polygon mode to GL_LINE
		// for a single pass; Default keeps the historical behaviour.
		auto runSceneRender = [&]() {
			if (onlyPassedScene) {
				renderer->RenderSceneWithVP(scene, vp, viewportAABB);
			}
			else {
				SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
					renderer->RenderSceneWithVP(s, vp, viewportAABB);
					});
			}

			// UI must paint into this panel's FBO (not the OS window
			// backbuffer that ImGui will cover). The auto BeginFrame path
			// is skipped in editor mode, so we drive RenderScene here while
			// the FBO is still bound.
			//
			// World-space mode (Editor View) projects through the same `vp`
			// the world used so UI rects translate/scale with the camera —
			// matching how the selection gizmo for a RectTransform2D draws.
			// Screen-space mode (Game View, runtime) keeps UI locked to the
			// canvas regardless of camera, the way runtime UI behaves.
			//
			// UI submits BEFORE gizmos so the gizmo pass paints on top —
			// selection outlines, manipulators, and debug overlays must be
			// visible even when the user is editing UI that fills the viewport.
			if (auto* gui = app->GetGuiRenderer()) {
				// Resolve the canonical pixel-to-world scale once per pass and
				// thread it through to RenderScene so the renderer doesn't reach
				// into Application / main-camera state itself.
				const float pixelToWorldScale = uiInWorldSpace
					? GuiRenderer::ComputeWorldUIPixelScale()
					: 0.0f;
				auto renderOne = [&](Scene& s) {
					if (uiInWorldSpace) {
						gui->RenderScene(s, vp, pixelToWorldScale);
					} else {
						gui->RenderScene(s);
					}
				};
				if (onlyPassedScene) {
					renderOne(scene);
				}
				else {
					SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
						renderOne(s);
						});
				}
			}

			if (withGizmos && Gizmo::IsEnabled()) {
				const GizmoLayerMask layerMask = sharedGizmosOnly ? GizmoLayerMask::Shared : GizmoLayerMask::All;
				GizmoRenderer2D::RenderWithVP(vp, layerMask);
			}
		};

		// Wireframe pass uses RenderApi::BeginColorLogicOpClear (GL_CLEAR
		// under the hood) instead of relying on each shader to honour a
		// debug uniform. The logic-op forces every touched pixel's RGB
		// to 0 *after* the fragment shader runs, and the alpha mask
		// keeps the FBO opaque. The net effect is "every triangle edge
		// becomes solid black, regardless of what the entity's shader/
		// texture/colour would normally output." Logic op also overrides
		// blending, so semi-transparent quads still emit an opaque black
		// edge — which is what the user wants in this debug view.
		auto runWireframePass = [&]() {
			RenderApi::SetPolygonMode(PolygonMode::Wireframe);
			RenderApi::BeginColorLogicOpClear();
			RenderApi::SetColorMask(true, true, true, false);
			runSceneRender();
			RenderApi::SetColorMask(true, true, true, true);
			RenderApi::EndColorLogicOpClear();
			RenderApi::SetPolygonMode(PolygonMode::Filled);
		};

		switch (drawMode) {
		case EditorViewDrawMode::Triangle:
			runWireframePass();
			break;
		case EditorViewDrawMode::Mixed:
			runSceneRender();
			runWireframePass();
			break;
		case EditorViewDrawMode::Default:
		default:
			runSceneRender();
			break;
		}

		RenderApi::BindDefaultFramebuffer();

		auto* window = Application::GetWindow();
		if (window) {
			RenderApi::SetViewport(0, 0, window->GetWidth(), window->GetHeight());
		}
	}

	void ImGuiEditorLayer::DrawEditorComponentGizmos(Scene& scene) {
		if (m_SelectedEntity == entt::null || !scene.IsValid(m_SelectedEntity)) {
			return;
		}

		const bool hasTransform = scene.HasComponent<Transform2DComponent>(m_SelectedEntity);
		const bool hasRectTransform = scene.HasComponent<RectTransform2DComponent>(m_SelectedEntity);

		// Per-component gizmos (registered by packages) are dispatched below
		// regardless of transform presence, so we no longer early-out when a
		// selection has neither Transform2D nor RectTransform2D — a registered
		// callback may want to paint on transformless entities (rare, but the
		// package surface allows it). Save/restore of gizmo state is cheap.

		const Color previousColor = Gizmo::GetColor();
		const float previousLineWidth = Gizmo::GetLineWidth();
		const GizmoLayer previousLayer = Gizmo::GetLayer();

		Gizmo::SetLayer(GizmoLayer::EditorOnly);

		if (hasTransform) {
			auto& transform = scene.GetComponent<Transform2DComponent>(m_SelectedEntity);
			const float rotationDegrees = transform.GetRotationDegrees();
	
			Gizmo::SetColor(Color(1.0f, 0.65f, 0.10f, 1.0f));
			Gizmo::SetLineWidth(2.0f);
			Gizmo::DrawSquare(transform.Position, transform.Scale, rotationDegrees);
	
			if (scene.HasComponent<Camera2DComponent>(m_SelectedEntity)) {
				auto& camera = scene.GetComponent<Camera2DComponent>(m_SelectedEntity);
				Gizmo::SetColor(Color::White());
				Gizmo::SetLineWidth(1.5f);
				Gizmo::DrawSquare(transform.Position, camera.WorldViewPort(), rotationDegrees);
			}
	
			if (scene.HasComponent<BoxCollider2DComponent>(m_SelectedEntity)) {
				auto& collider = scene.GetComponent<BoxCollider2DComponent>(m_SelectedEntity);
				if (collider.IsValid()) {
					const Vec2 center = transform.Position + Rotated(collider.GetCenter(), transform.Rotation);
					Gizmo::SetColor(Color(0.20f, 1.0f, 0.35f, 1.0f));
					Gizmo::SetLineWidth(2.0f);
					Gizmo::DrawSquare(center, collider.GetScale(), rotationDegrees);
				}
			}
	
			if (scene.HasComponent<CircleCollider2DComponent>(m_SelectedEntity)) {
				auto& collider = scene.GetComponent<CircleCollider2DComponent>(m_SelectedEntity);
				if (collider.IsValid()) {
					const Vec2 center = transform.Position + Rotated(collider.GetCenter(), transform.Rotation);
					Gizmo::SetColor(Color(0.20f, 1.0f, 0.35f, 1.0f));
					Gizmo::SetLineWidth(2.0f);
					Gizmo::DrawCircle(center, collider.GetRadius());
				}
			}
	
			if (scene.HasComponent<PolygonCollider2DComponent>(m_SelectedEntity)) {
				auto& collider = scene.GetComponent<PolygonCollider2DComponent>(m_SelectedEntity);
				if (collider.IsValid()) {
					// Outline by drawing line segments between consecutive world-space
					// vertices — Box2D's GetPolygon already applies the offset, scale,
					// and any custom hull, so we just need to project through the
					// entity's rotation to get the on-screen position.
					const std::vector<Vec2> worldPoints = collider.GetWorldPoints();
					if (worldPoints.size() >= 3) {
						Gizmo::SetColor(Color(0.20f, 1.0f, 0.35f, 1.0f));
						Gizmo::SetLineWidth(2.0f);
						const float rot = transform.Rotation;
						for (size_t i = 0; i < worldPoints.size(); ++i) {
							const Vec2 a = transform.Position + Rotated(worldPoints[i], rot);
							const Vec2 b = transform.Position + Rotated(worldPoints[(i + 1) % worldPoints.size()], rot);
							Gizmo::DrawLine(a, b);
						}
					}
				}
			}
	
			if (scene.HasComponent<FastBoxCollider2DComponent>(m_SelectedEntity)) {
				auto& collider = scene.GetComponent<FastBoxCollider2DComponent>(m_SelectedEntity);
				const Vec2 halfExtents = collider.GetHalfExtents();
				const Vec2 worldSize(
					std::abs(halfExtents.x * transform.Scale.x) * 2.0f,
					std::abs(halfExtents.y * transform.Scale.y) * 2.0f);
				Gizmo::SetColor(Color(0.10f, 0.85f, 0.85f, 1.0f));
				Gizmo::SetLineWidth(2.0f);
				Gizmo::DrawSquare(transform.Position, worldSize, rotationDegrees);
			}
	
			if (scene.HasComponent<FastCircleCollider2DComponent>(m_SelectedEntity)) {
				auto& collider = scene.GetComponent<FastCircleCollider2DComponent>(m_SelectedEntity);
				const float worldRadius = collider.GetRadius() * std::max(std::abs(transform.Scale.x), std::abs(transform.Scale.y));
				Gizmo::SetColor(Color(0.10f, 0.85f, 0.85f, 1.0f));
				Gizmo::SetLineWidth(2.0f);
				Gizmo::DrawCircle(transform.Position, worldRadius);
			}
	
			if (scene.HasComponent<ParticleSystem2DComponent>(m_SelectedEntity)) {
				auto& particleSystem = scene.GetComponent<ParticleSystem2DComponent>(m_SelectedEntity);
				Gizmo::SetColor(Color(1.0f, 0.20f, 0.75f, 1.0f));
				Gizmo::SetLineWidth(2.0f);
	
				std::visit([&](auto&& shape) {
					using T = std::decay_t<decltype(shape)>;
					if constexpr (std::is_same_v<T, ParticleSystem2DComponent::CircleParams>) {
						const float radius = shape.Radius * std::max(std::abs(transform.Scale.x), std::abs(transform.Scale.y));
						Gizmo::DrawCircle(transform.Position, radius);
					}
					else if constexpr (std::is_same_v<T, ParticleSystem2DComponent::SquareParams>) {
						const Vec2 size(
							std::abs(shape.HalfExtends.x * transform.Scale.x) * 2.0f,
							std::abs(shape.HalfExtends.y * transform.Scale.y) * 2.0f);
						Gizmo::DrawSquare(transform.Position, size, rotationDegrees);
					}
				}, particleSystem.Shape);
	
				Vec2 moveDirection = particleSystem.ParticleSettings.MoveDirection;
				if (LengthSquared(moveDirection) < 0.0001f) {
					moveDirection = Up();
				}
				moveDirection = Normalized(Rotated(moveDirection, transform.Rotation));
				const float indicatorLength = std::max(0.75f, std::max(std::abs(transform.Scale.x), std::abs(transform.Scale.y)));
				Gizmo::DrawLine(transform.Position, transform.Position + moveDirection * indicatorLength);
			}
		} // end if (hasTransform)

		if (hasRectTransform) {
			// UILayoutSystem normally runs inside GuiRenderer::RenderScene,
			// which fires later in this same RenderEditorView pass. Force a
			// layout refresh now so we read fresh resolved screen-space corners
			// for the selected rect (and its ancestors).
			ComputeUILayout(scene);

			auto& rect = scene.GetComponent<RectTransform2DComponent>(m_SelectedEntity);

			// The Editor View renders UI in world space (see
			// RenderSceneIntoFBO with uiInWorldSpace=true), scaling the
			// resolved UI-pixel coords by ComputeWorldUIPixelScale().
			// Mirror that scale here so the outline lands exactly on
			// the rendered widget instead of drifting away with camera
			// pan/zoom.
			const float worldScale = GuiRenderer::ComputeWorldUIPixelScale();

			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const Vec2 pivot = rect.ResolvedValid ? rect.ResolvedPivot
				: Vec2{ (bl.x + tr.x) * 0.5f, (bl.y + tr.y) * 0.5f };

			Vec2 corners[4] = {
				Vec2{ bl.x * worldScale, bl.y * worldScale },
				Vec2{ tr.x * worldScale, bl.y * worldScale },
				Vec2{ tr.x * worldScale, tr.y * worldScale },
				Vec2{ bl.x * worldScale, tr.y * worldScale },
			};

			// Rotate around the resolved pivot (in world units now) so
			// non-centered pivots match what the renderer draws.
			if (rect.Rotation != 0.0f) {
				const Vec2 worldPivot{ pivot.x * worldScale, pivot.y * worldScale };
				for (int i = 0; i < 4; ++i) {
					corners[i] = worldPivot + Rotated(corners[i] - worldPivot, rect.Rotation);
				}
			}

			Gizmo::SetColor(Color(0.30f, 0.80f, 1.0f, 1.0f));
			Gizmo::SetLineWidth(2.0f);
			Gizmo::DrawLine(corners[0], corners[1]);
			Gizmo::DrawLine(corners[1], corners[2]);
			Gizmo::DrawLine(corners[2], corners[3]);
			Gizmo::DrawLine(corners[3], corners[0]);

			const AABB rectHandleCamAABB = m_EditorCamera.GetViewportAABB();
			const float rectHandleWorldPerPx = rectHandleCamAABB.Scale().x / std::max(1.0f, static_cast<float>(m_EditorViewFBO.GetWidth()));
			const float rectHandleHalf = 5.0f * rectHandleWorldPerPx;
			const Vec2 rectHandleSize{ rectHandleHalf * 2.0f, rectHandleHalf * 2.0f };
			const float rectHandleRotationDegrees = Degrees(rect.Rotation);
			const Vec2 outerMidL{ (corners[0].x + corners[3].x) * 0.5f, (corners[0].y + corners[3].y) * 0.5f };
			const Vec2 outerMidR{ (corners[1].x + corners[2].x) * 0.5f, (corners[1].y + corners[2].y) * 0.5f };
			const Vec2 outerMidB{ (corners[0].x + corners[1].x) * 0.5f, (corners[0].y + corners[1].y) * 0.5f };
			const Vec2 outerMidT{ (corners[2].x + corners[3].x) * 0.5f, (corners[2].y + corners[3].y) * 0.5f };
			Gizmo::DrawSquare(outerMidL, rectHandleSize, rectHandleRotationDegrees);
			Gizmo::DrawSquare(outerMidR, rectHandleSize, rectHandleRotationDegrees);
			Gizmo::DrawSquare(outerMidB, rectHandleSize, rectHandleRotationDegrees);
			Gizmo::DrawSquare(outerMidT, rectHandleSize, rectHandleRotationDegrees);

			// ── Text margin gizmo ─────────────────────────────────
			// When the selected rect carries a TextRendererComponent,
			// paint an inner rect indicating the wrap area + four tiny
			// solid handle squares centred on each edge midpoint. The
			// handles are draggable; the InvisibleButton overlays + drag
			// math live in RenderEditorView, where mouse state and the
			// FBO's screen rect are in scope.
			if (scene.HasComponent<TextRendererComponent>(m_SelectedEntity)) {
				const auto& text = scene.GetComponent<TextRendererComponent>(m_SelectedEntity);

				// Margin is in pixel units (FontSize domain). Convert to
				// world units the same way the renderer does — via the
				// rect's uniform scale. Use the rect's local scale axis
				// since worldScale is already baked into `corners`.
				const float marginScale = worldScale * std::max(0.01f, std::abs(rect.Scale.x));
				const float ml = text.Margin.x * marginScale;
				const float mt = text.Margin.y * marginScale;
				const float mr = text.Margin.z * marginScale;
				const float mb = text.Margin.w * marginScale;

				// corners[] indices (world units, pre-rotation): 0=BL,1=BR,2=TR,3=TL.
				// Reconstruct the inner rect in unrotated space, then rotate
				// around the same pivot the outer rect uses. Working in
				// unrotated space lets margin offsets stay axis-aligned in
				// the rect's local frame, matching how the renderer applies
				// them (Margin.x always insets along the rect's local +X,
				// regardless of world rotation).
				const Vec2 innerBL{ bl.x * worldScale + ml, bl.y * worldScale + mb };
				const Vec2 innerTR{ tr.x * worldScale - mr, tr.y * worldScale - mt };
				Vec2 inner[4] = {
					Vec2{ innerBL.x, innerBL.y },
					Vec2{ innerTR.x, innerBL.y },
					Vec2{ innerTR.x, innerTR.y },
					Vec2{ innerBL.x, innerTR.y },
				};
				if (rect.Rotation != 0.0f) {
					const Vec2 worldPivot{ pivot.x * worldScale, pivot.y * worldScale };
					for (int i = 0; i < 4; ++i) {
						inner[i] = worldPivot + Rotated(inner[i] - worldPivot, rect.Rotation);
					}
				}

				Gizmo::SetColor(Color(0.95f, 0.95f, 0.95f, 0.85f));
				Gizmo::SetLineWidth(1.0f);
				Gizmo::DrawLine(inner[0], inner[1]);
				Gizmo::DrawLine(inner[1], inner[2]);
				Gizmo::DrawLine(inner[2], inner[3]);
				Gizmo::DrawLine(inner[3], inner[0]);

				// Edge-midpoint handles. Size scales with the camera's
				// world-per-pixel so the handles stay roughly screen-px
				// constant regardless of zoom — matches what users expect
				// from a manipulator handle.
				const AABB camAABB = m_EditorCamera.GetViewportAABB();
				const float worldPerScreenPx = camAABB.Scale().x / std::max(1.0f, static_cast<float>(m_EditorViewFBO.GetWidth()));
				const float handleHalf = 5.0f * worldPerScreenPx;
				const Vec2 handleSize{ handleHalf * 2.0f, handleHalf * 2.0f };
				const float rectRotDeg = Degrees(rect.Rotation);

				const Vec2 midL{ (inner[0].x + inner[3].x) * 0.5f, (inner[0].y + inner[3].y) * 0.5f };
				const Vec2 midR{ (inner[1].x + inner[2].x) * 0.5f, (inner[1].y + inner[2].y) * 0.5f };
				const Vec2 midB{ (inner[0].x + inner[1].x) * 0.5f, (inner[0].y + inner[1].y) * 0.5f };
				const Vec2 midT{ (inner[2].x + inner[3].x) * 0.5f, (inner[2].y + inner[3].y) * 0.5f };
				Gizmo::DrawSquare(midL, handleSize, rectRotDeg);
				Gizmo::DrawSquare(midR, handleSize, rectRotDeg);
				Gizmo::DrawSquare(midB, handleSize, rectRotDeg);
				Gizmo::DrawSquare(midT, handleSize, rectRotDeg);
			}
		}

		// Package-registered viewport gizmos. Walk every registered component
		// type and invoke `drawEditorGizmo` on any one the selected entity has.
		// Built-in components don't set this today (they paint via the
		// hardcoded branches above); the hook exists so packages — Tilemap2D,
		// future tools — can paint their own selection helper without the
		// editor having to learn about package-side types.
		if (auto* app = Application::GetInstance()) {
			if (auto* sm = app->GetSceneManager()) {
				Entity selected = scene.GetEntity(m_SelectedEntity);
				sm->GetComponentRegistry().ForEachComponentInfo(
					[&](const std::type_index&, const ComponentInfo& info) {
						if (info.drawEditorGizmo && info.has && info.has(selected)) {
							info.drawEditorGizmo(selected);
						}
					});
			}
		}

		Gizmo::SetLayer(previousLayer);
		Gizmo::SetColor(previousColor);
		Gizmo::SetLineWidth(previousLineWidth);
	}

	void ImGuiEditorLayer::RenderEditorView(Scene& scene) {
		m_IsEditorViewActive = ImGui::Begin("Editor View");

		if (!m_IsEditorViewActive) {
			m_IsEditorViewHovered = false;
			m_IsEditorViewFocused = false;
			ImGui::End();
			return;
		}

		// ── Top toolbar ─────────────────────────────────────────────
		// Mirror the Game View's per-window options strip: a single
		// row pinned above the viewport image. Currently exposes the
		// draw-mode selector (Default / Triangle / Mixed); add new
		// editor-only viewport options here so they group naturally.
		{
			constexpr const char* k_DrawModeLabels[] = { "Default", "Triangle", "Mixed" };
			const int currentIndex = static_cast<int>(m_EditorViewDrawMode);
			ImGui::TextUnformatted("Draw:");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(120.0f);
			if (ImGui::BeginCombo("##EditorViewDrawMode", k_DrawModeLabels[currentIndex])) {
				for (int i = 0; i < IM_ARRAYSIZE(k_DrawModeLabels); ++i) {
					const bool selected = (i == currentIndex);
					if (ImGui::Selectable(k_DrawModeLabels[i], selected)) {
						m_EditorViewDrawMode = static_cast<EditorViewDrawMode>(i);
					}
					if (selected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Editor View draw mode (Default / Triangle wireframe / Mixed overlay)");
			}
		}

		// Inspector edits and other panels run earlier in this same OnPreRender
		// phase — AFTER the scene system's OnPreRender propagation already
		// fired — so re-propagate here before either the gizmo pass or the
		// FBO render samples Position/Scale/Rotation. Without this, an edit
		// to a Local* field would only show up one frame later.
		if (IsInPrefabEditMode()) {
			TransformHierarchySystem::Propagate(*m_PrefabEditScene);
		}
		else {
			SceneManager::Get().ForeachLoadedScene([](Scene& s) {
				TransformHierarchySystem::Propagate(s);
				});
		}

		// In prefab-edit mode, every reference to the inspector's "scene"
		// in this function points at the detached prefab scene. We don't
		// want gizmos / camera icons / framebuffer renders to leak the
		// active scene's contents into the prefab viewport.
		Scene* renderScene = IsInPrefabEditMode() ? m_PrefabEditScene.get() : &scene;

		const ImVec2 viewportSize = ImGui::GetContentRegionAvail();
		const int fbW = static_cast<int>(viewportSize.x);
		const int fbH = static_cast<int>(viewportSize.y);

		if (fbW > 0 && fbH > 0) {

			m_EditorViewFBO.Recreate(fbW, fbH);
			m_EditorCamera.SetViewportSize(fbW, fbH);

			if (m_EditorViewFBO.IsValid()) {

				auto* app = Application::GetInstance();
				if (app) {
					auto& input = app->GetInput();
					float dt = app->GetTime().GetDeltaTimeUnscaled();

					Vec2 mouseDelta = { 0.0f, 0.0f };
					if (m_IsEditorViewHovered && input.GetMouse(MouseButton::Middle)) {
						mouseDelta = input.GetMouseDelta();
					}
					float scroll = m_IsEditorViewHovered ? input.ScrollValue() : 0.0f;

					m_EditorCamera.Update(dt, m_IsEditorViewHovered, mouseDelta, scroll);
				}

				if (!Application::GetIsPlaying()
					&& m_SelectedEntity != entt::null
					&& renderScene->IsValid(m_SelectedEntity)
					&& renderScene->HasComponent<ParticleSystem2DComponent>(m_SelectedEntity))
				{
					auto& particleSystem = renderScene->GetComponent<ParticleSystem2DComponent>(m_SelectedEntity);
					if (particleSystem.IsEmitting() || particleSystem.IsSimulating()) {
						const float dt = app ? app->GetTime().GetDeltaTimeUnscaled() : 0.0f;
						particleSystem.PreviewUpdate(dt);
					}
				}

				glm::mat4 vp = m_EditorCamera.GetViewProjectionMatrix();
				AABB viewAABB = m_EditorCamera.GetViewportAABB();
				Gizmo::SetViewportAABBOverride(viewAABB);
				DrawEditorComponentGizmos(*renderScene);

				static const Color k_EditorClearColor(0.18f, 0.18f, 0.20f, 1.0f);
				const Color k_PrefabClearColor(k_PrefabEditClearR, k_PrefabEditClearG, k_PrefabEditClearB, 1.0f);
				const Color& clearColor = IsInPrefabEditMode() ? k_PrefabClearColor : k_EditorClearColor;
				// uiInWorldSpace=true: UI joins sprites and gizmos in
				// the editor camera's world space so the user can pan
				// and zoom around the UI like any scene object.
				RenderSceneIntoFBO(m_EditorViewFBO, *renderScene, vp, viewAABB, true, false, clearColor, IsInPrefabEditMode(), true, m_EditorViewDrawMode);
				Gizmo::ClearViewportAABBOverride();

				// FBO color textures use the renderer's native top-left
				// origin (texel row 0 = top of the rendered image). Sample
				// with default UV(0,0)-(1,1) so what the engine drew at
				// world +y appears at the top of the panel.
				ImGui::Image(
					static_cast<ImTextureID>(static_cast<intptr_t>(m_EditorViewFBO.GetColorTextureBackendId())),
					viewportSize);

				ImVec2 imageTopLeft = ImGui::GetItemRectMin();

				if (m_SelectedEntity != entt::null
					&& renderScene->IsValid(m_SelectedEntity)
					&& renderScene->HasComponent<ParticleSystem2DComponent>(m_SelectedEntity))
				{
					auto& particleSystem = renderScene->GetComponent<ParticleSystem2DComponent>(m_SelectedEntity);
					const ImGuiWindowFlags overlayFlags =
						ImGuiWindowFlags_NoDecoration |
						ImGuiWindowFlags_NoMove |
						ImGuiWindowFlags_NoSavedSettings |
						ImGuiWindowFlags_NoDocking |
						ImGuiWindowFlags_AlwaysAutoResize;
					ImGui::SetNextWindowPos(
						ImVec2(imageTopLeft.x + viewportSize.x - 12.0f, imageTopLeft.y + viewportSize.y - 12.0f),
						ImGuiCond_Always,
						ImVec2(1.0f, 1.0f));
					ImGui::SetNextWindowBgAlpha(0.86f);
					if (ImGui::Begin("##ParticleSystem2DViewportControls", nullptr, overlayFlags)) {
						if (ImGui::Button("Play")) {
							particleSystem.Play();
							renderScene->MarkDirty();
						}
						ImGui::SameLine();
						if (ImGui::Button("Pause")) {
							particleSystem.Pause();
							renderScene->MarkDirty();
						}
						ImGui::SameLine();
						if (ImGui::Button("Stop")) {
							particleSystem.Stop();
							renderScene->MarkDirty();
						}
					}
					ImGui::End();
				}

				if (m_SelectedEntity != entt::null
					&& renderScene->IsValid(m_SelectedEntity)
					&& renderScene->HasComponent<RectTransform2DComponent>(m_SelectedEntity))
				{
					auto& rect = renderScene->GetComponent<RectTransform2DComponent>(m_SelectedEntity);

					const float worldScale = GuiRenderer::ComputeWorldUIPixelScale();
					const Vec2 bl = rect.GetBottomLeft();
					const Vec2 tr = rect.GetTopRight();
					const Vec2 pivot = rect.ResolvedValid ? rect.ResolvedPivot
						: Vec2{ (bl.x + tr.x) * 0.5f, (bl.y + tr.y) * 0.5f };

					Vec2 handles[4] = {
						Vec2{ bl.x * worldScale, ((bl.y + tr.y) * 0.5f) * worldScale },
						Vec2{ tr.x * worldScale, ((bl.y + tr.y) * 0.5f) * worldScale },
						Vec2{ ((bl.x + tr.x) * 0.5f) * worldScale, bl.y * worldScale },
						Vec2{ ((bl.x + tr.x) * 0.5f) * worldScale, tr.y * worldScale },
					};
					if (rect.Rotation != 0.0f) {
						const Vec2 worldPivot{ pivot.x * worldScale, pivot.y * worldScale };
						for (int i = 0; i < 4; ++i) {
							handles[i] = worldPivot + Rotated(handles[i] - worldPivot, rect.Rotation);
						}
					}

					auto worldToScreen = [&](const Vec2& w, ImVec2& outScreen) -> bool {
						glm::vec4 wp(w.x, w.y, 0.0f, 1.0f);
						glm::vec4 cp = vp * wp;
						if (cp.w == 0.0f) return false;
						const float ndcX = cp.x / cp.w;
						const float ndcY = cp.y / cp.w;
						outScreen.x = (ndcX * 0.5f + 0.5f) * viewportSize.x;
						outScreen.y = (1.0f - (ndcY * 0.5f + 0.5f)) * viewportSize.y;
						return true;
					};

					const AABB camAABB = m_EditorCamera.GetViewportAABB();
					const float worldPerScreenPxX = camAABB.Scale().x / std::max(1.0f, viewportSize.x);
					const float worldPerScreenPxY = camAABB.Scale().y / std::max(1.0f, viewportSize.y);
					const float scaleX = worldScale * std::max(0.01f, std::abs(rect.Scale.x));
					const float scaleY = worldScale * std::max(0.01f, std::abs(rect.Scale.y));
					constexpr const char* kButtonIds[4] = {
						"##RectSizeL", "##RectSizeR",
						"##RectSizeB", "##RectSizeT",
					};
					constexpr float kHandleSizePx = 12.0f;
					const float kHalf = kHandleSizePx * 0.5f;

					for (int i = 0; i < 4; ++i) {
						ImVec2 screen;
						if (!worldToScreen(handles[i], screen)) continue;

						const ImVec2 btnTL(imageTopLeft.x + screen.x - kHalf,
							imageTopLeft.y + screen.y - kHalf);
						ImGui::SetCursorScreenPos(btnTL);
						ImGui::InvisibleButton(kButtonIds[i], ImVec2(kHandleSizePx, kHandleSizePx));

						if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
							ImGui::SetMouseCursor(
								(i < 2) ? ImGuiMouseCursor_ResizeEW
										: ImGuiMouseCursor_ResizeNS);
						}
						if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
							const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
							const float localDx = (scaleX > 0.0f) ? (mouseDelta.x * worldPerScreenPxX) / scaleX : 0.0f;
							const float localDy = (scaleY > 0.0f) ? (-mouseDelta.y * worldPerScreenPxY) / scaleY : 0.0f;

							switch (i) {
							case 0: {
								float delta = localDx;
								if (rect.SizeDelta.x - delta < 1.0f) delta = rect.SizeDelta.x - 1.0f;
								rect.SizeDelta.x -= delta;
								rect.AnchoredPosition.x += delta * (1.0f - rect.Pivot.x);
								break;
							}
							case 1: {
								float delta = localDx;
								if (rect.SizeDelta.x + delta < 1.0f) delta = 1.0f - rect.SizeDelta.x;
								rect.SizeDelta.x += delta;
								rect.AnchoredPosition.x += delta * rect.Pivot.x;
								break;
							}
							case 2: {
								float delta = localDy;
								if (rect.SizeDelta.y - delta < 1.0f) delta = rect.SizeDelta.y - 1.0f;
								rect.SizeDelta.y -= delta;
								rect.AnchoredPosition.y += delta * (1.0f - rect.Pivot.y);
								break;
							}
							case 3: {
								float delta = localDy;
								if (rect.SizeDelta.y + delta < 1.0f) delta = 1.0f - rect.SizeDelta.y;
								rect.SizeDelta.y += delta;
								rect.AnchoredPosition.y += delta * rect.Pivot.y;
								break;
							}
							default:
								break;
							}
							renderScene->MarkDirty();
						}
					}
				}

				const float iconSize = 24.0f;
				const float halfIcon = iconSize * 0.5f;
				auto camView = renderScene->GetRegistry().view<Camera2DComponent, Transform2DComponent>();
				for (auto [ent, cam, transform] : camView.each()) {
					glm::vec4 worldPos(transform.Position.x, transform.Position.y, 0.0f, 1.0f);
					glm::vec4 clipPos = vp * worldPos;
					if (clipPos.w == 0.0f) continue;

					float ndcX = clipPos.x / clipPos.w;
					float ndcY = clipPos.y / clipPos.w;
					float screenX = (ndcX * 0.5f + 0.5f) * viewportSize.x;
					float screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * viewportSize.y;

					if (screenX < -halfIcon || screenX > viewportSize.x + halfIcon ||
						screenY < -halfIcon || screenY > viewportSize.y + halfIcon) {
						continue;
					}

					uint64_t camIcon = EditorIcons::Get("camera", 24);
					if (!camIcon) {
						continue;
					}

					ImVec2 iconPos(imageTopLeft.x + screenX - halfIcon, imageTopLeft.y + screenY - halfIcon);
					ImGui::GetWindowDrawList()->AddImage(
						static_cast<ImTextureID>(static_cast<intptr_t>(camIcon)),
						iconPos,
						ImVec2(iconPos.x + iconSize, iconPos.y + iconSize),
						ImVec2(0, 1), ImVec2(1, 0));
				}

				// ── Text margin draggable handles ─────────────────────
				// Layered on top of the FBO image as InvisibleButtons.
				// The visual squares are painted into the FBO by the
				// margin-gizmo block in DrawEditorComponentGizmos; this
				// loop only handles mouse picking + drag-to-margin
				// updates, so the picking rect always lines up with what
				// the user sees painted.
				if (m_SelectedEntity != entt::null
					&& renderScene->IsValid(m_SelectedEntity)
					&& renderScene->HasComponent<TextRendererComponent>(m_SelectedEntity)
					&& renderScene->HasComponent<RectTransform2DComponent>(m_SelectedEntity))
				{
					auto& text = renderScene->GetComponent<TextRendererComponent>(m_SelectedEntity);
					auto& rect = renderScene->GetComponent<RectTransform2DComponent>(m_SelectedEntity);

					const float worldScale = GuiRenderer::ComputeWorldUIPixelScale();
					const Vec2 bl = rect.GetBottomLeft();
					const Vec2 tr = rect.GetTopRight();
					const Vec2 pivot = rect.ResolvedValid ? rect.ResolvedPivot
						: Vec2{ (bl.x + tr.x) * 0.5f, (bl.y + tr.y) * 0.5f };
					const float marginScale = worldScale * std::max(0.01f, std::abs(rect.Scale.x));

					// Inner-rect midpoints in world space (matches the
					// gizmo block in DrawEditorComponentGizmos so the
					// invisible buttons sit exactly on the painted squares).
					const float innerLx = bl.x * worldScale + text.Margin.x * marginScale;
					const float innerRx = tr.x * worldScale - text.Margin.z * marginScale;
					const float innerBy = bl.y * worldScale + text.Margin.w * marginScale;
					const float innerTy = tr.y * worldScale - text.Margin.y * marginScale;
					const float midLx = innerLx, midLy = (innerBy + innerTy) * 0.5f;
					const float midRx = innerRx, midRy = (innerBy + innerTy) * 0.5f;
					const float midBx = (innerLx + innerRx) * 0.5f, midBy = innerBy;
					const float midTx = (innerLx + innerRx) * 0.5f, midTy = innerTy;

					Vec2 handles[4] = {
						Vec2{ midLx, midLy }, // 0 = Left
						Vec2{ midRx, midRy }, // 1 = Right
						Vec2{ midBx, midBy }, // 2 = Bottom
						Vec2{ midTx, midTy }, // 3 = Top
					};
					if (rect.Rotation != 0.0f) {
						const Vec2 worldPivot{ pivot.x * worldScale, pivot.y * worldScale };
						for (int i = 0; i < 4; ++i) {
							handles[i] = worldPivot + Rotated(handles[i] - worldPivot, rect.Rotation);
						}
					}

					// World→screen for each handle (same projection used
					// by the camera-icon overlay above).
					auto worldToScreen = [&](const Vec2& w, ImVec2& outScreen) -> bool {
						glm::vec4 wp(w.x, w.y, 0.0f, 1.0f);
						glm::vec4 cp = vp * wp;
						if (cp.w == 0.0f) return false;
						const float ndcX = cp.x / cp.w;
						const float ndcY = cp.y / cp.w;
						outScreen.x = (ndcX * 0.5f + 0.5f) * viewportSize.x;
						outScreen.y = (1.0f - (ndcY * 0.5f + 0.5f)) * viewportSize.y;
						return true;
					};

					// World units per screen pixel — needed to convert the
					// drag delta back into the rect's local pixel-domain
					// margin units.
					const AABB camAABB = m_EditorCamera.GetViewportAABB();
					const float worldPerScreenPxX = camAABB.Scale().x / std::max(1.0f, viewportSize.x);
					const float worldPerScreenPxY = camAABB.Scale().y / std::max(1.0f, viewportSize.y);

					constexpr const char* kButtonIds[4] = {
						"##TextMarginL", "##TextMarginR",
						"##TextMarginB", "##TextMarginT",
					};
					constexpr float kHandleSizePx = 12.0f;
					const float kHalf = kHandleSizePx * 0.5f;

					// Per-handle InvisibleButtons each call SetCursorScreenPos
					// then submit an InvisibleButton; ImGui's ItemSize clears
					// IsSetPos at the end of each item, so the loop itself
					// doesn't trip the "extending boundaries without an item
					// afterwards" check.
					//
					// We deliberately do NOT capture/restore an outer
					// cursor position around this loop. The capture site
					// is right after the FBO image, where ImGui has
					// already advanced cursor.y past CursorMaxPos.y for
					// the next-line slot — restoring to that position
					// at loop end with SetCursorScreenPos flips IsSetPos
					// back to true, and ImGui::End()'s post-window check
					// then asserts because cursor.y > max.y. Nothing in
					// this function consumes the cursor after this block
					// (just IsWindowHovered / IsWindowFocused / End), so
					// leaving the cursor wherever the last InvisibleButton
					// left it is harmless.

					for (int i = 0; i < 4; ++i) {
						ImVec2 screen;
						if (!worldToScreen(handles[i], screen)) continue;

						const ImVec2 btnTL(imageTopLeft.x + screen.x - kHalf,
							imageTopLeft.y + screen.y - kHalf);
						ImGui::SetCursorScreenPos(btnTL);
						ImGui::InvisibleButton(kButtonIds[i], ImVec2(kHandleSizePx, kHandleSizePx));

						if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
							// Cursor hint matching the axis being dragged.
							ImGui::SetMouseCursor(
								(i < 2) ? ImGuiMouseCursor_ResizeEW
										: ImGuiMouseCursor_ResizeNS);
						}
						if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
							const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
							// Convert pixel delta into rect-local pixel-margin
							// units. Up screen = -Y in world (we already flip
							// Y for screen → world), and growing margins shrink
							// the inner rect. Per-handle sign tables encode
							// the direction each handle should move the
							// corresponding margin axis.
							const float worldDx = mouseDelta.x * worldPerScreenPxX;
							const float worldDy = -mouseDelta.y * worldPerScreenPxY;
							// World-to-rect-pixel: divide by marginScale.
							const float pixDx = (marginScale > 0.0f) ? worldDx / marginScale : 0.0f;
							const float pixDy = (marginScale > 0.0f) ? worldDy / marginScale : 0.0f;
							switch (i) {
							case 0: text.Margin.x += pixDx; break; // Left   → drag right grows left margin
							case 1: text.Margin.z -= pixDx; break; // Right  → drag right SHRINKS right margin
							case 2: text.Margin.w += pixDy; break; // Bottom → drag up grows bottom margin
							case 3: text.Margin.y -= pixDy; break; // Top    → drag up SHRINKS top margin
							}
							// Margins are free to go negative — that lets a
							// designer push the inner text rect OUTSIDE the
							// host RectTransform2D's bounds (e.g. a label
							// whose visible text spills past the panel it
							// reads from). The previous non-negative clamp
							// was a defensive cap that vetoed that valid
							// authoring intent. The renderer still floors
							// the resulting wrap width at 0 so an over-
							// shot margin can't generate negative-width
							// wrap rects.
							if (renderScene) renderScene->MarkDirty();
						}
					}
					// (No cursor restore — see comment above the loop.)
				}
			}
		}
		else {
			ImGui::TextDisabled("Editor View has no drawable area");
		}

		m_IsEditorViewHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
		m_IsEditorViewFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		ImGui::End();
	}

	void ImGuiEditorLayer::RenderGameView(Scene& scene) {
		m_IsGameViewActive = ImGui::Begin("Game View");

		if (!m_IsGameViewActive) {
			ImGui::End();
			m_IsGameViewFocused = false;
			m_IsGameViewHovered = false;
			ApplicationEditorAccess::SetGameInputEnabled(false);
			return;
		}

		// Same rationale as RenderEditorView — propagate any inspector edits
		// from earlier in this OnPreRender phase before reading transforms.
		SceneManager::Get().ForeachLoadedScene([](Scene& s) {
			TransformHierarchySystem::Propagate(s);
			});

		const int aspectPresetIndex = std::clamp(m_GameViewAspectPresetIndex, 0, static_cast<int>(k_GameViewAspectPresets.size()) - 1);
		m_GameViewAspectPresetIndex = aspectPresetIndex;
		if (!m_GameViewAspectLoaded) {
			if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
				for (int i = 0; i < static_cast<int>(k_GameViewAspectPresets.size()); ++i) {
					if (project->GameViewAspect == k_GameViewAspectPresets[i].Label) {
						m_GameViewAspectPresetIndex = i;
						break;
					}
				}
			}
			m_GameViewAspectLoaded = true;
		}
		if (!m_GameViewVsyncLoaded) {
			if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
				m_GameViewVsync = project->GameViewVsync;
			}
			m_GameViewVsyncLoaded = true;
		}

		ImGui::SetNextItemWidth(140.0f);
		if (ImGui::BeginCombo("##GameViewAspect", k_GameViewAspectPresets[m_GameViewAspectPresetIndex].Label)) {
			for (int i = 0; i < static_cast<int>(k_GameViewAspectPresets.size()); ++i) {
				const bool selected = (i == m_GameViewAspectPresetIndex);
				if (ImGui::Selectable(k_GameViewAspectPresets[i].Label, selected)) {
					m_GameViewAspectPresetIndex = i;
					if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
						project->GameViewAspect = k_GameViewAspectPresets[i].Label;
						project->Save();
					}
				}
				if (selected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();
		if (ImGui::Checkbox("VSync##GameView", &m_GameViewVsync)) {
			m_GameViewHasRendered = false;
			if (AxiomProject* project = ProjectManager::GetCurrentProject()) {
				project->GameViewVsync = m_GameViewVsync;
				project->Save();
			}
		}

		// "Stats" toggle. Lives next to VSync; flips m_ShowGameViewStats so
		// the overlay block at the bottom of this function decides whether
		// to draw. Buttons show a depressed appearance when their overlay
		// is active so the user can see at a glance which are on.
		ImGui::SameLine();
		{
			const bool active = m_ShowGameViewStats;
			if (active) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			}
			if (ImGui::Button("Stats##GameView")) {
				m_ShowGameViewStats = !m_ShowGameViewStats;
			}
			if (active) {
				ImGui::PopStyleColor();
			}
		}

		// "Logs" toggle. Sibling to Stats; the log overlay stacks below
		// the stats overlay when both are visible.
		ImGui::SameLine();
		{
			const bool active = m_ShowGameViewLogs;
			if (active) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			}
			if (ImGui::Button("Logs##GameView")) {
				m_ShowGameViewLogs = !m_ShowGameViewLogs;
			}
			if (active) {
				ImGui::PopStyleColor();
			}
		}

		ImGui::Separator();

		const ImVec2 viewportSize = ImGui::GetContentRegionAvail();
		const float targetAspect = k_GameViewAspectPresets[m_GameViewAspectPresetIndex].Aspect;

		ImVec2 renderSize = viewportSize;
		if (targetAspect > 0.0f && viewportSize.x > 0.0f && viewportSize.y > 0.0f) {
			const float availableAspect = viewportSize.x / viewportSize.y;
			if (availableAspect > targetAspect) {
				renderSize.x = viewportSize.y * targetAspect;
			}
			else {
				renderSize.y = viewportSize.x / targetAspect;
			}
		}

		const int fbW = std::max(1, static_cast<int>(std::round(renderSize.x)));
		const int fbH = std::max(1, static_cast<int>(std::round(renderSize.y)));

		if (viewportSize.x > 0.0f && viewportSize.y > 0.0f) {
			Camera2DComponent* gameCam = Camera2DComponent::Main();
			if ((gameCam && gameCam->IsValid()) || !m_GameViewFBO.IsValid()) {
				m_GameViewFBO.Recreate(fbW, fbH);
			}

			auto drawNoCameraFallback = [&]() {
				const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
				const ImVec2 canvasMax(canvasMin.x + viewportSize.x, canvasMin.y + viewportSize.y);
				const ImVec2 imageMin(
					canvasMin.x + (viewportSize.x - renderSize.x) * 0.5f,
					canvasMin.y + (viewportSize.y - renderSize.y) * 0.5f);
				const ImVec2 imageMax(imageMin.x + renderSize.x, imageMin.y + renderSize.y);

				ImGui::InvisibleButton("##GameViewCanvas", viewportSize);
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(0, 0, 0, 255));
				if (m_GameViewHasRendered && m_GameViewFBO.IsValid()) {
					drawList->AddImage(
						static_cast<ImTextureID>(static_cast<intptr_t>(m_GameViewFBO.GetColorTextureBackendId())),
						imageMin,
						imageMax);
				}
				drawList->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 40));

				const char* message = "no main camera in scene";
				const ImVec2 textSize = ImGui::CalcTextSize(message);
				drawList->AddText(
					ImVec2(imageMin.x + (renderSize.x - textSize.x) * 0.5f,
						imageMin.y + (renderSize.y - textSize.y) * 0.5f),
					ImGui::GetColorU32(ImGuiCol_TextDisabled),
					message);

				float statsRenderedHeight = 0.0f;
				if (m_ShowGameViewStats) {
					m_GameViewStatsOverlay.RefreshIfDue(fbW, fbH);
					statsRenderedHeight = m_GameViewStatsOverlay.RenderInRect(imageMin, imageMax);
				}
				if (m_ShowGameViewLogs) {
					if (!m_GameViewLogOverlay) {
						m_GameViewLogOverlay = std::make_unique<Axiom::Diagnostics::LogOverlay>();
					}
					const float logYOffset = statsRenderedHeight > 0.0f
						? statsRenderedHeight + 8.0f
						: 0.0f;
					m_GameViewLogOverlay->RenderInRect(imageMin, imageMax, logYOffset);
				}
			};

			if (m_GameViewFBO.IsValid() && gameCam && gameCam->IsValid()) {
				Viewport* savedViewport = gameCam->GetViewport();
				if (!savedViewport) {
					drawNoCameraFallback();
					m_IsGameViewHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
					m_IsGameViewFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
					ApplicationEditorAccess::SetGameInputEnabled(m_IsGameViewFocused);
					ImGui::End();
					return;
				}
				const int savedW = savedViewport->GetWidth();
				const int savedH = savedViewport->GetHeight();

				// RAII guard: if RenderSceneIntoFBO throws, the camera viewport
				// is still restored before the exception unwinds out of ImGui::End.
				struct ViewportRestoreGuard {
					Viewport* vp;
					int w;
					int h;
					Camera2DComponent* cam;
					~ViewportRestoreGuard() {
						vp->SetSize(w, h);
						cam->UpdateViewport();
					}
				} guard{ savedViewport, savedW, savedH, gameCam };

				savedViewport->SetSize(fbW, fbH);
				gameCam->UpdateViewport();
				glm::mat4 vp = gameCam->GetViewProjectionMatrix();
				AABB viewAABB = gameCam->GetViewportAABB();
				const auto now = std::chrono::steady_clock::now();
				float targetFps = 0.0f;
				if (m_GameViewVsync) {
					if (auto* window = Application::GetWindow()) {
						const GLFWvidmode* videoMode = window->GetVideomode();
						targetFps = videoMode ? static_cast<float>(videoMode->refreshRate) : 60.0f;
					}
					else {
						targetFps = 60.0f;
					}
				}
				else {
					targetFps = std::max(Application::GetTargetFramerate(), 0.0f);
				}

				bool renderFrame = !m_GameViewHasRendered
					|| m_LastGameViewFbW != fbW
					|| m_LastGameViewFbH != fbH;
				if (!renderFrame && targetFps > 0.0f) {
					const auto frameDuration = std::chrono::duration<double>(1.0 / static_cast<double>(targetFps));
					renderFrame = now - m_LastGameViewRenderTime >= frameDuration;
				}
				else if (targetFps <= 0.0f) {
					renderFrame = true;
				}

				// Compute imageMin BEFORE rendering so we can publish UIRegion
				// before the UI pass runs. ImGui::GetCursorScreenPos returns the
				// position the upcoming InvisibleButton will use as its top-left
				// (matches what we'd otherwise get from ItemRectMin after the
				// button) — getting it ahead of time lets us set UIRegion in
				// time for the engine UI systems inside RenderSceneIntoFBO.
				const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
				const ImVec2 canvasMax(canvasMin.x + viewportSize.x, canvasMin.y + viewportSize.y);
				const ImVec2 imageMin(
					canvasMin.x + (viewportSize.x - renderSize.x) * 0.5f,
					canvasMin.y + (viewportSize.y - renderSize.y) * 0.5f);
				const ImVec2 imageMax(imageMin.x + renderSize.x, imageMin.y + renderSize.y);

				// Publish the panel's pixel rect so engine UI systems
				// (UILayoutSystem, UIEventSystem, GuiRenderer) resolve in
				// panel-relative coordinates instead of full-OS-window
				// coordinates. Without this, mouse hit-tests miss widgets
				// because the rendered position (panel-centered) differs
				// from the layout-resolved position (window-centered).
				//
				// Crucially this MUST happen before RenderSceneIntoFBO. The
				// game camera's m_Viewport aliases Window::s_MainViewport
				// (Camera2DComponent::Initialize), and savedViewport->SetSize
				// above mutates that shared object to the FBO size. UI systems
				// fall back to MainViewport when UIRegion is inactive, so
				// without the region published first they'd compute uiScale
				// against the temporarily-mutated MainViewport — different
				// from the editor view's uiScale and visually inconsistent
				// (rect resolves at one scale while the renderer projects with
				// another). Publishing UIRegion first pins layout, ortho, and
				// text rendering all to the same renderSize.
				Window::SetUIRegion(
					static_cast<int>(imageMin.x),
					static_cast<int>(imageMin.y),
					static_cast<int>(renderSize.x),
					static_cast<int>(renderSize.y));

				if (renderFrame) {
					RenderSceneIntoFBO(m_GameViewFBO, scene, vp, viewAABB, true, true, gameCam->GetClearColor());
					m_LastGameViewRenderTime = now;
					m_LastGameViewFbW = fbW;
					m_LastGameViewFbH = fbH;
					m_GameViewHasRendered = true;
				}

				// guard's destructor restores the viewport — explicit restore here
				// is no longer needed and would be a redundant double-set.

				ImGui::InvisibleButton("##GameViewCanvas", viewportSize);
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(0, 0, 0, 255));

				// FBO sampling uses default UVs — see RenderEditorView for
				// the rationale. Game View shares the same convention.
				drawList->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(m_GameViewFBO.GetColorTextureBackendId())),
					imageMin,
					imageMax);
				drawList->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 40));

				// Stats overlay — engine-level helper. The cached snapshot
				// refreshes at 30 Hz internally; we just feed the FBO size
				// and let it draw inside the rendered image rectangle.
				// Tracks rendered height so the log overlay below can stack.
				float statsRenderedHeight = 0.0f;
				if (m_ShowGameViewStats) {
					m_GameViewStatsOverlay.RefreshIfDue(fbW, fbH);
					statsRenderedHeight = m_GameViewStatsOverlay.RenderInRect(imageMin, imageMax);
				}

				// Log overlay — pinned same place as stats; offset down when
				// stats is also visible so the two don't overlap. Lazy-
				// constructed: subscribing to Log::OnLog at engine load is
				// safe (Log is initialized very early), but we mirror the
				// runtime's lazy-construction style for symmetry.
				if (m_ShowGameViewLogs) {
					if (!m_GameViewLogOverlay) {
						m_GameViewLogOverlay = std::make_unique<Axiom::Diagnostics::LogOverlay>();
					}
					const float logYOffset = statsRenderedHeight > 0.0f
						? statsRenderedHeight + 8.0f
						: 0.0f;
					m_GameViewLogOverlay->RenderInRect(imageMin, imageMax, logYOffset);
				}
			}
			else {
				drawNoCameraFallback();
			}
		}
		else {
			ImGui::TextDisabled("Game View has no drawable area");
		}

		m_IsGameViewHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
		m_IsGameViewFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		ApplicationEditorAccess::SetGameInputEnabled(m_IsGameViewFocused);
		ImGui::End();
	}

} // namespace Axiom
