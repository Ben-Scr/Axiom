using Index.ComponentCodegen;

// Index-ComponentCodegen: parses a user assembly (typically Index-Sandbox.dll),
// finds every struct marked [NativeComponent(..., Generate = true)] that
// implements IComponent, and emits the matching C++ struct + registration
// call into Index-Engine/src/Generated/CodegenComponents.cpp.
//
// Idempotent: the output file's second line carries a SHA-256 hash of the
// inputs. When the tool runs and the freshly-computed hash matches the
// existing file's, the write is skipped — so MSBuild sees no timestamp
// change and the engine isn't relinked for nothing.
//
// CLI: Index-ComponentCodegen --input <Sandbox.dll> --output <CodegenComponents.cpp>
//      [--namespace Index::Generated] [--verbose]

int Run(string[] argv)
{
    var opts = CliOptions.Parse(argv);
    if (opts is null) return 1;

    if (!File.Exists(opts.InputAssembly))
    {
        Console.Error.WriteLine($"Index-ComponentCodegen: input assembly not found: {opts.InputAssembly}");
        return 2;
    }

    var output = Path.GetFullPath(opts.OutputCpp);
    var outputDir = Path.GetDirectoryName(output);
    if (string.IsNullOrEmpty(outputDir))
    {
        Console.Error.WriteLine($"Index-ComponentCodegen: invalid output path: {output}");
        return 2;
    }
    Directory.CreateDirectory(outputDir);

    List<DiscoveredComponent> components;
    try
    {
        components = AssemblyScanner.Scan(opts.InputAssembly, opts.Verbose);
    }
    catch (CodegenError ex)
    {
        Console.Error.WriteLine($"Index-ComponentCodegen: {ex.Message}");
        return 3;
    }

    // Deterministic order — sort by FNV-1a hash of the component name.
    // Stable IDs across rebuilds depend on this (ComponentRegistry assigns
    // typeIdU32 in registration order; persisted ECB recordings reference
    // those IDs by integer value).
    components.Sort((a, b) =>
    {
        var ha = Fnv1A64(a.NativeName);
        var hb = Fnv1A64(b.NativeName);
        return ha.CompareTo(hb);
    });

    var rendered = CppEmitter.Emit(components, opts.GeneratedNamespace);
    var newHash = Sha256Hex(rendered);

    if (FileMatchesHash(output, newHash))
    {
        if (opts.Verbose)
            Console.Out.WriteLine($"Index-ComponentCodegen: {components.Count} components, hash unchanged — skip write");
        return 0;
    }

    // Stamp the hash on line 2 (line 1 stays @generated marker).
    var stamped = rendered.Replace(CppEmitter.HashPlaceholder, newHash);
    File.WriteAllText(output, stamped);

    Console.Out.WriteLine(
        $"Index-ComponentCodegen: wrote {components.Count} component(s) → {output}");
    return 0;
}

return Run(args);

static ulong Fnv1A64(string s)
{
    const ulong offset = 0xcbf29ce484222325UL;
    const ulong prime  = 0x100000001b3UL;
    ulong h = offset;
    foreach (var c in s)
    {
        h ^= (byte)c;
        h *= prime;
    }
    return h;
}

static string Sha256Hex(string s)
{
    var bytes = System.Text.Encoding.UTF8.GetBytes(s);
    var hash  = System.Security.Cryptography.SHA256.HashData(bytes);
    return Convert.ToHexString(hash).ToLowerInvariant();
}

static bool FileMatchesHash(string path, string expectedHash)
{
    if (!File.Exists(path)) return false;
    using var reader = new StreamReader(path);
    _ = reader.ReadLine();           // line 1: @generated marker
    var line2 = reader.ReadLine();   // line 2: // hash: <hex>
    if (line2 is null) return false;
    var marker = "// hash: ";
    var idx = line2.IndexOf(marker, StringComparison.Ordinal);
    if (idx < 0) return false;
    var fileHash = line2[(idx + marker.Length)..].Trim();
    return string.Equals(fileHash, expectedHash, StringComparison.OrdinalIgnoreCase);
}
