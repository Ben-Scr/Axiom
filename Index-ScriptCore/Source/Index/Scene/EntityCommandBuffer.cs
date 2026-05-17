using System;
using System.Runtime.CompilerServices;
using Index.Components;
using Index.Interop;

namespace Index;

/// <summary>
/// Batch-records entity creation and component additions, then plays the
/// whole batch back to the native scene in a single P/Invoke. Drop-in
/// replacement for tight <c>Entity.Create + entity.AddComponent</c> loops
/// when spawning many entities at once (bullets, particles, prefab waves)
/// — typically 50–100× faster on the dominant cases because:
///
/// <list type="bullet">
///   <item>One P/Invoke for the whole batch (vs. one per component).</item>
///   <item>Component identity travels as a stable <c>u32</c> type ID — no
///   per-call UTF-8 marshaling of the component name.</item>
///   <item>Native entity allocation goes through
///   <c>Scene::CreateEntitiesBulk</c> + <c>Scene::ReserveForLoad</c>,
///   collapsing N pool growths and N identity-map rehashes into one.</item>
///   <item>Each component payload is <c>memcpy</c>'d directly into the
///   EnTT storage from the recorded bytes — no per-property setter.</item>
///   <item>Idempotent on_construct hooks (Transform2D, SpriteRenderer,
///   StaticTag, ParticleSystem2D) are deferred under a <c>Scene::LoadGuard</c>
///   and re-fired in one sweep at the end of playback.</item>
/// </list>
///
/// <para>
/// Only unmanaged <c>IComponent</c> structs whose layout exactly mirrors
/// the C++ component are supported as command payloads — the
/// <c>ComponentTypes&lt;T&gt;</c> static constructor enforces the
/// <c>sizeof</c> match. Components whose C++ side holds scene-bound
/// runtime state (e.g. ParticleSystem2D's emitter handle) must opt-in
/// natively by supplying a custom <c>emplaceFromBytes</c> at
/// registration time; recording such a component without that opt-in
/// will fail validation during playback.
/// </para>
///
/// <para>
/// The recorder is single-threaded; serialize access externally if you
/// share an instance across threads. Each instance is reusable — call
/// <see cref="Clear"/> after playback to record a new batch without
/// reallocating the underlying buffer.
/// </para>
/// </summary>
public sealed class EntityCommandBuffer : IDisposable
{
    // Mirrors EcbOpcode in EntityCommandBufferWire.hpp.
    private const byte OP_ADD_COMPONENT = 1;

    // Sentinel "no name" matching kEcbNoName on the native side.
    private const uint NO_NAME = 0xFFFFFFFFu;

    // Fixed-size header (8) + per-command fixed prefix (11) — used for
    // capacity bookkeeping. Hardcoded rather than reading sizeof against
    // a managed mirror struct because the wire layout is intentionally
    // version-stable.
    private const int HEADER_BYTES = 8;
    private const int COMMAND_PREFIX_BYTES = 11; // u8 + u32 + u32 + u16

    // Command stream only — entity table holds no per-slot data in v1
    // (every slot is NO_NAME) so we synthesize it at playback time
    // instead of carrying 4 bytes per entity through the recording
    // window. This keeps the recorder's hot path strictly proportional
    // to the number of components actually written.
    private byte[] m_Commands;
    private int m_CommandsLen;
    private int m_CommandCount;
    private uint m_EntityCount;

    // Buffer the native playback writes runtime IDs into. Allocated
    // lazily on first Playback so a Clear()-and-reuse loop keeps the
    // backing storage when the entity count is stable.
    private ulong[]? m_CreatedIds;
    private int m_CreatedCount;

    /// <summary>
    /// Construct a new recorder with an initial command-stream capacity
    /// (in bytes). The buffer grows geometrically; pre-sizing avoids the
    /// first few resizes when the batch size is roughly known.
    /// </summary>
    public EntityCommandBuffer(int initialCapacity = 1024)
    {
        if (initialCapacity < HEADER_BYTES) initialCapacity = HEADER_BYTES;
        m_Commands = new byte[initialCapacity];
    }

