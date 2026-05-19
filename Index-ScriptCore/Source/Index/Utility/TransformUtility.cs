
namespace Index;

public static class TransformUtility
{
    private const float FullCircleDegrees = 360.0f;
    private const float HalfCircleDegrees = 180.0f;

    /// <summary>
    /// Calculates the rotation angle (in degrees) for <paramref name="origin"/>'s up axis
    /// (its visual forward in 2D) to face <paramref name="target"/>.
    /// </summary>
    /// <param name="origin">The position of the entity that should rotate.</param>
    /// <param name="target">The position to rotate towards.</param>
    /// <returns>The rotation angle in degrees. Assign to Transform2D.Rotation.</returns>
    public static float LookAt2D(Vector2 origin, Vector2 target)
    {
        return LookAt2DRadians(origin, target) * Mathf.Rad2Deg;
    }

    /// <summary>
    /// Calculates the rotation angle (in degrees) for <paramref name="origin"/>'s up axis
    /// to face <paramref name="target"/>, interpolated from 0 degrees.
    /// Use the Transform2D overload to interpolate from the current rotation.
    /// </summary>
    public static float LookAt2D(Vector2 origin, Vector2 target, float lerp)
    {
        return LerpAngleDegrees(0.0f, LookAt2D(origin, target), lerp);
    }

    /// <summary>
    /// Calculates the rotation angle (in degrees) for <paramref name="origin"/>'s up axis
    /// to face <paramref name="target"/>, interpolated from the current rotation.
    /// </summary>
    /// <param name="lerp">Interpolation factor between 0 (keep current rotation) and 1 (instant).</param>
    /// <returns>The interpolated rotation angle in degrees. Assign to Transform2D.Rotation.</returns>
    public static float LookAt2D(Transform2D origin, Vector2 target, float lerp = 1.0f)
    {
        if (origin == null) throw new ArgumentNullException(nameof(origin));

        if (!TryLookAt2DRadians(origin.Position, target, out float targetRotation))
            return origin.RotationDegrees;

        return LerpAngleDegrees(origin.RotationDegrees, targetRotation * Mathf.Rad2Deg, lerp);
    }

    /// <summary>
    /// Calculates the rotation angle (in radians) for <paramref name="origin"/>'s up axis to face <paramref name="target"/>.
    /// </summary>
    public static float LookAt2DRadians(Vector2 origin, Vector2 target)
    {
        return TryLookAt2DRadians(origin, target, out float rotation) ? rotation : 0.0f;
    }

    /// <summary>
    /// Calculates the rotation angle (in radians) for <paramref name="origin"/>'s up axis
    /// to face <paramref name="target"/>, interpolated from 0 radians.
    /// </summary>
    public static float LookAt2DRadians(Vector2 origin, Vector2 target, float lerp)
    {
        return LerpAngleRadians(0.0f, LookAt2DRadians(origin, target), lerp);
    }

    /// <summary>
    /// Calculates the rotation angle (in radians) for <paramref name="origin"/>'s up axis
    /// to face <paramref name="target"/>, interpolated from the current rotation.
    /// </summary>
    public static float LookAt2DRadians(Transform2D origin, Vector2 target, float lerp = 1.0f)
    {
        if (origin == null) throw new ArgumentNullException(nameof(origin));

        if (!TryLookAt2DRadians(origin.Position, target, out float targetRotation))
            return origin.Rotation * Mathf.Deg2Rad;

        return LerpAngleRadians(origin.Rotation * Mathf.Deg2Rad, targetRotation, lerp);
    }

    /// <summary>
    /// Calculates the rotation angle (in degrees) for <paramref name="origin"/>'s right axis to face <paramref name="target"/>.
    /// Use this only when your sprite art is authored facing right; prefer <see cref="LookAt2D(Vector2, Vector2)"/> otherwise.
    /// </summary>
    public static float LookAtRight2D(Vector2 origin, Vector2 target)
    {
        return LookAtRight2DRadians(origin, target) * Mathf.Rad2Deg;
    }

    /// <summary>
    /// Calculates the rotation angle (in degrees) for <paramref name="origin"/>'s right axis to face <paramref name="target"/>,
    /// interpolated from the current rotation.
    /// </summary>
    public static float LookAtRight2D(Transform2D origin, Vector2 target, float lerp = 1.0f)
    {
        if (origin == null) throw new ArgumentNullException(nameof(origin));

        if (!TryLookAtRight2DRadians(origin.Position, target, out float targetRotation))
            return origin.RotationDegrees;

        return LerpAngleDegrees(origin.RotationDegrees, targetRotation * Mathf.Rad2Deg, lerp);
    }

    /// <summary>
    /// Calculates the rotation angle (in radians) for <paramref name="origin"/>'s right axis to face <paramref name="target"/>.
    /// </summary>
    public static float LookAtRight2DRadians(Vector2 origin, Vector2 target)
    {
        return TryLookAtRight2DRadians(origin, target, out float rotation) ? rotation : 0.0f;
    }

    /// <summary>
    /// Rotates <paramref name="origin"/> so its up axis (visual forward) faces <paramref name="target"/>.
    /// </summary>
    public static void Face2D(Transform2D origin, Vector2 target, float lerp = 1.0f)
    {
        if (origin == null) throw new ArgumentNullException(nameof(origin));
        origin.RotationDegrees = LookAt2D(origin, target, lerp);
    }

