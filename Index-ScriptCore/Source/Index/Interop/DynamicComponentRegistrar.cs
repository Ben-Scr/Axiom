using System;
using System.Reflection;
using System.Runtime.InteropServices;

using Index;
using Index.Components;

namespace Index.Interop;

// Reflects over the user's loaded assembly at startup and registers every
// value type that implements `IComponent` — no attribute required. Matches
// the managed-component flow: declare the type, save the file, the editor
// picks it up.
//
// Lifetime:
//   - ScriptInstanceManager.LoadUserAssembly calls RegisterAll AFTER the
//     assembly has been loaded into the collectible AssemblyLoadContext.
//   - ScriptInstanceManager.UnloadUserAssembly calls UnregisterAll BEFORE
//     the ALC tears down, so captured native callbacks don't outlive their
//     DynamicComponentStorage.
internal static class DynamicComponentRegistrar
{
    public static void RegisterAll(Assembly userAssembly)
    {
        if (userAssembly is null)
        {
            return;
        }

        Type?[] types;
        try
        {
            types = userAssembly.GetTypes();
        }
        catch (ReflectionTypeLoadException ex)
        {
            // Some types failed to load — log and continue with what we got.
            string message = ex.Message;
            for (int i = 0; i < ex.LoaderExceptions.Length; ++i)
            {
                var inner = ex.LoaderExceptions[i];
                if (inner is not null)
                {
                    message += "\n  " + inner.Message;
                }
            }
            Log.Error($"DynamicComponentRegistrar: partial type load — {message}");
            types = ex.Types ?? Array.Empty<Type?>();
        }

        int registered = 0;
        foreach (Type? type in types)
        {
            if (type is null) continue;
            if (!type.IsValueType || type.IsEnum || type.IsPrimitive) continue;
            if (!typeof(IComponent).IsAssignableFrom(type)) continue;

            if (!TryRegisterStruct(type))
            {
                continue;
            }
            ++registered;
        }

        if (registered > 0)
        {
            Log.Info($"DynamicComponentRegistrar: registered {registered} user component(s)");
        }
    }

    public static void UnregisterAll()
    {
        InternalCalls.Component_UnregisterAllDynamic();
    }

    private static bool TryRegisterStruct(Type type)
    {
        // Layout-sequential required so the C++ side can treat the component
        // bytes as a flat blittable struct that mirrors C#'s field layout.
        // C# doesn't auto-enforce sequential on value types — we do.
        var layout = type.StructLayoutAttribute?.Value;
        if (layout is not null && layout != LayoutKind.Sequential && layout != LayoutKind.Explicit)
        {
            Log.Error(
                $"DynamicComponentRegistrar: '{type.FullName}' implements IComponent " +
                "but lacks [StructLayout(LayoutKind.Sequential)] — required so C# field " +
                "order matches the native byte layout. Component skipped.");
            return false;
        }

        int size;
        try
        {
            size = Marshal.SizeOf(type);
        }
        catch (Exception ex)
        {
            Log.Error(
                $"DynamicComponentRegistrar: '{type.FullName}' is not blittable: {ex.Message}. " +
                "Only blittable structs (no managed references, no auto layout) can be ECS components.");
            return false;
        }

        if (size <= 0)
        {
            Log.Error($"DynamicComponentRegistrar: '{type.FullName}' has non-positive size {size}.");
            return false;
        }

        string displayName = type.Name;
        string subcategory = "Native Components (C#)";
        uint   categoryCode = 0u; // 0 = Component (Tag is reserved for engine-internal)

        // Walk fields to compute the strictest required alignment and reject
        // any field type the native side can't safely consume. The C++ side
        // backs dynamic components with std::vector<uint8_t>, so the
        // strictest tolerable alignment is alignof(max_align_t) (typically
        // 16). A struct containing only float/int/bool/Vec2/3/4 needs 4-byte
        // alignment; adding `double` or `long` bumps it to 8.
        if (!TryComputeAlignment(type, out uint alignment))
        {
            return false;
        }

        uint typeIdU32 = InternalCalls.Component_RegisterDynamic(
            displayName,
            displayName,   // serializedName == displayName
            subcategory,
            categoryCode,
            (uint)size,
            alignment);

        if (typeIdU32 == 0)
        {
            Log.Error($"DynamicComponentRegistrar: native side refused registration for '{type.FullName}'.");
            return false;
        }

        return true;
    }

    // Whitelist of primitive field types and their alignment in bytes.
    // Anything not in this table is rejected — the native side has no
    // visibility into the C# field shape, so unsupported types would
    // silently produce wrong-sized or misaligned reads.
    private static bool TryComputeAlignment(Type structType, out uint alignment)
    {
        alignment = 1;
        var fields = structType.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        foreach (var field in fields)
        {
            if (field.IsStatic) continue;

            if (!TryGetFieldAlignment(field.FieldType, out uint fieldAlign))
            {
                Log.Error(
                    $"DynamicComponentRegistrar: '{structType.FullName}.{field.Name}' has field type " +
                    $"'{field.FieldType.FullName}' which is not in the supported ECS-component field " +
                    "whitelist (sbyte/byte/short/ushort/int/uint/long/ulong/float/double/bool, " +
                    "or a recursively-blittable struct of those). Component skipped.");
                return false;
            }
            if (fieldAlign > alignment) alignment = fieldAlign;
        }
        return true;
    }

    private static bool TryGetFieldAlignment(Type fieldType, out uint alignment)
    {
        if (fieldType == typeof(bool) || fieldType == typeof(sbyte) || fieldType == typeof(byte))
        { alignment = 1; return true; }
        if (fieldType == typeof(short) || fieldType == typeof(ushort))
        { alignment = 2; return true; }
        if (fieldType == typeof(int) || fieldType == typeof(uint) || fieldType == typeof(float))
        { alignment = 4; return true; }
        if (fieldType == typeof(long) || fieldType == typeof(ulong) || fieldType == typeof(double))
        { alignment = 8; return true; }

        // Recurse into nested blittable value types (e.g. Vector2, Vector3).
        if (fieldType.IsValueType && !fieldType.IsEnum && !fieldType.IsPrimitive)
        {
            return TryComputeAlignment(fieldType, out alignment);
        }

        // Enums: align to underlying integral type.
        if (fieldType.IsEnum)
        {
            return TryGetFieldAlignment(Enum.GetUnderlyingType(fieldType), out alignment);
        }

        alignment = 0;
        return false;
    }
}
