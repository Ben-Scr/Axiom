using System;
using Index;
using Index.Collections.Native;
using Index.Components;
using Index.Jobs;
using Index.Native;

// Worked example + integration test for EntityCommandBuffer.ParallelWriter.
//
// Demonstrates the "spawn N entities in parallel" pattern: a struct-typed
// IJobParallelFor records into a shared ECB.ParallelWriter, the host
// thread waits for the job to complete, then calls Playback() once. The
// parent ECB merges every worker's sub-buffer into a single
// Scene::CreateEntitiesBulk on the native side.
//
// Drop this GameSystem on any scene; it self-runs on Start, logs PASS/FAIL
// per case, then deletes the entities it spawned so the scene is left as
// it was found. The sample doubles as a regression test for the
// entityIndex-remap step in the merged-playback path.
public class CreateEntityJobSample : GameSystem
{
    [ShowInEditor("Entities Per Pass")] public int EntitiesPerPass = 10_000;
    [ShowInEditor("Reuse Iterations")]  public int ReuseIterations = 4;
    [ShowInEditor("Run On Start")]      public bool RunOnStart = true;

    private bool m_Ran;
    private int  m_TotalCases;
    private int  m_PassedCases;

    public override void OnStart()
    {
        if (!RunOnStart || m_Ran) return;
        m_Ran = true;
        RunAllCases();
    }

    // ── The sample job ──────────────────────────────────────────────

    // Reads one position per row from a NativeArray, creates one entity
    // per row, and attaches a NativeTransform2D at that position. Every
    // row runs on whichever worker thread Parallel.ForEach picks for it;
    // ParallelWriter routes each call into the calling thread's own
    // sub-buffer with no locks on the hot path.
    private struct CreateEntityJob : IJobParallelFor
    {
        [WriteOnly] public EntityCommandBuffer.ParallelWriter Ecb;
        [ReadOnly]  public NativeArrayView Positions;

        public void Execute(int i)
        {
            EntityRef e = Ecb.Create();
            Ecb.AddComponent(e, new NativeTransform2D
            {
                LocalPosition = Positions[i],
            });
        }
    }

    // Tiny ref-style view so the job struct doesn't have to hold the
    // managed NativeArray<Vector2> directly (NativeArray<T> is a class,
    // and jobs are most ergonomic when every field is unmanaged). The
    // view borrows the underlying buffer pointer for the job's lifetime.
    private unsafe readonly struct NativeArrayView
    {
        private readonly Vector2* m_Buffer;
        private readonly int m_Length;

        public NativeArrayView(Vector2* buffer, int length)
        {
            m_Buffer = buffer;
            m_Length = length;
        }

        public Vector2 this[int index] => m_Buffer[index];
        public int Length => m_Length;
    }

    // ── Driver ──────────────────────────────────────────────────────

    private void RunAllCases()
    {
        m_TotalCases = 0;
        m_PassedCases = 0;

        int n = EntitiesPerPass < 1 ? 1 : EntitiesPerPass;
        int iter = ReuseIterations < 1 ? 1 : ReuseIterations;

        Case("Parallel spawn + position round-trip",   () => Case_ParallelSpawn(n));
        Case("Reuse across N frames is allocation-stable", () => Case_ReuseLoop(n, iter));
        Case("Single-thread path still works (no AsParallelWriter)", () => Case_SingleThreadPathIntact(n));

        Log.Info($"[CreateEntityJobSample] {m_PassedCases}/{m_TotalCases} cases passed");
    }

    // ── Cases ───────────────────────────────────────────────────────

    private unsafe void Case_ParallelSpawn(int n)
    {
        using var positions = new NativeArray<Vector2>(n);
        Vector2* posPtr = (Vector2*)positions.GetUnsafePtr();
        for (int i = 0; i < n; i++)
        {
            posPtr[i] = new Vector2(i, i * 0.5f);
        }

        using var ecb = new EntityCommandBuffer(initialCapacity: 64 * 1024);
        var job = new CreateEntityJob
        {
            Ecb = ecb.AsParallelWriter(),
            Positions = new NativeArrayView(posPtr, n),
        };

        Job.ScheduleParallelFor(job, length: n).Complete();

        if (ecb.EntityCount != n)
            throw new InvalidOperationException(
                $"recorded entity count drift: ECB reports {ecb.EntityCount}, expected {n}");

        int created = ecb.Playback();
        if (created != n)
            throw new InvalidOperationException($"Playback created {created}, expected {n}");

        // The critical check: position-by-row round-trip. Each worker
        // recorded entity `localIndex` into its own slot; the merged-
        // playback path remaps that to `localIndex + slotBaseOffset`.
        // A bug in the remap would scramble positions even though the
        // total entity count looks right.
        //
        // We check that the SET of (position) values recorded matches
        // the set we asked for. Per-row identity isn't preserved
        // (workers pick up arbitrary row indices), but every position
        // we put in should come back out exactly once.
        var seen = new System.Collections.Generic.HashSet<long>(n);
        for (int i = 0; i < created; i++)
        {
            Entity spawned = ecb.GetCreatedEntity(i);
            ref NativeTransform2D tr = ref spawned.GetRef<NativeTransform2D>();
            if (Unsafe_IsNullRef(ref tr))
                throw new InvalidOperationException($"entity #{i} has no NativeTransform2D component after playback");

            long key = PackVector(tr.LocalPosition);
            if (!seen.Add(key))
                throw new InvalidOperationException($"entity #{i} duplicates position {tr.LocalPosition}");
        }

        for (int i = 0; i < n; i++)
        {
            long key = PackVector(posPtr[i]);
            if (!seen.Contains(key))
                throw new InvalidOperationException($"position {posPtr[i]} was recorded but no spawned entity has it");
        }

        // Clean up so the scene is left as we found it.
        DestroySpawned(ecb, created);
    }