    /// <summary>
    /// Rotates <paramref name="origin"/> so its right axis faces <paramref name="target"/>.
    /// </summary>
    public static void FaceRight2D(Transform2D origin, Vector2 target, float lerp = 1.0f)
    {
        if (origin == null) throw new ArgumentNullException(nameof(origin));
        origin.RotationDegrees = LookAtRight2D(origin, target, lerp);
    }

    /// <summary>
    /// Returns a normalized direction from <paramref name="origin"/> to <paramref name="target"/>.
    /// </summary>
    public static Vector2 DirectionTo2D(Vector2 origin, Vector2 target)
    {
        Vector2 direction = target - origin;
        return direction.LengthSquared() > Mathf.Epsilon * Mathf.Epsilon
            ? direction.Normalized()
            : Vector2.Zero;
    }

    /// <summary>
    /// Rotates <paramref name="point"/> around <paramref name="pivot"/> by <paramref name="degrees"/>.
    /// </summary>
    public static Vector2 RotatePoint2D(Vector2 point, Vector2 pivot, float degrees)
    {
        return RotatePoint2DRadians(point, pivot, degrees * Mathf.Deg2Rad);
    }

    /// <summary>
    /// Rotates <paramref name="point"/> around <paramref name="pivot"/> by <paramref name="radians"/>.
    /// </summary>
    public static Vector2 RotatePoint2DRadians(Vector2 point, Vector2 pivot, float radians)
    {
        float sin = Mathf.Sin(radians);
        float cos = Mathf.Cos(radians);
        Vector2 offset = point - pivot;

        return new Vector2(
            offset.X * cos - offset.Y * sin + pivot.X,
            offset.X * sin + offset.Y * cos + pivot.Y);
    }

    /// <summary>
    /// Interpolates between angles in degrees using the shortest path.
    /// </summary>
    public static float LerpAngleDegrees(float current, float target, float lerp)
    {
        return current + DeltaAngleDegrees(current, target) * Mathf.Clamp01(lerp);
    }

    /// <summary>
    /// Interpolates between angles in radians using the shortest path.
    /// </summary>
    public static float LerpAngleRadians(float current, float target, float lerp)
    {
        return current + DeltaAngleRadians(current, target) * Mathf.Clamp01(lerp);
    }

    /// <summary>
    /// Returns the shortest signed difference from <paramref name="current"/> to <paramref name="target"/> in degrees.
    /// </summary>
    public static float DeltaAngleDegrees(float current, float target)
    {
        float delta = Repeat(target - current, FullCircleDegrees);
        if (delta > HalfCircleDegrees) delta -= FullCircleDegrees;
        return delta;
    }

    /// <summary>
    /// Returns the shortest signed difference from <paramref name="current"/> to <paramref name="target"/> in radians.
    /// </summary>
    public static float DeltaAngleRadians(float current, float target)
    {
        float delta = Repeat(target - current, Mathf.TwoPI);
        if (delta > Mathf.PI) delta -= Mathf.TwoPI;
        return delta;
    }

    /// <summary>
    /// Calculates the XY-plane rotation angle (in degrees) from <paramref name="origin"/> to <paramref name="target"/>.
    /// </summary>
    public static float LookAt(Vector3 origin, Vector3 target, float lerp)
    {
        return LookAt2D(origin.XY, target.XY, lerp);
    }

    /// <summary>
    /// Calculates the XY-plane rotation angle (in degrees) from <paramref name="origin"/> to <paramref name="target"/>.
    /// </summary>
    public static float LookAt(Vector3 origin, Vector3 target)
    {
        return LookAt2D(origin.XY, target.XY);
    }

    /// <summary>
    /// Calculates the XY-plane rotation angle (in radians) from <paramref name="origin"/> to <paramref name="target"/>.
    /// </summary>
    public static float LookAtRadians(Vector3 origin, Vector3 target, float lerp)
    {
        return LookAt2DRadians(origin.XY, target.XY, lerp);
    }

    /// <summary>
    /// Calculates the XY-plane rotation angle (in radians) from <paramref name="origin"/> to <paramref name="target"/>.
    /// </summary>
    public static float LookAtRadians(Vector3 origin, Vector3 target)
    {
        return LookAt2DRadians(origin.XY, target.XY);
    }

    // Origin's Up axis (-sin θ, cos θ) faces (dx, dy) when sin θ = -dx/|d|, cos θ = dy/|d|
    // i.e. θ = atan2(-dx, dy).
    private static bool TryLookAt2DRadians(Vector2 origin, Vector2 target, out float rotation)
    {
        Vector2 direction = target - origin;
        if (direction.LengthSquared() <= Mathf.Epsilon * Mathf.Epsilon)
        {
            rotation = 0.0f;
            return false;
        }

        rotation = Mathf.Atan2(-direction.X, direction.Y);
        return true;
    }

    // Origin's Right axis (cos θ, sin θ) faces (dx, dy) when θ = atan2(dy, dx).
    private static bool TryLookAtRight2DRadians(Vector2 origin, Vector2 target, out float rotation)
    {
        Vector2 direction = target - origin;
        if (direction.LengthSquared() <= Mathf.Epsilon * Mathf.Epsilon)
        {
            rotation = 0.0f;
            return false;
        }

        rotation = Mathf.Atan2(direction.Y, direction.X);
        return true;
    }

    private static float Repeat(float value, float length)
    {
        return value - Mathf.Floor(value / length) * length;
    }
}
