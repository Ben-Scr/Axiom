using System.Runtime.InteropServices;

namespace Axiom.Components;

// Native (ECS pool-resident) view of the Transform 2D component. The managed
// `Axiom.Transform2D` class is the script-friendly wrapper around the same
// underlying data; this struct is the zero-marshal hot-path view used by
// Entity.GetRef<NativeTransform2D>() / TransformRef / scene.QueryRef<...>.
//
// Layout MUST match Axiom-Engine/src/Components/General/Transform2DComponent.hpp
// (the C++ Transform2DComponent class). ScriptHostBridge enforces this at
// script-engine init by calling Entity_GetComponentSize("Transform 2D") and
// comparing against sizeof(NativeTransform2D) — a drift caught early hard-fails
// the user assembly load instead of silently corrupting EnTT storage.
//
// World-space fields (Position/Scale/Rotation) are written by
// TransformHierarchySystem each frame from the Local* values composed against
// the parent's world transform. Scripts that want their writes to stick
// across the next frame's hierarchy pass MUST write the Local* fields — the
// world fields are a derived cache. The C++ side's setters do this; the ref-API
// is more direct, so the convention is "edit LocalPosition, observe Position
// after the propagate pass."
[StructLayout(LayoutKind.Sequential)]
public struct NativeTransform2D : IComponent
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

    // Direction vectors derived from world Rotation (0 rad → Up = (0, 1)).
    // `readonly` so accessing them on a `ref` to a component pool entry does
    // not force the compiler to make a defensive struct copy.
    public readonly Vector2 Up    => new(-Mathf.Sin(Rotation), Mathf.Cos(Rotation));
    public readonly Vector2 Down  => -Up;
    public readonly Vector2 Right => new(Mathf.Cos(Rotation),  Mathf.Sin(Rotation));
    public readonly Vector2 Left  => -Right;

    public float Speed
    {
        get
        {
            return 0;
        }
        set
        {

        }
    }

    public readonly float RotationDegrees => Rotation * Mathf.Rad2Deg;

    // Native serialized/display name used by the binding layer to find this
    // component's pool. Kept as a single source of truth so renames don't
    // diverge between IComponent::Get / inspector / scene file.
    internal const string NativeName = "Transform 2D";
}
