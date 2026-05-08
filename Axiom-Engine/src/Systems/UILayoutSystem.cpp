#include "pch.hpp"
#include "Systems/UILayoutSystem.hpp"

#include "Collections/Viewport.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Tags.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Math/Trigonometry.hpp"
#include "Scene/Scene.hpp"

#include <cmath>

namespace Axiom {

	namespace {

		// Compose this entity's authored rect against a parent rect to get
		// the resolved screen-space AABB.
		//
		//   parentMin / parentMax — the parent's resolved rect (or the
		//     window viewport for root entities), in centered screen
		//     space (origin = window centre, +Y up).
		//
		//   parentRotation — the accumulated rotation of all ancestors.
		//     Children inherit it so rotated panels rotate their kids.
		void ResolveRect(RectTransform2DComponent& rect,
			const Vec2& parentMin, const Vec2& parentMax,
			float parentRotation)
		{
			// Inverted anchors silently produce negative widths/heights, which
			// cascade into garbled or invisible UI elements with no error. Catch
			// the authoring mistake in non-shipping builds; clamp at runtime so
			// production doesn't crash on malformed scenes.
			AIM_ASSERT(rect.AnchorMin.x <= rect.AnchorMax.x && rect.AnchorMin.y <= rect.AnchorMax.y,
				AxiomErrorCode::InvalidValue,
				"RectTransform2D AnchorMin must be <= AnchorMax (inverted anchors render as negative-size rects)");

			const Vec2 parentSize{ parentMax.x - parentMin.x, parentMax.y - parentMin.y };

			// Anchor span: the bottom-left and top-right of the anchor
			// rectangle inside the parent. AnchorMin == AnchorMax → point
			// anchor. AnchorMin != AnchorMax → stretch anchor.
			const Vec2 anchorBL{
				parentMin.x + parentSize.x * rect.AnchorMin.x,
				parentMin.y + parentSize.y * rect.AnchorMin.y
			};
			const Vec2 anchorTR{
				parentMin.x + parentSize.x * rect.AnchorMax.x,
				parentMin.y + parentSize.y * rect.AnchorMax.y
			};

			// Final size: stretched span between anchors, plus SizeDelta,
			// scaled by Scale. For point anchors the span is zero so size
			// = SizeDelta * Scale.
			const Vec2 finalSize{
				(anchorTR.x - anchorBL.x + rect.SizeDelta.x) * rect.Scale.x,
				(anchorTR.y - anchorBL.y + rect.SizeDelta.y) * rect.Scale.y
			};

			// Anchor centre — the reference point that AnchoredPosition
			// is offset from.
			const Vec2 anchorCenter{
				(anchorBL.x + anchorTR.x) * 0.5f,
				(anchorBL.y + anchorTR.y) * 0.5f
			};

			// Pivot world position = anchor centre + AnchoredPosition.
			// (For point anchors anchorCenter == anchorBL == anchorTR.)
			const Vec2 pivotWorld{
				anchorCenter.x + rect.AnchoredPosition.x,
				anchorCenter.y + rect.AnchoredPosition.y
			};

			// Bottom-left = pivotWorld - pivot * size, ie the rect grows
			// from the pivot.
			const Vec2 bottomLeft{
				pivotWorld.x - finalSize.x * rect.Pivot.x,
				pivotWorld.y - finalSize.y * rect.Pivot.y
			};
			const Vec2 topRight{
				bottomLeft.x + finalSize.x,
				bottomLeft.y + finalSize.y
			};

			rect.ResolvedMin = bottomLeft;
			rect.ResolvedMax = topRight;
			rect.ResolvedPivot = pivotWorld;
			rect.ResolvedRotation = parentRotation + rect.Rotation;
			rect.ResolvedValid = true;
		}

