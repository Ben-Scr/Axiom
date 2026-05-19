using Index;
using Index.Interop;

namespace Index.UI;

// ── RectTransform ───────────────────────────────────────────────────
//
// Screen-space rectangle on a UI entity. Mirrors the C++
// RectTransform2DComponent — the engine's UILayoutSystem walks the
// hierarchy each frame and writes resolved corners back into the
// component, so reads of `Size` / `Position` reflect anchor + parent
// composition, not just the authored fields.

public class RectTransform : Component
{
    public Vector2 AnchorMin
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetAnchorMin(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetAnchorMin(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    public Vector2 AnchorMax
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetAnchorMax(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetAnchorMax(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    public Vector2 Pivot
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetPivot(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetPivot(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    public Vector2 AnchoredPosition
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetAnchoredPosition(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetAnchoredPosition(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    public Vector2 SizeDelta
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetSizeDelta(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetSizeDelta(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    // World rotation/scale — composed from this rect's Local* values
    // and every ancestor's. The getter returns the resolved world value
    // written by UILayoutSystem each frame; the setter writes the
    // authored Local* value (writing to the world cache directly would
    // be overwritten on the next layout pass). Mirrors Transform2D.
    public float Rotation
    {
        get => InternalCalls.RectTransform_GetRotation(RequireComponent<RectTransform>()) * Mathf.Rad2Deg;
        set => InternalCalls.RectTransform_SetRotation(RequireComponent<RectTransform>(), value * Mathf.Deg2Rad);
    }

    public Vector2 Scale
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetScale(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetScale(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    // Local rotation/scale — the authored values stored on this rect.
    // For root rects these match Rotation/Scale; for parented rects
    // they're the offset from the parent's world rotation/scale.
    public float LocalRotation
    {
        get => InternalCalls.RectTransform_GetLocalRotation(RequireComponent<RectTransform>()) * Mathf.Rad2Deg;
        set => InternalCalls.RectTransform_SetLocalRotation(RequireComponent<RectTransform>(), value * Mathf.Deg2Rad);
    }

    public Vector2 LocalScale
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetLocalScale(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.RectTransform_SetLocalScale(RequireComponent<RectTransform>(), value.X, value.Y);
    }

    // Resolved screen-space size as computed by UILayoutSystem this
    // frame. Combines anchor stretch + SizeDelta + parent rect; useful
    // for code that wants the actual on-screen size (e.g. positioning
    // a tooltip or scaling a child element). Read-only — set the
    // underlying values via SizeDelta / AnchorMin / AnchorMax.
    public Vector2 Size
    {
        get
        {
            ulong id = RequireComponent<RectTransform>();
            InternalCalls.RectTransform_GetResolvedSize(id, out float w, out float h);
            return new Vector2(w, h);
        }
    }
}

// ── Image ───────────────────────────────────────────────────────────

public class Image : Component
{
    public Vector4 Color
    {
        get
        {
            ulong id = RequireComponent<Image>();
            InternalCalls.Image_GetColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Image_SetColor(RequireComponent<Image>(), value.X, value.Y, value.Z, value.W);
    }

    public Texture? Texture
    {
        get
        {
            ulong assetId = InternalCalls.Image_GetTexture(RequireComponent<Image>());
            return Index.Texture.FromAssetUUID(assetId);
        }
        set => InternalCalls.Image_SetTexture(RequireComponent<Image>(), value?.UUID ?? 0);
    }

    public int SortingOrder
    {
        get => InternalCalls.Image_GetSortingOrder(RequireComponent<Image>());
        set => InternalCalls.Image_SetSortingOrder(RequireComponent<Image>(), value);
    }

    public int SortingLayer
    {
        get => InternalCalls.Image_GetSortingLayer(RequireComponent<Image>());
        set => InternalCalls.Image_SetSortingLayer(RequireComponent<Image>(), value);
    }
}

// ── Interactable ────────────────────────────────────────────────────
//
// Per-frame UI input state. Reads of IsHovered/IsClicked/etc are
// updated by UIEventSystem before any user code runs, so a script's
// OnUpdate() can react to clicks the same frame they happen.
//
// Events fire per-instance from UIEventDispatcher after UIEventSystem
// updates each frame. Subscribe on the specific Interactable whose
// transitions you care about — the handler receives that Interactable
// so you can check IsFocused / IsHovered / Entity directly:
//   interactable.OnClicked += i => Debug.Log($"{i.Entity.Name} clicked");

public class Interactable : Component
{
    public event Action<Interactable>? OnHovered;
    public event Action<Interactable>? OnClicked;

    // Fires every frame the user is holding the mouse down on this
    // Interactable — continuous, not edge. Use OnMouseDown for the
    // rising edge (one fire per press).
    public event Action<Interactable>? OnPressed;
    public event Action<Interactable>? OnMouseDown;

    // OnMouseUp pairs 1:1 with OnMouseDown — it fires on whichever
    // Interactable received the press, even if the cursor has since
    // left the rect. Use this for drag-style widgets that need to
    // release state on "let go" (joysticks, scrubbers, sliders). For
    // the hover-gated completion event (press AND release while still
    // over the widget), use OnClicked instead.
    public event Action<Interactable>? OnMouseUp;

    // Keyboard / controller focus transitions. OnFocusEnter fires on
    // the rising edge of IsFocused, OnFocusExit on the falling edge.
    // Both pass the Interactable so the handler can grab .Entity or
    // any sibling component without a closure-captured reference.
    public event Action<Interactable>? OnFocusEnter;
    public event Action<Interactable>? OnFocusExit;

    public bool IsInteractable
    {
        get => InternalCalls.Interactable_GetInteractable(RequireComponent<Interactable>());
        set => InternalCalls.Interactable_SetInteractable(RequireComponent<Interactable>(), value);
    }

    // Opt-in to keyboard / controller focus navigation. Defaults to
    // false (preserving simple mouse-only UI) — flip this to true on
    // any widget you want UIFocusSystem to include in Tab / D-pad /
    // arrow-key cycling. Existing scenes don't gain any focus-ring
    // visual unless you also set FocusedColor on the widget itself.
    public bool Focusable
    {
        get => InternalCalls.Interactable_GetFocusable(RequireComponent<Interactable>());
        set => InternalCalls.Interactable_SetFocusable(RequireComponent<Interactable>(), value);
    }

    // Read-only: these are driven entirely by UIEventSystem's hit-test
    // and press-tracking. Setting them from script wouldn't be honored
    // (they get overwritten next frame).
    public bool IsHovered => InternalCalls.Interactable_GetIsHovered(RequireComponent<Interactable>());
    public bool IsClicked => InternalCalls.Interactable_GetIsClicked(RequireComponent<Interactable>());
    public bool IsPressed => InternalCalls.Interactable_GetIsPressed(RequireComponent<Interactable>());
    public bool IsMouseDown => InternalCalls.Interactable_GetIsMouseDown(RequireComponent<Interactable>());
    public bool IsMouseUp => InternalCalls.Interactable_GetIsMouseUp(RequireComponent<Interactable>());

    // The currently-focused state, owned by UIFocusSystem. Setting
    // this from script is honoured for one frame as a programmatic
    // "give this widget focus" gesture; the system reconciles next
    // tick (and clears it if Focusable is false).
    public bool IsFocused
    {
        get => InternalCalls.Interactable_GetIsFocused(RequireComponent<Interactable>());
        set => InternalCalls.Interactable_SetIsFocused(RequireComponent<Interactable>(), value);
    }

    // Instance-side raisers used by UIEventDispatcher. Kept internal
    // (the events are public-subscribe only) so external code can't
    // synthesize fake transitions — only the engine pipeline drives
    // them. Each passes `this` to the handler so subscribers can read
    // any sibling state without a captured reference.
    internal void RaiseHovered() => OnHovered?.Invoke(this);
    internal void RaiseClicked() => OnClicked?.Invoke(this);
    internal void RaisePressed() => OnPressed?.Invoke(this);
    internal void RaiseMouseDown() => OnMouseDown?.Invoke(this);
    internal void RaiseMouseUp() => OnMouseUp?.Invoke(this);
    internal void RaiseFocusEnter() => OnFocusEnter?.Invoke(this);
    internal void RaiseFocusExit() => OnFocusExit?.Invoke(this);
}

// ── Button ──────────────────────────────────────────────────────────
//
// State-tint preset that the engine applies to the Image on the same
// entity each frame. Click semantics live on the sibling Interactable;
// the Button events fan out from the sibling Interactable's per-frame
// flags so a button with a separate TargetGraphic still receives them.
// OnClickDown / OnClicked / OnClickUp are edges (one fire per press);
// OnPressed is continuous (fires every frame the button is held).
// Each handler receives the Button so it can read any sibling
// component (Image, RectTransform, etc.) directly:
//   button.OnClickDown += b => b.PressedColor = Color.Red;
//   button.OnClicked   += b => Audio.Play("click");
//   button.OnClickUp   += b => b.PressedColor = Color.White;
//   button.OnPressed   += b => chargeAmount += Time.DeltaTime;

public class Button : Component
{
    // Rising edge of mouse-button-down while the cursor is over the
    // button (or its TargetGraphic). Fires once on the frame the press
    // starts.
    public event Action<Button>? OnClickDown;

    // Completed click: fires once on the frame the user releases the
    // mouse over the same button they pressed on. Mirrors the
    // sibling Interactable.OnClicked semantics.
    public event Action<Button>? OnClicked;

    // Rising edge of mouse-button-up while the cursor is over the
    // button (or its TargetGraphic). Fires whether or not the release
    // forms a completed click — pair with OnClickDown for hold-style
    // interactions (charge attacks, drag-to-cancel).
    public event Action<Button>? OnClickUp;

    // Fires every frame the button is being held — i.e. for the entire
    // span between OnClickDown and OnClickUp. Unlike the three edge
    // events above, this is continuous: a 30-frame press calls the
    // handler 30 times. Pair with Time.DeltaTime for charge meters,
    // auto-fire, hold-to-scroll, drag-preview behaviours.
    public event Action<Button>? OnPressed;

    // Optional explicit visual target. When set, the button retints /
    // sprite-swaps the ImageComponent (or, lacking one, the
    // TextRendererComponent) on this entity. Leaving it null falls
    // back to the button's own entity, picking Image first then
    // TextRenderer — so a button with a single Text child works
    // with no preset wiring.
    public Entity? TargetGraphic
    {
        get
        {
            ulong id = InternalCalls.Button_GetTargetGraphic(RequireComponent<Button>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Button_SetTargetGraphic(RequireComponent<Button>(), value?.ID ?? 0);
    }

    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<Button>();
            InternalCalls.Button_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Button_SetNormalColor(RequireComponent<Button>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<Button>();
            InternalCalls.Button_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Button_SetHoveredColor(RequireComponent<Button>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<Button>();
            InternalCalls.Button_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Button_SetPressedColor(RequireComponent<Button>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<Button>();
            InternalCalls.Button_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Button_SetDisabledColor(RequireComponent<Button>(), value.X, value.Y, value.Z, value.W);
    }

    // Tint applied when the sibling Interactable is focused via
    // UIFocusSystem. Alpha == 0 (the default) is treated as a
    // sentinel — the widget falls through to Hovered / Normal as if
    // focus didn't exist, so users who want navigation without any
    // visible indicator simply leave this color zeroed.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<Button>();
            InternalCalls.Button_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Button_SetFocusedColor(RequireComponent<Button>(), value.X, value.Y, value.Z, value.W);
    }

    // Picks between color-tint, sprite-swap, and "no automatic
    // transition" for this widget. Default ColorTint preserves the
    // original mouse-only behaviour. See UITransitionMode for details.
    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.Button_GetTransitionMode(RequireComponent<Button>());
        set => InternalCalls.Button_SetTransitionMode(RequireComponent<Button>(), (int)value);
    }

    // Per-state texture overrides used when TransitionMode is
    // SpriteSwap. null leaves the slot unset; unset slots fall back
    // to NormalSprite. NormalSprite being null disables the swap.
    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Button_GetNormalSprite(RequireComponent<Button>()));
        set => InternalCalls.Button_SetNormalSprite(RequireComponent<Button>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Button_GetHoveredSprite(RequireComponent<Button>()));
        set => InternalCalls.Button_SetHoveredSprite(RequireComponent<Button>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Button_GetPressedSprite(RequireComponent<Button>()));
        set => InternalCalls.Button_SetPressedSprite(RequireComponent<Button>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Button_GetDisabledSprite(RequireComponent<Button>()));
        set => InternalCalls.Button_SetDisabledSprite(RequireComponent<Button>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Button_GetFocusedSprite(RequireComponent<Button>()));
        set => InternalCalls.Button_SetFocusedSprite(RequireComponent<Button>(), value?.UUID ?? 0);
    }

    // Convenience: returns true on the frame the user clicked the
    // button. Reads the sibling Interactable component's IsClicked
    // flag, so the host entity must have one (the Button preset adds
    // it automatically).
    public bool WasClicked
    {
        get
        {
            Interactable? interactable = Entity.GetComponent<Interactable>();
            return interactable?.IsClicked ?? false;
        }
    }

    internal void RaiseClick() => OnClicked?.Invoke(this);
    internal void RaiseClickDown() => OnClickDown?.Invoke(this);
    internal void RaiseClickUp() => OnClickUp?.Invoke(this);
    internal void RaisePressed() => OnPressed?.Invoke(this);
}

// ── Slider ──────────────────────────────────────────────────────────

public class Slider : Component
{
    // Fires when the engine commits a value change this frame (drag /
    // keyboard / SetValue with notify), passing the new value. Subscribe
    // per-instance:
    //   slider.OnValueChanged += v => Debug.Log($"Volume = {v}");
    public event Action<float>? OnValueChanged;


    public float Value
    {
        get => InternalCalls.Slider_GetValue(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetValue(RequireComponent<Slider>(), value);
    }

    public float MinValue
    {
        get => InternalCalls.Slider_GetMinValue(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetMinValue(RequireComponent<Slider>(), value);
    }

    public float MaxValue
    {
        get => InternalCalls.Slider_GetMaxValue(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetMaxValue(RequireComponent<Slider>(), value);
    }

    public bool WholeNumbers
    {
        get => InternalCalls.Slider_GetWholeNumbers(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetWholeNumbers(RequireComponent<Slider>(), value);
    }

    // When true the slider's Value can't be changed by user drag /
    // keyboard / controller input. Programmatic Value writes still
    // work; visual state tracks normally.
    public bool IsReadOnly
    {
        get => InternalCalls.Slider_GetIsReadOnly(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetIsReadOnly(RequireComponent<Slider>(), value);
    }

    // Static track background entity. The slider system never mutates
    // it, but exposing the ref lets script tweak / theme it without
    // re-finding the child by name. UIEventSystem auto-resolves a
    // child named "Background" into this slot when it's null.
    public Entity? BackgroundEntity
    {
        get
        {
            ulong id = InternalCalls.Slider_GetBackgroundEntity(RequireComponent<Slider>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Slider_SetBackgroundEntity(RequireComponent<Slider>(), value?.ID ?? 0);
    }

    // Optional fill rect: child Image whose width grows with Value.
    public Entity? FillEntity
    {
        get
        {
            ulong id = InternalCalls.Slider_GetFillEntity(RequireComponent<Slider>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Slider_SetFillEntity(RequireComponent<Slider>(), value?.ID ?? 0);
    }

    // Optional handle (thumb) entity. Drag interactable usually lives
    // on the handle in the preset; the slider parent is the fallback.
    public Entity? HandleEntity
    {
        get
        {
            ulong id = InternalCalls.Slider_GetHandleEntity(RequireComponent<Slider>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Slider_SetHandleEntity(RequireComponent<Slider>(), value?.ID ?? 0);
    }

    // Programmatic value set with optional event suppression. Two
    // jobs in one call:
    //  - notifyEvent=true: fire OnValueChanged synchronously with the
    //    new value, the same way a same-frame consumer would expect.
    //  - notifyEvent=false: stay silent for this assignment.
    // Both branches call Slider_MarkValueObserved so UIEventSystem's
    // diff (which now catches inspector / property-setter changes too)
    // doesn't double-fire on the next tick.
    public void SetValue(float value, bool notifyEvent = true)
    {
        ulong id = RequireComponent<Slider>();
        float previous = InternalCalls.Slider_GetValue(id);
        InternalCalls.Slider_SetValue(id, value);
        InternalCalls.Slider_MarkValueObserved(id);
        if (notifyEvent)
        {
            float applied = InternalCalls.Slider_GetValue(id);
            if (applied != previous)
                OnValueChanged?.Invoke(applied);
        }
    }

    // Normalized [0, 1] value — convenient for piping into colors,
    // alphas, audio volumes, etc. without re-deriving from MinValue/MaxValue.
    public float NormalizedValue
    {
        get
        {
            float min = MinValue;
            float max = MaxValue;
            float range = max - min;
            return range != 0.0f ? (Value - min) / range : 0.0f;
        }
    }

    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<Slider>();
            InternalCalls.Slider_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Slider_SetNormalColor(RequireComponent<Slider>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<Slider>();
            InternalCalls.Slider_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Slider_SetHoveredColor(RequireComponent<Slider>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<Slider>();
            InternalCalls.Slider_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Slider_SetPressedColor(RequireComponent<Slider>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<Slider>();
            InternalCalls.Slider_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Slider_SetDisabledColor(RequireComponent<Slider>(), value.X, value.Y, value.Z, value.W);
    }

    // Alpha == 0 sentinel = no focus tint; see Button.FocusedColor.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<Slider>();
            InternalCalls.Slider_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Slider_SetFocusedColor(RequireComponent<Slider>(), value.X, value.Y, value.Z, value.W);
    }

    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.Slider_GetTransitionMode(RequireComponent<Slider>());
        set => InternalCalls.Slider_SetTransitionMode(RequireComponent<Slider>(), (int)value);
    }
    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Slider_GetNormalSprite(RequireComponent<Slider>()));
        set => InternalCalls.Slider_SetNormalSprite(RequireComponent<Slider>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Slider_GetHoveredSprite(RequireComponent<Slider>()));
        set => InternalCalls.Slider_SetHoveredSprite(RequireComponent<Slider>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Slider_GetPressedSprite(RequireComponent<Slider>()));
        set => InternalCalls.Slider_SetPressedSprite(RequireComponent<Slider>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Slider_GetDisabledSprite(RequireComponent<Slider>()));
        set => InternalCalls.Slider_SetDisabledSprite(RequireComponent<Slider>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Slider_GetFocusedSprite(RequireComponent<Slider>()));
        set => InternalCalls.Slider_SetFocusedSprite(RequireComponent<Slider>(), value?.UUID ?? 0);
    }

    internal void RaiseValueChanged() => OnValueChanged?.Invoke(Value);
}

// ── Toggle ──────────────────────────────────────────────────────────

public class Toggle : Component
{
    // Fires when IsOn flips this frame, passing the new state.
    // Subscribe per-instance:
    //   toggle.OnValueChanged += isOn => audio.Mute = isOn;
    public event Action<bool>? OnValueChanged;

    public bool IsOn
    {
        get => InternalCalls.Toggle_GetIsOn(RequireComponent<Toggle>());
        set => InternalCalls.Toggle_SetIsOn(RequireComponent<Toggle>(), value);
    }

    // When true the toggle ignores user clicks (mouse + keyboard
    // activate). Programmatic IsOn writes still work; visual state
    // tracks normally so the widget feels alive.
    public bool IsReadOnly
    {
        get => InternalCalls.Toggle_GetIsReadOnly(RequireComponent<Toggle>());
        set => InternalCalls.Toggle_SetIsReadOnly(RequireComponent<Toggle>(), value);
    }

    // The entity whose ImageComponent is shown / hidden as IsOn flips.
    // null clears the link. Writes resolve via the persistent UUID so
    // refs survive scene reload.
    public Entity? CheckmarkEntity
    {
        get
        {
            ulong id = InternalCalls.Toggle_GetCheckmarkEntity(RequireComponent<Toggle>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Toggle_SetCheckmarkEntity(RequireComponent<Toggle>(), value?.ID ?? 0);
    }

    // Programmatic toggle with optional event suppression. See Slider.SetValue.
    public void SetValue(bool value, bool notifyEvent = true)
    {
        ulong id = RequireComponent<Toggle>();
        bool previous = InternalCalls.Toggle_GetIsOn(id);
        InternalCalls.Toggle_SetIsOn(id, value);
        InternalCalls.Toggle_MarkIsOnObserved(id);
        if (notifyEvent && previous != value)
            OnValueChanged?.Invoke(value);
    }

    public bool ValueChangedThisFrame => InternalCalls.Toggle_GetValueChangedThisFrame(RequireComponent<Toggle>());

    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<Toggle>();
            InternalCalls.Toggle_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Toggle_SetNormalColor(RequireComponent<Toggle>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<Toggle>();
            InternalCalls.Toggle_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Toggle_SetHoveredColor(RequireComponent<Toggle>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<Toggle>();
            InternalCalls.Toggle_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Toggle_SetPressedColor(RequireComponent<Toggle>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<Toggle>();
            InternalCalls.Toggle_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Toggle_SetDisabledColor(RequireComponent<Toggle>(), value.X, value.Y, value.Z, value.W);
    }

    // Alpha == 0 sentinel = no focus tint; see Button.FocusedColor.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<Toggle>();
            InternalCalls.Toggle_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Toggle_SetFocusedColor(RequireComponent<Toggle>(), value.X, value.Y, value.Z, value.W);
    }

    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.Toggle_GetTransitionMode(RequireComponent<Toggle>());
        set => InternalCalls.Toggle_SetTransitionMode(RequireComponent<Toggle>(), (int)value);
    }
    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Toggle_GetNormalSprite(RequireComponent<Toggle>()));
        set => InternalCalls.Toggle_SetNormalSprite(RequireComponent<Toggle>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Toggle_GetHoveredSprite(RequireComponent<Toggle>()));
        set => InternalCalls.Toggle_SetHoveredSprite(RequireComponent<Toggle>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Toggle_GetPressedSprite(RequireComponent<Toggle>()));
        set => InternalCalls.Toggle_SetPressedSprite(RequireComponent<Toggle>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Toggle_GetDisabledSprite(RequireComponent<Toggle>()));
        set => InternalCalls.Toggle_SetDisabledSprite(RequireComponent<Toggle>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Toggle_GetFocusedSprite(RequireComponent<Toggle>()));
        set => InternalCalls.Toggle_SetFocusedSprite(RequireComponent<Toggle>(), value?.UUID ?? 0);
    }

    internal void RaiseValueChanged() => OnValueChanged?.Invoke(IsOn);
}

// ── InputField ──────────────────────────────────────────────────────

public class InputField : Component
{
    // Fires when the typed text changes (engine-driven keystroke or
    // SetValue with notify), passing the new text. Subscribe per-instance:
    //   field.OnValueChanged += text => searchSystem.Filter = text;
    public event Action<string>? OnValueChanged;

    // Focus gained / lost — pass the field so the handler can read
    // .Text or other state without a closure.
    public event Action<InputField>? OnSelect;
    public event Action<InputField>? OnDeselect;

    public string Text
    {
        get => InternalCalls.InputField_GetText(RequireComponent<InputField>());
        set => InternalCalls.InputField_SetText(RequireComponent<InputField>(), value ?? "");
    }

    public string PlaceholderText
    {
        get => InternalCalls.InputField_GetPlaceholderText(RequireComponent<InputField>());
        set => InternalCalls.InputField_SetPlaceholderText(RequireComponent<InputField>(), value ?? "");
    }

    public bool IsFocused
    {
        get => InternalCalls.InputField_GetIsFocused(RequireComponent<InputField>());
        set => InternalCalls.InputField_SetIsFocused(RequireComponent<InputField>(), value);
    }

    // The child entity whose TextRendererComponent shows the typed
    // text (or PlaceholderText). Auto-resolved by name "Text" when
    // null — set explicitly to point at a different entity.
    public Entity? TextEntity
    {
        get
        {
            ulong id = InternalCalls.InputField_GetTextEntity(RequireComponent<InputField>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.InputField_SetTextEntity(RequireComponent<InputField>(), value?.ID ?? 0);
    }

    // True on the frame the user pressed Enter while the field was
    // focused. Read once and act — clears at the start of next tick.
    public bool SubmittedThisFrame => InternalCalls.InputField_GetSubmittedThisFrame(RequireComponent<InputField>());

    public int CharacterLimit
    {
        get => InternalCalls.InputField_GetCharacterLimit(RequireComponent<InputField>());
        set => InternalCalls.InputField_SetCharacterLimit(RequireComponent<InputField>(), value);
    }

    // Programmatic text set with optional event suppression. The Text
    // setter alone never raises OnValueChanged; use this method when
    // you want script-driven changes to fan out to subscribers. The
    // dispatcher's text-diffing pass also ignores this assignment when
    // notifyEvent is false (the cached "last seen" string is bumped).
    public void SetValue(string value, bool notifyEvent = true)
    {
        ulong id = RequireComponent<InputField>();
        string previous = InternalCalls.InputField_GetText(id) ?? "";
        string next = value ?? "";
        InternalCalls.InputField_SetText(id, next);
        UIEventDispatcher.MarkInputFieldText(id, next);
        if (notifyEvent && previous != next)
            OnValueChanged?.Invoke(next);
    }

    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<InputField>();
            InternalCalls.InputField_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.InputField_SetNormalColor(RequireComponent<InputField>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<InputField>();
            InternalCalls.InputField_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.InputField_SetHoveredColor(RequireComponent<InputField>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<InputField>();
            InternalCalls.InputField_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.InputField_SetPressedColor(RequireComponent<InputField>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<InputField>();
            InternalCalls.InputField_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.InputField_SetDisabledColor(RequireComponent<InputField>(), value.X, value.Y, value.Z, value.W);
    }

    // Alpha == 0 sentinel = no focus tint; see Button.FocusedColor.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<InputField>();
            InternalCalls.InputField_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.InputField_SetFocusedColor(RequireComponent<InputField>(), value.X, value.Y, value.Z, value.W);
    }

    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.InputField_GetTransitionMode(RequireComponent<InputField>());
        set => InternalCalls.InputField_SetTransitionMode(RequireComponent<InputField>(), (int)value);
    }
    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.InputField_GetNormalSprite(RequireComponent<InputField>()));
        set => InternalCalls.InputField_SetNormalSprite(RequireComponent<InputField>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.InputField_GetHoveredSprite(RequireComponent<InputField>()));
        set => InternalCalls.InputField_SetHoveredSprite(RequireComponent<InputField>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.InputField_GetPressedSprite(RequireComponent<InputField>()));
        set => InternalCalls.InputField_SetPressedSprite(RequireComponent<InputField>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.InputField_GetDisabledSprite(RequireComponent<InputField>()));
        set => InternalCalls.InputField_SetDisabledSprite(RequireComponent<InputField>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.InputField_GetFocusedSprite(RequireComponent<InputField>()));
        set => InternalCalls.InputField_SetFocusedSprite(RequireComponent<InputField>(), value?.UUID ?? 0);
    }

    internal void RaiseValueChanged(string text) => OnValueChanged?.Invoke(text);
    internal void RaiseSelect() => OnSelect?.Invoke(this);
    internal void RaiseDeselect() => OnDeselect?.Invoke(this);
}

// ── Dropdown ────────────────────────────────────────────────────────

public class Dropdown : Component
{
    // Fires when the user (or a script with notifyEvent) picks a
    // different option this frame. Passes the new index so handlers
    // don't have to re-read the property.
    //   dropdown.OnSelectedIndexChange += i => qualityPreset = i;
    public event Action<int>? OnSelectedIndexChange;

    public int SelectedIndex
    {
        get => InternalCalls.Dropdown_GetSelectedIndex(RequireComponent<Dropdown>());
        set => InternalCalls.Dropdown_SetSelectedIndex(RequireComponent<Dropdown>(), value);
    }

    public bool IsOpen
    {
        get => InternalCalls.Dropdown_GetIsOpen(RequireComponent<Dropdown>());
        set => InternalCalls.Dropdown_SetIsOpen(RequireComponent<Dropdown>(), value);
    }

    // When true the dropdown can't be opened or have its selection
    // changed by user input. Programmatic IsOpen / SelectedIndex /
    // option mutation still work.
    public bool IsReadOnly
    {
        get => InternalCalls.Dropdown_GetIsReadOnly(RequireComponent<Dropdown>());
        set => InternalCalls.Dropdown_SetIsReadOnly(RequireComponent<Dropdown>(), value);
    }

    // Optional child entity whose TextRendererComponent shows the
    // currently-selected option's text on the closed header cell.
    public Entity? LabelEntity
    {
        get
        {
            ulong id = InternalCalls.Dropdown_GetLabelEntity(RequireComponent<Dropdown>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Dropdown_SetLabelEntity(RequireComponent<Dropdown>(), value?.ID ?? 0);
    }

    // Per-option-row tints for the popup. Alpha == 0 = "no override":
    // unset states fall through to the next-lower precedence and
    // ultimately to PopupBackgroundColor. Precedence:
    // pressed > hovered > selected > normal.
    public Vector4 OptionNormalColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetOptionNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetOptionNormalColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }
    public Vector4 OptionHoverColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetOptionHoverColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetOptionHoverColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }
    public Vector4 OptionPressedColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetOptionPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetOptionPressedColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }
    public Vector4 OptionSelectedColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetOptionSelectedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetOptionSelectedColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    // The container background colour drawn behind every popup option
    // row. Acts as the implicit fallback when OptionNormalColor is
    // unset (alpha == 0).
    public Vector4 PopupBackgroundColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetPopupBackgroundColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetPopupBackgroundColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    // Tint applied to each popup option's label text. Independent of
    // the per-state row tints — it only stains the glyphs.
    public Vector4 OptionTextColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetOptionTextColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetOptionTextColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    public bool SelectionChangedThisFrame => InternalCalls.Dropdown_GetSelectionChangedThisFrame(RequireComponent<Dropdown>());

    public int OptionCount => InternalCalls.Dropdown_GetOptionCount(RequireComponent<Dropdown>());

    // Currently-selected option text. Empty when no options exist.
    public string SelectedOption
    {
        get
        {
            int idx = SelectedIndex;
            int count = OptionCount;
            if (count <= 0) return "";
            if (idx < 0) idx = 0;
            if (idx >= count) idx = count - 1;
            return InternalCalls.Dropdown_GetOption(RequireComponent<Dropdown>(), idx);
        }
    }

    public string GetOption(int index)
    {
        return InternalCalls.Dropdown_GetOption(RequireComponent<Dropdown>(), index);
    }

    public void SetOption(int index, string text, bool ignoreEvent = true)
    {
        InternalCalls.Dropdown_SetOption(RequireComponent<Dropdown>(), index, text ?? "");
    }

    public void AddOption(string text)
    {
        InternalCalls.Dropdown_AddOption(RequireComponent<Dropdown>(), text ?? "");
    }

    public void RemoveOption(int index)
    {
        InternalCalls.Dropdown_RemoveOption(RequireComponent<Dropdown>(), index);
    }

    public void ClearOptions()
    {
        InternalCalls.Dropdown_ClearOptions(RequireComponent<Dropdown>());
    }

    // Convenience: replace the entire option list in one call.
    public void SetOptions(params string[] options)
    {
        ClearOptions();
        if (options == null) return;
        foreach (string opt in options)
        {
            AddOption(opt ?? "");
        }
    }

    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetNormalColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetHoveredColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetPressedColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetDisabledColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    // Alpha == 0 sentinel = no focus tint; see Button.FocusedColor.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<Dropdown>();
            InternalCalls.Dropdown_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Dropdown_SetFocusedColor(RequireComponent<Dropdown>(), value.X, value.Y, value.Z, value.W);
    }

    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.Dropdown_GetTransitionMode(RequireComponent<Dropdown>());
        set => InternalCalls.Dropdown_SetTransitionMode(RequireComponent<Dropdown>(), (int)value);
    }
    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Dropdown_GetNormalSprite(RequireComponent<Dropdown>()));
        set => InternalCalls.Dropdown_SetNormalSprite(RequireComponent<Dropdown>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Dropdown_GetHoveredSprite(RequireComponent<Dropdown>()));
        set => InternalCalls.Dropdown_SetHoveredSprite(RequireComponent<Dropdown>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Dropdown_GetPressedSprite(RequireComponent<Dropdown>()));
        set => InternalCalls.Dropdown_SetPressedSprite(RequireComponent<Dropdown>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Dropdown_GetDisabledSprite(RequireComponent<Dropdown>()));
        set => InternalCalls.Dropdown_SetDisabledSprite(RequireComponent<Dropdown>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Dropdown_GetFocusedSprite(RequireComponent<Dropdown>()));
        set => InternalCalls.Dropdown_SetFocusedSprite(RequireComponent<Dropdown>(), value?.UUID ?? 0);
    }

    internal void RaiseSelectedIndexChange() => OnSelectedIndexChange?.Invoke(SelectedIndex);
}

// ── Scrollbar ───────────────────────────────────────────────────────
//
// Draggable handle inside a track that produces a normalised [0, 1]
// Value. Mirrors Unity's Scrollbar — see
// Index-Engine/src/Components/UI/ScrollbarComponent.hpp for the full
// state-machine notes (page-clicks, snapping, drag thresholds).

public class Scrollbar : Component
{
    // Fires when Value changes this frame (drag, page-click, SetValue
    // with notify). Passes the new normalized value.
    public event Action<float>? OnValueChanged;

    public float Value
    {
        get => InternalCalls.Scrollbar_GetValue(RequireComponent<Scrollbar>());
        set => InternalCalls.Scrollbar_SetValue(RequireComponent<Scrollbar>(), value);
    }

    // Handle's length as a fraction of the track in [0, 1]. Larger
    // values give a chunkier handle; pair with a ScrollRect to track
    // viewport / content ratio automatically.
    public float Size
    {
        get => InternalCalls.Scrollbar_GetSize(RequireComponent<Scrollbar>());
        set => InternalCalls.Scrollbar_SetSize(RequireComponent<Scrollbar>(), value);
    }

    // 0 = smooth drag; > 1 snaps Value to (NumberOfSteps - 1)
    // equal divisions across the track. Set to (rowCount + 1) for a
    // scrollbar that snaps row-by-row through a list.
    public int NumberOfSteps
    {
        get => InternalCalls.Scrollbar_GetNumberOfSteps(RequireComponent<Scrollbar>());
        set => InternalCalls.Scrollbar_SetNumberOfSteps(RequireComponent<Scrollbar>(), value);
    }

    public ScrollbarDirection Direction
    {
        get => (ScrollbarDirection)InternalCalls.Scrollbar_GetDirection(RequireComponent<Scrollbar>());
        set => InternalCalls.Scrollbar_SetDirection(RequireComponent<Scrollbar>(), (int)value);
    }

    public bool IsReadOnly
    {
        get => InternalCalls.Scrollbar_GetIsReadOnly(RequireComponent<Scrollbar>());
        set => InternalCalls.Scrollbar_SetIsReadOnly(RequireComponent<Scrollbar>(), value);
    }

    // The child entity whose RectTransform is rewritten each frame to
    // position + size the visual handle. UIEventSystem auto-resolves a
    // child named "Handle" with an ImageComponent when null.
    public Entity? HandleEntity
    {
        get
        {
            ulong id = InternalCalls.Scrollbar_GetHandleEntity(RequireComponent<Scrollbar>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.Scrollbar_SetHandleEntity(RequireComponent<Scrollbar>(), value?.ID ?? 0);
    }

    public bool ValueChangedThisFrame => InternalCalls.Scrollbar_GetValueChangedThisFrame(RequireComponent<Scrollbar>());

    // Programmatic value set with optional event suppression. Mirrors
    // Slider.SetValue — see that method for the rationale.
    public void SetValue(float value, bool notifyEvent = true)
    {
        ulong id = RequireComponent<Scrollbar>();
        float previous = InternalCalls.Scrollbar_GetValue(id);
        InternalCalls.Scrollbar_SetValue(id, value);
        InternalCalls.Scrollbar_MarkValueObserved(id);
        if (notifyEvent)
        {
            float applied = InternalCalls.Scrollbar_GetValue(id);
            if (applied != previous)
                OnValueChanged?.Invoke(applied);
        }
    }

    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<Scrollbar>();
            InternalCalls.Scrollbar_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Scrollbar_SetNormalColor(RequireComponent<Scrollbar>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<Scrollbar>();
            InternalCalls.Scrollbar_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Scrollbar_SetHoveredColor(RequireComponent<Scrollbar>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<Scrollbar>();
            InternalCalls.Scrollbar_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Scrollbar_SetPressedColor(RequireComponent<Scrollbar>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<Scrollbar>();
            InternalCalls.Scrollbar_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Scrollbar_SetDisabledColor(RequireComponent<Scrollbar>(), value.X, value.Y, value.Z, value.W);
    }

    // Alpha == 0 sentinel = no focus tint; see Button.FocusedColor.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<Scrollbar>();
            InternalCalls.Scrollbar_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.Scrollbar_SetFocusedColor(RequireComponent<Scrollbar>(), value.X, value.Y, value.Z, value.W);
    }

    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.Scrollbar_GetTransitionMode(RequireComponent<Scrollbar>());
        set => InternalCalls.Scrollbar_SetTransitionMode(RequireComponent<Scrollbar>(), (int)value);
    }

    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Scrollbar_GetNormalSprite(RequireComponent<Scrollbar>()));
        set => InternalCalls.Scrollbar_SetNormalSprite(RequireComponent<Scrollbar>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Scrollbar_GetHoveredSprite(RequireComponent<Scrollbar>()));
        set => InternalCalls.Scrollbar_SetHoveredSprite(RequireComponent<Scrollbar>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Scrollbar_GetPressedSprite(RequireComponent<Scrollbar>()));
        set => InternalCalls.Scrollbar_SetPressedSprite(RequireComponent<Scrollbar>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Scrollbar_GetDisabledSprite(RequireComponent<Scrollbar>()));
        set => InternalCalls.Scrollbar_SetDisabledSprite(RequireComponent<Scrollbar>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.Scrollbar_GetFocusedSprite(RequireComponent<Scrollbar>()));
        set => InternalCalls.Scrollbar_SetFocusedSprite(RequireComponent<Scrollbar>(), value?.UUID ?? 0);
    }

    internal void RaiseValueChanged() => OnValueChanged?.Invoke(Value);
}

// ── ScrollRect ──────────────────────────────────────────────────────
//
// Clipping viewport that scrolls a Content rect to reveal off-screen
// children. Mirrors Unity's ScrollRect — see
// Index-Engine/src/Components/UI/ScrollRectComponent.hpp for the drag /
// inertia / elastic-rebound semantics. Pair with a Mask on the
// Viewport to clip overflow.

public class ScrollRect : Component
{
    // Fires when NormalizedPosition changes this frame (drag, wheel,
    // scrollbar drag, programmatic SetNormalizedPosition with notify).
    // Passes the new position so handlers don't have to re-read.
    public event Action<Vector2>? OnValueChanged;

    public Entity? Content
    {
        get
        {
            ulong id = InternalCalls.ScrollRect_GetContent(RequireComponent<ScrollRect>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.ScrollRect_SetContent(RequireComponent<ScrollRect>(), value?.ID ?? 0);
    }

    // Optional explicit viewport rect (defaults to the ScrollRect entity
    // itself when unset). The viewport is the clipping region; usually
    // hosts a Mask so off-viewport content doesn't bleed out.
    public Entity? Viewport
    {
        get
        {
            ulong id = InternalCalls.ScrollRect_GetViewport(RequireComponent<ScrollRect>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.ScrollRect_SetViewport(RequireComponent<ScrollRect>(), value?.ID ?? 0);
    }

    public bool Horizontal
    {
        get => InternalCalls.ScrollRect_GetHorizontal(RequireComponent<ScrollRect>());
        set => InternalCalls.ScrollRect_SetHorizontal(RequireComponent<ScrollRect>(), value);
    }

    public bool Vertical
    {
        get => InternalCalls.ScrollRect_GetVertical(RequireComponent<ScrollRect>());
        set => InternalCalls.ScrollRect_SetVertical(RequireComponent<ScrollRect>(), value);
    }

    public ScrollRectMovementType MovementType
    {
        get => (ScrollRectMovementType)InternalCalls.ScrollRect_GetMovementType(RequireComponent<ScrollRect>());
        set => InternalCalls.ScrollRect_SetMovementType(RequireComponent<ScrollRect>(), (int)value);
    }

    // Rubber-band rebound rate when MovementType==Elastic. Smaller =
    // springier; larger = stiffer. Default 0.1.
    public float Elasticity
    {
        get => InternalCalls.ScrollRect_GetElasticity(RequireComponent<ScrollRect>());
        set => InternalCalls.ScrollRect_SetElasticity(RequireComponent<ScrollRect>(), value);
    }

    public bool Inertia
    {
        get => InternalCalls.ScrollRect_GetInertia(RequireComponent<ScrollRect>());
        set => InternalCalls.ScrollRect_SetInertia(RequireComponent<ScrollRect>(), value);
    }

    // Per-second friction applied to inertia drift. 0 = velocity decays
    // immediately; 1 = no decay. Default 0.135 ≈ Unity's default.
    public float DecelerationRate
    {
        get => InternalCalls.ScrollRect_GetDecelerationRate(RequireComponent<ScrollRect>());
        set => InternalCalls.ScrollRect_SetDecelerationRate(RequireComponent<ScrollRect>(), value);
    }

    // Wheel scroll multiplier in inspector-friendly units (5 = default
    // speed, 10 = double, 1 = ⅕). See the native struct for the
    // /100 scaling rationale.
    public float ScrollSensitivity
    {
        get => InternalCalls.ScrollRect_GetScrollSensitivity(RequireComponent<ScrollRect>());
        set => InternalCalls.ScrollRect_SetScrollSensitivity(RequireComponent<ScrollRect>(), value);
    }

    public Entity? HorizontalScrollbar
    {
        get
        {
            ulong id = InternalCalls.ScrollRect_GetHorizontalScrollbar(RequireComponent<ScrollRect>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.ScrollRect_SetHorizontalScrollbar(RequireComponent<ScrollRect>(), value?.ID ?? 0);
    }

    public Entity? VerticalScrollbar
    {
        get
        {
            ulong id = InternalCalls.ScrollRect_GetVerticalScrollbar(RequireComponent<ScrollRect>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.ScrollRect_SetVerticalScrollbar(RequireComponent<ScrollRect>(), value?.ID ?? 0);
    }

    public ScrollbarVisibility HorizontalScrollbarVisibility
    {
        get => (ScrollbarVisibility)InternalCalls.ScrollRect_GetHorizontalScrollbarVisibility(RequireComponent<ScrollRect>());
        set => InternalCalls.ScrollRect_SetHorizontalScrollbarVisibility(RequireComponent<ScrollRect>(), (int)value);
    }

    public ScrollbarVisibility VerticalScrollbarVisibility
    {
        get => (ScrollbarVisibility)InternalCalls.ScrollRect_GetVerticalScrollbarVisibility(RequireComponent<ScrollRect>());
        set => InternalCalls.ScrollRect_SetVerticalScrollbarVisibility(RequireComponent<ScrollRect>(), (int)value);
    }

    public float HorizontalScrollbarSpacing
    {
        get => InternalCalls.ScrollRect_GetHorizontalScrollbarSpacing(RequireComponent<ScrollRect>());
        set => InternalCalls.ScrollRect_SetHorizontalScrollbarSpacing(RequireComponent<ScrollRect>(), value);
    }

    public float VerticalScrollbarSpacing
    {
        get => InternalCalls.ScrollRect_GetVerticalScrollbarSpacing(RequireComponent<ScrollRect>());
        set => InternalCalls.ScrollRect_SetVerticalScrollbarSpacing(RequireComponent<ScrollRect>(), value);
    }

    // Current normalized scroll position. X = horizontal in [0..1]
    // (0 = left, 1 = right); Y = vertical in [0..1] (0 = bottom,
    // 1 = top). Outside [0,1] is allowed during Elastic / Unrestricted
    // drag — handlers wanting the resolved bounds-checked value should
    // wait for ValueChangedThisFrame.
    public Vector2 NormalizedPosition
    {
        get
        {
            ulong id = RequireComponent<ScrollRect>();
            InternalCalls.ScrollRect_GetNormalizedPosition(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.ScrollRect_SetNormalizedPosition(RequireComponent<ScrollRect>(), value.X, value.Y);
    }

    public bool ValueChangedThisFrame => InternalCalls.ScrollRect_GetValueChangedThisFrame(RequireComponent<ScrollRect>());

    // Programmatic position set with optional event suppression. Same
    // pattern as Slider.SetValue.
    public void SetNormalizedPosition(Vector2 value, bool notifyEvent = true)
    {
        ulong id = RequireComponent<ScrollRect>();
        InternalCalls.ScrollRect_GetNormalizedPosition(id, out float px, out float py);
        InternalCalls.ScrollRect_SetNormalizedPosition(id, value.X, value.Y);
        InternalCalls.ScrollRect_MarkValueObserved(id);
        if (notifyEvent)
        {
            InternalCalls.ScrollRect_GetNormalizedPosition(id, out float ax, out float ay);
            if (ax != px || ay != py)
                OnValueChanged?.Invoke(new Vector2(ax, ay));
        }
    }

    internal void RaiseValueChanged() => OnValueChanged?.Invoke(NormalizedPosition);
}

// ── Mask ────────────────────────────────────────────────────────────
//
// Clips descendant rendering to the entity's resolved rect. See
// Index-Engine/src/Components/UI/MaskComponent.hpp for the nesting and
// scissor-rect details.

public class Mask : Component
{
    // When true the mask entity's own ImageComponent (if any) still
    // renders normally. When false, the entity's image is suppressed at
    // draw time and only the clipping effect remains — useful for an
    // invisible viewport clipper.
    public bool ShowMaskGraphic
    {
        get => InternalCalls.Mask_GetShowMaskGraphic(RequireComponent<Mask>());
        set => InternalCalls.Mask_SetShowMaskGraphic(RequireComponent<Mask>(), value);
    }
}

// ── CircularSlider ──────────────────────────────────────────────────
//
// Ring-shaped value control. Same value semantics as Slider but the
// drag math is polar and rendering is a pair of arcs. See
// Index-Engine/src/Components/UI/CircularSliderComponent.hpp for the
// geometry / hit-test notes.

public class CircularSlider : Component
{
    // Fires when Value changes this frame (drag, programmatic
    // SetValue with notify). Passes the new value.
    public event Action<float>? OnValueChanged;

    public float Value
    {
        get => InternalCalls.CircularSlider_GetValue(RequireComponent<CircularSlider>());
        set => InternalCalls.CircularSlider_SetValue(RequireComponent<CircularSlider>(), value);
    }

    public float MinValue
    {
        get => InternalCalls.CircularSlider_GetMinValue(RequireComponent<CircularSlider>());
        set => InternalCalls.CircularSlider_SetMinValue(RequireComponent<CircularSlider>(), value);
    }

    public float MaxValue
    {
        get => InternalCalls.CircularSlider_GetMaxValue(RequireComponent<CircularSlider>());
        set => InternalCalls.CircularSlider_SetMaxValue(RequireComponent<CircularSlider>(), value);
    }

    public bool WholeNumbers
    {
        get => InternalCalls.CircularSlider_GetWholeNumbers(RequireComponent<CircularSlider>());
        set => InternalCalls.CircularSlider_SetWholeNumbers(RequireComponent<CircularSlider>(), value);
    }

    public bool IsReadOnly
    {
        get => InternalCalls.CircularSlider_GetIsReadOnly(RequireComponent<CircularSlider>());
        set => InternalCalls.CircularSlider_SetIsReadOnly(RequireComponent<CircularSlider>(), value);
    }

    // Standard math convention: 0° = +X right, 90° = +Y up, etc.
    // Default 90° puts Value=Min at 12 o'clock.
    public float StartAngleDegrees
    {
        get => InternalCalls.CircularSlider_GetStartAngleDegrees(RequireComponent<CircularSlider>());
        set => InternalCalls.CircularSlider_SetStartAngleDegrees(RequireComponent<CircularSlider>(), value);
    }

    // How much of the ring the slider covers. 360 = full ring,
    // 270 = quarter gap ("C" shape), etc.
    public float SweepDegrees
    {
        get => InternalCalls.CircularSlider_GetSweepDegrees(RequireComponent<CircularSlider>());
        set => InternalCalls.CircularSlider_SetSweepDegrees(RequireComponent<CircularSlider>(), value);
    }

    public bool Clockwise
    {
        get => InternalCalls.CircularSlider_GetClockwise(RequireComponent<CircularSlider>());
        set => InternalCalls.CircularSlider_SetClockwise(RequireComponent<CircularSlider>(), value);
    }

    public float RingThickness
    {
        get => InternalCalls.CircularSlider_GetRingThickness(RequireComponent<CircularSlider>());
        set => InternalCalls.CircularSlider_SetRingThickness(RequireComponent<CircularSlider>(), value);
    }

    // Approximation density of the rendered arc. 64 is plenty for a
    // 200 px ring; bump it if you scale up.
    public int RingSegments
    {
        get => InternalCalls.CircularSlider_GetRingSegments(RequireComponent<CircularSlider>());
        set => InternalCalls.CircularSlider_SetRingSegments(RequireComponent<CircularSlider>(), value);
    }

    public Vector4 BackgroundColor
    {
        get
        {
            ulong id = RequireComponent<CircularSlider>();
            InternalCalls.CircularSlider_GetBackgroundColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.CircularSlider_SetBackgroundColor(RequireComponent<CircularSlider>(), value.X, value.Y, value.Z, value.W);
    }

    public Vector4 FillColor
    {
        get
        {
            ulong id = RequireComponent<CircularSlider>();
            InternalCalls.CircularSlider_GetFillColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.CircularSlider_SetFillColor(RequireComponent<CircularSlider>(), value.X, value.Y, value.Z, value.W);
    }

    // Optional child whose RectTransform is rewritten each frame to sit
    // on the ring at the current Value angle.
    public Entity? HandleEntity
    {
        get
        {
            ulong id = InternalCalls.CircularSlider_GetHandleEntity(RequireComponent<CircularSlider>());
            return id == 0 ? null : new Entity(id);
        }
        set => InternalCalls.CircularSlider_SetHandleEntity(RequireComponent<CircularSlider>(), value?.ID ?? 0);
    }

    public bool ValueChangedThisFrame => InternalCalls.CircularSlider_GetValueChangedThisFrame(RequireComponent<CircularSlider>());

    // Programmatic value set with optional event suppression. Mirrors
    // Slider.SetValue.
    public void SetValue(float value, bool notifyEvent = true)
    {
        ulong id = RequireComponent<CircularSlider>();
        float previous = InternalCalls.CircularSlider_GetValue(id);
        InternalCalls.CircularSlider_SetValue(id, value);
        InternalCalls.CircularSlider_MarkValueObserved(id);
        if (notifyEvent)
        {
            float applied = InternalCalls.CircularSlider_GetValue(id);
            if (applied != previous)
                OnValueChanged?.Invoke(applied);
        }
    }

    // Normalized [0, 1] value — convenient for piping into colors,
    // alphas, audio volumes, etc.
    public float NormalizedValue
    {
        get
        {
            float min = MinValue;
            float max = MaxValue;
            float range = max - min;
            return range != 0.0f ? (Value - min) / range : 0.0f;
        }
    }

    // Per-state handle palette. Mirrors Slider — these tint the
    // HandleEntity's ImageComponent, not the ring itself.
    public Vector4 NormalColor
    {
        get
        {
            ulong id = RequireComponent<CircularSlider>();
            InternalCalls.CircularSlider_GetNormalColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.CircularSlider_SetNormalColor(RequireComponent<CircularSlider>(), value.X, value.Y, value.Z, value.W);
    }
    public Vector4 HoveredColor
    {
        get
        {
            ulong id = RequireComponent<CircularSlider>();
            InternalCalls.CircularSlider_GetHoveredColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.CircularSlider_SetHoveredColor(RequireComponent<CircularSlider>(), value.X, value.Y, value.Z, value.W);
    }
    public Vector4 PressedColor
    {
        get
        {
            ulong id = RequireComponent<CircularSlider>();
            InternalCalls.CircularSlider_GetPressedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.CircularSlider_SetPressedColor(RequireComponent<CircularSlider>(), value.X, value.Y, value.Z, value.W);
    }
    public Vector4 DisabledColor
    {
        get
        {
            ulong id = RequireComponent<CircularSlider>();
            InternalCalls.CircularSlider_GetDisabledColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.CircularSlider_SetDisabledColor(RequireComponent<CircularSlider>(), value.X, value.Y, value.Z, value.W);
    }
    // Alpha == 0 sentinel = no focus tint; see Button.FocusedColor.
    public Vector4 FocusedColor
    {
        get
        {
            ulong id = RequireComponent<CircularSlider>();
            InternalCalls.CircularSlider_GetFocusedColor(id, out float r, out float g, out float b, out float a);
            return new Color(r, g, b, a);
        }
        set => InternalCalls.CircularSlider_SetFocusedColor(RequireComponent<CircularSlider>(), value.X, value.Y, value.Z, value.W);
    }

    public UITransitionMode TransitionMode
    {
        get => (UITransitionMode)InternalCalls.CircularSlider_GetTransitionMode(RequireComponent<CircularSlider>());
        set => InternalCalls.CircularSlider_SetTransitionMode(RequireComponent<CircularSlider>(), (int)value);
    }

    public Texture? NormalSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.CircularSlider_GetNormalSprite(RequireComponent<CircularSlider>()));
        set => InternalCalls.CircularSlider_SetNormalSprite(RequireComponent<CircularSlider>(), value?.UUID ?? 0);
    }
    public Texture? HoveredSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.CircularSlider_GetHoveredSprite(RequireComponent<CircularSlider>()));
        set => InternalCalls.CircularSlider_SetHoveredSprite(RequireComponent<CircularSlider>(), value?.UUID ?? 0);
    }
    public Texture? PressedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.CircularSlider_GetPressedSprite(RequireComponent<CircularSlider>()));
        set => InternalCalls.CircularSlider_SetPressedSprite(RequireComponent<CircularSlider>(), value?.UUID ?? 0);
    }
    public Texture? DisabledSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.CircularSlider_GetDisabledSprite(RequireComponent<CircularSlider>()));
        set => InternalCalls.CircularSlider_SetDisabledSprite(RequireComponent<CircularSlider>(), value?.UUID ?? 0);
    }
    public Texture? FocusedSprite
    {
        get => Index.Texture.FromAssetUUID(InternalCalls.CircularSlider_GetFocusedSprite(RequireComponent<CircularSlider>()));
        set => InternalCalls.CircularSlider_SetFocusedSprite(RequireComponent<CircularSlider>(), value?.UUID ?? 0);
    }

    internal void RaiseValueChanged() => OnValueChanged?.Invoke(Value);
}

// ── HorizontalLayoutGroup ───────────────────────────────────────────
//
// Lays out direct children left-to-right (or right-to-left) inside the
// parent rect. See
// Index-Engine/src/Components/UI/HorizontalLayoutGroupComponent.hpp
// for the padding / spacing / alignment / expand semantics.

public class HorizontalLayoutGroup : Component
{
    public float PaddingLeft
    {
        get => InternalCalls.HorizontalLayoutGroup_GetPaddingLeft(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetPaddingLeft(RequireComponent<HorizontalLayoutGroup>(), value);
    }
    public float PaddingRight
    {
        get => InternalCalls.HorizontalLayoutGroup_GetPaddingRight(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetPaddingRight(RequireComponent<HorizontalLayoutGroup>(), value);
    }
    public float PaddingTop
    {
        get => InternalCalls.HorizontalLayoutGroup_GetPaddingTop(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetPaddingTop(RequireComponent<HorizontalLayoutGroup>(), value);
    }
    public float PaddingBottom
    {
        get => InternalCalls.HorizontalLayoutGroup_GetPaddingBottom(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetPaddingBottom(RequireComponent<HorizontalLayoutGroup>(), value);
    }

    public float Spacing
    {
        get => InternalCalls.HorizontalLayoutGroup_GetSpacing(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetSpacing(RequireComponent<HorizontalLayoutGroup>(), value);
    }

    public UIAlignment ChildAlignment
    {
        get => (UIAlignment)InternalCalls.HorizontalLayoutGroup_GetChildAlignment(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetChildAlignment(RequireComponent<HorizontalLayoutGroup>(), (int)value);
    }

    public bool ReverseArrangement
    {
        get => InternalCalls.HorizontalLayoutGroup_GetReverseArrangement(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetReverseArrangement(RequireComponent<HorizontalLayoutGroup>(), value);
    }

    public bool ControlChildWidth
    {
        get => InternalCalls.HorizontalLayoutGroup_GetControlChildWidth(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetControlChildWidth(RequireComponent<HorizontalLayoutGroup>(), value);
    }
    public bool ControlChildHeight
    {
        get => InternalCalls.HorizontalLayoutGroup_GetControlChildHeight(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetControlChildHeight(RequireComponent<HorizontalLayoutGroup>(), value);
    }

    public bool UseChildScaleWidth
    {
        get => InternalCalls.HorizontalLayoutGroup_GetUseChildScaleWidth(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetUseChildScaleWidth(RequireComponent<HorizontalLayoutGroup>(), value);
    }
    public bool UseChildScaleHeight
    {
        get => InternalCalls.HorizontalLayoutGroup_GetUseChildScaleHeight(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetUseChildScaleHeight(RequireComponent<HorizontalLayoutGroup>(), value);
    }

    public bool ChildForceExpandWidth
    {
        get => InternalCalls.HorizontalLayoutGroup_GetChildForceExpandWidth(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetChildForceExpandWidth(RequireComponent<HorizontalLayoutGroup>(), value);
    }
    public bool ChildForceExpandHeight
    {
        get => InternalCalls.HorizontalLayoutGroup_GetChildForceExpandHeight(RequireComponent<HorizontalLayoutGroup>());
        set => InternalCalls.HorizontalLayoutGroup_SetChildForceExpandHeight(RequireComponent<HorizontalLayoutGroup>(), value);
    }
}

// ── VerticalLayoutGroup ─────────────────────────────────────────────
//
// Top-to-bottom (or bottom-to-top) sibling of HorizontalLayoutGroup —
// same field set, dominant axis flipped. See
// Index-Engine/src/Components/UI/VerticalLayoutGroupComponent.hpp.

public class VerticalLayoutGroup : Component
{
    public float PaddingLeft
    {
        get => InternalCalls.VerticalLayoutGroup_GetPaddingLeft(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetPaddingLeft(RequireComponent<VerticalLayoutGroup>(), value);
    }
    public float PaddingRight
    {
        get => InternalCalls.VerticalLayoutGroup_GetPaddingRight(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetPaddingRight(RequireComponent<VerticalLayoutGroup>(), value);
    }
    public float PaddingTop
    {
        get => InternalCalls.VerticalLayoutGroup_GetPaddingTop(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetPaddingTop(RequireComponent<VerticalLayoutGroup>(), value);
    }
    public float PaddingBottom
    {
        get => InternalCalls.VerticalLayoutGroup_GetPaddingBottom(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetPaddingBottom(RequireComponent<VerticalLayoutGroup>(), value);
    }

    public float Spacing
    {
        get => InternalCalls.VerticalLayoutGroup_GetSpacing(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetSpacing(RequireComponent<VerticalLayoutGroup>(), value);
    }

    public UIAlignment ChildAlignment
    {
        get => (UIAlignment)InternalCalls.VerticalLayoutGroup_GetChildAlignment(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetChildAlignment(RequireComponent<VerticalLayoutGroup>(), (int)value);
    }

    public bool ReverseArrangement
    {
        get => InternalCalls.VerticalLayoutGroup_GetReverseArrangement(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetReverseArrangement(RequireComponent<VerticalLayoutGroup>(), value);
    }

    public bool ControlChildWidth
    {
        get => InternalCalls.VerticalLayoutGroup_GetControlChildWidth(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetControlChildWidth(RequireComponent<VerticalLayoutGroup>(), value);
    }
    public bool ControlChildHeight
    {
        get => InternalCalls.VerticalLayoutGroup_GetControlChildHeight(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetControlChildHeight(RequireComponent<VerticalLayoutGroup>(), value);
    }

    public bool UseChildScaleWidth
    {
        get => InternalCalls.VerticalLayoutGroup_GetUseChildScaleWidth(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetUseChildScaleWidth(RequireComponent<VerticalLayoutGroup>(), value);
    }
    public bool UseChildScaleHeight
    {
        get => InternalCalls.VerticalLayoutGroup_GetUseChildScaleHeight(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetUseChildScaleHeight(RequireComponent<VerticalLayoutGroup>(), value);
    }

    public bool ChildForceExpandWidth
    {
        get => InternalCalls.VerticalLayoutGroup_GetChildForceExpandWidth(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetChildForceExpandWidth(RequireComponent<VerticalLayoutGroup>(), value);
    }
    public bool ChildForceExpandHeight
    {
        get => InternalCalls.VerticalLayoutGroup_GetChildForceExpandHeight(RequireComponent<VerticalLayoutGroup>());
        set => InternalCalls.VerticalLayoutGroup_SetChildForceExpandHeight(RequireComponent<VerticalLayoutGroup>(), value);
    }
}

// ── GridLayoutGroup ─────────────────────────────────────────────────
//
// Lays out direct children in a 2D grid of uniform cells. See
// Index-Engine/src/Components/UI/GridLayoutGroupComponent.hpp.

public class GridLayoutGroup : Component
{
    public float PaddingLeft
    {
        get => InternalCalls.GridLayoutGroup_GetPaddingLeft(RequireComponent<GridLayoutGroup>());
        set => InternalCalls.GridLayoutGroup_SetPaddingLeft(RequireComponent<GridLayoutGroup>(), value);
    }
    public float PaddingRight
    {
        get => InternalCalls.GridLayoutGroup_GetPaddingRight(RequireComponent<GridLayoutGroup>());
        set => InternalCalls.GridLayoutGroup_SetPaddingRight(RequireComponent<GridLayoutGroup>(), value);
    }
    public float PaddingTop
    {
        get => InternalCalls.GridLayoutGroup_GetPaddingTop(RequireComponent<GridLayoutGroup>());
        set => InternalCalls.GridLayoutGroup_SetPaddingTop(RequireComponent<GridLayoutGroup>(), value);
    }
    public float PaddingBottom
    {
        get => InternalCalls.GridLayoutGroup_GetPaddingBottom(RequireComponent<GridLayoutGroup>());
        set => InternalCalls.GridLayoutGroup_SetPaddingBottom(RequireComponent<GridLayoutGroup>(), value);
    }

    // Per-cell pixel size. Applied to every child's SizeDelta.
    public Vector2 CellSize
    {
        get
        {
            ulong id = RequireComponent<GridLayoutGroup>();
            InternalCalls.GridLayoutGroup_GetCellSize(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.GridLayoutGroup_SetCellSize(RequireComponent<GridLayoutGroup>(), value.X, value.Y);
    }

    // Per-axis gap between cells (X = horizontal, Y = vertical).
    public Vector2 Spacing
    {
        get
        {
            ulong id = RequireComponent<GridLayoutGroup>();
            InternalCalls.GridLayoutGroup_GetSpacing(id, out float x, out float y);
            return new Vector2(x, y);
        }
        set => InternalCalls.GridLayoutGroup_SetSpacing(RequireComponent<GridLayoutGroup>(), value.X, value.Y);
    }

    public GridLayoutStartCorner StartCorner
    {
        get => (GridLayoutStartCorner)InternalCalls.GridLayoutGroup_GetStartCorner(RequireComponent<GridLayoutGroup>());
        set => InternalCalls.GridLayoutGroup_SetStartCorner(RequireComponent<GridLayoutGroup>(), (int)value);
    }

    public GridLayoutStartAxis StartAxis
    {
        get => (GridLayoutStartAxis)InternalCalls.GridLayoutGroup_GetStartAxis(RequireComponent<GridLayoutGroup>());
        set => InternalCalls.GridLayoutGroup_SetStartAxis(RequireComponent<GridLayoutGroup>(), (int)value);
    }

    public UIAlignment ChildAlignment
    {
        get => (UIAlignment)InternalCalls.GridLayoutGroup_GetChildAlignment(RequireComponent<GridLayoutGroup>());
        set => InternalCalls.GridLayoutGroup_SetChildAlignment(RequireComponent<GridLayoutGroup>(), (int)value);
    }

    public GridLayoutConstraint Constraint
    {
        get => (GridLayoutConstraint)InternalCalls.GridLayoutGroup_GetConstraint(RequireComponent<GridLayoutGroup>());
        set => InternalCalls.GridLayoutGroup_SetConstraint(RequireComponent<GridLayoutGroup>(), (int)value);
    }

    public int ConstraintCount
    {
        get => InternalCalls.GridLayoutGroup_GetConstraintCount(RequireComponent<GridLayoutGroup>());
        set => InternalCalls.GridLayoutGroup_SetConstraintCount(RequireComponent<GridLayoutGroup>(), value);
    }

    // When true, children map onto cells in reverse hierarchy order
    // (last child → first cell). Cell positions still flow from
    // StartCorner along StartAxis as usual.
    public bool Reverse
    {
        get => InternalCalls.GridLayoutGroup_GetReverse(RequireComponent<GridLayoutGroup>());
        set => InternalCalls.GridLayoutGroup_SetReverse(RequireComponent<GridLayoutGroup>(), value);
    }
}

// ── ContentSizeFitter ───────────────────────────────────────────────<div style='direction:ltr;border-width:100%'>
//
// Auto-resizes the entity's RectTransform along enabled axes to fit the
// bounding box of its direct children. See
// Index-Engine/src/Components/UI/ContentSizeFitterComponent.hpp for the
// per-axis fit semantics and nested-fitter resolution order.

public class ContentSizeFitter : Component
{
    public bool HorizontalFit
    {
        get => InternalCalls.ContentSizeFitter_GetHorizontalFit(RequireComponent<ContentSizeFitter>());
        set => InternalCalls.ContentSizeFitter_SetHorizontalFit(RequireComponent<ContentSizeFitter>(), value);
    }

    public bool VerticalFit
    {
        get => InternalCalls.ContentSizeFitter_GetVerticalFit(RequireComponent<ContentSizeFitter>());
        set => InternalCalls.ContentSizeFitter_SetVerticalFit(RequireComponent<ContentSizeFitter>(), value);
    }

    public float PaddingLeft
    {
        get => InternalCalls.ContentSizeFitter_GetPaddingLeft(RequireComponent<ContentSizeFitter>());
        set => InternalCalls.ContentSizeFitter_SetPaddingLeft(RequireComponent<ContentSizeFitter>(), value);
    }
    public float PaddingRight
    {
        get => InternalCalls.ContentSizeFitter_GetPaddingRight(RequireComponent<ContentSizeFitter>());
        set => InternalCalls.ContentSizeFitter_SetPaddingRight(RequireComponent<ContentSizeFitter>(), value);
    }
    public float PaddingTop
    {
        get => InternalCalls.ContentSizeFitter_GetPaddingTop(RequireComponent<ContentSizeFitter>());
        set => InternalCalls.ContentSizeFitter_SetPaddingTop(RequireComponent<ContentSizeFitter>(), value);
    }
    public float PaddingBottom
    {
        get => InternalCalls.ContentSizeFitter_GetPaddingBottom(RequireComponent<ContentSizeFitter>());
        set => InternalCalls.ContentSizeFitter_SetPaddingBottom(RequireComponent<ContentSizeFitter>(), value);
    }
}

// ── WidthConstraint ─────────────────────────────────────────────────
//
// Clamps the entity's RectTransform.SizeDelta.x to [MinWidth, MaxWidth]
// each frame. A negative bound disables that side of the clamp. See
// Index-Engine/src/Components/UI/WidthConstraintComponent.hpp.

public class WidthConstraint : Component
{
    public float MinWidth
    {
        get => InternalCalls.WidthConstraint_GetMinWidth(RequireComponent<WidthConstraint>());
        set => InternalCalls.WidthConstraint_SetMinWidth(RequireComponent<WidthConstraint>(), value);
    }

    public float MaxWidth
    {
        get => InternalCalls.WidthConstraint_GetMaxWidth(RequireComponent<WidthConstraint>());
        set => InternalCalls.WidthConstraint_SetMaxWidth(RequireComponent<WidthConstraint>(), value);
    }
}
