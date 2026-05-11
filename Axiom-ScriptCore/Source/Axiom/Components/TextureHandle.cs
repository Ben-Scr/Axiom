using System.Runtime.InteropServices;

namespace Axiom.Components;

// Layout MUST match Axiom-Engine/src/Graphics/TextureHandle.hpp.
// Two 16-bit fields; total 4 bytes, 2-byte aligned. Used inside the
// SpriteRenderer struct mirror so reads round-trip with the native side.
// The runtime treats Index == uint16.MaxValue as the invalid sentinel —
// matches `TextureHandle::Invalid()` on the C++ side.
[StructLayout(LayoutKind.Sequential)]
public struct TextureHandle
{
    public ushort Index;
    public ushort Generation;

    public const ushort InvalidIndex = ushort.MaxValue;
    public bool IsValid => Index != InvalidIndex;
}
