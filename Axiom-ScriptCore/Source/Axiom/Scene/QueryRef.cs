using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using Axiom.Components;
using Axiom.Interop;

namespace Axiom;

// ECS ref-API query system. Yields raw component refs from EnTT storage with
// one native call per `foreach`, regardless of row count — replaces the
// per-entity managed-wrapper allocation + per-property P/Invoke that the
// legacy `Scene.Query<T>()` paid.
//
// Iteration-stability contract: no structural changes (Add/Remove/Destroy/
// Create on any of the involved component pools) inside the foreach body.
// EnTT pools may reallocate on structural ops; a pointer collected at the
// top of iteration would dangle. The legacy class-API `Scene.Query<T>()`
// stays available for code that needs mid-iteration mutation — it works
// by snapshotting entity IDs and re-resolving per row.
//
// Entry point: `scene.QueryRef<Transform2D>()` (extension method on Scene).
// Chain `.Without<T>()`, `.With<T>()`, `.EnabledOnly()`/`.DisabledOnly()`,
// `.Readonly<T>()` (adds a readonly component). v1 supports 1 write + 0 or 1
// readonly types; higher arities are straightforward generalization.

internal struct QueryRefFilters
{
    // Pipe-separated component display names — same format the legacy query
    // path uses. Mutable so the builder ref-struct can append per chain step.
    public string SceneName;
    public string WriteNames;
    public string ReadonlyNames;
    public string MustHaveNames;
    public string WithoutNames;
    public int    EnableFilter;   // 0=all, 1=enabled, 2=disabled

    public void AppendWithout(string? name)
    {
        if (string.IsNullOrEmpty(name)) return;
        WithoutNames = string.IsNullOrEmpty(WithoutNames) ? name : WithoutNames + "|" + name;
    }

    public void AppendMustHave(string? name)
    {
        if (string.IsNullOrEmpty(name)) return;
        MustHaveNames = string.IsNullOrEmpty(MustHaveNames) ? name : MustHaveNames + "|" + name;
    }
}

internal static class QueryRefBuffers
{
    // One thread-local IntPtr[] reused across queries on the same thread.
    // Sized for the largest query seen so far — for a typical scene this
    // stabilises at the worst-case row count × pool count after a few
    // frames and never reallocates. Pointers stored inside are native
    // addresses into EnTT storage; the managed array is just a value
    // holder so no pinning is needed.
    [ThreadStatic]
    private static IntPtr[]? s_Buffer;

    internal static IntPtr[] Rent(int minSize)
    {
        IntPtr[]? buf = s_Buffer;
        if (buf == null || buf.Length < minSize)
        {
            // Grow geometrically so repeated small-then-large queries don't
            // ping-pong reallocations.
            int newSize = Math.Max(minSize, buf?.Length * 2 ?? 64);
            buf = new IntPtr[newSize];
            s_Buffer = buf;
        }
        return buf;
    }

    // Two-call growth: open the view with the rented buffer, if the native
    // side reports more rows than fit, grow once and retry. Returns the
    // populated buffer and the actual row count.
    internal static unsafe (IntPtr[] buffer, int rowCount) Open(
        ref QueryRefFilters filters, int poolCount)
    {
        IntPtr[] buf = Rent(64 * poolCount);
        while (true)
        {
            int rowCap = buf.Length / poolCount;
            int rowCount;
            fixed (IntPtr* p = buf)
            {
                rowCount = InternalCalls.Scene_OpenQueryView(
                    filters.SceneName, filters.WriteNames, filters.ReadonlyNames,
                    filters.MustHaveNames, filters.WithoutNames, filters.EnableFilter,
                    (void**)p, rowCap);
            }
            if (rowCount <= rowCap) return (buf, rowCount);
            // Grow and retry — rare path, only when scene has grown beyond
            // the previous high-water mark since the last query.
            buf = Rent(rowCount * poolCount);
        }
    }
}

// ── Single-component query ───────────────────────────────────────────