    private unsafe void Case_ReuseLoop(int n, int iterations)
    {
        using var positions = new NativeArray<Vector2>(n);
        Vector2* posPtr = (Vector2*)positions.GetUnsafePtr();
        for (int i = 0; i < n; i++)
        {
            posPtr[i] = new Vector2(i, 0f);
        }

        using var ecb = new EntityCommandBuffer(initialCapacity: 64 * 1024);
        var writer = ecb.AsParallelWriter();

        for (int frame = 0; frame < iterations; frame++)
        {
            var job = new CreateEntityJob
            {
                Ecb = writer,
                Positions = new NativeArrayView(posPtr, n),
            };
            Job.ScheduleParallelFor(job, length: n).Complete();

            int created = ecb.Playback();
            if (created != n)
                throw new InvalidOperationException($"frame {frame}: Playback created {created}, expected {n}");

            DestroySpawned(ecb, created);
            ecb.Clear();

            // After Clear(), counts must reset cleanly.
            if (ecb.EntityCount != 0 || ecb.CommandCount != 0)
                throw new InvalidOperationException(
                    $"frame {frame}: ECB counts not zero after Clear() (entities={ecb.EntityCount}, commands={ecb.CommandCount})");
        }
    }

    private void Case_SingleThreadPathIntact(int n)
    {
        // Drives the recorder WITHOUT calling AsParallelWriter so the
        // single-threaded code path in Playback() is exercised. Catches
        // regressions where the refactor broke the no-slots branch.
        using var ecb = new EntityCommandBuffer(initialCapacity: 64 * 1024);
        for (int i = 0; i < n; i++)
        {
            EntityRef e = ecb.Create();
            ecb.AddComponent(e, new NativeTransform2D { LocalPosition = new Vector2(i, 0f) });
        }

        int created = ecb.Playback();
        if (created != n)
            throw new InvalidOperationException($"single-thread Playback created {created}, expected {n}");

        // Spot-check the first and last entity since this path
        // preserves recording order 1:1 with creation order.
        Entity first = ecb.GetCreatedEntity(0);
        ref NativeTransform2D firstTr = ref first.GetRef<NativeTransform2D>();
        if (Unsafe_IsNullRef(ref firstTr) || !Approx(firstTr.LocalPosition.X, 0f))
            throw new InvalidOperationException(
                $"single-thread: entity 0 LocalPosition.X = {(Unsafe_IsNullRef(ref firstTr) ? "null" : firstTr.LocalPosition.X.ToString())}, expected 0");

        Entity last = ecb.GetCreatedEntity(n - 1);
        ref NativeTransform2D lastTr = ref last.GetRef<NativeTransform2D>();
        if (Unsafe_IsNullRef(ref lastTr) || !Approx(lastTr.LocalPosition.X, n - 1))
            throw new InvalidOperationException(
                $"single-thread: entity {n - 1} LocalPosition.X = {(Unsafe_IsNullRef(ref lastTr) ? "null" : lastTr.LocalPosition.X.ToString())}, expected {n - 1}");

        DestroySpawned(ecb, created);
    }

    // ── Harness helpers ─────────────────────────────────────────────

    private void Case(string name, Action body)
    {
        m_TotalCases++;
        try
        {
            body();
            m_PassedCases++;
            Log.Info($"[CreateEntityJobSample] PASS  {name}");
        }
        catch (Exception ex)
        {
            Log.Error($"[CreateEntityJobSample] FAIL  {name}: {ex.Message}");
        }
    }

    private static void DestroySpawned(EntityCommandBuffer ecb, int count)
    {
        for (int i = 0; i < count; i++)
        {
            Entity e = ecb.GetCreatedEntity(i);
            if (e) e.Destroy();
        }
    }

    private static long PackVector(Vector2 v)
    {
        // BitConverter-style pack of two floats into a u64 so HashSet
        // membership tests treat NaN-bit-identical values as identical.
        uint hx = (uint)BitConverter.SingleToInt32Bits(v.X);
        uint hy = (uint)BitConverter.SingleToInt32Bits(v.Y);
        return ((long)hx << 32) | hy;
    }

    private static bool Approx(float a, float b)
    {
        const float Epsilon = 1e-4f;
        return Math.Abs(a - b) < Epsilon;
    }

    // System.Runtime.CompilerServices.Unsafe.IsNullRef is the actual API,
    // but a small wrapper keeps the call sites readable.
    private static bool Unsafe_IsNullRef<T>(ref T value)
        => System.Runtime.CompilerServices.Unsafe.IsNullRef(ref value);
}
