using System;
using Axiom.Interop;

namespace Axiom;
public sealed class Font : IEquatable<Font>
{
    public ulong UUID { get; }

    internal Font(ulong assetId)
    {
        UUID = assetId;
    }

    public bool IsValid => UUID != 0 && InternalCalls.Font_LoadAsset(UUID);

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

    // Public so packages built in separate assemblies (Pkg.<Name>.dll)
    // can construct managed Font wrappers from a stored asset GUID
    // without re-implementing the AssetRegistry lookup.
    public static Font? FromAssetUUID(ulong assetId)
    {
        if (assetId == 0 || !InternalCalls.Asset_IsValid(assetId))
            return null;

        return InternalCalls.Font_LoadAsset(assetId) ? new Font(assetId) : null;
    }

    public bool Equals(Font? other) => other is not null && UUID == other.UUID;
    public override bool Equals(object? obj) => obj is Font other && Equals(other);
    public override int GetHashCode() => UUID.GetHashCode();
    public override string ToString() => Name;
}
