using System;
using Index.Interop;

namespace Index.Jobs;

/// <summary>
/// Minimal blittable per-frame snapshot designed to live as a field on
/// user job structs. Carries the four things every non-trivial job
/// actually needs:
///
/// <list type="bullet">
///   <item><see cref="Ecb"/> — thread-safe structural-changes channel.
///   Instantiate, Create, AddComponent all live here.</item>
///   <item><see cref="DeltaTime"/> — current scaled frame delta,
///   snapshot at <see cref="ForFrame"/> call time. Snapshotting avoids
///   a P/Invoke per job worker per index.</item>
///   <item><see cref="FrameNumber"/> — current frame index, snapshot
///   at <see cref="ForFrame"/> call time. Useful as a per-frame seed
///   ingredient.</item>
///   <item><see cref="BaseSeed"/> — caller-controllable seed for
///   deterministic per-worker rng via <see cref="GetRng"/>.</item>
/// </list>
///
/// <para>
/// The struct is intentionally tiny and <c>readonly</c> so it can be
/// safely copied per worker partition during <see cref="Job.ScheduleParallelFor"/>
/// dispatch — every worker sees the same snapshotted frame state without
/// reading shared mutable state from a worker thread.
/// </para>
///
/// <para>
/// Construct via <see cref="ForFrame"/> on the main thread before
/// scheduling, then plant the result in your job struct as a field:
/// </para>
///
/// <code>
/// public struct SpawnBulletsJob : IJobParallelFor
/// {
///     public JobContext Ctx;
///     public Entity BulletPrefab;
///
///     public void Execute(int i)
///     {
///         EntityRef root = Ctx.Ecb.Instantiate(BulletPrefab);
///         Ctx.Ecb.AddComponent(root, new NativeTransform2D
///         {
///             LocalPosition = new Vector2(i, Ctx.GetRng().NextFloat01() * 10f),
///         });
///     }
/// }
///
/// using var ecb = new EntityCommandBuffer(initialCapacity: 256 * 1024);
/// var job = new SpawnBulletsJob
/// {
///     Ctx = JobContext.ForFrame(ecb.AsParallelWriter()),
///     BulletPrefab = Entity.FromPrefabGUID(bulletGuid),
/// };
/// Job.ScheduleParallelFor(job, length: 10_000).Complete();
/// ecb.Playback();
/// </code>
/// </summary>
public readonly struct JobContext
{
    /// <summary>Thread-safe parallel writer for structural changes.</summary>
    public readonly EntityCommandBuffer.ParallelWriter Ecb;

    /// <summary>Scaled delta-time at snapshot time. See <see cref="Time.DeltaTime"/>.</summary>
    public readonly float DeltaTime;

    /// <summary>Frame counter at snapshot time. See <see cref="Time.FrameCount"/>.</summary>
    public readonly int FrameNumber;

    /// <summary>
    /// Base seed for <see cref="GetRng"/>. Same <see cref="JobContext"/>
    /// + same calling thread produces the same PRNG stream — useful for
    /// bit-identical replay of a frame. Zero means "derive from the
    /// frame number" (the <see cref="ForFrame"/> default).
    /// </summary>
    public readonly uint BaseSeed;

    public JobContext(
        EntityCommandBuffer.ParallelWriter ecb,
        float deltaTime,
        int frameNumber,
        uint baseSeed)
    {
        Ecb = ecb;
        DeltaTime = deltaTime;
        FrameNumber = frameNumber;
        BaseSeed = baseSeed;
    }

    /// <summary>
    /// Per-call deterministic rng. Mixes <see cref="BaseSeed"/>,
    /// <see cref="FrameNumber"/>, and the calling thread's managed-id
    /// so each worker partition gets a divergent stream while the same
    /// <see cref="JobContext"/> instance replays identically across runs.
    ///
    /// <para>
    /// Call once per <c>Execute</c> invocation and use the returned
    /// <see cref="JobRandom"/> for subsequent draws — re-seeding on
    /// every NextUInt() call would defeat the divergence.
    /// </para>
    /// </summary>
    public JobRandom GetRng() => JobRandom.ForCurrentWorker(BaseSeed, FrameNumber);

    /// <summary>
    /// Snapshot the current frame state into a fresh <see cref="JobContext"/>.
    /// Call once on the main thread before scheduling — the returned struct
    /// is safe to embed in a job struct and copy across worker partitions.
    /// </summary>
    /// <param name="ecb">
    /// Parallel writer for the job's structural changes. Usually obtained
    /// via <c>myEcb.AsParallelWriter()</c>.
    /// </param>
    /// <param name="baseSeed">
    /// Optional explicit base seed for reproducibility. Zero (default)
    /// derives a seed from the current frame number so consecutive frames
    /// produce divergent streams without the caller having to plumb one.
    /// </param>
    public static JobContext ForFrame(
        EntityCommandBuffer.ParallelWriter ecb,
        uint baseSeed = 0u)
    {
        float dt = InternalCalls.Application_GetDeltaTime();
        int frame = InternalCalls.Time_GetFrameCount();

        // Default seed: hash the frame number with a fixed constant
        // (Knuth's golden-ratio multiplier) so adjacent frames produce
        // wildly different starting states. Callers that want bit-identical
        // replay between runs pass an explicit baseSeed.
        uint resolvedSeed = baseSeed != 0u
            ? baseSeed
            : unchecked((uint)frame * 2654435761u);
        if (resolvedSeed == 0u) resolvedSeed = 1u; // xorshift cannot start from zero

        return new JobContext(ecb, dt, frame, resolvedSeed);
    }
}

