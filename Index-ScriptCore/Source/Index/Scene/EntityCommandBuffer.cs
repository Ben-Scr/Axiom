using System;
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Threading;
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
/// The instance-level <c>CreateEntity</c> / <c>AddComponent</c> recorder
/// is single-threaded — serialize access externally if you share a single
/// recorder across threads. For parallel-record from inside
/// <see cref="Index.Jobs.IJobParallelFor"/> / <c>IJobQuery</c> workers,
/// call <see cref="AsParallelWriter"/> and hand the returned
/// <see cref="ParallelWriter"/> to each worker; the parent ECB merges
/// every worker's stream at <see cref="Playback"/> time. Each instance is
/// reusable — call <see cref="Clear"/> after playback to record a new
/// batch without reallocating the underlying buffer.
/// </para>
/// </summary>
public sealed partial class EntityCommandBuffer : IDisposable
{
    // Fixed-size header (8) + per-command fixed prefix (11) — used for
    // capacity bookkeeping. Hardcoded rather than reading sizeof against
    // a managed mirror struct because the wire layout is intentionally
    // version-stable.
    internal const int HEADER_BYTES = EcbWire.HEADER_BYTES;
    internal const int COMMAND_PREFIX_BYTES = EcbWire.COMMAND_PREFIX_BYTES;

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

    // Wire buffer (header + entity table + commands) handed to the
    // native playback in one shot. Pooled per-instance and grown
    // geometrically — a per-frame spawn loop allocates the buffer
    // once on the first Playback and reuses it thereafter.
    private byte[]? m_WireBuffer;

    // Per-thread sub-recorders for parallel writes. Allocated lazily on
    // the first AsParallelWriter() call — single-threaded callers never
    // pay the dictionary cost.
    private ConcurrentDictionary<int, WorkerSlot>? m_ParallelSlots;

    // Sorted-slot snapshot cache. The set of slot identities only grows
    // during recording (each worker thread inserts once on first use),
    // so we bump m_SlotSnapshotVersion exactly when a new slot is
    // added. Playback rebuilds m_CachedSortedSlots only when the
    // version drifts — steady-state per-frame Playback hits zero
    // allocations on the merged path.
    private WorkerSlot[]? m_CachedSortedSlots;
    private int m_CachedSortedSlotsVersion;
    private int m_SlotSnapshotVersion;

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

    /// <summary>
    /// Number of entities queued in this batch so far (across the main
    /// recorder and every parallel writer). Inspection API — calls
    /// snapshot the worker-slot dictionary and allocate; do not call
    /// from inside a per-frame hot loop. The hot path (Playback)
    /// computes its own totals inline.
    /// </summary>
    public int EntityCount
    {
        get
        {
            uint total = m_EntityCount;
            if (m_ParallelSlots != null)
            {
                foreach (WorkerSlot s in m_ParallelSlots.Values)
                    total += s.EntityCount;
            }
            return (int)total;
        }
    }

    /// <summary>
    /// Number of recorded commands so far (across the main recorder and
    /// every parallel writer). Inspection API — see <see cref="EntityCount"/>
    /// for the snapshot caveat.
    /// </summary>
    public int CommandCount
    {
        get
        {
            int total = m_CommandCount;
            if (m_ParallelSlots != null)
            {
                foreach (WorkerSlot s in m_ParallelSlots.Values)
                    total += s.CommandCount;
            }
            return total;
        }
    }

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

