using Axiom;
// Per-file aliases pick the ref-API forms of these types without bringing in
// every name from `Axiom.Components` (which would clash with the same-named
// class wrappers in `Axiom`). Users opt into the ref API per script.
using Transform2D    = Axiom.Components.Transform2D;
using SpriteRenderer = Axiom.Components.SpriteRenderer;

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

        // Single-component query — `Current` is `ref Transform2D` so the
        // foreach binds directly with `ref var`. Compound assignment writes
        // through to the EnTT pool slot.
        foreach (ref var t in Scene.QueryRef<Transform2D>())
        {
            t.LocalRotation += SpinSpeed * dt;
        }

        // Multi-component query, direct-ref access via `row.W` / `row.R`.
        // Visits every entity that has BOTH Transform2D and SpriteRenderer,
        // fading the alpha while spinning. Cleaner for hot loops than the
        // deconstruct form below.
        foreach (var row in Scene.QueryRef<Transform2D>().Readonly<SpriteRenderer>())
        {
            row.W.LocalRotation += row.R.SortingOrder * dt;
        }

        // Same query, deconstruct shape — equivalent compiled output, closer
        // to the legacy class-API look. Pick whichever reads better at the
        // call site.
        foreach (var (tr, sr) in Scene.QueryRef<Transform2D>().Readonly<SpriteRenderer>())
        {
            tr.Value.LocalScale = new Vector2(sr.Value.Color.A);
        }
    }
}
