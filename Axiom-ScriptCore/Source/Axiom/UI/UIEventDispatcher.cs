using System;
using System.Collections.Generic;
using Axiom;
using Axiom.Interop;

namespace Axiom.UI;

// Per-frame UI event dispatcher. Called from native code via
// ScriptInstanceManager.RaiseUiEventDispatch at the tail of
// UIEventSystem::Update — that's the moment the engine has finished
// writing transient flags (IsClicked, IsHovered, ValueChangedThisFrame,
// etc.) for the current frame, so we can read them and fan out to
// per-instance subscribers of the UI events.
//
// Each component instance owns its events (Slider.OnValueChanged etc.
// are no longer static), so the dispatcher resolves the entity ID it
// gets from the engine into a Component via Entity.GetComponent and
// invokes that instance's internal Raise* helper. The component class
// hands a typed payload to subscribers — the new value for value-bearing
// events (Slider→float, Toggle→bool, InputField→string, Dropdown→int)
// or `this` for transition events (Interactable hover/click/focus).
//
// Edge semantics:
//   - IsClicked / IsMouseDown / IsMouseUp are already one-frame edges
//     in UIEventSystem (set true on the frame they happen, cleared
//     next tick), so we fire those directly.
//   - IsHovered / IsPressed are continuous polling flags. We track
//     previous state per entity and fire OnHovered / OnPressed only
//     on the rising edge so handlers don't run every frame the user
//     is hovering or holding the mouse.
//   - InputField has no native "TextChangedThisFrame" flag, so we
//     diff against the last observed text per entity instead. Scripts
//     calling InputField.SetValue(..., notifyEvent: false) push the new
//     text into the cache via MarkInputFieldText to suppress the diff.

internal static class UIEventDispatcher
{
    private static readonly HashSet<ulong> s_PrevHovered = new();
    private static readonly HashSet<ulong> s_PrevPressed = new();
    private static readonly HashSet<ulong> s_PrevFocused = new();
    private static readonly HashSet<ulong> s_PrevFocusedInputFields = new();
    private static readonly Dictionary<ulong, string> s_LastInputFieldText = new();

    // Reusable buffers — UI rarely has thousands of widgets, so a
    // single per-component-type list keeps allocations off the hot path.
    private static ulong[] s_QueryBuffer = new ulong[64];

    internal static void MarkInputFieldText(ulong entityId, string text)
        => s_LastInputFieldText[entityId] = text ?? "";

    public static void Tick()
    {
        try
        {
            DispatchInteractables();
            DispatchButtons();
            DispatchSliders();
            DispatchToggles();
            DispatchDropdowns();
            DispatchInputFields();
        }
        catch (Exception ex)
        {
            Log.Error($"UIEventDispatcher.Tick failed: {ex}");
        }
    }

    private static int Query(string componentName)
    {
        while (true)
        {
            int count = InternalCalls.Scene_QueryEntities(componentName, s_QueryBuffer);
            if (count <= s_QueryBuffer.Length)
                return count < 0 ? 0 : count;

            // Buffer too small — grow and retry. Native side returns the
            // total it would have written when the buffer is undersized.
            s_QueryBuffer = new ulong[count];
        }
    }

    private static void DispatchInteractables()
    {
        int count = Query("Interactable");
        if (count == 0)
        {
            s_PrevHovered.Clear();
            s_PrevPressed.Clear();
            s_PrevFocused.Clear();
            return;
        }

        // We rebuild "currently hovered / pressed / focused" sets from
        // this frame and swap them in afterwards, so the next tick's
        // edge detection sees the right baseline even when entities
        // disappear or change Focusable.
        var nowHovered = new HashSet<ulong>();
        var nowPressed = new HashSet<ulong>();
        var nowFocused = new HashSet<ulong>();

        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];

            // Resolve the managed wrapper once per frame per widget so
            // the subsequent Raise* calls don't repeat the lookup. The
            // Entity wrapper here is throwaway, but Entity.GetComponent
            // routes through s_ManagedComponentStore (shared per
            // entity-ID + type), so we hit the same Interactable
            // instance the user's script subscribed to — its events
            // carry the user's handlers.
            Entity entity = new Entity(id);
            Interactable? interactable = entity.GetComponent<Interactable>();
            if (interactable == null) continue;

            bool hovered = InternalCalls.Interactable_GetIsHovered(id);
            bool pressed = InternalCalls.Interactable_GetIsPressed(id);
            bool clicked = InternalCalls.Interactable_GetIsClicked(id);
            bool mouseDown = InternalCalls.Interactable_GetIsMouseDown(id);
            bool mouseUp = InternalCalls.Interactable_GetIsMouseUp(id);
            bool focused = InternalCalls.Interactable_GetIsFocused(id);

            if (hovered)
            {
                nowHovered.Add(id);
                if (!s_PrevHovered.Contains(id))
                    interactable.RaiseHovered();
            }

            if (pressed)
            {
                nowPressed.Add(id);
                if (!s_PrevPressed.Contains(id))
                    interactable.RaisePressed();
            }

            if (focused)
            {
                nowFocused.Add(id);
                if (!s_PrevFocused.Contains(id))
                    interactable.RaiseFocusEnter();
            }
            else if (s_PrevFocused.Contains(id))
            {
                interactable.RaiseFocusExit();
            }