        EcbWire.WriteAddComponentRecord(
            ref m_Commands,
            ref m_CommandsLen,
            ref m_CommandCount,
            e.Index,
            ComponentTypes<T>.NativeId,
            (ushort)payloadSize,
            Unsafe.AsPointer(ref Unsafe.AsRef(in data)));
    }

    /// <summary>
    /// Records "spawn the prefab tree identified by <paramref name="prefabGuid"/>"
    /// and returns an <see cref="EntityRef"/> for the prefab's ROOT entity. The
    /// child entities of the prefab tree are bulk-created on the native side
    /// during playback and become reachable from the returned root via
    /// <see cref="Entity.GetChildren"/> AFTER <see cref="Playback"/> completes —
    /// they are NOT addressable through additional ECB records.
    ///
    /// <para>
    /// Subsequent <see cref="AddComponent{T}"/> calls against the returned
    /// <see cref="EntityRef"/> attach to (or replace on) the prefab's root
    /// entity, layered on top of whatever the prefab's own components defined.
    /// </para>
    ///
    /// <para>
    /// First-spawn cost: the native side bakes a memcpy-ready template from
    /// the .prefab on disk, then destroys the throwaway tree. Every subsequent
    /// spawn replays from the cached bytes — typically 50–200× faster than
    /// repeated <see cref="Entity.Instantiate"/> calls. Prefabs that hold
    /// internal entity references fall back to a per-spawn slow path in v1;
    /// the ECB rejects those at playback with a clear error.
    /// </para>
    /// </summary>
    public unsafe EntityRef InstantiatePrefab(ulong prefabGuid)
    {
        if (prefabGuid == 0)
        {
            throw new ArgumentException(
                "InstantiatePrefab called with prefabGuid == 0; pass a valid prefab asset GUID.",
                nameof(prefabGuid));
        }

        EntityRef root = new EntityRef(m_EntityCount);
        m_EntityCount++;

        EcbWire.WriteInstantiatePrefabRecord(
            ref m_Commands,
            ref m_CommandsLen,
            ref m_CommandCount,
            root.Index,
            prefabGuid);

        return root;
    }

    /// <summary>
    /// Convenience overload — accepts an <see cref="Entity"/> created by
    /// <see cref="Entity.FromPrefabGUID"/> (the same shape user scripts get
    /// from <c>[ShowInEditor] Entity MyPrefab</c> when the field is wired to
    /// a prefab asset in the inspector). Equivalent to
    /// <c>InstantiatePrefab(prefabAsset.PrefabGUID)</c>.
    /// </summary>
    public EntityRef InstantiatePrefab(Entity prefabAsset)
    {
        if (prefabAsset is null || !prefabAsset.IsPrefabAsset)
        {
            throw new ArgumentException(
                "InstantiatePrefab requires a prefab-asset Entity (Entity.FromPrefabGUID). " +
                "Pass a runtime entity? Use Clone instead.",
                nameof(prefabAsset));
        }
        return InstantiatePrefab(prefabAsset.PrefabGUID);
    }

    // ── CreateEntityWith / CreateEntitiesWith ────────────────────────
    //
    // Bundle the common "CreateEntity + N×AddComponent(default)" pattern
    // into one call. Each component is recorded as a payload-free
    // Ecb_DefaultConstructComponent op so the native playback calls
    // `registry.emplace<T>(handle)` and the C++ default-member-initializers
    // fire (Transform2D Scale = (1,1), SpriteRenderer Color = white). The
    // managed-side `default(T)` is all-zero bytes and would silently
    // overwrite those defaults if it traveled through the regular
    // Ecb_AddComponent path.
    //
    // To set initial values per entity, follow CreateEntityWith with an
    // `AddComponent(ref, value)` for the components you want to override
    // (last-write-wins per type on a given entity), or set them in-place
    // via `GetCreatedEntity(i).GetRef<T>()` after Playback.
    //
    // Native IComponent only — the ECB never records managed Component
    // subclasses (those go through `Entity.CreateWith` instead).

    // Records an Ecb_DefaultConstructComponent op against `e` so the native
    // playback default-constructs T from C++ (preserving engine defaults
    // like Transform2D Scale = (1,1)). The wire record is 11 bytes — just
    // the fixed prefix, no payload.
    private void RecordDefaultConstruct<T>(EntityRef e) where T : unmanaged, IComponent
    {
        if (e.Index >= m_EntityCount)
        {
            throw new ArgumentException(
                $"EntityRef index {e.Index} is out of range for this ECB (entityCount = {m_EntityCount}). " +
                "Did you call CreateEntity on a different ECB?",
                nameof(e));
        }

        EcbWire.WriteDefaultConstructRecord(
            ref m_Commands,
            ref m_CommandsLen,
            ref m_CommandCount,
            e.Index,
            ComponentTypes<T>.NativeId);
    }

    /// <summary>
    /// Records a single entity with one default-constructed component.
    /// </summary>
    public EntityRef CreateEntityWith<T1>()
        where T1 : unmanaged, IComponent
    {
        EntityRef e = CreateEntity();
        RecordDefaultConstruct<T1>(e);
        return e;
    }

    /// <summary>
    /// Records a single entity with two default-constructed components.
    /// </summary>
    public EntityRef CreateEntityWith<T1, T2>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
    {
        EntityRef e = CreateEntity();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        return e;
    }

    /// <summary>
    /// Records a single entity with three default-constructed components.
    /// </summary>
    public EntityRef CreateEntityWith<T1, T2, T3>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
    {
        EntityRef e = CreateEntity();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        return e;
    }

    /// <summary>
    /// Records a single entity with four default-constructed components.
    /// </summary>
    public EntityRef CreateEntityWith<T1, T2, T3, T4>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
    {
        EntityRef e = CreateEntity();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        RecordDefaultConstruct<T4>(e);
        return e;
    }

    /// <summary>
    /// Records a single entity with five default-constructed components.
    /// </summary>
    public EntityRef CreateEntityWith<T1, T2, T3, T4, T5>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
    {
        EntityRef e = CreateEntity();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        RecordDefaultConstruct<T4>(e);
        RecordDefaultConstruct<T5>(e);
        return e;
    }

    /// <summary>
    /// Records a single entity with six default-constructed components.
    /// </summary>
    public EntityRef CreateEntityWith<T1, T2, T3, T4, T5, T6>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
    {
        EntityRef e = CreateEntity();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        RecordDefaultConstruct<T4>(e);
        RecordDefaultConstruct<T5>(e);
        RecordDefaultConstruct<T6>(e);
        return e;
    }

    /// <summary>
    /// Records a single entity with seven default-constructed components.
    /// </summary>
    public EntityRef CreateEntityWith<T1, T2, T3, T4, T5, T6, T7>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
    {
        EntityRef e = CreateEntity();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        RecordDefaultConstruct<T4>(e);
        RecordDefaultConstruct<T5>(e);
        RecordDefaultConstruct<T6>(e);
        RecordDefaultConstruct<T7>(e);
        return e;
    }

    /// <summary>
    /// Records a single entity with eight default-constructed components.
    /// </summary>
    public EntityRef CreateEntityWith<T1, T2, T3, T4, T5, T6, T7, T8>()
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
        where T8 : unmanaged, IComponent
    {
        EntityRef e = CreateEntity();
        RecordDefaultConstruct<T1>(e);
        RecordDefaultConstruct<T2>(e);
        RecordDefaultConstruct<T3>(e);
        RecordDefaultConstruct<T4>(e);
        RecordDefaultConstruct<T5>(e);
        RecordDefaultConstruct<T6>(e);
        RecordDefaultConstruct<T7>(e);
        RecordDefaultConstruct<T8>(e);
        return e;
    }

    /// <summary>
    /// Records <paramref name="length"/> entities, each with one
    /// default-constructed component. The created entities' refs are
    /// written into <paramref name="output"/> in creation order, so
    /// <c>output[i].Index</c> is contiguous starting from the current
    /// entity count. <paramref name="output"/> must be at least
    /// <paramref name="length"/> elements long.
    /// </summary>
    public void CreateEntitiesWith<T1>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = CreateEntity();
            RecordDefaultConstruct<T1>(e);
            output[i] = e;
        }
    }

    /// <summary>
    /// Records <paramref name="length"/> entities, each with two
    /// default-constructed components. See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
    /// for the <paramref name="output"/> contract.
    /// </summary>
    public void CreateEntitiesWith<T1, T2>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = CreateEntity();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            output[i] = e;
        }
    }

    /// <summary>
    /// Records <paramref name="length"/> entities, each with three
    /// default-constructed components. See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
    /// for the <paramref name="output"/> contract.
    /// </summary>
    public void CreateEntitiesWith<T1, T2, T3>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = CreateEntity();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            output[i] = e;
        }
    }

    /// <summary>
    /// Records <paramref name="length"/> entities, each with four
    /// default-constructed components. See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
    /// for the <paramref name="output"/> contract.
    /// </summary>
    public void CreateEntitiesWith<T1, T2, T3, T4>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = CreateEntity();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            output[i] = e;
        }
    }

    /// <summary>
    /// Records <paramref name="length"/> entities, each with five
    /// default-constructed components. See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
    /// for the <paramref name="output"/> contract.
    /// </summary>
    public void CreateEntitiesWith<T1, T2, T3, T4, T5>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = CreateEntity();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            RecordDefaultConstruct<T5>(e);
            output[i] = e;
        }
    }

    /// <summary>
    /// Records <paramref name="length"/> entities, each with six
    /// default-constructed components. See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
    /// for the <paramref name="output"/> contract.
    /// </summary>
    public void CreateEntitiesWith<T1, T2, T3, T4, T5, T6>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = CreateEntity();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            RecordDefaultConstruct<T5>(e);
            RecordDefaultConstruct<T6>(e);
            output[i] = e;
        }
    }

    /// <summary>
    /// Records <paramref name="length"/> entities, each with seven
    /// default-constructed components. See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
    /// for the <paramref name="output"/> contract.
    /// </summary>
    public void CreateEntitiesWith<T1, T2, T3, T4, T5, T6, T7>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = CreateEntity();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            RecordDefaultConstruct<T5>(e);
            RecordDefaultConstruct<T6>(e);
            RecordDefaultConstruct<T7>(e);
            output[i] = e;
        }
    }

    /// <summary>
    /// Records <paramref name="length"/> entities, each with eight
    /// default-constructed components. See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
    /// for the <paramref name="output"/> contract.
    /// </summary>
    public void CreateEntitiesWith<T1, T2, T3, T4, T5, T6, T7, T8>(int length, Span<EntityRef> output)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
        where T8 : unmanaged, IComponent
    {
        ValidateCreateEntitiesArgs(length, output.Length);
        for (int i = 0; i < length; i++)
        {
            EntityRef e = CreateEntity();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            RecordDefaultConstruct<T5>(e);
            RecordDefaultConstruct<T6>(e);
            RecordDefaultConstruct<T7>(e);
            RecordDefaultConstruct<T8>(e);
            output[i] = e;
        }
    }

    // Shared argument validation for every CreateEntitiesWith overload
    // (main recorder and parallel writer). Kept as a single method so the
    // error messages stay consistent across arities and so adding a new
    // rule is a one-site change.
    internal static void ValidateCreateEntitiesArgs(int length, int outputLength)
    {
        if (length < 0)
            throw new ArgumentOutOfRangeException(nameof(length),
                "length must be non-negative.");
        if (outputLength < length)
            throw new ArgumentException(
                $"output span length ({outputLength}) is smaller than entity count ({length}).",
                "output");
    }

    /// <summary>
    /// Returns a thread-safe writer that can be shared across job workers.
    /// Each calling thread records into its own lock-free sub-buffer; the
    /// parent ECB merges every sub-buffer at <see cref="Playback"/> time,
    /// remapping per-worker <see cref="EntityRef"/> indices into the
    /// merged entity range so the final batch is a single
    /// <c>Scene::CreateEntitiesBulk</c>.
    ///
    /// <para>
    /// Contract: recording must be quiescent (every dependent job
    /// completed) before <c>Playback</c> is called. An
    /// <c>EntityRef</c> returned by one worker's
    /// <see cref="ParallelWriter.CreateEntity"/> is valid ONLY in
    /// <see cref="ParallelWriter.AddComponent{T}"/> calls from the same
    /// worker thread — passing it to another worker corrupts the merged
    /// batch.
    /// </para>
    /// </summary>
    public ParallelWriter AsParallelWriter()
    {
        m_ParallelSlots ??= new ConcurrentDictionary<int, WorkerSlot>();
        return new ParallelWriter(this);
    }

    /// <summary>
    /// Ships the recorded batch to the active scene. Returns the number
    /// of entities created (== <see cref="EntityCount"/> on success), or
    /// throws on a native error. After return, the runtime IDs of every
    /// created entity are available via <see cref="GetCreatedEntityId"/>
    /// or <see cref="GetCreatedEntity"/>, indexed by the same value the
    /// originating <see cref="EntityRef"/> wraps (with parallel writes,
    /// indices are concatenated in main → worker-ascending-tid order, so
    /// the main recorder's entities come first, then worker 1's slot, then
    /// worker 2's slot, and so on).
    /// </summary>
    public unsafe int Playback()
    {
        PlaybackTotals totals = ComputeTotals();
        if (totals.EntityCount == 0)
        {
            m_CreatedCount = 0;
            return 0;
        }

        EnsureCreatedIdsCapacity((int)totals.EntityCount);

        int created;
        fixed (ulong* outIds = m_CreatedIds)
        {
            created = PlaybackDispatch(outIds, m_CreatedIds.Length, totals);
        }

        m_CreatedCount = created;
        return created;
    }

    /// <summary>
    /// Ships the recorded batch into a caller-owned ID buffer. Use this
    /// overload when the consumer already has a destination span (e.g.
    /// a <c>NativeArray&lt;ulong&gt;</c> or a temporary
    /// <c>stackalloc</c>) so the ECB's internal <c>m_CreatedIds</c>
    /// buffer is never allocated. <paramref name="destination"/> must
    /// be at least <see cref="EntityCount"/> elements long.
    /// <para>
    /// Unlike <see cref="Playback"/>, this overload does NOT update the
    /// recorder's <c>m_CreatedCount</c> — <see cref="GetCreatedEntityId"/>
    /// continues to reflect the last call to <see cref="Playback"/>.
    /// </para>
    /// </summary>
    public unsafe int PlaybackInto(Span<ulong> destination)
    {
        PlaybackTotals totals = ComputeTotals();
        if (totals.EntityCount == 0)
        {
            return 0;
        }

        if ((uint)destination.Length < totals.EntityCount)
        {
            throw new ArgumentException(
                $"Destination span (length {destination.Length}) is smaller than " +
                $"the batch entity count ({totals.EntityCount}).",
                nameof(destination));
        }

        int created;
        fixed (ulong* outIds = destination)
        {
            created = PlaybackDispatch(outIds, destination.Length, totals);
        }
        return created;
    }

    // Aggregates totals across the main recorder + every parallel worker
    // slot in one pass so Playback / PlaybackInto don't iterate twice.
    // Snapshot is taken under the quiescent contract; the cached sorted
    // slot array is returned when it's still valid.
    private struct PlaybackTotals
    {
        public uint EntityCount;
        public int CommandsLen;
        public int CommandCount;
        public WorkerSlot[]? Slots; // null when there are no parallel slots
    }

    private PlaybackTotals ComputeTotals()
    {
        PlaybackTotals t;
        t.EntityCount = m_EntityCount;
        t.CommandsLen = m_CommandsLen;
        t.CommandCount = m_CommandCount;
        t.Slots = null;

        if (m_ParallelSlots != null && !m_ParallelSlots.IsEmpty)
        {
            t.Slots = SortedSlotSnapshot();
            for (int i = 0; i < t.Slots.Length; i++)
            {
                t.EntityCount += t.Slots[i].EntityCount;
                t.CommandsLen += t.Slots[i].CommandsLen;
                t.CommandCount += t.Slots[i].CommandCount;
            }
        }

        return t;
    }

    private unsafe int PlaybackDispatch(ulong* outIds, int maxOut, PlaybackTotals totals)
    {
        return totals.Slots == null
            ? PlaybackMainOnlyImpl(outIds, maxOut, totals)
            : PlaybackMergedImpl(outIds, maxOut, totals);
    }

    private unsafe int PlaybackMainOnlyImpl(ulong* outIds, int maxOut, PlaybackTotals totals)
    {
        int entityTableBytes = (int)totals.EntityCount * sizeof(uint);
        int totalBytes = HEADER_BYTES + entityTableBytes + totals.CommandsLen;
        EnsureWireCapacity(totalBytes);

        int created;
        fixed (byte* wirePtr = m_WireBuffer)
        fixed (byte* cmdSrc = m_Commands)
        {
            // Header.
            uint entityCountU = totals.EntityCount;
            uint commandCountU = (uint)totals.CommandCount;
            Unsafe.CopyBlockUnaligned(wirePtr, &entityCountU, 4);
            Unsafe.CopyBlockUnaligned(wirePtr + 4, &commandCountU, 4);

            // Entity table — every slot is NO_NAME in v1. NO_NAME is
            // 0xFFFFFFFF, so a single InitBlock fills four uints per
            // store instead of one and vectorizes to `rep stos`.
            Unsafe.InitBlockUnaligned(wirePtr + HEADER_BYTES, 0xFF, (uint)entityTableBytes);

            // Command stream.
            if (totals.CommandsLen > 0)
            {
                Unsafe.CopyBlockUnaligned(
                    wirePtr + HEADER_BYTES + entityTableBytes,
                    cmdSrc,
                    (uint)totals.CommandsLen);
            }

            created = InternalCalls.Ecb_Playback(wirePtr, totalBytes, outIds, maxOut);
        }

        ThrowOnPlaybackError(created);
        return created;
    }

    private unsafe int PlaybackMergedImpl(ulong* outIds, int maxOut, PlaybackTotals totals)
    {
        WorkerSlot[] slots = totals.Slots!;
        int entityTableBytes = (int)totals.EntityCount * sizeof(uint);
        int totalBytes = HEADER_BYTES + entityTableBytes + totals.CommandsLen;
        EnsureWireCapacity(totalBytes);

        int created;
        fixed (byte* wirePtr = m_WireBuffer)
        {
            // Header.
            uint entityCountU = totals.EntityCount;
            uint commandCountU = (uint)totals.CommandCount;
            Unsafe.CopyBlockUnaligned(wirePtr, &entityCountU, 4);
            Unsafe.CopyBlockUnaligned(wirePtr + 4, &commandCountU, 4);

            // Entity table — every slot is NO_NAME.
            Unsafe.InitBlockUnaligned(wirePtr + HEADER_BYTES, 0xFF, (uint)entityTableBytes);

            // Command stream — main recorder first (baseOffset 0), then
            // each worker slot at its running base offset.
            byte* cmdDst = wirePtr + HEADER_BYTES + entityTableBytes;
            uint baseOffset = 0;

            if (m_CommandsLen > 0)
            {
                EcbWire.CopyAndRemapCommands(cmdDst, m_Commands, m_CommandsLen, baseOffset);
                cmdDst += m_CommandsLen;
            }
            baseOffset += m_EntityCount;

            for (int i = 0; i < slots.Length; i++)
            {
                WorkerSlot s = slots[i];
                if (s.CommandsLen > 0)
                {
                    EcbWire.CopyAndRemapCommands(cmdDst, s.Commands, s.CommandsLen, baseOffset);
                    cmdDst += s.CommandsLen;
                }
                baseOffset += s.EntityCount;
            }

            created = InternalCalls.Ecb_Playback(wirePtr, totalBytes, outIds, maxOut);
        }

        ThrowOnPlaybackError(created);
        return created;
    }

    private static void ThrowOnPlaybackError(int created)
    {
        if (created >= 0) return;
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
            -5 => "the batch would exceed the EnTT entity cap configured for this " +
                  "build. Open Project Settings > Entity ID bits, raise the value " +
                  "(24 -> 16M, 28 -> 268M), then regenerate project files and " +
                  "rebuild. See the engine log for the current count / cap",
            -6 => "an Ecb_InstantiatePrefab record referenced a prefab GUID that " +
                  "is unknown OR whose template can't be baked into a memcpy-ready " +
                  "form (e.g. the prefab contains internal entity references — a " +
                  "v1 limitation; fall back to Entity.Instantiate(prefab) for those). " +
                  "See the engine log for the offending GUID",
            _ => $"native error code {created}",
        };
        throw new InvalidOperationException(
            $"EntityCommandBuffer.Playback failed: {reason}.");
    }

    [System.Diagnostics.CodeAnalysis.MemberNotNull(nameof(m_WireBuffer))]
    private void EnsureWireCapacity(int needed)
    {
        if (m_WireBuffer != null && m_WireBuffer.Length >= needed) return;
        int newCap = m_WireBuffer?.Length ?? 0;
        if (newCap < needed)
        {
            newCap = Math.Max(needed, newCap * 2);
        }
        m_WireBuffer = new byte[newCap];
    }

    [System.Diagnostics.CodeAnalysis.MemberNotNull(nameof(m_CreatedIds))]
    private void EnsureCreatedIdsCapacity(int needed)
    {
        if (m_CreatedIds != null && m_CreatedIds.Length >= needed) return;
        int newCap = m_CreatedIds?.Length ?? 0;
        if (newCap < needed)
        {
            newCap = Math.Max(needed, newCap + (newCap >> 1));
        }
        m_CreatedIds = new ulong[newCap];
    }

    private WorkerSlot[] SortedSlotSnapshot()
    {
        // Steady-state fast path: the slot set hasn't changed since the
        // last Playback, so the cached sorted array is still valid. The
        // Playback contract requires workers to be quiescent before
        // calling, so no new slot can be racing in here.
        int version = Volatile.Read(ref m_SlotSnapshotVersion);
        if (m_CachedSortedSlots != null && m_CachedSortedSlotsVersion == version)
        {
            return m_CachedSortedSlots;
        }

        // ConcurrentDictionary.Values produces a snapshot; OK to sort
        // in place. Slot count is bounded by the number of worker threads
        // that ever called AsParallelWriter on this ECB — handful at most.
        WorkerSlot[] arr = new WorkerSlot[m_ParallelSlots!.Count];
        int idx = 0;
        foreach (WorkerSlot s in m_ParallelSlots.Values)
        {
            if (idx < arr.Length) arr[idx++] = s;
        }
        // arr may be slightly shorter than allocated if dict grew during
        // the iteration — should not happen under the quiescent contract,
        // but trim defensively.
        if (idx != arr.Length) Array.Resize(ref arr, idx);
        Array.Sort(arr, static (a, b) => a.ManagedThreadId.CompareTo(b.ManagedThreadId));

        m_CachedSortedSlots = arr;
        m_CachedSortedSlotsVersion = version;
        return arr;
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
    /// Parallel-writer sub-buffers are reset in place too, so per-frame
    /// loops keep their per-worker allocations.
    /// </summary>
    public void Clear()
    {
        m_CommandsLen = 0;
        m_CommandCount = 0;
        m_EntityCount = 0;
        m_CreatedCount = 0;
        if (m_ParallelSlots != null)
        {
            foreach (WorkerSlot s in m_ParallelSlots.Values)
            {
                s.CommandsLen = 0;
                s.CommandCount = 0;
                s.EntityCount = 0;
            }
        }
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
        m_WireBuffer = null;
        m_ParallelSlots = null;
    }

    // ── Parallel-writer plumbing (called from ParallelWriter) ────────

    internal WorkerSlot GetOrCreateSlotForCurrentThread()
    {
        int tid = Environment.CurrentManagedThreadId;
        return m_ParallelSlots!.GetOrAdd(
            tid,
            static (id, self) =>
            {
                // Factory runs exactly once per missing key — safe place
                // to bump the cache-invalidation counter without paying
                // an interlock on every GetOrAdd hit.
                Interlocked.Increment(ref self.m_SlotSnapshotVersion);
                return new WorkerSlot(id);
            },
            this);
    }

    // Per-thread mini-recorder. Each thread that ever called
    // AsParallelWriter.{CreateEntity,AddComponent} on this ECB owns one
    // slot; no slot is ever shared across threads, so the fields here
    // are written without synchronization.
    internal sealed class WorkerSlot
    {
        public byte[] Commands = new byte[256];
        public int CommandsLen;
        public int CommandCount;
        public uint EntityCount;
        public readonly int ManagedThreadId;

        public WorkerSlot(int managedThreadId)
        {
            ManagedThreadId = managedThreadId;
        }
    }
}

