#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

#include <entt/entt.hpp>

namespace Axiom {

	// Direction the slider's value axis runs along. The handle slides
	// from the start of the chosen axis (Value=MinValue) toward the end
	// (Value=MaxValue), and the fill rect grows from that same start
	// edge regardless of how its pivot was authored.
	enum class SliderDirection : int {
		LeftToRight = 0,
		RightToLeft = 1,
		BottomToTop = 2,
		TopToBottom = 3,
	};

	// Slider widget state. UIEventSystem owns the dragging state
	// machine: while the user holds the mouse down on the handle,
	// Value is updated every frame from the cursor's position along
	// the active axis (X for horizontal directions, Y for vertical),
	// relative to the track's resolved rect. Game code reads Value
	// (and the convenience flag ValueChangedThisFrame) instead of
	// re-implementing the drag math.
	//
	// HandleEntity is the optional child entity used as the visual thumb;
	// the slider system repositions it along the track every frame by
	// updating its RectTransform2D::AnchoredPosition.x. By default the
	// preset places the InteractableComponent on this handle child, so
	// the thumb is the click/drag target rather than the whole track.
	// UIEventSystem falls back to an InteractableComponent on the slider
	// parent when the handle has none, so older scenes still work. Leave
	// HandleEntity as entt::null to draw the slider track-only (useful
	// when the thumb is implemented some other way, e.g. a fill bar);
	// in that case the parent must own the InteractableComponent for the
	// slider to be draggable.
	//
	// FillEntity is an optional child Image that grows/shrinks along the
	// X axis to visually represent the current value, like Unity's
	// "Fill Area / Fill" pair. The system rewrites its RectTransform2D
	// stretch each frame to span [0, t] of the track, where t is the
	// normalized [MinValue, MaxValue] position.
	//
	// IMPORTANT: the slider system OWNS the FillEntity's RectTransform2D
	// fields (AnchorMin/Max, Pivot, AnchoredPosition, SizeDelta) — every
	// frame they're rewritten from Value + Direction. Inspector edits
	// to those four fields on the fill entity look broken because they
	// reset on the next tick. Style the fill via its ImageComponent
	// (Color, Texture, sort layer) instead — those are NOT touched by
	// the slider. Same contract applies to HandleEntity: AnchoredPosition
	// is owned by the system, but SizeDelta is left alone.
	struct SliderComponent {
		float Value = 0.5f;       // current value in [MinValue, MaxValue]
		float MinValue = 0.0f;
		float MaxValue = 1.0f;
		bool WholeNumbers = false;

		// Which way Value increases visually. Drives both the fill's
		// growth direction (left/right/up/down from the start edge of
		// the track) and the handle's slide axis. The fill's authored
		// pivot/anchor are irrelevant — the slider system rewrites
		// the fill's RectTransform each frame so the fill always grows
		// from the start edge of the chosen direction.
		SliderDirection Direction = SliderDirection::LeftToRight;

		// When true the slider's value can't be changed by user input
		// (mouse drag or keyboard / controller adjustment). Visual
		// state still updates on hover / press so the user can see
		// they're aiming at it. Programmatic writes via script /
		// inspector still work.
		bool IsReadOnly = false;

		EntityHandle HandleEntity = entt::null;
		EntityHandle FillEntity = entt::null;
		// Optional explicit reference to the slider's static track
		// background. UIEventSystem auto-resolves a child named
		// "Background" with an ImageComponent when this is unset, so
		// existing scenes don't need a manual hookup. The slider
		// system never mutates this entity — it's exposed as a
		// scene-author convenience for theming and z-ordering.
		EntityHandle BackgroundEntity = entt::null;

		// Optional child whose TextRendererComponent gets rewritten each
		// frame to "{0..100}%" of the slider's normalised value. Used
		// by the Progress Bar preset (read-only slider with a centred
		// percent label). entt::null disables the auto-write — game
		// code can drive a label manually instead. Mirrors the
		// LabelEntity contract on Dropdown.
		EntityHandle LabelEntity = entt::null;

		// Set to true by UIEventSystem on the frame Value moved. Cleared
		// at the start of the next event-system tick so callers don't
		// have to remember a "previous value" themselves. The diff is
		// against LastObservedValue (below), so changes from any source
		// — drag, inspector edit, programmatic write — fan out to
		// OnValueChanged, not just drag-driven changes.
		bool ValueChangedThisFrame = false;

		// Last value UIEventSystem broadcast as "changed." Updated each
		// frame after the diff fires, so the next tick sees the freshly-
		// notified value as the baseline. Initialised on the first tick
		// (ValueObserved=false) so a scene-load deserialisation that sets
		// Value before any tick doesn't fire a spurious event. Transient —
		// not serialized.
		float LastObservedValue = 0.0f;
		bool ValueObserved = false;

		// Per-state tint applied to the draggable surface — the
		// HandleEntity's ImageComponent when present, otherwise the
		// slider parent's ImageComponent. Mirrors ButtonComponent's
		// preset: UIEventSystem retints every frame from the resolved
		// InteractableComponent's flags (whichever entity actually owns
		// the input — handle by default, parent as fallback).
		// See ButtonComponent — same TransitionMode + per-state sprite
		// model. Tinting applies to the resolved drag surface (handle
		// when present, slider parent otherwise), so SpriteSwap also
		// rewrites whichever ImageComponent UIEventSystem chose.
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

		// Transient drag state owned by UIEventSystem. PressMouseAxis is
		// the cursor coordinate (UI space) along the slider's value axis
		// when the handle was first pressed (X for horizontal, Y for
		// vertical) and PressValue is Value at that moment. IsDragging
		// stays false until the cursor has moved a small threshold since
		// the press, so a tap on the handle doesn't snap the value to the
		// cursor's exact position. Once dragging, Value tracks the
		// cursor's delta from the press point — not the absolute cursor
		// coord — so the handle follows the cursor smoothly instead of
		// jumping to it.
		float PressMouseAxis = 0.0f;
		float PressValue = 0.0f;
		bool IsDragging = false;

		// Inspector-bound event list — fires every binding on the rising
		// edge of `ValueChangedThisFrame`. Use a method that takes a
		// `float` parameter to receive the new value as the static
		// argument (the inspector renders a numeric editor for the
		// argument), or a `void` method for "value-changed-notify" only.
		InspectorEventList OnValueChanged;
	};

}
