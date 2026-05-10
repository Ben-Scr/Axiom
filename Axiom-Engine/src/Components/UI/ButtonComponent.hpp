#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

#include <entt/entt.hpp>

namespace Axiom {

	// Marker + visual-state preset for a clickable UI button. Pairs with
	// InteractableComponent (which provides the input flags) and an
	// ImageComponent on the same entity (which provides the background
	// rect that gets tinted). UIEventSystem applies the appropriate tint
	// per frame: NormalColor when idle, HoveredColor when the cursor is
	// inside, PressedColor while the mouse button is held down on this
	// entity, DisabledColor when InteractableComponent::Interactable is false.
	//
	// The button itself is just a state holder — game code reacts to
	// InteractableComponent::IsClicked to "do the thing" the button represents.
	struct ButtonComponent {
		// Optional explicit target graphic — the entity whose
		// ImageComponent (or TextRendererComponent) gets retinted /
		// sprite-swapped when the button changes state. When unset
		// (entt::null) UIEventSystem auto-resolves: it prefers an
		// ImageComponent on the button entity itself, falls back to
		// a TextRendererComponent on the button entity, and finally
		// gives up. Setting TargetGraphic to a child entity is the
		// "I want a label-only button whose visual lives elsewhere"
		// case, e.g. a panel that has both a background image and a
		// child text and you want the text colour to react.
		EntityHandle TargetGraphic = entt::null;

		// How the button visually transitions between input states.
		// Defaults to ColorTint (existing behaviour). See
		// UITransitionMode for the SpriteSwap and None modes.
		UITransitionMode TransitionMode = UITransitionMode::ColorTint;

		Color NormalColor   { 1.00f, 1.00f, 1.00f, 1.0f };
		Color HoveredColor  { 0.85f, 0.85f, 0.85f, 1.0f };
		Color PressedColor  { 0.65f, 0.65f, 0.65f, 1.0f };
		Color DisabledColor { 0.50f, 0.50f, 0.50f, 0.5f };

		// Optional focus-state tint applied when InteractableComponent
		// is Focusable + IsFocused. Alpha == 0 is treated as a sentinel
		// for "no focus tint" — the widget falls through to the hovered
		// or normal color as if focus didn't exist. This keeps the
		// "simple UI" experience for users who want navigation without
		// any visible indicator on the widget itself. Precedence:
		// disabled > pressed > focused > hovered > normal.
		Color FocusedColor  { 0.00f, 0.00f, 0.00f, 0.0f };

		// Per-state texture overrides used when TransitionMode is
		// SpriteSwap. Same precedence chain as the colors. Each slot
		// stores a UUID; UUID{0} = "unset, fall back to NormalSprite".
		// NormalSprite itself being unset disables the swap entirely
		// (the widget's authored ImageComponent.TextureAssetId is left
		// as-is). Ignored in ColorTint / None modes.
		UUID NormalSprite  { 0 };
		UUID HoveredSprite { 0 };
		UUID PressedSprite { 0 };
		UUID DisabledSprite{ 0 };
		UUID FocusedSprite { 0 };

		// Inspector-bound event list: parameterless C# methods invoked on the
		// rising edge of InteractableComponent::IsClicked. Mirrors Unity's
		// "On Click ()" foldout. UIEventSystem fires these via
		// InspectorEvents::Fire, which resolves each row's
		// (TargetEntityUUID, ScriptClassName, MethodName) against the active
		// scene at dispatch time.
		InspectorEventList OnClick;
	};

}
