#pragma once

#include "Collections/Color.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Components/UI/UITransitionMode.hpp"
#include "Core/UUID.hpp"
#include "Scene/EntityHandle.hpp"

#include <cstdint>
#include <entt/entt.hpp>
#include <string>

namespace Axiom {

	// Filter applied to typed input + clipboard paste. Programmatic
	// writes to Text are NOT filtered — game code is trusted to set
	// values it considers valid. Numeric types reject malformed
	// composite states (more than one '-' or '.', '-' anywhere but at
	// the start) by simulating insertion before each codepoint is
	// accepted.
	enum class InputContentType : std::uint8_t {
		Standard,        // accept everything printable
		AlphaNumeric,    // [A-Za-z0-9]
		Alpha,           // [A-Za-z]
		IntegerNumber,   // [0-9] plus a single leading '-'
		DecimalNumber,   // [0-9] plus single leading '-' and single '.'
	};

	// Text input widget state. Pairs with a TextRenderer child entity
	// (TextEntity) that displays Text (or PlaceholderText when Text is
	// empty and the field isn't focused). UIEventSystem owns the focus
	// state machine:
	//
	//   - Click inside the field's RectTransform2D → IsFocused = true
	//   - Click anywhere else → IsFocused = false
	//   - While focused, the system appends typed characters (from the
	//     OS char callback) to Text and treats Enter as a "submit"
	//     (sets SubmittedThisFrame).
	//   - Backspace removes selection if any, else the codepoint before
	//     the caret. Holding it auto-repeats after an initial delay.
	//   - Ctrl+A selects all; Ctrl+C / Ctrl+X / Ctrl+V copy / cut /
	//     paste via the OS clipboard.
	//   - Left-mouse-drag inside the field rect updates the caret while
	//     the anchor stays put — that's the "drag to select" gesture.
	//
	// The system writes the appropriate text into TextEntity's
	// TextRendererComponent every frame and tints it with TextColor or
	// PlaceholderColor depending on whether Text is empty + unfocused.
	// The caret and selection highlight are NOT inserted into the text
	// string — GuiRenderer renders them as separate quads so cursor
	// movement doesn't shift the underlying glyph layout.
	struct InputFieldComponent {
		std::string Text;
		std::string PlaceholderText = "Enter text...";

		bool IsFocused = false;
		bool SubmittedThisFrame = false;     // Enter pressed while focused
		int CharacterLimit = 0;              // 0 = unlimited

		// What the user is allowed to type / paste into this field. Has
		// no effect on programmatic Text writes.
		InputContentType ContentType = InputContentType::Standard;

		// When true the rendered text is masked one '*' per codepoint of
		// Text. Text itself is unchanged so game code reads what the
		// user actually typed.
		bool IsSecret = false;

		// Read-only fields still focus, scroll, and allow Copy / Select-
		// All (so the user can read + copy without altering content),
		// but reject Backspace / Delete / typed input / Cut / Paste.
		// Visual styling is unchanged — the field looks identical to a
		// normal one.
		bool IsReadOnly = false;

		// Caret blink frequency in Hz. 0 = never blink (caret stays on
		// while focused). Range exposed in the inspector clamps to
		// [0, 5].
		float CaretBlinkRate = 1.0f;

		// Caret bar width in pixels. Range exposed in the inspector
		// clamps to [1, 5]; the renderer enforces a >= 1 floor too.
		float CaretWidth = 1.0f;

		// Caret position and selection anchor, both as byte offsets into
		// Text (multi-byte UTF-8 codepoints are addressed at their leading
		// byte). The selected range is [min(Caret, Anchor), max(...)).
		// Caret == Anchor means "no selection" — typing inserts at the
		// caret rather than replacing.
		int CaretBytePos = 0;
		int SelectionAnchorBytePos = 0;

		// Transient runtime state (not serialized). MouseSelecting tracks
		// whether the most recent mouse-down landed inside this field's
		// rect, so we keep extending the selection while the cursor wanders
		// outside. The hold timers drive auto-repeat for Backspace / Delete
		// AND for the Left / Right caret-navigation arrows — initial fire
		// is on the down-edge, then we wait k_HoldDelay before repeating
		// at k_HoldRate. Without the arrow-key timers, holding Left/Right
		// only moved the caret a single codepoint regardless of duration.
		bool MouseSelecting = false;
		float BackspaceHoldTime = 0.0f;
		float BackspaceRepeatAccumulator = 0.0f;
		float DeleteHoldTime = 0.0f;
		float DeleteRepeatAccumulator = 0.0f;
		float LeftHoldTime = 0.0f;
		float LeftRepeatAccumulator = 0.0f;
		float RightHoldTime = 0.0f;
		float RightRepeatAccumulator = 0.0f;

		// Child entity carrying the TextRendererComponent that renders
		// either Text or PlaceholderText each frame. entt::null disables
		// the child-text path (the field still tracks Text in case the
		// game reads it directly).
		EntityHandle TextEntity = entt::null;

		// Tint applied to TextEntity when Text is non-empty or the field
		// is focused (so a caret-visible empty focused field also uses
		// TextColor).
		Color TextColor{ 0.10f, 0.10f, 0.10f, 1.0f };
		// Lighter tint applied when Text is empty AND not focused, so the
		// placeholder reads as "hint" rather than "real value".
		Color PlaceholderColor{ 0.55f, 0.55f, 0.55f, 1.0f };
		// Background tint for the selection-highlight quad. Alpha < 1 so
		// the underlying text stays readable through the highlight.
		Color SelectionColor{ 0.30f, 0.55f, 0.95f, 0.45f };
		// Color of the blinking caret bar.
		Color CaretColor{ 0.10f, 0.10f, 0.10f, 1.0f };

		// Per-state tint applied to the input field's background
		// ImageComponent. Mirrors ButtonComponent's preset: UIEventSystem
		// retints every frame from InteractableComponent's flags. The
		// text-related colors above (TextColor / PlaceholderColor /
		// SelectionColor / CaretColor) are independent and stay in their
		// own roles — these four only affect the background rect.
		// See ButtonComponent — same TransitionMode + per-state sprite
		// model.
		UITransitionMode TransitionMode = UITransitionMode::ColorTint;

		Color NormalColor   { 1.00f, 1.00f, 1.00f, 1.0f };
		Color HoveredColor  { 0.95f, 0.95f, 0.95f, 1.0f };
		Color PressedColor  { 0.90f, 0.90f, 0.90f, 1.0f };
		Color DisabledColor { 0.60f, 0.60f, 0.60f, 0.5f };
		// Alpha == 0 = "no focus tint" sentinel. See ButtonComponent.
		Color FocusedColor  { 0.00f, 0.00f, 0.00f, 0.0f };

		UUID NormalSprite  { 0 };
		UUID HoveredSprite { 0 };
		UUID PressedSprite { 0 };
		UUID DisabledSprite{ 0 };
		UUID FocusedSprite { 0 };

		// Inspector-bound event lists. OnValueChanged fires every frame
		// the Text mutates (per-keystroke) — methods that take a
		// `string` parameter receive the new text as the static
		// argument. OnSubmitted fires once when the user presses Enter
		// while the field is focused.
		InspectorEventList OnValueChanged;
		InspectorEventList OnSubmitted;

		// Mirror of `Text` from the previous tick — UIEventSystem
		// compares against this each frame to detect content edits and
		// fan them out to OnValueChanged. Initialised on the first
		// observation so a freshly-deserialised non-empty Text doesn't
		// fire a spurious "changed" event. Transient — not serialized.
		std::string LastObservedText;
		bool ValueObserved = false;
	};

}
