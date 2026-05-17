using System.Threading;

namespace Index.Jobs;

/// <summary>
/// Reference-typed atomic 64-bit integer for sharing state across parallel jobs.
/// Uses <see cref="Interlocked.Read(ref long)"/> for torn-read safety on 32-bit runtimes.
/// </summary>
public sealed class AtomicLong
{
    private long m_Value;

    public AtomicLong()
    {
    }

    public AtomicLong(long initialValue)
    {
        m_Value = initialValue;
    }

    public long Value
    {
        get => Interlocked.Read(ref m_Value);
        set => Interlocked.Exchange(ref m_Value, value);
    }

    public long Load() => Interlocked.Read(ref m_Value);

    public void Store(long value) => Interlocked.Exchange(ref m_Value, value);

    public long Add(long operand) => Interlocked.Add(ref m_Value, operand);

    public long Sub(long operand) => Interlocked.Add(ref m_Value, -operand);

    public long Increment() => Interlocked.Increment(ref m_Value);

    public long Decrement() => Interlocked.Decrement(ref m_Value);

    public long Exchange(long newValue) => Interlocked.Exchange(ref m_Value, newValue);

    public long CompareExchange(long newValue, long comparand)
        => Interlocked.CompareExchange(ref m_Value, newValue, comparand);

    public long Or(long operand) => Interlocked.Or(ref m_Value, operand);

    public long And(long operand) => Interlocked.And(ref m_Value, operand);

    public long FetchMax(long operand)
    {
        long current = Interlocked.Read(ref m_Value);
        while (operand > current)
        {
            long prev = Interlocked.CompareExchange(ref m_Value, operand, current);
            if (prev == current)
            {
                return prev;
            }
            current = prev;
        }
        return current;
    }

    public long FetchMin(long operand)
    {
        long current = Interlocked.Read(ref m_Value);
        while (operand < current)
        {
            long prev = Interlocked.CompareExchange(ref m_Value, operand, current);
            if (prev == current)
            {
                return prev;
            }
            current = prev;
        }
        return current;
    }

    public override string ToString() => Load().ToString();
}