    /// <summary>Number of entities queued in this batch so far.</summary>
    public int EntityCount => (int)m_EntityCount;

    /// <summary>Number of recorded commands so far.</summary>
    public int CommandCount => m_CommandCount;

    /// <summary>
    /// Records the creation of a fresh runtime-origin entity and returns
    /// a handle that subsequent <see cref="AddComponent{T}"/> calls
    /// reference. The returned handle is valid until the next
    /// <see cref="Clear"/> or <see cref="Dispose"/>.
    /// </summary>
    public EntityRef CreateEntity()
    {
        EntityRef r = new EntityRef(m_EntityCount);
        m_EntityCount++;
        return r;
    }

    /// <summary>
    /// Records "attach a component of type <typeparamref name="T"/> with
    /// the given value to the entity referenced by <paramref name="e"/>".
    /// The value's bytes are copied into the recorder's buffer
    /// immediately, so the caller can reuse the source struct after
    /// returning.
    /// </summary>
    public unsafe void AddComponent<T>(EntityRef e, in T data) where T : unmanaged, IComponent
    {
        int payloadSize = sizeof(T);
        // The wire format reserves a u16 for payloadSize, so 65535 is
        // the hard upper bound — defensive even though no real built-in
        // component approaches that size.
        if (payloadSize > ushort.MaxValue)
        {
            throw new ArgumentException(
                $"Component '{typeof(T).Name}' sizeof = {payloadSize} exceeds the ECB's u16 payload limit.",
                nameof(data));
        }
        if (e.Index >= m_EntityCount)
        {
            throw new ArgumentException(
                $"EntityRef index {e.Index} is out of range for this ECB (entityCount = {m_EntityCount}). " +
                "Did you call CreateEntity on a different ECB?",
                nameof(e));
        }

        int recordSize = COMMAND_PREFIX_BYTES + payloadSize;
        EnsureCommandCapacity(recordSize);

        fixed (byte* basePtr = m_Commands)
        {
            byte* w = basePtr + m_CommandsLen;
            *w = OP_ADD_COMPONENT; w += 1;
            // memcpy each field rather than punning through an aligned
            // pointer write — the command stream is byte-packed and the
            // u32 / u16 slots land on odd offsets after the opcode byte.
            uint entityIndex = e.Index;
            Unsafe.CopyBlockUnaligned(w, &entityIndex, 4); w += 4;
            uint typeId = ComponentTypes<T>.NativeId;
            Unsafe.CopyBlockUnaligned(w, &typeId, 4); w += 4;
            ushort payloadSize16 = (ushort)payloadSize;
            Unsafe.CopyBlockUnaligned(w, &payloadSize16, 2); w += 2;
            Unsafe.CopyBlockUnaligned(w,
                Unsafe.AsPointer(ref Unsafe.AsRef(in data)),
                (uint)payloadSize);
        }

        m_CommandsLen += recordSize;
        m_CommandCount++;
    }

