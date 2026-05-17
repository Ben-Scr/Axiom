using System.Threading;

namespace Index.Jobs;

/// <summary>
/// Reference-typed atomic boolean backed by a 32-bit int (0 = false, 1 = true).
/// Useful as a cancel/abort flag visible to every worker in a parallel job.
/// </summary>
public sealed class AtomicBool
{
    private int m_Value;

    public AtomicBool()
    {
    }

    public AtomicBool(bool initialValue)
    {
        m_Value = initialValue ? 1 : 0;
    }

    public bool Value
    {
        get => Volatile.Read(ref m_Value) != 0;
        set => Volatile.Write(ref m_Value, value ? 1 : 0);
    }

    public bool Load() => Volatile.Read(ref m_Value) != 0;

    public void Store(bool value) => Volatile.Write(ref m_Value, value ? 1 : 0);

    public bool Exchange(bool newValue)
        => Interlocked.Exchange(ref m_Value, newValue ? 1 : 0) != 0;

    public bool CompareExchange(bool newValue, bool comparand)
        => Interlocked.CompareExchange(ref m_Value, newValue ? 1 : 0, comparand ? 1 : 0) != 0;

    /// <summary>
    /// Atomically transitions false -> true. Returns true if this call made the transition.
    /// </summary>
    public bool TrySet() => Interlocked.CompareExchange(ref m_Value, 1, 0) == 0;

    /// <summary>
    /// Atomically transitions true -> false. Returns true if this call made the transition.
    /// </summary>
    public bool TryClear() => Interlocked.CompareExchange(ref m_Value, 0, 1) == 1;

    public override string ToString() => Load() ? "true" : "false";
}
