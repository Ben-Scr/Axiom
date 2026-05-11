using System;
using Axiom.Interop;

namespace Axiom;

// Engine-shipped fallback / primitive textures. Every entry here resolves
// to a file under `<AxiomAssets>/Textures/Default/` that the engine loads
// at startup and keeps in TextureManager's reserved 0–8 slots — so e.g.
// `Texture.Square` is the same white-quad fallback `Renderer2D` substitutes
// when a sprite's texture handle goes stale, and `Texture.Circle` is the
// same disc the CircularSlider's background uses.
//
// Order MUST match `Axiom-Engine/src/Graphics/DefaultTexture.hpp`. Adding
// a new default texture is a 4-site change: native enum, native default
// path list (`s_DefaultTextures`), this enum, and the convenience
// property below.
public enum DefaultTexture : byte
{
    Square = 0,
    Pixel = 1,
    Circle = 2,
    Capsule = 3,
    IsometricDiamond = 4,
    HexagonFlatTop = 5,
    HexagonPointedTop = 6,
    NineSliced = 7,
    Invisible = 8,
}

public sealed class Texture : IEquatable<Texture>
{
    public ulong UUID { get; }

    internal Texture(ulong assetId)
    {
        UUID = assetId;
    }

    public bool IsValid => UUID != 0 && InternalCalls.Texture_LoadAsset(UUID);

    public string Name
    {
        get
        {
            if (UUID == 0)
                return "(None)";

            string name = InternalCalls.Asset_GetDisplayName(UUID);
            return string.IsNullOrEmpty(name) ? "(Missing Asset)" : name;
        }
    }

    public string Path => UUID != 0 ? InternalCalls.Asset_GetPath(UUID) : "";
    public int Width => UUID != 0 ? InternalCalls.Texture_GetWidth(UUID) : 0;
    public int Height => UUID != 0 ? InternalCalls.Texture_GetHeight(UUID) : 0;

    // Public so packages built in separate assemblies (Pkg.<Name>.dll)
    // can construct managed Texture wrappers from a stored asset GUID
    // without re-implementing the AssetRegistry lookup.
    public static Texture? FromAssetUUID(ulong assetId)
    {
        if (assetId == 0 || !InternalCalls.Asset_IsValid(assetId))
            return null;

        return InternalCalls.Texture_LoadAsset(assetId) ? new Texture(assetId) : null;
    }

    // Engine-shipped default textures. The native side resolves the enum
    // to its AssetRegistry GUID via `TextureManager::GetDefaultTexture` →
    // `GetTextureAssetUUID`, so the wrapper round-trips through the same
    // FromAssetUUID path as user assets — `IsValid`, `Name`, `Path`,
    // `Width`, and `Height` all behave identically.
    //
    // Returns null if the engine hasn't initialised TextureManager yet
    // (calling from a static-init or pre-OnStart context) or if the
    // requested default failed to load — both cases match what
    // `FromAssetUUID` would return for an absent user asset, so callers
    // already null-checking inspector-bound textures don't need a
    // separate path.
    public static Texture? GetDefault(DefaultTexture defaultTexture)
    {
        ulong assetId = InternalCalls.Texture_GetDefaultAssetUUID((byte)defaultTexture);
        return FromAssetUUID(assetId);
    }

    // Convenience accessors. Each property hits the native side once per
    // get — cheap, but if a hot loop accesses it every frame, prefer
    // caching the returned Texture in a local field.
    //
    //   var sprite = entity.GetComponent<SpriteRenderer>();
    //   sprite.Texture = Texture.Square;
    //
    // is the intended use.
    public static Texture? Square            => GetDefault(DefaultTexture.Square);
    public static Texture? Pixel             => GetDefault(DefaultTexture.Pixel);
    public static Texture? Circle            => GetDefault(DefaultTexture.Circle);
    public static Texture? Capsule           => GetDefault(DefaultTexture.Capsule);
    public static Texture? IsometricDiamond  => GetDefault(DefaultTexture.IsometricDiamond);
    public static Texture? HexagonFlatTop    => GetDefault(DefaultTexture.HexagonFlatTop);
    public static Texture? HexagonPointedTop => GetDefault(DefaultTexture.HexagonPointedTop);
    public static Texture? NineSliced        => GetDefault(DefaultTexture.NineSliced);
    public static Texture? Invisible         => GetDefault(DefaultTexture.Invisible);

    public bool Equals(Texture? other) => other is not null && UUID == other.UUID;
    public override bool Equals(object? obj) => obj is Texture other && Equals(other);
    public override int GetHashCode() => UUID.GetHashCode();
    public override string ToString() => Name;
}
