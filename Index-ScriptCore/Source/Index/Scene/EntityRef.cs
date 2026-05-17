namespace Index;

/// <summary>
/// Opaque, zero-cost handle to an entity slot inside a single
/// <see cref="EntityCommandBuffer"/>. Returned by <c>CreateEntity</c>
/// and passed to subsequent <c>AddComponent</c> calls on the same ECB to
/// address which entity each component attaches to.
///
/// The handle is meaningful ONLY within its originating ECB and ONLY
/// before <c>Playback</c> runs. After playback, look up the freshly-
/// minted live entity via <c>EntityCommandBuffer.GetCreatedEntityId(int)</c>
/// using the same index this struct wraps.
/// </summary>
public readonly struct EntityRef
{
    /// <summary>
    /// 0-based index into the originating ECB's entity table. Stable for
    /// the lifetime of the recording.
    /// </summary>
    public readonly uint Index;

    internal EntityRef(uint index)
    {
        Index = index;
    }
}
