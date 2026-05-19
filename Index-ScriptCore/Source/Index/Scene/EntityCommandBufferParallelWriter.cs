using System;
using System.Runtime.CompilerServices;
using Index.Components;

namespace Index;

public sealed partial class EntityCommandBuffer
{
    /// <summary>
    /// Lock-free per-worker recorder handed to job structs so they can
    /// record entity creations and component additions in parallel.
    /// Obtained via <see cref="EntityCommandBuffer.AsParallelWriter"/>.
    ///
    /// <para>
    /// Each calling thread writes into its own sub-buffer keyed by
    /// <see cref="Environment.CurrentManagedThreadId"/>; no locks live
    /// on the hot path. The parent <see cref="EntityCommandBuffer"/>
    /// merges every sub-buffer at <see cref="EntityCommandBuffer.Playback"/>
    /// time, remapping per-worker <see cref="EntityRef"/> indices into
    /// the merged entity range so the final batch is a single
    /// <c>Scene::CreateEntitiesBulk</c> on the native side.
    /// </para>
    ///
    /// <para>
    /// <b>Contract.</b>
    /// </para>
    /// <list type="bullet">
    ///   <item>Recording must be quiescent (every dependent job
    ///   completed) before the parent's <c>Playback</c> is called.</item>
    ///   <item>An <see cref="EntityRef"/> returned by
    ///   <see cref="CreateEntity"/> on one thread is valid ONLY in
    ///   <see cref="AddComponent{T}"/> calls from the SAME thread on
    ///   the same parent ECB. Passing a ref across worker boundaries
    ///   silently corrupts the merged batch — there's no thread tag in
    ///   <see cref="EntityRef"/> to detect this at runtime.</item>
    ///   <item>The struct is a cheap value-typed handle (one object
    ///   reference) — copy it freely into job structs.</item>
    /// </list>
    ///
    /// <para>
    /// Typical use from an <see cref="Index.Jobs.IJobParallelFor"/>:
    /// </para>
    /// <code>
    /// public struct CreateEntityJob : IJobParallelFor
    /// {
    ///     public EntityCommandBuffer.ParallelWriter Ecb;
    ///     public NativeArray&lt;Vector2&gt; Positions;
    ///
    ///     public void Execute(int i)
    ///     {
    ///         EntityRef e = Ecb.CreateEntity();
    ///         Ecb.AddComponent&lt;NativeTransform2D&gt;(e, new NativeTransform2D
    ///         {
    ///             LocalPosition = Positions[i],
    ///         });
    ///     }
    /// }
    /// </code>
    /// </summary>
    public readonly struct ParallelWriter
    {
        private readonly EntityCommandBuffer m_Parent;

        internal ParallelWriter(EntityCommandBuffer parent)
        {
            m_Parent = parent;
        }

        /// <summary>
        /// Records the creation of a fresh runtime-origin entity inside
        /// the calling thread's sub-buffer and returns a handle valid
        /// for subsequent <see cref="AddComponent{T}"/> calls on this
        /// SAME thread. The handle's <see cref="EntityRef.Index"/> is
        /// LOCAL to the calling thread's sub-buffer — the parent ECB
        /// remaps it into the merged entity range during
        /// <see cref="EntityCommandBuffer.Playback"/>.
        /// </summary>
        public EntityRef Create()
        {
            WorkerSlot slot = m_Parent.GetOrCreateSlotForCurrentThread();
            uint localIndex = slot.EntityCount;
            slot.EntityCount = localIndex + 1;
            return new EntityRef(localIndex);
        }

        /// <summary>
        /// Records "attach a component of type <typeparamref name="T"/>
        /// with the given value to the entity referenced by
        /// <paramref name="e"/>" inside the calling thread's sub-buffer.
        /// The value's bytes are copied into the sub-buffer immediately,
        /// so the caller can reuse the source struct after returning.
        ///
        /// <para>
        /// <paramref name="e"/> must have been returned by
        /// <see cref="CreateEntity"/> on this same thread — using a ref
        /// from a different worker silently corrupts the merged batch.
        /// </para>
        /// </summary>
        public unsafe void AddComponent<T>(EntityRef e, in T data) where T : unmanaged, IComponent
        {
            int payloadSize = sizeof(T);
            if (payloadSize > ushort.MaxValue)
            {
                throw new ArgumentException(
                    $"Component '{typeof(T).Name}' sizeof = {payloadSize} exceeds the ECB's u16 payload limit.",
                    nameof(data));
            }

            WorkerSlot slot = m_Parent.GetOrCreateSlotForCurrentThread();
            if (e.Index >= slot.EntityCount)
            {
                // Catches the common cross-thread misuse: an EntityRef
                // created on thread A is passed to thread B, whose slot
                // hasn't reserved that index. Cannot catch the case where
                // thread B coincidentally has enough entities of its own
                // — see the contract note on ParallelWriter.
                throw new ArgumentException(
                    $"EntityRef index {e.Index} is out of range for this worker slot " +
                    $"(thread {slot.ManagedThreadId} entityCount = {slot.EntityCount}). " +
                    "Did you pass an EntityRef across worker threads, or call Create on a different ECB?",
                    nameof(e));
            }

            EcbWire.WriteAddComponentRecord(
                ref slot.Commands,
                ref slot.CommandsLen,
                ref slot.CommandCount,
                e.Index,
                ComponentTypes<T>.NativeId,
                (ushort)payloadSize,
                Unsafe.AsPointer(ref Unsafe.AsRef(in data)));
        }

        /// <summary>
        /// Records "spawn the prefab tree identified by <paramref name="prefabGuid"/>"
        /// in the calling thread's sub-buffer and returns an <see cref="EntityRef"/>
        /// for the prefab's ROOT entity. Children are bulk-created on the native
        /// side at playback (not addressable via additional ECB records — reach
        /// them via the root's <see cref="Entity.GetChildren"/> AFTER
        /// <see cref="EntityCommandBuffer.Playback"/>).
        ///
        /// <para>
        /// Same per-worker / per-thread semantics as <see cref="CreateEntity"/>:
        /// the returned ref's <c>Index</c> is local to the calling thread's slot
        /// and is remapped into the merged batch during playback. Passing it
        /// across worker threads silently corrupts the batch — keep refs on the
        /// thread that produced them.
        /// </para>
        /// </summary>
        public unsafe EntityRef Instantiate(ulong prefabGuid)
        {
            if (prefabGuid == 0)
            {
                throw new ArgumentException(
                    "Instantiate called with prefabGuid == 0; pass a valid prefab asset GUID.",
                    nameof(prefabGuid));
            }

            WorkerSlot slot = m_Parent.GetOrCreateSlotForCurrentThread();
            uint localIndex = slot.EntityCount;
            slot.EntityCount = localIndex + 1;

            EcbWire.WriteInstantiatePrefabRecord(
                ref slot.Commands,
                ref slot.CommandsLen,
                ref slot.CommandCount,
                localIndex,
                prefabGuid);

            return new EntityRef(localIndex);
        }

        /// <summary>
        /// Convenience overload — accepts an <see cref="Entity"/> obtained via
        /// <see cref="Entity.FromPrefabGUID"/> (e.g. an inspector-wired prefab
        /// field). Equivalent to <c>Instantiate(prefabAsset.PrefabGUID)</c>.
        /// </summary>
        public EntityRef Instantiate(Entity prefabAsset)
        {
            if (prefabAsset is null || !prefabAsset.IsPrefabAsset)
            {
                throw new ArgumentException(
                    "Instantiate requires a prefab-asset Entity (Entity.FromPrefabGUID). " +
                    "Pass a runtime entity? Use Clone instead.",
                    nameof(prefabAsset));
            }
            return Instantiate(prefabAsset.PrefabGUID);
        }

        // ── CreateEntityWith / CreateEntitiesWith ────────────────────
        //
        // Per-worker mirror of the main ECB overloads. Each call records a
        // payload-free Ecb_DefaultConstructComponent op against the calling
        // thread's WorkerSlot, so the native playback default-constructs
        // each component from C++ (preserving Transform2D Scale = (1,1),
        // SpriteRenderer Color = white). Thread-safety and entity-index
        // locality are inherited from CreateEntity on this struct.
        //
        // Native IComponent only — the parent ECB never records managed
        // Component subclasses.

        // Records an Ecb_DefaultConstructComponent op against `e` on the
        // calling thread's worker slot. Same wire format as the main ECB's
        // helper — payload-free, 11-byte prefix.
        private void RecordDefaultConstruct<T>(EntityRef e) where T : unmanaged, IComponent
        {
            WorkerSlot slot = m_Parent.GetOrCreateSlotForCurrentThread();
            if (e.Index >= slot.EntityCount)
            {
                throw new ArgumentException(
                    $"EntityRef index {e.Index} is out of range for this worker slot " +
                    $"(thread {slot.ManagedThreadId} entityCount = {slot.EntityCount}). " +
                    "Did you pass an EntityRef across worker threads, or call Create on a different ECB?",
                    nameof(e));
            }

            EcbWire.WriteDefaultConstructRecord(
                ref slot.Commands,
                ref slot.CommandsLen,
                ref slot.CommandCount,
                e.Index,
                ComponentTypes<T>.NativeId);
        }

        /// <summary>
        /// Records a single entity with one default-constructed component
        /// in the calling thread's sub-buffer.
        /// </summary>
        public EntityRef CreateWith<T1>()
            where T1 : unmanaged, IComponent
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            return e;
        }

