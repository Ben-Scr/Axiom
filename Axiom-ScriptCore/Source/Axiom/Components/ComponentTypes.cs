using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using Axiom.Interop;

namespace Axiom.Components;

// Maps an `IComponent` struct type to the native serialized/display name the
// binding layer uses to resolve the component's pool. Static-generic dispatch
// (ComponentTypes<T>.NativeName) so the JIT can hoist the lookup once per call
// site — there's no per-call dictionary cost on the hot ref-API path.
//
// Also performs the one-time layout-size verification in the static
// constructor: the first time C# touches `ComponentTypes<T>` we compare
// sizeof(T) against the C++ `sizeof(ComponentT)` reported by
// Entity_GetComponentSize. A mismatch throws, which propagates up through
// the script host's assembly-load path and makes the user's user.dll fail
// to load — preferable to silent memory corruption.
internal static class ComponentTypes
{
    // Registry filled at module init. Add a line here when migrating each
    // new component to the ref-API. Empty/tag types and managed-only types
    // (UI widgets with event delegates) deliberately stay out — they aren't
    // reachable through GetRef<T>.
    private static readonly Dictionary<Type, string> s_NativeNames = new()
    {
        { typeof(NativeTransform2D),    NativeTransform2D.NativeName },
        { typeof(NativeSpriteRenderer), NativeSpriteRenderer.NativeName },
    };

    internal static string? TryGetNativeName(Type t)
    {
        if (s_NativeNames.TryGetValue(t, out string? n))
            return n;

        return ResolveNativeName(t, 0, out _);
    }

    internal static string? ResolveNativeName(Type t, int managedSize, out int nativeSize)
    {
        var candidates = new List<string>(6);
        if (s_NativeNames.TryGetValue(t, out string? registered))
            candidates.Add(registered);

        NativeComponentAttribute? attr = t.GetCustomAttribute<NativeComponentAttribute>();
        if (!string.IsNullOrWhiteSpace(attr?.Name))
            candidates.Add(attr.Name);

        const BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
        FieldInfo? nativeNameField = t.GetField("NativeName", flags);
        if (nativeNameField?.FieldType == typeof(string)
            && nativeNameField.GetValue(null) is string fieldName
            && !string.IsNullOrWhiteSpace(fieldName))
        {
            candidates.Add(fieldName);
        }

        PropertyInfo? nativeNameProperty = t.GetProperty("NativeName", flags);
        if (nativeNameProperty?.PropertyType == typeof(string)
            && nativeNameProperty.GetValue(null) is string propertyName
            && !string.IsNullOrWhiteSpace(propertyName))
        {
            candidates.Add(propertyName);
        }

        AddNameCandidates(t.Name, candidates);

        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (string candidate in candidates)
        {
            if (string.IsNullOrWhiteSpace(candidate) || !seen.Add(candidate))
                continue;

            nativeSize = InternalCalls.Entity_GetComponentSize(candidate);
            if (nativeSize != 0)
                return candidate;
        }

        nativeSize = 0;
        return null;
    }

    private static void AddNameCandidates(string typeName, List<string> candidates)
    {
        if (string.IsNullOrWhiteSpace(typeName))
            return;

        candidates.Add(typeName);

        string stripped = typeName.StartsWith("Native", StringComparison.Ordinal)
            ? typeName.Substring("Native".Length)
            : typeName;

        if (!string.IsNullOrWhiteSpace(stripped))
            candidates.Add(stripped);

        const string suffix = "Component";
        if (stripped.EndsWith(suffix, StringComparison.Ordinal) && stripped.Length > suffix.Length)
            candidates.Add(stripped.Substring(0, stripped.Length - suffix.Length));
    }

    // Force the static constructor of every registered ComponentTypes<T> so
    // the layout-size guard runs at script-host init instead of lazily on
    // first GetRef<T>. A drift between C++ and C# struct sizes throws here,
    // ScriptHostBridge.Initialize catches it, and the user assembly fails
    // to load — the user sees the error before the first frame, not five
    // minutes into play mode when an unlucky entity touches the broken type.
    internal static void RunAllStaticInitializers()
    {
        Type generic = typeof(ComponentTypes<>);
        foreach (Type t in s_NativeNames.Keys)
        {
            RuntimeHelpers.RunClassConstructor(generic.MakeGenericType(t).TypeHandle);
        }
    }
}

// One per `T : IComponent` — `ComponentTypes<T>.NativeName` is a JIT-time
// constant for each T, and the static constructor runs the size check exactly
// once per type, lazily on first use.
internal static class ComponentTypes<T> where T : unmanaged, IComponent
{
    internal static readonly string NativeName;

    static ComponentTypes()
    {
        int managedSize = Unsafe.SizeOf<T>();
        string? name = ComponentTypes.ResolveNativeName(typeof(T), managedSize, out int nativeSize);
        if (name == null)
        {
            throw new InvalidOperationException(
                $"Component '{typeof(T).Name}' is not registered for the ECS ref-API. " +
                "Register the native component with the engine, or add [NativeComponent(\"Native Name\")] " +
                "when the managed struct name cannot be inferred.");
        }

        // Layout drift guard: the C++ component is the source of truth. Any
        // change there (added/removed/reordered field) MUST be mirrored in
        // the C# struct or this check fires. Catches the failure mode where
        // a header edit lands without the matching C# update.
        if (nativeSize != 0 && nativeSize != managedSize)
        {
            throw new InvalidOperationException(
                $"Layout mismatch for component '{typeof(T).Name}' (native '{name}'): " +
                $"C++ sizeof = {nativeSize}, C# sizeof = {managedSize}. " +
                "Mirror the C++ struct's fields (in order) in the C# struct and rebuild.");
        }

        NativeName = name;
    }
}
