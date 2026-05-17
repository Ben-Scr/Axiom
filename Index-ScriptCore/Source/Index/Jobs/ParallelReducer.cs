using System;
using System.Threading;

namespace Index.Jobs;

/// <summary>
/// Per-thread accumulator that merges into a single result on demand.
/// <para>
/// Prefer this over a hot <see cref="AtomicInt"/> / <see cref="AtomicFloat"/>
/// in the inner loop of an <c>IJobParallelFor</c>: every worker mutates its own
/// thread-local cell with no contention, and the final reduce happens once.
/// </para>
/// <example>
/// <code>
/// var sum = ParallelReducer.SumInt();
/// Job.ParallelFor(values.Length, i => sum.Add(values[i]));
/// int total = sum.Result();
/// </code>
/// </example>
/// </summary>
public sealed class ParallelReducer<T> : IDisposable
{
    private readonly Func<T, T, T> m_Combine;
    private readonly T m_Identity;
    private readonly ThreadLocal<StrongBox> m_Local;

    public ParallelReducer(T identity, Func<T, T, T> combine)
    {
        ArgumentNullException.ThrowIfNull(combine);
        m_Identity = identity;
        m_Combine = combine;
        m_Local = new ThreadLocal<StrongBox>(() => new StrongBox(identity), trackAllValues: true);
    }

    /// <summary>
    /// Folds <paramref name="value"/> into the calling thread's cell.
    /// Thread-safe by virtue of being thread-local; no atomics needed.
    /// </summary>
    public void Add(T value)
    {
        StrongBox box = m_Local.Value!;
        box.Value = m_Combine(box.Value, value);
    }

    /// <summary>
    /// Collapses all per-thread cells into one result using the combiner.
    /// Call this after the parallel job has completed.
    /// </summary>
    public T Result()
    {
        T acc = m_Identity;
        foreach (StrongBox box in m_Local.Values)
        {
            acc = m_Combine(acc, box.Value);
        }
        return acc;
    }

    /// <summary>
    /// Resets every thread-local cell back to the identity value.
    /// </summary>
    public void Reset()
    {
        foreach (StrongBox box in m_Local.Values)
        {
            box.Value = m_Identity;
        }
    }

    public void Dispose() => m_Local.Dispose();

    private sealed class StrongBox
    {
        public T Value;

        public StrongBox(T value)
        {
            Value = value;
        }
    }
}

/// <summary>
/// Convenience factories for the common reduction shapes.
/// </summary>
public static class ParallelReducer
{
    public static ParallelReducer<int> SumInt()
        => new ParallelReducer<int>(0, static (a, b) => a + b);

    public static ParallelReducer<long> SumLong()
        => new ParallelReducer<long>(0L, static (a, b) => a + b);

    public static ParallelReducer<float> SumFloat()
        => new ParallelReducer<float>(0f, static (a, b) => a + b);

    public static ParallelReducer<double> SumDouble()
        => new ParallelReducer<double>(0d, static (a, b) => a + b);

    public static ParallelReducer<int> MinInt()
        => new ParallelReducer<int>(int.MaxValue, static (a, b) => a < b ? a : b);

    public static ParallelReducer<int> MaxInt()
        => new ParallelReducer<int>(int.MinValue, static (a, b) => a > b ? a : b);

    public static ParallelReducer<float> MinFloat()
        => new ParallelReducer<float>(float.PositiveInfinity, static (a, b) => a < b ? a : b);

    public static ParallelReducer<float> MaxFloat()
        => new ParallelReducer<float>(float.NegativeInfinity, static (a, b) => a > b ? a : b);
}