/// <summary>
/// Deterministic per-worker pseudo-random generator. Tiny xorshift32
/// state (4 bytes), no allocation, suitable for the inside of a
/// <c>IJobParallelFor.Execute</c> body.
///
/// <para>
/// Construct via <see cref="JobContext.GetRng"/> rather than directly —
/// the factory mixes the calling thread's managed id into the seed so
/// each worker partition gets a divergent stream. Outside a job, use
/// <see cref="Index.Math.ParallelRandom"/> instead (that one is also
/// thread-local but wall-clock seeded, not deterministic).
/// </para>
/// </summary>
public struct JobRandom
{
    private uint m_State;

    /// <summary>
    /// Construct a per-worker generator. Mixes <paramref name="baseSeed"/>
    /// with the calling thread's managed id and the current frame so
    /// different workers, different frames, and different runs (when
    /// <paramref name="baseSeed"/> varies) each yield distinct streams.
    /// </summary>
    public static JobRandom ForCurrentWorker(uint baseSeed, int frame)
    {
        uint threadSalt = unchecked((uint)Environment.CurrentManagedThreadId);
        // Knuth's multiplier on the frame again so the per-worker seed
        // and the "default" base seed don't accidentally collide when
        // a caller passes BaseSeed = 0.
        uint frameSalt = unchecked((uint)frame * 0x9E3779B1u);
        uint state = baseSeed ^ threadSalt ^ frameSalt;
        if (state == 0u) state = 1u; // xorshift cannot start from zero
        return new JobRandom { m_State = state };
    }

    /// <summary>
    /// Uniform random uint in [0, uint.MaxValue]. Marsaglia xorshift32 —
    /// 2^32 - 1 period, statistically reasonable for game-loop purposes
    /// (visual variety, stochastic sampling). Not cryptographic.
    /// </summary>
    public uint NextUInt()
    {
        uint x = m_State;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        m_State = x;
        return x;
    }

    /// <summary>Uniform random float in [0, 1).</summary>
    public float NextFloat01()
    {
        // Drop the lowest 8 bits then scale by 2^-24 — the conventional
        // "produce a 24-bit float in [0,1)" trick that avoids the rounding
        // bias of (float)NextUInt() / float.MaxValue at the top of the range.
        return (NextUInt() >> 8) * (1.0f / 16777216.0f);
    }

    /// <summary>Uniform random float in [min, max).</summary>
    public float NextFloat(float min, float max) => min + (max - min) * NextFloat01();

    /// <summary>Uniform random int in [min, max). Returns <paramref name="min"/> when the range is empty.</summary>
    public int NextInt(int min, int max)
    {
        if (max <= min) return min;
        uint range = unchecked((uint)(max - min));
        return min + (int)(NextUInt() % range);
    }
}
