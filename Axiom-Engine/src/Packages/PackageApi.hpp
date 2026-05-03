#pragma once

// Shared API surface for Axiom packages.
//
// Every engine_core / standalone_cpp package compiles into its own SharedLib
// (`Pkg.<Name>.Native`). The runtime PackageHost LoadLibrary's the binary and
// resolves the standard entry points by name. Each package's TUs need to
// dllexport those entry points so GetProcAddress can find them.
//
// Including this header in a package source file gives it:
//   - AXIOM_PACKAGE_API           — visibility macro for exported symbols.
//   - AxiomPackage_OnLoad         — declared entry point (the package defines it).
//   - AxiomPackage_OnUnload       — optional entry point.
//
// AXIOM_PACKAGE_API is always export-side because this header is only included
// from package source. Consumers of a package's exports must use platform
// dlopen / GetProcAddress, not link-time imports.

#if defined(_WIN32)
    #define AXIOM_PACKAGE_API __declspec(dllexport)
#else
    #define AXIOM_PACKAGE_API __attribute__((visibility("default")))
#endif

extern "C" {

    // Called once after the package's DLL is loaded. Return 0 on success;
    // non-zero is logged but the module stays loaded. Optional — packages
    // without it just appear in the loaded list with no init step.
    AXIOM_PACKAGE_API int AxiomPackage_OnLoad();

    // Called once before the package's DLL is unloaded (engine shutdown,
    // currently). Optional.
    AXIOM_PACKAGE_API void AxiomPackage_OnUnload();

}