// Shared wire-format constants + low-level record writer/remapper.
// Both the single-threaded recorder and each parallel WorkerSlot pack
// command records through this helper so the byte layout stays in sync
// with the native side at one site.
internal static class EcbWire
{
    // Mirrors EcbOpcode in EntityCommandBufferWire.hpp.
    public const byte OP_ADD_COMPONENT      = 1;
    // OP_SET_COMPONENT = 2 (reserved on the native side; no managed
    // emitter today — call sites add components via OP_ADD_COMPONENT).
    public const byte OP_INSTANTIATE_PREFAB = 3;
    // Payload-free "attach this component, default-constructed from C++"
    // record. Used by CreateEntityWith / CreateEntitiesWith so the engine's
    // C++ member-initializers (Transform2D Scale = (1,1), SpriteRenderer
    // Color = white) survive instead of being overwritten by C#'s zero-init
    // `default(T)`. Native dispatch lives in ScriptBindingsEcb.cpp.
    public const byte OP_DEFAULT_CONSTRUCT_COMPONENT = 4;

    // Sentinel "no name" matching kEcbNoName on the native side.
    public const uint NO_NAME = 0xFFFFFFFFu;

    public const int HEADER_BYTES = 8;
    public const int COMMAND_PREFIX_BYTES = 11; // u8 opcode + u32 entityIndex + u32 typeId + u16 payloadSize
    // u64 prefabGuid — the only payload Ecb_InstantiatePrefab carries today.
    public const int INSTANTIATE_PREFAB_PAYLOAD_BYTES = 8;

