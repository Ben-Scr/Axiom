namespace Axiom.Components;

// Marker interface for blittable ECS component structs that mirror the native
// C++ component layout 1:1. Constrained on Entity.GetRef<T>() so only types
// that opt into the ref-API are reachable through the zero-copy path. The
// interface itself is empty — the contract is layout, not behavior.
//
// Naming convention: every struct in this namespace is prefixed `Native*`
// (e.g. `NativeTransform2D`, `NativeSpriteRenderer`). The unprefixed names
// (`Axiom.Transform2D`, `Axiom.SpriteRenderer`) are the managed class wrappers
// that go through InternalCalls — they live in `Axiom.Scene.Components.cs`
// and target script-side ergonomics over speed. Both views point at the same
// native EnTT pool; the prefix tells the reader which contract is in play
// at the call site:
//
//   - Managed class: convenience, per-property InternalCall, allocates a
//     wrapper, can host methods that need entity-ID context (SetParent, Play).
//   - Native struct: zero-marshal, ref-into-pool, requires `ref` discipline,
//     ideal for hot loops inside a GameSystem's QueryRef<T> iteration.
//
// To add a new native component to the ref-API:
//   1. Define `public struct NativeYourComponent : IComponent { ... fields ... }`
//      in this folder with `[StructLayout(LayoutKind.Sequential)]`.
//   2. Mirror the C++ field order from the matching Components/.../YourComponent.hpp.
//   3. Register the native display name → C# type mapping in ComponentTypes
//      (so the runtime can resolve `GetRef<NativeYourComponent>` to the right
//      native lookup).
//   4. ScriptHostBridge will verify sizeof(NativeYourComponent) == sizeof(C++ T)
//      at startup and refuse to load the user assembly on mismatch.
//   5. Optionally add a managed `YourComponent : Component` wrapper in
//      Axiom-ScriptCore/Source/Axiom/Scene/Components.cs for the convenience
//      API — the two coexist on the same entity.
public interface IComponent { }
