using System;

namespace Index.Components;

// Marker interface for blittable ECS component structs that mirror the native
// C++ component layout 1:1. Constrained on Entity.GetRef<T>() so only types
// that opt into the ref-API are reachable through the zero-copy path. The
// interface itself is empty: the contract is layout, not behavior.
//
// Naming convention: hot-path structs are usually prefixed `Native*`
// (for example `NativeTransform2D`, `NativeSpriteRenderer`). The unprefixed
// names (`Index.Transform2D`, `Index.SpriteRenderer`) are managed class
// wrappers that go through InternalCalls and target script-side ergonomics.
//
// To add a new native component to the ref-API:
//   1. Define `public struct NativeYourComponent : IComponent { ... fields ... }`
//      with `[StructLayout(LayoutKind.Sequential)]`.
//   2. Mirror the C++ field order from the matching component header.
//   3. If the native serialized/display name cannot be inferred from the C#
//      type name, add `[NativeComponent("Native Name")]`.
//   4. ScriptHostBridge verifies sizeof(NativeYourComponent) == sizeof(C++ T)
//      when the struct is first used and refuses to continue on mismatch.
public interface IComponent { }

[AttributeUsage(AttributeTargets.Struct, AllowMultiple = false, Inherited = false)]
public sealed class NativeComponentAttribute : Attribute
{
    public NativeComponentAttribute(string name)
    {
        Name = name;
    }

    public string Name { get; }

    // Codegen opt-in. Default `false` keeps the existing mirror semantics
    // (this struct names an existing native component). Set `true` to have
    // Index-ComponentCodegen emit the matching C++ struct + registration
    // call at build time, so a single C# file is the only thing the user
    // needs to author for a new ECS component. See plans/ja-mach-einen-plan-modular-clock.md.
    public bool Generate { get; set; } = false;

    // Inspector Add-Component menu placement when Generate=true. Ignored
    // when Generate=false (the mirrored native component already has its
    // own category from the C++ registration).
    public string Category { get; set; }    = "Component";  // "Component" or "Tag"
    public string Subcategory { get; set; } = "Custom";
}
