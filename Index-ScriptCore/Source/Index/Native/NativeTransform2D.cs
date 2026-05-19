using System.Runtime.InteropServices;
using Index.Components;

namespace Index.Native;

// Native (ECS pool-resident) view of the Transform 2D component. The managed
// `Index.Transform2D` class is the script-friendly wrapper around the same
// underlying data; this struct is the zero-marshal hot-path view used by
// Entity.GetRef<NativeTransform2D>() / TransformRef / scene.QueryRef<...>.
//
// Layout MUST match Index-Engine/src/Components/General/Transform2DComponent.hpp
// (the C++ Transform2DComponent class). ScriptHostBridge enforces this at
// script-engine init by calling Entity_GetComponentSize("Transform 2D") and
// comparing against sizeof(NativeTransform2D).
//
// World-space values are still a derived cache, but the public
// Position/Scale/Rotation setters mirror root-entity writes into Local* and
// mark the component dirty. That makes ref-API edits behave like the regular
// Transform2D setters instead of being overwritten on the next hierarchy pass.
[StructLayout(LayoutKind.Sequential)]
public struct NativeTransform2D : IComponent
{
    // Parameterless constructor exists so `new NativeTransform2D()` (and
    // object-initializer syntax like `new NativeTransform2D { Position = p }`)
    // triggers the field initializers below. WARNING: C# language rule —
    // `default(NativeTransform2D)` and uninitialized struct declarations
    // (e.g. `NativeTransform2D t;`) still produce a zero-init value where
    // Scale = (0,0) and m_Dirty = false. This trap is real for any direct
    // `ecb.AddComponent<NativeTransform2D>(e, default)` call. The
    // `ecb.CreateEntityWith<NativeTransform2D, ...>()` family is safe — it
    // records a payload-free Ecb_DefaultConstructComponent op so the C++
    // member-initializers fire on the native side.
    public NativeTransform2D() { }

    private Vector2 m_Position;
    private Vector2 m_Scale = new(1f, 1f);
    private float  m_RotationRadians;

    private Vector2 m_LocalPosition;
    private Vector2 m_LocalScale = new(1f, 1f);
    private float  m_LocalRotationRadians;

    // C++ `bool m_Dirty = true;` in Transform2DComponent.hpp — newly-created
    // transforms are dirty until TransformHierarchySystem composes them, so
    // mirror that default here to match the C++ component byte-for-byte.
    [MarshalAs(UnmanagedType.U1)]
    private bool m_Dirty = true;
    // Native owner metadata: Scene* + EntityHandle. These fields are intentionally
    // private to scripting, but they must exist so the managed layout stays byte-
    // identical to the C++ Transform2DComponent.
    private nint m_OwnerScene;
    private uint m_OwnerEntity;

    public Vector2 Position
    {
        readonly get => m_Position;
        set
        {
            m_Position = value;
            m_LocalPosition = value;
            m_Dirty = true;
        }
    }

    public Vector2 Scale
    {
        readonly get => m_Scale;
        set
        {
            m_Scale = value;
            m_LocalScale = value;
            m_Dirty = true;
        }
    }

    public Vector2 LocalPosition
    {
        readonly get => m_LocalPosition;
        set
        {
            m_LocalPosition = value;
            m_Position = value;
            m_Dirty = true;
        }
    }

    public Vector2 LocalScale
    {
        readonly get => m_LocalScale;
        set
        {
            m_LocalScale = value;
            m_Scale = value;
            m_Dirty = true;
        }
    }

    public bool Dirty
    {
        readonly get => m_Dirty;
        set => m_Dirty = value;
    }

    public float Rotation
    {
        readonly get => m_RotationRadians * Mathf.Rad2Deg;
        set
        {
            m_RotationRadians = value * Mathf.Deg2Rad;
            m_LocalRotationRadians = m_RotationRadians;
            m_Dirty = true;
        }
    }

    public float LocalRotation
    {
        readonly get => m_LocalRotationRadians * Mathf.Rad2Deg;
        set
        {
            m_LocalRotationRadians = value * Mathf.Deg2Rad;
            m_RotationRadians = m_LocalRotationRadians;
            m_Dirty = true;
        }
    }

    // Direction vectors derived from world Rotation (0 degrees -> Up = (0, 1)).
    public readonly Vector2 Up    => new(-Mathf.Sin(m_RotationRadians), Mathf.Cos(m_RotationRadians));
    public readonly Vector2 Down  => -Up;
    public readonly Vector2 Right => new(Mathf.Cos(m_RotationRadians),  Mathf.Sin(m_RotationRadians));
    public readonly Vector2 Left  => -Right;

    public readonly float RotationDegrees => Rotation;

    // Native serialized/display name used by the binding layer to find this
    // component's pool.
    internal const string NativeName = "Transform 2D";
}