		// Recursive walk. Always resolves the rect when present (even on
		// disabled entities) so children of a disabled subtree still
		// inherit a sensible parent rect for their own resolution. The
		// renderer / event system filter on DisabledTag separately, so
		// resolving a disabled rect just costs a few floats.
		void ResolveHierarchy(entt::registry& registry, EntityHandle entity,
			const Vec2& parentMin, const Vec2& parentMax,
			float parentRotation)
		{
			RectTransform2DComponent* rect = registry.try_get<RectTransform2DComponent>(entity);

			Vec2 childParentMin = parentMin;
			Vec2 childParentMax = parentMax;
			float childParentRotation = parentRotation;

			if (rect) {
				ResolveRect(*rect, parentMin, parentMax, parentRotation);
				childParentMin = rect->ResolvedMin;
				childParentMax = rect->ResolvedMax;
				childParentRotation = rect->ResolvedRotation;
			}

			if (auto* hierarchy = registry.try_get<HierarchyComponent>(entity)) {
				for (EntityHandle child : hierarchy->Children) {
					if (registry.valid(child)) {
						ResolveHierarchy(registry, child,
							childParentMin, childParentMax, childParentRotation);
					}
				}
			}
		}

	} // namespace

	void ComputeUILayout(Scene& scene) {
		// Resolve against the window viewport, NOT the camera viewport —
		// the UI is screen-space and must not move when the camera moves.
		// We always use the window's framebuffer-pixel size so DPI scaling
		// works the same way as everything else in the engine.
		//
		// In editor mode the UI is rendered into a sub-panel of the OS
		// window; the editor publishes that panel's pixel region via
		// Window::SetUIRegion so layout resolves against the same size
		// the renderer (and hit-tester) use. Without this, the layout
		// would be sized to the full OS window while only a sub-rect is
		// actually visible, putting widgets far outside the panel.
		const Window::UIRegion uiRegion = Window::GetUIRegion();
		int vpW = 0;
		int vpH = 0;
		if (uiRegion.IsActive()) {
			vpW = uiRegion.Width;
			vpH = uiRegion.Height;
		}
		else {
			Window* window = Application::GetWindow();
			Viewport* vp = window ? Window::GetMainViewport() : nullptr;
			if (!vp || vp->GetWidth() <= 0 || vp->GetHeight() <= 0) {
				return;
			}
			vpW = vp->GetWidth();
			vpH = vp->GetHeight();
		}

		const float halfW = static_cast<float>(vpW) * 0.5f;
		const float halfH = static_cast<float>(vpH) * 0.5f;
		const Vec2 windowMin{ -halfW, -halfH };
		const Vec2 windowMax{ +halfW, +halfH };

		entt::registry& registry = scene.GetRegistry();

		// Reset every rect's ResolvedValid first — entities whose subtree
		// hasn't been visited (orphan entities, refs from dangling parent
		// pointers) end the pass with ResolvedValid=false so the renderer
		// / event system fall back to authored values.
		auto rectView = registry.view<RectTransform2DComponent>();
		for (auto entity : rectView) {
			rectView.get<RectTransform2DComponent>(entity).ResolvedValid = false;
		}

		// Walk roots (entities with no HierarchyComponent::Parent) and
		// recurse into each subtree. Entities without a HierarchyComponent
		// are also roots — but those without a RectTransform are skipped
		// by ResolveHierarchy itself.
		//
		// We don't filter on DisabledTag here: a disabled root's children
		// might still be enabled (DisabledTag doesn't propagate) and they
		// need their parent's resolved rect to compute correctly.
		auto allEntities = registry.view<entt::entity>();
		for (auto entity : allEntities) {
			const HierarchyComponent* hierarchy = registry.try_get<HierarchyComponent>(entity);
			const bool isRoot = !hierarchy || hierarchy->Parent == entt::null;
			if (!isRoot) continue;

			ResolveHierarchy(registry, entity, windowMin, windowMax, 0.0f);
		}
	}

	void UILayoutSystem::Update(Scene& scene) {
		ComputeUILayout(scene);
	}

}
