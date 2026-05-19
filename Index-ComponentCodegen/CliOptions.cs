namespace Index.ComponentCodegen;

internal sealed class CliOptions
{
    public required string InputAssembly { get; init; }
    public required string OutputCpp     { get; init; }
    public string GeneratedNamespace     { get; init; } = "Index::Generated";
    public bool Verbose                  { get; init; }

    public static CliOptions? Parse(string[] argv)
    {
        string? input = null;
        string? output = null;
        string ns = "Index::Generated";
        bool verbose = false;

        for (int i = 0; i < argv.Length; ++i)
        {
            switch (argv[i])
            {
                case "--input":
                case "-i":
                    if (++i >= argv.Length) { PrintUsage("missing value for --input"); return null; }
                    input = argv[i];
                    break;
                case "--output":
                case "-o":
                    if (++i >= argv.Length) { PrintUsage("missing value for --output"); return null; }
                    output = argv[i];
                    break;
                case "--namespace":
                case "-n":
                    if (++i >= argv.Length) { PrintUsage("missing value for --namespace"); return null; }
                    ns = argv[i];
                    break;
                case "--verbose":
                case "-v":
                    verbose = true;
                    break;
                case "--help":
                case "-h":
                    PrintUsage(null);
                    return null;
                default:
                    PrintUsage($"unknown argument: {argv[i]}");
                    return null;
            }
        }

        if (input is null || output is null)
        {
            PrintUsage("--input and --output are required");
            return null;
        }

        return new CliOptions
        {
            InputAssembly      = input,
            OutputCpp          = output,
            GeneratedNamespace = ns,
            Verbose            = verbose,
        };
    }

    private static void PrintUsage(string? error)
    {
        if (error is not null)
            Console.Error.WriteLine($"Index-ComponentCodegen: {error}");
        Console.Error.WriteLine("Usage: Index-ComponentCodegen --input <assembly.dll> --output <CodegenComponents.cpp>");
        Console.Error.WriteLine("       [--namespace <Index::Generated>] [--verbose]");
    }
}
