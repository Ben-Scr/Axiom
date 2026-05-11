using Axiom;
using Axiom.Components;
// The Native* structs in Axiom.Components live alongside the managed
// `Axiom.Transform2D` / `Axiom.SpriteRenderer` class wrappers. Names are
// disambiguated by the `Native` prefix, so a single `using Axiom.Components;`
// is enough — no per-file aliases. Mix freely: managed class for cold paths,
// Native struct for hot loops in this system.

// Demonstrates the ECS ref-API query system. Inactive until the user adds it
// to a scene via the editor's GameSystem inspector. Shows the two iteration
// shapes for multi-component rows; either compiles to direct pool writes with
// one P/Invoke per OnUpdate.
public class SpinSystem : GameSystem
{
    [ShowInEditor("Spin Speed (rad/s)")] public float SpinSpeed = 1.0f;

    public override void OnUpdate()
    {
        float dt = Time.DeltaTime;

        // Single-component query — `Current` is `ref NativeTransform2D` so the
        // foreach binds directly with `ref var`. Compound assignment writes
        // through to the EnTT pool slot.
        foreach (ref var t in Scene.QueryRef<NativeTransform2D>())
        {
            t.LocalRotation += SpinSpeed * dt;
        }

        // Multi-component query, direct-ref access via `row.W` / `row.R`.
        // Visits every entity that has BOTH NativeTransform2D and NativeSpriteRenderer,
        // fading the alpha while spinning. Cleaner for hot loops than the
        // deconstruct form below.
        foreach (var row in Scene.QueryRef<NativeTransform2D>().Readonly<NativeSpriteRenderer>())
        {
            row.W.LocalRotation += row.R.SortingOrder * dt;
        }

        // Same query, deconstruct shape — equivalent compiled output, closer
        // to the legacy class-API look. Pick whichever reads better at the
        // call site.
        foreach (var (tr, sr) in Scene.QueryRef<NativeTransform2D>().Readonly<NativeSpriteRenderer>())
        {
            tr.Value.LocalScale = new Vector2(sr.Value.Color.A);
        }
    }
}
