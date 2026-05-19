using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace Index.ComponentCodegen;

internal static class AssemblyScanner
{
    private const string ComponentInterfaceFullName = "Index.Components.IComponent";
    private const string NativeComponentAttrFullName = "Index.Components.NativeComponentAttribute";
    // Reuse existing inspector attributes from Index-ScriptCore — same UX as
    // the EntityScript / GameSystem fields users already author with.
    private const string ShowInEditorAttrFullName    = "Index.ShowInEditorAttribute";
    private const string ClampValueAttrFullName      = "Index.ClampValueAttribute";
    private const string ToolTipAttrFullName         = "Index.ToolTipAttribute";
    private const string HideFromEditorAttrFullName  = "Index.HideFromEditorAttribute";

    public static List<DiscoveredComponent> Scan(string inputAssemblyPath, bool verbose)
    {
        var absolute = Path.GetFullPath(inputAssemblyPath);
        var inputDir = Path.GetDirectoryName(absolute)!;

        // Collectible so the host process can release the file lock after
        // the scan completes — important because the IDE rebuilds Sandbox
        // immediately after and would otherwise fail with a sharing violation.
        var alc = new AssemblyLoadContext("Index-ComponentCodegen-Scan", isCollectible: true);
        alc.Resolving += (ctx, asmName) =>
        {
            // Resolve every transitive reference (Index-ScriptCore.dll first,
            // any user-side NuGet packages copied next to the .dll second)
            // from the same directory as the input. Fall back to the default
            // ALC for framework assemblies (System.*, etc.) — those load
            // through the runtime's own resolution chain when this returns null.
            var candidate = Path.Combine(inputDir, asmName.Name + ".dll");
            return File.Exists(candidate) ? ctx.LoadFromAssemblyPath(candidate) : null;
        };

        Assembly asm;
        try
        {
            asm = alc.LoadFromAssemblyPath(absolute);
        }
        catch (Exception ex)
        {
            throw new CodegenError(
                $"Failed to load input assembly '{absolute}': {ex.Message}");
        }

        var discovered = new List<DiscoveredComponent>();

        Type[] types;
        try
        {
            types = asm.GetTypes();
        }
        catch (ReflectionTypeLoadException ex)
        {
            // Be tolerant: report unloadable types but continue with the
            // ones that did load. A missing reference shouldn't take out
            // the whole scan when only one user type is affected.
            var loaderMessages = string.Join("\n  ", ex.LoaderExceptions
                .Where(e => e is not null)
                .Select(e => e!.Message));
            Console.Error.WriteLine($"Index-ComponentCodegen: some types failed to load:\n  {loaderMessages}");
            types = ex.Types.Where(t => t is not null).Cast<Type>().ToArray();
        }

        foreach (var type in types)
        {
            if (!type.IsValueType || type.IsEnum || type.IsPrimitive) continue;
            if (!ImplementsIComponent(type))                          continue;

            var attrData = type.GetCustomAttributesData()
                .FirstOrDefault(a => a.AttributeType.FullName == NativeComponentAttrFullName);
            if (attrData is null) continue;

            // Generate flag is opt-in (named property). Without it we treat
            // the struct as a mirror of an existing native component — same
            // semantics the attribute has always had — and skip codegen.
            var generate = ReadNamedBool(attrData, "Generate", defaultValue: false);
            if (!generate) continue;

            // Layout-sequential is required for blitting. The C# language
            // doesn't enforce it on `struct` — we do.
            var layout = type.StructLayoutAttribute?.Value ?? LayoutKind.Auto;
            if (layout != LayoutKind.Sequential && layout != LayoutKind.Explicit)
            {
                throw new CodegenError(
                    $"Struct '{type.FullName}' is marked [NativeComponent(Generate=true)] but " +
                    "lacks [StructLayout(LayoutKind.Sequential)] — required so C# field order " +
                    "matches the generated C++ struct layout.");
            }

            var nativeName  = ReadCtorString(attrData, 0) ?? type.Name;
            var category    = ReadNamedString(attrData, "Category",    "Component");
            var subcategory = ReadNamedString(attrData, "Subcategory", "Custom");

            // Build a real instance — Activator.CreateInstance invokes the
            // parameterless constructor (synthesized or user-defined),
            // which RUNS field initializers. `default(T)` would skip them
            // and we'd emit zero defaults for every field.
            object? instance = null;
            try
            {
                instance = Activator.CreateInstance(type);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"Index-ComponentCodegen: warning: could not instantiate '{type.FullName}' " +
                    $"to read field defaults ({ex.Message}). C++ defaults will be zero.");
            }

            if (type.GetConstructor(Type.EmptyTypes) is null)
            {
                Console.Error.WriteLine(
                    $"Index-ComponentCodegen: warning: '{type.FullName}' has no parameterless " +
                    "constructor. Add `public " + type.Name + "() {{ }}` so C# field initializers " +
                    "run and their defaults round-trip into the generated C++ struct.");
            }

            int managedSize;
            try
            {
                managedSize = Marshal.SizeOf(type);
            }
            catch (Exception ex)
            {
                throw new CodegenError(
                    $"Struct '{type.FullName}' is not blittable: {ex.Message}. " +
                    "Only blittable structs (no managed references, no auto layout) can be ECS components.");
            }

            var fields = new List<DiscoveredField>();
            foreach (var field in type.GetFields(BindingFlags.Instance | BindingFlags.Public))
            {
                var hidden = field.GetCustomAttributesData()
                    .Any(a => a.AttributeType.FullName == HideFromEditorAttrFullName);

                // [ShowInEditor("Custom Name")] doubles as the display-name
                // override on components — same ctor signature as the
                // existing managed-script flow, so users don't learn a new
                // attribute for what's a one-line rename.
                var showAttr = field.GetCustomAttributesData()
                    .FirstOrDefault(a => a.AttributeType.FullName == ShowInEditorAttrFullName);
                var displayOverride = showAttr is null ? null : ReadCtorString(showAttr, 0);
                if (string.IsNullOrWhiteSpace(displayOverride)) displayOverride = null;

                var rangeAttr = field.GetCustomAttributesData()
                    .FirstOrDefault(a => a.AttributeType.FullName == ClampValueAttrFullName);
                var tooltipAttr = field.GetCustomAttributesData()
                    .FirstOrDefault(a => a.AttributeType.FullName == ToolTipAttrFullName);

                if (!TypeMap.TryMap(field.FieldType, out var cppType, out var literalFn))
                {
                    throw new CodegenError(
                        $"Field '{type.FullName}.{field.Name}' has type '{field.FieldType.FullName}' " +
                        "which is not in the v1 codegen field-type whitelist. " +
                        "Supported: primitives (bool, all int widths, float, double), Vector2/3/4, Color, UUID. " +
                        "Asset/Entity refs, string, List<T>, nested user structs are planned for v2.");
                }

                string? defaultExpr = null;
                if (instance is not null)
                {
                    var raw = field.GetValue(instance);
                    defaultExpr = literalFn(raw);
                }

                FieldMetadata? meta = null;
                if (rangeAttr is not null || tooltipAttr is not null)
                {
                    meta = new FieldMetadata
                    {
                        RangeMin = rangeAttr is null ? null : ReadCtorDouble(rangeAttr, 0),
                        RangeMax = rangeAttr is null ? null : ReadCtorDouble(rangeAttr, 1),
                        Tooltip  = tooltipAttr is null ? null : ReadCtorString(tooltipAttr, 0),
                    };
                }

                fields.Add(new DiscoveredField
                {
                    CSharpName        = field.Name,
                    DisplayName       = displayOverride ?? HumanizeCamelCase(field.Name),
                    CppType           = cppType,
                    CppDefaultExpr    = defaultExpr,
                    HideFromInspector = hidden,
                    Meta              = meta,
                });
            }

            var resolvedCategory = string.Equals(category, "Tag", StringComparison.OrdinalIgnoreCase)
                ? "Tag" : "Component";

            discovered.Add(new DiscoveredComponent
            {
                CSharpTypeName = type.Name,
                CSharpFullName = type.FullName ?? type.Name,
                NativeName     = nativeName,
                Category       = resolvedCategory,
                Subcategory    = subcategory,
                ManagedSize    = managedSize,
                Fields         = fields,
            });

            if (verbose)
                Console.Out.WriteLine(
                    $"Index-ComponentCodegen: discovered '{type.FullName}' → \"{nativeName}\" " +
                    $"({fields.Count} field(s), {managedSize} bytes)");
        }

