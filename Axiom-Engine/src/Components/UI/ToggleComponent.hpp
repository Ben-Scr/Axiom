#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

#include <entt/entt.hpp>

namespace Axiom {

	// Boolean toggle (checkbox) widget state. CheckmarkEntity is the
	// child entity whose ImageComponent should be drawn only when
	// IsOn is true — UIEventSystem flips its enabled-state every
	// frame to match. Leave entt::null if the on/off look is handled
	// some other way (e.g. by tinting the parent image directly).
	//
	// ValueChangedThisFrame is set on the frame the toggle flips,
	// cleared at the start of the next tick.
	//
	// The four state colors (Normal/Hovered/Pressed/Disabled) tint the
	// background ImageComponent on the toggle entity, mirroring the
	// per-state preset on ButtonComponent. UIEventSystem retints every
	// frame from InteractableComponent's flags.
	struct ToggleComponent {
		bool IsOn = false;

		// When true the toggle ignores click input — IsOn never flips
		// from user interaction (mouse or keyboard activate). Visual
		// hover / press state still tracks so the widget remains
		// responsive-feeling, only the value mutation is gated.
		// Programmatic writes via script / inspector still work.
		bool IsReadOnly = false;

		EntityHandle CheckmarkEntity = entt::null;

		bool ValueChangedThisFrame = false;

		// Last IsOn UIEventSystem observed as "changed." Updated each
		// frame after the diff fires so changes from any source —
		// click, inspector edit, programmatic SetValue — feed
		// OnValueChanged, not just user clicks. ValueObserved gates
		// the first tick so scene load doesn't fire a spurious event.
		// Transient — not serialized.
		bool LastObservedIsOn = false;
		bool ValueObserved = false;

		// See ButtonComponent — same TransitionMode + per-state sprite
		// model.
		UITransitionMode TransitionMode = UITransitionMode::ColorTint;

		Color NormalColor   { 1.00f, 1.00f, 1.00f, 1.0f };
		Color HoveredColor  { 0.85f, 0.85f, 0.85f, 1.0f };
		Color PressedColor  { 0.65f, 0.65f, 0.65f, 1.0f };
		Color DisabledColor { 0.50f, 0.50f, 0.50f, 0.5f };
		// Alpha == 0 = "no focus tint" sentinel. See ButtonComponent.
		Color FocusedColor  { 0.00f, 0.00f, 0.00f, 0.0f };

		UUID NormalSprite  { 0 };
		UUID HoveredSprite { 0 };
		UUID PressedSprite { 0 };
		UUID DisabledSprite{ 0 };
		UUID FocusedSprite { 0 };

		// Inspector-bound event list — fires every binding on the rising
		// edge of `ValueChangedThisFrame`. Methods that take a `bool`
		// receive the new IsOn value as the static argument; void
		// methods just notify "toggle changed".
		InspectorEventList OnValueChanged;
	};

}
