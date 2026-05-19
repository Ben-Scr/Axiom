namespace Index.ComponentCodegen;

internal sealed class DiscoveredComponent
{
    public required string CSharpTypeName   { get; init; }
    public required string CSharpFullName   { get; init; }
    public required string NativeName       { get; init; }
    public required string Category         { get; init; }   // "Component" or "Tag"
    public required string Subcategory      { get; init; }
    public required int    ManagedSize      { get; init; }
    public required List<DiscoveredField> Fields { get; init; }
}

internal sealed class DiscoveredField
{
    public required string CSharpName      { get; init; }
    public required string DisplayName     { get; init; }   // resolved after [Display] override
    public required string CppType         { get; init; }
    public required string? CppDefaultExpr { get; init; }   // null → zero-init
    public required bool   HideFromInspector { get; init; }
    public required FieldMetadata? Meta    { get; init; }   // [Range]/[Tooltip] hints
}

internal sealed class FieldMetadata
{
    public double? RangeMin   { get; init; }
    public double? RangeMax   { get; init; }
    public string? Tooltip    { get; init; }
}

internal sealed class CodegenError : Exception
{
    public CodegenError(string message) : base(message) { }
}