    // Appends one Ecb_AddComponent record to (commands, commandsLen),
    // growing the byte buffer geometrically if needed. Callers maintain
    // their own commandsLen / commandCount counters by ref so the helper
    // works for both the parent ECB and per-worker slots without copying
    // back through a state struct.
    public static unsafe void WriteAddComponentRecord(
        ref byte[] commands,
        ref int commandsLen,
        ref int commandCount,
        uint entityIndex,
        uint typeId,
        ushort payloadSize,
        void* payload)
    {
        int recordSize = COMMAND_PREFIX_BYTES + payloadSize;
        EnsureCapacity(ref commands, commandsLen + recordSize);

        fixed (byte* basePtr = commands)
        {
            byte* w = basePtr + commandsLen;
            *w = OP_ADD_COMPONENT; w += 1;
            // memcpy each field rather than punning through an aligned
            // pointer write — the command stream is byte-packed and the
            // u32 / u16 slots land on odd offsets after the opcode byte.
            Unsafe.CopyBlockUnaligned(w, &entityIndex, 4); w += 4;
            Unsafe.CopyBlockUnaligned(w, &typeId, 4); w += 4;
            Unsafe.CopyBlockUnaligned(w, &payloadSize, 2); w += 2;
            if (payloadSize > 0)
            {
                Unsafe.CopyBlockUnaligned(w, payload, payloadSize);
            }
        }

        commandsLen += recordSize;
        commandCount++;
    }

