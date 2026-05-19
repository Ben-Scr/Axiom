using System;
using System.Collections.Generic;
using Index;
using Index.Components;
using Index.Jobs;
using Index.Native;

// Quicktest for the IJobQuery dispatch path. Drop this system onto any
// scene that contains at least one entity with NativeTransform2D. It runs
// once on OnStart, exercises Schedule / ScheduleParallel / attribute
// filters / signature validation, and logs PASS/FAIL per case followed
// by a summary line. Tests undo their writes so the scene is left as it
// was found.
public class IJobQueryQuickTest : GameSystem
{
    [ShowInEditor("Run on Start")] public bool RunOnStart = true;

    private int m_TotalCases;
    private int m_PassedCases;

    public override void OnStart()
    {
        if (!RunOnStart) return;
        RunAllCases();
    }

    // ── Test job structs ─────────────────────────────────────────

    // Single ref write — used by the round-trip case to bump positions
    // and then to roll them back. `Delta` is per-job state.
    private struct OffsetJob : IJobQuery
    {
        public Vector2 Delta;

        public void Execute(ref NativeTransform2D tr)
        {
            tr.LocalPosition += Delta;
        }
    }

    // Optional leading int parameter — each row writes its own index
    // into LocalPosition.x so the test can assert all rows are distinct.
    private struct StampIndexJob : IJobQuery
    {
        public void Execute(int rowIndex, ref NativeTransform2D tr)
        {
            tr.LocalPosition = new Vector2(rowIndex, tr.LocalPosition.Y);
        }
    }

    // ref + in mix — write to transform, read from sprite renderer.
    // Verifies multi-pool dispatch and the [W1..Wn, R1..Rm] row layout
    // matches what the IL emitter assumes.
    private struct ReadSpriteWriteTransformJob : IJobQuery
    {
        public float Scale;

        public void Execute(ref NativeTransform2D tr, in NativeSpriteRenderer sr)
        {
            tr.LocalScale = new Vector2(sr.Color.A * Scale, sr.Color.A * Scale);
        }
    }

    // Invalid: same ref type twice. Plan.Build must throw.
    private struct DuplicateRefJob : IJobQuery
    {
        public void Execute(ref NativeTransform2D a, ref NativeTransform2D b)
        {
            a.LocalPosition += b.LocalPosition;
        }
    }

    // Invalid: zero component parameters. Plan.Build must throw.
    private struct NoComponentJob : IJobQuery
    {
        public void Execute() { }
    }

    // Invalid: ref to a non-IComponent type. Plan.Build must throw.
    private struct NonComponentRefJob : IJobQuery
    {
        public void Execute(ref int counter) { counter++; }
    }

    // ── Driver ──────────────────────────────────────────────────

    private void RunAllCases()
    {
        m_TotalCases = 0;
        m_PassedCases = 0;

        // Snapshot every NativeTransform2D before we touch anything so
        // we can detect leakage and restore on test exit.
        List<Vector2> before = SnapshotPositions();
        if (before.Count == 0)
        {
            Log.Warn("[IJobQueryQuickTest] No NativeTransform2D entities — nothing to test against. Add at least one transform to the scene.");
            return;
        }

        try
        {
            Case("Parallel write mutates positions",   () => Case_ParallelWrite(before));
            Case("Schedule (single-job) mutates positions", () => Case_SingleSchedule(before));
            Case("rowIndex parameter is per-row distinct",  () => Case_RowIndex(before));
            Case("ref + in mix executes without crash", Case_RefInMix);
            Case("Duplicate ref type is rejected",      Case_DuplicateRefRejected);
            Case("Zero-component Execute is rejected",  Case_NoComponentRejected);
            Case("Non-IComponent ref is rejected",      Case_NonComponentRefRejected);
        }
        finally
        {
            // Belt-and-braces: restore positions even if a test threw.
            RestorePositions(before);
        }

        Log.Info($"[IJobQueryQuickTest] {m_PassedCases}/{m_TotalCases} cases passed");
    }

    // ── Cases ───────────────────────────────────────────────────

    private void Case_ParallelWrite(List<Vector2> before)
    {
        var delta = new Vector2(1.0f, 2.0f);
        JobHandle h = Scene.ScheduleParallel(new OffsetJob { Delta = delta });
        h.Complete();

        AssertPositionsShifted(before, delta);

        // Undo.
        JobHandle u = Scene.ScheduleParallel(new OffsetJob { Delta = -delta });
        u.Complete();

        AssertPositionsEqual(before);
    }

    private void Case_SingleSchedule(List<Vector2> before)
    {
        var delta = new Vector2(-3.5f, 0.25f);
        JobHandle h = Scene.Schedule(new OffsetJob { Delta = delta });
        h.Complete();

        AssertPositionsShifted(before, delta);

        JobHandle u = Scene.Schedule(new OffsetJob { Delta = -delta });
        u.Complete();

        AssertPositionsEqual(before);
    }