            if (clicked)
            {
                interactable.RaiseClicked();
                // Button click fan-out moved to DispatchButtons so a
                // Button whose TargetGraphic owns the Interactable (rather
                // than the Button entity itself) still receives OnClick.
                // Same-entity buttons are covered by the same iteration.
            }

            if (mouseDown) interactable.RaiseMouseDown();
            if (mouseUp) interactable.RaiseMouseUp();
        }

        s_PrevHovered.Clear();
        foreach (ulong id in nowHovered) s_PrevHovered.Add(id);
        s_PrevPressed.Clear();
        foreach (ulong id in nowPressed) s_PrevPressed.Add(id);
        s_PrevFocused.Clear();
        foreach (ulong id in nowFocused) s_PrevFocused.Add(id);
    }

    private static void DispatchButtons()
    {
        // Buttons fan out their click event from whichever entity owns
        // the Interactable: the button entity itself OR the configured
        // TargetGraphic. The dispatcher looks up the target's
        // IsClicked flag rather than tracking a button-side edge,
        // because that flag is already a one-frame edge on the
        // Interactable side.
        int count = Query("Button");
        for (int i = 0; i < count; i++)
        {
            ulong buttonId = s_QueryBuffer[i];
            ulong interactableId = 0;
            if (InternalCalls.Entity_HasComponent(buttonId, "Interactable"))
            {
                interactableId = buttonId;
            }
            else
            {
                ulong target = InternalCalls.Button_GetTargetGraphic(buttonId);
                if (target != 0 && InternalCalls.Entity_HasComponent(target, "Interactable"))
                {
                    interactableId = target;
                }
            }
            if (interactableId == 0) continue;
            if (!InternalCalls.Interactable_GetIsClicked(interactableId)) continue;

            Button? button = new Entity(buttonId).GetComponent<Button>();
            button?.RaiseClick();
        }
    }

    private static void DispatchSliders()
    {
        int count = Query("Slider");
        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];
            if (!InternalCalls.Slider_GetValueChangedThisFrame(id)) continue;
            Slider? slider = new Entity(id).GetComponent<Slider>();
            slider?.RaiseValueChanged();
        }
    }

    private static void DispatchToggles()
    {
        int count = Query("Toggle");
        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];
            if (!InternalCalls.Toggle_GetValueChangedThisFrame(id)) continue;
            Toggle? toggle = new Entity(id).GetComponent<Toggle>();
            toggle?.RaiseValueChanged();
        }
    }

    private static void DispatchDropdowns()
    {
        int count = Query("Dropdown");
        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];
            if (!InternalCalls.Dropdown_GetSelectionChangedThisFrame(id)) continue;
            Dropdown? dropdown = new Entity(id).GetComponent<Dropdown>();
            dropdown?.RaiseSelectedIndexChange();
        }
    }

    private static void DispatchInputFields()
    {
        int count = Query("Input Field");
        if (count == 0)
        {
            // Any field that vanished while focused never gets an explicit
            // OnDeselect — that matches the "destroyed entity" semantics
            // already used elsewhere (no zombie events on dead handles).
            s_LastInputFieldText.Clear();
            s_PrevFocusedInputFields.Clear();
            return;
        }

        // Track which entities we saw this frame so we can prune cache
        // entries for destroyed input fields without a separate validity
        // ping per entry.
        var seen = new HashSet<ulong>();
        var nowFocused = new HashSet<ulong>();

        for (int i = 0; i < count; i++)
        {
            ulong id = s_QueryBuffer[i];
            seen.Add(id);

            InputField? field = new Entity(id).GetComponent<InputField>();
            if (field == null) continue;

            // ── Text change ────────────────────────────────────────
            string current = InternalCalls.InputField_GetText(id) ?? "";
            if (s_LastInputFieldText.TryGetValue(id, out string? previous))
            {
                if (previous != current)
                {
                    s_LastInputFieldText[id] = current;
                    field.RaiseValueChanged(current);
                }
            }
            else
            {
                // First time we've seen this field — seed without firing
                // (we have no prior baseline to call this a change against).
                s_LastInputFieldText[id] = current;
            }

            // ── Focus / blur edges ─────────────────────────────────
            // OnSelect on the rising edge of IsFocused, OnDeselect on
            // the falling edge. The continuous flag itself isn't useful
            // as an event; we only want the transitions.
            bool focused = InternalCalls.InputField_GetIsFocused(id);
            bool wasFocused = s_PrevFocusedInputFields.Contains(id);
            if (focused)
            {
                nowFocused.Add(id);
                if (!wasFocused) field.RaiseSelect();
            }
            else if (wasFocused)
            {
                field.RaiseDeselect();
            }
        }

        if (s_LastInputFieldText.Count > seen.Count)
        {
            List<ulong>? stale = null;
            foreach (ulong id in s_LastInputFieldText.Keys)
            {
                if (!seen.Contains(id))
                    (stale ??= new List<ulong>()).Add(id);
            }
            if (stale != null)
                foreach (ulong id in stale)
                    s_LastInputFieldText.Remove(id);
        }

        s_PrevFocusedInputFields.Clear();
        foreach (ulong id in nowFocused) s_PrevFocusedInputFields.Add(id);
    }
}