    // Appends one Ecb_DefaultConstructComponent record. No payload —
    // the native side default-constructs T from the registered
    // `defaultEmplace` callback. The fixed 11-byte prefix matches
    // every other opcode, so the merge-and-remap walker
    // (CopyAndRemapCommands below) handles it without an opcode branch.
    public static unsafe void WriteDefaultConstructRecord(
        ref byte[] commands,
        ref int commandsLen,
        ref int commandCount,
        uint entityIndex,
        uint typeId)
    {
        const int recordSize = COMMAND_PREFIX_BYTES;
        EnsureCapacity(ref commands, commandsLen + recordSize);

        fixed (byte* basePtr = commands)
        {
            byte* w = basePtr + commandsLen;
            *w = OP_DEFAULT_CONSTRUCT_COMPONENT; w += 1;
            Unsafe.CopyBlockUnaligned(w, &entityIndex, 4); w += 4;
            Unsafe.CopyBlockUnaligned(w, &typeId, 4); w += 4;
            // payloadSize = 0 — no bytes follow.
            ushort payloadSize = 0;
            Unsafe.CopyBlockUnaligned(w, &payloadSize, 2);
        }

        commandsLen += recordSize;
        commandCount++;
    }

    // Appends one Ecb_InstantiatePrefab record. Reuses the same fixed
    // 11-byte prefix layout as Ecb_AddComponent so the parallel-merge
    // walker (CopyAndRemapCommands below) can splice prefab spawns into
    // the merged stream without per-opcode branching — it rewrites the
    // u32 entityIndex blindly and copies the payload through.
    public static unsafe void WriteInstantiatePrefabRecord(
        ref byte[] commands,
        ref int commandsLen,
        ref int commandCount,
        uint entityIndex,
        ulong prefabGuid)
    {
        const int recordSize = COMMAND_PREFIX_BYTES + INSTANTIATE_PREFAB_PAYLOAD_BYTES;
        EnsureCapacity(ref commands, commandsLen + recordSize);

        fixed (byte* basePtr = commands)
        {
            byte* w = basePtr + commandsLen;
            *w = OP_INSTANTIATE_PREFAB; w += 1;
            Unsafe.CopyBlockUnaligned(w, &entityIndex, 4); w += 4;
            // typeId slot is unused for this opcode; emit zero so a future
            // wire reader that doesn't know about OP_INSTANTIATE_PREFAB
            // sees a benign sentinel rather than uninitialized bytes.
            uint typeIdZero = 0;
            Unsafe.CopyBlockUnaligned(w, &typeIdZero, 4); w += 4;
            ushort payloadSize = INSTANTIATE_PREFAB_PAYLOAD_BYTES;
            Unsafe.CopyBlockUnaligned(w, &payloadSize, 2); w += 2;
            Unsafe.CopyBlockUnaligned(w, &prefabGuid, INSTANTIATE_PREFAB_PAYLOAD_BYTES);
        }

        commandsLen += recordSize;
        commandCount++;
    }

