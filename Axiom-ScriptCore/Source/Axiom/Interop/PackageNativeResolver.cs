using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Axiom.Interop;

// Routes `[DllImport("Pkg.<Name>.Native")]` calls from package C# layers to the
// matching native sibling DLL.
//
// Why this exists
// ----------------
// Engine packages with both a `csharp` and `engine_core`/`standalone_cpp`
// layer build to two separate output folders:
//
//   bin/<config>/Pkg.<Name>/         — managed assembly (Pkg.<Name>.dll)
//   bin/<config>/Pkg.<Name>.Native/  — unmanaged DLL (Pkg.<Name>.Native.dll)
//
// .NET's default DllImport resolver searches the host's app dir
// (`AppContext.BaseDirectory`) and then platform PATH. Neither contains the
// package's native DLL — they're siblings of the host's exe folder, not
// children. Without an explicit resolver, every P/Invoke call from a package
// throws DllNotFoundException at first invocation.
//
// This resolver registers a callback on Axiom-ScriptCore's load. ScriptCore is
// referenced by every Pkg.<Name> assembly the loader emits, so the
// `[ModuleInitializer]` fires the first time any package code touches a
// ScriptCore type — which in practice is always before any P/Invoke runs.
//
// Resolution order
// ----------------
// For library names that start with "Pkg.", we try (in order):
//   1. <baseDir>/../<libraryName>/<libraryName>.{dll,so,dylib}
//      — the dev layout: native sits next to the host exe folder.
//   2. <baseDir>/Packages/<libraryName>/<libraryName>.{dll,so,dylib}
//      — the future shipped layout: native bundled under Packages/.
//   3. <baseDir>/<libraryName>.{dll,so,dylib}
//      — fallback if the user copied the DLL alongside the host.
//
// On miss we return IntPtr.Zero so .NET's default search still gets a chance.
// We do NOT throw — the package's own DllImport call site will surface the
// underlying DllNotFoundException with the right context if every probe fails.
internal static class PackageNativeResolver
{
    private const string PackagePrefix = "Pkg.";

    // CA2255 warns about [ModuleInitializer] in library assemblies because the
    // timing isn't obvious to library consumers. Here we *want* exactly that —
    // ScriptCore's runtime users (Axiom-Sandbox, Pkg.<Name>.dll plug-ins) all
    // benefit from the resolver being installed before any of their P/Invoke
    // calls execute. The behavior is documented at the type level above.
#pragma warning disable CA2255
    [ModuleInitializer]
#pragma warning restore CA2255
    internal static void Init()
    {
        // Register against the executing assembly (Axiom-ScriptCore). Each
        // Pkg.<Name> assembly that depends on ScriptCore inherits this hook
        // because the runtime walks the import chain when resolving — but to
        // be defensive, we also register on every newly-loaded assembly that
        // looks like an Axiom package. Ensures resolution even if a package
        // is loaded before ScriptCore (shouldn't happen, but cheap).
        try
        {
            NativeLibrary.SetDllImportResolver(typeof(PackageNativeResolver).Assembly, Resolve);
        }
        catch
        {
            // SetDllImportResolver throws if a resolver is already registered
            // for this assembly. Swallow — first-wins semantics suit us.
        }

        AppDomain.CurrentDomain.AssemblyLoad += static (_, args) =>
        {
            string? name = args.LoadedAssembly.GetName().Name;
            if (name == null || !name.StartsWith(PackagePrefix, StringComparison.Ordinal))
                return;
            try
            {
                NativeLibrary.SetDllImportResolver(args.LoadedAssembly, Resolve);
            }
            catch
            {
                // Idempotent: already registered (e.g. by an earlier
                // ModuleInitializer in this assembly). Ignore.
            }
        };
    }

    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (string.IsNullOrEmpty(libraryName))
            return IntPtr.Zero;
        if (!libraryName.StartsWith(PackagePrefix, StringComparison.Ordinal))
            return IntPtr.Zero;

        string baseDir = AppContext.BaseDirectory;
        if (string.IsNullOrEmpty(baseDir))
            return IntPtr.Zero;

        // Strip a single trailing dir-sep so GetParent() resolves predictably.
        baseDir = baseDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

        string? parentDir = Directory.GetParent(baseDir)?.FullName;
        string fileName = libraryName + GetNativeLibraryExtension();

        // Probe order documented in the type comment.
        if (parentDir != null)
        {
            string devCandidate = Path.Combine(parentDir, libraryName, fileName);
            if (TryLoad(devCandidate, out IntPtr handle))
                return handle;
        }

        string shippedCandidate = Path.Combine(baseDir, "Packages", libraryName, fileName);
        if (TryLoad(shippedCandidate, out IntPtr shippedHandle))
            return shippedHandle;

        string flatCandidate = Path.Combine(baseDir, fileName);
        if (TryLoad(flatCandidate, out IntPtr flatHandle))
            return flatHandle;

        return IntPtr.Zero;
    }

    private static bool TryLoad(string path, out IntPtr handle)
    {
        handle = IntPtr.Zero;
        if (!File.Exists(path))
            return false;
        try
        {
            handle = NativeLibrary.Load(path);
            return handle != IntPtr.Zero;
        }
        catch
        {
            return false;
        }
    }

    private static string GetNativeLibraryExtension()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return ".dll";
        if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX)) return ".dylib";
        return ".so";
    }
}
