using System.Runtime.InteropServices;

namespace Index.Components;

// Native (ECS pool-resident) view of the Sprite Renderer component. The managed
// `Index.SpriteRenderer` class is the script-friendly wrapper around the same
// underlying data; this struct is the zero-marshal hot-path view used by
// Entity.GetRef<NativeSpriteRenderer>() / scene.QueryRef<...>.
//
// Layout MUST match Index-Engine/src/Components/Graphics/SpriteRendererComponent.hpp.
// Total size = 32 bytes (verified by Entity_GetComponentSize at script host init).
//
// Field offsets (Windows x64, MSVC natural alignment):
//   00: short SortingOrder
//   02: byte  SortingLayer
//   03: 1-byte tail pad (next field is 2-aligned)
//   04: TextureHandle (2 × uint16)
//   08: ulong TextureAssetId (UUID)
//   16: Color (4 × float)
//   32: struct end (8-byte alignment from UUID)
//
// The texture is identified two ways:
//   - `TextureAssetId` (UUID) is the persistent identity that survives scene
//     reload and serializes; this is what scripts should write to swap sprites.
//   - `TextureHandle` is the live runtime slot in TextureManager — set by the
//     engine's asset-load hook AFTER the UUID changes. Scripts treat it as
//     read-only; writing it directly will be overwritten on next refresh and
//     can desync against TextureManager's generation tracking.
[StructLayout(LayoutKind.Sequential)]
public struct NativeSpriteRenderer : IComponent
{
    // Parameterless constructor exists so `new NativeSpriteRenderer()` (and
    // object-initializer syntax like `new NativeSpriteRenderer { SortingOrder = 1 }`)
    // triggers the field initializers below. WARNING: C# language rule —
    // `default(NativeSpriteRenderer)` and uninitialized struct declarations
    // still produce a zero-init value where Color = (0,0,0,0). Callers using
    // EntityCommandBuffer should prefer `new NativeSpriteRenderer { ... }`
    // so the renderer doesn't paint sprites as fully transparent black.
    public NativeSpriteRenderer() { }

    public short SortingOrder;
    public byte  SortingLayer;
    private byte _pad;

    public TextureHandle TextureHandle;
    public ulong         TextureAssetId;
    // Mirror of SpriteRendererComponent.hpp:15 `Color{1.0f, 1.0f, 1.0f, 1.0f}` —
    // matches the C++ default so freshly-created sprites are white-tinted
    // rather than transparent black.
    public Color         Color = new(1f, 1f, 1f, 1f);

    internal const string NativeName = "Sprite Renderer";
}
