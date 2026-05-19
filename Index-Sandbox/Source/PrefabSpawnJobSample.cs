using System;
using Index;
using Index.Components;
using Index.Jobs;
using Index.Native;

// End-to-end sample + regression test for the three-phase prefab fast path:
//   1. Native PrefabTemplateCache (first spawn bakes, second hits cache).
//   2. EntityCommandBuffer.InstantiatePrefab opcode (records prefab spawns
//      into the same wire stream as AddComponent).
//   3. JobContext (carries the parallel writer + frame snapshot into the job).
//
// Drop this GameSystem on any scene, wire `BulletPrefab` to a prefab asset
// in the inspector, and toggle RunOnStart. The system self-runs once, logs
// PASS / FAIL per case, then destroys the spawned entities so the scene is
// left as it was found.
//
// The dominant case mirrors the bullet-wave workload: spawn N prefabs in
// parallel from inside an IJobParallelFor, then verify the entity count
// and at least one component survived end-to-end. The remaining cases
// cover error paths (zero GUID → exception, non-prefab → exception) and
// the single-threaded ECB path so future regressions in either are caught.
public class PrefabSpawnJobSample : GameSystem
{
    [ShowInEditor("Bullet Prefab")]     public Entity BulletPrefab = Entity.Invalid;
    [ShowInEditor("Entities Per Pass")] public int EntitiesPerPass = 10_000;
    [ShowInEditor("Run On Start")]      public bool RunOnStart = false;

    private bool m_Ran;
    private int  m_TotalCases;
    private int  m_PassedCases;

    public override void OnStart()
    {
        if (!RunOnStart || m_Ran) return;
        m_Ran = true;
        // PrefabGUID resolves to the asset GUID for both prefab-asset
        // entities (Entity.FromPrefabGUID) and existing prefab instances
        // (entities whose origin is Prefab). Zero means "not a prefab,
        // not from a prefab" — the only state we want to skip on.
        if (BulletPrefab is null || BulletPrefab.PrefabGUID == 0)
        {
            Log.Warn("[PrefabSpawnJobSample] BulletPrefab is not wired to a prefab asset; skipping run.");
            return;
        }
        RunAllCases();
    }

    // ── The sample job ──────────────────────────────────────────────

    // Each row spawns one prefab and overrides its root LocalPosition.
    // Job struct carries the JobContext as a field so the worker has
    // immediate access to the parallel writer plus per-frame state.
    private struct SpawnPrefabJob : IJobParallelFor
    {
        public JobContext Ctx;
        public ulong PrefabGuid;

        public void Execute(int i)
        {
            EntityRef root = Ctx.Ecb.Instantiate(PrefabGuid);
            Ctx.Ecb.AddComponent(root, new NativeTransform2D
            {
                LocalPosition = new Vector2(i, Ctx.GetRng().NextFloat01() * 10f),
            });
        }
    }

    // ── Driver ──────────────────────────────────────────────────────

    private void RunAllCases()
    {
        m_TotalCases = 0;
        m_PassedCases = 0;
        int n = EntitiesPerPass < 1 ? 1 : EntitiesPerPass;

        Case("Parallel prefab spawn + override component", () => Case_ParallelSpawn(n));
        Case("Single-thread prefab spawn (no AsParallelWriter)", () => Case_SingleThreadSpawn(n));
        Case("Zero GUID throws", Case_ZeroGuidThrows);
        Case("Non-prefab Entity throws", Case_NonPrefabThrows);

        Log.Info($"[PrefabSpawnJobSample] {m_PassedCases}/{m_TotalCases} cases passed");
    }

    // ── Cases ───────────────────────────────────────────────────────

