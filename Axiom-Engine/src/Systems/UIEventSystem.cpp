#include "pch.hpp"
#include "Systems/UIEventSystem.hpp"

#include "Collections/Viewport.hpp"
#include "Components/General/HierarchyComponent.hpp"
#include "Components/General/NameComponent.hpp"
#include "Components/General/RectTransform2DComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Tags.hpp"
#include "Components/UI/ButtonComponent.hpp"
#include "Components/UI/DropdownComponent.hpp"
#include "Components/UI/InputFieldComponent.hpp"
#include "Components/UI/InteractableComponent.hpp"
#include "Components/UI/SliderComponent.hpp"
#include "Components/UI/ToggleComponent.hpp"
#include "Core/Application.hpp"
#include "Core/Input.hpp"
#include "Core/KeyCodes.hpp"
#include "Core/Log.hpp"
#include "Core/Window.hpp"
#include "Scene/Scene.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace Axiom {

	namespace {

		// Convert raw GLFW pixel coords (top-left origin, +Y down) to the
		// centred-origin, +Y-up screen space the layout system uses.
		Vec2 ScreenPixelToUiSpace(const Vec2& mouseRaw, int viewportWidth, int viewportHeight) {
			return Vec2{
				mouseRaw.x - static_cast<float>(viewportWidth) * 0.5f,
				static_cast<float>(viewportHeight) * 0.5f - mouseRaw.y
			};
		}

		Color ResolveButtonTint(const ButtonComponent& btn,
			const InteractableComponent& interact)
		{
			if (!interact.Interactable) return btn.DisabledColor;
			if (interact.IsPressed)     return btn.PressedColor;
			if (interact.IsHovered)     return btn.HoveredColor;
			return btn.NormalColor;
		}

		void SetEntityEnabled(Scene& scene, EntityHandle entity, bool enabled) {
			if (!scene.IsValid(entity)) return;
			scene.GetEntity(entity).SetEnabled(enabled);
		}

		// Convert a typed UTF-8 string into a "where would the resulting
		// text be" string, respecting the field's CharacterLimit. Caller
		// already ensured the field is focused.
		std::string AppendBoundedUtf8(const std::string& current,
			const std::string& typed, int characterLimit)
		{
			if (typed.empty()) return current;
			if (characterLimit <= 0) return current + typed;

			// CharacterLimit counts UTF-8 codepoints, not bytes.
			auto countCodepoints = [](const std::string& s) -> int {
				int count = 0;
				for (size_t i = 0; i < s.size(); ) {
					const unsigned char c = static_cast<unsigned char>(s[i]);
					if (c < 0x80) i += 1;
					else if ((c & 0xE0) == 0xC0) i += 2;
					else if ((c & 0xF0) == 0xE0) i += 3;
					else if ((c & 0xF8) == 0xF0) i += 4;
					else i += 1; // malformed; advance one byte
					++count;
				}
				return count;
			};

			const int currentCount = countCodepoints(current);
			if (currentCount >= characterLimit) return current;

			std::string out = current;
			int remaining = characterLimit - currentCount;
			for (size_t i = 0; i < typed.size() && remaining > 0; ) {
				const unsigned char c = static_cast<unsigned char>(typed[i]);
				size_t step = 1;
				if (c < 0x80) step = 1;
				else if ((c & 0xE0) == 0xC0) step = 2;
				else if ((c & 0xF0) == 0xE0) step = 3;
				else if ((c & 0xF8) == 0xF0) step = 4;

				if (i + step > typed.size()) break;
				out.append(typed, i, step);
				i += step;
				--remaining;
			}
			return out;
		}

		// Drop the last UTF-8 codepoint from `s`. Used for backspace.
		void RemoveLastUtf8Codepoint(std::string& s) {
			if (s.empty()) return;
			size_t i = s.size();
			while (i > 0) {
				--i;
				const unsigned char c = static_cast<unsigned char>(s[i]);
				// Continuation bytes are 10xxxxxx; back up past them.
				if ((c & 0xC0) != 0x80) break;
			}
			s.erase(i);
		}

		// Find the first child of `parent` matching the predicate. Used
		// to auto-resolve cross-entity refs (Slider's HandleEntity,
		// Toggle's CheckmarkEntity, InputField's TextEntity, Dropdown's
		// LabelEntity) so they survive scene reload without explicit
		// UUID round-tripping. Skip a child whose UUID matches `skip`
		// — that lets a slider with a Fill child also have a separate
		// Handle child without resolving both to the same entity.
		template <typename Pred>
		EntityHandle FindFirstChildWith(entt::registry& registry,
			EntityHandle parent, Pred&& predicate, EntityHandle skip = entt::null)
		{
			if (!registry.valid(parent)) return entt::null;
			auto* hierarchy = registry.try_get<HierarchyComponent>(parent);
			if (!hierarchy) return entt::null;
			for (EntityHandle child : hierarchy->Children) {
				if (!registry.valid(child)) continue;
				if (child == skip) continue;
				if (predicate(child)) return child;
			}
			return entt::null;
		}

		// First-by-name variant. Falls through to the predicate-only
		// FindFirstChildWith when no child has the requested name. The
		// presets in EntityHelper.cpp tag children with stable names
		// ("Handle", "Fill", "Text", "Label", "Checkmark") so the
		// post-reload auto-resolve picks the right child even when a
		// component has multiple children of the same shape (the slider
		// case: two Image children — one fill, one handle).
		template <typename Pred>
		EntityHandle FindFirstChildByNameOrPredicate(entt::registry& registry,
			EntityHandle parent, std::string_view preferredName,
			Pred&& predicate, EntityHandle skip = entt::null)
		{
			if (!registry.valid(parent)) return entt::null;
			auto* hierarchy = registry.try_get<HierarchyComponent>(parent);
			if (!hierarchy) return entt::null;

			// First pass: name match wins.
			for (EntityHandle child : hierarchy->Children) {
				if (!registry.valid(child)) continue;
				if (child == skip) continue;
				if (auto* name = registry.try_get<NameComponent>(child)) {
					if (name->Name == preferredName && predicate(child)) {
						return child;
					}
				}
			}
			// Second pass: shape match (any child satisfying the predicate).
			for (EntityHandle child : hierarchy->Children) {
				if (!registry.valid(child)) continue;
				if (child == skip) continue;
				if (predicate(child)) return child;
			}
			return entt::null;
		}

	} // namespace

	void UIEventSystem::Update(Scene& scene) {
		Application* app = Application::GetInstance();
		if (!app) return;
		Input& input = app->GetInput();

		// Prefer the editor-published UI panel region when active so
		// hit-tests resolve in the same coordinate space the panel was
		// rendered in (panel-relative pixels). For standalone runtime
		// builds the region stays unset and we fall back to the OS
		// window viewport, which is also where mouse coords originate.
		const Window::UIRegion uiRegion = Window::GetUIRegion();
		Vec2 mouseRaw = input.GetMousePosition();
		int vpW = 0;
		int vpH = 0;
		if (uiRegion.IsActive()) {
			mouseRaw.x -= static_cast<float>(uiRegion.OffsetX);
			mouseRaw.y -= static_cast<float>(uiRegion.OffsetY);
			vpW = uiRegion.Width;
			vpH = uiRegion.Height;
		}
		else {
			Viewport* viewport = Window::GetMainViewport();
			if (!viewport || viewport->GetWidth() <= 0 || viewport->GetHeight() <= 0) return;
			vpW = viewport->GetWidth();
			vpH = viewport->GetHeight();
		}

		const Vec2 mouseUi = ScreenPixelToUiSpace(mouseRaw, vpW, vpH);
		const bool mouseDownThisFrame = input.GetMouseDown(MouseButton::Left);
		const bool mouseUpThisFrame   = input.GetMouseUp(MouseButton::Left);
		const bool mouseHeld          = input.GetMouse(MouseButton::Left);

		auto& registry = scene.GetRegistry();

		// ── 0. Auto-resolve cross-entity references each frame ──────
		// Cross-entity refs aren't serialized — they're resolved by
		// finding the first child of the right shape. This survives
		// scene reload (refs default to entt::null after deserialize)
		// and editor flows that copy entities (refs become invalid;
		// re-resolve next frame). Explicit user-set refs survive too:
		// we only re-resolve when the field is null or refers to an
		// entity that no longer exists.

		const auto childHasImage = [&registry](EntityHandle e) {
			return registry.all_of<RectTransform2DComponent, ImageComponent>(e);
		};
		const auto childHasText = [&registry](EntityHandle e) {
			return registry.all_of<RectTransform2DComponent, TextRendererComponent>(e);
		};

		// Sliders: prefer name-matched children ("Fill", "Handle") so
		// the slider keeps working after scene reload even when both
		// cross-entity refs have to be re-resolved from scratch. Resolve
		// fill first, then handle excluding fill — guarantees the two
		// refs end up on different entities even with two image children.
		auto sliderResolveView = registry.view<SliderComponent>();
		for (auto&& [entity, slider] : sliderResolveView.each()) {
			if (slider.FillEntity == entt::null || !registry.valid(slider.FillEntity)) {
				slider.FillEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Fill", childHasImage, slider.HandleEntity);
			}
			if (slider.HandleEntity == entt::null || !registry.valid(slider.HandleEntity)) {
				slider.HandleEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Handle", childHasImage, slider.FillEntity);
			}
		}

		// Toggles: CheckmarkEntity (image, named "Checkmark" by default).
		auto toggleResolveView = registry.view<ToggleComponent>();
		for (auto&& [entity, toggle] : toggleResolveView.each()) {
			if (toggle.CheckmarkEntity == entt::null || !registry.valid(toggle.CheckmarkEntity)) {
				toggle.CheckmarkEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Checkmark", childHasImage);
			}
		}

		// Input fields: TextEntity (text renderer, named "Text" by default).
		auto inputResolveView = registry.view<InputFieldComponent>();
		for (auto&& [entity, field] : inputResolveView.each()) {
			if (field.TextEntity == entt::null || !registry.valid(field.TextEntity)) {
				field.TextEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Text", childHasText);
			}
		}

		// Dropdowns: LabelEntity (text renderer, named "Label" by default).
		auto dropdownResolveView = registry.view<DropdownComponent>();
		for (auto&& [entity, dropdown] : dropdownResolveView.each()) {
			if (dropdown.LabelEntity == entt::null || !registry.valid(dropdown.LabelEntity)) {
				dropdown.LabelEntity = FindFirstChildByNameOrPredicate(
					registry, entity, "Label", childHasText);
			}
		}

		// ── 1. Resolve dropdown popup hits FIRST ─────────────────────
		// Open dropdowns extend a popup below their button. When the
		// cursor is inside any popup row, that hit consumes the click —
		// rects underneath shouldn't react. We scan dropdowns up-front,
		// remember which row (if any) is hovered and, on click, mutate
		// the dropdown selection / IsOpen state directly.
		struct DropdownHit {
			EntityHandle Entity = entt::null;
			int RowIndex = -1; // -1 = no popup hit
		};
		DropdownHit dropdownHit;

		auto dropdownView = registry.view<RectTransform2DComponent, DropdownComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, rect, dd] : dropdownView.each()) {
			dd.SelectionChangedThisFrame = false;
			if (!dd.IsOpen || dd.Options.empty()) continue;

			const Vec2 bl = rect.GetBottomLeft();
			const Vec2 tr = rect.GetTopRight();
			const float width = tr.x - bl.x;
			const float topOfPopup = bl.y;

			for (int i = 0; i < static_cast<int>(dd.Options.size()); ++i) {
				const float rowTop = topOfPopup - dd.OptionRowHeight * static_cast<float>(i);
				const float rowBottom = rowTop - dd.OptionRowHeight;
				if (mouseUi.x >= bl.x && mouseUi.x <= bl.x + width
					&& mouseUi.y >= rowBottom && mouseUi.y <= rowTop)
				{
					dropdownHit.Entity = entity;
					dropdownHit.RowIndex = i;
					// Don't break — last (front-most) dropdown wins. Today
					// only one dropdown is typically open at a time; if
					// multiple are, the iteration order acts as tiebreak.
				}
			}
		}

		// ── 2. Hit-test interactable rects (skip when popup consumed) ─
		EntityHandle hovered = entt::null;
		const bool popupConsumes = dropdownHit.Entity != entt::null;

		auto hitView = registry.view<RectTransform2DComponent, InteractableComponent>(entt::exclude<DisabledTag>);
		if (!popupConsumes) {
			for (auto&& [entity, rect, interact] : hitView.each()) {
				if (!interact.Interactable) continue;
				if (rect.ContainsPoint(mouseUi)) {
					hovered = entity;
				}
			}
		}

		// ── 3. Per-entity interaction state machine ──────────────────
		for (auto&& [entity, rect, interact] : hitView.each()) {
			interact.IsHovered     = (entity == hovered);
			interact.IsMouseDown   = false;
			interact.IsMouseUp     = false;
			interact.IsClicked     = false;

			if (!interact.Interactable) {
				interact.IsPressed = false;
				continue;
			}

			if (interact.IsHovered && mouseDownThisFrame) {
				interact.IsMouseDown = true;
				interact.IsPressed = true;
				m_PressedEntity = entity;
			}

			if (!mouseHeld) {
				interact.IsPressed = false;
			}

			if (mouseUpThisFrame) {
				if (interact.IsHovered) {
					interact.IsMouseUp = true;
					if (m_PressedEntity == entity) {
						interact.IsClicked = true;
					}
				}
			}
		}

		if (mouseUpThisFrame) {
			m_PressedEntity = entt::null;
		}

		// ── 4. Dropdown popup actions ────────────────────────────────
		// Done before button/etc. so that closing a dropdown via outside
		// click doesn't also trigger a click on whatever's underneath.
		if (mouseDownThisFrame) {
			// Click outside any open dropdown closes it (without changing
			// selection). This matches OS-typical combo-box behaviour.
			if (!popupConsumes) {
				for (auto&& [entity, rect, dd] : dropdownView.each()) {
					if (dd.IsOpen && !rect.ContainsPoint(mouseUi)) {
						dd.IsOpen = false;
					}
				}
			}
		}
		if (mouseUpThisFrame && popupConsumes) {
			if (registry.valid(dropdownHit.Entity)
				&& registry.all_of<DropdownComponent>(dropdownHit.Entity))
			{
				auto& dd = registry.get<DropdownComponent>(dropdownHit.Entity);
				if (dropdownHit.RowIndex >= 0
					&& dropdownHit.RowIndex < static_cast<int>(dd.Options.size())
					&& dd.SelectedIndex != dropdownHit.RowIndex)
				{
					dd.SelectedIndex = dropdownHit.RowIndex;
					dd.SelectionChangedThisFrame = true;
				}
				dd.IsOpen = false;
			}
		}

		// ── 5. Button tints ──────────────────────────────────────────
		auto buttonView = registry.view<InteractableComponent, ButtonComponent, ImageComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, interact, btn, image] : buttonView.each()) {
			image.Color = ResolveButtonTint(btn, interact);
		}

		// ── 6. Sliders ───────────────────────────────────────────────
		auto sliderView = registry.view<InteractableComponent, SliderComponent, RectTransform2DComponent>(entt::exclude<DisabledTag>);
		for (auto&& [entity, interact, slider, rect] : sliderView.each()) {
			const float prevValue = slider.Value;
			slider.ValueChangedThisFrame = false;

			if (interact.Interactable && interact.IsPressed) {
				const Vec2 bl = rect.GetBottomLeft();
				const Vec2 size{ rect.GetTopRight().x - bl.x, rect.GetTopRight().y - bl.y };
				if (size.x > 0.0f) {
					float t = (mouseUi.x - bl.x) / size.x;
					t = std::clamp(t, 0.0f, 1.0f);
					float newValue = slider.MinValue + t * (slider.MaxValue - slider.MinValue);
					if (slider.WholeNumbers) {
						newValue = std::round(newValue);
					}
					slider.Value = newValue;
				}
			}

			// Clamp Value defensively (game code may have set it directly).
			slider.Value = std::clamp(slider.Value,
				std::min(slider.MinValue, slider.MaxValue),
				std::max(slider.MinValue, slider.MaxValue));

			if (slider.Value != prevValue) {
				slider.ValueChangedThisFrame = true;
			}

			const float range = slider.MaxValue - slider.MinValue;
			const float t = (range != 0.0f) ? (slider.Value - slider.MinValue) / range : 0.0f;
			const float trackWidth = rect.GetTopRight().x - rect.GetBottomLeft().x;

			// Move the optional handle child along the track. The handle's
			// AnchoredPosition.x is local to its parent's anchor centre,
			// so we map t -> [-half, +half].
			if (slider.HandleEntity != entt::null
				&& registry.valid(slider.HandleEntity)
				&& registry.all_of<RectTransform2DComponent>(slider.HandleEntity))
			{
				auto& handleRect = registry.get<RectTransform2DComponent>(slider.HandleEntity);
				handleRect.AnchoredPosition.x = -trackWidth * 0.5f + trackWidth * t;
			}

			// Resize the optional fill child to span [0, t] of the track,
			// authored by stretching its anchors against the parent rect.
			if (slider.FillEntity != entt::null
				&& registry.valid(slider.FillEntity)
				&& registry.all_of<RectTransform2DComponent>(slider.FillEntity))
			{
				auto& fillRect = registry.get<RectTransform2DComponent>(slider.FillEntity);
				// Stretch fill from the parent's left edge to fraction t.
				// Pivot must be (0.5, 0.5) so the rect fully fills the
				// [AnchorMin, AnchorMax] span: the layout system positions
				// the rect by `bottomLeft = pivotWorld - finalSize*Pivot`
				// where `pivotWorld = anchorCenter + AnchoredPosition`. With
				// Pivot=(0, 0.5) the fill's left edge ends up at the *anchor
				// centre* of the [0, t] span (i.e. 0.5*t into the track) and
				// the rect overshoots the right side, which is what produced
				// the off-by-half slider fill. (0.5, 0.5) makes
				// bottomLeft == anchorBL and topRight == anchorTR — fill
				// occupies exactly [0, t] of the track.
				fillRect.AnchorMin = Vec2{ 0.0f, 0.0f };
				fillRect.AnchorMax = Vec2{ t, 1.0f };
				fillRect.Pivot = Vec2{ 0.5f, 0.5f };
				fillRect.AnchoredPosition = Vec2{ 0.0f, 0.0f };
				fillRect.SizeDelta = Vec2{ 0.0f, 0.0f };
			}
		}

		// ── 7. Toggles ───────────────────────────────────────────────
		auto toggleView = registry.view<InteractableComponent, ToggleComponent>(entt::exclude<DisabledTag>);
		std::vector<std::pair<EntityHandle, bool>> deferredCheckmarkEnable;
		for (auto&& [entity, interact, toggle] : toggleView.each()) {
			toggle.ValueChangedThisFrame = false;
			if (interact.IsClicked) {
				toggle.IsOn = !toggle.IsOn;
				toggle.ValueChangedThisFrame = true;
			}

			if (toggle.CheckmarkEntity != entt::null && registry.valid(toggle.CheckmarkEntity)) {
				deferredCheckmarkEnable.emplace_back(toggle.CheckmarkEntity, toggle.IsOn);
			}
		}
		for (const auto& [checkmark, desiredEnabled] : deferredCheckmarkEnable) {
			SetEntityEnabled(scene, checkmark, desiredEnabled);
		}

		// ── 8. Input fields ──────────────────────────────────────────
		auto inputView = registry.view<InteractableComponent, InputFieldComponent>(entt::exclude<DisabledTag>);
		bool clickedAnyInputField = false;
		for (auto&& [entity, interact, field] : inputView.each()) {
			field.SubmittedThisFrame = false;
			if (interact.IsClicked) {
				field.IsFocused = true;
				clickedAnyInputField = true;
			}
		}
		if (mouseDownThisFrame && !clickedAnyInputField) {
			for (auto&& [entity, interact, field] : inputView.each()) {
				field.IsFocused = false;
			}
		}

		// Per-frame typed text + key edits, applied to every focused field.
		const std::string typedText = input.GetTypedTextUtf8();
		// Backspace and Enter via the engine's KeyCode enum (note the
		// name is `Backspace`, and numeric keypad enter is `KpEnter`).
		// We trigger on edges only to avoid auto-repeat duplication —
		// repeated typing comes through the char buffer directly.
		const bool backspacePressed = input.GetKeyDown(KeyCode::Backspace);
		const bool enterPressed = input.GetKeyDown(KeyCode::Enter)
			|| input.GetKeyDown(KeyCode::KpEnter);

		for (auto&& [entity, interact, field] : inputView.each()) {
			if (!field.IsFocused) continue;

			if (backspacePressed && !field.Text.empty()) {
				RemoveLastUtf8Codepoint(field.Text);
			}

			if (!typedText.empty()) {
				field.Text = AppendBoundedUtf8(field.Text, typedText, field.CharacterLimit);
			}

			if (enterPressed) {
				field.SubmittedThisFrame = true;
			}
		}

		// Sync the child TextRenderer for every input field so it shows
		// either the entered text (or placeholder when empty + unfocused).
		for (auto&& [entity, interact, field] : inputView.each()) {
			if (field.TextEntity == entt::null || !registry.valid(field.TextEntity)) continue;
			if (!registry.all_of<TextRendererComponent>(field.TextEntity)) continue;
			auto& tc = registry.get<TextRendererComponent>(field.TextEntity);

			const bool useText = !field.Text.empty() || field.IsFocused;
			tc.Text = useText ? field.Text : field.PlaceholderText;
			tc.Color = useText ? field.TextColor : field.PlaceholderColor;
		}

		// ── 9. Dropdowns: open/close, sync label ─────────────────────
		// Open/close on click is handled in the action block above (and
		// outside-click closing happens before #5). Here we just sync
		// the optional LabelEntity to display the selected option.
		for (auto&& [entity, rect, dd] : dropdownView.each()) {
			InteractableComponent* interact = registry.try_get<InteractableComponent>(entity);
			if (interact && interact->IsClicked && !popupConsumes) {
				dd.IsOpen = !dd.IsOpen;
			}

			if (dd.LabelEntity != entt::null && registry.valid(dd.LabelEntity)
				&& registry.all_of<TextRendererComponent>(dd.LabelEntity))
			{
				auto& tc = registry.get<TextRendererComponent>(dd.LabelEntity);
				if (!dd.Options.empty()) {
					const int idx = std::clamp(dd.SelectedIndex, 0,
						static_cast<int>(dd.Options.size()) - 1);
					tc.Text = dd.Options[idx];
				}
				else {
					tc.Text.clear();
				}
			}
		}
	}

}