    public static void EnsureCapacity(ref byte[] buf, int needed)
    {
        if (needed <= buf.Length) return;
        int newCap = buf.Length * 2;
        if (newCap < needed) newCap = needed;
        Array.Resize(ref buf, newCap);
    }

    // Walks a packed command stream record-by-record and emits each
    // record into `dst`, rewriting its u32 entityIndex field by adding
    // `baseOffset`. Used by the merged-playback path to splice every
    // worker slot's stream into one buffer while keeping per-record
    // entity indices in the merged range.
    public static unsafe void CopyAndRemapCommands(
        byte* dst,
        byte[] src,
        int srcLen,
        uint baseOffset)
    {
        if (srcLen <= 0) return;

        fixed (byte* srcPtr = src)
        {
            byte* r = srcPtr;
            byte* end = srcPtr + srcLen;
            byte* w = dst;

            while (r < end)
            {
                byte op = *r;
                uint entityIndex;
                Unsafe.CopyBlockUnaligned(&entityIndex, r + 1, 4);
                uint typeId;
                Unsafe.CopyBlockUnaligned(&typeId, r + 5, 4);
                ushort payloadSize;
                Unsafe.CopyBlockUnaligned(&payloadSize, r + 9, 2);
                int recordSize = COMMAND_PREFIX_BYTES + payloadSize;

                uint remappedIndex = entityIndex + baseOffset;
                *w = op;
                Unsafe.CopyBlockUnaligned(w + 1, &remappedIndex, 4);
                Unsafe.CopyBlockUnaligned(w + 5, &typeId, 4);
                Unsafe.CopyBlockUnaligned(w + 9, &payloadSize, 2);
                if (payloadSize > 0)
                {
                    Unsafe.CopyBlockUnaligned(w + 11, r + 11, payloadSize);
                }

                r += recordSize;
                w += recordSize;
            }
        }
    }
}