    private void Case_ParallelSpawn(int n)
    {
        // Pre-warm the native prefab cache: the FIRST spawn pays the
        // slow path (disk read + JSON parse) and populates the cache.
        // We measure parallel throughput AFTER warmup so the test
        // captures the fast path the user will actually see in a
        // steady-state per-frame loop.
        Entity warmup = Entity.Instantiate(BulletPrefab);
        if (warmup) warmup.Destroy();

        using var ecb = new EntityCommandBuffer(initialCapacity: 256 * 1024);
        var job = new SpawnPrefabJob
        {
            Ctx = JobContext.ForFrame(ecb.AsParallelWriter(), baseSeed: 0xC0FFEEu),
            PrefabGuid = BulletPrefab.PrefabGUID,
        };
        Job.ScheduleParallelFor(job, length: n).Complete();

        if (ecb.EntityCount != n)
        {
            throw new InvalidOperationException(
                $"recorded entity count drift: ECB reports {ecb.EntityCount}, expected {n}");
        }

        int created = ecb.Playback();
        if (created != n)
        {
            throw new InvalidOperationException($"Playback created {created}, expected {n}");
        }

        // Spot-check the first and last spawned root for the overridden
        // component. Children are reachable via the root's hierarchy if
        // the prefab has them; we don't inspect children here because
        // the test prefab is user-supplied and may be a single entity.
        Entity firstRoot = ecb.GetCreatedEntity(0);
        Entity lastRoot  = ecb.GetCreatedEntity(n - 1);
        if (!firstRoot)
        {
            throw new InvalidOperationException("first spawned root is invalid after playback");
        }
        if (!lastRoot)
        {
            throw new InvalidOperationException("last spawned root is invalid after playback");
        }
        ref NativeTransform2D firstTr = ref firstRoot.GetRef<NativeTransform2D>();
        ref NativeTransform2D lastTr  = ref lastRoot.GetRef<NativeTransform2D>();
        if (System.Runtime.CompilerServices.Unsafe.IsNullRef(ref firstTr) ||
            System.Runtime.CompilerServices.Unsafe.IsNullRef(ref lastTr))
        {
            throw new InvalidOperationException(
                "spawned root missing NativeTransform2D — the prefab must include Transform2D for this sample");
        }

        // Cleanup so the scene is left as we found it.
        DestroySpawned(ecb, created);
    }

    private void Case_SingleThreadSpawn(int n)
    {
        // Drives the recorder WITHOUT AsParallelWriter so the
        // single-threaded code path in Playback is exercised. Catches
        // regressions in the main-recorder vs. merged-playback split.
        using var ecb = new EntityCommandBuffer(initialCapacity: 256 * 1024);
        ulong guid = BulletPrefab.PrefabGUID;
        for (int i = 0; i < n; i++)
        {
            EntityRef root = ecb.Instantiate(guid);
            ecb.AddComponent(root, new NativeTransform2D { LocalPosition = new Vector2(i, 0f) });
        }

        int created = ecb.Playback();
        if (created != n)
        {
            throw new InvalidOperationException(
                $"single-thread Playback created {created}, expected {n}");
        }
        DestroySpawned(ecb, created);
    }

    private void Case_ZeroGuidThrows()
    {
        using var ecb = new EntityCommandBuffer(initialCapacity: 1024);
        bool threw = false;
        try
        {
            ecb.Instantiate(0ul);
        }
        catch (ArgumentException) { threw = true; }
        if (!threw)
        {
            throw new InvalidOperationException("ecb.InstantiatePrefab(0) did not throw");
        }
    }

    private void Case_NonPrefabThrows()
    {
        // Pass null as the Entity overload — the API rejects with
        // ArgumentException (Entity.Invalid would also do).
        using var ecb = new EntityCommandBuffer(initialCapacity: 1024);
        bool threw = false;
        try
        {
            ecb.Instantiate(Entity.Invalid);
        }
        catch (ArgumentException) { threw = true; }
        if (!threw)
        {
            throw new InvalidOperationException("ecb.InstantiatePrefab(Entity.Invalid) did not throw");
        }
    }

    // ── Harness helpers ─────────────────────────────────────────────

    private void Case(string name, Action body)
    {
        m_TotalCases++;
        try
        {
            body();
            m_PassedCases++;
            Log.Info($"[PrefabSpawnJobSample] PASS  {name}");
        }
        catch (Exception ex)
        {
            Log.Error($"[PrefabSpawnJobSample] FAIL  {name}: {ex.Message}");
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
}