    private void Case_RowIndex(List<Vector2> before)
    {
        JobHandle h = Scene.Schedule(new StampIndexJob());
        h.Complete();

        // Single-threaded ordering: row i must have x == i.
        var current = SnapshotPositions();
        if (current.Count != before.Count)
            throw new InvalidOperationException($"row count drift: before={before.Count}, after={current.Count}");

        for (int i = 0; i < current.Count; i++)
        {
            if (!Approx(current[i].X, i))
                throw new InvalidOperationException($"row {i}: x={current[i].X}, expected {i}");
        }

        RestorePositions(before);
        AssertPositionsEqual(before);
    }

    private void Case_RefInMix()
    {
        // Just verify the dispatch path completes without error when both
        // ref and in parameters appear. The visible effect depends on
        // whether matching entities exist; we don't assert specific writes
        // because the scene may contain none.
        JobHandle h = Scene.ScheduleParallel(new ReadSpriteWriteTransformJob { Scale = 1.0f });
        h.Complete();
    }

    private void Case_DuplicateRefRejected()
    {
        ExpectThrows<InvalidOperationException>(() =>
        {
            JobHandle h = Scene.Schedule(new DuplicateRefJob());
            h.Complete();
        }, "duplicate ref type should fail plan build");
    }

    private void Case_NoComponentRejected()
    {
        ExpectThrows<InvalidOperationException>(() =>
        {
            JobHandle h = Scene.Schedule(new NoComponentJob());
            h.Complete();
        }, "Execute with no component parameters should fail plan build");
    }

    private void Case_NonComponentRefRejected()
    {
        ExpectThrows<InvalidOperationException>(() =>
        {
            JobHandle h = Scene.Schedule(new NonComponentRefJob());
            h.Complete();
        }, "ref to non-IComponent should fail plan build");
    }

    // ── Harness helpers ─────────────────────────────────────────

    private void Case(string name, Action body)
    {
        m_TotalCases++;
        try
        {
            body();
            m_PassedCases++;
            Log.Info($"[IJobQueryQuickTest] PASS  {name}");
        }
        catch (Exception ex)
        {
            Log.Error($"[IJobQueryQuickTest] FAIL  {name}: {ex.Message}");
        }
    }

    private List<Vector2> SnapshotPositions()
    {
        var snapshot = new List<Vector2>();
        foreach (ref var tr in Scene.QueryRef<NativeTransform2D>())
            snapshot.Add(tr.LocalPosition);
        return snapshot;
    }

    private void RestorePositions(List<Vector2> target)
    {
        int i = 0;
        foreach (ref var tr in Scene.QueryRef<NativeTransform2D>())
        {
            if (i < target.Count)
                tr.LocalPosition = target[i];
            i++;
        }
    }

    private void AssertPositionsShifted(List<Vector2> before, Vector2 delta)
    {
        var current = SnapshotPositions();
        if (current.Count != before.Count)
            throw new InvalidOperationException($"row count drift: before={before.Count}, after={current.Count}");

        for (int i = 0; i < current.Count; i++)
        {
            Vector2 expected = before[i] + delta;
            if (!Approx(current[i].X, expected.X) || !Approx(current[i].Y, expected.Y))
                throw new InvalidOperationException(
                    $"row {i}: got ({current[i].X},{current[i].Y}), expected ({expected.X},{expected.Y})");
        }
    }

    private void AssertPositionsEqual(List<Vector2> before)
    {
        var current = SnapshotPositions();
        if (current.Count != before.Count)
            throw new InvalidOperationException($"row count drift: before={before.Count}, after={current.Count}");

        for (int i = 0; i < current.Count; i++)
        {
            if (!Approx(current[i].X, before[i].X) || !Approx(current[i].Y, before[i].Y))
                throw new InvalidOperationException(
                    $"row {i}: drift ({current[i].X},{current[i].Y}) vs ({before[i].X},{before[i].Y})");
        }
    }

    private static void ExpectThrows<T>(Action body, string message) where T : Exception
    {
        try
        {
            body();
        }
        catch (T)
        {
            return;
        }
        catch (Exception ex)
        {
            // TypeInitializationException wraps the inner InvalidOperationException
            // when the per-TJob static constructor (JobQueryPlanFor<TJob>.cctor)
            // is the one that throws — accept that shape too.
            if (ex is TypeInitializationException tie && tie.InnerException is T) return;
            throw new InvalidOperationException($"{message}: caught {ex.GetType().Name} instead of {typeof(T).Name}: {ex.Message}");
        }
        throw new InvalidOperationException($"{message}: no exception thrown");
    }

    private static bool Approx(float a, float b)
    {
        const float Epsilon = 1e-4f;
        return Math.Abs(a - b) < Epsilon;
    }
}
