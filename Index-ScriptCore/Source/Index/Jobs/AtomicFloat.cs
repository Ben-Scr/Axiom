using System.Threading;

namespace Index.Jobs;

/// <summary>
/// Reference-typed atomic 32-bit float. Arithmetic is implemented as a CAS loop on
/// <see cref="Interlocked.CompareExchange(ref float, float, float)"/>; expect modest
/// contention overhead compared to int counters.
/// </summary>
public sealed class AtomicFloat
{
    private float m_Value;

    public AtomicFloat()
    {
    }

    public AtomicFloat(float initialValue)
    {
        m_Value = initialValue;
    }

    public float Value
    {
        get => Volatile.Read(ref m_Value);
        set => Volatile.Write(ref m_Value, value);
    }

    public float Load() => Volatile.Read(ref m_Value);

    public void Store(float value) => Volatile.Write(ref m_Value, value);

    public float Exchange(float newValue) => Interlocked.Exchange(ref m_Value, newValue);

    public float CompareExchange(float newValue, float comparand)
        => Interlocked.CompareExchange(ref m_Value, newValue, comparand);

    public float Add(float operand)
    {
        float current = Volatile.Read(ref m_Value);
        while (true)
        {
            float updated = current + operand;
            float prev = Interlocked.CompareExchange(ref m_Value, updated, current);
            if (prev.Equals(current))
            {
                return updated;
            }
            current = prev;
        }
    }

    public float Sub(float operand) => Add(-operand);

    public float FetchMax(float operand)
    {
        float current = Volatile.Read(ref m_Value);
        while (operand > current)
        {
            float prev = Interlocked.CompareExchange(ref m_Value, operand, current);
            if (prev.Equals(current))
            {
                return prev;
            }
            current = prev;
        }
        return current;
    }

    public float FetchMin(float operand)
    {
        float current = Volatile.Read(ref m_Value);
        while (operand < current)
        {
            float prev = Interlocked.CompareExchange(ref m_Value, operand, current);
            if (prev.Equals(current))
            {
                return prev;
            }
            current = prev;
        }
        return current;
    }

    public override string ToString() => Load().ToString();
}
