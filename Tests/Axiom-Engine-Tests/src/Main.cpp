// Single doctest entry point. Generates main(), parses CLI flags, runs every
// TEST_CASE registered in any TU linked into this binary.
//
// Add new test files as additional .cpp under src/ — they only need to:
//     #include <doctest/doctest.h>
//     #include "Engine/Public/Header.hpp"
//     TEST_CASE("...") { ... }
// No registration boilerplate, no per-file includes here.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
