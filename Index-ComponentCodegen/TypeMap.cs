using System.Globalization;

namespace Index.ComponentCodegen;

// C# → C++ type mapping for codegen field emission.
//
// v1 whitelist: primitives + Index Vector2/3/4 + Color. Anything else
// throws CodegenError, on purpose — silent emission of an unsupported
// field type would only manifest as a `static_assert(sizeof) failed` at
// the C++ compile step, much harder to diagnose than a clear error from
// the tool. v2 will add UUID/AssetRef/EntityRef once the inspector
// helpers for those land.
internal static class TypeMap
{
    public delegate string LiteralFormatter(object? value);

    public static bool TryMap(Type csharpType, out string cppType, out LiteralFormatter literal)
    {
        // Built-in primitives.
        if (csharpType == typeof(bool))    { cppType = "bool";     literal = v => (v is bool b && b) ? "true" : "false"; return true; }
        if (csharpType == typeof(sbyte))   { cppType = "int8_t";   literal = v => $"static_cast<int8_t>({Fmt(v, 0)})"; return true; }
        if (csharpType == typeof(byte))    { cppType = "uint8_t";  literal = v => $"static_cast<uint8_t>({Fmt(v, 0u)})"; return true; }
        if (csharpType == typeof(short))   { cppType = "int16_t";  literal = v => $"static_cast<int16_t>({Fmt(v, 0)})"; return true; }
        if (csharpType == typeof(ushort))  { cppType = "uint16_t"; literal = v => $"static_cast<uint16_t>({Fmt(v, 0u)})"; return true; }
        if (csharpType == typeof(int))     { cppType = "int32_t";  literal = v => Fmt(v, 0); return true; }
        if (csharpType == typeof(uint))    { cppType = "uint32_t"; literal = v => $"{Fmt(v, 0u)}u"; return true; }
        if (csharpType == typeof(long))    { cppType = "int64_t";  literal = v => $"static_cast<int64_t>({Fmt(v, 0L)}LL)"; return true; }
        if (csharpType == typeof(ulong))   { cppType = "uint64_t"; literal = v => $"static_cast<uint64_t>({Fmt(v, 0UL)}ULL)"; return true; }
        if (csharpType == typeof(float))   { cppType = "float";    literal = v => FormatFloat(v); return true; }
        if (csharpType == typeof(double))  { cppType = "double";   literal = v => FormatDouble(v); return true; }

        // Index math types — keyed by FullName so we don't need a hard
        // assembly reference. The C++ side uses glm types, which we
        // construct field-by-field to mirror the C# layout.
        switch (csharpType.FullName)
        {
            case "Index.Vector2":
                cppType = "glm::vec2";
                literal = v => FormatVec2(v);
                return true;
            case "Index.Vector3":
                cppType = "glm::vec3";
                literal = v => FormatVec3(v);
                return true;
            case "Index.Vector4":
                cppType = "glm::vec4";
                literal = v => FormatVec4(v);
                return true;
            case "Index.Color":
                cppType = "Color";
                literal = v => FormatColor(v);
                return true;
        }

        cppType = "";
        literal = _ => "{}";
        return false;
    }

    private static string Fmt<T>(object? v, T fallback)
        => Convert.ToString(v ?? fallback, CultureInfo.InvariantCulture) ?? "";

    private static string FormatFloat(object? v)
    {
        var f = v is float fv ? fv : (v is double dv ? (float)dv : 0f);
        return ToCppFloatLiteral(f);
    }

    // C++ float literals need a decimal point or exponent — `1f` is not a
    // valid token; `1.0f` is. "R" round-trip formatting drops the point for
    // integer-valued floats ("1"), so we add it back.
    private static string ToCppFloatLiteral(float f)
    {
        var s = f.ToString("R", CultureInfo.InvariantCulture);
        if (!s.Contains('.') && !s.Contains('e') && !s.Contains('E')) s += ".0";
        return s + "f";
    }

    private static string FormatDouble(object? v)
    {
        var d = v is double dv ? dv : (v is float fv ? fv : 0.0);
        var s = d.ToString("R", CultureInfo.InvariantCulture);
        // Make sure the literal is a double — `.0` keeps it from being
        // parsed as int when the value is whole.
        if (!s.Contains('.') && !s.Contains('e') && !s.Contains('E')) s += ".0";
        return s;
    }

    // Read X/Y/Z/W (and R/G/B/A for Color) by name via reflection so we
    // don't need a project reference to Index-ScriptCore.
    private static float ReadFloatField(object? obj, string name)
    {
        if (obj is null) return 0f;
        var fi = obj.GetType().GetField(name);
        if (fi is null) return 0f;
        var v = fi.GetValue(obj);
        return v is float f ? f : (v is double d ? (float)d : 0f);
    }

    private static string FormatVec2(object? v)
        => $"glm::vec2({Lit(ReadFloatField(v, "X"))}, {Lit(ReadFloatField(v, "Y"))})";

    private static string FormatVec3(object? v)
        => $"glm::vec3({Lit(ReadFloatField(v, "X"))}, {Lit(ReadFloatField(v, "Y"))}, {Lit(ReadFloatField(v, "Z"))})";

    private static string FormatVec4(object? v)
        => $"glm::vec4({Lit(ReadFloatField(v, "X"))}, {Lit(ReadFloatField(v, "Y"))}, {Lit(ReadFloatField(v, "Z"))}, {Lit(ReadFloatField(v, "W"))})";

    private static string FormatColor(object? v)
        => $"Color{{ {Lit(ReadFloatField(v, "R"))}, {Lit(ReadFloatField(v, "G"))}, {Lit(ReadFloatField(v, "B"))}, {Lit(ReadFloatField(v, "A"))} }}";

    private static string Lit(float f) => ToCppFloatLiteral(f);
}
