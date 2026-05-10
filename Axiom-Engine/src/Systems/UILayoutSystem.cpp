#include "pch.hpp"
#include "Systems/UILayoutSystem.hpp"

#include "Collections/Viewport.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Tags.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/UI/ContentSizeFitterComponent.hpp"
#include "Components/UI/GridLayoutGroupComponent.hpp"
#include "Components/UI/HorizontalLayoutGroupComponent.hpp"
#include "Components/UI/VerticalLayoutGroupComponent.hpp"
#include "Components/UI/WidthConstraintComponent.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/TextRenderer.hpp"
#include "Math/Trigonometry.hpp"
#include "Project/AxiomProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/Scene.hpp"

#include <algorithm>
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
		//   parentRotation — the accumulated world rotation of all
		//     ancestors. Children inherit it so rotated panels rotate
		//     their kids.
		//
		//   parentScale — the accumulated world scale of all ancestors
		//     ((1, 1) at the root). Children inherit it as their own
		//     world Scale (read by sprite / text renderers for visual
		//     scaling), but *not* into their pixel Width/Height — see
		//     the size note below.
		//
		// Width/height inheritance contract:
		//   A child's pixel size = SizeDelta × world Scale (parent ⊙ local).
		//   The parent's resolved width/height does NOT contribute (so a
		//   stretch anchor doesn't inflate the child's authored size on
		//   its own), but the parent's accumulated world scale DOES
		//   multiply through — that's how scaling a panel scales every
		//   widget inside it. Anchors still control where in the parent
		//   the child sits.
		//
		// Rotation contract:
		//   A child's world rotation is parent.Rotation + LocalRotation.
		//   Children also have their pivot rotated AROUND the parent's
		//   pivot by the parent's rotation, so rotating a panel rotates
		//   the whole subtree as one rigid body. ResolvedMin/Max stay
		//   axis-aligned (the renderer reads them as the unrotated
		//   layout rect and rotates the quad around ResolvedPivot itself
		//   using the per-rect Rotation), so layout-group code continues
		//   to operate in unrotated parent-local space.
		void ResolveRect(RectTransform2DComponent& rect,
			const Vec2& parentMin, const Vec2& parentMax,
			float parentRotation, const Vec2& parentScale,
			const Vec2& parentPivot)
		{
			rect.Rotation = parentRotation + rect.LocalRotation;
			rect.Scale = Vec2{ parentScale.x * rect.LocalScale.x,
			                   parentScale.y * rect.LocalScale.y };

			const Vec2 parentSize{ parentMax.x - parentMin.x, parentMax.y - parentMin.y };

			// Inverted anchors (Min > Max on either axis) get tolerated
			// here rather than asserted: the inspector lets the user type
			// or drag anchor values one component at a time, so an
			// intermediate inverted state during a single edit is normal.
			// Asserting in the per-frame layout pass would crash the
			// editor mid-drag. Use clamped local copies for the math
			// instead — the user's authored values are preserved on the
			// component, so unswapping them re-yields the intended layout
			// on the very next frame.
			const Vec2 normMin{
				std::min(rect.AnchorMin.x, rect.AnchorMax.x),
				std::min(rect.AnchorMin.y, rect.AnchorMax.y)
			};
			const Vec2 normMax{
				std::max(rect.AnchorMin.x, rect.AnchorMax.x),
				std::max(rect.AnchorMin.y, rect.AnchorMax.y)
			};

			// Anchor span: a sub-rectangle inside the parent where the
			// child's anchor reference lives. AnchorMin == AnchorMax →
			// point anchor at a single fraction of the parent. When
			// AnchorMin != AnchorMax the centre of this span is the
			// reference point; the span itself does NOT scale the child.
			const Vec2 anchorBL{
				parentMin.x + parentSize.x * normMin.x,
				parentMin.y + parentSize.y * normMin.y
			};
			const Vec2 anchorTR{
				parentMin.x + parentSize.x * normMax.x,
				parentMin.y + parentSize.y * normMax.y
			};

			// Final size = SizeDelta × world Scale (parent ⊙ local). This
			// is the change from the prior "LocalScale only" formula —
			// without it, scaling a parent translated children but never
			// resized them, breaking the obvious "panel scales as a unit"
			// expectation.
			const Vec2 finalSize{
				rect.SizeDelta.x * rect.Scale.x,
				rect.SizeDelta.y * rect.Scale.y
			};

			// Anchor centre — the reference point that AnchoredPosition
			// is offset from.
			const Vec2 anchorCenter{
				(anchorBL.x + anchorTR.x) * 0.5f,
				(anchorBL.y + anchorTR.y) * 0.5f
			};

			// Pivot world position in pre-rotation parent-local space:
			// anchor centre + AnchoredPosition scaled by parent's world
			// scale so a scaled parent moves its children proportionally.
			Vec2 pivotWorld{
				anchorCenter.x + rect.AnchoredPosition.x * parentScale.x,
				anchorCenter.y + rect.AnchoredPosition.y * parentScale.y
			};

			// Apply the parent's rotation around its pivot. parentMin/Max
			// describe the parent's UNROTATED rect (renderer rotates the
			// final quad), so without this step a rotated parent would
			// keep its children visually pinned to the unrotated layout
			// position — drawn at the parent's pre-rotation slot but
			// individually rotated by the same angle, producing the
			// shifted-fill / floating-handle artefacts visible on a
			// rotated slider.
			if (parentRotation != 0.0f) {
				const float c = std::cos(parentRotation);
				const float s = std::sin(parentRotation);
				const float dx = pivotWorld.x - parentPivot.x;
				const float dy = pivotWorld.y - parentPivot.y;
				pivotWorld = {
					parentPivot.x + c * dx - s * dy,
					parentPivot.y + s * dx + c * dy
				};
			}

			// Bottom-left = pivotWorld - pivot * size, i.e. the rect grows
			// from the pivot. Stays axis-aligned at this stage; the per-
			// rect Rotation applied during draw rotates the quad around
			// pivotWorld using the renderer's rotation step.
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
			rect.ResolvedValid = true;
		}

		// Collect the children of `entity` that have a RectTransform2D and
		// aren't disabled, in the registry's hierarchy order. Used by the
		// layout-group passes so they can rewrite child anchors / sizes
		// before the children are resolved. Skips disabled children so
		// hidden rows don't take up a slot.
		std::vector<EntityHandle> CollectLaidOutChildren(entt::registry& registry, EntityHandle entity) {
			std::vector<EntityHandle> out;
			if (auto* hierarchy = registry.try_get<HierarchyComponent>(entity)) {
				out.reserve(hierarchy->Children.size());
				for (EntityHandle child : hierarchy->Children) {
					if (!registry.valid(child)) continue;
					if (!registry.all_of<RectTransform2DComponent>(child)) continue;
					if (registry.all_of<DisabledTag>(child)) continue;
					out.push_back(child);
				}
			}
			return out;
		}

		// Pin a child's RectTransform to the upper-left corner of the layout
		// frame and let AnchoredPosition + SizeDelta describe its slot in
		// pixels. Anchors of (0,1) put the child's pivot at the parent's
		// top-left, +X right and +Y down become +X right and -Y down once
		// flipped through pivot/anchored offsets, so the pixel math below
		// reads naturally as "left-of-frame, top-of-frame, width, height".
		//
		// `parentScale` is the parent's accumulated world scale (project UI
		// scale × LocalScale chain). Layout groups operate in scaled screen
		// pixels (so padding/spacing line up with the rendered frame), but
		// SizeDelta and AnchoredPosition feed back through ResolveRect which
		// multiplies them by the SAME parent scale. Without dividing here we
		// would scale twice and the child would overflow the parent. Dividing
		// out the parent scale produces an "unscaled" value that re-multiplies
		// to the intended scaled pixel size on the next ResolveRect call.
		void PinChildToTopLeft(RectTransform2DComponent& rect,
			float xLeft, float yTop, float width, float height, float parentScale)
		{
			const float invScale = parentScale != 0.0f ? 1.0f / parentScale : 1.0f;
			rect.AnchorMin = Vec2{ 0.0f, 1.0f };
			rect.AnchorMax = Vec2{ 0.0f, 1.0f };
			rect.Pivot     = Vec2{ 0.0f, 1.0f };
			rect.AnchoredPosition = Vec2{ xLeft * invScale, -yTop * invScale };
			rect.SizeDelta = Vec2{ width * invScale, height * invScale };
		}

		// Rewrites every direct child of `entity` so they sit in a single
		// row inside the parent's authored rect, separated by Spacing px,
		// respecting Padding and the alignment / expand flags.
		//
		// `parentWorldScale` is the parent's accumulated world scale and is
		// used to keep authored Padding/Spacing/CellSize values in screen
		// pixels: the parent's resolved frame is already in scaled pixels,
		// so subtracting unscaled padding would leave the frame too wide.
		// Multiplying padding/spacing by the parent's scale puts them in the
		// same coordinate system as the frame.
		void ApplyHorizontalLayout(entt::registry& registry, EntityHandle entity,
			const HorizontalLayoutGroupComponent& layout, const Vec2& parentMin, const Vec2& parentMax,
			float parentWorldScale)
		{
			auto children = CollectLaidOutChildren(registry, entity);
			if (children.empty()) return;
			if (layout.ReverseArrangement) std::reverse(children.begin(), children.end());

			const float s = parentWorldScale;
			const float frameWidth  = (parentMax.x - parentMin.x) - (layout.PaddingLeft + layout.PaddingRight) * s;
			const float frameHeight = (parentMax.y - parentMin.y) - (layout.PaddingTop  + layout.PaddingBottom) * s;
			const float frameLeft   = layout.PaddingLeft * s;
			const float frameTop    = layout.PaddingTop  * s;
			if (frameWidth <= 0.0f || frameHeight <= 0.0f) return;

			const int n = static_cast<int>(children.size());
			const float totalSpacing = layout.Spacing * s * static_cast<float>(std::max(0, n - 1));

			// Two width strategies: equal slots (ControlChildWidth +
			// ChildForceExpandWidth) or authored widths summed up. Equal-
			// slot wins when both are on; otherwise children keep their
			// authored SizeDelta.x. Authored sizes scale by `s` so they
			// share units with the frame.
			const bool equalSlots = layout.ControlChildWidth && layout.ChildForceExpandWidth;
			std::vector<float> widths(n);
			float sumWidth = 0.0f;
			if (equalSlots) {
				const float slot = (frameWidth - totalSpacing) / static_cast<float>(n);
				for (float& w : widths) { w = std::max(0.0f, slot); sumWidth += w; }
			}
			else {
				for (int i = 0; i < n; ++i) {
					const auto& cr = registry.get<RectTransform2DComponent>(children[i]);
					// UseChildScale: fold LocalScale into the natural size so
					// a scaled-up child reserves more space along the axis.
					const float scale = layout.UseChildScaleWidth ? cr.LocalScale.x : 1.0f;
					widths[i] = cr.SizeDelta.x * scale * s;
					sumWidth += widths[i];
				}
			}
			const float usedWidth = sumWidth + totalSpacing;

			// Horizontal alignment of the row block inside the frame.
			float xCursor = frameLeft;
			const int alignIdx = static_cast<int>(layout.ChildAlignment);
			const int hAlign = alignIdx % 3; // 0=Left,1=Center,2=Right
			if (hAlign == 1)      xCursor = frameLeft + (frameWidth - usedWidth) * 0.5f;
			else if (hAlign == 2) xCursor = frameLeft + (frameWidth - usedWidth);

			// Vertical alignment of each child inside the row's height.
			const int vAlign = alignIdx / 3; // 0=Upper,1=Middle,2=Lower

			for (int i = 0; i < n; ++i) {
				auto& cr = registry.get<RectTransform2DComponent>(children[i]);
				float w = widths[i];
				const float heightScale = layout.UseChildScaleHeight ? cr.LocalScale.y : 1.0f;
				float h = layout.ControlChildHeight ? frameHeight : cr.SizeDelta.y * heightScale * s;
				if (layout.ControlChildHeight && layout.ChildForceExpandHeight) {
					h = frameHeight;
				}
				h = std::max(0.0f, h);
				float yTop = frameTop;
				if (vAlign == 1)      yTop = frameTop + (frameHeight - h) * 0.5f;
				else if (vAlign == 2) yTop = frameTop + (frameHeight - h);
				PinChildToTopLeft(cr, xCursor, yTop, w, h, s);
				xCursor += w + layout.Spacing * s;
			}
		}

		void ApplyVerticalLayout(entt::registry& registry, EntityHandle entity,
			const VerticalLayoutGroupComponent& layout, const Vec2& parentMin, const Vec2& parentMax,
			float parentWorldScale)
		{
			auto children = CollectLaidOutChildren(registry, entity);
			if (children.empty()) return;
			if (layout.ReverseArrangement) std::reverse(children.begin(), children.end());

			const float s = parentWorldScale;
			const float frameWidth  = (parentMax.x - parentMin.x) - (layout.PaddingLeft + layout.PaddingRight) * s;
			const float frameHeight = (parentMax.y - parentMin.y) - (layout.PaddingTop  + layout.PaddingBottom) * s;
			const float frameLeft   = layout.PaddingLeft * s;
			const float frameTop    = layout.PaddingTop  * s;
			if (frameWidth <= 0.0f || frameHeight <= 0.0f) return;

			const int n = static_cast<int>(children.size());
			const float totalSpacing = layout.Spacing * s * static_cast<float>(std::max(0, n - 1));

			const bool equalSlots = layout.ControlChildHeight && layout.ChildForceExpandHeight;
			std::vector<float> heights(n);
			float sumHeight = 0.0f;
			if (equalSlots) {
				const float slot = (frameHeight - totalSpacing) / static_cast<float>(n);
				for (float& h : heights) { h = std::max(0.0f, slot); sumHeight += h; }
			}
			else {
				for (int i = 0; i < n; ++i) {
					const auto& cr = registry.get<RectTransform2DComponent>(children[i]);
					const float scale = layout.UseChildScaleHeight ? cr.LocalScale.y : 1.0f;
					heights[i] = cr.SizeDelta.y * scale * s;
					sumHeight += heights[i];
				}
			}
			const float usedHeight = sumHeight + totalSpacing;

			float yCursor = frameTop;
			const int alignIdx = static_cast<int>(layout.ChildAlignment);
			const int vAlign = alignIdx / 3; // 0=Upper,1=Middle,2=Lower
			if (vAlign == 1)      yCursor = frameTop + (frameHeight - usedHeight) * 0.5f;
			else if (vAlign == 2) yCursor = frameTop + (frameHeight - usedHeight);

			const int hAlign = alignIdx % 3; // 0=Left,1=Center,2=Right

			for (int i = 0; i < n; ++i) {
				auto& cr = registry.get<RectTransform2DComponent>(children[i]);
				float h = heights[i];
				const float widthScale = layout.UseChildScaleWidth ? cr.LocalScale.x : 1.0f;
				float w = layout.ControlChildWidth ? frameWidth : cr.SizeDelta.x * widthScale * s;
				if (layout.ControlChildWidth && layout.ChildForceExpandWidth) {
					w = frameWidth;
				}
				w = std::max(0.0f, w);
				float xLeft = frameLeft;
				if (hAlign == 1)      xLeft = frameLeft + (frameWidth - w) * 0.5f;
				else if (hAlign == 2) xLeft = frameLeft + (frameWidth - w);
				PinChildToTopLeft(cr, xLeft, yCursor, w, h, s);
				yCursor += h + layout.Spacing * s;
			}
		}

		void ApplyGridLayout(entt::registry& registry, EntityHandle entity,
			const GridLayoutGroupComponent& layout, const Vec2& parentMin, const Vec2& parentMax,
			float parentWorldScale)
		{
			auto children = CollectLaidOutChildren(registry, entity);
			if (children.empty()) return;

			const float s = parentWorldScale;
			const float frameWidth  = (parentMax.x - parentMin.x) - (layout.PaddingLeft + layout.PaddingRight) * s;
			const float frameHeight = (parentMax.y - parentMin.y) - (layout.PaddingTop  + layout.PaddingBottom) * s;
			const float frameLeft   = layout.PaddingLeft * s;
			const float frameTop    = layout.PaddingTop  * s;
			if (frameWidth <= 0.0f || frameHeight <= 0.0f) return;

			const int n = static_cast<int>(children.size());
			const float cellW = std::max(1.0f, layout.CellSize.x) * s;
			const float cellH = std::max(1.0f, layout.CellSize.y) * s;
			const float spacingX = layout.Spacing.x * s;
			const float spacingY = layout.Spacing.y * s;

			// Decide column / row counts from the constraint.
			int cols = 0;
			int rows = 0;
			switch (layout.Constraint) {
			case GridLayoutConstraint::FixedColumnCount:
				cols = std::max(1, layout.ConstraintCount);
				rows = (n + cols - 1) / cols;
				break;
			case GridLayoutConstraint::FixedRowCount:
				rows = std::max(1, layout.ConstraintCount);
				cols = (n + rows - 1) / rows;
				break;
			case GridLayoutConstraint::Flexible:
			default:
				if (layout.StartAxis == GridLayoutStartAxis::Horizontal) {
					cols = std::max(1, static_cast<int>((frameWidth + spacingX) / (cellW + spacingX)));
					rows = (n + cols - 1) / cols;
				}
				else {
					rows = std::max(1, static_cast<int>((frameHeight + spacingY) / (cellH + spacingY)));
					cols = (n + rows - 1) / rows;
				}
				break;
			}
			cols = std::max(1, cols);
			rows = std::max(1, rows);

			const float usedWidth  = static_cast<float>(cols) * cellW + static_cast<float>(std::max(0, cols - 1)) * spacingX;
			const float usedHeight = static_cast<float>(rows) * cellH + static_cast<float>(std::max(0, rows - 1)) * spacingY;

			const int alignIdx = static_cast<int>(layout.ChildAlignment);
			const int hAlign = alignIdx % 3;
			const int vAlign = alignIdx / 3;
			float originX = frameLeft;
			float originY = frameTop;
			if (hAlign == 1)      originX = frameLeft + (frameWidth - usedWidth) * 0.5f;
			else if (hAlign == 2) originX = frameLeft + (frameWidth - usedWidth);
			if (vAlign == 1)      originY = frameTop + (frameHeight - usedHeight) * 0.5f;
			else if (vAlign == 2) originY = frameTop + (frameHeight - usedHeight);

			const bool flipX = (layout.StartCorner == GridLayoutStartCorner::UpperRight)
				|| (layout.StartCorner == GridLayoutStartCorner::LowerRight);
			const bool flipY = (layout.StartCorner == GridLayoutStartCorner::LowerLeft)
				|| (layout.StartCorner == GridLayoutStartCorner::LowerRight);

			for (int i = 0; i < n; ++i) {
				int col = 0;
				int row = 0;
				if (layout.StartAxis == GridLayoutStartAxis::Horizontal) {
					col = i % cols;
					row = i / cols;
				}
				else {
					row = i % rows;
					col = i / rows;
				}
				if (flipX) col = (cols - 1) - col;
				if (flipY) row = (rows - 1) - row;
				const float x = originX + static_cast<float>(col) * (cellW + spacingX);
				const float y = originY + static_cast<float>(row) * (cellH + spacingY);
				// Reverse swaps which child lands in which cell: cell index
				// i now receives child[n-1-i] instead of child[i]. Cell
				// positions are still derived from StartCorner/StartAxis
				// (so the same on-screen geometry), only the children
				// list is mirrored. Done at consumption time so a layout
				// with Reverse=true on a 1-child grid behaves identically
				// to Reverse=false; only multi-child grids visibly flip.
				const int childIdx = layout.Reverse ? (n - 1 - i) : i;
				auto& cr = registry.get<RectTransform2DComponent>(children[childIdx]);
				PinChildToTopLeft(cr, x, y, cellW, cellH, s);
			}
		}

		// Per-entity natural-size pass: when a TextRendererComponent has
		// WrapMode::None, its host RectTransform2D's SizeDelta should hug
		// the text's measured dimensions instead of whatever the user
		// authored. The inspector disables the Width/Height fields in
		// this case (see DrawRectTransform2DInspector), and this pass
		// keeps the in-component values in sync each frame so other
		// systems (ContentSizeFitter on a parent, layout groups, the
		// renderer's text-positioning math) see the natural size.
		// Returns true if SizeDelta was rewritten — used by the caller
		// to skip the FitContentSize bubble-up when the rect is owned
		// entirely by the text.
		void FitTextNaturalSize(entt::registry& registry, EntityHandle entity)
		{
			auto* text = registry.try_get<TextRendererComponent>(entity);
			auto* rect = registry.try_get<RectTransform2DComponent>(entity);
			if (!text || !rect) return;
			if (text->WrapMode != TextWrapMode::None) return;

			Font* font = TextRenderer::ResolveFont(*text);
			if (!font || !font->IsLoaded()) return;

			const Vec2 naturalAtlasPx = TextRenderer::MeasureNaturalSize(
				*font, text->Text, text->LetterSpacing);

			// Convert atlas-pixel space → screen-pixel space (the same
			// `drawScale = FontSize / bakedSize` factor the renderer
			// uses on the way to the GPU).
			const float bakedSize = font->GetPixelSize() > 0.0f
				? font->GetPixelSize()
				: text->FontSize;
			const float pxScale = bakedSize > 0.0f ? (text->FontSize / bakedSize) : 1.0f;

			rect->SizeDelta.x = naturalAtlasPx.x * pxScale;
			rect->SizeDelta.y = naturalAtlasPx.y * pxScale;
		}

		// Bottom-up pre-pass that resolves ContentSizeFitter sizes against
		// each child's *authored* SizeDelta * LocalScale. Runs before the
		// top-down ResolveHierarchy so by the time a parent layout-group
		// reads this rect's SizeDelta, the fitted size is already in.
		// Recursive: a fitter whose child is also a fitter sees the inner
		// fitted size first because the inner runs as part of the descent.
		// Disabled children are skipped, matching CollectLaidOutChildren.
		void FitContentSize(entt::registry& registry, EntityHandle entity)
		{
			if (auto* hierarchy = registry.try_get<HierarchyComponent>(entity)) {
				for (EntityHandle child : hierarchy->Children) {
					if (registry.valid(child)) {
						FitContentSize(registry, child);
					}
				}
			}

			// Text natural-size hugs its rect when WrapMode::None; runs
			// after the recurse but before ContentSizeFitter so a parent
			// fitter measuring this entity sees the freshly-fitted text
			// dimensions, not the stale authored SizeDelta.
			FitTextNaturalSize(registry, entity);

			auto* rect = registry.try_get<RectTransform2DComponent>(entity);
			if (!rect) return;

			// ContentSizeFitter pass — wrapped so the WidthConstraint pass
			// below still runs on entities that have no fitter or no enabled
			// fit axes (the constraint is independent of CSF).
			if (auto* csf = registry.try_get<ContentSizeFitterComponent>(entity);
				csf && (csf->HorizontalFit || csf->VerticalFit))
			{
				float minLeft = 0.0f;
				float maxRight = 0.0f;
				float minTop = 0.0f;
				float maxBottom = 0.0f;
				bool anyChild = false;
				if (auto* hierarchy = registry.try_get<HierarchyComponent>(entity)) {
					for (EntityHandle child : hierarchy->Children) {
						if (!registry.valid(child)) continue;
						if (registry.all_of<DisabledTag>(child)) continue;
						auto* childRect = registry.try_get<RectTransform2DComponent>(child);
						if (!childRect) continue;
						// Authored extent ignores anchors deliberately — the
						// fitter assumes children carry their own size via
						// SizeDelta (the common, point-anchored case). Children
						// that stretch via AnchorMin != AnchorMax would feed
						// the fitter a degenerate size; avoid that pairing.
						const float w = childRect->SizeDelta.x * childRect->LocalScale.x;
						const float h = childRect->SizeDelta.y * childRect->LocalScale.y;
						// Compute the full bounding box of the child relative to
						// its AnchoredPosition, accounting for both sides of the
						// pivot. A centered child (pivot 0.5) at AnchoredPosition 0
						// with width 100 occupies [-50, +50] — the previous
						// formula only counted the +50 side and the parent ended
						// up at half the expected size.
						const float left   = childRect->AnchoredPosition.x - w * childRect->Pivot.x;
						const float right  = childRect->AnchoredPosition.x + w * (1.0f - childRect->Pivot.x);
						// AnchoredPosition.y uses screen-Y-down convention here
						// (positive Y goes downward inside the parent), so the
						// vertical extent flips relative to X.
						const float top    = -childRect->AnchoredPosition.y - h * (1.0f - childRect->Pivot.y);
						const float bottom = -childRect->AnchoredPosition.y + h * childRect->Pivot.y;
						minLeft   = std::min(minLeft, left);
						maxRight  = std::max(maxRight, right);
						minTop    = std::min(minTop, top);
						maxBottom = std::max(maxBottom, bottom);
						anyChild = true;
					}
				}
				if (anyChild) {
					if (csf->HorizontalFit) {
						rect->SizeDelta.x = std::max(0.0f, maxRight - minLeft) + csf->PaddingLeft + csf->PaddingRight;
					}
					if (csf->VerticalFit) {
						rect->SizeDelta.y = std::max(0.0f, maxBottom - minTop) + csf->PaddingTop + csf->PaddingBottom;
					}
				}
			}

			// WidthConstraint pass — clamps SizeDelta.x to [MinWidth, MaxWidth]
			// after the fitter has resolved, so a CSF that would otherwise
			// produce a row wider than MaxWidth is clipped before any parent
			// layout-group reads this rect's size. Negative bound = side
			// disabled (Unity LayoutElement convention).
			if (auto* wc = registry.try_get<WidthConstraintComponent>(entity)) {
				if (wc->MinWidth >= 0.0f) {
					rect->SizeDelta.x = std::max(rect->SizeDelta.x, wc->MinWidth);
				}
				if (wc->MaxWidth >= 0.0f) {
					rect->SizeDelta.x = std::min(rect->SizeDelta.x, wc->MaxWidth);
				}
			}
		}

		// Recursive walk. Always resolves the rect when present (even on
		// disabled entities) so children of a disabled subtree still
		// inherit a sensible parent rect for their own resolution. The
		// renderer / event system filter on DisabledTag separately, so
		// resolving a disabled rect just costs a few floats.
		void ResolveHierarchy(entt::registry& registry, EntityHandle entity,
			const Vec2& parentMin, const Vec2& parentMax,
			float parentRotation, const Vec2& parentScale,
			const Vec2& parentPivot)
		{
			RectTransform2DComponent* rect = registry.try_get<RectTransform2DComponent>(entity);

			Vec2 childParentMin = parentMin;
			Vec2 childParentMax = parentMax;
			float childParentRotation = parentRotation;
			Vec2 childParentScale = parentScale;
			Vec2 childParentPivot = parentPivot;

			if (rect) {
				ResolveRect(*rect, parentMin, parentMax, parentRotation, parentScale, parentPivot);
				childParentMin = rect->ResolvedMin;
				childParentMax = rect->ResolvedMax;
				childParentRotation = rect->Rotation;
				childParentScale = rect->Scale;
				childParentPivot = rect->ResolvedPivot;

				// Apply layout-group rewrites NOW — after the parent's
				// rect is resolved (so we know the frame size) but before
				// children are resolved (so the rewritten anchors /
				// sizes propagate). Mutually exclusive via the conflict
				// declarations in BuiltInComponentRegistration, so at
				// most one branch runs per entity. Pass childParentScale.x
				// so layout groups can scale Padding/Spacing/CellSize the
				// same way ResolveRect scales SizeDelta — without it, a
				// layout in a non-1.0 scale tree would have the right
				// frame size but the wrong cell pitch.
				const float layoutScale = childParentScale.x;
				if (auto* h = registry.try_get<HorizontalLayoutGroupComponent>(entity)) {
					ApplyHorizontalLayout(registry, entity, *h, childParentMin, childParentMax, layoutScale);
				}
				else if (auto* v = registry.try_get<VerticalLayoutGroupComponent>(entity)) {
					ApplyVerticalLayout(registry, entity, *v, childParentMin, childParentMax, layoutScale);
				}
				else if (auto* g = registry.try_get<GridLayoutGroupComponent>(entity)) {
					ApplyGridLayout(registry, entity, *g, childParentMin, childParentMax, layoutScale);
				}
			}

			if (auto* hierarchy = registry.try_get<HierarchyComponent>(entity)) {
				for (EntityHandle child : hierarchy->Children) {
					if (registry.valid(child)) {
						ResolveHierarchy(registry, child,
							childParentMin, childParentMax,
							childParentRotation, childParentScale,
							childParentPivot);
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

		// Canvas-Scaler-style "Scale With Screen Size": SizeDelta /
		// AnchoredPosition / Padding values are interpreted as reference
		// pixels at the project's authored resolution. The actual canvas
		// uses the window's pixel size, but every UI dimension is scaled
		// by `s = (curW/refW)^(1-match) × (curH/refH)^match` so a 1080p-
		// authored layout shrinks to fit a 720p window or grows to fill a
		// 4K window automatically. Match=0.5 (geometric mean) is the
		// neutral default; bump toward 1.0 for HUDs anchored to top/bottom
		// edges, toward 0.0 for side-bar menus. Anchors stay relative to
		// the actual canvas so corners/edges still pin where the user
		// placed them.
		float uiScale = 1.0f;
		if (const AxiomProject* project = ProjectManager::GetCurrentProject()) {
			const int refW = std::max(1, project->UIReferenceWidth);
			const int refH = std::max(1, project->UIReferenceHeight);
			const float xRatio = static_cast<float>(vpW) / static_cast<float>(refW);
			const float yRatio = static_cast<float>(vpH) / static_cast<float>(refH);
			const float match  = std::clamp(project->UIScaleMatch, 0.0f, 1.0f);
			// Log-space blend so match=0/1 give pure x/y ratios and 0.5 gives
			// the geometric mean. Equivalent to xRatio^(1-match) × yRatio^match.
			if (xRatio > 0.0f && yRatio > 0.0f) {
				const float logBlend = (1.0f - match) * std::log(xRatio) + match * std::log(yRatio);
				uiScale = std::exp(logBlend);
			}
			// Defensive clamp: a degenerate project file with a near-zero
			// reference resolution could otherwise produce inf/NaN.
			if (!std::isfinite(uiScale) || uiScale <= 0.0f) {
				uiScale = 1.0f;
			}
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

		// First pass: bottom-up ContentSizeFitter resolution. Updates
		// SizeDelta on fitter entities so the next pass's ResolveRect
		// reflects fitted dimensions, and any parent layout-group sees
		// the fitted child size when computing its own layout.
		for (auto entity : allEntities) {
			const HierarchyComponent* hierarchy = registry.try_get<HierarchyComponent>(entity);
			const bool isRoot = !hierarchy || hierarchy->Parent == entt::null;
			if (!isRoot) continue;
			FitContentSize(registry, entity);
		}

		for (auto entity : allEntities) {
			const HierarchyComponent* hierarchy = registry.try_get<HierarchyComponent>(entity);
			const bool isRoot = !hierarchy || hierarchy->Parent == entt::null;
			if (!isRoot) continue;

			// Roots get the window centre as their "parent pivot" so root
			// rotation rotates around the screen centre — matches the
			// existing renderer behaviour where Rotation pivots about
			// ResolvedPivot. Root parentScale = (uiScale, uiScale) so the
			// project's reference-resolution scale propagates through every
			// rect's worldScale chain (consumed by ResolveRect for SizeDelta /
			// AnchoredPosition and by text rendering for font sizing).
			const Vec2 windowCenter{
				(windowMin.x + windowMax.x) * 0.5f,
				(windowMin.y + windowMax.y) * 0.5f
			};
			ResolveHierarchy(registry, entity, windowMin, windowMax, 0.0f, Vec2{ uiScale, uiScale }, windowCenter);
		}
	}

	void UILayoutSystem::Update(Scene& scene) {
		ComputeUILayout(scene);
	}

}
