using System.Runtime.InteropServices;

namespace Axiom.Components;

// Layout MUST match Axiom-Engine/src/Components/General/Transform2DComponent.hpp
// (the C++ Transform2DComponent class). ScriptHostBridge enforces this at
// script-engine init by calling Entity_GetComponentSize("Transform 2D") and
// comparing against sizeof(Transform2D) — a drift caught early hard-fails the
// user assembly load instead of silently corrupting EnTT storage.
//
// World-space fields (Position/Scale/Rotation) are written by
// TransformHierarchySystem each frame from the Local* values composed against
// the parent's world transform. Scripts that want their writes to stick
// across the next frame's hierarchy pass MUST write the Local* fields — the
// world fields are a derived cache. The C++ side's setters do this; the ref-API
// is more direct, so the convention is "edit LocalPosition, observe Position
// after the propagate pass."
[StructLayout(LayoutKind.Sequential)]
public struct Transform2D : IComponent
{
    public Vector2 Position;
    public Vector2 Scale;
    public float   Rotation;

    public Vector2 LocalPosition;
    public Vector2 LocalScale;
    public float   LocalRotation;

    // C++ `bool m_Dirty` — 1 byte. Native side flips this when SetPosition /
    // SetRotation / SetScale runs; scripts can mark a transform dirty for the
    // next hierarchy pass after writing Local* directly. Trailing padding is
    // 3 bytes (sequential layout, 4-byte natural alignment of the struct).
    [MarshalAs(UnmanagedType.U1)]
    public bool Dirty;

    // Native serialized/display name used by the binding layer to find this
    // component's pool. Kept as a single source of truth so renames don't
    // diverge between IComponent::Get / inspector / scene file.
    internal const string NativeName = "Transform 2D";
}