    /// <summary>
    /// Ships the recorded batch to the active scene. Returns the number
    /// of entities created (== <see cref="EntityCount"/> on success), or
    /// throws on a native error. After return, the runtime IDs of every
    /// created entity are available via <see cref="GetCreatedEntityId"/>
    /// or <see cref="GetCreatedEntity"/>, indexed by the same value the
    /// originating <see cref="EntityRef"/> wraps.
    /// </summary>
    public unsafe int Playback()
    {
        // Fast-out: an empty ECB is a valid no-op so a caller wiring
        // ECB into a generic spawn pipeline doesn't have to special-case
        // "this frame produced no work."
        if (m_EntityCount == 0)
        {
            m_CreatedCount = 0;
            return 0;
        }

        // Assemble the wire buffer: header + synthesized entity table + commands.
        int entityTableBytes = (int)m_EntityCount * sizeof(uint);
        int totalBytes = HEADER_BYTES + entityTableBytes + m_CommandsLen;
        byte[] wire = new byte[totalBytes];

        // Ensure the output ID buffer is large enough. Reuse the prior
        // allocation when the entity count fits — typical "spawn a wave
        // each frame" loop sees this branch every frame after the first.
        if (m_CreatedIds == null || m_CreatedIds.Length < m_EntityCount)
        {
            m_CreatedIds = new ulong[m_EntityCount];
        }

        int created;
        fixed (byte* wirePtr = wire)
        fixed (byte* cmdSrc = m_Commands)
        fixed (ulong* outIds = m_CreatedIds)
        {
            // Header.
            uint entityCount = m_EntityCount;
            uint commandCount = (uint)m_CommandCount;
            Unsafe.CopyBlockUnaligned(wirePtr, &entityCount, 4);
            Unsafe.CopyBlockUnaligned(wirePtr + 4, &commandCount, 4);

            // Entity table — every slot is NO_NAME in v1.
            uint* table = (uint*)(wirePtr + HEADER_BYTES);
            for (uint i = 0; i < m_EntityCount; i++)
            {
                table[i] = NO_NAME;
            }

            // Command stream.
            if (m_CommandsLen > 0)
            {
                Unsafe.CopyBlockUnaligned(
                    wirePtr + HEADER_BYTES + entityTableBytes,
                    cmdSrc,
                    (uint)m_CommandsLen);
            }

            created = InternalCalls.Ecb_Playback(wirePtr, totalBytes, outIds, m_CreatedIds.Length);
        }

        if (created < 0)
        {
            string reason = created switch
            {
                -1 => "wire buffer was truncated",
                -2 => "no active scene to play back into",
                -3 => "output ID buffer was too small (internal bug — please report)",
                -4 => "an AddComponent referenced a component type id with no native " +
                      "emplacer registered. Either the component isn't registered on the " +
                      "native side, or it holds non-memcpy-safe state (smart pointers / " +
                      "owning containers / scene-bound handles) and needs a custom " +
                      "ComponentInfo::emplaceFromBytes registered at engine startup. See " +
                      "the engine log for the offending typeId",
                _  => $"native error code {created}",
            };
            throw new InvalidOperationException(
                $"EntityCommandBuffer.Playback failed: {reason}.");
        }

        m_CreatedCount = created;
        return created;
    }

    /// <summary>
    /// Runtime ID of the i-th entity created by the most recent
    /// <see cref="Playback"/>. Compatible with every <c>Entity.*</c> API
    /// that takes a ulong entity ID. Throws when called before any
    /// successful playback or when <paramref name="index"/> is out of
    /// range.
    /// </summary>
    public ulong GetCreatedEntityId(int index)
    {
        if ((uint)index >= (uint)m_CreatedCount || m_CreatedIds == null)
        {
            throw new ArgumentOutOfRangeException(nameof(index),
                $"No playback result at index {index} (last playback created {m_CreatedCount} entities).");
        }
        return m_CreatedIds[index];
    }

    /// <summary>
    /// Convenience wrapper — same as <see cref="GetCreatedEntityId"/>
    /// but returns a live <see cref="Entity"/>.
    /// </summary>
    public Entity GetCreatedEntity(int index) => new Entity(GetCreatedEntityId(index));

    /// <summary>
    /// Discards every recorded command without releasing the backing
    /// buffer, leaving the instance ready to record a fresh batch. Used
    /// by per-frame spawn loops to avoid re-allocating between frames.
    /// </summary>
    public void Clear()
    {
        m_CommandsLen = 0;
        m_CommandCount = 0;
        m_EntityCount = 0;
        m_CreatedCount = 0;
    }

    /// <summary>
    /// Releases the recorder's buffers and clears the result table. The
    /// instance is unusable afterwards.
    /// </summary>
    public void Dispose()
    {
        Clear();
        m_Commands = Array.Empty<byte>();
        m_CreatedIds = null;
    }

    private void EnsureCommandCapacity(int additional)
    {
        int needed = m_CommandsLen + additional;
        if (needed <= m_Commands.Length) return;
        // Geometric growth — at least double, at least enough for the
        // pending record. Caps the amortised cost of repeated appends
        // at O(1) per append.
        int newCap = m_Commands.Length * 2;
        if (newCap < needed) newCap = needed;
        Array.Resize(ref m_Commands, newCap);
    }
}
