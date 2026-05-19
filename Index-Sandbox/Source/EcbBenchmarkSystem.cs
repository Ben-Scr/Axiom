using System.Diagnostics;
using Index;
using Index.Components;
using Index.Native;

// Single-shot benchmark + smoke test for EntityCommandBuffer. Add this
// GameSystem to a scene via the editor's GameSystem inspector; press Play
// once and read the timing block from the log. Self-destructs after the
// first run so re-entering Play doesn't spam the log or stack thousands of
// entities every press.
//
// Compares:
//   A. The classic per-entity path: N × Entity.Create + AddComponent.
//   B. The ECB path: one recording + one Playback.
//
// Expected ballpark on a desktop Ryzen at 10 000 entities × 2 components:
//   A: ~30–120 ms
//   B: ~0.3–2 ms
//   speedup ≈ 50–100×
public class EcbBenchmarkSystem : GameSystem
{
    [ShowInEditor("Entities Per Pass")] public int EntitiesPerPass = 10_000;
    [ShowInEditor("Run On Start")]      public bool RunOnStart = true;

    private bool _ran;

    public override void OnStart()
    {
        if (RunOnStart && !_ran)
        {
            _ran = true;
            Run();
        }
    }

    private void Run()
    {
        int n = EntitiesPerPass < 1 ? 1 : EntitiesPerPass;

        // Warm up the ECB's per-type metadata resolution (which runs the
        // ComponentTypes<T> static ctor) so its cost isn't billed against
        // the first sample. A single-entity ECB whose entries match the
        // benchmark's component set is the cheapest way to do this from
        // a user-assembly: ComponentTypes<T> itself is internal to the
        // ScriptCore assembly, but the ECB API surface that uses it is
        // public.
        using (var warmup = new EntityCommandBuffer(initialCapacity: 256))
        {
            var w = warmup.Create();
            warmup.AddComponent(w, new NativeTransform2D());
            warmup.AddComponent(w, new NativeSpriteRenderer());
            warmup.Playback();
        }

        // A: classic per-entity path.
        var sw = Stopwatch.StartNew();
        for (int i = 0; i < n; i++)
        {
            var e = Entity.Create("classic_" + i);
            e.AddComponent<Transform2D>();
            e.AddComponent<SpriteRenderer>();
        }
        sw.Stop();
        long classicMs = sw.ElapsedMilliseconds;
        long classicTicks = sw.ElapsedTicks;

        // B: ECB path.
        var ecb = new EntityCommandBuffer(initialCapacity: 64 * 1024);
        sw.Restart();
        for (int i = 0; i < n; i++)
        {
            var e = ecb.Create();
            ecb.AddComponent(e, new NativeTransform2D());
            ecb.AddComponent(e, new NativeSpriteRenderer());
        }
        int created = ecb.Playback();
        sw.Stop();
        long ecbMs = sw.ElapsedMilliseconds;
        long ecbTicks = sw.ElapsedTicks;

        double speedup = classicTicks > 0 && ecbTicks > 0
            ? (double)classicTicks / ecbTicks
            : 0.0;

        Log.Info($"[EcbBenchmark] N={n}");
        Log.Info($"[EcbBenchmark]   classic   : {classicMs} ms ({classicTicks} ticks)");
        Log.Info($"[EcbBenchmark]   ecb       : {ecbMs} ms ({ecbTicks} ticks) — {created} entities created");
        Log.Info($"[EcbBenchmark]   speedup   : {speedup:F1}x");

        // Regression smoke for the "non-trivially-copyable component
        // silently dropped" bug. SpriteRendererComponent holds a UUID with
        // a custom copy ctor, so the older auto-wire predicate
        // (`is_trivially_copyable_v<T>`) excluded it and Playback skipped
        // the AddComponent without raising. Pin both component types
        // explicitly so any future re-tightening of the predicate fails
        // here instead of leaving users to puzzle over an inspector that's
        // missing rows.
        if (created > 0)
        {
            Entity first = ecb.GetCreatedEntity(0);
            bool hasTransform = first.HasComponent<Transform2D>();
            bool hasSprite    = first.HasComponent<SpriteRenderer>();
            if (!hasTransform || !hasSprite)
            {
                Log.Error(
                    $"[EcbBenchmark] REGRESSION: first ECB-spawned entity is missing components " +
                    $"— Transform2D={hasTransform}, SpriteRenderer={hasSprite}. " +
                    "The ComponentRegistry emplaceFromBytes auto-wire likely re-acquired its " +
                    "is_trivially_copyable_v<T> gate; see the floating-dancing-pretzel plan.");
            }
            else
            {
                Log.Info("[EcbBenchmark]   smoke     : first entity has Transform2D + SpriteRenderer OK");
            }
        }

        ecb.Dispose();
    }
}