public ref struct QueryRefBuilder1<TW1> where TW1 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefBuilder1(string sceneName)
    {
        m_Filters = new QueryRefFilters {
            SceneName     = sceneName,
            WriteNames    = ComponentTypes<TW1>.NativeName,
            ReadonlyNames = "",
            MustHaveNames = "",
            WithoutNames  = "",
            EnableFilter  = 0,
        };
    }

    public QueryRefBuilder1<TW1> Without<T>() where T : unmanaged, IComponent
    { m_Filters.AppendWithout(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder1<TW1> With<T>() where T : unmanaged, IComponent
    { m_Filters.AppendMustHave(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder1<TW1> EnabledOnly()  { m_Filters.EnableFilter = 1; return this; }
    public QueryRefBuilder1<TW1> DisabledOnly() { m_Filters.EnableFilter = 2; return this; }

    public QueryRefBuilder1_RO1<TW1, TRO1> Readonly<TRO1>() where TRO1 : unmanaged, IComponent
        => new QueryRefBuilder1_RO1<TW1, TRO1>(m_Filters);

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_Count) = QueryRefBuffers.Open(ref filters, poolCount: 1);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        public unsafe ref TW1 Current
            => ref Unsafe.AsRef<TW1>((void*)m_Buffer[m_Index]);
    }
}

// ── 1 write + 1 readonly ─────────────────────────────────────────────

public ref struct QueryRefBuilder1_RO1<TW1, TRO1>
    where TW1  : unmanaged, IComponent
    where TRO1 : unmanaged, IComponent
{
    private QueryRefFilters m_Filters;

    internal QueryRefBuilder1_RO1(QueryRefFilters inherited)
    {
        m_Filters = inherited;
        m_Filters.ReadonlyNames = ComponentTypes<TRO1>.NativeName;
    }

    public QueryRefBuilder1_RO1<TW1, TRO1> Without<T>() where T : unmanaged, IComponent
    { m_Filters.AppendWithout(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder1_RO1<TW1, TRO1> With<T>() where T : unmanaged, IComponent
    { m_Filters.AppendMustHave(ComponentTypes<T>.NativeName); return this; }

    public QueryRefBuilder1_RO1<TW1, TRO1> EnabledOnly()  { m_Filters.EnableFilter = 1; return this; }
    public QueryRefBuilder1_RO1<TW1, TRO1> DisabledOnly() { m_Filters.EnableFilter = 2; return this; }

    public Enumerator GetEnumerator() => new Enumerator(m_Filters);

    public ref struct Enumerator
    {
        private readonly IntPtr[] m_Buffer;
        private readonly int      m_Count;
        private int               m_Index;

        internal Enumerator(QueryRefFilters filters)
        {
            (m_Buffer, m_Count) = QueryRefBuffers.Open(ref filters, poolCount: 2);
            m_Index = -1;
        }

        public bool MoveNext() => ++m_Index < m_Count;

        public unsafe Row Current
        {
            get
            {
                int baseIdx = m_Index * 2;
                return new Row(
                    (TW1*)m_Buffer[baseIdx + 0],
                    (TRO1*)m_Buffer[baseIdx + 1]);
            }
        }
    }

    // Two access styles. Pick the one that fits the code site:
    //   - Direct ref access (no `.Value`, no deconstruct ceremony):
    //         row.W.LocalPosition += row.R.Linear * Time.DeltaTime;
    //   - Deconstruction (closer to the legacy class-API shape):
    //         var (tr, vel) = row;
    //         tr.Value.LocalPosition += vel.Value.Linear * Time.DeltaTime;
    //
    // The direct form is shorter and saves the Ref<>/RefRO<> wrapper construction.
    // The deconstruct form reads better for >2 components and lets the user
    // name fields meaningfully.
    public readonly ref struct Row
    {
        private readonly unsafe TW1*  m_W1;
        private readonly unsafe TRO1* m_RO1;

        internal unsafe Row(TW1* w1, TRO1* ro1) { m_W1 = w1; m_RO1 = ro1; }

        public unsafe ref TW1          W => ref Unsafe.AsRef<TW1>(m_W1);
        public unsafe ref readonly TRO1 R => ref Unsafe.AsRef<TRO1>(m_RO1);

        public unsafe void Deconstruct(out Ref<TW1> w1, out RefRO<TRO1> ro1)
        {
            w1  = new Ref<TW1>(ref Unsafe.AsRef<TW1>(m_W1));
            ro1 = new RefRO<TRO1>(in Unsafe.AsRef<TRO1>(m_RO1));
        }
    }
}

// Extension entry point: `scene.QueryRef<T>()`. Lives outside the Scene class
// so the legacy SceneQuery.cs file stays untouched.
public static class SceneQueryRefExtensions
{
    public static QueryRefBuilder1<T> QueryRef<T>(this Scene scene)
        where T : unmanaged, IComponent
        => new QueryRefBuilder1<T>(scene.Name);
}
