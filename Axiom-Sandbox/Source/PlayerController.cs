using Axiom;

public class PlayerController : EntityScript
{
    private float speed = 5.0f;

    public override void OnStart()
    {
        Log.Info("PlayerController attached to: " + Entity.Name);
    }

    public override void OnUpdate()
    {
        var velocity = Vector2.Zero;

        if (Input.GetKey(KeyCode.W) || Input.GetKey(KeyCode.Up))
            velocity.Y += 1.0f;
        if (Input.GetKey(KeyCode.S) || Input.GetKey(KeyCode.Down))
            velocity.Y -= 1.0f;
        if (Input.GetKey(KeyCode.A) || Input.GetKey(KeyCode.Left))
            velocity.X -= 1.0f;
        if (Input.GetKey(KeyCode.D) || Input.GetKey(KeyCode.Right))
            velocity.X += 1.0f;

        if (velocity == Vector2.Zero) return;

        // ECS ref-API: TransformRef returns `ref NativeTransform2D` (the struct
        // in Axiom.Components) pointing directly at this entity's slot in the
        // EnTT pool. The += writes through to native storage with zero
        // per-property P/Invoke. Writes go to LocalPosition because
        // TransformHierarchySystem recomputes world Position from it each
        // frame; touching the world cache directly would be overwritten.
        Entity.TransformRef.LocalPosition += velocity.Normalized() * speed * Time.DeltaTime;
    }
}