        return discovered;
    }

    private static bool ImplementsIComponent(Type t)
    {
        foreach (var i in t.GetInterfaces())
        {
            if (i.FullName == ComponentInterfaceFullName) return true;
        }
        return false;
    }

    private static string? ReadCtorString(CustomAttributeData attr, int index)
    {
        if (attr.ConstructorArguments.Count <= index) return null;
        return attr.ConstructorArguments[index].Value as string;
    }

    private static double ReadCtorDouble(CustomAttributeData attr, int index)
    {
        if (attr.ConstructorArguments.Count <= index) return 0.0;
        var v = attr.ConstructorArguments[index].Value;
        return v switch
        {
            double d => d,
            float f  => f,
            int i    => i,
            long l   => l,
            _        => Convert.ToDouble(v ?? 0)
        };
    }

    private static bool ReadNamedBool(CustomAttributeData attr, string name, bool defaultValue)
    {
        foreach (var na in attr.NamedArguments)
        {
            if (na.MemberName == name && na.TypedValue.Value is bool b) return b;
        }
        return defaultValue;
    }

    private static string ReadNamedString(CustomAttributeData attr, string name, string defaultValue)
    {
        foreach (var na in attr.NamedArguments)
        {
            if (na.MemberName == name && na.TypedValue.Value is string s && !string.IsNullOrWhiteSpace(s))
                return s;
        }
        return defaultValue;
    }

    private static string HumanizeCamelCase(string camel)
    {
        if (string.IsNullOrEmpty(camel)) return camel;
        var sb = new System.Text.StringBuilder(camel.Length + 4);
        sb.Append(char.ToUpperInvariant(camel[0]));
        for (int i = 1; i < camel.Length; ++i)
        {
            char c = camel[i];
            if (char.IsUpper(c) && !char.IsUpper(camel[i - 1]))
                sb.Append(' ');
            sb.Append(c);
        }
        return sb.ToString();
    }
}
