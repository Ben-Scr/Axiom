namespace Axiom.Components;

// Marker interface for blittable ECS component structs that mirror the native
// C++ component layout 1:1. Constrained on Entity.GetRef<T>() so only types
// that opt into the ref-API are reachable through the zero-copy path. The
// interface itself is empty — the contract is layout, not behavior.
//
// To add a new component to the ref-API:
//   1. Define `public struct YourComponent : IComponent { ... fields ... }` in
//      this folder with `[StructLayout(LayoutKind.Sequential)]`.
//   2. Mirror the C++ field order from the matching Components/.../YourComponent.hpp.
//   3. Register the native name → C# type mapping in ComponentTypes (so the
//      runtime can resolve `GetRef<YourComponent>` to the right native lookup).
//   4. ScriptHostBridge will verify sizeof(YourComponent) == sizeof(C++ T) at
//      startup and refuse to load the user assembly on mismatch.
public interface IComponent { }
