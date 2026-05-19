namespace Index.Components;

// Marker interface for blittable ECS component structs. Every value-type
// that implements IComponent in the user assembly is auto-registered with
// the engine at script-load — no attribute, no codegen, no Rebuild Engine.
// The struct's C# type name becomes its display + serialized name; the
// component lands under the "Native Components (C#)" header in the Add
// Component popup. Authoring is symmetric with managed `class : Component`
// scripts: declare the type, save the file, the editor picks it up.
//
// Layout rules:
//   - The struct should be [StructLayout(LayoutKind.Sequential)] (auto-
//     enforced by the registrar — non-sequential types are rejected).
//   - Fields must be blittable primitives or other blittable structs;
//     Marshal.SizeOf<T> must succeed.
//   - Field initializers run when the engine default-constructs the
//     component (via `new T()` reflection), so write `public T() { }` so
//     C# emits the parameterless ctor that triggers them.
//
// Hand-written mirrors of built-in C++ components (NativeTransform2D,
// NativeSpriteRenderer in Index.Native) live in the engine assembly,
// not the user assembly, so they are NOT auto-registered as duplicates.
// They resolve to the matching C++ component via the `internal const string
// NativeName` field on the struct (see ComponentTypes.ResolveNativeName).
public interface IComponent { }
