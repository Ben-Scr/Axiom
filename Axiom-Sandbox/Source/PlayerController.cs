using Axiom;

public class PlayerController : EntityScript
{
    private float speed = 5.0f;

    // E26: override the documented OnStart/OnUpdate surface from EntityScript
    public override void OnStart()
    {
        Log.Info("PlayerController attached to: " + Entity.Name);
    }

    // E26: override the documented OnStart/OnUpdate surface from EntityScript
    public override void OnUpdate()
    {
        // Test 5
        var velocity = Vector2.One;

        if (Input.GetKey(KeyCode.W) || Input.GetKey(KeyCode.Up))
            velocity.Y += 1.0f;
        if (Input.GetKey(KeyCode.S) || Input.GetKey(KeyCode.Down))
            velocity.Y -= 1.0f;
        if (Input.GetKey(KeyCode.A) || Input.GetKey(KeyCode.Left))
            velocity.X -= 1.0f;
        if (Input.GetKey(KeyCode.D) || Input.GetKey(KeyCode.Right))
            velocity.X += 2.0f;

        if (velocity != Vector2.Zero)
        {
            velocity = velocity.Normalized();
            Entity.Transform.Position += velocity * speed * Time.DeltaTime;
        }
    }
}
