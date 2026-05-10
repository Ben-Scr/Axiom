#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

#include <entt/entt.hpp>
#include <string>
#include <vector>

namespace Axiom {

	// Dropdown / combo box widget state. The button portion is the
	// entity itself; the option list is rendered as a popup by
	// UIRenderer when IsOpen is true (drawn as floating rows directly
	// below the button rect, so no child entity is required for the
	// option list).
	//
	// SelectedIndex is the persisted choice. SelectionChangedThisFrame
	// is set by UIEventSystem on the frame the user picks a new option,
	// then cleared at the start of the next tick.
	//
	// LabelEntity is an optional child TextRenderer the system writes
	// the currently-selected option name into every frame, so the closed
	// dropdown's button shows the active selection (mirroring the
	// "header" cell of a Unity / TMP dropdown).
	struct DropdownComponent {
		std::vector<std::string> Options;
		int SelectedIndex = 0;

		bool IsOpen = false;
		bool SelectionChangedThisFrame = false;

		// Last index UIEventSystem broadcast as "changed." Updated each
		// frame after the diff fires so changes from any source — popup
		// click, inspector edit, programmatic write — feed
		// OnSelectedIndexChange. SelectionObserved gates the first tick
		// so scene load doesn't fire a spurious event. Transient — not
		// serialized.
		int LastObservedSelectedIndex = 0;
		bool SelectionObserved = false;

		// When true the dropdown can't be opened or have its selection
		// changed by user input. Visual hover / press state on the
		// closed header cell still tracks; programmatic writes to
		// IsOpen / SelectedIndex still work.
		bool IsReadOnly = false;

		// Child entity whose TextRendererComponent shows the current
		// selection on the dropdown button. entt::null disables the
		// auto-write (game code can drive it manually then).
		EntityHandle LabelEntity = entt::null;

		// Visual tuning for the popup option list (drawn by UIRenderer).
		float OptionRowHeight = 28.0f;
		Color PopupBackgroundColor{ 0.95f, 0.95f, 0.95f, 1.0f };
		Color OptionTextColor{ 0.10f, 0.10f, 0.10f, 1.0f };

		// Per-state tints for individual option rows in the popup.
		// Precedence (highest first): pressed > hovered > selected >
		// normal. Alpha == 0 on any slot is a "no override" sentinel
		// — that state falls through to the next-lower precedence
		// (so e.g. unset PressedColor reuses HoveredColor when held).
		// OptionHoverColor is the legacy name kept so existing scenes
		// don't lose their hover styling on upgrade.
		Color OptionNormalColor{ 0.00f, 0.00f, 0.00f, 0.0f };
		Color OptionHoverColor{ 0.85f, 0.90f, 1.00f, 1.0f };
		Color OptionPressedColor{ 0.00f, 0.00f, 0.00f, 0.0f };
		Color OptionSelectedColor{ 0.00f, 0.00f, 0.00f, 0.0f };

		// Per-state tint applied to the dropdown button's background
		// ImageComponent (the closed "header" cell, not the popup list —
		// the popup uses PopupBackgroundColor / OptionHoverColor above).
		// UIEventSystem retints every frame from InteractableComponent's
		// flags, same contract as ButtonComponent.
		// See ButtonComponent — same TransitionMode + per-state sprite
		// model. Affects the dropdown's closed "header" cell only;
		// the popup option list is unaffected by transition mode.
		UITransitionMode TransitionMode = UITransitionMode::ColorTint;

		Color NormalColor   { 1.00f, 1.00f, 1.00f, 1.0f };
		Color HoveredColor  { 0.92f, 0.94f, 0.98f, 1.0f };
		Color PressedColor  { 0.80f, 0.85f, 0.95f, 1.0f };
		Color DisabledColor { 0.60f, 0.60f, 0.60f, 0.5f };
		// Alpha == 0 = "no focus tint" sentinel. See ButtonComponent.
		Color FocusedColor  { 0.00f, 0.00f, 0.00f, 0.0f };

		UUID NormalSprite  { 0 };
		UUID HoveredSprite { 0 };
		UUID PressedSprite { 0 };
		UUID DisabledSprite{ 0 };
		UUID FocusedSprite { 0 };

		// Inspector-bound event list — fires every binding on the rising
		// edge of `SelectionChangedThisFrame`. Methods that take an
		// `int` parameter receive the new SelectedIndex as the static
		// argument; methods that take a `string` receive the option's
		// text. Void methods just notify "selection changed".
		InspectorEventList OnValueChanged;
	};

}
