using System.Threading;

namespace Index.Jobs;

/// <summary>
/// Reference-typed atomic slot for a reference value. Useful for "first writer wins"
/// patterns across parallel jobs, e.g. recording the first match found.
/// </summary>
public sealed class AtomicRef<T> where T : class
{
    private T? m_Value;

    public AtomicRef()
    {
    }

    public AtomicRef(T? initialValue)
    {
        m_Value = initialValue;
    }

    public T? Value
    {
        get => Volatile.Read(ref m_Value);
        set => Volatile.Write(ref m_Value, value);
    }

    public T? Load() => Volatile.Read(ref m_Value);

    public void Store(T? value) => Volatile.Write(ref m_Value, value);

    public T? Exchange(T? newValue) => Interlocked.Exchange(ref m_Value, newValue);

    public T? CompareExchange(T? newValue, T? comparand)
        => Interlocked.CompareExchange(ref m_Value, newValue, comparand);

    /// <summary>
    /// Sets the slot if it is currently null. Returns true if this call performed the write.
    /// </summary>
    public bool TrySetIfNull(T value)
        => Interlocked.CompareExchange(ref m_Value, value, null) == null;
}
