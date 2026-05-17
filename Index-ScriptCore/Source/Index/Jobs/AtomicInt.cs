using System.Threading;

namespace Index.Jobs;

/// <summary>
/// Reference-typed atomic 32-bit integer for sharing state across parallel jobs.
/// <para>
/// IJob / IJobParallelFor structs are copied per partition, so plain int fields
/// inside them are not shared. Capture an <see cref="AtomicInt"/> by reference to
/// get a single counter every worker mutates.
/// </para>
/// </summary>
public sealed class AtomicInt
{
    private int m_Value;

    public AtomicInt()
    {
    }

    public AtomicInt(int initialValue)
    {
        m_Value = initialValue;
    }

    public int Value
    {
        get => Volatile.Read(ref m_Value);
        set => Volatile.Write(ref m_Value, value);
    }

    public int Load() => Volatile.Read(ref m_Value);

    public void Store(int value) => Volatile.Write(ref m_Value, value);

    public int Add(int operand) => Interlocked.Add(ref m_Value, operand);

    public int Sub(int operand) => Interlocked.Add(ref m_Value, -operand);

    public int Increment() => Interlocked.Increment(ref m_Value);

    public int Decrement() => Interlocked.Decrement(ref m_Value);

    public int Exchange(int newValue) => Interlocked.Exchange(ref m_Value, newValue);

    public int CompareExchange(int newValue, int comparand)
        => Interlocked.CompareExchange(ref m_Value, newValue, comparand);

    public int Or(int operand) => Interlocked.Or(ref m_Value, operand);

    public int And(int operand) => Interlocked.And(ref m_Value, operand);

    public int FetchMax(int operand)
    {
        int current = Volatile.Read(ref m_Value);
        while (operand > current)
        {
            int prev = Interlocked.CompareExchange(ref m_Value, operand, current);
            if (prev == current)
            {
                return prev;
            }
            current = prev;
        }
        return current;
    }

    public int FetchMin(int operand)
    {
        int current = Volatile.Read(ref m_Value);
        while (operand < current)
        {
            int prev = Interlocked.CompareExchange(ref m_Value, operand, current);
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
