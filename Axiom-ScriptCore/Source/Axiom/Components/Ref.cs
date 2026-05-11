namespace Axiom.Components;

// Generic wrappers used as the row-field type for multi-component queries.
// They exist because C# tuple deconstruction binds to plain locals, not
// `ref` locals — so a `Row.Deconstruct(out ref T, ...)` shape can't yield
// refs directly. Wrapping the ref inside a `ref struct` lets the local
// "copy" of the wrapper still reach the underlying memory through `.Value`.
//
// Single-component queries (`scene.QueryRef<T>()`) skip Ref<T> entirely and
// yield raw `ref T` via the enumerator's Current — no wrapper indirection.
// These types only appear when you chain `.Readonly<T>()` or have multiple
// write components.
//
// Both types are zero-cost wrappers: the JIT inlines `.Value` to a direct
// memory access, and a write through `tr.Value.X = v` writes into the
// component's pool slot.
public readonly ref struct Ref<T> where T : unmanaged, IComponent
{
    private readonly ref T m_Ref;
    public ref T Value => ref m_Ref;
    internal Ref(ref T r) { m_Ref = ref r; }
}

public readonly ref struct RefRO<T> where T : unmanaged, IComponent
{
    private readonly ref readonly T m_Ref;
    public ref readonly T Value => ref m_Ref;
    internal RefRO(in T r) { m_Ref = ref r; }
}