        /// <summary>
        /// Records a single entity with two default-constructed components
        /// in the calling thread's sub-buffer.
        /// </summary>
        public EntityRef CreateWith<T1, T2>()
            where T1 : unmanaged, IComponent
            where T2 : unmanaged, IComponent
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            return e;
        }

        /// <summary>
        /// Records a single entity with three default-constructed components
        /// in the calling thread's sub-buffer.
        /// </summary>
        public EntityRef CreateWith<T1, T2, T3>()
            where T1 : unmanaged, IComponent
            where T2 : unmanaged, IComponent
            where T3 : unmanaged, IComponent
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            return e;
        }

        /// <summary>
        /// Records a single entity with four default-constructed components
        /// in the calling thread's sub-buffer.
        /// </summary>
        public EntityRef CreateWith<T1, T2, T3, T4>()
            where T1 : unmanaged, IComponent
            where T2 : unmanaged, IComponent
            where T3 : unmanaged, IComponent
            where T4 : unmanaged, IComponent
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            return e;
        }

        /// <summary>
        /// Records a single entity with five default-constructed components
        /// in the calling thread's sub-buffer.
        /// </summary>
        public EntityRef CreateWith<T1, T2, T3, T4, T5>()
            where T1 : unmanaged, IComponent
            where T2 : unmanaged, IComponent
            where T3 : unmanaged, IComponent
            where T4 : unmanaged, IComponent
            where T5 : unmanaged, IComponent
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            RecordDefaultConstruct<T5>(e);
            return e;
        }

        /// <summary>
        /// Records a single entity with six default-constructed components
        /// in the calling thread's sub-buffer.
        /// </summary>
        public EntityRef CreateWith<T1, T2, T3, T4, T5, T6>()
            where T1 : unmanaged, IComponent
            where T2 : unmanaged, IComponent
            where T3 : unmanaged, IComponent
            where T4 : unmanaged, IComponent
            where T5 : unmanaged, IComponent
            where T6 : unmanaged, IComponent
        {
            EntityRef e = Create();
            RecordDefaultConstruct<T1>(e);
            RecordDefaultConstruct<T2>(e);
            RecordDefaultConstruct<T3>(e);
            RecordDefaultConstruct<T4>(e);
            RecordDefaultConstruct<T5>(e);
            RecordDefaultConstruct<T6>(e);
            return e;
        }

        /// <summary>
        /// Records a single entity with seven default-constructed components
        /// in the calling thread's sub-buffer.
        /// </summary>
        public EntityRef CreateWith<T1, T2, T3, T4, T5, T6, T7>()
            where T1 : unmanaged, IComponent
            where T2 : unmanaged, IComponent
            where T3 : unmanaged, IComponent
            where T4 : unmanaged, IComponent
            where T5 : unmanaged, IComponent
            where T6 : unmanaged, IComponent
            where T7 : unmanaged, IComponent
        {
            EntityRef e = Create();
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
        /// Records a single entity with eight default-constructed components
        /// in the calling thread's sub-buffer.
        /// </summary>
        public EntityRef CreateWith<T1, T2, T3, T4, T5, T6, T7, T8>()
            where T1 : unmanaged, IComponent
            where T2 : unmanaged, IComponent
            where T3 : unmanaged, IComponent
            where T4 : unmanaged, IComponent
            where T5 : unmanaged, IComponent
            where T6 : unmanaged, IComponent
            where T7 : unmanaged, IComponent
            where T8 : unmanaged, IComponent
        {
            EntityRef e = Create();
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
        /// Records <paramref name="length"/> entities into the calling
        /// thread's sub-buffer, each with one default-constructed
        /// component. The created entities' refs are written into
        /// <paramref name="output"/> in creation order; the index values
        /// are local to this worker slot and are remapped into the
        /// merged entity range at <see cref="EntityCommandBuffer.Playback"/>.
        /// <paramref name="output"/> must be at least
        /// <paramref name="length"/> elements long.
        /// </summary>
        public void CreateEntitiesWith<T1>(int length, Span<EntityRef> output)
            where T1 : unmanaged, IComponent
        {
            ValidateCreateEntitiesArgs(length, output.Length);
            for (int i = 0; i < length; i++)
            {
                EntityRef e = Create();
                RecordDefaultConstruct<T1>(e);
                output[i] = e;
            }
        }

        /// <summary>
        /// Records <paramref name="length"/> entities into the calling
        /// thread's sub-buffer, each with two default-constructed components.
        /// See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
        /// for the <paramref name="output"/> contract.
        /// </summary>
        public void CreateEntitiesWith<T1, T2>(int length, Span<EntityRef> output)
            where T1 : unmanaged, IComponent
            where T2 : unmanaged, IComponent
        {
            ValidateCreateEntitiesArgs(length, output.Length);
            for (int i = 0; i < length; i++)
            {
                EntityRef e = Create();
                RecordDefaultConstruct<T1>(e);
                RecordDefaultConstruct<T2>(e);
                output[i] = e;
            }
        }

        /// <summary>
        /// Records <paramref name="length"/> entities into the calling
        /// thread's sub-buffer, each with three default-constructed components.
        /// See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
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
                EntityRef e = Create();
                RecordDefaultConstruct<T1>(e);
                RecordDefaultConstruct<T2>(e);
                RecordDefaultConstruct<T3>(e);
                output[i] = e;
            }
        }

        /// <summary>
        /// Records <paramref name="length"/> entities into the calling
        /// thread's sub-buffer, each with four default-constructed components.
        /// See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
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
                EntityRef e = Create();
                RecordDefaultConstruct<T1>(e);
                RecordDefaultConstruct<T2>(e);
                RecordDefaultConstruct<T3>(e);
                RecordDefaultConstruct<T4>(e);
                output[i] = e;
            }
        }

        /// <summary>
        /// Records <paramref name="length"/> entities into the calling
        /// thread's sub-buffer, each with five default-constructed components.
        /// See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
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
                EntityRef e = Create();
                RecordDefaultConstruct<T1>(e);
                RecordDefaultConstruct<T2>(e);
                RecordDefaultConstruct<T3>(e);
                RecordDefaultConstruct<T4>(e);
                RecordDefaultConstruct<T5>(e);
                output[i] = e;
            }
        }

        /// <summary>
        /// Records <paramref name="length"/> entities into the calling
        /// thread's sub-buffer, each with six default-constructed components.
        /// See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
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
                EntityRef e = Create();
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
        /// Records <paramref name="length"/> entities into the calling
        /// thread's sub-buffer, each with seven default-constructed components.
        /// See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
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
                EntityRef e = Create();
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
        /// Records <paramref name="length"/> entities into the calling
        /// thread's sub-buffer, each with eight default-constructed components.
        /// See <see cref="CreateEntitiesWith{T1}(int, Span{EntityRef})"/>
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
                EntityRef e = Create();
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
    }
}
